/*
 * forward_keep — the Port Forwarding dialog's rules, as the connection keeps
 * them.
 *
 * A forward belongs to the libssh2 session, and a reconnect makes a new one: a
 * rule the dialog added would go with the old connection and its port would
 * answer "connection refused" from then on. So a rule is also kept in the pane's
 * config, in a bounded list of PortForwardSpec — the TCP local forwards in
 * `local_forwards`, the SOCKS5 proxies (only `bind_port` is theirs) in
 * `socks_forwards` — and the new connection starts what is listed
 * (session_start_config_forwards_locked). Removing a rule from the dialog
 * forgets it; a split pane's config carries none of them (a second bind of the
 * port would fail).
 *
 * The list is addressed by its local port: one listener per port, so a rule
 * added twice is one entry. Pure: no session is needed.
 */
#ifndef SSH_FORWARD_KEEP_H
#define SSH_FORWARD_KEEP_H

#include "ssh/ssh_session.h"

/* The entry whose local port is `port`, or -1. */
static inline i32 forward_keep_find(const PortForwardSpec *list, i32 count, i32 port) {
    for (i32 i = 0; list && i < count; i++)
        if (list[i].bind_port == port) return i;
    return -1;
}

/* May a rule for `port` be added to a list holding `count`? Not when the list
 * is full (MAX_PORT_FORWARDS, as the config's own forwards are bounded) or the
 * port is already kept. */
static inline bool forward_keep_may_add(const PortForwardSpec *list, i32 count, i32 port) {
    if (port <= 0 || port > 65535) return false;
    if (count < 0 || count >= MAX_PORT_FORWARDS) return false;
    return forward_keep_find(list, count, port) < 0;
}

/* Forgets the entry for `port`, closing the gap. True when there was one. */
static inline bool forward_keep_drop(PortForwardSpec *list, i32 *count, i32 port) {
    if (!list || !count) return false;
    i32 at = forward_keep_find(list, *count, port);
    if (at < 0) return false;
    for (i32 i = at; i + 1 < *count; i++) list[i] = list[i + 1];
    (*count)--;
    return true;
}

#endif /* SSH_FORWARD_KEEP_H */
