/*
 * Liu - terminal state management + key input encoding
 */
#include "terminal/terminal.h"
#include "core/utf8.h"
#include "platform/platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define TERM_SCROLLBACK_HARD_MAX_LINES 20000
#define TERM_SCROLLBACK_MIN_BUDGET_BYTES ((usize)(512u * 1024u))
/* Per-terminal scrollback cap. With 32 tabs * 2 split panes the worst-case
 * footprint is 32 * 2 * cap; lowered from 64 MiB so even a maxed-out window
 * stays under ~1 GiB. */
#define TERM_SCROLLBACK_MAX_BUDGET_BYTES ((usize)(16u * 1024u * 1024u))

static inline i32 content_len_trimmed(const Cell *cells, i32 len) {
    while (len > 0 && cells[len - 1].codepoint == ' ' &&
           !(cells[len - 1].attr.flags & (ATTR_WIDE | ATTR_WDUMMY)))
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
        terminal_free_scrollback(t);
        TERM_DIRTY_ALL(t);
        return;
    }

    terminal_refresh_scrollback_usage(t);
    while (t->sb_ring &&
           (t->sb_count > limit ||
            (t->sb_byte_budget > 0 && t->sb_byte_usage > t->sb_byte_budget))) {
        TermLine *oldest = &t->sb_ring[t->sb_head];
        usize old_bytes = oldest->cold
                        ? termline_compressed_bytes(oldest->cold)
                        : scrollback_line_bytes(oldest->cap);
        if (oldest->cold) {
            termline_compressed_destroy(oldest->cold);
            oldest->cold = NULL;
        }
        free(oldest->cells);
        oldest->cells = NULL;
        oldest->len = 0;
        oldest->cap = 0;
        oldest->wrapped = false;
        if (t->sb_byte_usage > old_bytes) t->sb_byte_usage -= old_bytes;
        else t->sb_byte_usage = 0;
        t->sb_head = (t->sb_head + 1) % t->sb_capacity;
        t->sb_count--;
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

Terminal *terminal_create(i32 cols, i32 rows) {
    Terminal *t = calloc(1, sizeof(Terminal));
    if (!t) return NULL;

    t->cols = cols;
    t->rows = rows;
    t->cells = calloc((usize)cols * (usize)rows, sizeof(Cell));
    if (!t->cells) { terminal_destroy(t); return NULL; }
    t->alt_cells = NULL;

    /* Default attributes */
    t->cursor_attr.fg = FG_DEFAULT;
    t->cursor_attr.bg = BG_DEFAULT;
    t->cursor_visible = true;
    t->cursor_style   = CURSOR_BLOCK;

    /* Inline agent-image detection is OFF by default and switched ON per-frame
     * by the UI only while a known AI agent owns the foreground (see
     * app_poll_sessions). That keeps plain shell output like `ls` — which can
     * list image filenames — from being rendered as pictures.
     * cell_px_* stay 0 until the first render syncs real font metrics. */
    t->inline_image_detect = false;

    /* Scroll region = full screen */
    t->scroll_top    = 0;
    t->scroll_bottom = rows - 1;

    /* Mode defaults */
    t->mode = MODE_WRAP;

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
    for (i32 i = 0; i < cols * rows; i++) {
        t->cells[i] = blank;
    }

    /* allocate multi-word dirty bitmap */
    t->dirty_words = (rows + 63) / 64;
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
    t->output_start_row = -1;
    t->last_cmd_failed = false;

    /* Initialize 256-color palette + default colors */
    terminal_init_palette(t);

    TERM_DIRTY_ALL(t);
    return t;
}

void terminal_destroy(Terminal *t) {
    if (!t) return;
    free(t->cells);
    free(t->alt_cells);
    free(t->tab_stops);
    free(t->ul_map);
    free(t->title_stack);
    free(t->hyperlink_uri);
    free(t->cwd);
    free(t->icon_name);
    free(t->search.matches);
    free(t->dirty_rows);
    free(t->apc_buf);
    free(t->dcs_buf);
    free(t->line_wrapped);
    free(t->row_zones);
    free(t->pending_command);
    free(t->palette);
    free(t->sb_scratch);
    /* Free inline images (per-pixel) then the slot pool itself. */
    terminal_clear_images(t);
    free(t->images);
    t->images = NULL;
    free(t->pending_img);   /* a decoded-but-not-yet-placed inline image */
    t->pending_img = NULL;
    free(t->pending_img_path);
    t->pending_img_path = NULL;
    terminal_free_scrollback(t);
    free(t);
}

/* External scrollback helpers from buffer.c */
extern void sb_push(Terminal *t, Cell *row_cells, i32 len, bool wrapped);
extern TermLine *sb_get(Terminal *t, i32 index);
extern void buffer_scroll_up(Terminal *t, i32 n);

/* Forward decl: defined below the inline-image helpers but referenced by
 * terminal_resize() when invalidating out-of-bounds image slots. */
static inline usize term_image_pixel_bytes(const TermImage *img);

void terminal_resize(Terminal *t, i32 new_cols, i32 new_rows) {
    if (new_cols == t->cols && new_rows == t->rows) return;
    if (new_cols < 1 || new_rows < 1) return;

    i32 old_cols = t->cols;
    i32 old_rows = t->rows;
    Cell blank = { .codepoint = ' ', .attr = t->cursor_attr };

    /* ---------------------------------------------------------------
     * Alt screen: simple truncate/pad (no reflow)
     * --------------------------------------------------------------- */
    if (t->mode & MODE_ALT_SCREEN) {
        Cell *new_cells = calloc((usize)(new_cols * new_rows), sizeof(Cell));
        if (!new_cells) return;
        for (i32 i = 0; i < new_cols * new_rows; i++) new_cells[i] = blank;
        i32 cp_rows = MIN(old_rows, new_rows);
        i32 cp_cols = MIN(old_cols, new_cols);
        for (i32 y = 0; y < cp_rows; y++)
            memcpy(&new_cells[y * new_cols], &t->cells[y * old_cols],
                   (usize)cp_cols * sizeof(Cell));
        free(t->cells);
        t->cells = new_cells;
        goto finish_resize;
    }

    /* ---------------------------------------------------------------
     * Primary screen: 5-phase reflow algorithm
     * --------------------------------------------------------------- */
    {
        /*
         * Phase 1: Build logical lines from scrollback + screen
         *          by chaining soft-wrapped rows.
         *
         * A "logical line" = a sequence of physical rows where each row
         * (except the last) was soft-wrapped (line_wrapped flag is true).
         */
        i32 total_phys = t->sb_count + old_rows;

        /* Collect all physical rows into a flat array of (Cell*, len, wrapped).
         * `owned_cells` records which entries point to per-row heap buffers we
         * malloc'd here for cold scrollback rows; we free them at end_resize. */
        typedef struct { Cell *cells; i32 len; bool wrapped; bool from_sb; bool owned; } PhysRow;

        PhysRow *phys = calloc((usize)total_phys, sizeof(PhysRow));
        if (!phys) {
            /* Allocation failed: fall back to simple truncate/pad */
            Cell *new_cells = calloc((usize)(new_cols * new_rows), sizeof(Cell));
            if (!new_cells) return;
            for (i32 i = 0; i < new_cols * new_rows; i++) new_cells[i] = blank;
            i32 cp_r = MIN(old_rows, new_rows);
            i32 cp_c = MIN(old_cols, new_cols);
            for (i32 y = 0; y < cp_r; y++)
                memcpy(&new_cells[y * new_cols], &t->cells[y * old_cols],
                       (usize)cp_c * sizeof(Cell));
            free(t->cells);
            t->cells = new_cells;
            goto finish_resize;
        }

        /* Fill from scrollback. Cold rows are materialized into their own
         * heap buffer for the duration of the reflow — sl->cells is NULL on
         * the cold path so we cannot share storage across rows. */
        for (i32 i = 0; i < t->sb_count; i++) {
            TermLine *sl = sb_get(t, i);
            if (!sl) continue;
            if (sl->cold) {
                i32 lc = sl->cold->len;
                if (lc > 0) {
                    Cell *buf = malloc((usize)lc * sizeof(Cell));
                    if (buf) {
                        i32 wrote = termline_materialize(sl->cold, buf, lc);
                        phys[i].cells = buf;
                        phys[i].len   = wrote;
                        phys[i].owned = true;
                    }
                }
                phys[i].wrapped = sl->cold->wrapped;
            } else {
                phys[i].cells   = sl->cells;
                phys[i].len     = sl->len;
                phys[i].wrapped = sl->wrapped;
            }
            phys[i].from_sb = true;
        }
        /* Fill from screen */
        for (i32 y = 0; y < old_rows; y++) {
            i32 idx = t->sb_count + y;
            phys[idx].cells   = &t->cells[y * old_cols];
            phys[idx].len     = old_cols;
            phys[idx].wrapped = (t->line_wrapped && y < old_rows - 1) ? t->line_wrapped[y] : false;
            phys[idx].from_sb = false;
        }

        /* Build logical lines: find where each one starts.
         * A row starts a new logical line if it is the first row, or
         * the *previous* row was NOT wrapped (i.e. ended with a newline). */
        i32 *log_start = calloc((usize)total_phys, sizeof(i32));  /* start phys index of each logical line */
        i32 log_count = 0;
        if (log_start) {
            for (i32 i = 0; i < total_phys; i++) {
                if (i == 0 || !phys[i].wrapped) {
                    log_start[log_count++] = i;
                }
            }
        } else {
            /* Can't build logical lines, fall through with no reflow */
            for (i32 i = 0; i < total_phys; i++) {
                if (phys[i].owned) free(phys[i].cells);
            }
            free(phys);
            Cell *new_cells = calloc((usize)(new_cols * new_rows), sizeof(Cell));
            if (!new_cells) return;
            for (i32 i = 0; i < new_cols * new_rows; i++) new_cells[i] = blank;
            i32 cp_r = MIN(old_rows, new_rows);
            i32 cp_c = MIN(old_cols, new_cols);
            for (i32 y = 0; y < cp_r; y++)
                memcpy(&new_cells[y * new_cols], &t->cells[y * old_cols],
                       (usize)cp_c * sizeof(Cell));
            free(t->cells);
            t->cells = new_cells;
            goto finish_resize;
        }

        /*
         * Phase 2: Count physical rows after re-wrapping to new_cols.
         *          For each logical line, compute how many new physical rows
         *          it will occupy when wrapped to new_cols.
         */
        /* Helper: compute effective content length (trim trailing spaces) */
        #define CONTENT_LEN(cells, len) content_len_trimmed((cells), (len))

        /* For each logical line, gather total cells */
        i32 new_total_phys = 0;
        i32 *log_new_rows = calloc((usize)log_count, sizeof(i32));
        if (!log_new_rows) {
            free(log_start);
            for (i32 i = 0; i < total_phys; i++) {
                if (phys[i].owned) free(phys[i].cells);
            }
            free(phys);
            /* Fallback */
            Cell *new_cells = calloc((usize)(new_cols * new_rows), sizeof(Cell));
            if (!new_cells) return;
            for (i32 i = 0; i < new_cols * new_rows; i++) new_cells[i] = blank;
            i32 cp_r = MIN(old_rows, new_rows);
            i32 cp_c = MIN(old_cols, new_cols);
            for (i32 y = 0; y < cp_r; y++)
                memcpy(&new_cells[y * new_cols], &t->cells[y * old_cols],
                       (usize)cp_c * sizeof(Cell));
            free(t->cells);
            t->cells = new_cells;
            goto finish_resize;
        }

        for (i32 li = 0; li < log_count; li++) {
            i32 start = log_start[li];
            i32 end = (li + 1 < log_count) ? log_start[li + 1] : total_phys;

            /* Count total non-trailing-space cells in this logical line */
            i32 total_cells = 0;
            for (i32 pi = start; pi < end; pi++) {
                if (pi == end - 1) {
                    /* Last physical row: trim trailing spaces */
                    total_cells += CONTENT_LEN(phys[pi].cells, phys[pi].len);
                } else {
                    total_cells += phys[pi].len;
                }
            }

            i32 nrows = (total_cells > 0) ? ((total_cells + new_cols - 1) / new_cols) : 1;
            log_new_rows[li] = nrows;
            new_total_phys += nrows;
        }

        /*
         * Phase 3: Map cursor position through logical lines.
         *          Find which logical line the cursor is in, and its offset.
         */
        i32 cursor_phys_idx = t->sb_count + t->cursor_y;
        i32 cursor_log = -1;
        i32 cursor_cell_offset = -1;

        for (i32 li = 0; li < log_count; li++) {
            i32 start = log_start[li];
            i32 end = (li + 1 < log_count) ? log_start[li + 1] : total_phys;
            if (cursor_phys_idx >= start && cursor_phys_idx < end) {
                cursor_log = li;
                /* Calculate cell offset within the logical line */
                cursor_cell_offset = 0;
                for (i32 pi = start; pi < cursor_phys_idx; pi++) {
                    cursor_cell_offset += phys[pi].len;
                }
                cursor_cell_offset += t->cursor_x;
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
         * Previously each output row malloc'd its own `new_cols`-sized cell
         * buffer — so a 2000-line scrollback reflow produced 2000 tiny
         * allocations. We now back every row with a slice of one contiguous
         * block (`out_flat`), and `out_rows[i].cells = out_flat + i*new_cols`.
         * One calloc, zero per-row frees, strictly better locality. Same with
         * `flat_cap` — the per-logical-line scratch that we used to malloc/free
         * once per logical line. We keep one high-water-mark buffer and grow
         * it only when a longer logical line appears. */
        typedef struct { Cell *cells; i32 len; bool wrapped; } OutRow;
        OutRow *out_rows = calloc((usize)out_cap, sizeof(OutRow));
        Cell   *out_flat = calloc((usize)out_cap * (usize)new_cols, sizeof(Cell));
        if (!out_rows || !out_flat) {
            free(out_rows);
            free(out_flat);
            free(log_new_rows);
            free(log_start);
            for (i32 i = 0; i < total_phys; i++) {
                if (phys[i].owned) free(phys[i].cells);
            }
            free(phys);
            Cell *new_cells = calloc((usize)(new_cols * new_rows), sizeof(Cell));
            if (!new_cells) return;
            for (i32 i = 0; i < new_cols * new_rows; i++) new_cells[i] = blank;
            free(t->cells);
            t->cells = new_cells;
            goto finish_resize;
        }
        /* Prefill the flat block with blanks; rows just index into it. */
        for (i32 i = 0; i < out_cap * new_cols; i++) out_flat[i] = blank;
        for (i32 i = 0; i < out_cap; i++) {
            out_rows[i].cells = out_flat + (usize)i * (usize)new_cols;
            out_rows[i].len   = new_cols;
        }

        i32   out_row_count = 0;
        i32   new_cursor_x = 0, new_cursor_y = 0;
        Cell *flat = NULL;
        i32   flat_cap = 0;

        for (i32 li = 0; li < log_count; li++) {
            i32 start = log_start[li];
            i32 end = (li + 1 < log_count) ? log_start[li + 1] : total_phys;

            /* Flatten the logical line's cells */
            i32 total_cells = 0;
            for (i32 pi = start; pi < end; pi++) {
                if (pi == end - 1)
                    total_cells += CONTENT_LEN(phys[pi].cells, phys[pi].len);
                else
                    total_cells += phys[pi].len;
            }

            /* If empty, emit one blank row (slice is already blank) */
            if (total_cells == 0) {
                if (out_row_count < out_cap) {
                    out_rows[out_row_count].wrapped = false;
                }
                /* Map cursor */
                if (li == cursor_log) {
                    new_cursor_y = out_row_count;
                    new_cursor_x = 0;
                }
                out_row_count++;
                continue;
            }

            /* Grow the shared scratch buffer on demand; one allocation across
             * all logical lines instead of malloc/free per line. */
            if (total_cells > flat_cap) {
                Cell *grown = realloc(flat, (usize)total_cells * sizeof(Cell));
                if (!grown) {
                    /* Skip this logical line on alloc failure — emit a blank
                     * row (slice is already blanked). */
                    if (out_row_count < out_cap) {
                        out_rows[out_row_count].wrapped = false;
                    }
                    out_row_count++;
                    continue;
                }
                flat = grown;
                flat_cap = total_cells;
            }

            i32 fi = 0;
            for (i32 pi = start; pi < end; pi++) {
                i32 copy_len = (pi == end - 1) ? CONTENT_LEN(phys[pi].cells, phys[pi].len) : phys[pi].len;
                if (copy_len > 0) {
                    memcpy(&flat[fi], phys[pi].cells, (usize)copy_len * sizeof(Cell));
                    fi += copy_len;
                }
            }

            /* Re-wrap into rows of new_cols */
            i32 nrows = log_new_rows[li];
            i32 first_out = out_row_count;
            i32 src = 0;

            for (i32 r = 0; r < nrows && out_row_count < out_cap; r++) {
                Cell *row = out_rows[out_row_count].cells;
                /* Slice already prefilled with blanks */

                i32 col = 0;
                while (col < new_cols && src < total_cells) {
                    /* Check for wide character at end of row */
                    if ((flat[src].attr.flags & ATTR_WIDE) && col == new_cols - 1) {
                        /* Wide char won't fit — leave this cell blank, wrap to next row */
                        row[col] = blank;
                        break;
                    }
                    row[col] = flat[src];
                    /* Skip WDUMMY following a WIDE */
                    if (flat[src].attr.flags & ATTR_WIDE) {
                        col++;
                        src++;
                        if (col < new_cols && src < total_cells &&
                            (flat[src].attr.flags & ATTR_WDUMMY)) {
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

                bool is_wrapped = (r < nrows - 1);
                out_rows[out_row_count].wrapped = is_wrapped;
                out_row_count++;
            }

            /* Map cursor position */
            if (li == cursor_log && cursor_cell_offset >= 0) {
                i32 cy_in_log = cursor_cell_offset / new_cols;
                i32 cx_in_log = cursor_cell_offset % new_cols;
                if (cy_in_log >= nrows) {
                    cy_in_log = nrows - 1;
                    cx_in_log = new_cols - 1;
                }
                new_cursor_y = first_out + cy_in_log;
                new_cursor_x = cx_in_log;
            }
        }

        free(flat);

        #undef CONTENT_LEN

        /*
         * Phase 5: Distribute re-wrapped lines to scrollback + screen.
         *          Keep cursor visible on screen.
         */

        /* Clear old scrollback (both hot and cold storage). */
        if (t->sb_ring) {
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
        }

        /* Determine which rows go to scrollback vs screen.
         * The cursor must be visible on screen (within new_rows). */
        i32 screen_start = out_row_count - new_rows;
        if (screen_start < 0) screen_start = 0;

        /* Reflow output rows are already in new_cols width; apply the budget
         * that corresponds to the new geometry before sb_push() decisions. */
        t->sb_byte_budget = scrollback_budget_for(new_cols, t->scrollback_limit);

        /* Ensure cursor is on screen */
        if (new_cursor_y < screen_start) {
            screen_start = new_cursor_y;
        } else if (new_cursor_y >= screen_start + new_rows) {
            screen_start = new_cursor_y - new_rows + 1;
        }
        if (screen_start < 0) screen_start = 0;

        /* Push rows before screen_start into scrollback. sb_push copies the
         * cells, so the underlying out_flat slice can stay — it will be freed
         * in bulk below. */
        for (i32 i = 0; i < screen_start; i++) {
            if (out_rows[i].cells) {
                sb_push(t, out_rows[i].cells, out_rows[i].len, out_rows[i].wrapped);
            }
        }

        /* Allocate new screen buffer */
        Cell *new_cells = calloc((usize)(new_cols * new_rows), sizeof(Cell));
        if (!new_cells) {
            /* Cleanup — single free for the whole slab. */
            for (i32 i = 0; i < total_phys; i++) {
                if (phys[i].owned) free(phys[i].cells);
            }
            free(out_flat);
            free(out_rows);
            free(log_new_rows);
            free(log_start);
            free(phys);
            return;
        }
        for (i32 i = 0; i < new_cols * new_rows; i++) new_cells[i] = blank;

        /* Copy screen rows */
        bool *new_line_wrapped = calloc((usize)new_rows, sizeof(bool));
        for (i32 y = 0; y < new_rows; y++) {
            i32 src_idx = screen_start + y;
            if (src_idx < out_row_count && out_rows[src_idx].cells) {
                i32 copy_c = MIN(out_rows[src_idx].len, new_cols);
                memcpy(&new_cells[y * new_cols], out_rows[src_idx].cells,
                       (usize)copy_c * sizeof(Cell));
                if (new_line_wrapped)
                    new_line_wrapped[y] = out_rows[src_idx].wrapped;
            }
        }

        /* Single bulk free for all output row cells. Per-row cold materialize
         * buffers (phys[i].owned) are freed here too. */
        for (i32 i = 0; i < total_phys; i++) {
            if (phys[i].owned) free(phys[i].cells);
        }
        free(out_flat);
        free(out_rows);
        free(log_new_rows);
        free(log_start);
        free(phys);

        /* Install new screen */
        free(t->cells);
        t->cells = new_cells;
        free(t->line_wrapped);
        t->line_wrapped = new_line_wrapped;

        /* Adjust cursor */
        t->cursor_x = new_cursor_x;
        t->cursor_y = new_cursor_y - screen_start;
        if (t->cursor_x < 0) t->cursor_x = 0;
        if (t->cursor_y < 0) t->cursor_y = 0;
        if (t->cursor_x >= new_cols) t->cursor_x = new_cols - 1;
        if (t->cursor_y >= new_rows) t->cursor_y = new_rows - 1;
    }

    /* ---------------------------------------------------------------
     * Alt buffer: simple truncate/pad (always, for both paths)
     * --------------------------------------------------------------- */
finish_resize:
    if (t->alt_cells) {
        Cell *new_alt = calloc((usize)(new_cols * new_rows), sizeof(Cell));
        if (new_alt) {
            Cell bk = { .codepoint = ' ', .attr = t->cursor_attr };
            for (i32 i = 0; i < new_cols * new_rows; i++) new_alt[i] = bk;
            i32 cp_r = MIN(t->rows, new_rows);
            i32 cp_c = MIN(t->cols, new_cols);
            for (i32 y = 0; y < cp_r; y++)
                memcpy(&new_alt[y * new_cols], &t->alt_cells[y * t->cols],
                       (usize)cp_c * sizeof(Cell));
        }
        free(t->alt_cells);
        t->alt_cells = new_alt;
    }

    /* Tab stops */
    free(t->tab_stops);
    t->tab_stops = calloc((usize)new_cols, sizeof(bool));
    for (i32 i = 0; i < new_cols; i += 8) t->tab_stops[i] = true;

    t->cols = new_cols;
    t->rows = new_rows;
    t->scroll_top = 0;
    t->scroll_bottom = new_rows - 1;

    /* Width changed; refresh scrollback budgets and trim if needed. */
    terminal_set_scrollback_limit(t, t->scrollback_limit);

    /* resize dirty bitmap */
    i32 need_words = (new_rows + 63) / 64;
    if (need_words < 1) need_words = 1;
    if (need_words != t->dirty_words) {
        free(t->dirty_rows);
        t->dirty_rows  = calloc((usize)need_words, sizeof(u64));
        t->dirty_words = need_words;
    }

    /* Reallocate row_zones for new row count */
    free(t->row_zones);
    t->row_zones = calloc((usize)new_rows, sizeof(SemanticZone));

    /* Ensure line_wrapped exists (alt-screen path doesn't set it) */
    if (!t->line_wrapped) {
        t->line_wrapped = calloc((usize)new_rows, sizeof(bool));
    }

    /* Invalidate sparse underline map — keys are col-dependent */
    terminal_clear_underline_map(t);

    /* Drop all inline images on resize. The reflow above rebuilds scrollback
     * from scratch (sb_count reset + re-push at the new width), so every
     * image's absolute-line anchor now points at a different line than the one
     * it was pinned to — there is no cheap, correct re-map. Clearing is the
     * safe choice (a mis-anchored image floating over unrelated text is worse
     * than a gone one); fresh output re-detects/re-emits as needed. */
    terminal_clear_images(t);

    /* Clamp cursor */
    if (t->cursor_x >= new_cols) t->cursor_x = new_cols - 1;
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

void terminal_set_underline(Terminal *t, i32 col, i32 row, u32 color, u8 style) {
    if (color == 0 && style == 0) return; /* default, no need to store */

    /* Lazy allocate */
    if (!t->ul_map) {
        t->ul_map_cap = UL_MAP_CAP;
        t->ul_map = malloc((usize)t->ul_map_cap * sizeof(UnderlineEntry));
        if (!t->ul_map) return;
        for (i32 i = 0; i < t->ul_map_cap; i++)
            t->ul_map[i].key = 0xFFFFFFFF;
        t->ul_map_count = 0;
    }

    u32 key = (u32)(row * t->cols + col);
    u32 mask = (u32)(t->ul_map_cap - 1);

    /* Grow if >70% full */
    if (t->ul_map_count * 10 > t->ul_map_cap * 7) {
        i32 new_cap = t->ul_map_cap * 2;
        UnderlineEntry *new_map = malloc((usize)new_cap * sizeof(UnderlineEntry));
        if (!new_map) return;
        for (i32 i = 0; i < new_cap; i++) new_map[i].key = 0xFFFFFFFF;
        u32 new_mask = (u32)(new_cap - 1);
        for (i32 i = 0; i < t->ul_map_cap; i++) {
            if (t->ul_map[i].key != 0xFFFFFFFF) {
                u32 h = t->ul_map[i].key & new_mask;
                while (new_map[h].key != 0xFFFFFFFF) h = (h + 1) & new_mask;
                new_map[h] = t->ul_map[i];
            }
        }
        free(t->ul_map);
        t->ul_map = new_map;
        t->ul_map_cap = new_cap;
        mask = new_mask;
    }

    u32 h = key & mask;
    while (t->ul_map[h].key != 0xFFFFFFFF && t->ul_map[h].key != key)
        h = (h + 1) & mask;
    if (t->ul_map[h].key == 0xFFFFFFFF) t->ul_map_count++;
    t->ul_map[h].key = key;
    t->ul_map[h].underline_color = color;
    t->ul_map[h].underline_style = style;
}

void terminal_get_underline(Terminal *t, i32 col, i32 row, u32 *color, u8 *style) {
    *color = 0;
    *style = 0;
    if (!t->ul_map || t->ul_map_count == 0) return;
    u32 key = (u32)(row * t->cols + col);
    u32 mask = (u32)(t->ul_map_cap - 1);
    u32 h = key & mask;
    for (i32 probe = 0; probe < 16; probe++) {
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
}

/* =========================================================================
 * Scrollback navigation
 * ========================================================================= */

void terminal_scroll_up(Terminal *t, i32 lines) {
    t->scroll_offset += lines;
    if (t->scroll_offset > t->sb_count) t->scroll_offset = t->sb_count;
    TERM_DIRTY_ALL(t);
}

void terminal_scroll_down(Terminal *t, i32 lines) {
    t->scroll_offset -= lines;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    TERM_DIRTY_ALL(t);
}

void terminal_scroll_to_bottom(Terminal *t) {
    t->scroll_offset = 0;
    TERM_DIRTY_ALL(t);
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

/* xterm modifier encoding for modifyOtherKeys CSI 27 format.
 * Same encoding as Liu: 1 + shift(1) + alt(2) + ctrl(4) + meta(8) */
static i32 xterm_mods(u32 mods) {
    return Liu_mods(mods);  /* same bit layout */
}

/* snprintf returns the length it WOULD have written; on truncation that exceeds
 * out_size, and terminal_char_input's caller sends exactly that many bytes to
 * the PTY — reading past the (small, stack) `out` buffer. Clamp to the bytes
 * that actually fit so we never report more than was written. */
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
static u32 key_to_unicode(u32 key) {
    if (key >= KEY_A && key <= KEY_Z) return 'a' + (key - KEY_A);
    if (key >= KEY_0 && key <= KEY_9) return '0' + (key - KEY_0);
    switch (key) {
    case KEY_ENTER: return 13; case KEY_TAB: return 9;
    case KEY_BACKSPACE: return 127; case KEY_ESCAPE: return 27;
    case KEY_SPACE: return 32; case KEY_DELETE: return 57348;
    case KEY_UP: return 57352; case KEY_DOWN: return 57353;
    case KEY_LEFT: return 57354; case KEY_RIGHT: return 57355;
    case KEY_HOME: return 57356; case KEY_END: return 57357;
    case KEY_PAGE_UP: return 57358; case KEY_PAGE_DOWN: return 57359;
    case KEY_INSERT: return 57348;
    case KEY_F1: return 57364; case KEY_F2: return 57365;
    case KEY_F3: return 57366; case KEY_F4: return 57367;
    case KEY_F5: return 57368; case KEY_F6: return 57369;
    case KEY_F7: return 57370; case KEY_F8: return 57371;
    case KEY_F9: return 57372; case KEY_F10: return 57373;
    case KEY_F11: return 57374; case KEY_F12: return 57375;
    default: return 0;
    }
}

/* =========================================================================
 * Key input → escape sequence encoding
 * ========================================================================= */

i32 terminal_key_input(Terminal *t, u32 key, u32 mods, u8 *out, i32 out_size) {
    i32 n = 0;
    bool app = (t->mode & MODE_APP_CURSOR) != 0;

    /* Liu / kitty keyboard protocol: encode keys with CSI-u format.
     *
     * IMPORTANT: under flag 0x01 ("disambiguate") *only*, printable keys
     * with no non-shift modifier MUST go through the legacy text path
     * (CHAR_INPUT → terminal_char_input → plain ASCII byte). Emitting
     * CSI-u here AND letting CHAR_INPUT also fire produced doubled
     * characters in any tool that enables disambiguate mode (codex, fzf,
     * helix, …) — the user typed 'a' and saw 'aa'.
     *
     * Only emit CSI-u when the kitty spec actually requires it:
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
             * mode. The kitty spec keeps emitting those legacy codes UNLESS
             * "report all keys as escape codes" (flag 0x08) is set — only then
             * do they switch to the functional CSI <codepoint> u form. Emitting
             * CSI-u for an arrow under mere "disambiguate" (flag 0x01, what most
             * TUIs enable) makes apps that expect CSI A — codex, fzf, helix … —
             * ignore the arrow keys entirely. So use CSI-u here only when
             * report_all is set, or for a NON-special key carrying a real
             * modifier (e.g. Ctrl+letter under disambiguate); modified and plain
             * special keys fall through to the legacy switch below. */
            bool has_shift = (mods & MOD_SHIFT) != 0;
            bool is_control_char = (ucp > 0 && ucp < 0x20);
            if (report_all || (has_real_mod && !is_special_key) ||
                (has_shift && is_control_char)) {
                i32 km = Liu_mods(mods);
                if (km > 1)
                    n = snprintf((char *)out, (usize)out_size, "\x1b[%u;%du", ucp, km);
                else
                    n = snprintf((char *)out, (usize)out_size, "\x1b[%uu", ucp);
                if (n > 0) return n;
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
            switch (key) {
            case KEY_ENTER:     cp = 13; break;
            case KEY_TAB:       cp = 9; break;
            case KEY_BACKSPACE: cp = 127; break;
            case KEY_ESCAPE:    cp = 27; break;
            case KEY_SPACE:     cp = 32; break;
            default: break;
            }
            if (cp > 0) {
                n = encode_modify_other_keys(cp, mods, out, out_size);
                if (n > 0) return n;
            }
        }
    }

    /* xterm modifier parameter: 1+shift(1)+alt(2)+ctrl(4)+meta(8)
     * When modifier > 1, arrow/nav keys use: ESC [ 1 ; mod X
     * Function keys use: ESC [ code ; mod ~    */
    i32 mod_param = 1;
    if (mods & MOD_SHIFT) mod_param += 1;
    if (mods & MOD_ALT)   mod_param += 2;
    if (mods & MOD_CTRL)  mod_param += 4;
    if (mods & MOD_SUPER) mod_param += 8;
    bool has_mod = (mod_param > 1);

    /* OS line-editing maps (Cmd/Option on macOS, Ctrl/Alt on Linux) only apply
     * on the pure-legacy path. In kitty/CSI-u (Liu_kbd_flags) or modifyOtherKeys
     * mode the agent TUI receives the disambiguated event and runs its own line
     * editing — for modified ARROWS especially, the kitty path falls through to
     * the legacy switch under "disambiguate" (flag 0x01), so this guard is what
     * stops us from clobbering the CSI modified-arrow such TUIs expect. */
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
    #define EMIT_NAV(code, letter) do { \
        if (has_mod) { \
            n = snprintf((char *)out, (usize)out_size, "\x1b[1;%d" letter, mod_param); \
        } else { \
            EMIT("\x1b[" letter); \
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
        case KEY_ENTER:     EMIT("\r"); break;
        case KEY_BACKSPACE:
            if (legacy_kbd && (mods & MOD_SUPER)) {
                /* Cmd+Backspace → Ctrl-U (kill to start of line, macOS) */
                EMIT("\x15");
            } else if (legacy_kbd && (mods & (MOD_ALT | MOD_CTRL))) {
                /* Option+Backspace (macOS) / Ctrl+Backspace (Linux) →
                 * ESC DEL (backward-kill-word in readline/zle) */
                EMIT("\x1b\x7f");
            } else {
                EMIT("\x7f");
            }
            break;
        case KEY_TAB:
            if (mods & MOD_SHIFT) { EMIT("\x1b[Z"); }
            else { EMIT("\t"); }
            break;
        case KEY_ESCAPE:    EMIT("\x1b"); break;

        /* Arrow keys — with modifier: ESC[1;modX */
        case KEY_UP:        EMIT_ARROW("A"); break;
        case KEY_DOWN:      EMIT_ARROW("B"); break;
        case KEY_RIGHT:
            if (legacy_kbd && (mods & MOD_SUPER)) {
                EMIT("\x05");          /* Cmd+Right → Ctrl-E (end of line) */
            } else if (legacy_kbd && (mods & (MOD_ALT | MOD_CTRL))) {
                EMIT("\x1b" "f");      /* Option/Ctrl+Right → ESC f (forward-word) */
            } else {
                EMIT_ARROW("C");
            }
            break;
        case KEY_LEFT:
            if (legacy_kbd && (mods & MOD_SUPER)) {
                EMIT("\x01");          /* Cmd+Left → Ctrl-A (start of line) */
            } else if (legacy_kbd && (mods & (MOD_ALT | MOD_CTRL))) {
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
            if (legacy_kbd && (mods & MOD_SUPER)) {
                /* Cmd+ForwardDelete → Ctrl-K (kill to end of line, macOS) */
                EMIT("\x0b");
            } else if (legacy_kbd && (mods & (MOD_ALT | MOD_CTRL))) {
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
    return n;
}

i32 terminal_char_input(Terminal *t, u32 codepoint, u32 mods, u8 *out, i32 out_size) {
    /* Job-control suspend must remain a real SUB byte even when an app has
     * enabled kitty/CSI-u keyboard reporting. Otherwise the PTY line
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
     *  here we handle Alt+key which is ambiguous with ESC+key) */
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
     * (e.g., 'b' for Opt+B), so codepoint < 0x80 and we correctly add ESC.
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

    /* UTF-8 encode via shared utility */
    u32 len = utf8_encode(codepoint, out);
    return (i32)len;
}

/* =========================================================================
 * Inline image management
 * ========================================================================= */

void terminal_clear_images(Terminal *t) {
    if (!t || !t->images) return;
    for (i32 i = 0; i < MAX_TERM_IMAGES; i++) {
        if (t->images[i].valid && t->images[i].pixels) {
            free(t->images[i].pixels);
        }
        free(t->images[i].src_path);
        t->images[i].src_path = NULL;
        t->images[i].clickable = false;
        t->images[i].pixels = NULL;
        t->images[i].valid = false;
        t->images[i].serial = 0;
        t->images[i].last_used_sec = 0.0;
    }
    t->image_count = 0;
    t->images_byte_usage = 0;
}

void terminal_delete_image(Terminal *t, u32 image_id) {
    if (!t || !t->images) return;
    for (i32 i = 0; i < MAX_TERM_IMAGES; i++) {
        if (t->images[i].valid && t->images[i].image_id == image_id) {
            usize bytes = (usize)t->images[i].width *
                          (usize)t->images[i].height * 4u;
            free(t->images[i].pixels);
            t->images[i].pixels = NULL;
            free(t->images[i].src_path);
            t->images[i].src_path = NULL;
            t->images[i].clickable = false;
            t->images[i].valid = false;
            t->images[i].serial = 0;
            t->images[i].last_used_sec = 0.0;
            if (t->images_byte_usage > bytes) t->images_byte_usage -= bytes;
            else t->images_byte_usage = 0;
            t->image_count--;
            TERM_DIRTY_ALL(t);
            break;
        }
    }
}

/* Per-terminal inline image memory cap. Inline image payloads can claim
 * up to 4096x4096x4B (64 MiB) each; with MAX_TERM_IMAGES=32 that's ~2 GiB of
 * resident RAM per terminal if a hostile remote sprays images. Aggregate
 * bytes are capped by TERM_IMAGES_BUDGET_BYTES (terminal.h) and we evict the
 * least-recently-used slot until each new image fits. */

static inline usize term_image_pixel_bytes(const TermImage *img) {
    if (!img || !img->valid) return 0;
    i32 w = img->width  > 0 ? img->width  : 0;
    i32 h = img->height > 0 ? img->height : 0;
    return (usize)w * (usize)h * 4u;
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
    if (t->images[slot].pixels) free(t->images[slot].pixels);
    t->images[slot].pixels = NULL;
    free(t->images[slot].src_path);
    t->images[slot].src_path = NULL;
    t->images[slot].clickable = false;
    t->images[slot].valid = false;
    t->images[slot].serial = 0;
    t->images[slot].last_used_sec = 0.0;
    if (t->images_byte_usage > bytes) t->images_byte_usage -= bytes;
    else t->images_byte_usage = 0;
    t->image_count--;
}

i32 terminal_add_image(Terminal *t, u8 *pixels, i32 w, i32 h,
                       i32 col, i32 row, i32 cols, i32 rows,
                       u32 image_id, u32 placement_id) {
    if (!t || !pixels) return -1;

    usize incoming_bytes = (usize)(w > 0 ? w : 0) * (usize)(h > 0 ? h : 0) * 4u;
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

    /* Evict LRU slots until the new image fits within the byte budget. */
    while (t->images_byte_usage + incoming_bytes > TERM_IMAGES_BUDGET_BYTES) {
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
    t->images[slot].serial = ++t->image_serial_next;
    if (t->images[slot].serial == 0) t->images[slot].serial = ++t->image_serial_next;
    t->images[slot].z_index = 0;
    t->images[slot].last_used_sec = platform_time_sec();
    t->images[slot].valid = true;
    t->image_count++;
    t->images_byte_usage += incoming_bytes;
    TERM_DIRTY_ALL(t);
    return slot;
}

void terminal_set_cell_pixels(Terminal *t, i32 cell_px_w, i32 cell_px_h) {
    if (!t) return;
    if (cell_px_w > 0) t->cell_px_w = cell_px_w;
    if (cell_px_h > 0) t->cell_px_h = cell_px_h;
}

/* Agent-detected inline previews are reserved as a compact thumbnail (a few
 * rows tall) instead of the full-bleed image, which used to swallow the whole
 * pane. The full file stays one click away via TermImage.src_path. */
/* Kept deliberately small: since the preview is now a pure overlay (no row
 * reservation — see terminal_place_image_inline), a tall thumbnail would cover
 * more of the agent's live text, so we trade preview size for less occlusion.
 * The full image is one click away. */
#define TERM_THUMB_MAX_ROWS 6    /* preview height cap, in cells */
#define TERM_THUMB_MAX_COLS 22   /* preview width cap, in cells  */

/* Area-average (box filter) RGBA downscale src(sw x sh) -> dst(dw x dh). Each
 * source pixel is read exactly once. Mirrors md_image's downscaler; kept local
 * so the terminal core doesn't pull in the markdown image module. */
static void term_box_downscale_rgba(const u8 *src, i32 sw, i32 sh,
                                     u8 *dst, i32 dw, i32 dh) {
    for (i32 dy = 0; dy < dh; dy++) {
        i32 sy0 = (i32)((i64)dy * sh / dh);
        i32 sy1 = (i32)((i64)(dy + 1) * sh / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (i32 dx = 0; dx < dw; dx++) {
            i32 sx0 = (i32)((i64)dx * sw / dw);
            i32 sx1 = (i32)((i64)(dx + 1) * sw / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            u32 r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (i32 yy = sy0; yy < sy1; yy++) {
                const u8 *row = src + ((i64)yy * sw + sx0) * 4;
                for (i32 xx = sx0; xx < sx1; xx++) {
                    r += row[0]; g += row[1]; b += row[2]; a += row[3];
                    row += 4; cnt++;
                }
            }
            u8 *d = dst + ((i64)dy * dw + dx) * 4;
            d[0] = (u8)(r / cnt); d[1] = (u8)(g / cnt);
            d[2] = (u8)(b / cnt); d[3] = (u8)(a / cnt);
        }
    }
}

void terminal_place_image_inline(Terminal *t, u8 *pixels, i32 w, i32 h) {
    if (!t || !pixels || w <= 0 || h <= 0) {
        free(pixels);
        if (t) { free(t->pending_img_path); t->pending_img_path = NULL; }
        return;
    }

    /* Consume the parked source path (set by the line scanner). Owned here. */
    char *src_path = t->pending_img_path;
    t->pending_img_path = NULL;

    /* Cell metrics: prefer the UI-pushed values, fall back to an 8x16 guess so
     * placement still works before the first render syncs them. */
    i32 cpw = t->cell_px_w > 0 ? t->cell_px_w : 8;
    i32 cph = t->cell_px_h > 0 ? t->cell_px_h : 16;

    /* Compact preview footprint: fit the image inside a small box
     * (TERM_THUMB_MAX_COLS x TERM_THUMB_MAX_ROWS cells, clamped to the pane)
     * preserving aspect, and never upscale past native pixels. */
    i32 cap_cols = t->cols > 0 ? t->cols : 80;
    if (cap_cols > TERM_THUMB_MAX_COLS) cap_cols = TERM_THUMB_MAX_COLS;
    if (cap_cols < 1) cap_cols = 1;
    i32 cap_rows = TERM_THUMB_MAX_ROWS;

    i64 box_w = (i64)cap_cols * cpw;          /* preview box in pixels */
    i64 box_h = (i64)cap_rows * cph;

    /* scale = min(box/native, 1.0), kept as a num/den ratio for integer math. */
    i64 disp_px_w, disp_px_h;
    if ((i64)w * box_h <= (i64)h * box_w) {
        /* height is the binding dimension */
        disp_px_h = (i64)h < box_h ? (i64)h : box_h;
        disp_px_w = disp_px_h * (i64)w / (i64)h;
    } else {
        disp_px_w = (i64)w < box_w ? (i64)w : box_w;
        disp_px_h = disp_px_w * (i64)h / (i64)w;
    }
    if (disp_px_w < 1) disp_px_w = 1;
    if (disp_px_h < 1) disp_px_h = 1;

    i32 disp_cols = (i32)((disp_px_w + cpw - 1) / cpw);
    i32 disp_rows = (i32)((disp_px_h + cph - 1) / cph);
    if (disp_cols < 1) disp_cols = 1;
    if (disp_rows < 1) disp_rows = 1;
    if (disp_cols > cap_cols) disp_cols = cap_cols;
    if (disp_rows > cap_rows) disp_rows = cap_rows;

    /* Bake the thumbnail at the footprint's pixel size (device px) and drop the
     * full-resolution source — the click-to-expand path reloads it from disk. */
    i32 tw = disp_cols * cpw;
    i32 th = disp_rows * cph;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    u8 *thumb = (u8 *)malloc((usize)tw * (usize)th * 4u);
    if (thumb) {
        term_box_downscale_rgba(pixels, w, h, thumb, tw, th);
        free(pixels);
    } else {
        /* Allocation failed — fall back to showing the source pixels as-is. */
        thumb = pixels;
        tw = w; th = h;
    }

    /* Right-align the preview against the pane's right edge: the supported
     * agents lay their text out left-aligned, so the right margin is usually
     * the emptiest place to float a thumbnail. terminal_add_image derives the
     * absolute-line anchor from cursor_y and owns the pixel buffer from here
     * (frees it itself on failure). */
    i32 anchor_col = t->cols - disp_cols;
    if (anchor_col < 0) anchor_col = 0;
    i32 slot = terminal_add_image(t, thumb, tw, th, anchor_col, t->cursor_y,
                                  disp_cols, disp_rows, 0, 0);
    if (slot < 0) { free(src_path); return; }

    /* Attach the source path so a click reopens the full image. */
    if (src_path && src_path[0]) {
        t->images[slot].src_path  = src_path;
        t->images[slot].clickable = true;
    } else {
        free(src_path);
    }

    /* Pure overlay: do NOT reserve rows, move the cursor, or scroll. The
     * supported agents (Claude Code, Codex, OpenCode, Grok) keep redrawing
     * their live region in the MAIN buffer via cursor addressing — they don't
     * use the alternate screen — so any scroll/cursor mutation here desyncs
     * their model of the screen and shreds the TUI (the old reservation
     * line-feeds were exactly what made the prompt box duplicate and the image
     * land on top of the menu). The thumbnail is pinned to its absolute line,
     * so it still rides the text up into scrollback without perturbing a single
     * cell of the agent's layout. */
    TERM_DIRTY_ALL(t);
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
