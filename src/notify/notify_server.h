/*
 * Liu — in-process notification server.
 *
 * Hosts the same AF_UNIX server the standalone `liu-notify daemon` runs, but
 * inside the Liu GUI on a dedicated thread. This is what lets notifications
 * work *without* a separate persistent background process: the service lives
 * and dies with the app.
 *
 * Single-instance is enforced by the socket bind (a second Liu window, or a
 * leftover standalone daemon, simply keeps hosting). No pidfile is taken, so
 * `liu-notify stop` can never read the GUI's PID and SIGTERM it.
 */
#ifndef NOTIFY_SERVER_H
#define NOTIFY_SERVER_H

#include "core/types.h"

/* Bind the socket and start the serving thread. Returns false (and hosts
 * nothing) if another instance already holds the socket or setup fails.
 * Safe to call once at startup; idempotent while running. */
bool notify_server_start(void);

/* Signal the serving thread to stop, join it, and release the socket. */
void notify_server_stop(void);

/* Ask the running server to re-read notify.conf (after the Settings UI saves).
 * No-op when the server isn't hosted by this process. */
void notify_server_reload(void);

/* True when this process is the one hosting the server. */
bool notify_server_running(void);

#endif /* NOTIFY_SERVER_H */
