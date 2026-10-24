/*
 * Device viewport presets expressed in CSS pixels. Responsive layout and media
 * queries use CSS dimensions rather than physical display pixels.
 */
#include "browser/browser_viewport.h"

static const BrowserViewportPreset g_presets[] = {
    /* name                      w     h     dpr    group */
    { "Responsive",              0,    0,  0.00f, BVP_GROUP_PHONE   },  /* sentinel */

    /* ---- Phones -------------------------------------------------------- */
    { "iPhone SE",             375,  667,  2.00f, BVP_GROUP_PHONE   },
    { "iPhone XR",             414,  896,  2.00f, BVP_GROUP_PHONE   },
    { "iPhone 14",             390,  844,  3.00f, BVP_GROUP_PHONE   },
    { "iPhone 14 Plus",        428,  926,  3.00f, BVP_GROUP_PHONE   },
    { "iPhone 15 Pro",         393,  852,  3.00f, BVP_GROUP_PHONE   },
    { "iPhone 15 Pro Max",     430,  932,  3.00f, BVP_GROUP_PHONE   },
    { "iPhone 16 Pro",         402,  874,  3.00f, BVP_GROUP_PHONE   },
    { "iPhone 16 Pro Max",     440,  956,  3.00f, BVP_GROUP_PHONE   },
    { "Pixel 8",               412,  915,  2.63f, BVP_GROUP_PHONE   },
    { "Pixel 9",               412,  924,  2.63f, BVP_GROUP_PHONE   },
    { "Pixel 9 Pro",           427,  952,  3.00f, BVP_GROUP_PHONE   },
    { "Pixel 9 Pro XL",        448,  997,  3.00f, BVP_GROUP_PHONE   },
    { "Galaxy S20 Ultra",      412,  915,  3.50f, BVP_GROUP_PHONE   },
    { "Galaxy Z Fold 5",       344,  882,  2.63f, BVP_GROUP_PHONE   },
    { "Surface Duo",           540,  720,  2.50f, BVP_GROUP_PHONE   },

    /* ---- Tablets ------------------------------------------------------- */
    { "iPad Mini",             768, 1024,  2.00f, BVP_GROUP_TABLET  },
    { "iPad Air",              820, 1180,  2.00f, BVP_GROUP_TABLET  },
    { "iPad Pro",             1024, 1366,  2.00f, BVP_GROUP_TABLET  },
    { "Surface Pro 7",         912, 1368,  2.00f, BVP_GROUP_TABLET  },
    { "Asus Zenbook Fold",     853, 1280,  1.50f, BVP_GROUP_TABLET  },

    /* ---- Laptops — natively landscape ---------------------------------- */
    { "Laptop (MDPI)",        1280,  800,  1.00f, BVP_GROUP_LAPTOP  },
    { "Laptop (HiDPI)",       1440,  900,  2.00f, BVP_GROUP_LAPTOP  },
    { "Desktop",              1920, 1080,  1.00f, BVP_GROUP_LAPTOP  },
    { "Desktop 4K",           2560, 1440,  1.00f, BVP_GROUP_LAPTOP  },
};


i32 browser_viewport_count(void) {
    return (i32)(sizeof g_presets / sizeof g_presets[0]);
}

const BrowserViewportPreset *browser_viewport_get(i32 idx) {
    if (idx < 0 || idx >= browser_viewport_count()) return NULL;
    return &g_presets[idx];
}

const char *browser_viewport_group_name(i32 group) {
    switch (group) {
    case BVP_GROUP_PHONE:  return "Phone";
    case BVP_GROUP_TABLET: return "Tablet";
    case BVP_GROUP_LAPTOP: return "Laptop / Display";
    default:               return "";
    }
}

bool browser_viewport_size(i32 idx, bool rotated, i32 *out_w, i32 *out_h) {
    const BrowserViewportPreset *p = browser_viewport_get(idx);
    if (!p || idx == BVP_RESPONSIVE || p->w <= 0 || p->h <= 0) return false;
    i32 w = p->w, h = p->h;
    if (rotated) { i32 t = w; w = h; h = t; }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}

bool browser_viewport_is_landscape(i32 idx, bool rotated) {
    i32 w = 0, h = 0;
    if (!browser_viewport_size(idx, rotated, &w, &h)) return false;
    return w > h;
}

BrowserViewportFit browser_viewport_fit(i32 vw, i32 vh,
                                        f32 ax, f32 ay, f32 aw, f32 ah) {
    BrowserViewportFit f;
    if (vw <= 0 || vh <= 0 || aw <= 0.0f || ah <= 0.0f) {
        f.x = ax; f.y = ay; f.w = aw; f.h = ah; f.scale = 1.0f;
        return f;
    }
    f32 sx = aw / (f32)vw;
    f32 sy = ah / (f32)vh;
    f32 s = sx < sy ? sx : sy;
    /* Never magnify: the fit only shrinks the device, and the page still
     * lays out at the device width. A 375 px phone blown up to fill a wide
     * pane would show the device at a size it does not have. */
    if (s > 1.0f) s = 1.0f;
    f.scale = s;
    f.w = (f32)vw * s;
    f.h = (f32)vh * s;
    f.x = ax + (aw - f.w) * 0.5f;
    f.y = ay + (ah - f.h) * 0.5f;
    return f;
}
