/*
 * pane_drop_anim.h — how a Cmd-dragged pane's drop preview MOVES.
 *
 * The preview is not redrawn wherever the pointer's zone says: two panes
 * trading places is a movement, and it is shown as one:
 *
 *   - the frame LIFTS OFF the dragged pane: the first time the drag aims at
 *     something it starts from the pane's own rect and travels to where it
 *     would land;
 *   - a new target is a change of course, not a cut: the frame goes on from
 *     where it is, at the speed it has (a critically damped spring per edge —
 *     no overshoot, and no jerk when the pointer sweeps across three zones);
 *   - when the drag aims at nothing it goes HOME, and the preview ends when it
 *     gets there;
 *   - on a swap the other pane's picture travels the opposite way, from its
 *     own place to the dragged pane's (`swap`: 0 at home … 1 arrived), and
 *     back when the pointer leaves the middle.
 *
 * When the button comes up the layout's own move animation takes over from
 * where these stand (ui.c hands the rects over), so the drop continues the
 * movement the preview began instead of starting it again from the old places.
 *
 * Pure: no renderer, no AppState, no clock of its own.
 */
#ifndef LIU_UI_PANE_DROP_ANIM_H
#define LIU_UI_PANE_DROP_ANIM_H

#include "core/types.h"

#include <math.h>

/* Critically damped. Within 2% of its target in a fifth of a second
 * (5.8 / omega), which is the time a dropped pane's own move takes. */
#define PANE_DROP_OMEGA      28.0f
/* Rest: closer than this and slower than this (pixels, pixels a second). */
#define PANE_DROP_REST_PX    0.5f
#define PANE_DROP_REST_V     6.0f
/* The swap's travel is 0…1, not pixels. */
#define PANE_DROP_REST_UNIT  0.002f
#define PANE_DROP_REST_UNIT_V 0.02f
/* The longest step taken at once: a loop that slept is not a leap. */
#define PANE_DROP_MAX_DT     0.05
/* The step a frame at rest takes when it is sent somewhere: the time since
 * its last one is how long the pointer stood still, not time on the way. */
#define PANE_DROP_FIRST_DT   (1.0 / 60.0)

typedef struct { f32 x, y, w, h; } PaneAnimRect;
typedef struct { f32 x, v; } PaneSpring;

/* One step of a critically damped spring toward `target`, by its own solution —
 * x(t) = target + (d + c t) e^(-omega t) — and not by a numerical step: it is
 * stable at any step, never passes its target from rest, and is where the clock
 * says whatever the frame rate. A numerical step would damp the spring by the
 * length of the frame. */
static inline void pane_spring_step(PaneSpring *s, f32 target, f32 omega, f32 dt) {
    if (!(dt > 0.0f)) return;
    f32 d = s->x - target;
    f32 c = s->v + omega * d;
    f32 e = expf(-omega * dt);
    s->x = target + (d + c * dt) * e;
    s->v = (s->v - omega * c * dt) * e;
}
static inline bool pane_spring_rest(const PaneSpring *s, f32 target, f32 eps_x, f32 eps_v) {
    f32 d = s->x - target, v = s->v;
    if (d < 0.0f) d = -d;
    if (v < 0.0f) v = -v;
    return d < eps_x && v < eps_v;
}

typedef struct {
    bool live;             /* a frame is on screen: travelling, arrived, or going home */
    bool leaving;          /* going home; the preview ends when it arrives */
    PaneSpring x, y, w, h; /* the frame */
    PaneAnimRect to;       /* where it is going */
    i32  swap_pane;        /* the pane trading places with the dragged one; -1 none */
    PaneSpring swap;       /* 0 at its own place … 1 at the dragged pane's */
    f32  swap_to;
    f64  last;             /* when the last step was taken; 0 before the first */
} PaneDropAnim;

static inline void pane_drop_anim_cancel(PaneDropAnim *a) {
    *a = (PaneDropAnim){ .swap_pane = -1 };
}

/* The drag aims at `target`. `home` is the dragged pane's own rect, where a
 * frame that is not on screen yet starts from. `swap_pane` is the pane it
 * would trade places with, or -1 when the drop is a move to a side. */
static inline void pane_drop_anim_aim(PaneDropAnim *a, PaneAnimRect target, PaneAnimRect home, i32 swap_pane) {
    if (!a->live) {
        *a = (PaneDropAnim){ .live = true, .swap_pane = -1,
                             .x = { home.x, 0 }, .y = { home.y, 0 }, .w = { home.w, 0 }, .h = { home.h, 0 } };
    }
    a->leaving = false;
    a->to = target;
    if (swap_pane >= 0) {
        /* Another pane than the one that was on its way: that one's picture
         * is dropped where it is and this one starts from its own place. */
        if (a->swap_pane != swap_pane) a->swap = (PaneSpring){ 0, 0 };
        a->swap_pane = swap_pane;
        a->swap_to = 1.0f;
    } else {
        a->swap_to = 0.0f;       /* whoever was on its way goes back */
    }
}

/* The drag aims at nothing: the frame goes home, and so does the other pane. */
static inline void pane_drop_anim_release(PaneDropAnim *a, PaneAnimRect home) {
    if (!a->live) return;
    a->leaving = true;
    a->to = home;
    a->swap_to = 0.0f;
}

/* Advance to `now`. True while anything is still moving: the caller owes it
 * frames for that long, and none after. */
static inline bool pane_drop_anim_step(PaneDropAnim *a, f64 now) {
    if (!a->live) return false;
    f64 dt64 = a->last > 0.0 ? now - a->last : 0.0;
    a->last = now;
    if (dt64 < 0.0) dt64 = 0.0;
    if (dt64 > PANE_DROP_MAX_DT) dt64 = PANE_DROP_MAX_DT;
    bool still = a->x.v == 0.0f && a->y.v == 0.0f && a->w.v == 0.0f && a->h.v == 0.0f && a->swap.v == 0.0f;
    if (still && dt64 > PANE_DROP_FIRST_DT) dt64 = PANE_DROP_FIRST_DT;
    f32 dt = (f32)dt64;
    pane_spring_step(&a->x, a->to.x, PANE_DROP_OMEGA, dt);
    pane_spring_step(&a->y, a->to.y, PANE_DROP_OMEGA, dt);
    pane_spring_step(&a->w, a->to.w, PANE_DROP_OMEGA, dt);
    pane_spring_step(&a->h, a->to.h, PANE_DROP_OMEGA, dt);
    pane_spring_step(&a->swap, a->swap_to, PANE_DROP_OMEGA, dt);

    bool frame_rest = pane_spring_rest(&a->x, a->to.x, PANE_DROP_REST_PX, PANE_DROP_REST_V) &&
                      pane_spring_rest(&a->y, a->to.y, PANE_DROP_REST_PX, PANE_DROP_REST_V) &&
                      pane_spring_rest(&a->w, a->to.w, PANE_DROP_REST_PX, PANE_DROP_REST_V) &&
                      pane_spring_rest(&a->h, a->to.h, PANE_DROP_REST_PX, PANE_DROP_REST_V);
    bool swap_rest = pane_spring_rest(&a->swap, a->swap_to, PANE_DROP_REST_UNIT, PANE_DROP_REST_UNIT_V);
    if (frame_rest) {
        a->x = (PaneSpring){ a->to.x, 0 }; a->y = (PaneSpring){ a->to.y, 0 };
        a->w = (PaneSpring){ a->to.w, 0 }; a->h = (PaneSpring){ a->to.h, 0 };
    }
    if (swap_rest) {
        a->swap = (PaneSpring){ a->swap_to, 0 };
        if (a->swap_to == 0.0f) a->swap_pane = -1;      /* it is home: nobody is travelling */
    }
    if (frame_rest && swap_rest && a->leaving) {
        pane_drop_anim_cancel(a);                       /* home: the preview is over */
        return false;
    }
    return !(frame_rest && swap_rest);
}

static inline PaneAnimRect pane_drop_anim_rect(const PaneDropAnim *a) {
    return (PaneAnimRect){ a->x.x, a->y.x, a->w.x, a->h.x };
}
/* How far the other pane's picture has travelled, 0…1. */
static inline f32 pane_drop_anim_swap(const PaneDropAnim *a) {
    f32 s = a->swap.x;
    return s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
}
static inline PaneAnimRect pane_anim_rect_lerp(PaneAnimRect a, PaneAnimRect b, f32 t) {
    return (PaneAnimRect){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t };
}

#endif /* LIU_UI_PANE_DROP_ANIM_H */
