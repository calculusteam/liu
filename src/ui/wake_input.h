/* Main-thread-owned input retained while a tab is waking. The queue owns only
 * its lazy ring; events contain copied scalar payloads and a pane recipient. */
#ifndef LIU_WAKE_INPUT_H
#define LIU_WAKE_INPUT_H

#include "platform/platform.h"
#include "core/checked.h"
#include "core/mem_budget.h"
#include <stdlib.h>

#define WAKE_INPUT_INITIAL_CAP 16
#define WAKE_INPUT_MAX_CAP 4096

typedef struct {
    PlatformEvent event;
    i32 pane;
} WakeInputEvent;

typedef struct {
    WakeInputEvent *events;
    i32 cap, head, count;
} WakeInputQueue;

/* Queue structs may be moved with their owner; zero the old owner after the
 * transfer. A copied queue must not be independently cleared a second time. */
static inline void wake_input_clear(WakeInputQueue *queue) {
    if (!queue) return;
    if (queue->events) {
        usize bytes = (usize)queue->cap * sizeof(*queue->events);
        free(queue->events);
        mem_budget_transient_adjust(MEM_TERMINAL, -(isize)bytes);
    }
    *queue = (WakeInputQueue){0};
}

static inline bool wake_input_push(WakeInputQueue *queue, const PlatformEvent *event,
                                   i32 pane) {
    if (!queue || !event || (event->type != EVENT_KEY_DOWN &&
        event->type != EVENT_KEY_UP && event->type != EVENT_CHAR_INPUT)) return false;
    if (queue->count >= WAKE_INPUT_MAX_CAP) return false;
    if (queue->count == queue->cap) {
        i32 next = queue->cap ? queue->cap * 2 : WAKE_INPUT_INITIAL_CAP;
        if (next > WAKE_INPUT_MAX_CAP) next = WAKE_INPUT_MAX_CAP;
        usize bytes;
        if (!checked_mul_usize((usize)next, sizeof(*queue->events), &bytes)) return false;
        WakeInputEvent *events = malloc(bytes);
        if (!events) return false;
        for (i32 i = 0; i < queue->count; i++)
            events[i] = queue->events[(queue->head + i) % queue->cap];
        usize old_bytes = (usize)queue->cap * sizeof(*queue->events);
        free(queue->events);
        queue->events = events;
        queue->cap = next;
        queue->head = 0;
        mem_budget_transient_adjust(MEM_TERMINAL, (isize)bytes - (isize)old_bytes);
    }
    WakeInputEvent copy = {0};
    copy.pane = pane;
    copy.event.type = event->type;
    copy.event.timestamp = event->timestamp;
    copy.event.timestamp_uncertain = event->timestamp_uncertain;
    /* No borrowed window or union pointer survives a move/close. Replay uses
     * the current queue owner and the captured pane instead. */
    if (event->type == EVENT_CHAR_INPUT) {
        copy.event.char_input.codepoint = event->char_input.codepoint;
        copy.event.char_input.mods = event->char_input.mods;
    } else {
        copy.event.key.key = event->key.key;
        copy.event.key.mods = event->key.mods;
        copy.event.key.repeat = event->key.repeat;
    }
    queue->events[(queue->head + queue->count) % queue->cap] = copy;
    queue->count++;
    return true;
}

static inline bool wake_input_take(WakeInputQueue *queue, WakeInputEvent *out) {
    if (!queue || !out || queue->count == 0) return false;
    *out = queue->events[queue->head];
    queue->head = (queue->head + 1) % queue->cap;
    if (--queue->count == 0) queue->head = 0;
    return true;
}

#endif
