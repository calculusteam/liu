/*
 * Liu - the desktop's scale, asked of the server (X11)
 *
 * What x11_dpi_source.h chooses between, read off an X display: the resources
 * (RESOURCE_MANAGER of screen 0's root, which every screen of the display
 * shares, with the screen's own SCREEN_RESOURCES laid over it) and the
 * settings manager's property. Included once, by platform_linux.c, after the
 * X11 headers.
 *
 * The settings manager (XSETTINGS, x11_xsettings.h) is looked for once and
 * then followed: its property is read again when it changes (the caller sees
 * the PropertyNotify), and the owner again when a manager announces itself with
 * the MANAGER client message or the owner's window goes.
 *
 * LIU_DPI_PROBE=1 says on stderr where the scale came from each time it is
 * worked out.
 */
#ifndef LIU_PLATFORM_X11_SCALE_XLIB_H
#define LIU_PLATFORM_X11_SCALE_XLIB_H

#include "platform/x11_dpi_source.h"
#include "platform/x11_cursor_size.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Display       *dpy;
    int            screen;
    Atom           a_selection;         /* _XSETTINGS_S<screen> */
    Atom           a_property;          /* _XSETTINGS_SETTINGS */
    Atom           a_manager;           /* MANAGER */
    Atom           a_screen_resources;  /* SCREEN_RESOURCES */
    Window         owner;               /* the settings manager's window, None = none runs */
    XSettingsScale xs;                  /* the scale settings of its property */
    bool           probed;              /* the owner has been looked for once */
    X11DpiResult   dpi;                 /* the last answer and where it came from */
    i32            cursor_res;          /* Xcursor.size of the resources, 0 = none */
} X11ScaleState;

static inline void x11x_scale_init(X11ScaleState *st, Display *dpy, int screen) {
    char sel[32];
    memset(st, 0, sizeof *st);
    st->dpy = dpy;
    st->screen = screen;
    snprintf(sel, sizeof sel, "_XSETTINGS_S%d", screen);
    st->a_selection        = XInternAtom(dpy, sel, False);
    st->a_property         = XInternAtom(dpy, "_XSETTINGS_SETTINGS", False);
    st->a_manager          = XInternAtom(dpy, "MANAGER", False);
    st->a_screen_resources = XInternAtom(dpy, "SCREEN_RESOURCES", False);
}

/* A property read whole into XFree()'d storage, of format 8 (a string). */
static inline bool x11x_string_property(Display *dpy, Window win, Atom prop, Atom type,
                                        unsigned long max_units, unsigned char **data,
                                        unsigned long *n) {
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *d = NULL;
    if (win == None || prop == None) return false;
    if (XGetWindowProperty(dpy, win, prop, 0, (long)max_units, False, type, &actual_type,
                           &actual_format, &nitems, &after, &d) != Success || !d)
        return false;
    if (actual_type == None || actual_format != 8 || nitems == 0) { XFree(d); return false; }
    *data = d;
    *n = nitems;
    return true;
}

/* A root's string property as a resource database; NULL when it has none. The
 * database copies what it keeps. */
static inline XrmDatabase x11x_root_resources(Display *dpy, Window root, Atom prop) {
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (!x11x_string_property(dpy, root, prop, XA_STRING, 256 * 1024, &data, &n)) return NULL;
    XrmDatabase db = XrmGetStringDatabase((const char *)data);
    XFree(data);
    return db;
}

/* The manager's property, read again (after it changed, or after its owner did). */
static inline void x11x_xsettings_read(X11ScaleState *st) {
    memset(&st->xs, 0, sizeof st->xs);
    if (st->owner == None) return;
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (x11x_string_property(st->dpy, st->owner, st->a_property, AnyPropertyType, 64 * 1024,
                             &data, &n)) {
        st->xs = xsettings_scale_of(data, n);
        XFree(data);
    }
}

/* Find the manager: whoever owns the selection now. Its window's property
 * changes and its destruction are asked for before the first read, so a change
 * in between is not lost. */
static inline void x11x_xsettings_find(X11ScaleState *st) {
    st->probed = true;
    st->owner = XGetSelectionOwner(st->dpy, st->a_selection);
    memset(&st->xs, 0, sizeof st->xs);
    if (st->owner == None) return;
    XSelectInput(st->dpy, st->owner, PropertyChangeMask | StructureNotifyMask);
    x11x_xsettings_read(st);
}

/* The scale from every place that can declare one, in the order x11_dpi_source.h
 * gives; remembered in the state with the cursor resource the same read found. */
static inline X11DpiResult x11x_scale_read(X11ScaleState *st) {
    X11DpiInputs in;
    memset(&in, 0, sizeof in);
    /* Read the resources off the root ourselves rather than calling
     * XResourceManagerString(): that helper returns the copy Xlib cached when
     * the Display was opened and never refetches, so it reports the startup
     * value forever and a live Xft.dpi change would go unnoticed. */
    XrmDatabase db = x11x_root_resources(st->dpy, RootWindow(st->dpy, 0), XA_RESOURCE_MANAGER);
    XrmDatabase sdb = x11x_root_resources(st->dpy, RootWindow(st->dpy, st->screen),
                                          st->a_screen_resources);
    if (sdb) { if (db) XrmCombineDatabase(sdb, &db, True); else db = sdb; }
    i32 cursor_res = 0;
    if (db) {
        char *type = NULL; XrmValue val;
        if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &val) && val.addr)
            in.has_xft_dpi = x11_parse_leading_double(val.addr, &in.xft_dpi);
        if (XrmGetResource(db, "Xcursor.size", "Xcursor.Size", &type, &val) && val.addr)
            cursor_res = x11_cursor_size_text(val.addr);
        XrmDestroyDatabase(db);
    }
    if (!st->probed) x11x_xsettings_find(st);
    in.xs = st->xs;
    in.gdk_scale = getenv("GDK_SCALE");
    in.qt_scale_factor = getenv("QT_SCALE_FACTOR");

    X11DpiResult r = x11_dpi_resolve(&in);
    st->dpi = r;
    st->cursor_res = cursor_res;
    const char *probe = getenv("LIU_DPI_PROBE");
    if (probe && *probe && *probe != '0')
        fprintf(stderr, "[dpi] scale %.3f from %s (%.1f dpi), Xcursor.size %d, settings manager 0x%lx\n",
                (double)r.scale, x11_dpi_source_name(r.source), r.dpi, cursor_res,
                (unsigned long)st->owner);
    return r;
}

#endif /* LIU_PLATFORM_X11_SCALE_XLIB_H */
