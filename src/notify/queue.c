/*
 * liu-notify - queue implementation. Single-writer / single-reader; no
 * locking needed.
 *
 * Speech text is heap-allocated per message (see queue.h). Build into a
 * stack scratch buffer (NOTIFY_MSG_TEXT_CAP) then malloc(exact), so the
 * truncation policy is the fixed buffer's without leaving a full buffer
 * of slack on every live slot.
 */
#include "notify/queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static u32 fnv1a_32(const u8 *buf, usize len, u32 seed) {
    u32 h = seed ? seed : 0x811C9DC5u;
    for (usize i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 0x01000193u;
    }
    return h;
}

/* Fold one meta value into `h`. The separator goes in whether or not the key
 * is present, so a value's POSITION says which field it is: (cwd "a", no
 * session) and (no cwd, session "a") must not hash alike. Sets *any when the
 * key carried a value. */
static u32 hash_meta_field(const NotifyFrame *f, const char *key, u32 h, bool *any) {
    char val[NOTIFY_MAX_META];
    h = fnv1a_32((const u8 *)"\x1f", 1, h);
    if (notify_meta_get(f, key, val, sizeof val) && val[0]) {
        h = fnv1a_32((const u8 *)val, strlen(val), h);
        if (any) *any = true;
    }
    return h;
}

/* Exact-repeat key: everything that makes this frame THIS notification — who
 * sent it, what it says, and which run it came from.
 *
 * The run is needed because an installed hook sends neither title nor body:
 * keyed on (tool, event, title, body) alone, every `claude … complete` would
 * hash alike, a second Claude session finishing in another project inside the
 * window would be dropped as a "duplicate" of the first, and every agent the
 * wire calls TOOL_CUSTOM (Grok, OpenCode, Antigravity, Cline, …) would share
 * one key. The frame's meta says which run it was — pane, agent, session, cwd
 * — so that is what tells two notifications apart; the same hook firing twice
 * for one turn still carries identical meta and is still dropped. Named keys
 * rather than the raw meta block, so a key added later that differs per
 * invocation cannot quietly switch dedup off. */
static u32 compute_dedup_key(const NotifyFrame *f) {
    u32 h = 0x811C9DC5u;
    u8 prefix[2] = { f->header.tool_id, f->header.event_id };
    h = fnv1a_32(prefix, sizeof prefix, h);
    if (f->title && f->header.title_len)
        h = fnv1a_32(f->title, f->header.title_len, h);
    h = fnv1a_32((const u8 *)"\x1f", 1, h);
    if (f->body && f->header.body_len)
        h = fnv1a_32(f->body, f->header.body_len, h);
    h = hash_meta_field(f, NOTIFY_META_PANE,    h, NULL);
    h = hash_meta_field(f, NOTIFY_META_AGENT,   h, NULL);
    h = hash_meta_field(f, NOTIFY_META_SESSION, h, NULL);
    h = hash_meta_field(f, NOTIFY_META_CWD,     h, NULL);
    return h;
}

/* Identity of the TURN a frame belongs to, independent of which agent label
 * the hook carried: the event plus where the agent ran — the cwd and session
 * id from its payload, and the Liu pane its hook was started in. See the `turn`
 * table in queue.h for why this exists. Deliberately NOT the agent or the tool
 * id: one run reporting under two labels is the case this table collapses, and
 * both of that run's hooks inherit the same pane, cwd and session. Returns
 * false when the frame carries no identity at all (no meta) — then there is
 * nothing to match on and the frame must not be collapsed against others. */
static bool compute_turn_key(const NotifyFrame *f, u32 *out) {
    u32 h = 0x811C9DC5u;
    u8 ev = f->header.event_id;
    h = fnv1a_32(&ev, 1, h);
    bool any = false;
    h = hash_meta_field(f, NOTIFY_META_CWD,     h, &any);
    h = hash_meta_field(f, NOTIFY_META_SESSION, h, &any);
    h = hash_meta_field(f, NOTIFY_META_PANE,    h, &any);
    if (!any) return false;
    *out = h;
    return true;
}

static void free_msg_text(NotifyMsg *m) {
    free(m->text);
    m->text = NULL;
    m->text_len = 0;
}

/* Drop in-flight heap strings before zeroing (static queue re-init). */
static void free_queued_texts(NotifyQueue *q) {
    if (!q || q->tail <= q->head) return;
    for (u64 i = q->head; i < q->tail; i++) {
        free_msg_text(&q->slots[i & (NOTIFY_QUEUE_CAP - 1u)]);
    }
}

void queue_init(NotifyQueue *q, f64 now) {
    free_queued_texts(q);
    *q = (NotifyQueue){0};
    q->refill_rate  = NOTIFY_RATE_REFILL;
    q->burst_cap    = (f64)NOTIFY_RATE_BURST;
    q->dedup_window = NOTIFY_DEDUP_WINDOW;
    q->tokens       = q->burst_cap;
    q->last_refill  = now;
}

void queue_set_limits(NotifyQueue *q, u32 refill_per_sec, u32 burst_cap, f64 dedup_window_sec) {
    if (!q) return;
    /* 0 means "the built-in default", NOT "leave it as it is": a reload passes
     * 0 for every key the file no longer has, so deleting `rate_burst = 1`
     * from notify.conf and reloading must bring the default burst back without
     * a server restart. */
    q->refill_rate  = refill_per_sec > 0 ? (f64)refill_per_sec : NOTIFY_RATE_REFILL;
    q->burst_cap    = burst_cap      > 0 ? (f64)burst_cap      : (f64)NOTIFY_RATE_BURST;
    q->dedup_window = (isfinite(dedup_window_sec) && dedup_window_sec > 0.0)
                    ? dedup_window_sec : NOTIFY_DEDUP_WINDOW;
    /* A smaller bucket spills at once; a larger one fills at the refill rate,
     * like any bucket — raising the cap is not a grant of tokens. */
    if (q->tokens > q->burst_cap) q->tokens = q->burst_cap;
}

static void refill_tokens(NotifyQueue *q, f64 now) {
    if (!isfinite(now)) return;
    if (now <= q->last_refill) return;
    f64 dt = now - q->last_refill;
    q->last_refill = now;
    q->tokens += dt * q->refill_rate;
    if (q->tokens > q->burst_cap) q->tokens = q->burst_cap;
}

/* Slot-hashed dedup: a key and a timestamp per slot. Collisions (different
 * keys hashing to the same slot) overwrite the freshness window of the prior
 * occupant — acceptable, because the worst case is a duplicate that is not
 * suppressed; a unique message is never dropped spuriously. With 32-bit
 * FNV1a into 32 slots the collision rate is low for the daemon's rate
 * profile. */
static bool slot_seen(DedupSlot *table, u32 key, f64 now, f64 window) {
    u32 idx = key & (NOTIFY_DEDUP_SLOTS - 1u);
    DedupSlot *s = &table[idx];
    if (s->key == key && (now - s->t) < window) return true;
    s->key = key;
    s->t   = now;
    return false;
}

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

/* The name the user reads on the banner and hears in speech.
 *
 * The wire has ids for three tools; every other agent travels as TOOL_CUSTOM,
 * whose label is the word "custom" — so Grok, OpenCode, Antigravity, Cline and
 * the rest would all announce themselves as "custom notify". The sender's own
 * id is in the frame's meta (`agent=`) for exactly this. It comes from another
 * process, so only a conservative alphabet survives (it lands in a banner
 * title, a speech string and an AppleScript literal) and it is length-bound;
 * what does not survive falls back to the tool id's label. */
#define SENDER_LABEL_CAP 32
static const char *sender_label(const NotifyFrame *f, char *buf) {
    if (f->header.tool_id != TOOL_CUSTOM) return notify_tool_label(f->header.tool_id);

    char raw[64];
    if (!notify_meta_get(f, NOTIFY_META_AGENT, raw, sizeof raw))
        return notify_tool_label(TOOL_CUSTOM);
    usize o = 0;
    for (const char *p = raw; *p && o + 1 < SENDER_LABEL_CAP; p++) {
        unsigned char c = (unsigned char)*p;
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                    (c == ' ' && o > 0);
        if (keep) buf[o++] = (char)c;
    }
    while (o > 0 && buf[o - 1] == ' ') o--;
    buf[o] = '\0';
    return o > 0 ? buf : notify_tool_label(TOOL_CUSTOM);
}

/* Build speech into stack scratch, then own an exact-sized heap copy.
 * Returns false only on malloc failure (m left with text == NULL). */
static bool build_text(NotifyMsg *m, const NotifyFrame *f) {
    char label[SENDER_LABEL_CAP];
    const char *tool = sender_label(f, label);
    const char *evt  = notify_event_label(f->header.event_id);
    char scratch[NOTIFY_MSG_TEXT_CAP];
    usize off = 0;

    /* Pattern: "<tool>: <title> - <body>"  (ASCII dash keeps voices consistent). */
    append_cstr(scratch, NOTIFY_MSG_TEXT_CAP, &off, tool);

    if (f->header.title_len && f->title) {
        append_cstr(scratch, NOTIFY_MSG_TEXT_CAP, &off, ": ");
        append_bytes(scratch, NOTIFY_MSG_TEXT_CAP, &off, f->title, f->header.title_len);
    } else {
        append_cstr(scratch, NOTIFY_MSG_TEXT_CAP, &off, " ");
        append_cstr(scratch, NOTIFY_MSG_TEXT_CAP, &off, evt);
    }

    if (f->header.body_len && f->body) {
        append_cstr(scratch, NOTIFY_MSG_TEXT_CAP, &off, " - ");
        append_bytes(scratch, NOTIFY_MSG_TEXT_CAP, &off, f->body, f->header.body_len);
    }

    if (off >= NOTIFY_MSG_TEXT_CAP) off = NOTIFY_MSG_TEXT_CAP - 1;
    scratch[off] = '\0';

    char *heap = (char *)malloc(off + 1);
    if (!heap) {
        m->text = NULL;
        m->text_len = 0;
        return false;
    }
    memcpy(heap, scratch, off + 1);
    m->text = heap;
    m->text_len = (u32)off;
    return true;
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
    /* Same turn already announced under some other agent label? Checked after
     * the exact-match table so an identical repeat is still counted there.
     * The two never fight: the exact key is the turn's identity PLUS the label
     * and text, so a frame the turn table lets through (a different pane, cwd
     * or session) always has a different exact key as well. */
    u32 turn_key = 0;
    if (compute_turn_key(f, &turn_key) &&
        slot_seen(q->turn, turn_key, now, q->dedup_window)) {
        q->dropped_dedup++;
        return Q_PUSH_DROP_DEDUP;
    }

    if (queue_size(q) >= NOTIFY_QUEUE_CAP) {
        q->dropped_full++;
        return Q_PUSH_DROP_FULL;
    }

    u32 idx = (u32)(q->tail & (NOTIFY_QUEUE_CAP - 1u));
    NotifyMsg *m = &q->slots[idx];
    /* Slot was freed on the previous pop (or never used); do not free here. */
    memset(m, 0, sizeof *m);
    m->tool_id    = f->header.tool_id;
    m->event_id   = f->header.event_id;
    m->flags      = (u8)f->header.flags;
    m->seq        = f->header.seq;
    m->dedup_key  = key;
    if (!build_text(m, f)) {
        /* OOM: leave ring unadvanced. Dedup already recorded — rare, acceptable. */
        return Q_PUSH_DROP_BADARG;
    }

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
    u32 idx = (u32)(q->head & (NOTIFY_QUEUE_CAP - 1u));
    free_msg_text(&q->slots[idx]);
    q->head++;
}
