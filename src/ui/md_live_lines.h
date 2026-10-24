/*
 * Liu — what a SOURCE line is in the Markdown live editor, and how tall.
 *
 * The live editor draws one source line at a time (filebrowser.c): a heading
 * larger, a fence's body as code, everything else as prose. Three places have
 * to agree on which line is which, because they all convert between a line and
 * a pixel row: the painter, the click that puts the caret (fb_md_live_locate)
 * and the walk that finds the first visible line. All three ask this header.
 * A disagreement shows at once:
 *
 *   - a fence toggled on ANY line that begins with three backticks would let a
 *     ```php written inside a ````md block close it, and every line below would
 *     be classified upside down (code as prose, prose as code);
 *   - a `# comment` in a shell block is a code line, and a copy of the rules
 *     that took it for a heading would put every click below it on the wrong
 *     source line.
 *
 * The fence rule is md_parse.c's own (it is the Read view): a fence opens on
 * a run of three or more of ` or ~; it closes on a run of the SAME mark at
 * least as long, with nothing after it but blanks, no more than three columns
 * deeper than the opening one. Anything else in between — including another
 * "```js" — is code. A heading is a heading only outside a fence.
 *
 * Pure, so the arithmetic can be checked without a window.
 */
#ifndef UI_MD_LIVE_LINES_H
#define UI_MD_LIVE_LINES_H

#include "core/types.h"
#include "ui/editor_text.h"

#include <math.h>
#include <string.h>

/* The fence the walk is inside of; mark == 0 is "not in one". */
typedef struct {
    char mark;      /* '`' or '~' */
    i32  run;       /* length of the opening run: a closer must be as long */
    i32  indent;    /* columns the opening fence sat in */
} MdLiveFence;

typedef struct {
    i32  indent;    /* leading blanks, in bytes (a tab is one cell here) */
    i32  heading;   /* 1..6, 0 = not a heading */
    bool fence;     /* the line opens or closes a fence (and is not drawn) */
    bool code;      /* the line is inside a fence: drawn literally */
} MdLiveLine;

/* Heading size relative to a body line. Mirrors H_STEP[].scale in md_render.c,
 * so a heading keeps its size when the user toggles Read <-> Edit. */
static inline f32 md_live_scale(i32 heading) {
    static const f32 S[7] = { 1.0f, 1.85f, 1.42f, 1.20f, 1.06f, 1.00f, 0.94f };
    return (heading >= 1 && heading <= 6) ? S[heading] : 1.0f;
}

/* A glyph cell of a line: a heading's cell is a rounded multiple of the body's,
 * so its columns still land on whole pixels. */
static inline f32 md_live_cell_w(i32 heading, f32 cw) {
    return heading ? roundf(cw * md_live_scale(heading)) : cw;
}

/* The line's height in pixels, the gap below it included. `ch` is the body
 * cell height, `dpi` the scale its gaps are in points of. */
static inline f32 md_live_line_h(i32 heading, f32 ch, f32 dpi) {
    return ch * md_live_scale(heading) + (heading ? 6.0f * dpi : 2.0f * dpi);
}

/* Classify one line (`len` bytes, its CR already dropped) and advance `fs`
 * past it. */
static inline MdLiveLine md_live_classify(const char *line, i32 len, MdLiveFence *fs) {
    MdLiveLine r = { 0, 0, false, false };
    i32 ind = 0;
    while (ind < len && (line[ind] == ' ' || line[ind] == '\t')) ind++;
    r.indent = ind;
    const char *t = line + ind;
    i32 tl = len - ind;

    i32 run = 0;
    if (tl >= 3 && (t[0] == '`' || t[0] == '~'))
        while (run < tl && t[run] == t[0]) run++;

    if (fs->mark) {
        /* Inside: only the same mark, as long, and bare closes it. */
        if (run >= 3 && t[0] == fs->mark && run >= fs->run && ind <= fs->indent + 3) {
            i32 rest = run;
            while (rest < tl && (t[rest] == ' ' || t[rest] == '\t')) rest++;
            if (rest == tl) {
                fs->mark = 0;
                r.fence = true;
                return r;
            }
        }
        r.code = true;
        return r;
    }

    if (run >= 3 && !(t[0] == '`' && memchr(t + run, '`', (usize)(tl - run)))) {
        fs->mark = t[0];
        fs->run = run;
        fs->indent = ind;
        r.fence = true;
        return r;
    }

    if (tl > 0 && t[0] == '#') {
        i32 k = 0;
        while (k < tl && k < 6 && t[k] == '#') k++;
        if (k > 0 && k < tl && t[k] == ' ') r.heading = k;
    }
    return r;
}

/* The start of line `line` (0-based) of `doc`, found by LENGTH — a NUL is a
 * byte of the note, not its end — with `fs` as it stands at that line. When the
 * document has fewer lines the end is returned and *reached says how many it
 * had. */
static inline const char *md_live_walk_to(const char *doc, usize size, i32 line,
                                          MdLiveFence *fs, i32 *reached) {
    const char *p = doc;
    const char *end = doc + size;
    i32 li = 0;
    *fs = (MdLiveFence){ 0, 0, 0 };
    for (; li < line && p < end; li++) {
        const char *eol = memchr(p, '\n', (usize)(end - p));
        i32 ll = eol ? (i32)(eol - p) : (i32)(end - p);
        i32 text = (eol && ll > 0 && p[ll - 1] == '\r') ? ll - 1 : ll;
        (void)md_live_classify(p, text, fs);
        p = eol ? eol + 1 : end;
    }
    if (reached) *reached = li;
    return p;
}

/* A soft row retains every source byte, including whitespace at its end.
 * Break at the last blank that fits; a long word is split on a character. */
static inline i32 md_live_wrap_end(const char *s, i32 len, i32 start, i32 cols) {
    if (cols < 1) cols = 1;
    i32 pos = start, used = 0, blank = -1;
    while (pos < len) {
        EditorCell c = editor_cell_at(s, len, pos, used, 1);
        if (used + c.cols > cols && pos > start) break;
        pos += c.bytes; used += c.cols;
        if (s[pos - 1] == ' ' || s[pos - 1] == '\t') blank = pos;
    }
    return pos < len && blank > start ? blank : pos;
}

static inline i32 md_live_wrap_cols(MdLiveLine cls, f32 width, f32 cw) {
    if (cls.code || cls.fence || width <= 0.0f) return INT32_MAX;
    i32 n = (i32)(width / md_live_cell_w(cls.heading, cw));
    return n > 1 ? n - 1 : 1; /* room for the end-of-line caret */
}

static inline i32 md_live_rows(const char *s, i32 len, i32 cols) {
    i32 rows = 0, at = 0;
    do { at = md_live_wrap_end(s, len, at, cols); rows++; } while (at < len);
    return rows;
}

#endif /* UI_MD_LIVE_LINES_H */
