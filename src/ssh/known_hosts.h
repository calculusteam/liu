#ifndef SSH_KNOWN_HOSTS_H
#define SSH_KNOWN_HOSTS_H

#include "core/types.h"
#include "ssh/ssh_algs.h"
#include <libssh2.h>

typedef enum {
    HOST_KEY_OK,
    HOST_KEY_NEW,
    HOST_KEY_CHANGED,
    HOST_KEY_REVOKED,
    HOST_KEY_ERROR,
} HostKeyStatus;

/* Entry returned by known_hosts_list() */
typedef struct {
    char hostname[256];
    i32  port;
    char key_type[32];     /* e.g., "ssh-ed25519", "ssh-rsa" */
    char fingerprint[128]; /* SHA256 hex fingerprint */
} KnownHostEntry;

HostKeyStatus known_hosts_verify(LIBSSH2_SESSION *session, const char *hostname, i32 port);
/* Which key types the stores (the user's, then the machine's) hold for
 * host:port. Asked before the handshake; see ssh/ssh_algs.h. */
void known_hosts_stored_kinds(LIBSSH2_SESSION *session, const char *hostname, i32 port,
                              bool stored[SSH_HOSTKEY_KINDS]);
/* Classify a knownhost result only after checking the negotiated key's
 * revocation status. Revocation takes precedence over a changed key. */
HostKeyStatus known_hosts_check_status(i32 check, bool revoked);
bool known_hosts_add(LIBSSH2_SESSION *session, const char *hostname, i32 port);
bool known_hosts_remove(LIBSSH2_SESSION *session, const char *hostname, i32 port);
/* Retire every stored key for hostname:port that contradicts the session's
 * key (accepted key change). True when the store is consistent afterwards. */
bool known_hosts_forget(LIBSSH2_SESSION *session, const char *hostname, i32 port);
/* One revoked_keys line vs. a host key: "SHA256:<b64|hex>" or "<type> <b64>".
 * Exposed for tests; known_hosts_is_revoked drives it. */
bool known_hosts_revocation_line_matches(const char *line,
                                         const unsigned char *fp_sha256,
                                         const char *key, usize key_len);
bool known_hosts_fingerprint_sha256(LIBSSH2_SESSION *session, char *out, usize out_size);

/* The store operations with the host key passed in and the file named: verify /
 * add / forget are these with the session's negotiated key and the user's
 * ~/.ssh/known_hosts, and these work without a handshake. `store_check` answers
 * a LIBSSH2_KNOWNHOST_CHECK_* value; `key_typemask` is a
 * LIBSSH2_KNOWNHOST_KEY_* value; `store_revokes` reads `@revoked` markers. */
i32  known_hosts_store_check(LIBSSH2_SESSION *session, const char *path, const char *hostname,
                             i32 port, const char *key, usize key_len);
bool known_hosts_store_add(LIBSSH2_SESSION *session, const char *path, const char *hostname,
                           i32 port, const char *key, usize key_len, int key_typemask);
bool known_hosts_store_forget(LIBSSH2_SESSION *session, const char *path, const char *hostname,
                              i32 port, const char *key, usize key_len);
bool known_hosts_store_revokes(const char *path, const unsigned char *fp_sha256,
                               const char *key, usize key_len);

/* Known hosts management */
i32  known_hosts_list(KnownHostEntry *entries, i32 max_entries);
bool known_hosts_remove_entry(const char *hostname, i32 port);
bool known_hosts_remove_all(void);

/* True when ~/.ssh or its known_hosts is there but refused as the trust store:
 * not the user's, not a plain folder / file, or writable by another user.
 * Says why a verification ended in HOST_KEY_ERROR. */
bool known_hosts_store_unsafe(void);

/* Host key revocation check (checks ~/.ssh/revoked_keys) */
bool known_hosts_is_revoked(LIBSSH2_SESSION *session);

#endif
