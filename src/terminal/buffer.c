/*
 * Liu - terminal screen buffer operations
 * Ring buffer scrollback for O(1) eviction.
 */
#include "terminal/terminal.h"
#include "core/memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Hand-written cell ops: ARM64 NEON (Apple Silicon) or x86-64 SSE2. CMake sets
 * USE_ASM only when it linked the matching *_arm64.S / *_x86_64.S for this arch. */
#if defined(USE_ASM) && (defined(__aarch64__) || defined(__arm64__) || defined(__x86_64__) || defined(__amd64__))
#define HAVE_TERM_ASM 1
extern void asm_buffer_clear_row(void *row_start, int cols, u32 codepoint, u32 fg, u32 bg);
extern void asm_buffer_scroll_up(void *buffer, int rows, int cols, int n);
#else
#define HAVE_TERM_ASM 0
#endif

static Cell default_cell(void) {
    return (Cell){
        .codepoint = ' ',
        .attr = { .fg = FG_DEFAULT, .bg = BG_DEFAULT, .flags = 0 }
    };
}

static i32 scrollback_line_len(const Cell *row_cells, i32 len, bool wrapped) {
    if (wrapped) return len;
    while (len > 0) {
        const Cell *cell = &row_cells[len - 1];
        if ((cell->attr.flags & (ATTR_WIDE | ATTR_WDUMMY)) ||
            (cell->codepoint != 0 && cell->codepoint != ' ')) {
            break;
        }
        len--;
    }
    return len;
}

static inline usize scrollback_line_bytes(i32 cap) {
    if (cap <= 0) return 0;
    if ((usize)cap > SIZE_MAX / sizeof(Cell)) return SIZE_MAX;
    return (usize)cap * sizeof(Cell);
}

/* Bytes a single ring slot currently retains. Cold lines report their
 * compressed footprint; hot lines report the legacy Cell array bytes. */
static usize sb_line_retained_bytes(const TermLine *line) {
    if (!line) return 0;
    if (line->cold) return termline_compressed_bytes(line->cold);
    return scrollback_line_bytes(line->cap);
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

static void sb_evict_oldest(Terminal *t) {
    if (!t || t->sb_count <= 0 || !t->sb_ring) return;
    TermLine *oldest = &t->sb_ring[t->sb_head];
    usize old_bytes = sb_line_retained_bytes(oldest);
    sb_line_release(oldest);
    if (t->sb_byte_usage > old_bytes) t->sb_byte_usage -= old_bytes;
    else t->sb_byte_usage = 0;
    t->sb_head = (t->sb_head + 1) % t->sb_capacity;
    t->sb_count--;
    t->sb_abs_base++;   /* one line dropped from the front */
}

Cell *terminal_cell_at(Terminal *t, i32 col, i32 row) {
    if (col < 0 || col >= t->cols || row < 0 || row >= t->rows) return NULL;
    return &t->cells[row * t->cols + col];
}

/* Grow the per-Terminal scratch buffer to fit `cols` cells. Returns false on
 * alloc failure (caller treats as "no row"). */
static bool sb_scratch_reserve(Terminal *t, i32 cols) {
    if (cols <= 0) return false;
    if (t->sb_scratch && t->sb_scratch_cap >= cols) return true;
    i32 new_cap = t->sb_scratch_cap > 0 ? t->sb_scratch_cap : 64;
    while (new_cap < cols) new_cap *= 2;
    Cell *grown = realloc(t->sb_scratch, (usize)new_cap * sizeof(Cell));
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
    i32 logical = visible_row - t->scroll_offset;
    if (logical >= 0) {
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
        i32 n = termline_materialize(ln->cold, t->sb_scratch, t->sb_scratch_cap);
        if (out_len) *out_len = n;
        return t->sb_scratch;
    }

    if (out_len) *out_len = ln->len;
    return ln->cells;
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

/* Compress `row_cells` into a freshly heap-allocated TermLineCompressed.
 * Returns NULL on allocation failure. */
static TermLineCompressed *sb_make_cold(const Cell *row_cells, i32 len, bool wrapped) {
    TermLineCompressed *c = NULL;
    if (!termline_compress(row_cells, len, wrapped, &c)) {
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
    sb_line_release(slot);
    slot->cold    = cold;
    slot->len     = len;
    slot->cap     = 0;
    slot->cells   = NULL;
    slot->wrapped = wrapped;
    usize new_bytes = sb_line_retained_bytes(slot);
    if (t->sb_byte_usage > old_bytes) t->sb_byte_usage -= old_bytes;
    else t->sb_byte_usage = 0;
    t->sb_byte_usage += new_bytes;
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
           t->sb_byte_usage + incoming_bytes > t->sb_byte_budget) {
        sb_evict_oldest(t);
    }

    i32  len     = cold->len;
    bool wrapped = cold->wrapped;

    if (t->sb_count < t->sb_capacity) {
        i32 tail = (t->sb_head + t->sb_count) % t->sb_capacity;
        sb_slot_install_cold(t, &t->sb_ring[tail], cold, len, wrapped);
        t->sb_count++;
    } else if (t->sb_capacity < t->scrollback_limit) {
        i32 new_cap = t->sb_capacity * 2;
        if (new_cap > t->scrollback_limit) new_cap = t->scrollback_limit;
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
    if (!t->sb_ring || t->sb_capacity == 0) return;
    len = scrollback_line_len(row_cells, len, wrapped);
    TermLineCompressed *cold = sb_make_cold(row_cells, len, wrapped);
    if (!cold) return;
    sb_install_cold_line(t, cold);
}

/* Public sibling of sb_push that takes an already-compressed line. The
 * sleep-snapshot v2 reader uses this to install rows directly without paying
 * the compress pass. Takes ownership of `cold` (frees on overflow/error). */
void sb_push_cold(Terminal *t, TermLineCompressed *cold) {
    sb_install_cold_line(t, cold);
}

/* Get scrollback line by index (0 = oldest visible) */
TermLine *sb_get(Terminal *t, i32 index) {
    if (index < 0 || index >= t->sb_count) return NULL;
    i32 ring_idx = (t->sb_head + index) % t->sb_capacity;
    return &t->sb_ring[ring_idx];
}

/* =========================================================================
 * Phase 1 skeleton: TermLineCompressed helpers.
 *
 * These are exercised by direct callers (and a future commit will wire them
 * into sb_push/sb_get). For now `sb_push` still stores the legacy `TermLine`
 * cell array, so behaviour is unchanged — the helpers below exist so the
 * call-site migration in commit 2 stays a one-file change.
 * ========================================================================= */

static bool ws_set_bit(u64 *bits, i32 i) {
    if (!bits) return false;
    bits[i >> 6] |= (1ULL << (i & 63));
    return true;
}

static inline bool ws_get_bit(const u64 *bits, i32 i) {
    return bits && ((bits[i >> 6] >> (i & 63)) & 1ULL);
}

TermLineCompressed *termline_alloc_block(i32 len, u16 run_count, bool wrapped) {
    if (len < 0) return NULL;
    if (len == 0) {
        TermLineCompressed *empty = calloc(1, sizeof(TermLineCompressed));
        if (empty) empty->wrapped = wrapped;
        return empty;
    }

    usize struct_size = sizeof(TermLineCompressed);
    usize cp_offset   = struct_size;
    usize cp_size     = (usize)len * sizeof(u32);
    usize runs_offset = (cp_offset + cp_size + 15) & ~15ULL;
    usize runs_size   = (usize)run_count * sizeof(AttrRun);
    usize wb_words    = (usize)((len + 63) / 64);
    usize wb_offset   = (runs_offset + runs_size + 7) & ~7ULL;
    usize wb_size     = wb_words ? wb_words * sizeof(u64) : sizeof(u64);
    usize total_size  = wb_offset + wb_size;

    u8 *block = malloc(total_size);
    if (!block) return NULL;
    memset(block, 0, total_size);

    TermLineCompressed *out = (TermLineCompressed *)block;
    out->codepoints = (u32 *)(block + cp_offset);
    out->runs       = (AttrRun *)(block + runs_offset);
    out->wide_bits  = (u64 *)(block + wb_offset);
    out->len        = len;
    out->cap        = len;
    out->run_count  = run_count;
    out->run_cap    = run_count;
    out->wrapped    = wrapped;
    return out;
}

bool termline_compress(const Cell *cells, i32 len, bool wrapped,
                       TermLineCompressed **out_ptr) {
    if (!out_ptr) return false;
    *out_ptr = NULL;

    if (len < 0) len = 0;

    /* Empty line: return a minimal block. */
    if (len == 0 || !cells) {
        TermLineCompressed *empty = calloc(1, sizeof(TermLineCompressed));
        if (!empty) return false;
        empty->wrapped = wrapped;
        *out_ptr = empty;
        return true;
    }

    /* RLE pass: First pass to count runs to calculate exact block size */
    CellAttr cur = cells[0].attr;
    cur.flags = (u16)(cur.flags & ~(u16)ATTR_WDUMMY);
    u16 run_len = 1;
    u16 run_count = 0;

    for (i32 i = 1; i < len; i++) {
        CellAttr a = cells[i].attr;
        a.flags = (u16)(a.flags & ~(u16)ATTR_WDUMMY);
        if (memcmp(&a, &cur, sizeof(CellAttr)) == 0 && run_len < UINT16_MAX) {
            run_len++;
            continue;
        }
        run_count++;
        cur = a;
        run_len = 1;
    }
    run_count++; /* Flush final run */

    /* Single contiguous block (struct + codepoints + runs + wide_bits). */
    TermLineCompressed *out = termline_alloc_block(len, run_count, wrapped);
    if (!out) return false;
    out->run_count = 0; /* the fill loop below uses run_count++ as write index */

    /* Fill codepoints and wide_bits */
    for (i32 i = 0; i < len; i++) {
        out->codepoints[i] = cells[i].codepoint;
        if (cells[i].attr.flags & ATTR_WDUMMY) ws_set_bit(out->wide_bits, i);
    }

    /* Fill RLE runs */
    cur = cells[0].attr;
    cur.flags = (u16)(cur.flags & ~(u16)ATTR_WDUMMY);
    run_len = 1;

    for (i32 i = 1; i < len; i++) {
        CellAttr a = cells[i].attr;
        a.flags = (u16)(a.flags & ~(u16)ATTR_WDUMMY);
        if (memcmp(&a, &cur, sizeof(CellAttr)) == 0 && run_len < UINT16_MAX) {
            run_len++;
            continue;
        }
        out->runs[out->run_count++] = (AttrRun){ .attr = cur, .length = run_len, ._pad = 0 };
        cur = a;
        run_len = 1;
    }
    out->runs[out->run_count++] = (AttrRun){ .attr = cur, .length = run_len, ._pad = 0 };

    *out_ptr = out;
    return true;
}

i32 termline_materialize(const TermLineCompressed *line,
                          Cell *out, i32 out_cap) {
    if (!line || !out || out_cap <= 0) return 0;
    i32 n = line->len < out_cap ? line->len : out_cap;
    if (n <= 0) return 0;

    /* Walk the run array in lockstep with the codepoints. We never overshoot
     * `n` because run lengths sum to line->len by construction. */
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

const u32 *termline_codepoints(const TermLineCompressed *line, i32 *out_len) {
    if (!line) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = line->len;
    return line->codepoints;
}

void termline_compressed_destroy(TermLineCompressed *line) {
    /* line is now a single allocation block */
    if (!line) return;
    free(line);
}

usize termline_compressed_bytes(const TermLineCompressed *line) {
    if (!line) return 0;
    if (line->len == 0) return sizeof(TermLineCompressed);
    
    usize struct_size = sizeof(TermLineCompressed);
    usize cp_offset = struct_size;
    usize cp_size = (usize)line->len * sizeof(u32);
    usize runs_offset = (cp_offset + cp_size + 15) & ~15ULL;
    usize runs_size = (usize)line->run_cap * sizeof(AttrRun);
    usize wb_words = (usize)((line->len + 63) / 64);
    usize wb_offset = (runs_offset + runs_size + 7) & ~7ULL;
    usize wb_size = wb_words ? wb_words * sizeof(u64) : sizeof(u64);

    return wb_offset + wb_size;
}

/* Scroll the scroll region up by n lines (new blank lines at bottom) */
void buffer_scroll_up(Terminal *t, i32 n) {
    if (n <= 0) return;
    i32 top = t->scroll_top;
    i32 bot = t->scroll_bottom;
    i32 region_height = bot - top + 1;
    if (n > region_height) n = region_height;

    /* Save scrolled-out lines to scrollback — only on the PRIMARY screen with a
     * full-screen scroll region. The alt screen (vim/htop/Claude-Code)
     * is a transient framebuffer that must never feed the scrollback; pushing
     * its rows there leaked full-screen TUI content into history and made it
     * reappear (duplicated, wrong place) on the next reflow/scrollback view. */
    if (!(t->mode & MODE_ALT_SCREEN) && top == 0 && bot == t->rows - 1) {
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

    /* Move lines up */
    i32 move_count = region_height - n;
    if (move_count > 0) {
#if HAVE_TERM_ASM
        asm_buffer_scroll_up(&t->cells[top * t->cols], region_height, t->cols, n);
#else
        memmove(&t->cells[top * t->cols],
                &t->cells[(top + n) * t->cols],
                (usize)(move_count * t->cols) * sizeof(Cell));
#endif
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
}

/* Scroll the scroll region down by n lines (new blank lines at top) */
void buffer_scroll_down(Terminal *t, i32 n) {
    if (n <= 0) return;
    i32 top = t->scroll_top;
    i32 bot = t->scroll_bottom;
    i32 region_height = bot - top + 1;
    if (n > region_height) n = region_height;

    i32 move_count = region_height - n;
    if (move_count > 0) {
        memmove(&t->cells[(top + n) * t->cols],
                &t->cells[top * t->cols],
                (usize)(move_count * t->cols) * sizeof(Cell));
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
}
