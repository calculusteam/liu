/*
 * liu-notify - AF_UNIX socket helpers.
 * Separate from core/net.c: the daemon must never unlink a live socket.
 */
#ifndef NOTIFY_SOCKET_H
#define NOTIFY_SOCKET_H

#include "core/types.h"
#include <sys/types.h>

/* Fill `out` with the notify socket path. Returns true on success. */
bool notify_socket_path(char *out, usize out_sz);

/* Fill `out` with the PID/lock file path (socket_path + ".pid"). */
bool notify_pid_path(char *out, usize out_sz);

/* Create a listening AF_UNIX SOCK_STREAM at `path`.
 *   Returns  >= 0  : listening fd, ready for accept().
 *   Returns  -1    : hard error (errno set).
 *   Returns  -2    : another daemon is live at that path (caller should exit 0).
 * Side effects on success: O_CLOEXEC fd, fchmod 0600, backlog 8.
 * Never unconditionally unlinks — probes with connect() first.
 */
int notify_socket_listen(const char *path);

/* Connect to a listening AF_UNIX socket.
 *   Returns  >= 0  : connected fd (O_CLOEXEC set).
 *   Returns  -1    : failed (errno set — ECONNREFUSED/ENOENT mean no daemon).
 */
int notify_socket_connect(const char *path);

/* Verify peer euid == our euid. Returns true if same UID.
 * On failure or mismatch returns false and the caller should close(fd).
 * Also fetches peer PID on macOS/Linux if non-NULL (best-effort, for logs).
 */
bool notify_peer_uid_ok(int fd, i32 *out_peer_pid);

#endif /* NOTIFY_SOCKET_H */
