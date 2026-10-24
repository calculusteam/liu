/* The three status colours: "this is protected / worked", "look at this" and
 * "this failed or destroys". No state.
 *
 * Each one is the theme's own colour when it has a convincing one and a
 * canonical colour otherwise. A monochrome palette's green, yellow and red
 * slots are grey, and a grey padlock, caution mark or Delete says nothing —
 * so a slot is taken only when its hue leads by enough to be a colour.
 * `ui_safe()` and `ui_danger()` in ui.c are these two; the floating
 * notifications are the third's first reader. */
#ifndef UI_STATUS_COLORS_H
#define UI_STATUS_COLORS_H

#include "core/config.h"

static inline Color status_color_safe(const Theme *t) {
    const Color fallback = { 0.33f, 0.78f, 0.49f, 1.0f };
    if (!t) return fallback;
    const i32 slots[2] = { 10, 2 };
    for (i32 k = 0; k < 2; k++) {
        Color c = t->ansi[slots[k]];
        f32 max_c = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
        f32 min_c = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
        /* Green leads, and by enough to be a colour. */
        if (c.g >= c.r && c.g >= c.b && max_c - min_c >= 0.22f && max_c >= 0.45f) { c.a = 1.0f; return c; }
    }
    return fallback;
}

static inline Color status_color_warn(const Theme *t) {
    const Color fallback = { 0.92f, 0.72f, 0.33f, 1.0f };
    if (!t) return fallback;
    const i32 slots[2] = { 11, 3 };
    for (i32 k = 0; k < 2; k++) {
        Color c = t->ansi[slots[k]];
        f32 max_c = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
        f32 min_c = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
        /* Yellow and orange: red and green both lead blue, and neither is
         * much below the other (a red has no green in it, a lime no red). */
        if (c.r >= c.b && c.g >= c.b && c.r >= 0.6f * c.g && c.g >= 0.45f * c.r &&
            max_c - min_c >= 0.22f && max_c >= 0.45f) { c.a = 1.0f; return c; }
    }
    return fallback;
}

static inline Color status_color_danger(const Theme *t) {
    const Color fallback = { 0.93f, 0.32f, 0.33f, 1.0f };
    if (!t) return fallback;
    const i32 slots[2] = { 9, 1 };
    for (i32 k = 0; k < 2; k++) {
        Color c = t->ansi[slots[k]];
        f32 max_c = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
        f32 min_c = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
        f32 sat = max_c > 0.001f ? (max_c - min_c) / max_c : 0.0f;
        if (max_c > 0.45f && sat > 0.35f && c.r >= c.g && c.r >= c.b) { c.a = 1.0f; return c; }
    }
    return fallback;
}

#endif /* UI_STATUS_COLORS_H */
