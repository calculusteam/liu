/*
 * Liu - the focused pane's text in AT-SPI's units (Linux screen readers)
 *
 * The provider (platform_a11y.h) lays the text out in UTF-16 code units,
 * which is what NSAccessibility counts. AT-SPI counts CHARACTERS: one per
 * Unicode code point, so an emoji outside the BMP is one character where
 * UTF-16 has two units. This header is the ONE place the two are converted —
 * a snapshot is copied out of the provider's buffers (which live only until
 * its next call) and every table re-expressed in characters — and the place
 * AT-SPI's text units are found: characters (a cell's whole cluster), words,
 * sentences and paragraphs (a terminal line between hard breaks), and the
 * visual lines the provider already numbers.
 *
 * The converted tables keep the provider's SHAPE, so the helpers of
 * platform_a11y.h (line for an index, a range's cell box, the index at a
 * cell) run on `AtspiText.s` unchanged, in characters instead of units. One
 * departure: a cursor standing past its row's trimmed text gets the blanks up
 * to it, so AT-SPI's caret is where the next character will appear.
 *
 * Also here: the edits that turn the text an assistive app last heard into
 * the current one (atspi_text_diff), and the UTF-8 cleaning every string goes
 * through before libdbus sees it — libdbus ABORTS the process on a string
 * that is not valid UTF-8, and a window title is whatever a program set.
 *
 * Pure C over a snapshot, so it runs without a bus. Everything runs on the
 * main thread.
 */
#ifndef LIU_PLATFORM_ATSPI_TEXT_H
#define LIU_PLATFORM_ATSPI_TEXT_H

#include "core/types.h"
#include "core/utf8.h"
#include "platform/platform_a11y.h"

#include <stdlib.h>
#include <string.h>

/* AT-SPI's text enums, by value (atspi-constants.h; nothing links libatspi). */
enum {
    ATSPI_GRAN_CHAR = 0,
    ATSPI_GRAN_WORD = 1,
    ATSPI_GRAN_SENTENCE = 2,
    ATSPI_GRAN_LINE = 3,
    ATSPI_GRAN_PARAGRAPH = 4,
};
enum {
    ATSPI_BOUND_CHAR = 0,
    ATSPI_BOUND_WORD_START = 1,
    ATSPI_BOUND_WORD_END = 2,
    ATSPI_BOUND_SENTENCE_START = 3,
    ATSPI_BOUND_SENTENCE_END = 4,
    ATSPI_BOUND_LINE_START = 5,
    ATSPI_BOUND_LINE_END = 6,
};

/* One snapshot, in characters. Zero-initialise; reuse across loads (the
 * buffers only grow); atspi_text_free releases it. */
typedef struct {
    /* The provider's description re-aimed at the arrays below: length16 is the
     * CHARACTER count, line_start16 / line_text_end16 / col16 are per
     * character, sel_* and cursor16 are characters. text / label / sel_text
     * point at this struct's own cleaned copies. */
    PlatformA11yText s;
    bool  valid;        /* a load succeeded and `s` describes something */
    f64   built_at;     /* the caller's clock, for its cache policy */

    u32   n;            /* characters */
    u32   len16;        /* the provider's UTF-16 length it was built from */
    u32   pad_at;       /* blanks added before the cursor (see atspi_text_load): */
    u32   pad_len;      /* characters [pad_at, pad_at + pad_len), none when 0 */
    u32  *cp;           /* n: each character's code point (as cleaned) */
    u32  *byte_of;      /* n + 1: where each character starts in `text` */
    u32  *u16_of;       /* n + 1: the provider's UTF-16 index of each character */
    u32  *char_of_u16;  /* len16 + 1: the character a UTF-16 index falls in */
    u16  *col;          /* n */
    char *text;         /* text_bytes + NUL */
    u32   text_bytes;
    u32  *line_start;   /* lines + 1 */
    u32  *line_text_end;/* lines */
    u16  *line_end_col; /* lines */
    char *label;
    char *sel_text;

    u32   cap_chars, cap_u16, cap_bytes, cap_label, cap_sel;
    i32   cap_lines;
} AtspiText;

/* ---- UTF-8 that libdbus accepts ------------------------------------------ */

/* A code point every libdbus accepts in a string: a Unicode scalar value that
 * is not NUL and not a noncharacter (older libdbus refused those). */
static inline bool atspi_cp_sendable(u32 c) {
    if (c == 0 || c > 0x10FFFFu) return false;
    if (c >= 0xD800u && c <= 0xDFFFu) return false;
    if (c >= 0xFDD0u && c <= 0xFDEFu) return false;
    if ((c & 0xFFFEu) == 0xFFFEu) return false;
    return true;
}

/* Copy `len` bytes of `in` into `out` (capacity `cap`, always NUL-terminated
 * when cap > 0) with every undecodable byte and every unsendable code point
 * replaced by U+FFFD. A sequence that does not fit is dropped whole, never
 * cut. Returns the bytes written. */
static inline u32 atspi_utf8_clean(const char *in, u32 len, char *out, u32 cap) {
    if (!out || cap == 0) return 0;
    u32 o = 0;
    for (u32 i = 0; in && i < len; ) {
        u32 c = 0;
        u32 k = utf8_decode((const u8 *)in + i, len - i, &c);
        if (k == 0) { c = 0xFFFDu; k = 1; }
        else if (!atspi_cp_sendable(c)) c = 0xFFFDu;
        u8 enc[4];
        u32 m = utf8_encode(c, enc);
        if (o + m + 1 > cap) break;
        memcpy(out + o, enc, m);
        o += m;
        i += k;
    }
    out[o] = '\0';
    return o;
}

/* ---- loading a snapshot ---------------------------------------------------- */

static inline bool atspi_text_grow(void **p, u32 *cap, u32 need, usize elem) {
    if (need <= *cap) return true;
    u32 c = *cap ? *cap : 64u;
    while (c < need) {
        if (c > 0x3FFFFFFFu) return false;
        c *= 2u;
    }
    void *q = realloc(*p, (usize)c * elem);
    if (!q) return false;
    *p = q;
    *cap = c;
    return true;
}

/* A cleaned copy of a C string into a growable buffer. */
static inline bool atspi_text_copy_str(char **dst, u32 *cap, const char *src, u32 len) {
    u32 need = len * 3u + 1u;          /* every byte could become U+FFFD */
    if (!atspi_text_grow((void **)dst, cap, need, 1)) return false;
    atspi_utf8_clean(src, len, *dst, *cap);
    return true;
}

static inline void atspi_text_free(AtspiText *t) {
    if (!t) return;
    free(t->cp); free(t->byte_of); free(t->u16_of); free(t->char_of_u16);
    free(t->col); free(t->text); free(t->line_start); free(t->line_text_end);
    free(t->line_end_col); free(t->label); free(t->sel_text);
    memset(t, 0, sizeof(*t));
}

/* The four per-character arrays share one capacity: grow them together. */
static inline bool atspi_text_reserve_chars(AtspiText *t, u32 need) {
    if (need <= t->cap_chars) return true;
    u32 c = t->cap_chars ? t->cap_chars : 64u;
    while (c < need) {
        if (c > 0x3FFFFFFFu) return false;
        c *= 2u;
    }
    u32 *cp = realloc(t->cp, (usize)c * sizeof(u32));
    if (cp) t->cp = cp;
    u32 *bo = realloc(t->byte_of, (usize)c * sizeof(u32));
    if (bo) t->byte_of = bo;
    u32 *uo = realloc(t->u16_of, (usize)c * sizeof(u32));
    if (uo) t->u16_of = uo;
    u16 *cl = realloc(t->col, (usize)c * sizeof(u16));
    if (cl) t->col = cl;
    if (!cp || !bo || !uo || !cl) return false;
    t->cap_chars = c;
    return true;
}

/* An empty description that still carries the provider's generations, the
 * pane it names and the label, so change detection and the object's name
 * keep working. */
static inline void atspi_text_set_empty(AtspiText *t, const PlatformA11yText *src) {
    PlatformA11yText e;
    memset(&e, 0, sizeof(e));
    if (src) {
        e.gen_value = src->gen_value;
        e.gen_selection = src->gen_selection;
        e.source = src->source;
        e.has_geometry = src->has_geometry;
        e.grid_x = src->grid_x; e.grid_y = src->grid_y;
        e.cell_w = src->cell_w; e.cell_h = src->cell_h;
    }
    t->n = 0;
    t->len16 = 0;
    t->pad_at = t->pad_len = 0;
    t->text_bytes = 0;
    if (atspi_text_grow((void **)&t->text, &t->cap_bytes, 1, 1)) t->text[0] = '\0';
    if (atspi_text_reserve_chars(t, 1)) { t->byte_of[0] = 0; t->u16_of[0] = 0; }
    e.text = t->text;
    e.label = t->label;
    t->s = e;
    t->valid = false;
}

/* Copy the provider's snapshot `src` into `t`, in characters. False when it
 * describes nothing, when memory runs out, or when its UTF-16 tables do not
 * add up to its text (they always should; the description then degrades to
 * empty rather than to wrong offsets). `t` is usable either way. */
static inline bool atspi_text_load(AtspiText *t, const PlatformA11yText *src) {
    if (!t) return false;
    if (src && src->label) {
        if (!atspi_text_copy_str(&t->label, &t->cap_label, src->label,
                                 (u32)strlen(src->label))) {
            atspi_text_set_empty(t, src);
            return false;
        }
    } else if (t->label) {
        t->label[0] = '\0';
    }
    if (!src || !src->text || src->line_count <= 0 || !src->line_start16 ||
        !src->col16 || !src->line_text_end16 || !src->line_end_col) {
        atspi_text_set_empty(t, src);
        return false;
    }

    /* Pass 1: count characters and check the UTF-16 total. */
    u32 nchars = 0, units = 0;
    for (u32 i = 0; i < src->text_bytes; ) {
        u32 c = 0;
        u32 k = utf8_decode((const u8 *)src->text + i, src->text_bytes - i, &c);
        if (k == 0) { atspi_text_set_empty(t, src); return false; }
        units += c >= 0x10000u ? 2u : 1u;
        nchars++;
        i += k;
    }
    i32 lines = src->line_count;
    if (units != src->length16 || src->line_start16[lines] != src->length16) {
        atspi_text_set_empty(t, src);
        return false;
    }

    /* If the cursor is past the row's last character, extend the accessible
     * row with spaces through the cursor column. This keeps the
     * accessibility caret aligned with the terminal cursor when the provider
     * would otherwise trim trailing blanks. */
    u32 pad = 0, pad_at16 = 0;
    i32 pad_row = -1;
    if (src->has_cursor && src->cursor16 <= units) {
        i32 cr = a11y_line_for_index(src, src->cursor16);
        u32 te = src->line_text_end16[cr];
        bool row_ends = cr + 1 >= lines || src->line_start16[cr + 1] != te;
        i32 want = src->cursor_col < src->cols ? src->cursor_col : src->cols;
        if (src->cursor16 == te && row_ends && want > (i32)src->line_end_col[cr]) {
            pad = (u32)(want - (i32)src->line_end_col[cr]);
            pad_at16 = te;
            pad_row = cr;
        }
    }
    nchars += pad;

    if (!atspi_text_reserve_chars(t, nchars + 1u)) goto oom;
    if (!atspi_text_grow((void **)&t->char_of_u16, &t->cap_u16, units + 1u, sizeof(u32))) goto oom;
    /* A noncharacter of 4 bytes becomes U+FFFD of 3: never longer. */
    if (!atspi_text_grow((void **)&t->text, &t->cap_bytes, src->text_bytes + pad + 1u, 1)) goto oom;
    if (lines + 1 > t->cap_lines) {
        u32 *ls = realloc(t->line_start, (usize)(lines + 1) * sizeof(u32));
        if (ls) t->line_start = ls;
        u32 *le = realloc(t->line_text_end, (usize)(lines + 1) * sizeof(u32));
        if (le) t->line_text_end = le;
        u16 *ec = realloc(t->line_end_col, (usize)(lines + 1) * sizeof(u16));
        if (ec) t->line_end_col = ec;
        if (!ls || !le || !ec) goto oom;
        t->cap_lines = lines + 1;
    }

    /* Pass 2: the characters, their bytes as cleaned, and both index maps.
     * The cursor row's blanks go in before the provider's unit at pad_at16,
     * so that unit (its '\n', or the end) maps to the character after them;
     * a blank has no unit of its own and names the row's end. */
    u32 ci = 0, u16i = 0, bo = 0;
    t->pad_at = 0;
    t->pad_len = 0;
    for (u32 i = 0; i <= src->text_bytes; ) {
        if (pad && u16i == pad_at16) {
            t->pad_at = ci;
            t->pad_len = pad;
            for (u32 k = 0; k < pad; k++, ci++) {
                t->cp[ci] = ' ';
                t->byte_of[ci] = bo;
                t->u16_of[ci] = u16i;
                t->col[ci] = (u16)(src->line_end_col[pad_row] + k);
                t->text[bo++] = ' ';
            }
            pad = 0;
        }
        if (i == src->text_bytes) break;
        u32 c = 0;
        u32 k = utf8_decode((const u8 *)src->text + i, src->text_bytes - i, &c);
        u32 w = c >= 0x10000u ? 2u : 1u;
        u32 sc = atspi_cp_sendable(c) ? c : 0xFFFDu;
        t->cp[ci] = sc;
        t->byte_of[ci] = bo;
        t->u16_of[ci] = u16i;
        t->col[ci] = src->col16[u16i];
        for (u32 j = 0; j < w; j++) t->char_of_u16[u16i + j] = ci;
        bo += utf8_encode(sc, (u8 *)t->text + bo);
        u16i += w;
        ci++;
        i += k;
    }
    t->byte_of[nchars] = bo;
    t->u16_of[nchars] = units;
    t->char_of_u16[units] = nchars;
    t->text[bo] = '\0';
    t->n = nchars;
    t->len16 = units;
    t->text_bytes = bo;

    for (i32 l = 0; l <= lines; l++) {
        u32 a = src->line_start16[l];
        t->line_start[l] = t->char_of_u16[a > units ? units : a];
    }
    for (i32 l = 0; l < lines; l++) {
        u32 e = src->line_text_end16[l];
        t->line_text_end[l] = t->char_of_u16[e > units ? units : e];
        t->line_end_col[l] = src->line_end_col[l];
    }
    if (pad_row >= 0) {
        /* The row now ends at the cursor, and its '\n' carries that column. */
        u16 end = (u16)(src->line_end_col[pad_row] + t->pad_len);
        t->line_end_col[pad_row] = end;
        u32 nl = t->pad_at + t->pad_len;
        if (nl < t->n && t->cp[nl] == '\n') t->col[nl] = end;
    }

    if (src->sel_text && src->sel_text_bytes) {
        if (!atspi_text_copy_str(&t->sel_text, &t->cap_sel, src->sel_text, src->sel_text_bytes))
            goto oom;
    } else if (t->sel_text) {
        t->sel_text[0] = '\0';
    }

    PlatformA11yText s = *src;
    s.text            = t->text;
    s.text_bytes      = bo;
    s.length16        = nchars;
    s.line_start16    = t->line_start;
    s.line_text_end16 = t->line_text_end;
    s.line_end_col    = t->line_end_col;
    s.col16           = t->col;
    s.label           = t->label;
    s.sel_text        = (src->sel_text && src->sel_text_bytes) ? t->sel_text : NULL;
    s.sel_text_bytes  = s.sel_text ? (u32)strlen(s.sel_text) : 0;
    if (s.has_selection) {
        u32 a = src->sel_start16 > units ? units : src->sel_start16;
        u32 e = src->sel_len16 > units - a ? units : a + src->sel_len16;
        u32 ca = t->char_of_u16[a];
        /* An end inside a surrogate pair still covers that character. */
        u32 ce = t->char_of_u16[e];
        if (e < units && t->u16_of[ce] != e) ce++;
        s.sel_start16 = ca;
        s.sel_len16 = ce > ca ? ce - ca : 0;
    }
    if (s.has_cursor)
        s.cursor16 = t->char_of_u16[src->cursor16 > units ? units : src->cursor16];
    t->s = s;
    t->valid = true;
    return true;

oom:
    atspi_text_set_empty(t, src);
    return false;
}

/* The provider's UTF-16 index of character `ci`, and back. Out-of-range input
 * clamps to the end; a UTF-16 index inside a surrogate pair names the
 * character it is part of; a blank added before the cursor has no unit of its
 * own and names the row's end. */
static inline u32 atspi_u16_of_char(const AtspiText *t, u32 ci) {
    if (!t || t->n == 0 || !t->u16_of) return 0;
    return t->u16_of[ci > t->n ? t->n : ci];
}
static inline u32 atspi_char_of_u16(const AtspiText *t, u32 u) {
    if (!t || t->len16 == 0 || !t->char_of_u16) return 0;
    return t->char_of_u16[u > t->len16 ? t->len16 : u];
}

/* ---- units of text ---------------------------------------------------------- */

/* Clamp an AT-SPI offset (an i32 that may be negative or past the end). */
static inline u32 atspi_clamp_offset(const AtspiText *t, i32 off) {
    if (off < 0) return 0;
    return (u32)off > t->n ? t->n : (u32)off;
}

/* A letter or digit as far as reading by words goes. ASCII is exact; past it,
 * everything is part of a word EXCEPT spaces, punctuation and the symbol
 * blocks terminal programs draw with (box drawing, blocks, braille spinners,
 * arrows, private-use icon glyphs, emoji and pictographs). */
static inline bool atspi_is_word_cp(u32 c) {
    if (c < 0x80u)
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z') || c == '_';
    if (c < 0xC0u) return c == 0xAAu || c == 0xB5u || c == 0xBAu;   /* ª µ º */
    if (c == 0xD7u || c == 0xF7u) return false;                     /* × ÷ */
    if (c == 0x1680u || c == 0x2028u || c == 0x2029u) return false;
    if (c >= 0x2000u && c <= 0x2BFFu) return false;   /* spaces, punctuation, symbols,
                                                         arrows, box drawing, shapes */
    if (c >= 0x2800u && c <= 0x28FFu) return false;   /* braille (spinners) */
    if (c >= 0x2E00u && c <= 0x2E7Fu) return false;   /* supplemental punctuation */
    if (c >= 0x3000u && c <= 0x303Fu) return false;   /* CJK symbols and punctuation */
    if (c >= 0xE000u && c <= 0xF8FFu) return false;   /* private use (icon fonts) */
    if (c >= 0xFE10u && c <= 0xFE1Fu) return false;   /* vertical forms */
    if (c >= 0xFE30u && c <= 0xFE6Fu) return false;   /* compatibility forms */
    if (c >= 0xFF01u && c <= 0xFF0Fu) return false;   /* fullwidth punctuation */
    if (c >= 0xFF1Au && c <= 0xFF20u) return false;
    if (c >= 0xFF3Bu && c <= 0xFF40u) return false;
    if (c >= 0xFF5Bu && c <= 0xFF65u) return false;
    if (c == 0xFFFDu) return false;
    if (c >= 0x1F000u && c <= 0x1FBFFu) return false; /* emoji, pictographs */
    if (c >= 0xF0000u) return false;                  /* supplementary private use */
    return true;
}

/* Character `i` belongs to a word: a word character, or a '.' / apostrophe
 * BETWEEN two of them, so "main.c", "1.5" and "don't" read as one word. */
static inline bool atspi_word_at(const AtspiText *t, u32 i) {
    if (i >= t->n) return false;
    u32 c = t->cp[i];
    if (atspi_is_word_cp(c)) return true;
    if ((c == '.' || c == '\'' || c == 0x2019u) && i > 0 && i + 1 < t->n)
        return atspi_is_word_cp(t->cp[i - 1]) && atspi_is_word_cp(t->cp[i + 1]);
    return false;
}

/* Character `i` starts a unit of reading: not a continuation of the cell
 * before it. The code points of one cell (a cluster, a base and its marks)
 * share its column, and a character is never split from them. */
static inline bool atspi_char_starts(const AtspiText *t, u32 i) {
    if (i == 0 || i >= t->n) return true;
    if (t->cp[i] == '\n' || t->cp[i - 1] == '\n') return true;
    if (t->col[i] != t->col[i - 1]) return true;
    return a11y_line_for_index(&t->s, i) != a11y_line_for_index(&t->s, i - 1);
}

/* Where the reading unit that holds character `i` ends. */
static inline u32 atspi_char_end(const AtspiText *t, u32 i) {
    if (i >= t->n) return t->n;
    u32 e = i + 1;
    while (e < t->n && !atspi_char_starts(t, e)) e++;
    return e;
}

/* Is `pos` (0..n) a boundary of `type` in its own right? The start and end of
 * the text are where every walk below stops anyway; this says whether a unit
 * really begins (or ends) there. */
static inline bool atspi_real_boundary(const AtspiText *t, u32 type, u32 pos) {
    u32 n = t->n;
    switch (type) {
    case ATSPI_BOUND_CHAR:
        return pos < n && atspi_char_starts(t, pos);
    case ATSPI_BOUND_WORD_START:
        return pos < n && atspi_word_at(t, pos) && (pos == 0 || !atspi_word_at(t, pos - 1));
    case ATSPI_BOUND_WORD_END:
        return pos > 0 && atspi_word_at(t, pos - 1) && (pos >= n || !atspi_word_at(t, pos));
    case ATSPI_BOUND_SENTENCE_START:
        /* An empty paragraph after a final '\n' starts at the very end. */
        if (pos == 0) return n > 0;
        return pos <= n && t->cp[pos - 1] == '\n';
    case ATSPI_BOUND_SENTENCE_END:
        return pos >= n ? n > 0 : t->cp[pos] == '\n';
    case ATSPI_BOUND_LINE_START: {
        /* The end of the text is a line start only for an empty last row. */
        if (t->s.line_count <= 0 || pos > n) return false;
        i32 l = a11y_line_for_index(&t->s, pos);
        return t->line_start[l] == pos;
    }
    case ATSPI_BOUND_LINE_END: {
        if (t->s.line_count <= 0 || pos == 0 || pos > n) return false;
        i32 l = a11y_line_for_index(&t->s, pos);
        if (t->line_text_end[l] == pos) return true;
        return l > 0 && t->line_text_end[l - 1] == pos;
    }
    default:
        return false;
    }
}

static inline bool atspi_boundary(const AtspiText *t, u32 type, u32 pos) {
    return pos == 0 || pos >= t->n || atspi_real_boundary(t, type, pos);
}

/* GetTextBeforeOffset / GetTextAtOffset / GetTextAfterOffset (`which` -1, 0,
 * +1): the span between two boundaries of `type`. "At" runs from the last
 * boundary at or before the offset to the first one after it — so WORD_START
 * is a word and the blanks after it, WORD_END the blanks before a word and
 * the word, LINE_START a line with its '\n', LINE_END the '\n' before a line
 * and the line. The end of the text belongs to the unit before it, unless a
 * unit really starts there (an empty last line) — except for CHAR: there is
 * no character at the end. */
static inline void atspi_text_bounded(const AtspiText *t, i32 offset, u32 type, i32 which,
                                      u32 *out_start, u32 *out_end) {
    u32 n = t->n;
    if (type > ATSPI_BOUND_LINE_END) type = ATSPI_BOUND_CHAR;
    u32 off = atspi_clamp_offset(t, offset);
    u32 s = off;
    if (s == n && n > 0 && type != ATSPI_BOUND_CHAR && !atspi_real_boundary(t, type, n)) s = n - 1;
    if (s < n)
        while (s > 0 && !atspi_real_boundary(t, type, s)) s--;
    u32 e = off >= n ? n : off + 1;
    while (e < n && !atspi_boundary(t, type, e)) e++;
    if (s == n) e = n;
    if (which < 0) {
        if (s == 0) { e = 0; }
        else {
            e = s;
            s = e - 1;
            while (s > 0 && !atspi_real_boundary(t, type, s)) s--;
        }
    } else if (which > 0) {
        if (e >= n) { s = n; e = n; }
        else {
            s = e;
            e = s + 1;
            while (e < n && !atspi_boundary(t, type, e)) e++;
        }
    }
    *out_start = s;
    *out_end = e;
}

/* GetStringAtOffset: the unit of `granularity` at `offset`.
 *   CHAR       the reading unit (a whole cell's cluster) at the offset;
 *   WORD       the word the offset is in, else the word before it, else the
 *              word after it — the word alone, no blanks;
 *   LINE       the visual row, with its '\n' when it has one (a row that
 *              soft-wraps has none: its text runs on in the next row);
 *   SENTENCE,
 *   PARAGRAPH  the terminal line between hard breaks, with its '\n'. A
 *              terminal draws no sentences; the line a command or a message
 *              was printed as is the nearest thing it has.
 * An offset at the end of the text belongs to the last line. */
static inline void atspi_text_granule(const AtspiText *t, i32 offset, u32 granularity,
                                      u32 *out_start, u32 *out_end) {
    u32 n = t->n;
    u32 off = atspi_clamp_offset(t, offset);
    u32 s = off, e = off;
    switch (granularity) {
    case ATSPI_GRAN_CHAR:
        if (off < n) {
            while (s > 0 && !atspi_char_starts(t, s)) s--;
            e = atspi_char_end(t, s);
        }
        break;
    case ATSPI_GRAN_WORD: {
        u32 at = off;
        if (!atspi_word_at(t, at)) {
            u32 b = at;
            while (b > 0 && !atspi_word_at(t, b - 1)) b--;
            if (b > 0) at = b - 1;
            else {
                u32 f = at;
                while (f < n && !atspi_word_at(t, f)) f++;
                at = f;
            }
        }
        if (at < n && atspi_word_at(t, at)) {
            s = at;
            while (s > 0 && atspi_word_at(t, s - 1)) s--;
            e = at + 1;
            while (e < n && atspi_word_at(t, e)) e++;
        }
        break;
    }
    case ATSPI_GRAN_LINE:
        if (t->s.line_count > 0) {
            i32 l = a11y_line_for_index(&t->s, off);
            s = t->line_start[l];
            e = t->line_start[l + 1];
        }
        break;
    case ATSPI_GRAN_SENTENCE:
    case ATSPI_GRAN_PARAGRAPH:
    default:
        while (s > 0 && t->cp[s - 1] != '\n') s--;
        while (e < n && t->cp[e] != '\n') e++;
        if (e < n) e++;
        break;
    }
    *out_start = s;
    *out_end = e;
}

/* The bytes of characters [start, end) of the text, clamped. */
static inline const char *atspi_text_slice(const AtspiText *t, u32 start, u32 end, u32 *out_bytes) {
    *out_bytes = 0;
    if (!t->text || !t->byte_of) return "";
    if (start > t->n) start = t->n;
    if (end > t->n) end = t->n;
    if (end < start) end = start;
    *out_bytes = t->byte_of[end] - t->byte_of[start];
    return t->text + t->byte_of[start];
}

/* ---- what changed ------------------------------------------------------------ */

/* One step of turning the old text into the new one, applied in order: a
 * deletion of `len` characters at `at` (taken from the OLD text at `src`),
 * or an insertion of `len` characters at `at` (from the NEW text at `src`). */
typedef struct {
    bool del;
    u32  at;
    u32  len;
    u32  src;
} AtspiTextEdit;

static inline u64 atspi_line_hash(const AtspiText *t, i32 l) {
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = t->line_start[l]; i < t->line_start[l + 1]; i++)
        h = (h ^ (u64)t->cp[i]) * 0x100000001B3ull;
    return h;
}

/* Common prefix and suffix of old[from..] and new[], never overlapping. */
static inline void atspi_text_affix(const AtspiText *o, u32 from, const AtspiText *n,
                                    u32 *out_pre, u32 *out_suf) {
    u32 ol = o->n - from, nl = n->n;
    u32 lim = ol < nl ? ol : nl;
    u32 p = 0;
    while (p < lim && o->cp[from + p] == n->cp[p]) p++;
    u32 s = 0;
    while (s < lim - p && o->cp[o->n - 1 - s] == n->cp[nl - 1 - s]) s++;
    *out_pre = p;
    *out_suf = s;
}

/* The edits (at most three) that turn `o` into `n`: rows that scrolled off
 * the top, then the one span in between that differs. Output that scrolls the
 * screen is therefore "the top rows went, the new rows came at the bottom"
 * rather than the whole screen deleted and inserted again, and typing one
 * character is one insertion. A scroll is looked for only when both describe
 * the same number of rows; it is taken when it changes fewer characters. */
static inline u32 atspi_text_diff(const AtspiText *o, const AtspiText *n, AtspiTextEdit out[3]) {
    u32 k_chars = 0;
    i32 rows = o->s.line_count;
    if (rows > 1 && rows == n->s.line_count && rows <= 4096) {
        u64 *ho = malloc((usize)rows * 2u * sizeof(u64));
        if (ho) {
            u64 *hn = ho + rows;
            for (i32 l = 0; l < rows; l++) { ho[l] = atspi_line_hash(o, l); hn[l] = atspi_line_hash(n, l); }
            i32 m0 = 0;
            while (m0 < rows && ho[m0] == hn[m0]) m0++;
            i32 best_k = 0, best_m = 0;
            for (i32 k = 1; k < rows; k++) {
                i32 m = 0;
                while (k + m < rows && ho[k + m] == hn[m]) m++;
                if (m > best_m) { best_m = m; best_k = k; }
            }
            free(ho);
            if (best_k > 0 && best_m > m0) {
                u32 p0, s0, pk, sk;
                atspi_text_affix(o, 0, n, &p0, &s0);
                u32 d = o->line_start[best_k];
                atspi_text_affix(o, d, n, &pk, &sk);
                u64 cost0 = (u64)(o->n - p0 - s0) + (u64)(n->n - p0 - s0);
                u64 costk = (u64)d + (u64)(o->n - d - pk - sk) + (u64)(n->n - pk - sk);
                if (costk < cost0) k_chars = d;
            }
        }
    }
    u32 cnt = 0;
    if (k_chars > 0) out[cnt++] = (AtspiTextEdit){ .del = true, .at = 0, .len = k_chars, .src = 0 };
    u32 p, s;
    atspi_text_affix(o, k_chars, n, &p, &s);
    u32 dl = o->n - k_chars - p - s;
    u32 il = n->n - p - s;
    if (dl > 0) out[cnt++] = (AtspiTextEdit){ .del = true, .at = p, .len = dl, .src = k_chars + p };
    if (il > 0) out[cnt++] = (AtspiTextEdit){ .del = false, .at = p, .len = il, .src = p };
    return cnt;
}

#endif /* LIU_PLATFORM_ATSPI_TEXT_H */
