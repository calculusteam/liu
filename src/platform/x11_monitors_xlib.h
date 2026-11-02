/*
 * Liu - the monitors of an X screen, asked of the server (X11)
 *
 * The queries behind x11_screens.h's order: RandR monitors, RandR CRTCs and
 * Xinerama. Included once, by platform_linux.c, after the X11 headers.
 *
 * libXinerama is the system's own and is opened here when it is there (Liu
 * links no library for it); without it, or on a server that has no Xinerama,
 * that layer is empty.
 *
 * LIU_X11_RANDR_VERSION (a dev-hooks variable) holds the RandR version Liu
 * believes down to "major.minor": 1.4 stands a current server in for one that
 * has no monitors to ask for, 0.0 for one without RandR, which is how the
 * layers below the first are driven on a server that has all of them.
 * LIU_SCREEN_PROBE=1 says on stderr which layer the monitors came from.
 */
#ifndef LIU_PLATFORM_X11_MONITORS_XLIB_H
#define LIU_PLATFORM_X11_MONITORS_XLIB_H

#include "platform/x11_screens.h"
#include <X11/Xlib.h>
#ifdef LIU_HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { Display *dpy; int screen; } X11MonCtx;

/* The RandR version the server speaks, 0.0 without the extension. */
static inline void x11x_randr_version(Display *dpy, i32 *major, i32 *minor) {
    static bool probed = false;
    static i32 maj = 0, min = 0;
    if (!probed) {
        probed = true;
#ifdef LIU_HAVE_XRANDR
        int ev = 0, er = 0, a = 0, b = 0;
        if (dpy && XRRQueryExtension(dpy, &ev, &er) && XRRQueryVersion(dpy, &a, &b)) {
            maj = a; min = b;
        }
#else
        (void)dpy;
#endif
        x11_randr_version_cap(&maj, &min, liu_dev_env("LIU_X11_RANDR_VERSION"));
    }
    *major = maj; *minor = min;
}

#ifdef LIU_HAVE_XRANDR
static i32 x11x_mons_randr_monitors(void *ctx, X11Rect *out, i32 cap) {
    const X11MonCtx *c = (const X11MonCtx *)ctx;
    int nmon = 0;
    XRRMonitorInfo *mons = XRRGetMonitors(c->dpy, RootWindow(c->dpy, c->screen), True, &nmon);
    i32 n = 0;
    for (int i = 0; mons && i < nmon && n < cap; i++)
        out[n++] = (X11Rect){ mons[i].x, mons[i].y, mons[i].width, mons[i].height };
    if (mons) XRRFreeMonitors(mons);
    return n;
}

/* One rectangle per CRTC that is scanning something out. */
static i32 x11x_mons_randr_crtcs(void *ctx, X11Rect *out, i32 cap) {
    const X11MonCtx *c = (const X11MonCtx *)ctx;
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(c->dpy, RootWindow(c->dpy, c->screen));
    if (!res) return 0;
    i32 n = 0;
    for (int i = 0; i < res->ncrtc && n < cap; i++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(c->dpy, res, res->crtcs[i]);
        if (!ci) continue;
        if (ci->mode != None && ci->noutput > 0 && ci->width > 0 && ci->height > 0)
            out[n++] = (X11Rect){ ci->x, ci->y, (i32)ci->width, (i32)ci->height };
        XRRFreeCrtcInfo(ci);
    }
    XRRFreeScreenResources(res);
    return n;
}
#endif

/* libXinerama's reply: its own header is not needed for this much of it. */
typedef struct { int screen_number; short x_org, y_org, width, height; } X11XineramaScreen;

/* The heads of a server that glues its screens into one (+xinerama) or has no
 * RandR. Xinerama describes screen 0 only. */
static i32 x11x_mons_xinerama(void *ctx, X11Rect *out, i32 cap) {
    const X11MonCtx *c = (const X11MonCtx *)ctx;
    static bool tried = false;
    static Bool (*is_active)(Display *);
    static X11XineramaScreen *(*query)(Display *, int *);
    if (c->screen != 0 || !c->dpy) return 0;
    if (!tried) {
        tried = true;
        void *lib = dlopen("libXinerama.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (lib) {
            *(void **)(&is_active) = dlsym(lib, "XineramaIsActive");
            *(void **)(&query) = dlsym(lib, "XineramaQueryScreens");
        }
    }
    if (!is_active || !query || !is_active(c->dpy)) return 0;
    int count = 0;
    X11XineramaScreen *heads = query(c->dpy, &count);
    i32 n = 0;
    for (int i = 0; heads && i < count && n < cap; i++)
        out[n++] = (X11Rect){ heads[i].x_org, heads[i].y_org, heads[i].width, heads[i].height };
    if (heads) XFree(heads);
    return n;
}

/* The monitors of `screen` of the display: RandR monitors, else RandR CRTCs,
 * else Xinerama, else the whole screen. */
static i32 x11x_monitors(Display *dpy, int screen, X11Rect *out, i32 cap, X11MonSource *source) {
    X11MonCtx ctx = { dpy, screen };
    X11MonLayers L;
    memset(&L, 0, sizeof L);
    L.ctx = &ctx;
    L.xinerama = x11x_mons_xinerama;
    L.screen_w = DisplayWidth(dpy, screen);
    L.screen_h = DisplayHeight(dpy, screen);
    x11x_randr_version(dpy, &L.randr_major, &L.randr_minor);
#ifdef LIU_HAVE_XRANDR
    L.randr_monitors = x11x_mons_randr_monitors;
    L.randr_crtcs = x11x_mons_randr_crtcs;
#endif
    X11MonSource src = X11_MONS_SCREEN;
    i32 n = x11_monitors_discover(&L, out, cap, &src);
    if (source) *source = src;
    const char *probe = getenv("LIU_SCREEN_PROBE");
    if (probe && *probe && *probe != '0') {
        fprintf(stderr, "[screen] %d of the display, %dx%d, %d monitor%s via %s (RandR %d.%d):",
                screen, (int)L.screen_w, (int)L.screen_h, (int)n, n == 1 ? "" : "s",
                x11_monitors_source_name(src), (int)L.randr_major, (int)L.randr_minor);
        for (i32 i = 0; i < n; i++)
            fprintf(stderr, " %dx%d%+d%+d", (int)out[i].w, (int)out[i].h, (int)out[i].x, (int)out[i].y);
        fprintf(stderr, "\n");
    }
    return n;
}

#endif /* LIU_PLATFORM_X11_MONITORS_XLIB_H */
