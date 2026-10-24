#ifndef LIU_TAB_FLOAT_H
#define LIU_TAB_FLOAT_H

/* A tab that leaves its strip travels as a WINDOW.
 *
 * Dragging a tab out of its window turns it into a window on the spot — while
 * the button is still down — and that window follows the pointer until the
 * release: dropped on a tab strip (the one it came from included) it is a tab
 * again, dropped anywhere else it stays a window. A window that holds ONE tab
 * is that tab: dragging the tab moves the window, so the same gesture puts it
 * back. main.c owns the windows; this header is the arithmetic those decisions
 * are made with, kept pure so it works without one.
 *
 * Units: `*_pt` are points, everything else is whatever the caller measures in
 * (framebuffer pixels for window-local values). Screen space is the one
 * platform_window_get_position() reports: origin at the top-left of the main
 * display, y down. */

#include "core/types.h"

/* How far outside its window the pointer must be before the tab lets go.
 *
 * The top is the short one: the strip sits on the window's top edge, so "up" is
 * the natural way out and the pointer has the least room there — a window parked
 * under the menu bar leaves 24 pt of it on a plain display, too little for a 30
 * pt margin. The sides and the bottom keep a wider margin because a reorder or a
 * split-drop sweeps along those edges on its way somewhere else. */
#define TAB_FLOAT_MARGIN_TOP_PT   14.0f
#define TAB_FLOAT_MARGIN_SIDE_PT  24.0f

/* The press has to travel this far before it is a drag. Measured in BOTH axes,
 * so a tab pulled straight up (to tear it off) or straight down (to split)
 * starts dragging without the hand first wandering sideways. */
#define TAB_DRAG_THRESHOLD_PT      5.0f

/* How much of a dropped window must stay reachable. */
#define TAB_FLOAT_KEEP_PT         96.0f

/* The float window's alpha while a strip under it would take the drop: it sits
 * exactly over the seam the tab is about to land in, and the caret is drawn on
 * the window BELOW it. Low on purpose — the window is about to stop being one,
 * and what has to read at that moment is the strip it is dissolving into. */
#define TAB_FLOAT_OVER_ALPHA       0.30f

static inline bool tab_drag_started(f32 dx, f32 dy, f32 threshold) {
    return dx * dx + dy * dy > threshold * threshold;
}

/* Has the pointer — in window-local units, the window being w x h — left the
 * window by the tear-off margins? `scale` converts the margins from points. */
static inline bool tab_float_left_window(f32 x, f32 y, f32 w, f32 h, f32 scale) {
    f32 top  = TAB_FLOAT_MARGIN_TOP_PT  * scale;
    f32 side = TAB_FLOAT_MARGIN_SIDE_PT * scale;
    return y < -top || x < -side || x > w + side || y > h + side;
}

/* Top-left of the window that keeps `grab` under the pointer. */
static inline void tab_float_origin(f32 sx, f32 sy, f32 grab_x, f32 grab_y,
                                    f32 *ox, f32 *oy) {
    if (ox) *ox = sx - grab_x;
    if (oy) *oy = sy - grab_y;
}

/* Where inside a NEW window the pointer should rest: on its first pill, at the
 * same spot of the pill the user is holding (`in_pill_pt` from the pill's left
 * edge — the dragged ghost keeps that offset too, so the pill does not shift
 * under the hand when the tab turns into a window). `strip_x_pt` is the strip's
 * left origin (past the window buttons), `strip_h_pt` its height. The grab
 * never exceeds the window. */
static inline void tab_float_grab(f32 strip_x_pt, f32 pill_w_pt, f32 in_pill_pt,
                                  f32 strip_h_pt, f32 win_w_pt,
                                  f32 *grab_x, f32 *grab_y) {
    if (in_pill_pt < 0.0f) in_pill_pt = 0.0f;
    if (in_pill_pt > pill_w_pt) in_pill_pt = pill_w_pt;
    f32 gx = strip_x_pt + in_pill_pt;
    if (win_w_pt > 0.0f && gx > win_w_pt - 8.0f) gx = win_w_pt - 8.0f;
    if (gx < 8.0f) gx = 8.0f;
    if (grab_x) *grab_x = gx;
    if (grab_y) *grab_y = strip_h_pt > 0.0f ? strip_h_pt * 0.5f : 0.0f;
}

/* Screen units per point. The screen is in points on macOS and in pixels on
 * X11, where a window's size is in pixels too; a window's framebuffer is in
 * pixels on both. So its size over its framebuffer's, times the scale, says
 * which: 1 on macOS, the scale on X11. The helpers above take points; a value
 * in screen units is divided by this on the way in and multiplied on the way
 * out. Taken as points, an X11 window at scale 2 would be handed half the
 * offsets, and a torn-off tab would come out with the pointer halfway between
 * the window's edge and the pill it was holding. */
static inline f32 tab_float_screen_per_pt(f32 win_w, f32 fb_w, f32 scale) {
    if (!(win_w > 0.0f) || !(fb_w > 0.0f) || !(scale > 0.0f)) return 1.0f;
    return scale * win_w / fb_w;
}

/* tab_float_grab for a strip measured in framebuffer pixels (`*_px`, as the
 * layout keeps it), answered in screen units: `win_w` is the window's width in
 * screen units, `fb_w` its framebuffer's, `scale` its pixels per point. */
static inline void tab_float_grab_px(f32 strip_x_px, f32 pill_w_px, f32 in_pill_px,
                                     f32 strip_h_pt, f32 win_w, f32 fb_w, f32 scale,
                                     f32 *grab_x, f32 *grab_y) {
    f32 u = tab_float_screen_per_pt(win_w, fb_w, scale);
    f32 s = scale > 0.0f ? scale : 1.0f;
    f32 gx = 0.0f, gy = 0.0f;
    tab_float_grab(strip_x_px / s, pill_w_px / s, in_pill_px / s, strip_h_pt,
                   win_w / u, &gx, &gy);
    if (grab_x) *grab_x = gx * u;
    if (grab_y) *grab_y = gy * u;
}

/* While it travels, a window's strip may not slide up under the menu bar: the
 * top of the visible frame is as high as it goes. Nothing else is clamped
 * mid-drag — a window half off the side of the screen is an ordinary thing to
 * pass through on the way somewhere. */
static inline f32 tab_float_clamp_top(f32 oy, f32 vis_y) {
    return oy < vis_y ? vis_y : oy;
}

/* Where a window may REST. A tab torn off by pulling it down or sideways opens
 * at the pointer, which near an edge would put the new window almost entirely
 * off the screen — a strip peeking in at the bottom of the display, or nothing
 * at all. The strip has to stay on screen (it is the only handle the window
 * has) and so does a grabbable width. `vis_*` is the visible frame of the
 * screen the pointer is on. */
static inline void tab_float_settle(f32 *ox, f32 *oy, f32 win_w, f32 strip_h,
                                    f32 vis_x, f32 vis_y, f32 vis_w, f32 vis_h) {
    if (!ox || !oy || vis_w <= 0.0f || vis_h <= 0.0f) return;
    f32 keep = TAB_FLOAT_KEEP_PT;
    if (keep > win_w) keep = win_w;
    f32 min_x = vis_x - win_w + keep;
    f32 max_x = vis_x + vis_w - keep;
    if (*ox < min_x) *ox = min_x;
    if (*ox > max_x) *ox = max_x;
    f32 max_y = vis_y + vis_h - strip_h;
    if (*oy > max_y) *oy = max_y;
    if (*oy < vis_y) *oy = vis_y;      /* last: the top edge wins on a tiny screen */
}

/* tab_float_settle in screen units, `u` of them to the point
 * (tab_float_screen_per_pt): the strip and the grabbable width it keeps on
 * screen are points. */
static inline void tab_float_settle_screen(f32 *ox, f32 *oy, f32 win_w, f32 strip_h_pt, f32 u,
                                           f32 vis_x, f32 vis_y, f32 vis_w, f32 vis_h) {
    if (!ox || !oy) return;
    if (!(u > 0.0f)) u = 1.0f;
    f32 px = *ox / u, py = *oy / u;
    tab_float_settle(&px, &py, win_w / u, strip_h_pt, vis_x / u, vis_y / u, vis_w / u, vis_h / u);
    *ox = px * u;
    *oy = py * u;
}

#endif
