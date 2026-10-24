/*
 * ctx_menu.h — where a context menu's box and rows are.
 *
 * Liu has four context menus (a tab's, a pane's, the explorer's, a tab
 * group's). They are "the same menu" to the eye only as long as nothing lets
 * them differ, so they share one placement here, one painter in ui.c
 * (ctx_menu_draw_rows) and one set of numbers in layout.h (CTX_MENU_*): a menu
 * says WHICH rows it has and which of them a hairline follows, and gets
 * everything else.
 *
 * Pure: no renderer, no AppState.
 */
#ifndef LIU_UI_CTX_MENU_H
#define LIU_UI_CTX_MENU_H

#include "core/types.h"
#include "ui/layout.h"
#include <string.h>

#define CTX_MENU_MAX_ROWS 12

typedef struct {
    f32  x, y, w, h;                        /* the panel, inside the window */
    f32  row_h;
    i32  count;
    f32  row_y[CTX_MENU_MAX_ROWS];          /* top of each row, window px */
    bool sep_after[CTX_MENU_MAX_ROWS];      /* a hairline in the gap under it */
} CtxMenuBox;

/* Place `count` rows with their top-left asked at (at_x, at_y), `w` wide,
 * kept inside a win_w x win_h window. `sep_after` may be NULL (no hairlines);
 * one after the LAST row is ignored — a hairline separates two rows.
 *
 * Clamp first, THEN put the rows on the final origin: the other order leaves
 * the rows where the menu was asked for and the box where it fits. */
static inline void ctx_menu_place(CtxMenuBox *b, i32 count, const bool *sep_after,
                                  f32 at_x, f32 at_y, f32 w,
                                  f32 win_w, f32 win_h, f32 dpi) {
    memset(b, 0, sizeof *b);
    if (count < 0) count = 0;
    if (count > CTX_MENU_MAX_ROWS) count = CTX_MENU_MAX_ROWS;
    f32 vpad = CTX_MENU_VPAD_PT * dpi, sep_h = CTX_MENU_SEP_H_PT * dpi;
    b->count = count;
    b->row_h = CTX_MENU_ROW_H_PT * dpi;
    b->w = w;
    f32 y = vpad;
    for (i32 i = 0; i < count; i++) {
        b->row_y[i] = y;                            /* panel-relative for now */
        y += b->row_h;
        if (sep_after && sep_after[i] && i + 1 < count) {
            b->sep_after[i] = true;
            y += sep_h;
        }
    }
    b->h = y + vpad;

    f32 edge = 4.0f * dpi;
    f32 x = at_x, top = at_y;
    if (x + b->w > win_w - edge)   x   = win_w - b->w - edge;
    if (top + b->h > win_h - edge) top = win_h - b->h - edge;
    if (x < edge)   x = edge;
    if (top < edge) top = edge;
    b->x = x; b->y = top;
    for (i32 i = 0; i < count; i++) b->row_y[i] += top;
}

static inline bool ctx_menu_contains(const CtxMenuBox *b, f32 px, f32 py) {
    return px >= b->x && px < b->x + b->w && py >= b->y && py < b->y + b->h;
}

/* The row under a point, or -1: outside, on the padding, in a hairline's gap. */
static inline i32 ctx_menu_row_at(const CtxMenuBox *b, f32 px, f32 py) {
    if (px < b->x || px >= b->x + b->w) return -1;
    for (i32 i = 0; i < b->count; i++)
        if (py >= b->row_y[i] && py < b->row_y[i] + b->row_h) return i;
    return -1;
}

/* The arrow keys: the next row that can act from `from` in direction `dir`
 * (+1 down, -1 up), wrapping. `from` outside the rows starts at the first (or
 * the last) one. `enabled` may be NULL — every row can act. -1 when none can.
 *
 * ...and what a pointer may do to the highlight afterwards (ctx_menu_pointer):
 * inside the menu the pointer owns it — the row under it, or none on a
 * hairline or the padding; outside it takes the highlight away ONLY if the
 * pointer put it there. A row chosen with the arrows must not vanish because
 * a hand resting on the mouse moved it a pixel somewhere else on the screen. */
static inline i32 ctx_menu_step(i32 count, const bool *enabled, i32 from, i32 dir) {
    if (count <= 0 || dir == 0) return -1;
    i32 i = from;
    if (i < 0 || i >= count) i = dir > 0 ? -1 : count;
    for (i32 n = 0; n < count; n++) {
        i += dir > 0 ? 1 : -1;
        if (i >= count) i = 0;
        if (i < 0) i = count - 1;
        if (!enabled || enabled[i]) return i;
    }
    return -1;
}

/* The highlight after a pointer move. `row` is the row under the pointer when
 * `inside` (a row that cannot act counts as none — pass -1), `by_key` whether
 * the current highlight was put there by the arrows; it is updated. */
static inline i32 ctx_menu_pointer(i32 selected, bool inside, i32 row, bool *by_key) {
    if (inside) { *by_key = false; return row; }
    return *by_key ? selected : -1;
}

/* Where the hairline under row `i` is drawn: centred in the gap, on a whole
 * pixel, at least one pixel thick. */
static inline void ctx_menu_sep_rect(const CtxMenuBox *b, i32 i, f32 dpi,
                                     f32 *x, f32 *y, f32 *w, f32 *h) {
    f32 thick = dpi > 1.0f ? dpi : 1.0f;
    f32 gap_top = b->row_y[i] + b->row_h;
    *x = b->x + 8.0f * dpi;
    *w = b->w - 16.0f * dpi;
    *y = (f32)(i32)(gap_top + (CTX_MENU_SEP_H_PT * dpi - 1.0f * dpi) * 0.5f);
    *h = thick;
}

/* The highlighted row's bar: inset from the panel's edge, rounded. */
static inline void ctx_menu_highlight_rect(const CtxMenuBox *b, i32 i, f32 dpi,
                                           f32 *x, f32 *y, f32 *w, f32 *h) {
    *x = b->x + CTX_MENU_HL_INSET_PT * dpi;
    *w = b->w - 2.0f * CTX_MENU_HL_INSET_PT * dpi;
    *y = b->row_y[i];
    *h = b->row_h;
}

/* Left edge of a row's label: past the icon rail, whether or not this row
 * has an icon — labels line up down the menu. */
static inline f32 ctx_menu_label_x(const CtxMenuBox *b, f32 dpi) {
    return b->x + (CTX_MENU_ICON_X_PT + CTX_MENU_ICON_PT + CTX_MENU_LABEL_GAP_PT) * dpi;
}

/* The width one row needs: rail, label, and — when it has keys — a readable
 * gap and the keys. `keys_w` is the drawn width of the key row (0 = none). */
static inline f32 ctx_menu_row_width(i32 label_cols, f32 keys_w, f32 ui_cw, f32 dpi) {
    f32 w = (CTX_MENU_ICON_X_PT + CTX_MENU_ICON_PT + CTX_MENU_LABEL_GAP_PT) * dpi
          + (f32)label_cols * ui_cw + CTX_MENU_PAD_R_PT * dpi;
    if (keys_w > 0.0f) w += CTX_MENU_KEYS_GAP_PT * dpi + keys_w;
    return w;
}

#endif /* LIU_UI_CTX_MENU_H */
