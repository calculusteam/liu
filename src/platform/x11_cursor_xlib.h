/*
 * Liu - libXcursor's default size (X11)
 *
 * libX11 makes a font cursor through libXcursor, which settles the theme's size
 * once per display from the resources of the moment the display was opened. The
 * library is the system's own - libX11 loads it itself for the same call - and
 * is opened here only to ask for and set that default size; without it the
 * cursors are the core font's and there is nothing to resize. Included once, by
 * platform_linux.c, after the X11 headers.
 */
#ifndef LIU_PLATFORM_X11_CURSOR_XLIB_H
#define LIU_PLATFORM_X11_CURSOR_XLIB_H

#include "core/types.h"
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

/* Bring the default size to `want`; true when that moved it, false when it was
 * there already or the library is not. */
static inline bool x11x_cursor_default_size(Display *dpy, i32 want) {
    static bool tried = false;
    static int (*get_size)(Display *);
    static int (*set_size)(Display *, int);
    if (!tried) {
        tried = true;
        void *lib = dlopen("libXcursor.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (lib) {
            *(void **)(&get_size) = dlsym(lib, "XcursorGetDefaultSize");
            *(void **)(&set_size) = dlsym(lib, "XcursorSetDefaultSize");
        }
    }
    if (!dpy || !get_size || !set_size) return false;
    i32 have = get_size(dpy);
    const char *probe = getenv("LIU_DPI_PROBE");
    if (probe && *probe && *probe != '0')
        fprintf(stderr, "[cursor] default size %d, rule says %d%s\n", have, want,
                have == want ? "" : " -> set");
    if (have == want) return false;
    set_size(dpy, want);
    return true;
}

#endif /* LIU_PLATFORM_X11_CURSOR_XLIB_H */
