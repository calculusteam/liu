/*
 * Liu - terminal emulator
 * VT100/xterm compatible terminal state machine.
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

/* Semantic zone type for shell integration prompts */
typedef enum {
    ZONE_NONE = 0,
    ZONE_PROMPT,
    ZONE_INPUT,
    ZONE_OUTPUT,
} SemanticZone;

/* Forward decl so TermLine can carry a cold pointer below. */
struct TermLineCompressed_s;

/* Scrollback ring entry. WT-hybrid layout: a single TermLine slot is either
 * hot (cells != NULL, cold == NULL) or cold (cells == NULL, cold != NULL),
 * never both at once. The cold path stores codepoints + RLE attribute runs
 * (~4× smaller for typical AI / build-log content); the hot path is the
 * historical Cell array used by the primary screen and by transient unfold
 * buffers during resize.
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
 * Phase 1 (skeleton): WT-hybrid compressed scrollback line.
 *
 * AI-agent / vibe-coding terminals are dominated by long uniform-style runs:
 * Claude prose, build logs, file dumps. A 200-cell line typically uses 1-3
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
 * Today this struct is unused by the runtime — the helpers in buffer.c
 * (termline_compress / termline_materialize / termline_codepoints /
 * termline_compressed_destroy) are exercised only by direct callers (later
 * commits will move sb_push / sb_get over). The legacy `TermLine` path
 * remains the source of truth until phase 2. */
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

    /* Wide-char trailer bitmap: bit i set => codepoints[i] is the spacer
     * (right half) of a wide character. Size = ceil(len/64) u64 words. */
    u64     *wide_bits;

    bool     wrapped;     /* matches TermLine.wrapped */
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

/* =========================================================================
 * Selection (per-terminal instance)
 * ========================================================================= */

typedef struct {
    i32  start_col, start_row;
    i32  end_col, end_row;
    bool active;
    bool is_word;
    bool is_line;
    bool is_rect;
} TermSelection;

/* =========================================================================
 * Search (per-terminal instance)
 * ========================================================================= */

typedef struct {
    i32  col;
    i32  row;
    i32  length;
} TermSearchMatch;

typedef struct {
    char             query[256];
    bool             active;
    bool             case_sensitive;
    bool             use_regex;
    TermSearchMatch *matches;
    i32              match_count;
    i32              match_cap;
    i32              current;
} TermSearch;

/* Sparse underline attributes -- only allocated for cells that use non-default underline */
typedef struct {
    u32  key;              /* row * cols + col (0xFFFFFFFF = empty) */
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
    /* Agent-detected inline previews are stored as a small downscaled
     * thumbnail; `src_path` keeps the original file so a click can open the
     * full-resolution image in a viewer. NULL for protocol images (Sixel /
     * iTerm2 / Liu graphics), which are shown at their requested size and are
     * not click-to-expand. `clickable` gates the hit-test. */
    char *src_path;
    bool  clickable;
    /* Screen rect of the last frame this slot was drawn at (framebuffer px),
     * cached by the renderer so the click handler can hit-test thumbnails
     * without redoing the scroll/split placement math. Valid only while the
     * slot was visible last frame. */
    f32  scr_x, scr_y, scr_w, scr_h;
} TermImage;

typedef struct Terminal Terminal;

struct Terminal {
    /* Screen buffer (primary) */
    Cell    *cells;           /* rows * cols grid */
    i32      cols;
    i32      rows;

    /* Alternate screen buffer */
    Cell    *alt_cells;
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
    i32      params[16];
    i32      param_count;
    bool     param_has_value;

    /* Saved cursor (DECSC/DECRC) */
    i32      saved_x, saved_y;
    CellAttr saved_attr;
    u32      saved_ul_color;
    u8       saved_ul_style;

    /* Dirty tracking — renderer checks these.
     * multi-word bitmap so > 64 row grids aren't silently truncated.
     * Sized to ceil(rows / 64) words; reallocated by terminal_resize(). */
    bool     dirty;          /* any change at all (frame-level) */
    u64     *dirty_rows;     /* bitmask: bit (r%64) in word (r/64) */
    i32      dirty_words;    /* allocated length of dirty_rows */

    /* Selection state (per-terminal) */
    TermSelection selection;

    /* Search state (per-terminal) */
    TermSearch search;

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

    /* Hyperlink state (OSC 8) — lazy allocated */
    char    *hyperlink_uri;      /* NULL until first hyperlink */
    bool     in_hyperlink;

    /* Semantic prompt regions (OSC 133) */
    i32      prompt_start_row;
    i32      input_start_row;
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

    /* Liu keyboard protocol */
    u32      Liu_kbd_flags;
    u32      Liu_kbd_stack[8];
    i32      Liu_kbd_stack_len;

    /* modifyOtherKeys (xterm) */
    u8       modify_other_keys;  /* 0=off, 1=level1, 2=level2 */

    /* Sparse underline map */
    UnderlineEntry *ul_map;
    i32             ul_map_cap;
    i32             ul_map_count;

    /* BiDi (bidirectional text) */
    bool     bidi_enabled;       /* apply implicit BiDi reordering */

    /* Last printed character (for REP CSI b) */
    u32      last_printed_char;

    /* Inline images — heap [MAX_TERM_IMAGES] when non-NULL, else no slots
     * have been allocated yet. terminal_add_image lazy-creates the array;
     * terminal_destroy / terminal_clear_images free the slots and pixels. */
    TermImage *images;
    i32        image_count;
    u64        image_serial_next;
    usize      images_byte_usage;  /* sum of pixel-buffer bytes across valid slots */

    /* Inline-image sizing/detection support. cell_px_* are pixel dimensions of
     * one cell, pushed down from the UI (it owns the font metrics) so the
     * terminal can convert decoded pixel sizes into a cell-grid footprint and
     * reserve the right number of rows. inline_image_detect gates the
     * "agent printed an image path → show it inline" scan (default on). */
    i32        cell_px_w, cell_px_h;
    bool       inline_image_detect;
    /* An image decoded during a line-feed scan, parked until just after the LF
     * advances/scrolls so it anchors to the blank line below the path text.
     * Owned here until terminal_place_image_inline consumes it. */
    u8        *pending_img;
    i32        pending_img_w, pending_img_h;
    /* Absolute path of the parked image, owned here until placement consumes
     * it. Lets terminal_place_image_inline stash the source on the thumbnail
     * so a later click can reopen the full-resolution file. */
    char      *pending_img_path;
    /* Dedup key = hash(resolved path) mixed with the file mtime. Skips
     * re-placing the same image on a tight repaint loop, while a rewritten
     * file (new mtime) still re-shows. 0 = nothing placed yet. */
    u64        last_inline_img_key;

    /* APC (Liu graphics) dynamic buffer — lazily allocated like dcs_buf */
    u8      *apc_buf;
    i32      apc_len;
    i32      apc_cap;

    /* Line wrap tracking */
    bool    *line_wrapped;       /* per-row wrap flag */

    /* Semantic zones (shell integration: prompt/input/output markers) */
    SemanticZone *row_zones;     /* per-row zone type */
    SemanticZone  current_zone;  /* current zone being written to */

    /* Per-terminal ANSI palette (for OSC 4 palette changes). Lazy-allocated
     * so the 1 KB table only lives for terminals that actually receive
     * OSC 4. Until then, default_fg/bg/cursor_color and the global
     * g_ansi_colors LUT cover every read path; readers must NULL-check
     * before indexing. */
    u32     *palette;            /* heap [256] when non-NULL, else use defaults */
    bool     palette_modified;   /* true if OSC 4 has changed palette */
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
    void   (*on_command_finished)(Terminal *t, const char *command, i32 exit_code, void *command_userdata);
    void    *long_cmd_userdata;
    void    *command_userdata;
    void    *userdata;
};

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
void      terminal_destroy(Terminal *t);
void      terminal_resize(Terminal *t, i32 cols, i32 rows);
void      terminal_set_scrollback_limit(Terminal *t, i32 limit);
void      terminal_refresh_scrollback_usage(Terminal *t);

/* Feed raw bytes from PTY/SSH into terminal (parses VT sequences) */
void      terminal_feed(Terminal *t, const u8 *data, usize len);

/* Get cell at position */
Cell     *terminal_cell_at(Terminal *t, i32 col, i32 row);

/* Resolve a visible row (0..rows-1) into either a primary-grid pointer or
 * a scrollback line pointer, honoring t->scroll_offset. If the visible row
 * is in scrollback, *out_len is the scrollback line length (which may be
 * < t->cols); cells past that should render as blank spaces.
 * For primary rows, *out_len == t->cols.
 * Returns NULL if out of bounds. */
const Cell *terminal_visible_row(Terminal *t, i32 visible_row, i32 *out_len);

/* Sparse underline attribute access */
void  terminal_set_underline(Terminal *t, i32 col, i32 row, u32 color, u8 style);
void  terminal_get_underline(Terminal *t, i32 col, i32 row, u32 *color, u8 *style);
void  terminal_clear_underline_map(Terminal *t);

/* Scrollback */
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
void  selection_clear(Terminal *t);
bool  selection_active(Terminal *t);
bool  selection_contains(Terminal *t, i32 col, i32 row);
char *selection_get_text(Terminal *t);

/* Search (per-terminal, in search.c) */
void  terminal_search_start(Terminal *t, const char *query, bool case_sensitive, bool use_regex);
void  terminal_search_next(Terminal *t);
void  terminal_search_prev(Terminal *t);
void  terminal_search_stop(Terminal *t);
bool  terminal_search_is_highlighted(Terminal *t, i32 col, i32 row);
bool  terminal_search_is_current(Terminal *t, i32 col, i32 row);

/* Inline image management */
void  terminal_clear_images(Terminal *t);
void  terminal_delete_image(Terminal *t, u32 image_id);
i32   terminal_add_image(Terminal *t, u8 *pixels, i32 w, i32 h,
                         i32 col, i32 row, i32 cols, i32 rows,
                         u32 image_id, u32 placement_id);

/* Pixel dimensions of one cell, pushed from the UI so the terminal can size
 * inline images and reserve the right number of grid rows. */
void  terminal_set_cell_pixels(Terminal *t, i32 cell_px_w, i32 cell_px_h);

/* Drop a freshly-decoded RGBA image into the text flow at the cursor: sizes it
 * to a cell-grid footprint (preserving aspect, clamped to the pane width and a
 * row cap), anchors it to the current line, and feeds that many line-feeds so
 * following output lands below it. Takes ownership of `pixels` (frees on any
 * early-out). Safe to call with a NULL/!pixels (no-op). */
void  terminal_place_image_inline(Terminal *t, u8 *pixels, i32 w, i32 h);

/* Mouse reporting (in mouse.c) */
i32   terminal_mouse_encode(Terminal *t, i32 button, i32 col, i32 row,
                             bool pressed, u32 mods, u8 *out, i32 out_size);
i32   terminal_mouse_scroll(Terminal *t, i32 col, i32 row, bool up, u32 mods,
                             u8 *out, i32 out_size);

/* Shell integration: navigate between prompts */
void  terminal_goto_prev_prompt(Terminal *t);
void  terminal_goto_next_prompt(Terminal *t);
SemanticZone terminal_row_zone(Terminal *t, i32 row);

/* Palette initialization (OSC 4/10/11/12) */
void  terminal_init_palette(Terminal *t);

#endif /* TERMINAL_H */
