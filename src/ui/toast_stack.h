/* The floating notices' timeline: up to three on screen, newest on top, each
 * with its own kind, hold and fade. No renderer and no clock of its own: the
 * caller passes `now`.
 *
 * A notice's AGE is `now - born - held`: the clock, less what the pointer
 * kept. While the pointer rests on one its age is frozen (and pulled back
 * inside the fully-visible part, so a notice caught half-faded comes back to
 * full), for at most TOAST_HOVER_MAX_S: a pointer that left the window by its
 * edge leaves its last position behind, and that must not keep a notice for
 * good. `held` grows only on frames the window paints, so a window that stops
 * painting cannot keep a notice, or the event loop, awake: its age runs on the
 * wall clock and ends. */
#ifndef UI_TOAST_STACK_H
#define UI_TOAST_STACK_H

#include "ui/notification.h"
#include <stdio.h>

#define TOAST_MAX          3      /* shown at once; a fourth sends the oldest away */
#define TOAST_CAP          5      /* shown plus the ones still leaving */
#define TOAST_FADE_IN_S    0.20
#define TOAST_FADE_OUT_S   1.0
#define TOAST_CLOSE_S      0.16   /* a clicked notice's way out */
#define TOAST_HOVER_MAX_S  20.0   /* the most the pointer keeps one: a pointer that left
                                   * the window leaves its last position behind */
#define TOAST_FOLLOW_OMEGA 28.0f  /* a slot change settles in about a fifth of a second */

typedef struct {
    char             msg[256];
    NotificationKind kind;
    u64   about;         /* what the notice is about (0 = nothing named): a newer
                          * word on the same thing takes this notice's place */
    f64   born;          /* clock when pushed (or pushed again) */
    f64   held;          /* seconds the pointer kept it */
    f64   age_seen;      /* its age at the last tick: what a hover freezes */
    f64   closing_at;    /* 0 = staying; else the clock when it was clicked away */
    f32   closing_a;     /* its alpha at that moment */
    f32   y;             /* px below the stack's top; follows the slot it is in */
    bool  placed;        /* `y` has been given once: a new notice starts in its slot */
    f32   hx, hy, hw, hh;/* the rect it was drawn in last frame (click, extent); hw 0 = not drawn */
} ToastEntry;

typedef struct {
    ToastEntry e[TOAST_CAP];   /* e[0] is the newest */
    i32        n;
    f64        last_tick;      /* the clock at the previous tick; 0 while empty */
    f32        dt;             /* the previous tick to this one, capped, for the slot follow */
} ToastStack;

static inline f32 toast_ease_out(f32 t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    f32 u = 1.0f - t;
    return 1.0f - u * u * u;
}

static inline f32 toast_ease_in_out(f32 t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) return 4.0f * t * t * t;
    f32 u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u * 0.5f;
}

static inline void toast_stack_clear(ToastStack *s) {
    memset(s, 0, sizeof *s);
}

static inline f64 toast_entry_age(const ToastEntry *e, f64 now) {
    return now - e->born - e->held;
}

static inline f64 toast_entry_hold(const ToastEntry *e) {
    return notification_hold_s(e->kind);
}

/* Opacity: in over the first fifth of a second, out over the last second of
 * its hold — or, once clicked, out from wherever it was. */
static inline f32 toast_entry_alpha(const ToastEntry *e, f64 now) {
    if (e->closing_at > 0.0) {
        f32 t = (f32)((now - e->closing_at) / TOAST_CLOSE_S);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return e->closing_a * (1.0f - t);
    }
    f64 age = toast_entry_age(e, now);
    if (age < TOAST_FADE_IN_S)
        return toast_ease_out((f32)(age / TOAST_FADE_IN_S));
    f64 out_at = toast_entry_hold(e) - TOAST_FADE_OUT_S;
    if (age > out_at)
        return 1.0f - toast_ease_in_out((f32)((age - out_at) / TOAST_FADE_OUT_S));
    return 1.0f;
}

/* 0 = at its slide-in start, 1 = in place. A clicked notice goes back the way
 * it came. */
static inline f32 toast_entry_enter(const ToastEntry *e, f64 now) {
    if (e->closing_at > 0.0) {
        f32 t = (f32)((now - e->closing_at) / TOAST_CLOSE_S);
        return 1.0f - toast_ease_out(t);
    }
    return toast_ease_out((f32)(toast_entry_age(e, now) / TOAST_FADE_IN_S));
}

static inline bool toast_entry_expired(const ToastEntry *e, f64 now) {
    if (e->closing_at > 0.0) return now - e->closing_at >= TOAST_CLOSE_S;
    return toast_entry_age(e, now) >= toast_entry_hold(e);
}

static inline void toast_stack_remove(ToastStack *s, i32 i) {
    if (i < 0 || i >= s->n) return;
    memmove(&s->e[i], &s->e[i + 1], (usize)(s->n - i - 1) * sizeof s->e[0]);
    s->n--;
    memset(&s->e[s->n], 0, sizeof s->e[0]);
}

static inline i32 toast_stack_staying(const ToastStack *s) {
    i32 c = 0;
    for (i32 i = 0; i < s->n; i++) if (s->e[i].closing_at <= 0.0) c++;
    return c;
}

/* Starts a notice on top. The same words while one is up are the same notice:
 * it comes back to the top with its time restarted, so a held key does not
 * stack the same sentence three times. So is a notice about the same thing
 * (`about`, when not 0): "Starting web" becomes "web is running at …" in its
 * place instead of a second card under it. Returns 0 (its slot), or -1 for an
 * empty message. */
static inline i32 toast_stack_push_about(ToastStack *s, u64 about, NotificationKind kind,
                                         const char *msg, f64 now) {
    if (!s || !msg || !msg[0] || now <= 0.0) return -1;
    ToastEntry fresh;
    memset(&fresh, 0, sizeof fresh);
    for (i32 i = 0; i < s->n; i++) {
        if (s->e[i].closing_at <= 0.0 &&
            ((about != 0 && s->e[i].about == about) || strcmp(s->e[i].msg, msg) == 0)) {
            fresh = s->e[i];          /* keeps its place on screen, so it rises to the top */
            toast_stack_remove(s, i);
            fresh.closing_at = 0.0;
            fresh.held = 0.0;
            fresh.age_seen = 0.0;
            break;
        }
    }
    snprintf(fresh.msg, sizeof fresh.msg, "%s", msg);
    fresh.about = about;
    fresh.kind = kind < NOTIFY_KIND_COUNT ? kind : NOTIFY_INFO;
    fresh.born = now;
    fresh.hw = 0.0f;                  /* not drawn yet in this life */
    if (s->n == TOAST_CAP) toast_stack_remove(s, s->n - 1);
    memmove(&s->e[1], &s->e[0], (usize)s->n * sizeof s->e[0]);
    s->e[0] = fresh;
    s->n++;
    /* A fourth notice sends the oldest one away; it does not wait its turn. */
    while (toast_stack_staying(s) > TOAST_MAX) {
        for (i32 i = s->n - 1; i >= 0; i--) {
            if (s->e[i].closing_at <= 0.0) {
                s->e[i].closing_a = toast_entry_alpha(&s->e[i], now);
                s->e[i].closing_at = now;
                break;
            }
        }
    }
    return 0;
}

static inline i32 toast_stack_push(ToastStack *s, NotificationKind kind,
                                   const char *msg, f64 now) {
    return toast_stack_push_about(s, 0, kind, msg, now);
}

/* Retires the notices that have finished. Run before anything asks the stack
 * where a notice is (the click, the hover, the extent): the last frame of a
 * fade runs before its end, so without a later frame a finished notice would
 * stay in the stack with its rect — it would take the next click there, and a
 * pointer moving onto it would ask for a frame whose hover hold brings it
 * back. */
static inline void toast_stack_retire(ToastStack *s, f64 now) {
    if (!s) return;
    for (i32 i = s->n - 1; i >= 0; i--)
        if (toast_entry_expired(&s->e[i], now)) toast_stack_remove(s, i);
}

/* One frame: freezes the hovered notice, retires the finished ones and keeps
 * `dt` for the slot follow. `hovered` is an index into e[] as it is NOW, or -1. */
static inline void toast_stack_tick(ToastStack *s, f64 now, i32 hovered) {
    s->dt = 0.0f;
    if (s->last_tick > 0.0) {
        f64 d = now - s->last_tick;
        s->dt = (f32)(d < 0.0 ? 0.0 : (d > 0.1 ? 0.1 : d));
    }
    s->last_tick = now;
    for (i32 i = 0; i < s->n; i++) {
        ToastEntry *e = &s->e[i];
        if (e->closing_at > 0.0) continue;
        f64 age = toast_entry_age(e, now);
        if (i == hovered && e->held < TOAST_HOVER_MAX_S) {
            f64 limit = toast_entry_hold(e) - TOAST_FADE_OUT_S - 0.001;
            f64 keep = e->age_seen < limit ? e->age_seen : limit;
            if (keep < 0.0) keep = 0.0;
            e->held = now - e->born - keep;
            age = keep;
        }
        e->age_seen = age;
    }
    toast_stack_retire(s, now);
    if (s->n == 0) s->last_tick = 0.0;
}

/* A click on notice `i`: it leaves at once. Returns whether it was staying. */
static inline bool toast_stack_dismiss(ToastStack *s, i32 i, f64 now) {
    if (!s || i < 0 || i >= s->n || s->e[i].closing_at > 0.0) return false;
    s->e[i].closing_a = toast_entry_alpha(&s->e[i], now);
    s->e[i].closing_at = now;
    return true;
}

/* The staying notice under a point, by the rects they were last drawn in. */
/* Sends away the notice still up about `about` (0 names none). */
static inline bool toast_stack_dismiss_about(ToastStack *s, u64 about, f64 now) {
    if (!s || !about) return false;
    for (i32 i = 0; i < s->n; i++)
        if (s->e[i].about == about && s->e[i].closing_at <= 0.0) return toast_stack_dismiss(s, i, now);
    return false;
}

static inline i32 toast_stack_hit(const ToastStack *s, f32 x, f32 y) {
    for (i32 i = 0; i < s->n; i++) {
        const ToastEntry *e = &s->e[i];
        if (e->closing_at > 0.0 || e->hw <= 0.0f) continue;
        if (x >= e->hx && x < e->hx + e->hw && y >= e->hy && y < e->hy + e->hh)
            return i;
    }
    return -1;
}

/* Something is still on screen or on its way: the event loop stays awake. */
static inline bool toast_stack_active(const ToastStack *s, f64 now) {
    for (i32 i = 0; i < s->n; i++)
        if (!toast_entry_expired(&s->e[i], now)) return true;
    return false;
}

/* The stack's height from its top edge — what a card hung below it clears. */
static inline f32 toast_stack_extent(const ToastStack *s) {
    f32 ext = 0.0f;
    for (i32 i = 0; i < s->n; i++) {
        const ToastEntry *e = &s->e[i];
        if (e->hw <= 0.0f) continue;
        f32 b = e->y + e->hh;
        if (b > ext) ext = b;
    }
    return ext;
}

/* `cur` moves toward `target` as a critically damped spring does, in closed
 * form, so the rate does not depend on the frame rate. */
static inline f32 toast_follow(f32 cur, f32 target, f32 dt) {
    f32 d = target - cur;
    if (d > -0.5f && d < 0.5f) return target;
    return cur + d * (1.0f - expf(-TOAST_FOLLOW_OMEGA * dt));
}

#endif /* UI_TOAST_STACK_H */
