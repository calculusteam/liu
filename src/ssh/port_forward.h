#ifndef SSH_PORT_FORWARD_H
#define SSH_PORT_FORWARD_H

#include "core/types.h"
#include <libssh2.h>

typedef enum {
    FWD_LOCAL = 0,
    FWD_REMOTE = 1,
    FWD_DYNAMIC = 2,
} ForwardType;

typedef struct {
    ForwardType type;
    i32         local_port;
    char        remote_host[256];
    i32         remote_port;
    bool        active;
} ForwardRule;

i32  forward_add_local(LIBSSH2_SESSION *owner, i32 local_port, const char *remote_host, i32 remote_port);
i32  forward_add_socks5(LIBSSH2_SESSION *owner, i32 local_port);
void forward_remove(LIBSSH2_SESSION *owner, i32 index);
/* Returns true when a budget stopped it with work left (SSH latency S05):
 * the caller should come back without waiting for the next socket wake. */
bool forward_poll(LIBSSH2_SESSION *owner);
i32  forward_count(LIBSSH2_SESSION *owner);
bool forward_get(LIBSSH2_SESSION *owner, i32 index, ForwardRule *out);
/* libssh2 carries one channel open at a time per session — per CONNECTION, which
 * several panes can share (ssh/open_gate.h) — and continues it whatever the next
 * call asks for. True while a TCP / SOCKS forward's open of `owner` is in flight,
 * or while another Unix-socket set than `who` has one, or a borrowing pane has
 * reserved the connection: the set `who` then waits to start its own. The sets
 * hold the connection's open (forward_open_hold) while one of theirs is in
 * flight, so no TCP / SOCKS open starts meanwhile. */
bool forward_open_busy(LIBSSH2_SESSION *owner, const void *who);
void forward_open_hold(LIBSSH2_SESSION *owner, const void *who, bool held);
/* A pane that borrows the connection is about to open its channels: nothing new
 * starts until it has (forward_open_reserve off), and forward_open_idle says when
 * the open that was in flight has ended. */
void forward_open_reserve(LIBSSH2_SESSION *owner, bool on);
bool forward_open_idle(LIBSSH2_SESSION *owner);
/* The pane that owns the forwards is leaving a connection that goes on: a TCP /
 * SOCKS open of it that is in flight is carried to its end (the session is in
 * blocking mode, for a short bound) and its channel closed, because libssh2
 * cannot cancel it and the next open on the connection would be handed it. An
 * open the bound cuts short stays in flight: forward_cleanup_pane keeps it. */
void forward_open_settle(LIBSSH2_SESSION *owner);
void forward_cleanup(LIBSSH2_SESSION *owner);
/* The pane that started `owner`'s forwards leaves a connection other panes go
 * on using: its rules, connections and handshakes end as forward_cleanup ends
 * them, but an open still in flight stays as an orphan that the connection's
 * next forward poll carries to its end before any new open starts — libssh2
 * would hand its channel to the next open on the connection. */
void forward_cleanup_pane(LIBSSH2_SESSION *owner);
/* An open on `owner` that no forward knows (a Unix-socket forward's, left in
 * flight when its set ended) is carried to its end the same way: an orphan
 * whose own request is never sent, since libssh2 continues the open it has. */
void forward_open_carry(LIBSSH2_SESSION *owner);
void forward_cleanup_all(void);

#endif
