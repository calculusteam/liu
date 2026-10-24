/*
 * A queued event owns a copy of its drop path or input-method composition until
 * delivery. The payload remains valid while the app handles that event and is
 * released on the next delivery; discarding an undelivered event releases it
 * immediately. Validate each uri-list entry before allocating or queuing it so
 * rejected entries cannot alter queued payloads.
 */
#ifndef PLATFORM_EVENT_PAYLOAD_H
#define PLATFORM_EVENT_PAYLOAD_H

#include "platform/platform.h"
#include "platform/event_queue.h"
#include "platform/xdnd_uri.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PLATFORM_PAYLOAD_MAX_COUNT 4096u
#define PLATFORM_PAYLOAD_MAX_BYTES ((usize)8 * 1024 * 1024)

typedef struct {
    const char *delivered;    /* payload of the event the app is handling now */
    u32   live;               /* strings currently owned */
    usize live_bytes;
    u32   refused;            /* could not be kept: over a bound, or no memory */
    u32   kept_total;         /* monotonic, for tests */
    u32   freed_total;
} PlatformPayloads;

/* The allocation is [usize length][bytes][NUL]; events see the bytes. */
static inline const char *platform_payload_keep(PlatformPayloads *p, const char *s, usize len) {
    if (!p || !s) return NULL;
    if (p->live >= PLATFORM_PAYLOAD_MAX_COUNT ||
        len >= PLATFORM_PAYLOAD_MAX_BYTES ||
        len + 1 > PLATFORM_PAYLOAD_MAX_BYTES - p->live_bytes) { p->refused++; return NULL; }
    usize *block = (usize *)malloc(sizeof(usize) + len + 1);
    if (!block) { p->refused++; return NULL; }
    block[0] = len + 1;
    char *out = (char *)(block + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    p->live++;
    p->live_bytes += len + 1;
    p->kept_total++;
    return out;
}

static inline void platform_payload_free(PlatformPayloads *p, const char *s) {
    if (!p || !s) return;
    usize *block = ((usize *)(void *)s) - 1;
    if (p->live) p->live--;
    p->live_bytes = block[0] <= p->live_bytes ? p->live_bytes - block[0] : 0;
    p->freed_total++;
    free(block);
}

/* Which string of `e` is owned here: a drop's path, an input method's
 * composition (X11 on-the-spot preedit). */
static inline const char **platform_event_owned_payload(PlatformEvent *e) {
    if (!e) return NULL;
    if (e->type == EVENT_DROP_FILE)   return &e->drop.path;
    if (e->type == EVENT_IME_COMPOSE) return &e->ime.text;
    return NULL;
}

/* The event at `e` is being handed to the app. Whatever the previously
 * delivered event owned is finished with. */
static inline void platform_payload_delivered(PlatformPayloads *p, PlatformEvent *e) {
    if (!p) return;
    if (p->delivered) { platform_payload_free(p, p->delivered); p->delivered = NULL; }
    const char **slot = platform_event_owned_payload(e);
    if (slot && *slot) p->delivered = *slot;
}

/* The queue is destroying `e` undelivered. Signature of the queue's hook. */
static inline void platform_payload_discard(void *ctx, PlatformEvent *e) {
    PlatformPayloads *p = (PlatformPayloads *)ctx;
    const char **slot = platform_event_owned_payload(e);
    if (!p || !slot || !*slot) return;
    platform_payload_free(p, *slot);
    *slot = NULL;
}

/* Nothing is being handled any more (shutdown, after the queue was cleared). */
static inline void platform_payloads_reset(PlatformPayloads *p) {
    if (p && p->delivered) { platform_payload_free(p, p->delivered); p->delivered = NULL; }
}

/* Split a text/uri-list payload (CR/LF separated, '#' comments) and queue one
 * EVENT_DROP_FILE per local file. Each line is validated into a scratch buffer
 * FIRST and kept only if it is a path: a refused line touches nothing that is
 * already queued. Returns how many events were queued; `*refused`, when given,
 * gets the lines that were not (not a local file, too long, a control byte,
 * over the payload bound, queue full). */
static inline i32 platform_drop_emit_uri_list(PlatformEventQueue *q, PlatformPayloads *p,
                                              PlatformWindow *window,
                                              const char *data, usize len,
                                              i32 x, i32 y, f64 timestamp,
                                              i32 *refused) {
    i32 queued = 0, bad = 0;
    usize i = 0;
    while (data && i < len) {
        usize j = i;
        while (j < len && data[j] != '\n' && data[j] != '\r') j++;
        usize line_len = j - i;
        if (line_len > 0 && data[i] != '#') {
            char path[PATH_MAX];
            const char *kept = NULL;
            if (xdnd_uri_to_path(data + i, line_len, path, sizeof path))
                kept = platform_payload_keep(p, path, strlen(path));
            if (kept) {
                PlatformEvent e = {
                    .type = EVENT_DROP_FILE,
                    .window = window,
                    .timestamp = timestamp,
                    .drop = { .path = kept, .x = x, .y = y },
                };
                /* A push that is not kept hands the event to the discard hook,
                 * which frees the copy — nothing to do here but count it. */
                if (platform_event_queue_push(q, e)) queued++;
                else bad++;
            } else {
                bad++;
            }
        }
        while (j < len && (data[j] == '\n' || data[j] == '\r')) j++;
        i = j;
    }
    if (refused) *refused = bad;
    return queued;
}

#endif /* PLATFORM_EVENT_PAYLOAD_H */
