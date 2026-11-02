/*
 * open_queue — which pending channel open of a libssh2 session is asked for
 * next.
 *
 * libssh2 keeps ONE channel open in progress per session (session->open_state,
 * and for direct-tcpip a message built from the FIRST call's host and port,
 * session->direct_state), and the next call continues it whatever that call
 * asks for. A forward's connection that is waiting for its channel therefore
 * must not ask while another's open is in flight: it would be handed that
 * one's channel — the data of a connection to host B written to host A. Asking
 * for every waiting connection on each poll, lowest slot first, would hand an
 * older connection's channel to one that took a free low slot meanwhile. The TCP
 * and SOCKS forwards (ssh/port_forward.c) ask with
 * libssh2_channel_direct_tcpip and the Unix-socket forwards with
 * libssh2_channel_open_ex, which share the state.
 *
 * The order, shared by both: the open that is in flight is asked for again
 * (alone, with its own host and port) until it has answered; when none is, the
 * OLDEST waiting connection starts — unless the session's open is held by the
 * Unix-socket forwards (ssh/streamlocal_sched.h), which hold it while one of
 * theirs is in flight.
 *
 * Pure: nothing here calls libssh2.
 */
#ifndef SSH_OPEN_QUEUE_H
#define SSH_OPEN_QUEUE_H

#include "core/types.h"

typedef struct {
    bool        active;    /* a connection is waiting for its channel (or its open is in flight) */
    bool        started;   /* its open has been asked for: libssh2 carries it */
    u64         seq;       /* arrival order: the lower, the older */
    const void *owner;     /* the libssh2 session it belongs to */
} OpenQueueEntry;

/* The entry of `owner` to ask libssh2 for a channel for now, or -1. `held`:
 * another user of the session's open (the Unix-socket forwards) has one in
 * flight, so nothing new may start. */
static inline i32 open_queue_pick(const OpenQueueEntry *e, i32 n, const void *owner, bool held) {
    i32 oldest = -1;
    for (i32 i = 0; i < n; i++) {
        if (!e[i].active || e[i].owner != owner) continue;
        if (e[i].started) return i;                       /* the one libssh2 carries */
        if (oldest < 0 || e[i].seq < e[oldest].seq) oldest = i;
    }
    return held ? -1 : oldest;
}

/* Is an open of `owner` in flight (asked for, not yet answered)? */
static inline bool open_queue_busy(const OpenQueueEntry *e, i32 n, const void *owner) {
    for (i32 i = 0; i < n; i++)
        if (e[i].active && e[i].started && e[i].owner == owner) return true;
    return false;
}

#endif /* SSH_OPEN_QUEUE_H */
