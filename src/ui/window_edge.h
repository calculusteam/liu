#ifndef LIU_WINDOW_EDGE_H
#define LIU_WINDOW_EDGE_H

/* The edge of a floating window, drawn by Liu.
 *
 * On X11 Liu's windows carry no window-manager frame (the tab strip is the
 * title bar), and compositing window managers draw no border or shadow for a
 * window that has none, so two Liu windows laid over each other show no line between them.
 * A floating window gets a one-device-pixel outline in the theme's divider
 * colour instead, painted over the outermost ring of the window; nothing is
 * inset, so every pane, hit test and band keeps its place. A maximized or
 * fullscreen window fills the screen and has no edge to show.
 *
 * Pure: the decision and the geometry need no display. Drawn by
 * render_window_edge in ui.c (X11 only). */

#include "core/types.h"

#include <math.h>

typedef struct { f32 x, y, w, h; } WindowEdgeRect;

/* The outline's thickness in device pixels: one point, never under one pixel. */
static inline f32 window_edge_line_px(f32 dpi_scale) {
    f32 l = roundf(dpi_scale);
    return l < 1.0f ? 1.0f : l;
}

/* Does the window get an edge? Only where Liu draws the window's frame, and
 * only while the window is not maximized or fullscreen. A window too small to
 * hold two lines gets none. */
static inline bool window_edge_wanted(bool own_frame, bool maximized, bool fullscreen,
                                      f32 fb_w, f32 fb_h, f32 line) {
    if (!own_frame || maximized || fullscreen) return false;
    return fb_w >= 4.0f * line && fb_h >= 4.0f * line;
}

/* The four strips of the outline, none overlapping another (a translucent
 * colour is blended once per pixel): the top and bottom rows across the whole
 * width, the left and right columns between them. Returns 4. */
static inline i32 window_edge_rects(f32 fb_w, f32 fb_h, f32 line, WindowEdgeRect out[4]) {
    out[0] = (WindowEdgeRect){ 0.0f, 0.0f, fb_w, line };
    out[1] = (WindowEdgeRect){ 0.0f, fb_h - line, fb_w, line };
    out[2] = (WindowEdgeRect){ 0.0f, line, line, fb_h - 2.0f * line };
    out[3] = (WindowEdgeRect){ fb_w - line, line, line, fb_h - 2.0f * line };
    return 4;
}

#endif
