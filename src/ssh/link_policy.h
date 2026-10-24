/*
 * link_policy — what an SSH pane does when its connection changes state.
 *
 * Pure: no session, no clock, no I/O. The session layer feeds it what it sees
 * (ssh_session.c, session_link_step) and the UI prints what it answers.
 *
 * Two cases look alike from the poll loop and need opposite answers:
 *
 *   - a remote shell that EXITED (the user typed `exit`, or ^D) closes its
 *     channel with EOF while the transport is perfectly healthy: logging
 *     straight back in would undo the `exit`;
 *   - a link that DIED (peer gone, keepalive unanswered, read error) leaves
 *     the session in SESSION_ERROR: left alone, the tab would sit there dead,
 *     with nothing on screen and no way back but closing it.
 *
 * A failed first connect is reported with the reason session_error() holds.
 *
 * What is decided here:
 *
 *   ended     the shell exited. Said once; nothing is retried. Enter connects
 *             again.
 *   lost      the link went down while it was up. Retried by itself on a
 *             back-off, as long as each attempt fails for a NETWORK reason
 *             (no route, no answer, handshake stalled).
 *   failed    an attempt failed for any other reason — a host key that was
 *             refused, an authentication that was not accepted — or it was
 *             the very first connect. Never retried by itself: asking a
 *             server for the same wrong credential five times is how an
 *             account gets locked. Enter tries again.
 */
#ifndef SSH_LINK_POLICY_H
#define SSH_LINK_POLICY_H

#include "core/types.h"

/* How many automatic attempts follow a lost link, and how long each waits.
 * 38 s in all: long enough for a laptop to rejoin Wi-Fi or a VPN to come
 * back, short enough that "it is not coming back" is said while the user
 * still remembers what they were doing. */
#define LINK_RETRY_MAX 5

static inline i32 link_retry_delay_ms(i32 attempt) {
    static const i32 delay[LINK_RETRY_MAX] = { 1000, 2000, 5000, 10000, 20000 };
    return attempt >= 1 && attempt <= LINK_RETRY_MAX ? delay[attempt - 1] : -1;
}

typedef enum {
    LINK_IDLE = 0,        /* nothing seen yet */
    LINK_CONNECTING,      /* an attempt is in flight (or waiting out its back-off) */
    LINK_UP,
    LINK_ENDED,           /* the remote shell exited */
    LINK_DOWN,            /* not connected, nothing in flight: Enter connects */
} LinkPhase;

typedef struct {
    u8   phase;           /* LinkPhase */
    bool was_up;          /* connected at least once in this pane's life */
    bool auto_retry;      /* the attempt in flight is automatic, not the user's */
    i32  attempt;         /* automatic attempts since the link was last up */
} LinkState;

typedef enum {
    LINK_SAY_NOTHING = 0,
    LINK_SAY_CONNECTING,  /* an attempt the user asked for has started */
    LINK_SAY_UP,          /* first connection: the server's own output follows */
    LINK_SAY_RECONNECTED,
    LINK_SAY_ENDED,
    LINK_SAY_LOST,        /* down; `retry_delay_ms` says when the next attempt starts */
    LINK_SAY_FAILED,      /* not retried: see the header */
    LINK_SAY_GAVE_UP,     /* every automatic attempt failed */
} LinkSay;

typedef struct {
    LinkSay say;
    i32     retry_delay_ms;   /* > 0: start a reconnect that waits this long first */
    i32     attempt;          /* which automatic attempt that is (1-based), else 0 */
} LinkStep;

/* The four things a session can be, as far as this is concerned. */
typedef enum {
    LINK_SEES_CONNECTING = 0,   /* SESSION_CONNECTING / SESSION_AUTHENTICATING */
    LINK_SEES_CONNECTED,
    LINK_SEES_DOWN,             /* SESSION_ERROR / SESSION_DISCONNECTED */
} LinkSees;

/* `channel_open`: only meaningful while connected — false once the remote
 * shell's channel has reported EOF. `network_failure`: only meaningful while
 * down — true when the failed attempt never got past reaching the host. */
static inline LinkStep link_policy_step(LinkState *st, LinkSees sees,
                                        bool channel_open, bool network_failure) {
    LinkStep out = { LINK_SAY_NOTHING, 0, 0 };
    switch (sees) {
    case LINK_SEES_CONNECTING:
        if (st->phase != LINK_CONNECTING) {
            st->phase = LINK_CONNECTING;
            /* An automatic attempt was announced when it was scheduled. */
            if (!st->auto_retry) out.say = LINK_SAY_CONNECTING;
        }
        break;

    case LINK_SEES_CONNECTED:
        if (st->phase == LINK_ENDED) break;          /* being wound down by the caller */
        if (st->phase != LINK_UP) {
            out.say = st->was_up ? LINK_SAY_RECONNECTED : LINK_SAY_UP;
            st->phase = LINK_UP;
            st->was_up = true;
            st->auto_retry = false;
            st->attempt = 0;
        } else if (!channel_open) {
            st->phase = LINK_ENDED;
            out.say = LINK_SAY_ENDED;
        }
        break;

    case LINK_SEES_DOWN:
        if (st->phase == LINK_UP) {
            /* It was working and now it is not: retried automatically.
             */
            st->phase = LINK_CONNECTING;
            st->auto_retry = true;
            st->attempt = 1;
            out.say = LINK_SAY_LOST;
            out.attempt = 1;
            out.retry_delay_ms = link_retry_delay_ms(1);
        } else if (st->phase == LINK_CONNECTING || st->phase == LINK_IDLE) {
            if (st->auto_retry && network_failure && st->attempt < LINK_RETRY_MAX) {
                st->attempt++;
                out.say = LINK_SAY_LOST;
                out.attempt = st->attempt;
                out.retry_delay_ms = link_retry_delay_ms(st->attempt);
            } else {
                out.say = st->auto_retry && network_failure ? LINK_SAY_GAVE_UP
                                                            : LINK_SAY_FAILED;
                st->phase = LINK_DOWN;
                st->auto_retry = false;
            }
        }
        /* LINK_ENDED / LINK_DOWN: already said. */
        break;
    }
    return out;
}

/* Enter in a pane whose link is ended or down. True when a new attempt should
 * be started (by the user, so it is announced and never auto-retried). */
static inline bool link_policy_manual(LinkState *st) {
    if (st->phase != LINK_ENDED && st->phase != LINK_DOWN) return false;
    st->phase = LINK_IDLE;
    st->auto_retry = false;
    st->attempt = 0;
    return true;
}

/* May a host-key question be put to the user on this attempt?
 *
 * A NEW key may always be asked about. A CHANGED key may not when the attempt
 * is automatic (`unattended`: a reconnect the policy started by itself after
 * the link dropped). Dropping a link is something anybody on the path can do,
 * and a dialog raised a second later would land under whatever the user is
 * typing. The attempt fails instead — for a reason that is not the network's,
 * so the policy above stops retrying and says "Enter connects again" — and the
 * dialog comes up when the user asks for the connection. */
static inline bool link_hostkey_may_prompt(bool key_changed, bool unattended) {
    return !(key_changed && unattended);
}

/* The reconnect the policy asked for could not be started (a transfer holds
 * the session): put the state back so the same answer is given next time. */
static inline void link_policy_unstarted(LinkState *st, const LinkState *before) {
    *st = *before;
}

#endif /* SSH_LINK_POLICY_H */
