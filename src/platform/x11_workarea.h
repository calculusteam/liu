#ifndef LIU_X11_WORKAREA_H
#define LIU_X11_WORKAREA_H

/* The work area of ONE monitor, when the desktop publishes one.
 *
 * EWMH gives a single _NET_WORKAREA for the whole screen: the screen less
 * every panel's strut. GNOME's top bar sits on the primary monitor only, so
 * that one rectangle starts 32 px down for the other monitors too, and a window
 * snapped on the second monitor would leave a band of it unused that the window
 * manager's own maximize fills. mutter also publishes the areas per monitor, on
 * the root window, as _GTK_WORKAREAS_D<desktop>: a CARDINAL[4 * monitors] of
 * x, y, width, height quads in root coordinates, one per monitor, the panels
 * of THAT monitor already taken off (for example 0,32,1600,968 for the monitor
 * with the bar and 1600,0,1600,1000 for the other). The order is the window
 * manager's, not RandR's, so a monitor is matched to the quad that covers most
 * of it.
 *
 * Pure: the property's items are plain numbers, so the choice needs no server. */

#include <stdio.h>
#include "platform/x11_monitor.h"

#define X11_WORKAREAS_MAX 16

/* The name of the property for virtual desktop `desktop`
 * (_NET_CURRENT_DESKTOP). False when it does not fit. */
static inline bool x11_gtk_workareas_name(unsigned long desktop, char *out, usize cap) {
    if (!out || cap == 0) return false;
    int n = snprintf(out, cap, "_GTK_WORKAREAS_D%lu", desktop);
    return n > 0 && (usize)n < cap;
}

/* The quads of the property's `n_items` 32-bit items (an unsigned long each,
 * as Xlib hands format-32 data over). A short tail is ignored, a quad with no
 * area is skipped. Returns how many rectangles were written to `out`. */
static inline i32 x11_gtk_workareas_parse(const unsigned long *v, unsigned long n_items,
                                          X11Rect *out, i32 cap) {
    i32 n = 0;
    if (!v || !out) return 0;
    for (unsigned long i = 0; i + 3 < n_items && n < cap; i += 4) {
        i32 w = (i32)v[i + 2], h = (i32)v[i + 3];
        if (w <= 0 || h <= 0) continue;
        out[n++] = (X11Rect){ (i32)v[i], (i32)v[i + 1], w, h };
    }
    return n;
}

/* How much of `a` lies inside `b`, in pixels. */
static inline i64 x11_rect_overlap(X11Rect a, X11Rect b) {
    i64 x0 = a.x > b.x ? a.x : b.x;
    i64 y0 = a.y > b.y ? a.y : b.y;
    i64 x1 = ((i64)a.x + a.w) < ((i64)b.x + b.w) ? ((i64)a.x + a.w) : ((i64)b.x + b.w);
    i64 y1 = ((i64)a.y + a.h) < ((i64)b.y + b.h) ? ((i64)a.y + a.h) : ((i64)b.y + b.h);
    return (x1 > x0 && y1 > y0) ? (x1 - x0) * (y1 - y0) : 0;
}

/* What a window may use of monitor `m`: its own published area when the
 * desktop gave one (the quad covering most of it, clipped to it), else the
 * desktop-wide work area clipped to it (x11_visible_frame). */
static inline X11Rect x11_monitor_work(X11Rect m, const X11Rect *areas, i32 n,
                                       X11Rect desktop_wide) {
    i32 best = -1;
    i64 best_o = 0;
    for (i32 i = 0; areas && i < n; i++) {
        i64 o = x11_rect_overlap(areas[i], m);
        if (o > best_o) { best = i; best_o = o; }
    }
    return x11_visible_frame(m, best >= 0 ? areas[best] : desktop_wide);
}

#endif
