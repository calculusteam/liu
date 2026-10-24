/*
 * Liu - terminal state management + key input encoding
 */
#include "terminal/terminal.h"
#include "terminal/palette256.h"
#include "core/checked.h"
#include "core/memory.h"
#include "core/utf8.h"
#include "core/mem_budget.h"
#include "terminal/row_stats.h"
#include <stdatomic.h>

/* The one instance for the process; see terminal/row_stats.h. */
#ifdef LIU_ROW_STATS
LiuRowStats g_liu_row_stats;
bool        g_liu_row_timing_on = false;
#endif
#include "platform/platform.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define TERM_SCROLLBACK_HARD_MAX_LINES 20000
#define TERM_MAX_DIMENSION 32767
#define TERM_UNDERLINE_MAP_MAX_CAP (1 << 20)
#define TERM_SCROLLBACK_MIN_BUDGET_BYTES ((usize)(512u * 1024u))
/* Per-terminal scrollback cap. With 32 tabs * 2 split panes the worst-case
 * footprint is 32 * 2 * cap; lowered from 64 MiB so even a maxed-out window
 * stays under ~1 GiB. */
#define TERM_SCROLLBACK_MAX_BUDGET_BYTES ((usize)(16u * 1024u * 1024u))

/* Cell offsets and several VT/reflow counters are i32. Reject impossible UI
 * geometries at the boundary instead of letting later cols*rows expressions
 * overflow signed arithmetic. 32767 is far above a realizable pane size and
 * also keeps screen + maximum scrollback reflow counts representable. */
static bool terminal_grid_cell_count(i32 cols, i32 rows, usize *out) {
    if (cols < 1 || rows < 1 ||
        cols > TERM_MAX_DIMENSION || rows > TERM_MAX_DIMENSION) {
        return false;
    }

    usize count;
    if (!checked_mul_usize((usize)cols, (usize)rows, &count) ||
        count > (usize)INT32_MAX) {
        return false;
    }
    if (out) *out = count;
    return true;
}

static inline i32 content_len_trimmed(const Cell *cells, i32 len) {
    if (!cells || len <= 0) return 0;
    /* Same rule as the scrollback's stored length (buffer.c): a blank that can
     * be seen — background, inverse, a decoration — is content. */
    while (len > 0 && cells[len - 1].codepoint == ' ' &&
           !(cells[len - 1].attr.flags & (ATTR_WIDE | ATTR_WDUMMY)) &&
           term_cell_attr_plain(&cells[len - 1]))
        len--;
    return len;
}

static inline usize scrollback_line_bytes(i32 cap) {
    if (cap <= 0) return 0;
    if ((usize)cap > SIZE_MAX / sizeof(Cell)) return SIZE_MAX;
    return (usize)cap * sizeof(Cell);
}

static usize scrollback_budget_for(i32 cols, i32 limit) {
    if (limit <= 0 || cols <= 0) return 0;

    usize bytes = SIZE_MAX;
    if ((usize)limit <= SIZE_MAX / (usize)cols) {
        usize cells = (usize)limit * (usize)cols;
        if (cells <= SIZE_MAX / sizeof(Cell))
            bytes = cells * sizeof(Cell);
    }

    if (bytes < TERM_SCROLLBACK_MIN_BUDGET_BYTES) bytes = TERM_SCROLLBACK_MIN_BUDGET_BYTES;
    if (bytes > TERM_SCROLLBACK_MAX_BUDGET_BYTES) bytes = TERM_SCROLLBACK_MAX_BUDGET_BYTES;
    return bytes;
}

/* What one terminal actually retains, split the way the ledger classes are.
 *
 * `sb_byte_usage` is only the compressed line PAYLOAD. Two more allocations
 * belong to the terminal itself —
 *
 *   the RING, sb_capacity * sizeof(TermLine), which is scrollback storage by
 *   any reading and belongs in the same class; and
 *
 *   the live grid, the parked alt grid and the per-row side tables, which are
 *   not history at all — hence MEM_TERMINAL rather than inflating a class
 *   whose name says "compressed terminal history".
 *
 * Both scale with the pane count, and Liu allows 8 windows x 32 tabs x 8 panes.
 * What is still not counted is per-allocation malloc overhead (every
 * compressed line is its own block), because no class in this ledger counts
 * it and one that did would be alone.
 *
 * Both grids include their scroll SLACK: `cells` is a window into grid_base
 * when grid_slack > 0, so rows*cols understates the block it points into. */
usize terminal_scrollback_bytes(const Terminal *t) {
    if (!t) return 0;
    usize bytes = t->sb_byte_usage + termline_cache_bytes(t);
    if (t->sb_ring && t->sb_capacity > 0)
        bytes += (usize)t->sb_capacity * sizeof(TermLine);
    return bytes;
}

usize terminal_grid_bytes(const Terminal *t) {
    if (!t || t->cols <= 0) return 0;
    usize cols = (usize)t->cols;
    usize bytes = 0;
    if (t->cells)
        bytes += ((usize)t->rows + (usize)t->grid_slack) * cols * sizeof(Cell);
    if (t->alt_cells)
        bytes += ((usize)t->rows + (usize)t->alt_slack) * cols * sizeof(Cell);
    if (t->tab_stops)    bytes += cols * sizeof(bool);
    if (t->dirty_rows)   bytes += (usize)t->dirty_words * sizeof(u64);
    if (t->line_wrapped) bytes += (usize)t->rows * sizeof(bool);
    if (t->row_zones)    bytes += (usize)t->rows * sizeof(SemanticZone);
    if (t->alt_line_wrapped) bytes += (usize)t->rows * sizeof(bool);
    if (t->alt_row_zones)    bytes += (usize)t->rows * sizeof(SemanticZone);
    if (t->ul_map)       bytes += (usize)t->ul_map_cap * sizeof(UnderlineEntry);
    if (t->ul_alt_map)   bytes += (usize)t->ul_alt_cap * sizeof(UnderlineEntry);
    if (t->palette)      bytes += 256u * sizeof(u32);
    if (t->sb_scratch)   bytes += (usize)t->sb_scratch_cap * sizeof(Cell);
    /* The synchronized-output picture: a second copy of the grid and its
     * side tables, kept between BSU/ESU pairs. Terminal-owned CPU memory by
     * any reading, and up to 16 MiB a pane, so it belongs here. */
    bytes += terminal_sync_arena_bytes(t);
    bytes += terminal_search_bytes(t);
    return bytes;
}

void terminal_refresh_scrollback_usage(Terminal *t) {
    if (!t) return;
    t->sb_byte_usage = 0;
    if (!t->sb_ring || t->sb_count <= 0) return;
    for (i32 i = 0; i < t->sb_count; i++) {
        i32 idx = (t->sb_head + i) % t->sb_capacity;
        const TermLine *ln = &t->sb_ring[idx];
        if (ln->cold)       t->sb_byte_usage += termline_compressed_bytes(ln->cold);
        else                t->sb_byte_usage += scrollback_line_bytes(ln->cap);
    }
}

static void terminal_free_scrollback(Terminal *t) {
    termline_cache_free(t);
    if (!t || !t->sb_ring) return;
    for (i32 i = 0; i < t->sb_count; i++) {
        i32 idx = (t->sb_head + i) % t->sb_capacity;
        TermLine *ln = &t->sb_ring[idx];
        if (ln->cold) {
            /* termline_compressed_destroy already free()s the block — calling
             * free(ln->cold) afterwards is a double-free that aborts on
             * libmalloc's guard pages. */
            termline_compressed_destroy(ln->cold);
            ln->cold = NULL;
        }
        free(ln->cells);
        ln->cells = NULL;
        ln->len = 0;
        ln->cap = 0;
        ln->wrapped = false;
    }
    free(t->sb_ring);
    t->sb_ring = NULL;
    t->sb_capacity = 0;
    t->sb_count = 0;
    t->sb_head = 0;
    t->sb_byte_usage = 0;
    t->sb_byte_budget = 0;
    t->scroll_offset = 0;
}

static i32 scrollback_target_capacity(i32 limit, i32 count) {
    if (limit <= 0) return 0;
    if (count < 1) count = 1;

    i32 cap = limit < 64 ? limit : 64;
    while (cap < count && cap < limit) {
        if (cap > limit / 2) {
            cap = limit;
            break;
        }
        cap *= 2;
    }
    if (cap < count) cap = count;
    if (cap > limit) cap = limit;
    return cap;
}

void terminal_set_scrollback_limit(Terminal *t, i32 limit) {
    if (!t) return;

    if (limit < 0) limit = 0;
    if (limit > TERM_SCROLLBACK_HARD_MAX_LINES) limit = TERM_SCROLLBACK_HARD_MAX_LINES;
    usize new_budget = scrollback_budget_for(t->cols, limit);
    if (limit == t->scrollback_limit &&
        (limit == 0 || t->sb_capacity > 0) &&
        new_budget == t->sb_byte_budget)
        return;

    t->scrollback_limit = limit;
    t->sb_byte_budget = new_budget;
    if (limit == 0) {
        /* The lines are dropped from the front, all of them: the origin moves
         * past them so what stays on screen keeps its absolute numbers. A full
         * RESET is a different thing and keeps its own identity policy. */
        if (t->sb_ring && t->sb_count > 0) t->sb_abs_base += (i64)t->sb_count;
        terminal_free_scrollback(t);
        TERM_DIRTY_ALL(t);
        return;
    }

    terminal_refresh_scrollback_usage(t);
    while (t->sb_ring && t->sb_count > 0 &&
           (t->sb_count > limit ||
            (t->sb_byte_budget > 0 && t->sb_byte_usage > t->sb_byte_budget))) {
        sb_evict_oldest(t);
    }
    if (t->scroll_offset > t->sb_count) t->scroll_offset = t->sb_count;

    i32 target_cap = scrollback_target_capacity(limit, t->sb_count);
    if (target_cap != t->sb_capacity) {
        TermLine *new_ring = NULL;
        if (target_cap > 0) {
            new_ring = calloc((usize)target_cap, sizeof(TermLine));
        }

        if (target_cap == 0 || new_ring) {
            for (i32 i = 0; i < t->sb_count && new_ring; i++) {
                new_ring[i] = t->sb_ring[(t->sb_head + i) % t->sb_capacity];
            }
            free(t->sb_ring);
            t->sb_ring = new_ring;
            t->sb_capacity = target_cap;
            t->sb_head = 0;
        }
    }

    TERM_DIRTY_ALL(t);
}

/* A synchronized pane owns a small CPU picture, independent of font-atlas
 * placement. Pixel payloads are immutable and retained only if their live slot
 * is deleted while the picture is displayed; never copy megabytes at BSU. */
typedef struct TermSyncSnapshot {
    Terminal view;
    Arena arena;
    bool ready;
    bool retired[MAX_TERM_IMAGES];
    usize retired_bytes;
} TermSyncSnapshot;

/* The picture's arena is kept across BSU/ESU pairs on purpose — a TUI that
 * brackets every repaint would otherwise pay an mmap per frame — but an arena
 * that is only ever GROWN leaves a pane that once ran at 800x400 and then
 * shrank to 80x24 carrying megabytes it cannot use, for the life of the
 * process. Two points give it back safely, both with `ready` false — i.e. no
 * render is reading `view` and nothing else ever borrows the arena:
 *   - a capture that finds the arena at least twice what it needs (and the
 *     excess worth a page walk) replaces it with a right-sized one;
 *   - terminal_sync_trim(), from a resize and from the memory-pressure
 *     reclaimer, drops it outright — the next capture allocates for the new
 *     geometry, which it had to size anyway. */
#define SYNC_ARENA_SHRINK_SLACK KB(256)

usize terminal_sync_trim(Terminal *t) {
    TermSyncSnapshot *s = t ? t->sync_snapshot : NULL;
    if (!s || s->ready) return 0;
    usize freed = s->arena.size;
    arena_destroy(&s->arena);
    return freed;
}

usize terminal_sync_arena_bytes(const Terminal *t) {
    const TermSyncSnapshot *s = t ? t->sync_snapshot : NULL;
    return s ? s->arena.size : 0;
}

void terminal_sync_release(Terminal *t) {
    TermSyncSnapshot *s = t ? t->sync_snapshot : NULL;
    if (!s || !s->ready) return;
    for (i32 i = 0; i < MAX_TERM_IMAGES; i++) {
        if (s->retired[i]) free(s->view.images[i].pixels);
        s->retired[i] = false;
    }
    if (s->retired_bytes)
        mem_budget_adjust(MEM_IMAGE_CPU, -(isize)s->retired_bytes);
    s->retired_bytes = 0;
    s->ready = false;
    /* Live dirty rows and palette changes were never consumed while drawing
     * the picture. They describe exactly what must be repainted after ESU. */
}

static void terminal_image_free_or_retain(Terminal *t, TermImage *img) {
    TermSyncSnapshot *s = t->sync_snapshot;
    if (s && s->ready && s->view.images && img->pixels) {
        for (i32 i = 0; i < MAX_TERM_IMAGES; i++) {
            if (s->view.images[i].pixels != img->pixels) continue;
            if (!s->retired[i]) {
                usize bytes = (usize)img->width * (usize)img->height * 4u;
                s->retired[i] = true;
                s->retired_bytes += bytes;
                /* The caller removes the live slot from the ledger. */
                mem_budget_adjust(MEM_IMAGE_CPU, (isize)bytes);
            }
            return;
        }
    }
    free(img->pixels);
}

bool terminal_sync_capture(Terminal *t) {
    if (!t || t->sync_view) return false;
    if (t->sync_pending) return t->sync_snapshot && t->sync_snapshot->ready;
    terminal_sync_release(t);
    if (!t->sync_snapshot) t->sync_snapshot = calloc(1, sizeof(TermSyncSnapshot));
    TermSyncSnapshot *s = t->sync_snapshot;
    if (!s) return false;
    usize count;
    if (!terminal_grid_cell_count(t->cols, t->rows, &count)) return false;
    /* All dimensions have terminal-owned bounds. Refuse oversized pictures
     * instead of doubling an implausibly large grid on the UI thread. */
    usize bytes = count * sizeof(Cell) + (usize)t->dirty_words * sizeof(u64)
        + (usize)t->rows * (sizeof(bool) + sizeof(SemanticZone) + sizeof(i32))
        + (usize)t->ul_map_cap * sizeof(UnderlineEntry)
        + sizeof(TermImage) * MAX_TERM_IMAGES + 256u * sizeof(u32)
        + (t->cwd ? strlen(t->cwd) + 1 : 0) + 256u;
    if (bytes > MB(16)) return false;
    bool oversized = s->arena.size >= bytes * 2 &&
                     s->arena.size - bytes >= SYNC_ARENA_SHRINK_SLACK;
    if (s->arena.size < bytes || oversized) {
        Arena next = arena_create(bytes);
        if (!next.base) {
            /* Cannot shrink right now: an oversized arena still WORKS. Only a
             * too-small one is a failed capture. */
            if (s->arena.size < bytes) return false;
        } else {
            arena_destroy(&s->arena);
            s->arena = next;
        }
    }
    arena_reset(&s->arena);
    s->view = *t;
    Terminal *v = &s->view;
    v->sync_snapshot = NULL;
    v->paste_log = NULL;                    /* owned by the live terminal */
    v->gfx_transfer = NULL;                 /* likewise: the view parses nothing */
    v->paste_log_free = NULL;
    v->sync_view = true;
    v->sync_view_slots = NULL;              /* dense: every row is present */
    v->cells = arena_alloc(&s->arena, count * sizeof(Cell));
    v->sync_view_lengths = arena_alloc(&s->arena, (usize)t->rows * sizeof(i32));
    if (!v->cells || !v->sync_view_lengths) return false;
    Cell blank = {.codepoint = ' ', .attr = t->cursor_attr};
    for (i32 row = 0; row < t->rows; row++) {
        i32 len = 0;
        const Cell *src = terminal_visible_row(t, row, &len);
        Cell *dst = v->cells + (usize)row * t->cols;
        i32 n = src ? MIN(len, t->cols) : 0;
        v->sync_view_lengths[row] = n;
        if (n > 0) memcpy(dst, src, (usize)n * sizeof(Cell));
        for (i32 col = n; col < t->cols; col++) dst[col] = blank;
    }
#define SNAP_COPY(field, size) do { \
    v->field = NULL; \
    if (t->field && (size)) { \
        v->field = arena_alloc(&s->arena, (size)); \
        if (!v->field) return false; \
        memcpy(v->field, t->field, (size)); \
    } \
} while (0)
    SNAP_COPY(dirty_rows, (usize)t->dirty_words * sizeof(u64));
    SNAP_COPY(line_wrapped, (usize)t->rows * sizeof(bool));
    SNAP_COPY(row_zones, (usize)t->rows * sizeof(SemanticZone));
    SNAP_COPY(ul_map, (usize)t->ul_map_cap * sizeof(UnderlineEntry));
    SNAP_COPY(palette, 256u * sizeof(u32));
    SNAP_COPY(images, MAX_TERM_IMAGES * sizeof(TermImage));
    SNAP_COPY(cwd, t->cwd ? strlen(t->cwd) + 1 : 0);
#undef SNAP_COPY
    /* Only render code consumes this view; no mutable parser, worker or
     * scrollback allocation may be borrowed across subsequent PTY reads. */
    v->grid_base = v->alt_cells = v->alt_base = NULL;
    v->ul_alt_map = NULL; v->ul_alt_cap = v->ul_alt_count = 0; v->ul_alt_base = 0;
    v->sb_ring = NULL; v->sb_scratch = NULL; v->sb_scratch_cap = 0;
    v->tab_stops = NULL; v->dcs_buf = v->apc_buf = NULL;
    v->icon_name = v->hyperlink_uri = v->pending_command = v->command_cwd = NULL;
    v->title_stack = NULL; v->image_async = NULL;
    v->search.matches = NULL; v->search.match_count = v->search.match_cap = 0;
    v->search.work = NULL;
    /* No matches means no current match: terminal_search_is_current() only
     * checks active && current >= 0 before indexing matches[]. */
    v->search.active = false; v->search.current = -1;
    s->ready = true;
    return true;
}

Terminal *terminal_sync_view(Terminal *t) {
    if (!t) return NULL;
    if (!t->sync_pending) { terminal_sync_release(t); return t; }
    if (t->sync_snapshot && t->sync_snapshot->ready) {
        return &t->sync_snapshot->view;
    }
    return t;
}

Terminal *terminal_create(i32 cols, i32 rows) {
    usize cell_count;
    if (!terminal_grid_cell_count(cols, rows, &cell_count)) return NULL;

    Terminal *t = calloc(1, sizeof(Terminal));
    if (!t) return NULL;

    t->cols = cols;
    t->rows = rows;
    t->cells = calloc(cell_count, sizeof(Cell));
    if (!t->cells) { terminal_destroy(t); return NULL; }
    t->alt_cells = NULL;

    /* Default attributes */
    t->cursor_attr.fg = FG_DEFAULT;
    t->cursor_attr.bg = BG_DEFAULT;
    /* A saved-cursor slot nothing was saved into restores home and THESE
     * colours, on either screen — a zeroed attribute is black on black. */
    t->saved_attr = t->cursor_attr;
    t->parked.saved_attr = t->cursor_attr;
    t->cursor_visible = true;
    t->cursor_style   = CURSOR_BLOCK;

    /* Scroll region = full screen */
    t->scroll_top    = 0;
    t->scroll_bottom = rows - 1;

    /* No cell is open for cluster continuation yet (see put_char). */
    t->cluster_x = -1;
    t->cluster_y = -1;

    /* Mode defaults. Grapheme clustering (DEC private mode 2027) is ON. A
     * program that wants the old per-codepoint wcwidth arithmetic turns it off
     * with CSI ? 2027 l. */
    t->mode = MODE_WRAP | MODE_GRAPHEME;

    /* Scrollback ring buffer */
    terminal_set_scrollback_limit(t, 2000);

    /* Tab stops every 8 columns */
    t->tab_stops = calloc((usize)cols, sizeof(bool));
    if (!t->tab_stops) { terminal_destroy(t); return NULL; }
    for (i32 i = 0; i < cols; i += 8) {
        t->tab_stops[i] = true;
    }

    /* Fill with spaces */
    Cell blank = { .codepoint = ' ', .attr = t->cursor_attr };
    for (usize i = 0; i < cell_count; i++) {
        t->cells[i] = blank;
    }

    /* allocate multi-word dirty bitmap */
    t->dirty_words = (rows - 1) / 64 + 1;
    if (t->dirty_words < 1) t->dirty_words = 1;
    t->dirty_rows  = calloc((usize)t->dirty_words, sizeof(u64));
    if (!t->dirty_rows) { terminal_destroy(t); return NULL; }

    /* Line wrap tracking (per-row) */
    t->line_wrapped = calloc((usize)rows, sizeof(bool));
    if (!t->line_wrapped) { terminal_destroy(t); return NULL; }

    /* Semantic zones (per-row) */
    t->row_zones = calloc((usize)rows, sizeof(SemanticZone));
    if (!t->row_zones) { terminal_destroy(t); return NULL; }
    t->current_zone = ZONE_NONE;
    t->prompt_start_row = -1;
    t->input_start_row = -1;
    t->input_start_col = -1;
    t->output_start_row = -1;
    t->last_cmd_failed = false;

    /* Initialize 256-color palette + default colors */
    terminal_init_palette(t);

    TERM_DIRTY_ALL(t);
    return t;
}

usize terminal_owned_bytes(const Terminal *t) {
    if (!t) return 0;
    usize n = t->sb_byte_usage + termline_cache_bytes(t) + t->images_byte_usage;
    if (t->sync_snapshot) n += sizeof(TermSyncSnapshot) + t->sync_snapshot->arena.size
        + t->sync_snapshot->retired_bytes;
    /* Both grids, and the slack travels with them: `cells` is a WINDOW into a
     * larger allocation whenever grid_slack > 0 and destroying the Terminal
     * frees the whole thing, so counting rows*cols alone under-reports. */
    if (t->cols > 0 && t->rows > 0) {
        usize row_bytes = (usize)t->cols * sizeof(Cell);
        n += (usize)(t->rows + (t->grid_slack > 0 ? t->grid_slack : 0)) * row_bytes;
        if (t->alt_base)
            n += (usize)(t->rows + (t->alt_slack > 0 ? t->alt_slack : 0)) * row_bytes;
    }
    if (t->dcs_cap > 0) n += (usize)t->dcs_cap;
    if (t->apc_cap > 0) n += (usize)t->apc_cap;

    /* THE SIDE TABLES, and the reason they are here rather than "close
     * enough": this figure decides what the tab-trash queue admits, so a
     * terminal whose scrollback is empty but whose per-row and per-match
     * tables are not still costs what it costs. Everything terminal_destroy
     * frees is counted or deliberately named below; the list is kept in the
     * same order as that function so the two can be read against each other.
     *
     * A cold 80x24 terminal is a few KiB of this. A 200x50 one that has been
     * searched, underlined and scrolled is not: match_cap alone reaches
     * MAX_SEARCH_MATCHES entries. */
    if (t->tab_stops)          n += (usize)t->cols * sizeof(bool);
    if (t->ul_map)             n += (usize)t->ul_map_cap * sizeof(UnderlineEntry);
    if (t->ul_alt_map)         n += (usize)t->ul_alt_cap * sizeof(UnderlineEntry);
    if (t->title_stack)        n += 8u * 256u;         /* calloc(8, 256) */
    n += terminal_search_bytes(t);
    if (t->dirty_rows)         n += (usize)t->dirty_words * sizeof(u64);
    if (t->line_wrapped)       n += (usize)t->rows * sizeof(bool);
    if (t->row_zones)          n += (usize)t->rows * sizeof(SemanticZone);
    if (t->alt_line_wrapped)   n += (usize)t->rows * sizeof(bool);
    if (t->alt_row_zones)      n += (usize)t->rows * sizeof(SemanticZone);
    if (t->palette)            n += 256u * sizeof(u32);
    if (t->sb_scratch)         n += (usize)t->sb_scratch_cap * sizeof(Cell);
    if (t->images)             n += (usize)MAX_TERM_IMAGES * sizeof(TermImage);
    if (t->pending_command)    n += (usize)t->pending_command_cap;
    if (t->command_cwd) n += strlen(t->command_cwd) + 1;
    if (t->cwd)                n += strlen(t->cwd) + 1;
    if (t->icon_name)          n += strlen(t->icon_name) + 1;
    if (t->hyperlink_uri)      n += strlen(t->hyperlink_uri) + 1;

    /* NOT counted, and each for a reason rather than by omission:
     *   - the Terminal struct itself, because the queue charges for what a
     *     terminal RETAINS and the struct is the same fixed cost for every
     *     one of them;
     *   - the Session behind it, which the queue does not own and does not
     *     free — it is detached before a terminal reaches the trash;
     *   - the async image decoder's transient staging, which is bounded by
     *     its own worker and gone by the time anything is trashed. */
    return n;
}

void terminal_destroy(Terminal *t) {
    if (!t) return;
    terminal_sync_release(t);
    if (t->sync_snapshot) {
        arena_destroy(&t->sync_snapshot->arena);
        free(t->sync_snapshot);
        t->sync_snapshot = NULL;
    }
    /* free() wants the allocation, not the slid window into it. No copy needed
     * — the contents are about to go away. */
    if (t->grid_slide > 0 && t->grid_base) {
        t->cells = t->grid_base;
        t->grid_slide = 0;
    }
    free(t->cells);
    terminal_grid_free_alt(t);
    free(t->tab_stops);
    free(t->ul_map);
    free(t->ul_alt_map);
    free(t->title_stack);
    free(t->hyperlink_uri);
    free(t->cwd);
    free(t->icon_name);
    terminal_search_stop(t);
    free(t->dirty_rows);
    terminal_str_bufs_free(t);
    free(t->line_wrapped);
    free(t->row_zones);
    free(t->pending_command);
    free(t->command_cwd);
    free(t->palette);
    free(t->sb_scratch);
    /* Decode workers publish into this Terminal's image slots. Stop/join before
     * freeing either the slot pool or the Terminal owner. */
    terminal_image_async_shutdown(t);
    /* Free inline images (per-pixel) then the slot pool itself. */
    terminal_clear_images(t);
    free(t->images);
    t->images = NULL;
    terminal_free_scrollback(t);
    if (t->paste_log && t->paste_log_free) t->paste_log_free(t->paste_log);
    free(t);
}

/* External scrollback helpers from buffer.c */
extern void sb_push(Terminal *t, Cell *row_cells, i32 len, bool wrapped);
extern void sb_pop_newest(Terminal *t, i32 n);
extern TermLine *sb_get(Terminal *t, i32 index);
extern void buffer_scroll_up(Terminal *t, i32 n);

/* Forward decl: defined below the inline-image helpers but referenced by
 * terminal_resize() when invalidating out-of-bounds image slots. */
static inline usize term_image_pixel_bytes(const TermImage *img);

/* Disables the keep-scrollback fast path. The switch is checked once per
 * resize, not per row or cell. */
bool liu_resize_keep_scrollback_disabled = false;

/* Counts resizes that use the keep-scrollback path and the number of
 * scrollback rows preserved. */
_Atomic u64 liu_resize_keep_hits = 0;
_Atomic u64 liu_resize_keep_rows = 0;

/* Does the stored row start with the lead of a wide character? */
static bool resize_row_starts_wide(TermLine *ln) {
    if (!ln) return false;
    if (!ln->cold) return ln->len > 0 && ln->cells && (ln->cells[0].attr.flags & ATTR_WIDE);
    const TermLineCompressed *c = ln->cold;
    if (c->len <= 0) return false;
    if (c->has_leads) return (c->wide_bits[(c->len + 63) / 64] & 1u) != 0;
    return c->run_count > 0 && (c->runs[0].attr.flags & ATTR_WIDE);
}

/* Would the reflow lay this stored row out as it is, at width `cols`? The
 * metadata test below proves the SHAPE of a logical line survives; this proves
 * its CELLS do, without materializing anything. Three things the reflow changes
 * at an unchanged width:
 *
 *   - A narrow cell whose character is wide gets its width back (a character
 *     that passed through a one-column grid lost the flag there). The parser
 *     also leaves one of those on purpose — a cluster that a variation
 *     selector widens in the LAST column stays one cell rather than clobber
 *     its neighbour (vt_parser.c, cluster_extend) — and the reflow moves it
 *     onto the next row. In a one-column grid it is the other way round: a
 *     wide cell is narrowed.
 *   - The row that ends a logical line is trimmed of its trailing blanks. A
 *     row pushed as a wrap and unflagged later (a one-row screen, where every
 *     character lands on the cell wrap_break_above_screen guards) keeps them.
 *   - A SPACER (a dummy with no lead: the column a wide character that did
 *     not fit left behind) is skipped, and made again only where a wide
 *     character lands in the last column. So one stays only in the last
 *     column of a wrapped row whose next row starts with a wide character.
 *
 * Most rows are decided by OR-ing their codepoints: nothing at or above
 * U+1100 can be wide. */
static bool resize_row_is_kept_as_is(TermLine *ln, TermLine *next, i32 cols, bool ends_line) {
    if (!ln) return false;
    if (!ln->cold) {
        const Cell *cells = ln->cells;
        i32 len = ln->len;
        if (len > 0 && !cells) return false;
        /* The reflow trims an uncompressed row whether or not it wraps. */
        if (content_len_trimmed(cells, len) != len) return false;
        for (i32 i = 0; i < len; i++) {
            u16 f = cells[i].attr.flags;
            bool wide = (f & ATTR_WIDE) != 0;
            if (cols < 2 && (f & (ATTR_WIDE | ATTR_WDUMMY))) return false;
            if ((f & ATTR_WDUMMY) && !(i > 0 && (cells[i - 1].attr.flags & ATTR_WIDE)) &&
                (ends_line || i != len - 1 || !resize_row_starts_wide(next))) return false;
            if (!wide && !(f & ATTR_WDUMMY) && cols >= 2 && cells[i].codepoint >= 0x1100 &&
                utf8_char_width(cells[i].codepoint) == 2) return false;
        }
        return true;
    }
    const TermLineCompressed *c = ln->cold;
    i32 len = c->len;
    if (len <= 0) return true;
    if (ends_line && termline_content_length(c) != len) return false;
    u32 any = 0;
    for (i32 i = 0; i < len; i++) any |= c->codepoints[i];
    i32 words = (len + 63) / 64;
    u64 dummies = 0;
    for (i32 w = 0; c->wide_bits && w < words; w++) dummies |= c->wide_bits[w];
    if (cols < 2) return !c->has_leads && dummies == 0 && any < 0x1100;
    /* A line from an older sleep snapshot carries the wide LEAD in its runs
     * rather than in lead words: not worth decoding here. */
    if (!c->has_leads && (dummies || any >= 0x1100))
        for (u16 r = 0; r < c->run_count; r++)
            if (c->runs[r].attr.flags & ATTR_WIDE) return false;
    const u64 *leads = c->has_leads ? c->wide_bits + words : NULL;
    if (dummies) {
        for (i32 i = 0; i < len; i++) {
            if (!((c->wide_bits[i >> 6] >> (i & 63)) & 1u)) continue;
            bool led = i > 0 && leads && ((leads[(i - 1) >> 6] >> ((i - 1) & 63)) & 1u);
            if (!led && (ends_line || i != len - 1 || !resize_row_starts_wide(next))) return false;
        }
    }
    if (any < 0x1100) return true;
    for (i32 i = 0; i < len; i++) {
        u32 cp = c->codepoints[i];
        if (cp < 0x1100) continue;
        bool lead = leads && ((leads[i >> 6] >> (i & 63)) & 1u);
        bool dummy = c->wide_bits && ((c->wide_bits[i >> 6] >> (i & 63)) & 1u);
        if (!lead && !dummy && utf8_char_width(cp) == 2) return false;
    }
    return true;
}

/* How many leading scrollback lines a resize may leave exactly
 * where they are. Returns 0 when none may (which is also the answer for
 * an empty scrollback) and never touches the terminal.
 *
 * The reflow is not always the identity even at an unchanged width: it
 * concatenates each logical line and re-wraps the total, so a soft-wrapped
 * run whose LAST row is empty comes back one row shorter. Everything else
 * about a same-width rewrap is identity, and both facts are decidable from
 * METADATA alone — the stored length and the wrapped flag — with no line
 * decompressed and nothing allocated:
 *
 *   - sb_push stores a wrapped row's length verbatim and trims an unwrapped
 *     one (buffer.c scrollback_line_len), so within a logical line every row
 *     but the last must have the NEW width and the last is already content-trimmed.
 *     content_len_trimmed would take nothing further off it.
 *   - so the line's concatenated length is (k-1)*cols + last, and
 *     ceil(that / cols) is k for last in [1, cols] and k-1 for last == 0.
 *
 * A line that would collapse, or one whose stored lengths do not match that
 * shape, ends the prefix rather than being handled — the caller falls back to
 * the reflow, which is always correct. So does a line whose CELLS the reflow
 * would lay out differently even at this width (resize_row_is_kept_as_is):
 * the metadata alone cannot see those.
 *
 * The last logical line is excluded when it continues onto the SCREEN (its
 * final row is wrapped), because then it is not a scrollback line at all: the
 * screen half of it can rewrap and the two halves must travel together. */
static i32 resize_keepable_prefix(Terminal *t, i32 cols) {
    if (!t || !t->sb_ring || t->sb_count <= 0 || cols <= 0) return 0;

    #define SB_META(i, LEN, WRAP) do {                                        \
        TermLine *ln_ = sb_get(t, (i));                                       \
        if (!ln_) return 0;                                                   \
        (LEN)  = ln_->cold ? ln_->cold->len     : ln_->len;                   \
        (WRAP) = ln_->cold ? ln_->cold->wrapped : ln_->wrapped;               \
    } while (0)

    i32 limit = t->sb_count;
    {
        i32 len; bool wrapped;
        SB_META(limit - 1, len, wrapped);
        (void)len;
        if (wrapped) {
            /* Straddles onto the screen — walk back to where its first row is. */
            limit--;
            while (limit > 0) {
                bool w; i32 l;
                SB_META(limit - 1, l, w);
                (void)l;
                if (!w) break;
                limit--;
            }
        }
    }

    i32 a = 0;
    i32 keep = 0;
    while (a < limit) {
        i32 i = a;
        while (i < limit - 1) {
            i32 len; bool wrapped;
            SB_META(i, len, wrapped);
            if (!wrapped) break;
            if (len != cols) return keep;   /* not the shape the rewrap assumes */
            if (!resize_row_is_kept_as_is(sb_get(t, i), sb_get(t, i + 1), cols, false)) return keep;
            i++;
        }
        i32 last_len; bool last_wrapped;
        SB_META(i, last_len, last_wrapped);
        if (last_wrapped) return keep;      /* runs past the prefix */
        if (last_len < 0 || last_len > cols) return keep;
        i32 k = i - a + 1;
        if (k > 1 && last_len == 0) return keep;  /* the rewrap would collapse it */
        if (!resize_row_is_kept_as_is(sb_get(t, i), NULL, cols, true)) return keep;
        a = i + 1;
        keep = a;
    }
    #undef SB_META
    return keep;
}

/* Underline entries across the reflow — defined with the sparse map below. */
typedef struct { u32 color; u8 style; } UnderlineKind;
typedef struct { UnderlineKind *kinds; i32 count, cap; } UnderlineKinds;
static void underline_stamp(Terminal *t, Cell *grid, i32 cols, i32 rows, UnderlineKinds *tb);
static void underline_harvest(Terminal *t, Cell *grid, const UnderlineKinds *tb);

/* One reflowed row: where its cells sit in the packed output buffer, how many
 * of them are stored, and whether the row continues on the next one. */
typedef struct { i32 off; i32 len; bool wrapped; } ReflowOutRow;

/* Make room for `need` output rows.
 *
 * Phase 2 counts ceil(cells / new_cols) rows per logical line, and that is a
 * FLOOR: a wide character that does not fit in the last column starts the next
 * row and leaves a pad cell behind, so the layout can need more. The layout is
 * the authority; the count only sizes the first allocation. `cell_cap` follows
 * (rows x columns, the most the packed buffer can ever hold) and both stay
 * within i32, the type of every offset in ReflowOutRow. False leaves the
 * arrays as they were. */
static bool reflow_grow_rows(ReflowOutRow **rows, i32 *cap, usize *cell_cap,
                             i32 new_cols, i32 need) {
    if (need <= *cap) return true;
    i64 want = (i64)*cap + (i64)*cap / 2 + 16;
    if (want < need) want = need;
    if (want > INT32_MAX) want = INT32_MAX;
    if (want < need) return false;
    usize bytes, cells;
    if (!checked_mul_usize((usize)want, sizeof **rows, &bytes) ||
        !checked_mul_usize((usize)want, (usize)new_cols, &cells) ||
        cells > (usize)INT32_MAX) return false;
    ReflowOutRow *grown = realloc(*rows, bytes);
    if (!grown) return false;
    memset(grown + *cap, 0, (usize)(want - *cap) * sizeof *grown);
    *rows = grown;
    *cap = (i32)want;
    *cell_cap = cells;
    return true;
}

void terminal_resize(Terminal *t, i32 new_cols, i32 new_rows) {
    usize new_cell_count;
    if (!t || !terminal_grid_cell_count(new_cols, new_rows, &new_cell_count)) return;
    if (!terminal_grid_cell_count(t->cols, t->rows, NULL) ||
        !t->cells || t->sb_count < 0 ||
        t->sb_count > TERM_SCROLLBACK_HARD_MAX_LINES) return;
    if (new_cols == t->cols && new_rows == t->rows) return;

    /* A full-screen program is up: the screen that is SHOWING is its own and
     * is only cut or padded (it repaints on SIGWINCH), but the shell's screen
     * and its history are parked behind it and nobody repaints those. Cut to
     * the new size from the top-left as well, they would lose the tail of
     * every long line and the rows with the prompt, and the history would
     * keep lines wider than the terminal. So the parked primary is made the
     * live grid for the length of an ordinary resize, with the cursor the
     * shell will get back, and the program's screen is put back on top
     * afterwards. */
    if ((t->mode & MODE_ALT_SCREEN) && t->alt_cells) {
        i32 ax = t->cursor_x, ay = t->cursor_y;
        bool entry_is_saved = (t->alt_cursor_x == t->parked.saved_x && t->alt_cursor_y == t->parked.saved_y);
        terminal_grid_unslide(t);
        terminal_grid_swap_alt(t);                       /* primary live, the program's parked */
        t->mode &= ~MODE_ALT_SCREEN;
        t->cursor_x = t->parked.saved_x < 0 ? 0 : t->parked.saved_x > t->cols ? t->cols : t->parked.saved_x;
        t->cursor_y = t->parked.saved_y < 0 ? 0 : t->parked.saved_y >= t->rows ? t->rows - 1 : t->parked.saved_y;
        terminal_resize(t, new_cols, new_rows);
        bool resized = (t->cols == new_cols && t->rows == new_rows);
        t->parked.saved_x = t->cursor_x;
        t->parked.saved_y = t->cursor_y;
        if (entry_is_saved) {
            t->alt_cursor_x = t->cursor_x;
            t->alt_cursor_y = t->cursor_y;
        }
        if (t->alt_cells) terminal_grid_swap_alt(t);     /* the program's screen back on top */
        t->mode |= MODE_ALT_SCREEN;
        t->cursor_x = ax;
        t->cursor_y = ay;
        if (resized) {
            if (t->cursor_x >= new_cols) t->cursor_x = new_cols - 1;
            if (t->cursor_y >= new_rows) t->cursor_y = new_rows - 1;
            if (t->alt_cursor_x > new_cols) t->alt_cursor_x = new_cols;
            if (t->alt_cursor_y >= new_rows) t->alt_cursor_y = new_rows - 1;
        }
        return;
    }

    /* Set by the reflow when the cursor sat right after text that fills its
     * row: the wrap is still pending at the new size. */
    bool cursor_pending_wrap = false;

    /* The synchronized-output picture was sized for the old geometry. See
     * terminal_sync_trim: a no-op while a picture is being shown. */
    terminal_sync_trim(t);

    /* The reflow below rewraps every scrollback line, so a selection's anchor
     * no longer names the same run of text — its line may become two, or half
     * of one. There is no faithful way to carry it across, and keeping it
     * would put the highlight on whatever ends up at those rows. Note this is
     * after the no-change early-out, so RIS (which resizes to the current
     * geometry) does not drop a selection. */
    selection_clear(t);

    /* Everything below frees and replaces t->cells. Put the grid window back
     * on its allocation first, and drop the slack bookkeeping: the replacement
     * buffers are plain rows*cols and a stale grid_slack would let the next
     * scroll walk off the end of one. The next full-screen scroll re-arms it. */
    terminal_grid_unslide(t);
    t->grid_base  = NULL;
    t->grid_slack = 0;

    i32 old_cols = t->cols;
    i32 old_rows = t->rows;
    Cell blank = { .codepoint = ' ', .attr = t->cursor_attr };

    /* Allocate every geometry-dependent array before mutating the live
     * terminal. This makes resize atomic with respect to OOM and guarantees
     * the parser never observes a NULL tab-stop table or active alt screen
     * with a missing backing framebuffer. */
    i32 resize_dirty_words = (new_rows - 1) / 64 + 1;
    Cell *resize_cells = calloc(new_cell_count, sizeof(Cell));
    Cell *resize_alt = t->alt_cells
                     ? calloc(new_cell_count, sizeof(Cell))
                     : NULL;
    bool *resize_tab_stops = calloc((usize)new_cols, sizeof(bool));
    u64 *resize_dirty_rows = calloc((usize)resize_dirty_words, sizeof(u64));
    bool *resize_line_wrapped = calloc((usize)new_rows, sizeof(bool));
    SemanticZone *resize_row_zones = calloc((usize)new_rows,
                                             sizeof(SemanticZone));
    /* The parked screen's flags and zones follow its grid to the new height. */
    bool *resize_alt_wrapped = t->alt_cells
                             ? calloc((usize)new_rows, sizeof(bool)) : NULL;
    SemanticZone *resize_alt_zones = t->alt_cells
                             ? calloc((usize)new_rows, sizeof(SemanticZone)) : NULL;
    if (!resize_cells || (t->alt_cells && !resize_alt) ||
        (t->alt_cells && (!resize_alt_wrapped || !resize_alt_zones)) ||
        !resize_tab_stops || !resize_dirty_rows ||
        !resize_line_wrapped || !resize_row_zones) {
        free(resize_cells);
        free(resize_alt);
        free(resize_alt_wrapped);
        free(resize_alt_zones);
        free(resize_tab_stops);
        free(resize_dirty_rows);
        free(resize_line_wrapped);
        free(resize_row_zones);
        return;
    }
    for (usize i = 0; i < new_cell_count; i++) resize_cells[i] = blank;
    if (resize_alt) {
        for (usize i = 0; i < new_cell_count; i++) resize_alt[i] = blank;
    }
    for (i32 i = 0; i < new_cols; i += 8) resize_tab_stops[i] = true;

    /* Underline style and colour are keyed by position, and every position is
     * about to change. Stamp each entry into its own cell — for both screens —
     * so whatever happens to the cells below carries it; finish_resize reads
     * the stamps back. Nothing between here and there returns. */
    UnderlineKinds ul_kinds = {0};
    underline_stamp(t, t->cells, old_cols, old_rows, &ul_kinds);
    if (t->alt_cells && t->ul_alt_count > 0) {
        terminal_underline_swap_parked(t);
        underline_stamp(t, t->alt_cells, old_cols, old_rows, &ul_kinds);
        terminal_underline_swap_parked(t);
    }

    /* ---------------------------------------------------------------
     * Alt screen: simple truncate/pad (no reflow)
     * --------------------------------------------------------------- */
    if (t->mode & MODE_ALT_SCREEN) {
        i32 cp_rows = MIN(old_rows, new_rows);
        i32 cp_cols = MIN(old_cols, new_cols);
        for (i32 y = 0; y < cp_rows; y++)
            memcpy(&resize_cells[y * new_cols], &t->cells[y * old_cols],
                   (usize)cp_cols * sizeof(Cell));
        free(t->cells);
        t->cells = resize_cells;
        resize_cells = NULL;
        goto finish_resize;
    }


    /* ---------------------------------------------------------------
     * Primary screen: 5-phase reflow algorithm
     *
     * Leading scrollback lines that provably come out of the reflow as they
     * are (resize_keepable_prefix: their lengths and wrap flags, and the cells
     * of a row with a wide character, a spacer or blanks the reflow trims —
     * resize_row_is_kept_as_is), so they are skipped
     * here and left untouched in the ring by Phase 5 instead of being
     * decompressed, re-packed and re-compressed. On a deep history that round
     * trip is most of the cost of a vertical drag step.
     *
     * `sb_first` is the ONLY thing the two cases differ by. Every rule below —
     * trimming, wide-character packing, the cursor mapping, the screen/
     * scrollback split — runs unchanged on the suffix, which is what keeps
     * this from being a second implementation of the reflow that drifts from
     * the first one.
     * --------------------------------------------------------------- */
    i32 sb_first = 0;
    if (!liu_resize_keep_scrollback_disabled)
        sb_first = resize_keepable_prefix(t, new_cols);
    if (sb_first > 0) {
        atomic_fetch_add_explicit(&liu_resize_keep_hits, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&liu_resize_keep_rows, (u64)sb_first, memory_order_relaxed);
    }
    const i32 sb_live = t->sb_count - sb_first;
    {
        /*
         * Phase 1: Build logical lines from scrollback + screen
         *          by chaining soft-wrapped rows.
         *
         * A "logical line" = a sequence of physical rows where each row
         * (except the last) was soft-wrapped (line_wrapped flag is true).
         */
        i32 total_phys = sb_live + old_rows;

        /* Borrow source rows until publication. Cold rows stay compressed;
         * content length comes from their codepoint/run metadata. */
        typedef struct {
            const Cell *cells;
            const TermLineCompressed *cold;
            i32 len, content;
            bool wrapped;
        } PhysRow;

        PhysRow *phys = calloc((usize)total_phys, sizeof(PhysRow));
        if (!phys) {
            /* Allocation failed: fall back to simple truncate/pad */
            i32 cp_r = MIN(old_rows, new_rows);
            i32 cp_c = MIN(old_cols, new_cols);
            for (i32 y = 0; y < cp_r; y++)
                memcpy(&resize_cells[y * new_cols], &t->cells[y * old_cols],
                       (usize)cp_c * sizeof(Cell));
            free(t->cells);
            t->cells = resize_cells;
            resize_cells = NULL;
            goto finish_resize;
        }

        for (i32 i = sb_first; i < t->sb_count; i++) {
            const i32 pi = i - sb_first;
            TermLine *sl = sb_get(t, i);
            if (!sl) continue;
            if (sl->cold) {
                phys[pi].cold = sl->cold;
                phys[pi].len = sl->cold->len;
                phys[pi].content = sl->cold->wrapped && pi + 1 < total_phys
                                ? sl->cold->len : termline_content_length(sl->cold);
                phys[pi].wrapped = sl->cold->wrapped;
            } else {
                phys[pi].cells = sl->cells;
                phys[pi].len = sl->len;
                phys[pi].content = content_len_trimmed(sl->cells, sl->len);
                phys[pi].wrapped = sl->wrapped;
            }
        }
        /* Fill from screen */
        for (i32 y = 0; y < old_rows; y++) {
            i32 idx = sb_live + y;
            phys[idx].cells   = &t->cells[y * old_cols];
            phys[idx].len     = old_cols;
            phys[idx].wrapped = (t->line_wrapped && y < old_rows - 1) ? t->line_wrapped[y] : false;
            phys[idx].content = content_len_trimmed(phys[idx].cells, old_cols);
        }

        /* Build logical lines: find where each one starts.
         * A row starts a new logical line if it is the first row, or
         * the *previous* row was NOT wrapped (i.e. ended with a newline). */
        i32 *log_start = calloc((usize)total_phys, sizeof(i32));  /* start phys index of each logical line */
        i32 log_count = 0;
        if (log_start) {
            for (i32 i = 0; i < total_phys; i++) {
                if (i == 0 || !phys[i - 1].wrapped) {
                    log_start[log_count++] = i;
                }
            }
        } else {
            /* Can't build logical lines, fall through with no reflow */
            free(phys);
            i32 cp_r = MIN(old_rows, new_rows);
            i32 cp_c = MIN(old_cols, new_cols);
            for (i32 y = 0; y < cp_r; y++)
                memcpy(&resize_cells[y * new_cols], &t->cells[y * old_cols],
                       (usize)cp_c * sizeof(Cell));
            free(t->cells);
            t->cells = resize_cells;
            resize_cells = NULL;
            goto finish_resize;
        }

        /*
         * Phase 2: Estimate physical rows after re-wrapping to new_cols.
         *          For each logical line, ceil(cells / new_cols). A FLOOR, not
         *          the answer: a wide character that will not fit in the last
         *          column starts the next row, so Phase 4 lays the line out
         *          itself and grows its output to what the layout needs.
         */

        /* For each logical line, gather total cells */
        i32 new_total_phys = 0;
        /* Sum of real (trailing-space-trimmed) content across all logical
         * lines. Phase 4 sizes its packed output buffer from this instead of
         * out_cap * new_cols. */
        i64 content_total64 = 0;
        for (i32 li = 0; li < log_count; li++) {
            i32 start = log_start[li];
            i32 end = (li + 1 < log_count) ? log_start[li + 1] : total_phys;

            /* Count total non-trailing-space cells in this logical line */
            i64 total_cells64 = 0;
            for (i32 pi = start; pi < end; pi++) {
                i32 part;
                if (pi == end - 1) {
                    /* Last physical row: trim trailing spaces */
                    part = phys[pi].content;
                } else {
                    part = phys[pi].len;
                }
                if (part < 0 || part > old_cols ||
                    total_cells64 > INT32_MAX - (i64)part) {
                    total_cells64 = 0;
                    break;
                }
                total_cells64 += part;
            }

            i32 total_cells = (i32)total_cells64;
            i32 nrows = total_cells > 0
                      ? (i32)(((i64)total_cells + new_cols - 1) / new_cols)
                      : 1;
            if (content_total64 > (i64)INT32_MAX - (i64)total_cells)
                content_total64 = INT32_MAX;
            else
                content_total64 += total_cells;
            if (new_total_phys > INT32_MAX - nrows)
                new_total_phys = INT32_MAX;
            else
                new_total_phys += nrows;
        }

        /*
         * Phase 3: Map cursor position through logical lines.
         *          Find which logical line the cursor is in, and its offset.
         */
        i32 cursor_phys_idx = sb_live + t->cursor_y;
        i32 cursor_log = -1;
        i32 cursor_cell_offset = -1;

        for (i32 li = 0; li < log_count; li++) {
            i32 start = log_start[li];
            i32 end = (li + 1 < log_count) ? log_start[li + 1] : total_phys;
            if (cursor_phys_idx >= start && cursor_phys_idx < end) {
                cursor_log = li;
                /* Calculate cell offset within the logical line */
                i64 cursor_offset64 = 0;
                for (i32 pi = start; pi < cursor_phys_idx; pi++) {
                    i32 part = phys[pi].len;
                    if (part < 0 || part > old_cols ||
                        cursor_offset64 > INT32_MAX - (i64)part) {
                        cursor_offset64 = INT32_MAX;
                        break;
                    }
                    cursor_offset64 += part;
                }
                if (t->cursor_x > 0 &&
                    cursor_offset64 <= INT32_MAX - (i64)t->cursor_x)
                    cursor_offset64 += t->cursor_x;
                cursor_cell_offset = (i32)cursor_offset64;
                break;
            }
        }

        /*
         * Phase 4: Write re-wrapped lines into a flat output buffer.
         *          Handle ATTR_WIDE/ATTR_WDUMMY properly (wide char can't be
         *          split at line boundary — push dummy to next line).
         */
        i32 out_cap = new_total_phys;
        if (out_cap < new_rows) out_cap = new_rows;

        /* Allocate output rows.
         *
         * Rows are appended to one packed buffer at their real length and
         * `OutRow` stores the offset. A full-width, uncompressed cell for every
         * row, whether or not the row has content and *written* by a blank
         * prefill, would make every byte resident — tens of MB at a 20000-line
         * scrollback, re-paid on every frame of a live window drag. Only cells
         * the re-wrap loop never wrote are dropped, and those are
         * blank-identical to the prefill of `resize_cells`, so the screen copy
         * is byte-identical. Soft-wrapped rows still store the full `new_cols`:
         * sb_push() records their length verbatim and the *next* reflow
         * concatenates exactly that many cells, so trimming them would shift
         * text. `flat_cap` is the same idea for the per-logical-line scratch:
         * one high-water-mark buffer grown on demand. */
        typedef ReflowOutRow OutRow;
        usize out_cell_count;
        bool out_size_ok = checked_mul_usize((usize)out_cap,
                                              (usize)new_cols,
                                              &out_cell_count) &&
                           out_cell_count <= (usize)INT32_MAX;
        /* Size from the measured content (Phase 2) plus one slack cell per row
         * for wide-char padding. Grows geometrically if a wide-char-heavy
         * buffer needs more; never exceeds out_cell_count, one full-width row
         * per output row. */
        usize out_alloc = 0, out_bytes = 0;
        if (out_size_ok) {
            usize est = (usize)content_total64 + (usize)out_cap;
            if (est > out_cell_count) est = out_cell_count;
            if (est < (usize)new_cols) est = (usize)new_cols;
            if (checked_mul_usize(est, sizeof(Cell), &out_bytes))
                out_alloc = est;
        }
        OutRow *out_rows = calloc((usize)out_cap, sizeof(OutRow));
        Cell   *out_flat = out_alloc ? malloc(out_bytes) : NULL;
        if (!out_rows || !out_flat) {
            free(out_rows);
            free(out_flat);
            free(log_start);
            free(phys);
            free(t->cells);
            t->cells = resize_cells;
            resize_cells = NULL;
            goto finish_resize;
        }
        /* out_rows is calloc'd: rows that are never emitted, and rows emitted
         * by the early-continue paths below, stay {off=0,len=0} — a zero-length
         * row is exactly what a full-width all-blank row reduces to in both
         * consumers (sb_push trims blanks; the screen copy leaves the
         * already-blank destination alone). */

        i32   out_row_count = 0;
        usize out_used = 0;
        i32   new_cursor_x = 0, new_cursor_y = 0;
        Cell *flat = NULL;
        i32   flat_cap = 0;
        bool  reflow_oom = false;   /* a line could not be laid out: see below */

        for (i32 li = 0; li < log_count; li++) {
            i32 start = log_start[li];
            i32 end = (li + 1 < log_count) ? log_start[li + 1] : total_phys;

            /* Flatten the logical line's cells */
            i64 total_cells64 = 0;
            for (i32 pi = start; pi < end; pi++) {
                i32 part = pi == end - 1
                         ? phys[pi].content
                         : phys[pi].len;
                if (part < 0 || part > old_cols ||
                    total_cells64 > INT32_MAX - (i64)part) {
                    total_cells64 = 0;
                    break;
                }
                total_cells64 += part;
            }
            i32 total_cells = (i32)total_cells64;

            /* If empty, emit one blank row (zero-length = all blanks) */
            if (total_cells == 0) {
                if (!reflow_grow_rows(&out_rows, &out_cap, &out_cell_count,
                                      new_cols, out_row_count + 1)) {
                    reflow_oom = true;
                    break;
                }
                out_rows[out_row_count].wrapped = false;
                /* Map cursor. The line is blank, not short: the cursor keeps
                 * its column (sending it to 0 would make a prompt of spaces, or
                 * a cursor parked by CUP, jump to the left edge on a resize).
                 */
                if (li == cursor_log) {
                    new_cursor_y = out_row_count;
                    new_cursor_x = cursor_cell_offset < new_cols ? cursor_cell_offset : new_cols - 1;
                    if (new_cursor_x < 0) new_cursor_x = 0;
                }
                out_row_count++;
                continue;
            }

            /* Grow the shared scratch buffer on demand; one allocation across
             * all logical lines instead of malloc/free per line. */
            if (total_cells > flat_cap) {
                usize flat_bytes;
                Cell *grown = checked_mul_usize((usize)total_cells, sizeof(Cell),
                                                &flat_bytes)
                            ? realloc(flat, flat_bytes) : NULL;
                /* A line that cannot be laid out is not "skipped", which would
                 * leave a row of blanks standing in for its text. The whole
                 * reflow gives up instead (below), before anything old is
                 * released. */
                if (!grown) { reflow_oom = true; break; }
                flat = grown;
                flat_cap = total_cells;
            }

            if (!flat || flat_cap < total_cells) { reflow_oom = true; break; }
            for (i32 i = 0; i < total_cells; i++) flat[i] = blank;

            i32 fi = 0;
            for (i32 pi = start; pi < end; pi++) {
                i32 copy_len = (pi == end - 1) ? phys[pi].content : phys[pi].len;
                if (copy_len > 0 && (phys[pi].cells || phys[pi].cold) &&
                    fi >= 0 && fi <= total_cells && copy_len <= total_cells - fi) {
                    if (phys[pi].cold)
                        termline_materialize(phys[pi].cold, &flat[fi], copy_len);
                    else
                        memcpy(&flat[fi], phys[pi].cells, (usize)copy_len * sizeof(Cell));
                    fi += copy_len;
                }

            }

            /* Re-wrap into rows of new_cols.
             *
             * The LAYOUT decides how many rows the line takes — a wide
             * character that does not fit in the last column starts the next
             * row — so rows are made until the line's cells are used up, not
             * for a count worked out beforehand (a loop that stopped on the
             * count would silently drop whatever was left). The cursor is
             * mapped through the place its cell actually landed. */
            i32  src = 0;
            i32  last_col = 0;         /* columns used by the line's last row */
            bool cursor_line = (li == cursor_log && cursor_cell_offset >= 0);
            bool cursor_done = false;

            while (src < total_cells) {
                if (!reflow_grow_rows(&out_rows, &out_cap, &out_cell_count,
                                      new_cols, out_row_count + 1)) {
                    reflow_oom = true;
                    break;
                }

                /* Reserve a full-width slot at the packed tail. Only the cells
                 * actually written are kept, so the unused remainder of this
                 * reservation is handed straight back to the next row. */
                Cell *row = NULL;
                if (out_used + (usize)new_cols <= out_alloc) {
                    row = out_flat + out_used;
                } else {
                    usize want = out_alloc * 2u;
                    if (want < out_used + (usize)new_cols)
                        want = out_used + (usize)new_cols;
                    if (want > out_cell_count) want = out_cell_count;
                    usize want_bytes;
                    Cell *grown_out =
                        (want >= out_used + (usize)new_cols &&
                         checked_mul_usize(want, sizeof(Cell), &want_bytes))
                        ? realloc(out_flat, want_bytes) : NULL;
                    if (grown_out) {
                        out_flat  = grown_out;
                        out_alloc = want;
                        row       = out_flat + out_used;
                    }
                }
                if (!row) { reflow_oom = true; break; }

                i32 col = 0;
                while (col < new_cols && src < total_cells) {
                    bool is_wide = (flat[src].attr.flags & ATTR_WIDE) != 0;
                    /* A dummy with no lead in front of it is the spacer a wrap
                     * leaves where a wide character did not fit (or an orphan
                     * half): it is not text, and a reflow that kept it turned
                     * the gap into a space in the middle of a word — one more
                     * with every resize. */
                    if (!is_wide && (flat[src].attr.flags & ATTR_WDUMMY)) {
                        if (cursor_line && !cursor_done && src == cursor_cell_offset) {
                            new_cursor_y = out_row_count;
                            new_cursor_x = col;
                            cursor_done = true;
                        }
                        src++;
                        continue;
                    }
                    /* A wide character that passed through a one-column grid
                     * lost its flag there; the character itself still says
                     * how wide it is. */
                    if (!is_wide && new_cols >= 2 && flat[src].codepoint >= 0x1100 &&
                        utf8_char_width(flat[src].codepoint) == 2) {
                        is_wide = true;
                        flat[src].attr.flags |= ATTR_WIDE;
                    }

                    if (is_wide && new_cols < 2) {
                        /* One column cannot hold both halves of a wide
                         * character. Keep the character, as a narrow cell:
                         * the text survives and copies back out intact. */
                        if (cursor_line && !cursor_done && src == cursor_cell_offset) {
                            new_cursor_y = out_row_count;
                            new_cursor_x = col;
                            cursor_done = true;
                        }
                        row[col] = flat[src];
                        row[col].attr.flags &= (u16)~(ATTR_WIDE | ATTR_WDUMMY);
                        col++;
                        src++;
                        if (src < total_cells &&
                            (flat[src].attr.flags & ATTR_WDUMMY)) {
                            if (cursor_line && !cursor_done &&
                                src == cursor_cell_offset) {
                                new_cursor_y = out_row_count;
                                new_cursor_x = col - 1;
                                cursor_done = true;
                            }
                            src++;
                        }
                        continue;
                    }
                    if (is_wide && col == new_cols - 1) {
                        /* Wide char won't fit — this cell is a spacer (a dummy
                         * with no lead, see the top of the loop) and the
                         * character starts the next row. */
                        row[col] = blank;
                        row[col].attr.flags |= ATTR_WDUMMY;
                        col++;
                        break;
                    }
                    if (cursor_line && !cursor_done && src == cursor_cell_offset) {
                        new_cursor_y = out_row_count;
                        new_cursor_x = col;
                        cursor_done = true;
                    }
                    row[col] = flat[src];
                    /* Skip WDUMMY following a WIDE */
                    if (is_wide) {
                        col++;
                        src++;
                        if (col < new_cols && src < total_cells &&
                            (flat[src].attr.flags & ATTR_WDUMMY)) {
                            /* A cursor on the dummy half belongs on the
                             * character, not on half of it. */
                            if (cursor_line && !cursor_done &&
                                src == cursor_cell_offset) {
                                new_cursor_y = out_row_count;
                                new_cursor_x = col - 1;
                                cursor_done = true;
                            }
                            row[col] = flat[src];
                            col++;
                            src++;
                        } else if (col < new_cols) {
                            /* Insert dummy for wide char */
                            row[col] = blank;
                            row[col].attr.flags |= ATTR_WDUMMY;
                            col++;
                        }
                    } else {
                        col++;
                        src++;
                    }
                }

                bool is_wrapped = src < total_cells;
                /* [0,col) is fully written by the loop above; [col,new_cols)
                 * was never touched. A soft-wrapped row must still carry its
                 * full width (sb_push stores the length verbatim and the next
                 * reflow concatenates exactly that many cells), so blank out
                 * the tail — normally a no-op because col == new_cols. The
                 * final row of a logical line keeps only `col`: everything
                 * past it is blank, which is what both consumers already
                 * produce for it. */
                i32 store_len = col;
                if (is_wrapped) {
                    for (i32 c = col; c < new_cols; c++) row[c] = blank;
                    store_len = new_cols;
                }
                out_rows[out_row_count].off     = (i32)out_used;
                out_rows[out_row_count].len     = store_len;
                out_rows[out_row_count].wrapped = is_wrapped;
                out_used += (usize)store_len;
                out_row_count++;
                last_col = col;
            }
            if (reflow_oom) break;

            /* The cursor was past the line's last cell (typing position after
             * the text): keep it that many columns on from the end of the last
             * row, clamped to the row — the same place plain column arithmetic
             * gives when nothing was padded. */
            if (cursor_line && !cursor_done) {
                i64 c = (i64)last_col + ((i64)cursor_cell_offset - (i64)total_cells);
                if (c < 0) c = 0;
                new_cursor_y = out_row_count - 1;
                /* Exactly one past the last column is the pending wrap, not
                 * the last column: clamped onto it, the next character
                 * overwrote the line's last one. */
                cursor_pending_wrap = (c == new_cols);
                new_cursor_x = c < new_cols ? (i32)c : new_cols - 1;
            }
        }

        free(flat);

        if (reflow_oom) {
            /* Nothing old has been released yet: the screen, the ring and the
             * rows `phys` borrows are all intact. Take the geometry the way the
             * no-logical-lines path does (the cells that fit, no reflow) rather
             * than installing a history with lines missing from it. */
            free(out_flat);
            free(out_rows);
            free(log_start);
            free(phys);
            i32 cp_r = MIN(old_rows, new_rows);
            i32 cp_c = MIN(old_cols, new_cols);
            for (i32 y = 0; y < cp_r; y++)
                memcpy(&resize_cells[y * new_cols], &t->cells[y * old_cols],
                       (usize)cp_c * sizeof(Cell));
            free(t->cells);
            t->cells = resize_cells;
            resize_cells = NULL;
            goto finish_resize;
        }

        /*
         * Phase 5: Distribute re-wrapped lines to scrollback + screen.
         *          Keep cursor visible on screen.
         */

        /* Clear the scrollback this reflow REBUILT — everything from `sb_first`
         * on. The prefix before it was proven unable to rewrap and stays
         * exactly as it is, ring slots and compression included. With no
         * keepable prefix this clears the whole ring, slot for slot. */
        if (t->sb_ring) {
            if (sb_first <= 0) {
                for (i32 i = 0; i < t->sb_count; i++) {
                    i32 idx = (t->sb_head + i) % t->sb_capacity;
                    TermLine *ln = &t->sb_ring[idx];
                    if (ln->cold) {
                        termline_compressed_destroy(ln->cold);
                        ln->cold = NULL;
                    }
                    free(ln->cells);
                    ln->cells = NULL;
                    ln->len = 0;
                    ln->cap = 0;
                    ln->wrapped = false;
                }
                t->sb_count = 0;
                t->sb_head = 0;
                t->sb_byte_usage = 0;
            } else {
                sb_pop_newest(t, t->sb_count - sb_first);
            }
        }

        /* Determine which rows go to scrollback vs screen.
         *
         * The split is over the WHOLE history — the kept prefix plus what this
         * reflow produced — so it is computed in those coordinates and only
         * then expressed as an index into out_rows. With an empty retained
         * prefix every line below reduces to the original.
         *
         * The cursor must be visible on screen (within new_rows). */
        /* Blank rows under the cursor are room, not content. Counting them
         * pushed the TOP of a mostly empty screen into the history when the
         * window got one row shorter, and made grow-then-shrink leave a blank
         * row at the bottom. */
        i32 used_rows = out_row_count;
        while (used_rows > new_cursor_y + 1 && used_rows > 1 && out_rows[used_rows - 1].len == 0) used_rows--;
        i32 total_rows = sb_first + used_rows;
        i32 cursor_total = sb_first + new_cursor_y;
        i32 screen_start_total = total_rows - new_rows;
        if (screen_start_total < 0) screen_start_total = 0;

        /* Ensure cursor is on screen */
        if (cursor_total < screen_start_total) {
            screen_start_total = cursor_total;
        } else if (cursor_total >= screen_start_total + new_rows) {
            screen_start_total = cursor_total - new_rows + 1;
        }
        if (screen_start_total < 0) screen_start_total = 0;

        /* A screen that grew taller reaches BACK into the kept prefix. Those
         * rows are already compressed in the ring, so they are materialized
         * straight onto the new screen and then dropped from the ring — the
         * one case where the prefix is touched at all, and it is bounded by
         * new_rows rather than by the history depth. Order matters: read them
         * before popping them. */
        i32 from_prefix = sb_first - screen_start_total;
        if (from_prefix > 0) {
            if (from_prefix > new_rows) from_prefix = new_rows;
            for (i32 y = 0; y < from_prefix; y++) {
                TermLine *ln = sb_get(t, screen_start_total + y);
                if (!ln) continue;
                Cell *dst = &resize_cells[y * new_cols];
                if (ln->cold) {
                    i32 lc = ln->cold->len;
                    if (lc > new_cols) lc = new_cols;
                    if (lc > 0) termline_materialize(ln->cold, dst, lc);
                    resize_line_wrapped[y] = ln->cold->wrapped;
                } else if (ln->cells) {
                    i32 lc = MIN(ln->len, new_cols);
                    if (lc > 0) memcpy(dst, ln->cells, (usize)lc * sizeof(Cell));
                    resize_line_wrapped[y] = ln->wrapped;
                }
            }
            /* Exactly what was consumed, which is `from_prefix` AFTER the
             * clamp above and not the difference that produced it. The clamp
             * is unreachable — the split can never put more than new_rows of
             * the prefix on screen — but popping more than was materialized
             * would drop lines, and popping fewer only leaves them in the
             * history where they already are. */
            sb_pop_newest(t, from_prefix);
        }

        /* Reflow output rows are already in new_cols width; apply the budget
         * that corresponds to the new geometry before sb_push() decisions. */
        t->sb_byte_budget = scrollback_budget_for(new_cols, t->scrollback_limit);

        /* Push the produced rows that fall before the screen into scrollback,
         * behind whatever prefix survived. sb_push copies (and compresses) the
         * cells, so the packed out_flat range can stay — it is freed in bulk
         * below. Every row is pushed, including zero-length ones: sb_push
         * would trim a full-width blank row to length 0 anyway, and skipping
         * them here would swallow blank scrollback lines. */
        i32 push_count = screen_start_total - sb_first;
        if (push_count < 0) push_count = 0;
        if (push_count > out_row_count) push_count = out_row_count;
        for (i32 i = 0; i < push_count; i++) {
            sb_push(t, out_flat + (usize)out_rows[i].off,
                    out_rows[i].len, out_rows[i].wrapped);
        }

        /* Copy screen rows. resize_cells is already blank-prefilled, so a row
         * shorter than new_cols leaves exactly the blanks it was trimmed of.
         * `from_prefix` rows at the top were already filled above. */
        for (i32 y = from_prefix > 0 ? from_prefix : 0; y < new_rows; y++) {
            i32 src_idx = screen_start_total - sb_first + y;
            if (src_idx >= 0 && src_idx < out_row_count) {
                i32 copy_c = MIN(out_rows[src_idx].len, new_cols);
                if (copy_c > 0) {
                    memcpy(&resize_cells[y * new_cols],
                           out_flat + (usize)out_rows[src_idx].off,
                           (usize)copy_c * sizeof(Cell));
                }
                resize_line_wrapped[y] = out_rows[src_idx].wrapped;
            }
        }

        /* Only the current logical line was expanded; all input rows were borrowed. */
        free(out_flat);
        free(out_rows);
        free(log_start);
        free(phys);

        /* Install new screen */
        free(t->cells);
        t->cells = resize_cells;
        resize_cells = NULL;

        /* Adjust cursor */
        t->cursor_x = cursor_pending_wrap ? new_cols : new_cursor_x;
        t->cursor_y = cursor_total - screen_start_total;
        if (t->cursor_x < 0) t->cursor_x = 0;
        if (t->cursor_y < 0) t->cursor_y = 0;
        if (t->cursor_x >= new_cols) t->cursor_x = cursor_pending_wrap ? new_cols : new_cols - 1;
        if (t->cursor_y >= new_rows) t->cursor_y = new_rows - 1;
    }

    /* ---------------------------------------------------------------
     * Alt buffer: simple truncate/pad (always, for both paths)
     * --------------------------------------------------------------- */
finish_resize:
    if (t->alt_cells && resize_alt) {
        i32 cp_r = MIN(t->rows, new_rows);
        i32 cp_c = MIN(t->cols, new_cols);
        for (i32 y = 0; y < cp_r; y++)
            memcpy(&resize_alt[y * new_cols], &t->alt_cells[y * t->cols],
                   (usize)cp_c * sizeof(Cell));
        /* Its flags and zones, row for row, before the old ones are freed. */
        for (i32 y = 0; y < cp_r; y++) {
            if (t->alt_line_wrapped) resize_alt_wrapped[y] = t->alt_line_wrapped[y];
            if (t->alt_row_zones)    resize_alt_zones[y]   = t->alt_row_zones[y];
        }
        /* Frees alt_base, not the window, and clears the parked slide state —
         * resize_alt is a plain new_rows*new_cols block with no slack, and a
         * stale alt_slack would let the next scroll walk off the end of it.
         * Same discipline the live grid gets from terminal_grid_unslide above.
         * (It frees the parked flags and zones too: the new ones go in after.) */
        terminal_grid_free_alt(t);
        t->alt_cells = resize_alt;
        t->alt_line_wrapped = resize_alt_wrapped;
        t->alt_row_zones = resize_alt_zones;
        resize_alt = NULL;
        resize_alt_wrapped = NULL;
        resize_alt_zones = NULL;
    }
    free(resize_alt_wrapped);        /* only non-NULL if the branch above was not taken */
    free(resize_alt_zones);

    /* Tab stops */
    free(t->tab_stops);
    t->tab_stops = resize_tab_stops;
    resize_tab_stops = NULL;

    t->cols = new_cols;
    t->rows = new_rows;
    t->search_layout_generation++;
    t->scroll_top = 0;
    t->scroll_bottom = new_rows - 1;

    /* Width changed; refresh scrollback budgets and trim if needed. */
    terminal_set_scrollback_limit(t, t->scrollback_limit);

    /* Install the remaining arrays allocated before the resize began. */
    free(t->dirty_rows);
    t->dirty_rows = resize_dirty_rows;
    t->dirty_words = resize_dirty_words;
    resize_dirty_rows = NULL;

    free(t->row_zones);
    t->row_zones = resize_row_zones;
    resize_row_zones = NULL;

    free(t->line_wrapped);
    t->line_wrapped = resize_line_wrapped;
    resize_line_wrapped = NULL;

    /* The sparse underline maps were keyed for the old geometry. Rebuild both
     * from the stamps the cells carried through (see the stamp above). */
    terminal_clear_underline_map(t);
    terminal_underline_swap_parked(t);
    terminal_clear_underline_map(t);
    terminal_underline_swap_parked(t);
    if (ul_kinds.count > 0) {
        underline_harvest(t, t->cells, &ul_kinds);
        if (t->alt_cells) {
            terminal_underline_swap_parked(t);
            underline_harvest(t, t->alt_cells, &ul_kinds);
            terminal_underline_swap_parked(t);
        }
    }
    free(ul_kinds.kinds);

    /* Drop all inline images on resize. The reflow above rebuilds scrollback
     * from scratch (sb_count reset + re-push at the new width), so every
     * image's absolute-line anchor now points at a different line than the one
     * it was pinned to — there is no cheap, correct re-map. Clearing is the
     * safe choice (a mis-anchored image floating over unrelated text is worse
     * than a gone one); fresh output re-detects/re-emits as needed. */
    terminal_clear_images(t);

    /* Clamp cursor (a pending wrap is the one position past the last column) */
    if (t->cursor_x >= new_cols) t->cursor_x = cursor_pending_wrap ? new_cols : new_cols - 1;
    if (t->cursor_y >= new_rows) t->cursor_y = new_rows - 1;

    /* In-band resize notification (mode 2048) */
    if (t->mode & MODE_IN_BAND_RESIZE) {
        char buf[64];
        i32 n = snprintf(buf, sizeof(buf), "\x1b[48;%d;%d;0;0t", new_rows, new_cols);
        if (t->on_response) t->on_response(t, (const u8 *)buf, n, t->userdata);
    }

    TERM_DIRTY_ALL(t);
}

/* =========================================================================
 * Sparse underline map
 * ========================================================================= */

/* Mix upper row/column bits into the bucket bits. Masking the raw cell
 * index made positions one table-capacity apart collide and formed long
 * clusters at the frequently rewritten bottom row. */
static inline u32 underline_hash(u32 key) {
    key *= 0x9e3779b9u;
    return key ^ (key >> 16);
}

/* (row, col) on screen -> the entry's key. See UnderlineEntry for why the row
 * is offset. False when it does not fit (the re-key keeps the base under one
 * screenful, so that takes a grid of billions of cells). */
static inline bool underline_key(const Terminal *t, i32 col, i32 row, u32 *key) {
    u64 key64 = ((u64)(u32)row + (u64)t->ul_row_base) * (u64)(u32)t->cols + (u64)(u32)col;
    if (key64 >= UINT32_MAX) return false;
    *key = (u32)key64;
    return true;
}

void terminal_set_underline(Terminal *t, i32 col, i32 row, u32 color, u8 style) {
    if (!t || col < 0 || row < 0 || col >= t->cols || row >= t->rows) return;
    u32 key;
    if (!underline_key(t, col, row, &key)) return;
    /* A single underline in the cell's own colour is what the flag already
     * says: the renderer draws an underlined cell with no entry as exactly
     * that. Storing it would put one entry per underlined cell into the map,
     * and every screenful of scrolling re-keys all of them (underline_rebuild),
     * which makes colored output that uses `ESC[4m` expensive to parse. Treated
     * as "no override", so a cell that had a styled entry still loses it. */
    if (color == 0 && style == 1) style = 0;
    if (color == 0 && style == 0) {
        /* Reset an existing override without allocating or growing a table.
         * Keep the key occupied so later entries' probe chains stay intact. */
        if (!t->ul_map || t->ul_map_count <= 0 || t->ul_map_cap <= 0) return;
        u32 mask = (u32)(t->ul_map_cap - 1);
        u32 h = underline_hash(key) & mask;
        for (i32 probe = 0; probe < t->ul_map_cap; probe++) {
            UnderlineEntry *entry = &t->ul_map[h];
            if (entry->key == key) {
                entry->underline_color = 0;
                entry->underline_style = 0;
                return;
            }
            if (entry->key == 0xFFFFFFFF) return;
            h = (h + 1) & mask;
        }
        return;
    }

    /* Lazy allocate */
    if (!t->ul_map) {
        t->ul_map_cap = UL_MAP_CAP;
        t->ul_map = calloc((usize)t->ul_map_cap, sizeof(UnderlineEntry));
        if (!t->ul_map) return;
        for (i32 i = 0; i < t->ul_map_cap; i++)
            t->ul_map[i].key = 0xFFFFFFFF;
        t->ul_map_count = 0;
    }

    u32 mask = (u32)(t->ul_map_cap - 1);

    u32 h = underline_hash(key) & mask;
    while (t->ul_map[h].key != 0xFFFFFFFF && t->ul_map[h].key != key)
        h = (h + 1) & mask;

    /* An existing key only changes its value, even near the load/cap limit.
     * Growth is needed only when consuming another slot. */
    if (t->ul_map[h].key == 0xFFFFFFFF &&
        (i64)t->ul_map_count * 10 > (i64)t->ul_map_cap * 7) {
        if (t->ul_map_cap <= 0 ||
            t->ul_map_cap >= TERM_UNDERLINE_MAP_MAX_CAP) return;
        i32 new_cap = t->ul_map_cap * 2;
        UnderlineEntry *new_map = calloc((usize)new_cap, sizeof(UnderlineEntry));
        if (!new_map) return;
        for (i32 i = 0; i < new_cap; i++) new_map[i].key = 0xFFFFFFFF;
        u32 new_mask = (u32)(new_cap - 1);
        for (i32 i = 0; i < t->ul_map_cap; i++) {
            if (t->ul_map[i].key != 0xFFFFFFFF) {
                u32 h = underline_hash(t->ul_map[i].key) & new_mask;
                while (new_map[h].key != 0xFFFFFFFF) h = (h + 1) & new_mask;
                new_map[h] = t->ul_map[i];
            }
        }
        free(t->ul_map);
        t->ul_map = new_map;
        t->ul_map_cap = new_cap;
        mask = new_mask;
        h = underline_hash(key) & mask;
        while (t->ul_map[h].key != 0xFFFFFFFF) h = (h + 1) & mask;
    }

    if (t->ul_map[h].key == 0xFFFFFFFF) t->ul_map_count++;
    t->ul_map[h].key = key;
    t->ul_map[h].underline_color = color;
    t->ul_map[h].underline_style = style;
}

void terminal_get_underline(Terminal *t, i32 col, i32 row, u32 *color, u8 *style) {
    if (!color || !style) return;
    *color = 0;
    *style = 0;
    if (!t || col < 0 || row < 0 || col >= t->cols || row >= t->rows ||
        !t->ul_map || t->ul_map_count <= 0 || t->ul_map_cap <= 0) return;
    u32 key;
    if (!underline_key(t, col, row, &key)) return;
    u32 mask = (u32)(t->ul_map_cap - 1);
    u32 h = underline_hash(key) & mask;
    for (i32 probe = 0; probe < t->ul_map_cap; probe++) {
        if (t->ul_map[h].key == key) {
            *color = t->ul_map[h].underline_color;
            *style = t->ul_map[h].underline_style;
            return;
        }
        if (t->ul_map[h].key == 0xFFFFFFFF) return;
        h = (h + 1) & mask;
    }
}

void terminal_clear_underline_map(Terminal *t) {
    if (t->ul_map) {
        for (i32 i = 0; i < t->ul_map_cap; i++)
            t->ul_map[i].key = 0xFFFFFFFF;
        t->ul_map_count = 0;
    }
    t->ul_row_base = 0;
}

/* What moved, for underline_rebuild. Rows of [top, bot] go up by `up` (negative
 * = down); the cells of `row` from column `from` go right by `right` (negative
 * = left). Either half may be a no-op. */
typedef struct {
    i32 top, bot, up;
    i32 row, from, right;
} UnderlineMove;

/* Re-key every entry to row base 0, applying `mv`. Entries of rows that are no
 * longer on screen are dropped, and so are the "no override" ones a reset
 * leaves behind to keep probe chains intact — a rebuild has no chains to keep.
 * An entry whose cell left its region, or fell off either end of its row, is
 * gone with the cell. */
static void underline_rebuild(Terminal *t, const UnderlineMove *mv) {
    i32 cap = t->ul_map_cap;
    if (!t->ul_map || cap <= 0 || t->cols <= 0) { t->ul_row_base = 0; return; }
    UnderlineEntry on_stack[256];
    UnderlineEntry *live = on_stack;
    if (t->ul_map_count > (i32)(sizeof on_stack / sizeof on_stack[0])) {
        live = malloc((usize)t->ul_map_count * sizeof(UnderlineEntry));
        /* Without room to move them the entries would describe other cells.
         * Dropping them costs the style and colour, not the underline. */
        if (!live) { terminal_clear_underline_map(t); return; }
    }
    u32 cols = (u32)t->cols, base = t->ul_row_base;
    i32 n = 0;
    for (i32 i = 0; i < cap; i++) {
        UnderlineEntry e = t->ul_map[i];
        if (e.key == 0xFFFFFFFF) continue;
        t->ul_map[i].key = 0xFFFFFFFF;
        if (e.underline_color == 0 && e.underline_style == 0) continue;
        i64 row = (i64)(e.key / cols) - (i64)base;
        i64 col = (i64)(e.key % cols);
        if (row < 0 || row >= t->rows) continue;
        if (mv && mv->up != 0 && row >= mv->top && row <= mv->bot) {
            row -= mv->up;
            if (row < mv->top || row > mv->bot) continue;
        }
        if (mv && mv->right != 0 && row == mv->row && col >= mv->from) {
            /* Moving left deletes the cells the others close over. */
            if (mv->right < 0 && col < (i64)mv->from - mv->right) continue;
            col += mv->right;
            if (col >= (i64)cols) continue;
        }
        e.key = (u32)row * cols + (u32)col;
        live[n++] = e;
    }
    t->ul_row_base = 0;
    t->ul_map_count = n;
    u32 mask = (u32)(cap - 1);
    for (i32 k = 0; k < n; k++) {
        u32 h = underline_hash(live[k].key) & mask;
        while (t->ul_map[h].key != 0xFFFFFFFF) h = (h + 1) & mask;
        t->ul_map[h] = live[k];
    }
    if (live != on_stack) free(live);
}

void terminal_underline_scroll(Terminal *t, i32 top, i32 bot, i32 n) {
    if (!t || t->ul_map_count <= 0 || n == 0) return;
    if (n > 0 && top <= 0 && bot >= t->rows - 1) {
        /* The whole screen moved up: every surviving cell keeps its virtual
         * row, so nothing is touched. The rows that left are swept once a
         * screenful of them has gone — which is also what lets the count fall
         * back to zero, and this function back to its first line. */
        t->ul_row_base += (u32)n;
        if (t->ul_row_base >= (u32)t->rows) underline_rebuild(t, NULL);
        return;
    }
    UnderlineMove mv = {.top = top, .bot = bot, .up = n, .row = -1};
    underline_rebuild(t, &mv);
}

void terminal_underline_shift_cols(Terminal *t, i32 row, i32 from, i32 n) {
    if (!t || t->ul_map_count <= 0 || n == 0 || row < 0 || row >= t->rows) return;
    UnderlineMove mv = {.top = 0, .bot = -1, .up = 0, .row = row, .from = from, .right = n};
    underline_rebuild(t, &mv);
}

void terminal_underline_swap_parked(Terminal *t) {
    if (!t) return;
    UnderlineEntry *m = t->ul_map; t->ul_map       = t->ul_alt_map;   t->ul_alt_map   = m;
    i32 c = t->ul_map_cap;         t->ul_map_cap   = t->ul_alt_cap;   t->ul_alt_cap   = c;
    i32 k = t->ul_map_count;       t->ul_map_count = t->ul_alt_count; t->ul_alt_count = k;
    u32 b = t->ul_row_base;        t->ul_row_base  = t->ul_alt_base;  t->ul_alt_base  = b;
}

void terminal_underline_drop_parked(Terminal *t) {
    if (!t) return;
    free(t->ul_alt_map);
    t->ul_alt_map = NULL;
    t->ul_alt_cap = t->ul_alt_count = 0;
    t->ul_alt_base = 0;
}

/* --- Across a resize ------------------------------------------------------
 *
 * The reflow rewraps every logical line, trims, and re-derives which rows are
 * screen and which are history; where a cell ends up is known only to it. So
 * the entries do not try to follow: each is stamped into its own cell, the
 * reflow moves the cells as it always has — bitwise — and the map is rebuilt
 * from wherever the stamps came to rest.
 *
 * The stamp is CellAttr._pad, which is dead (no part of a cell's value; see
 * attr_run_eq in buffer.c). It holds an index into the (style, colour) pairs
 * in use, lives only inside terminal_resize, and cannot outlast it: the
 * harvest zeroes every stamp on the new grids, and a row that went to history
 * was compressed on the way, where attr_run_make drops the field. */
/* More distinct pairs than this on one screen is a colour gradient; past it
 * the rest keep their underline and lose the colour, rather than turning the
 * stamp into a quadratic search. */
#define UL_RESIZE_MAX_KINDS 1024

static u16 underline_kind_intern(UnderlineKinds *tb, u32 color, u8 style) {
    for (i32 i = tb->count - 1; i >= 0; i--)
        if (tb->kinds[i].color == color && tb->kinds[i].style == style) return (u16)(i + 1);
    if (tb->count >= UL_RESIZE_MAX_KINDS) return 0;
    if (tb->count == tb->cap) {
        i32 cap = tb->cap ? tb->cap * 2 : 16;
        UnderlineKind *grown = realloc(tb->kinds, (usize)cap * sizeof(UnderlineKind));
        if (!grown) return 0;
        tb->kinds = grown; tb->cap = cap;
    }
    tb->kinds[tb->count] = (UnderlineKind){color, style};
    return (u16)(++tb->count);
}

/* Stamp the LIVE map's entries into `grid` (cols x rows, the geometry the map
 * is keyed for). */
static void underline_stamp(Terminal *t, Cell *grid, i32 cols, i32 rows, UnderlineKinds *tb) {
    if (!t->ul_map || t->ul_map_count <= 0 || !grid || cols <= 0) return;
    u32 ucols = (u32)cols, base = t->ul_row_base;
    for (i32 i = 0; i < t->ul_map_cap; i++) {
        const UnderlineEntry *e = &t->ul_map[i];
        if (e->key == 0xFFFFFFFF) continue;
        if (e->underline_color == 0 && e->underline_style == 0) continue;
        i64 row = (i64)(e->key / ucols) - (i64)base;
        u32 col = e->key % ucols;
        if (row < 0 || row >= rows) continue;
        Cell *c = &grid[(usize)row * (usize)cols + col];
        if (!(c->attr.flags & ATTR_UNDERLINE)) continue;
        c->attr._pad = underline_kind_intern(tb, e->underline_color, e->underline_style);
    }
}

/* Rebuild the LIVE map from the stamps in `grid` (t->cols x t->rows by now),
 * leaving no stamp behind. */
static void underline_harvest(Terminal *t, Cell *grid, const UnderlineKinds *tb) {
    if (!grid) return;
    for (i32 row = 0; row < t->rows; row++) {
        Cell *line = &grid[(usize)row * (usize)t->cols];
        for (i32 col = 0; col < t->cols; col++) {
            u16 tag = line[col].attr._pad;
            if (!tag) continue;
            line[col].attr._pad = 0;
            if (tag > tb->count || !(line[col].attr.flags & ATTR_UNDERLINE)) continue;
            terminal_set_underline(t, col, row, tb->kinds[tag - 1].color, tb->kinds[tag - 1].style);
        }
    }
}

/* =========================================================================
 * Scrollback navigation
 * ========================================================================= */

/* Clamp a wanted offset to one the viewport can actually show.
 *
 * On the PRIMARY screen every offset in [0, sb_count] is a coherent picture:
 * terminal_visible_row resolves each row from the live grid or from scrollback
 * and the two are the same timeline.
 *
 * On the ALT screen they are NOT. Nothing is ever pushed to scrollback there,
 * so `sb_count` still counts the lines the SHELL left behind before the
 * full-screen program took over — a different buffer entirely. An offset
 * between 1 and rows-1 therefore paints part of the screen from that old
 * history and the rest from the live alt grid: at offset 2 on a 6-row terminal,
 * rows 0-1 show the shell's last history lines while rows 2-4 hold the
 * program's UI, shifted down two rows with its bottom status line pushed off
 * the screen — and the program keeps painting its own absolute rows underneath,
 * unaware.
 *
 * Scrolling still has to WORK there: Shift+wheel is the way back into the
 * history an agent CLI covered up. So the offset snaps past the mixed band
 * instead of being forbidden — the first scroll up lands on a full screen of
 * history, and coming back down returns to the program. With less than a
 * screenful of scrollback behind it there is no clean view to snap to, so it
 * stays put. */
static i32 scroll_offset_snap(const Terminal *t, i32 want, bool going_up) {
    if (want < 0) want = 0;
    if (want > t->sb_count) want = t->sb_count;
    if (!(t->mode & MODE_ALT_SCREEN)) return want;
    if (want == 0 || want >= t->rows) return want;
    return going_up && t->sb_count >= t->rows ? t->rows : 0;
}

/* Move the viewport, and mark the screen only if it actually moved.
 *
 * The dirty-all belongs to the MOVE: every visible row now resolves to a
 * different line, so nothing the row cache holds is reusable. When the offset
 * lands where it already was there is no such claim to make, and marking
 * anyway is a full-grid rebuild that produces the picture already on screen.
 *
 * That is not a rare edge. app_send_input calls terminal_scroll_to_bottom on
 * EVERY keystroke ("typing returns the view to the live bottom"), and the view
 * is almost always already at the bottom — so marking anyway would rebuild
 * every row for each typed character and produce a frame identical to the one
 * before it: a keystroke changes nothing on screen until the ECHO arrives back
 * from the pty, which is a later frame. Keystroke frames bypass the present
 * grid (had_latency_event) and so are not held to the panel rate; under load
 * that surplus is what pushes app_render into -nextDrawable, where the main
 * thread can neither read input nor drain a pty.
 *
 * The wheel paths take the same test for the same reason: a notch at either
 * end of the scrollback clamps to the offset it already had. */
static void scroll_set_offset(Terminal *t, i32 want) {
    if (t->scroll_offset == want) return;
    t->scroll_offset = want;
    TERM_DIRTY_ALL(t);
}

void terminal_scroll_up(Terminal *t, i32 lines) {
    scroll_set_offset(t, scroll_offset_snap(t, t->scroll_offset + lines, true));
}

void terminal_scroll_down(Terminal *t, i32 lines) {
    scroll_set_offset(t, scroll_offset_snap(t, t->scroll_offset - lines, false));
}

void terminal_scroll_to_bottom(Terminal *t) {
    scroll_set_offset(t, 0);
}

void terminal_select_start(Terminal *t, i32 col, i32 row) {
    selection_start(t, col, row, 1, false);
}
void terminal_select_update(Terminal *t, i32 col, i32 row) {
    selection_update(t, col, row);
}
char *terminal_select_text(Terminal *t) {
    return selection_get_text(t);
}
void terminal_select_clear(Terminal *t) {
    selection_clear(t);
}

/* Liu keyboard modifier encoding: 1 + shift(1) + alt(2) + ctrl(4) + super(8) */
static i32 Liu_mods(u32 mods) {
    i32 m = 1;
    if (mods & MOD_SHIFT) m += 1;
    if (mods & MOD_ALT)   m += 2;
    if (mods & MOD_CTRL)  m += 4;
    if (mods & MOD_SUPER) m += 8;
    return m;
}

/* Modifier encoding for modifyOtherKeys CSI 27 format.
 * Same encoding as Liu: 1 + shift(1) + alt(2) + ctrl(4) + meta(8) */
static i32 xterm_mods(u32 mods) {
    return Liu_mods(mods);  /* same bit layout */
}

/* snprintf returns the length it WOULD have written; on truncation that exceeds
 * out_size, and terminal_char_input's caller sends exactly that many bytes to
 * the PTY — reading past the (small, stack) `out` buffer. Clamp to the bytes
 * that actually fit so the result never reports more than was written. */
static inline i32 clamp_emit(i32 n, i32 out_size) {
    if (n < 0 || out_size <= 0) return 0;
    return n < out_size ? n : out_size - 1;
}

/* Encode a key as CSI 27 ; modifier ; keycode ~ (modifyOtherKeys format) */
static i32 encode_modify_other_keys(u32 codepoint, u32 mods, u8 *out, i32 out_size) {
    i32 m = xterm_mods(mods);
    return clamp_emit(snprintf((char *)out, (usize)out_size, "\x1b[27;%d;%u~", m, codepoint), out_size);
}

/* Map KeyCode to Unicode codepoint for Liu protocol */
/* The codepoint a key carries in the CSI-u form, or 0 for "this key has no
 * CSI-u form — use its legacy escape code".
 *
 * The protocol assigns Private Use Area numbers ONLY to keys with no legacy
 * escape code of their own: CAPS_LOCK 57358, SCROLL_LOCK 57359, NUM_LOCK
 * 57360, PRINT_SCREEN 57361, PAUSE 57362, MENU 57363, F13 57376 upwards.
 * Everything else keeps its legacy form under every flag combination,
 * report-all-keys included — the spec states a non-text key comes out as
 * `CSI number;modifier u` OR `CSI 1;modifier [~ABCDEFHPQS]`, and its
 * functional table spells those out: Insert `2~`, Delete `3~`, Left `1D`,
 * Right `1C`, Up `1A`, Down `1B`, PageUp `5~`, PageDown `6~`, Home `1H`, End
 * `1F`, F1 `1P`, F5 `15~`, F12 `24~`.
 *
 * Returning 0 drops those keys through to the legacy switch below, which
 * emits exactly the forms the table prescribes. Numbers of its own for them
 * would collide with the spec's (Page Up at 57358 arrives as Caps Lock) or
 * with each other, and no two distinct keys may encode identically. The
 * CSI-u path reaches these keys only when flag 0x08 (report all keys as
 * escape codes) is set. */
static u32 key_to_unicode(u32 key) {
    if (key >= KEY_A && key <= KEY_Z) return 'a' + (key - KEY_A);
    if (key >= KEY_0 && key <= KEY_9) return '0' + (key - KEY_0);
    switch (key) {
    case KEY_ENTER: return 13; case KEY_TAB: return 9;
    case KEY_BACKSPACE: return 127; case KEY_ESCAPE: return 27;
    case KEY_SPACE: return 32;
    default: return 0;
    }
}

/* =========================================================================
 * Key input → escape sequence encoding
 * ========================================================================= */

i32 terminal_key_input(Terminal *t, u32 key, u32 mods, u8 *out, i32 out_size) {
    i32 n = 0;
    bool app = (t->mode & MODE_APP_CURSOR) != 0;

    /* Cmd+⌫ / Cmd+⌦ / Cmd+← / Cmd+→ are macOS text-field shortcuts, and they stay that
     * way under every keyboard protocol.
     *
     * On macOS a Cmd chord is not forwarded to the program, so no TUI binds one.
     * Encoding them as protocol events therefore hands the app something it has no
     * binding for and the keystroke is simply dropped: opencode turns on
     * modifyOtherKeys level 1 (CSI>4;1m) at startup, where Cmd+⌫ would go out as CSI
     * 27;9;127~ and delete nothing, and codex turns on enhanced keyboard flags and
     * would get CSI 127;9u.
     *
     * The line-editing control byte works in all of them: it is what the legacy path
     * below already sends, and the agents document ^U/^K/^A/^E as their own
     * line-editing keys. This must run BEFORE the enhanced and modifyOtherKeys
     * encoders, which would otherwise claim the key first. */
    if (mods & MOD_SUPER) {
        u8 macos_edit = 0;
        switch (key) {
        case KEY_BACKSPACE: macos_edit = 0x15; break;  /* ^U kill to line start */
        case KEY_DELETE:    macos_edit = 0x0b; break;  /* ^K kill to line end   */
        case KEY_LEFT:      macos_edit = 0x01; break;  /* ^A start of line      */
        case KEY_RIGHT:     macos_edit = 0x05; break;  /* ^E end of line        */
        default: break;
        }
        if (macos_edit && out_size >= 1) {
            out[0] = macos_edit;
            return 1;
        }
    }

    /* Liu keyboard protocol: encode keys with CSI-u format.
     *
     * IMPORTANT: under flag 0x01 ("disambiguate") *only*, printable keys
     * with no non-shift modifier MUST go through the legacy text path
     * (CHAR_INPUT → terminal_char_input → plain ASCII byte). Emitting
     * CSI-u here AND letting CHAR_INPUT also fire would double every
     * character in any tool that enables disambiguate mode.
     *
     * Only emit CSI-u when the protocol actually requires it:
     *   - 0x08 (report-all-keys-as-escape) is set, OR
     *   - the key carries a non-shift modifier (Ctrl/Alt/Meta), OR
     *   - the key is special (no plain-text representation). The
     *     unicode mapping uses Private-Use-Area codepoints ≥ 0xE000
     *     (57344) for those keys (arrows, F-keys, …). */
    if (t->Liu_kbd_flags != 0) {
        u32 ucp = key_to_unicode(key);
        if (ucp > 0) {
            bool report_all     = (t->Liu_kbd_flags & 0x08) != 0;
            bool has_real_mod   = (mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) != 0;
            bool is_special_key = (ucp >= 0xE000);
            /* Special keys (arrows, F-keys, Home/End/Page, …) own a legacy
             * escape code: CSI A, CSI 1;mods A, or SS3 (ESC O A) in app-cursor
             * mode. The protocol keeps emitting those legacy codes UNLESS
             * "report all keys as escape codes" (flag 0x08) is set — only then
             * do they switch to the functional CSI <codepoint> u form. Emitting
             * CSI-u for an arrow under mere "disambiguate" (flag 0x01, what most
             * TUIs enable) makes apps that expect CSI A — including codex —
             * ignore the arrow keys entirely. So use CSI-u here only when
             * report_all is set, or for a NON-special key carrying a real
             * modifier (e.g. Ctrl+letter under disambiguate); modified and plain
             * special keys fall through to the legacy switch below. */
            bool has_shift = (mods & MOD_SHIFT) != 0;
            bool is_control_char = (ucp > 0 && ucp < 0x20);
            /* Esc is the first key "disambiguate" exists for: a bare ESC byte
             * is also how every escape sequence begins. */
            bool esc_disambiguated = (ucp == 27 && (t->Liu_kbd_flags & 0x01));
            /* A digit with Shift alone is a symbol on every layout, and which
             * symbol only the text path knows (it is sent from there): saying
             * `CSI 49;2u` as well sent Shift+1 twice under report-all. */
            bool shifted_digit = (ucp >= '0' && ucp <= '9' && has_shift && !has_real_mod);
            if (!shifted_digit &&
                (report_all || (has_real_mod && !is_special_key) ||
                 (has_shift && is_control_char) || esc_disambiguated)) {
                i32 km = Liu_mods(mods);
                if (km > 1)
                    n = snprintf((char *)out, (usize)out_size, "\x1b[%u;%du", ucp, km);
                else
                    n = snprintf((char *)out, (usize)out_size, "\x1b[%uu", ucp);
                if (n > 0) return clamp_emit(n, out_size);
            }
            /* else: fall through. Plain printable keys go out as the legacy
             * ASCII byte via CHAR_INPUT; special keys use the legacy switch. */
        }
    }

    /* modifyOtherKeys: encode modified special keys with CSI 27 format.
     * Level 1: modified function/special keys; Level 2: all modified keys.
     * Liu keyboard takes priority (checked above). */
    if (t->modify_other_keys >= 1 && t->Liu_kbd_flags == 0) {
        i32 xm = xterm_mods(mods);
        if (xm > 1) {
            /* Map special keys to their Unicode codepoint for CSI 27 encoding */
            u32 cp = 0;
            /* Shift ALONE on Tab and Backspace has a code of its own (CSI Z,
             * DEL) that level 1 leaves in place, and Shift+Space is a space,
             * which the text path sends — encoding it here as well sent the
             * escape AND the space. */
            bool shift_only = (mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) == 0;
            switch (key) {
            case KEY_ENTER:     cp = 13; break;
            case KEY_TAB:       if (!(shift_only && t->modify_other_keys == 1)) cp = 9; break;
            case KEY_BACKSPACE: if (!(shift_only && t->modify_other_keys == 1)) cp = 127; break;
            case KEY_ESCAPE:    cp = 27; break;
            case KEY_SPACE:     if (!shift_only) cp = 32; break;
            default: break;
            }
            if (cp > 0) {
                n = encode_modify_other_keys(cp, mods, out, out_size);
                if (n > 0) return n;
            }
        }
    }

    /* Modifier parameter: 1+shift(1)+alt(2)+ctrl(4)+meta(8)
     * When modifier > 1, arrow/nav keys use: ESC [ 1 ; mod X
     * Function keys use: ESC [ code ; mod ~    */
    i32 mod_param = 1;
    if (mods & MOD_SHIFT) mod_param += 1;
    if (mods & MOD_ALT)   mod_param += 2;
    if (mods & MOD_CTRL)  mod_param += 4;
    if (mods & MOD_SUPER) mod_param += 8;
    bool has_mod = (mod_param > 1);

    /* The Option/Alt line-editing maps (Option on macOS, Alt/Ctrl on Linux) only
     * apply on the pure-legacy path. In CSI-u (Liu_kbd_flags) or modifyOtherKeys
     * mode the agent TUI receives the disambiguated event and runs its own line
     * editing — for modified ARROWS especially, the protocol path falls through
     * to the legacy switch under "disambiguate" (flag 0x01), so this guard is
     * what keeps the maps from clobbering the CSI modified-arrow such TUIs
     * expect. Alt IS a modifier TUIs bind, so it stays protocol-first; Cmd/Super
     * is not, which is why it is mapped unconditionally at the top. */
    bool legacy_kbd = (t->Liu_kbd_flags == 0 && t->modify_other_keys == 0);

    #define EMIT(s) do { \
        i32 sl = (i32)strlen(s); \
        if (n + sl <= out_size) { memcpy(out + n, s, (usize)sl); n += sl; } \
    } while(0)
    #define EMIT_ARROW(letter) do { \
        if (has_mod) { \
            n = snprintf((char *)out, (usize)out_size, "\x1b[1;%d" letter, mod_param); \
        } else { \
            EMIT(app ? "\x1bO" letter : "\x1b[" letter); \
        } \
    } while(0)
    /* Home and End are cursor keys: under DECCKM they are SS3 like the arrows
     * (terminfo's khome / kend after smkx are \EOH / \EOF). They ignored the
     * mode, so an ncurses program did not recognise them. */
    #define EMIT_NAV(code, letter) do { \
        if (has_mod) { \
            n = snprintf((char *)out, (usize)out_size, "\x1b[1;%d" letter, mod_param); \
        } else { \
            EMIT(app ? "\x1bO" letter : "\x1b[" letter); \
        } \
    } while(0)
    #define EMIT_FUNC(code) do { \
        if (has_mod) { \
            n = snprintf((char *)out, (usize)out_size, "\x1b[" code ";%d~", mod_param); \
        } else { \
            EMIT("\x1b[" code "~"); \
        } \
    } while(0)

    switch (key) {
        /* Alt is Meta on the legacy path: an ESC before the key's own byte,
         * as for Backspace below and for every letter (xterm's
         * metaSendsEscape). Enter, Tab and Esc get it too: sent bare,
         * Option+Enter would SUBMIT where every agent CLI reads ESC CR as
         * "new line". Under a keyboard protocol these never get here. */
        case KEY_ENTER:
            if (legacy_kbd && (mods & MOD_ALT)) EMIT("\x1b");
            EMIT("\r");
            break;
        case KEY_BACKSPACE:
            /* Cmd+Backspace already returned ^U at the top of the function. */
            if (legacy_kbd && (mods & (MOD_ALT | MOD_CTRL))) {
                /* Option+Backspace (macOS) / Ctrl+Backspace (Linux) →
                 * ESC DEL (backward-kill-word in readline/zle) */
                EMIT("\x1b\x7f");
            } else {
                EMIT("\x7f");
            }
            break;
        case KEY_TAB:
            if (legacy_kbd && (mods & MOD_ALT)) EMIT("\x1b");
            if (mods & MOD_SHIFT) { EMIT("\x1b[Z"); }
            else { EMIT("\t"); }
            break;
        case KEY_ESCAPE:
            if (legacy_kbd && (mods & MOD_ALT)) EMIT("\x1b");
            EMIT("\x1b");
            break;

        /* Arrow keys — with modifier: ESC[1;modX */
        case KEY_UP:        EMIT_ARROW("A"); break;
        case KEY_DOWN:      EMIT_ARROW("B"); break;
        case KEY_RIGHT:
            /* Cmd+Right already returned ^E at the top of the function. */
            if (legacy_kbd && (mods & (MOD_ALT | MOD_CTRL))) {
                EMIT("\x1b" "f");      /* Option/Ctrl+Right → ESC f (forward-word) */
            } else {
                EMIT_ARROW("C");
            }
            break;
        case KEY_LEFT:
            /* Cmd+Left already returned ^A at the top of the function. */
            if (legacy_kbd && (mods & (MOD_ALT | MOD_CTRL))) {
                EMIT("\x1b" "b");      /* Option/Ctrl+Left → ESC b (backward-word) */
            } else {
                EMIT_ARROW("D");
            }
            break;

        /* Home/End — with modifier: ESC[1;modH / ESC[1;modF */
        case KEY_HOME:      EMIT_NAV(1, "H"); break;
        case KEY_END:       EMIT_NAV(1, "F"); break;

        /* Page Up/Down, Insert, Delete — with modifier: ESC[code;mod~ */
        case KEY_PAGE_UP:   EMIT_FUNC("5"); break;
        case KEY_PAGE_DOWN: EMIT_FUNC("6"); break;
        case KEY_INSERT:    EMIT_FUNC("2"); break;
        case KEY_DELETE:
            /* Cmd+ForwardDelete already returned ^K at the top of the function. */
            if (legacy_kbd && (mods & (MOD_ALT | MOD_CTRL))) {
                /* Option/Ctrl+ForwardDelete → ESC d (kill-word forward) */
                EMIT("\x1b" "d");
            } else if (has_mod) {
                n = snprintf((char *)out, (usize)out_size, "\x1b[3;%d~", mod_param);
            } else {
                EMIT("\x1b[3~");
            }
            break;

        /* Function keys — with modifier: ESC[code;mod~ or ESC[1;modP etc. */
        case KEY_F1:
            if (has_mod) n = snprintf((char *)out, (usize)out_size, "\x1b[1;%dP", mod_param);
            else EMIT("\x1bOP");
            break;
        case KEY_F2:
            if (has_mod) n = snprintf((char *)out, (usize)out_size, "\x1b[1;%dQ", mod_param);
            else EMIT("\x1bOQ");
            break;
        case KEY_F3:
            if (has_mod) n = snprintf((char *)out, (usize)out_size, "\x1b[1;%dR", mod_param);
            else EMIT("\x1bOR");
            break;
        case KEY_F4:
            if (has_mod) n = snprintf((char *)out, (usize)out_size, "\x1b[1;%dS", mod_param);
            else EMIT("\x1bOS");
            break;
        case KEY_F5:  EMIT_FUNC("15"); break;
        case KEY_F6:  EMIT_FUNC("17"); break;
        case KEY_F7:  EMIT_FUNC("18"); break;
        case KEY_F8:  EMIT_FUNC("19"); break;
        case KEY_F9:  EMIT_FUNC("20"); break;
        case KEY_F10: EMIT_FUNC("21"); break;
        case KEY_F11: EMIT_FUNC("23"); break;
        case KEY_F12: EMIT_FUNC("24"); break;
        default: break;
    }
    #undef EMIT
    #undef EMIT_ARROW
    #undef EMIT_NAV
    #undef EMIT_FUNC
    /* Every snprintf above returns the length it WOULD have written; the
     * caller sends exactly `n` bytes, so a truncated sequence must not be
     * reported longer than the buffer (clamp_emit, as on every other path
     * in this file). */
    return clamp_emit(n, out_size);
}

i32 terminal_char_input(Terminal *t, u32 codepoint, u32 mods, u8 *out, i32 out_size) {
    /* Job-control suspend must remain a real SUB byte even when an app has
     * enabled CSI-u keyboard reporting. Otherwise the PTY line
     * discipline never sees VSUSP and foreground TUIs don't suspend.
     */
    if ((mods & MOD_CTRL) && !(mods & (MOD_ALT | MOD_SUPER)) &&
        (codepoint == 'z' || codepoint == 'Z')) {
        if (out_size >= 1) {
            out[0] = 0x1a;
            return 1;
        }
        return 0;
    }

    /* Liu keyboard protocol: encode all keys with modifiers */
    if (t->Liu_kbd_flags & 0x08) { /* report all keys as escapes */
        /* One physical keypress arrives HERE and at terminal_key_input both:
         * platform_macos.m's keyDown: pushes EVENT_KEY_DOWN and then lets the
         * input manager produce EVENT_CHAR_INPUT for the same key. Exactly one
         * of the two may put bytes on the wire.
         *
         * Under plain disambiguate that is already arranged from the other
         * side — terminal_key_input declines a printable key with no real
         * modifier, and this path sends the text. Under report-all-keys the
         * ownership flips: the key path encodes EVERY key it has a code point
         * for, so this one has to stand down for the same set, or the
         * character goes out twice — `CSI 97u` from the key path plus a bare
         * `a` from here.
         *
         * The set is exactly key_to_unicode's: ASCII letters (either case —
         * Shift+a reaches here as 'A' with mods already folded in), digits,
         * space, and the four C0 keys. Punctuation has no KEY_ code, so the
         * key path emits nothing for it and it must still be sent from here.
         *
         * Gated on 0x08 (report all keys), which codex, for one, does not
         * request (it asks for `CSI > 7 u`). */
        bool key_path_owns_it =
            (codepoint >= 'a' && codepoint <= 'z') ||
            (codepoint >= 'A' && codepoint <= 'Z') ||
            (codepoint >= '0' && codepoint <= '9') ||
            codepoint == ' ' || codepoint == 13 || codepoint == 9 ||
            codepoint == 27 || codepoint == 127;
        /* Ctrl+letter is the exception: the platform delivers it only here,
         * with no key event at all, so standing down would send NOTHING and
         * Ctrl+C would be dead in a program that asked for report-all. */
        if (key_path_owns_it && !(mods & MOD_CTRL)) return 0;
        i32 km = Liu_mods(mods);
        if (km > 1) {
            return clamp_emit(snprintf((char *)out, (usize)out_size, "\x1b[%u;%du", codepoint, km), out_size);
        }
    }

    /* Ctrl+key combinations */
    if ((mods & MOD_CTRL) && codepoint >= 'a' && codepoint <= 'z') {
        if (t->Liu_kbd_flags & 0x01) { /* disambiguate */
            i32 km = Liu_mods(mods);
            return clamp_emit(snprintf((char *)out, (usize)out_size, "\x1b[%u;%du", codepoint, km), out_size);
        }
        /* modifyOtherKeys level 2: encode Ctrl+letter as CSI 27 instead of ^A-^Z */
        if (t->modify_other_keys >= 2 && t->Liu_kbd_flags == 0) {
            return encode_modify_other_keys(codepoint, mods, out, out_size);
        }
        /* modifyOtherKeys level 1: Ctrl+letter is ambiguous, encode it */
        if (t->modify_other_keys == 1 && t->Liu_kbd_flags == 0) {
            return encode_modify_other_keys(codepoint, mods, out, out_size);
        }
        if (out_size >= 1) {
            out[0] = (u8)(codepoint - 'a' + 1);
            return 1;
        }
        return 0;
    }
    if ((mods & MOD_CTRL) && codepoint >= 'A' && codepoint <= 'Z') {
        if (t->Liu_kbd_flags & 0x01) {
            i32 km = Liu_mods(mods);
            return clamp_emit(snprintf((char *)out, (usize)out_size, "\x1b[%u;%du", codepoint + 32, km), out_size);
        }
        /* modifyOtherKeys: encode Ctrl+LETTER as CSI 27 with lowercase codepoint */
        if (t->modify_other_keys >= 1 && t->Liu_kbd_flags == 0) {
            return encode_modify_other_keys(codepoint + 32, mods, out, out_size);
        }
        if (out_size >= 1) {
            out[0] = (u8)(codepoint - 'A' + 1);
            return 1;
        }
        return 0;
    }

    /* modifyOtherKeys level 2: encode ALL modified keys when any modifier
     * (other than Shift alone producing a different character) is held.
     * Liu keyboard takes priority (checked above). */
    if (t->modify_other_keys >= 2 && t->Liu_kbd_flags == 0) {
        bool has_ctrl = (mods & MOD_CTRL) != 0;
        bool has_alt  = (mods & MOD_ALT) != 0;
        bool has_meta = (mods & MOD_SUPER) != 0;
        /* Shift alone just produces the shifted character, no special encoding */
        if (has_ctrl || has_alt || has_meta) {
            return encode_modify_other_keys(codepoint, mods, out, out_size);
        }
    }

    /* modifyOtherKeys level 1: only encode keys that would be ambiguous
     * (function keys with modifiers are handled in terminal_key_input;
     *  this handles Alt+key, which is ambiguous with ESC+key) */
    if (t->modify_other_keys == 1 && t->Liu_kbd_flags == 0) {
        if (mods & MOD_ALT) {
            return encode_modify_other_keys(codepoint, mods, out, out_size);
        }
    }

    /* Alt+key → ESC prefix (standard terminal Alt behavior)
     * Only for ASCII characters (codepoint < 0x80) — when macOS Option key
     * produces a special Unicode character (like ∫ for Opt+B), the codepoint
     * is >= 0x80 and should be sent as-is without ESC prefix.
     * When option_as_alt is enabled, the platform sends the raw ASCII character
     * (e.g., 'b' for Opt+B), so codepoint < 0x80 and the ESC is added.
     *
     * This makes readline/zsh word navigation work:
     *   Alt+B → ESC b (word backward)
     *   Alt+F → ESC f (word forward)
     *   Alt+D → ESC d (delete word forward)
     *   Alt+. → ESC . (insert last argument)
     */
    if ((mods & MOD_ALT) && !(mods & MOD_CTRL) && !(mods & MOD_SUPER) && codepoint < 0x80) {
        if (out_size >= 2) {
            out[0] = 0x1b; /* ESC */
            out[1] = (u8)codepoint;
            return 2;
        }
        return 0;
    }

    /* UTF-8 encode via shared utility. utf8_encode writes up to 4 bytes and
     * takes no capacity, so the contract is checked here rather than by every
     * caller: a buffer that cannot hold the longest sequence gets nothing, not
     * a partial code point. */
    if (out_size < 4) return 0;
    u32 len = utf8_encode(codepoint, out);
    return (i32)len;
}

/* =========================================================================
 * Inline image management
 * ========================================================================= */

void terminal_clear_images(Terminal *t) {
    if (!t) return;
    t->image_epoch++;
    if (!t->images) return;
    usize released = t->images_byte_usage;
    for (i32 i = 0; i < MAX_TERM_IMAGES; i++) {
        if (t->images[i].valid && t->images[i].pixels) {
            terminal_image_free_or_retain(t, &t->images[i]);
        }
        t->images[i].pixels = NULL;
        t->images[i].valid = false;
        t->images[i].serial = 0;
        t->images[i].last_used_sec = 0.0;
    }
    t->image_count = 0;
    t->images_byte_usage = 0;
    if (released) mem_budget_adjust(MEM_IMAGE_CPU, -(isize)released);
}

void terminal_delete_image(Terminal *t, u32 image_id) {
    if (!t || !t->images) return;
    for (i32 i = 0; i < MAX_TERM_IMAGES; i++) {
        if (t->images[i].valid && t->images[i].image_id == image_id) {
            usize bytes = (usize)t->images[i].width *
                          (usize)t->images[i].height * 4u;
            terminal_image_free_or_retain(t, &t->images[i]);
            t->images[i].pixels = NULL;
            t->images[i].valid = false;
            t->images[i].serial = 0;
            t->images[i].last_used_sec = 0.0;
            if (t->images_byte_usage > bytes) t->images_byte_usage -= bytes;
            else t->images_byte_usage = 0;
            if (bytes != SIZE_MAX) mem_budget_adjust(MEM_IMAGE_CPU, -(isize)bytes);
            t->image_count--;
            TERM_DIRTY_ALL(t);
            /* Every placement of the id goes, not the first one found. */
        }
    }
}

/* Per-terminal inline image memory cap. Inline image payloads can claim up
 * to 4096x4096x4B (64 MiB) each; with MAX_TERM_IMAGES=32 that's ~2 GiB of
 * resident RAM per terminal if a hostile remote sprays images. Aggregate
 * bytes are capped by TERM_IMAGES_BUDGET_BYTES (terminal.h), and the
 * least-recently-used slot is evicted until each new image fits. */

static inline usize term_image_pixel_bytes(const TermImage *img) {
    if (!img || !img->valid) return 0;
    i32 w = img->width  > 0 ? img->width  : 0;
    i32 h = img->height > 0 ? img->height : 0;
    usize pixels;
    usize bytes;
    if (!checked_mul_usize((usize)w, (usize)h, &pixels) ||
        !checked_mul_usize(pixels, 4u, &bytes)) {
        return SIZE_MAX;
    }
    return bytes;
}

/* Return the slot whose last_used_sec is smallest (least recently used).
 * Falls back to image_id ordering when timestamps are identical, so the
 * eviction order stays deterministic before any render touches a slot. */
static i32 term_image_lru_slot(const Terminal *t) {
    if (!t->images) return -1;
    i32 best = -1;
    f64 best_ts = 0.0;
    u32 best_id = 0;
    for (i32 i = 0; i < MAX_TERM_IMAGES; i++) {
        if (!t->images[i].valid) continue;
        f64 ts = t->images[i].last_used_sec;
        u32 id = t->images[i].image_id;
        if (best < 0 || ts < best_ts ||
            (ts == best_ts && id < best_id)) {
            best = i;
            best_ts = ts;
            best_id = id;
        }
    }
    return best;
}

/* Drop a single slot's pixels and update the byte accounting. */
static void term_image_release_slot(Terminal *t, i32 slot) {
    if (!t->images || slot < 0 || slot >= MAX_TERM_IMAGES) return;
    if (!t->images[slot].valid) return;
    usize bytes = term_image_pixel_bytes(&t->images[slot]);
    if (t->images[slot].pixels) terminal_image_free_or_retain(t, &t->images[slot]);
    t->images[slot].pixels = NULL;
    t->images[slot].valid = false;
    t->images[slot].serial = 0;
    t->images[slot].last_used_sec = 0.0;
    if (t->images_byte_usage > bytes) t->images_byte_usage -= bytes;
    else t->images_byte_usage = 0;
    if (bytes != SIZE_MAX) mem_budget_adjust(MEM_IMAGE_CPU, -(isize)bytes);
    t->image_count--;
}

/* Never reset, never per-Terminal. See the assignment in terminal_add_image. */
static _Atomic u64 g_image_serial_next;

i32 terminal_add_image(Terminal *t, u8 *pixels, i32 w, i32 h,
                       i32 col, i32 row, i32 cols, i32 rows,
                       u32 image_id, u32 placement_id) {
    if (!pixels) return -1;
    if (!t || w <= 0 || h <= 0) {
        free(pixels);
        return -1;
    }

    usize pixel_count;
    usize incoming_bytes;
    if (!checked_mul_usize((usize)w, (usize)h, &pixel_count) ||
        !checked_mul_usize(pixel_count, 4u, &incoming_bytes)) {
        free(pixels);
        return -1;
    }
    /* Reject images larger than the whole budget outright */
    if (incoming_bytes > TERM_IMAGES_BUDGET_BYTES) {
        free(pixels);
        return -1;
    }

    /* Lazy-allocate the slot pool on first image — terminals that never
     * display inline graphics keep their MAX_TERM_IMAGES * sizeof(TermImage)
     * (~1.8 KB) off the resident set. */
    if (!t->images) {
        t->images = calloc(MAX_TERM_IMAGES, sizeof(*t->images));
        if (!t->images) {
            free(pixels);
            return -1;
        }
    }

    /* An image id names ONE image per placement. Transmitting an id+placement
     * that is already on screen replaces it (the protocol's own rule) instead
     * of adding a second slot that a delete by id then half removes. Another
     * placement of the same id is a different slot and stays. Id 0 means
     * "unassigned" and never matches. */
    if (image_id != 0) {
        for (i32 i = 0; i < MAX_TERM_IMAGES; i++) {
            if (t->images[i].valid && t->images[i].image_id == image_id &&
                t->images[i].placement_id == placement_id)
                term_image_release_slot(t, i);
        }
    }

    /* Evict LRU slots until the new image fits within the byte budget. */
    while (t->images_byte_usage > TERM_IMAGES_BUDGET_BYTES - incoming_bytes) {
        i32 victim = term_image_lru_slot(t);
        if (victim < 0) break;
        term_image_release_slot(t, victim);
    }

    /* Find a free slot */
    i32 slot = -1;
    for (i32 i = 0; i < MAX_TERM_IMAGES; i++) {
        if (!t->images[i].valid) {
            slot = i;
            break;
        }
    }

    /* If no free slot, evict the LRU */
    if (slot < 0) {
        slot = term_image_lru_slot(t);
        if (slot < 0) slot = 0;
        term_image_release_slot(t, slot);
    }

    t->images[slot].pixels = pixels;
    t->images[slot].width = w;
    t->images[slot].height = h;
    t->images[slot].col = col;
    t->images[slot].row = row;
    /* Pin to the absolute scrollback line so the image scrolls with its text
     * and drops out once that line is evicted. Alt-screen images never reach
     * scrollback, so anchor them to the fixed grid `row` (abs_line = -1). */
    t->images[slot].abs_line = (t->mode & MODE_ALT_SCREEN)
        ? -1
        : t->sb_abs_base + (i64)t->sb_count + (i64)row;
    t->images[slot].cols = cols;
    t->images[slot].rows = rows;
    t->images[slot].image_id = image_id;
    t->images[slot].placement_id = placement_id;
    /* PROCESS-WIDE, not per Terminal.
     *
     * The renderer's texture cache is keyed on the ADDRESS of this slot plus
     * this serial. A per-Terminal counter would restart at zero for every
     * Terminal, and the allocator readily hands a new Terminal the address a
     * dead one had, so the pair would repeat and the cache would hit on a
     * different image.
     *
     * A single monotone counter makes the serial unique on its own, so the pair
     * cannot repeat however the allocator behaves. Atomic because images are
     * placed from the parser and from the decode worker; relaxed because
     * uniqueness needs no ordering against anything else. u64 at one image per
     * nanosecond is 584 years. */
    t->images[slot].serial = atomic_fetch_add_explicit(&g_image_serial_next, 1,
                                                       memory_order_relaxed) + 1;
    t->images[slot].z_index = 0;
    t->images[slot].last_used_sec = platform_time_sec();
    t->images[slot].valid = true;
    t->image_count++;
    t->images_byte_usage += incoming_bytes;
    mem_budget_adjust(MEM_IMAGE_CPU, (isize)incoming_bytes);
    TERM_DIRTY_ALL(t);
    return slot;
}

/* =========================================================================
 * Shell integration: prompt navigation
 * ========================================================================= */

void terminal_goto_prev_prompt(Terminal *t) {
    if (!t || !t->row_zones) return;
    i32 cur = t->cursor_y - 1;
    while (cur >= 0) {
        if (t->row_zones[cur] == ZONE_PROMPT) {
            t->cursor_y = cur;
            TERM_DIRTY_ALL(t);
            return;
        }
        cur--;
    }
}

void terminal_goto_next_prompt(Terminal *t) {
    if (!t || !t->row_zones) return;
    i32 cur = t->cursor_y + 1;
    while (cur < t->rows) {
        if (t->row_zones[cur] == ZONE_PROMPT) {
            t->cursor_y = cur;
            TERM_DIRTY_ALL(t);
            return;
        }
        cur++;
    }
}

SemanticZone terminal_row_zone(Terminal *t, i32 row) {
    if (!t || !t->row_zones || row < 0 || row >= t->rows)
        return ZONE_NONE;
    return t->row_zones[row];
}

/* =========================================================================
 * Palette initialization (OSC 4/10/11/12)
 * ========================================================================= */

/* Standard ANSI colors (0-7: normal, 8-15: bright) — only the 16-color base
 * is needed inside terminal.c; the 6x6x6 cube and grayscale ramp are
 * deterministic and resolved on demand by terminal_palette_color() if a
 * caller ever looks past index 15 without a per-instance OSC 4 override. */
static const u32 LIU_ANSI16[16] = {
    0x000000, 0xCC0000, 0x00CC00, 0xCCCC00,
    0x0000CC, 0xCC00CC, 0x00CCCC, 0xCCCCCC,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

void terminal_init_palette(Terminal *t) {
    if (!t) return;

    /* The full 256-entry palette is dead weight unless OSC 4 ever modifies
     * it. Skip the per-instance allocation here; resolve default_fg/bg/cursor
     * directly from the global LUT. If OSC 4 hits later, the writer will
     * lazy-allocate t->palette and seed it from this same default table. */
    t->default_fg   = LIU_ANSI16[7];   /* white */
    t->default_bg   = LIU_ANSI16[0];   /* black */
    t->cursor_color = LIU_ANSI16[7];   /* white */
    t->palette_modified = false;
    /* t->palette stays NULL until first OSC 4 write. */
}

/* ---- OSC 4 / OSC 104 -----------------------------------------------------
 *
 * The writers of the per-terminal palette. An OSC 4 lands in this Terminal's
 * own table, never in the process-global LUT, so it repaints no other window.
 *
 * An entry is an OVERRIDE: 0 means "not set, follow the theme", anything else
 * carries bit 24 the way a truecolor cell attribute does. So black is
 * 0x01000000 and unset is 0x00000000, and a program can legitimately set
 * index 0 to black without that reading as a reset. */
static struct { bool known, ansi_known; u32 fg, bg, cursor; u32 ansi[16]; } g_theme_colors;

void terminal_theme_colors_set(u32 fg_rgb, u32 bg_rgb, u32 cursor_rgb) {
    g_theme_colors.fg     = fg_rgb & 0xFFFFFFu;
    g_theme_colors.bg     = bg_rgb & 0xFFFFFFu;
    g_theme_colors.cursor = cursor_rgb & 0xFFFFFFu;
    g_theme_colors.known  = true;
}

void terminal_theme_palette_set(const u32 shown[16]) {
    if (!shown) return;
    for (i32 i = 0; i < 16; i++) g_theme_colors.ansi[i] = shown[i] & 0xFFFFFFu;
    g_theme_colors.ansi_known = true;
}

void terminal_theme_colors_forget(void) {
    g_theme_colors.known = false;
    g_theme_colors.ansi_known = false;
}

bool terminal_theme_index_color(i32 idx, u32 *rgb) {
    if (!rgb || !g_theme_colors.ansi_known || idx < 0 || idx > 255) return false;
    *rgb = idx < 16 ? g_theme_colors.ansi[idx] : palette256_rgb(idx);
    return true;
}

bool terminal_theme_color(i32 code, u32 *rgb) {
    if (!g_theme_colors.known || !rgb) return false;
    switch (code) {
        case 10: *rgb = g_theme_colors.fg;     return true;
        case 11: *rgb = g_theme_colors.bg;     return true;
        case 12: *rgb = g_theme_colors.cursor; return true;
    }
    return false;
}

bool terminal_theme_is_dark(bool *dark) {
    if (!g_theme_colors.known || !dark) return false;
    u32 bg = g_theme_colors.bg;
    /* Perceived brightness of the background, 0-255: the split every "is this
     * terminal dark?" probe uses. */
    u32 y = (299u * ((bg >> 16) & 0xFFu) + 587u * ((bg >> 8) & 0xFFu) + 114u * (bg & 0xFFu)) / 1000u;
    *dark = y < 128u;
    return true;
}

void terminal_palette_set(Terminal *t, i32 idx, u32 rgb) {
    if (!t || idx < 0 || idx > 255) return;
    if (!t->palette) {
        t->palette = calloc(256, sizeof(u32));
        if (!t->palette) return;         /* out of memory: keep the theme */
        /* No ledger post: terminal_grid_bytes() and terminal_owned_bytes()
         * both already count this table on demand, which is how every other
         * side table here is accounted. */
    }
    u32 v = 0x01000000u | (rgb & 0x00FFFFFFu);
    if (t->palette[idx] == v) return;    /* a repaint nobody needs */
    t->palette[idx] = v;
    t->palette_modified = true;
}

void terminal_palette_reset(Terminal *t, i32 idx) {
    if (!t || !t->palette) return;
    if (idx < 0) {
        for (i32 i = 0; i < 256; i++) t->palette[i] = 0;
        t->palette_modified = true;
        return;
    }
    if (idx > 255 || t->palette[idx] == 0) return;
    t->palette[idx] = 0;
    t->palette_modified = true;
}

bool terminal_palette_get(const Terminal *t, i32 idx, u32 *out_rgb) {
    if (!t || !t->palette || idx < 0 || idx > 255) return false;
    u32 v = t->palette[idx];
    if (!v) return false;
    if (out_rgb) *out_rgb = v & 0x00FFFFFFu;
    return true;
}
