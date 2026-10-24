/*
 * link_sees — what the link policy (ssh/link_policy.h) is told a session is.
 *
 * A connection that is waiting for the user to answer the SERVER's own prompt
 * (keyboard-interactive: a password, a one-time code) is an attempt in
 * flight, not "down" — the one state that is neither connected nor
 * connecting. Read as down, the pane would say "Could not connect to
 * user@host: no reason given — press Enter to try again" while that dialog is
 * up, and an Enter there would cancel the attempt the dialog is answering.
 *
 * Pure.
 */
#ifndef SSH_LINK_SEES_H
#define SSH_LINK_SEES_H

#include "ssh/link_policy.h"

/* `connected`: the shell's channel is up. `connecting`: connecting or
 * authenticating. `waiting_on_user`: the worker is blocked on the server's
 * prompt until the dialog answers. Anything else is down. */
static inline LinkSees link_sees_session(bool connected, bool connecting, bool waiting_on_user) {
    if (connected) return LINK_SEES_CONNECTED;
    if (connecting || waiting_on_user) return LINK_SEES_CONNECTING;
    return LINK_SEES_DOWN;
}

#endif /* SSH_LINK_SEES_H */
