/*
 * Liu — the "Customize sidebar" popover, as arithmetic.
 *
 * The popover is the sidebar's own settings surface: where the tabs live (top
 * strip or sidebar), which sections show and in what order (a switch per row,
 * the rows drag to reorder), the row density, the bar's width and whether it
 * folds to its icon rail below a window width. Two places have to agree on
 * every control's rect — render_sidebar_customize paints them and the event
 * half (ui/sidebar_customize.c) routes the press, the drag and the release by
 * them — so, as with ui/sidebar_layout.h, the geometry is derived HERE from
 * primitive inputs and nowhere else. Nothing in this header knows what an
 * AppState is, so it works without a window.
 *
 * Anchoring: the panel hangs off the sidebar's right edge with its bottom on
 * the bar's bottom edge, clamped inside the window — it is opened from Cmd+K
 * ("Customize Sidebar") or Settings › Terminal, never from the bar itself.
 * With no sidebar to hang off it is centred.
 */
#ifndef UI_SIDEBAR_CUSTOMIZE_H
#define UI_SIDEBAR_CUSTOMIZE_H

#include "core/types.h"
#include "ui/sidebar_layout.h"

#include <math.h>

/* Points. The painter and the event half both come here. */
#define SBC_PANEL_W_PT        420.0f
#define SBC_HDR_PT             44.0f
#define SBC_PAD_X_PT           16.0f
#define SBC_PAD_Y_PT           14.0f
#define SBC_LABEL_PT           18.0f   /* the tracked-caps band above a control */
#define SBC_LABEL_GAP_PT        6.0f
#define SBC_SEG_PT             30.0f   /* segmented: 26pt buttons inside a 2pt inset */
#define SBC_SEG_INSET_PT        2.0f
#define SBC_GROUP_GAP_PT       14.0f
#define SBC_ROW_PT             40.0f   /* one section row */
#define SBC_ROW_GAP_PT          4.0f
#define SBC_COLLAPSE_PT        34.0f
#define SBC_FOOTER_PT          52.0f
#define SBC_BTN_PT             28.0f
#define SBC_SWITCH_W_PT        30.0f
#define SBC_SWITCH_H_PT        18.0f
#define SBC_SLIDER_PT          26.0f   /* the track row's height; the knob is 14pt */
#define SBC_KNOB_PT            14.0f
#define SBC_CLOSE_PT           26.0f
#define SBC_GRIP_PT            22.0f   /* the grip column at a row's left edge */
#define SBC_ANCHOR_GAP_PT      10.0f
#define SBC_MARGIN_PT          12.0f
#define SBC_DRAG_SLOP_PT        4.0f   /* a press moves this far before it is a drag */
#define SBC_RADIUS_PT          12.0f

typedef struct { f32 x, y, w, h; } SbcRect;

typedef struct {
    f32  win_w, win_h;        /* framebuffer px */
    f32  dpi;
    f32  top_min;             /* the panel never rises above this y (below the toolbar) */
    f32  anchor_x;            /* the sidebar's right edge; < 0 → centre the panel */
    f32  anchor_bottom;       /* y the panel's bottom edge aligns to (the bar's bottom) */
    i32  rows;                /* section rows (SB_SEC_COUNT) */
} SbcIn;

typedef struct {
    SbcRect panel, header, close;
    SbcRect tabs_label, tabs_seg;         /* Top bar | Sidebar */
    SbcRect rows_label, rows_area;        /* the section list */
    f32     row_h, row_gap, grip_w;
    i32     rows;
    SbcRect density_label, density_seg;   /* Comfortable | Compact */
    SbcRect width_label, width_slider;    /* the track row */
    SbcRect collapse;                     /* the whole row toggles */
    SbcRect footer, reset_btn, done_btn;
    f32     switch_w, switch_h;
} SbcLayout;

static inline bool sbc_rect_has(SbcRect r, f32 px, f32 py) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

/* Natural height of the whole panel, px. Every band is fixed except the
 * section list, which is rows × row plus the gaps between them. */
static inline f32 sbc_panel_height(i32 rows, f32 dpi) {
    if (rows < 1) rows = 1;
    f32 list = (f32)rows * SBC_ROW_PT + (f32)(rows - 1) * SBC_ROW_GAP_PT;
    f32 group = SBC_LABEL_PT + SBC_LABEL_GAP_PT;
    f32 pt = SBC_HDR_PT
           + SBC_PAD_Y_PT
           + group + SBC_SEG_PT                 /* tabs */
           + SBC_GROUP_GAP_PT
           + group + list                       /* sections */
           + SBC_GROUP_GAP_PT
           + group + SBC_SEG_PT                 /* density | width */
           + SBC_GROUP_GAP_PT
           + SBC_COLLAPSE_PT
           + SBC_PAD_Y_PT
           + SBC_FOOTER_PT;
    return pt * dpi;
}

static inline void sbc_layout_compute(const SbcIn *in, SbcLayout *L) {
    f32 dpi = in->dpi;
    f32 w = SBC_PANEL_W_PT * dpi;
    f32 h = sbc_panel_height(in->rows, dpi);
    f32 margin = SBC_MARGIN_PT * dpi;
    if (w > in->win_w - 2.0f * margin) w = in->win_w - 2.0f * margin;
    if (w < 200.0f * dpi) w = 200.0f * dpi;

    f32 x, y;
    if (in->anchor_x < 0.0f) {
        x = floorf((in->win_w - w) * 0.5f);
        y = floorf((in->win_h - h) * 0.5f);
    } else {
        x = in->anchor_x + SBC_ANCHOR_GAP_PT * dpi;
        y = in->anchor_bottom - h;
    }
    /* Clamp inside the window: right edge first, then the left margin wins
     * (a window narrower than the panel shows its left side). Vertically the
     * toolbar's floor wins over the anchor — a short window clips the bottom
     * rather than covering the toolbar. */
    if (x + w > in->win_w - margin) x = in->win_w - margin - w;
    if (x < margin) x = margin;
    if (y + h > in->win_h - margin) y = in->win_h - margin - h;
    if (y < in->top_min) y = in->top_min;
    x = floorf(x); y = floorf(y);

    L->panel = (SbcRect){ x, y, w, h };
    L->header = (SbcRect){ x, y, w, SBC_HDR_PT * dpi };
    f32 cl = SBC_CLOSE_PT * dpi;
    L->close = (SbcRect){ x + w - 10.0f * dpi - cl, y + floorf((SBC_HDR_PT * dpi - cl) * 0.5f), cl, cl };

    f32 px = SBC_PAD_X_PT * dpi;
    f32 cx = x + px, cw = w - 2.0f * px;
    f32 cy = y + SBC_HDR_PT * dpi + SBC_PAD_Y_PT * dpi;
    f32 lab = SBC_LABEL_PT * dpi, lgap = SBC_LABEL_GAP_PT * dpi;
    f32 ggap = SBC_GROUP_GAP_PT * dpi;

    L->tabs_label = (SbcRect){ cx, cy, cw, lab };            cy += lab + lgap;
    L->tabs_seg   = (SbcRect){ cx, cy, cw, SBC_SEG_PT * dpi }; cy += SBC_SEG_PT * dpi + ggap;

    L->rows = in->rows < 1 ? 1 : in->rows;
    L->row_h = SBC_ROW_PT * dpi;
    L->row_gap = SBC_ROW_GAP_PT * dpi;
    L->grip_w = SBC_GRIP_PT * dpi;
    L->rows_label = (SbcRect){ cx, cy, cw, lab };            cy += lab + lgap;
    f32 list_h = (f32)L->rows * L->row_h + (f32)(L->rows - 1) * L->row_gap;
    L->rows_area = (SbcRect){ cx, cy, cw, list_h };          cy += list_h + ggap;

    /* Density and width side by side, one gap between them. */
    f32 col_w = floorf((cw - ggap) * 0.5f);
    L->density_label = (SbcRect){ cx, cy, col_w, lab };
    L->width_label   = (SbcRect){ cx + col_w + ggap, cy, cw - col_w - ggap, lab };
    cy += lab + lgap;
    L->density_seg   = (SbcRect){ cx, cy, col_w, SBC_SEG_PT * dpi };
    L->width_slider  = (SbcRect){ cx + col_w + ggap, cy + floorf((SBC_SEG_PT - SBC_SLIDER_PT) * 0.5f * dpi),
                                  cw - col_w - ggap, SBC_SLIDER_PT * dpi };
    cy += SBC_SEG_PT * dpi + ggap;

    L->collapse = (SbcRect){ cx, cy, cw, SBC_COLLAPSE_PT * dpi };

    L->switch_w = SBC_SWITCH_W_PT * dpi;
    L->switch_h = SBC_SWITCH_H_PT * dpi;

    f32 fh = SBC_FOOTER_PT * dpi;
    L->footer = (SbcRect){ x, y + h - fh, w, fh };
    f32 bh = SBC_BTN_PT * dpi;
    f32 by = L->footer.y + floorf((fh - bh) * 0.5f);
    /* Button widths in points; the label is centred inside. */
    L->reset_btn = (SbcRect){ cx, by, 138.0f * dpi, bh };
    L->done_btn  = (SbcRect){ cx + cw - 76.0f * dpi, by, 76.0f * dpi, bh };
}

/* Row `slot` (0-based, top to bottom) of the section list. */
static inline SbcRect sbc_row_rect(const SbcLayout *L, i32 slot) {
    return (SbcRect){ L->rows_area.x, L->rows_area.y + (f32)slot * (L->row_h + L->row_gap),
                      L->rows_area.w, L->row_h };
}

/* A switch sits flush right inside its row, vertically centred, with the
 * row's 10pt right padding. Shared by the section rows and the collapse row. */
static inline SbcRect sbc_switch_in(const SbcLayout *L, SbcRect row, f32 dpi) {
    return (SbcRect){ row.x + row.w - 10.0f * dpi - L->switch_w,
                      row.y + floorf((row.h - L->switch_h) * 0.5f),
                      L->switch_w, L->switch_h };
}

/* The slot a dragged row's TOP edge is nearest to, clamped to the list. */
static inline i32 sbc_slot_for_top(const SbcLayout *L, f32 top_y) {
    f32 pitch = L->row_h + L->row_gap;
    i32 slot = (i32)floorf((top_y - L->rows_area.y) / pitch + 0.5f);
    if (slot < 0) slot = 0;
    if (slot > L->rows - 1) slot = L->rows - 1;
    return slot;
}

/* Move order[from] to position `to`, shifting the rows between. */
static inline void sbc_order_move(u8 *order, i32 n, i32 from, i32 to) {
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    u8 v = order[from];
    if (from < to) { for (i32 i = from; i < to; i++) order[i] = order[i + 1]; }
    else           { for (i32 i = from; i > to; i--) order[i] = order[i - 1]; }
    order[to] = v;
}

/* The width slider: 0..1 ↔ SIDEBAR_MIN_PT..SIDEBAR_MAX_PT, whole points. The
 * knob has the same radius the drawing uses, so the value under the pointer
 * is the value drawn there. */
#define SBC_WIDTH_MIN_PT 160.0f
#define SBC_WIDTH_MAX_PT 500.0f

static inline f32 sbc_slider_t_at(SbcRect track, f32 px, f32 dpi) {
    f32 kr = SBC_KNOB_PT * 0.5f * dpi;
    f32 span = track.w - 2.0f * kr;
    if (span <= 0.0f) return 0.0f;
    f32 t = (px - track.x - kr) / span;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

static inline f32 sbc_width_pt_from_t(f32 t) {
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return floorf(SBC_WIDTH_MIN_PT + t * (SBC_WIDTH_MAX_PT - SBC_WIDTH_MIN_PT) + 0.5f);
}

static inline f32 sbc_t_from_width_pt(f32 pt) {
    f32 t = (pt - SBC_WIDTH_MIN_PT) / (SBC_WIDTH_MAX_PT - SBC_WIDTH_MIN_PT);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

typedef enum {
    SBC_HIT_OUTSIDE = 0,     /* not on the panel: dismiss */
    SBC_HIT_PANEL,           /* on the panel, on nothing in particular */
    SBC_HIT_CLOSE,
    SBC_HIT_TABS_SEG,        /* index: 0 top bar, 1 sidebar */
    SBC_HIT_ROW,             /* index: slot; the row body (a click toggles, a drag reorders) */
    SBC_HIT_ROW_GRIP,        /* index: slot; the grip (drag only) */
    SBC_HIT_ROW_SWITCH,      /* index: slot */
    SBC_HIT_DENSITY_SEG,     /* index: 0 comfortable, 1 compact */
    SBC_HIT_WIDTH,           /* t: 0..1 along the track */
    SBC_HIT_COLLAPSE,
    SBC_HIT_RESET,
    SBC_HIT_DONE,
} SbcHitKind;

typedef struct { SbcHitKind kind; i32 index; f32 t; } SbcHit;

static inline SbcHit sbc_hit(const SbcLayout *L, f32 px, f32 py, f32 dpi) {
    SbcHit h = { SBC_HIT_OUTSIDE, -1, 0.0f };
    if (!sbc_rect_has(L->panel, px, py)) return h;
    h.kind = SBC_HIT_PANEL;
    if (sbc_rect_has(L->close, px, py)) { h.kind = SBC_HIT_CLOSE; return h; }
    if (sbc_rect_has(L->tabs_seg, px, py)) {
        h.kind = SBC_HIT_TABS_SEG;
        h.index = (px - L->tabs_seg.x) < L->tabs_seg.w * 0.5f ? 0 : 1;
        return h;
    }
    if (sbc_rect_has(L->rows_area, px, py)) {
        for (i32 s = 0; s < L->rows; s++) {
            SbcRect row = sbc_row_rect(L, s);
            if (!sbc_rect_has(row, px, py)) continue;
            h.index = s;
            SbcRect sw = sbc_switch_in(L, row, dpi);
            /* The switch's hit box is the row's full height and reaches the
             * row's right edge: 30×18 is a small target. */
            if (px >= sw.x - 6.0f * dpi) h.kind = SBC_HIT_ROW_SWITCH;
            else if (px < row.x + L->grip_w) h.kind = SBC_HIT_ROW_GRIP;
            else h.kind = SBC_HIT_ROW;
            return h;
        }
        return h;   /* the gap between rows */
    }
    if (sbc_rect_has(L->density_seg, px, py)) {
        h.kind = SBC_HIT_DENSITY_SEG;
        h.index = (px - L->density_seg.x) < L->density_seg.w * 0.5f ? 0 : 1;
        return h;
    }
    /* The slider answers for its whole 30pt band (the track row is thinner). */
    {
        SbcRect band = L->width_slider;
        band.y = L->density_seg.y; band.h = L->density_seg.h;
        if (sbc_rect_has(band, px, py)) {
            h.kind = SBC_HIT_WIDTH;
            h.t = sbc_slider_t_at(L->width_slider, px, dpi);
            return h;
        }
    }
    if (sbc_rect_has(L->collapse, px, py)) { h.kind = SBC_HIT_COLLAPSE; return h; }
    if (sbc_rect_has(L->reset_btn, px, py)) { h.kind = SBC_HIT_RESET; return h; }
    if (sbc_rect_has(L->done_btn, px, py))  { h.kind = SBC_HIT_DONE; return h; }
    return h;
}

/* ------------------------------------------------------------------------
 * The AppState-facing half. Layout fill lives in ui/sidebar_sections.c (pure,
 * no renderer); open/close and the event routing in ui/sidebar_customize.c
 * (Liu only — it saves config and relayouts).
 * ------------------------------------------------------------------------ */
struct AppState;
void app_sbc_layout(const struct AppState *app, SbcLayout *out);
/* Freeze the anchor to the bar's CURRENT edge (the painter calls it on the
 * popover's first frame). Until it is called, app_sbc_layout follows the live
 * bar. */
void app_sbc_anchor_capture(struct AppState *app);

void sbc_open(struct AppState *app);
void sbc_close(struct AppState *app);
void sbc_toggle(struct AppState *app);
/* Each returns whether the event was consumed. Coordinates are framebuffer px. */
bool sbc_key(struct AppState *app, i32 key, u32 mods);
bool sbc_mouse_down(struct AppState *app, f32 x, f32 y);
bool sbc_mouse_move(struct AppState *app, f32 x, f32 y);
bool sbc_mouse_up(struct AppState *app, f32 x, f32 y);
bool sbc_scroll(struct AppState *app, f32 x, f32 y);
/* Reset every sidebar setting to its default (the footer's button). */
void sbc_reset_defaults(struct AppState *app);

#endif /* UI_SIDEBAR_CUSTOMIZE_H */
