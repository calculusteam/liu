/*
 * Liu - SSH known_hosts management
 * Verify server host keys against ~/.ssh/known_hosts.
 * List, remove, and manage known host entries.
 * Check host key revocation against ~/.ssh/revoked_keys.
 */
#include "ssh/known_hosts.h"
#include "ssh/ssh_target.h"
#include "core/checked.h"
#include "core/net.h"
#include "core/string_utils.h"   /* base64_encode */
#include <libssh2.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp — glibc declares it here, not in string.h */
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* =========================================================================
 * Helpers
 * ========================================================================= */

static bool ssh_home_path(char *path, usize path_size, const char *rel) {
    if (!path || path_size == 0 || !rel) return false;
    const char *home = net_home_dir();
    if (!home[0]) { path[0] = '\0'; return false; }
    int n = snprintf(path, path_size, "%s/%s", home, rel);
    if (n < 0 || (usize)n >= path_size) {
        path[0] = '\0';
        return false;
    }
    return true;
}

static bool get_known_hosts_path(char *path, usize path_size) {
    return ssh_home_path(path, path_size, ".ssh/known_hosts");
}

static bool get_revoked_keys_path(char *path, usize path_size) {
    return ssh_home_path(path, path_size, ".ssh/revoked_keys");
}

typedef struct {
    char *line;
    bool  keep;
} KnownHostLine;

/* The directory holding the trust store (~/.ssh) has to be the user's own and
 * not writable by other users, or the file check below is a check on whatever
 * somebody else rename()d into place a moment ago. Group bits are tolerated:
 * OpenSSH's own client does not police them on the client side and
 * shared-group home directories exist. */
static bool known_hosts_dir_safe(const char *path) {
    char dir[4096];
    int n = snprintf(dir, sizeof dir, "%s", path);
    if (n < 0 || (usize)n >= sizeof dir) return false;
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return false;
    *slash = '\0';
    struct stat st;
    if (lstat(dir, &st) != 0) return false;
    return S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & S_IWOTH);
}

/* If ~/.ssh is absent, treat the known-hosts store as empty.
 * known_hosts_dir_make() creates the directory with mode 0700 before a write.
 */
static bool known_hosts_dir_missing(const char *path) {
    char dir[4096];
    int n = snprintf(dir, sizeof dir, "%s", path);
    if (n < 0 || (usize)n >= sizeof dir) return false;
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return false;
    *slash = '\0';
    struct stat st;
    return lstat(dir, &st) != 0 && errno == ENOENT;
}

/* Only the missing folder itself is made, never its parents (that would be a
 * home directory), and anything already in its place is left for
 * known_hosts_dir_safe to judge. */
static bool known_hosts_dir_make(const char *path) {
    char dir[4096];
    int n = snprintf(dir, sizeof dir, "%s", path);
    if (n < 0 || (usize)n >= sizeof dir) return false;
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return false;
    *slash = '\0';
    return mkdir(dir, 0700) == 0 || errno == EEXIST;
}

static bool known_hosts_path_safe(const char *path, bool allow_missing) {
    if (!known_hosts_dir_safe(path))
        return allow_missing && known_hosts_dir_missing(path);
    struct stat st;
    if (lstat(path, &st) == 0)
        return S_ISREG(st.st_mode) && st.st_uid == geteuid();
    return allow_missing && errno == ENOENT;
}

/* libssh2 needs the key TYPE bit to serialise an entry: with KEY_UNKNOWN and
 * no type name, knownhost_writeline() returns METHOD_NOT_SUPPORTED and the
 * whole writefile fails. So every host-key type libssh2 names — ECDSA and
 * ed25519 as well as RSA and DSS — maps to its own bit. */
static int known_hosts_key_typemask(int key_type) {
    switch (key_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
#ifdef LIBSSH2_HOSTKEY_TYPE_DSS
    case LIBSSH2_HOSTKEY_TYPE_DSS:       return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
#endif
    default:                             return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

/* OpenSSH's "[host]:port" spelling for a non-default port, so two servers
 * sharing a hostname on different ports are tracked as distinct identities.
 * Port 22 keeps the plain hostname for compatibility. */
static void known_hosts_entry_name(const char *hostname, i32 port, char *out, usize cap) {
    if (port > 0 && port != 22) snprintf(out, cap, "[%s]:%d", hostname, port);
    else                        snprintf(out, cap, "%s", hostname);
}

/* An RSA-8192 key is under 1.5 KB of base64; a line longer than this is not
 * an entry and is left alone. */
#define KNOWN_HOSTS_LINE_MAX (64 * 1024)

/* Is this line one libssh2 should be shown? Comments, blank lines and
 * OpenSSH's markers (`@revoked`, `@cert-authority`) are not: libssh2 reads a
 * marker as a host of that name. */
static bool known_hosts_line_is_entry(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    return *line && *line != '\n' && *line != '\r' && *line != '#' && *line != '@';
}

/* Read `path` into `nh` one line at a time, leaving out what libssh2 cannot
 * read.
 *
 * libssh2_knownhost_readfile() stops at the FIRST line it cannot parse:
 * every host after such a line would read as unknown ("New SSH Host Key"
 * for a host that is pinned), and the next accepted key would write back
 * only what had been read — the rest of the file, comments included, would
 * be gone, for OpenSSH too. Reading by the line keeps a bad line that
 * line's problem. */
static void known_hosts_read_lines(LIBSSH2_KNOWNHOSTS *nh, const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        if ((usize)n > KNOWN_HOSTS_LINE_MAX || !known_hosts_line_is_entry(line)) continue;
        (void)libssh2_knownhost_readline(nh, line, (size_t)n, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    }
    free(line);
    fclose(f);
}

/* The machine-wide store an administrator fills. Read, never written, and
 * only when it is root's own file. */
static const char *known_hosts_system_path(void) {
    static const char *const path = "/etc/ssh/ssh_known_hosts";
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) return NULL;
    return path;
}

/* The store's answer for host:port, asked the way OpenSSH asks: the exact
 * name first ("[host]:port" for a port that is not 22) and, only when that
 * name has NO entry at all, the plain host's key as a MATCH — never as a
 * contradiction. libssh2's own checkp falls back to the plain name even when
 * "[host]:port" is pinned to another key, so with it a server on port N
 * showing the port-22 key would pass, and accepting a change on port N would
 * delete the port-22 pin. */
static int known_hosts_check_exact(LIBSSH2_KNOWNHOSTS *nh, const char *hostname, i32 port,
                                   const char *key, size_t key_len, int typemask) {
    char name[320];
    known_hosts_entry_name(hostname, port, name, sizeof name);
    int check = libssh2_knownhost_check(nh, name, key, key_len, typemask, NULL);
    if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND && port > 0 && port != 22 &&
        libssh2_knownhost_check(nh, hostname, key, key_len, typemask, NULL) ==
            LIBSSH2_KNOWNHOST_CHECK_MATCH)
        return LIBSSH2_KNOWNHOST_CHECK_MATCH;
    return check;
}

/* `@revoked <hosts> <type> <base64>` in a known_hosts file: is that this key?
 * The host pattern is not weighed — a key somebody marked as revoked is
 * refused wherever it turns up. libssh2 has no notion of the marker, and on
 * its own would accept a key that is both pinned and revoked without a word.
 */
bool known_hosts_store_revokes(const char *path, const unsigned char *fp,
                               const char *key, usize key_len) {
    if (!path || !fp) return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return false; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    bool revoked = false;
    while (!revoked && (n = getline(&line, &cap, f)) > 0) {
        if ((usize)n > KNOWN_HOSTS_LINE_MAX) continue;
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' ||
                         line[n-1] == ' ' || line[n-1] == '\t')) line[--n] = '\0';
        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, "@revoked", 8) != 0 || (q[8] != ' ' && q[8] != '\t')) continue;
        q += 8;
        while (*q == ' ' || *q == '\t') q++;
        q += strcspn(q, " \t");                       /* the host pattern */
        while (*q == ' ' || *q == '\t') q++;
        if (*q) revoked = known_hosts_revocation_line_matches(q, fp, key, key_len);
    }
    free(line);
    fclose(f);
    return revoked;
}

typedef enum {
    KH_DROP_NONE = 0,
    KH_DROP_OTHER_KEYS,     /* the entries for `name` whose key is NOT this one */
    KH_DROP_THIS_KEY,       /* the entries for `name` whose key IS this one */
} KnownHostsDrop;

/* Rewrite the store line by line: every line is copied as it stands, except
 * the entries for `name` that `drop` names, and `append` (one entry) is added
 * at the end. Lines libssh2 cannot read, comments and markers are copied like
 * any other — the file is the user's, and OpenSSH reads it too. Serialising
 * libssh2's own view of the file instead would drop every line after one it
 * could not parse.
 *
 * Atomic and 0600: a temporary sibling, fsynced, renamed over the file. */
static bool known_hosts_rewrite(LIBSSH2_SESSION *session, const char *path, const char *name,
                                const char *key, size_t key_len, KnownHostsDrop drop,
                                const char *append, i32 *dropped) {
    if (dropped) *dropped = 0;
    if (append && known_hosts_dir_missing(path) && !known_hosts_dir_make(path)) return false;
    if (!known_hosts_path_safe(path, true)) return false;
    FILE *in = NULL;
    int in_fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in_fd >= 0) {
        in = fdopen(in_fd, "r");
        if (!in) { close(in_fd); return false; }
    } else if (errno != ENOENT) {
        return false;
    } else if (!append) {
        return true;                                  /* no file, nothing to add */
    }

    char tmp[4160];
    int tn = snprintf(tmp, sizeof tmp, "%s.liu.XXXXXX", path);
    int fd = (tn > 0 && (usize)tn < sizeof tmp) ? mkstemp(tmp) : -1;
    FILE *out = NULL;
    if (fd >= 0 && fchmod(fd, 0600) == 0) out = fdopen(fd, "w");
    if (!out) {
        if (fd >= 0) { close(fd); unlink(tmp); }
        if (in) fclose(in);
        return false;
    }

    bool ok = true, ends_nl = true;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while (ok && in && (n = getline(&line, &cap, in)) > 0) {
        bool gone = false;
        if (drop != KH_DROP_NONE && (usize)n <= KNOWN_HOSTS_LINE_MAX &&
            known_hosts_line_is_entry(line)) {
            LIBSSH2_KNOWNHOSTS *one = libssh2_knownhost_init(session);
            if (!one) { ok = false; break; }
            if (libssh2_knownhost_readline(one, line, (size_t)n,
                                           LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0) {
                int c = libssh2_knownhost_check(one, name, key, key_len,
                            LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW, NULL);
                gone = (drop == KH_DROP_OTHER_KEYS && c == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) ||
                       (drop == KH_DROP_THIS_KEY && c == LIBSSH2_KNOWNHOST_CHECK_MATCH);
            }
            libssh2_knownhost_free(one);
        }
        if (gone) { if (dropped) (*dropped)++; continue; }
        if (fwrite(line, 1, (size_t)n, out) != (size_t)n) ok = false;
        ends_nl = line[n - 1] == '\n';
    }
    if (in && ferror(in)) ok = false;
    free(line);
    if (in) fclose(in);

    if (ok && append && append[0]) {
        usize al = strlen(append);
        if (!ends_nl && fputc('\n', out) == EOF) ok = false;
        if (ok && fwrite(append, 1, al, out) != al) ok = false;
        if (ok && append[al - 1] != '\n' && fputc('\n', out) == EOF) ok = false;
    }
    if (ok && fflush(out) != 0) ok = false;
    if (ok && fsync(fileno(out)) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    if (ok && rename(tmp, path) == 0) return true;
    unlink(tmp);
    return false;
}

static bool known_hosts_replace(const char *path, const KnownHostLine *lines,
                                i32 line_count) {
    if (!path || line_count < 0 || (line_count > 0 && !lines)) return false;
    if (known_hosts_dir_missing(path) && !known_hosts_dir_make(path)) return false;
    if (!known_hosts_path_safe(path, true)) return false;
    char tmp[4160];
    int n = snprintf(tmp, sizeof tmp, "%s.liu.XXXXXX", path);
    if (n < 0 || (usize)n >= sizeof tmp) return false;
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    bool ok = fchmod(fd, 0600) == 0;
    FILE *f = ok ? fdopen(fd, "w") : NULL;
    if (!f) {
        close(fd);
        unlink(tmp);
        return false;
    }
    for (i32 i = 0; ok && i < line_count; i++) {
        if (lines[i].keep && fputs(lines[i].line, f) == EOF) ok = false;
    }
    if (ok && fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) == 0) return true;
    unlink(tmp);
    return false;
}

/* Convert raw binary to hex string */
static void bin_to_hex(const u8 *data, usize len, char *out, usize out_size) {
    if (!out || out_size == 0) return;
    static const char hex[] = "0123456789abcdef";
    usize p = 0;
    for (usize i = 0; i < len && p + 2 < out_size; i++) {
        out[p++] = hex[(data[i] >> 4) & 0xF];
        out[p++] = hex[data[i] & 0xF];
    }
    out[p] = '\0';
}

/* =========================================================================
 * Verify
 * ========================================================================= */

/* ---- the store, with the key passed in ---------------------------------
 * verify / add / forget below are these with the session's negotiated key.
 * Declared in the header because a test has no handshake to give them. */

i32 known_hosts_store_check(LIBSSH2_SESSION *session, const char *path, const char *hostname,
                            i32 port, const char *key, usize key_len) {
    LIBSSH2_KNOWNHOSTS *nh = session && path ? libssh2_knownhost_init(session) : NULL;
    if (!nh) return LIBSSH2_KNOWNHOST_CHECK_FAILURE;
    known_hosts_read_lines(nh, path);
    /* No key-type bits on purpose: a host that has ANY stored key and shows
     * one that is not among them is a contradiction, whatever its type. A
     * host pinned with one type is kept from negotiating another before the
     * handshake: the stored types are offered first
     * (known_hosts_stored_kinds). */
    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    int check = known_hosts_check_exact(nh, hostname, port, key, key_len, typemask);
    libssh2_knownhost_free(nh);
    return check;
}

bool known_hosts_store_add(LIBSSH2_SESSION *session, const char *path, const char *hostname,
                           i32 port, const char *key, usize key_len, int key_typemask) {
    /* The name becomes a line of a file other programs trust. A direct
     * connection could not have resolved a name with a space or a line break
     * in it; one reached through a jump host is resolved by the hop. */
    if (!session || !path || !key || !ssh_target_name_is_safe(hostname)) return false;
    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | key_typemask;
    /* A type libssh2 cannot name cannot be written back, so it is refused
     * here rather than in a write that fails later. */
    if ((typemask & LIBSSH2_KNOWNHOST_KEY_MASK) == LIBSSH2_KNOWNHOST_KEY_UNKNOWN) return false;
    LIBSSH2_KNOWNHOSTS *nh = libssh2_knownhost_init(session);
    if (!nh) return false;

    /* libssh2 only spells the ONE new line; the file itself is copied, not
     * re-serialised (known_hosts_rewrite). */
    char name[320];
    known_hosts_entry_name(hostname, port, name, sizeof name);
    struct libssh2_knownhost *node = NULL;
    bool ok = libssh2_knownhost_addc(nh, name, NULL, key, key_len, NULL, 0, typemask, &node) == 0 &&
              node;
    usize cap = key_len * 2 + sizeof name + 128;
    char *line = ok ? malloc(cap) : NULL;
    size_t line_len = 0;
    ok = line && libssh2_knownhost_writeline(nh, node, line, cap, &line_len,
                                             LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0 &&
         line_len > 0 && line_len < cap;
    if (ok) {
        line[line_len] = '\0';
        ok = known_hosts_rewrite(session, path, name, key, key_len, KH_DROP_NONE, line, NULL);
    }
    free(line);
    libssh2_knownhost_free(nh);
    return ok;
}

bool known_hosts_store_forget(LIBSSH2_SESSION *session, const char *path, const char *hostname,
                              i32 port, const char *key, usize key_len) {
    if (!session || !path || !key || !hostname || !hostname[0]) return false;
    /* The exact name only: "[host]:port" for a port that is not 22. The
     * plain host's pin belongs to port 22 and stays. */
    char name[320];
    known_hosts_entry_name(hostname, port, name, sizeof name);
    return known_hosts_rewrite(session, path, name, key, key_len, KH_DROP_OTHER_KEYS, NULL, NULL);
}

HostKeyStatus known_hosts_verify(LIBSSH2_SESSION *session, const char *hostname, i32 port) {
    if (!session || !hostname || !hostname[0] || strnlen(hostname, 256) >= 256 ||
        port <= 0 || port > 65535) return HOST_KEY_ERROR;

    char path[4096];
    if (!get_known_hosts_path(path, sizeof(path)) ||
        !known_hosts_path_safe(path, true)) return HOST_KEY_ERROR;

    /* Get remote host key */
    size_t key_len;
    int key_type;
    const char *key = libssh2_session_hostkey(session, &key_len, &key_type);
    if (!key) return HOST_KEY_ERROR;

    i32 check = known_hosts_store_check(session, path, hostname, port, key, key_len);
    if (check == LIBSSH2_KNOWNHOST_CHECK_FAILURE) return HOST_KEY_ERROR;
    /* The user's own store had nothing to say: ask the machine's. */
    const char *sys = known_hosts_system_path();
    if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND && sys)
        check = known_hosts_store_check(session, sys, hostname, port, key, key_len);

    const unsigned char *fp = (const unsigned char *)libssh2_hostkey_hash(
        session, LIBSSH2_HOSTKEY_HASH_SHA256);
    bool revoked = known_hosts_is_revoked(session) ||
                   known_hosts_store_revokes(path, fp, key, key_len) ||
                   known_hosts_store_revokes(sys, fp, key, key_len);
    return known_hosts_check_status(check, revoked);
}

/* Read stored host-key types before the handshake so trusted types are
 * offered first during algorithm negotiation. The library matcher handles
 * hashed entries and [host]:port names. */
void known_hosts_stored_kinds(LIBSSH2_SESSION *session, const char *hostname, i32 port,
                              bool stored[SSH_HOSTKEY_KINDS]) {
    for (i32 k = 0; k < SSH_HOSTKEY_KINDS; k++) stored[k] = false;
    if (!session || !hostname || !hostname[0] || strnlen(hostname, 256) >= 256 ||
        port <= 0 || port > 65535) return;
    static const int bits[SSH_HOSTKEY_KINDS] = {
        [SSH_HOSTKEY_ED25519]   = LIBSSH2_KNOWNHOST_KEY_ED25519,
        [SSH_HOSTKEY_ECDSA_256] = LIBSSH2_KNOWNHOST_KEY_ECDSA_256,
        [SSH_HOSTKEY_ECDSA_384] = LIBSSH2_KNOWNHOST_KEY_ECDSA_384,
        [SSH_HOSTKEY_ECDSA_521] = LIBSSH2_KNOWNHOST_KEY_ECDSA_521,
        [SSH_HOSTKEY_RSA]       = LIBSSH2_KNOWNHOST_KEY_SSHRSA,
    };
    char path[4096], name[320];
    const char *files[2] = { NULL, known_hosts_system_path() };
    if (get_known_hosts_path(path, sizeof path) && known_hosts_path_safe(path, false))
        files[0] = path;
    known_hosts_entry_name(hostname, port, name, sizeof name);
    static const char probe[] = "liu-no-such-key";
    for (i32 fi = 0; fi < 2; fi++) {
        if (!files[fi]) continue;
        LIBSSH2_KNOWNHOSTS *nh = libssh2_knownhost_init(session);
        if (!nh) continue;
        known_hosts_read_lines(nh, files[fi]);
        for (i32 k = 0; k < SSH_HOSTKEY_KINDS; k++) {
            int mask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | bits[k];
            if (libssh2_knownhost_check(nh, name, probe, sizeof probe - 1, mask, NULL) ==
                    LIBSSH2_KNOWNHOST_CHECK_MISMATCH ||
                (port != 22 &&
                 libssh2_knownhost_check(nh, hostname, probe, sizeof probe - 1, mask, NULL) ==
                    LIBSSH2_KNOWNHOST_CHECK_MISMATCH))
                stored[k] = true;
        }
        libssh2_knownhost_free(nh);
    }
}

HostKeyStatus known_hosts_check_status(i32 check, bool revoked) {
    if (revoked) return HOST_KEY_REVOKED;
    switch (check) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        return HOST_KEY_OK;
    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND:
        return HOST_KEY_NEW;
    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
        return HOST_KEY_CHANGED;
    default:
        return HOST_KEY_ERROR;
    }
}

/* =========================================================================
 * Add
 * ========================================================================= */

bool known_hosts_add(LIBSSH2_SESSION *session, const char *hostname, i32 port) {
    if (!session || !hostname || !hostname[0] || strnlen(hostname, 256) >= 256 ||
        port <= 0 || port > 65535) return false;
    char path[4096];
    if (!get_known_hosts_path(path, sizeof(path)) ||
        !known_hosts_path_safe(path, true)) return false;
    size_t key_len;
    int key_type;
    const char *key = libssh2_session_hostkey(session, &key_len, &key_type);
    if (!key) return false;
    return known_hosts_store_add(session, path, hostname, port, key, key_len,
                                 known_hosts_key_typemask(key_type));
}

/* Retire every entry the store holds for hostname:port — whatever key type,
 * plain or hashed — so an accepted key change leaves exactly one identity.
 * Driven through libssh2's own matcher: a MISMATCH answer names the entry
 * that clashed, it is deleted, and the check repeats until nothing for the
 * host is left (MATCH would mean the new key was already there; that entry
 * stays). Returns true when the store no longer contradicts the session's
 * key, including when there was nothing to remove. */
bool known_hosts_forget(LIBSSH2_SESSION *session, const char *hostname, i32 port) {
    if (!session || !hostname || !hostname[0] || strnlen(hostname, 256) >= 256 ||
        port <= 0 || port > 65535) return false;

    char path[4096];
    if (!get_known_hosts_path(path, sizeof(path)) ||
        !known_hosts_path_safe(path, true)) return false;

    size_t key_len;
    int key_type;
    const char *key = libssh2_session_hostkey(session, &key_len, &key_type);
    if (!key) return false;

    return known_hosts_store_forget(session, path, hostname, port, key, key_len);
}

/* =========================================================================
 * Remove (by active session key)
 * ========================================================================= */

bool known_hosts_remove(LIBSSH2_SESSION *session, const char *hostname, i32 port) {
    if (!session || !hostname || !hostname[0] || strnlen(hostname, 256) >= 256 ||
        port <= 0 || port > 65535) return false;

    char path[4096];
    if (!get_known_hosts_path(path, sizeof(path))) return false;
    if (!known_hosts_path_safe(path, false)) return false;

    size_t key_len;
    int key_type;
    const char *key = libssh2_session_hostkey(session, &key_len, &key_type);
    if (!key) return false;

    char name[320];
    known_hosts_entry_name(hostname, port, name, sizeof name);
    i32 dropped = 0;
    return known_hosts_rewrite(session, path, name, key, key_len, KH_DROP_THIS_KEY, NULL, &dropped) &&
           dropped > 0;
}

/* =========================================================================
 * Fingerprint
 * ========================================================================= */

/* "SHA256:<base64, no padding>" — the spelling `ssh-keygen -lf` and every
 * OpenSSH prompt use, so the fingerprint in Liu's dialog can be compared
 * byte for byte with the one the server's administrator published. */
static void fp_sha256_b64(const unsigned char *fp, char *out, usize out_size) {
    char b64[64] = {0};
    i32 n = base64_encode(fp, 32, b64, sizeof b64);
    while (n > 0 && b64[n - 1] == '=') b64[--n] = '\0';
    snprintf(out, out_size, "SHA256:%s", b64);
}

bool known_hosts_fingerprint_sha256(LIBSSH2_SESSION *session, char *out, usize out_size) {
    if (!session || !out) return false;
    const unsigned char *fp = (const unsigned char *)libssh2_hostkey_hash(
        session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!fp || out_size < 72) return false;
    fp_sha256_b64(fp, out, out_size);
    return true;
}

/* Does one revoked_keys line name this session's host key? Three spellings
 * are accepted: "SHA256:<base64>" as ssh-keygen prints it (with or without
 * the "=" padding), "SHA256:<hex>" as earlier Liu versions printed it, and
 * a full public-key line "<type> <base64-key> [comment]" as OpenSSH's own
 * RevokedHostKeys file takes. */
bool known_hosts_revocation_line_matches(const char *line,
                                         const unsigned char *fp_sha256,
                                         const char *key, usize key_len) {
    if (!line || !fp_sha256) return false;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return false;
    if (strncasecmp(line, "SHA256:", 7) == 0) {
        const char *want = line + 7;
        char hex[72], b64[72];
        bin_to_hex(fp_sha256, 32, hex, sizeof hex);
        fp_sha256_b64(fp_sha256, b64, sizeof b64);
        usize wl = strlen(want);
        while (wl > 0 && want[wl - 1] == '=') wl--;
        return strcasecmp(want, hex) == 0 ||
               (wl == strlen(b64 + 7) && strncmp(want, b64 + 7, wl) == 0);
    }
    /* "<type> <base64> …": compare the base64 body against the raw key. */
    if (key && key_len > 0) {
        const char *sp = line + strcspn(line, " \t");
        if (!*sp) return false;
        const char *body = sp + 1;
        while (*body == ' ' || *body == '\t') body++;
        usize bl = 0;
        while (body[bl] && body[bl] != ' ' && body[bl] != '\t') bl++;
        if (bl == 0 || key_len > (usize)0x7FFFFFFF / 2) return false;
        i32 cap = (i32)((key_len + 2) / 3 * 4 + 4);
        char *enc = (char *)malloc((usize)cap);
        if (!enc) return false;
        i32 n = base64_encode((const u8 *)key, (i32)key_len, enc, cap);
        bool match = n > 0 && (usize)n == bl && memcmp(enc, body, bl) == 0;
        free(enc);
        return match;
    }
    return false;
}

/* =========================================================================
 * List all known hosts (parse file directly)
 * ========================================================================= */

/*
 * Parse an OpenSSH known_hosts line.
 * Format: hostname[,hostname2]... key-type base64-key [comment]
 * Or:     [hostname]:port key-type base64-key [comment]
 * Hashed entries start with |1|...
 */
static bool parse_known_hosts_line(const char *line, KnownHostEntry *entry) {
    if (!line || line[0] == '#' || line[0] == '\n' || line[0] == '\0')
        return false;

    /* Skip hashed entries — the hostname cannot be recovered */
    if (line[0] == '|') {
        snprintf(entry->hostname, sizeof(entry->hostname), "(hashed)");
        entry->port = 22;
    } else {
        /* Parse hostname[:port] */
        const char *p = line;
        if (*p == '[') {
            /* [hostname]:port format */
            p++;
            const char *end = strchr(p, ']');
            if (!end) return false;
            usize hlen = (usize)(end - p);
            if (hlen >= sizeof(entry->hostname)) hlen = sizeof(entry->hostname) - 1;
            memcpy(entry->hostname, p, hlen);
            entry->hostname[hlen] = '\0';
            p = end + 1;
            if (*p == ':') {
                p++;
                const char *end_port;
                i32 port;
                if (checked_parse_i32_prefix(p, &port, &end_port) &&
                    port > 0 && port <= 65535 &&
                    (*end_port == '\0' || *end_port == ',' ||
                     isspace((unsigned char)*end_port)))
                    entry->port = port;
                else
                    entry->port = 22;
            } else {
                entry->port = 22;
            }
            /* Skip past port digits */
            while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
        } else {
            /* Plain hostname or hostname,ip format */
            const char *sp = p;
            while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
            usize hlen = (usize)(p - sp);
            if (hlen >= sizeof(entry->hostname)) hlen = sizeof(entry->hostname) - 1;
            memcpy(entry->hostname, sp, hlen);
            entry->hostname[hlen] = '\0';
            entry->port = 22;
        }
    }

    /* Skip past the hostname token, then any whitespace, to reach the key type. */
    const char *p = line;
    while (*p && !isspace((unsigned char)*p)) p++;
    while (*p && isspace((unsigned char)*p)) p++;

    const char *kt_start = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    usize kt_len = (usize)(p - kt_start);
    if (kt_len >= sizeof(entry->key_type)) kt_len = sizeof(entry->key_type) - 1;
    memcpy(entry->key_type, kt_start, kt_len);
    entry->key_type[kt_len] = '\0';

    /* Skip to base64 key and use a portion as a "fingerprint" identifier.
     * The raw key is not at hand to hash here, so the first 32 chars of
     * the base64 key serve as a visual identifier. */
    while (*p && isspace((unsigned char)*p)) p++;
    const char *b64_start = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    usize b64_len = (usize)(p - b64_start);
    if (b64_len > 0) {
        snprintf(entry->fingerprint, sizeof(entry->fingerprint), "B64:%.43s%s",
                 b64_start, b64_len > 43 ? "..." : "");
    } else {
        entry->fingerprint[0] = '\0';
    }

    return entry->key_type[0] != '\0';
}

i32 known_hosts_list(KnownHostEntry *entries, i32 max_entries) {
    if (!entries || max_entries <= 0) return 0;

    char path[4096];
    if (!get_known_hosts_path(path, sizeof(path))) return 0;
    if (!known_hosts_path_safe(path, false)) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[4096];
    i32 count = 0;
    while (count < max_entries && fgets(line, sizeof(line), f)) {
        if (!strchr(line, '\n') && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
            if (ch == EOF) break;
            continue;
        }
        /* Remove trailing newline */
        usize len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

        KnownHostEntry entry = {0};
        if (parse_known_hosts_line(line, &entry)) {
            entries[count++] = entry;
        }
    }

    fclose(f);
    return count;
}

/* =========================================================================
 * Remove a specific host entry by hostname and port
 * ========================================================================= */

bool known_hosts_remove_entry(const char *hostname, i32 port) {
    if (!hostname || !hostname[0] || strnlen(hostname, 256) >= 256 ||
        port < 0 || port > 65535) return false;

    char path[4096];
    if (!get_known_hosts_path(path, sizeof(path))) return false;
    if (!known_hosts_path_safe(path, false)) return false;

    FILE *f = fopen(path, "r");
    if (!f) return false;

    KnownHostLine *lines = NULL;
    i32 line_cap = 0;
    i32 line_count = 0;
    bool found = false;
    bool complete = true;

    char line_buf[4096];
    while (fgets(line_buf, sizeof(line_buf), f)) {
        if (line_count >= line_cap) {
            if (line_cap > INT32_MAX / 2) { complete = false; break; }
            i32 new_cap = line_cap > 0 ? line_cap * 2 : 128;
            usize bytes;
            if (!checked_mul_usize((usize)new_cap, sizeof(*lines), &bytes)) {
                complete = false;
                break;
            }
            KnownHostLine *grown = realloc(lines, bytes);
            if (!grown) { complete = false; break; }
            lines = grown;
            line_cap = new_cap;
        }

        usize raw_len = strlen(line_buf);
        bool had_nl = raw_len > 0 && line_buf[raw_len - 1] == '\n';
        if (!had_nl && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
            complete = false;
            break;
        }
        if (had_nl) line_buf[raw_len - 1] = '\0';

        KnownHostEntry entry = {0};
        bool match = false;
        if (parse_known_hosts_line(line_buf, &entry)) {
            bool host_match = (strcmp(entry.hostname, hostname) == 0);
            bool port_match = (port <= 0 || entry.port == port);
            match = host_match && port_match;
        }

        if (had_nl) line_buf[raw_len - 1] = '\n';
        lines[line_count].line = malloc(raw_len + 1);
        if (!lines[line_count].line) { complete = false; break; }
        memcpy(lines[line_count].line, line_buf, raw_len + 1);
        lines[line_count].keep = !match;
        if (match) found = true;
        line_count++;
    }
    if (ferror(f)) complete = false;
    fclose(f);

    if (!found || !complete) {
        for (i32 i = 0; i < line_count; i++) free(lines[i].line);
        free(lines);
        return false;
    }

    /* Atomic, 0600 replacement: an interrupted rewrite cannot erase the
     * trust database or leave a half-written line. */
    bool replaced = known_hosts_replace(path, lines, line_count);
    for (i32 i = 0; i < line_count; i++) free(lines[i].line);
    free(lines);
    return replaced;
}

/* =========================================================================
 * Remove all known hosts
 * ========================================================================= */

bool known_hosts_remove_all(void) {
    char path[4096];
    if (!get_known_hosts_path(path, sizeof(path))) return false;

    return known_hosts_replace(path, NULL, 0);
}

/* =========================================================================
 * Host key revocation check
 * ========================================================================= */

/*
 * Check if the current session's host key is in the revocation list.
 * The revocation list (~/.ssh/revoked_keys) is a text file with one entry
 * per line: a "SHA256:<base64>" fingerprint as ssh-keygen -lf prints it (the
 * older hex spelling still matches), or a full "<type> <base64-key>" public
 * key line as OpenSSH's RevokedHostKeys takes.
 *
 * Lines starting with '#' are comments. Empty lines are skipped.
 */
bool known_hosts_is_revoked(LIBSSH2_SESSION *session) {
    if (!session) return false;
    char path[4096];
    if (!get_revoked_keys_path(path, sizeof(path))) return false;
    /* The user's own file, or one an administrator put there: skipping a
     * list root deployed would make revocation fail open. */
    struct stat rst;
    if (!known_hosts_dir_safe(path) || lstat(path, &rst) != 0 || !S_ISREG(rst.st_mode) ||
        (rst.st_uid != geteuid() && rst.st_uid != 0) || (rst.st_mode & S_IWOTH)) return false;

    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false; /* No revocation list = not revoked */
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return false; }

    const unsigned char *fp = (const unsigned char *)libssh2_hostkey_hash(
        session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!fp) { fclose(f); return false; }
    size_t key_len = 0;
    int key_type = 0;
    const char *key = libssh2_session_hostkey(session, &key_len, &key_type);

    char line[4096];
    bool revoked = false;
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing whitespace */
        usize len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '  || line[len-1] == '\t')) {
            line[--len] = '\0';
        }

        /* Skip comments and empty lines */
        if (len == 0 || line[0] == '#') continue;

        if (known_hosts_revocation_line_matches(line, fp, key, key ? key_len : 0)) {
            revoked = true;
            break;
        }
    }

    fclose(f);
    return revoked;
}
