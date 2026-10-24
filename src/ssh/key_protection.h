/*
 * Liu — is this private key passphrase-protected? Read off the key itself.
 *
 * Neither of the indirect signals answers it reliably:
 *
 *   - libssh2's ERROR TEXT for an encrypted key with no passphrase is
 *     "Callback returned error", which names no passphrase, so matching the
 *     text for one never brings up the prompt;
 *   - `ssh-keygen -y -P ""` and its exit status cost a process per question
 *     and answer "encrypted" for any key the tool cannot read for any other
 *     reason.
 *
 * A private key says so in its first bytes:
 *
 *   OpenSSH   -----BEGIN OPENSSH PRIVATE KEY-----, then base64 of
 *             "openssh-key-v1\0" · string ciphername · … ; "none" = not
 *             protected, anything else (aes256-ctr, …) = protected
 *   PEM       "Proc-Type: 4,ENCRYPTED" / "DEK-Info:" headers
 *   PKCS#8    -----BEGIN ENCRYPTED PRIVATE KEY-----
 *
 * Pure. UNKNOWN is a real answer — text that is none of the above — and a
 * caller that gets it keeps whatever fallback it had.
 */
#ifndef SSH_KEY_PROTECTION_H
#define SSH_KEY_PROTECTION_H

#include "core/types.h"

#include <string.h>

typedef enum {
    SSH_KEY_UNKNOWN,     /* not a private key this recognises */
    SSH_KEY_PLAIN,       /* no passphrase */
    SSH_KEY_ENCRYPTED,   /* a passphrase is needed to use it */
} SshKeyProtection;

/* Bounded substring search: the text need not be NUL-terminated. */
static inline const char *ssh_keyp_find(const char *hay, usize hay_len, const char *needle) {
    usize n = strlen(needle);
    if (n == 0 || hay_len < n) return NULL;
    for (usize i = 0; i + n <= hay_len; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, n) == 0) return hay + i;
    return NULL;
}

static inline int ssh_keyp_b64(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode up to `cap` bytes of base64 from `s` (whitespace skipped, stops at
 * the first byte that is not base64). Returns the number of bytes decoded. */
static inline usize ssh_keyp_b64_decode(const char *s, usize len, u8 *out, usize cap) {
    u32 acc = 0;
    int bits = 0;
    usize n = 0;
    for (usize i = 0; i < len && n < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = ssh_keyp_b64(c);
        if (v < 0) break;
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (u8)((acc >> bits) & 0xffu);
        }
    }
    return n;
}

static inline SshKeyProtection ssh_key_protection(const char *text, usize len) {
    if (!text || len == 0) return SSH_KEY_UNKNOWN;

    static const char openssh[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
    const char *b = ssh_keyp_find(text, len, openssh);
    if (b) {
        const char *body = b + sizeof openssh - 1;
        usize body_len = len - (usize)(body - text);
        /* magic (15) + u32 length (4) + a cipher name: 64 bytes hold any. */
        u8 head[64];
        usize n = ssh_keyp_b64_decode(body, body_len, head, sizeof head);
        static const char magic[] = "openssh-key-v1";      /* 14 chars + its NUL = 15 bytes */
        if (n < 15 + 4 || memcmp(head, magic, 15) != 0) return SSH_KEY_UNKNOWN;
        u32 clen = ((u32)head[15] << 24) | ((u32)head[16] << 16) | ((u32)head[17] << 8) | (u32)head[18];
        if (clen == 0 || clen > 32 || 19 + (usize)clen > n) return SSH_KEY_UNKNOWN;
        return (clen == 4 && memcmp(head + 19, "none", 4) == 0) ? SSH_KEY_PLAIN : SSH_KEY_ENCRYPTED;
    }

    if (ssh_keyp_find(text, len, "-----BEGIN ENCRYPTED PRIVATE KEY-----")) return SSH_KEY_ENCRYPTED;

    static const char *const pem[] = {
        "-----BEGIN RSA PRIVATE KEY-----", "-----BEGIN EC PRIVATE KEY-----",
        "-----BEGIN DSA PRIVATE KEY-----", "-----BEGIN PRIVATE KEY-----",
    };
    for (usize i = 0; i < sizeof pem / sizeof pem[0]; i++) {
        if (!ssh_keyp_find(text, len, pem[i])) continue;
        if (ssh_keyp_find(text, len, "Proc-Type: 4,ENCRYPTED") || ssh_keyp_find(text, len, "DEK-Info:"))
            return SSH_KEY_ENCRYPTED;
        return SSH_KEY_PLAIN;
    }
    return SSH_KEY_UNKNOWN;
}

#endif /* SSH_KEY_PROTECTION_H */
