/*
 * liu-notify - config loader.
 * Line-based key=value parsing with strict length caps and ASCII trim.
 * No allocator use (all on-stack / into caller's struct).
 */
#include "notify/config.h"
#include "notify/protocol.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#ifndef PLATFORM_WIN32
#include <pwd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp — glibc declares it here, not in string.h */
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static void apply_sound_rule(NotifyConfig *cfg, u8 tool, u8 event, const char *path);

void notify_config_defaults(NotifyConfig *cfg) {
    if (!cfg) return;
    cfg->enabled               = true;
    cfg->voice[0]              = '\0';
    cfg->rate                  = 1.0f;
    cfg->desktop_notification  = false;
    /* TTS fallback defaults to off. Bundled sound rules provide
     * notifications by default; configuration can explicitly enable speech.
     */
    cfg->tts_fallback          = false;
    cfg->rate_limit_per_sec    = 0;
    cfg->rate_burst            = 0;
    cfg->dedup_window_sec      = 0.0f;
    cfg->sound_count           = 0;
    cfg->sounds_configured     = false;
    memset(cfg->sounds, 0, sizeof cfg->sounds);
    memset(cfg->event_banner, 0, sizeof cfg->event_banner);
    /* Notify events are interrupt-class — needs user attention — so a
     * banner is the sensible default. Other events stay sound-only. */
    cfg->event_banner[EVT_NOTIFY] = true;
    /* Notification events are enabled by default; individual events can be
     * disabled in Settings. */
    for (usize e = 0; e < sizeof cfg->event_enabled / sizeof cfg->event_enabled[0]; e++)
        cfg->event_enabled[e] = true;
    cfg->tool_enabled.claude   = true;
    cfg->tool_enabled.copilot  = true;
    cfg->tool_enabled.codex    = true;
    cfg->tool_enabled.custom   = true;
}

/* Resolve a bundled sound shipped under assets/sounds/<name>. Mirrors the
 * Settings UI resolver so the daemon can seed the same defaults without the
 * GUI ever being opened. Returns false (and clears `out`) when the file
 * can't be located — callers then skip that rule rather than store a bad
 * path that would silently fall through to TTS. */
bool notify_bundled_sound_path(const char *name, char *out, usize cap) {
    if (!name || !*name || !out || cap < 4) return false;
    out[0] = '\0';
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) return false;
    char candidate[1024];
    candidate[0] = '\0';
    struct stat st;
#if defined(__APPLE__)
    /* Bundle: Contents/MacOS/Liu (or liu-notify) → Contents/assets/sounds/. */
    char exe[1024];
    u32 exe_sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &exe_sz) == 0) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            int n = snprintf(candidate, sizeof candidate,
                             "%s/../assets/sounds/%s", exe, name);
            if (n <= 0 || (usize)n >= sizeof candidate ||
                stat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
                candidate[0] = '\0';
        }
    }
#elif defined(__linux__)
    /* The Settings resolver's layouts (ui/settings.c), relative to the binary:
     * a tarball keeps assets/ beside it, install.sh puts the binary in
     * <prefix>/bin and assets/ in <prefix>, an FHS install puts the data in
     * <prefix>/share/liu/assets. A Release build does not define
     * LIU_ASSETS_DIR, so this branch is how it finds the shipped sounds. */
    {
        char exe[1024];
        ssize_t elen = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (elen > 0) {
            exe[elen] = '\0';
            char *slash = strrchr(exe, '/');
            if (slash) {
                *slash = '\0';
                static const char *const layouts[] = {
                    "%s/assets/sounds/%s",
                    "%s/../assets/sounds/%s",
                    "%s/../share/liu/assets/sounds/%s",
                };
                for (usize i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
                    int n = snprintf(candidate, sizeof candidate, layouts[i],
                                     exe, name);
                    if (n > 0 && (usize)n < sizeof candidate &&
                        stat(candidate, &st) == 0 && S_ISREG(st.st_mode))
                        break;
                    candidate[0] = '\0';
                }
            }
        }
    }
#endif
#ifdef LIU_ASSETS_DIR
    if (!candidate[0]) {
        int n = snprintf(candidate, sizeof candidate,
                         "%s/sounds/%s", LIU_ASSETS_DIR, name);
        if (n <= 0 || (usize)n >= sizeof candidate ||
            stat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
            candidate[0] = '\0';
    }
#endif
    if (!candidate[0]) return false;
    char resolved[PATH_MAX];
    if (realpath(candidate, resolved) && strlen(resolved) < cap) {
        snprintf(out, cap, "%s", resolved);
        return true;
    }
    if (strlen(candidate) < cap) { snprintf(out, cap, "%s", candidate); return true; }
    return false;
}

/* The four sounds Liu ships, one per event. */
static const struct { const char *file; u8 event; } kDefs[] = {
    { "complete.wav", EVT_COMPLETE },
    { "notify.wav",   EVT_NOTIFY   },
    { "error.wav",    EVT_ERROR    },
    { "stop.wav",     EVT_STOP     },
};

/* Seed the four bundled WAV rules, one per event for any tool. The operation
 * is idempotent: replacing an exact `(tool,event)` match leaves rules for
 * other tuples unchanged. Skip missing sound files. */
void notify_config_seed_bundled_sounds(NotifyConfig *cfg) {
    if (!cfg) return;
    char path[1024];
    for (usize i = 0; i < sizeof kDefs / sizeof kDefs[0]; i++) {
        if (notify_bundled_sound_path(kDefs[i].file, path, sizeof path))
            apply_sound_rule(cfg, NOTIFY_MATCH_ANY, kDefs[i].event, path);
    }
}

static bool resolve_config_path(char *out, size_t out_sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/') {
        int n = snprintf(out, out_sz, "%s/liu/notify.conf", xdg);
        return n > 0 && (size_t)n < out_sz;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(geteuid());
        if (!pw || !pw->pw_dir) return false;
        home = pw->pw_dir;
    }
    int n = snprintf(out, out_sz, "%s/.config/liu/notify.conf", home);
    return n > 0 && (size_t)n < out_sz;
}

static char *trim_ascii(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
    return s;
}

static bool parse_bool(const char *val, bool *out) {
    if (!strcasecmp(val, "true")  || !strcmp(val, "1") || !strcasecmp(val, "yes") ||
        !strcasecmp(val, "on"))  { *out = true;  return true; }
    if (!strcasecmp(val, "false") || !strcmp(val, "0") || !strcasecmp(val, "no") ||
        !strcasecmp(val, "off")) { *out = false; return true; }
    return false;
}

static bool parse_u32(const char *val, u32 *out) {
    if (!val || !out || *val == '-') return false;
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(val, &end, 10);
    if (errno || end == val || *end != '\0' || v > 0xffffffffUL) return false;
    *out = (u32)v;
    return true;
}

static bool parse_f32(const char *val, f32 *out) {
    if (!val || !out) return false;
    char *end = NULL;
    errno = 0;
    double v = strtod(val, &end);
    if (errno || end == val || *end != '\0' || !isfinite(v) ||
        v < -(double)FLT_MAX || v > (double)FLT_MAX) return false;
    *out = (f32)v;
    return true;
}

/* Resolve the "claude"/"codex"/... token to a NotifyTool id, or
 * NOTIFY_MATCH_ANY if the token is empty/"any"/"default". Returns -1 when
 * the token doesn't match any known tool. */
static int parse_tool_token(const char *s) {
    if (!s || !*s || !strcasecmp(s, "any") || !strcasecmp(s, "default")) return NOTIFY_MATCH_ANY;
    if (!strcasecmp(s, "claude"))  return TOOL_CLAUDE;
    if (!strcasecmp(s, "copilot")) return TOOL_COPILOT;
    if (!strcasecmp(s, "codex"))   return TOOL_CODEX;
    if (!strcasecmp(s, "custom"))  return TOOL_CUSTOM;
    return -1;
}

static int parse_event_token(const char *s) {
    if (!s || !*s || !strcasecmp(s, "any") || !strcasecmp(s, "default")) return NOTIFY_MATCH_ANY;
    if (!strcasecmp(s, "start"))    return EVT_START;
    if (!strcasecmp(s, "stop"))     return EVT_STOP;
    if (!strcasecmp(s, "notify"))   return EVT_NOTIFY;
    if (!strcasecmp(s, "error"))    return EVT_ERROR;
    if (!strcasecmp(s, "complete")) return EVT_COMPLETE;
    return -1;
}

/* Append or replace a sound rule. Exact-duplicate selectors are overwritten
 * so a later config line can override an earlier one. */
static void apply_sound_rule(NotifyConfig *cfg, u8 tool, u8 event, const char *path) {
    if (!cfg || !path || !*path || strchr(path, '\n') || strchr(path, '\r')) return;
    if (strlen(path) >= NOTIFY_SOUND_PATH_CAP) {
        /* Reported on stderr: a rule dropped silently would leave a chosen
         * sound not playing with nothing anywhere to explain it. */
        fprintf(stderr, "liu-notify: sound path too long (%zu bytes, max %d), "
                        "rule ignored: %.60s\n",
                strlen(path), NOTIFY_SOUND_PATH_CAP - 1, path);
        return;
    }
    if (cfg->sound_count > NOTIFY_SOUND_RULE_CAP)
        cfg->sound_count = NOTIFY_SOUND_RULE_CAP;
    for (u32 i = 0; i < cfg->sound_count; i++) {
        if (cfg->sounds[i].tool_id == tool && cfg->sounds[i].event_id == event) {
            snprintf(cfg->sounds[i].path, NOTIFY_SOUND_PATH_CAP, "%s", path);
            return;
        }
    }
    if (cfg->sound_count >= NOTIFY_SOUND_RULE_CAP) return;
    NotifySoundRule *r = &cfg->sounds[cfg->sound_count++];
    r->tool_id  = tool;
    r->event_id = event;
    snprintf(r->path, NOTIFY_SOUND_PATH_CAP, "%s", path);
}

void notify_config_apply_sound_rule(NotifyConfig *cfg, u8 tool_id, u8 event_id,
                                    const char *path) {
    if (!cfg || !path || !*path) return;
    apply_sound_rule(cfg, tool_id, event_id, path);
}

void notify_config_remove_sound_rule(NotifyConfig *cfg, u8 tool_id, u8 event_id) {
    if (!cfg) return;
    if (cfg->sound_count > NOTIFY_SOUND_RULE_CAP)
        cfg->sound_count = NOTIFY_SOUND_RULE_CAP;
    for (u32 i = 0; i < cfg->sound_count; i++) {
        if (cfg->sounds[i].tool_id == tool_id && cfg->sounds[i].event_id == event_id) {
            /* Swap-and-shrink — order isn't load-bearing. */
            cfg->sounds[i] = cfg->sounds[cfg->sound_count - 1];
            memset(&cfg->sounds[cfg->sound_count - 1], 0, sizeof(NotifySoundRule));
            cfg->sound_count--;
            return;
        }
    }
}

/* Parse a key starting with "sound" — forms accepted:
 *   sound                     → (any, any)
 *   sound.<tool>              → (tool, any)
 *   sound.<event>             → (any, event)
 *   sound.<tool>.<event>      → (tool, event)
 * Tool and event tokens share the enum namespaces above and never collide. */
static bool parse_sound_key(const char *key, u8 *tool_out, u8 *event_out) {
    if (strcmp(key, "sound") == 0) {
        *tool_out = NOTIFY_MATCH_ANY; *event_out = NOTIFY_MATCH_ANY; return true;
    }
    if (strncmp(key, "sound.", 6) != 0) return false;

    const char *rest = key + 6;
    const char *dot  = strchr(rest, '.');
    char first[32];  size_t flen = dot ? (size_t)(dot - rest) : strlen(rest);
    if (flen >= sizeof first) return false;
    memcpy(first, rest, flen); first[flen] = '\0';

    int t = parse_tool_token(first);
    int e = parse_event_token(first);
    if (!dot) {
        if (t >= 0) { *tool_out = (u8)t; *event_out = NOTIFY_MATCH_ANY; return true; }
        if (e >= 0) { *tool_out = NOTIFY_MATCH_ANY; *event_out = (u8)e; return true; }
        return false;
    }
    int e2 = parse_event_token(dot + 1);
    if (t < 0 || e2 < 0) return false;
    *tool_out = (u8)t; *event_out = (u8)e2;
    return true;
}

static void apply_kv(NotifyConfig *cfg, const char *key, const char *val, int lineno) {
    bool  b; u32 u; f32 f;

    if      (!strcmp(key, "enabled")              && parse_bool(val, &b)) cfg->enabled = b;
    else if (!strcmp(key, "voice")) {
        snprintf(cfg->voice, sizeof cfg->voice, "%s", val);
    }
    else if (!strcmp(key, "rate")                 && parse_f32 (val, &f)) {
        if (f < 0.5f) f = 0.5f; if (f > 2.0f) f = 2.0f;
        cfg->rate = f;
    }
    else if (!strcmp(key, "desktop_notification") && parse_bool(val, &b)) {
        /* The desktop_notification setting initializes every
         * event_banner entry. Per-event banner settings applied later
         * in file order override that value. Keep the mirror field
         * synchronized for readers of desktop_notification. */
        cfg->desktop_notification = b;
        for (usize e = 0;
             e < sizeof cfg->event_banner / sizeof cfg->event_banner[0]; e++) {
            cfg->event_banner[e] = b;
        }
    }
    else if (!strcmp(key, "tts_fallback")         && parse_bool(val, &b)) cfg->tts_fallback = b;
    else if (!strcmp(key, "sounds_configured")    && parse_bool(val, &b)) cfg->sounds_configured = b;
    else if (!strcmp(key, "rate_limit_per_sec")   && parse_u32 (val, &u)) cfg->rate_limit_per_sec = u;
    else if (!strcmp(key, "rate_burst")           && parse_u32 (val, &u)) cfg->rate_burst = u;
    else if (!strcmp(key, "dedup_window_sec")     && parse_f32 (val, &f)) cfg->dedup_window_sec = f;
    else if (!strcmp(key, "tool.claude.enabled")  && parse_bool(val, &b)) cfg->tool_enabled.claude  = b;
    else if (!strcmp(key, "tool.copilot.enabled") && parse_bool(val, &b)) cfg->tool_enabled.copilot = b;
    else if (!strcmp(key, "tool.codex.enabled")   && parse_bool(val, &b)) cfg->tool_enabled.codex   = b;
    else if (!strcmp(key, "tool.custom.enabled")  && parse_bool(val, &b)) cfg->tool_enabled.custom  = b;
    else if (!strncmp(key, "banner.", 7) && parse_bool(val, &b)) {
        /* Per-event banner toggle. Reuses the event-name parser so the
         * accepted tokens stay in sync with sound.<event> keys. */
        int e = parse_event_token(key + 7);
        if (e >= 0 && e < (int)(sizeof cfg->event_banner / sizeof cfg->event_banner[0]))
            cfg->event_banner[e] = b;
    }
    else if (!strncmp(key, "enabled.", 8) && parse_bool(val, &b)) {
        /* Per-event master on/off (Settings "On/Off" pill). */
        int e = parse_event_token(key + 8);
        if (e >= 0 && e < (int)(sizeof cfg->event_enabled / sizeof cfg->event_enabled[0]))
            cfg->event_enabled[e] = b;
    }
    else {
        u8 tool, event;
        if (parse_sound_key(key, &tool, &event)) {
            apply_sound_rule(cfg, tool, event, val);
            return;
        }
        fprintf(stderr, "liu-notify: notify.conf:%d: unknown key '%s'\n", lineno, key);
    }
}

bool notify_config_load(NotifyConfig *cfg) {
    if (!cfg) return false;
    char path[512];
    if (!resolve_config_path(path, sizeof path)) return false;

    /* O_NOFOLLOW: a same-UID attacker must not be able to point a symlink
     * here at a sensitive file; the file is opened read-only but is also
     * stat()ed. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || st.st_size < 0 ||
        (u64)st.st_size > (u64)MB(1)) {
        close(fd);
        return false;
    }

    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return false; }

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        /* Reject rather than parse a prefix of an overlong logical line; the
         * suffix could otherwise be interpreted as a second key. */
        if (!strchr(line, '\n') && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) { }
            if (c == EOF) break;
            continue;
        }
        char *p = trim_ascii(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "liu-notify: notify.conf:%d: missing '='\n", lineno);
            continue;
        }
        *eq = '\0';
        char *key = trim_ascii(p);
        char *val = trim_ascii(eq + 1);

        /* strip optional surrounding double-quotes on value */
        size_t vlen = strlen(val);
        if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
            val[vlen - 1] = '\0';
            val++;
        }

        apply_kv(cfg, key, val, lineno);
    }
    fclose(f);
    return true;
}

/* Settings stores sound rules as absolute paths. If a bundled sound path is
 * missing and names one of the four bundled files, use that file from the
 * current Liu copy; preserve existing custom sound paths and bundled paths
 * that still exist. Speech remains the fallback only when enabled. */
static void follow_bundled_sounds(NotifyConfig *cfg) {
    u32 n = cfg->sound_count < NOTIFY_SOUND_RULE_CAP ? cfg->sound_count : NOTIFY_SOUND_RULE_CAP;
    for (u32 i = 0; i < n; i++) {
        NotifySoundRule *r = &cfg->sounds[i];
        const char *slash = strrchr(r->path, '/');
        if (!slash) continue;
        const char *name = slash + 1;
        bool shipped = false;
        for (usize k = 0; k < sizeof kDefs / sizeof kDefs[0] && !shipped; k++)
            shipped = strcmp(name, kDefs[k].file) == 0;
        static const char kDir[] = "/assets/sounds";
        usize dir_len = (usize)(slash - r->path);
        if (!shipped || dir_len < sizeof kDir - 1 ||
            memcmp(slash - (sizeof kDir - 1), kDir, sizeof kDir - 1) != 0) continue;
        struct stat st;
        if (stat(r->path, &st) == 0) continue;
        char here[1024];
        if (notify_bundled_sound_path(name, here, sizeof here) && strlen(here) < sizeof r->path)
            snprintf(r->path, sizeof r->path, "%s", here);
    }
}

bool notify_config_load_effective(NotifyConfig *cfg) {
    if (!cfg) return false;
    /* Defaults FIRST, every time: this is also what a reload calls, and a key
     * deleted from the file has to come back as its default, not linger. */
    notify_config_defaults(cfg);
    bool had_file = notify_config_load(cfg);
    follow_bundled_sounds(cfg);
    /* Seed bundled rules only when sound configuration is absent.
     * `sound_count == 0` can also mean that all rules were explicitly cleared.
     */
    if (cfg->sound_count == 0 && !cfg->sounds_configured)
        notify_config_seed_bundled_sounds(cfg);
    return had_file;
}

bool notify_config_tool_enabled(const NotifyConfig *cfg, u8 tool_id) {
    if (!cfg) return false;
    switch (tool_id) {
        case TOOL_CLAUDE:  return cfg->tool_enabled.claude;
        case TOOL_COPILOT: return cfg->tool_enabled.copilot;
        case TOOL_CODEX:   return cfg->tool_enabled.codex;
        case TOOL_CUSTOM:  return cfg->tool_enabled.custom;
        default:           return false;
    }
}

/* Map tool/event ids back to the tokens parse_*_token() expects. Note:
 * TOOL_CUSTOM (255) collides with NOTIFY_MATCH_ANY (also 255) at the byte
 * level — the parser treats `sound.custom` and `sound.any` identically, so
 * "any" is emitted for both and the catch-all lookup handles them. */
static const char *tool_token(u8 tool_id) {
    switch (tool_id) {
        case NOTIFY_MATCH_ANY: return "any";  /* also covers TOOL_CUSTOM */
        case TOOL_CLAUDE:      return "claude";
        case TOOL_COPILOT:     return "copilot";
        case TOOL_CODEX:       return "codex";
        default:               return NULL;
    }
}
static const char *event_token(u8 event_id) {
    switch (event_id) {
        case NOTIFY_MATCH_ANY: return "any";
        case EVT_START:        return "start";
        case EVT_STOP:         return "stop";
        case EVT_NOTIFY:       return "notify";
        case EVT_ERROR:        return "error";
        case EVT_COMPLETE:     return "complete";
        default:               return NULL;
    }
}

/* Best-effort `mkdir -p` for the parent of `path`. Walks the path, creating
 * intermediate dirs with 0700. Pre-existing dirs are fine. */
static bool ensure_parent_dir(const char *path) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return true;
    *slash = '\0';
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) return false;
    return true;
}

bool notify_config_save(const NotifyConfig *cfg) {
    if (!cfg) return false;
    char path[512];
    if (!resolve_config_path(path, sizeof path)) return false;
    if (!ensure_parent_dir(path)) return false;

    /* Write to a sibling tmp then rename — partial writes can't be picked up
     * by a concurrent reader (the daemon re-reading after SIGHUP). */
    char tmp[600];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) return false;

    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags < 0 || fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0) {
        close(fd); unlink(tmp); return false;
    }
    if (fchmod(fd, 0600) != 0) { close(fd); unlink(tmp); return false; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return false; }

    fprintf(f, "# liu-notify config — written by Liu settings UI\n");
    fprintf(f, "enabled = %s\n",              cfg->enabled ? "true" : "false");
    if (cfg->voice[0] && !strchr(cfg->voice, '\n') && !strchr(cfg->voice, '\r'))
        fprintf(f, "voice = %s\n",            cfg->voice);
    f32 rate = isfinite(cfg->rate) ? CLAMP(cfg->rate, 0.5f, 2.0f) : 1.0f;
    fprintf(f, "rate = %.2f\n",               (double)rate);
    /* Serialize per-event banner settings as the authoritative values.
     * The legacy desktop_notification key remains readable and
     * initializes every event slot, but omitting it from output
     * preserves per-event overrides on reload. */
    fprintf(f, "tts_fallback = %s\n",         cfg->tts_fallback ? "true" : "false");
    /* Always write the `sounds_configured` marker, including when the
     * sound-rule set is empty. This distinguishes an explicitly cleared
     * configuration from one that never specified sounds, so defaults are
     * not re-seeded. */
    fprintf(f, "sounds_configured = true\n");
    if (cfg->rate_limit_per_sec)
        fprintf(f, "rate_limit_per_sec = %u\n", cfg->rate_limit_per_sec);
    if (cfg->rate_burst)
        fprintf(f, "rate_burst = %u\n",         cfg->rate_burst);
    if (isfinite(cfg->dedup_window_sec) && cfg->dedup_window_sec > 0.0f)
        fprintf(f, "dedup_window_sec = %.2f\n", (double)cfg->dedup_window_sec);

    fprintf(f, "tool.claude.enabled  = %s\n", cfg->tool_enabled.claude  ? "true" : "false");
    fprintf(f, "tool.copilot.enabled = %s\n", cfg->tool_enabled.copilot ? "true" : "false");
    fprintf(f, "tool.codex.enabled   = %s\n", cfg->tool_enabled.codex   ? "true" : "false");
    fprintf(f, "tool.custom.enabled  = %s\n", cfg->tool_enabled.custom  ? "true" : "false");

    /* Per-event banner flags. Only emit events the UI actually exposes so
     * the file doesn't carry slots for the (start) event that has no
     * Settings row. */
    static const struct { u8 evt; const char *name; } banner_evts[4] = {
        { EVT_STOP, "stop" }, { EVT_NOTIFY, "notify" },
        { EVT_ERROR, "error" }, { EVT_COMPLETE, "complete" },
    };
    for (int i = 0; i < 4; i++) {
        fprintf(f, "banner.%s = %s\n", banner_evts[i].name,
                cfg->event_banner[banner_evts[i].evt] ? "true" : "false");
    }
    /* Per-event master on/off — same four UI-exposed events. */
    for (int i = 0; i < 4; i++) {
        fprintf(f, "enabled.%s = %s\n", banner_evts[i].name,
                cfg->event_enabled[banner_evts[i].evt] ? "true" : "false");
    }

    u32 sound_count = cfg->sound_count < NOTIFY_SOUND_RULE_CAP
                    ? cfg->sound_count : NOTIFY_SOUND_RULE_CAP;
    for (u32 i = 0; i < sound_count; i++) {
        const NotifySoundRule *r = &cfg->sounds[i];
        if (!r->path[0] || strchr(r->path, '\n') || strchr(r->path, '\r')) continue;
        const char *tt = tool_token(r->tool_id);
        const char *et = event_token(r->event_id);
        if (!tt || !et) continue;
        if (r->tool_id == NOTIFY_MATCH_ANY && r->event_id == NOTIFY_MATCH_ANY)
            fprintf(f, "sound = %s\n", r->path);
        else if (r->tool_id == NOTIFY_MATCH_ANY)
            fprintf(f, "sound.%s = %s\n", et, r->path);
        else if (r->event_id == NOTIFY_MATCH_ANY)
            fprintf(f, "sound.%s = %s\n", tt, r->path);
        else
            fprintf(f, "sound.%s.%s = %s\n", tt, et, r->path);
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f); unlink(tmp); return false;
    }
    if (fclose(f) != 0) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

const char *notify_config_sound_for(const NotifyConfig *cfg,
                                    u8 tool_id, u8 event_id) {
    if (!cfg) return NULL;
    const NotifySoundRule *best = NULL;
    int best_rank = -1;
    u32 sound_count = cfg->sound_count < NOTIFY_SOUND_RULE_CAP
                    ? cfg->sound_count : NOTIFY_SOUND_RULE_CAP;
    for (u32 i = 0; i < sound_count; i++) {
        const NotifySoundRule *r = &cfg->sounds[i];
        bool tool_ok  = (r->tool_id  == tool_id  || r->tool_id  == NOTIFY_MATCH_ANY);
        bool event_ok = (r->event_id == event_id || r->event_id == NOTIFY_MATCH_ANY);
        if (!tool_ok || !event_ok) continue;
        /* Rank: more specific is better. tool-exact + event-exact = 3. */
        int rank = (r->tool_id != NOTIFY_MATCH_ANY ? 2 : 0)
                 + (r->event_id != NOTIFY_MATCH_ANY ? 1 : 0);
        if (rank > best_rank) { best_rank = rank; best = r; }
    }
    return best && best->path[0] ? best->path : NULL;
}
