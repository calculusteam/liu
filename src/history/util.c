#include "history/util.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
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
    char *d = arena_alloc(a, n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

i64 hist_parse_iso8601_ms(const char *iso) {
    if (!iso || !*iso) return 0;
    int Y, M, D, h, m, s, ms = 0;
    int scanned = sscanf(iso, "%d-%d-%dT%d:%d:%d.%d",
                         &Y, &M, &D, &h, &m, &s, &ms);
    if (scanned < 6) return 0;
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
    return (i64)secs * 1000 + (i64)ms;
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

char *hist_cjson_serialize_compact(Arena *a, cJSON *node) {
    if (!node) return NULL;
    char *printed = cJSON_PrintUnformatted(node);
    if (!printed) return NULL;
    char *owned = hist_strdup(a, printed);
    cJSON_free(printed);
    return owned;
}

bool hist_slurp_file(Arena *a, const char *path, usize max_bytes,
                     char **out_buf, usize *out_len) {
    if (!a || !path || !out_buf) return false;
    *out_buf = NULL;
    if (out_len) *out_len = 0;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd); return false;
    }
    if ((usize)st.st_size > max_bytes) { close(fd); return false; }

    usize len = (usize)st.st_size;
    char *buf = arena_alloc(a, len + 1);
    if (!buf) { close(fd); return false; }

    usize off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd); return false;
        }
        if (n == 0) { close(fd); return false; }  /* premature EOF / truncation */
        off += (usize)n;
    }
    close(fd);
    buf[off] = '\0';
    *out_buf = buf;
    if (out_len) *out_len = off;
    return true;
}
