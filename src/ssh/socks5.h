/*
 * Liu — the SOCKS5 handshake, as a fed parser.
 *
 * The dynamic-forwarding accept path runs from the UI poll loop. Reading the
 * whole greeting + request inline there, with the client socket in BLOCKING
 * mode and only a receive timeout as a bound, would let a client that sends one
 * byte and stops freeze the terminal for that timeout PER READ — and the
 * handshake makes up to six of them — and any process on the machine that can
 * reach the SOCKS port could do it. Several such clients are additive.
 *
 * So the protocol is separated from the socket entirely: this parser is FED
 * whatever a non-blocking read happened to return and reports what to do next.
 * It cannot block because it never reads anything, and the wire format holds
 * under arbitrary fragmentation — one byte at a time, a greeting split across
 * its length prefix, a domain name split in the middle — with no sockets
 * involved.
 *
 * Usage: init, then feed until the status is not SOCKS5_NEED_MORE, acting on
 * each status and feeding the unconsumed remainder back in.
 *
 * Only the no-auth method (0x00) and CONNECT (0x01) are accepted; IPv4, IPv6
 * and domain address forms are all carried through unchanged.
 */
#ifndef SSH_SOCKS5_H
#define SSH_SOCKS5_H

#include "core/types.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    SOCKS5_NEED_MORE = 0,     /* nothing to do; feed more bytes            */
    SOCKS5_SEND_METHOD_OK,    /* send {0x05,0x00}, then keep feeding       */
    SOCKS5_DONE,              /* host/port are ready; open the channel     */
    SOCKS5_ERR_VERSION,       /* not SOCKS5 — close, no reply              */
    SOCKS5_ERR_NO_NOAUTH,     /* send {0x05,0xFF}, close                   */
    SOCKS5_ERR_COMMAND,       /* reply 0x07 (command unsupported), close   */
    SOCKS5_ERR_ATYP,          /* reply 0x08 (address type unsupported)     */
    SOCKS5_ERR_PROTOCOL,      /* malformed — close, no reply               */
} Socks5Status;

typedef enum {
    SOCKS5_ST_GREETING_HEADER = 0,   /* ver, nmethods                      */
    SOCKS5_ST_GREETING_METHODS,      /* nmethods method bytes              */
    SOCKS5_ST_REQUEST_HEADER,        /* ver, cmd, rsv, atyp                */
    SOCKS5_ST_REQUEST_DOMAIN_LEN,    /* one length byte, domain form only  */
    SOCKS5_ST_REQUEST_ADDRESS,       /* 4 / 16 / len bytes                 */
    SOCKS5_ST_REQUEST_PORT,          /* two bytes, big endian              */
    SOCKS5_ST_COMPLETE,
} Socks5State;

typedef struct {
    Socks5State state;
    u8   buf[256];      /* the largest single field: 255 methods, or a
                         * 255-byte domain name */
    i32  need, got;     /* wanted / accumulated for the CURRENT state */
    u8   atyp;
    bool greeted;       /* the method-ok reply has been reported once */

    /* Outputs, valid once SOCKS5_DONE is returned. */
    char host[256];     /* dotted quad, bracketless IPv6 text, or domain */
    i32  port;
} Socks5Parser;

static inline void socks5_expect(Socks5Parser *p, Socks5State next, i32 need) {
    p->state = next;
    p->need  = need;
    p->got   = 0;
}

static inline void socks5_init(Socks5Parser *p) {
    memset(p, 0, sizeof *p);
    socks5_expect(p, SOCKS5_ST_GREETING_HEADER, 2);
}

/* Textual form of an IPv4 address, without pulling in inet_ntop for four
 * bytes already at hand. */
static inline void socks5_ipv4_text(const u8 a[4], char *out, usize cap) {
    snprintf(out, cap, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

/* Consume as much of `data` as the current state can take; *consumed is how
 * many bytes were taken (always <= len, and the caller must feed the rest back
 * in after acting on the returned status). */
static inline Socks5Status socks5_feed(Socks5Parser *p, const u8 *data, usize len,
                                       usize *consumed) {
    usize off = 0;
    for (;;) {
        /* Top up the current field from the caller's bytes. */
        while (p->got < p->need && off < len) {
            i32 want = p->need - p->got;
            usize have = len - off;
            i32 take = (usize)want < have ? want : (i32)have;
            memcpy(p->buf + p->got, data + off, (usize)take);
            p->got += take;
            off    += (usize)take;
        }
        if (p->got < p->need) { if (consumed) *consumed = off; return SOCKS5_NEED_MORE; }

        switch (p->state) {
        case SOCKS5_ST_GREETING_HEADER:
            if (p->buf[0] != 0x05) { if (consumed) *consumed = off; return SOCKS5_ERR_VERSION; }
            if (p->buf[1] == 0)    { if (consumed) *consumed = off; return SOCKS5_ERR_PROTOCOL; }
            socks5_expect(p, SOCKS5_ST_GREETING_METHODS, p->buf[1]);
            break;

        case SOCKS5_ST_GREETING_METHODS: {
            bool noauth = false;
            for (i32 i = 0; i < p->got; i++) if (p->buf[i] == 0x00) { noauth = true; break; }
            if (!noauth) { if (consumed) *consumed = off; return SOCKS5_ERR_NO_NOAUTH; }
            socks5_expect(p, SOCKS5_ST_REQUEST_HEADER, 4);
            if (!p->greeted) {
                /* The caller must put {05,00} on the wire before the client
                 * will send its request, so hand control back exactly once. */
                p->greeted = true;
                if (consumed) *consumed = off;
                return SOCKS5_SEND_METHOD_OK;
            }
            break;
        }

        case SOCKS5_ST_REQUEST_HEADER:
            if (p->buf[0] != 0x05) { if (consumed) *consumed = off; return SOCKS5_ERR_VERSION; }
            if (p->buf[1] != 0x01) { if (consumed) *consumed = off; return SOCKS5_ERR_COMMAND; }
            p->atyp = p->buf[3];
            if      (p->atyp == 0x01) socks5_expect(p, SOCKS5_ST_REQUEST_ADDRESS, 4);
            else if (p->atyp == 0x04) socks5_expect(p, SOCKS5_ST_REQUEST_ADDRESS, 16);
            else if (p->atyp == 0x03) socks5_expect(p, SOCKS5_ST_REQUEST_DOMAIN_LEN, 1);
            else { if (consumed) *consumed = off; return SOCKS5_ERR_ATYP; }
            break;

        case SOCKS5_ST_REQUEST_DOMAIN_LEN:
            if (p->buf[0] == 0) { if (consumed) *consumed = off; return SOCKS5_ERR_PROTOCOL; }
            socks5_expect(p, SOCKS5_ST_REQUEST_ADDRESS, p->buf[0]);
            break;

        case SOCKS5_ST_REQUEST_ADDRESS:
            if (p->atyp == 0x01) {
                socks5_ipv4_text(p->buf, p->host, sizeof p->host);
            } else if (p->atyp == 0x04) {
                /* Written out rather than inet_ntop so the parser stays free of
                 * socket headers and testable anywhere. Canonical RFC 5952 form
                 * is not required — this string is handed to the SSH server as
                 * a host to connect to, and it round-trips either way. */
                char *o = p->host;
                for (i32 i = 0; i < 16; i += 2) {
                    if (i) *o++ = ':';
                    o += snprintf(o, (usize)(p->host + sizeof p->host - o), "%x",
                                  ((u32)p->buf[i] << 8) | p->buf[i + 1]);
                }
                *o = '\0';
            } else {
                i32 n = p->got < (i32)sizeof p->host - 1 ? p->got : (i32)sizeof p->host - 1;
                memcpy(p->host, p->buf, (usize)n);
                p->host[n] = '\0';
            }
            socks5_expect(p, SOCKS5_ST_REQUEST_PORT, 2);
            break;

        case SOCKS5_ST_REQUEST_PORT:
            p->port = ((i32)p->buf[0] << 8) | p->buf[1];
            p->state = SOCKS5_ST_COMPLETE;
            p->need = p->got = 0;
            if (consumed) *consumed = off;
            return SOCKS5_DONE;

        case SOCKS5_ST_COMPLETE:
            if (consumed) *consumed = off;
            return SOCKS5_DONE;
        }
    }
}

#endif /* SSH_SOCKS5_H */
