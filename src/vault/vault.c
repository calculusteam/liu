/*
 * Vault implementation for SQLite metadata and encrypted secrets.
 *
 * The metadata store holds hosts, groups, snippets, public keys,
 * history, and favorites in plaintext. Smart Vault secrets are
 * encrypted and require an unlocked master password; the DEK is held in
 * an mlock'd page while unlocked. Each decrypt authenticates the row
 * identity as AAD.
 *
 * Schema migrations use PRAGMA user_version. Fresh databases and v1
 * metadata-only databases migrate to the current schema, and CREATE
 * TABLE IF NOT EXISTS keeps schema creation safe to repeat.
 */
#include "vault/vault.h"
#include "vault/crypto.h"
#include "vault/secret_binding.h"
#include "core/checked.h"
#include "core/sqlite_harden.h"
#include "sqlite3.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define VAULT_SCHEMA_VERSION 2

/* Forward declaration — the definition lives in the host section below.
 * Used by both read_host_row and read_secret_row. */
static void copy_text_col(char *dst, usize cap, sqlite3_stmt *stmt, int col);

struct Vault {
    sqlite3 *db;
    /* Smart Vault state. `dek_page` is a single page-sized mmap region
     * that holds the 32-byte DEK at its head (the rest is zeroed slack
     * kept because `mlock` works on page-granularity). `dek_page_cap`
     * is the mmap length. Both are NULL/0 while the vault is locked. */
    u8      *dek_page;
    usize    dek_page_cap;
    bool     unlocked;
    f64      last_activity_ts;
};

/* =========================================================================
 * Enum ↔ string conversion (DB boundary)
 * ========================================================================= */

const char *vault_protocol_str(VaultProtocol p) {
    switch (p) {
        case PROTO_LOCAL:  return "local";
        case PROTO_SSH:    return "ssh";
        case PROTO_TELNET: return "telnet";
        case PROTO_MOSH:   return "mosh";
        case PROTO_SERIAL: return "serial";
    }
    return "ssh";
}

VaultProtocol vault_protocol_from_str(const char *s) {
    if (!s) return PROTO_SSH;
    if (strcmp(s, "local")  == 0) return PROTO_LOCAL;
    if (strcmp(s, "ssh")    == 0) return PROTO_SSH;
    if (strcmp(s, "telnet") == 0) return PROTO_TELNET;
    if (strcmp(s, "mosh")   == 0) return PROTO_MOSH;
    if (strcmp(s, "serial") == 0) return PROTO_SERIAL;
    return PROTO_SSH;
}

const char *vault_auth_str(VaultAuthMethod a) {
    switch (a) {
        case VAUTH_PASSWORD:  return "password";
        case VAUTH_PUBLICKEY: return "key";
        case VAUTH_AGENT:     return "agent";
        case VAUTH_GSSAPI:    return "gssapi";
    }
    return "password";
}

VaultAuthMethod vault_auth_from_str(const char *s) {
    if (!s) return VAUTH_PASSWORD;
    if (strcmp(s, "password") == 0) return VAUTH_PASSWORD;
    if (strcmp(s, "key")      == 0) return VAUTH_PUBLICKEY;
    if (strcmp(s, "agent")    == 0) return VAUTH_AGENT;
    if (strcmp(s, "gssapi")   == 0) return VAUTH_GSSAPI;
    return VAUTH_PASSWORD;
}

static const char *schema_sql =
    "CREATE TABLE IF NOT EXISTS groups ("
    "  id TEXT PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  parent_id TEXT,"
    "  color TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS hosts ("
    "  id TEXT PRIMARY KEY,"
    "  label TEXT NOT NULL,"
    "  hostname TEXT NOT NULL,"
    "  port INTEGER NOT NULL DEFAULT 22,"
    "  protocol TEXT NOT NULL DEFAULT 'ssh',"
    "  username TEXT,"
    "  auth_method TEXT NOT NULL DEFAULT 'password',"
    "  key_id TEXT,"
    "  group_id TEXT,"
    "  tags TEXT,"
    "  color TEXT,"
    "  last_connected INTEGER,"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS snippets ("
    "  id TEXT PRIMARY KEY,"
    "  label TEXT NOT NULL,"
    "  command TEXT NOT NULL,"
    "  description TEXT,"
    "  tags TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS ssh_keys ("
    "  id TEXT PRIMARY KEY,"
    "  label TEXT NOT NULL,"
    "  algorithm TEXT NOT NULL,"
    "  public_key TEXT NOT NULL,"
    "  fingerprint TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_hosts_group ON hosts(group_id);"
    "CREATE INDEX IF NOT EXISTS idx_hosts_last ON hosts(last_connected DESC);"
    "CREATE TABLE IF NOT EXISTS connection_history ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  hostname TEXT NOT NULL,"
    "  port INTEGER DEFAULT 22,"
    "  username TEXT,"
    "  auth_method TEXT,"
    "  timestamp REAL,"
    "  succeeded INTEGER DEFAULT 1"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_history_ts ON connection_history(timestamp DESC);"
    "CREATE TABLE IF NOT EXISTS command_history ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  command TEXT NOT NULL,"
    "  timestamp REAL NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_command_history_ts ON command_history(timestamp DESC);"
    "CREATE TABLE IF NOT EXISTS favorites ("
    "  hostname TEXT NOT NULL,"
    "  port INTEGER DEFAULT 22,"
    "  username TEXT,"
    "  label TEXT,"
    "  UNIQUE(hostname, port, username)"
    ");";

/* =========================================================================
 * Smart Vault schema (v2)
 * ========================================================================= */

static const char *smart_vault_schema_sql =
    "CREATE TABLE IF NOT EXISTS vault_meta ("
    "  key   TEXT PRIMARY KEY,"
    "  value BLOB"
    ");"
    "CREATE TABLE IF NOT EXISTS vault_secrets ("
    "  id              TEXT PRIMARY KEY,"
    "  kind            INTEGER NOT NULL,"
    "  label           TEXT NOT NULL,"
    "  host_id         TEXT,"
    "  scope           TEXT,"
    "  env_name        TEXT,"
    "  key_algo        TEXT,"
    "  key_fingerprint TEXT,"
    "  encrypted_value BLOB NOT NULL,"
    "  created_at      INTEGER NOT NULL,"
    "  updated_at      INTEGER NOT NULL,"
    "  last_accessed   INTEGER,"
    "  access_count    INTEGER DEFAULT 0,"
    "  FOREIGN KEY(host_id) REFERENCES hosts(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_secrets_host  ON vault_secrets(host_id);"
    "CREATE INDEX IF NOT EXISTS idx_secrets_kind  ON vault_secrets(kind);"
    "CREATE INDEX IF NOT EXISTS idx_secrets_scope ON vault_secrets(scope, kind);";

/* PRAGMA user_version drives forward-only schema migrations. Fresh databases
 * and v1 metadata-only databases migrate to v2 by adding host columns and
 * creating the secret tables.
 *
 * ALTER TABLE ADD COLUMN fails if the column already exists, so user_version
 * gates each migration to run once per database. */

static i32 read_user_version(sqlite3 *db) {
    sqlite3_stmt *s = NULL;
    i32 version = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            version = sqlite3_column_int(s, 0);
        }
    }
    sqlite3_finalize(s);
    return version;
}

static bool migrate_to_v2(sqlite3 *db) {
    char *err = NULL;

    /* Pre-existing v1 DBs have a `hosts` table without the secret_id
     * columns. Fresh DBs have just run `schema_sql` so the table
     * exists but without those columns too. Either way, ALTER adds them. */
    const char *alters[] = {
        "ALTER TABLE hosts ADD COLUMN password_secret_id TEXT",
        "ALTER TABLE hosts ADD COLUMN passphrase_secret_id TEXT",
        "ALTER TABLE hosts ADD COLUMN private_key_secret_id TEXT",
        NULL,
    };
    for (i32 i = 0; alters[i]; i++) {
        int rc = sqlite3_exec(db, alters[i], NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            /* "duplicate column" is tolerable if a partial prior migration
             * already landed some columns — continue. Everything else is
             * fatal. SQLite error message contains "duplicate column". */
            if (err && strstr(err, "duplicate column") == NULL) {
                fprintf(stderr, "vault: migrate_to_v2 ALTER failed: %s\n", err);
                sqlite3_free(err);
                return false;
            }
            sqlite3_free(err);
            err = NULL;
        }
    }

    if (sqlite3_exec(db, smart_vault_schema_sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "vault: migrate_to_v2 schema failed: %s\n", err);
        sqlite3_free(err);
        return false;
    }

    return true;
}

static bool apply_migrations(sqlite3 *db) {
    i32 v = read_user_version(db);
    if (v >= VAULT_SCHEMA_VERSION) return true;

    /* Wrap the migration in a single transaction so a crash halfway
     * leaves the DB in the starting version. */
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "vault: begin migration: %s\n", err);
        sqlite3_free(err);
        return false;
    }

    bool ok = true;
    if (v < 2) ok = migrate_to_v2(db);

    if (ok) {
        char buf[64];
        snprintf(buf, sizeof buf, "PRAGMA user_version=%d", VAULT_SCHEMA_VERSION);
        if (sqlite3_exec(db, buf, NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "vault: user_version bump: %s\n", err);
            sqlite3_free(err);
            ok = false;
        }
    }

    sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

/* =========================================================================
 * Process-wide hardening (core dump disabled)
 *
 * Called once at first vault_open. Disabling core dumps for the whole
 * process prevents the DEK (and every other in-memory secret) from
 * leaking into /cores on macOS. `setrlimit(RLIMIT_CORE, 0)` is
 * unprivileged and cannot be undone without raising the hard limit.
 * ========================================================================= */

static void disable_core_dumps_once(void) {
    static bool done = false;
    if (done) return;
    struct rlimit rl = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &rl);
    done = true;
}

/* Narrow `path` to 0600 through a descriptor: O_NOFOLLOW so a symlink planted
 * under the name is not followed, fstat so only a REGULAR file is touched,
 * fchmod so the mode lands on the inode that was opened rather than on
 * whatever the name resolves to a moment later. `flags` is O_RDWR | O_CREAT
 * for the db itself (create it 0600 before SQLite can create it 0644) and
 * O_RDONLY for a sidecar (never create one — SQLite owns their lifetime).
 * Silent on every failure: a missing sidecar is the normal case. */
static void vault_narrow_mode(const char *path, int flags) {
    int fd = open(path, flags | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 077))
        (void)fchmod(fd, 0600);
    close(fd);
}

Vault *vault_open(const char *db_path) {
    if (!db_path || !db_path[0]) return NULL;
    /* SQLITE_OMIT_AUTOINIT is set — one-time init must happen explicitly. */
    static bool sqlite_inited = false;
    if (!sqlite_inited) {
        sqlite3_initialize();
        sqlite_inited = true;
    }

    /* Drop RLIMIT_CORE before any secret-adjacent state is loaded.
     * Cheap, process-wide, and cannot be reversed by an attacker with
     * the same privilege level. */
    disable_core_dumps_once();

    Vault *v = calloc(1, sizeof(Vault));
    if (!v) return NULL;

    /* The database holds plaintext METADATA around the encrypted secrets —
     * hostnames, usernames, labels, the command history — and SQLite creates
     * it with the umask (0644) and gives -wal/-shm the same mode. Create it
     * 0600 first so SQLite inherits that for the sidecars, and narrow an
     * existing file that others can read. Only a regular file is opened. */
    {
        vault_narrow_mode(db_path, O_RDWR | O_CREAT);
        /* The sidecars take the SAME open+fchmod route, never a chmod() by
         * name: chmod follows a symlink, and the ~/.config/Liu 0700 that makes
         * that unexploitable today is enforced somewhere else (ui.c) — a
         * hardening that only holds while another file's rule holds is not
         * one. Existing sidecars only; SQLite creates the rest 0600 from the
         * db's own mode. */
        static const char *const side[] = { "-wal", "-shm", "-journal" };
        for (usize i = 0; i < sizeof side / sizeof side[0]; i++) {
            char sp[PATH_MAX];
            int n = snprintf(sp, sizeof sp, "%s%s", db_path, side[i]);
            if (n > 0 && (usize)n < sizeof sp) vault_narrow_mode(sp, O_RDONLY);
        }
    }

    int rc = sqlite3_open(db_path, &v->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "vault: cannot open db: %s\n", sqlite3_errmsg(v->db));
        /* sqlite3_open allocates a handle even on failure — close it so
         * it does not leak when the Vault struct is freed. */
        sqlite3_close(v->db);
        free(v);
        return NULL;
    }

    /* The header's threat model includes a DB-WRITE adversary (a synced or backed-up
     * ~/.config/Liu is exactly that), and a schema is SQL that runs when the file is
     * used: a TRIGGER on hosts fires on every insert, a VIEW can stand in for a table,
     * and a hostile CHECK/DEFAULT can call functions. The vault uses none of those —
     * only plain tables, indexes and ALTER TABLE ADD COLUMN, checked against the real
     * database — so they are switched off before anything runs (core/sqlite_harden.h).
     * Applied BEFORE the pragmas and schema_sql so even those run under it. */
    liu_sqlite_harden_foreign(v->db);
    sqlite3_exec(v->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(v->db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    sqlite3_exec(v->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    /* What is deleted is overwritten, not left in a free page: a forgotten or
     * cleared Recent row is a host name in plain text, and a removed secret's
     * sealed blob need not outlive its row either. */
    sqlite3_exec(v->db, "PRAGMA secure_delete=ON;", NULL, NULL, NULL);
    /* Sized for a tiny config DB (hosts + snippets + small history tables): a
     * 256 KB page cache and a 512 KB mmap keep SQLite hot without holding
     * megabytes of idle RSS. */
    sqlite3_exec(v->db, "PRAGMA cache_size=-256;", NULL, NULL, NULL);
    sqlite3_exec(v->db, "PRAGMA mmap_size=524288;", NULL, NULL, NULL);

    char *err = NULL;
    rc = sqlite3_exec(v->db, schema_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "vault: schema error: %s\n", err);
        sqlite3_free(err);
    }

    /* Smart Vault migration. Idempotent — user_version gates the work. */
    if (!apply_migrations(v->db)) {
        fprintf(stderr, "vault: smart-vault migration failed — "
                        "secret storage disabled for this session\n");
        /* Keep the metadata vault usable even if migration failed;
         * `vault_is_initialized` will simply return false. */
    }

    {
        sqlite3_stmt *stmt = NULL;
        const char *prune = "DELETE FROM command_history WHERE timestamp < ?";
        if (sqlite3_prepare_v2(v->db, prune, -1, &stmt, NULL) == SQLITE_OK) {
            f64 cutoff = (f64)time(NULL) -
                         (f64)(VAULT_COMMAND_HISTORY_RETENTION_DAYS * 24 * 60 * 60);
            sqlite3_bind_double(stmt, 1, cutoff);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

    return v;
}

void vault_close(Vault *v) {
    if (!v) return;
    vault_lock(v);
    if (v->db) sqlite3_close(v->db);
    free(v);
}

/* =========================================================================
 * Smart Vault — lifecycle
 *
 * State machine (enforced by `unlocked` + presence of `wrapped_dek`):
 *
 *     FRESH  ──vault_init_master──▶  LOCKED
 *     LOCKED ──vault_unlock──────▶  UNLOCKED
 *     UNLOCKED ──vault_lock───────▶  LOCKED
 *
 * `vault_change_master` requires UNLOCKED and re-verifies the old
 * password by re-running the KEK derive + unwrap — a screen-lock
 * bypass therefore cannot silently rotate the password.
 * ========================================================================= */

static f64 now_ts(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

static bool meta_set_blob(sqlite3 *db, const char *key,
                          const u8 *value, usize value_len) {
    sqlite3_stmt *s = NULL;
    const char *sql = "INSERT INTO vault_meta(key,value) VALUES(?,?) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, value, (int)value_len, SQLITE_STATIC);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

/* Reads a BLOB row from vault_meta. Returns true on success and copies
 * up to `cap_out` bytes into `out`, writing the actual length to
 * `*len_out`. Returns false if the row does not exist. */
static bool meta_get_blob(sqlite3 *db, const char *key,
                          u8 *out, usize cap_out, usize *len_out) {
    sqlite3_stmt *s = NULL;
    /* octet_length() reads the stored size, not the value: a value longer
     * than the caller's buffer comes back NULL and is never loaded. */
    const char *sql = "SELECT CASE WHEN octet_length(value) <= ?2 THEN value END"
                      " FROM vault_meta WHERE key=?1";
    if (cap_out > (usize)INT_MAX) cap_out = (usize)INT_MAX;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)cap_out);
    bool ok = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const void *v = sqlite3_column_blob(s, 0);
        int vlen = sqlite3_column_bytes(s, 0);
        if (v && vlen > 0 && (usize)vlen <= cap_out) {
            memcpy(out, v, (usize)vlen);
            *len_out = (usize)vlen;
            ok = true;
        }
    }
    sqlite3_finalize(s);
    return ok;
}

static bool meta_set_u32(sqlite3 *db, const char *key, u32 value) {
    u8 buf[4];
    buf[0] = (u8)(value >> 24);
    buf[1] = (u8)(value >> 16);
    buf[2] = (u8)(value >> 8);
    buf[3] = (u8)(value);
    return meta_set_blob(db, key, buf, 4);
}

static bool meta_get_u32(sqlite3 *db, const char *key, u32 *out) {
    u8 buf[4];
    usize n = 0;
    if (!meta_get_blob(db, key, buf, sizeof buf, &n) || n != 4) return false;
    *out = ((u32)buf[0] << 24) | ((u32)buf[1] << 16) |
           ((u32)buf[2] << 8)  |  (u32)buf[3];
    return true;
}

bool vault_is_initialized(Vault *v) {
    if (!v || !v->db) return false;
    u8 buf[CRYPTO_WRAPPED_DEK_LEN];
    usize n = 0;
    return meta_get_blob(v->db, "wrapped_dek", buf, sizeof buf, &n)
        && n == CRYPTO_WRAPPED_DEK_LEN;
}

bool vault_is_unlocked(const Vault *v) {
    return v && v->unlocked && v->dek_page != NULL;
}

/* Allocate a page-aligned, mlock'd buffer to hold the DEK. Best-effort
 * mlock (unprivileged callers on macOS get a small ulimit, but even if
 * that fails, the memory is still in a dedicated region away from
 * general heap — the failure is logged and the allocation stands). */
static u8 *alloc_dek_page(usize *cap_out) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    u8 *p = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (mlock(p, (size_t)page) != 0) {
        /* Silent on release builds — noisy only in debug. */
#ifdef DEBUG
        fprintf(stderr, "vault: mlock of DEK page failed (ulimit?)\n");
#endif
    }
    *cap_out = (usize)page;
    return p;
}

static void free_dek_page(u8 *p, usize cap) {
    if (!p) return;
    crypto_secure_zero(p, cap);
    (void)munlock(p, cap);
    munmap(p, cap);
}

bool vault_init_master(Vault *v, const char *master_password) {
    if (!v || !v->db || !master_password || !*master_password) return false;
    if (vault_is_initialized(v)) return false;

    u8 dek[CRYPTO_KEY_LEN];
    u8 salt[CRYPTO_KDF_SALT_LEN];
    u8 kek[CRYPTO_KEY_LEN];
    u8 wrapped[CRYPTO_WRAPPED_DEK_LEN];

    if (!crypto_random(dek, sizeof dek))  return false;
    if (!crypto_random(salt, sizeof salt)) {
        crypto_secure_zero(dek, sizeof dek);
        return false;
    }

    const u32 iters = CRYPTO_DEFAULT_KDF_ITERATIONS;
    if (!crypto_kdf_pbkdf2(master_password,
                           salt, sizeof salt,
                           iters,
                           kek, sizeof kek)) {
        crypto_secure_zero(dek, sizeof dek);
        crypto_secure_zero(salt, sizeof salt);
        return false;
    }

    bool wrap_ok = crypto_wrap_dek(kek, dek, iters,
                                   salt, sizeof salt,
                                   wrapped, sizeof wrapped);
    crypto_secure_zero(kek, sizeof kek);

    if (!wrap_ok) {
        crypto_secure_zero(dek, sizeof dek);
        crypto_secure_zero(salt, sizeof salt);
        return false;
    }

    char *err = NULL;
    bool ok = true;
    if (sqlite3_exec(v->db, "BEGIN", NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err);
        ok = false;
    }
    if (ok) ok = meta_set_blob(v->db, "kdf_salt", salt, sizeof salt);
    if (ok) ok = meta_set_u32(v->db, "kdf_iterations", iters);
    if (ok) ok = meta_set_blob(v->db, "wrapped_dek", wrapped, sizeof wrapped);
    if (ok) ok = meta_set_u32(v->db, "schema_version",
                              (u32)VAULT_SCHEMA_VERSION);
    {
        u64 created = (u64)time(NULL);
        u8 buf[8];
        for (int i = 0; i < 8; i++) buf[i] = (u8)(created >> (56 - 8*i));
        if (ok) ok = meta_set_blob(v->db, "created_at", buf, 8);
    }
    sqlite3_exec(v->db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);

    crypto_secure_zero(dek, sizeof dek);
    crypto_secure_zero(salt, sizeof salt);
    crypto_secure_zero(wrapped, sizeof wrapped);
    return ok;
}

bool vault_unlock(Vault *v, const char *master_password) {
    if (!v || !v->db || !master_password) return false;
    if (v->unlocked) return false;
    if (!vault_is_initialized(v)) return false;

    u8 salt[CRYPTO_KDF_SALT_LEN];
    u8 wrapped[CRYPTO_WRAPPED_DEK_LEN];
    usize salt_len = 0, wrapped_len = 0;
    u32 iters = 0;

    if (!meta_get_blob(v->db, "kdf_salt", salt, sizeof salt, &salt_len) ||
        salt_len != sizeof salt) return false;
    if (!meta_get_blob(v->db, "wrapped_dek", wrapped, sizeof wrapped, &wrapped_len) ||
        wrapped_len != sizeof wrapped) return false;
    if (!meta_get_u32(v->db, "kdf_iterations", &iters) || iters == 0) return false;

    u8 kek[CRYPTO_KEY_LEN];
    if (!crypto_kdf_pbkdf2(master_password,
                           salt, salt_len,
                           iters,
                           kek, sizeof kek)) return false;

    if (!v->dek_page) {
        v->dek_page = alloc_dek_page(&v->dek_page_cap);
        if (!v->dek_page) { crypto_secure_zero(kek, sizeof kek); return false; }
    }

    bool ok = crypto_unwrap_dek(kek, wrapped, wrapped_len,
                                iters, salt, salt_len,
                                v->dek_page);
    crypto_secure_zero(kek, sizeof kek);
    if (!ok) {
        /* Scrub any partial plaintext from the locked page and release it,
         * so the next attempt starts clean. */
        crypto_secure_zero(v->dek_page, v->dek_page_cap);
        return false;
    }

    v->unlocked = true;
    v->last_activity_ts = now_ts();
    return true;
}

void vault_lock(Vault *v) {
    if (!v) return;
    if (v->dek_page) {
        free_dek_page(v->dek_page, v->dek_page_cap);
        v->dek_page = NULL;
        v->dek_page_cap = 0;
    }
    v->unlocked = false;
    v->last_activity_ts = 0.0;
}

bool vault_change_master(Vault *v,
                         const char *old_password,
                         const char *new_password) {
    if (!v || !v->db || !old_password || !new_password || !*new_password) {
        return false;
    }
    if (!v->unlocked) return false;

    /* Re-verify the old password by unwrapping the stored DEK and
     * comparing to the in-RAM one. A wrong old password must be refused
     * — otherwise someone with access to an unlocked process could
     * silently rotate credentials. */
    u8 salt[CRYPTO_KDF_SALT_LEN];
    u8 wrapped[CRYPTO_WRAPPED_DEK_LEN];
    usize salt_len = 0, wrapped_len = 0;
    u32 iters = 0;
    if (!meta_get_blob(v->db, "kdf_salt", salt, sizeof salt, &salt_len) ||
        salt_len != sizeof salt) return false;
    if (!meta_get_blob(v->db, "wrapped_dek", wrapped, sizeof wrapped, &wrapped_len) ||
        wrapped_len != sizeof wrapped) return false;
    if (!meta_get_u32(v->db, "kdf_iterations", &iters) || iters == 0) return false;

    u8 kek_old[CRYPTO_KEY_LEN];
    if (!crypto_kdf_pbkdf2(old_password, salt, salt_len,
                           iters, kek_old, sizeof kek_old)) return false;

    u8 dek_verify[CRYPTO_KEY_LEN];
    bool old_ok = crypto_unwrap_dek(kek_old, wrapped, wrapped_len,
                                    iters, salt, salt_len, dek_verify);
    crypto_secure_zero(kek_old, sizeof kek_old);
    if (!old_ok) {
        crypto_secure_zero(dek_verify, sizeof dek_verify);
        return false;
    }
    /* `dek_verify` should equal the in-RAM DEK — sanity-check, then
     * re-wrap the in-RAM copy (not the verify copy, to tolerate a
     * future API where verify might produce a different buffer). */
    if (memcmp(dek_verify, v->dek_page, CRYPTO_KEY_LEN) != 0) {
        crypto_secure_zero(dek_verify, sizeof dek_verify);
        return false;
    }
    crypto_secure_zero(dek_verify, sizeof dek_verify);

    /* New KEK gets a fresh salt — cheap, eliminates KDF reuse across
     * password rotations. */
    u8 salt_new[CRYPTO_KDF_SALT_LEN];
    if (!crypto_random(salt_new, sizeof salt_new)) return false;

    u32 iters_new = CRYPTO_DEFAULT_KDF_ITERATIONS; /* allow iter upgrade on rotate */
    u8 kek_new[CRYPTO_KEY_LEN];
    if (!crypto_kdf_pbkdf2(new_password, salt_new, sizeof salt_new,
                           iters_new, kek_new, sizeof kek_new)) {
        crypto_secure_zero(salt_new, sizeof salt_new);
        return false;
    }

    u8 wrapped_new[CRYPTO_WRAPPED_DEK_LEN];
    bool wrap_ok = crypto_wrap_dek(kek_new, v->dek_page,
                                   iters_new, salt_new, sizeof salt_new,
                                   wrapped_new, sizeof wrapped_new);
    crypto_secure_zero(kek_new, sizeof kek_new);
    if (!wrap_ok) {
        crypto_secure_zero(salt_new, sizeof salt_new);
        crypto_secure_zero(wrapped_new, sizeof wrapped_new);
        return false;
    }

    char *err = NULL;
    bool ok = true;
    if (sqlite3_exec(v->db, "BEGIN", NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err); ok = false;
    }
    if (ok) ok = meta_set_blob(v->db, "kdf_salt", salt_new, sizeof salt_new);
    if (ok) ok = meta_set_u32(v->db, "kdf_iterations", iters_new);
    if (ok) ok = meta_set_blob(v->db, "wrapped_dek", wrapped_new, sizeof wrapped_new);
    sqlite3_exec(v->db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);

    crypto_secure_zero(salt_new, sizeof salt_new);
    crypto_secure_zero(wrapped_new, sizeof wrapped_new);
    return ok;
}

void vault_touch_activity(Vault *v) {
    if (!v) return;
    v->last_activity_ts = now_ts();
}

f64 vault_get_last_activity(const Vault *v) {
    return v ? v->last_activity_ts : 0.0;
}

/* Version 2 envelopes authenticate an immutable routing snapshot as AAD.
 * Plaintext database columns remain browsable but do not authorize automatic
 * export. Nonce|tag|ciphertext envelopes remain manually readable and require
 * explicit review and save before they can authorize an automatic connection.
 */

#define AAD_PREFIX     "liu-v1-sec:"
#define AAD_PREFIX_LEN 11u
#define AAD_MAX_LEN    (AAD_PREFIX_LEN + 32 + 1 + 1) /* prefix+id+'|'+kind */

/* Do not truncate or reinterpret SQLite authorization fields. */
static bool secret_text_col(sqlite3_stmt *s, int col, char *out, usize cap) {
    int type = sqlite3_column_type(s, col);
    if (type == SQLITE_NULL) { out[0] = 0; return true; }
    if (type != SQLITE_TEXT) return false;
    int n = sqlite3_column_bytes(s, col);
    const void *p = sqlite3_column_text(s, col);
    if (n < 0 || (usize)n >= cap || !p || memchr(p, 0, (usize)n)) return false;
    memcpy(out, p, (usize)n); out[n] = 0;
    return true;
}

static bool binding_load_host(Vault *v, SecretBinding *b) {
    if (!b->host_id[0]) return true;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(v->db,
        "SELECT hostname,username,port,protocol FROM hosts WHERE id=?",
        -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, b->host_id, -1, SQLITE_STATIC);
    char protocol[16];
    bool ok = sqlite3_step(s) == SQLITE_ROW &&
        secret_text_col(s, 0, b->hostname, sizeof b->hostname) &&
        secret_text_col(s, 1, b->username, sizeof b->username) &&
        sqlite3_column_type(s, 2) == SQLITE_INTEGER &&
        secret_text_col(s, 3, protocol, sizeof protocol);
    if (ok) {
        i64 port = sqlite3_column_int64(s, 2);
        ok = port >= 0 && port <= 65535;
        if (ok) {
            static const char *const names[] = {"local", "ssh", "telnet", "mosh", "serial"};
            ok = false;
            for (i32 i = 0; i <= PROTO_SERIAL; i++) if (!strcmp(protocol, names[i])) {
                b->protocol = i; b->port = (i32)port;
                if (i != PROTO_LOCAL && !b->port) b->port = 22;
                ok = true; break;
            }
        }
    }
    sqlite3_finalize(s);
    return ok;
}

static bool binding_make(Vault *v, SecretBinding *b, const char *id,
                         VaultSecretKind kind, const char *host_id,
                         const char *scope, const char *env_name,
                         const VaultHost *recipient) {
    memset(b, 0, sizeof *b); b->kind = kind;
    if (kind < VAULT_SECRET_PASSWORD || kind > VAULT_SECRET_NOTE ||
        !binding_text(b->id, sizeof b->id, id) || strlen(b->id) != 32 ||
        !binding_text(b->host_id, sizeof b->host_id, host_id) ||
        !binding_text(b->scope, sizeof b->scope, scope) ||
        !binding_text(b->env_name, sizeof b->env_name, env_name)) return false;
    if (kind == VAULT_SECRET_ENV_VAR &&
        ((!strcmp(b->scope, "global") && b->host_id[0]) ||
         (!strcmp(b->scope, "host") && !b->host_id[0]))) return false;
    if (!b->host_id[0]) return recipient == NULL;
    if (!recipient) return binding_load_host(v, b); /* trusted API compatibility */
    char reviewed_id[64];
    if (!binding_text(reviewed_id, sizeof reviewed_id, recipient->id) ||
        strcmp(reviewed_id, b->host_id) ||
        !binding_text(b->hostname, sizeof b->hostname, recipient->hostname) ||
        !binding_text(b->username, sizeof b->username, recipient->username) ||
        recipient->port < 0 || recipient->port > 65535 ||
        (i32)recipient->protocol < PROTO_LOCAL || recipient->protocol > PROTO_SERIAL)
        return false;
    b->port = recipient->port; b->protocol = (i32)recipient->protocol;
    if (b->protocol != PROTO_LOCAL && !b->port) b->port = 22;
    return true;
}

bool vault_recipient_get(Vault *v, const char *id, VaultHost *out) {
    SecretBinding b = {0};
    if (!v || !id || !*id || !out ||
        !binding_text(b.host_id, sizeof b.host_id, id) || !binding_load_host(v, &b)) return false;
    memset(out, 0, sizeof *out);
    memcpy(out->id, b.host_id, sizeof out->id);
    memcpy(out->hostname, b.hostname, sizeof out->hostname);
    memcpy(out->username, b.username, sizeof out->username);
    out->port = b.port; out->protocol = (VaultProtocol)b.protocol;
    return true;
}

static bool binding_current_row(Vault *v, const SecretBinding *b) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(v->db,
        "SELECT kind,host_id,scope,env_name FROM vault_secrets WHERE id=?",
        -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, b->id, -1, SQLITE_STATIC);
    char host_id[64], scope[16], env_name[64];
    bool ok = sqlite3_step(s) == SQLITE_ROW &&
        sqlite3_column_type(s, 0) == SQLITE_INTEGER &&
        sqlite3_column_int64(s, 0) == b->kind &&
        secret_text_col(s, 1, host_id, sizeof host_id) &&
        secret_text_col(s, 2, scope, sizeof scope) &&
        secret_text_col(s, 3, env_name, sizeof env_name) &&
        !strcmp(host_id, b->host_id) && !strcmp(scope, b->scope) &&
        !strcmp(env_name, b->env_name);
    sqlite3_finalize(s);
    return ok;
}

static void hex_encode(const u8 *in, usize n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (usize i = 0; i < n; i++) {
        out[i*2]     = h[(in[i] >> 4) & 0xF];
        out[i*2 + 1] = h[ in[i]       & 0xF];
    }
    out[n*2] = '\0';
}

/* Generate a random 32-hex-char ID (16 bytes of entropy). */
static bool gen_secret_id(char *id_out /* >= 33 */) {
    u8 raw[16];
    if (!crypto_random(raw, sizeof raw)) return false;
    hex_encode(raw, sizeof raw, id_out);
    return true;
}

static usize build_secret_aad(u8 *out, usize cap,
                              const char *id, VaultSecretKind kind) {
    if (!out || !id) return 0;
    if (kind < VAULT_SECRET_PASSWORD || kind > VAULT_SECRET_NOTE) return 0;
    usize id_len = strlen(id);
    if (id_len != 32) return 0;
    usize need = AAD_PREFIX_LEN + id_len + 1 + 1;
    if (cap < need) return 0;
    memcpy(out, AAD_PREFIX, AAD_PREFIX_LEN);
    memcpy(out + AAD_PREFIX_LEN, id, id_len);
    out[AAD_PREFIX_LEN + id_len] = '|';
    out[AAD_PREFIX_LEN + id_len + 1] = (u8)kind;
    return need;
}

usize vault_secret_max_len(VaultSecretKind kind) {
    switch (kind) {
        case VAULT_SECRET_PASSWORD:
        case VAULT_SECRET_PASSPHRASE:  return (usize)1  * 1024;
        case VAULT_SECRET_PRIVATE_KEY: return (usize)64 * 1024;
        case VAULT_SECRET_ENV_VAR:     return (usize)128 * 1024;
        case VAULT_SECRET_NOTE:        return (usize)1  * 1024 * 1024;
    }
    /* An unrecognised kind — a row from a future schema, say — gets the
     * smallest cap rather than the largest. */
    return (usize)1 * 1024;
}

/* Encrypt plaintext with the current DEK + per-row AAD. Returns a
 * freshly allocated blob of [nonce|tag|ct] bytes. */
static u8 *encrypt_secret_value(const Vault *v,
                                const SecretBinding *binding,
                                const u8 *pt, usize pt_len,
                                usize *blob_len_out) {
    if (!v || !v->unlocked || !v->dek_page) return NULL;
    if (!binding || !pt || !blob_len_out) return NULL;
    *blob_len_out = 0;

    /* Refuse before allocating. */
    if (pt_len > vault_secret_max_len(binding->kind)) return NULL;

    u8 aad[SECRET_BINDING_CAP];
    usize aad_len = binding_encode(binding, aad, sizeof aad);
    if (!aad_len) return NULL;

    const usize header = SECRET_ENVELOPE_HEADER + aad_len +
                         CRYPTO_AEAD_NONCE_LEN + CRYPTO_AEAD_TAG_LEN;
    usize blob_len;
    if (!checked_add_usize(header, pt_len, &blob_len) || blob_len > INT_MAX)
        return NULL;
    u8 *blob = malloc(blob_len);
    if (!blob) return NULL;

    memcpy(blob, SECRET_ENVELOPE_MAGIC, 8);
    blob[8] = (u8)(aad_len >> 8); blob[9] = (u8)aad_len;
    memcpy(blob + SECRET_ENVELOPE_HEADER, aad, aad_len);
    u8 *nonce = blob + SECRET_ENVELOPE_HEADER + aad_len;
    u8 *tag   = nonce + CRYPTO_AEAD_NONCE_LEN;
    u8 *ct    = tag + CRYPTO_AEAD_TAG_LEN;

    if (!crypto_random(nonce, CRYPTO_AEAD_NONCE_LEN)) {
        free(blob);
        return NULL;
    }

    bool ok = crypto_aead_encrypt(v->dek_page, nonce,
                                  aad, aad_len,
                                  pt, pt_len,
                                  ct, tag);
    crypto_secure_zero(aad, aad_len);

    if (!ok) { free(blob); return NULL; }
    *blob_len_out = blob_len;
    return blob;
}

static u8 *decrypt_secret_value(const Vault *v,
                                const char *id, VaultSecretKind kind,
                                const u8 *blob, usize blob_len,
                                usize *pt_len_out, SecretBinding *binding_out) {
    if (!v || !v->unlocked || !v->dek_page) return NULL;
    if (!id || !blob || !pt_len_out) return NULL;
    *pt_len_out = 0;
    usize header = CRYPTO_AEAD_NONCE_LEN + CRYPTO_AEAD_TAG_LEN;
    if (blob_len < header) return NULL;

    u8 legacy_aad[AAD_MAX_LEN];
    const u8 *aad = legacy_aad;
    usize aad_len;
    SecretBinding binding;
    bool v2 = blob_len >= 8 && !memcmp(blob, SECRET_ENVELOPE_MAGIC, 8);
    if (binding_out) memset(binding_out, 0, sizeof *binding_out);
    if (v2) {
        if (blob_len < SECRET_ENVELOPE_HEADER) return NULL;
        aad_len = ((usize)blob[8] << 8) | blob[9];
        if (!aad_len || aad_len > SECRET_BINDING_CAP ||
            blob_len < SECRET_ENVELOPE_HEADER + aad_len + header) return NULL;
        aad = blob + SECRET_ENVELOPE_HEADER;
        if (!binding_decode(aad, aad_len, &binding) || strcmp(binding.id, id) ||
            binding.kind != kind) return NULL;
        header += SECRET_ENVELOPE_HEADER + aad_len;
    } else {
        /* Manual recovery only. Never infer trusted routing from mutable
         * database columns. */
        aad_len = build_secret_aad(legacy_aad, sizeof legacy_aad, id, kind);
        if (!aad_len || binding_out) return NULL;
    }

    usize ct_len = blob_len - header;
    /* Refuse a secret row larger than its kind's maximum before allocating
     * memory or decrypting it. */
    if (ct_len > vault_secret_max_len(kind)) return NULL;
    const u8 *ct = blob + header;
    const u8 *tag = ct - CRYPTO_AEAD_TAG_LEN;
    const u8 *nonce = tag - CRYPTO_AEAD_NONCE_LEN;

    /* Extra byte for NUL terminator. */
    usize alloc_len;
    if (!checked_add_usize(ct_len, 1, &alloc_len)) {
        return NULL;
    }
    u8 *pt = malloc(alloc_len);
    if (!pt) return NULL;

    bool ok = crypto_aead_decrypt(v->dek_page, nonce,
                                  aad, aad_len,
                                  ct, ct_len, tag,
                                  pt);
    if (!ok) { crypto_secure_zero(pt, ct_len + 1); free(pt); return NULL; }
    if (binding_out) *binding_out = binding;
    pt[ct_len] = '\0';
    *pt_len_out = ct_len;
    return pt;
}

/* ---------- Create ---------- */

static bool secret_create(Vault *v,
                         VaultSecretKind kind,
                         const char *label,
                         const char *host_id,
                         const u8 *plaintext, usize pt_len,
                         const char *env_name,
                         const char *scope,
                         const char *key_algo,
                         const char *key_fingerprint,
                         char *id_out, const VaultHost *recipient) {
    if (!v || !v->unlocked) return false;
    if (!label || !plaintext || !id_out) return false;

    char id[33];
    if (!gen_secret_id(id)) return false;
    SecretBinding binding;
    if (!binding_make(v, &binding, id, kind, host_id, scope, env_name, recipient)) return false;

    usize blob_len = 0;
    u8 *blob = encrypt_secret_value(v, &binding, plaintext, pt_len, &blob_len);
    if (!blob) return false;

    const char *sql =
        "INSERT INTO vault_secrets "
        "(id,kind,label,host_id,scope,env_name,key_algo,key_fingerprint,"
        " encrypted_value,created_at,updated_at,last_accessed,access_count) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,NULL,0)";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(v->db, sql, -1, &s, NULL) != SQLITE_OK) {
        crypto_secure_zero(blob, blob_len); free(blob);
        return false;
    }
    i64 now = (i64)time(NULL);
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (s, 2, (int)kind);
    sqlite3_bind_text(s, 3, label, -1, SQLITE_STATIC);
    if (host_id && *host_id) sqlite3_bind_text(s, 4, host_id, -1, SQLITE_STATIC);
    else                     sqlite3_bind_null(s, 4);
    if (scope && *scope)     sqlite3_bind_text(s, 5, scope, -1, SQLITE_STATIC);
    else                     sqlite3_bind_null(s, 5);
    if (env_name && *env_name) sqlite3_bind_text(s, 6, env_name, -1, SQLITE_STATIC);
    else                       sqlite3_bind_null(s, 6);
    if (key_algo && *key_algo) sqlite3_bind_text(s, 7, key_algo, -1, SQLITE_STATIC);
    else                       sqlite3_bind_null(s, 7);
    if (key_fingerprint && *key_fingerprint)
        sqlite3_bind_text(s, 8, key_fingerprint, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 8);
    sqlite3_bind_blob(s, 9, blob, (int)blob_len, SQLITE_STATIC);
    sqlite3_bind_int64(s, 10, now);
    sqlite3_bind_int64(s, 11, now);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    crypto_secure_zero(blob, blob_len);
    free(blob);

    if (ok) {
        memcpy(id_out, id, 33);
        vault_touch_activity(v);
    }
    return ok;
}

bool vault_secret_create(Vault *v, VaultSecretKind kind, const char *label,
                         const char *host_id, const u8 *plaintext, usize pt_len,
                         const char *env_name, const char *scope, const char *key_algo,
                         const char *key_fingerprint, char *id_out) {
    return secret_create(v, kind, label, host_id, plaintext, pt_len, env_name,
                         scope, key_algo, key_fingerprint, id_out, NULL);
}

bool vault_secret_create_reviewed(Vault *v, VaultSecretKind kind, const char *label,
                         const char *host_id, const u8 *plaintext, usize pt_len,
                         const char *env_name, const char *scope, const VaultHost *recipient,
                         char *id_out) {
    if (host_id && *host_id && !recipient) return false;
    return secret_create(v, kind, label, host_id, plaintext, pt_len, env_name,
                         scope, NULL, NULL, id_out, recipient);
}

/* ---------- Update ---------- */

static bool secret_update_bound(Vault *v, const SecretBinding *binding,
                                 const u8 *new_plaintext, usize pt_len) {
    const char *id = binding->id;
    if (!v || !v->unlocked || !id || !new_plaintext) return false;
    sqlite3_stmt *s = NULL;

    usize blob_len = 0;
    u8 *blob = encrypt_secret_value(v, binding, new_plaintext, pt_len, &blob_len);
    if (!blob) return false;

    if (sqlite3_prepare_v2(v->db,
        "UPDATE vault_secrets SET encrypted_value=?,updated_at=?,host_id=?,scope=?,env_name=? WHERE id=? AND kind=?",
        -1, &s, NULL) != SQLITE_OK) {
        crypto_secure_zero(blob, blob_len); free(blob);
        return false;
    }
    sqlite3_bind_blob(s, 1, blob, (int)blob_len, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, (i64)time(NULL));
    if (binding->host_id[0]) sqlite3_bind_text(s, 3, binding->host_id, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 3);
    if (binding->scope[0]) sqlite3_bind_text(s, 4, binding->scope, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 4);
    if (binding->env_name[0]) sqlite3_bind_text(s, 5, binding->env_name, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 5);
    sqlite3_bind_text(s, 6, id, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 7, (int)binding->kind);
    bool ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(v->db) > 0;
    sqlite3_finalize(s);
    crypto_secure_zero(blob, blob_len);
    free(blob);
    if (ok) vault_touch_activity(v);
    return ok;
}

/* ---------- Delete ---------- */

bool vault_secret_delete(Vault *v, const char *id) {
    if (!v || !v->db || !id) return false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(v->db,
        "DELETE FROM vault_secrets WHERE id=?", -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(v->db) > 0;
    sqlite3_finalize(s);
    if (ok) vault_touch_activity(v);
    return ok;
}

/* ---------- Reveal / release ---------- */

/* The longest envelope any kind's secret can make: the v2 header with the
 * largest binding, nonce and tag, around the largest plaintext. A stored
 * value longer than this is no secret Liu wrote. */
static sqlite3_int64 secret_envelope_max(void) {
    usize most = 0;
    for (i32 k = VAULT_SECRET_PASSWORD; k <= VAULT_SECRET_NOTE; k++) {
        usize n = vault_secret_max_len((VaultSecretKind)k);
        if (n > most) most = n;
    }
    return (sqlite3_int64)(SECRET_ENVELOPE_HEADER + SECRET_BINDING_CAP +
                           CRYPTO_AEAD_NONCE_LEN + CRYPTO_AEAD_TAG_LEN + most);
}

static u8 *secret_reveal(Vault *v, const char *id, usize *out_len,
                         SecretBinding *binding) {
    if (!v || !v->unlocked || !id || !out_len) return NULL;

    sqlite3_stmt *s = NULL;
    /* The value is read only when octet_length() — the stored size, read
     * without the bytes — fits an envelope; a larger one comes back NULL
     * without ever being loaded. The kind's own maximum is then applied by
     * decrypt_secret_value before anything is allocated. */
    if (sqlite3_prepare_v2(v->db,
        "SELECT kind, CASE WHEN octet_length(encrypted_value) <= ?2"
        " THEN encrypted_value END FROM vault_secrets WHERE id=?1",
        -1, &s, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, secret_envelope_max());

    u8 *out = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        if (sqlite3_column_type(s, 0) != SQLITE_INTEGER) {
            sqlite3_finalize(s); return NULL;
        }
        i64 raw_kind = sqlite3_column_int64(s, 0);
        if (raw_kind < VAULT_SECRET_PASSWORD || raw_kind > VAULT_SECRET_NOTE) {
            sqlite3_finalize(s); return NULL;
        }
        VaultSecretKind kind = (VaultSecretKind)raw_kind;
        const void *blob = sqlite3_column_blob(s, 1);
        int blob_len = sqlite3_column_bytes(s, 1);
        if (blob && blob_len > 0) {
            usize pt_len = 0;
            out = decrypt_secret_value(v, id, kind,
                                       (const u8 *)blob, (usize)blob_len,
                                       &pt_len, binding);
            if (out) *out_len = pt_len;
        }
    }
    sqlite3_finalize(s);

    if (out) {
        /* Bump access counters in a best-effort way (non-fatal if it fails). */
        sqlite3_stmt *u = NULL;
        if (sqlite3_prepare_v2(v->db,
            "UPDATE vault_secrets SET last_accessed=?, access_count=access_count+1 "
            "WHERE id=?", -1, &u, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(u, 1, (i64)time(NULL));
            sqlite3_bind_text(u, 2, id, -1, SQLITE_STATIC);
            sqlite3_step(u);
        }
        sqlite3_finalize(u);
        vault_touch_activity(v);
    }
    return out;
}

u8 *vault_secret_reveal(Vault *v, const char *id, usize *out_len) {
    return secret_reveal(v, id, out_len, NULL);
}

bool vault_secret_update(Vault *v, const char *id,
                         const u8 *new_plaintext, usize pt_len) {
    if (!v || !v->unlocked || !id || !new_plaintext) return false;
    SecretBinding binding; usize n = 0;
    u8 *old = secret_reveal(v, id, &n, &binding);
    if (!old) return false; /* Secrets without authenticated routing require explicit review. */
    vault_secret_release(old, n);
    if (!binding_current_row(v, &binding)) return false;
    return secret_update_bound(v, &binding, new_plaintext, pt_len);
}

bool vault_secret_update_binding(Vault *v, const char *id, VaultSecretKind expected_kind,
                                 const u8 *plaintext, usize len,
                                 const char *host_id, const char *scope,
                                 const char *env_name, const VaultHost *recipient) {
    if (!v || !v->unlocked || !id || !plaintext) return false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(v->db, "SELECT kind FROM vault_secrets WHERE id=?",
        -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) == SQLITE_INTEGER;
    i64 kind = ok ? sqlite3_column_int64(s, 0) : -1;
    sqlite3_finalize(s);
    SecretBinding binding;
    if ((host_id && *host_id && !recipient) || kind != expected_kind ||
        kind < VAULT_SECRET_PASSWORD || kind > VAULT_SECRET_NOTE ||
        !binding_make(v, &binding, id, (VaultSecretKind)kind, host_id, scope, env_name, recipient))
        return false;
    return secret_update_bound(v, &binding, plaintext, len);
}

u8 *vault_secret_reveal_for_ssh(Vault *v, const char *id, VaultSecretKind kind,
                               const char *hostname, i32 port, const char *username,
                               usize *out_len) {
    if (!out_len || !hostname || !username || kind < VAULT_SECRET_PASSWORD ||
        kind > VAULT_SECRET_PRIVATE_KEY || port <= 0 || port > 65535) return NULL;
    SecretBinding b; usize n = 0;
    u8 *pt = secret_reveal(v, id, &n, &b);
    if (!pt) return NULL;
    if (b.kind != kind || !b.host_id[0] || b.protocol != PROTO_SSH ||
        b.port != port || strcmp(b.hostname, hostname) || strcmp(b.username, username) ||
        !binding_current_row(v, &b)) {
        vault_secret_release(pt, n); return NULL;
    }
    *out_len = n;
    return pt;
}

i32 vault_secret_count_unreviewed(Vault *v) {
    if (!v || !v->db) return -1;
    sqlite3_stmt *s = NULL;
    /* A v2 envelope starts with its magic; other values use the
     * nonce|tag|ciphertext layout. substr() on a BLOB counts bytes, and loads
     * the whole value: it runs only on a row whose stored size (octet_length,
     * read without the bytes) is an envelope's. A larger row can never be
     * revealed, so there is nothing in it to review. */
    if (sqlite3_prepare_v2(v->db,
        "SELECT COUNT(*) FROM vault_secrets"
        " WHERE encrypted_value IS NOT NULL"
        "   AND octet_length(encrypted_value) BETWEEN 1 AND ?2"
        "   AND substr(encrypted_value,1,8) <> ?1",
        -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_blob(s, 1, SECRET_ENVELOPE_MAGIC, 8, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, secret_envelope_max());
    i32 n = sqlite3_step(s) == SQLITE_ROW ? (i32)sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    return n;
}

void vault_secret_release(u8 *plaintext, usize len) {
    if (!plaintext) return;
    /* decrypt_secret_value allocates len+1 (for the NUL), so wipe one
     * extra byte too. */
    crypto_secure_zero(plaintext, len + 1);
    free(plaintext);
}

/* ---------- List (metadata only) ---------- */

static void read_secret_row(sqlite3_stmt *s, VaultSecret *out) {
    memset(out, 0, sizeof *out);
    copy_text_col(out->id,       sizeof out->id,       s, 0);
    out->kind = (VaultSecretKind)sqlite3_column_int(s, 1);
    copy_text_col(out->label,            sizeof out->label,            s, 2);
    copy_text_col(out->host_id,          sizeof out->host_id,          s, 3);
    copy_text_col(out->scope,            sizeof out->scope,            s, 4);
    copy_text_col(out->env_name,         sizeof out->env_name,         s, 5);
    copy_text_col(out->key_algo,         sizeof out->key_algo,         s, 6);
    copy_text_col(out->key_fingerprint,  sizeof out->key_fingerprint,  s, 7);
    out->created_at    = sqlite3_column_int64(s, 8);
    out->updated_at    = sqlite3_column_int64(s, 9);
    out->last_accessed = sqlite3_column_int64(s, 10);
    out->access_count  = sqlite3_column_int64(s, 11);
}

#define SECRET_SELECT_COLS \
    "id,kind,label,host_id,scope,env_name,key_algo,key_fingerprint," \
    "created_at,updated_at,last_accessed,access_count"

i32 vault_secret_list(Vault *v,
                      i32 kind_filter,
                      const char *host_id_or_null,
                      VaultSecret *out, i32 max) {
    if (!v || !v->db || !out || max <= 0) return 0;

    sqlite3_stmt *s = NULL;
    const char *sql;
    if (kind_filter == VAULT_SECRET_KIND_ANY) {
        if (host_id_or_null && *host_id_or_null) {
            sql = "SELECT " SECRET_SELECT_COLS
                  " FROM vault_secrets WHERE host_id=? ORDER BY updated_at DESC";
        } else {
            sql = "SELECT " SECRET_SELECT_COLS
                  " FROM vault_secrets ORDER BY updated_at DESC";
        }
    } else {
        if (host_id_or_null && *host_id_or_null) {
            sql = "SELECT " SECRET_SELECT_COLS
                  " FROM vault_secrets WHERE kind=? AND host_id=? "
                  "ORDER BY updated_at DESC";
        } else {
            sql = "SELECT " SECRET_SELECT_COLS
                  " FROM vault_secrets WHERE kind=? "
                  "ORDER BY updated_at DESC";
        }
    }
    if (sqlite3_prepare_v2(v->db, sql, -1, &s, NULL) != SQLITE_OK) return 0;

    int col = 1;
    if (kind_filter != VAULT_SECRET_KIND_ANY) {
        sqlite3_bind_int(s, col++, kind_filter);
    }
    if (host_id_or_null && *host_id_or_null) {
        sqlite3_bind_text(s, col++, host_id_or_null, -1, SQLITE_STATIC);
    }

    i32 n = 0;
    while (n < max && sqlite3_step(s) == SQLITE_ROW) {
        read_secret_row(s, &out[n]);
        n++;
    }
    sqlite3_finalize(s);
    return n;
}

/* ---------- Env-var convenience ---------- */

i32 vault_env_list_for_host(Vault *v,
                   const VaultHost *recipient,
                   VaultEnvVar *out, i32 max) {
    if (!v || !v->unlocked || !out || max <= 0) return 0;

    /* Vars scoped to this host (if host_id given) PLUS globals. */
    sqlite3_stmt *s = NULL;
    const char *sql;
    const char *host_id_or_null = recipient ? recipient->id : NULL;
    if (host_id_or_null && *host_id_or_null) {
        sql = "SELECT id,kind,label,host_id,scope,env_name,key_algo,key_fingerprint,"
              "created_at,updated_at,last_accessed,access_count"
              " FROM vault_secrets"
              " WHERE kind=? AND (scope='global' OR (scope='host' AND host_id=?))"
              " ORDER BY env_name";
    } else {
        sql = "SELECT id,kind,label,host_id,scope,env_name,key_algo,key_fingerprint,"
              "created_at,updated_at,last_accessed,access_count"
              " FROM vault_secrets"
              " WHERE kind=? AND scope='global'"
              " ORDER BY env_name";
    }
    if (sqlite3_prepare_v2(v->db, sql, -1, &s, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(s, 1, (int)VAULT_SECRET_ENV_VAR);
    if (host_id_or_null && *host_id_or_null) {
        sqlite3_bind_text(s, 2, host_id_or_null, -1, SQLITE_STATIC);
    }

    i32 n = 0;
    while (n < max && sqlite3_step(s) == SQLITE_ROW) {
        VaultSecret meta;
        read_secret_row(s, &meta);
        if (!secret_text_col(s, 0, meta.id, sizeof meta.id)) continue;

        usize pt_len = 0;
        SecretBinding b;
        u8 *pt = secret_reveal(v, meta.id, &pt_len, &b);
        if (!pt) continue;
        bool allowed = b.kind == VAULT_SECRET_ENV_VAR && binding_current_row(v, &b) &&
            ((!strcmp(b.scope, "global") && !b.host_id[0]) ||
             (!strcmp(b.scope, "host") && binding_recipient(&b, recipient)));
        if (!allowed) { vault_secret_release(pt, pt_len); continue; }
        /* Truncate silently rather than fail the whole batch. Env-var
         * values are typically short; anything north of 1 KB is almost
         * certainly misuse. */
        usize copy = pt_len < sizeof out[n].value - 1
                     ? pt_len : sizeof out[n].value - 1;
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].name,  sizeof out[n].name,  "%s", b.env_name);
        snprintf(out[n].scope, sizeof out[n].scope, "%s", b.scope);
        memcpy(out[n].value, pt, copy);
        out[n].value[copy] = '\0';
        vault_secret_release(pt, pt_len);
        n++;
    }
    sqlite3_finalize(s);
    return n;
}

i32 vault_env_list(Vault *v, const char *host_id, VaultEnvVar *out, i32 max) {
    if (!host_id || !*host_id) return vault_env_list_for_host(v, NULL, out, max);
    SecretBinding b = {0};
    if (!v || !binding_text(b.host_id, sizeof b.host_id, host_id) || !binding_load_host(v, &b))
        return 0;
    VaultHost h = {0};
    memcpy(h.id, b.host_id, sizeof h.id);
    memcpy(h.hostname, b.hostname, sizeof h.hostname);
    memcpy(h.username, b.username, sizeof h.username);
    h.port = b.port; h.protocol = (VaultProtocol)b.protocol;
    return vault_env_list_for_host(v, &h, out, max);
}

/* =========================================================================
 * Hosts
 * ========================================================================= */

/* `*_sid` helper: bind an empty secret_id as NULL so FK constraints /
 * downstream queries can treat "no secret" uniformly. */
static void bind_sid(sqlite3_stmt *stmt, int col, const char *sid) {
    if (sid && sid[0]) sqlite3_bind_text(stmt, col, sid, -1, SQLITE_STATIC);
    else               sqlite3_bind_null(stmt, col);
}

bool vault_host_insert(Vault *v, const VaultHost *h) {
    const char *sql = "INSERT INTO hosts "
                      "(id,label,hostname,port,protocol,username,auth_method,"
                      " key_id,group_id,tags,color,last_connected,created_at,updated_at,"
                      " password_secret_id,passphrase_secret_id,private_key_secret_id)"
                      " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    i64 now = (i64)time(NULL);
    sqlite3_bind_text(stmt, 1, h->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, h->label, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, h->hostname, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, h->port);
    sqlite3_bind_text(stmt, 5, vault_protocol_str(h->protocol), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, h->username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, vault_auth_str(h->auth_method), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, h->key_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, h->group_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, h->tags, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, h->color, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 12, h->last_connected);
    sqlite3_bind_int64(stmt, 13, h->created_at ? h->created_at : now);
    sqlite3_bind_int64(stmt, 14, now);
    bind_sid(stmt, 15, h->password_secret_id);
    bind_sid(stmt, 16, h->passphrase_secret_id);
    bind_sid(stmt, 17, h->private_key_secret_id);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool vault_host_update(Vault *v, const VaultHost *h) {
    const char *sql = "UPDATE hosts SET "
                      "label=?,hostname=?,port=?,protocol=?,username=?,auth_method=?,"
                      "key_id=?,group_id=?,tags=?,color=?,last_connected=?,updated_at=?,"
                      "password_secret_id=?,passphrase_secret_id=?,private_key_secret_id=?"
                      " WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, h->label, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, h->hostname, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, h->port);
    sqlite3_bind_text(stmt, 4, vault_protocol_str(h->protocol), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, h->username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, vault_auth_str(h->auth_method), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, h->key_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, h->group_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, h->tags, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, h->color, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 11, h->last_connected);
    sqlite3_bind_int64(stmt, 12, (i64)time(NULL));
    bind_sid(stmt, 13, h->password_secret_id);
    bind_sid(stmt, 14, h->passphrase_secret_id);
    bind_sid(stmt, 15, h->private_key_secret_id);
    sqlite3_bind_text(stmt, 16, h->id, -1, SQLITE_STATIC);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool vault_host_delete(Vault *v, const char *id) {
    const char *sql = "DELETE FROM hosts WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static void copy_text_col(char *dst, usize cap, sqlite3_stmt *stmt, int col) {
    const char *s = (const char *)sqlite3_column_text(stmt, col);
    if (s) snprintf(dst, cap, "%s", s);
}

static void read_host_row(sqlite3_stmt *stmt, VaultHost *h) {
    memset(h, 0, sizeof(*h));
    copy_text_col(h->id,       sizeof h->id,       stmt, 0);
    copy_text_col(h->label,    sizeof h->label,    stmt, 1);
    copy_text_col(h->hostname, sizeof h->hostname, stmt, 2);
    h->port = sqlite3_column_int(stmt, 3);
    h->protocol = vault_protocol_from_str((const char *)sqlite3_column_text(stmt, 4));
    copy_text_col(h->username, sizeof h->username, stmt, 5);
    h->auth_method = vault_auth_from_str((const char *)sqlite3_column_text(stmt, 6));
    copy_text_col(h->key_id,   sizeof h->key_id,   stmt, 7);
    copy_text_col(h->group_id, sizeof h->group_id, stmt, 8);
    copy_text_col(h->tags,     sizeof h->tags,     stmt, 9);
    copy_text_col(h->color,    sizeof h->color,    stmt, 10);
    h->last_connected = sqlite3_column_int64(stmt, 11);
    h->created_at     = sqlite3_column_int64(stmt, 12);
    h->updated_at     = sqlite3_column_int64(stmt, 13);
    copy_text_col(h->password_secret_id,    sizeof h->password_secret_id,    stmt, 14);
    copy_text_col(h->passphrase_secret_id,  sizeof h->passphrase_secret_id,  stmt, 15);
    copy_text_col(h->private_key_secret_id, sizeof h->private_key_secret_id, stmt, 16);
}

/* Shared SELECT column list — any change here MUST be mirrored in
 * read_host_row. */
#define HOST_SELECT_COLS \
    "id,label,hostname,port,protocol,username,auth_method," \
    "key_id,group_id,tags,color,last_connected,created_at,updated_at," \
    "password_secret_id,passphrase_secret_id,private_key_secret_id"

i32 vault_host_list(Vault *v, VaultHost *out, i32 max) {
    const char *sql = "SELECT " HOST_SELECT_COLS
                      " FROM hosts ORDER BY last_connected DESC";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    i32 count = 0;
    while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
        read_host_row(stmt, &out[count]);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

bool vault_host_get(Vault *v, const char *id, VaultHost *out) {
    const char *sql = "SELECT " HOST_SELECT_COLS
                      " FROM hosts WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    if (ok) read_host_row(stmt, out);
    sqlite3_finalize(stmt);
    return ok;
}

/* Fetch the host at ordinal `index` in the SAME order vault_host_list returns
 * (last_connected DESC) — lets a caller resolve a list position without
 * materializing the whole list into a big stack array. */
bool vault_host_get_at(Vault *v, i32 index, VaultHost *out) {
    if (!v || !v->db || !out || index < 0) return false;
    const char *sql = "SELECT " HOST_SELECT_COLS
                      " FROM hosts ORDER BY last_connected DESC LIMIT 1 OFFSET ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, index);
    bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    if (ok) read_host_row(stmt, out);
    sqlite3_finalize(stmt);
    return ok;
}

/* Count secrets per kind via a GROUP BY aggregate — fills counts[0..ncounts)
 * and returns the total. Far cheaper than listing+materializing every secret
 * just to tally kinds (no value decryption, ≤ a handful of result rows). */
i32 vault_secret_counts(Vault *v, i32 *counts, i32 ncounts) {
    for (i32 i = 0; i < ncounts; i++) counts[i] = 0;
    if (!v || !v->db) return 0;
    const char *sql = "SELECT kind, COUNT(*) FROM vault_secrets GROUP BY kind";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    i32 total = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        i32 kind = sqlite3_column_int(stmt, 0);
        i32 c    = sqlite3_column_int(stmt, 1);
        if (kind >= 0 && kind < ncounts) counts[kind] = c;
        total += c;
    }
    sqlite3_finalize(stmt);
    return total;
}

/* =========================================================================
 * Groups (simplified)
 * ========================================================================= */

bool vault_group_insert(Vault *v, const VaultGroup *g) {
    const char *sql = "INSERT INTO groups (id,name,parent_id,color) VALUES (?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, g->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, g->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, g->parent_id[0] ? g->parent_id : NULL, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, g->color, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool vault_group_delete(Vault *v, const char *id) {
    const char *sql = "DELETE FROM groups WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

i32 vault_group_list(Vault *v, VaultGroup *out, i32 max) {
    const char *sql = "SELECT id,name,parent_id,color FROM groups ORDER BY name";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    i32 count = 0;
    while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
        VaultGroup *g = &out[count];
        memset(g, 0, sizeof(*g));
        snprintf(g->id, sizeof(g->id), "%s", (const char *)sqlite3_column_text(stmt, 0));
        snprintf(g->name, sizeof(g->name), "%s", (const char *)sqlite3_column_text(stmt, 1));
        const char *p = (const char *)sqlite3_column_text(stmt, 2);
        if (p) snprintf(g->parent_id, sizeof(g->parent_id), "%s", p);
        const char *c = (const char *)sqlite3_column_text(stmt, 3);
        if (c) snprintf(g->color, sizeof(g->color), "%s", c);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

/* =========================================================================
 * Snippets
 * ========================================================================= */

bool vault_snippet_insert(Vault *v, const VaultSnippet *s) {
    const char *sql = "INSERT INTO snippets (id,label,command,description,tags) VALUES (?,?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, s->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, s->label, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, s->command, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, s->description, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, s->tags, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool vault_snippet_update(Vault *v, const VaultSnippet *s) {
    const char *sql = "UPDATE snippets SET label=?,command=?,description=?,tags=? WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, s->label, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, s->command, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, s->description, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, s->tags, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, s->id, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool vault_snippet_delete(Vault *v, const char *id) {
    const char *sql = "DELETE FROM snippets WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

/* Read one snippet row from `stmt` (already SQLITE_ROW-positioned) into `out`. */
static void snippet_fill_from_stmt(VaultSnippet *out, sqlite3_stmt *stmt) {
    memset(out, 0, sizeof(*out));
    snprintf(out->id,    sizeof(out->id),    "%s", (const char *)sqlite3_column_text(stmt, 0));
    snprintf(out->label, sizeof(out->label), "%s", (const char *)sqlite3_column_text(stmt, 1));
    snprintf(out->command, sizeof(out->command), "%s", (const char *)sqlite3_column_text(stmt, 2));
    const char *d = (const char *)sqlite3_column_text(stmt, 3);
    if (d) snprintf(out->description, sizeof(out->description), "%s", d);
    const char *t = (const char *)sqlite3_column_text(stmt, 4);
    if (t) snprintf(out->tags, sizeof(out->tags), "%s", t);
}

i32 vault_snippet_list(Vault *v, VaultSnippet *out, i32 max) {
    const char *sql = "SELECT id,label,command,description,tags FROM snippets ORDER BY label";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    i32 count = 0;
    while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
        snippet_fill_from_stmt(&out[count], stmt);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

i32 vault_snippet_for_each(Vault *v, vault_snippet_cb cb, void *user) {
    if (!v || !cb) return 0;
    const char *sql = "SELECT id,label,command,description,tags FROM snippets ORDER BY label";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    i32 count = 0;
    /* One reusable record on the stack — cycled per row instead of allocating
     * an array sized for the full result. */
    VaultSnippet rec;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        snippet_fill_from_stmt(&rec, stmt);
        count++;
        if (!cb(&rec, user)) break;
    }
    sqlite3_finalize(stmt);
    return count;
}

/* =========================================================================
 * Keys
 * ========================================================================= */

bool vault_key_insert(Vault *v, const VaultKey *k) {
    const char *sql = "INSERT INTO ssh_keys (id,label,algorithm,public_key,fingerprint,created_at) VALUES (?,?,?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, k->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, k->label, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, k->algorithm, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, k->public_key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, k->fingerprint, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, k->created_at ? k->created_at : (i64)time(NULL));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool vault_key_delete(Vault *v, const char *id) {
    const char *sql = "DELETE FROM ssh_keys WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

i32 vault_key_list(Vault *v, VaultKey *out, i32 max) {
    const char *sql = "SELECT id,label,algorithm,public_key,fingerprint,created_at FROM ssh_keys ORDER BY label";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    i32 count = 0;
    while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
        VaultKey *k = &out[count];
        memset(k, 0, sizeof(*k));
        snprintf(k->id, sizeof(k->id), "%s", (const char *)sqlite3_column_text(stmt, 0));
        snprintf(k->label, sizeof(k->label), "%s", (const char *)sqlite3_column_text(stmt, 1));
        snprintf(k->algorithm, sizeof(k->algorithm), "%s", (const char *)sqlite3_column_text(stmt, 2));
        snprintf(k->public_key, sizeof(k->public_key), "%s", (const char *)sqlite3_column_text(stmt, 3));
        snprintf(k->fingerprint, sizeof(k->fingerprint), "%s", (const char *)sqlite3_column_text(stmt, 4));
        k->created_at = sqlite3_column_int64(stmt, 5);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

/* =========================================================================
 * Connection History
 * ========================================================================= */

bool vault_add_history(Vault *v, const char *hostname, i32 port,
                       const char *username, const char *auth_method, bool succeeded) {
    if (!v || !hostname) return false;

    /* Insert the new history entry */
    const char *sql = "INSERT INTO connection_history (hostname,port,username,auth_method,timestamp,succeeded)"
                      " VALUES (?,?,?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    f64 now = (f64)time(NULL);
    sqlite3_bind_text(stmt, 1, hostname, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, port);
    sqlite3_bind_text(stmt, 3, username ? username : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, auth_method ? auth_method : "password", -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 5, now);
    sqlite3_bind_int(stmt, 6, succeeded ? 1 : 0);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);

    /* Prune old entries beyond VAULT_HISTORY_MAX */
    if (ok) {
        const char *prune = "DELETE FROM connection_history WHERE id NOT IN "
                            "(SELECT id FROM connection_history ORDER BY timestamp DESC LIMIT ?)";
        sqlite3_stmt *ps;
        if (sqlite3_prepare_v2(v->db, prune, -1, &ps, NULL) == SQLITE_OK) {
            sqlite3_bind_int(ps, 1, VAULT_HISTORY_MAX);
            sqlite3_step(ps);
            sqlite3_finalize(ps);
        }
    }

    return ok;
}

bool vault_update_history_result(Vault *v, const char *hostname, i32 port,
                                 const char *username, bool succeeded) {
    if (!v || !hostname) return false;

    /* Update the most recent history entry matching this connection */
    const char *sql = "UPDATE connection_history SET succeeded=? "
                      "WHERE id=(SELECT id FROM connection_history "
                      "WHERE hostname=? AND port=? AND username=? "
                      "ORDER BY timestamp DESC LIMIT 1)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, succeeded ? 1 : 0);
    sqlite3_bind_text(stmt, 2, hostname, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, port);
    sqlite3_bind_text(stmt, 4, username ? username : "", -1, SQLITE_STATIC);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

i32 vault_get_history(Vault *v, VaultHistoryEntry *entries, i32 max_entries) {
    if (!v || !entries) return -1;

    const char *sql = "SELECT hostname,port,username,auth_method,timestamp,succeeded "
                      "FROM connection_history ORDER BY timestamp DESC LIMIT ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, max_entries);

    i32 count = 0;
    while (count < max_entries && sqlite3_step(stmt) == SQLITE_ROW) {
        VaultHistoryEntry *e = &entries[count];
        memset(e, 0, sizeof(*e));
        const char *h = (const char *)sqlite3_column_text(stmt, 0);
        if (h) snprintf(e->hostname, sizeof(e->hostname), "%s", h);
        e->port = sqlite3_column_int(stmt, 1);
        const char *u = (const char *)sqlite3_column_text(stmt, 2);
        if (u) snprintf(e->username, sizeof(e->username), "%s", u);
        const char *a = (const char *)sqlite3_column_text(stmt, 3);
        if (a) snprintf(e->auth_method_str, sizeof(e->auth_method_str), "%s", a);
        e->timestamp = sqlite3_column_double(stmt, 4);
        e->succeeded = sqlite3_column_int(stmt, 5) != 0;
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

bool vault_clear_history(Vault *v) {
    if (!v) return false;
    return sqlite3_exec(v->db, "DELETE FROM connection_history", NULL, NULL, NULL) == SQLITE_OK;
}

bool vault_forget_history(Vault *v, const char *hostname, i32 port, const char *username) {
    if (!v || !hostname) return false;
    /* Matched the way vault_get_history lists a row: a NULL user reads as ""
     * and a NULL port as 0, so the row the list shows is the row deleted (a
     * plain `username=?` never matches a NULL). Nothing left to delete
     * (another window forgot it first) is still a success. */
    const char *sql = "DELETE FROM connection_history WHERE hostname=? "
                      "AND COALESCE(port, 0)=? AND COALESCE(username, '')=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, hostname, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, port);
    sqlite3_bind_text(stmt, 3, username ? username : "", -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

/* =========================================================================
 * Command History
 * ========================================================================= */

static void trim_command_text(const char *src, char *dst, usize dst_size) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src) return;

    const char *start = src;
    while (*start && isspace((unsigned char)*start)) start++;

    const char *end = src + strlen(src);
    while (end > start && isspace((unsigned char)end[-1])) end--;

    usize len = (usize)(end - start);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, start, len);
    dst[len] = '\0';
}

static void vault_prune_command_history(Vault *v) {
    if (!v) return;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM command_history WHERE timestamp < ?";
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    f64 cutoff = (f64)time(NULL) -
                 (f64)(VAULT_COMMAND_HISTORY_RETENTION_DAYS * 24 * 60 * 60);
    sqlite3_bind_double(stmt, 1, cutoff);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool vault_add_command_history(Vault *v, const char *command) {
    if (!v || !command) return false;

    char cleaned[4096];
    trim_command_text(command, cleaned, sizeof(cleaned));
    if (!cleaned[0]) return false;

    const char *sql = "INSERT INTO command_history (command, timestamp) VALUES (?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, cleaned, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, (f64)time(NULL));

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);

    if (ok) vault_prune_command_history(v);
    return ok;
}

i32 vault_get_recent_command_history(Vault *v,
                                     VaultCommandHistoryEntry *entries,
                                     i32 max_entries) {
    if (!v || !entries || max_entries <= 0) return -1;

    vault_prune_command_history(v);

    const char *sql = "SELECT command, timestamp "
                      "FROM command_history "
                      "WHERE timestamp >= ? "
                      "ORDER BY timestamp DESC "
                      "LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    f64 cutoff = (f64)time(NULL) -
                 (f64)(VAULT_COMMAND_HISTORY_RETENTION_DAYS * 24 * 60 * 60);
    sqlite3_bind_double(stmt, 1, cutoff);
    sqlite3_bind_int(stmt, 2, max_entries);

    i32 count = 0;
    while (count < max_entries && sqlite3_step(stmt) == SQLITE_ROW) {
        VaultCommandHistoryEntry *e = &entries[count];
        memset(e, 0, sizeof(*e));

        const char *cmd = (const char *)sqlite3_column_text(stmt, 0);
        if (cmd) snprintf(e->command, sizeof(e->command), "%s", cmd);
        e->timestamp = sqlite3_column_double(stmt, 1);
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

/* =========================================================================
 * Favorites
 * ========================================================================= */

bool vault_set_favorite(Vault *v, const char *hostname, i32 port,
                        const char *username, bool is_favorite) {
    if (!v || !hostname) return false;

    if (is_favorite) {
        const char *sql = "INSERT OR IGNORE INTO favorites (hostname,port,username,label)"
                          " VALUES (?,?,?,?)";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, hostname, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, port);
        sqlite3_bind_text(stmt, 3, username ? username : "", -1, SQLITE_STATIC);
        /* Generate a default label: user@host */
        char label[256];
        if (username && username[0])
            snprintf(label, sizeof(label), "%s@%s", username, hostname);
        else
            snprintf(label, sizeof(label), "%s", hostname);
        sqlite3_bind_text(stmt, 4, label, -1, SQLITE_TRANSIENT);
        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    } else {
        const char *sql = "DELETE FROM favorites WHERE hostname=? AND port=? AND username=?";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, hostname, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, port);
        sqlite3_bind_text(stmt, 3, username ? username : "", -1, SQLITE_STATIC);
        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }
}

bool vault_is_favorite(Vault *v, const char *hostname, i32 port,
                       const char *username) {
    if (!v || !hostname) return false;

    const char *sql = "SELECT COUNT(*) FROM favorites WHERE hostname=? AND port=? AND username=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, hostname, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, port);
    sqlite3_bind_text(stmt, 3, username ? username : "", -1, SQLITE_STATIC);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    return found;
}

i32 vault_get_favorites(Vault *v, VaultHost *hosts, i32 max) {
    if (!v || !hosts) return -1;

    /* Join favorites with hosts table if a matching host exists, otherwise return
     * a synthetic VaultHost from the favorites table alone. */
    const char *sql =
        "SELECT f.hostname, f.port, f.username, f.label, "
        "  h.id, h.protocol, h.auth_method, h.key_id, h.group_id, h.tags, h.color, "
        "  h.last_connected, h.created_at, h.updated_at "
        "FROM favorites f "
        "LEFT JOIN hosts h ON f.hostname=h.hostname AND f.port=h.port AND f.username=h.username "
        "LIMIT ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, max);

    i32 count = 0;
    while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
        VaultHost *out = &hosts[count];
        memset(out, 0, sizeof(*out));

        const char *fhost = (const char *)sqlite3_column_text(stmt, 0);
        if (fhost) snprintf(out->hostname, sizeof(out->hostname), "%s", fhost);
        out->port = sqlite3_column_int(stmt, 1);
        const char *fuser = (const char *)sqlite3_column_text(stmt, 2);
        if (fuser) snprintf(out->username, sizeof(out->username), "%s", fuser);
        const char *flabel = (const char *)sqlite3_column_text(stmt, 3);
        if (flabel) snprintf(out->label, sizeof(out->label), "%s", flabel);

        /* If there is a matching host record, fill in the rest */
        const char *hid = (const char *)sqlite3_column_text(stmt, 4);
        if (hid) {
            snprintf(out->id, sizeof(out->id), "%s", hid);
            out->protocol = vault_protocol_from_str((const char *)sqlite3_column_text(stmt, 5));
            out->auth_method = vault_auth_from_str((const char *)sqlite3_column_text(stmt, 6));
            const char *kid = (const char *)sqlite3_column_text(stmt, 7);
            if (kid) snprintf(out->key_id, sizeof(out->key_id), "%s", kid);
            const char *gid = (const char *)sqlite3_column_text(stmt, 8);
            if (gid) snprintf(out->group_id, sizeof(out->group_id), "%s", gid);
            const char *tags = (const char *)sqlite3_column_text(stmt, 9);
            if (tags) snprintf(out->tags, sizeof(out->tags), "%s", tags);
            const char *clr = (const char *)sqlite3_column_text(stmt, 10);
            if (clr) snprintf(out->color, sizeof(out->color), "%s", clr);
            out->last_connected = sqlite3_column_int64(stmt, 11);
            out->created_at = sqlite3_column_int64(stmt, 12);
            out->updated_at = sqlite3_column_int64(stmt, 13);
        } else {
            /* Synthetic entry: default to SSH protocol */
            out->protocol = PROTO_SSH;
            out->auth_method = VAUTH_PASSWORD;
            if (out->port == 0) out->port = 22;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}
