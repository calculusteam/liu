/*
 * pane_ghost.h — where a terminal's live picture lands inside a drop preview.
 *
 * A drop preview shows the terminal that is being moved, not a tinted box. It
 * is the pane's own retained GPU rows drawn a second time in the same frame,
 * at another origin and under a scissor (term_pane_ghost in ui.c) — so it
 * costs three draw calls over buffers that already exist, and it cannot be
 * stale: the frame that repaints the pane repaints its ghost from the same
 * bytes.
 *
 * That only works UNSCALED. The terminal atlas samples nearest and is exact
 * because every glyph origin is a whole pixel; a scaled ghost would need a
 * second raster or a filtered texture. So the ghost is the grid at 1:1 and the
 * question this header answers is the remaining one: the box is rarely the
 * pane's size, so WHICH part of the grid shows, and where is cell (0,0)?
 *
 *   - whole cells only (a half glyph at the edge reads as a rendering fault);
 *   - the cells are placed the way a pane of that box places its own grid —
 *     the leftover centred (term_grid_pad_x / _y) — so the picture sits in its
 *     frame as the text will sit in the pane;
 *   - when the grid is taller than the box the window follows the ANCHOR row —
 *     the cursor's on a shell, the last row on a full-screen program — because
 *     that is the part a real resize would keep on screen.
 *
 * Pure: no renderer, no Terminal.
 */
#ifndef LIU_UI_PANE_GHOST_H
#define LIU_UI_PANE_GHOST_H

#include "core/types.h"
#include "ui/layout.h"
#include <math.h>

typedef struct {
    f32 origin_x, origin_y;                /* where grid cell (0,0) is drawn */
    f32 clip_x, clip_y, clip_w, clip_h;    /* the cells on show; 0 = nothing */
    i32 first_row, rows, cols;             /* the grid window behind the clip */
} PaneGhostFit;

/* The row that must stay visible when the grid does not fit. */
static inline i32 pane_ghost_anchor_row(bool alt_screen, bool scrolled,
                                        i32 cursor_y, i32 term_rows) {
    if (term_rows <= 0) return 0;
    i32 row = (alt_screen || scrolled) ? term_rows - 1 : cursor_y;
    if (row < 0) row = 0;
    if (row > term_rows - 1) row = term_rows - 1;
    return row;
}

/* Fit a term_cols x term_rows grid of cw x ch cells into the box. */
static inline PaneGhostFit pane_ghost_fit(f32 box_x, f32 box_y, f32 box_w, f32 box_h,
                                          i32 term_cols, i32 term_rows, i32 anchor_row,
                                          f32 cw, f32 ch) {
    PaneGhostFit f = {0};
    if (term_cols <= 0 || term_rows <= 0 || !(cw >= 1.0f) || !(ch >= 1.0f)) return f;
    if (!(box_w > 0.0f) || !(box_h > 0.0f)) return f;
    /* Whole pixels inside the box: the grid is nearest-sampled. */
    f32 x0 = ceilf(box_x), y0 = ceilf(box_y);
    f32 w = floorf(box_x + box_w) - x0, h = floorf(box_y + box_h) - y0;
    if (w < cw || h < ch) return f;
    i32 box_cols = (i32)(w / cw), box_rows = (i32)(h / ch);
    f32 pad_x = term_grid_pad_x(w, box_cols, cw);
    f32 pad_y = term_grid_pad_y(h, box_rows, ch);
    f.cols = term_cols < box_cols ? term_cols : box_cols;
    f.rows = term_rows < box_rows ? term_rows : box_rows;
    if (term_rows > f.rows) {
        if (anchor_row < 0) anchor_row = 0;
        if (anchor_row > term_rows - 1) anchor_row = term_rows - 1;
        f.first_row = anchor_row - f.rows + 1;
        if (f.first_row < 0) f.first_row = 0;
    }
    f.clip_x = x0 + pad_x;
    f.clip_y = y0 + pad_y;
    f.clip_w = (f32)f.cols * cw;
    f.clip_h = (f32)f.rows * ch;
    f.origin_x = f.clip_x;
    f.origin_y = f.clip_y - (f32)f.first_row * ch;
    return f;
}

/* Carry a fit made for one box over to a box that is still on its way there
 * (an eased preview): the picture rides the moving box's top-left corner by
 * whole pixels and is cut to what the box has uncovered so far. Fitting the
 * moving box itself would re-centre and re-anchor the grid on every frame. */
static inline void pane_ghost_fit_carry(PaneGhostFit *f, f32 from_x, f32 from_y,
                                        f32 now_x, f32 now_y, f32 now_w, f32 now_h) {
    if (!f || f->clip_w <= 0.0f || f->clip_h <= 0.0f) return;
    f32 dx = roundf(now_x - from_x), dy = roundf(now_y - from_y);
    f->origin_x += dx; f->origin_y += dy;
    f->clip_x += dx;   f->clip_y += dy;
    f32 x0 = fmaxf(f->clip_x, ceilf(now_x)), y0 = fmaxf(f->clip_y, ceilf(now_y));
    f32 x1 = fminf(f->clip_x + f->clip_w, floorf(now_x + now_w));
    f32 y1 = fminf(f->clip_y + f->clip_h, floorf(now_y + now_h));
    if (x1 <= x0 || y1 <= y0) { f->clip_w = f->clip_h = 0.0f; return; }
    f->clip_x = x0; f->clip_y = y0; f->clip_w = x1 - x0; f->clip_h = y1 - y0;
}

#endif /* LIU_UI_PANE_GHOST_H */
