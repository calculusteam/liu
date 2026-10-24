/*
 * auth_second_factor — what a server still wants after it took the key.
 *
 * A server that asks for a key AND something else (an `AuthenticationMethods
 * publickey,keyboard-interactive` setting — a key plus a one-time code, the
 * usual two-factor setup) answers the key with a PARTIAL success, which
 * libssh2 reports as a failed key: "Invalid signature for supplied public
 * key, or bad username/public key combination". Ending the session there
 * would make such a host impossible to log into: the keyboard-interactive
 * dialog that exists for those codes would never come up.
 *
 * Asked again after the key, the server lists what it still accepts. A list
 * without "publickey" means the key was taken (a refused key leaves publickey
 * on the list), or that this server takes no keys at all, so a key was never
 * the way in: either way what remains is the server's own prompt
 * (keyboard-interactive) first, a password when that is what it wants and one
 * was given.
 *
 * Pure.
 */
#ifndef SSH_AUTH_SECOND_FACTOR_H
#define SSH_AUTH_SECOND_FACTOR_H

#include "core/types.h"
#include <string.h>

typedef enum {
    SSH_SECOND_FACTOR_NONE = 0,   /* the key itself failed (or nothing is left to try) */
    SSH_SECOND_FACTOR_KBI,        /* the server's prompt: keyboard-interactive */
    SSH_SECOND_FACTOR_PASSWORD,   /* the stored password */
} SshSecondFactor;

/* Is `method` one of the comma-separated names in `list` (whole names only)? */
static inline bool ssh_auth_list_has(const char *list, const char *method) {
    if (!list || !method) return false;
    usize n = strlen(method);
    for (const char *p = list; *p; ) {
        const char *end = strchr(p, ',');
        usize len = end ? (usize)(end - p) : strlen(p);
        if (len == n && strncmp(p, method, n) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

/* `remaining`: libssh2_userauth_list asked AFTER the key attempt (NULL when the
 * server gave none). `have_password`: a password is known for this host. */
static inline SshSecondFactor ssh_auth_second_factor(const char *remaining, bool have_password) {
    if (!remaining || !remaining[0]) return SSH_SECOND_FACTOR_NONE;
    if (ssh_auth_list_has(remaining, "publickey")) return SSH_SECOND_FACTOR_NONE;
    if (ssh_auth_list_has(remaining, "keyboard-interactive")) return SSH_SECOND_FACTOR_KBI;
    if (have_password && ssh_auth_list_has(remaining, "password")) return SSH_SECOND_FACTOR_PASSWORD;
    return SSH_SECOND_FACTOR_NONE;
}

#endif /* SSH_AUTH_SECOND_FACTOR_H */
