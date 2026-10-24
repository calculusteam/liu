/*
 * Liu - in-app settings panel implementation
 */
#include "ui/settings.h"
#include "ui/model_delete.h"
#include "ui/settings_layout.h"
#include "ui/ui.h"      /* AppState + app_edit* (shared field editing) */
#include "ui/anim.h"
#include "ui/icon.h"
#include "ui/agent_icons.h"   /* liu_app_icon_rgba — the About tab's app mark */
#include "ui/chrome_palette.h"
#include "core/string_utils.h"
#include "core/app_support.h"
#include "core/checked.h"
#include "core/utf8.h"
#include "core/theme_import.h"
#include "translate/model_catalog.h"
#include "translate/translate_api.h"
#include "translate/model_download.h"
#include "translate/model_paths.h"
#include "voice/voice_api.h"
#include "voice/voice_lang.h"
#include "voice/voice_paths.h"
#include "platform/platform.h"
#include "vault/crypto.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
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

/* ---- shared API inline editors ------------------------------------------
 *
 * The Translate and the Voice tab both carry an API provider block and both
 * drive the same three buffers (see SettingsPanel.api_edit_tab). Everything
 * that renders, commits or captures keys for them asks these, so a flag left
 * set by the other tab can neither paint an editor nor swallow a keystroke. */
static bool sp_api_edit_key(const SettingsPanel *sp, SettingsTab tab) {
    return sp->editing_api_key && sp->api_edit_tab == tab;
}
static bool sp_api_edit_url(const SettingsPanel *sp, SettingsTab tab) {
    return sp->editing_api_url && sp->api_edit_tab == tab;
}
static bool sp_api_edit_model(const SettingsPanel *sp, SettingsTab tab) {
    return sp->editing_api_model && sp->api_edit_tab == tab;
}

/* Open one of them, remembering which tab it belongs to. */
static void sp_api_edit_open(SettingsPanel *sp, SettingsTab tab,
                             bool *flag, char *buf, usize cap,
                             const char *seed) {
    sp->editing_api_key = sp->editing_api_url = sp->editing_api_model = false;
    sp->api_edit_tab = tab;
    *flag = true;
    snprintf(buf, cap, "%s", seed ? seed : "");
}

/* True while one of them owns the keyboard: an editor is open, its own tab is
 * the one showing, and that tab is still on the API backend. All three
 * conditions are load-bearing — the palette's Cycle Backend can change a
 * backend while Settings is closed, and a flag surviving that would swallow
 * every keystroke on a tab with no visible field to type into. */
static bool settings_api_editing(const SettingsPanel *sp) {
    if (!(sp->editing_api_key || sp->editing_api_url || sp->editing_api_model))
        return false;
    if (sp->active_tab != sp->api_edit_tab) return false;
    if (sp->api_edit_tab == SETTINGS_TAB_TRANSLATE)
        return sp->config->translate.backend == TRANSLATE_BACKEND_API;
    if (sp->api_edit_tab == SETTINGS_TAB_VOICE)
        return sp->config->voice.backend == VOICE_BACKEND_API;
    return false;
}

/* App version (defined by CMake; fall back for non-CMake builds). */
#ifndef LIU_VERSION
#define LIU_VERSION "0.1.0"
#endif
#ifndef LIU_BUILD
#define LIU_BUILD ""
#endif
/* LIU_RELEASES_URL (About-button fallback target) comes from updater.h. */
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include "notify/protocol.h"
#include "notify/claude_hooks.h"
#include "notify/agent_hooks.h"
#include "notify/notify_server.h"
#include "notify/platform_notify.h"   /* notify_play_sound */
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

/* The preferred bundled font appears first. Every bundled face is a Nerd Font
 * Mono build: the family's own glyphs plus the Nerd Fonts icons, each icon one
 * cell wide. A face whose licence reserves its name is listed under the name
 * its Nerd Fonts build carries. Any other face, a system one included, can be
 * added through "Add Custom Font". */
static const BundledFontSpec g_bundled_fonts[] = {
    {"JetBrainsMono", "JetBrainsMonoNerdFontMono-Regular.ttf"},
    {"Iosevka", "IosevkaNerdFontMono-Medium.ttf"},
    {"GeistMono", "GeistMonoNerdFontMono-Regular.otf"},
    {"MonaspiceNe", "MonaspiceNeNerdFontMono-Regular.otf"},
    {"Maple Mono", "MapleMono-NF-Regular.ttf"},
    {"CommitMono", "CommitMonoNerdFontMono-Regular.otf"},
    {"FiraCode", "FiraCodeNerdFontMono-Regular.ttf"},
    {"CaskaydiaCove", "CaskaydiaCoveNerdFontMono-SemiLight.ttf"},
    {"Hack", "HackNerdFontMono-Regular.ttf"},
    {"BlexMono", "BlexMonoNerdFontMono-Regular.ttf"},
    {"SauceCodePro", "SauceCodeProNerdFontMono-Regular.ttf"},
    {"VictorMono", "VictorMonoNerdFontMono-SemiBold.ttf"},
    {"RobotoMono", "RobotoMonoNerdFontMono-Regular.ttf"},
    {"Inconsolata", "InconsolataNerdFontMono-Regular.ttf"},
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

/* The configured font as a path the grid can compare: a bare file name names a
 * bundled face, resolved against the bundled fonts folder as font_atlas_create
 * resolves it. Anything with a '/' is returned as it is. */
static const char *settings_font_path_full(const char *path, char *buf, usize cap) {
    if (!path || !path[0] || strchr(path, '/')) return path;
    if (!liu_path_join(buf, cap, font_user_dir(), path)) return path;
    return buf;
}

/* Files in the font dirs that exist for internal use only (icon glyph
 * fallback, ligature shaping, …) and would render as garbage if picked
 * as the primary terminal font. Match by case-insensitive substring on
 * the filename. */
static bool is_blacklisted_font_file(const char *filename) {
    if (!filename) return false;
    static const char *blacklist[] = {
        "SymbolsNerdFontMono",   /* icon-only fallback font (PUA glyphs) */
        "SystemNerdFontMono",    /* alias hidden from the font list */
        NULL,
    };
    for (i32 i = 0; blacklist[i]; i++) {
        if (strcasestr(filename, blacklist[i])) return true;
    }
    return false;
}

/* A bold or italic face of another font in the same folder is that font's
 * style, not a font of its own: the renderer opens it for SGR 1 and 3 by the
 * same names ("Regular" replaced by the style word, or "-<style>" after a name
 * with no "Regular"). */
static bool is_style_face_file(const char *dir, const char *filename) {
    static const char *const words[] = { "BoldItalic", "Bold", "Italic" };
    const char *dot = strrchr(filename, '.');
    if (!dot) return false;
    for (usize w = 0; w < sizeof words / sizeof words[0]; w++) {
        const char *at = strstr(filename, words[w]);
        if (!at) continue;
        usize wl = strlen(words[w]);
        char regular[512], path[1024];
        snprintf(regular, sizeof regular, "%.*sRegular%s",
                 (int)(at - filename), filename, at + wl);
        if (liu_path_join(path, sizeof path, dir, regular) && access(path, R_OK) == 0)
            return true;
        if (at > filename && at[-1] == '-' && at + wl == dot) {
            snprintf(regular, sizeof regular, "%.*s%s",
                     (int)(at - 1 - filename), filename, dot);
            if (liu_path_join(path, sizeof path, dir, regular) && access(path, R_OK) == 0)
                return true;
        }
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
        if (is_style_face_file(dir, ent->d_name)) continue;

        char path[1024];
        char name[128];
        liu_path_join(path, sizeof(path), dir, ent->d_name);
        prettify_font_name(ent->d_name, name, sizeof(name));
        add_font_from_path(name, path);
    }
    closedir(d);
}

/* Appearance-tab preset tables.
 *
 * File-scope so the renderer and the click handler read the SAME arrays — the
 * two walk the chip rows in lockstep, and a preset list duplicated at both
 * sites is exactly how a chip ends up selecting its neighbour's value. */
static const f32   s_blur_presets[] = { 0.0f, 10.0f, 22.0f, 36.0f };
static const char *s_blur_labels[]  = { "Off", "Light", "Medium", "Strong" };

/* Renderer mode ids: 0 stretch, 1 center, 2 tile, 3 fill (aspect). Ordered
 * with the most useful first rather than by id. */
static const i32   s_fit_modes[]  = { 3, 0, 1, 2 };
static const char *s_fit_labels[] = { "Fill", "Stretch", "Center", "Tile" };


static const char *path_basename(const char *path) {
    if (!path) return "";
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

/* ---- Translate allowlist rows ---------------------------------------------
 *
 * Which agents Translate may intercept typing for. ONE table, walked by both
 * the painter and the click handler, so adding an agent is one row and a click
 * toggles the agent drawn under it.
 *
 * Fields are addressed by offset rather than pointer because the table is file
 * scope and the TranslateConfig it applies to only exists per call.
 * ------------------------------------------------------------------------- */
typedef struct { const char *label; usize flag_off; } TranslateAgentRow;

static const TranslateAgentRow g_translate_agents[] = {
    { "Claude:",      offsetof(TranslateConfig, active_in_claude)      },
    { "Codex:",       offsetof(TranslateConfig, active_in_codex)       },
    { "OpenCode:",    offsetof(TranslateConfig, active_in_opencode)    },
    { "Grok:",        offsetof(TranslateConfig, active_in_grok)        },
    { "Antigravity:", offsetof(TranslateConfig, active_in_antigravity) },
    { "Cline:",       offsetof(TranslateConfig, active_in_cline)       },
    { "Command Code:",offsetof(TranslateConfig, active_in_commandcode) },
    { "Pi:",          offsetof(TranslateConfig, active_in_pi)          },
    { "Oh My Pi:",    offsetof(TranslateConfig, active_in_omp)         },
    { "OpenClaw:",    offsetof(TranslateConfig, active_in_openclaw)    },
    { "Hermes:",      offsetof(TranslateConfig, active_in_hermes)      },
};
#define TRANSLATE_AGENT_ROW_COUNT \
    ((i32)(sizeof g_translate_agents / sizeof g_translate_agents[0]))

static bool *translate_agent_flag(TranslateConfig *tc, i32 i) {
    if (!tc || i < 0 || i >= TRANSLATE_AGENT_ROW_COUNT) return NULL;
    return (bool *)((char *)tc + g_translate_agents[i].flag_off);
}

/* ---- hover-area registry (drives pointing-hand cursor in main.c) ---- */

static void sp_push_hover(SettingsPanel *sp, f32 x, f32 y, f32 w, f32 h) {
    if (!sp) return;
    /* Clip to what the scissor actually shows — a control half-hidden under the
     * pinned header keeps the visible half clickable and loses the rest. */
    if (sp->clip_active) {
        f32 y0 = y > sp->clip_y0 ? y : sp->clip_y0;
        f32 y1 = (y + h) < sp->clip_y1 ? (y + h) : sp->clip_y1;
        if (y1 <= y0) return;
        y = y0;
        h = y1 - y0;
    }
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
    /* Under a popover only the popover's own areas count: the controls it
     * covers were pushed too, earlier in the frame, and are not reachable. */
    i32 first = 0;
    if (sp->overlay_w > 0.0f && x >= sp->overlay_x && x < sp->overlay_x + sp->overlay_w &&
        y >= sp->overlay_y && y < sp->overlay_y + sp->overlay_h) first = sp->overlay_first_area;
    for (i32 i = first; i < sp->hover_area_count; i++) {
        const f32 ax = sp->hover_areas[i].x;
        const f32 ay = sp->hover_areas[i].y;
        if (x >= ax && x < ax + sp->hover_areas[i].w &&
            y >= ay && y < ay + sp->hover_areas[i].h) return true;
    }
    return false;
}

static void settings_browser_commit(SettingsPanel *sp) {
    BrowserAllowlist *list = &sp->config->browser_allowlist;
    char rule[BROWSER_RULE_CAP];
    const char *error = NULL;
    if (!browser_nav_rule_normalize(sp->browser_rule_buf, rule, sizeof rule))
        error = "Enter a host, *.domain, or an HTTP or HTTPS URL.";
    else if (list->count < 0 || list->count > BROWSER_ALLOWLIST_MAX)
        error = "The allowed sites list is invalid.";
    else {
        for (i32 i = 0; i < list->count; i++) {
            char existing[BROWSER_RULE_CAP];
            if (i != sp->browser_rule_index &&
                browser_nav_rule_normalize(list->rules[i], existing, sizeof existing) &&
                strcmp(existing, rule) == 0) { error = "This site is already in the list."; break; }
        }
    }
    if (!error && sp->browser_rule_index >= list->count && sp->browser_rule_index >= 0)
        error = "This site was removed. Cancel and add it again.";
    if (!error && sp->browser_rule_index < 0 && list->count == BROWSER_ALLOWLIST_MAX)
        error = "The list is full. Remove a site before adding another.";
    if (error) {
        sp->browser_rule_status_error = true;
        snprintf(sp->browser_rule_status, sizeof sp->browser_rule_status, "%s", error);
        sp->browser_rule_editing = true;
        return;
    }
    i32 index = sp->browser_rule_index;
    if (index < 0 || index >= list->count) index = list->count++;
    snprintf(list->rules[index], BROWSER_RULE_CAP, "%s", rule);
    sp->browser_policy_dirty = true;
    sp->browser_rule_status_error = false;
    sp->browser_rule_editing = false;
    sp->browser_rule_index = -1;
    sp->browser_rule_buf[0] = '\0';
    snprintf(sp->browser_rule_status, sizeof sp->browser_rule_status, "Site saved. Applies to the next navigation.");
}

/* ---- liu-notify integration helpers (used by SETTINGS_TAB_NOTIFY) ---- */

/* Forward decl — ensure_loaded calls into seed_defaults, which is defined
 * further down (right after the path resolver it depends on). */
static void settings_notify_seed_defaults(SettingsPanel *sp);

/* When notify.conf is absent, first-run seeding populates the four event
 * slots with bundled sound paths and saves them. An existing config with
 * no sound rules remains empty on later loads. */
/* Bumped by every in-process write of notify.conf. There is one file and one
 * NotifyConfig PER WINDOW, so a panel holding a copy older than the newest
 * write is editing a stale snapshot: its next save would rewrite the whole
 * file from that snapshot and silently drop whatever the other window just
 * changed. `ensure_loaded` is called from both the render path and the click
 * handler before any mutation, so re-reading on a stale generation is a
 * reload-before-write. */
static u32 s_notify_cfg_gen = 1;

static void settings_notify_ensure_loaded(SettingsPanel *sp) {
    if (sp->notify_cfg_loaded) {
        if (sp->notify_cfg_gen == s_notify_cfg_gen) return;
        /* Another window wrote the file since this panel read it. Its version
         * is the truth; this panel has no unsaved deltas, because every
         * settings mutation flushes through settings_notify_apply_save
         * immediately. */
        notify_config_load_effective(&sp->notify_cfg);
        sp->notify_cfg_gen = s_notify_cfg_gen;
        return;
    }
    /* The EFFECTIVE configuration, i.e. what the server plays: a file from
     * before the "sounds were configured" marker, with no sound line in it,
     * still gets the bundled sounds there, and read raw this panel would
     * show four empty rows beside a server that is chiming. */
    bool had_file = notify_config_load_effective(&sp->notify_cfg);
    if (!had_file) {
        settings_notify_seed_defaults(sp);
        notify_config_save(&sp->notify_cfg);
        s_notify_cfg_gen++;              /* the seed IS a write */
    }
    sp->notify_cfg_loaded = true;
    sp->notify_cfg_gen = s_notify_cfg_gen;
}

/* Persist the working config and tell the in-process server to re-read it so
 * sound-rule / voice changes apply live. No-op when this process isn't the
 * one hosting the server — it will load the saved file on next launch. */
static void settings_notify_apply_save(SettingsPanel *sp) {
    if (!sp || !sp->notify_cfg_loaded) return;
    notify_config_save(&sp->notify_cfg);
    /* Publish the write: any other window's panel now reads as stale and
     * re-reads before its next edit instead of overwriting this one. */
    sp->notify_cfg_gen = ++s_notify_cfg_gen;
    notify_server_reload();
}

/* Fire-and-forget afplay so the Test button doesn't block the UI thread.
 * Double-fork so the grandchild that runs afplay is reparented to init and
 * reaped by it — otherwise the finished afplay processes pile up as zombies
 * (the UI thread never waitpid()s them). The intermediate child exits at once
 * and is reaped synchronously here, so nothing leaks. */
static void settings_notify_play_test(const char *path) {
    if (!path || !*path) return;
#ifndef PLATFORM_MACOS
    /* There is no afplay off macOS — the button did nothing. The notify
     * back-end already knows this platform's players and does not block. */
    notify_play_sound(path);
    return;
#endif
    pid_t pid = fork();
    if (pid == 0) {
        /* Intermediate child: detach the player into a grandchild. */
        pid_t grandchild = fork();
        if (grandchild == 0) {
            execl("/usr/bin/afplay", "afplay", path, (char *)NULL);
            _exit(127);
        }
        _exit(grandchild > 0 ? 0 : 127);
    }
    if (pid > 0)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
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
    /* Extension must match what the backend actually writes — the recorder is
     * platform-chosen (AAC/.m4a via AVAudioRecorder on macOS, WAV via the
     * PulseAudio/ALSA capture tools on Linux) and the player has to be able to
     * open the result. paplay/aplay cannot read .m4a. */
#ifdef PLATFORM_LINUX
    snprintf(out, cap, "%s/notify_%s.wav", dir, notify_event_short(evt));
#else
    snprintf(out, cap, "%s/notify_%s.m4a", dir, notify_event_short(evt));
#endif
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
#elif defined(__linux__)
    /* Installed layouts, relative to the binary: a tarball/AppImage keeps
     * assets/ next to the executable, an FHS install puts the binary in
     * <prefix>/bin and the data in <prefix>/share/liu/assets. */
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
#if defined(__linux__)
    if (!candidate[0]) {
        static const char *const prefixes[] = {
            "/usr/share/liu/assets", "/usr/local/share/liu/assets",
            "/opt/liu/assets",
        };
        for (usize i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
            int n = snprintf(candidate, sizeof candidate, "%s/sounds/%s",
                             prefixes[i], name);
            if (n > 0 && (usize)n < sizeof candidate &&
                stat(candidate, &st) == 0 && S_ISREG(st.st_mode))
                break;
            candidate[0] = '\0';
        }
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
#elif defined(__linux__)
    /* /proc/self/exe is the Linux _NSGetExecutablePath: the hook command
     * written into ~/.claude/settings.json must be absolute, because the agent
     * that runs it does not inherit the user's interactive shell PATH. */
    char exe[1024];
    ssize_t elen = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (elen > 0) {
        exe[elen] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            int n = snprintf(out, cap, "%s/liu-notify", exe);
            if (n > 0 && (usize)n < cap && access(out, X_OK) == 0) return true;
        }
    }
#endif
    int n = snprintf(out, cap, "liu-notify");
    return n > 0 && (usize)n < cap;
}

/* Populate the freshly-loaded config with bundled defaults. Called from (a)
 * first-install (no notify.conf existed) and (b) "Reset to bundled defaults"
 * — both want a clean slate so the bundled (any, event) rules (rank 1 in the
 * resolver) aren't shadowed by stale (tool, any) rules (rank 2) configured by
 * hand. Every event gets its own bundled file under assets/sounds/. */
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
    /* Stop has its own file, so every event maps 1-to-1 to a file under
     * assets/sounds/ and one cue can change without touching another. */
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

    char buf[8192];
    bool ok = true;
    for (;;) {
        size_t nread = fread(buf, 1, sizeof(buf), in);
        bool read_error = ferror(in) != 0;
        if (fwrite(buf, 1, nread, out) != nread) {
            ok = false;
            break;
        }
        if (read_error) { ok = false; break; }
        if (nread < sizeof(buf)) break;
    }
    if (fclose(in) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    if (!ok) (void)unlink(dst);
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
    sp->browser_rule_index = -1;
    sp->notify_recording_row = -1;   /* 0 would mean "row 0 is recording" */
    sp->drag_slider = SETTINGS_SLIDER_NONE;   /* 0 is a real slider id */
    /* -1 rather than 0: 0 would draw "0 fixed, 0 could not be" next to the
     * buttons before Fix All has ever been pressed. */
    sp->diag_last_fixed = -1;
    sp->diag_expanded = -1;
    if (config && config->font_path[0]) {
        char name[128], full[1024];
        prettify_font_name(path_basename(config->font_path), name, sizeof(name));
        add_font_from_path(name, settings_font_path_full(config->font_path, full, sizeof full));
    }

    /* First-launch bootstrap for the notify daemon, so the bundled wavs
     * are installed without the Notifications tab having been opened.
     * settings_notify_ensure_loaded seeds and saves a missing config;
     * on an existing one it is a no-op, so a hand-curated notify.conf
     * is left alone. */
    settings_notify_ensure_loaded(sp);
}

/* Notify-tab agent detection cache. agent_detect_available() walks $PATH with
 * a stat/access call per candidate, too costly for every frame, so the result
 * is cached and refreshed only when Settings (re)opens; an agent installed
 * meanwhile appears the next time Settings opens. */
static AgentInfo s_notify_agents[AGENT_MAX];
static i32       s_notify_agents_count = 0;
static bool      s_notify_agents_loaded = false;

static void settings_vault_submit(SettingsPanel *sp) {
    if (!sp->vault || !vault_is_unlocked(sp->vault)) {
        snprintf(sp->vault_status, sizeof sp->vault_status, "Error: unlock the vault first");
    } else if (strcmp(sp->vault_new_pw, sp->vault_new_pw2) != 0) {
        snprintf(sp->vault_status, sizeof sp->vault_status, "Error: new passwords differ");
    } else if (!sp->vault_new_pw[0]) {
        snprintf(sp->vault_status, sizeof sp->vault_status, "Error: enter a new password");
    } else if (!vault_change_master(sp->vault, sp->vault_old_pw, sp->vault_new_pw)) {
        snprintf(sp->vault_status, sizeof sp->vault_status, "Error: incorrect current password");
    } else snprintf(sp->vault_status, sizeof sp->vault_status, "Master password updated");
    crypto_secure_zero(sp->vault_old_pw, sizeof sp->vault_old_pw);
    crypto_secure_zero(sp->vault_new_pw, sizeof sp->vault_new_pw);
    crypto_secure_zero(sp->vault_new_pw2, sizeof sp->vault_new_pw2);
    sp->vault_editing_old_pw = sp->vault_editing_new_pw = sp->vault_editing_new_pw2 = false;
}

void settings_close(SettingsPanel *sp) {
    if (!sp) return;
    settings_commit_api_editors(sp);
    settings_notify_finalize_recording(sp);
    settings_clear_focus(sp);
    sp->open = false;
}

void settings_select_tab(SettingsPanel *sp, SettingsTab tab) {
    if (!sp || tab < 0 || tab >= SETTINGS_TAB_COUNT || tab == sp->active_tab) return;
    settings_commit_api_editors(sp);
    if (sp->active_tab == SETTINGS_TAB_NOTIFY) settings_notify_finalize_recording(sp);
    settings_clear_focus(sp);
    sp->active_tab = tab;
    sp->scroll_y = 0;
    sp->nav_reveal_selection = true;
}

void settings_toggle(SettingsPanel *sp) {
    if (sp->open) { settings_close(sp); return; }
    settings_clear_focus(sp);
    sp->open = true;
    sp->nav_scroll = 0;
    sp->nav_reveal_selection = true;
    sp->vault_stats_valid = false;
    sp->scroll_y = 0;
    sp->hover_item = -1;
    settings_refresh_fonts();
    s_notify_agents_loaded = false;
    sp->notify_recording_row = -1;
    sp->notify_record_status[0] = '\0';
    sp->translate_model_menu_scroll = 0;
    sp->voice_lang_menu_scroll = 0;
    sp->voice_lang_menu_accum = 0;
    sp->voice_mic_scanned_at = 0;
}

/* Fixed UI cell width (set by settings_render) — keeps settings text
 * spacing independent of the terminal font size. */
static f32 s_ui_cw = 0.0f;

/* Per-frame alpha multiplier for the entire settings panel (1.0 = opaque).
 * Drives the close-fade — every rect/rrect/glyph drawn through s_rect/
 * s_rrect_simple/draw_label has its color alpha scaled by this so the whole
 * panel fades in lockstep instead of just the dim overlay. */
static f32 s_panel_alpha = 1.0f;

/* Pointer position (framebuffer px) for this frame, stashed at the top of
 * settings_render so any draw helper — even those without an `sp` param
 * (switch, segmented) — can route its rect through ui_hover_amount(). */
static f32 s_ptr_x = -1.0f, s_ptr_y = -1.0f;
static SettingsPanel *s_render_panel;

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

/* Rounded controls are not submitted one draw call each. Glyph flushes already
 * enforce rect -> rrect -> glyph ordering in both backends, and the rrect
 * instance stream preserves order for overlapping translucent controls, so
 * per-control submits buy no correctness. They are coalesced until text,
 * scissor, image, capacity, or end-frame creates a real ordering boundary.
 *
 * A flat-rect flush is the one exception: callers use it for a later underline
 * or divider that must sit above an earlier rounded surface. Preserve that
 * intent by draining rounded instances first. These macros are scoped to the
 * painter and are undefined immediately after settings_render. */
static inline void settings_flush_flat_ordered(Renderer *r) {
    renderer_flush_rrects(r);
    renderer_flush_rects(r);
}
#define renderer_flush_rrects(r_) ((void)(r_))
#define renderer_flush_rects(r_)  settings_flush_flat_ordered((r_))

/* ---- Universal hover highlight (shared ui_hover_amount pool, anim.h) ----
 * Every clickable control in settings routes its rect through these so the
 * whole panel shares one hover feel: a smooth fade-in wash on mouse-over.
 * Pointer position comes from the s_ptr_* file-statics, so even helpers
 * without an `sp` param (switch, segmented) can use them. */
static inline f32 s_hover_amt(f32 x, f32 y, f32 w, f32 h) {
    return ui_hover_amount(x, y, w, h, s_ptr_x, s_ptr_y);
}
/* Translucent rounded wash painted after its base instance and before text.
 * Instance order preserves that compositing without a separate draw call. */
static void s_hover_wash(Renderer *r, f32 x, f32 y, f32 w, f32 h, f32 radius,
                         Color tint, f32 peak) {
    f32 amt = s_hover_amt(x, y, w, h);
    if (amt <= 0.001f) return;
    Color c = tint; c.a = peak * amt;
    s_rrect_simple(r, x, y, w, h, c, radius);
    renderer_flush_rrects(r);
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

/* Vector BELL icon for the per-event banner toggle. Built entirely from the rrect
 * family (s_rrect_per_corner / s_rrect_simple) so it batches in the SAME rrect
 * pass as the button background and composites ON TOP of it by submission order.
 * s_rect/renderer_draw_rect cannot be used here (the flat pass flushes first and
 * would draw the bell underneath the button bg), and neither can glyphs (they
 * follow the font's cell metrics, not `size`). All sizes derive from `size` so the
 * icon scales with dpi; (cx,cy) is the icon center in framebuffer pixels. `c` is
 * the on/off tint (bn_fg) — only its alpha is touched (via s_tint inside the
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
            if (drawn == max_chars - 1 && p[consumed]) {
                renderer_push_glyph(r, x, y, 0x2026, fg);
                break;
            }
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

static void s_scaled_label(Renderer *r, const char *text, f32 x, f32 y,
                            Color fg, f32 width, f32 factor, f32 dpi) {
    s_ui_cw = 8.0f * factor * dpi;
    renderer_set_ui_scale(r, s_ui_cw, 16.0f * factor * dpi);
    draw_label_limited(r, text, x, y, fg, (i32)(width / s_ui_cw));
    s_ui_cw = 8.0f * dpi;
    renderer_set_ui_scale(r, s_ui_cw, 16.0f * dpi);
}

/* ICONS AND EDIT FIELDS GO THROUGH THE PANEL TINT TOO.
 *
 * Everything drawn after `s_panel_alpha` becomes the CONTENT alpha has to fade
 * on the content's curve. icon_draw() and ui_draw_edit_field() take their
 * colours as parameters, so a call that passes a theme colour straight through
 * paints at full opacity for the whole close: the panel slides away while its
 * icons and API-key fields stay until the animation ends. The raw entry points
 * are poisoned below so a new call site cannot take that route without a
 * compile error. */
static inline void s_icon(Renderer *r, IconKind kind, f32 x, f32 y, f32 size,
                          Color color) {
    icon_draw(r, kind, x, y, size, s_tint(color));
}

static inline f32 s_edit_field(Renderer *r, struct AppState *app,
                               const char *buf, i32 len,
                               f32 x, f32 y, f32 cw, f32 ch, i32 max_cells,
                               Color fg, Color caret_c, Color sel_c,
                               bool mask, bool show_caret) {
    return ui_draw_edit_field(r, app, buf, len, x, y, cw, ch, max_cells,
                              s_tint(fg), s_tint(caret_c), s_tint(sel_c),
                              mask, show_caret);
}

/* Every draw below this line must carry the panel's alpha; one that does not
 * shows only during a close. The wrappers above are the whole vocabulary;
 * these macros make the alternatives fail to compile rather than fail on
 * screen. Flushes are untouched: they move no colour. */
#define renderer_draw_rect(...)          use_s_rect_instead(__VA_ARGS__)
#define renderer_draw_rrect(...)         use_s_rrect_per_corner_instead(__VA_ARGS__)
#define renderer_draw_rrect_simple(...)  use_s_rrect_simple_instead(__VA_ARGS__)
#define renderer_draw_rrect_bordered(...) use_s_rrect_bordered_instead(__VA_ARGS__)
#define renderer_push_glyph(...)         use_draw_label_instead(__VA_ARGS__)
#define icon_draw(...)                   use_s_icon_instead(__VA_ARGS__)
#define ui_draw_edit_field(...)          use_s_edit_field_instead(__VA_ARGS__)
#define renderer_draw_image_cached(...)  use_renderer_draw_image_tinted_instead(__VA_ARGS__)

/* One chip width for a whole section.
 *
 * Widths are derived from the value column alone, not from each row's own
 * labels: the same width for every row, so Blur, Fit and Fade line up as one
 * set of controls, and a narrow panel shrinks all of them together. Five is
 * the widest row in the section (Fade), so sizing on it keeps every row inside
 * the column. */
static f32 settings_chip_w(f32 value_room, f32 cw) {
    f32 w = 8.0f * cw;
    if (value_room > 0.0f && w * 5.0f > value_room) w = value_room / 5.0f;
    return w < 1.0f ? 1.0f : w;
}

/* Draw a segmented chip row: `n` equal cells, outer corners rounded, the
 * selected one filled and underlined. `sel` < 0 means nothing is selected
 * (an action row rather than a choice). Shared so the eight-odd rows that
 * want this control stop reimplementing it — and stop drifting apart. */
static void settings_draw_chip_row(Renderer *r, f32 x, f32 y, f32 chip_w, f32 chip_h,
                                   const char *const *labels, i32 n, i32 sel,
                                   Color active_bg, Color idle_bg, Color border,
                                   Color on_fg, Color off_fg,
                                   f32 cw, f32 ch, f32 dpi) {
    f32 radius = fminf(chip_h * 0.5f, 6.0f * dpi);
    f32 bw = fmaxf(1.0f, dpi);
    f32 cx = x;
    for (i32 i = 0; i < n; i++) {
        Color bg = (i == sel) ? active_bg : idle_bg;
        s_rrect_bordered_per_corner(r, cx, y, chip_w, chip_h, bg, border, bw,
                                    (i == 0)     ? radius : 0.0f,
                                    (i == n - 1) ? radius : 0.0f,
                                    (i == n - 1) ? radius : 0.0f,
                                    (i == 0)     ? radius : 0.0f);
        cx += chip_w;
    }
    renderer_flush_rrects(r);

    /* Selection underline, drawn after the fills so it sits on top. */
    if (sel >= 0 && sel < n) {
        f32 ux = x + chip_w * (f32)sel;
        f32 uh = fmaxf(2.0f, 2.0f * dpi);
        s_rrect_bordered_per_corner(r, ux, y + chip_h - uh, chip_w, uh,
                                    on_fg, on_fg, 0.0f,
                                    0.0f, 0.0f,
                                    (sel == n - 1) ? radius : 0.0f,
                                    (sel == 0)     ? radius : 0.0f);
        renderer_flush_rrects(r);
    }

    cx = x;
    for (i32 i = 0; i < n; i++) {
        Color tc = (i == sel) ? on_fg : off_fg;
        /* Width in GLYPHS, not bytes. draw_label advances one cell per
         * codepoint, so measuring with strlen() over-counts every multi-byte
         * character — "Choose…" is 7 cells but 9 bytes, and centring against
         * 9 would shift it a full cell left, out through the chip's left
         * edge. */
        f32 lw = (f32)utf8_len((const u8 *)labels[i], strlen(labels[i])) * cw;
        /* A label wider than its chip is centred anyway — clipping at the
         * panel edge is better than pushing the row out of the column. */
        draw_label(r, labels[i], cx + (chip_w - lw) * 0.5f,
                   y + (chip_h - ch) * 0.5f, tc);
        cx += chip_w;
    }
}


static void draw_value(Renderer *r, const char *text, f32 x, f32 y, Color fg) {
    draw_label(r, text, x, y, fg);
}

/* ---- Action button (Create Theme / Import Theme / Add Custom Font) -------
 *
 * One painter for every icon+label action in the panel, so they are the same
 * object at different widths. The shape is the Reset button's: bordered
 * rounded rect, vector icon and label centred as a group, hover routed through
 * the shared ui_hover_amount pool.
 *
 * The leading mark is a vector icon, not a text glyph, so it matches the
 * vector artwork everywhere else in the panel. `primary` is the only thing
 * that distinguishes the two variants — a solid accent fill against a quiet
 * outline — so a pair reads as an action and its alternative.
 *
 * Geometry comes from `b` (settings_action_btn), which the hit test also
 * calls. Nothing here computes a rect. */
static void s_action_button(Renderer *r, SettingsPanel *sp,
                            const SettingsActionBtn *b,
                            IconKind icon, const char *label,
                            const ChromePalette *cp, Color text_dim,
                            bool primary, f32 dpi) {
    f32 hv = s_hover_amt(b->x, b->y, b->w, b->h);

    if (primary) {
        /* Solid accent — and drawn WITHOUT a border, not with a transparent
         * one. The rrect shader blends border_color across the outer ~1 px
         * whenever border_width > 0.5, so an alpha-0 border would erode the
         * edge into a transparent ring instead of being a no-op. */
        Color fill = cp->btn_primary_bg;
        fill.a = 0.92f + 0.08f * hv;
        s_rrect_simple(r, b->x, b->y, b->w, b->h, fill, b->radius);
        renderer_flush_rrects(r);
        /* No border to brighten, so the hover reads through a wash instead. */
        s_hover_wash(r, b->x, b->y, b->w, b->h, b->radius, cp->btn_primary_fg, 0.12f);
    } else {
        /* Quiet outline, so the row reads as an action and its alternative
         * rather than two unrelated buttons. */
        Color fill = cp->surface_sunken;
        fill.a = 0.45f + 0.30f * hv;
        Color border = color_lerp(cp->divider_subtle, cp->btn_secondary_fg, 0.35f * hv);
        s_rrect_bordered(r, b->x, b->y, b->w, b->h, fill, border,
                         fmaxf(1.0f, dpi), b->radius);
        renderer_flush_rrects(r);
    }

    Color ink = primary ? cp->btn_primary_fg
                        : color_lerp(text_dim, cp->btn_secondary_fg, hv);

    s_icon(r, icon, b->icon_x, b->icon_y, b->icon_sz, ink);
    draw_label_limited(r, label, b->label_x, b->label_y, ink, b->label_max);
    sp_push_hover(sp, b->x, b->y, b->w, b->h);
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
    else               snprintf(out, cap, "ETA %dh %dm", t / 3600, (t % 3600) / 60);
}

/* A model path is Liu-owned when it is inside Liu's managed models directory.
 * Only models downloaded there are eligible for deletion; manually selected
 * paths are excluded. */
static bool settings_model_is_liu_owned(const char *path) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return false;
    char dir[512];
    int n = snprintf(dir, sizeof dir, "%s/%s", home, LIU_WHISPER_SUBDIR);
    if (n <= 0 || (usize)n >= sizeof dir) return false;
    return model_delete_is_liu_owned(path, dir);
}

/* Width a settings_button would occupy for `label` (UTF-8 code-point aware,
 * so multi-byte characters don't over-size the box). */
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
    if (sp) s_hover_wash(r, x, y, w, h, rad, fg, 0.12f);
    draw_label(r, label, x + (w - tw) * 0.5f, y + (h - ch) * 0.5f, fg);
    if (bx) *bx = x; if (by) *by = y; if (bw) *bw = w; if (bh) *bh = h;
    sp_push_hover(sp, x, y, w, h);
    return w;
}

/* The Delete button, in its two states.
 *
 * Same shape as any other settings_button so it sits in the row cleanly, but
 * it carries the destructive palette and swaps its own label when armed —
 * the identical two-click contract the Reset button in the nav column uses,
 * because both throw away something the user cannot get back with an undo.
 * "Confirm?" rather than a second "Delete": an armed button that reads the
 * same as the unarmed one invites the second click it is supposed to guard.
 *
 * Colour comes from the caller's ChromePalette rather than fixed values, so
 * the warning reads on a light theme as well as a dark one. */
static f32 settings_delete_button(Renderer *r, SettingsPanel *sp,
                                  ChromePalette cp, bool armed,
                                  f32 x, f32 y, f32 h, f32 cw, f32 ch, f32 dpi,
                                  Color idle_bg, Color idle_border, Color idle_fg,
                                  f32 *out_x, f32 *out_y, f32 *out_w, f32 *out_h) {
    Color bg = idle_bg, bd = idle_border, fg = idle_fg;
    if (armed) {
        bg = cp.btn_destructive_bg; bg.a = 0.45f;
        bd = cp.btn_destructive_fg; bd.a = 0.80f;
        fg = cp.btn_destructive_fg;
    } else {
        /* Unarmed it must not shout: it sits beside two ordinary buttons and
         * is the one thing on the row that cannot be undone, so it reads as
         * quiet until it is actually armed. */
        bd = color_lerp(idle_border, cp.btn_destructive_fg, 0.35f);
        fg = color_lerp(idle_fg, cp.btn_destructive_fg, 0.45f);
    }
    return settings_button(r, sp, armed ? "Confirm?" : "Delete",
                           x, y, h, cw, ch, dpi, bg, bd, fg,
                           out_x, out_y, out_w, out_h);
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
/* Codex receives the selected model through -c model=<X>; the spawn path
 * handles that flag. This list supplies picker values, while JSON config
 * also accepts other model IDs. Models are ordered by family, with
 * general entries before specialized and older entries. */
static const char *settings_translate_models_codex[] = {
    "gpt-5.5",                /* First in the picker */
    "gpt-5.4",
    "gpt-5.4-mini",
    "gpt-5.3-codex",          /* Coding model */
    "gpt-5.3-codex-spark",    /* Spark model */
    "gpt-5.2"
};
/* Grok passes the model via `-m <X>` (handled in translate_agent.c). Slugs are
 * the canonical Grok CLI model ids; "" lets Grok use its configured default
 * (grok-composer-2.5-fast). */
static const char *settings_translate_models_grok[] = {
    "grok-composer-2.5-fast",    /* Fast model */
    "grok-build"                 /* Coding model */
};
/* Antigravity accepts --model <X> before -p. The picker uses identifiers
 * reported by the CLI; an empty value keeps the CLI default. The -low,
 * -medium, and -high suffixes select reasoning effort. These presets are shown
 * until the CLI model list has been read. */
static const char *settings_translate_models_antigravity[] = {
    "gemini-3.8-flash-low",      /* Low reasoning effort */
    "gemini-3.8-flash-medium",
    "gemini-3.8-flash-high",
    "claude-sonnet-4-6"
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
    } else if (strcmp(tc->agent_id, "antigravity") == 0) {
        SETTINGS_PICK_FB(settings_translate_models_antigravity);
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

typedef SettingsShellLayout SettingsLayout;

/* Icon for each settings section. Same monoline Lucide language as the rest of
 * the chrome, so the nav reads as part of the app rather than as a text list. */
/* Hairline that closes a section header row. Drawn at one physical pixel in
 * the theme's divider tone so it groups without competing with content. */
static void settings_section_rule(Renderer *r, f32 x, f32 y, f32 w,
                                  Color tint, f32 dpi) {
    Color c = tint; c.a *= 0.55f;
    s_rect(r, x, y, w, fmaxf(1.0f, dpi), c);
    renderer_flush_rects(r);
}

static f32 settings_group_heading(Renderer *r, const char *label, f32 x, f32 y,
                                   f32 w, f32 ch, f32 dpi, Color fg, Color rule) {
    draw_label_limited(r, label, x, y + 10 * dpi, fg, (i32)(w / (8 * dpi)));
    f32 rx = x + (f32)strlen(label) * 8 * dpi + 12 * dpi;
    if (x + w > rx) settings_section_rule(r, rx, y + 10 * dpi + ch * 0.6f, x + w - rx, rule, dpi);
    return y + settings_section_h(ch, dpi);
}

/* ---- About tab: card chrome -------------------------------------------
 *
 * Each About section is the same object at a different height — a hairline
 * surface holding an icon, a title, its body text and its own actions. Grouping
 * gives a heading like "Notes Vault" more weight than the path string under it
 * and keeps the update status inside its section. It is the same grouping the
 * theme/font grids use, so About shares the other tabs' visual language. */
static void about_card_bg(Renderer *r, f32 x, f32 y, f32 w, f32 h,
                          const ChromePalette *cp, f32 dpi) {
    Color fill = cp->surface_sunken; fill.a = 0.42f;
    s_rrect_bordered(r, x, y, w, h, fill, cp->divider_subtle,
                     fmaxf(1.0f, dpi), 10.0f * dpi);
    renderer_flush_rrects(r);
}

/* Icon + title row inside a card. Returns the x that the card's body text
 * aligns to, so the icon-column width is stated once instead of at every
 * caller's indent. */
static f32 about_card_head(Renderer *r, IconKind icon, const char *title,
                           f32 x, f32 y, f32 ch, f32 dpi,
                           Color icon_fg, Color title_fg) {
    s_icon(r, icon, x, y, ch, icon_fg);
    f32 tx = x + ch + 10.0f * dpi;
    draw_label(r, title, tx, y, title_fg);
    return tx;
}

/* $HOME → "~" so a Vault or config path stays readable at panel width. */
static void about_tilde_path(const char *p, char *out, usize cap) {
    const char *home = getenv("HOME");
    usize hl = home ? strlen(home) : 0;
    if (hl && strncmp(p, home, hl) == 0 && (p[hl] == '/' || p[hl] == '\0'))
        snprintf(out, cap, "~%s", p + hl);
    else
        snprintf(out, cap, "%s", p);
}

/* =========================================================================
 * Diagnostics tab — row geometry, shared by the renderer and the hit test.
 *
 * Same discipline g_keys_rows enforces one tab over: a row's height depends on
 * how many lines its body wraps to, so a renderer that measured it one way and
 * a click handler that measured it another would put every Fix button on the
 * wrong row as soon as one message got longer. Both walk these functions.
 * ========================================================================= */

/* The full body text of a row: what was found, then what the user must do, then
 * what the last repair said. Composed in one place so the wrap arithmetic and
 * the drawing operate on the same string. */
static void diag_row_body(const DiagItem *it, char *out, usize cap) {
    out[0] = '\0';
    usize n = 0;
    n += (usize)snprintf(out + n, (n < cap) ? cap - n : 0, "%s", it->detail);
    if (it->remedy[0] && n < cap)
        n += (usize)snprintf(out + n, cap - n, "\n\xe2\x86\x92 %s", it->remedy);
    if (it->fix_attempted && it->fix_msg[0] && n < cap)
        snprintf(out + n, cap - n, "\n%s %s",
                 it->fix_ok ? "Fixed:" : "Could not fix:", it->fix_msg);
}

/* Greedy wrap at `max_cells`, breaking only on ASCII space or an explicit
 * newline. Never splits a codepoint: every byte it may break at is < 0x80, and
 * UTF-8 continuation bytes are not. Returns the line count; `starts`/`lens` are
 * filled for the first `max_lines`. */
static i32 diag_wrap(const char *text, i32 max_cells, i32 max_lines,
                     i32 *starts, i32 *lens) {
    if (max_cells < 8) max_cells = 8;
    i32 lines = 0, i = 0, line_start = 0, cells = 0, last_space = -1;
    for (;; i++) {
        char c = text[i];
        if (c == '\0' || c == '\n') {
            if (lines < max_lines) { starts[lines] = line_start; lens[lines] = i - line_start; }
            lines++;
            if (c == '\0') break;
            line_start = i + 1; cells = 0; last_space = -1;
            continue;
        }
        if (c == ' ') last_space = i;
        /* Count cells, not bytes: a continuation byte occupies no column. */
        if (((unsigned char)c & 0xC0) != 0x80) cells++;
        if (cells > max_cells) {
            i32 brk = (last_space > line_start) ? last_space : i;
            if (lines < max_lines) { starts[lines] = line_start; lens[lines] = brk - line_start; }
            lines++;
            line_start = (last_space > line_start) ? brk + 1 : brk;
            i = line_start - 1;
            cells = 0; last_space = -1;
            if (lines >= max_lines + 1) break;   /* caller only draws max_lines */
        }
    }
    return lines;
}

#define DIAG_ROW_MAX_LINES 6
#define DIAG_BODY_CAP      (DIAG_TEXT_CAP * 3)

typedef struct {
    f32  y, h;                 /* the row's box */
    f32  text_x, text_w;
    i32  text_cells;
    SettingsActionBtn fix;
    bool has_fix;
} DiagRowGeom;

static DiagRowGeom diag_row_geom(const DiagItem *it, f32 x, f32 y, f32 w,
                                 f32 cw, f32 ch, f32 dpi) {
    DiagRowGeom g;
    memset(&g, 0, sizeof g);
    g.y = y;

    f32 pad     = 12.0f * dpi;
    f32 dot_col = ch;                       /* status dot / icon column */
    g.has_fix   = (it->fix != DIAG_FIX_NONE);

    f32 btn_w = 0.0f;
    if (g.has_fix) {
        const char *lbl = diag_fix_label(it->fix);
        btn_w = settings_action_w((i32)strlen(lbl), cw, ch, dpi);
    }

    g.text_x = x + pad + dot_col + 8.0f * dpi;
    g.text_w = x + w - pad - g.text_x - (g.has_fix ? btn_w + 10.0f * dpi : 0.0f);
    if (g.text_w < 8.0f * cw) g.text_w = 8.0f * cw;
    g.text_cells = (i32)(g.text_w / (cw > 0.0f ? cw : 1.0f));

    char body[DIAG_BODY_CAP];
    diag_row_body(it, body, sizeof body);
    i32 st[DIAG_ROW_MAX_LINES], ln[DIAG_ROW_MAX_LINES];
    i32 nlines = diag_wrap(body, g.text_cells, DIAG_ROW_MAX_LINES, st, ln);
    if (nlines > DIAG_ROW_MAX_LINES) nlines = DIAG_ROW_MAX_LINES;

    /* title line + body lines + padding above and below */
    g.h = pad + ch + 4.0f * dpi + (f32)nlines * (ch + 2.0f * dpi) + pad;

    if (g.has_fix) {
        f32 bh = settings_action_h(ch, dpi);
        g.fix = settings_action_btn(x + w - pad - btn_w, y + pad, btn_w,
                                    (i32)strlen(diag_fix_label(it->fix)),
                                    cw, ch, dpi);
        (void)bh;
    }
    return g;
}

/* A row is drawn only when it is not healthy — see the render block for why the
 * OK rows collapse into one line instead. */
static bool diag_row_visible(const DiagItem *it) {
    return it->status == DIAG_WARN || it->status == DIAG_FAIL;
}

/* Display Browser beside Terminal without renumbering existing shortcuts. */
static const SettingsTab settings_nav_order[SETTINGS_TAB_COUNT] = {
    SETTINGS_TAB_APPEARANCE, SETTINGS_TAB_TERMINAL, SETTINGS_TAB_BROWSER,
    SETTINGS_TAB_TRANSLATE, SETTINGS_TAB_VOICE, SETTINGS_TAB_NOTIFY,
    SETTINGS_TAB_KEYS, SETTINGS_TAB_VAULT, SETTINGS_TAB_DIAGNOSTICS, SETTINGS_TAB_ABOUT
};
static i32 settings_nav_index(SettingsTab tab) {
    for (i32 i = 0; i < SETTINGS_TAB_COUNT; i++)
        if (settings_nav_order[i] == tab) return i;
    return 0;
}

static IconKind settings_tab_icon(SettingsTab tab) {
    switch (tab) {
    /* Each icon names its section literally. */
    case SETTINGS_TAB_APPEARANCE: return ICON_PALETTE;
    case SETTINGS_TAB_TERMINAL:   return ICON_FT_TERMINAL;
    case SETTINGS_TAB_BROWSER:    return ICON_NETWORK;
    case SETTINGS_TAB_TRANSLATE:  return ICON_LANGUAGES;
    case SETTINGS_TAB_VOICE:      return ICON_MIC;
    case SETTINGS_TAB_NOTIFY:     return ICON_BELL;
    case SETTINGS_TAB_KEYS:       return ICON_KEYBOARD;
    case SETTINGS_TAB_VAULT:      return ICON_FT_LOCK;
    case SETTINGS_TAB_DIAGNOSTICS: return ICON_SCOPE;
    case SETTINGS_TAB_ABOUT:      return ICON_INFO;
    case SETTINGS_TAB_COUNT:      break;
    }
    return ICON_CHEVRON_RIGHT;
}

static const char *settings_tab_name(SettingsTab tab) {
    static const char *names[] = {
        "Appearance", "Terminal", "Translate", "Voice", "Notifications",
        "Keys", "Vault", "Diagnostics", "About", "Browser"
    };
    i32 idx = (i32)tab;
    return (idx >= 0 && idx < SETTINGS_TAB_COUNT) ? names[idx] : "Settings";
}

static const char *settings_tab_desc(SettingsTab tab) {
    switch (tab) {
    case SETTINGS_TAB_APPEARANCE: return "Fonts, themes, and terminal density.";
    case SETTINGS_TAB_TERMINAL:   return "Cursor, selection, tabs, and sidebar behavior.";
    case SETTINGS_TAB_BROWSER:    return "Choose which sites open in Liu Browser.";
    case SETTINGS_TAB_TRANSLATE:  return "Translate prompts before sending them.";
    case SETTINGS_TAB_VOICE:      return "Dictate prompts with on-device Whisper or a speech API.";
    case SETTINGS_TAB_NOTIFY:     return "Choose when Liu gets your attention.";
    case SETTINGS_TAB_KEYS:       return "Keyboard shortcuts and rebinding.";
    case SETTINGS_TAB_VAULT:      return "Encrypted secrets and lock behavior.";
    case SETTINGS_TAB_DIAGNOSTICS: return "Check and repair agent integrations.";
    case SETTINGS_TAB_ABOUT:      return "Version, updates, and storage locations.";
    default:                      return "";
    }
}

static SettingsLayout settings_layout_for(f32 sw, f32 sh, f32 dpi,
                                          f32 scale, f32 yoff) {
    return settings_shell_layout(sw, sh, dpi, scale, yoff, SETTINGS_TAB_COUNT);
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
    { ACT_CUT,              NULL, NULL },
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
#ifndef PLATFORM_MACOS
    { ACT_NEW_WINDOW,       NULL, NULL },
    { ACT_QUIT,             NULL, NULL },
#endif
    { ACT_SETTINGS,         NULL, NULL },
    { ACT_SSH_CONNECT,      NULL, NULL },
    { ACT_IMPORT_SSH_CONFIG, NULL, NULL },

    { ACT_NONE, "Vault", NULL },
    { ACT_VAULT_BROWSER,       NULL, NULL },
    { ACT_VAULT_UNLOCK,        NULL, NULL },
    { ACT_VAULT_LOCK,          NULL, NULL },
    { ACT_VAULT_CHANGE_MASTER, NULL, NULL },

    /* macOS text-field shortcuts aimed at the shell's input line. Fixed
     * mappings: they become key sequences the line editor already
     * understands. */
    { ACT_NONE, "Prompt line  ·  not rebindable", NULL },
    { ACT_NONE, "Start / end of line",            UI_MOD_PRIMARY "+Left / Right" },
    { ACT_NONE, "Word left / right",              "Alt+Left / Right" },
    { ACT_NONE, "Delete to line start / end",     UI_MOD_PRIMARY "+Backspace / Delete" },
    { ACT_NONE, "Delete word left / right",       "Alt+Backspace / Delete" },
    { ACT_NONE, "Undo line edit",                 UI_MOD_PRIMARY "+Z" },
    { ACT_NONE, "Scroll to top / bottom",         UI_MOD_PRIMARY "+Up / Down" },

    { ACT_NONE, "Window & system  ·  not rebindable", NULL },
    /* Spelled with UI_MOD_PRIMARY so the reference names the key actually
     * pressed: "Cmd" is wrong wherever MOD_SUPER is the Super key. */
    { ACT_NONE, "Snap window half / maximize",  "Ctrl+" UI_MOD_PRIMARY "+Arrows" },
    { ACT_NONE, "Snap window quadrant",         "Ctrl+" UI_MOD_PRIMARY "+Shift+Arrows" },
    { ACT_NONE, "Resize split divider",         UI_MOD_PRIMARY "+Alt+Arrows" },
    { ACT_NONE, "Command history",              "Alt+Up" },
    { ACT_NONE, "Close item under cursor",      UI_MOD_PRIMARY "+R" },
#ifdef PLATFORM_LINUX
    /* X11 has no native fullscreen menu; the F11 binding added for Linux is
     * the one that works here. */
    { ACT_NONE, "Toggle fullscreen",            "F11" },
#else
    { ACT_NONE, "Toggle fullscreen",            "Ctrl+" UI_MOD_PRIMARY "+F" },
#endif
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
static inline f32 settings_content_top_off(f32 ch, f32 dpi) { return 2.0f * ch + 44.0f * dpi; }
static inline f32 settings_clip_top_off(f32 ch, f32 dpi)    { return 2.0f * ch + 32.0f * dpi; }

static void settings_draw_switch(Renderer *r, f32 x, f32 y, bool on,
                                 Color active_bg, Color inactive_bg, Color border,
                                 Color active_fg, Color inactive_fg,
                                 f32 cw, f32 ch, f32 dpi) {
    /* One geometry for the painter and every hit test (settings_layout.h):
     * the word ends the same distance from the track whether it says On or
     * Off. */
    SettingsSwitch g = settings_switch(x, y, on, cw, ch, dpi);
    sp_push_hover(s_render_panel, g.x, g.y, g.w, g.h);
    Color track = on ? active_bg : inactive_bg;
    Color edge = on ? active_bg : border;
    f32 hv = s_hover_amt(g.x, g.y, g.w, g.h);
    edge = color_lerp(edge, active_fg, hv * 0.25f);
    s_rrect_bordered(r, g.track_x, g.track_y, g.track_w, g.track_h, track, edge,
                     fmaxf(1.0f, dpi), g.track_h * 0.5f);
    Color ink = on ? chrome_legible_on(track) : inactive_fg;
    s_rrect_simple(r, g.knob_x, g.knob_y, g.knob_sz, g.knob_sz, ink, g.knob_sz * 0.5f);
    renderer_flush_rrects(r);
    draw_label(r, g.word, g.word_x, g.word_y, on ? active_fg : inactive_fg);
}

/* Floor for window opacity. Below roughly a third the terminal stops being
 * readable against whatever is behind it, and a slider that can be dragged to
 * "invisible" is a way to lose the window, not a setting. */
#define SETTINGS_OPACITY_MIN 0.30f

/* Slider — a continuous value the pointer can land on or scrub.
 *
 * Used for the section's two continuous settings (window opacity, wallpaper
 * fade). A track with a knob shows the range and the current position within
 * it, and, unlike an inline number editor that swallows every keystroke until
 * Enter/Escape, it cannot trap the keyboard.
 *
 * `t` is 0..1. The filled part reads as "how much", which is exactly what both
 * of these settings mean. */
static void settings_draw_slider(Renderer *r, f32 x, f32 y, f32 w, f32 h,
                                 f32 t, bool enabled,
                                 Color fill, Color track, Color knob_border,
                                 f32 dpi) {
    if (w <= 0.0f) return;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    f32 track_h = fmaxf(3.0f, roundf(h * 0.22f));
    f32 track_y = y + (h - track_h) * 0.5f;
    f32 rad     = track_h * 0.5f;
    f32 alpha   = enabled ? 1.0f : 0.40f;

    Color tr = track; tr.a *= alpha;
    s_rrect_simple(r, x, track_y, w, track_h, tr, rad);

    f32 knob_r = roundf(h * 0.30f);
    f32 filled = t * (w - knob_r * 2.0f) + knob_r;
    if (filled > knob_r) {
        Color fl = fill; fl.a *= alpha;
        s_rrect_simple(r, x, track_y, filled, track_h, fl, rad);
    }

    /* Knob: a filled disc with a ring, so it reads on both the filled and the
     * empty side of the track. */
    f32 kx = x + filled - knob_r;
    f32 ky = y + (h - knob_r * 2.0f) * 0.5f;
    Color kb = knob_border; kb.a *= alpha;
    Color kf = fill;        kf.a *= alpha;
    s_rrect_simple(r, kx, ky, knob_r * 2.0f, knob_r * 2.0f, kb, knob_r);
    f32 inset = fmaxf(1.5f, dpi);
    s_rrect_simple(r, kx + inset, ky + inset,
                               knob_r * 2.0f - inset * 2.0f,
                               knob_r * 2.0f - inset * 2.0f, kf, knob_r - inset);
    renderer_flush_rrects(r);
}

/* Pointer x → 0..1 along a track whose knob has the same radius the drawing
 * uses, so the value under the cursor is the value that gets drawn there. */
static f32 settings_slider_t_at(f32 px, f32 track_x, f32 track_w, f32 h) {
    f32 knob_r = roundf(h * 0.30f);
    f32 span = track_w - knob_r * 2.0f;
    if (span <= 0.0f) return 0.0f;
    f32 t = (px - track_x - knob_r) / span;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
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
        sp_push_hover(s_render_panel, sx, y, seg_w, h);
        /* Only the outermost segments get rounded corners; interior
         * segments stay flat so their shared edges abut without gaps. */
        f32 r_tl = (i == 0)          ? radius : 0.0f;
        f32 r_bl = (i == 0)          ? radius : 0.0f;
        f32 r_tr = (i == count - 1)  ? radius : 0.0f;
        f32 r_br = (i == count - 1)  ? radius : 0.0f;
        /* Unselected segments warm toward the active fill on hover. */
        Color seg_fill = on ? active_bg : inactive_bg;
        if (!on) {
            f32 hv = s_hover_amt(sx, y, seg_w, h);
            if (hv > 0.001f) seg_fill = color_lerp(seg_fill, active_bg, 0.35f * hv);
        }
        s_rrect_bordered_per_corner(r, sx, y, seg_w, h,
                                    seg_fill,
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
        i32 cells = (i32)(fmaxf(0, seg_w - 8 * dpi) / cw);
        i32 shown = MIN((i32)strlen(labels[i]), cells);
        draw_label_limited(r, labels[i], sx + (seg_w - (f32)shown * cw) * 0.5f,
                           y + (h - ch) * 0.5f, on ? active_fg : inactive_fg, cells);
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

/* Microphone picker.
 *
 * Use a dropdown because the available device list is open-ended and users
 * need to see the choices. Row 0, `System default`, represents an empty
 * configured device pin. */

/* Re-read the device list, at most once a second unless forced. */
static void settings_voice_mics_refresh(SettingsPanel *sp, bool force) {
    f64 now = platform_time_sec();
    if (!force && sp->voice_mic_scanned_at > 0.0 &&
        now - sp->voice_mic_scanned_at < 1.0) {
        return;
    }
    sp->voice_mic_count = platform_audio_input_devices(sp->voice_mics,
                                                       SETTINGS_MIC_MAX);
    sp->voice_mic_scanned_at = now;
}

/* System default + one row per device. */
static i32 settings_voice_mic_rows(const SettingsPanel *sp) {
    return sp->voice_mic_count + 1;
}

static f32 settings_voice_mic_item_h(f32 ch, f32 dpi) {
    return ch + 8.0f * dpi;
}

static f32 settings_voice_mic_menu_height(const SettingsPanel *sp,
                                          f32 ch, f32 dpi) {
    return (f32)settings_voice_mic_rows(sp) * settings_voice_mic_item_h(ch, dpi)
           + 12.0f * dpi;
}

/* Which row is selected: 0 for the system default, else the device index + 1.
 * A pinned device that is not plugged in selects nothing — the chip already
 * says "Unavailable device", and highlighting a row the user did not choose
 * would contradict it. */
static i32 settings_voice_mic_selected(const SettingsPanel *sp) {
    const VoiceConfig *vc = &sp->config->voice;
    if (!vc->input_device[0]) return 0;
    for (i32 i = 0; i < sp->voice_mic_count; i++)
        if (strcmp(sp->voice_mics[i].uid, vc->input_device) == 0) return i + 1;
    return -1;
}

static void settings_draw_voice_mic_menu(SettingsPanel *sp, Renderer *r,
                                         f32 x, f32 y, f32 menu_w,
                                         f32 cw, f32 ch, f32 dpi,
                                         Color panel_bg, Color value_fg,
                                         Color dim_fg, Color accent) {
    if (!sp || !sp->voice_mic_picker_open) return;

    const f32 item_h = settings_voice_mic_item_h(ch, dpi);
    const f32 pad    = 6.0f * dpi;
    const f32 menu_h = settings_voice_mic_menu_height(sp, ch, dpi);
    const i32 rows   = settings_voice_mic_rows(sp);
    const i32 sel    = settings_voice_mic_selected(sp);

    Color menu_bg = { fmaxf(0, panel_bg.r - 0.035f),
                      fmaxf(0, panel_bg.g - 0.035f),
                      fmaxf(0, panel_bg.b - 0.035f), 1.0f };
    Color item_bg = { accent.r * 0.28f, accent.g * 0.28f, accent.b * 0.42f, 1.0f };
    s_rrect_simple(r, x, y, menu_w, menu_h, menu_bg, 6.0f * dpi);
    renderer_flush_rrects(r);
    Color item_fg = chrome_legible_on(item_bg);

    for (i32 i = 0; i < rows; i++) {
        const f32 iy = y + pad + (f32)i * item_h;
        const bool on = (i == sel);
        if (on) {
            s_rrect_simple(r, x + pad, iy + 1.0f * dpi,
                           menu_w - pad * 2.0f, item_h - 2.0f * dpi,
                           item_bg, 4.0f * dpi);
            renderer_flush_rrects(r);
        }
        /* The System default row displays the currently resolved OS input
         * device. The configured device remains empty so the choice follows
         * the system default. */
        char label[160];
        if (i == 0) {
            const char *dflt = NULL;
            for (i32 d = 0; d < sp->voice_mic_count; d++)
                if (sp->voice_mics[d].is_default) { dflt = sp->voice_mics[d].name; break; }
            if (dflt) snprintf(label, sizeof label,
                               "System default  \xc2\xb7  %s", dflt);
            else      snprintf(label, sizeof label, "System default");
        } else {
            snprintf(label, sizeof label, "%s", sp->voice_mics[i - 1].name);
        }
        i32 max_chars = (i32)((menu_w - pad * 2.0f - 12.0f * dpi) / cw);
        if (max_chars < 1) max_chars = 1;
        draw_label_limited(r, label, x + pad + 6.0f * dpi, iy + 4.0f * dpi,
                           on ? item_fg : (i == 0 ? value_fg : dim_fg),
                           max_chars);
    }
}

/* Row index under (x, y), or -1. Mirrors the painter. */
static i32 settings_voice_mic_menu_hit(const SettingsPanel *sp,
                                       f32 mx, f32 my, f32 x, f32 y,
                                       f32 menu_w, f32 ch, f32 dpi) {
    const f32 item_h = settings_voice_mic_item_h(ch, dpi);
    const f32 pad    = 6.0f * dpi;
    if (mx < x || mx >= x + menu_w) return -1;
    if (my < y + pad) return -1;
    i32 row = (i32)((my - (y + pad)) / item_h);
    return (row >= 0 && row < settings_voice_mic_rows(sp)) ? row : -1;
}

/* -------------------------------------------------------------------------
 * Voice language picker.
 *
 * Whisper large-v3 transcribes 100 languages. That is far past what a
 * segmented control can hold, and past what a click-to-cycle chip can
 * either. So: a popup, three columns wide, filled ROW-MAJOR and scrolled by
 * rows, which is what makes the alphabet run in reading order as the wheel
 * turns. Column-major — which the Translate language grid uses, because it
 * shows every entry at once and never scrolls — would put A and M and T on
 * the same screen row and make scrolling incomprehensible.
 *
 * Index 0 is "Auto", which is not a language: it is the absence of one, and
 * costs a single extra decoder step to detect. It leads because it is right
 * more often than a wrong pin.
 * ------------------------------------------------------------------------- */

#define VOICE_LANG_MENU_COLS    3
#define VOICE_LANG_MENU_VISIBLE 11      /* rows on screen at once */

/* Auto + every language, as one flat list. */
static i32 settings_voice_lang_total(void) { return voice_lang_count() + 1; }

static i32 settings_voice_lang_rows(void) {
    return (settings_voice_lang_total() + VOICE_LANG_MENU_COLS - 1) /
           VOICE_LANG_MENU_COLS;
}

static i32 settings_voice_lang_visible_rows(void) {
    i32 rows = settings_voice_lang_rows();
    return rows > VOICE_LANG_MENU_VISIBLE ? VOICE_LANG_MENU_VISIBLE : rows;
}

static i32 settings_voice_lang_clamp_scroll(const SettingsPanel *sp) {
    i32 max_scroll = settings_voice_lang_rows() - settings_voice_lang_visible_rows();
    if (max_scroll < 0) max_scroll = 0;
    i32 s = sp->voice_lang_menu_scroll;
    if (s < 0) s = 0;
    if (s > max_scroll) s = max_scroll;
    return s;
}

/* Label for flat index `i`; index 0 is "Auto". */
static const char *settings_voice_lang_label(i32 i) {
    return i == 0 ? "Auto" : voice_lang_name(i - 1);
}
static const char *settings_voice_lang_code(i32 i) {
    return i == 0 ? "" : voice_lang_code(i - 1);
}

static f32 settings_voice_lang_menu_item_h(f32 ch, f32 dpi) {
    return ch + 8.0f * dpi;
}

static f32 settings_voice_lang_menu_height(f32 ch, f32 dpi) {
    f32 header_h = ch + 14.0f * dpi;
    f32 item_h   = settings_voice_lang_menu_item_h(ch, dpi);
    return header_h + (f32)settings_voice_lang_visible_rows() * item_h +
           12.0f * dpi;
}

static f32 settings_voice_lang_menu_width(f32 cw, f32 dpi, f32 max_w) {
    /* "Luxembourgish" and "Haitian Creole" are the long ones; 16 cells plus
     * the selection pill's padding clears both. */
    f32 w = (f32)VOICE_LANG_MENU_COLS * 16.0f * cw + 16.0f * dpi;
    if (w > max_w) w = max_w;
    return w;
}

static void settings_draw_voice_lang_menu(SettingsPanel *sp, Renderer *r,
                                          f32 x, f32 y, f32 menu_w,
                                          f32 cw, f32 ch, f32 dpi,
                                          Color panel_bg, Color value_fg,
                                          Color dim_fg, Color accent) {
    if (!sp || !sp->voice_lang_picker_open) return;

    const VoiceConfig *vc = &sp->config->voice;
    const i32 total = settings_voice_lang_total();
    const i32 vis   = settings_voice_lang_visible_rows();
    const i32 scroll = settings_voice_lang_clamp_scroll(sp);
    sp->voice_lang_menu_scroll = scroll;

    const f32 header_h = ch + 14.0f * dpi;
    const f32 item_h   = settings_voice_lang_menu_item_h(ch, dpi);
    const f32 pad      = 8.0f * dpi;
    const f32 menu_h   = settings_voice_lang_menu_height(ch, dpi);
    const f32 col_w    = (menu_w - pad * 2.0f) / (f32)VOICE_LANG_MENU_COLS;

    Color menu_bg = { fmaxf(0, panel_bg.r - 0.035f),
                      fmaxf(0, panel_bg.g - 0.035f),
                      fmaxf(0, panel_bg.b - 0.035f), 1.0f };
    Color item_bg = { accent.r * 0.28f, accent.g * 0.28f, accent.b * 0.42f, 1.0f };
    s_rrect_simple(r, x, y, menu_w, menu_h, menu_bg, 6.0f * dpi);
    renderer_flush_rrects(r);
    Color item_fg = chrome_legible_on(item_bg);

    /* Header carries the count — with a scrolling window the user cannot see
     * how much list there is, and "100 languages" is the answer to the
     * question the scrollbar would otherwise raise. */
    char title[64];
    snprintf(title, sizeof title, "Choose language  ·  %d", voice_lang_count());
    draw_label(r, title, x + pad + 4.0f * dpi, y + 7.0f * dpi, dim_fg);

    const i32 sel = vc->lang[0] ? voice_lang_index_for_code(vc->lang) + 1 : 0;

    for (i32 row = 0; row < vis; row++) {
        for (i32 col = 0; col < VOICE_LANG_MENU_COLS; col++) {
            const i32 idx = (scroll + row) * VOICE_LANG_MENU_COLS + col;
            if (idx >= total) break;
            const f32 ix = x + pad + (f32)col * col_w;
            const f32 iy = y + header_h + (f32)row * item_h;
            const bool on = idx == sel;
            if (on) {
                s_rrect_simple(r, ix, iy + 1.0f * dpi,
                               col_w - 4.0f * dpi, item_h - 2.0f * dpi,
                               item_bg, 4.0f * dpi);
                renderer_flush_rrects(r);
            }
            /* draw_label does not clip and the columns are tight, so the
             * label is capped to what fits rather than allowed to run into
             * the next column. */
            i32 max_chars = (i32)((col_w - 12.0f * dpi) / cw);
            if (max_chars < 1) max_chars = 1;
            draw_label_limited(r, settings_voice_lang_label(idx),
                               ix + 6.0f * dpi, iy + 4.0f * dpi,
                               on ? item_fg : (idx == 0 ? value_fg : dim_fg),
                               max_chars);
        }
    }

    /* Overflow track: the list is four screens tall and nothing else says so. */
    const i32 rows = settings_voice_lang_rows();
    if (rows > vis) {
        const f32 track_x = x + menu_w - 5.0f * dpi;
        const f32 track_y = y + header_h;
        const f32 track_h = (f32)vis * item_h;
        Color track = dim_fg; track.a = 0.18f;
        Color thumb = accent; thumb.a = 0.55f;
        f32 thumb_h = track_h * (f32)vis / (f32)rows;
        if (thumb_h < 12.0f * dpi) thumb_h = 12.0f * dpi;
        f32 t = rows > vis ? (f32)scroll / (f32)(rows - vis) : 0.0f;
        s_rrect_simple(r, track_x, track_y, 3.0f * dpi, track_h, track,
                       1.5f * dpi);
        s_rrect_simple(r, track_x, track_y + (track_h - thumb_h) * t,
                       3.0f * dpi, thumb_h, thumb, 1.5f * dpi);
        renderer_flush_rrects(r);
    }
}

/* Flat index under (x, y) inside the menu, or -1. Mirrors the painter. */
static i32 settings_voice_lang_menu_hit(const SettingsPanel *sp,
                                        f32 mx, f32 my, f32 x, f32 y,
                                        f32 menu_w, f32 ch, f32 dpi) {
    const f32 header_h = ch + 14.0f * dpi;
    const f32 item_h   = settings_voice_lang_menu_item_h(ch, dpi);
    const f32 pad      = 8.0f * dpi;
    const f32 col_w    = (menu_w - pad * 2.0f) / (f32)VOICE_LANG_MENU_COLS;
    const i32 vis      = settings_voice_lang_visible_rows();

    if (mx < x + pad || mx >= x + menu_w - pad) return -1;
    if (my < y + header_h) return -1;
    i32 row = (i32)((my - (y + header_h)) / item_h);
    if (row < 0 || row >= vis) return -1;
    i32 col = (i32)((mx - (x + pad)) / col_w);
    if (col < 0 || col >= VOICE_LANG_MENU_COLS) return -1;
    i32 idx = (settings_voice_lang_clamp_scroll(sp) + row) *
              VOICE_LANG_MENU_COLS + col;
    return idx < settings_voice_lang_total() ? idx : -1;
}

/* -------------------------------------------------------------------------
 * The Translate Model menu.
 *
 * A popover on the Model chip: it hangs from the chip, over the rows below it
 * (ui/settings_layout.h places it — settings_model_menu(), the one geometry
 * the painter, the click and the driver share), and it is one of the app's
 * menus: the panel, the rail and the highlight are a context menu's.
 *
 * What it says, top to bottom:
 *   - whose models these are — or, when the list is longer than the window,
 *     a search field in that place — and WHERE THE LIST CAME FROM: Live (the
 *     CLI's or the provider's own answer), Updating, Cached (the fetch failed
 *     and this is the last list that arrived, with its age) or Offline (the
 *     presets compiled in);
 *   - Default, pinned: "let the agent choose" is always one click away;
 *   - the models, the configured one checked.
 *
 * It opens over the controls without moving them, takes the keys as well as
 * the pointer, and opens on the configured model.
 * ------------------------------------------------------------------------- */

#define SETTINGS_MODEL_LBL_DEFAULT  "Default"

typedef enum {
    MODEL_SRC_NONE = 0,   /* never asked yet, or an agent with no catalog       */
    MODEL_SRC_LIVE,       /* the source's own answer, inside its TTL            */
    MODEL_SRC_UPDATING,   /* a fetch is in flight                               */
    MODEL_SRC_CACHED,     /* the fetch failed: the last list that arrived       */
    MODEL_SRC_OFFLINE,    /* ...and none ever did: the presets compiled in      */
} ModelSrc;

static const ModelList *settings_translate_model_list(const TranslateConfig *tc) {
    return tc->backend == TRANSLATE_BACKEND_API ? model_catalog_api(tc->api_provider)
                                                : model_catalog_agent(tc->agent_id);
}

static ModelSrc settings_translate_model_src(const TranslateConfig *tc) {
    const ModelList *live = settings_translate_model_list(tc);
    if (!live) return MODEL_SRC_NONE;
    if (live->state == MODEL_LIST_FETCHING) return MODEL_SRC_UPDATING;
    if (live->state == MODEL_LIST_READY)    return MODEL_SRC_LIVE;
    if (live->state == MODEL_LIST_FAILED)   return live->count > 0 ? MODEL_SRC_CACHED : MODEL_SRC_OFFLINE;
    return MODEL_SRC_NONE;
}

/* Whose list it is, as the head names it. */
static const char *settings_translate_model_owner(const TranslateConfig *tc) {
    if (tc->backend == TRANSLATE_BACKEND_API) return translate_api_provider_display(tc->api_provider);
    if (strcmp(tc->agent_id, "opencode") == 0)    return "OpenCode Go";
    if (strcmp(tc->agent_id, "codex") == 0)       return "Codex";
    if (strcmp(tc->agent_id, "grok") == 0)        return "Grok";
    if (strcmp(tc->agent_id, "antigravity") == 0) return "Antigravity";
    return "Claude";
}

/* The status words. A cached list says how old it is, so a stale list reads
 * as stale. */
static void settings_translate_model_status(const TranslateConfig *tc, char *out, usize cap) {
    out[0] = '\0';
    switch (settings_translate_model_src(tc)) {
    case MODEL_SRC_LIVE:     snprintf(out, cap, "Live"); break;
    case MODEL_SRC_UPDATING: snprintf(out, cap, "Updating\xe2\x80\xa6"); break;
    case MODEL_SRC_OFFLINE:  snprintf(out, cap, "Offline"); break;
    case MODEL_SRC_CACHED: {
        const ModelList *live = settings_translate_model_list(tc);
        long long age = live && live->fetched_at > 0 ? (long long)time(NULL) - live->fetched_at : -1;
        if (age < 0)                snprintf(out, cap, "Cached");
        else if (age < 3600)        snprintf(out, cap, "Cached \xc2\xb7 %lldm old", age / 60 > 0 ? age / 60 : 1);
        else if (age < 48 * 3600)   snprintf(out, cap, "Cached \xc2\xb7 %lldh old", age / 3600);
        else                        snprintf(out, cap, "Cached \xc2\xb7 %lldd old", age / 86400);
        break;
    }
    default: break;
    }
}

/* The rows the menu shows: the options, the configured model among them, and
 * the search applied. `id` points into the catalog, the static tables or the
 * config — good for the frame or the event that asked. */
typedef struct {
    const char *id[SETTINGS_MODEL_OPTS_CAP + 1];
    i32  n;            /* rows listed                                          */
    i32  total;        /* ...before the search                                 */
    i32  current;      /* the configured model's row; -1 when searched away    */
    bool pinned;       /* id[0] is "" — the Default row                        */
    bool searchable;   /* more models than the window: a search field is shown */
} SettingsModelRows;

/* Every word of the search is somewhere in the label, case aside:
 * "flash low" finds gemini-3.8-flash-low. */
static bool settings_model_match(const char *label, const char *needle) {
    while (*needle) {
        while (*needle == ' ') needle++;
        usize len = strcspn(needle, " ");
        if (len == 0) break;
        bool found = false;
        for (const char *p = label; *p && !found; p++)
            found = strncasecmp(p, needle, len) == 0;
        if (!found) return false;
        needle += len;
    }
    return true;
}

static void settings_translate_model_rows(const SettingsPanel *sp, SettingsModelRows *rows) {
    memset(rows, 0, sizeof *rows);
    rows->current = -1;
    TranslateConfig *tc = &sp->config->translate;
    const char *opts[SETTINGS_MODEL_OPTS_CAP];
    i32 n = settings_translate_model_options(tc, opts, SETTINGS_MODEL_OPTS_CAP);
    const char *cur = settings_translate_model_slot(tc, NULL);

    bool has_default = n > 0 && opts[0][0] == '\0';
    bool known = false;
    for (i32 i = 0; i < n && !known; i++) known = strcmp(opts[i], cur) == 0;

    /* A model set by hand in config.json, or one a refreshed list no longer
     * carries, is still the configured one: it is listed, first, so the menu
     * never opens with its check on nothing. */
    const char *all[SETTINGS_MODEL_OPTS_CAP + 1];
    i32 total = 0, from = 0;
    if (has_default) all[total++] = opts[from++];
    if (!known && cur[0]) all[total++] = cur;
    for (; from < n; from++) all[total++] = opts[from];

    rows->total = total;
    rows->searchable = total - (has_default ? 1 : 0) > SETTINGS_MODEL_MENU_ROWS;
    const char *needle = rows->searchable ? sp->model_menu_filter : "";
    for (i32 i = 0; i < total; i++) {
        if (!settings_model_match(all[i][0] ? all[i] : SETTINGS_MODEL_LBL_DEFAULT, needle)) continue;
        if (strcmp(all[i], cur) == 0) rows->current = rows->n;
        rows->id[rows->n++] = all[i];
    }
    rows->pinned = rows->n > 0 && rows->id[0][0] == '\0';
}

static i32 settings_model_menu_window(const SettingsPanel *sp) {
    return sp->model_menu_visible > 0 ? sp->model_menu_visible : SETTINGS_MODEL_MENU_ROWS;
}

static void settings_model_menu_close(SettingsPanel *sp) {
    sp->translate_model_picker_open = false;
    sp->translate_model_menu_scroll = 0;
    sp->model_menu_filter[0] = '\0';
    sp->model_menu_cursor = -1;
}

/* The cursor goes to row `to` and the window follows it. A key did this: the
 * pointer, wherever it rests, does not take the cursor back until it moves. */
static void settings_model_menu_move(SettingsPanel *sp, const SettingsModelRows *rows, i32 to) {
    if (rows->n < 1) { sp->model_menu_cursor = -1; return; }
    if (to < 0) to = 0;
    if (to >= rows->n) to = rows->n - 1;
    sp->model_menu_cursor = to;
    i32 pin = rows->pinned ? 1 : 0;
    if (to >= pin) {
        sp->translate_model_menu_scroll = settings_model_menu_reveal(
            sp->translate_model_menu_scroll, to - pin, rows->n - pin, settings_model_menu_window(sp));
    }
    sp->model_menu_key_px = sp->ptr_x;
    sp->model_menu_key_py = sp->ptr_y;
}

static void settings_model_menu_open(SettingsPanel *sp) {
    sp->translate_model_picker_open = true;
    sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
    sp->model_menu_filter[0] = '\0';
    sp->model_menu_accum = 0.0f;
    sp->model_menu_visible = 0;
    sp->translate_model_menu_scroll = 0;
    SettingsModelRows rows;
    settings_translate_model_rows(sp, &rows);
    /* Open on the configured model, with that row and what follows it in
     * view. */
    i32 pin = rows.pinned ? 1 : 0;
    i32 at = rows.current >= 0 ? rows.current : (rows.n > 0 ? 0 : -1);
    sp->model_menu_cursor = at;
    if (at - pin >= SETTINGS_MODEL_MENU_ROWS) {
        sp->translate_model_menu_scroll = settings_model_menu_clamp_scroll(
            at - pin - SETTINGS_MODEL_MENU_ROWS / 2, rows.n - pin, SETTINGS_MODEL_MENU_ROWS);
    }
    sp->model_menu_key_px = sp->ptr_x;
    sp->model_menu_key_py = sp->ptr_y;
}

/* Row `i` becomes the model and the menu closes. */
static void settings_model_menu_pick(SettingsPanel *sp, const SettingsModelRows *rows, i32 i) {
    if (i < 0 || i >= rows->n) return;
    usize cap = 0;
    char *slot = settings_translate_model_slot(&sp->config->translate, &cap);
    /* The hand-set row points INTO the slot; go through a copy either way. */
    char pick[MODEL_ID_CAP];
    snprintf(pick, sizeof pick, "%s", rows->id[i]);
    if (strcmp(slot, pick) != 0) {
        snprintf(slot, cap, "%s", pick);
        sp->config_dirty = true;
    }
    settings_model_menu_close(sp);
}

/* The search changed: the list is shorter or longer, so the cursor goes to
 * its first row — or back to the configured model when the field is empty. */
static void settings_model_menu_refilter(SettingsPanel *sp) {
    SettingsModelRows rows;
    settings_translate_model_rows(sp, &rows);
    sp->translate_model_menu_scroll = 0;
    settings_model_menu_move(sp, &rows, !sp->model_menu_filter[0] && rows.current >= 0 ? rows.current : 0);
}

/* The menu's place for the rows it has now, from the chip the last frame
 * drew. False when no frame has drawn the chip. */
static bool settings_translate_model_menu_place(const SettingsPanel *sp, const SettingsModelRows *rows,
                                                f32 sw, f32 sh, f32 dpi, SettingsModelMenu *m) {
    if (sp->tr_model_chip_w <= 0.0f) return false;
    SettingsLayout sl = settings_layout_for(sw, sh, dpi, 1.0f, 0.0f);
    SettingsRect chip = {sp->tr_model_chip_x, sp->tr_model_chip_y, sp->tr_model_chip_w, sp->tr_model_chip_h};
    *m = settings_model_menu(chip, sl.content_top + settings_clip_top_off(16.0f * dpi, dpi), sl.clip_bottom,
                             sl.content_x, sl.content_w, rows->pinned,
                             rows->n - (rows->pinned ? 1 : 0), dpi);
    return true;
}

void settings_model_menu_info(const SettingsPanel *sp, f32 sw, f32 sh, f32 dpi,
                              SettingsModelMenuInfo *out) {
    static char status[32];
    memset(out, 0, sizeof *out);
    out->cursor = out->current = -1;
    out->model = out->filter = out->status = "";
    if (!sp || !sp->config) return;
    const TranslateConfig *tc = &sp->config->translate;
    out->model = tc->backend == TRANSLATE_BACKEND_API ? tc->api_model : tc->agent_model;
    settings_translate_model_status(tc, status, sizeof status);
    out->status = status;
    out->chip_x = sp->tr_model_chip_x; out->chip_y = sp->tr_model_chip_y;
    out->chip_w = sp->tr_model_chip_w; out->chip_h = sp->tr_model_chip_h;
    SettingsModelRows rows;
    settings_translate_model_rows(sp, &rows);
    out->rows = rows.n; out->total = rows.total; out->current = rows.current;
    out->searchable = rows.searchable; out->pinned = rows.pinned;
    if (!sp->open || sp->active_tab != SETTINGS_TAB_TRANSLATE || !sp->translate_model_picker_open) return;
    out->open = true;
    out->cursor = sp->model_menu_cursor;
    out->scroll = sp->translate_model_menu_scroll;
    out->filter = sp->model_menu_filter;
    SettingsModelMenu m;
    if (!settings_translate_model_menu_place(sp, &rows, sw, sh, dpi, &m)) return;
    out->visible = m.visible; out->above = m.above;
    out->card_x = m.card.x; out->card_y = m.card.y; out->card_w = m.card.w; out->card_h = m.card.h;
    out->row_x = m.list.x; out->row_y = m.list.y; out->row_w = m.list.w; out->row_h = m.row_h;
    out->pinned_y = m.pinned.w > 0.0f ? m.pinned.y : 0.0f;
}

/* The colour of a list that is the source's own answer, and of one that is a
 * copy which may have gone stale. The theme's own green and yellow when they
 * are colours at all — a monochrome palette's are greys, and a grey dot says
 * nothing — and canonical ones otherwise. */
static Color settings_tone(const Theme *t, bool warn) {
    const Color fallback = warn ? (Color){0.96f, 0.72f, 0.26f, 1.0f} : (Color){0.33f, 0.78f, 0.49f, 1.0f};
    if (!t) return fallback;
    const i32 slots[2] = { warn ? 11 : 10, warn ? 3 : 2 };
    for (i32 k = 0; k < 2; k++) {
        Color c = t->ansi[slots[k]];
        f32 max_c = fmaxf(c.r, fmaxf(c.g, c.b)), min_c = fminf(c.r, fminf(c.g, c.b));
        bool hue = warn ? (c.r >= c.b && c.g >= c.b) : (c.g >= c.r && c.g >= c.b);
        if (hue && max_c - min_c >= 0.22f && max_c >= 0.45f) { c.a = 1.0f; return c; }
    }
    return fallback;
}

/* One row: the highlight under it (how much of one: `lit`), the check when it
 * is the configured model, the label, and a note against the right edge. */
static void settings_model_menu_draw_row(Renderer *r, const SettingsModelMenu *m, SettingsRect row,
                                         const char *id, const char *note, bool current, f32 lit,
                                         Color hl, Color ink, Color ink_dim, Color accent,
                                         f32 cw, f32 ch, f32 dpi) {
    SettingsRect bar = settings_model_menu_highlight(m, row);
    if (lit > 0.001f) {
        Color c = hl; c.a *= lit;
        s_rrect_simple(r, bar.x, bar.y, bar.w, bar.h, c, CTX_MENU_HL_RADIUS_PT * dpi);
    }
    f32 isz = CTX_MENU_ICON_PT * dpi;
    if (current) {
        s_icon(r, ICON_CHECK, m->card.x + CTX_MENU_ICON_X_PT * dpi, row.y + (row.h - isz) * 0.5f, isz, accent);
    }
    f32 x = m->card.x + (CTX_MENU_ICON_X_PT + CTX_MENU_ICON_PT + CTX_MENU_LABEL_GAP_PT) * dpi;
    f32 y = row.y + (row.h - ch) * 0.5f;
    f32 right = bar.x + bar.w - 10.0f * dpi;
    i32 room = (i32)((right - x) / cw);
    if (room < 1) return;

    const char *label = id[0] ? id : SETTINGS_MODEL_LBL_DEFAULT;
    i32 label_cells = (i32)strlen(label);
    i32 note_cells = note ? (i32)strlen(note) : 0;
    /* The note is dropped before the label is cut: what the row IS matters
     * more than what is said about it. */
    if (note_cells > 0 && label_cells + 3 + note_cells <= room) {
        draw_label_limited(r, note, right - (f32)note_cells * cw, y, ink_dim, note_cells);
    }
    /* "opencode-go/glm-5.1": the provider is the same on every row of a list
     * and the model is what is being chosen, so the part up to the last slash
     * steps back. */
    const char *slash = id[0] ? strrchr(id, '/') : NULL;
    i32 lead = slash ? (i32)(slash - id) + 1 : 0;
    if (lead > 0 && lead < room) {
        char prefix[MODEL_ID_CAP];
        snprintf(prefix, sizeof prefix, "%.*s", lead, id);
        draw_label_limited(r, prefix, x, y, ink_dim, lead);
        draw_label_limited(r, id + lead, x + (f32)lead * cw, y, ink, room - lead);
    } else {
        draw_label_limited(r, label, x, y, ink, room);
    }
}

static void settings_draw_translate_model_menu(SettingsPanel *sp, struct AppState *app, Renderer *r,
                                               const Theme *theme, const SettingsModelMenu *m,
                                               const SettingsModelRows *rows, Color accent,
                                               f32 cw, f32 ch, f32 dpi) {
    TranslateConfig *tc = &sp->config->translate;
    i32 pin = rows->pinned ? 1 : 0;
    i32 count = rows->n - pin;
    i32 scroll = settings_model_menu_clamp_scroll(sp->translate_model_menu_scroll, count, m->visible);
    /* The first frame since it opened is the first that knows the window's
     * real size, so the configured model is scrolled into view again here:
     * at open it was placed for a window of eight rows, which in a short
     * window leaves it out of view. */
    if (sp->model_menu_visible == 0 && sp->model_menu_cursor >= pin)
        scroll = settings_model_menu_reveal(scroll, sp->model_menu_cursor - pin, count, m->visible);
    sp->translate_model_menu_scroll = scroll;
    sp->model_menu_visible = m->visible;
    if (sp->model_menu_cursor >= rows->n) sp->model_menu_cursor = rows->n - 1;

    /* The colours of a context menu (ctx_menu_draw_panel / _rows, ui.c) — but
     * OPAQUE. A context menu's 1.5% of see-through is over a terminal; here it
     * is over a column of controls, and in linear light that much of a white
     * label stays legible as a ghost behind the rows. */
    Color card = { fminf(1.0f, theme->bg.r + 0.06f), fminf(1.0f, theme->bg.g + 0.06f),
                   fminf(1.0f, theme->bg.b + 0.06f), 1.0f };
    Color edge = theme->border; edge.a = fmaxf(edge.a, 0.55f);
    Color rule = theme->border; rule.a = fmaxf(rule.a, 0.40f) * 0.8f;
    Color hl = { fminf(1.0f, theme->tab_active_bg.r + 0.05f), fminf(1.0f, theme->tab_active_bg.g + 0.05f),
                 fminf(1.0f, theme->tab_active_bg.b + 0.05f), 0.80f };
    Color ink = theme->fg;       ink.a = 0.95f;
    Color ink_dim = theme->fg;   ink_dim.a = 0.45f;
    Color ink_faint = theme->fg; ink_faint.a = 0.38f;

    /* What the tab drew is UNDER the menu: commit it, then the panel, so the
     * flat rects and the text that follow land on the panel. */
    renderer_flush_glyphs(r);
    f32 radius = CTX_MENU_RADIUS_PT * dpi;
    s_rrect_bordered_shadow(r, m->card.x, m->card.y, m->card.w, m->card.h, card, edge, fmaxf(1.0f, dpi),
                            radius, radius, radius, radius, 14.0f * dpi, 0.32f, 0.0f, 5.0f * dpi);
    renderer_flush_rects(r);

    /* Under the card only its own rows are clickable. */
    sp->overlay_x = m->card.x; sp->overlay_y = m->card.y;
    sp->overlay_w = m->card.w; sp->overlay_h = m->card.h;
    sp->overlay_first_area = sp->hover_area_count;

    /* ---- head: the list's name or its search field, and its status ------- */
    f32 rail_x  = m->card.x + CTX_MENU_ICON_X_PT * dpi;
    f32 label_x = rail_x + (CTX_MENU_ICON_PT + CTX_MENU_LABEL_GAP_PT) * dpi;
    f32 right   = m->card.x + m->card.w - CTX_MENU_PAD_R_PT * dpi;
    f32 head_y  = m->head.y + (m->head.h - ch) * 0.5f;
    const f32 small = 0.875f;
    f32 small_cw = cw * small;

    char status[32];
    settings_translate_model_status(tc, status, sizeof status);
    f32 status_x = right;
    if (status[0]) {
        ModelSrc src = settings_translate_model_src(tc);
        Color tone = src == MODEL_SRC_LIVE ? settings_tone(theme, false)
                   : src == MODEL_SRC_CACHED ? settings_tone(theme, true) : ink_dim;
        f32 dot = 6.0f * dpi;
        f32 words_w = (f32)utf8_len((const u8 *)status, strlen(status)) * small_cw;
        f32 words_x = right - words_w;
        status_x = words_x - 6.0f * dpi - dot;
        s_rrect_simple(r, status_x, m->head.y + (m->head.h - dot) * 0.5f, dot, dot, tone, dot * 0.5f);
        /* The smaller line sits on the same centre as the row's text. */
        s_scaled_label(r, status, words_x, m->head.y + (m->head.h - ch * small) * 0.5f,
                       src == MODEL_SRC_CACHED ? ink : ink_dim, words_w + small_cw, small, dpi);
    }
    if (rows->searchable) {
        f32 isz = CTX_MENU_ICON_PT * dpi;
        s_icon(r, ICON_SEARCH, rail_x, m->head.y + (m->head.h - isz) * 0.5f, isz, ink_dim);
        i32 cells = (i32)((status_x - 14.0f * dpi - label_x) / cw);
        if (cells < 4) cells = 4;
        if (!sp->model_menu_filter[0]) {
            /* Clear of the caret, which stands where the first letter goes. */
            char hint[48];
            snprintf(hint, sizeof hint, "Search %d models", rows->total - pin);   /* empty field: nothing is filtered away */
            draw_label_limited(r, hint, label_x + 5.0f * dpi, head_y, ink_faint, cells - 1);
        }
        Color caret = { theme->fg.r, theme->fg.g, theme->fg.b, 0.8f * ui_caret_alpha() };
        Color band  = { accent.r, accent.g, accent.b, 0.30f };
        if (app) {
            s_edit_field(r, app, sp->model_menu_filter, (i32)strlen(sp->model_menu_filter),
                         label_x, head_y, cw, ch, cells, ink, caret, band, false, true);
        }
    } else {
        char title[64];
        snprintf(title, sizeof title, "%s models", settings_translate_model_owner(tc));
        for (char *c = title; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        s_scaled_label(r, title, rail_x, m->head.y + (m->head.h - ch * small) * 0.5f, ink_dim,
                       status_x - 14.0f * dpi - rail_x, small, dpi);
    }
    f32 hair = fmaxf(1.0f, dpi);
    s_rect(r, m->card.x + hair, m->rule_y, m->card.w - hair * 2.0f, hair, rule);
    if (m->sep_y >= 0.0f) {
        s_rect(r, m->card.x + CTX_MENU_HL_INSET_PT * dpi, m->sep_y,
               m->card.w - CTX_MENU_HL_INSET_PT * dpi * 2.0f, hair, rule);
    }

    /* ---- rows -------------------------------------------------------------
     * The pointer moves the cursor: the row it is on is the active one, the
     * one Enter takes. After an arrow key the cursor is the keyboard's until
     * the pointer moves again — a mouse left resting on a row would otherwise
     * take every arrow press back. Each row's bar goes through the hover
     * registry first, so the loop knows a move onto another row is a frame. */
    bool pointer_led = sp->ptr_x != sp->model_menu_key_px || sp->ptr_y != sp->model_menu_key_py;
    f32 lit_pinned = 0.0f, lit_row[SETTINGS_MODEL_MENU_ROWS] = {0};
    i32 shown = count < m->visible ? count : m->visible;
    if (pin) {
        SettingsRect bar = settings_model_menu_highlight(m, m->pinned);
        lit_pinned = s_hover_amt(bar.x, bar.y, bar.w, bar.h);
        if (pointer_led && settings_rect_contains(bar, sp->ptr_x, sp->ptr_y)) sp->model_menu_cursor = 0;
        sp_push_hover(sp, bar.x, bar.y, bar.w, bar.h);
    }
    for (i32 vi = 0; vi < shown && vi < SETTINGS_MODEL_MENU_ROWS; vi++) {
        SettingsRect bar = settings_model_menu_highlight(m, settings_model_menu_row(m, vi));
        lit_row[vi] = s_hover_amt(bar.x, bar.y, bar.w, bar.h);
        if (pointer_led && settings_rect_contains(bar, sp->ptr_x, sp->ptr_y))
            sp->model_menu_cursor = pin + scroll + vi;
        sp_push_hover(sp, bar.x, bar.y, bar.w, bar.h);
    }
    i32 cursor = sp->model_menu_cursor;

    if (pin) {
        /* What Default means, said on the row: the provider's default has a
         * name; an agent's is whatever its CLI is set to. */
        char note[64];
        const char *api_default = tc->backend == TRANSLATE_BACKEND_API
            ? translate_api_default_model(tc->api_provider) : "";
        if (api_default[0]) snprintf(note, sizeof note, "%s", api_default);
        else snprintf(note, sizeof note, "Chosen by %s", settings_translate_model_owner(tc));
        f32 lit = cursor == 0 ? 1.0f : (pointer_led ? lit_pinned : 0.0f);
        settings_model_menu_draw_row(r, m, m->pinned, "", note, rows->current == 0, lit,
                                     hl, ink, ink_dim, accent, cw, ch, dpi);
    }
    for (i32 vi = 0; vi < shown && vi < SETTINGS_MODEL_MENU_ROWS; vi++) {
        i32 i = pin + scroll + vi;
        f32 lit = cursor == i ? 1.0f : (pointer_led ? lit_row[vi] : 0.0f);
        settings_model_menu_draw_row(r, m, settings_model_menu_row(m, vi), rows->id[i], NULL,
                                     rows->current == i, lit, hl, ink, ink_dim, accent, cw, ch, dpi);
    }
    if (rows->n == 0) {
        draw_label_limited(r, "No models match", label_x, m->list.y + (m->row_h - ch) * 0.5f, ink_faint,
                           (i32)((right - label_x) / cw));
    }

    /* ---- overflow: a thumb the size of the window ---------------------- */
    if (m->track.w > 0.0f && count > m->visible) {
        Color track = theme->fg; track.a = 0.08f;
        Color thumb = theme->fg; thumb.a = 0.34f;
        f32 thumb_h = m->track.h * (f32)m->visible / (f32)count;
        if (thumb_h < 14.0f * dpi) thumb_h = 14.0f * dpi;
        f32 t = (f32)scroll / (f32)(count - m->visible);
        s_rrect_simple(r, m->track.x, m->track.y, m->track.w, m->track.h, track, m->track.w * 0.5f);
        s_rrect_simple(r, m->track.x, m->track.y + (m->track.h - thumb_h) * t, m->track.w, thumb_h,
                       thumb, m->track.w * 0.5f);
    }
    renderer_flush_glyphs(r);
}

/* A key while the menu is open. The arrows, Page keys and Enter are the
 * list's; with a search field everything else is the field's. */
static bool settings_model_menu_key(SettingsPanel *sp, struct AppState *app, u32 key, u32 mods) {
    SettingsModelRows rows;
    settings_translate_model_rows(sp, &rows);
    i32 cur = sp->model_menu_cursor;
    i32 page = settings_model_menu_window(sp);
    bool typing = rows.searchable && sp->model_menu_filter[0];

    if (key == KEY_ESCAPE) {
        /* First the search, then the menu: Escape undoes the last thing. */
        if (typing) { sp->model_menu_filter[0] = '\0'; settings_model_menu_refilter(sp); }
        else settings_model_menu_close(sp);
        return true;
    }
    if (key == KEY_ENTER) {
        if (cur >= 0) settings_model_menu_pick(sp, &rows, cur);
        return true;
    }
    if (key == KEY_TAB) return true;   /* not a Settings tab switch from inside a menu */
    if (mods & (MOD_SUPER | MOD_CTRL | MOD_ALT)) {
        /* A chord is not navigation; the field takes the ones it knows. */
    } else if (key == KEY_UP)        { settings_model_menu_move(sp, &rows, cur < 0 ? rows.n - 1 : cur - 1); return true; }
    else if (key == KEY_DOWN)        { settings_model_menu_move(sp, &rows, cur < 0 ? 0 : cur + 1); return true; }
    else if (key == KEY_PAGE_UP)     { settings_model_menu_move(sp, &rows, (cur < 0 ? 0 : cur) - page); return true; }
    else if (key == KEY_PAGE_DOWN)   { settings_model_menu_move(sp, &rows, (cur < 0 ? 0 : cur) + page); return true; }
    else if (key == KEY_HOME && !typing) { settings_model_menu_move(sp, &rows, 0); return true; }
    else if (key == KEY_END && !typing)  { settings_model_menu_move(sp, &rows, rows.n - 1); return true; }

    if (rows.searchable && app) {
        char before[sizeof sp->model_menu_filter];
        snprintf(before, sizeof before, "%s", sp->model_menu_filter);
        i32 len = (i32)strlen(sp->model_menu_filter);
        TextEdit e = app_edit(app, sp->model_menu_filter, &len, (i32)sizeof sp->model_menu_filter);
        if (textedit_key(&e, &app->te_undo, (i32)key, mods)) {
            app_edit_done(app, (i32)strlen(sp->model_menu_filter));
            if (strcmp(before, sp->model_menu_filter) != 0) settings_model_menu_refilter(sp);
            return true;
        }
    }
    return false;
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

void settings_render(SettingsPanel *sp, struct AppState *app, Renderer *r,
                     f32 sw, f32 sh, f32 dpi) {
    /* Edge-detection state for open + close transitions. The close path
     * keeps rendering after sp->open flips back to false until the close
     * animation drains, so dismissals fade out smoothly.
     *
     * On the PANEL, not function statics: this function is called once per
     * window per frame, so one shared triplet meant window A's call consumed
     * window B's rising edge — A painted an opaque, fully-drawn Settings panel
     * over its own terminal while B's real panel never appeared. */
    f32 panel_scale, panel_alpha, panel_yoff;
    bool render_settings = modal_anim_progress(
        sp->open,
        &sp->open_anim, &sp->close_anim,
        &sp->was_open, dpi, MODAL_OPEN_DUR_LARGE,
        &panel_scale, &panel_alpha, &panel_yoff);
    if (!render_settings) return;
    s_panel_alpha = panel_alpha;
    s_ptr_x = sp->ptr_x;
    s_ptr_y = sp->ptr_y;
    s_render_panel = sp;

    /* Hover registry is per-frame state: clear on entry, then draw sites
     * push their rectangles as they're laid out. settings_point_clickable
     * walks this list to drive the pointing-hand cursor in main.c. */
    sp->hover_area_count = 0;
    /* So are the popover and the chip it hangs from: a frame that does not
     * draw them leaves nothing of theirs to click. */
    sp->overlay_w = 0.0f;
    sp->tr_model_chip_w = 0.0f;

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
     * it would make the sidebar read as a separate bluish surface. */
    const Theme *theme = sp->config->theme ? sp->config->theme : &THEME_Liu;
    ChromePalette cp = chrome_palette_for(theme);
    Color panel_bg = color_lerp(theme->bg, theme->fg, 0.035f);
    panel_bg.a = 1;
    Color text_fg = theme->fg;
    Color text_dim = color_lerp(panel_bg, text_fg, 0.64f);
    Color border_clr = cp.divider_strong;
    border_clr.a = 0.70f;
    Color accent = cp.btn_primary_bg;
    Color nav_active_bg = color_lerp(theme->bg, accent, cp.is_light ? 0.10f : 0.14f);
    nav_active_bg.a = 1;

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

    /* The shell is up; everything below is CONTENT and leaves ahead of it.
     * Without the stagger the panel's labels and the terminal's own text are
     * equally legible through the middle of every open and close — the fill is
     * theme->bg, so at half alpha it hides nothing. See modal_content_alpha. */
    s_panel_alpha = modal_content_alpha(panel_alpha);

    f32 title_h = sl.title_h;
    f32 nav_w = sl.nav_w;
    /* The shell's border lies inside the panel's edge. The nav's fill stays
     * inside it, and the title rule and the nav divider run into it, so the
     * outline is unbroken and the three lines meet with no gap. */
    f32 shell_bw = fmaxf(1.0f, dpi);
    s_rrect_per_corner(r, px + shell_bw, py + title_h, nav_w - shell_bw,
                       panel_h - title_h - shell_bw,
                       theme->bg, 0, 0, 0, panel_radius - shell_bw);
    s_rect(r, px + shell_bw, py + title_h - dpi,
           panel_w - 2 * shell_bw, dpi, cp.divider_subtle);
    s_rect(r, px + nav_w, py + title_h, dpi, panel_h - title_h - shell_bw,
           cp.divider_subtle);
    renderer_flush_rects(r);
    s_icon(r, ICON_GEAR, px + 18 * dpi, py + (title_h - ch) * 0.5f, ch, text_dim);
    draw_label(r, "Settings", px + 44 * dpi, py + (title_h - ch) * 0.5f, text_fg);
    SettingsRect close = sl.close;
    s_hover_wash(r, close.x, close.y, close.w, close.h, 7 * dpi, text_fg, 0.08f);
    s_icon(r, ICON_CLOSE, close.x + 6 * dpi, close.y + 6 * dpi, 16 * dpi, text_dim);
    sp_push_hover(sp, close.x, close.y, close.w, close.h);
    if (panel_w > 500 * dpi)
        s_scaled_label(r, "Esc", close.x - 32 * dpi, close.y + 7 * dpi, text_dim, 28 * dpi, 0.875f, dpi);

    /* The menu can scroll independently in short windows; group captions
     * never count as selectable rows. */
    sp->nav_max_scroll = fmaxf(0, sl.nav_content_h - (sl.nav_bottom - sl.nav_top));
    sp->nav_scroll = CLAMP(sp->nav_scroll, 0, sp->nav_max_scroll);
    if (sp->nav_reveal_selection) {
        SettingsRect selected = settings_nav_row(sl, settings_nav_index(sp->active_tab), sp->nav_scroll, dpi);
        if (selected.y < sl.nav_top) sp->nav_scroll -= sl.nav_top - selected.y;
        if (selected.y + selected.h > sl.nav_bottom) sp->nav_scroll += selected.y + selected.h - sl.nav_bottom;
        sp->nav_scroll = CLAMP(sp->nav_scroll, 0, sp->nav_max_scroll);
        sp->nav_reveal_selection = false;
    }
    sp->nav_clip_x = px; sp->nav_clip_y = sl.nav_top;
    sp->nav_clip_w = nav_w; sp->nav_clip_h = sl.nav_bottom - sl.nav_top;
    renderer_push_scissor(r, px, sl.nav_top, nav_w, sp->nav_clip_h);
    sp->clip_active = true; sp->clip_y0 = sl.nav_top; sp->clip_y1 = sl.nav_bottom;
    if (sl.nav_group_h > 0) {
        const char *groups[] = {"GENERAL", "WORKFLOW", "MANAGE"};
        const i32 first[] = {0, 3, 6};
        s_ui_cw = 7 * dpi; renderer_set_ui_scale(r, s_ui_cw, 14 * dpi);
        for (i32 i = 0; i < 3; i++)
            draw_label_limited(r, groups[i], px + 22 * dpi,
                               settings_nav_group_y(sl, first[i], sp->nav_scroll, dpi) + 2 * dpi,
                               text_dim, (i32)((nav_w - 36 * dpi) / s_ui_cw));
        s_ui_cw = cw; renderer_set_ui_scale(r, cw, ch);
    }
    for (i32 i = 0; i < SETTINGS_TAB_COUNT; i++) {
        SettingsRect row = settings_nav_row(sl, i, sp->nav_scroll, dpi);
        bool active = settings_nav_order[i] == sp->active_tab;
        f32 inset = 2 * dpi;
        if (active) s_rrect_simple(r, row.x, row.y + inset, row.w, row.h - 2 * inset,
                                  nav_active_bg, 8 * dpi);
        else s_hover_wash(r, row.x, row.y + inset, row.w, row.h - 2 * inset,
                          8 * dpi, text_fg, 0.055f);
    }
    renderer_flush_rrects(r);
    for (i32 i = 0; i < SETTINGS_TAB_COUNT; i++) {
        SettingsRect row = settings_nav_row(sl, i, sp->nav_scroll, dpi);
        bool active = settings_nav_order[i] == sp->active_tab;
        f32 ix = sl.nav_icon_only ? row.x + (row.w - ch) * 0.5f : row.x + 12 * dpi;
        s_icon(r, settings_tab_icon(settings_nav_order[i]), ix, row.y + (row.h - ch) * 0.5f,
               ch, active ? accent : text_dim);
        if (!sl.nav_icon_only)
            draw_label_limited(r, settings_tab_name(settings_nav_order[i]), ix + ch + 10 * dpi,
                               row.y + (row.h - ch) * 0.5f, active ? text_fg : text_dim,
                               (i32)((row.x + row.w - 12 * dpi - ix - ch - 10 * dpi) / cw));
        sp_push_hover(sp, row.x, row.y, row.w, row.h);
    }
    renderer_flush_glyphs(r);
    if (sp->nav_max_scroll > 0) {
        f32 height = sl.nav_bottom - sl.nav_top;
        f32 thumb_h = fmaxf(18 * dpi, height * height / sl.nav_content_h);
        if (thumb_h > height) thumb_h = height;
        f32 thumb_y = sl.nav_top + (height - thumb_h) * sp->nav_scroll / sp->nav_max_scroll;
        Color thumb = text_dim; thumb.a = 0.48f;
        s_rrect_simple(r, px + nav_w - 4 * dpi, thumb_y, 2 * dpi, thumb_h, thumb, dpi);
        renderer_flush_rrects(r);
    }
    sp->clip_active = false;
    renderer_pop_scissor(r);

    SettingsRect reset = sl.reset;
    Color reset_fg = sp->reset_armed ? cp.btn_destructive_fg : text_dim;
    Color reset_fill = sp->reset_armed ? cp.btn_destructive_bg : theme->bg;
    Color reset_edge = sp->reset_armed ? cp.btn_destructive_fg : cp.divider_subtle;
    s_rrect_bordered(r, reset.x, reset.y, reset.w, reset.h, reset_fill, reset_edge,
                     dpi, 7 * dpi);
    s_hover_wash(r, reset.x, reset.y, reset.w, reset.h, 7 * dpi, reset_fg, 0.08f);
    renderer_flush_rrects(r);
    if (sl.nav_icon_only) {
        s_icon(r, sp->reset_armed ? ICON_CHECK : ICON_REFRESH,
               reset.x + (reset.w - ch) * 0.5f, reset.y + (reset.h - ch) * 0.5f, ch, reset_fg);
    } else {
        s_icon(r, ICON_REFRESH, reset.x + 10 * dpi, reset.y + (reset.h - ch) * 0.5f, ch, reset_fg);
        draw_label_limited(r, sp->reset_armed ? "Confirm reset" : "Reset settings",
                           reset.x + 36 * dpi, reset.y + (reset.h - ch) * 0.5f, reset_fg,
                           (i32)((reset.w - 44 * dpi) / cw));
    }
    sp_push_hover(sp, reset.x, reset.y, reset.w, reset.h);
    renderer_flush_glyphs(r);

    /* Content header */
    f32 content_x = sl.content_x;
    f32 content_top = sl.content_top;
    f32 content_w = sl.content_w;
    i32 content_max = (i32)(content_w / cw);
    if (content_max < 1) content_max = 1;
    s_scaled_label(r, settings_tab_name(sp->active_tab), content_x, content_top,
                   text_fg, content_w, 1.25f, dpi);
    draw_label_limited(r, settings_tab_desc(sp->active_tab), content_x,
                       content_top + 30 * dpi, text_dim, content_max);
    renderer_flush_glyphs(r);

    /* Content area — scrollable */
    f32 scroll_off = (f32)sp->scroll_y * ch * 0.5f;
    f32 content_y = content_top + settings_content_top_off(ch, dpi) - scroll_off;
    f32 row_h = 32 * dpi;
    f32 clip_top = content_top + settings_clip_top_off(ch, dpi);
    f32 clip_bottom = sl.clip_bottom;
    renderer_push_scissor(r, content_x - 4.0f * dpi, clip_top,
                          content_w + 8.0f * dpi, clip_bottom - clip_top);
    /* Everything pushed from here to the matching pop belongs to the scrolled
     * body, so its hover rects are clipped to the same band (see SettingsPanel
     * .clip_active). settings_handle_click applies the band too. */
    sp->clip_y0 = clip_top;
    sp->clip_y1 = clip_bottom;
    sp->clip_active = true;
    f32 content_end_y = content_y; /* updated at end of each tab block */
    Color label_fg = text_dim;
    Color value_fg = text_fg;
    Color dim_fg = color_lerp(panel_bg, text_fg, 0.52f);

    if (sp->active_tab == SETTINGS_TAB_BROWSER) {
        SettingsBrowserLayout g = settings_browser_layout(content_x, content_y, content_w, dpi);
        f32 pad = fminf(16 * dpi, content_w * 0.08f);
        Color card_bg = color_lerp(panel_bg, text_fg, cp.is_light ? 0.018f : 0.025f);
        Color card_edge = color_lerp(panel_bg, text_fg, cp.is_light ? 0.14f : 0.12f);
        s_rrect_bordered(r, g.access.x, g.access.y, g.access.w, g.access.h,
                         card_bg, card_edge, dpi, 10 * dpi);
        renderer_flush_rrects(r);
        i32 title_cells = (i32)((g.toggle.x - content_x - 2 * pad) / cw);
        draw_label_limited(r, "Liu Browser", content_x + pad, g.access.y + 18 * dpi, value_fg, title_cells);
        draw_label_limited(r, "Open previews and allowed sites in Liu.", content_x + pad,
                           g.access.y + 46 * dpi, label_fg, (i32)((content_w - 2 * pad) / cw));
        settings_draw_switch(r, g.toggle.x, g.toggle.y, sp->config->browser_enabled,
                             accent, card_bg, card_edge, value_fg, label_fg, cw, ch, dpi);
        sp_push_hover(sp, g.toggle.x, g.toggle.y, g.toggle.w, g.toggle.h);

        s_icon(r, ICON_FT_LOCK, content_x + 2 * dpi, g.local_y, ch, label_fg);
        draw_label_limited(r, "Local access is built in", content_x + ch + 10 * dpi,
                           g.local_y, label_fg, (i32)((content_w - ch - 10 * dpi) / cw));
        s_scaled_label(r, "Localhost, private networks, and Tailscale.", content_x + ch + 10 * dpi,
                       g.local_y + 22 * dpi, dim_fg, content_w - ch - 10 * dpi, 0.875f, dpi);

        i32 count = CLAMP(sp->config->browser_allowlist.count, 0, BROWSER_ALLOWLIST_MAX);
        draw_label_limited(r, "Allowed sites", content_x, g.heading_y, value_fg, content_max);
        char tally[32]; snprintf(tally, sizeof tally, "%d / %d", count, BROWSER_ALLOWLIST_MAX);
        f32 tally_w = (f32)strlen(tally) * 7 * dpi;
        s_scaled_label(r, tally, content_x + content_w - tally_w, g.heading_y + 2 * dpi,
                       dim_fg, tally_w + dpi, 0.875f, dpi);
        draw_label_limited(r, "Add a host, subdomain pattern, or URL.", content_x, g.hint_y, label_fg, content_max);
        s_scaled_label(r, "URL rules include every path at that site.", content_x, g.hint_y + 22 * dpi,
                       dim_fg, content_w, 0.875f, dpi);

        Color field_bg = color_lerp(panel_bg, theme->bg, 0.55f);
        Color field_edge = sp->browser_rule_status_error ? cp.btn_destructive_fg :
                           sp->browser_rule_editing ? color_lerp(card_edge, accent, 0.65f) : card_edge;
        s_rrect_bordered(r, g.input.x, g.input.y, g.input.w, g.input.h,
                         field_bg, field_edge, dpi, 8 * dpi);
        Color submit_bg = color_lerp(cp.btn_primary_bg, cp.btn_primary_hover,
                                     s_hover_amt(g.submit.x, g.submit.y, g.submit.w, g.submit.h));
        s_rrect_simple(r, g.submit.x, g.submit.y, g.submit.w, g.submit.h, submit_bg, 8 * dpi);
        renderer_flush_rrects(r);
        i32 field_cells = (i32)((g.input.w - 24 * dpi) / cw);
        if (field_cells < 1) field_cells = 1;
        f32 field_y = g.input.y + (g.input.h - ch) * 0.5f;
        if (sp->browser_rule_editing) {
            Color caret = value_fg; caret.a *= ui_caret_alpha();
            Color selection = accent; selection.a = 0.22f;
            s_edit_field(r, app, sp->browser_rule_buf, (i32)strlen(sp->browser_rule_buf),
                         g.input.x + 12 * dpi, field_y, cw, ch, field_cells,
                         value_fg, caret, selection, false, true);
        } else draw_label_limited(r, sp->browser_rule_buf[0] ? sp->browser_rule_buf : "example.com or https://example.com",
                                  g.input.x + 12 * dpi, field_y, dim_fg, field_cells);
        const char *submit_label = sp->browser_rule_index >= 0 ? "Save" : "Add site";
        draw_label_limited(r, submit_label, g.submit.x + (g.submit.w - strlen(submit_label) * cw) * 0.5f,
                           g.submit.y + (g.submit.h - ch) * 0.5f, cp.btn_primary_fg, (i32)(g.submit.w / cw));
        sp_push_hover(sp, g.input.x, g.input.y, g.input.w, g.input.h);
        sp_push_hover(sp, g.submit.x, g.submit.y, g.submit.w, g.submit.h);
        s_scaled_label(r, sp->browser_rule_status[0] ? sp->browser_rule_status : "Changes apply to the next navigation.",
                       content_x, g.status_y, sp->browser_rule_status_error ? cp.btn_destructive_fg : dim_fg,
                       content_w, 0.875f, dpi);

        f32 list_h = count ? (f32)count * g.row_h : 88 * dpi;
        s_rrect_bordered(r, content_x, g.list_y, content_w, list_h, card_bg, card_edge, dpi, 10 * dpi);
        renderer_flush_rrects(r);
        if (!count) {
            s_icon(r, ICON_NETWORK, content_x + pad, g.list_y + 24 * dpi, ch, label_fg);
            draw_label_limited(r, "No additional sites", content_x + pad + ch + 12 * dpi,
                               g.list_y + 20 * dpi, value_fg, (i32)((content_w - 2 * pad - ch - 12 * dpi) / cw));
            s_scaled_label(r, "Add a site to open it inside Liu.", content_x + pad + ch + 12 * dpi,
                           g.list_y + 46 * dpi, dim_fg, content_w - 2 * pad - ch - 12 * dpi, 0.875f, dpi);
        }
        for (i32 i = 0; i < count; i++) {
            SettingsBrowserRow row = settings_browser_row(g, i);
            if (i > 0) s_rect(r, content_x + pad, row.row.y, content_w - 2 * pad, dpi, card_edge);
            s_hover_wash(r, row.row.x + dpi, row.row.y + dpi, row.row.w - 2 * dpi, row.row.h - 2 * dpi,
                         8 * dpi, text_fg, 0.035f);
            s_hover_wash(r, row.edit.x, row.edit.y, row.edit.w, row.edit.h, 6 * dpi, text_fg, 0.08f);
            s_hover_wash(r, row.remove.x, row.remove.y, row.remove.w, row.remove.h, 6 * dpi, cp.btn_destructive_fg, 0.12f);
        }
        renderer_flush_rects(r);
        for (i32 i = 0; i < count; i++) {
            SettingsBrowserRow row = settings_browser_row(g, i);
            draw_label_limited(r, sp->config->browser_allowlist.rules[i], row.text_x, row.title_y,
                               value_fg, (i32)(row.text_w / cw));
            s_icon(r, ICON_PENCIL, row.edit.x + 6 * dpi, row.edit.y + 6 * dpi, 16 * dpi, label_fg);
            Color remove_fg = color_lerp(label_fg, cp.btn_destructive_fg,
                                        s_hover_amt(row.remove.x, row.remove.y, row.remove.w, row.remove.h));
            s_icon(r, ICON_CLOSE, row.remove.x + 6 * dpi, row.remove.y + 6 * dpi, 16 * dpi, remove_fg);
            sp_push_hover(sp, row.row.x, row.row.y, row.row.w, row.row.h);
        }
        /* One small-text batch for all row descriptions. */
        s_ui_cw = 7 * dpi; renderer_set_ui_scale(r, s_ui_cw, 14 * dpi);
        for (i32 i = 0; i < count; i++) {
            SettingsBrowserRow row = settings_browser_row(g, i);
            const char *rule = sp->config->browser_allowlist.rules[i];
            bool https = strncmp(rule, "https://", 8) == 0, http = strncmp(rule, "http://", 7) == 0;
            bool wildcard = strstr(rule, "*.") != NULL;
            const char *detail = https ? (wildcard ? "HTTPS subdomains · all paths" : "HTTPS site · all paths") :
                http ? (wildcard ? "HTTP subdomains · all paths" : "HTTP site · all paths") :
                wildcard ? "Subdomains · HTTP and HTTPS" : "Exact host · HTTP and HTTPS";
            draw_label_limited(r, detail, row.text_x, row.detail_y, dim_fg, (i32)(row.text_w / s_ui_cw));
        }
        s_ui_cw = cw; renderer_set_ui_scale(r, cw, ch);
        content_end_y = g.list_y + list_h + 12 * dpi;
    }

    if (sp->active_tab == SETTINGS_TAB_APPEARANCE) {
        f32 y = content_y;
        f32 col2 = content_x + 14 * cw; /* value column */
        f32 item_h = ch + 10;
        i32 font_cols = content_w >= 420.0f * dpi ? 3 :
                        content_w >= 260.0f * dpi ? 2 : 1;
        f32 font_row_h = ch + 16.0f * dpi;   /* same card height as themes */
        f32 font_gap = 6.0f * dpi;
        /* Theme tiles carry a colour strip, so they read at a smaller width
         * than a text-only row would — hence more columns. Geometry comes
         * from settings_layout.h; the hit test calls the same function, so
         * the two cannot drift. */
        i32 theme_cols = content_w >= 820.0f * dpi ? 6 :
                         content_w >= 640.0f * dpi ? 5 :
                         content_w >= 440.0f * dpi ? 4 :
                         content_w >= 260.0f * dpi ? 2 : 1;


        /* Font — current + selector */
        draw_label(r, "Font", content_x, y, label_fg);
        /* Show current font friendly name */
        const char *fname = sp->config->font_path;
        char font_full[1024];
        const char *font_cmp = settings_font_path_full(sp->config->font_path, font_full, sizeof font_full);
        for (i32 fi = 0; fi < g_font_count; fi++) {
            if (liu_path_equiv(font_cmp, g_fonts[fi].path)) {
                fname = g_fonts[fi].name;
                break;
            }
        }
        /* Fallback: extract filename from path */
        if (fname == sp->config->font_path) {
            for (const char *fp2 = fname; *fp2; fp2++) { if (*fp2 == '/') fname = fp2 + 1; }
        }

        /* Same object as the Create / Import pair under the theme grid, right-
         * aligned on the Font row. Geometry from settings_action_*, which the
         * hit test calls too. */
        SettingsFontHeader fh = settings_font_header(content_x, y, content_w, cw, ch, dpi);
        SettingsActionBtn addf = fh.add;
        draw_label_limited(r, fname, fh.value_x, y, value_fg, (i32)(fh.value_w / cw));
        s_action_button(r, sp, &addf, ICON_PLUS, SETTINGS_LBL_ADD_FONT,
                        &cp, dim_fg, false, dpi);
        y = fh.grid_y - 12 * dpi;

        /* Font grid — directly selectable. (No hairline: the cards carry
         * the grouping; the spacing below stays as breathing room.) */
        settings_section_rule(r, content_x, y - 4.0f * dpi, content_w,
                              cp.divider_subtle, dpi);
        y += 12.0f * dpi;
        f32 col_w = content_w / (f32)font_cols;
        for (i32 fi = 0; fi < g_font_count; fi++) {
            bool sel = liu_path_equiv(font_cmp, g_fonts[fi].path);
            f32 fx = content_x + (f32)(fi % font_cols) * col_w;
            f32 fy = y + (f32)(fi / font_cols) * (font_row_h + font_gap);

            /* Card geometry — byte-for-byte the theme grid's. */
            f32 pill_x = fx - 2.0f * dpi;
            f32 pill_y = fy - 4.0f * dpi;
            f32 pill_w = col_w - 8.0f * dpi;
            f32 pill_h = font_row_h;
            f32 radius = fminf(pill_h * 0.5f, 6.0f * dpi);
            f32 hv = s_hover_amt(pill_x, pill_y, pill_w, pill_h);
            if (sel) {
                Color sel_fill = cp.btn_primary_bg;
                sel_fill.a = 0.18f + 0.10f * hv;   /* quiet selection; brighter on hover */
                s_rrect_bordered(r, pill_x, pill_y, pill_w, pill_h,
                                 sel_fill, accent, fmaxf(1.0f, dpi), radius);
            } else {
                /* Hover lifts the tile: fill warms toward surface_raised, the
                 * hairline border warms toward the accent. */
                Color tile_bg = color_lerp(cp.surface_sunken, cp.surface_raised, hv);
                tile_bg.a = 0.55f + 0.30f * hv;
                Color bd = color_lerp(cp.divider_subtle, accent, 0.55f * hv);
                s_rrect_bordered(r, pill_x, pill_y, pill_w, pill_h,
                                 tile_bg, bd, fmaxf(1.0f, dpi), radius);
            }
            renderer_flush_rrects(r);
            Color fc = value_fg;
            f32 fty = pill_y + (pill_h - ch) * 0.5f;

            /* Selected card carries the same filled-accent tick the theme
             * grid uses, so "chosen" looks identical wherever it appears. */
            f32 fbadge = ch + 2.0f * dpi;
            f32 fbadge_x = pill_x + pill_w - fbadge - 8.0f * dpi;
            f32 fname_right = sel ? fbadge_x - 6.0f * dpi
                                  : pill_x + pill_w - 8.0f * dpi;
            i32 fname_max = (i32)((fname_right - (fx + 10.0f * dpi)) / cw);
            if (fname_max < 1) fname_max = 1;
            draw_label_limited(r, g_fonts[fi].name, fx + 10.0f * dpi, fty, fc,
                               fname_max);
            if (sel) {
                s_rrect_simple(r, fbadge_x, pill_y + (pill_h - fbadge) * 0.5f,
                               fbadge, fbadge, accent, fbadge * 0.5f);
                renderer_flush_rrects(r);
                f32 ftick = fbadge * 0.62f;
                s_icon(r, ICON_CHECK,
                          fbadge_x + (fbadge - ftick) * 0.5f,
                          pill_y + (pill_h - ftick) * 0.5f,
                          ftick, chrome_legible_on(accent));
            }
            sp_push_hover(sp, pill_x, pill_y, pill_w, pill_h);
        }
        y += ((g_font_count + font_cols - 1) / font_cols) *
             (font_row_h + font_gap) + 16.0f * dpi;

        y += 10;

        /* Size / Line Height — segmented steppers. The - and + glyphs sit
         * at col2 + 7cw / 9cw, the x positions settings_handle_click's
         * zones test; the bordered pill around them is pure chrome. */
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
            f32 _srad = fminf(_sh * 0.5f, 6.0f * dpi);                         \
            s_rrect_bordered(r, _sx, _sy, _sw, _sh, _seg_bg,                    \
                             cp.divider_strong, fmaxf(1.0f, dpi), _srad);       \
            renderer_flush_rrects(r);                                           \
            /* Highlight the − / + half the pointer is over. */                 \
            { f32 _hl = s_hover_amt(_sx, _sy, _sw * 0.5f, _sh);                 \
              if (_hl > 0.001f) { Color _wl = accent; _wl.a = 0.16f * _hl;      \
                s_rrect_per_corner(r, _sx, _sy, _sw * 0.5f, _sh, _wl,           \
                                   _srad, 0.0f, 0.0f, _srad);                   \
                renderer_flush_rrects(r); } }                                   \
            { f32 _hr = s_hover_amt(_sx + _sw * 0.5f, _sy, _sw * 0.5f, _sh);    \
              if (_hr > 0.001f) { Color _wr = accent; _wr.a = 0.16f * _hr;      \
                s_rrect_per_corner(r, _sx + _sw * 0.5f, _sy, _sw * 0.5f, _sh,   \
                                   _wr, 0.0f, _srad, _srad, 0.0f);              \
                renderer_flush_rrects(r); } }                                   \
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
        y += item_h + 6.0f * dpi;   /* air between the stepper boxes */

        /* Text Weight — a manual trim on how heavy text draws, in two places:
         * the terminal grid, and everything else (palette, tabs, sidebar,
         * editor). Same segmented steppers as Size and Line Height; the click
         * zones below take their rows from the same settings_weight_block. */
        {
            SettingsWeightBlock wb = settings_weight_block(y, item_h, dpi);
            draw_label(r, "Text Weight", content_x, wb.head_y, label_fg);
            draw_label(r, "0 = default", col2, wb.head_y, dim_fg);
            char wt[16], wu[16];
            settings_weight_str(wt, sizeof wt, sp->config->text_weight_terminal);
            settings_weight_str(wu, sizeof wu, sp->config->text_weight_ui);
            y = wb.term_y;
            APPEAR_STEPPER("  Terminal", "%s", wt);
            y = wb.ui_y;
            APPEAR_STEPPER("  Interface", "%s", wu);
            y = wb.end;
        }
        #undef APPEAR_STEPPER
        (void)buf;

        y += 10;

        /* Theme — compact grid. User themes (index >= THEME_COUNT) get a
         * trailing close-icon button that deletes the on-disk JSON.
         * Built-ins have no delete affordance — they live in static
         * const storage and theme_delete_user refuses to touch names
         * not in the user-themes cache. */
        draw_label(r, "Theme", content_x, y, label_fg);
        y += ch + 8.0f;
        settings_section_rule(r, content_x, y, content_w, cp.divider_subtle, dpi);
        y += 8.0f;

        i32 theme_count;
        const char **theme_names = theme_list_names(&theme_count);
        /* Delete-button geometry lives in settings_theme_card() (card.del_*),
         * so the painter and the hit test read the same numbers. */

        for (i32 i = 0; i < theme_count; i++) {
            bool selected = (strcmp(sp->config->theme_name, theme_names[i]) == 0);
            bool is_user_theme = (i >= THEME_COUNT);
            ThemeCardRect card = settings_theme_card(i, theme_cols, content_x, y,
                                                     content_w, ch, dpi);
            f32 pill_x = card.x;
            f32 pill_w = card.w;
            f32 pill_y = card.y;
            f32 pill_h = card.h;
            f32 radius = 8.0f * dpi;
            /* Every tile is a quiet card, so the list reads as contained
             * controls; selection swaps the hairline for an accent ring
             * over a faint accent wash. */
            f32 hv = s_hover_amt(pill_x, pill_y, pill_w, pill_h);
            if (selected) {
                Color sel_fill = cp.btn_primary_bg;
                sel_fill.a = 0.18f + 0.10f * hv;
                s_rrect_bordered(r, pill_x, pill_y, pill_w, pill_h,
                                 sel_fill, accent, fmaxf(1.0f, dpi), radius);
            } else {
                /* Hover lifts the tile: fill warms toward surface_raised, the
                 * hairline border warms toward the accent. */
                Color tile_bg = color_lerp(cp.surface_sunken, cp.surface_raised, hv);
                tile_bg.a = 0.55f + 0.30f * hv;
                Color bd = color_lerp(cp.divider_subtle, accent, 0.55f * hv);
                s_rrect_bordered(r, pill_x, pill_y, pill_w, pill_h,
                                 tile_bg, bd, fmaxf(1.0f, dpi), radius);
            }
            renderer_flush_rrects(r);

            /* Colour preview: the theme's own background with a band of its
             * own colours along the bottom, which is why the tile is taller
             * than a name. A column of names says nothing about what each
             * theme looks like, so without it every theme would have to be
             * selected to be seen. */
            {
                const Theme *pt = theme_get_by_name(theme_names[i]);
                if (pt) {
                    s_rect(r, card.swatch_x, card.swatch_y,
                                       card.swatch_w, card.swatch_h, pt->bg);
                    /* Prompt mark, so the strip reads as a terminal. */
                    draw_label(r, ">_", card.swatch_x + 4.0f * dpi,
                               card.swatch_y + 3.0f * dpi, pt->fg);
                    /* Band along the bottom: the colours THIS theme is
                     * actually wearing — text, cursor, accent, scrollbar,
                     * selection, active sidebar row, active tab — ordered
                     * light-to-dark so the strip reads as the theme's own ramp
                     * instead of an arbitrary row of chips.
                     *
                     * Not ANSI slots 1..7: every palette puts the same hues in
                     * those slots in the same order, so every tile would paint
                     * the same red-green-yellow-blue-magenta-cyan-white
                     * rainbow, a monochrome theme included. The chrome colours
                     * differ per theme by construction, so the band varies
                     * with the theme and a monochrome theme reads as
                     * monochrome.
                     *
                     * Selection carries alpha (it is drawn OVER text), so it
                     * is composited onto the theme's own bg rather than onto
                     * whatever is behind the settings panel. */
                    Color sel = pt->selection;
                    Color sel_flat = {
                        sel.r * sel.a + pt->bg.r * (1.0f - sel.a),
                        sel.g * sel.a + pt->bg.g * (1.0f - sel.a),
                        sel.b * sel.a + pt->bg.b * (1.0f - sel.a),
                        1.0f
                    };
                    Color band_cols[] = {
                        pt->fg,
                        pt->cursor,
                        theme_ui_accent(pt),
                        pt->scrollbar_thumb,
                        sel_flat,
                        pt->sidebar_active,
                        pt->tab_active_bg,
                    };
                    i32 nslot = (i32)(sizeof band_cols / sizeof band_cols[0]);
                    f32 band_h = settings_theme_band_h(dpi);
                    f32 band_y = card.swatch_y + card.swatch_h - band_h;
                    /* Flush with the strip: the band is the preview's footer,
                     * not a chip floating on it. An inset would leave a sliver
                     * of the theme's bg at each end, visible on any theme
                     * whose end colours contrast with its own bg and invisible
                     * on the rest.
                     *
                     * Each segment spans [k, k+1] of the strip computed from
                     * the strip's own width rather than stepping by a rounded
                     * segment width: adjacent segments then share an exact
                     * edge (no seam between them) and the last one lands
                     * exactly on the strip's right edge instead of a fraction
                     * of a pixel short. */
                    for (i32 k = 0; k < nslot; k++) {
                        /* A theme that omitted an optional chrome key leaves
                         * it zero-initialised (alpha 0). Painting that would
                         * punch a transparent hole in the band and show the
                         * settings panel through it, so fall back to the
                         * theme's own bg. */
                        Color c = band_cols[k];
                        if (c.a <= 0.001f) c = pt->bg;
                        c.a = 1.0f;
                        f32 x0 = card.swatch_x
                               + card.swatch_w * (f32)k / (f32)nslot;
                        f32 x1 = card.swatch_x
                               + card.swatch_w * (f32)(k + 1) / (f32)nslot;
                        s_rect(r, x0, band_y, x1 - x0, band_h, c);
                    }
                    renderer_flush_rects(r);
                }
            }

            f32 prev_x = card.name_x;
            f32 text_y = card.name_y;
            Color tc = selected ? cp.btn_primary_bg : value_fg;
            /* Clamp the name inside the pill: stop before the delete button
             * (user themes) or the right padding, with an ellipsis when cut. */
            {
                /* Clear the delete BUTTON, not just its glyph — the button
                 * paints a filled background on hover, and a name measured to
                 * the glyph box would sit under that fill. */
                f32 name_right = is_user_theme ? card.del_btn_x - 4.0f * dpi
                                               : card.x + card.w - 6.0f * dpi;
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

            if (selected) {
                /* Filled accent disc with a tick — the mockup's "this is the
                 * active one" marker. */
                Color disc = accent;
                s_rrect_simple(r, card.badge_x, card.badge_y,
                               card.badge_sz, card.badge_sz, disc,
                               card.badge_sz * 0.5f);
                renderer_flush_rrects(r);
                f32 tick = card.badge_sz * 0.62f;
                s_icon(r, ICON_CHECK,
                          card.badge_x + (card.badge_sz - tick) * 0.5f,
                          card.badge_y + (card.badge_sz - tick) * 0.5f,
                          tick, chrome_legible_on(disc));
            }

            if (is_user_theme) {
                /* Draw the delete glyph red at rest with subdued alpha. Hover
                 * increases its strength and lightly tints the background; the
                 * confirmation dialog handles deletion. The painted bounds and
                 * hit-test use the same `card.del_btn_*` rectangle.
                 *
                 * Keep the glyph size fixed because `icon_draw` caches its
                 * raster and GPU texture by kind and size; changing size during
                 * the fade would create a new upload each frame. */
                f32 dhv = s_hover_amt(card.del_btn_x, card.del_btn_y,
                                      card.del_btn_sz, card.del_btn_sz);
                Color danger = cp.btn_destructive_fg;
                if (dhv > 0.001f) {
                    /* The palette's own destructive tint, so the wash tracks
                     * the theme instead of a second hardcoded red. */
                    Color fill = cp.btn_destructive_bg;
                    fill.a = 0.22f * dhv;
                    s_rrect_simple(r, card.del_btn_x, card.del_btn_y,
                                   card.del_btn_sz, card.del_btn_sz,
                                   fill, card.del_btn_sz * 0.30f);
                    renderer_flush_rrects(r);
                }
                /* Quiet red -> full-strength red as the wash comes up. */
                Color del_fg = danger;
                del_fg.a = 0.62f + 0.38f * dhv;
                s_icon(r, ICON_CLOSE, card.del_x, card.del_y,
                          card.del_sz, del_fg);
            }
            sp_push_hover(sp, pill_x, pill_y, pill_w, pill_h);
        }
        f32 grid_end_y = y + settings_theme_grid_h(theme_count, theme_cols, ch, dpi);

        /* Create / Import — the primary action and its alternative, one shape,
         * one width, drawn by the same painter as every other action button in
         * the panel. Geometry from settings_action_row_theme(), which the hit
         * test calls with the same arguments. */
        SettingsActionBtn cbtn, ibtn;
        settings_action_row_theme(content_x, content_w, grid_end_y,
                                  cw, ch, dpi, &cbtn, &ibtn);
        s_action_button(r, sp, &cbtn, ICON_PLUS, SETTINGS_LBL_CREATE_THEME,
                        &cp, dim_fg, true,  dpi);
        s_action_button(r, sp, &ibtn, ICON_UP,   SETTINGS_LBL_IMPORT_THEME,
                        &cp, dim_fg, false, dpi);

        /* --- WINDOW section --------------------------------------------
         * Opacity, blur and the wallpaper are what the window LOOKS like, so
         * they belong beside the theme and the font rather than at the end of
         * the Terminal tab, where they sat behind cursor and scrollback
         * settings that have nothing to do with them.
         *
         * The section brings its own control geometry: the Appearance tab is
         * laid out around cards and grids, not the label/value column the
         * rows below assume. Same numbers as the Terminal tab so the two read
         * identically — and the hit test derives its column from the same
         * expressions. */
        f32 y2 = grid_end_y + cbtn.h + 18.0f * dpi;
        {
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
            char buf[64];
            f32 y = y2;

        /* Every chip row below shares ONE width (settings_chip_w) so the
         * columns line up instead of each row sizing itself from its own
         * label lengths. */
        y += 12.0f * dpi;
        draw_label(r, "WINDOW", content_x, y, dim_fg);
        {
            f32 rule_x = content_x + fminf(10.0f * cw, content_w * 0.50f);
            f32 rule_w = content_x + content_w - rule_x;
            if (rule_w > 0.0f)
                s_rect(r, rule_x, y + ch * 0.65f, rule_w, fmaxf(1.0f, dpi), section_rule);
        }
        y += ch + 12.0f * dpi;

        /* A slider displays the range and current value. */
        {
            f32 read_w = 6.0f * cw;
            f32 track_w = value_room - read_w - 8.0f * dpi;
            if (track_w < 6.0f * cw) track_w = value_room * 0.6f;
            draw_label(r, "Opacity:", content_x, y, label_fg);
            f32 op = sp->config->opacity;
            f32 t  = (op - SETTINGS_OPACITY_MIN) /
                     (1.0f - SETTINGS_OPACITY_MIN);
            settings_draw_slider(r, value_x, y - 4.0f * dpi, track_w, control_h,
                                 t, true, accent, control_bg, control_border, dpi);
            snprintf(buf, sizeof buf, "%.0f%%", op * 100.0f);
            draw_value(r, buf, value_x + track_w + 8.0f * dpi, y, value_fg);
            sp_push_hover(sp, value_x, y - 4.0f * dpi, track_w, control_h);
        }
        y += row_h;

        /* Blur — four discrete steps, so a segmented control rather than chips
         * like the Choose…/Remove ACTIONS below: a control that picks one of N
         * states and a pair of buttons that DO something should not look
         * identical. */
        {
            const i32 n = (i32)(sizeof s_blur_presets / sizeof s_blur_presets[0]);
            f32 cur = sp->config->background_blur ? sp->config->background_blur_radius : 0.0f;
            i32 sel = -1;
            for (i32 pi = 0; pi < n; pi++)
                if (fabsf(cur - s_blur_presets[pi]) < 0.5f) sel = pi;
            f32 seg_w = settings_chip_w(value_room, cw);
            draw_label(r, "Blur:", content_x, y, label_fg);
            settings_draw_segmented(r, value_x, y - 4.0f * dpi,
                                    s_blur_labels, n, sel, seg_w, control_h,
                                    control_active, control_bg, control_border,
                                    value_fg, label_fg, cw, ch, dpi);
            sp_push_hover(sp, value_x, y - 4.0f * dpi, seg_w * (f32)n, control_h);
        }
        y += row_h;

        /* --- Wallpaper ------------------------------------------------------
         * Fit and Fade only mean anything once an image is chosen, so the
         * whole group dims and stops responding until there is an image, and
         * it is indented under the row that produces one, so the dependency
         * is visible rather than implied. */
        bool has_img = sp->config->background_image[0] != '\0';
        draw_label(r, "Wallpaper:", content_x, y, label_fg);
        {
            /* The current file IS the value of this row; the two actions sit
             * after it as buttons, which is what they are. */
            const char *img = sp->config->background_image;
            const char *shown = img[0] ? path_basename(img) : "None";
            f32 btn_w = 9.0f * cw;
            f32 gap   = 6.0f * dpi;
            f32 name_room = value_room - btn_w * 2.0f - gap * 2.0f;
            if (name_room < 6.0f * cw) name_room = 6.0f * cw;
            i32 room = (i32)(name_room / cw);
            if (room < 4) room = 4;
            char nb[160];
            snprintf(nb, sizeof nb, "%.*s", room, shown);
            draw_label(r, nb, value_x, y, img[0] ? value_fg : dim_fg);

            f32 bx = value_x + value_room - btn_w * 2.0f - gap;
            static const char *bl[2] = { "Choose\u2026", "Remove" };
            settings_draw_chip_row(r, bx, y - 4.0f * dpi, btn_w, control_h,
                                   bl, 2, -1,
                                   control_active, control_bg, control_border,
                                   value_fg, has_img ? label_fg : dim_fg,
                                   cw, ch, dpi);
            sp_push_hover(sp, bx, y - 4.0f * dpi, btn_w * 2.0f, control_h);
        }
        y += row_h;

        /* Indent + a rail: the two rows below belong to the wallpaper above. */
        f32 sub_x  = content_x + 2.0f * cw;
        f32 sub_vx = value_x;
        f32 sub_room = content_x + content_w - sub_vx;
        f32 rail_top = y - 6.0f * dpi;

        {
            const i32 n = (i32)(sizeof s_fit_modes / sizeof s_fit_modes[0]);
            i32 sel = -1;
            for (i32 pi = 0; pi < n; pi++)
                if (sp->config->background_mode == s_fit_modes[pi]) sel = pi;
            f32 seg_w = settings_chip_w(sub_room, cw);
            draw_label(r, "Fit:", sub_x, y, has_img ? label_fg : dim_fg);
            Color fit_active = has_img ? control_active
                                       : settings_blend(control_bg, control_active, 0.35f);
            settings_draw_segmented(r, sub_vx, y - 4.0f * dpi,
                                    s_fit_labels, n, sel, seg_w, control_h,
                                    fit_active, control_bg, control_border,
                                    has_img ? value_fg : dim_fg, dim_fg, cw, ch, dpi);
            if (has_img)
                sp_push_hover(sp, sub_vx, y - 4.0f * dpi, seg_w * (f32)n, control_h);
        }
        y += row_h;

        {
            f32 read_w = 6.0f * cw;
            f32 track_w = sub_room - read_w - 8.0f * dpi;
            if (track_w < 6.0f * cw) track_w = sub_room * 0.6f;
            draw_label(r, "Fade:", sub_x, y, has_img ? label_fg : dim_fg);
            f32 fa = sp->config->background_opacity;
            settings_draw_slider(r, sub_vx, y - 4.0f * dpi, track_w, control_h,
                                 fa, has_img, accent, control_bg, control_border, dpi);
            snprintf(buf, sizeof buf, "%.0f%%", fa * 100.0f);
            draw_value(r, buf, sub_vx + track_w + 8.0f * dpi, y,
                       has_img ? value_fg : dim_fg);
            if (has_img)
                sp_push_hover(sp, sub_vx, y - 4.0f * dpi, track_w, control_h);
        }

        /* The rail is drawn last so it spans exactly the two rows it owns. */
        {
            Color rail = section_rule;
            rail.a *= has_img ? 0.9f : 0.5f;
            s_rect(r, content_x + 0.6f * cw, rail_top,
                   fmaxf(1.0f, dpi), (y + ch) - rail_top, rail);
        }

            y2 = y;
        }
        content_end_y = y2 + ch + 16;

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
        draw_label_limited(r, "Style:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_segmented(r, value_x, y - 4.0f * dpi,
                                cursor_names, 3, sp->config->cursor_style % 3,
                                cursor_seg_w, control_h,
                                control_active, control_bg,
                                control_border, value_fg,
                                value_fg, cw, ch, dpi);
        y += row_h;

        draw_label_limited(r, "Blink:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
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
        draw_label_limited(r, "History lines:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        SettingsStepper history = settings_stepper(value_x, y, value_room, ch, dpi);
        snprintf(buf, sizeof buf, "%d", sp->config->scrollback_lines);
        draw_label_limited(r, buf, history.value_x, y, value_fg, (i32)(history.value_w / cw));
        SettingsRect steps[] = {history.minus, history.plus};
        for (i32 i = 0; i < 2; i++) {
            s_rrect_bordered(r, steps[i].x, steps[i].y, steps[i].w, steps[i].h,
                             control_bg, control_border, dpi, 6 * dpi);
            renderer_flush_rrects(r);
            draw_label(r, i ? "+" : "−", steps[i].x + (steps[i].w - cw) * 0.5f,
                       steps[i].y + (steps[i].h - ch) * 0.5f, value_fg);
            sp_push_hover(sp, steps[i].x, steps[i].y, steps[i].w, steps[i].h);
        }
        y += row_h;

        draw_label_limited(r, "Bright bold text:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->bold_is_bright, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* Selecting, copying automatically and changing what is copied are
         * three separate choices, so each switch says which one it is and a
         * dim caption says WHEN it acts (UX-12). A caption is a row of its
         * own; the click walk below mirrors every one of them. */
        i32 cap_cols = (i32)(value_room / cw);
        if (cap_cols < 1) cap_cols = 1;
        y = settings_group_heading(r, "SELECTION", content_x, y, content_w, ch, dpi, dim_fg, section_rule);
        draw_label_limited(r, "Drag selection:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->mouse_drag_selects, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;
        draw_label_limited(r, sp->config->mouse_drag_selects
                           ? "Also in mouse apps. A click still reaches them."
                           : "Mouse apps get the mouse. Hold Shift to select.",
                           value_x, y, dim_fg, cap_cols);
        y += row_h;

        draw_label_limited(r, "Copy on Select:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->copy_on_select, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;
        draw_label_limited(r, sp->config->copy_on_select
                           ? "Copies when you release the mouse."
                           : "Selecting does not copy. Use " UI_CHORD_COPY ".",
                           value_x, y, dim_fg, cap_cols);
        y += row_h;

        draw_label_limited(r, "Trim Line Ends:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->clipboard_trim_trailing_spaces,
                             control_active, control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;
        draw_label_limited(r, "Removes spaces at the end of each copied line.",
                           value_x, y, dim_fg, cap_cols);
        y += row_h;

        /* Option=Alt → 4-up segmented. "Off"
         * keeps Option composing characters, which is what non-US layouts need
         * for '@', '#', '[', ']'. */
        {
            const char *opt_names[] = {"Off", "Left", "Right", "Both"};
            f32 opt_seg_w = 7.0f * cw;
            if (value_room > 0.0f && opt_seg_w * 4.0f > value_room) {
                opt_seg_w = value_room / 4.0f;
            }
            if (opt_seg_w < 1.0f) opt_seg_w = 1.0f;
            draw_label_limited(r, "Option=Alt:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
            settings_draw_segmented(r, value_x, y - 4.0f * dpi,
                                    opt_names, 4, (i32)sp->config->option_as_alt,
                                    opt_seg_w, control_h,
                                    control_active, control_bg,
                                    control_border, value_fg,
                                    value_fg, cw, ch, dpi);
        }
        y += row_h;

        y = settings_group_heading(r, "WORKSPACE", content_x, y, content_w, ch, dpi, dim_fg, section_rule);
        draw_label_limited(r, "Tab Bar:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->show_tab_bar, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label_limited(r, "Toolbar Icons:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->show_toolbar_icons, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* ---- Sidebar (ui/sidebar_layout.h) ----
         * Seven rows, mirrored one for one in settings_handle_click below. The
         * tab-strip switch is first because it changes the most: on, the pills
         * leave the top bar for a vertical list in the sidebar. */
        y = settings_group_heading(r, "SIDEBAR", content_x, y, content_w, ch, dpi, dim_fg, section_rule);
        draw_label_limited(r, "Tabs in Sidebar:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->sidebar_tabs, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label_limited(r, "Sidebar Files:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->sidebar_enabled[SB_SEC_FILES], control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label_limited(r, "Sidebar Agents:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->sidebar_enabled[SB_SEC_AGENTS], control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label_limited(r, "Sidebar Notes:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->sidebar_enabled[SB_SEC_NOTES], control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label_limited(r, "Compact Sidebar:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->sidebar_compact, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* Below this window width the docked sidebar folds to its icon rail. */
        {
            const char *fold_names[] = {"Never", "1100pt", "1300pt"};
            i32 fold_sel = sp->config->sidebar_collapse_below_pt <= 0.0f ? 0
                         : sp->config->sidebar_collapse_below_pt < 1200.0f ? 1 : 2;
            f32 fold_seg_w = 8.0f * cw;
            if (value_room > 0.0f && fold_seg_w * 3.0f > value_room)
                fold_seg_w = value_room / 3.0f;
            if (fold_seg_w < 1.0f) fold_seg_w = 1.0f;
            draw_label_limited(r, "Collapse below:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
            settings_draw_segmented(r, value_x, y - 4.0f * dpi,
                                    fold_names, 3, fold_sel,
                                    fold_seg_w, control_h,
                                    control_active, control_bg,
                                    control_border, value_fg,
                                    value_fg, cw, ch, dpi);
        }
        y += row_h;

        /* The popover with the rest of it — section order, width, reset.
         * Cmd+K and this button open it. Same object as the Create / Import
         * pair. */
        draw_label_limited(r, "Sidebar Layout:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        {
            SettingsActionBtn sbb = settings_action_row_sidebar(value_x, value_room,
                                                                y, cw, ch, dpi);
            s_action_button(r, sp, &sbb, ICON_SLIDERS, SETTINGS_LBL_CUSTOMIZE_SIDEBAR,
                            &cp, dim_fg, false, dpi);
        }
        y += row_h;

        y = settings_group_heading(r, "WINDOW", content_x, y, content_w, ch, dpi, dim_fg, section_rule);
#ifdef PLATFORM_LINUX
        /* Linux-only: X11 has no server-side decoration to fall back on, so
         * this hides the whole top strip. Kept in lockstep with the matching
         * hit-test block in settings_handle_click — same #ifdef, same slot. */
        draw_label_limited(r, "Hide Title Bar:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->hide_title_bar, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;
#endif

        draw_label_limited(r, "Scrollbar:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->show_scrollbar, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label_limited(r, "Auto-hide Scrollbar:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->scrollbar_autohide, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        draw_label_limited(r, "Status Bar:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->show_status_bar, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* Ask before a paste with line breaks reaches a program that did not
         * enable bracketed paste (ui/paste_confirm.h). Same row order as the
         * click handler below. */
        draw_label_limited(r, "Confirm paste:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->paste_protection, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* Ask before closing a tab whose PTY runs an AI agent CLI. */
        y = settings_group_heading(r, "AGENT WORKFLOWS", content_x, y, content_w, ch, dpi, dim_fg, section_rule);
        draw_label_limited(r, "Confirm close:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
        settings_draw_switch(r, value_x, y - 2.0f * dpi,
                             sp->config->confirm_close_agent, control_active,
                             control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* Prompt Recall — the "what did I ask this agent?" chip in an agent
         * pane's top-right corner. Off / a chip that opens on click / the panel
         * open from the start. Kept in step with the click handler below, which
         * walks the same rows in the same order. */
        {
            const char *pr_names[] = {"Off", "Badge", "Always"};
            f32 pr_seg_w = 8.0f * cw;
            if (value_room > 0.0f && pr_seg_w * 3.0f > value_room) {
                pr_seg_w = value_room / 3.0f;
            }
            if (pr_seg_w < 1.0f) pr_seg_w = 1.0f;
            draw_label_limited(r, "Last prompt:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
            settings_draw_segmented(r, value_x, y - 4.0f * dpi,
                                    pr_names, 3, (i32)sp->config->prompt_recall,
                                    pr_seg_w, control_h,
                                    control_active, control_bg,
                                    control_border, value_fg,
                                    value_fg, cw, ch, dpi);
        }
        y += row_h;
        draw_label(r, "Shows the last prompt you sent to an agent pane.",
                   value_x, y, dim_fg);
        y += row_h;

        /* Tab Sleep: preset chips row (rounded outer corners). */
        draw_label_limited(r, "Sleep idle tabs:", content_x, y, label_fg, (i32)(fmaxf(0, value_off - 8 * dpi) / cw));
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
        draw_label(r, "PROMPT TRANSLATION", content_x, y, dim_fg);
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
        draw_label(r, "Enabled:", content_x, y, label_fg);
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
         * clip, and the "Run with:" caption makes them unambiguous. */
        {
            const char *names[] = {"Agent", "Local", "API"};
            i32 sel = (tc->backend == TRANSLATE_BACKEND_LOCAL) ? 1
                    : (tc->backend == TRANSLATE_BACKEND_API)   ? 2 : 0;
            f32 seg_w = 10.0f * cw;
            if (value_room > 0.0f && seg_w * 3.0f > value_room) {
                seg_w = value_room / 3.0f;
            }
            if (seg_w < 1.0f) seg_w = 1.0f;
            draw_label(r, "Run with:", content_x, y, label_fg);
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
            /* Agent → segmented picker (Claude / OpenCode / Codex / Grok /
             * Agy). Drives tc->agent_id when Backend = Agent CLI. Labelled
             * "Agy" rather than "Antigravity" so the segment stays inside its
             * 9-cell width. */
            draw_label(r, "Agent:", content_x, y, label_fg);
            {
                const char *names[] = {"Claude", "OpenCode", "Codex", "Grok",
                                       "Agy"};
                const char *ids[]   = {"claude", "opencode", "codex", "grok",
                                       "antigravity"};
                const i32 seg_n = (i32)(sizeof ids / sizeof ids[0]);
                i32 sel = 0;
                for (i32 i = 0; i < seg_n; i++) {
                    if (strcmp(tc->agent_id, ids[i]) == 0) { sel = i; break; }
                }
                f32 seg_w = 9.0f * cw;
                if (value_room > 0.0f && seg_w * (f32)seg_n > value_room) {
                    seg_w = value_room / (f32)seg_n;
                }
                if (seg_w < 1.0f) seg_w = 1.0f;
                settings_draw_segmented(r, value_x, y - 4.0f * dpi,
                                        names, seg_n, sel,
                                        seg_w, control_h,
                                        control_active, control_bg,
                                        control_border, value_fg,
                                        value_fg, cw, ch, dpi);
            }
            y += row_h;

            /* Model → the chip the model menu hangs from. Empty string = the
             * CLI's own default; it reads "Default", as the menu's own row
             * does, so the chip never looks blank. The menu itself is drawn
             * last, over the tab (see the end of this block). */
            draw_label(r, "Model:", content_x, y, label_fg);
            {
                const char *disp = tc->agent_model[0] ? tc->agent_model : SETTINGS_MODEL_LBL_DEFAULT;
                f32 chip_w = value_room;
                if (chip_w < 12.0f * cw) chip_w = 12.0f * cw;
                f32 radius = fminf(control_h * 0.5f, 6.0f * dpi);
                bool picker_open = sp->translate_model_picker_open;
                sp->tr_model_chip_x = value_x;  sp->tr_model_chip_y = y - 4.0f * dpi;
                sp->tr_model_chip_w = chip_w;   sp->tr_model_chip_h = control_h;
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
                /* The trailing chevron indicates the menu state: ▾ when
                 * closed and ▴ when open. Clicking the chip opens options
                 * rather than cycling values. */
                const char *chevron = picker_open ? "\xe2\x96\xb4" : "\xe2\x96\xbe";
                f32 chev_w = (f32)cw;
                f32 chev_x = value_x + chip_w - 10.0f * dpi - chev_w;
                i32 max_chars = (i32)((chev_x - (value_x + 8.0f * dpi)) / cw);
                if (max_chars < 1) max_chars = 1;
                draw_label_limited(r, disp, value_x + 8.0f * dpi, y,
                                   tc->agent_model[0] ? value_fg : dim_fg,
                                   max_chars);
                draw_label(r, chevron, chev_x, y, dim_fg);
                sp_push_hover(sp, value_x, y - 4.0f * dpi, chip_w, control_h);
            }
            y += row_h;
        } else if (tc->backend == TRANSLATE_BACKEND_API) {
            bool custom = strcmp(tc->api_provider, "custom") == 0;
            /* The three inline editors are shared with the Voice tab, so ask
             * whether THIS tab owns them rather than reading the raw flags. */
            const bool ed_key   = sp_api_edit_key(sp, SETTINGS_TAB_TRANSLATE);
            const bool ed_url   = sp_api_edit_url(sp, SETTINGS_TAB_TRANSLATE);
            const bool ed_model = sp_api_edit_model(sp, SETTINGS_TAB_TRANSLATE);
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
                Color chip_bg = ed_key ? control_active : control_bg;
                s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w,
                                 control_h, chip_bg, control_border,
                                 fmaxf(1.0f, dpi), radius);
                if (ed_key) {
                    s_rrect_bordered_per_corner(r, value_x,
                        y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
                }
                renderer_flush_rrects(r);
                i32 max_chars = (i32)((chip_w - 16.0f * dpi) / cw);
                if (max_chars < 4) max_chars = 4;
                if (ed_key) {
                    Color kc = {text_fg.r, text_fg.g, text_fg.b,
                                0.8f * ui_caret_alpha()};
                    Color ks = {accent.r, accent.g, accent.b, 0.30f};
                    s_edit_field(r, app, sp->api_key_buf,
                                       (i32)strlen(sp->api_key_buf),
                                       value_x + 8.0f * dpi, y, cw, ch,
                                       max_chars - 1, value_fg, kc, ks,
                                       true, true);
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
            if (ed_model) {
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
                Color mc = {text_fg.r, text_fg.g, text_fg.b,
                            0.8f * ui_caret_alpha()};
                Color ms = {accent.r, accent.g, accent.b, 0.30f};
                s_edit_field(r, app, sp->api_model_buf,
                                   (i32)strlen(sp->api_model_buf),
                                   value_x + 8.0f * dpi, y, cw, ch,
                                   max_chars - 1, value_fg, mc, ms,
                                   false, true);
            } else {
                const char *disp = tc->api_model[0] ? tc->api_model
                                 : custom ? "(tap to set model)" : SETTINGS_MODEL_LBL_DEFAULT;
                bool picker_open = sp->translate_model_picker_open;
                Color chip_bg = picker_open ? control_active : control_bg;
                sp->tr_model_chip_x = value_x;  sp->tr_model_chip_y = y - 4.0f * dpi;
                sp->tr_model_chip_w = chip_w;   sp->tr_model_chip_h = control_h;
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
                const char *chevron = picker_open ? "\xe2\x96\xb4" : "\xe2\x96\xbe";   /* ▴ open, ▾ closed */
                f32 chev_w = (f32)cw;
                f32 chev_x = value_x + chip_w - 10.0f * dpi - chev_w;
                i32 max_chars = (i32)((chev_x - (value_x + 8.0f * dpi)) / cw);
                if (max_chars < 1) max_chars = 1;
                draw_label_limited(r, disp, value_x + 8.0f * dpi, y,
                                   tc->api_model[0] ? value_fg : dim_fg,
                                   max_chars);
                draw_label(r, chevron, chev_x, y, dim_fg);
                sp_push_hover(sp, value_x, y - 4.0f * dpi, chip_w, control_h);
            }
            y += row_h;

            /* Base URL — only for the custom OpenAI-compatible provider. */
            if (custom) {
                draw_label(r, "Base URL:", content_x, y, label_fg);
                Color chip_bg = ed_url ? control_active : control_bg;
                s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w,
                                 control_h, chip_bg, control_border,
                                 fmaxf(1.0f, dpi), radius);
                if (ed_url) {
                    s_rrect_bordered_per_corner(r, value_x,
                        y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
                }
                renderer_flush_rrects(r);
                i32 max_chars = (i32)((chip_w - 16.0f * dpi) / cw);
                if (max_chars < 4) max_chars = 4;
                if (ed_url) {
                    Color uc = {text_fg.r, text_fg.g, text_fg.b,
                                0.8f * ui_caret_alpha()};
                    Color us = {accent.r, accent.g, accent.b, 0.30f};
                    s_edit_field(r, app, sp->api_url_buf,
                                       (i32)strlen(sp->api_url_buf),
                                       value_x + 8.0f * dpi, y, cw, ch,
                                       max_chars - 1, value_fg, uc, us,
                                       false, true);
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
        /* The Model menu takes no place here: it is a popover on its chip,
         * drawn after the whole tab (below, before the body's clip ends). */

        /* Chord window — rounded chip row (outer corners only). */
        draw_label(r, "Shortcut timing:", content_x, y, label_fg);
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

        /* The local GGUF is downloaded on demand and is not bundled. Show
         * live speed, remaining amount, and ETA. Keep the region two rows
         * high so rendering and hit-testing stay aligned; button handlers use
         * the cached rectangles. */
        {
            ModelDownloadStatus md;
            model_download_poll(&md);
            bool downloading = (md.state == MODEL_DL_DOWNLOADING ||
                                md.state == MODEL_DL_VERIFYING);
            /* Fall back to the canonical download location when the setting
             * is unset, exactly as the Voice row does: `translate_local_path`
             * is only written when a file is picked by hand, so a model Liu
             * downloaded itself leaves the setting empty and the row would
             * read "Not downloaded". */
            char tr_defpath[512];
            const char *tr_mpath =
                tc->local_model_path[0]
                    ? tc->local_model_path
                    : liu_model_default_path(tr_defpath, sizeof tr_defpath);
            bool have_model = liu_model_file_ok(tr_mpath);
            f32 bh = control_h;
            f32 by = y + row_h;                 /* action row */

            /* Stale-rect guard: a button not drawn this frame must not stay
             * clickable (w == 0 fails the hit test). */
            sp->tr_model_dl_w = sp->tr_model_cancel_w = sp->tr_model_pick_w = 0.0f;
            sp->tr_model_del_w = 0.0f;

            draw_label(r, "Model:", content_x, y, label_fg);

            if (downloading) {
                f32 pct = (md.bytes_total > 0)
                            ? (f32)md.bytes_done / (f32)md.bytes_total : 0.0f;
                if (md.state == MODEL_DL_VERIFYING) pct = 1.0f;
                if (pct < 0.0f) pct = 0.0f;
                if (pct > 1.0f) pct = 1.0f;

                char st[176];
                if (md.state == MODEL_DL_VERIFYING) {
                    snprintf(st, sizeof st, "Verifying…");
                } else {
                    char spd[28], eta[28];
                    settings_fmt_speed(md.speed_bps, spd, sizeof spd);
                    settings_fmt_eta(md.eta_sec, eta, sizeof eta);
                    f64 left = (f64)md.bytes_total - (f64)md.bytes_done;
                    if (left < 0.0) left = 0.0;
                    snprintf(st, sizeof st,
                             "%d%%  ·  %s  ·  %.0f MB left  ·  %s",
                             (i32)(pct * 100.0f + 0.5f), spd,
                             left / 1048576.0, eta);
                }
                draw_label(r, st, value_x, y, value_fg);

                /* Action row: progress bar + Cancel. */
                f32 cancel_w = settings_button_w("Cancel", cw, dpi);
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
                settings_button(r, sp, "Cancel", cancel_x, by, bh, cw, ch, dpi,
                                control_bg, control_border, value_fg,
                                &sp->tr_model_cancel_x, &sp->tr_model_cancel_y,
                                &sp->tr_model_cancel_w, &sp->tr_model_cancel_h);
            } else {
                if (md.state == MODEL_DL_ERROR) {
                    Color warn = (Color){ .r = 0.90f, .g = 0.46f, .b = 0.33f, .a = 1.0f };
                    i32 mc = (i32)(value_room / cw);
                    if (mc < 1) mc = 1;
                    draw_label_limited(r,
                        md.error[0] ? md.error : "Download failed",
                        value_x, y, warn, mc);
                } else if (have_model) {
                    draw_label(r, "Downloaded ✓", value_x, y, value_fg);
                } else {
                    draw_label(r, "Not downloaded  ·  440 MB", value_x, y, dim_fg);
                }

                const char *primary = (md.state == MODEL_DL_ERROR) ? "Retry"
                                    : have_model ? "Re-download" : "Download";
                f32 bx = value_x;
                bx += settings_button(r, sp, primary, bx, by, bh, cw, ch, dpi,
                                      control_active, control_border, value_fg,
                                      &sp->tr_model_dl_x, &sp->tr_model_dl_y,
                                      &sp->tr_model_dl_w, &sp->tr_model_dl_h);
                bx += 8.0f * dpi;
                bx += settings_button(r, sp, "Choose file", bx, by, bh, cw, ch, dpi,
                                      control_bg, control_border, label_fg,
                                      &sp->tr_model_pick_x, &sp->tr_model_pick_y,
                                      &sp->tr_model_pick_w, &sp->tr_model_pick_h);
                if (have_model && settings_model_is_liu_owned(tr_mpath)) {
                    bx += 8.0f * dpi;
                    settings_delete_button(r, sp, cp, sp->tr_model_del_armed,
                                           bx, by, bh, cw, ch, dpi,
                                           control_bg, control_border, label_fg,
                                           &sp->tr_model_del_x, &sp->tr_model_del_y,
                                           &sp->tr_model_del_w, &sp->tr_model_del_h);
                }
            }
            y += 2.0f * row_h;
        }

        /* —— ALLOWLIST section —— */
        y += 12.0f * dpi;
        draw_label(r, "ACTIVE IN", content_x, y, dim_fg);
        {
            f32 rule_x = content_x + fminf(11.0f * cw, content_w * 0.50f);
            f32 rule_w = content_x + content_w - rule_x;
            if (rule_w > 0.0f) {
                s_rect(r, rule_x, y + ch * 0.65f,
                       rule_w, fmaxf(1.0f, dpi), section_rule);
            }
        }
        y += ch + 12.0f * dpi;

        for (i32 i = 0; i < TRANSLATE_AGENT_ROW_COUNT; i++) {
            bool *flag = translate_agent_flag(tc, i);
            if (!flag) continue;
            draw_label(r, g_translate_agents[i].label, content_x, y, label_fg);
            settings_draw_switch(r, value_x, y - 2.0f * dpi,
                                 *flag, control_active,
                                 control_bg, control_border,
                                 value_fg, label_fg, cw, ch, dpi);
            y += row_h;
        }

        y += 8.0f * dpi;
        draw_label(r, "Type a prompt, press Ctrl+" UI_MOD_PRIMARY " twice to translate.",
                   content_x, y, dim_fg);
        y += row_h;
        draw_label(r, "Changes are saved automatically.",
                   content_x, y, dim_fg);
        content_end_y = y + ch + 16;

    } else if (sp->active_tab == SETTINGS_TAB_VOICE) {
        VoiceConfig *vc = &sp->config->voice;
        f32 y = content_y;

        /* Same geometry constants as the Translate tab so the two read as
         * one settings language; the hit-test below re-derives them. */
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

        draw_label(r, "VOICE INPUT", content_x, y, dim_fg);
        {
            f32 rule_x = content_x + fminf(18.0f * cw, content_w * 0.55f);
            f32 rule_w = content_x + content_w - rule_x;
            if (rule_w > 0.0f)
                s_rect(r, rule_x, y + ch * 0.65f, rule_w, fmaxf(1.0f, dpi), section_rule);
        }
        y += ch + 12.0f * dpi;

        draw_label(r, "Enabled:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi, vc->enabled,
                             control_active, control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* The Local backend runs Whisper on device and keeps audio local. The
         * API backend sends recorded audio to the selected provider and does
         * not require the local model. */
        {
            const char *names[] = { "Local", "API" };
            const i32 n = (i32)(sizeof names / sizeof names[0]);
            i32 sel = (vc->backend == VOICE_BACKEND_API) ? 1 : 0;
            f32 seg_w = 10.0f * cw;
            if (value_room > 0.0f && seg_w * (f32)n > value_room) seg_w = value_room / (f32)n;
            if (seg_w < 1.0f) seg_w = 1.0f;
            draw_label(r, "Run with:", content_x, y, label_fg);
            settings_draw_segmented(r, value_x, y - 4.0f * dpi, names, n, sel,
                                    seg_w, control_h, control_active, control_bg,
                                    control_border, value_fg, value_fg, cw, ch, dpi);
        }
        y += row_h;

        /* —— API provider block ——
         * Rendered only under Backend = API, and the hit-test below skips the
         * same rows under Local — every row here would otherwise be a dead
         * control absorbing clicks meant for the ones beneath it. */
        if (vc->backend == VOICE_BACKEND_API) {
            const bool vo_custom  = strcmp(vc->api_provider, "custom") == 0;
            const bool ed_key   = sp_api_edit_key(sp, SETTINGS_TAB_VOICE);
            const bool ed_url   = sp_api_edit_url(sp, SETTINGS_TAB_VOICE);
            const bool ed_model = sp_api_edit_model(sp, SETTINGS_TAB_VOICE);
            const f32 radius = fminf(control_h * 0.5f, 6.0f * dpi);
            f32 chip_w = value_room;
            if (chip_w < 12.0f * cw) chip_w = 12.0f * cw;

            /* Providers are the speech-to-text ones, NOT the Translate tab's:
             * Anthropic has no transcription API, and ElevenLabs (which has no
             * chat API) does. Same reason each list is built where it is used
             * rather than shared. */
            draw_label(r, "Provider:", content_x, y, label_fg);
            {
                const char *names[] = { "OpenAI", "Groq", "ElevenLabs", "Custom" };
                const char *ids[]   = { "openai", "groq", "elevenlabs", "custom" };
                const i32 n = 4;
                i32 sel = 0;
                for (i32 i = 0; i < n; i++)
                    if (strcmp(vc->api_provider, ids[i]) == 0) { sel = i; break; }
                /* 11 cells: "ElevenLabs" is 10 chars and draw_label does not
                 * clip, so a 9-wide segment would spill into its neighbour. */
                f32 seg_w = 11.0f * cw;
                if (value_room > 0.0f && seg_w * (f32)n > value_room)
                    seg_w = value_room / (f32)n;
                if (seg_w < 1.0f) seg_w = 1.0f;
                settings_draw_segmented(r, value_x, y - 4.0f * dpi, names, n, sel,
                                        seg_w, control_h, control_active, control_bg,
                                        control_border, value_fg, value_fg, cw, ch, dpi);
            }
            y += row_h;

            /* API Key — masked once committed, echoed while editing. */
            draw_label(r, "API Key:", content_x, y, label_fg);
            {
                Color chip_bg = ed_key ? control_active : control_bg;
                s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w, control_h,
                                 chip_bg, control_border, fmaxf(1.0f, dpi), radius);
                if (ed_key) {
                    s_rrect_bordered_per_corner(r, value_x,
                        y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
                }
                renderer_flush_rrects(r);
                i32 max_chars = (i32)((chip_w - 16.0f * dpi) / cw);
                if (max_chars < 4) max_chars = 4;
                if (ed_key) {
                    Color kc = { text_fg.r, text_fg.g, text_fg.b, 0.8f * ui_caret_alpha() };
                    Color ks = { accent.r, accent.g, accent.b, 0.30f };
                    s_edit_field(r, app, sp->api_key_buf,
                                       (i32)strlen(sp->api_key_buf),
                                       value_x + 8.0f * dpi, y, cw, ch,
                                       max_chars - 1, value_fg, kc, ks, true, true);
                } else if (vc->api_key[0]) {
                    usize klen = strlen(vc->api_key);
                    const char *tail = klen > 4 ? vc->api_key + klen - 4 : vc->api_key;
                    char masked[48];
                    snprintf(masked, sizeof masked,
                             "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"
                             "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"
                             "%s", tail);
                    draw_label_limited(r, masked, value_x + 8.0f * dpi, y,
                                       value_fg, max_chars);
                } else {
                    char envkey[256];
                    voice_api_effective_key(vc, envkey, sizeof envkey);
                    draw_label_limited(r, envkey[0] ? "(using env key)" : "(not set)",
                                       value_x + 8.0f * dpi, y, dim_fg, max_chars);
                }
            }
            y += row_h;

            /* Keep the model as free text because transcription model
             * availability is provider-specific and is not returned by
             * `/v1/models`. */
            draw_label(r, "Model:", content_x, y, label_fg);
            {
                Color chip_bg = ed_model ? control_active : control_bg;
                s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w, control_h,
                                 chip_bg, control_border, fmaxf(1.0f, dpi), radius);
                if (ed_model) {
                    s_rrect_bordered_per_corner(r, value_x,
                        y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
                }
                renderer_flush_rrects(r);
                i32 max_chars = (i32)((chip_w - 16.0f * dpi) / cw);
                if (max_chars < 4) max_chars = 4;
                if (ed_model) {
                    Color mc = { text_fg.r, text_fg.g, text_fg.b, 0.8f * ui_caret_alpha() };
                    Color ms = { accent.r, accent.g, accent.b, 0.30f };
                    s_edit_field(r, app, sp->api_model_buf,
                                       (i32)strlen(sp->api_model_buf),
                                       value_x + 8.0f * dpi, y, cw, ch,
                                       max_chars - 1, value_fg, mc, ms, false, true);
                } else {
                    const char *def = voice_api_default_model(vc->api_provider);
                    char shown[80];
                    if (vc->api_model[0])
                        snprintf(shown, sizeof shown, "%s", vc->api_model);
                    else if (def[0])
                        snprintf(shown, sizeof shown, "%s (default)", def);
                    else
                        snprintf(shown, sizeof shown, "(tap to set model)");
                    draw_label_limited(r, shown, value_x + 8.0f * dpi, y,
                                       vc->api_model[0] ? value_fg : dim_fg, max_chars);
                }
            }
            y += row_h;

            if (vo_custom) {
                draw_label(r, "Base URL:", content_x, y, label_fg);
                Color chip_bg = ed_url ? control_active : control_bg;
                s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w, control_h,
                                 chip_bg, control_border, fmaxf(1.0f, dpi), radius);
                if (ed_url) {
                    s_rrect_bordered_per_corner(r, value_x,
                        y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                        chip_w, fmaxf(2.0f, 2.0f * dpi),
                        accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
                }
                renderer_flush_rrects(r);
                i32 max_chars = (i32)((chip_w - 16.0f * dpi) / cw);
                if (max_chars < 4) max_chars = 4;
                if (ed_url) {
                    Color uc = { text_fg.r, text_fg.g, text_fg.b, 0.8f * ui_caret_alpha() };
                    Color us = { accent.r, accent.g, accent.b, 0.30f };
                    s_edit_field(r, app, sp->api_url_buf,
                                       (i32)strlen(sp->api_url_buf),
                                       value_x + 8.0f * dpi, y, cw, ch,
                                       max_chars - 1, value_fg, uc, us, false, true);
                } else {
                    draw_label_limited(r,
                        vc->api_base_url[0] ? vc->api_base_url
                                            : "(e.g. http://localhost:8080/v1)",
                        value_x + 8.0f * dpi, y,
                        vc->api_base_url[0] ? value_fg : dim_fg, max_chars);
                }
                y += row_h;
            }

            {
                i32 max_cols = (i32)(content_w / cw);
                if (max_cols < 1) max_cols = 1;
                draw_label_limited(r, vo_custom
                    ? "Key optional for local servers; stored in config.json."
                    : "Key is stored in config.json; provider env vars also work.",
                    content_x, y, dim_fg, max_cols);
            }
            y += row_h;
        }

        /* Microphone: which input device to record from. "System default" is
         * first and is the default — but it is also the one that moves on its
         * own when headphones or a docked webcam arrive, which is why a
         * device can be pinned at all. */
        {
            /* Refreshed here so the click handler hit-tests the same rows that
             * were painted — see the cache comment in settings.h. */
            settings_voice_mics_refresh(sp, false);

            const f32 radius = fminf(control_h * 0.5f, 6.0f * dpi);
            f32 chip_w = value_room;
            if (chip_w < 12.0f * cw) chip_w = 12.0f * cw;

            const char *shown = "System default";
            bool missing = false;
            if (vc->input_device[0]) {
                shown = NULL;
                for (i32 i = 0; i < sp->voice_mic_count; i++) {
                    if (strcmp(sp->voice_mics[i].uid, vc->input_device) == 0) {
                        shown = sp->voice_mics[i].name; break;
                    }
                }
                /* A pinned device that is unplugged still shows, marked — the
                 * recording silently falls back to the default and the user
                 * needs to be able to tell that from a setting that did not
                 * take. */
                if (!shown) { shown = "Unavailable device"; missing = true; }
            }

            draw_label(r, "Microphone:", content_x, y, label_fg);
            Color chip_bg = sp->voice_mic_picker_open ? control_active : control_bg;
            s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w, control_h,
                             chip_bg, control_border, fmaxf(1.0f, dpi), radius);
            if (sp->voice_mic_picker_open) {
                s_rrect_bordered_per_corner(r, value_x,
                    y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                    chip_w, fmaxf(2.0f, 2.0f * dpi),
                    accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
            }
            renderer_flush_rrects(r);
            const char *chevron = "\xe2\x96\xbe"; /* ▾ */
            f32 chev_x = value_x + chip_w - 10.0f * dpi - (f32)cw;
            i32 max_chars = (i32)((chev_x - (value_x + 8.0f * dpi)) / cw);
            if (max_chars < 1) max_chars = 1;
            Color mic_fg = missing
                ? (Color){ .r = 0.90f, .g = 0.46f, .b = 0.33f, .a = 1.0f }
                : (vc->input_device[0] ? value_fg : dim_fg);
            draw_label_limited(r, shown, value_x + 8.0f * dpi, y, mic_fg, max_chars);
            draw_label(r, chevron, chev_x, y, dim_fg);
        }
        y += row_h;

        if (sp->voice_mic_picker_open) {
            f32 menu_w = value_room;
            if (menu_w < 24.0f * cw) menu_w = 24.0f * cw;
            f32 menu_x = value_x;
            if (menu_x + menu_w > content_x + content_w)
                menu_x = content_x + content_w - menu_w;
            if (menu_x < content_x) menu_x = content_x;
            settings_draw_voice_mic_menu(sp, r, menu_x, y + 4.0f * dpi, menu_w,
                                         cw, ch, dpi, panel_bg, value_fg,
                                         dim_fg, accent);
            y += settings_voice_mic_menu_height(sp, ch, dpi) + 8.0f * dpi;
        }

        /* Language: a chip opening the full picker, since Whisper knows 100
         * languages. Auto leads — it costs one extra decoder step on the
         * first window and is right more often than a wrong pin. */
        {
            const f32 radius = fminf(control_h * 0.5f, 6.0f * dpi);
            f32 chip_w = value_room;
            if (chip_w < 12.0f * cw) chip_w = 12.0f * cw;
            const char *name = vc->lang[0] ? voice_lang_name_for_code(vc->lang)
                                           : NULL;
            char shown[64];
            if (!vc->lang[0])   snprintf(shown, sizeof shown, "Auto");
            else if (name)      snprintf(shown, sizeof shown, "%s", name);
            /* A code config.json carries that this build does not know: show
             * it rather than silently rendering "Auto" over a real setting. */
            else                snprintf(shown, sizeof shown, "%s (unknown)", vc->lang);

            draw_label(r, "Language:", content_x, y, label_fg);
            Color chip_bg = sp->voice_lang_picker_open ? control_active : control_bg;
            s_rrect_bordered(r, value_x, y - 4.0f * dpi, chip_w, control_h,
                             chip_bg, control_border, fmaxf(1.0f, dpi), radius);
            if (sp->voice_lang_picker_open) {
                s_rrect_bordered_per_corner(r, value_x,
                    y - 4.0f * dpi + control_h - fmaxf(2.0f, 2.0f * dpi),
                    chip_w, fmaxf(2.0f, 2.0f * dpi),
                    accent, accent, 0.0f, 0.0f, 0.0f, radius, radius);
            }
            renderer_flush_rrects(r);
            const char *chevron = "\xe2\x96\xbe"; /* ▾ */
            f32 chev_x = value_x + chip_w - 10.0f * dpi - (f32)cw;
            i32 max_chars = (i32)((chev_x - (value_x + 8.0f * dpi)) / cw);
            if (max_chars < 1) max_chars = 1;
            draw_label_limited(r, shown, value_x + 8.0f * dpi, y,
                               vc->lang[0] ? value_fg : dim_fg, max_chars);
            draw_label(r, chevron, chev_x, y, dim_fg);
        }
        y += row_h;

        if (sp->voice_lang_picker_open) {
            f32 menu_w = settings_voice_lang_menu_width(cw, dpi, content_w);
            f32 menu_x = value_x;
            if (menu_x + menu_w > content_x + content_w)
                menu_x = content_x + content_w - menu_w;
            if (menu_x < content_x) menu_x = content_x;
            settings_draw_voice_lang_menu(sp, r, menu_x, y + 4.0f * dpi, menu_w,
                                          cw, ch, dpi, panel_bg, value_fg,
                                          dim_fg, accent);
            y += settings_voice_lang_menu_height(ch, dpi) + 8.0f * dpi;
        }

        /* Labels have to fit the value column (18 cells, shared with every
         * other tab) — draw_label does not clip, so a longer one runs under
         * its own control. */
        draw_label(r, "To English:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi, vc->translate,
                             control_active, control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        {
            /* Say WHICH engine will do it.
             *
             * The bundled large-v3-turbo has no translate task at all — it was
             * distilled on transcription alone and answers a whole sentence
             * with "..." — so a dictation on that model is translated by
             * routing the transcript through Liu's Translate engine instead.
             * That is a second step with its own configuration and its own
             * failure modes, and the user has to know it is happening rather
             * than discover it from a toast. Reads the model header only (a
             * 48-byte fread), so it costs nothing per frame. */
            char defp[512];
            const char *mp = vc->model_path[0] ? vc->model_path
                           : liu_whisper_default_path(defp, sizeof defp);
            if (vc->translate && vc->backend == VOICE_BACKEND_LOCAL &&
                liu_whisper_file_ok(mp) && !liu_whisper_can_translate(mp)) {
                const TranslateConfig *tc2 = &sp->config->translate;
                char note[96];
                char why[96];
                bool bad = false;
                if (tc2->backend == TRANSLATE_BACKEND_AGENT) {
                    snprintf(note, sizeof note, "via Translate (%s)",
                             tc2->agent_id[0] ? tc2->agent_id : "agent");
                } else if (tc2->backend == TRANSLATE_BACKEND_API) {
                    if (!translate_api_ready(tc2, why, sizeof why)) {
                        snprintf(note, sizeof note, "Translate API not set up");
                        bad = true;
                    } else {
                        snprintf(note, sizeof note, "via Translate (%s)",
                                 translate_api_provider_display(tc2->api_provider));
                    }
                } else {
                    snprintf(note, sizeof note, "via Translate (local)");
                }
                Color warn = (Color){ .r = 0.90f, .g = 0.46f, .b = 0.33f, .a = 1.0f };
                i32 mc = (i32)(value_room / cw);
                if (mc < 1) mc = 1;
                draw_label_limited(r, note, value_x + settings_switch_w(cw, dpi) + 2.0f * cw, y,
                                   bad ? warn : dim_fg, mc);
            }
        }
        y += row_h;

        /* Auto-stop after trailing silence. Off means the user ends the
         * recording by hand with the same shortcut. */
        {
            const char *names[] = { "Off", "1.5s", "2s", "3s" };
            const f32 vals[] = { 0.0f, 1.5f, 2.0f, 3.0f };
            const i32 n = (i32)(sizeof names / sizeof names[0]);
            i32 sel = 0;
            for (i32 i = 0; i < n; i++)
                if (fabsf(vc->silence_stop_sec - vals[i]) < 0.05f) sel = i;
            f32 seg_w = 7.0f * cw;
            if (value_room > 0.0f && seg_w * (f32)n > value_room) seg_w = value_room / (f32)n;
            if (seg_w < 1.0f) seg_w = 1.0f;
            draw_label(r, "Stop on silence:", content_x, y, label_fg);
            settings_draw_segmented(r, value_x, y - 4.0f * dpi, names, n, sel,
                                    seg_w, control_h, control_active, control_bg,
                                    control_border, value_fg, value_fg, cw, ch, dpi);
        }
        y += row_h;

        draw_label(r, "Auto-submit:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi, vc->auto_submit,
                             control_active, control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* The two sounds of a dictation: the microphone opening and closing. */
        draw_label(r, "Sounds:", content_x, y, label_fg);
        settings_draw_switch(r, value_x, y - 2.0f * dpi, vc->sounds,
                             control_active, control_bg, control_border,
                             value_fg, label_fg, cw, ch, dpi);
        y += row_h;

        /* —— Model: download-on-demand ——
         * Fixed 2-row region for the same reason as the Translate tab: render
         * and hit-test have to advance in lockstep whatever the state is.
         *
         * Local backend only. The buttons are also the cached-rect kind, so
         * the zeroing below matters: a rect left over from a frame when the
         * backend WAS local would keep a Download button clickable where the
         * API rows now are. */
        if (vc->backend != VOICE_BACKEND_LOCAL) {
            sp->vo_model_dl_w = sp->vo_model_cancel_w = sp->vo_model_pick_w = 0.0f;
            sp->vo_model_del_w = 0.0f;
        } else {
            ModelDownloadStatus md;
            model_download_poll(&md);
            /* The downloader is a single global slot shared with the
             * Translate model, so a transfer only belongs to this row when
             * its destination is ours — otherwise the Translate download
             * would paint its progress here as well. */
            bool mine = (md.dest_path[0] == '\0') ||
                        (strstr(md.dest_path, LIU_WHISPER_FILENAME) != NULL);
            bool downloading = mine && (md.state == MODEL_DL_DOWNLOADING ||
                                        md.state == MODEL_DL_VERIFYING);
            char defpath[512];
            const char *mpath = vc->model_path[0] ? vc->model_path
                                : liu_whisper_default_path(defpath, sizeof defpath);
            bool have_model = liu_whisper_file_ok(mpath);
            f32 bh = control_h;
            f32 by = y + row_h;

            sp->vo_model_dl_w = sp->vo_model_cancel_w = sp->vo_model_pick_w = 0.0f;
            sp->vo_model_del_w = 0.0f;

            draw_label(r, "Model:", content_x, y, label_fg);

            if (downloading) {
                f32 pct = (md.bytes_total > 0)
                            ? (f32)md.bytes_done / (f32)md.bytes_total : 0.0f;
                if (md.state == MODEL_DL_VERIFYING) pct = 1.0f;
                if (pct < 0.0f) pct = 0.0f;
                if (pct > 1.0f) pct = 1.0f;

                char st[176];
                if (md.state == MODEL_DL_VERIFYING) {
                    snprintf(st, sizeof st, "Verifying…");
                } else {
                    char spd[28], eta[28];
                    settings_fmt_speed(md.speed_bps, spd, sizeof spd);
                    settings_fmt_eta(md.eta_sec, eta, sizeof eta);
                    f64 left = (f64)md.bytes_total - (f64)md.bytes_done;
                    if (left < 0.0) left = 0.0;
                    snprintf(st, sizeof st, "%d%%  ·  %s  ·  %.0f MB left  ·  %s",
                             (i32)(pct * 100.0f + 0.5f), spd, left / 1048576.0, eta);
                }
                draw_label(r, st, value_x, y, value_fg);

                f32 cancel_w = settings_button_w("Cancel", cw, dpi);
                f32 cancel_x = value_x + value_room - cancel_w;
                f32 bar_w = cancel_x - 8.0f * dpi - value_x;
                if (bar_w < 4.0f * cw) bar_w = 4.0f * cw;
                f32 bar_h = fmaxf(5.0f, 6.0f * dpi);
                f32 bar_y = by + (bh - bar_h) * 0.5f;
                s_rrect_simple(r, value_x, bar_y, bar_w, bar_h, control_bg, bar_h * 0.5f);
                s_rrect_simple(r, value_x, bar_y, bar_w * pct, bar_h, accent, bar_h * 0.5f);
                renderer_flush_rrects(r);
                settings_button(r, sp, "Cancel", cancel_x, by, bh, cw, ch, dpi,
                                control_bg, control_border, value_fg,
                                &sp->vo_model_cancel_x, &sp->vo_model_cancel_y,
                                &sp->vo_model_cancel_w, &sp->vo_model_cancel_h);
            } else {
                if (mine && md.state == MODEL_DL_ERROR) {
                    Color warn = (Color){ .r = 0.90f, .g = 0.46f, .b = 0.33f, .a = 1.0f };
                    i32 mc = (i32)(value_room / cw);
                    if (mc < 1) mc = 1;
                    draw_label_limited(r, md.error[0] ? md.error : "Download failed",
                                       value_x, y, warn, mc);
                } else if (have_model) {
                    draw_label(r, "Downloaded ✓  ·  large-v3-turbo", value_x, y, value_fg);
                } else {
                    draw_label(r, "Not downloaded  ·  547 MB", value_x, y, dim_fg);
                }

                const char *primary = (mine && md.state == MODEL_DL_ERROR) ? "Retry"
                                    : have_model ? "Re-download" : "Download";
                f32 bx = value_x;
                bx += settings_button(r, sp, primary, bx, by, bh, cw, ch, dpi,
                                      control_active, control_border, value_fg,
                                      &sp->vo_model_dl_x, &sp->vo_model_dl_y,
                                      &sp->vo_model_dl_w, &sp->vo_model_dl_h);
                bx += 8.0f * dpi;
                bx += settings_button(r, sp, "Choose file", bx, by, bh, cw, ch, dpi,
                                      control_bg, control_border, label_fg,
                                      &sp->vo_model_pick_x, &sp->vo_model_pick_y,
                                      &sp->vo_model_pick_w, &sp->vo_model_pick_h);
                /* Delete — only for a model Liu itself downloaded, and only
                 * when one is really on disk. settings_model_is_liu_owned says
                 * why a file the user chose is exempt. */
                if (have_model && settings_model_is_liu_owned(mpath)) {
                    bx += 8.0f * dpi;
                    settings_delete_button(r, sp, cp, sp->vo_model_del_armed,
                                           bx, by, bh, cw, ch, dpi,
                                           control_bg, control_border, label_fg,
                                           &sp->vo_model_del_x, &sp->vo_model_del_y,
                                           &sp->vo_model_del_w, &sp->vo_model_del_h);
                }
            }
            y += 2.0f * row_h;
        }

        /* Footnotes. draw_label_limited, not draw_label: these are the only
         * full-width sentences on the tab, and an unclipped one runs off the
         * panel instead of ending. */
        y += 12.0f * dpi;
        {
            i32 max_cols = (i32)(content_w / cw);
            if (max_cols < 1) max_cols = 1;

            /* Name the shortcut that is actually bound — it is rebindable on
             * the Keys tab. */
            const KeyBinding *kb = sp->keybinds ? keybind_get(sp->keybinds, ACT_VOICE_INPUT) : NULL;
            char hint[160];
            snprintf(hint, sizeof hint, "%s starts and stops dictation.",
                     kb ? keybind_format(kb) : "The Voice Input shortcut");
            draw_label_limited(r, hint, content_x, y, dim_fg, max_cols);
            y += row_h;

            /* Show the data-handling behavior for the selected backend. */
            if (vc->backend == VOICE_BACKEND_API) {
                char note[160];
                snprintf(note, sizeof note,
                         "Recordings are uploaded to %s for transcription.",
                         voice_api_provider_display(vc->api_provider));
                draw_label_limited(r, note, content_x, y, dim_fg, max_cols);
            } else {
                draw_label_limited(r, "Audio and transcription stay on this Mac.",
                                   content_x, y, dim_fg, max_cols);
            }
            y += row_h;

            if (platform_audio_mic_permission() == PLATFORM_MIC_DENIED) {
                Color warn = (Color){ .r = 0.90f, .g = 0.46f, .b = 0.33f, .a = 1.0f };
                draw_label_limited(r, "Microphone access is off for Liu.",
                                   content_x, y, warn, max_cols);
                y += row_h;
            }
        }
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
            /* An empty row is silence unless speech is switched on. */
            const char *disp = cur_path ? path_basename(cur_path)
                             : sp->notify_cfg.tts_fallback ? "(default — TTS)" : "(none)";
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
         * with paths to the wavs shipped in assets/sounds/, so an
         * existing notify.conf can adopt the bundled defaults without
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
         * to the machine. An agent without a hook API Liu can target
         * (agent_hook_supported) gets an inline "No hook API yet" note
         * instead of a button, so the row does not look like a broken
         * button. */
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

        /* The switch for the whole thing. The hooks go into files that belong
         * to other programs — each agent's own settings — so whether Liu may
         * do that when it starts is a setting, stated here in words. */
        draw_label(r, "Install at launch:", content_x, y, label_fg);
        {
            /* The switch's colours, as every other tab derives them. */
            Color sw_bg     = settings_blend(panel_bg, text_fg, cp.is_light ? 0.05f : 0.08f);
            Color sw_active = settings_blend(sw_bg, accent, cp.is_light ? 0.24f : 0.34f);
            Color sw_border = settings_blend(panel_bg, text_fg, cp.is_light ? 0.16f : 0.20f);
            sw_bg.a = 1.0f; sw_active.a = 1.0f; sw_border.a = 0.68f;
            f32 sw_x = content_x + 20.0f * cw;
            settings_draw_switch(r, sw_x, y - 2.0f * dpi, sp->config->agent_hooks,
                                 sw_active, sw_bg, sw_border,
                                 value_fg, label_fg, cw, ch, dpi);
            /* settings_draw_switch registers the hover rect itself. */
            sp->notify_auto_x = sw_x;
            sp->notify_auto_y = y - 2.0f * dpi;
            sp->notify_auto_w = settings_switch_w(cw, dpi);
            sp->notify_auto_h = ch + 6.0f * dpi;
        }
        y += row_h_notify;
        draw_label(r, sp->config->agent_hooks
                       ? "Added to each agent's settings while Liu runs; removed on quit."
                       : "Liu adds no hook by itself. Install one by hand below.",
                   content_x, y, dim_fg);
        y += row_h_notify + 4.0f * dpi;

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

        draw_label(r, sp->notify_cfg.tts_fallback
                       ? "Without a sound file, speech plays."
                       : "Without a sound file, no audio plays.",
                   content_x, y, dim_fg);
        y += row_h_notify;
        draw_label(r, "Changes are saved automatically.",
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
            draw_label(r, "Escape cancels · Delete removes the binding", content_x, y, dim_fg);
            content_end_y = y + ch + 16;
        } else {
            draw_label(r, "CUSTOM SHORTCUTS · click a binding to change it",
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
                    key_str = kb ? (kb->key == KEY_UNKNOWN ? "Menu shortcut" : keybind_format(kb)) : "Unbound";
                } else {
                    name    = kr->label;
                    key_str = kr->keys;
                }

                /* Right-aligned rounded "kbd" pill. Rebindable rows take the
                 * accent tint; read-only rows are muted so they read as
                 * reference rather than something you can click to change. */
                f32 ks_w   = (f32)strlen(key_str) * cw;
                f32 pad_x  = 8.0f * dpi;
                f32 pill_w = fminf(ks_w + 2.0f * pad_x, content_w * 0.55f);
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
                draw_label_limited(r, name, content_x + 4 * dpi, y + text_dy, name_fg,
                                   (i32)(fmaxf(0, pill_x - content_x - 12 * dpi) / cw));
                draw_label_limited(r, key_str, pill_x + pad_x, y + text_dy, key_fg,
                                   (i32)((pill_w - 2 * pad_x) / cw));

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
                                 : initialized ? "Locked" : "Vault not set up";
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
            /* One aggregate per second while visible, rather than one per
             * animation frame. A fresh open always samples immediately;
             * edits from another window appear on the next refresh. */
            f64 stats_now = platform_time_sec();
            if (!sp->vault_stats_valid || stats_now < sp->vault_stats_at ||
                stats_now - sp->vault_stats_at >= 1.0) {
                sp->vault_stats_total = vault_secret_counts(v, sp->vault_stats_counts, 5);
                sp->vault_stats_at = stats_now;
                sp->vault_stats_valid = true;
            }
            const i32 *counts = sp->vault_stats_counts;
            i32 n = sp->vault_stats_total;
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
            const char *l2 = "View secrets";
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
                /* Hand-drawn box (not draw_input_field), so it registers its
                 * own rect for the I-beam. */
                ui_text_area_push(field_x, fy, field_w, field_h);
                if (*editing[i]) {
                    /* Focus ring — single thin pixel along the bottom. */
                    s_rrect_per_corner(r, field_x, fy + field_h - 2.0f * dpi,
                                       field_w, 2.0f * dpi, accent,
                                       0.0f, 0.0f, input_radius, input_radius);
                }
                renderer_flush_rrects(r);
                Color caret = value_fg; caret.a *= ui_caret_alpha();
                Color selected = accent; selected.a = 0.22f;
                s_edit_field(r, app, bufs[i], (i32)strlen(bufs[i]),
                             field_x + 10 * dpi, fy + (field_h - ch) * 0.5f, cw, ch,
                             MAX(1, (i32)((field_w - 20 * dpi) / cw)),
                             value_fg, caret, selected, true, *editing[i]);
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
                ? cp.btn_destructive_fg : value_fg;
            draw_label(r, sp->vault_status, content_x, y, sc);
            y += ch + 4;
        }
        content_end_y = y + ch + 16;

    } else if (sp->active_tab == SETTINGS_TAB_DIAGNOSTICS) {
        /* Problems first, healthy checks collapsed into one line.
         *
         * The panel exists because these faults are invisible from inside Liu,
         * so a wall of green rows would bury the few that matter. Every WARN
         * and FAIL gets its own row with its own path and its own action; the
         * rest are a count. */
        /* When `LIU_DIAG_RUN=1`, run read-only diagnostics on first paint.
         * Repairs still require the Fix button. */
        if (!sp->diag.ran) {
            static i32 auto_run = -1;
            if (auto_run < 0) {
                const char *e = liu_dev_env("LIU_DIAG_RUN");
                auto_run = (e && *e && *e != '0') ? atoi(e) : 0;
            }
            if (auto_run) {
                char nb[1024];
                diag_run(&sp->diag, diag_notify_bin(nb, sizeof nb) ? nb : NULL);
                /* When `LIU_DIAG_RUN=2`, add a synthetic problem row if space
                 * is available. This allows the diagnostic card to display its
                 * Fix action when real diagnostics find no issue. */
                if (auto_run == 2 && sp->diag.count < DIAG_MAX_ITEMS) {
                    DiagItem *d = &sp->diag.items[sp->diag.count++];
                    memset(d, 0, sizeof *d);
                    d->group  = DIAG_GROUP_HOOKS;
                    d->status = DIAG_FAIL;
                    d->fix    = DIAG_FIX_HOOK_REMOVE;
                    snprintf(d->id, sizeof d->id, "demo");
                    snprintf(d->title, sizeof d->title, "Claude Code — notify hook");
                    snprintf(d->detail, sizeof d->detail,
                             "1 recorded command no longer exists. Claude Code prints a hook "
                             "failure every turn, in every terminal \xe2\x80\x94 not just Liu's:\n"
                             "/old/build/Liu.app/Contents/MacOS/liu-notify");
                    diag_recount(&sp->diag);
                }
            }
        }

        f32 y    = content_y;
        f32 pad  = 14.0f * dpi;
        Color accent = cp.btn_primary_bg;
        Color bad    = cp.btn_destructive_fg;
        Color warn   = color_lerp(cp.btn_destructive_fg, cp.btn_primary_bg, 0.45f);

        /* ---- header card: what this is, and the two actions -------------- */
        {
            f32 bh     = settings_action_h(ch, dpi);
            f32 run_w  = settings_action_w((i32)strlen(SETTINGS_LBL_DIAG_RUN), cw, ch, dpi);
            bool can_fix_all = sp->diag.ran && sp->diag.n_fixable > 0;
            f32 fix_w  = can_fix_all
                       ? settings_action_w((i32)strlen(SETTINGS_LBL_DIAG_FIXALL), cw, ch, dpi)
                       : 0.0f;
            f32 head_h = pad + ch + 6.0f * dpi + ch + 10.0f * dpi + bh + pad;
            about_card_bg(r, content_x, y, content_w, head_h, &cp, dpi);

            f32 tx = content_x + pad;
            i32 maxc = (i32)((content_w - 2.0f * pad) / cw);
            if (!sp->diag.ran) {
                draw_label_limited(r, "Nothing has been checked yet.",
                                   tx, y + pad, value_fg, maxc);
                draw_label_limited(r,
                    "Check agent integrations and saved sessions. Checks do not change files.",
                    tx, y + pad + ch + 6.0f * dpi, label_fg, maxc);
            } else {
                char sum[192];
                snprintf(sum, sizeof sum,
                         "%d problem%s, %d warning%s, %d healthy  \xc2\xb7  checked at %s",
                         sp->diag.n_fail, sp->diag.n_fail == 1 ? "" : "s",
                         sp->diag.n_warn, sp->diag.n_warn == 1 ? "" : "s",
                         sp->diag.n_ok + sp->diag.n_info, sp->diag.ran_at);
                Color sc = sp->diag.n_fail ? bad : (sp->diag.n_warn ? warn : accent);
                draw_label_limited(r, sum, tx, y + pad, sc, maxc);

                const char *sub =
                    sp->diag.n_fail || sp->diag.n_warn
                      ? (sp->diag.n_fixable > 0
                           ? "Available fixes are listed with each issue."
                           : "Liu cannot repair these from here — each row says what to do.")
                      : "Nothing to do.";
                draw_label_limited(r, sub, tx, y + pad + ch + 6.0f * dpi, label_fg, maxc);
            }
            renderer_flush_glyphs(r);

            f32 by = y + pad + ch + 6.0f * dpi + ch + 10.0f * dpi;
            SettingsActionBtn run = settings_action_btn(tx, by, run_w,
                                        (i32)strlen(SETTINGS_LBL_DIAG_RUN), cw, ch, dpi);
            s_action_button(r, sp, &run, ICON_REFRESH, SETTINGS_LBL_DIAG_RUN,
                            &cp, dim_fg, true, dpi);
            if (can_fix_all) {
                SettingsActionBtn fx = settings_action_btn(
                        tx + run_w + settings_action_gap(dpi), by, fix_w,
                        (i32)strlen(SETTINGS_LBL_DIAG_FIXALL), cw, ch, dpi);
                s_action_button(r, sp, &fx, ICON_CHECK, SETTINGS_LBL_DIAG_FIXALL,
                                &cp, dim_fg, false, dpi);
            }
            /* Result of the last Fix All, next to the buttons that caused it. */
            if (sp->diag_last_fixed >= 0) {
                char fm[128];
                snprintf(fm, sizeof fm, "%d fixed, %d could not be",
                         sp->diag_last_fixed, sp->diag_last_failed);
                draw_label_limited(r, fm,
                                   tx + run_w + settings_action_gap(dpi) + fix_w
                                     + (can_fix_all ? 12.0f * dpi : 0.0f),
                                   by + (bh - ch) * 0.5f,
                                   sp->diag_last_failed ? warn : accent, 40);
                renderer_flush_glyphs(r);
            }
            y += head_h + 10.0f * dpi;
        }

        if (sp->diag.ran) {
            DiagGroup shown_group = (DiagGroup)-1;
            for (i32 i = 0; i < sp->diag.count; i++) {
                DiagItem *it = &sp->diag.items[i];
                if (!diag_row_visible(it)) continue;

                if (it->group != shown_group) {
                    shown_group = it->group;
                    draw_label_limited(r, diag_group_label(shown_group),
                                       content_x + 2.0f * dpi, y, dim_fg,
                                       (i32)(content_w / cw));
                    renderer_flush_glyphs(r);
                    y += ch + 6.0f * dpi;
                }

                DiagRowGeom g = diag_row_geom(it, content_x, y, content_w, cw, ch, dpi);
                Color tone = (it->status == DIAG_FAIL) ? bad : warn;

                /* The card, tinted by severity so the list reads at a glance. */
                Color fill = tone; fill.a = 0.10f;
                s_rrect_bordered(r, content_x, g.y, content_w, g.h, fill,
                                 (Color){tone.r, tone.g, tone.b, 0.45f},
                                 fmaxf(1.0f, dpi), 10.0f * dpi);
                renderer_flush_rrects(r);

                /* Status dot. A filled disc for a problem, a ring for a warning:
                 * severity has to survive a monochrome theme, where the two
                 * tones can land close together. */
                f32 dsz = ch * 0.5f;
                f32 dx  = content_x + 12.0f * dpi + (ch - dsz) * 0.5f;
                f32 dy  = g.y + 12.0f * dpi + (ch - dsz) * 0.5f;
                if (it->status == DIAG_FAIL)
                    s_rrect_simple(r, dx, dy, dsz, dsz, tone, dsz * 0.5f);
                else
                    s_rrect_bordered(r, dx, dy, dsz, dsz, (Color){0, 0, 0, 0},
                                     tone, fmaxf(1.5f, 1.5f * dpi), dsz * 0.5f);
                renderer_flush_rrects(r);

                f32 ty = g.y + 12.0f * dpi;
                draw_label_limited(r, it->title, g.text_x, ty, value_fg, g.text_cells);
                ty += ch + 4.0f * dpi;

                char body[DIAG_BODY_CAP];
                diag_row_body(it, body, sizeof body);
                i32 st[DIAG_ROW_MAX_LINES], ln[DIAG_ROW_MAX_LINES];
                i32 nl = diag_wrap(body, g.text_cells, DIAG_ROW_MAX_LINES, st, ln);
                if (nl > DIAG_ROW_MAX_LINES) nl = DIAG_ROW_MAX_LINES;
                for (i32 k = 0; k < nl; k++) {
                    char line[DIAG_BODY_CAP];
                    i32 len = ln[k];
                    if (len < 0) len = 0;
                    if (len > (i32)sizeof line - 1) len = (i32)sizeof line - 1;
                    memcpy(line, body + st[k], (usize)len);
                    line[len] = '\0';
                    /* The remedy line is the one the user has to act on, so it
                     * keeps full contrast while the finding stays quiet. */
                    Color lc = (line[0] == '\xe2') ? value_fg : label_fg;
                    draw_label_limited(r, line, g.text_x, ty, lc, g.text_cells);
                    ty += ch + 2.0f * dpi;
                }
                renderer_flush_glyphs(r);

                if (g.has_fix)
                    s_action_button(r, sp, &g.fix, ICON_CHECK,
                                    diag_fix_label(it->fix), &cp, dim_fg, false, dpi);

                y += g.h + 8.0f * dpi;
            }

            /* Everything that is fine, in one line rather than N rows. */
            i32 healthy = sp->diag.n_ok + sp->diag.n_info;
            if (healthy > 0) {
                char hm[160];
                snprintf(hm, sizeof hm,
                         "%d other check%s passed \xe2\x80\x94 hooks, session stores and Liu's own files.",
                         healthy, healthy == 1 ? "" : "s");
                draw_label_limited(r, hm, content_x + 2.0f * dpi, y + 4.0f * dpi,
                                   dim_fg, (i32)(content_w / cw));
                renderer_flush_glyphs(r);
                y += ch + 12.0f * dpi;
            }
        }
        content_end_y = y;
    } else if (sp->active_tab == SETTINGS_TAB_ABOUT) {
        /* Four cards — identity, updates, configuration, vault — then a centred
         * signature. Card geometry is shared: `pad` is the inset on every side,
         * body text hangs off the icon column, and each section's actions sit on
         * the title row when the panel is wide enough to hold them there. */
        f32 y = content_y;
        Color accent   = cp.btn_primary_bg;
        Color path_fg  = label_fg; path_fg.a *= 0.75f;
        /* Quiet outline shared by every non-primary About button — same fill
         * s_action_button gives its secondary, so the two are one object. */
        Color quiet_bg = cp.surface_sunken; quiet_bg.a = 0.45f;
        f32 pad        = 14.0f * dpi;
        f32 card_gap   = 10.0f * dpi;
        /* Same control height the Terminal tab's segmented controls and chips
         * use, so an About button is that object rather than a near-miss. */
        f32 btn_h      = ch + 8.0f * dpi;
        f32 btn_gap    = 8.0f * dpi;
        f32 body_gap   = 6.0f * dpi;
        f32 inner_x    = content_x + pad;
        f32 inner_w    = content_w - 2.0f * pad;
        f32 body_x     = inner_x + ch + 10.0f * dpi;   /* past the icon column */
        f32 body_w     = content_x + content_w - pad - body_x;

        /* ---- Identity ---------------------------------------------------- */
        {
            f32 mark    = 52.0f * dpi;
            f32 vh      = ch + 8.0f * dpi;
            /* Card height follows the TALLEST column, not the mark: with a build
             * hash present the version stack is taller, and sizing off the mark
             * pushed the hash out through the card's bottom edge. */
            f32 stack_h = LIU_BUILD[0] ? vh + ch + 4.0f * dpi : vh;
            f32 row_h_id = fmaxf(mark, stack_h);
            f32 card_h  = row_h_id + 2.0f * pad;
            about_card_bg(r, content_x, y, content_w, card_h, &cp, dpi);

            /* The real app mark, at exactly its drawn pixel size (the source is
             * 1024² and the GPU path has no mipmaps, so anything else shimmers)
             * with squircle corners already masked in by liu_app_icon_rgba.
             *
             * The artwork is a near-black plate, which on a dark theme is the
             * card's own colour, so it gets a hairline ring ON TOP: the icon is
             * opaque, and a ring drawn under it would simply be covered. */
            f32 mx = inner_x, my = y + pad + (row_h_id - mark) * 0.5f;
            i32 mpx = (i32)(mark + 0.5f);
            i32 aw = 0, ahh = 0;
            const u8 *apx = liu_app_icon_rgba(mpx, &aw, &ahh);
            if (apx) {
                /* Stable anchor as the GPU cache key, never the pixel pointer:
                 * a freed-and-reused rasteriser buffer once aliased a stale
                 * texture and handed back the wrong icon (see icon.c). The
                 * generation is the size, so a DPI change re-uploads. */
                static const char app_mark_key[1];
                /* The tinted entry point with a white tint is the same draw
                 * with one difference that matters here: tint.a scales the
                 * image's coverage, so the app mark leaves on the panel's
                 * curve instead of sitting at full opacity through the close.
                 * See the poison block near draw_label. */
                renderer_draw_image_tinted(r, app_mark_key, (u64)mpx,
                                           apx, aw, ahh, mx, my, mark, mark,
                                           s_tint((Color){1, 1, 1, 1}));
                Color ring = cp.divider_subtle; ring.a *= 0.9f;
                s_rrect_bordered(r, mx, my, mark, mark, (Color){0, 0, 0, 0},
                                 ring, fmaxf(1.0f, dpi), mark * 0.224f);
                renderer_flush_rrects(r);
            } else {
                /* Asset missing from this build — keep a mark rather than a
                 * hole, using the same accent tile the tab shipped with. */
                s_rrect_simple(r, mx, my, mark, mark, accent, 12.0f * dpi);
                renderer_flush_rrects(r);
                f32 gsz = mark * 0.52f;
                s_icon(r, ICON_FT_TERMINAL, mx + (mark - gsz) * 0.5f,
                          my + (mark - gsz) * 0.5f, gsz, chrome_legible_on(accent));
            }

            /* Version chip is laid out first: the name/tagline block is
             * truncated against its left edge, so a narrow panel loses tagline
             * characters rather than overlapping two live elements. */
            f32 vw = (f32)strlen(LIU_VERSION) * cw + 16.0f * dpi;
            f32 vx = content_x + content_w - pad - vw;
            f32 vy = y + pad + (row_h_id - stack_h) * 0.5f;
            Color chip = accent; chip.a = 0.16f;
            s_rrect_bordered(r, vx, vy, vw, vh, chip, accent, fmaxf(1.0f, dpi),
                             fminf(vh * 0.5f, 8.0f * dpi));
            renderer_flush_rrects(r);
            draw_label(r, LIU_VERSION, vx + 8.0f * dpi,
                       vy + (vh - ch) * 0.5f, accent);
            if (LIU_BUILD[0]) {
                f32 hw = (f32)strlen(LIU_BUILD) * cw;
                draw_label(r, LIU_BUILD,
                           content_x + content_w - pad - hw,
                           vy + vh + 4.0f * dpi, dim_fg);
            }

            f32 tx = mx + mark + 12.0f * dpi;
            f32 block_h = ch * 2.0f + 4.0f * dpi;
            f32 ty = my + (mark - block_h) * 0.5f;
            i32 name_max = (i32)((vx - 10.0f * dpi - tx) / cw);
            if (name_max < 3) name_max = 3;
            draw_label_limited(r, "Liu", tx, ty, value_fg, name_max);
            /* Says what Liu is FOR, not just what it is built as. "native
             * terminal" alone described the implementation and left the About
             * card silent on the thing the product is actually organised
             * around. Kept to one short line: it is truncated against the
             * version chip's left edge, so a narrow panel eats characters from
             * the end (~60 cells at the full panel width, ~27 at the smallest
             * usable one). */
            draw_label_limited(r, "native terminal for AI agents",
                               tx, ty + ch + 4.0f * dpi, dim_fg, name_max);
            y += card_h + card_gap;
        }

        /* ---- Updates ------------------------------------------------------
         * One primary button whose label + action follow the mirrored updater
         * phase, a status/error line, a progress track while downloading, and a
         * Release Notes link when one is offered. */
        {
            const char *upd_lbl;
            bool upd_disabled = false;
            switch (sp->update_phase) {
                case UPD_AVAILABLE:
                    upd_lbl = sp->update_auto_install ? "Install & Relaunch"
                                                      : "Open Releases Page";
                    break;
                case UPD_CHECKING:    upd_lbl = "Checking\xe2\x80\xa6";    upd_disabled = true; break;
                case UPD_DOWNLOADING: upd_lbl = "Downloading\xe2\x80\xa6"; upd_disabled = true; break;
                case UPD_VERIFYING:   upd_lbl = "Verifying\xe2\x80\xa6";   upd_disabled = true; break;
                case UPD_INSTALLING:
                case UPD_RELAUNCHING: upd_lbl = "Installing\xe2\x80\xa6";  upd_disabled = true; break;
                default:              upd_lbl = "Check for Updates";       break;
            }

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
            } else {
                snprintf(line, sizeof line, "You are on %s.", LIU_VERSION);
            }

            bool has_notes = (sp->update_phase == UPD_AVAILABLE && sp->update_notes[0]);
            bool has_bar   = (sp->update_phase == UPD_DOWNLOADING &&
                              sp->update_bytes_total > 0);

            f32 upd_w = settings_button_w(upd_lbl, cw, dpi);
            /* Keep the button on the title row only while the status line still
             * has a readable column left over; otherwise stack it. */
            bool inline_btn = (inner_w - upd_w - btn_gap) >= 18.0f * cw;

            f32 head_h = inline_btn ? fmaxf(ch, btn_h) : ch;
            f32 card_h = 2.0f * pad + head_h + body_gap + ch;
            if (has_bar)    card_h += body_gap + 6.0f * dpi;
            if (has_notes)  card_h += body_gap + ch;
            if (!inline_btn) card_h += body_gap + btn_h;
            about_card_bg(r, content_x, y, content_w, card_h, &cp, dpi);

            f32 cy = y + pad;
            about_card_head(r, ICON_REFRESH, "Updates", inner_x,
                            cy + (head_h - ch) * 0.5f, ch, dpi, accent, value_fg);

            /* The one primary action on the tab, so it is the solid accent —
             * the same primary/secondary split s_action_button documents. Every
             * other About button gets the quiet outline below, so the grey
             * config pills never read as the heavier controls. While the
             * updater is busy the button drops to a sunken, dimmed state so
             * "disabled" is not just a colour swap. */
            Color ubg = upd_disabled ? cp.surface_sunken : accent;
            Color ufg = upd_disabled ? dim_fg            : chrome_legible_on(accent);
            Color ubd = upd_disabled ? cp.divider_subtle : accent;
            if (inline_btn) {
                settings_button(r, upd_disabled ? NULL : sp, upd_lbl,
                                content_x + content_w - pad - upd_w,
                                cy + (head_h - btn_h) * 0.5f, btn_h, cw, ch, dpi,
                                ubg, ubd, ufg,
                                &sp->update_check_x, &sp->update_check_y,
                                &sp->update_check_w, &sp->update_check_h);
            }
            cy += head_h + body_gap;

            draw_label_limited(r, line, body_x, cy, line_fg,
                               (i32)(body_w / cw));
            cy += ch;

            if (has_bar) {
                cy += body_gap;
                f32 trk_h = 6.0f * dpi;
                f32 frac  = (f32)((f64)sp->update_bytes_done /
                                  (f64)sp->update_bytes_total);
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                Color trk = cp.divider_subtle; trk.a *= 0.8f;
                s_rrect_simple(r, body_x, cy, body_w, trk_h, trk, trk_h * 0.5f);
                if (frac > 0.0f)
                    s_rrect_simple(r, body_x, cy, fmaxf(trk_h, body_w * frac),
                                   trk_h, accent, trk_h * 0.5f);
                renderer_flush_rrects(r);
                cy += trk_h;
            }

            sp->update_notes_w = 0.0f;   /* set only while a link is shown */
            if (has_notes) {
                cy += body_gap;
                const char *nl = "Release Notes";
                f32 nw = (f32)strlen(nl) * cw;
                draw_label(r, nl, body_x, cy, accent);
                /* Underline the link so it is not colour-only — the accent on a
                 * dark surface is not a reliable affordance on its own. */
                Color ul = accent; ul.a *= 0.55f;
                s_rect(r, body_x, cy + ch - 1.0f * dpi, nw,
                                   fmaxf(1.0f, dpi), ul);
                renderer_flush_rects(r);
                s_icon(r, ICON_ARROW_UP_RIGHT, body_x + nw + 4.0f * dpi,
                          cy, ch * 0.8f, accent);
                sp->update_notes_x = body_x; sp->update_notes_y = cy;
                sp->update_notes_w = nw;     sp->update_notes_h = ch;
                sp_push_hover(sp, body_x, cy, nw, ch);
                cy += ch;
            }

            if (!inline_btn) {
                cy += body_gap;
                settings_button(r, upd_disabled ? NULL : sp, upd_lbl, body_x, cy, btn_h, cw, ch, dpi,
                                ubg, ubd, ufg,
                                &sp->update_check_x, &sp->update_check_y,
                                &sp->update_check_w, &sp->update_check_h);
            }
            if (upd_disabled) sp->update_check_w = 0;
            y += card_h + card_gap;
        }

        /* ---- Configuration ------------------------------------------------
         * Both labels say what the button does (SETTINGS_LBL_CFG_SAVE writes
         * the file, SETTINGS_LBL_CFG_OPEN opens it), and the path they act on
         * is shown above them. */
        {
            f32 card_h = 2.0f * pad + ch + body_gap + ch + body_gap + btn_h;
            about_card_bg(r, content_x, y, content_w, card_h, &cp, dpi);

            f32 cy = y + pad;
            about_card_head(r, ICON_GEAR, "Configuration", inner_x, cy, ch, dpi,
                            accent, value_fg);
            cy += ch + body_gap;

            char cfg_shown[1100];
            about_tilde_path(config_file_path(), cfg_shown, sizeof cfg_shown);
            draw_label_limited(r, cfg_shown, body_x, cy, path_fg,
                               (i32)(body_w / cw));
            cy += ch + body_gap;

            f32 sx = settings_button(r, sp, SETTINGS_LBL_CFG_SAVE,
                                     body_x, cy, btn_h, cw, ch, dpi,
                                     quiet_bg, cp.divider_subtle,
                                     cp.btn_secondary_fg,
                                     &sp->about_export_x, &sp->about_export_y,
                                     &sp->about_export_w, &sp->about_export_h);
            settings_button(r, sp, SETTINGS_LBL_CFG_OPEN,
                            body_x + sx + btn_gap, cy, btn_h, cw, ch, dpi,
                            quiet_bg, cp.divider_subtle,
                            cp.btn_secondary_fg,
                            &sp->about_edit_x, &sp->about_edit_y,
                            &sp->about_edit_w, &sp->about_edit_h);
            y += card_h + card_gap;
        }

        /* ---- Notes Vault --------------------------------------------------
         * Where Open Note / Graph / AI-generated docs live. */
        {
            f32 card_h = 2.0f * pad + ch + body_gap + ch + body_gap + btn_h;
            about_card_bg(r, content_x, y, content_w, card_h, &cp, dpi);

            f32 cy = y + pad;
            about_card_head(r, ICON_FT_FOLDER, "Notes Vault", inner_x, cy, ch, dpi,
                            accent, value_fg);
            cy += ch + body_gap;

            char shown[1100];
            const char *vp = sp->config->notes_vault_path;
            if (vp && vp[0])
                about_tilde_path(vp, shown, sizeof shown);
            else
                snprintf(shown, sizeof shown,
                         "~/" LIU_APP_SUPPORT_SUBDIR "/Vault  (default)");
            draw_label_limited(r, shown, body_x, cy, path_fg,
                               (i32)(body_w / cw));
            cy += ch + body_gap;

            settings_button(r, sp, "Change Folder\xe2\x80\xa6",
                            body_x, cy, btn_h, cw, ch, dpi,
                            quiet_bg, cp.divider_subtle,
                            cp.btn_secondary_fg,
                            &sp->vault_btn_x, &sp->vault_btn_y,
                            &sp->vault_btn_w, &sp->vault_btn_h);
            y += card_h + card_gap;
        }

        /* ---- Signature ----------------------------------------------------
         * Centred under the stack rather than hanging off the left margin,
         * where it read as one more setting. The brand is the link (rect cached
         * for the About click handler in settings_handle_click). */
        y += 10.0f * dpi;
        settings_section_rule(r, content_x + content_w * 0.25f, y,
                              content_w * 0.5f, cp.divider_subtle, dpi);
        y += 14.0f * dpi;

        {
            Color made_fg = label_fg; made_fg.a *= 0.65f;
            const char *made_prefix = "made by ";
            const char *made_brand  = "calculus.team";
            f32 pw = (f32)strlen(made_prefix) * cw;
            f32 bwid = (f32)strlen(made_brand) * cw;
            f32 arrow = ch * 0.8f;
            f32 total = pw + bwid + 4.0f * dpi + arrow;
            f32 mx = content_x + (content_w - total) * 0.5f;
            if (mx < content_x) mx = content_x;

            draw_label(r, made_prefix, mx, y, made_fg);
            f32 brand_x = mx + pw;
            draw_label(r, made_brand, brand_x, y, accent);
            Color ul = accent; ul.a *= 0.5f;
            s_rect(r, brand_x, y + ch - 1.0f * dpi, bwid,
                               fmaxf(1.0f, dpi), ul);
            renderer_flush_rects(r);
            s_icon(r, ICON_ARROW_UP_RIGHT, brand_x + bwid + 4.0f * dpi, y,
                      arrow, accent);
            sp->about_made_x = brand_x;  sp->about_made_y = y;
            sp->about_made_w = bwid;     sp->about_made_h = ch;
            sp_push_hover(sp, brand_x, y, bwid, ch);
            y += ch;
        }

        content_end_y = y + 16;
    }

    /* Recompute max_scroll from actual content height vs visible area */
    f32 actual_h    = content_end_y - content_y;
    f32 available_h = (clip_bottom - clip_top) - 16.0f;
    {
        i32 new_max = actual_h > available_h
            ? (i32)((actual_h - available_h) / (ch * 0.5f)) + 1
            : 0;
        sp->max_scroll = new_max;
        if (sp->scroll_y > sp->max_scroll) sp->scroll_y = sp->max_scroll;
    }

    /* The Translate Model menu: LAST, so it is over every row of the tab, and
     * inside the body's clip, which is the band it was placed in. A menu whose
     * chip this frame did not draw (the backend went Local, the row scrolled
     * away) has nothing to hang from and closes. */
    if (sp->active_tab == SETTINGS_TAB_TRANSLATE && sp->translate_model_picker_open) {
        SettingsModelRows rows;
        settings_translate_model_rows(sp, &rows);
        if (sp->tr_model_chip_w <= 0.0f || rows.total < 1) {
            settings_model_menu_close(sp);
        } else {
            SettingsRect chip = {sp->tr_model_chip_x, sp->tr_model_chip_y,
                                 sp->tr_model_chip_w, sp->tr_model_chip_h};
            SettingsModelMenu menu = settings_model_menu(chip, clip_top, clip_bottom, content_x, content_w,
                                                         rows.pinned, rows.n - (rows.pinned ? 1 : 0), dpi);
            settings_draw_translate_model_menu(sp, app, r, theme, &menu, &rows, accent, cw, ch, dpi);
        }
    }

    sp->clip_active = false;
    renderer_pop_scissor(r);

    /* ---- Body scrollbar -------------------------------------------------
     * Drawn AFTER the pop: it is chrome pinned to the panel, not part of the
     * scrolled body, so it must not travel with the content (and must not be
     * clipped by the body band). Same auto-hide rule as the file-browser
     * sidebar — present while the pointer is over the body or the body just
     * moved, then it fades out. */
    sp->sb_track_w = 0.0f;   /* republished below; 0 = no bar this frame */
    if (sp->max_scroll > 0 && actual_h > 1.0f) {
        f32 bar_w = 4.0f * dpi;
        f32 track_x = px + panel_w - 10.0f * dpi - bar_w;
        f32 track_y = clip_top + 6.0f * dpi;
        /* Stop short of the panel's rounded bottom corner (12 dpi radius). */
        f32 track_h = (clip_bottom - 10.0f * dpi) - track_y;
        if (track_h > 0.0f) {
            f32 thumb_h = track_h * (available_h / actual_h);
            if (thumb_h < 28.0f * dpi) thumb_h = 28.0f * dpi;
            if (thumb_h > track_h)     thumb_h = track_h;
            f32 frac = (f32)sp->scroll_y / (f32)sp->max_scroll;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            f32 thumb_y = track_y + (track_h - thumb_h) * frac;

            /* Published for the click hit-test and the drag. */
            sp->sb_track_x = track_x;  sp->sb_track_y = track_y;
            sp->sb_track_w = bar_w;    sp->sb_track_h = track_h;
            sp->sb_thumb_h = thumb_h;

            /* Activity: pointer anywhere over the panel (the nav column reads
             * as part of the same surface — hiding the bar while the user is
             * picking a section would just make it flicker), a scroll offset
             * that moved since last frame, or an in-progress thumb drag. */
            bool over_panel = sp->ptr_x >= px && sp->ptr_x <= px + panel_w &&
                              sp->ptr_y >= py && sp->ptr_y <= py + panel_h;
            if (sp->scroll_y != sp->sb_seen_scroll) {
                sp->sb_seen_scroll = sp->scroll_y;
                sp->sb_active_t    = platform_time_sec();
            }
            bool live = over_panel || sp->sb_dragging;
            if (live) sp->sb_active_t = platform_time_sec();
            f32 sb_alpha = ui_autohide_alpha(sp->sb_active_t, live);

            if (sb_alpha > 0.004f) {
                bool hot = sp->sb_dragging ||
                           (sp->ptr_x >= track_x - 6.0f * dpi &&
                            sp->ptr_x <= track_x + bar_w + 6.0f * dpi &&
                            sp->ptr_y >= track_y &&
                            sp->ptr_y <= track_y + track_h);
                renderer_flush_rects(r);
                Color track_c = text_dim;
                track_c.a = 0.14f * sb_alpha;
                Color thumb_c = text_dim;
                thumb_c.a = (hot ? 0.78f : 0.48f) * sb_alpha;
                s_rrect_simple(r, track_x, track_y, bar_w, track_h,
                               track_c, bar_w * 0.5f);
                s_rrect_simple(r, track_x, thumb_y, bar_w, thumb_h,
                               thumb_c, bar_w * 0.5f);
                renderer_flush_rrects(r);
            }
        }
    }
    if (sp->sb_track_w <= 0.0f) sp->sb_dragging = false;

    renderer_reset_ui_scale(r);
    s_render_panel = NULL;
}

#undef renderer_flush_rects
#undef renderer_flush_rrects

static void settings_scrollbar_scrub(SettingsPanel *sp, f32 y);

static bool settings_handle_click_impl(SettingsPanel *sp, f32 x, f32 y, f32 sw, f32 sh,
                            f32 dpi, f32 cw, f32 ch) {
    if (!sp->open) return false;

    cw = 8.0f * dpi;
    ch = 16.0f * dpi;
    SettingsLayout sl = settings_layout_for(sw, sh, dpi, 1.0f, 0.0f);
    f32 panel_w = sl.panel_w;
    f32 panel_h = sl.panel_h;
    f32 px = sl.px;
    f32 py = sl.py;

    /* An armed Delete survives exactly one click — its own. Snapshot the flags
     * and clear them here, at the top, so every other control in the panel
     * disarms it just by being clicked, and the two handlers far below read
     * the snapshot rather than the field. Doing it the other way round (clear
     * after the hit tests) needs every early `return true` in between to
     * remember, and there are dozens. */
    const bool vo_del_was_armed = sp->vo_model_del_armed;
    const bool tr_del_was_armed = sp->tr_model_del_armed;
    sp->vo_model_del_armed = false;
    sp->tr_model_del_armed = false;

    /* Outside panel = close */
    if (x < px || x > px + panel_w || y < py || y > py + panel_h) {
        settings_close(sp);
        return true;
    }

    /* Body scrollbar — tested before any content control because the bar is
     * an overlay drawn on top of them. Only live when the last render actually
     * published a track (sb_track_w > 0, i.e. the body overflows). Clicking
     * the thumb grabs it; clicking the empty track pages the thumb to the
     * pointer and grabs it from the middle, so the press-and-drag gesture is
     * continuous either way. */
    bool picker_open = sp->translate_model_picker_open ||
                       sp->translate_lang_picker != SETTINGS_LANG_PICKER_CLOSED;
    if (!picker_open && sp->sb_track_w > 0.0f && sp->max_scroll > 0) {
        f32 grab_pad = 6.0f * dpi;   /* the bar is thin — widen the target */
        if (x >= sp->sb_track_x - grab_pad &&
            x <= sp->sb_track_x + sp->sb_track_w + grab_pad &&
            y >= sp->sb_track_y && y <= sp->sb_track_y + sp->sb_track_h) {
            f32 travel  = sp->sb_track_h - sp->sb_thumb_h;
            f32 frac    = (f32)sp->scroll_y / (f32)sp->max_scroll;
            f32 thumb_y = sp->sb_track_y + (travel > 0.0f ? travel * frac : 0.0f);
            bool on_thumb = (y >= thumb_y && y <= thumb_y + sp->sb_thumb_h);
            sp->sb_drag_grab = on_thumb ? (y - thumb_y) : sp->sb_thumb_h * 0.5f;
            sp->sb_dragging  = true;
            sp->sb_active_t  = platform_time_sec();
            if (!on_thumb) settings_scrollbar_scrub(sp, y);
            return true;
        }
    }

    if (settings_rect_contains(sl.close, x, y)) {
        settings_close(sp);
        return true;
    }
    if (x >= px && x < px + sl.nav_w && y >= sl.nav_top && y < sl.nav_bottom) {
        for (i32 clicked = 0; clicked < SETTINGS_TAB_COUNT; clicked++) {
            SettingsRect row = settings_nav_row(sl, clicked, sp->nav_scroll, dpi);
            if (!settings_rect_contains(row, x, y)) continue;
            settings_select_tab(sp, settings_nav_order[clicked]);
            return true;
        }
        return true;
    }

    /* Reset to Defaults — same rect the renderer uses at the foot of the nav
     * column. Two clicks: arm, then commit. Anything else that gets clicked
     * disarms it (below), so an armed button can't be triggered by accident
     * minutes later. */
    {
        if (settings_rect_contains(sl.reset, x, y)) {
            if (!sp->reset_armed) {
                sp->reset_armed = true;
            } else {
                sp->reset_armed = false;
                /* Preferences go back to their defaults; what the user HAS
                 * stays: the vault/notes location, the API keys and endpoints,
                 * the models downloaded or picked, the Site Manager's list,
                 * the terminal profiles, the browser's allowed sites and
                 * Recents, and the hooks notice already shown. */
                const AppConfig *old = sp->config;
                AppConfig fresh = config_default();
                snprintf(fresh.notes_vault_path, sizeof fresh.notes_vault_path,
                         "%s", old->notes_vault_path);
                memcpy(fresh.translate.api_key, old->translate.api_key, sizeof fresh.translate.api_key);
                memcpy(fresh.translate.api_base_url, old->translate.api_base_url, sizeof fresh.translate.api_base_url);
                memcpy(fresh.translate.local_model_path, old->translate.local_model_path,
                       sizeof fresh.translate.local_model_path);
                memcpy(fresh.voice.api_key, old->voice.api_key, sizeof fresh.voice.api_key);
                memcpy(fresh.voice.api_base_url, old->voice.api_base_url, sizeof fresh.voice.api_base_url);
                memcpy(fresh.voice.model_path, old->voice.model_path, sizeof fresh.voice.model_path);
                memcpy(fresh.sites, old->sites, sizeof fresh.sites);
                fresh.site_count = old->site_count;
                memcpy(fresh.profiles, old->profiles, sizeof fresh.profiles);
                fresh.profile_count = old->profile_count;
                fresh.browser_allowlist = old->browser_allowlist;
                memcpy(fresh.browser_recents, old->browser_recents, sizeof fresh.browser_recents);
                fresh.browser_recents_count = old->browser_recents_count;
                memcpy(fresh.browser_last_url, old->browser_last_url, sizeof fresh.browser_last_url);
                fresh.agent_hooks_notice_seen = old->agent_hooks_notice_seen;
                *sp->config = fresh;
                sp->browser_policy_dirty = true;
                sp->config->theme = theme_get_by_name(sp->config->theme_name);
                sp->needs_font_reload = true;
                sp->needs_layout = true;
            }
            return true;
        }
    }
    if (sp->reset_armed) sp->reset_armed = false;

    /* Content area — fixed settings UI metrics, apply scroll */
    f32 content_x = sl.content_x;
    f32 content_w = sl.content_w;
    f32 scroll_off = (f32)sp->scroll_y * ch * 0.5f;
    f32 content_y = sl.content_top + settings_content_top_off(ch, dpi) - scroll_off;
    f32 row_h = 32 * dpi;

    /* The body scrolls under a PINNED header (section title + description) and
     * is clipped to the band below it — see the scissor in settings_render.
     * Every tab's hit test below walks the same unclipped geometry the renderer
     * walks, so without this guard a click on the header would hit whatever row
     * had scrolled beneath it, invisible there but still live. Clipped away
     * means not clickable, so anything outside the band is swallowed here. */
    {
        f32 body_top    = sl.content_top + settings_clip_top_off(ch, dpi);
        f32 body_bottom = sl.clip_bottom;
        if (y < body_top || y >= body_bottom) return true;
    }

    if (sp->active_tab == SETTINGS_TAB_BROWSER) {
        SettingsBrowserLayout g = settings_browser_layout(content_x, content_y, content_w, dpi);
        if (settings_rect_contains(g.toggle, x, y)) {
            sp->config->browser_enabled = !sp->config->browser_enabled;
            sp->browser_policy_dirty = true;
        } else if (settings_rect_contains(g.submit, x, y)) {
            settings_browser_commit(sp);
        } else if (settings_rect_contains(g.input, x, y)) {
            sp->browser_rule_editing = true;
        } else {
            i32 count = CLAMP(sp->config->browser_allowlist.count, 0, BROWSER_ALLOWLIST_MAX);
            for (i32 i = 0; i < count; i++) {
                SettingsBrowserRow row = settings_browser_row(g, i);
                if (!settings_rect_contains(row.row, x, y)) continue;
                if (settings_rect_contains(row.remove, x, y)) {
                    browser_nav_allowlist_remove(&sp->config->browser_allowlist, i);
                    sp->browser_policy_dirty = true;
                    sp->browser_rule_status_error = false;
                    sp->browser_rule_editing = false;
                    sp->browser_rule_index = -1;
                    sp->browser_rule_buf[0] = '\0';
                    snprintf(sp->browser_rule_status, sizeof sp->browser_rule_status, "Site removed. Applies to the next navigation.");
                } else {
                    sp->browser_rule_index = i;
                    sp->browser_rule_editing = true;
                    sp->browser_rule_status_error = false;
                    snprintf(sp->browser_rule_buf, sizeof sp->browser_rule_buf, "%s", sp->config->browser_allowlist.rules[i]);
                    sp->browser_rule_status[0] = '\0';
                }
                return true;
            }
            sp->browser_rule_editing = false;
        }
        return true;
    }

    if (sp->active_tab == SETTINGS_TAB_DIAGNOSTICS) {
        /* Walks exactly the geometry the renderer walked — same header height
         * arithmetic, same diag_row_geom per row — so a Fix button can never
         * belong to a different row than the one under the pointer. */
        f32 yy   = content_y;
        f32 pad  = 14.0f * dpi;
        f32 bh   = settings_action_h(ch, dpi);
        f32 run_w = settings_action_w((i32)strlen(SETTINGS_LBL_DIAG_RUN), cw, ch, dpi);
        bool can_fix_all = sp->diag.ran && sp->diag.n_fixable > 0;
        f32 fix_w = can_fix_all
                  ? settings_action_w((i32)strlen(SETTINGS_LBL_DIAG_FIXALL), cw, ch, dpi)
                  : 0.0f;
        f32 head_h = pad + ch + 6.0f * dpi + ch + 10.0f * dpi + bh + pad;

        char nbin[1024];
        if (!diag_notify_bin(nbin, sizeof nbin)) nbin[0] = '\0';

        f32 by = yy + pad + ch + 6.0f * dpi + ch + 10.0f * dpi;
        f32 tx = content_x + pad;

        SettingsActionBtn run = settings_action_btn(tx, by, run_w,
                                    (i32)strlen(SETTINGS_LBL_DIAG_RUN), cw, ch, dpi);
        if (settings_action_hit(&run, x, y)) {
            diag_run(&sp->diag, nbin[0] ? nbin : NULL);
            sp->diag_last_fixed = -1;
            sp->diag_last_failed = 0;
            sp->scroll_y = 0;
            return true;
        }
        if (can_fix_all) {
            SettingsActionBtn fx = settings_action_btn(
                    tx + run_w + settings_action_gap(dpi), by, fix_w,
                    (i32)strlen(SETTINGS_LBL_DIAG_FIXALL), cw, ch, dpi);
            if (settings_action_hit(&fx, x, y)) {
                i32 nf = 0, nb = 0;
                diag_fix_all(&sp->diag, nbin[0] ? nbin : NULL, &nf, &nb);
                sp->diag_last_fixed  = nf;
                sp->diag_last_failed = nb;
                return true;
            }
        }
        yy += head_h + 10.0f * dpi;

        if (sp->diag.ran) {
            DiagGroup shown_group = (DiagGroup)-1;
            for (i32 i = 0; i < sp->diag.count; i++) {
                DiagItem *it = &sp->diag.items[i];
                if (!diag_row_visible(it)) continue;
                if (it->group != shown_group) {
                    shown_group = it->group;
                    yy += ch + 6.0f * dpi;
                }
                DiagRowGeom g = diag_row_geom(it, content_x, yy, content_w, cw, ch, dpi);
                if (g.has_fix && settings_action_hit(&g.fix, x, y)) {
                    diag_fix(it, nbin[0] ? nbin : NULL);
                    diag_recount(&sp->diag);
                    /* Deliberately NOT a full re-run: the user pressed one row's
                     * button, and re-running would reorder the list under the
                     * pointer. diag_fix already re-derived this row from disk. */
                    return true;
                }
                yy += g.h + 8.0f * dpi;
            }
        }
        return true;
    }

    if (sp->active_tab == SETTINGS_TAB_APPEARANCE) {
        f32 item_h = ch + 10;
        f32 col2 = content_x + 14 * cw;
        f32 panel_w2 = content_w;
        i32 font_cols = content_w >= 420.0f * dpi ? 3 :
                        content_w >= 260.0f * dpi ? 2 : 1;
        f32 font_row_h = ch + 16.0f * dpi;   /* same card height as themes */
        f32 font_gap = 6.0f * dpi;
        /* Theme tiles carry a colour strip, so they read at a smaller width
         * than a text-only row would — hence more columns. Geometry comes
         * from settings_layout.h; the hit test calls the same function, so
         * the two cannot drift. */
        i32 theme_cols = content_w >= 820.0f * dpi ? 6 :
                         content_w >= 640.0f * dpi ? 5 :
                         content_w >= 440.0f * dpi ? 4 :
                         content_w >= 260.0f * dpi ? 2 : 1;

        f32 ry = content_y;

        /* Add custom font button — same call the renderer makes. */
        SettingsFontHeader fh = settings_font_header(content_x, ry, content_w, cw, ch, dpi);
        SettingsActionBtn addf = fh.add;
        if (settings_action_hit(&addf, x, y)) {
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
        ry = fh.grid_y;

        /* Font grid */
        /* same header extent as the renderer */ /* separator + breathing room */
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
                /* 72, where the menu, the palette and Cmd+= stop too: the
                 * config load drops a size past 96 at the next launch, and a
                 * large enough one fails the glyph atlas. */
                if (sp->config->font_size < 72.0f) { sp->config->font_size += 1.0f; sp->needs_font_reload = true; }
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
        ry += item_h + 6.0f * dpi;

        /* Text Weight steppers — rows from settings_weight_block, the exact
         * function the renderer above called. A click at the end of the scale
         * changes nothing (settings_weight_step says so) and must not save. */
        {
            SettingsWeightBlock wb = settings_weight_block(ry, item_h, dpi);
            f32 *slots[2] = { &sp->config->text_weight_terminal,
                              &sp->config->text_weight_ui };
            f32 rows_y[2] = { wb.term_y, wb.ui_y };
            for (i32 k = 0; k < 2; k++) {
                if (y < rows_y[k] || y >= rows_y[k] + item_h) continue;
                i32 dir = 0;
                if (x >= seg_x && x < seg_x + seg_w * 0.5f)              dir = -1;
                else if (x >= seg_x + seg_w * 0.5f && x < seg_x + seg_w) dir = +1;
                if (dir == 0) break;
                if (settings_weight_step(slots[k], dir)) sp->text_weight_dirty = true;
                return true;
            }
            ry = wb.end + 10;
        }

        /* Theme grid. Hit zones must stay aligned with the
         * renderer above; if you change one, change both. */
        ry += ch + 16;
        i32 theme_count;
        const char **theme_names = theme_list_names(&theme_count);
        for (i32 i = 0; i < theme_count; i++) {
            bool is_user_theme = (i >= THEME_COUNT);
            ThemeCardRect card = settings_theme_card(i, theme_cols, content_x, ry,
                                                     panel_w2, ch, dpi);
            if (settings_theme_card_hit(i, theme_cols, content_x, ry,
                                        panel_w2, ch, dpi, x, y)) {
                /* Delete button hit-zone — card.del_btn_*, the exact rect the
                 * renderer fills on hover. */
                if (is_user_theme) {
                    f32 bx = card.del_btn_x, by = card.del_btn_y;
                    if (x >= bx && x < bx + card.del_btn_sz &&
                        y >= by && y < by + card.del_btn_sz) {
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
        f32 grid_end_y = ry + settings_theme_grid_h(theme_count, theme_cols, ch, dpi);

        /* Create / Import action buttons — same call the renderer makes. */
        SettingsActionBtn cbtn, ibtn;
        settings_action_row_theme(content_x, content_w, grid_end_y,
                                  cw, ch, dpi, &cbtn, &ibtn);
        {
            if (settings_action_hit(&cbtn, x, y)) {
                sp->requests_create_theme = true;
                return true;
            }
            if (settings_action_hit(&ibtn, x, y)) {
                /* Reuse the existing Import Theme... pipeline by piggy-
                 * backing on platform_open_file_dialog directly. */
                const char *filepath = platform_open_file_dialog(
                    "Import Theme",
                    "itermcolors,yml,yaml,toml,json");
                if (filepath) {
                    Theme imported = {0};
                    if (theme_import_file(filepath, &imported) && theme_save_user(&imported)) {
                        theme_load_user_themes();
                        app_themes_reloaded();   /* every window's config.theme points into the rebuilt array */
                        const Theme *ti = theme_get_by_name(imported.name);
                        /* The lookup falls back to the default theme: only the
                         * theme that was saved is applied. */
                        if (ti && strcmp(ti->name, imported.name) == 0) {
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

        /* --- WINDOW section ----------------------------------------------
         * Mirrors the render's own geometry block: same value column, same
         * chip width, same row order. */
        {
            f32 ry = grid_end_y + cbtn.h + 18.0f * dpi;
            f32 value_off = fminf(18.0f * cw, content_w * 0.42f);
            if (value_off > content_w - 8.0f * cw) value_off = content_w - 8.0f * cw;
            if (value_off < 0.0f) value_off = 0.0f;
            f32 value_x = content_x + value_off;
            f32 value_room = content_x + content_w - value_x;

        /* --- WINDOW section: header, then one row per control -------------
         * Mirrors the render row for row. Both sliders record their track so
         * the drag that follows the press scrubs the same geometry that was
         * drawn, and the wallpaper sub-rows refuse clicks while there is no
         * image — matching the dimmed state they are drawn in. */
        ry += 12.0f * dpi;          /* section gap  */
        ry += ch + 12.0f * dpi;     /* "WINDOW" header */

        f32 read_w = 6.0f * cw;

        /* Opacity slider — press anywhere on the track sets the value and
         * arms the scrub. */
        if (y >= ry && y < ry + row_h) {
            f32 track_w = value_room - read_w - 8.0f * dpi;
            if (track_w < 6.0f * cw) track_w = value_room * 0.6f;
            if (x >= value_x && x <= value_x + track_w) {
                f32 t = settings_slider_t_at(x, value_x, track_w, ch + 8.0f * dpi);
                sp->config->opacity = SETTINGS_OPACITY_MIN +
                                      t * (1.0f - SETTINGS_OPACITY_MIN);
                /* Window alpha lives in the window server, not in the frame
                 * Liu paints, so the window has to be told: changing the
                 * number alone leaves the window as it was. */
                sp->bg_dirty = true;
                sp->drag_slider  = SETTINGS_SLIDER_OPACITY;
                sp->drag_track_x = value_x;
                sp->drag_track_w = track_w;
                sp->drag_track_h = ch + 8.0f * dpi;
                return true;
            }
            return false;
        }
        ry += row_h;

        /* Blur */
        if (y >= ry && y < ry + row_h) {
            const i32 n = (i32)(sizeof s_blur_presets / sizeof s_blur_presets[0]);
            f32 seg_w = settings_chip_w(value_room, cw);
            i32 pi = (i32)((x - value_x) / seg_w);
            if (x >= value_x && pi >= 0 && pi < n) {
                f32 rad = s_blur_presets[pi];
                sp->config->background_blur = (rad > 0.0f);
                if (rad > 0.0f) sp->config->background_blur_radius = rad;
                sp->bg_dirty = true;
            }
            return true;
        }
        ry += row_h;

        /* Wallpaper row: Choose… / Remove, right-aligned in the value column. */
        if (y >= ry && y < ry + row_h) {
            f32 btn_w = 9.0f * cw;
            f32 gap   = 6.0f * dpi;
            f32 bx = value_x + value_room - btn_w * 2.0f - gap;
            i32 pi = (i32)((x - bx) / btn_w);
            if (x >= bx && pi == 0) {
                const char *pick = platform_open_file_dialog(
                    "Choose background image", "png,jpg,jpeg,bmp,gif,tga");
                if (pick && pick[0]) {
                    snprintf(sp->config->background_image,
                             sizeof sp->config->background_image, "%s", pick);
                    sp->bg_dirty = true;
                }
            } else if (x >= bx && pi == 1) {
                sp->config->background_image[0] = '\0';
                sp->bg_dirty = true;
            }
            return true;
        }
        ry += row_h;

        /* Fit / Fade belong to the wallpaper: inert until one exists. */
        bool has_img = sp->config->background_image[0] != '\0';
        f32 sub_room = content_x + content_w - value_x;

        if (y >= ry && y < ry + row_h) {
            if (!has_img) return true;   /* consumed, but nothing to configure */
            const i32 n = (i32)(sizeof s_fit_modes / sizeof s_fit_modes[0]);
            f32 seg_w = settings_chip_w(sub_room, cw);
            i32 pi = (i32)((x - value_x) / seg_w);
            if (x >= value_x && pi >= 0 && pi < n) {
                sp->config->background_mode = s_fit_modes[pi];
                sp->bg_dirty = true;
            }
            return true;
        }
        ry += row_h;

        if (y >= ry && y < ry + row_h) {
            if (!has_img) return true;
            f32 track_w = sub_room - read_w - 8.0f * dpi;
            if (track_w < 6.0f * cw) track_w = sub_room * 0.6f;
            if (x >= value_x && x <= value_x + track_w) {
                sp->config->background_opacity =
                    settings_slider_t_at(x, value_x, track_w, ch + 8.0f * dpi);
                sp->bg_dirty = true;
                sp->drag_slider  = SETTINGS_SLIDER_FADE;
                sp->drag_track_x = value_x;
                sp->drag_track_w = track_w;
                sp->drag_track_h = ch + 8.0f * dpi;
                return true;
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
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->cursor_blink = !sp->config->cursor_blink;
            return true;
        }
        ry += row_h;

        /* "BEHAVIOR" header */
        ry += 12.0f * dpi;
        ry += ch + 12.0f * dpi;

        SettingsStepper history = settings_stepper(value_x, ry, value_room, ch, dpi);
        if (settings_rect_contains(history.minus, x, y) || settings_rect_contains(history.plus, x, y)) {
            settings_scrollback_step(&sp->config->scrollback_lines,
                                     settings_rect_contains(history.plus, x, y) ? 1 : -1);
            return true;
        }
        ry += row_h;

        /* Bold bright */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->bold_is_bright = !sp->config->bold_is_bright;
            return true;
        }
        ry += row_h;

        ry += settings_section_h(ch, dpi);
        /* Drag selects, even over a program that grabbed the mouse */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->mouse_drag_selects = !sp->config->mouse_drag_selects;
            return true;
        }
        ry += row_h;
        ry += row_h;   /* its dim caption row */

        /* Copy on select */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->copy_on_select = !sp->config->copy_on_select;
            return true;
        }
        ry += row_h;
        ry += row_h;   /* its dim caption row */

        /* Trim the blanks off the end of every copied line */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->clipboard_trim_trailing_spaces =
                !sp->config->clipboard_trim_trailing_spaces;
            selection_set_trim_trailing(sp->config->clipboard_trim_trailing_spaces);
            return true;
        }
        ry += row_h;
        ry += row_h;   /* its dim caption row */

        /* Option as alt — mirrors the 4-up segmented control drawn above */
        if (y >= ry && y < ry + row_h) {
            f32 seg_w = 7.0f * cw;
            if (value_room > 0.0f && seg_w * 4.0f > value_room) {
                seg_w = value_room / 4.0f;
            }
            if (seg_w < 1.0f) seg_w = 1.0f;
            if (x >= value_x && x < value_x + seg_w * 4.0f) {
                sp->config->option_as_alt = (OptionAsAlt)((x - value_x) / seg_w);
                /* Push straight to the platform: this row changes no layout,
                 * so nothing else here would re-apply the config, and the key
                 * handler reads platform state on the very next keystroke. */
                platform_set_option_as_alt((i32)sp->config->option_as_alt);
                return true;
            }
            return false;   /* empty space beside the control is inert */
        }
        ry += row_h;

        ry += settings_section_h(ch, dpi);
        /* Tab bar toggle */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->show_tab_bar = !sp->config->show_tab_bar;
            sp->needs_layout = true;
            return true;
        }
        ry += row_h;

        /* Toolbar icons toggle */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->show_toolbar_icons = !sp->config->show_toolbar_icons;
            sp->needs_layout = true;
            return true;
        }
        ry += row_h;

        ry += settings_section_h(ch, dpi);
        /* ---- Sidebar rows — same order and count as the painter above ---- */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->sidebar_tabs = !sp->config->sidebar_tabs;
            config_sidebar_ensure_section(sp->config);   /* never an empty bar */
            sp->needs_layout = true;
            return true;
        }
        ry += row_h;
        {
            static const i32 sec_rows[3] = { SB_SEC_FILES, SB_SEC_AGENTS, SB_SEC_NOTES };
            for (i32 k = 0; k < 3; k++) {
                if (y >= ry && y < ry + row_h &&
                    x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
                    bool on = sp->config->sidebar_enabled[sec_rows[k]];
                    /* The last visible section stays on: the bar must never be empty. */
                    if (on && config_sidebar_visible_sections(sp->config) <= 1) return true;
                    sp->config->sidebar_enabled[sec_rows[k]] = !on;
                    sp->needs_layout = true;
                    return true;
                }
                ry += row_h;
            }
        }
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->sidebar_compact = !sp->config->sidebar_compact;
            sp->needs_layout = true;
            return true;
        }
        ry += row_h;
        if (y >= ry && y < ry + row_h) {
            f32 fold_seg_w = 8.0f * cw;
            if (value_room > 0.0f && fold_seg_w * 3.0f > value_room)
                fold_seg_w = value_room / 3.0f;
            if (fold_seg_w < 1.0f) fold_seg_w = 1.0f;
            if (x >= value_x && x < value_x + fold_seg_w * 3.0f) {
                i32 seg = (i32)((x - value_x) / fold_seg_w);
                if (seg < 0) seg = 0;
                if (seg > 2) seg = 2;
                sp->config->sidebar_collapse_below_pt = seg == 0 ? 0.0f : seg == 1 ? 1100.0f : 1300.0f;
                sp->needs_layout = true;
                return true;
            }
            return false;
        }
        ry += row_h;
        {
            /* "Customize Sidebar" — same geometry call the painter makes. */
            SettingsActionBtn sbb = settings_action_row_sidebar(value_x, value_room,
                                                                ry, cw, ch, dpi);
            if (settings_action_hit(&sbb, x, y)) {
                sp->requests_customize_sidebar = true;
                return true;
            }
        }
        ry += row_h;

        ry += settings_section_h(ch, dpi);
#ifdef PLATFORM_LINUX
        /* Hide title bar toggle — mirrors the draw block above under the same
         * #ifdef so the row count stays identical on both sides. */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->hide_title_bar = !sp->config->hide_title_bar;
            sp->needs_layout = true;
            return true;
        }
        ry += row_h;
#endif

        /* Scrollbar toggle */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->show_scrollbar = !sp->config->show_scrollbar;
            return true;
        }
        ry += row_h;

        /* Auto-hide toggle — same row order as the painter above */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->scrollbar_autohide = !sp->config->scrollbar_autohide;
            return true;
        }
        ry += row_h;

        /* Status bar toggle */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->show_status_bar = !sp->config->show_status_bar;
            sp->needs_layout = true;
            return true;
        }
        ry += row_h;

        /* Confirm paste toggle — mirrors the row painted after Status Bar */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->paste_protection = !sp->config->paste_protection;
            return true;
        }
        ry += row_h;

        ry += settings_section_h(ch, dpi);
        /* Close guard (confirm close while an agent CLI runs) */
        if (y >= ry && y < ry + row_h &&
            x >= value_x && x < value_x + settings_switch_w(cw, dpi)) {
            sp->config->confirm_close_agent = !sp->config->confirm_close_agent;
            return true;
        }
        ry += row_h;

        /* Prompt recall — mirrors the 3-up segmented control drawn above */
        if (y >= ry && y < ry + row_h) {
            f32 seg_w = 8.0f * cw;
            if (value_room > 0.0f && seg_w * 3.0f > value_room) {
                seg_w = value_room / 3.0f;
            }
            if (seg_w < 1.0f) seg_w = 1.0f;
            if (x >= value_x && x < value_x + seg_w * 3.0f) {
                sp->config->prompt_recall =
                    (PromptRecallMode)((x - value_x) / seg_w);
                sp->requests_prompt_recall_sync = true;
                return true;
            }
            return false;   /* empty space beside the control is inert */
        }
        ry += row_h;
        /* Caption row (no-op) */
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
        ry += ch + 12.0f * dpi;  /* past "PROMPT TRANSLATION" header + section rule */

        /* Inline API editors commit on any click — including one on their
         * own row, whose handler below just reopens the editor over the
         * committed value (net effect: commit and keep editing). */
        settings_commit_api_editors(sp);

        /* The Model menu is OVER the rows below its chip, so a click is its
         * own before it is theirs: on a row it picks that model, on the head
         * or the padding it does nothing, and anywhere else — the chip that
         * opened it included — it closes the menu and is spent. The click that
         * dismisses a menu does not also flip whatever was under the pointer. */
        if (sp->translate_model_picker_open) {
            SettingsModelRows rows;
            settings_translate_model_rows(sp, &rows);
            SettingsModelMenu menu;
            i32 hit = settings_translate_model_menu_place(sp, &rows, sw, sh, dpi, &menu)
                    ? settings_model_menu_hit(&menu, x, y) : SETTINGS_MODEL_HIT_NONE;
            i32 pin = rows.pinned ? 1 : 0;
            if (hit == SETTINGS_MODEL_HIT_NONE) settings_model_menu_close(sp);
            else if (hit == SETTINGS_MODEL_HIT_PINNED) settings_model_menu_pick(sp, &rows, 0);
            else if (hit == SETTINGS_MODEL_HIT_TRACK && menu.track.h > 0.0f) {
                /* The window goes to where the track was clicked. */
                i32 over = rows.n - pin - menu.visible;
                f32 f = (y - menu.track.y) / menu.track.h;
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                sp->translate_model_menu_scroll = settings_model_menu_clamp_scroll(
                    (i32)lroundf(f * (f32)(over > 0 ? over : 0)), rows.n - pin, menu.visible);
            }
            else if (hit >= 0) {
                i32 scroll = settings_model_menu_clamp_scroll(sp->translate_model_menu_scroll,
                                                              rows.n - pin, menu.visible);
                settings_model_menu_pick(sp, &rows, pin + scroll + hit);
            }
            return true;
        }

        /* Enable */
        if (y >= ry && y < ry + row_h &&
            x >= tr_value_x && x < tr_value_x + settings_switch_w(cw, dpi)) {
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
            /* Agent → segmented click (5 segments: Claude / OpenCode /
             * Codex / Grok / Antigravity). Segment hit boundaries
             * mirror the render math exactly — keep the same seg_w
             * divisor as render. */
            if (y >= ry && y < ry + row_h) {
                f32 value_off = fminf(18.0f * cw, content_w * 0.42f);
                if (value_off > content_w - 8.0f * cw) value_off = content_w - 8.0f * cw;
                if (value_off < 0.0f) value_off = 0.0f;
                f32 value_x = content_x + value_off;
                f32 value_room = content_x + content_w - value_x;
                const char *ids[] = {"claude", "opencode", "codex", "grok",
                                     "antigravity"};
                const i32 seg_n = (i32)(sizeof ids / sizeof ids[0]);
                f32 seg_w = 9.0f * cw;
                if (value_room > 0.0f && seg_w * (f32)seg_n > value_room) {
                    seg_w = value_room / (f32)seg_n;
                }
                if (seg_w < 1.0f) seg_w = 1.0f;
                for (i32 i = 0; i < seg_n; i++) {
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

            /* Model → its menu opens (an open menu took the click above, so
             * this is always an opening). The chip is the control: a click on
             * the row's label or beside the chip is not one. Opening it closes
             * the language picker — one popup at a time. */
            if (y >= ry && y < ry + row_h) {
                if (x >= tr_value_x) settings_model_menu_open(sp);
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
                /* 11cw — mirrors the render width, which leaves room for
                 * "OpenRouter". */
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
                    sp_api_edit_open(sp, SETTINGS_TAB_TRANSLATE,
                                     &sp->editing_api_key,
                                     sp->api_key_buf, sizeof(sp->api_key_buf),
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
                        sp_api_edit_open(sp, SETTINGS_TAB_TRANSLATE,
                                         &sp->editing_api_model,
                                         sp->api_model_buf,
                                         sizeof(sp->api_model_buf),
                                         tc->api_model);
                        sp->translate_model_picker_open = false;
                        sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
                    }
                    return true;
                }
                if (x >= tr_value_x) settings_model_menu_open(sp);
                return true;
            }
            ry += row_h;

            /* Base URL (custom provider only) → inline editor. */
            if (custom) {
                if (y >= ry && y < ry + row_h) {
                    if (x >= tr_value_x) {
                        sp_api_edit_open(sp, SETTINGS_TAB_TRANSLATE,
                                         &sp->editing_api_url,
                                         sp->api_url_buf,
                                         sizeof(sp->api_url_buf),
                                         tc->api_base_url);
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
        if (sp->tr_model_del_w > 0.0f &&
            x >= sp->tr_model_del_x && x < sp->tr_model_del_x + sp->tr_model_del_w &&
            y >= sp->tr_model_del_y && y < sp->tr_model_del_y + sp->tr_model_del_h) {
            if (!tr_del_was_armed) {
                sp->tr_model_del_armed = true;
            } else {
                char delp[512];
                const char *target =
                    tc->local_model_path[0]
                        ? tc->local_model_path
                        : liu_model_default_path(delp, sizeof delp);
                /* Re-resolved and re-checked here rather than trusted from the
                 * frame that drew the button: the path can change under a
                 * click, and this is the one action that cannot be undone. */
                if (target && settings_model_is_liu_owned(target)) {
                    /* Nothing to unload first — the Translate engine loads and
                     * frees the model per translation and keeps nothing
                     * resident between them. */
                    remove(target);
                }
            }
            return true;
        }
        ry += 2.0f * row_h;

        /* "ACTIVE IN" header + section rule */
        ry += 12.0f * dpi;
        ry += ch + 12.0f * dpi;

        /* Same table, same order, same row pitch as the painter. */
        for (i32 i = 0; i < TRANSLATE_AGENT_ROW_COUNT; i++) {
            if (y >= ry && y < ry + row_h &&
                x >= tr_value_x && x < tr_value_x + settings_switch_w(cw, dpi)) {
                bool *flag = translate_agent_flag(tc, i);
                if (flag) { *flag = !*flag; return true; }
            }
            ry += row_h;
        }
    }

    if (sp->active_tab == SETTINGS_TAB_VOICE) {
        VoiceConfig *vc = &sp->config->voice;
        f32 ry = content_y;
        f32 vo_value_off = fminf(18.0f * cw, content_w * 0.42f);
        if (vo_value_off > content_w - 8.0f * cw) vo_value_off = content_w - 8.0f * cw;
        if (vo_value_off < 0.0f) vo_value_off = 0.0f;
        f32 vo_value_x = content_x + vo_value_off;
        f32 vo_value_room = content_x + content_w - vo_value_x;
        f32 vo_control_h = ch + 8.0f * dpi;

        ry += ch + 12.0f * dpi;   /* past the "VOICE INPUT" header + rule */

        /* Inline API editors commit on any click, exactly as on the Translate
         * tab — a click on their own row reopens the editor over the committed
         * value, so the net effect there is "commit and keep editing". */
        settings_commit_api_editors(sp);

        /* Enable */
        if (y >= ry && y < ry + row_h &&
            x >= vo_value_x && x < vo_value_x + settings_switch_w(cw, dpi)) {
            vc->enabled = !vc->enabled;
            return true;
        }
        ry += row_h;

        /* Backend — Local / API. Same divisor as the painter. */
        if (y >= ry && y < ry + row_h) {
            const i32 n = 2;
            f32 seg_w = 10.0f * cw;
            if (vo_value_room > 0.0f && seg_w * (f32)n > vo_value_room)
                seg_w = vo_value_room / (f32)n;
            if (seg_w < 1.0f) seg_w = 1.0f;
            if (x >= vo_value_x && x < vo_value_x + seg_w * (f32)n) {
                i32 seg = (i32)((x - vo_value_x) / seg_w);
                VoiceBackend nb = (seg == 1) ? VOICE_BACKEND_API
                                             : VOICE_BACKEND_LOCAL;
                vc->backend = nb;
            }
            return true;
        }
        ry += row_h;

        /* The API rows only exist under Backend = API. Hit-testing them
         * unconditionally would offset every row below by four phantom slots
         * and swallow the clicks meant for them. */
        if (vc->backend == VOICE_BACKEND_API) {
            const bool vo_custom = strcmp(vc->api_provider, "custom") == 0;

            /* Provider */
            if (y >= ry && y < ry + row_h) {
                const char *ids[] = { "openai", "groq", "elevenlabs", "custom" };
                const i32 n = 4;
                f32 seg_w = 11.0f * cw;
                if (vo_value_room > 0.0f && seg_w * (f32)n > vo_value_room)
                    seg_w = vo_value_room / (f32)n;
                if (seg_w < 1.0f) seg_w = 1.0f;
                for (i32 i = 0; i < n; i++) {
                    f32 sx = vo_value_x + (f32)i * seg_w;
                    if (x >= sx && x < sx + seg_w) {
                        if (strcmp(vc->api_provider, ids[i]) != 0) {
                            snprintf(vc->api_provider, sizeof(vc->api_provider),
                                     "%s", ids[i]);
                            /* Every provider names its models differently, so
                             * the old id is meaningless under the new one. */
                            vc->api_model[0] = '\0';
                        }
                        return true;
                    }
                }
                return true;
            }
            ry += row_h;

            /* API Key */
            if (y >= ry && y < ry + row_h) {
                if (x >= vo_value_x)
                    sp_api_edit_open(sp, SETTINGS_TAB_VOICE,
                                     &sp->editing_api_key,
                                     sp->api_key_buf, sizeof(sp->api_key_buf),
                                     vc->api_key);
                return true;
            }
            ry += row_h;

            /* Model */
            if (y >= ry && y < ry + row_h) {
                if (x >= vo_value_x)
                    sp_api_edit_open(sp, SETTINGS_TAB_VOICE,
                                     &sp->editing_api_model,
                                     sp->api_model_buf, sizeof(sp->api_model_buf),
                                     vc->api_model);
                return true;
            }
            ry += row_h;

            /* Base URL (custom provider only) */
            if (vo_custom) {
                if (y >= ry && y < ry + row_h) {
                    if (x >= vo_value_x)
                        sp_api_edit_open(sp, SETTINGS_TAB_VOICE,
                                         &sp->editing_api_url,
                                         sp->api_url_buf, sizeof(sp->api_url_buf),
                                         vc->api_base_url);
                    return true;
                }
                ry += row_h;
            }

            ry += row_h;   /* the dim caption row */
        }

        /* Microphone — toggle the picker. Opening it forces a device rescan:
         * the cached list may be up to a second old, and a menu that opens
         * without the headset the user just plugged in is the one moment the
         * staleness would show. */
        if (y >= ry && y < ry + row_h) {
            sp->voice_mic_picker_open = !sp->voice_mic_picker_open;
            if (sp->voice_mic_picker_open) {
                settings_voice_mics_refresh(sp, true);
                sp->voice_lang_picker_open = false;   /* one popup at a time */
            }
            return true;
        }
        ry += row_h;

        /* The open picker takes a band of its own and pushes the rows below it
         * down, exactly as the painter does. */
        if (sp->voice_mic_picker_open) {
            f32 menu_w = vo_value_room;
            if (menu_w < 24.0f * cw) menu_w = 24.0f * cw;
            f32 menu_x = vo_value_x;
            if (menu_x + menu_w > content_x + content_w)
                menu_x = content_x + content_w - menu_w;
            if (menu_x < content_x) menu_x = content_x;
            f32 menu_y = ry + 4.0f * dpi;
            f32 menu_h = settings_voice_mic_menu_height(sp, ch, dpi);
            if (y >= menu_y && y < menu_y + menu_h &&
                x >= menu_x && x < menu_x + menu_w) {
                i32 row = settings_voice_mic_menu_hit(sp, x, y, menu_x, menu_y,
                                                      menu_w, ch, dpi);
                if (row == 0) {
                    vc->input_device[0] = '\0';
                } else if (row > 0) {
                    snprintf(vc->input_device, sizeof vc->input_device, "%s",
                             sp->voice_mics[row - 1].uid);
                }
                if (row >= 0) {
                    /* Takes effect on the next recording; a device cannot be
                     * swapped under a running capture engine. */
                    platform_audio_set_input_device(vc->input_device);
                    sp->voice_mic_picker_open = false;
                }
                return true;   /* swallow clicks anywhere on the popup */
            }
            ry += menu_h + 8.0f * dpi;
        }

        /* Language — toggle the picker. */
        if (y >= ry && y < ry + row_h) {
            sp->voice_lang_picker_open = !sp->voice_lang_picker_open;
            sp->voice_lang_menu_scroll = 0;
            sp->voice_lang_menu_accum  = 0.0f;
            if (sp->voice_lang_picker_open) sp->voice_mic_picker_open = false;
            return true;
        }
        ry += row_h;

        /* The open picker takes a band of its own and pushes the rows below it
         * down, exactly as the painter does — so it is consumed here, in the
         * same place, before `ry` moves on. Skipping the advance would leave
         * every row under it offset by a whole menu height. */
        if (sp->voice_lang_picker_open) {
            f32 menu_w = settings_voice_lang_menu_width(cw, dpi, content_w);
            f32 menu_x = vo_value_x;
            if (menu_x + menu_w > content_x + content_w)
                menu_x = content_x + content_w - menu_w;
            if (menu_x < content_x) menu_x = content_x;
            f32 menu_y = ry + 4.0f * dpi;
            f32 menu_h = settings_voice_lang_menu_height(ch, dpi);
            if (y >= menu_y && y < menu_y + menu_h &&
                x >= menu_x && x < menu_x + menu_w) {
                i32 idx = settings_voice_lang_menu_hit(sp, x, y, menu_x, menu_y,
                                                       menu_w, ch, dpi);
                if (idx >= 0) {
                    snprintf(vc->lang, sizeof vc->lang, "%s",
                             settings_voice_lang_code(idx));
                    sp->voice_lang_picker_open = false;
                }
                return true;   /* swallow clicks anywhere on the popup */
            }
            ry += menu_h + 8.0f * dpi;
        }

        /* Translate to English */
        if (y >= ry && y < ry + row_h &&
            x >= vo_value_x && x < vo_value_x + settings_switch_w(cw, dpi)) {
            vc->translate = !vc->translate;
            return true;
        }
        ry += row_h;

        /* Stop on silence */
        if (y >= ry && y < ry + row_h) {
            const f32 vals[] = { 0.0f, 1.5f, 2.0f, 3.0f };
            const i32 n = (i32)(sizeof vals / sizeof vals[0]);
            f32 seg_w = 7.0f * cw;
            if (vo_value_room > 0.0f && seg_w * (f32)n > vo_value_room)
                seg_w = vo_value_room / (f32)n;
            if (seg_w < 1.0f) seg_w = 1.0f;
            if (x >= vo_value_x && x < vo_value_x + seg_w * (f32)n) {
                i32 seg = (i32)((x - vo_value_x) / seg_w);
                if (seg >= 0 && seg < n) vc->silence_stop_sec = vals[seg];
            }
            return true;
        }
        ry += row_h;

        /* Submit after dictation */
        if (y >= ry && y < ry + row_h &&
            x >= vo_value_x && x < vo_value_x + settings_switch_w(cw, dpi)) {
            vc->auto_submit = !vc->auto_submit;
            return true;
        }
        ry += row_h;

        /* Sounds */
        if (y >= ry && y < ry + row_h &&
            x >= vo_value_x && x < vo_value_x + settings_switch_w(cw, dpi)) {
            vc->sounds = !vc->sounds;
            return true;
        }
        ry += row_h;

        /* Model buttons — dispatched off the rects the painter cached. */
        if (sp->vo_model_cancel_w > 0.0f &&
            x >= sp->vo_model_cancel_x &&
            x <  sp->vo_model_cancel_x + sp->vo_model_cancel_w &&
            y >= sp->vo_model_cancel_y &&
            y <  sp->vo_model_cancel_y + sp->vo_model_cancel_h) {
            model_download_cancel();
            return true;
        }
        if (sp->vo_model_dl_w > 0.0f &&
            x >= sp->vo_model_dl_x && x < sp->vo_model_dl_x + sp->vo_model_dl_w &&
            y >= sp->vo_model_dl_y && y < sp->vo_model_dl_y + sp->vo_model_dl_h) {
            char dest[512];
            if (liu_whisper_default_path(dest, sizeof dest)) {
                /* Explicit URL/dest/hash: the downloader's compiled-in
                 * defaults are the Translate GGUF, and passing NULL here
                 * would quietly fetch the wrong model. */
                model_download_start(LIU_WHISPER_URL, dest,
                                     LIU_WHISPER_SHA256, LIU_WHISPER_SIZE);
                snprintf(vc->model_path, sizeof vc->model_path, "%s", dest);
                config_save(sp->config, config_file_path());
            }
            return true;
        }
        if (sp->vo_model_pick_w > 0.0f &&
            x >= sp->vo_model_pick_x &&
            x <  sp->vo_model_pick_x + sp->vo_model_pick_w &&
            y >= sp->vo_model_pick_y &&
            y <  sp->vo_model_pick_y + sp->vo_model_pick_h) {
            const char *fp = platform_open_file_dialog("Select Whisper Model", "bin");
            if (fp) {
                snprintf(vc->model_path, sizeof vc->model_path, "%s", fp);
                config_save(sp->config, config_file_path());
            }
            return true;
        }
        if (sp->vo_model_del_w > 0.0f &&
            x >= sp->vo_model_del_x && x < sp->vo_model_del_x + sp->vo_model_del_w &&
            y >= sp->vo_model_del_y && y < sp->vo_model_del_y + sp->vo_model_del_h) {
            if (!vo_del_was_armed) {
                sp->vo_model_del_armed = true;
            } else {
                char delp[512];
                const char *target =
                    vc->model_path[0] ? vc->model_path
                                      : liu_whisper_default_path(delp, sizeof delp);
                /* Re-checked here, not trusted from the frame that drew the
                 * button: the path can change under a click (Choose file, a
                 * finished download) and this is the one action that cannot be
                 * taken back. */
                if (target && settings_model_is_liu_owned(target)) {
                    /* Nothing to unload first: the voice worker frees the
                     * engine after every utterance rather than keeping ~600 MB
                     * resident (see voice.c), so in the steady state no handle
                     * is open on this file. Deleting DURING a transcription is
                     * safe too — the unlink leaves the running engine's open
                     * handle valid and the space comes back when it closes;
                     * the next dictation reports that no model is present,
                     * which is the truth. */
                    remove(target);
                }
                /* Keep the configured path when a model is removed: it is the
                 * download destination, not an indication that a model is
                 * present. The row checks the file and updates its status. */
            }
            return true;
        }
        ry += 2.0f * row_h;
        (void)vo_control_h;
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
        /* Per-agent hook button row. Only agents with a hook installer
         * (agent_hook_supported) have cached rects; other detected agents
         * are listed for visibility and a click on them falls through.
         * The status string is shared across rows: it reports the last
         * install or uninstall, whichever row was clicked. */
        if (sp->notify_auto_w > 0.0f &&
            x >= sp->notify_auto_x && x < sp->notify_auto_x + sp->notify_auto_w &&
            y >= sp->notify_auto_y && y < sp->notify_auto_y + sp->notify_auto_h) {
            sp->config->agent_hooks = !sp->config->agent_hooks;
            config_save(sp->config, config_file_path());
            sp->notify_hooks_request = sp->config->agent_hooks ? 1 : 2;
            snprintf(sp->notify_claude_status, sizeof sp->notify_claude_status, "%s",
                     sp->config->agent_hooks ? "Hooks are installed while Liu runs."
                                             : "Hooks removed. Liu will not add them at launch.");
            return true;
        }
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
                        /* Through the shim, like the install at launch: the
                         * resolved binary's own path stops existing when the
                         * build is cleaned or the app moves, which is what
                         * the shim exists for (agent_hooks.h). */
                        char nbin[1024];
                        resolve_liu_notify_path(nbin, sizeof nbin);
                        res = agent_hook_install_shimmed(id, nbin);
                        if (res.ok)
                            snprintf(sp->notify_hook_installed,
                                     sizeof sp->notify_hook_installed, "%s", id);
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
                settings_close(sp);
                sp->requests_vault_unlock = true;
                return true;
            }
        } else if (!unlocked) {
            f32 cbw = panel_inner_w;
            if (y >= ry && y < ry + btn_h && x >= content_x && x < content_x + cbw) {
                settings_close(sp);
                sp->requests_vault_unlock = true;
                return true;
            }
        } else {
            /* Unlocked: Lock Now + Open Browser (side-by-side halves). */
            f32 gap = 8.0f * dpi;
            f32 half_w = (panel_inner_w - gap) * 0.5f;
            if (y >= ry && y < ry + btn_h) {
                if (x >= content_x && x < content_x + half_w) {
                    vault_lock(v);
                    settings_clear_focus(sp);
                    snprintf(sp->vault_status, sizeof sp->vault_status,
                             "Vault locked");
                    return true;
                }
                f32 bx2 = content_x + half_w + gap;
                if (x >= bx2 && x < bx2 + half_w) {
                    settings_close(sp);
                    sp->requests_vault_browser = true;
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
                settings_vault_submit(sp);
                return true;
            }
        }
        return true;
    }

    if (sp->active_tab == SETTINGS_TAB_ABOUT) {
        /* All About-tab rects are cached during render — use them directly so
         * clicks stay aligned with the phase-dependent update layout. */
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

bool settings_handle_click(SettingsPanel *sp, f32 x, f32 y, f32 sw, f32 sh,
                            f32 dpi, f32 cw, f32 ch) {
    if (!sp || !sp->open || !sp->config) return false;
    /* Pointer actions are infrequent. A bounded snapshot catches changes in
     * every section without file writes or hashing during rendering/typing. */
    AppConfig before = *sp->config;
    bool handled = settings_handle_click_impl(sp, x, y, sw, sh, dpi, cw, ch);
    sp->config_dirty |= memcmp(&before, sp->config, sizeof before) != 0;
    crypto_secure_zero(&before, sizeof before);
    return handled;
}

bool settings_handle_char(SettingsPanel *sp, struct AppState *app, u32 codepoint) {
    if (!sp->open) return false;

    if (sp->active_tab == SETTINGS_TAB_BROWSER && sp->browser_rule_editing) {
        if (codepoint >= 32 && codepoint < 127) {
            i32 len = (i32)strlen(sp->browser_rule_buf);
            TextEdit e = app_edit(app, sp->browser_rule_buf, &len, sizeof sp->browser_rule_buf);
            textedit_insert_cp(&e, app ? &app->te_undo : NULL, codepoint);
            app_edit_done(app, len);
        }
        return true;
    }

    /* The Model menu's search field: what is typed while the menu is open
     * narrows its list (model ids are ASCII). A list short enough to show
     * whole has no field, and what is typed over it goes nowhere. */
    if (sp->active_tab == SETTINGS_TAB_TRANSLATE && sp->translate_model_picker_open) {
        SettingsModelRows rows;
        settings_translate_model_rows(sp, &rows);
        if (rows.searchable && app && codepoint >= 32 && codepoint < 127) {
            i32 len = (i32)strlen(sp->model_menu_filter);
            TextEdit e = app_edit(app, sp->model_menu_filter, &len, (i32)sizeof sp->model_menu_filter);
            textedit_insert_cp(&e, &app->te_undo, codepoint);
            app_edit_done(app, (i32)strlen(sp->model_menu_filter));
            settings_model_menu_refilter(sp);
        }
        return true;
    }

    /* API key / base URL / model inline editors accept ASCII printable (keys,
     * URLs and model ids are ASCII by construction). Shared by the Translate
     * and the Voice tab — settings_api_editing() is what decides whether one
     * is really open, and on which tab. */
    if (settings_api_editing(sp)) {
        char *buf; usize cap;
        if (sp->editing_api_key)        { buf = sp->api_key_buf;   cap = sizeof(sp->api_key_buf); }
        else if (sp->editing_api_url)   { buf = sp->api_url_buf;   cap = sizeof(sp->api_url_buf); }
        else                            { buf = sp->api_model_buf; cap = sizeof(sp->api_model_buf); }
        /* '"' and '\\' are excluded: config.json is written with a raw
         * fprintf("%s") (no JSON escaping) and they'd also break the
         * curl-config quoting at spawn. Neither is valid in a key/URL/id. */
        if (codepoint >= 32 && codepoint < 127 &&
            codepoint != '"' && codepoint != '\\') {
            i32 flen = (i32)strlen(buf);
            TextEdit e = app_edit_ex(app, buf, &flen, (i32)cap,
                                     sp->editing_api_key ? TEXTEDIT_SECRET : 0);
            textedit_insert_cp(&e, sp->editing_api_key || !app ? NULL : &app->te_undo,
                               codepoint);
            app_edit_done(app, flen);
        }
        return true;   /* swallow everything while editing */
    }

    /* Vault — password fields accept ASCII printable. */
    if (sp->active_tab == SETTINGS_TAB_VAULT) {
        char *buf = sp->vault_editing_old_pw ? sp->vault_old_pw
                  : sp->vault_editing_new_pw ? sp->vault_new_pw
                  : sp->vault_editing_new_pw2 ? sp->vault_new_pw2 : NULL;
        if (buf && codepoint >= 32 && codepoint < 127) {
            i32 flen = (i32)strlen(buf);
            TextEdit e = app_edit_ex(app, buf, &flen,
                                     (i32)sizeof sp->vault_old_pw,
                                     TEXTEDIT_SECRET);
            textedit_insert_cp(&e, NULL, codepoint);
            app_edit_done(app, flen);
            return true;
        }
    }

    return false;
}

bool settings_handle_key(SettingsPanel *sp, struct AppState *app, u32 key, u32 mods) {
    if (!sp->open) return false;

    if (sp->active_tab == SETTINGS_TAB_BROWSER && sp->browser_rule_editing) {
        if (key == KEY_ENTER || key == 13) { settings_browser_commit(sp); return true; }
        if (key == KEY_ESCAPE || key == 27) {
            sp->browser_rule_editing = false;
            sp->browser_rule_index = -1;
            sp->browser_rule_buf[0] = '\0';
            sp->browser_rule_status[0] = '\0';
            sp->browser_rule_status_error = false;
            return true;
        }
        i32 len = (i32)strlen(sp->browser_rule_buf);
        TextEdit e = app_edit(app, sp->browser_rule_buf, &len, sizeof sp->browser_rule_buf);
        textedit_key(&e, app ? &app->te_undo : NULL, key, mods);
        app_edit_done(app, len);
        return true;
    }

    if (sp->translate_lang_picker != SETTINGS_LANG_PICKER_CLOSED &&
        (key == KEY_ESCAPE || key == 27)) {
        sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
        return true;
    }
    /* The Model menu has the keyboard while it is open: the arrows and Enter
     * are its rows', Escape its own, and with a search field the rest is the
     * field's. What it does not take (Cmd+1..9) goes on to the panel. */
    if (sp->translate_model_picker_open && sp->active_tab == SETTINGS_TAB_TRANSLATE) {
        if (settings_model_menu_key(sp, app, key, mods)) return true;
    } else if (sp->translate_model_picker_open && (key == KEY_ESCAPE || key == 27)) {
        settings_model_menu_close(sp);
        return true;
    }
    if (sp->voice_lang_picker_open && (key == KEY_ESCAPE || key == 27)) {
        sp->voice_lang_picker_open = false;
        return true;
    }
    if (sp->voice_mic_picker_open && (key == KEY_ESCAPE || key == 27)) {
        sp->voice_mic_picker_open = false;
        return true;
    }

    /* API key / base URL / model inline editors (Translate and Voice share
     * them). Enter commits into config, Escape cancels, Cmd+V pastes (keys are
     * unpleasant to type), Tab is swallowed so it can't switch Settings tabs
     * mid-edit. */
    if (settings_api_editing(sp)) {
        char *buf; usize bcap;
        if (sp->editing_api_key)        { buf = sp->api_key_buf;   bcap = sizeof(sp->api_key_buf); }
        else if (sp->editing_api_url)   { buf = sp->api_url_buf;   bcap = sizeof(sp->api_url_buf); }
        else                            { buf = sp->api_model_buf; bcap = sizeof(sp->api_model_buf); }
        {
            /* Full editing (caret, word/line deletion, selection, undo,
             * clipboard). The API key is a secret: no undo ring and Cmd+C/X
             * refused. Paste still lands — keys are unpleasant to type. */
            i32 flen = (i32)strlen(buf);
            TextEdit e = app_edit_ex(app, buf, &flen, (i32)bcap,
                                     sp->editing_api_key ? TEXTEDIT_SECRET : 0);
            if (textedit_key(&e, sp->editing_api_key || !app ? NULL : &app->te_undo,
                             (i32)key, mods)) {
                /* config.json is written with a raw fprintf("%s") and the
                 * value is also spliced into a curl config, so a pasted '"'
                 * or '\\' would break both. Strip them after the edit rather
                 * than teaching textedit about this field's grammar. */
                char *w = buf;
                for (char *rp = buf; *rp; rp++)
                    if (*rp != '"' && *rp != '\\') *w++ = *rp;
                *w = '\0';
                app_edit_done(app, (i32)strlen(buf));
                return true;
            }
        }
        if (key == 13) {   /* enter — commit */
            settings_commit_api_editors(sp);
            return true;
        }
        if (key == 27 || key == KEY_ESCAPE) {   /* escape — cancel */
            settings_clear_focus(sp);
            return true;
        }
        if (key == KEY_TAB) return true;
    }

    /* Tab switching shortcuts. ⌘1..⌘7 land on a specific tab the same way
     * macOS Settings / browsers do; bare Tab / Shift+Tab cycles. Both reset
     * scroll + close any open dropdowns so the new tab opens fresh. Skipped
     * inside any text-input mode so Tab keeps its in-field meaning. */
    bool input_focused = (sp->active_tab == SETTINGS_TAB_KEYS && sp->rebinding)
                       || settings_api_editing(sp)
                       || (sp->active_tab == SETTINGS_TAB_VAULT &&
                           (sp->vault_editing_old_pw || sp->vault_editing_new_pw || sp->vault_editing_new_pw2));
    if (!input_focused) {
        SettingsTab want = sp->active_tab;
        bool change = false;
        if ((mods & MOD_APP) && key >= KEY_1 && key <= KEY_9) {
            i32 idx = (i32)(key - KEY_1);
            if (idx < SETTINGS_TAB_COUNT) { want = (SettingsTab)idx; change = true; }
        } else if (key == KEY_TAB &&
                   !(mods & (MOD_SUPER | MOD_CTRL | MOD_ALT))) {
            i32 dir = (mods & MOD_SHIFT) ? -1 : 1;
            i32 next = (settings_nav_index(sp->active_tab) + dir + SETTINGS_TAB_COUNT)
                       % SETTINGS_TAB_COUNT;
            want = settings_nav_order[next];
            change = true;
        }
        if (change && want != sp->active_tab) settings_select_tab(sp, want);
        if (change) return true;
    }

    if (settings_capture_binding(sp, key, mods)) return true;

    if (sp->active_tab == SETTINGS_TAB_VAULT) {
        char *buf = sp->vault_editing_old_pw ? sp->vault_old_pw
                  : sp->vault_editing_new_pw ? sp->vault_new_pw
                  : sp->vault_editing_new_pw2 ? sp->vault_new_pw2 : NULL;
        if (buf) {
            if (key == KEY_TAB) {
                bool old = sp->vault_editing_old_pw, next = sp->vault_editing_new_pw;
                sp->vault_editing_old_pw = !old && !next;
                sp->vault_editing_new_pw = old;
                sp->vault_editing_new_pw2 = next;
                return true;
            }
            if (key == KEY_ENTER) { settings_vault_submit(sp); return true; }
            i32 len = (i32)strlen(buf);
            TextEdit e = app_edit_ex(app, buf, &len, sizeof sp->vault_old_pw, TEXTEDIT_SECRET);
            if (textedit_key(&e, NULL, key, mods)) {
                app_edit_done(app, len);
                return true;
            }
        }
    }

    return false;
}

/* Place the body scroll so the thumb's top sits at pointer-y minus the grab
 * offset recorded on press — i.e. the thumb stays glued to the cursor instead
 * of jumping under it. Track geometry is whatever the last render published. */
static void settings_scrollbar_scrub(SettingsPanel *sp, f32 y) {
    f32 travel = sp->sb_track_h - sp->sb_thumb_h;
    if (travel <= 0.0f || sp->max_scroll <= 0) { sp->scroll_y = 0; return; }
    f32 frac = (y - sp->sb_drag_grab - sp->sb_track_y) / travel;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    sp->scroll_y = (i32)(frac * (f32)sp->max_scroll + 0.5f);
    sp->scroll_accum = 0.0f;   /* a wheel remainder would fight the drag */
}

bool settings_handle_drag(SettingsPanel *sp, f32 x, f32 y) {
    if (!sp || !sp->open) return false;
    if (sp->sb_dragging) { settings_scrollbar_scrub(sp, y); return true; }
    if (sp->drag_slider == SETTINGS_SLIDER_NONE) return false;
    if (sp->drag_track_w <= 0.0f || !sp->config) return false;
    /* The knob radius is a function of the control height the renderer used;
     * settings_slider_t_at derives it the same way, so the value under the
     * cursor stays the value drawn there. */
    f32 h = sp->drag_track_h > 0.0f ? sp->drag_track_h : 24.0f;
    f32 t = settings_slider_t_at(x, sp->drag_track_x, sp->drag_track_w, h);
    if (sp->drag_slider == SETTINGS_SLIDER_OPACITY) {
        sp->config->opacity = SETTINGS_OPACITY_MIN + t * (1.0f - SETTINGS_OPACITY_MIN);
        sp->bg_dirty = true;
    } else if (sp->drag_slider == SETTINGS_SLIDER_FADE) {
        sp->config->background_opacity = t;
        sp->bg_dirty = true;
    }
    return true;
}

void settings_end_drag(SettingsPanel *sp) {
    if (!sp) return;
    sp->drag_slider = SETTINGS_SLIDER_NONE;
    sp->sb_dragging = false;
}

bool settings_handle_scroll(SettingsPanel *sp, f32 dy, bool precise, f32 dpi) {
    if (!sp->open) return false;

    SettingsRect nav = {sp->nav_clip_x, sp->nav_clip_y, sp->nav_clip_w, sp->nav_clip_h};
    if (settings_rect_contains(nav, sp->ptr_x, sp->ptr_y)) {
        f32 delta = precise ? -dy : -dy * 32 * dpi;
        sp->nav_scroll = CLAMP(sp->nav_scroll + delta, 0, sp->nav_max_scroll);
        return true;
    }

    /* While the Translate Model menu is open the wheel moves its window, not
     * the panel under it — a live list is longer than the window, and a body
     * that scrolled would carry the chip the menu hangs from away. */
    if (sp->active_tab == SETTINGS_TAB_TRANSLATE &&
        sp->translate_model_picker_open &&
        sp->config->translate.backend != TRANSLATE_BACKEND_LOCAL) {
        SettingsModelRows rows;
        settings_translate_model_rows(sp, &rows);
        /* On the panel, not a static: two windows sharing one remainder
         * stole each other's sub-tick pixels. One row per row of travel. */
        const f32 step_px = CTX_MENU_ROW_H_PT * (dpi > 0.0f ? dpi : 1.0f);
        sp->model_menu_accum += precise ? -dy / step_px : -dy;
        i32 step = (i32)sp->model_menu_accum;
        sp->model_menu_accum -= (f32)step;
        sp->translate_model_menu_scroll = settings_model_menu_clamp_scroll(
            sp->translate_model_menu_scroll + step, rows.n - (rows.pinned ? 1 : 0),
            settings_model_menu_window(sp));
        return true;
    }

    /* Same for the voice language picker: 100 languages are four screens of
     * rows, so the wheel belongs to the popup while it is open. */
    if (sp->active_tab == SETTINGS_TAB_VOICE && sp->voice_lang_picker_open) {
        const f32 step_px = 24.0f * (dpi > 0.0f ? dpi : 1.0f);
        sp->voice_lang_menu_accum += precise ? -dy / step_px : -dy;
        i32 step = (i32)sp->voice_lang_menu_accum;
        sp->voice_lang_menu_accum -= (f32)step;
        sp->voice_lang_menu_scroll += step;
        sp->voice_lang_menu_scroll = settings_voice_lang_clamp_scroll(sp);
        return true;
    }

    sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;

    /* scroll_y is in "half-cell" units (render path: scroll_off = scroll_y * ch
     * * 0.5). One half-cell ≈ 8 * dpi pixels at the default 16-pt font. macOS
     * NSEvent gives pixel deltas for trackpad/Magic Mouse (precise=true) and
     * line-unit deltas for legacy wheels; treating the pixel delta as raw
     * half-cell ticks would make the panel jump on any trackpad swipe. Convert
     * both into half-cell ticks and accumulate the fractional remainder so slow
     * trackpad pans don't get rounded away. */
    /* On the panel, not a static: two windows would drain the same
     * fractional remainder. */
    const f32 step_px = 8.0f * (dpi > 0.0f ? dpi : 1.0f);
    f32 ticks_f;
    if (precise) {
        ticks_f = -dy / step_px;
    } else {
        /* One wheel click ≈ 3 lines. line_h ≈ 16 * dpi → 4 half-cell ticks. */
        ticks_f = -dy * 4.0f;
    }
    sp->scroll_accum += ticks_f;
    i32 step = (i32)sp->scroll_accum;
    sp->scroll_accum -= (f32)step;
    sp->scroll_y += step;
    if (sp->scroll_y < 0) sp->scroll_y = 0;
    if (sp->scroll_y > sp->max_scroll) sp->scroll_y = sp->max_scroll;
    return true;
}
