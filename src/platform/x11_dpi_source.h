/*
 * Liu - where the desktop's scale comes from (X11)
 *
 * X11 has no per-window backing scale. What a desktop declares is spread over
 * three places, and which one a session fills depends on who started it:
 *
 *   1. Xft.dpi in the root window's RESOURCE_MANAGER - what xrdb loads, what
 *      GNOME's and Plasma's session scripts write. When it is there it wins:
 *      it is the setting a person edits by hand.
 *   2. XSETTINGS (x11_xsettings.h) - the settings daemons of Xfce, Cinnamon,
 *      MATE, LXQt and xsettingsd keep the scale ONLY there. Xft/DPI is dots per
 *      inch x 1024 and already carries the window scale; where it is missing,
 *      Gdk/WindowScalingFactor is the integer scale (times Gdk/UnscaledDPI over
 *      96 when that is given too, which is how the daemon builds Xft/DPI).
 *   3. The environment of the process: GDK_SCALE (an integer, GTK's window
 *      scale) and QT_SCALE_FACTOR (a factor, Qt's) - the way a compositor
 *      user without a settings daemon asks for a scale.
 *
 * Not used, on purpose: GDK_DPI_SCALE scales text only, not windows;
 * QT_SCREEN_SCALE_FACTORS names outputs one by one and QT_AUTO_SCREEN_SCALE_FACTOR
 * derives the scale from the physical size; and the screen's size in
 * millimetres, which a server fills from whatever the monitor's EDID says (a
 * virtual screen, a projector, a KVM or a nested compositor reports anything),
 * so deriving a scale from it makes a correct 1x screen come out 1.4x.
 *
 * Nothing scales below 1: a point dimension is never shrunk. Pure C: the order
 * and the edges need no server.
 */
#ifndef LIU_PLATFORM_X11_DPI_SOURCE_H
#define LIU_PLATFORM_X11_DPI_SOURCE_H

#include "core/types.h"
#include "platform/x11_xsettings.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef enum {
    X11_DPI_NONE = 0,          /* nothing declared: 1x */
    X11_DPI_RESOURCE,          /* Xft.dpi in RESOURCE_MANAGER */
    X11_DPI_XSETTINGS_DPI,     /* XSETTINGS Xft/DPI */
    X11_DPI_XSETTINGS_SCALE,   /* XSETTINGS Gdk/WindowScalingFactor */
    X11_DPI_GDK_SCALE,         /* $GDK_SCALE */
    X11_DPI_QT_SCALE           /* $QT_SCALE_FACTOR */
} X11DpiSource;

#define X11_DPI_BASE      96.0
#define X11_DPI_MAX       768.0   /* 8x */
#define X11_SCALE_MAX     8.0

typedef struct {
    bool           has_xft_dpi;       /* RESOURCE_MANAGER has Xft.dpi ... */
    double         xft_dpi;           /* ... and this is its value */
    XSettingsScale xs;                /* all zero when no settings manager runs */
    const char    *gdk_scale;         /* $GDK_SCALE, or NULL */
    const char    *qt_scale_factor;   /* $QT_SCALE_FACTOR, or NULL */
} X11DpiInputs;

typedef struct {
    f32          scale;     /* >= 1 */
    double       dpi;       /* what the source said, in dots per inch; 0 = nothing said */
    X11DpiSource source;
} X11DpiResult;

static inline const char *x11_dpi_source_name(X11DpiSource s) {
    switch (s) {
    case X11_DPI_RESOURCE:        return "Xft.dpi";
    case X11_DPI_XSETTINGS_DPI:   return "XSETTINGS Xft/DPI";
    case X11_DPI_XSETTINGS_SCALE: return "XSETTINGS Gdk/WindowScalingFactor";
    case X11_DPI_GDK_SCALE:       return "GDK_SCALE";
    case X11_DPI_QT_SCALE:        return "QT_SCALE_FACTOR";
    default:                      return "default";
    }
}

/* A resource's number the way a resource reads: a leading number counts, what
 * follows it does not. */
static inline bool x11_parse_leading_double(const char *s, double *out) {
    if (!s || !out) return false;
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || errno == ERANGE || !isfinite(v)) return false;
    *out = v;
    return true;
}

/* An environment variable's number: all of it, give or take blanks. */
static inline bool x11_parse_whole_double(const char *s, double *out) {
    if (!s || !out) return false;
    while (*s == ' ' || *s == '\t') s++;
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || errno == ERANGE || !isfinite(v)) return false;
    while (*end == ' ' || *end == '\t' || *end == '\n') end++;
    if (*end) return false;
    *out = v;
    return true;
}

static inline f32 x11_dpi_to_scale(double dpi) {
    if (dpi > X11_DPI_MAX) dpi = X11_DPI_MAX;
    f32 s = (f32)(dpi / X11_DPI_BASE);
    return s < 1.0f ? 1.0f : s;
}

static inline X11DpiResult x11_dpi_result(double dpi, X11DpiSource src) {
    X11DpiResult r = { x11_dpi_to_scale(dpi), dpi, src };
    return r;
}

static inline X11DpiResult x11_scale_result(double scale, X11DpiSource src) {
    if (scale > X11_SCALE_MAX) scale = X11_SCALE_MAX;
    if (scale < 1.0) scale = 1.0;
    X11DpiResult r = { (f32)scale, scale * X11_DPI_BASE, src };
    return r;
}

/* The scale the desktop declares, from the first place that declares one. */
static inline X11DpiResult x11_dpi_resolve(const X11DpiInputs *in) {
    X11DpiResult none = { 1.0f, 0.0, X11_DPI_NONE };
    if (!in) return none;

    if (in->has_xft_dpi && isfinite(in->xft_dpi) && in->xft_dpi > 1.0)
        return x11_dpi_result(in->xft_dpi, X11_DPI_RESOURCE);

    const XSettingsScale *xs = &in->xs;
    if (xs->has_xft_dpi && xs->xft_dpi_1024 > 1024)
        return x11_dpi_result((double)xs->xft_dpi_1024 / 1024.0, X11_DPI_XSETTINGS_DPI);

    {
        bool factor = xs->has_window_scale && xs->window_scale >= 1 &&
                      xs->window_scale <= (i32)X11_SCALE_MAX;
        bool unscaled = xs->has_unscaled_dpi && xs->unscaled_dpi_1024 > 1024;
        if (factor || unscaled) {
            double scale = factor ? (double)xs->window_scale : 1.0;
            if (unscaled) scale *= ((double)xs->unscaled_dpi_1024 / 1024.0) / X11_DPI_BASE;
            return x11_scale_result(scale, X11_DPI_XSETTINGS_SCALE);
        }
    }

    {
        double v;
        if (x11_parse_whole_double(in->gdk_scale, &v) && v >= 1.0 && v <= X11_SCALE_MAX &&
            v == floor(v))
            return x11_scale_result(v, X11_DPI_GDK_SCALE);
        if (x11_parse_whole_double(in->qt_scale_factor, &v) && v >= 1.0 && v <= X11_SCALE_MAX)
            return x11_scale_result(v, X11_DPI_QT_SCALE);
    }
    return none;
}

/* Which property change on which window can change the answer above. The root
 * of the screen Liu draws on and the root of screen 0 (where RESOURCE_MANAGER
 * lives for every screen of the display) carry the resources; the settings
 * manager's window carries XSETTINGS. */
typedef enum { X11_SCALE_EVENT_NONE = 0, X11_SCALE_EVENT_RESOURCES, X11_SCALE_EVENT_XSETTINGS } X11ScaleEvent;

static inline X11ScaleEvent x11_scale_event_for(unsigned long win, unsigned long atom,
                                                unsigned long our_root, unsigned long root0,
                                                unsigned long xs_owner,
                                                unsigned long a_resource_manager,
                                                unsigned long a_screen_resources,
                                                unsigned long a_xsettings) {
    if (atom == 0) return X11_SCALE_EVENT_NONE;
    if (xs_owner != 0 && win == xs_owner && atom == a_xsettings)
        return X11_SCALE_EVENT_XSETTINGS;
    if (win == root0 && atom == a_resource_manager) return X11_SCALE_EVENT_RESOURCES;
    if (win == our_root && (atom == a_resource_manager || atom == a_screen_resources))
        return X11_SCALE_EVENT_RESOURCES;
    return X11_SCALE_EVENT_NONE;
}

#endif /* LIU_PLATFORM_X11_DPI_SOURCE_H */
