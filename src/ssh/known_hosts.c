/*
 * Liu - SSH known_hosts management
 * Verify server host keys against ~/.ssh/known_hosts.
 * List, remove, and manage known host entries.
 * Check host key revocation against ~/.ssh/revoked_keys.
 */
#include "ssh/known_hosts.h"
#include <libssh2.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void ssh_home_path(char *path, usize path_size, const char *rel) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, path_size, "%s/%s", home, rel);
}

static void get_known_hosts_path(char *path, usize path_size) {
    ssh_home_path(path, path_size, ".ssh/known_hosts");
}

static void get_revoked_keys_path(char *path, usize path_size) {
    ssh_home_path(path, path_size, ".ssh/revoked_keys");
}

/* Convert raw binary to hex string */
static void bin_to_hex(const u8 *data, usize len, char *out, usize out_size) {
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

HostKeyStatus known_hosts_verify(LIBSSH2_SESSION *session, const char *hostname, i32 port) {
    LIBSSH2_KNOWNHOSTS *nh = libssh2_knownhost_init(session);
    if (!nh) return HOST_KEY_ERROR;

    char path[512];
    get_known_hosts_path(path, sizeof(path));
    libssh2_knownhost_readfile(nh, path, LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    /* Get remote host key */
    size_t key_len;
    int key_type;
    const char *key = libssh2_session_hostkey(session, &key_len, &key_type);
    if (!key) {
        libssh2_knownhost_free(nh);
        return HOST_KEY_ERROR;
    }

    /* Check against known hosts */
    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    struct libssh2_knownhost *host = NULL;

    int check = libssh2_knownhost_checkp(nh, hostname, port, key, key_len, typemask, &host);

    HostKeyStatus status;
    switch (check) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        status = HOST_KEY_OK;
        break;
    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND:
        status = HOST_KEY_NEW;
        break;
    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
        status = HOST_KEY_CHANGED;
        break;
    default:
        status = HOST_KEY_ERROR;
        break;
    }

    libssh2_knownhost_free(nh);

    /* If the key matched or is new, also check the revocation list */
    if (status == HOST_KEY_OK || status == HOST_KEY_NEW) {
        if (known_hosts_is_revoked(session)) {
            return HOST_KEY_REVOKED;
        }
    }

    return status;
}

/* =========================================================================
 * Add
 * ========================================================================= */

bool known_hosts_add(LIBSSH2_SESSION *session, const char *hostname, i32 port) {
    LIBSSH2_KNOWNHOSTS *nh = libssh2_knownhost_init(session);
    if (!nh) return false;

    char path[512];
    get_known_hosts_path(path, sizeof(path));
    libssh2_knownhost_readfile(nh, path, LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    size_t key_len;
    int key_type;
    const char *key = libssh2_session_hostkey(session, &key_len, &key_type);
    if (!key) {
        libssh2_knownhost_free(nh);
        return false;
    }

    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    if (key_type == LIBSSH2_HOSTKEY_TYPE_RSA)
        typemask |= LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    else if (key_type == LIBSSH2_HOSTKEY_TYPE_DSS)
        typemask |= LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    else
        typemask |= LIBSSH2_KNOWNHOST_KEY_UNKNOWN;

    /* OpenSSH encodes non-standard ports as "[host]:port" so two servers
     * sharing a hostname on different ports are tracked as distinct
     * identities. Port 22 keeps the plain hostname for compatibility. */
    char host_key_name[320];
    if (port > 0 && port != 22) {
        snprintf(host_key_name, sizeof(host_key_name), "[%s]:%d", hostname, port);
    } else {
        snprintf(host_key_name, sizeof(host_key_name), "%s", hostname);
    }
    int rc = libssh2_knownhost_addc(nh, host_key_name, NULL, key, key_len,
                                     NULL, 0, typemask, NULL);
    if (rc == 0) {
        libssh2_knownhost_writefile(nh, path, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    }

    libssh2_knownhost_free(nh);
    return rc == 0;
}

/* =========================================================================
 * Remove (by active session key)
 * ========================================================================= */

bool known_hosts_remove(LIBSSH2_SESSION *session, const char *hostname, i32 port) {
    LIBSSH2_KNOWNHOSTS *nh = libssh2_knownhost_init(session);
    if (!nh) return false;

    char path[512];
    get_known_hosts_path(path, sizeof(path));
    libssh2_knownhost_readfile(nh, path, LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    size_t key_len;
    int key_type;
    const char *key = libssh2_session_hostkey(session, &key_len, &key_type);
    if (!key) {
        libssh2_knownhost_free(nh);
        return false;
    }

    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    struct libssh2_knownhost *host = NULL;

    int check = libssh2_knownhost_checkp(nh, hostname, port, key, key_len, typemask, &host);
    if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH && host) {
        libssh2_knownhost_del(nh, host);
        libssh2_knownhost_writefile(nh, path, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        libssh2_knownhost_free(nh);
        return true;
    }

    libssh2_knownhost_free(nh);
    return false;
}

/* =========================================================================
 * Fingerprint
 * ========================================================================= */

bool known_hosts_fingerprint_sha256(LIBSSH2_SESSION *session, char *out, usize out_size) {
    const unsigned char *fp = (const unsigned char *)libssh2_hostkey_hash(
        session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!fp || out_size < 72) return false;

    snprintf(out, out_size, "SHA256:");
    usize p = strlen(out);
    bin_to_hex(fp, 32, out + p, out_size - p);
    return true;
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

    /* Skip hashed entries — we cannot recover the hostname */
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
                entry->port = atoi(p);
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
     * We don't have the raw key to hash here, so we use the first 32 chars
     * of the base64 key as a visual identifier. */
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

    char path[512];
    get_known_hosts_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[4096];
    i32 count = 0;
    while (count < max_entries && fgets(line, sizeof(line), f)) {
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
    if (!hostname || !hostname[0]) return false;

    char path[512];
    get_known_hosts_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return false;

    /* Read all lines into a heap vector to avoid multi-megabyte stack frames. */
    typedef struct {
        char *line;
        bool  keep;
    } KnownHostLine;

    KnownHostLine *lines = NULL;
    i32 line_cap = 0;
    i32 line_count = 0;
    bool found = false;

    char line_buf[4096];
    while (fgets(line_buf, sizeof(line_buf), f)) {
        if (line_count >= line_cap) {
            i32 new_cap = line_cap > 0 ? line_cap * 2 : 128;
            KnownHostLine *grown = realloc(lines, (usize)new_cap * sizeof(*grown));
            if (!grown) break;
            lines = grown;
            line_cap = new_cap;
        }

        usize raw_len = strlen(line_buf);
        bool had_nl = raw_len > 0 && line_buf[raw_len - 1] == '\n';
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
        if (!lines[line_count].line) break;
        memcpy(lines[line_count].line, line_buf, raw_len + 1);
        lines[line_count].keep = !match;
        if (match) found = true;
        line_count++;
    }
    fclose(f);

    if (!found) {
        for (i32 i = 0; i < line_count; i++) free(lines[i].line);
        free(lines);
        return false;
    }

    /* Rewrite the file without the removed entries */
    f = fopen(path, "w");
    if (!f) {
        for (i32 i = 0; i < line_count; i++) free(lines[i].line);
        free(lines);
        return false;
    }

    for (i32 i = 0; i < line_count; i++) {
        if (lines[i].keep) {
            fputs(lines[i].line, f);
        }
    }
    fclose(f);
    for (i32 i = 0; i < line_count; i++) free(lines[i].line);
    free(lines);
    return true;
}

/* =========================================================================
 * Remove all known hosts
 * ========================================================================= */

bool known_hosts_remove_all(void) {
    char path[512];
    get_known_hosts_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) return false;
    fclose(f);
    return true;
}

/* =========================================================================
 * Host key revocation check
 * ========================================================================= */

/*
 * Check if the current session's host key is in the revocation list.
 * The revocation list (~/.ssh/revoked_keys) is a simple text file with
 * one SHA256 fingerprint per line (in the same format as our fingerprint
 * output: "SHA256:<hex>").
 *
 * Lines starting with '#' are comments. Empty lines are skipped.
 */
bool known_hosts_is_revoked(LIBSSH2_SESSION *session) {
    char path[512];
    get_revoked_keys_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return false; /* No revocation list = not revoked */

    /* Get current session fingerprint */
    char fp[128] = {0};
    if (!known_hosts_fingerprint_sha256(session, fp, sizeof(fp))) {
        fclose(f);
        return false;
    }

    char line[256];
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

        /* Compare fingerprints (case-insensitive) */
        if (strcasecmp(line, fp) == 0) {
            revoked = true;
            break;
        }
    }

    fclose(f);
    return revoked;
}
