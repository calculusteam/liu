/*
 * Liu - washes on the explorer panel
 *
 * The renderer composites in linear light. A wash of a light ink over a dark
 * surface at a small alpha therefore lands far lighter than the same fraction
 * mixed in display space: white at 3.5% over #0A0A0B reads #363636, not
 * #121214. Every tint of the panel is stated as a display-space mix and drawn
 * as a wash whose alpha is solved for it here.
 */
#ifndef UI_PANEL_INK_H
#define UI_PANEL_INK_H

#include "core/types.h"
#include "renderer/renderer.h"
#include <math.h>

static inline f32 panel_ink_lin(f32 c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static inline f32 panel_ink_lum_lin(Color c) {
    return 0.2126f * panel_ink_lin(c.r) + 0.7152f * panel_ink_lin(c.g) +
           0.0722f * panel_ink_lin(c.b);
}

/* `base` and `ink` mixed in display space at `k`, opaque. */
static inline Color panel_mix(Color base, Color ink, f32 k) {
    return (Color){ base.r + (ink.r - base.r) * k, base.g + (ink.g - base.g) * k,
                    base.b + (ink.b - base.b) * k, 1.0f };
}

/* `ink` at the alpha that, composited over `base` in linear light, lands on
 * the luminance of panel_mix(base, ink, k). */
static inline Color panel_wash(Color base, Color ink, f32 k) {
    f32 lb = panel_ink_lum_lin(base), li = panel_ink_lum_lin(ink);
    f32 lt = panel_ink_lum_lin(panel_mix(base, ink, k));
    f32 a = fabsf(li - lb) > 1e-4f ? (lt - lb) / (li - lb) : k;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return (Color){ ink.r, ink.g, ink.b, a };
}

#endif /* UI_PANEL_INK_H */
