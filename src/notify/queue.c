/*
 * liu-notify - queue implementation.
 * Single-writer / single-reader; no locking needed.
 */
#include "notify/queue.h"

#include <stdio.h>
#include <string.h>

static u32 fnv1a_32(const u8 *buf, usize len, u32 seed) {
    u32 h = seed ? seed : 0x811C9DC5u;
    for (usize i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 0x01000193u;
    }
    return h;
}

static u32 compute_dedup_key(const NotifyFrame *f) {
    u32 h = 0x811C9DC5u;
    u8 prefix[2] = { f->header.tool_id, f->header.event_id };
    h = fnv1a_32(prefix, sizeof prefix, h);
    if (f->title && f->header.title_len)
        h = fnv1a_32(f->title, f->header.title_len, h);
    if (f->body && f->header.body_len)
        h = fnv1a_32(f->body, f->header.body_len, h);
    return h;
}

void queue_init(NotifyQueue *q, f64 now) {
    *q = (NotifyQueue){0};
    q->refill_rate  = NOTIFY_RATE_REFILL;
    q->burst_cap    = (f64)NOTIFY_RATE_BURST;
    q->dedup_window = NOTIFY_DEDUP_WINDOW;
    q->tokens       = q->burst_cap;
    q->last_refill  = now;
}

void queue_set_limits(NotifyQueue *q, u32 refill_per_sec, u32 burst_cap, f64 dedup_window_sec) {
    if (refill_per_sec > 0) q->refill_rate  = (f64)refill_per_sec;
    if (burst_cap      > 0) q->burst_cap    = (f64)burst_cap;
    if (dedup_window_sec > 0.0) q->dedup_window = dedup_window_sec;
    if (q->tokens > q->burst_cap) q->tokens = q->burst_cap;
}

static void refill_tokens(NotifyQueue *q, f64 now) {
    if (now <= q->last_refill) return;
    f64 dt = now - q->last_refill;
    q->last_refill = now;
    q->tokens += dt * q->refill_rate;
    if (q->tokens > q->burst_cap) q->tokens = q->burst_cap;
}

/* Slot-hashed dedup: a key and a timestamp per slot. Collisions (different
 * keys hashing to the same slot) overwrite the freshness window of the prior
 * occupant — acceptable, because the worst case is a duplicate we failed to
 * suppress; we never spuriously drop a unique message. With 32-bit FNV1a
 * into 32 slots the collision rate is low for the daemon's rate profile. */
static bool dedup_seen(NotifyQueue *q, u32 key, f64 now) {
    u32 idx = key & (NOTIFY_DEDUP_SLOTS - 1u);
    DedupSlot *s = &q->dedup[idx];
    if (s->key == key && (now - s->t) < q->dedup_window) return true;
    s->key = key;
    s->t   = now;
    return false;
}

/* Safely append a C-string into the text buffer; clamps on overflow without
 * trusting snprintf's may-return-larger-than-n semantics. */
static void append_cstr(char *buf, usize cap, usize *off, const char *s) {
    if (!s || *off + 1 >= cap) return;
    usize room = cap - *off - 1;
    usize slen = strlen(s);
    if (slen > room) slen = room;
    memcpy(buf + *off, s, slen);
    *off += slen;
}

static void append_bytes(char *buf, usize cap, usize *off, const u8 *src, usize n) {
    if (!src || n == 0 || *off + 1 >= cap) return;
    usize room = cap - *off - 1;
    if (n > room) n = room;
    memcpy(buf + *off, src, n);
    *off += n;
}

static void build_text(NotifyMsg *m, const NotifyFrame *f) {
    const char *tool = notify_tool_label(f->header.tool_id);
    const char *evt  = notify_event_label(f->header.event_id);
    usize off = 0;

    /* Pattern: "<tool>: <title> - <body>"  (ASCII dash keeps voices consistent). */
    append_cstr(m->text, NOTIFY_MSG_TEXT_CAP, &off, tool);

    if (f->header.title_len && f->title) {
        append_cstr(m->text, NOTIFY_MSG_TEXT_CAP, &off, ": ");
        append_bytes(m->text, NOTIFY_MSG_TEXT_CAP, &off, f->title, f->header.title_len);
    } else {
        append_cstr(m->text, NOTIFY_MSG_TEXT_CAP, &off, " ");
        append_cstr(m->text, NOTIFY_MSG_TEXT_CAP, &off, evt);
    }

    if (f->header.body_len && f->body) {
        append_cstr(m->text, NOTIFY_MSG_TEXT_CAP, &off, " - ");
        append_bytes(m->text, NOTIFY_MSG_TEXT_CAP, &off, f->body, f->header.body_len);
    }

    if (off >= NOTIFY_MSG_TEXT_CAP) off = NOTIFY_MSG_TEXT_CAP - 1;
    m->text[off] = '\0';
    m->text_len = (u32)off;
}

QueuePushResult queue_push(NotifyQueue *q, const NotifyFrame *f, f64 now) {
    if (!q || !f) return Q_PUSH_DROP_BADARG;

    refill_tokens(q, now);
    if (q->tokens < 1.0) {
        q->dropped_rate++;
        return Q_PUSH_DROP_RATE;
    }

    u32 key = compute_dedup_key(f);
    if (dedup_seen(q, key, now)) {
        q->dropped_dedup++;
        return Q_PUSH_DROP_DEDUP;
    }

    if (queue_size(q) >= NOTIFY_QUEUE_CAP) {
        q->dropped_full++;
        return Q_PUSH_DROP_FULL;
    }

    u32 idx = (u32)(q->tail & (NOTIFY_QUEUE_CAP - 1u));
    NotifyMsg *m = &q->slots[idx];
    memset(m, 0, sizeof *m);
    m->tool_id    = f->header.tool_id;
    m->event_id   = f->header.event_id;
    m->flags      = (u8)f->header.flags;
    m->seq        = f->header.seq;
    m->dedup_key  = key;
    build_text(m, f);

    q->tokens -= 1.0;
    q->tail++;
    return Q_PUSH_OK;
}

const NotifyMsg *queue_peek(const NotifyQueue *q) {
    if (queue_empty(q)) return NULL;
    u32 idx = (u32)(q->head & (NOTIFY_QUEUE_CAP - 1u));
    return &q->slots[idx];
}

void queue_pop_commit(NotifyQueue *q) {
    if (queue_empty(q)) return;
    q->head++;
}
