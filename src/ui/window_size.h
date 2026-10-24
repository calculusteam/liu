#ifndef LIU_WINDOW_SIZE_H
#define LIU_WINDOW_SIZE_H

/* The size a window opens at when nothing gives it one.
 *
 * 1200x800 POINTS. A window's size is in screen units, which are points on
 * macOS and PIXELS on X11, so the caller passes the desktop's scale (pixels
 * per point: Xft.dpi / 96 there) and the visible frame in screen units, and
 * gets the size in screen units back. Kept inside 90% of the visible frame, so
 * a small screen gets a window that fits instead of one the window manager has
 * to cut down (or silently maximize) when it maps. Pure: it needs no display.
 */

#include "core/types.h"

#include <math.h>

#define WINDOW_DEFAULT_W_PT   1200.0f
#define WINDOW_DEFAULT_H_PT   800.0f
#define WINDOW_FIT_FRACTION   0.9f
/* Never smaller than a usable terminal, whatever the frame says. */
#define WINDOW_MIN_W          480
#define WINDOW_MIN_H          320

/* `vis_w`/`vis_h` <= 0: the frame is not known, the size is the points times
 * the scale. A scale below 1 is read as 1. */
static inline void window_default_size(f32 scale, f32 vis_w, f32 vis_h, i32 *w, i32 *h) {
    f32 s = scale >= 1.0f ? scale : 1.0f;
    f32 fw = WINDOW_DEFAULT_W_PT * s, fh = WINDOW_DEFAULT_H_PT * s;
    if (vis_w > 0.0f && vis_h > 0.0f) {
        f32 fit = fminf(1.0f, fminf(vis_w * WINDOW_FIT_FRACTION / fw,
                                    vis_h * WINDOW_FIT_FRACTION / fh));
        fw *= fit;
        fh *= fit;
    }
    i32 iw = (i32)lroundf(fw), ih = (i32)lroundf(fh);
    if (w) *w = iw < WINDOW_MIN_W ? WINDOW_MIN_W : iw;
    if (h) *h = ih < WINDOW_MIN_H ? WINDOW_MIN_H : ih;
}

#endif
