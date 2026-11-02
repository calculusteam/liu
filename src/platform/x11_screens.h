/*
 * Liu - the monitors of the X screen Liu is on (X11)
 *
 * An X server describes its monitors in more than one way, and which ways it
 * has depends on its age and how it was started:
 *
 *   1. RandR monitors (protocol 1.5, 2015 on): the list a desktop shows in its
 *      display settings, with user-made monitors and mirrored outputs folded.
 *   2. RandR CRTCs (1.2 on): one rectangle per scanned-out controller. Older
 *      servers and some nested ones (Xvnc, Xephyr) offer nothing else.
 *   3. Xinerama, for a server without RandR, or one with several X screens
 *      glued into one by +xinerama: the screen is then one big rectangle whose
 *      heads are the former screens.
 *   4. The whole screen: a server with none of them is one monitor.
 *
 * Liu takes the first that yields a monitor. It only ever looks at the screen
 * its $DISPLAY names (DISPLAY=:0.1 is screen 1): a server of several separate
 * X screens keeps each screen's windows on that screen, and a second screen is
 * not used unless Liu was started on it.
 *
 * Pure C: the order, the version gates and the clean-up take the three queries
 * as function pointers, so stand-ins can replace them.
 */
#ifndef LIU_PLATFORM_X11_SCREENS_H
#define LIU_PLATFORM_X11_SCREENS_H

#include "core/types.h"
#include "platform/x11_monitor.h"
#include <stdlib.h>

#define X11_MONITORS_MAX 16

typedef enum {
    X11_MONS_RANDR_MONITORS = 0,
    X11_MONS_RANDR_CRTCS,
    X11_MONS_XINERAMA,
    X11_MONS_SCREEN
} X11MonSource;

static inline const char *x11_monitors_source_name(X11MonSource s) {
    switch (s) {
    case X11_MONS_RANDR_MONITORS: return "RandR monitors";
    case X11_MONS_RANDR_CRTCS:    return "RandR CRTCs";
    case X11_MONS_XINERAMA:       return "Xinerama";
    default:                      return "the screen";
    }
}

static inline bool x11_randr_has_monitors(i32 major, i32 minor) {
    return major > 1 || (major == 1 && minor >= 5);
}

static inline bool x11_randr_has_crtcs(i32 major, i32 minor) {
    return major > 1 || (major == 1 && minor >= 2);
}

/* The version a server speaks, held down to "major.minor" text when there is
 * one (a test aid: it makes a 1.6 server answer as the 1.4 one it is standing
 * in for). Text that is not a version changes nothing. */
static inline void x11_randr_version_cap(i32 *major, i32 *minor, const char *cap) {
    if (!major || !minor || !cap) return;
    char *end = NULL;
    long a = strtol(cap, &end, 10);
    if (end == cap || *end != '.' || a < 0 || a > 100) return;
    char *end2 = NULL;
    long b = strtol(end + 1, &end2, 10);
    if (end2 == end + 1 || b < 0 || b > 1000) return;
    if (a < *major || (a == *major && b < *minor)) { *major = (i32)a; *minor = (i32)b; }
}

/* One layer's rectangles made usable: cut to the screen, the empty dropped, a
 * rectangle that repeats an earlier one (outputs that mirror each other) kept
 * once. Returns the new count. */
static inline i32 x11_monitors_clean(X11Rect *r, i32 n, i32 screen_w, i32 screen_h) {
    i32 out = 0;
    for (i32 i = 0; r && i < n; i++) {
        X11Rect m = r[i];
        if (m.x < 0) { m.w += m.x; m.x = 0; }
        if (m.y < 0) { m.h += m.y; m.y = 0; }
        if (screen_w > 0 && m.x + m.w > screen_w) m.w = screen_w - m.x;
        if (screen_h > 0 && m.y + m.h > screen_h) m.h = screen_h - m.y;
        if (m.w <= 0 || m.h <= 0) continue;
        bool seen = false;
        for (i32 j = 0; j < out && !seen; j++)
            seen = r[j].x == m.x && r[j].y == m.y && r[j].w == m.w && r[j].h == m.h;
        if (!seen) r[out++] = m;
    }
    return out;
}

/* A query for one layer: fills out[0..cap) and returns how many it wrote,
 * 0 for "nothing here". */
typedef i32 (*X11MonFetch)(void *ctx, X11Rect *out, i32 cap);

typedef struct {
    void       *ctx;
    X11MonFetch randr_monitors;   /* NULL: the build has no RandR */
    X11MonFetch randr_crtcs;
    X11MonFetch xinerama;         /* NULL: no libXinerama on this system */
    i32         randr_major, randr_minor;   /* 0.0: the server has no RandR */
    i32         screen_w, screen_h;
} X11MonLayers;

static inline i32 x11_monitors_take(X11MonFetch f, void *ctx, X11Rect *out, i32 cap,
                                    i32 screen_w, i32 screen_h) {
    i32 got = f(ctx, out, cap);
    if (got > cap) got = cap;
    if (got < 0) got = 0;
    return x11_monitors_clean(out, got, screen_w, screen_h);
}

/* The monitors of the screen: the first layer that gives one, cleaned. The
 * whole screen when none does, so there is always at least one. */
static inline i32 x11_monitors_discover(const X11MonLayers *L, X11Rect *out, i32 cap,
                                        X11MonSource *source) {
    if (cap > X11_MONITORS_MAX) cap = X11_MONITORS_MAX;
    i32 n = 0;
    X11MonSource src = X11_MONS_SCREEN;
    if (L && out && cap > 0) {
        if (L->randr_monitors && x11_randr_has_monitors(L->randr_major, L->randr_minor)) {
            n = x11_monitors_take(L->randr_monitors, L->ctx, out, cap, L->screen_w, L->screen_h);
            if (n > 0) src = X11_MONS_RANDR_MONITORS;
        }
        if (n == 0 && L->randr_crtcs && x11_randr_has_crtcs(L->randr_major, L->randr_minor)) {
            n = x11_monitors_take(L->randr_crtcs, L->ctx, out, cap, L->screen_w, L->screen_h);
            if (n > 0) src = X11_MONS_RANDR_CRTCS;
        }
        if (n == 0 && L->xinerama) {
            n = x11_monitors_take(L->xinerama, L->ctx, out, cap, L->screen_w, L->screen_h);
            if (n > 0) src = X11_MONS_XINERAMA;
        }
    }
    if (n == 0 && out && cap > 0) {
        out[0] = (X11Rect){ 0, 0, L ? L->screen_w : 0, L ? L->screen_h : 0 };
        n = 1;
        src = X11_MONS_SCREEN;
    }
    if (source) *source = src;
    return n;
}

#endif /* LIU_PLATFORM_X11_SCREENS_H */
