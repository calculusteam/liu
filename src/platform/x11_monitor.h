#ifndef LIU_X11_MONITOR_H
#define LIU_X11_MONITOR_H

/* Which monitor of an X screen a point is on, and what a window may use of it.
 *
 * X11 has one root window for every monitor: RandR lists the monitors as
 * rectangles of it, and the window manager publishes ONE work area for the
 * whole desktop (_NET_WORKAREA: the screen less every panel's strut). The
 * visible frame of a monitor is that work area clipped to the monitor. Pure:
 * no X server is needed. */

#include "core/types.h"

typedef struct { i32 x, y, w, h; } X11Rect;

/* The monitor holding (px, py). A point on none of them — the gap of an
 * L-shaped layout, past the screen's edge where a window may hang — belongs
 * to the NEAREST one: falling back to the whole screen there made a window
 * half off monitor 2 snap and settle against both monitors at once.
 * -1 when there are no monitors. */
static inline i32 x11_monitor_at(const X11Rect *mon, i32 n, i32 px, i32 py) {
    i32 best = -1;
    i64 best_d = 0;
    for (i32 i = 0; mon && i < n; i++) {
        if (mon[i].w <= 0 || mon[i].h <= 0) continue;
        i64 dx = px < mon[i].x ? (i64)mon[i].x - px
               : px >= mon[i].x + mon[i].w ? (i64)px - (mon[i].x + mon[i].w - 1) : 0;
        i64 dy = py < mon[i].y ? (i64)mon[i].y - py
               : py >= mon[i].y + mon[i].h ? (i64)py - (mon[i].y + mon[i].h - 1) : 0;
        i64 d = dx * dx + dy * dy;
        if (d == 0) return i;
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    return best;
}

/* The desktop's work area clipped to monitor `m`; the whole monitor when the
 * two do not meet (a work area that only describes another monitor). */
static inline X11Rect x11_visible_frame(X11Rect m, X11Rect work) {
    i32 x0 = m.x > work.x ? m.x : work.x;
    i32 y0 = m.y > work.y ? m.y : work.y;
    i32 x1 = (m.x + m.w) < (work.x + work.w) ? (m.x + m.w) : (work.x + work.w);
    i32 y1 = (m.y + m.h) < (work.y + work.h) ? (m.y + m.h) : (work.y + work.h);
    if (x1 <= x0 || y1 <= y0) return m;
    return (X11Rect){ x0, y0, x1 - x0, y1 - y0 };
}

#endif
