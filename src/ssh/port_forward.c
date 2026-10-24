/*
 * Liu - SSH port forwarding via libssh2
 * Supports local and dynamic SOCKS5 forwarding.
 */
#include "ssh/port_forward.h"
#include "core/net.h"
#include "ssh/socks5.h"
#include "ssh/relay_watch.h"
#include "platform/platform.h"   /* platform_time_sec — the handshake deadline */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_FORWARD_RULES 32
#define MAX_FORWARD_CONNS 64
#define MAX_FORWARD_OPENING 16
#define FORWARD_OPEN_SEC 10.0

typedef struct {
    ForwardRule     info;
    LIBSSH2_SESSION *owner;
    i32             listen_fd;
} ForwardRuleSlot;

typedef struct {
    bool            active;
    LIBSSH2_SESSION *owner;
    i32             rule_slot;
    i32             client_fd;
    bool            watched;   /* client_fd wakes the loop (ssh/relay_watch.h) */
    LIBSSH2_CHANNEL *channel;
    /* Channel->client backpressure buffer: bytes read from the SSH channel
     * that the client socket couldn't accept yet (write() EAGAIN). They are
     * flushed before reading more from the channel, so a slow client stalls the
     * stream instead of silently dropping proxied bytes. */
    u8              pending[8192];
    usize           pending_len;
    usize           pending_off;
    /* Client->channel counterpart. libssh2 can return EAGAIN; retain the
     * unread tail across frames instead of busy-spinning the UI thread. */
    u8              upstream[8192];
    usize           upstream_len;
    usize           upstream_off;
    /* The two directions end separately. A single `closed` flag for both would
     * tear the connection down on the client's EOF (a request written, then
     * shutdown(SHUT_WR) — the normal way to say "that is all") before the
     * reply was read, and the channel's EOF would drop bytes the client
     * socket had not taken yet. A connection is finished when each side has
     * been told the other is, or on a hard error.
     *   client_eof  the client will send no more (read() returned 0)
     *   eof_sent    that has been passed to the channel (send_eof done)
     *   chan_eof    the server will send no more and its data is all read
     *   shut_wr     the client has been told so (shutdown(SHUT_WR) done) */
    bool            client_eof, eof_sent, chan_eof, shut_wr;
    f64             linger_until;   /* a client that never half-closes back */
} ForwardConn;

/* After the server is done and the client told, how long a client may keep its
 * own side open before the connection is closed for it. */
#define FWD_LINGER_SEC 30.0

/* A direct-tcpip channel open is itself an SSH round trip. libssh2's blocking
 * form would run that round trip inside forward_poll() on the UI thread,
 * undoing the non-blocking SOCKS handshake below. Keep the accepted socket
 * here while the non-blocking open advances over later polls. */
typedef struct {
    bool             active;
    bool             socks_reply;
    int              fd;
    LIBSSH2_SESSION *owner;
    i32              rule_slot;
    char             host[256];
    i32              port;
    f64              deadline;
} ForwardOpening;

/* ---------------------------------------------------------------------------
 * SOCKS5 handshake — a state machine, not a blocking read
 *
 * accept_rule_connection() runs from the UI poll path. Reading the SOCKS5
 * greeting + request there with the client socket in BLOCKING mode, even
 * bounded by a receive timeout, would let a client that sends one byte and
 * stops freeze the entire terminal for that timeout PER READ — and the
 * handshake makes up to six of them — from any process on the machine that
 * can reach the SOCKS port. Several such clients are additive.
 *
 * The handshake advances only over bytes that are already there. Each
 * pending connection keeps its own partial state, is fed whatever a
 * non-blocking read returns, and is dropped when its monotonic deadline
 * passes. No path here can block. */
/* Bounded: more than this many clients mid-handshake at once is refused
 * cleanly. Each entry is ~600 bytes, and a handshake normally lives for one
 * poll. */
#define MAX_SOCKS_PENDING   16
#define SOCKS_HANDSHAKE_SEC 10.0

typedef struct {
    bool             active;
    int              fd;
    LIBSSH2_SESSION *owner;
    i32              rule_slot;
    Socks5Parser     parser;
    f64              deadline;   /* platform_time_sec() when this is abandoned */
} SocksPending;

typedef struct {
    ForwardRuleSlot rules[MAX_FORWARD_RULES];
    i32             rule_hi;
    SocksPending    socks[MAX_SOCKS_PENDING];
    /* Lazy — each accepted connection owns a heap ForwardConn (16 KB of relay
     * buffers) that is freed when it closes. Keeping the whole array inline
     * pinned ~1 MB of .bss from process start even though most sessions never
     * open a single forward. A NULL slot is a free slot; every loop over
     * MAX_FORWARD_CONNS must tolerate NULLs. */
    ForwardConn    *conns[MAX_FORWARD_CONNS];
    ForwardOpening  opening[MAX_FORWARD_OPENING];
    /* Round-robin start for poll_connections: with a budget per call, the
     * connection served first would otherwise be served first every frame. */
    i32             rr_next;
} ForwardManager;

static ForwardManager g_fwd = {0};

/* g_fwd is PROCESS-global while every caller only holds its own session's
 * io_lock: the UI thread polls owner A's rules and walks every conn slot
 * while the tab-trash worker can run forward_cleanup() for owner B, freeing
 * a ForwardConn the poll is about to read. One mutex around every public
 * entry point. Nothing under it takes a Session lock, so the order io_lock
 * → g_fwd_mu can never invert. */
static pthread_mutex_t g_fwd_mu = PTHREAD_MUTEX_INITIALIZER;
#define FWD_LOCK()   pthread_mutex_lock(&g_fwd_mu)
#define FWD_UNLOCK() pthread_mutex_unlock(&g_fwd_mu)

static bool set_blocking(int fd, bool blocking) {
    int flags;
    do { flags = fcntl(fd, F_GETFL, 0); }
    while (flags < 0 && errno == EINTR);
    if (flags < 0) return false;
    if (blocking) flags &= ~O_NONBLOCK;
    else flags |= O_NONBLOCK;
    int rc;
    do { rc = fcntl(fd, F_SETFL, flags); }
    while (rc < 0 && errno == EINTR);
    return rc == 0;
}

static bool set_cloexec(int fd) {
    int flags;
    do { flags = fcntl(fd, F_GETFD, 0); }
    while (flags < 0 && errno == EINTR);
    if (flags < 0) return false;
    int rc;
    do { rc = fcntl(fd, F_SETFD, flags | FD_CLOEXEC); }
    while (rc < 0 && errno == EINTR);
    return rc == 0;
}

static ssize_t socket_send(int fd, const void *buf, usize len) {
#ifdef MSG_NOSIGNAL
    return send(fd, buf, len, MSG_NOSIGNAL);
#else
    return send(fd, buf, len, 0);
#endif
}

static bool socket_send_all(int fd, const void *buf, usize len) {
    const u8 *p = buf;
    usize sent = 0;
    while (sent < len) {
        ssize_t n = socket_send(fd, p + sent, len - sent);
        if (n > 0) sent += (usize)n;
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

/* Create a listening TCP socket */
static int create_listener(int port) {
    if (port <= 0 || port > 65535) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (!set_cloexec(fd)) { close(fd); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u16)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }

    if (!set_blocking(fd, false)) { close(fd); return -1; }
    return fd;
}

static int alloc_rule_slot(void) {
    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        if (!g_fwd.rules[i].info.active) return i;
    }
    if (g_fwd.rule_hi >= MAX_FORWARD_RULES) return -1;
    return g_fwd.rule_hi++;
}

/* Index of a free slot; does not allocate (the ForwardConn is only committed
 * once the connection can actually be started, so failure paths leak nothing). */
static int alloc_conn_slot(void) {
    for (i32 i = 0; i < MAX_FORWARD_CONNS; i++) {
        if (!g_fwd.conns[i]) return i;
    }
    return -1;
}

/* The loop is woken for a connection's client socket only while the relay
 * can take what arrives there (ssh/relay_watch.h). */
static void conn_watch_update(ForwardConn *conn) {
    bool want = relay_wants_read_watch(conn->client_fd >= 0, conn->client_eof,
                                       conn->upstream_len);
    if (want == conn->watched) return;
    if (want) platform_watch_socket(conn->client_fd);
    else      platform_unwatch_socket(conn->client_fd);
    conn->watched = want;
}

static void close_conn_slot(i32 idx) {
    if (idx < 0 || idx >= MAX_FORWARD_CONNS) return;
    ForwardConn *conn = g_fwd.conns[idx];
    if (!conn) return;
    if (conn->channel) {
        libssh2_channel_close(conn->channel);
        libssh2_channel_free(conn->channel);
    }
    if (conn->watched) platform_unwatch_socket(conn->client_fd);
    if (conn->client_fd >= 0) close(conn->client_fd);
    free(conn);
    g_fwd.conns[idx] = NULL;
}

static void socks_reply(int fd, u8 code) {
    u8 resp[10] = { 0x05, code, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
    (void)socket_send_all(fd, resp, sizeof(resp));
}

/* Defined below; the handshake's terminal state hands the socket to it. */
static bool queue_connection_open(LIBSSH2_SESSION *owner, i32 rule_slot,
                                  int client_fd, const char *host, i32 port,
                                  bool socks_reply_on_open);

/* ---------------------------------------------------------------------------
 * SOCKS5 handshake, driven from the poll loop. Nothing below may block.
 * The wire format itself lives in ssh/socks5.h; this half is only the socket.
 * ------------------------------------------------------------------------- */

static void socks_pending_drop(SocksPending *p) {
    if (!p->active) return;
    if (p->fd >= 0) { platform_unwatch_socket(p->fd); close(p->fd); }
    memset(p, 0, sizeof *p);
    p->fd = -1;
}

/* Advance one pending handshake over whatever bytes are already available.
 * Returns false when the connection is finished with (handed off or dropped). */
static bool socks_pending_step(SocksPending *p) {
    for (;;) {
        u8 chunk[512];
        /* Exactly the bytes the parser is waiting for, never more: a read past
         * the end of the request would hand the tail (an optimistic first
         * payload) to a handshake that is already finished with, and it would
         * be thrown away. What follows the request stays in the socket for the
         * relay. */
        usize want = p->parser.need > p->parser.got
                   ? (usize)(p->parser.need - p->parser.got) : 1u;
        if (want > sizeof chunk) want = sizeof chunk;
        ssize_t n = read(p->fd, chunk, want);
        if (n == 0) { socks_pending_drop(p); return false; }   /* orderly close */
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;  /* later */
            socks_pending_drop(p);
            return false;
        }

        usize off = 0;
        while (off < (usize)n) {
            usize used = 0;
            Socks5Status st = socks5_feed(&p->parser, chunk + off,
                                          (usize)n - off, &used);
            off += used;
            switch (st) {
            case SOCKS5_NEED_MORE:
                off = (usize)n;              /* chunk exhausted; read again */
                break;
            case SOCKS5_SEND_METHOD_OK: {
                u8 resp[2] = { 0x05, 0x00 };
                if (!socket_send_all(p->fd, resp, sizeof resp)) {
                    socks_pending_drop(p);
                    return false;
                }
                continue;                    /* keep parsing this chunk */
            }
            case SOCKS5_DONE: {
                /* The opening table takes ownership of the fd on success.
                 * SOCKS success is sent only after the SSH channel really
                 * opens, never while libssh2 is still returning EAGAIN. */
                int fd = p->fd;
                /* The client waits for its reply now; the channel's opening
                 * is what moves things on. */
                platform_unwatch_socket(fd);
                if (!queue_connection_open(p->owner, p->rule_slot, fd,
                                           p->parser.host, p->parser.port,
                                           true)) {
                    socks_reply(fd, 0x05);   /* general failure */
                    socks_pending_drop(p);
                    return false;
                }
                p->fd = -1;                  /* opening slot owns it now */
                memset(p, 0, sizeof *p);
                p->fd = -1;
                return false;
            }
            case SOCKS5_ERR_NO_NOAUTH: {
                u8 resp[2] = { 0x05, 0xFF };
                (void)socket_send_all(p->fd, resp, sizeof resp);
                socks_pending_drop(p);
                return false;
            }
            case SOCKS5_ERR_COMMAND:
                socks_reply(p->fd, 0x07);
                socks_pending_drop(p);
                return false;
            case SOCKS5_ERR_ATYP:
                socks_reply(p->fd, 0x08);
                socks_pending_drop(p);
                return false;
            case SOCKS5_ERR_VERSION:
            case SOCKS5_ERR_PROTOCOL:
                socks_pending_drop(p);       /* not SOCKS5: say nothing */
                return false;
            }
        }
        /* Reads are sized to a field, so a short read is normal: keep going
         * until the socket says EAGAIN (the handshake is a few dozen bytes). */
    }
}

/* Called every poll: advance each pending handshake and time out the stalled. */
static void socks_poll_pending(LIBSSH2_SESSION *owner) {
    f64 now = platform_time_sec();
    for (i32 i = 0; i < MAX_SOCKS_PENDING; i++) {
        SocksPending *p = &g_fwd.socks[i];
        if (!p->active || p->owner != owner) continue;
        if (now > p->deadline) { socks_pending_drop(p); continue; }
        socks_pending_step(p);
    }
}

/* Adopt an accepted client socket into the handshake table. Returns false when
 * the table is full or the socket cannot be made non-blocking — the caller
 * closes the fd in that case. */
static bool socks_pending_begin(LIBSSH2_SESSION *owner, i32 rule_slot, int fd) {
    if (!set_blocking(fd, false)) return false;
    for (i32 i = 0; i < MAX_SOCKS_PENDING; i++) {
        SocksPending *p = &g_fwd.socks[i];
        if (p->active) continue;
        memset(p, 0, sizeof *p);
        p->active    = true;
        p->fd        = fd;
        p->owner     = owner;
        p->rule_slot = rule_slot;
        p->deadline  = platform_time_sec() + SOCKS_HANDSHAKE_SEC;
        socks5_init(&p->parser);
        /* A greeting that arrives in pieces wakes the loop for the rest. */
        platform_watch_socket(fd);
        /* Whatever already arrived is consumed now, so a client that sent the
         * whole greeting in one packet is handed off without waiting a frame. */
        socks_pending_step(p);
        return true;
    }
    return false;
}

static bool install_connection(LIBSSH2_SESSION *owner, i32 rule_slot,
                               int client_fd, LIBSSH2_CHANNEL *ch) {
    if (!owner || client_fd < 0 || !ch) {
        if (ch) {
            libssh2_channel_close(ch);
            libssh2_channel_free(ch);
        }
        return false;
    }
    int slot = alloc_conn_slot();
    if (slot < 0) {
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
        return false;
    }

    ForwardConn *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
        return false;
    }
    libssh2_channel_set_blocking(ch, 0);

    conn->active = true;
    conn->owner = owner;
    conn->rule_slot = rule_slot;
    conn->client_fd = client_fd;
    conn->channel = ch;
    g_fwd.conns[slot] = conn;
    conn_watch_update(conn);
    return true;
}

static void opening_drop(ForwardOpening *p) {
    if (!p || !p->active) return;
    if (p->fd >= 0) close(p->fd);
    memset(p, 0, sizeof *p);
    p->fd = -1;
}

static bool queue_connection_open(LIBSSH2_SESSION *owner, i32 rule_slot,
                                  int client_fd, const char *host, i32 port,
                                  bool socks_reply_on_open) {
    if (!owner || client_fd < 0 || !host || !host[0] ||
        strnlen(host, sizeof(g_fwd.opening[0].host)) >= sizeof(g_fwd.opening[0].host) ||
        port <= 0 || port > 65535) return false;
    if (!set_blocking(client_fd, false)) return false;

    for (i32 i = 0; i < MAX_FORWARD_OPENING; i++) {
        ForwardOpening *p = &g_fwd.opening[i];
        if (p->active) continue;
        memset(p, 0, sizeof *p);
        p->active      = true;
        p->socks_reply = socks_reply_on_open;
        p->fd          = client_fd;
        p->owner       = owner;
        p->rule_slot   = rule_slot;
        p->port        = port;
        p->deadline    = platform_time_sec() + FORWARD_OPEN_SEC;
        snprintf(p->host, sizeof p->host, "%s", host);
        /* Session traffic is owned by the main poll thread and is normally
         * already non-blocking. Make the requirement explicit so the channel
         * open below can never park that thread. */
        libssh2_session_set_blocking(owner, 0);
        return true;
    }
    return false;
}

static void poll_opening_connections(LIBSSH2_SESSION *owner) {
    f64 now = platform_time_sec();
    for (i32 i = 0; i < MAX_FORWARD_OPENING; i++) {
        ForwardOpening *p = &g_fwd.opening[i];
        if (!p->active || p->owner != owner) continue;

        if (now > p->deadline) {
            if (p->socks_reply) socks_reply(p->fd, 0x05);
            opening_drop(p);
            continue;
        }

        LIBSSH2_CHANNEL *ch = libssh2_channel_direct_tcpip(owner, p->host, p->port);
        if (!ch) {
            if (libssh2_session_last_errno(owner) == LIBSSH2_ERROR_EAGAIN)
                continue;
            if (p->socks_reply) socks_reply(p->fd, 0x05);
            opening_drop(p);
            continue;
        }

        int fd = p->fd;
        bool wants_reply = p->socks_reply;
        if (!install_connection(owner, p->rule_slot, fd, ch)) {
            if (wants_reply) socks_reply(fd, 0x05);
            /* install_connection owns and releases ch on every failure. */
            opening_drop(p);
            continue;
        }
        if (wants_reply) socks_reply(fd, 0x00);
        p->fd = -1;                         /* ForwardConn owns it */
        opening_drop(p);
    }
}

static void accept_rule_connection(ForwardRuleSlot *slot) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(slot->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) return;
    if (!set_cloexec(client_fd)) { close(client_fd); return; }
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif

    if (slot->info.type == FWD_LOCAL) {
        i32 rule_slot = (i32)(slot - g_fwd.rules);
        if (!queue_connection_open(slot->owner, rule_slot, client_fd,
                                   slot->info.remote_host,
                                   slot->info.remote_port, false)) {
            close(client_fd);
        }
        return;
    }

    if (slot->info.type == FWD_DYNAMIC) {
        /* The handshake is adopted into the pending table and advanced from
         * forward_poll(); nothing about it blocks the UI. A full table means
         * more than MAX_SOCKS_PENDING clients are mid-handshake at once, which
         * is refused cleanly rather than served by stalling the terminal. */
        i32 rule_slot = (i32)(slot - g_fwd.rules);
        if (!socks_pending_begin(slot->owner, rule_slot, client_fd))
            close(client_fd);
        return;
    }

    close(client_fd);
}

/* The relay's poll budget. This runs on the UI thread under the session's io
 * lock, once per frame per session. Draining every connection in both
 * directions until EAGAIN, EOF or backpressure would let a saturated tunnel (a
 * download through a SOCKS rule, a port-forwarded video stream) keep the loop
 * here for as long as the kernel and the SSH window keep handing it data, with
 * the frame's input and render waiting behind it. Each connection moves at
 * most FWD_POLL_CONN_BYTES per direction per call and the call stops at
 * FWD_POLL_TOTAL_BYTES; the start index rotates so no connection is always
 * first; and the caller is told whether work was left, so the loop comes back
 * for it instead of waiting for the next fd wake. */
#define FWD_POLL_CONN_BYTES  ((usize)(64u * 1024u))
#define FWD_POLL_TOTAL_BYTES ((usize)(256u * 1024u))

static bool poll_connections(LIBSSH2_SESSION *owner) {
    char buf[8192];
    usize total_moved = 0;
    bool more = false;

    for (i32 step = 0; step < MAX_FORWARD_CONNS; step++) {
        i32 i = (g_fwd.rr_next + step) % MAX_FORWARD_CONNS;
        ForwardConn *conn = g_fwd.conns[i];
        if (!conn || !conn->active || conn->owner != owner) continue;
        if (total_moved >= FWD_POLL_TOTAL_BYTES) { more = true; break; }

        bool closed = false;
        usize up_moved = 0, down_moved = 0;

        while (conn->upstream_off < conn->upstream_len) {
            ssize_t w = libssh2_channel_write(
                conn->channel, (const char *)conn->upstream + conn->upstream_off,
                conn->upstream_len - conn->upstream_off);
            if (w == LIBSSH2_ERROR_EAGAIN) break;
            if (w <= 0) { closed = true; break; }
            conn->upstream_off += (usize)w;
            up_moved += (usize)w;
        }
        if (conn->upstream_off >= conn->upstream_len)
            conn->upstream_off = conn->upstream_len = 0;

        while (!closed && !conn->client_eof && conn->upstream_len == 0) {
            if (up_moved >= FWD_POLL_CONN_BYTES) { more = true; break; }
            ssize_t n = read(conn->client_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                closed = true;
                break;
            }
            if (n == 0) {
                /* The client has nothing more to send. That is not the end of
                 * the conversation: the reply is still on its way. */
                conn->client_eof = true;
                break;
            }

            ssize_t written = 0;
            while (written < n) {
                ssize_t w = libssh2_channel_write(conn->channel, buf + written, (usize)(n - written));
                if (w == LIBSSH2_ERROR_EAGAIN || w == 0) {
                    usize rem = (usize)(n - written);
                    memcpy(conn->upstream, buf + written, rem);
                    conn->upstream_len = rem;
                    conn->upstream_off = 0;
                    break;
                }
                if (w < 0) { closed = true; break; }
                written += w;
                up_moved += (usize)w;
            }
            if (closed || conn->upstream_len > 0) break;
        }

        /* Pass the client's EOF on once every byte it sent is in the channel.
         * EAGAIN means "not yet": tried again next poll, sent exactly once. */
        if (!closed && conn->client_eof && !conn->eof_sent && conn->upstream_len == 0) {
            int rc = libssh2_channel_send_eof(conn->channel);
            if (rc == 0) conn->eof_sent = true;
            else if (rc != LIBSSH2_ERROR_EAGAIN) closed = true;
        }

        if (!closed) {
            /* First, drain any bytes a prior frame couldn't push to the client
             * (backpressure). While anything is pending, nothing more is read
             * from the channel: reading would drop the un-written remainder
             * and corrupt the proxied stream. */
            while (conn->pending_off < conn->pending_len) {
                ssize_t w = socket_send(conn->client_fd,
                                        conn->pending + conn->pending_off,
                                        conn->pending_len - conn->pending_off);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* retry next frame */
                    closed = true;
                    break;
                }
                if (w == 0) { closed = true; break; }
                conn->pending_off += (usize)w;
                down_moved += (usize)w;
            }
            if (conn->pending_off >= conn->pending_len)
                conn->pending_off = conn->pending_len = 0;

            /* Only pull new channel data once the backpressure buffer is empty. */
            while (!closed && !conn->chan_eof && conn->pending_len == 0) {
                if (down_moved >= FWD_POLL_CONN_BYTES) { more = true; break; }
                ssize_t n = libssh2_channel_read(conn->channel, buf, sizeof(buf));
                if (n == LIBSSH2_ERROR_EAGAIN) break;
                if (n <= 0) {
                    if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) closed = true;
                    if (n == 0 && libssh2_channel_eof(conn->channel)) conn->chan_eof = true;
                    break;
                }

                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = socket_send(conn->client_fd, buf + written,
                                            (usize)(n - written));
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            /* Client socket full — stash the unwritten tail and
                             * deliver it next frame instead of dropping it. */
                            usize rem = (usize)(n - written);
                            memcpy(conn->pending, buf + written, rem);
                            conn->pending_len = rem;
                            conn->pending_off = 0;
                            break;
                        }
                        closed = true;
                        break;
                    }
                    if (w == 0) { closed = true; break; }
                    written += w;
                    down_moved += (usize)w;
                }
                if (closed || conn->pending_len > 0) break;
            }
        }

        /* The server's EOF ends only the server's half, and only once every
         * byte it sent has been taken by the client (pending is empty). */
        if (!closed && !conn->chan_eof && conn->pending_len == 0 &&
            libssh2_channel_eof(conn->channel))
            conn->chan_eof = true;
        if (!closed && conn->chan_eof && conn->pending_len == 0 && !conn->shut_wr) {
            /* Tell the client the server is finished. A failure here is the
             * client being gone, which the next read/write notices. */
            (void)shutdown(conn->client_fd, SHUT_WR);
            conn->shut_wr = true;
            conn->linger_until = platform_time_sec() + FWD_LINGER_SEC;
        }
        /* Finished: both sides have said they are done and been heard — or the
         * client has had its grace to send its own EOF and has not. */
        if (!closed && conn->shut_wr &&
            (conn->eof_sent || platform_time_sec() > conn->linger_until))
            closed = true;
        if (closed) close_conn_slot(i);
        else        conn_watch_update(conn);
        total_moved += up_moved + down_moved;
    }
    g_fwd.rr_next = (g_fwd.rr_next + 1) % MAX_FORWARD_CONNS;
    return more;
}

static i32 forward_count_locked(LIBSSH2_SESSION *owner);

static int nth_rule_slot(LIBSSH2_SESSION *owner, i32 index) {
    i32 seen = 0;
    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        if (!g_fwd.rules[i].info.active || g_fwd.rules[i].owner != owner) continue;
        if (seen == index) return i;
        seen++;
    }
    return -1;
}

i32 forward_add_local(LIBSSH2_SESSION *owner, i32 local_port, const char *remote_host, i32 remote_port) {
    if (!owner || local_port <= 0 || local_port > 65535 || !remote_host ||
        !remote_host[0] || strnlen(remote_host, 256) >= 256 ||
        remote_port <= 0 || remote_port > 65535) return -1;

    FWD_LOCK();
    int slot = alloc_rule_slot();
    if (slot < 0) { FWD_UNLOCK(); return -1; }

    int fd = create_listener(local_port);
    if (fd < 0) { FWD_UNLOCK(); return -1; }

    ForwardRuleSlot *r = &g_fwd.rules[slot];
    memset(r, 0, sizeof(*r));
    r->owner = owner;
    r->listen_fd = fd;
    platform_watch_socket(fd);     /* a connection wakes the loop (ssh/relay_watch.h) */
    r->info.type = FWD_LOCAL;
    r->info.local_port = local_port;
    snprintf(r->info.remote_host, sizeof(r->info.remote_host), "%s", remote_host);
    r->info.remote_port = remote_port;
    r->info.active = true;
    i32 idx = forward_count_locked(owner) - 1;
    FWD_UNLOCK();
    return idx;
}

i32 forward_add_socks5(LIBSSH2_SESSION *owner, i32 local_port) {
    if (!owner || local_port <= 0 || local_port > 65535) return -1;

    FWD_LOCK();
    int slot = alloc_rule_slot();
    if (slot < 0) { FWD_UNLOCK(); return -1; }

    int fd = create_listener(local_port);
    if (fd < 0) { FWD_UNLOCK(); return -1; }

    ForwardRuleSlot *r = &g_fwd.rules[slot];
    memset(r, 0, sizeof(*r));
    r->owner = owner;
    r->listen_fd = fd;
    platform_watch_socket(fd);
    r->info.type = FWD_DYNAMIC;
    r->info.local_port = local_port;
    r->info.active = true;
    i32 idx = forward_count_locked(owner) - 1;
    FWD_UNLOCK();
    return idx;
}

void forward_remove(LIBSSH2_SESSION *owner, i32 index) {
    FWD_LOCK();
    int slot = nth_rule_slot(owner, index);
    if (slot < 0) { FWD_UNLOCK(); return; }

    if (g_fwd.rules[slot].listen_fd >= 0) {
        platform_unwatch_socket(g_fwd.rules[slot].listen_fd);
        close(g_fwd.rules[slot].listen_fd);
    }
    g_fwd.rules[slot].listen_fd = -1;
    g_fwd.rules[slot].info.active = false;

    for (i32 i = 0; i < MAX_FORWARD_CONNS; i++) {
        ForwardConn *c = g_fwd.conns[i];
        if (c && c->active && c->owner == owner && c->rule_slot == slot) {
            close_conn_slot(i);
        }
    }
    for (i32 i = 0; i < MAX_SOCKS_PENDING; i++) {
        SocksPending *p = &g_fwd.socks[i];
        if (p->active && p->owner == owner && p->rule_slot == slot)
            socks_pending_drop(p);
    }
    for (i32 i = 0; i < MAX_FORWARD_OPENING; i++) {
        ForwardOpening *p = &g_fwd.opening[i];
        if (p->active && p->owner == owner && p->rule_slot == slot)
            opening_drop(p);
    }
    FWD_UNLOCK();
}

bool forward_poll(LIBSSH2_SESSION *owner) {
    if (!owner) return false;

    FWD_LOCK();
    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        ForwardRuleSlot *r = &g_fwd.rules[i];
        if (!r->info.active || r->owner != owner || r->listen_fd < 0) continue;
        accept_rule_connection(r);
    }

    /* SOCKS5 greetings that arrived in pieces. */
    socks_poll_pending(owner);

    /* direct-tcpip is an SSH round trip too; advance it without blocking the
     * UI thread and only then install the relay connection. */
    poll_opening_connections(owner);

    bool more = poll_connections(owner);
    FWD_UNLOCK();
    return more;
}

static i32 forward_count_locked(LIBSSH2_SESSION *owner) {
    i32 count = 0;
    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        if (g_fwd.rules[i].info.active && g_fwd.rules[i].owner == owner) count++;
    }
    return count;
}

i32 forward_count(LIBSSH2_SESSION *owner) {
    FWD_LOCK();
    i32 n = forward_count_locked(owner);
    FWD_UNLOCK();
    return n;
}

bool forward_get(LIBSSH2_SESSION *owner, i32 index, ForwardRule *out) {
    if (!out) return false;
    FWD_LOCK();
    int slot = nth_rule_slot(owner, index);
    if (slot < 0) { FWD_UNLOCK(); return false; }
    *out = g_fwd.rules[slot].info;
    FWD_UNLOCK();
    return true;
}

void forward_cleanup(LIBSSH2_SESSION *owner) {
    if (!owner) return;

    FWD_LOCK();
    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        if (g_fwd.rules[i].owner != owner) continue;
        if (g_fwd.rules[i].info.active && g_fwd.rules[i].listen_fd >= 0) {
            platform_unwatch_socket(g_fwd.rules[i].listen_fd);
            close(g_fwd.rules[i].listen_fd);
        }
        memset(&g_fwd.rules[i], 0, sizeof(g_fwd.rules[i]));
        g_fwd.rules[i].listen_fd = -1;
    }

    for (i32 i = 0; i < MAX_FORWARD_CONNS; i++) {
        ForwardConn *c = g_fwd.conns[i];
        if (c && c->active && c->owner == owner) {
            close_conn_slot(i);
        }
    }
    for (i32 i = 0; i < MAX_SOCKS_PENDING; i++) {
        SocksPending *p = &g_fwd.socks[i];
        if (p->active && p->owner == owner) socks_pending_drop(p);
    }
    for (i32 i = 0; i < MAX_FORWARD_OPENING; i++) {
        ForwardOpening *p = &g_fwd.opening[i];
        if (p->active && p->owner == owner) opening_drop(p);
    }
    FWD_UNLOCK();
}

void forward_cleanup_all(void) {
    FWD_LOCK();
    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        if (g_fwd.rules[i].info.active && g_fwd.rules[i].listen_fd >= 0) {
            platform_unwatch_socket(g_fwd.rules[i].listen_fd);
            close(g_fwd.rules[i].listen_fd);
        }
    }
    for (i32 i = 0; i < MAX_FORWARD_CONNS; i++) {
        close_conn_slot(i); /* no-op on NULL slots */
    }
    for (i32 i = 0; i < MAX_SOCKS_PENDING; i++)
        socks_pending_drop(&g_fwd.socks[i]);
    for (i32 i = 0; i < MAX_FORWARD_OPENING; i++)
        opening_drop(&g_fwd.opening[i]);
    memset(&g_fwd, 0, sizeof(g_fwd));
    FWD_UNLOCK();
}
