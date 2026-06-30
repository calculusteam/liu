/*
 * Liu - smart terminal selection (per-terminal instance)
 * Double-click word select, triple-click line select, rectangular select.
 */
#include "terminal/terminal.h"
#include "core/utf8.h"
#include "platform/platform.h"
#include <stdlib.h>
#include <string.h>

static bool is_word_char(u32 cp) {
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= '0' && cp <= '9') return true;
    if (cp == '_' || cp == '-' || cp == '.') return true;
    if (cp == '/' || cp == '~') return true;
    return false;
}

void selection_start(Terminal *t, i32 col, i32 row, i32 click_count, bool alt_held) {
    TermSelection *s = &t->selection;
    s->active = true;
    s->is_rect = alt_held;
    s->is_word = (click_count == 2);
    s->is_line = (click_count >= 3);

    if (s->is_line) {
        s->start_col = 0;
        s->start_row = row;
        s->end_col = t->cols - 1;
        s->end_row = row;
    } else if (s->is_word) {
        /* Scroll-aware: read the row as currently displayed (scrollback when
         * scrolled up), not the live grid — otherwise word boundaries are
         * computed from the wrong text while scrolled. */
        i32 vlen = 0;
        const Cell *rc = terminal_visible_row(t, row, &vlen);
        i32 ncols = (rc && vlen > 0) ? (vlen < t->cols ? vlen : t->cols) : 0;
        i32 left = col, right = col;
        while (left > 0 && left - 1 < ncols &&
               is_word_char(rc[left - 1].codepoint)) {
            left--;
        }
        while (right < t->cols - 1 && right + 1 < ncols &&
               is_word_char(rc[right + 1].codepoint)) {
            right++;
        }
        s->start_col = left;
        s->start_row = row;
        s->end_col = right;
        s->end_row = row;
    } else {
        s->start_col = col;
        s->start_row = row;
        s->end_col = col;
        s->end_row = row;
    }
}

void selection_update(Terminal *t, i32 col, i32 row) {
    TermSelection *s = &t->selection;
    if (!s->active) return;
    if (s->is_line) {
        s->end_row = row;
        return;
    }
    s->end_col = col;
    s->end_row = row;
}

void selection_clear(Terminal *t) {
    t->selection.active = false;
    t->selection.start_col = -1;
}

bool selection_active(Terminal *t) {
    return t->selection.active && t->selection.start_col >= 0;
}

bool selection_contains(Terminal *t, i32 col, i32 row) {
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
    if (s->is_line) {
        i32 min_row = sy < ey ? sy : ey;
        i32 max_row = sy > ey ? sy : ey;
        return row >= min_row && row <= max_row;
    }
    if (row < sy || row > ey) return false;
    if (row == sy && row == ey) return col >= sx && col <= ex;
    if (row == sy) return col >= sx;
    if (row == ey) return col <= ex;
    return true;
}

char *selection_get_text(Terminal *t) {
    TermSelection *s = &t->selection;
    if (!s->active) return NULL;

    i32 sy = s->start_row, sx = s->start_col;
    i32 ey = s->end_row, ex = s->end_col;
    if (sy > ey || (sy == ey && sx > ex)) {
        i32 tmp;
        tmp = sy; sy = ey; ey = tmp;
        tmp = sx; sx = ex; ex = tmp;
    }

    usize cap = (usize)((ey - sy + 1) * (t->cols + 1) * 4);
    char *buf = malloc(cap + 1);
    if (!buf) return NULL;
    usize pos = 0;

    for (i32 y = sy; y <= ey; y++) {
        i32 x_start, x_end;

        if (s->is_line) {
            /* Line selection always spans the full row width, regardless of
             * which direction the user dragged. Without this, the coordinate
             * swap above corrupts the column bounds: the first/last row would
             * capture only a single column instead of the full row. */
            x_start = 0;
            x_end = t->cols - 1;
        } else if (s->is_rect) {
            i32 min_col = sx < ex ? sx : ex;
            i32 max_col = sx > ex ? sx : ex;
            x_start = min_col;
            x_end = max_col;
        } else {
            x_start = (y == sy) ? sx : 0;
            x_end = (y == ey) ? ex : t->cols - 1;
        }

        /* Read the row as displayed (scroll-aware). The returned pointer is
         * valid only until the next terminal_visible_row call, so we finish
         * this row's reads before advancing to y+1. Cells past `vlen` are
         * trailing blanks (trimmed on cold scrollback rows). */
        i32 vlen = 0;
        const Cell *rc = terminal_visible_row(t, y, &vlen);
        i32 ncols = (rc && vlen > 0) ? (vlen < t->cols ? vlen : t->cols) : 0;

        i32 last_nonspace = x_start - 1;
        for (i32 x = x_start; x <= x_end && x < ncols; x++) {
            if (rc[x].codepoint > 32) last_nonspace = x;
        }

        for (i32 x = x_start; x <= last_nonspace && x < ncols; x++) {
            /* Skip wide-character dummy cells: a CJK/wide glyph occupies two
             * cells — the first carries the codepoint (ATTR_WIDE), the second
             * is a placeholder with codepoint=' ' (ATTR_WDUMMY). Including the
             * dummy injects a spurious space after every wide character. */
            if (rc[x].attr.flags & ATTR_WDUMMY) continue;
            u32 cp = rc[x].codepoint;
            if (cp >= 32) {
                /* Encode base codepoint */
                if (cp < 128) {
                    buf[pos++] = (char)cp;
                } else {
                    u8 utf[4];
                    u32 len = utf8_encode(cp, utf);
                    for (u32 k = 0; k < len && pos < cap; k++)
                        buf[pos++] = (char)utf[k];
                }
            }
        }
        if (y < ey) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    return buf;
}
