/*
 * Liu - Telnet protocol implementation
 */
#include "ssh/telnet.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "core/net.h"
#include "core/checked.h"

#define TELNET_WRITE_LIMIT ((usize)(8u * 1024u * 1024u))

/* Parser state */
enum {
    TS_DATA = 0,
    TS_IAC,
    TS_WILL,
    TS_WONT,
    TS_DO,
    TS_DONT,
    TS_SB,
    TS_SB_DATA,
    TS_SB_IAC,
};

struct TelnetSession {
    int   sock;
    NetConnect *connecting;   /* the connect, until it lands (sock is -1 meanwhile) */
    i32   cols, rows;
    u8    state;
    bool  alive;

    /* Option state */
    bool  local_echo;
    bool  remote_echo;
    bool  sga;

    /* Subnegotiation buffer */
    u8    sb_buf[256];
    i32   sb_len;
    u8    sb_option;

    /* Wire-format output queue. Telnet doubles every literal IAC byte, so a
     * socket partial write cannot be reported directly as a count of caller
     * bytes. Queue the escaped form and consume caller input atomically. */
    u8   *write_buf;
    usize write_off;
    usize write_len;
    usize write_cap;
};

/* tcp_connect is in core/net.c */

static ssize_t telnet_send_socket(int fd, const void *data, usize len) {
#ifdef MSG_NOSIGNAL
    return send(fd, data, len, MSG_NOSIGNAL);
#else
    return send(fd, data, len, 0);
#endif
}

static bool telnet_flush(TelnetSession *ts) {
    if (!ts || ts->sock < 0) return false;
    while (ts->write_off < ts->write_len) {
        ssize_t n = telnet_send_socket(ts->sock,
                                       ts->write_buf + ts->write_off,
                                       ts->write_len - ts->write_off);
        if (n > 0) ts->write_off += (usize)n;
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        else { ts->alive = false; return false; }
    }
    ts->write_off = ts->write_len = 0;
    /* A paste can grow the escaped queue to megabytes behind a slow peer.
     * Once every byte is sent, release burst capacity just as Session's
     * outer write queue does. Keep small buffers for interactive typing. */
    if (ts->write_cap > KB(64)) {
        free(ts->write_buf);
        ts->write_buf = NULL;
        ts->write_cap = 0;
    }
    return true;
}

static bool telnet_reserve_output(TelnetSession *ts, usize append) {
    if (!ts || append > TELNET_WRITE_LIMIT) return false;
    if (ts->write_off > 0) {
        usize remain = ts->write_len - ts->write_off;
        if (remain) memmove(ts->write_buf, ts->write_buf + ts->write_off, remain);
        ts->write_off = 0;
        ts->write_len = remain;
    }
    usize need;
    if (!checked_add_usize(ts->write_len, append, &need) ||
        need > TELNET_WRITE_LIMIT) return false;
    if (need <= ts->write_cap) return true;
    usize cap = ts->write_cap ? ts->write_cap : 1024;
    while (cap < need) {
        if (cap > TELNET_WRITE_LIMIT / 2u) { cap = TELNET_WRITE_LIMIT; break; }
        cap *= 2u;
    }
    u8 *grown = realloc(ts->write_buf, cap);
    if (!grown) return false;
    ts->write_buf = grown;
    ts->write_cap = cap;
    return true;
}

static void send_bytes(TelnetSession *ts, const u8 *data, i32 len) {
    if (!ts || ts->sock < 0 || !data || len <= 0 ||
        !telnet_reserve_output(ts, (usize)len)) return;
    memcpy(ts->write_buf + ts->write_len, data, (usize)len);
    ts->write_len += (usize)len;
    (void)telnet_flush(ts);
}

static void send_cmd(TelnetSession *ts, u8 cmd, u8 option) {
    u8 buf[3] = { TEL_IAC, cmd, option };
    send_bytes(ts, buf, 3);
}

static void send_naws(TelnetSession *ts) {
    u8 buf[9] = {
        TEL_IAC, TEL_SB, TELOPT_NAWS,
        (u8)(ts->cols >> 8), (u8)(ts->cols & 0xFF),
        (u8)(ts->rows >> 8), (u8)(ts->rows & 0xFF),
        TEL_IAC, TEL_SE
    };
    send_bytes(ts, buf, 9);
}

static void send_ttype(TelnetSession *ts) {
    const char *ttype = "xterm-256color";
    i32 tlen = (i32)strlen(ttype);
    u8 buf[64];
    i32 i = 0;
    buf[i++] = TEL_IAC;
    buf[i++] = TEL_SB;
    buf[i++] = TELOPT_TTYPE;
    buf[i++] = 0; /* IS */
    memcpy(buf + i, ttype, (size_t)tlen);
    i += tlen;
    buf[i++] = TEL_IAC;
    buf[i++] = TEL_SE;
    send_bytes(ts, buf, i);
}

static void handle_will(TelnetSession *ts, u8 option) {
    switch (option) {
    case TELOPT_ECHO:
        ts->remote_echo = true;
        send_cmd(ts, TEL_DO, option);
        break;
    case TELOPT_SGA:
        ts->sga = true;
        send_cmd(ts, TEL_DO, option);
        break;
    default:
        send_cmd(ts, TEL_DONT, option);
        break;
    }
}

static void handle_wont(TelnetSession *ts, u8 option) {
    switch (option) {
    case TELOPT_ECHO:
        ts->remote_echo = false;
        break;
    case TELOPT_SGA:
        ts->sga = false;
        break;
    }
}

static void handle_do(TelnetSession *ts, u8 option) {
    switch (option) {
    case TELOPT_TTYPE:
        send_cmd(ts, TEL_WILL, option);
        break;
    case TELOPT_NAWS:
        send_cmd(ts, TEL_WILL, option);
        send_naws(ts);
        break;
    case TELOPT_TSPEED:
        send_cmd(ts, TEL_WILL, option);
        break;
    case TELOPT_NEW_ENVIRON:
        /* WONT: Liu sends no environment variable to a telnet server, by name
         * or otherwise, so it does not say it will. */
        send_cmd(ts, TEL_WONT, option);
        break;
    default:
        send_cmd(ts, TEL_WONT, option);
        break;
    }
}

static void handle_dont(TelnetSession *ts, u8 option) {
    (void)ts; (void)option;
    /* Acknowledge by not sending the option */
}

static void handle_subneg(TelnetSession *ts) {
    if (ts->sb_len < 1) return;
    u8 option = ts->sb_option;

    switch (option) {
    case TELOPT_TTYPE:
        if (ts->sb_len >= 1 && ts->sb_buf[0] == 1) { /* SEND */
            send_ttype(ts);
        }
        break;
    case TELOPT_TSPEED: {
        /* Send terminal speed */
        u8 buf[] = { TEL_IAC, TEL_SB, TELOPT_TSPEED, 0,
                     '3','8','4','0','0',',','3','8','4','0','0',
                     TEL_IAC, TEL_SE };
        send_bytes(ts, buf, sizeof(buf));
        break;
    }
    }
}

TelnetSession *telnet_create(const char *host, i32 port, i32 cols, i32 rows,
                             void (*wake)(void)) {
    if (!host || !host[0] || strnlen(host, 256) >= 256 ||
        port < 0 || port > 65535 || cols <= 0 || rows <= 0) return NULL;
    TelnetSession *ts = calloc(1, sizeof(TelnetSession));
    if (!ts) return NULL;

    ts->cols = cols > 65535 ? 65535 : cols;
    ts->rows = rows > 65535 ? 65535 : rows;
    ts->sock = -1;
    ts->local_echo = true;
    ts->connecting = net_connect_begin(host, port > 0 ? port : 23,
                                       NET_CONNECT_TIMEOUT_MS, wake);
    if (!ts->connecting) {
        free(ts);
        return NULL;
    }
    return ts;
}

i32 telnet_connect_poll(TelnetSession *ts) {
    if (!ts) return -1;
    if (!ts->connecting) return ts->sock >= 0 ? 1 : -1;
    int fd = net_connect_poll(ts->connecting);
    if (fd == NET_CONNECT_PENDING) return 0;
    ts->connecting = NULL;                 /* consumed either way */
    if (fd >= 0 && !net_set_nonblocking(fd)) { close(fd); fd = -1; }
    if (fd < 0) return -1;
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    ts->sock = fd;
    ts->alive = true;
    return 1;
}

void telnet_destroy(TelnetSession *ts) {
    if (!ts) return;
    if (ts->connecting) net_connect_abandon(ts->connecting);
    if (ts->sock >= 0) close(ts->sock);
    free(ts->write_buf);
    free(ts);
}

i32 telnet_read(TelnetSession *ts, u8 *buf, i32 buf_size) {
    if (!ts || ts->sock < 0 || !buf || buf_size <= 0) return -1;
    if (!telnet_flush(ts)) return -1;
    u8 raw[4096];
    usize recv_cap = (usize)buf_size < sizeof(raw) ? (usize)buf_size : sizeof(raw);
    ssize_t n = recv(ts->sock, raw, recv_cap, 0);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        ts->alive = false;
        return -1;
    }
    if (n == 0) { ts->alive = false; return -1; }

    /* Parse telnet protocol, extract user data */
    i32 out_len = 0;

    for (i32 i = 0; i < (i32)n; i++) {
        u8 b = raw[i];

        switch (ts->state) {
        case TS_DATA:
            if (b == TEL_IAC) {
                ts->state = TS_IAC;
            } else {
                if (out_len < buf_size) buf[out_len++] = b;
            }
            break;

        case TS_IAC:
            switch (b) {
            case TEL_IAC:  /* Escaped 0xFF */
                if (out_len < buf_size) buf[out_len++] = 0xFF;
                ts->state = TS_DATA;
                break;
            case TEL_WILL: ts->state = TS_WILL; break;
            case TEL_WONT: ts->state = TS_WONT; break;
            case TEL_DO:   ts->state = TS_DO; break;
            case TEL_DONT: ts->state = TS_DONT; break;
            case TEL_SB:   ts->state = TS_SB; ts->sb_len = 0; break;
            case TEL_NOP:
            case TEL_GA:
                ts->state = TS_DATA;
                break;
            default:
                ts->state = TS_DATA;
                break;
            }
            break;

        case TS_WILL:
            handle_will(ts, b);
            ts->state = TS_DATA;
            break;
        case TS_WONT:
            handle_wont(ts, b);
            ts->state = TS_DATA;
            break;
        case TS_DO:
            handle_do(ts, b);
            ts->state = TS_DATA;
            break;
        case TS_DONT:
            handle_dont(ts, b);
            ts->state = TS_DATA;
            break;

        case TS_SB:
            ts->sb_option = b;
            ts->state = TS_SB_DATA;
            break;

        case TS_SB_DATA:
            if (b == TEL_IAC) {
                ts->state = TS_SB_IAC;
            } else {
                if (ts->sb_len < (i32)sizeof(ts->sb_buf))
                    ts->sb_buf[ts->sb_len++] = b;
            }
            break;

        case TS_SB_IAC:
            if (b == TEL_SE) {
                handle_subneg(ts);
                ts->state = TS_DATA;
            } else if (b == TEL_IAC) {
                if (ts->sb_len < (i32)sizeof(ts->sb_buf))
                    ts->sb_buf[ts->sb_len++] = 0xFF;
                ts->state = TS_SB_DATA;
            } else {
                ts->state = TS_DATA;
            }
            break;
        }
    }
    return out_len;
}

usize telnet_pending_bytes(const TelnetSession *ts) {
    return ts && ts->write_len >= ts->write_off ? ts->write_len - ts->write_off : 0;
}

i32 telnet_write(TelnetSession *ts, const u8 *data, i32 len) {
    if (!ts || ts->sock < 0 || !data || len <= 0) return len == 0 ? 0 : -1;
    usize escaped = (usize)len;
    for (i32 i = 0; i < len; i++) {
        if (data[i] == TEL_IAC && !checked_add_usize(escaped, 1u, &escaped))
            return -1;
    }
    if (!telnet_reserve_output(ts, escaped)) return 0;
    /* Escape every literal 0xFF into IAC IAC. */
    usize j = ts->write_len;
    for (i32 i = 0; i < len; i++) {
        if (data[i] == 0xFF) {
            ts->write_buf[j++] = TEL_IAC;
            ts->write_buf[j++] = TEL_IAC;
        } else {
            ts->write_buf[j++] = data[i];
        }
    }
    ts->write_len = j;
    return telnet_flush(ts) ? len : -1;
}

void telnet_resize(TelnetSession *ts, i32 cols, i32 rows) {
    if (!ts || cols <= 0 || rows <= 0) return;
    if (cols > 65535) cols = 65535;
    if (rows > 65535) rows = 65535;
    ts->cols = cols;
    ts->rows = rows;
    send_naws(ts);
}

bool telnet_is_alive(TelnetSession *ts) {
    return ts && ts->alive;
}

i32 telnet_fd(TelnetSession *ts) {
    return ts ? ts->sock : -1;
}
