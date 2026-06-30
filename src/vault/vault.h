/*
 * Liu - vault (SQLite storage + Smart Vault encrypted secrets)
 *
 * Two separate concerns live here:
 *
 *   1. Metadata store (hosts, groups, snippets, public keys, history,
 *      favorites). Plaintext SQLite, always-on.
 *
 *   2. Smart Vault (passwords, passphrases, env vars, private keys).
 *      AES-256-GCM per-row with an unlock-gated Data-Encryption-Key.
 *      Deliberately leaks non-secret metadata (kind, label, host_id,
 *      timestamps) so the vault can be browsed while locked.
 *
 * Crypto primitives live in vault/crypto.h; everything that talks to
 * vault_secrets must go through the `vault_secret_*` helpers so AAD
 * binding (id || kind || host_id || updated_at) is consistently applied.
 */
#ifndef VAULT_H
#define VAULT_H

#include "core/types.h"

typedef struct Vault Vault;

/* Protocol and auth enums (matches ssh_session.h values) */
typedef enum {
    PROTO_LOCAL  = 0,
    PROTO_SSH    = 1,
    PROTO_TELNET = 2,
    PROTO_MOSH   = 3,
    PROTO_SERIAL = 4,
} VaultProtocol;

typedef enum {
    VAUTH_PASSWORD  = 0,
    VAUTH_PUBLICKEY = 1,
    VAUTH_AGENT     = 2,
    VAUTH_GSSAPI    = 3,
} VaultAuthMethod;

/* Convert enum ↔ string for DB storage */
const char    *vault_protocol_str(VaultProtocol p);
VaultProtocol  vault_protocol_from_str(const char *s);
const char    *vault_auth_str(VaultAuthMethod a);
VaultAuthMethod vault_auth_from_str(const char *s);

/* Host record */
typedef struct {
    char            id[64];
    char            label[128];
    char            hostname[256];
    i32             port;
    VaultProtocol   protocol;
    char            username[128];
    VaultAuthMethod auth_method;
    char            key_id[64];
    char            group_id[64];
    char            tags[256];
    char            color[8];
    i64             last_connected;
    i64             created_at;
    i64             updated_at;
    /* Smart Vault secret references. Empty string = not set. When set, the
     * SSH session layer reveals the secret just-in-time at auth and zeros
     * the plaintext buffer after libssh2 returns. */
    char            password_secret_id[48];
    char            passphrase_secret_id[48];
    char            private_key_secret_id[48];
} VaultHost;

/* Group record */
typedef struct {
    char  id[64];
    char  name[128];
    char  parent_id[64];
    char  color[8];
} VaultGroup;

/* Snippet record */
typedef struct {
    char  id[64];
    char  label[128];
    char  command[4096];
    char  description[256];
    char  tags[256];
} VaultSnippet;

/* SSH key record */
typedef struct {
    char  id[64];
    char  label[128];
    char  algorithm[32];
    char  public_key[2048];
    char  fingerprint[128];
    i64   created_at;
} VaultKey;

/* =========================================================================
 * Vault lifecycle
 * ========================================================================= */

Vault *vault_open(const char *db_path);
void   vault_close(Vault *v);

/* =========================================================================
 * Connection History
 * ========================================================================= */

#define VAULT_HISTORY_MAX 50
#define VAULT_COMMAND_HISTORY_RETENTION_DAYS 30

typedef struct {
    char hostname[256];
    i32  port;
    char username[128];
    char auth_method_str[32];
    f64  timestamp;        /* Unix timestamp of connection */
    bool succeeded;        /* did the connection succeed? */
} VaultHistoryEntry;

bool vault_add_history(Vault *v, const char *hostname, i32 port,
                       const char *username, const char *auth_method, bool succeeded);
bool vault_update_history_result(Vault *v, const char *hostname, i32 port,
                                 const char *username, bool succeeded);
i32  vault_get_history(Vault *v, VaultHistoryEntry *entries, i32 max_entries);
bool vault_clear_history(Vault *v);

/* =========================================================================
 * Command History
 * ========================================================================= */

typedef struct {
    char command[4096];
    f64  timestamp;
} VaultCommandHistoryEntry;

bool vault_add_command_history(Vault *v, const char *command);
i32  vault_get_recent_command_history(Vault *v,
                                      VaultCommandHistoryEntry *entries,
                                      i32 max_entries);

/* =========================================================================
 * Favorites
 * ========================================================================= */

bool vault_set_favorite(Vault *v, const char *hostname, i32 port,
                        const char *username, bool is_favorite);
bool vault_is_favorite(Vault *v, const char *hostname, i32 port,
                       const char *username);
i32  vault_get_favorites(Vault *v, VaultHost *hosts, i32 max);

/* =========================================================================
 * Host CRUD
 * ========================================================================= */

bool vault_host_insert(Vault *v, const VaultHost *host);
bool vault_host_update(Vault *v, const VaultHost *host);
bool vault_host_delete(Vault *v, const char *id);
i32  vault_host_list(Vault *v, VaultHost *out, i32 max);
bool vault_host_get(Vault *v, const char *id, VaultHost *out);
/* Resolve the host at ordinal `index` in vault_host_list() order without
 * materializing the whole list. */
bool vault_host_get_at(Vault *v, i32 index, VaultHost *out);

/* =========================================================================
 * Group CRUD
 * ========================================================================= */

bool vault_group_insert(Vault *v, const VaultGroup *group);
bool vault_group_delete(Vault *v, const char *id);
i32  vault_group_list(Vault *v, VaultGroup *out, i32 max);

/* =========================================================================
 * Snippet CRUD
 * ========================================================================= */

/* Per-snippet callback. Return false to stop iteration early. The pointer
 * is borrowed for the call duration only — copy what you need. */
typedef bool (*vault_snippet_cb)(const VaultSnippet *snippet, void *user);

bool vault_snippet_insert(Vault *v, const VaultSnippet *snippet);
bool vault_snippet_update(Vault *v, const VaultSnippet *snippet);
bool vault_snippet_delete(Vault *v, const char *id);
i32  vault_snippet_list(Vault *v, VaultSnippet *out, i32 max);
/* Streaming variant — emits each snippet to `cb` without materializing an
 * intermediate array. Saves the ~4.7 KB per snippet record on the caller
 * side; useful when the caller only needs label/short description and
 * doesn't keep the full record. Returns the count of rows emitted. */
i32  vault_snippet_for_each(Vault *v, vault_snippet_cb cb, void *user);

/* =========================================================================
 * Key CRUD
 * ========================================================================= */

bool vault_key_insert(Vault *v, const VaultKey *key);
bool vault_key_delete(Vault *v, const char *id);
i32  vault_key_list(Vault *v, VaultKey *out, i32 max);

/* =========================================================================
 * Smart Vault — encrypted secrets
 *
 * The vault has three states: FRESH (no master password set), LOCKED
 * (master set but DEK not in RAM), UNLOCKED (DEK in RAM). A FRESH vault
 * is produced by `vault_init_master` exactly once; subsequent opens go
 * LOCKED → UNLOCKED via `vault_unlock`.
 *
 * Secret plaintexts never touch disk: `vault_secret_reveal` returns a
 * freshly allocated buffer that the caller MUST release with
 * `vault_secret_release` (which also crypto_secure_zero's it).
 * ========================================================================= */

typedef enum {
    VAULT_SECRET_PASSWORD    = 0,
    VAULT_SECRET_PASSPHRASE  = 1,
    VAULT_SECRET_PRIVATE_KEY = 2,
    VAULT_SECRET_ENV_VAR     = 3,
    VAULT_SECRET_NOTE        = 4,
} VaultSecretKind;

/* Metadata for a secret. The encrypted value is NOT included here —
 * reveal it separately with `vault_secret_reveal`. The non-encrypted
 * columns (kind, label, host_id, scope, env_name, key_algo,
 * key_fingerprint, timestamps) are queryable while the vault is
 * LOCKED — this is the intentional metadata leak (same trade-off as
 * 1Password/Bitwarden) that lets users browse entries without typing
 * their master password every time. */
typedef struct {
    char            id[48];
    VaultSecretKind kind;
    char            label[128];
    char            host_id[64];           /* empty = not host-scoped */
    char            scope[16];             /* "global" | "host" | "group" */
    char            env_name[64];          /* for kind=ENV_VAR */
    char            key_algo[32];          /* for kind=PRIVATE_KEY */
    char            key_fingerprint[128];  /* for kind=PRIVATE_KEY */
    i64             created_at;
    i64             updated_at;
    i64             last_accessed;
    i64             access_count;
} VaultSecret;

typedef struct {
    char name[64];
    char value[1024];
    char scope[16];
} VaultEnvVar;

/* ---- Lifecycle ------------------------------------------------------- */

/* True iff a master password has ever been set (i.e. the wrapped-DEK row
 * exists). A fresh DB returns false. */
bool vault_is_initialized(Vault *v);

/* True iff the DEK is currently in RAM. */
bool vault_is_unlocked(const Vault *v);

/* Initialise a FRESH vault: generate DEK + salt, wrap DEK with a KEK
 * derived from `master_password`, store wrapped_dek + kdf params in
 * `vault_meta`. Does NOT leave the vault unlocked — the caller still has
 * to call `vault_unlock` if they want to start adding secrets. Returns
 * false if the vault is already initialised or on I/O error. */
bool vault_init_master(Vault *v, const char *master_password);

/* Unlock the vault: derive KEK from `master_password` + stored salt +
 * iterations, unwrap the DEK, pin it in an mlock'd page. Returns false
 * on wrong password / tampered meta rows. Idempotent: already-unlocked
 * returns false (caller should `vault_lock` first if re-unlocking). */
bool vault_unlock(Vault *v, const char *master_password);

/* Zero the DEK and drop the mlock'd page. Safe to call when already
 * locked — no-op in that case. */
void vault_lock(Vault *v);

/* Change the master password. The vault MUST be unlocked first; the
 * `old_password` is verified via a second unwrap (so a screen-lock
 * bypass cannot silently rotate the password). Writes a new salt and
 * wrapped_dek atomically. */
bool vault_change_master(Vault *v,
                         const char *old_password,
                         const char *new_password);

/* Auto-lock activity tracking. `vault_touch_activity` bumps the
 * last-activity timestamp (should be called ONLY from vault-originated
 * UI events — palette commands, overlay interaction, reveal calls —
 * NOT from terminal I/O, or auto-lock never fires). */
void vault_touch_activity(Vault *v);
f64  vault_get_last_activity(const Vault *v);

/* ---- Secret CRUD ----------------------------------------------------- */

/* Sentinel accepted by vault_secret_list for "no kind filter". */
#define VAULT_SECRET_KIND_ANY (-1)

/* Create a new secret. Encrypts `plaintext` with the in-RAM DEK and
 * stores metadata + ciphertext in one SQLite transaction. A random
 * 16-byte ID is generated and written (hex-encoded + NUL) to `id_out`,
 * which must point at least 33 bytes of space.
 *
 * Optional per-kind metadata columns — pass NULL/"" when not applicable:
 *   env_name         — for kind=VAULT_SECRET_ENV_VAR
 *   scope            — "global"|"host"|"group" (for env vars)
 *   key_algo         — for kind=VAULT_SECRET_PRIVATE_KEY (e.g. "ed25519")
 *   key_fingerprint  — for kind=VAULT_SECRET_PRIVATE_KEY
 *
 * Requires the vault to be unlocked. */
bool vault_secret_create(Vault *v,
                         VaultSecretKind kind,
                         const char *label,
                         const char *host_id,
                         const u8 *plaintext, usize pt_len,
                         const char *env_name,
                         const char *scope,
                         const char *key_algo,
                         const char *key_fingerprint,
                         char *id_out);

/* Re-encrypt an existing secret with a new plaintext. The id and kind
 * do not change, so the AAD remains valid. */
bool vault_secret_update(Vault *v,
                         const char *id,
                         const u8 *new_plaintext, usize pt_len);

/* Delete a secret. Returns true iff a row was actually removed. */
bool vault_secret_delete(Vault *v, const char *id);

/* Decrypt a secret. Returns a freshly allocated buffer with a trailing
 * NUL (not counted in *out_len). Caller MUST release it via
 * `vault_secret_release`. Returns NULL on wrong-AAD / tampered
 * ciphertext / missing row / locked vault. Bumps last_accessed and
 * access_count on success. */
u8  *vault_secret_reveal(Vault *v, const char *id, usize *out_len);

/* Release a buffer returned by vault_secret_reveal — zeroes then frees. */
void vault_secret_release(u8 *plaintext, usize len);

/* List secrets by filter. `kind_filter` may be a specific VaultSecretKind
 * or VAULT_SECRET_KIND_ANY. `host_id_or_null` filters by host (NULL =
 * all). Does NOT decrypt — metadata only. Safe to call while locked. */
i32  vault_secret_list(Vault *v,
                       i32 kind_filter,
                       const char *host_id_or_null,
                       VaultSecret *out, i32 max);

/* Tally secrets per kind via a GROUP BY aggregate: fills counts[0..ncounts)
 * and returns the total. Avoids listing/decrypting every secret just to count. */
i32  vault_secret_counts(Vault *v, i32 *counts, i32 ncounts);

/* Fetch the env-var list for a host plus any scope='global' vars into
 * plaintext `out` slots. Requires the vault to be unlocked. The
 * returned `out` values contain plaintext — callers must zero them
 * with `crypto_secure_zero` once the env has been injected. */
i32  vault_env_list(Vault *v,
                    const char *host_id_or_null,
                    VaultEnvVar *out, i32 max);

#endif /* VAULT_H */
