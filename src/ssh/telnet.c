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
};

/* tcp_connect is in core/net.c */

static void send_bytes(TelnetSession *ts, const u8 *data, i32 len) {
    if (ts->sock < 0) return;
    send(ts->sock, data, (size_t)len, 0);
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
        send_cmd(ts, TEL_WILL, option);
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

TelnetSession *telnet_create(const char *host, i32 port, i32 cols, i32 rows) {
    TelnetSession *ts = calloc(1, sizeof(TelnetSession));
    if (!ts) return NULL;

    ts->cols = cols;
    ts->rows = rows;
    ts->sock = net_tcp_connect(host, port > 0 ? port : 23);
    if (ts->sock >= 0) net_set_nonblocking(ts->sock);
    if (ts->sock < 0) {
        free(ts);
        return NULL;
    }
    ts->alive = true;
    ts->local_echo = true;
    return ts;
}

void telnet_destroy(TelnetSession *ts) {
    if (!ts) return;
    if (ts->sock >= 0) close(ts->sock);
    free(ts);
}

i32 telnet_read(TelnetSession *ts, u8 *buf, i32 buf_size) {
    u8 raw[4096];
    ssize_t n = recv(ts->sock, raw, sizeof(raw), 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
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

i32 telnet_write(TelnetSession *ts, const u8 *data, i32 len) {
    /* Escape any 0xFF bytes in user data */
    u8 buf[8192];
    i32 j = 0;
    for (i32 i = 0; i < len && j < (i32)sizeof(buf) - 1; i++) {
        if (data[i] == 0xFF) {
            buf[j++] = TEL_IAC;
            buf[j++] = TEL_IAC;
        } else {
            buf[j++] = data[i];
        }
    }
    ssize_t sent = send(ts->sock, buf, (size_t)j, 0);
    return sent > 0 ? (i32)sent : -1;
}

void telnet_resize(TelnetSession *ts, i32 cols, i32 rows) {
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
