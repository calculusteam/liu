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
 * something is animating — a WAKE question, which is genuinely process-wide:
 * the loop is one loop, and it must wake for any window's animation. */
bool anim_global_active(void);
/* Internal: bumps the global "active until" timestamp. */
void anim_register_until(f64 end_time);

/* Low-rate variant for continuous visuals that look fine at ~30 Hz (the
 * agent-working tab accent). main.c wakes at a soft cadence while this is
 * active rather than the full FRAME_DT_BURST pin used by anim_global_active. */
bool anim_soft_active(void);
void anim_register_soft_until(f64 end_time);

/* WHOSE animation it is — the PAINT question, which is not the wake question.
 *
 * "Wake the loop" and "repaint this window" are different lists (main.c keeps
 * three of them in step). Waking is process-wide; painting is not, and
 * answering it with anim_global_active() would make every visible window
 * repaint for the length of ANY window's hover fade, modal or palette slide,
 * at the burst rate the same deadline pins the ladder to.
 *
 * A registration is attributed to the current hover scope, which main.c keeps
 * pointed at the window whose code is running (LIU_SERVICE_PTR). A
 * registration made with no scope set, or one that cannot be given a slot,
 * lands in an unscoped deadline that answers true for EVERY window — so the
 * failure mode of attribution is an extra repaint, never a frozen animation.
 * `owner` is hashed, never dereferenced. */
bool anim_active_for(const void *owner);
bool anim_soft_active_for(const void *owner);

/* -------------------------------------------------------------------------
 * Universal hover easing.
 *
 * Call once per clickable element per frame with its rect and the current
 * pointer position; returns a smoothly eased [0..1] "hoveredness" that fades
 * IN on enter and OUT on leave. Backed by a small global slot pool keyed by
 * rect position, so many elements ease independently and only the one or two
 * in-flight at a time consume a slot. Keeps the render loop awake (full rate)
 * while any slot is mid-fade so the transition plays even when the pointer is
 * still. Feed the result into ui_hover_mix()/a wash alpha at the draw site.
 *
 * This is the single source of truth for hover feel across the whole UI —
 * toolbar buttons, settings cards, dialog buttons, list rows all share it.
 * (Colour blending for the wash lives in renderer.h as color_lerp().) */
f32 ui_hover_amount(f32 x, f32 y, f32 w, f32 h, f32 px, f32 py);

/* -------------------------------------------------------------------------
 * Auto-hiding overlay scrollbars.
 *
 * A scrollbar is feedback, not chrome: it belongs on screen while the pointer
 * is over the surface it scrolls (or right after that surface moved), and it
 * should be gone otherwise. Callers keep one f64 timestamp per surface, stamp
 * it with platform_time_sec() whenever the surface is hovered or its offset
 * changes, and paint with the alpha this returns — solid for the hold, then a
 * short ramp to zero.
 *
 * It also keeps the loop awake across hold + fade. Without that, a pointer
 * that leaves the window with nothing else animating leaves the bar frozen on
 * screen until some unrelated redraw happens. The hold registers at the soft
 * (~30 Hz) rate — the bar is static, the loop only needs to be awake when it
 * expires — and the fade at the full rate, because an alpha ramp wants frames.
 * ------------------------------------------------------------------------- */
#define UI_AUTOHIDE_HOLD_S  2.2
#define UI_AUTOHIDE_FADE_S  0.45
/* `active_now` = the surface is hovered/moving this frame (caller has just
 * stamped last_active); it short-circuits to full opacity and registers
 * nothing, since something is already driving frames. */
f32 ui_autohide_alpha(f64 last_active, bool active_now);

/* Which window the following ui_hover_amount() calls belong to.
 *
 * The slot pool is keyed by a hash of the element's rect, and a rect is
 * WINDOW-LOCAL: two same-sized windows hash the toolbar's first button to the
 * same slot. Every window is painted in one loop iteration (background first,
 * focused last), so without a scope the background pass would consume the
 * frame's whole dt, the focused window's call would arrive with dt≈0 and its
 * target never take, and the background pass would then trip the release path
 * — hover feedback would die app-wide. Salting the key with the window makes
 * the pools disjoint.
 *
 * Call once per window immediately before that window's render pass, with any
 * stable per-window token (the AppState* is the obvious one). `owner` is never
 * dereferenced — only hashed — so a recycled address costs at most one
 * mis-shared fade, never a use-after-free. Never calling it at all leaves the
 * scope at 0, i.e. exactly the single-window behaviour. */
void anim_set_hover_scope(const void *owner);

/* Pointer-damage registry for determining whether pointer movement can change
 * the rendered hover state.
 *
 * ui_hover_amount records each control's rectangle and the pointer position
 * used for its last draw. If the new pointer position falls within the same
 * set of rectangles, no hover state changed and another frame would be
 * identical. Skipping that frame avoids repainting when pointer events arrive
 * faster than display refresh.
 *
 * The damage check errs toward repainting: an overflowed or uninitialized
 * registry reports damage. Register controls before the early return for
 * inactive hover states so pointer entry is detected. Entries carry a hover
 * scope so one window's pass cannot answer for another; ui_hover_frame_begin
 * clears only the current scope. */

/* Start recording a fresh set for the current hover scope. Called once per
 * window per paint, at the top of app_render. */
void ui_hover_frame_begin(void);

/* True if moving the pointer to (px,py) would change the hoveredness of any
 * control recorded for the current scope — i.e. the frame must be repainted.
 * Also true when the registry holds nothing (nothing painted yet) or overflowed.
 * Pure: it never mutates the fade pool, so asking is free of side effects. */
bool ui_hover_pointer_damage(f32 px, f32 py);

/* Plain containment, recorded in the same registry — for the controls that
 * highlight on hover but do NOT fade.
 *
 * ui_hover_amount() is the registry's main feeder because every fading control
 * already calls it. A hard-switching one (a tab's close button, the "+" zone,
 * the toolbar chips, the status-bar segments, a divider's grab strip) writes
 * its own `px >= x && px < x+w && ...` at the draw site and would therefore be
 * invisible to the gate — and invisible means its highlight stops responding
 * the moment the loop stops repainting on raw motion. This is that same test,
 * with the registry write attached, so those sites keep their exact behaviour
 * (no fade, no easing) and still hold the gate open when they need to.
 *
 * Half-open on all four edges, matching ui_hover_amount(), so a control and its
 * neighbour cannot both claim the seam. */
bool ui_hover_hit(f32 x, f32 y, f32 w, f32 h, f32 px, f32 py);

/* While paused, ui_hover_hit() and ui_hover_amount() record nothing for the
 * pointer gate. app_render pauses it for what it draws BEHIND a dialog with
 * the pointer nowhere: those controls cannot be hovered while it is up, so
 * moving over the dialog must not ask for frames on their behalf. */
void ui_hover_record_pause(bool paused);

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
 * deceleration than OUT_BACK without overshoot, which reads as jumpy at
 * shorter durations.
 *
 * Close is shorter than open (MODAL_CLOSE_RATIO of the open duration) and
 * uses a QUADRATIC ease-out curve on the alpha decay: alpha = (1-t)^2.
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
/* Close duration as a fraction of open duration. A close that reads as "the
 * panel snaps shut" comes from the alpha/motion curves (below), not from the
 * duration; with the curves right, a longer close only makes the exit feel
 * sluggish. */
#define MODAL_CLOSE_RATIO     0.78
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

/* Alpha for a modal's CONTENT, given the alpha of its shell.
 *
 * A modal that fades its shell and its text on the SAME curve is unreadable in
 * the middle of the transition, and the settings panel is the worst case: its
 * fill is theme->bg, the very colour of the terminal behind it. At alpha 0.5 the
 * fill hides nothing, so the user sees the panel's labels and the terminal's
 * text at equal strength, interleaved — which reads as a corrupted frame rather
 * than as a fade. Dropped frames make it worse, because a wall-clock animation
 * then holds that state for two or three refreshes instead of one.
 *
 * Cubing staggers the two: content is at 0.35 when the shell is still at 0.70,
 * so by the time the shell stops covering the terminal its text has already
 * gone. It needs no direction flag — on open the shell arrives first and the
 * content follows, which is the same stagger run backwards — and it is exactly
 * 1.0 at rest, so nothing changes once the modal is up. */
static inline f32 modal_content_alpha(f32 shell_alpha) {
    if (shell_alpha <= 0.0f) return 0.0f;
    if (shell_alpha >= 1.0f) return 1.0f;
    return shell_alpha * shell_alpha * shell_alpha;
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
            /* Reopen mid-close: start the open over rather than blink to
             * alpha=0 first. anim_start always reads platform_time_sec(),
             * so the open curve cannot easily resume at the same alpha —
             * but the close is SHORT, so reopens land in lockstep without
             * feeling jumpy. */
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
        /* Close: quadratic ease-out on the *alpha decay*, alpha =
         * (1-t)^2 (see the tuning comment above MODAL_OPEN_DUR_FAST).
         * The horizontal tangent at the end means no visible "snap" on
         * the last frame.
         *
         * Scale and Y-slide use a SEPARATE eased progress (raw with
         * EASE_OUT_CUBIC) instead of being slaved to alpha, so the
         * geometry moves along its own curve. */
        f32 raw = anim_progress(close_anim);
        if (anim_done(close_anim)) {
            if (out_scale)    *out_scale    = MODAL_OPEN_SCALE_FROM;
            if (out_alpha)    *out_alpha    = 0.0f;
            if (out_y_offset) *out_y_offset = MODAL_OPEN_Y_OFFSET * dpi;
        } else {
            /* Quadratic alpha and a front-loaded motion curve: with
             * EASE_OUT_CUBIC most of the travel happens while the panel is
             * still visible. With the motion back-loaded, the movement would
             * happen after the panel had faded out and the exit would read as
             * a stationary dissolve that "hangs" and then vanishes. */
            f32 inv   = 1.0f - raw;
            f32 alpha = inv * inv;           /* (1-t)^2 — see the note above */
            f32 motion = ease_apply(raw, EASE_OUT_CUBIC);
            if (out_alpha)    *out_alpha    = alpha;
            if (out_scale)    *out_scale    = anim_lerp(1.0f, MODAL_OPEN_SCALE_FROM, motion);
            if (out_y_offset) *out_y_offset = motion * MODAL_OPEN_Y_OFFSET * dpi;
        }
    }
    return true;
}

#endif /* LIU_ANIM_H */
