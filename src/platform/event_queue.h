/* Platform event FIFO: a 256-slot fast ring with low-priority coalescing.
 * Native pumps stop before saturation. One large text commit can still emit
 * more events; critical spill is retained in FIFO order until consumed, then
 * released. Only allocation failure may lose a critical event. Shutdown must
 * clear outstanding spill nodes. */
#ifndef PLATFORM_EVENT_QUEUE_H
#define PLATFORM_EVENT_QUEUE_H

#include "platform/platform.h"

#include <string.h>
#include <stdlib.h>

typedef struct PlatformEventOverflow {
    PlatformEvent event;
    struct PlatformEventOverflow *next;
} PlatformEventOverflow;

#define PLATFORM_EVENT_QUEUE_CAP 256

typedef struct {
    PlatformEvent ev[PLATFORM_EVENT_QUEUE_CAP];
    i32 read, write, count;
    PlatformEventOverflow *overflow_head, *overflow_tail;
    u32 overflow_count;

    /* Diagnostics. Monotonic for the life of the process; the frame probe
     * reads them, nothing resets them. */
    i32 high_water;         /* deepest the queue has ever been              */
    u32 coalesced;          /* folded into an existing event                */
    u32 evicted_lowpri;     /* displaced so a critical event could be kept  */
    u32 dropped_lowpri;     /* low-priority lost outright                   */
    u32 dropped_critical;   /* MUST stay 0                                  */

    /* Called for an event the queue DESTROYS without delivering it — cleared,
     * blanked for a window that is going away, evicted, or refused at push.
     * An event can own what it points at (platform/event_payload.h); without
     * this hook nothing would release that memory. Optional. */
    void (*discard)(void *ctx, PlatformEvent *e);
    void  *discard_ctx;
} PlatformEventQueue;

static inline void platform_event_queue_discard(PlatformEventQueue *q, PlatformEvent *e) {
    if (q->discard) q->discard(q->discard_ctx, e);
}

/* Where an event sits on the ladder above. */
enum {
    EVQ_PRIO_DISPOSABLE = 0,
    EVQ_PRIO_ACCUMULATING,
    EVQ_PRIO_LATEST,
    EVQ_PRIO_CRITICAL,
};

static inline i32 platform_event_priority(EventType type) {
    switch (type) {
        case EVENT_MOUSE_MOVE:
        case EVENT_DRAG_ENTER:  return EVQ_PRIO_DISPOSABLE;
        case EVENT_SCROLL:      return EVQ_PRIO_ACCUMULATING;
        case EVENT_RESIZE:
        case EVENT_DPI_CHANGE:
        case EVENT_VISIBILITY:  return EVQ_PRIO_LATEST;
        default:                return EVQ_PRIO_CRITICAL;
    }
}

/* State-style events: an older one can be folded into a newer one of the same
 * kind without losing information. */
static inline bool platform_event_is_coalescable(EventType type) {
    return platform_event_priority(type) < EVQ_PRIO_CRITICAL;
}

/* Can `dst` absorb `src`? Same type, same window, and for scroll the same
 * modifier/precision/momentum shape — summing a trackpad delta into a wheel
 * notch, or a Shift-scroll into a plain one, would change what it means. */
static inline bool platform_event_absorb(PlatformEvent *dst, const PlatformEvent *src) {
    if (dst->type != src->type) return false;
    if (dst->window != src->window) return false;   /* never fold across windows */
    switch (src->type) {
        case EVENT_SCROLL:
            if (dst->scroll.mods     != src->scroll.mods ||
                dst->scroll.precise  != src->scroll.precise ||
                dst->scroll.momentum != src->scroll.momentum) return false;
            dst->scroll.dx += src->scroll.dx;
            dst->scroll.dy += src->scroll.dy;
            return true;
        case EVENT_MOUSE_MOVE:  dst->mouse  = src->mouse;  return true;
        case EVENT_DRAG_ENTER:  dst->drag   = src->drag;   return true;
        case EVENT_RESIZE:      dst->resize = src->resize; return true;
        case EVENT_DPI_CHANGE:  dst->dpi    = src->dpi;    return true;
        /* Carries no payload — it exists only to wake the loop, so two of
         * them are the same event and keeping one is exact. */
        case EVENT_VISIBILITY:                             return true;
        default: return false;
    }
}

/* Newest-first scan for an event `e` can be folded into. */
static inline bool platform_event_queue_fold_back(PlatformEventQueue *q,
                                                  const PlatformEvent *e) {
    for (i32 i = 0; i < q->count; i++) {
        i32 idx = (q->write + PLATFORM_EVENT_QUEUE_CAP - 1 - i) % PLATFORM_EVENT_QUEUE_CAP;
        if (platform_event_absorb(&q->ev[idx], e)) { q->coalesced++; return true; }
    }
    return false;
}

/* Fold the event at `idx` into a NEWER sibling, so evicting it loses nothing. */
static inline bool platform_event_queue_fold_forward(PlatformEventQueue *q, i32 idx) {
    for (i32 j = (idx + 1) % PLATFORM_EVENT_QUEUE_CAP; j != q->write;
         j = (j + 1) % PLATFORM_EVENT_QUEUE_CAP) {
        /* Fold the OLD one into the NEW one: for scroll that sums the deltas
         * in the right order, and for the latest-wins kinds the newer value is
         * already correct, so this only has to confirm a sibling exists. */
        if (q->ev[j].type != q->ev[idx].type) continue;
        if (q->ev[j].window != q->ev[idx].window) continue;
        if (q->ev[j].type == EVENT_SCROLL) {
            if (q->ev[j].scroll.mods     != q->ev[idx].scroll.mods ||
                q->ev[j].scroll.precise  != q->ev[idx].scroll.precise ||
                q->ev[j].scroll.momentum != q->ev[idx].scroll.momentum) continue;
            q->ev[j].scroll.dx += q->ev[idx].scroll.dx;
            q->ev[j].scroll.dy += q->ev[idx].scroll.dy;
        }
        return true;
    }
    return false;
}

/* Remove the entry at `idx`, closing the gap. O(count) and only reached under
 * saturation, where a 256-entry shift is nothing against the frame that is
 * already late. */
static inline void platform_event_queue_remove_at(PlatformEventQueue *q, i32 idx) {
    for (i32 pos = idx;;) {
        i32 next = (pos + 1) % PLATFORM_EVENT_QUEUE_CAP;
        if (next == q->write) break;
        q->ev[pos] = q->ev[next];
        pos = next;
    }
    q->write = (q->write + PLATFORM_EVENT_QUEUE_CAP - 1) % PLATFORM_EVENT_QUEUE_CAP;
    q->count--;
}

/* Returns true when the event was kept (queued or folded into one that was). */
static inline bool platform_event_queue_push(PlatformEventQueue *q, PlatformEvent e) {
    /* A native text commit may emit more than one ring's worth of characters.
     * Preserve FIFO order until the consumer catches up. Native pumps stop
     * before saturation, so this is normally only a single commit's spill. */
    bool spill = q->overflow_head != NULL;
    if (!spill && q->count == PLATFORM_EVENT_QUEUE_CAP &&
        platform_event_priority(e.type) == EVQ_PRIO_CRITICAL) {
        spill = true;
        for (i32 i = 0; i < q->count; i++)
            if (platform_event_priority(q->ev[(q->read+i)%PLATFORM_EVENT_QUEUE_CAP].type)
                < EVQ_PRIO_CRITICAL) { spill = false; break; }
    }
    if (spill) {
        PlatformEventOverflow *node = malloc(sizeof *node);
        if (!node) { q->dropped_critical++; platform_event_queue_discard(q, &e); return false; }
        *node = (PlatformEventOverflow){.event=e};
        if (q->overflow_tail) q->overflow_tail->next = node;
        else q->overflow_head = node;
        q->overflow_tail = node; q->overflow_count++;
        i64 depth=(i64)q->count+q->overflow_count;
        if (depth>q->high_water) q->high_water=depth>INT32_MAX?INT32_MAX:(i32)depth;
        return true;
    }
    if (q->count >= PLATFORM_EVENT_QUEUE_CAP) {
        /* 1. Fold into a queued sibling. Exact — nothing is lost. */
        if (platform_event_is_coalescable(e.type) &&
            platform_event_queue_fold_back(q, &e)) return true;

        /* 2. Take a slot from the oldest queued event of strictly lower
         *    priority, folding IT forward first if it has somewhere to go. */
        i32 prio = platform_event_priority(e.type);
        i32 victim = -1;
        for (i32 i = 0; i < q->count; i++) {
            i32 idx = (q->read + i) % PLATFORM_EVENT_QUEUE_CAP;
            if (platform_event_priority(q->ev[idx].type) < prio) { victim = idx; break; }
        }
        if (victim < 0) {
            /* Everything queued is at least as important. This one is lost —
             * the counters exist so that is observable rather than silent. */
            if (prio == EVQ_PRIO_CRITICAL) q->dropped_critical++;
            else                           q->dropped_lowpri++;
            platform_event_queue_discard(q, &e);
            return false;
        }
        if (platform_event_queue_fold_forward(q, victim)) q->coalesced++;
        else                                              q->dropped_lowpri++;
        platform_event_queue_discard(q, &q->ev[victim]);
        platform_event_queue_remove_at(q, victim);
        q->evicted_lowpri++;
    }
    q->ev[q->write] = e;
    q->write = (q->write + 1) % PLATFORM_EVENT_QUEUE_CAP;
    q->count++;
    if (q->count > q->high_water) q->high_water = q->count;
    return true;
}

static inline bool platform_event_queue_pop(PlatformEventQueue *q, PlatformEvent *out) {
    if (!out) return false;
    if (q->count == 0) {
        PlatformEventOverflow *node = q->overflow_head;
        if (!node) return false;
        *out = node->event; q->overflow_head = node->next;
        if (!q->overflow_head) q->overflow_tail = NULL;
        q->overflow_count--; free(node); return true;
    }
    *out = q->ev[q->read];
    q->read = (q->read + 1) % PLATFORM_EVENT_QUEUE_CAP;
    q->count--;
    return true;
}

/* A queued event still naming a window being freed would hand the app a
 * dangling pointer. Cheaper to blank them than to make every consumer
 * re-validate. Walks the whole array, not just the live span: a stale slot can
 * still be read back by the folding scans above. */
static inline void platform_event_queue_forget_window(PlatformEventQueue *q,
                                                      const void *w) {
    /* Only the LIVE span can own anything: a slot outside it is a copy of an
     * event that was already delivered or discarded. Blanking the type is what
     * stops the event from ever being delivered, so its payload goes here. */
    for (i32 k = 0; k < q->count; k++) {
        i32 i = (q->read + k) % PLATFORM_EVENT_QUEUE_CAP;
        if ((const void *)q->ev[i].window == w) platform_event_queue_discard(q, &q->ev[i]);
    }
    for (i32 i = 0; i < PLATFORM_EVENT_QUEUE_CAP; i++)
        if ((const void *)q->ev[i].window == w) { q->ev[i].window = NULL; q->ev[i].type = EVENT_NONE; }
    for (PlatformEventOverflow *n = q->overflow_head; n; n = n->next)
        if ((const void *)n->event.window == w) {
            platform_event_queue_discard(q, &n->event);
            n->event.window = NULL; n->event.type = EVENT_NONE;
        }
}

static inline void platform_event_queue_clear(PlatformEventQueue *q) {
    PlatformEvent e;
    while (platform_event_queue_pop(q, &e)) platform_event_queue_discard(q, &e);
}

#endif /* PLATFORM_EVENT_QUEUE_H */
