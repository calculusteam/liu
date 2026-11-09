/* Liu — native Mermaid engine. See md_diagram.h. */
#include "ui/markdown/md_diagram_int.h"
#include "core/utf8.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The parsed model — single-threaded, reset per build. */
DNode  g_nodes[DG_MAX_NODES];
u32    g_nnodes;
DEdge  g_edges[DG_MAX_EDGES];
u32    g_nedges;
DGroup g_groups[DG_MAX_GROUPS];
u32    g_ngroups;
DPart  g_parts[DG_MAX_PARTS];
u32    g_nparts;
DMsg   g_msgs[DG_MAX_MSGS];
u32    g_nmsgs, g_nreal_msgs;
DBar   g_bars[DG_MAX_BARS];
u32    g_nbars;
DFrame g_frames[DG_MAX_FRAMES];
u32    g_nframes;
DDiv   g_divs[DG_MAX_DIVS];
u32    g_ndivs;
DSlice g_slices[DG_MAX_SLICES];
u32    g_nslices;
char   g_title[DG_LABEL_CAP];
bool   g_show_data;
bool   g_trunc;     /* a cap was hit — fall back to the code block */
u8     g_dir;       /* 0 TD/TB, 1 LR, 2 BT, 3 RL */
u32    g_head_rows; /* tallest participant label, in rows */
f32    g_cw, g_ch;  /* text cell the layout is made with */
f32    g_gap = 1.0f;

static f32 dg_head_h(void) {
    u32 rows = g_head_rows ? g_head_rows : 1u;
    return g_ch * (DG_ROW_H * (f32)rows + 0.95f);
}

/* ---- small helpers ---- */
bool dg_is_space(u8 c) { return c == ' ' || c == '\t'; }

bool dg_is_word(u8 c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

/* End of the identifier starting at `i`.
 *
 * '-' and '.' belong to an id only *inside* one (`my-node`, `file.txt`): the
 * dashes of `A-->B` and the dots of `A..>B` are operators. Without that rule the
 * id scanner eats the operator and the whole statement collapses into a single
 * node named "A--", so an unspaced edge vanishes. Bytes past ASCII are word
 * bytes: an agent names nodes in the user's language. */
u32 dg_scan_id(const u8 *s, u32 len, u32 i) {
    u32 start = i;
    while (i < len) {
        u8 c = s[i];
        if (dg_is_word(c)) { i++; continue; }
        if ((c == '-' || c == '.') && i > start && i + 1 < len && dg_is_word(s[i+1]) &&
            s[i+1] != '-' && s[i+1] != '.') {
            i++;
            continue;
        }
        break;
    }
    return i;
}

void dg_strcpy(char *dst, u32 cap, const u8 *s, u32 n) {
    if (!dst || cap == 0 || (!s && n != 0)) return;
    if (n >= cap) {
        n = cap - 1;
        /* Never cut a code point in half. */
        while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
    }
    if (n) memcpy(dst, s, n);
    dst[n] = 0;
}

/* Display cells of [s, s+n): a wide character is two, a mark that joins its
 * neighbour none, an undecodable byte one — the measure pass and the draw
 * pass (em_text) walk a label the same way. */
u32 dg_cells(const char *s, u32 n) {
    u32 cells = 0, i = 0;
    while (i < n) {
        u32 cp = 0;
        u32 adv = utf8_decode((const u8 *)s + i, n - i, &cp);
        if (!adv) { i++; cells++; continue; }
        i += adv;
        if (cp < 32) { cells++; continue; }
        int w = utf8_char_width(cp);
        if (w > 0) cells += (u32)w;
    }
    return cells;
}

/* Row `k` of a '\n'-separated label. False past the last row. */
bool dg_row(const char *s, u32 k, const char **out, u32 *outlen) {
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

void dg_text_metrics(const char *s, u32 *rows, u32 *maxcells) {
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

/* Mermaid's entity spellings: `#quot;` (its own), `&quot;`, `&#35;`, `#35;`,
 * `&#x23;`. Returns the bytes consumed and the code point, or 0. */
static u32 dg_entity(const u8 *s, u32 n, u32 i, u32 *cp) {
    if (i >= n || (s[i] != '#' && s[i] != '&')) return 0;
    u32 j = i + 1;
    if (s[i] == '&' && j < n && s[j] == '#') j++;
    bool numeric = (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == 'x' || s[j] == 'X')) &&
                   (s[i] == '#' || s[i + 1] == '#');
    u32 e = j;
    while (e < n && e < j + 10 && s[e] != ';' && !dg_is_space(s[e])) e++;
    if (e >= n || s[e] != ';' || e == j) return 0;
    if (numeric) {
        u32 v = 0, k = j;
        bool hex = (s[k] == 'x' || s[k] == 'X');
        if (hex) k++;
        if (k == e) return 0;
        for (; k < e; k++) {
            u8 c = s[k];
            u32 d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = 10u + (c - 'a');
            else if (hex && c >= 'A' && c <= 'F') d = 10u + (c - 'A');
            else return 0;
            v = v * (hex ? 16u : 10u) + d;
            if (v > 0x10FFFF) return 0;
        }
        if (v < 32 || (v >= 0xD800 && v <= 0xDFFF)) return 0;
        *cp = v;
        return e + 1 - i;
    }
    static const struct { const char *name; u32 cp; } names[] = {
        { "quot", '"' }, { "amp", '&' }, { "lt", '<' }, { "gt", '>' },
        { "nbsp", ' ' }, { "apos", '\'' }, { "semi", ';' }, { "num", '#' },
        { NULL, 0 }
    };
    u32 nl = e - j;
    for (u32 k = 0; names[k].name; k++) {
        if (strlen(names[k].name) == nl && memcmp(s + j, names[k].name, nl) == 0) {
            *cp = names[k].cp;
            return e + 1 - i;
        }
    }
    return 0;
}

/* A simple inline HTML tag (`<b>`, `</em>`…) at i: its length, or 0. The
 * text keeps its words; the markup carries nothing a box can show. */
static u32 dg_inline_tag(const u8 *s, u32 n, u32 i) {
    static const char *tags[] = {
        "b", "i", "u", "s", "em", "strong", "code", "small", "sub", "sup",
        "span", "big", "mark", "del", "ins", NULL
    };
    if (i >= n || s[i] != '<') return 0;
    u32 j = i + 1;
    if (j < n && s[j] == '/') j++;
    u32 ns = j;
    while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) j++;
    u32 nl = j - ns;
    if (!nl || j >= n || s[j] != '>') return 0;
    for (u32 k = 0; tags[k]; k++) {
        if (strlen(tags[k]) != nl) continue;
        bool eq = true;
        for (u32 m = 0; m < nl; m++) {
            u8 c = s[ns + m];
            if (c >= 'A' && c <= 'Z') c = (u8)(c + 32);
            if (c != (u8)tags[k][m]) { eq = false; break; }
        }
        if (eq) return j + 1 - i;
    }
    return 0;
}

/* Copy a Mermaid label: trims, strips one layer of quotes (and the backticks
 * of a markdown string), folds `<br/>` and a literal `\n` into real rows,
 * decodes entities, drops inline markup and `**`, and replaces control bytes
 * with a space so an adversarial fence cannot push a NUL or an escape into the
 * glyph stream. */
void dg_set_label(char *dst, u32 cap, const u8 *s, u32 n) {
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!s) return;
    while (n && dg_is_space(s[0])) { s++; n--; }
    while (n && (dg_is_space(s[n-1]) || s[n-1] == '\r')) n--;
    if (n >= 2 && s[0] == '"' && s[n-1] == '"') { s++; n -= 2; }
    if (n >= 2 && s[0] == '`' && s[n-1] == '`') { s++; n -= 2; }
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
        u32 tl = dg_inline_tag(s, n, i);
        if (tl) { i += tl; continue; }
        if (c == '*' && i + 1 < n && s[i+1] == '*') { i += 2; continue; }
        u32 cp = 0;
        u32 el = dg_entity(s, n, i, &cp);
        if (el) {
            u8 buf[4];
            u32 bl = utf8_encode(cp, buf);
            if (!bl || o + bl + 1 > cap) break;
            memcpy(dst + o, buf, bl);
            o += bl;
            i += el;
            continue;
        }
        dst[o++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
        i++;
    }
    /* A copy cut at the cap must not end inside a code point. */
    if (o) {
        u32 k = o - 1;
        while (k > 0 && ((u8)dst[k] & 0xC0) == 0x80) k--;
        u32 cp = 0;
        if (((u8)dst[k] & 0x80) && utf8_decode((const u8 *)dst + k, o - k, &cp) != o - k) o = k;
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
u32 dg_find_close(const u8 *s, u32 len, u32 from, const char *close) {
    u32 cl = (u32)strlen(close);
    bool q = false;
    for (u32 i = from; i < len; i++) {
        if (s[i] == '"') { q = !q; continue; }
        if (q) continue;
        if (i + cl <= len && memcmp(s + i, close, cl) == 0) return i;
    }
    return len;
}

bool dg_starts(const u8 *s, u32 len, const char *kw) {
    u32 n = (u32)strlen(kw);
    return len >= n && memcmp(s, kw, n) == 0;
}

/* Keyword at the head of the line, followed by a non-word byte. */
bool dg_kw(const u8 *s, u32 len, const char *kw) {
    u32 n = (u32)strlen(kw);
    if (len < n || memcmp(s, kw, n) != 0) return false;
    return len == n || !dg_is_word(s[n]);
}

/* ---- model ---- */
static void dg_reset(void) {
    g_nnodes = 0; g_nedges = 0; g_ngroups = 0;
    g_nparts = 0; g_nmsgs = 0; g_nreal_msgs = 0; g_nbars = 0;
    g_nframes = 0; g_ndivs = 0; g_nslices = 0;
    g_title[0] = 0; g_show_data = false;
    g_trunc = false; g_dir = 0; g_head_rows = 0;
    dgm_reset();
}

void dg_mark_used(i32 group) {
    for (i32 g = group, guard = 0; g >= 0 && guard < DG_MAX_GROUPS; guard++) {
        if ((u32)g >= g_ngroups) break;
        g_groups[g].used = true;
        g = g_groups[g].parent;
    }
}

/* Is subgraph g inside subgraph anc (or anc itself)? */
static bool dg_group_under(i32 g, i32 anc) {
    for (i32 guard = 0; g >= 0 && (u32)g < g_ngroups && guard < DG_MAX_GROUPS; guard++) {
        if (g == anc) return true;
        g = g_groups[g].parent;
    }
    return false;
}

/* A node is the subgraph's whose statements name it, wherever else it was
 * named first: a mention outside every subgraph claims nothing, and of two
 * subgraphs the one nested deeper keeps it (it is closed first). */
i32 dg_intern(const u8 *id, u32 n, i16 group) {
    char key[DG_ID_CAP];
    dg_strcpy(key, sizeof key, id, n);
    if (!key[0]) return -1;
    for (u32 i = 0; i < g_nnodes; i++) {
        if (strcmp(g_nodes[i].id, key) != 0) continue;
        DNode *nd = &g_nodes[i];
        if (group >= 0 && group != nd->group &&
            (nd->group < 0 || dg_group_under(group, nd->group))) {
            nd->group = group;
            dg_mark_used(group);
        }
        return (i32)i;
    }
    if (g_nnodes >= DG_MAX_NODES) { g_trunc = true; return -1; }
    DNode *nd = &g_nodes[g_nnodes];
    memset(nd, 0, sizeof *nd);
    memcpy(nd->id, key, strlen(key) + 1);
    dg_strcpy(nd->label, sizeof nd->label, (const u8 *)key, (u32)strlen(key));
    nd->shape = SH_RECT;
    nd->group = group;
    nd->proxy = -1;
    /* Mark the whole ancestor chain: a subgraph that only wraps other
     * subgraphs still needs a box drawn around them. */
    dg_mark_used(group);
    return (i32)g_nnodes++;
}

void dg_add_edge(i32 from, i32 to, u8 style, u8 mk_from, u8 mk_to,
                        const char *label) {
    if (from < 0 || to < 0) return;
    if (g_nedges >= DG_MAX_EDGES) { g_trunc = true; return; }
    DEdge *e = &g_edges[g_nedges++];
    memset(e, 0, sizeof *e);
    e->from = from; e->to = to;
    e->style = style; e->mk_from = mk_from; e->mk_to = mk_to;
    if (label) dg_strcpy(e->label, sizeof e->label, (const u8 *)label,
                         (u32)strlen(label));
}

/* ---- line scanner ---- */

/* Next non-empty, non-comment line, trimmed. False at end / at the line cap. */
bool dg_next_line(DScan *sc, const u8 **out, u32 *outlen) {
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

/* `A@{ shape: diam, label: "Text" }` — the newer shape spelling. */
static void dg_parse_at_shape(const u8 *s, u32 len, u32 *pi, i32 idx) {
    u32 i = *pi;
    if (!(i + 1 < len && s[i] == '@' && s[i+1] == '{')) return;
    u32 e = dg_find_close(s, len, i + 2, "}");
    u32 body = i + 2, bend = e < len ? e : len;
    *pi = e < len ? e + 1 : len;
    if (idx < 0) return;
    for (u32 k = body; k + 5 < bend; k++) {
        if (memcmp(s + k, "label", 5) == 0) {
            u32 p = k + 5;
            while (p < bend && (dg_is_space(s[p]) || s[p] == ':')) p++;
            u32 ls = p, le;
            if (p < bend && s[p] == '"') {
                le = p + 1;
                while (le < bend && s[le] != '"') le++;
                if (le < bend) le++;
            } else {
                le = p;
                while (le < bend && s[le] != ',') le++;
            }
            dg_set_label(g_nodes[idx].label, sizeof g_nodes[idx].label, s + ls, le - ls);
            g_nodes[idx].declared = true;
            break;
        }
    }
    static const struct { const char *name; u8 shape; } shapes[] = {
        { "diam", SH_DIAMOND }, { "decision", SH_DIAMOND }, { "rhombus", SH_DIAMOND },
        { "rounded", SH_ROUND }, { "stadium", SH_STADIUM }, { "pill", SH_STADIUM },
        { "circle", SH_CIRCLE }, { "circ", SH_CIRCLE }, { "hex", SH_HEX },
        { "cyl", SH_CYLINDER }, { "database", SH_CYLINDER }, { "db", SH_CYLINDER },
        { "subproc", SH_SUBROUTINE }, { "subroutine", SH_SUBROUTINE },
        { "lean-r", SH_PARA }, { "lean-l", SH_PARA }, { "rect", SH_RECT },
        { NULL, 0 }
    };
    for (u32 k = body; k + 5 < bend; k++) {
        if (memcmp(s + k, "shape", 5) != 0) continue;
        u32 p = k + 5;
        while (p < bend && (dg_is_space(s[p]) || s[p] == ':')) p++;
        u32 ws = p;
        while (p < bend && s[p] != ',' && !dg_is_space(s[p])) p++;
        for (u32 m = 0; shapes[m].name; m++) {
            u32 nl = (u32)strlen(shapes[m].name);
            if (p - ws == nl && memcmp(s + ws, shapes[m].name, nl) == 0) {
                g_nodes[idx].shape = shapes[m].shape;
                g_nodes[idx].declared = true;
                break;
            }
        }
        break;
    }
}

/* Node reference at *pi: id + optional shape bracket. Returns the node index. */
i32 dg_parse_noderef(const u8 *s, u32 len, u32 *pi, i16 group) {
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
        { ">",   "]",   SH_PARA },          /* asymmetric flag */
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
            g_nodes[idx].declared = true;
            if (ll) dg_set_label(g_nodes[idx].label, sizeof g_nodes[idx].label, lp, ll);
        }
        break;
    }
    dg_parse_at_shape(s, len, &i, idx);
    /* `:::className` styles the node; it carries no geometry. */
    if (i + 3 <= len && s[i] == ':' && s[i+1] == ':' && s[i+2] == ':')
        i = dg_scan_id(s, len, i + 3);
    *pi = i;
    return idx;
}

/* `A & B[x] & C` — up to DG_MAX_FAN nodes on one side of a link. */
static u32 dg_parse_refs(const u8 *s, u32 len, u32 *pi, i16 group, i32 *out) {
    u32 n = 0;
    for (;;) {
        u32 before = *pi;
        i32 idx = dg_parse_noderef(s, len, pi, group);
        if (idx < 0) { *pi = before; break; }
        if (n < DG_MAX_FAN) out[n++] = idx;
        u32 p = *pi;
        while (p < len && dg_is_space(s[p])) p++;
        if (p < len && s[p] == '&') { *pi = p + 1; continue; }
        break;
    }
    return n;
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
    } else if ((s[i] == 'x' || s[i] == 'o') && i + 2 < len && s[i+1] == '-' && s[i+2] == '-') {
        *mk_from = s[i] == 'x' ? MK_CROSS : MK_CIRCLE;
        i++;
    }
    if (i >= len || (s[i] != '-' && s[i] != '=' && s[i] != '~')) return false;

    if (s[i] == '~') {                                  /* invisible link */
        u32 run = 0;
        while (i < len && s[i] == '~') { i++; run++; }
        if (run < 3) return false;
        *style = ES_DOTTED;
    } else if (s[i] == '=') {
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

    /* `x` and `o` end a link only when no id follows them directly: in
     * `A --> xray` the x is the next node's first letter. */
    if (i < len && s[i] == '>')      { *mk_to = MK_ARROW; i++; }
    else if (i < len && (s[i] == 'x' || s[i] == 'o') &&
             (i + 1 >= len || !dg_is_word(s[i+1]))) {
        *mk_to = s[i] == 'x' ? MK_CROSS : MK_CIRCLE;
        i++;
    }
    *pi = i;
    return true;
}

/* `subgraph id [Title]` / `subgraph id["Title"]` / `subgraph Title`. Returns
 * the new group or the parent when the cap is hit. */
i16 dg_open_group(const u8 *line, u32 ll, u32 kwlen, i16 parent) {
    u32 p = kwlen;
    while (p < ll && dg_is_space(line[p])) p++;
    if (g_ngroups >= DG_MAX_GROUPS) { g_trunc = true; return parent; }
    DGroup *g = &g_groups[g_ngroups];
    memset(g, 0, sizeof *g);
    g->parent = parent;
    u32 ids = p;
    u32 ide = dg_scan_id(line, ll, p);
    if (p < ll && line[p] == '"') {                 /* `subgraph "Title"` */
        dg_set_label(g->label, sizeof g->label, line + p, ll - p);
        dg_strcpy(g->id, sizeof g->id, (const u8 *)g->label, (u32)strlen(g->label));
        return (i16)g_ngroups++;
    }
    dg_strcpy(g->id, sizeof g->id, line + ids, ide - ids);
    u32 q = ide;
    while (q < ll && dg_is_space(line[q])) q++;
    if (q < ll && (line[q] == '[' || line[q] == '(')) {
        char close = line[q] == '[' ? ']' : ')';
        u32 ts = q + 1;
        u32 te = dg_find_close(line, ll, ts, close == ']' ? "]" : ")");
        dg_set_label(g->label, sizeof g->label, line + ts, (te < ll ? te : ll) - ts);
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
            i16 g = dg_open_group(line, ll, 8, parent);
            if (depth < DG_MAX_DEPTH) stack[depth++] = g;
            continue;
        }
        if (dg_kw(line, ll, "end")) { if (depth) depth--; continue; }
        if (dg_kw(line, ll, "direction")) {
            /* A subgraph's own direction is not modelled; the graph's is. */
            if (depth) continue;
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
            dg_kw(line, ll, "accDescr") || dg_kw(line, ll, "title"))
            continue;

        i16 group = depth ? stack[depth-1] : (i16)-1;
        u32 p = 0;
        /* One line may hold several statements separated by `;`. */
        for (u32 stmt = 0; stmt < DG_MAX_LINKS && p < ll; stmt++) {
            i32 prev[DG_MAX_FAN];
            u32 np = dg_parse_refs(line, ll, &p, group, prev);
            if (!np) break;
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
                i32 cur[DG_MAX_FAN];
                u32 nc = dg_parse_refs(line, ll, &p, group, cur);
                if (!nc) break;
                for (u32 a = 0; a < np; a++)
                    for (u32 b = 0; b < nc; b++)
                        dg_add_edge(prev[a], cur[b], style, mkf, mkt, elabel);
                memcpy(prev, cur, sizeof(i32) * nc);
                np = nc;
            }
            while (p < ll && dg_is_space(line[p])) p++;
            if (p < ll && line[p] == ';') { p++; continue; }
            break;
        }
    }
}

/* ---- state diagrams ---- */

/* `[*]` is a marker, not an id: one start dot and one end ring per scope. */
static i32 dg_state_ref(const u8 *s, u32 len, u32 *pi, bool as_target, i16 group) {
    u32 i = *pi;
    while (i < len && dg_is_space(s[i])) i++;
    if (i + 2 < len && s[i] == '[' && s[i+1] == '*' && s[i+2] == ']') {
        *pi = i + 3;
        char key[16];
        key[0] = '\x01';
        key[1] = as_target ? 'e' : 's';
        key[2] = (char)('A' + (group + 1));
        key[3] = 0;
        i32 idx = dg_intern((const u8 *)key, 3, group);
        if (idx >= 0) {
            g_nodes[idx].shape = as_target ? SH_RING : SH_DOT;
            g_nodes[idx].label[0] = 0;
            g_nodes[idx].declared = true;
        }
        return idx;
    }
    u32 ids = i;
    i = dg_scan_id(s, len, i);
    *pi = i;
    if (i == ids) return -1;
    i32 idx = dg_intern(s + ids, i - ids, group);
    if (idx >= 0 && g_nodes[idx].shape == SH_RECT) g_nodes[idx].shape = SH_ROUND;
    return idx;
}

void dg_parse_state(DScan *sc) {
    const u8 *line; u32 ll;
    bool in_note = false;
    i16 stack[DG_MAX_DEPTH];
    u32 depth = 0;
    while (dg_next_line(sc, &line, &ll)) {
        i16 group = depth ? stack[depth-1] : (i16)-1;
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
            if (depth) continue;
            u32 p = 9;
            while (p < ll && dg_is_space(line[p])) p++;
            if (p + 1 < ll && line[p] == 'L' && line[p+1] == 'R') g_dir = 1;
            else g_dir = 0;
            continue;
        }
        if (dg_kw(line, ll, "classDef") || dg_kw(line, ll, "class") ||
            dg_kw(line, ll, "style") || dg_kw(line, ll, "accTitle") ||
            dg_kw(line, ll, "accDescr") || dg_kw(line, ll, "title"))
            continue;
        if (ll == 1 && line[0] == '}') { if (depth) depth--; continue; }
        if (ll == 1 && line[0] == '{') continue;
        if (ll >= 2 && line[0] == '-' && line[1] == '-' &&
            (ll == 2 || line[2] == '-')) continue;              /* concurrency */

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
            u32 q = ide;
            while (q < ll && dg_is_space(line[q])) q++;
            if (q < ll && line[q] == '{') {          /* composite state */
                i16 g = dg_open_group(line, ll, 5, group);
                if (g != group) {
                    dg_strcpy(g_groups[g].id, sizeof g_groups[g].id, line + ids, ide - ids);
                    if (desc[0]) dg_strcpy(g_groups[g].label, sizeof g_groups[g].label,
                                           (const u8 *)desc, (u32)strlen(desc));
                    else dg_strcpy(g_groups[g].label, sizeof g_groups[g].label,
                                   line + ids, ide - ids);
                }
                if (depth < DG_MAX_DEPTH) stack[depth++] = g;
                continue;
            }
            i32 idx = dg_intern(line + ids, ide - ids, group);
            if (idx < 0) continue;
            if (g_nodes[idx].shape == SH_RECT) g_nodes[idx].shape = SH_ROUND;
            if (desc[0]) {
                dg_strcpy(g_nodes[idx].label, sizeof g_nodes[idx].label,
                          (const u8 *)desc, (u32)strlen(desc));
                g_nodes[idx].declared = true;
            }
            if (q + 1 < ll && line[q] == '<' && line[q+1] == '<') {
                bool choice = (q + 8 <= ll && memcmp(line + q + 2, "choice", 6) == 0);
                g_nodes[idx].shape = choice ? SH_DIAMOND : SH_BAR;
                g_nodes[idx].label[0] = 0;
                g_nodes[idx].declared = true;
            }
            continue;
        }

        u32 p = 0;
        i32 from = dg_state_ref(line, ll, &p, false, group);
        if (from < 0) continue;
        u8 style, mkf, mkt;
        char elabel[DG_ELABEL_CAP];
        u32 before = p;
        if (!dg_parse_link(line, ll, &p, &style, &mkf, &mkt, elabel, sizeof elabel)) {
            /* `Idle : waiting for input` describes a state. */
            p = before;
            while (p < ll && dg_is_space(line[p])) p++;
            if (p < ll && line[p] == ':' && g_nodes[from].shape != SH_DOT) {
                dg_set_label(g_nodes[from].label, sizeof g_nodes[from].label,
                             line + p + 1, ll - p - 1);
                g_nodes[from].declared = true;
            }
            continue;
        }
        i32 to = dg_state_ref(line, ll, &p, true, group);
        if (to < 0) continue;
        while (p < ll && dg_is_space(line[p])) p++;
        if (p < ll && line[p] == ':')
            dg_set_label(elabel, sizeof elabel, line + p + 1, ll - p - 1);
        dg_add_edge(from, to, style, mkf, mkt ? mkt : (u8)MK_ARROW, elabel);
    }
}

/* ---- class diagrams ---- */

/* Append one member line to a class node's compartments: methods (a `(` in
 * the line) to the second, everything else to the first; `<<x>>` is the
 * class's stereotype, shown over its name. */
static void dg_class_member(i32 idx, const u8 *s, u32 n) {
    if (idx < 0) return;
    while (n && dg_is_space(s[0])) { s++; n--; }
    while (n && dg_is_space(s[n-1])) n--;
    if (!n) return;
    DNode *nd = &g_nodes[idx];
    if (n > 4 && s[0] == '<' && s[1] == '<') {
        char st[DG_ELABEL_CAP], lab[DG_LABEL_CAP];
        u32 e = 2;
        while (e < n && s[e] != '>') e++;
        dg_set_label(st, sizeof st, s + 2, e - 2);
        snprintf(lab, sizeof lab, "\xC2\xAB%s\xC2\xBB\n%s", st, nd->label);
        memcpy(nd->label, lab, sizeof nd->label);
        nd->label[sizeof nd->label - 1] = 0;
        return;
    }
    bool method = false;
    for (u32 k = 0; k < n; k++) if (s[k] == '(') { method = true; break; }
    char *body = method ? nd->body2 : nd->body;
    u32 have = (u32)strlen(body);
    if (have + 2 >= DG_BODY_CAP) return;             /* compartment is full */
    u32 have_rows = 0;
    for (u32 i = 0; i < have; i++) if (body[i] == '\n') have_rows++;
    if (have_rows + 1 >= DG_MAX_ROWS) return;
    char row[DG_ELABEL_CAP];
    /* `~T~` is a generic parameter: List~int~ reads List<int>. */
    char tmp[DG_ELABEL_CAP];
    u32 tn = 0;
    for (u32 k = 0; k < n && tn + 1 < sizeof tmp; k++) tmp[tn++] = (char)s[k];
    tmp[tn] = 0;
    bool open = true;
    for (u32 k = 0; k < tn; k++) if (tmp[k] == '~') { tmp[k] = open ? '<' : '>'; open = !open; }
    dg_set_label(row, sizeof row, (const u8 *)tmp, tn);
    if (!row[0]) return;
    u32 rl = (u32)strlen(row);
    if (have) body[have++] = '\n';
    if (have + rl >= DG_BODY_CAP) rl = DG_BODY_CAP - have - 1;
    memcpy(body + have, row, rl);
    body[have + rl] = 0;
}

/* `Name~T~` — the id ends at the tilde; the label shows `Name<T>`. */
static void dg_class_generic(i32 idx, const u8 *s, u32 len, u32 *pi) {
    u32 p = *pi;
    if (p >= len || s[p] != '~') return;
    u32 e = p + 1;
    while (e < len && s[e] != '~') e++;
    if (e >= len) return;
    if (idx >= 0) {
        char lab[DG_LABEL_CAP];
        int n = snprintf(lab, sizeof lab, "%s<%.*s>", g_nodes[idx].id, (int)(e - p - 1),
                         (const char *)s + p + 1);
        if (n > 0) dg_set_label(g_nodes[idx].label, sizeof g_nodes[idx].label,
                                (const u8 *)lab, (u32)strlen(lab));
    }
    *pi = e + 1;
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
    if (p < ll && line[p] == '~') { while (++p < ll && line[p] != '~') {} if (p < ll) p++; }
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
    if (p < ll && line[p] == '~') { while (++p < ll && line[p] != '~') {} if (p < ll) p++; }
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

void dg_parse_class(DScan *sc) {
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
            dg_kw(line, ll, "accDescr") || dg_kw(line, ll, "namespace") ||
            dg_kw(line, ll, "title"))
            continue;
        if (ll == 1 && (line[0] == '}' || line[0] == '{')) continue;

        /* `<<interface>> Animal` */
        if (ll > 4 && line[0] == '<' && line[1] == '<') {
            u32 e = 2;
            while (e + 1 < ll && !(line[e] == '>' && line[e+1] == '>')) e++;
            u32 p = e + 2;
            while (p < ll && dg_is_space(line[p])) p++;
            u32 ids = p, ide = dg_scan_id(line, ll, p);
            if (ide == ids) continue;
            i32 idx = dg_intern(line + ids, ide - ids, -1);
            if (idx < 0) continue;
            g_nodes[idx].shape = SH_CLASS;
            dg_class_member(idx, line, e + 2);
            continue;
        }

        if (dg_kw(line, ll, "class")) {
            u32 p = 5;
            while (p < ll && dg_is_space(line[p])) p++;
            u32 ids = p, ide = dg_scan_id(line, ll, p);
            if (ide == ids) continue;
            i32 idx = dg_intern(line + ids, ide - ids, -1);
            if (idx < 0) continue;
            g_nodes[idx].shape = SH_CLASS;
            p = ide;
            dg_class_generic(idx, line, ll, &p);
            while (p < ll && dg_is_space(line[p])) p++;
            if (p < ll && line[p] == '[') {          /* class A["Label"] */
                u32 te = dg_find_close(line, ll, p + 1, "]");
                dg_set_label(g_nodes[idx].label, sizeof g_nodes[idx].label,
                             line + p + 1, (te < ll ? te : ll) - p - 1);
                p = te < ll ? te + 1 : ll;
                while (p < ll && dg_is_space(line[p])) p++;
            }
            if (p < ll && line[p] == '{') {
                open = idx;
                /* `class A { +x }` on one line */
                u32 cl = p + 1;
                while (cl < ll && line[cl] != '}') cl++;
                if (cl < ll) {
                    dg_class_member(idx, line + p + 1, cl - p - 1);
                    open = -1;
                }
                continue;
            }
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

/* ---- entity-relationship diagrams ---- */

/* `||` `|o` `}|` `}o` on the left of the line, `||` `o|` `|{` `o{` on the
 * right. Returns the marker and advances, or MK_NONE without moving. */
static u8 dg_er_card(const u8 *s, u32 len, u32 *pi, bool right) {
    u32 i = *pi;
    if (i + 1 >= len) return MK_NONE;
    u8 a = s[i], b = s[i+1];
    u8 mk = MK_NONE;
    if (!right) {
        if (a == '|' && b == '|') mk = MK_ER_ONE;
        else if (a == '|' && b == 'o') mk = MK_ER_ZERO_ONE;
        else if (a == '}' && b == '|') mk = MK_ER_MANY;
        else if (a == '}' && b == 'o') mk = MK_ER_ZERO_MANY;
    } else {
        if (a == '|' && b == '|') mk = MK_ER_ONE;
        else if (a == 'o' && b == '|') mk = MK_ER_ZERO_ONE;
        else if (a == '|' && b == '{') mk = MK_ER_MANY;
        else if (a == 'o' && b == '{') mk = MK_ER_ZERO_MANY;
    }
    if (mk) *pi = i + 2;
    return mk;
}

static void dg_er_attr(i32 idx, const u8 *s, u32 n) {
    if (idx < 0) return;
    DNode *nd = &g_nodes[idx];
    /* type name [PK|FK|UK[, …]] ["comment"] — the comment is dropped. */
    char row[DG_ELABEL_CAP];
    u32 o = 0;
    bool q = false;
    for (u32 i = 0; i < n && o + 1 < sizeof row; i++) {
        if (s[i] == '"') { q = !q; continue; }
        if (q) continue;
        row[o++] = (char)s[i];
    }
    row[o] = 0;
    char clean[DG_ELABEL_CAP];
    dg_set_label(clean, sizeof clean, (const u8 *)row, o);
    if (!clean[0]) return;
    u32 have = (u32)strlen(nd->body);
    u32 have_rows = 0;
    for (u32 i = 0; i < have; i++) if (nd->body[i] == '\n') have_rows++;
    if (have_rows + 1 >= DG_MAX_ROWS) return;
    u32 rl = (u32)strlen(clean);
    if (have + rl + 2 >= DG_BODY_CAP) return;
    if (have) nd->body[have++] = '\n';
    memcpy(nd->body + have, clean, rl + 1);
}

static void dg_parse_er(DScan *sc) {
    const u8 *line; u32 ll;
    i32 open = -1;
    while (dg_next_line(sc, &line, &ll)) {
        if (open >= 0) {
            if (line[0] == '}') { open = -1; continue; }
            dg_er_attr(open, line, ll);
            continue;
        }
        if (dg_kw(line, ll, "direction")) {
            u32 p = 9;
            while (p < ll && dg_is_space(line[p])) p++;
            if (p + 1 < ll && line[p] == 'L' && line[p+1] == 'R') g_dir = 1;
            else g_dir = 0;
            continue;
        }
        if (dg_kw(line, ll, "title") || dg_kw(line, ll, "accTitle") ||
            dg_kw(line, ll, "accDescr") || dg_kw(line, ll, "style") ||
            dg_kw(line, ll, "classDef") || dg_kw(line, ll, "class"))
            continue;
        u32 p = 0;
        u32 ids = p, ide = dg_scan_id(line, ll, p);
        if (ide == ids) continue;
        p = ide;
        char alias[DG_LABEL_CAP]; alias[0] = 0;
        if (p < ll && line[p] == '[') {                 /* ENTITY["Shown name"] */
            u32 te = dg_find_close(line, ll, p + 1, "]");
            dg_set_label(alias, sizeof alias, line + p + 1, (te < ll ? te : ll) - p - 1);
            p = te < ll ? te + 1 : ll;
        }
        while (p < ll && dg_is_space(line[p])) p++;
        if (p < ll && line[p] == '{') {
            i32 idx = dg_intern(line + ids, ide - ids, -1);
            if (idx < 0) continue;
            g_nodes[idx].shape = SH_ENTITY;
            if (alias[0]) memcpy(g_nodes[idx].label, alias, sizeof g_nodes[idx].label);
            open = idx;
            u32 cl = p + 1;
            while (cl < ll && line[cl] != '}') cl++;
            if (cl < ll) open = -1;
            continue;
        }
        if (p >= ll) {
            i32 idx = dg_intern(line + ids, ide - ids, -1);
            if (idx >= 0) g_nodes[idx].shape = SH_ENTITY;
            continue;
        }
        u8 mk_from = dg_er_card(line, ll, &p, false);
        if (!mk_from) continue;
        if (p + 1 >= ll) continue;
        u8 style;
        if (line[p] == '-' && line[p+1] == '-') style = ES_SOLID;
        else if (line[p] == '.' && line[p+1] == '.') style = ES_DOTTED;
        else continue;
        p += 2;
        u8 mk_to = dg_er_card(line, ll, &p, true);
        if (!mk_to) continue;
        while (p < ll && dg_is_space(line[p])) p++;
        u32 tds = p, tde = dg_scan_id(line, ll, p);
        if (tde == tds) continue;
        p = tde;
        while (p < ll && dg_is_space(line[p])) p++;
        char elabel[DG_ELABEL_CAP]; elabel[0] = 0;
        if (p < ll && line[p] == ':')
            dg_set_label(elabel, sizeof elabel, line + p + 1, ll - p - 1);
        i32 a = dg_intern(line + ids, ide - ids, -1);
        i32 b = dg_intern(line + tds, tde - tds, -1);
        if (a < 0 || b < 0) continue;
        g_nodes[a].shape = SH_ENTITY;
        g_nodes[b].shape = SH_ENTITY;
        dg_add_edge(a, b, style, mk_from, mk_to, elabel);
    }
}

/* ---- pie ---- */
static void dg_parse_pie(DScan *sc, const u8 *head, u32 hl) {
    /* `pie showData title Pets` — both modifiers may sit on the header. */
    u32 p = 3;
    while (p < hl) {
        while (p < hl && dg_is_space(head[p])) p++;
        if (dg_kw(head + p, hl - p, "showData")) { g_show_data = true; p += 8; continue; }
        if (dg_kw(head + p, hl - p, "title")) {
            dg_set_label(g_title, sizeof g_title, head + p + 5, hl - p - 5);
            break;
        }
        break;
    }
    const u8 *line; u32 ll;
    while (dg_next_line(sc, &line, &ll)) {
        if (dg_kw(line, ll, "title")) {
            dg_set_label(g_title, sizeof g_title, line + 5, ll - 5);
            continue;
        }
        if (dg_kw(line, ll, "showData")) { g_show_data = true; continue; }
        if (dg_kw(line, ll, "accTitle") || dg_kw(line, ll, "accDescr")) continue;
        if (line[0] != '"') continue;
        u32 e = 1;
        while (e < ll && line[e] != '"') e++;
        if (e >= ll) continue;
        u32 q = e + 1;
        while (q < ll && dg_is_space(line[q])) q++;
        if (q >= ll || line[q] != ':') continue;
        q++;
        while (q < ll && dg_is_space(line[q])) q++;
        char num[32];
        u32 nl = 0;
        while (q < ll && nl + 1 < sizeof num &&
               ((line[q] >= '0' && line[q] <= '9') || line[q] == '.')) num[nl++] = (char)line[q++];
        num[nl] = 0;
        if (!nl) continue;
        f64 v = strtod(num, NULL);
        if (!(v > 0.0) || !isfinite(v)) continue;
        if (g_nslices >= DG_MAX_SLICES) { g_trunc = true; break; }
        DSlice *sl = &g_slices[g_nslices++];
        dg_set_label(sl->label, sizeof sl->label, line + 1, e - 1);
        sl->value = v;
    }
}

/* ---- sequence diagrams ---- */
i32 dg_part(const u8 *s, u32 n) {
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
    dg_set_label(p->label, sizeof p->label, (const u8 *)key, k);
    return (i32)g_nparts++;
}

DMsg *dg_add_msg(u8 kind) {
    if (g_nmsgs >= DG_MAX_MSGS) { g_trunc = true; return NULL; }
    DMsg *m = &g_msgs[g_nmsgs++];
    memset(m, 0, sizeof *m);
    m->kind = kind;
    m->from = m->to = m->span = -1;
    if (kind == SQ_MSG) g_nreal_msgs++;
    return m;
}

/* `A->>+B: text` — arrows are `->` `-->` (no head), `->>` `-->>` (head),
 * `-x` `--x` (cross), `-)` `--)` (open async head), and a leading `<<` for
 * both ends. */
static bool dg_parse_seq_msg(const u8 *line, u32 ll, u16 *autonum) {
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
        u32 heads = 0;
        while (p < ll && line[p] == '>') { p++; heads++; }
        mk_to = heads >= 2 ? MK_ARROW : MK_NONE;
    } else if (p < ll && line[p] == 'x') { p++; mk_to = MK_CROSS; }
    else if (p < ll && line[p] == ')')   { p++; mk_to = MK_OPEN; }
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
    if (*autonum) m->number = (*autonum)++;
    if (p < ll) dg_set_label(m->label, sizeof m->label, line + p + 1, ll - p - 1);
    return true;
}

static void dg_parse_seq(DScan *sc) {
    const u8 *line; u32 ll;
    u16 autonum = 0;
    /* What each open `end` closes: 1 a drawn frame, 0 a box of participants. */
    u8  open[DG_MAX_DEPTH * 4];
    u32 depth = 0;
    while (dg_next_line(sc, &line, &ll)) {
        bool create = dg_kw(line, ll, "create");
        if (create) {
            u32 p = 6;
            while (p < ll && dg_is_space(line[p])) p++;
            line += p; ll -= p;
        }
        if (dg_kw(line, ll, "participant") || dg_kw(line, ll, "actor")) {
            bool actor = line[0] == 'a';
            u32 p = actor ? 5u : 11u;
            while (p < ll && dg_is_space(line[p])) p++;
            u32 ids = p;
            while (p < ll && !dg_is_space(line[p])) p++;
            u32 ide = p;
            if (ide == ids) continue;
            i32 idx = dg_part(line + ids, ide - ids);
            if (idx < 0) continue;
            g_parts[idx].actor = actor;
            while (p < ll && dg_is_space(line[p])) p++;
            if (dg_kw(line + p, ll - p, "as")) {
                p += 2;
                dg_set_label(g_parts[idx].label, sizeof g_parts[idx].label,
                             line + p, ll - p);
            }
            continue;
        }
        if (dg_kw(line, ll, "destroy")) continue;
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
        if (dg_kw(line, ll, "autonumber")) {
            u32 p = 10;
            while (p < ll && dg_is_space(line[p])) p++;
            if (dg_kw(line + p, ll - p, "off")) { autonum = 0; continue; }
            u32 v = 0;
            while (p < ll && line[p] >= '0' && line[p] <= '9' && v < 10000)
                v = v * 10u + (u32)(line[p++] - '0');
            autonum = (u16)(v ? v : 1);
            continue;
        }
        static const char *frames[] = {
            "loop", "alt", "opt", "par", "critical", "break", "rect", NULL
        };
        bool framed = false;
        for (u32 k = 0; frames[k]; k++) {
            if (!dg_kw(line, ll, frames[k])) continue;
            u32 kl = (u32)strlen(frames[k]);
            DMsg *m = dg_add_msg(SQ_FRAME_OPEN);
            if (m) {
                dg_strcpy(m->label, sizeof m->label, (const u8 *)frames[k], kl);
                /* The condition rides in the note-span slot's text: kept in
                 * a second field to avoid a struct of its own. */
                if (k != 6) {                             /* rect: a colour */
                    char cond[DG_ELABEL_CAP];
                    dg_set_label(cond, sizeof cond, line + kl, ll - kl);
                    if (cond[0]) {
                        u32 have = (u32)strlen(m->label);
                        if (have + 2 + strlen(cond) < sizeof m->label) {
                            m->label[have] = '\n';
                            memcpy(m->label + have + 1, cond, strlen(cond) + 1);
                        }
                    }
                }
            }
            if (depth < sizeof open) open[depth++] = 1;
            framed = true;
            break;
        }
        if (framed) continue;
        if (dg_kw(line, ll, "box")) {
            if (depth < sizeof open) open[depth++] = 0;
            continue;
        }
        if (dg_kw(line, ll, "else") || dg_kw(line, ll, "and") || dg_kw(line, ll, "option")) {
            u32 kl = line[0] == 'e' ? 4u : (line[0] == 'a' ? 3u : 6u);
            DMsg *m = dg_add_msg(SQ_FRAME_DIV);
            if (m) dg_set_label(m->label, sizeof m->label, line + kl, ll - kl);
            continue;
        }
        if (dg_kw(line, ll, "end")) {
            if (depth) {
                depth--;
                if (open[depth]) dg_add_msg(SQ_FRAME_CLOSE);
            }
            continue;
        }
        if (dg_kw(line, ll, "title") || dg_kw(line, ll, "accTitle") ||
            dg_kw(line, ll, "accDescr") || dg_kw(line, ll, "links") ||
            dg_kw(line, ll, "link") || dg_kw(line, ll, "properties") ||
            dg_kw(line, ll, "details"))
            continue;
        dg_parse_seq_msg(line, ll, &autonum);
    }
    while (depth) { depth--; if (open[depth]) dg_add_msg(SQ_FRAME_CLOSE); }
}

/* ---- build ---- */

/* Header keyword → kind, leaving the scanner on the first body line. Skips a
 * YAML frontmatter block and any leading `%%{init}%%` directive. */
static MdDiagramKind dg_header(DScan *sc, const u8 **hline, u32 *hlen) {
    const u8 *line; u32 ll;
    if (!dg_next_line(sc, &line, &ll)) return MD_DIAGRAM_NONE;
    if (ll >= 3 && dg_starts(line, ll, "---")) {         /* frontmatter */
        bool closed = false;
        while (dg_next_line(sc, &line, &ll)) {
            if (dg_starts(line, ll, "---")) { closed = true; break; }
        }
        if (!closed || !dg_next_line(sc, &line, &ll)) return MD_DIAGRAM_NONE;
    }
    *hline = line; *hlen = ll;
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
    if (dg_kw(line, ll, "classDiagram") || dg_kw(line, ll, "classDiagram-v2"))
        return MD_DIAGRAM_CLASS;
    if (dg_kw(line, ll, "erDiagram")) return MD_DIAGRAM_ER;
    if (dg_kw(line, ll, "pie")) return MD_DIAGRAM_PIE;
    if (dg_kw(line, ll, "gantt")) return MD_DIAGRAM_GANTT;
    if (dg_kw(line, ll, "mindmap")) return MD_DIAGRAM_MINDMAP;
    if (dg_kw(line, ll, "timeline")) return MD_DIAGRAM_TIMELINE;
    if (dg_kw(line, ll, "gitGraph") || dg_starts(line, ll, "gitGraph:")) return MD_DIAGRAM_GITGRAPH;
    if (dg_kw(line, ll, "xychart-beta") || dg_kw(line, ll, "xychart")) return MD_DIAGRAM_XYCHART;
    if (dg_kw(line, ll, "quadrantChart")) return MD_DIAGRAM_QUADRANT;
    if (dg_kw(line, ll, "journey")) return MD_DIAGRAM_JOURNEY;
    return MD_DIAGRAM_NONE;
}

/* Subgraphs a link names: a node that was never given a shape or label of its
 * own and shares a subgraph's id IS that subgraph in a link (`A --> Cluster`),
 * as long as the subgraph holds something to point at. */
static void dg_resolve_proxies(void) {
    for (u32 i = 0; i < g_nnodes; i++) {
        g_nodes[i].proxy = -1;
        if (g_nodes[i].declared) continue;
        for (u32 g = 0; g < g_ngroups; g++) {
            if (!g_groups[g].used || !g_groups[g].id[0]) continue;
            if (strcmp(g_groups[g].id, g_nodes[i].id) != 0) continue;
            /* A node inside the very subgraph it names is a member. */
            bool inside = false;
            for (i32 k = g_nodes[i].group, guard = 0; k >= 0 && guard < DG_MAX_GROUPS;
                 guard++, k = g_groups[k].parent)
                if (k == (i32)g) { inside = true; break; }
            if (inside) continue;
            g_nodes[i].proxy = (i16)g;
            break;
        }
    }
}

/* Parse `src` into the model. Returns the kind, or NONE when the header is
 * unknown, a cap was hit, or the body produced nothing drawable. */
/* The language of the diagram being built, and the Mermaid a source in
 * another language was translated into. */
static MdDiagramLang g_lang;
static char g_xlat[DG_MAX_SRC + 1];

static MdDiagramKind dg_build(const u8 *src, u32 len) {
    dg_reset();
    if (!src || len == 0 || len > DG_MAX_SRC) return MD_DIAGRAM_NONE;
    g_lang = dgl_lang(src, len);
    if (g_lang != MD_DIAGRAM_LANG_MERMAID) {
        u32 xl = 0;
        if (!dgl_translate(src, len, g_lang, g_xlat, sizeof g_xlat, &xl)) return MD_DIAGRAM_NONE;
        src = (const u8 *)g_xlat;
        len = xl;
    }
    DScan sc = { src, len, 0, 0 };
    const u8 *hl = NULL; u32 hn = 0;
    MdDiagramKind kind = dg_header(&sc, &hl, &hn);
    switch (kind) {
    case MD_DIAGRAM_FLOWCHART: dg_parse_flow(&sc);  break;
    case MD_DIAGRAM_SEQUENCE:  dg_parse_seq(&sc);   break;
    case MD_DIAGRAM_STATE:     dg_parse_state(&sc); break;
    case MD_DIAGRAM_CLASS:     dg_parse_class(&sc); break;
    case MD_DIAGRAM_ER:        dg_parse_er(&sc);    break;
    case MD_DIAGRAM_PIE:       dg_parse_pie(&sc, hl, hn); break;
    case MD_DIAGRAM_GANTT: case MD_DIAGRAM_MINDMAP: case MD_DIAGRAM_TIMELINE:
    case MD_DIAGRAM_GITGRAPH: case MD_DIAGRAM_XYCHART: case MD_DIAGRAM_QUADRANT:
    case MD_DIAGRAM_JOURNEY:
        return dgm_parse(kind, &sc, hl, hn) && !g_trunc ? kind : MD_DIAGRAM_NONE;
    default: return MD_DIAGRAM_NONE;
    }
    /* A recognised header over an unparseable body must not render as an empty
     * box — the raw fence is strictly more useful than that. */
    if (g_trunc) return MD_DIAGRAM_NONE;
    if (kind == MD_DIAGRAM_SEQUENCE)
        return (g_nparts > 0 && g_nreal_msgs > 0) ? kind : MD_DIAGRAM_NONE;
    if (kind == MD_DIAGRAM_PIE)
        return g_nslices > 0 ? kind : MD_DIAGRAM_NONE;
    dg_resolve_proxies();
    for (u32 i = 0; i < g_nnodes; i++)
        if (g_nodes[i].proxy < 0) return kind;
    return MD_DIAGRAM_NONE;
}

/* =========================================================================
 * The draw list
 *
 * Everything is emitted in diagram space at scale 1 into scratch arrays, then
 * moved so the drawing starts at the margin, sorted by pass and copied into
 * one MdDiagram allocation. A pass draws its boxes, then its stroke segments;
 * glyphs come after every pass.
 * ========================================================================= */



typedef struct { f32 x, y, w, h, radius, border; u8 fill, stroke, pass; } DPBox;
typedef struct {
    f32 x0, y0, x1, y1;
    f32 nx0, ny0, nx1, ny1;     /* across-direction at each end (miter scaled) */
    f32 width;
    u8  ink, pass;
} DPStroke;
typedef struct { f32 x, y; u32 cp; u8 ink, wide; } DPGlyph;

struct MdDiagram {
    MdDiagramKind kind;
    MdDiagramLang lang;
    f32 w, h, cw, ch;
    u32 nbox, nstroke, nglyph;
    DPBox    *box;
    DPStroke *str;
    DPGlyph  *gly;
    u32 box_at[DG_PASSES + 1], str_at[DG_PASSES + 1];
    usize bytes;
};

#define DG_MAX_BOX    4096u
#define DG_MAX_STROKE 49152u
#define DG_MAX_GLYPH  32768u

static DPBox    *e_box;   static u32 e_nbox, e_cbox;
static DPStroke *e_str;   static u32 e_nstr, e_cstr;
static DPGlyph  *e_gly;   static u32 e_ngly, e_cgly;

static void em_reset(void) { e_nbox = e_nstr = e_ngly = 0; }

static bool em_grow(void **buf, u32 *cap, u32 need, u32 max, usize item) {
    if (need <= *cap) return true;
    if (need > max) { g_trunc = true; return false; }
    u32 nc = *cap ? *cap * 2u : 256u;
    while (nc < need) nc *= 2u;
    if (nc > max) nc = max;
    void *nb = realloc(*buf, (usize)nc * item);
    if (!nb) { g_trunc = true; return false; }
    *buf = nb;
    *cap = nc;
    return true;
}

void em_box(u8 pass, f32 x, f32 y, f32 w, f32 h, f32 radius, f32 border,
                   u8 fill, u8 stroke) {
    if (!(w > 0.0f) || !(h > 0.0f) || !isfinite(x) || !isfinite(y)) return;
    if (!em_grow((void **)&e_box, &e_cbox, e_nbox + 1, DG_MAX_BOX, sizeof *e_box)) return;
    f32 rmax = fminf(w, h) * 0.5f;
    e_box[e_nbox++] = (DPBox){ x, y, w, h, radius < 0 ? 0 : fminf(radius, rmax),
                               border, fill, stroke, pass };
}

void em_stroke(u8 pass, f32 x0, f32 y0, f32 x1, f32 y1,
                      f32 nx0, f32 ny0, f32 nx1, f32 ny1, f32 width, u8 ink) {
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return;
    if (!em_grow((void **)&e_str, &e_cstr, e_nstr + 1, DG_MAX_STROKE, sizeof *e_str)) return;
    e_str[e_nstr++] = (DPStroke){ x0, y0, x1, y1, nx0, ny0, nx1, ny1, width, ink, pass };
}

/* A straight segment with square ends. */
void em_seg(u8 pass, f32 x0, f32 y0, f32 x1, f32 y1, f32 width, u8 ink) {
    f32 dx = x1 - x0, dy = y1 - y0, d = sqrtf(dx*dx + dy*dy);
    if (!(d > 1e-4f)) return;
    f32 nx = -dy / d, ny = dx / d;
    em_stroke(pass, x0, y0, x1, y1, nx, ny, nx, ny, width, ink);
}

/* A polyline with mitred joints (the miter bounded at 3x), so a curve made of
 * many segments reads as one ribbon. `closed` joins the end to the start. */
void em_poly(u8 pass, const f32 *px, const f32 *py, u32 n, bool closed,
                    f32 width, u8 ink) {
    if (n < 2) return;
    enum { MAXP = 512 };
    if (n > MAXP) n = MAXP;
    f32 mx[MAXP], my[MAXP];
    for (u32 i = 0; i < n; i++) {
        bool has_prev = closed || i > 0, has_next = closed || i + 1 < n;
        u32 ip = (i + n - 1) % n, in = (i + 1) % n;
        f32 n0x = 0, n0y = 0, n1x = 0, n1y = 0;
        if (has_prev) {
            f32 dx = px[i] - px[ip], dy = py[i] - py[ip], d = sqrtf(dx*dx + dy*dy);
            if (d > 1e-4f) { n0x = -dy / d; n0y = dx / d; } else has_prev = false;
        }
        if (has_next) {
            f32 dx = px[in] - px[i], dy = py[in] - py[i], d = sqrtf(dx*dx + dy*dy);
            if (d > 1e-4f) { n1x = -dy / d; n1y = dx / d; } else has_next = false;
        }
        if (!has_prev) { n0x = n1x; n0y = n1y; }
        if (!has_next) { n1x = n0x; n1y = n0y; }
        f32 ax = n0x + n1x, ay = n0y + n1y, al = sqrtf(ax*ax + ay*ay);
        if (!(al > 1e-4f)) { mx[i] = n1x; my[i] = n1y; continue; }
        ax /= al; ay /= al;
        f32 c = ax * n1x + ay * n1y;
        f32 sc = c > 0.34f ? 1.0f / c : 3.0f;
        mx[i] = ax * sc; my[i] = ay * sc;
    }
    u32 segs = closed ? n : n - 1;
    for (u32 i = 0; i < segs; i++) {
        u32 j = (i + 1) % n;
        if (fabsf(px[j] - px[i]) + fabsf(py[j] - py[i]) < 1e-4f) continue;
        em_stroke(pass, px[i], py[i], px[j], py[j], mx[i], my[i], mx[j], my[j], width, ink);
    }
}

/* A dashed run along a polyline: `dash` on, `gap` off, by arc length. */
void em_dashed(u8 pass, const f32 *px, const f32 *py, u32 n, f32 width,
                      f32 dash, f32 gap, u8 ink) {
    if (n < 2 || !(dash > 0.5f)) return;
    f32 phase = 0;          /* distance into the current on/off cycle */
    bool on = true;
    for (u32 i = 0; i + 1 < n; i++) {
        f32 dx = px[i+1] - px[i], dy = py[i+1] - py[i];
        f32 d = sqrtf(dx*dx + dy*dy);
        if (!(d > 1e-4f)) continue;
        f32 ux = dx / d, uy = dy / d, t = 0;
        u32 guard = 0;
        while (t < d && guard++ < 4096) {
            f32 left = (on ? dash : gap) - phase;
            f32 step = fminf(left, d - t);
            if (on) em_seg(pass, px[i] + ux*t, py[i] + uy*t,
                           px[i] + ux*(t+step), py[i] + uy*(t+step), width, ink);
            t += step;
            phase += step;
            if (phase >= (on ? dash : gap) - 1e-4f) { phase = 0; on = !on; }
        }
    }
}

/* Filled isosceles triangle: apex A, base centred on B, half base `hb`. The
 * stroke shader narrows a ribbon to nothing at an end whose across-direction
 * is zero. */
void em_wedge(u8 pass, f32 ax, f32 ay, f32 bx, f32 by, f32 hb, u8 ink) {
    f32 dx = bx - ax, dy = by - ay, d = sqrtf(dx*dx + dy*dy);
    if (!(d > 1e-4f) || !(hb > 0.0f)) return;
    f32 nx = -dy / d, ny = dx / d;
    em_stroke(pass, ax, ay, bx, by, 0.0f, 0.0f, nx, ny, hb * 2.0f, ink);
}

/* Glyphs of one row of text from (x, y), the cell box's top-left. */
void em_text(f32 x, f32 y, const char *s, u32 n, u8 ink) {
    u32 i = 0;
    f32 gx = x;
    while (i < n) {
        u32 cp = 0;
        u32 adv = utf8_decode((const u8 *)s + i, n - i, &cp);
        if (!adv) { i++; gx += g_cw; continue; }
        i += adv;
        if (cp < 32) { gx += g_cw; continue; }
        int w = utf8_char_width(cp);
        if (w <= 0) continue;
        if (cp != ' ') {
            if (!em_grow((void **)&e_gly, &e_cgly, e_ngly + 1, DG_MAX_GLYPH, sizeof *e_gly))
                return;
            e_gly[e_ngly++] = (DPGlyph){ gx, y, cp, ink, (u8)(w >= 2) };
        }
        gx += (f32)w * g_cw;
    }
}

/* Centred multi-row label inside the box (x,y,w,h). */
void em_text_box(const char *s, f32 x, f32 y, f32 w, f32 h, u8 ink) {
    if (!s || !s[0]) return;
    u32 rows, cells;
    dg_text_metrics(s, &rows, &cells);
    f32 rh = g_ch * DG_ROW_H;
    f32 top = y + (h - (f32)rows * rh) * 0.5f + (rh - g_ch) * 0.5f;
    for (u32 k = 0; k < rows; k++) {
        const char *p; u32 n;
        if (!dg_row(s, k, &p, &n)) break;
        f32 tw = (f32)dg_cells(p, n) * g_cw;
        em_text(x + (w - tw) * 0.5f, top + (f32)k * rh, p, n, ink);
    }
}

/* Left-aligned rows from (x, y), one DG_ROW_H pitch apart. */
void em_text_rows(const char *s, f32 x, f32 y, u8 ink) {
    f32 rh = g_ch * DG_ROW_H;
    for (u32 k = 0; k < DG_MAX_ROWS; k++) {
        const char *p; u32 n;
        if (!dg_row(s, k, &p, &n)) break;
        em_text(x, y + (f32)k * rh + (rh - g_ch) * 0.5f, p, n, ink);
    }
}

f32 dg_stroke_w(void) { return fmaxf(1.0f, g_ch * 0.075f); }

/* =========================================================================
 * Graph layout
 * ========================================================================= */

#define DG_MAX_LN    2048
#define DG_MAX_LE    1024
#define DG_MAX_SEG   4352
#define DG_MAX_LRANK 520

typedef struct {
    f32 w, h;            /* size; a plain dummy is 0 x 0 */
    f32 pos;             /* centre on the cross axis */
    i32 rank, order;
    i32 real;            /* g_nodes index, or -1 for a dummy */
    i32 edge;            /* g_edges index a dummy belongs to */
    i16 group;           /* innermost subgraph */
    u8  label;           /* this dummy carries its edge's label */
    u8  depth;           /* subgraph nesting depth */
    i16 chain[DG_MAX_DEPTH];   /* subgraphs root → innermost */
    f32 bc;
} LNode;

typedef struct { i32 u, v; i32 e; bool drawn, rev; } LEdge;
typedef struct { i32 a, b; } LSeg;      /* a in rank r, b in rank r + 1 */

static LNode L_n[DG_MAX_LN];
static u32   L_nn;
static i32   L_map[DG_MAX_NODES];
static LEdge L_e[DG_MAX_LE];
static u32   L_ne;
static i32   L_chain_at[DG_MAX_EDGES], L_chain_n[DG_MAX_EDGES];
static bool  L_chain_rev[DG_MAX_EDGES];
static i32   L_chain[DG_MAX_LN + DG_MAX_EDGES * 2];
static u32   L_nchain;
static LSeg  L_seg[DG_MAX_SEG];
static u32   L_nseg;
static i32   L_up_at[DG_MAX_LN + 1], L_up[DG_MAX_SEG];
static i32   L_dn_at[DG_MAX_LN + 1], L_dn[DG_MAX_SEG];
static i32   L_rank_at[DG_MAX_LRANK + 2], L_rank[DG_MAX_LN];
static i32   L_maxrank;
static i32   L_best[DG_MAX_LN];
static bool  L_horizontal;

static i32 dg_group_parent(i32 g) {
    return (g >= 0 && (u32)g < g_ngroups) ? g_groups[g].parent : -1;
}

/* Subgraphs from the root down to `g`. */
static u32 dg_group_chain(i32 g, i16 *out) {
    i16 tmp[DG_MAX_DEPTH + 2];
    u32 n = 0;
    for (u32 guard = 0; g >= 0 && guard < DG_MAX_GROUPS && n < DG_MAX_DEPTH; guard++) {
        tmp[n++] = (i16)g;
        g = dg_group_parent(g);
    }
    for (u32 i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static bool dg_group_within(i32 g, i32 anc) {
    for (u32 guard = 0; g >= 0 && guard < DG_MAX_GROUPS; guard++) {
        if (g == anc) return true;
        g = dg_group_parent(g);
    }
    return false;
}

static i32 dg_group_lca(i32 a, i32 b) {
    i16 ca[DG_MAX_DEPTH], cb[DG_MAX_DEPTH];
    u32 na = dg_group_chain(a, ca), nb = dg_group_chain(b, cb);
    i32 l = -1;
    for (u32 i = 0; i < na && i < nb && ca[i] == cb[i]; i++) l = ca[i];
    return l;
}

/* Members of subgraph g at its edges: `entry` = no link reaches them from
 * inside g, else `exit` = no link leaves them for inside g. All members when
 * every one has such a link (a cycle). */
static u32 dg_group_ends(i32 g, bool entry, i32 *out, u32 cap) {
    u32 n = 0, all = 0;
    i32 members[DG_MAX_NODES];
    for (u32 i = 0; i < g_nnodes; i++) {
        if (g_nodes[i].proxy >= 0 || !dg_group_within(g_nodes[i].group, g)) continue;
        members[all++] = (i32)i;
    }
    for (u32 k = 0; k < all && n < cap; k++) {
        i32 m = members[k];
        bool linked = false;
        for (u32 e = 0; e < g_nedges && !linked; e++) {
            const DEdge *ed = &g_edges[e];
            i32 other = entry ? ed->from : ed->to;
            i32 self = entry ? ed->to : ed->from;
            if (self != m || other == m || other < 0) continue;
            if (g_nodes[other].proxy < 0 && dg_group_within(g_nodes[other].group, g))
                linked = true;
        }
        if (!linked) out[n++] = m;
    }
    if (n == 0) for (u32 k = 0; k < all && n < cap; k++) out[n++] = members[k];
    return n;
}

f32 dg_label_chip_w(const char *label) {
    u32 rows, cells;
    dg_text_metrics(label, &rows, &cells);
    return (f32)cells * g_cw + g_cw * 1.0f;
}
f32 dg_label_chip_h(const char *label) {
    u32 rows, cells;
    dg_text_metrics(label, &rows, &cells);
    return (f32)rows * g_ch * DG_ROW_H + g_ch * 0.3f;
}

/* Rows a greedy wrap of [s, s+n) at `width` cells makes, breaking at spaces
 * only; with `write` the breaking spaces become '\n'. */
static u32 dg_wrap_rows(char *s, u32 n, u32 width, bool write) {
    u32 rows = 1, used = 0, i = 0;
    while (i < n) {
        while (i < n && s[i] == ' ') i++;
        u32 ws = i;
        while (i < n && s[i] != ' ') i++;
        if (i == ws) break;
        u32 wc = dg_cells(s + ws, i - ws);
        if (used > 0 && used + 1 + wc > width) {
            rows++;
            if (write) s[ws - 1] = '\n';
            used = wc;
        } else {
            used += (used > 0 ? 1 : 0) + wc;
        }
    }
    return rows;
}

/* A label's rows longer than `limit` cells are broken at spaces into as few
 * rows as that takes, each as even as it can be: the narrowest width that
 * still gives that many rows. A word longer than the limit keeps its row. */
void dg_wrap_label(char *s, u32 limit) {
    u32 n = (u32)strlen(s), i = 0, rows = 0;
    while (i <= n && rows < DG_MAX_ROWS) {
        u32 e = i;
        while (e < n && s[e] != '\n') e++;
        u32 len = e - i;
        rows++;
        if (dg_cells(s + i, len) > limit) {
            u32 want = dg_wrap_rows(s + i, len, limit, false);
            u32 widest = 0;
            for (u32 k = i; k < e; ) {
                u32 ws = k;
                while (k < e && s[k] != ' ') k++;
                u32 wc = dg_cells(s + ws, k - ws);
                if (wc > widest) widest = wc;
                while (k < e && s[k] == ' ') k++;
            }
            u32 lo = widest > 0 ? widest : 1, hi = limit;
            while (lo < hi) {
                u32 mid = (lo + hi) / 2;
                if (dg_wrap_rows(s + i, len, mid, false) <= want) hi = mid; else lo = mid + 1;
            }
            if (rows - 1 + want <= DG_MAX_ROWS) {
                dg_wrap_rows(s + i, len, lo, true);
                rows += want - 1;
            }
        }
        i = e + 1;
    }
}

/* Cells a label's row may take before it is wrapped: a rhombus holds less
 * the wider its text, so its own label wraps sooner. */
static u32 dg_wrap_limit(u8 shape) {
    switch (shape) {
    case SH_DIAMOND: return 22;
    case SH_CIRCLE:  return 20;
    case SH_HEX:     return 28;
    case SH_CLASS: case SH_ENTITY: case SH_DOT: case SH_RING: case SH_BAR: return 0;
    default:         return 32;
    }
}

/* Text box a node's label needs, before its shape is wrapped around it. */
static void dg_size_node(DNode *n) {
    u32 rows, cells;
    dg_text_metrics(n->label, &rows, &cells);
    f32 tw = (f32)cells * g_cw;
    f32 th = (f32)rows * g_ch * DG_ROW_H;
    f32 w = tw + g_cw * (1.0f + 2.0f * g_gap);
    f32 h = th + g_ch * (0.45f + 0.55f * g_gap);
    f32 hmin = g_ch * (1.6f + 0.6f * g_gap);
    if (h < hmin) h = hmin;
    switch (n->shape) {
    case SH_DIAMOND: {
        if (!n->label[0]) { w = h = g_ch * 1.6f; break; }
        /* The smallest rhombus `k` times as wide as tall that holds the text
         * box (bw, bh): its corners are inside when bw/w + bh/h <= 1, so
         * h = bw/k + bh. */
        f32 k = L_horizontal ? 1.6f : 2.0f;
        f32 bw = tw + g_cw * 2.0f, bh = th + g_ch * 0.5f;
        h = bw / k + bh;
        w = h * k;
        break;
    }
    case SH_HEX:        w += h * 0.7f; break;
    case SH_PARA:       w += h * 0.8f; break;
    case SH_CYLINDER:   h += g_ch * 1.0f; break;
    case SH_SUBROUTINE: w += g_cw * 2.0f; break;
    case SH_STADIUM:    w += h * 0.4f; break;
    case SH_ROUND:      w += g_cw * 0.5f; break;
    case SH_CIRCLE: {
        f32 d = sqrtf(tw * tw + th * th) + g_cw * 2.0f;
        if (d < h) d = h;
        w = h = d;
        break;
    }
    case SH_DOT: case SH_RING: w = h = g_ch * 1.0f; break;
    case SH_BAR:
        if (g_dir == 1 || g_dir == 3) { w = g_ch * 0.45f; h = g_ch * 3.6f; }
        else { w = g_ch * 3.6f; h = g_ch * 0.45f; }
        break;
    case SH_CLASS: case SH_ENTITY: {
        f32 head = th + g_ch * 0.9f;
        f32 total = head;
        f32 wmax = tw;
        for (u32 part = 0; part < 2; part++) {
            const char *b = part ? n->body2 : n->body;
            if (!b[0]) {
                if (n->shape == SH_CLASS) total += g_ch * 0.6f;
                continue;
            }
            u32 br, bc;
            dg_text_metrics(b, &br, &bc);
            if ((f32)bc * g_cw > wmax) wmax = (f32)bc * g_cw;
            total += (f32)br * g_ch * DG_ROW_H + g_ch * 0.6f;
        }
        w = wmax + g_cw * 3.0f;
        h = total;
        break;
    }
    default: break;
    }
    n->w = w;
    n->h = h;
}

static f32 dg_rank_size(const LNode *n)  { return L_horizontal ? n->w : n->h; }
static f32 dg_cross_size(const LNode *n) { return L_horizontal ? n->h : n->w; }

static f32 dg_title_band(i32 g) {
    u32 rows, cells;
    dg_text_metrics(g_groups[g].label, &rows, &cells);
    if (!g_groups[g].label[0]) return g_ch * 0.6f;
    return (f32)rows * g_ch * DG_ROW_H + g_ch * 0.8f * g_gap;
}

static f32 dg_cluster_pad(void) { return (L_horizontal ? g_ch * 0.7f : g_cw * 1.6f) * g_gap; }

/* Each subgraph's border on the cross axis, from where its members are now:
 * every level between a member and the subgraph adds its padding (and, in a
 * horizontal layout, a title band on the low side). An outsider sharing a
 * rank with the subgraph keeps clear of the whole box, not only of the
 * member beside it, because the box is as wide as its widest rank. */
static f32  g_cb_lo[DG_MAX_GROUPS], g_cb_hi[DG_MAX_GROUPS];
static bool g_cb_valid;

static f32 dg_cross_size(const LNode *n);
static f32 dg_title_band(i32 g);

static void dg_cluster_extents(void) {
    g_cb_valid = g_ngroups > 0;
    if (!g_cb_valid) return;
    for (u32 g = 0; g < g_ngroups; g++) { g_cb_lo[g] = 1e30f; g_cb_hi[g] = -1e30f; }
    f32 pad = dg_cluster_pad();
    for (u32 i = 0; i < L_nn; i++) {
        const LNode *n = &L_n[i];
        if (!n->depth) continue;
        f32 l = n->pos - dg_cross_size(n) * 0.5f, h = n->pos + dg_cross_size(n) * 0.5f;
        f32 acc_lo = 0, acc_hi = 0;
        for (i32 j = (i32)n->depth - 1; j >= 0; j--) {
            i32 c = n->chain[j];
            acc_lo += pad + (L_horizontal ? dg_title_band(c) : 0.0f);
            acc_hi += pad;
            if (l - acc_lo < g_cb_lo[c]) g_cb_lo[c] = l - acc_lo;
            if (h + acc_hi > g_cb_hi[c]) g_cb_hi[c] = h + acc_hi;
        }
    }
    if (L_horizontal) return;
    /* A top-down box is at least as wide as its title, widened on both sides
     * as the drawn box is, and its parent holds it: from the innermost out. */
    for (i32 depth = DG_MAX_DEPTH; depth >= 1; depth--) {
        for (u32 g = 0; g < g_ngroups; g++) {
            i16 ch[DG_MAX_DEPTH];
            if ((i32)dg_group_chain((i32)g, ch) != depth || g_cb_lo[g] > 1e29f) continue;
            u32 tr, tc;
            dg_text_metrics(g_groups[g].label, &tr, &tc);
            f32 need = (f32)tc * g_cw + g_cw * 2.0f, w = g_cb_hi[g] - g_cb_lo[g];
            if (w < need) { g_cb_lo[g] -= (need - w) * 0.5f; g_cb_hi[g] += (need - w) * 0.5f; }
            i32 p = g_groups[g].parent;
            if (p >= 0 && (u32)p < g_ngroups) {
                if (g_cb_lo[g] - pad < g_cb_lo[p]) g_cb_lo[p] = g_cb_lo[g] - pad;
                if (g_cb_hi[g] + pad > g_cb_hi[p]) g_cb_hi[p] = g_cb_hi[g] + pad;
            }
        }
    }
}

/* Minimum centre-to-centre distance between neighbours a, b (a before b) on
 * the cross axis. Crossing into or out of a subgraph adds its padding, and
 * in a horizontal layout entering one from above also its title band. */
static f32 dg_sep(const LNode *a, const LNode *b) {
    f32 node_gap = (L_horizontal ? g_ch * 1.4f : g_cw * 2.6f) * g_gap;
    f32 edge_gap = (L_horizontal ? g_ch * 0.55f : g_cw * 1.1f) * g_gap;
    bool ra = a->real >= 0 || a->label, rb = b->real >= 0 || b->label;
    f32 gap = (ra && rb) ? node_gap : (!ra && !rb) ? edge_gap : (node_gap + edge_gap) * 0.5f;
    u32 k = 0;
    while (k < a->depth && k < b->depth && a->chain[k] == b->chain[k]) k++;
    f32 pad = (f32)((a->depth - k) + (b->depth - k)) * dg_cluster_pad();
    if (L_horizontal)
        for (u32 j = k; j < b->depth; j++) pad += dg_title_band(b->chain[j]);
    return (dg_cross_size(a) + dg_cross_size(b)) * 0.5f + gap + pad;
}

/* The gap kept between a node and a subgraph border beside it. */
static f32 dg_border_gap(const LNode *n) {
    bool real = n->real >= 0 || n->label;
    return (L_horizontal ? (real ? g_ch * 0.9f : g_ch * 0.4f) : (real ? g_cw * 2.0f : g_cw * 0.8f)) * g_gap;
}

static void dg_lnode_chain(LNode *n) {
    n->depth = (u8)dg_group_chain(n->group, n->chain);
}

static i32 dg_new_lnode(i32 real, f32 w, f32 h, i16 group) {
    if (L_nn >= DG_MAX_LN) return -1;
    LNode *n = &L_n[L_nn];
    memset(n, 0, sizeof *n);
    n->w = w; n->h = h;
    n->real = real;
    n->edge = -1;
    n->group = group;
    dg_lnode_chain(n);
    return (i32)L_nn++;
}

/* Order of a rank's members, written into L_rank by their `order`. */
static void dg_rank_sort_by_order(i32 r) {
    i32 a = L_rank_at[r], b = L_rank_at[r + 1];
    for (i32 i = a + 1; i < b; i++) {
        i32 v = L_rank[i], j = i;
        while (j > a && L_n[L_rank[j - 1]].order > L_n[v].order) { L_rank[j] = L_rank[j - 1]; j--; }
        L_rank[j] = v;
    }
}

/* Crossings between rank r and r + 1: segments sorted by their upper end,
 * then inversions of the lower ends counted with a Fenwick tree. */
static u32 dg_cross_pair(i32 r) {
    static i32 pa[DG_MAX_SEG], pb[DG_MAX_SEG];
    static u32 bit[DG_MAX_LN + 2];
    u32 m = 0;
    i32 a = L_rank_at[r], b = L_rank_at[r + 1];
    for (i32 i = a; i < b; i++) {
        i32 v = L_rank[i];
        for (i32 k = L_dn_at[v]; k < L_dn_at[v + 1]; k++) {
            if (m >= DG_MAX_SEG) break;
            pa[m] = L_n[v].order;
            pb[m] = L_n[L_dn[k]].order;
            m++;
        }
    }
    if (m < 2) return 0;
    /* insertion sort by (pa, pb) — the upper ends are already nearly sorted */
    for (u32 i = 1; i < m; i++) {
        i32 xa = pa[i], xb = pb[i];
        u32 j = i;
        while (j > 0 && (pa[j-1] > xa || (pa[j-1] == xa && pb[j-1] > xb))) {
            pa[j] = pa[j-1]; pb[j] = pb[j-1]; j--;
        }
        pa[j] = xa; pb[j] = xb;
    }
    u32 width = (u32)(L_rank_at[r + 2] - L_rank_at[r + 1]) + 1;
    if (width > DG_MAX_LN + 1) width = DG_MAX_LN + 1;
    memset(bit, 0, sizeof(u32) * (width + 1));
    u32 cross = 0, seen = 0;
    for (u32 i = 0; i < m; i++) {
        u32 x = (u32)pb[i] + 1;
        if (x > width) x = width;
        /* how many seen so far are <= x */
        u32 le = 0;
        for (u32 j = x; j > 0; j -= j & (~j + 1)) le += bit[j];
        cross += seen - le;
        for (u32 j = x; j <= width; j += j & (~j + 1)) bit[j]++;
        seen++;
    }
    return cross;
}

static u32 dg_crossings(void) {
    u32 c = 0;
    for (i32 r = 0; r < L_maxrank; r++) c += dg_cross_pair(r);
    return c;
}

/* Sort one rank by barycentre, keeping every subgraph contiguous: at the
 * first level two nodes' subgraph chains differ, the units being compared are
 * those subgraphs (by their members' mean barycentre in this rank) or the
 * node itself where its chain has ended. */
static f32 g_unit_sum[DG_MAX_GROUPS], g_unit_cnt[DG_MAX_GROUPS];
static i32 g_unit_min[DG_MAX_GROUPS];

static int dg_unit_cmp(const LNode *a, const LNode *b) {
    u32 k = 0;
    while (k < a->depth && k < b->depth && a->chain[k] == b->chain[k]) k++;
    f32 ua, ub;
    i32 ta, tb;
    if (k < a->depth) {
        i32 g = a->chain[k];
        ua = g_unit_sum[g] / fmaxf(1.0f, g_unit_cnt[g]); ta = g_unit_min[g];
    } else { ua = a->bc; ta = a->order; }
    if (k < b->depth) {
        i32 g = b->chain[k];
        ub = g_unit_sum[g] / fmaxf(1.0f, g_unit_cnt[g]); tb = g_unit_min[g];
    } else { ub = b->bc; tb = b->order; }
    if (ua < ub - 1e-6f) return -1;
    if (ua > ub + 1e-6f) return 1;
    if (ta != tb) return ta < tb ? -1 : 1;
    return a->order < b->order ? -1 : (a->order > b->order ? 1 : 0);
}

static void dg_sort_rank(i32 r, bool use_up) {
    i32 a = L_rank_at[r], b = L_rank_at[r + 1];
    if (b - a < 2) return;
    for (i32 i = a; i < b; i++) {
        LNode *n = &L_n[L_rank[i]];
        i32 v = L_rank[i];
        const i32 *nb = use_up ? &L_up[L_up_at[v]] : &L_dn[L_dn_at[v]];
        i32 cnt = use_up ? L_up_at[v + 1] - L_up_at[v] : L_dn_at[v + 1] - L_dn_at[v];
        if (cnt <= 0) { n->bc = (f32)n->order; continue; }
        f32 sum = 0;
        for (i32 k = 0; k < cnt; k++) sum += (f32)L_n[nb[k]].order;
        n->bc = sum / (f32)cnt;
    }
    for (u32 g = 0; g < g_ngroups; g++) { g_unit_sum[g] = 0; g_unit_cnt[g] = 0; g_unit_min[g] = 1 << 30; }
    for (i32 i = a; i < b; i++) {
        const LNode *n = &L_n[L_rank[i]];
        for (u32 k = 0; k < n->depth; k++) {
            i32 g = n->chain[k];
            g_unit_sum[g] += n->bc;
            g_unit_cnt[g] += 1.0f;
            if (n->order < g_unit_min[g]) g_unit_min[g] = n->order;
        }
    }
    /* stable insertion sort with the unit comparator */
    for (i32 i = a + 1; i < b; i++) {
        i32 v = L_rank[i], j = i;
        while (j > a && dg_unit_cmp(&L_n[L_rank[j - 1]], &L_n[v]) > 0) { L_rank[j] = L_rank[j - 1]; j--; }
        L_rank[j] = v;
    }
    for (i32 i = a; i < b; i++) L_n[L_rank[i]].order = i - a;
}

/* Minimise sum w_i (x_i - t_i)^2 subject to x_{i+1} - x_i >= s_i: shift by
 * the cumulative separations, then a weighted isotonic regression (pool
 * adjacent violators) on what is left. */
static void dg_place_row(const f32 *t, const f32 *w, const f32 *sep, u32 n, f32 *x) {
    static f32 off[DG_MAX_LN], bw[DG_MAX_LN], bv[DG_MAX_LN];
    static u32 bn[DG_MAX_LN];
    if (n == 0) return;
    off[0] = 0;
    for (u32 i = 1; i < n; i++) off[i] = off[i - 1] + sep[i - 1];
    u32 nb = 0;
    for (u32 i = 0; i < n; i++) {
        f32 wi = w[i] > 1e-4f ? w[i] : 1e-4f;
        bw[nb] = wi; bv[nb] = t[i] - off[i]; bn[nb] = 1; nb++;
        while (nb > 1 && bv[nb - 2] > bv[nb - 1]) {
            f32 tw = bw[nb - 2] + bw[nb - 1];
            bv[nb - 2] = (bv[nb - 2] * bw[nb - 2] + bv[nb - 1] * bw[nb - 1]) / tw;
            bw[nb - 2] = tw;
            bn[nb - 2] += bn[nb - 1];
            nb--;
        }
    }
    u32 i = 0;
    for (u32 k = 0; k < nb; k++)
        for (u32 c = 0; c < bn[k]; c++, i++) x[i] = bv[k] + off[i];
}

static f32 dg_median(f32 *v, u32 n) {
    for (u32 i = 1; i < n; i++) {
        f32 x = v[i]; u32 j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
    if (n & 1u) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) * 0.5f;
}

/* One placement pass over rank r toward the neighbours above (dir < 0),
 * below (dir > 0) or both (dir == 0). */
static void dg_place_rank(i32 r, int dir) {
    static f32 t[DG_MAX_LN], w[DG_MAX_LN], sep[DG_MAX_LN], x[DG_MAX_LN], nb[64];
    i32 a = L_rank_at[r], b = L_rank_at[r + 1];
    u32 n = (u32)(b - a);
    if (!n) return;
    for (u32 i = 0; i < n; i++) {
        i32 v = L_rank[a + (i32)i];
        const LNode *ln = &L_n[v];
        u32 c = 0;
        if (dir <= 0) for (i32 k = L_up_at[v]; k < L_up_at[v + 1] && c < 64; k++) nb[c++] = L_n[L_up[k]].pos;
        if (dir >= 0) for (i32 k = L_dn_at[v]; k < L_dn_at[v + 1] && c < 64; k++) nb[c++] = L_n[L_dn[k]].pos;
        if (c) {
            t[i] = dg_median(nb, c);
            w[i] = ln->real >= 0 ? 1.0f : (ln->label ? 1.5f : 2.5f);
        } else {
            t[i] = ln->pos;
            w[i] = 0.05f;
        }
        if (i + 1 < n) sep[i] = dg_sep(ln, &L_n[L_rank[a + (i32)i + 1]]);
    }
    dg_place_row(t, w, sep, n, x);
    /* A node beside a subgraph it is not in stays outside that subgraph's
     * whole box: a bound on it, enforced by pulling it there hard and solving
     * again. A bound on the node rather than a separation from its neighbour
     * leaves the members free to move inside their box. */
    if (g_cb_valid) {
        static f32 lo[DG_MAX_LN], hi[DG_MAX_LN];
        bool any = false;
        for (u32 i = 0; i < n; i++) {
            const LNode *ln = &L_n[L_rank[a + (i32)i]];
            lo[i] = -1e30f; hi[i] = 1e30f;
            f32 half = dg_cross_size(ln) * 0.5f + dg_border_gap(ln);
            if (i + 1 < n) {
                const LNode *nx = &L_n[L_rank[a + (i32)i + 1]];
                u32 k = 0;
                while (k < ln->depth && k < nx->depth && ln->chain[k] == nx->chain[k]) k++;
                if (k == ln->depth && k < nx->depth && g_cb_lo[nx->chain[k]] < 1e29f) {
                    hi[i] = g_cb_lo[nx->chain[k]] - half;
                    any = true;
                }
            }
            if (i > 0) {
                const LNode *pv = &L_n[L_rank[a + (i32)i - 1]];
                u32 k = 0;
                while (k < ln->depth && k < pv->depth && ln->chain[k] == pv->chain[k]) k++;
                if (k == ln->depth && k < pv->depth && g_cb_hi[pv->chain[k]] > -1e29f) {
                    lo[i] = g_cb_hi[pv->chain[k]] + half;
                    any = true;
                }
            }
        }
        for (u32 round = 0; any && round < 3; round++) {
            bool broke = false;
            for (u32 i = 0; i < n; i++) {
                if (x[i] > hi[i] + 0.5f) { t[i] = hi[i]; w[i] = 400.0f; broke = true; }
                else if (x[i] < lo[i] - 0.5f) { t[i] = lo[i]; w[i] = 400.0f; broke = true; }
            }
            if (!broke) break;
            dg_place_row(t, w, sep, n, x);
        }
    }
    for (u32 i = 0; i < n; i++) L_n[L_rank[a + (i32)i]].pos = x[i];
}

/* What the placement cannot promise: two subgraphs side by side are each as
 * wide as their widest rank, and a rank can only move its own nodes. So,
 * level by level from the innermost subgraphs out, the units inside one
 * parent — its child subgraphs, as rigid blocks, and its own nodes — are
 * swept from the left, and a unit that overlaps one already placed in a rank
 * they share moves right, with everything in it, by as much as it must.
 * Rightward moves only, each unit once a level: it ends. */
typedef struct { f32 lo, hi; i32 r0, r1; i32 group; i32 node; bool dummy; } DgUnit;

static void dg_unit_shift(const DgUnit *u, f32 d) {
    if (u->node >= 0) { L_n[u->node].pos += d; return; }
    for (u32 i = 0; i < L_nn; i++)
        if (dg_group_within(L_n[i].group, u->group)) L_n[i].pos += d;
}

static void dg_unit_extent(DgUnit *u) {
    if (u->node >= 0) {
        const LNode *n = &L_n[u->node];
        u->lo = n->pos - dg_cross_size(n) * 0.5f;
        u->hi = n->pos + dg_cross_size(n) * 0.5f;
        u->r0 = u->r1 = n->rank;
        return;
    }
    dg_cluster_extents();
    u->lo = g_cb_lo[u->group];
    u->hi = g_cb_hi[u->group];
    u->r0 = 1 << 30; u->r1 = -1;
    for (u32 i = 0; i < L_nn; i++) {
        if (!dg_group_within(L_n[i].group, u->group)) continue;
        if (L_n[i].rank < u->r0) u->r0 = L_n[i].rank;
        if (L_n[i].rank > u->r1) u->r1 = L_n[i].rank;
    }
}

static void dg_separate_units(void) {
    if (!g_ngroups) return;
    static DgUnit units[DG_MAX_LN + DG_MAX_GROUPS];
    static f32 front[DG_MAX_LRANK + 2];
    static u8  front_dummy[DG_MAX_LRANK + 2];
    /* deepest subgraphs first, the top level (-1) last */
    i32 order[DG_MAX_GROUPS + 1];
    u32 no = 0;
    for (i32 depth = DG_MAX_DEPTH; depth >= 1; depth--)
        for (u32 g = 0; g < g_ngroups; g++) {
            i16 ch[DG_MAX_DEPTH];
            if ((i32)dg_group_chain((i32)g, ch) == depth && g_groups[g].used) order[no++] = (i32)g;
        }
    order[no++] = -1;
    f32 node_gap = (L_horizontal ? g_ch * 1.4f : g_cw * 2.6f) * g_gap;
    f32 edge_gap = (L_horizontal ? g_ch * 0.55f : g_cw * 1.1f) * g_gap;
    for (u32 lv = 0; lv < no; lv++) {
        i32 parent = order[lv];
        u32 nu = 0;
        for (u32 g = 0; g < g_ngroups; g++) {
            if (g_groups[g].parent != parent || !g_groups[g].used) continue;
            DgUnit u = { 0, 0, 0, 0, (i32)g, -1, false };
            dg_unit_extent(&u);
            if (u.r1 < 0) continue;
            units[nu++] = u;
        }
        bool has_child = nu > 0;
        if (!has_child) continue;              /* nothing rigid to keep apart here */
        u32 ngroups_here = nu;
        for (u32 i = 0; i < L_nn; i++) {
            if (L_n[i].group != parent) continue;
            DgUnit u = { 0, 0, 0, 0, -1, (i32)i, L_n[i].real < 0 && !L_n[i].label };
            dg_unit_extent(&u);
            units[nu++] = u;
        }
        /* A node keeps the side of a subgraph its rank's order gives it: one
         * ordered before the subgraph's members in a rank they share is moved
         * clear of the box's left side, one ordered after clear of its right.
         * The placement bounded it by the box as it stood before its rank's
         * members moved, so it can stand inside the box, and the sweep below,
         * by left edge, would then carry it out of the far side. */
        for (u32 i = ngroups_here; i < nu; i++) {
            DgUnit *u = &units[i];
            LNode *ln = &L_n[u->node];
            for (u32 j = 0; j < ngroups_here; j++) {
                const DgUnit *gu = &units[j];
                if (ln->rank < gu->r0 || ln->rank > gu->r1) continue;
                i32 lo_order = 1 << 30, hi_order = -1;
                for (i32 k = L_rank_at[ln->rank]; k < L_rank_at[ln->rank + 1]; k++) {
                    const LNode *m = &L_n[L_rank[k]];
                    if (!dg_group_within(m->group, gu->group)) continue;
                    if (m->order < lo_order) lo_order = m->order;
                    if (m->order > hi_order) hi_order = m->order;
                }
                if (hi_order < 0) continue;
                f32 gap = u->dummy ? (node_gap + edge_gap) * 0.5f : node_gap;
                f32 d = 0;
                if (ln->order < lo_order && u->hi > gu->lo - gap) d = (gu->lo - gap) - u->hi;
                else if (ln->order > hi_order && u->lo < gu->hi + gap) d = (gu->hi + gap) - u->lo;
                if (d != 0) { ln->pos += d; u->lo += d; u->hi += d; }
            }
        }
        /* by left edge */
        for (u32 i = 1; i < nu; i++) {
            DgUnit v = units[i];
            u32 j = i;
            while (j > 0 && units[j - 1].lo > v.lo) { units[j] = units[j - 1]; j--; }
            units[j] = v;
        }
        for (i32 r = 0; r <= L_maxrank; r++) { front[r] = -1e30f; front_dummy[r] = 1; }
        for (u32 i = 0; i < nu; i++) {
            DgUnit *u = &units[i];
            if (u->group >= 0) dg_unit_extent(u);   /* an earlier move may have carried it */
            f32 need = -1e30f;
            for (i32 r = u->r0; r <= u->r1; r++) {
                if (front[r] < -1e29f) continue;
                f32 gap = (u->dummy && front_dummy[r]) ? edge_gap
                        : (u->dummy || front_dummy[r]) ? (node_gap + edge_gap) * 0.5f : node_gap;
                if (front[r] + gap > need) need = front[r] + gap;
            }
            if (need > u->lo + 0.5f) {
                f32 d = need - u->lo;
                dg_unit_shift(u, d);
                /* units to its right that it now reaches are carried in turn
                 * when their turn comes; their recorded edges move with them */
                u->lo += d; u->hi += d;
            }
            for (i32 r = u->r0; r <= u->r1; r++) {
                if (u->hi > front[r]) { front[r] = u->hi; front_dummy[r] = u->dummy; }
            }
        }
    }
}

/* Edges, in layout terms: a drawn edge per source link between its chosen
 * ends, and phantom edges that only rank (a link to a subgraph holds every
 * one of its entry members below the source). */
static bool dg_build_ledges(void) {
    L_ne = 0;
    for (u32 e = 0; e < g_nedges; e++) {
        const DEdge *ed = &g_edges[e];
        L_chain_n[e] = 0;
        if (ed->from < 0 || ed->to < 0) continue;
        i32 us[8], vs[8];
        u32 nu, nv;
        if (g_nodes[ed->from].proxy >= 0) nu = dg_group_ends(g_nodes[ed->from].proxy, false, us, 8);
        else { us[0] = ed->from; nu = 1; }
        if (g_nodes[ed->to].proxy >= 0) nv = dg_group_ends(g_nodes[ed->to].proxy, true, vs, 8);
        else { vs[0] = ed->to; nv = 1; }
        if (!nu || !nv) continue;
        /* The drawn link meets the subgraph in its middle member. */
        i32 du = us[nu / 2], dv = vs[nv / 2];
        if (du == dv) continue;                      /* a self loop: drawn apart */
        for (u32 i = 0; i < nu; i++) {
            for (u32 j = 0; j < nv; j++) {
                bool drawn = (us[i] == du && vs[j] == dv);
                if (us[i] == vs[j]) continue;
                if (!drawn && L_ne >= DG_MAX_LE - 8) continue;
                if (L_ne >= DG_MAX_LE) return false;
                i32 lu = L_map[us[i]], lv = L_map[vs[j]];
                if (lu < 0 || lv < 0) continue;
                L_e[L_ne++] = (LEdge){ lu, lv, (i32)e, drawn, false };
            }
        }
    }
    return true;
}

/* Reverse the edges that close a cycle, found by a depth-first walk that
 * starts from the nodes nothing points at. */
static void dg_break_cycles(u32 nreal) {
    static i32 out_at[DG_MAX_LN + 1], out[DG_MAX_LE], indeg[DG_MAX_LN];
    static u8  state[DG_MAX_LN];
    static i32 stack[DG_MAX_LN], iter[DG_MAX_LN];
    memset(out_at, 0, sizeof(i32) * (nreal + 1));
    memset(indeg, 0, sizeof(i32) * nreal);
    for (u32 k = 0; k < L_ne; k++) { out_at[L_e[k].u + 1]++; indeg[L_e[k].v]++; }
    for (u32 i = 0; i < nreal; i++) out_at[i + 1] += out_at[i];
    static i32 fill[DG_MAX_LN];
    memcpy(fill, out_at, sizeof(i32) * nreal);
    for (u32 k = 0; k < L_ne; k++) out[fill[L_e[k].u]++] = (i32)k;
    memset(state, 0, nreal);
    for (u32 pass = 0; pass < 2; pass++) {
        for (u32 root = 0; root < nreal; root++) {
            if (state[root] || (pass == 0 && indeg[root] > 0)) continue;
            i32 sp = 0;
            stack[0] = (i32)root; iter[0] = out_at[root]; state[root] = 1;
            while (sp >= 0) {
                i32 v = stack[sp];
                if (iter[sp] >= out_at[v + 1]) { state[v] = 2; sp--; continue; }
                i32 k = out[iter[sp]++];
                i32 w = L_e[k].v;
                if (state[w] == 1) L_e[k].rev = true;
                else if (state[w] == 0 && sp + 1 < (i32)DG_MAX_LN) {
                    state[w] = 1;
                    stack[++sp] = w;
                    iter[sp] = out_at[w];
                }
            }
        }
    }
    for (u32 k = 0; k < L_ne; k++)
        if (L_e[k].rev) { i32 t = L_e[k].u; L_e[k].u = L_e[k].v; L_e[k].v = t; }
}

/* Longest path from the sources with every edge two ranks long, then each
 * source pulled down to just above the nearest thing it points at. */
static bool dg_rank_nodes(u32 nreal) {
    static i32 indeg[DG_MAX_LN], topo[DG_MAX_LN], out_at[DG_MAX_LN + 1], out[DG_MAX_LE];
    memset(indeg, 0, sizeof(i32) * nreal);
    memset(out_at, 0, sizeof(i32) * (nreal + 1));
    for (u32 k = 0; k < L_ne; k++) { indeg[L_e[k].v]++; out_at[L_e[k].u + 1]++; }
    for (u32 i = 0; i < nreal; i++) out_at[i + 1] += out_at[i];
    static i32 fill[DG_MAX_LN];
    memcpy(fill, out_at, sizeof(i32) * nreal);
    for (u32 k = 0; k < L_ne; k++) out[fill[L_e[k].u]++] = L_e[k].v;
    static i32 deg[DG_MAX_LN];
    memcpy(deg, indeg, sizeof(i32) * nreal);
    u32 head = 0, tail = 0;
    for (u32 i = 0; i < nreal; i++) { L_n[i].rank = 0; if (!deg[i]) topo[tail++] = (i32)i; }
    while (head < tail) {
        i32 v = topo[head++];
        for (i32 k = out_at[v]; k < out_at[v + 1]; k++) {
            i32 w = out[k];
            if (L_n[w].rank < L_n[v].rank + 2) L_n[w].rank = L_n[v].rank + 2;
            if (--deg[w] == 0) topo[tail++] = w;
        }
    }
    if (tail != nreal) return false;                 /* not a DAG: cannot happen */
    for (i32 i = (i32)tail - 1; i >= 0; i--) {
        i32 v = topo[i];
        if (indeg[v] || out_at[v] == out_at[v + 1]) continue;
        i32 best = 1 << 30;
        for (i32 k = out_at[v]; k < out_at[v + 1]; k++)
            if (L_n[out[k]].rank - 2 < best) best = L_n[out[k]].rank - 2;
        if (best > L_n[v].rank) L_n[v].rank = best;
    }
    L_maxrank = 0;
    for (u32 i = 0; i < nreal; i++) if (L_n[i].rank > L_maxrank) L_maxrank = L_n[i].rank;
    return L_maxrank <= DG_MAX_LRANK - 2;
}

static bool dg_layout_graph(void) {
    L_horizontal = (g_dir == 1 || g_dir == 3);
    L_nn = 0; L_nchain = 0; L_nseg = 0;
    g_cb_valid = false;
    for (u32 i = 0; i < g_nnodes; i++) {
        u32 lim = dg_wrap_limit(g_nodes[i].shape);
        if (lim) dg_wrap_label(g_nodes[i].label, lim);
        dg_size_node(&g_nodes[i]);
    }
    for (u32 e = 0; e < g_nedges; e++) dg_wrap_label(g_edges[e].label, 24);
    for (u32 i = 0; i < g_nnodes; i++) {
        L_map[i] = -1;
        if (g_nodes[i].proxy >= 0) continue;
        L_map[i] = dg_new_lnode((i32)i, g_nodes[i].w, g_nodes[i].h, g_nodes[i].group);
        if (L_map[i] < 0) return false;
    }
    u32 nreal = L_nn;
    if (!nreal) return false;
    if (!dg_build_ledges()) return false;
    dg_break_cycles(nreal);
    if (!dg_rank_nodes(nreal)) return false;

    /* Dummies along every drawn edge, the middle one holding the label. */
    for (u32 k = 0; k < L_ne; k++) {
        const LEdge *le = &L_e[k];
        if (!le->drawn) continue;
        i32 e = le->e;
        i32 span = L_n[le->v].rank - L_n[le->u].rank;
        if (span < 1) continue;
        if (L_nn + (u32)span > DG_MAX_LN || L_nchain + (u32)span + 1 > (u32)(DG_MAX_LN + DG_MAX_EDGES * 2))
            continue;                                /* drawn straight */
        i16 grp = (i16)dg_group_lca(L_n[le->u].group, L_n[le->v].group);
        L_chain_at[e] = (i32)L_nchain;
        L_chain_rev[e] = le->rev;
        L_chain[L_nchain++] = le->u;
        i32 mid = L_n[le->u].rank + span / 2;
        for (i32 r = L_n[le->u].rank + 1; r < L_n[le->v].rank; r++) {
            bool lab = (r == mid) && g_edges[e].label[0];
            f32 w = lab ? dg_label_chip_w(g_edges[e].label) : 0.0f;
            f32 h = lab ? dg_label_chip_h(g_edges[e].label) : 0.0f;
            i32 d = dg_new_lnode(-1, w, h, grp);
            if (d < 0) return false;
            L_n[d].rank = r;
            L_n[d].edge = e;
            L_n[d].label = lab;
            L_chain[L_nchain++] = d;
        }
        L_chain[L_nchain++] = le->v;
        L_chain_n[e] = (i32)L_nchain - L_chain_at[e];
    }
    /* Segments and the neighbour lists the sweeps read. */
    for (u32 e = 0; e < g_nedges; e++) {
        for (i32 i = 0; i + 1 < L_chain_n[e]; i++) {
            if (L_nseg >= DG_MAX_SEG) break;
            L_seg[L_nseg++] = (LSeg){ L_chain[L_chain_at[e] + i], L_chain[L_chain_at[e] + i + 1] };
        }
    }
    memset(L_up_at, 0, sizeof(i32) * (L_nn + 1));
    memset(L_dn_at, 0, sizeof(i32) * (L_nn + 1));
    for (u32 s = 0; s < L_nseg; s++) { L_dn_at[L_seg[s].a + 1]++; L_up_at[L_seg[s].b + 1]++; }
    for (u32 i = 0; i < L_nn; i++) { L_dn_at[i + 1] += L_dn_at[i]; L_up_at[i + 1] += L_up_at[i]; }
    {
        static i32 fu[DG_MAX_LN], fd[DG_MAX_LN];
        memcpy(fu, L_up_at, sizeof(i32) * L_nn);
        memcpy(fd, L_dn_at, sizeof(i32) * L_nn);
        for (u32 s = 0; s < L_nseg; s++) {
            L_dn[fd[L_seg[s].a]++] = L_seg[s].b;
            L_up[fu[L_seg[s].b]++] = L_seg[s].a;
        }
    }
    /* Ranks, and a first order from a depth-first walk down the edges. */
    memset(L_rank_at, 0, sizeof(i32) * (u32)(L_maxrank + 3));
    for (u32 i = 0; i < L_nn; i++) L_rank_at[L_n[i].rank + 1]++;
    for (i32 r = 0; r <= L_maxrank + 1; r++) L_rank_at[r + 1] += L_rank_at[r];
    {
        static i32 next[DG_MAX_LRANK + 2], stack[DG_MAX_LN];
        static u8 seen[DG_MAX_LN];
        memset(next, 0, sizeof(i32) * (u32)(L_maxrank + 2));
        memset(seen, 0, L_nn);
        for (u32 pass = 0; pass < 2; pass++) {
            for (u32 root = 0; root < L_nn; root++) {
                if (seen[root]) continue;
                if (pass == 0 && (L_n[root].real < 0 || L_up_at[root + 1] > L_up_at[root])) continue;
                i32 sp = 0;
                stack[sp++] = (i32)root;
                while (sp > 0) {
                    i32 v = stack[--sp];
                    if (seen[v]) continue;
                    seen[v] = 1;
                    L_n[v].order = next[L_n[v].rank]++;
                    for (i32 k = L_dn_at[v + 1] - 1; k >= L_dn_at[v]; k--)
                        if (!seen[L_dn[k]] && sp < (i32)DG_MAX_LN) stack[sp++] = L_dn[k];
                }
            }
        }
        static i32 fill[DG_MAX_LRANK + 2];
        memcpy(fill, L_rank_at, sizeof(i32) * (u32)(L_maxrank + 2));
        for (u32 i = 0; i < L_nn; i++) L_rank[fill[L_n[i].rank]++] = (i32)i;
        for (i32 r = 0; r <= L_maxrank; r++) dg_rank_sort_by_order(r);
    }
    /* Barycentre sweeps, down then up, keeping the ordering with the fewest
     * crossings; stop after four sweeps that found nothing better. */
    {
        u32 best = dg_crossings();
        for (u32 i = 0; i < L_nn; i++) L_best[i] = L_n[i].order;
        u32 stale = 0;
        for (u32 it = 0; it < 24 && best > 0 && stale < 4; it++) {
            if (it & 1u) for (i32 r = L_maxrank - 1; r >= 0; r--) dg_sort_rank(r, false);
            else         for (i32 r = 1; r <= L_maxrank; r++) dg_sort_rank(r, true);
            u32 c = dg_crossings();
            if (c < best) {
                best = c; stale = 0;
                for (u32 i = 0; i < L_nn; i++) L_best[i] = L_n[i].order;
            } else stale++;
        }
        for (u32 i = 0; i < L_nn; i++) L_n[i].order = L_best[i];
        for (i32 r = 0; r <= L_maxrank; r++) dg_rank_sort_by_order(r);
    }
    /* Cross axis: pack, then pull each node toward its neighbours. */
    for (i32 r = 0; r <= L_maxrank; r++) {
        i32 a = L_rank_at[r], b = L_rank_at[r + 1];
        f32 x = 0;
        for (i32 i = a; i < b; i++) {
            if (i > a) x += dg_sep(&L_n[L_rank[i - 1]], &L_n[L_rank[i]]);
            L_n[L_rank[i]].pos = x;
        }
        for (i32 i = a; i < b; i++) L_n[L_rank[i]].pos -= x * 0.5f;
    }
    for (u32 it = 0; it < 6; it++) {
        dg_cluster_extents();
        for (i32 r = 1; r <= L_maxrank; r++) dg_place_rank(r, -1);
        dg_cluster_extents();
        for (i32 r = L_maxrank - 1; r >= 0; r--) dg_place_rank(r, 1);
    }
    dg_cluster_extents();
    for (i32 r = 0; r <= L_maxrank; r++) dg_place_rank(r, 0);
    dg_cluster_extents();
    for (i32 r = 0; r <= L_maxrank; r++) dg_place_rank(r, 0);
    g_cb_valid = false;
    dg_separate_units();

    /* Rank axis: each rank's band, with room for subgraph titles and padding
     * before the rank a subgraph starts at and after the one it ends at. */
    static f32 thick[DG_MAX_LRANK + 2], before[DG_MAX_LRANK + 2], after[DG_MAX_LRANK + 2],
               start[DG_MAX_LRANK + 2];
    for (i32 r = 0; r <= L_maxrank; r++) { thick[r] = 0; before[r] = 0; after[r] = 0; }
    for (u32 i = 0; i < L_nn; i++) {
        f32 s = dg_rank_size(&L_n[i]);
        if (s > thick[L_n[i].rank]) thick[L_n[i].rank] = s;
    }
    for (u32 g = 0; g < g_ngroups; g++) {
        if (!g_groups[g].used) continue;
        i32 lo = 1 << 30, hi = -1;
        for (u32 i = 0; i < L_nn; i++) {
            if (!dg_group_within(L_n[i].group, (i32)g)) continue;
            if (L_n[i].rank < lo) lo = L_n[i].rank;
            if (L_n[i].rank > hi) hi = L_n[i].rank;
        }
        if (hi < 0) continue;
        before[lo] += dg_cluster_pad() + (L_horizontal ? 0.0f : dg_title_band((i32)g));
        after[hi]  += dg_cluster_pad();
    }
    f32 half_gap = (L_horizontal ? g_cw * 2.6f : g_ch * 1.25f) * (0.35f + 0.65f * g_gap);
    f32 cursor = 0;
    for (i32 r = 0; r <= L_maxrank; r++) {
        if (r > 0) cursor += half_gap;
        cursor += before[r];
        start[r] = cursor;
        cursor += thick[r] + after[r];
    }
    /* Final top-left of every layout node, kept in pos (x) and bc (y) from
     * here on; a real node's also goes back to g_nodes. */
    f32 extent = cursor;
    for (u32 i = 0; i < L_nn; i++) {
        LNode *n = &L_n[i];
        f32 rs = start[n->rank] + (thick[n->rank] - dg_rank_size(n)) * 0.5f;
        if (g_dir == 2 || g_dir == 3) rs = extent - rs - dg_rank_size(n);
        f32 cs = n->pos - dg_cross_size(n) * 0.5f;
        f32 x = L_horizontal ? rs : cs;
        f32 y = L_horizontal ? cs : rs;
        if (n->real >= 0) { g_nodes[n->real].x = x; g_nodes[n->real].y = y; }
        n->pos = x;
        n->bc = y;
    }
    /* Subgraph boxes, innermost first: their members, the dummies of the
     * links inside them, and the boxes nested in them. */
    for (i32 g = (i32)g_ngroups - 1; g >= 0; g--) {
        DGroup *gp = &g_groups[g];
        gp->w = gp->h = 0;
        if (!gp->used) continue;
        f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (u32 i = 0; i < L_nn; i++) {
            if (L_n[i].group != g) continue;
            f32 nx = L_n[i].pos, ny = L_n[i].bc;
            if (nx < x0) x0 = nx;
            if (ny < y0) y0 = ny;
            if (nx + L_n[i].w > x1) x1 = nx + L_n[i].w;
            if (ny + L_n[i].h > y1) y1 = ny + L_n[i].h;
        }
        for (u32 c = (u32)g + 1; c < g_ngroups; c++) {
            const DGroup *cg = &g_groups[c];
            if (cg->parent != g || cg->w <= 0) continue;
            if (cg->x < x0) x0 = cg->x;
            if (cg->y < y0) y0 = cg->y;
            if (cg->x + cg->w > x1) x1 = cg->x + cg->w;
            if (cg->y + cg->h > y1) y1 = cg->y + cg->h;
        }
        if (x1 < x0) { gp->used = false; continue; }
        f32 pad = dg_cluster_pad(), band = dg_title_band(g);
        f32 padx = L_horizontal ? g_cw * 1.6f : pad;
        f32 pady = L_horizontal ? pad : g_ch * 0.7f;
        gp->x = x0 - padx;
        gp->w = (x1 - x0) + padx * 2.0f;
        gp->y = y0 - pady - band;
        gp->h = (y1 - y0) + pady * 2.0f + band;
        u32 tr, tc;
        dg_text_metrics(gp->label, &tr, &tc);
        f32 need = (f32)tc * g_cw + g_cw * 2.0f;
        if (gp->w < need) { gp->x -= (need - gp->w) * 0.5f; gp->w = need; }
    }
    return true;
}

/* ---- graph edges ---- */

static bool dg_inside_node(const DNode *n, f32 px, f32 py) {
    f32 cx = n->x + n->w * 0.5f, cy = n->y + n->h * 0.5f;
    f32 hw = n->w * 0.5f, hh = n->h * 0.5f;
    if (hw <= 0 || hh <= 0) return false;
    f32 dx = fabsf(px - cx), dy = fabsf(py - cy);
    switch (n->shape) {
    case SH_DIAMOND: return dx / hw + dy / hh <= 1.0f;
    case SH_CIRCLE: case SH_DOT: case SH_RING:
        return (dx * dx) / (hw * hw) + (dy * dy) / (hh * hh) <= 1.0f;
    default: return dx <= hw && dy <= hh;
    }
}

static bool dg_inside_box(const DGroup *g, f32 px, f32 py) {
    return px >= g->x && px <= g->x + g->w && py >= g->y && py <= g->y + g->h;
}

typedef struct { const DNode *n; const DGroup *g; } DEnd;

static bool dg_inside_end(DEnd e, f32 x, f32 y) {
    return e.g ? dg_inside_box(e.g, x, y) : (e.n ? dg_inside_node(e.n, x, y) : false);
}

/* Cut the polyline where it leaves `from` at the start and enters `to` at the
 * end (bisection on the crossing segment). Returns the new point count. */
static u32 dg_clip_path(f32 *px, f32 *py, u32 n, DEnd from, DEnd to) {
    if (n < 2) return n;
    u32 s = 0;
    while (s + 1 < n && dg_inside_end(from, px[s + 1], py[s + 1])) s++;
    if (s + 1 < n && dg_inside_end(from, px[s], py[s])) {
        f32 ax = px[s], ay = py[s], bx = px[s + 1], by = py[s + 1];
        for (u32 it = 0; it < 12; it++) {
            f32 mx = (ax + bx) * 0.5f, my = (ay + by) * 0.5f;
            if (dg_inside_end(from, mx, my)) { ax = mx; ay = my; } else { bx = mx; by = my; }
        }
        px[s] = bx; py[s] = by;
    }
    if (s) { memmove(px, px + s, sizeof(f32) * (n - s)); memmove(py, py + s, sizeof(f32) * (n - s)); n -= s; }
    u32 e = n - 1;
    while (e > 0 && dg_inside_end(to, px[e - 1], py[e - 1])) e--;
    if (e > 0 && dg_inside_end(to, px[e], py[e])) {
        f32 ax = px[e], ay = py[e], bx = px[e - 1], by = py[e - 1];
        for (u32 it = 0; it < 12; it++) {
            f32 mx = (ax + bx) * 0.5f, my = (ay + by) * 0.5f;
            if (dg_inside_end(to, mx, my)) { ax = mx; ay = my; } else { bx = mx; by = my; }
        }
        px[e] = bx; py[e] = by;
    }
    return e + 1;
}

/* Walk `len` back from the end of the polyline: the point there, and the
 * polyline shortened to end at it. */
static u32 dg_trim_end(f32 *px, f32 *py, u32 n, f32 len, f32 *bx, f32 *by) {
    f32 left = len;
    u32 i = n - 1;
    while (i > 0) {
        f32 dx = px[i] - px[i - 1], dy = py[i] - py[i - 1];
        f32 d = sqrtf(dx*dx + dy*dy);
        if (d >= left && d > 1e-4f) {
            f32 t = left / d;
            *bx = px[i] - dx * t; *by = py[i] - dy * t;
            px[i] = *bx; py[i] = *by;
            return i + 1;
        }
        left -= d;
        i--;
    }
    *bx = px[0]; *by = py[0];
    return 1;
}

static void dg_reverse(f32 *px, f32 *py, u32 n) {
    for (u32 i = 0; i < n / 2; i++) {
        f32 t = px[i]; px[i] = px[n - 1 - i]; px[n - 1 - i] = t;
        t = py[i]; py[i] = py[n - 1 - i]; py[n - 1 - i] = t;
    }
}

/* End decoration with its tip at (tx, ty), pointing along (ux, uy). */
static void em_marker(u8 mk, f32 tx, f32 ty, f32 ux, f32 uy, f32 t, u8 ink) {
    f32 nx = -uy, ny = ux;
    f32 L = g_ch * 0.62f, hw = g_ch * 0.31f;
    switch (mk) {
    case MK_ARROW:
        em_wedge(P_EDGE, tx, ty, tx - ux * L, ty - uy * L, hw, ink);
        break;
    case MK_OPEN: {
        f32 px[3] = { tx - ux * L + nx * hw, tx, tx - ux * L - nx * hw };
        f32 py[3] = { ty - uy * L + ny * hw, ty, ty - uy * L - ny * hw };
        em_poly(P_EDGE, px, py, 3, false, t, ink);
        break;
    }
    case MK_CROSS: {
        f32 h = g_ch * 0.26f;
        f32 cx = tx - ux * h * 1.2f, cy = ty - uy * h * 1.2f;
        em_seg(P_EDGE, cx - (ux + nx) * h, cy - (uy + ny) * h, cx + (ux + nx) * h, cy + (uy + ny) * h, t * 1.3f, ink);
        em_seg(P_EDGE, cx - (ux - nx) * h, cy - (uy - ny) * h, cx + (ux - nx) * h, cy + (uy - ny) * h, t * 1.3f, ink);
        break;
    }
    case MK_CIRCLE: {
        f32 r = g_ch * 0.22f;
        em_box(P_NODE, tx - ux * r - r, ty - uy * r - r, r * 2, r * 2, r, t, DI_SURFACE, ink);
        break;
    }
    case MK_TRIANGLE: {
        f32 bx = tx - ux * L * 1.1f, by = ty - uy * L * 1.1f;
        f32 hh = hw * 1.15f;
        em_wedge(P_EDGE, tx, ty, bx, by, hh, DI_SURFACE);
        f32 px[3] = { tx, bx + nx * hh, bx - nx * hh };
        f32 py[3] = { ty, by + ny * hh, by - ny * hh };
        em_poly(P_EDGE, px, py, 3, true, t, ink);
        break;
    }
    case MK_DIAMOND_F: case MK_DIAMOND_O: {
        f32 dl = L * 1.5f, dh = hw * 0.85f;
        f32 mx = tx - ux * dl * 0.5f, my = ty - uy * dl * 0.5f;
        f32 ex = tx - ux * dl, ey = ty - uy * dl;
        u8 fill = mk == MK_DIAMOND_F ? ink : DI_SURFACE;
        em_wedge(P_EDGE, tx, ty, mx, my, dh, fill);
        em_wedge(P_EDGE, ex, ey, mx, my, dh, fill);
        f32 px[4] = { tx, mx + nx * dh, ex, mx - nx * dh };
        f32 py[4] = { ty, my + ny * dh, ey, my - ny * dh };
        em_poly(P_EDGE, px, py, 4, true, t, ink);
        break;
    }
    case MK_ER_ONE: case MK_ER_ZERO_ONE: case MK_ER_MANY: case MK_ER_ZERO_MANY: {
        bool many = (mk == MK_ER_MANY || mk == MK_ER_ZERO_MANY);
        bool zero = (mk == MK_ER_ZERO_ONE || mk == MK_ER_ZERO_MANY);
        f32 bar = g_ch * 0.34f;
        if (many) {
            f32 fx = tx - ux * L, fy = ty - uy * L;
            em_seg(P_EDGE, fx, fy, tx + nx * bar, ty + ny * bar, t, ink);
            em_seg(P_EDGE, fx, fy, tx - nx * bar, ty - ny * bar, t, ink);
        } else {
            f32 bx = tx - ux * L * 0.45f, by = ty - uy * L * 0.45f;
            em_seg(P_EDGE, bx + nx * bar, by + ny * bar, bx - nx * bar, by - ny * bar, t, ink);
        }
        f32 b2 = L * (many ? 1.25f : 0.85f);
        if (zero) {
            f32 r = g_ch * 0.2f;
            f32 cx = tx - ux * (b2 + r), cy = ty - uy * (b2 + r);
            em_box(P_NODE, cx - r, cy - r, r * 2, r * 2, r, t, DI_SURFACE, ink);
        } else {
            f32 bx = tx - ux * b2, by = ty - uy * b2;
            em_seg(P_EDGE, bx + nx * bar, by + ny * bar, bx - nx * bar, by - ny * bar, t, ink);
        }
        break;
    }
    default: break;
    }
}

/* How far back from the tip the line stops for a marker of this kind. */
static f32 dg_marker_back(u8 mk) {
    switch (mk) {
    case MK_ARROW:      return g_ch * 0.55f;
    case MK_TRIANGLE:   return g_ch * 0.62f * 1.1f;
    case MK_DIAMOND_F: case MK_DIAMOND_O: return g_ch * 0.62f * 1.5f;
    case MK_CIRCLE:     return g_ch * 0.44f;
    default:            return 0.0f;
    }
}

/* Emit one path with its style and end markers. */
void em_path(f32 *px, f32 *py, u32 n, u8 style, u8 mk_from, u8 mk_to, u8 ink) {
    if (n < 2) return;
    f32 t = dg_stroke_w();
    f32 w = style == ES_THICK ? t * 2.2f : t;
    f32 tip_x = px[n - 1], tip_y = py[n - 1];
    f32 start_x = px[0], start_y = py[0];
    /* Direction at each end, from a point one head length along the path. */
    f32 ux = 0, uy = 0, sx = 0, sy = 0;
    {
        f32 bx, by;
        f32 cx[512], cy[512];
        u32 m = n < 512 ? n : 512;
        memcpy(cx, px, sizeof(f32) * m); memcpy(cy, py, sizeof(f32) * m);
        dg_trim_end(cx, cy, m, g_ch * 0.6f, &bx, &by);
        f32 dx = tip_x - bx, dy = tip_y - by, d = sqrtf(dx*dx + dy*dy);
        if (d > 1e-4f) { ux = dx / d; uy = dy / d; }
        memcpy(cx, px, sizeof(f32) * m); memcpy(cy, py, sizeof(f32) * m);
        dg_reverse(cx, cy, m);
        dg_trim_end(cx, cy, m, g_ch * 0.6f, &bx, &by);
        dx = start_x - bx; dy = start_y - by; d = sqrtf(dx*dx + dy*dy);
        if (d > 1e-4f) { sx = dx / d; sy = dy / d; }
    }
    f32 bx, by;
    if (dg_marker_back(mk_to) > 0) n = dg_trim_end(px, py, n, dg_marker_back(mk_to), &bx, &by);
    if (dg_marker_back(mk_from) > 0 && n >= 2) {
        dg_reverse(px, py, n);
        n = dg_trim_end(px, py, n, dg_marker_back(mk_from), &bx, &by);
        dg_reverse(px, py, n);
    }
    if (n >= 2) {
        if (style == ES_DOTTED) em_dashed(P_EDGE, px, py, n, w, g_ch * 0.32f, g_ch * 0.24f, ink);
        else em_poly(P_EDGE, px, py, n, false, w, ink);
    }
    if (ux != 0 || uy != 0) em_marker(mk_to, tip_x, tip_y, ux, uy, t, ink);
    if (sx != 0 || sy != 0) em_marker(mk_from, start_x, start_y, sx, sy, t, ink);
}

/* One cubic from the current point (x0, y0), flattened into short segments. */
u32 dg_bezier(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
              f32 *px, f32 *py, u32 n, u32 cap) {
    /* As many chords as its bend needs (the cubic's second differences bound
     * how far it leaves a chord): a straight stretch is one. */
    f32 ax = x0 - 2.0f * x1 + x2, ay = y0 - 2.0f * y1 + y2;
    f32 bx = x1 - 2.0f * x2 + x3, by = y1 - 2.0f * y2 + y3;
    f32 m = fmaxf(hypotf(ax, ay), hypotf(bx, by));
    u32 steps = (u32)ceilf(sqrtf(0.75f * m / (g_ch * DG_FLAT_TOL)));
    if (steps < 1) steps = 1;
    if (steps > 32) steps = 32;
    for (u32 k = 1; k <= steps && n < cap; k++) {
        f32 t = (f32)k / (f32)steps, it = 1.0f - t;
        f32 a = it * it * it, b = 3 * it * it * t, c = 3 * it * t * t, d = t * t * t;
        px[n] = a * x0 + b * x1 + c * x2 + d * x3;
        py[n] = a * y0 + b * y1 + c * y2 + d * y3;
        n++;
    }
    return n;
}

/* The path through the chain's points as one uniform cubic B-spline, clamped
 * at both ends: it starts and ends on the end points and bends through the
 * points between without a kink or an S at each of them. */
u32 dg_smooth(const f32 *qx, const f32 *qy, u32 q, f32 *px, f32 *py, u32 cap) {
    u32 n = 0;
    if (q == 0 || cap < 2) return 0;
    px[n] = qx[0]; py[n] = qy[0]; n++;
    if (q == 1) return n;
    if (q == 2) { px[n] = qx[1]; py[n] = qy[1]; return n + 1; }
    /* the first sixth of the way, straight, then a cubic per point */
    f32 cx = (5.0f * qx[0] + qx[1]) / 6.0f, cy = (5.0f * qy[0] + qy[1]) / 6.0f;
    px[n] = cx; py[n] = cy; n++;
    for (u32 i = 2; i <= q && n < cap; i++) {
        f32 ax = qx[i - 2], ay = qy[i - 2], bx = qx[i - 1], by = qy[i - 1];
        f32 x = i < q ? qx[i] : qx[q - 1], y = i < q ? qy[i] : qy[q - 1];
        if (i == q) { ax = qx[q - 2]; ay = qy[q - 2]; bx = qx[q - 1]; by = qy[q - 1]; }
        f32 ex = (ax + 4.0f * bx + x) / 6.0f, ey = (ay + 4.0f * by + y) / 6.0f;
        n = dg_bezier(cx, cy, (2.0f * ax + bx) / 3.0f, (2.0f * ay + by) / 3.0f,
                      (ax + 2.0f * bx) / 3.0f, (ay + 2.0f * by) / 3.0f, ex, ey, px, py, n, cap);
        cx = ex; cy = ey;
    }
    if (n < cap) { px[n] = qx[q - 1]; py[n] = qy[q - 1]; n++; }
    return n;
}

void em_chip(const char *label, f32 cx, f32 cy) {
    f32 w = dg_label_chip_w(label), h = dg_label_chip_h(label);
    em_box(P_NODE, cx - w * 0.5f, cy - h * 0.5f, w, h, g_ch * 0.22f, 0.0f, DI_CHIP, DI_NONE);
    em_text_box(label, cx - w * 0.5f, cy - h * 0.5f, w, h, DI_TEXT_DIM);
}

/* Every drawn edge's chain of points, from its source to its target, made
 * once so the ends can be spread before any is drawn. */
#define DG_MAX_QP (DG_MAX_LN + DG_MAX_EDGES * 2)
static f32  G_qx[DG_MAX_QP], G_qy[DG_MAX_QP];
static u32  G_q_at[DG_MAX_EDGES], G_q_n[DG_MAX_EDGES];
static f32  G_lab_x[DG_MAX_EDGES], G_lab_y[DG_MAX_EDGES];
static bool G_lab_at[DG_MAX_EDGES];

/* Where drawn edges cross each subgraph's title row, as runs along it. */
#define DG_MAX_CROSS 48
static f32 G_cross_lo[DG_MAX_GROUPS][DG_MAX_CROSS], G_cross_hi[DG_MAX_GROUPS][DG_MAX_CROSS];
static u32 G_ncross[DG_MAX_GROUPS];

static f32 dg_title_y(const DGroup *g) { return g->y + g_ch * 0.25f; }
static f32 dg_title_h(const DGroup *g) {
    u32 rows, cells;
    dg_text_metrics(g->label, &rows, &cells);
    return (f32)rows * g_ch * DG_ROW_H + g_ch * 0.3f;
}

/* The cross axis is x in a top-down layout, y in a left-right one. */
static f32 dg_cross_of(f32 x, f32 y) { return L_horizontal ? y : x; }
static f32 dg_rank_of(f32 x, f32 y)  { return L_horizontal ? x : y; }
static void dg_point(f32 cross, f32 rank, f32 *x, f32 *y) {
    if (L_horizontal) { *x = rank; *y = cross; } else { *x = cross; *y = rank; }
}

static void dg_edge_chain(u32 e, u32 at) {
    const DEdge *ed = &g_edges[e];
    u32 q = 0;
    f32 *qx = G_qx + at, *qy = G_qy + at;
    G_lab_at[e] = false;
    if (L_chain_n[e] >= 2) {
        for (i32 i = 0; i < L_chain_n[e] && at + q < DG_MAX_QP; i++) {
            const LNode *ln = &L_n[L_chain[L_chain_at[e] + i]];
            f32 cx = ln->pos + ln->w * 0.5f, cy = ln->bc + ln->h * 0.5f;
            if (ln->label) { G_lab_x[e] = cx; G_lab_y[e] = cy; G_lab_at[e] = true; }
            qx[q] = cx; qy[q] = cy; q++;
        }
        if (L_chain_rev[e]) dg_reverse(qx, qy, q);
    } else if (at + 2 <= DG_MAX_QP) {
        const DNode *a = &g_nodes[ed->from], *b = &g_nodes[ed->to];
        const DGroup *ga = a->proxy >= 0 ? &g_groups[a->proxy] : NULL;
        const DGroup *gb = b->proxy >= 0 ? &g_groups[b->proxy] : NULL;
        qx[0] = ga ? ga->x + ga->w * 0.5f : a->x + a->w * 0.5f;
        qy[0] = ga ? ga->y + ga->h * 0.5f : a->y + a->h * 0.5f;
        qx[1] = gb ? gb->x + gb->w * 0.5f : b->x + b->w * 0.5f;
        qy[1] = gb ? gb->y + gb->h * 0.5f : b->y + b->h * 0.5f;
        q = 2;
    }
    G_q_at[e] = at;
    G_q_n[e] = q;
}

static bool dg_drawn_edge(u32 e) {
    const DEdge *ed = &g_edges[e];
    return ed->from >= 0 && ed->to >= 0 && ed->from != ed->to && G_q_n[e] >= 2;
}

/* Edges that meet a node on the same side meet it side by side: their ends
 * are spread over the middle of that side, in the order they arrive in, so
 * their heads do not land on one another. */
static void dg_spread_ends(void) {
    typedef struct { u32 e; bool tail; f32 at; } End;
    static End ends[DG_MAX_EDGES * 2];
    for (u32 v = 0; v < g_nnodes; v++) {
        const DNode *nd = &g_nodes[v];
        if (nd->proxy >= 0) continue;
        f32 cc = dg_cross_of(nd->x + nd->w * 0.5f, nd->y + nd->h * 0.5f);
        f32 rc = dg_rank_of(nd->x + nd->w * 0.5f, nd->y + nd->h * 0.5f);
        f32 size = L_horizontal ? nd->h : nd->w;
        for (int side = 0; side < 2; side++) {
            u32 n = 0;
            for (u32 e = 0; e < g_nedges; e++) {
                if (!dg_drawn_edge(e)) continue;
                const DEdge *ed = &g_edges[e];
                u32 at = G_q_at[e], qn = G_q_n[e];
                for (int which = 0; which < 2; which++) {
                    bool tail = which == 0;          /* the edge's source end */
                    if ((tail ? ed->from : ed->to) != (i32)v) continue;
                    u32 nb = tail ? at + 1 : at + qn - 2;
                    f32 r = dg_rank_of(G_qx[nb], G_qy[nb]);
                    if ((r < rc) != (side == 0)) continue;
                    ends[n++] = (End){ e, tail, dg_cross_of(G_qx[nb], G_qy[nb]) };
                }
            }
            if (n < 2) continue;
            for (u32 i = 1; i < n; i++) {
                End x = ends[i]; u32 j = i;
                while (j > 0 && ends[j - 1].at > x.at) { ends[j] = ends[j - 1]; j--; }
                ends[j] = x;
            }
            f32 span = fminf(size * 0.55f, (f32)(n - 1) * g_cw * 2.4f);
            for (u32 i = 0; i < n; i++) {
                u32 e = ends[i].e;
                u32 k = ends[i].tail ? G_q_at[e] : G_q_at[e] + G_q_n[e] - 1;
                f32 c = cc - span * 0.5f + span * (f32)i / (f32)(n - 1);
                dg_point(c, rc, &G_qx[k], &G_qy[k]);
            }
        }
    }
}

/* Does the segment a-b pass through the box (x0, y0)-(x1, y1)? */
static bool dg_seg_hits_box(f32 ax, f32 ay, f32 bx, f32 by, f32 x0, f32 y0, f32 x1, f32 y1) {
    f32 t0 = 0.0f, t1 = 1.0f, dx = bx - ax, dy = by - ay;
    const f32 p[4] = { -dx, dx, -dy, dy };
    const f32 q[4] = { ax - x0, x1 - ax, ay - y0, y1 - ay };
    for (int i = 0; i < 4; i++) {
        if (fabsf(p[i]) < 1e-6f) { if (q[i] < 0) return false; continue; }
        f32 t = q[i] / p[i];
        if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
        else          { if (t < t0) return false; if (t < t1) t1 = t; }
    }
    return t1 > t0;
}

/* An edge goes round a subgraph neither of its ends is in: where the step
 * from one of its points to the next would cut the box — a point beside the
 * box to one past its end, or the other way — it first runs on along the
 * rank axis to clear the box, then turns (the corner doubled, so the curve
 * holds to it). */
static u32 dg_detour(u32 e, const f32 *qx, const f32 *qy, u32 q, f32 *ox, f32 *oy, u32 cap) {
    const DEdge *ed = &g_edges[e];
    const DNode *a = &g_nodes[ed->from], *b = &g_nodes[ed->to];
    i32 ga = a->proxy >= 0 ? a->proxy : a->group, gb = b->proxy >= 0 ? b->proxy : b->group;
    f32 m = g_ch * 0.25f, clear = g_ch * 0.9f;
    u32 n = 0;
    for (u32 i = 0; i < q && n < cap; i++) {
        ox[n] = qx[i]; oy[n] = qy[i]; n++;
        if (i + 1 >= q) break;
        for (u32 g = 0; g < g_ngroups && n + 2 < cap; g++) {
            const DGroup *gp = &g_groups[g];
            if (!gp->used || gp->w <= 0) continue;
            if (dg_group_within(ga, (i32)g) || dg_group_within(gb, (i32)g)) continue;
            if (!dg_seg_hits_box(qx[i], qy[i], qx[i + 1], qy[i + 1],
                                 gp->x - m, gp->y - m, gp->x + gp->w + m, gp->y + gp->h + m))
                continue;
            f32 c0 = dg_cross_of(gp->x, gp->y), c1 = dg_cross_of(gp->x + gp->w, gp->y + gp->h);
            f32 r0 = dg_rank_of(gp->x, gp->y), r1 = dg_rank_of(gp->x + gp->w, gp->y + gp->h);
            f32 ca = dg_cross_of(qx[i], qy[i]), ra = dg_rank_of(qx[i], qy[i]);
            f32 cb = dg_cross_of(qx[i + 1], qy[i + 1]), rb = dg_rank_of(qx[i + 1], qy[i + 1]);
            /* the corner stays between the box's edge and the point past it */
            f32 cx, rx;
            if ((ca < c0 || ca > c1) && ra >= r0 && ra <= r1 && (rb > r1 || rb < r0)) {
                cx = ca;
                rx = rb > r1 ? r1 + fminf(clear, (rb - r1) * 0.5f) : r0 - fminf(clear, (r0 - rb) * 0.5f);
            } else if ((cb < c0 || cb > c1) && rb >= r0 && rb <= r1 && (ra < r0 || ra > r1)) {
                cx = cb;
                rx = ra < r0 ? r0 - fminf(clear, (r0 - ra) * 0.5f) : r1 + fminf(clear, (ra - r1) * 0.5f);
            } else continue;
            dg_point(cx, rx, &ox[n], &oy[n]); n++;
            ox[n] = ox[n - 1]; oy[n] = oy[n - 1]; n++;
            break;
        }
    }
    return n;
}

/* Note where the polyline crosses each subgraph's title row. */
static void dg_note_title_crossings(const f32 *px, const f32 *py, u32 n) {
    for (u32 g = 0; g < g_ngroups; g++) {
        const DGroup *gp = &g_groups[g];
        if (!gp->used || gp->w <= 0 || !gp->label[0]) continue;
        f32 y0 = dg_title_y(gp), y1 = y0 + dg_title_h(gp);
        for (u32 i = 0; i + 1 < n && G_ncross[g] < DG_MAX_CROSS; i++) {
            f32 ax = px[i], ay = py[i], bx = px[i + 1], by = py[i + 1];
            if ((ay < y0 && by < y0) || (ay > y1 && by > y1)) continue;
            /* the part of the step inside the row */
            f32 ta = 0.0f, tb = 1.0f;
            if (fabsf(by - ay) > 1e-6f) {
                f32 u0 = (y0 - ay) / (by - ay), u1 = (y1 - ay) / (by - ay);
                if (u0 > u1) { f32 t = u0; u0 = u1; u1 = t; }
                ta = fmaxf(0.0f, u0); tb = fminf(1.0f, u1);
            }
            if (tb < ta) continue;
            f32 xa = ax + (bx - ax) * ta, xb = ax + (bx - ax) * tb;
            f32 lo = fminf(xa, xb), hi = fmaxf(xa, xb);
            if (hi < gp->x || lo > gp->x + gp->w) continue;
            G_cross_lo[g][G_ncross[g]] = lo;
            G_cross_hi[g][G_ncross[g]] = hi;
            G_ncross[g]++;
        }
    }
}

static void em_graph_edges(void) {
    static f32 wx[DG_MAX_LN * 2], wy[DG_MAX_LN * 2];
    static f32 px[4096], py[4096];
    u32 at = 0;
    for (u32 e = 0; e < g_nedges; e++) {
        G_q_n[e] = 0;
        const DEdge *ed = &g_edges[e];
        if (ed->from < 0 || ed->to < 0 || ed->from == ed->to) continue;
        dg_edge_chain(e, at);
        at += G_q_n[e];
    }
    dg_spread_ends();
    for (u32 g = 0; g < g_ngroups; g++) G_ncross[g] = 0;
    for (u32 e = 0; e < g_nedges; e++) {
        const DEdge *ed = &g_edges[e];
        if (ed->from < 0 || ed->to < 0) continue;
        DEnd from = { &g_nodes[ed->from], NULL }, to = { &g_nodes[ed->to], NULL };
        if (g_nodes[ed->from].proxy >= 0) from = (DEnd){ NULL, &g_groups[g_nodes[ed->from].proxy] };
        if (g_nodes[ed->to].proxy >= 0)   to   = (DEnd){ NULL, &g_groups[g_nodes[ed->to].proxy] };
        u8 ink = DI_EDGE;
        if (ed->from == ed->to && g_nodes[ed->from].proxy < 0) {     /* self loop */
            /* out of the right side, round, and back in lower down */
            const DNode *a = &g_nodes[ed->from];
            f32 x0 = a->x + a->w, r = g_cw * 2.2f;
            f32 lx[4] = { x0, x0 + r, x0 + r, x0 };
            f32 ly[4] = { a->y + a->h * 0.3f, a->y + a->h * 0.3f, a->y + a->h * 0.7f, a->y + a->h * 0.7f };
            em_path(lx, ly, 4, ed->style, MK_NONE, ed->mk_to, ink);
            if (ed->label[0]) {
                f32 w = dg_label_chip_w(ed->label);
                em_chip(ed->label, x0 + r + w * 0.5f + g_cw * 0.3f, a->y + a->h * 0.5f);
            }
            continue;
        }
        if (G_q_n[e] < 2) continue;
        f32 lab_x = G_lab_x[e], lab_y = G_lab_y[e];
        bool lab_at = G_lab_at[e];
        u32 q = dg_detour(e, G_qx + G_q_at[e], G_qy + G_q_at[e], G_q_n[e], wx, wy, DG_MAX_LN * 2);
        u32 n = dg_smooth(wx, wy, q, px, py, 4096);
        n = dg_clip_path(px, py, n, from, to);
        if (!lab_at && ed->label[0] && n >= 2) {
            /* the polyline's middle by length */
            f32 total = 0;
            for (u32 i = 0; i + 1 < n; i++) total += hypotf(px[i+1]-px[i], py[i+1]-py[i]);
            f32 half = total * 0.5f, acc = 0;
            lab_x = (px[0] + px[n-1]) * 0.5f; lab_y = (py[0] + py[n-1]) * 0.5f;
            for (u32 i = 0; i + 1 < n; i++) {
                f32 d = hypotf(px[i+1]-px[i], py[i+1]-py[i]);
                if (acc + d >= half && d > 0) {
                    f32 t = (half - acc) / d;
                    lab_x = px[i] + (px[i+1]-px[i]) * t; lab_y = py[i] + (py[i+1]-py[i]) * t;
                    break;
                }
                acc += d;
            }
            lab_at = true;
        }
        if (n >= 2) dg_note_title_crossings(px, py, n);
        if (n >= 2) em_path(px, py, n, ed->style, ed->mk_from, ed->mk_to, ink);
        if (lab_at && ed->label[0]) em_chip(ed->label, lab_x, lab_y);
    }
}

/* ---- graph nodes ---- */

static void em_graph_node(const DNode *n) {
    f32 t = dg_stroke_w();
    f32 x0 = n->x, y0 = n->y, x1 = x0 + n->w, y1 = y0 + n->h;
    f32 mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    u8 fill = DI_NODE_FILL, edge = DI_NODE_STROKE;
    switch (n->shape) {
    case SH_DIAMOND: case SH_HEX: case SH_PARA: {
        f32 px[6], py[6];
        u32 k = 0;
        if (n->shape == SH_DIAMOND) {
            /* Two triangles from the top and bottom corners, their wide
             * angles at the apex, meeting a hair past the middle. */
            f32 o = fmaxf(0.5f, t * 0.5f);
            em_wedge(P_NODE, mx, y0, mx, my + o, n->w * 0.5f, fill);
            em_wedge(P_NODE, mx, y1, mx, my - o, n->w * 0.5f, fill);
            px[0] = mx; py[0] = y0; px[1] = x1; py[1] = my;
            px[2] = mx; py[2] = y1; px[3] = x0; py[3] = my; k = 4;
        } else if (n->shape == SH_HEX) {
            f32 s = fminf(n->h * 0.35f, n->w * 0.25f);
            em_wedge(P_NODE, x0, my, x0 + s, my, n->h * 0.5f, fill);
            em_wedge(P_NODE, x1, my, x1 - s, my, n->h * 0.5f, fill);
            em_stroke(P_NODE, x0 + s, my, x1 - s, my, 0, 1, 0, 1, n->h, fill);
            px[0] = x0; py[0] = my; px[1] = x0 + s; py[1] = y0; px[2] = x1 - s; py[2] = y0;
            px[3] = x1; py[3] = my; px[4] = x1 - s; py[4] = y1; px[5] = x0 + s; py[5] = y1; k = 6;
        } else {
            f32 s = fminf(n->h * 0.4f, n->w * 0.25f);
            /* a band whose across-direction leans: every row shifted by the slant */
            f32 lean = s / n->h;
            em_stroke(P_NODE, x0 + s * 0.5f, my, x1 - s * 0.5f, my, -lean, 1, -lean, 1, n->h, fill);
            px[0] = x0 + s; py[0] = y0; px[1] = x1; py[1] = y0;
            px[2] = x1 - s; py[2] = y1; px[3] = x0; py[3] = y1; k = 4;
        }
        em_poly(P_NODE, px, py, k, true, t, edge);
        break;
    }
    case SH_DOT:
        em_box(P_NODE, x0, y0, n->w, n->h, n->w * 0.5f, 0, DI_SOLID, DI_NONE);
        break;
    case SH_RING: {
        em_box(P_NODE, x0, y0, n->w, n->h, n->w * 0.5f, t * 1.4f, DI_SURFACE, DI_SOLID);
        f32 in = n->w * 0.26f;
        em_box(P_TOP, x0 + in, y0 + in, n->w - in * 2, n->h - in * 2, (n->w - in * 2) * 0.5f, 0,
               DI_SOLID, DI_NONE);
        break;
    }
    case SH_BAR:
        em_box(P_NODE, x0, y0, n->w, n->h, fminf(n->w, n->h) * 0.3f, 0, DI_SOLID, DI_NONE);
        break;
    case SH_CYLINDER: {
        f32 ry = fminf(n->h * 0.14f, g_ch * 0.45f);
        em_box(P_NODE, x0, y0, n->w, n->h, fminf(n->w * 0.5f, ry * 1.6f), t, fill, edge);
        f32 px[17], py[17];
        for (u32 k = 0; k <= 16; k++) {
            f32 a = 3.14159265f * (f32)k / 16.0f;
            px[k] = mx - cosf(a) * n->w * 0.5f;
            py[k] = y0 + ry + sinf(a) * ry;
        }
        em_poly(P_TOP, px, py, 17, false, t, edge);
        break;
    }
    case SH_CLASS: case SH_ENTITY: {
        f32 r = g_ch * 0.15f;
        u32 rows, cells;
        dg_text_metrics(n->label, &rows, &cells);
        f32 head = (f32)rows * g_ch * DG_ROW_H + g_ch * 0.9f;
        em_box(P_NODE, x0, y0, n->w, n->h, r, t, fill, edge);
        if (n->shape == SH_ENTITY)
            em_box(P_TOP, x0 + t, y0 + t, n->w - t * 2, head - t, r, 0, DI_ACTIVATION, DI_NONE);
        f32 y = y0 + head;
        for (u32 part = 0; part < 2; part++) {
            const char *b = part ? n->body2 : n->body;
            if (part == 1 && n->shape == SH_ENTITY) break;
            em_seg(P_TOP, x0, y, x1, y, t, edge);
            u32 br, bc;
            if (b[0]) {
                dg_text_metrics(b, &br, &bc);
                y += (f32)br * g_ch * DG_ROW_H + g_ch * 0.6f;
            } else if (n->shape == SH_CLASS) y += g_ch * 0.6f;
        }
        break;
    }
    default: {
        f32 r = n->shape == SH_STADIUM ? n->h * 0.5f
              : n->shape == SH_CIRCLE ? n->w * 0.5f
              : n->shape == SH_ROUND ? fminf(n->h * 0.3f, g_ch * 0.6f)
              : g_ch * 0.14f;
        em_box(P_NODE, x0, y0, n->w, n->h, r, t, fill, edge);
        if (n->shape == SH_SUBROUTINE) {
            f32 s = g_cw * 0.9f;
            em_seg(P_TOP, x0 + s, y0, x0 + s, y1, t, edge);
            em_seg(P_TOP, x1 - s, y0, x1 - s, y1, t, edge);
        }
        break;
    }
    }
}

static void em_graph_labels(const DNode *n) {
    if (n->shape == SH_CLASS || n->shape == SH_ENTITY) {
        u32 rows, cells;
        dg_text_metrics(n->label, &rows, &cells);
        f32 head = (f32)rows * g_ch * DG_ROW_H + g_ch * 0.9f;
        em_text_box(n->label, n->x, n->y, n->w, head, DI_TEXT);
        f32 y = n->y + head + g_ch * 0.3f;
        for (u32 part = 0; part < 2; part++) {
            const char *b = part ? n->body2 : n->body;
            if (!b[0]) { if (n->shape == SH_CLASS) y += g_ch * 0.6f; continue; }
            em_text_rows(b, n->x + g_cw * 1.5f, y, n->shape == SH_ENTITY ? DI_TEXT_DIM : DI_TEXT);
            u32 br, bc;
            dg_text_metrics(b, &br, &bc);
            y += (f32)br * g_ch * DG_ROW_H + g_ch * 0.6f;
        }
        return;
    }
    if (n->shape == SH_DOT || n->shape == SH_RING || n->shape == SH_BAR) return;
    em_text_box(n->label, n->x, n->y, n->w, n->h, DI_TEXT);
}

/* A subgraph's title, centred on its box's top row unless an edge crosses
 * the row there: then at the left or the right of the row, whichever is
 * clear, and over a plate of the box's own fill when neither is. */
static void em_group_title(u32 g) {
    const DGroup *gp = &g_groups[g];
    u32 rows, cells;
    dg_text_metrics(gp->label, &rows, &cells);
    f32 tw = (f32)cells * g_cw, ty = dg_title_y(gp), th = dg_title_h(gp);
    f32 pad = g_cw * 0.6f;
    f32 cand[3] = { gp->x + (gp->w - tw) * 0.5f, gp->x + g_cw * 1.0f, gp->x + gp->w - tw - g_cw * 1.0f };
    i32 pick = -1;
    for (u32 c = 0; c < 3 && pick < 0; c++) {
        f32 lo = cand[c] - pad, hi = cand[c] + tw + pad;
        if (lo < gp->x || hi > gp->x + gp->w) { if (c) continue; }
        bool clear = true;
        for (u32 k = 0; k < G_ncross[g] && clear; k++)
            if (G_cross_hi[g][k] > lo && G_cross_lo[g][k] < hi) clear = false;
        if (clear) pick = (i32)c;
    }
    f32 x = cand[pick < 0 ? 0 : pick];
    if (pick < 0)
        em_box(P_NODE, x - pad * 0.6f, ty, tw + pad * 1.2f, th, g_ch * 0.2f, 0.0f, DI_GROUP_FILL, DI_NONE);
    em_text_box(gp->label, x, ty, tw, th, DI_GROUP_TEXT);
}

static void em_graph(void) {
    f32 t = dg_stroke_w();
    for (u32 g = 0; g < g_ngroups; g++) {
        const DGroup *gp = &g_groups[g];
        if (!gp->used || gp->w <= 0 || gp->h <= 0) continue;
        em_box(P_BACK, gp->x, gp->y, gp->w, gp->h, g_ch * 0.4f, t, DI_GROUP_FILL, DI_GROUP_STROKE);
    }
    em_graph_edges();
    for (u32 g = 0; g < g_ngroups; g++) {
        const DGroup *gp = &g_groups[g];
        if (!gp->used || gp->w <= 0 || gp->h <= 0 || !gp->label[0]) continue;
        em_group_title(g);
    }
    for (u32 i = 0; i < g_nnodes; i++) {
        if (g_nodes[i].proxy >= 0) continue;
        em_graph_node(&g_nodes[i]);
        em_graph_labels(&g_nodes[i]);
    }
}

/* =========================================================================
 * Sequence layout
 * ========================================================================= */
static f32 dg_bar_w(void) { return g_cw * 1.1f; }

static void dg_layout_seq(void) {
    f32 margin = g_cw * 1.5f;
    f32 gap[DG_MAX_PARTS];
    /* Space each lifeline needs *outside* its head box, for the self-call
     * loops and the side notes that hang off it. */
    f32 need_l[DG_MAX_PARTS], need_r[DG_MAX_PARTS];
    for (u32 i = 0; i < g_nparts; i++) {
        u32 rows, cells;
        dg_text_metrics(g_parts[i].label, &rows, &cells);
        if (rows > g_head_rows) g_head_rows = rows;
        g_parts[i].w = (f32)(cells + 3) * g_cw;
        if (g_parts[i].w < g_cw * 7.0f) g_parts[i].w = g_cw * 7.0f;
        gap[i] = g_cw * 4.0f * g_gap;
        need_l[i] = need_r[i] = 0;
    }
    f32 head_h = dg_head_h();
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        if (ms->kind != SQ_MSG && ms->kind != SQ_NOTE_L && ms->kind != SQ_NOTE_R &&
            ms->kind != SQ_NOTE_OVER) continue;
        u32 rows, cells;
        dg_text_metrics(ms->label, &rows, &cells);
        (void)rows;
        bool note = (ms->kind != SQ_MSG);
        f32 need = (f32)(cells + 4 + (ms->number ? 4 : 0)) * g_cw;
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
            if (ms->kind == SQ_NOTE_OVER) {
                f32 want = need * 0.5f - half;
                if (want > need_l[a]) need_l[a] = want;
                if (want > need_r[a]) need_r[a] = want;
            } else if (need + g_cw * 3.0f > need_r[a]) {
                need_r[a] = need + g_cw * 3.0f;
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

    /* Rows. Activate/deactivate markers take no height; frame markers take
     * their label band. */
    f32 y = head_h + g_ch * 1.2f * g_gap;
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        u32 rows, cells;
        dg_text_metrics(ms->label, &rows, &cells);
        ms->y = y;
        ms->ly = y;
        switch (ms->kind) {
        case SQ_ACT: case SQ_DEACT: ms->h = 0; break;
        case SQ_FRAME_OPEN: case SQ_FRAME_DIV:
            ms->h = g_ch * 2.0f;
            break;
        case SQ_FRAME_CLOSE: ms->h = g_ch * 0.9f; break;
        case SQ_NOTE_L: case SQ_NOTE_R: case SQ_NOTE_OVER:
            ms->h = (f32)rows * g_ch * DG_ROW_H + g_ch * 1.4f;
            break;
        default:
            /* The label band sits above the arrow and grows with its rows. */
            ms->ly = y + (f32)rows * g_ch * DG_ROW_H + g_ch * 0.35f;
            ms->h = (ms->ly - y) + ((ms->from == ms->to) ? g_ch * 1.7f : g_ch * (0.55f + 0.45f * g_gap));
            break;
        }
        y += ms->h;
    }
    f32 total_h = y + g_ch * 1.2f;

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
            b->y1 = total_h - g_ch * 0.6f;
        }

    /* Frames: from their marker row to their closing row, wide enough for
     * every lifeline a message inside them touches, outer ones around inner
     * ones. */
    struct { i32 frame; i32 lo, hi; i32 inner; } st[DG_MAX_DEPTH * 4];
    u32 sp = 0;
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        if (ms->kind == SQ_FRAME_OPEN) {
            if (g_nframes >= DG_MAX_FRAMES || sp >= sizeof st / sizeof st[0]) continue;
            DFrame *f = &g_frames[g_nframes];
            memset(f, 0, sizeof *f);
            const char *nl = strchr(ms->label, '\n');
            u32 kl = nl ? (u32)(nl - ms->label) : (u32)strlen(ms->label);
            dg_strcpy(f->label, sizeof f->label, (const u8 *)ms->label, kl);
            if (nl) dg_strcpy(f->text, sizeof f->text, (const u8 *)nl + 1, (u32)strlen(nl + 1));
            f->y = ms->y + g_ch * 0.2f;
            f->depth = (i32)sp;
            st[sp].frame = (i32)g_nframes++;
            st[sp].lo = 1 << 20; st[sp].hi = -1; st[sp].inner = 0;
            sp++;
            continue;
        }
        if (ms->kind == SQ_FRAME_DIV) {
            if (sp && g_ndivs < DG_MAX_DIVS) {
                DDiv *d = &g_divs[g_ndivs++];
                d->frame = st[sp - 1].frame;
                memcpy(d->text, ms->label, sizeof d->text);
                d->y = ms->y + g_ch * 0.2f;
            }
            continue;
        }
        if (ms->kind == SQ_FRAME_CLOSE) {
            if (!sp) continue;
            sp--;
            DFrame *f = &g_frames[st[sp].frame];
            i32 lo = st[sp].lo, hi = st[sp].hi;
            if (hi < 0) { lo = 0; hi = (i32)g_nparts - 1; }
            f32 pad = g_cw * (1.2f + 0.8f * (f32)st[sp].inner);
            f32 l = g_parts[lo].cx - g_parts[lo].w * 0.5f - pad;
            f32 r = g_parts[hi].cx + g_parts[hi].w * 0.5f + pad;
            if (lo == hi) r += g_cw * 2.0f;
            f->x = l; f->w = r - l;
            f->h = ms->y + ms->h * 0.5f - f->y;
            if (sp) {
                if (lo < st[sp - 1].lo) st[sp - 1].lo = lo;
                if (hi > st[sp - 1].hi) st[sp - 1].hi = hi;
                if (st[sp].inner + 1 > st[sp - 1].inner) st[sp - 1].inner = st[sp].inner + 1;
            }
            continue;
        }
        i32 parts[2] = { -1, -1 };
        if (ms->kind == SQ_MSG) { parts[0] = ms->from; parts[1] = ms->to; }
        else if (ms->kind == SQ_NOTE_L || ms->kind == SQ_NOTE_R || ms->kind == SQ_NOTE_OVER) {
            parts[0] = ms->to; parts[1] = ms->span >= 0 ? ms->span : ms->to;
        }
        for (u32 k = 0; k < sp; k++)
            for (u32 j = 0; j < 2; j++) {
                if (parts[j] < 0 || (u32)parts[j] >= g_nparts) continue;
                if (parts[j] < st[k].lo) st[k].lo = parts[j];
                if (parts[j] > st[k].hi) st[k].hi = parts[j];
            }
    }
}

/* Note box for `ms`. */
static void dg_note_box(const DMsg *ms, f32 *bx, f32 *by, f32 *bw, f32 *bh) {
    u32 rows, cells;
    dg_text_metrics(ms->label, &rows, &cells);
    (void)rows;
    f32 w = (f32)(cells + 2) * g_cw;
    f32 cx = g_parts[ms->to].cx;
    f32 nx = cx - w * 0.5f;
    if (ms->kind == SQ_NOTE_L) nx = cx - w - g_cw * 0.8f;
    else if (ms->kind == SQ_NOTE_R) nx = cx + g_cw * 0.8f;
    else if (ms->span >= 0 && (u32)ms->span < g_nparts) {
        f32 ox = g_parts[ms->span].cx;
        f32 mid = (cx + ox) * 0.5f;
        f32 span = fabsf(ox - cx) + g_cw * 2.0f;
        if (span > w) w = span;
        nx = mid - w * 0.5f;
    }
    *bx = nx;
    *by = ms->y + g_ch * 0.3f;
    *bw = w;
    *bh = ms->h - g_ch * 0.6f;
}

static void em_seq(void) {
    f32 t = dg_stroke_w();
    f32 head_h = dg_head_h();
    f32 bottom = 0;
    for (u32 m = 0; m < g_nmsgs; m++) {
        f32 e = g_msgs[m].y + g_msgs[m].h;
        if (e > bottom) bottom = e;
    }
    bottom += g_ch * 1.0f;

    /* frames, behind everything */
    for (u32 f = 0; f < g_nframes; f++) {
        const DFrame *fr = &g_frames[f];
        if (fr->w <= 0 || fr->h <= 0) continue;
        bool shade = strcmp(fr->label, "rect") == 0;
        em_box(P_BACK, fr->x, fr->y, fr->w, fr->h, g_ch * 0.2f, t,
               shade ? DI_GROUP_FILL : DI_NONE, DI_GROUP_STROKE);
        if (shade) continue;
        f32 tw = (f32)(dg_cells(fr->label, (u32)strlen(fr->label)) + 2) * g_cw;
        f32 th = g_ch * 1.35f;
        em_box(P_NODE, fr->x, fr->y, tw, th, g_ch * 0.2f, 0, DI_GROUP_STROKE, DI_NONE);
        em_text(fr->x + g_cw, fr->y + (th - g_ch) * 0.5f, fr->label, (u32)strlen(fr->label), DI_SURFACE);
        if (fr->text[0]) {
            char br[DG_ELABEL_CAP + 4];
            snprintf(br, sizeof br, "[%s]", fr->text);
            em_text(fr->x + tw + g_cw, fr->y + (th - g_ch) * 0.5f, br, (u32)strlen(br), DI_GROUP_TEXT);
        }
    }
    for (u32 d = 0; d < g_ndivs; d++) {
        const DDiv *dv = &g_divs[d];
        const DFrame *fr = &g_frames[dv->frame];
        if (fr->w <= 0) continue;
        f32 lx[2] = { fr->x, fr->x + fr->w }, ly[2] = { dv->y, dv->y };
        em_dashed(P_BACK, lx, ly, 2, t, g_ch * 0.35f, g_ch * 0.25f, DI_GROUP_STROKE);
        if (dv->text[0]) {
            char br[DG_ELABEL_CAP + 4];
            snprintf(br, sizeof br, "[%s]", dv->text);
            f32 tw = (f32)dg_cells(br, (u32)strlen(br)) * g_cw;
            em_text(fr->x + (fr->w - tw) * 0.5f, dv->y + g_ch * 0.3f, br, (u32)strlen(br), DI_GROUP_TEXT);
        }
    }

    /* lifelines */
    for (u32 i = 0; i < g_nparts; i++) {
        f32 lx[2] = { g_parts[i].cx, g_parts[i].cx }, ly[2] = { head_h, bottom - g_ch * 0.4f };
        em_dashed(P_BACK, lx, ly, 2, t, g_ch * 0.3f, g_ch * 0.25f, DI_GROUP_STROKE);
    }
    /* activation bars */
    f32 bw = dg_bar_w();
    for (u32 b = 0; b < g_nbars; b++) {
        const DBar *ba = &g_bars[b];
        if (ba->part < 0 || (u32)ba->part >= g_nparts) continue;
        f32 bx = g_parts[ba->part].cx - bw * 0.5f + (f32)ba->depth * bw * 0.5f;
        f32 hh = ba->y1 - ba->y0;
        if (hh < g_ch * 0.5f) hh = g_ch * 0.5f;
        em_box(P_EDGE, bx, ba->y0, bw, hh, 2.0f, t, DI_ACTIVATION, DI_NODE_STROKE);
    }
    /* messages */
    i32 depth[DG_MAX_PARTS];
    for (u32 i = 0; i < g_nparts; i++) depth[i] = 0;
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        if (ms->kind == SQ_ACT && ms->to >= 0 && (u32)ms->to < g_nparts) { depth[ms->to]++; continue; }
        if (ms->kind == SQ_DEACT && ms->to >= 0 && (u32)ms->to < g_nparts) {
            if (depth[ms->to] > 0) depth[ms->to]--;
            continue;
        }
        if (ms->kind != SQ_MSG) continue;
        if (ms->from < 0 || (u32)ms->from >= g_nparts ||
            ms->to < 0 || (u32)ms->to >= g_nparts) continue;
        if (ms->act > 0) depth[ms->to]++;
        f32 ax = g_parts[ms->from].cx, bx2 = g_parts[ms->to].cx;
        f32 ly = ms->ly;
        if (ms->from == ms->to) {
            f32 off = depth[ms->from] > 0 ? bw * 0.5f : 0.0f;
            f32 rx = ax + g_cw * 3.0f + off;
            f32 y1 = ly + g_ch * 1.2f;
            f32 px[4] = { ax + off, rx, rx, ax + off }, py[4] = { ly, ly, y1, y1 };
            em_path(px, py, 4, ms->style, MK_NONE, ms->mk_to, DI_EDGE);
        } else {
            f32 dir = bx2 > ax ? 1.0f : -1.0f;
            f32 so = depth[ms->from] > 0 ? bw * 0.5f : 0.0f;
            f32 eo = depth[ms->to] > 0 ? bw * 0.5f + (f32)(depth[ms->to] - 1) * bw * 0.5f : 0.0f;
            f32 px[2] = { ax + dir * so, bx2 - dir * eo }, py[2] = { ly, ly };
            em_path(px, py, 2, ms->style, ms->mk_from, ms->mk_to, DI_EDGE);
            if (ms->number) {
                f32 r = g_ch * 0.55f;
                f32 cx = px[0], cy = ly;
                em_box(P_TOP, cx - r, cy - r, r * 2, r * 2, r, 0, DI_ACCENT, DI_NONE);
                char num[8];
                snprintf(num, sizeof num, "%u", (unsigned)ms->number);
                f32 tw = (f32)strlen(num) * g_cw;
                em_text(cx - tw * 0.5f, cy - g_ch * 0.5f, num, (u32)strlen(num), DI_SURFACE);
            }
        }
        if (ms->act < 0 && depth[ms->from] > 0) depth[ms->from]--;
    }
    /* heads and notes */
    for (u32 i = 0; i < g_nparts; i++) {
        f32 w = g_parts[i].w;
        f32 x0 = g_parts[i].cx - w * 0.5f;
        f32 hh = head_h - g_ch * 0.25f;
        em_box(P_NODE, x0, 0, w, hh, g_parts[i].actor ? hh * 0.5f : g_ch * 0.18f, t,
               DI_NODE_FILL, DI_NODE_STROKE);
        em_text_box(g_parts[i].label, x0, 0, w, hh, DI_TEXT);
    }
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        if (ms->kind != SQ_NOTE_L && ms->kind != SQ_NOTE_R && ms->kind != SQ_NOTE_OVER) continue;
        if (ms->to < 0 || (u32)ms->to >= g_nparts) continue;
        f32 bx, by, bw2, bh;
        dg_note_box(ms, &bx, &by, &bw2, &bh);
        em_box(P_NODE, bx, by, bw2, bh, g_ch * 0.15f, t, DI_NOTE_FILL, DI_NOTE_STROKE);
        em_text_box(ms->label, bx, by, bw2, bh, DI_TEXT);
    }
    /* message labels */
    for (u32 m = 0; m < g_nmsgs; m++) {
        DMsg *ms = &g_msgs[m];
        if (ms->kind != SQ_MSG || !ms->label[0]) continue;
        if (ms->from < 0 || (u32)ms->from >= g_nparts ||
            ms->to < 0 || (u32)ms->to >= g_nparts) continue;
        u32 rows, cells;
        dg_text_metrics(ms->label, &rows, &cells);
        f32 tw = (f32)cells * g_cw;
        f32 ax = g_parts[ms->from].cx, bx = g_parts[ms->to].cx;
        f32 bx0 = (ms->from == ms->to) ? ax + g_cw * 3.6f : fminf(ax, bx);
        f32 bw2 = (ms->from == ms->to) ? tw : fabsf(bx - ax);
        em_text_box(ms->label, bx0, ms->y, bw2, ms->ly - ms->y, DI_TEXT);
    }
}

/* =========================================================================
 * Pie
 * ========================================================================= */
static void em_pie(void) {
    f64 total = 0;
    for (u32 i = 0; i < g_nslices; i++) total += g_slices[i].value;
    if (!(total > 0)) return;
    f32 top = 0;
    if (g_title[0]) {
        u32 rows, cells;
        dg_text_metrics(g_title, &rows, &cells);
        em_text_box(g_title, 0, 0, (f32)cells * g_cw + g_cw * 2, (f32)rows * g_ch * DG_ROW_H, DI_TEXT);
        top = (f32)rows * g_ch * DG_ROW_H + g_ch * 0.8f;
    }
    f32 radius = g_ch * 5.0f;
    f32 cx = radius + g_cw, cy = top + radius;
    f32 a = -1.5707963f;
    for (u32 i = 0; i < g_nslices; i++) {
        f32 sweep = (f32)(g_slices[i].value / total) * 6.2831853f;
        u32 steps = (u32)ceilf(sweep / 0.12f);
        if (steps < 1) steps = 1;
        /* Neighbouring wedges of one slice overlap, so their antialiased
         * edges never meet half-covered: that would draw a seam. */
        const f32 eps = 0.035f;
        for (u32 k = 0; k < steps; k++) {
            f32 a0 = a + sweep * (f32)k / (f32)steps, a1 = a + sweep * (f32)(k + 1) / (f32)steps;
            if (k > 0) a0 -= eps;
            if (k + 1 < steps) a1 += eps;
            f32 x0 = cx + cosf(a0) * radius, y0 = cy + sinf(a0) * radius;
            f32 x1 = cx + cosf(a1) * radius, y1 = cy + sinf(a1) * radius;
            f32 hb = hypotf(x1 - x0, y1 - y0) * 0.5f;
            em_wedge(P_EDGE, cx, cy, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, hb, (u8)(DI_SLICE0 + (i % 8)));
        }
        if (g_nslices > 1)
            em_seg(P_NODE, cx, cy, cx + cosf(a) * radius, cy + sinf(a) * radius,
                   fmaxf(1.0f, g_ch * 0.09f), DI_SURFACE);
        a += sweep;
    }
    /* legend */
    f32 lx = cx + radius + g_cw * 3.0f;
    f32 ly = cy - (f32)g_nslices * g_ch * 1.5f * 0.5f;
    for (u32 i = 0; i < g_nslices; i++) {
        f32 y = ly + (f32)i * g_ch * 1.5f;
        em_box(P_NODE, lx, y + g_ch * 0.15f, g_ch * 0.75f, g_ch * 0.75f, g_ch * 0.15f, 0,
               (u8)(DI_SLICE0 + (i % 8)), DI_NONE);
        char row[DG_ELABEL_CAP + 48];
        f64 pct = g_slices[i].value / total * 100.0;
        if (g_show_data) snprintf(row, sizeof row, "%s  %g (%.1f%%)", g_slices[i].label, g_slices[i].value, pct);
        else snprintf(row, sizeof row, "%s  %.1f%%", g_slices[i].label, pct);
        em_text(lx + g_ch * 1.2f, y + (g_ch * 1.05f - g_ch) * 0.5f, row, (u32)strlen(row), DI_TEXT);
    }
}

/* =========================================================================
 * Building and drawing
 * ========================================================================= */

/* Bounds of everything emitted, ink width included. */
static void em_bounds(f32 *x0, f32 *y0, f32 *x1, f32 *y1) {
    f32 a = 1e30f, b = 1e30f, c = -1e30f, d = -1e30f;
#define GROW(px, py) do { f32 _x = (px), _y = (py); \
        if (_x < a) a = _x; if (_y < b) b = _y; if (_x > c) c = _x; if (_y > d) d = _y; } while (0)
    for (u32 i = 0; i < e_nbox; i++) {
        f32 hb = e_box[i].stroke != DI_NONE ? e_box[i].border * 0.5f : 0.0f;
        GROW(e_box[i].x - hb, e_box[i].y - hb);
        GROW(e_box[i].x + e_box[i].w + hb, e_box[i].y + e_box[i].h + hb);
    }
    for (u32 i = 0; i < e_nstr; i++) {
        const DPStroke *s = &e_str[i];
        f32 h = s->width * 0.5f;
        GROW(s->x0 + s->nx0 * h, s->y0 + s->ny0 * h); GROW(s->x0 - s->nx0 * h, s->y0 - s->ny0 * h);
        GROW(s->x1 + s->nx1 * h, s->y1 + s->ny1 * h); GROW(s->x1 - s->nx1 * h, s->y1 - s->ny1 * h);
    }
    for (u32 i = 0; i < e_ngly; i++) {
        GROW(e_gly[i].x, e_gly[i].y);
        GROW(e_gly[i].x + g_cw * (e_gly[i].wide ? 2.0f : 1.0f), e_gly[i].y + g_ch);
    }
#undef GROW
    *x0 = a; *y0 = b; *x1 = c; *y1 = d;
}

static MdDiagram *dg_finish(MdDiagramKind kind) {
    if (g_trunc || (!e_nbox && !e_nstr && !e_ngly)) return NULL;
    f32 x0, y0, x1, y1;
    em_bounds(&x0, &y0, &x1, &y1);
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return NULL;
    f32 margin = g_cw * 1.0f;
    f32 dx = margin - x0, dy = margin - y0;
    f32 w = (x1 - x0) + margin * 2.0f, h = (y1 - y0) + margin * 2.0f;
    if (!(w > 0.0f) || !(h > 0.0f) || w > 1.0e7f || h > 1.0e7f) return NULL;

    usize bytes = sizeof(MdDiagram) + sizeof(DPBox) * e_nbox +
                  sizeof(DPStroke) * e_nstr + sizeof(DPGlyph) * e_ngly + 64;
    MdDiagram *d = (MdDiagram *)calloc(1, bytes);
    if (!d) return NULL;
    u8 *p = (u8 *)(d + 1);
    p = (u8 *)(((uintptr_t)p + 15u) & ~(uintptr_t)15u);
    d->box = (DPBox *)p;    p += sizeof(DPBox) * e_nbox;
    d->str = (DPStroke *)p; p += sizeof(DPStroke) * e_nstr;
    d->gly = (DPGlyph *)p;
    d->kind = kind;
    d->lang = g_lang;
    d->w = w; d->h = h; d->cw = g_cw; d->ch = g_ch;
    d->bytes = bytes;
    /* counting sort by pass, order kept within a pass */
    u32 nb[DG_PASSES] = {0}, ns[DG_PASSES] = {0};
    for (u32 i = 0; i < e_nbox; i++) nb[e_box[i].pass < DG_PASSES ? e_box[i].pass : DG_PASSES - 1]++;
    for (u32 i = 0; i < e_nstr; i++) ns[e_str[i].pass < DG_PASSES ? e_str[i].pass : DG_PASSES - 1]++;
    d->box_at[0] = 0; d->str_at[0] = 0;
    for (u32 k = 0; k < DG_PASSES; k++) {
        d->box_at[k + 1] = d->box_at[k] + nb[k];
        d->str_at[k + 1] = d->str_at[k] + ns[k];
    }
    u32 fb[DG_PASSES], fs[DG_PASSES];
    for (u32 k = 0; k < DG_PASSES; k++) { fb[k] = d->box_at[k]; fs[k] = d->str_at[k]; }
    for (u32 i = 0; i < e_nbox; i++) {
        DPBox b = e_box[i];
        b.x += dx; b.y += dy;
        u32 k = b.pass < DG_PASSES ? b.pass : DG_PASSES - 1;
        d->box[fb[k]++] = b;
    }
    for (u32 i = 0; i < e_nstr; i++) {
        DPStroke s = e_str[i];
        s.x0 += dx; s.y0 += dy; s.x1 += dx; s.y1 += dy;
        u32 k = s.pass < DG_PASSES ? s.pass : DG_PASSES - 1;
        d->str[fs[k]++] = s;
    }
    for (u32 i = 0; i < e_ngly; i++) {
        DPGlyph g = e_gly[i];
        g.x += dx; g.y += dy;
        d->gly[i] = g;
    }
    d->nbox = e_nbox; d->nstroke = e_nstr; d->nglyph = e_ngly;
    return d;
}

static bool dg_metrics_ok(f32 cw, f32 ch) {
    return isfinite(cw) && isfinite(ch) && cw > 0.0f && ch > 0.0f &&
           cw <= 16384.0f && ch <= 16384.0f;
}

MdDiagram *md_diagram_create(const u8 *src, u32 len, f32 cw, f32 ch) {
    return md_diagram_create_ex(src, len, cw, ch, MD_DIAGRAM_READING);
}

MdDiagram *md_diagram_create_ex(const u8 *src, u32 len, f32 cw, f32 ch, MdDiagramSpacing spacing) {
    if (!src || !len || !dg_metrics_ok(cw, ch)) return NULL;
    g_cw = cw; g_ch = ch;
    g_gap = spacing == MD_DIAGRAM_COMPACT ? 0.55f : 1.0f;
    MdDiagramKind kind = dg_build(src, len);
    if (kind == MD_DIAGRAM_NONE) return NULL;
    em_reset();
    switch (kind) {
    case MD_DIAGRAM_SEQUENCE: dg_layout_seq(); em_seq(); break;
    case MD_DIAGRAM_PIE:      em_pie(); break;
    case MD_DIAGRAM_GANTT: case MD_DIAGRAM_MINDMAP: case MD_DIAGRAM_TIMELINE:
    case MD_DIAGRAM_GITGRAPH: case MD_DIAGRAM_XYCHART: case MD_DIAGRAM_QUADRANT:
    case MD_DIAGRAM_JOURNEY:
        dgm_emit(kind);
        break;
    default:
        if (!dg_layout_graph()) return NULL;
        em_graph();
        break;
    }
    return dg_finish(kind);
}

void md_diagram_destroy(MdDiagram *d) { free(d); }

MdDiagramKind md_diagram_kind_of(const MdDiagram *d) { return d ? d->kind : MD_DIAGRAM_NONE; }

MdDiagramLang md_diagram_lang_of(const MdDiagram *d) { return d ? d->lang : MD_DIAGRAM_LANG_MERMAID; }

MdDiagramLang md_diagram_lang_detect(const u8 *src, u32 len) {
    return src && len ? dgl_lang(src, len) : MD_DIAGRAM_LANG_MERMAID;
}

const char *md_diagram_lang_name(MdDiagramLang lang) {
    switch (lang) {
    case MD_DIAGRAM_LANG_GRAPHVIZ: return "Graphviz";
    case MD_DIAGRAM_LANG_PLANTUML: return "PlantUML";
    default:                       return "Mermaid";
    }
}

void md_diagram_extent(const MdDiagram *d, f32 *w, f32 *h) {
    if (w) *w = d ? d->w : 0.0f;
    if (h) *h = d ? d->h : 0.0f;
}

usize md_diagram_bytes(const MdDiagram *d) { return d ? d->bytes : 0; }

void md_diagram_counts(const MdDiagram *d, u32 *boxes, u32 *strokes, u32 *glyphs) {
    if (boxes)   *boxes = d ? d->nbox : 0;
    if (strokes) *strokes = d ? d->nstroke : 0;
    if (glyphs)  *glyphs = d ? d->nglyph : 0;
}

const char *md_diagram_kind_name(MdDiagramKind kind) {
    switch (kind) {
    case MD_DIAGRAM_FLOWCHART: return "flowchart";
    case MD_DIAGRAM_SEQUENCE:  return "sequence";
    case MD_DIAGRAM_STATE:     return "state";
    case MD_DIAGRAM_CLASS:     return "class";
    case MD_DIAGRAM_ER:        return "entity relationship";
    case MD_DIAGRAM_PIE:       return "pie";
    case MD_DIAGRAM_GANTT:     return "gantt";
    case MD_DIAGRAM_MINDMAP:   return "mindmap";
    case MD_DIAGRAM_TIMELINE:  return "timeline";
    case MD_DIAGRAM_GITGRAPH:  return "git graph";
    case MD_DIAGRAM_XYCHART:   return "chart";
    case MD_DIAGRAM_QUADRANT:  return "quadrant chart";
    case MD_DIAGRAM_JOURNEY:   return "user journey";
    default:                   return "diagram";
    }
}

bool md_diagram_header_line(const u8 *line, u32 len) {
    while (len && dg_is_space(line[0])) { line++; len--; }
    while (len && (dg_is_space(line[len - 1]) || line[len - 1] == '\r' || line[len - 1] == ';')) len--;
    if (!len) return false;
    /* `@startuml`, and DOT's `digraph G {` / `strict graph {` (a brace on the row) */
    if (dg_kw(line, len, "@startuml")) return true;
    if (line[len - 1] == '{' && dgl_lang(line, len) == MD_DIAGRAM_LANG_GRAPHVIZ) return true;
    if (dg_kw(line, len, "graph") || dg_kw(line, len, "flowchart")) {
        u32 p = dg_kw(line, len, "graph") ? 5u : 9u;
        while (p < len && dg_is_space(line[p])) p++;
        if (p == len) return true;
        if (len - p != 2) return false;
        static const char *dirs[] = { "TD", "TB", "BT", "LR", "RL", NULL };
        for (u32 k = 0; dirs[k]; k++) if (memcmp(line + p, dirs[k], 2) == 0) return true;
        return false;
    }
    if (len == 15 && memcmp(line, "sequenceDiagram", 15) == 0) return true;
    if ((len == 12 && memcmp(line, "stateDiagram", 12) == 0) ||
        (len == 15 && memcmp(line, "stateDiagram-v2", 15) == 0)) return true;
    if ((len == 12 && memcmp(line, "classDiagram", 12) == 0) ||
        (len == 15 && memcmp(line, "classDiagram-v2", 15) == 0)) return true;
    if (len == 9 && memcmp(line, "erDiagram", 9) == 0) return true;
    if (dg_kw(line, len, "pie")) {
        u32 p = 3;
        while (p < len && dg_is_space(line[p])) p++;
        return p == len || dg_kw(line + p, len - p, "title") || dg_kw(line + p, len - p, "showData");
    }
    static const char *alone[] = { "gantt", "mindmap", "timeline", "quadrantChart", "journey",
                                   "gitGraph", "gitGraph:", "xychart-beta", "xychart", NULL };
    for (u32 k = 0; alone[k]; k++)
        if (len == strlen(alone[k]) && memcmp(line, alone[k], len) == 0) return true;
    /* `gitGraph LR:` / `TB:` / `BT:`, `xychart-beta horizontal` */
    if (dg_kw(line, len, "gitGraph")) {
        u32 p = 8;
        while (p < len && dg_is_space(line[p])) p++;
        return len - p == 3 && line[len - 1] == ':' &&
               (!memcmp(line + p, "LR", 2) || !memcmp(line + p, "TB", 2) || !memcmp(line + p, "BT", 2));
    }
    if (dg_kw(line, len, "xychart-beta") || dg_kw(line, len, "xychart")) {
        u32 p = dg_kw(line, len, "xychart-beta") ? 12u : 7u;
        while (p < len && dg_is_space(line[p])) p++;
        return (len - p == 10 && !memcmp(line + p, "horizontal", 10)) ||
               (len - p == 8 && !memcmp(line + p, "vertical", 8));
    }
    return false;
}

static Color dg_mix(Color a, Color b, f32 t) {
    return (Color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                    a.b + (b.b - a.b) * t, 1.0f };
}

void md_diagram_inks(const Theme *theme, Color surface, Color accent, MdDiagramInks *out) {
    if (!out) return;
    Color fg = theme ? theme->fg : (Color){ 0.86f, 0.86f, 0.90f, 1.0f };
    Color yellow = theme ? theme->ansi[3] : (Color){ 0.85f, 0.72f, 0.35f, 1.0f };
    surface.a = 1.0f;
    accent.a = 1.0f;
    out->surface      = surface;
    out->text         = fg;
    out->text_dim     = dg_mix(surface, fg, 0.70f);
    out->node_fill    = dg_mix(dg_mix(surface, fg, 0.06f), accent, 0.10f);
    out->node_stroke  = dg_mix(surface, accent, 0.62f);
    out->edge         = dg_mix(surface, fg, 0.55f);
    out->chip         = dg_mix(surface, fg, 0.05f);
    out->group_fill   = dg_mix(surface, fg, 0.035f);
    out->group_stroke = dg_mix(surface, fg, 0.22f);
    out->group_text   = dg_mix(surface, fg, 0.62f);
    out->note_fill    = dg_mix(surface, yellow, 0.16f);
    out->note_stroke  = dg_mix(surface, yellow, 0.55f);
    out->accent       = accent;
    out->activation   = dg_mix(surface, accent, 0.28f);
    out->solid        = dg_mix(surface, fg, 0.85f);
    static const u8 slice_ansi[8] = { 4, 5, 2, 3, 6, 1, 12, 13 };
    for (u32 i = 0; i < 8; i++) {
        Color c = theme ? theme->ansi[slice_ansi[i]] : (Color){ 0.3f + 0.08f * (f32)i, 0.5f, 0.8f - 0.07f * (f32)i, 1.0f };
        c.a = 1.0f;
        out->slice[i] = dg_mix(c, surface, 0.12f);
        out->soft[i] = dg_mix(surface, c, 0.30f);
    }
}

static Color dg_ink(const MdDiagramInks *k, u8 ink, f32 alpha) {
    Color c;
    switch (ink) {
    case DI_SURFACE:      c = k->surface; break;
    case DI_TEXT:         c = k->text; break;
    case DI_TEXT_DIM:     c = k->text_dim; break;
    case DI_NODE_FILL:    c = k->node_fill; break;
    case DI_NODE_STROKE:  c = k->node_stroke; break;
    case DI_EDGE:         c = k->edge; break;
    case DI_CHIP:         c = k->chip; break;
    case DI_GROUP_FILL:   c = k->group_fill; break;
    case DI_GROUP_STROKE: c = k->group_stroke; break;
    case DI_GROUP_TEXT:   c = k->group_text; break;
    case DI_NOTE_FILL:    c = k->note_fill; break;
    case DI_NOTE_STROKE:  c = k->note_stroke; break;
    case DI_ACCENT:       c = k->accent; break;
    case DI_ACTIVATION:   c = k->activation; break;
    case DI_SOLID:        c = k->solid; break;
    default:
        if (ink >= DI_SLICE0 && ink < DI_SLICE0 + 8) { c = k->slice[ink - DI_SLICE0]; break; }
        if (ink >= DI_SOFT0 && ink < DI_SOFT0 + 8) { c = k->soft[ink - DI_SOFT0]; break; }
        c = (Color){ 0, 0, 0, 0 };
        break;
    }
    c.a *= alpha;
    return c;
}

/* What the clip keeps: an item wholly outside the scissor is not sent. */
static bool dg_clipped(const Renderer *r, f32 x0, f32 y0, f32 x1, f32 y1) {
    if (!r->scissor_active) return false;
    return x1 < r->scissor_x || y1 < r->scissor_y ||
           x0 > r->scissor_x + r->scissor_w || y0 > r->scissor_y + r->scissor_h;
}

/* A stroke that fills a shape (an arrow head, a wedge of a rhombus) rather
 * than outlining one: it keeps its own width and is never joined to another. */
static bool dg_stroke_fills(const MdDiagram *d, const DPStroke *st) {
    return (st->nx0 == 0.0f && st->ny0 == 0.0f) || st->width > d->ch * 0.3f;
}

/* Consecutive pieces of one outline: the second starts where the first ends,
 * in the same ink and width. */
static bool dg_stroke_continues(const MdDiagram *d, const DPStroke *a, const DPStroke *b) {
    return b->x0 == a->x1 && b->y0 == a->y1 && b->ink == a->ink && b->width == a->width &&
           !dg_stroke_fills(d, b);
}

/* A curve's pieces shorter than this on screen are drawn as one chord: past
 * it a chord strays less than a pixel from the curve it stands for, and a
 * diagram drawn small would otherwise send every piece of every edge. */
#define DG_DRAW_MIN_PX 1.5f

void md_diagram_draw(Renderer *r, const MdDiagram *d, const MdDiagramInks *inks,
                     f32 x, f32 y, f32 s, f32 alpha) {
    if (!r || !d || !inks || !isfinite(x) || !isfinite(y) || !isfinite(s) || !(s > 0.0f)) return;
    if (!(alpha > 0.0f)) return;
    if (alpha > 1.0f) alpha = 1.0f;
    for (u32 pass = 0; pass < DG_PASSES; pass++) {
        u32 b0 = d->box_at[pass], b1 = d->box_at[pass + 1];
        bool boxes = false;
        for (u32 i = b0; i < b1; i++) {
            const DPBox *b = &d->box[i];
            f32 bx = x + b->x * s, by = y + b->y * s, bw = b->w * s, bh = b->h * s;
            if (dg_clipped(r, bx - 2.0f, by - 2.0f, bx + bw + 2.0f, by + bh + 2.0f)) continue;
            boxes = true;
            Color fill = b->fill == DI_NONE ? (Color){ 0, 0, 0, 0 } : dg_ink(inks, b->fill, alpha);
            f32 rad = b->radius * s;
            if (b->stroke != DI_NONE && b->border > 0.0f) {
                Color edge = dg_ink(inks, b->stroke, alpha);
                renderer_draw_rrect_bordered(r, bx, by, bw, bh, fill, edge, fmaxf(1.0f, b->border * s),
                                             rad, rad, rad, rad, 0, 0, 0, 0);
            } else {
                renderer_draw_rrect(r, bx, by, bw, bh, fill, rad, rad, rad, rad, 0, 0, 0, 0);
            }
        }
        if (boxes) renderer_flush_rrects(r);
        u32 s0 = d->str_at[pass], s1 = d->str_at[pass + 1];
        bool lines = false;
        /* the chord's square, in the diagram's own units, that spans the minimum on screen */
        f32 min_chord = DG_DRAW_MIN_PX / s;
        f32 min_chord2 = min_chord * min_chord;
        for (u32 i = s0; i < s1; ) {
            const DPStroke *st = &d->str[i];
            bool fills = dg_stroke_fills(d, st);
            /* the pieces of one outline whose chord still spans under a pixel and a half */
            u32 j = i;
            if (!fills) {
                f32 cx = st->x1 - st->x0, cy = st->y1 - st->y0;
                while (cx * cx + cy * cy < min_chord2 && j + 1 < s1 &&
                       dg_stroke_continues(d, &d->str[j], &d->str[j + 1])) {
                    j++;
                    cx = d->str[j].x1 - st->x0;
                    cy = d->str[j].y1 - st->y0;
                }
            }
            const DPStroke *en = &d->str[j];
            i = j + 1;
            /* An outline never thins below a pixel; a fill keeps its size. */
            f32 w = st->width * s;
            if (!fills && w < 1.0f) w = 1.0f;
            f32 ax = x + st->x0 * s, ay = y + st->y0 * s, bx = x + en->x1 * s, by = y + en->y1 * s;
            f32 pad = w * 2.0f + 1.0f;
            if (dg_clipped(r, fminf(ax, bx) - pad, fminf(ay, by) - pad, fmaxf(ax, bx) + pad, fmaxf(ay, by) + pad))
                continue;
            lines = true;
            renderer_draw_stroke_segment(r, ax, ay, bx, by, st->nx0, st->ny0, en->nx1, en->ny1,
                                         w, dg_ink(inks, st->ink, alpha));
        }
        if (lines) renderer_flush_lines(r);
    }
    f32 gcw = d->cw * s, gch = d->ch * s;
    if (gcw < DG_MIN_TEXT || !d->nglyph) return;
    renderer_set_ui_scale(r, gcw, gch);
    for (u32 i = 0; i < d->nglyph; i++) {
        const DPGlyph *g = &d->gly[i];
        f32 gx = x + g->x * s, gy = y + g->y * s;
        if (dg_clipped(r, gx, gy, gx + gcw * (g->wide ? 2.0f : 1.0f), gy + gch)) continue;
        renderer_push_glyph(r, gx, gy, g->cp, dg_ink(inks, g->ink, alpha));
    }
    renderer_flush_glyphs(r);
}

/* ---- the markdown viewer's cache ---- */

typedef struct {
    u64 hash;
    u32 len;
    f32 cw, ch;
    MdDiagram *d;
    MdDiagramKind kind;      /* NONE is remembered too */
    u64 used;
} DgCacheEnt;

#define DG_CACHE 8
static DgCacheEnt g_cache[DG_CACHE];
static u64 g_cache_clock;

static u64 dg_hash(const u8 *s, u32 n) {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; }
    return h;
}

static MdDiagram *dg_cached(const u8 *src, u32 len, f32 cw, f32 ch) {
    u64 h = dg_hash(src, len);
    for (u32 i = 0; i < DG_CACHE; i++) {
        DgCacheEnt *e = &g_cache[i];
        if (e->used && e->hash == h && e->len == len && e->cw == cw && e->ch == ch) {
            e->used = ++g_cache_clock;
            return e->d;
        }
    }
    u32 victim = 0;
    for (u32 i = 1; i < DG_CACHE; i++) if (g_cache[i].used < g_cache[victim].used) victim = i;
    DgCacheEnt *e = &g_cache[victim];
    md_diagram_destroy(e->d);
    e->d = md_diagram_create(src, len, cw, ch);
    e->kind = md_diagram_kind_of(e->d);
    e->hash = h; e->len = len; e->cw = cw; e->ch = ch;
    e->used = ++g_cache_clock;
    return e->d;
}

void md_diagram_cache_clear(void) {
    for (u32 i = 0; i < DG_CACHE; i++) {
        md_diagram_destroy(g_cache[i].d);
        memset(&g_cache[i], 0, sizeof g_cache[i]);
    }
}

/* Scale that fits `d` into `avail_w`, or 0 when it would be below the floor. */
static f32 dg_fit_scale(const MdDiagram *d, f32 avail_w) {
    if (!(avail_w > 0.0f) || !isfinite(avail_w) || d->w <= avail_w) return 1.0f;
    f32 s = avail_w / d->w;
    return s < DG_MIN_SCALE ? 0.0f : s;
}

void md_diagram_measure(const u8 *src, u32 len, f32 avail_w, f32 cw, f32 ch,
                        f32 *out_w, f32 *out_h) {
    f32 w = 0, h = 0;
    if (src && len && dg_metrics_ok(cw, ch)) {
        MdDiagram *d = dg_cached(src, len, cw, ch);
        if (d) {
            f32 s = dg_fit_scale(d, avail_w);
            if (s > 0.0f) {
                w = d->w * s; h = d->h * s;
                if (avail_w > 0.0f && w > avail_w) w = avail_w;
            }
        }
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

void md_diagram_render(Renderer *r, const Theme *theme, const u8 *src, u32 len,
                       f32 x, f32 y, f32 avail_w, f32 cw, f32 ch) {
    if (!r || !src || !len || !isfinite(x) || !isfinite(y) ||
        !isfinite(avail_w) || !dg_metrics_ok(cw, ch))
        return;
    MdDiagram *d = dg_cached(src, len, cw, ch);
    if (!d) return;
    f32 s = dg_fit_scale(d, avail_w);
    if (!(s > 0.0f)) return;
    Color surface = theme ? theme->bg : (Color){ 0.12f, 0.12f, 0.14f, 1.0f };
    Color accent  = theme ? theme->ansi[12] : (Color){ 0.45f, 0.62f, 1.0f, 1.0f };
    MdDiagramInks inks;
    md_diagram_inks(theme, surface, accent, &inks);
    md_diagram_draw(r, d, &inks, x, y, s, 1.0f);
    /* The glyph cell size is sticky renderer state: put the caller's back, or
     * every block after this one in the document keeps rendering its text at
     * the diagram's scale. */
    renderer_set_ui_scale(r, cw, ch);
}

MdDiagramKind md_diagram_probe(const u8 *src, u32 len) {
    if (!src || !len) return MD_DIAGRAM_NONE;
    u64 h = dg_hash(src, len);
    for (u32 i = 0; i < DG_CACHE; i++)
        if (g_cache[i].used && g_cache[i].hash == h && g_cache[i].len == len)
            return g_cache[i].kind;
    return dg_build(src, len);
}
