/* Liu — minimal native Mermaid flowchart engine. See md_diagram.h. */
#include "ui/markdown/md_diagram.h"
#include "core/utf8.h"

#include <string.h>
#include <math.h>

#define DG_MAX_NODES 128
#define DG_MAX_EDGES 256

typedef enum { SH_RECT, SH_ROUND, SH_CIRCLE, SH_DIAMOND } NShape;
typedef enum { ES_SOLID, ES_DOTTED, ES_THICK } EStyle;

typedef struct {
    char id[48];
    char label[96];
    u8   shape;
    i32  rank;
    f32  x, y, w, h;       /* px, top-left + size after layout */
} DNode;

typedef struct {
    i32  from, to;
    char label[64];
    u8   style;
    bool arrow;
} DEdge;

/* Parsed model — single-threaded, reset per public call. */
static DNode g_nodes[DG_MAX_NODES];
static u32   g_nnodes;
static DEdge g_edges[DG_MAX_EDGES];
static u32   g_nedges;
static u8    g_dir;        /* 0 TD/TB, 1 LR, 2 BT, 3 RL */
static f32   g_w, g_h;     /* total diagram size */
static f32   g_cw, g_ch;

/* ---- small helpers ---- */
static bool dg_is_idchar(u8 c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}
static void dg_strcpy(char *dst, usize cap, const u8 *s, u32 n) {
    if (n >= cap) n = (u32)cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

static i32 dg_intern(const u8 *id, u32 n) {
    char key[48];
    dg_strcpy(key, sizeof key, id, n);
    for (u32 i = 0; i < g_nnodes; i++)
        if (strcmp(g_nodes[i].id, key) == 0) return (i32)i;
    if (g_nnodes >= DG_MAX_NODES) return -1;
    DNode *nd = &g_nodes[g_nnodes];
    memset(nd, 0, sizeof *nd);
    memcpy(nd->id, key, strlen(key) + 1);
    memcpy(nd->label, key, strlen(key) + 1);   /* default label = id */
    nd->shape = SH_RECT;
    return (i32)g_nnodes++;
}

/* Parse a node ref at *pi: id + optional shape bracket. Returns node index. */
static i32 dg_parse_noderef(const u8 *s, u32 len, u32 *pi) {
    u32 i = *pi;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    u32 ids = i;
    while (i < len && dg_is_idchar(s[i])) i++;
    if (i == ids) { *pi = i; return -1; }
    i32 idx = dg_intern(s + ids, i - ids);

    /* optional shape: [..] (..) ((..)) {..} */
    u8 shape = SH_RECT; u32 ls = 0, le = 0;
    if (i < len && (s[i] == '[' || s[i] == '(' || s[i] == '{')) {
        u8 open = s[i];
        if (open == '(' && i + 1 < len && s[i+1] == '(') { shape = SH_CIRCLE; i += 2; }
        else if (open == '(') { shape = SH_ROUND; i += 1; }
        else if (open == '{') { shape = SH_DIAMOND; i += 1; }
        else { shape = SH_RECT; i += 1; if (i < len && s[i] == '[') i++; } /* [[..]] */
        ls = i;
        /* read until the matching close run */
        while (i < len) {
            if (shape == SH_CIRCLE && s[i] == ')' && i + 1 < len && s[i+1] == ')') break;
            if (shape == SH_ROUND && s[i] == ')') break;
            if (shape == SH_DIAMOND && s[i] == '}') break;
            if (shape == SH_RECT && s[i] == ']') break;
            i++;
        }
        le = i;
        /* advance past close */
        if (shape == SH_CIRCLE) { if (i < len) i += 2; }
        else if (i < len) { i += 1; if (s[i-1] == ']' && i < len && s[i] == ']') i++; }
        if (idx >= 0 && le > ls) {
            /* strip surrounding quotes */
            const u8 *lp = s + ls; u32 ll = le - ls;
            if (ll >= 2 && lp[0] == '"' && lp[ll-1] == '"') { lp++; ll -= 2; }
            dg_strcpy(g_nodes[idx].label, sizeof g_nodes[idx].label, lp, ll);
            g_nodes[idx].shape = shape;
        } else if (idx >= 0) {
            g_nodes[idx].shape = shape;
        }
    }
    *pi = i;
    return idx;
}

/* Match an edge operator at *pi. Returns true + fills style/arrow, advances. */
static bool dg_parse_edge_op(const u8 *s, u32 len, u32 *pi, u8 *style, bool *arrow) {
    u32 i = *pi;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= len) { *pi = i; return false; }
    /* longest first */
    struct { const char *op; u8 st; bool ar; } ops[] = {
        {"-.->", ES_DOTTED, true}, {"-.-", ES_DOTTED, false},
        {"==>", ES_THICK, true},   {"===", ES_THICK, false},
        {"-->", ES_SOLID, true},   {"---", ES_SOLID, false},
        {NULL,0,0}
    };
    for (u32 k = 0; ops[k].op; k++) {
        u32 ol = (u32)strlen(ops[k].op);
        if (i + ol <= len && memcmp(s + i, ops[k].op, ol) == 0) {
            *style = ops[k].st; *arrow = ops[k].ar;
            i += ol;
            *pi = i;
            return true;
        }
    }
    *pi = i;
    return false;
}

static void dg_reset(void) {
    g_nnodes = 0; g_nedges = 0; g_dir = 0; g_w = g_h = 0;
}

/* Parse the flowchart source into nodes/edges. */
static void dg_parse(const u8 *src, u32 len, f32 cw, f32 ch) {
    dg_reset();
    g_cw = cw; g_ch = ch;
    u32 i = 0;
    bool first = true;
    while (i < len) {
        u32 ls = i;
        while (i < len && src[i] != '\n') i++;
        u32 le = i; if (i < len) i++;            /* consume newline */
        /* trim */
        while (ls < le && (src[ls] == ' ' || src[ls] == '\t' || src[ls] == '\r')) ls++;
        while (le > ls && (src[le-1] == ' ' || src[le-1] == '\t' || src[le-1] == '\r')) le--;
        if (ls >= le) continue;
        const u8 *line = src + ls; u32 ll = le - ls;

        if (first) {
            first = false;
            /* header: graph TD / flowchart LR / … — read direction token */
            u32 p = 0;
            while (p < ll && dg_is_idchar(line[p])) p++;     /* "graph"/"flowchart" */
            while (p < ll && (line[p] == ' ')) p++;
            if (p + 1 < ll) {
                u8 a = line[p], b = line[p+1];
                if ((a=='L'&&b=='R')) g_dir = 1;
                else if ((a=='B'&&b=='T')) g_dir = 2;
                else if ((a=='R'&&b=='L')) g_dir = 3;
                else g_dir = 0;  /* TD/TB */
            }
            continue;
        }

        if (ll >= 2 && line[0] == '%' && line[1] == '%') continue;   /* comment */
        /* skip directives we don't model */
        if (ll >= 8 && memcmp(line, "subgraph", 8) == 0) continue;
        if (ll == 3 && memcmp(line, "end", 3) == 0) continue;
        if (ll >= 5 && memcmp(line, "style", 5) == 0) continue;
        if (ll >= 8 && memcmp(line, "classDef", 8) == 0) continue;
        if (ll >= 5 && memcmp(line, "click", 5) == 0) continue;
        if (ll >= 9 && memcmp(line, "linkStyle", 9) == 0) continue;
        if (ll >= 5 && memcmp(line, "class", 5) == 0) continue;

        /* statement: nodeRef (op [|label|] nodeRef)* */
        u32 p = 0;
        i32 prev = dg_parse_noderef(line, ll, &p);
        if (prev < 0) continue;
        for (;;) {
            u8 style; bool arrow;
            u32 before = p;
            if (!dg_parse_edge_op(line, ll, &p, &style, &arrow)) { p = before; break; }
            /* optional |label| right after the operator */
            char elabel[64]; elabel[0] = 0;
            while (p < ll && line[p] == ' ') p++;
            if (p < ll && line[p] == '|') {
                p++; u32 lbs = p;
                while (p < ll && line[p] != '|') p++;
                dg_strcpy(elabel, sizeof elabel, line + lbs, p - lbs);
                if (p < ll) p++;  /* closing | */
            }
            i32 cur = dg_parse_noderef(line, ll, &p);
            if (cur < 0) break;
            if (g_nedges < DG_MAX_EDGES) {
                DEdge *e = &g_edges[g_nedges++];
                e->from = prev; e->to = cur; e->style = style; e->arrow = arrow;
                memcpy(e->label, elabel, strlen(elabel) + 1);
            }
            prev = cur;
        }
    }
}

/* ---- layout ---- */
static void dg_layout(f32 avail_w) {
    (void)avail_w;
    if (g_nnodes == 0) { g_w = g_h = 0; return; }

    /* node sizing from label length */
    for (u32 i = 0; i < g_nnodes; i++) {
        DNode *n = &g_nodes[i];
        usize cells = utf8_len((const u8 *)n->label, (u32)strlen(n->label));
        if (cells < 1) cells = 1;
        f32 w = (f32)(cells + 2) * g_cw;
        f32 h = g_ch * 1.9f;
        if (n->shape == SH_DIAMOND) { w += 2.0f * g_cw; h += 0.4f * g_ch; }
        if (n->shape == SH_CIRCLE)  { f32 d = w > h ? w : h; w = h = d * 1.05f; }
        n->w = w; n->h = h;
    }

    /* longest-path rank assignment (cap iterations → cycles just stop growing) */
    for (u32 i = 0; i < g_nnodes; i++) g_nodes[i].rank = 0;
    for (u32 it = 0; it < g_nnodes; it++) {
        bool changed = false;
        for (u32 e = 0; e < g_nedges; e++) {
            DNode *a = &g_nodes[g_edges[e].from], *b = &g_nodes[g_edges[e].to];
            if (b->rank < a->rank + 1) { b->rank = a->rank + 1; changed = true; }
        }
        if (!changed) break;
    }
    i32 maxrank = 0;
    for (u32 i = 0; i < g_nnodes; i++) if (g_nodes[i].rank > maxrank) maxrank = g_nodes[i].rank;
    /* BT / RL reverse the rank axis */
    if (g_dir == 2 || g_dir == 3)
        for (u32 i = 0; i < g_nnodes; i++) g_nodes[i].rank = maxrank - g_nodes[i].rank;

    bool horizontal = (g_dir == 1 || g_dir == 3);   /* LR / RL */
    f32 margin = g_cw * 1.5f;
    f32 rank_gap = horizontal ? g_cw * 5.0f : g_ch * 2.2f;
    f32 cross_gap = horizontal ? g_ch * 1.0f : g_cw * 2.0f;

    /* place rank by rank; "rank axis" = down (TD) or right (LR) */
    f32 rank_cursor = margin;
    f32 total_cross = 0;
    for (i32 r = 0; r <= maxrank; r++) {
        /* rank thickness = max node extent along the rank axis */
        f32 thick = 0;
        for (u32 i = 0; i < g_nnodes; i++) if (g_nodes[i].rank == r) {
            f32 t = horizontal ? g_nodes[i].w : g_nodes[i].h;
            if (t > thick) thick = t;
        }
        f32 cross_cursor = margin;
        for (u32 i = 0; i < g_nnodes; i++) if (g_nodes[i].rank == r) {
            DNode *n = &g_nodes[i];
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
}

/* ---- render ---- */
static void dg_node_center(const DNode *n, f32 ox, f32 oy, f32 *cx, f32 *cy) {
    *cx = ox + n->x + n->w * 0.5f;
    *cy = oy + n->y + n->h * 0.5f;
}
/* boundary point of node's box in the direction of (tx,ty) */
static void dg_edge_point(const DNode *n, f32 ox, f32 oy, f32 tx, f32 ty, f32 *px, f32 *py) {
    f32 cx, cy; dg_node_center(n, ox, oy, &cx, &cy);
    f32 dx = tx - cx, dy = ty - cy;
    f32 hw = n->w * 0.5f, hh = n->h * 0.5f;
    f32 sx = (fabsf(dx) > 1e-3f) ? hw / fabsf(dx) : 1e9f;
    f32 sy = (fabsf(dy) > 1e-3f) ? hh / fabsf(dy) : 1e9f;
    f32 s = sx < sy ? sx : sy;
    *px = cx + dx * s;
    *py = cy + dy * s;
}

void md_diagram_render(Renderer *r, const Theme *theme, const u8 *src, u32 len,
                       f32 x, f32 y, f32 avail_w, f32 cw, f32 ch) {
    if (!r || !src || !len) return;
    dg_parse(src, len, cw, ch);
    if (g_nnodes == 0) return;
    dg_layout(avail_w);

    Color fg     = theme ? theme->fg : (Color){0.86f,0.86f,0.90f,1.0f};
    Color edgec  = { fg.r*0.7f, fg.g*0.7f, fg.b*0.7f, fg.a };
    Color accent = theme ? theme->ansi[12] : (Color){0.45f,0.62f,1.0f,1.0f};
    Color fill   = theme ? theme->bg : (Color){0.12f,0.12f,0.14f,1.0f};
    fill.r = fminf(1.0f, fill.r + 0.07f); fill.g = fminf(1.0f, fill.g + 0.07f); fill.b = fminf(1.0f, fill.b + 0.07f);

    f32 thick = fmaxf(1.0f, ch * 0.06f);

    /* edges first (behind nodes) */
    for (u32 e = 0; e < g_nedges; e++) {
        DNode *a = &g_nodes[g_edges[e].from], *b = &g_nodes[g_edges[e].to];
        f32 acx, acy, bcx, bcy;
        dg_node_center(a, x, y, &acx, &acy);
        dg_node_center(b, x, y, &bcx, &bcy);
        f32 p0x, p0y, p1x, p1y;
        dg_edge_point(a, x, y, bcx, bcy, &p0x, &p0y);
        dg_edge_point(b, x, y, acx, acy, &p1x, &p1y);
        f32 et = g_edges[e].style == ES_THICK ? thick * 2.0f : thick;
        if (g_edges[e].style == ES_DOTTED) {
            /* dashed: short segments */
            f32 dx = p1x - p0x, dy = p1y - p0y;
            f32 dist = sqrtf(dx*dx + dy*dy);
            if (dist > 0.5f) {
                f32 step = 6.0f, ux = dx/dist, uy = dy/dist;
                for (f32 t = 0; t < dist; t += step*2.0f) {
                    f32 t2 = fminf(t + step, dist);
                    renderer_draw_line(r, p0x+ux*t, p0y+uy*t, p0x+ux*t2, p0y+uy*t2, et, edgec);
                }
            }
        } else {
            renderer_draw_line(r, p0x, p0y, p1x, p1y, et, edgec);
        }
        /* arrowhead at p1 */
        if (g_edges[e].arrow) {
            f32 dx = p1x - p0x, dy = p1y - p0y;
            f32 d = sqrtf(dx*dx + dy*dy);
            if (d > 0.5f) {
                f32 ux = dx/d, uy = dy/d;
                f32 ah = ch * 0.5f;
                /* two barbs at ~±28° */
                f32 c28 = 0.883f, s28 = 0.469f;
                f32 b1x = p1x - ah*(ux*c28 - uy*s28), b1y = p1y - ah*(uy*c28 + ux*s28);
                f32 b2x = p1x - ah*(ux*c28 + uy*s28), b2y = p1y - ah*(uy*c28 - ux*s28);
                renderer_draw_line(r, p1x, p1y, b1x, b1y, et, edgec);
                renderer_draw_line(r, p1x, p1y, b2x, b2y, et, edgec);
            }
        }
    }
    renderer_flush_lines(r);

    /* node fills + borders */
    for (u32 i = 0; i < g_nnodes; i++) {
        DNode *n = &g_nodes[i];
        f32 nx = x + n->x, ny = y + n->y;
        f32 radius = n->shape == SH_CIRCLE ? fminf(n->w, n->h) * 0.5f
                   : n->shape == SH_ROUND  ? n->h * 0.5f
                   : 3.0f;
        renderer_draw_rrect_simple(r, nx, ny, n->w, n->h, fill, radius);
    }
    renderer_flush_rrects(r);

    /* node borders as line rectangles (rrect_simple has no border on all backends
     * uniformly, so stroke with lines) + labels */
    for (u32 i = 0; i < g_nnodes; i++) {
        DNode *n = &g_nodes[i];
        f32 nx = x + n->x, ny = y + n->y;
        Color bc = (n->shape == SH_DIAMOND) ? accent : edgec;
        renderer_draw_line(r, nx, ny, nx + n->w, ny, thick, bc);
        renderer_draw_line(r, nx, ny + n->h, nx + n->w, ny + n->h, thick, bc);
        renderer_draw_line(r, nx, ny, nx, ny + n->h, thick, bc);
        renderer_draw_line(r, nx + n->w, ny, nx + n->w, ny + n->h, thick, bc);
    }
    renderer_flush_lines(r);

    /* labels (centered) */
    renderer_set_ui_scale(r, cw, ch);
    for (u32 i = 0; i < g_nnodes; i++) {
        DNode *n = &g_nodes[i];
        font_warm_text_glyphs(&r->font, n->label);
        usize cells = utf8_len((const u8 *)n->label, (u32)strlen(n->label));
        f32 tw = (f32)cells * cw;
        f32 tx = x + n->x + (n->w - tw) * 0.5f;
        f32 ty = y + n->y + (n->h - ch) * 0.5f;
        const u8 *p = (const u8 *)n->label; u32 k = 0;
        f32 gx = tx;
        while (n->label[k]) {
            u32 cp; u32 nn = utf8_decode(p + k, (u32)strlen(n->label) - k, &cp);
            if (!nn) break;
            if (cp >= 32) { renderer_push_glyph(r, gx, ty, cp, fg); gx += cw; }
            k += nn;
        }
    }
    /* edge labels on a chip at segment midpoint */
    for (u32 e = 0; e < g_nedges; e++) {
        if (!g_edges[e].label[0]) continue;
        DNode *a = &g_nodes[g_edges[e].from], *b = &g_nodes[g_edges[e].to];
        f32 acx, acy, bcx, bcy;
        dg_node_center(a, x, y, &acx, &acy);
        dg_node_center(b, x, y, &bcx, &bcy);
        f32 mx = (acx + bcx) * 0.5f, my = (acy + bcy) * 0.5f;
        font_warm_text_glyphs(&r->font, g_edges[e].label);
        usize cells = utf8_len((const u8 *)g_edges[e].label, (u32)strlen(g_edges[e].label));
        f32 tw = (f32)cells * cw;
        renderer_draw_rect(r, mx - tw*0.5f - 2.0f, my - ch*0.5f, tw + 4.0f, ch,
                           (Color){fill.r, fill.g, fill.b, 0.95f});
        f32 gx = mx - tw*0.5f; const u8 *p = (const u8 *)g_edges[e].label; u32 k = 0;
        while (g_edges[e].label[k]) {
            u32 cp; u32 nn = utf8_decode(p + k, (u32)strlen(g_edges[e].label) - k, &cp);
            if (!nn) break;
            if (cp >= 32) { renderer_push_glyph(r, gx, my - ch*0.5f, cp, fg); gx += cw; }
            k += nn;
        }
    }
    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_set_ui_scale(r, cw, ch);
}

void md_diagram_measure(const u8 *src, u32 len, f32 avail_w, f32 cw, f32 ch,
                        f32 *out_w, f32 *out_h) {
    f32 w = 0, h = 0;
    if (src && len) {
        dg_parse(src, len, cw, ch);
        if (g_nnodes > 0) { dg_layout(avail_w); w = g_w; h = g_h; }
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

MdDiagramKind md_diagram_probe(const u8 *src, u32 len) {
    u32 i = 0;
    while (i < len && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) i++;
    if (i + 5 <= len && memcmp(src + i, "graph", 5) == 0) return MD_DIAGRAM_FLOWCHART;
    if (i + 9 <= len && memcmp(src + i, "flowchart", 9) == 0) return MD_DIAGRAM_FLOWCHART;
    return MD_DIAGRAM_NONE;
}
