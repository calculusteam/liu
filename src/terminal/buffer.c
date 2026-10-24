/*
 * Liu - terminal screen buffer operations
 * Ring buffer scrollback for O(1) eviction.
 */
#include "terminal/terminal.h"
#include "terminal/row_stats.h"

#ifdef LIU_ROW_STATS
/* row_stats times the cold branch; the terminal layer has no platform header
 * of its own, so the function is declared here. Declared only under the
 * macro so an ordinary build carries no reference to a symbol it never
 * calls. */
f64 platform_time_sec(void);
#endif
#include "core/memory.h"
#include "core/checked.h"
#include "core/mem_budget.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#ifdef LIU_SCROLLBACK_ALLOC_PROBE
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif
#endif

/* Hand-written cell ops: ARM64 NEON (Apple Silicon) or x86-64 SSE2. CMake sets
 * USE_ASM only when it linked the matching *_arm64.S / *_x86_64.S for this arch. */
#if defined(USE_ASM) && (defined(__aarch64__) || defined(__arm64__) || defined(__x86_64__) || defined(__amd64__))
#define HAVE_TERM_ASM 1
extern void asm_buffer_clear_row(void *row_start, int cols, u32 codepoint, u32 fg, u32 bg);
extern void asm_buffer_scroll_up(void *buffer, int rows, int cols, int n);
#else
#define HAVE_TERM_ASM 0
#endif

/* Spare rows appended to the grid allocation so a full-screen scroll can move
 * a pointer instead of the grid. Bounded in BYTES, because a wide grid would
 * otherwise turn a generous row count into megabytes per pane. 128 KB buys a
 * 16x reduction in copy traffic on the widest grids and 100x on an 80-column
 * one — but never less than the screen's own height (grid_ensure_slack): the
 * fold copies the whole grid, so a slack shorter than the screen copies more
 * than a row per scrolled line. */
#define GRID_SLACK_BYTES  (128u * 1024u)
#define GRID_SLACK_MIN_ROWS 16
#define GRID_SLACK_MAX_ROWS 128

void terminal_grid_unslide(Terminal *t) {
    if (!t || t->grid_slide <= 0 || !t->grid_base) return;
    memmove(t->grid_base, t->cells,
            (usize)t->rows * (usize)t->cols * sizeof(Cell));
    t->cells = t->grid_base;
    t->grid_slide = 0;
}

void terminal_grid_swap_alt(Terminal *t) {
    if (!t) return;
    Cell *c  = t->cells;      t->cells      = t->alt_cells; t->alt_cells = c;
    Cell *b  = t->grid_base;  t->grid_base  = t->alt_base;  t->alt_base  = b;
    i32   sd = t->grid_slide; t->grid_slide = t->alt_slide; t->alt_slide = sd;
    i32   sk = t->grid_slack; t->grid_slack = t->alt_slack; t->alt_slack = sk;
    /* Each screen's underline styles go with its grid: the primary's wait,
     * parked, while a full-screen program draws its own. */
    terminal_underline_swap_parked(t);
    /* ...and so do its wrap flags and prompt zones. The incoming arrays are
     * made first so the live pair is never NULL on entry (a fresh zeroed pair:
     * the program's screen starts with no wraps and no zones); on exit they are
     * the primary's again, and what is parked is the program's — dropped by
     * terminal_grid_free_alt() with its grid. */
    if (t->rows > 0) {
        if (!t->alt_line_wrapped)
            t->alt_line_wrapped = calloc((usize)t->rows, sizeof(bool));
        if (!t->alt_row_zones)
            t->alt_row_zones = calloc((usize)t->rows, sizeof(SemanticZone));
    }
    bool         *w = t->line_wrapped; t->line_wrapped = t->alt_line_wrapped; t->alt_line_wrapped = w;
    SemanticZone *z = t->row_zones;    t->row_zones    = t->alt_row_zones;    t->alt_row_zones    = z;
    /* Switching grids replaces every visible row without writing a cell.
     * Invalidate row caches even when the escape carries no text/repaint. */
    TERM_DIRTY_ALL(t);
}

void terminal_grid_free_alt(Terminal *t) {
    if (!t) return;
    /* free() wants the allocation, and alt_cells is a window into it. */
    free(t->alt_base ? t->alt_base : t->alt_cells);
    t->alt_cells = NULL;
    t->alt_base  = NULL;
    t->alt_slide = 0;
    t->alt_slack = 0;
    /* The parked screen's flags and zones are part of the same ownership. */
    free(t->alt_line_wrapped);
    t->alt_line_wrapped = NULL;
    free(t->alt_row_zones);
    t->alt_row_zones = NULL;
}

/* Grow the LIVE grid's allocation so it carries slack rows. Only ever reached
 * with that grid unslid (grid_slack == 0 implies grid_slide == 0), so t->cells
 * is the allocation and realloc is safe — which holds for the alt grid as much
 * as the primary, since the triple always describes whichever one is live.
 * Returns false and leaves the terminal untouched if the grid cannot grow — the
 * caller then takes the ordinary copying path. */
static bool grid_ensure_slack(Terminal *t) {
    if (t->grid_slack > 0) return true;
    if (t->cols <= 0 || t->rows <= 0) return false;

    usize row_bytes = (usize)t->cols * sizeof(Cell);
    usize want = GRID_SLACK_BYTES / row_bytes;
    /* Keep at least one screen of slack so folding can be amortized across
     * scroll operations. GRID_SLACK_BYTES bounds the extra storage for
     * wider grids. */
    if (want < (usize)t->rows) want = (usize)t->rows;
    if (want < GRID_SLACK_MIN_ROWS) want = GRID_SLACK_MIN_ROWS;
    if (want > GRID_SLACK_MAX_ROWS) want = GRID_SLACK_MAX_ROWS;

    usize total_rows = (usize)t->rows + want;
    usize total_cells;
    if (!checked_mul_usize(total_rows, (usize)t->cols, &total_cells)) return false;
    usize total_bytes;
    if (!checked_mul_usize(total_cells, sizeof(Cell), &total_bytes)) return false;

    Cell *nb = realloc(t->cells, total_bytes);
    if (!nb) return false;
    /* The slack rows are uninitialised: every row the window ever exposes is
     * cleared by the bottom-fill below before anything reads it. */
    t->cells      = nb;
    t->grid_base  = nb;
    t->grid_slack = (i32)want;
    t->grid_slide = 0;
    return true;
}

static Cell default_cell(void) {
    return (Cell){
        .codepoint = ' ',
        .attr = { .fg = FG_DEFAULT, .bg = BG_DEFAULT, .flags = 0 }
    };
}

/* Nonzero when a cell shows something, or is half of a wide character: a
 * codepoint other than 0 or a space, a background, a decoration a blank draws
 * (term_cell_attr_plain), the wide bits. One expression with no branch, so the
 * walk in scrollback_line_len decides four cells at a time. */
static inline u32 scrollback_cell_ink(const Cell *c) {
    return (c->codepoint & ~(u32)' ') | (c->attr.bg ^ (u32)BG_DEFAULT) |
           (u32)(c->attr.flags & (ATTR_WIDE | ATTR_WDUMMY | TERM_ATTR_VISIBLE_ON_BLANK));
}

/* The stored length of a line: its trailing cells that can show nothing are
 * dropped (term_cell_attr_plain). A blank with a background, inverse or a
 * decoration is NOT nothing — that is the bar of a status row, and dropping it
 * lost the bar the moment the row scrolled off (the attribute is not stored
 * anywhere else, so nothing could bring it back). */
static i32 scrollback_line_len(const Cell *row_cells, i32 len, bool wrapped) {
    if (wrapped) return len;
    /* A short line leaves most of its row blank, and this walk runs for every
     * line that scrolls off, so it decides four cells a step instead of
     * branching per cell. */
    while (len >= 4) {
        const Cell *c = &row_cells[len - 4];
        if (scrollback_cell_ink(c) | scrollback_cell_ink(c + 1) |
            scrollback_cell_ink(c + 2) | scrollback_cell_ink(c + 3)) break;
        len -= 4;
    }
    while (len > 0 && !scrollback_cell_ink(&row_cells[len - 1])) len--;
    return len;
}

static inline usize scrollback_line_bytes(i32 cap) {
    if (cap <= 0) return 0;
    /* size_mul returns 0 on overflow; preserve prior SIZE_MAX sentinel. */
    usize n = size_mul((usize)cap, sizeof(Cell));
    return n ? n : SIZE_MAX;
}

/* Bytes a single ring slot currently retains. Cold lines report their
 * compressed footprint; hot lines report the legacy Cell array bytes. */
static usize sb_line_retained_bytes(const TermLine *line) {
    if (!line) return 0;
    if (line->cold) return termline_compressed_bytes(line->cold);
    return scrollback_line_bytes(line->cap);
}

typedef struct TermLineCache TermLineCache;
static void termline_compressed_recycle(Terminal *t, TermLineCompressed *line);
static bool termline_compress_into(const Cell *cells, i32 len, bool wrapped,
                                   TermLineCompressed **out_ptr, TermLineCache *cache);

/* sb_line_release for a line leaving through the front of the ring: its block
 * is kept for the next line (termline_compressed_recycle). */
static void sb_line_evict(Terminal *t, TermLine *line) {
    if (!line) return;
    if (line->cold) {
        termline_compressed_recycle(t, line->cold);
        line->cold = NULL;
    }
    free(line->cells);
    line->cells = NULL;
    line->len = 0;
    line->cap = 0;
    line->wrapped = false;
}

/* Drop both hot and cold storage attached to a ring slot. Idempotent — safe
 * to call on a slot that's already empty. The slot is left fully zeroed so
 * the ring buffer can immediately reuse it. */
static void sb_line_release(TermLine *line) {
    if (!line) return;
    if (line->cold) {
        /* destroy already free()s the block — second free() trips libmalloc's
         * double-free guard and aborts. */
        termline_compressed_destroy(line->cold);
        line->cold = NULL;
    }
    free(line->cells);
    line->cells = NULL;
    line->len = 0;
    line->cap = 0;
    line->wrapped = false;
}

/* Drop the oldest scrollback line and advance sb_abs_base by one. THE way a
 * line leaves the front of the ring — the eviction on push, the byte budget,
 * and a shortened history limit all come here, so an absolute line number keeps
 * meaning the same line. */
void sb_evict_oldest(Terminal *t) {
    if (!t || t->sb_count <= 0 || !t->sb_ring) return;
    TermLine *oldest = &t->sb_ring[t->sb_head];
    usize old_bytes = sb_line_retained_bytes(oldest);
    sb_line_evict(t, oldest);
    if (t->sb_byte_usage > old_bytes) t->sb_byte_usage -= old_bytes;
    else t->sb_byte_usage = 0;
    t->sb_head = (t->sb_head + 1) % t->sb_capacity;
    t->sb_count--;
    t->sb_abs_base++;   /* one line dropped from the front */
}

Cell *terminal_cell_at(Terminal *t, i32 col, i32 row) {
    if (!t || !t->cells || col < 0 || col >= t->cols || row < 0 || row >= t->rows)
        return NULL;
    return &t->cells[row * t->cols + col];
}

/* Grow the per-Terminal scratch buffer to fit `cols` cells. Returns false on
 * alloc failure (caller treats as "no row"). */
static bool sb_scratch_reserve(Terminal *t, i32 cols) {
    if (cols <= 0) return false;
    if (t->sb_scratch && t->sb_scratch_cap >= cols) return true;
    i32 new_cap = t->sb_scratch_cap > 0 ? t->sb_scratch_cap : 64;
    while (new_cap < cols) {
        if (new_cap > INT32_MAX / 2) { new_cap = cols; break; }
        new_cap *= 2;
    }
    usize bytes;
    if (!checked_mul_usize((usize)new_cap, sizeof(Cell), &bytes)) return false;
    Cell *grown = realloc(t->sb_scratch, bytes);
    if (!grown) return false;
    t->sb_scratch = grown;
    t->sb_scratch_cap = new_cap;
    return true;
}

const Cell *terminal_visible_row(Terminal *t, i32 visible_row, i32 *out_len) {
    if (!t || visible_row < 0 || visible_row >= t->rows) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    return terminal_row_unclipped(t, visible_row, out_len);
}

bool terminal_row_exists(const Terminal *t, i32 visible_row) {
    if (!t) return false;
    if (t->sync_view) {
        if (visible_row < 0 || visible_row >= t->rows) return false;
        return !t->sync_view_slots || t->sync_view_slots[visible_row] >= 0;
    }
    i32 logical = visible_row - t->scroll_offset;
    if (logical >= 0) return logical < t->rows;
    if (!t->sb_ring || t->sb_count == 0) return false;
    i32 sb_idx = t->sb_count + logical;
    return sb_idx >= 0 && sb_idx < t->sb_count;
}

const Cell *terminal_row_unclipped(Terminal *t, i32 visible_row, i32 *out_len) {
    if (t && t->sync_view) {
        bool valid = visible_row >= 0 && visible_row < t->rows;
        i32 packed = visible_row;
        if (valid && t->sync_view_slots) {
            packed = t->sync_view_slots[visible_row];
            valid = packed >= 0;
        }
        if (out_len) *out_len = valid ? t->sync_view_lengths[visible_row] : 0;
        return valid ? t->cells + (usize)packed * t->cols : NULL;
    }
    if (!t) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    i32 logical = visible_row - t->scroll_offset;
    if (logical >= t->rows) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (logical >= 0) {
        liu_row_stat_live();
        if (out_len) *out_len = t->cols;
        return &t->cells[logical * t->cols];
    }
    /* Scrollback: sb_count + logical (logical is negative) */
    if (!t->sb_ring || t->sb_count == 0) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    i32 sb_idx = t->sb_count + logical;
    if (sb_idx < 0 || sb_idx >= t->sb_count) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    i32 ring_idx = (t->sb_head + sb_idx) % t->sb_capacity;
    TermLine *ln = &t->sb_ring[ring_idx];

    /* Cold row → materialize into the per-terminal scratch buffer. The
     * returned pointer is valid until the next visible-row resolve, so
     * callers that need to keep cells around must memcpy. */
    if (ln->cold) {
        if (!sb_scratch_reserve(t, ln->cold->len > 0 ? ln->cold->len : 1)) {
            if (out_len) *out_len = 0;
            return NULL;
        }
#ifdef LIU_ROW_STATS
        f64 mt0 = g_liu_row_timing_on ? platform_time_sec() : 0.0;
#endif
        i32 n = termline_materialize(ln->cold, t->sb_scratch, t->sb_scratch_cap);
#ifdef LIU_ROW_STATS
        liu_row_stat_cold(n, g_liu_row_timing_on
                             ? (u64)((platform_time_sec() - mt0) * 1e9) : 0);
#endif
        if (out_len) *out_len = n;
        return t->sb_scratch;
    }

    /* A hot scrollback line: still a Cell array, so it costs what the live
     * grid costs. */
    liu_row_stat_live();
    if (out_len) *out_len = ln->len;
    return ln->cells;
}

bool terminal_visible_row_wrapped(Terminal *t, i32 visible_row) {
    if (!t || visible_row < 0 || visible_row >= t->rows) return false;
    return terminal_row_wrapped_unclipped(t, visible_row);
}

bool terminal_row_wrapped_unclipped(Terminal *t, i32 visible_row) {
    if (!t) return false;
    /* The alt screen never marks wraps (see put_char), so asking would only
     * read the primary buffer's stale flags through the shared array. */
    if (t->mode & MODE_ALT_SCREEN) return false;

    i32 logical = visible_row - t->scroll_offset;
    if (logical >= t->rows) return false;
    if (logical >= 0)
        return t->line_wrapped ? t->line_wrapped[logical] : false;

    if (!t->sb_ring || t->sb_count == 0) return false;
    i32 sb_idx = t->sb_count + logical;
    if (sb_idx < 0 || sb_idx >= t->sb_count) return false;
    TermLine *ln = &t->sb_ring[(t->sb_head + sb_idx) % t->sb_capacity];
    return ln->cold ? ln->cold->wrapped : ln->wrapped;
}

void buffer_clear_region(Cell *cells, i32 cols, i32 x0, i32 y0, i32 x1, i32 y1, CellAttr attr) {
    Cell c = { .codepoint = ' ', .attr = attr };
    /* Clamp the horizontal range to the grid. Callers may pass x == cols when
     * the cursor is in deferred-wrap state (cursor_x == cols), which would
     * otherwise write one cell past the row — and past the whole grid on the
     * last row. */
    if (x0 < 0) x0 = 0;
    if (x1 > cols - 1) x1 = cols - 1;
#if HAVE_TERM_ASM
    if (c.attr.flags == 0 && x0 == 0 && x1 == cols - 1) {
        for (i32 y = y0; y <= y1; y++) {
            asm_buffer_clear_row(&cells[y * cols], cols, c.codepoint, c.attr.fg, c.attr.bg);
        }
        return;
    }
#endif
    for (i32 y = y0; y <= y1; y++) {
        for (i32 x = x0; x <= x1; x++) {
            cells[y * cols + x] = c;
        }
    }
}

/* Do these two cells belong to the same attribute run?
 *
 * This runs once per cell, twice per line across the two passes, in the hottest
 * loop of bulk output. The arguments are taken by value so the whole comparison
 * stays in registers: a memcmp needs both operands as real objects in memory,
 * which forces a masked copy of each cell's attributes to be spilled to the
 * stack and loaded straight back. The run counts and compressed bytes are
 * bit-for-bit what a memcmp of the masked attributes gives.
 *
 * Folding the wide-dummy mask into the XOR removes those temporaries: the bit
 * lives in a separate bitmap and must not split a run.
 *
 * `_pad` is not part of a cell's value: nothing reads or writes it, and both
 * the ASM and C row-clear paths zero it. It is excluded here so a future writer
 * of that field cannot turn every uniform line into one run per column. */
/* The wide LEAD bit (ATTR_WIDE) is folded out the same way and for the same
 * reason: it lives in the lead words after the dummy bitmap, and left in the
 * runs it would cut a run before and after every wide character. */
static inline bool attr_run_eq(CellAttr a, CellAttr b) {
    return a.fg == b.fg && a.bg == b.bg &&
           ((a.flags ^ b.flags) & ~(u16)(ATTR_WDUMMY | ATTR_WIDE)) == 0;
}

/* Build the run record for `attr`. The wide bits are cleared because
 * termline_materialize re-applies them from the bitmaps; leaving one set would
 * OR it onto every cell of the run. */
static inline AttrRun attr_run_make(CellAttr attr, u16 length) {
    attr.flags = (u16)(attr.flags & ~(u16)(ATTR_WDUMMY | ATTR_WIDE));
    /* Not part of a cell's value (attr_run_eq above), so not part of a run's
     * either. terminal_resize parks a transient stamp there; a compressed line
     * must never hand one back to a later materialize. */
    attr._pad = 0;
    return (AttrRun){ .attr = attr, .length = length, ._pad = 0 };
}

/* Compress `row_cells` into a freshly heap-allocated TermLineCompressed.
 * Returns NULL on allocation failure. */
static TermLineCompressed *sb_make_cold(Terminal *t, const Cell *row_cells, i32 len, bool wrapped) {
    TermLineCompressed *c = NULL;
    if (!termline_compress_into(row_cells, len, wrapped, &c, t->sb_cache)) {
        return NULL;
    }
    return c;
}

/* Install `cold` into the given ring slot, releasing any previous storage
 * (hot or cold) and updating the byte-usage accounting. */
static void sb_slot_install_cold(Terminal *t, TermLine *slot,
                                 TermLineCompressed *cold,
                                 i32 len, bool wrapped) {
    usize old_bytes = sb_line_retained_bytes(slot);
    sb_line_evict(t, slot);
    slot->cold    = cold;
    slot->len     = len;
    slot->cap     = 0;
    slot->cells   = NULL;
    slot->wrapped = wrapped;
    usize new_bytes = sb_line_retained_bytes(slot);
    if (t->sb_byte_usage > old_bytes) t->sb_byte_usage -= old_bytes;
    else t->sb_byte_usage = 0;
    if (!checked_add_usize(t->sb_byte_usage, new_bytes, &t->sb_byte_usage))
        t->sb_byte_usage = SIZE_MAX;
}

/* Place a heap-allocated cold line into the ring (tail-append + grow + head-
 * overwrite per existing eviction policy). On budget overflow `cold` is
 * destroyed and freed. Used by both sb_push (after compress) and sb_push_cold
 * (sleep snapshot v2 reader). Takes ownership of `cold`. */
static void sb_install_cold_line(Terminal *t, TermLineCompressed *cold) {
    if (!t || !t->sb_ring || t->sb_capacity == 0 || !cold) {
        if (cold) termline_compressed_destroy(cold);
        return;
    }
    usize incoming_bytes = termline_compressed_bytes(cold);
    if (t->sb_byte_budget > 0 && incoming_bytes > t->sb_byte_budget) {
        /* Single line is larger than the whole budget — drop instead of
         * letting RSS grow unbounded. */
        termline_compressed_destroy(cold);
        return;
    }

    while (t->sb_count > 0 && t->sb_byte_budget > 0 &&
           (t->sb_byte_usage >= t->sb_byte_budget ||
            incoming_bytes > t->sb_byte_budget - t->sb_byte_usage)) {
        sb_evict_oldest(t);
    }

    i32  len     = cold->len;
    bool wrapped = cold->wrapped;

    if (t->sb_count < t->sb_capacity) {
        i32 tail = (t->sb_head + t->sb_count) % t->sb_capacity;
        sb_slot_install_cold(t, &t->sb_ring[tail], cold, len, wrapped);
        t->sb_count++;
    } else if (t->sb_capacity < t->scrollback_limit) {
        i32 new_cap = t->sb_capacity > t->scrollback_limit / 2
                    ? t->scrollback_limit : t->sb_capacity * 2;
        TermLine *new_ring = calloc((usize)new_cap, sizeof(TermLine));
        if (new_ring) {
            for (i32 i = 0; i < t->sb_count; i++) {
                new_ring[i] = t->sb_ring[(t->sb_head + i) % t->sb_capacity];
            }
            free(t->sb_ring);
            t->sb_ring = new_ring;
            t->sb_head = 0;
            t->sb_capacity = new_cap;
            sb_slot_install_cold(t, &t->sb_ring[t->sb_count], cold, len, wrapped);
            t->sb_count++;
        } else {
            termline_compressed_destroy(cold);
        }
    } else {
        /* Ring at max capacity — overwrite oldest (head) in O(1). This drops a
         * line from the front (sb_slot_install_cold handles the byte
         * accounting), so bump the absolute-line base to match. The byte-budget
         * eviction above goes through sb_evict_oldest, which bumps it already;
         * this capacity path does not. */
        sb_slot_install_cold(t, &t->sb_ring[t->sb_head], cold, len, wrapped);
        t->sb_head = (t->sb_head + 1) % t->sb_capacity;
        t->sb_abs_base++;
    }
}

/* Ring buffer scrollback helpers */
void sb_push(Terminal *t, Cell *row_cells, i32 len, bool wrapped) {
    if (!t || !t->sb_ring || t->sb_capacity <= 0 || len < 0 ||
        (len > 0 && !row_cells)) return;
    len = scrollback_line_len(row_cells, len, wrapped);
    TermLineCompressed *cold = sb_make_cold(t, row_cells, len, wrapped);
    if (!cold) return;
    /* The caller already trimmed hard-ended rows. Reuse that answer rather
     * than adding another cell scan to the parser's compression hot path. */
    if (!wrapped) { cold->content_len = len; cold->content_valid = true; }
    sb_install_cold_line(t, cold);
}

/* Public sibling of sb_push that takes an already-compressed line. The
 * sleep-snapshot v2 reader uses this to install rows directly without paying
 * the compress pass. Takes ownership of `cold` (frees on overflow/error). */
void sb_push_cold(Terminal *t, TermLineCompressed *cold) {
    if (cold && !cold->wrapped && !cold->content_valid) {
        cold->content_len = termline_content_length(cold);
        cold->content_valid = true;
    }
    sb_install_cold_line(t, cold);
}

/* Drop the N most recently pushed lines.
 *
 * terminal_resize's width-unchanged path hands rows back to a screen that grew
 * taller, and those rows are already sitting compressed in the ring, so that
 * path does not re-derive them. Nothing leaves the FRONT here, so sb_abs_base
 * (which counts lines evicted from the front) is deliberately untouched. */
void sb_pop_newest(Terminal *t, i32 n) {
    if (!t || !t->sb_ring || t->sb_capacity <= 0 || n <= 0) return;
    if (n > t->sb_count) n = t->sb_count;
    for (i32 k = 0; k < n; k++) {
        i32 idx = (t->sb_head + t->sb_count - 1) % t->sb_capacity;
        TermLine *ln = &t->sb_ring[idx];
        usize bytes = sb_line_retained_bytes(ln);
        sb_line_release(ln);
        if (t->sb_byte_usage > bytes) t->sb_byte_usage -= bytes;
        else t->sb_byte_usage = 0;
        t->sb_count--;
    }
}

/* Erase Saved Lines: drop every scrollback row and give its memory back.
 *
 * `CSI 3 J` is the terminal protocol's "erase saved lines", and it is what ncurses' `clear`
 * sends through terminfo's E3 capability, which the standard 256-color TERM every Liu
 * session runs under defines. It is also how a user releases a deep scrollback without
 * closing the tab.
 *
 * sb_abs_base advances by the number dropped, exactly as the front-eviction path does: the
 * absolute line numbering is what images and the selection anchor are pinned to, and a reset
 * to zero would silently re-point them at lines that no longer exist. Everything anchored to
 * a dropped line then resolves as evicted, which is what it is.
 *
 * And it keeps the LIVE GRID's numbering identical, which is what makes this safe. A live
 * row g sits at (sb_abs_base + sb_count + g); before this call that is B + N + g, after it
 * is (B + N) + 0 + g. Every image and selection anchored to the screen resolves to the same
 * row it did a moment ago, and only the ones anchored to history that is now gone stop
 * resolving. */
/* The newest line of history no longer continues onto the screen: what it
 * continued onto (row 0 of the primary screen) has been erased or written
 * over from its first column. Without this the flag would outlive the text:
 * after a long line had scrolled off and the screen was cleared (Ctrl+L), the
 * next resize would join the fresh prompt onto the old line's tail. */
void sb_newest_break_wrap(Terminal *t) {
    if (!t || !t->sb_ring || t->sb_count <= 0 || t->sb_capacity <= 0) return;
    TermLine *ln = &t->sb_ring[(t->sb_head + t->sb_count - 1) % t->sb_capacity];
    if (ln->cold) ln->cold->wrapped = false;
    ln->wrapped = false;
}

void sb_clear(Terminal *t) {
    if (!t || !t->sb_ring || t->sb_capacity <= 0 || t->sb_count <= 0) return;
    i32 n = t->sb_count;
    for (i32 k = 0; k < n; k++) {
        i32 idx = (t->sb_head + k) % t->sb_capacity;
        sb_line_release(&t->sb_ring[idx]);
    }
    t->sb_head       = 0;
    t->sb_count      = 0;
    t->sb_byte_usage = 0;
    t->sb_abs_base  += (u64)n;
    termline_cache_free(t);     /* a cleared history gives its memory back */
    /* The viewport cannot stay scrolled into history that no longer exists. */
    t->scroll_offset = 0;
}

/* Get scrollback line by index (0 = oldest visible) */
TermLine *sb_get(Terminal *t, i32 index) {
    if (index < 0 || index >= t->sb_count) return NULL;
    i32 ring_idx = (t->sb_head + index) % t->sb_capacity;
    return &t->sb_ring[ring_idx];
}

/* =========================================================================
 * TermLineCompressed helpers: allocation, compression, materialisation and
 * freeing of the compressed form a scrollback line is kept in (sb_make_cold).
 * ========================================================================= */

static bool ws_set_bit(u64 *bits, i32 i) {
    if (!bits) return false;
    bits[i >> 6] |= (1ULL << (i & 63));
    return true;
}

static inline bool ws_get_bit(const u64 *bits, i32 i) {
    return bits && ((bits[i >> 6] >> (i & 63)) & 1ULL);
}

static bool termline_align_up(usize value, usize align, usize *out) {
    usize with_slack;
    if (!out || align == 0 || (align & (align - 1u)) != 0 ||
        !checked_add_usize(value, align - 1u, &with_slack)) return false;
    *out = with_slack & ~(align - 1u);
    return true;
}

static bool termline_block_layout(i32 len, u16 run_cap, bool leads,
                                  usize *runs_offset_out,
                                  usize *wb_offset_out,
                                  usize *total_size_out) {
    if (len <= 0 || len > (i32)UINT16_MAX || run_cap == 0 || run_cap > (u16)len)
        return false;

    usize cp_size, after_cp, runs_offset, runs_size, after_runs;
    usize wb_words = ((usize)len + 63u) / 64u * (leads ? 2u : 1u);
    usize wb_size, wb_offset, total_size;
    if (!checked_mul_usize((usize)len, sizeof(u32), &cp_size) ||
        !checked_add_usize(sizeof(TermLineCompressed), cp_size, &after_cp) ||
        !termline_align_up(after_cp, 16u, &runs_offset) ||
        !checked_mul_usize((usize)run_cap, sizeof(AttrRun), &runs_size) ||
        !checked_add_usize(runs_offset, runs_size, &after_runs) ||
        !termline_align_up(after_runs, 8u, &wb_offset) ||
        !checked_mul_usize(wb_words, sizeof(u64), &wb_size) ||
        !checked_add_usize(wb_offset, wb_size, &total_size)) return false;

    if (runs_offset_out) *runs_offset_out = runs_offset;
    if (wb_offset_out) *wb_offset_out = wb_offset;
    if (total_size_out) *total_size_out = total_size;
    return true;
}

#ifdef LIU_SCROLLBACK_ALLOC_PROBE
static TermlineAllocProbeStats g_termline_alloc_probe;

static u64 termline_probe_ns_between(const struct timespec *a,
                                     const struct timespec *b) {
    i64 sec = (i64)b->tv_sec - (i64)a->tv_sec;
    i64 ns = (i64)b->tv_nsec - (i64)a->tv_nsec;
    i64 total = sec * 1000000000ll + ns;
    return total > 0 ? (u64)total : 0;
}

static usize termline_probe_usable(void *p, usize requested) {
    (void)requested;
#if defined(__APPLE__)
    return p ? malloc_size(p) : 0;
#elif defined(__GLIBC__)
    return p ? malloc_usable_size(p) : 0;
#else
    (void)p;
    return requested;
#endif
}

static u32 termline_probe_size_class(usize n) {
    if (n <= 1) return 0;
    u32 i = 0;
    usize top = 1;
    while (top < n && i + 1 < 24) { top <<= 1; i++; }
    return i;
}

void termline_alloc_probe_record_alloc(void *p, usize requested,
                                       const struct timespec *t0,
                                       const struct timespec *t1) {
    usize usable = termline_probe_usable(p, requested);
    g_termline_alloc_probe.alloc_count++;
    g_termline_alloc_probe.requested_bytes += requested;
    g_termline_alloc_probe.usable_bytes += usable;
    g_termline_alloc_probe.size_class[termline_probe_size_class(usable)]++;
    liu_hist_add_ns(&g_termline_alloc_probe.malloc_ns,
                    termline_probe_ns_between(t0, t1));
}

void termline_alloc_probe_record_free(void *p, usize requested) {
    (void)requested;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    free(p);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_termline_alloc_probe.free_count++;
    liu_hist_add_ns(&g_termline_alloc_probe.free_ns,
                    termline_probe_ns_between(&t0, &t1));
}

void termline_alloc_probe_reset(void) {
    memset(&g_termline_alloc_probe, 0, sizeof g_termline_alloc_probe);
    liu_hist_reset(&g_termline_alloc_probe.malloc_ns);
    liu_hist_reset(&g_termline_alloc_probe.free_ns);
}

void termline_alloc_probe_get(TermlineAllocProbeStats *out) {
    if (out) *out = g_termline_alloc_probe;
}
#endif

/* ---- the history's blocks, kept for the next line -------------------------
 *
 * Once the history is full every line that scrolls in pushes one out: a
 * malloc for the new line's block and a free for the evicted one. A block that
 * leaves through the front of the ring (sb_evict_oldest, the overwrite at
 * capacity) goes onto its Terminal's free list for its size class instead, and
 * the next line of that class takes it.
 *
 * A class is a size malloc itself hands out — 16-byte steps up to 1008 bytes,
 * 512-byte steps up to TL_CACHE_MAX_BLOCK — and every block is allocated at
 * its class's size, which is what malloc would give the line anyway: no line
 * holds more memory for being cached, and termline_compressed_bytes still says
 * what the line needs. A longer line, and an empty one, is allocated and freed
 * directly. The cache is the Terminal's (the thread that feeds it pushes and
 * evicts its lines), at most TL_CACHE_BYTES of blocks, counted in
 * terminal_scrollback_bytes and freed with the scrollback (sb_clear,
 * terminal_destroy) wherever that runs — and once the history has stopped
 * moving (termline_cache_tick): it pays only while lines keep scrolling off. */
#define TL_CACHE_TINY_MAX   1008u
#define TL_CACHE_MAX_BLOCK  (16u * 1024u)
#define TL_CACHE_CLASSES    (TL_CACHE_TINY_MAX / 16u + TL_CACHE_MAX_BLOCK / 512u + 1u)
#ifndef TL_CACHE_BYTES
#define TL_CACHE_BYTES      ((usize)256 * 1024)
#endif

_Static_assert(sizeof(TermLineCompressed) <= 48, "slab_class must stay in the header's padding");

typedef struct TlFree { struct TlFree *next; } TlFree;
struct TermLineCache {
    TlFree *free[TL_CACHE_CLASSES];
    usize   bytes;          /* of the blocks on the lists */
    i64     seen_evicted;   /* sb_abs_base at the last termline_cache_tick */
    u32     quiet_ticks;    /* ticks in a row with nothing evicted */
};

#ifdef LIU_SB_CACHE_PROBE
u64 g_sb_cache_takes, g_sb_cache_hits, g_sb_cache_keeps, g_sb_cache_drops;
#define SB_CACHE_COUNT(x) ((x)++)
#else
#define SB_CACHE_COUNT(x) ((void)0)
#endif

/* The class of an `n`-byte block and the size it is allocated at; 0 for a
 * block that is allocated at `n` and never kept. */
static inline u8 tl_class(usize n, usize *size) {
    *size = n;
    if (n == 0 || n > TL_CACHE_MAX_BLOCK) return 0;
    if (n <= TL_CACHE_TINY_MAX) {
        usize k = (n + 15u) / 16u;                          /* 1 .. 63 */
        *size = k * 16u;
        return (u8)k;
    }
    usize k = (n + 511u) / 512u;                            /* 2 .. 32 */
    *size = k * 512u;
    return (u8)(TL_CACHE_TINY_MAX / 16u + k - 1u);          /* 64 .. 94 */
}

static inline usize tl_class_size(u8 c) {
    return c <= TL_CACHE_TINY_MAX / 16u ? (usize)c * 16u
                                        : (usize)(c - TL_CACHE_TINY_MAX / 16u + 1u) * 512u;
}

/* A block for `n` bytes: one `cache` kept of its class, else a new one at the
 * class's size. *cls is the class (0: allocated at `n`, never kept). */
static u8 *tl_block_take(TermLineCache *cache, usize n, u8 *cls) {
    usize size;
    u8 c = tl_class(n, &size);
    *cls = c;
    SB_CACHE_COUNT(g_sb_cache_takes);
    if (c && cache && cache->free[c]) {
        TlFree *b = cache->free[c];
        cache->free[c] = b->next;
        cache->bytes -= size;
        SB_CACHE_COUNT(g_sb_cache_hits);
        return (u8 *)b;
    }
    return malloc(size);
}

/* A line leaving the front of `t`'s history: kept for the next line of its
 * class while the cache has room, freed otherwise. */
static void termline_compressed_recycle(Terminal *t, TermLineCompressed *line) {
    if (!line) return;
    u8 c = line->slab_class;
#ifndef LIU_SCROLLBACK_ALLOC_PROBE
    usize size = c ? tl_class_size(c) : 0;
    if (c && c < TL_CACHE_CLASSES && t) {
        if (!t->sb_cache) t->sb_cache = calloc(1, sizeof *t->sb_cache);
        TermLineCache *cache = t->sb_cache;
        if (cache && cache->bytes + size <= TL_CACHE_BYTES) {
            TlFree *b = (TlFree *)(void *)line;
            b->next = cache->free[c];
            cache->free[c] = b;
            cache->bytes += size;
            SB_CACHE_COUNT(g_sb_cache_keeps);
            return;
        }
    }
#else
    (void)t; (void)c;
#endif
    SB_CACHE_COUNT(g_sb_cache_drops);
    termline_compressed_destroy(line);
}

void termline_cache_tick(Terminal *t, u32 quiet_limit) {
    if (!t || !t->sb_cache) return;
    TermLineCache *c = t->sb_cache;
    if (c->seen_evicted != t->sb_abs_base) {
        c->seen_evicted = t->sb_abs_base;
        c->quiet_ticks = 0;
    } else if (++c->quiet_ticks >= quiet_limit) {
        termline_cache_free(t);
    }
}

usize termline_cache_bytes(const Terminal *t) {
    return (t && t->sb_cache) ? t->sb_cache->bytes + sizeof *t->sb_cache : 0;
}

void termline_cache_free(Terminal *t) {
    if (!t || !t->sb_cache) return;
    for (u32 c = 0; c < TL_CACHE_CLASSES; c++) {
        while (t->sb_cache->free[c]) {
            TlFree *b = t->sb_cache->free[c];
            t->sb_cache->free[c] = b->next;
            free(b);
        }
    }
    free(t->sb_cache);
    t->sb_cache = NULL;
}

TermLineCompressed *termline_alloc_block(i32 len, u16 run_count, bool wrapped) {
    return termline_alloc_block_leads(len, run_count, wrapped, false);
}

/* The lead words of `line` (has_leads), or NULL. */
static inline const u64 *termline_lead_bits(const TermLineCompressed *line) {
    return line->has_leads && line->wide_bits ? line->wide_bits + (line->len + 63) / 64 : NULL;
}

/* `zero_arrays` false: only the header and the wide bitmaps are zeroed, for a
 * caller that writes every codepoint and every run itself. */
static TermLineCompressed *termline_alloc_block_impl(i32 len, u16 run_count, bool wrapped,
                                                     bool leads, bool zero_arrays,
                                                     TermLineCache *cache) {
    if (len < 0 || len > (i32)UINT16_MAX) return NULL;
    if (len == 0) {
        if (run_count != 0) return NULL;
        TermLineCompressed *empty = calloc(1, sizeof(TermLineCompressed));
        if (empty) empty->wrapped = wrapped;
        return empty;
    }

    usize runs_offset, wb_offset, total_size;
    if (!termline_block_layout(len, run_count, leads, &runs_offset,
                               &wb_offset, &total_size)) return NULL;

#ifdef LIU_SCROLLBACK_ALLOC_PROBE
    struct timespec alloc_t0, alloc_t1;
    clock_gettime(CLOCK_MONOTONIC, &alloc_t0);
#endif
#ifdef LIU_SCROLLBACK_ALLOC_PROBE
    u8 cls = 0;
    u8 *block = malloc(total_size);
    clock_gettime(CLOCK_MONOTONIC, &alloc_t1);
#else
    u8 cls = 0;
    u8 *block = tl_block_take(cache, total_size, &cls);
#endif
    if (!block) return NULL;
    if (zero_arrays) {
        memset(block, 0, total_size);
    } else {
        memset(block, 0, sizeof(TermLineCompressed));
        memset(block + wb_offset, 0, total_size - wb_offset);
    }

    TermLineCompressed *out = (TermLineCompressed *)block;
    out->codepoints = (u32 *)(block + sizeof(TermLineCompressed));
    out->runs       = (AttrRun *)(block + runs_offset);
    out->wide_bits  = (u64 *)(block + wb_offset);
    out->len        = len;
    out->cap        = len;
    out->run_count  = run_count;
    out->run_cap    = run_count;
    out->wrapped    = wrapped;
    out->has_leads  = leads;
    out->slab_class = cls;
#ifdef LIU_SCROLLBACK_ALLOC_PROBE
    termline_alloc_probe_record_alloc(block, total_size, &alloc_t0, &alloc_t1);
#endif
    return out;
}

TermLineCompressed *termline_alloc_block_leads(i32 len, u16 run_count, bool wrapped, bool leads) {
    return termline_alloc_block_impl(len, run_count, wrapped, leads, true, NULL);
}

/* The dummy bits and, when the line has them, the lead words. */
static void termline_set_wide_bits(TermLineCompressed *out, const Cell *cells, i32 len) {
    u64 *lead_bits = out->has_leads ? out->wide_bits + (len + 63) / 64 : NULL;
    for (i32 i = 0; i < len; i++) {
        if (cells[i].attr.flags & ATTR_WDUMMY) ws_set_bit(out->wide_bits, i);
        if (cells[i].attr.flags & ATTR_WIDE) ws_set_bit(lead_bits, i);
    }
}

bool termline_compress(const Cell *cells, i32 len, bool wrapped,
                       TermLineCompressed **out_ptr) {
    return termline_compress_into(cells, len, wrapped, out_ptr, NULL);
}

/* termline_compress, its block taken from `cache` when one of its class is
 * kept there (sb_push: the Terminal's history). */
static bool termline_compress_into(const Cell *cells, i32 len, bool wrapped,
                                   TermLineCompressed **out_ptr, TermLineCache *cache) {
    if (!out_ptr) return false;
    *out_ptr = NULL;

    if (len < 0 || len > (i32)UINT16_MAX) return false;

    /* Empty line: return a minimal block. */
    if (len == 0 || !cells) {
        TermLineCompressed *empty = calloc(1, sizeof(TermLineCompressed));
        if (!empty) return false;
        empty->wrapped = wrapped;
        *out_ptr = empty;
        return true;
    }

    /* The run walk, EIGHT cells a step while they all continue the current
     * run: their differences from it are folded into one word with no branch
     * per cell, in a loop the compiler widens, and a cell-by-cell step is
     * taken only where a run may end. A row of real output holds few runs,
     * often one, so most of a row goes by eight at a time. Same boundaries and
     * the same limit on a run's length (attr_run_eq) as the plain walk, so the
     * same line.
     *
     * Common rows keep their run boundaries for the fill rather than
     * rediscovering them; fixed stack storage bounds scratch memory for
     * arbitrarily wide rows, and high-run-count input keeps the two-pass
     * fallback below. */
    enum { CACHED_RUNS = 64, RUN_STEP = 8 };
    const u16 run_mask = (u16)~(u16)(ATTR_WDUMMY | ATTR_WIDE);
    u16 cached[CACHED_RUNS];
    CellAttr cur = cells[0].attr;
    u16 run_len = 1;
    u16 run_count = 0;
    u16 flags_seen = cells[0].attr.flags;

    for (i32 i = 1; i < len;) {
        if (len - i >= RUN_STEP && run_len <= UINT16_MAX - RUN_STEP) {
            /* Which of the eight differ from the run, as bits: none, and the
             * run takes all eight; otherwise it takes those before the first
             * that differs, which then starts the next run below — a step
             * never examines a cell for nothing, however short the runs. */
            u32 differ = 0;
            u16 seen = 0;
            for (i32 k = 0; k < RUN_STEP; k++) {
                const CellAttr a = cells[i + k].attr;
                u32 d = (a.fg ^ cur.fg) | (a.bg ^ cur.bg) | (u32)((u16)(a.flags ^ cur.flags) & run_mask);
                differ |= (u32)(d != 0) << k;
                seen |= a.flags;
            }
            flags_seen |= seen;
            if (differ == 0) {
                run_len += RUN_STEP;
                i += RUN_STEP;
                continue;
            }
            i32 same = __builtin_ctz(differ);
            run_len = (u16)(run_len + same);
            i += same;
        }
        flags_seen |= cells[i].attr.flags;
        if (attr_run_eq(cells[i].attr, cur) && run_len < UINT16_MAX) {
            run_len++;
            i++;
            continue;
        }
        if (run_count < CACHED_RUNS)
            cached[run_count] = run_len;
        run_count++;
        cur = cells[i].attr;
        run_len = 1;
        i++;
    }
    if (run_count > 0 && run_count < CACHED_RUNS)
        cached[run_count] = run_len;
    run_count++;

    /* Single contiguous block (struct + codepoints + runs + wide_bits, and
     * the lead words only when a cell is a wide character's left half). */
    bool leads = (flags_seen & ATTR_WIDE) != 0;
    if (run_count <= CACHED_RUNS) {
        /* Every codepoint and every run is written below, so only the header
         * and the bitmaps are zeroed. */
        TermLineCompressed *out = termline_alloc_block_impl(len, run_count, wrapped, leads, false, cache);
        if (!out) return false;
        if (run_count == 1) out->runs[0] = attr_run_make(cells[0].attr, (u16)len);
        else {
            i32 col = 0;
            for (u16 r = 0; r < run_count; r++) {
                out->runs[r] = attr_run_make(cells[col].attr, cached[r]);
                col += cached[r];
            }
        }
        /* The codepoints alone, as a plain copy the compiler can widen; the
         * bitmaps only for a row that has a wide cell, so a row of ASCII
         * pays no bit test per cell. */
        for (i32 i = 0; i < len; i++) out->codepoints[i] = cells[i].codepoint;
        if (flags_seen & (ATTR_WDUMMY | ATTR_WIDE)) termline_set_wide_bits(out, cells, len);
        *out_ptr = out;
        return true;
    }
    TermLineCompressed *out = termline_alloc_block_impl(len, run_count, wrapped, leads, true, cache);
    if (!out) return false;
    u64 *lead_bits = leads ? out->wide_bits + (len + 63) / 64 : NULL;
    out->run_count = 0; /* the fill loop below uses run_count++ as write index */

    /* Pass 2: codepoints, the wide bitmap and the runs, in ONE walk.
     *
     * A separate codepoint loop would read every cell once more and re-derive
     * exactly the run boundaries pass 1 has just found. Merged, the two writes
     * go to different arrays in the same block and the row is walked once
     * here. */
    cur = cells[0].attr;
    run_len = 1;
    out->codepoints[0] = cells[0].codepoint;
    if (cells[0].attr.flags & ATTR_WDUMMY) ws_set_bit(out->wide_bits, 0);
    if (cells[0].attr.flags & ATTR_WIDE) ws_set_bit(lead_bits, 0);

    for (i32 i = 1; i < len; i++) {
        out->codepoints[i] = cells[i].codepoint;
        if (cells[i].attr.flags & ATTR_WDUMMY) ws_set_bit(out->wide_bits, i);
        if (cells[i].attr.flags & ATTR_WIDE) ws_set_bit(lead_bits, i);

        if (attr_run_eq(cells[i].attr, cur) && run_len < UINT16_MAX) {
            run_len++;
            continue;
        }
        out->runs[out->run_count++] = attr_run_make(cur, run_len);
        cur = cells[i].attr;
        run_len = 1;
    }
    out->runs[out->run_count++] = attr_run_make(cur, run_len);

    *out_ptr = out;
    return true;
}

i32 termline_materialize(const TermLineCompressed *line,
                          Cell *out, i32 out_cap) {
    if (!line || !out || out_cap <= 0) return 0;
    i32 n = line->len < out_cap ? line->len : out_cap;
    if (n <= 0) return 0;

    /* Walk the run array in lockstep with the codepoints. The walk never
     * overshoots `n` because run lengths sum to line->len by construction.
     */
    const u64 *leads = termline_lead_bits(line);
    i32 col = 0;
    for (u16 r = 0; r < line->run_count && col < n; r++) {
        const AttrRun *run = &line->runs[r];
        i32 end = col + (i32)run->length;
        if (end > n) end = n;
        for (i32 i = col; i < end; i++) {
            out[i].codepoint = line->codepoints[i];
            out[i].attr      = run->attr;
            if (ws_get_bit(line->wide_bits, i))
                out[i].attr.flags = (u16)(out[i].attr.flags | ATTR_WDUMMY);
            if (ws_get_bit(leads, i))
                out[i].attr.flags = (u16)(out[i].attr.flags | ATTR_WIDE);
        }
        col = end;
    }
    /* Defensive: zero any tail cells that no run covered (shouldn't happen
     * for well-formed lines but the caller is reading them either way). */
    for (i32 i = col; i < n; i++) {
        out[i].codepoint = ' ';
        out[i].attr = (CellAttr){ .fg = FG_DEFAULT, .bg = BG_DEFAULT, .flags = 0 };
    }
    return n;
}

i32 termline_content_length(const TermLineCompressed *line) {
    if (!line || line->len <= 0) return 0;
    if (line->content_valid && line->content_len >= 0 && line->content_len <= line->len)
        return line->content_len;
    /* A run can still carry the wide bits: a line read back from a sleep
     * snapshot written before they moved out of the runs. */
    const u64 *leads = termline_lead_bits(line);
    i32 col = 0, content = 0;
    for (u16 r = 0; r < line->run_count && col < line->len; r++) {
        const AttrRun *run = &line->runs[r];
        i32 end = col + MIN((i32)run->length, line->len - col);
        if (end == col) continue;
        if (run->attr.flags & (ATTR_WIDE | ATTR_WDUMMY)) content = end;
        else {
            for (i32 i = end; i > col; i--) {
                if (line->codepoints[i - 1] != ' ' || ws_get_bit(line->wide_bits, i - 1) ||
                    ws_get_bit(leads, i - 1)) {
                    content = i; break;
                }
            }
        }
        col = end;
    }
    /* Uncovered tail cells are blank in termline_materialize as well. */
    return content;
}

const u32 *termline_codepoints(const TermLineCompressed *line, i32 *out_len) {
    if (!line) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = line->len;
    return line->codepoints;
}

void termline_compressed_destroy(TermLineCompressed *line) {
    /* line is now a single allocation block */
    if (!line) return;
#ifdef LIU_SCROLLBACK_ALLOC_PROBE
    usize requested = termline_compressed_bytes(line);
    termline_alloc_probe_record_free(line, requested);
#else
    free(line);
#endif
}

usize termline_compressed_bytes(const TermLineCompressed *line) {
    if (!line) return 0;
    if (line->len == 0) return sizeof(TermLineCompressed);
    usize total_size;
    return termline_block_layout(line->len, line->run_cap, line->has_leads, NULL, NULL,
                                 &total_size) ? total_size : 0;
}

/* Mark a scroll in the per-row dirty bitmap.
 *
 * A scroll changes the content of EVERY row in the region at once — the window
 * moves under them — while the parser only ever marks the row it writes a cell
 * into. ui.c's TermRenderCache rebuilds a row solely when term_row_dirty() says
 * so and replays its cached GPU instances otherwise, and the bits are cleared
 * after every render. So a scroll that marks nothing leaves the cache replaying
 * the pre-scroll picture: the screen freezes with only the cursor's row alive,
 * silently, with no crash and no log.
 *
 * Rows OUTSIDE a partial region did not move, so the region is enough — that is
 * the case a TUI's status line lives in. The full-screen case takes the memset
 * instead of the loop because it is cheaper. A viewport scrolled into history
 * has every visible row change (the scrollback slides under it, and this
 * function's caller advances scroll_offset), so it always takes ALL. */
static inline void scroll_mark_dirty(Terminal *t, i32 top, i32 bot) {
    if (t->scroll_offset > 0 || (top <= 0 && bot >= t->rows - 1)) {
        TERM_DIRTY_ALL(t);
        return;
    }
    for (i32 y = top; y <= bot; y++) TERM_DIRTY_ROW(t, y);
}

/* Scroll the scroll region up by n lines (new blank lines at bottom) */
void buffer_scroll_up(Terminal *t, i32 n) {
    if (n <= 0) return;
    i32 top = t->scroll_top;
    i32 bot = t->scroll_bottom;
    i32 region_height = bot - top + 1;
    if (n > region_height) n = region_height;
    /* Underline style/colour is side data: it moves with the rows or it ends
     * up describing other cells. */
    if (t->ul_map_count > 0) terminal_underline_scroll(t, top, bot, n);

    /* Save scrolled-out lines to scrollback — only on the PRIMARY screen, and only when the
     * region's TOP is the top of the screen. The alt screen (vim/htop/Claude-Code) is a transient
     * framebuffer that must never feed the scrollback; pushing its rows there would leak
     * full-screen TUI content into history and make it reappear (duplicated, wrong place) on the
     * next reflow/scrollback view.
     *
     * The bottom margin does NOT have to be the last row: only `top == 0` is required, and codex
     * depends on it: its inline viewport inserts transcript lines with `CSI 1;Nr` + CRLF, i.e. a
     * region anchored at the top that stops above the live viewport. Requiring a full-screen region
     * would throw every one of those lines away and leave a codex session with no scrollback. A
     * region that starts below the top still adds nothing: the row leaving it lands on the rows
     * above, not off the screen. */
    if (!(t->mode & MODE_ALT_SCREEN) && top == 0) {
        for (i32 i = 0; i < n; i++) {
            bool is_wrapped = t->line_wrapped ? t->line_wrapped[i] : false;
            sb_push(t, &t->cells[i * t->cols], t->cols, is_wrapped);
        }
        /* If the user has scrolled up into history, advance scroll_offset by the
         * lines just pushed so their view stays anchored on the same content
         * instead of drifting (or being yanked) as new output streams in. */
        if (t->scroll_offset > 0) {
            t->scroll_offset += n;
            if (t->scroll_offset > t->sb_count) t->scroll_offset = t->sb_count;
        }
    }

    /* Move lines up.
     *
     * When the whole screen scrolls — which is what a stream of output does,
     * and the only case that runs per line of a build log — the rows do not
     * have to move at all: advance the window into the slack at the end of the
     * allocation instead, and pay the copy once every grid_slack scrolls.
     *
     * Both screens qualify. The slide state travels with its buffer
     * (terminal_grid_swap_alt), so the alt grid arms its own slack on first
     * scroll exactly like the primary, and pagers and TUIs take the same path
     * as plain output.
     *
     * A scroll REGION can slide too, even though advancing the window moves the
     * rows outside it that were supposed to stay put: slide anyway, then put
     * those rows back. That trades `move_count` rows of copying for
     * `above + below`, which is why it is taken only when the fix-up is the
     * smaller of the two — a TUI with a one-line status bar scrolls 88 rows and
     * restores 1, while a half-screen region would gain nothing. A full-screen
     * program with a status line scrolls through here rather than through the
     * whole-screen case. */
    i32 move_count = region_height - n;
    i32 above = top;                    /* rows before the region */
    i32 below = t->rows - 1 - bot;      /* rows after it          */
    bool whole_screen = (above == 0 && below == 0);
    bool region_slide = move_count > 0 && !whole_screen &&
                        (above + below < move_count) && grid_ensure_slack(t);
    if (region_slide && t->grid_slide + n > t->grid_slack) {
        /* Out of slack: fold the window back to the base and try again from
         * zero. Unlike the whole-screen case below, the fold cannot be merged
         * with the scroll — the rows outside the region have to survive it. */
        terminal_grid_unslide(t);
        region_slide = (n <= t->grid_slack);
    }
    if (move_count > 0) {
        if (whole_screen && grid_ensure_slack(t)) {
            if (t->grid_slide + n <= t->grid_slack) {
                t->cells += (usize)n * (usize)t->cols;
                t->grid_slide += n;
            } else {
                /* Slack exhausted: fold the live rows back to the base. This
                 * copy runs once per slack instead of once per line. */
                memmove(t->grid_base, &t->cells[n * t->cols],
                        (usize)move_count * (usize)t->cols * sizeof(Cell));
                t->cells = t->grid_base;
                t->grid_slide = 0;
            }
        } else if (region_slide) {
            /* The window advances, so row y now shows what row y+n held — the
             * scroll, for free. Rows outside the region have to be dragged
             * forward by the same n to end up unchanged; both copies read from
             * inside the allocation because grid_slide >= n once advanced, and
             * bot + 1 > n follows from move_count > 0. */
            t->cells      += (usize)n * (usize)t->cols;
            t->grid_slide += n;
            if (above > 0)
                memmove(&t->cells[0], t->cells - (usize)n * (usize)t->cols,
                        (usize)above * (usize)t->cols * sizeof(Cell));
            if (below > 0)
                memmove(&t->cells[(bot + 1) * t->cols],
                        &t->cells[(bot + 1 - n) * t->cols],
                        (usize)below * (usize)t->cols * sizeof(Cell));
        } else {
#if HAVE_TERM_ASM
            asm_buffer_scroll_up(&t->cells[top * t->cols], region_height, t->cols, n);
#else
            memmove(&t->cells[top * t->cols],
                    &t->cells[(top + n) * t->cols],
                    (usize)(move_count * t->cols) * sizeof(Cell));
#endif
        }
    }

    /* The rows a prompt, its input and its output began on move with the
     * text. Once the prompt is on the last row — the state of any pane after
     * one screenful — the newline that ends a command scrolls the command
     * away from under `input_start_row`, and an unmoved row would make the
     * capture at OSC 133;C read the empty row below it. A row that leaves the
     * top goes negative: "above the screen", which the readers refuse. */
    if (t->input_start_row >= top && t->input_start_row <= bot) t->input_start_row -= n;
    if (t->prompt_start_row >= top && t->prompt_start_row <= bot) {
        t->prompt_start_row -= n;
        if (t->prompt_start_row < top) t->prompt_start_row = -1;
    }
    if (t->output_start_row >= top && t->output_start_row <= bot) {
        t->output_start_row -= n;
        if (t->output_start_row < top) t->output_start_row = -1;
    }

    /* Shift per-row semantic zones */
    if (t->row_zones) {
        if (move_count > 0)
            memmove(&t->row_zones[top], &t->row_zones[top + n],
                    (usize)move_count * sizeof(SemanticZone));
        for (i32 y = bot - n + 1; y <= bot; y++)
            t->row_zones[y] = t->current_zone;
    }
    if (t->line_wrapped) {
        if (move_count > 0)
            memmove(&t->line_wrapped[top], &t->line_wrapped[top + n],
                    (usize)move_count * sizeof(bool));
        for (i32 y = bot - n + 1; y <= bot; y++)
            t->line_wrapped[y] = false;
    }

    /* Clear new lines at bottom */
    Cell blank = default_cell();
    blank.attr = t->cursor_attr;
    blank.attr.flags = 0;
    for (i32 y = bot - n + 1; y <= bot; y++) {
#if HAVE_TERM_ASM
        asm_buffer_clear_row(&t->cells[y * t->cols], t->cols,
                             blank.codepoint, blank.attr.fg, blank.attr.bg);
#else
        for (i32 x = 0; x < t->cols; x++) {
            t->cells[y * t->cols + x] = blank;
        }
#endif
    }

    scroll_mark_dirty(t, top, bot);
}

/* Scroll the scroll region down by n lines (new blank lines at top) */
void buffer_scroll_down(Terminal *t, i32 n) {
    if (n <= 0) return;
    i32 top = t->scroll_top;
    i32 bot = t->scroll_bottom;
    i32 region_height = bot - top + 1;
    if (n > region_height) n = region_height;
    if (t->ul_map_count > 0) terminal_underline_scroll(t, top, bot, -n);

    i32 move_count = region_height - n;
    if (move_count > 0) {
        memmove(&t->cells[(top + n) * t->cols],
                &t->cells[top * t->cols],
                (usize)(move_count * t->cols) * sizeof(Cell));
    }

    /* …and down with it (see buffer_scroll_up). A row pushed past the
     * region's bottom is gone. */
    if (t->input_start_row >= top && t->input_start_row <= bot) {
        t->input_start_row += n;
        if (t->input_start_row > bot) { t->input_start_row = -1; t->input_start_col = -1; }
    }
    if (t->prompt_start_row >= top && t->prompt_start_row <= bot) {
        t->prompt_start_row += n;
        if (t->prompt_start_row > bot) t->prompt_start_row = -1;
    }
    if (t->output_start_row >= top && t->output_start_row <= bot) {
        t->output_start_row += n;
        if (t->output_start_row > bot) t->output_start_row = -1;
    }

    /* Shift per-row semantic zones */
    if (t->row_zones) {
        if (move_count > 0)
            memmove(&t->row_zones[top + n], &t->row_zones[top],
                    (usize)move_count * sizeof(SemanticZone));
        for (i32 y = top; y < top + n; y++)
            t->row_zones[y] = ZONE_NONE;
    }
    if (t->line_wrapped) {
        if (move_count > 0)
            memmove(&t->line_wrapped[top + n], &t->line_wrapped[top],
                    (usize)move_count * sizeof(bool));
        for (i32 y = top; y < top + n; y++)
            t->line_wrapped[y] = false;
    }

    Cell blank = default_cell();
    blank.attr = t->cursor_attr;
    blank.attr.flags = 0;
    for (i32 y = top; y < top + n; y++) {
#if HAVE_TERM_ASM
        asm_buffer_clear_row(&t->cells[y * t->cols], t->cols,
                             blank.codepoint, blank.attr.fg, blank.attr.bg);
#else
        for (i32 x = 0; x < t->cols; x++) {
            t->cells[y * t->cols + x] = blank;
        }
#endif
    }

    scroll_mark_dirty(t, top, bot);
}
