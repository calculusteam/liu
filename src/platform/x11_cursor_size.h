/*
 * Liu - the size of the pointer's cursor (X11)
 *
 * libX11 hands cursor creation (XCreateFontCursor) to libXcursor, which picks
 * the theme's image for ONE size per display and settles it the first time it
 * is asked, from the resources that were there when the display was opened.
 * A desktop whose Xft.dpi changes while Liu runs (a scale change in the
 * settings, a window carried to a screen of another density) would leave the
 * pointer at the old size over Liu, which is redrawn at the new scale.
 *
 * XcursorSetDefaultSize() moves it, and what it is to be set to is libXcursor's
 * own rule, applied to the resources as they are NOW:
 *
 *   1. $XCURSOR_SIZE
 *   2. the Xcursor.size resource
 *   3. Xft.dpi * 16 / 72 - or, where the desktop declares its scale some other
 *      way (x11_dpi_source.h), the dpi that scale implies
 *   4. the shorter side of the screen / 48
 *
 * Pure C: the order and the arithmetic need no server.
 */
#ifndef LIU_PLATFORM_X11_CURSOR_SIZE_H
#define LIU_PLATFORM_X11_CURSOR_SIZE_H

#include "core/types.h"
#include <stdlib.h>
#include <math.h>

#define X11_CURSOR_SIZE_MIN 8
#define X11_CURSOR_SIZE_MAX 256

/* A size as text: the leading digits, positive; 0 when there are none. */
static inline i32 x11_cursor_size_text(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return 0;
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > 100000) return 100000;
        s++;
    }
    return (i32)v;
}

/* The size libXcursor would choose. env_size and res_size are 0 when the
 * variable or resource is not there; dpi is 0 when nothing declares one. */
static inline i32 x11_cursor_size(i32 env_size, i32 res_size, double dpi,
                                  i32 screen_w, i32 screen_h) {
    i32 size = 0;
    if (env_size > 0) size = env_size;
    else if (res_size > 0) size = res_size;
    else {
        /* libXcursor reads the dpi as an integer */
        i64 d = (isfinite(dpi) && dpi >= 1.0) ? (i64)dpi : 0;
        if (d > 0) size = (i32)(d * 16 / 72);
        if (size == 0) {
            i32 dim = screen_h < screen_w ? screen_h : screen_w;
            size = dim > 0 ? dim / 48 : 0;
        }
    }
    if (size < X11_CURSOR_SIZE_MIN) size = X11_CURSOR_SIZE_MIN;
    if (size > X11_CURSOR_SIZE_MAX) size = X11_CURSOR_SIZE_MAX;
    return size;
}

#endif /* LIU_PLATFORM_X11_CURSOR_SIZE_H */
