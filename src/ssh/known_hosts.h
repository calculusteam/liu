#ifndef SSH_KNOWN_HOSTS_H
#define SSH_KNOWN_HOSTS_H

#include "core/types.h"
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
bool known_hosts_add(LIBSSH2_SESSION *session, const char *hostname, i32 port);
bool known_hosts_remove(LIBSSH2_SESSION *session, const char *hostname, i32 port);
bool known_hosts_fingerprint_sha256(LIBSSH2_SESSION *session, char *out, usize out_size);

/* Known hosts management */
i32  known_hosts_list(KnownHostEntry *entries, i32 max_entries);
bool known_hosts_remove_entry(const char *hostname, i32 port);
bool known_hosts_remove_all(void);

/* Host key revocation check (checks ~/.ssh/revoked_keys) */
bool known_hosts_is_revoked(LIBSSH2_SESSION *session);

#endif
