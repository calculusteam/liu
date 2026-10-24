/*
 * liu-notify - AF_UNIX socket helpers.
 * Separate from core/net.c: the daemon must never unlink a live socket.
 */
#ifndef NOTIFY_SOCKET_H
#define NOTIFY_SOCKET_H

#include "core/types.h"
#include <sys/types.h>

/* Fill `out` with the notify socket path. Returns true on success. The
 * directory it names is verified owner-only (0700, owned by this user) on
 * every branch, or the call fails. */
bool notify_socket_path(char *out, usize out_sz);

/* Fill `out` with the PID/lock file path (socket_path + ".pid"). */
bool notify_pid_path(char *out, usize out_sz);

/* Create a listening AF_UNIX SOCK_STREAM at `path`.
 *   Returns  >= 0  : listening fd, ready for accept().
 *   Returns  -1    : hard error (errno set).
 *   Returns  -2    : a live host owns that path (caller should not host) —
 *                    another process, or this one if it is already listening.
 * Side effects on success: O_CLOEXEC fd, node chmod 0600, backlog 64, and this
 * process holds the ownership lock `<path>.lock` until notify_socket_release()
 * / notify_socket_unlink_owned() or its own death.
 *
 * Ownership is that lock (a POSIX write lock), not a guess from connect(): a
 * node is removed only by a process that holds the lock AND has asked the node
 * itself whether a host from before the lock existed still answers on it. The
 * process umask is never touched. One hosted path per process. */
int notify_socket_listen(const char *path);

/* Stop hosting: close `listen_fd` (-1 = already closed), unlink the node this
 * process bound at `path`, THEN give the ownership lock up. A no-op beyond the
 * close when this process does not own `path` — it never removes a node that
 * belongs to somebody else. Every listener from notify_socket_listen() must
 * end here; `close(fd); unlink(path)` leaves the lock held. */
void notify_socket_release(int listen_fd, const char *path);

/* The exit-path form of the above, for code that cannot reach the listener's
 * descriptor (the GUI's atexit handler: Cmd+Q never joins the serving thread).
 * Unlinks the node and drops the lock only if THIS process owns `path`; safe
 * when it never listened, safe to call twice, safe in a fork()ed child (which
 * inherits the bookkeeping but not the lock), and never blocks. */
void notify_socket_unlink_owned(const char *path);

/* Connect to a listening AF_UNIX socket.
 *   Returns  >= 0  : connected fd (O_CLOEXEC set).
 *   Returns  -1    : failed (errno set). ENOENT means no host. ECONNREFUSED
 *                    does NOT by itself: a live host with a full accept queue
 *                    refuses too (macOS) — retry before believing it.
 */
int notify_socket_connect(const char *path);

/* Same, with a deadline: the socket carries SO_SNDTIMEO + SO_RCVTIMEO of
 * `timeout_ms`, set before connect(), so neither the connect (which blocks on a
 * full accept queue on Linux) nor a later read/write can wait longer than that.
 * A connect that ran out of time fails with errno == ETIMEDOUT. */
int notify_socket_connect_timeout(const char *path, i32 timeout_ms);

/* Verify peer euid == this process's euid. Returns true if same UID.
 * On failure or mismatch returns false and the caller should close(fd).
 * Also fetches peer PID on macOS/Linux if non-NULL (best-effort, for logs).
 */
bool notify_peer_uid_ok(int fd, i32 *out_peer_pid);

#endif /* NOTIFY_SOCKET_H */
