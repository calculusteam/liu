/*
 * streamlocal_sched — who gets the next channel open of a session's
 * Unix-socket forwards, and how many connections a rule may hold.
 *
 * libssh2 keeps ONE channel open in progress per session, and the next
 * libssh2_channel_open_ex continues it whatever it is asked for (channel.c,
 * session->open_state): two opens cannot be in flight together, and a call that
 * asks for another socket while one is pending would be handed the pending
 * one's channel. The forwards of a session — up to four rules, each serving any
 * number of clients — therefore take their opens ONE AT A TIME, and this is the
 * order they take them in. (Once open, channels are independent: every relay
 * runs on its own.)
 *
 *   - An accepted client waits in its rule's pending queue; the listener keeps
 *     what the queue has no room for in the kernel's listen queue, and is not
 *     watched by the loop while it has no room (a readable listener nobody
 *     accepts from would wake it for ever).
 *   - A rule holds up to SLS_RELAY_CAP connections at once, pending ones
 *     included, so a long-lived connection (an event stream, a shell) does not
 *     shut out the other clients of its socket.
 *   - The next open goes to the rule after the one served last that has a client
 *     waiting (round robin): a rule with a flood of clients cannot starve
 *     another, whose client waits at most one open of each other rule.
 *   - An open that does not answer in SLS_OPEN_SEC gives up its CLIENT (closed:
 *     the client sees the failure instead of hanging for ever). libssh2 has no
 *     way to cancel it, so the open stays in flight — still asked for with its own
 *     rule's path — until the server's answer comes, and that answer's channel is
 *     closed; no other open starts meanwhile. The clients of the other rules wait
 *     behind it, and no client waits longer than SLS_OPEN_SEC from the moment it
 *     was accepted: they are closed too, so a server that never answers an open
 *     costs its clients a failure after ten seconds, not a hang.
 *
 * Pure: no libssh2 and no sockets.
 */
#ifndef SSH_STREAMLOCAL_SCHED_H
#define SSH_STREAMLOCAL_SCHED_H

#include "core/types.h"
#include "ssh/streamlocal_open.h"

#define SLS_RULES          4      /* the session's Unix-socket rules (the config's own bound) */
#define SLS_PENDING_CAP    4      /* accepted clients of a rule waiting for their channel */
#define SLS_RELAY_CAP      8      /* connections of a rule at once, waiting ones included */
#define SLS_OPEN_SEC       10.0   /* the longest a client waits for its channel */
#define SLS_OPENS_PER_POLL 8      /* opens one poll may finish and start (a bound, not a rate) */

/* Has a client that was accepted at `accepted_at` waited as long as it may for
 * its channel? Checked for every waiting client, not only the one whose open is
 * in flight. */
static inline bool sls_wait_over(f64 accepted_at, f64 now) {
    return now - accepted_at >= SLS_OPEN_SEC;
}

typedef struct {
    bool listening[SLS_RULES];   /* the rule has a listener */
    i32  pending[SLS_RULES];     /* accepted, no channel yet */
    i32  relays[SLS_RULES];      /* channels open */
    i32  inflight;               /* the rule whose open libssh2 carries, -1 = none */
    bool abandoned;              /* ...its client was given up; the open still has to finish */
    f64  since;                  /* when it started */
    i32  cursor;                 /* the rule the next pick looks at first */
} StreamSched;

typedef enum {
    SLS_WAIT = 0,        /* nothing yet: ask again on the next poll */
    SLS_RELAY,           /* a channel for the rule's oldest waiting client: start relaying */
    SLS_DROP_CLIENT,     /* refused: close the rule's oldest waiting client */
    SLS_ABANDON,         /* too long: close that client; the open stays in flight */
    SLS_DISCARD,         /* an abandoned open's channel arrived: close the channel */
    SLS_IGNORE,          /* an abandoned open ended without one: nothing to do */
} SlsAction;

static inline void sls_init(StreamSched *s) {
    for (i32 i = 0; i < SLS_RULES; i++) { s->listening[i] = false; s->pending[i] = s->relays[i] = 0; }
    s->inflight = -1;
    s->abandoned = false;
    s->since = 0.0;
    s->cursor = 0;
}

static inline void sls_set_listening(StreamSched *s, i32 rule, bool on) {
    if (rule >= 0 && rule < SLS_RULES) s->listening[rule] = on;
}

/* May a rule take another client off its listener? */
static inline bool sls_may_accept(const StreamSched *s, i32 rule) {
    if (rule < 0 || rule >= SLS_RULES || !s->listening[rule]) return false;
    return s->pending[rule] < SLS_PENDING_CAP &&
           s->pending[rule] + s->relays[rule] < SLS_RELAY_CAP;
}

/* The loop is woken for a listener only while the rule would accept from it. */
static inline bool sls_wants_listen_watch(const StreamSched *s, i32 rule) {
    return sls_may_accept(s, rule);
}

static inline void sls_accepted(StreamSched *s, i32 rule) {
    if (rule >= 0 && rule < SLS_RULES) s->pending[rule]++;
}

/* A waiting client of the rule was closed for waiting too long (sls_wait_over). */
static inline void sls_gave_up_waiting(StreamSched *s, i32 rule) {
    if (rule >= 0 && rule < SLS_RULES && s->pending[rule] > 0) s->pending[rule]--;
}

/* A relay of the rule has ended. */
static inline void sls_relay_closed(StreamSched *s, i32 rule) {
    if (rule >= 0 && rule < SLS_RULES && s->relays[rule] > 0) s->relays[rule]--;
}

/* The rule to ask libssh2 for a channel for now: the one whose open is in
 * flight (it must be asked for with ITS path, answered before anything else
 * starts), else the next one with a client waiting after the one served last.
 * -1 when there is nothing to ask. */
static inline i32 sls_pick(StreamSched *s, f64 now) {
    if (s->inflight >= 0) return s->inflight;
    for (i32 k = 0; k < SLS_RULES; k++) {
        i32 r = (s->cursor + k) % SLS_RULES;
        if (s->listening[r] && s->pending[r] > 0) {
            s->inflight = r;
            s->abandoned = false;
            s->since = now;
            return r;
        }
    }
    return -1;
}

/* What the open just asked for said (`step`: ssh/streamlocal_open.h), and what
 * to do about it. */
static inline SlsAction sls_step(StreamSched *s, i32 rule, StreamlocalOpenStep step, f64 now) {
    if (rule < 0 || rule >= SLS_RULES || rule != s->inflight) return SLS_IGNORE;
    if (step == STREAMLOCAL_OPEN_WAIT) {
        if (!s->abandoned && s->pending[rule] > 0 && now - s->since >= SLS_OPEN_SEC) {
            s->abandoned = true;
            s->pending[rule]--;
            return SLS_ABANDON;
        }
        return SLS_WAIT;
    }
    bool gave_up = s->abandoned;
    s->inflight = -1;
    s->abandoned = false;
    s->cursor = (rule + 1) % SLS_RULES;
    if (step == STREAMLOCAL_OPEN_READY) {
        if (gave_up || s->pending[rule] <= 0) return SLS_DISCARD;
        s->pending[rule]--;
        s->relays[rule]++;
        return SLS_RELAY;
    }
    if (gave_up || s->pending[rule] <= 0) return SLS_IGNORE;
    s->pending[rule]--;
    return SLS_DROP_CLIENT;
}

#endif /* SSH_STREAMLOCAL_SCHED_H */
