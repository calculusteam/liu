/* Liu — minimal native Mermaid engine. See md_diagram.h. */
#include "ui/markdown/md_diagram.h"
#include "core/utf8.h"

#include <string.h>
#include <math.h>

/* Hard caps. Every one of them truncates instead of growing — and anything
 * that would truncate *misleadingly* (a dropped node silently rewires the
 * graph, a dropped participant silently reassigns a message) raises g_trunc,
 * which makes the whole fence probe as NONE. Showing the reader their own
 * source is always better than showing them a diagram that is not theirs. */
#define DG_MAX_SRC      (32u * 1024u)   /* bigger fences stay code */
#define DG_MAX_LINES    2048u
#define DG_MAX_NODES    128
#define DG_MAX_EDGES    256
#define DG_MAX_PARTS    24
#define DG_MAX_MSGS     128
#define DG_MAX_GROUPS   16
#define DG_MAX_DEPTH    8
#define DG_MAX_BARS     48
#define DG_MAX_LINKS    32              /* links per statement line */
#define DG_MAX_ROWS     12              /* <br/> rows kept per label */
#define DG_MAX_RANK     127
#define DG_MIN_TEXT     3.5f            /* px cell below which labels are noise */
#define DG_MIN_SCALE    0.02f           /* fit-to-column floor; below it, NONE */
/* Baseline pitch of a stacked label row, in cells. Every box that holds text
 * has to reserve `rows * DG_ROW_H * g_ch`, because that is exactly what
 * dg_text_box draws — measuring a multi-row label at one row per cell would
 * make the text overflow its own box and paint over the document. */
#define DG_ROW_H        1.15f
#define DG_ID_CAP       48
#define DG_LABEL_CAP    128
#define DG_BODY_CAP     192
#define DG_ELABEL_CAP   64

typedef enum {
    SH_RECT = 0, SH_ROUND, SH_STADIUM, SH_CIRCLE, SH_DIAMOND,
    SH_SUBROUTINE, SH_CYLINDER, SH_HEX, SH_PARA, SH_DOT, SH_RING, SH_CLASS
} NShape;

typedef enum { ES_SOLID = 0, ES_DOTTED, ES_THICK } EStyle;

/* End decorations. Flowcharts only ever use ARROW/CROSS/DIAMOND_O; the UML
 * shapes come from classDiagram relations. */
typedef enum {
    MK_NONE = 0, MK_ARROW, MK_CROSS, MK_TRIANGLE, MK_DIAMOND_F, MK_DIAMOND_O
} EMark;

typedef enum {
    SQ_MSG = 0, SQ_NOTE_L, SQ_NOTE_R, SQ_NOTE_OVER, SQ_ACT, SQ_DEACT
} SqKind;

typedef struct {
    char id[DG_ID_CAP];
    char label[DG_LABEL_CAP];   /* '\n' separates rendered rows */
    char body[DG_BODY_CAP];     /* classDiagram member compartment */
    u8   shape;
    i16  group;                 /* enclosing subgraph, -1 = none */
    i32  rank, order;
    f32  x, y, w, h;            /* px, top-left + size after layout */
} DNode;

typedef struct {
    i32  from, to;
    char label[DG_ELABEL_CAP];
    u8   style;
    u8   mk_from, mk_to;
    bool back;                  /* closes a cycle — ignored by the ranker */
} DEdge;

typedef struct {
    char label[DG_ELABEL_CAP];
    i16  parent;                /* enclosing subgraph, -1 at top level */
    bool used;                  /* empty subgraphs are never drawn */
    f32  x, y, w, h;
} DGroup;

typedef struct {
    char id[DG_ID_CAP];
    char label[DG_LABEL_CAP];
    bool actor;
    f32  cx, w;                 /* lifeline centre + head-box width */
} DPart;

typedef struct {
    i32  from, to;              /* participant index; from == to = self call */
    i32  span;                  /* note over A,B — second participant */
    char label[DG_LABEL_CAP];
    u8   kind;
    u8   style;
    u8   mk_to, mk_from;
    i8   act;                   /* +1 activate target, -1 deactivate source */
    f32  y, h;                  /* row top + row height */
    f32  ly;                    /* arrow baseline, below the label band */
} DMsg;

typedef struct { i32 part; i32 depth; f32 y0, y1; } DBar;

/* Parsed model — single-threaded, reset per public call. */
static DNode  g_nodes[DG_MAX_NODES];
static u32    g_nnodes;
static DEdge  g_edges[DG_MAX_EDGES];
static u32    g_nedges;
static DGroup g_groups[DG_MAX_GROUPS];
static u32    g_ngroups;
static DPart  g_parts[DG_MAX_PARTS];
static u32    g_nparts;
static DMsg   g_msgs[DG_MAX_MSGS];
static u32    g_nmsgs;
static DBar   g_bars[DG_MAX_BARS];
static u32    g_nbars;
static bool   g_trunc;     /* a cap was hit — fall back to the code block */
static u8     g_dir;       /* 0 TD/TB, 1 LR, 2 BT, 3 RL */
static u32    g_head_rows; /* tallest participant label, in rows */
static f32    g_w, g_h;    /* total diagram size */
static f32    g_cw, g_ch;  /* text cell, shrunk when the diagram is downscaled */

/* Sequence head-box band. Kept as a row *count* rather than a px height so it
 * survives dg_fit's rescale of g_ch, and so the layout pass and the draw pass
 * can never disagree about where the lifelines start. One row gives a
 * 2.0×cell band (DG_ROW_H + 0.85). */
static f32 dg_head_h(void) {
    u32 rows = g_head_rows ? g_head_rows : 1u;
    return g_ch * (DG_ROW_H * (f32)rows + 0.85f);
}

/* ---- small helpers ---- */
static bool dg_is_space(u8 c) { return c == ' ' || c == '\t'; }

static bool dg_is_word(u8 c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* End of the identifier starting at `i`.
 *
 * '-' and '.' belong to an id only *inside* one (`my-node`, `file.txt`): the
 * dashes of `A-->B` and the dots of `A..>B` are operators. Without that rule the
 * id scanner eats the operator and the whole statement collapses into a single
 * node named "A--", so an unspaced edge vanishes. */
static u32 dg_scan_id(const u8 *s, u32 len, u32 i) {
    u32 start = i;
    while (i < len) {
        u8 c = s[i];
        if (dg_is_word(c)) { i++; continue; }
        if ((c == '-' || c == '.') && i > start && i + 1 < len && dg_is_word(s[i+1])) {
            i++;
            continue;
        }
        break;
    }
    return i;
}

static void dg_strcpy(char *dst, u32 cap, const u8 *s, u32 n) {
    if (!dst || cap == 0 || (!s && n != 0)) return;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, s, n);
    dst[n] = 0;
}

/* Codepoints in [s, s+n), counting an undecodable byte as one cell so the
 * measure pass and the draw pass can never disagree about a label's width. */
static u32 dg_cells(const char *s, u32 n) {
    u32 cells = 0, i = 0;
    while (i < n) {
        u32 cp = 0;
        u32 adv = utf8_decode((const u8 *)s + i, n - i, &cp);
        i += adv ? adv : 1;
        cells++;
    }
    return cells;
}

/* Row `k` of a '\n'-separated label. False past the last row. */
static bool dg_row(const char *s, u32 k, const char **out, u32 *outlen) {
    u32 i = 0, row = 0;
    u32 n = (u32)strlen(s);
    for (;;) {
        u32 e = i;
        while (e < n && s[e] != '\n') e++;
        if (row == k) { *out = s + i; *outlen = e - i; return true; }
        if (e >= n) return false;
        i = e + 1;
        row++;
        if (row > DG_MAX_ROWS) return false;
    }
}

static void dg_text_metrics(const char *s, u32 *rows, u32 *maxcells) {
    u32 nr = 0, mx = 0;
    const char *p; u32 n;
    while (nr < DG_MAX_ROWS && dg_row(s, nr, &p, &n)) {
        u32 c = dg_cells(p, n);
        if (c > mx) mx = c;
        nr++;
    }
    if (nr == 0) nr = 1;
    *rows = nr;
    *maxcells = mx;
}

/* Copy a Mermaid label: trims, strips one layer of quotes, folds `<br/>` and a
 * literal `\n` into real rows, and replaces control bytes with a space so an
 * adversarial fence cannot push a NUL or an escape into the glyph stream. */
static void dg_set_label(char *dst, u32 cap, const u8 *s, u32 n) {
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!s) return;
    while (n && dg_is_space(s[0])) { s++; n--; }
    while (n && (dg_is_space(s[n-1]) || s[n-1] == '\r')) n--;
    if (n >= 2 && s[0] == '"' && s[n-1] == '"') { s++; n -= 2; }
    u32 o = 0, rows = 1;
    for (u32 i = 0; i < n && o + 1 < cap; ) {
        u8 c = s[i];
        bool brk = false;
        if (c == '<' && i + 3 <= n && (s[i+1] == 'b' || s[i+1] == 'B') &&
            (s[i+2] == 'r' || s[i+2] == 'R')) {
            u32 j = i + 3;
            while (j < n && j < i + 8 && s[j] != '>') j++;
            if (j < n && s[j] == '>') { brk = true; i = j + 1; }
        } else if (c == '\\' && i + 1 < n && s[i+1] == 'n') {
            brk = true; i += 2;
        }
        if (brk) {
            if (rows < DG_MAX_ROWS) { dst[o++] = '\n'; rows++; }
            else dst[o++] = ' ';
            continue;
        }
        dst[o++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
        i++;
    }
    while (o && dst[o-1] == ' ') o--;
    dst[o] = 0;
}

/* Index of the first `c1c2` pair at or after `from`, or `len`. */
static u32 dg_find2(const u8 *s, u32 len, u32 from, u8 c1, u8 c2) {
    for (u32 i = from; i + 1 < len; i++)
        if (s[i] == c1 && s[i+1] == c2) return i;
    return len;
}

/* Index of the first `close` (1 or 2 bytes) at or after `from`, skipping
 * "quoted" spans so a label may contain the delimiter. `len` if unclosed. */
static u32 dg_find_close(const u8 *s, u32 len, u32 from, const char *close) {
    u32 cl = (u32)strlen(close);
    bool q = false;
    for (u32 i = from; i < len; i++) {
        if (s[i] == '"') { q = !q; continue; }
        if (q) continue;
        if (i + cl <= len && memcmp(s + i, close, cl) == 0) return i;
    }
    return len;
}

static bool dg_starts(const u8 *s, u32 len, const char *kw) {
    u32 n = (u32)strlen(kw);
    return len >= n && memcmp(s, kw, n) == 0;
}

/* Keyword at the head of the line, followed by a non-word byte. */
static bool dg_kw(const u8 *s, u32 len, const char *kw) {
    u32 n = (u32)strlen(kw);
    if (len < n || memcmp(s, kw, n) != 0) return false;
    return len == n || !dg_is_word(s[n]);
}

/* ---- model ---- */
static void dg_reset(void) {
    g_nnodes = 0; g_nedges = 0; g_ngroups = 0;
    g_nparts = 0; g_nmsgs = 0; g_nbars = 0;
    g_trunc = false; g_dir = 0; g_head_rows = 0; g_w = g_h = 0;
}

static i32 dg_intern(const u8 *id, u32 n, i16 group) {
    char key[DG_ID_CAP];
    dg_strcpy(key, sizeof key, id, n);
    if (!key[0]) return -1;
    for (u32 i = 0; i < g_nnodes; i++)
        if (strcmp(g_nodes[i].id, key) == 0) return (i32)i;
    if (g_nnodes >= DG_MAX_NODES) { g_trunc = true; return -1; }
    DNode *nd = &g_nodes[g_nnodes];
    memset(nd, 0, sizeof *nd);
    memcpy(nd->id, key, strlen(key) + 1);
    dg_strcpy(nd->label, sizeof nd->label, (const u8 *)key, (u32)strlen(key));
    nd->shape = SH_RECT;
    nd->group = group;
    /* Mark the whole ancestor chain: a subgraph that only wraps other
     * subgraphs still needs a box drawn around them. */
    for (i32 g = group, guard = 0; g >= 0 && guard < DG_MAX_GROUPS; guard++) {
        if ((u32)g >= g_ngroups) break;
        g_groups[g].used = true;
        g = g_groups[g].parent;
    }
    return (i32)g_nnodes++;
}

static void dg_add_edge(i32 from, i32 to, u8 style, u8 mk_from, u8 mk_to,
                        const char *label) {
    if (from < 0 || to < 0) return;
    if (g_nedges >= DG_MAX_EDGES) { g_trunc = true; return; }
    DEdge *e = &g_edges[g_nedges++];
    memset(e, 0, sizeof *e);
    e->from = from; e->to = to;
    e->style = style; e->mk_from = mk_from; e->mk_to = mk_to;
    e->back = (from == to);          /* a self loop can never set a rank */
    if (label) dg_strcpy(e->label, sizeof e->label, (const u8 *)label,
                         (u32)strlen(label));
}

/* ---- line scanner ---- */
typedef struct { const u8 *s; u32 len, i, lines; } DScan;

/* Next non-empty, non-comment line, trimmed. False at end / at the line cap. */
static bool dg_next_line(DScan *sc, const u8 **out, u32 *outlen) {
    while (sc->i < sc->len) {
        if (sc->lines++ >= DG_MAX_LINES) return false;
        u32 ls = sc->i;
        while (sc->i < sc->len && sc->s[sc->i] != '\n') sc->i++;
        u32 le = sc->i;
        if (sc->i < sc->len) sc->i++;
        while (ls < le && (dg_is_space(sc->s[ls]) || sc->s[ls] == '\r')) ls++;
        while (le > ls && (dg_is_space(sc->s[le-1]) || sc->s[le-1] == '\r')) le--;
        if (ls >= le) continue;
        if (le - ls >= 2 && sc->s[ls] == '%' && sc->s[ls+1] == '%') continue;
        *out = sc->s + ls;
        *outlen = le - ls;
        return true;
    }
    return false;
}

/* ---- flowchart ---- */

/* Node reference at *pi: id + optional shape bracket. Returns the node index. */
static i32 dg_parse_noderef(const u8 *s, u32 len, u32 *pi, i16 group) {
    u32 i = *pi;
    while (i < len && dg_is_space(s[i])) i++;
    u32 ids = i;
    i = dg_scan_id(s, len, i);
    if (i == ids) { *pi = i; return -1; }
    i32 idx = dg_intern(s + ids, i - ids, group);

    /* Shape brackets, longest opener first. */
    static const struct { const char *open, *close; u8 shape; } forms[] = {
        { "(((", ")))", SH_CIRCLE },
        { "([",  "])",  SH_STADIUM },
        { "[[",  "]]",  SH_SUBROUTINE },
        { "[(",  ")]",  SH_CYLINDER },
        { "[/",  "]",   SH_PARA },
        { "[\\", "]",   SH_PARA },
        { "{{",  "}}",  SH_HEX },
        { "((",  "))",  SH_CIRCLE },
        { "(",   ")",   SH_ROUND },
        { "[",   "]",   SH_RECT },
        { "{",   "}",   SH_DIAMOND },
        { NULL,  NULL,  0 }
    };
    for (u32 k = 0; forms[k].open; k++) {
        u32 ol = (u32)strlen(forms[k].open);
        if (i + ol > len || memcmp(s + i, forms[k].open, ol) != 0) continue;
        u32 ls = i + ol;
        u32 le = dg_find_close(s, len, ls, forms[k].close);
        /* Unclosed bracket: take the rest of the line rather than dropping the
         * node — the reader still gets a diagram, just a short one. */
        i = (le < len) ? le + (u32)strlen(forms[k].close) : len;
        if (i > len) i = len;
        if (idx >= 0) {
            const u8 *lp = s + ls;
            u32 ll = (le < len ? le : len) - ls;
            if (forms[k].shape == SH_PARA && ll && (lp[ll-1] == '/' || lp[ll-1] == '\\'))
                ll--;
            g_nodes[idx].shape = forms[k].shape;
            if (ll) dg_set_label(g_nodes[idx].label, sizeof g_nodes[idx].label, lp, ll);
        }
        break;
    }
    *pi = i;
    return idx;
}

/* Match a link operator at *pi — `-->` `---` `-.->` `==>`, the inline-label
 * forms (`-- text -->`, `-. text .->`, `== text ==>`) and the `<-->` bidi
 * prefix. Mermaid's own disambiguation is the rule here: exactly two dashes
 * *open* a labelled link, three or more are a complete link by themselves. */
static bool dg_parse_link(const u8 *s, u32 len, u32 *pi, u8 *style,
                          u8 *mk_from, u8 *mk_to, char *label, u32 lcap) {
    u32 i = *pi;
    label[0] = 0;
    *style = ES_SOLID; *mk_from = MK_NONE; *mk_to = MK_NONE;
    while (i < len && dg_is_space(s[i])) i++;
    if (i >= len) return false;
    if (s[i] == '<' && i + 1 < len && (s[i+1] == '-' || s[i+1] == '=')) {
        *mk_from = MK_ARROW;
        i++;
    }
    if (i >= len || (s[i] != '-' && s[i] != '=')) return false;

    if (s[i] == '=') {
        u32 run = 0;
        while (i < len && s[i] == '=') { i++; run++; }
        if (run < 2) return false;
        *style = ES_THICK;
        if (run == 2 && i < len && s[i] != '>') {
            u32 ts = i;
            u32 close = dg_find2(s, len, i, '=', '=');
            if (close >= len) return false;
            dg_set_label(label, lcap, s + ts, close - ts);
            i = close;
            while (i < len && s[i] == '=') i++;
        }
    } else if (i + 1 < len && s[i+1] == '.') {
        *style = ES_DOTTED;
        i += 2;
        if (i < len && (s[i] == '.' || s[i] == '-')) {
            while (i < len && s[i] == '.') i++;
            if (i < len && s[i] == '-') i++;
        } else {
            u32 ts = i;
            u32 close = dg_find2(s, len, i, '.', '-');
            if (close >= len) return false;
            dg_set_label(label, lcap, s + ts, close - ts);
            i = close + 2;
        }
    } else {
        u32 run = 0;
        while (i < len && s[i] == '-') { i++; run++; }
        if (run < 2) return false;
        if (run == 2 && i < len && s[i] != '>' && s[i] != 'x' && s[i] != 'o') {
            /* The closing operator is a dash *run*, not a single dash — a label
             * of "re-try" must not end the link at its own hyphen. */
            u32 ts = i;
            u32 close = dg_find2(s, len, i, '-', '-');
            if (close >= len) return false;
            dg_set_label(label, lcap, s + ts, close - ts);
            i = close;
            while (i < len && s[i] == '-') i++;
        }
    }

    if (i < len && s[i] == '>')      { *mk_to = MK_ARROW;     i++; }
    else if (i < len && s[i] == 'x') { *mk_to = MK_CROSS;     i++; }
    else if (i < len && s[i] == 'o') { *mk_to = MK_DIAMOND_O; i++; }
    *pi = i;
    return true;
}

/* `subgraph id [Title]` / `subgraph Title`. Returns the new group or -1. */
static i16 dg_open_group(const u8 *line, u32 ll, i16 parent) {
    u32 p = 8;                                   /* past "subgraph" */
    while (p < ll && dg_is_space(line[p])) p++;
    if (g_ngroups >= DG_MAX_GROUPS) { g_trunc = true; return parent; }
    DGroup *g = &g_groups[g_ngroups];
    memset(g, 0, sizeof *g);
    g->parent = parent;
    u32 ids = p;
    u32 ide = dg_scan_id(line, ll, p);
    u32 q = ide;
    while (q < ll && dg_is_space(line[q])) q++;
    if (q < ll && (line[q] == '[' || line[q] == '(')) {
        char close = line[q] == '[' ? ']' : ')';
        u32 ts = q + 1, te = ts;
        while (te < ll && line[te] != (u8)close) te++;
        dg_set_label(g->label, sizeof g->label, line + ts, te - ts);
    } else {
        dg_set_label(g->label, sizeof g->label, line + ids, ll - ids);
    }
    return (i16)g_ngroups++;
}

static void dg_parse_flow(DScan *sc) {
    i16 stack[DG_MAX_DEPTH];
    u32 depth = 0;
    const u8 *line; u32 ll;
    while (dg_next_line(sc, &line, &ll)) {
        if (dg_kw(line, ll, "subgraph")) {
            i16 parent = depth ? stack[depth-1] : (i16)-1;
            i16 g = dg_open_group(line, ll, parent);
            if (depth < DG_MAX_DEPTH) stack[depth++] = g;
            continue;
        }
        if (dg_kw(line, ll, "end")) { if (depth) depth--; continue; }
        if (dg_kw(line, ll, "direction")) {
            u32 p = 9;
            while (p < ll && dg_is_space(line[p])) p++;
            if (p + 1 < ll) {
                if (line[p] == 'L' && line[p+1] == 'R') g_dir = 1;
                else if (line[p] == 'R' && line[p+1] == 'L') g_dir = 3;
                else if (line[p] == 'B' && line[p+1] == 'T') g_dir = 2;
                else g_dir = 0;
            }
            continue;
        }
        /* Styling and interaction directives carry no geometry. */
        if (dg_kw(line, ll, "style") || dg_kw(line, ll, "classDef") ||
            dg_kw(line, ll, "class") || dg_kw(line, ll, "click") ||
            dg_kw(line, ll, "linkStyle") || dg_kw(line, ll, "accTitle") ||
            dg_kw(line, ll, "accDescr"))
            continue;

        i16 group = depth ? stack[depth-1] : (i16)-1;
        u32 p = 0;
        i32 prev = dg_parse_noderef(line, ll, &p, group);
        if (prev < 0) continue;
        for (u32 link = 0; link < DG_MAX_LINKS; link++) {
            u8 style, mkf, mkt;
            char elabel[DG_ELABEL_CAP];
            u32 before = p;
            if (!dg_parse_link(line, ll, &p, &style, &mkf, &mkt,
                               elabel, sizeof elabel)) { p = before; break; }
            /* `-->|text|` is the other half of the label syntax. */
            while (p < ll && dg_is_space(line[p])) p++;
            if (!elabel[0] && p < ll && line[p] == '|') {
                p++;
                u32 lbs = p;
                while (p < ll && line[p] != '|') p++;
                dg_set_label(elabel, sizeof elabel, line + lbs, p - lbs);
                if (p < ll) p++;
            }
            i32 cur = dg_parse_noderef(line, ll, &p, group);
            if (cur < 0) break;
            dg_add_edge(prev, cur, style, mkf, mkt, elabel);
            prev = cur;
        }
    }
}

/* ---- state diagrams ---- */

/* `[*]` is a marker, not an id: one start dot and one end ring per diagram. */
static i32 dg_state_ref(const u8 *s, u32 len, u32 *pi, bool as_target) {
    u32 i = *pi;
    while (i < len && dg_is_space(s[i])) i++;
    if (i + 2 < len && s[i] == '[' && s[i+1] == '*' && s[i+2] == ']') {
        *pi = i + 3;
        const char *key = as_target ? "\x01""end" : "\x01start";
        i32 idx = dg_intern((const u8 *)key, (u32)strlen(key), -1);
        if (idx >= 0) {
            g_nodes[idx].shape = as_target ? SH_RING : SH_DOT;
            g_nodes[idx].label[0] = 0;
        }
        return idx;
    }
    u32 ids = i;
    i = dg_scan_id(s, len, i);
    *pi = i;
    if (i == ids) return -1;
    i32 idx = dg_intern(s + ids, i - ids, -1);
    if (idx >= 0 && g_nodes[idx].shape == SH_RECT) g_nodes[idx].shape = SH_ROUND;
    return idx;
}

static void dg_parse_state(DScan *sc) {
    const u8 *line; u32 ll;
    bool in_note = false;
    while (dg_next_line(sc, &line, &ll)) {
        if (in_note) {                       /* multi-line `note … end note` */
            if (dg_kw(line, ll, "end")) in_note = false;
            continue;
        }
        if (dg_kw(line, ll, "note") || dg_kw(line, ll, "Note")) {
            /* `note X : text` is one line; `note X` opens a block that runs to
             * `end note`, and those body lines must not parse as states. */
            bool inline_note = false;
            for (u32 k = 0; k < ll; k++) if (line[k] == ':') { inline_note = true; break; }
            in_note = !inline_note;
            continue;
        }
        if (dg_kw(line, ll, "direction")) {
            u32 p = 9;
            while (p < ll && dg_is_space(line[p])) p++;
            if (p + 1 < ll && line[p] == 'L' && line[p+1] == 'R') g_dir = 1;
            else g_dir = 0;
            continue;
        }
        if (dg_kw(line, ll, "classDef") || dg_kw(line, ll, "class") ||
            dg_kw(line, ll, "style") || dg_kw(line, ll, "accTitle") ||
            dg_kw(line, ll, "accDescr"))
            continue;
        if (ll == 1 && (line[0] == '}' || line[0] == '{')) continue;

        /* `state "Long name" as id` / `state id { … }` / `state id <<fork>>` */
        if (dg_kw(line, ll, "state")) {
            u32 p = 5;
            while (p < ll && dg_is_space(line[p])) p++;
            char desc[DG_LABEL_CAP]; desc[0] = 0;
            if (p < ll && line[p] == '"') {
                u32 ts = ++p;
                while (p < ll && line[p] != '"') p++;
                dg_set_label(desc, sizeof desc, line + ts, p - ts);
                if (p < ll) p++;
                while (p < ll && dg_is_space(line[p])) p++;
                if (dg_kw(line + p, ll - p, "as")) p += 2;
                while (p < ll && dg_is_space(line[p])) p++;
            }
            u32 ids = p;
            u32 ide = dg_scan_id(line, ll, p);
            if (ide == ids) continue;
            i32 idx = dg_intern(line + ids, ide - ids, -1);
            if (idx < 0) continue;
            if (g_nodes[idx].shape == SH_RECT) g_nodes[idx].shape = SH_ROUND;
            if (desc[0])
                dg_strcpy(g_nodes[idx].label, sizeof g_nodes[idx].label,
                          (const u8 *)desc, (u32)strlen(desc));
            continue;
        }

        u32 p = 0;
        i32 from = dg_state_ref(line, ll, &p, false);
        if (from < 0) continue;
        u8 style, mkf, mkt;
        char elabel[DG_ELABEL_CAP];
        if (!dg_parse_link(line, ll, &p, &style, &mkf, &mkt,
                           elabel, sizeof elabel))
            continue;
        i32 to = dg_state_ref(line, ll, &p, true);
        if (to < 0) continue;
        while (p < ll && dg_is_space(line[p])) p++;
        if (p < ll && line[p] == ':')
            dg_set_label(elabel, sizeof elabel, line + p + 1, ll - p - 1);
        dg_add_edge(from, to, style, mkf, mkt ? mkt : (u8)MK_ARROW, elabel);
    }
}

/* ---- class diagrams ---- */

/* Append one member line to a class node's compartment. */
static void dg_class_member(i32 idx, const u8 *s, u32 n) {
    if (idx < 0) return;
    while (n && dg_is_space(s[0])) { s++; n--; }
    while (n && dg_is_space(s[n-1])) n--;
    if (!n) return;
    DNode *nd = &g_nodes[idx];
    u32 have = (u32)strlen(nd->body);
    if (have + 2 >= sizeof nd->body) return;         /* compartment is full */
    u32 have_rows = 0;
    for (u32 i = 0; i < have; i++) if (nd->body[i] == '\n') have_rows++;
    if (have_rows + 1 >= DG_MAX_ROWS) return;
    char row[DG_ELABEL_CAP];
    dg_set_label(row, sizeof row, s, n);
    if (!row[0]) return;
    u32 rl = (u32)strlen(row);
    if (have) nd->body[have++] = '\n';
    if (have + rl >= sizeof nd->body) rl = (u32)sizeof nd->body - have - 1;
    memcpy(nd->body + have, row, rl);
    nd->body[have + rl] = 0;
}

/* One UML relation end: `<|` `*` `o` `<` before the line, `|>` `*` `o` `>`
 * after it. Returns the marker and advances past it. */
static u8 dg_class_mark_left(const u8 *s, u32 len, u32 *pi) {
    u32 i = *pi;
    if (i + 1 < len && s[i] == '<' && s[i+1] == '|') { *pi = i + 2; return MK_TRIANGLE; }
    if (i < len && s[i] == '<') { *pi = i + 1; return MK_ARROW; }
    if (i < len && s[i] == '*') { *pi = i + 1; return MK_DIAMOND_F; }
    if (i < len && s[i] == 'o') { *pi = i + 1; return MK_DIAMOND_O; }
    return MK_NONE;
}
static u8 dg_class_mark_right(const u8 *s, u32 len, u32 *pi) {
    u32 i = *pi;
    if (i + 1 < len && s[i] == '|' && s[i+1] == '>') { *pi = i + 2; return MK_TRIANGLE; }
    if (i < len && s[i] == '>') { *pi = i + 1; return MK_ARROW; }
    if (i < len && s[i] == '*') { *pi = i + 1; return MK_DIAMOND_F; }
    if (i < len && s[i] == 'o') { *pi = i + 1; return MK_DIAMOND_O; }
    return MK_NONE;
}

/* Cardinality strings (`A "1" --> "*" B`) carry no geometry — skip them. */
static void dg_skip_quoted(const u8 *s, u32 len, u32 *pi) {
    u32 i = *pi;
    while (i < len && dg_is_space(s[i])) i++;
    if (i < len && s[i] == '"') {
        i++;
        while (i < len && s[i] != '"') i++;
        if (i < len) i++;
        *pi = i;
    }
}

static bool dg_parse_class_rel(const u8 *line, u32 ll) {
    u32 p = 0;
    while (p < ll && dg_is_space(line[p])) p++;
    u32 ids = p;
    u32 ide = dg_scan_id(line, ll, p);
    if (ide == ids) return false;
    p = ide;
    dg_skip_quoted(line, ll, &p);
    while (p < ll && dg_is_space(line[p])) p++;
    u32 mark_start = p;
    u8 mk_from = dg_class_mark_left(line, ll, &p);
    if (p >= ll || (line[p] != '-' && line[p] != '.')) { p = mark_start; return false; }
    u8 c = line[p], style = (c == '.') ? ES_DOTTED : ES_SOLID;
    u32 run = 0;
    while (p < ll && (line[p] == '-' || line[p] == '.')) { p++; run++; }
    if (run < 2) return false;
    u8 mk_to = dg_class_mark_right(line, ll, &p);
    dg_skip_quoted(line, ll, &p);
    while (p < ll && dg_is_space(line[p])) p++;
    u32 tds = p;
    u32 tde = dg_scan_id(line, ll, p);
    if (tde == tds) return false;
    p = tde;
    char elabel[DG_ELABEL_CAP]; elabel[0] = 0;
    while (p < ll && dg_is_space(line[p])) p++;
    if (p < ll && line[p] == ':')
        dg_set_label(elabel, sizeof elabel, line + p + 1, ll - p - 1);

    i32 a = dg_intern(line + ids, ide - ids, -1);
    i32 b = dg_intern(line + tds, tde - tds, -1);
    if (a < 0 || b < 0) return true;         /* consumed, just over the cap */
    g_nodes[a].shape = SH_CLASS;
    g_nodes[b].shape = SH_CLASS;
    dg_add_edge(a, b, style, mk_from, mk_to, elabel);
    return true;
}

static void dg_parse_class(DScan *sc) {
    const u8 *line; u32 ll;
    i32 open = -1;                            /* class whose `{` block is open */
    while (dg_next_line(sc, &line, &ll)) {
        if (open >= 0) {
            if (line[0] == '}') { open = -1; continue; }
            dg_class_member(open, line, ll);
            continue;
        }
        if (dg_kw(line, ll, "direction")) {
            u32 p = 9;
            while (p < ll && dg_is_space(line[p])) p++;
            if (p + 1 < ll && line[p] == 'L' && line[p+1] == 'R') g_dir = 1;
            else g_dir = 0;
            continue;
        }
        if (dg_kw(line, ll, "note") || dg_kw(line, ll, "cssClass") ||
            dg_kw(line, ll, "style") || dg_kw(line, ll, "classDef") ||
            dg_kw(line, ll, "click") || dg_kw(line, ll, "callback") ||
            dg_kw(line, ll, "link") || dg_kw(line, ll, "accTitle") ||
            dg_kw(line, ll, "accDescr") || dg_kw(line, ll, "namespace"))
            continue;
        if (ll == 1 && (line[0] == '}' || line[0] == '{')) continue;

        if (dg_kw(line, ll, "class")) {
            u32 p = 5;
            while (p < ll && dg_is_space(line[p])) p++;
            u32 ids = p, ide = dg_scan_id(line, ll, p);
            if (ide == ids) continue;
            i32 idx = dg_intern(line + ids, ide - ids, -1);
            if (idx < 0) continue;
            g_nodes[idx].shape = SH_CLASS;
            p = ide;
            while (p < ll && dg_is_space(line[p])) p++;
            if (p < ll && line[p] == '{') { open = idx; continue; }
            if (p < ll && line[p] == ':') dg_class_member(idx, line + p + 1, ll - p - 1);
            continue;
        }
        if (dg_parse_class_rel(line, ll)) continue;

        /* `Animal : +int age` — member without a block. */
        u32 p = 0;
        u32 ids = p, ide = dg_scan_id(line, ll, p);
        if (ide == ids) continue;
        p = ide;
        while (p < ll && dg_is_space(line[p])) p++;
        if (p < ll && line[p] == ':') {
            i32 idx = dg_intern(line + ids, ide - ids, -1);
            if (idx < 0) continue;
            g_nodes[idx].shape = SH_CLASS;
            dg_class_member(idx, line + p + 1, ll - p - 1);
        } else if (p >= ll) {
            i32 idx = dg_intern(line + ids, ide - ids, -1);
            if (idx >= 0) g_nodes[idx].shape = SH_CLASS;
        }
    }
}

/* ---- sequence diagrams ---- */
static i32 dg_part(const u8 *s, u32 n) {
    while (n && dg_is_space(s[0])) { s++; n--; }
    while (n && dg_is_space(s[n-1])) n--;
    char key[DG_ID_CAP];
    dg_strcpy(key, sizeof key, s, n);
    u32 k = (u32)strlen(key);
    if (!k) return -1;
    for (u32 i = 0; i < g_nparts; i++)
        if (strcmp(g_parts[i].id, key) == 0) return (i32)i;
    if (g_nparts >= DG_MAX_PARTS) { g_trunc = true; return -1; }
    DPart *p = &g_parts[g_nparts];
    memset(p, 0, sizeof *p);
    memcpy(p->id, key, k + 1);
    dg_strcpy(p->label, sizeof p->label, (const u8 *)key, k);
    return (i32)g_nparts++;
}

static DMsg *dg_add_msg(u8 kind) {
    if (g_nmsgs >= DG_MAX_MSGS) { g_trunc = true; return NULL; }
    DMsg *m = &g_msgs[g_nmsgs++];
    memset(m, 0, sizeof *m);
    m->kind = kind;
    m->from = m->to = m->span = -1;
    return m;
}

/* `A->>+B: text` — arrow shapes are `->` `-->` `->>` `-->>` `-x` `--x` `-)`. */
static bool dg_parse_seq_msg(const u8 *line, u32 ll) {
    u32 p = 0;
    while (p < ll && dg_is_space(line[p])) p++;
    u32 ids = p;
    while (p < ll && line[p] != '-' && line[p] != '<' && line[p] != ':') p++;
    u32 ide = p;
    if (ide == ids) return false;
    u8 mk_from = MK_NONE;
    if (p < ll && line[p] == '<') {
        while (p < ll && line[p] == '<') p++;
        mk_from = MK_ARROW;
    }
    if (p >= ll || line[p] != '-') return false;
    u32 run = 0;
    while (p < ll && line[p] == '-') { p++; run++; }
    if (run < 1 || run > 2) return false;
    u8 style = (run == 2) ? ES_DOTTED : ES_SOLID;
    u8 mk_to = MK_NONE;
    if (p < ll && line[p] == '>') {
        while (p < ll && line[p] == '>') p++;
        mk_to = MK_ARROW;
    } else if (p < ll && line[p] == 'x') { p++; mk_to = MK_CROSS; }
    else if (p < ll && line[p] == ')')   { p++; mk_to = MK_ARROW; }
    else return false;
    i8 act = 0;
    if (p < ll && line[p] == '+') { act = 1; p++; }
    else if (p < ll && line[p] == '-') { act = -1; p++; }
    u32 tds = p;
    while (p < ll && line[p] != ':') p++;
    u32 tde = p;
    if (tde == tds) return false;

    i32 a = dg_part(line + ids, ide - ids);
    i32 b = dg_part(line + tds, tde - tds);
    if (a < 0 || b < 0) return true;
    DMsg *m = dg_add_msg(SQ_MSG);
    if (!m) return true;
    m->from = a; m->to = b;
    m->style = style; m->mk_to = mk_to; m->mk_from = mk_from;
    m->act = act;
    if (p < ll) dg_set_label(m->label, sizeof m->label, line + p + 1, ll - p - 1);
    return true;
}

static void dg_parse_seq(DScan *sc) {
    const u8 *line; u32 ll;
    while (dg_next_line(sc, &line, &ll)) {
        if (dg_kw(line, ll, "participant") || dg_kw(line, ll, "actor")) {
            bool actor = line[0] == 'a';
            u32 p = actor ? 5u : 11u;
            while (p < ll && dg_is_space(line[p])) p++;
            u32 ids = p, ide = dg_scan_id(line, ll, p);
            if (ide == ids) continue;
            i32 idx = dg_part(line + ids, ide - ids);
            if (idx < 0) continue;
            g_parts[idx].actor = actor;
            p = ide;
            while (p < ll && dg_is_space(line[p])) p++;
            if (dg_kw(line + p, ll - p, "as")) {
                p += 2;
                dg_set_label(g_parts[idx].label, sizeof g_parts[idx].label,
                             line + p, ll - p);
            }
            continue;
        }
        if (dg_kw(line, ll, "activate") || dg_kw(line, ll, "deactivate")) {
            bool on = line[0] == 'a';
            u32 p = on ? 8u : 10u;
            while (p < ll && dg_is_space(line[p])) p++;
            i32 idx = dg_part(line + p, ll - p);
            if (idx < 0) continue;
            DMsg *m = dg_add_msg(on ? SQ_ACT : SQ_DEACT);
            if (m) m->to = idx;
            continue;
        }
        if (dg_kw(line, ll, "Note") || dg_kw(line, ll, "note")) {
            u32 p = 4;
            while (p < ll && dg_is_space(line[p])) p++;
            u8 kind;
            if (dg_kw(line + p, ll - p, "left"))       { kind = SQ_NOTE_L; p += 4; }
            else if (dg_kw(line + p, ll - p, "right")) { kind = SQ_NOTE_R; p += 5; }
            else if (dg_kw(line + p, ll - p, "over"))  { kind = SQ_NOTE_OVER; p += 4; }
            else continue;
            while (p < ll && dg_is_space(line[p])) p++;
            if (kind != SQ_NOTE_OVER && dg_kw(line + p, ll - p, "of")) p += 2;
            while (p < ll && dg_is_space(line[p])) p++;
            u32 as = p;
            while (p < ll && line[p] != ':' && line[p] != ',') p++;
            i32 a = dg_part(line + as, p - as);
            i32 b = -1;
            if (p < ll && line[p] == ',') {
                u32 bs = ++p;
                while (p < ll && line[p] != ':') p++;
                b = dg_part(line + bs, p - bs);
            }
            if (a < 0) continue;
            DMsg *m = dg_add_msg(kind);
            if (!m) continue;
            m->to = a; m->span = b;
            if (p < ll && line[p] == ':')
                dg_set_label(m->label, sizeof m->label, line + p + 1, ll - p - 1);
            continue;
        }
        /* Grouping constructs carry no geometry we model; their bodies still
         * render as ordinary messages. */
        if (dg_kw(line, ll, "loop") || dg_kw(line, ll, "alt") ||
            dg_kw(line, ll, "else") || dg_kw(line, ll, "opt") ||
            dg_kw(line, ll, "par") || dg_kw(line, ll, "and") ||
            dg_kw(line, ll, "critical") || dg_kw(line, ll, "option") ||
            dg_kw(line, ll, "break") || dg_kw(line, ll, "rect") ||
            dg_kw(line, ll, "box") || dg_kw(line, ll, "end") ||
            dg_kw(line, ll, "autonumber") || dg_kw(line, ll, "title") ||
            dg_kw(line, ll, "accTitle") || dg_kw(line, ll, "accDescr") ||
            dg_kw(line, ll, "links") || dg_kw(line, ll, "link"))
            continue;
        dg_parse_seq_msg(line, ll);
    }
}

/* ---- build ---- */

/* Header keyword → kind, leaving the scanner on the first body line. Skips a
 * YAML frontmatter block and any leading `%%{init}%%` directive. */
static MdDiagramKind dg_header(DScan *sc) {
    const u8 *line; u32 ll;
    if (!dg_next_line(sc, &line, &ll)) return MD_DIAGRAM_NONE;
    if (ll >= 3 && dg_starts(line, ll, "---")) {         /* frontmatter */
        bool closed = false;
        while (dg_next_line(sc, &line, &ll)) {
            if (dg_starts(line, ll, "---")) { closed = true; break; }
        }
        if (!closed || !dg_next_line(sc, &line, &ll)) return MD_DIAGRAM_NONE;
    }
    if (dg_kw(line, ll, "graph") || dg_kw(line, ll, "flowchart") ||
        dg_starts(line, ll, "flowchart-")) {
        u32 p = 0;
        while (p < ll && !dg_is_space(line[p])) p++;
        while (p < ll && dg_is_space(line[p])) p++;
        if (p + 1 < ll) {
            u8 a = line[p], b = line[p+1];
            if (a == 'L' && b == 'R') g_dir = 1;
            else if (a == 'B' && b == 'T') g_dir = 2;
            else if (a == 'R' && b == 'L') g_dir = 3;
            else g_dir = 0;
        }
        return MD_DIAGRAM_FLOWCHART;
    }
    if (dg_kw(line, ll, "sequenceDiagram")) return MD_DIAGRAM_SEQUENCE;
    if (dg_kw(line, ll, "stateDiagram-v2") || dg_kw(line, ll, "stateDiagram"))
        return MD_DIAGRAM_STATE;
    if (dg_kw(line, ll, "classDiagram")) return MD_DIAGRAM_CLASS;
    return MD_DIAGRAM_NONE;
}

/* Parse `src` into the model. Returns the kind, or NONE when the header is
 * unknown, a cap was hit, or the body produced nothing drawable. */
static MdDiagramKind dg_build(const u8 *src, u32 len) {
    dg_reset();
    if (!src || len == 0 || len > DG_MAX_SRC) return MD_DIAGRAM_NONE;
    DScan sc = { src, len, 0, 0 };
    MdDiagramKind kind = dg_header(&sc);
    switch (kind) {
    case MD_DIAGRAM_FLOWCHART: dg_parse_flow(&sc);  break;
    case MD_DIAGRAM_SEQUENCE:  dg_parse_seq(&sc);   break;
    case MD_DIAGRAM_STATE:     dg_parse_state(&sc); break;
    case MD_DIAGRAM_CLASS:     dg_parse_class(&sc); break;
    default: return MD_DIAGRAM_NONE;
    }
    /* A recognised header over an unparseable body must not render as an empty
     * box — the raw fence is strictly more useful than that. */
    if (g_trunc) return MD_DIAGRAM_NONE;
    if (kind == MD_DIAGRAM_SEQUENCE)
        return (g_nparts > 0 && g_nmsgs > 0) ? kind : MD_DIAGRAM_NONE;
    return g_nnodes > 0 ? kind : MD_DIAGRAM_NONE;
}

/* ---- graph layout ---- */
/* Chip an edge label is drawn on, centred at (mx,my) in diagram space. One
 * definition so the layout's bounding box and the draw agree — a label may be
 * several rows tall, and measuring it as one long run would make the chip
 * wide enough to reach outside the diagram. */
static void dg_chip_box(const char *label, f32 mx, f32 my,
                        f32 *bx, f32 *by, f32 *bw, f32 *bh) {
    u32 rows, cells;
    dg_text_metrics(label, &rows, &cells);
    f32 w = (f32)cells * g_cw + 4.0f;
    f32 h = (f32)rows * g_ch * DG_ROW_H;   /* == what dg_text_box will draw */
    *bx = mx - w * 0.5f;
    *by = my - h * 0.5f;
    *bw = w;
    *bh = h;
}

/* Midpoint an edge's label chip is centred on. */
static void dg_edge_label_mid(const DEdge *e, f32 *mx, f32 *my) {
    const DNode *a = &g_nodes[e->from], *b = &g_nodes[e->to];
    if (e->from == e->to) {
        *mx = a->x + a->w + g_cw * 2.0f;
        *my = a->y + a->h * 0.55f;
        return;
    }
    *mx = (a->x + a->w * 0.5f + b->x + b->w * 0.5f) * 0.5f;
    *my = (a->y + a->h * 0.5f + b->y + b->h * 0.5f) * 0.5f;
}

static void dg_size_node(DNode *n) {
    u32 rows, cells;
    dg_text_metrics(n->label, &rows, &cells);
    f32 w = (f32)(cells + 2) * g_cw;
    f32 h = g_ch * 1.9f + (f32)(rows - 1) * g_ch * DG_ROW_H;
    switch (n->shape) {
    case SH_DIAMOND: w += 2.2f * g_cw; h += 0.5f * g_ch; break;
    case SH_HEX:     w += 1.6f * g_cw; break;
    case SH_PARA:    w += 1.6f * g_cw; break;
    case SH_CYLINDER: h += 0.7f * g_ch; break;
    case SH_SUBROUTINE: w += 1.6f * g_cw; break;
    case SH_STADIUM: w += 1.0f * g_cw; break;
    case SH_CIRCLE: { f32 d = w > h ? w : h; w = h = d * 1.05f; break; }
    case SH_DOT: case SH_RING: w = h = g_ch * 0.9f; break;
    case SH_CLASS: {
        u32 mrows = 0, mcells = 0;
        if (n->body[0]) {
            dg_text_metrics(n->body, &mrows, &mcells);
            if ((f32)(mcells + 2) * g_cw > w) w = (f32)(mcells + 2) * g_cw;
            h += (f32)mrows * g_ch * 1.2f + g_ch * 0.5f;
        }
        break;
    }
    default: break;
    }
    n->w = w;
    n->h = h;
}

/* Mark the edges that close a cycle so the ranker only ever sees a DAG.
 * Without this a single `A --> B --> A` inflates ranks until the iteration cap
 * stops it, and the diagram grows a column of empty ranks. */
static void dg_mark_back_edges(void) {
    u8  state[DG_MAX_NODES];
    i32 stack[DG_MAX_NODES];
    u32 iter[DG_MAX_NODES];
    memset(state, 0, sizeof state);
    for (u32 root = 0; root < g_nnodes; root++) {
        if (state[root]) continue;
        i32 sp = 0;
        stack[0] = (i32)root; iter[0] = 0; state[root] = 1;
        while (sp >= 0) {
            i32 v = stack[sp];
            u32 e = iter[sp];
            while (e < g_nedges &&
                   (g_edges[e].from != v || g_edges[e].to == v)) e++;
            if (e >= g_nedges) { state[v] = 2; sp--; continue; }
            iter[sp] = e + 1;
            i32 w = g_edges[e].to;
            if (w < 0 || (u32)w >= g_nnodes) continue;
            if (state[w] == 1) g_edges[e].back = true;
            else if (state[w] == 0 && sp + 1 < (i32)g_nnodes) {
                state[w] = 1;
                stack[++sp] = w;
                iter[sp] = 0;
            }
        }
    }
}

/* Root subgraph of `n`, used as the primary within-rank sort key so a cluster
 * stays contiguous and its box stays tight. */
static i32 dg_group_root(const DNode *n) {
    i32 g = n->group;
    for (u32 guard = 0; guard < DG_MAX_GROUPS && g >= 0; guard++) {
        if ((u32)g >= g_ngroups) return -1;
        i32 p = g_groups[g].parent;
        if (p < 0) break;
        g = p;
    }
    return g;
}

static void dg_layout_graph(void) {
    for (u32 i = 0; i < g_nnodes; i++) dg_size_node(&g_nodes[i]);

    dg_mark_back_edges();
    for (u32 i = 0; i < g_nnodes; i++) g_nodes[i].rank = 0;
    for (u32 it = 0; it < g_nnodes; it++) {
        bool changed = false;
        for (u32 e = 0; e < g_nedges; e++) {
            if (g_edges[e].back) continue;
            DNode *a = &g_nodes[g_edges[e].from], *b = &g_nodes[g_edges[e].to];
            if (b->rank < a->rank + 1 && a->rank < DG_MAX_RANK) {
                b->rank = a->rank + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    i32 maxrank = 0;
    for (u32 i = 0; i < g_nnodes; i++) {
        if (g_nodes[i].rank > DG_MAX_RANK) g_nodes[i].rank = DG_MAX_RANK;
        if (g_nodes[i].rank > maxrank) maxrank = g_nodes[i].rank;
    }
    if (g_dir == 2 || g_dir == 3)                    /* BT / RL flip the axis */
        for (u32 i = 0; i < g_nnodes; i++)
            g_nodes[i].rank = maxrank - g_nodes[i].rank;

    bool horizontal = (g_dir == 1 || g_dir == 3);
    f32 margin = g_cw * 1.5f;
    f32 rank_gap = horizontal ? g_cw * 5.0f : g_ch * 2.4f;
    f32 cross_gap = horizontal ? g_ch * 1.1f : g_cw * 2.0f;
    if (g_ngroups) rank_gap *= 1.5f;                 /* room for cluster boxes */

    f32 rank_cursor = margin;
    f32 total_cross = 0;
    for (i32 r = 0; r <= maxrank; r++) {
        i32 idx[DG_MAX_NODES];
        u32 k = 0;
        for (u32 i = 0; i < g_nnodes; i++)
            if (g_nodes[i].rank == r) idx[k++] = (i32)i;
        if (!k) continue;

        /* One downward barycentre pass: order each rank by the mean position
         * of its predecessors, clusters first. Deterministic (ties keep source
         * order) and enough to unpick the common crossings. */
        if (r > 0) {
            f32 bc[DG_MAX_NODES];
            for (u32 j = 0; j < k; j++) {
                f32 sum = 0; u32 cnt = 0;
                for (u32 e = 0; e < g_nedges; e++) {
                    if (g_edges[e].to != idx[j] || g_edges[e].back) continue;
                    const DNode *pn = &g_nodes[g_edges[e].from];
                    if (pn->rank != r - 1) continue;
                    sum += (f32)pn->order;
                    cnt++;
                }
                bc[j] = cnt ? sum / (f32)cnt : (f32)j;
            }
            for (u32 j = 1; j < k; j++) {            /* stable insertion sort */
                i32 vi = idx[j]; f32 vb = bc[j];
                i32 vg = dg_group_root(&g_nodes[vi]);
                u32 m = j;
                while (m > 0) {
                    i32 gi = dg_group_root(&g_nodes[idx[m-1]]);
                    if (gi > vg || (gi == vg && bc[m-1] > vb)) {
                        idx[m] = idx[m-1]; bc[m] = bc[m-1]; m--;
                    } else break;
                }
                idx[m] = vi; bc[m] = vb;
            }
        }
        for (u32 j = 0; j < k; j++) g_nodes[idx[j]].order = (i32)j;

        f32 thick = 0;
        for (u32 j = 0; j < k; j++) {
            f32 t = horizontal ? g_nodes[idx[j]].w : g_nodes[idx[j]].h;
            if (t > thick) thick = t;
        }
        f32 cross_cursor = margin;
        i32 prev_group = -2;
        for (u32 j = 0; j < k; j++) {
            DNode *n = &g_nodes[idx[j]];
            i32 gr = dg_group_root(n);
            if (j && gr != prev_group) cross_cursor += cross_gap;
            prev_group = gr;
            if (horizontal) {
                n->x = rank_cursor + (thick - n->w) * 0.5f;
                n->y = cross_cursor;
                cross_cursor += n->h + cross_gap;
            } else {
                n->y = rank_cursor + (thick - n->h) * 0.5f;
                n->x = cross_cursor;
                cross_cursor += n->w + cross_gap;
            }
        }
        f32 cross_extent = cross_cursor - cross_gap;
        if (cross_extent > total_cross) total_cross = cross_extent;
        rank_cursor += thick + rank_gap;
    }
    f32 rank_extent = rank_cursor - rank_gap + margin;
    if (horizontal) { g_w = rank_extent; g_h = total_cross + margin; }
    else            { g_w = total_cross + margin; g_h = rank_extent; }


    /* Subgraph boxes: bounding box of every member, then of every nested box.
     * Groups are pushed in source order, so a child always has a higher index
     * than its parent and one reverse pass accumulates the nesting. */
    if (g_ngroups) {
        for (u32 g = 0; g < g_ngroups; g++) {
            g_groups[g].x = g_groups[g].y = 1e9f;
            g_groups[g].w = g_groups[g].h = -1e9f;   /* right/bottom for now */
        }
        for (u32 i = 0; i < g_nnodes; i++) {
            i32 g = g_nodes[i].group;
            for (u32 guard = 0; guard < DG_MAX_GROUPS && g >= 0; guard++) {
                if ((u32)g >= g_ngroups) break;
                DGroup *gp = &g_groups[g];
                if (g_nodes[i].x < gp->x) gp->x = g_nodes[i].x;
                if (g_nodes[i].y < gp->y) gp->y = g_nodes[i].y;
                if (g_nodes[i].x + g_nodes[i].w > gp->w) gp->w = g_nodes[i].x + g_nodes[i].w;
                if (g_nodes[i].y + g_nodes[i].h > gp->h) gp->h = g_nodes[i].y + g_nodes[i].h;
                g = gp->parent;
            }
        }
        for (u32 g = 0; g < g_ngroups; g++) {
            DGroup *gp = &g_groups[g];
            if (!gp->used) continue;
            /* A long title over a small cluster decides the box width, and a
             * title broken with <br/> decides the title band's height, so
             * every row of the title is both measured and drawn. */
            u32 trows, tcells;
            dg_text_metrics(gp->label, &trows, &tcells);
            if (trows < 1) trows = 1;
            f32 band = g_ch * 1.9f + g_ch * 1.15f * (f32)(trows - 1);
            gp->x -= g_cw * 1.0f;
            gp->y -= band;                           /* title band */
            gp->w += g_cw * 1.0f;
            gp->h += band - g_ch * 1.2f;
            f32 need = gp->x + (f32)tcells * g_cw + g_cw * 1.6f;
            if (need > gp->w) gp->w = need;
        }
        for (i32 g = (i32)g_ngroups - 1; g > 0; g--) {
            DGroup *gp = &g_groups[g];
            if (!gp->used || gp->parent < 0) continue;
            DGroup *pp = &g_groups[gp->parent];
            if (gp->x < pp->x) pp->x = gp->x;
            if (gp->y < pp->y) pp->y = gp->y;
            if (gp->w > pp->w) pp->w = gp->w;
            if (gp->h > pp->h) pp->h = gp->h;
        }
        /* Boxes may reach above/left of the origin — shift the whole drawing
         * back inside instead of clipping it. */
        f32 minx = 0, miny = 0;
        for (u32 g = 0; g < g_ngroups; g++) {
            if (!g_groups[g].used) continue;
            if (g_groups[g].x < minx) minx = g_groups[g].x;
            if (g_groups[g].y < miny) miny = g_groups[g].y;
        }
        f32 dx = minx < margin ? margin - minx : 0;
        f32 dy = miny < margin ? margin - miny : 0;
        for (u32 g = 0; g < g_ngroups; g++) {
            DGroup *gp = &g_groups[g];
            if (!gp->used) { gp->w = gp->h = 0; continue; }
            gp->x += dx; gp->y += dy;
            gp->w = gp->w + dx - gp->x;              /* right → width */
            gp->h = gp->h + dy - gp->y;
            if (gp->x + gp->w + margin > g_w) g_w = gp->x + gp->w + margin;
            if (gp->y + gp->h + margin > g_h) g_h = gp->y + gp->h + margin;
        }
        for (u32 i = 0; i < g_nnodes; i++) { g_nodes[i].x += dx; g_nodes[i].y += dy; }
        g_w += dx; g_h += dy;
    }

    /* Edge decorations live outside the node boxes: a label chip is centred on
     * the midpoint of two node centres and a self loop hangs off its node's
     * right edge, so either can reach past the bounding box the nodes imply.
     * Grow the box around them — md_render clips nothing, and a chip drawn at
     * a negative x paints over the document to the left of the diagram. */
    /* Extents of the *content*, i.e. without the trailing margin, so that a
     * diagram with nothing sticking out keeps exactly the size it had. */
    f32 minx = margin, miny = margin;
    f32 maxx = g_w - margin, maxy = g_h - margin;
    for (u32 e = 0; e < g_nedges; e++) {
        const DEdge *ed = &g_edges[e];
        if (ed->from == ed->to) {
            const DNode *a = &g_nodes[ed->from];
            f32 loop_r = a->x + a->w + g_cw * 1.8f;
            if (loop_r > maxx) maxx = loop_r;
        }
        if (!ed->label[0]) continue;
        f32 mx, my, bx, by, bw, bh;
        dg_edge_label_mid(ed, &mx, &my);
        dg_chip_box(ed->label, mx, my, &bx, &by, &bw, &bh);
        if (bx < minx) minx = bx;
        if (by < miny) miny = by;
        if (bx + bw > maxx) maxx = bx + bw;
        if (by + bh > maxy) maxy = by + bh;
    }
    f32 sx = minx < margin ? margin - minx : 0;
    f32 sy = miny < margin ? margin - miny : 0;
    if (sx > 0 || sy > 0) {
        for (u32 i = 0; i < g_nnodes; i++) { g_nodes[i].x += sx; g_nodes[i].y += sy; }
        for (u32 g = 0; g < g_ngroups; g++) { g_groups[g].x += sx; g_groups[g].y += sy; }
    }
    g_w += sx; g_h += sy;
    if (maxx + sx + margin > g_w) g_w = maxx + sx + margin;
    if (maxy + sy + margin > g_h) g_h = maxy + sy + margin;
}

/* ---- sequence layout ---- */
static void dg_layout_seq(void) {
    f32 margin = g_cw * 1.5f;
    f32 gap[DG_MAX_PARTS];
    /* Space each lifeline needs *outside* its head box, for the self-call
     * loops and the side notes that hang off it. Everything that can stick out
     * sideways is accounted here, or the diagram measures narrower than it
     * draws and paints over the panel edge. */
    f32 need_l[DG_MAX_PARTS], need_r[DG_MAX_PARTS];
    for (u32 i = 0; i < g_nparts; i++) {
        u32 rows, cells;
        dg_text_metrics(g_parts[i].label, &rows, &cells);
        /* `participant A as one<br/>two` really is drawn stacked, so the head
         * band has to grow with it — a fixed two-cell band would put the
         * upper rows above the diagram origin, i.e. over the document. */
        if (rows > g_head_rows) g_head_rows = rows;
        g_parts[i].w = (f32)(cells + 2) * g_cw;
        if (g_parts[i].w < g_cw * 6.0f) g_parts[i].w = g_cw * 6.0f;
        gap[i] = g_cw * 3.0f;
        need_l[i] = need_r[i] = 0;
    }
    f32 head_h = dg_head_h();
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        if (ms->kind == SQ_ACT || ms->kind == SQ_DEACT) continue;
        u32 rows, cells;
        dg_text_metrics(ms->label, &rows, &cells);
        (void)rows;
        bool note = (ms->kind != SQ_MSG);
        f32 need = (f32)(cells + 4) * g_cw;
        i32 a = ms->to, b = note ? ms->span : ms->from;
        if (a < 0 || (u32)a >= g_nparts) continue;
        f32 half = g_parts[a].w * 0.5f;
        if (ms->kind == SQ_NOTE_L) {
            f32 want = need + g_cw - half;
            if (want > need_l[a]) need_l[a] = want;
            continue;
        }
        if (ms->kind == SQ_NOTE_R) {
            f32 want = need + g_cw - half;
            if (want > need_r[a]) need_r[a] = want;
            continue;
        }
        if (b < 0 || (u32)b >= g_nparts || a == b) {
            /* Self call, or a note over a single lifeline: both grow the same
             * lifeline's own margins. */
            if (ms->kind == SQ_NOTE_OVER) {
                f32 want = need * 0.5f - half;
                if (want > need_l[a]) need_l[a] = want;
                if (want > need_r[a]) need_r[a] = want;
            } else if (need > need_r[a]) {
                need_r[a] = need;
            }
            continue;
        }
        i32 lo = a < b ? a : b, hi = a < b ? b : a;
        f32 per = need / (f32)(hi - lo);
        for (i32 g = lo; g < hi; g++) if (per > gap[g]) gap[g] = per;
    }
    for (u32 i = 0; i + 1 < g_nparts; i++) {
        f32 want = need_r[i] + need_l[i+1] + g_cw;
        if (want > gap[i]) gap[i] = want;
    }

    f32 cx = margin + need_l[0] + g_parts[0].w * 0.5f;
    g_parts[0].cx = cx;
    for (u32 i = 1; i < g_nparts; i++) {
        cx += (g_parts[i-1].w + g_parts[i].w) * 0.5f + gap[i-1];
        g_parts[i].cx = cx;
    }
    g_w = g_parts[g_nparts-1].cx + g_parts[g_nparts-1].w * 0.5f +
          need_r[g_nparts-1] + margin;

    /* Rows. Activate/deactivate markers take no height; they only pin a y. */
    f32 y = head_h + g_ch * 1.0f;
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        u32 rows, cells;
        dg_text_metrics(ms->label, &rows, &cells);
        ms->y = y;
        ms->ly = y;
        switch (ms->kind) {
        case SQ_ACT: case SQ_DEACT: ms->h = 0; break;
        case SQ_NOTE_L: case SQ_NOTE_R: case SQ_NOTE_OVER:
            ms->h = (f32)rows * g_ch * 1.2f + g_ch * 1.4f;
            break;
        default:
            /* The label band sits above the arrow and grows with its rows. */
            ms->ly = y + (f32)rows * g_ch * DG_ROW_H + g_ch * 0.3f;
            ms->h = (ms->ly - y) + ((ms->from == ms->to) ? g_ch * 1.9f
                                                         : g_ch * 0.9f);
            break;
        }
        y += ms->h;
    }
    g_h = y + g_ch * 1.2f;

    /* Activation bars, closed in message order; an unclosed one runs to the
     * bottom of the diagram. */
    i32 depth[DG_MAX_PARTS];
    f32 open_y[DG_MAX_PARTS][4];
    for (u32 i = 0; i < g_nparts; i++) depth[i] = 0;
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        i32 on = -1, off = -1;
        if (ms->kind == SQ_ACT) on = ms->to;
        else if (ms->kind == SQ_DEACT) off = ms->to;
        else if (ms->kind == SQ_MSG && ms->act > 0) on = ms->to;
        else if (ms->kind == SQ_MSG && ms->act < 0) off = ms->from;
        if (on >= 0 && (u32)on < g_nparts && depth[on] < 4)
            open_y[on][depth[on]++] = ms->ly;
        if (off >= 0 && (u32)off < g_nparts && depth[off] > 0) {
            depth[off]--;
            if (g_nbars < DG_MAX_BARS) {
                DBar *b = &g_bars[g_nbars++];
                b->part = off; b->depth = depth[off];
                b->y0 = open_y[off][depth[off]];
                b->y1 = ms->ly;
            }
        }
    }
    for (u32 i = 0; i < g_nparts; i++)
        while (depth[i] > 0 && g_nbars < DG_MAX_BARS) {
            depth[i]--;
            DBar *b = &g_bars[g_nbars++];
            b->part = (i32)i; b->depth = depth[i];
            b->y0 = open_y[i][depth[i]];
            b->y1 = g_h - g_ch * 0.6f;
        }
}

/* Shrink an oversized diagram to the column instead of bleeding out of it.
 *
 * The fit is exact rather than floored: md_render reserves exactly the height
 * reported and clips nothing horizontally, so a diagram that stays wider than
 * the column paints over the panel edge. Text that ends up below DG_MIN_TEXT
 * is dropped by dg_text instead — at that size a label is noise, and the shape
 * map still reads.
 *
 * False means the diagram does not fit at any scale worth drawing: below
 * DG_MIN_SCALE the shapes are sub-pixel, and *clamping* to the floor would
 * leave the drawing wider than the column it was fitted to, painting over the
 * panel. The caller reports NONE for those, and the fence falls back to code,
 * which is the same trade every cap in this file makes. */
static bool dg_fit(f32 avail_w) {
    if (!(avail_w > 0.0f) || g_w <= avail_w || g_w <= 0.0f) return true;
    f32 s = avail_w / g_w;
    if (s < DG_MIN_SCALE) return false;   /* never collapse to nothing */
    for (u32 i = 0; i < g_nnodes; i++) {
        g_nodes[i].x *= s; g_nodes[i].y *= s;
        g_nodes[i].w *= s; g_nodes[i].h *= s;
    }
    for (u32 g = 0; g < g_ngroups; g++) {
        g_groups[g].x *= s; g_groups[g].y *= s;
        g_groups[g].w *= s; g_groups[g].h *= s;
    }
    for (u32 i = 0; i < g_nparts; i++) { g_parts[i].cx *= s; g_parts[i].w *= s; }
    for (u32 m = 0; m < g_nmsgs; m++) {
        g_msgs[m].y *= s; g_msgs[m].h *= s; g_msgs[m].ly *= s;
    }
    for (u32 b = 0; b < g_nbars; b++)  { g_bars[b].y0 *= s; g_bars[b].y1 *= s; }
    g_w *= s; g_h *= s; g_cw *= s; g_ch *= s;
    /* The multiply is not exactly the reciprocal of the divide; pin the width
     * so the reported box can never round a hair past the column. */
    if (g_w > avail_w) g_w = avail_w;
    return true;
}

/* Parse + lay out. Returns the kind actually drawable at this size. */
static MdDiagramKind dg_prepare(const u8 *src, u32 len, f32 avail_w,
                                f32 cw, f32 ch) {
    g_cw = cw; g_ch = ch;
    MdDiagramKind kind = dg_build(src, len);
    if (kind == MD_DIAGRAM_NONE) { g_w = g_h = 0; return kind; }
    if (kind == MD_DIAGRAM_SEQUENCE) dg_layout_seq();
    else                             dg_layout_graph();
    if (!isfinite(g_w) || !isfinite(g_h) || g_w <= 0.0f || g_h <= 0.0f) {
        g_w = g_h = 0;
        return MD_DIAGRAM_NONE;
    }
    if (!dg_fit(avail_w)) {              /* too wide to draw at any usable scale */
        g_w = g_h = 0;
        return MD_DIAGRAM_NONE;
    }
    return kind;
}

/* ---- render helpers ---- */
static void dg_center(const DNode *n, f32 ox, f32 oy, f32 *cx, f32 *cy) {
    *cx = ox + n->x + n->w * 0.5f;
    *cy = oy + n->y + n->h * 0.5f;
}

/* Boundary point of a node's box in the direction of (tx,ty). */
static void dg_edge_point(const DNode *n, f32 ox, f32 oy, f32 tx, f32 ty,
                          f32 *px, f32 *py) {
    f32 cx, cy; dg_center(n, ox, oy, &cx, &cy);
    f32 dx = tx - cx, dy = ty - cy;
    f32 hw = n->w * 0.5f, hh = n->h * 0.5f;
    f32 sx = (fabsf(dx) > 1e-3f) ? hw / fabsf(dx) : 1e9f;
    f32 sy = (fabsf(dy) > 1e-3f) ? hh / fabsf(dy) : 1e9f;
    f32 s = sx < sy ? sx : sy;
    *px = cx + dx * s;
    *py = cy + dy * s;
}

static void dg_dashed(Renderer *r, f32 x0, f32 y0, f32 x1, f32 y1,
                      f32 t, Color c) {
    f32 dx = x1 - x0, dy = y1 - y0;
    f32 dist = sqrtf(dx*dx + dy*dy);
    if (!(dist > 0.5f)) return;
    f32 step = fmaxf(4.0f, g_ch * 0.35f);
    f32 ux = dx / dist, uy = dy / dist;
    u32 guard = 0;
    for (f32 s = 0; s < dist && guard < 512; s += step * 2.0f, guard++) {
        f32 e = fminf(s + step, dist);
        renderer_draw_line(r, x0 + ux*s, y0 + uy*s, x0 + ux*e, y0 + uy*e, t, c);
    }
}

/* Polyline arc — the renderer only draws straight segments, so every curve
 * here is a bounded fan of them. */
static void dg_arc(Renderer *r, f32 cx, f32 cy, f32 rx, f32 ry,
                   f32 a0, f32 a1, u32 segs, f32 t, Color c) {
    if (segs < 2) segs = 2;
    if (segs > 24) segs = 24;
    f32 px = cx + rx * cosf(a0), py = cy + ry * sinf(a0);
    for (u32 i = 1; i <= segs; i++) {
        f32 a = a0 + (a1 - a0) * ((f32)i / (f32)segs);
        f32 qx = cx + rx * cosf(a), qy = cy + ry * sinf(a);
        renderer_draw_line(r, px, py, qx, qy, t, c);
        px = qx; py = qy;
    }
}

/* End decoration for an edge whose tip is at (tx,ty) travelling along the unit
 * vector (ux,uy). */
static void dg_marker(Renderer *r, f32 tx, f32 ty, f32 ux, f32 uy,
                      u8 mk, f32 size, f32 t, Color c) {
    if (mk == MK_NONE) return;
    f32 nx = -uy, ny = ux;
    f32 bx = tx - ux * size, by = ty - uy * size;
    switch (mk) {
    case MK_ARROW:
        renderer_draw_line(r, tx, ty, bx + nx*size*0.5f, by + ny*size*0.5f, t, c);
        renderer_draw_line(r, tx, ty, bx - nx*size*0.5f, by - ny*size*0.5f, t, c);
        break;
    case MK_CROSS: {
        f32 h = size * 0.42f;
        renderer_draw_line(r, tx - (ux+nx)*h, ty - (uy+ny)*h,
                              tx + (ux+nx)*h, ty + (uy+ny)*h, t, c);
        renderer_draw_line(r, tx - (ux-nx)*h, ty - (uy-ny)*h,
                              tx + (ux-nx)*h, ty + (uy-ny)*h, t, c);
        break;
    }
    case MK_TRIANGLE: {
        f32 l1x = bx + nx*size*0.55f, l1y = by + ny*size*0.55f;
        f32 l2x = bx - nx*size*0.55f, l2y = by - ny*size*0.55f;
        renderer_draw_line(r, tx, ty, l1x, l1y, t, c);
        renderer_draw_line(r, tx, ty, l2x, l2y, t, c);
        renderer_draw_line(r, l1x, l1y, l2x, l2y, t, c);
        break;
    }
    case MK_DIAMOND_F: case MK_DIAMOND_O: {
        f32 mx = tx - ux*size*0.5f, my = ty - uy*size*0.5f;
        f32 s1x = mx + nx*size*0.34f, s1y = my + ny*size*0.34f;
        f32 s2x = mx - nx*size*0.34f, s2y = my - ny*size*0.34f;
        f32 ex = tx - ux*size, ey = ty - uy*size;
        renderer_draw_line(r, tx, ty, s1x, s1y, t, c);
        renderer_draw_line(r, s1x, s1y, ex, ey, t, c);
        renderer_draw_line(r, tx, ty, s2x, s2y, t, c);
        renderer_draw_line(r, s2x, s2y, ex, ey, t, c);
        if (mk == MK_DIAMOND_F)                /* fill by over-stroking the axis */
            renderer_draw_line(r, tx, ty, ex, ey, size * 0.62f, c);
        break;
    }
    default: break;
    }
}

/* Outline of a node's real silhouette. The fill underneath is always an rrect
 * (the only filled primitive the renderer batches), so for the slanted shapes
 * the fill is inset to stay inside the outline the reader actually sees. */
static void dg_stroke_shape(Renderer *r, const DNode *n, f32 ox, f32 oy,
                            f32 t, Color c) {
    f32 x0 = ox + n->x, y0 = oy + n->y;
    f32 x1 = x0 + n->w, y1 = y0 + n->h;
    f32 mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    switch (n->shape) {
    case SH_DIAMOND:
        renderer_draw_line(r, mx, y0, x1, my, t, c);
        renderer_draw_line(r, x1, my, mx, y1, t, c);
        renderer_draw_line(r, mx, y1, x0, my, t, c);
        renderer_draw_line(r, x0, my, mx, y0, t, c);
        break;
    case SH_HEX: {
        f32 s = fminf(n->w * 0.2f, n->h * 0.5f);
        renderer_draw_line(r, x0, my, x0 + s, y0, t, c);
        renderer_draw_line(r, x0 + s, y0, x1 - s, y0, t, c);
        renderer_draw_line(r, x1 - s, y0, x1, my, t, c);
        renderer_draw_line(r, x1, my, x1 - s, y1, t, c);
        renderer_draw_line(r, x1 - s, y1, x0 + s, y1, t, c);
        renderer_draw_line(r, x0 + s, y1, x0, my, t, c);
        break;
    }
    case SH_PARA: {
        f32 s = fminf(n->w * 0.18f, n->h);
        renderer_draw_line(r, x0 + s, y0, x1, y0, t, c);
        renderer_draw_line(r, x1, y0, x1 - s, y1, t, c);
        renderer_draw_line(r, x1 - s, y1, x0, y1, t, c);
        renderer_draw_line(r, x0, y1, x0 + s, y0, t, c);
        break;
    }
    case SH_SUBROUTINE: {
        f32 s = fminf(g_cw * 0.7f, n->w * 0.2f);
        renderer_draw_line(r, x0, y0, x1, y0, t, c);
        renderer_draw_line(r, x0, y1, x1, y1, t, c);
        renderer_draw_line(r, x0, y0, x0, y1, t, c);
        renderer_draw_line(r, x1, y0, x1, y1, t, c);
        renderer_draw_line(r, x0 + s, y0, x0 + s, y1, t, c);
        renderer_draw_line(r, x1 - s, y0, x1 - s, y1, t, c);
        break;
    }
    case SH_CYLINDER: {
        f32 ry = fminf(n->h * 0.16f, g_ch * 0.5f);
        renderer_draw_line(r, x0, y0 + ry, x0, y1 - ry, t, c);
        renderer_draw_line(r, x1, y0 + ry, x1, y1 - ry, t, c);
        dg_arc(r, mx, y0 + ry, n->w * 0.5f, ry, 3.14159f, 6.28318f, 10, t, c);
        dg_arc(r, mx, y0 + ry, n->w * 0.5f, ry, 0.0f, 3.14159f, 10, t, c);
        dg_arc(r, mx, y1 - ry, n->w * 0.5f, ry, 0.0f, 3.14159f, 10, t, c);
        break;
    }
    case SH_CIRCLE: case SH_DOT: case SH_RING:
        dg_arc(r, mx, my, n->w * 0.5f, n->h * 0.5f, 0.0f, 6.28318f, 20, t, c);
        break;
    case SH_STADIUM: case SH_ROUND: {
        f32 rr = (n->shape == SH_STADIUM) ? n->h * 0.5f
                                          : fminf(n->h * 0.32f, n->w * 0.3f);
        renderer_draw_line(r, x0 + rr, y0, x1 - rr, y0, t, c);
        renderer_draw_line(r, x0 + rr, y1, x1 - rr, y1, t, c);
        renderer_draw_line(r, x0, y0 + rr, x0, y1 - rr, t, c);
        renderer_draw_line(r, x1, y0 + rr, x1, y1 - rr, t, c);
        dg_arc(r, x0 + rr, y0 + rr, rr, rr, 3.14159f, 4.71239f, 5, t, c);
        dg_arc(r, x1 - rr, y0 + rr, rr, rr, 4.71239f, 6.28318f, 5, t, c);
        dg_arc(r, x1 - rr, y1 - rr, rr, rr, 0.0f, 1.57080f, 5, t, c);
        dg_arc(r, x0 + rr, y1 - rr, rr, rr, 1.57080f, 3.14159f, 5, t, c);
        break;
    }
    default:
        renderer_draw_line(r, x0, y0, x1, y0, t, c);
        renderer_draw_line(r, x0, y1, x1, y1, t, c);
        renderer_draw_line(r, x0, y0, x0, y1, t, c);
        renderer_draw_line(r, x1, y0, x1, y1, t, c);
        break;
    }
}

/* Fill rect for a shape — inset where the silhouette is not a rectangle. */
static void dg_fill_rect(const DNode *n, f32 ox, f32 oy,
                         f32 *fx, f32 *fy, f32 *fw, f32 *fh) {
    f32 x = ox + n->x, y = oy + n->y, w = n->w, h = n->h;
    switch (n->shape) {
    case SH_DIAMOND:                       /* inscribed rect of the rhombus */
        x += w * 0.25f; y += h * 0.25f; w *= 0.5f; h *= 0.5f;
        break;
    case SH_HEX: { f32 s = fminf(w * 0.2f, h * 0.5f); x += s; w -= 2*s; break; }
    case SH_PARA: { f32 s = fminf(w * 0.18f, h); x += s; w -= 2*s; break; }
    default: break;
    }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    *fx = x; *fy = y; *fw = w; *fh = h;
}

static void dg_text(Renderer *r, const char *s, u32 n, f32 x, f32 y, Color c) {
    if (g_cw < DG_MIN_TEXT) return;      /* downscaled past legibility */
    u32 i = 0;
    f32 gx = x;
    while (i < n) {
        u32 cp = 0;
        u32 adv = utf8_decode((const u8 *)s + i, n - i, &cp);
        if (!adv) { adv = 1; cp = 0; }
        if (cp >= 32) renderer_push_glyph(r, gx, y, cp, c);
        gx += g_cw;
        i += adv;
    }
}

/* Centred multi-row label inside the box (x,y,w,h). */
static void dg_text_box(Renderer *r, const char *s, f32 x, f32 y, f32 w, f32 h,
                        Color c) {
    if (!s || !s[0] || g_cw < DG_MIN_TEXT) return;
    u32 rows, cells;
    dg_text_metrics(s, &rows, &cells);
    font_warm_text_glyphs(&r->font, s);
    f32 rh = g_ch * DG_ROW_H;
    f32 top = y + (h - (f32)rows * rh) * 0.5f;
    for (u32 k = 0; k < rows; k++) {
        const char *p; u32 n;
        if (!dg_row(s, k, &p, &n)) break;
        f32 tw = (f32)dg_cells(p, n) * g_cw;
        dg_text(r, p, n, x + (w - tw) * 0.5f, top + (f32)k * rh, c);
    }
}

/* ---- graph render ---- */
static void dg_render_graph(Renderer *r, f32 x, f32 y, Color fg, Color edgec,
                            Color accent, Color fill) {
    f32 t = fmaxf(1.0f, g_ch * 0.06f);
    Color faint = { edgec.r, edgec.g, edgec.b, edgec.a * 0.55f };

    /* subgraph boxes, behind everything */
    for (u32 g = 0; g < g_ngroups; g++) {
        DGroup *gp = &g_groups[g];
        if (!gp->used || gp->w <= 0 || gp->h <= 0) continue;
        renderer_draw_rrect_simple(r, x + gp->x, y + gp->y, gp->w, gp->h,
                                   (Color){fill.r, fill.g, fill.b, 0.45f},
                                   g_ch * 0.35f);
    }
    renderer_flush_rrects(r);
    for (u32 g = 0; g < g_ngroups; g++) {
        DGroup *gp = &g_groups[g];
        if (!gp->used || gp->w <= 0 || gp->h <= 0) continue;
        f32 x0 = x + gp->x, y0 = y + gp->y, x1 = x0 + gp->w, y1 = y0 + gp->h;
        renderer_draw_line(r, x0, y0, x1, y0, t, faint);
        renderer_draw_line(r, x0, y1, x1, y1, t, faint);
        renderer_draw_line(r, x0, y0, x0, y1, t, faint);
        renderer_draw_line(r, x1, y0, x1, y1, t, faint);
    }

    /* edges under the nodes */
    for (u32 e = 0; e < g_nedges; e++) {
        DEdge *ed = &g_edges[e];
        DNode *a = &g_nodes[ed->from], *b = &g_nodes[ed->to];
        f32 et = ed->style == ES_THICK ? t * 2.0f : t;
        f32 ah = g_ch * 0.5f;
        if (ed->from == ed->to) {                    /* self loop, right side */
            f32 lx = x + a->x + a->w, ly = y + a->y + a->h * 0.35f;
            f32 rx = lx + g_cw * 1.8f, by = y + a->y + a->h * 0.75f;
            renderer_draw_line(r, lx, ly, rx, ly, et, edgec);
            renderer_draw_line(r, rx, ly, rx, by, et, edgec);
            renderer_draw_line(r, rx, by, lx, by, et, edgec);
            dg_marker(r, lx, by, -1.0f, 0.0f, ed->mk_to, ah, et, edgec);
            continue;
        }
        f32 acx, acy, bcx, bcy;
        dg_center(a, x, y, &acx, &acy);
        dg_center(b, x, y, &bcx, &bcy);
        f32 p0x, p0y, p1x, p1y;
        dg_edge_point(a, x, y, bcx, bcy, &p0x, &p0y);
        dg_edge_point(b, x, y, acx, acy, &p1x, &p1y);
        if (ed->style == ES_DOTTED) dg_dashed(r, p0x, p0y, p1x, p1y, et, edgec);
        else renderer_draw_line(r, p0x, p0y, p1x, p1y, et, edgec);
        f32 dx = p1x - p0x, dy = p1y - p0y;
        f32 d = sqrtf(dx*dx + dy*dy);
        if (d > 0.5f) {
            f32 ux = dx/d, uy = dy/d;
            dg_marker(r, p1x, p1y, ux, uy, ed->mk_to, ah, et, edgec);
            dg_marker(r, p0x, p0y, -ux, -uy, ed->mk_from, ah, et, edgec);
        }
    }
    renderer_flush_lines(r);

    /* node fills */
    for (u32 i = 0; i < g_nnodes; i++) {
        DNode *n = &g_nodes[i];
        f32 fx, fy, fw, fh;
        dg_fill_rect(n, x, y, &fx, &fy, &fw, &fh);
        f32 radius = (n->shape == SH_CIRCLE || n->shape == SH_DOT ||
                      n->shape == SH_RING || n->shape == SH_STADIUM)
                        ? fminf(fw, fh) * 0.5f
                   : (n->shape == SH_ROUND) ? fminf(fh * 0.32f, fw * 0.3f)
                   : 3.0f;
        Color c = (n->shape == SH_DOT) ? fg : fill;
        renderer_draw_rrect_simple(r, fx, fy, fw, fh, c, radius);
    }
    renderer_flush_rrects(r);

    /* borders + class compartment dividers */
    for (u32 i = 0; i < g_nnodes; i++) {
        DNode *n = &g_nodes[i];
        if (n->shape == SH_DOT) continue;            /* filled disc, no stroke */
        Color bc = (n->shape == SH_DIAMOND || n->shape == SH_RING) ? accent : edgec;
        dg_stroke_shape(r, n, x, y, t, bc);
        if (n->shape == SH_CLASS && n->body[0]) {
            f32 dy = y + n->y + g_ch * 1.9f;
            renderer_draw_line(r, x + n->x, dy, x + n->x + n->w, dy, t, edgec);
        }
    }
    renderer_flush_lines(r);

    /* labels */
    renderer_set_ui_scale(r, g_cw, g_ch);
    for (u32 i = 0; i < g_nnodes; i++) {
        DNode *n = &g_nodes[i];
        if (n->shape == SH_CLASS) {
            dg_text_box(r, n->label, x + n->x, y + n->y, n->w, g_ch * 1.9f, fg);
            if (!n->body[0]) continue;
            u32 rows, cells;
            dg_text_metrics(n->body, &rows, &cells);
            font_warm_text_glyphs(&r->font, n->body);
            f32 ty = y + n->y + g_ch * 2.2f;
            for (u32 k = 0; k < rows; k++) {
                const char *p; u32 pn;
                if (!dg_row(n->body, k, &p, &pn)) break;
                dg_text(r, p, pn, x + n->x + g_cw, ty + (f32)k * g_ch * 1.2f, fg);
            }
            continue;
        }
        dg_text_box(r, n->label, x + n->x, y + n->y, n->w, n->h, fg);
    }
    /* subgraph titles, left-aligned in the title band */
    for (u32 g = 0; g < g_ngroups; g++) {
        DGroup *gp = &g_groups[g];
        const char *row; u32 rn;
        if (!gp->used || !gp->label[0] || gp->w <= 0) continue;
        if (!dg_row(gp->label, 0, &row, &rn) || !rn) continue;
        font_warm_text_glyphs(&r->font, gp->label);
        /* Every row, not just the first: the band above was sized for them. */
        for (u32 k = 0; dg_row(gp->label, k, &row, &rn); k++) {
            if (!rn) continue;
            dg_text(r, row, rn, x + gp->x + g_cw * 0.8f,
                    y + gp->y + g_ch * 0.45f + g_ch * 1.15f * (f32)k, accent);
        }
    }
    /* edge labels on a chip at the midpoint */
    for (u32 e = 0; e < g_nedges; e++) {
        DEdge *ed = &g_edges[e];
        if (!ed->label[0]) continue;
        f32 mx, my, bx, by, bw, bh;
        dg_edge_label_mid(ed, &mx, &my);
        dg_chip_box(ed->label, x + mx, y + my, &bx, &by, &bw, &bh);
        renderer_draw_rect(r, bx, by, bw, bh,
                           (Color){fill.r, fill.g, fill.b, 0.95f});
        dg_text_box(r, ed->label, bx, by, bw, bh, fg);
    }
}

/* Note box for `ms`, in screen space. One definition so the fill pass and the
 * text pass can never disagree about where the box is. */
static void dg_note_box(const DMsg *ms, f32 x, f32 y,
                        f32 *bx, f32 *by, f32 *bw, f32 *bh) {
    u32 rows, cells;
    dg_text_metrics(ms->label, &rows, &cells);
    (void)rows;
    f32 w = (f32)(cells + 2) * g_cw;
    f32 cx = x + g_parts[ms->to].cx;
    f32 nx = cx - w * 0.5f;
    if (ms->kind == SQ_NOTE_L) nx = cx - w - g_cw * 0.8f;
    else if (ms->kind == SQ_NOTE_R) nx = cx + g_cw * 0.8f;
    else if (ms->span >= 0 && (u32)ms->span < g_nparts) {
        f32 ox = x + g_parts[ms->span].cx;
        f32 mid = (cx + ox) * 0.5f;
        f32 span = fabsf(ox - cx) + g_cw * 2.0f;
        if (span > w) w = span;
        nx = mid - w * 0.5f;
    }
    *bx = nx;
    *by = y + ms->y + g_ch * 0.3f;
    *bw = w;
    *bh = ms->h - g_ch * 0.6f;
}

/* ---- sequence render ---- */
static void dg_render_seq(Renderer *r, f32 x, f32 y, Color fg, Color edgec,
                          Color accent, Color fill) {
    f32 t = fmaxf(1.0f, g_ch * 0.06f);
    f32 head_h = dg_head_h();
    Color faint = { edgec.r, edgec.g, edgec.b, edgec.a * 0.55f };

    /* lifelines */
    for (u32 i = 0; i < g_nparts; i++)
        dg_dashed(r, x + g_parts[i].cx, y + head_h,
                  x + g_parts[i].cx, y + g_h - g_ch * 0.4f, t, faint);

    /* messages */
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        if (ms->kind != SQ_MSG) continue;
        if (ms->from < 0 || (u32)ms->from >= g_nparts ||
            ms->to < 0 || (u32)ms->to >= g_nparts) continue;
        f32 ax = x + g_parts[ms->from].cx, bx = x + g_parts[ms->to].cx;
        f32 ly = y + ms->ly;
        f32 ah = g_ch * 0.5f;
        if (ms->from == ms->to) {
            f32 rx = ax + g_cw * 3.0f;       /* matches dg_layout_seq's need_r */
            f32 y1 = ly + g_ch * 1.1f;
            if (ms->style == ES_DOTTED) {
                dg_dashed(r, ax, ly, rx, ly, t, edgec);
                dg_dashed(r, rx, ly, rx, y1, t, edgec);
                dg_dashed(r, rx, y1, ax, y1, t, edgec);
            } else {
                renderer_draw_line(r, ax, ly, rx, ly, t, edgec);
                renderer_draw_line(r, rx, ly, rx, y1, t, edgec);
                renderer_draw_line(r, rx, y1, ax, y1, t, edgec);
            }
            dg_marker(r, ax, y1, -1.0f, 0.0f, ms->mk_to, ah, t, edgec);
            continue;
        }
        f32 dir = bx > ax ? 1.0f : -1.0f;
        f32 x0 = ax + dir * g_cw * 0.2f, x1 = bx - dir * g_cw * 0.2f;
        if (ms->style == ES_DOTTED) dg_dashed(r, x0, ly, x1, ly, t, edgec);
        else renderer_draw_line(r, x0, ly, x1, ly, t, edgec);
        dg_marker(r, x1, ly, dir, 0.0f, ms->mk_to, ah, t, edgec);
        dg_marker(r, x0, ly, -dir, 0.0f, ms->mk_from, ah, t, edgec);
    }
    renderer_flush_lines(r);

    /* participant heads, activation bars and note boxes */
    for (u32 i = 0; i < g_nparts; i++) {
        f32 w = g_parts[i].w;
        renderer_draw_rrect_simple(r, x + g_parts[i].cx - w * 0.5f, y, w,
                                   head_h - g_ch * 0.25f, fill,
                                   g_parts[i].actor ? head_h * 0.4f : 3.0f);
    }
    for (u32 b = 0; b < g_nbars; b++) {
        DBar *ba = &g_bars[b];
        if (ba->part < 0 || (u32)ba->part >= g_nparts) continue;
        f32 bw = g_cw * 0.8f;
        f32 bx = x + g_parts[ba->part].cx - bw * 0.5f + (f32)ba->depth * bw * 0.5f;
        f32 hh = ba->y1 - ba->y0;
        if (hh < g_ch * 0.4f) hh = g_ch * 0.4f;
        renderer_draw_rrect_simple(r, bx, y + ba->y0, bw, hh,
                                   (Color){accent.r, accent.g, accent.b, 0.55f}, 2.0f);
    }
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        if (ms->kind != SQ_NOTE_L && ms->kind != SQ_NOTE_R &&
            ms->kind != SQ_NOTE_OVER) continue;
        if (ms->to < 0 || (u32)ms->to >= g_nparts) continue;
        f32 bx, by, bw, bh;
        dg_note_box(ms, x, y, &bx, &by, &bw, &bh);
        renderer_draw_rrect_simple(r, bx, by, bw, bh, fill, g_ch * 0.25f);
    }
    renderer_flush_rrects(r);

    /* head + note borders */
    for (u32 i = 0; i < g_nparts; i++) {
        f32 w = g_parts[i].w;
        f32 x0 = x + g_parts[i].cx - w * 0.5f, y0 = y;
        f32 x1 = x0 + w, y1 = y + head_h - g_ch * 0.25f;
        renderer_draw_line(r, x0, y0, x1, y0, t, edgec);
        renderer_draw_line(r, x0, y1, x1, y1, t, edgec);
        renderer_draw_line(r, x0, y0, x0, y1, t, edgec);
        renderer_draw_line(r, x1, y0, x1, y1, t, edgec);
    }
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        if (ms->kind != SQ_NOTE_L && ms->kind != SQ_NOTE_R &&
            ms->kind != SQ_NOTE_OVER) continue;
        if (ms->to < 0 || (u32)ms->to >= g_nparts) continue;
        f32 bx, by, bw, bh;
        dg_note_box(ms, x, y, &bx, &by, &bw, &bh);
        renderer_draw_line(r, bx, by, bx + bw, by, t, faint);
        renderer_draw_line(r, bx, by + bh, bx + bw, by + bh, t, faint);
        renderer_draw_line(r, bx, by, bx, by + bh, t, faint);
        renderer_draw_line(r, bx + bw, by, bx + bw, by + bh, t, faint);
    }
    renderer_flush_lines(r);

    /* text */
    renderer_set_ui_scale(r, g_cw, g_ch);
    for (u32 i = 0; i < g_nparts; i++)
        dg_text_box(r, g_parts[i].label, x + g_parts[i].cx - g_parts[i].w * 0.5f,
                    y, g_parts[i].w, head_h - g_ch * 0.25f, fg);
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        if (!ms->label[0]) continue;
        if (ms->kind == SQ_MSG) {
            if (ms->from < 0 || (u32)ms->from >= g_nparts ||
                ms->to < 0 || (u32)ms->to >= g_nparts) continue;
            u32 rows, cells;
            dg_text_metrics(ms->label, &rows, &cells);
            f32 tw = (f32)cells * g_cw;
            f32 ax = x + g_parts[ms->from].cx, bx = x + g_parts[ms->to].cx;
            /* Centre the label over the segment; the layout reserved the span
             * for exactly this width. */
            f32 bx0 = (ms->from == ms->to) ? ax + g_cw * 3.5f
                                           : fminf(ax, bx);
            f32 bw = (ms->from == ms->to) ? tw : fabsf(bx - ax);
            dg_text_box(r, ms->label, bx0, y + ms->y, bw, ms->ly - ms->y, fg);
        } else {
            if (ms->to < 0 || (u32)ms->to >= g_nparts) continue;
            f32 bx, by, bw, bh;
            dg_note_box(ms, x, y, &bx, &by, &bw, &bh);
            dg_text_box(r, ms->label, bx, by, bw, bh, fg);
        }
    }
}

void md_diagram_render(Renderer *r, const Theme *theme, const u8 *src, u32 len,
                       f32 x, f32 y, f32 avail_w, f32 cw, f32 ch) {
    if (!r || !src || !len || !isfinite(x) || !isfinite(y) ||
        !isfinite(avail_w) || !isfinite(cw) || !isfinite(ch) ||
        cw <= 0.0f || ch <= 0.0f || cw > 16384.0f || ch > 16384.0f)
        return;
    MdDiagramKind kind = dg_prepare(src, len, avail_w, cw, ch);
    if (kind == MD_DIAGRAM_NONE) return;

    Color fg     = theme ? theme->fg : (Color){0.86f,0.86f,0.90f,1.0f};
    Color edgec  = { fg.r*0.7f, fg.g*0.7f, fg.b*0.7f, fg.a };
    Color accent = theme ? theme->ansi[12] : (Color){0.45f,0.62f,1.0f,1.0f};
    Color fill   = theme ? theme->bg : (Color){0.12f,0.12f,0.14f,1.0f};
    fill.r = fminf(1.0f, fill.r + 0.07f);
    fill.g = fminf(1.0f, fill.g + 0.07f);
    fill.b = fminf(1.0f, fill.b + 0.07f);

    if (kind == MD_DIAGRAM_SEQUENCE) dg_render_seq(r, x, y, fg, edgec, accent, fill);
    else                             dg_render_graph(r, x, y, fg, edgec, accent, fill);

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    /* The glyph cell size is sticky renderer state, and the labels above were
     * drawn on the diagram's own cell — which dg_fit may have shrunk. Put the
     * caller's box back, or every block after this one in the document keeps
     * rendering its text at the diagram's scale. */
    renderer_set_ui_scale(r, cw, ch);
}

void md_diagram_measure(const u8 *src, u32 len, f32 avail_w, f32 cw, f32 ch,
                        f32 *out_w, f32 *out_h) {
    f32 w = 0, h = 0;
    if (src && len && isfinite(avail_w) && isfinite(cw) && isfinite(ch) &&
        cw > 0.0f && ch > 0.0f && cw <= 16384.0f && ch <= 16384.0f) {
        if (dg_prepare(src, len, avail_w, cw, ch) != MD_DIAGRAM_NONE) {
            w = g_w; h = g_h;
        }
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

MdDiagramKind md_diagram_probe(const u8 *src, u32 len) {
    return dg_build(src, len);
}
