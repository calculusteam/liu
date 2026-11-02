/*
 * open_gate — who may start a channel open on a libssh2 connection.
 *
 * libssh2 keeps ONE channel open in progress per session (session->open_state)
 * and the next call continues it whatever that call asks for. Everything that
 * opens a channel on a connection therefore takes its turn, and "a connection"
 * is the libssh2 session — not the pane: with a shared connection (ControlMaster)
 * several panes, each with its own forwards and its own shell, are inside one
 * libssh2 session. The users of that one open are
 *
 *   - the TCP / SOCKS forwards (ssh/port_forward.c), whose pending connections
 *     form one table per connection and are ordered by ssh/open_queue.h;
 *   - a Unix-socket forward set (ssh/streamlocal_sched.h): its open in flight is
 *     held here under the set's own identity;
 *   - a pane that borrows the connection: its shell and SFTP channels are opened
 *     in one blocking sequence on a worker thread. It reserves the connection
 *     first, so nothing new starts while it waits for an open in flight to end,
 *     and it opens only when none is.
 *
 * A new open may start when nobody else's is in flight and nobody has reserved
 * the connection; an open that is in flight is always carried to its end by its
 * own user. Pure: it only decides; the caller makes every libssh2 call.
 */
#ifndef SSH_OPEN_GATE_H
#define SSH_OPEN_GATE_H

#include "core/types.h"

#define OPEN_GATE_SLOTS 16

typedef struct {
    const void *conn;       /* the libssh2 session; NULL: a free slot */
    const void *holder;     /* the Unix-socket set whose open is in flight, or NULL */
    i32         reserved;   /* panes waiting to open their channels on the connection */
} OpenGateSlot;

typedef struct {
    OpenGateSlot slot[OPEN_GATE_SLOTS];
} OpenGate;

static inline OpenGateSlot *open_gate_find(OpenGate *g, const void *conn) {
    if (!conn) return NULL;
    for (i32 i = 0; i < OPEN_GATE_SLOTS; i++)
        if (g->slot[i].conn == conn) return &g->slot[i];
    return NULL;
}

static inline OpenGateSlot *open_gate_take(OpenGate *g, const void *conn) {
    if (!conn) return NULL;
    OpenGateSlot *s = open_gate_find(g, conn);
    if (s) return s;
    for (i32 i = 0; i < OPEN_GATE_SLOTS; i++) {
        if (g->slot[i].conn) continue;
        g->slot[i].conn = conn;
        g->slot[i].holder = NULL;
        g->slot[i].reserved = 0;
        return &g->slot[i];
    }
    return NULL;
}

static inline void open_gate_trim(OpenGateSlot *s) {
    if (s && !s->holder && s->reserved <= 0) { s->conn = NULL; s->holder = NULL; s->reserved = 0; }
}

/* May `who` start a NEW open on `conn`? `who` is the Unix-socket set asking, or
 * NULL for the TCP / SOCKS forwards. Not while another set's open is in flight
 * and not while a pane has reserved the connection. (An open that is already
 * in flight is never asked about: its user carries it to the end.) */
static inline bool open_gate_blocked(const OpenGate *g, const void *conn, const void *who) {
    if (!conn) return false;
    for (i32 i = 0; i < OPEN_GATE_SLOTS; i++) {
        const OpenGateSlot *s = &g->slot[i];
        if (s->conn != conn) continue;
        return s->reserved > 0 || (s->holder && s->holder != who);
    }
    return false;
}

/* The set `who` has an open in flight on `conn` (held) or has none (released).
 * Taking a hold that another set already has changes nothing; a full table holds
 * nothing, and the TCP forwards then ask on their own turn only. */
static inline void open_gate_hold(OpenGate *g, const void *conn, const void *who, bool held) {
    if (!conn || !who) return;
    if (held) {
        OpenGateSlot *s = open_gate_take(g, conn);
        if (s && !s->holder) s->holder = who;
        return;
    }
    OpenGateSlot *s = open_gate_find(g, conn);
    if (s && s->holder == who) s->holder = NULL;
    open_gate_trim(s);
}

/* A pane that borrows `conn` is about to open its channels (on) or has done (off).
 * Counted: several can wait together. */
static inline void open_gate_reserve(OpenGate *g, const void *conn, bool on) {
    if (!conn) return;
    if (on) {
        OpenGateSlot *s = open_gate_take(g, conn);
        if (s) s->reserved++;
        return;
    }
    OpenGateSlot *s = open_gate_find(g, conn);
    if (s && s->reserved > 0) s->reserved--;
    open_gate_trim(s);
}

/* Is a Unix-socket set's open in flight on `conn`? */
static inline bool open_gate_held(const OpenGate *g, const void *conn) {
    if (!conn) return false;
    for (i32 i = 0; i < OPEN_GATE_SLOTS; i++)
        if (g->slot[i].conn == conn) return g->slot[i].holder != NULL;
    return false;
}

/* The connection is gone: nothing about it is kept. */
static inline void open_gate_forget(OpenGate *g, const void *conn) {
    OpenGateSlot *s = open_gate_find(g, conn);
    if (s) { s->conn = NULL; s->holder = NULL; s->reserved = 0; }
}

#endif /* SSH_OPEN_GATE_H */
