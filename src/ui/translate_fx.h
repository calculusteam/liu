/*
 * Liu — the prompt while it is being translated.
 *
 * Two phases, both painted INTO the prompt's own cells (the row builder asks
 * translate_fx_paint_cell for each glyph's colour; nothing is drawn over the
 * text, so ligatures, bold faces and a translucent window stay what they were):
 *
 *   WAIT    from the second Ctrl+Cmd until the first translated byte: the
 *           prompt stays where it is, stepped back to a third of its contrast,
 *           and a light passes over it from left to right every 0.55 s.
 *   ARRIVE  the translation goes in in the accent colour, and a tenth of a
 *           second after its last byte it settles to its own colour over half
 *           a second.
 *
 * The waiting prompt is the theme's `faint` with a gradient three
 * prompt-widths long sliding over it; the arriving one is
 * `mix(accent, fg, ramp(t, end + 0.1, 0.5, smooth))`. Colours are mixed in
 * display space, as CSS mixes them; the row builder's colours are sRGB until
 * they reach the GPU, so the same arithmetic gives the same colours.
 *
 * Pure: no AppState, no renderer, no clock of its own.
 */
#ifndef UI_TRANSLATE_FX_H
#define UI_TRANSLATE_FX_H

#include "core/types.h"

typedef enum {
    TRANSLATE_FX_NONE = 0,
    TRANSLATE_FX_WAIT,
    TRANSLATE_FX_ARRIVE,
} TranslateFxPhase;

/* How far the waiting text steps toward its background. A `faint` of #55555C
 * sits 0.316 of the way from its background (#0A0A0B) to its ink (#F7F7F8): the
 * text keeps that much of its contrast and gives up the rest. */
#define TRANSLATE_FX_DIM           0.684f
/* One pass of the light, in seconds. */
#define TRANSLATE_FX_SWEEP_SEC     0.55
/* The light's half-width, in prompt widths: the gradient is three widths
 * long and its bright part runs from 35% to 65% of it. */
#define TRANSLATE_FX_LIGHT_HALF    0.45f
/* After the last translated byte: this long in the accent, then this long to settle. */
#define TRANSLATE_FX_SETTLE_DELAY  0.10
#define TRANSLATE_FX_SETTLE_SEC    0.50

typedef struct {
    TranslateFxPhase phase;
    f64  since;      /* when the phase began */
    f64  last_byte;  /* ARRIVE: when the last translated byte went in */
    bool streaming;  /* ARRIVE: the translation is not finished; more may come */
} TranslateFx;

/* ---- the state's four moves ---------------------------------------------- */
static inline void translate_fx_wait(TranslateFx *fx, f64 now) {
    *fx = (TranslateFx){ .phase = TRANSLATE_FX_WAIT, .since = now };
}
/* The first translated bytes are going in: the original is gone. */
static inline void translate_fx_arrive(TranslateFx *fx, f64 now) {
    *fx = (TranslateFx){ .phase = TRANSLATE_FX_ARRIVE, .since = now, .last_byte = now, .streaming = true };
}
/* More of it went in. */
static inline void translate_fx_byte(TranslateFx *fx, f64 now) {
    if (fx->phase == TRANSLATE_FX_ARRIVE) fx->last_byte = now;
}
/* The translation is whole: it settles from its last byte. A translation that
 * ends without ever having arrived (a failure, a cancel while waiting) has
 * nothing to settle and stops at once. */
static inline void translate_fx_end(TranslateFx *fx, bool arrived_whole) {
    if (fx->phase == TRANSLATE_FX_ARRIVE && arrived_whole) fx->streaming = false;
    else *fx = (TranslateFx){0};
}
static inline void translate_fx_stop(TranslateFx *fx) { *fx = (TranslateFx){0}; }

/* ---- the curves ----------------------------------------------------------- */
static inline f32 translate_fx_clamp01(f32 x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* Where the middle of the light is, in prompt widths, `elapsed` seconds into
 * the wait: it comes in from half a width before the text and leaves half a
 * width after it, once every TRANSLATE_FX_SWEEP_SEC. */
static inline f32 translate_fx_light_centre(f64 elapsed) {
    if (elapsed < 0.0) elapsed = 0.0;
    f64 turns = elapsed / TRANSLATE_FX_SWEEP_SEC;
    f64 x = turns - (f64)(i64)turns;          /* 0 … 1 through one pass */
    return (f32)(2.0 * x - 0.5);
}
/* How much of the light falls at `u` (0 at the text's left edge, 1 at its right). */
static inline f32 translate_fx_light(f32 u, f32 centre) {
    f32 d = u - centre;
    if (d < 0.0f) d = -d;
    return translate_fx_clamp01(1.0f - d / TRANSLATE_FX_LIGHT_HALF);
}
/* How much of the accent the arriving text still wears: 1 while it streams
 * and through the delay, then down to 0 on a smoothstep. */
static inline f32 translate_fx_accent(const TranslateFx *fx, f64 now) {
    if (fx->phase != TRANSLATE_FX_ARRIVE) return 0.0f;
    if (fx->streaming) return 1.0f;
    f32 x = translate_fx_clamp01((f32)((now - fx->last_byte - TRANSLATE_FX_SETTLE_DELAY) / TRANSLATE_FX_SETTLE_SEC));
    return 1.0f - x * x * (3.0f - 2.0f * x);
}
/* Is the effect still on at `now`? False once the accent has settled: the
 * caller stops it then (translate_fx_stop). */
static inline bool translate_fx_running(const TranslateFx *fx, f64 now) {
    if (fx->phase == TRANSLATE_FX_WAIT) return true;
    if (fx->phase != TRANSLATE_FX_ARRIVE) return false;
    return fx->streaming || now - fx->last_byte < TRANSLATE_FX_SETTLE_DELAY + TRANSLATE_FX_SETTLE_SEC;
}
/* Does it change with the CLOCK right now, and so owe frames of its own? The
 * light does, and the settle does. Text that is still streaming in does not:
 * it is all accent until its last byte, and every byte that arrives is output,
 * which is a frame already. */
static inline bool translate_fx_on_clock(const TranslateFx *fx, f64 now) {
    if (fx->phase == TRANSLATE_FX_WAIT) return true;
    return fx->phase == TRANSLATE_FX_ARRIVE && !fx->streaming && translate_fx_running(fx, now);
}

/* ---- what the row builder is handed --------------------------------------- */
/* The prompt's cells, as the terminal's cmdline span has them: the first cell
 * inclusive, the end exclusive, and the body columns of the rows in between. */
typedef struct {
    i32 r0, c0;        /* first cell of the text */
    i32 r1, c1;        /* one past its last cell */
    i32 left, right;   /* columns [left, right) of a row that is neither first nor last */
} TranslateFxSpan;

typedef struct { f32 r, g, b, a; } TranslateFxColor;

/* One frame of the effect in one pane. `phase` zero: paint nothing. */
typedef struct {
    u8  phase;                 /* TranslateFxPhase */
    TranslateFxSpan span;
    i32 u_left, u_right;       /* the columns the light crosses: the text's own extent */
    f32 centre;                /* WAIT: translate_fx_light_centre */
    f32 accent_amount;         /* ARRIVE: translate_fx_accent */
    TranslateFxColor accent;
} TranslateFxPaint;

static inline TranslateFxPaint translate_fx_paint(const TranslateFx *fx, const TranslateFxSpan *span,
                                                  f64 now, TranslateFxColor accent) {
    TranslateFxPaint p = {0};
    if (!translate_fx_running(fx, now) || !span || span->r1 < span->r0) return p;
    p.phase = (u8)fx->phase;
    p.span = *span;
    /* A one-row prompt is crossed from its first cell to its last; a prompt of
     * several rows is crossed as a block, from its leftmost column to its
     * rightmost, so the light is one band over all of its rows. */
    if (span->r0 == span->r1) {
        p.u_left = span->c0;
        p.u_right = span->c1;
    } else {
        p.u_left = span->c0 < span->left ? span->c0 : span->left;
        p.u_right = span->right > span->c1 ? span->right : span->c1;
    }
    if (p.u_right <= p.u_left) p.u_right = p.u_left + 1;
    /* Each phase carries only what it paints with, so that two frames which
     * paint alike ARE alike (translate_fx_paint_same): the light's place means
     * nothing to arriving text, and would differ every frame. */
    if (fx->phase == TRANSLATE_FX_WAIT) {
        p.centre = translate_fx_light_centre(now - fx->since);
    } else {
        p.accent_amount = translate_fx_accent(fx, now);
        p.accent = accent;
    }
    return p;
}

/* Would two frames paint the same thing? The row builder's output for the
 * prompt's rows depends on nothing else of the effect, so rows built with an
 * equal paint need not be built again. */
static inline bool translate_fx_paint_same(const TranslateFxPaint *a, const TranslateFxPaint *b) {
    return a->phase == b->phase &&
           a->span.r0 == b->span.r0 && a->span.c0 == b->span.c0 &&
           a->span.r1 == b->span.r1 && a->span.c1 == b->span.c1 &&
           a->span.left == b->span.left && a->span.right == b->span.right &&
           a->u_left == b->u_left && a->u_right == b->u_right &&
           a->centre == b->centre && a->accent_amount == b->accent_amount &&
           a->accent.r == b->accent.r && a->accent.g == b->accent.g && a->accent.b == b->accent.b;
}

/* Is (row, col) one of the prompt's cells? */
static inline bool translate_fx_paint_has(const TranslateFxPaint *p, i32 row, i32 col) {
    if (!p->phase || row < p->span.r0 || row > p->span.r1) return false;
    i32 from = row == p->span.r0 ? p->span.c0 : p->span.left;
    i32 to   = row == p->span.r1 ? p->span.c1 : p->span.right;
    return col >= from && col < to;
}
static inline bool translate_fx_paint_row(const TranslateFxPaint *p, i32 row) {
    return p->phase && row >= p->span.r0 && row <= p->span.r1;
}

static inline TranslateFxColor translate_fx_mix(TranslateFxColor a, TranslateFxColor b, f32 t) {
    return (TranslateFxColor){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a };
}

/* The colour a glyph of the prompt is drawn in: `fg` is its own, `bg` what it
 * sits on. Alpha is the glyph's own throughout. */
static inline TranslateFxColor translate_fx_paint_cell(const TranslateFxPaint *p, i32 col,
                                                       TranslateFxColor fg, TranslateFxColor bg) {
    if (p->phase == TRANSLATE_FX_WAIT) {
        f32 u = ((f32)(col - p->u_left) + 0.5f) / (f32)(p->u_right - p->u_left);
        f32 light = translate_fx_light(u, p->centre);
        return translate_fx_mix(fg, bg, TRANSLATE_FX_DIM * (1.0f - light));
    }
    if (p->phase == TRANSLATE_FX_ARRIVE && p->accent_amount > 0.0f) {
        TranslateFxColor accent = p->accent;
        accent.a = fg.a;
        return translate_fx_mix(fg, accent, p->accent_amount);
    }
    return fg;
}

#endif /* UI_TRANSLATE_FX_H */
