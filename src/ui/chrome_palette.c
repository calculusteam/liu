/*
 * Liu — chrome palette implementation
 */
#include "ui/chrome_palette.h"
#include <math.h>

/* sRGB relative luminance (IEC 61966-2-1, ignores alpha). */
f32 chrome_luminance(Color c)
{
    /* Linearise each channel — fast piecewise approximation. */
    f32 r = c.r <= 0.04045f ? c.r / 12.92f : powf((c.r + 0.055f) / 1.055f, 2.4f);
    f32 g = c.g <= 0.04045f ? c.g / 12.92f : powf((c.g + 0.055f) / 1.055f, 2.4f);
    f32 b = c.b <= 0.04045f ? c.b / 12.92f : powf((c.b + 0.055f) / 1.055f, 2.4f);
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/* Return pure black or white, whichever achieves the higher contrast on fill. */
Color chrome_legible_on(Color fill)
{
    f32 L = chrome_luminance(fill);
    /* WCAG contrast with white = (L + 0.05) / 0.05; with black = 1.05 / (L + 0.05).
     * White wins when L < 0.179, black otherwise. */
    if (L < 0.179f)
        return (Color){1.0f, 1.0f, 1.0f, 1.0f};
    return (Color){0.0f, 0.0f, 0.0f, 1.0f};
}

/* Blend two colours linearly (alpha ignored, output alpha = 1). */
static Color blend(Color a, Color b, f32 t)
{
    return (Color){
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        1.0f
    };
}

/* Clamp a float to [0,1]. */
static f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

/* Scale luminance of a colour toward black (t<0) or white (t>0). */
static Color shift(Color c, f32 t)
{
    if (t >= 0.0f)
        return (Color){ clamp01(c.r + (1.0f - c.r) * t),
                        clamp01(c.g + (1.0f - c.g) * t),
                        clamp01(c.b + (1.0f - c.b) * t), 1.0f };
    f32 s = 1.0f + t; /* t is negative → s < 1 */
    return (Color){ c.r * s, c.g * s, c.b * s, 1.0f };
}

ChromePalette chrome_palette_for(const Theme *t)
{
    if (!t) {
        /* Return a hardcoded dark palette when no theme is present. */
        ChromePalette p = {0};
        p.is_light = false;

        p.surface_neutral    = (Color){0.13f, 0.13f, 0.16f, 1.0f};
        p.surface_raised     = (Color){0.18f, 0.18f, 0.22f, 1.0f};
        p.surface_sunken     = (Color){0.09f, 0.09f, 0.11f, 1.0f};

        p.btn_primary_bg     = (Color){0.26f, 0.42f, 0.72f, 1.0f};
        p.btn_primary_fg     = (Color){1.0f,  1.0f,  1.0f,  1.0f};
        p.btn_primary_hover  = (Color){0.32f, 0.50f, 0.82f, 1.0f};

        p.btn_secondary_bg   = (Color){0.18f, 0.20f, 0.26f, 1.0f};
        p.btn_secondary_fg   = (Color){0.88f, 0.88f, 0.92f, 1.0f};
        p.btn_secondary_hover= (Color){0.22f, 0.24f, 0.32f, 1.0f};

        p.btn_destructive_bg = (Color){0.95f, 0.30f, 0.30f, 0.14f};
        p.btn_destructive_fg = (Color){0.95f, 0.55f, 0.55f, 1.0f};

        p.divider_subtle     = (Color){0.22f, 0.22f, 0.26f, 0.5f};
        p.divider_strong     = (Color){0.30f, 0.30f, 0.35f, 1.0f};
        return p;
    }

    ChromePalette p = {0};
    f32 bg_lum = chrome_luminance(t->bg);
    p.is_light = (bg_lum > 0.5f);

    /* ---- Surfaces ---- */
    if (p.is_light) {
        /* Light theme: neutral is bg, raised is slightly darker, sunken is
         * slightly more tinted toward black. */
        p.surface_neutral = t->bg;
        p.surface_raised  = shift(t->bg, -0.06f);
        p.surface_sunken  = shift(t->bg, -0.10f);
    } else {
        /* Dark theme: neutral is slightly above bg, raised is lighter. */
        p.surface_neutral = shift(t->bg, 0.06f);
        p.surface_raised  = shift(t->bg, 0.12f);
        p.surface_sunken  = shift(t->bg, -0.03f);
    }
    p.surface_neutral.a = 1.0f;
    p.surface_raised.a  = 1.0f;
    p.surface_sunken.a  = 1.0f;

    /* ---- Primary button accent.  Resolution order:
     *   1. theme->ui_accent if the user/theme set one explicitly.
     *   2. theme->cursor when it is vividly saturated (sat > 0.40) —
     *      warm cursors imply warm chrome without forcing the theme
     *      author to duplicate the colour into ui_accent.
     *   3. theme->ansi[4] — the legacy default-blue.
     * Built-in themes whose cursor sits in the neutral gray/white
     * family fall straight through to ansi[4]. ---- */
    Color accent;
    if (t->ui_accent.a > 0.0f) {
        accent = t->ui_accent;
    } else {
        f32 cr = t->cursor.r, cg = t->cursor.g, cb = t->cursor.b;
        f32 max_c = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
        f32 min_c = cr < cg ? (cr < cb ? cr : cb) : (cg < cb ? cg : cb);
        f32 sat   = max_c > 0.001f ? (max_c - min_c) / max_c : 0.0f;
        accent = (t->cursor.a > 0.5f && sat > 0.40f) ? t->cursor : t->ansi[4];
    }
    accent.a = 1.0f;
    f32 acc_lum = chrome_luminance(accent);

    /* If accent is too bright for legible text (> 0.55), darken it; if it's
     * too similar to the background, push it toward a known-safe blue. */
    Color primary_bg = accent;
    if (p.is_light && acc_lum > 0.45f) {
        primary_bg = shift(accent, -0.35f);
    } else if (!p.is_light && acc_lum < 0.05f) {
        primary_bg = shift(accent, 0.30f);
    }
    /* Final contrast check — if still not dark/light enough, blend toward
     * a guaranteed-readable colour. */
    f32 pb_lum = chrome_luminance(primary_bg);
    if (pb_lum > 0.45f) {
        primary_bg = blend(primary_bg, (Color){0.05f, 0.05f, 0.10f, 1.0f}, 0.55f);
    }
    p.btn_primary_bg    = primary_bg;
    p.btn_primary_fg    = chrome_legible_on(primary_bg);
    p.btn_primary_hover = shift(primary_bg, p.is_light ? -0.08f : 0.08f);

    /* ---- Secondary button — neutral fill, legible text ---- */
    p.btn_secondary_bg    = p.surface_raised;
    p.btn_secondary_fg    = t->fg;
    p.btn_secondary_fg.a  = 1.0f;
    /* Ensure secondary text is legible on the secondary bg */
    {
        Color lbl = chrome_legible_on(p.surface_raised);
        /* Blend toward the forced black/white only if contrast is too low
         * (<3.0 is clearly failing); preserves subtle tint otherwise. */
        f32 raised_lum = chrome_luminance(p.surface_raised);
        f32 fg_lum     = chrome_luminance(p.btn_secondary_fg);
        f32 lighter = raised_lum > fg_lum ? raised_lum : fg_lum;
        f32 darker  = raised_lum < fg_lum ? raised_lum : fg_lum;
        f32 contrast_ratio = (lighter + 0.05f) / (darker + 0.05f);
        if (contrast_ratio < 3.0f) {
            p.btn_secondary_fg = lbl;
        }
    }
    p.btn_secondary_hover = shift(p.surface_raised, p.is_light ? -0.05f : 0.05f);

    /* ---- Destructive ---- */
    if (p.is_light) {
        /* On light themes darker red on a light-red tint reads better. */
        p.btn_destructive_bg = (Color){0.90f, 0.30f, 0.30f, 0.12f};
        p.btn_destructive_fg = (Color){0.70f, 0.10f, 0.10f, 1.0f};
    } else {
        p.btn_destructive_bg = (Color){0.95f, 0.30f, 0.30f, 0.14f};
        p.btn_destructive_fg = (Color){0.95f, 0.55f, 0.55f, 1.0f};
    }

    /* ---- Dividers — derived from border colour ---- */
    p.divider_subtle = (Color){ t->border.r, t->border.g, t->border.b, 0.45f };
    p.divider_strong = (Color){ t->border.r, t->border.g, t->border.b, 1.0f  };
    /* If the theme's border is identical to bg (some themes don't set it),
     * synthesise a sensible divider from fg instead. */
    if (fabsf(t->border.r - t->bg.r) < 0.01f &&
        fabsf(t->border.g - t->bg.g) < 0.01f &&
        fabsf(t->border.b - t->bg.b) < 0.01f) {
        Color synth = blend(t->bg, t->fg, 0.20f);
        p.divider_subtle = (Color){ synth.r, synth.g, synth.b, 0.45f };
        p.divider_strong = (Color){ synth.r, synth.g, synth.b, 0.85f };
    }

    return p;
}
