/*
 * Liu - SSH port forwarding via libssh2
 * Supports local and dynamic SOCKS5 forwarding.
 */
#include "ssh/port_forward.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/time.h>       /* struct timeval for SO_RCVTIMEO */
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_FORWARD_RULES 32
#define MAX_FORWARD_CONNS 64

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
    LIBSSH2_CHANNEL *channel;
    /* Channel->client backpressure buffer: bytes read from the SSH channel
     * that the client socket couldn't accept yet (write() EAGAIN). They are
     * flushed before reading more from the channel, so a slow client stalls the
     * stream instead of silently dropping proxied bytes. */
    u8              pending[8192];
    usize           pending_len;
    usize           pending_off;
} ForwardConn;

typedef struct {
    ForwardRuleSlot rules[MAX_FORWARD_RULES];
    i32             rule_hi;
    ForwardConn     conns[MAX_FORWARD_CONNS];
} ForwardManager;

static ForwardManager g_fwd = {0};

static void set_blocking(int fd, bool blocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    if (blocking) flags &= ~O_NONBLOCK;
    else flags |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}

/* Create a listening TCP socket */
static int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

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

    set_blocking(fd, false);
    return fd;
}

static int alloc_rule_slot(void) {
    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        if (!g_fwd.rules[i].info.active) return i;
    }
    if (g_fwd.rule_hi >= MAX_FORWARD_RULES) return -1;
    return g_fwd.rule_hi++;
}

static int alloc_conn_slot(void) {
    for (i32 i = 0; i < MAX_FORWARD_CONNS; i++) {
        if (!g_fwd.conns[i].active) return i;
    }
    return -1;
}

static void close_conn_slot(ForwardConn *conn) {
    if (!conn->active) return;
    if (conn->channel) {
        libssh2_channel_close(conn->channel);
        libssh2_channel_free(conn->channel);
    }
    if (conn->client_fd >= 0) close(conn->client_fd);
    memset(conn, 0, sizeof(*conn));
    conn->client_fd = -1;
}

static LIBSSH2_CHANNEL *open_direct_tcpip(LIBSSH2_SESSION *owner, const char *host, i32 port) {
    if (!owner || !host || !host[0] || port <= 0) return NULL;
    libssh2_session_set_blocking(owner, 1);
    LIBSSH2_CHANNEL *ch = libssh2_channel_direct_tcpip(owner, host, port);
    libssh2_session_set_blocking(owner, 0);
    return ch;
}

static bool read_exact(int fd, void *buf, usize len) {
    u8 *p = (u8 *)buf;
    usize got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        got += (usize)n;
    }
    return true;
}

static void socks_reply(int fd, u8 code) {
    u8 resp[10] = { 0x05, code, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
    (void)write(fd, resp, sizeof(resp));
}

static bool socks5_handshake(int fd, char *host_out, usize host_size, i32 *port_out) {
    u8 hdr[2];
    if (!read_exact(fd, hdr, 2)) return false;
    if (hdr[0] != 0x05 || hdr[1] == 0) return false;

    u8 methods[256];
    if (!read_exact(fd, methods, hdr[1])) return false;

    bool supports_noauth = false;
    for (u8 i = 0; i < hdr[1]; i++) {
        if (methods[i] == 0x00) {
            supports_noauth = true;
            break;
        }
    }
    if (!supports_noauth) {
        u8 resp[2] = { 0x05, 0xFF };
        (void)write(fd, resp, sizeof(resp));
        return false;
    }

    {
        u8 resp[2] = { 0x05, 0x00 };
        if (write(fd, resp, sizeof(resp)) != (ssize_t)sizeof(resp)) return false;
    }

    u8 req[4];
    if (!read_exact(fd, req, 4)) return false;
    if (req[0] != 0x05 || req[1] != 0x01) {
        socks_reply(fd, 0x07);
        return false;
    }

    char host[256] = {0};
    if (req[3] == 0x01) {
        u8 addr[4];
        if (!read_exact(fd, addr, sizeof(addr))) return false;
        snprintf(host, sizeof(host), "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3]);
    } else if (req[3] == 0x03) {
        u8 len = 0;
        if (!read_exact(fd, &len, 1)) return false;
        if (len == 0) return false;
        if (!read_exact(fd, host, len)) return false;
        host[len] = '\0';
    } else if (req[3] == 0x04) {
        u8 addr[16];
        if (!read_exact(fd, addr, sizeof(addr))) return false;
        if (!inet_ntop(AF_INET6, addr, host, (socklen_t)sizeof(host))) return false;
    } else {
        socks_reply(fd, 0x08);
        return false;
    }

    u8 port_be[2];
    if (!read_exact(fd, port_be, 2)) return false;

    snprintf(host_out, host_size, "%s", host);
    *port_out = ((i32)port_be[0] << 8) | port_be[1];
    return true;
}

static bool start_connection(LIBSSH2_SESSION *owner, i32 rule_slot, int client_fd,
                             const char *host, i32 port) {
    LIBSSH2_CHANNEL *ch = open_direct_tcpip(owner, host, port);
    if (!ch) return false;

    int slot = alloc_conn_slot();
    if (slot < 0) {
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
        return false;
    }

    set_blocking(client_fd, false);
    libssh2_channel_set_blocking(ch, 0);

    g_fwd.conns[slot].active = true;
    g_fwd.conns[slot].owner = owner;
    g_fwd.conns[slot].rule_slot = rule_slot;
    g_fwd.conns[slot].client_fd = client_fd;
    g_fwd.conns[slot].channel = ch;
    return true;
}

static void accept_rule_connection(ForwardRuleSlot *slot) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(slot->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) return;

    if (slot->info.type == FWD_LOCAL) {
        i32 rule_slot = (i32)(slot - g_fwd.rules);
        if (!start_connection(slot->owner, rule_slot, client_fd,
                              slot->info.remote_host, slot->info.remote_port)) {
            close(client_fd);
        }
        return;
    }

    if (slot->info.type == FWD_DYNAMIC) {
        char host[256];
        i32 port = 0;
        i32 rule_slot = (i32)(slot - g_fwd.rules);
        set_blocking(client_fd, true);
        /* Bound the handshake: socks5_handshake reads with blocking read_exact
         * on the main UI thread, so a stalled or hostile SOCKS client would
         * otherwise hang the whole terminal indefinitely. SO_RCVTIMEO makes the
         * reads time out (read() -> EAGAIN -> read_exact fails -> we close). */
        struct timeval rcv_to = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
        if (!socks5_handshake(client_fd, host, sizeof(host), &port)) {
            close(client_fd);
            return;
        }
        /* Handshake done — restore an unbounded recv for the proxied stream. */
        struct timeval rcv_off = { .tv_sec = 0, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_off, sizeof(rcv_off));
        if (!start_connection(slot->owner, rule_slot, client_fd, host, port)) {
            socks_reply(client_fd, 0x05);
            close(client_fd);
            return;
        }
        socks_reply(client_fd, 0x00);
        return;
    }

    close(client_fd);
}

static void poll_connections(LIBSSH2_SESSION *owner) {
    char buf[8192];

    for (i32 i = 0; i < MAX_FORWARD_CONNS; i++) {
        ForwardConn *conn = &g_fwd.conns[i];
        if (!conn->active || conn->owner != owner) continue;

        bool closed = false;

        for (;;) {
            ssize_t n = read(conn->client_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                closed = true;
                break;
            }
            if (n == 0) {
                closed = true;
                break;
            }

            ssize_t written = 0;
            while (written < n) {
                ssize_t w = libssh2_channel_write(conn->channel, buf + written, (usize)(n - written));
                /* poll_connections runs on the main UI thread every frame;
                 * never sleep here — a 1ms pause per EAGAIN serializes with
                 * keystroke echo and makes typing feel laggy. Busy-spin was
                 * the original behavior and kept the UI fluid. */
                if (w == LIBSSH2_ERROR_EAGAIN) continue;
                if (w < 0) { closed = true; break; }
                written += w;
            }
            if (closed) break;
        }

        if (!closed) {
            /* First, drain any bytes a prior frame couldn't push to the client
             * (backpressure). While anything is pending we must NOT read more
             * from the channel — doing so previously dropped the un-written
             * remainder, silently corrupting the proxied stream. */
            while (conn->pending_off < conn->pending_len) {
                ssize_t w = write(conn->client_fd, conn->pending + conn->pending_off,
                                  conn->pending_len - conn->pending_off);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* retry next frame */
                    closed = true;
                    break;
                }
                conn->pending_off += (usize)w;
            }
            if (conn->pending_off >= conn->pending_len)
                conn->pending_off = conn->pending_len = 0;

            /* Only pull new channel data once the backpressure buffer is empty. */
            while (!closed && conn->pending_len == 0) {
                ssize_t n = libssh2_channel_read(conn->channel, buf, sizeof(buf));
                if (n == LIBSSH2_ERROR_EAGAIN) break;
                if (n <= 0) {
                    if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) closed = true;
                    if (n == 0 && libssh2_channel_eof(conn->channel)) closed = true;
                    break;
                }

                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(conn->client_fd, buf + written, (usize)(n - written));
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
                    written += w;
                }
                if (closed || conn->pending_len > 0) break;
            }
        }

        if (!closed && libssh2_channel_eof(conn->channel)) closed = true;
        if (closed) close_conn_slot(conn);
    }
}

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
    if (!owner || local_port <= 0 || !remote_host || !remote_host[0] || remote_port <= 0) return -1;

    int slot = alloc_rule_slot();
    if (slot < 0) return -1;

    int fd = create_listener(local_port);
    if (fd < 0) return -1;

    ForwardRuleSlot *r = &g_fwd.rules[slot];
    memset(r, 0, sizeof(*r));
    r->owner = owner;
    r->listen_fd = fd;
    r->info.type = FWD_LOCAL;
    r->info.local_port = local_port;
    snprintf(r->info.remote_host, sizeof(r->info.remote_host), "%s", remote_host);
    r->info.remote_port = remote_port;
    r->info.active = true;
    return forward_count(owner) - 1;
}

i32 forward_add_socks5(LIBSSH2_SESSION *owner, i32 local_port) {
    if (!owner || local_port <= 0) return -1;

    int slot = alloc_rule_slot();
    if (slot < 0) return -1;

    int fd = create_listener(local_port);
    if (fd < 0) return -1;

    ForwardRuleSlot *r = &g_fwd.rules[slot];
    memset(r, 0, sizeof(*r));
    r->owner = owner;
    r->listen_fd = fd;
    r->info.type = FWD_DYNAMIC;
    r->info.local_port = local_port;
    r->info.active = true;
    return forward_count(owner) - 1;
}

void forward_remove(LIBSSH2_SESSION *owner, i32 index) {
    int slot = nth_rule_slot(owner, index);
    if (slot < 0) return;

    if (g_fwd.rules[slot].listen_fd >= 0) close(g_fwd.rules[slot].listen_fd);
    g_fwd.rules[slot].listen_fd = -1;
    g_fwd.rules[slot].info.active = false;

    for (i32 i = 0; i < MAX_FORWARD_CONNS; i++) {
        if (g_fwd.conns[i].active && g_fwd.conns[i].owner == owner &&
            g_fwd.conns[i].rule_slot == slot) {
            close_conn_slot(&g_fwd.conns[i]);
        }
    }
}

void forward_poll(LIBSSH2_SESSION *owner) {
    if (!owner) return;

    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        ForwardRuleSlot *r = &g_fwd.rules[i];
        if (!r->info.active || r->owner != owner || r->listen_fd < 0) continue;
        accept_rule_connection(r);
    }

    poll_connections(owner);
}

i32 forward_count(LIBSSH2_SESSION *owner) {
    i32 count = 0;
    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        if (g_fwd.rules[i].info.active && g_fwd.rules[i].owner == owner) count++;
    }
    return count;
}

bool forward_get(LIBSSH2_SESSION *owner, i32 index, ForwardRule *out) {
    int slot = nth_rule_slot(owner, index);
    if (slot < 0 || !out) return false;
    *out = g_fwd.rules[slot].info;
    return true;
}

void forward_cleanup(LIBSSH2_SESSION *owner) {
    if (!owner) return;

    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        if (g_fwd.rules[i].owner != owner) continue;
        if (g_fwd.rules[i].info.active && g_fwd.rules[i].listen_fd >= 0)
            close(g_fwd.rules[i].listen_fd);
        memset(&g_fwd.rules[i], 0, sizeof(g_fwd.rules[i]));
        g_fwd.rules[i].listen_fd = -1;
    }

    for (i32 i = 0; i < MAX_FORWARD_CONNS; i++) {
        if (g_fwd.conns[i].active && g_fwd.conns[i].owner == owner) {
            close_conn_slot(&g_fwd.conns[i]);
        }
    }
}

void forward_cleanup_all(void) {
    for (i32 i = 0; i < g_fwd.rule_hi; i++) {
        if (g_fwd.rules[i].info.active && g_fwd.rules[i].listen_fd >= 0)
            close(g_fwd.rules[i].listen_fd);
    }
    for (i32 i = 0; i < MAX_FORWARD_CONNS; i++) {
        if (g_fwd.conns[i].active) close_conn_slot(&g_fwd.conns[i]);
    }
    memset(&g_fwd, 0, sizeof(g_fwd));
}
