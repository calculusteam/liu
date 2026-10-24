/*
 * A file drag that dwells over an inactive tab switches to it after the delay.
 * The drop targets the tab under the pointer, so retargeting restarts the
 * timer and a drag passing across tabs does not activate them.
 */
#ifndef UI_DRAG_SPRING_H
#define UI_DRAG_SPRING_H

#include "core/types.h"

/* How long the drag has to rest before the tab changes.
 *
 * The window is bounded on both sides. Below it: a sweep across the strip
 * spends roughly a pill's width divided by the drag speed on each tab — ~150
 * ms at a 180 px pill and a deliberate 1200 px/s — so anything under ~0.3 s
 * starts firing on pass-through. Above it: the aim is to reach a tab
 * mid-gesture, so the wait must stay short enough that the feature does not
 * seem absent. 0.55 s is the usual spring-loading delay on macOS. */
#define DRAG_SPRING_DELAY_SEC 0.55

typedef enum {
    DRAG_SPRING_NONE = 0,   /* nothing under the pointer worth springing to */
    DRAG_SPRING_TAB,        /* a tab that is not already the active one */
} DragSpringKind;

typedef struct {
    DragSpringKind kind;    /* NONE = disarmed */
    i32            index;
    f64            since;   /* when this target was first seen */
} DragSpringState;

/* Advance the dwell one tick.
 *
 * `kind`/`index` describe what is under the pointer RIGHT NOW —
 * DRAG_SPRING_NONE covers every "nothing to do" case together: no drag in
 * progress, the pointer off the tab bar, over the active tab, or over a tab
 * that must not be sprung to. The caller does the resolving; this only counts.
 *
 * Returns true on exactly ONE tick, the one where the dwell completes, and
 * disarms itself as it does. That matters because the caller acts on the
 * return value: left armed, a target whose state the action did not change
 * would fire again on every following tick.
 *
 * Called from the frame loop rather than from the drag events, because the
 * pointer is STANDING STILL for the whole interval that matters — the thing
 * being measured is precisely the absence of input. */
static inline bool drag_spring_tick(DragSpringState *st, DragSpringKind kind,
                                    i32 index, f64 now) {
    if (!st) return false;
    if (kind == DRAG_SPRING_NONE) {
        st->kind = DRAG_SPRING_NONE;
        st->index = -1;
        st->since = 0.0;
        return false;
    }
    if (st->kind != kind || st->index != index) {   /* a new target restarts it */
        st->kind  = kind;
        st->index = index;
        st->since = now;
        return false;
    }
    /* A clock that went backwards restarts rather than firing instantly. */
    if (now < st->since) { st->since = now; return false; }
    if (now - st->since < DRAG_SPRING_DELAY_SEC) return false;
    st->kind  = DRAG_SPRING_NONE;
    st->index = -1;
    st->since = 0.0;
    return true;
}

/* 0..1 across the dwell, for the tab bar to draw.
 *
 * The bar is not decoration: a tab that simply changes under a drag looks like
 * a bug and gives no way to learn the rule or to pull away in time. The fill
 * says "keep still and this happens" before it happens. */
static inline f32 drag_spring_progress(const DragSpringState *st,
                                       DragSpringKind kind, i32 index, f64 now) {
    if (!st || st->kind == DRAG_SPRING_NONE) return 0.0f;
    if (st->kind != kind || st->index != index) return 0.0f;
    f64 dt = now - st->since;
    if (dt <= 0.0) return 0.0f;
    if (dt >= DRAG_SPRING_DELAY_SEC) return 1.0f;
    return (f32)(dt / DRAG_SPRING_DELAY_SEC);
}

#endif /* UI_DRAG_SPRING_H */
