/*
 * Liu — Graphviz DOT and PlantUML, read as the Mermaid the engine draws.
 *
 * Neither language gets a layout of its own: each is translated into the
 * Mermaid source of the same diagram, and the engine's parsers and layouts
 * draw it. DOT becomes a flowchart (its clusters subgraphs, its rankdir the
 * direction, its shapes the nearest Mermaid ones). PlantUML's sequence,
 * class and state diagrams map almost word for word; an activity diagram is
 * walked as the structure it is (if / else / endif, while, repeat, fork) into
 * a flowchart; anything else with links between named things becomes a graph.
 *
 * What a translation cannot represent it leaves out (colours, skin
 * parameters, notes on classes, swimlanes); what it cannot read makes the
 * whole source fail, so the reader sees their text and not a wrong picture.
 * Every buffer is bounded and a translation that would not fit fails.
 */
#include "ui/markdown/md_diagram_int.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the output ---- */

typedef struct { char *s; u32 cap, len; bool full; } Out;

static void put(Out *o, const char *s, u32 n) {
    if (o->full) return;
    if (o->len + n + 1 > o->cap) { o->full = true; return; }
    memcpy(o->s + o->len, s, n);
    o->len += n;
    o->s[o->len] = 0;
}
static void puts_(Out *o, const char *s) { put(o, s, (u32)strlen(s)); }
static void putf(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void putf(Out *o, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((u32)n >= sizeof buf) { o->full = true; return; }
    put(o, buf, (u32)n);
}

/* A label inside Mermaid's `"…"`: a quote becomes an entity, a line break
 * `<br/>`, control bytes a space. */
static void put_label(Out *o, const u8 *s, u32 n) {
    for (u32 i = 0; i < n && !o->full; i++) {
        u8 c = s[i];
        if (c == '"') puts_(o, "#quot;");
        else if (c == '\n') puts_(o, "<br/>");
        else if (c == '\\' && i + 1 < n && (s[i+1] == 'n' || s[i+1] == 'l' || s[i+1] == 'r')) { puts_(o, "<br/>"); i++; }
        else if (c == '\\' && i + 1 < n && s[i+1] == '"') { puts_(o, "#quot;"); i++; }
        else if (c < 0x20) put(o, " ", 1);
        else put(o, (const char *)&s[i], 1);
    }
}

static bool is_word(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

static void trim(const u8 **s, u32 *n) {
    while (*n && (dg_is_space((*s)[0]) || (*s)[0] == '\r')) { (*s)++; (*n)--; }
    while (*n && (dg_is_space((*s)[*n - 1]) || (*s)[*n - 1] == '\r')) (*n)--;
}

static bool starts_word(const u8 *s, u32 n, const char *kw) {
    u32 k = (u32)strlen(kw);
    return n >= k && !memcmp(s, kw, k) && (n == k || !is_word(s[k]));
}

/* ---- names: anything a source calls a thing becomes `n<index>` ---- */

#define NAME_CAP 160

typedef struct { char key[DG_ID_CAP]; char label[DG_LABEL_CAP]; u8 shape; i32 cluster; bool shown; } Name;

static Name g_names[NAME_CAP];
static u32  g_nnames;
static bool g_overflow;

static i32 name_of(const u8 *s, u32 n) {
    trim(&s, &n);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }
    if (!n) return -1;
    if (n >= DG_ID_CAP) n = DG_ID_CAP - 1;
    for (u32 i = 0; i < g_nnames; i++)
        if (strlen(g_names[i].key) == n && !memcmp(g_names[i].key, s, n)) return (i32)i;
    if (g_nnames >= NAME_CAP) { g_overflow = true; return -1; }
    Name *m = &g_names[g_nnames];
    memset(m, 0, sizeof *m);
    memcpy(m->key, s, n);
    m->key[n] = 0;
    memcpy(m->label, s, n);
    m->label[n] = 0;
    m->cluster = -1;
    return (i32)g_nnames++;
}

/* A node's declaration in Mermaid: `n3["label"]` in its shape. */
enum { SHP_RECT = 0, SHP_ROUND, SHP_STADIUM, SHP_CIRCLE, SHP_DIAMOND, SHP_CYL, SHP_HEX, SHP_PARA, SHP_START, SHP_STOP };

static void put_node(Out *o, i32 id) {
    const Name *m = &g_names[id];
    static const char *open[]  = { "[\"", "(\"", "([\"", "((\"", "{\"", "[(\"", "{{\"", "[/\"", "((\"", "((\"" };
    static const char *close[] = { "\"]", "\")", "\"])", "\"))", "\"}", "\")]", "\"}}", "\"/]", "\"))", "\"))" };
    u8 sh = m->shape <= SHP_STOP ? m->shape : SHP_RECT;
    putf(o, "n%d%s", id, open[sh]);
    put_label(o, (const u8 *)m->label, (u32)strlen(m->label));
    puts_(o, close[sh]);
}

/* =========================================================================
 * Graphviz DOT
 * ========================================================================= */

typedef enum { T_END = 0, T_ID, T_LBRACE, T_RBRACE, T_LBRACK, T_RBRACK, T_SEMI, T_COMMA, T_EQ, T_COLON,
               T_EDGE, T_UEDGE } TokKind;

typedef struct { TokKind k; const u8 *s; u32 n; } Tok;

typedef struct { const u8 *s; u32 n, i; Tok cur; } Lex;

static void lex_next(Lex *x) {
    const u8 *s = x->s;
    u32 n = x->n, i = x->i;
    for (;;) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i + 1 < n && s[i] == '/' && s[i+1] == '/') { while (i < n && s[i] != '\n') i++; continue; }
        if (i + 1 < n && s[i] == '/' && s[i+1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i+1] == '/')) i++;
            i = i + 2 <= n ? i + 2 : n;
            continue;
        }
        if (i < n && s[i] == '#' && (i == 0 || s[i-1] == '\n')) { while (i < n && s[i] != '\n') i++; continue; }
        break;
    }
    Tok t = { T_END, s + i, 0 };
    if (i >= n) { x->cur = t; x->i = i; return; }
    u8 c = s[i];
    if (c == '{') { t.k = T_LBRACE; i++; }
    else if (c == '}') { t.k = T_RBRACE; i++; }
    else if (c == '[') { t.k = T_LBRACK; i++; }
    else if (c == ']') { t.k = T_RBRACK; i++; }
    else if (c == ';') { t.k = T_SEMI; i++; }
    else if (c == ',') { t.k = T_COMMA; i++; }
    else if (c == '=') { t.k = T_EQ; i++; }
    else if (c == ':') { t.k = T_COLON; i++; }
    else if (c == '-' && i + 1 < n && s[i+1] == '>') { t.k = T_EDGE; i += 2; }
    else if (c == '-' && i + 1 < n && s[i+1] == '-') { t.k = T_UEDGE; i += 2; }
    else if (c == '"') {
        u32 e = i + 1;
        while (e < n && s[e] != '"') { if (s[e] == '\\' && e + 1 < n) e++; e++; }
        t.k = T_ID; t.s = s + i + 1; t.n = (e < n ? e : n) - i - 1;
        i = e < n ? e + 1 : n;
    } else if (c == '<') {
        /* an HTML label: its text with the tags dropped happens at use */
        u32 e = i, depth = 0;
        do {
            if (s[e] == '<') depth++;
            else if (s[e] == '>') depth--;
            e++;
        } while (e < n && depth > 0);
        t.k = T_ID; t.s = s + i; t.n = e - i;
        i = e;
    } else if (is_word(c) || c == '.' || c == '-') {
        u32 e = i + 1;
        while (e < n && (is_word(s[e]) || s[e] == '.')) e++;
        t.k = T_ID; t.n = e - i;
        i = e;
    } else {
        t.k = T_ID; t.n = 1; i++;          /* a stray byte: an id of its own, ignored later */
    }
    x->cur = t;
    x->i = i;
}

/* An HTML label's text: tags dropped, <br/> a line break. */
static void html_text(const u8 *s, u32 n, char *out, u32 cap) {
    u32 o = 0;
    for (u32 i = 0; i < n && o + 2 < cap; i++) {
        if (s[i] == '<') {
            u32 e = i;
            while (e < n && s[e] != '>') e++;
            if (e - i >= 3 && (s[i+1] == 'b' || s[i+1] == 'B') && (s[i+2] == 'r' || s[i+2] == 'R')) out[o++] = '\n';
            i = e;
            continue;
        }
        out[o++] = (char)s[i];
    }
    out[o] = 0;
}

typedef struct { char label[DG_ELABEL_CAP]; bool dashed, bold, both, none, back; } EdgeAttr;
typedef struct { char label[DG_LABEL_CAP]; u8 shape; bool has_label, has_shape; } NodeAttr;

static u8 dot_shape(const u8 *s, u32 n) {
    static const struct { const char *name; u8 shape; } map[] = {
        { "box", SHP_RECT }, { "rect", SHP_RECT }, { "rectangle", SHP_RECT }, { "square", SHP_RECT },
        { "record", SHP_RECT }, { "plaintext", SHP_RECT }, { "plain", SHP_RECT }, { "none", SHP_RECT },
        { "note", SHP_RECT }, { "tab", SHP_RECT }, { "folder", SHP_RECT }, { "component", SHP_RECT },
        { "Mrecord", SHP_ROUND }, { "ellipse", SHP_STADIUM }, { "oval", SHP_STADIUM },
        { "circle", SHP_CIRCLE }, { "doublecircle", SHP_CIRCLE }, { "point", SHP_CIRCLE },
        { "diamond", SHP_DIAMOND }, { "Mdiamond", SHP_DIAMOND }, { "cylinder", SHP_CYL },
        { "hexagon", SHP_HEX }, { "octagon", SHP_HEX }, { "parallelogram", SHP_PARA }, { NULL, 0 }
    };
    for (u32 k = 0; map[k].name; k++)
        if (strlen(map[k].name) == n && !memcmp(map[k].name, s, n)) return map[k].shape;
    return SHP_STADIUM;
}

static void tok_text(const Tok *t, char *out, u32 cap) {
    if (t->n && t->s[0] == '<') { html_text(t->s, t->n, out, cap); return; }
    u32 n = t->n < cap - 1 ? t->n : cap - 1;
    memcpy(out, t->s, n);
    out[n] = 0;
}

/* `[a=b, c="d"]`, after the `[`: the attributes a node or an edge reads. */
static void dot_attrs(Lex *x, NodeAttr *na, EdgeAttr *ea, char *rankdir) {
    while (x->cur.k != T_END && x->cur.k != T_RBRACK) {
        if (x->cur.k != T_ID) { lex_next(x); continue; }
        Tok key = x->cur;
        lex_next(x);
        if (x->cur.k != T_EQ) continue;
        lex_next(x);
        Tok val = x->cur;
        lex_next(x);
        char v[DG_LABEL_CAP];
        tok_text(&val, v, sizeof v);
        u32 vl = (u32)strlen(v);
#define KEY(k) (key.n == strlen(k) && !memcmp(key.s, k, key.n))
        if (KEY("label") || (ea && KEY("xlabel"))) {
            if (na) { snprintf(na->label, sizeof na->label, "%s", v); na->has_label = true; }
            if (ea) snprintf(ea->label, sizeof ea->label, "%s", v);
        } else if (KEY("shape") && na) { na->shape = dot_shape((const u8 *)v, vl); na->has_shape = true; }
        else if (KEY("style") && ea) { ea->dashed = strstr(v, "dash") || strstr(v, "dot"); ea->bold = strstr(v, "bold") != NULL; }
        else if (KEY("penwidth") && ea) ea->bold = atof(v) >= 2.5;
        else if (KEY("dir") && ea) { ea->both = !strcmp(v, "both"); ea->none = !strcmp(v, "none"); ea->back = !strcmp(v, "back"); }
        else if (KEY("arrowhead") && ea) ea->none = !strcmp(v, "none") || ea->none;
        else if (KEY("rankdir") && rankdir) snprintf(rankdir, 4, "%s", v);
#undef KEY
    }
    if (x->cur.k == T_RBRACK) lex_next(x);
}

typedef struct {
    i32 a[32], na;            /* the nodes on one side of an edge (a `{ a b }` set) */
} Side;

typedef struct { i32 from, to; EdgeAttr at; } DotEdge;

#define DOT_EDGES 256
#define DOT_CLUSTERS 16

static DotEdge g_dedges[DOT_EDGES];
static u32     g_ndedges;
static char    g_clabel[DOT_CLUSTERS][DG_LABEL_CAP];
static i32     g_cparent[DOT_CLUSTERS];
static u32     g_nclusters;
static bool    g_directed;
static NodeAttr g_node_default;
static EdgeAttr g_edge_default;
static char    g_rankdir[4];

static void dot_stmts(Lex *x, i32 cluster, Side *made);

/* A node id with its port, or a `{ … }` / `subgraph` set, into `side`. */
static bool dot_side(Lex *x, i32 cluster, Side *side) {
    side->na = 0;
    if (x->cur.k == T_LBRACE || (x->cur.k == T_ID && x->cur.n == 8 && !memcmp(x->cur.s, "subgraph", 8))) {
        dot_stmts(x, cluster, side);
        return true;
    }
    if (x->cur.k != T_ID) return false;
    char key[DG_ID_CAP];
    tok_text(&x->cur, key, sizeof key);
    i32 id = name_of((const u8 *)key, (u32)strlen(key));
    lex_next(x);
    while (x->cur.k == T_COLON) { lex_next(x); if (x->cur.k == T_ID) lex_next(x); }   /* :port:compass */
    if (id < 0) return false;
    if (g_names[id].cluster < 0) g_names[id].cluster = cluster;
    if (!g_names[id].shown) {
        g_names[id].shown = true;
        g_names[id].shape = g_node_default.has_shape ? g_node_default.shape : SHP_STADIUM;
    }
    side->a[side->na++] = id;
    return true;
}

/* Statements up to the closing brace; `made` collects every node named in a
 * braced set, for an edge to or from it. */
static void dot_stmts(Lex *x, i32 cluster, Side *made) {
    i32 here = cluster;
    if (x->cur.k == T_ID && x->cur.n == 8 && !memcmp(x->cur.s, "subgraph", 8)) {
        lex_next(x);
        bool is_cluster = false;
        char name[DG_ID_CAP] = "";
        if (x->cur.k == T_ID) {
            tok_text(&x->cur, name, sizeof name);
            is_cluster = !strncmp(name, "cluster", 7);
            lex_next(x);
        }
        if (is_cluster && g_nclusters < DOT_CLUSTERS) {
            here = (i32)g_nclusters++;
            snprintf(g_clabel[here], DG_LABEL_CAP, "%s", name + 7 + (name[7] == '_'));
            g_cparent[here] = cluster;
        }
    }
    if (x->cur.k != T_LBRACE) return;
    lex_next(x);
    NodeAttr saved_node = g_node_default;
    EdgeAttr saved_edge = g_edge_default;
    while (x->cur.k != T_END && x->cur.k != T_RBRACE) {
        if (x->cur.k == T_SEMI || x->cur.k == T_COMMA) { lex_next(x); continue; }
        if (x->cur.k == T_ID) {
            Tok t = x->cur;
            bool g = t.n == 5 && !memcmp(t.s, "graph", 5), nd = t.n == 4 && !memcmp(t.s, "node", 4),
                 ed = t.n == 4 && !memcmp(t.s, "edge", 4);
            if (g || nd || ed) {
                lex_next(x);
                if (x->cur.k == T_LBRACK) {
                    lex_next(x);
                    NodeAttr na = g_node_default; EdgeAttr ea = g_edge_default;
                    na.has_label = false;
                    dot_attrs(x, g ? &na : nd ? &na : NULL, ed ? &ea : NULL, g_rankdir);
                    if (g && na.has_label && here >= 0) snprintf(g_clabel[here], DG_LABEL_CAP, "%s", na.label);
                    if (nd) { na.has_label = false; g_node_default = na; }
                    if (ed) g_edge_default = ea;
                }
                continue;
            }
            /* `label = "x"` / `rankdir = LR` in a graph's body */
            Lex peek = *x;
            lex_next(&peek);
            if (peek.cur.k == T_EQ) {
                lex_next(&peek);
                char v[DG_LABEL_CAP];
                tok_text(&peek.cur, v, sizeof v);
                if (t.n == 5 && !memcmp(t.s, "label", 5) && here >= 0) snprintf(g_clabel[here], DG_LABEL_CAP, "%s", v);
                if (t.n == 7 && !memcmp(t.s, "rankdir", 7)) snprintf(g_rankdir, sizeof g_rankdir, "%s", v);
                lex_next(&peek);
                *x = peek;
                continue;
            }
        }
        Side a;
        if (!dot_side(x, here, &a)) { lex_next(x); continue; }
        for (i32 k = 0; k < a.na && made && made->na < 32; k++) made->a[made->na++] = a.a[k];
        if (x->cur.k != T_EDGE && x->cur.k != T_UEDGE) {
            /* a node statement: its own attributes */
            if (x->cur.k == T_LBRACK && a.na == 1) {
                lex_next(x);
                NodeAttr na = { "", g_names[a.a[0]].shape, false, false };
                dot_attrs(x, &na, NULL, NULL);
                if (na.has_label) snprintf(g_names[a.a[0]].label, DG_LABEL_CAP, "%s", na.label);
                if (na.has_shape) g_names[a.a[0]].shape = na.shape;
            }
            continue;
        }
        /* an edge chain: a -> b -> c [attrs] */
        Side chain[16];
        chain[0] = a;
        u32 nc = 1;
        while ((x->cur.k == T_EDGE || x->cur.k == T_UEDGE) && nc < 16) {
            lex_next(x);
            if (!dot_side(x, here, &chain[nc])) break;
            for (i32 k = 0; k < chain[nc].na && made && made->na < 32; k++) made->a[made->na++] = chain[nc].a[k];
            nc++;
        }
        EdgeAttr ea = g_edge_default;
        ea.label[0] = 0;
        if (x->cur.k == T_LBRACK) { lex_next(x); dot_attrs(x, NULL, &ea, NULL); }
        for (u32 c = 0; c + 1 < nc; c++)
            for (i32 i = 0; i < chain[c].na; i++)
                for (i32 j = 0; j < chain[c + 1].na; j++) {
                    if (g_ndedges >= DOT_EDGES) { g_overflow = true; break; }
                    g_dedges[g_ndedges++] = (DotEdge){ chain[c].a[i], chain[c + 1].a[j], ea };
                }
    }
    if (x->cur.k == T_RBRACE) lex_next(x);
    g_node_default = saved_node;
    g_edge_default = saved_edge;
}

static void dot_cluster(Out *o, i32 c) {
    putf(o, "subgraph c%d [\"", c);
    put_label(o, (const u8 *)g_clabel[c], (u32)strlen(g_clabel[c]));
    puts_(o, "\"]\n");
    for (u32 k = 0; k < g_nclusters; k++) if (g_cparent[k] == c) dot_cluster(o, (i32)k);
    for (u32 i = 0; i < g_nnames; i++)
        if (g_names[i].cluster == c) { put_node(o, (i32)i); puts_(o, "\n"); }
    puts_(o, "end\n");
}

static bool dot_translate(const u8 *src, u32 len, Out *o) {
    Lex x = { src, len, 0, { T_END, NULL, 0 } };
    lex_next(&x);
    if (x.cur.k == T_ID && x.cur.n == 6 && !memcmp(x.cur.s, "strict", 6)) lex_next(&x);
    if (x.cur.k != T_ID) return false;
    g_directed = x.cur.n == 7 && !memcmp(x.cur.s, "digraph", 7);
    if (!g_directed && !(x.cur.n == 5 && !memcmp(x.cur.s, "graph", 5))) return false;
    lex_next(&x);
    if (x.cur.k == T_ID) lex_next(&x);              /* the graph's name */
    if (x.cur.k != T_LBRACE) return false;
    dot_stmts(&x, -1, NULL);
    if (g_overflow || !g_nnames) return false;
    const char *dir = !strcmp(g_rankdir, "LR") ? "LR" : !strcmp(g_rankdir, "RL") ? "RL" :
                      !strcmp(g_rankdir, "BT") ? "BT" : "TD";
    putf(o, "flowchart %s\n", dir);
    for (u32 c = 0; c < g_nclusters; c++) if (g_cparent[c] < 0) dot_cluster(o, (i32)c);
    for (u32 i = 0; i < g_nnames; i++) if (g_names[i].cluster < 0) { put_node(o, (i32)i); puts_(o, "\n"); }
    for (u32 e = 0; e < g_ndedges; e++) {
        const DotEdge *d = &g_dedges[e];
        i32 a = d->at.back ? d->to : d->from, b = d->at.back ? d->from : d->to;
        bool head = g_directed && !d->at.none;
        bool tail = g_directed && d->at.both;
        const char *op = d->at.bold ? (head ? "==>" : "===") : d->at.dashed ? (head ? "-.->" : "-.-")
                       : (head ? "-->" : "---");
        putf(o, "n%d %s%s", a, tail ? "<" : "", op);
        if (d->at.label[0]) {
            puts_(o, "|\"");
            put_label(o, (const u8 *)d->at.label, (u32)strlen(d->at.label));
            puts_(o, "\"|");
        }
        putf(o, " n%d\n", b);
    }
    return !o->full;
}

/* =========================================================================
 * PlantUML
 * ========================================================================= */

/* The source's lines without the wrapper and the presentation statements;
 * multi-line `skinparam {…}` and `legend…endlegend` blocks dropped. */
typedef struct { const u8 *s; u32 n; } Line;

#define PU_LINES 1024

static Line g_pl[PU_LINES];
static u32  g_npl;
static bool g_pu_lr;

static bool pu_lines(const u8 *src, u32 len) {
    g_npl = 0;
    g_pu_lr = false;
    u32 i = 0;
    bool in_skin = false, in_legend = false, started = false;
    while (i < len) {
        u32 e = i;
        while (e < len && src[e] != '\n') e++;
        const u8 *s = src + i; u32 n = e - i;
        i = e + 1;
        trim(&s, &n);
        if (!n || s[0] == '\'') continue;
        if (starts_word(s, n, "@startuml")) { started = true; continue; }
        if (starts_word(s, n, "@enduml")) break;
        if (!started) return false;
        if (in_skin) { if (s[0] == '}') in_skin = false; continue; }
        if (in_legend) { if (starts_word(s, n, "endlegend") || starts_word(s, n, "end legend")) in_legend = false; continue; }
        if (starts_word(s, n, "skinparam")) { if (s[n - 1] == '{') in_skin = true; continue; }
        if (starts_word(s, n, "legend")) { in_legend = true; continue; }
        if (starts_word(s, n, "left to right direction")) { g_pu_lr = true; continue; }
        static const char *skip[] = { "top to bottom direction", "!theme", "!include", "!define", "!pragma",
                                      "hide", "show", "scale", "caption", "header", "footer", "newpage",
                                      "autoactivate", "skin", "allowmixing", "set", NULL };
        bool skipped = false;
        for (u32 k = 0; skip[k] && !skipped; k++) skipped = starts_word(s, n, skip[k]);
        if (skipped) continue;
        if (g_npl >= PU_LINES) return false;
        g_pl[g_npl++] = (Line){ s, n };
    }
    return started && g_npl > 0;
}

static bool has(const u8 *s, u32 n, const char *needle) {
    u32 k = (u32)strlen(needle);
    for (u32 i = 0; i + k <= n; i++) if (!memcmp(s + i, needle, k)) return true;
    return false;
}

typedef enum { PU_SEQ, PU_CLASS, PU_STATE, PU_ACTIVITY, PU_GRAPH } PuKind;

static PuKind pu_kind(void) {
    u32 seq = 0, cls = 0, st = 0, act = 0, grf = g_pu_lr ? 3u : 0u;
    for (u32 i = 0; i < g_npl; i++) {
        const u8 *s = g_pl[i].s; u32 n = g_pl[i].n;
        if (s[0] == ':' && (s[n - 1] == ';' || s[n - 1] == '|' || s[n - 1] == '<' || s[n - 1] == '>' ||
                            s[n - 1] == '/' || s[n - 1] == ']' || s[n - 1] == '}')) act += 2;
        if (starts_word(s, n, "start") || starts_word(s, n, "stop") || starts_word(s, n, "endif") ||
            (starts_word(s, n, "if") && has(s, n, "then")) || starts_word(s, n, "endwhile") ||
            starts_word(s, n, "fork") || starts_word(s, n, "repeat")) act += 2;
        if (starts_word(s, n, "participant") || starts_word(s, n, "actor") || starts_word(s, n, "boundary") ||
            starts_word(s, n, "control") || starts_word(s, n, "entity") || starts_word(s, n, "database") ||
            starts_word(s, n, "collections") || starts_word(s, n, "queue") || starts_word(s, n, "activate") ||
            starts_word(s, n, "autonumber")) seq += 2;
        if ((has(s, n, "->") || has(s, n, "-->")) && has(s, n, ":") && !has(s, n, "[*]")) seq++;
        if (starts_word(s, n, "class") || starts_word(s, n, "interface") || starts_word(s, n, "abstract") ||
            starts_word(s, n, "enum") || has(s, n, "<|--") || has(s, n, "--|>") || has(s, n, "*--") ||
            has(s, n, "o--") || has(s, n, "..|>")) cls += 3;
        if (has(s, n, "[*]") || starts_word(s, n, "state")) st += 3;
        if (starts_word(s, n, "rectangle") || starts_word(s, n, "package") || starts_word(s, n, "node") ||
            starts_word(s, n, "component") || starts_word(s, n, "usecase") || starts_word(s, n, "cloud") ||
            starts_word(s, n, "frame") || starts_word(s, n, "folder") || starts_word(s, n, "artifact") ||
            starts_word(s, n, "storage")) grf += 3;
        if (s[0] == '(' || (s[0] == '[' && has(s, n, "]"))) grf += 2;
    }
    if (act && act >= seq && act >= cls && act >= st) return PU_ACTIVITY;
    if (cls && cls >= seq && cls >= st) return PU_CLASS;
    if (st && st >= seq) return PU_STATE;
    if (seq && seq > grf) return PU_SEQ;
    return PU_GRAPH;
}

/* ---- sequence ---- */

static i32 pu_participant(const u8 *s, u32 n) {
    trim(&s, &n);
    while (n && (s[n - 1] == '+' || s[n - 1] == '-' || s[n - 1] == '*' || s[n - 1] == '!')) n--;   /* activation marks */
    if (n && (s[0] == '+' || s[0] == '-')) { s++; n--; }
    return name_of(s, n);
}

/* A participant as Mermaid names it: its index, labelled with its name. */
static void pu_seq_declare(Out *o, i32 id, bool actor) {
    if (id < 0 || g_names[id].shown) return;
    g_names[id].shown = true;
    putf(o, "%s p%d as ", actor ? "actor" : "participant", id);
    put(o, g_names[id].label, (u32)strlen(g_names[id].label));
    puts_(o, "\n");
}

static bool pu_sequence(Out *o) {
    puts_(o, "sequenceDiagram\n");
    i32 last_from = -1, last_to = -1;
    for (u32 li = 0; li < g_npl && !o->full; li++) {
        const u8 *s = g_pl[li].s; u32 n = g_pl[li].n;
        static const char *decl[] = { "participant", "actor", "boundary", "control", "entity", "database",
                                      "collections", "queue", NULL };
        bool declared = false;
        for (u32 k = 0; decl[k] && !declared; k++) {
            if (!starts_word(s, n, decl[k])) continue;
            declared = true;
            const u8 *r = s + strlen(decl[k]); u32 rn = n - (u32)strlen(decl[k]);
            trim(&r, &rn);
            if (!rn) continue;
            /* `"Long name" as L` / `L as "Long name"` / `L` — colours and orders dropped */
            u32 as = 0;
            while (as + 4 <= rn && memcmp(r + as, " as ", 4) != 0) as++;
            const u8 *key = r, *label = r; u32 kl = rn, ll = rn;
            if (as + 4 <= rn) {
                const u8 *a = r; u32 al = as; const u8 *b = r + as + 4; u32 bl = rn - as - 4;
                trim(&a, &al); trim(&b, &bl);
                u32 bw = 0;
                while (bw < bl && !dg_is_space(b[bw])) bw++;
                if (al && a[0] == '"') { key = b; kl = bw; label = a; ll = al; }
                else { key = a; kl = al; label = b; ll = bw ? bl : bl; }
            } else {
                u32 w = 0;
                while (w < rn && !dg_is_space(r[w])) w++;
                if (r[0] == '"') { w = 1; while (w < rn && r[w] != '"') w++; w = w < rn ? w + 1 : rn; }
                kl = w; ll = w;
            }
            i32 id = name_of(key, kl);
            if (id < 0) return false;
            const u8 *lb = label; u32 lbn = ll;
            trim(&lb, &lbn);
            if (lbn >= 2 && lb[0] == '"' && lb[lbn - 1] == '"') { lb++; lbn -= 2; }
            u32 cut = 0;
            while (cut < lbn && lb[cut] != '#' && !(lb[cut] == '<' && cut + 1 < lbn && lb[cut + 1] == '<')) cut++;
            while (cut && dg_is_space(lb[cut - 1])) cut--;
            dg_strcpy(g_names[id].label, DG_LABEL_CAP, lb, cut);
            pu_seq_declare(o, id, !strcmp(decl[k], "actor"));
        }
        if (declared) continue;
        if (starts_word(s, n, "title")) { puts_(o, "title "); put(o, (const char *)s + 5, n - 5); puts_(o, "\n"); continue; }
        if (starts_word(s, n, "autonumber")) { puts_(o, "autonumber\n"); continue; }
        if (starts_word(s, n, "activate") || starts_word(s, n, "deactivate")) {
            bool act = s[0] == 'a';
            u32 k = act ? 8u : 10u;
            i32 id = pu_participant(s + k, n - k);
            if (id < 0) continue;
            pu_seq_declare(o, id, false);
            putf(o, "%s p%d\n", act ? "activate" : "deactivate", id);
            continue;
        }
        static const char *frames[] = { "alt", "loop", "opt", "par", "critical", "break", "group", NULL };
        bool framed = false;
        for (u32 k = 0; frames[k] && !framed; k++) {
            if (!starts_word(s, n, frames[k])) continue;
            framed = true;
            puts_(o, !strcmp(frames[k], "group") ? "opt" : frames[k]);
            put(o, (const char *)s + strlen(frames[k]), n - (u32)strlen(frames[k]));
            puts_(o, "\n");
        }
        if (framed) continue;
        if (starts_word(s, n, "else")) { puts_(o, "else"); put(o, (const char *)s + 4, n - 4); puts_(o, "\n"); continue; }
        if (starts_word(s, n, "end") && !starts_word(s, n, "end note")) { puts_(o, "end\n"); continue; }
        if (starts_word(s, n, "note") || starts_word(s, n, "hnote") || starts_word(s, n, "rnote")) {
            /* `note left of A : text` or a block ending in `end note` */
            const u8 *colon = memchr(s, ':', n);
            u32 hl = colon ? (u32)(colon - s) : n;
            const char *where = has(s, hl, "left") ? "left of" : has(s, hl, "right") ? "right of" : "over";
            /* the participants after `of` or `over` */
            u32 of = 0;
            bool named = false;
            for (; of + 3 <= hl && !named; of++) {
                if (!memcmp(s + of, " of", 3) && (of + 3 == hl || !is_word(s[of + 3]))) { of += 3; named = true; }
                else if (of + 4 <= hl && !memcmp(s + of, "over", 4)) { of += 4; named = true; }
                if (named) break;
            }
            if (!named) of = hl;
            const u8 *who = s + of; u32 wn = hl > of ? hl - of : 0;
            trim(&who, &wn);
            i32 ids[2] = { -1, -1 };
            u32 c = 0;
            while (c < wn && who[c] != ',') c++;
            ids[0] = pu_participant(who, c);
            if (c < wn) ids[1] = pu_participant(who + c + 1, wn - c - 1);
            if (ids[0] < 0 && last_from >= 0) ids[0] = last_from;
            char text[DG_LABEL_CAP * 2];
            u32 tl = 0;
            if (colon) {
                const u8 *t = colon + 1; u32 tn = n - (u32)(colon - s) - 1;
                trim(&t, &tn);
                tl = tn < sizeof text - 1 ? tn : (u32)sizeof text - 1;
                memcpy(text, t, tl);
            } else {
                while (++li < g_npl && !starts_word(g_pl[li].s, g_pl[li].n, "end note") &&
                       !starts_word(g_pl[li].s, g_pl[li].n, "endnote") &&
                       !starts_word(g_pl[li].s, g_pl[li].n, "end hnote") &&
                       !starts_word(g_pl[li].s, g_pl[li].n, "end rnote")) {
                    if (tl && tl + 5 < sizeof text) { memcpy(text + tl, "<br/>", 5); tl += 5; }
                    u32 k = g_pl[li].n < sizeof text - 1 - tl ? g_pl[li].n : (u32)(sizeof text - 1 - tl);
                    memcpy(text + tl, g_pl[li].s, k);
                    tl += k;
                }
            }
            text[tl] = 0;
            if (ids[0] < 0) continue;
            pu_seq_declare(o, ids[0], false);
            if (ids[1] >= 0) pu_seq_declare(o, ids[1], false);
            putf(o, "Note %s p%d", where, ids[0]);
            if (ids[1] >= 0 && !strcmp(where, "over")) putf(o, ",p%d", ids[1]);
            puts_(o, ": ");
            put(o, text, tl);
            puts_(o, "\n");
            continue;
        }
        if (starts_word(s, n, "return")) {
            if (last_from < 0 || last_to < 0) continue;
            putf(o, "p%d-->>p%d: ", last_to, last_from);
            put(o, (const char *)s + 6, n - 6);
            puts_(o, "\n");
            continue;
        }
        if (s[0] == '=' || s[0] == '.' || s[0] == '|' || starts_word(s, n, "ref") || starts_word(s, n, "delay") ||
            starts_word(s, n, "box") || starts_word(s, n, "end box") || starts_word(s, n, "newpage")) continue;
        /* a message: `A -> B : text`, `A --> B`, `A ->> B`, `B <- A`, `A ->x B`, `A -[#red]> B` */
        u32 a0 = 0;
        if (s[0] == '"') { a0 = 1; while (a0 < n && s[a0] != '"') a0++; if (a0 < n) a0++; }
        else while (a0 < n && !dg_is_space(s[a0]) && s[a0] != '-' && s[a0] != '<' && s[a0] != '.') a0++;
        u32 p = a0;
        while (p < n && dg_is_space(s[p])) p++;
        u32 op0 = p;
        while (p < n && (s[p] == '-' || s[p] == '<' || s[p] == '>' || s[p] == '.' || s[p] == 'x' || s[p] == 'o' ||
                         s[p] == '/' || s[p] == '\\' || s[p] == '[' || s[p] == ']' || s[p] == '#' || is_word(s[p]))) {
            if (s[p] == '[') { while (p < n && s[p] != ']') p++; if (p >= n) break; }
            if (is_word(s[p]) && s[p] != 'x' && s[p] != 'o') break;
            p++;
        }
        const u8 *op = s + op0; u32 opn = p - op0;
        if (!opn || !(has(op, opn, "-") || has(op, opn, "."))) continue;
        while (opn && (op[opn - 1] == 'x' || op[opn - 1] == 'o') && opn >= 2 && op[opn - 2] != '-' && op[opn - 2] != '>') opn--;
        u32 b0 = op0 + opn;
        u32 colon = b0;
        while (colon < n && s[colon] != ':') colon++;
        i32 a = pu_participant(s, a0), b = pu_participant(s + b0, colon - b0);
        if (a < 0 || b < 0) continue;
        bool rev = op[0] == '<' && !has(op, opn, ">");
        bool dashed = has(op, opn, "--") || has(op, opn, "..");
        bool lost = op[opn - 1] == 'x';
        bool thin = has(op, opn, ">>") || has(op, opn, "\\") || has(op, opn, "/");
        if (rev) { i32 t = a; a = b; b = t; }
        pu_seq_declare(o, a, false);
        pu_seq_declare(o, b, false);
        const char *arrow = lost ? (dashed ? "--x" : "-x") : thin ? (dashed ? "--)" : "-)") : (dashed ? "-->>" : "->>");
        putf(o, "p%d%sp%d: ", a, arrow, b);
        if (colon < n) {
            const u8 *t = s + colon + 1; u32 tn = n - colon - 1;
            trim(&t, &tn);
            put(o, (const char *)t, tn);
        }
        puts_(o, "\n");
        last_from = a; last_to = b;
    }
    return !o->full;
}

/* ---- class and state: nearly Mermaid already ---- */

static bool pu_class(Out *o) {
    puts_(o, "classDiagram\n");
    for (u32 li = 0; li < g_npl && !o->full; li++) {
        const u8 *s = g_pl[li].s; u32 n = g_pl[li].n;
        if (starts_word(s, n, "note") || starts_word(s, n, "title")) {
            if (starts_word(s, n, "note") && !memchr(s, ':', n))
                while (++li < g_npl && !starts_word(g_pl[li].s, g_pl[li].n, "end note")) {}
            continue;
        }
        if (starts_word(s, n, "package") || starts_word(s, n, "namespace")) {
            /* a package's braces only group: its classes are listed as they are */
            continue;
        }
        const char *stereo = NULL;
        u32 skip = 0;
        if (starts_word(s, n, "abstract class")) { stereo = "abstract"; skip = 14; }
        else if (starts_word(s, n, "abstract")) { stereo = "abstract"; skip = 8; }
        else if (starts_word(s, n, "interface")) { stereo = "interface"; skip = 9; }
        else if (starts_word(s, n, "enum")) { stereo = "enumeration"; skip = 4; }
        else if (starts_word(s, n, "annotation")) { stereo = "annotation"; skip = 10; }
        else if (starts_word(s, n, "class")) skip = 5;
        if (skip) {
            const u8 *r = s + skip; u32 rn = n - skip;
            trim(&r, &rn);
            if (!rn) continue;
            bool open = r[rn - 1] == '{';
            if (open) { rn--; trim(&r, &rn); }
            u32 w = 0;
            while (w < rn && !dg_is_space(r[w]) && r[w] != '<' && r[w] != '{') w++;
            if (r[0] == '"') { w = 1; while (w < rn && r[w] != '"') w++; w = w < rn ? w + 1 : rn; }
            i32 id = name_of(r, w);
            if (id < 0) return false;
            if (!stereo) for (u32 k = 0; k + 2 < rn; k++)
                if (r[k] == '<' && r[k + 1] == '<') {
                    u32 e = k + 2;
                    while (e + 1 < rn && !(r[e] == '>' && r[e + 1] == '>')) e++;
                    static char st[48];
                    u32 sl = e - k - 2 < sizeof st - 1 ? e - k - 2 : (u32)sizeof st - 1;
                    memcpy(st, r + k + 2, sl); st[sl] = 0;
                    stereo = st;
                    break;
                }
            putf(o, "class c%d[\"", id);
            put_label(o, (const u8 *)g_names[id].label, (u32)strlen(g_names[id].label));
            puts_(o, "\"]");
            if (open || stereo) {
                puts_(o, " {\n");
                if (stereo) putf(o, "<<%s>>\n", stereo);
                if (open)
                    while (++li < g_npl && g_pl[li].s[0] != '}') {
                        const u8 *m = g_pl[li].s; u32 mn = g_pl[li].n;
                        if (mn >= 2 && (m[0] == '-' || m[0] == '.' || m[0] == '=' || m[0] == '_') && m[1] == m[0]) continue;
                        put(o, (const char *)m, mn);
                        puts_(o, "\n");
                    }
                puts_(o, "}\n");
            } else puts_(o, "\n");
            continue;
        }
        if (s[0] == '}') continue;
        /* a relation: `A <|-- B`, `A "1" *-- "many" B : has`, `A -up-|> B` */
        u32 p = 0;
        if (s[0] == '"') { p = 1; while (p < n && s[p] != '"') p++; if (p < n) p++; }
        else while (p < n && !dg_is_space(s[p]) && s[p] != '-' && s[p] != '.' && s[p] != '<' && s[p] != '*' &&
                    !((s[p] == 'o' || s[p] == 'x') && p + 1 < n && (s[p + 1] == '-' || s[p + 1] == '.') && p > 0 &&
                      !is_word(s[p - 1]))) p++;
        if (!p || p >= n) continue;
        i32 a = name_of(s, p);
        u32 q = p;
        while (q < n && dg_is_space(s[q])) q++;
        char card_a[24] = "", card_b[24] = "";
        if (q < n && s[q] == '"') {
            u32 e = q + 1;
            while (e < n && s[e] != '"') e++;
            dg_strcpy(card_a, sizeof card_a, s + q + 1, e - q - 1);
            q = e < n ? e + 1 : n;
            while (q < n && dg_is_space(s[q])) q++;
        }
        u32 op0 = q;
        while (q < n && !dg_is_space(s[q]) && s[q] != '"') q++;
        char op[24];
        u32 ol = 0;
        for (u32 k = op0; k < q && ol + 1 < sizeof op; k++) {
            /* direction hints `-up-`, `-left-` are layout only */
            if (is_word(s[k]) && s[k] != 'o' && s[k] != 'x' && s[k] != '*') continue;
            if (is_word(s[k]) && k > op0 && k + 1 < q && is_word(s[k - 1]) && is_word(s[k + 1])) continue;
            op[ol++] = (char)s[k];
        }
        op[ol] = 0;
        while (ol >= 2 && op[ol - 1] == '-' && op[ol - 2] == '-' && ol > 2 && op[ol - 3] == '-') op[--ol] = 0;
        if (!ol || !(strchr(op, '-') || strchr(op, '.'))) continue;
        while (q < n && dg_is_space(s[q])) q++;
        if (q < n && s[q] == '"') {
            u32 e = q + 1;
            while (e < n && s[e] != '"') e++;
            dg_strcpy(card_b, sizeof card_b, s + q + 1, e - q - 1);
            q = e < n ? e + 1 : n;
            while (q < n && dg_is_space(s[q])) q++;
        }
        u32 b0 = q;
        while (q < n && s[q] != ':') q++;
        i32 b = name_of(s + b0, q - b0);
        if (a < 0 || b < 0) continue;
        putf(o, "c%d ", a);
        if (card_a[0]) putf(o, "\"%s\" ", card_a);
        putf(o, "%s ", op);
        if (card_b[0]) putf(o, "\"%s\" ", card_b);
        putf(o, "c%d", b);
        if (q < n) { puts_(o, " : "); const u8 *t = s + q + 1; u32 tn = n - q - 1; trim(&t, &tn); put(o, (const char *)t, tn); }
        puts_(o, "\n");
    }
    /* every class named only in a relation still needs its label */
    for (u32 i = 0; i < g_nnames; i++) {
        putf(o, "class c%u[\"", i);
        put_label(o, (const u8 *)g_names[i].label, (u32)strlen(g_names[i].label));
        puts_(o, "\"]\n");
    }
    return !o->full;
}

static bool pu_state(Out *o) {
    puts_(o, "stateDiagram-v2\n");
    for (u32 li = 0; li < g_npl && !o->full; li++) {
        const u8 *s = g_pl[li].s; u32 n = g_pl[li].n;
        if (starts_word(s, n, "note")) {
            if (!memchr(s, ':', n)) while (++li < g_npl && !starts_word(g_pl[li].s, g_pl[li].n, "end note")) {}
            continue;
        }
        if (starts_word(s, n, "title") || (n == 2 && (!memcmp(s, "--", 2) || !memcmp(s, "||", 2)))) continue;
        /* PlantUML's single-dash links and direction hints are Mermaid's `-->` */
        char line[512];
        u32 o2 = 0;
        for (u32 k = 0; k < n && o2 + 4 < sizeof line; k++) {
            if (s[k] == '-' && (k + 1 >= n || s[k + 1] != '-') && (k == 0 || s[k - 1] != '-')) {
                u32 e = k + 1;
                while (e < n && is_word(s[e])) e++;
                bool hint = e > k + 1 && e < n && s[e] == '-';
                if (hint) { k = e; }
                if (k + 1 < n && s[k + 1] == '>') { memcpy(line + o2, "-->", 3); o2 += 3; k++; continue; }
                if (hint && k + 1 < n && s[k + 1] == '>') { memcpy(line + o2, "-->", 3); o2 += 3; k++; continue; }
            }
            line[o2++] = (char)s[k];
        }
        put(o, line, o2);
        puts_(o, "\n");
    }
    return !o->full;
}

/* ---- activity: the structure walked into a flowchart ---- */

#define ACT_TAILS 24

typedef struct { i32 node[ACT_TAILS]; char label[ACT_TAILS][DG_ELABEL_CAP]; u32 n; } Tails;
typedef struct { u8 kind; i32 node; Tails ends; Tails start; char out_label[DG_ELABEL_CAP]; bool had_else; i32 first; } ActFrame;

static Tails g_tails;
static char  g_next_label[DG_ELABEL_CAP];
static i32   g_last_made;

static i32 act_node(Out *o, const u8 *label, u32 ln, u8 shape) {
    if (g_nnames >= NAME_CAP) { g_overflow = true; return -1; }
    Name *m = &g_names[g_nnames];
    memset(m, 0, sizeof *m);
    snprintf(m->key, sizeof m->key, "#%u", g_nnames);
    u32 k = ln < DG_LABEL_CAP - 1 ? ln : DG_LABEL_CAP - 1;
    memcpy(m->label, label, k);
    m->label[k] = 0;
    m->shape = shape;
    m->cluster = -1;
    i32 nid = (i32)g_nnames++;
    put_node(o, nid);
    puts_(o, "\n");
    for (u32 t = 0; t < g_tails.n; t++) {
        putf(o, "n%d -->", g_tails.node[t]);
        const char *lab = g_tails.label[t][0] ? g_tails.label[t] : g_next_label;
        if (lab[0]) { puts_(o, "|\""); put_label(o, (const u8 *)lab, (u32)strlen(lab)); puts_(o, "\"|"); }
        putf(o, " n%d\n", nid);
    }
    g_tails.n = 1;
    g_tails.node[0] = nid;
    g_tails.label[0][0] = 0;
    g_next_label[0] = 0;
    g_last_made = nid;
    return nid;
}

static void tails_add(Tails *t, const Tails *from) {
    for (u32 i = 0; i < from->n && t->n < ACT_TAILS; i++) {
        t->node[t->n] = from->node[i];
        snprintf(t->label[t->n], DG_ELABEL_CAP, "%s", from->label[i]);
        t->n++;
    }
}

/* `(text)` after a keyword: the text. */
static void paren(const u8 *s, u32 n, const char *after, char *out, u32 cap) {
    out[0] = 0;
    u32 i = 0;
    if (after) {
        u32 k = (u32)strlen(after);
        while (i + k <= n && memcmp(s + i, after, k)) i++;
        if (i + k > n) return;
        i += k;
    }
    while (i < n && s[i] != '(') i++;
    if (i >= n) return;
    u32 e = i + 1, depth = 1;
    while (e < n && depth) { if (s[e] == '(') depth++; else if (s[e] == ')') depth--; if (depth) e++; }
    dg_strcpy(out, cap, s + i + 1, e - i - 1);
}

static bool pu_activity(Out *o) {
    puts_(o, "flowchart TD\n");
    ActFrame st[32];
    u32 sp = 0;
    g_tails.n = 0;
    g_next_label[0] = 0;
    g_last_made = -1;
    for (u32 li = 0; li < g_npl && !o->full && !g_overflow; li++) {
        const u8 *s = g_pl[li].s; u32 n = g_pl[li].n;
        char c[DG_LABEL_CAP];
        if (s[0] == '|' || starts_word(s, n, "note") || starts_word(s, n, "partition") || s[0] == '}' ||
            starts_word(s, n, "title") || starts_word(s, n, "detach")) {
            if (starts_word(s, n, "note") && !memchr(s, ':', n))
                while (++li < g_npl && !starts_word(g_pl[li].s, g_pl[li].n, "end note")) {}
            continue;
        }
        if (starts_word(s, n, "start")) { act_node(o, (const u8 *)"start", 5, SHP_START); continue; }
        if (starts_word(s, n, "stop") || starts_word(s, n, "end") || starts_word(s, n, "kill")) {
            act_node(o, (const u8 *)"end", 3, SHP_STOP);
            g_tails.n = 0;
            continue;
        }
        if (s[0] == '-' && n > 1 && s[1] == '>') {           /* `-> label;` labels the next link */
            const u8 *t = s + 2; u32 tn = n - 2;
            trim(&t, &tn);
            if (tn && t[tn - 1] == ';') tn--;
            dg_strcpy(g_next_label, sizeof g_next_label, t, tn);
            continue;
        }
        if (s[0] == ':') {
            /* an action, possibly over several lines, to its closing `;` */
            char text[DG_LABEL_CAP * 2];
            u32 tl = 0;
            const u8 *t = s + 1; u32 tn = n - 1;
            for (;;) {
                bool last = tn && (t[tn - 1] == ';' || t[tn - 1] == '|' || t[tn - 1] == '<' || t[tn - 1] == '>' ||
                                   t[tn - 1] == '/' || t[tn - 1] == ']' || t[tn - 1] == '}');
                u32 k = last ? tn - 1 : tn;
                if (tl && tl + 1 < sizeof text) text[tl++] = '\n';
                if (k > sizeof text - 1 - tl) k = (u32)(sizeof text - 1 - tl);
                memcpy(text + tl, t, k);
                tl += k;
                if (last || li + 1 >= g_npl) break;
                li++;
                t = g_pl[li].s; tn = g_pl[li].n;
            }
            act_node(o, (const u8 *)text, tl, SHP_ROUND);
            continue;
        }
        if (starts_word(s, n, "if") || starts_word(s, n, "elseif")) {
            bool elif = s[1] == 'l';
            paren(s, n, NULL, c, sizeof c);
            char yes[DG_ELABEL_CAP];
            paren(s, n, "then", yes, sizeof yes);
            if (elif && sp) {
                ActFrame *f = &st[sp - 1];
                tails_add(&f->ends, &g_tails);
                g_tails.n = 1;
                g_tails.node[0] = f->node;
                snprintf(g_tails.label[0], DG_ELABEL_CAP, "%s", f->out_label[0] ? f->out_label : "no");
            }
            i32 d = act_node(o, (const u8 *)c, (u32)strlen(c), SHP_DIAMOND);
            if (elif && sp) {
                st[sp - 1].node = d;
                char no[DG_ELABEL_CAP];
                paren(s, n, "else", no, sizeof no);
                snprintf(st[sp - 1].out_label, DG_ELABEL_CAP, "%s", no);
            } else {
                if (sp >= 32) return false;
                ActFrame *f = &st[sp++];
                memset(f, 0, sizeof *f);
                f->kind = 'i';
                f->node = d;
            }
            snprintf(g_tails.label[0], DG_ELABEL_CAP, "%s", yes[0] ? yes : "yes");
            continue;
        }
        if (starts_word(s, n, "else")) {
            if (!sp) continue;
            ActFrame *f = &st[sp - 1];
            tails_add(&f->ends, &g_tails);
            paren(s, n, NULL, c, sizeof c);
            g_tails.n = 1;
            g_tails.node[0] = f->node;
            snprintf(g_tails.label[0], DG_ELABEL_CAP, "%s", c[0] ? c : (f->out_label[0] ? f->out_label : "no"));
            f->had_else = true;
            continue;
        }
        if (starts_word(s, n, "endif") || starts_word(s, n, "end if")) {
            if (!sp) continue;
            ActFrame *f = &st[--sp];
            if (!f->had_else && g_tails.n < ACT_TAILS) {
                Tails no = { { f->node }, { "" }, 1 };
                snprintf(no.label[0], DG_ELABEL_CAP, "%s", f->out_label[0] ? f->out_label : "no");
                tails_add(&g_tails, &no);
            }
            tails_add(&g_tails, &f->ends);
            continue;
        }
        if (starts_word(s, n, "while")) {
            paren(s, n, NULL, c, sizeof c);
            char yes[DG_ELABEL_CAP];
            paren(s, n, "is", yes, sizeof yes);
            i32 d = act_node(o, (const u8 *)c, (u32)strlen(c), SHP_DIAMOND);
            if (sp >= 32) return false;
            ActFrame *f = &st[sp++];
            memset(f, 0, sizeof *f);
            f->kind = 'w';
            f->node = d;
            snprintf(g_tails.label[0], DG_ELABEL_CAP, "%s", yes[0] ? yes : "yes");
            continue;
        }
        if (starts_word(s, n, "endwhile") || starts_word(s, n, "end while")) {
            if (!sp) continue;
            ActFrame *f = &st[--sp];
            for (u32 t = 0; t < g_tails.n; t++) putf(o, "n%d --> n%d\n", g_tails.node[t], f->node);
            paren(s, n, NULL, c, sizeof c);
            g_tails.n = 1;
            g_tails.node[0] = f->node;
            snprintf(g_tails.label[0], DG_ELABEL_CAP, "%s", c[0] ? c : "no");
            continue;
        }
        if (starts_word(s, n, "repeat") && !starts_word(s, n, "repeat while")) {
            if (sp >= 32) return false;
            ActFrame *f = &st[sp++];
            memset(f, 0, sizeof *f);
            f->kind = 'r';
            f->first = (i32)g_nnames;               /* the next node made starts the loop */
            continue;
        }
        if (starts_word(s, n, "repeat while") || starts_word(s, n, "repeatwhile")) {
            if (!sp) continue;
            ActFrame *f = &st[--sp];
            paren(s, n, NULL, c, sizeof c);
            char yes[DG_ELABEL_CAP], no[DG_ELABEL_CAP];
            paren(s, n, "is", yes, sizeof yes);
            paren(s, n, "not", no, sizeof no);
            i32 d = act_node(o, (const u8 *)c, (u32)strlen(c), SHP_DIAMOND);
            if (f->first < (i32)g_nnames && f->first != d) {
                putf(o, "n%d -->|\"", d);
                puts_(o, yes[0] ? yes : "yes");
                putf(o, "\"| n%d\n", f->first);
            }
            snprintf(g_tails.label[0], DG_ELABEL_CAP, "%s", no[0] ? no : "no");
            continue;
        }
        if (starts_word(s, n, "fork") || starts_word(s, n, "split")) {
            bool again = has(s, n, "again");
            if (!again) {
                if (sp >= 32) return false;
                ActFrame *f = &st[sp++];
                memset(f, 0, sizeof *f);
                f->kind = 'f';
                tails_add(&f->start, &g_tails);
            } else if (sp) {
                ActFrame *f = &st[sp - 1];
                tails_add(&f->ends, &g_tails);
                g_tails = f->start;
            }
            continue;
        }
        if (starts_word(s, n, "end fork") || starts_word(s, n, "endfork") || starts_word(s, n, "end split") ||
            starts_word(s, n, "end merge")) {
            if (!sp) continue;
            ActFrame *f = &st[--sp];
            tails_add(&g_tails, &f->ends);
            continue;
        }
    }
    return !o->full && g_nnames > 0;
}

/* ---- anything else: links between named things ---- */

static bool pu_graph(Out *o) {
    putf(o, "flowchart %s\n", g_pu_lr ? "LR" : "TD");
    i32 groups[16];
    u32 gsp = 0, ng = 0;
    for (u32 li = 0; li < g_npl && !o->full; li++) {
        const u8 *s = g_pl[li].s; u32 n = g_pl[li].n;
        if (starts_word(s, n, "note") || starts_word(s, n, "title")) {
            if (starts_word(s, n, "note") && !memchr(s, ':', n))
                while (++li < g_npl && !starts_word(g_pl[li].s, g_pl[li].n, "end note")) {}
            continue;
        }
        if (s[0] == '}') { if (gsp) { gsp--; puts_(o, "end\n"); } continue; }
        static const char *kinds[] = { "actor", "usecase", "component", "node", "database", "rectangle",
                                       "package", "folder", "frame", "cloud", "artifact", "storage",
                                       "interface", "boundary", "control", "entity", "agent", "queue", NULL };
        u32 kw = 0;
        for (u32 k = 0; kinds[k] && !kw; k++) if (starts_word(s, n, kinds[k])) kw = (u32)strlen(kinds[k]);
        if (kw && s[n - 1] == '{') {
            /* a container: a subgraph */
            const u8 *r = s + kw; u32 rn = n - kw - 1;
            trim(&r, &rn);
            if (gsp < 16) groups[gsp++] = (i32)ng;
            putf(o, "subgraph g%u [\"", ng++);
            if (rn >= 2 && r[0] == '"') { r++; rn -= 2; }
            put_label(o, r, rn);
            puts_(o, "\"]\n");
            continue;
        }
        if (kw) {
            const u8 *r = s + kw; u32 rn = n - kw;
            trim(&r, &rn);
            u32 as = 0;
            while (as + 4 <= rn && memcmp(r + as, " as ", 4)) as++;
            const u8 *lab = r; u32 ll = as + 4 <= rn ? as : rn;
            const u8 *key = as + 4 <= rn ? r + as + 4 : r; u32 kl = as + 4 <= rn ? rn - as - 4 : rn;
            i32 id = name_of(key, kl);
            if (id < 0) continue;
            trim(&lab, &ll);
            if (ll >= 2 && (lab[0] == '"' || lab[0] == '(' || lab[0] == '[' || lab[0] == ':')) { lab++; ll -= 2; }
            dg_strcpy(g_names[id].label, DG_LABEL_CAP, lab, ll);
            g_names[id].shape = !strncmp((const char *)s, "usecase", 7) ? SHP_STADIUM :
                                !strncmp((const char *)s, "database", 8) ? SHP_CYL :
                                !strncmp((const char *)s, "actor", 5) ? SHP_CIRCLE : SHP_RECT;
            if (!g_names[id].shown) { g_names[id].shown = true; put_node(o, id); puts_(o, "\n"); }
            continue;
        }
        /* `(Label) as ID`, `[Label] as ID`, `"Label" as ID`: a declaration */
        if (!has(s, n, "--") && !has(s, n, "..") && !has(s, n, "->")) {
            u32 as = 0;
            while (as + 4 <= n && memcmp(s + as, " as ", 4)) as++;
            if (as + 4 <= n) {
                const u8 *lab = s; u32 ll = as;
                trim(&lab, &ll);
                u8 sh = lab[0] == '(' ? SHP_STADIUM : lab[0] == ':' ? SHP_CIRCLE : SHP_RECT;
                if (ll >= 2 && (lab[0] == '(' || lab[0] == '[' || lab[0] == ':' || lab[0] == '"')) { lab++; ll -= 2; }
                i32 id = name_of(s + as + 4, n - as - 4);
                if (id >= 0) {
                    dg_strcpy(g_names[id].label, DG_LABEL_CAP, lab, ll);
                    g_names[id].shape = sh;
                    if (!g_names[id].shown) { g_names[id].shown = true; put_node(o, id); puts_(o, "\n"); }
                }
            }
            continue;
        }
        /* `A --> B : label` with `(use case)`, `[component]`, `:actor:` ends */
        u32 p = 0;
        u8 open = s[0];
        u8 close = open == '(' ? ')' : open == '[' ? ']' : open == ':' ? ':' : open == '"' ? '"' : 0;
        if (close) { p = 1; while (p < n && s[p] != close) p++; if (p < n) p++; }
        else while (p < n && !dg_is_space(s[p]) && s[p] != '-' && s[p] != '.' && s[p] != '<') p++;
        u32 q = p;
        while (q < n && dg_is_space(s[q])) q++;
        u32 op0 = q;
        while (q < n && (s[q] == '-' || s[q] == '.' || s[q] == '<' || s[q] == '>' || s[q] == '|' || s[q] == '*' ||
                         s[q] == 'o' || (is_word(s[q]) && q > op0 && s[q - 1] == '-'))) q++;
        const u8 *op = s + op0; u32 opn = q - op0;
        if (!opn || !(has(op, opn, "-") || has(op, opn, "."))) continue;
        while (q < n && dg_is_space(s[q])) q++;
        u32 b0 = q;
        u8 bo = q < n ? s[q] : 0;
        u8 bc = bo == '(' ? ')' : bo == '[' ? ']' : bo == ':' ? ':' : bo == '"' ? '"' : 0;
        if (bc) { q++; while (q < n && s[q] != bc) q++; if (q < n) q++; }
        else while (q < n && !dg_is_space(s[q]) && s[q] != ':') q++;
        i32 ids[2];
        const u8 *ends[2] = { s, s + b0 }; u32 lens[2] = { p, q - b0 };
        for (u32 k = 0; k < 2; k++) {
            const u8 *e = ends[k]; u32 el = lens[k];
            trim(&e, &el);
            u8 sh = SHP_RECT;
            if (el >= 2 && (e[0] == '(' || e[0] == '[' || e[0] == ':' || e[0] == '"')) {
                sh = e[0] == '(' ? SHP_STADIUM : e[0] == ':' ? SHP_CIRCLE : SHP_RECT;
                e++; el -= 2;
            }
            ids[k] = name_of(e, el);
            if (ids[k] >= 0 && !g_names[ids[k]].shown) {
                g_names[ids[k]].shown = true;
                if (sh != SHP_RECT) g_names[ids[k]].shape = sh;
                put_node(o, ids[k]);
                puts_(o, "\n");
            }
        }
        if (ids[0] < 0 || ids[1] < 0) continue;
        bool rev = op[0] == '<' && op[opn - 1] != '>';
        bool head = op[opn - 1] == '>' || rev;
        bool dashed = has(op, opn, "..");
        putf(o, "n%d %s", rev ? ids[1] : ids[0], dashed ? (head ? "-.->" : "-.-") : (head ? "-->" : "---"));
        while (q < n && s[q] != ':') q++;
        if (q < n) {
            const u8 *t = s + q + 1; u32 tn = n - q - 1;
            trim(&t, &tn);
            if (tn) { puts_(o, "|\""); put_label(o, t, tn); puts_(o, "\"|"); }
        }
        putf(o, " n%d\n", rev ? ids[0] : ids[1]);
    }
    while (gsp--) puts_(o, "end\n");
    return !o->full && g_nnames > 0;
}

/* =========================================================================
 * the entry point
 * ========================================================================= */

/* Skip blank lines and comments: where the source's first statement is. */
static u32 first_line(const u8 *s, u32 n) {
    u32 i = 0;
    for (;;) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i + 1 < n && s[i] == '/' && s[i+1] == '/') { while (i < n && s[i] != '\n') i++; continue; }
        if (i + 1 < n && s[i] == '/' && s[i+1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i+1] == '/')) i++;
            i += 2;
            continue;
        }
        if (i < n && s[i] == '#') { while (i < n && s[i] != '\n') i++; continue; }
        return i < n ? i : n;
    }
}

MdDiagramLang dgl_lang(const u8 *src, u32 len) {
    u32 i = first_line(src, len);
    const u8 *s = src + i; u32 n = len - i;
    if (starts_word(s, n, "@startuml")) return MD_DIAGRAM_LANG_PLANTUML;
    u32 k = 0;
    if (starts_word(s, n, "strict")) { k = 6; while (k < n && dg_is_space(s[k])) k++; }
    if (!starts_word(s + k, n - k, "digraph") && !starts_word(s + k, n - k, "graph")) return MD_DIAGRAM_LANG_MERMAID;
    /* `graph` is Mermaid's header too: DOT's opens a brace before its first line ends, or on the next */
    u32 e = k;
    while (e < n && s[e] != '\n' && s[e] != '{') e++;
    if (e < n && s[e] == '{') return MD_DIAGRAM_LANG_GRAPHVIZ;
    u32 nx = e;
    while (nx < n && (s[nx] == '\n' || s[nx] == ' ' || s[nx] == '\t' || s[nx] == '\r')) nx++;
    return nx < n && s[nx] == '{' ? MD_DIAGRAM_LANG_GRAPHVIZ : MD_DIAGRAM_LANG_MERMAID;
}

bool dgl_translate(const u8 *src, u32 len, MdDiagramLang lang, char *out, u32 cap, u32 *out_len) {
    g_nnames = 0;
    g_overflow = false;
    g_ndedges = 0;
    g_nclusters = 0;
    g_rankdir[0] = 0;
    memset(&g_node_default, 0, sizeof g_node_default);
    memset(&g_edge_default, 0, sizeof g_edge_default);
    Out o = { out, cap, 0, false };
    if (cap) out[0] = 0;
    bool ok = false;
    if (lang == MD_DIAGRAM_LANG_GRAPHVIZ) ok = dot_translate(src, len, &o);
    else if (lang == MD_DIAGRAM_LANG_PLANTUML && pu_lines(src, len)) {
        switch (pu_kind()) {
        case PU_SEQ:      ok = pu_sequence(&o); break;
        case PU_CLASS:    ok = pu_class(&o); break;
        case PU_STATE:    ok = pu_state(&o); break;
        case PU_ACTIVITY: ok = pu_activity(&o); break;
        default:          ok = pu_graph(&o); break;
        }
    }
    *out_len = o.len;
    return ok && !g_overflow && !o.full && o.len > 0;
}
