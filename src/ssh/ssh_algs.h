/*
 * ssh_algs — which algorithms Liu offers an SSH server, and in what order.
 *
 * libssh2's own lists, sent as they are, have two problems:
 *
 *   - the host-key list starts with ECDSA whatever ~/.ssh/known_hosts holds.
 *     A host pinned with an ed25519 key alone (ssh-keyscan -t ed25519, a
 *     provisioning script) that also serves an ECDSA key would be negotiated
 *     on ECDSA, the stored key would not match, and the user would be shown
 *     "Host Key Changed" for a host whose key had not changed. A false alarm
 *     on that dialog teaches the one click the dialog exists to prevent.
 *     OpenSSH orders its HostKeyAlgorithms from known_hosts for the same
 *     reason;
 *   - the lists still end in what OpenSSH stopped offering years ago: SHA-1
 *     key exchanges and `ssh-rsa` signatures (7.0, 8.2, 8.8), CBC ciphers,
 *     MD5 and truncated MACs. Somebody on the path cannot force them — the
 *     lists are bound into the signed exchange hash — but a server that
 *     offers nothing better would get a session on them without a word.
 *
 * The rule: an allow-list per method, newest first, with the legacy names
 * appended only when the user asked (`"ssh_legacy_algorithms": true`, for the
 * switch or the storage box that speaks nothing else). For host keys the
 * types the store already holds for this host come first.
 *
 * AES-GCM is listed before ChaCha20-Poly1305 on purpose: against a server
 * without strict key exchange, the prefix-truncation attack (someone on the
 * path deletes messages at the start of the encrypted channel without the
 * integrity check noticing) works on ChaCha20-Poly1305 and on CBC with EtM,
 * and not on GCM.
 *
 * libssh2 strips a name it does not implement and keeps the strict-KEX marker
 * in front of whatever key-exchange list it is given, so these are plain
 * strings. Pure.
 */
#ifndef SSH_SSH_ALGS_H
#define SSH_SSH_ALGS_H

#include "core/types.h"

#include <stdio.h>
#include <string.h>

#define SSH_ALGS_KEX \
    "curve25519-sha256,curve25519-sha256@libssh.org," \
    "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521," \
    "diffie-hellman-group-exchange-sha256,diffie-hellman-group16-sha512," \
    "diffie-hellman-group18-sha512,diffie-hellman-group14-sha256"
#define SSH_ALGS_KEX_LEGACY \
    "diffie-hellman-group14-sha1,diffie-hellman-group-exchange-sha1,diffie-hellman-group1-sha1"

#define SSH_ALGS_CIPHER \
    "aes256-gcm@openssh.com,aes128-gcm@openssh.com,chacha20-poly1305@openssh.com," \
    "aes256-ctr,aes192-ctr,aes128-ctr"
#define SSH_ALGS_CIPHER_LEGACY "aes256-cbc,aes192-cbc,aes128-cbc,3des-cbc"

#define SSH_ALGS_MAC \
    "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com," \
    "hmac-sha2-256,hmac-sha2-512,hmac-sha1-etm@openssh.com,hmac-sha1"
#define SSH_ALGS_MAC_LEGACY "hmac-sha1-96,hmac-md5,hmac-md5-96,hmac-ripemd160"

/* A stored key's type, as the host-key list spells what proves it. */
typedef enum {
    SSH_HOSTKEY_ED25519 = 0,
    SSH_HOSTKEY_ECDSA_256,
    SSH_HOSTKEY_ECDSA_384,
    SSH_HOSTKEY_ECDSA_521,
    SSH_HOSTKEY_RSA,
    SSH_HOSTKEY_KINDS
} SshHostKeyKind;

static inline const char *ssh_hostkey_kind_algs(SshHostKeyKind kind, bool legacy) {
    switch (kind) {
    case SSH_HOSTKEY_ED25519:   return "ssh-ed25519";
    case SSH_HOSTKEY_ECDSA_256: return "ecdsa-sha2-nistp256";
    case SSH_HOSTKEY_ECDSA_384: return "ecdsa-sha2-nistp384";
    case SSH_HOSTKEY_ECDSA_521: return "ecdsa-sha2-nistp521";
    /* One RSA key, three ways to sign with it; the SHA-1 one is the legacy. */
    case SSH_HOSTKEY_RSA:       return legacy ? "rsa-sha2-512,rsa-sha2-256,ssh-rsa"
                                              : "rsa-sha2-512,rsa-sha2-256";
    default:                    return "";
    }
}

static inline bool ssh_algs_append(char *out, usize cap, const char *names) {
    usize have = strlen(out), add = strlen(names);
    if (!add) return true;
    if (have + (have ? 1u : 0u) + add + 1u > cap) return false;
    if (have) out[have++] = ',';
    memcpy(out + have, names, add + 1);
    return true;
}

/* A plain method's list. `out` is left empty when it does not fit. */
static inline bool ssh_algs_list(const char *modern, const char *legacy, bool allow_legacy,
                                 char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    bool ok = ssh_algs_append(out, cap, modern);
    if (ok && allow_legacy) ok = ssh_algs_append(out, cap, legacy);
    if (!ok) out[0] = '\0';
    return ok;
}

/* The host-key list: `stored[kind]` says the store holds a key of that type
 * for this host; those go first, in the fixed order of the enum, and every
 * other type follows in the same order. */
static inline bool ssh_algs_hostkey_list(const bool stored[SSH_HOSTKEY_KINDS], bool allow_legacy,
                                         char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    bool ok = true;
    for (i32 pass = 0; ok && pass < 2; pass++)
        for (i32 k = 0; ok && k < SSH_HOSTKEY_KINDS; k++) {
            bool have = stored && stored[k];
            if (have == (pass == 0))
                ok = ssh_algs_append(out, cap, ssh_hostkey_kind_algs((SshHostKeyKind)k, allow_legacy));
        }
    if (!ok) out[0] = '\0';
    return ok;
}

#endif /* SSH_SSH_ALGS_H */
