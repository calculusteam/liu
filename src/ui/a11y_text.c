/*
 * Liu - the terminal's text as the accessibility tree sees it. See a11y_text.h.
 */
#include "ui/a11y_text.h"

#include "core/utf8.h"

#include <stdlib.h>
#include <string.h>

/* ---- generations ---------------------------------------------------------- */

static inline u64 mix64(u64 h, u64 v) {
    /* FNV-1a over the 8 bytes, then a finaliser: order-sensitive, and a +1 in
     * any input moves the result. */
    for (i32 i = 0; i < 8; i++) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    h ^= h >> 29;
    return h;
}

/* A fingerprint of what a reader would be given: the viewport's code points,
 * which cells are HIDDEN (those read as blank), and where rows wrap.
 *
 * The cheap fields below (content_writes, the scrollback origin, the cursor)
 * miss every edit that writes no glyph and does not move the cursor — EL, ED,
 * ECH, ICH, DCH, IL, DL, SU/SD, a hidden-attribute rewrite — and would leave
 * the AX value stale with no notification posted. Hashing the text itself is
 * exact: it moves when what is READ changes and for nothing else (an SGR, a
 * mode, an identical redraw leave it alone). One xor-multiply per cell, run
 * only while VoiceOver asks; terminal_visible_row decompresses a history row
 * once per call, and only while the view is scrolled back. */
static u64 viewport_text_hash(Terminal *t) {
    u64 h = 0x84222325CBF29CE4ull;
    for (i32 r = 0; r < t->rows; r++) {
        i32 len = 0;
        const Cell *c = terminal_visible_row(t, r, &len);
        i32 n = (c && len < t->cols) ? len : t->cols;
        h = (h ^ (u64)(u32)r) * 0x100000001B3ull;
        if (c) {
            for (i32 x = 0; x < n; x++) {
                u32 cp = c[x].codepoint;
                if (c[x].attr.flags & ATTR_HIDDEN) cp = 0x80000000u;   /* reads as blank */
                else if (cp == 0) cp = ' ';
                h = (h ^ (u64)cp) * 0x100000001B3ull;
            }
        }
        h = (h ^ (terminal_visible_row_wrapped(t, r) ? 0x9Eull : 0x61ull)) * 0x100000001B3ull;
    }
    return h;
}

void a11y_text_generation(Terminal *t, u64 *out_value, u64 *out_selection) {
    u64 v = 0xCBF29CE484222325ull, s = 0x9E3779B97F4A7C15ull;
    if (t) {
        /* What the text is made of — the url_hover.h signature (content_writes
         * misses a scroll that writes nothing; sb_abs_base + sb_count catches
         * it) plus the cursor, which a clear-and-home moves even when the
         * erase itself counted no writes. */
        v = mix64(v, (u64)(uintptr_t)t);
        v = mix64(v, t->content_writes);
        v = mix64(v, (u64)(t->sb_abs_base + (i64)t->sb_count));
        v = mix64(v, (u64)(u32)t->scroll_offset);
        v = mix64(v, ((u64)(u32)t->cols << 32) | (u32)t->rows);
        v = mix64(v, (t->mode & MODE_ALT_SCREEN) ? 1u : 0u);
        v = mix64(v, ((u64)(u32)t->cursor_x << 32) | (u32)t->cursor_y);
        v = mix64(v, viewport_text_hash(t));

        const TermSelection *sel = &t->selection;
        if (sel->active && sel->start_col >= 0) {
            s = mix64(s, (u64)(uintptr_t)t);
            s = mix64(s, ((u64)(u32)sel->start_col << 32) | (u32)sel->start_row);
            s = mix64(s, ((u64)(u32)sel->end_col << 32) | (u32)sel->end_row);
            s = mix64(s, (u64)sel->anchor_abs);
            s = mix64(s, sel->is_rect ? 1u : 0u);
            /* The viewport moving under a selection moves its on-screen span. */
            s = mix64(s, (u64)(u32)t->scroll_offset);
            s = mix64(s, (u64)(t->sb_abs_base + (i64)t->sb_count));
        }
    }
    if (out_value)     *out_value = v;
    if (out_selection) *out_selection = s;
}

/* ---- growable storage ----------------------------------------------------- */

static bool grow(void **p, u32 *cap, u32 need, usize elem) {
    if (need <= *cap) return true;
    u32 c = *cap ? *cap : 256u;
    while (c < need) {
        if (c > 0x7FFFFFFFu) return false;
        c *= 2u;
    }
    void *n = realloc(*p, (usize)c * elem);
    if (!n) return false;
    *p = n;
    *cap = c;
    return true;
}

/* Append one cell's bytes, all of whose UTF-16 units map to column `col`. */
static bool put_unit_bytes(A11yTextBuf *b, const u8 *bytes, u32 n, u32 units, u16 col) {
    if (!grow((void **)&b->text, &b->text_cap, b->text_len + n + 1u, 1)) return false;
    if (!grow((void **)&b->col16, &b->col_cap, b->len16 + units, sizeof(u16))) return false;
    memcpy(b->text + b->text_len, bytes, n);
    b->text_len += n;
    for (u32 i = 0; i < units; i++) b->col16[b->len16++] = col;
    return true;
}

/* A cell's text: its UTF-8 and how many UTF-16 units that is. Anything that
 * would not survive the trip into an NSString — a surrogate or out-of-range
 * value in a cell, an undecodable byte — becomes U+FFFD, because one invalid
 * sequence makes -initWithUTF8String: return nil for the WHOLE screen. */
static u32 cell_text(u32 cp, u8 *out, u32 cap, u32 *out_units) {
    static const u8 repl[3] = { 0xEF, 0xBF, 0xBD };
    u32 n = utf8_cluster_encode(cp, out, cap);
    u32 units = 0;
    bool good = n > 0;
    for (u32 i = 0; good && i < n; ) {
        u32 c = 0;
        u32 k = utf8_decode(out + i, n - i, &c);
        if (k == 0 || c > 0x10FFFFu || (c >= 0xD800u && c <= 0xDFFFu) || c < 32u ||
            (c >= 0x7Fu && c <= 0x9Fu)) {
            good = false;
            break;
        }
        units += (c >= 0x10000u) ? 2u : 1u;
        i += k;
    }
    if (!good) {
        memcpy(out, repl, 3);
        *out_units = 1;
        return 3;
    }
    *out_units = units;
    return n;
}

static inline bool cell_blank(const Cell *c) {
    return c->codepoint == 0 || c->codepoint == ' ' || (c->attr.flags & ATTR_HIDDEN);
}

bool a11y_text_build(Terminal *t, A11yTextBuf *b, PlatformA11yText *out) {
    if (!b || !out) return false;
    memset(out, 0, sizeof(*out));
    a11y_text_generation(t, &out->gen_value, &out->gen_selection);
    b->text_len = 0;
    b->len16 = 0;
    free(b->sel_text);
    b->sel_text = NULL;
    if (!t || t->rows <= 0 || t->cols <= 0 || t->cols > 0xFFFF) return false;

    i32 rows = t->rows, cols = t->cols;
    if (rows + 1 > b->line_cap) {
        u32 *ls = realloc(b->line_start16, (usize)(rows + 1) * sizeof(u32));
        if (ls) b->line_start16 = ls;
        u32 *le = realloc(b->line_text_end16, (usize)(rows + 1) * sizeof(u32));
        if (le) b->line_text_end16 = le;
        u16 *ec = realloc(b->line_end_col, (usize)(rows + 1) * sizeof(u16));
        if (ec) b->line_end_col = ec;
        if (!ls || !le || !ec) return false;
        b->line_cap = rows + 1;
    }
    if (!grow((void **)&b->text, &b->text_cap, 1u, 1)) return false;

    /* Once, before the per-cell tests: selection_contains deliberately does
     * not sync (it is the renderer's per-cell test), selection_active does. */
    bool sel_on = selection_active(t);
    bool sel_hit = false;
    u32  sel_a = 0, sel_b = 0;

    i32  cur_row = t->cursor_y + t->scroll_offset;
    bool cur_on  = cur_row >= 0 && cur_row < rows && t->cursor_x >= 0;
    u32  cur16   = 0;

    for (i32 r = 0; r < rows; r++) {
        b->line_start16[r] = b->len16;
        /* The wrap flag first: the row pointer can live in a scratch buffer
         * that the next reader call reuses. The last row's continuation is
         * off screen, so it ends here either way. */
        bool wraps = (r + 1 < rows) && terminal_visible_row_wrapped(t, r);
        i32 vlen = 0;
        const Cell *rc = terminal_visible_row(t, r, &vlen);
        i32 ncols = (rc && vlen > 0) ? (vlen < cols ? vlen : cols) : 0;

        i32 last = -1;
        if (wraps) {
            last = ncols - 1;
        } else {
            for (i32 x = ncols - 1; x >= 0; x--)
                if (!cell_blank(&rc[x]) && !(rc[x].attr.flags & ATTR_WDUMMY)) { last = x; break; }
        }

        i32 end_col = 0;
        for (i32 x = 0; x <= last; x++) {
            const Cell *c = &rc[x];
            if (c->attr.flags & ATTR_WDUMMY) continue;
            u32 cp = c->codepoint;
            u32 a = b->len16;
            if (cp == 0 || (c->attr.flags & ATTR_HIDDEN)) cp = ' ';
            if (cp < 128u) {
                u8 ch = (u8)((cp < 32u || cp == 0x7Fu) ? ' ' : cp);
                if (!put_unit_bytes(b, &ch, 1, 1, (u16)x)) return false;
            } else {
                /* A cell can hold a whole grapheme (👨‍👩‍👧 is 18 bytes, a
                 * skin-toned family more): four bytes is not enough. */
                u8 utf[UTF8_CLUSTER_MAX * 4];
                u32 units = 0;
                u32 n = cell_text(cp, utf, sizeof(utf), &units);
                if (!put_unit_bytes(b, utf, n, units, (u16)x)) return false;
            }
            end_col = x + ((c->attr.flags & ATTR_WIDE) ? 2 : 1);
            if (end_col > cols) end_col = cols;
            if (sel_on && selection_contains(t, x, r)) {
                if (!sel_hit) { sel_a = a; sel_hit = true; }
                sel_b = b->len16;
            }
        }
        if (wraps) end_col = cols;
        b->line_text_end16[r] = b->len16;
        b->line_end_col[r] = (u16)end_col;

        if (cur_on && r == cur_row) {
            cur16 = b->len16;
            for (u32 i = b->line_start16[r]; i < b->len16; i++)
                if ((i32)b->col16[i] >= t->cursor_x) { cur16 = i; break; }
        }

        if (!wraps && r + 1 < rows) {
            u8 nl = '\n';
            if (!put_unit_bytes(b, &nl, 1, 1, (u16)end_col)) return false;
        }
    }
    b->line_start16[rows] = b->len16;
    b->text[b->text_len] = '\0';

    out->text            = b->text;
    out->text_bytes      = b->text_len;
    out->length16        = b->len16;
    out->line_count      = rows;
    out->line_start16    = b->line_start16;
    out->col16           = b->col16;
    out->line_end_col    = b->line_end_col;
    out->line_text_end16 = b->line_text_end16;
    out->cols            = cols;

    if (sel_on) {
        out->has_selection = true;
        if (sel_hit) {
            out->sel_start16 = sel_a;
            out->sel_len16   = sel_b - sel_a;
        } else {
            /* Selected, but nothing of it drawn: only trailing blanks, or it
             * sits wholly above/below the viewport. A caret where it starts. */
            const TermSelection *s = &t->selection;
            i32 sy = s->start_row < s->end_row ? s->start_row : s->end_row;
            out->sel_start16 = sy < 0 ? 0
                             : sy >= rows ? b->len16
                             : b->line_text_end16[sy];
            out->sel_len16 = 0;
        }
        b->sel_text = selection_get_text(t);
        out->sel_text = b->sel_text;
        out->sel_text_bytes = b->sel_text ? (u32)strlen(b->sel_text) : 0;
    }
    if (cur_on) {
        out->has_cursor = true;
        out->cursor16   = cur16;
        out->cursor_col = t->cursor_x < cols ? t->cursor_x : cols;
    }
    return true;
}

void a11y_text_buf_free(A11yTextBuf *b) {
    if (!b) return;
    free(b->text);
    free(b->col16);
    free(b->line_start16);
    free(b->line_text_end16);
    free(b->line_end_col);
    free(b->sel_text);
    memset(b, 0, sizeof(*b));
}
