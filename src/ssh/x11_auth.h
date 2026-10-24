/*
 * Liu — who may use the forwarded X11 display.
 *
 * X11 forwarding hands the SERVER a display: sshd opens a listener (localhost:10)
 * on the remote machine, and every connection to it becomes an SSH channel that
 * Liu relays to the local X server. Whoever can connect to that listener drives
 * the local screen and keyboard — the remote user's own programs, but also every
 * other user on that machine and anything that user runs. What stands between
 * them and the display is the MIT-MAGIC-COOKIE: the client invents a FAKE cookie
 * for this session, tells sshd (which stores it in the user's ~/.Xauthority), and
 * checks the first packet of every X11 channel for it, closing the channel when
 * it is absent or wrong. A relay that skips the check is a raw pipe, and then the
 * only protection is the local X server's own access control — which is exactly
 * the thing a user switches off to make forwarding work. This is that check.
 *
 * Pure parsing of the X11 connection-setup request (X Window System Protocol,
 * "Connection Setup"): byte 0 is the byte order ('B' or 'l'), then the protocol
 * version, then two 16-bit lengths — the authorization protocol name and its
 * data — each followed by the bytes padded to a multiple of 4. Liu only ever
 * authorizes MIT-MAGIC-COOKIE-1 with a 16-byte cookie, so the whole request is
 * exactly X11_SETUP_LEN bytes; anything else is refused rather than waited for.
 */
#ifndef SSH_X11_AUTH_H
#define SSH_X11_AUTH_H

#include "core/types.h"
#include <stdbool.h>
#include <string.h>

#define X11_COOKIE_LEN      16u
#define X11_AUTH_NAME       "MIT-MAGIC-COOKIE-1"
#define X11_AUTH_NAME_LEN   18u
/* 12-byte header + name padded 18->20 + data 16 (already a multiple of 4). */
#define X11_SETUP_LEN       48u
#define X11_HEADER_LEN      12u

typedef enum {
    X11_AUTH_NEED_MORE = 0,   /* read `*need` more bytes, then ask again */
    X11_AUTH_OK,              /* the packet is complete and carries the right cookie */
    X11_AUTH_BAD              /* wrong shape, wrong name, wrong cookie: close the channel */
} X11AuthResult;

/* Judge what has been read of a connection-setup request. `*need` receives how
 * many more bytes the verdict is waiting for (only when X11_AUTH_NEED_MORE) so the
 * caller reads exactly that many and never consumes past the request. The cookie
 * compare touches every byte whatever the first difference. */
static inline X11AuthResult x11_setup_check(const u8 *buf, usize len,
                                            const u8 cookie[X11_COOKIE_LEN], usize *need) {
    if (need) *need = 0;
    if (len < X11_HEADER_LEN) {
        if (need) *need = X11_HEADER_LEN - len;
        /* A byte-order byte that is neither is already wrong; don't wait for more. */
        if (len >= 1 && buf[0] != 'B' && buf[0] != 'l') return X11_AUTH_BAD;
        return X11_AUTH_NEED_MORE;
    }
    bool big = buf[0] == 'B';
    if (!big && buf[0] != 'l') return X11_AUTH_BAD;
    usize name_len = big ? ((usize)buf[6] << 8) | buf[7] : ((usize)buf[7] << 8) | buf[6];
    usize data_len = big ? ((usize)buf[8] << 8) | buf[9] : ((usize)buf[9] << 8) | buf[8];
    /* Only MIT-MAGIC-COOKIE-1 with a 16-byte cookie is ever authorized. An empty or
     * longer authorization is not a request to wait for more of. */
    if (name_len != X11_AUTH_NAME_LEN || data_len != X11_COOKIE_LEN) return X11_AUTH_BAD;
    if (len < X11_SETUP_LEN) {
        if (need) *need = X11_SETUP_LEN - len;
        return X11_AUTH_NEED_MORE;
    }
    const u8 *name = buf + X11_HEADER_LEN;
    const u8 *data = name + 20;                       /* 18 bytes + 2 padding */
    u8 diff = 0;
    for (usize i = 0; i < X11_AUTH_NAME_LEN; i++) diff |= (u8)(name[i] ^ (u8)X11_AUTH_NAME[i]);
    for (usize i = 0; i < X11_COOKIE_LEN; i++)    diff |= (u8)(data[i] ^ cookie[i]);
    return diff == 0 ? X11_AUTH_OK : X11_AUTH_BAD;
}

/* ---- the REAL cookie, and which display to reach ---------------------------------------
 *
 * The check above only proves the channel knows the FAKE cookie sshd was given. The local X
 * server knows a different one: so that the connection works with its access control ON, the
 * relay substitutes the real cookie into the request it forwards. It is read from the user's
 * Xauthority file: a sequence of entries
 *     family(u16 BE) | address(u16 len + bytes) | number | name | data      (each counted)
 * of which only family Local (256, address = this host's name) or Wild (65535), name
 * MIT-MAGIC-COOKIE-1, data 16 bytes and a matching display number apply. A host-specific match
 * beats a wildcard one. Nothing found = nothing substituted (the relay then works only where
 * the local server admits the connection).
 */
#define X11_FAMILY_LOCAL 256u
#define X11_FAMILY_WILD  65535u

static inline bool x11_xauth_find(const u8 *buf, usize len, const char *host, const char *number,
                                  u8 out[X11_COOKIE_LEN]) {
    bool have_wild = false, have_local = false;
    u8 wild[X11_COOKIE_LEN], local[X11_COOKIE_LEN];
    usize i = 0, hlen = host ? strlen(host) : 0, nlen = number ? strlen(number) : 0;
    while (len - i >= 2) {
        unsigned family = ((unsigned)buf[i] << 8) | buf[i + 1];
        i += 2;
        const u8 *field[4]; usize flen[4];
        bool ok = true;
        for (int f = 0; f < 4 && ok; f++) {
            if (len - i < 2) { ok = false; break; }
            usize n = ((usize)buf[i] << 8) | buf[i + 1];
            i += 2;
            if (len - i < n) { ok = false; break; }
            field[f] = buf + i; flen[f] = n; i += n;
        }
        if (!ok) break;                                       /* a truncated entry ends the file */
        if (flen[2] != X11_AUTH_NAME_LEN || memcmp(field[2], X11_AUTH_NAME, X11_AUTH_NAME_LEN) != 0) continue;
        if (flen[3] != X11_COOKIE_LEN) continue;
        if (flen[1] != 0 && (flen[1] != nlen || memcmp(field[1], number, nlen) != 0)) continue;
        if (family == X11_FAMILY_LOCAL && flen[0] == hlen && hlen > 0 && memcmp(field[0], host, hlen) == 0) {
            if (!have_local) { memcpy(local, field[3], X11_COOKIE_LEN); have_local = true; }
        } else if (family == X11_FAMILY_WILD) {
            if (!have_wild) { memcpy(wild, field[3], X11_COOKIE_LEN); have_wild = true; }
        }
    }
    if (have_local) { memcpy(out, local, X11_COOKIE_LEN); return true; }
    if (have_wild)  { memcpy(out, wild, X11_COOKIE_LEN); return true; }
    return false;
}

/* $DISPLAY forms Liu reaches: ":N[.S]" and "host:N[.S]" (the existing paths), and the path form
 * XQuartz gives GUI apps — "/private/tmp/com.apple.launchd.XXXX/org.xquartz:0" — where everything
 * before the last ':' is the socket to connect to. These two answer the questions the caller needs:
 * which display NUMBER (for the Xauthority lookup), and is it a socket PATH. */
static inline bool x11_display_number(const char *display, char *out, usize cap) {
    if (!display || !out || cap < 2) return false;
    const char *colon = strrchr(display, ':');
    if (!colon || !colon[1]) return false;
    usize n = 0;
    for (const char *p = colon + 1; *p && *p != '.'; p++) {
        if (*p < '0' || *p > '9' || n + 2 > cap) return false;
        out[n++] = *p;
    }
    out[n] = '\0';
    return n > 0 && n <= 5;
}

static inline bool x11_display_socket_path(const char *display, char *out, usize cap) {
    if (!display || display[0] != '/' || !out || cap < 2) return false;
    const char *colon = strrchr(display, ':');
    if (!colon || colon == display) return false;
    usize n = (usize)(colon - display);
    if (n + 1 > cap) return false;
    for (usize i = 0; i < n; i++) if ((unsigned char)display[i] < 0x20 || display[i] == 0x7f) return false;
    memcpy(out, display, n);
    out[n] = '\0';
    return true;
}

#endif /* SSH_X11_AUTH_H */
