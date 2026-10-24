/*
 * Device viewport emulation constrains a page to a preset CSS-pixel size and
 * device-pixel ratio instead of filling the browser pane.
 */
#ifndef BROWSER_VIEWPORT_H
#define BROWSER_VIEWPORT_H

#include "core/types.h"

typedef enum {
    BVP_GROUP_PHONE = 0,
    BVP_GROUP_TABLET,
    BVP_GROUP_LAPTOP,   /* laptops + smart displays: natively landscape */
    BVP_GROUP_COUNT,
} BrowserViewportGroup;

typedef struct {
    const char *name;    /* "iPhone 15 Pro" */
    /* CSS pixels in the device's NATURAL orientation — portrait for phones and
     * tablets, landscape for laptops and smart displays. Storing everything
     * portrait-first would make the default view wrong for every laptop. */
    i32         w, h;
    f32         dpr;     /* device pixel ratio — displayed, not emulated */
    u8          group;   /* BrowserViewportGroup */
} BrowserViewportPreset;

/* Index 0 is always "Responsive" (fill the pane); presets start at 1. */
#define BVP_RESPONSIVE 0

i32 browser_viewport_count(void);                       /* including Responsive */
const BrowserViewportPreset *browser_viewport_get(i32 idx);   /* NULL if OOR */
const char *browser_viewport_group_name(i32 group);

/* Effective CSS viewport for a preset. `rotated` swaps away from the device's
 * natural orientation. Returns false for Responsive (fill the pane). */
bool browser_viewport_size(i32 idx, bool rotated, i32 *out_w, i32 *out_h);

/* Whether the effective viewport is wider than tall — what the UI should
 * label it, rather than assuming "rotated" means landscape (it does not for a
 * laptop, whose natural orientation already is). */
bool browser_viewport_is_landscape(i32 idx, bool rotated);

/* Fit a viewport into an available area.
 *
 * `scale` is <= 1: a viewport wider or taller than the pane is scaled down so
 * all of it stays visible (never scaled UP — a phone rendered at 2x would
 * misrepresent the device's size). The result is the centred on-screen rect
 * plus the scale, all in points.
 */
typedef struct {
    f32 x, y, w, h;   /* on-screen rect, centred in the available area */
    f32 scale;        /* <= 1.0 */
} BrowserViewportFit;

BrowserViewportFit browser_viewport_fit(i32 vw, i32 vh,
                                        f32 ax, f32 ay, f32 aw, f32 ah);

#endif /* BROWSER_VIEWPORT_H */
