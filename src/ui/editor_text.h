/*
 * Liu — the code editor's arithmetic: where a line ends, how it breaks, and
 * which bytes become which cells.
 *
 * Three places turn the bytes of a line into cells on screen — the painter,
 * the caret, and the click that becomes a caret — and they must agree about
 * everything that is not one valid character in one cell:
 *
 *   - a byte that is not UTF-8 (a Latin-1 or Windows-1254 file) takes the same
 *     cell in all three, or letters vanish and the caret drifts right;
 *   - a tab reaches the same tab stop the indent guides beside it count to;
 *   - a NUL is a character like any other: no loop is a C-string loop
 *     (strchr, strlen, `while (*p)`), so the text after it exists;
 *   - the CR of a CRLF line is part of the line break, not a cell.
 *
 * So there is one walk, editor_cell_at(), and everything that needs a column
 * asks it; one line splitter that takes a length; and one answer to "what
 * ends a line here". All of it is arithmetic on a byte range: no FileBrowser,
 * no renderer.
 *
 * What a cell is:
 *
 *   valid character     its glyph, one column
 *   byte not UTF-8      U+FFFD, one column, ONE byte — the next byte gets its
 *                       own cell, so a run of Latin-1 letters keeps its length
 *   NUL                 U+2400 (the "NUL" picture), one column
 *   tab                 nothing drawn, up to the next multiple of the tab width
 *   other C0 control    nothing drawn, one column
 *
 * A line's TEXT stops before the CR of a CRLF: the CR belongs to the break, so
 * it is never a cell and the caret can never sit between it and the LF.
 */
#ifndef UI_EDITOR_TEXT_H
#define UI_EDITOR_TEXT_H

#include "core/types.h"
#include "core/utf8.h"

#include <string.h>

#define EDITOR_CP_INVALID 0xFFFDu   /* a byte that is not UTF-8 */
#define EDITOR_CP_NUL     0x2400u   /* the picture of a NUL */

typedef struct {
    u32 cp;      /* what to draw; 0 = nothing (a tab, a control character) */
    i32 bytes;   /* bytes of the line this cell stands for; 0 past the end */
    i32 cols;    /* columns it takes; 0 past the end */
} EditorCell;

/* The cell that starts at byte `pos` of `line[len]`, when it is drawn from
 * visual column `col` (only a tab's width depends on where it starts). */
static inline EditorCell editor_cell_at(const char *line, i32 len, i32 pos, i32 col, i32 tab) {
    EditorCell c = { 0, 1, 1 };
    if (!line || pos < 0 || pos >= len) { c.bytes = 0; c.cols = 0; return c; }
    u8 b = (u8)line[pos];
    if (b == '\t') {
        if (tab < 1) tab = 1;
        if (col < 0) col = 0;
        c.cols = tab - (col % tab);
        return c;
    }
    if (b == 0) { c.cp = EDITOR_CP_NUL; return c; }
    if (b < 0x20) return c;
    if (b < 0x80) { c.cp = b; return c; }
    u32 cp = 0;
    u32 n = utf8_decode((const u8 *)line + pos, (usize)(len - pos), &cp);
    if (n == 0) { c.cp = EDITOR_CP_INVALID; return c; }
    c.cp = cp;
    c.bytes = (i32)n;
    return c;
}

/* Bytes of the character that starts at `pos`: a whole valid sequence, or one
 * byte of anything else. 0 at or past the end. This is the step the cursor
 * takes, and it is the cell walk's step, so the caret can only ever stand
 * where a cell begins. */
static inline i32 editor_char_len(const char *line, i32 len, i32 pos) {
    if (!line || pos < 0 || pos >= len) return 0;
    if ((u8)line[pos] < 0x80) return 1;
    u32 cp = 0;
    u32 n = utf8_decode((const u8 *)line + pos, (usize)(len - pos), &cp);
    return n ? (i32)n : 1;
}

/* Bytes of the character that ENDS at `pos` (0 at the line's start). The
 * longest valid sequence ending there is the character: a lead byte is never
 * inside another sequence, so if one sits 2-4 bytes back and its sequence
 * reaches exactly `pos`, the forward walk stops there too. Otherwise the byte
 * before `pos` is a cell of its own. */
static inline i32 editor_char_back(const char *line, i32 len, i32 pos) {
    if (!line || pos <= 0) return 0;
    if (pos > len) pos = len;
    for (i32 back = 4; back >= 2; back--) {
        if (back > pos) continue;
        if (editor_char_len(line, pos, pos - back) == back) return back;
    }
    return 1;
}

/* Visual column of byte offset `byte` (clamped to the line). */
static inline i32 editor_col_at(const char *line, i32 len, i32 byte, i32 tab) {
    if (byte > len) byte = len;
    i32 col = 0;
    for (i32 pos = 0; pos < byte; ) {
        /* A run of plain ASCII is a byte and a column each: counted without
         * the cell walk, because the painter asks this of every visible line
         * on every frame and most of every line is exactly that. */
        i32 run = pos;
        while (run < byte && (u8)((u8)line[run] - 0x20u) < 0x5Fu) run++;
        col += run - pos;
        pos = run;
        if (pos >= byte) break;
        EditorCell c = editor_cell_at(line, len, pos, col, tab);
        if (c.bytes <= 0) break;
        pos += c.bytes;
        col += c.cols;
    }
    return col;
}

/* Columns the whole line takes. */
static inline i32 editor_cols(const char *line, i32 len, i32 tab) {
    return editor_col_at(line, len, len, tab);
}

/* The cell boundary nearest to visual column `col`, as a byte offset: the
 * start of the cell that holds the column, or its end when the column is in
 * the cell's right half (which only a tab has). Past the last cell: the end of
 * the line. Always where a cell begins. */
static inline i32 editor_byte_at_col(const char *line, i32 len, i32 col, i32 tab) {
    if (col <= 0) return 0;
    i32 at = 0;
    for (i32 pos = 0; pos < len; ) {
        EditorCell c = editor_cell_at(line, len, pos, at, tab);
        if (c.bytes <= 0) break;
        if (col < at + c.cols)
            return (col - at) * 2 >= c.cols ? pos + c.bytes : pos;
        pos += c.bytes;
        at += c.cols;
    }
    return len;
}

/* The same for a POINT: `col` is a fractional column (a click's x divided by
 * the cell width). The caret goes to whichever edge of the cell under the
 * point is nearer — its right half puts the caret after it, the way a native
 * text field answers, and a four-column tab is judged at ITS middle. */
static inline i32 editor_byte_at_point(const char *line, i32 len, f32 col, i32 tab) {
    if (col <= 0.0f) return 0;
    i32 at = 0;
    for (i32 pos = 0; pos < len; ) {
        EditorCell c = editor_cell_at(line, len, pos, at, tab);
        if (c.bytes <= 0) break;
        if (col < (f32)(at + c.cols))
            return (col - (f32)at) * 2.0f >= (f32)c.cols ? pos + c.bytes : pos;
        pos += c.bytes;
        at += c.cols;
    }
    return len;
}

/* ---- lines, by length ---------------------------------------------------- */

/* Offset of the LF that ends the line starting at `from`, or `size` when the
 * line is the last one. Never reads past `size`, and a NUL is a byte. */
static inline usize editor_line_break(const char *buf, usize size, usize from) {
    if (!buf || from >= size) return size;
    const char *nl = memchr(buf + from, '\n', size - from);
    return nl ? (usize)(nl - buf) : size;
}

/* Where the line's TEXT ends, given its break at `brk` (editor_line_break):
 * before the CR when the break is CRLF. A CR with no LF after it is text. */
static inline usize editor_line_text_end(const char *buf, usize size, usize from, usize brk) {
    if (buf && brk < size && brk > from && buf[brk - 1] == '\r') return brk - 1;
    return brk;
}

/* Bytes of the break itself: 2 for CRLF, 1 for LF, 0 at the end of the text. */
static inline usize editor_line_break_len(const char *buf, usize size, usize from, usize brk) {
    if (brk >= size) return 0;
    return 1 + (brk - editor_line_text_end(buf, size, from, brk));
}

/* Does this text end its lines with CRLF? What DOMINATES, not what comes
 * first: a file that is LF with one stray CRLF is an LF file with a defect.
 * The first megabyte settles it — endings are uniform in practice. */
static inline bool editor_detect_crlf(const char *buf, usize len) {
    if (!buf || len == 0) return false;
    usize crlf = 0, lf = 0;
    usize scan = len < ((usize)1 << 20) ? len : ((usize)1 << 20);
    for (usize i = 0; i < scan; i++) {
        if (buf[i] != '\n') continue;
        if (i > 0 && buf[i - 1] == '\r') crlf++;
        else lf++;
    }
    return crlf > lf;
}

/* Rewrite every line break of in[len] — LF, CRLF, a lone CR — as the FILE's
 * own, into `out`, which must hold 2 * len bytes. Returns the length written.
 * What a paste goes through: the clipboard's endings are whatever the program
 * it came from uses, and the file's are the file's. */
static inline usize editor_convert_eol(const char *in, usize len, char *out, bool crlf) {
    usize n = 0;
    for (usize i = 0; i < len; i++) {
        char c = in[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < len && in[i + 1] == '\n') i++;
            if (crlf) out[n++] = '\r';
            out[n++] = '\n';
        } else {
            out[n++] = c;
        }
    }
    return n;
}

/* ---- what the bytes are, for the status bar ------------------------------ */

typedef enum {
    EDITOR_ENC_UTF8 = 0,
    EDITOR_ENC_UTF16,       /* starts with a UTF-16 byte order mark */
    EDITOR_ENC_NOT_UTF8,    /* text, but some byte is not UTF-8 (Latin-1, Windows-125x, …) */
    EDITOR_ENC_BINARY,      /* holds a NUL */
} EditorEncoding;

/* Judged on the first megabyte, like the line ending: the editor asks again
 * after every edit, and a file is one encoding from its first bytes on. */
static inline EditorEncoding editor_detect_encoding(const char *buf, usize len) {
    if (!buf || len == 0) return EDITOR_ENC_UTF8;
    const u8 *p = (const u8 *)buf;
    if (len >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF)))
        return EDITOR_ENC_UTF16;
    bool invalid = false;
    usize scan = len < ((usize)1 << 20) ? len : ((usize)1 << 20);
    for (usize i = 0; i < scan; ) {
        if (p[i] == 0) return EDITOR_ENC_BINARY;
        if (p[i] < 0x80) { i++; continue; }
        u32 cp = 0;
        u32 n = utf8_decode(p + i, len - i, &cp);
        if (n == 0) { invalid = true; i++; }
        else i += n;
    }
    return invalid ? EDITOR_ENC_NOT_UTF8 : EDITOR_ENC_UTF8;
}

/* The status bar's third field. It said "UTF-8" for every file there is. */
static inline const char *editor_encoding_label(EditorEncoding e) {
    switch (e) {
    case EDITOR_ENC_UTF16:    return "UTF-16";
    case EDITOR_ENC_NOT_UTF8: return "Not UTF-8";
    case EDITOR_ENC_BINARY:   return "Binary?";
    case EDITOR_ENC_UTF8:     break;
    }
    return "UTF-8";
}

#endif /* UI_EDITOR_TEXT_H */
