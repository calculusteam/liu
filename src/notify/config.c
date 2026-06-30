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
#include <limits.h>
#ifndef PLATFORM_WIN32
#include <pwd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static void apply_sound_rule(NotifyConfig *cfg, u8 tool, u8 event, const char *path);

void notify_config_defaults(NotifyConfig *cfg) {
    cfg->enabled               = true;
    cfg->voice[0]              = '\0';
    cfg->rate                  = 1.0f;
    cfg->desktop_notification  = false;
    /* TTS is supported but OFF by default — notifications should play one of
     * the bundled wavs (see notify_config_seed_bundled_sounds), never speak,
     * unless the user explicitly opts back into speech. */
    cfg->tts_fallback          = false;
    cfg->rate_limit_per_sec    = 0;
    cfg->rate_burst            = 0;
    cfg->dedup_window_sec      = 0.0f;
    cfg->sound_count           = 0;
    memset(cfg->sounds, 0, sizeof cfg->sounds);
    memset(cfg->event_banner, 0, sizeof cfg->event_banner);
    /* Notify events are interrupt-class — needs user attention — so a
     * banner is the sensible default. Other events stay sound-only. */
    cfg->event_banner[EVT_NOTIFY] = true;
    /* Every event notifies by default — the per-event master switch starts
     * "On"; the user turns individual events off from Settings. */
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

/* Seed the four bundled-wav rules (one per event, any tool) so the default
 * experience is "play the shipped sound", never speak. Idempotent: an
 * existing exact (tool,event) rule is overwritten, so calling this when the
 * user already mapped a custom sound for an event leaves their choice intact
 * for the *other* events. Missing files are skipped. */
void notify_config_seed_bundled_sounds(NotifyConfig *cfg) {
    if (!cfg) return;
    static const struct { const char *file; u8 event; } kDefs[] = {
        { "complete.wav", EVT_COMPLETE },
        { "notify.wav",   EVT_NOTIFY   },
        { "error.wav",    EVT_ERROR    },
        { "stop.wav",     EVT_STOP     },
    };
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
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(val, &end, 10);
    if (errno || !end || *end != '\0' || v > 0xffffffffUL) return false;
    *out = (u32)v;
    return true;
}

static bool parse_f32(const char *val, f32 *out) {
    char *end = NULL;
    errno = 0;
    double v = strtod(val, &end);
    if (errno || !end || *end != '\0') return false;
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
        /* Legacy global. Fold it into every per-event slot so the
         * decision lives in one place (event_banner[]). Applied in file
         * order, which means a later `banner.<event> = ...` line still
         * wins — users can override individual events even when the
         * global is set. The mirror field stays for any older code that
         * still reads it directly. */
        cfg->desktop_notification = b;
        for (usize e = 0;
             e < sizeof cfg->event_banner / sizeof cfg->event_banner[0]; e++) {
            cfg->event_banner[e] = b;
        }
    }
    else if (!strcmp(key, "tts_fallback")         && parse_bool(val, &b)) cfg->tts_fallback = b;
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
    char path[512];
    if (!resolve_config_path(path, sizeof path)) return false;

    /* O_NOFOLLOW: don't let a same-UID attacker symlink us into a sensitive
     * file for read — we fopen read-only but we also stat() it. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;

    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return false; }

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
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

bool notify_config_tool_enabled(const NotifyConfig *cfg, u8 tool_id) {
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
 * we just emit "any" for both and let the catch-all lookup handle them. */
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
    char path[512];
    if (!resolve_config_path(path, sizeof path)) return false;
    if (!ensure_parent_dir(path)) return false;

    /* Write to a sibling tmp then rename — partial writes can't be picked up
     * by a concurrent reader (the daemon re-reading after SIGHUP). */
    char tmp[600];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof tmp) return false;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return false; }

    fprintf(f, "# liu-notify config — written by Liu settings UI\n");
    fprintf(f, "enabled = %s\n",              cfg->enabled ? "true" : "false");
    if (cfg->voice[0])
        fprintf(f, "voice = %s\n",            cfg->voice);
    fprintf(f, "rate = %.2f\n",               (double)cfg->rate);
    /* desktop_notification is intentionally NOT written — per-event
     * banner.<event> lines below are the source of truth. The legacy
     * global is still loadable for hand-edited configs (apply_kv folds
     * it into every event slot), but writing it back would resurrect
     * the override that hid per-event "false" toggles in the UI. */
    fprintf(f, "tts_fallback = %s\n",         cfg->tts_fallback ? "true" : "false");
    if (cfg->rate_limit_per_sec)
        fprintf(f, "rate_limit_per_sec = %u\n", cfg->rate_limit_per_sec);
    if (cfg->rate_burst)
        fprintf(f, "rate_burst = %u\n",         cfg->rate_burst);
    if (cfg->dedup_window_sec > 0.0f)
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

    for (u32 i = 0; i < cfg->sound_count; i++) {
        const NotifySoundRule *r = &cfg->sounds[i];
        if (!r->path[0]) continue;
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
    fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

const char *notify_config_sound_for(const NotifyConfig *cfg,
                                    u8 tool_id, u8 event_id) {
    const NotifySoundRule *best = NULL;
    int best_rank = -1;
    for (u32 i = 0; i < cfg->sound_count; i++) {
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
