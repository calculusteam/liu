/*
 * streamlocal_open — the open request of a Unix-socket forward, and what a
 * pending one does on each poll.
 *
 * A forward to a Unix socket on the server is a channel of type
 * "direct-streamlocal@openssh.com". After the header every channel open
 * carries (type, sender channel, window, packet size), its request holds
 *
 *     string  socket path
 *     string  reserved     ""
 *     uint32  reserved     0
 *
 * Liu builds those bytes itself and opens the channel with
 * libssh2_channel_open_ex, which every libssh2 has. The ready-made
 * libssh2_channel_direct_streamlocal_ex arrived only in libssh2 1.11.0, and
 * the Linux release is built against Ubuntu 22.04's 1.10 and runs against
 * whatever libssh2 the system has, where that call may be missing.
 *
 * Once connected the session is non-blocking, so an open answers "would
 * block" until the server's confirmation comes back. The connection that
 * asked for it waits for that answer: "would block" is not a failure.
 *
 * Pure (no libssh2, no I/O).
 */
#ifndef SSH_STREAMLOCAL_OPEN_H
#define SSH_STREAMLOCAL_OPEN_H

#include "core/types.h"
#include <string.h>

#define STREAMLOCAL_CHANNEL_TYPE     "direct-streamlocal@openssh.com"
#define STREAMLOCAL_CHANNEL_TYPE_LEN ((u32)(sizeof STREAMLOCAL_CHANNEL_TYPE - 1))

/* The bytes a path of `path_len` takes in the request: its length word, the
 * path, the empty reserved string's length word, the reserved word. */
#define STREAMLOCAL_OPEN_PAYLOAD_LEN(path_len) ((usize)(path_len) + 12u)

static inline void streamlocal_put_u32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* Writes the type-specific part of the open into `out` and returns its
 * length; 0 when there is no path or `out` cannot hold the request. */
static inline u32 streamlocal_open_payload(u8 *out, usize cap, const char *path) {
    if (!out || !path) return 0;
    usize n = strlen(path);
    if (n == 0 || n > 0xffffu || cap < STREAMLOCAL_OPEN_PAYLOAD_LEN(n)) return 0;
    u8 *p = out;
    streamlocal_put_u32(p, (u32)n);
    p += 4;
    memcpy(p, path, n);
    p += n;
    streamlocal_put_u32(p, 0);       /* reserved string: empty */
    p += 4;
    streamlocal_put_u32(p, 0);       /* reserved uint32 */
    return (u32)STREAMLOCAL_OPEN_PAYLOAD_LEN(n);
}

typedef enum {
    STREAMLOCAL_OPEN_WAIT = 0,   /* no answer yet: keep the connection, ask again next poll */
    STREAMLOCAL_OPEN_READY,      /* a channel: start relaying */
    STREAMLOCAL_OPEN_FAILED,     /* refused, or the session failed: close the connection */
} StreamlocalOpenStep;

/* One poll of a pending open. `got_channel`: the open returned a channel.
 * `would_block`: it did not, and the session's last error is EAGAIN. */
static inline StreamlocalOpenStep streamlocal_open_step(bool got_channel, bool would_block) {
    if (got_channel) return STREAMLOCAL_OPEN_READY;
    return would_block ? STREAMLOCAL_OPEN_WAIT : STREAMLOCAL_OPEN_FAILED;
}

#endif /* SSH_STREAMLOCAL_OPEN_H */
