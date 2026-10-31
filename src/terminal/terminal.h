/*
 * Liu - terminal emulator
 * VT-compatible terminal state machine.
 * Works for both local PTY sessions and SSH channels.
 */
#ifndef TERMINAL_H
#define TERMINAL_H

#include "core/types.h"
#include <string.h>

/* =========================================================================
 * Cell attributes
 * ========================================================================= */

typedef struct {
    u32  fg;              /* foreground: ANSI index (0-255) or 0x01RRGGBB for truecolor */
    u32  bg;              /* background: same encoding */
    u16  flags;           /* TermAttrFlag bitmask */
    u16  _pad;
} CellAttr;

typedef enum {
    ATTR_BOLD       = 1 << 0,
    ATTR_DIM        = 1 << 1,
    ATTR_ITALIC     = 1 << 2,
    ATTR_UNDERLINE  = 1 << 3,
    ATTR_BLINK      = 1 << 4,
    ATTR_INVERSE    = 1 << 5,
    ATTR_HIDDEN     = 1 << 6,
    ATTR_STRIKETHROUGH = 1 << 7,
    ATTR_WIDE       = 1 << 8,   /* first cell of a wide (CJK) character */
    ATTR_WDUMMY     = 1 << 9,   /* second cell of a wide character */
    ATTR_OVERLINE   = 1 << 10,
    ATTR_HYPERLINK  = 1 << 11,
} TermAttrFlag;

#define FG_DEFAULT 7
#define BG_DEFAULT 0

/* What a BLANK cell can show: its background, and the decorations that are
 * drawn whether or not there is a glyph (inverse paints the foreground colour
 * as a block). Bold, italic, dim, blink and hidden show nothing on a space. */
#define TERM_ATTR_VISIBLE_ON_BLANK \
    (ATTR_UNDERLINE | ATTR_INVERSE | ATTR_STRIKETHROUGH | ATTR_OVERLINE)
#define FG_TRUECOLOR(r,g,b) (0x01000000 | ((u32)(r)<<16) | ((u32)(g)<<8) | (u32)(b))
#define BG_TRUECOLOR(r,g,b) (0x01000000 | ((u32)(r)<<16) | ((u32)(g)<<8) | (u32)(b))
#define IS_TRUECOLOR(c) ((c) & 0x01000000)

/* =========================================================================
 * Cell
 * ========================================================================= */

typedef struct {
    u32      codepoint;
    CellAttr attr;
} Cell;

/* True when nothing about this cell can be SEEN once its glyph is a space: the
 * default background and none of the decorations above. Only such a cell may
 * be dropped from the tail of a stored line (scrollback, reflow) — the text
 * copy has its own, separate, policy about trailing spaces. */
static inline bool term_cell_attr_plain(const Cell *c) {
    return c->attr.bg == BG_DEFAULT && !(c->attr.flags & TERM_ATTR_VISIBLE_ON_BLANK);
}

_Static_assert(sizeof(CellAttr) == 12, "CellAttr layout must remain 12 bytes");
_Static_assert(offsetof(Cell, codepoint) == 0, "Cell.codepoint offset changed");
_Static_assert(offsetof(Cell, attr) == 4, "Cell.attr offset changed");
_Static_assert(sizeof(Cell) == 16, "Cell layout must remain 16 bytes");

/* =========================================================================
 * Terminal
 * ========================================================================= */

typedef enum {
    CURSOR_BLOCK = 0,
    CURSOR_UNDERLINE,
    CURSOR_BAR,
} CursorStyle;

/* A CSI sequence's parameters; more are dropped. A truecolor foreground and
 * background with an underline colour and four attributes is already 19. */
#define TERM_MAX_PARAMS 32

typedef enum {
    MODE_WRAP         = 1 << 0,
    MODE_INSERT       = 1 << 1,
    MODE_APP_CURSOR   = 1 << 2,
    MODE_APP_KEYPAD   = 1 << 3,
    MODE_ALT_SCREEN   = 1 << 4,
    MODE_MOUSE_BTN    = 1 << 5,
    MODE_MOUSE_MOTION = 1 << 6,
    MODE_MOUSE_SGR    = 1 << 7,
    MODE_BRACKETED_PASTE = 1 << 8,
    MODE_FOCUS_EVENT  = 1 << 9,
    MODE_ORIGIN         = 1 << 10,
    MODE_CRLF           = 1 << 11,
    MODE_SYNC_OUTPUT    = 1 << 12,  /* 2026 */
    MODE_GRAPHEME       = 1 << 13,  /* 2027 */
    MODE_IN_BAND_RESIZE = 1 << 14,  /* 2048 */
    MODE_COLOR_SCHEME   = 1 << 15,  /* 2031 */
    MODE_ALT_SCROLL     = 1 << 16,  /* 1007 */
    MODE_MOUSE_ANY      = 1 << 17,  /* 1003 */
    MODE_MOUSE_URXVT    = 1 << 18,  /* 1015 */
    MODE_MOUSE_PIXEL    = 1 << 19,  /* 1016 */
    MODE_MODIFY_OTHER_KEYS = 1 << 20,  /* modifyOtherKeys level */
} TermMode;

/* A program asked for the mouse: clicks (1000), drags (1002) or every move
 * (1003). Each is a tracking mode on its own, so a program that sets just
 * 1003 + 1006 gets clicks, the wheel and motion. */
#define MODE_MOUSE_TRACKING (MODE_MOUSE_BTN | MODE_MOUSE_MOTION | MODE_MOUSE_ANY)

/* DEC 2026 (synchronized update): how long a pane goes on showing its last
 * complete frame after a BSU before the terminal paints what has arrived.
 *
 * A pane with an open BSU draws the picture captured at the BSU
 * (terminal_sync_capture); its siblings and the chrome paint as usual. So the
 * hold costs nobody else anything, and what it buys is that a frame is never
 * shown half written.
 *
 * The limit is generous because the programs that reach it are mostly not dead
 * but late: a TUI descheduled between its BSU and its ESU on a busy machine —
 * its write() goes to sleep once per kilobyte, the pty's queue being 1024
 * bytes — would otherwise have its half-drawn frame put on screen.
 * LIU_SYNC_TIMEOUT_MS overrides it. */
#define SYNC_UPDATE_TIMEOUT_SEC 1.0
/* The one case that still holds the window: the picture could not be captured
 * (no memory), so the pane has nothing complete to show and the frame waits
 * for it. That barrier keeps the short bound. */
#define SYNC_BARRIER_TIMEOUT_SEC 0.15

/* In-band notifications: how much of the program's text is kept. */
#define TERM_NOTIFY_TITLE_CAP 128
#define TERM_NOTIFY_BODY_CAP  384

/* OSC 9;4;<state>;<value> */
enum {
    TERM_PROGRESS_NONE = 0,       /* 0: remove */
    TERM_PROGRESS_SET,            /* 1: value is 0-100 */
    TERM_PROGRESS_ERROR,          /* 2: failed (value optional) */
    TERM_PROGRESS_INDETERMINATE,  /* 3: busy, no figure */
    TERM_PROGRESS_PAUSED,         /* 4: waiting (value optional) */
};

/* Semantic zone type for shell integration prompts */
typedef enum {
    ZONE_NONE = 0,
    ZONE_PROMPT,
    ZONE_INPUT,
    ZONE_OUTPUT,
} SemanticZone;

/* Forward decl so TermLine can carry a cold pointer below. */
struct TermLineCompressed_s;

/* Scrollback ring entry. A single TermLine slot is either hot (cells !=
 * NULL, cold == NULL) or cold (cells == NULL, cold != NULL), never both at
 * once. The cold path stores codepoints + RLE attribute runs (~4× smaller
 * for typical AI / build-log content); the hot path is the plain Cell array
 * used by the primary screen and by transient unfold buffers during resize.
 *
 * sb_push compresses incoming rows directly into a cold TermLineCompressed.
 * Readers that need a Cell pointer (renderer scrollback rendering, sleep
 * snapshot serializer) materialize the cold form into a per-Terminal scratch
 * buffer; readers that only need codepoints (search regex match, selection
 * text) call termline_codepoints and skip materialization entirely. */
typedef struct {
    Cell                          *cells;    /* hot path: row cells (NULL when cold) */
    i32                            len;
    i32                            cap;      /* allocated hot Cell capacity (0 when cold) */
    bool                           wrapped;  /* true if line continues from previous line */
    struct TermLineCompressed_s   *cold;     /* cold path: heap RLE; NULL when hot */
} TermLine;

/* =========================================================================
 * Compressed scrollback line.
 *
 * Agent and build output is dominated by long uniform-style runs: agent
 * prose, build logs, file dumps. A 200-cell line typically uses 1-3
 * distinct CellAttr values, so storing attributes per-cell wastes ~3 KB/line.
 *
 * `TermLineCompressed` keeps codepoints flat (one u32 per visual cell) and
 * replaces the per-cell CellAttr with run-length-encoded `AttrRun`s. A side
 * bitmap remembers which cells are wide-spacers so the materialize path can
 * reattach `ATTR_WDUMMY` without a flag in every cell.
 *
 * Memory profile (200-cell row examples):
 *   Hot TermLine             : 200 * 16          = 3200 B
 *   Cold uniform AI prose    : 200*4 + 25 + 16   ≈  841 B   (1 run)
 *   Cold syntax-highlighted  : 200*4 + 25 + 12*16≈ 1017 B   (12 runs)
 *
 * The helpers in buffer.c are termline_compress / termline_materialize /
 * termline_codepoints / termline_compressed_destroy. */
typedef struct {
    CellAttr attr;        /* attribute applied to `length` consecutive cells */
    u16      length;      /* run length in cells */
    u16      _pad;        /* reserved; keep AttrRun a clean 16 B for clone copies */
} AttrRun;

typedef struct TermLineCompressed_s {
    /* Cell content — one codepoint per visual cell, contiguous. */
    u32     *codepoints;
    i32      len;
    i32      cap;

    /* RLE attribute spans. Sum of run lengths == len. */
    AttrRun *runs;
    u16      run_count;
    u16      run_cap;
    /* The block's size class (buffer.c, sb_cache): the block was allocated at
     * that class's size and may be kept for the next line of it. 0 for one
     * that may not (an empty line, a line too long to keep). Sits in the
     * padding before wide_bits. */
    u8       slab_class;

    /* Wide-char trailer bitmap: bit i set => codepoints[i] is the spacer
     * (right half) of a wide character. Size = ceil(len/64) u64 words —
     * followed, when has_leads, by as many words again whose bit i says cell i
     * is the LEFT half (ATTR_WIDE). Neither bit lives in the runs: a lead
     * there would open two runs per wide character. */
    u64     *wide_bits;

    bool     wrapped;     /* matches TermLine.wrapped */
    u8       ascii_search_state; /* immutable row: 0 unknown, 1 ASCII, 2 general; not serialized */
    bool     content_valid;
    bool     has_leads;   /* the lead words follow wide_bits; only rows with a wide character pay for them */
    i32      content_len;       /* immutable trim length; occupies tail padding on 64-bit */
} TermLineCompressed;

/* Compression / materialization API.
 *
 * `termline_compress` builds a fully-owned compressed line from `cells`. On
 * alloc failure leaves `out` zeroed and returns false. On success the caller
 * owns the heap allocations; release with `termline_compressed_destroy`.
 *
 * `termline_materialize` expands a compressed line back into a Cell array
 * provided by the caller (renderer scratch buffer, bidi/search staging, etc).
 * Writes at most `out_cap` cells and returns the number written.
 *
 * `termline_codepoints` returns the raw codepoint array — useful for callers
 * that only need text (selection text, search regex matching) and don't
 * touch attributes; avoids the materialize allocation. */
bool   termline_compress(const Cell *cells, i32 len, bool wrapped,
                          TermLineCompressed **out_ptr);
i32    termline_materialize(const TermLineCompressed *line,
                          Cell *out, i32 out_cap);
/* Length after trimming ordinary trailing spaces, as on materialized cells.
 * Wide/dummy cells stay significant; no temporary Cell array is needed. */
i32    termline_content_length(const TermLineCompressed *line);
const u32 *termline_codepoints(const TermLineCompressed *line, i32 *out_len);
void   termline_compressed_destroy(TermLineCompressed *line);
usize  termline_compressed_bytes(const TermLineCompressed *line);

/* Allocate a single contiguous compressed line sized for `len` cells and
 * `run_count` runs — the exact layout termline_compressed_destroy() / _bytes()
 * assume, so a plain free(line) releases everything. Arrays are zeroed; the
 * caller fills codepoints / wide_bits / runs. Returns NULL on OOM. This is the
 * single source of truth for the block layout (used by both the compressor and
 * the sleep-snapshot reader). */
TermLineCompressed *termline_alloc_block(i32 len, u16 run_count, bool wrapped);
/* The same with the lead words (`has_leads`) when `leads`. */
TermLineCompressed *termline_alloc_block_leads(i32 len, u16 run_count, bool wrapped, bool leads);

#ifdef LIU_SCROLLBACK_ALLOC_PROBE
#include "core/percentile.h"
typedef struct {
    u64 alloc_count, free_count;
    u64 requested_bytes, usable_bytes;
    LiuHist malloc_ns, free_ns;
    u64 size_class[24];
} TermlineAllocProbeStats;
void termline_alloc_probe_reset(void);
void termline_alloc_probe_get(TermlineAllocProbeStats *out);
#endif

/* =========================================================================
 * Selection (per-terminal instance)
 * ========================================================================= */

typedef struct {
    /* start/end are VIEWPORT rows — the row the highlight is drawn on and the
     * row selection_get_text reads through terminal_visible_row. They are kept
     * current by selection_sync(), which is what makes the highlight stay on
     * the TEXT rather than on the screen position. */
    i32  start_col, start_row;
    i32  end_col, end_row;
    bool active;
    bool is_word;
    bool is_line;
    bool is_rect;
    /* --- a character drag is between EDGES, not cells ---------------------
     * A drag's ends are placed on the cell edge NEAREST the pointer, like a
     * text editor's caret: a press on the right half of the space before a
     * word starts the selection AT the word, and a character the pointer
     * merely touches at the other end is not taken. So neither the highlight
     * nor the copy picks up a stray space or letter at either end.
     *
     * selection_start_edge/_update_edge keep that semantics while start/end
     * stay INCLUSIVE CELLS for every reader: the press edge is the pivot cell
     * plus `pivot_right`, the pointer's edge is its cell plus its own half,
     * and the cells strictly between the two edges are written into
     * start/end. Equal edges select nothing, which is `empty` — the gesture
     * is live, the range is zero width, and nothing reads as selected or
     * copies. `edge_mode` is off for word/line units and for a plain
     * selection_start. */
    bool edge_mode;
    bool pivot_right;
    bool empty;

    /* --- what keeps the selection tied to its content ---------------------
     * A selection is anchored to its text, not to viewport rows: when the
     * program prints more lines or the view scrolls into scrollback, the
     * highlight moves with the text it covers instead of staying on rows that
     * now hold something else.
     *
     * `anchor_abs` is the absolute scrollback line of start_row, the same
     * addressing TermImage.abs_line uses: sb_abs_base + sb_count + (row -
     * scroll_offset). selection_sync re-derives the rows from it, so output
     * and viewport scrolling both move the highlight with its text, and a line
     * evicted from scrollback drops the selection.
     *
     * The alt screen has no line identity at all — a full-screen program
     * repaints at absolute rows and nothing is ever pushed to scrollback — so
     * an alt-screen selection anchors to nothing (-1) and is dropped as soon
     * as the content under it CHANGES. `epoch` is the test: content_writes
     * counts cells whose codepoint actually changed, so a spinner redrawing
     * the same glyph costs the selection nothing while real new output ends
     * it. `on_alt` catches the screen being switched underneath it. */
    i64  anchor_abs;
    u64  epoch;
    bool on_alt;
    /* Alt screen only: a hash of the codepoints the selection COVERS, taken
     * whenever the selection is (re)made. `epoch` alone only says that some
     * cell somewhere changed, and on an agent CLI's screen some cell changes
     * several times a second (a spinner, an elapsed-seconds counter),
     * mid-drag included. When epoch moves, the covered cells are re-hashed:
     * same picture, same selection. */
    u64  content_hash;

    /* --- what a word/line drag extends FROM -------------------------------
     * A double- or triple-click does not just place a caret, it selects a
     * UNIT, and dragging afterwards has to extend by that same unit — drag
     * right and the selection grows a word at a time, drag back past where
     * the gesture started and the unit under the pointer becomes the new
     * head while the original unit stays the tail. That needs the original
     * unit kept around: start/end are rewritten on every mouse move, so by
     * the second move the gesture's own origin is gone.
     *
     * Viewport rows, like start/end — selection_sync shifts all three by the
     * same delta, so the pivot follows its text exactly as the rest does. */
    i32  pivot_start_col, pivot_start_row;
    i32  pivot_end_col, pivot_end_row;
} TermSelection;

/* =========================================================================
 * Search (per-terminal instance)
 * ========================================================================= */

typedef struct {
    i32  col;
    i32  row;
    i32  length;
} TermSearchMatch;

/* Why a search has the matches it has. A pattern that does not compile
 * leaves match_count at 0 exactly like a query nothing matches, and the Find
 * bar has to tell the two apart ("Invalid regex" vs "0"). */
typedef enum {
    TERM_SEARCH_OK = 0,
    TERM_SEARCH_BAD_REGEX,     /* use_regex and regcomp() refused the query */
} TermSearchStatus;

typedef struct TermSearchWork TermSearchWork;

typedef struct {
    char             query[256];
    bool             active;
    bool             case_sensitive;
    bool             use_regex;
    TermSearchStatus status;
    TermSearchMatch *matches;
    i32              match_count;
    i32              match_cap;
    i32              current;
    /* Budgeted scans keep rows relative to this absolute origin. Resolve
     * through terminal_search_match_row() as new output moves the live grid. */
    i64              row_origin;
    bool             anchored;
    TermSearchWork  *work;
} TermSearch;

/* Sparse underline attributes — the style and colour of a decorated cell,
 * beside the 16-byte Cell rather than in it.
 *
 * The cell's own ATTR_UNDERLINE says THAT it is underlined; this says how. So
 * an underlined cell with no entry is a plain single underline in its own
 * foreground — never "draw nothing" — which is what keeps an underline on
 * screen across the paths that drop the map (resize, alt-screen switch).
 *
 * The key is a VIRTUAL row: (row + Terminal.ul_row_base) * cols + col. Side
 * data has to travel with its cell, and a screen-row key does not: a line
 * feed at the bottom moves the cells up and would leave the entries behind. A
 * full-screen scroll just advances the base (the entries follow for free);
 * anything else re-keys them (terminal_underline_scroll for rows,
 * terminal_underline_shift_cols for the cells of one row).
 *
 * Two moves cannot be described as a re-key, and each has its own carrier:
 * the alt screen PARKS the primary's map beside its grid (ul_alt_*), and a
 * resize — whose reflow rewraps every line — stamps each entry into the dead
 * CellAttr._pad of its own cell for the length of terminal_resize, so the
 * reflow carries it wherever the cell goes and the map is rebuilt from the
 * new grid. Outside that one call _pad is zero everywhere. */
typedef struct {
    u32  key;              /* (row + ul_row_base) * cols + col (0xFFFFFFFF = empty) */
    u32  underline_color;  /* 0 = use fg */
    u8   underline_style;  /* 0=none, 1=single, 2=double, 3=curly, 4=dotted, 5=dashed */
    u8   _pad[3];
} UnderlineEntry;

#define UL_MAP_CAP 256  /* initial capacity, enough for most usage */

#define MAX_TERM_IMAGES 32  /* max inline images per terminal */

/* Per-terminal byte budget for inline graphics (sixel and other formats). With
 * MAX_TERM_IMAGES = 32 and a 4096*4096*4B worst-case payload per slot a
 * single hostile remote could pin ~2 GiB; cap the *aggregate* bytes instead
 * and evict the least-recently-used slot until the budget fits. */
#define TERM_IMAGES_BUDGET_BYTES ((usize)(128u * 1024u * 1024u))

/* Inline image record (Sixel, Liu graphics). The Terminal struct
 * holds a heap-allocated array of these — NULL until the first image is
 * placed, so terminals that never display inline graphics (the vast
 * majority) pay zero for the slot pool. */
typedef struct TermImage {
    u8  *pixels;         /* RGBA pixel data */
    i32  width, height;  /* image dimensions in pixels */
    i32  col, row;       /* terminal cell position (row = fallback/alt-screen anchor) */
    /* Absolute scrollback line the image is pinned to (sb_abs_base + sb_count +
     * row at creation). Drives scroll-aware placement: on-screen row =
     * (abs_line - sb_abs_base - sb_count) + scroll_offset. -1 = pin to the
     * fixed grid `row` instead (alt-screen images, which never scroll back). */
    i64  abs_line;
    i32  cols, rows;     /* display size in cells (0 = auto from pixel size) */
    u32  image_id;       /* Liu image ID (0 = unassigned) */
    u32  placement_id;   /* Liu placement ID */
    u64  serial;         /* changes whenever the slot receives new pixels */
    i32  z_index;        /* z-index for layering */
    f64  last_used_sec;  /* platform_time_sec() at last add/lookup — drives LRU eviction */
    bool valid;
} TermImage;

typedef struct Terminal Terminal;

struct Terminal {
    /* Screen buffer (primary) */
    Cell    *cells;           /* rows * cols grid */
    i32      cols;
    i32      rows;

    /* Scroll-without-copying window.
     *
     * A full-screen scroll would otherwise memmove the whole grid up one row
     * for every line of output. The grid is instead allocated with `grid_slack`
     * spare rows at the end and `cells` ADVANCES into them; the copy happens
     * once every grid_slack scrolls instead of once per line. Every reader
     * still indexes cells[y * cols + x], so nothing else changes.
     *
     * grid_base owns the allocation whenever grid_slack > 0, so grid_slide > 0
     * implies cells points into grid_base's block. Anything that frees or
     * replaces `cells` must call terminal_grid_unslide() first.
     *
     * The triple describes whichever grid is LIVE. The alt screen scrolls just
     * as hard as the primary — a pager, or anything running in a pane, pushes a
     * build log's worth of lines through it — so it slides too, and the parked
     * grid keeps its own copy of the triple below. The two are exchanged
     * wholesale by terminal_grid_swap_alt(). */
    Cell    *grid_base;       /* allocation base when grid_slack > 0 */
    i32      grid_slide;      /* rows `cells` has advanced from grid_base */
    i32      grid_slack;      /* spare rows in the allocation past `rows` */

    /* Alternate screen buffer. alt_cells is a WINDOW, exactly like `cells`:
     * whichever grid is parked here keeps the slide state it had, so freeing it
     * means freeing alt_base (terminal_grid_free_alt), never alt_cells. */
    Cell    *alt_cells;
    Cell    *alt_base;        /* allocation base when alt_slack > 0 */
    i32      alt_slide;       /* rows alt_cells has advanced from alt_base */
    i32      alt_slack;       /* spare rows in the parked allocation */
    i32      alt_cursor_x, alt_cursor_y;

    /* Cursor */
    i32      cursor_x;
    i32      cursor_y;
    CellAttr cursor_attr;     /* current attribute for new characters */
    CursorStyle cursor_style;
    bool     cursor_visible;

    /* Transient underline state (cursor template, not stored in CellAttr) */
    u32      cursor_ul_color;
    u8       cursor_ul_style;

    /* Scrollback — ring buffer for O(1) eviction */
    TermLine *sb_ring;         /* ring buffer of scrollback lines */
    i32       sb_capacity;     /* ring buffer capacity */
    i32       sb_count;        /* number of valid lines */
    i32       sb_head;         /* index of oldest line */
    usize     sb_byte_usage;   /* retained bytes in scrollback cell payloads */
    usize     sb_byte_budget;  /* hard cap for scrollback payload bytes */
    /* The blocks of lines that left the front of a full history, kept for the
     * next lines of their size (buffer.c); NULL until the history first fills.
     * Counted in terminal_scrollback_bytes, freed with the scrollback. */
    struct TermLineCache *sb_cache;
    i32       scroll_offset;   /* 0 = bottom, positive = scrolled up */
    i32       scrollback_limit;
    /* Monotonic count of lines ever evicted from the FRONT of scrollback.
     * The absolute index of the line currently at scrollback position i is
     * (sb_abs_base + i); live grid row g sits at (sb_abs_base + sb_count + g).
     * Inline images anchor to an absolute line via this so they scroll with
     * their text and drop out once the line is evicted. */
    i64       sb_abs_base;

    /* Per-terminal scratch buffer used by the renderer/snapshot/bidi paths to
     * materialize a cold scrollback row into a Cell array on demand. Sized to
     * the current cell column count (lazy-grown by terminal_visible_row). The
     * buffer's contents are valid only until the next call that materializes;
     * callers must copy out before issuing another materialize. */
    Cell     *sb_scratch;
    i32       sb_scratch_cap;

    /* Scroll region */
    i32      scroll_top;
    i32      scroll_bottom;

    /* Modes */
    u32      mode;

    /* Tab stops */
    bool    *tab_stops;

    /* VT parser state */
    u8       parse_state;
    u8       esc_buf[256];
    i32      esc_len;

    /* Large DCS buffer for Sixel etc. (lazily allocated) */
    u8      *dcs_buf;
    i32      dcs_len;
    i32      dcs_cap;
    i32      params[TERM_MAX_PARAMS];
    i32      param_count;
    bool     param_has_value;
    u32      param_sub;          /* bit i: parameter i followed a ':' (a sub-parameter of the one before) */
    bool     param_overflow;     /* more parameters than fit: the rest of the sequence's digits are dropped */

    /* Saved cursor (DECSC/DECRC): the slot of the screen that is SHOWING.
     * The other screen's slot is `parked` (below) and the two change places
     * with the grids — a full-screen program's ESC 7 must not overwrite what
     * `?1049h` saved for the shell. */
    i32      saved_x, saved_y;
    CellAttr saved_attr;
    u32      saved_ul_color;
    u8       saved_ul_style;
    bool     saved_origin;       /* DECOM as DECSC found it */
    u8       saved_charset_g[2];
    u8       saved_charset_gl;

    /* DECSCUSR: a program asked for a cursor shape (cursor_style, below) and
     * a blink state (1 blinking, 2 steady, 0 not said). While `cursor_style_set`
     * is false the user's setting draws the cursor. */
    bool     cursor_style_set;
    u8       cursor_blink_set;

    /* The pane's cell in pixels, for the size reports (CSI 14 t / 16 t). */
    i32      cell_px_w, cell_px_h;

    /* Character sets: G0 and G1 each hold ASCII (0) or DEC Special Graphics
     * (1, `ESC ( 0`); SO / SI choose which of them printable bytes go through.
     * It is how terminfo's smacs / acsc draw boxes. */
    u8       charset_g[2];
    u8       charset_gl;

    /* What the screen that is NOT showing keeps for itself: its saved cursor
     * and its keyboard-protocol flags (the CSI u keyboard protocol keeps one
     * flag stack for the main screen and one for the alternate). See
     * screen_state_swap(). */
    struct {
        i32      saved_x, saved_y;
        CellAttr saved_attr;
        u32      saved_ul_color;
        u8       saved_ul_style;
        bool     saved_origin;
        u8       saved_charset_g[2];
        u8       saved_charset_gl;
        u32      kbd_flags;
        u32      kbd_stack[8];
        i32      kbd_stack_len;
    } parked;

    /* Dirty tracking — renderer checks these.
     * multi-word bitmap so > 64 row grids aren't silently truncated.
     * Sized to ceil(rows / 64) words; reallocated by terminal_resize(). */
    bool     dirty;          /* any change at all (frame-level) */
    u64     *dirty_rows;     /* bitmask: bit (r%64) in word (r/64) */
    i32      dirty_words;    /* allocated length of dirty_rows */

    /* Monotonic count of cells whose glyph actually CHANGED.
     *
     * Not a byte count and not a write count: a TUI agent repaints its whole
     * frame on every token, so the bytes arriving on the PTY are dominated by
     * redraw traffic that carries no new content — the same characters written
     * over themselves. Comparing before storing leaves only genuinely new
     * glyphs, which is what actually tracks generation rate.
     *
     * Read as a delta by the UI (see Tab.content_seen) to drive the tab's
     * agent-activity bar; never reset, so a wrap after 2^64 changes is the
     * only discontinuity and the delta absorbs it. */
    u64      content_writes;

    /* The app's: the restore stream last written for this pane (ui.c,
     * session_pane_screen) — its file's name, and content_writes and the
     * history's end when it was written, so a pane that has not changed is
     * not written again. `replay_owned`: this process created the file, so
     * it is rewritten under the same name (a restored pane's file came from
     * an earlier run and is replaced by one of its own). `replay_at`: the
     * loop clock of the last write, which paces the refresh between saves.
     * `replay_alt`, `replay_cols`, `replay_rows`: whether the alternate screen
     * was up, and the grid's size, at that write. */
    char     replay_name[48];
    u64      replay_writes;
    i64      replay_history_end;
    bool     replay_owned;
    bool     replay_alt;
    i32      replay_cols, replay_rows;
    f64      replay_at;

    /* The app's: until when this pane counts as FLOODING — its last drain ran
     * out of budget with bytes still queued (ui.c, poll_drain_pane). Rows are
     * built without ligatures meanwhile: hundreds of lines a frame scroll past
     * unread, and shaping them would take most of a frame (term_pane_draw). */
    f64      lig_flood_until;

    /* Selection state (per-terminal) */
    TermSelection selection;

    /* Search state (per-terminal) */
    TermSearch search;
    u64 search_revision;          /* input, including erases with no glyph writes */
    u64 search_layout_generation; /* reflow/reset invalidates cached row identity */

    /* Title tracking */
    char     title[256];       /* OSC 0/2: shell-set title */
    char    *cwd;              /* OSC 7: current working directory, NULL until set */
    u64      cwd_seq;          /* bumped on every OSC 7 — lets pollers react
                                  the same frame instead of on a rate-limit tick */
    char    *icon_name;        /* OSC 1: icon name, NULL until set */
    bool     title_changed;

    /* Title stack (XTPUSHTITLE/XTPOPTITLE) — lazy allocated */
    char    (*title_stack)[256];  /* pointer to array of char[256], NULL until needed */
    i32      title_stack_len;

    /* Synchronized output (mode 2026) */
    bool     sync_pending;
    f64      sync_start_time;
    struct TermSyncSnapshot *sync_snapshot;
    /* Render-only view: cells contain already materialized visible rows. */
    bool     sync_view;
    i32     *sync_view_lengths;
    /* A sync view may be SPARSE: `cells` then holds only the rows that were
     * captured, packed, and this maps a visible row to its packed index
     * (-1 = not captured; terminal_visible_row answers NULL). NULL means the
     * identity — every row present, as terminal_sync_capture lays it out.
     * The render-prepare worker's job view uses it so a 6-row update to a
     * 200x120 pane captures 6 rows of cells, not 120. */
    const i32 *sync_view_slots;

    /* Hyperlink state (OSC 8) — lazy allocated */
    char    *hyperlink_uri;      /* NULL until first hyperlink */
    bool     in_hyperlink;

    /* Semantic prompt regions (OSC 133) */
    i32      prompt_start_row;
    i32      input_start_row;
    i32      input_start_col;    /* column the prompt ends / input begins on */
    i32      output_start_row;
    i32      last_exit_code;
    bool     last_cmd_failed;

    /* Command timing (OSC 133 C/D markers) */
    f64      cmd_start_time;     /* timestamp when 'C' marker (command start) was received */
    f64      cmd_end_time;       /* timestamp when 'D' marker (command end) was received */
    f64      last_cmd_duration;  /* duration of last completed command in seconds */
    /* Buffered command text captured between OSC 133 B/D markers. Lazy-allocated
     * on first capture so terminals that never see OSC 133 don't pay the 4 KB. */
    char    *pending_command;    /* heap, NULL until first OSC 133 B */
    i32      pending_command_cap;
    bool     history_integrated; /* OSC 133 owns recording once a B that carries this pane's key is seen */
    char     shell_mark_key[33]; /* the key this pane's shell puts on its OSC 133 marks ("" = none):
                                  * a mark without it still shades the prompt, and is not believed
                                  * for the command history (vt_parser.c, osc133_mark_is_ours) */
    char    *command_cwd;        /* cwd at prompt start, before command execution */

    /* Progress the program reports about its own work (OSC 9 ; 4, which the
     * agent CLIs emit). Cleared by the program, by a new prompt (OSC 133;A:
     * the command that reported it is over) and by a full reset. */
    u8       progress_state;     /* TERM_PROGRESS_* */
    u8       progress_value;     /* 0-100 */
    /* OSC 99 sends a notification in pieces; these are the pieces so far. */
    char     note99_id[32];
    char     note99_title[TERM_NOTIFY_TITLE_CAP];
    char     note99_body[TERM_NOTIFY_BODY_CAP];
    bool     note99_title_full, note99_body_full;   /* a piece did not fit: later pieces are not appended after the cut */

    /* Liu keyboard protocol */
    u32      Liu_kbd_flags;
    u32      Liu_kbd_stack[8];
    i32      Liu_kbd_stack_len;

    /* modifyOtherKeys */
    u8       modify_other_keys;  /* 0=off, 1=level1, 2=level2 */

    /* Sparse underline map */
    UnderlineEntry *ul_map;
    i32             ul_map_cap;
    i32             ul_map_count;
    u32             ul_row_base;   /* rows scrolled off the top since the last re-key */
    /* The OTHER screen's map, parked with its grid by terminal_grid_swap_alt:
     * the primary's while a full-screen program has the screen. */
    UnderlineEntry *ul_alt_map;
    i32             ul_alt_cap;
    i32             ul_alt_count;
    u32             ul_alt_base;

    /* BiDi (bidirectional text) */
    bool     bidi_enabled;       /* apply implicit BiDi reordering */

    /* Last printed character (for REP CSI b) */
    u32      last_printed_char;

    /* Where that character landed, so a joiner arriving next (ZWJ, VS16, skin
     * tone) can fold itself into the cell instead of being dropped. -1 when
     * the run was broken by an escape sequence, a control character or a
     * cursor move — a cluster may only extend the cell the parser just wrote.
     * See put_char / cluster_extend in vt_parser.c. */
    i32      cluster_x;
    i32      cluster_y;

    /* Leading bytes of a UTF-8 sequence that a read() boundary cut in half.
     * A PTY hands over whatever bytes are ready, so a 4-byte emoji routinely
     * straddles two terminal_feed calls; without carrying the head over, the
     * whole codepoint is lost (and with it, half a flag or a ZWJ, which splits
     * the grapheme). Never more than 3 bytes pending. */
    u8       utf8_pending[4];
    u8       utf8_pending_len;

    /* Inline images — heap [MAX_TERM_IMAGES] when non-NULL, else no slots
     * have been allocated yet. terminal_add_image lazy-creates the array;
     * terminal_destroy / terminal_clear_images free the slots and pixels. */
    TermImage *images;
    i32        image_count;
    usize      images_byte_usage;  /* sum of pixel-buffer bytes across valid slots */
    u64        image_epoch;        /* invalidates queued image completions on reset */
    void      *image_async;        /* private bounded decode worker/queues */
    /* A graphics transmission arriving in pieces (m=1 ... m=0): the base64
     * collected so far and the first piece's keys. At most one, bounded like a
     * single command; private to vt_parser.c, NULL when none is in flight. */
    void      *gfx_transfer;

    /* STATE_STR_SINK's one bit of memory: was the previous byte an ESC, i.e.
     * is this byte the `\` half of an ST. Its own field rather than a reuse of
     * esc_len, because a discarded payload and a collected one must not share
     * a counter. */
    bool     str_sink_esc;

    /* APC (Liu graphics) dynamic buffer — lazily allocated like dcs_buf */
    u8      *apc_buf;
    i32      apc_len;
    i32      apc_cap;

    /* Line wrap tracking */
    bool    *line_wrapped;       /* per-row wrap flag */

    /* Semantic zones (shell integration: prompt/input/output markers) */
    SemanticZone *row_zones;     /* per-row zone type */
    SemanticZone  current_zone;  /* current zone being written to */

    /* Store the alternate screen's wrap flags and semantic zones with its grid
     * during terminal_grid_swap_alt. The active screen's metadata stays paired
     * with its cells. Allocate these arrays on the first alternate-screen swap
     * and release them in terminal_grid_free_alt. */
    bool         *alt_line_wrapped;
    SemanticZone *alt_row_zones;

    /* Per-terminal ANSI palette (OSC 4). Lazy-allocated so the 1 KB table only
     * lives for terminals that actually receive OSC 4.
     *
     * It holds OVERRIDES, not a full palette, and that distinction is the
     * point: an entry is 0 when the program has not set it and
     * `0x01RRGGBB` when it has — the same bit-24 truecolor convention the
     * cell attributes use, so "unset" and "black" are different values. An
     * unset index therefore still follows the THEME (the renderer's own
     * g_ansi_colors, which app_apply_theme rewrites), and a set one belongs
     * to the program that set it. Seeding a private copy of all 256 defaults
     * instead would freeze the theme at the moment of the first OSC 4 and
     * would need a second copy of the cube/greyscale table living beside the
     * renderer's. */
    u32     *palette;            /* heap [256] overrides, or NULL */
    bool     palette_modified;   /* an override changed; the render cache rebuilds */
    u32      default_fg;         /* default foreground color (from palette) */
    u32      default_bg;         /* default background color (from palette) */
    u32      cursor_color;       /* cursor color (from palette) */

    /* Callbacks */
    void   (*on_bell)(Terminal *t, void *userdata);
    void   (*on_title)(Terminal *t, const char *title, void *userdata);
    void   (*on_cwd)(Terminal *t, const char *path, void *userdata);
    void   (*on_response)(Terminal *t, const u8 *data, i32 len, void *userdata);
    void   (*on_clipboard_set)(Terminal *t, const char *data, void *userdata);
    const char *(*on_clipboard_get)(Terminal *t, void *userdata);
    void   (*on_long_command)(Terminal *t, f64 duration, i32 exit_code, void *long_cmd_userdata);
    void   (*on_command_prompt)(Terminal *t, void *command_userdata);
    void   (*on_command_finished)(Terminal *t, const char *command, i32 exit_code, void *command_userdata);
    /* The program in the pane asked for the user's attention in-band: OSC 9 ;
     * text, OSC 777 ; notify or OSC 99. Both strings are valid UTF-8 with
     * controls and deceptive code points taken out, and bounded
     * (TERM_NOTIFY_*_CAP); `title` may be "". They are still the PROGRAM's
     * words: the receiver limits how often it listens. */
    void   (*on_notify)(Terminal *t, const char *title, const char *body, void *userdata);
    void    *long_cmd_userdata;
    void    *command_userdata;
    void    *userdata;

    /* What liu pasted into this pane, so a copy of an agent's
     * "[Pasted text #1 +11 lines]" placeholder can carry the text behind it
     * (terminal/paste_log.h). Opaque here and freed through its own function
     * pointer, so terminal.c — linked into dozens of test targets — does not
     * depend on the module. NULL until the pane's first paste. */
    void    *paste_log;
    void   (*paste_log_free)(void *paste_log);
};

bool terminal_sync_capture(Terminal *t);
void terminal_sync_release(Terminal *t);
/* Drop the picture's arena while no picture is being shown (a no-op while
 * one is). Called from a resize and from the memory-pressure reclaimer; the
 * next capture allocates for the geometry it finds. Returns the bytes
 * released, so a reclaimer can credit the ledger in the same pass. */
usize terminal_sync_trim(Terminal *t);
/* The reservation the picture's arena holds right now (0 when trimmed). */
usize terminal_sync_arena_bytes(const Terminal *t);
Terminal *terminal_sync_view(Terminal *t);

/* Mark a row (or all rows) as needing redraw.
 * multi-word bitmap — no silent truncation beyond row 63. */
#define TERM_DIRTY_ROW(t, row) do { \
    (t)->dirty = true; \
    i32 _tdr = (row); \
    if (_tdr >= 0 && _tdr < (t)->rows && (t)->dirty_rows) \
        (t)->dirty_rows[_tdr >> 6] |= (1ULL << (_tdr & 63)); \
} while(0)
#define TERM_DIRTY_ALL(t) do { \
    (t)->dirty = true; \
    if ((t)->dirty_rows) \
        memset((t)->dirty_rows, 0xFF, (usize)(t)->dirty_words * sizeof(u64)); \
} while(0)

static inline bool term_row_dirty(const Terminal *t, i32 row) {
    if (row < 0 || row >= t->rows || !t->dirty_rows) return false;
    return (t->dirty_rows[row >> 6] >> (row & 63)) & 1ULL;
}
static inline void term_clear_dirty(Terminal *t) {
    t->dirty = false;
    if (t->dirty_rows)
        memset(t->dirty_rows, 0, (usize)t->dirty_words * sizeof(u64));
}

/* =========================================================================
 * API
 * ========================================================================= */

Terminal *terminal_create(i32 cols, i32 rows);
/* Everything this Terminal owns, in bytes: scrollback payloads, both cell grids
 * including their slack rows, retained image pixels and the parser's overflow
 * buffers. For whoever has to BOUND a Terminal rather than describe it
 * (`tab_trash_defer`): `sb_byte_usage` alone counts a terminal with no
 * scrollback and 4 MiB of images as zero. */
usize     terminal_owned_bytes(const Terminal *t);
void      terminal_destroy(Terminal *t);
void      terminal_resize(Terminal *t, i32 cols, i32 rows);
void      terminal_set_scrollback_limit(Terminal *t, i32 limit);
void      terminal_refresh_scrollback_usage(Terminal *t);

/* Feed raw bytes from PTY/SSH into terminal (parses VT sequences) */
void      terminal_feed(Terminal *t, const u8 *data, usize len);
/* Release the APC/DCS payload buffers and the memory-ledger bytes they hold.
 * Lives in vt_parser.c with the rest of the string-buffer lifetime. */
void      terminal_str_bufs_free(Terminal *t);

/* What this terminal retains, split the way the memory ledger's classes are:
 * the compressed history plus the ring that holds it, and everything that is
 * the live screen rather than history. The per-frame accounting walk in ui.c
 * is the only caller. */
usize     terminal_scrollback_bytes(const Terminal *t);
/* What sb_cache holds (its blocks and itself), and give it back. */
usize     termline_cache_bytes(const Terminal *t);
void      termline_cache_free(Terminal *t);
/* Called on a steady cadence: after `quiet_limit` calls in a row with no line
 * evicted from the history, the cache is given back (a flood is over). */
void      termline_cache_tick(Terminal *t, u32 quiet_limit);
usize     terminal_grid_bytes(const Terminal *t);

/* Return the grid window to its allocation base. Must be called before any
 * code frees or reallocs t->cells (resize, destroy) — see the grid_base
 * comment in struct Terminal. No-op unless a scroll has slid the window. */
void      terminal_grid_unslide(Terminal *t);

/* Exchange the live grid with the parked one, slide state and all. This is the
 * alt-screen switch: neither grid has to be unslid, because the base/slide/
 * slack triple travels with its buffer. */
void      terminal_grid_swap_alt(Terminal *t);

/* Release the parked grid (alt_cells + alt_base) and zero its window. */
void      terminal_grid_free_alt(Terminal *t);

/* Get cell at position */
Cell     *terminal_cell_at(Terminal *t, i32 col, i32 row);

/* Resolve a visible row (0..rows-1) into either a primary-grid pointer or
 * a scrollback line pointer, honoring t->scroll_offset. If the visible row
 * is in scrollback, *out_len is the scrollback line length (which may be
 * < t->cols); cells past that should render as blank spaces.
 * For primary rows, *out_len == t->cols.
 * Returns NULL if out of bounds. */
const Cell *terminal_visible_row(Terminal *t, i32 visible_row, i32 *out_len);

/* Internal (buffer.c): drop the oldest scrollback line and advance sb_abs_base.
 * Every removal from the FRONT of the ring goes through this one. */
void sb_evict_oldest(Terminal *t);

/* Is the visible row soft-wrapped, i.e. does its text continue on the row
 * below with no line break between them? Answered for the row as DISPLAYED,
 * so it reads t->line_wrapped for a live row and the scrollback line's own
 * flag for a row scrolled into history — the same split terminal_visible_row
 * makes. Copying and word/line selection both stand on this: a wrapped row
 * boundary is not a newline and not a word boundary.
 *
 * Always false on the alt screen, which keeps no wrap flags of its own (the
 * array is shared with the primary buffer and only primary-screen reflow
 * consumes it, so alt-screen autowrap deliberately does not mark it). */
bool  terminal_visible_row_wrapped(Terminal *t, i32 visible_row);

/* The same two readers WITHOUT the viewport clip. `visible_row` is still a
 * viewport coordinate (row 0 is the top of the screen as displayed), but it
 * may lie above the top or below the bottom: everything from the oldest
 * scrollback line down to the last row of the live grid resolves, and only a
 * row past the grid's bottom (or evicted from the ring) answers NULL/false.
 *
 * This is what a SELECTION reads through. Its rows are viewport rows that
 * selection_sync shifts as the view scrolls, and the drag that made it can
 * scroll the view under it — dragging past the top edge scrolls history in,
 * and every scrolled line pushes the pressed cell one row further below the
 * screen. The text there is not gone, it is just not on screen, and reading
 * it through the clipped reader copied an EMPTY LINE for every such row: the
 * selection's tail came back as a run of newlines. Same pointer-validity rule
 * as terminal_visible_row (a cold row materialises into one shared scratch
 * buffer). terminal_row_exists is the bounds test alone, for a walker that
 * only wants to know whether stepping onto a row is possible. */
const Cell *terminal_row_unclipped(Terminal *t, i32 visible_row, i32 *out_len);
bool  terminal_row_wrapped_unclipped(Terminal *t, i32 visible_row);
bool  terminal_row_exists(const Terminal *t, i32 visible_row);

/* Sparse underline attribute access */
void  terminal_set_underline(Terminal *t, i32 col, i32 row, u32 color, u8 style);
void  terminal_get_underline(Terminal *t, i32 col, i32 row, u32 *color, u8 *style);
void  terminal_clear_underline_map(Terminal *t);
/* The rows of [top, bot] moved by `n` (> 0 up, < 0 down): move their underline
 * entries with them. Called by the two scroll primitives; costs one branch
 * while the map is empty. */
void  terminal_underline_scroll(Terminal *t, i32 top, i32 bot, i32 n);
/* The cells of `row` from column `from` on moved by `n` columns (> 0 right:
 * ICH, insert mode; < 0 left: DCH). Entries pushed past either end go with
 * their cells. */
void  terminal_underline_shift_cols(Terminal *t, i32 row, i32 from, i32 n);
/* Exchange the live map with the parked one / free the parked one. The grid
 * swap calls the first; leaving the alt screen calls the second. */
void  terminal_underline_swap_parked(Terminal *t);
void  terminal_underline_drop_parked(Terminal *t);

/* Scrollback */
/* Erase Saved Lines (CSI 3 J): drop every scrollback row. Advances
 * sb_abs_base by the number dropped so anything anchored to an absolute line
 * — images, the selection — resolves as evicted rather than as some other
 * line. See buffer.c. */
void      sb_clear(Terminal *t);
void      sb_newest_break_wrap(Terminal *t);   /* row 0 no longer continues the newest history line */

void      terminal_scroll_up(Terminal *t, i32 lines);
void      terminal_scroll_down(Terminal *t, i32 lines);
void      terminal_scroll_to_bottom(Terminal *t);

/* Selection */
void      terminal_select_start(Terminal *t, i32 col, i32 row);
void      terminal_select_update(Terminal *t, i32 col, i32 row);
char     *terminal_select_text(Terminal *t);
void      terminal_select_clear(Terminal *t);

/* Generate response bytes for key input → send to PTY/SSH */
i32       terminal_key_input(Terminal *t, u32 key, u32 mods, u8 *out, i32 out_size);
i32       terminal_char_input(Terminal *t, u32 codepoint, u32 mods, u8 *out, i32 out_size);

/* Smart selection (per-terminal, in selection.c) */
void  selection_start(Terminal *t, i32 col, i32 row, i32 click_count, bool alt_held);
void  selection_update(Terminal *t, i32 col, i32 row);
/* A character drag between pointer EDGES (see TermSelection.edge_mode):
 * `right_half` says the pointer is in the right half of (col,row), i.e. its
 * nearest edge is the one AFTER the cell. Word/line selections ignore it. */
void  selection_start_edge(Terminal *t, i32 col, i32 row, bool right_half, bool alt_held);
void  selection_update_edge(Terminal *t, i32 col, i32 row, bool right_half);
void  selection_clear(Terminal *t);

/* Shift+click: move the free end of the existing selection to (col,row),
 * keeping the end the gesture was anchored to. Extends by whatever unit the
 * selection was made with — a word selection grows by words, a line selection
 * by lines. With no selection active it starts a plain one at that point, so
 * a first Shift+click behaves like a click. */
void  selection_extend(Terminal *t, i32 col, i32 row);

/* Re-derive the selection's viewport rows from its content anchor, and drop it
 * when the content it covered is gone. Called by selection_active/_contains/
 * _get_text, so every reader — the renderer and the clipboard alike — sees the
 * same answer; callers do not need to invoke it themselves. */
void  selection_sync(Terminal *t);
bool  selection_active(Terminal *t);
bool  selection_contains(Terminal *t, i32 col, i32 row);

/* Trim the blanks off the end of every copied line (default on). Process-wide
 * rather than per-terminal: there is one clipboard and one preference behind
 * it, whichever window the selection was made in. */
void  selection_set_trim_trailing(bool on);
char *selection_get_text(Terminal *t);
/* The columns of `row` (a viewport row, not clipped to the screen) that
 * selection_get_text takes: [*first, *last]. False when the row contributes
 * no text. This IS the copy's per-row arithmetic — selection_get_text calls
 * it — so a highlight painted from it shows exactly what Cmd+C will put on
 * the clipboard: interior blanks included, blanks the copy trims off the end
 * of a line not. Not synced (the render gate syncs once per frame). */
bool  selection_row_copy_span(Terminal *t, i32 row, i32 *first, i32 *last);

/* Search (per-terminal, in search.c) */
/* `query` is searched EXACTLY as given unless `use_regex` (POSIX ERE); there
 * is no implicit /pattern/ syntax. A regex that does not compile leaves
 * search.status == TERM_SEARCH_BAD_REGEX with no matches. */
void  terminal_search_start(Terminal *t, const char *query, bool case_sensitive, bool use_regex);
/* Live Find: compile once, scan a bounded number of rows per UI turn, and
 * retain immutable history matches on output. restart coalesces mutations
 * while a scan is pending instead of abandoning its progress. */
void  terminal_search_begin(Terminal *t, const char *query, bool case_sensitive, bool use_regex);
void  terminal_search_restart(Terminal *t);
bool  terminal_search_step(Terminal *t, f64 budget_seconds, i32 max_rows);
bool  terminal_search_pending(const Terminal *t);
i32   terminal_search_match_row(const Terminal *t, const TermSearchMatch *match);
usize terminal_search_bytes(const Terminal *t);
void  terminal_search_next(Terminal *t);
void  terminal_search_prev(Terminal *t);
void  terminal_search_stop(Terminal *t);
bool  terminal_search_is_highlighted(Terminal *t, i32 col, i32 row);
bool  terminal_search_is_current(Terminal *t, i32 col, i32 row);
/* Viewport row of match `index` at the CURRENT scroll (may be outside
 * [0, rows)), or -1 for no such match. Match rows are grid-relative:
 * negative ones are scrollback, `row + scroll_offset` is where it shows. */
i32   terminal_search_visible_row(const Terminal *t, i32 index);
/* Re-pick `current` after a re-run: the match nearest to the absolute line
 * (sb_abs_base + sb_count + row) and column the previous current sat on, so
 * output arriving under an open search does not throw the cursor back to the
 * newest match. Returns true when the same match was found exactly. */
bool  terminal_search_set_current_at(Terminal *t, i64 abs_line, i32 col);

/* Inline image management */
void  terminal_clear_images(Terminal *t);
void  terminal_delete_image(Terminal *t, u32 image_id);
i32   terminal_add_image(Terminal *t, u8 *pixels, i32 w, i32 h,
                         i32 col, i32 row, i32 cols, i32 rows,
                          u32 image_id, u32 placement_id);
bool  terminal_image_async_pump(Terminal *t);
bool  terminal_image_async_pending(const Terminal *t);
bool  terminal_image_async_completion_ready(const Terminal *t);
void  terminal_image_async_shutdown(Terminal *t);


/* Mouse reporting (in mouse.c) */
i32   terminal_mouse_encode(Terminal *t, i32 button, i32 col, i32 row,
                             bool pressed, u32 mods, u8 *out, i32 out_size);
/* The pointer moved with NO button held (any-event tracking, 1003). */
i32   terminal_mouse_encode_move(Terminal *t, i32 col, i32 row, u32 mods, u8 *out, i32 out_size);
i32   terminal_mouse_scroll(Terminal *t, i32 col, i32 row, bool up, u32 mods,
                             u8 *out, i32 out_size);
/* Motion with `button` (0 left, 1 middle, 2 right) held, for a program that
 * asked for it: 1002 (button-event) or 1003 (any-event). 0 bytes otherwise. */
i32   terminal_mouse_encode_drag(Terminal *t, i32 button, i32 col, i32 row,
                                 u32 mods, u8 *out, i32 out_size);

/* Shell integration: navigate between prompts */
void  terminal_goto_prev_prompt(Terminal *t);
void  terminal_goto_next_prompt(Terminal *t);
SemanticZone terminal_row_zone(Terminal *t, i32 row);

/* Palette initialization (OSC 4/10/11/12) */
void  terminal_init_palette(Terminal *t);

/* OSC 4 — set one indexed colour on THIS terminal. `rgb` is 0xRRGGBB. The
 * override table is allocated on first use and freed with the terminal. */
void  terminal_palette_set(Terminal *t, i32 idx, u32 rgb);
/* OSC 104 — drop overrides: one index, or every one when idx < 0. The index
 * goes back to following the theme, which is what the protocol's reset means. */
void  terminal_palette_reset(Terminal *t, i32 idx);

/* The THEME's default foreground, background and cursor colours, as 0xRRGGBB:
 * what an OSC 10 / 11 / 12 query is answered with (vt_parser.c has the rule
 * and why the answer is the theme's and never a colour a program set).
 * Process-wide, like the renderer's ANSI table that app_apply_config rewrites
 * beside it; until the app has said them — every headless tool and test —
 * the query is not answered at all. Main thread. */
void  terminal_theme_colors_set(u32 fg_rgb, u32 bg_rgb, u32 cursor_rgb);
/* The THEME's sixteen colours as the screen draws them (index 0 is the
 * theme's background and 7 its foreground, FG_DEFAULT / BG_DEFAULT), as
 * 0xRRGGBB: what an OSC 4 query is answered with — 16-255 come from
 * terminal/palette256.h. Never a colour a program set. Forgotten with the
 * three above. Main thread. */
void  terminal_theme_palette_set(const u32 shown[16]);
void  terminal_theme_colors_forget(void);
/* `code` is the OSC number: 10 foreground, 11 background, 12 cursor. */
bool  terminal_theme_color(i32 code, u32 *rgb);
/* Index `idx` (0-255) of the theme's palette; false until the app has said it. */
bool  terminal_theme_index_color(i32 idx, u32 *rgb);
/* Is the theme a dark one (by its background)? False when no theme is known. */
bool  terminal_theme_is_dark(bool *dark);
/* The theme went from dark to light or back: a program that asked to be told
 * (DEC private mode 2031) gets `CSI ? 997 ; 1 n` (dark) or `; 2 n` (light).
 * The same answer `CSI ? 996 n` gets. A digit of the terminal's own, nothing
 * a program chose. */
void  terminal_color_scheme_changed(Terminal *t);
/* The override for `idx`, or false when the index still follows the theme.
 * The render path asks this per index while rebuilding its LUT. */
bool  terminal_palette_get(const Terminal *t, i32 idx, u32 *out_rgb);

/* Where a cursor position report (CSI 6 n) would put the cursor right now,
 * 1-based as reported: the same rule vt_parser.c answers with. False while the
 * parser is inside a sequence or a character (not at rest), when nothing may
 * be answered on its behalf — core/term_reply.h, the pty reader's own answer
 * to a chunk that is nothing but questions. Main thread. */
bool  terminal_reply_position(const Terminal *t, i32 *row, i32 *col);

#endif /* TERMINAL_H */
