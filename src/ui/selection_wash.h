/*
 * Selection alpha is adjusted per theme using linear-light contrast, matching
 * the renderer's blend space. Raise alpha until the washed cell meets
 * `SELW_VISIBLE_CR`, capped at `SELW_MAX_A`; then reduce it as needed to keep
 * text contrast at `min(4.5, original contrast)`, without dropping below the
 * visibility threshold. Blank cells use half the text wash, raised to
 * `SELW_BLANK_CR`. Only alpha changes; the theme hue and glyph colors remain
 * unchanged.
 */
#ifndef LIU_SELECTION_WASH_H
#define LIU_SELECTION_WASH_H

#include "core/types.h"
#include "core/config.h"   /* Color */
#include <math.h>

#define SELW_VISIBLE_CR 1.25f   /* selected text cell vs the plain background */
#define SELW_BLANK_CR   1.10f   /* selected BLANK cell vs the plain background */
#define SELW_LEGIBLE_CR 4.50f   /* WCAG AA body text — the most that is required */
#define SELW_MIN_A      0.05f
#define SELW_MAX_A      0.85f
#define SELW_STEP       0.01f

typedef struct {
    f32 text_a;    /* alpha over cells that carry a glyph */
    f32 blank_a;   /* alpha over empty cells (space / NUL) */
} SelectionWash;

static inline f32 selw_lin(f32 c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static inline f32 selw_lum(Color c) {
    return 0.2126f * selw_lin(c.r) + 0.7152f * selw_lin(c.g) + 0.0722f * selw_lin(c.b);
}

static inline f32 selw_cr(f32 a, f32 b) {
    f32 hi = a > b ? a : b, lo = a > b ? b : a;
    return (hi + 0.05f) / (lo + 0.05f);
}

/* Luminance of `sel` composited at alpha `a` over a background of luminance
 * `lb`. Luminance is linear in linear RGB, so the mix is the mix of the two. */
static inline f32 selw_mix(f32 lb, f32 ls, f32 a) {
    return lb * (1.0f - a) + ls * a;
}

static inline SelectionWash selection_wash(Color sel, Color bg, Color fg) {
    f32 lb = selw_lum(bg), ls = selw_lum(sel), lf = selw_lum(fg);
    f32 a = sel.a;
    if (!(a >= SELW_MIN_A)) a = SELW_MIN_A;       /* also catches NaN */
    if (a > 1.0f) a = 1.0f;

    /* Visible. */
    while (a < SELW_MAX_A && selw_cr(lb, selw_mix(lb, ls, a)) < SELW_VISIBLE_CR)
        a += SELW_STEP;
    if (a > SELW_MAX_A && sel.a <= SELW_MAX_A) a = SELW_MAX_A;

    /* Legible — back off while the text suffers, but not below visible. */
    f32 need = fminf(SELW_LEGIBLE_CR, selw_cr(lf, lb));
    while (a - SELW_STEP >= SELW_MIN_A &&
           selw_cr(lf, selw_mix(lb, ls, a)) < need &&
           selw_cr(lb, selw_mix(lb, ls, a - SELW_STEP)) >= SELW_VISIBLE_CR)
        a -= SELW_STEP;

    /* Blank cells: half the text wash, but still findable. */
    f32 b = a * 0.5f;
    while (b < a && selw_cr(lb, selw_mix(lb, ls, b)) < SELW_BLANK_CR)
        b += SELW_STEP;
    if (b > a) b = a;

    return (SelectionWash){ a, b };
}

#endif /* LIU_SELECTION_WASH_H */
