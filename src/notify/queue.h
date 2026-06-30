/*
 * liu-notify - bounded in-memory queue with dedup + token-bucket rate limit.
 * Single-threaded; the daemon event loop is the only producer/consumer.
 */
#ifndef NOTIFY_QUEUE_H
#define NOTIFY_QUEUE_H

#include "core/types.h"
#include "notify/protocol.h"

#define NOTIFY_QUEUE_CAP     64u
#define NOTIFY_DEDUP_SLOTS   32u
#define NOTIFY_DEDUP_WINDOW  5.0   /* seconds */
#define NOTIFY_RATE_BURST    20u
#define NOTIFY_RATE_REFILL   10.0  /* tokens per second */

#define NOTIFY_MSG_TEXT_CAP  (NOTIFY_MAX_TITLE + NOTIFY_MAX_BODY + 64u)

_Static_assert((NOTIFY_QUEUE_CAP & (NOTIFY_QUEUE_CAP - 1u)) == 0,
               "NOTIFY_QUEUE_CAP must be a power of 2");
_Static_assert((NOTIFY_DEDUP_SLOTS & (NOTIFY_DEDUP_SLOTS - 1u)) == 0,
               "NOTIFY_DEDUP_SLOTS must be a power of 2");

typedef struct {
    u8   tool_id;
    u8   event_id;
    u8   flags;
    u8   _pad;
    u32  seq;
    u32  dedup_key;
    u32  text_len;
    char text[NOTIFY_MSG_TEXT_CAP];
} NotifyMsg;

typedef struct {
    u32 key;
    f64 t;       /* platform_time_sec equivalent: monotonic seconds */
} DedupSlot;

typedef struct {
    NotifyMsg slots[NOTIFY_QUEUE_CAP];
    u64       head;        /* monotonic; pop index = head & (CAP-1) */
    u64       tail;        /* monotonic; push index = tail & (CAP-1) */
    DedupSlot dedup[NOTIFY_DEDUP_SLOTS];
    f64       tokens;
    f64       last_refill; /* monotonic seconds */
    u64       dropped_rate;
    u64       dropped_dedup;
    u64       dropped_full;

    /* Tunable limits (overridable via queue_set_limits). */
    f64       refill_rate;   /* tokens / second */
    f64       burst_cap;     /* max token bucket depth */
    f64       dedup_window;  /* seconds */
} NotifyQueue;

typedef enum {
    Q_PUSH_OK         = 0,
    Q_PUSH_DROP_RATE  = 1,
    Q_PUSH_DROP_DEDUP = 2,
    Q_PUSH_DROP_FULL  = 3,
    Q_PUSH_DROP_BADARG= 4,
} QueuePushResult;

void queue_init(NotifyQueue *q, f64 now);

/* Override tuning limits after queue_init. 0 / 0.0 keeps the current value. */
void queue_set_limits(NotifyQueue *q, u32 refill_per_sec, u32 burst_cap, f64 dedup_window_sec);

/* Produces a human speech string from the frame and pushes it. */
QueuePushResult queue_push(NotifyQueue *q, const NotifyFrame *frame, f64 now);

/* Returns pointer to next message, or NULL if empty.  Owned by queue until
 * queue_pop_commit() is called. */
const NotifyMsg *queue_peek(const NotifyQueue *q);
void             queue_pop_commit(NotifyQueue *q);

static inline bool queue_empty(const NotifyQueue *q) { return q->head == q->tail; }
static inline u32  queue_size (const NotifyQueue *q) { return (u32)(q->tail - q->head); }

#endif /* NOTIFY_QUEUE_H */
