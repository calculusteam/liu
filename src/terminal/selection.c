/*
 * Liu - smart terminal selection (per-terminal instance)
 * Double-click word select, triple-click logical-line select, rectangular
 * select, and the clipboard text those produce.
 */
#include "terminal/terminal.h"
#include "core/checked.h"
#include "core/utf8.h"
#include "platform/platform.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Word boundaries
 *
 * A double click selects the run of cells that are on the SAME SIDE of a
 * small boundary set as the cell that was clicked. Two properties fall out
 * of that, and both are what people expect from a terminal:
 *
 *   - the set is a deny-list, not an allow-list, so `-`, `_`, `.`, `/`, `~`,
 *     `=`, `@`, `%`, `+`, `?`, `&`, `#`, `*` and every other unlisted glyph
 *     are part of a word. `~/src/main.c` and `--color=always` come out whole,
 *     which an allow-list can only manage by growing forever.
 *   - clicking a boundary character selects the boundary RUN, so a double
 *     click in the gap between two words takes the gap, not one blank cell.
 *
 * The listed characters are the ones that reliably delimit rather than
 * belong: quoting (' " `), the shell pipe, the separators that end a token in
 * paths and diagnostics (: ,), every bracket pair, and the box-drawing bar
 * TUIs rail their output with — `│` is a frame, not a letter, and treating it
 * as one would glue a TUI's border to the first word beside it.
 * ========================================================================= */

static bool is_boundary(u32 cp) {
    /* Cells can hold a grapheme HANDLE rather than a codepoint, so compare
     * against the base — a handle is numerically larger than anything here
     * and would otherwise read as a non-boundary by accident rather than on
     * purpose. */
    cp = utf8_cluster_base(cp);
    switch (cp) {
        case 0:      /* never-written cell */
        case ' ':
        case '\t':
        case '\'':
        case '"':
        case '`':
        case '|':
        case ':':
        case ',':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '<':
        case '>':
        case 0x2502: /* │ BOX DRAWINGS LIGHT VERTICAL */
            return true;
        default:
            return false;
    }
}

/* A cell that contributes nothing to the copied text: never written, or a
 * plain blank. Used for the whitespace trimming a line selection and the
 * clipboard both do. */
static bool is_blank_cp(u32 cp) { return cp == 0 || cp == ' '; }

/* =========================================================================
 * Content anchoring — see the TermSelection comment in terminal.h.
 *
 * The mapping is terminal_visible_row's, inverted. That function resolves a
 * viewport row as `logical = visible_row - scroll_offset`, taking the live
 * grid for logical >= 0 and scrollback index `sb_count + logical` otherwise —
 * so in BOTH cases the absolute line is
 *     sb_abs_base + sb_count + (visible_row - scroll_offset)
 * which is exactly what TermImage.abs_line already pins to. Deriving it any
 * other way would put the highlight on a different row than the text it is
 * meant to cover.
 * ========================================================================= */

static i64 sel_abs_of_row(const Terminal *t, i32 row) {
    return t->sb_abs_base + (i64)t->sb_count + (i64)(row - t->scroll_offset);
}

static i32 sel_row_of_abs(const Terminal *t, i64 abs) {
    return (i32)(abs - t->sb_abs_base - (i64)t->sb_count) + t->scroll_offset;
}

/* FNV-1a hashes the displayed codepoints covered by an alternate-screen
 * selection, where there is no line identity to track. Unwritten cells and
 * written blanks hash the same because they display the same content.
 * Recompute the hash only after content_writes changes or the selection is
 * remade. */
static u64 sel_covered_hash(Terminal *t) {
    const TermSelection *s = &t->selection;
    i32 sy = s->start_row, sx = s->start_col;
    i32 ey = s->end_row,   ex = s->end_col;
    if (sy > ey || (sy == ey && sx > ex)) {
        i32 tmp;
        tmp = sy; sy = ey; ey = tmp;
        tmp = sx; sx = ex; ex = tmp;
    }
    i32 min_col = sx < ex ? sx : ex;
    i32 max_col = sx > ex ? sx : ex;
    u64 h = 1469598103934665603ULL;
    for (i32 y = sy; y <= ey; y++) {
        if (y < 0 || y >= t->rows) continue;
        i32 vlen = 0;
        const Cell *rc = terminal_visible_row(t, y, &vlen);
        i32 x0, x1;
        if (s->is_rect) { x0 = min_col; x1 = max_col; }
        else {
            x0 = (y == sy) ? sx : 0;
            x1 = (y == ey) ? ex : t->cols - 1;
        }
        if (x0 < 0) x0 = 0;
        if (x1 >= t->cols) x1 = t->cols - 1;
        for (i32 x = x0; x <= x1; x++) {
            u32 cp = (rc && x < vlen) ? rc[x].codepoint : 0;
            if (cp == ' ') cp = 0;
            h ^= (u64)cp;
            h *= 1099511628211ULL;
        }
        /* Row boundary, so "ab" / "" and "a" / "b" do not collide. */
        h ^= 0xFFFFFFFFULL;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Re-take the anchor from the CURRENT start_row. Called whenever the user
 * changes the selection, so a drag re-bases instead of accumulating drift. */
static void selection_reanchor(Terminal *t) {
    TermSelection *s = &t->selection;
    s->on_alt     = (t->mode & MODE_ALT_SCREEN) != 0;
    s->anchor_abs = s->on_alt ? -1 : sel_abs_of_row(t, s->start_row);
    s->epoch      = t->content_writes;
    s->content_hash = s->on_alt ? sel_covered_hash(t) : 0;
}

void selection_sync(Terminal *t) {
    if (!t) return;
    TermSelection *s = &t->selection;
    if (!s->active) return;

    /* The screen was swapped under it. A primary-screen selection means
     * nothing on the alt grid and vice versa — the cells are a different
     * buffer entirely. */
    bool alt_now = (t->mode & MODE_ALT_SCREEN) != 0;
    if (alt_now != s->on_alt) { selection_clear(t); return; }

    if (s->on_alt) {
        /* No line identity to follow, so the only rule is: the moment the
         * COVERED content actually changes, the selection is over, as when an
         * agent CLI repaints its transcript under a copy's highlight on fixed
         * rows. Covered, not "anywhere on the screen": the same CLIs keep a
         * spinner and a clock running while the user is selecting their
         * output, and a rule that ended the selection on any changed cell
         * would end every selection within a tenth of a second. */
        if (t->content_writes != s->epoch) {
            if (sel_covered_hash(t) == s->content_hash) s->epoch = t->content_writes;
            else selection_clear(t);
        }
        return;
    }

    /* Evicted from the ring: the text is gone for good. */
    if (s->anchor_abs < t->sb_abs_base) { selection_clear(t); return; }

    i32 delta = sel_row_of_abs(t, s->anchor_abs) - s->start_row;
    if (delta) {
        s->start_row += delta;
        s->end_row   += delta;
        /* The pivot names the same text as start/end and has to travel with
         * it — left behind, a drag that continues after the view scrolled
         * would extend from a row the gesture never touched. */
        s->pivot_start_row += delta;
        s->pivot_end_row   += delta;
    }
}

/* =========================================================================
 * Reading the grid as displayed
 *
 * Every helper below resolves rows through terminal_row_unclipped, so they
 * are scroll-aware — a word straddling the boundary between scrollback and
 * the live grid reads as one word — and they are NOT clipped to the
 * viewport: a selection's rows are viewport rows that travel with the text,
 * and after the view scrolls under a drag they can name rows above the top
 * or below the bottom of the screen. Those rows still hold text (see the
 * terminal_row_unclipped comment in terminal.h). The returned pointer is only
 * valid until the NEXT resolve (a cold scrollback row is materialised into
 * one shared scratch buffer), so nothing here holds a row across a row
 * change.
 * ========================================================================= */

/* The row a walk is on, kept resolved until the walk leaves it.
 *
 * Asking terminal_row_unclipped() for ONE codepoint costs, for a row in
 * history, a decompression of the whole line into the terminal's scratch
 * buffer. A word walk takes a step per character, so a
 * double-click inside a long soft-wrapped token paid a full-row decompression
 * per character: 51 million cells for one click in a 256 KiB token. The walk
 * only ever needs the SAME row until it crosses a boundary, and nothing else
 * resolves a row while it runs (terminal_row_exists and
 * terminal_row_wrapped_unclipped read flags, not cells), so the pointer stays
 * valid for exactly as long as it is used. */
typedef struct {
    Terminal   *t;
    i32         row;
    const Cell *cells;
    i32         len;
    bool        have;
} SelRowCursor;

static u32 sel_cursor_cp(SelRowCursor *rc, i32 col, i32 row) {
    if (!rc->have || rc->row != row) {
        rc->cells = terminal_row_unclipped(rc->t, row, &rc->len);
        rc->row = row;
        rc->have = true;
    }
    if (!rc->cells || col < 0 || col >= rc->len || col >= rc->t->cols) return 0;
    /* The right half of a wide character (and the spacer a wrap leaves) holds
     * a blank of its own, but it is not a gap between words: it reads as the
     * cell before it. Taken literally it made every CJK character a word by
     * itself — a double click selected one, and `file名.txt` stopped at 名. */
    if ((rc->cells[col].attr.flags & ATTR_WDUMMY) && col > 0)
        return rc->cells[col - 1].codepoint;
    return rc->cells[col].codepoint;
}

/* A dummy cell that is the right half of the wide character before it: part
 * of the text, though the cell itself holds a blank. */
static inline bool sel_cell_is_wide_tail(const Cell *row, i32 x) {
    return x > 0 && (row[x].attr.flags & ATTR_WDUMMY) && (row[x - 1].attr.flags & ATTR_WIDE);
}

/* Last column of `row` holding anything but blanks, or -1 for an empty row. */
static i32 sel_row_last_content(Terminal *t, i32 row) {
    i32 vlen = 0;
    const Cell *rc = terminal_row_unclipped(t, row, &vlen);
    if (!rc) return -1;
    i32 n = vlen < t->cols ? vlen : t->cols;
    i32 last = -1;
    for (i32 x = 0; x < n; x++)
        if (!is_blank_cp(rc[x].codepoint) || sel_cell_is_wide_tail(rc, x)) last = x;
    return last;
}

/* First column of `row` holding anything but blanks, or -1 for an empty row. */
static i32 sel_row_first_content(Terminal *t, i32 row) {
    i32 vlen = 0;
    const Cell *rc = terminal_row_unclipped(t, row, &vlen);
    if (!rc) return -1;
    i32 n = vlen < t->cols ? vlen : t->cols;
    for (i32 x = 0; x < n; x++)
        if (!is_blank_cp(rc[x].codepoint)) return x;
    return -1;
}

/* Step one cell left/right, following a soft wrap across the row boundary.
 * Returns false when there is nowhere to go — the oldest scrollback line,
 * the last grid row, or a hard line break. */
static bool sel_step_left(Terminal *t, i32 *col, i32 *row) {
    if (*col > 0) { (*col)--; return true; }
    if (!terminal_row_exists(t, *row - 1)) return false;
    if (!terminal_row_wrapped_unclipped(t, *row - 1)) return false;
    (*row)--;
    *col = t->cols - 1;
    return true;
}

static bool sel_step_right(Terminal *t, i32 *col, i32 *row) {
    if (*col < t->cols - 1) { (*col)++; return true; }
    if (!terminal_row_exists(t, *row + 1)) return false;
    if (!terminal_row_wrapped_unclipped(t, *row)) return false;
    (*row)++;
    *col = 0;
    return true;
}

/* The word under (col,row), grown in both directions over cells on the same
 * side of the boundary set — across soft wraps, so a path broken over two
 * screen rows selects as the one token it is. */
static void sel_word_at(Terminal *t, i32 col, i32 row,
                        i32 *sc, i32 *sr, i32 *ec, i32 *er) {
    SelRowCursor rc = { .t = t };
    bool boundary = is_boundary(sel_cursor_cp(&rc, col, row));

    i32 c = col, r = row;
    for (;;) {
        i32 pc = c, pr = r;
        if (!sel_step_left(t, &pc, &pr)) break;
        if (is_boundary(sel_cursor_cp(&rc, pc, pr)) != boundary) break;
        c = pc; r = pr;
    }
    *sc = c; *sr = r;

    c = col; r = row;
    for (;;) {
        i32 nc = c, nr = r;
        if (!sel_step_right(t, &nc, &nr)) break;
        if (is_boundary(sel_cursor_cp(&rc, nc, nr)) != boundary) break;
        c = nc; r = nr;
    }
    *ec = c; *er = r;
}

/* The LOGICAL line `row` belongs to: the whole soft-wrapped run, with the
 * blank margins at either end left out. Selecting a wrapped command line has
 * to take every screen row it occupies — stopping at the visual row is what
 * makes a triple-click copy half a command. */
static void sel_line_at(Terminal *t, i32 row,
                        i32 *sc, i32 *sr, i32 *ec, i32 *er) {
    i32 top = row;
    while (terminal_row_exists(t, top - 1) &&
           terminal_row_wrapped_unclipped(t, top - 1)) top--;
    i32 bot = row;
    while (terminal_row_exists(t, bot + 1) &&
           terminal_row_wrapped_unclipped(t, bot)) bot++;

    i32 first = sel_row_first_content(t, top);
    i32 last  = sel_row_last_content(t, bot);
    *sr = top;
    *er = bot;
    *sc = first >= 0 ? first : 0;
    *ec = last  >= 0 ? last  : 0;
    /* An all-blank first row of a wrapped run would otherwise start the
     * selection past its own end. */
    if (*sr == *er && *ec < *sc) *ec = *sc;
}

static bool sel_before(i32 c1, i32 r1, i32 c2, i32 r2) {
    return r1 < r2 || (r1 == r2 && c1 < c2);
}

static void sel_set_pivot(TermSelection *s) {
    s->pivot_start_col = s->start_col;
    s->pivot_start_row = s->start_row;
    s->pivot_end_col   = s->end_col;
    s->pivot_end_row   = s->end_row;
}

/* Floor division — rows can be negative once the view scrolled under a drag. */
static i64 sel_floordiv(i64 a, i64 b) {
    i64 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/* Zero width. Encoded as a one-row range whose only column is `cols`, past
 * the last cell, so every per-cell reader (selection_contains runs once per
 * cell for VoiceOver) answers "no" with no extra test on its path. The
 * readers that take a range as a whole (the copy, the per-row span) check
 * `empty` once. */
static void sel_set_empty(Terminal *t) {
    TermSelection *s = &t->selection;
    s->empty = true;
    s->start_col = s->end_col = t->cols;
    s->start_row = s->end_row = s->pivot_start_row;
}

/* Write the cells strictly between the press edge (pivot cell + pivot_right)
 * and the pointer's edge ((col,row) + right_half) into start/end, in reading
 * order. Edges are numbered along the text: edge e of row r is r*cols + e, so
 * the edge after a row's last cell IS the edge before the next row's first —
 * a stream selection needs no special case at the right margin. A rectangle
 * takes its columns from the two edges and its rows from the two rows. */
static void sel_edge_apply(Terminal *t, i32 col, i32 row, bool right_half) {
    TermSelection *s = &t->selection;
    i64 cols = t->cols;
    i64 pe = (i64)s->pivot_start_col + (s->pivot_right ? 1 : 0);
    i64 ce = (i64)col + (right_half ? 1 : 0);
    s->empty = false;
    if (s->is_rect) {
        i64 lo = pe < ce ? pe : ce, hi = pe < ce ? ce : pe;
        if (lo == hi) {
            sel_set_empty(t);
        } else {
            s->start_col = (i32)lo;        s->start_row = s->pivot_start_row;
            s->end_col   = (i32)(hi - 1);  s->end_row   = row;
        }
        selection_reanchor(t);
        return;
    }
    i64 b0 = (i64)s->pivot_start_row * cols + pe;
    i64 b1 = (i64)row * cols + ce;
    if (b0 == b1) {
        sel_set_empty(t);
        selection_reanchor(t);
        return;
    }
    i64 first = b0 < b1 ? b0 : b1;
    i64 last  = (b0 < b1 ? b1 : b0) - 1;
    i64 fr = sel_floordiv(first, cols), lr = sel_floordiv(last, cols);
    s->start_row = (i32)fr;  s->start_col = (i32)(first - fr * cols);
    s->end_row   = (i32)lr;  s->end_col   = (i32)(last  - lr * cols);
    selection_reanchor(t);
}

void selection_start(Terminal *t, i32 col, i32 row, i32 click_count, bool alt_held) {
    if (!t || t->cols <= 0 || t->rows <= 0) return;
    if (col < 0) col = 0;
    if (col >= t->cols) col = t->cols - 1;
    if (row < 0) row = 0;
    if (row >= t->rows) row = t->rows - 1;
    TermSelection *s = &t->selection;
    s->active = true;
    s->edge_mode = false;
    s->pivot_right = false;
    s->empty = false;
    s->is_word = (click_count == 2);
    s->is_line = (click_count >= 3);
    /* A rectangle is a drag shape. Holding the modifier over a double or
     * triple click asks for a unit, and the unit wins — a one-row rectangle
     * around a word is what the word selection already is. */
    s->is_rect = alt_held && !s->is_word && !s->is_line;

    if (s->is_line) {
        sel_line_at(t, row, &s->start_col, &s->start_row, &s->end_col, &s->end_row);
    } else if (s->is_word) {
        sel_word_at(t, col, row, &s->start_col, &s->start_row, &s->end_col, &s->end_row);
    } else {
        s->start_col = col;
        s->start_row = row;
        s->end_col = col;
        s->end_row = row;
    }
    sel_set_pivot(s);
    selection_reanchor(t);
}

void selection_update(Terminal *t, i32 col, i32 row) {
    if (!t || t->cols <= 0 || t->rows <= 0) return;
    TermSelection *s = &t->selection;
    /* Bring the pivot up to date FIRST. A drag that scrolls the view — the
     * edge auto-scroll, output arriving under the pointer — moves the text
     * under the stored viewport rows, and the pivot is what the new range is
     * built from: rebuilt from a stale pivot and re-anchored against the new
     * scroll offset, the fixed end would walk one row per scrolled line. */
    selection_sync(t);
    if (!s->active) return;
    if (col < 0) col = 0;
    if (col >= t->cols) col = t->cols - 1;
    if (row < 0) row = 0;
    if (row >= t->rows) row = t->rows - 1;

    if (s->is_word || s->is_line) {
        /* Extend by the unit the gesture was made with, from the unit it
         * started on: the pointer's own unit becomes whichever end of the
         * selection it is on, and the pivot supplies the other. */
        i32 uc0, ur0, uc1, ur1;
        if (s->is_line) sel_line_at(t, row, &uc0, &ur0, &uc1, &ur1);
        else            sel_word_at(t, col, row, &uc0, &ur0, &uc1, &ur1);

        if (sel_before(uc0, ur0, s->pivot_start_col, s->pivot_start_row)) {
            s->start_col = uc0; s->start_row = ur0;
            s->end_col   = s->pivot_end_col; s->end_row = s->pivot_end_row;
        } else {
            s->start_col = s->pivot_start_col; s->start_row = s->pivot_start_row;
            s->end_col   = uc1; s->end_row = ur1;
        }
        selection_reanchor(t);
        return;
    }

    if (s->edge_mode) {
        /* A cell-addressed update of an edge-made selection (Shift+click
         * extend, the auto-scroll step): take the cell under the pointer
         * INTO the range, whichever side of the press it is on. */
        bool forward = !sel_before(col, row, s->pivot_start_col, s->pivot_start_row);
        sel_edge_apply(t, col, row, forward);
        return;
    }
    s->start_col = s->pivot_start_col;
    s->start_row = s->pivot_start_row;
    s->end_col = col;
    s->end_row = row;
    selection_reanchor(t);
}

void selection_start_edge(Terminal *t, i32 col, i32 row, bool right_half, bool alt_held) {
    if (!t || t->cols <= 0 || t->rows <= 0) return;
    selection_start(t, col, row, 1, alt_held);
    TermSelection *s = &t->selection;
    s->edge_mode = true;
    s->pivot_right = right_half;
    /* The pointer is still ON the press edge: nothing between them yet. */
    sel_set_empty(t);
    selection_reanchor(t);
}

void selection_update_edge(Terminal *t, i32 col, i32 row, bool right_half) {
    if (!t || t->cols <= 0 || t->rows <= 0) return;
    TermSelection *s = &t->selection;
    if (!s->edge_mode || s->is_word || s->is_line) { selection_update(t, col, row); return; }
    selection_sync(t);                      /* the pivot first, as selection_update does */
    if (!s->active) return;
    if (col < 0) col = 0;
    if (col >= t->cols) col = t->cols - 1;
    if (row < 0) row = 0;
    if (row >= t->rows) row = t->rows - 1;
    sel_edge_apply(t, col, row, right_half);
}

void selection_extend(Terminal *t, i32 col, i32 row) {
    if (!t || t->cols <= 0 || t->rows <= 0) return;
    if (!t->selection.active || t->selection.start_col < 0) {
        selection_start(t, col, row, 1, false);
        return;
    }
    selection_sync(t);
    if (!t->selection.active) { selection_start(t, col, row, 1, false); return; }
    selection_update(t, col, row);
}

void selection_clear(Terminal *t) {
    if (!t) return;
    t->selection.active = false;
    t->selection.empty = false;
    t->selection.edge_mode = false;
    t->selection.start_col = -1;
    t->selection.anchor_abs = -1;
    t->selection.epoch = 0;
    t->selection.on_alt = false;
}

bool selection_active(Terminal *t) {
    if (!t) return false;
    selection_sync(t);
    return t->selection.active && t->selection.start_col >= 0;
}

/* NOT synced here, deliberately. This is called once per visible CELL by the
 * render pass, and a sync in it would repeat, for every cell, a decision
 * that is identical for every cell in the frame. Callers that scan many
 * cells sync ONCE first; both production callers do, via selection_active()
 * (the render pass's own gate) or an explicit selection_sync(). */
bool selection_contains(Terminal *t, i32 col, i32 row) {
    if (!t) return false;
    TermSelection *s = &t->selection;
    if (!s->active) return false;

    i32 sy = s->start_row, sx = s->start_col;
    i32 ey = s->end_row, ex = s->end_col;
    if (sy > ey || (sy == ey && sx > ex)) {
        i32 tmp;
        tmp = sy; sy = ey; ey = tmp;
        tmp = sx; sx = ex; ex = tmp;
    }

    if (s->is_rect) {
        i32 min_col = sx < ex ? sx : ex;
        i32 max_col = sx > ex ? sx : ex;
        return row >= sy && row <= ey && col >= min_col && col <= max_col;
    }
    if (row < sy || row > ey) return false;
    if (row == sy && row == ey) return col >= sx && col <= ex;
    if (row == sy) return col >= sx;
    if (row == ey) return col <= ex;
    return true;
}

/* =========================================================================
 * Clipboard text
 *
 * Trailing blanks are trimmed off every line the copy ends, controlled by
 * selection_set_trim_trailing(). It is a process-wide setting because the
 * preference is: one user, one clipboard, whatever window the drag happened
 * in. */
static bool g_trim_trailing = true;

void selection_set_trim_trailing(bool on) { g_trim_trailing = on; }

/* The copy's output buffer, grown as the text is produced. Sized up front for
 * the WORST case — every selected row, the terminal's full width, 16 bytes
 * per column — a narrow selection on a wide terminal would ask for megabytes
 * to hand back a few hundred bytes. Geometric growth from an estimate of the
 * SELECTED span keeps the common copy inside one allocation and the odd one
 * proportional to its text. An allocation failure abandons the copy rather
 * than hand back truncated UTF-8 as a successful copy. */
typedef struct { char *data; usize len, cap; } SelBuf;

static bool sel_reserve(SelBuf *b, usize need) {
    usize want;
    if (!checked_add_usize(b->len, need, &want) ||
        !checked_add_usize(want, 1u, &want)) return false;        /* + NUL */
    if (want <= b->cap) return true;
    usize cap = b->cap ? b->cap : 256u;
    while (cap < want) {
        if (cap > ((usize)-1) / 2u) { cap = want; break; }
        cap *= 2u;
    }
    char *d = realloc(b->data, cap);
    if (!d) return false;
    b->data = d;
    b->cap  = cap;
    return true;
}

static bool sel_put(SelBuf *b, const void *bytes, usize n) {
    if (!sel_reserve(b, n)) return false;
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
    return true;
}

/* The selection in reading order, columns clamped to the grid. */
typedef struct { i32 sy, sx, ey, ex; } SelBounds;

static SelBounds sel_bounds(const Terminal *t) {
    const TermSelection *s = &t->selection;
    SelBounds b = { s->start_row, s->start_col, s->end_row, s->end_col };
    if (b.sy > b.ey || (b.sy == b.ey && b.sx > b.ex)) {
        i32 tmp;
        tmp = b.sy; b.sy = b.ey; b.ey = tmp;
        tmp = b.sx; b.sx = b.ex; b.ex = tmp;
    }
    if (b.sx < 0) b.sx = 0;
    if (b.ex < 0) b.ex = 0;
    if (b.sx >= t->cols) b.sx = t->cols - 1;
    if (b.ex >= t->cols) b.ex = t->cols - 1;
    return b;
}

/* One row of the copy: the first column it takes (*x_start), the last
 * (returned; < *x_start when the row gives nothing), and whether its text
 * runs on into the next row with no newline between (*joins_next). `rc` and
 * `ncols` are the row as resolved by terminal_row_unclipped. The single
 * definition of "what a copy takes from this row", shared by
 * selection_get_text and selection_row_copy_span — the highlight is painted
 * from the latter, so the two cannot disagree. */
static i32 sel_row_span(Terminal *t, const SelBounds *b, i32 y,
                        const Cell *rc, i32 ncols, i32 *x_start, bool *joins_next) {
    const TermSelection *s = &t->selection;
    i32 x_end;
    if (s->is_rect) {
        i32 min_col = b->sx < b->ex ? b->sx : b->ex;
        i32 max_col = b->sx > b->ex ? b->sx : b->ex;
        *x_start = min_col;
        x_end = max_col;
    } else {
        *x_start = (y == b->sy) ? b->sx : 0;
        x_end = (y == b->ey) ? b->ex : t->cols - 1;
    }

    /* A soft-wrapped row's text CONTINUES on the row below — the wrap is
     * the terminal running out of columns, not the program ending a line.
     * So no newline is written after it and its tail is not trimmed: a
     * copied command comes back as the one line it was typed as, instead
     * of arriving in the shell as several lines that each execute. A
     * rectangle ignores wrapping by definition; it copies a block of
     * columns, and every row in it is its own line. */
    *joins_next = (!s->is_rect && y < b->ey &&
                   terminal_row_wrapped_unclipped(t, y));

    if (*joins_next) {
        /* Nothing exists past the materialised width, and padding to it
         * would insert blanks INSIDE the joined line. */
        return x_end < ncols - 1 ? x_end : ncols - 1;
    }
    if (g_trim_trailing) {
        i32 last = *x_start - 1;
        for (i32 x = *x_start; x <= x_end && x < ncols; x++)
            if (!is_blank_cp(rc[x].codepoint) || sel_cell_is_wide_tail(rc, x)) last = x;
        return last;
    }
    return x_end;
}

bool selection_row_copy_span(Terminal *t, i32 row, i32 *first, i32 *last) {
    if (!t || t->cols <= 0 || t->rows <= 0) return false;
    const TermSelection *s = &t->selection;
    if (!s->active || s->empty || s->start_col < 0) return false;
    SelBounds b = sel_bounds(t);
    if (row < b.sy || row > b.ey) return false;
    i32 vlen = 0;
    const Cell *rc = terminal_row_unclipped(t, row, &vlen);
    i32 ncols = (rc && vlen > 0) ? (vlen < t->cols ? vlen : t->cols) : 0;
    i32 x0 = 0;
    bool joins = false;
    i32 x1 = sel_row_span(t, &b, row, rc, ncols, &x0, &joins);
    if (first) *first = x0;
    if (last)  *last = x1;
    return x1 >= x0;
}

char *selection_get_text(Terminal *t) {
    if (!t || t->cols <= 0 || t->rows <= 0) return NULL;
    selection_sync(t);
    TermSelection *s = &t->selection;
    if (!s->active || s->empty) return NULL;

    SelBounds b = sel_bounds(t);
    i32 sy = b.sy, sx = b.sx, ey = b.ey, ex = b.ex;

    /* Rows outside the viewport are NOT clamped in: clamping would turn
     * "selection scrolled out of view" into "copied whatever is on row 0
     * instead" — plausible text the user never selected. They are read for
     * what they are instead: terminal_row_unclipped resolves a row above the
     * top of the screen into scrollback and a row below the bottom into the
     * live grid, because a drag that scrolls the view pushes the pressed cell
     * off the screen without making its text go anywhere. Only a row that no
     * longer exists (evicted, or past the grid) contributes nothing. */
    if (sy > ey) return NULL;

    usize rows = (usize)(ey - sy) + 1u;
    /* First allocation: the selected span at one byte per column plus the
     * newline — exact for ASCII, which is what most copies are. A rectangle
     * spans its column block; a stream selection spans the full width only on
     * its interior rows. Anything wider (a wide glyph is 3 bytes in one cell,
     * a ZWJ family 25) grows the buffer as it is written. */
    SelBuf out = { NULL, 0, 0 };
    {
        usize span = s->is_rect ? (usize)((sx > ex ? sx - ex : ex - sx) + 1)
                                : (rows > 1 ? (usize)t->cols : (usize)(ex - sx) + 1u);
        usize guess;
        if (!checked_add_usize(span, 1u, &guess) ||
            !checked_mul_usize(guess, rows, &guess)) guess = 0;
        if (guess && !sel_reserve(&out, guess)) return NULL;
    }

    for (i32 y = sy; y <= ey; y++) {
        /* Read the row as displayed (scroll-aware). The returned pointer is
         * valid only until the next terminal_visible_row call, so this
         * row's reads finish before advancing to y+1. Cells past `vlen` are
         * trailing blanks (trimmed on cold scrollback rows). */
        i32 vlen = 0;
        const Cell *rc = terminal_row_unclipped(t, y, &vlen);
        i32 ncols = (rc && vlen > 0) ? (vlen < t->cols ? vlen : t->cols) : 0;

        i32 x_start = 0;
        bool joins_next = false;
        i32 last = sel_row_span(t, &b, y, rc, ncols, &x_start, &joins_next);

        for (i32 x = x_start; x <= last; x++) {
            /* Skip wide-character dummy cells: a CJK/wide glyph occupies two
             * cells — the first carries the codepoint (ATTR_WIDE), the second
             * is a placeholder with codepoint=' ' (ATTR_WDUMMY). Including the
             * dummy injects a spurious space after every wide character. */
            if (x < ncols && (rc[x].attr.flags & ATTR_WDUMMY)) continue;
            /* A cell that was never written is a blank, not nothing. Dropping
             * it would close up the gap a program left with a cursor move, so
             * a copied table would come back with its columns run together.
             * Same for a cell past a trimmed cold row's width. */
            u32 cp = (x < ncols) ? rc[x].codepoint : ' ';
            if (cp == 0) cp = ' ';
            if (cp < 32) continue;
            if (cp < 128) {
                /* The common byte, inline: copying a screen is mostly this
                 * store, and a call per character would show. */
                if (out.len + 1u < out.cap) out.data[out.len++] = (char)cp;
                else { char c = (char)cp; if (!sel_put(&out, &c, 1)) goto fail; }
            } else {
                /* Cluster-aware: one cell can hold a whole grapheme
                 * (👨‍👩‍👧, ❤️), so copying must emit every codepoint in it,
                 * not just the base — otherwise the pasted text is a
                 * different emoji from the one on screen. */
                u8 utf[UTF8_CLUSTER_MAX * 4];
                u32 len = utf8_cluster_encode(cp, utf, sizeof(utf));
                if (!sel_put(&out, utf, len)) goto fail;
            }
        }
        if (y < ey && !joins_next && !sel_put(&out, "\n", 1)) goto fail;
    }
    if (!sel_reserve(&out, 0)) goto fail;      /* the NUL; only when nothing was written */
    out.data[out.len] = '\0';
    /* Hand back what the text needs. The doubling above can leave nearly
     * half the buffer unused, and the clipboard copy holds this for as long
     * as it holds the selection. A failed shrink keeps the larger block. */
    if (out.len + 1u < out.cap) {
        char *d = realloc(out.data, out.len + 1u);
        if (d) out.data = d;
    }
    return out.data;

fail:
    free(out.data);
    return NULL;
}
