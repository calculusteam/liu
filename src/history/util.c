#include "history/util.h"
#include "core/checked.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#ifndef PLATFORM_WIN32
#include <pwd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

char *hist_strdup(Arena *a, const char *s) {
    if (!s) return NULL;
    return hist_strndup(a, s, strlen(s));
}

char *hist_strndup(Arena *a, const char *s, usize n) {
    if (!s) return NULL;
    usize alloc_len;
    if (!checked_add_usize(n, 1, &alloc_len)) return NULL;
    char *d = arena_alloc(a, alloc_len);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

i64 hist_parse_iso8601_ms(const char *iso) {
    if (!iso || !*iso) return 0;
    const char *p = iso;
    i32 parts[6] = {0};
    const i32 widths[6] = {4, 2, 2, 2, 2, 2};
    const char separators[5] = {'-', '-', 'T', ':', ':'};
    for (i32 part = 0; part < 6; part++) {
        for (i32 j = 0; j < widths[part]; j++) {
            if (*p < '0' || *p > '9') return 0;
            parts[part] = parts[part] * 10 + (*p++ - '0');
        }
        if (part < 5) {
            if (part == 2 && *p == ' ') p++; /* ISO also permits a space */
            else if (*p++ != separators[part]) return 0;
        }
    }
    i32 Y = parts[0], M = parts[1], D = parts[2];
    i32 h = parts[3], m = parts[4], s = parts[5], ms = 0;
    if (Y < 1900 || M < 1 || M > 12 || h > 23 || m > 59 || s > 60) return 0;
    static const u8 month_days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    i32 max_day = month_days[M - 1];
    bool leap = (Y % 4 == 0 && Y % 100 != 0) || (Y % 400 == 0);
    if (M == 2 && leap) max_day = 29;
    if (D < 1 || D > max_day) return 0;
    if (*p == '.') {
        p++;
        i32 digits = 0;
        while (digits < 3 && *p >= '0' && *p <= '9') {
            ms = ms * 10 + (*p++ - '0');
            digits++;
        }
        if (digits == 0) return 0;
        while (digits++ < 3) ms *= 10; /* .1 = 100 ms, .12 = 120 ms */
        /* Finer than a millisecond is valid and common — Python's isoformat()
         * writes six digits, Go and Rust nine — and must not make the whole
         * timestamp 0. Cut rather than rounded: .9996 s belongs to this
         * second, and rounding it up would have to carry into the date. */
        while (*p >= '0' && *p <= '9') p++;
    }
    i32 tz_minutes = 0;
    if (*p == 'Z') {
        p++;
    } else if (*p == '+' || *p == '-') {
        /* ±HH:MM, and the two forms ISO 8601 allows without the colon:
         * ±HHMM (what Python's %z prints) and ±HH. */
        bool negative = *p++ == '-';
        if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return 0;
        i32 th = (p[0] - '0') * 10 + (p[1] - '0');
        i32 tm = 0;
        p += 2;
        bool colon = *p == ':';
        if (colon) p++;
        if (colon || *p != '\0') {      /* minutes: required after a colon */
            if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return 0;
            tm = (p[0] - '0') * 10 + (p[1] - '0');
            p += 2;
        }
        if (th > 23 || tm > 59) return 0;
        tz_minutes = (th * 60 + tm) * (negative ? -1 : 1);
    }
    if (*p != '\0') return 0;
    struct tm t = {0};
    t.tm_year = Y - 1900;
    t.tm_mon  = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = s;
#if defined(__APPLE__) || defined(__linux__)
    time_t secs = timegm(&t);
#else
    time_t secs = mktime(&t);
#endif
    if (secs == (time_t)-1) return 0;
    i64 sec64 = (i64)secs;
    sec64 -= (i64)tz_minutes * 60;
    if (sec64 < 0 || (u64)sec64 > (u64)INT64_MAX / 1000u) return 0;
    return sec64 * 1000 + (i64)ms;
}

usize hist_copy_bounded(char *dst, usize cap, const char *src) {
    if (!dst || cap == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }
    usize n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

static const char *resolve_home(void) {
    const char *home = getenv("HOME");
    if (home && *home) return home;
    struct passwd *pw = getpwuid(geteuid());
    return (pw && pw->pw_dir) ? pw->pw_dir : NULL;
}

bool hist_home_path(char *out, usize cap, const char *rel) {
    const char *home = resolve_home();
    if (!home) return false;
    int n = snprintf(out, cap, "%s/%s", home, rel ? rel : "");
    return n > 0 && (usize)n < cap;
}

const char *hist_env_dir(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return NULL;
    return v;
}

bool hist_resolve_root(char *out, usize cap,
                       const char *env_name, const char *env_suffix,
                       const char *home_rel) {
    const char *env = env_name ? hist_env_dir(env_name) : NULL;
    if (env) {
        int n = snprintf(out, cap, "%s/%s", env, env_suffix ? env_suffix : "");
        return n > 0 && (usize)n < cap;
    }
    return hist_home_path(out, cap, home_rel);
}

bool hist_env_or_home_path(char *out, usize cap, const char *env_name,
                           const char *home_def_rel, const char *rel) {
    const char *env = env_name ? hist_env_dir(env_name) : NULL;
    if (env) {
        int n = snprintf(out, cap, "%s/%s", env, rel ? rel : "");
        return n > 0 && (usize)n < cap;
    }
    char joined[1024];
    int n = snprintf(joined, sizeof joined, "%s/%s",
                     home_def_rel ? home_def_rel : "", rel ? rel : "");
    if (n <= 0 || (usize)n >= sizeof joined) return false;
    return hist_home_path(out, cap, joined);
}

bool hist_xdg_data_path(char *out, usize cap, const char *rel) {
    return hist_env_or_home_path(out, cap, "XDG_DATA_HOME", ".local/share", rel);
}

bool hist_xdg_config_path(char *out, usize cap, const char *rel) {
    return hist_env_or_home_path(out, cap, "XDG_CONFIG_HOME", ".config", rel);
}

bool hist_xdg_state_path(char *out, usize cap, const char *rel) {
    return hist_env_or_home_path(out, cap, "XDG_STATE_HOME", ".local/state", rel);
}

bool hist_appdata_path(char *out, usize cap, const char *rel) {
#if defined(__APPLE__)
    char joined[1024];
    int n = snprintf(joined, sizeof joined, "Library/Application Support/%s",
                     rel ? rel : "");
    if (n <= 0 || (usize)n >= sizeof joined) return false;
    return hist_home_path(out, cap, joined);
#else
    return hist_xdg_config_path(out, cap, rel);
#endif
}

bool hist_local_appdata_path(char *out, usize cap, const char *rel) {
#if defined(__APPLE__)
    return hist_appdata_path(out, cap, rel);   /* mac has one app-data root */
#else
    return hist_xdg_data_path(out, cap, rel);
#endif
}

const char *hist_cjson_str(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

bool hist_cjson_bool(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return v && cJSON_IsTrue(v);
}

bool hist_cjson_i64(cJSON *obj, const char *key, i64 *out) {
    if (!obj || !key || !out) return false;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v || !cJSON_IsNumber(v)) return false;
    f64 d = v->valuedouble;
    /* Upper bound is exclusive: INT64_MAX rounds to 2^63 as a double. */
    if (!isfinite(d) || d < -0x1p63 || d >= 0x1p63) return false;
    *out = (i64)d;
    return true;
}

char *hist_cjson_serialize_compact(Arena *a, cJSON *node) {
    if (!node) return NULL;
    char *printed = cJSON_PrintUnformatted(node);
    if (!printed) return NULL;
    char *owned = hist_strdup(a, printed);
    cJSON_free(printed);
    return owned;
}

/* ---------------------------------------------------------------- MD5 ----
 * RFC 1321, straight reference construction. Only here to reproduce Kimi
 * CLI's session-bucket name (see hist_md5_hex in util.h); nothing in Liu
 * uses it for integrity or authentication. Operates on a single in-memory
 * buffer, so there is no streaming context to manage. */

static u32 md5_rotl(u32 x, u32 c) { return (x << c) | (x >> (32 - c)); }

void hist_md5_hex(const void *data, usize len, char out[33]) {
    static const u32 K[64] = {
        0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,
        0xa8304613u,0xfd469501u,0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,
        0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,0xf61e2562u,0xc040b340u,
        0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
        0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,
        0x676f02d9u,0x8d2a4c8au,0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,
        0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,0x289b7ec6u,0xeaa127fau,
        0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
        0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,
        0xffeff47du,0x85845dd1u,0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,
        0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u,
    };
    static const u32 R[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
    };

    u32 h[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    const u8 *p = (const u8 *)data;
    u64 bitlen = (u64)len * 8u;

    /* Whole blocks are read straight out of the caller's buffer; only the
     * remainder is copied, padded (0x80, zeros to 56 mod 64, little-endian bit
     * length) and processed as one or two extra blocks. */
    usize full = len / 64;
    usize rem  = len % 64;

    u8 tail[128];
    memcpy(tail, p + full * 64, rem);
    tail[rem] = 0x80;
    usize padded = (rem + 1 <= 56) ? 56 : 120;
    memset(tail + rem + 1, 0, padded - (rem + 1));
    for (i32 i = 0; i < 8; i++)
        tail[padded + i] = (u8)((bitlen >> (8 * i)) & 0xFF);
    usize tail_blocks = (padded + 8) / 64;      /* 1 or 2 */

    for (usize bi = 0; bi < full + tail_blocks; bi++) {
        const u8 *blk = (bi < full) ? (p + bi * 64) : (tail + (bi - full) * 64);

        u32 m[16];
        for (i32 i = 0; i < 16; i++)
            m[i] = (u32)blk[i * 4] | ((u32)blk[i * 4 + 1] << 8) |
                   ((u32)blk[i * 4 + 2] << 16) | ((u32)blk[i * 4 + 3] << 24);

        u32 a = h[0], b = h[1], c = h[2], d = h[3];
        for (i32 i = 0; i < 64; i++) {
            u32 f; i32 g;
            if (i < 16)      { f = (b & c) | (~b & d);          g = i; }
            else if (i < 32) { f = (d & b) | (~d & c);          g = (5 * i + 1) & 15; }
            else if (i < 48) { f = b ^ c ^ d;                   g = (3 * i + 5) & 15; }
            else             { f = c ^ (b | ~d);                g = (7 * i) & 15; }
            u32 tmp = d;
            d = c; c = b;
            b = b + md5_rotl(a + f + K[i] + m[g], R[i]);
            a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }

    static const char hexd[] = "0123456789abcdef";
    for (i32 i = 0; i < 4; i++) {
        for (i32 j = 0; j < 4; j++) {
            u8 byte = (u8)((h[i] >> (8 * j)) & 0xFF);
            out[i * 4 * 2 + j * 2]     = hexd[byte >> 4];
            out[i * 4 * 2 + j * 2 + 1] = hexd[byte & 0x0F];
        }
    }
    out[32] = '\0';
}

bool hist_slugify_lower(char *out, usize cap, const char *path) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!path) return false;

    usize n = 0;
    bool pending_dash = false;   /* collapse runs; also drops the leading dash */
    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        /* ASCII-only on purpose: the JS regex /[^a-z0-9]+/ runs *after*
         * toLowerCase(), and a UTF-8 lead byte is never in [a-z0-9], so every
         * non-ASCII rune collapses into the same single dash either way. */
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!keep) { if (n > 0) pending_dash = true; continue; }
        if (pending_dash) {
            if (n + 1 >= cap) return false;   /* truncation would forge a slug */
            out[n++] = '-';
            pending_dash = false;
        }
        if (n + 1 >= cap) return false;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    if (n == 0) {
        if (cap < sizeof "root") return false;
        memcpy(out, "root", sizeof "root");
    }
    return true;
}

bool hist_slugify_claude(char *out, usize cap, const char *path) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!path) return false;
    usize n = 0;
    const unsigned char *p = (const unsigned char *)path;
    while (*p) {
        /* Code units of this character: ASCII and 2/3-byte sequences are one
         * UTF-16 unit, a 4-byte one is a surrogate pair. A byte that does not
         * start a valid sequence is one unit by itself. */
        i32 len = 1, units = 1;
        if (*p >= 0xF0 && *p <= 0xF4 && p[1] && p[2] && p[3] &&
            (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            len = 4; units = 2;
        } else if (*p >= 0xE0 && *p <= 0xEF && p[1] && p[2] &&
                   (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            len = 3;
        } else if (*p >= 0xC2 && *p <= 0xDF && p[1] && (p[1] & 0xC0) == 0x80) {
            len = 2;
        }
        bool alnum = len == 1 && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                                  (*p >= '0' && *p <= '9'));
        for (i32 u = 0; u < units; u++) {
            if (n + 1 >= cap) return false;
            out[n++] = alnum ? (char)*p : '-';
        }
        p += len;
    }
    out[n] = '\0';
    return true;
}

/* Shared core. `a == NULL` allocates with malloc (caller frees), otherwise the
 * bytes come out of the arena and live as long as it does. */
static bool slurp_into(Arena *a, const char *path, usize max_bytes,
                       char **out_buf, usize *out_len) {
    if (!path || !out_buf) return false;
    *out_buf = NULL;
    if (out_len) *out_len = 0;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd); return false;
    }
    if ((usize)st.st_size > max_bytes) { close(fd); return false; }

    usize len = (usize)st.st_size;
    usize alloc_len;
    if (!checked_add_usize(len, 1, &alloc_len)) { close(fd); return false; }
    char *buf = a ? (char *)arena_alloc(a, alloc_len) : (char *)malloc(alloc_len);
    if (!buf) { close(fd); return false; }

    usize off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd); if (!a) free(buf); return false;
        }
        if (n == 0) { close(fd); if (!a) free(buf); return false; }  /* premature EOF */
        off += (usize)n;
    }
    close(fd);
    buf[off] = '\0';
    *out_buf = buf;
    if (out_len) *out_len = off;
    return true;
}

bool hist_slurp_file(Arena *a, const char *path, usize max_bytes,
                     char **out_buf, usize *out_len) {
    if (!a) return false;
    return slurp_into(a, path, max_bytes, out_buf, out_len);
}

bool hist_slurp_file_heap(const char *path, usize max_bytes,
                          char **out_buf, usize *out_len) {
    return slurp_into(NULL, path, max_bytes, out_buf, out_len);
}

/* ---------------------------------------------------------------------------
 * Bounded line reader — see history/util.h.
 *
 * The reader owns its raw buffer so the hot loop is memchr + memcpy: a
 * getc_unlocked loop over stdio is correct and simple but far slower, and
 * this keeps the cap from costing throughput against getline().
 */
#define HIST_CHUNK_SIZE   ((usize)(64u * 1024u))
#define HIST_LINE_MIN_CAP ((usize)512u)

bool hist_line_reader_attach(HistLineReader *r, int fd) {
    if (!r || fd < 0) return false;
    memset(r, 0, sizeof *r);
    r->chunk = (char *)malloc(HIST_CHUNK_SIZE);
    if (!r->chunk) { r->fd = -1; return false; }
    r->fd = fd;
    return true;
}

FILE *hist_fopen_regular(const char *path, bool nofollow) {
    if (!path || !path[0]) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | (nofollow ? O_NOFOLLOW : 0));
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    FILE *f = fdopen(fd, "rb");
    if (!f) close(fd);
    return f;
}

bool hist_line_reader_open(HistLineReader *r, const char *path, bool nofollow) {
    if (r) { memset(r, 0, sizeof *r); r->fd = -1; }
    if (!r || !path) return false;
    int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | (nofollow ? O_NOFOLLOW : 0);
    int fd = open(path, flags);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return false; }
    if (!hist_line_reader_attach(r, fd)) { close(fd); return false; }
    return true;
}

void hist_line_reader_close(HistLineReader *r) {
    if (!r) return;
    if (r->fd >= 0) close(r->fd);
    free(r->chunk);
    free(r->line);
    memset(r, 0, sizeof *r);
    r->fd = -1;
}

/* Refill when the chunk is drained. false = nothing more (eof or error set). */
static bool hist_line_refill(HistLineReader *r) {
    if (r->chunk_pos < r->chunk_len) return true;
    if (r->eof || r->error) return false;
    ssize_t n;
    do { n = read(r->fd, r->chunk, HIST_CHUNK_SIZE); } while (n < 0 && errno == EINTR);
    if (n < 0) { r->error = true; return false; }
    if (n == 0) { r->eof = true; return false; }
    r->chunk_pos = 0;
    r->chunk_len = (usize)n;
    return true;
}

/* Ensure the line buffer holds `need` bytes, never exceeding `hard_cap`. */
static bool hist_line_grow(HistLineReader *r, usize need, usize hard_cap) {
    if (need <= r->line_cap) return true;
    usize want = r->line_cap ? (r->line_cap > SIZE_MAX / 2 ? SIZE_MAX
                                                           : r->line_cap * 2)
                             : HIST_LINE_MIN_CAP;
    if (want < need) want = need;
    if (want > hard_cap) want = hard_cap;
    char *nb = (char *)realloc(r->line, want);
    if (!nb) return false;
    r->line = nb;
    r->line_cap = want;
    return true;
}

HistLineResult hist_line_reader_next(HistLineReader *r, usize max_bytes,
                                     const char **out_line, usize *out_len) {
    if (out_line) *out_line = NULL;
    if (out_len)  *out_len  = 0;
    if (!r || r->fd < 0) return HIST_LINE_IO_ERROR;
    if (r->error) return HIST_LINE_IO_ERROR;
    if (max_bytes == 0) max_bytes = 1;
    if (max_bytes > SIZE_MAX - 1) max_bytes = SIZE_MAX - 1;
    const usize hard_cap = max_bytes + 1;

    usize len = 0;
    bool overlong = false, saw_any = false;

    for (;;) {
        if (!hist_line_refill(r)) {
            if (r->error) return HIST_LINE_IO_ERROR;
            break;                                     /* EOF */
        }
        const char *p    = r->chunk + r->chunk_pos;
        usize      avail = r->chunk_len - r->chunk_pos;
        const char *nl   = (const char *)memchr(p, '\n', avail);
        usize      seg   = nl ? (usize)(nl - p) : avail;  /* bytes before '\n' */
        saw_any = true;

        if (!overlong) {
            usize room = max_bytes - len;
            usize take = seg < room ? seg : room;
            if (take) {
                if (!hist_line_grow(r, len + take + 1, hard_cap))
                    return HIST_LINE_IO_ERROR;
                memcpy(r->line + len, p, take);
                len += take;
            }
            if (seg > take) overlong = true;    /* the rest of the line is dropped */
        }
        r->chunk_pos += seg + (nl ? 1u : 0u);
        if (nl) break;
    }

    if (overlong) return HIST_LINE_TOO_LONG;
    if (!saw_any) return HIST_LINE_EOF;

    /* An empty line still needs a valid, NUL-terminated pointer to hand back. */
    if (!hist_line_grow(r, len + 1, hard_cap)) return HIST_LINE_IO_ERROR;
    if (len > 0 && r->line[len - 1] == '\r') len--;
    r->line[len] = '\0';
    if (out_line) *out_line = r->line;
    if (out_len)  *out_len  = len;
    return HIST_LINE_OK;
}
