/* Liu — diagrams, formulas and pictures drawn over an agent's output. See agent_diagram.h. */
#include "ui/agent_diagram.h"
#include "ui/agent_visual.h"
#include "ui/mermaid_scan.h"
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/chrome_palette.h"
#include "ui/icon.h"
#include "core/utf8.h"
#include "platform/platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Declared here; defined in ui.c. */
extern void draw_text_ex(Renderer *r, const char *text, f32 x, f32 y,
                         Color fg, i32 max_chars, f32 step);

#define ADG_KNOWN      24         /* blocks a pane remembers as seen whole: a screen of formulas */
#define ADG_KNOWN_ROWS 512        /* rows of one remembered block */
#define ADG_SRC_SCRATCH (128u * 1024u)

/* A block seen whole: its source, what it draws, and its rows' hashes. */
typedef struct {
    u64        src_hash;
    char      *src;
    u32        len;
    AgentVisual *d;               /* NULL: seen, and not drawable */
    f32        cw, ch;
    u64       *rows;
    i32        nrows;
    i32        col0, col1;
    bool       source_shown;      /* the user asked for the agent's text */
    u64        used;
} AdgKnown;

/* A card on screen: which block, the screen row its first row is on (above
 * the top for one scrolled part way off), and the rows of it that show. */
typedef struct {
    i32 known;
    i32 row0, nrows;
    i32 vis0, vis1;
    i32 col0, col1;
    bool grew_top, grew_bot;      /* it took the blank row above / below its block */
} AdgCard;

struct AgentDiagramPane {
    AgentDiagramPaneHead head;    /* first: see agent_diagram_pane_release */
    MmdRow  *sum;
    i32      rows, cols;
    u32     *cp;                  /* one row's codepoints */
    char    *src;                 /* mmd_find's scratch */
    AdgKnown known[ADG_KNOWN];
    u64      clock;
    AdgCard  cards[MMD_MAX_BLOCKS];
    u32      ncards;
    bool     dirty;               /* a summary changed since the cards were placed */
    bool     busy;
    f32      cw, ch;              /* the cell the cards were placed for */
    /* The agent's output as the pane has shown it, so a block taller than
     * the pane is found whole; out0..out1 are this picture's output rows,
     * the rows a card may cover. */
    MmdTape  tape;
    i32      out0, out1;
};

/* ---- pane state ---- */

static void adg_known_clear(AdgKnown *k) {
    agv_destroy(k->d);
    free(k->src);
    free(k->rows);
    memset(k, 0, sizeof *k);
}

void agent_diagram_pane_free(AgentDiagramPane **state) {
    if (!state || !*state) return;
    AgentDiagramPane *p = *state;
    for (u32 i = 0; i < ADG_KNOWN; i++) adg_known_clear(&p->known[i]);
    mmd_tape_free(&p->tape);
    free(p->sum);
    free(p->cp);
    free(p->src);
    free(p);
    *state = NULL;
}

usize agent_diagram_pane_bytes(const AgentDiagramPane *p) {
    if (!p) return 0;
    usize b = sizeof *p + sizeof(MmdRow) * (usize)p->rows + sizeof(u32) * (usize)p->cols +
              (p->src ? ADG_SRC_SCRATCH : 0) + mmd_tape_bytes(&p->tape);
    for (u32 i = 0; i < ADG_KNOWN; i++) {
        const AdgKnown *k = &p->known[i];
        b += k->len + sizeof(u64) * (usize)k->nrows + agv_bytes(k->d);
    }
    return b;
}

static bool adg_ensure(AgentDiagramPane **state, i32 rows, i32 cols) {
    AgentDiagramPane *p = *state;
    if (!p) {
        p = (AgentDiagramPane *)calloc(1, sizeof *p);
        if (!p) return false;
        p->head.free = agent_diagram_pane_free;
        *state = p;
    }
    if (p->rows != rows || p->cols != cols || !p->sum || !p->cp) {
        MmdRow *sum = (MmdRow *)calloc((usize)rows, sizeof *sum);
        u32 *cp = (u32 *)calloc((usize)cols, sizeof *cp);
        if (!sum || !cp) { free(sum); free(cp); return false; }
        free(p->sum); free(p->cp);
        p->sum = sum; p->cp = cp;
        p->rows = rows; p->cols = cols;
        p->dirty = true;
        p->ncards = 0;
    }
    if (!p->src) {
        p->src = (char *)malloc(ADG_SRC_SCRATCH);
        if (!p->src) return false;
    }
    return true;
}

/* One visible row as codepoints: an empty cell and the second half of a wide
 * character are 0, a cluster its base. */
static bool adg_read_row(void *ctx, i32 row, u32 *cp, i32 cols) {
    Terminal *t = (Terminal *)ctx;
    i32 len = 0;
    const Cell *cells = terminal_visible_row(t, row, &len);
    if (!cells) return false;
    for (i32 x = 0; x < cols; x++) {
        if (x >= len) { cp[x] = 0; continue; }
        const Cell *c = &cells[x];
        u32 v = c->codepoint;
        if ((c->attr.flags & (ATTR_WDUMMY | ATTR_HIDDEN)) || v < 32) { cp[x] = 0; continue; }
        if (v & (1u << 30)) v = utf8_cluster_base(v);
        cp[x] = v;
    }
    return true;
}

static i32 adg_known_find(AgentDiagramPane *p, u64 hash, f32 cw, f32 ch) {
    for (i32 i = 0; i < ADG_KNOWN; i++)
        if (p->known[i].src && p->known[i].src_hash == hash &&
            p->known[i].cw == cw && p->known[i].ch == ch)
            return i;
    return -1;
}

/* Remember a block seen whole, building its diagram once: an empty slot,
 * else the one used longest ago that no card placed this pass stands on. */
static i32 adg_known_add(AgentDiagramPane *p, const char *src, u32 len, u64 hash,
                         f32 cw, f32 ch, const bool *in_use) {
    i32 slot = -1;
    for (i32 i = 0; i < ADG_KNOWN; i++) {
        if (in_use[i]) continue;
        if (!p->known[i].src) { slot = i; break; }
        if (slot < 0 || p->known[i].used < p->known[slot].used) slot = i;
    }
    if (slot < 0) return -1;
    AdgKnown *k = &p->known[slot];
    adg_known_clear(k);
    k->src = (char *)malloc(len + 1);
    if (!k->src) return -1;
    memcpy(k->src, src, len);
    k->src[len] = 0;
    k->len = len;
    k->src_hash = hash;
    k->cw = cw; k->ch = ch;
    agv_set_wake(platform_wake_event_loop);
    k->d = agv_create(src, len, cw, ch, true);
    k->used = ++p->clock;
    return slot;
}

static void adg_known_rows(AdgKnown *k, const MmdRow *sum, i32 row0, i32 row1, i32 col0, i32 col1) {
    i32 n = row1 - row0 + 1;
    if (n <= 0 || n > ADG_KNOWN_ROWS) return;
    if (k->nrows != n || !k->rows) {
        u64 *rows = (u64 *)realloc(k->rows, sizeof(u64) * (usize)n);
        if (!rows) return;
        k->rows = rows;
        k->nrows = n;
    }
    for (i32 i = 0; i < n; i++) k->rows[i] = sum[row0 + i].hash;
    k->col0 = col0;
    k->col1 = col1;
}

static bool adg_rows_taken(const AgentDiagramPane *p, i32 v0, i32 v1) {
    for (u32 c = 0; c < p->ncards; c++)
        if (v0 <= p->cards[c].vis1 && v1 >= p->cards[c].vis0) return true;
    return false;
}

/* A card shorter than what it holds needs takes the blank row on either
 * side of its block as well, where that row shows and no other card has it:
 * a formula is one row of source and two or three of drawing. Blank rows
 * only, so nothing of the agent's text is covered. */
#define ADG_SHORT_ROWS 4

static bool adg_row_free(const AgentDiagramPane *p, i32 row) {
    return row >= p->out0 && row < p->out1 && row >= 0 && row < p->rows &&
           p->sum[row].kind == MMD_ROW_BLANK && !p->sum[row].rule && !adg_rows_taken(p, row, row);
}

static AdgCard adg_grow(const AgentDiagramPane *p, AdgCard c) {
    if (c.nrows >= ADG_SHORT_ROWS) return c;
    if (c.vis0 == c.row0 && adg_row_free(p, c.row0 - 1)) { c.row0--; c.nrows++; c.vis0--; c.grew_top = true; }
    i32 below = c.row0 + c.nrows;
    if (c.vis1 == below - 1 && adg_row_free(p, below)) { c.nrows++; c.vis1++; c.grew_bot = true; }
    return c;
}

/* A card of fewer rows than this has no header row: what it holds takes all
 * of it, and its two buttons stand in its top corner while the pointer is on
 * it. A formula of one or two rows read at a third of its size under one. */
#define ADG_HEADER_ROWS 4
/* A formula is carded only where it is drawn at this much of the terminal's
 * text or more: under it the TeX the agent printed reads better. */
#define ADG_MATH_MIN_SCALE 0.6f

/* Where a card and what it holds go, in the pane's pixels from (ox, oy): the
 * frame, the box its drawing is fitted into, and the scale it is drawn at
 * (never past the terminal's own cell). A card taller than the pane's output
 * is fitted to the output's height, so its size holds while it scrolls. */
typedef struct {
    f32  x0, x1, vy0, vy1;        /* the frame, rows that show or not */
    f32  bx0, bx1, by0, by1;      /* the drawing's box */
    f32  bsz;                     /* a button's side */
    f32  s;                       /* 0: nothing fits */
    bool header;
} AdgGeom;

static AdgGeom adg_geom(const AgentDiagramPane *p, const AdgCard *c, const AgentVisual *v,
                        f32 ox, f32 oy, f32 cw, f32 ch) {
    AdgGeom g;
    memset(&g, 0, sizeof g);
    f32 pane_x1 = ox + (f32)p->cols * cw;
    g.x0 = ox + (f32)c->col0 * cw - cw * 0.6f;
    if (g.x0 < ox) g.x0 = ox;
    g.x1 = pane_x1 - cw * 0.4f;
    if (g.x1 < ox + (f32)(c->col1 + 1) * cw) g.x1 = fminf(pane_x1, ox + (f32)(c->col1 + 1) * cw);
    g.vy0 = oy + (f32)c->row0 * ch;
    g.vy1 = oy + (f32)(c->row0 + c->nrows) * ch;
    /* a blank row it took keeps part of itself: the text beside stays clear of the card */
    if (c->grew_top) g.vy0 += ch * 0.2f;
    if (c->grew_bot) g.vy1 -= ch * 0.2f;
    g.header = c->nrows >= ADG_HEADER_ROWS;
    f32 pad = cw * 0.8f;
    g.bx0 = g.x0 + pad;
    g.bx1 = g.x1 - pad;
    f32 out_h = (f32)(p->out1 - p->out0) * ch;
    f32 room;
    if (g.header) {
        g.bsz = ch * 0.9f;
        g.by0 = g.vy0 + ch;
        g.by1 = g.vy1 - ch * 0.25f;
        room = fminf(g.by1 - g.by0, out_h - ch * 1.25f);
    } else {
        /* the buttons' corner is kept clear on the right */
        g.bsz = ch * 0.8f;
        g.bx1 -= g.bsz * 2.0f + cw * 0.9f;
        g.by0 = g.vy0 + ch * 0.08f;
        g.by1 = g.vy1 - ch * 0.08f;
        room = fminf(g.by1 - g.by0, out_h - ch * 0.5f);
    }
    f32 dw = 0, dh = 0;
    agv_extent(v, &dw, &dh);
    if (g.bx1 > g.bx0 && room > 0 && dw > 0 && dh > 0)
        g.s = fminf(1.0f, fminf((g.bx1 - g.bx0) / dw, room / dh));
    /* A formula's card is as wide as what it covers of the agent's text or
     * as the formula drawn, whichever is wider: a line of TeX, not the pane. */
    if (agv_type(v) == AGV_MATH && g.s > 0.0f) {
        f32 text_x1 = fminf(pane_x1, ox + (f32)(c->col1 + 1) * cw + cw * 0.6f);
        f32 need = g.x1 - (g.bx1 - g.bx0 - dw * g.s);
        f32 x1 = fmaxf(text_x1, need);
        if (x1 < g.x1) {
            g.bx1 -= g.x1 - x1;
            g.x1 = x1;
        }
    }
    return g;
}

/* Whether a card is worth drawing over the agent's text: a formula is set
 * in display style where that reads, else in text style, and one that would
 * be drawn smaller than reads either way is left as the TeX it is. */
static bool adg_card_reads(const AgentDiagramPane *p, const AdgCard *c, AgentVisual *v, f32 cw, f32 ch) {
    if (agv_type(v) != AGV_MATH) return true;
    agv_set_text_style(v, false);
    if (adg_geom(p, c, v, 0, 0, cw, ch).s >= ADG_MATH_MIN_SCALE) return true;
    agv_set_text_style(v, true);
    if (adg_geom(p, c, v, 0, 0, cw, ch).s >= ADG_MATH_MIN_SCALE) return true;
    agv_set_text_style(v, false);
    return false;
}

/* Find the blocks and decide which cards go where. The blocks are looked
 * for in the tape — what the pane has shown of the agent's output — and
 * those with rows on screen get a card over the output rows they cover. */
static void adg_place(AgentDiagramPane *p, Terminal *view, f32 cw, f32 ch) {
    i32 cursor = view->cursor_visible ? view->cursor_y + view->scroll_offset : -1;
    MmdScreen scr;
    bool taped = mmd_tape_take(&p->tape, p->sum, p->rows, p->cols, cursor, adg_read_row, view);
    if (taped) {
        mmd_tape_screen(&p->tape, &scr, p->busy);
        p->out0 = p->tape.out0;
        p->out1 = p->tape.out1;
    } else {
        scr = (MmdScreen){ p->sum, p->rows, p->cols, adg_read_row, view, p->busy, false, 0, 0 };
        p->out0 = 0;
        p->out1 = p->rows;
    }
    MmdBlock b[MMD_MAX_BLOCKS];
    u32 n = mmd_find(&scr, b, MMD_MAX_BLOCKS, p->src, ADG_SRC_SCRATCH);
    p->ncards = 0;
    bool placed[ADG_KNOWN] = { false };
    for (u32 i = 0; i < n && p->ncards < MMD_MAX_BLOCKS; i++) {
        const MmdBlock *bl = &b[i];
        i32 k = adg_known_find(p, bl->src_hash, cw, ch);
        /* A block known whole stays whole: the same source was seen ended.
         * The agent at work again under it changes nothing about it. */
        if (k < 0 && !bl->complete) continue;
        if (k < 0) k = adg_known_add(p, p->src + bl->src_off, bl->src_len, bl->src_hash, cw, ch, placed);
        if (k < 0) continue;
        AdgKnown *kn = &p->known[k];
        kn->used = ++p->clock;
        adg_known_rows(kn, scr.rows, bl->row0, bl->row1, bl->col0, bl->col1);
        placed[k] = true;
        if (!kn->d) continue;
        i32 r0 = taped ? mmd_tape_screen_row(&p->tape, bl->row0) : bl->row0;
        i32 r1 = taped ? mmd_tape_screen_row(&p->tape, bl->row1) : bl->row1;
        i32 v0 = r0 > p->out0 ? r0 : p->out0;
        i32 v1 = r1 < p->out1 - 1 ? r1 : p->out1 - 1;
        if (v1 < v0) continue;
        AdgCard card = adg_grow(p, (AdgCard){ k, r0, r1 - r0 + 1, v0, v1, bl->col0, bl->col1 });
        if (!adg_card_reads(p, &card, kn->d, cw, ch)) continue;
        p->cards[p->ncards++] = card;
    }
    /* Blocks seen whole before and now only partly on screen, where the tape
     * no longer holds them (it starts again when a picture shares too few
     * rows with the one before). */
    for (i32 k = 0; k < ADG_KNOWN && p->ncards < MMD_MAX_BLOCKS; k++) {
        AdgKnown *kn = &p->known[k];
        if (placed[k] || !kn->d || !kn->rows || kn->cw != cw || kn->ch != ch) continue;
        i32 r0, v0, v1;
        if (!mmd_locate_known(p->sum, p->rows, kn->rows, kn->nrows, &r0, &v0, &v1)) continue;
        if (v0 < p->out0) v0 = p->out0;
        if (v1 > p->out1 - 1) v1 = p->out1 - 1;
        if (v1 < v0 || adg_rows_taken(p, v0, v1)) continue;
        kn->used = ++p->clock;
        AdgCard card = adg_grow(p, (AdgCard){ k, r0, kn->nrows, v0, v1, kn->col0, kn->col1 });
        if (!adg_card_reads(p, &card, kn->d, cw, ch)) continue;
        p->cards[p->ncards++] = card;
    }
}

/* ---- drawing ---- */

static void adg_publish(AppState *app, const Terminal *term, u64 hash, u8 kind,
                        f32 x, f32 y, f32 w, f32 h) {
    if (app->adg_hit_count >= ADG_MAX_HITS) return;
    app->adg_hit[app->adg_hit_count++] = (AgentDiagramHit){ term, hash, kind, x, y, w, h, 0.0f };
}

/* A small button on the card's header row: an icon, a hover wash. */
static void adg_button(AppState *app, const Terminal *term, u64 hash, u8 kind, IconKind icon,
                       f32 x, f32 y, f32 sz, Color ink, Color wash_c, f32 clip_y0, f32 clip_y1) {
    Renderer *r = &app->renderer;
    f32 hov = ui_hover_amount(x, y, sz, sz, app->hover_x, app->hover_y);
    if (hov > 0.01f) {
        Color wash = wash_c;
        wash.a *= hov;
        renderer_draw_rrect_simple(r, x, y, sz, sz, wash, 4.0f * app->dpi_scale);
        renderer_flush_rrects(r);
    }
    f32 isz = sz * 0.62f;
    Color c = ink;
    c.a *= 0.70f + 0.30f * hov;
    icon_draw(r, icon, x + (sz - isz) * 0.5f, y + (sz - isz) * 0.5f, isz, c);
    /* Only the part that shows takes a click. */
    f32 y0 = fmaxf(y, clip_y0), y1 = fminf(y + sz, clip_y1);
    if (y1 > y0) adg_publish(app, term, hash, kind, x, y0, sz, y1 - y0);
}

/* What a card holds, as a mark in its header: a graph, a picture, or a sum
 * sign for a formula (no icon is one). */
static void adg_type_mark(Renderer *r, AgvType type, f32 x, f32 row_y, f32 isz, f32 cw, f32 ch, Color ink) {
    if (type == AGV_MATH) {
        renderer_reset_ui_scale(r);
        renderer_push_glyph(r, x + (isz - cw) * 0.5f, row_y, 0x2211, ink);
        renderer_flush_glyphs(r);
        return;
    }
    icon_draw(r, type == AGV_SVG ? ICON_FT_IMAGE : ICON_GRAPH, x, row_y + (ch - isz) * 0.5f, isz, ink);
}

static void adg_draw_card(AppState *app, const Terminal *source, const AgentDiagramPane *p,
                          const AdgCard *c, f32 ox, f32 oy, f32 cw, f32 ch) {
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme;
    const AdgKnown *kn = &p->known[c->known];
    f32 dpi = app->dpi_scale;
    ChromePalette cp = chrome_palette_for(t);
    Color accent = theme_ui_accent(t);
    Color dim = color_lerp(cp.surface_neutral, t->fg, 0.62f);
    Color wash = accent; wash.a = 0.16f;

    AdgGeom g = adg_geom(p, c, kn->d, ox, oy, cw, ch);
    f32 x0 = g.x0, x1 = g.x1, vy0 = g.vy0, vy1 = g.vy1;
    f32 cy0 = oy + (f32)c->vis0 * ch;                 /* the rows of it on screen */
    f32 cy1 = oy + (f32)(c->vis1 + 1) * ch;
    if (cy1 <= cy0 || x1 <= x0) return;

    if (kn->source_shown) {
        /* The agent's text stays; a pill on the first row brings the diagram
         * back. */
        if (c->vis0 > c->row0) return;
        AgvType ty = agv_type(kn->d);
        const char *lbl = ty == AGV_MATH ? "Formula" : ty == AGV_SVG ? "Picture" : "Diagram";
        f32 pw = (f32)(strlen(lbl) + 3) * cw;
        f32 px = x1 - pw - cw * 0.2f, py = vy0 + ch * 0.12f, ph = ch * 0.76f;
        f32 hov = ui_hover_amount(px, py, pw, ph, app->hover_x, app->hover_y);
        Color fill = color_lerp(cp.surface_neutral, accent, 0.10f + 0.10f * hov);
        renderer_draw_rrect_bordered(r, px, py, pw, ph, fill, cp.divider_subtle, fmaxf(1.0f, dpi),
                                     ph * 0.5f, ph * 0.5f, ph * 0.5f, ph * 0.5f, 0, 0, 0, 0);
        renderer_flush_rrects(r);
        f32 isz = ch * 0.5f;
        adg_type_mark(r, agv_type(kn->d), px + cw * 0.6f, vy0, isz, cw, ch, accent);
        renderer_reset_ui_scale(r);
        draw_text_ex(r, lbl, px + cw * 0.6f + isz + cw * 0.4f, vy0, t->fg, (i32)strlen(lbl), cw);
        renderer_flush_glyphs(r);
        adg_publish(app, source, kn->src_hash, ADG_HIT_DIAGRAM, px, py, pw, ph);
        return;
    }

    renderer_push_scissor(r, x0 - 2.0f, cy0, x1 - x0 + 4.0f, cy1 - cy0);
    f32 rad = 7.0f * dpi;
    renderer_draw_rrect_bordered(r, x0, vy0 + 1.0f, x1 - x0, vy1 - vy0 - 2.0f,
                                 cp.surface_neutral, cp.divider_subtle, fmaxf(1.0f, dpi),
                                 rad, rad, rad, rad, 0, 0, 0, 0);
    renderer_flush_rrects(r);

    f32 bx0 = g.bx0, bx1 = g.bx1, by0 = g.by0, by1 = g.by1, s = g.s;
    f32 dw, dh;
    agv_extent(kn->d, &dw, &dh);

    /* Header: what it is on the left (and, when the rows only hold it small,
     * that a click shows it large), the two buttons on the right. A card
     * without one keeps the buttons in its top corner, shown while the
     * pointer is on the card. */
    f32 bsz = g.bsz;
    f32 by = g.header ? vy0 + (ch - bsz) * 0.5f : vy0 + ch * 0.1f;
    bool buttons = true;
    if (g.header) {
        f32 isz = ch * 0.56f;
        adg_type_mark(r, agv_type(kn->d), x0 + cw * 0.7f, vy0, isz, cw, ch, accent);
        char title[64], head[112];
        agv_title(kn->d, title, sizeof title);
        snprintf(head, sizeof head, "%s%s", title, s > 0.0f && s < 0.7f ? "  \xC2\xB7  click to enlarge" : "");
        renderer_reset_ui_scale(r);
        draw_text_ex(r, head, x0 + cw * 0.7f + isz + cw * 0.6f, vy0, dim,
                     (i32)utf8_len((const u8 *)head, strlen(head)), cw);
        renderer_flush_glyphs(r);
    } else {
        f32 hy0 = fmaxf(cy0, vy0), hy1 = fminf(cy1, vy1);
        buttons = hy1 > hy0 && ui_hover_amount(x0, hy0, x1 - x0, hy1 - hy0, app->hover_x, app->hover_y) > 0.01f;
    }
    if (buttons) {
        f32 bx = x1 - bsz - cw * 0.4f;
        adg_button(app, source, kn->src_hash, ADG_HIT_EXPAND, ICON_MAXIMIZE, bx, by, bsz, t->fg, wash, cy0, cy1);
        bx -= bsz + cw * 0.3f;
        adg_button(app, source, kn->src_hash, ADG_HIT_SOURCE, ICON_FT_CODE, bx, by, bsz, t->fg, wash, cy0, cy1);
        renderer_flush_rrects(r);
    }

    if (s > 0.0f) {
        /* a formula stands where its TeX began, as the line it replaces */
        f32 gx = agv_type(kn->d) == AGV_MATH ? bx0 : bx0 + ((bx1 - bx0) - dw * s) * 0.5f;
        f32 gy = by0 + ((by1 - by0) - dh * s) * 0.5f;
        /* Centred in the card; where the card runs past the rows that show,
         * moved into them — but never out of its card. */
        f32 lo = cy0 + ch * 0.25f, hi = cy1 - ch * 0.25f - dh * s;
        if (hi < lo) hi = lo;
        gy = fminf(fmaxf(gy, lo), hi);
        gy = fminf(fmaxf(gy, by0), fmaxf(by0, by1 - dh * s));
        MdDiagramInks inks;
        md_diagram_inks(t, cp.surface_neutral, accent, &inks);
        agv_draw(r, kn->d, &inks, gx, gy, s, 1.0f);
        renderer_reset_ui_scale(r);
    }
    renderer_pop_scissor(r);

    /* The card takes the click everywhere the buttons do not: published
     * after them, so a hit test that takes the first match finds a button
     * first. */
    f32 hy0 = fmaxf(cy0, vy0), hy1 = fminf(cy1, vy1);
    if (hy1 > hy0) {
        adg_publish(app, source, kn->src_hash, ADG_HIT_CARD, x0, hy0, x1 - x0, hy1 - hy0);
        if (app->adg_hit_count > 0 && app->adg_hit[app->adg_hit_count - 1].kind == ADG_HIT_CARD)
            app->adg_hit[app->adg_hit_count - 1].scale = s;
    }
    app->adg_cards++;
}

void agent_diagram_pane_pass(AppState *app, Terminal *source, Terminal *view,
                             AgentDiagramPane **state, f32 ox, f32 oy, bool full, bool draw) {
    if (!app || !source || !view || !state) return;
    if (!app->config.agent_diagrams) { agent_diagram_pane_free(state); return; }
    PaneAgent *pa = app_pane_agent_for_terminal(app, source);
    if (!pa || !pa->tool) { agent_diagram_pane_free(state); return; }
    i32 rows = view->rows, cols = view->cols;
    if (rows <= 0 || cols <= 0) return;
    bool fresh = !*state || (*state)->rows != rows || (*state)->cols != cols;
    if (!adg_ensure(state, rows, cols)) return;
    AgentDiagramPane *p = *state;
    Renderer *r = &app->renderer;
    f32 cw = r->font.cell_width, ch = r->font.cell_height;

    /* Rows that changed are summarised again; only a summary that differs
     * asks for the cards to be placed again. */
    for (i32 row = 0; row < rows; row++) {
        if (!(full || fresh || term_row_dirty(view, row))) continue;
        MmdRow s;
        if (adg_read_row(view, row, p->cp, cols)) mmd_row_summarize(p->cp, cols, &s);
        else { memset(&s, 0, sizeof s); s.first = s.last = s.t0 = s.t1 = -1; }
        MmdRow *o = &p->sum[row];
        if (s.hash != o->hash || s.kind != o->kind || s.first != o->first || s.last != o->last) {
            *o = s;
            p->dirty = true;
        }
    }
    bool busy = platform_time_sec() < pa->work_until;
    if (busy != p->busy) { p->busy = busy; p->dirty = true; }
    if (cw != p->cw || ch != p->ch) { p->cw = cw; p->ch = ch; p->dirty = true; }
    if (p->dirty) {
        p->dirty = false;
        adg_place(p, view, cw, ch);
    }
    if (!draw) return;
    for (u32 i = 0; i < p->ncards; i++)
        adg_draw_card(app, source, p, &p->cards[i], ox, oy, cw, ch);
}

/* ---- clicks ---- */

i32 agent_diagram_hit(const AppState *app, f32 x, f32 y) {
    if (!app) return -1;
    for (i32 i = 0; i < app->adg_hit_count; i++) {
        const AgentDiagramHit *h = &app->adg_hit[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) return i;
    }
    return -1;
}

/* The pane state behind a published control, re-found by its terminal. */
static AgentDiagramPane *adg_pane_of(AppState *app, const Terminal *term) {
    for (i32 ti = 0; ti < app->tab_count; ti++) {
        Tab *tab = &app->tabs[ti];
        if (tab->kind != TAB_TERMINAL) continue;
        for (i32 pi = 0; pi < MAX_SPLIT_PANES; pi++) {
            if (app_tab_pane_terminal(tab, pi) != term) continue;
            TermRenderCache *c = app_tab_pane_cache(tab, pi);
            return c ? c->adg : NULL;
        }
    }
    return NULL;
}

bool agent_diagram_click(AppState *app, i32 hit) {
    if (!app || hit < 0 || hit >= app->adg_hit_count) return false;
    AgentDiagramHit h = app->adg_hit[hit];
    AgentDiagramPane *p = adg_pane_of(app, h.term);
    if (!p) return false;
    AdgKnown *kn = NULL;
    for (u32 i = 0; i < ADG_KNOWN; i++)
        if (p->known[i].src && p->known[i].src_hash == h.src_hash) { kn = &p->known[i]; break; }
    if (!kn) return false;
    switch (h.kind) {
    case ADG_HIT_SOURCE:  kn->source_shown = true;  return true;
    case ADG_HIT_DIAGRAM: kn->source_shown = false; return true;
    default:              return diagram_viewer_open(app, kn->src, kn->len);
    }
}

/* =========================================================================
 * The viewer
 * ========================================================================= */

#define DGV_ZOOM_MIN 0.25f
#define DGV_ZOOM_MAX 8.0f

static void dgv_home(AppState *app);

static void dgv_release(DiagramViewer *v) {
    agv_destroy(v->vis);
    free(v->src);
    v->vis = NULL;
    v->src = NULL;
    v->len = 0;
}

void diagram_viewer_free(DiagramViewer *v) {
    if (!v) return;
    dgv_release(v);
    v->open = false;
}

bool diagram_viewer_animating(const DiagramViewer *v) {
    return v && ((v->open && !anim_done(&v->open_anim)) || !anim_done(&v->close_anim));
}

bool diagram_viewer_open(AppState *app, const char *src, u32 len) {
    if (!app || !src || !len) return false;
    DiagramViewer *v = &app->dgv;
    Renderer *r = &app->renderer;
    f32 cw = r->font.cell_width, ch = r->font.cell_height;
    agv_set_wake(platform_wake_event_loop);
    AgentVisual *d = agv_create(src, len, cw, ch, false);
    if (!d) { app_show_toast(app, "This cannot be drawn"); return false; }
    char *copy = (char *)malloc(len + 1);
    if (!copy) { agv_destroy(d); return false; }
    memcpy(copy, src, len);
    copy[len] = 0;
    dgv_release(v);
    v->src = copy;
    v->len = len;
    v->vis = d;
    v->cw = cw; v->ch = ch;
    v->dragging = false;
    v->open = true;
    dgv_home(app);
    return true;
}

void diagram_viewer_close(AppState *app) {
    if (!app || !app->dgv.open) return;
    app->dgv.open = false;
    app->dgv.dragging = false;
}

/* The box the diagram is shown in: the window less its margins and the
 * caption's band. */
static void dgv_box(const AppState *app, f32 *x, f32 *y, f32 *w, f32 *h) {
    f32 dpi = app->dpi_scale;
    f32 m = 40.0f * dpi, cap = 64.0f * dpi;
    *x = m; *y = m;
    *w = fmaxf(1.0f, (f32)app->fb_width - m * 2.0f);
    *h = fmaxf(1.0f, (f32)app->fb_height - m - cap - m * 0.5f);
}

#define DGV_READABLE 0.75f     /* the smallest text a first look shows, of the terminal's cell */
#define DGV_MATH_MAX 4.0f      /* the largest a formula opens at, of the terminal's cell */

static f32 dgv_fit(const AppState *app) {
    const DiagramViewer *v = &app->dgv;
    f32 bx, by, bw, bh, dw, dh;
    dgv_box(app, &bx, &by, &bw, &bh);
    agv_extent(v->vis, &dw, &dh);
    if (!(dw > 0) || !(dh > 0)) return 1.0f;
    /* Whole, up to twice the terminal's cell: a small diagram reads large,
     * and the glyphs are rastered at the size they are drawn at. A diagram
     * that is whole only with its text too small to read is shown at a size
     * that reads, as wide as the box allows, and panned. */
    f32 whole = fminf(bw / dw, bh / dh);
    f32 readable = fminf(DGV_READABLE, bw / dw);
    /* a formula is a line or two of text: it opens as large as a heading */
    f32 most = agv_type(v->vis) == AGV_MATH ? DGV_MATH_MAX : 2.0f;
    return fminf(most, fmaxf(whole, readable));
}

static void dgv_clamp_pan(AppState *app);

/* The first look: fitted, and a diagram taller than the box from its top. */
static void dgv_home(AppState *app) {
    DiagramViewer *v = &app->dgv;
    v->zoom = 1.0f;
    v->pan_x = 0.0f;
    v->pan_y = 0.0f;
    f32 bx, by, bw, bh, dw, dh;
    dgv_box(app, &bx, &by, &bw, &bh);
    agv_extent(v->vis, &dw, &dh);
    f32 s = dgv_fit(app);
    if (dh * s > bh) v->pan_y = (dh * s - bh) * 0.5f;
    dgv_clamp_pan(app);
}

static void dgv_clamp_pan(AppState *app) {
    DiagramViewer *v = &app->dgv;
    f32 bx, by, bw, bh, dw, dh;
    dgv_box(app, &bx, &by, &bw, &bh);
    agv_extent(v->vis, &dw, &dh);
    f32 s = dgv_fit(app) * v->zoom;
    f32 mx = fmaxf(0.0f, (dw * s - bw) * 0.5f + bw * 0.25f);
    f32 my = fmaxf(0.0f, (dh * s - bh) * 0.5f + bh * 0.25f);
    if (v->pan_x > mx) v->pan_x = mx;
    if (v->pan_x < -mx) v->pan_x = -mx;
    if (v->pan_y > my) v->pan_y = my;
    if (v->pan_y < -my) v->pan_y = -my;
}

void diagram_viewer_panel_rect(const AppState *app, f32 *x, f32 *y, f32 *w, f32 *h) {
    const DiagramViewer *v = &app->dgv;
    f32 bx, by, bw, bh, dw = 0, dh = 0;
    dgv_box(app, &bx, &by, &bw, &bh);
    agv_extent(v->vis, &dw, &dh);
    f32 s = dgv_fit(app) * v->zoom;
    f32 pw = fminf(bw, dw * s), ph = fminf(bh, dh * s);
    *x = bx + (bw - pw) * 0.5f;
    *y = by + (bh - ph) * 0.5f;
    *w = pw;
    *h = ph;
}

void diagram_viewer_render(AppState *app) {
    if (!app) return;
    DiagramViewer *v = &app->dgv;
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme;
    f32 dpi = app->dpi_scale;
    if (!v->open && !v->was_open && !v->close_anim.active) {
        if (v->vis) dgv_release(v);
        return;
    }
    f32 scale = 1.0f, prog = 1.0f;
    bool render = modal_anim_progress(v->open, &v->open_anim, &v->close_anim, &v->was_open,
                                      dpi, MODAL_OPEN_DUR_FAST, &scale, &prog, NULL);
    if (!render || !v->vis) {
        dgv_release(v);
        anim_reset(&v->close_anim);
        return;
    }
    if (diagram_viewer_animating(v)) anim_register_until(platform_time_sec() + 0.05);

    renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                       (Color){ 0.0f, 0.0f, 0.0f, 0.82f * prog });
    renderer_flush_rects(r);

    ChromePalette cp = chrome_palette_for(t);
    Color accent = theme_ui_accent(t);
    f32 bx, by, bw, bh, dw, dh;
    dgv_box(app, &bx, &by, &bw, &bh);
    agv_extent(v->vis, &dw, &dh);
    f32 s = dgv_fit(app) * v->zoom;
    f32 px, py, pw, ph;
    diagram_viewer_panel_rect(app, &px, &py, &pw, &ph);
    /* The panel grows a little round the drawing and scales with the
     * modal's curve about its centre. */
    f32 margin = 14.0f * dpi;
    f32 pcx = px + pw * 0.5f, pcy = py + ph * 0.5f;
    f32 qw = (pw + margin * 2.0f) * scale, qh = (ph + margin * 2.0f) * scale;
    Color surface = cp.surface_neutral;
    surface.a = prog;
    Color edge = cp.divider_subtle; edge.a *= prog;
    f32 rad = 10.0f * dpi;
    renderer_draw_rrect_bordered(r, pcx - qw * 0.5f, pcy - qh * 0.5f, qw, qh, surface, edge,
                                 fmaxf(1.0f, dpi), rad, rad, rad, rad,
                                 24.0f * dpi, 0.35f * prog, 0, 6.0f * dpi);
    renderer_flush_rrects(r);

    f32 ds = s * scale;
    f32 gx = bx + (bw - dw * ds) * 0.5f + v->pan_x * scale;
    f32 gy = by + (bh - dh * ds) * 0.5f + v->pan_y * scale;
    renderer_push_scissor(r, pcx - qw * 0.5f, pcy - qh * 0.5f, qw, qh);
    MdDiagramInks inks;
    md_diagram_inks(t, cp.surface_neutral, accent, &inks);
    agv_draw(r, v->vis, &inks, gx, gy, ds, prog);
    renderer_reset_ui_scale(r);
    renderer_pop_scissor(r);

    /* Caption on a plate under the panel, in the UI's own cell. */
    f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
    char title[64], line[112];
    agv_title(v->vis, title, sizeof title);
    snprintf(line, sizeof line, "%s  \xC2\xB7  %.0f%%", title, (f64)(s * 100.0f / fmaxf(1e-3f, 1.0f)));
    const char *hint = diagram_viewer_hint();
    i32 nl = (i32)utf8_len((const u8 *)line, strlen(line));
    i32 nh = (i32)utf8_len((const u8 *)hint, strlen(hint));
    f32 cap_y = by + bh + 14.0f * dpi;
    f32 pad_x = 14.0f * dpi, pad_y = 8.0f * dpi;
    i32 widest = nl > nh ? nl : nh;
    f32 cwid = (f32)widest * ui_cw + pad_x * 2.0f;
    f32 chei = ui_ch * 2.0f + 6.0f * dpi + pad_y * 2.0f;
    f32 cx = ((f32)app->fb_width - cwid) * 0.5f;
    Color plate = cp.surface_neutral; plate.a = prog;
    renderer_draw_rrect_simple(r, cx, cap_y, cwid, chei, plate, 8.0f * dpi);
    renderer_flush_rrects(r);
    Color fg = t->fg; fg.a = prog;
    Color dim = color_lerp(cp.surface_neutral, t->fg, 0.62f); dim.a = prog;
    renderer_set_ui_scale(r, ui_cw, ui_ch);
    draw_text_ex(r, line, ((f32)app->fb_width - (f32)nl * ui_cw) * 0.5f, cap_y + pad_y, fg, nl, ui_cw);
    draw_text_ex(r, hint, ((f32)app->fb_width - (f32)nh * ui_cw) * 0.5f,
                 cap_y + pad_y + ui_ch + 6.0f * dpi, dim, nh, ui_cw);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

const char *diagram_viewer_hint(void) {
#ifdef PLATFORM_MACOS
    return "Esc close  \xC2\xB7  drag or scroll to pan  \xC2\xB7  \xE2\x8C\x98 scroll or \xE2\x8C\x98+/\xE2\x88\x92 to zoom  \xC2\xB7  \xE2\x8C\x98" "0 fit  \xC2\xB7  \xE2\x8C\x98" "C copy source";
#else
    return "Esc close  \xC2\xB7  drag or scroll to pan  \xC2\xB7  Ctrl+scroll or Ctrl+=/\xE2\x88\x92 to zoom  \xC2\xB7  Ctrl+0 fit  \xC2\xB7  Ctrl+C copy source";
#endif
}

/* ---- viewer input ---- */

static void dgv_zoom_to(AppState *app, f32 z) {
    DiagramViewer *v = &app->dgv;
    if (z < DGV_ZOOM_MIN) z = DGV_ZOOM_MIN;
    if (z > DGV_ZOOM_MAX) z = DGV_ZOOM_MAX;
    if (z == v->zoom) return;
    /* the point at the middle stays at the middle */
    v->pan_x *= z / v->zoom;
    v->pan_y *= z / v->zoom;
    v->zoom = z;
    dgv_clamp_pan(app);
}

bool diagram_viewer_key(AppState *app, i32 key, u32 mods) {
    if (!app || !app->dgv.open) return false;
    DiagramViewer *v = &app->dgv;
    if (key == KEY_ESCAPE) { diagram_viewer_close(app); return true; }
    if ((mods & MOD_APP) && key == KEY_C) {
        AgvType ty = agv_type(v->vis);
        (void)app_clipboard_copy(app, v->src, ty == AGV_MATH ? "Formula source copied"
                                             : ty == AGV_SVG ? "SVG source copied" : "Diagram source copied");
        return true;
    }
    if (key == KEY_0) { diagram_viewer_zoom(app, 0); return true; }
    /* X11 reports these keys, and with Ctrl held it sends no text for them;
     * a bare one zooms through its text (diagram_viewer_char). */
    if ((mods & MOD_APP) && key == KEY_EQUAL) { diagram_viewer_zoom(app, 1); return true; }
    if ((mods & MOD_APP) && key == KEY_MINUS) { diagram_viewer_zoom(app, -1); return true; }
    f32 step = 48.0f * app->dpi_scale;
    if (key == KEY_LEFT)  { v->pan_x += step; dgv_clamp_pan(app); return true; }
    if (key == KEY_RIGHT) { v->pan_x -= step; dgv_clamp_pan(app); return true; }
    if (key == KEY_UP)    { v->pan_y += step; dgv_clamp_pan(app); return true; }
    if (key == KEY_DOWN)  { v->pan_y -= step; dgv_clamp_pan(app); return true; }
    /* Modal: nothing reaches the agent the user cannot see. */
    return true;
}

/* `=` `+` `-` arrive as text (no key of theirs is reported on macOS); ⌘+,
 * ⌘− and ⌘0 as the View menu's font size items, which mean the drawing's
 * size while the viewer is up; Ctrl+= Ctrl+− Ctrl+0 on X11 as keys. */
bool diagram_viewer_char(AppState *app, u32 cp) {
    if (!app || !app->dgv.open) return false;
    if (cp == '=' || cp == '+') diagram_viewer_zoom(app, 1);
    else if (cp == '-')         diagram_viewer_zoom(app, -1);
    else if (cp == '0')         diagram_viewer_zoom(app, 0);
    return true;
}

void diagram_viewer_zoom(AppState *app, i32 dir) {
    if (!app || !app->dgv.open) return;
    DiagramViewer *v = &app->dgv;
    if (dir > 0) dgv_zoom_to(app, v->zoom * 1.25f);
    else if (dir < 0) dgv_zoom_to(app, v->zoom / 1.25f);
    else dgv_home(app);
}

bool diagram_viewer_mouse_down(AppState *app, f32 x, f32 y) {
    if (!app || !app->dgv.open) return false;
    DiagramViewer *v = &app->dgv;
    f32 px, py, pw, ph;
    diagram_viewer_panel_rect(app, &px, &py, &pw, &ph);
    f32 m = 14.0f * app->dpi_scale;
    if (x >= px - m && x < px + pw + m && y >= py - m && y < py + ph + m) {
        v->dragging = true;
        v->drag_x = x;
        v->drag_y = y;
        return true;
    }
    diagram_viewer_close(app);
    return true;
}

bool diagram_viewer_mouse_up(AppState *app) {
    if (!app || !app->dgv.open) return false;
    app->dgv.dragging = false;
    return true;
}

bool diagram_viewer_mouse_move(AppState *app, f32 x, f32 y) {
    if (!app || !app->dgv.open) return false;
    DiagramViewer *v = &app->dgv;
    if (!v->dragging) return true;
    v->pan_x += x - v->drag_x;
    v->pan_y += y - v->drag_y;
    v->drag_x = x;
    v->drag_y = y;
    dgv_clamp_pan(app);
    return true;
}

bool diagram_viewer_scroll(AppState *app, f32 dx, f32 dy, bool precise, u32 mods) {
    if (!app || !app->dgv.open) return false;
    DiagramViewer *v = &app->dgv;
    if (mods & MOD_APP) {
        f32 step = precise ? dy * 0.006f : dy * 0.14f;
        dgv_zoom_to(app, v->zoom * (1.0f + step));
        return true;
    }
    /* A trackpad's pixels pan by themselves; a wheel's lines by a few rows. */
    f32 k = precise ? 1.0f : 40.0f * app->dpi_scale;
    v->pan_x += dx * k;
    v->pan_y += dy * k;
    dgv_clamp_pan(app);
    return true;
}
