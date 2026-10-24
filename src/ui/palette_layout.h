/*
 * Liu — where the command palette is, and how it moves.
 *
 * The palette is a panel under the tab strip with a line saying where you are,
 * a search field, rows whose highlight is a layer of its own that GLIDES
 * between them, and a footer saying what the keys do and how much was found.
 * PAL_FILM_* are the reference design's pixels; everything else derives from
 * them, converted to points.
 *
 * It is a header of its own, pure, for the reason ui/ctx_menu.h is: the
 * painter (ui.c) and the pointer (main.c) must be asking ONE author where a
 * row is. The panel also hugs its rows — five results make a five-row panel —
 * so "where is row 3" is not a constant anybody could multiply out by hand.
 */
#ifndef UI_PALETTE_LAYOUT_H
#define UI_PALETTE_LAYOUT_H

#include "core/types.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- the reference numbers ------------------------------------------------
 *
 * The reference design sets its text at 18 px, an advance of 10.8; the
 * palette's text is the UI cell, 8 pt an advance. That ratio is the
 * conversion, so the panel keeps its size RELATIVE TO THE TEXT inside it. */
#define PAL_FILM_PT            (8.0f / 10.8f)
#define PAL_FILM_W             720.0f
#define PAL_FILM_TOP           38.0f    /* under the tab strip (88, in a window whose strip is 50) */
#define PAL_FILM_RADIUS        16.0f
#define PAL_FILM_CRUMB_PAD_T   14.0f
#define PAL_FILM_CRUMB_PAD_X   22.0f
#define PAL_FILM_SMALL_LINE    19.0f    /* the line box of the 14.5 px text: the crumb, the footer */
#define PAL_FILM_FIELD_GAP_T   12.0f
#define PAL_FILM_INSET_X       14.0f    /* the field, the rows and the footer's rule share it */
#define PAL_FILM_FIELD_H       52.0f
#define PAL_FILM_FIELD_RADIUS  11.0f
#define PAL_FILM_FIELD_PAD_X   16.0f
#define PAL_FILM_FIELD_ICON    20.0f
#define PAL_FILM_FIELD_GAP     14.0f
#define PAL_FILM_LIST_GAP_T    12.0f    /* 10 under the field + 2 over the first row */
#define PAL_FILM_LIST_GAP_B    8.0f
#define PAL_FILM_ROW_H         46.0f
#define PAL_FILM_ROW2_H        62.0f    /* a two-line row: Agent History */
#define PAL_FILM_ROW_RADIUS    10.0f
#define PAL_FILM_ROW_PAD_X     16.0f
#define PAL_FILM_ROW_ICON      20.0f
#define PAL_FILM_ROW_GAP       16.0f
#define PAL_FILM_ROW2_ICON     28.0f
#define PAL_FILM_ROW2_LINE     24.0f
#define PAL_FILM_KEYS_H        28.0f
#define PAL_FILM_KEYS_RADIUS   6.0f
#define PAL_FILM_KEYS_PAD_X    8.0f
#define PAL_FILM_FOOT_PAD_T    11.0f
#define PAL_FILM_FOOT_PAD_B    13.0f
#define PAL_FILM_FOOT_PAD_X    8.0f
#define PAL_FILM_CARET_W       2.0f
#define PAL_FILM_CARET_H       24.0f
#define PAL_FILM_RISE          14.0f    /* it comes DOWN this far as it opens */
#define PAL_FILM_ROW_RISE      8.0f     /* a row of a list that has just arrived comes up this far */
#define PAL_FILM_SHADOW        80.0f
#define PAL_FILM_SHADOW_Y      28.0f
#define PAL_FILM_GLOW          26.0f    /* around a session row as it is taken */
/* Rows on show at most: six commands, or five two-line session rows. */
#define PAL_ROWS               6
#define PAL_ROWS2              5
/* The window's own gutter, in points: the panel never comes closer than this
 * to an edge of it. */
#define PAL_MARGIN_PT          16.0f

/* ---- how it moves ---------------------------------------------------------*/
#define PAL_OPEN_HZ            3.8f     /* the entrance is a spring: spring(t, 3.8, 0.9) */
#define PAL_OPEN_ZETA          0.9f
/* By here the spring is within a thousandth of rest. It is how long the open
 * animation is REGISTERED for, not a duration anything is eased over. */
#define PAL_OPEN_SEC           0.36
#define PAL_CLOSE_SEC          0.20     /* it leaves on a smoothstep */
#define PAL_SCALE_FROM         0.975f   /* about the middle of its TOP edge */
#define PAL_ALPHA_GAIN         1.4f     /* opaque before it has stopped moving */
#define PAL_GLIDE_SEC          0.16     /* the highlight, row to row: in-out cubic */
#define PAL_PICK_SEC           0.10     /* the taken row lights up as the panel goes */
#define PAL_PICK2_SEC          0.12
#define PAL_ENTER_LEAD_SEC     0.05     /* a list that arrives: its rows one after the other */
#define PAL_ENTER_STEP_SEC     0.05
#define PAL_ENTER_ROW_SEC      0.14
#define PAL_ENTER_SEL_LEAD_SEC 0.10     /* ...and its highlight after the first of them */
#define PAL_ENTER_SEL_SEC      0.20
#define PAL_CARET_HOLD_SEC     0.50     /* solid for this long after a key */
#define PAL_CARET_HALF_SEC     0.53     /* then on / off, this long each */
/* Reference opacities, which are display-space ones (see pal_linear_alpha). */
#define PAL_DIM                0.32f    /* the window behind it */
#define PAL_SHADOW             0.60f
#define PAL_SHADOW_LIGHT       0.20f    /* ...on a light theme */

typedef struct { f32 x, y, w, h; } PalRect;

static inline bool pal_rect_has(PalRect r, f32 px, f32 py) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

typedef struct {
    f32  fb_w, fb_h;    /* the framebuffer, px */
    f32  top;           /* height of the tab strip, px */
    f32  bottom;        /* height of the status bar (0 without one), px */
    f32  dpi;
    bool two_line;      /* Agent History's rows */
    i32  count;         /* rows the filter lets through */
    i32  min_rows;      /* rows kept open with nothing in them: a line that says why */
} PalLayoutIn;

typedef struct {
    f32     u;          /* px per PAL_FILM_* px */
    f32     hair;       /* what a hairline takes up in the stack: whole pixels */
    f32     line;       /* ...and how wide one is DRAWN: one PAL_FILM_* px, which is 1.48 at 2x */
    PalRect panel;      f32 radius;
    PalRect crumb;      /* the line box "Commands > folder ... Esc" is set in */
    PalRect field;      f32 field_radius;
    PalRect list;       /* the rows on show: also what the highlight spans */
    f32     row_h;      f32 row_radius;
    i32     rows;       /* rows drawn */
    i32     capacity;   /* the most this window can show */
    PalRect rule;       /* the footer's top line */
    PalRect foot;       /* the line box the footer's text is set in */
} PalLayout;

static inline f32 pal_round(f32 v) { return floorf(v + 0.5f); }
static inline f32 pal_clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* The palette at REST, on whole pixels. Every piece of the stack is rounded
 * by itself and the panel's height is their sum, so nothing below a rounded
 * piece inherits half a pixel from it. */
static inline PalLayout pal_layout(const PalLayoutIn *in) {
    PalLayout L;
    const f32 dpi = in->dpi > 0.0f ? in->dpi : 1.0f;
    const f32 u = PAL_FILM_PT * dpi;
    const f32 margin = pal_round(PAL_MARGIN_PT * dpi);
    L.u = u;
    L.hair = pal_round(u) < 1.0f ? 1.0f : pal_round(u);
    L.line = u < 1.0f ? 1.0f : u;

    /* Across: PAL_FILM_W, or what the window has between its gutters. */
    f32 w = pal_round(PAL_FILM_W * u);
    f32 max_w = floorf(in->fb_w - 2.0f * margin);
    if (max_w < 1.0f) max_w = floorf(in->fb_w) > 1.0f ? floorf(in->fb_w) : 1.0f;
    if (w > max_w) w = max_w;
    L.panel.w = w;
    L.panel.x = floorf((in->fb_w - w) * 0.5f);
    if (L.panel.x < 0.0f) L.panel.x = 0.0f;

    /* Down, from the panel's own top. */
    const f32 crumb_y = pal_round(PAL_FILM_CRUMB_PAD_T * u);
    const f32 crumb_h = pal_round(PAL_FILM_SMALL_LINE * u);
    const f32 field_y = crumb_y + crumb_h + pal_round(PAL_FILM_FIELD_GAP_T * u);
    const f32 field_h = pal_round(PAL_FILM_FIELD_H * u);
    const f32 list_y  = field_y + field_h + pal_round(PAL_FILM_LIST_GAP_T * u);
    const f32 gap_b   = pal_round(PAL_FILM_LIST_GAP_B * u);
    const f32 foot_t  = pal_round(PAL_FILM_FOOT_PAD_T * u);
    const f32 foot_h  = pal_round(PAL_FILM_SMALL_LINE * u);
    const f32 foot_b  = pal_round(PAL_FILM_FOOT_PAD_B * u);
    const f32 below   = gap_b + L.hair + foot_t + foot_h + foot_b;   /* under the last row */
    f32 row_h = pal_round((in->two_line ? PAL_FILM_ROW2_H : PAL_FILM_ROW_H) * u);
    if (row_h < 1.0f) row_h = 1.0f;
    L.row_h = row_h;

    /* How many rows the window has room for: PAL_ROWS (PAL_ROWS2 for
     * two-line rows), or fewer on a short window — never none, a palette
     * with no row cannot be used. */
    f32 top = pal_round(in->top + PAL_FILM_TOP * u);
    const f32 floor_y = in->fb_h - in->bottom - margin;
    i32 cap = (i32)((floor_y - top - list_y - below) / row_h);
    const i32 film_rows = in->two_line ? PAL_ROWS2 : PAL_ROWS;
    if (cap > film_rows) cap = film_rows;
    if (cap < 1) cap = 1;
    L.capacity = cap;

    i32 rows = in->count;
    if (rows < in->min_rows) rows = in->min_rows;
    if (rows > cap) rows = cap;
    if (rows < 0) rows = 0;
    L.rows = rows;

    const f32 h = list_y + (f32)rows * row_h + below;
    /* A window too short even for one row: the panel moves up before it runs
     * off the bottom, and stops at the gutter. */
    if (top + h > floor_y) top = floorf(floor_y - h);
    if (top < margin) top = margin;
    L.panel.y = top;
    L.panel.h = h;
    L.radius = PAL_FILM_RADIUS * u;

    const f32 inset = pal_round(PAL_FILM_INSET_X * u);
    const f32 crumb_x = pal_round(PAL_FILM_CRUMB_PAD_X * u);
    L.crumb = (PalRect){ L.panel.x + crumb_x, top + crumb_y, w - 2.0f * crumb_x, crumb_h };
    L.field = (PalRect){ L.panel.x + inset, top + field_y, w - 2.0f * inset, field_h };
    L.field_radius = PAL_FILM_FIELD_RADIUS * u;
    L.list  = (PalRect){ L.panel.x + inset, top + list_y, w - 2.0f * inset, (f32)rows * row_h };
    L.row_radius = PAL_FILM_ROW_RADIUS * u;
    L.rule  = (PalRect){ L.list.x, L.list.y + L.list.h + gap_b, L.list.w, L.hair };
    const f32 foot_x = pal_round(PAL_FILM_FOOT_PAD_X * u);
    L.foot  = (PalRect){ L.list.x + foot_x, L.rule.y + L.hair + foot_t, L.list.w - 2.0f * foot_x, foot_h };
    return L;
}

/* ---- motion ---------------------------------------------------------------*/

static inline f32 pal_smooth(f32 x) { x = pal_clamp01(x); return x * x * (3.0f - 2.0f * x); }
static inline f32 pal_out_cubic(f32 x) { x = 1.0f - pal_clamp01(x); return 1.0f - x * x * x; }
static inline f32 pal_in_out_cubic(f32 x) {
    x = pal_clamp01(x);
    if (x < 0.5f) return 4.0f * x * x * x;
    const f32 y = -2.0f * x + 2.0f;
    return 1.0f - y * y * y * 0.5f;
}

/* Step response of the entrance spring, `t` seconds after it was let go: 0,
 * then settling on 1 (it passes it by a seventh of a percent, once). Analytic,
 * so it is the same entrance at any frame rate. */
static inline f32 pal_spring(f32 t) {
    if (t <= 0.0f) return 0.0f;
    const f32 w = 2.0f * (f32)M_PI * PAL_OPEN_HZ;
    const f32 z = PAL_OPEN_ZETA;
    const f32 wd = w * sqrtf(1.0f - z * z);
    return 1.0f - expf(-z * w * t) * (cosf(wd * t) + (z * w / wd) * sinf(wd * t));
}

/* How open it is `t` seconds after being asked to open, having been `from`
 * open at that moment (0 for a closed palette; more when it is reopened while
 * still on its way out, so it turns round instead of starting over). */
static inline f32 pal_open_amount(f32 from, f32 t) {
    return from + (1.0f - from) * pal_spring(t);
}
/* ...and `t` seconds after being asked to close, from `from`. */
static inline f32 pal_close_amount(f32 from, f32 t) {
    return from * (1.0f - pal_smooth(t / (f32)PAL_CLOSE_SEC));
}
static inline f32 pal_alpha(f32 open) { return pal_clamp01(open * PAL_ALPHA_GAIN); }

/* The palette as it is DRAWN while it is `open` of the way in: a little
 * smaller about the middle of its top edge, and above where it will rest.
 * Fully open is the layout itself, untouched, so the resting panel stays on
 * its pixels. */
static inline PalRect pal_xf_rect(PalRect r, f32 cx, f32 y0, f32 s, f32 dy) {
    return (PalRect){ cx + (r.x - cx) * s, y0 + (r.y - y0) * s + dy, r.w * s, r.h * s };
}
static inline PalLayout pal_layout_at(const PalLayout *rest, f32 open) {
    if (open == 1.0f) return *rest;
    PalLayout L = *rest;
    const f32 s = PAL_SCALE_FROM + (1.0f - PAL_SCALE_FROM) * open;
    const f32 dy = -(1.0f - open) * PAL_FILM_RISE * rest->u;
    const f32 cx = rest->panel.x + rest->panel.w * 0.5f, y0 = rest->panel.y;
    L.panel = pal_xf_rect(rest->panel, cx, y0, s, dy);
    L.crumb = pal_xf_rect(rest->crumb, cx, y0, s, dy);
    L.field = pal_xf_rect(rest->field, cx, y0, s, dy);
    L.list  = pal_xf_rect(rest->list,  cx, y0, s, dy);
    L.rule  = pal_xf_rect(rest->rule,  cx, y0, s, dy);
    L.foot  = pal_xf_rect(rest->foot,  cx, y0, s, dy);
    L.rule.h = rest->hair;                      /* a hairline is not scaled */
    L.radius = rest->radius * s;
    L.field_radius = rest->field_radius * s;
    L.row_radius = rest->row_radius * s;
    L.row_h = rest->row_h * s;
    L.u = rest->u * s;
    return L;
}

/* The row on show under a point: 0 is the first one drawn, -1 is no row. A
 * row is as wide as its highlight — the gutters beside the list are not it. */
static inline i32 pal_row_at(const PalLayout *L, f32 px, f32 py) {
    if (L->rows <= 0 || L->row_h <= 0.0f) return -1;
    if (!pal_rect_has(L->list, px, py)) return -1;
    i32 v = (i32)((py - L->list.y) / L->row_h);
    if (v < 0) return -1;
    if (v >= L->rows) v = L->rows - 1;      /* the last pixel row, after rounding */
    return v;
}
/* Where row `v` is drawn. */
static inline PalRect pal_row_rect(const PalLayout *L, f32 v) {
    return (PalRect){ L->list.x, L->list.y + v * L->row_h, L->list.w, L->row_h };
}

/* How far in row `row` of a list is, `t` seconds after the list arrived: they
 * come one after the other, each over PAL_ENTER_ROW_SEC. */
static inline f32 pal_row_enter(f32 t, i32 row) {
    const f32 start = (f32)PAL_ENTER_LEAD_SEC + (f32)row * (f32)PAL_ENTER_STEP_SEC;
    return pal_out_cubic((t - start) / (f32)PAL_ENTER_ROW_SEC);
}
/* ...and the highlight under them. */
static inline f32 pal_sel_enter(f32 t) {
    return pal_out_cubic((t - (f32)PAL_ENTER_SEL_LEAD_SEC) / (f32)PAL_ENTER_SEL_SEC);
}
/* When every row of an arriving list of `rows` has landed. */
static inline f32 pal_enter_sec(i32 rows) {
    if (rows < 1) rows = 1;
    const f32 last = (f32)PAL_ENTER_LEAD_SEC + (f32)(rows - 1) * (f32)PAL_ENTER_STEP_SEC + (f32)PAL_ENTER_ROW_SEC;
    const f32 sel = (f32)PAL_ENTER_SEL_LEAD_SEC + (f32)PAL_ENTER_SEL_SEC;
    return last > sel ? last : sel;
}

/* How lit the taken row is, `t` seconds after it was taken. */
static inline f32 pal_pick(f32 t, bool two_line) {
    if (t <= 0.0f) return 0.0f;
    return pal_out_cubic(t / (f32)(two_line ? PAL_PICK2_SEC : PAL_PICK_SEC));
}

/* The caret: solid while keys are arriving, blinking once they stop. `since`
 * is seconds since the last key (or since the palette opened). */
static inline bool pal_caret_on(f64 since) {
    if (since < PAL_CARET_HOLD_SEC) return true;
    return ((i64)(since / PAL_CARET_HALF_SEC) % 2) == 0;
}

/* The reference opacities are display-space: black at 32% over a pixel
 * leaves 68% of its VALUE. This renderer blends in linear light, where the
 * same alpha leaves 68% of its light — a much fainter dim. This is the alpha
 * that puts the reference pixel on the screen, and for BLACK it does not
 * depend on what is underneath: (1 - a_linear) = (1 - a_display)^2.2. */
static inline f32 pal_linear_alpha(f32 a_display) {
    a_display = pal_clamp01(a_display);
    return 1.0f - powf(1.0f - a_display, 2.2f);
}

#endif /* UI_PALETTE_LAYOUT_H */
