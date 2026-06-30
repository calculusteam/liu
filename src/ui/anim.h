/*
 * Liu — minimal animation primitives
 *
 * Each animated value owns an Anim struct: anim_start() captures start time
 * and duration; anim_eased() returns the eased [0..1] progress on demand.
 * No per-frame ticks, no global animation manager — render code samples the
 * progress when drawing.
 *
 * The main loop uses anim_global_active() to keep waking until every running
 * animation has finished, so animations don't stall when the user idles.
 */
#ifndef LIU_ANIM_H
#define LIU_ANIM_H

#include "core/types.h"
#include "platform/platform.h"
#include <math.h>
#include <stdbool.h>

typedef enum {
    EASE_LINEAR,
    EASE_OUT_CUBIC,
    EASE_IN_OUT_CUBIC,
    EASE_OUT_BACK,        /* slight overshoot — pleasant for modal in */
} EaseFunc;

typedef struct {
    f64  start_time;
    f64  duration;
    bool active;
} Anim;

/* Begin an animation. Safe to re-start an already-running animation. */
void anim_start(Anim *a, f64 duration_sec);
/* Reverse-direction start: sets duration but with `active=false` so the
 * eased value reads 0 immediately. Used when an exit animation has not yet
 * begun and the UI element should be drawn at its rest state. */
void anim_reset(Anim *a);

/* Returns whether any animation in the process is still running. main.c
 * uses this to keep the event loop awake at FRAME_DT_INTERACTIVE while
 * something is animating. */
bool anim_global_active(void);
/* Internal: bumps the global "active until" timestamp. */
void anim_register_until(f64 end_time);

/* Low-rate variant for continuous visuals that look fine at ~30 Hz (the
 * agent-working tab accent). main.c wakes at a soft cadence while this is
 * active rather than the full FRAME_DT_BURST pin used by anim_global_active. */
bool anim_soft_active(void);
void anim_register_soft_until(f64 end_time);

static inline f32 anim_progress(const Anim *a) {
    if (!a->active) return 0.0f;
    f64 t = (platform_time_sec() - a->start_time) / a->duration;
    if (t <= 0.0) return 0.0f;
    if (t >= 1.0) return 1.0f;
    return (f32)t;
}

static inline bool anim_done(const Anim *a) {
    return !a->active || (platform_time_sec() - a->start_time) >= a->duration;
}

static inline f32 ease_apply(f32 t, EaseFunc fn) {
    switch (fn) {
    case EASE_LINEAR:        return t;
    case EASE_OUT_CUBIC: {
        f32 u = 1.0f - t;
        return 1.0f - u * u * u;
    }
    case EASE_IN_OUT_CUBIC:
        return (t < 0.5f) ? (4.0f * t * t * t)
                          : (1.0f - powf(-2.0f * t + 2.0f, 3.0f) * 0.5f);
    case EASE_OUT_BACK: {
        const f32 c1 = 1.70158f;
        const f32 c3 = c1 + 1.0f;
        f32 u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }
    }
    return t;
}

static inline f32 anim_eased(const Anim *a, EaseFunc fn) {
    return ease_apply(anim_progress(a), fn);
}

static inline f32 anim_lerp(f32 from, f32 to, f32 t) {
    return from + (to - from) * t;
}

/* Modal open animation tuning. Centralised so every modal feels identical
 * — change once here and all panels (palette, settings, all 9 dialogs,
 * agent picker) animate the same way. EASE_OUT_CUBIC gives a smoother
 * deceleration than OUT_BACK without overshoot, which felt jumpy at
 * shorter durations.
 *
 * Close is shorter than open (≈70% of open duration) but uses a
 * QUADRATIC ease-out curve on the alpha decay: alpha = (1-t)^2.
 *
 * Why not linear?
 *   Linear gives a constant rate of alpha change. Human vision is
 *   logarithmic, so the eye sees alpha 1.0 → 0.5 as a *small* change
 *   and alpha 0.10 → 0 as a *huge* change. The user therefore perceives
 *   linear close as "smooth, then sudden snap at ~25% alpha" because
 *   the last several discrete frames cross the visibility threshold in
 *   one perceptible jump.
 *
 * Why not cubic?
 *   Cubic (1-t)^3 fades alpha to near-zero in the first 50% of the
 *   duration, then trails for the remaining 50% at alpha < 0.05. The
 *   trail is invisible to the user — but the panel rect technically
 *   keeps rendering, so user sees "text gone first, panel ghost lingers".
 *
 * Quadratic (1-t)^2 sits between: alpha decelerates smoothly to zero
 * with a horizontal-tangent landing, no perceptible snap and no long
 * invisible tail. */
#define MODAL_OPEN_DUR_FAST   0.22   /* small/medium dialogs */
#define MODAL_OPEN_DUR_LARGE  0.28   /* large modals (settings, transcript) */
/* Close duration as a fraction of open duration. Bumped from 0.72 → 0.88
 * because the previous close was perceptibly snappier than the open, and
 * the asymmetry read as "the panel snaps shut" rather than the smoother
 * fade-out users expected. */
#define MODAL_CLOSE_RATIO     0.88
#define MODAL_OPEN_EASE       EASE_OUT_CUBIC
#define MODAL_OPEN_SCALE_FROM 0.96f
/* dpi-units; multiply by dpi at site. Bumped from 8 → 10 so the slide
 * is visible on standard retina without being noisy on small modals. */
#define MODAL_OPEN_Y_OFFSET   10.0f

/* Resolve the scale, alpha and Y-slide values for a modal's current frame.
 * All three are eased identically so position, size and opacity move in
 * lockstep — that's what reads as "smooth" rather than "snappy". */
static inline void modal_open_progress(const Anim *a, f32 dpi,
                                        f32 *out_scale, f32 *out_alpha,
                                        f32 *out_y_offset) {
    f32 raw = anim_progress(a);
    f32 e   = ease_apply(raw, MODAL_OPEN_EASE);
    if (anim_done(a)) {
        if (out_scale)    *out_scale    = 1.0f;
        if (out_alpha)    *out_alpha    = 1.0f;
        if (out_y_offset) *out_y_offset = 0.0f;
        return;
    }
    if (out_scale)    *out_scale    = anim_lerp(MODAL_OPEN_SCALE_FROM, 1.0f, e);
    if (out_alpha)    *out_alpha    = e;
    if (out_y_offset) *out_y_offset = (1.0f - e) * MODAL_OPEN_Y_OFFSET * dpi;
}

/* Bidirectional modal animation gate.
 *
 * Drives both the open and close transitions for a modal whose visibility
 * is owned by `want_open`. The caller passes in two persistent Anim
 * structs (one for opening, one for closing) plus a `was_open` tracker
 * — this function detects the rising/falling edge and starts the right
 * animation, then returns:
 *   - render?      → whether the modal should still draw this frame
 *                    (true while active OR while closing anim runs)
 *   - out_scale    → 0.94 ↔ 1.0 (eased in OUT_CUBIC, never overshoots)
 *   - out_alpha    → 0   ↔ 1   (paired with scale)
 *   - out_y_offset → 10dpi ↔ 0 (slides up on open, drops down on close)
 *
 * Shared duration tunables in anim.h keep every modal feeling identical.
 * On open, want_open flips true and *was_open becomes true. The close
 * path (want_open=false but the close anim hasn't drained yet) keeps the
 * caller drawing the modal so the dismissal isn't a hard cut. */
static inline bool modal_anim_progress(bool want_open,
                                        Anim *open_anim,
                                        Anim *close_anim,
                                        bool *was_open,
                                        f32 dpi,
                                        f32 duration,
                                        f32 *out_scale,
                                        f32 *out_alpha,
                                        f32 *out_y_offset) {
    if (want_open != *was_open) {
        *was_open = want_open;
        if (want_open) {
            /* Reopen mid-close: start the open from wherever we were so
             * we don't blink to alpha=0 first. anim_start always reads
             * platform_time_sec(), so we can't easily resume the open
             * curve at the same alpha — but since we use a SHORT close
             * (~120ms), reopens land in lockstep without feeling jumpy. */
            anim_start(open_anim, duration);
            anim_reset(close_anim);
        } else {
            /* Close runs at MODAL_CLOSE_RATIO * open duration so the
             * dismissal feels snappy. Linear ease (below) keeps alpha
             * and scale moving in lockstep — text and panel disappear
             * together instead of text vanishing first. */
            anim_start(close_anim, duration * MODAL_CLOSE_RATIO);
            anim_reset(open_anim);
        }
    }

    bool render = want_open || !anim_done(close_anim);
    if (!render) {
        if (out_scale)    *out_scale    = 1.0f;
        if (out_alpha)    *out_alpha    = 0.0f;
        if (out_y_offset) *out_y_offset = 0.0f;
        return false;
    }

    if (want_open) {
        modal_open_progress(open_anim, dpi, out_scale, out_alpha, out_y_offset);
    } else {
        /* Close: cubic ease-out on the *alpha decay*, alpha = (1-t)^3.
         *
         * Cubic gives a longer tail than the previous quadratic curve:
         * the panel becomes near-transparent quickly, then drifts the
         * last 10–15% of opacity over several frames so the dismissal
         * settles into the background instead of clipping off. The
         * horizontal tangent at the end means no visible "snap" on the
         * last frame.
         *
         * Scale and Y-slide use a SEPARATE eased progress (raw with
         * EASE_IN_OUT_CUBIC) instead of being slaved to alpha. The
         * previous coupling — scale = lerp(1, X, 1-alpha) — held the
         * scale near 1.0 during most of the close because alpha drops
         * slowly under (1-t)^N. Decoupling lets the geometry slide
         * along its own curve, giving the motion more presence. */
        f32 raw = anim_progress(close_anim);
        if (anim_done(close_anim)) {
            if (out_scale)    *out_scale    = MODAL_OPEN_SCALE_FROM;
            if (out_alpha)    *out_alpha    = 0.0f;
            if (out_y_offset) *out_y_offset = MODAL_OPEN_Y_OFFSET * dpi;
        } else {
            f32 inv   = 1.0f - raw;
            f32 alpha = inv * inv * inv;     /* (1-t)^3 — cubic ease-out */
            f32 motion = ease_apply(raw, EASE_IN_OUT_CUBIC);
            if (out_alpha)    *out_alpha    = alpha;
            if (out_scale)    *out_scale    = anim_lerp(1.0f, MODAL_OPEN_SCALE_FROM, motion);
            if (out_y_offset) *out_y_offset = motion * MODAL_OPEN_Y_OFFSET * dpi;
        }
    }
    return true;
}

#endif /* LIU_ANIM_H */
