/*
 * Liu - in-app settings panel implementation
 */
#include "ui/settings.h"
#include "ui/anim.h"
#include "ui/icon.h"
#include "ui/chrome_palette.h"
#include "core/string_utils.h"
#include "core/utf8.h"
#include "core/theme_import.h"
#include "translate/model_catalog.h"
#include "translate/translate_api.h"
#include "translate/model_download.h"
#include "translate/model_paths.h"
#include "platform/platform.h"
#include "vault/crypto.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#ifndef PLATFORM_WIN32
#include <pwd.h>
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h> /* waitpid() — reap the Test-button afplay forks */
#include <errno.h>
#include <strings.h>
#include <signal.h>
#include <limits.h>

/* App version (defined by CMake; fall back for non-CMake builds). */
#ifndef LIU_VERSION
#define LIU_VERSION "0.1.0"
#endif
#ifndef LIU_BUILD
#define LIU_BUILD ""
#endif
/* Project releases page (fallback target for the About button when the in-app
 * updater can't self-install, e.g. dev/bare-binary runs). */
#define LIU_RELEASES_URL "https://github.com/calculusteam/liu/releases"
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include "notify/protocol.h"
#include "notify/claude_hooks.h"
#include "notify/agent_hooks.h"
#include "notify/notify_server.h"
#include "core/agent_detect.h"
#include "update/updater.h"

#ifdef PLATFORM_MACOS
    #include <OpenGL/gl3.h>
#else
    #include <GL/gl.h>
#endif

/* =========================================================================
 * Font discovery — find monospace fonts available on system
 * ========================================================================= */

#define MAX_FONTS 64

typedef struct {
    char name[128];
    char path[512];
} FontEntry;

static FontEntry g_fonts[MAX_FONTS];
static i32 g_font_count = 0;
static bool g_fonts_discovered = false;

typedef struct {
    const char *name;
    const char *filename;
} BundledFontSpec;

static const BundledFontSpec g_bundled_fonts[] = {
    {"JetBrains Mono", "JetBrainsMono-Regular.ttf"},
    {"Fira Code", "FiraCode-Regular.ttf"},
    {"Cascadia Code", "CascadiaCode.ttf"},
    {"Hack", "Hack-Regular.ttf"},
    {"Source Code Pro", "SourceCodePro-Regular.ttf"},
    {"Victor Mono", "VictorMono-Regular.ttf"},
    {"IBM Plex Mono", "IBMPlexMono-Regular.ttf"},
    {"Inconsolata", "Inconsolata-Regular.ttf"},
    {"Roboto Mono", "RobotoMono-Regular.ttf"},
    {"Space Mono", "SpaceMono-Regular.ttf"},
    {"Ubuntu Mono", "UbuntuMono-Regular.ttf"},
    {"Anonymous Pro", "AnonymousPro-Regular.ttf"},
    {"Cousine", "Cousine-Regular.ttf"},
    {"PT Mono", "PTMono-Regular.ttf"},
};

static bool ensure_dir_recursive(const char *path) {
    if (!path || !path[0]) return false;

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }

    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static bool has_font_extension(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".ttf") == 0 ||
           strcasecmp(ext, ".otf") == 0 ||
           strcasecmp(ext, ".ttc") == 0 ||
           strcasecmp(ext, ".otc") == 0;
}

static void prettify_font_name(const char *filename, char *out, usize out_size) {
    usize oi = 0;
    for (usize i = 0; filename[i] && oi + 1 < out_size; i++) {
        char c = filename[i];
        if (c == '.') break;
        if (c == '_' || c == '-') c = ' ';
        out[oi++] = c;
    }
    out[oi] = '\0';
}

static void add_font_from_path(const char *name, const char *path) {
    if (g_font_count >= MAX_FONTS) return;
    if (access(path, R_OK) != 0) return;
    for (i32 i = 0; i < g_font_count; i++) {
        if (strcmp(g_fonts[i].path, path) == 0) return;
    }
    FontEntry *e = &g_fonts[g_font_count];
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->path, sizeof(e->path), "%s", path);
    g_font_count++;
}

static void add_bundled_font(const char *name, const char *filename) {
    char path[1024];
    liu_path_join(path, sizeof(path), font_user_dir(), filename);
    add_font_from_path(name, path);
}

/* Files in the font dirs that exist for internal use only (icon glyph
 * fallback, ligature shaping, …) and would render as garbage if picked
 * as the primary terminal font. Match by case-insensitive substring on
 * the filename. */
static bool is_blacklisted_font_file(const char *filename) {
    if (!filename) return false;
    static const char *blacklist[] = {
        "SymbolsNerdFontMono",   /* icon-only fallback font (PUA glyphs) */
        "SystemNerdFontMono",    /* alias the user asked us to hide */
        NULL,
    };
    for (i32 i = 0; blacklist[i]; i++) {
        if (strcasestr(filename, blacklist[i])) return true;
    }
    return false;
}

static void add_fonts_from_dir(const char *dir) {
    if (!dir || !dir[0]) return;
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_font_count < MAX_FONTS) {
        if (ent->d_name[0] == '.') continue;
        if (!has_font_extension(ent->d_name)) continue;
        if (is_blacklisted_font_file(ent->d_name)) continue;

        char path[1024];
        char name[128];
        liu_path_join(path, sizeof(path), dir, ent->d_name);
        prettify_font_name(ent->d_name, name, sizeof(name));
        add_font_from_path(name, path);
    }
    closedir(d);
}

static const char *path_basename(const char *path) {
    if (!path) return "";
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

/* ---- hover-area registry (drives pointing-hand cursor in main.c) ---- */

static void sp_push_hover(SettingsPanel *sp, f32 x, f32 y, f32 w, f32 h) {
    if (!sp) return;
    i32 cap = (i32)(sizeof sp->hover_areas / sizeof sp->hover_areas[0]);
    if (sp->hover_area_count >= cap) return;
    sp->hover_areas[sp->hover_area_count].x = x;
    sp->hover_areas[sp->hover_area_count].y = y;
    sp->hover_areas[sp->hover_area_count].w = w;
    sp->hover_areas[sp->hover_area_count].h = h;
    sp->hover_area_count++;
}

bool settings_point_clickable(const SettingsPanel *sp, f32 x, f32 y) {
    if (!sp || !sp->open) return false;
    for (i32 i = 0; i < sp->hover_area_count; i++) {
        const f32 ax = sp->hover_areas[i].x;
        const f32 ay = sp->hover_areas[i].y;
        if (x >= ax && x < ax + sp->hover_areas[i].w &&
            y >= ay && y < ay + sp->hover_areas[i].h) return true;
    }
    return false;
}

/* ---- liu-notify integration helpers (used by SETTINGS_TAB_NOTIFY) ---- */

/* Forward decl — ensure_loaded calls into seed_defaults, which is defined
 * further down (right after the path resolver it depends on). */
static void settings_notify_seed_defaults(SettingsPanel *sp);

/* Lazy-load the daemon config into the panel. Called from both the render
 * and click handler — once loaded, mutations live in sp->notify_cfg until
 * settings_notify_apply_save flushes them.
 *
 * First-run seeding: when no notify.conf existed on disk, populate the
 * four event slots with bundled wav paths and persist them so the next
 * daemon launch picks them up. Re-runs after a user-cleared config do
 * NOT re-seed (the file exists, just with no sound rules) — that's the
 * "I want pure TTS, stop suggesting sounds" escape hatch. */
static void settings_notify_ensure_loaded(SettingsPanel *sp) {
    if (sp->notify_cfg_loaded) return;
    notify_config_defaults(&sp->notify_cfg);
    bool had_file = notify_config_load(&sp->notify_cfg);
    if (!had_file) {
        settings_notify_seed_defaults(sp);
        notify_config_save(&sp->notify_cfg);
    }
    sp->notify_cfg_loaded = true;
}

/* Persist the working config and tell the in-process server to re-read it so
 * sound-rule / voice changes apply live. No-op when this process isn't the
 * one hosting the server — it will load the saved file on next launch. */
static void settings_notify_apply_save(SettingsPanel *sp) {
    if (!sp || !sp->notify_cfg_loaded) return;
    notify_config_save(&sp->notify_cfg);
    notify_server_reload();
}

/* Fire-and-forget afplay so the Test button doesn't block the UI thread.
 * Double-fork so the grandchild that runs afplay is reparented to init and
 * reaped by it — otherwise the finished afplay processes pile up as zombies
 * (the UI thread never waitpid()s them). The intermediate child exits at once
 * and is reaped synchronously here, so nothing leaks. */
static void settings_notify_play_test(const char *path) {
    if (!path || !*path) return;
    pid_t pid = fork();
    if (pid == 0) {
        /* Intermediate child: detach the player into a grandchild. */
        pid_t grandchild = fork();
        if (grandchild == 0) {
            execlp("afplay", "afplay", path, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0); /* reap the short-lived intermediate child */
}

/* Short human label for the 4 events the Settings UI exposes. */
static const char *notify_event_short(u8 evt) {
    switch (evt) {
        case EVT_STOP:     return "Stop";
        case EVT_NOTIFY:   return "Notify";
        case EVT_ERROR:    return "Error";
        case EVT_COMPLETE: return "Complete";
        default:           return "?";
    }
}

/* mkdir -p: create every prefix of `path` (ignoring EEXIST). */
static void settings_mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    usize n = strlen(tmp);
    for (usize i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0700);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0700);
}

/* Build (and ensure the directory of) the file path where a user-recorded
 * notification sound for `evt` lives: $XDG_CONFIG_HOME/liu/sounds or
 * ~/.config/liu/sounds, alongside notify.conf. Returns false if no home
 * directory can be resolved. */
static bool settings_notify_record_path(u8 evt, char *out, usize cap) {
    char dir[1024];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(dir, sizeof dir, "%s/liu/sounds", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            struct passwd *pw = getpwuid(geteuid());
            home = (pw && pw->pw_dir) ? pw->pw_dir : NULL;
        }
        if (!home || !*home) return false;
        snprintf(dir, sizeof dir, "%s/.config/liu/sounds", home);
    }
    settings_mkdir_p(dir);
    snprintf(out, cap, "%s/notify_%s.m4a", dir, notify_event_short(evt));
    return true;
}

/* Stop the in-flight recording (if any), and — when the captured file is
 * non-trivial — install it as the recording row's event sound and persist.
 * Idempotent: a no-op when nothing is recording. */
static void settings_notify_finalize_recording(SettingsPanel *sp) {
    if (!sp) return;
    i32 row = sp->notify_recording_row;
    sp->notify_recording_row = -1;        /* clear before re-entrant calls */
    /* Always release the mic — this also clears an "orphan" recording that a
     * late permission-grant may have started after the user cancelled. */
    platform_audio_record_stop();
    if (row < 0 || row >= 4) return;      /* nothing tracked to assign */
    static const u8 kEvents[4] = {
        EVT_STOP, EVT_NOTIFY, EVT_ERROR, EVT_COMPLETE,
    };
    u8 evt = kEvents[row];
    char rp[1024];
    if (settings_notify_record_path(evt, rp, sizeof rp)) {
        struct stat st;
        if (stat(rp, &st) == 0 && st.st_size > 256) {
            notify_config_apply_sound_rule(&sp->notify_cfg,
                                           NOTIFY_MATCH_ANY, evt, rp);
            settings_notify_apply_save(sp);
            snprintf(sp->notify_record_status, sizeof sp->notify_record_status,
                     "Saved your recording for \"%s\".", notify_event_short(evt));
        } else {
            /* Empty/failed capture (e.g. record then immediately stop, or no
             * mic input) — don't assign a broken file, and say why. */
            snprintf(sp->notify_record_status, sizeof sp->notify_record_status,
                     "Recording was too short \xe2\x80\x94 nothing saved.");
        }
    }
}

/* Resolve `<name>` (e.g. "complete.wav") into an absolute path to the
 * bundled sound file. Tries the .app bundle's Contents/assets/sounds/
 * first (production path resolved via _NSGetExecutablePath), then the
 * compile-time LIU_ASSETS_DIR (dev builds running straight from build/).
 * The candidate is realpath()'d so the final string has no `..` segment
 * — NSSound copes with relative components but the config file is much
 * easier to debug when it stores a canonical absolute path. */
static bool notify_default_sound_path(const char *name, char *out, usize cap) {
    if (!name || !*name || !out || cap < 4) return false;
    struct stat st;
    char candidate[1024];
    candidate[0] = '\0';
#if defined(__APPLE__)
    /* Bundle: Contents/MacOS/Liu → Contents/assets/sounds/<name> */
    char exe[1024];
    uint32_t exe_sz = sizeof exe;
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
    if (!candidate[0]) { out[0] = '\0'; return false; }
    char resolved[PATH_MAX];
    if (realpath(candidate, resolved)) {
        if (strlen(resolved) < cap) {
            snprintf(out, cap, "%s", resolved);
            return true;
        }
    }
    /* realpath shouldn't fail when stat just succeeded, but cap the path
     * fallback to the unresolved string rather than dropping the rule. */
    snprintf(out, cap, "%s", candidate);
    return true;
}

/* Resolve the absolute path of the liu-notify binary that ships next to
 * Liu. The Claude hook installer needs an absolute path so Claude can
 * spawn it regardless of the shell PATH it inherits. Falls back to the
 * bare name "liu-notify" if introspection fails — that still works when
 * the user has the binary on PATH. */
static bool resolve_liu_notify_path(char *out, size_t cap) {
    if (!out || cap < 4) return false;
#if defined(__APPLE__)
    char exe[1024]; uint32_t exe_sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &exe_sz) == 0) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            int n = snprintf(out, cap, "%s/liu-notify", exe);
            if (n > 0 && (usize)n < cap) return true;
        }
    }
#endif
    int n = snprintf(out, cap, "liu-notify");
    return n > 0 && (usize)n < cap;
}

/* Populate the freshly-loaded config with bundled defaults. Called from
 * (a) first-install (no notify.conf existed) and (b) "Reset to bundled
 * defaults" — both want a clean slate so the bundled (any, event) rules
 * (rank 1 in the resolver) aren't shadowed by stale (tool, any) rules
 * (rank 2) that a user might have configured manually long ago. Stop and
 * Error intentionally share error.wav (kalın version of complete.wav). */
static void settings_notify_seed_defaults(SettingsPanel *sp) {
    /* Hard reset: drop every existing sound rule, otherwise old higher-
     * rank entries (e.g. sound.claude.* from a hand-edited config) keep
     * winning the resolver lookup and the bundled wavs never play. */
    sp->notify_cfg.sound_count = 0;
    memset(sp->notify_cfg.sounds, 0, sizeof sp->notify_cfg.sounds);

    char path[1024];
    if (notify_default_sound_path("complete.wav", path, sizeof path))
        notify_config_apply_sound_rule(&sp->notify_cfg,
                                       NOTIFY_MATCH_ANY, EVT_COMPLETE, path);
    if (notify_default_sound_path("notify.wav", path, sizeof path))
        notify_config_apply_sound_rule(&sp->notify_cfg,
                                       NOTIFY_MATCH_ANY, EVT_NOTIFY, path);
    if (notify_default_sound_path("error.wav", path, sizeof path))
        notify_config_apply_sound_rule(&sp->notify_cfg,
                                       NOTIFY_MATCH_ANY, EVT_ERROR, path);
    /* Stop ships as its own file even though it's an exact copy of error
     * today — keeps every event 1-to-1 with a file under assets/sounds/
     * so a future "swap Stop for a louder cue" doesn't risk regressing
     * Error too. */
    if (notify_default_sound_path("stop.wav", path, sizeof path))
        notify_config_apply_sound_rule(&sp->notify_cfg,
                                       NOTIFY_MATCH_ANY, EVT_STOP, path);
}

static bool copy_file_binary(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }

    char buf[32768];
    size_t nread;
    bool ok = true;
    while ((nread = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, nread, out) != nread) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;

    fclose(in);
    fclose(out);
    return ok;
}

static void discover_fonts(void) {
    if (g_fonts_discovered) return;
    g_fonts_discovered = true;
    g_font_count = 0;

    for (usize i = 0; i < sizeof(g_bundled_fonts) / sizeof(g_bundled_fonts[0]); i++) {
        add_bundled_font(g_bundled_fonts[i].name, g_bundled_fonts[i].filename);
    }
    add_fonts_from_dir(font_user_dir());
    add_fonts_from_dir(font_custom_dir());
}

/* Accessor functions for font data (used by native settings panel) */
i32 settings_get_font_count(void) { discover_fonts(); return g_font_count; }
const char *settings_get_font_name(i32 idx) { return (idx >= 0 && idx < g_font_count) ? g_fonts[idx].name : ""; }
const char *settings_get_font_path(i32 idx) { return (idx >= 0 && idx < g_font_count) ? g_fonts[idx].path : ""; }
bool settings_get_font_installed(i32 idx) { return idx >= 0 && idx < g_font_count; }

void settings_refresh_fonts(void) {
    g_fonts_discovered = false;
    discover_fonts();
}

bool settings_import_font_file(const char *source_path, char *out_path, usize out_size) {
    if (!source_path || !source_path[0] || !has_font_extension(source_path)) return false;
    if (access(source_path, R_OK) != 0) return false;
    if (!ensure_dir_recursive(font_custom_dir())) return false;

    const char *filename = path_basename(source_path);
    if (!filename[0]) return false;

    char dest_path[1024];
    liu_path_join(dest_path, sizeof(dest_path), font_custom_dir(), filename);
    if (strcmp(source_path, dest_path) != 0) {
        const char *ext = strrchr(filename, '.');
        char stem[256];
        if (ext) {
            usize stem_len = (usize)(ext - filename);
            if (stem_len >= sizeof(stem)) stem_len = sizeof(stem) - 1;
            memcpy(stem, filename, stem_len);
            stem[stem_len] = '\0';
        } else {
            snprintf(stem, sizeof(stem), "%s", filename);
            ext = "";
        }

        i32 suffix = 2;
        while (access(dest_path, F_OK) == 0) {
            snprintf(dest_path, sizeof(dest_path), "%s/%s-%d%s", font_custom_dir(), stem, suffix, ext);
            suffix++;
        }

        if (!copy_file_binary(source_path, dest_path)) return false;
    }

    if (out_path && out_size > 0) {
        snprintf(out_path, out_size, "%s", dest_path);
    }
    settings_refresh_fonts();
    return true;
}

void settings_init(SettingsPanel *sp, AppConfig *config, KeyBindTable *keybinds,
                   Vault *vault) {
    discover_fonts();
    memset(sp, 0, sizeof(*sp));
    sp->config = config;
    sp->keybinds = keybinds;
    sp->vault = vault;
    sp->hover_item = -1;
    sp->notify_recording_row = -1;   /* 0 would mean "row 0 is recording" */
    if (config && config->font_path[0]) {
        char name[128];
        prettify_font_name(path_basename(config->font_path), name, sizeof(name));
        add_font_from_path(name, config->font_path);
    }

    /* First-launch bootstrap for the notify daemon — without this the
     * bundled wavs only get installed when the user happens to open the
     * Notifications tab. Settings_notify_ensure_loaded does the right
     * thing on a missing config (seed + save); on an existing one it's
     * a no-op so users who hand-curate notify.conf aren't disturbed. */
    settings_notify_ensure_loaded(sp);
}

/* Notify-tab agent detection cache. agent_detect_available() walks $PATH with
 * ~100-175 stat/access syscalls; the Notify tab re-rendered it every frame.
 * Cache it and refresh only when Settings (re)opens — covers "user installs an
 * agent then reopens Settings" without per-frame syscalls. */
static AgentInfo s_notify_agents[AGENT_MAX];
static i32       s_notify_agents_count = 0;
static bool      s_notify_agents_loaded = false;

void settings_toggle(SettingsPanel *sp) {
    sp->open = !sp->open;
    sp->scroll_y = 0;
    sp->hover_item = -1;
    if (sp->open) {
        settings_refresh_fonts();
        s_notify_agents_loaded = false;   /* re-detect agents on next Notify render */
        sp->notify_recording_row = -1;    /* never resume a recording across opens */
        sp->notify_record_status[0] = '\0';
        /* Never reopen with a Translate overlay/editor left set from a
         * prior session — the palette "Cycle Backend" command can change
         * the backend out from under these flags while Settings is closed,
         * which would otherwise leave an invisible dropdown intercepting
         * clicks or a hidden editor swallowing keys. */
        sp->translate_model_picker_open = false;
        sp->translate_model_menu_scroll = 0;
        sp->editing_api_key = false;
        sp->editing_api_url = false;
        sp->editing_api_model = false;
    } else {
        /* Finalize a mic recording in flight so the microphone is released
         * the moment the panel is dismissed. */
        settings_notify_finalize_recording(sp);
        /* Scrub any master-password buffers the user may have typed. */
        crypto_secure_zero(sp->vault_old_pw,  sizeof sp->vault_old_pw);
        crypto_secure_zero(sp->vault_new_pw,  sizeof sp->vault_new_pw);
        crypto_secure_zero(sp->vault_new_pw2, sizeof sp->vault_new_pw2);
        sp->vault_editing_old_pw = false;
        sp->vault_editing_new_pw = false;
        sp->vault_editing_new_pw2 = false;
    }
}

/* Fixed UI cell width (set by settings_render) — keeps settings text
 * spacing independent of the terminal font size. */
static f32 s_ui_cw = 0.0f;

/* Per-frame alpha multiplier for the entire settings panel (1.0 = opaque).
 * Drives the close-fade — every rect/rrect/glyph drawn through s_rect/
 * s_rrect_simple/draw_label has its color alpha scaled by this so the whole
 * panel fades in lockstep instead of just the dim overlay. */
static f32 s_panel_alpha = 1.0f;

static inline Color s_tint(Color c) { c.a *= s_panel_alpha; return c; }

static inline void s_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h, Color c) {
    renderer_draw_rect(r, x, y, w, h, s_tint(c));
}
static inline void s_rrect_simple(Renderer *r, f32 x, f32 y, f32 w, f32 h,
                                  Color fill, f32 radius) {
    renderer_draw_rrect_simple(r, x, y, w, h, s_tint(fill), radius);
}

/* Per-corner radii variant used by segmented controls / chip rows: the
 * leftmost segment rounds its left corners only, the rightmost rounds
 * its right corners, the middle segments stay flat so the shared edges
 * butt without gaps. Border width = 1 logical dpi pixel. */
static inline void s_rrect_bordered_per_corner(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill, Color border, f32 border_w,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl) {
    renderer_draw_rrect_bordered(r, x, y, w, h,
        s_tint(fill), s_tint(border), border_w,
        r_tl, r_tr, r_br, r_bl,
        0.0f, 0.0f, 0.0f, 0.0f);
}

static inline void s_rrect_bordered(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill, Color border, f32 border_w, f32 radius) {
    s_rrect_bordered_per_corner(r, x, y, w, h, fill, border, border_w,
                                radius, radius, radius, radius);
}

/* Fill-only rounded rect with independent corner radii (no border, no
 * shadow) — for selection pills / focus accents that round only some corners. */
static inline void s_rrect_per_corner(Renderer *r,
    f32 x, f32 y, f32 w, f32 h, Color fill,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl) {
    renderer_draw_rrect(r, x, y, w, h, s_tint(fill),
        r_tl, r_tr, r_br, r_bl,
        0.0f, 0.0f, 0.0f, 0.0f);
}

static inline void s_rrect_bordered_shadow(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill, Color border, f32 border_w,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
    f32 shadow_size, f32 shadow_alpha,
    f32 shadow_offset_x, f32 shadow_offset_y) {
    renderer_draw_rrect_bordered(r, x, y, w, h,
        s_tint(fill), s_tint(border), border_w,
        r_tl, r_tr, r_br, r_bl,
        shadow_size, shadow_alpha * s_panel_alpha,
        shadow_offset_x, shadow_offset_y);
}

/* Vector BELL icon for the per-event banner toggle. Built entirely from the
 * rrect family (s_rrect_per_corner / s_rrect_simple) so it batches in the SAME
 * rrect pass as the button background and composites ON TOP of it by submission
 * order — do NOT use s_rect/renderer_draw_rect here (the flat pass flushes first
 * and would draw the bell underneath the button bg), and no glyphs (that is the
 * cell-metric problem this replaces). All sizes derive from `size` so the icon
 * scales with dpi; (cx,cy) is the icon center in framebuffer pixels. `c` is the
 * on/off tint (bn_fg) — only its alpha is touched (via s_tint inside the
 * wrappers); rgb is left for the sRGB->linear boundary.
 *
 * Geometry (back-to-front): crown nub, flared dome body, base rim bar, clapper. */
__attribute__((unused))
static void draw_bell_icon(Renderer *r, f32 cx, f32 cy, f32 size, Color c) {
    /* Overall bell footprint is `size` tall; width a touch narrower. */
    f32 body_w   = size * 0.66f;          /* dome width                       */
    f32 body_h   = size * 0.60f;          /* dome height                      */
    f32 rim_w    = size * 0.80f;          /* flared lip, wider than the body  */
    f32 rim_h    = fmaxf(size * 0.12f, 1.5f);
    f32 nub_d    = fmaxf(size * 0.16f, 2.0f);   /* crown nub diameter         */
    f32 clap_d   = fmaxf(size * 0.18f, 2.0f);   /* clapper diameter           */
    f32 gap      = size * 0.06f;          /* rim -> clapper gap               */

    /* Vertical stack height (nub + body + rim + gap + clapper), centered on cy.
     * Nub overlaps the body top by half its height, so it does not add height. */
    f32 stack_h  = body_h + rim_h + gap + clap_d;
    f32 top_y    = cy - stack_h * 0.5f;

    f32 body_x   = cx - body_w * 0.5f;
    f32 body_y   = top_y;
    f32 rim_x    = cx - rim_w * 0.5f;
    f32 rim_y    = body_y + body_h;
    f32 nub_x    = cx - nub_d * 0.5f;
    f32 nub_y    = body_y - nub_d * 0.5f;       /* straddles the dome shoulder */
    f32 clap_x   = cx - clap_d * 0.5f;
    f32 clap_y   = rim_y + rim_h + gap;

    /* 1) Crown nub — tiny AA disc straddling the top of the dome. */
    s_rrect_simple(r, nub_x, nub_y, nub_d, nub_d, c, nub_d * 0.5f);

    /* 2) Dome body — very round top corners (smooth shoulder), nearly-square
     *    small bottom corners so the silhouette flares toward the rim. */
    f32 r_top = body_w * 0.46f;                  /* big -> rounded shoulder    */
    f32 r_bot = body_w * 0.12f;                  /* small -> flared sides      */
    s_rrect_per_corner(r, body_x, body_y, body_w, body_h, c,
                       r_top, r_top, r_bot, r_bot);

    /* 3) Base rim — wider lip flush under the body. Top corners square so it
     *    butts the body cleanly; bottom corners lightly rounded. */
    f32 rim_r = fminf(rim_h * 0.5f, rim_w * 0.10f);
    s_rrect_per_corner(r, rim_x, rim_y, rim_w, rim_h, c,
                       0.0f, 0.0f, rim_r, rim_r);

    /* 4) Clapper — small AA disc hanging just below the rim. */
    s_rrect_simple(r, clap_x, clap_y, clap_d, clap_d, c, clap_d * 0.5f);
}

/* Draw text helper */
static void draw_label_limited(Renderer *r, const char *text, f32 x, f32 y,
                               Color fg, i32 max_chars) {
    f32 adv = (s_ui_cw > 0.0f) ? s_ui_cw : r->font.cell_width;
    fg = s_tint(fg);
    const u8 *p = (const u8 *)text;
    i32 drawn = 0;
    if (max_chars < 0) max_chars = 64;
    while (*p && drawn < max_chars) {
        u32 cp;
        u32 consumed = utf8_decode(p, 4, &cp);
        if (consumed == 0) { p++; continue; }
        if (cp >= 32) {
            renderer_push_glyph(r, x, y, cp, fg);
            x += adv;
            drawn++;
        }
        p += consumed;
    }
}

static void draw_label(Renderer *r, const char *text, f32 x, f32 y, Color fg) {
    draw_label_limited(r, text, x, y, fg, 64);
}

static void draw_value(Renderer *r, const char *text, f32 x, f32 y, Color fg) {
    draw_label(r, text, x, y, fg);
}

/* Human-readable transfer rate / ETA for the model-download progress line. */
static void settings_fmt_speed(f64 bps, char *out, usize cap) {
    if (bps < 1024.0)            snprintf(out, cap, "%.0f B/s", bps);
    else if (bps < 1048576.0)    snprintf(out, cap, "%.0f KB/s", bps / 1024.0);
    else if (bps < 1073741824.0) snprintf(out, cap, "%.1f MB/s", bps / 1048576.0);
    else                         snprintf(out, cap, "%.2f GB/s", bps / 1073741824.0);
}
static void settings_fmt_eta(f64 secs, char *out, usize cap) {
    if (secs < 0.0) { snprintf(out, cap, "ETA —"); return; }
    int t = (int)(secs + 0.5);
    if (t < 60)        snprintf(out, cap, "ETA %ds", t);
    else if (t < 3600) snprintf(out, cap, "ETA %d:%02d", t / 60, t % 60);
    else               snprintf(out, cap, "ETA %dsa %ddk", t / 3600, (t % 3600) / 60);
}

/* Width a settings_button would occupy for `label` (UTF-8 code-point aware,
 * so Turkish diacritics don't over-size the box). */
static f32 settings_button_w(const char *label, f32 cw, f32 dpi) {
    i32 glyphs = 0;
    for (const char *s = label; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) glyphs++;
    return (f32)glyphs * cw + 20.0f * dpi;
}

/* Pill button with a centered label. Caches its hit rect into the four out
 * params (any may be NULL) and registers a hover region. Returns the button
 * width so callers can lay buttons out left-to-right. */
static f32 settings_button(Renderer *r, SettingsPanel *sp, const char *label,
                           f32 x, f32 y, f32 h, f32 cw, f32 ch, f32 dpi,
                           Color bg, Color border, Color fg,
                           f32 *bx, f32 *by, f32 *bw, f32 *bh) {
    f32 w   = settings_button_w(label, cw, dpi);
    f32 tw  = w - 20.0f * dpi;
    f32 rad = fminf(h * 0.5f, 6.0f * dpi);
    s_rrect_bordered(r, x, y, w, h, bg, border, fmaxf(1.0f, dpi), rad);
    renderer_flush_rrects(r);
    draw_label(r, label, x + (w - tw) * 0.5f, y + (h - ch) * 0.5f, fg);
    if (bx) *bx = x; if (by) *by = y; if (bw) *bw = w; if (bh) *bh = h;
    sp_push_hover(sp, x, y, w, h);
    return w;
}

static Color settings_blend(Color a, Color b, f32 t) {
    return (Color){
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t
    };
}

enum {
    SETTINGS_LANG_PICKER_CLOSED = 0,
    SETTINGS_LANG_PICKER_SOURCE = 1,
    SETTINGS_LANG_PICKER_TARGET = 2,
};

enum {
    SETTINGS_LANG_MENU_ACTION_NONE = -1,
    SETTINGS_LANG_MENU_ACTION_SWAP = -2,
    SETTINGS_LANG_MENU_ACTION_RESET = -3,
};

/* Static fallback model lists per agent CLI / API provider. The Model
 * dropdown prefers the live model_catalog lists (models.dev for
 * claude/codex/opencode, `grok models` for grok, the provider's
 * /v1/models for the API backend) — these tables only show until that
 * fetch lands, and whenever it fails. Keep ids short (<63 chars) to fit
 * agent_model[64]/api_model[64]. The "(default)" empty-string row is
 * added by settings_translate_model_options, not stored here. */
static const char *settings_translate_models_claude[] = {
    "claude-opus-4-8", "claude-opus-4-7", "claude-sonnet-4-6",
    "claude-haiku-4-5"
};
/* OpenCode is deliberately limited to the OpenCode Go plan's catalog
 * (ids carry the opencode-go/ provider prefix that `opencode run -m`
 * expects) — the wider zen/free/openrouter providers are excluded by
 * design. */
static const char *settings_translate_models_opencode[] = {
    "opencode-go/glm-5.1",
    "opencode-go/glm-5",
    "opencode-go/kimi-k2.6",
    "opencode-go/kimi-k2.5",
    "opencode-go/minimax-m3",
    "opencode-go/minimax-m2.7",
    "opencode-go/minimax-m2.5",
    "opencode-go/mimo-v2.5-pro",
    "opencode-go/mimo-v2.5",
    "opencode-go/qwen3.7-max",
    "opencode-go/qwen3.7-plus",
    "opencode-go/qwen3.6-plus",
    "opencode-go/deepseek-v4-pro",
    "opencode-go/deepseek-v4-flash",
};
/* Codex passes the model via `-c model=<X>` (a config override) — the
 * spawn path handles the flag shape, this list just enumerates what
 * the user picks from. Slugs are the canonical Codex CLI model ids
 * from developers.openai.com/codex/models; users can also paste any
 * other model id via JSON config (agent_model[64] holds up to 63 chars).
 * Order: recommended default first, then the rest of the GPT-5.x line
 * (general → coding-specialised → "Spark" reasoning → older). */
static const char *settings_translate_models_codex[] = {
    "gpt-5.5",                /* recommended default for most tasks */
    "gpt-5.4",
    "gpt-5.4-mini",
    "gpt-5.3-codex",          /* coding-specialised */
    "gpt-5.3-codex-spark",    /* "Spark" — strong reasoning + tool use */
    "gpt-5.2"
};
/* Grok passes the model via `-m <X>` (handled in translate_agent.c). Slugs are
 * the canonical Grok CLI model ids; "" lets Grok use its configured default
 * (grok-composer-2.5-fast). */
static const char *settings_translate_models_grok[] = {
    "grok-composer-2.5-fast",    /* fast, good for translation */
    "grok-build"                 /* heavier coding model */
};
/* API backend fallbacks — shown until the provider's /v1/models fetch
 * (which needs the user's key) succeeds. */
static const char *settings_translate_models_api_anthropic[] = {
    "claude-haiku-4-5", "claude-sonnet-4-6", "claude-opus-4-8"
};
static const char *settings_translate_models_api_openai[] = {
    "gpt-5.4-mini", "gpt-5.5", "gpt-5.4", "gpt-5.2"
};
static const char *settings_translate_models_api_openrouter[] = {
    "anthropic/claude-haiku-4.5", "anthropic/claude-sonnet-4.6",
    "openai/gpt-5.4-mini", "google/gemini-3.5-flash",
};

#define SETTINGS_MODEL_OPTS_CAP (MODEL_CATALOG_MAX + 1)

/* Fill `out` with the Model dropdown rows for the current backend and
 * agent/provider selection. Row 0 is "" ("(default)") except for the
 * custom API provider, which has no meaningful default. Prefers the live
 * model_catalog list whenever it has entries (READY, or stale ids kept
 * across a failed refresh); falls back to the static tables above.
 * Returns the row count. */
static i32 settings_translate_model_options(const TranslateConfig *tc,
                                            const char **out, i32 cap) {
    i32 n = 0;
    bool is_api  = tc->backend == TRANSLATE_BACKEND_API;
    bool custom  = is_api && strcmp(tc->api_provider, "custom") == 0;
    if (!custom && n < cap) out[n++] = "";

    const ModelList *live = is_api ? model_catalog_api(tc->api_provider)
                                   : model_catalog_agent(tc->agent_id);
    if (live && live->count > 0) {
        for (i32 i = 0; i < live->count && n < cap; i++) {
            out[n++] = live->ids[i];
        }
        return n;
    }

    const char **fb = NULL;
    i32 fbn = 0;
#define SETTINGS_PICK_FB(tbl) do { \
        fb = tbl; fbn = (i32)(sizeof(tbl) / sizeof(tbl[0])); } while (0)
    if (is_api) {
        if (strcmp(tc->api_provider, "openai") == 0) {
            SETTINGS_PICK_FB(settings_translate_models_api_openai);
        } else if (strcmp(tc->api_provider, "openrouter") == 0) {
            SETTINGS_PICK_FB(settings_translate_models_api_openrouter);
        } else if (custom) {
            fb = NULL; fbn = 0;   /* nothing sensible to suggest */
        } else {
            SETTINGS_PICK_FB(settings_translate_models_api_anthropic);
        }
    } else if (strcmp(tc->agent_id, "opencode") == 0) {
        SETTINGS_PICK_FB(settings_translate_models_opencode);
    } else if (strcmp(tc->agent_id, "codex") == 0) {
        SETTINGS_PICK_FB(settings_translate_models_codex);
    } else if (strcmp(tc->agent_id, "grok") == 0) {
        SETTINGS_PICK_FB(settings_translate_models_grok);
    } else {
        SETTINGS_PICK_FB(settings_translate_models_claude);
    }
#undef SETTINGS_PICK_FB
    for (i32 i = 0; i < fbn && n < cap; i++) out[n++] = fb[i];
    return n;
}

/* The model the dropdown's selection highlight should match — the agent
 * model under AGENT, the API model under API. */
static char *settings_translate_model_slot(TranslateConfig *tc,
                                           usize *out_cap) {
    if (tc->backend == TRANSLATE_BACKEND_API) {
        if (out_cap) *out_cap = sizeof(tc->api_model);
        return tc->api_model;
    }
    if (out_cap) *out_cap = sizeof(tc->agent_model);
    return tc->agent_model;
}

typedef struct {
    f32 px, py, panel_w, panel_h;
    f32 title_h, nav_w, nav_row_h;
    f32 content_x, content_top, content_w;
    f32 clip_top, clip_bottom;
} SettingsLayout;

static const char *settings_tab_name(SettingsTab tab) {
    static const char *names[] = {
        "Appearance", "Terminal", "Translate", "Notifications",
        "Keys", "Vault", "About"
    };
    i32 idx = (i32)tab;
    return (idx >= 0 && idx < SETTINGS_TAB_COUNT) ? names[idx] : "Settings";
}

static const char *settings_tab_desc(SettingsTab tab) {
    switch (tab) {
    case SETTINGS_TAB_APPEARANCE: return "Fonts, themes, and terminal density.";
    case SETTINGS_TAB_TERMINAL:   return "Cursor, behavior, chrome, and transparency.";
    case SETTINGS_TAB_TRANSLATE:  return "Translate-on-Tab engine and allowlist.";
    case SETTINGS_TAB_NOTIFY:     return "Per-event sound files for liu-notify.";
    case SETTINGS_TAB_KEYS:       return "Keyboard shortcuts and rebinding.";
    case SETTINGS_TAB_VAULT:      return "Encrypted secrets and lock behavior.";
    case SETTINGS_TAB_ABOUT:      return "Version, updates, and configuration export.";
    default:                      return "";
    }
}

static SettingsLayout settings_layout_for(f32 sw, f32 sh, f32 dpi,
                                          f32 scale, f32 yoff) {
    f32 margin = 16.0f * dpi;
    f32 base_w = 860.0f * dpi;
    f32 max_w = sw - 2.0f * margin;
    if (max_w < 320.0f * dpi) max_w = sw - 2.0f * dpi;
    if (base_w > max_w) base_w = max_w;
    if (base_w < 1.0f) base_w = 1.0f;

    f32 base_h = sh - 2.0f * margin;
    if (base_h > 760.0f * dpi) base_h = 760.0f * dpi;
    if (base_h < 360.0f * dpi) base_h = sh - 2.0f * dpi;
    if (base_h < 1.0f) base_h = 1.0f;

    f32 title_h = 38.0f * dpi;
    if (base_h < 360.0f * dpi) title_h = 32.0f * dpi;
    if (title_h > base_h * 0.22f) title_h = base_h * 0.22f;
    if (title_h < 26.0f * dpi) title_h = 26.0f * dpi;

    f32 nav_w = 138.0f * dpi;
    if (base_w < 700.0f * dpi) nav_w = 122.0f * dpi;
    if (base_w < 560.0f * dpi) nav_w = 108.0f * dpi;
    if (base_w < 420.0f * dpi) nav_w = 92.0f * dpi;

    f32 content_pad = 28.0f * dpi;
    if (base_w < 560.0f * dpi) content_pad = 18.0f * dpi;
    if (base_w < 420.0f * dpi) content_pad = 12.0f * dpi;

    f32 min_content_w = 24.0f * dpi;
    f32 max_nav_w = base_w - 2.0f * content_pad - min_content_w;
    if (nav_w > max_nav_w) nav_w = max_nav_w;
    if (nav_w < 0.0f) nav_w = 0.0f;

    f32 content_top_pad = 24.0f * dpi;
    if (base_h < 420.0f * dpi) content_top_pad = 16.0f * dpi;
    if (base_h < 280.0f * dpi) content_top_pad = 8.0f * dpi;

    f32 nav_row_h = 30.0f * dpi;
    f32 nav_space = base_h - title_h - 10.0f * dpi;
    f32 nav_fit_h = nav_space / (f32)SETTINGS_TAB_COUNT;
    if (nav_fit_h > 0.0f && nav_row_h > nav_fit_h) nav_row_h = nav_fit_h;
    if (nav_row_h < 22.0f * dpi) nav_row_h = 22.0f * dpi;

    SettingsLayout l = {0};
    l.panel_w = base_w * scale;
    l.panel_h = base_h * scale;
    l.px = (sw - l.panel_w) * 0.5f;
    l.py = (sh - l.panel_h) * 0.5f - yoff;
    if (l.px < dpi) l.px = dpi;
    if (l.py < dpi) l.py = dpi;
    if (l.px + l.panel_w > sw - dpi) l.px = sw - dpi - l.panel_w;
    if (l.py + l.panel_h > sh - dpi) l.py = sh - dpi - l.panel_h;
    if (l.px < 0.0f) l.px = 0.0f;
    if (l.py < 0.0f) l.py = 0.0f;
    l.title_h = title_h * scale;
    l.nav_w = nav_w * scale;
    l.nav_row_h = nav_row_h * scale;
    l.content_x = l.px + l.nav_w + content_pad * scale;
    l.content_top = l.py + l.title_h + content_top_pad * scale;
    l.content_w = l.panel_w - l.nav_w - 2.0f * content_pad * scale;
    if (l.content_w < 1.0f) l.content_w = 1.0f;
    l.clip_top = l.py + l.title_h;
    l.clip_bottom = l.py + l.panel_h;
    return l;
}

/* =========================================================================
 * Keyboard-shortcuts tab — single source of truth for the row list.
 *
 * The renderer (settings_render) and the click hit-test (settings_handle_click)
 * both walk this one table at a uniform row pitch, so their geometry can never
 * drift apart. Three row kinds, distinguished without a tag field:
 *   - Section header:  action == ACT_NONE, keys == NULL   (label = title)
 *   - Rebindable row:  action != ACT_NONE                 (label/keys derived)
 *   - Read-only row:   action == ACT_NONE, keys != NULL   (label + fixed keys)
 * Read-only rows document chords that are handled inline in the event loop
 * (window snap, divider nudge, …) and therefore cannot be rebound here.
 * ========================================================================= */
typedef struct {
    Action      action;
    const char *label;   /* section title, or read-only row name */
    const char *keys;    /* read-only row key string; NULL otherwise */
} KeysRow;

static const KeysRow g_keys_rows[] = {
    { ACT_NONE, "Tabs", NULL },
    { ACT_NEW_TAB,          NULL, NULL },
    { ACT_CLOSE_TAB,        NULL, NULL },
    { ACT_UNDO_CLOSE_TAB,   NULL, NULL },
    { ACT_NEXT_TAB,         NULL, NULL },
    { ACT_PREV_TAB,         NULL, NULL },
    { ACT_TAB_1, NULL, NULL }, { ACT_TAB_2, NULL, NULL }, { ACT_TAB_3, NULL, NULL },
    { ACT_TAB_4, NULL, NULL }, { ACT_TAB_5, NULL, NULL }, { ACT_TAB_6, NULL, NULL },
    { ACT_TAB_7, NULL, NULL }, { ACT_TAB_8, NULL, NULL }, { ACT_TAB_9, NULL, NULL },
    { ACT_RENAME_TAB,       NULL, NULL },
    { ACT_CREATE_TAB_GROUP, NULL, NULL },
    { ACT_TOGGLE_TAB_GROUP, NULL, NULL },

    { ACT_NONE, "View", NULL },
    { ACT_TOGGLE_SIDEBAR,   NULL, NULL },
    { ACT_COMMAND_PALETTE,  NULL, NULL },
    { ACT_QUAKE_TOGGLE,     NULL, NULL },

    { ACT_NONE, "Font", NULL },
    { ACT_FONT_BIGGER,      NULL, NULL },
    { ACT_FONT_SMALLER,     NULL, NULL },
    { ACT_FONT_RESET,       NULL, NULL },

    { ACT_NONE, "Edit", NULL },
    { ACT_COPY,             NULL, NULL },
    { ACT_PASTE,            NULL, NULL },
    { ACT_SELECT_ALL,       NULL, NULL },
    { ACT_FIND,             NULL, NULL },

    { ACT_NONE, "Scroll & navigation", NULL },
    { ACT_SCROLL_UP_PAGE,   NULL, NULL },
    { ACT_SCROLL_DOWN_PAGE, NULL, NULL },
    { ACT_SCROLL_TO_TOP,    NULL, NULL },
    { ACT_SCROLL_TO_BOTTOM, NULL, NULL },
    { ACT_PREV_PROMPT,      NULL, NULL },
    { ACT_NEXT_PROMPT,      NULL, NULL },
    { ACT_ACCEPT_SUGGESTION, NULL, NULL },
    { ACT_CLEAR_SCREEN,     NULL, NULL },

    { ACT_NONE, "Panes & session", NULL },
    { ACT_SPLIT_HORIZONTAL, NULL, NULL },
    { ACT_SPLIT_VERTICAL,   NULL, NULL },
    { ACT_BROADCAST_TOGGLE, NULL, NULL },
    { ACT_SAVE_FILE,        NULL, NULL },

    { ACT_NONE, "Application", NULL },
    { ACT_SETTINGS,         NULL, NULL },
    { ACT_SSH_CONNECT,      NULL, NULL },
    { ACT_IMPORT_SSH_CONFIG, NULL, NULL },

    { ACT_NONE, "Vault", NULL },
    { ACT_VAULT_BROWSER,       NULL, NULL },
    { ACT_VAULT_UNLOCK,        NULL, NULL },
    { ACT_VAULT_LOCK,          NULL, NULL },
    { ACT_VAULT_CHANGE_MASTER, NULL, NULL },

    { ACT_NONE, "Window & system  ·  not rebindable", NULL },
    { ACT_NONE, "Snap window half / maximize",  "Ctrl+Cmd+Arrows" },
    { ACT_NONE, "Snap window quadrant",         "Ctrl+Cmd+Shift+Arrows" },
    { ACT_NONE, "Resize split divider",         "Cmd+Alt+Arrows" },
    { ACT_NONE, "Command history",              "Alt+Up" },
    { ACT_NONE, "Close item under cursor",      "Cmd+R" },
    { ACT_NONE, "Toggle fullscreen",            "Ctrl+Cmd+F" },
};
static const i32 g_keys_row_count = (i32)(sizeof(g_keys_rows) / sizeof(g_keys_rows[0]));

/* Shared geometry — keep render and hit-test pixel-identical. The intro line
 * sits above the first row; every row (header, rebindable, read-only) uses the
 * same pitch so the click handler can index rows without replaying styling. */
static inline f32 keys_list_row_h(f32 ch, f32 dpi)   { return ch + 13.0f * dpi; }
static inline f32 keys_list_top_gap(f32 ch, f32 dpi) { return ch + 14.0f * dpi; }

/* Offsets (added to content_top) for the scrollable content band, shared by
 * render and the click hit-test so a click lands on the row that was drawn.
 * content_y is where scrollable content starts (below the tab title + desc);
 * clip_top is the top of the scissor band rows are clipped to. */
static inline f32 settings_content_top_off(f32 ch, f32 dpi) { return 2.0f * ch + 28.0f * dpi; }
static inline f32 settings_clip_top_off(f32 ch, f32 dpi)    { return 2.0f * ch + 18.0f * dpi; }

static void settings_draw_switch(Renderer *r, f32 x, f32 y, bool on,
                                 Color active_bg, Color inactive_bg, Color border,
                                 Color active_fg, Color inactive_fg,
                                 f32 cw, f32 ch, f32 dpi) {
    f32 w = 7.0f * cw;
    f32 h = ch + 6.0f * dpi;
    Color bg = on ? active_bg : inactive_bg;
    bg.a = 1.0f;
    f32 radius = fminf(h * 0.5f, 6.0f * dpi);
    /* Border color tracks state: accent (active_fg) when On, neutral when Off.
     * Replaces the old left-edge strip — the entire 1px perimeter now carries
     * the on/off cue instead of a single edge. */
    Color stroke = on ? active_fg : border;
    s_rrect_bordered(r, x, y, w, h, bg, stroke, fmaxf(1.0f, dpi), radius);
    renderer_flush_rrects(r);
    const char *label = on ? "On" : "Off";
    Color fg = on ? active_fg : inactive_fg;
    f32 label_w = (f32)strlen(label) * cw;
    draw_label(r, label, x + (w - label_w) * 0.5f, y + (h - ch) * 0.5f, fg);
}

static void settings_draw_segmented(Renderer *r, f32 x, f32 y,
                                    const char **labels, i32 count, i32 selected,
                                    f32 seg_w, f32 h,
                                    Color active_bg, Color inactive_bg,
                                    Color border, Color active_fg, Color inactive_fg,
                                    f32 cw, f32 ch, f32 dpi) {
    f32 radius = fminf(h * 0.5f, 6.0f * dpi);
    f32 bw = fmaxf(1.0f, dpi);
    for (i32 i = 0; i < count; i++) {
        f32 sx = x + (f32)i * seg_w;
        bool on = (i == selected);
        /* Only the outermost segments get rounded corners; interior
         * segments stay flat so their shared edges abut without gaps. */
        f32 r_tl = (i == 0)          ? radius : 0.0f;
        f32 r_bl = (i == 0)          ? radius : 0.0f;
        f32 r_tr = (i == count - 1)  ? radius : 0.0f;
        f32 r_br = (i == count - 1)  ? radius : 0.0f;
        s_rrect_bordered_per_corner(r, sx, y, seg_w, h,
                                    on ? active_bg : inactive_bg,
                                    border, bw,
                                    r_tl, r_tr, r_br, r_bl);
    }
    renderer_flush_rrects(r);
    for (i32 i = 0; i < count; i++) {
        bool on = (i == selected);
        if (on) {
            f32 sx = x + (f32)i * seg_w;
            f32 r_bl = (i == 0)         ? radius : 0.0f;
            f32 r_br = (i == count - 1) ? radius : 0.0f;
            s_rrect_bordered_per_corner(r, sx, y + h - fmaxf(2.0f, 2.0f * dpi),
                                        seg_w, fmaxf(2.0f, 2.0f * dpi),
                                        active_fg, active_fg, 0.0f,
                                        0.0f, 0.0f, r_br, r_bl);
        }
    }
    renderer_flush_rrects(r);
    for (i32 i = 0; i < count; i++) {
        f32 sx = x + (f32)i * seg_w;
        bool on = (i == selected);
        f32 label_w = (f32)strlen(labels[i]) * cw;
        draw_label(r, labels[i], sx + (seg_w - label_w) * 0.5f,
                   y + (h - ch) * 0.5f, on ? active_fg : inactive_fg);
    }
}

static f32 settings_translate_lang_menu_height(f32 ch, f32 dpi) {
    i32 n = translate_language_count();
    i32 rows = (n + 2) / 3;
    return 36.0f * dpi + (f32)rows * (ch + 8.0f * dpi);
}

static void settings_draw_translate_lang_menu(SettingsPanel *sp, Renderer *r,
                                              f32 x, f32 y, f32 cw, f32 ch,
                                              f32 dpi, Color panel_bg,
                                              Color value_fg, Color dim_fg,
                                              Color accent) {
    if (!sp || sp->translate_lang_picker == SETTINGS_LANG_PICKER_CLOSED) return;

    TranslateConfig *tc = &sp->config->translate;
    const char *selected = sp->translate_lang_picker == SETTINGS_LANG_PICKER_SOURCE
        ? (tc->source_lang[0] ? tc->source_lang : "Turkish")
        : (tc->target_lang[0] ? tc->target_lang : "English");

    i32 n = translate_language_count();
    i32 rows = (n + 2) / 3;
    f32 pad = 4.0f * dpi;
    f32 header_h = 28.0f * dpi;
    f32 item_h = ch + 8.0f * dpi;
    f32 col_w = 20.0f * cw;
    f32 menu_w = col_w * 3.0f + pad * 2.0f;
    f32 menu_h = settings_translate_lang_menu_height(ch, dpi);
    Color menu_bg = {fmaxf(0, panel_bg.r - 0.035f),
                     fmaxf(0, panel_bg.g - 0.035f),
                     fmaxf(0, panel_bg.b - 0.035f), 1.0f};
    Color item_bg = {accent.r * 0.28f, accent.g * 0.28f, accent.b * 0.42f, 1.0f};
    Color action_bg = {accent.r * 0.18f, accent.g * 0.18f, accent.b * 0.26f, 1.0f};

    s_rrect_simple(r, x, y, menu_w, menu_h, menu_bg, 6.0f * dpi);
    renderer_flush_rrects(r);
    const char *title = sp->translate_lang_picker == SETTINGS_LANG_PICKER_SOURCE
        ? "Choose source language" : "Choose target language";
    draw_label(r, title, x + cw, y + 6.0f * dpi, value_fg);
    f32 reset_w = 8.0f * cw;
    f32 swap_w = 7.0f * cw;
    f32 reset_x = x + menu_w - reset_w - cw;
    f32 swap_x = reset_x - swap_w - cw;
    s_rrect_simple(r, swap_x, y + 4.0f * dpi, swap_w, ch + 8.0f * dpi,
                   action_bg, 4.0f * dpi);
    s_rrect_simple(r, reset_x, y + 4.0f * dpi, reset_w, ch + 8.0f * dpi,
                   action_bg, 4.0f * dpi);
    renderer_flush_rrects(r);
    /* Swap/Reset and the selected-language pill sit on a darkened-accent
     * fill that is dark on every theme; value_fg follows the theme text
     * colour and is dark on light themes (unreadable). Pick black/white
     * by the fill's luminance instead. */
    Color action_fg = chrome_legible_on(action_bg);
    Color item_fg   = chrome_legible_on(item_bg);
    draw_label(r, "Swap", swap_x + cw, y + 6.0f * dpi, action_fg);
    draw_label(r, "Reset", reset_x + cw, y + 6.0f * dpi, action_fg);
    for (i32 i = 0; i < n; i++) {
        i32 col = i / rows;
        i32 row = i % rows;
        f32 ix = x + pad + (f32)col * col_w;
        f32 iy = y + header_h + pad + (f32)row * item_h;
        const char *lang = translate_language_name(i);
        bool is_selected = strcmp(selected, lang) == 0;
        if (is_selected) {
            s_rrect_simple(r, ix, iy + 1.0f * dpi,
                           col_w - 4.0f * dpi, item_h - 2.0f * dpi,
                           item_bg, 4.0f * dpi);
            renderer_flush_rrects(r);
        }
        draw_label(r, lang, ix + cw, iy + 4.0f * dpi,
                   is_selected ? item_fg : dim_fg);
    }
}

/* -------------------------------------------------------------------------
 * Translate Model dropdown.
 *
 * Replaces the previous click-to-cycle chip with a real popup so the user
 * can see every model for the current agent/provider at once. The list is
 * single-column (model strings are wider than language names), each row
 * painted as a rounded chip with the active selection highlighted by an
 * accent pill behind it. "(default)" is row 0 — selecting it clears the
 * model so the CLI/provider uses whatever default it ships with.
 *
 * Live model_catalog lists can exceed the popup height, so the row strip
 * is a window of TRANSLATE_MODEL_MENU_VISIBLE rows over
 * sp->translate_model_menu_scroll; the mouse wheel scrolls it while the
 * picker is open (see settings_handle_scroll) and a thin track on the
 * right signals the overflow.
 * ------------------------------------------------------------------------- */

#define TRANSLATE_MODEL_MENU_VISIBLE 9

static i32 settings_translate_model_menu_visible_rows(i32 n) {
    if (n < 1) n = 1;
    return n > TRANSLATE_MODEL_MENU_VISIBLE ? TRANSLATE_MODEL_MENU_VISIBLE : n;
}

static i32 settings_translate_model_menu_clamp_scroll(const SettingsPanel *sp,
                                                      i32 n) {
    i32 vis = settings_translate_model_menu_visible_rows(n);
    i32 max_scroll = n - vis;
    if (max_scroll < 0) max_scroll = 0;
    i32 s = sp->translate_model_menu_scroll;
    if (s < 0) s = 0;
    if (s > max_scroll) s = max_scroll;
    return s;
}

static f32 settings_translate_model_menu_height(const TranslateConfig *tc,
                                                f32 ch, f32 dpi) {
    const char *opts[SETTINGS_MODEL_OPTS_CAP];
    i32 n = settings_translate_model_options(tc, opts,
                                             SETTINGS_MODEL_OPTS_CAP);
    i32 vis = settings_translate_model_menu_visible_rows(n);
    f32 header_h = ch + 14.0f * dpi;
    f32 item_h   = ch + 12.0f * dpi;
    f32 gap      = 4.0f * dpi;
    f32 pad      = 8.0f * dpi;
    return header_h + (f32)vis * item_h + (f32)(vis - 1) * gap + pad * 2.0f;
}

static f32 settings_translate_model_menu_width(f32 cw, f32 dpi,
                                               f32 max_w) {
    /* Roomy enough for the longest preset string + generous padding;
     * caller still clamps with the available content width. */
    f32 w = 36.0f * cw + 24.0f * dpi;
    if (w > max_w) w = max_w;
    if (w < 18.0f * cw) w = 18.0f * cw;
    return w;
}

/* Header line for the dropdown: who the list is for + where it came
 * from ("live" fetch vs the static fallback). */
static void settings_translate_model_menu_header(const TranslateConfig *tc,
                                                 char *out, usize cap) {
    const char *disp;
    const ModelList *live;
    if (tc->backend == TRANSLATE_BACKEND_API) {
        disp = translate_api_provider_display(tc->api_provider);
        live = model_catalog_api(tc->api_provider);
    } else {
        disp = "Claude";
        if (strcmp(tc->agent_id, "opencode") == 0)   disp = "OpenCode Go";
        else if (strcmp(tc->agent_id, "codex") == 0) disp = "Codex";
        else if (strcmp(tc->agent_id, "grok") == 0)  disp = "Grok";
        live = model_catalog_agent(tc->agent_id);
    }
    const char *status = "";
    if (live) {
        if (live->state == MODEL_LIST_FETCHING)      status = " — fetching…";
        else if (live->state == MODEL_LIST_READY)    status = " — live";
        else if (live->state == MODEL_LIST_FAILED) {
            status = live->count > 0 ? " — cached" : " — offline list";
        }
    }
    snprintf(out, cap, "Model for %s%s", disp, status);
}

static void settings_draw_translate_model_menu(SettingsPanel *sp, Renderer *r,
                                               f32 x, f32 y, f32 menu_w,
                                               f32 cw, f32 ch, f32 dpi,
                                               Color panel_bg, Color value_fg,
                                               Color dim_fg, Color accent) {
    if (!sp || !sp->translate_model_picker_open) return;
    TranslateConfig *tc = &sp->config->translate;
    const char *opts[SETTINGS_MODEL_OPTS_CAP];
    i32 n = settings_translate_model_options(tc, opts,
                                             SETTINGS_MODEL_OPTS_CAP);
    if (n < 1) return;
    i32 vis = settings_translate_model_menu_visible_rows(n);
    i32 scroll = settings_translate_model_menu_clamp_scroll(sp, n);
    sp->translate_model_menu_scroll = scroll;

    f32 pad      = 8.0f * dpi;
    f32 header_h = ch + 14.0f * dpi;
    f32 item_h   = ch + 12.0f * dpi;
    f32 gap      = 4.0f * dpi;
    f32 menu_h   = settings_translate_model_menu_height(tc, ch, dpi);
    f32 card_r   = 8.0f * dpi;
    f32 row_r    = 6.0f * dpi;

    /* Card. */
    Color menu_bg = {fmaxf(0, panel_bg.r - 0.035f),
                     fmaxf(0, panel_bg.g - 0.035f),
                     fmaxf(0, panel_bg.b - 0.035f), 1.0f};
    s_rrect_simple(r, x, y, menu_w, menu_h, menu_bg, card_r);
    renderer_flush_rrects(r);

    /* Header. */
    char header[96];
    settings_translate_model_menu_header(tc, header, sizeof header);
    draw_label(r, header, x + pad + 4.0f * dpi,
               y + (header_h - ch) * 0.5f, dim_fg);

    /* Rows — a vis-row window starting at `scroll`. */
    const char *current = tc->backend == TRANSLATE_BACKEND_API
        ? tc->api_model : tc->agent_model;
    f32 row_y = y + header_h;
    Color row_bg = (Color){menu_bg.r * 1.20f, menu_bg.g * 1.20f,
                           menu_bg.b * 1.20f, 1.0f};
    Color selected_bg = {accent.r * 0.30f, accent.g * 0.30f,
                         accent.b * 0.50f, 1.0f};
    for (i32 vi = 0; vi < vis; vi++) {
        i32 i = scroll + vi;
        if (i >= n) break;
        bool selected = (strcmp(current, opts[i]) == 0);
        f32 rx = x + pad;
        f32 ry = row_y + (f32)vi * (item_h + gap);
        f32 rw = menu_w - pad * 2.0f;
        if (n > vis) rw -= 6.0f * dpi;   /* room for the scroll track */
        /* Selected row gets a 1px accent border around the full perimeter;
         * unselected rows stay borderless. The previous design used a 2px
         * accent strip on the left edge only — replaced for visual
         * consistency with the rest of Settings (toggle pills, etc.). */
        if (selected) {
            s_rrect_bordered(r, rx, ry, rw, item_h, selected_bg, accent,
                             fmaxf(1.0f, dpi), row_r);
        } else {
            s_rrect_simple(r, rx, ry, rw, item_h, row_bg, row_r);
        }
        renderer_flush_rrects(r);
        const char *label = opts[i][0] ? opts[i] : "(default)";
        /* Selected row sits on a darkened-accent fill (dark on every
         * theme); pick a legible text colour from the fill rather than the
         * theme's value_fg, which is dark on light themes. */
        Color fg = selected ? chrome_legible_on(selected_bg)
                            : (opts[i][0] ? value_fg : dim_fg);
        i32 max_chars = (i32)((rw - 24.0f * dpi) / cw);
        if (max_chars < 1) max_chars = 1;
        draw_label_limited(r, label, rx + 14.0f * dpi,
                           ry + (item_h - ch) * 0.5f, fg, max_chars);
    }

    /* Overflow scrollbar — proportional thumb on a thin right-edge track. */
    if (n > vis) {
        f32 track_x = x + menu_w - pad - 3.0f * dpi;
        f32 track_y = row_y;
        f32 track_h = (f32)vis * item_h + (f32)(vis - 1) * gap;
        Color track = dim_fg;  track.a = 0.18f;
        Color thumb = dim_fg;  thumb.a = 0.55f;
        s_rect(r, track_x, track_y, 3.0f * dpi, track_h, track);
        f32 thumb_h = track_h * (f32)vis / (f32)n;
        if (thumb_h < 12.0f * dpi) thumb_h = 12.0f * dpi;
        f32 thumb_y = track_y + (track_h - thumb_h) *
                      ((f32)scroll / (f32)(n - vis));
        s_rect(r, track_x, thumb_y, 3.0f * dpi, thumb_h, thumb);
    }
}

/* Returns the absolute option index under (x, y) within the model menu
 * (scroll already applied), or -1 if the click landed in chrome /
 * outside the rows. */
static i32 settings_translate_model_menu_hit(SettingsPanel *sp,
                                             f32 x, f32 y, f32 menu_x,
                                             f32 menu_y, f32 ch, f32 dpi) {
    if (!sp || !sp->translate_model_picker_open) return -1;
    const char *opts[SETTINGS_MODEL_OPTS_CAP];
    i32 n = settings_translate_model_options(&sp->config->translate, opts,
                                             SETTINGS_MODEL_OPTS_CAP);
    if (n < 1) return -1;
    i32 vis = settings_translate_model_menu_visible_rows(n);
    i32 scroll = settings_translate_model_menu_clamp_scroll(sp, n);
    f32 pad      = 8.0f * dpi;
    f32 header_h = ch + 14.0f * dpi;
    f32 item_h   = ch + 12.0f * dpi;
    f32 gap      = 4.0f * dpi;
    f32 local_y  = y - menu_y - header_h;
    if (local_y < 0.0f) return -1;
    /* Floor-divide accounting for the inter-row gap. */
    for (i32 vi = 0; vi < vis; vi++) {
        f32 row_y = (f32)vi * (item_h + gap);
        if (local_y >= row_y && local_y < row_y + item_h) {
            f32 local_x = x - menu_x - pad;
            if (local_x < 0.0f) return -1;
            i32 i = scroll + vi;
            return i < n ? i : -1;
        }
    }
    (void)pad;
    return -1;
}

static i32 settings_translate_lang_menu_hit(f32 x, f32 y, f32 menu_x,
                                            f32 menu_y, f32 cw, f32 ch,
                                            f32 dpi) {
    i32 n = translate_language_count();
    i32 rows = (n + 2) / 3;
    f32 pad = 4.0f * dpi;
    f32 header_h = 28.0f * dpi;
    f32 item_h = ch + 8.0f * dpi;
    f32 col_w = 20.0f * cw;
    f32 menu_w = col_w * 3.0f + pad * 2.0f;
    f32 local_header_x = x - menu_x;
    f32 local_header_y = y - menu_y;
    if (local_header_y >= 4.0f * dpi &&
        local_header_y < 4.0f * dpi + ch + 8.0f * dpi) {
        f32 reset_w = 8.0f * cw;
        f32 swap_w = 7.0f * cw;
        f32 reset_x = menu_w - reset_w - cw;
        f32 swap_x = reset_x - swap_w - cw;
        if (local_header_x >= swap_x && local_header_x < swap_x + swap_w) {
            return SETTINGS_LANG_MENU_ACTION_SWAP;
        }
        if (local_header_x >= reset_x && local_header_x < reset_x + reset_w) {
            return SETTINGS_LANG_MENU_ACTION_RESET;
        }
    }
    f32 local_x = x - menu_x - pad;
    f32 local_y = y - menu_y - header_h - pad;
    if (local_x < 0 || local_y < 0) return -1;
    i32 col = (i32)(local_x / col_w);
    i32 row = (i32)(local_y / item_h);
    if (col < 0 || col >= 3 || row < 0 || row >= rows) return -1;
    if (local_x - (f32)col * col_w > col_w - 4.0f * dpi) return -1;
    i32 idx = col * rows + row;
    return idx >= 0 && idx < n ? idx : -1;
}

void settings_render(SettingsPanel *sp, Renderer *r, f32 sw, f32 sh, f32 dpi) {
    /* Edge-detection state for open + close transitions. The close path
     * keeps rendering after sp->open flips back to false until the close
     * animation drains, so dismissals fade out smoothly. */
    static bool s_settings_was_open = false;
    static Anim s_settings_open_anim  = {0};
    static Anim s_settings_close_anim = {0};
    f32 panel_scale, panel_alpha, panel_yoff;
    bool render_settings = modal_anim_progress(
        sp->open,
        &s_settings_open_anim, &s_settings_close_anim,
        &s_settings_was_open, dpi, MODAL_OPEN_DUR_LARGE,
        &panel_scale, &panel_alpha, &panel_yoff);
    if (!render_settings) return;
    s_panel_alpha = panel_alpha;

    /* Hover registry is per-frame state: clear on entry, then draw sites
     * push their rectangles as they're laid out. settings_point_clickable
     * walks this list to drive the pointing-hand cursor in main.c. */
    sp->hover_area_count = 0;

    /* Fixed UI scale — settings panel font never changes with terminal font size */
    f32 cw = 8.0f * dpi;
    f32 ch = 16.0f * dpi;
    renderer_set_ui_scale(r, cw, ch);
    s_ui_cw = cw;

    /* Theme-derived colors for the settings panel.
     *
     * Container chrome (panel, title bar, sidebar) all match the active
     * terminal theme's `bg` so the modal feels like part of the same
     * surface. The single "interactive" tone — used for the active nav
     * pill, the title bar lift, and the panel border — is theme.border,
     * which is #141416 (and the analogous one-step brighter shade on
     * every other built-in theme). No chrome-palette accent blend here:
     * that's what previously made the sidebar read as a separate bluish
     * surface. */
    const Theme *theme = sp->config->theme;
    ChromePalette cp   = chrome_palette_for(theme);
    Color panel_bg, title_bg, tab_inactive_bg, accent;
    Color text_fg, text_dim, border_clr;
    if (theme) {
        panel_bg        = theme->bg;
        title_bg        = theme->bg;
        tab_inactive_bg = theme->bg;
        text_fg    = theme->fg;
        text_dim   = (Color){theme->fg.r * 0.6f, theme->fg.g * 0.6f,
                             theme->fg.b * 0.6f, 1.0f};
        border_clr = theme->border;
        border_clr.a = (border_clr.a > 0.05f) ? border_clr.a : 1.0f;
        /* `accent` still drives intra-content buttons / focus rings —
         * kept here so the settings content itself doesn't go monochrome,
         * just the sidebar shell. */
        accent     = cp.btn_primary_bg;
        accent.a   = 0.92f;
    } else {
        panel_bg        = (Color){0.04f, 0.04f, 0.055f, 1.0f};
        title_bg        = panel_bg;
        tab_inactive_bg = panel_bg;
        text_fg         = (Color){0.9f, 0.9f, 0.92f, 1.0f};
        text_dim        = (Color){0.55f, 0.55f, 0.58f, 1.0f};
        border_clr      = (Color){0.078f, 0.078f, 0.086f, 1.0f};
        accent          = cp.btn_primary_bg;
    }
    /* Active nav row = the same single accent shade as borders so the
     * sidebar uses just two tones (panel_bg + border_clr). Visually flat
     * and theme-matching. The thin left accent strip uses theme.cursor
     * with low alpha so it still hints at a focus indicator without
     * adding a third hue. */
    Color nav_active_bg = border_clr;
    nav_active_bg.a = 1.0f;
    Color nav_accent = theme ? theme->cursor : (Color){0.78f, 0.78f, 0.82f, 1.0f};
    nav_accent.a = 0.55f;

    SettingsLayout sl = settings_layout_for(sw, sh, dpi, panel_scale, panel_yoff);
    f32 px = sl.px;
    f32 py = sl.py;
    f32 panel_w = sl.panel_w;
    f32 panel_h = sl.panel_h;

    /* Dimmed overlay — fades in with the panel. */
    s_rect(r, 0, 0, sw, sh, (Color){0, 0, 0, 0.5f});
    renderer_flush_rects(r);

    /* Panel shell: rounded body + soft drop shadow. One bordered rrect
     * draws the entire outline + fill in a single SDF instance so the
     * corner anti-aliasing is consistent. */
    f32 panel_radius = 12.0f * dpi;
    s_rrect_bordered_shadow(r,
        px, py, panel_w, panel_h,
        panel_bg, border_clr, fmaxf(1.0f, dpi),
        panel_radius, panel_radius, panel_radius, panel_radius,
        24.0f * dpi, 0.35f, 0.0f, 6.0f * dpi);
    renderer_flush_rrects(r);

    f32 title_h = sl.title_h;
    f32 nav_w = sl.nav_w;
    /* Subtle 1-pixel divider between title bar and body. The body is
     * already a single colour so we don't need a filled title strip —
     * just the divider line keeps the header visually anchored. */
    s_rect(r, px + panel_radius * 0.5f, py + title_h - fmaxf(1.0f, dpi),
           panel_w - panel_radius, fmaxf(1.0f, dpi), cp.divider_subtle);
    /* Sidebar / content divider — a hairline, since both surfaces share
     * the same bg now. Inset from top/bottom so it doesn't visibly
     * touch the rounded corners. */
    s_rect(r, px + nav_w, py + title_h + 4.0f * dpi,
           fmaxf(1.0f, dpi),
           panel_h - title_h - 8.0f * dpi - panel_radius * 0.5f,
           cp.divider_subtle);
    renderer_flush_rects(r);
    (void)title_bg;
    (void)tab_inactive_bg;
    draw_label(r, "Settings", px + 14.0f * dpi, py + (title_h - ch) / 2, text_fg);
    draw_label(r, "Esc", px + panel_w - 6.0f * cw, py + (title_h - ch) / 2,
               text_dim);
    renderer_flush_glyphs(r);

    /* Left navigation
     *
     * Active row is an inset pill: 6dpi gutter on each side so the
     * selection looks like a card rather than a wall-to-wall stripe
     * butting against the panel edge. The accent bar sits INSIDE the
     * pill flush with its left edge, so it never touches the label.
     * Text is vertically centred to the row. */
    f32 nav_y = py + title_h + 8.0f * dpi;
    f32 nav_row_h = sl.nav_row_h;
    f32 row_gutter = 6.0f * dpi;
    f32 row_inset_top = 2.0f * dpi;
    f32 row_x = px + row_gutter;
    f32 row_w = nav_w - 2.0f * row_gutter;
    if (row_w < 8.0f * dpi) {
        row_x = px;
        row_w = nav_w;
        row_gutter = 0.0f;
    }
    f32 accent_w = fmaxf(2.0f, 2.0f * dpi);
    f32 text_left_pad = 14.0f * dpi;
    f32 pill_radius = 6.0f * dpi;
    for (i32 i = 0; i < SETTINGS_TAB_COUNT; i++) {
        bool active = (i == (i32)sp->active_tab);
        f32 ny = nav_y + (f32)i * nav_row_h;
        if (active) {
            f32 pill_y = ny + row_inset_top;
            f32 pill_h = nav_row_h - 2.0f * row_inset_top;
            if (pill_h < 2.0f * dpi) pill_h = nav_row_h;
            /* Rounded pill so the selection reads as a card instead of a
             * hard-edge rectangle. The accent strip rides the pill's left
             * radius so it doesn't poke past the rounded corners. */
            s_rrect_simple(r, row_x, pill_y, row_w, pill_h,
                           nav_active_bg, pill_radius);
            s_rrect_per_corner(r, row_x, pill_y, accent_w, pill_h,
                               nav_accent,
                               pill_radius, 0.0f, 0.0f, pill_radius);
        }
    }
    renderer_flush_rrects(r);
    renderer_flush_rects(r);
    for (i32 i = 0; i < SETTINGS_TAB_COUNT; i++) {
        bool active = (i == (i32)sp->active_tab);
        Color fg = active ? text_fg : text_dim;
        f32 ny = nav_y + (f32)i * nav_row_h;
        i32 nav_max = (i32)((row_w - text_left_pad - 6.0f * dpi) / cw);
        if (nav_max < 1) nav_max = 1;
        draw_label_limited(r, settings_tab_name((SettingsTab)i),
                           row_x + text_left_pad,
                           ny + (nav_row_h - ch) * 0.5f,
                           fg, nav_max);
        /* Hover region for the cursor — clickable even when not the
         * active tab. The bounding rect covers the full row, matching
         * the hit-test in settings_handle_click. */
        sp_push_hover(sp, row_x, ny, row_w, nav_row_h);
    }
    renderer_flush_glyphs(r);

    /* Content header */
    f32 content_x = sl.content_x;
    f32 content_top = sl.content_top;
    f32 content_w = sl.content_w;
    i32 content_max = (i32)(content_w / cw);
    if (content_max < 1) content_max = 1;
    draw_label_limited(r, settings_tab_name(sp->active_tab),
                       content_x, content_top, text_fg, content_max);
    draw_label_limited(r, settings_tab_desc(sp->active_tab),
                       content_x, content_top + ch + 6.0f * dpi,
                       text_dim, content_max);
    renderer_flush_glyphs(r);

    /* Content area — scrollable */
    f32 scroll_off = (f32)sp->scroll_y * ch * 0.5f;
    f32 content_y = content_top + settings_content_top_off(ch, dpi) - scroll_off;
    f32 row_h = 32 * dpi;
    f32 clip_top = content_top + settings_clip_top_off(ch, dpi);
    f32 clip_bottom = sl.clip_bottom;
    renderer_push_scissor(r, content_x - 4.0f * dpi, clip_top,
                          content_w + 8.0f * dpi, clip_bottom - clip_top);
    f32 content_end_y = content_y; /* updated at end of each tab block */
    Color label_fg = text_dim;
    Color value_fg = text_fg;
    Color dim_fg   = (Color){text_dim.r * 0.7f, text_dim.g * 0.7f, text_dim.b * 0.7f, 1.0f};

    if (sp->active_tab == SETTINGS_TAB_APPEARANCE) {
        f32 y = content_y;
        f32 col2 = content_x + 14 * cw; /* value column */
        f32 item_h = ch + 10;
        i32 font_cols = content_w >= 420.0f * dpi ? 3 :
                        content_w >= 260.0f * dpi ? 2 : 1;
        f32 font_row_h = ch + 16.0f * dpi;   /* same card height as themes */
        f32 font_gap = 6.0f * dpi;
        i32 theme_cols = content_w >= 640.0f * dpi ? 3 :   /* preview cards need width; cap at 3 */
                         content_w >= 440.0f * dpi ? 3 :
                         content_w >= 260.0f * dpi ? 2 : 1;
        f32 theme_row_h = ch + 16.0f * dpi;
        f32 theme_gap = 6.0f * dpi;

        /* Font — current + selector */
        draw_label(r, "Font", content_x, y, label_fg);
        /* Show current font friendly name */
        const char *fname = sp->config->font_path;
        for (i32 fi = 0; fi < g_font_count; fi++) {
            if (strcmp(sp->config->font_path, g_fonts[fi].path) == 0) {
                fname = g_fonts[fi].name;
                break;
            }
        }
        /* Fallback: extract filename from path */
        if (fname == sp->config->font_path) {
            for (const char *fp2 = fname; *fp2; fp2++) { if (*fp2 == '/') fname = fp2 + 1; }
        }
        draw_label(r, fname, col2, y, value_fg);
        /* Visually match the Create Theme / Import Theme pill buttons under
         * the theme grid. Same height, padding and leading-glyph approach. */
        f32 add_btn_pad_x = 14;
        f32 add_btn_label_chars = 18;  /* "+  Add Custom Font" */
        f32 add_btn_w = add_btn_pad_x + add_btn_label_chars * cw + add_btn_pad_x;
        if (add_btn_w > content_w * 0.55f) add_btn_w = content_w * 0.55f;
        f32 add_btn_h = ch + 12;
        f32 add_btn_x = content_x + content_w - add_btn_w;
        f32 add_btn_y = y - (add_btn_h - ch) * 0.5f;
        s_rrect_simple(r, add_btn_x, add_btn_y, add_btn_w, add_btn_h,
                       cp.btn_secondary_bg, fminf(add_btn_h * 0.5f, 8.0f * dpi));
        renderer_flush_rrects(r);
        draw_label(r, "+  Add Custom Font",
                   add_btn_x + add_btn_pad_x,
                   add_btn_y + (add_btn_h - ch) * 0.5f,
                   cp.btn_secondary_fg);
        sp_push_hover(sp, add_btn_x, add_btn_y, add_btn_w, add_btn_h);
        y += item_h + 12.0f * dpi;

        /* Font grid — directly selectable. (No hairline: the cards carry
         * the grouping; the spacing below stays as breathing room.) */
        y += 12.0f * dpi;
        f32 col_w = content_w / (f32)font_cols;
        for (i32 fi = 0; fi < g_font_count; fi++) {
            bool sel = (strcmp(sp->config->font_path, g_fonts[fi].path) == 0);
            f32 fx = content_x + (f32)(fi % font_cols) * col_w;
            f32 fy = y + (f32)(fi / font_cols) * (font_row_h + font_gap);

            /* Card geometry — byte-for-byte the theme grid's. */
            f32 pill_x = fx - 2.0f * dpi;
            f32 pill_y = fy - 4.0f * dpi;
            f32 pill_w = col_w - 8.0f * dpi;
            f32 pill_h = font_row_h;
            f32 radius = fminf(pill_h * 0.5f, 6.0f * dpi);
            if (sel) {
                Color sel_fill = cp.btn_primary_bg;
                sel_fill.a = 0.18f;   /* quiet selection: thin ring carries the state */
                s_rrect_bordered(r, pill_x, pill_y, pill_w, pill_h,
                                 sel_fill, accent, fmaxf(1.0f, dpi), radius);
            } else {
                Color tile_bg = cp.surface_sunken; tile_bg.a = 0.55f;
                s_rrect_bordered(r, pill_x, pill_y, pill_w, pill_h,
                                 tile_bg, cp.divider_subtle,
                                 fmaxf(1.0f, dpi), radius);
            }
            renderer_flush_rrects(r);
            Color fc = sel ? accent : value_fg;
            f32 fty = pill_y + (pill_h - ch) * 0.5f;
            draw_label(r, g_fonts[fi].name, fx + 10.0f * dpi, fty, fc);
            sp_push_hover(sp, pill_x, pill_y, pill_w, pill_h);
        }
        y += ((g_font_count + font_cols - 1) / font_cols) *
             (font_row_h + font_gap) + 16.0f * dpi;

        y += 10;

        /* Size / Line Height — Zed-style segmented steppers. The - and +
         * glyphs stay at their historical x positions (col2 + 7cw / 9cw) so
         * settings_handle_click's zones keep matching; the bordered pill
         * around them is pure chrome. */
        #define APPEAR_STEPPER(label_, valfmt_, val_)  do {                     \
            draw_label(r, (label_), content_x, y, label_fg);                    \
            char _b[32];                                                        \
            snprintf(_b, sizeof _b, (valfmt_), (val_));                         \
            draw_label(r, _b, col2, y, value_fg);                               \
            f32 _sx = col2 + 6.5f * cw;                                         \
            f32 _sw = 6.0f * cw;       /* two roomy halves */                    \
            f32 _sy = y - 3.0f * dpi;                                           \
            f32 _sh = ch + 6.0f * dpi;                                          \
            Color _seg_bg = cp.surface_sunken; _seg_bg.a = 1.0f;                \
            s_rrect_bordered(r, _sx, _sy, _sw, _sh, _seg_bg,                    \
                             cp.divider_strong, fmaxf(1.0f, dpi),               \
                             fminf(_sh * 0.5f, 6.0f * dpi));                    \
            renderer_flush_rrects(r);                                           \
            s_rect(r, _sx + _sw * 0.5f, _sy + 3.0f * dpi,                       \
                   fmaxf(1.0f, dpi), _sh - 6.0f * dpi, cp.divider_subtle);      \
            /* glyphs centered inside their halves */                           \
            draw_label(r, "-", _sx + _sw * 0.25f - cw * 0.5f, y, accent);       \
            draw_label(r, "+", _sx + _sw * 0.75f - cw * 0.5f, y, accent);       \
        } while (0)

        char buf[32];
        APPEAR_STEPPER("Size", "%.0f pt", sp->config->font_size);
        y += item_h + 6.0f * dpi;   /* air between the two stepper boxes */

        APPEAR_STEPPER("Line Height", "%.2f", sp->config->cell_height_scale);
        y += item_h + 8;
        #undef APPEAR_STEPPER
        (void)buf;

        y += 10;

        /* Theme — compact grid. User themes (index >= THEME_COUNT) get a
         * trailing close-icon button that deletes the on-disk JSON.
         * Built-ins have no delete affordance — they live in static
         * const storage and theme_delete_user refuses to touch names
         * not in the user-themes cache. */
        draw_label(r, "Theme", content_x, y, label_fg);
        y += ch + 16;

        i32 theme_count;
        const char **theme_names = theme_list_names(&theme_count);
        f32 tcol_w = content_w / (f32)theme_cols;
        /* Delete button — sized in *characters* (one cell square) so it
         * matches the text height, not the row height. Earlier we tied
         * it to row_h which made it visually overpower the text. */
        f32 del_btn_sz = ch + 4.0f * dpi;
        f32 del_btn_pad_right = 8.0f * dpi;

        for (i32 i = 0; i < theme_count; i++) {
            bool selected = (strcmp(sp->config->theme_name, theme_names[i]) == 0);
            bool is_user_theme = (i >= THEME_COUNT);
            f32 tx2 = content_x + (f32)(i % theme_cols) * tcol_w;
            f32 ty2 = y + (f32)(i / theme_cols) * (theme_row_h + theme_gap);

            f32 pill_x = tx2 - 2.0f * dpi;
            f32 pill_w = tcol_w - 8.0f * dpi;
            f32 pill_y = ty2 - 4.0f * dpi;
            f32 pill_h = theme_row_h;
            f32 radius = fminf(pill_h * 0.5f, 6.0f * dpi);
            /* Every tile is a quiet card (Zed: lists read as contained
             * controls); selection swaps the hairline for an accent ring
             * over a faint accent wash. */
            if (selected) {
                Color sel_fill = cp.btn_primary_bg;
                sel_fill.a = 0.18f;
                s_rrect_bordered(r, pill_x, pill_y, pill_w, pill_h,
                                 sel_fill, accent, fmaxf(1.0f, dpi), radius);
            } else {
                Color tile_bg = cp.surface_sunken; tile_bg.a = 0.55f;
                s_rrect_bordered(r, pill_x, pill_y, pill_w, pill_h,
                                 tile_bg, cp.divider_subtle,
                                 fmaxf(1.0f, dpi), radius);
            }
            renderer_flush_rrects(r);

            f32 prev_x = tx2 + 10.0f * dpi;
            f32 text_y = pill_y + (pill_h - ch) * 0.5f;
            Color tc = selected ? cp.btn_primary_bg : value_fg;
            /* Clamp the name inside the pill: stop before the delete button
             * (user themes) or the right padding, with an ellipsis when cut. */
            {
                f32 name_right = pill_x + pill_w -
                                 (is_user_theme ? del_btn_sz + del_btn_pad_right + 4.0f * dpi
                                                : 8.0f * dpi);
                i32 name_max = (i32)((name_right - prev_x) / cw);
                if (name_max < 1) name_max = 1;
                char nbuf[64];
                snprintf(nbuf, sizeof nbuf, "%s", theme_names[i]);
                if ((i32)strlen(nbuf) > name_max) {
                    if (name_max >= 2) {
                        nbuf[name_max - 1] = '\0';
                        /* draw_label is byte-wise; ASCII theme names make the
                         * cut safe. Append a midline ellipsis glyph. */
                        strncat(nbuf, "\xe2\x80\xa6", sizeof nbuf - strlen(nbuf) - 1);
                    } else {
                        nbuf[name_max] = '\0';
                    }
                }
                draw_label(r, nbuf, prev_x, text_y, tc);
            }

            if (is_user_theme) {
                /* Delete button — small rounded square anchored to the
                 * pill's right edge. Subtle red tint so it reads as a
                 * destructive action without screaming. Renders the X
                 * with the terminal font directly to avoid the SF Symbol
                 * cache flicker we saw at small sizes — the icon button
                 * is now a plain glyph in a tinted pill. */
                f32 bx = pill_x + pill_w - del_btn_sz - del_btn_pad_right;
                f32 by = pill_y + (pill_h - del_btn_sz) * 0.5f;
                Color del_bg = cp.btn_destructive_bg;
                del_bg.a = selected ? 0.24f : 0.14f;
                Color del_fg = cp.btn_destructive_fg;
                s_rrect_simple(r, bx, by, del_btn_sz, del_btn_sz,
                               del_bg, fminf(del_btn_sz * 0.5f, 6.0f * dpi));
                renderer_flush_rrects(r);
                /* "×" U+00D7 multiplication sign — visually balanced and
                 * present in every monospace font (unlike a literal "x"
                 * which reads as text noise). */
                f32 gx = bx + (del_btn_sz - cw) * 0.5f;
                f32 gy = by + (del_btn_sz - ch) * 0.5f;
                draw_label(r, "\xc3\x97", gx, gy, del_fg);
            }
            sp_push_hover(sp, pill_x, pill_y, pill_w, pill_h);
        }
        f32 grid_end_y = y + ((theme_count + theme_cols - 1) / theme_cols) *
                         (theme_row_h + theme_gap) + 16.0f * dpi;

        /* "Create / Import" action buttons under the grid. Pill-shaped
         * with leading glyphs (drawn through the text path) so the
         * icons stay rasterised identically with the rest of the UI —
         * SF Symbol caching at sub-pixel sizes was making the icons
         * flicker between + and ↑ when the modal animation tweaked
         * the layout. */
        f32 btn_h = ch + 12;
        f32 btn_pad_x = 14;
        f32 btn_gap = 8;
        f32 create_label_chars = 15;  /* "+  Create Theme" */
        f32 import_label_chars = 15;  /* "↑  Import Theme" */
        f32 create_w = btn_pad_x + create_label_chars * cw + btn_pad_x;
        f32 import_w = btn_pad_x + import_label_chars * cw + btn_pad_x;
        if (create_w + btn_gap + import_w > content_w) {
            create_w = (content_w - btn_gap) * 0.5f;
            import_w = create_w;
            if (create_w < 1.0f) create_w = import_w = 1.0f;
        }

        f32 theme_btn_radius = fminf(btn_h * 0.5f, 8.0f * dpi);
        s_rrect_simple(r, content_x, grid_end_y, create_w, btn_h,
                       cp.btn_primary_bg, theme_btn_radius);
        f32 import_x = content_x + create_w + btn_gap;
        s_rrect_simple(r, import_x, grid_end_y, import_w, btn_h,
                       cp.btn_secondary_bg, theme_btn_radius);
        renderer_flush_rrects(r);
        draw_label(r, "+  Create Theme",
                   content_x + btn_pad_x,
                   grid_end_y + (btn_h - ch) * 0.5f,
                   cp.btn_primary_fg);
        /* "↑" U+2191 — same kind of typographic glyph, no SF Symbol path. */
        draw_label(r, "\xe2\x86\x91  Import Theme",
                   import_x + btn_pad_x,
                   grid_end_y + (btn_h - ch) * 0.5f,
                   cp.btn_secondary_fg);
        sp_push_hover(sp, content_x, grid_end_y, create_w, btn_h);
        sp_push_hover(sp, import_x,  grid_end_y, import_w, btn_h);

        content_end_y = grid_end_y + btn_h + 18;

    } else if (sp->active_tab == SETTINGS_TAB_TERMINAL) {
        f32 y = content_y;
        f32 value_off = fminf(18.0f * cw, content_w * 0.42f);
        if (value_off > content_w - 8.0f * cw) value_off = content_w - 8.0f * cw;
        if (value_off < 0.0f) value_off = 0.0f;
        f32 value_x = content_x + value_off;
        f32 value_room = content_x + content_w - value_x;
        f32 control_h = ch + 8.0f * dpi;
        Color control_bg = settings_blend(panel_bg, text_fg, cp.is_light ? 0.05f : 0.08f);
        control_bg.a = 1.0f;
        Color control_active = settings_blend(control_bg, accent, cp.is_light ? 0.24f : 0.34f);
        control_active.a = 1.0f;
        Color control_border = settings_blend(panel_bg, text_fg, cp.is_light ? 0.16f : 0.20f);
        control_border.a = 0.68f;
        Color section_rule = control_border;
        section_rule.a = 0.42f;

        draw_label(r, "CURSOR", content_x, y, dim_fg);
        f32 cursor_rule_x = content_x + fminf(8.0f * cw, content_w * 0.45f);
        f32 cursor_rule_w = content_x + content_w - cursor_rule_x;
        if (cursor_rule_w > 0.0f) {
            s_rect(r, cursor_rule_x, y + ch * 0.65f,
                   cursor_rule_w, fmaxf(1.0f, dpi), section_rule);
        }
        y += ch + 12.0f * dpi;

        const char *cursor_names[] = {"Block", "Underline", "Bar"};
        f32 cursor_seg_w = 10.0f * cw;
        if (value_room > 0.0f && cursor_seg_w * 3.0f > value_room) {
            cursor_seg_w = value_room / 3.0f;
        }
        if (cursor_seg_w < 1.0f) cursor_seg_w = 1.0f;
        draw_label(r, "Style:", content_x, y, label_fg);
        settings_draw_segmented(r, value_x, y - 4.0f * dpi,
                                cursor_names, 3, sp->config->cursor_style % 3,
                                cursor_seg_w, control_h,
                                control_active, control_bg,
                                control_border, value_fg,
                                value_fg, cw, ch, dpi);
        y += row_h;

        draw_label(r, "Blink:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->cursor_blink, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        y += 12.0f * dpi;
        draw_label(r, "BEHAVIOR", content_x, y, dim_fg);
        f32 behavior_rule_x = content_x + fminf(10.0f * cw, content_w * 0.50f);
        f32 behavior_rule_w = content_x + content_w - behavior_rule_x;
        if (behavior_rule_w > 0.0f) {
            s_rect(r, behavior_rule_x, y + ch * 0.65f,
                   behavior_rule_w, fmaxf(1.0f, dpi), section_rule);
        }
        y += ch + 12.0f * dpi;

        char buf[32];
        snprintf(buf, sizeof(buf), "%d lines", sp->config->scrollback_lines);
        draw_label(r, "Scrollback:", content_x, y, label_fg);
        draw_value(r, buf, value_x, y, value_fg);
        y += row_h;

        draw_label(r, "Bold Bright:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->bold_is_bright, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label(r, "Copy Select:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->copy_on_select, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label(r, "Option=Alt:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->option_as_alt, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label(r, "Tab Bar:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->show_tab_bar, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label(r, "Toolbar Icons:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->show_toolbar_icons, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label(r, "Scrollbar:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->show_scrollbar, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label(r, "Status Bar:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->show_status_bar, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* Ask before closing a tab whose PTY runs an AI agent CLI. */
        draw_label(r, "Close Guard:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->confirm_close_agent, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* Tab Sleep: preset chips row (rounded outer corners). */
        draw_label(r, "Tab Sleep:", content_x, y, label_fg);
        {
            static const f32 ts_presets[] = {0.0f, 5.0f, 10.0f, 20.0f, 30.0f, 60.0f};
            static const char *ts_labels[] = {"Off", "5m", "10m", "20m", "30m", "60m"};
            const i32 n = (i32)(sizeof(ts_presets) / sizeof(ts_presets[0]));
            f32 chip_x = value_x;
            f32 chip_y = y - 4.0f * dpi;
            f32 chip_h = control_h;
            f32 chip_w = 5.0f * cw;
            if (value_room > 0.0f && chip_w * (f32)n > value_room) {
                chip_w = value_room / (f32)n;
            }
            if (chip_w < 1.0f) chip_w = 1.0f;
            f32 radius = fminf(chip_h * 0.5f, 6.0f * dpi);
            f32 bw = fmaxf(1.0f, dpi);
            for (i32 pi = 0; pi < n; pi++) {
                bool selected = fabsf(sp->config->tab_sleep_idle_minutes - ts_presets[pi]) < 0.5f;
                Color bg = selected ? control_active : control_bg;
                f32 r_tl = (pi == 0)     ? radius : 0.0f;
                f32 r_bl = (pi == 0)     ? radius : 0.0f;
                f32 r_tr = (pi == n - 1) ? radius : 0.0f;
                f32 r_br = (pi == n - 1) ? radius : 0.0f;
                s_rrect_bordered_per_corner(r, chip_x, chip_y, chip_w, chip_h,
                                            bg, control_border, bw,
                                            r_tl, r_tr, r_br, r_bl);
                chip_x += chip_w;
            }
            renderer_flush_rrects(r);
            chip_x = value_x;
            for (i32 pi = 0; pi < n; pi++) {
                bool selected = fabsf(sp->config->tab_sleep_idle_minutes - ts_presets[pi]) < 0.5f;
                if (selected) {
                    f32 r_bl = (pi == 0)     ? radius : 0.0f;
                    f32 r_br = (pi == n - 1) ? radius : 0.0f;
                    s_rrect_bordered_per_corner(r,
                        chip_x, chip_y + chip_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f,
                        0.0f, 0.0f, r_br, r_bl);
                }
                chip_x += chip_w;
            }
            renderer_flush_rrects(r);
            chip_x = value_x;
            for (i32 pi = 0; pi < n; pi++) {
                bool selected = fabsf(sp->config->tab_sleep_idle_minutes - ts_presets[pi]) < 0.5f;
                Color tc = selected ? value_fg : label_fg;
                f32 label_w = strlen(ts_labels[pi]) * cw;
                f32 tx = chip_x + (chip_w - label_w) * 0.5f;
                draw_label(r, ts_labels[pi], tx, chip_y + (chip_h - ch) * 0.5f, tc);
                chip_x += chip_w;
            }
        }
        y += row_h;
        /* Helper caption explaining the setting */
        draw_label(r, "Inactive tabs auto-sleep after idle.",
                   value_x, y, dim_fg);
        y += row_h;

        draw_label(r, "Opacity:", content_x, y, label_fg);
        if (sp->editing_opacity) {
            f32 inp_x = value_x;
            f32 inp_w = 8 * cw + 8.0f * dpi;
            Color inp_bg = control_bg;
            f32 radius = fminf(control_h * 0.5f, 6.0f * dpi);
            s_rrect_bordered(r, inp_x - 4.0f, y - 4.0f * dpi, inp_w, control_h,
                             inp_bg, control_border, fmaxf(1.0f, dpi), radius);
            s_rrect_bordered_per_corner(r,
                inp_x - 4.0f, y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                inp_w, fmaxf(2.0f, 2.0f * dpi),
                accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
            renderer_flush_rrects(r);
            draw_value(r, sp->opacity_buf, inp_x, y, value_fg);
            /* Cursor */
            s_rect(r, inp_x + sp->opacity_buf_len * cw, y, 1, ch,
                              (Color){text_fg.r, text_fg.g, text_fg.b, 0.8f});
            draw_label(r, "%", inp_x + (sp->opacity_buf_len + 1) * cw, y, dim_fg);
        } else {
            snprintf(buf, sizeof(buf), "%.0f%%", sp->config->opacity * 100);
            draw_value(r, buf, value_x, y, value_fg);
        }
        content_end_y = y + ch + 16;

    } else if (sp->active_tab == SETTINGS_TAB_TRANSLATE) {
        TranslateConfig *tc = &sp->config->translate;
        translate_normalize_direction(tc);
        f32 y = content_y;

        /* Mirror Terminal tab geometry so segmented controls, switches
         * and chip rows align identically: value column, control sizes,
         * section rules, and chip-row backgrounds are the same. */
        f32 value_off = fminf(18.0f * cw, content_w * 0.42f);
        if (value_off > content_w - 8.0f * cw) value_off = content_w - 8.0f * cw;
        if (value_off < 0.0f) value_off = 0.0f;
        f32 value_x = content_x + value_off;
        f32 value_room = content_x + content_w - value_x;
        f32 control_h = ch + 8.0f * dpi;
        Color control_bg = settings_blend(panel_bg, text_fg, cp.is_light ? 0.05f : 0.08f);
        control_bg.a = 1.0f;
        Color control_active = settings_blend(control_bg, accent, cp.is_light ? 0.24f : 0.34f);
        control_active.a = 1.0f;
        Color control_border = settings_blend(panel_bg, text_fg, cp.is_light ? 0.16f : 0.20f);
        control_border.a = 0.68f;
        Color section_rule = control_border;
        section_rule.a = 0.42f;

        /* —— TRANSLATE-ON-TAB section —— */
        draw_label(r, "TRANSLATE-ON-TAB", content_x, y, dim_fg);
        {
            f32 rule_x = content_x + fminf(18.0f * cw, content_w * 0.55f);
            f32 rule_w = content_x + content_w - rule_x;
            if (rule_w > 0.0f) {
                s_rect(r, rule_x, y + ch * 0.65f,
                       rule_w, fmaxf(1.0f, dpi), section_rule);
            }
        }
        y += ch + 12.0f * dpi;

        /* Enable → switch (matches Terminal Blink / Bold Bright / etc.) */
        draw_label(r, "Enable:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             tc->enabled, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* Kick the async model fetch for whatever the dropdown would show
         * — internally throttled (TTL / in-flight / retry window), so the
         * per-frame call is cheap and the list is warm by the time the
         * user opens the picker. */
        if (tc->backend == TRANSLATE_BACKEND_AGENT) {
            model_catalog_request_agent(tc->agent_id);
        } else if (tc->backend == TRANSLATE_BACKEND_API) {
            char key[256];
            translate_api_effective_key(tc, key, sizeof key);
            model_catalog_request_api(tc->api_provider, key,
                                      tc->api_base_url);
        }

        /* Backend → segmented (matches Terminal Cursor Style). Labels are
         * kept short ("Agent"/"Local" not "Agent CLI"/"Local model") so
         * they never overflow the 3-up segment width — draw_label doesn't
         * clip, and the "Backend:" caption makes them unambiguous. */
        {
            const char *names[] = {"Agent", "Local", "API"};
            i32 sel = (tc->backend == TRANSLATE_BACKEND_LOCAL) ? 1
                    : (tc->backend == TRANSLATE_BACKEND_API)   ? 2 : 0;
            f32 seg_w = 10.0f * cw;
            if (value_room > 0.0f && seg_w * 3.0f > value_room) {
                seg_w = value_room / 3.0f;
            }
            if (seg_w < 1.0f) seg_w = 1.0f;
            draw_label(r, "Backend:", content_x, y, label_fg);
            settings_draw_segmented(r, value_x, y - 4.0f * dpi,
                                    names, 3, sel,
                                    seg_w, control_h,
                                    control_active, control_bg,
                                    control_border, value_fg,
                                    value_fg, cw, ch, dpi);
        }
        y += row_h;

        /* Agent + Model rows are only meaningful when Backend = Agent CLI;
         * under Local both the picker and the preset chip would be dead
         * controls (tc->agent_id / agent_model aren't read by the local
         * inference path). Hide the whole pair and let the rows below
         * float up — the API backend swaps in its own row set instead.
         * The Local GGUF row below stays visible in every mode because
         * the path is still configurable. */
        if (tc->backend == TRANSLATE_BACKEND_AGENT) {
            /* Agent → 4-segment picker (Claude / OpenCode / Codex / Grok).
             * Drives tc->agent_id when Backend = Agent CLI. */
            draw_label(r, "Agent:", content_x, y, label_fg);
            {
                const char *names[] = {"Claude", "OpenCode", "Codex", "Grok"};
                const char *ids[]   = {"claude", "opencode", "codex", "grok"};
                i32 sel = 0;
                for (i32 i = 0; i < 4; i++) {
                    if (strcmp(tc->agent_id, ids[i]) == 0) { sel = i; break; }
                }
                f32 seg_w = 9.0f * cw;
                if (value_room > 0.0f && seg_w * 4.0f > value_room) {
                    seg_w = value_room / 4.0f;
                }
                if (seg_w < 1.0f) seg_w = 1.0f;
                settings_draw_segmented(r, value_x, y - 4.0f * dpi,
                                        names, 4, sel,
                                        seg_w, control_h,
                                        control_active, control_bg,
                                        control_border, value_fg,
                                        value_fg, cw, ch, dpi);
            }
            y += row_h;

            /* Model → rounded chip that cycles through presets when clicked.
             * Empty string = the CLI's own default; the renderer prints it
             * as "(default)" so the chip never looks blank. Presets depend
             * on the currently-selected agent. Clicking opens a dropdown
             * (settings_draw_translate_model_menu) listing every preset for
             * the current agent. */
            draw_label(r, "Model:", content_x, y, label_fg);
            {
                const char *disp = tc->agent_model[0] ? tc->agent_model : "(default)";
                f32 chip_w = value_room;
                if (chip_w < 12.0f * cw) chip_w = 12.0f * cw;
                f32 radius = fminf(control_h * 0.5f, 6.0f * dpi);
                bool picker_open = sp->translate_model_picker_open;
                Color chip_bg = picker_open ? control_active : control_bg;
                s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w, control_h,
                                 chip_bg, control_border,
                                 fmaxf(1.0f, dpi), radius);
                if (picker_open) {
                    s_rrect_bordered_per_corner(r, value_x,
                        y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
                }
                renderer_flush_rrects(r);
                /* Trailing chevron — the chip needs to read as "tap me to
                 * see options" rather than "press to cycle". A small ▾
                 * (U+25BE) on the right margin sells the dropdown. */
                const char *chevron = "\xe2\x96\xbe"; /* ▾ */
                f32 chev_w = (f32)cw;
                f32 chev_x = value_x + chip_w - 10.0f * dpi - chev_w;
                i32 max_chars = (i32)((chev_x - (value_x + 8.0f * dpi)) / cw);
                if (max_chars < 1) max_chars = 1;
                draw_label_limited(r, disp, value_x + 8.0f * dpi, y,
                                   tc->agent_model[0] ? value_fg : dim_fg,
                                   max_chars);
                draw_label(r, chevron, chev_x, y, dim_fg);
            }
            y += row_h;
        } else if (tc->backend == TRANSLATE_BACKEND_API) {
            bool custom = strcmp(tc->api_provider, "custom") == 0;
            f32 radius = fminf(control_h * 0.5f, 6.0f * dpi);
            f32 chip_w = value_room;
            if (chip_w < 12.0f * cw) chip_w = 12.0f * cw;

            /* Provider → 4-segment picker. Selecting one clears the model
             * (every provider has its own id scheme) — see click handler. */
            draw_label(r, "Provider:", content_x, y, label_fg);
            {
                const char *names[] = {"Anthropic", "OpenAI", "OpenRouter",
                                       "Custom"};
                const char *ids[]   = {"anthropic", "openai", "openrouter",
                                       "custom"};
                i32 sel = 0;
                for (i32 i = 0; i < 4; i++) {
                    if (strcmp(tc->api_provider, ids[i]) == 0) { sel = i; break; }
                }
                /* 11cw, not 9: "OpenRouter" (10 chars) overflowed a 9-wide
                 * segment (draw_label doesn't clip). */
                f32 seg_w = 11.0f * cw;
                if (value_room > 0.0f && seg_w * 4.0f > value_room) {
                    seg_w = value_room / 4.0f;
                }
                if (seg_w < 1.0f) seg_w = 1.0f;
                settings_draw_segmented(r, value_x, y - 4.0f * dpi,
                                        names, 4, sel,
                                        seg_w, control_h,
                                        control_active, control_bg,
                                        control_border, value_fg,
                                        value_fg, cw, ch, dpi);
            }
            y += row_h;

            /* API Key → masked inline editor (Enter commits, Esc cancels,
             * Cmd+V pastes; the committed key lands in config.json). */
            draw_label(r, "API Key:", content_x, y, label_fg);
            {
                Color chip_bg = sp->editing_api_key ? control_active
                                                    : control_bg;
                s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w,
                                 control_h, chip_bg, control_border,
                                 fmaxf(1.0f, dpi), radius);
                if (sp->editing_api_key) {
                    s_rrect_bordered_per_corner(r, value_x,
                        y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
                }
                renderer_flush_rrects(r);
                i32 max_chars = (i32)((chip_w - 16.0f * dpi) / cw);
                if (max_chars < 4) max_chars = 4;
                if (sp->editing_api_key) {
                    /* One bullet per typed char, tail-windowed so the
                     * cursor never leaves the chip. */
                    i32 len = (i32)strlen(sp->api_key_buf);
                    i32 shown = len < max_chars - 1 ? len : max_chars - 1;
                    char dots[388];
                    i32 di = 0;
                    for (i32 k = 0; k < shown &&
                                    di + 4 < (i32)sizeof dots; k++) {
                        memcpy(dots + di, "\xe2\x80\xa2", 3);   /* • */
                        di += 3;
                    }
                    dots[di] = '\0';
                    draw_label(r, dots, value_x + 8.0f * dpi, y, value_fg);
                    s_rect(r, value_x + 8.0f * dpi + (f32)shown * cw, y,
                           1, ch, (Color){text_fg.r, text_fg.g, text_fg.b,
                                          0.8f});
                } else if (tc->api_key[0]) {
                    usize klen = strlen(tc->api_key);
                    const char *tail = klen > 4 ? tc->api_key + klen - 4
                                                : tc->api_key;
                    char masked[48];
                    snprintf(masked, sizeof masked,
                             "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"
                             "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"
                             "%s", tail);
                    draw_label_limited(r, masked, value_x + 8.0f * dpi, y,
                                       value_fg, max_chars);
                } else {
                    char envkey[256];
                    translate_api_effective_key(tc, envkey, sizeof envkey);
                    draw_label_limited(r,
                        envkey[0] ? "(using env key)" : "(not set)",
                        value_x + 8.0f * dpi, y, dim_fg, max_chars);
                }
            }
            y += row_h;

            /* Model → dropdown chip over the provider's live /v1/models
             * list (or static fallback). A custom server may expose no
             * model list at all, so its Model row falls back to a free-text
             * inline editor (editing_api_model) — the only way to set the
             * id when there's nothing to pick from. */
            draw_label(r, "Model:", content_x, y, label_fg);
            if (sp->editing_api_model) {
                Color chip_bg = control_active;
                s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w,
                                 control_h, chip_bg, control_border,
                                 fmaxf(1.0f, dpi), radius);
                s_rrect_bordered_per_corner(r, value_x,
                    y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                    chip_w, fmaxf(2.0f, 2.0f * dpi),
                    accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
                renderer_flush_rrects(r);
                i32 max_chars = (i32)((chip_w - 16.0f * dpi) / cw);
                if (max_chars < 4) max_chars = 4;
                i32 len = (i32)strlen(sp->api_model_buf);
                i32 shown = len < max_chars - 1 ? len : max_chars - 1;
                const char *vis_text = sp->api_model_buf + (len - shown);
                draw_label(r, vis_text, value_x + 8.0f * dpi, y, value_fg);
                s_rect(r, value_x + 8.0f * dpi + (f32)shown * cw, y, 1, ch,
                       (Color){text_fg.r, text_fg.g, text_fg.b, 0.8f});
            } else {
                const char *disp = tc->api_model[0] ? tc->api_model
                                 : custom ? "(tap to set model)" : "(default)";
                bool picker_open = sp->translate_model_picker_open;
                Color chip_bg = picker_open ? control_active : control_bg;
                s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w,
                                 control_h, chip_bg, control_border,
                                 fmaxf(1.0f, dpi), radius);
                if (picker_open) {
                    s_rrect_bordered_per_corner(r, value_x,
                        y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
                }
                renderer_flush_rrects(r);
                const char *chevron = "\xe2\x96\xbe"; /* ▾ */
                f32 chev_w = (f32)cw;
                f32 chev_x = value_x + chip_w - 10.0f * dpi - chev_w;
                i32 max_chars = (i32)((chev_x - (value_x + 8.0f * dpi)) / cw);
                if (max_chars < 1) max_chars = 1;
                draw_label_limited(r, disp, value_x + 8.0f * dpi, y,
                                   tc->api_model[0] ? value_fg : dim_fg,
                                   max_chars);
                draw_label(r, chevron, chev_x, y, dim_fg);
            }
            y += row_h;

            /* Base URL — only for the custom OpenAI-compatible provider. */
            if (custom) {
                draw_label(r, "Base URL:", content_x, y, label_fg);
                Color chip_bg = sp->editing_api_url ? control_active
                                                    : control_bg;
                s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w,
                                 control_h, chip_bg, control_border,
                                 fmaxf(1.0f, dpi), radius);
                if (sp->editing_api_url) {
                    s_rrect_bordered_per_corner(r, value_x,
                        y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
                }
                renderer_flush_rrects(r);
                i32 max_chars = (i32)((chip_w - 16.0f * dpi) / cw);
                if (max_chars < 4) max_chars = 4;
                if (sp->editing_api_url) {
                    i32 len = (i32)strlen(sp->api_url_buf);
                    i32 shown = len < max_chars - 1 ? len : max_chars - 1;
                    const char *vis_text = sp->api_url_buf + (len - shown);
                    draw_label(r, vis_text, value_x + 8.0f * dpi, y,
                               value_fg);
                    s_rect(r, value_x + 8.0f * dpi + (f32)shown * cw, y,
                           1, ch, (Color){text_fg.r, text_fg.g, text_fg.b,
                                          0.8f});
                } else {
                    draw_label_limited(r,
                        tc->api_base_url[0] ? tc->api_base_url
                            : "(e.g. http://localhost:11434/v1)",
                        value_x + 8.0f * dpi, y,
                        tc->api_base_url[0] ? value_fg : dim_fg, max_chars);
                }
                y += row_h;
            }

            /* Helper caption — mirrors the Terminal tab's dim hints. */
            draw_label(r, custom
                       ? "Key optional for local servers; stored in config.json."
                       : "Key is stored in config.json; provider env vars also work.",
                       content_x, y, dim_fg);
            y += row_h;
        }

        /* From / To — rounded chips that open the language picker. */
        {
            bool pick_src = (sp->translate_lang_picker == SETTINGS_LANG_PICKER_SOURCE);
            bool pick_dst = (sp->translate_lang_picker == SETTINGS_LANG_PICKER_TARGET);
            const char *src = tc->source_lang[0] ? tc->source_lang : "Turkish";
            const char *dst = tc->target_lang[0] ? tc->target_lang : "English";
            f32 chip_w = 12.0f * cw;
            if (value_room > 0.0f && chip_w > value_room) chip_w = value_room;
            f32 radius = fminf(control_h * 0.5f, 6.0f * dpi);
            f32 bw = fmaxf(1.0f, dpi);

            draw_label(r, "From:", content_x, y, label_fg);
            Color src_bg = pick_src ? control_active : control_bg;
            s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w, control_h,
                             src_bg, control_border, bw, radius);
            if (pick_src) {
                s_rrect_bordered_per_corner(r, value_x,
                    y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                    chip_w, fmaxf(2.0f, 2.0f * dpi),
                    accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
            }
            renderer_flush_rrects(r);
            draw_label(r, src, value_x + 8.0f * dpi, y, value_fg);
            y += row_h;

            draw_label(r, "To:", content_x, y, label_fg);
            Color dst_bg = pick_dst ? control_active : control_bg;
            s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w, control_h,
                             dst_bg, control_border, bw, radius);
            if (pick_dst) {
                s_rrect_bordered_per_corner(r, value_x,
                    y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                    chip_w, fmaxf(2.0f, 2.0f * dpi),
                    accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
            }
            renderer_flush_rrects(r);
            draw_label(r, dst, value_x + 8.0f * dpi, y, value_fg);
            y += row_h;
        }

        if (sp->translate_lang_picker != SETTINGS_LANG_PICKER_CLOSED) {
            f32 menu_h = settings_translate_lang_menu_height(ch, dpi);
            f32 menu_y = y + 4.0f * dpi;
            settings_draw_translate_lang_menu(sp, r, content_x, menu_y,
                                             cw, ch, dpi, panel_bg, value_fg,
                                             dim_fg, accent);
            y += menu_h + 8.0f * dpi;
        }
        /* Model picker popup is meaningless under Local — guarded both here
         * and in the Backend cycle's close-on-switch in the click handler. */
        if (sp->translate_model_picker_open &&
            tc->backend != TRANSLATE_BACKEND_LOCAL) {
            f32 menu_w = settings_translate_model_menu_width(cw, dpi, content_w);
            f32 menu_h = settings_translate_model_menu_height(tc, ch, dpi);
            f32 menu_x = value_x;
            if (menu_x + menu_w > content_x + content_w)
                menu_x = content_x + content_w - menu_w;
            if (menu_x < content_x) menu_x = content_x;
            f32 menu_y = y + 4.0f * dpi;
            settings_draw_translate_model_menu(sp, r, menu_x, menu_y, menu_w,
                                               cw, ch, dpi, panel_bg, value_fg,
                                               dim_fg, accent);
            y += menu_h + 8.0f * dpi;
        }

        /* Chord window — rounded chip row (outer corners only). */
        draw_label(r, "Chord window:", content_x, y, label_fg);
        {
            static const f32 cw_presets[] = {0.30f, 0.40f, 0.50f, 0.60f, 0.80f, 1.00f};
            static const char *cw_labels[] = {"0.3s","0.4s","0.5s","0.6s","0.8s","1.0s"};
            const i32 n = (i32)(sizeof(cw_presets) / sizeof(cw_presets[0]));
            f32 chip_x = value_x;
            f32 chip_y = y - 4.0f * dpi;
            f32 chip_h = control_h;
            f32 chip_w = 5.5f * cw;
            if (value_room > 0.0f && chip_w * (f32)n > value_room) {
                chip_w = value_room / (f32)n;
            }
            if (chip_w < 1.0f) chip_w = 1.0f;
            f32 radius = fminf(chip_h * 0.5f, 6.0f * dpi);
            f32 bw = fmaxf(1.0f, dpi);
            for (i32 pi = 0; pi < n; pi++) {
                bool selected = fabsf(tc->tab_window_sec - cw_presets[pi]) < 0.005f;
                Color bg = selected ? control_active : control_bg;
                f32 r_tl = (pi == 0)     ? radius : 0.0f;
                f32 r_bl = (pi == 0)     ? radius : 0.0f;
                f32 r_tr = (pi == n - 1) ? radius : 0.0f;
                f32 r_br = (pi == n - 1) ? radius : 0.0f;
                s_rrect_bordered_per_corner(r, chip_x, chip_y, chip_w, chip_h,
                                            bg, control_border, bw,
                                            r_tl, r_tr, r_br, r_bl);
                chip_x += chip_w;
            }
            renderer_flush_rrects(r);
            chip_x = value_x;
            for (i32 pi = 0; pi < n; pi++) {
                bool selected = fabsf(tc->tab_window_sec - cw_presets[pi]) < 0.005f;
                if (selected) {
                    f32 r_bl = (pi == 0)     ? radius : 0.0f;
                    f32 r_br = (pi == n - 1) ? radius : 0.0f;
                    s_rrect_bordered_per_corner(r,
                        chip_x, chip_y + chip_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f,
                        0.0f, 0.0f, r_br, r_bl);
                }
                chip_x += chip_w;
            }
            renderer_flush_rrects(r);
            chip_x = value_x;
            for (i32 pi = 0; pi < n; pi++) {
                bool selected = fabsf(tc->tab_window_sec - cw_presets[pi]) < 0.005f;
                Color tc_col = selected ? value_fg : label_fg;
                f32 label_w = strlen(cw_labels[pi]) * cw;
                f32 tx = chip_x + (chip_w - label_w) * 0.5f;
                draw_label(r, cw_labels[pi], tx, chip_y + (chip_h - ch) * 0.5f, tc_col);
                chip_x += chip_w;
            }
        }
        y += row_h;

        /* —— Local model: download-on-demand ——
         * The ~440 MB GGUF is not bundled; fetch it here with a live speed /
         * remaining / ETA readout. The region is a fixed 2 rows so the render
         * and the click hit-test advance in lockstep regardless of state; the
         * buttons dispatch off the rects cached below. */
        {
            ModelDownloadStatus md;
            model_download_poll(&md);
            bool downloading = (md.state == MODEL_DL_DOWNLOADING ||
                                md.state == MODEL_DL_VERIFYING);
            bool have_model = liu_model_file_ok(tc->local_model_path);
            f32 bh = control_h;
            f32 by = y + row_h;                 /* action row */

            /* Stale-rect guard: a button not drawn this frame must not stay
             * clickable (w == 0 fails the hit test). */
            sp->tr_model_dl_w = sp->tr_model_cancel_w = sp->tr_model_pick_w = 0.0f;

            draw_label(r, "Model:", content_x, y, label_fg);

            if (downloading) {
                f32 pct = (md.bytes_total > 0)
                            ? (f32)md.bytes_done / (f32)md.bytes_total : 0.0f;
                if (md.state == MODEL_DL_VERIFYING) pct = 1.0f;
                if (pct < 0.0f) pct = 0.0f;
                if (pct > 1.0f) pct = 1.0f;

                char st[176];
                if (md.state == MODEL_DL_VERIFYING) {
                    snprintf(st, sizeof st, "Doğrulanıyor…");
                } else {
                    char spd[28], eta[28];
                    settings_fmt_speed(md.speed_bps, spd, sizeof spd);
                    settings_fmt_eta(md.eta_sec, eta, sizeof eta);
                    f64 left = (f64)md.bytes_total - (f64)md.bytes_done;
                    if (left < 0.0) left = 0.0;
                    snprintf(st, sizeof st,
                             "%d%%  ·  %s  ·  %.0f MB kaldı  ·  %s",
                             (i32)(pct * 100.0f + 0.5f), spd,
                             left / 1048576.0, eta);
                }
                draw_label(r, st, value_x, y, value_fg);

                /* Action row: progress bar + Cancel. */
                f32 cancel_w = settings_button_w("İptal", cw, dpi);
                f32 cancel_x = value_x + value_room - cancel_w;
                f32 bar_w = cancel_x - 8.0f * dpi - value_x;
                if (bar_w < 4.0f * cw) bar_w = 4.0f * cw;
                f32 bar_h = fmaxf(5.0f, 6.0f * dpi);
                f32 bar_y = by + (bh - bar_h) * 0.5f;
                s_rrect_simple(r, value_x, bar_y, bar_w, bar_h,
                               control_bg, bar_h * 0.5f);
                s_rrect_simple(r, value_x, bar_y, bar_w * pct, bar_h,
                               accent, bar_h * 0.5f);
                renderer_flush_rrects(r);
                settings_button(r, sp, "İptal", cancel_x, by, bh, cw, ch, dpi,
                                control_bg, control_border, value_fg,
                                &sp->tr_model_cancel_x, &sp->tr_model_cancel_y,
                                &sp->tr_model_cancel_w, &sp->tr_model_cancel_h);
            } else {
                if (md.state == MODEL_DL_ERROR) {
                    Color warn = (Color){ .r = 0.90f, .g = 0.46f, .b = 0.33f, .a = 1.0f };
                    i32 mc = (i32)(value_room / cw);
                    if (mc < 1) mc = 1;
                    draw_label_limited(r,
                        md.error[0] ? md.error : "İndirme başarısız",
                        value_x, y, warn, mc);
                } else if (have_model) {
                    draw_label(r, "İndirildi ✓", value_x, y, value_fg);
                } else {
                    draw_label(r, "İndirilmedi  ·  440 MB", value_x, y, dim_fg);
                }

                const char *primary = (md.state == MODEL_DL_ERROR) ? "Tekrar dene"
                                    : have_model ? "Yeniden indir" : "İndir";
                f32 bx = value_x;
                bx += settings_button(r, sp, primary, bx, by, bh, cw, ch, dpi,
                                      control_active, control_border, value_fg,
                                      &sp->tr_model_dl_x, &sp->tr_model_dl_y,
                                      &sp->tr_model_dl_w, &sp->tr_model_dl_h);
                bx += 8.0f * dpi;
                settings_button(r, sp, "Dosya seç", bx, by, bh, cw, ch, dpi,
                                control_bg, control_border, label_fg,
                                &sp->tr_model_pick_x, &sp->tr_model_pick_y,
                                &sp->tr_model_pick_w, &sp->tr_model_pick_h);
            }
            y += 2.0f * row_h;
        }

        /* —— ALLOWLIST section —— */
        y += 12.0f * dpi;
        draw_label(r, "ALLOWLIST", content_x, y, dim_fg);
        {
            f32 rule_x = content_x + fminf(11.0f * cw, content_w * 0.50f);
            f32 rule_w = content_x + content_w - rule_x;
            if (rule_w > 0.0f) {
                s_rect(r, rule_x, y + ch * 0.65f,
                       rule_w, fmaxf(1.0f, dpi), section_rule);
            }
        }
        y += ch + 12.0f * dpi;

        struct { const char *label; bool *flag; } agents[] = {
            { "Claude:",   &tc->active_in_claude },
            { "Codex:",    &tc->active_in_codex },
            { "OpenCode:", &tc->active_in_opencode },
            { "Grok:",     &tc->active_in_grok },
        };
        for (i32 i = 0; i < (i32)(sizeof(agents) / sizeof(agents[0])); i++) {
            draw_label(r, agents[i].label, content_x, y, label_fg);
            settings_draw_switch(r, value_x, y - 2.0f * dpi,
                                 *agents[i].flag, control_active,
                                 control_bg, control_border,
                                 value_fg, label_fg, cw, ch, dpi);
            y += row_h;
        }

        y += 8.0f * dpi;
        draw_label(r, "Type a prompt, press Ctrl+Cmd twice to translate.",
                   content_x, y, dim_fg);
        y += row_h;
        draw_label(r, "Settings persist on Config Export (About tab).",
                   content_x, y, dim_fg);
        content_end_y = y + ch + 16;

    } else if (sp->active_tab == SETTINGS_TAB_NOTIFY) {
        settings_notify_ensure_loaded(sp);
        f32 y = content_y;

        /* Mic-record housekeeping: enforce a short cap so a forgotten
         * recording can't run forever, and bail out (with a visible note)
         * if the user denied access so a row can't get stuck "recording". */
        if (sp->notify_recording_row >= 0) {
            if (platform_audio_mic_permission() == PLATFORM_MIC_DENIED) {
                platform_audio_record_stop();
                sp->notify_recording_row = -1;
                snprintf(sp->notify_record_status, sizeof sp->notify_record_status,
                         "Microphone access denied \xe2\x80\x94 enable Liu under System "
                         "Settings \xe2\x80\xba Privacy & Security \xe2\x80\xba Microphone.");
            } else if (platform_audio_record_elapsed() >= 15.0) {
                settings_notify_finalize_recording(sp);
            }
        } else if (platform_audio_recording()) {
            /* Untracked capture (a permission-grant landed after the user
             * cancelled) — release the mic so it can't linger. */
            platform_audio_record_stop();
        }

        /* Section separator color — mirrors the recipe other tabs use. */
        Color control_border = settings_blend(panel_bg, text_fg,
                                              cp.is_light ? 0.16f : 0.20f);
        control_border.a = 0.68f;
        Color section_rule = control_border;
        section_rule.a = 0.42f;

        draw_label(r, "SOUND PER EVENT", content_x, y, dim_fg);
        {
            f32 rule_x = content_x + fminf(15.0f * cw, content_w * 0.55f);
            f32 rule_w = content_x + content_w - rule_x;
            if (rule_w > 0.0f) {
                s_rect(r, rule_x, y + ch * 0.65f,
                       rule_w, fmaxf(1.0f, dpi), section_rule);
            }
        }
        y += ch + 14.0f * dpi;

        /* 4 rows × {label, path display, Browse, ▶, ✕}. The renderer caches
         * each button's rect into sp->notify_* so the click handler doesn't
         * have to re-layout. The cache is overwritten every frame which is
         * fine — clicks land before the next frame's render. */
        static const u8 kEvents[4] = {
            EVT_STOP, EVT_NOTIFY, EVT_ERROR, EVT_COMPLETE,
        };
        f32 row_h_notify = ch + 14.0f * dpi;
        f32 label_col_w  = 14.0f * cw;
        f32 btn_h        = ch + 8.0f * dpi;
        f32 btn_gap      = 6.0f * dpi;
        f32 btn_w_browse = 9.0f * cw;
        f32 btn_w_icon   = 4.0f * cw;
        Color btn_bg     = cp.surface_sunken;
        Color btn_border = cp.divider_subtle;
        Color icon_fg    = value_fg;

        for (i32 i = 0; i < 4; i++) {
            u8 evt = kEvents[i];
            const char *evt_label = notify_event_short(evt);
            const char *cur_path  = notify_config_sound_for(&sp->notify_cfg,
                                                            NOTIFY_MATCH_ANY, evt);
            const char *disp = cur_path ? path_basename(cur_path) : "(default — TTS)";
            Color disp_fg = cur_path ? value_fg : dim_fg;

            /* While this row is capturing, the path slot shows a live timer.
             * Between the click and an actual capture (the permission prompt is
             * still up, or a grant is in flight) show a "waiting" state instead
             * of a 0s timer so the row isn't misleading. */
            bool rec_here = (sp->notify_recording_row == i);
            char rec_disp[80];
            if (rec_here) {
                if (platform_audio_recording()) {
                    f64 el = platform_audio_record_elapsed();
                    snprintf(rec_disp, sizeof rec_disp,
                             "\xe2\x97\x8f Recording\xe2\x80\xa6 %.0fs", el);
                } else {
                    snprintf(rec_disp, sizeof rec_disp,
                             "\xe2\x97\x8f Waiting for microphone\xe2\x80\xa6");
                }
                disp = rec_disp;
                disp_fg = (Color){ .r = 0.90f, .g = 0.27f, .b = 0.27f, .a = 1.0f };
            }

            f32 row_y = y;
            f32 btn_y = row_y - 2.0f * dpi;

            draw_label(r, evt_label, content_x, row_y, label_fg);

            f32 path_x = content_x + label_col_w;
            f32 right_edge = content_x + content_w;
            /* Right-anchored button strip: Banner | Clear | Test | Mic |
             * Browse, reading right-to-left. Browse + Mic are the two ways
             * to set a sound (pick a file / record one) so they sit together
             * on the left of the cluster. */
            f32 banner_x = right_edge - btn_w_icon;
            f32 clear_x  = banner_x - btn_gap - btn_w_icon;
            f32 test_x   = clear_x  - btn_gap - btn_w_icon;
            f32 mic_x    = test_x   - btn_gap - btn_w_icon;
            f32 browse_x = mic_x    - btn_gap - btn_w_browse;
            f32 path_w_avail = browse_x - btn_gap - path_x;

            i32 max_chars = (i32)(path_w_avail / cw);
            if (max_chars < 1) max_chars = 1;
            draw_label_limited(r, disp, path_x, row_y, disp_fg, max_chars);

            /* Browse button */
            s_rrect_bordered_per_corner(r,
                browse_x, btn_y, btn_w_browse, btn_h,
                btn_bg, btn_border, fmaxf(1.0f, dpi),
                6.0f * dpi, 6.0f * dpi, 6.0f * dpi, 6.0f * dpi);
            /* Centre "Browse" (6 glyphs at cw advance) in the button. */
            draw_label(r, "Browse", browse_x + (btn_w_browse - 6.0f * cw) * 0.5f,
                       btn_y + (btn_h - ch) * 0.5f, icon_fg);
            sp->notify_browse_x[i] = browse_x;
            sp->notify_browse_y[i] = btn_y;
            sp->notify_browse_w[i] = btn_w_browse;
            sp->notify_browse_h[i] = btn_h;

            /* Mic — record / stop a custom sound for this event. A filled
             * circle (record) flips to a filled square (stop) and the button
             * tints red while this row is capturing. */
            Color mic_bg  = rec_here
                ? (Color){ .r = 0.78f, .g = 0.20f, .b = 0.20f, .a = 0.92f }
                : btn_bg;
            Color mic_brd = rec_here
                ? (Color){ .r = 0.86f, .g = 0.26f, .b = 0.26f, .a = 1.0f }
                : btn_border;
            s_rrect_bordered_per_corner(r,
                mic_x, btn_y, btn_w_icon, btn_h,
                mic_bg, mic_brd, fmaxf(1.0f, dpi),
                6.0f * dpi, 6.0f * dpi, 6.0f * dpi, 6.0f * dpi);
            /* U+25CF ● record, U+25A0 ■ stop */
            const char *mic_glyph = rec_here ? "\xe2\x96\xa0" : "\xe2\x97\x8f";
            Color mic_fg = rec_here
                ? (Color){ .r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f }
                : (Color){ .r = 0.86f, .g = 0.30f, .b = 0.30f, .a = 1.0f };
            draw_label(r, mic_glyph,
                       mic_x + (btn_w_icon - cw) * 0.5f,
                       btn_y + (btn_h - ch) * 0.5f, mic_fg);
            sp->notify_mic_x[i] = mic_x;
            sp->notify_mic_y[i] = btn_y;
            sp->notify_mic_w[i] = btn_w_icon;
            sp->notify_mic_h[i] = btn_h;

            /* Test (▶) — disabled-looking if no path is set */
            s_rrect_bordered_per_corner(r,
                test_x, btn_y, btn_w_icon, btn_h,
                btn_bg, btn_border, fmaxf(1.0f, dpi),
                6.0f * dpi, 6.0f * dpi, 6.0f * dpi, 6.0f * dpi);
            Color play_fg = cur_path ? icon_fg : dim_fg;
            /* U+25B6 ▶ — black right-pointing triangle */
            draw_label(r, "\xe2\x96\xb6",
                       test_x + (btn_w_icon - cw) * 0.5f,
                       btn_y + (btn_h - ch) * 0.5f, play_fg);
            sp->notify_test_x[i] = test_x;
            sp->notify_test_y[i] = btn_y;
            sp->notify_test_w[i] = btn_w_icon;
            sp->notify_test_h[i] = btn_h;

            /* Clear (✕) — also disabled-looking when nothing to clear */
            s_rrect_bordered_per_corner(r,
                clear_x, btn_y, btn_w_icon, btn_h,
                btn_bg, btn_border, fmaxf(1.0f, dpi),
                6.0f * dpi, 6.0f * dpi, 6.0f * dpi, 6.0f * dpi);
            Color x_fg = cur_path ? icon_fg : dim_fg;
            /* U+00D7 × */
            draw_label(r, "\xc3\x97",
                       clear_x + (btn_w_icon - cw) * 0.5f,
                       btn_y + (btn_h - ch) * 0.5f, x_fg);
            sp->notify_clear_x[i] = clear_x;
            sp->notify_clear_y[i] = btn_y;
            sp->notify_clear_w[i] = btn_w_icon;
            sp->notify_clear_h[i] = btn_h;

            /* Per-event master On/Off. When off the daemon suppresses the
             * whole event — no sound, no TTS, no banner. */
            bool evt_on = sp->notify_cfg.event_enabled[evt];
            /* OFF state reads as "muted": red text + faint red rim (the
             * mic-mute convention), reusing the mic-record red so the palette
             * stays consistent. ON keeps the accent (primary) fill. */
            Color off_red = (Color){ .r = 0.86f, .g = 0.30f, .b = 0.30f, .a = 1.0f };
            Color off_rim = off_red; off_rim.a = 0.55f;
            Color bn_bg = evt_on ? cp.btn_primary_bg : btn_bg;
            if (evt_on) bn_bg.a = fminf(bn_bg.a, 0.85f);
            Color bn_border = evt_on ? cp.btn_primary_bg : off_rim;
            s_rrect_bordered_per_corner(r,
                banner_x, btn_y, btn_w_icon, btn_h,
                bn_bg, bn_border, fmaxf(1.0f, dpi),
                6.0f * dpi, 6.0f * dpi, 6.0f * dpi, 6.0f * dpi);
            /* Plain "On"/"Off" text — clearer at a glance. Legible colour
             * picked from the fill so it reads on every theme (the ON fill
             * is the accent primary; OFF is the row button bg). */
            Color bn_fg = evt_on ? chrome_legible_on(bn_bg) : off_red;
            const char *bn_txt = evt_on ? "On" : "Off";
            f32 bn_txt_w = (f32)strlen(bn_txt) * cw;
            draw_label(r, bn_txt,
                       banner_x + (btn_w_icon - bn_txt_w) * 0.5f,
                       btn_y + (btn_h - ch) * 0.5f, bn_fg);
            sp->notify_banner_x[i] = banner_x;
            sp->notify_banner_y[i] = btn_y;
            sp->notify_banner_w[i] = btn_w_icon;
            sp->notify_banner_h[i] = btn_h;

            y += row_h_notify;
        }
        renderer_flush_rrects(r);

        /* Cursor hover: register the 4 per-event buttons. The same
         * rectangles drive click hit-test in settings_handle_click. */
        for (i32 i = 0; i < 4; i++) {
            sp_push_hover(sp, sp->notify_browse_x[i], sp->notify_browse_y[i],
                          sp->notify_browse_w[i], sp->notify_browse_h[i]);
            sp_push_hover(sp, sp->notify_test_x[i],   sp->notify_test_y[i],
                          sp->notify_test_w[i],   sp->notify_test_h[i]);
            sp_push_hover(sp, sp->notify_clear_x[i],  sp->notify_clear_y[i],
                          sp->notify_clear_w[i],  sp->notify_clear_h[i]);
            sp_push_hover(sp, sp->notify_banner_x[i], sp->notify_banner_y[i],
                          sp->notify_banner_w[i], sp->notify_banner_h[i]);
            sp_push_hover(sp, sp->notify_mic_x[i],    sp->notify_mic_y[i],
                          sp->notify_mic_w[i],    sp->notify_mic_h[i]);
        }

        /* Mic hint / status line (e.g. "recording will begin", "access
         * denied"). Drawn under the rows so it doesn't disturb the strip. */
        {
            const char *hint = sp->notify_record_status[0]
                ? sp->notify_record_status
                : "\xe2\x97\x8f Record your own sound with the mic button.";
            draw_label_limited(r, hint, content_x, y, dim_fg,
                               (i32)(content_w / cw));
            y += ch + 6.0f * dpi;
        }

        y += 12.0f * dpi;
        /* Reset-to-bundled action — overwrites the 4 (any, event) rules
         * with paths to the wavs shipped in assets/sounds/. Lets a user
         * with a pre-existing notify.conf adopt the new defaults without
         * editing the file by hand. */
        {
            f32 reset_w = (f32)strlen("Reset to bundled defaults") * cw + 24.0f * dpi;
            f32 reset_x = content_x;
            s_rrect_bordered_per_corner(r,
                reset_x, y, reset_w, btn_h,
                btn_bg, btn_border, fmaxf(1.0f, dpi),
                6.0f * dpi, 6.0f * dpi, 6.0f * dpi, 6.0f * dpi);
            renderer_flush_rrects(r);
            draw_label(r, "Reset to bundled defaults",
                       reset_x + 12.0f * dpi,
                       y + (btn_h - ch) * 0.5f, value_fg);
            sp->notify_reset_x = reset_x;
            sp->notify_reset_y = y;
            sp->notify_reset_w = reset_w;
            sp->notify_reset_h = btn_h;
            sp_push_hover(sp, reset_x, y, reset_w, btn_h);
            y += btn_h + 12.0f * dpi;
        }

        /* Per-agent hook installer rows. agent_detect_available() lists
         * every CLI agent that's actually on PATH, so the section adapts
         * to the user's box. Today only Claude Code exposes a hook API
         * we can target — other rows render with an inline "no hook API
         * yet" note so users see the roadmap rather than wondering if
         * the button is broken. */
        draw_label(r, "AGENT HOOKS", content_x, y, dim_fg);
        {
            f32 rule_x = content_x + fminf(12.0f * cw, content_w * 0.55f);
            f32 rule_w = content_x + content_w - rule_x;
            if (rule_w > 0.0f) {
                s_rect(r, rule_x, y + ch * 0.65f,
                       rule_w, fmaxf(1.0f, dpi), section_rule);
            }
        }
        y += ch + 10.0f * dpi;

        if (!s_notify_agents_loaded) {
            s_notify_agents_count = agent_detect_available(s_notify_agents, AGENT_MAX);
            s_notify_agents_loaded = true;
        }
        const AgentInfo *detected = s_notify_agents;
        i32 n_detected = s_notify_agents_count;
        sp->notify_hook_count = 0;
        for (i32 ai = 0; ai < n_detected && ai < 16; ai++) {
            const AgentInfo *a = &detected[ai];
            bool has_hook = agent_hook_supported(a->id);
            bool installed = has_hook && agent_hook_installed(a->id);
            const char *lbl = has_hook ? (installed ? "Uninstall" : "Install")
                                       : "No hook API yet";

            f32 hook_w = (f32)strlen(lbl) * cw + 24.0f * dpi;
            f32 hook_x = content_x + content_w - hook_w;
            /* Agent display name on the left, action button on the right. */
            draw_label(r, a->display, content_x, y + (btn_h - ch) * 0.5f,
                       value_fg);
            Color row_bg = has_hook ? btn_bg : cp.surface_neutral;
            Color row_border = btn_border;
            if (!has_hook) row_border.a *= 0.5f;
            s_rrect_bordered_per_corner(r,
                hook_x, y, hook_w, btn_h,
                row_bg, row_border, fmaxf(1.0f, dpi),
                6.0f * dpi, 6.0f * dpi, 6.0f * dpi, 6.0f * dpi);
            renderer_flush_rrects(r);
            Color lbl_fg = has_hook ? value_fg : dim_fg;
            draw_label(r, lbl,
                       hook_x + 12.0f * dpi,
                       y + (btn_h - ch) * 0.5f, lbl_fg);
            if (has_hook) {
                /* Every supported agent's button is clickable; unsupported
                 * ones are pure presentation. Record geometry for hover+click. */
                sp->notify_hook_x[sp->notify_hook_count] = hook_x;
                sp->notify_hook_y[sp->notify_hook_count] = y;
                sp->notify_hook_w[sp->notify_hook_count] = hook_w;
                sp->notify_hook_h[sp->notify_hook_count] = btn_h;
                snprintf(sp->notify_hook_id[sp->notify_hook_count],
                         sizeof sp->notify_hook_id[0], "%s", a->id);
                sp->notify_hook_count++;
                sp_push_hover(sp, hook_x, y, hook_w, btn_h);
            }
            y += btn_h + 6.0f * dpi;
        }
        if (sp->notify_claude_status[0]) {
            y += 4.0f * dpi;
            draw_label(r, sp->notify_claude_status, content_x, y, dim_fg);
            y += row_h_notify;
        }
        y += 8.0f * dpi;

        draw_label(r, "When no sound is set the daemon falls back to TTS.",
                   content_x, y, dim_fg);
        y += row_h_notify;
        draw_label(r, "Changes apply live on save.",
                   content_x, y, dim_fg);
        content_end_y = y + ch + 16;

    } else if (sp->active_tab == SETTINGS_TAB_KEYS) {
        f32 y = content_y;
        if (sp->rebinding) {
            Color warn_clr = theme ? theme->ansi[3] : (Color){1.0f, 0.8f, 0.3f, 1.0f}; /* yellow */
            draw_label(r, "Press new key combo...", content_x, y, warn_clr);
            y += ch + 4;
            draw_label(r, action_name((Action)sp->rebind_action), content_x, y, value_fg);
            y += ch + 12;
            draw_label(r, "ESC to cancel  ·  Delete to unbind", content_x, y, dim_fg);
            content_end_y = y + ch + 16;
        } else {
            draw_label(r, "SHORTCUTS  ·  click any binding to change it",
                       content_x, y, dim_fg);
            y += keys_list_top_gap(ch, dpi);

            /* Walk the shared row table (g_keys_rows) at a uniform pitch so the
             * click hit-test in settings_handle_click stays pixel-aligned with
             * what is drawn. Generous row height gives the chips breathing room;
             * section headers + read-only rows are drawn but never clickable. */
            Color accent = theme ? theme->cursor : (Color){0.78f, 0.78f, 0.82f, 1.0f};
            f32 row_h   = keys_list_row_h(ch, dpi);
            f32 text_dy = (row_h - ch) * 0.5f;   /* vertical centering in the row */

            for (i32 i = 0; i < g_keys_row_count; i++) {
                const KeysRow *kr = &g_keys_rows[i];

                /* Section header — dim label + trailing hairline rule. */
                if (kr->action == ACT_NONE && kr->keys == NULL) {
                    Color hdr = dim_fg; hdr.a *= 0.9f;
                    f32 hy = y + text_dy;
                    draw_label(r, kr->label, content_x, hy, hdr);
                    f32 lbl_w  = (f32)strlen(kr->label) * cw;
                    f32 rule_x = content_x + lbl_w + 10.0f * dpi;
                    f32 rule_w = content_x + content_w - rule_x;
                    if (rule_w > 8.0f * dpi) {
                        Color rule = dim_fg; rule.a *= 0.22f;
                        s_rect(r, rule_x, hy + ch * 0.5f, rule_w, fmaxf(1.0f, dpi), rule);
                    }
                    y += row_h;
                    continue;
                }

                bool rebindable = (kr->action != ACT_NONE);
                const KeyBinding *kb = NULL;
                const char *name, *key_str;
                if (rebindable) {
                    kb      = sp->keybinds ? keybind_get(sp->keybinds, kr->action) : NULL;
                    name    = action_name(kr->action);
                    key_str = kb ? keybind_format(kb) : "Unbound";
                } else {
                    name    = kr->label;
                    key_str = kr->keys;
                }

                /* Right-aligned rounded "kbd" pill. Rebindable rows take the
                 * accent tint; read-only rows are muted so they read as
                 * reference rather than something you can click to change. */
                f32 ks_w   = (f32)strlen(key_str) * cw;
                f32 pad_x  = 8.0f * dpi;
                f32 pill_w = ks_w + 2.0f * pad_x;
                f32 pill_h = ch + 4.0f * dpi;
                f32 pill_x = content_x + content_w - pill_w - 6.0f * dpi;
                f32 pill_y = y + (row_h - pill_h) * 0.5f;
                Color pill_fill, pill_bd;
                if (rebindable) {
                    pill_fill = cp.btn_primary_bg; pill_fill.a = kb ? 0.22f : 0.10f;
                    pill_bd   = (Color){ accent.r, accent.g, accent.b, kb ? 0.45f : 0.22f };
                } else {
                    pill_fill = cp.surface_sunken; pill_fill.a = 0.35f;
                    pill_bd   = dim_fg; pill_bd.a = 0.22f;
                }
                s_rrect_bordered(r, pill_x, pill_y, pill_w, pill_h, pill_fill,
                                 pill_bd, fmaxf(1.0f, dpi),
                                 fminf(pill_h * 0.5f, 7.0f * dpi));
                renderer_flush_rrects(r);   /* keep labels above the pill */

                Color name_fg = rebindable ? value_fg : dim_fg;
                Color key_fg  = rebindable ? (kb ? value_fg : dim_fg) : dim_fg;
                draw_label(r, name, content_x + 4.0f * dpi, y + text_dy, name_fg);
                draw_label(r, key_str, pill_x + pad_x, y + text_dy, key_fg);

                /* Pointing-hand cursor + click target only on rebindable rows,
                 * and only while the row is inside the visible scissor band —
                 * matching the click hit-test in settings_handle_click. */
                if (rebindable && y < clip_bottom && y + row_h > clip_top) {
                    sp_push_hover(sp, content_x - 4.0f * dpi, y,
                                  content_w + 8.0f * dpi, row_h);
                }
                y += row_h;
            }
            content_end_y = y + 16.0f * dpi;
        }

    } else if (sp->active_tab == SETTINGS_TAB_VAULT) {
        f32 y = content_y;
        Vault *v = sp->vault;
        bool initialized = v && vault_is_initialized(v);
        bool unlocked    = v && vault_is_unlocked(v);
        f32 panel_inner_w = content_w;
        if (panel_inner_w < 16.0f * dpi) panel_inner_w = 16.0f * dpi;

        f32 card_radius = 10.0f * dpi;
        f32 input_radius = 6.0f * dpi;
        f32 btn_radius  = 8.0f * dpi;

        /* Status card — large, rounded, with a colored dot indicator.
         * Colour: green when unlocked, amber when locked, dim when the
         * vault hasn't been initialised yet. */
        Color status_dot = unlocked
            ? (Color){0.36f, 0.84f, 0.45f, 1.0f}
            : initialized ? (Color){1.00f, 0.72f, 0.18f, 1.0f}
                          : (Color){0.45f, 0.45f, 0.50f, 1.0f};
        const char *status_label = unlocked ? "Unlocked"
                                 : initialized ? "Locked" : "Not initialized";
        const char *status_sub =
            unlocked       ? "Secrets are decrypted in memory."
            : initialized  ? "Unlock to access stored secrets."
                           : "Create a master password to start storing secrets.";

        f32 card_h = ch * 2.0f + 22.0f * dpi;
        s_rrect_simple(r, content_x, y, panel_inner_w, card_h,
                                   cp.surface_neutral, card_radius);
        renderer_flush_rrects(r);
        f32 dot_d = 10.0f * dpi;
        f32 dot_cx = content_x + 18.0f * dpi;
        f32 dot_cy = y + card_h * 0.5f - dot_d * 0.5f;
        s_rrect_simple(r, dot_cx, dot_cy, dot_d, dot_d,
                       status_dot, dot_d * 0.5f);
        renderer_flush_rrects(r);
        f32 text_x = dot_cx + dot_d + 10.0f * dpi;
        draw_label(r, status_label, text_x,
                   y + 10.0f * dpi, value_fg);
        draw_label(r, status_sub, text_x,
                   y + 10.0f * dpi + ch + 2.0f * dpi, dim_fg);
        y += card_h + 12.0f * dpi;

        /* Stats card — only when initialised. Single line with totals
         * and a comma-separated kind breakdown so it stays compact. */
        if (initialized) {
            /* Tally via a GROUP BY aggregate instead of a 130 KB malloc +
             * full-table list/decrypt of every secret just to count kinds. */
            i32 counts[5] = {0};
            i32 n = vault_secret_counts(v, counts, 5);
            f32 stats_h = ch * 2.0f + 18.0f * dpi;
            s_rrect_simple(r, content_x, y, panel_inner_w, stats_h,
                                       cp.surface_neutral, card_radius);
            renderer_flush_rrects(r);
            char head[64];
            snprintf(head, sizeof head, "%d encrypted secret%s",
                     n, n == 1 ? "" : "s");
            draw_label(r, head, content_x + 18.0f * dpi,
                       y + 9.0f * dpi, value_fg);
            char detail[160];
            snprintf(detail, sizeof detail,
                     "%d password · %d passphrase · %d key · %d env · %d note",
                     counts[0], counts[1], counts[2], counts[3], counts[4]);
            draw_label(r, detail, content_x + 18.0f * dpi,
                       y + 9.0f * dpi + ch + 2.0f * dpi, dim_fg);
            y += stats_h + 12.0f * dpi;
        }

        /* Auto-lock row — rounded chips for ± instead of bare glyphs. */
        f32 row_h = ch + 14.0f * dpi;
        draw_label(r, "Auto-lock", content_x + 4.0f * dpi,
                   y + (row_h - ch) * 0.5f, label_fg);
        char lockbuf[32];
        i32 mins = sp->config->vault_auto_lock_minutes;
        if (mins <= 0) snprintf(lockbuf, sizeof lockbuf, "Never");
        else           snprintf(lockbuf, sizeof lockbuf, "%d min", mins);
        f32 chip_w = 22.0f * dpi;
        f32 chip_h = ch + 8.0f * dpi;
        f32 chip_y = y + (row_h - chip_h) * 0.5f;
        f32 right_edge = content_x + panel_inner_w - 4.0f * dpi;
        f32 plus_x  = right_edge - chip_w;
        f32 minus_x = plus_x - 6.0f * dpi - chip_w;
        f32 val_w = (f32)strlen(lockbuf) * cw;
        f32 val_x = minus_x - 10.0f * dpi - val_w;
        draw_label(r, lockbuf, val_x, y + (row_h - ch) * 0.5f, value_fg);
        renderer_flush_glyphs(r);
        s_rrect_simple(r, minus_x, chip_y, chip_w, chip_h,
                       cp.surface_neutral, btn_radius);
        s_rrect_simple(r, plus_x, chip_y, chip_w, chip_h,
                       cp.surface_neutral, btn_radius);
        renderer_flush_rrects(r);
        draw_label(r, "-", minus_x + (chip_w - cw) * 0.5f,
                   chip_y + (chip_h - ch) * 0.5f, value_fg);
        draw_label(r, "+", plus_x + (chip_w - cw) * 0.5f,
                   chip_y + (chip_h - ch) * 0.5f, value_fg);
        y += row_h + 12.0f * dpi;

        /* Action buttons */
        f32 btn_h = ch + 14.0f * dpi;

        if (!initialized) {
            f32 cbw = panel_inner_w;
            s_rrect_simple(r, content_x, y, cbw, btn_h,
                                       cp.btn_primary_bg, btn_radius);
            renderer_flush_rrects(r);
            const char *lbl = "Create Master Password";
            f32 lblw = (f32)strlen(lbl) * cw;
            draw_label(r, lbl, content_x + (cbw - lblw) * 0.5f,
                       y + (btn_h - ch) * 0.5f, cp.btn_primary_fg);
            y += btn_h + 8.0f * dpi;
        } else if (!unlocked) {
            f32 cbw = panel_inner_w;
            s_rrect_simple(r, content_x, y, cbw, btn_h,
                                       cp.btn_primary_bg, btn_radius);
            renderer_flush_rrects(r);
            const char *lbl = "Unlock Vault";
            f32 lblw = (f32)strlen(lbl) * cw;
            draw_label(r, lbl, content_x + (cbw - lblw) * 0.5f,
                       y + (btn_h - ch) * 0.5f, cp.btn_primary_fg);
            y += btn_h + 8.0f * dpi;
        } else {
            /* Two side-by-side actions: destructive Lock + secondary Open. */
            f32 gap = 8.0f * dpi;
            f32 half_w = (panel_inner_w - gap) * 0.5f;
            Color lock_fill = {
                cp.btn_destructive_fg.r * 0.35f,
                cp.btn_destructive_fg.g * 0.20f,
                cp.btn_destructive_fg.b * 0.20f, 1.0f
            };
            s_rrect_simple(r, content_x, y, half_w, btn_h,
                                       lock_fill, btn_radius);
            f32 bx2 = content_x + half_w + gap;
            s_rrect_simple(r, bx2, y, half_w, btn_h,
                           cp.btn_secondary_bg, btn_radius);
            renderer_flush_rrects(r);
            const char *l1 = "Lock Now";
            const char *l2 = "Open Browser";
            f32 l1w = (f32)strlen(l1) * cw;
            f32 l2w = (f32)strlen(l2) * cw;
            draw_label(r, l1, content_x + (half_w - l1w) * 0.5f,
                       y + (btn_h - ch) * 0.5f, cp.btn_destructive_fg);
            draw_label(r, l2, bx2 + (half_w - l2w) * 0.5f,
                       y + (btn_h - ch) * 0.5f, cp.btn_secondary_fg);
            y += btn_h + 16.0f * dpi;

            /* Change Master Password card */
            f32 form_pad   = 14.0f * dpi;
            f32 field_h    = ch + 12.0f * dpi;
            f32 field_gap  = 6.0f * dpi;
            f32 form_h = ch + 8.0f * dpi +
                         (field_h + field_gap) * 3.0f +
                         btn_h + form_pad * 2.0f - field_gap;
            s_rrect_simple(r, content_x, y, panel_inner_w, form_h,
                                       cp.surface_neutral, card_radius);
            renderer_flush_rrects(r);

            f32 fy = y + form_pad;
            draw_label(r, "Change Master Password",
                       content_x + form_pad, fy, label_fg);
            fy += ch + 8.0f * dpi;

            f32 label_w  = 9.0f * cw;
            f32 field_x  = content_x + form_pad + label_w;
            f32 field_w  = panel_inner_w - 2.0f * form_pad - label_w;
            const char *labels[3] = {"Current", "New", "Confirm"};
            char *bufs[3] = {sp->vault_old_pw, sp->vault_new_pw, sp->vault_new_pw2};
            bool *editing[3] = {&sp->vault_editing_old_pw,
                                &sp->vault_editing_new_pw,
                                &sp->vault_editing_new_pw2};
            for (i32 i = 0; i < 3; i++) {
                draw_label(r, labels[i], content_x + form_pad,
                           fy + (field_h - ch) * 0.5f, label_fg);
                s_rrect_simple(r, field_x, fy, field_w, field_h,
                               cp.surface_sunken, input_radius);
                if (*editing[i]) {
                    /* Focus ring — single thin pixel along the bottom. */
                    s_rrect_per_corner(r, field_x, fy + field_h - 2.0f * dpi,
                                       field_w, 2.0f * dpi, accent,
                                       0.0f, 0.0f, input_radius, input_radius);
                }
                renderer_flush_rrects(r);
                char masked[32];
                i32 plen = (i32)strlen(bufs[i]);
                i32 show = plen < 20 ? plen : 20;
                for (i32 j = 0; j < show; j++) masked[j] = (char)0xE2;
                /* Use ASCII bullet '*' for portability — the atlas might
                 * not have the dot glyph at this point in startup. */
                for (i32 j = 0; j < show; j++) masked[j] = '*';
                masked[show] = '\0';
                draw_label(r, masked, field_x + 10.0f * dpi,
                           fy + (field_h - ch) * 0.5f, value_fg);
                fy += field_h + field_gap;
            }

            f32 apply_w = 22.0f * cw;
            if (apply_w > panel_inner_w - 2.0f * form_pad)
                apply_w = panel_inner_w - 2.0f * form_pad;
            s_rrect_simple(r, content_x + form_pad,
                                       fy + (field_gap - 2.0f * dpi),
                                       apply_w, btn_h,
                                       cp.btn_primary_bg, btn_radius);
            renderer_flush_rrects(r);
            const char *al = "Apply";
            f32 alw = (f32)strlen(al) * cw;
            draw_label(r, al,
                       content_x + form_pad + (apply_w - alw) * 0.5f,
                       fy + (field_gap - 2.0f * dpi) + (btn_h - ch) * 0.5f,
                       cp.btn_primary_fg);

            y += form_h + 12.0f * dpi;
        }

        /* Transient status message (success/failure of password change) */
        if (sp->vault_status[0]) {
            Color sc = (strncmp(sp->vault_status, "Error", 5) == 0)
                ? (Color){1.0f, 0.5f, 0.5f, 1.0f} : accent;
            draw_label(r, sp->vault_status, content_x, y, sc);
            y += ch + 4;
        }
        content_end_y = y + ch + 16;

    } else if (sp->active_tab == SETTINGS_TAB_ABOUT) {
        f32 y = content_y;
        draw_label(r, "Liu", content_x, y, value_fg);
        y += ch + 4;
        draw_label(r, "native terminal", content_x, y, label_fg);
        y += ch + 22;

        /* Version line — semantic version plus the short git build hash. */
        char ver[96];
        if (LIU_BUILD[0])
            snprintf(ver, sizeof ver, "Version %s  (%s)", LIU_VERSION, LIU_BUILD);
        else
            snprintf(ver, sizeof ver, "Version %s", LIU_VERSION);
        draw_label(r, ver, content_x, y, label_fg);
        y += ch + 12;

        f32 btn_gap = 2 * cw;
        f32 btn_h = ch + 8;
        f32 about_radius = fminf(btn_h * 0.5f, 8.0f * dpi);

        /* Auto-update: a single primary button whose label + action depend on
         * the mirrored updater phase, then a status / progress / error line and
         * (when an update is available) a Release Notes link. */
        const char *upd_lbl;
        bool upd_disabled = false;
        switch (sp->update_phase) {
            case UPD_AVAILABLE:
                upd_lbl = sp->update_auto_install ? "Install & Relaunch" : "Open Releases Page";
                break;
            case UPD_CHECKING:    upd_lbl = "Checking\xe2\x80\xa6";    upd_disabled = true; break;
            case UPD_DOWNLOADING: upd_lbl = "Downloading\xe2\x80\xa6"; upd_disabled = true; break;
            case UPD_VERIFYING:   upd_lbl = "Verifying\xe2\x80\xa6";   upd_disabled = true; break;
            case UPD_INSTALLING:
            case UPD_RELAUNCHING: upd_lbl = "Installing\xe2\x80\xa6";  upd_disabled = true; break;
            default:              upd_lbl = "Check for Updates";       break;
        }
        f32 upd_w = (f32)strlen(upd_lbl) * cw + 2.0f * cw + 8.0f * dpi;
        s_rrect_simple(r, content_x, y, upd_w, btn_h,
                       upd_disabled ? cp.surface_sunken : cp.btn_secondary_bg, about_radius);
        renderer_flush_rrects(r);
        draw_label(r, upd_lbl, content_x + cw + 4.0f * dpi,
                   y + (btn_h - ch) * 0.5f,
                   upd_disabled ? dim_fg : cp.btn_secondary_fg);
        sp->update_check_x = content_x; sp->update_check_y = y;
        sp->update_check_w = upd_w;     sp->update_check_h = btn_h;
        y += btn_h + 8;

        /* Status / progress / error line. */
        sp->update_notes_w = 0.0f;   /* recomputed below only when shown */
        {
            char line[256]; line[0] = '\0';
            Color line_fg = label_fg;
            if (sp->update_phase == UPD_ERROR) {
                snprintf(line, sizeof line, "%s", sp->update_err);
                line_fg = (Color){ .r = 0.88f, .g = 0.38f, .b = 0.38f, .a = 1.0f };
            } else if (sp->update_phase == UPD_DOWNLOADING && sp->update_bytes_total > 0) {
                int pct = (int)(sp->update_bytes_done * 100 / sp->update_bytes_total);
                if (pct > 100) pct = 100;
                snprintf(line, sizeof line, "Downloading\xe2\x80\xa6 %d%%", pct);
            } else if (sp->update_status[0]) {
                snprintf(line, sizeof line, "%s", sp->update_status);
            }
            if (line[0]) {
                draw_label(r, line, content_x, y, line_fg);
                y += ch + 6;
            }
            if (sp->update_phase == UPD_AVAILABLE && sp->update_notes[0]) {
                const char *nl = "Release Notes";
                draw_label(r, nl, content_x, y, cp.btn_primary_bg);
                sp->update_notes_x = content_x; sp->update_notes_y = y;
                sp->update_notes_w = (f32)strlen(nl) * cw; sp->update_notes_h = ch;
                y += ch + 6;
            }
        }
        y += 12;

        /* Config Export / Edit buttons — rounded pills, matching widths AND
         * centred labels so the pair reads as a tidy two-button row. btn_w is
         * sized to fit the longest label with symmetric padding. */
        const char *exp_lbl  = "Config Export";
        const char *edit_lbl = "Config Edit";
        f32 exp_w  = (f32)strlen(exp_lbl)  * cw;
        f32 edit_w = (f32)strlen(edit_lbl) * cw;
        f32 longest = exp_w > edit_w ? exp_w : edit_w;
        f32 btn_w = longest + 2.0f * cw + 8.0f * dpi;
        f32 edit_x = content_x + btn_w + btn_gap;
        s_rrect_simple(r, content_x, y, btn_w, btn_h,
                       cp.btn_secondary_bg, about_radius);
        s_rrect_simple(r, edit_x, y, btn_w, btn_h,
                       cp.btn_secondary_bg, about_radius);
        renderer_flush_rrects(r);
        draw_label(r, exp_lbl,
                   content_x + (btn_w - exp_w) * 0.5f,
                   y + (btn_h - ch) * 0.5f, cp.btn_secondary_fg);
        draw_label(r, edit_lbl,
                   edit_x + (btn_w - edit_w) * 0.5f,
                   y + (btn_h - ch) * 0.5f, cp.btn_secondary_fg);
        sp->about_export_x = content_x; sp->about_export_y = y;
        sp->about_export_w = btn_w;     sp->about_export_h = btn_h;
        sp->about_edit_x = edit_x;      sp->about_edit_y = y;
        sp->about_edit_w = btn_w;       sp->about_edit_h = btn_h;
        y += btn_h + 20;

        /* Notes Vault folder — where Open Note / Graph / AI-generated docs
         * live. Shows the current path ($HOME→~) and a folder picker. */
        {
            draw_label(r, "Notes Vault", content_x, y, label_fg);
            y += ch + 4;
            char shown[1100];
            const char *vp = sp->config->notes_vault_path;
            if (vp && vp[0]) {
                const char *home = getenv("HOME");
                size_t hl = home ? strlen(home) : 0;
                if (hl && strncmp(vp, home, hl) == 0 && (vp[hl] == '/' || vp[hl] == '\0'))
                    snprintf(shown, sizeof shown, "~%s", vp + hl);
                else
                    snprintf(shown, sizeof shown, "%s", vp);
            } else {
                snprintf(shown, sizeof shown, "~/Library/Application Support/Liu/Vault (default)");
            }
            Color path_fg = label_fg; path_fg.a *= 0.7f;
            draw_label(r, shown, content_x, y, path_fg);
            y += ch + 8;
            const char *vlbl = "Change Folder\xe2\x80\xa6";
            f32 vw = (f32)strlen(vlbl) * cw + 2.0f * cw + 8.0f * dpi;
            s_rrect_simple(r, content_x, y, vw, btn_h, cp.btn_secondary_bg, about_radius);
            renderer_flush_rrects(r);
            draw_label(r, vlbl, content_x + cw + 4.0f * dpi,
                       y + (btn_h - ch) * 0.5f, cp.btn_secondary_fg);
            sp->vault_btn_x = content_x; sp->vault_btn_y = y;
            sp->vault_btn_w = vw;        sp->vault_btn_h = btn_h;
            y += btn_h + 20;
        }

        /* Footer signature — "made by calculus.team"; the brand is an accent-
         * coloured link that opens the site (rect cached for the About click
         * handler in settings_handle_click). */
        Color made_fg = label_fg; made_fg.a *= 0.7f;
        const char *made_prefix = "made by ";
        const char *made_brand  = "calculus.team";
        draw_label(r, made_prefix, content_x, y, made_fg);
        f32 made_brand_x = content_x + (f32)strlen(made_prefix) * cw;
        draw_label(r, made_brand, made_brand_x, y, cp.btn_primary_bg);
        sp->about_made_x = made_brand_x;            sp->about_made_y = y;
        sp->about_made_w = (f32)strlen(made_brand) * cw; sp->about_made_h = ch;
        y += ch;

        content_end_y = y + 16;
    }

    /* Recompute max_scroll from actual content height vs visible area */
    {
        f32 actual_h    = content_end_y - content_y;
        f32 available_h = (clip_bottom - clip_top) - 16.0f;
        i32 new_max = actual_h > available_h
            ? (i32)((actual_h - available_h) / (ch * 0.5f)) + 1
            : 0;
        sp->max_scroll = new_max;
        if (sp->scroll_y > sp->max_scroll) sp->scroll_y = sp->max_scroll;
    }

    renderer_pop_scissor(r);
    renderer_reset_ui_scale(r);
}

/* Commit any in-progress Translate API inline editor into config and
 * clear its editing flag. Called on click-away, nav-tab switch and any
 * other focus-loss path so a half-typed key/URL/model is never silently
 * discarded (matches the click-away-commit behaviour in the tab body). */
static void settings_commit_api_editors(SettingsPanel *sp) {
    TranslateConfig *tc = &sp->config->translate;
    if (sp->editing_api_key) {
        snprintf(tc->api_key, sizeof(tc->api_key), "%s", sp->api_key_buf);
        sp->editing_api_key = false;
    }
    if (sp->editing_api_url) {
        snprintf(tc->api_base_url, sizeof(tc->api_base_url), "%s",
                 sp->api_url_buf);
        sp->editing_api_url = false;
    }
    if (sp->editing_api_model) {
        snprintf(tc->api_model, sizeof(tc->api_model), "%s",
                 sp->api_model_buf);
        sp->editing_api_model = false;
    }
}

bool settings_handle_click(SettingsPanel *sp, f32 x, f32 y, f32 sw, f32 sh,
                            f32 dpi, f32 cw, f32 ch) {
    if (!sp->open) return false;

    cw = 8.0f * dpi;
    ch = 16.0f * dpi;
    SettingsLayout sl = settings_layout_for(sw, sh, dpi, 1.0f, 0.0f);
    f32 panel_w = sl.panel_w;
    f32 panel_h = sl.panel_h;
    f32 px = sl.px;
    f32 py = sl.py;

    /* Outside panel = close */
    if (x < px || x > px + panel_w || y < py || y > py + panel_h) {
        sp->open = false;
        return true;
    }

    /* Left navigation clicks */
    f32 title_h = sl.title_h;
    f32 nav_y = py + title_h + 8.0f * dpi;
    f32 nav_row_h = sl.nav_row_h;
    if (x >= px && x < px + sl.nav_w &&
        y >= nav_y && y < nav_y + nav_row_h * SETTINGS_TAB_COUNT) {
        i32 clicked = (i32)((y - nav_y) / nav_row_h);
        if (clicked >= 0 && clicked < SETTINGS_TAB_COUNT) {
            /* Commit any in-progress API editor before leaving the tab,
             * matching the content-area click-away-commit. */
            settings_commit_api_editors(sp);
            sp->active_tab = (SettingsTab)clicked;
            sp->scroll_y   = 0;
            sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
            sp->translate_model_picker_open = false;
        }
        return true;
    }

    /* Content area — fixed settings UI metrics, apply scroll */
    f32 content_x = sl.content_x;
    f32 content_w = sl.content_w;
    f32 scroll_off = (f32)sp->scroll_y * ch * 0.5f;
    f32 content_y = sl.content_top + settings_content_top_off(ch, dpi) - scroll_off;
    f32 row_h = 32 * dpi;

    if (sp->active_tab == SETTINGS_TAB_APPEARANCE) {
        f32 item_h = ch + 10;
        f32 col2 = content_x + 14 * cw;
        f32 panel_w2 = content_w;
        i32 font_cols = content_w >= 420.0f * dpi ? 3 :
                        content_w >= 260.0f * dpi ? 2 : 1;
        f32 font_row_h = ch + 16.0f * dpi;   /* same card height as themes */
        f32 font_gap = 6.0f * dpi;
        i32 theme_cols = content_w >= 640.0f * dpi ? 3 :   /* preview cards need width; cap at 3 */
                         content_w >= 440.0f * dpi ? 3 :
                         content_w >= 260.0f * dpi ? 2 : 1;
        f32 theme_row_h = ch + 16.0f * dpi;
        f32 theme_gap = 6.0f * dpi;
        f32 ry = content_y;

        /* Font label row — mirror the render-side pill geometry. */
        f32 add_btn_pad_x = 14;
        f32 add_btn_label_chars = 18;  /* "+  Add Custom Font" */
        f32 add_btn_w = add_btn_pad_x + add_btn_label_chars * cw + add_btn_pad_x;
        if (add_btn_w > content_w * 0.55f) add_btn_w = content_w * 0.55f;
        f32 add_btn_h = ch + 12;
        f32 add_btn_x = content_x + content_w - add_btn_w;
        f32 add_btn_y = ry - (add_btn_h - ch) * 0.5f;

        /* Add custom font button */
        if (y >= add_btn_y && y < add_btn_y + add_btn_h &&
            x >= add_btn_x && x < add_btn_x + add_btn_w) {
            const char *filepath = platform_open_file_dialog("Add Custom Font", "ttf,otf,ttc,otc");
            if (filepath) {
                char imported_path[1024];
                if (settings_import_font_file(filepath, imported_path, sizeof(imported_path))) {
                    snprintf(sp->config->font_path, sizeof(sp->config->font_path), "%s", imported_path);
                    sp->needs_font_reload = true;
                }
            }
            return true;
        }
        ry += item_h + 12.0f * dpi;

        /* Font grid */
        ry += 12.0f * dpi; /* separator + breathing room */
        f32 col_w = panel_w2 / (f32)font_cols;
        for (i32 fi = 0; fi < g_font_count; fi++) {
            f32 fx = content_x + (f32)(fi % font_cols) * col_w;
            f32 fy = ry + (f32)(fi / font_cols) * (font_row_h + font_gap);
            f32 pill_x = fx - 2.0f * dpi;
            f32 pill_y = fy - 4.0f * dpi;
            if (y >= pill_y && y < pill_y + font_row_h &&
                x >= pill_x && x < pill_x + col_w - 8.0f * dpi) {
                snprintf(sp->config->font_path, sizeof(sp->config->font_path),
                         "%s", g_fonts[fi].path);
                sp->needs_font_reload = true;
                return true;
            }
        }
        ry += ((g_font_count + font_cols - 1) / font_cols) *
              (font_row_h + font_gap) + 16.0f * dpi + 10;

        /* Size row — segmented stepper: each half of the box is a target.
         * Geometry mirrors APPEAR_STEPPER in settings_render exactly. */
        f32 seg_x = col2 + 6.5f * cw;
        f32 seg_w = 6.0f * cw;
        if (y >= ry && y < ry + item_h) {
            if (x >= seg_x && x < seg_x + seg_w * 0.5f) {
                if (sp->config->font_size > 6.0f) { sp->config->font_size -= 1.0f; sp->needs_font_reload = true; }
                return true;
            }
            if (x >= seg_x + seg_w * 0.5f && x < seg_x + seg_w) {
                sp->config->font_size += 1.0f; sp->needs_font_reload = true;
                return true;
            }
        }
        ry += item_h + 6.0f * dpi;

        /* Line height row — same segmented zones */
        if (y >= ry && y < ry + item_h) {
            if (x >= seg_x && x < seg_x + seg_w * 0.5f) {
                if (sp->config->cell_height_scale > 0.8f) {
                    sp->config->cell_height_scale -= 0.05f;
                    sp->needs_font_reload = true;
                }
                return true;
            }
            if (x >= seg_x + seg_w * 0.5f && x < seg_x + seg_w) {
                if (sp->config->cell_height_scale < 2.0f) {
                    sp->config->cell_height_scale += 0.05f;
                    sp->needs_font_reload = true;
                }
                return true;
            }
        }
        ry += item_h + 8 + 10;

        /* Theme grid. Hit zones must stay aligned with the
         * renderer above; if you change one, change both. */
        ry += ch + 16;
        i32 theme_count;
        const char **theme_names = theme_list_names(&theme_count);
        f32 tcol_w = panel_w2 / (f32)theme_cols;
        f32 del_btn_sz = ch + 4.0f * dpi;
        f32 del_btn_pad_right = 8.0f * dpi;
        for (i32 i = 0; i < theme_count; i++) {
            bool is_user_theme = (i >= THEME_COUNT);
            f32 tx2 = content_x + (f32)(i % theme_cols) * tcol_w;
            f32 ty2 = ry + (f32)(i / theme_cols) * (theme_row_h + theme_gap);
            f32 pill_x = tx2 - 2.0f * dpi;
            f32 pill_w = tcol_w - 8.0f * dpi;
            f32 pill_y = ty2 - 4.0f * dpi;
            f32 pill_h = theme_row_h;
            if (y >= pill_y && y < pill_y + pill_h &&
                x >= pill_x && x < pill_x + pill_w) {
                /* Delete button hit-zone — same geometry as the
                 * renderer (right-edge inset, square pill). */
                if (is_user_theme) {
                    f32 bx = pill_x + pill_w - del_btn_sz - del_btn_pad_right;
                    f32 by = pill_y + (pill_h - del_btn_sz) * 0.5f;
                    if (x >= bx && x < bx + del_btn_sz &&
                        y >= by && y < by + del_btn_sz) {
                        snprintf(sp->theme_to_delete,
                                 sizeof(sp->theme_to_delete),
                                 "%s", theme_names[i]);
                        return true;
                    }
                }
                sp->config->theme = theme_get_by_name(theme_names[i]);
                snprintf(sp->config->theme_name, sizeof(sp->config->theme_name), "%s", theme_names[i]);
                /* Style overrides (opacity / cursor / bold-is-bright) only
                 * apply when the theme switches — running through this
                 * function on every settings tweak would clobber later
                 * manual changes. needs_layout still triggers the
                 * full re-layout + window opacity push from main.c. */
                theme_apply_style_overrides(sp->config->theme, sp->config);
                sp->needs_layout = true;
                return true;
            }
        }
        f32 grid_end_y = ry + ((theme_count + theme_cols - 1) / theme_cols) *
                         (theme_row_h + theme_gap) + 16.0f * dpi;

        /* Create / Import action buttons. Same widths as the renderer. */
        f32 btn_h = ch + 12;
        f32 btn_pad_x = 14;
        f32 btn_gap = 8;
        f32 create_label_chars = 15;
        f32 import_label_chars = 15;
        f32 create_w = btn_pad_x + create_label_chars * cw + btn_pad_x;
        f32 import_w = btn_pad_x + import_label_chars * cw + btn_pad_x;
        if (create_w + btn_gap + import_w > content_w) {
            create_w = (content_w - btn_gap) * 0.5f;
            import_w = create_w;
            if (create_w < 1.0f) create_w = import_w = 1.0f;
        }
        if (y >= grid_end_y && y < grid_end_y + btn_h) {
            if (x >= content_x && x < content_x + create_w) {
                sp->requests_create_theme = true;
                return true;
            }
            f32 ix = content_x + create_w + btn_gap;
            if (x >= ix && x < ix + import_w) {
                /* Reuse the existing Import Theme... pipeline by piggy-
                 * backing on platform_open_file_dialog directly. */
                const char *filepath = platform_open_file_dialog(
                    "Import Theme",
                    "itermcolors,yml,yaml,toml,json");
                if (filepath) {
                    Theme imported = {0};
                    if (theme_import_file(filepath, &imported)) {
                        theme_save_user(&imported);
                        theme_load_user_themes();
                        const Theme *ti = theme_get_by_name(imported.name);
                        if (ti) {
                            sp->config->theme = ti;
                            snprintf(sp->config->theme_name,
                                     sizeof(sp->config->theme_name),
                                     "%s", imported.name);
                            theme_apply_style_overrides(ti, sp->config);
                            sp->needs_layout = true;
                        }
                    }
                }
                return true;
            }
        }
    }

    if (sp->active_tab == SETTINGS_TAB_TERMINAL) {
        f32 ry = content_y;
        f32 value_off = fminf(18.0f * cw, content_w * 0.42f);
        if (value_off > content_w - 8.0f * cw) value_off = content_w - 8.0f * cw;
        if (value_off < 0.0f) value_off = 0.0f;
        f32 value_x = content_x + value_off;
        f32 value_room = content_x + content_w - value_x;
        ry += ch + 12.0f * dpi;       /* past "CURSOR" label */

        /* Cursor style row */
        if (y >= ry && y < ry + row_h) {
            f32 seg_w = 10.0f * cw;
            if (value_room > 0.0f && seg_w * 3.0f > value_room) {
                seg_w = value_room / 3.0f;
            }
            if (seg_w < 1.0f) seg_w = 1.0f;
            if (x >= value_x && x < value_x + seg_w * 3.0f) {
                sp->config->cursor_style = (i32)((x - value_x) / seg_w);
                return true;
            }
            return false;   /* empty space beside the control is inert */
        }
        ry += row_h;

        /* Cursor blink row */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + 7.0f * cw) {
            sp->config->cursor_blink = !sp->config->cursor_blink;
            return true;
        }
        ry += row_h;

        /* "BEHAVIOR" header */
        ry += 12.0f * dpi;
        ry += ch + 12.0f * dpi;

        /* Scrollback row — skip (not togglable by click) */
        ry += row_h;

        /* Bold bright */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + 7.0f * cw) {
            sp->config->bold_is_bright = !sp->config->bold_is_bright;
            return true;
        }
        ry += row_h;

        /* Copy on select */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + 7.0f * cw) {
            sp->config->copy_on_select = !sp->config->copy_on_select;
            return true;
        }
        ry += row_h;

        /* Option as alt */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + 7.0f * cw) {
            sp->config->option_as_alt = !sp->config->option_as_alt;
            return true;
        }
        ry += row_h;

        /* Tab bar toggle */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + 7.0f * cw) {
            sp->config->show_tab_bar = !sp->config->show_tab_bar;
            sp->needs_layout = true;
            return true;
        }
        ry += row_h;

        /* Toolbar icons toggle */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + 7.0f * cw) {
            sp->config->show_toolbar_icons = !sp->config->show_toolbar_icons;
            sp->needs_layout = true;
            return true;
        }
        ry += row_h;

        /* Scrollbar toggle */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + 7.0f * cw) {
            sp->config->show_scrollbar = !sp->config->show_scrollbar;
            return true;
        }
        ry += row_h;

        /* Status bar toggle */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + 7.0f * cw) {
            sp->config->show_status_bar = !sp->config->show_status_bar;
            sp->needs_layout = true;
            return true;
        }
        ry += row_h;

        /* Close guard (confirm close while an agent CLI runs) */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + 7.0f * cw) {
            sp->config->confirm_close_agent = !sp->config->confirm_close_agent;
            return true;
        }
        ry += row_h;

        /* Tab sleep preset chips */
        if (y >= ry && y < ry + row_h) {
            static const f32 ts_presets[] = {0.0f, 5.0f, 10.0f, 20.0f, 30.0f, 60.0f};
            const i32 n = (i32)(sizeof(ts_presets) / sizeof(ts_presets[0]));
            f32 chip_x = value_x;
            f32 chip_w = 5.0f * cw;
            if (value_room > 0.0f && chip_w * (f32)n > value_room) {
                chip_w = value_room / (f32)n;
            }
            if (chip_w < 1.0f) chip_w = 1.0f;
            f32 chip_gap = 0.0f;
            for (i32 pi = 0; pi < n; pi++) {
                if (x >= chip_x && x < chip_x + chip_w) {
                    sp->config->tab_sleep_idle_minutes = ts_presets[pi];
                    return true;
                }
                chip_x += chip_w + chip_gap;
            }
            return true;
        }
        ry += row_h;
        /* Caption row (no-op) */
        ry += row_h;

        /* Opacity — click to edit */
        if (y >= ry && y < ry + row_h) {
            sp->editing_opacity = true;
            snprintf(sp->opacity_buf, sizeof(sp->opacity_buf), "%.0f", sp->config->opacity * 100);
            sp->opacity_buf_len = (i32)strlen(sp->opacity_buf);
            return true;
        }
    }

    if (sp->active_tab == SETTINGS_TAB_TRANSLATE) {
        TranslateConfig *tc = &sp->config->translate;
        translate_normalize_direction(tc);
        f32 ry = content_y;
        /* Switch rects start at the shared value column; clicks beside a
         * switch (same row, empty space) must stay inert. */
        f32 tr_value_off = fminf(18.0f * cw, content_w * 0.42f);
        if (tr_value_off > content_w - 8.0f * cw) tr_value_off = content_w - 8.0f * cw;
        if (tr_value_off < 0.0f) tr_value_off = 0.0f;
        f32 tr_value_x = content_x + tr_value_off;
        ry += ch + 12.0f * dpi;  /* past "TRANSLATE-ON-TAB" header + section rule */

        /* Inline API editors commit on any click — including one on their
         * own row, whose handler below just reopens the editor over the
         * committed value (net effect: commit and keep editing). */
        settings_commit_api_editors(sp);

        /* Enable */
        if (y >= ry && y < ry + row_h &&
            x >= tr_value_x && x < tr_value_x + 7.0f * cw) {
            tc->enabled = !tc->enabled;
            return true;
        }
        ry += row_h;

        /* Backend — segmented chip: click a segment to select it directly
         * (Agent CLI / Local model / API). Re-clicking the segment that's
         * already active is a no-op. Geometry mirrors the render exactly. */
        if (y >= ry && y < ry + row_h) {
            f32 value_off = fminf(18.0f * cw, content_w * 0.42f);
            if (value_off > content_w - 8.0f * cw) value_off = content_w - 8.0f * cw;
            if (value_off < 0.0f) value_off = 0.0f;
            f32 value_x = content_x + value_off;
            f32 value_room = content_x + content_w - value_x;
            f32 seg_w = 10.0f * cw;
            if (value_room > 0.0f && seg_w * 3.0f > value_room) {
                seg_w = value_room / 3.0f;
            }
            if (seg_w < 1.0f) seg_w = 1.0f;
            if (x >= value_x && x < value_x + seg_w * 3.0f) {
                i32 seg = (i32)((x - value_x) / seg_w);
                TranslateBackend nb = (seg == 0) ? TRANSLATE_BACKEND_AGENT
                                    : (seg == 1) ? TRANSLATE_BACKEND_LOCAL
                                                 : TRANSLATE_BACKEND_API;
                if (nb != tc->backend) {
                    tc->backend = nb;
                    /* The dropdown's content (and anchor row) changes per
                     * backend; never carry the popup across a switch. */
                    sp->translate_model_picker_open = false;
                    sp->translate_model_menu_scroll = 0;
                }
            }
            return true;
        }
        ry += row_h;

        /* Agent + Model hit-test only when those rows actually rendered —
         * otherwise the From/To rows would be offset by two phantom slots
         * and absorb clicks meant for them. */
        if (tc->backend == TRANSLATE_BACKEND_AGENT) {
            /* Agent → segmented click (4 segments: Claude / OpenCode /
             * Codex / Grok). Segment hit boundaries mirror the render
             * math exactly — keep the same seg_w divisor as render. */
            if (y >= ry && y < ry + row_h) {
                f32 value_off = fminf(18.0f * cw, content_w * 0.42f);
                if (value_off > content_w - 8.0f * cw) value_off = content_w - 8.0f * cw;
                if (value_off < 0.0f) value_off = 0.0f;
                f32 value_x = content_x + value_off;
                f32 value_room = content_x + content_w - value_x;
                f32 seg_w = 9.0f * cw;
                if (value_room > 0.0f && seg_w * 4.0f > value_room) {
                    seg_w = value_room / 4.0f;
                }
                if (seg_w < 1.0f) seg_w = 1.0f;
                const char *ids[] = {"claude", "opencode", "codex", "grok"};
                for (i32 i = 0; i < 4; i++) {
                    f32 sx = value_x + (f32)i * seg_w;
                    if (x >= sx && x < sx + seg_w) {
                        if (strcmp(tc->agent_id, ids[i]) != 0) {
                            snprintf(tc->agent_id, sizeof(tc->agent_id),
                                     "%s", ids[i]);
                            /* Switching agents invalidates the previous model
                             * string — each CLI has its own naming scheme. */
                            tc->agent_model[0] = '\0';
                            /* Close the model picker (preset list changed). */
                            sp->translate_model_picker_open = false;
                            sp->translate_model_menu_scroll = 0;
                        }
                        return true;
                    }
                }
                return true;
            }
            ry += row_h;

            /* Model → toggle the preset dropdown. The list itself is hit-
             * tested below if the picker was already open. Opening the
             * model picker auto-closes the language picker (mutually
             * exclusive overlays). */
            if (y >= ry && y < ry + row_h) {
                sp->translate_model_picker_open = !sp->translate_model_picker_open;
                sp->translate_model_menu_scroll = 0;
                if (sp->translate_model_picker_open) {
                    sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
                }
                return true;
            }
            ry += row_h;
        } else if (tc->backend == TRANSLATE_BACKEND_API) {
            bool custom = strcmp(tc->api_provider, "custom") == 0;

            /* Provider → segmented click (Anthropic / OpenAI / OpenRouter /
             * Custom). Geometry mirrors the render exactly. */
            if (y >= ry && y < ry + row_h) {
                f32 value_off = fminf(18.0f * cw, content_w * 0.42f);
                if (value_off > content_w - 8.0f * cw) value_off = content_w - 8.0f * cw;
                if (value_off < 0.0f) value_off = 0.0f;
                f32 value_x = content_x + value_off;
                f32 value_room = content_x + content_w - value_x;
                /* 11cw — mirrors the render width (OpenRouter overflow fix). */
                f32 seg_w = 11.0f * cw;
                if (value_room > 0.0f && seg_w * 4.0f > value_room) {
                    seg_w = value_room / 4.0f;
                }
                if (seg_w < 1.0f) seg_w = 1.0f;
                const char *ids[] = {"anthropic", "openai", "openrouter",
                                     "custom"};
                for (i32 i = 0; i < 4; i++) {
                    f32 sx = value_x + (f32)i * seg_w;
                    if (x >= sx && x < sx + seg_w) {
                        if (strcmp(tc->api_provider, ids[i]) != 0) {
                            snprintf(tc->api_provider,
                                     sizeof(tc->api_provider), "%s", ids[i]);
                            /* Provider switch invalidates the model id —
                             * every provider has its own naming scheme. */
                            tc->api_model[0] = '\0';
                            sp->translate_model_picker_open = false;
                            sp->translate_model_menu_scroll = 0;
                        }
                        return true;
                    }
                }
                return true;
            }
            ry += row_h;

            /* API Key → open the masked inline editor seeded with the
             * current key (the click-away commit above already ran, so
             * `editing` is false here even on a re-click). */
            if (y >= ry && y < ry + row_h) {
                if (x >= tr_value_x) {
                    sp->editing_api_key = true;
                    snprintf(sp->api_key_buf, sizeof(sp->api_key_buf), "%s",
                             tc->api_key);
                    sp->translate_model_picker_open = false;
                    sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
                }
                return true;
            }
            ry += row_h;

            /* Model → toggle the provider-model dropdown, OR fall back to a
             * free-text editor when there is nothing to pick from (a custom
             * server that exposes no /v1/models list). Without this a custom
             * provider could never have its model set, so translate_api_ready
             * would refuse forever. */
            if (y >= ry && y < ry + row_h) {
                const char *opts[SETTINGS_MODEL_OPTS_CAP];
                i32 nopt = settings_translate_model_options(
                    tc, opts, SETTINGS_MODEL_OPTS_CAP);
                if (nopt < 1) {
                    if (x >= tr_value_x) {
                        sp->editing_api_model = true;
                        snprintf(sp->api_model_buf, sizeof(sp->api_model_buf),
                                 "%s", tc->api_model);
                        sp->translate_model_picker_open = false;
                        sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
                    }
                    return true;
                }
                sp->translate_model_picker_open = !sp->translate_model_picker_open;
                sp->translate_model_menu_scroll = 0;
                if (sp->translate_model_picker_open) {
                    sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
                }
                return true;
            }
            ry += row_h;

            /* Base URL (custom provider only) → inline editor. */
            if (custom) {
                if (y >= ry && y < ry + row_h) {
                    if (x >= tr_value_x) {
                        sp->editing_api_url = true;
                        snprintf(sp->api_url_buf, sizeof(sp->api_url_buf),
                                 "%s", tc->api_base_url);
                        sp->translate_model_picker_open = false;
                        sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
                    }
                    return true;
                }
                ry += row_h;
            }

            /* Helper caption row — inert, but occupies a slot. */
            if (y >= ry && y < ry + row_h) return true;
            ry += row_h;
        }

        /* Source language picker */
        if (y >= ry && y < ry + row_h) {
            sp->translate_lang_picker =
                sp->translate_lang_picker == SETTINGS_LANG_PICKER_SOURCE
                    ? SETTINGS_LANG_PICKER_CLOSED
                    : SETTINGS_LANG_PICKER_SOURCE;
            /* Lang and model pickers are mutually-exclusive overlays. */
            sp->translate_model_picker_open = false;
            sp->translate_model_menu_scroll = 0;
            return true;
        }
        ry += row_h;

        /* Target language picker */
        if (y >= ry && y < ry + row_h) {
            sp->translate_lang_picker =
                sp->translate_lang_picker == SETTINGS_LANG_PICKER_TARGET
                    ? SETTINGS_LANG_PICKER_CLOSED
                    : SETTINGS_LANG_PICKER_TARGET;
            /* Lang and model pickers are mutually-exclusive overlays. */
            sp->translate_model_picker_open = false;
            sp->translate_model_menu_scroll = 0;
            return true;
        }
        ry += row_h;

        if (sp->translate_lang_picker != SETTINGS_LANG_PICKER_CLOSED) {
            f32 menu_x = content_x;
            f32 menu_h = settings_translate_lang_menu_height(ch, dpi);
            f32 menu_y = ry + 4.0f * dpi;
            i32 lang_idx = settings_translate_lang_menu_hit(x, y, menu_x, menu_y,
                                                            cw, ch, dpi);
            if (lang_idx == SETTINGS_LANG_MENU_ACTION_SWAP) {
                char tmp[sizeof(tc->source_lang)];
                snprintf(tmp, sizeof tmp, "%s", tc->source_lang);
                snprintf(tc->source_lang, sizeof(tc->source_lang), "%s",
                         tc->target_lang);
                snprintf(tc->target_lang, sizeof(tc->target_lang), "%s", tmp);
                translate_normalize_direction(tc);
                return true;
            }
            if (lang_idx == SETTINGS_LANG_MENU_ACTION_RESET) {
                snprintf(tc->source_lang, sizeof(tc->source_lang), "Turkish");
                snprintf(tc->target_lang, sizeof(tc->target_lang), "English");
                return true;
            }
            if (lang_idx >= 0) {
                const char *lang = translate_language_name(lang_idx);
                if (sp->translate_lang_picker == SETTINGS_LANG_PICKER_SOURCE) {
                    snprintf(tc->source_lang, sizeof(tc->source_lang), "%s", lang);
                } else {
                    snprintf(tc->target_lang, sizeof(tc->target_lang), "%s", lang);
                }
                translate_normalize_direction(tc);
                sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
                return true;
            }
            if (y >= menu_y && y < menu_y + menu_h) return true;
            sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
            return true;
        }

        /* Model picker dropdown — same overlay semantics as the lang
         * picker: a row click selects + closes, a click inside the
         * card but outside any row stays open, any other click closes
         * without changing the selection. */
        if (sp->translate_model_picker_open &&
            tc->backend != TRANSLATE_BACKEND_LOCAL) {
            f32 menu_w = settings_translate_model_menu_width(cw, dpi, content_w);
            /* Mirror the render anchor (value_x) and clamp order exactly so
             * the drawn card and the hit-test card coincide at every width
             * — tr_value_x carries the same value_off clamps as render. */
            f32 menu_x = tr_value_x;
            if (menu_x + menu_w > content_x + content_w)
                menu_x = content_x + content_w - menu_w;
            if (menu_x < content_x) menu_x = content_x;
            f32 menu_h = settings_translate_model_menu_height(tc, ch, dpi);
            f32 menu_y = ry + 4.0f * dpi;
            i32 row = settings_translate_model_menu_hit(sp, x, y, menu_x,
                                                        menu_y, ch, dpi);
            if (row >= 0) {
                const char *opts[SETTINGS_MODEL_OPTS_CAP];
                i32 pn = settings_translate_model_options(
                    tc, opts, SETTINGS_MODEL_OPTS_CAP);
                if (row < pn) {
                    usize slot_cap = 0;
                    char *slot = settings_translate_model_slot(tc, &slot_cap);
                    snprintf(slot, slot_cap, "%s", opts[row]);
                }
                sp->translate_model_picker_open = false;
                return true;
            }
            if (y >= menu_y && y < menu_y + menu_h &&
                x >= menu_x && x < menu_x + menu_w) {
                /* Inside the menu card but not on a row — stay open. */
                return true;
            }
            sp->translate_model_picker_open = false;
            return true;
        }

        /* Chord window — segmented chip row: click a segment to select that
         * value directly (no more click-to-cycle). Geometry is recomputed
         * exactly as the renderer lays it out (see the "Chord window" block in
         * the render path) so segment N here maps to segment N on screen. */
        if (y >= ry && y < ry + row_h) {
            static const f32 presets[] = {0.30f, 0.40f, 0.50f, 0.60f, 0.80f, 1.00f};
            const i32 n = (i32)(sizeof(presets) / sizeof(presets[0]));
            f32 value_off = fminf(18.0f * cw, content_w * 0.42f);
            if (value_off > content_w - 8.0f * cw) value_off = content_w - 8.0f * cw;
            if (value_off < 0.0f) value_off = 0.0f;
            f32 value_x = content_x + value_off;
            f32 value_room = content_x + content_w - value_x;
            f32 chip_w = 5.5f * cw;
            if (value_room > 0.0f && chip_w * (f32)n > value_room)
                chip_w = value_room / (f32)n;
            if (chip_w < 1.0f) chip_w = 1.0f;
            if (x >= value_x && x < value_x + chip_w * (f32)n) {
                i32 seg = (i32)((x - value_x) / chip_w);
                if (seg < 0) seg = 0;
                if (seg >= n) seg = n - 1;
                tc->tab_window_sec = presets[seg];
            }
            return true;
        }
        ry += row_h;

        /* Local model row (2 rows tall): download / cancel / file-picker.
         * Dispatch off the rects the renderer cached this frame; the region's
         * fixed height keeps the rows below in lockstep. */
        if (sp->tr_model_cancel_w > 0.0f &&
            x >= sp->tr_model_cancel_x &&
            x <  sp->tr_model_cancel_x + sp->tr_model_cancel_w &&
            y >= sp->tr_model_cancel_y &&
            y <  sp->tr_model_cancel_y + sp->tr_model_cancel_h) {
            model_download_cancel();
            return true;
        }
        if (sp->tr_model_dl_w > 0.0f &&
            x >= sp->tr_model_dl_x && x < sp->tr_model_dl_x + sp->tr_model_dl_w &&
            y >= sp->tr_model_dl_y && y < sp->tr_model_dl_y + sp->tr_model_dl_h) {
            model_download_start(NULL, NULL, NULL, 0);
            return true;
        }
        if (sp->tr_model_pick_w > 0.0f &&
            x >= sp->tr_model_pick_x &&
            x <  sp->tr_model_pick_x + sp->tr_model_pick_w &&
            y >= sp->tr_model_pick_y &&
            y <  sp->tr_model_pick_y + sp->tr_model_pick_h) {
            const char *fp = platform_open_file_dialog("Select GGUF Model", "gguf");
            if (fp) {
                snprintf(tc->local_model_path, sizeof(tc->local_model_path),
                         "%s", fp);
                config_save(sp->config, config_file_path());
            }
            return true;
        }
        ry += 2.0f * row_h;

        /* "ALLOWLIST" header + section rule */
        ry += 12.0f * dpi;
        ry += ch + 12.0f * dpi;

        /* Claude */
        if (y >= ry && y < ry + row_h &&
            x >= tr_value_x && x < tr_value_x + 7.0f * cw) {
            tc->active_in_claude = !tc->active_in_claude;
            return true;
        }
        ry += row_h;

        /* Codex */
        if (y >= ry && y < ry + row_h &&
            x >= tr_value_x && x < tr_value_x + 7.0f * cw) {
            tc->active_in_codex = !tc->active_in_codex;
            return true;
        }
        ry += row_h;

        /* OpenCode */
        if (y >= ry && y < ry + row_h &&
            x >= tr_value_x && x < tr_value_x + 7.0f * cw) {
            tc->active_in_opencode = !tc->active_in_opencode;
            return true;
        }
        ry += row_h;

        /* Grok */
        if (y >= ry && y < ry + row_h &&
            x >= tr_value_x && x < tr_value_x + 7.0f * cw) {
            tc->active_in_grok = !tc->active_in_grok;
            return true;
        }
    }

    if (sp->active_tab == SETTINGS_TAB_NOTIFY) {
        settings_notify_ensure_loaded(sp);
        /* Reset-to-bundled — overwrites the 4 (any, event) rules with the
         * paths to shipped wavs. Same Stop+Error sharing as first-install. */
        if (x >= sp->notify_reset_x &&
            x <  sp->notify_reset_x + sp->notify_reset_w &&
            y >= sp->notify_reset_y &&
            y <  sp->notify_reset_y + sp->notify_reset_h) {
            settings_notify_seed_defaults(sp);
            settings_notify_apply_save(sp);
            return true;
        }
        /* Per-agent hook button row. Today only the Claude entry is wired
         * into a real installer; other detected agents are in the list
         * for visibility but their rects aren't cached so they fall
         * through silently. Status string is shared across rows — a
         * successful Claude install shows up under whichever button the
         * user clicked. */
        for (i32 ai = 0; ai < sp->notify_hook_count; ai++) {
            if (x >= sp->notify_hook_x[ai] &&
                x <  sp->notify_hook_x[ai] + sp->notify_hook_w[ai] &&
                y >= sp->notify_hook_y[ai] &&
                y <  sp->notify_hook_y[ai] + sp->notify_hook_h[ai]) {
                AgentHookResult res = { .ok = false, .msg = {0} };
                const char *id = sp->notify_hook_id[ai];
                if (agent_hook_supported(id)) {
                    if (agent_hook_installed(id)) {
                        res = agent_hook_uninstall(id);
                    } else {
                        char nbin[1024];
                        resolve_liu_notify_path(nbin, sizeof nbin);
                        res = agent_hook_install(id, nbin);
                    }
                } else {
                    snprintf(res.msg, sizeof res.msg,
                             "%s: no hook API yet", id);
                }
                snprintf(sp->notify_claude_status,
                         sizeof sp->notify_claude_status, "%s", res.msg);
                return true;
            }
        }
        static const u8 kEvents[4] = {
            EVT_STOP, EVT_NOTIFY, EVT_ERROR, EVT_COMPLETE,
        };
        for (i32 i = 0; i < 4; i++) {
            u8 evt = kEvents[i];

            /* Browse — open native file picker, install rule on accept. */
            if (x >= sp->notify_browse_x[i] &&
                x <  sp->notify_browse_x[i] + sp->notify_browse_w[i] &&
                y >= sp->notify_browse_y[i] &&
                y <  sp->notify_browse_y[i] + sp->notify_browse_h[i]) {
                const char *picked = platform_open_file_dialog(
                    "Pick a sound file",
                    "wav,mp3,aiff,aif,m4a,caf,flac,ogg");
                if (picked && *picked) {
                    notify_config_apply_sound_rule(&sp->notify_cfg,
                                                   NOTIFY_MATCH_ANY, evt, picked);
                    settings_notify_apply_save(sp);
                }
                return true;
            }
            /* Test — afplay the current rule (if any). */
            if (x >= sp->notify_test_x[i] &&
                x <  sp->notify_test_x[i] + sp->notify_test_w[i] &&
                y >= sp->notify_test_y[i] &&
                y <  sp->notify_test_y[i] + sp->notify_test_h[i]) {
                const char *cur = notify_config_sound_for(&sp->notify_cfg,
                                                          NOTIFY_MATCH_ANY, evt);
                if (cur) settings_notify_play_test(cur);
                return true;
            }
            /* Clear — remove rule, save, reload daemon. */
            if (x >= sp->notify_clear_x[i] &&
                x <  sp->notify_clear_x[i] + sp->notify_clear_w[i] &&
                y >= sp->notify_clear_y[i] &&
                y <  sp->notify_clear_y[i] + sp->notify_clear_h[i]) {
                notify_config_remove_sound_rule(&sp->notify_cfg,
                                                NOTIFY_MATCH_ANY, evt);
                settings_notify_apply_save(sp);
                return true;
            }
            /* On/Off — flip the per-event master switch. When off the daemon
             * suppresses the whole event (sound + TTS + banner). */
            if (x >= sp->notify_banner_x[i] &&
                x <  sp->notify_banner_x[i] + sp->notify_banner_w[i] &&
                y >= sp->notify_banner_y[i] &&
                y <  sp->notify_banner_y[i] + sp->notify_banner_h[i]) {
                sp->notify_cfg.event_enabled[evt] = !sp->notify_cfg.event_enabled[evt];
                settings_notify_apply_save(sp);
                return true;
            }
            /* Mic — toggle recording a custom sound for this event. */
            if (x >= sp->notify_mic_x[i] &&
                x <  sp->notify_mic_x[i] + sp->notify_mic_w[i] &&
                y >= sp->notify_mic_y[i] &&
                y <  sp->notify_mic_y[i] + sp->notify_mic_h[i]) {
                sp->notify_record_status[0] = '\0';
                if (sp->notify_recording_row == i) {
                    /* Stop, finalize and assign this row's recording. */
                    settings_notify_finalize_recording(sp);
                } else {
                    /* Finalize any other in-flight recording, then start. */
                    settings_notify_finalize_recording(sp);
                    PlatformMicPermission perm = platform_audio_mic_permission();
                    if (perm == PLATFORM_MIC_DENIED) {
                        /* macOS won't re-prompt once denied — send the user to
                         * the privacy pane so they can switch Liu on. (If Liu
                         * was launched from a terminal, the request is charged
                         * to the parent process; opening from Finder/Dock gives
                         * Liu its own microphone permission.) */
                        platform_open_microphone_settings();
                        snprintf(sp->notify_record_status,
                                 sizeof sp->notify_record_status,
                                 "Microphone is off for Liu \xe2\x80\x94 turn it on in "
                                 "System Settings, then reopen Liu and try again.");
                    } else {
                        /* UNKNOWN -> the system prompt appears; GRANTED -> records
                         * immediately. Both go through record_start. */
                        char rp[1024];
                        if (settings_notify_record_path(evt, rp, sizeof rp) &&
                            platform_audio_record_start(rp)) {
                            sp->notify_recording_row = i;
                            if (perm == PLATFORM_MIC_UNKNOWN) {
                                snprintf(sp->notify_record_status,
                                         sizeof sp->notify_record_status,
                                         "Allow microphone access, then recording "
                                         "will begin\xe2\x80\xa6");
                            }
                        } else {
                            snprintf(sp->notify_record_status,
                                     sizeof sp->notify_record_status,
                                     "Could not start recording \xe2\x80\x94 the "
                                     "microphone may be in use by another app.");
                        }
                    }
                }
                return true;
            }
        }
        return false;
    }

    if (sp->active_tab == SETTINGS_TAB_VAULT) {
        f32 ry = content_y;
        Vault *v = sp->vault;
        bool initialized = v && vault_is_initialized(v);
        bool unlocked    = v && vault_is_unlocked(v);
        f32 panel_inner_w = content_w;
        if (panel_inner_w < 16.0f * dpi) panel_inner_w = 16.0f * dpi;

        /* Status card — non-interactive. */
        f32 status_card_h = ch * 2.0f + 22.0f * dpi;
        ry += status_card_h + 12.0f * dpi;

        /* Stats card — non-interactive, only when initialised. */
        if (initialized) {
            f32 stats_h = ch * 2.0f + 18.0f * dpi;
            ry += stats_h + 12.0f * dpi;
        }

        /* Auto-lock row — ± chips on the right edge. */
        f32 row_h = ch + 14.0f * dpi;
        f32 chip_w = 22.0f * dpi;
        f32 chip_h = ch + 8.0f * dpi;
        f32 chip_y = ry + (row_h - chip_h) * 0.5f;
        f32 right_edge = content_x + panel_inner_w - 4.0f * dpi;
        f32 plus_x  = right_edge - chip_w;
        f32 minus_x = plus_x - 6.0f * dpi - chip_w;
        if (y >= chip_y && y < chip_y + chip_h) {
            if (x >= minus_x && x < minus_x + chip_w) {
                i32 m = sp->config->vault_auto_lock_minutes;
                if (m >= 60) m = 30;
                else if (m >= 30) m = 15;
                else if (m >= 15) m = 5;
                else if (m >= 5)  m = 1;
                else              m = 0;
                sp->config->vault_auto_lock_minutes = m;
                return true;
            }
            if (x >= plus_x && x < plus_x + chip_w) {
                i32 m = sp->config->vault_auto_lock_minutes;
                if (m <= 0) m = 1;
                else if (m < 5)  m = 5;
                else if (m < 15) m = 15;
                else if (m < 30) m = 30;
                else if (m < 60) m = 60;
                sp->config->vault_auto_lock_minutes = m;
                return true;
            }
        }
        ry += row_h + 12.0f * dpi;

        f32 btn_h = ch + 14.0f * dpi;

        if (!initialized) {
            f32 cbw = panel_inner_w;
            if (y >= ry && y < ry + btn_h && x >= content_x && x < content_x + cbw) {
                sp->open = false;
                snprintf(sp->vault_status, sizeof sp->vault_status,
                         "Run 'Vault: Unlock' from palette");
                sp->vault_status_ts = 0;
                return true;
            }
            ry += btn_h + 8.0f * dpi;
        } else if (!unlocked) {
            f32 cbw = panel_inner_w;
            if (y >= ry && y < ry + btn_h && x >= content_x && x < content_x + cbw) {
                sp->open = false;
                snprintf(sp->vault_status, sizeof sp->vault_status,
                         "Run 'Vault: Unlock' from palette");
                return true;
            }
            ry += btn_h + 8.0f * dpi;
        } else {
            /* Unlocked: Lock Now + Open Browser (side-by-side halves). */
            f32 gap = 8.0f * dpi;
            f32 half_w = (panel_inner_w - gap) * 0.5f;
            if (y >= ry && y < ry + btn_h) {
                if (x >= content_x && x < content_x + half_w) {
                    vault_lock(v);
                    snprintf(sp->vault_status, sizeof sp->vault_status,
                             "Vault locked");
                    return true;
                }
                f32 bx2 = content_x + half_w + gap;
                if (x >= bx2 && x < bx2 + half_w) {
                    sp->open = false;
                    snprintf(sp->vault_status, sizeof sp->vault_status,
                             "browse");
                    return true;
                }
            }
            ry += btn_h + 16.0f * dpi;

            /* Change Master Password card (same geometry as render). */
            f32 form_pad   = 14.0f * dpi;
            f32 field_h    = ch + 12.0f * dpi;
            f32 field_gap  = 6.0f * dpi;
            f32 fy = ry + form_pad + ch + 8.0f * dpi;
            f32 label_w = 9.0f * cw;
            f32 field_x = content_x + form_pad + label_w;
            f32 field_w = panel_inner_w - 2.0f * form_pad - label_w;
            for (i32 i = 0; i < 3; i++) {
                if (y >= fy && y < fy + field_h &&
                    x >= field_x && x < field_x + field_w) {
                    sp->vault_editing_old_pw  = (i == 0);
                    sp->vault_editing_new_pw  = (i == 1);
                    sp->vault_editing_new_pw2 = (i == 2);
                    return true;
                }
                fy += field_h + field_gap;
            }
            /* Apply button — at `fy + (field_gap - 2*dpi)`, width 22*cw. */
            f32 apply_w = 22.0f * cw;
            if (apply_w > panel_inner_w - 2.0f * form_pad)
                apply_w = panel_inner_w - 2.0f * form_pad;
            f32 apply_x = content_x + form_pad;
            f32 apply_y = fy + (field_gap - 2.0f * dpi);
            if (y >= apply_y && y < apply_y + btn_h &&
                x >= apply_x && x < apply_x + apply_w) {
                if (strcmp(sp->vault_new_pw, sp->vault_new_pw2) != 0) {
                    snprintf(sp->vault_status, sizeof sp->vault_status,
                             "Error: new passwords differ");
                } else if (sp->vault_new_pw[0] == '\0') {
                    snprintf(sp->vault_status, sizeof sp->vault_status,
                             "Error: new password empty");
                } else if (!vault_change_master(v, sp->vault_old_pw,
                                                sp->vault_new_pw)) {
                    snprintf(sp->vault_status, sizeof sp->vault_status,
                             "Error: wrong current password");
                } else {
                    snprintf(sp->vault_status, sizeof sp->vault_status,
                             "Master password updated");
                }
                crypto_secure_zero(sp->vault_old_pw, sizeof sp->vault_old_pw);
                crypto_secure_zero(sp->vault_new_pw, sizeof sp->vault_new_pw);
                crypto_secure_zero(sp->vault_new_pw2, sizeof sp->vault_new_pw2);
                sp->vault_editing_old_pw = sp->vault_editing_new_pw =
                    sp->vault_editing_new_pw2 = false;
                return true;
            }
        }
        return true;
    }

    if (sp->active_tab == SETTINGS_TAB_ABOUT) {
        /* All About-tab rects are cached during render — use them directly so
         * clicks stay aligned with the (now phase-dependent) update layout. */
        #define ABOUT_HIT(pfx) (x >= sp->pfx##_x && x < sp->pfx##_x + sp->pfx##_w && \
                                y >= sp->pfx##_y && y < sp->pfx##_y + sp->pfx##_h)
        /* Primary update button — action depends on phase. */
        if (sp->update_check_w > 0.0f && ABOUT_HIT(update_check)) {
            switch (sp->update_phase) {
                case UPD_AVAILABLE:
                    if (sp->update_auto_install) sp->requests_update_install = true;
                    else                         platform_open_url(LIU_RELEASES_URL);
                    break;
                case UPD_CHECKING: case UPD_DOWNLOADING: case UPD_VERIFYING:
                case UPD_INSTALLING: case UPD_RELAUNCHING:
                    break; /* busy — ignore */
                default:
                    sp->requests_update_check = true;
                    break;
            }
            return true;
        }
        /* Release Notes link (only present while an update is available).
         * https-only — defense in depth against a tampered feed dispatching an
         * arbitrary URL scheme through NSWorkspace (updater.c already filters). */
        if (sp->update_notes_w > 0.0f && ABOUT_HIT(update_notes)) {
            if (strncmp(sp->update_notes, "https://", 8) == 0)
                platform_open_url(sp->update_notes);
            return true;
        }
        /* Config Export — save current config. */
        if (sp->about_export_w > 0.0f && ABOUT_HIT(about_export)) {
            config_save(sp->config, config_file_path());
            return true;
        }
        /* Config Edit — save then open config in the system editor (argv spawn). */
        if (sp->about_edit_w > 0.0f && ABOUT_HIT(about_edit)) {
            config_save(sp->config, config_file_path());
            platform_open_path(config_file_path());
            return true;
        }
        /* Notes Vault folder picker — pick a directory, persist it, and ask the
         * main loop to re-export $LIU_VAULT + seed the new Vault. */
        if (sp->vault_btn_w > 0.0f && ABOUT_HIT(vault_btn)) {
            const char *picked = platform_open_folder_dialog("Choose Notes Vault Folder");
            if (picked && picked[0]) {
                snprintf(sp->config->notes_vault_path,
                         sizeof sp->config->notes_vault_path, "%s", picked);
                config_save(sp->config, config_file_path());
                sp->requests_vault_resync = true;
            }
            return true;
        }
        /* "made by calculus.team" footer link — open the site. */
        if (sp->about_made_w > 0.0f && ABOUT_HIT(about_made)) {
            platform_open_url("https://calculus.team");
            return true;
        }
        #undef ABOUT_HIT
    }

    if (sp->active_tab == SETTINGS_TAB_KEYS && !sp->rebinding && sp->keybinds) {
        /* Mirror the render walk in settings_render exactly: same shared table,
         * same top gap and uniform pitch. Only rebindable rows (action != NONE)
         * are click targets; section headers and read-only rows fall through.
         * Constrain to the same scissor band the render path clips to — rows
         * scrolled into the dead strip above/below the list are drawn-clipped,
         * so a click there must not rebind an invisible shortcut. Bound the
         * x-range to the row hit area the renderer pushes for hover
         * (content_x - 4*dpi .. content_x + content_w + 8*dpi) so clicks on
         * the panel's right gutter / scrollbar don't snap a random row into
         * rebind mode. */
        f32 clip_top    = sl.content_top + settings_clip_top_off(ch, dpi);
        f32 clip_bottom = sl.clip_bottom;
        f32 hit_x0      = content_x - 4.0f * dpi;
        f32 hit_x1      = content_x + content_w + 8.0f * dpi;
        if (y >= clip_top && y < clip_bottom && x >= hit_x0 && x < hit_x1) {
            f32 row_h = keys_list_row_h(ch, dpi);
            f32 ry    = content_y + keys_list_top_gap(ch, dpi);
            for (i32 i = 0; i < g_keys_row_count; i++) {
                const KeysRow *kr = &g_keys_rows[i];
                if (kr->action != ACT_NONE && y >= ry && y < ry + row_h) {
                    sp->rebinding = true;
                    sp->rebind_action = (i32)kr->action;
                    return true;
                }
                ry += row_h;
            }
        }
    }

    return true; /* consumed click */
}

bool settings_handle_char(SettingsPanel *sp, u32 codepoint) {
    if (!sp->open) return false;

    /* Translate — API key / base URL / model inline editors accept ASCII
     * printable (keys, URLs and model ids are ASCII by construction).
     * Gated on backend==API so a stale editor flag (left set by the
     * palette "Cycle Backend" command) can't swallow keys under another
     * backend. */
    if (sp->active_tab == SETTINGS_TAB_TRANSLATE &&
        sp->config->translate.backend == TRANSLATE_BACKEND_API &&
        (sp->editing_api_key || sp->editing_api_url || sp->editing_api_model)) {
        char *buf; usize cap;
        if (sp->editing_api_key)        { buf = sp->api_key_buf;   cap = sizeof(sp->api_key_buf); }
        else if (sp->editing_api_url)   { buf = sp->api_url_buf;   cap = sizeof(sp->api_url_buf); }
        else                            { buf = sp->api_model_buf; cap = sizeof(sp->api_model_buf); }
        /* '"' and '\\' are excluded: config.json is written with a raw
         * fprintf("%s") (no JSON escaping) and they'd also break the
         * curl-config quoting at spawn. Neither is valid in a key/URL/id. */
        if (codepoint >= 32 && codepoint < 127 &&
            codepoint != '"' && codepoint != '\\') {
            usize len = strlen(buf);
            if (len + 1 < cap) {
                buf[len] = (char)codepoint;
                buf[len + 1] = '\0';
            }
        }
        return true;   /* swallow everything while editing */
    }

    /* Vault — password fields accept ASCII printable. */
    if (sp->active_tab == SETTINGS_TAB_VAULT) {
        char *buf = sp->vault_editing_old_pw ? sp->vault_old_pw
                  : sp->vault_editing_new_pw ? sp->vault_new_pw
                  : sp->vault_editing_new_pw2 ? sp->vault_new_pw2 : NULL;
        if (buf && codepoint >= 32 && codepoint < 127) {
            i32 len = (i32)strlen(buf);
            if (len < 254) {
                buf[len] = (char)codepoint;
                buf[len + 1] = '\0';
            }
            return true;
        }
    }

    if (!sp->editing_opacity) return false;
    /* Only accept digits and dot */
    if ((codepoint >= '0' && codepoint <= '9') || codepoint == '.') {
        if (sp->opacity_buf_len < 6) {
            sp->opacity_buf[sp->opacity_buf_len++] = (char)codepoint;
            sp->opacity_buf[sp->opacity_buf_len] = '\0';
        }
        return true;
    }
    return false;
}

bool settings_handle_key(SettingsPanel *sp, u32 key, u32 mods) {
    if (!sp->open) return false;

    if (sp->translate_lang_picker != SETTINGS_LANG_PICKER_CLOSED &&
        (key == KEY_ESCAPE || key == 27)) {
        sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
        return true;
    }
    if (sp->translate_model_picker_open &&
        (key == KEY_ESCAPE || key == 27)) {
        sp->translate_model_picker_open = false;
        return true;
    }

    /* Translate — API key / base URL / model inline editors. Enter commits
     * into config, Escape cancels, Cmd+V pastes (keys are unpleasant to
     * type), Tab is swallowed so it can't switch Settings tabs mid-edit.
     * Gated on backend==API so a stale flag can't capture keys elsewhere. */
    if (sp->active_tab == SETTINGS_TAB_TRANSLATE &&
        sp->config->translate.backend == TRANSLATE_BACKEND_API &&
        (sp->editing_api_key || sp->editing_api_url || sp->editing_api_model)) {
        char *buf; usize bcap;
        if (sp->editing_api_key)        { buf = sp->api_key_buf;   bcap = sizeof(sp->api_key_buf); }
        else if (sp->editing_api_url)   { buf = sp->api_url_buf;   bcap = sizeof(sp->api_url_buf); }
        else                            { buf = sp->api_model_buf; bcap = sizeof(sp->api_model_buf); }
        if (key == 8 || key == 127 || key == KEY_BACKSPACE) {
            usize len = strlen(buf);
            if (len > 0) buf[len - 1] = '\0';
            return true;
        }
        if (key == KEY_V && (mods & MOD_SUPER)) {
            const char *clip = platform_clipboard_get();
            if (clip) {
                usize len = strlen(buf);
                for (const char *p = clip; *p && len + 1 < bcap; p++) {
                    u8 c = (u8)*p;
                    if (c < 32 || c >= 127) continue;   /* strip \r\n\t etc. */
                    if (c == '"' || c == '\\') continue; /* config.json/curl safe */
                    buf[len++] = (char)c;
                }
                buf[len] = '\0';
            }
            return true;
        }
        if (key == 13) {   /* enter — commit */
            TranslateConfig *tc = &sp->config->translate;
            if (sp->editing_api_key) {
                snprintf(tc->api_key, sizeof(tc->api_key), "%s", buf);
                sp->editing_api_key = false;
            } else if (sp->editing_api_url) {
                snprintf(tc->api_base_url, sizeof(tc->api_base_url), "%s",
                         buf);
                sp->editing_api_url = false;
            } else {
                snprintf(tc->api_model, sizeof(tc->api_model), "%s", buf);
                sp->editing_api_model = false;
            }
            return true;
        }
        if (key == 27 || key == KEY_ESCAPE) {   /* escape — cancel */
            sp->editing_api_key = false;
            sp->editing_api_url = false;
            sp->editing_api_model = false;
            return true;
        }
        if (key == KEY_TAB) return true;
    }

    /* Tab switching shortcuts. ⌘1..⌘7 land on a specific tab the same way
     * macOS Settings / browsers do; bare Tab / Shift+Tab cycles. Both reset
     * scroll + close any open dropdowns so the new tab opens fresh. Skipped
     * inside any text-input mode so Tab keeps its in-field meaning. */
    bool input_focused = sp->rebinding
                       || sp->editing_opacity
                       || (sp->config->translate.backend == TRANSLATE_BACKEND_API &&
                           (sp->editing_api_key || sp->editing_api_url ||
                            sp->editing_api_model))
                       || sp->vault_editing_old_pw
                       || sp->vault_editing_new_pw
                       || sp->vault_editing_new_pw2;
    if (!input_focused) {
        SettingsTab want = sp->active_tab;
        bool change = false;
        if ((mods & MOD_SUPER) && key >= KEY_1 && key <= KEY_9) {
            i32 idx = (i32)(key - KEY_1);
            if (idx < SETTINGS_TAB_COUNT) { want = (SettingsTab)idx; change = true; }
        } else if (key == KEY_TAB &&
                   !(mods & (MOD_SUPER | MOD_CTRL | MOD_ALT))) {
            i32 dir = (mods & MOD_SHIFT) ? -1 : 1;
            i32 next = ((i32)sp->active_tab + dir + SETTINGS_TAB_COUNT)
                       % SETTINGS_TAB_COUNT;
            want = (SettingsTab)next;
            change = true;
        }
        if (change && want != sp->active_tab) {
            sp->active_tab = want;
            sp->scroll_y = 0;
            sp->translate_lang_picker      = SETTINGS_LANG_PICKER_CLOSED;
            sp->translate_model_picker_open = false;
            sp->editing_api_key = false;
            sp->editing_api_url = false;
            sp->editing_api_model = false;
        }
        if (change) return true;
    }

    /* Key rebinding capture */
    if (sp->rebinding) {
        if (key == KEY_ESCAPE) {
            sp->rebinding = false;
            return true;
        }
        if (key == KEY_DELETE) {
            if (sp->keybinds)
                keybind_remove(sp->keybinds, (Action)sp->rebind_action);
            sp->rebinding = false;
            return true;
        }
        /* Ignore modifier-only presses */
        if (key >= KEY_LSHIFT && key <= KEY_RSUPER) return true;
        if (sp->keybinds) {
            keybind_set(sp->keybinds, (Action)sp->rebind_action, (KeyCode)key, mods);
        }
        sp->rebinding = false;
        return true;
    }

    /* Vault — backspace on focused password field, tab cycles. */
    if (sp->active_tab == SETTINGS_TAB_VAULT) {
        char *buf = sp->vault_editing_old_pw ? sp->vault_old_pw
                  : sp->vault_editing_new_pw ? sp->vault_new_pw
                  : sp->vault_editing_new_pw2 ? sp->vault_new_pw2 : NULL;
        if (buf && (key == 8 || key == 127 || key == KEY_BACKSPACE)) {
            i32 len = (i32)strlen(buf);
            if (len > 0) buf[len - 1] = '\0';
            return true;
        }
        if (buf && key == KEY_TAB) {
            if (sp->vault_editing_old_pw) {
                sp->vault_editing_old_pw = false;
                sp->vault_editing_new_pw = true;
            } else if (sp->vault_editing_new_pw) {
                sp->vault_editing_new_pw = false;
                sp->vault_editing_new_pw2 = true;
            } else {
                sp->vault_editing_new_pw2 = false;
                sp->vault_editing_old_pw = true;
            }
            return true;
        }
    }

    if (!sp->editing_opacity) return false;
    if (key == 8 || key == 127) { /* backspace */
        if (sp->opacity_buf_len > 0)
            sp->opacity_buf[--sp->opacity_buf_len] = '\0';
        return true;
    }
    if (key == 13) { /* enter — apply */
        f32 val = (f32)atof(sp->opacity_buf);
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        sp->config->opacity = val / 100.0f;
        sp->editing_opacity = false;
        sp->needs_layout = true; /* trigger opacity re-apply in main loop */
        return true;
    }
    if (key == 27) { /* escape — cancel */
        sp->editing_opacity = false;
        return true;
    }
    return false;
}

bool settings_handle_scroll(SettingsPanel *sp, f32 dy, bool precise, f32 dpi) {
    if (!sp->open) return false;

    /* While the Translate model dropdown is open the wheel scrolls its
     * row window, not the panel — live model lists overflow the popup. */
    if (sp->active_tab == SETTINGS_TAB_TRANSLATE &&
        sp->translate_model_picker_open &&
        sp->config->translate.backend != TRANSLATE_BACKEND_LOCAL) {
        const char *opts[SETTINGS_MODEL_OPTS_CAP];
        i32 n = settings_translate_model_options(&sp->config->translate,
                                                 opts,
                                                 SETTINGS_MODEL_OPTS_CAP);
        static f32 menu_accum = 0.0f;
        const f32 step_px = 24.0f * (dpi > 0.0f ? dpi : 1.0f);
        menu_accum += precise ? -dy / step_px : -dy;
        i32 step = (i32)menu_accum;
        menu_accum -= (f32)step;
        sp->translate_model_menu_scroll += step;
        sp->translate_model_menu_scroll =
            settings_translate_model_menu_clamp_scroll(sp, n);
        return true;
    }

    sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;

    /* scroll_y is in "half-cell" units (render path: scroll_off = scroll_y *
     * ch * 0.5). One half-cell ≈ 8 * dpi pixels at the default 16-pt font.
     * macOS NSEvent gives us pixel deltas for trackpad/Magic Mouse
     * (precise=true) and line-unit deltas for legacy wheels — treating the
     * pixel delta as raw half-cell ticks made the panel teleport on any
     * trackpad swipe, which is what the user saw as "scroll way too fast".
     * Convert both into half-cell ticks and accumulate the fractional remainder
     * so slow trackpad pans don't get rounded away. */
    static f32 accum = 0.0f;
    const f32 step_px = 8.0f * (dpi > 0.0f ? dpi : 1.0f);
    f32 ticks_f;
    if (precise) {
        ticks_f = -dy / step_px;
    } else {
        /* One wheel click ≈ 3 lines. line_h ≈ 16 * dpi → 4 half-cell ticks. */
        ticks_f = -dy * 4.0f;
    }
    accum += ticks_f;
    i32 step = (i32)accum;
    accum -= (f32)step;
    sp->scroll_y += step;
    if (sp->scroll_y < 0) sp->scroll_y = 0;
    if (sp->scroll_y > sp->max_scroll) sp->scroll_y = sp->max_scroll;
    return true;
}
