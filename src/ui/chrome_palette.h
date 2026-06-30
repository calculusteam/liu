/*
 * Liu — chrome palette
 *
 * Pure-function derivation of the *semantic* colours every chrome
 * surface in Liu paints with: button fills/labels (primary, secondary,
 * destructive), neutral/raised/sunken surfaces, dividers.
 *
 * Why this exists: the chrome used to hardcode RGB values (or naive
 * `accent.rgb * 0.28f` darkenings) that only happened to read on dark
 * themes. The moment a light or pastel theme is loaded, white text
 * sat on white, the delete button disappeared, dividers vanished.
 *
 * Rules of the road for callers:
 *
 *   1. NEVER hardcode an RGBA literal in a chrome render path. Compute
 *      the palette once at the top of the function and use its fields.
 *   2. The palette is cheap to derive (a handful of arithmetic ops);
 *      compute per-frame, do not cache between frames.
 *   3. `legible_on(fill)` returns whichever of black/white reads on
 *      `fill`. Use it any time you are about to paint a literal text
 *      colour onto a fill that varies with the theme.
 */
#ifndef UI_CHROME_PALETTE_H
#define UI_CHROME_PALETTE_H

#include "core/types.h"
#include "core/config.h"
#include "renderer/renderer.h"

typedef struct {
    bool   is_light;          /* mean(bg.r,g,b) > 0.5 */

    /* Surfaces */
    Color  surface_neutral;   /* card/chip background — adapts to theme */
    Color  surface_raised;    /* one step lighter (dark) / darker (light) */
    Color  surface_sunken;    /* input/field background */

    /* Buttons — primary action (Generate / Connect / Add Rule). The
     * background is always saturated/dark enough to take a white-or-
     * dark legible foreground. */
    Color  btn_primary_bg;
    Color  btn_primary_fg;
    Color  btn_primary_hover;

    /* Buttons — secondary (Cancel, Import). Neutral fill, theme-aware
     * text. */
    Color  btn_secondary_bg;
    Color  btn_secondary_fg;
    Color  btn_secondary_hover;

    /* Destructive (delete affordances). Always reads as red but
     * tone-adjusted for light vs dark. */
    Color  btn_destructive_bg;     /* tinted background of the pill */
    Color  btn_destructive_fg;     /* the actual ✕ glyph colour */

    /* Dividers — subtle row separators / strong panel borders. */
    Color  divider_subtle;
    Color  divider_strong;
} ChromePalette;

/* Derive the chrome palette for a theme. Cheap to call per-frame.
 * `t` may be NULL — falls back to THEME_DARK. */
ChromePalette chrome_palette_for(const Theme *t);

/* Pick whichever of pure-black / pure-white reads on `fill`. Uses the
 * sRGB relative-luminance approximation. Caller can supply alpha
 * separately; output alpha is 1.0. */
Color chrome_legible_on(Color fill);

/* Approximate sRGB relative luminance of `c`, ignoring its alpha. */
f32 chrome_luminance(Color c);

#endif /* UI_CHROME_PALETTE_H */
