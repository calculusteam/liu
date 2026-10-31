/*
 * Liu — a pane's screen and history as the bytes that draw them again. See
 * screen_replay.h.
 */
#include "terminal/screen_replay.h"
#include "core/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    u8   *data;
    usize len, cap;
    bool  failed;
    /* Where each hard line begins, so the oldest can be cut off whole. */
    usize *line_at;
    usize  lines, lines_cap;
} ReplayBuf;

static void rb_put(ReplayBuf *b, const void *p, usize n) {
    if (b->failed || n == 0) return;
    if (b->len + n > b->cap) {
        usize cap = b->cap ? b->cap : 64 * 1024;
        while (cap < b->len + n) cap *= 2;
        u8 *grown = realloc(b->data, cap);
        if (!grown) { b->failed = true; return; }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void rb_puts(ReplayBuf *b, const char *s) { rb_put(b, s, strlen(s)); }

static void rb_line_start(ReplayBuf *b) {
    if (b->failed) return;
    if (b->lines == b->lines_cap) {
        usize cap = b->lines_cap ? b->lines_cap * 2 : 1024;
        usize *grown = realloc(b->line_at, cap * sizeof *grown);
        if (!grown) { b->failed = true; return; }
        b->line_at = grown;
        b->lines_cap = cap;
    }
    b->line_at[b->lines++] = b->len;
}

/* SGR parameters for one colour; `base` is 30 (foreground) or 40 (background).
 * Index 7 is the default foreground and 0 the default background: the cell
 * cannot tell them from an explicit 37 / 40, and the default reads the same. */
static void sgr_color(char *out, usize cap, u32 c, i32 base) {
    if (IS_TRUECOLOR(c)) {
        snprintf(out, cap, ";%d;2;%u;%u;%u", base + 8, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        return;
    }
    u32 i = c & 0xFF;
    if ((base == 30 && i == FG_DEFAULT) || (base == 40 && i == BG_DEFAULT)) out[0] = '\0';
    else if (i < 8)  snprintf(out, cap, ";%u", (u32)base + i);
    else if (i < 16) snprintf(out, cap, ";%u", (u32)base + 60 + (i - 8));
    else             snprintf(out, cap, ";%d;5;%u", base + 8, i);
}

static bool attr_is_default(const CellAttr *a) {
    u16 shown = a->flags & (u16)~(ATTR_WIDE | ATTR_WDUMMY | ATTR_HYPERLINK);
    return shown == 0 && !IS_TRUECOLOR(a->fg) && (a->fg & 0xFF) == FG_DEFAULT &&
           !IS_TRUECOLOR(a->bg) && (a->bg & 0xFF) == BG_DEFAULT;
}

static bool attr_same(const CellAttr *a, const CellAttr *b) {
    u16 mask = (u16)~(ATTR_WIDE | ATTR_WDUMMY | ATTR_HYPERLINK);
    return a->fg == b->fg && a->bg == b->bg && (a->flags & mask) == (b->flags & mask);
}

/* The whole pen in one SGR, from a reset: no state is carried between
 * sequences, so a stream cut at a line boundary still draws right. */
static void rb_sgr(ReplayBuf *b, const CellAttr *a) {
    char sgr[96] = "\x1b[0";
    usize n = 3;
    static const struct { u16 flag; const char *code; } flags[] = {
        { ATTR_BOLD, ";1" }, { ATTR_DIM, ";2" }, { ATTR_ITALIC, ";3" },
        { ATTR_UNDERLINE, ";4" }, { ATTR_BLINK, ";5" }, { ATTR_INVERSE, ";7" },
        { ATTR_HIDDEN, ";8" }, { ATTR_STRIKETHROUGH, ";9" }, { ATTR_OVERLINE, ";53" },
    };
    for (usize i = 0; i < sizeof flags / sizeof flags[0]; i++)
        if (a->flags & flags[i].flag) n += (usize)snprintf(sgr + n, sizeof sgr - n, "%s", flags[i].code);
    char col[32];
    sgr_color(col, sizeof col, a->fg, 30);
    n += (usize)snprintf(sgr + n, sizeof sgr - n, "%s", col);
    sgr_color(col, sizeof col, a->bg, 40);
    n += (usize)snprintf(sgr + n, sizeof sgr - n, "%s", col);
    if (n < sizeof sgr - 1) sgr[n++] = 'm';
    rb_put(b, sgr, n);
}

/* How many cells of a row carry something: a soft-wrapped row all of them,
 * otherwise up to its last cell that shows (a glyph, or a blank with a
 * background or a decoration). */
static i32 row_extent(const Cell *row, i32 len, bool wrapped) {
    if (wrapped) return len;
    while (len > 0) {
        const Cell *c = &row[len - 1];
        bool blank = (c->codepoint == ' ' || c->codepoint == 0) && !(c->attr.flags & ATTR_WDUMMY);
        if (!blank || !term_cell_attr_plain(c)) break;
        len--;
    }
    return len;
}

/* True when a cell's encoded text holds no control character, C0, DEL or C1:
 * the rule screen_replay_is_inert applies to a whole stream. A cluster is
 * checked code point by code point, since its base alone says nothing of
 * what was joined to it. */
static bool rb_text_plain(const u8 *s, u32 n) {
    u32 i = 0;
    while (i < n) {
        if (s[i] < 0x20 || s[i] == 0x7F) return false;
        if (s[i] < 0x80) { i++; continue; }
        u32 cp = 0;
        u32 k = utf8_decode(s + i, n - i, &cp);
        if (k == 0 || (cp >= 0x80 && cp < 0xA0)) return false;
        i += k;
    }
    return true;
}

/* One row's text. `pen` is the SGR in effect; `pen_default` that it is the
 * terminal's default pen. */
static void rb_row(ReplayBuf *b, const Cell *row, i32 len, bool wrapped,
                   CellAttr *pen, bool *pen_default) {
    i32 n = row_extent(row, len, wrapped);
    for (i32 x = 0; x < n; x++) {
        const Cell *c = &row[x];
        /* The right half of a wide character, and the spacer a wide character
         * left at a row's end: the new terminal places the character itself. */
        if (c->attr.flags & ATTR_WDUMMY) continue;
        if (*pen_default ? !attr_is_default(&c->attr) : !attr_same(&c->attr, pen)) {
            if (attr_is_default(&c->attr)) {
                rb_puts(b, "\x1b[0m");
                *pen_default = true;
            } else {
                rb_sgr(b, &c->attr);
                *pen_default = false;
            }
            *pen = c->attr;
        }
        u32 cp = c->codepoint;
        u8 enc[64];
        u32 w = 0;
        if (cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp < 0xA0))
            w = utf8_cluster_encode(cp, enc, sizeof enc);
        if (w == 0 || !rb_text_plain(enc, w)) { enc[0] = ' '; w = 1; }
        rb_put(b, enc, w);
    }
    if (!wrapped) {
        /* A background must not run on into the next line's first cells. */
        if (!*pen_default) { rb_puts(b, "\x1b[0m"); *pen_default = true; }
        rb_puts(b, "\r\n");
    }
}

/* The live primary screen's last row worth keeping: the cursor's, or below it
 * the last that is not blank. -1 for an empty screen. */
static i32 grid_last_row(const Cell *cells, const bool *wrap, i32 cols, i32 rows, i32 cursor_y) {
    i32 last = -1;
    for (i32 y = rows - 1; y >= 0; y--) {
        if (row_extent(&cells[(usize)y * (usize)cols], cols, wrap && wrap[y]) > 0) { last = y; break; }
    }
    /* The cursor's row counts when there is text on or above it. */
    if (last >= 0 && cursor_y > last && cursor_y < rows) {
        bool cursor_row_text = row_extent(&cells[(usize)cursor_y * (usize)cols], cols, false) > 0;
        if (cursor_row_text) last = cursor_y;
    }
    return last;
}

bool screen_replay_build(Terminal *t, u8 **out, usize *out_len) {
    if (!out || !out_len) return false;
    *out = NULL;
    *out_len = 0;
    if (!t || t->cols <= 0 || t->rows <= 0) return true;

    bool alt = (t->mode & MODE_ALT_SCREEN) != 0;
    /* The primary grid: live, or parked behind the alternate screen. */
    const Cell *grid = alt ? t->alt_cells : t->cells;
    const bool *gwrap = alt ? t->alt_line_wrapped : t->line_wrapped;
    i32 gcursor = alt ? t->alt_cursor_y : t->cursor_y;
    i32 last = grid ? grid_last_row(grid, gwrap, t->cols, t->rows, gcursor) : -1;

    i32 sb = t->sb_ring ? t->sb_count : 0;
    i32 total = sb + last + 1;
    if (total <= 0) return true;
    /* The newest SCREEN_REPLAY_MAX_ROWS rows, beginning on a whole line. */
    i32 first = total > SCREEN_REPLAY_MAX_ROWS ? total - SCREEN_REPLAY_MAX_ROWS : 0;

    ReplayBuf b = {0};
    CellAttr pen = {0};
    bool pen_default = true;
    bool prev_wrapped = false, started = false;
    for (i32 r = 0; r < total && !b.failed; r++) {
        const Cell *row = NULL;
        i32 len = 0;
        bool wrapped = false;
        if (r < sb) {
            /* The line's own flag: the viewport reader answers false for
             * every row while the alternate screen is up. */
            const TermLine *ln = &t->sb_ring[(t->sb_head + r) % t->sb_capacity];
            wrapped = ln->cold ? ln->cold->wrapped : ln->wrapped;
        } else {
            i32 y = r - sb;
            wrapped = gwrap && gwrap[y] && y < last;
        }
        /* Rows before the first kept one, and the rest of a line cut there:
         * only their flag is needed, so a history row is not decompressed. */
        if (r < first || (!started && prev_wrapped)) { prev_wrapped = wrapped; continue; }
        if (r < sb) {
            /* History row: a viewport coordinate, as the reader takes it. */
            i32 v = (r - sb) + t->scroll_offset;
            row = terminal_row_unclipped(t, v, &len);
        } else {
            row = &grid[(usize)(r - sb) * (usize)t->cols];
            len = t->cols;
        }
        started = true;
        if (!prev_wrapped) rb_line_start(&b);
        if (row && len > 0) rb_row(&b, row, len, wrapped, &pen, &pen_default);
        else if (!wrapped) rb_puts(&b, "\r\n");
        prev_wrapped = wrapped;
    }
    if (prev_wrapped) {
        /* The last row continued past the screen's end: close the line. */
        if (!pen_default) rb_puts(&b, "\x1b[0m");
        rb_puts(&b, "\r\n");
    }
    if (b.failed) {
        free(b.data);
        free(b.line_at);
        return false;
    }
    /* Over budget: the oldest whole lines go. */
    usize from = 0;
    if (b.len > SCREEN_REPLAY_MAX_BYTES) {
        for (usize i = 0; i < b.lines; i++)
            if (b.len - b.line_at[i] <= SCREEN_REPLAY_MAX_BYTES) { from = b.line_at[i]; break; }
        if (from == 0) from = b.len;   /* one line longer than the budget */
    }
    free(b.line_at);
    if (from > 0) {
        memmove(b.data, b.data + from, b.len - from);
        b.len -= from;
    }
    if (b.len == 0) { free(b.data); return true; }
    *out = b.data;
    *out_len = b.len;
    return true;
}

bool screen_replay_is_inert(const u8 *data, usize len) {
    if (!data) return len == 0;
    usize i = 0;
    while (i < len) {
        u8 c = data[i];
        if (c == 0x1B) {
            /* ESC [ params m — and nothing else. */
            if (i + 1 >= len || data[i + 1] != '[') return false;
            usize j = i + 2;
            while (j < len && ((data[j] >= '0' && data[j] <= '9') || data[j] == ';' || data[j] == ':'))
                j++;
            if (j >= len || data[j] != 'm' || j - i > 96) return false;
            i = j + 1;
            continue;
        }
        if (c == '\r' || c == '\n') { i++; continue; }
        if (c < 0x20 || c == 0x7F) return false;
        if (c < 0x80) { i++; continue; }
        u32 cp = 0;
        u32 n = utf8_decode(data + i, len - i, &cp);
        /* A C1 control is a control however it is spelled. */
        if (n == 0 || (cp >= 0x80 && cp < 0xA0)) return false;
        i += n;
    }
    return true;
}
