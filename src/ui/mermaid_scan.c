/* Liu — Mermaid blocks in an agent's output. See mermaid_scan.h. */
#include "ui/mermaid_scan.h"
#include "ui/markdown/md_diagram.h"
#include "core/utf8.h"

#include <stdlib.h>
#include <string.h>

#define MMD_LINE_CAP 4096u          /* one logical source line */
#define MMD_ROW_TEXT_CAP 512u       /* a row's text, for its kind */

u64 mmd_hash_bytes(const char *s, u32 n) {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 1099511628211ull; }
    return h;
}

static bool mmd_blank_cp(u32 c) { return c == 0 || c == ' ' || c == '\t' || c == 0xA0; }

/* Box drawing and block elements: the border of a framed code block. */
static bool mmd_frame_cp(u32 c) { return c >= 0x2500 && c <= 0x259F; }

/* UTF-8 of cp[a..b], blanks as spaces; NUL-terminated, cut at `cap`. */
static u32 mmd_utf8(const u32 *cp, i32 a, i32 b, char *out, u32 cap) {
    u32 o = 0;
    for (i32 i = a; i <= b && o + 5 < cap; i++) {
        u32 c = cp[i];
        if (c == 0) {
            /* An empty cell is a space; the second half of a wide character
             * is nothing — the caller cannot tell them apart, so a zero right
             * after a wide character is skipped. */
            if (i > a && cp[i - 1] && utf8_char_width(cp[i - 1]) >= 2) continue;
            out[o++] = ' ';
            continue;
        }
        if (c < 0x20 || c == 0x7F) { out[o++] = ' '; continue; }
        u8 buf[4];
        u32 n = utf8_encode(c, buf);
        if (!n) { out[o++] = '?'; continue; }
        memcpy(out + o, buf, n);
        o += n;
    }
    out[o] = 0;
    return o;
}

u32 mmd_row_text(const u32 *cp, i32 a, i32 b, char *out, u32 cap) {
    if (!out || !cap) return 0;
    if (!cp || a > b) { out[0] = 0; return 0; }
    return mmd_utf8(cp, a, b, out, cap);
}

void mmd_row_cells(const char *text, u32 len, i32 col, u32 *cp, i32 cols) {
    if (!cp || cols <= 0) return;
    memset(cp, 0, sizeof(u32) * (usize)cols);
    i32 x = col < 0 ? 0 : col;
    for (u32 i = 0; text && i < len && x < cols; ) {
        u32 c;
        u32 n = utf8_decode((const u8 *)text + i, len - i, &c);
        if (!n) { n = 1; c = '?'; }
        i += n;
        cp[x] = c;
        /* mmd_utf8 left out the second half of a wide character */
        x += utf8_char_width(c) >= 2 ? 2 : 1;
    }
}

static bool mmd_eq_ci(const char *a, u32 an, const char *b);

/* A code block's language naming something drawn: its MmdType, or -1. */
static i32 mmd_label_type(const char *t, u32 n) {
    static const char *diagram[] = { "mermaid", "dot", "graphviz", "gv", "plantuml", "puml", NULL };
    static const char *math[]    = { "math", "latex", "tex", "katex", NULL };
    static const char *svg[]     = { "svg", "xml", NULL };
    for (u32 k = 0; diagram[k]; k++) if (mmd_eq_ci(t, n, diagram[k])) return MMD_TYPE_DIAGRAM;
    for (u32 k = 0; math[k]; k++)    if (mmd_eq_ci(t, n, math[k]))    return MMD_TYPE_MATH;
    for (u32 k = 0; svg[k]; k++)     if (mmd_eq_ci(t, n, svg[k]))     return MMD_TYPE_SVG;
    return -1;
}

/* A row that opens display math (`$$`, `\[`) or an SVG document. */
static i32 mmd_open_type(const char *t, u32 n) {
    if (n >= 2 && t[0] == '$' && t[1] == '$') return MMD_TYPE_MATH;
    if (n >= 2 && t[0] == '\\' && t[1] == '[') return MMD_TYPE_MATH;
    if (n >= 4 && !memcmp(t, "<svg", 4) && (n == 4 || t[4] == ' ' || t[4] == '>')) return MMD_TYPE_SVG;
    if (n >= 5 && !memcmp(t, "<?xml", 5)) return MMD_TYPE_SVG;
    return -1;
}

static bool mmd_eq_ci(const char *a, u32 an, const char *b) {
    u32 bn = (u32)strlen(b);
    if (an != bn) return false;
    for (u32 i = 0; i < an; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (x != y) return false;
    }
    return true;
}

void mmd_row_summarize(const u32 *cp, i32 cols, MmdRow *out) {
    memset(out, 0, sizeof *out);
    out->first = out->last = out->t0 = out->t1 = -1;
    if (!cp || cols <= 0) return;
    i32 f = 0;
    while (f < cols && mmd_blank_cp(cp[f])) f++;
    if (f >= cols) return;
    i32 l = cols - 1;
    while (l > f && mmd_blank_cp(cp[l])) l--;
    i32 a = f, b = l;
    while (a <= b && (mmd_frame_cp(cp[a]) || mmd_blank_cp(cp[a]))) a++;
    while (b >= a && (mmd_frame_cp(cp[b]) || mmd_blank_cp(cp[b]))) b--;
    if (a > b) { out->rule = 1; return; }   /* a rule of box drawing: blank */
    out->first = (i16)f; out->last = (i16)l;
    out->t0 = (i16)a; out->t1 = (i16)b;
    out->kind = MMD_ROW_TEXT;

    u64 h = 1469598103934665603ull ^ (u64)(u32)a;
    for (i32 i = a; i <= b; i++) { h ^= cp[i]; h *= 1099511628211ull; }
    out->hash = h ? h : 1;

    u32 w = 0;
    for (i32 i = a; i <= b && !mmd_blank_cp(cp[i]) && w < 255; i++) w++;
    /* the second half of a wide character reads as 0, which is not text */
    out->first_word = (u8)w;

    if (b - a > 160) return;
    char t[MMD_ROW_TEXT_CAP];
    u32 n = mmd_utf8(cp, a, b, t, sizeof t);
    if (n >= 3 && (t[0] == '`' || t[0] == '~')) {
        u32 k = 0;
        while (k < n && t[k] == t[0]) k++;
        if (k >= 3) {
            u32 q = k;
            while (q < n && t[q] == ' ') q++;
            if (q == n) { out->kind = MMD_ROW_FENCE; return; }
            i32 lt = mmd_label_type(t + q, n - q);
            if (lt >= 0) { out->kind = MMD_ROW_LABEL; out->tag = (u8)lt | MMD_TAG_FENCED; }
            return;
        }
    }
    i32 lt = mmd_label_type(t, n);
    if (lt >= 0) { out->kind = MMD_ROW_LABEL; out->tag = (u8)lt; return; }
    i32 ot = mmd_open_type(t, n);
    if (ot >= 0) { out->kind = MMD_ROW_HEADER; out->tag = (u8)ot; return; }
    if (md_diagram_header_line((const u8 *)t, n)) out->kind = MMD_ROW_HEADER;
}

/* ---- source lines ---- */

typedef enum {
    K_FLOW, K_SEQ, K_STATE, K_CLASS, K_ER, K_PIE,
    K_GANTT, K_MINDMAP, K_TIMELINE, K_GIT, K_XY, K_QUADRANT, K_JOURNEY,
    /* the languages that close themselves: their statements are not judged */
    K_DOT, K_PUML, K_MATH, K_MATHL, K_SVG
} MmdKind;

static MmdKind mmd_kind_of(const char *h) {
    u32 hn = (u32)strlen(h);
    if (!strncmp(h, "@startuml", 9))        return K_PUML;
    i32 ot = mmd_open_type(h, hn);
    if (ot == MMD_TYPE_MATH)                return K_MATH;
    if (ot == MMD_TYPE_SVG)                 return K_SVG;
    if (md_diagram_lang_detect((const u8 *)h, hn) == MD_DIAGRAM_LANG_GRAPHVIZ) return K_DOT;
    if (!strncmp(h, "sequenceDiagram", 15)) return K_SEQ;
    if (!strncmp(h, "stateDiagram", 12))    return K_STATE;
    if (!strncmp(h, "classDiagram", 12))    return K_CLASS;
    if (!strncmp(h, "erDiagram", 9))        return K_ER;
    if (!strncmp(h, "pie", 3))              return K_PIE;
    if (!strncmp(h, "gantt", 5))            return K_GANTT;
    if (!strncmp(h, "mindmap", 7))          return K_MINDMAP;
    if (!strncmp(h, "timeline", 8))         return K_TIMELINE;
    if (!strncmp(h, "gitGraph", 8))         return K_GIT;
    if (!strncmp(h, "xychart", 7))          return K_XY;
    if (!strncmp(h, "quadrantChart", 13))   return K_QUADRANT;
    if (!strncmp(h, "journey", 7))          return K_JOURNEY;
    return K_FLOW;
}

/* Words in [s, s+n). */
static u32 mmd_words(const char *s, u32 n) {
    u32 w = 0;
    bool in = false;
    for (u32 i = 0; i < n; i++) {
        bool sp = s[i] == ' ';
        if (!sp && !in) w++;
        in = !sp;
    }
    return w;
}

/* A row of one of the kinds whose statements are `name : …` lines: the
 * grammar after the colon, not merely a colon, which prose has too. */
static bool mmd_colon_statement(MmdKind kind, const char *t) {
    const char *c = strchr(t, ':');
    if (!c) return false;
    const char *r = c + 1;
    while (*r == ' ') r++;
    switch (kind) {
    case K_GANTT:
        for (const char *p = r; *p; p++) if (*p >= '0' && *p <= '9') return true;
        return !strncmp(r, "done", 4) || !strncmp(r, "active", 6) || !strncmp(r, "crit", 4) ||
               !strncmp(r, "milestone", 9) || !strncmp(r, "after ", 6);
    case K_JOURNEY:
        return *r >= '1' && *r <= '5' && (r[1] == 0 || r[1] == ' ' || r[1] == ':');
    case K_QUADRANT:
        return *r == '[';
    case K_TIMELINE:
        return t[0] == ':' || mmd_words(t, (u32)(c - t)) <= 4;
    default:
        return false;
    }
}

static bool mmd_word_byte(u8 c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c >= 0x80;
}

/* The line's first word is `kw`, standing alone. */
static bool mmd_kw(const char *s, const char *kw) {
    u32 n = (u32)strlen(kw);
    if (strncmp(s, kw, n) != 0) return false;
    u8 c = (u8)s[n];
    return c == 0 || c == ' ' || c == '\t' || c == ':' || c == '{' || c == '[' || c == '(';
}

static bool mmd_has(const char *s, const char *needle) { return strstr(s, needle) != NULL; }

/* Is this row, at the block's own column, a source statement rather than the
 * prose after the block? */
static bool mmd_statement(MmdKind kind, const char *t, i32 braces) {
    if (!t[0]) return false;
    if (braces > 0) return true;                     /* inside a { … } body */
    if (t[0] == '%' && t[1] == '%') return true;
    if (mmd_has(t, "-->") || mmd_has(t, "---") || mmd_has(t, "==>") || mmd_has(t, "===") ||
        mmd_has(t, "-.-") || mmd_has(t, "->>") || mmd_has(t, "->") || mmd_has(t, "--x") ||
        mmd_has(t, "--)") || mmd_has(t, "~~~") || mmd_has(t, "..>") || mmd_has(t, "<|") ||
        mmd_has(t, "|>") || mmd_has(t, "--o") || mmd_has(t, "o--") || mmd_has(t, "*--") ||
        mmd_has(t, "||") || mmd_has(t, "|{") || mmd_has(t, "}|") || mmd_has(t, "}o") ||
        mmd_has(t, "o{") || mmd_has(t, "-x"))
        return true;
    if (t[0] == '}' || t[0] == '{') return true;
    if (!strcmp(t, "end")) return true;
    static const char *flow[] = { "subgraph", "direction", "style", "classDef", "class",
                                  "click", "linkStyle", "accTitle", "accDescr", NULL };
    static const char *seq[]  = { "participant", "actor", "loop", "alt", "else", "opt", "par",
                                  "and", "critical", "break", "rect", "box", "activate",
                                  "deactivate", "autonumber", "create", "destroy", "title",
                                  "links", "link", NULL };
    static const char *state[] = { "state", "direction", "classDef", "class", "[*]", NULL };
    static const char *cls[]  = { "class", "direction", "namespace", "style", "classDef",
                                  "cssClass", "callback", "click", "link", NULL };
    static const char *pie[]  = { "title", "showData", NULL };
    static const char *gantt[] = { "title", "dateFormat", "axisFormat", "section", "excludes", "includes",
                                   "todayMarker", "tickInterval", "weekday", NULL };
    static const char *git[] = { "commit", "branch", "checkout", "switch", "merge", "cherry-pick", NULL };
    static const char *xy[]  = { "title", "x-axis", "y-axis", "bar", "line", NULL };
    static const char *quad[] = { "title", "x-axis", "y-axis", "quadrant-1", "quadrant-2", "quadrant-3",
                                  "quadrant-4", NULL };
    static const char *titled[] = { "title", "section", NULL };
    const char **list = kind == K_SEQ ? seq : kind == K_STATE ? state : kind == K_CLASS ? cls
                      : kind == K_PIE ? pie : kind == K_FLOW ? flow : kind == K_GANTT ? gantt
                      : kind == K_GIT ? git : kind == K_XY ? xy : kind == K_QUADRANT ? quad
                      : (kind == K_TIMELINE || kind == K_JOURNEY) ? titled : NULL;
    for (u32 k = 0; list && list[k]; k++) if (mmd_kw(t, list[k])) return true;
    /* `Note left of A: …` but not the prose "Note: …" */
    if ((kind == K_SEQ || kind == K_STATE || kind == K_CLASS) &&
        (!strncmp(t, "Note ", 5) || !strncmp(t, "note ", 5))) {
        const char *p = t + 5;
        if (!strncmp(p, "left", 4) || !strncmp(p, "right", 5) || !strncmp(p, "over", 4) ||
            !strncmp(p, "for", 3))
            return true;
    }
    if (kind == K_CLASS && t[0] == '<' && t[1] == '<') return true;
    if (kind == K_PIE && t[0] == '"' && mmd_has(t, ":")) return true;
    if (mmd_colon_statement(kind, t)) return true;
    /* An id with a shape right after it: `A[Start]`, `B{Ok?}`, `C((x))`. */
    u32 i = 0;
    while (t[i] && mmd_word_byte((u8)t[i])) i++;
    if (i > 0 && (t[i] == '[' || t[i] == '(' || t[i] == '{' || t[i] == '>' || t[i] == '@' ||
                  (t[i] == ':' && t[i+1] == ':' && t[i+2] == ':')))
        return true;
    /* `Animal : +int age`, `Idle : waiting` */
    if (i > 0 && (kind == K_CLASS || kind == K_STATE)) {
        u32 j = i;
        while (t[j] == ' ') j++;
        if (t[j] == ':') return true;
    }
    /* `CUSTOMER {` */
    if (i > 0 && kind == K_ER) {
        u32 j = i;
        while (t[j] == ' ') j++;
        if (t[j] == '{' || t[j] == 0) return true;
    }
    return false;
}

/* The line so far is not finished: an open bracket or quote, or a link
 * operator with nothing after it. */
static bool mmd_open_line(const char *s, u32 n) {
    i32 depth = 0;
    bool q = false;
    for (u32 i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') { q = !q; continue; }
        if (q) continue;
        if (c == '[' || c == '(' || c == '{') depth++;
        else if ((c == ']' || c == ')' || c == '}') && depth > 0) depth--;
    }
    if (q || depth > 0) return true;
    while (n && s[n - 1] == ' ') n--;
    if (!n) return false;
    char e = s[n - 1];
    if (e == '&' || e == '|') return true;
    if (n >= 2 && ((s[n-2] == '-' && e == '-') || (s[n-2] == '=' && e == '=') ||
                   (s[n-2] == '-' && e == '.') || (s[n-2] == '-' && e == '>') ||
                   (s[n-2] == '=' && e == '>')))
        return true;
    return false;
}

/* Structure a whole block must close: subgraphs and sequence frames by `end`,
 * bodies by `}`. */
typedef struct { i32 subs, frames, braces; } MmdOpen;

static void mmd_note_line(MmdKind kind, const char *t, MmdOpen *o) {
    if (kind >= K_DOT) return;
    if (kind == K_FLOW) {
        if (mmd_kw(t, "subgraph")) o->subs++;
        else if (!strcmp(t, "end") && o->subs > 0) o->subs--;
        return;
    }
    if (kind == K_SEQ) {
        static const char *fr[] = { "loop", "alt", "opt", "par", "critical", "break",
                                    "rect", "box", NULL };
        for (u32 k = 0; fr[k]; k++) if (mmd_kw(t, fr[k])) { o->frames++; return; }
        if (!strcmp(t, "end") && o->frames > 0) o->frames--;
        return;
    }
    bool q = false;
    for (const char *p = t; *p; p++) {
        if (*p == '"') { q = !q; continue; }
        if (q) continue;
        if (*p == '{') o->braces++;
        else if (*p == '}' && o->braces > 0) o->braces--;
    }
}

typedef struct { char *buf; u32 cap, len; bool full; } MmdOut;

static void mmd_put(MmdOut *o, const char *s, u32 n) {
    if (o->full) return;
    if (o->len + n + 1 > o->cap) { o->full = true; return; }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static u32 *g_cp;
static i32  g_cp_cap;

static u32 *mmd_row_cps(const MmdScreen *scr, i32 y) {
    if (scr->cols > g_cp_cap) {
        u32 *n = (u32 *)realloc(g_cp, sizeof(u32) * (usize)scr->cols);
        if (!n) return NULL;
        g_cp = n;
        g_cp_cap = scr->cols;
    }
    if (!scr->read || !scr->read(scr->ctx, y, g_cp, scr->cols)) return NULL;
    return g_cp;
}

static const char *mmd_trim(char *s) {
    while (*s == ' ') s++;
    u32 n = (u32)strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    return s;
}

static bool mmd_alnum_byte(u8 c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c >= 0x80;
}

/* Cells of the last word of `s`. */
static i32 mmd_tail_word_cells(const char *s, u32 n) {
    u32 i = n;
    while (i > 0 && s[i - 1] != ' ') i--;
    i32 c = 0;
    for (u32 k = i; k < n; ) {
        u32 cp;
        u32 adv = utf8_decode((const u8 *)s + k, n - k, &cp);
        if (!adv) { adv = 1; cp = '?'; }
        k += adv;
        c += utf8_char_width(cp) >= 2 ? 2 : 1;
    }
    return c;
}

/* An XML tag left open on the line: a `<` with no `>` after it, outside
 * quotes. */
static bool mmd_tag_open(const char *s, u32 n) {
    bool in = false;
    char q = 0;
    for (u32 i = 0; i < n; i++) {
        char c = s[i];
        if (q) { if (c == q) q = 0; continue; }
        if (in && (c == '"' || c == '\'')) { q = c; continue; }
        if (c == '<') in = true;
        else if (c == '>') in = false;
    }
    return in;
}

/* `{` less `}` outside quotes: how far a DOT row opens the graph. */
static i32 mmd_brace_delta(const char *t, bool *opened) {
    i32 d = 0;
    bool q = false;
    for (const char *p = t; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '"') { q = !q; continue; }
        if (q) continue;
        if (*p == '{') { d++; *opened = true; }
        else if (*p == '}') d--;
    }
    return d;
}

/* `<svg` elements opened less those closed on a row. */
static i32 mmd_svg_delta(const char *t, bool *opened) {
    i32 d = 0;
    for (const char *p = t; (p = strchr(p, '<')) != NULL; p++) {
        if (!strncmp(p, "<svg", 4) && (p[4] == ' ' || p[4] == '>' || p[4] == 0)) {
            d++;
            *opened = true;
            const char *gt = strchr(p, '>');
            if (gt && gt > p && gt[-1] == '/') d--;          /* <svg … /> */
        } else if (!strncmp(p, "</svg", 5)) d--;
    }
    return d;
}

/* Does the row end display math opened with `open` ('$' for `$$`, '[' for
 * `\[`)? `from` skips the opener on the row that opened it. */
static bool mmd_math_closes(const char *t, char open, u32 from) {
    u32 n = (u32)strlen(t);
    while (n && t[n - 1] == ' ') n--;
    if (n < from + 2) return false;
    if (open == '$') return t[n - 2] == '$' && t[n - 1] == '$';
    return t[n - 2] == '\\' && t[n - 1] == ']';
}

/* A math block's source as the cards read it: the TeX alone between two `$$`
 * lines, its own delimiters taken off. */
static void mmd_math_source(MmdOut *o, MmdKind kind) {
    if (o->full) return;
    char *s = o->buf;
    u32 a = 0, e = o->len;
    while (a < e && (s[a] == ' ' || s[a] == '\n')) a++;
    while (e > a && (s[e - 1] == ' ' || s[e - 1] == '\n')) e--;
    if (kind == K_MATH && e - a >= 2) {
        if ((s[a] == '$' && s[a + 1] == '$') || (s[a] == '\\' && s[a + 1] == '[')) a += 2;
        if (e - a >= 2 && ((s[e - 2] == '$' && s[e - 1] == '$') || (s[e - 2] == '\\' && s[e - 1] == ']'))) e -= 2;
    }
    while (a < e && (s[a] == ' ' || s[a] == '\n')) a++;
    while (e > a && (s[e - 1] == ' ' || s[e - 1] == '\n')) e--;
    u32 body = e - a;
    if (body + 8 + 1 > o->cap) { o->full = true; return; }
    memmove(s + 3, s + a, body);
    memcpy(s, "$$\n", 3);
    memcpy(s + 3 + body, "\n$$\n", 4);
    o->len = body + 7;
}

/* Walk the block whose header is on row `head` (its label on `row0`), with
 * `width` the column the agent's wrap stops at. `force` is the kind a label
 * gave it (-1: from its header); `fenced`, that the label opened a fence. */
static bool mmd_walk(const MmdScreen *scr, i32 row0, i32 head, i32 width, i32 force, bool fenced,
                     MmdBlock *b, char *dst, u32 cap) {
    const MmdRow *rows = scr->rows;
    const MmdRow *hr = &rows[head];
    i32 col0 = hr->t0;
    /* math under a label: its rows may start left of its first */
    if (force == K_MATHL && rows[row0].t0 >= 0 && rows[row0].t0 < col0) col0 = rows[row0].t0;
    bool framed = hr->first < hr->t0;

    MmdOut out = { dst, cap, 0, false };
    char text[MMD_LINE_CAP];
    u32 *cp = mmd_row_cps(scr, head);
    if (!cp) return false;
    u32 hn = mmd_utf8(cp, col0, hr->t1, text, sizeof text);
    static char line[MMD_LINE_CAP];
    u32 ll = 0;
    char hcopy[MMD_LINE_CAP];
    memcpy(hcopy, text, hn + 1);
    MmdKind kind = force >= 0 ? (MmdKind)force : mmd_kind_of(mmd_trim(hcopy));
    MmdOpen open = { 0, 0, 0 };
    /* The languages that close themselves: the block is whole when it does.
     * Their first row is a line like the others, which the agent may have
     * wrapped (an SVG's `<svg …>`); a diagram's header is a row of its own. */
    bool closes = kind >= K_DOT, closed = false, opened = false;
    if (closes) {
        memcpy(line, text, hn + 1);
        ll = hn;
        while (ll && line[ll - 1] == ' ') line[--ll] = 0;
    } else {
        mmd_put(&out, text, hn);
        mmd_put(&out, "\n", 1);
    }
    i32 depth = 0;
    char math_open = 0;
    {
        const char *h = mmd_trim(text);
        if (kind == K_DOT) { depth = mmd_brace_delta(h, &opened); closed = opened && depth <= 0; }
        else if (kind == K_SVG) { depth = mmd_svg_delta(h, &opened); closed = opened && depth <= 0; }
        else if (kind == K_MATH) {
            math_open = h[0] == '$' ? '$' : '[';
            closed = mmd_math_closes(h, math_open, 2);
        }
    }

    i32 last_nb = head, blank = 0, prev_last = hr->t1, col1 = hr->t1, row1 = -1;
    /* The agent wraps a row only at its box's edge, near the pane's: a block
     * none of whose rows comes near it was not wrapped, and only a line left
     * open (a bracket, a quote, a trailing link) goes on onto the next row. */
    bool may_wrap = width + 8 >= scr->cols;
    u8 end = closed ? MMD_END_CLOSED : MMD_END_SCREEN;
    if (closed) row1 = head;
    i32 y;
    for (y = head + 1; y < scr->nrows && !closed; y++) {
        const MmdRow *r = &rows[y];
        if (r->kind == MMD_ROW_BLANK) {
            /* math under a label ends at its first blank row: display math has none */
            if (kind == K_MATHL && !fenced) { closed = true; end = MMD_END_CLOSED; break; }
            blank++;
            continue;
        }
        i32 rf = framed ? r->t0 : r->first;
        i32 rl = framed ? r->t1 : r->last;
        if (r->kind == MMD_ROW_FENCE && r->t0 >= col0 - 3) {
            end = MMD_END_FENCE;
            row1 = y;
            if (kind == K_MATHL) closed = true;
            break;
        }
        if (rf < col0) { end = MMD_END_CHROME; break; }
        cp = mmd_row_cps(scr, y);
        if (!cp) break;
        u32 tn = mmd_utf8(cp, col0, rl, text, sizeof text);
        (void)tn;
        const char *trimmed = mmd_trim(text);
        bool cont = ll > 0 && blank == 0 && rf == col0 &&
                    (mmd_open_line(line, ll) || (kind == K_SVG && mmd_tag_open(line, ll)) ||
                     (may_wrap && prev_last + 2 + (i32)r->first_word > width));
        if (cont) {
            u32 tl = (u32)strlen(trimmed);
            /* The wrap drops the space it breaks at, so one goes back — except
             * where a word longer than the box was cut: the row before is
             * full to the edge and the two halves are more than a row. */
            /* A word that fills the row exactly reads the same from the
             * cells, so a cut is taken only for a word longer than the pane
             * itself is wide (the box is nearly all of it), and only between
             * two letters or digits, where a wrap would not have broken. */
            i32 joined = mmd_tail_word_cells(line, ll) + (i32)r->first_word;
            i32 box = width - col0, pane = scr->cols - col0 - 4;
            bool cut = prev_last + 1 >= width && ll > 0 &&
                       joined > (box > pane ? box : pane) &&
                       mmd_alnum_byte((u8)line[ll - 1]) && mmd_alnum_byte((u8)trimmed[0]);
            if (ll + 1 + tl < sizeof line) {
                if (!cut) line[ll++] = ' ';
                memcpy(line + ll, trimmed, tl);
                ll += tl;
                line[ll] = 0;
            }
        } else {
            if (!closes && rf == col0 && !mmd_statement(kind, trimmed, open.braces)) {
                end = MMD_END_PROSE;
                break;
            }
            if (ll) {
                line[ll] = 0;
                mmd_note_line(kind, mmd_trim(line), &open);
                mmd_put(&out, line, ll);
                mmd_put(&out, "\n", 1);
            }
            /* keep the row's own indentation past the block's column */
            ll = 0;
            for (i32 k = col0; k < rf && ll + 1 < sizeof line; k++) line[ll++] = ' ';
            u32 tl = (u32)strlen(trimmed);
            if (ll + tl >= sizeof line) tl = (u32)sizeof line - ll - 1;
            memcpy(line + ll, trimmed, tl);
            ll += tl;
            line[ll] = 0;
        }
        blank = 0;
        last_nb = y;
        prev_last = rl;
        if (rl > col1) col1 = rl;
        if (closes) {
            if (kind == K_PUML) closed = !strncmp(trimmed, "@enduml", 7);
            else if (kind == K_DOT) { depth += mmd_brace_delta(trimmed, &opened); closed = opened && depth <= 0; }
            else if (kind == K_SVG) { depth += mmd_svg_delta(trimmed, &opened); closed = opened && depth <= 0; }
            else if (kind == K_MATH) closed = mmd_math_closes(trimmed, math_open, 0);
            if (closed) { end = MMD_END_CLOSED; row1 = y; }
        }
    }
    if (ll) {
        line[ll] = 0;
        char tmp[MMD_LINE_CAP];
        memcpy(tmp, line, ll + 1);
        mmd_note_line(kind, mmd_trim(tmp), &open);
        mmd_put(&out, line, ll);
        mmd_put(&out, "\n", 1);
    }
    if (kind == K_MATH || kind == K_MATHL) mmd_math_source(&out, kind);
    if (out.full) return false;
    if (row1 < 0) row1 = last_nb;
    /* Every row read, and the agent's own rows come next. */
    if (end == MMD_END_SCREEN && y >= scr->nrows && scr->tail_chrome) end = MMD_END_CHROME;

    bool complete;
    switch (end) {
    case MMD_END_FENCE: case MMD_END_PROSE: complete = true; break;
    /* The agent's own rows below it: whole when a blank row stands between
     * (a viewport cutting a block shows none) and the agent has finished. */
    case MMD_END_CHROME: complete = blank > 0 && !scr->busy; break;
    default: complete = false; break;
    }
    if (open.subs > 0 || open.frames > 0 || open.braces > 0) complete = false;
    if (closes) complete = closed;

    memset(b, 0, sizeof *b);
    b->row0 = row0;
    b->head = head;
    b->row1 = row1;
    b->col0 = rows[row0].t0 < col0 ? rows[row0].t0 : col0;
    b->col1 = col1;
    b->complete = complete;
    b->end = end;
    b->type = kind == K_MATH || kind == K_MATHL ? MMD_TYPE_MATH : kind == K_SVG ? MMD_TYPE_SVG : MMD_TYPE_DIAGRAM;
    b->src_len = out.len;
    b->src_hash = mmd_hash_bytes(dst, out.len);
    return out.len > 0;
}

u32 mmd_find(const MmdScreen *scr, MmdBlock *out, u32 cap, char *src, u32 src_cap) {
    if (!scr || !scr->rows || scr->nrows <= 0 || scr->cols <= 0 || !out || !src) return 0;
    u32 nb = 0, used = 0;
    for (i32 y = 0; y < scr->nrows && nb < cap; y++) {
        const MmdRow *r = &scr->rows[y];
        i32 head = -1, force = -1;
        bool fenced = false;
        if (r->kind == MMD_ROW_LABEL) {
            i32 h = y + 1;
            if (h < scr->nrows && scr->rows[h].kind == MMD_ROW_BLANK) h++;
            if ((r->tag & MMD_TAG_TYPE) == MMD_TYPE_MATH) {
                /* TeX has no header: the label's next row starts it */
                if (h < scr->nrows && (scr->rows[h].kind == MMD_ROW_TEXT || scr->rows[h].kind == MMD_ROW_HEADER) &&
                    scr->rows[h].t0 >= r->t0 - 1) {
                    head = h;
                    force = K_MATHL;
                    fenced = (r->tag & MMD_TAG_FENCED) != 0;
                }
            } else if (h < scr->nrows && scr->rows[h].kind == MMD_ROW_HEADER &&
                       scr->rows[h].t0 >= r->t0 - 1)
                head = h;
        } else if (r->kind == MMD_ROW_HEADER) {
            head = y;
        }
        if (head < 0) continue;
        if (used >= src_cap) break;
        MmdBlock b;
        u32 room = src_cap - used;
        if (room > MMD_MAX_SRC) room = MMD_MAX_SRC;
        /* The agent's wrap width as the block's own rows show it: first
         * from every row until something left of the block, to find where
         * it ends, then from the block's rows alone — prose after it may be
         * laid out by other rules. A row that wrapped reached it. */
        const MmdRow *hr = &scr->rows[head];
        bool framed = hr->first < hr->t0;
        i32 width = hr->t1 + 1;
        for (i32 k = head + 1; k < scr->nrows; k++) {
            const MmdRow *rk = &scr->rows[k];
            if (rk->kind == MMD_ROW_BLANK) continue;
            i32 rf = framed ? rk->t0 : rk->first;
            if (rf < hr->t0 || rk->kind == MMD_ROW_FENCE) break;
            i32 rl = framed ? rk->t1 : rk->last;
            if (rl + 1 > width) width = rl + 1;
        }
        bool walked = mmd_walk(scr, y, head, width, force, fenced, &b, src + used, room);
        if (walked) {
            i32 own = hr->t1 + 1;
            for (i32 k = head + 1; k <= b.row1 && k < scr->nrows; k++) {
                const MmdRow *rk = &scr->rows[k];
                if (rk->kind == MMD_ROW_BLANK || rk->kind == MMD_ROW_FENCE) continue;
                i32 rl = framed ? rk->t1 : rk->last;
                if (rl + 1 > own) own = rl + 1;
            }
            if (own != width) walked = mmd_walk(scr, y, head, own, force, fenced, &b, src + used, room);
        }
        if (walked && scr->want_n > 0 &&
            (b.row1 < scr->want_lo || b.row0 >= scr->want_lo + scr->want_n)) {
            y = b.row1;                      /* not on screen: walked past, not kept */
        } else if (walked) {
            b.src_off = used;
            used += b.src_len;
            out[nb++] = b;
            y = b.row1;
        } else {
            y = head;
        }
    }
    return nb;
}

bool mmd_locate_known(const MmdRow *rows, i32 nrows, const u64 *known, i32 nknown,
                      i32 *row0, i32 *vis0, i32 *vis1) {
    if (!rows || nrows <= 0 || !known || nknown <= 0) return false;
    i32 best_len = 0, best_r0 = 0, best_v0 = 0, best_v1 = -1;
    for (i32 y = 0; y < nrows; y++) {
        u64 h = rows[y].hash;
        if (!h) continue;
        /* Inside a run already found: it cannot start a longer one. */
        if (best_v1 >= 0 && y >= best_v0 && y <= best_v1) continue;
        for (i32 j = 0; j < nknown; j++) {
            if (known[j] != h) continue;
            /* The run must start at its own first row, the screen's first
             * row, or under a row that is not the block's. */
            i32 v0 = y, k = j;
            while (v0 > 0 && k > 0 && rows[v0 - 1].hash == known[k - 1]) { v0--; k--; }
            i32 v1 = y, m = j;
            while (v1 + 1 < nrows && m + 1 < nknown && rows[v1 + 1].hash == known[m + 1]) { v1++; m++; }
            i32 len = v1 - v0 + 1, nb = 0;
            for (i32 t = v0; t <= v1; t++) if (rows[t].hash) nb++;
            bool partial = k > 0 || m < nknown - 1;
            /* Two rows that happen to read alike are no evidence: at least
             * three of the block's non-blank rows, or all of a short one. */
            i32 need = nknown < 3 ? nknown : 3;
            if (!partial || nb < need) continue;
            if (len > best_len) {
                best_len = len;
                best_r0 = v0 - k;
                best_v0 = v0;
                best_v1 = v1;
            }
        }
    }
    if (best_v1 < 0) return false;
    *row0 = best_r0;
    *vis0 = best_v0;
    *vis1 = best_v1;
    return true;
}

/* ---- how far a picture moved ---- */

bool mmd_row_shift(const MmdRow *old, i32 old_n, const MmdRow *cur, i32 cur_n, i32 need,
                   i32 *shift, i32 *run0, i32 *run1) {
    if (!old || !cur || old_n <= 0 || cur_n <= 0 || !shift || !run0 || !run1) return false;
    if (need < 1) need = 1;
    i32 best_nb = 0, best_len = 0, best_s = 0, b0 = 0, b1 = -1;
    for (i32 s = -(cur_n - 1); s < old_n; s++) {
        i32 y0 = s < 0 ? -s : 0;
        i32 y1 = old_n - s < cur_n ? old_n - s : cur_n;
        if (y1 - y0 < need) continue;
        i32 as = s < 0 ? -s : s;
        i32 rs = -1, nb = 0;
        for (i32 y = y0; y < y1; y++) {
            if (cur[y].hash != old[y + s].hash) { rs = -1; continue; }
            if (rs < 0) { rs = y; nb = 0; }
            if (cur[y].hash) nb++;
            if (!nb) continue;                 /* blank rows alone prove nothing */
            i32 len = y - rs + 1;
            i32 ab = best_s < 0 ? -best_s : best_s;
            bool better = nb > best_nb ||
                          (nb == best_nb && (s == best_s ? len > best_len : as < ab));
            if (better) { best_nb = nb; best_len = len; best_s = s; b0 = rs; b1 = y; }
        }
    }
    if (best_nb < need) return false;
    *shift = best_s;
    *run0 = b0;
    *run1 = b1;
    return true;
}

/* ---- the tape ---- */

#define MMD_TAPE_MASK ((u64)MMD_TAPE_ROWS - 1u)
#define MMD_TAPE_NEED 3              /* non-blank rows two pictures must share */
#define MMD_INPUT_RULE_ROWS 8        /* how far over the cursor the input's rule may be */

static MmdTapeRow *tape_slot(const MmdTape *t, i64 idx) {
    return &t->slot[(u64)idx & MMD_TAPE_MASK];
}

static bool tape_has(const MmdTape *t, i64 idx) {
    const MmdTapeRow *s = tape_slot(t, idx);
    return s->gen == t->gen && s->idx == idx;
}

void mmd_tape_free(MmdTape *t) {
    if (!t) return;
    if (t->slot)
        for (u32 i = 0; i < MMD_TAPE_ROWS; i++) free(t->slot[i].text);
    free(t->slot);
    free(t->prev);
    free(t->seg);
    free(t->cp);
    memset(t, 0, sizeof *t);
}

usize mmd_tape_bytes(const MmdTape *t) {
    if (!t || !t->slot) return 0;
    usize b = sizeof(MmdTapeRow) * MMD_TAPE_ROWS +
              sizeof(MmdRow) * ((usize)t->rows + MMD_TAPE_ROWS) + sizeof(u32) * (usize)t->cols;
    for (u32 i = 0; i < MMD_TAPE_ROWS; i++) b += t->slot[i].cap;
    return b;
}

/* Keep screen row `y` as tape row `idx`. */
static bool tape_write(MmdTape *t, i64 idx, const MmdRow *sum, i32 y, MmdRowRead read, void *ctx) {
    MmdTapeRow *s = tape_slot(t, idx);
    u32 len = 0;
    if (sum->first >= 0) {
        if (!read(ctx, y, t->cp, t->cols)) return false;
        u32 need = (u32)(sum->last - sum->first + 1) * 4u + 8u;
        /* grown to fit, and given back when a much shorter row takes the slot */
        if (s->cap < need || s->cap > need * 2u + 256u) {
            char *n = (char *)realloc(s->text, need);
            if (!n) return false;
            s->text = n;
            s->cap = need;
        }
        len = mmd_row_text(t->cp, sum->first, sum->last, s->text, s->cap);
    }
    s->idx = idx;
    s->gen = t->gen;
    s->sum = *sum;
    s->len = len;
    return true;
}

static bool tape_read(void *ctx, i32 row, u32 *cp, i32 cols) {
    const MmdTape *t = (const MmdTape *)ctx;
    if (!t || row < 0 || row >= t->nseg) return false;
    i64 idx = t->seg0 + row;
    if (!tape_has(t, idx)) return false;
    const MmdTapeRow *s = tape_slot(t, idx);
    mmd_row_cells(s->text, s->len, s->sum.first, cp, cols);
    return true;
}

bool mmd_tape_take(MmdTape *t, const MmdRow *sum, i32 rows, i32 cols, i32 cursor,
                   MmdRowRead read, void *ctx) {
    if (!t || !sum || !read || rows <= 0 || cols <= 0) return false;
    /* Another size is another layout: the agent wraps everything again. */
    if (t->slot && (t->rows != rows || t->cols != cols)) mmd_tape_free(t);
    if (!t->slot) {
        t->slot = (MmdTapeRow *)calloc(MMD_TAPE_ROWS, sizeof *t->slot);
        t->prev = (MmdRow *)calloc((usize)rows, sizeof *t->prev);
        t->seg  = (MmdRow *)calloc(MMD_TAPE_ROWS, sizeof *t->seg);
        t->cp   = (u32 *)calloc((usize)cols, sizeof *t->cp);
        if (!t->slot || !t->prev || !t->seg || !t->cp) { mmd_tape_free(t); return false; }
        t->rows = rows;
        t->cols = cols;
    }

    i32 s = 0, r0 = 0, r1 = -1;
    bool moved = t->gen && mmd_row_shift(t->prev, rows, sum, rows, MMD_TAPE_NEED, &s, &r0, &r1);
    if (!moved) {
        if (t->gen) t->restarts++;
        if (++t->gen == 0) t->gen = 1;      /* every slot is stale */
        t->base = 0;
    } else if (s != 0) {
        t->base += s;
        /* The rows that stayed put on the side the output moved toward are
         * the agent's own: under the run when it moved down, over it when it
         * moved up. Only a run that is most of the picture says so. */
        if (r1 - r0 + 1 >= rows / 2) {
            if (s < 0) t->fixed_bot = rows - 1 - r1;
            else       t->fixed_top = r0;
        }
    }

    i32 out0 = t->fixed_top, out1 = rows - t->fixed_bot;
    if (cursor >= rows / 2 && cursor < rows) {
        /* The input box: the cursor's row, from the rule drawn over it when
         * there is one close above. */
        i32 in = cursor;
        for (i32 y = cursor - 1; y >= 0 && y >= cursor - MMD_INPUT_RULE_ROWS; y--)
            if (sum[y].rule) { in = y; break; }
        if (in < out1) out1 = in;
    }
    if (out1 - out0 < MMD_TAPE_NEED) { out0 = 0; out1 = rows; }
    t->out0 = out0;
    t->out1 = out1;

    i32 fnb = out1, lnb = out0 - 1;
    for (i32 y = out0; y < out1; y++)
        if (sum[y].hash) { if (fnb == out1) fnb = y; lnb = y; }
    for (i32 y = out0; y < out1; y++) {
        i64 idx = t->base + y;
        const MmdTapeRow *sl = tape_slot(t, idx);
        bool have = sl->gen == t->gen && sl->idx == idx;
        /* A blank row at the output's edge may be the input box's margin,
         * standing over a row the tape already holds. */
        if (have && !sum[y].hash && (y < fnb || y > lnb)) continue;
        if (have && sl->sum.hash == sum[y].hash && sl->sum.first == sum[y].first &&
            sl->sum.last == sum[y].last && sl->sum.kind == sum[y].kind)
            continue;
        if (!tape_write(t, idx, &sum[y], y, read, ctx)) {
            if (++t->gen == 0) t->gen = 1;
            return false;
        }
    }

    i64 end = t->base + out1;
    if (!moved || end > t->end) t->end = end;
    memcpy(t->prev, sum, sizeof(MmdRow) * (usize)rows);

    /* The known run of rows around the picture, as much below as above. */
    i64 lo = t->base + out0, hi = t->base + out1;
    bool up = true, down = true;
    while ((up || down) && hi - lo < MMD_TAPE_ROWS) {
        if (down) { if (hi < t->end && tape_has(t, hi)) hi++; else down = false; }
        if (up && hi - lo < MMD_TAPE_ROWS) { if (tape_has(t, lo - 1)) lo--; else up = false; }
    }
    t->seg0 = lo;
    t->nseg = (i32)(hi - lo);
    for (i64 i = lo; i < hi; i++) t->seg[i - lo] = tape_slot(t, i)->sum;
    return true;
}

void mmd_tape_screen(MmdTape *t, MmdScreen *scr, bool busy) {
    if (!scr) return;
    memset(scr, 0, sizeof *scr);
    if (!t || !t->slot) return;
    scr->rows = t->seg;
    scr->nrows = t->nseg;
    scr->cols = t->cols;
    scr->read = tape_read;
    scr->ctx = t;
    scr->busy = busy;
    scr->tail_chrome = t->seg0 + t->nseg == t->end;
    scr->want_lo = (i32)(t->base + t->out0 - t->seg0);
    scr->want_n = t->out1 - t->out0;
}
