/*
 * relay_watch — when the main loop is woken for a forwarded connection's local
 * socket.
 *
 * The loop polls only the sessions something announced (core/poll_budget.h): an
 * fd that fired, input, a write still queued. A port forward's local listener
 * and the sockets it accepted must be watched too, or a connection to a
 * forwarded port — and every byte its client sends — waits for a wake that has
 * nothing to do with it (a key press, the shell printing), and with the pane
 * idle nothing comes at all. The same holds for the local end of a
 * RemoteForward, a Unix-socket forward and a forwarded X11 connection.
 *
 * A listener is watched while it exists. An accepted socket is watched only
 * while its relay can take what arrives there — nothing of its peer's is still
 * waiting for the channel, and the peer has not ended its side: on a
 * level-triggered poll a readable socket the relay will not read wakes the loop
 * for ever. What the SERVER sends arrives on the SSH socket, which is watched
 * already, and frees the channel when it has room again.
 *
 * Pure.
 */
#ifndef SSH_RELAY_WATCH_H
#define SSH_RELAY_WATCH_H

#include "core/types.h"

/* `open`: the relay has a socket. `peer_eof`: the socket's peer has said it
 * will send no more. `waiting`: bytes read from the socket that the channel
 * has not taken yet. */
static inline bool relay_wants_read_watch(bool open, bool peer_eof, usize waiting) {
    return open && !peer_eof && waiting == 0;
}

#endif /* SSH_RELAY_WATCH_H */
