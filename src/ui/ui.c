/*
 * Liu — UI system with multi-pass rendering
 */
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/hittest.h"
#include "ui/palette.h"
#include "ui/sites_ui.h"
#include "ui/anim.h"
#include "ui/chrome_palette.h"
#include "ui/icon.h"
#include "ui/agent_icons.h"
#include "ui/tab_trash.h"
#include "core/agent_detect.h"
#include "core/memory.h"
#include "core/string_utils.h"
#include "core/utf8.h"
#include "core/keybind.h"
#include "core/cmd_history.h"
#include "translate/translate_local.h"
#include "translate/model_catalog.h"  /* model_catalog_shutdown on teardown */
#include "translate/model_download.h" /* model_download_shutdown on teardown */
#include "terminal/bidi.h"
#include "ssh/known_hosts.h"
#include "ssh/sftp.h"    /* transfer progress overlay accessors */
#include "vault/vault.h"
#include "vault/crypto.h"
#include "platform/platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#ifndef PLATFORM_WIN32
#include <sys/wait.h>
#endif
#include <fcntl.h>
#include <time.h>
#include "cJSON.h"

#ifdef PLATFORM_MACOS
    #include <mach/mach.h>
    #include <mach/mach_time.h>
    #include <sys/resource.h>
#elif defined(PLATFORM_LINUX)
    #include <sys/resource.h>
#endif

/* Query current process resource usage */
static void query_resources(f32 *cpu_pct, f32 *mem_mb) {
#ifdef PLATFORM_MACOS
    /* Memory: use phys_footprint via TASK_VM_INFO — this is the same metric
     * Activity Monitor shows under "Memory", which is what users compare the
     * HUD against. MACH_TASK_BASIC_INFO.resident_size (the previous reading)
     * excludes IOSurface / Metal-private allocations, so on a Metal-backed
     * app like Liu it under-reports by an order of magnitude and ends up the
     * same ballpark as plain CLI daemons like liu-notify — confusing users
     * into thinking the HUD is reading the wrong process. */
    task_vm_info_data_t vm_info;
    mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  (task_info_t)&vm_info, &vm_count) == KERN_SUCCESS) {
        *mem_mb = (f32)vm_info.phys_footprint / (1024.0f * 1024.0f);
    } else {
        /* Fallback to resident_size if TASK_VM_INFO isn't available
         * (older macOS / sandbox restrictions). */
        mach_task_basic_info_data_t info;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                      (task_info_t)&info, &count) == KERN_SUCCESS) {
            *mem_mb = (f32)info.resident_size / (1024.0f * 1024.0f);
        }
    }

    /* CPU: from thread times */
    thread_array_t threads;
    mach_msg_type_number_t thread_count;
    if (task_threads(mach_task_self(), &threads, &thread_count) == KERN_SUCCESS) {
        f64 total_cpu = 0;
        for (mach_msg_type_number_t i = 0; i < thread_count; i++) {
            thread_basic_info_data_t tinfo;
            mach_msg_type_number_t tcount = THREAD_BASIC_INFO_COUNT;
            if (thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&tinfo, &tcount) == KERN_SUCCESS) {
                total_cpu += tinfo.cpu_usage / (f64)TH_USAGE_SCALE * 100.0;
            }
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)threads, thread_count * sizeof(thread_t));
        *cpu_pct = (f32)total_cpu;
    }
#elif defined(PLATFORM_LINUX)
    /* Memory from /proc/self/status */
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                i64 kb = 0;
                sscanf(line + 6, " %lld", &kb);
                *mem_mb = (f32)kb / 1024.0f;
                break;
            }
        }
        fclose(f);
    }
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        *cpu_pct = 0; /* simplified */
    }
#else
    *cpu_pct = 0;
    *mem_mb = 0;
#endif
}

/* =========================================================================
 * Init / Destroy
 * ========================================================================= */

/* Global app pointer for callbacks that can't carry AppState through userdata */
static AppState *g_bell_app = NULL;

/* Terminal response callback — sends DSR/DA/DECRQM responses back to PTY */
static void app_terminal_response(Terminal *t, const u8 *data, i32 len, void *ud) {
    (void)t;
    Session *s = (Session *)ud;
    if (s) session_write(s, data, len);
}

/* Long-running command notification callback */
static void app_long_command_cb(Terminal *t, f64 duration, i32 exit_code, void *ud) {
    (void)t;
    AppState *app = (AppState *)ud;
    if (!app) return;

    /* Only notify if duration exceeds the configured threshold */
    if (duration < (f64)app->config.notify_command_threshold) return;

    /* Format toast message */
    if (exit_code == 0) {
        snprintf(app->toast_message, sizeof(app->toast_message),
                 "Command finished in %.1fs", duration);
    } else {
        snprintf(app->toast_message, sizeof(app->toast_message),
                 "Command failed (exit %d) after %.1fs", exit_code, duration);
    }
    app->toast_start_time = platform_time_sec();

    /* If app is not focused, send system notification + dock bounce */
    if (!app->window_focused) {
        char body[256];
        if (exit_code == 0)
            snprintf(body, sizeof(body), "Completed in %.1fs", duration);
        else
            snprintf(body, sizeof(body), "Failed (exit %d) after %.1fs", exit_code, duration);
        platform_post_notification("Command finished", body);
        platform_request_attention();
    }
}

void app_show_toast(AppState *app, const char *message) {
    if (!app || !message || !message[0]) return;
    snprintf(app->toast_message, sizeof(app->toast_message), "%s", message);
    app->toast_start_time = platform_time_sec();
}

/* Defined later in this file (near the other chrome draw helpers). */
void draw_text_ex(Renderer *r, const char *text, f32 x, f32 y, Color fg,
                  i32 max_chars, f32 step);

void app_show_crash_banner(AppState *app, const char *reason, const char *path) {
    if (!app) return;
    app->crash_banner_active = true;
    snprintf(app->crash_banner_reason, sizeof(app->crash_banner_reason),
             "%s", (reason && reason[0]) ? reason : "bilinmeyen hata");
    snprintf(app->crash_banner_path, sizeof(app->crash_banner_path),
             "%s", path ? path : "");
}

/* Persistent top banner shown after a crash. Drawn on top of everything, just
 * under the tab bar; clears on Esc or a click (handled in main.c via the rect
 * stashed in app->crash_banner_*). */
void render_crash_banner(AppState *app) {
    if (!app || !app->crash_banner_active) return;
    Renderer *r = &app->renderer;
    f32 dpi = app->dpi_scale;
    f32 cw = 8.0f * dpi, ch = 16.0f * dpi;
    renderer_set_ui_scale(r, cw, ch);

    f32 pad   = 12.0f * dpi;
    f32 margin = 10.0f * dpi;
    f32 x = margin;
    f32 w = (f32)app->fb_width - 2.0f * margin;
    f32 y = app->tab_bar_height + 8.0f * dpi;
    f32 h = ch * 2.0f + pad * 2.0f + 4.0f * dpi;

    /* Warning chrome — fixed red so it reads regardless of theme. */
    Color bg     = {0.42f, 0.10f, 0.10f, 0.97f};
    Color stripe = {0.95f, 0.35f, 0.30f, 1.00f};
    Color title  = {1.00f, 0.90f, 0.88f, 1.00f};
    Color sub    = {0.95f, 0.78f, 0.74f, 0.95f};

    renderer_draw_rrect_simple(r, x, y, w, h, bg, 8.0f * dpi);
    renderer_draw_rect(r, x, y, 3.0f * dpi, h, stripe);   /* left accent bar */

    f32 tx = x + pad + 6.0f * dpi;
    f32 ty = y + pad;
    i32 maxc = (i32)((w - pad * 2.0f - 6.0f * dpi) / cw);
    if (maxc < 8) maxc = 8;

    char line1[320];
    snprintf(line1, sizeof(line1), "\xE2\x9A\xA0  Liu \xC3\xB6nceki oturumda \xC3\xA7\xC3\xB6kt\xC3\xBc: %s",
             app->crash_banner_reason);
    draw_text_ex(r, line1, tx, ty, title, maxc, cw);

    char line2[640];
    const char *fname = app->crash_banner_path;
    const char *slash = strrchr(fname, '/');
    if (slash) fname = slash + 1;
    snprintf(line2, sizeof(line2), "Rapor: crasherrors/%s     [Esc] veya t\xC4\xB1kla = kapat",
             fname);
    draw_text_ex(r, line2, tx, ty + ch + 4.0f * dpi, sub, maxc, cw);

    /* Stash the rect for click-to-dismiss hit testing. */
    app->crash_banner_x = x; app->crash_banner_y = y;
    app->crash_banner_w = w; app->crash_banner_h = h;

    renderer_reset_ui_scale(r);
}

/* Bell callback — fires when terminal receives BEL (0x07) */
static void app_bell_callback(Terminal *t, void *ud) {
    (void)ud;
    AppState *app = g_bell_app;
    if (!app) return;

    /* Find which tab owns this terminal */
    i32 tab_idx = -1;
    for (i32 i = 0; i < app->tab_count; i++) {
        if (app->tabs[i].terminal == t || app->tabs[i].terminal2 == t) {
            tab_idx = i;
            break;
        }
    }

    bool is_active_tab = (tab_idx == app->active_tab);
    bool app_focused = app->window_focused;

    /* Visual bell flash (only for active tab) */
    if (is_active_tab && app->config.visual_bell) {
        app->bell_flash_time = platform_time_sec();
    }

    /* Audible bell */
    if (app->config.audible_bell) {
        platform_play_bell();
    }

    /* Mark tab as having a pending bell (for non-active tabs) */
    if (tab_idx >= 0 && !is_active_tab) {
        app->tabs[tab_idx].has_bell = true;
    }

    /* Dock badge + bounce + notification for background */
    if (!app_focused) {
        platform_set_dock_badge("!");
        platform_request_attention();

        /* Post notification with tab title */
        const char *tab_title = (tab_idx >= 0) ? app->tabs[tab_idx].title : "Terminal";
        char body[256];
        snprintf(body, sizeof(body), "Bell in tab: %s", tab_title);
        platform_post_notification("Liu", body);
    } else if (!is_active_tab) {
        /* App focused but bell on non-active tab: set dock badge */
        platform_set_dock_badge("!");
    }
}

/* OSC 52 clipboard callbacks. The write path is gated by config — remote
 * shells (or `cat` of a crafted file) can otherwise silently replace the
 * system clipboard with attacker-controlled bytes. The default-off
 * posture common to terminal emulators. */
static void app_terminal_clipboard_set(Terminal *t, const char *text, void *ud) {
    (void)t; (void)ud;
    if (!text) return;
    /* Fail closed: if the app singleton isn't wired yet, refuse. This is
     * a security gate, not a convenience. */
    if (!g_bell_app || !g_bell_app->config.allow_osc52_write) return;
    platform_clipboard_set(text);
}

static const char *app_terminal_clipboard_get(Terminal *t, void *ud) {
    (void)t; (void)ud;
    return platform_clipboard_get();
}

static void app_sanitize_history_label(const char *src, char *dst, usize dst_size) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src) return;

    usize di = 0;
    bool in_space = false;
    for (const unsigned char *p = (const unsigned char *)src; *p && di + 1 < dst_size; p++) {
        if (isspace(*p)) {
            if (di == 0 || in_space) continue;
            dst[di++] = ' ';
            in_space = true;
            continue;
        }
        dst[di++] = (char)*p;
        in_space = false;
    }

    while (di > 0 && dst[di - 1] == ' ') di--;
    dst[di] = '\0';
}

static bool app_history_has_raw(const AppState *app, const char *command) {
    if (!app || !app->cmd_history_raw || !command || !command[0]) return false;
    for (i32 i = 0; i < app->cmd_history_count; i++) {
        if (strcmp(app->cmd_history_raw[i], command) == 0) return true;
    }
    return false;
}

/* Append a command at the next free slot (older than what's already there).
 * The popup is filled newest-first, so the first append lands at slot 0 (top)
 * and successive ones trail behind it. Dedups against entries already shown. */
static void app_history_append(AppState *app, const char *command) {
    if (!app || !command || !command[0] || app->cmd_history_count >= 5) return;
    if (!app->cmd_history_raw) {
        app->cmd_history_raw = calloc(5, sizeof(*app->cmd_history_raw));
        if (!app->cmd_history_raw) return;
    }
    if (app_history_has_raw(app, command)) return;

    char label[512];
    app_sanitize_history_label(command, label, sizeof(label));
    if (!label[0]) return;

    i32 i = app->cmd_history_count;
    snprintf(app->cmd_history[i], sizeof(app->cmd_history[i]), "%s", label);
    snprintf(app->cmd_history_raw[i], sizeof(app->cmd_history_raw[i]), "%s", command);
    app->cmd_history_count++;
}

const char *app_terminal_cwd(Terminal *t, Session *s) {
    if (t && t->cwd && t->cwd[0]) return t->cwd;
    if (s) {
        const char *probe = session_local_cwd(s);
        if (probe && probe[0]) return probe;
    }
    return NULL;
}

static void app_command_finished_cb(Terminal *t, const char *command, i32 exit_code, void *ud) {
    (void)exit_code;
    (void)ud;
    if (!t || !command || !command[0]) return;
    /* Record into the folder the command ran in, so suggestions stay scoped to
     * the project. Only local PTYs have a meaningful local filesystem path —
     * an SSH session's cwd is remote — no folder history applies. */
    Session *s = (Session *)t->userdata;
    if (!s || session_type(s) != SESSION_LOCAL) return;
    const char *cwd = app_terminal_cwd(t, s);
    if (cwd && cwd[0]) cmd_history_record(cwd, command);
}

static void app_configure_terminal_callbacks(AppState *app, Terminal *term, Session *session) {
    if (!app || !term) return;

    term->on_response = app_terminal_response;
    term->on_bell = app_bell_callback;
    term->on_clipboard_set = app_terminal_clipboard_set;
    term->on_clipboard_get = app_terminal_clipboard_get;
    term->on_long_command = app_long_command_cb;
    term->on_command_finished = app_command_finished_cb;
    term->userdata = session;
    term->long_cmd_userdata = app;
    term->command_userdata = app;
}

static void app_vault_path(char *out, usize out_size) {
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%s/vault.db", config_user_dir());
}

bool app_init(AppState *app, i32 width, i32 height, f32 dpi_scale) {
    memset(app, 0, sizeof(*app));
    /* Spin up the background tear-down worker before any tab is opened so
     * the first close hits the fast path immediately. */
    tab_trash_init();
    app->tab_drag_target_group = -1;
    app->chip_rename_group_index = -1;
    app->pane_drag_src_pane = -1;
    app->pane_drag_hover_pane = -1;
    app->pane_drag_drop_zone = 0;
    app->pane_drag_tab_index = -1;
    app->group_ctx_menu_group_index = -1;
    app->dpi_scale = dpi_scale;
    app->config = config_default();
    g_bell_app = app; /* set global for bell callback */
    app->config.theme = theme_get_by_name(app->config.theme_name);

    app->tab_bar_height   = app->config.tab_height * dpi_scale;
    app->status_bar_height = 22.0f * dpi_scale;
    app->padding          = app->config.padding * dpi_scale;
    app->sidebar_width    = 0;
    app->sidebar_visible  = false;
    app->sidebar_mode     = SIDEBAR_HOSTS;
    app->window_focused   = true;
    app->toast_start_time = 0;
    app->cursor_animating = false;
    app->translate_stdout_fd = -1;

    /* Rarely-used bulky buffers stay NULL until first access. Total deferred:
     *   cmd_history_raw   ~20 KB  (Option+Up popup)
     *   passphrase_cache  ~12 KB  (SSH key with passphrase)
     *   key_list          ~32 KB  (Key Manager overlay)
     * Each accessor lazy-allocates via ensure-helpers below.  */
    app->cmd_history_raw       = NULL;
    app->passphrase_cache      = NULL;
    app->passphrase_cache_cap  = 16;      /* target, not "allocated"      */
    app->key_list              = NULL;
    app->key_list_cap          = 32;      /* target, not "allocated"      */

    /* Filebrowser drag / ctx-menu sentinels (0 would collide with entry 0). */
    app->fb_drag_src_entry     = -1;
    app->fb_ctx_menu_entry     = -1;
    app->fb_ctx_menu_selected  = -1;
    app->fb_prompt_index       = -1;

    /* Try loading user config */
    config_load(&app->config, config_file_path());
    app->config.theme = theme_get_by_name(app->config.theme_name);

    /* Sites / dev-server manager: build the runtime registry from config. */
    sites_manager_init(&app->site_mgr, 120, 40, app->config.scrollback_lines);
    sites_manager_load_from_config(&app->site_mgr, &app->config);
    sites_ui_init(app);

    /* Open persistent vault beside the config file. */
    {
        char vault_path[1024];
        app_vault_path(vault_path, sizeof(vault_path));
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", vault_path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir(dir, 0755);
        }
        app->vault = vault_open(vault_path);
    }

    if (!renderer_init(&app->renderer, dpi_scale)) {
        fprintf(stderr, "Failed to init renderer\n");
        return false;
    }

    /* Set user-configured fallback fonts (loaded automatically during font_atlas_create) */
    {
        const char *fb_paths[MAX_CONFIG_FALLBACK_FONTS];
        i32 fb_count = 0;
        for (i32 i = 0; i < app->config.fallback_font_count && i < MAX_CONFIG_FALLBACK_FONTS; i++) {
            if (app->config.fallback_fonts[i][0])
                fb_paths[fb_count++] = app->config.fallback_fonts[i];
        }
        if (fb_count > 0) font_set_user_fallback_config(fb_paths, fb_count);
    }

    /* Load font from config path, with fallbacks */
    char local_fonts[16][512];
    i32 local_font_count = 0;
    const char *local_font_files[] = {
        "JetBrainsMono-Regular.ttf",
        "FiraCode-Regular.ttf",
        "CascadiaCode.ttf",
        "Hack-Regular.ttf",
        "SourceCodePro-Regular.ttf",
        "VictorMono-Regular.ttf",
        "IBMPlexMono-Regular.ttf",
        "Inconsolata-Regular.ttf",
        "RobotoMono-Regular.ttf",
        "SpaceMono-Regular.ttf",
        "UbuntuMono-Regular.ttf",
        "AnonymousPro-Regular.ttf",
        "Cousine-Regular.ttf",
        "PTMono-Regular.ttf",
        "Monaco.ttf",
        "Menlo.ttc",
        "DejaVuSansMono.ttf",
        "Consolas.ttf",
        NULL
    };
    for (i32 i = 0; local_font_files[i] && local_font_count < 16; i++) {
        liu_path_join(local_fonts[local_font_count], sizeof(local_fonts[local_font_count]),
                      font_user_dir(), local_font_files[i]);
        local_font_count++;
    }

    const char *paths[] = {
        app->config.font_path,
#ifdef PLATFORM_MACOS
        "/System/Library/Fonts/Monaco.ttf",
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Courier.ttc",
#elif defined(PLATFORM_LINUX)
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
#endif
        NULL
    };

    bool font_ok = false;
    if (app->config.font_path[0] &&
        font_atlas_create(&app->renderer.font, app->config.font_path, app->config.font_size, dpi_scale, app->config.font_weight)) {
        font_ok = true;
    }
    for (i32 i = 0; !font_ok && i < local_font_count; i++) {
        if (strcmp(local_fonts[i], app->config.font_path) == 0) continue;
        if (font_atlas_create(&app->renderer.font, local_fonts[i], app->config.font_size, dpi_scale, app->config.font_weight)) {
            snprintf(app->config.font_path, sizeof(app->config.font_path), "%s", local_fonts[i]);
            font_ok = true;
            break;
        }
    }
    for (int i = 0; !font_ok && paths[i]; i++) {
        if (strcmp(paths[i], app->config.font_path) == 0) continue;
        if (font_atlas_create(&app->renderer.font, paths[i], app->config.font_size, dpi_scale, app->config.font_weight)) {
            snprintf(app->config.font_path, sizeof(app->config.font_path), "%s", paths[i]);
            font_ok = true;
            break;
        }
    }
    if (!font_ok) {
        fprintf(stderr, "No monospace font found\n");
        return false;
    }

    /* Init keybindings, settings, file browser */
    keybind_init_defaults(&app->keybinds);
    /* Try loading custom keybindings */
    {
        char kb_path[1024];
        snprintf(kb_path, sizeof(kb_path), "%s/keybindings.json", config_user_dir());
        keybind_load(&app->keybinds, kb_path);
    }
    settings_init(&app->settings, &app->config, &app->keybinds, app->vault);
    updater_init(&app->updater);
    fb_init(&app->filebrowser);
    if (app->vault) {
        extern void sidebar_load_hosts(Vault *v);
        sidebar_load_hosts(app->vault);
    }

    /* Apply cell spacing from config */
    app->renderer.font.cell_width  *= app->config.cell_width_scale;
    app->renderer.font.cell_height *= app->config.cell_height_scale;

    /* Apply theme colors to ANSI palette */
    app_apply_config(app);

    app_update_layout(app, width, height, (i32)(width * dpi_scale), (i32)(height * dpi_scale));
    return true;
}

static void term_render_cache_free(TermRenderCache *c);
static void app_trim_inactive_render_caches(AppState *app);
static void tab_sleep_snapshot_reset(TabSleepSnapshot **snap);
static void closed_tab_clear(ClosedTabInfo *info);
static void session_cd_to(Session *s, const char *cwd);

/* =========================================================================
 * Passphrase cache — in-memory only, never written to disk
 * ========================================================================= */

/* Lazy allocate the passphrase cache pool on first use. Keeps ~12 KB off
 * idle RSS for the (common) case where no SSH key has a passphrase. */
static bool ensure_passphrase_cache(AppState *app) {
    if (!app) return false;
    if (app->passphrase_cache) return true;
    if (app->passphrase_cache_cap <= 0) app->passphrase_cache_cap = 16;
    app->passphrase_cache = calloc((usize)app->passphrase_cache_cap,
                                   sizeof(*app->passphrase_cache));
    return app->passphrase_cache != NULL;
}

const char *passphrase_cache_lookup(AppState *app, const char *key_path) {
    if (!app || !app->passphrase_cache || !key_path) return NULL;
    for (i32 i = 0; i < app->passphrase_cache_count; i++) {
        if (strcmp(app->passphrase_cache[i].path, key_path) == 0)
            return app->passphrase_cache[i].passphrase;
    }
    return NULL;
}

void passphrase_cache_store(AppState *app, const char *key_path, const char *passphrase) {
    if (!app || !key_path || !passphrase) return;
    if (!ensure_passphrase_cache(app)) return;
    /* Check if already cached — scrub old secret, then update in place */
    for (i32 i = 0; i < app->passphrase_cache_count; i++) {
        if (strcmp(app->passphrase_cache[i].path, key_path) == 0) {
            secure_zero(app->passphrase_cache[i].passphrase,
                        sizeof(app->passphrase_cache[i].passphrase));
            snprintf(app->passphrase_cache[i].passphrase,
                     sizeof(app->passphrase_cache[i].passphrase), "%s", passphrase);
            return;
        }
    }
    /* Add new entry */
    if (app->passphrase_cache_count < app->passphrase_cache_cap) {
        i32 idx = app->passphrase_cache_count++;
        snprintf(app->passphrase_cache[idx].path,
                 sizeof(app->passphrase_cache[idx].path), "%s", key_path);
        snprintf(app->passphrase_cache[idx].passphrase,
                 sizeof(app->passphrase_cache[idx].passphrase), "%s", passphrase);
    }
}

static void passphrase_cache_clear(AppState *app) {
    if (!app || !app->passphrase_cache) return;
    /* secure_zero (volatile) so the compiler cannot drop the scrub on destroy. */
    for (i32 i = 0; i < app->passphrase_cache_count; i++) {
        secure_zero(app->passphrase_cache[i].passphrase,
                    sizeof(app->passphrase_cache[i].passphrase));
    }
    app->passphrase_cache_count = 0;
}

typedef struct {
    f32 x, y, w, h;
} PaneRect;

#define SPLIT_MOVE_DUR 0.28

static void app_terminal_content_rect(AppState *app,
                                      f32 *ox, f32 *oy,
                                      f32 *total_w, f32 *total_h) {
    f32 x = app->sidebar_width + app->padding;
    /* No +app->padding on Y: the canonical render frame (render_terminal,
     * app_update_layout, the pane + IME paths) dropped the top padding in
     * ef7b3612, but this helper kept it — so the rects baked into
     * split_move_from[] lived ~app->padding px LOWER than the render-loop
     * targets, making a freshly-split source pane start pushed down and slide
     * up over SPLIT_MOVE_DUR ("pushed down then snaps back"). Match the renderer
     * exactly; total_h auto-grows by the same padding via the formula below. */
    f32 y = app->tab_bar_height +
            app->config.style.terminal_top_gap * app->dpi_scale;
    if (ox) *ox = x;
    if (oy) *oy = y;
    if (total_w) *total_w = (f32)app->fb_width - x - app->padding;
    if (total_h) *total_h = (f32)app->fb_height - y -
                            app->status_bar_height - app->padding;
}

static Terminal *tab_pane_terminal(const Tab *tab, i32 pane) {
    if (!tab) return NULL;
    if (pane == 0) return tab->terminal;
    if (pane == 1) return tab->terminal2;
    if (pane >= 2 && pane < MAX_SPLIT_PANES) return tab->extra_terminals[pane - 2];
    return NULL;
}

static Session *tab_pane_session(const Tab *tab, i32 pane) {
    if (!tab) return NULL;
    if (pane == 0) return tab->session;
    if (pane == 1) return tab->session2;
    if (pane >= 2 && pane < MAX_SPLIT_PANES) return tab->extra_sessions[pane - 2];
    return NULL;
}

static TermRenderCache *tab_pane_cache(Tab *tab, i32 pane) {
    if (!tab) return NULL;
    if (pane == 0) return &tab->cache1;
    if (pane == 1) return &tab->cache2;
    if (pane >= 2 && pane < MAX_SPLIT_PANES) return &tab->extra_caches[pane - 2];
    return NULL;
}

static void tab_set_pane_refs(Tab *tab, i32 pane, Terminal *term, Session *session) {
    if (!tab || pane < 0 || pane >= MAX_SPLIT_PANES) return;
    if (pane == 0) {
        tab->terminal = term;
        tab->session = session;
    } else if (pane == 1) {
        tab->terminal2 = term;
        tab->session2 = session;
    } else {
        tab->extra_terminals[pane - 2] = term;
        tab->extra_sessions[pane - 2] = session;
    }
}

static i32 tab_split_pane_count(const Tab *tab) {
    if (!tab || tab->split == SPLIT_NONE) return 1;
    i32 count = tab->split_pane_count;
    if (count < 2) count = 2;
    if (count > MAX_SPLIT_PANES) count = MAX_SPLIT_PANES;
    return count;
}

static void tab_split_tree_reset(Tab *tab) {
    if (!tab) return;
    memset(tab->split_nodes, 0, sizeof(tab->split_nodes));
    tab->split_root = -1;
    tab->split_next_node = 0;
}

static i32 tab_split_alloc_node(Tab *tab) {
    if (!tab) return -1;
    /* Reuse a freed slot below the high-water mark first; only grow the
     * high-water when none is free. split_next_node alone is monotonic, so
     * without this, repeated split/close cycles exhaust the node pool even
     * though collapse marks closed nodes used=false. */
    i32 idx = -1;
    for (i32 i = 0; i < tab->split_next_node; i++) {
        if (!tab->split_nodes[i].used) { idx = i; break; }
    }
    if (idx < 0) {
        if (tab->split_next_node >= MAX_SPLIT_NODES) return -1;
        idx = tab->split_next_node++;
    }
    memset(&tab->split_nodes[idx], 0, sizeof(tab->split_nodes[idx]));
    tab->split_nodes[idx].used = true;
    tab->split_nodes[idx].first = -1;
    tab->split_nodes[idx].second = -1;
    tab->split_nodes[idx].ratio = 0.5f;
    return idx;
}

static i32 tab_split_make_leaf(Tab *tab, i32 pane) {
    i32 idx = tab_split_alloc_node(tab);
    if (idx < 0) return -1;
    tab->split_nodes[idx].leaf = true;
    tab->split_nodes[idx].pane = pane;
    return idx;
}

static i32 tab_split_find_leaf_node(Tab *tab, i32 node, i32 pane) {
    if (!tab || node < 0 || node >= tab->split_next_node) return -1;
    SplitLayoutNode *n = &tab->split_nodes[node];
    if (!n->used) return -1;
    if (n->leaf) return n->pane == pane ? node : -1;
    i32 found = tab_split_find_leaf_node(tab, n->first, pane);
    if (found >= 0) return found;
    return tab_split_find_leaf_node(tab, n->second, pane);
}

static i32 tab_split_find_parent_node(Tab *tab, i32 node, i32 child) {
    if (!tab || node < 0 || node >= tab->split_next_node) return -1;
    SplitLayoutNode *n = &tab->split_nodes[node];
    if (!n->used || n->leaf) return -1;
    if (n->first == child || n->second == child) return node;
    i32 found = tab_split_find_parent_node(tab, n->first, child);
    if (found >= 0) return found;
    return tab_split_find_parent_node(tab, n->second, child);
}

static void tab_split_replace_child(Tab *tab, i32 parent, i32 old_child, i32 new_child) {
    if (!tab || parent < 0 || parent >= tab->split_next_node) return;
    SplitLayoutNode *n = &tab->split_nodes[parent];
    if (n->first == old_child) n->first = new_child;
    else if (n->second == old_child) n->second = new_child;
}

static void tab_split_update_leaf_pane(Tab *tab, i32 node, i32 old_pane, i32 new_pane) {
    if (!tab || node < 0 || node >= tab->split_next_node) return;
    SplitLayoutNode *n = &tab->split_nodes[node];
    if (!n->used) return;
    if (n->leaf) {
        if (n->pane == old_pane) n->pane = new_pane;
        return;
    }
    tab_split_update_leaf_pane(tab, n->first, old_pane, new_pane);
    tab_split_update_leaf_pane(tab, n->second, old_pane, new_pane);
}

static void tab_split_init_tree(Tab *tab, SplitType dir, bool new_first) {
    tab_split_tree_reset(tab);
    i32 root = tab_split_alloc_node(tab);
    i32 a = tab_split_make_leaf(tab, new_first ? 1 : 0);
    i32 b = tab_split_make_leaf(tab, new_first ? 0 : 1);
    if (root < 0 || a < 0 || b < 0) {
        tab_split_tree_reset(tab);
        return;
    }
    tab->split_nodes[root].leaf = false;
    tab->split_nodes[root].split = dir == SPLIT_NONE ? SPLIT_H : dir;
    tab->split_nodes[root].ratio = 0.5f;
    tab->split_nodes[root].first = a;
    tab->split_nodes[root].second = b;
    tab->split_root = root;
}

static void tab_split_layout_node(const Tab *tab, i32 node, PaneRect r,
                                  f32 dpi, PaneRect out[MAX_SPLIT_PANES]) {
    if (!tab || node < 0 || node >= tab->split_next_node) return;
    const SplitLayoutNode *n = &tab->split_nodes[node];
    if (!n->used) return;
    if (n->leaf) {
        if (n->pane >= 0 && n->pane < MAX_SPLIT_PANES) out[n->pane] = r;
        return;
    }
    f32 div = 6.0f * dpi;   /* inter-pane gap: 2px + 2px line + 2px */
    f32 ratio = n->ratio;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    if (n->split == SPLIT_V) {
        f32 split = r.h * ratio;
        PaneRect first = {r.x, r.y, r.w, split - div * 0.5f};
        PaneRect second = {r.x, r.y + split + div * 0.5f,
                           r.w, r.h - split - div * 0.5f};
        tab_split_layout_node(tab, n->first, first, dpi, out);
        tab_split_layout_node(tab, n->second, second, dpi, out);
    } else {
        f32 split = r.w * ratio;
        PaneRect first = {r.x, r.y, split - div * 0.5f, r.h};
        PaneRect second = {r.x + split + div * 0.5f, r.y,
                           r.w - split - div * 0.5f, r.h};
        tab_split_layout_node(tab, n->first, first, dpi, out);
        tab_split_layout_node(tab, n->second, second, dpi, out);
    }
}

static void tab_split_layout_rects(const Tab *tab, f32 ox, f32 oy,
                                   f32 total_w, f32 total_h, f32 dpi,
                                   PaneRect rects[MAX_SPLIT_PANES]) {
    for (i32 i = 0; i < MAX_SPLIT_PANES; i++) rects[i] = (PaneRect){0};
    if (!tab || tab->split_root < 0) {
        rects[0] = (PaneRect){ox, oy, total_w, total_h};
        return;
    }
    tab_split_layout_node(tab, tab->split_root,
                          (PaneRect){ox, oy, total_w, total_h}, dpi, rects);
}

static void tab_split_start_move_anim(Tab *tab,
                                      const PaneRect old_rects[MAX_SPLIT_PANES],
                                      i32 source_pane,
                                      i32 new_pane) {
    if (!tab || !old_rects) return;
    for (i32 i = 0; i < MAX_SPLIT_PANES; i++) {
        tab->split_move_from[i] = (SplitAnimRect){
            old_rects[i].x, old_rects[i].y, old_rects[i].w, old_rects[i].h
        };
    }
    if (new_pane >= 0 && new_pane < MAX_SPLIT_PANES) {
        if (source_pane < 0 || source_pane >= MAX_SPLIT_PANES) source_pane = 0;
        tab->split_move_from[new_pane] = tab->split_move_from[source_pane];
    }
    f64 now = platform_time_sec();
    tab->split_move_animating = true;
    tab->split_move_anim_start = now;
    anim_register_until(now + SPLIT_MOVE_DUR);
}

static void tab_split_start_hy3_open_anim(Tab *tab,
                                          const PaneRect old_rects[MAX_SPLIT_PANES],
                                          const PaneRect final_rects[MAX_SPLIT_PANES],
                                          i32 source_pane,
                                          i32 new_pane) {
    if (!tab || !old_rects || !final_rects) return;
    /* Mirror of the close reflow (tab_split_start_move_anim) — same lightweight
     * geometry path, no GPU transform: every pane interpolates from a stored
     * "from" rect to the final layout with its content moving along. The source
     * shrinks from the whole pre-split cell to its half, and the new pane sweeps
     * IN from the OUTER (window) edge so the shared divider slides into place —
     * the exact reverse of a pane collapsing while its neighbour reflows to fill
     * the gap. Outer-edge (not divider) sweep glues the new pane's leading edge
     * to the source's receding edge: no gap, no overlap, and no "content covered
     * then reappears" glitch (the new pane never starts over the source). */
    for (i32 i = 0; i < MAX_SPLIT_PANES; i++) {
        tab->split_move_from[i] = (SplitAnimRect){
            old_rects[i].x, old_rects[i].y, old_rects[i].w, old_rects[i].h
        };
    }
    if (source_pane >= 0 && source_pane < MAX_SPLIT_PANES &&
        new_pane    >= 0 && new_pane    < MAX_SPLIT_PANES) {
        PaneRect sr = final_rects[source_pane];
        PaneRect nr = final_rects[new_pane];
        SplitAnimRect sliver;
        if (nr.y >= sr.y + sr.h - 1.0f)            /* new below source  */
            sliver = (SplitAnimRect){ nr.x, nr.y + nr.h - 2.0f, nr.w, 2.0f };
        else if (nr.y + nr.h <= sr.y + 1.0f)       /* new above source  */
            sliver = (SplitAnimRect){ nr.x, nr.y, nr.w, 2.0f };
        else if (nr.x >= sr.x + sr.w - 1.0f)       /* new right of source */
            sliver = (SplitAnimRect){ nr.x + nr.w - 2.0f, nr.y, 2.0f, nr.h };
        else                                       /* new left of source  */
            sliver = (SplitAnimRect){ nr.x, nr.y, 2.0f, nr.h };
        /* Source shrinks from the whole pre-split cell (bounding box of the two
         * halves) to its half. Deriving it from sr∪nr rather than
         * old_rects[source_pane] is robust to the new_first first-split. */
        f32 x0 = sr.x < nr.x ? sr.x : nr.x;
        f32 y0 = sr.y < nr.y ? sr.y : nr.y;
        f32 x1 = (sr.x + sr.w) > (nr.x + nr.w) ? (sr.x + sr.w) : (nr.x + nr.w);
        f32 y1 = (sr.y + sr.h) > (nr.y + nr.h) ? (sr.y + sr.h) : (nr.y + nr.h);
        tab->split_move_from[source_pane] =
            (SplitAnimRect){ x0, y0, x1 - x0, y1 - y0 };
        tab->split_move_from[new_pane] = sliver;
    }
    f64 now = platform_time_sec();
    tab->split_move_animating = true;
    tab->split_move_anim_start = now;
    anim_register_until(now + SPLIT_MOVE_DUR);
}

static void tab_split_draw_dividers(Renderer *r, const Tab *tab, i32 node,
                                    PaneRect rect, f32 dpi, Color color) {
    if (!r || !tab || node < 0 || node >= tab->split_next_node) return;
    const SplitLayoutNode *n = &tab->split_nodes[node];
    if (!n->used || n->leaf) return;
    f32 div  = 6.0f * dpi;          /* inter-pane gap (matches layout) */
    f32 line = 2.0f * dpi;          /* the divider line; 2px clear each side */
    f32 ratio = n->ratio;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    if (n->split == SPLIT_V) {
        f32 split = rect.h * ratio;
        renderer_draw_rect(r, rect.x, rect.y + split - line * 0.5f,
                           rect.w, line, color);
        PaneRect first = {rect.x, rect.y, rect.w, split - div * 0.5f};
        PaneRect second = {rect.x, rect.y + split + div * 0.5f,
                           rect.w, rect.h - split - div * 0.5f};
        tab_split_draw_dividers(r, tab, n->first, first, dpi, color);
        tab_split_draw_dividers(r, tab, n->second, second, dpi, color);
    } else {
        f32 split = rect.w * ratio;
        renderer_draw_rect(r, rect.x + split - line * 0.5f, rect.y,
                           line, rect.h, color);
        PaneRect first = {rect.x, rect.y, split - div * 0.5f, rect.h};
        PaneRect second = {rect.x + split + div * 0.5f, rect.y,
                           rect.w - split - div * 0.5f, rect.h};
        tab_split_draw_dividers(r, tab, n->first, first, dpi, color);
        tab_split_draw_dividers(r, tab, n->second, second, dpi, color);
    }
}

/* Bounded child reap for app_destroy: SIGTERM, then poll ~300 ms, then
 * SIGKILL. The translate/create-theme children can be a Node/Python agent
 * CLI that traps SIGTERM and won't exit promptly; a naive blocking
 * waitpid would hang Liu on quit. Mirrors main.c's kill_child_graceful
 * (static there, so duplicated rather than shared). EINTR-safe. */
static void app_reap_child_bounded(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 60; i++) {              /* ~300 ms */
        pid_t r;
        do { r = waitpid(pid, NULL, WNOHANG); } while (r < 0 && errno == EINTR);
        if (r == pid || (r < 0 && errno == ECHILD)) return;
        struct timespec ts = { 0, 5 * 1000 * 1000 };  /* 5 ms */
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    /* Bounded: SIGKILL is uncatchable, so the child exits promptly. */
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) { }
}

void app_destroy(AppState *app) {
    /* Clear passphrase cache before exit (scrub from memory) */
    passphrase_cache_clear(app);

    /* Scrub any revealed vault plaintext that the user left on screen
     * — without this, quitting from an active reveal leaves decrypted
     * password bytes resident through atexit. */
    app_vault_browser_close(app);

    /* Sidebar file browser owns up to ~50 MiB of cached state (md
     * arena, image cache pixels, view RGBA) — released here. Tabs
     * with their own FileBrowser are destroyed in the tab loop
     * below; this handles the always-present sidebar instance. */
    fb_destroy(&app->filebrowser);

    /* Transcript viewer events — geometric realloc buffer plus per-
     * event text/tool_name heap strdups; freed when the user hits
     * Esc but skipped on Cmd+Q without dismissal. */
    if (app->transcript_events) {
        for (i32 ti = 0; ti < app->transcript_count; ti++) {
            free(app->transcript_events[ti].text);
            free(app->transcript_events[ti].tool_name);
        }
        free(app->transcript_events);
        app->transcript_events = NULL;
        app->transcript_count  = 0;
        app->transcript_cap    = 0;
    }

    /* Release palette side-tables (history sids/paths, ~152 KB) and
     * flush the MRU cache so any unsaved floats land on disk. */
    palette_shutdown();

    /* Persist the dev-server registry, then stop every running site (tree-kill
     * each process group) and free their log terminals before saving config. */
    sites_manager_sync_to_config(&app->site_mgr, &app->config);
    sites_manager_shutdown(&app->site_mgr);
    term_render_cache_free(&app->sites.log_cache);

    /* Save config + keybindings on exit */
    config_save(&app->config, config_file_path());
    {
        char kb_path[1024];
        snprintf(kb_path, sizeof(kb_path), "%s/keybindings.json", config_user_dir());
        keybind_save(&app->keybinds, kb_path);
    }

    for (i32 i = 0; i < app->tab_count; i++) {
        Tab *t = &app->tabs[i];
        if (t->kind == TAB_FILEBROWSER) {
            if (t->fb) { fb_destroy(t->fb); free(t->fb); t->fb = NULL; }
            continue;
        }
        /* Resume suspended sessions so SIGHUP in session_destroy actually
         * reaches the shell (stopped processes ignore signals until CONT). */
        if (t->session && session_is_suspended(t->session)) {
            session_resume(t->session);
        }
        if (t->session2 && session_is_suspended(t->session2)) {
            session_resume(t->session2);
        }
        for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
            Session *xs = t->extra_sessions[p - 2];
            if (xs && session_is_suspended(xs)) session_resume(xs);
        }
        terminal_destroy(t->terminal);
        session_destroy(t->session);
        if (t->terminal2) terminal_destroy(t->terminal2);
        if (t->session2)  session_destroy(t->session2);
        for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
            if (t->extra_terminals[p - 2]) terminal_destroy(t->extra_terminals[p - 2]);
            if (t->extra_sessions[p - 2])  session_destroy(t->extra_sessions[p - 2]);
            term_render_cache_free(&t->extra_caches[p - 2]);
        }
        term_render_cache_free(&t->cache1);
        term_render_cache_free(&t->cache2);
        tab_sleep_snapshot_reset(&t->sleep_pane1);
        tab_sleep_snapshot_reset(&t->sleep_pane2);
        for (i32 p = 2; p < MAX_SPLIT_PANES; p++)
            tab_sleep_snapshot_reset(&t->sleep_extra[p - 2]);
        free(t->git_status.cached_cwd);
        t->git_status.cached_cwd = NULL;
    }
    for (i32 i = 0; i < app->closed_tab_count; i++) {
        closed_tab_clear(&app->closed_tabs[i]);
    }
    if (app->vault) {
        vault_close(app->vault);
        app->vault = NULL;
    }
    renderer_destroy(&app->renderer);

    /* Module-global caches freed for a quiescent exit (the OS reclaims them
     * regardless; this keeps leak-checkers clean). */
    extern void sidebar_free_hosts(void);
    extern void chat_scan_shutdown(void);
    sidebar_free_hosts();
    chat_scan_shutdown();

    free(app->cmd_history_raw);
    app->cmd_history_raw = NULL;
    free(app->key_list);
    app->key_list = NULL;
    free(app->passphrase_cache);
    app->passphrase_cache = NULL;
    app->passphrase_cache_cap = 0;
    app->key_list_cap = 0;
    free(app->bench_samples_ms);
    app->bench_samples_ms = NULL;
    free(app->agent_picker_rows);
    app->agent_picker_rows = NULL;
    free(app->ssh_history);
    app->ssh_history = NULL;
    free(app->kbi_prompts);
    app->kbi_prompts = NULL;
    free(app->kbi_responses);
    app->kbi_responses = NULL;
    /* Create Theme dialog state — the agent buffer + log outlive a
     * single dialog session so we only release them here. If the agent
     * is still running, send SIGTERM and reap it so we don't strand a
     * subprocess writing to a closed pipe past process exit. */
    if (app->create_theme_child_pid > 0) {
        app_reap_child_bounded((pid_t)app->create_theme_child_pid);
        app->create_theme_child_pid = 0;
    }
    free(app->create_theme_agents);
    app->create_theme_agents = NULL;
    free(app->create_theme_log);
    app->create_theme_log = NULL;
    app->create_theme_log_cap = 0;
    if (app->create_theme_stdout_fd >= 0) {
        close(app->create_theme_stdout_fd);
        app->create_theme_stdout_fd = -1;
    }
    /* Translate-on-Tab — mirror Create Theme: SIGTERM + reap any orphan
     * agent child, then release the rolling log buffer. */
    if (app->translate_child_pid > 0) {
        app_reap_child_bounded((pid_t)app->translate_child_pid);
        app->translate_child_pid = 0;
    }
    if (app->translate_stdout_fd >= 0) {
        close(app->translate_stdout_fd);
        app->translate_stdout_fd = -1;
    }
    /* Settings model-fetch children (also reaped from the atexit hook for
     * the Cmd+Q path that skips app_destroy entirely). */
    model_catalog_shutdown();
    free(app->translate_log);
    app->translate_log = NULL;
    app->translate_log_cap = 0;
    /* Abort + join any in-flight model download (its own worker thread). */
    model_download_shutdown();
#ifdef LIU_HAVE_LOCAL_LLM
    /* Local-LLM backend: join the worker thread + free the cached engine. */
    translate_local_shutdown();
#endif
    /* Drain the deferred-destroy queue last — everything else above may
     * have queued more entries via app_close_tab on the way out. */
    tab_trash_shutdown();
}


static const char *sessions_file_path(void) {
    static char path[1024] = {0};
    if (path[0]) return path;
    const char *cfg = config_file_path();
    snprintf(path, sizeof(path), "%s", cfg);
    char *slash = strrchr(path, '/');
    if (slash) {
        slash[1] = '\0';
        strncat(path, "sessions.json", sizeof(path) - strlen(path) - 1);
    } else {
        snprintf(path, sizeof(path), "sessions.json");
    }
    return path;
}

/* Escape a string for JSON output (handles backslash and double-quote) */
static void json_write_escaped(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '\\': fputs("\\\\", f); break;
        case '"':  fputs("\\\"", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:   fputc(*p, f);     break;
        }
    }
    fputc('"', f);
}

static const char *split_type_str(SplitType s) {
    switch (s) {
    case SPLIT_H: return "horizontal";
    case SPLIT_V: return "vertical";
    default:      return "none";
    }
}

static const char *auth_method_str(AuthMethod m) {
    switch (m) {
    case AUTH_PASSWORD:  return "password";
    case AUTH_PUBLICKEY: return "publickey";
    case AUTH_AGENT:     return "agent";
    default:             return "agent";
    }
}

static const char *sidebar_mode_str(SidebarMode m) {
    switch (m) {
    case SIDEBAR_HOSTS:    return "hosts";
    case SIDEBAR_SFTP:     return "sftp";
    case SIDEBAR_SNIPPETS: return "snippets";
    default:               return "hosts";
    }
}

bool app_save_sessions(AppState *app) {
    if (app->tab_count <= 0) return false;

    const char *path = sessions_file_path();

    /* Ensure directory exists */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }

    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": 1,\n");
    fprintf(f, "  \"active_tab\": %d,\n", app->active_tab);
    fprintf(f, "  \"sidebar_visible\": %s,\n", app->sidebar_visible ? "true" : "false");
    fprintf(f, "  \"sidebar_mode\": \"%s\",\n", sidebar_mode_str(app->sidebar_mode));
    fprintf(f, "  \"tabs\": [\n");

    for (i32 i = 0; i < app->tab_count; i++) {
        Tab *tab = &app->tabs[i];
        SessionType stype = tab_primary_session_type(tab);
        const TabSleepSnapshot *sleep1 = tab->sleep_pane1;
        const TabSleepSnapshot *sleep2 = tab->sleep_pane2;
        const char *cwd = tab->sleeping
            ? (sleep1 ? sleep1->cwd : NULL)
            : (tab->terminal ? tab->terminal->cwd : NULL);

        fprintf(f, "    {\n");
        fprintf(f, "      \"title\": ");
        json_write_escaped(f, tab->title);
        fprintf(f, ",\n");

        if (stype == SESSION_SSH || stype == SESSION_MOSH) {
            const SSHConfig *cfg = tab->sleeping
                ? (sleep1 ? sleep1->ssh_config : NULL)
                : session_get_config(tab->session);
            fprintf(f, "      \"type\": \"%s\",\n", stype == SESSION_MOSH ? "mosh" : "ssh");
            if (cfg) {
                fprintf(f, "      \"hostname\": ");
                json_write_escaped(f, cfg->hostname);
                fprintf(f, ",\n");
                fprintf(f, "      \"port\": %d,\n", cfg->port);
                fprintf(f, "      \"username\": ");
                json_write_escaped(f, cfg->username);
                fprintf(f, ",\n");
                fprintf(f, "      \"auth_method\": \"%s\",\n", auth_method_str(cfg->auth_method));
                fprintf(f, "      \"key_path\": ");
                json_write_escaped(f, cfg->key_path);
                fprintf(f, ",\n");
            } else {
                fprintf(f, "      \"hostname\": \"\",\n");
                fprintf(f, "      \"port\": 22,\n");
                fprintf(f, "      \"username\": \"\",\n");
                fprintf(f, "      \"auth_method\": \"agent\",\n");
                fprintf(f, "      \"key_path\": \"\",\n");
            }
        } else {
            fprintf(f, "      \"type\": \"local\",\n");
            fprintf(f, "      \"cwd\": ");
            json_write_escaped(f, cwd ? cwd : "");
            fprintf(f, ",\n");
        }

        fprintf(f, "      \"split\": \"%s\",\n", split_type_str(tab->split));
        fprintf(f, "      \"split_ratio\": %.2f", tab->split_ratio);

        /* Second pane info for split tabs */
        if (tab->split != SPLIT_NONE && (tab->session2 || sleep2)) {
            SessionType stype2 = tab_secondary_session_type(tab);
            const char *cwd2 = tab->sleeping
                ? (sleep2 ? sleep2->cwd : NULL)
                : (tab->terminal2 ? tab->terminal2->cwd : NULL);
            fprintf(f, ",\n");

            if (stype2 == SESSION_SSH || stype2 == SESSION_MOSH) {
                const SSHConfig *cfg2 = tab->sleeping
                    ? (sleep2 ? sleep2->ssh_config : NULL)
                    : session_get_config(tab->session2);
                fprintf(f, "      \"pane2_type\": \"%s\"", stype2 == SESSION_MOSH ? "mosh" : "ssh");
                if (cfg2) {
                    fprintf(f, ",\n      \"pane2_hostname\": ");
                    json_write_escaped(f, cfg2->hostname);
                    fprintf(f, ",\n      \"pane2_port\": %d", cfg2->port);
                    fprintf(f, ",\n      \"pane2_username\": ");
                    json_write_escaped(f, cfg2->username);
                    fprintf(f, ",\n      \"pane2_auth_method\": \"%s\"", auth_method_str(cfg2->auth_method));
                    fprintf(f, ",\n      \"pane2_key_path\": ");
                    json_write_escaped(f, cfg2->key_path);
                }
            } else {
                fprintf(f, "      \"pane2_type\": \"local\",\n");
                fprintf(f, "      \"pane2_cwd\": ");
                json_write_escaped(f, cwd2 ? cwd2 : "");
            }
        }

        fprintf(f, "\n    }%s\n", (i < app->tab_count - 1) ? "," : "");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

/* Minimal JSON token helpers for session restore */
static const char *json_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Read a JSON string value (after opening quote). Writes into buf, returns
 * pointer past closing quote, or NULL on error. */
static const char *json_read_string(const char *p, char *buf, i32 buf_size) {
    i32 i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            if (i < buf_size - 1) buf[i++] = c;
            p++;
        } else {
            if (i < buf_size - 1) buf[i++] = *p;
            p++;
        }
    }
    buf[i] = '\0';
    if (*p == '"') p++;
    return p;
}

/* Try to cd into a given directory by sending "cd <path>\n" to the session */
static void session_cd_to(Session *s, const char *cwd) {
    if (!s || !cwd || !cwd[0]) return;
    char cmd[1024];
    i32 len = snprintf(cmd, sizeof(cmd), "cd ");
    /* Quote the path with single quotes for shell safety */
    cmd[len++] = '\'';
    for (const char *p = cwd; *p && len < (i32)sizeof(cmd) - 4; p++) {
        if (*p == '\'') {
            /* Escape single quote: end quote, escaped quote, start quote */
            if (len < (i32)sizeof(cmd) - 7) {
                cmd[len++] = '\'';
                cmd[len++] = '\\';
                cmd[len++] = '\'';
                cmd[len++] = '\'';
            }
        } else {
            cmd[len++] = *p;
        }
    }
    cmd[len++] = '\'';
    cmd[len++] = '\n';
    cmd[len] = '\0';
    session_write(s, (const u8 *)cmd, len);
    /* Also send clear to hide the cd command */
    session_write(s, (const u8 *)"clear\n", 6);
}

static const char *chat_tool_display(ChatTool t);   /* defined below */

/* True when a session-restore title equals one of the agent display names
 * ("OpenCode", "Claude Code", …). Those titles describe a foreground process,
 * not the tab — and the process is gone after an app relaunch (restore spawns
 * a plain shell), so carrying the name over would mislabel a fresh prompt.
 * The auto-title pass can't fix it either: fg stays UNKNOWN and no OSC title
 * arrives at a quiet prompt, so nothing ever re-derives the stale name. */
static bool title_is_agent_display_name(const char *title) {
    if (!title || !title[0]) return false;
    for (i32 t = CHAT_TOOL_CLAUDE; t < CHAT_TOOL_COUNT_; t++) {
        if (strcmp(title, chat_tool_display((ChatTool)t)) == 0) return true;
    }
    return false;
}

bool app_restore_sessions(AppState *app) {
    const char *path = sessions_file_path();
    FILE *f = fopen(path, "r");
    if (!f) return false;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 1024 * 1024) { fclose(f); return false; }

    char *data = (char *)malloc((usize)fsize + 1);
    if (!data) { fclose(f); return false; }
    fread(data, 1, (usize)fsize, f);
    data[fsize] = '\0';
    fclose(f);

    /* Parse top-level fields */
    i32 active_tab_idx = 0;
    bool sidebar_vis = false;
    SidebarMode sidebar_m = SIDEBAR_HOSTS;

    /* Simple line-by-line parse for top-level scalars */
    const char *p = data;

    /* Find "active_tab" */
    {
        const char *at = strstr(data, "\"active_tab\"");
        if (at) {
            at = strchr(at, ':');
            if (at) { at++; active_tab_idx = atoi(at); }
        }
    }
    {
        const char *sv = strstr(data, "\"sidebar_visible\"");
        if (sv) {
            sv = strchr(sv, ':');
            if (sv) {
                sv = json_skip_ws(sv + 1);
                sidebar_vis = (strncmp(sv, "true", 4) == 0);
            }
        }
    }
    {
        const char *sm = strstr(data, "\"sidebar_mode\"");
        if (sm) {
            sm = strchr(sm, ':');
            if (sm) {
                sm = json_skip_ws(sm + 1);
                if (*sm == '"') {
                    char mode_buf[32];
                    json_read_string(sm + 1, mode_buf, sizeof(mode_buf));
                    if (strcmp(mode_buf, "sftp") == 0) sidebar_m = SIDEBAR_SFTP;
                    else if (strcmp(mode_buf, "snippets") == 0) sidebar_m = SIDEBAR_SNIPPETS;
                }
            }
        }
    }

    /* Find the "tabs" array */
    const char *tabs_start = strstr(data, "\"tabs\"");
    if (!tabs_start) { free(data); return false; }
    tabs_start = strchr(tabs_start, '[');
    if (!tabs_start) { free(data); return false; }
    tabs_start++; /* past '[' */

    /* Parse tab objects */
    i32 tabs_created = 0;
    p = tabs_start;
    while (*p) {
        p = json_skip_ws(p);
        if (*p == ']') break;
        if (*p != '{') { p++; continue; }
        p++; /* past '{' */

        /* Per-tab fields */
        char title[128] = "Terminal";
        char type_str[16] = "local";
        char cwd_buf[1024] = "";
        char hostname[256] = "";
        i32  port = 22;
        char username[128] = "";
        char auth_method[32] = "agent";
        char key_path[512] = "";
        char split_str[16] = "none";
        f32  split_ratio = 0.5f;
        char pane2_type[16] = "";
        char pane2_cwd[1024] = "";
        char pane2_hostname[256] = "";
        i32  pane2_port = 22;
        char pane2_username[128] = "";
        char pane2_auth_method[32] = "agent";
        char pane2_key_path[512] = "";

        /* Parse key-value pairs until '}' */
        i32 depth = 1;
        while (*p && depth > 0) {
            p = json_skip_ws(p);
            if (*p == '}') { depth--; p++; break; }
            if (*p == ',') { p++; continue; }

            /* Expect a key string */
            if (*p != '"') { p++; continue; }
            p++; /* past opening quote */
            char key[64] = "";
            p = json_read_string(p, key, sizeof(key));
            p = json_skip_ws(p);
            if (*p == ':') p++;
            p = json_skip_ws(p);

            /* Read value */
            if (*p == '"') {
                /* String value */
                p++;
                char val[1024] = "";
                p = json_read_string(p, val, sizeof(val));
                if (strcmp(key, "title") == 0) snprintf(title, sizeof(title), "%s", val);
                else if (strcmp(key, "type") == 0) snprintf(type_str, sizeof(type_str), "%s", val);
                else if (strcmp(key, "cwd") == 0) snprintf(cwd_buf, sizeof(cwd_buf), "%s", val);
                else if (strcmp(key, "hostname") == 0) snprintf(hostname, sizeof(hostname), "%s", val);
                else if (strcmp(key, "username") == 0) snprintf(username, sizeof(username), "%s", val);
                else if (strcmp(key, "auth_method") == 0) snprintf(auth_method, sizeof(auth_method), "%s", val);
                else if (strcmp(key, "key_path") == 0) snprintf(key_path, sizeof(key_path), "%s", val);
                else if (strcmp(key, "split") == 0) snprintf(split_str, sizeof(split_str), "%s", val);
                else if (strcmp(key, "pane2_type") == 0) snprintf(pane2_type, sizeof(pane2_type), "%s", val);
                else if (strcmp(key, "pane2_cwd") == 0) snprintf(pane2_cwd, sizeof(pane2_cwd), "%s", val);
                else if (strcmp(key, "pane2_hostname") == 0) snprintf(pane2_hostname, sizeof(pane2_hostname), "%s", val);
                else if (strcmp(key, "pane2_username") == 0) snprintf(pane2_username, sizeof(pane2_username), "%s", val);
                else if (strcmp(key, "pane2_auth_method") == 0) snprintf(pane2_auth_method, sizeof(pane2_auth_method), "%s", val);
                else if (strcmp(key, "pane2_key_path") == 0) snprintf(pane2_key_path, sizeof(pane2_key_path), "%s", val);
            } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
                /* Numeric value */
                char numstr[64];
                i32 ni = 0;
                while ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-') {
                    if (ni < 63) numstr[ni++] = *p;
                    p++;
                }
                numstr[ni] = '\0';
                if (strcmp(key, "port") == 0) port = atoi(numstr);
                else if (strcmp(key, "split_ratio") == 0) split_ratio = (f32)atof(numstr);
                else if (strcmp(key, "pane2_port") == 0) pane2_port = atoi(numstr);
            } else if (*p == 't' || *p == 'f') {
                /* Boolean — skip */
                while (*p && *p != ',' && *p != '}') p++;
            }
        }

        /* Create the session */
        Session *session = NULL;
        if ((strcmp(type_str, "ssh") == 0 || strcmp(type_str, "mosh") == 0) && hostname[0]) {
            SSHConfig cfg = {0};
            snprintf(cfg.hostname, sizeof(cfg.hostname), "%s", hostname);
            snprintf(cfg.username, sizeof(cfg.username), "%s", username);
            cfg.port = port > 0 ? port : 22;
            snprintf(cfg.key_path, sizeof(cfg.key_path), "%s", key_path);
            if (strcmp(auth_method, "publickey") == 0) cfg.auth_method = AUTH_PUBLICKEY;
            else if (strcmp(auth_method, "password") == 0) cfg.auth_method = AUTH_PASSWORD;
            else cfg.auth_method = AUTH_AGENT;
            session = (strcmp(type_str, "mosh") == 0)
                ? session_create_mosh(&cfg, app->grid_cols, app->grid_rows)
                : session_create_ssh(&cfg, app->grid_cols, app->grid_rows);
        } else {
            session = session_create_local(app->grid_cols, app->grid_rows);
        }

        if (!session) continue;

        /* Saved agent titles are stale on restore — re-derive from the cwd. */
        if (strcmp(type_str, "local") == 0 && title_is_agent_display_name(title)) {
            const char *home = getenv("HOME");
            if (cwd_buf[0]) {
                if (home && home[0] && strcmp(cwd_buf, home) == 0) {
                    snprintf(title, sizeof(title), "~");
                } else {
                    const char *slash = strrchr(cwd_buf, '/');
                    snprintf(title, sizeof(title), "%s",
                             (slash && slash[1]) ? slash + 1 : cwd_buf);
                }
            } else {
                snprintf(title, sizeof(title), "Terminal");
            }
        }

        i32 tab_idx = app_new_tab(app, session, title);
        if (tab_idx < 0) { session_destroy(session); continue; }
        tabs_created++;

        /* For local sessions, cd to saved CWD */
        if (strcmp(type_str, "local") == 0 && cwd_buf[0]) {
            session_cd_to(session, cwd_buf);
        }

        /* Restore split pane */
        Tab *tab = &app->tabs[tab_idx];
        SplitType sp = SPLIT_NONE;
        if (strcmp(split_str, "horizontal") == 0) sp = SPLIT_H;
        else if (strcmp(split_str, "vertical") == 0) sp = SPLIT_V;

        if (sp != SPLIT_NONE && pane2_type[0]) {
            tab->split = sp;
            tab->split_ratio = split_ratio;
            tab->split_pane_count = 2;
            tab_split_init_tree(tab, sp, false);
            if (tab->split_root >= 0) tab->split_nodes[tab->split_root].ratio = split_ratio;

            i32 cols2 = app->grid_cols / (sp == SPLIT_H ? 2 : 1);
            i32 rows2 = app->grid_rows / (sp == SPLIT_V ? 2 : 1);
            if (cols2 < 10) cols2 = 10;
            if (rows2 < 5) rows2 = 5;

            Session *s2 = NULL;
            if ((strcmp(pane2_type, "ssh") == 0 || strcmp(pane2_type, "mosh") == 0) && pane2_hostname[0]) {
                SSHConfig cfg2 = {0};
                snprintf(cfg2.hostname, sizeof(cfg2.hostname), "%s", pane2_hostname);
                snprintf(cfg2.username, sizeof(cfg2.username), "%s", pane2_username);
                cfg2.port = pane2_port > 0 ? pane2_port : 22;
                snprintf(cfg2.key_path, sizeof(cfg2.key_path), "%s", pane2_key_path);
                if (strcmp(pane2_auth_method, "publickey") == 0) cfg2.auth_method = AUTH_PUBLICKEY;
                else if (strcmp(pane2_auth_method, "password") == 0) cfg2.auth_method = AUTH_PASSWORD;
                else cfg2.auth_method = AUTH_AGENT;
                s2 = (strcmp(pane2_type, "mosh") == 0)
                    ? session_create_mosh(&cfg2, cols2, rows2)
                    : session_create_ssh(&cfg2, cols2, rows2);
            } else {
                s2 = session_create_local(cols2, rows2);
            }

            if (s2) {
                tab->session2 = s2;
                tab->terminal2 = terminal_create(cols2, rows2);
                if (tab->terminal2) {
                    terminal_set_scrollback_limit(tab->terminal2, app->config.scrollback_lines);
                    app_configure_terminal_callbacks(app, tab->terminal2, s2);
                    tab->terminal2->bidi_enabled = app->config.bidi_enabled;
                    tab->active_pane = 0;
                } else {
                    session_destroy(s2);
                    tab->session2 = NULL;
                    tab->split = SPLIT_NONE;
                    tab->split_pane_count = 1;
                    tab_split_tree_reset(tab);
                }

                /* cd to saved CWD for local pane2 */
                if (tab->terminal2 &&
                    strcmp(pane2_type, "local") == 0 && pane2_cwd[0]) {
                    session_cd_to(s2, pane2_cwd);
                }
            } else {
                tab->split = SPLIT_NONE;
                tab->split_pane_count = 1;
                tab_split_tree_reset(tab);
            }
        } else {
            tab->split_ratio = split_ratio;
        }

        /* Skip comma between tab objects */
        p = json_skip_ws(p);
        if (*p == ',') p++;
    }

    free(data);

    if (tabs_created == 0) return false;

    /* Restore sidebar state */
    app->sidebar_visible = sidebar_vis;
    app->sidebar_mode = sidebar_m;
    if (sidebar_vis) {
        /* Reopen at the user's remembered width (points, persisted in config).
         * The open animation only re-derives the width on a visibility *flip*,
         * which doesn't happen on this startup-restore path, so set it here. */
        app->sidebar_width = CLAMP(app->config.sidebar_width,
                                   SIDEBAR_MIN_PT, SIDEBAR_MAX_PT) * app->dpi_scale;
    }

    /* Restore active tab */
    if (active_tab_idx >= 0 && active_tab_idx < app->tab_count)
        app->active_tab = active_tab_idx;

    return true;
}

void app_apply_config(AppState *app) {
    const Theme *t = app->config.theme;
    if (!t) t = &THEME_DARK;

    /* Override ANSI palette from theme */
    for (i32 i = 0; i < 16; i++) {
        g_ansi_colors[i] = t->ansi[i];
    }
    /* Default fg/bg mapped to indices 7 and 0 */
    g_ansi_colors[FG_DEFAULT] = t->fg;
    g_ansi_colors[BG_DEFAULT] = t->bg;

    /* Sync background image config to renderer */
    app->renderer.bg_opacity = app->config.background_opacity;
    app->renderer.bg_mode    = app->config.background_mode;

    for (i32 i = 0; i < app->tab_count; i++) {
        if (app->tabs[i].terminal)
            terminal_set_scrollback_limit(app->tabs[i].terminal, app->config.scrollback_lines);
        if (app->tabs[i].terminal2)
            terminal_set_scrollback_limit(app->tabs[i].terminal2, app->config.scrollback_lines);
        for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
            if (app->tabs[i].extra_terminals[p - 2])
                terminal_set_scrollback_limit(app->tabs[i].extra_terminals[p - 2],
                                              app->config.scrollback_lines);
        }
    }

    /* palette just changed — every cached rect/glyph holds stale
     * colors, force a full cache rebuild on next render. */
    for (i32 i = 0; i < app->tab_count; i++) {
        app->tabs[i].cache1.all_rows_dirty = true;
        app->tabs[i].cache2.all_rows_dirty = true;
        for (i32 p = 2; p < MAX_SPLIT_PANES; p++)
            app->tabs[i].extra_caches[p - 2].all_rows_dirty = true;
    }
}

/* =========================================================================
 * Tab management
 * ========================================================================= */

i32 app_new_tab(AppState *app, Session *session, const char *title) {
    if (app->tab_count >= MAX_TABS) return -1;

    i32 idx = app->tab_count++;
    Tab *tab = &app->tabs[idx];
    memset(tab, 0, sizeof(*tab));
    /* Historical sentinel fields; pane swaps now animate geometry. */
    tab->swap_flash_slot_a = -1;
    tab->swap_flash_slot_b = -1;

    snprintf(tab->title, sizeof(tab->title), "%s", title);
    tab->kind     = TAB_TERMINAL;
    tab->session  = session;
    tab->terminal = terminal_create(app->grid_cols > 0 ? app->grid_cols : 80,
                                     app->grid_rows > 0 ? app->grid_rows : 24);
    terminal_set_scrollback_limit(tab->terminal, app->config.scrollback_lines);
    tab->active = true;
    tab->dirty  = true;
    tab->group_index = -1;  /* no group by default */
    tab->split  = SPLIT_NONE;
    tab->split_ratio = 0.5f;
    tab->split_pane_count = 1;
    tab->split_root = -1;
    tab->active_pane = 0;
    tab->profile_index = -1; /* use global config */

    app_configure_terminal_callbacks(app, tab->terminal, session);
    /* Apply BiDi setting from config */
    tab->terminal->bidi_enabled = app->config.bidi_enabled;

    app->broadcast_targets[idx][0] = app->broadcast_mode;
    app->broadcast_targets[idx][1] = false;
    app->active_tab = idx;
    tab->last_activity_time = platform_time_sec();
    /* Fade + slide the new pill in. anim_start registers the frame loop so the
     * other tabs' render_x springs (they get narrower) settle in the same
     * window. */
    anim_start(&tab->tab_open, 0.24);
    app_trim_inactive_render_caches(app);
    return idx;
}

/* Smart FB-tab title: derive a short, contextual label from the FB's
 * current cwd. Honors HOME ("~"), root ("/"), and strips any trailing
 * slash so `/foo/bar/` and `/foo/bar` render the same.
 *
 * Skips tabs with a user-set custom_title — once the user renames the
 * tab, navigation no longer overrides their choice.
 *
 * Safe to call on any Tab*; bails for non-FB or fb-less tabs. */
void app_refresh_fb_tab_title(Tab *tab) {
    if (!tab) return;
    if (tab->kind != TAB_FILEBROWSER || !tab->fb) return;
    if (tab->custom_title[0]) return;   /* user renamed; don't clobber */

    /* In graph mode the meaningful name is the GRAPHED folder (graph_root), not
     * the file browser's cwd (which may still be "/" or wherever it was when the
     * tab was created) — so the tab reads as the opened folder, not "/". */
    const char *cwd = (tab->fb->view_mode == FVIEW_GRAPH && tab->fb->graph_root[0])
                        ? tab->fb->graph_root : tab->fb->cwd;
    if (!cwd || !cwd[0]) {
        snprintf(tab->title, sizeof(tab->title), "Files");
        return;
    }

    /* Strip any trailing slash (except for the root "/"). */
    char trimmed[FB_MAX_PATH];
    snprintf(trimmed, sizeof(trimmed), "%s", cwd);
    usize tlen = strlen(trimmed);
    while (tlen > 1 && trimmed[tlen - 1] == '/') {
        trimmed[--tlen] = '\0';
    }

    /* Root */
    if (tlen == 1 && trimmed[0] == '/') {
        snprintf(tab->title, sizeof(tab->title), "/");
        return;
    }

    /* HOME → "~" ; HOME/foo → "~/foo basename" simplification: just show
     * the basename, but for the home dir itself use "~". */
    const char *home = getenv("HOME");
    if (home && home[0]) {
        usize hlen = strlen(home);
        /* Trim trailing slash on home so the prefix match is exact. */
        while (hlen > 1 && home[hlen - 1] == '/') hlen--;
        if (tlen == hlen && strncmp(trimmed, home, hlen) == 0) {
            snprintf(tab->title, sizeof(tab->title), "~");
            return;
        }
    }

    /* Generic case: basename of the trimmed path. */
    const char *slash = strrchr(trimmed, '/');
    const char *base = slash ? slash + 1 : trimmed;
    if (!base[0]) base = "/";
    snprintf(tab->title, sizeof(tab->title), "%s", base);
}

/* Create a file-browser tab rooted at `path`. Returns the new tab index,
 * or -1 if the slot table is full or the path can't be opened.
 *
 * The tab carries its own heap-allocated FileBrowser; broadcasting,
 * sleep, sessions, and split-pane state stay zeroed. */
i32 app_new_filebrowser_tab(AppState *app, const char *path) {
    if (app->tab_count >= MAX_TABS) return -1;
    if (!path || !path[0]) return -1;

    FileBrowser *fb = (FileBrowser *)calloc(1, sizeof(*fb));
    if (!fb) return -1;
    fb_init(fb);
    if (!fb_navigate(fb, path)) {
        fb_destroy(fb);
        free(fb);
        return -1;
    }
    fb->open = true;

    i32 idx = app->tab_count++;
    Tab *tab = &app->tabs[idx];
    memset(tab, 0, sizeof(*tab));
    tab->swap_flash_slot_a = -1;
    tab->swap_flash_slot_b = -1;

    tab->kind         = TAB_FILEBROWSER;
    tab->fb           = fb;
    tab->fb_viewer_ratio = 0.0f;     /* lazy: use VIEWER_WIDTH_RATIO default */
    tab->active       = true;
    tab->dirty        = true;
    tab->group_index  = -1;
    tab->split        = SPLIT_NONE;
    tab->split_ratio  = 0.5f;
    tab->split_pane_count = 1;
    tab->split_root = -1;
    tab->active_pane  = 0;
    tab->profile_index = -1;
    tab->last_activity_time = platform_time_sec();

    /* Derive title from the just-navigated cwd. Honors ~ and /. */
    app_refresh_fb_tab_title(tab);

    app->broadcast_targets[idx][0] = false;
    app->broadcast_targets[idx][1] = false;
    app->active_tab = idx;
    return idx;
}

const char *tab_effective_title(const Tab *tab) {
    if (tab->custom_title[0])
        return tab->custom_title;
    return tab->title;
}

void tab_format_display_title(const Tab *tab, char *buf, i32 buf_size) {
    if (!buf || buf_size <= 0) return;
    buf[0] = '\0';
    const char *title = tab ? tab_effective_title(tab) : "Terminal";
    snprintf(buf, (size_t)buf_size, "%s", title);
}

/* Draw a small crescent-moon icon using two rects: a base dot and an offset
 * cutout that carves the crescent shape. cutout_bg should match the tile bg. */
__attribute__((unused)) static void draw_sleep_moon(Renderer *r, f32 x, f32 y, f32 sz,
                                                    Color moon, Color cutout_bg) {
    renderer_draw_rect(r, x, y, sz, sz, moon);
    f32 c_sz = sz * 0.72f;
    f32 c_ox = sz * 0.38f;
    f32 c_oy = -sz * 0.06f;
    renderer_draw_rect(r, x + c_ox, y + c_oy, c_sz, c_sz, cutout_bg);
}

void app_start_tab_rename(AppState *app, i32 tab_index) {
    if (tab_index < 0 || tab_index >= app->tab_count) return;
    app->tab_rename_active = true;
    app->tab_rename_started_at = platform_time_sec();
    app->tab_rename_index = tab_index;
    const char *current = tab_effective_title(&app->tabs[tab_index]);
    snprintf(app->tab_rename_buf, sizeof(app->tab_rename_buf), "%s", current);
    app->tab_rename_len = (i32)strlen(app->tab_rename_buf);
}

void app_confirm_tab_rename(AppState *app) {
    if (!app->tab_rename_active) return;
    i32 idx = app->tab_rename_index;
    if (idx < 0 || idx >= app->tab_count) {
        app->tab_rename_active = false;
        return;
    }
    Tab *tab = &app->tabs[idx];
    if (app->tab_rename_len == 0) {
        /* Empty name: clear custom title and unlock */
        tab->custom_title[0] = '\0';
        tab->title_locked = false;
    } else {
        snprintf(tab->custom_title, sizeof(tab->custom_title), "%s", app->tab_rename_buf);
        tab->title_locked = true;
    }
    app->tab_rename_active = false;
}

void app_cancel_tab_rename(AppState *app) {
    app->tab_rename_active = false;
}

void app_start_chip_rename(AppState *app, i32 group_index) {
    if (group_index < 0 || group_index >= MAX_TAB_GROUPS) return;
    if (!app->tab_groups[group_index].used) return;
    app->chip_rename_active      = true;
    app->chip_rename_started_at  = platform_time_sec();
    app->chip_rename_group_index = group_index;
    snprintf(app->chip_rename_buf, sizeof(app->chip_rename_buf),
             "%s", app->tab_groups[group_index].name);
    app->chip_rename_len = (i32)strlen(app->chip_rename_buf);
}

void app_confirm_chip_rename(AppState *app) {
    if (!app->chip_rename_active) return;
    i32 gi = app->chip_rename_group_index;
    if (gi >= 0 && gi < MAX_TAB_GROUPS && app->tab_groups[gi].used &&
        app->chip_rename_len > 0) {
        app_rename_tab_group(app, gi, app->chip_rename_buf);
    }
    app->chip_rename_active = false;
}

void app_cancel_chip_rename(AppState *app) {
    app->chip_rename_active = false;
}

static void term_render_cache_free(TermRenderCache *c) {
    if (c->bg_rows) {
        for (i32 row = 0; row < c->cached_rows; row++) free(c->bg_rows[row]);
    }
    free(c->bg_rows);       c->bg_rows = NULL;
    free(c->bg_counts);     c->bg_counts = NULL;
    free(c->bg_caps);       c->bg_caps = NULL;
    if (c->glyph_rows) {
        for (i32 row = 0; row < c->cached_rows; row++) free(c->glyph_rows[row]);
    }
    free(c->glyph_rows);    c->glyph_rows = NULL;
    free(c->glyph_counts);  c->glyph_counts = NULL;
    free(c->glyph_caps);    c->glyph_caps = NULL;
    c->cached_rows = 0;
    c->cached_cols = 0;
    c->all_rows_dirty = true;
}

static void app_trim_inactive_render_caches(AppState *app) {
    if (!app) return;
    for (i32 i = 0; i < app->tab_count; i++) {
        if (i == app->active_tab) continue;
        term_render_cache_free(&app->tabs[i].cache1);
        term_render_cache_free(&app->tabs[i].cache2);
        for (i32 p = 2; p < MAX_SPLIT_PANES; p++)
            term_render_cache_free(&app->tabs[i].extra_caches[p - 2]);
    }
}

static void tab_sleep_snapshot_free(TabSleepSnapshot *snap) {
    if (!snap) return;
    if (snap->snap_path && snap->snap_path[0]) unlink(snap->snap_path);
    free(snap->snap_path);
    free(snap->cwd);
    if (snap->ssh_config) {
        ssh_config_dispose(snap->ssh_config);
        free(snap->ssh_config);
    }
    free(snap);
}

static void tab_sleep_snapshot_reset(TabSleepSnapshot **snap) {
    if (!snap || !*snap) return;
    tab_sleep_snapshot_free(*snap);
    *snap = NULL;
}

static TabSleepSnapshot *tab_sleep_snapshot_ensure(TabSleepSnapshot **snap) {
    if (!snap) return NULL;
    if (!*snap) {
        *snap = calloc(1, sizeof(**snap));
    }
    return *snap;
}

static void closed_tab_clear(ClosedTabInfo *info) {
    if (!info) return;
    if (info->ssh_config) {
        ssh_config_dispose(info->ssh_config);
        free(info->ssh_config);
    }
    free(info->cwd);
    memset(info, 0, sizeof(*info));
}

/* NOTE: callers guarantee tab_pane_sleepable() — telnet/serial never get
 * here, so the SSHConfig-only snapshot below stays sufficient. */
static void tab_snapshot_pane(Session *s, Terminal *t, TabSleepSnapshot **snap_out) {
    TabSleepSnapshot *snap = tab_sleep_snapshot_ensure(snap_out);
    if (!snap) return;

    snap->type = SESSION_LOCAL;
    if (snap->ssh_config) {
        ssh_config_dispose(snap->ssh_config);
        free(snap->ssh_config);
        snap->ssh_config = NULL;
    }
    free(snap->cwd);
    snap->cwd = NULL;
    if (snap->snap_path) snap->snap_path[0] = '\0';
    if (!s) return;

    snap->type = session_type(s);
    if (snap->type == SESSION_LOCAL) {
        if (t && t->cwd) {
            usize n = strlen(t->cwd);
            snap->cwd = malloc(n + 1);
            if (snap->cwd) memcpy(snap->cwd, t->cwd, n + 1);
        }
    } else {
        const SSHConfig *cfg = session_get_config(s);
        if (cfg) {
            snap->ssh_config = calloc(1, sizeof(*snap->ssh_config));
            if (snap->ssh_config && !ssh_config_clone(snap->ssh_config, cfg)) {
                free(snap->ssh_config);
                snap->ssh_config = NULL;
            }
        }
    }
}

static Session *tab_restore_pane(SessionType type, const SSHConfig *cfg,
                                 i32 cols, i32 rows) {
    switch (type) {
    case SESSION_SSH:
        return (cfg && cfg->hostname[0]) ? session_create_ssh(cfg, cols, rows) : NULL;
    case SESSION_MOSH:
        return (cfg && cfg->hostname[0]) ? session_create_mosh(cfg, cols, rows) : NULL;
    case SESSION_LOCAL:
    default:
        return session_create_local(cols, rows);
    }
}

SessionType tab_primary_session_type(const Tab *tab) {
    if (!tab) return SESSION_LOCAL;
    if (tab->session) return session_type(tab->session);
    return tab->sleep_pane1 ? tab->sleep_pane1->type : SESSION_LOCAL;
}

SessionType tab_secondary_session_type(const Tab *tab) {
    if (!tab) return SESSION_LOCAL;
    if (tab->session2) return session_type(tab->session2);
    return tab->sleep_pane2 ? tab->sleep_pane2->type : SESSION_LOCAL;
}

static bool sleep_snap_read(const char *path, Terminal *t);
static bool sleep_snap_write(const char *path, Terminal *t);
static void sleep_snap_path(char *out, usize n, i32 tab_idx, i32 pane);

bool app_wake_tab(AppState *app, i32 index) {
    if (!app || index < 0 || index >= app->tab_count) return false;
    Tab *tab = &app->tabs[index];
    TabSleepSnapshot *sleep1 = tab->sleep_pane1;
    TabSleepSnapshot *sleep2 = tab->sleep_pane2;
    if (!tab->sleeping) return true;
    if (!sleep1) return false;

    /* Compute per-pane dimensions honoring split_ratio for correct PTY setup */
    i32 grid_c = app->grid_cols > 0 ? app->grid_cols : 80;
    i32 grid_r = app->grid_rows > 0 ? app->grid_rows : 24;
    f32 ratio = (tab->split_ratio > 0.05f && tab->split_ratio < 0.95f)
                ? tab->split_ratio : 0.5f;

    i32 cols1 = grid_c, rows1 = grid_r;
    i32 cols2 = grid_c, rows2 = grid_r;
    if (tab->split == SPLIT_H) {
        cols1 = (i32)((f32)grid_c * ratio);
        cols2 = grid_c - cols1;
    } else if (tab->split == SPLIT_V) {
        rows1 = (i32)((f32)grid_r * ratio);
        rows2 = grid_r - rows1;
    }
    if (cols1 < 10) cols1 = 10;
    if (rows1 < 5)  rows1 = 5;
    if (cols2 < 10) cols2 = 10;
    if (rows2 < 5)  rows2 = 5;

    /* Pane 1: reuse suspended session if still alive, else recreate */
    Session *s1 = tab->session;
    bool reused_s1 = false;
    if (s1 && session_is_suspended(s1)) {
        if (session_resume(s1)) {
            reused_s1 = true;
            session_resize(s1, cols1, rows1);
        } else {
            session_destroy(s1);
            s1 = NULL;
        }
    } else if (s1) {
        /* Session somehow still live without being suspended; drop it */
        session_destroy(s1);
        s1 = NULL;
    }
    if (!s1) {
        s1 = tab_restore_pane(sleep1->type, sleep1->ssh_config, cols1, rows1);
    }
    if (!s1) return false;

    Terminal *t1 = terminal_create(cols1, rows1);
    if (!t1) {
        session_destroy(s1);
        tab->session = NULL;
        return false;
    }
    terminal_set_scrollback_limit(t1, app->config.scrollback_lines);
    t1->bidi_enabled = app->config.bidi_enabled;
    app_configure_terminal_callbacks(app, t1, s1);
    if (!reused_s1 && sleep1->type == SESSION_LOCAL && sleep1->cwd && sleep1->cwd[0]) {
        session_cd_to(s1, sleep1->cwd);
    }
    if (sleep1->snap_path && sleep1->snap_path[0]) {
        sleep_snap_read(sleep1->snap_path, t1);
        sleep1->snap_path[0] = '\0';
    }

    tab->session = s1;
    tab->terminal = t1;

    if (tab->split != SPLIT_NONE && sleep2) {
        Session *s2 = tab->session2;
        bool reused_s2 = false;
        if (s2 && session_is_suspended(s2)) {
            if (session_resume(s2)) {
                reused_s2 = true;
                session_resize(s2, cols2, rows2);
            } else {
                session_destroy(s2);
                s2 = NULL;
            }
        } else if (s2) {
            session_destroy(s2);
            s2 = NULL;
        }
        if (!s2) {
            s2 = tab_restore_pane(sleep2->type, sleep2->ssh_config, cols2, rows2);
        }
        Terminal *t2 = s2 ? terminal_create(cols2, rows2) : NULL;
        if (s2 && t2) {
            terminal_set_scrollback_limit(t2, app->config.scrollback_lines);
            t2->bidi_enabled = app->config.bidi_enabled;
            app_configure_terminal_callbacks(app, t2, s2);
            if (!reused_s2 && sleep2->type == SESSION_LOCAL && sleep2->cwd && sleep2->cwd[0]) {
                session_cd_to(s2, sleep2->cwd);
            }
            if (sleep2->snap_path && sleep2->snap_path[0]) {
                sleep_snap_read(sleep2->snap_path, t2);
                sleep2->snap_path[0] = '\0';
            }
            tab->session2 = s2;
            tab->terminal2 = t2;
        } else {
            if (t2) terminal_destroy(t2);
            if (s2) session_destroy(s2);
            tab->session2 = NULL;
            tab->terminal2 = NULL;
            tab->split = SPLIT_NONE;
            tab->active_pane = 0;  /* pane2 gone -> activate pane1 */
            app_show_toast(app, "Pane 2 restore failed; reverted to single pane");
        }
    } else if (tab->split == SPLIT_NONE) {
        tab->active_pane = 0;
    }

    /* Extra panes (3rd+): restore with the same resume-or-recreate pattern
     * as pane 2. Dims are approximate here — app_resize_tab_panes below
     * recomputes them from the split tree once the tab is awake. Panes that
     * fail to restore are collapsed afterwards (highest index first, so the
     * close's last→hole shuffle doesn't invalidate pending indices). */
    i32 failed_panes[MAX_SPLIT_PANES];
    i32 failed_count = 0;
    for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
        TabSleepSnapshot *snapx = tab->sleep_extra[p - 2];
        Session *xs = tab->extra_sessions[p - 2];
        if (!snapx && !xs) continue;
        bool reused_xs = false;
        if (xs && session_is_suspended(xs)) {
            if (session_resume(xs)) {
                reused_xs = true;
                session_resize(xs, grid_c, grid_r);
            } else {
                session_destroy(xs);
                xs = NULL;
            }
        } else if (xs) {
            session_destroy(xs);
            xs = NULL;
        }
        if (!xs && snapx) {
            xs = tab_restore_pane(snapx->type, snapx->ssh_config, grid_c, grid_r);
        }
        Terminal *xt = xs ? terminal_create(grid_c, grid_r) : NULL;
        if (xs && xt) {
            terminal_set_scrollback_limit(xt, app->config.scrollback_lines);
            xt->bidi_enabled = app->config.bidi_enabled;
            app_configure_terminal_callbacks(app, xt, xs);
            if (!reused_xs && snapx && snapx->type == SESSION_LOCAL &&
                snapx->cwd && snapx->cwd[0]) {
                session_cd_to(xs, snapx->cwd);
            }
            if (snapx && snapx->snap_path && snapx->snap_path[0]) {
                sleep_snap_read(snapx->snap_path, xt);
                snapx->snap_path[0] = '\0';
            }
            tab->extra_terminals[p - 2] = xt;
            tab->extra_sessions[p - 2]  = xs;
        } else {
            if (xt) terminal_destroy(xt);
            if (xs) session_destroy(xs);
            tab->extra_terminals[p - 2] = NULL;
            tab->extra_sessions[p - 2]  = NULL;
            failed_panes[failed_count++] = p;
        }
    }

    tab_sleep_snapshot_reset(&tab->sleep_pane1);
    tab_sleep_snapshot_reset(&tab->sleep_pane2);
    for (i32 p = 2; p < MAX_SPLIT_PANES; p++)
        tab_sleep_snapshot_reset(&tab->sleep_extra[p - 2]);
    tab->sleeping = false;
    tab->last_activity_time = platform_time_sec();
    tab->cache1.all_rows_dirty = true;
    tab->cache2.all_rows_dirty = true;
    for (i32 f = failed_count - 1; f >= 0; f--) {
        app_close_split_pane(app, index, failed_panes[f]);
        app_show_toast(app, "A split pane failed to restore and was closed");
    }
    if (tab->split_root >= 0 && tab_split_pane_count(tab) > 2)
        app_resize_tab_panes(app, tab);
    {
        char msg[192];
        snprintf(msg, sizeof(msg), "Restored tab: %s", tab_effective_title(tab));
        app_show_toast(app, msg);
    }
    app_trim_inactive_render_caches(app);
    return true;
}

/* Scrollback snapshot serialization — persists primary screen + scrollback
 * to a temp file so that wake can paint prior output without reconnecting.
 *
 * v1 (legacy): scrollback rows written as full Cell streams.
 * v2 (WT-hybrid): scrollback rows written in their native compressed form
 * (codepoints + RLE attribute runs + wide bitmap). 3-4× smaller on disk and
 * skips both materialize-on-write and re-compress-on-read passes. The reader
 * still understands v1 for cross-process robustness — but in practice every
 * snapshot file is keyed to a single PID via TMPDIR/liu-sleep-<pid>-…, so a
 * mismatched version means the user upgraded mid-snapshot which is rare. */
#define SLEEP_SNAP_MAGIC          0x4C535031u   /* 'LSP1' */
#define SLEEP_SNAP_VERSION_V1     1u
#define SLEEP_SNAP_VERSION_V2     2u
#define SLEEP_SNAP_VERSION        SLEEP_SNAP_VERSION_V2

typedef struct {
    u32 magic;
    u32 version;
    i32 cols;
    i32 rows;
    i32 cursor_x;
    i32 cursor_y;
    i32 sb_count;
    u32 _reserved;
} SleepSnapHeader;

/* Forward decls from buffer.c — exposed for the v1/v2 snapshot readers so
 * they can install rows directly into the scrollback ring. */
extern void sb_push_cold(Terminal *t, TermLineCompressed *cold);
extern void sb_push(Terminal *t, Cell *row_cells, i32 len, bool wrapped);

static void sleep_snap_path(char *out, usize n, i32 tab_idx, i32 pane) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(out, n, "%s/liu-sleep-%d-t%d-p%d.bin",
             dir, (int)getpid(), tab_idx, pane);
}

/* Write a single scrollback row in the v2 native compressed format:
 *   i32   len
 *   u8    wrapped
 *   u16   run_count
 *   u32   codepoints[len]
 *   u64   wide_bits[(len+63)/64]
 *   AttrRun runs[run_count]
 *
 * Hot rows (no cold pointer) are compressed inline into a transient stack
 * record. Cold rows write straight from their existing buffers — no
 * materialize, no re-encode. */
static bool snap_v2_write_row(FILE *f, const TermLine *ln) {
    /* Build a temporary view; if hot, compress on demand. */
    TermLineCompressed *temp = NULL;
    bool free_temp = false;
    const TermLineCompressed *src = ln->cold;
    if (!src) {
        if (!termline_compress(ln->cells, ln->len, ln->wrapped, &temp)) return false;
        free_temp = true;
        src = temp;
    }

    i32 len      = src->len;
    u8  wrapped  = src->wrapped ? 1 : 0;
    u16 runs     = src->run_count;
    bool ok = true;
    if (fwrite(&len,     sizeof(i32), 1, f) != 1) ok = false;
    if (ok && fwrite(&wrapped,  1,    1, f) != 1) ok = false;
    if (ok && fwrite(&runs,     sizeof(u16), 1, f) != 1) ok = false;
    if (ok && len > 0 && src->codepoints) {
        if (fwrite(src->codepoints, sizeof(u32), (usize)len, f) != (usize)len) ok = false;
    }
    if (ok && len > 0 && src->wide_bits) {
        usize wb_words = (usize)((len + 63) / 64);
        if (fwrite(src->wide_bits, sizeof(u64), wb_words, f) != wb_words) ok = false;
    }
    if (ok && runs > 0 && src->runs) {
        if (fwrite(src->runs, sizeof(AttrRun), runs, f) != runs) ok = false;
    }
    if (free_temp) termline_compressed_destroy(temp);
    return ok;
}

static bool sleep_snap_write(const char *path, Terminal *t) {
    if (!t || !path || !path[0]) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    SleepSnapHeader h = {
        .magic    = SLEEP_SNAP_MAGIC,
        .version  = SLEEP_SNAP_VERSION_V2,
        .cols     = t->cols,
        .rows     = t->rows,
        .cursor_x = t->cursor_x,
        .cursor_y = t->cursor_y,
        .sb_count = t->sb_count,
        ._reserved = 0,
    };
    if (fwrite(&h, sizeof(h), 1, f) != 1) goto err;

    /* Primary screen stays as a flat Cell stream — it's small (rows*cols
     * cells, no compression upside) and simplifies wake-time restore. */
    if (t->cells && t->rows > 0 && t->cols > 0) {
        usize n = (usize)t->rows * (usize)t->cols;
        if (fwrite(t->cells, sizeof(Cell), n, f) != n) goto err;
    }
    /* Scrollback rows in native compressed format — direct write from the
     * cold ring representation, no intermediate Cell scratch. */
    if (t->sb_ring && t->sb_capacity > 0) {
        for (i32 i = 0; i < t->sb_count; i++) {
            i32 idx = (t->sb_head + i) % t->sb_capacity;
            const TermLine *ln = &t->sb_ring[idx];
            if (!snap_v2_write_row(f, ln)) goto err;
        }
    }
    fclose(f);
    return true;
err:
    fclose(f);
    unlink(path);
    return false;
}

/* v1 row reader: legacy format with a flat Cell stream per row.
 * Always feeds through sb_push (which compresses), so nothing in the ring
 * stays as a hot Cell array. Returns false on read error. */
static bool snap_v1_read_row(FILE *f, Terminal *t,
                             Cell **row_buf, i32 *row_cap, bool skip) {
    i32 len; u8 wrapped;
    if (fread(&len, sizeof(i32), 1, f) != 1) return false;
    if (fread(&wrapped, 1, 1, f) != 1)        return false;
    if (skip) {
        if (len > 0 && fseek(f, (long)len * (long)sizeof(Cell), SEEK_CUR) != 0)
            return false;
        return true;
    }
    if (len > 0) {
        if (len > *row_cap) {
            Cell *grown = realloc(*row_buf, (usize)len * sizeof(Cell));
            if (!grown) {
                fseek(f, (long)len * (long)sizeof(Cell), SEEK_CUR);
                return true;
            }
            *row_buf = grown;
            *row_cap = len;
        }
        if (fread(*row_buf, sizeof(Cell), (usize)len, f) != (usize)len) return false;
        sb_push(t, *row_buf, len, wrapped != 0);
    } else {
        sb_push(t, NULL, 0, wrapped != 0);
    }
    return true;
}

/* v2 row reader: native compressed format. Reads codepoints + wide_bits +
 * RLE runs directly into a heap TermLineCompressed and hands it to
 * sb_push_cold which installs it without re-compression. Returns false on
 * read error. */
static bool snap_v2_read_row(FILE *f, Terminal *t, bool skip) {
    i32 len; u8 wrapped; u16 run_count;
    if (fread(&len, sizeof(i32), 1, f) != 1) return false;
    if (fread(&wrapped, 1, 1, f) != 1)        return false;
    if (fread(&run_count, sizeof(u16), 1, f) != 1) return false;

    /* Defensive bounds: discourage absurd allocations from a tampered file. */
    if (len < 0 || len > 16384 || run_count > (u16)len + 1) return false;
    usize wb_words = (usize)((len + 63) / 64);

    if (skip) {
        long advance = (long)len * (long)sizeof(u32)
                     + (long)wb_words * (long)sizeof(u64)
                     + (long)run_count * (long)sizeof(AttrRun);
        if (advance > 0 && fseek(f, advance, SEEK_CUR) != 0) return false;
        return true;
    }

    /* Build the line as a single contiguous block (same layout the compressor
     * produces) so termline_compressed_destroy / eviction free it correctly.
     * The previous split-alloc layout was incompatible with the destructor:
     * it double-freed on the fail path and leaked codepoints/wide_bits/runs on
     * every successful row. */
    TermLineCompressed *cold = termline_alloc_block(len, run_count, wrapped != 0);
    if (!cold) return false;

    if (len > 0) {
        if (fread(cold->codepoints, sizeof(u32), (usize)len, f) != (usize)len) goto fail;
        if (wb_words > 0 &&
            fread(cold->wide_bits, sizeof(u64), wb_words, f) != wb_words) goto fail;
    }
    if (run_count > 0) {
        if (fread(cold->runs, sizeof(AttrRun), run_count, f) != run_count) goto fail;
    }

    sb_push_cold(t, cold);   /* takes ownership */
    return true;

fail:
    termline_compressed_destroy(cold);
    return false;
}

static bool sleep_snap_read(const char *path, Terminal *t) {
    if (!t || !path || !path[0]) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    SleepSnapHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return false; }
    if (h.magic != SLEEP_SNAP_MAGIC ||
        (h.version != SLEEP_SNAP_VERSION_V1 &&
         h.version != SLEEP_SNAP_VERSION_V2) ||
        h.cols <= 0 || h.rows <= 0 ||
        h.cols > 4096 || h.rows > 4096) {
        fclose(f);
        unlink(path);
        return false;
    }

    /* Primary screen — same flat Cell stream in v1 and v2. */
    usize src_n = (usize)h.rows * (usize)h.cols;
    Cell *buf = malloc(src_n * sizeof(Cell));
    if (!buf) { fclose(f); return false; }
    if (fread(buf, sizeof(Cell), src_n, f) != src_n) {
        free(buf); fclose(f); return false;
    }

    i32 cp_rows = h.rows < t->rows ? h.rows : t->rows;
    i32 cp_cols = h.cols < t->cols ? h.cols : t->cols;
    if (t->cells && cp_rows > 0 && cp_cols > 0) {
        for (i32 y = 0; y < cp_rows; y++) {
            memcpy(&t->cells[y * t->cols],
                   &buf[y * h.cols],
                   (usize)cp_cols * sizeof(Cell));
        }
    }
    free(buf);

    t->cursor_x = h.cursor_x;
    t->cursor_y = h.cursor_y;
    if (t->cursor_x < 0) t->cursor_x = 0;
    if (t->cursor_y < 0) t->cursor_y = 0;
    if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
    if (t->cursor_y >= t->rows) t->cursor_y = t->rows - 1;

    /* Scrollback — version-dispatched per row. */
    i32 want = h.sb_count;
    if (want > 0) {
        i32 cap = t->scrollback_limit > 0 ? t->scrollback_limit : 2000;
        i32 skip = want > cap ? want - cap : 0;

        if (!t->sb_ring) {
            t->sb_capacity = cap;
            t->sb_ring = calloc((usize)t->sb_capacity, sizeof(TermLine));
            t->sb_head = 0;
            t->sb_count = 0;
        }
        if (t->sb_ring) {
            Cell *row_buf = NULL;
            i32   row_cap = 0;
            /* Skip the oldest rows that won't fit under the current limit. */
            for (i32 i = 0; i < skip; i++) {
                bool ok = (h.version == SLEEP_SNAP_VERSION_V1)
                        ? snap_v1_read_row(f, t, &row_buf, &row_cap, true)
                        : snap_v2_read_row(f, t, true);
                if (!ok) { want = skip + i; break; }
            }
            for (i32 i = skip; i < want; i++) {
                bool ok = (h.version == SLEEP_SNAP_VERSION_V1)
                        ? snap_v1_read_row(f, t, &row_buf, &row_cap, false)
                        : snap_v2_read_row(f, t, false);
                if (!ok) break;
            }
            free(row_buf);
            terminal_set_scrollback_limit(t, t->scrollback_limit);
        }
    }

    if (t->dirty_rows) {
        for (i32 w = 0; w < t->dirty_words; w++) t->dirty_rows[w] = ~(u64)0;
    }
    t->dirty = true;
    fclose(f);
    unlink(path);
    return true;
}

static void app_sleep_tab_internal(AppState *app, i32 i) {
    Tab *tab = &app->tabs[i];
    if (tab->sleeping || !tab->session || !tab->terminal) return;

    tab_sleep_snapshot_reset(&tab->sleep_pane1);
    tab_sleep_snapshot_reset(&tab->sleep_pane2);
    for (i32 p = 2; p < MAX_SPLIT_PANES; p++)
        tab_sleep_snapshot_reset(&tab->sleep_extra[p - 2]);

    tab_snapshot_pane(tab->session, tab->terminal, &tab->sleep_pane1);
    if (tab->split != SPLIT_NONE && tab->session2 && tab->terminal2) {
        tab_snapshot_pane(tab->session2, tab->terminal2, &tab->sleep_pane2);
    } else {
        tab_sleep_snapshot_reset(&tab->sleep_pane2);
    }

    /* Serialize scrollback/screen to disk before destroy.
     * snap_path is a fixed-size temp path -- alloc 512 B on demand. */
    enum { SLEEP_SNAP_PATH_LEN = 512 };
    if (tab->sleep_pane1) {
        if (!tab->sleep_pane1->snap_path)
            tab->sleep_pane1->snap_path = calloc(1, SLEEP_SNAP_PATH_LEN);
        if (tab->sleep_pane1->snap_path) {
            sleep_snap_path(tab->sleep_pane1->snap_path, SLEEP_SNAP_PATH_LEN, i, 0);
            if (!sleep_snap_write(tab->sleep_pane1->snap_path, tab->terminal)) {
                tab->sleep_pane1->snap_path[0] = '\0';
            }
        }
    }
    if (tab->sleep_pane2) {
        if (!tab->sleep_pane2->snap_path)
            tab->sleep_pane2->snap_path = calloc(1, SLEEP_SNAP_PATH_LEN);
        if (tab->sleep_pane2->snap_path) {
            sleep_snap_path(tab->sleep_pane2->snap_path, SLEEP_SNAP_PATH_LEN, i, 1);
            if (!sleep_snap_write(tab->sleep_pane2->snap_path, tab->terminal2)) {
                tab->sleep_pane2->snap_path[0] = '\0';
            }
        }
    }

    /* Prefer SIGSTOP over destroy: keeps the child process alive so shell
     * state (env, cwd, running programs) survives sleep. Fall back to full
     * destroy for SSH / unsupported platforms. */
    bool pane1_suspended = session_suspend(tab->session);
    terminal_destroy(tab->terminal);
    tab->terminal = NULL;
    if (pane1_suspended) {
        /* Drain any buffered PTY bytes so wake starts clean */
        u8 drain[4096];
        for (i32 tries = 0; tries < 8; tries++) {
            if (session_read(tab->session, drain, sizeof(drain)) <= 0) break;
        }
    } else {
        session_destroy(tab->session);
        tab->session = NULL;
    }

    bool pane2_suspended = false;
    if (tab->session2) {
        pane2_suspended = session_suspend(tab->session2);
    }
    if (tab->terminal2) terminal_destroy(tab->terminal2);
    tab->terminal2 = NULL;
    if (tab->session2) {
        if (pane2_suspended) {
            u8 drain[4096];
            for (i32 tries = 0; tries < 8; tries++) {
                if (session_read(tab->session2, drain, sizeof(drain)) <= 0) break;
            }
        } else {
            session_destroy(tab->session2);
            tab->session2 = NULL;
        }
    }

    /* Extra panes (3rd+): same snapshot + suspend-or-destroy treatment as
     * panes 1/2. Without this a 3+-pane tab "slept" while its extra
     * terminals kept their scrollback resident and their processes kept
     * running — sleep reclaimed nothing for those panes. */
    for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
        Terminal *xt = tab->extra_terminals[p - 2];
        Session  *xs = tab->extra_sessions[p - 2];
        if (!xt && !xs) continue;
        tab_snapshot_pane(xs, xt, &tab->sleep_extra[p - 2]);
        TabSleepSnapshot *snap = tab->sleep_extra[p - 2];
        if (snap && xt) {
            if (!snap->snap_path)
                snap->snap_path = calloc(1, SLEEP_SNAP_PATH_LEN);
            if (snap->snap_path) {
                sleep_snap_path(snap->snap_path, SLEEP_SNAP_PATH_LEN, i, p);
                if (!sleep_snap_write(snap->snap_path, xt))
                    snap->snap_path[0] = '\0';
            }
        }
        if (xt) {
            terminal_destroy(xt);
            tab->extra_terminals[p - 2] = NULL;
        }
        if (xs) {
            if (session_suspend(xs)) {
                u8 drain[4096];
                for (i32 tries = 0; tries < 8; tries++) {
                    if (session_read(xs, drain, sizeof(drain)) <= 0) break;
                }
            } else {
                session_destroy(xs);
                tab->extra_sessions[p - 2] = NULL;
            }
        }
        term_render_cache_free(&tab->extra_caches[p - 2]);
    }

    tab->sleeping = true;
    app->broadcast_targets[i][0] = false;
    app->broadcast_targets[i][1] = false;
    term_render_cache_free(&tab->cache1);
    term_render_cache_free(&tab->cache2);
    {
        char msg[192];
        snprintf(msg, sizeof(msg), "Slept tab: %s", tab_effective_title(tab));
        app_show_toast(app, msg);
    }
}

/* Telnet/serial sessions can't be snapshot-restored: TabSleepSnapshot only
 * carries an SSHConfig, and a serial line's device state wouldn't survive a
 * reconnect anyway. Such tabs simply never sleep. */
static bool tab_pane_sleepable(const Session *s) {
    if (!s) return true;
    SessionType t = session_type(s);
    return t != SESSION_TELNET && t != SESSION_SERIAL;
}

static bool tab_all_panes_sleepable(const Tab *tab) {
    if (!tab_pane_sleepable(tab->session) ||
        !tab_pane_sleepable(tab->session2)) return false;
    for (i32 p = 2; p < MAX_SPLIT_PANES; p++)
        if (!tab_pane_sleepable(tab->extra_sessions[p - 2])) return false;
    return true;
}

bool app_sleep_tab(AppState *app, i32 index) {
    if (!app || index < 0 || index >= app->tab_count) return false;
    Tab *tab = &app->tabs[index];
    if (tab->sleeping || !tab->session || !tab->terminal) return false;
    if (!tab_all_panes_sleepable(tab)) return false;
    app_sleep_tab_internal(app, index);
    return true;
}

void app_sleep_inactive_tabs(AppState *app, f64 now_sec) {
    if (!app || now_sec <= 0) return;
    if (app->config.tab_sleep_idle_minutes <= 0) return;
    f64 idle_limit = (f64)app->config.tab_sleep_idle_minutes * 60.0;

    for (i32 i = 0; i < app->tab_count; i++) {
        Tab *tab = &app->tabs[i];
        if (i == app->active_tab || tab->sleeping) continue;
        if (tab->sleep_disabled) continue;
        if (!tab->session || !tab->terminal) continue;
        if (!tab_all_panes_sleepable(tab)) continue;
        if (tab->last_activity_time <= 0) tab->last_activity_time = now_sec;
        if (now_sec - tab->last_activity_time < idle_limit) continue;

        app_sleep_tab_internal(app, i);
    }
}

static void closed_tab_push(AppState *app, const ClosedTabInfo *info) {
    if (app->closed_tab_count >= MAX_CLOSED_TABS) {
        /* Stack full — drop oldest (index 0), shift everything down */
        closed_tab_clear(&app->closed_tabs[0]);
        memmove(&app->closed_tabs[0], &app->closed_tabs[1],
                (usize)(MAX_CLOSED_TABS - 1) * sizeof(ClosedTabInfo));
        app->closed_tab_count = MAX_CLOSED_TABS - 1;
    }
    app->closed_tabs[app->closed_tab_count] = *info;
    app->closed_tab_count++;
}

static void app_snap_tab_render_positions(AppState *app) {
    if (!app) return;
    for (i32 i = 0; i < app->tab_count; i++) {
        app->tabs[i].render_x = 0.0f;
    }
}

void app_close_tab(AppState *app, i32 index) {
    if (index < 0 || index >= app->tab_count) return;

    /* A pending Cmd+drag pane layout move is keyed on tab_index. If THIS tab
     * is the drag target, cancel the drag before the array shifts so we
     * don't apply the eventual MOUSE_UP move to a wrong tab. If it's a
     * tab whose index is *above* the one being closed, decrement so the
     * drag continues to point at the same logical tab. */
    if (app->pane_drag_tab_index == index) {
        app->pane_drag_pending  = false;
        app->pane_drag_active   = false;
        app->pane_drag_tab_index = -1;
        app->pane_drag_src_pane  = -1;
        app->pane_drag_hover_pane = -1;
        app->pane_drag_drop_zone = 0;
    } else if (app->pane_drag_tab_index > index) {
        app->pane_drag_tab_index--;
    }

    Tab *tab = &app->tabs[index];

    /* Snapshot the closing pill for the exit ghost (drawn shrinking + fading
     * after the live pills, while the remaining tabs slide into the gap). */
    app->tab_close_active = true;
    app->tab_close_x = tab->render_x;
    app->tab_close_w = app->_tab_w;
    app->tab_close_was_active = (index == app->active_tab);
    app->tab_close_in_group = (tab->group_index >= 0);
    if (tab->group_index >= 0 && tab->group_index < MAX_TAB_GROUPS)
        app->tab_close_group_col = app->tab_groups[tab->group_index].color;
    anim_start(&app->tab_close_anim, 0.20);
    anim_register_until(platform_time_sec() + 0.34);   /* let neighbour springs settle */

    bool closes_translate_target = app->translate_target_term && app->translate_target_sess &&
        ((app->translate_target_term == tab->terminal &&
         app->translate_target_sess == tab->session) ||
        (app->translate_target_term == tab->terminal2 &&
         app->translate_target_sess == tab->session2));
    for (i32 p = 2; !closes_translate_target && p < MAX_SPLIT_PANES; p++) {
        closes_translate_target =
            app->translate_target_term == tab->extra_terminals[p - 2] &&
            app->translate_target_sess == tab->extra_sessions[p - 2];
    }
    if (app->translate_active && closes_translate_target) {
        app->translate_cancel_requested = true;
    }
    bool closes_pending_translate_tabs =
        app->translate_tab_target_term && app->translate_tab_target_sess &&
        ((app->translate_tab_target_term == tab->terminal &&
         app->translate_tab_target_sess == tab->session) ||
        (app->translate_tab_target_term == tab->terminal2 &&
         app->translate_tab_target_sess == tab->session2));
    for (i32 p = 2; !closes_pending_translate_tabs && p < MAX_SPLIT_PANES; p++) {
        closes_pending_translate_tabs =
            app->translate_tab_target_term == tab->extra_terminals[p - 2] &&
            app->translate_tab_target_sess == tab->extra_sessions[p - 2];
    }
    if (closes_pending_translate_tabs) {
        app->translate_tab_pending = 0;
        app->translate_tab_first_time = 0;
        app->translate_tab_last_time = 0;
        app->translate_tab_target_term = NULL;
        app->translate_tab_target_sess = NULL;
    }

    /* File-browser tabs have no PTY/SSH state to preserve and aren't
     * eligible for undo-close (re-open by drag is the natural path). */
    if (tab->kind == TAB_FILEBROWSER) {
        if (tab->fb) {
            /* Drop any transient viewer pointers that alias this fb before it
             * is freed, so a still-open palette outline mode can't dereference
             * a dangling FileBrowser on the next heading activation. */
            if (app->outline_fb == tab->fb) app->outline_fb = NULL;
            if (app->viewer_drag_source == tab->fb) app->viewer_drag_source = NULL;
            fb_destroy(tab->fb);
            free(tab->fb);
            tab->fb = NULL;
        }
        for (i32 i = index; i < app->tab_count - 1; i++) {
            app->tabs[i] = app->tabs[i + 1];
            app->broadcast_targets[i][0] = app->broadcast_targets[i + 1][0];
            app->broadcast_targets[i][1] = app->broadcast_targets[i + 1][1];
        }
        app->tab_count--;
        if (app->tab_count >= 0 && app->tab_count < MAX_TABS) {
            app->broadcast_targets[app->tab_count][0] = false;
            app->broadcast_targets[app->tab_count][1] = false;
        }
        if (app->active_tab >= app->tab_count) app->active_tab = app->tab_count - 1;
        if (app->active_tab < 0) app->active_tab = 0;
        app_snap_tab_render_positions(app);
        return;
    }

    /* Save tab info before destroying for undo-close */
    ClosedTabInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.valid = true;
    snprintf(ci.title, sizeof(ci.title), "%s", tab->title);
    ci.type = tab_primary_session_type(tab);

    if (ci.type == SESSION_SSH || ci.type == SESSION_MOSH) {
        const SSHConfig *cfg = tab->sleeping
            ? (tab->sleep_pane1 ? tab->sleep_pane1->ssh_config : NULL)
            : session_get_config(tab->session);
        if (cfg) {
            ci.ssh_config = calloc(1, sizeof(*ci.ssh_config));
            if (ci.ssh_config && !ssh_config_clone(ci.ssh_config, cfg)) {
                free(ci.ssh_config);
                ci.ssh_config = NULL;
            }
        }
    } else {
        /* Local session: save CWD from terminal OSC 7 */
        const char *src_cwd = NULL;
        if (tab->sleeping && tab->sleep_pane1 && tab->sleep_pane1->cwd
            && tab->sleep_pane1->cwd[0]) {
            src_cwd = tab->sleep_pane1->cwd;
        } else if (tab->terminal && tab->terminal->cwd) {
            src_cwd = tab->terminal->cwd;
        }
        if (src_cwd && *src_cwd) {
            usize n = strlen(src_cwd);
            ci.cwd = malloc(n + 1);
            if (ci.cwd) memcpy(ci.cwd, src_cwd, n + 1);
        } else {
            ci.cwd = NULL;
        }
    }

    closed_tab_push(app, &ci);

    /* Detach filebrowser from this tab's session BEFORE destroying it —
     * otherwise fb->session / fb->sftp_handle become dangling pointers
     * that the next sidebar interaction would dereference. Null-guard
     * each side: browsing a local dir leaves fb->session == NULL, and a
     * tab without a split leaves tab->session2 == NULL — without the
     * guards, NULL == NULL would spuriously detach a local-fs session. */
    if ((tab->session  && app->filebrowser.session == (void *)tab->session) ||
        (tab->session2 && app->filebrowser.session == (void *)tab->session2)) {
        fb_detach_sftp(&app->filebrowser);
    }
    for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
        if (tab->extra_sessions[p - 2] &&
            app->filebrowser.session == (void *)tab->extra_sessions[p - 2]) {
            fb_detach_sftp(&app->filebrowser);
            break;
        }
    }

    /* Same null-out treatment for modal dialogs whose `*_session` globals
     * may still point at this tab's session(s). Without this, a hostkey /
     * KBI / passphrase prompt left open while the tab is closed will UAF
     * on the user's next Enter/Esc — the trash worker frees the Session
     * underneath the dialog handler. */
    Session *tab_sessions[2 + (MAX_SPLIT_PANES - 2)];
    i32 ts_n = 0;
    if (tab->session)  tab_sessions[ts_n++] = tab->session;
    if (tab->session2) tab_sessions[ts_n++] = tab->session2;
    for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
        if (tab->extra_sessions[p - 2])
            tab_sessions[ts_n++] = tab->extra_sessions[p - 2];
    }
    for (i32 i = 0; i < ts_n; i++) {
        Session *s = tab_sessions[i];
        if (app->kbi_session == s) {
            app->kbi_session = NULL;
            app->kbi_dialog_active = false;
        }
        if (app->passphrase_session == s) {
            app->passphrase_session = NULL;
            app->passphrase_dialog_active = false;
            memset(app->passphrase_input, 0, sizeof(app->passphrase_input));
        }
        if (app->hostkey_session == s) {
            app->hostkey_session = NULL;
            app->hostkey_dialog_active = false;
        }
    }

    /* Unwatching the read-path fd is the responsibility of session_destroy —
     * it knows whether this session is the fd owner (ControlMaster-shared
     * non-owner sessions alias the owner's fd and must not cancel its watch).
     * Doing it here unconditionally would kill the owner's watch when a
     * borrowed tab closes, silently regressing the owner to 8 ms polling.
     *
     * Hand the heavy teardown (scrollback free, libssh2 disconnect, connect
     * worker join) to the trash thread so close + drag-reorder don't stall
     * the main loop. The pair is now owned by the background worker; nulling
     * the slot pointers prevents accidental double-free if a future caller
     * walks tabs[] before the array shift below. */
    if (!tab_trash_defer(tab->terminal, tab->session)) {
        terminal_destroy(tab->terminal);
        session_destroy(tab->session);
    }
    tab->terminal = NULL;
    tab->session  = NULL;
    /* Cleanup split pane if present */
    if (tab->terminal2 || tab->session2) {
        if (!tab_trash_defer(tab->terminal2, tab->session2)) {
            if (tab->terminal2) terminal_destroy(tab->terminal2);
            if (tab->session2)  session_destroy(tab->session2);
        }
        tab->terminal2 = NULL;
        tab->session2  = NULL;
    }
    for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
        Terminal *xt = tab->extra_terminals[p - 2];
        Session *xs = tab->extra_sessions[p - 2];
        if (xt || xs) {
            if (!tab_trash_defer(xt, xs)) {
                if (xt) terminal_destroy(xt);
                if (xs) session_destroy(xs);
            }
            tab->extra_terminals[p - 2] = NULL;
            tab->extra_sessions[p - 2] = NULL;
        }
    }
    tab_sleep_snapshot_reset(&tab->sleep_pane1);
    tab_sleep_snapshot_reset(&tab->sleep_pane2);
    for (i32 p = 2; p < MAX_SPLIT_PANES; p++)
        tab_sleep_snapshot_reset(&tab->sleep_extra[p - 2]);

    /* release render caches */
    term_render_cache_free(&tab->cache1);
    term_render_cache_free(&tab->cache2);
    for (i32 p = 2; p < MAX_SPLIT_PANES; p++)
        term_render_cache_free(&tab->extra_caches[p - 2]);
    free(tab->git_status.cached_cwd);
    tab->git_status.cached_cwd = NULL;

    for (i32 i = index; i < app->tab_count - 1; i++) {
        app->tabs[i] = app->tabs[i + 1];
        app->broadcast_targets[i][0] = app->broadcast_targets[i + 1][0];
        app->broadcast_targets[i][1] = app->broadcast_targets[i + 1][1];
    }
    app->tab_count--;
    if (app->tab_count >= 0 && app->tab_count < MAX_TABS) {
        app->broadcast_targets[app->tab_count][0] = false;
        app->broadcast_targets[app->tab_count][1] = false;
    }

    if (app->active_tab >= app->tab_count)
        app->active_tab = app->tab_count - 1;
    if (app->active_tab < 0) app->active_tab = 0;
    /* NOTE: deliberately NOT snapping render_x here — the array shift left each
     * surviving tab holding its previous on-screen x, so the per-frame spring
     * slides them into their new (wider) slots instead of jumping. The close
     * ghost (see render) fades out over the same window. */
}

bool app_undo_close_tab(AppState *app) {
    if (app->closed_tab_count <= 0) return false;

    /* Pop most recent entry (LIFO) */
    app->closed_tab_count--;
    ClosedTabInfo *ci = &app->closed_tabs[app->closed_tab_count];
    if (!ci->valid) return false;

    Session *s = NULL;

    if (ci->type == SESSION_SSH || ci->type == SESSION_MOSH) {
        s = (ci->type == SESSION_MOSH)
            ? session_create_mosh(ci->ssh_config,
                                  app->grid_cols > 0 ? app->grid_cols : 80,
                                  app->grid_rows > 0 ? app->grid_rows : 24)
            : session_create_ssh(ci->ssh_config,
                                 app->grid_cols > 0 ? app->grid_cols : 80,
                                 app->grid_rows > 0 ? app->grid_rows : 24);
    } else {
        s = session_create_local(app->grid_cols > 0 ? app->grid_cols : 80,
                                 app->grid_rows > 0 ? app->grid_rows : 24);
    }

    if (!s) {
        closed_tab_clear(ci);
        return false;
    }

    const char *title = ci->title[0] ? ci->title : "Terminal";
    i32 idx = app_new_tab(app, s, title);
    if (idx < 0) {
        session_destroy(s);
        closed_tab_clear(ci);
        return false;
    }

    /* For local sessions, cd to the saved CWD. Use session_cd_to so the path
     * is single-quote-escaped — ci->cwd comes from OSC 7 (an untrusted terminal
     * stream) and an unescaped "cd '%s'" would let a crafted cwd inject shell
     * commands into the restored tab. */
    if (ci->type == SESSION_LOCAL && ci->cwd && ci->cwd[0]) {
        session_cd_to(s, ci->cwd);
    }

    closed_tab_clear(ci);
    return true;
}

void app_switch_tab(AppState *app, i32 index) {
    if (index >= 0 && index < app->tab_count) {
        /* Switching away from the dragged tab kills the pane layout drag —
         * its feedback only renders on the original tab, and a silent
         * swap in a hidden tab on the next mouseup is worse than a
         * dropped drag the user can simply re-initiate. */
        if (app->pane_drag_tab_index >= 0 &&
            app->pane_drag_tab_index != index) {
            app->pane_drag_pending  = false;
            app->pane_drag_active   = false;
            app->pane_drag_tab_index = -1;
            app->pane_drag_src_pane  = -1;
            app->pane_drag_hover_pane = -1;
            app->pane_drag_drop_zone = 0;
        }
        app->active_tab = index;
        if (!app->tabs[index].sleeping)
            app->tabs[index].last_activity_time = platform_time_sec();
        /* Clear bell indicator for the newly focused tab */
        app->tabs[index].has_bell = false;
        /* Clear dock badge if no other tabs have pending bells */
        bool any_bell = false;
        for (i32 i = 0; i < app->tab_count; i++) {
            if (i != index && app->tabs[i].has_bell) {
                any_bell = true;
                break;
            }
        }
        if (!any_bell) platform_set_dock_badge(NULL);
        app_trim_inactive_render_caches(app);

        /* Re-point the (single, shared) filebrowser at the newly focused
         * tab's remote FS when the sidebar is open. Without this, switching
         * between SSH tabs would leave the sidebar showing the previous
         * tab's directory listing while operating on a handle that belongs
         * to a different session. */
        Tab *tab = &app->tabs[index];
        if (app->sidebar_visible && app->filebrowser.open &&
            tab->session && session_type(tab->session) == SESSION_SSH &&
            session_status(tab->session) == SESSION_CONNECTED) {
            void *sftp = session_sftp_handle(tab->session);
            const char *target = (tab->terminal && tab->terminal->cwd && tab->terminal->cwd[0])
                                 ? tab->terminal->cwd
                                 : session_initial_cwd(tab->session);
            if (sftp && target && target[0]) {
                app->filebrowser.session = (void *)tab->session;
                fb_navigate_sftp(&app->filebrowser, sftp, target);
            }
        } else if (app->sidebar_visible && app->filebrowser.open &&
                   tab->session && session_type(tab->session) == SESSION_LOCAL &&
                   tab->terminal && tab->terminal->cwd && tab->terminal->cwd[0]) {
            app->filebrowser.session = NULL;
            fb_navigate(&app->filebrowser, tab->terminal->cwd);
        }
    }
}

void app_move_tab(AppState *app, i32 from, i32 to) {
    if (from < 0 || from >= app->tab_count) return;
    if (to < 0 || to >= app->tab_count) return;
    if (from == to) return;

    Tab tmp = app->tabs[from];
    bool target0 = app->broadcast_targets[from][0];
    bool target1 = app->broadcast_targets[from][1];
    if (from < to) {
        for (i32 i = from; i < to; i++) {
            app->tabs[i] = app->tabs[i + 1];
            app->broadcast_targets[i][0] = app->broadcast_targets[i + 1][0];
            app->broadcast_targets[i][1] = app->broadcast_targets[i + 1][1];
        }
    } else {
        for (i32 i = from; i > to; i--) {
            app->tabs[i] = app->tabs[i - 1];
            app->broadcast_targets[i][0] = app->broadcast_targets[i - 1][0];
            app->broadcast_targets[i][1] = app->broadcast_targets[i - 1][1];
        }
    }
    app->tabs[to] = tmp;
    app->broadcast_targets[to][0] = target0;
    app->broadcast_targets[to][1] = target1;

    /* Keep active_tab following the correct tab */
    if (app->active_tab == from) {
        app->active_tab = to;
    } else if (from < to) {
        if (app->active_tab > from && app->active_tab <= to)
            app->active_tab--;
    } else {
        if (app->active_tab >= to && app->active_tab < from)
            app->active_tab++;
    }
}

Tab *app_active_tab(AppState *app) {
    if (app->tab_count == 0) return NULL;
    return &app->tabs[app->active_tab];
}

/* Load the last 5 commands run in the active tab's working directory into the
 * popup. Folder-scoped: the source is the central per-project cmdhistory, so the popup
 * shows what you ran in *this* project, not one global list. */
void app_extract_cmd_history(AppState *app) {
    if (!app) return;
    app->cmd_history_count = 0;
    memset(app->cmd_history, 0, sizeof(app->cmd_history));
    if (app->cmd_history_raw) memset(app->cmd_history_raw, 0, 5 * sizeof(*app->cmd_history_raw));

    Tab *tab = app_active_tab(app);
    if (!tab) return;
    Terminal *t = tab_pane_terminal(tab, tab->active_pane);
    Session  *s = tab_pane_session(tab, tab->active_pane);
    const char *cwd = app_terminal_cwd(t, s);
    if (!cwd || !cwd[0]) return;

    const CmdHistory *h = cmd_history_get(cwd);
    i32 n = cmd_history_count(h);
    /* Walk newest -> oldest; app_history_append fills slot 0 first. */
    for (i32 i = n - 1; i >= 0 && app->cmd_history_count < 5; i--) {
        app_history_append(app, cmd_history_entry(h, i));
    }
}

/* Split the active tab */
void app_split_tab(AppState *app, SplitType dir) {
    app_split_tab_dir(app, dir, false);
}

void app_split_tab_dir(AppState *app, SplitType dir, bool new_first) {
    Tab *tab = app_active_tab(app);
    if (!tab || tab->kind != TAB_TERMINAL || tab->sleeping) return;

    f32 ox = 0, oy = 0, total_w = 0, total_h = 0;
    app_terminal_content_rect(app, &ox, &oy, &total_w, &total_h);
    f32 cw = app->renderer.font.cell_width > 1.0f ? app->renderer.font.cell_width : 8.0f;
    f32 ch = app->renderer.font.cell_height > 1.0f ? app->renderer.font.cell_height : 16.0f;

    if (dir == SPLIT_NONE) dir = SPLIT_H;

    i32 count = tab_split_pane_count(tab);
    if (count >= MAX_SPLIT_PANES) {
        app_show_toast(app, "Split limit reached (8 panes)");
        return;
    }

    PaneRect old_rects[MAX_SPLIT_PANES];
    if (tab->split == SPLIT_NONE) {
        for (i32 i = 0; i < MAX_SPLIT_PANES; i++) old_rects[i] = (PaneRect){0};
        old_rects[0] = (PaneRect){ox, oy, total_w, total_h};
    } else {
        if (tab->split_root < 0) {
            tab->split_pane_count = count;
            tab_split_init_tree(tab, tab->split, false);
        }
        tab_split_layout_rects(tab, ox, oy, total_w, total_h,
                               app->dpi_scale, old_rects);
    }

    i32 active = tab->active_pane;
    if (active < 0 || active >= count) active = 0;
    PaneRect target = old_rects[active];
    SplitType split_dir = dir;
    if (tab->split != SPLIT_NONE) {
        split_dir = (target.h > target.w) ? SPLIT_V : SPLIT_H;
    }

    i32 cols = (i32)((split_dir == SPLIT_H ? target.w * 0.5f : target.w) / cw);
    i32 rows = (i32)((split_dir == SPLIT_V ? target.h * 0.5f : target.h) / ch);
    if (cols < 10) cols = 10;
    if (rows < 5) rows = 5;

    /* Inherit the active pane's current directory so a split opens where the
     * user actually IS, not in $HOME. Two-source resolution mirrors the new-
     * tab / Agent-History pattern: OSC 7 first (set by zsh/bash when shells
     * emit it), then proc_pidinfo via session_local_cwd as a fallback that
     * works with any unmodified shell. Skip the inheritance entirely for SSH
     * panes — that "cwd" is a remote path that probably doesn't exist
     * locally, so let the split fall back to the default $HOME landing. */
    const char *split_cwd = app_active_local_cwd(app);

    Session *new_s = session_create_local_with_env(cols, rows, NULL, 0, split_cwd);
    Terminal *new_t = terminal_create(cols, rows);
    if (!new_s || !new_t) {
        if (new_t) terminal_destroy(new_t);
        if (new_s) session_destroy(new_s);
        return;
    }
    terminal_set_scrollback_limit(new_t, app->config.scrollback_lines);
    new_t->bidi_enabled = app->config.bidi_enabled;
    app_configure_terminal_callbacks(app, new_t, new_s);

    if (tab->split == SPLIT_NONE) {
        tab->split = split_dir;
        tab->split_ratio = 0.5f;
        tab->split_pane_count = 2;
        tab->terminal2 = new_t;
        tab->session2 = new_s;
        app->broadcast_targets[app->active_tab][1] = (app->broadcast_mode && tab->session2);

        if (new_first) {
            Session  *ts = tab->session;   tab->session  = tab->session2;   tab->session2  = ts;
            Terminal *tt = tab->terminal;  tab->terminal = tab->terminal2;  tab->terminal2 = tt;
            bool b0 = app->broadcast_targets[app->active_tab][0];
            app->broadcast_targets[app->active_tab][0] = app->broadcast_targets[app->active_tab][1];
            app->broadcast_targets[app->active_tab][1] = b0;
            tab->active_pane = 0;
        } else {
            tab->active_pane = 1;
        }
        tab_split_init_tree(tab, split_dir, false);
        PaneRect final_rects[MAX_SPLIT_PANES];
        tab_split_layout_rects(tab, ox, oy, total_w, total_h,
                               app->dpi_scale, final_rects);
        i32 source_after_split = new_first ? 1 : 0;
        tab_split_start_hy3_open_anim(tab, old_rects, final_rects,
                                      source_after_split, tab->active_pane);
        app_resize_tab_panes(app, tab);
        return;
    }

    tab_set_pane_refs(tab, count, new_t, new_s);

    i32 leaf = tab_split_find_leaf_node(tab, tab->split_root, active);
    if (leaf < 0) {
        terminal_destroy(new_t);
        session_destroy(new_s);
        tab_set_pane_refs(tab, count, NULL, NULL);
        return;
    }
    i32 old_leaf = tab_split_make_leaf(tab, active);
    i32 new_leaf = tab_split_make_leaf(tab, count);
    if (old_leaf < 0 || new_leaf < 0) {
        terminal_destroy(new_t);
        session_destroy(new_s);
        tab_set_pane_refs(tab, count, NULL, NULL);
        return;
    }

    SplitLayoutNode *n = &tab->split_nodes[leaf];
    n->leaf = false;
    n->split = split_dir;
    n->ratio = 0.5f;
    n->pane = -1;
    n->first = new_first ? new_leaf : old_leaf;
    n->second = new_first ? old_leaf : new_leaf;

    tab->split_pane_count = count + 1;
    tab->active_pane = count;
    if (tab->split_root >= 0) tab->split = tab->split_nodes[tab->split_root].split;
    PaneRect final_rects[MAX_SPLIT_PANES];
    tab_split_layout_rects(tab, ox, oy, total_w, total_h,
                           app->dpi_scale, final_rects);
    tab_split_start_hy3_open_anim(tab, old_rects, final_rects,
                                  active, count);

    app_resize_tab_panes(app, tab);
}

Session *app_pane_session(const Tab *tab, i32 pane) {
    return tab_pane_session(tab, pane);
}

Terminal *app_pane_terminal(const Tab *tab, i32 pane) {
    return tab_pane_terminal(tab, pane);
}

i32 app_pane_index_at(AppState *app, const Tab *tab, f32 x, f32 y) {
    if (!app || !tab) return -1;
    if (tab->split == SPLIT_NONE) {
        f32 ox, oy, tw, th;
        app_terminal_content_rect(app, &ox, &oy, &tw, &th);
        if (x < ox || y < oy || x >= ox + tw || y >= oy + th) return -1;
        return 0;
    }
    f32 ox, oy, total_w, total_h;
    app_terminal_content_rect(app, &ox, &oy, &total_w, &total_h);
    if (x < ox || y < oy || x >= ox + total_w || y >= oy + total_h) return -1;

    /* Multi-pane layout tree wins when available (split_root >= 0) —
     * the binary tab->split + split_ratio is only an authoritative
     * geometry source when no tree has been built. */
    i32 count = tab_split_pane_count(tab);
    if (tab->split_root >= 0) {
        PaneRect rects[MAX_SPLIT_PANES];
        tab_split_layout_rects(tab, ox, oy, total_w, total_h,
                               app->dpi_scale, rects);
        for (i32 p = 0; p < count; p++) {
            const PaneRect *rr = &rects[p];
            if (rr->w <= 0.0f || rr->h <= 0.0f) continue;
            if (x >= rr->x && y >= rr->y &&
                x <  rr->x + rr->w && y <  rr->y + rr->h) {
                return p;
            }
        }
        return -1;
    }
    /* Binary fallback. `>=` matches the renderer's intent that clicks
     * landing exactly on the divider line belong to the right/bottom
     * pane — consistent with the multi-pane `x < rr->x + rr->w` check
     * above, which treats the divider as the right pane's left edge. */
    if (tab->split == SPLIT_H) {
        f32 div_x = ox + total_w * tab->split_ratio;
        return (x >= div_x) ? 1 : 0;
    }
    if (tab->split == SPLIT_V) {
        f32 div_y = oy + total_h * tab->split_ratio;
        return (y >= div_y) ? 1 : 0;
    }
    return 0;
}

i32 app_pane_drop_zone_at(AppState *app, const Tab *tab,
                          i32 pane, f32 x, f32 y) {
    if (!app || !tab || pane < 0) return 0;
    f32 ox = 0, oy = 0, total_w = 0, total_h = 0;
    app_terminal_content_rect(app, &ox, &oy, &total_w, &total_h);
    PaneRect rr = {0};
    if (tab->split_root >= 0) {
        PaneRect rects[MAX_SPLIT_PANES];
        tab_split_layout_rects(tab, ox, oy, total_w, total_h,
                               app->dpi_scale, rects);
        if (pane >= MAX_SPLIT_PANES) return 0;
        rr = rects[pane];
    } else if (tab->split == SPLIT_H) {
        f32 div = total_w * tab->split_ratio;
        f32 dw = 2.0f * app->dpi_scale;
        rr = (pane == 0)
           ? (PaneRect){ox, oy, div - dw * 0.5f, total_h}
           : (PaneRect){ox + div + dw * 0.5f, oy,
                        total_w - div - dw * 0.5f, total_h};
    } else if (tab->split == SPLIT_V) {
        f32 div = total_h * tab->split_ratio;
        f32 dh = 2.0f * app->dpi_scale;
        rr = (pane == 0)
           ? (PaneRect){ox, oy, total_w, div - dh * 0.5f}
           : (PaneRect){ox, oy + div + dh * 0.5f, total_w,
                        total_h - div - dh * 0.5f};
    } else {
        return 0;
    }
    if (rr.w <= 1.0f || rr.h <= 1.0f) return 0;

    f32 rx = (x - rr.x) / rr.w;
    f32 ry = (y - rr.y) / rr.h;
    if (rx < 0.0f) rx = 0.0f;
    if (rx > 1.0f) rx = 1.0f;
    if (ry < 0.0f) ry = 0.0f;
    if (ry > 1.0f) ry = 1.0f;

    f32 dl = rx;
    f32 dr = 1.0f - rx;
    f32 dt = ry;
    f32 db = 1.0f - ry;
    i32 zone = 1;
    f32 best = dl;
    if (dr < best) { best = dr; zone = 2; }
    if (dt < best) { best = dt; zone = 3; }
    if (db < best) { zone = 4; }
    return zone;
}

/* Linear lookup of a Tab*'s index in app->tabs[]. Replaces direct
 * pointer subtraction so callers passing a Tab* from a different
 * allocation can't trigger UB / corrupt the broadcast_targets row. */
static i32 app_tab_index_of(const AppState *app, const Tab *tab) {
    if (!app || !tab) return -1;
    for (i32 i = 0; i < app->tab_count; i++) {
        if (&app->tabs[i] == tab) return i;
    }
    return -1;
}

void app_move_pane_to_zone(AppState *app, Tab *tab, i32 src, i32 dst, i32 zone) {
    if (!app || !tab || src == dst) return;
    i32 count = tab_split_pane_count(tab);
    if (src < 0 || dst < 0 || src >= count || dst >= count) return;
    if (zone < 1 || zone > 4) return;

    if (tab->split_root < 0) {
        if (tab->split == SPLIT_NONE) return;
        tab_split_init_tree(tab, tab->split, false);
    }
    if (tab->split_root < 0) return;

    f32 ox = 0, oy = 0, total_w = 0, total_h = 0;
    app_terminal_content_rect(app, &ox, &oy, &total_w, &total_h);
    PaneRect old_rects[MAX_SPLIT_PANES];
    tab_split_layout_rects(tab, ox, oy, total_w, total_h,
                           app->dpi_scale, old_rects);

    i32 src_leaf = tab_split_find_leaf_node(tab, tab->split_root, src);
    i32 dst_leaf = tab_split_find_leaf_node(tab, tab->split_root, dst);
    if (src_leaf < 0 || dst_leaf < 0 || src_leaf == dst_leaf) return;

    i32 src_parent = tab_split_find_parent_node(tab, tab->split_root, src_leaf);
    if (src_parent < 0) return;
    SplitLayoutNode *sp = &tab->split_nodes[src_parent];
    i32 sibling = (sp->first == src_leaf) ? sp->second : sp->first;
    if (sibling < 0) return;

    i32 src_grand = tab_split_find_parent_node(tab, tab->split_root, src_parent);
    if (src_grand < 0) tab->split_root = sibling;
    else tab_split_replace_child(tab, src_grand, src_parent, sibling);

    SplitType split = (zone == 1 || zone == 2) ? SPLIT_H : SPLIT_V;
    bool source_first = (zone == 1 || zone == 3);
    SplitLayoutNode *insert = &tab->split_nodes[src_parent];
    memset(insert, 0, sizeof(*insert));
    insert->used = true;
    insert->leaf = false;
    insert->split = split;
    insert->ratio = 0.5f;
    insert->pane = -1;
    insert->first = source_first ? src_leaf : dst_leaf;
    insert->second = source_first ? dst_leaf : src_leaf;

    i32 dst_parent = tab_split_find_parent_node(tab, tab->split_root, dst_leaf);
    if (dst_parent < 0) tab->split_root = src_parent;
    else tab_split_replace_child(tab, dst_parent, dst_leaf, src_parent);

    if (tab->split_root >= 0) tab->split = tab->split_nodes[tab->split_root].split;
    tab->active_pane = src;

    for (i32 i = 0; i < MAX_SPLIT_PANES; i++) {
        tab->split_move_from[i] = (SplitAnimRect){
            old_rects[i].x, old_rects[i].y, old_rects[i].w, old_rects[i].h
        };
    }
    f64 now = platform_time_sec();
    tab->split_move_animating = true;
    tab->split_move_anim_start = now;
    anim_register_until(now + SPLIT_MOVE_DUR);

    app_resize_tab_panes(app, tab);
}

void app_swap_panes(AppState *app, Tab *tab, i32 a, i32 b) {
    if (!app || !tab || a == b) return;
    i32 count = tab_split_pane_count(tab);
    if (a < 0 || b < 0 || a >= count || b >= count) return;

    /* Capture pre-swap geometry so split_move_from[] can be refreshed —
     * otherwise the next layout-driven animation (window resize, sidebar
     * toggle) interpolates each pane from the previous occupant's rect
     * and the user sees a snap that doesn't match the swap. */
    f32 ox = 0, oy = 0, total_w = 0, total_h = 0;
    app_terminal_content_rect(app, &ox, &oy, &total_w, &total_h);
    PaneRect pre_rects[MAX_SPLIT_PANES];
    tab_split_layout_rects(tab, ox, oy, total_w, total_h,
                           app->dpi_scale, pre_rects);

    Terminal *ta = tab_pane_terminal(tab, a);
    Session  *sa = tab_pane_session(tab, a);
    Terminal *tb = tab_pane_terminal(tab, b);
    Session  *sb = tab_pane_session(tab, b);
    tab_set_pane_refs(tab, a, tb, sb);
    tab_set_pane_refs(tab, b, ta, sa);

    /* Swap the per-pane render caches in lockstep. The cache and its terminal
     * move together (tab_set_pane_refs above), so after the swap each cache's
     * cached_rows/cached_cols still match the per-row bg_rows[]/glyph_rows[]
     * buffers it carries — and term_render_cache_free frees those buffers in a
     * `row < cached_rows` loop. Do NOT zero cached_rows/cached_cols here: that
     * orphaned every per-row buffer of both panes (the outer arrays got freed
     * but the loop ran 0 times), leaking on every swap. all_rows_dirty forces
     * a rebuild; app_resize_tab_panes at the bottom reconciles geometry. */
    TermRenderCache *ca = tab_pane_cache(tab, a);
    TermRenderCache *cb = tab_pane_cache(tab, b);
    if (ca && cb) {
        TermRenderCache tmp = *ca;
        *ca = *cb;
        *cb = tmp;
        ca->all_rows_dirty = true;
        cb->all_rows_dirty = true;
    }

    /* Broadcast targets follow the visual slot, not the terminal — so
     * the broadcast-input arrows stay anchored to the position the user
     * placed them in. Only the binary-split slots (0/1) have a
     * dedicated bool; multi-pane broadcast is a known limitation
     * (only 2 slots in app->broadcast_targets[*][2]). */
    i32 tab_idx = app_tab_index_of(app, tab);
    if (tab_idx >= 0 && a < 2 && b < 2) {
        bool tmp = app->broadcast_targets[tab_idx][a];
        app->broadcast_targets[tab_idx][a] = app->broadcast_targets[tab_idx][b];
        app->broadcast_targets[tab_idx][b] = tmp;
    }

    /* Active pane follows the *terminal* the user was focused on. */
    if (tab->active_pane == a) tab->active_pane = b;
    else if (tab->active_pane == b) tab->active_pane = a;

    /* Animate the visual slots rather than flashing a color. After the
     * pointer swap, slot `a` holds the terminal that used to live in slot
     * `b`, so it starts at b's old rect and moves to a's rect; same for
     * the other side. Other panes keep no-op rects. */
    i32 pre_count = count;
    if (pre_count > MAX_SPLIT_PANES) pre_count = MAX_SPLIT_PANES;
    for (i32 i = 0; i < pre_count; i++) {
        tab->split_move_from[i] = (SplitAnimRect){
            pre_rects[i].x, pre_rects[i].y, pre_rects[i].w, pre_rects[i].h
        };
    }
    if (a < MAX_SPLIT_PANES && b < MAX_SPLIT_PANES) {
        tab->split_move_from[a] = (SplitAnimRect){
            pre_rects[b].x, pre_rects[b].y, pre_rects[b].w, pre_rects[b].h
        };
        tab->split_move_from[b] = (SplitAnimRect){
            pre_rects[a].x, pre_rects[a].y, pre_rects[a].w, pre_rects[a].h
        };
    }
    f64 now = platform_time_sec();
    tab->split_move_animating = true;
    tab->split_move_anim_start = now;
    anim_register_until(now + SPLIT_MOVE_DUR);
    tab->swap_flash_slot_a = -1;
    tab->swap_flash_slot_b = -1;

    /* Resize panes so each terminal gets the dimensions of its new
     * slot — slot rects didn't change, but the terminals behind them
     * did, and their PTYs need a SIGWINCH for the right wrap column. */
    app_resize_tab_panes(app, tab);
}

/* Collapse a split tab to a single pane by killing one side. Mirrors the
 * tab-close path: the dead pane's Terminal+Session go to the background
 * trash queue, render caches are dropped, and the survivor is resized
 * back to the full tab grid so its shell wraps at the right column. */
void app_close_split_pane(AppState *app, i32 tab_index, i32 pane) {
    if (!app || tab_index < 0 || tab_index >= app->tab_count) return;
    Tab *tab = &app->tabs[tab_index];
    if (tab->split == SPLIT_NONE) return;
    if (tab->sleeping) return;
    i32 count = tab_split_pane_count(tab);
    if (pane < 0 || pane >= count) return;

    /* Cancel any in-flight pane layout drag that's keyed on this tab —
     * the dead pane's index becomes ambiguous once the slots shift. */
    if (app->pane_drag_tab_index == tab_index) {
        app->pane_drag_pending  = false;
        app->pane_drag_active   = false;
        app->pane_drag_tab_index = -1;
        app->pane_drag_src_pane  = -1;
        app->pane_drag_hover_pane = -1;
        app->pane_drag_drop_zone = 0;
    }

    if (count > 2 && tab->split_root >= 0) {
        f32 ox = 0, oy = 0, total_w = 0, total_h = 0;
        app_terminal_content_rect(app, &ox, &oy, &total_w, &total_h);
        PaneRect old_rects[MAX_SPLIT_PANES];
        tab_split_layout_rects(tab, ox, oy, total_w, total_h,
                               app->dpi_scale, old_rects);

        Terminal *dead_t = tab_pane_terminal(tab, pane);
        Session *dead_s = tab_pane_session(tab, pane);
        TermRenderCache *dead_cache = tab_pane_cache(tab, pane);

        i32 leaf = tab_split_find_leaf_node(tab, tab->split_root, pane);
        i32 parent = tab_split_find_parent_node(tab, tab->split_root, leaf);
        if (leaf < 0 || parent < 0) return;
        SplitLayoutNode *pn = &tab->split_nodes[parent];
        i32 sibling = (pn->first == leaf) ? pn->second : pn->first;
        i32 grand = tab_split_find_parent_node(tab, tab->split_root, parent);
        if (grand < 0) tab->split_root = sibling;
        else tab_split_replace_child(tab, grand, parent, sibling);
        tab->split_nodes[leaf].used = false;
        tab->split_nodes[parent].used = false;

        i32 last = count - 1;
        i32 old_active = tab->active_pane;
        if (pane != last) {
            Terminal *move_t = tab_pane_terminal(tab, last);
            Session *move_s = tab_pane_session(tab, last);
            TermRenderCache *move_cache = tab_pane_cache(tab, last);
            if (dead_cache) term_render_cache_free(dead_cache);
            tab_set_pane_refs(tab, pane, move_t, move_s);
            tab_set_pane_refs(tab, last, NULL, NULL);
            if (dead_cache && move_cache) {
                *dead_cache = *move_cache;
                memset(move_cache, 0, sizeof(*move_cache));
            }
            tab_split_update_leaf_pane(tab, tab->split_root, last, pane);
        } else {
            if (dead_cache) term_render_cache_free(dead_cache);
            tab_set_pane_refs(tab, last, NULL, NULL);
        }

        if (old_active == pane) tab->active_pane = 0;
        else if (old_active == last && pane != last) tab->active_pane = pane;
        else tab->active_pane = old_active;
        tab->split_pane_count = count - 1;
        if (tab->active_pane >= tab->split_pane_count) tab->active_pane = tab->split_pane_count - 1;
        if (tab->active_pane < 0) tab->active_pane = 0;
        if (tab->split_root >= 0) tab->split = tab->split_nodes[tab->split_root].split;

        if (app->translate_target_term == dead_t ||
            app->translate_target_sess == dead_s) {
            app->translate_target_term = NULL;
            app->translate_target_sess = NULL;
            app->translate_cancel_requested = true;
        }
        if (app->translate_tab_target_term == dead_t ||
            app->translate_tab_target_sess == dead_s) {
            app->translate_tab_target_term = NULL;
            app->translate_tab_target_sess = NULL;
            app->translate_tab_pending = 0;
            app->translate_tab_first_time = 0;
            app->translate_tab_last_time = 0;
        }

        /* Filebrowser SFTP detach + modal session pointer cleanup —
         * mirrors app_close_tab so the sidebar and KBI/passphrase/host-key
         * dialogs don't dereference the freed session through their cached
         * pointers after the trash worker runs. */
        if (dead_s) {
            if (app->filebrowser.session == (void *)dead_s) {
                fb_detach_sftp(&app->filebrowser);
            }
            if (app->kbi_session == dead_s) {
                app->kbi_session = NULL;
                app->kbi_dialog_active = false;
            }
            if (app->passphrase_session == dead_s) {
                app->passphrase_session = NULL;
                app->passphrase_dialog_active = false;
                memset(app->passphrase_input, 0, sizeof(app->passphrase_input));
            }
            if (app->hostkey_session == dead_s) {
                app->hostkey_session = NULL;
                app->hostkey_dialog_active = false;
            }
        }

        /* Pane 0 carries the cwd that drives tab->git_status. Killing it
         * means the survivor is a different shell — drop the cached repo
         * numbers and the cached_cwd pointer; the next git_status_tick
         * will repopulate from the new cwd. */
        if (pane == 0) {
            free(tab->git_status.cached_cwd);
            memset(&tab->git_status, 0, sizeof(tab->git_status));
        }

        /* Push the dead pane onto the undo-close stack — Cmd+Shift+T
         * restores it as a new tab. Same shape as app_close_tab so the
         * undo path doesn't need to special-case panes. */
        if (dead_s) {
            ClosedTabInfo pci;
            memset(&pci, 0, sizeof(pci));
            pci.valid = true;
            snprintf(pci.title, sizeof(pci.title), "%s", tab->title);
            pci.type = session_type(dead_s);
            if (pci.type == SESSION_SSH || pci.type == SESSION_MOSH) {
                const SSHConfig *cfg = session_get_config(dead_s);
                if (cfg) {
                    pci.ssh_config = calloc(1, sizeof(*pci.ssh_config));
                    if (pci.ssh_config && !ssh_config_clone(pci.ssh_config, cfg)) {
                        free(pci.ssh_config);
                        pci.ssh_config = NULL;
                    }
                }
            } else if (dead_t && dead_t->cwd && dead_t->cwd[0]) {
                usize n = strlen(dead_t->cwd);
                pci.cwd = malloc(n + 1);
                if (pci.cwd) memcpy(pci.cwd, dead_t->cwd, n + 1);
            }
            closed_tab_push(app, &pci);
        }

        if (!tab_trash_defer(dead_t, dead_s)) {
            if (dead_t) terminal_destroy(dead_t);
            if (dead_s) session_destroy(dead_s);
        }

        tab_split_start_move_anim(tab, old_rects, -1, -1);
        app_resize_tab_panes(app, tab);
        return;
    }
    if (pane != 0 && pane != 1) return;

    Terminal *dead_t = NULL;
    Session  *dead_s = NULL;

    if (pane == 0) {
        /* Killing pane 0 (left/top): pane 1 (right/bottom) is promoted
         * into slot 0 so the slot-0 terminal remains the active pane.
         * Move cache2 → cache1 instead of freeing both, so the survivor's
         * warm row cache carries over and the next frame doesn't have to
         * rebuild every row from scratch. */
        dead_t = tab->terminal;
        dead_s = tab->session;
        tab->terminal = tab->terminal2;
        tab->session  = tab->session2;
        app->broadcast_targets[tab_index][0] = app->broadcast_targets[tab_index][1];
        term_render_cache_free(&tab->cache1);
        tab->cache1 = tab->cache2;
        memset(&tab->cache2, 0, sizeof(tab->cache2));
    } else {
        /* Killing pane 1 — just drop it. */
        dead_t = tab->terminal2;
        dead_s = tab->session2;
        term_render_cache_free(&tab->cache2);
    }
    tab->terminal2 = NULL;
    tab->session2  = NULL;
    app->broadcast_targets[tab_index][1] = false;
    tab->split = SPLIT_NONE;
    tab->split_pane_count = 1;
    tab_split_tree_reset(tab);
    tab->active_pane = 0;

    /* Filebrowser SFTP detach + dialog session cleanup — same rationale
     * as app_close_tab: any cached pointer to the dead session would UAF
     * once the trash worker frees it. */
    if (dead_s) {
        if (app->filebrowser.session == (void *)dead_s) {
            fb_detach_sftp(&app->filebrowser);
        }
        if (app->kbi_session == dead_s) {
            app->kbi_session = NULL;
            app->kbi_dialog_active = false;
        }
        if (app->passphrase_session == dead_s) {
            app->passphrase_session = NULL;
            app->passphrase_dialog_active = false;
            memset(app->passphrase_input, 0, sizeof(app->passphrase_input));
        }
        if (app->hostkey_session == dead_s) {
            app->hostkey_session = NULL;
            app->hostkey_dialog_active = false;
        }
    }

    /* Register the dead pane on the undo-close stack so Cmd+Shift+T
     * restores it (as a new tab — we don't preserve the split layout,
     * but the user gets the session/shell back). Title comes from the
     * containing tab; ssh_config or cwd from the live Session. */
    if (dead_s) {
        ClosedTabInfo pci;
        memset(&pci, 0, sizeof(pci));
        pci.valid = true;
        snprintf(pci.title, sizeof(pci.title), "%s", tab->title);
        pci.type = session_type(dead_s);
        if (pci.type == SESSION_SSH || pci.type == SESSION_MOSH) {
            const SSHConfig *cfg = session_get_config(dead_s);
            if (cfg) {
                pci.ssh_config = calloc(1, sizeof(*pci.ssh_config));
                if (pci.ssh_config && !ssh_config_clone(pci.ssh_config, cfg)) {
                    free(pci.ssh_config);
                    pci.ssh_config = NULL;
                }
            }
        } else if (dead_t && dead_t->cwd && dead_t->cwd[0]) {
            usize n = strlen(dead_t->cwd);
            pci.cwd = malloc(n + 1);
            if (pci.cwd) memcpy(pci.cwd, dead_t->cwd, n + 1);
        }
        closed_tab_push(app, &pci);
    }

    /* Translate-on-Tab cancels itself if its target lived in the dead pane.
     * Without this the streaming write loop would chase a freed terminal
     * the next time PTY bytes arrive. */
    if (app->translate_target_term == dead_t ||
        app->translate_target_sess == dead_s) {
        app->translate_target_term = NULL;
        app->translate_target_sess = NULL;
        app->translate_cancel_requested = true;
    }
    if (app->translate_tab_target_term == dead_t ||
        app->translate_tab_target_sess == dead_s) {
        app->translate_tab_target_term = NULL;
        app->translate_tab_target_sess = NULL;
        app->translate_tab_pending = 0;
        app->translate_tab_first_time = 0;
        app->translate_tab_last_time = 0;
    }

    /* git_status is keyed off pane 0's cwd. Killing pane 0 promotes a
     * different shell — flush the cached numbers + cached_cwd so the
     * status bar doesn't show the dead pane's repo state until the
     * 2 s git_status throttle elapses. */
    if (pane == 0) {
        free(tab->git_status.cached_cwd);
        memset(&tab->git_status, 0, sizeof(tab->git_status));
    }

    if (!tab_trash_defer(dead_t, dead_s)) {
        if (dead_t) terminal_destroy(dead_t);
        if (dead_s) session_destroy(dead_s);
    }

    /* Survivor: stretch back to the full tab grid and force a redraw. */
    app_resize_tab_panes(app, tab);
    tab->cache1.all_rows_dirty = true;
    if (tab->terminal) TERM_DIRTY_ALL(tab->terminal);
}

/* Merge dragged tab (src_idx) into the active tab as a split pane.
 * zone: 1=left, 2=right, 3=top, 4=bottom.
 * The src tab's terminal/session are stolen (not destroyed) and placed
 * into the destination tab's second pane slot. */
void app_split_tab_from_drag(AppState *app, i32 src_idx, i32 zone) {
    if (src_idx < 0 || src_idx >= app->tab_count) return;
    if (zone < 1 || zone > 4) return;

    i32 dst_idx = app->active_tab;
    if (dst_idx == src_idx) return;

    Tab *dst = &app->tabs[dst_idx];
    Tab *src = &app->tabs[src_idx];

    if (dst->split != SPLIT_NONE) return; /* already split */
    if (src->split != SPLIT_NONE) return; /* nested split not supported */

    SplitType split_type = (zone == 1 || zone == 2) ? SPLIT_H : SPLIT_V;

    dst->split       = split_type;
    dst->split_ratio = 0.5f;
    dst->split_pane_count = 2;

    /* zone 1=left, 3=top: src goes into pane1, existing dst terminal to pane2 */
    if (zone == 1 || zone == 3) {
        dst->terminal2 = dst->terminal;
        dst->session2  = dst->session;
        dst->terminal  = src->terminal;
        dst->session   = src->session;
        dst->active_pane = 0;
    } else {
        /* zone 2=right, 4=bottom: src goes into pane2 */
        dst->terminal2 = src->terminal;
        dst->session2  = src->session;
        dst->active_pane = 1;
    }

    /* Nullify src pointers so app_close_tab won't destroy them */
    src->terminal = NULL;
    src->session  = NULL;
    tab_split_init_tree(dst, split_type, false);

    /* Adjust dst_idx before removal in case src comes before dst */
    i32 new_dst = dst_idx;
    if (src_idx < dst_idx) new_dst--;

    /* Remove the source tab entry (terminal/session are NULL, so nothing freed) */
    app_close_tab(app, src_idx);

    app->active_tab = new_dst;

    app_resize_tab_panes(app, &app->tabs[new_dst]);
}

/* Get the focused terminal/session of the active tab */
Terminal *app_focused_terminal(AppState *app) {
    Tab *tab = app_active_tab(app);
    if (!tab) return NULL;
    if (tab->split != SPLIT_NONE) {
        Terminal *t = tab_pane_terminal(tab, tab->active_pane);
        if (t) return t;
    }
    return tab->terminal;
}

Session *app_focused_session(AppState *app) {
    Tab *tab = app_active_tab(app);
    if (!tab) return NULL;
    if (tab->split != SPLIT_NONE) {
        Session *s = tab_pane_session(tab, tab->active_pane);
        if (s) return s;
    }
    return tab->session;
}

const char *app_active_local_cwd(AppState *app) {
    Tab *tab = app ? app_active_tab(app) : NULL;
    if (!tab) return NULL;
    Terminal *src_t = tab_pane_terminal(tab, tab->active_pane);
    Session  *src_s = tab_pane_session(tab, tab->active_pane);
    /* SSH panes report a remote path that probably doesn't exist locally —
     * skip them so the new tab falls back to the default $HOME landing. */
    if (!src_s || session_type(src_s) != SESSION_LOCAL) return NULL;
    if (src_t && src_t->cwd && src_t->cwd[0]) return src_t->cwd;  /* OSC 7 */
    const char *probe = session_local_cwd(src_s);                 /* proc_pidinfo */
    if (probe && probe[0]) return probe;
    return NULL;
}

/* =========================================================================
 * Tab Groups
 * ========================================================================= */

/* Predefined group colors (8 distinct colors) */
static const Color g_group_colors[MAX_TAB_GROUPS] = {
    {0.35f, 0.55f, 0.85f, 1.0f},  /* blue */
    {0.30f, 0.75f, 0.40f, 1.0f},  /* green */
    {0.85f, 0.55f, 0.25f, 1.0f},  /* orange */
    {0.75f, 0.35f, 0.65f, 1.0f},  /* purple */
    {0.85f, 0.35f, 0.35f, 1.0f},  /* red */
    {0.25f, 0.75f, 0.75f, 1.0f},  /* teal */
    {0.85f, 0.75f, 0.25f, 1.0f},  /* yellow */
    {0.55f, 0.55f, 0.75f, 1.0f},  /* slate */
};

i32 app_create_tab_group(AppState *app, const char *name, Color color) {
    /* Reclaim any group that currently holds no tabs. A tabless group is
     * never drawn (the tab-bar render skips groups with no tabs), so it's
     * invisible and unusable — yet it still occupies a slot and inflates
     * the auto-name counter. This bites hardest right after launch: group
     * definitions are persisted (workspaces.json) but tab→group membership
     * is not, so every restored group starts empty and would otherwise push
     * the next group's number up (a fresh launch naming the first group
     * "Group 4"). Pruning here self-heals that and keeps numbering tight. */
    for (i32 i = 0; i < MAX_TAB_GROUPS; i++) {
        if (!app->tab_groups[i].used) continue;
        bool has_tabs = false;
        for (i32 t = 0; t < app->tab_count; t++) {
            if (app->tabs[t].group_index == i) { has_tabs = true; break; }
        }
        if (!has_tabs) {
            app->tab_groups[i].used = false;
            if (app->tab_group_count > 0) app->tab_group_count--;
        }
    }

    if (app->tab_group_count >= MAX_TAB_GROUPS) return -1;

    /* Find first unused slot */
    i32 slot = -1;
    for (i32 i = 0; i < MAX_TAB_GROUPS; i++) {
        if (!app->tab_groups[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;

    TabGroup *g = &app->tab_groups[slot];
    /* Empty/NULL name → default to the slot number, so numbering tracks the
     * actual (reclaimed) slot rather than a stale cumulative count. */
    if (name && name[0])
        snprintf(g->name, sizeof(g->name), "%s", name);
    else
        snprintf(g->name, sizeof(g->name), "Group %d", slot + 1);
    /* Use provided color, or fall back to predefined color for this slot */
    if (color.r == 0 && color.g == 0 && color.b == 0 && color.a == 0) {
        g->color = g_group_colors[slot % MAX_TAB_GROUPS];
    } else {
        g->color = color;
    }
    g->collapsed = false;
    g->used = true;
    app->tab_group_count++;
    app_save_workspaces(app);
    return slot;
}

void app_delete_tab_group(AppState *app, i32 group_index) {
    if (group_index < 0 || group_index >= MAX_TAB_GROUPS) return;
    if (!app->tab_groups[group_index].used) return;

    /* Remove all tabs from this group */
    for (i32 i = 0; i < app->tab_count; i++) {
        if (app->tabs[i].group_index == group_index)
            app->tabs[i].group_index = -1;
    }

    app->tab_groups[group_index].used = false;
    app->tab_group_count--;
    app_save_workspaces(app);
}

void app_rename_tab_group(AppState *app, i32 group_index, const char *name) {
    if (group_index < 0 || group_index >= MAX_TAB_GROUPS) return;
    if (!app->tab_groups[group_index].used) return;
    snprintf(app->tab_groups[group_index].name,
             sizeof(app->tab_groups[group_index].name), "%s", name);
    app_save_workspaces(app);
}

void app_set_tab_group(AppState *app, i32 tab_index, i32 group_index) {
    if (tab_index < 0 || tab_index >= app->tab_count) return;
    if (group_index >= MAX_TAB_GROUPS) return;
    if (group_index >= 0 && !app->tab_groups[group_index].used) return;
    app->tabs[tab_index].group_index = group_index;
}

void app_remove_tab_from_group(AppState *app, i32 tab_index) {
    if (tab_index < 0 || tab_index >= app->tab_count) return;
    app->tabs[tab_index].group_index = -1;
}

void app_toggle_tab_group_collapsed(AppState *app, i32 group_index) {
    if (group_index < 0 || group_index >= MAX_TAB_GROUPS) return;
    if (!app->tab_groups[group_index].used) return;
    bool now_collapsed = !app->tab_groups[group_index].collapsed;
    app->tab_groups[group_index].collapsed = now_collapsed;
    anim_start(&app->tab_groups[group_index].collapse_anim, 0.18);

    /* If active tab is in collapsed group, switch to first visible tab */
    if (now_collapsed &&
        app->active_tab >= 0 && app->active_tab < app->tab_count &&
        app->tabs[app->active_tab].group_index == group_index) {
        for (i32 i = 0; i < app->tab_count; i++) {
            i32 gi = app->tabs[i].group_index;
            if (gi < 0 || !app->tab_groups[gi].collapsed) {
                app_switch_tab(app, i);
                break;
            }
        }
    }

    /* Closing the group puts every member tab to sleep — Chrome-style "tab
     * group hidden" but with our session-sleep mechanism so the PTY/SSH
     * sockets are released. Expand keeps tabs asleep; clicking a tab wakes
     * it lazily via the existing wake-on-activate path. */
    if (now_collapsed) {
        for (i32 i = 0; i < app->tab_count; i++) {
            if (app->tabs[i].group_index == group_index) {
                app_sleep_tab(app, i);
            }
        }
    }
    app_save_workspaces(app);
}

void app_close_tab_group(AppState *app, i32 group_index) {
    if (group_index < 0 || group_index >= MAX_TAB_GROUPS) return;
    if (!app->tab_groups[group_index].used) return;

    /* Close all tabs in this group (iterate backwards to avoid index shifting) */
    for (i32 i = app->tab_count - 1; i >= 0; i--) {
        if (app->tabs[i].group_index == group_index) {
            app_close_tab(app, i);
        }
    }

    app->tab_groups[group_index].used = false;
    app->tab_group_count--;
    app_save_workspaces(app);
}

i32 app_tab_group_count_tabs(AppState *app, i32 group_index) {
    if (group_index < 0 || group_index >= MAX_TAB_GROUPS) return 0;
    if (!app->tab_groups[group_index].used) return 0;
    i32 count = 0;
    for (i32 i = 0; i < app->tab_count; i++) {
        if (app->tabs[i].group_index == group_index) count++;
    }
    return count;
}

void app_recolor_tab_group(AppState *app, i32 group_index, Color color) {
    if (group_index < 0 || group_index >= MAX_TAB_GROUPS) return;
    if (!app->tab_groups[group_index].used) return;
    app->tab_groups[group_index].color = color;
    app_save_workspaces(app);
}

void app_ungroup_tab_group(AppState *app, i32 group_index) {
    if (group_index < 0 || group_index >= MAX_TAB_GROUPS) return;
    if (!app->tab_groups[group_index].used) return;
    /* Reuse delete: it already clears member tabs' group_index then frees the
     * slot. Tabs stay open — that's the "Ungroup" semantics. */
    app_delete_tab_group(app, group_index);
}

i32 app_group_palette_size(void) { return MAX_TAB_GROUPS; }

Color app_group_palette_color(i32 idx) {
    if (idx < 0 || idx >= MAX_TAB_GROUPS) return (Color){1,1,1,1};
    return g_group_colors[idx];
}

i32 app_group_palette_match(Color c) {
    i32 best = 0;
    f32 best_d = 1e9f;
    for (i32 i = 0; i < MAX_TAB_GROUPS; i++) {
        Color p = g_group_colors[i];
        f32 dr = c.r - p.r, dg = c.g - p.g, db = c.b - p.b;
        f32 d = dr*dr + dg*dg + db*db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Workspace persistence — groups only.
 *
 * Format (~/.config/Liu/workspaces.json):
 *   { "groups": [
 *       { "slot": 0, "name": "Personal", "color": [r,g,b,a], "collapsed": false }
 *     ] }
 *
 * Tab→group mapping is intentionally *not* persisted: PTY/SSH sessions die
 * with the process, so tabs always start fresh. Groups themselves persist so
 * the user's named/coloured workspaces survive a relaunch and tabs can be
 * drag-dropped back in. */
static void workspaces_path(char *out, usize cap) {
    snprintf(out, cap, "%s/workspaces.json", config_user_dir());
}

void app_save_workspaces(const AppState *app) {
    if (!app) return;
    char path[1024];
    workspaces_path(path, sizeof(path));

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "groups");
    for (i32 i = 0; i < MAX_TAB_GROUPS; i++) {
        const TabGroup *g = &app->tab_groups[i];
        if (!g->used) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "slot", i);
        cJSON_AddStringToObject(obj, "name", g->name);
        cJSON *col = cJSON_AddArrayToObject(obj, "color");
        cJSON_AddItemToArray(col, cJSON_CreateNumber(g->color.r));
        cJSON_AddItemToArray(col, cJSON_CreateNumber(g->color.g));
        cJSON_AddItemToArray(col, cJSON_CreateNumber(g->color.b));
        cJSON_AddItemToArray(col, cJSON_CreateNumber(g->color.a));
        cJSON_AddBoolToObject(obj, "collapsed", g->collapsed);
        cJSON_AddItemToArray(arr, obj);
    }
    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) return;

    /* Ensure dir exists, then write atomically via *.tmp + rename. */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0755); }

    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(txt, f);
        fclose(f);
        rename(tmp, path);
    }
    free(txt);
}

void app_load_workspaces(AppState *app) {
    if (!app) return;
    char path[1024];
    workspaces_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1<<20)) { fclose(f); return; }
    char *buf = (char *)malloc((usize)sz + 1);
    if (!buf) { fclose(f); return; }
    usize n = fread(buf, 1, (usize)sz, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    /* Reset groups before loading. Tabs keep their default group_index = -1
     * since this is called at startup before any tab restoration. */
    for (i32 i = 0; i < MAX_TAB_GROUPS; i++) {
        app->tab_groups[i].used = false;
    }
    app->tab_group_count = 0;

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "groups");
    if (cJSON_IsArray(arr)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            cJSON *jslot  = cJSON_GetObjectItemCaseSensitive(it, "slot");
            cJSON *jname  = cJSON_GetObjectItemCaseSensitive(it, "name");
            cJSON *jcol   = cJSON_GetObjectItemCaseSensitive(it, "color");
            cJSON *jclpsd = cJSON_GetObjectItemCaseSensitive(it, "collapsed");
            i32 slot = cJSON_IsNumber(jslot) ? (i32)jslot->valuedouble : -1;
            if (slot < 0 || slot >= MAX_TAB_GROUPS) continue;
            TabGroup *g = &app->tab_groups[slot];
            if (cJSON_IsString(jname) && jname->valuestring) {
                snprintf(g->name, sizeof(g->name), "%s", jname->valuestring);
            } else {
                snprintf(g->name, sizeof(g->name), "Group %d", slot + 1);
            }
            if (cJSON_IsArray(jcol) && cJSON_GetArraySize(jcol) >= 3) {
                cJSON *cr = cJSON_GetArrayItem(jcol, 0);
                cJSON *cg = cJSON_GetArrayItem(jcol, 1);
                cJSON *cb = cJSON_GetArrayItem(jcol, 2);
                cJSON *ca = cJSON_GetArrayItem(jcol, 3);
                g->color.r = cJSON_IsNumber(cr) ? (f32)cr->valuedouble : 1.0f;
                g->color.g = cJSON_IsNumber(cg) ? (f32)cg->valuedouble : 1.0f;
                g->color.b = cJSON_IsNumber(cb) ? (f32)cb->valuedouble : 1.0f;
                g->color.a = (cJSON_IsNumber(ca) ? (f32)ca->valuedouble : 1.0f);
            } else {
                g->color = g_group_colors[slot % MAX_TAB_GROUPS];
            }
            g->collapsed = cJSON_IsBool(jclpsd) && cJSON_IsTrue(jclpsd);
            g->used = true;
            app->tab_group_count++;
        }
    }
    cJSON_Delete(root);
}

/* =========================================================================
 * Layout
 * ========================================================================= */

void app_update_layout(AppState *app, i32 win_w, i32 win_h, i32 fb_w, i32 fb_h) {
    app->win_width  = win_w;
    app->win_height = win_h;
    app->fb_width   = fb_w;
    app->fb_height  = fb_h;

    /* Toolbar height: if either tab bar or icons are visible, use full size.
     * If both hidden, keep a small strip on macOS for traffic lights / drag,
     * zero on other platforms. */
    bool any_top_ui = app->config.show_tab_bar || app->config.show_toolbar_icons;
    if (any_top_ui) {
        app->tab_bar_height = TOOLBAR_HEIGHT_PT * app->dpi_scale;
    } else {
#ifdef PLATFORM_MACOS
        app->tab_bar_height = 22.0f * app->dpi_scale;
#else
        app->tab_bar_height = 0.0f;
#endif
    }
#ifdef PLATFORM_MACOS
    /* Keep the macOS traffic lights centered on the strip we actually paint:
     * the full toolbar normally, or the ~30pt drag strip (see the FVIEW strip
     * clamp) when both the tab bar and toolbar icons are hidden. In points. */
    platform_set_titlebar_height(any_top_ui ? TOOLBAR_HEIGHT_PT
                                            : fmaxf(app->tab_bar_height / app->dpi_scale, 30.0f));
#endif

    /* Status bar height (0 when hidden) */
    app->status_bar_height = app->config.show_status_bar
        ? app->config.style.status_bar_height * app->dpi_scale
        : 0.0f;

    /* Terminal padding — at least style.terminal_padding */
    f32 cfg_pad = app->config.padding > app->config.style.terminal_padding
                  ? app->config.padding : app->config.style.terminal_padding;
    app->padding = cfg_pad * app->dpi_scale;

    f32 cw = app->renderer.font.cell_width;
    f32 ch = app->renderer.font.cell_height;
    if (cw < 1 || ch < 1) return;

    f32 top_gap = app->config.style.terminal_top_gap * app->dpi_scale;
    f32 content_x = app->sidebar_width + app->padding;
    f32 content_y = app->tab_bar_height + top_gap;
    f32 content_w = fb_w - content_x - app->padding;
    f32 content_h = fb_h - content_y - app->status_bar_height - app->padding;

    i32 new_cols = (i32)(content_w / cw);
    i32 new_rows = (i32)(content_h / ch);
    if (new_cols < 1) new_cols = 1;
    if (new_rows < 1) new_rows = 1;

    /* idempotency guard — skip the expensive per-tab resize pass
     * (terminal_resize copies every cell; session_resize ioctl's PTY) when
     * grid dimensions haven't changed. Callers fire this from many paths
     * (sidebar toggle, tab switch, split, window resize), most of which
     * leave grid_cols/rows unchanged. */
    if (new_cols == app->grid_cols && new_rows == app->grid_rows) {
        return;
    }
    app->grid_cols = new_cols;
    app->grid_rows = new_rows;

    for (i32 i = 0; i < app->tab_count; i++) {
        app_resize_tab_panes(app, &app->tabs[i]);
    }
}

void app_resize_tab_panes(AppState *app, Tab *t) {
    if (!app || !t || t->sleeping) return;
    if (app->grid_cols <= 0 || app->grid_rows <= 0) return;

    if (t->split == SPLIT_NONE) {
        if (t->terminal) terminal_resize(t->terminal, app->grid_cols, app->grid_rows);
        if (t->session)  session_resize(t->session,  app->grid_cols, app->grid_rows);
        return;
    }

    if (t->split_root >= 0) {
        f32 ox = 0, oy = 0, total_w = 0, total_h = 0;
        app_terminal_content_rect(app, &ox, &oy, &total_w, &total_h);
        PaneRect rects[MAX_SPLIT_PANES];
        tab_split_layout_rects(t, ox, oy, total_w, total_h, app->dpi_scale, rects);
        f32 cw = app->renderer.font.cell_width > 1.0f ? app->renderer.font.cell_width : 8.0f;
        f32 ch = app->renderer.font.cell_height > 1.0f ? app->renderer.font.cell_height : 16.0f;
        i32 count = tab_split_pane_count(t);
        for (i32 p = 0; p < count; p++) {
            Terminal *pt = tab_pane_terminal(t, p);
            Session *ps = tab_pane_session(t, p);
            if (!pt && !ps) continue;
            i32 cols = (i32)(rects[p].w / cw);
            i32 rows = (i32)(rects[p].h / ch);
            if (cols < 10) cols = 10;
            if (rows < 5) rows = 5;
            if (pt) {
                terminal_resize(pt, cols, rows);
                TERM_DIRTY_ALL(pt);
            }
            if (ps) session_resize(ps, cols, rows);
            TermRenderCache *cache = tab_pane_cache(t, p);
            if (cache) term_render_cache_free(cache);
        }
        return;
    }

    f32 ratio = (t->split_ratio > 0.05f && t->split_ratio < 0.95f)
                ? t->split_ratio : 0.5f;
    i32 c1 = app->grid_cols, r1 = app->grid_rows;
    i32 c2 = app->grid_cols, r2 = app->grid_rows;
    if (t->split == SPLIT_H) {
        c1 = (i32)((f32)app->grid_cols * ratio);
        c2 = app->grid_cols - c1;
    } else {
        r1 = (i32)((f32)app->grid_rows * ratio);
        r2 = app->grid_rows - r1;
    }
    if (c1 < 10) c1 = 10;
    if (r1 < 5)  r1 = 5;
    if (c2 < 10) c2 = 10;
    if (r2 < 5)  r2 = 5;

    if (t->terminal)  terminal_resize(t->terminal,  c1, r1);
    if (t->session)   session_resize(t->session,   c1, r1);
    if (t->terminal2) terminal_resize(t->terminal2, c2, r2);
    if (t->session2)  session_resize(t->session2,  c2, r2);

    term_render_cache_free(&t->cache1);
    term_render_cache_free(&t->cache2);
    if (t->terminal)  TERM_DIRTY_ALL(t->terminal);
    if (t->terminal2) TERM_DIRTY_ALL(t->terminal2);
}

/* =========================================================================
 * Rendering helpers
 * ========================================================================= */

/* step=0 means use font cell_width; pass explicit step for UI-scaled text.
 * Handles UTF-8 multi-byte characters properly. */
void draw_text_ex(Renderer *r, const char *text, f32 x, f32 y, Color fg, i32 max_chars, f32 step) {
    if (step <= 0) step = r->font.cell_width;
    const u8 *p = (const u8 *)text;
    i32 drawn = 0;
    while (*p && drawn < max_chars) {
        u32 cp;
        u32 consumed = utf8_decode(p, 4, &cp);
        if (consumed == 0) { p++; continue; } /* skip invalid bytes */

        if (cp >= 32) {
            renderer_draw_glyph(r, cp, x, y, fg, (Color){0,0,0,0});
            x += step;
            drawn++;
        }
        p += consumed;
    }
}

static void draw_text(Renderer *r, const char *text, f32 x, f32 y, Color fg, i32 max_chars) {
    draw_text_ex(r, text, x, y, fg, max_chars, 0);
}

/* Toolbar button → unified icon kind. The new icon_draw helper handles
 * SF Symbol resolution, vector fallback and pixel snapping uniformly so
 * every toolbar button shares the same visual language. The previous
 * patchwork (SF Symbol → ASCII text → hand-drawn rect grid → custom
 * font_scale_icon) is gone — one path now serves all buttons. */
static IconKind toolbar_button_icon_kind(ToolbarButton btn) {
    switch (btn) {
    case TB_SIDEBAR:   return ICON_SIDEBAR;
    case TB_SSH:       return ICON_NETWORK;
    case TB_SETTINGS:  return ICON_GEAR;
    case TB_FONT_DOWN: return ICON_FONT_SMALLER;
    case TB_FONT_UP:   return ICON_FONT_LARGER;
    default:           return ICON_NONE;
    }
}

/* Hover feedback for toolbar buttons. Tracks hover-anim per button key
 * (a unique integer per call site) so re-entry replays the fade-in. Hit
 * test uses app->hover_x/hover_y; the bg is a soft rrect that fades in. */
static void draw_toolbar_button_bg(AppState *app, f32 x, f32 y, f32 w, f32 h,
                                   i32 hover_key, f32 dpi) {
    Renderer *r = &app->renderer;
    bool hover = (app->hover_x >= x && app->hover_x < x + w &&
                  app->hover_y >= y && app->hover_y < y + h);
    static i32  s_btn_hover_key = -1;
    static Anim s_btn_hover_anim = {0};
    if (hover) {
        if (hover_key != s_btn_hover_key) {
            s_btn_hover_key = hover_key;
            anim_start(&s_btn_hover_anim, 0.16);
        }
        f32 t = anim_eased(&s_btn_hover_anim, EASE_OUT_CUBIC);
        f32 alpha = 0.12f * t;
        const Theme *th = app->config.theme;
        Color base = th ? th->fg : (Color){1, 1, 1, 1};
        Color bg = { base.r, base.g, base.b, alpha };
        renderer_draw_rect(r,
            x + 2.0f * dpi, y + 2.0f * dpi,
            w - 4.0f * dpi, h - 4.0f * dpi,
            bg);
    } else if (hover_key == s_btn_hover_key) {
        s_btn_hover_key = -1;
    }
}

/* Generic toolbar-glyph draw (ssh/settings/font buttons). The sidebar
 * toggle now renders its glyph directly at a larger size, so this is
 * currently only kept for the other toolbar buttons' future re-enable. */
__attribute__((unused))
static void draw_toolbar_button_icon(Renderer *r, ToolbarButton btn,
                                     f32 x, f32 y, f32 w, f32 icon_sz,
                                     Color fg, f32 cw, f32 ch, f32 dpi) {
    (void)cw; (void)ch; (void)dpi;
    IconKind k = toolbar_button_icon_kind(btn);
    if (k == ICON_NONE) return;
    /* Slightly inset the SF Symbol against the click target — buttons
     * read better when the icon doesn't kiss the hover rect's edge. */
    f32 inset = icon_sz * 0.12f;
    f32 sz    = icon_sz - 2.0f * inset;
    icon_draw(r, k,
              x + (w - sz) * 0.5f,
              y + (icon_sz - sz) * 0.5f,
              sz, fg);
}

/* Pixel-aligned wrappers around the GPU SDF rounded-rect primitive.
 * Metal renders true antialiased curves; OpenGL falls back to flat rects.
 * Replaces the legacy 3-step CPU staircase that visibly aliased on Retina. */
__attribute__((unused)) static void draw_rounded_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h, Color c, f32 radius) {
    if (radius < 0.5f) { renderer_draw_rect(r, x, y, w, h, c); return; }
    renderer_draw_rrect_simple(r, x, y, w, h, c, radius);
}

/* Rectangle with only the top two corners rounded (tab style). */
__attribute__((unused)) static void draw_rounded_top_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h, Color c, f32 radius) {
    if (radius < 0.5f || w < 2.0f * radius || h < radius) {
        renderer_draw_rect(r, x, y, w, h, c);
        return;
    }
    renderer_draw_rrect_top(r, x, y, w, h, c, radius);
}

/* Pill (rounded on all four corners). */
__attribute__((unused)) static void draw_pill_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h, Color c, f32 radius) {
    if (radius < 0.5f || w < 2.0f * radius || h < 2.0f * radius) {
        renderer_draw_rect(r, x, y, w, h, c);
        return;
    }
    renderer_draw_rrect_simple(r, x, y, w, h, c, radius);
}

/* Small round-ish dot — render as a circle via half-min-dim radius. */
static void draw_soft_dot(Renderer *r, f32 x, f32 y, f32 sz, Color c) {
    if (sz < 4.0f) { renderer_draw_rect(r, x, y, sz, sz, c); return; }
    renderer_draw_rrect_simple(r, x, y, sz, sz, c, sz * 0.5f);
}

/* =========================================================================
 * Shared modal/overlay chrome — the rounded design language primitives used
 * across dialogs, menus, overlays and inputs. These collapse the recurring
 * legacy anti-patterns (fake double-rect drop shadow, flat 4-edge borders,
 * "flat box + 2px bottom underline" inputs) into one consistent treatment.
 * All queue into the rrect batch; callers flush rrects (then glyphs).
 * ========================================================================= */

/* Floating-panel chrome: a 1px rounded border ring carrying a soft SDF drop
 * shadow, with the rounded body painted on top. Pass shadow_size = 0 for a
 * flush (non-floating) panel. Replaces the "two offset black rects + flat
 * border rect + flat body rect" pattern. */
static void draw_panel_chrome(Renderer *r, f32 x, f32 y, f32 w, f32 h,
                              Color body, Color border, f32 radius, f32 dpi,
                              f32 shadow_size, f32 shadow_alpha) {
    renderer_draw_rrect(r, x - 1.0f * dpi, y - 1.0f * dpi,
                        w + 2.0f * dpi, h + 2.0f * dpi, border,
                        radius + 1.0f, radius + 1.0f, radius + 1.0f, radius + 1.0f,
                        shadow_size, shadow_alpha,
                        0.0f, shadow_size > 0.0f ? 5.0f * dpi : 0.0f);
    renderer_draw_rrect_simple(r, x, y, w, h, body, radius);
}

/* Recessed rounded input field. Replaces the legacy "flat box + 2px bottom
 * accent underline" pattern with a filled rounded box + border ring. Fill and
 * border are passed in so callers control theming and modal-fade alpha — pass
 * the UI accent (theme_ui_accent) as `border` when the field is focused, the
 * theme border when idle. Commits immediately (flush_rrects) so the caller's
 * text + blinking caret, drawn afterwards, land on top of the field. */
static void draw_input_field(Renderer *r, f32 x, f32 y, f32 w, f32 h,
                             Color fill, Color border, f32 dpi) {
    f32 rad = 8.0f * dpi;
    renderer_draw_rrect_bordered(r, x, y, w, h, fill, border, 1.5f * dpi,
                                 rad, rad, rad, rad, 0.0f, 0.0f, 0.0f, 0.0f);
    renderer_flush_rrects(r);
}

/* Footer keycap chip (e.g. Enter / Esc hints). Draws a small rounded, bordered
 * key cap with the label, and returns the x advance just past the cap so the
 * caller can place trailing text. Queues the cap into the rrect batch and the
 * label into the glyph batch — caller must flush rrects before glyphs (glyphs
 * always flush last, so the label lands above the cap). */
static f32 draw_keycap(Renderer *r, f32 x, f32 y, const char *label,
                       const Theme *t, f32 ui_cw, f32 ui_ch, f32 dpi) {
    i32 n = (i32)strlen(label);
    f32 pad  = 7.0f * dpi;
    f32 capw = (f32)n * ui_cw + pad * 2.0f;
    f32 caph = ui_ch + 7.0f * dpi;
    Color bg = t->tab_active_bg;
    Color bd = t->border; bd.a = fmaxf(bd.a, 0.60f);
    renderer_draw_rrect_bordered(r, x, y - 3.5f * dpi, capw, caph, bg, bd,
                                 1.0f * dpi, 5.0f * dpi, 5.0f * dpi,
                                 5.0f * dpi, 5.0f * dpi, 0.0f, 0.0f, 0.0f, 0.0f);
    Color fg = t->fg; fg.a *= 0.92f;
    draw_text_ex(r, label, x + pad, y, fg, n, ui_cw);
    return x + capw;
}

static void draw_square_mark(Renderer *r, f32 x, f32 y, f32 sz, Color c) {
    renderer_draw_rect(r, x, y, sz, sz, c);
}

/* Output-volume gate for the agent accent animation, in units of Tab.out_accum
 * (a ~0.5 s exponentially-weighted byte count; steady rate R B/s ⇒ accum≈R*0.5).
 * An idle agent's only output is a small periodic cursor blink (well under this
 * level), whereas a working agent streams tokens / repaints a spinner+counter
 * (far above it). Tuned to clear an idle blink with margin while catching the
 * thinking and streaming phases. */
#define TAB_ACCENT_WORK_THRESHOLD 300.0

/* Keep-alive hold for the accent bar, in seconds. Agent output is bursty —
 * token-stream gaps, "thinking" pauses, and tool calls all let out_accum decay
 * below the threshold within ~0.5 s, which made the bar flicker on/off like a
 * blinking lamp. Once a real burst crosses the threshold we hold "working" for
 * this long past the last burst, so short gaps stay lit and only a genuinely
 * idle agent (no qualifying output for the whole window) lets the bar fade. */
#define TAB_ACCENT_WORK_HOLD 2.5

/* Tab top-accent bar. A plain tab draws a static line; when an AI agent CLI is
 * actively working in the tab the accent becomes a living indicator: a slow
 * "breathing" alpha pulse over the whole line, plus a brighter crest that
 * sweeps left→right and loops. While animating we push the global redraw
 * deadline forward so the event loop keeps waking even when the user idles. */
static void draw_tab_accent(Renderer *r, f32 x, f32 y, f32 w, f32 h,
                            Color base, bool agent_running, f32 alpha) {
    if (!agent_running) {
        Color c = base; c.a *= alpha;
        renderer_draw_rect(r, x, y, w, h, c);
        return;
    }
    f64 now = platform_time_sec();
    /* fmod before the f32 cast keeps the sin/phase args small + precise even
     * if platform_time_sec() is epoch-based. */
    f32 breath = 0.5f + 0.5f * sinf((f32)(fmod(now, 2.4) / 2.4 * 6.2831853));
    f32 base_a = (0.34f + 0.46f * breath) * alpha;       /* breathing floor */

    /* Base line first so any sub-pixel gaps between crest slices reveal the
     * line underneath rather than dark seams. */
    Color cb = base; cb.a = base_a;
    renderer_draw_rect(r, x, y, w, h, cb);

    /* Crest sweeping left→right (~1.5 s per pass). */
    f32 phase = (f32)(fmod(now, 1.5) / 1.5);
    const i32 N = 36;
    f32 cell = w / (f32)N;
    f32 sw   = cell + 1.0f;                              /* overlap → no gaps */
    for (i32 s = 0; s < N; s++) {
        f32 f = (s + 0.5f) / (f32)N;
        f32 d = f - phase; d -= floorf(d);               /* 0 at crest */
        if (d > 0.5f) d = 1.0f - d;                      /* symmetric bump */
        f32 crest = 1.0f - d * 2.0f;                     /* 1 at crest → 0 */
        if (crest <= 0.02f) continue;
        crest = crest * crest * crest;                   /* tight, comet head */
        Color c = {
            fminf(1.0f, base.r + 0.30f * crest),
            fminf(1.0f, base.g + 0.30f * crest),
            fminf(1.0f, base.b + 0.22f * crest),
            fminf(1.0f, 0.30f + 0.85f * crest) * alpha,
        };
        renderer_draw_rect(r, x + (f32)s * cell, y, sw, h, c);
    }
    /* Keep the loop awake for the breathing/sweep, but on the soft (~30 Hz)
     * cadence — a streaming agent must not pin the loop at full panel rate. */
    anim_register_soft_until(now + 0.1);
}

static bool agent_name_eq(const char *name, const char *want) {
    if (!name || !want) return false;
    while (*name && *want) {
        if (tolower((unsigned char)*name) != tolower((unsigned char)*want)) return false;
        name++;
        want++;
    }
    return *name == '\0' && *want == '\0';
}

static ChatTool chat_tool_for_process_basename(const char *name) {
    if (!name || !name[0]) return CHAT_TOOL_UNKNOWN;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;

    if (agent_name_eq(base, "claude"))       return CHAT_TOOL_CLAUDE;
    if (agent_name_eq(base, "codex"))        return CHAT_TOOL_CODEX;
    if (agent_name_eq(base, "gpt") ||
        agent_name_eq(base, "chatgpt") ||
        agent_name_eq(base, "openai"))       return CHAT_TOOL_CODEX;
    if (agent_name_eq(base, "copilot"))      return CHAT_TOOL_COPILOT;
    if (agent_name_eq(base, "cursor-agent") ||
        agent_name_eq(base, "cursor"))       return CHAT_TOOL_CURSOR;
    if (agent_name_eq(base, "amp"))          return CHAT_TOOL_AMP;
    if (agent_name_eq(base, "cline"))        return CHAT_TOOL_CLINE;
    if (agent_name_eq(base, "roo"))          return CHAT_TOOL_ROO;
    if (agent_name_eq(base, "kilo"))         return CHAT_TOOL_KILO;
    if (agent_name_eq(base, "kiro"))         return CHAT_TOOL_KIRO;
    if (agent_name_eq(base, "crush"))        return CHAT_TOOL_CRUSH;
    if (agent_name_eq(base, "opencode"))     return CHAT_TOOL_OPENCODE;
    if (agent_name_eq(base, "droid"))        return CHAT_TOOL_DROID;
    if (agent_name_eq(base, "antigravity"))  return CHAT_TOOL_ANTIGRAVITY;
    if (agent_name_eq(base, "kimi"))         return CHAT_TOOL_KIMI;
    if (agent_name_eq(base, "qwen"))         return CHAT_TOOL_QWEN;
    if (agent_name_eq(base, "aider"))        return CHAT_TOOL_AIDER;
    if (agent_name_eq(base, "amazon-q") ||
        agent_name_eq(base, "amazonq"))      return CHAT_TOOL_AMAZON_Q;
    if (agent_name_eq(base, "continue"))     return CHAT_TOOL_CONTINUE;
    if (agent_name_eq(base, "windsurf"))     return CHAT_TOOL_WINDSURF;
    if (agent_name_eq(base, "zed"))          return CHAT_TOOL_ZED;
    if (agent_name_eq(base, "commandcode") ||
        agent_name_eq(base, "command-code") ||
        agent_name_eq(base, "cmd"))          return CHAT_TOOL_COMMANDCODE;
    if (agent_name_eq(base, "grok") ||
        agent_name_eq(base, "xai"))          return CHAT_TOOL_XAI;
    return CHAT_TOOL_UNKNOWN;
}

/* Human-facing tab title for a detected agent CLI. Distinct from
 * chat_tool_name() (which yields lowercase machine ids); these are the
 * capitalized product names shown on the tab, matching the leading icon. */
static const char *chat_tool_display(ChatTool t) {
    switch (t) {
    case CHAT_TOOL_CLAUDE:      return "Claude Code";
    case CHAT_TOOL_CODEX:       return "Codex";
    case CHAT_TOOL_COPILOT:     return "Copilot";
    case CHAT_TOOL_CURSOR:      return "Cursor";
    case CHAT_TOOL_AMP:         return "Amp";
    case CHAT_TOOL_CLINE:       return "Cline";
    case CHAT_TOOL_ROO:         return "Roo Code";
    case CHAT_TOOL_KILO:        return "Kilo Code";
    case CHAT_TOOL_KIRO:        return "Kiro";
    case CHAT_TOOL_CRUSH:       return "Crush";
    case CHAT_TOOL_OPENCODE:    return "OpenCode";
    case CHAT_TOOL_DROID:       return "Droid";
    case CHAT_TOOL_ANTIGRAVITY: return "Antigravity";
    case CHAT_TOOL_KIMI:        return "Kimi";
    case CHAT_TOOL_QWEN:        return "Qwen Code";
    case CHAT_TOOL_AIDER:       return "Aider";
    case CHAT_TOOL_AMAZON_Q:    return "Amazon Q";
    case CHAT_TOOL_CONTINUE:    return "Continue";
    case CHAT_TOOL_WINDSURF:    return "Windsurf";
    case CHAT_TOOL_ZED:         return "Zed";
    case CHAT_TOOL_COMMANDCODE: return "CommandCode";
    case CHAT_TOOL_XAI:         return "Grok";
    default:                    return "Terminal";
    }
}

static Session *tab_agent_probe_session(const Tab *tab) {
    if (!tab || tab->kind != TAB_TERMINAL) return NULL;
    if (tab->active_pane == 1 && tab->session2) return tab->session2;
    if (tab->session) return tab->session;
    return tab->session2;
}

static ChatTool tab_agent_tool(const Tab *tab) {
    enum { CACHE_N = 64 };
    typedef struct {
        const Session *session;
        i32 pid;            /* child pid — disambiguates a recycled Session address */
        ChatTool tool;
        f64 checked_at;
    } AgentToolCache;
    static AgentToolCache cache[CACHE_N];
    static i32 next_slot = 0;

    Session *s = tab_agent_probe_session(tab);
    if (!s || session_type(s) != SESSION_LOCAL ||
        session_status(s) != SESSION_CONNECTED) {
        return CHAT_TOOL_UNKNOWN;
    }

    /* This memo is keyed by the raw Session*, but closed sessions are freed and
     * the heap block is handed back to the next session_create_local(), so a
     * fresh tab can land on a dead tab's address and match its stale slot
     * (wrong agent icon for up to 0.75s). The child pid differs across that
     * reuse, so require it to match too — no teardown invalidation hook needed. */
    i32 spid = session_child_pid(s);

    f64 now = platform_time_sec();
    for (i32 i = 0; i < CACHE_N; i++) {
        if (cache[i].session == s && cache[i].pid == spid) {
            if (now - cache[i].checked_at < 0.75) return cache[i].tool;
            const char *proc = session_fg_process(s);
            cache[i].tool = chat_tool_for_process_basename(proc);
            cache[i].checked_at = now;
            return cache[i].tool;
        }
    }

    const char *proc = session_fg_process(s);
    ChatTool tool = chat_tool_for_process_basename(proc);
    cache[next_slot] = (AgentToolCache){ s, spid, tool, now };
    next_slot = (next_slot + 1) % CACHE_N;
    return tool;
}

/* Foreground AI-agent probe for the close confirm. Unlike tab_agent_tool()
 * (active-pane only, memoized for the per-frame title pass), this checks one
 * specific pane fresh — only session_fg_process()'s own 0.6 s TTL applies,
 * which is fine for a close-time prompt. Returns the ChatTool as i32 so the
 * header doesn't need the history enum; 0 == CHAT_TOOL_UNKNOWN == none. */
i32 app_pane_running_agent(AppState *app, i32 tab_index, i32 pane, const char **display) {
    if (display) *display = NULL;
    if (!app || tab_index < 0 || tab_index >= app->tab_count) return 0;
    const Tab *tab = &app->tabs[tab_index];
    if (tab->kind != TAB_TERMINAL) return 0;
    Session *s = tab_pane_session(tab, pane);
    if (!s || session_type(s) != SESSION_LOCAL ||
        session_status(s) != SESSION_CONNECTED) {
        return 0;
    }
    /* A close is a one-shot user action — scan live rather than trust the
     * ~0.6 s fg cache, which could miss a just-launched agent. */
    session_invalidate_fg_cache(s);
    ChatTool tool = chat_tool_for_process_basename(session_fg_process(s));
    if (tool != CHAT_TOOL_UNKNOWN && display) *display = chat_tool_display(tool);
    return (i32)tool;
}

i32 app_tab_running_agent(AppState *app, i32 tab_index, const char **display) {
    if (display) *display = NULL;
    if (!app || tab_index < 0 || tab_index >= app->tab_count) return 0;
    const Tab *tab = &app->tabs[tab_index];
    if (tab->kind != TAB_TERMINAL) return 0;
    i32 panes = tab_split_pane_count(tab);
    for (i32 p = 0; p < panes; p++) {
        i32 tool = app_pane_running_agent(app, tab_index, p, display);
        if (tool) return tool;
    }
    return 0;
}

f32 app_get_group_chip_width(const AppState *app, i32 gi) {
    if (gi < 0 || gi >= MAX_TAB_GROUPS || !app->tab_groups[gi].used) return 0.0f;
    f32 dpi = app->dpi_scale;
    f32 cw = 8.0f * dpi; /* ui_cw */

    char glabel[80];
    bool is_renaming = (app->chip_rename_active && app->chip_rename_group_index == gi);
    if (is_renaming) {
        snprintf(glabel, sizeof(glabel), "%s", app->chip_rename_buf);
    } else if (app->tab_groups[gi].collapsed) {
        /* Count tabs in group */
        i32 grp_tab_count = 0;
        for (i32 i = 0; i < app->tab_count; i++) {
            if (app->tabs[i].group_index == gi) grp_tab_count++;
        }
        snprintf(glabel, sizeof(glabel), "%s %d", app->tab_groups[gi].name, grp_tab_count);
    } else {
        snprintf(glabel, sizeof(glabel), "%s", app->tab_groups[gi].name);
    }

    size_t len = strlen(glabel);
    if (len > 24) len = 24; 

    /* Mirror render_toolbar's chip geometry EXACTLY (ui.c chip_h/g_dot_x/
     * g_dot_sz/glbl_x/lbl_max) so its (i32) lbl_max clip never eats the last
     * column. The old 25*dpi chip_h here vs the render's 30*dpi (tab_h-6, and
     * tab_h == tb_h == TOOLBAR_HEIGHT_PT*dpi) under-budgeted the chrome by
     * ~1.2*dpi, so lbl_max came out as len-1 — the live rename label and caret
     * lagged one keystroke behind the buffer. */
    f32 chip_h = TOOLBAR_HEIGHT_PT * dpi - 6.0f * dpi;
    if (chip_h < 6.0f * dpi) chip_h = 6.0f * dpi;
    f32 g_dot_sz = chip_h * 0.38f;

    /* Left chrome before the label = render's (glbl_x - cur_x):
     *   g_dot_x offset (chip_h*0.46) + dot + 8dpi gap.
     * Right pad 8dpi matches render's lbl_max -8*dpi term; the +1dpi slack
     * defeats float truncation in render's (i32) lbl_max cast. Both 8*dpi
     * terms MUST stay in sync with render_toolbar's glbl_x / lbl_max. */
    f32 left_chrome = chip_h * 0.46f + g_dot_sz + 8.0f * dpi;
    f32 w = left_chrome + (f32)len * cw + 8.0f * dpi + 1.0f * dpi;
    if (w < 60.0f * dpi) w = 60.0f * dpi;
    return w;
}

static i32 broadcast_selected_count(const AppState *app) {
    i32 count = 0;
    for (i32 i = 0; i < app->tab_count; i++) {
        if (app->tabs[i].session && app->broadcast_targets[i][0]) count++;
        if (app->tabs[i].session2 && app->broadcast_targets[i][1]) count++;
    }
    return count;
}

/* =========================================================================
 * Render: Toolbar (transparent title bar + tabs + buttons)
 * ========================================================================= */

static void render_toolbar(AppState *app) {
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme;
    f32 dpi = app->dpi_scale;
    bool show_tabs  = app->config.show_tab_bar;
    bool show_icons = app->config.show_toolbar_icons;

    /* Tab hover animation state.
     * s_hover_tab tracks which tab the cursor most recently entered. When it
     * changes, anim_start replays the fade-in for the new tab. RENDER_TAB_BODY
     * reads s_hover_anim's eased progress to ramp the hover-fill alpha.
     * Re-entry is handled below the loops by resetting s_hover_tab when the
     * cursor leaves all tabs. */
    static i32  s_hover_tab = -1;
    static Anim s_hover_anim = {0};
    bool any_tab_hovered_this_frame = false;
    if (!show_tabs && !show_icons) {
        /* Both hidden: draw a strip over the macOS title bar area so the
         * traffic-light region matches the terminal body's opacity (uniform
         * look). Must reach the full macOS title bar height (28pt on modern
         * macOS) plus a small overlap — anything shorter leaves a padding
         * gap above the body where alpha differs. Use max(tab_bar_height,
         * 30pt) and the configured window opacity. */
        f32 strip_h = app->tab_bar_height;
        f32 min_strip = 30.0f * dpi;
        if (strip_h < min_strip) strip_h = min_strip;
        f32 op = app->config.opacity;
        Color strip = { t->bg.r, t->bg.g, t->bg.b, op };
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, strip_h, strip);
        renderer_flush_rects(r);
        return;
    }
    f32 tb_h = TOOLBAR_HEIGHT_PT * dpi;
    f32 icon_sz = TB_ICON_SIZE_PT * dpi;

    /* UI chrome renders at fixed 13pt regardless of terminal font size */
    f32 ui_cw = 8.0f * dpi;  /* ~13pt monospace cell width */
    f32 ui_ch = 16.0f * dpi; /* ~13pt monospace cell height */
    renderer_set_ui_scale(r, ui_cw, ui_ch);
    f32 cw = ui_cw;
    f32 ch = ui_ch;

    /* Background — exactly the terminal background colour (no lightening) so the
     * tab bar reads as the same surface as the terminal body. Alpha follows
     * window opacity so transparency stays uniform across the whole window. */
    f32 op = app->config.opacity;
    Color tb_bg = {t->bg.r, t->bg.g, t->bg.b, op};
    renderer_draw_rect(r, 0, 0, (f32)app->fb_width, tb_h, tb_bg);
    /* No bottom border — the tab bar reads as one surface with the body. */

    Color btn_fg  = {0.55f, 0.55f, 0.58f, 1.0f};
    Color btn_hi  = {0.85f, 0.85f, 0.88f, 1.0f};
    f32 btn_y = (tb_h - icon_sz) / 2;

    /* Sidebar toggle + separator — only when action icons are enabled.
     * The toggle gets its own slightly-larger box (TB_SIDEBAR_ICON_SIZE_PT),
     * vertically centred in the toolbar, with the glyph filling the box (no
     * overflow) so it stays cleanly aligned and its hit target — derived from
     * the same constant in hittest.c — matches exactly. */
    f32 bx = TRAFFIC_LIGHT_ZONE_PT * dpi;
    if (show_icons) {
        Color sb_clr = app->sidebar_visible ? btn_hi : btn_fg;
        f32 sb_box = TB_SIDEBAR_ICON_SIZE_PT * dpi;
        f32 sb_y   = (tb_h - sb_box) * 0.5f;
        draw_toolbar_button_bg(app, bx, sb_y, sb_box, sb_box, TB_SIDEBAR, dpi);
        icon_draw(r, ICON_SIDEBAR, bx, sb_y, sb_box, sb_clr);
        bx += sb_box + TB_BTN_GAP_PT * dpi;
        /* Separator: short, vertically centred in the toolbar. */
        f32 sep_h = icon_sz - 6.0f * dpi;
        renderer_draw_rect(r, bx + 2, (tb_h - sep_h) * 0.5f, 1, sep_h,
                          (Color){0.3f, 0.3f, 0.33f, 0.5f});
        bx += 8 * dpi;
    }

    /* Commit the toolbar background (and left buttons) to the framebuffer
     * before any tab pill is drawn. Tab pills are rounded rects flushed
     * mid-loop; without committing the flat bg first, the bg batch would
     * flush LAST (at icons_done) and paint over the pills, hiding them. */
    renderer_flush_rects(r);

    if (!show_tabs) goto tabs_done;

    /* ========================================================================
     * Tab bar — sharp rail pass
     *
     * Layout:  [ group chip ] [ tab1 tab2 … ] ·  [ ungrouped tabs ]   (+)
     *
     * Visual language:
     *   - No rounded corners: every active, hover, chip and drag state is
     *     built from rects and 1px rules.
     *   - Active tabs read as a connected slab with a top accent and crisp
     *     side rules.
     *   - Inactive tabs stay quiet; hover uses a low-contrast flat band.
     *   - Group colour becomes a hard underline clipped under each group tab.
     *
     * Geometry contract (must match hittest.c):
     *   tab width / group chip width / close-button rect / +24dpi new-tab zone
     *   all compute identically to the original implementation.
     * ======================================================================== */

    f32 tab_gap = app->config.style.tab_gap * dpi;
    /* The inter-tab separator (drawn below) is a 1px line centered in the
     * gap; clear space on each side = (tab_gap + 2*PILL_INSET_X - line)/2.
     * Floor the gap at 7pt so the line gets ~4pt of breathing room on each
     * side instead of sitting cramped against the neighbouring tabs. */
    if (tab_gap < 7.0f * dpi) tab_gap = 7.0f * dpi;
    f32 tab_y  = 0.0f;
    f32 tab_h  = tb_h;                         /* fill the toolbar height */
    f32 close_sz     = app->config.style.tab_close_size * dpi;
    f32 title_pad    = TAB_TITLE_PAD_PT * dpi;

    /* --- Pill geometry ---------------------------------------------------
     * Tabs paint as rounded, inset "pills". The hit zones in hittest.c still
     * span the full tab_w (a click in the inset gap still lands on the tab);
     * only the painted body is inset, which produces the padded rounded look.
     * pill_y / pill_h / pill_radius are loop-invariant; pill_x / pill_w are
     * derived per tab from its render_x. */
    f32 pill_pad_y   = PILL_PAD_Y_PT * dpi;     /* symmetric vertical margin (centered) */
    f32 pill_inset_x = PILL_INSET_X_PT * dpi;   /* horizontal gap between adjacent pills */
    f32 pill_inner   = PILL_INNER_PAD_PT * dpi; /* content padding inside the pill (l/r) */
    f32 pill_y = tab_y + pill_pad_y;
    f32 pill_h = tab_h - 2.0f * pill_pad_y;
    if (pill_h < 8.0f * dpi) { pill_y = tab_y; pill_h = tab_h; }  /* tiny bar: fill */
    f32 pill_radius = 8.0f * dpi;
    if (pill_radius > pill_h * 0.5f) pill_radius = pill_h * 0.5f;
    f32 accent_h = fmaxf(2.5f, 2.5f * dpi);   /* working / group indicator bar */

    /* Count visible tabs (non-collapsed) and group headers */
    i32 visible_tab_count = 0;
    f32 group_header_w = 0;
    {
        bool group_header_shown[MAX_TAB_GROUPS] = {0};
        for (i32 i = 0; i < app->tab_count; i++) {
            i32 gi = app->tabs[i].group_index;
            if (gi >= 0 && gi < MAX_TAB_GROUPS && app->tab_groups[gi].used) {
                if (!group_header_shown[gi]) {
                    group_header_shown[gi] = true;
                    group_header_w += app_get_group_chip_width(app, gi) + tab_gap;
                }
                if (app->tab_groups[gi].collapsed) continue;
            }
            visible_tab_count++;
        }
    }

    /* Reserve space for the right-side cluster (same calc as hittest.c). */
    f32 right_zone = 0;
    {
        /* Right side now holds only the resource monitor text — the
         * Settings / SSH / Font ± buttons were removed. Reserve only
         * the text's width plus a small margin. */
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "%.0f%% %.0fMB", app->res_cpu, app->res_mem_mb);
        right_zone = (f32)(strlen(tmp) + 1) * cw + 16 * dpi;
    }
    /* --- Tab strip is a horizontally-scrolling viewport --------------------
     * Tabs grow to fill the bar when few and shrink to TAB_MIN_W_PT as more
     * open; once they'd be narrower than that, the strip SCROLLS rather than
     * shrinking further (so tabs never collapse into slivers / overlapping
     * ×'s). The "+" button is pinned at the strip's right edge so it stays
     * reachable, and the resource monitor keeps its slot. MUST match the
     * mirror calc in hittest.c. */
    f32 new_tab_btn_w = 24.0f * dpi;
    f32 view_x = bx;
    f32 view_w = (f32)app->fb_width - bx - right_zone - new_tab_btn_w - 10.0f * dpi;
    if (view_w < 40.0f * dpi) view_w = 40.0f * dpi;   /* degenerate-narrow guard */
    /* Keep the scissor rect inside the framebuffer — Metal asserts on an
     * out-of-bounds setScissorRect, and the guard above can overshoot on a
     * very narrow window. */
    if (view_x + view_w > (f32)app->fb_width) view_w = (f32)app->fb_width - view_x;
    if (view_w < 1.0f) view_w = 1.0f;

    f32 max_tab_w = TAB_WIDTH_PT * dpi;  /* MUST match hittest.c's cap (same constant) */
    i32 tab_divisor = visible_tab_count > 0 ? visible_tab_count : 1;
    /* Best-fit so a handful of tabs fill the viewport; group chips scroll with
     * the content, so the tabs share (view_w - chips). */
    f32 raw_tab_w = (view_w - group_header_w - (tab_divisor - 1) * tab_gap) / (f32)tab_divisor;
    f32 tab_w = raw_tab_w;
    if (tab_w > max_tab_w) tab_w = max_tab_w;
    if (tab_w < TAB_MIN_W_PT * dpi) tab_w = TAB_MIN_W_PT * dpi;  /* MUST match hittest.c */

    /* Ratchet against count-driven growth (anti-pulse on close): while the
     * pointer is inside the bar mid close-sequence, hold the previous width so
     * survivors don't pulse wider out from under the cursor on every X-click.
     * Only bites in the non-overflow regime (raw_tab_w < max_tab_w); releases
     * when the pointer leaves or the window is resized. */
    static f32 s_held_tab_w  = 0.0f;
    static i32 s_held_fb_w   = -1;
    bool _mouse_in_tabbar = (app->hover_y >= 0.0f && app->hover_y < tab_y + tab_h);
    if (_mouse_in_tabbar &&
        s_held_fb_w == app->fb_width &&
        s_held_tab_w > 0.0f &&
        tab_w > s_held_tab_w &&
        raw_tab_w < max_tab_w) {
        tab_w = s_held_tab_w;
    }
    s_held_tab_w = tab_w;
    s_held_fb_w  = app->fb_width;

    /* Content extent (scroll width) + the active tab's content-space X. Walk
     * groups-then-ungrouped in the SAME order as the draw loops below so
     * auto-scroll-to-active targets the right pill. */
    f32 content_w = 0.0f;
    f32 active_cx = -1.0f;
    {
        f32 cx = 0.0f;
        for (i32 gi = 0; gi < MAX_TAB_GROUPS; gi++) {
            if (!app->tab_groups[gi].used) continue;
            bool has = false;
            for (i32 i = 0; i < app->tab_count; i++)
                if (app->tabs[i].group_index == gi) { has = true; break; }
            if (!has) continue;
            /* Mirror the draw/hit-test geometry: chip + members pack flush (no
             * inner gap, no capsule pad — the group is marked by a bottom line),
             * then one tab_gap after the whole group. */
            cx += app_get_group_chip_width(app, gi);
            if (!app->tab_groups[gi].collapsed) {
                for (i32 i = 0; i < app->tab_count; i++) {
                    if (app->tabs[i].group_index != gi) continue;
                    if (i == app->active_tab) active_cx = cx;
                    cx += tab_w;
                }
            }
            cx += tab_gap;
        }
        for (i32 i = 0; i < app->tab_count; i++) {
            i32 g = app->tabs[i].group_index;
            if (g >= 0 && g < MAX_TAB_GROUPS && app->tab_groups[g].used) continue;
            if (i == app->active_tab) active_cx = cx;
            cx += tab_w + tab_gap;
        }
        content_w = cx;
    }
    f32 scroll_max = content_w - view_w;
    if (scroll_max < 0.0f) scroll_max = 0.0f;

    /* Auto-scroll the active tab into view when it changes (open / switch). */
    if (app->active_tab != app->_tab_scroll_last_active) {
        if (active_cx >= 0.0f) {
            if (active_cx < app->_tab_scroll_x)
                app->_tab_scroll_x = active_cx;
            else if (active_cx + tab_w > app->_tab_scroll_x + view_w)
                app->_tab_scroll_x = active_cx + tab_w - view_w;
        }
        app->_tab_scroll_last_active = app->active_tab;
    }
    if (app->_tab_scroll_x > scroll_max) app->_tab_scroll_x = scroll_max;
    if (app->_tab_scroll_x < 0.0f) app->_tab_scroll_x = 0.0f;
    f32 tab_scroll = app->_tab_scroll_x;

    /* Preserve drag-target / scroll geometry for main.c + hittest.c */
    app->_tab_bar_x      = bx;
    app->_tab_w          = tab_w;
    app->_tab_gap        = tab_gap;
    app->_tab_scroll_max = scroll_max;
    app->_tab_view_x     = view_x;
    app->_tab_view_w     = view_w;

    /* Active pill fill: the theme's own active-tab colour, drawn fully opaque
     * (solid — never see-through), lifted to at least bg+0.06 so the pill is
     * always clearly filled even on themes whose tab_active_bg sits near bg. */
    Color tab_active_fill = {
        fmaxf(t->tab_active_bg.r, t->bg.r + 0.06f),
        fmaxf(t->tab_active_bg.g, t->bg.g + 0.06f),
        fmaxf(t->tab_active_bg.b, t->bg.b + 0.06f),
        1.0f
    };
    Color tab_hover_fill = {
        fminf(1.0f, t->bg.r + 0.035f),
        fminf(1.0f, t->bg.g + 0.035f),
        fminf(1.0f, t->bg.b + 0.035f),
        0.9f
    };
    Color _ui_ac = theme_ui_accent(t);
    Color tab_accent = {_ui_ac.r, _ui_ac.g, _ui_ac.b, 0.95f};
    /* Pill rim: a hairline a touch lighter than the fill for a crisp edge. */
    Color pill_border = {
        fminf(1.0f, tab_active_fill.r + 0.07f),
        fminf(1.0f, tab_active_fill.g + 0.07f),
        fminf(1.0f, tab_active_fill.b + 0.07f),
        0.7f
    };
    /* Inline tab-rename editor ("Change Title"). All four colors derive from
     * the theme — the old hardcoded blue/white set ignored the palette and
     * clashed on anything that wasn't the stock dark theme. The field fill is
     * the active pill blended back toward the terminal bg, which reads as an
     * inset input on both dark and light themes; underline + caret take the
     * UI accent; the text picks whichever of the theme's fg/bg is legible on
     * the resulting fill. */
    Color rename_fill_c = {
        tab_active_fill.r + (t->bg.r - tab_active_fill.r) * 0.65f,
        tab_active_fill.g + (t->bg.g - tab_active_fill.g) * 0.65f,
        tab_active_fill.b + (t->bg.b - tab_active_fill.b) * 0.65f,
        1.0f
    };
    Color rename_accent_c = {_ui_ac.r, _ui_ac.g, _ui_ac.b, 0.9f};
    Color rename_caret_c  = {_ui_ac.r, _ui_ac.g, _ui_ac.b, 1.0f};
    Color rename_text_c   = chrome_legible_on(rename_fill_c);

    /* Dragged-tab tracking: skip in the normal pass, render floating on top. */
    i32  dragged_idx   = app->tab_dragging ? app->tab_drag_index : -1;
    bool dragged_in_group  = false;
    Color dragged_group_col = (Color){0,0,0,0};

    f32 cur_x = bx - tab_scroll;   /* content scrolled left by tab_scroll */

    /* --- Pass: collect tab positions so we can render groups → ungrouped in
     *     a single unified loop while tracking drag/underline state. --- */

    /* Helper macro: renders one tab at (tx, tab_y) with width tab_w.
     * Variables captured: app, r, t, dpi, cw, ch, tab_h, close_sz,
     * title_pad, pill_inset_x, pill_inner, pill_y, pill_h, pill_radius,
     * accent_h, tab_y, tb_h, tab_active_fill, tab_hover_fill, pill_border,
     * rename_fill_c, rename_accent_c, rename_text_c, rename_caret_c. */
    #define RENDER_TAB_BODY(tab_i, tab_tx, in_group, g_color, is_ghost)            \
    do {                                                                           \
        i32   _ti = (tab_i);                                                       \
        f32   _target_tx = (tab_tx);                                               \
                                                                                   \
        if (app->tabs[_ti].render_x == 0.0f || (is_ghost)) {                       \
            app->tabs[_ti].render_x = _target_tx;                                  \
        } else {                                                                   \
            /* Eased spring towards target X */                                    \
            app->tabs[_ti].render_x += (_target_tx - app->tabs[_ti].render_x) * 0.35f; \
            if (fabsf(app->tabs[_ti].render_x - _target_tx) < 0.5f) {              \
                app->tabs[_ti].render_x = _target_tx;                              \
            }                                                                      \
        }                                                                          \
        f32   _tx = app->tabs[_ti].render_x;                                       \
        /* Tab OPEN animation: fade in + slide from the right. A settled or       \
         * never-animated tab reads 1.0 (fully open, no offset). */               \
        f32   _open_e = app->tabs[_ti].tab_open.active                            \
            ? anim_eased(&app->tabs[_ti].tab_open, EASE_OUT_CUBIC) : 1.0f;         \
        _tx += (1.0f - _open_e) * (14.0f * dpi);                                   \
                                                                                   \
        bool  _active   = (_ti == app->active_tab);                                \
        bool  _hover    = (!app->tab_dragging && !(is_ghost) &&                    \
                           app->hover_y >= 0 && app->hover_y < tb_h &&             \
                           app->hover_x >= _tx && app->hover_x < _tx + tab_w);     \
        bool  _sleeping = app->tabs[_ti].sleeping;                                 \
        f32   _alpha    = ((is_ghost) ? 0.88f : 1.0f) * _open_e;                   \
                                                                                   \
        /* Known foreground AI CLI in this tab? _has_agent_icon drives the leading \
         * icon (shown whenever the agent is present). _agent_working is the       \
         * narrower "agent is busy right now" signal — gated on recent PTY output  \
         * — and that is what animates the accent bar. */                          \
        ChatTool _agent_tool = tab_agent_tool(&app->tabs[_ti]);                     \
        bool _has_agent_icon = (_agent_tool != CHAT_TOOL_UNKNOWN);                  \
        /* Grok runs as a full-screen TUI that keeps repainting (status line /  \
         * spinner) even when idle, pegging the output-burst heuristic so its    \
         * tab "working" bar would stay lit forever. It shows its own spinner,   \
         * so exclude it here; agents that fall quiet when idle read correctly.  \
         * (XAI = grok, see tab_agent_tool / chat_tool_display.) */              \
        bool _agent_working  = _has_agent_icon && _agent_tool != CHAT_TOOL_XAI && \
            (platform_time_sec() < app->tabs[_ti].agent_work_until);                \
        Color _accent_base   = (in_group) ? (g_color) : tab_accent;                \
                                                                                   \
        /* Pill body — rounded, inset, raised. Active / dragged tabs get a       \
         * filled bordered pill with a soft drop shadow; hover gets a faint       \
         * rounded fill; idle inactive tabs stay bare (text only). The pill is    \
         * flushed straight away so the accent / icon / title composite on top    \
         * of it (rrects otherwise layer above flat rects at the final flush). */ \
        f32 _pillx = _tx + pill_inset_x;                                          \
        f32 _pillw = tab_w - 2.0f * pill_inset_x;                                 \
        if (_active || (is_ghost)) {                                              \
            Color _fill = tab_active_fill; _fill.a = _alpha;                      \
            Color _bord = pill_border;     _bord.a *= _alpha;                     \
            /* Flat solid pill — no drop shadow (a soft shadow read as a          \
             * translucent halo). Only the dragged ghost keeps a lift shadow. */  \
            f32 _sh_a  = (is_ghost ? 0.42f : 0.0f) * _alpha;                      \
            f32 _sh_sz = (is_ghost ? 13.0f : 0.0f) * dpi;                         \
            renderer_draw_rrect_bordered(r, _pillx, pill_y, _pillw, pill_h,       \
                _fill, _bord, fmaxf(1.0f, dpi),                                   \
                pill_radius, pill_radius, pill_radius, pill_radius,               \
                _sh_sz, _sh_a, 0.0f, (is_ghost ? 1.5f * dpi : 0.0f));            \
            renderer_flush_rrects(r);                                             \
        } else if (_hover) {                                                      \
            any_tab_hovered_this_frame = true;                                    \
            if (_ti != s_hover_tab) {                                             \
                s_hover_tab = _ti;                                                \
                anim_start(&s_hover_anim, 0.18);                                  \
            }                                                                     \
            f32 _ha = anim_eased(&s_hover_anim, EASE_OUT_CUBIC);                  \
            Color _h = tab_hover_fill; _h.a *= _ha * _alpha;                      \
            renderer_draw_rrect_simple(r, _pillx, pill_y, _pillw, pill_h,         \
                                       _h, pill_radius);                          \
            renderer_flush_rrects(r);                                             \
        }                                                                         \
                                                                                  \
        /* Bottom-edge indicator inside the pill band:                           \
         *   - agent working → breathing / flowing accent (active or inactive)   \
         *   - else grouped  → static group-colour identity bar                  \
         * Both are flat rects, composited above the already-flushed pill. */     \
        f32 _acc_x = _pillx + 4.0f * dpi;                                         \
        f32 _acc_w = _pillw - 8.0f * dpi;                                         \
        f32 _acc_y = pill_y + pill_h - accent_h - 2.0f * dpi;                     \
        /* Group membership is shown by the enclosing capsule now, not a         \
         * per-tab bar (which used to collide with this accent), so only the     \
         * agent-working accent paints here. */                                  \
        if (_acc_w > 6.0f * dpi && _agent_working) {                             \
            draw_tab_accent(r, _acc_x, _acc_y, _acc_w, accent_h,                  \
                            _accent_base, true, _alpha);                          \
        }                                                                         \
                                                                                   \
        Color _fg = _active ? t->tab_active_fg : t->tab_inactive_fg;               \
        if (_sleeping) _fg.a *= 0.55f;                                             \
        _fg.a *= _alpha;                                                           \
                                                                                   \
        /* Leading agent icon (uses _agent_tool / _has_agent_icon from above).      \
         * Positioned from the pill's inner-padding edge, not the slot edge. */     \
        f32 _lead_x = _pillx + pill_inner;                                         \
        f32 _agent_sz = 16.0f * dpi;                                               \
        if (_agent_sz > tab_h - 8.0f * dpi) _agent_sz = tab_h - 8.0f * dpi;         \
        if (_agent_sz < 8.0f * dpi) _agent_sz = 8.0f * dpi;                        \
        f32 _agent_y = tab_y + (tab_h - _agent_sz) * 0.5f;                         \
        if (_has_agent_icon) {                                                     \
            i32 _iw = 0, _ih = 0;                                                  \
            const u8 *_px = agent_icon_rgba(_agent_tool, &_iw, &_ih);              \
            if (_px) {                                                             \
                renderer_flush_rects(r);                                           \
                renderer_draw_image(r, _px, _iw, _ih,                              \
                                    _lead_x, _agent_y, _agent_sz, _agent_sz);      \
            } else {                                                               \
                AgentTint _tint = agent_icon_tint(_agent_tool);                    \
                draw_text_ex(r, agent_icon_letter(_agent_tool),                    \
                             _lead_x + (_agent_sz - cw) * 0.5f,                    \
                             tab_y + (tab_h - ch) * 0.5f - 1.0f * dpi,             \
                             (Color){_tint.r, _tint.g, _tint.b, _alpha}, 1, cw);   \
            }                                                                      \
        }                                                                          \
                                                                                   \
        /* Bell pip: keep notification affordance, but do not create a fake        \
         * leading session icon on idle terminals. */                              \
        if (!_active && app->tabs[_ti].has_bell) {                                 \
            f32 _bs = fmaxf(3.0f * dpi, 3.0f);                                     \
            f32 _bx = _has_agent_icon ? (_lead_x + _agent_sz - _bs)                \
                                      : (_pillx + pill_inner * 0.5f);              \
            f32 _by = tab_y + 4.0f * dpi;                                          \
            draw_square_mark(r, _bx, _by, _bs,                                     \
                             (Color){1.0f, 0.65f, 0.1f, 1.0f * _alpha});           \
        }                                                                          \
                                                                                   \
        /* Close (×): hidden on inactive tabs unless they're hovered. Anchored to  \
         * the pill's right inner-padding edge (kept in sync with hittest.c). */    \
        f32 _cx = _pillx + _pillw - pill_inner - close_sz;                         \
        /* Center on a pixel-snapped row so the SF-symbol ×, which icon_draw     \
         * floors to integer pixels, lines up with the geometric + button (which \
         * uses the same snapped center) instead of drifting half a pixel. */    \
        f32 _cy = roundf(tab_y + tab_h * 0.5f) - close_sz * 0.5f;                  \
        bool _close_hover = (!app->tab_dragging && !(is_ghost) &&                  \
                             app->hover_y >= tab_y && app->hover_y < tab_y + tab_h \
                          && app->hover_x >= _cx && app->hover_x < _cx + close_sz);\
        /* The close × occupies a fixed 27.5pt zone at the pill's right edge.     \
         * When a tab is narrower than that the × would spill past its own left    \
         * edge into the neighbouring tab (the "overlapping ×" at high tab         \
         * counts) — so hide × entirely below TAB_CLOSE_MIN_W_PT, for the ACTIVE   \
         * tab too. Narrow tabs are then icon/title-only; close via Cmd+W until    \
         * closing siblings lets the survivors widen again. MUST match hittest.c.  \
         * (Hiding it also keeps a tiny pill a pure select target.) */             \
        bool _close_room  = tab_w >= TAB_CLOSE_MIN_W_PT * dpi;                     \
        bool _show_close  = _close_room && (_active || _hover || _close_hover);    \
                                                                                   \
        /* Title */                                                                \
        f32 _title_x = _has_agent_icon                                             \
            ? (_lead_x + _agent_sz + 8.0f * dpi)                                   \
            : (_pillx + pill_inner);                                              \
        f32 _title_y = tab_y + (tab_h - ch) * 0.5f;                   \
        f32 _close_reserve = _show_close ? (close_sz + pill_inner) : pill_inner;   \
        f32 _title_max_w = _pillw - (_title_x - _pillx) - _close_reserve - title_pad; \
        i32 _maxc = (i32)(_title_max_w / cw);                                      \
        if (_maxc < 1) _maxc = 1;                                                  \
                                                                                   \
        if (app->tab_rename_active && app->tab_rename_index == _ti && !(is_ghost))\
        {                                                                          \
            renderer_draw_rect(r, _title_x - 3.0f * dpi, _title_y - 1.0f * dpi,    \
                               _title_max_w + 6.0f * dpi, ch + 2.0f * dpi,         \
                               rename_fill_c);                                     \
            renderer_draw_rect(r, _title_x - 3.0f * dpi,                           \
                               _title_y + ch + 1.0f * dpi,                         \
                               _title_max_w + 6.0f * dpi, 1.0f * dpi,              \
                               rename_accent_c);                                   \
            draw_text_ex(r, app->tab_rename_buf, _title_x, _title_y,               \
                         rename_text_c, _maxc, cw);                                \
            i32 _cpos = (i32)utf8_len((const u8 *)app->tab_rename_buf,             \
                                      (usize)app->tab_rename_len);                 \
            if (_cpos > _maxc) _cpos = _maxc;                                      \
            renderer_draw_rect(r, _title_x + _cpos * cw, _title_y,                 \
                               fmaxf(1.0f, dpi), ch,                               \
                               rename_caret_c);                                    \
        } else {                                                                   \
            char _disp[160];                                                       \
            tab_format_display_title(&app->tabs[_ti], _disp, (i32)sizeof(_disp));  \
            draw_text_ex(r, _disp, _title_x, _title_y, _fg, _maxc, cw);            \
        }                                                                          \
                                                                                   \
        /* Close button render */                                                  \
        if (_show_close) {                                                         \
            if (_close_hover) {                                                    \
                Color _badge = {0.92f, 0.32f, 0.32f, 0.20f};                       \
                f32 _bs = close_sz;                                                \
                renderer_draw_rrect_simple(r, _cx, _cy, _bs, _bs, _badge,          \
                                           4.0f * dpi);                            \
            }                                                                      \
            Color _close_fg = _close_hover                                         \
                ? (Color){0.98f, 0.45f, 0.45f, 1.0f * _alpha}                      \
                : (Color){_fg.r, _fg.g, _fg.b,                                     \
                          (_active ? 0.55f : 0.70f) * _alpha};                     \
            /* SF-Symbol xmark via icon_draw — replaces the ASCII 'x'.             \
             * Sized to ~62% of the click target for the right visual              \
             * weight; pixel-snapped inside icon_draw. */                          \
            f32 _ix_sz = close_sz * 0.62f;                                         \
            icon_draw(r, ICON_CLOSE,                                               \
                      _cx + (close_sz - _ix_sz) * 0.5f,                            \
                      _cy + (close_sz - _ix_sz) * 0.5f,                            \
                      _ix_sz, _close_fg);                                          \
        }                                                                          \
        /* Group identity is shown by the bottom-edge bar handled in the pill     \
         * indicator block above, so no separate underline is drawn here. */       \
    } while (0)

    /* Clip the tab strip to its viewport so scrolled-out tabs (and their
     * titles / × / accent bars) don't paint over the sidebar toggle or the
     * resource monitor. Flush pre-tab toolbar content first so it isn't caught
     * by the scissor; tab content is then drawn AND flushed inside it (the
     * scissor applies at flush time), and popped before the pinned "+". */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
    renderer_push_scissor(r, view_x, tab_y, view_w, tab_h);

    /* --- Grouped tabs --- */
    bool any_group_drawn = false;
    for (i32 gi = 0; gi < MAX_TAB_GROUPS; gi++) {
        if (!app->tab_groups[gi].used) continue;

        bool has_tabs = false;
        for (i32 i = 0; i < app->tab_count; i++)
            if (app->tabs[i].group_index == gi) { has_tabs = true; break; }
        if (!has_tabs) continue;

        TabGroup *grp = &app->tab_groups[gi];
        i32 grp_tab_count = app_tab_group_count_tabs(app, gi);

        /* --- Group chip --- */
        f32 chip_y = tab_y + 2.0f * dpi;
        f32 chip_h = tab_h - 6.0f * dpi;
        if (chip_h < 6.0f * dpi) chip_h = 6.0f * dpi;

        f32 chip_w = app_get_group_chip_width(app, gi);

        /* Collapse factor: 0 = fully expanded, 1 = fully collapsed. Drives the
         * member-tab width + capsule width so toggling the chip squeezes the
         * members in/out smoothly (subsequent tabs slide via their render_x
         * springs). `collapsed` is the TARGET; the eased anim runs toward it. */
        f32 _ce = anim_done(&grp->collapse_anim)
                    ? 1.0f : anim_eased(&grp->collapse_anim, EASE_OUT_CUBIC);
        f32 cf = grp->collapsed ? _ce : (1.0f - _ce);
        if (cf < 0.0f) cf = 0.0f; else if (cf > 1.0f) cf = 1.0f;
        f32 mtab_w = tab_w * (1.0f - cf);                 /* animated member width */
        bool draw_members = (grp_tab_count > 0 && cf < 0.999f);

        /* Chrome-style group identity: NO enclosing capsule — just a rounded
         * group-coloured underline spanning the chip + member tabs, drawn after
         * the content below. Remember the group's left edge to size that line. */
        f32 grp_left = cur_x;

        /* Chip fill: tint 18 % of group colour over the toolbar bg.
         * When this chip is the live drag drop-target, lift the tint
         * to ~45 % so the user sees a clear "I'll land here" pulse. */
        bool is_drop_target = (app->tab_dragging &&
                               app->tab_drag_target_group == gi);
        f32 tint = is_drop_target ? 0.45f : 0.18f;
        Color chip_bg = {
            grp->color.r * tint + (t->bg.r + 0.025f) * (1.0f - tint),
            grp->color.g * tint + (t->bg.g + 0.025f) * (1.0f - tint),
            grp->color.b * tint + (t->bg.b + 0.025f) * (1.0f - tint),
            1.0f
        };
        /* Rounded chip, matching the pill language. The drop-target state
         * swaps the hairline border for a full-strength group-colour ring
         * plus a soft shadow instead of the old four-rect outline. */
        f32 chip_radius = 7.0f * dpi;
        if (chip_radius > chip_h * 0.5f) chip_radius = chip_h * 0.5f;
        Color chip_edge = is_drop_target
            ? (Color){ grp->color.r, grp->color.g, grp->color.b, 0.95f }
            : (Color){ t->border.r, t->border.g, t->border.b, 0.38f };
        f32 chip_bw = is_drop_target ? (1.5f * dpi) : fmaxf(1.0f, dpi);
        renderer_draw_rrect_bordered(r, cur_x, chip_y, chip_w, chip_h,
            chip_bg, chip_edge, chip_bw,
            chip_radius, chip_radius, chip_radius, chip_radius,
            is_drop_target ? (8.0f * dpi) : 0.0f,
            is_drop_target ? 0.30f : 0.0f, 0.0f, 1.0f * dpi);
        renderer_flush_rrects(r);

        /* Colour swatch inside the chip — and the open/closed indicator.
         * The dot itself carries the state (no chevron): an OPEN group is a
         * solid dot with a soft outer halo ("lit"), a CLOSED group is a
         * hollow ring ("off"). Solid-vs-hollow is binary at a glance, costs
         * zero extra width, and reads on both dark and light themes because
         * both states use the group colour at full strength. */
        f32 g_dot_sz = chip_h * 0.38f;
        f32 g_dot_x  = cur_x + chip_h * 0.46f;
        f32 g_dot_y  = chip_y + (chip_h - g_dot_sz) * 0.5f;
        if (grp->collapsed) {
            /* Hollow ring: group-colour disc with the chip fill punched
             * back in (ring thickness ~19% of the dot). */
            renderer_draw_rrect_simple(r, g_dot_x, g_dot_y, g_dot_sz, g_dot_sz,
                                       grp->color, g_dot_sz * 0.5f);
            f32 hole = g_dot_sz * 0.62f;
            f32 hx = g_dot_x + (g_dot_sz - hole) * 0.5f;
            f32 hy = g_dot_y + (g_dot_sz - hole) * 0.5f;
            renderer_draw_rrect_simple(r, hx, hy, hole, hole,
                                       chip_bg, hole * 0.5f);
        } else {
            /* Open group: a clean solid dot (no halo) — the dashed/transparent
             * glow read as a stray ring, so it's gone. */
            renderer_draw_rrect_simple(r, g_dot_x, g_dot_y, g_dot_sz, g_dot_sz,
                                       grp->color, g_dot_sz * 0.5f);
        }
        renderer_flush_rrects(r);

        /* Label (or inline rename buffer) */
        bool is_renaming = (app->chip_rename_active &&
                            app->chip_rename_group_index == gi);
        char glabel[80];
        if (is_renaming)
            snprintf(glabel, sizeof(glabel), "%s", app->chip_rename_buf);
        else if (grp->collapsed)
            snprintf(glabel, sizeof(glabel), "%s %d", grp->name, grp_tab_count);
        else
            snprintf(glabel, sizeof(glabel), "%s", grp->name);
        f32 glbl_x = g_dot_x + g_dot_sz + 8.0f * dpi;   /* breathing room dot→label */
        f32 glbl_y = chip_y + (chip_h - ch) * 0.5f;
        /* No chevron anymore — the label can run to the chip's right pad
         * (8dpi keeps the last glyph clear of the 7dpi corner radius). */
        i32 lbl_max = (i32)((cur_x + chip_w - 8.0f * dpi - glbl_x) / cw);
        if (lbl_max < 1) lbl_max = 1;
        Color glbl_fg = {
            fminf(1.0f, grp->color.r * 0.75f + 0.35f),
            fminf(1.0f, grp->color.g * 0.75f + 0.35f),
            fminf(1.0f, grp->color.b * 0.75f + 0.35f),
            0.95f
        };
        if (is_renaming) {
            /* Soft highlight bar under the label and a 1dpi accent below */
            Color hl = { grp->color.r, grp->color.g, grp->color.b, 0.18f };
            renderer_draw_rect(r, glbl_x - 2.0f * dpi, glbl_y - 1.0f * dpi,
                               (f32)lbl_max * cw + 4.0f * dpi, ch + 2.0f * dpi, hl);
            Color acc = { grp->color.r, grp->color.g, grp->color.b, 0.85f };
            renderer_draw_rect(r, glbl_x - 2.0f * dpi, glbl_y + ch + 1.0f * dpi,
                               (f32)lbl_max * cw + 4.0f * dpi, 1.0f * dpi, acc);
            draw_text_ex(r, glabel, glbl_x, glbl_y, glbl_fg, lbl_max, cw);
            /* Caret (codepoint column, not byte offset — UTF-8 safe) */
            i32 cpos = (i32)utf8_len((const u8 *)app->chip_rename_buf,
                                     (usize)app->chip_rename_len);
            if (cpos > lbl_max) cpos = lbl_max;
            renderer_draw_rect(r, glbl_x + (f32)cpos * cw, glbl_y,
                               fmaxf(1.0f, dpi), ch,
                               (Color){0.55f, 0.75f, 1.0f, 1.0f});
        } else {
            draw_text_ex(r, glabel, glbl_x, glbl_y, glbl_fg, lbl_max, cw);
            /* State lives in the dot: solid + halo = open, hollow ring =
             * closed (plus the "name N" hidden-tab count while closed).
             * The chevron is gone — it read as decoration, not state. */
        }

        /* Members sit flush against the chip and each other inside the capsule
         * — no inter-tab gap within a group. */
        cur_x += chip_w;

        /* Tabs in this group — drawn while not fully collapsed, at the animated
         * width mtab_w so they squeeze in/out. tab_w is briefly retargeted to
         * mtab_w around each member draw (the macro reads tab_w for the pill /
         * hover / close geometry), then restored. */
        if (draw_members) {
            for (i32 i = 0; i < app->tab_count; i++) {
                if (app->tabs[i].group_index != gi) continue;
                if (i == dragged_idx) {
                    dragged_in_group  = true;
                    dragged_group_col = grp->color;
                    cur_x += mtab_w;
                    continue;
                }
                if (mtab_w > 2.0f * pill_inset_x + 2.0f * dpi) {
                    f32 _saved_tw = tab_w;
                    tab_w = mtab_w;
                    RENDER_TAB_BODY(i, cur_x, true, grp->color, false);
                    tab_w = _saved_tw;
                }
                cur_x += mtab_w;
            }
        }

        /* Chrome-style underline: a rounded group-coloured bar along the bottom
         * spanning the chip + member tabs (grp_left → cur_x). It shrinks with
         * the collapse animation since cur_x tracks the animated member widths. */
        {
            f32 ln_h = 3.0f * dpi;
            f32 ln_x = grp_left;                              /* chip's left edge */
            f32 ln_w = (cur_x - grp_left) - pill_inset_x;     /* → last pill's right */
            f32 ln_y = tab_y + tab_h - ln_h - 1.0f * dpi;
            if (ln_w > 4.0f * dpi) {
                renderer_draw_rrect_simple(r, ln_x, ln_y, ln_w, ln_h,
                                           grp->color, ln_h * 0.5f);
                renderer_flush_rrects(r);
            }
        }

        /* One gap separating the whole group from the next unit. */
        cur_x += tab_gap;
        any_group_drawn = true;
    }

    /* --- Ungrouped tabs ---
     * Idle inactive tabs render as bare text; a Chrome-style vertical
     * separator sits in the gap between adjacent tabs (n tabs → n-1 lines)
     * so neighbouring titles don't run together. Seeded from any_group_drawn
     * so the FIRST ungrouped tab after a group also gets a leading divider —
     * a clear line between the group capsule and the standalone terminal. */
    bool _drew_prev_tab = any_group_drawn;
    for (i32 i = 0; i < app->tab_count; i++) {
        i32 gi = app->tabs[i].group_index;
        if (gi >= 0 && gi < MAX_TAB_GROUPS && app->tab_groups[gi].used) continue;
        if (i == dragged_idx) {
            cur_x += tab_w + tab_gap;
            continue;
        }
        /* Separator centered in the gap to the LEFT of this tab (i.e. between
         * it and the previously-rendered tab); a short, vertically-centered
         * hairline like the tab dividers in Chromium. */
        if (_drew_prev_tab) {
            f32 _sln = fmaxf(1.0f, dpi);
            f32 _slh = pill_h * 0.5f;
            f32 _sly = pill_y + (pill_h - _slh) * 0.5f;
            f32 _slx = cur_x - tab_gap * 0.5f - _sln * 0.5f;
            renderer_draw_rect(r, _slx, _sly, _sln, _slh, pill_border);
        }
        Color _no_group = {0,0,0,0};
        RENDER_TAB_BODY(i, cur_x, false, _no_group, false);
        cur_x += tab_w + tab_gap;
        _drew_prev_tab = true;
    }

    /* --- Tab CLOSE ghost ---
     * The just-closed pill, drawn on top, shrinking toward its centre and
     * fading out at its old slot while the surviving tabs slide into the gap. */
    if (app->tab_close_active) {
        if (anim_done(&app->tab_close_anim)) {
            app->tab_close_active = false;
        } else {
            f32 _ce = anim_eased(&app->tab_close_anim, EASE_OUT_CUBIC);  /* 0→1 */
            f32 _ga = 1.0f - _ce;                                        /* fade out */
            f32 _gpillx = app->tab_close_x + pill_inset_x;
            f32 _gpillw = app->tab_close_w - 2.0f * pill_inset_x;
            f32 _gscale = 1.0f - 0.32f * _ce;                            /* shrink to centre */
            f32 _gcx = _gpillx + _gpillw * 0.5f;
            f32 _gcy = pill_y + pill_h * 0.5f;
            f32 _gw = _gpillw * _gscale;
            f32 _gh = pill_h * _gscale;
            if (_gpillw > 1.0f) {
                Color _gf = tab_active_fill; _gf.a *= _ga;
                Color _gb = pill_border;     _gb.a *= _ga;
                renderer_draw_rrect_bordered(r, _gcx - _gw * 0.5f, _gcy - _gh * 0.5f,
                    _gw, _gh, _gf, _gb, fmaxf(1.0f, dpi),
                    pill_radius, pill_radius, pill_radius, pill_radius,
                    0.0f, 0.0f, 0.0f, 0.0f);
                renderer_flush_rrects(r);
            }
        }
    }

    /* Flush all tab content while the scissor is still active, then drop the
     * clip so the pinned "+" and the floating dragged ghost paint unclipped. */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
    renderer_pop_scissor(r);

    /* The "+" follows the last tab, but is pinned at the viewport's right edge
     * once the strip overflows so it stays visible (never under the monitor). */
    f32 plus_x = cur_x;
    f32 plus_pin = view_x + view_w + 2.0f * dpi;
    if (plus_x > plus_pin) plus_x = plus_pin;
    if (plus_x < view_x)   plus_x = view_x;
    app->_tab_plus_x = plus_x;

    /* --- New tab (+) button ---
     * 24dpi-wide zone (matches hittest.c). Flat hover backdrop with a
     * crisp centred plus. */
    {
        f32 new_zone_w = 24.0f * dpi;
        f32 ncy = roundf(tb_h * 0.5f);   /* same snapped center as the tab × */
        f32 ncx = roundf(plus_x + new_zone_w * 0.5f);

        bool plus_hover = (app->hover_y >= 0 && app->hover_y < tb_h &&
                           app->hover_x >= plus_x && app->hover_x < plus_x + new_zone_w);
        /* Hover backdrop — a touch lighter than the inactive tab fill. Kept
         * opaque here so the plus colour below can be derived from it. */
        Color hover_bg = {
            fminf(1.0f, t->tab_inactive_bg.r + 0.08f),
            fminf(1.0f, t->tab_inactive_bg.g + 0.08f),
            fminf(1.0f, t->tab_inactive_bg.b + 0.08f),
            1.0f
        };
        if (plus_hover) {
            f32 box = 18.0f * dpi;
            Color bg = hover_bg; bg.a = 0.7f;
            renderer_draw_rrect_bordered(r, ncx - box * 0.5f, ncy - box * 0.5f,
                box, box, bg,
                (Color){t->border.r, t->border.g, t->border.b, 0.45f},
                fmaxf(1.0f, dpi),
                5.0f * dpi, 5.0f * dpi, 5.0f * dpi, 5.0f * dpi,
                0.0f, 0.0f, 0.0f, 0.0f);
            renderer_flush_rrects(r);
        }
        /* On hover pick a colour that stays legible on the hover backdrop —
         * a hardcoded near-white vanished on light/negative themes. */
        Color plus_fg = plus_hover
            ? chrome_legible_on(hover_bg)
            : (Color){btn_fg.r, btn_fg.g, btn_fg.b, 0.85f};
        f32 arm  = 5.0f * dpi;
        f32 thk  = fmaxf(1.0f, 1.25f * dpi);
        renderer_draw_rect(r, ncx - arm,      ncy - thk * 0.5f, arm * 2.0f, thk,        plus_fg);
        renderer_draw_rect(r, ncx - thk*0.5f, ncy - arm,        thk,        arm * 2.0f, plus_fg);
    }

    /* --- Drop-slot indicator removed (live array reorder handles visual feedback) --- */

    /* --- Floating dragged tab on top --- */
    if (dragged_idx >= 0 && app->tab_dragging && !app->tab_drag_into_split) {
        f32 ghost_x = app->hover_x - tab_w * 0.5f;
        if (ghost_x < bx) ghost_x = bx;
        if (ghost_x + tab_w > (f32)app->fb_width) ghost_x = (f32)app->fb_width - tab_w;

        /* The ghost's own bordered pill + deeper drop shadow (is_ghost path
         * in RENDER_TAB_BODY) carries the "lifted" look — no extra frame. */
        RENDER_TAB_BODY(dragged_idx, ghost_x,
                        dragged_in_group, dragged_group_col, true);
    }

    #undef RENDER_TAB_BODY

    /* Advance cur_x past the new-tab hit zone so tabs_done math stays stable. */
    cur_x += 24.0f * dpi;

tabs_done:
    /* If no tab is hovered this frame, reset s_hover_tab so re-entering a
     * previously-hovered tab triggers a fresh fade-in. */
    if (!any_tab_hovered_this_frame) s_hover_tab = -1;

    if (!show_icons) goto icons_done;

    /* ---- Resource monitor (rightmost) ---- */
    {
        /* Update every ~2s — Mach task_threads + per-thread thread_info
         * is not free, and a 2 Hz cadence in the tab-bar render path was
         * a measurable contributor to idle CPU jitter. */
        f64 now = platform_time_sec();
        if (now - app->res_last_update > 2.0) {
            query_resources(&app->res_cpu, &app->res_mem_mb);
            app->res_last_update = now;
        }

        char res_buf[48];
        snprintf(res_buf, sizeof(res_buf), "%.0f%% %.0fMB", app->res_cpu, app->res_mem_mb);
        f32 res_w = (f32)strlen(res_buf) * cw;
        f32 res_x = (f32)app->fb_width - res_w - 8 * dpi;
        f32 res_y = btn_y + (icon_sz - ch) / 2;

        /* Color: green if low, yellow if medium, red if high */
        Color res_clr = {0.45f, 0.55f, 0.45f, 1.0f}; /* dim green */
        if (app->res_cpu > 50) res_clr = (Color){0.8f, 0.7f, 0.3f, 1.0f};
        if (app->res_cpu > 100) res_clr = (Color){0.8f, 0.35f, 0.3f, 1.0f};

        draw_text_ex(r, res_buf, res_x, res_y, res_clr, 20, cw);
    }

    /* Right-side toolbar buttons (Settings / SSH / Font ± / Font −)
     * removed — the user wanted only the resource monitor on the right.
     * Same actions are still reachable via the command palette (Cmd+K)
     * or keyboard shortcuts. */

icons_done:
    renderer_flush_rects(r);
    renderer_flush_rrects(r);   /* close-button badges + any pending pills */
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

/* =========================================================================
 * Duration format helper
 * ========================================================================= */

static void format_duration(f64 seconds, char *buf, i32 buf_size) {
    if (seconds < 60.0) {
        snprintf(buf, buf_size, "%.1fs", seconds);
    } else if (seconds < 3600.0) {
        i32 mins = (i32)(seconds / 60.0);
        i32 secs = (i32)(seconds) % 60;
        snprintf(buf, buf_size, "%dm %ds", mins, secs);
    } else {
        i32 hours = (i32)(seconds / 3600.0);
        i32 mins = ((i32)(seconds) % 3600) / 60;
        snprintf(buf, buf_size, "%dh %dm", hours, mins);
    }
}

/* =========================================================================
 * Render: Status bar
 * ========================================================================= */

static void render_status_bar(AppState *app) {
    if (!app->config.show_status_bar) return;
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme;
    f32 dpi = app->dpi_scale;

    /* Fixed UI scale for status bar */
    f32 ui_cw = 8.0f * dpi;
    f32 ui_ch = 16.0f * dpi;
    renderer_set_ui_scale(r, ui_cw, ui_ch);
    f32 cw = ui_cw;
    f32 ch = ui_ch;

    f32 y = (f32)app->fb_height - app->status_bar_height;
    f32 w = (f32)app->fb_width;

    {
        f32 op = app->config.opacity;
        Color sb_bg = t->status_bg; sb_bg.a *= op;
        Color sb_bd = t->border;    sb_bd.a *= op;
        renderer_draw_rect(r, 0, y, w, app->status_bar_height, sb_bg);
        /* Hairline separator + soft upward shadow that bleeds into the
         * terminal area, separating the status bar visually. The 1px-tall
         * rrect carries the shadow; positive Y offset would push it down,
         * so we use a NEGATIVE offset to push the shadow up into terminal. */
        renderer_draw_rrect(r,
            0, y, w, 1.0f,
            sb_bd,
            0.0f, 0.0f, 0.0f, 0.0f,
            6.0f * dpi, 0.18f, 0.0f, -3.0f * dpi);
    }

    f32 ty = y + (app->status_bar_height - ch) / 2 + 1;
    f32 px = 10 * dpi;

    Tab *tab = app_active_tab(app);
    if (tab) {
        /* Session type — plain lowercase text, no dot */
        const char *type_s = "local";
        if (tab_primary_session_type(tab) == SESSION_SSH) type_s = "ssh";
        else if (tab_primary_session_type(tab) == SESSION_MOSH) type_s = "mosh";
        draw_text_ex(r, type_s, px, ty, t->status_fg, 8, cw);
        px += (f32)(strlen(type_s) + 1) * cw;

        if (app->broadcast_mode) {
            char bbuf[32];
            snprintf(bbuf, sizeof(bbuf), "broadcast %d", broadcast_selected_count(app));
            draw_text_ex(r, bbuf, px, ty, (Color){0.95f, 0.7f, 0.25f, 1.0f}, 20, cw);
            px += (f32)(strlen(bbuf) + 2) * cw;
        }

        /* CWD */
        if (tab->terminal && tab->terminal->cwd && tab->terminal->cwd[0]) {
            const char *cwd = tab->terminal->cwd;
            const char *home = getenv("HOME");
            char short_cwd[256];
            if (home && strncmp(cwd, home, strlen(home)) == 0) {
                snprintf(short_cwd, sizeof(short_cwd), "~%s", cwd + strlen(home));
            } else {
                snprintf(short_cwd, sizeof(short_cwd), "%s", cwd);
            }
            draw_text_ex(r, short_cwd, px + 8, ty, t->status_fg, 40, cw);
            px += 8 + (f32)strlen(short_cwd) * cw;
        }

        /* Git status — only drawn when the cache says we're in a repo
         * with non-zero activity or commits ahead. Colors: green "+N"
         * for additions, red "-N" for removals, dim "↑K" for ahead. */
        if (tab->git_status.valid && tab->git_status.is_repo) {
            const i32 added   = tab->git_status.added;
            const i32 removed = tab->git_status.removed;
            const i32 ahead   = tab->git_status.ahead;
            if (added || removed || ahead) {
                px += 8 * dpi;  /* gap after cwd */
                char seg[24];
                if (added) {
                    snprintf(seg, sizeof seg, "+%d", added);
                    draw_text_ex(r, seg, px, ty,
                                 (Color){0.4f, 0.85f, 0.4f, 1.0f},
                                 12, cw);
                    px += (f32)strlen(seg) * cw + 4 * dpi;
                }
                if (removed) {
                    snprintf(seg, sizeof seg, "-%d", removed);
                    draw_text_ex(r, seg, px, ty,
                                 (Color){0.95f, 0.4f, 0.4f, 1.0f},
                                 12, cw);
                    px += (f32)strlen(seg) * cw + 4 * dpi;
                }
                if (ahead) {
                    /* '^' is 1 char to keep layout ASCII-safe; the
                     * glyph atlas won't guarantee ↑ on every font. */
                    snprintf(seg, sizeof seg, "^%d", ahead);
                    Color dim = (Color){t->status_fg.r, t->status_fg.g,
                                        t->status_fg.b, 0.7f};
                    draw_text_ex(r, seg, px, ty, dim, 12, cw);
                    px += (f32)strlen(seg) * cw + 4 * dpi;
                }
            }
        }
    }

    /* SFTP transfer inline indicator removed from the center status bar —
     * the rich overlay in the bottom-left (rendered in app_render after all
     * other UI) now owns transfer progress display. */

    /* Right side: quake / group / tab count / git uncommitted summary.
     * Terminal-size and theme were removed — the size is implied by the
     * window itself and the theme by the cell colours; the right corner is
     * better spent on actionable signal (uncommitted code in the cwd). */
    f32 rx = w - 10 * dpi;

    /* Quake mode indicator */
    if (app->quake_active) {
        const char *qstr = "[Q]";
        f32 qw = (f32)strlen(qstr) * cw;
        draw_text_ex(r, qstr, rx - qw, ty,
                    (Color){0.85f, 0.55f, 0.25f, 1.0f}, 4, cw);
        rx -= qw + 8;
    }

    /* Tab group indicator for active tab */
    if (tab && tab->group_index >= 0 && tab->group_index < MAX_TAB_GROUPS &&
        app->tab_groups[tab->group_index].used) {
        TabGroup *grp = &app->tab_groups[tab->group_index];
        f32 gw = (f32)strlen(grp->name) * cw;
        draw_text_ex(r, grp->name, rx - gw, ty, grp->color, 16, cw);
        rx -= gw + 8;
    }

    /* Tab count */
    if (app->tab_count > 1) {
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%d/%d", app->active_tab + 1, app->tab_count);
        f32 tw = (f32)strlen(tbuf) * cw;
        draw_text_ex(r, tbuf, rx - tw, ty, t->status_fg, 8, cw);
        rx -= tw + 16;
    }

    /* Right-corner git summary — laid out RIGHT-to-LEFT so the segments
     * stay flush against the edge. Drawn only when the cwd is a repo with
     * actual uncommitted activity; clean trees stay quiet so the status
     * bar doesn't grow noise. Order on screen (left→right): "+N -M • F" */
    if (tab && tab->git_status.valid && tab->git_status.is_repo) {
        const i32 r_added   = tab->git_status.added;
        const i32 r_removed = tab->git_status.removed;
        const i32 r_files   = tab->git_status.files_changed;
        if (r_added || r_removed || r_files) {
            Color add_clr = (Color){0.4f,  0.85f, 0.4f, 1.0f};
            Color del_clr = (Color){0.95f, 0.4f,  0.4f, 1.0f};
            Color dim_clr = (Color){t->status_fg.r, t->status_fg.g,
                                    t->status_fg.b, 0.75f};
            char seg[32];

            if (r_files) {
                snprintf(seg, sizeof seg, "%d uncommitted", r_files);
                f32 segw = (f32)strlen(seg) * cw;
                draw_text_ex(r, seg, rx - segw, ty, dim_clr, 32, cw);
                rx -= segw + 8 * dpi;
            }
            if (r_removed) {
                snprintf(seg, sizeof seg, "-%d", r_removed);
                f32 segw = (f32)strlen(seg) * cw;
                draw_text_ex(r, seg, rx - segw, ty, del_clr, 12, cw);
                rx -= segw + 6 * dpi;
            }
            if (r_added) {
                snprintf(seg, sizeof seg, "+%d", r_added);
                f32 segw = (f32)strlen(seg) * cw;
                draw_text_ex(r, seg, rx - segw, ty, add_clr, 12, cw);
                rx -= segw + 6 * dpi;
            }
            rx -= 8 * dpi; /* separator gap before the next segment */
        }
    }

    /* Command duration (only show if > 0.5s) */
    if (tab && tab->terminal && tab->terminal->last_cmd_duration > 0.5) {
        char dur_buf[32];
        format_duration(tab->terminal->last_cmd_duration, dur_buf, sizeof(dur_buf));
        /* Prepend timer icon */
        char dur_label[48];
        snprintf(dur_label, sizeof(dur_label), "~ %s", dur_buf);
        f32 dur_w = (f32)strlen(dur_label) * cw;
        Color dim_fg = (Color){t->status_fg.r, t->status_fg.g, t->status_fg.b, 0.6f};
        draw_text_ex(r, dur_label, rx - dur_w, ty, dim_fg, 20, cw);
        rx -= dur_w + 16;
    }

    /* Exit code badge (show when last command failed) */
    if (tab && tab->terminal && tab->terminal->last_cmd_failed) {
        char exit_buf[16];
        snprintf(exit_buf, sizeof(exit_buf), "E:%d", tab->terminal->last_exit_code);
        f32 exit_w = (f32)strlen(exit_buf) * cw;
        f32 badge_pad = 4 * dpi;
        f32 badge_w = exit_w + badge_pad * 2;
        f32 badge_h = ch + 2 * dpi;
        f32 badge_x = rx - badge_w;
        f32 badge_y = y + (app->status_bar_height - badge_h) / 2;
        /* Destructive-context badge — derived from chrome palette so
         * it stays legible on light themes. */
        ChromePalette _cp_exit = chrome_palette_for(t);
        Color exit_bg = _cp_exit.btn_destructive_fg; exit_bg.a = 0.85f;
        renderer_draw_rrect_simple(r, badge_x, badge_y, badge_w, badge_h, exit_bg, 4 * dpi);
        draw_text_ex(r, exit_buf, badge_x + badge_pad, ty,
            chrome_legible_on(exit_bg), 8, cw);
        rx -= badge_w + 16;
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

/* =========================================================================
 * Render: Sidebar
 * ========================================================================= */

const char *app_active_graph_root(AppState *app) {
    Tab *t = app_active_tab(app);
    if (t && t->kind == TAB_FILEBROWSER && t->fb && fb_graph_active(t->fb) &&
        t->fb->graph_root[0])
        return t->fb->graph_root;
    return NULL;
}

static void render_sidebar(AppState *app) {
    /* Render whenever the sidebar has any visible width — covers the open
     * state and the close-animation tail when sidebar_visible has flipped
     * to false but sidebar_width is still animating down toward zero. */
    if (app->sidebar_width <= 0.5f) return;

    const Theme *t = app->config.theme;
    f32 x = 0;
    f32 y = app->tab_bar_height;
    f32 w = app->sidebar_width;
    f32 h = (f32)app->fb_height - y - app->status_bar_height;

    /* File browser is the sidebar content */
    if (!app->filebrowser.open) {
        /* Auto-open to CWD of active terminal. For connected SSH tabs,
         * prefer the SFTP handle + remote home resolved at connect time
         * over the local-shell getcwd fallback — otherwise opening the
         * sidebar on a remote tab lands the user in whatever local dir
         * the app was launched from. */
        Tab *tab = app_active_tab(app);
        bool opened_remote = false;
        if (tab && tab->session && session_type(tab->session) == SESSION_SSH &&
            session_status(tab->session) == SESSION_CONNECTED) {
            void *sftp = session_sftp_handle(tab->session);
            const char *home = session_initial_cwd(tab->session);
            const char *target = (tab->terminal && tab->terminal->cwd && tab->terminal->cwd[0])
                                 ? tab->terminal->cwd : home;
            if (sftp && target && target[0]) {
                app->filebrowser.session = (void *)tab->session;
                opened_remote = fb_navigate_sftp(&app->filebrowser, sftp, target);
                if (opened_remote) tab->sftp_auto_wired = true;
            }
        }
        if (!opened_remote) {
            app->filebrowser.session = NULL;
            /* A full-window Vault graph: list that same Vault rather than the
             * terminal's cwd, so the sidebar mirrors what the graph shows. */
            const char *groot = app_active_graph_root(app);
            if (groot) {
                fb_navigate(&app->filebrowser, groot);
            } else if (tab && tab->terminal && tab->terminal->cwd && tab->terminal->cwd[0]) {
                fb_navigate(&app->filebrowser, tab->terminal->cwd);
            } else {
                char cwd[1024];
                if (getcwd(cwd, sizeof(cwd))) {
                    fb_navigate(&app->filebrowser, cwd);
                }
            }
        }
        app->filebrowser.open = true;
    }

    /* Sidebar renders at fixed UI scale */
    f32 dpi = app->dpi_scale;
    renderer_set_ui_scale(&app->renderer, 8.0f * dpi, 16.0f * dpi);
    /* Apply window opacity to sidebar bg */
    Color side_bg = t->sidebar_bg;
    side_bg.a *= app->config.opacity;

    /* Scissor-clip everything inside the sidebar to its current animated
     * width. During the open/close slide, icons and labels live at fixed
     * x positions (e.g. left-indented at 12dpi) and would otherwise stay
     * fully visible as the sidebar shrinks past them — the user sees
     * "icons linger after the panel slid closed". The scissor cuts them
     * off cleanly at the right edge so the entire sidebar contents move
     * in lockstep with the bg slide. */
    renderer_push_scissor(&app->renderer, x, y, w, h);
    Color fb_accent = theme_ui_accent(t);
    fb_render_sidebar(&app->filebrowser, &app->renderer, x, y, w, h,
                      &side_bg, &t->sidebar_fg, &t->sidebar_active,
                      &t->border, &fb_accent, dpi,
                      app->hover_x, app->hover_y);
    renderer_pop_scissor(&app->renderer);

    /* Resize handle — a divider at the right edge that brightens (plus an
     * accent line) when hovered or while dragging, so the drag affordance is
     * discoverable. The hit zone (hittest.c) is SIDEBAR_RESIZE_GRAB_PT wide on
     * each side of x = w. Drawn outside the scissor so it isn't clipped. */
    {
        f32 grab = SIDEBAR_RESIZE_GRAB_PT * dpi;
        bool over = app->sidebar_resizing ||
                    (app->hover_x >= w - grab && app->hover_x <= w + grab &&
                     app->hover_y >= y && app->hover_y <= y + h);
        Color edge = t->border;
        edge.a = app->config.opacity * (over ? 0.95f : 0.40f);
        f32 lw = over ? fmaxf(2.0f, 2.0f * dpi) : fmaxf(1.0f, dpi);
        renderer_draw_rect(&app->renderer, w - lw, y, lw, h, edge);
        if (over) {
            Color ac = fb_accent;
            ac.a = app->sidebar_resizing ? 0.60f : 0.32f;
            renderer_draw_rect(&app->renderer, w - lw - 1.0f * dpi, y,
                               fmaxf(1.0f, dpi), h, ac);
        }
        renderer_flush_rects(&app->renderer);
    }

    renderer_reset_ui_scale(&app->renderer);
}

/* =========================================================================
 * Render: Scrollbar
 * ========================================================================= */

static void render_scrollbar(AppState *app, Terminal *term, f32 area_x __attribute__((unused)), f32 area_y, f32 area_h) {
    if (!app->config.show_scrollbar) return;
    if (term->sb_count == 0) return;

    /* Auto-hide: only show scrollbar for 2 seconds after scroll activity */
    f64 now = platform_time_sec();
    f64 elapsed = now - app->scrollbar_last_activity;
    if (elapsed > 2.0 && term->scroll_offset == 0) return;

    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme;
    f32 bar_w = 8 * app->dpi_scale;
    f32 bar_x = (f32)app->fb_width - bar_w - app->padding;

    /* Fade alpha based on time since last activity */
    f32 alpha = 1.0f;
    if (elapsed > 1.5) alpha = 1.0f - (f32)((elapsed - 1.5) / 0.5);
    if (alpha < 0.0f) alpha = 0.0f;

    /* Track */
    Color track_c = t->scrollbar;
    track_c.a *= alpha;
    renderer_draw_rect(r, bar_x, area_y, bar_w, area_h, track_c);

    /* Thumb */
    i32 total = term->rows + term->sb_count;
    f32 visible_ratio = (f32)term->rows / (f32)total;
    f32 thumb_h = area_h * visible_ratio;
    if (thumb_h < 20) thumb_h = 20;

    f32 scroll_ratio = (f32)term->scroll_offset / (f32)term->sb_count;
    f32 thumb_y = area_y + (area_h - thumb_h) * (1.0f - scroll_ratio);

    Color thumb_c = t->scrollbar_thumb;
    thumb_c.a *= alpha;
    renderer_draw_rect(r, bar_x + 1, thumb_y, bar_w - 2, thumb_h, thumb_c);

    /* Search result markers on scrollbar */
    if (term->search.active && term->search.match_count > 0) {
        f32 tick_h = 2 * app->dpi_scale;
        f32 tick_w = bar_w - 2;

        for (i32 i = 0; i < term->search.match_count; i++) {
            TermSearchMatch *m = &term->search.matches[i];

            /* Match row is relative to the visible grid (negative = scrollback).
             * Convert to absolute row in the total buffer: scrollback rows + visible row. */
            i32 abs_row = term->sb_count + m->row;
            f32 tick_y = area_y + ((f32)abs_row / (f32)total) * area_h;

            /* Clamp to scrollbar area */
            if (tick_y < area_y) tick_y = area_y;
            if (tick_y + tick_h > area_y + area_h) tick_y = area_y + area_h - tick_h;

            /* Current match in orange, others in yellow */
            Color tick_color;
            if (i == term->search.current) {
                tick_color = (Color){ 1.0f, 0.45f, 0.1f, alpha }; /* orange */
            } else {
                tick_color = (Color){ 1.0f, 0.85f, 0.0f, 0.8f * alpha }; /* yellow */
            }
            renderer_draw_rect(r, bar_x + 1, tick_y, tick_w, tick_h, tick_color);
        }
    }

    /* Failed command markers (semantic zones with non-zero exit code) */
    if (term->last_exit_code != 0 && term->output_start_row >= 0) {
        f32 tick_h = 2 * app->dpi_scale;
        f32 tick_w = bar_w - 2;
        i32 abs_row = term->sb_count + term->prompt_start_row;
        if (abs_row >= 0 && abs_row < total) {
            f32 tick_y = area_y + ((f32)abs_row / (f32)total) * area_h;
            if (tick_y >= area_y && tick_y + tick_h <= area_y + area_h) {
                Color red = { 0.9f, 0.2f, 0.2f, 0.9f * alpha };
                renderer_draw_rect(r, bar_x + 1, tick_y, tick_w, tick_h, red);
            }
        }
    }
}

/* =========================================================================
 * Render: Terminal grid
 * ========================================================================= */

/* =========================================================================
 * Per-row render cache helpers
 * ========================================================================= */

/* Ensure cache storage matches (rows, cols, cw, ch, origin); on change
 * reallocate per-row arrays and force a full rebuild. */
static bool term_render_cache_reserve_rects(TermRenderCache *c, i32 row, i32 min_cap) {
    if (row < 0 || row >= c->cached_rows) return false;
    if (c->bg_caps[row] >= min_cap) return true;

    i32 new_cap = c->bg_caps[row] > 0 ? c->bg_caps[row] : 8;
    while (new_cap < min_cap) new_cap *= 2;

    RectInstance *new_row = realloc(c->bg_rows[row], (usize)new_cap * sizeof(RectInstance));
    if (!new_row) return false;
    c->bg_rows[row] = new_row;
    c->bg_caps[row] = new_cap;
    return true;
}

static bool term_render_cache_reserve_glyphs(TermRenderCache *c, i32 row, i32 min_cap) {
    if (row < 0 || row >= c->cached_rows) return false;
    if (c->glyph_caps[row] >= min_cap) return true;

    i32 new_cap = c->glyph_caps[row] > 0 ? c->glyph_caps[row] : 16;
    while (new_cap < min_cap) new_cap *= 2;

    GlyphInstance *new_row = realloc(c->glyph_rows[row], (usize)new_cap * sizeof(GlyphInstance));
    if (!new_row) return false;
    c->glyph_rows[row] = new_row;
    c->glyph_caps[row] = new_cap;
    return true;
}

/* Reclaim a per-row buffer that grew to a wide line and now holds far fewer
 * cells, bounding the active pane's steady-state working set. The reserve
 * helpers only grow; this is the matching shrink. Called ONLY on a full
 * rebuild (resize / tab switch / theme-font change — occasional), never on the
 * per-frame dirty-row path, so realloc never thrashes. Shrinks only when a cap
 * is ≥64 and ≥4× oversized vs its current count, to a snug power-of-two. */
static void term_render_cache_shrink_row(TermRenderCache *c, i32 row) {
    if (row < 0 || row >= c->cached_rows) return;
    i32 bn = c->bg_counts[row];
    if (c->bg_caps[row] >= 64 && c->bg_caps[row] >= bn * 4 + 4) {
        i32 snug = 8; while (snug < bn + 1) snug *= 2;
        if (snug < c->bg_caps[row]) {
            RectInstance *nb = realloc(c->bg_rows[row], (usize)snug * sizeof(RectInstance));
            if (nb) { c->bg_rows[row] = nb; c->bg_caps[row] = snug; }
        }
    }
    i32 gnv = c->glyph_counts[row];
    if (c->glyph_caps[row] >= 64 && c->glyph_caps[row] >= gnv * 4 + 4) {
        i32 snug = 16; while (snug < gnv + 1) snug *= 2;
        if (snug < c->glyph_caps[row]) {
            GlyphInstance *ng = realloc(c->glyph_rows[row], (usize)snug * sizeof(GlyphInstance));
            if (ng) { c->glyph_rows[row] = ng; c->glyph_caps[row] = snug; }
        }
    }
}

static bool term_render_cache_ensure(TermRenderCache *c,
                                     i32 rows, i32 cols,
                                     f32 cw, f32 ch,
                                     f32 origin_x, f32 origin_y) {
    bool geom_same = (c->cached_rows == rows && c->cached_cols == cols);
    if (!geom_same) {
        term_render_cache_free(c);
        c->bg_rows      = calloc((usize)rows, sizeof(RectInstance *));
        c->bg_counts    = calloc((usize)rows, sizeof(i32));
        c->bg_caps      = calloc((usize)rows, sizeof(i32));
        c->glyph_rows   = calloc((usize)rows, sizeof(GlyphInstance *));
        c->glyph_counts = calloc((usize)rows, sizeof(i32));
        c->glyph_caps   = calloc((usize)rows, sizeof(i32));
        if (!c->bg_rows || !c->bg_counts || !c->bg_caps ||
            !c->glyph_rows || !c->glyph_counts || !c->glyph_caps) {
            /* OOM: release whatever succeeded and present an empty but valid
             * cache (cached_rows == 0) so the caller skips the per-row loop
             * instead of dereferencing a NULL array. */
            free(c->bg_rows);    free(c->bg_counts);    free(c->bg_caps);
            free(c->glyph_rows); free(c->glyph_counts); free(c->glyph_caps);
            c->bg_rows = NULL;    c->bg_counts = NULL;    c->bg_caps = NULL;
            c->glyph_rows = NULL; c->glyph_counts = NULL; c->glyph_caps = NULL;
            c->cached_rows = 0;
            c->cached_cols = 0;
            return false;
        }
        c->cached_rows  = rows;
        c->cached_cols  = cols;
        c->all_rows_dirty = true;
    }
    if (c->cached_cw != cw || c->cached_ch != ch ||
        c->cached_origin_x != origin_x || c->cached_origin_y != origin_y) {
        c->cached_cw = cw; c->cached_ch = ch;
        c->cached_origin_x = origin_x;
        c->cached_origin_y = origin_y;
        c->all_rows_dirty = true;
    }
    return true;
}

/* Rebuild a single row's bg + glyph cache entries from the terminal cells.
 * Mirrors the color/attr logic that used to live inline in pass 1 & 2.
 * When ligatures are enabled, scans ahead to detect multi-char ligature
 * sequences and emits a single wide glyph instance spanning N cells. */
static void term_render_cache_build_row(TermRenderCache *c, AppState *app,
                                        Terminal *term, i32 row,
                                        f32 origin_x, f32 origin_y,
                                        f32 cw, f32 ch) {
    i32 bg_n = 0, gn = 0;
    RectInstance  *bg_row = c->bg_rows[row];
    GlyphInstance *gl_row = c->glyph_rows[row];
    f32 y = origin_y + (f32)row * ch;
    f32 y_px = floorf(y);
    f32 ch_px = ceilf(ch);
    f32 cw_px = ceilf(cw);
    bool ligatures = app->config.enable_ligatures;

    /* Resolve visible row -> scrollback or primary grid based on scroll_offset */
    i32 row_len = 0;
    const Cell *row_cells = terminal_visible_row(term, row, &row_len);

    /* BiDi: if enabled and row has RTL content, produce visual-order cells.
     * We use a stack buffer (BIDI_MAX_COLS = 1024) to avoid allocation. */
    Cell bidi_visual[1024];
    Cell blank_cell = { .codepoint = ' ', .attr = term->cursor_attr };
    bool use_bidi = term->bidi_enabled &&
                    term->cols <= 1024 &&
                    row_cells && row_len >= term->cols &&
                    bidi_line_has_rtl(row_cells, term->cols);
    if (use_bidi) {
        bidi_reorder_line(row_cells, term->cols, bidi_visual, NULL);
        bidi_shape_arabic(bidi_visual, term->cols);
    }

    for (i32 col = 0; col < term->cols; col++) {
        const Cell *cell;
        if (use_bidi) {
            cell = &bidi_visual[col];
        } else if (row_cells && col < row_len) {
            cell = &row_cells[col];
        } else {
            cell = &blank_cell;
        }
        u32 fg_val = cell->attr.fg;
        u32 bg_val = cell->attr.bg;
        bool has_inverse = (cell->attr.flags & ATTR_INVERSE) != 0;
        f32 x = origin_x + (f32)col * cw;
        f32 x_px = floorf(x);

        /* --- bg rect (color_resolve LUT) --- */
        bool emit_bg = has_inverse ||
                       IS_TRUECOLOR(bg_val) || bg_val != BG_DEFAULT;
        if (emit_bg) {
            Color bg = has_inverse
                ? color_resolve(fg_val, FG_DEFAULT)
                : color_resolve(bg_val, BG_DEFAULT);
            /* When a background image is active, reduce cell bg opacity
             * so the wallpaper shows through. */
            if (app->renderer.bg_texture) {
                bg.a *= (1.0f - app->renderer.bg_opacity);
            }
            /* When window opacity < 1.0, reduce cell bg alpha so the
             * desktop shows through. Overlays draw their own opaque rects. */
            if (app->config.opacity < 1.0f) {
                bg.a *= app->config.opacity;
            }
            bg = color_linear_from_srgb(bg);
            if (!term_render_cache_reserve_rects(c, row, bg_n + 1)) continue;
            bg_row = c->bg_rows[row];
            RectInstance *ri = &bg_row[bg_n++];
            ri->x = x_px; ri->y = y_px;
            ri->w = cw_px; ri->h = ch_px;
            ri->r = bg.r; ri->g = bg.g; ri->b = bg.b; ri->a = bg.a;
        }

        /* --- glyph (color_resolve LUT) --- */
        if (cell->codepoint > 32 && !(cell->attr.flags & ATTR_WDUMMY)) {
            Color fg = has_inverse
                ? color_resolve(bg_val, BG_DEFAULT)
                : color_resolve(fg_val, FG_DEFAULT);
            if ((cell->attr.flags & ATTR_BOLD) && app->config.bold_is_bright &&
                !IS_TRUECOLOR(fg_val) && fg_val < 8 && !has_inverse) {
                fg = g_ansi_colors[fg_val + 8];
            }
            if (cell->attr.flags & ATTR_DIM) {
                /* sRGB-space fudge factor: multiplying sRGB by 0.797 is
                 * perceptually equivalent to multiplying linear by 0.6.
                 * The conversion to linear happens at the CPU → GPU boundary
                 * (see renderer.h); keeping the fudge here avoids a
                 * round-trip srgb→lin→*0.6→srgb per cell. */
                fg.r *= 0.797f; fg.g *= 0.797f; fg.b *= 0.797f;
            }

            /* --- Ligature lookahead ---
             * Only attempt ligatures for printable ASCII chars that commonly
             * form programming ligatures. Skip if cell has special attrs
             * (wide, bold+italic combos that might break shaping).
             * Try longest match first (4, 3, 2 chars). */
            i32 lig_skip = 0;
            if (ligatures && cell->codepoint >= 33 && cell->codepoint < 127) {
                i32 max_look = 4;
                if (col + max_look > term->cols) max_look = term->cols - col;
                for (i32 try_len = max_look; try_len >= 2; try_len--) {
                    /* Gather codepoints for the run, verify all are same
                     * fg/bg/attr (ligature should only apply to uniform runs) */
                    u32 run_cps[4];
                    bool uniform = true;
                    for (i32 k = 0; k < try_len; k++) {
                        Cell *nc = terminal_cell_at(term, col + k, row);
                        if (!nc || nc->codepoint < 33 || nc->codepoint >= 127 ||
                            (nc->attr.flags & (ATTR_WDUMMY | ATTR_WIDE))) {
                            uniform = false; break;
                        }
                        /* Ensure all cells share the same foreground */
                        if (k > 0 && (nc->attr.fg != fg_val ||
                                      nc->attr.bg != bg_val)) {
                            uniform = false; break;
                        }
                        run_cps[k] = nc->codepoint;
                    }
                    if (!uniform) continue;

                    f32 lu0, lv0, lu1, lv1;
                    i32 lig_w = font_check_ligature(&app->renderer.font,
                                                     run_cps, try_len,
                                                     &lu0, &lv0, &lu1, &lv1);
                    if (lig_w > 0) {
                        /* Emit ONE wide glyph instance for the ligature */
                        if (!term_render_cache_reserve_glyphs(c, row, gn + 1)) break;
                        gl_row = c->glyph_rows[row];
                        GlyphInstance *gi = &gl_row[gn++];
                        gi->x = x_px;
                        gi->y = y_px;
                        gi->u0 = lu0; gi->v0 = lv0;
                        gi->u1 = lu1; gi->v1 = lv1;
                        Color lin_fg = color_linear_from_srgb(fg);
                        gi->r = lin_fg.r; gi->g = lin_fg.g; gi->b = lin_fg.b;
                        gi->is_color = 0.0f;
                        gi->w_cells = (f32)lig_w;
                        gi->a = 1.0f;
                        lig_skip = lig_w - 1;
                        break;
                    }
                }
            }

            if (lig_skip > 0) {
                /* Skip the remaining cells of the ligature (bg already emitted
                 * individually above; continue the loop to process bg for the
                 * next cells but the glyph part was handled as the wide glyph). */
                for (i32 sk = 1; sk <= lig_skip && (col + sk) < term->cols; sk++) {
                    Cell *sc = terminal_cell_at(term, col + sk, row);
                    if (!sc) continue;
                    u32 s_fg = sc->attr.fg;
                    u32 s_bg = sc->attr.bg;
                    bool s_inv = (sc->attr.flags & ATTR_INVERSE) != 0;
                    f32 sx = origin_x + (f32)(col + sk) * cw;
                    f32 sx_px = floorf(sx);
                    bool s_emit_bg = s_inv ||
                                     IS_TRUECOLOR(s_bg) || s_bg != BG_DEFAULT;
                    if (s_emit_bg) {
                        Color sbg = s_inv
                            ? color_resolve(s_fg, FG_DEFAULT)
                            : color_resolve(s_bg, BG_DEFAULT);
                        if (app->renderer.bg_texture) {
                            sbg.a *= (1.0f - app->renderer.bg_opacity);
                        }
                        if (app->config.opacity < 1.0f) {
                            sbg.a *= app->config.opacity;
                        }
                        sbg = color_linear_from_srgb(sbg);
                        if (!term_render_cache_reserve_rects(c, row, bg_n + 1)) continue;
                        bg_row = c->bg_rows[row];
                        RectInstance *ri = &bg_row[bg_n++];
                        ri->x = sx_px; ri->y = y_px;
                        ri->w = cw_px; ri->h = ch_px;
                        ri->r = sbg.r; ri->g = sbg.g; ri->b = sbg.b; ri->a = sbg.a;
                    }
                }
                col += lig_skip; /* skip processed cells */
                continue;
            }

            /* Normal single-glyph path */
            f32 u0, v0, u1, v1;
            bool got_glyph = font_get_glyph_uv(&app->renderer.font, cell->codepoint,
                                               &u0, &v0, &u1, &v1);
            if (got_glyph) {
                if (!term_render_cache_reserve_glyphs(c, row, gn + 1)) continue;
                gl_row = c->glyph_rows[row];
                GlyphInstance *gi = &gl_row[gn++];
                gi->x = x_px;
                gi->y = y_px;
#ifdef USE_METAL
                bool is_color = (u0 < 0);
                gi->is_color = is_color ? 1.0f : 0.0f;
                gi->u0 = is_color ? -u0 : u0;
                gi->v0 = v0;
                gi->u1 = is_color ? -u1 : u1;
                gi->v1 = v1;
#else
                gi->u0 = u0; gi->v0 = v0;
                gi->u1 = u1; gi->v1 = v1;
                gi->is_color = 0.0f;
#endif
                Color lin_fg = color_linear_from_srgb(fg);
                gi->r = lin_fg.r; gi->g = lin_fg.g; gi->b = lin_fg.b;
                gi->w_cells = gi->is_color > 0.5f ? 2.0f : 1.0f;
                gi->a = 1.0f;
            }
        }
    }
    c->bg_counts[row]    = bg_n;
    c->glyph_counts[row] = gn;
}

/* Render a single terminal pane at given origin */
/* Active-pane indicator: no border anymore. Active = unchanged; inactive
 * gets a tiny black overlay so its content reads slightly desaturated /
 * dimmer (~10% reduction) without losing legibility. The strength comes
 * from style.active_pane_indicator_color's alpha when the user has set
 * one; otherwise we use the default 10% black wash. */
static void render_pane_focus_indicator(AppState *app, f32 x, f32 y, f32 w, f32 h,
                                        bool focused) {
    if (focused) return;
    if (w < 2.0f || h < 2.0f) return;

    Color dim = app->config.style.active_pane_indicator_color;
    if (dim.a <= 0.001f) {
        /* Default: 10% pure-black wash. Slightly stronger than the 5%
         * default so the inactive pane reads as visibly secondary
         * without the focused pane needing extra glow on top. */
        dim = (Color){0.0f, 0.0f, 0.0f, 0.10f};
    }
    dim.a *= app->config.opacity;

    Renderer *r = &app->renderer;
    renderer_draw_rect(r, x, y, w, h, dim);
}

void render_terminal_pane(AppState *app, Terminal *term, TermRenderCache *cache,
                          f32 origin_x, f32 origin_y, bool focused) {
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme;
    if (!term) return;

    /* Sync terminal's mutable palette into the global color LUT.
     * The per-terminal palette is lazy: NULL == "use built-in defaults"
     * which the global g_ansi_colors LUT already mirrors at startup, so
     * this work only runs when OSC 4 has actually hit. */
    if (term->palette_modified && term->palette) {
        const f32 inv = 1.0f / 255.0f;
        for (i32 i = 0; i < 256; i++) {
            u32 v = term->palette[i];
            g_ansi_colors[i] = (Color){
                (f32)((v >> 16) & 0xFF) * inv,
                (f32)((v >> 8)  & 0xFF) * inv,
                (f32)( v        & 0xFF) * inv,
                1.0f
            };
        }
        term->palette_modified = false;
        cache->all_rows_dirty = true;
    }

    f32 cw = r->font.cell_width;
    f32 ch = r->font.cell_height;
    (void)focused;

    /* Keep the terminal's cell-pixel hint in sync so inline-image sizing /
     * row reservation (terminal_place_image_inline) uses real font metrics. */
    terminal_set_cell_pixels(term, (i32)(cw + 0.5f), (i32)(ch + 0.5f));

    /* resize/validate cache — forces full rebuild on geometry change.
     * On allocation failure the cache is left empty (cached_rows == 0); skip
     * the cell passes for this frame rather than dereferencing NULL row arrays.
     * Decorations (pass 3) read term->cells directly and still run. */
    bool cache_ok = term_render_cache_ensure(cache, term->rows, term->cols, cw, ch, origin_x, origin_y);

    /*
     * Multi-pass terminal rendering:
     *
     * Pass 0: glClear already filled bg (done in renderer_begin_frame)
     * Pass 1: Cell backgrounds — only non-default bg cells as rects
     * Pass 2: Cell glyphs — all visible glyphs in one instanced draw
     * Pass 3: Decorations — underline, strikethrough
     */

    /* GPU compute pipeline is built and ready (renderer_compute_terminal)
     * but requires indirect draw commands for correct GPU→GPU counter passing.
     * Using CPU multi-pass rendering until indirect draw is implemented. */

    /* === PASS 1 + 2 (merged cache): Cell backgrounds + glyphs ===
     * rebuild only dirty rows; replay cached rows via bulk memcpy
     * into the renderer batch.  Dirty rows come from term->dirty_rows bitmap
     * set by vt_parser / buffer code as cells are modified. */
    if (cache_ok) {
        bool full_rebuild = cache->all_rows_dirty;
        for (i32 row = 0; row < term->rows; row++) {
            if (full_rebuild || term_row_dirty(term, row)) {
                term_render_cache_build_row(cache, app, term, row,
                                            origin_x, origin_y, cw, ch);
                /* Occasional full rebuild → reclaim any row buffer left
                 * oversized by a previously-wide line (thrash-safe here). */
                if (full_rebuild) term_render_cache_shrink_row(cache, row);
            }
        }
        cache->all_rows_dirty = false;

        /* Emit all bg rects */
        for (i32 row = 0; row < term->rows; row++) {
            i32 n = cache->bg_counts[row];
            RectInstance *bg_row = cache->bg_rows[row];
            if (n > 0 && bg_row) {
                renderer_append_rects(r, bg_row, (u32)n);
            }
        }
        renderer_flush_rects(r); /* before glyphs */

        /* Emit all glyphs */
        for (i32 row = 0; row < term->rows; row++) {
            i32 n = cache->glyph_counts[row];
            GlyphInstance *gl_row = cache->glyph_rows[row];
            if (n > 0 && gl_row) {
                renderer_append_glyphs(r, gl_row, (u32)n);
            }
        }
        renderer_flush_glyphs(r);
    }

    /* === PASS 3: Decorations (underline, strikethrough, overline) === */
    for (i32 row = 0; row < term->rows; row++) {
        /* Resolve the visible row (scroll-aware) so decorations match the
         * glyphs emitted in passes 1+2. terminal_cell_at and the sparse
         * underline map are keyed to the LIVE grid; with scroll_offset > 0 a
         * visible row maps to live-grid index `logical = row - scroll_offset`,
         * and rows scrolled in from scrollback (logical < 0) retain no sparse
         * underline style/color. Reading the live grid here drew decorations
         * for the wrong rows whenever the view was scrolled back. */
        i32 vlen = 0;
        const Cell *vrow = terminal_visible_row(term, row, &vlen);
        if (!vrow || vlen <= 0) continue;
        i32 logical = row - term->scroll_offset;

        /* BiDi: compute logical-to-visual mapping for decoration positions */
        i32 deco_l2v[1024];
        bool deco_bidi = false;
        if (term->bidi_enabled && term->cols <= 1024 && vlen >= term->cols) {
            if (bidi_line_has_rtl(vrow, term->cols)) {
                Cell deco_tmp[1024];
                bidi_reorder_line(vrow, term->cols, deco_tmp, deco_l2v);
                deco_bidi = true;
            }
        }
        for (i32 col = 0; col < term->cols && col < vlen; col++) {
            const Cell *cell = &vrow[col];
            if (!(cell->attr.flags & (ATTR_UNDERLINE | ATTR_STRIKETHROUGH | ATTR_OVERLINE))) continue;

            /* Underline color/style: the sparse map only covers visible
             * live-grid rows; scrollback rows fall back to a single underline. */
            Color ul_clr;
            u32 ul_val = 0;
            u8 ul_style_val = 0;
            if (logical >= 0) {
                terminal_get_underline(term, col, logical, &ul_val, &ul_style_val);
            } else if (cell->attr.flags & ATTR_UNDERLINE) {
                ul_style_val = 1; /* default single underline */
            }
            if (ul_val != 0) {
                ul_clr = color_resolve(ul_val, FG_DEFAULT);
            } else {
                ul_clr = color_resolve(cell->attr.fg, FG_DEFAULT);
            }

            i32 vis_col = deco_bidi ? deco_l2v[col] : col;
            f32 x = origin_x + vis_col * cw;
            f32 y = origin_y + row * ch;

            if (cell->attr.flags & ATTR_UNDERLINE) {
                u8 style = ul_style_val;
                f32 uy = y + ch - 2.0f;

                switch (style) {
                case 0: break; /* none */
                case 1: /* single */
                    renderer_draw_rect(r, x, uy, cw, 1, ul_clr);
                    break;
                case 2: /* double */
                    renderer_draw_rect(r, x, uy - 2, cw, 1, ul_clr);
                    renderer_draw_rect(r, x, uy, cw, 1, ul_clr);
                    break;
                case 3: { /* curly/undercurl — sine wave approximation */
                    f32 amp = 2.0f;
                    f32 segments = 6.0f;
                    f32 seg_w = cw / segments;
                    for (f32 si = 0; si < segments; si++) {
                        f32 sx = x + si * seg_w;
                        f32 t1 = si / segments;
                        f32 t2 = (si + 1) / segments;
                        f32 y1 = uy + sinf(t1 * 3.14159f * 2) * amp;
                        f32 y2 = uy + sinf(t2 * 3.14159f * 2) * amp;
                        f32 min_y = y1 < y2 ? y1 : y2;
                        f32 max_y = y1 > y2 ? y1 : y2;
                        renderer_draw_rect(r, sx, min_y, seg_w, max_y - min_y + 1, ul_clr);
                    }
                    break;
                }
                case 4: /* dotted */
                    for (f32 dx = 0; dx < cw; dx += 4)
                        renderer_draw_rect(r, x + dx, uy, 2, 1, ul_clr);
                    break;
                case 5: /* dashed */
                    for (f32 dx = 0; dx < cw; dx += 6)
                        renderer_draw_rect(r, x + dx, uy, 4, 1, ul_clr);
                    break;
                default: /* fallback single */
                    renderer_draw_rect(r, x, uy, cw, 1, ul_clr);
                    break;
                }
            }
            if (cell->attr.flags & ATTR_STRIKETHROUGH)
                renderer_draw_rect(r, x, y + ch * 0.5f, cw, 1, ul_clr);
            if (cell->attr.flags & ATTR_OVERLINE)
                renderer_draw_rect(r, x, y, cw, 1, ul_clr);
        }
    }

    /* === Inline images (iTerm2 / Sixel / Liu / detected agent paths) ===
     * Images pin to an ABSOLUTE scrollback line (term->images[ii].abs_line) so
     * they ride the text up/down as it scrolls and into scrollback. The screen
     * row of an anchored line is:
     *     vis_row = (abs_line - sb_abs_base - sb_count) + scroll_offset
     * (the same identity terminal_visible_row() inverts). abs_line < 0 marks an
     * alt-screen image, which never scrolls back, so it keeps its fixed `row`.
     * Everything is scissored to the pane so a tall image can't bleed into the
     * status bar or a neighbouring split. */
    if (term->images) {
        f32 pane_w = (f32)term->cols * cw;
        f32 pane_h = (f32)term->rows * ch;
        renderer_push_scissor(r, origin_x, origin_y, pane_w, pane_h);
        for (i32 ii = 0; ii < MAX_TERM_IMAGES; ii++) {
            if (!term->images[ii].valid || !term->images[ii].pixels) continue;

            i64 abs_line = term->images[ii].abs_line;
            /* Evicted past the top of scrollback — its text is gone, so is it.
             * Zero the cached hit-rect before skipping: the slot isn't freed on
             * eviction, so a stale non-zero scr_w/scr_h would let a click on the
             * now-vacated cells re-open an image that's no longer on screen. */
            if (abs_line >= 0 && abs_line < term->sb_abs_base) {
                term->images[ii].scr_w = 0.0f;
                term->images[ii].scr_h = 0.0f;
                continue;
            }

            i32 vis_row = (abs_line >= 0)
                ? (i32)(abs_line - term->sb_abs_base - (i64)term->sb_count) + term->scroll_offset
                : term->images[ii].row;

            i32 img_col = term->images[ii].col;
            i32 img_w = term->images[ii].width;
            i32 img_h = term->images[ii].height;

            /* Display size: cell-based dims if specified, else native pixels. */
            f32 draw_w = (term->images[ii].cols > 0)
                ? (f32)term->images[ii].cols * cw : (f32)img_w;
            f32 draw_h = (term->images[ii].rows > 0)
                ? (f32)term->images[ii].rows * ch : (f32)img_h;

            f32 img_x = origin_x + (f32)img_col * cw;
            f32 img_y = origin_y + (f32)vis_row * ch;

            /* Cache the on-screen rect so the click handler can hit-test
             * clickable thumbnails without redoing this scroll/split math.
             * Stored before the cull so an off-screen slot reads as zero-area
             * (its rect is overwritten next time it's visible). */
            term->images[ii].scr_x = img_x;
            term->images[ii].scr_y = img_y;
            term->images[ii].scr_w = draw_w;
            term->images[ii].scr_h = draw_h;

            /* Cull fully off-pane (scissor would clip them anyway, but this
             * skips the draw-call + texture bind entirely). */
            if (img_y + draw_h <= origin_y || img_y >= origin_y + pane_h) continue;

            renderer_draw_image_cached(r, &term->images[ii], term->images[ii].serial,
                                       term->images[ii].pixels, img_w, img_h,
                                       img_x, img_y, draw_w, draw_h);

            /* Agent-preview thumbnails get a subtle frame + an "expand" glyph
             * in the corner so it reads as a clickable preview rather than a
             * cropped image. Protocol images (Sixel/iTerm2/Liu) skip this. */
            if (term->images[ii].clickable) {
                Color frame = (Color){ t->fg.r, t->fg.g, t->fg.b, 0.22f };
                renderer_draw_rect(r, img_x, img_y, draw_w, 1.0f, frame);
                renderer_draw_rect(r, img_x, img_y + draw_h - 1.0f, draw_w, 1.0f, frame);
                renderer_draw_rect(r, img_x, img_y, 1.0f, draw_h, frame);
                renderer_draw_rect(r, img_x + draw_w - 1.0f, img_y, 1.0f, draw_h, frame);

                /* Expand badge: a small translucent chip bottom-right with a
                 * diagonal-arrows hint, signalling click-to-open. */
                f32 badge = ch * 0.9f;
                if (badge > draw_w * 0.5f) badge = draw_w * 0.5f;
                if (badge > draw_h * 0.5f) badge = draw_h * 0.5f;
                if (badge >= 6.0f) {
                    f32 bx = img_x + draw_w - badge - 3.0f;
                    f32 by = img_y + draw_h - badge - 3.0f;
                    Color chip = (Color){ 0.0f, 0.0f, 0.0f, 0.45f };
                    renderer_draw_rect(r, bx, by, badge, badge, chip);
                    Color ic = (Color){ 1.0f, 1.0f, 1.0f, 0.85f };
                    f32 m = badge * 0.28f;        /* glyph inset */
                    f32 lx0 = bx + m, ly0 = by + m;
                    f32 lx1 = bx + badge - m, ly1 = by + badge - m;
                    f32 tk = 1.5f;
                    /* Diagonal line corner-to-corner. */
                    i32 steps = (i32)(badge);
                    for (i32 s = 0; s <= steps; s++) {
                        f32 tt = steps > 0 ? (f32)s / (f32)steps : 0.0f;
                        renderer_draw_rect(r, lx0 + (lx1 - lx0) * tt,
                                           ly0 + (ly1 - ly0) * tt, tk, tk, ic);
                    }
                    /* Arrowheads at both ends. */
                    renderer_draw_rect(r, lx0, ly0, badge * 0.30f, tk, ic);
                    renderer_draw_rect(r, lx0, ly0, tk, badge * 0.30f, ic);
                    renderer_draw_rect(r, lx1 - badge * 0.30f, ly1 - tk, badge * 0.30f, tk, ic);
                    renderer_draw_rect(r, lx1 - tk, ly1 - badge * 0.30f, tk, badge * 0.30f, ic);
                }
            }
        }
        renderer_pop_scissor(r);
    }

    /* Selection highlight — when BiDi is enabled, selection coordinates
     * are logical but visual positions may differ.  We compute the
     * logical-to-visual map per row and highlight visual positions.
     *
     * Text-bearing cells get the normal selection wash; cells that are
     * empty (space / NUL) get a much fainter version so the user can
     * see the active range without painting a giant gray block over
     * blank screen real estate when the drag extends past content
     * (which was reading as "huge dark rectangle" overlap). The copy
     * range is unchanged — selection_get_text still includes blanks. */
    if (selection_active(term)) {
        Color sel_text  = (Color){t->selection.r, t->selection.g, t->selection.b,
                                  t->selection.a * 0.40f};
        Color sel_blank = (Color){t->selection.r, t->selection.g, t->selection.b,
                                  t->selection.a * 0.10f};
        for (i32 row = 0; row < term->rows; row++) {
            /* BiDi mapping for this row */
            i32 bidi_l2v[1024];
            bool bidi_row = false;
            const Cell *row_cells = &term->cells[row * term->cols];
            if (term->bidi_enabled && term->cols <= 1024) {
                if (bidi_line_has_rtl(row_cells, term->cols)) {
                    Cell bidi_tmp[1024];
                    bidi_reorder_line(row_cells, term->cols, bidi_tmp, bidi_l2v);
                    bidi_row = true;
                }
            }
            for (i32 col = 0; col < term->cols; col++) {
                if (selection_contains(term, col, row)) {
                    i32 vis_col = bidi_row ? bidi_l2v[col] : col;
                    f32 x = origin_x + vis_col * cw;
                    f32 y = origin_y + row * ch;
                    u32 cp = row_cells[col].codepoint;
                    bool empty = (cp == 0 || cp == ' ');
                    renderer_draw_rect(r, x, y, cw, ch, empty ? sel_blank : sel_text);
                }
            }
        }
    }

    /* URL hover highlight (Cmd+hover underline) */
    if (app->url_hover_active && app->hover_url.url[0]) {
        /* Blue-ish link underline color */
        Color url_clr = (Color){ 0.4f, 0.6f, 1.0f, 0.9f };
        for (i32 row = app->hover_url.start_row; row <= app->hover_url.end_row && row < term->rows; row++) {
            i32 sc = (row == app->hover_url.start_row) ? app->hover_url.start_col : 0;
            i32 ec = (row == app->hover_url.end_row) ? app->hover_url.end_col : term->cols - 1;
            for (i32 col = sc; col <= ec && col < term->cols; col++) {
                f32 x = origin_x + col * cw;
                f32 y = origin_y + row * ch;
                /* Draw underline */
                renderer_draw_rect(r, x, y + ch - 2.0f, cw, 1, url_clr);
            }
        }
    }

    /* Cursor (with smooth animation + blink fade) — BiDi-aware.
     *
     * Inactive panes in a split don't get a typing caret at all so the
     * focused pane is the only one drawing a cursor. `focused == true`
     * is also passed for single-pane tabs (see SPLIT_NONE caller) so
     * this gating is split-only in practice. */
    if (focused && term->cursor_visible && term->scroll_offset == 0) {
        i32 visual_cx = term->cursor_x;
        if (term->bidi_enabled && term->cols <= 1024) {
            const Cell *crow = &term->cells[term->cursor_y * term->cols];
            if (bidi_line_has_rtl(crow, term->cols)) {
                Cell bidi_tmp[1024];
                i32  bidi_l2v[1024];
                bidi_reorder_line(crow, term->cols, bidi_tmp, bidi_l2v);
                if (term->cursor_x >= 0 && term->cursor_x < term->cols)
                    visual_cx = bidi_l2v[term->cursor_x];
            }
        }
        f32 target_x = origin_x + visual_cx * cw;
        f32 target_y = origin_y + term->cursor_y * ch;
        f32 cx, cy;

        if (app->config.cursor_animate && focused) {
            /* If focus moved to a different terminal (pane switch / tab
             * switch), the previous AppState animation snapshot points
             * at the OLD terminal's cursor position. Snap to the new
             * position instead of animating across the divider. */
            if (app->cursor_anim_term != term) {
                app->cursor_anim_x      = target_x;
                app->cursor_anim_y      = target_y;
                app->cursor_anim_from_x = target_x;
                app->cursor_anim_from_y = target_y;
                app->cursor_target_x    = target_x;
                app->cursor_target_y    = target_y;
                app->cursor_animating   = false;
                app->cursor_anim_term   = term;
            }
            /* Detect target change and (re)start animation. We snapshot
             * the current animated position as `from` so a target change
             * mid-animation reroutes smoothly from wherever we are
             * instead of restarting from the previous target. */
            if (target_x != app->cursor_target_x || target_y != app->cursor_target_y) {
                bool placed = (app->cursor_target_x != 0 || app->cursor_target_y != 0);
                if (!placed) {
                    /* First placement: nothing to glide from — snap. */
                    app->cursor_anim_x = target_x;
                    app->cursor_anim_y = target_y;
                    app->cursor_anim_from_x = target_x;
                    app->cursor_anim_from_y = target_y;
                    app->cursor_animating   = false;
                } else {
                    /* Every move glides — including a single typed column. We
                     * start from wherever the caret is drawn right now, so fast
                     * typing reroutes the glide mid-flight (the caret trails the
                     * real cell by a fraction instead of teleporting). The
                     * duration scales with distance (below) so a one-cell step
                     * is a quick ~60ms slide while a line/vim jump glides over
                     * ~160ms — motion stays uniform with no snap/glide split. */
                    app->cursor_anim_from_x = app->cursor_anim_x;
                    app->cursor_anim_from_y = app->cursor_anim_y;
                    app->cursor_anim_start  = platform_time_sec();
                    app->cursor_animating   = true;
                }
                app->cursor_target_x = target_x;
                app->cursor_target_y = target_y;
            }

            if (app->cursor_animating) {
                /* Closed-form interpolation: pos = lerp(from, to, eased(t)).
                 * Direct lerp (vs the old `pos += (target-pos)*ease`
                 * exponential decay) is smooth regardless of when frames land.
                 *
                 * Distance-proportional duration: a one-cell typing step glides
                 * in ~60ms (snappy, no perceptible lag) and the duration grows
                 * with the move length, capped at 160ms so even a full-screen
                 * jump lands quickly. from/target are fixed for the life of an
                 * animation, so recomputing each frame yields the same value. */
                f32 dxc = (app->cursor_target_x - app->cursor_anim_from_x) / cw;
                f32 dyc = (app->cursor_target_y - app->cursor_anim_from_y) / ch;
                f32 dist = sqrtf(dxc * dxc + dyc * dyc);
                f64 dur = 0.05 + 0.010 * (f64)dist;
                if (dur > 0.16) dur = 0.16;
                f64 anim_elapsed = platform_time_sec() - app->cursor_anim_start;
                f32 t = (dur > 0.0) ? (f32)(anim_elapsed / dur) : 1.0f;
                if (t >= 1.0f) {
                    t = 1.0f;
                    app->cursor_animating = false;
                }
                /* EASE_OUT_CUBIC: 1 - (1-t)^3, gentler landing than quad */
                f32 inv = 1.0f - t;
                f32 e   = 1.0f - inv * inv * inv;
                app->cursor_anim_x = app->cursor_anim_from_x +
                    (app->cursor_target_x - app->cursor_anim_from_x) * e;
                app->cursor_anim_y = app->cursor_anim_from_y +
                    (app->cursor_target_y - app->cursor_anim_from_y) * e;
                if (!app->cursor_animating) {
                    app->cursor_anim_x = app->cursor_target_x;
                    app->cursor_anim_y = app->cursor_target_y;
                }
            }
            cx = app->cursor_anim_x;
            cy = app->cursor_anim_y;
        } else {
            /* No animation — snap directly */
            cx = target_x;
            cy = target_y;
            if (focused) {
                app->cursor_anim_x = target_x;
                app->cursor_anim_y = target_y;
                app->cursor_anim_from_x = target_x;
                app->cursor_anim_from_y = target_y;
                app->cursor_target_x = target_x;
                app->cursor_target_y = target_y;
            }
        }

        /* Cursor blink — smooth sine pulse with a high floor so the
         * cursor is always legible while still visibly "breathing".
         * Replaces the previous discrete 1.0 / 0.15 square wave that
         * read as flicker. Period 1.1s, alpha range 0.55..1.0. */
        f32 blink_alpha = 1.0f;
        if (app->config.cursor_blink && focused) {
            f64 bnow = platform_time_sec();
            f32 phase = (f32)fmod(bnow * (2.0 * 3.14159265 / 1.1), 2.0 * 3.14159265);
            f32 wave  = 0.5f + 0.5f * sinf(phase);  /* 0..1 */
            blink_alpha = 0.55f + 0.45f * wave;
        }

        Color cursor_clr = (Color){t->cursor.r, t->cursor.g, t->cursor.b, 0.7f * blink_alpha};
        switch (app->config.cursor_style) {
        case 0: /* Block */
            renderer_draw_rect(r, cx, cy, cw, ch, cursor_clr);
            break;
        case 1: /* Underline */
            renderer_draw_rect(r, cx, cy + ch - 2, cw, 2,
                (Color){t->cursor.r, t->cursor.g, t->cursor.b, blink_alpha});
            break;
        case 2: /* Bar */
            renderer_draw_rect(r, cx, cy, 2, ch,
                (Color){t->cursor.r, t->cursor.g, t->cursor.b, blink_alpha});
            break;
        }
    }

    /* Semantic zone indicators — colored left margin bars (3px) */
    {
        f32 bar_w = 3.0f * app->dpi_scale;
        f32 bar_x = origin_x - bar_w - 1.0f;
        Color prompt_clr = (Color){0.3f, 0.6f, 1.0f, 0.7f};  /* blue */
        Color output_clr = (Color){0.4f, 0.8f, 0.4f, 0.5f};  /* green */
        Color fail_clr   = (Color){0.9f, 0.25f, 0.25f, 0.8f}; /* red */

        for (i32 row = 0; row < term->rows; row++) {
            f32 ry = origin_y + row * ch;
            bool is_prompt = (row >= term->prompt_start_row && row < term->input_start_row
                              && term->prompt_start_row >= 0 && term->input_start_row > term->prompt_start_row);
            bool is_output = (row >= term->output_start_row && term->output_start_row > 0);

            if (is_prompt) {
                Color clr = (term->last_cmd_failed) ? fail_clr : prompt_clr;
                renderer_draw_rect(r, bar_x, ry, bar_w, ch, clr);

                /* Exit code badge on prompt start row */
                if (term->last_cmd_failed && row == term->prompt_start_row) {
                    char ec_buf[8];
                    snprintf(ec_buf, sizeof(ec_buf), "%d", term->last_exit_code);
                    f32 ec_cw = cw * 0.75f;
                    f32 ec_w = (f32)strlen(ec_buf) * ec_cw + 4.0f * app->dpi_scale;
                    f32 ec_h = ch * 0.8f;
                    f32 ec_x = bar_x - ec_w - 1.0f;
                    f32 ec_y = ry + (ch - ec_h) / 2.0f;
                    /* Red badge background */
                    renderer_draw_rect(r, ec_x, ec_y, ec_w, ec_h, fail_clr);
                    /* Exit code text */
                    f32 text_x = ec_x + 2.0f * app->dpi_scale;
                    f32 text_y = ry + (ch - r->font.cell_height) / 2.0f;
                    draw_text_ex(r, ec_buf, text_x, text_y,
                        (Color){1.0f, 1.0f, 1.0f, 1.0f}, 4, ec_cw);
                }
            } else if (is_output) {
                renderer_draw_rect(r, bar_x, ry, bar_w, ch, output_clr);
            }
        }
    }

    /* Scrollbar */
    render_scrollbar(app, term, origin_x, origin_y, ch * term->rows);

    /* Inline autosuggest ghost text — drawn at the typing cursor in
     * dimmed foreground so the still-untyped suffix follows the cursor
     * naturally. Skipped for inactive panes / when the suggestion was
     * computed for a different terminal. The text is drawn as visible
     * cells; if the suffix would overflow the row it gets clipped. */
    if (focused &&
        app->autosuggest_visible &&
        app->autosuggest_target_term == term &&
        app->autosuggest_suffix[0] &&
        /* Fail-safe guards: a latched ghost must never paint over an alt
         * screen, scrolled-back view, or mid-text cursor (Tab completion /
         * history recall / arrow keys moved the line under us). */
        !(term->mode & MODE_ALT_SCREEN) &&
        term->scroll_offset == 0) {
        Cell *under = terminal_cell_at(term, term->cursor_x, term->cursor_y);
        bool cursor_on_text = under && under->codepoint &&
                              under->codepoint != ' ';
        if (!cursor_on_text) {
            f32 gx = origin_x + (f32)term->cursor_x * cw;
            f32 gy = origin_y + (f32)term->cursor_y * ch;
            i32 room = term->cols - term->cursor_x;
            if (room < 1) room = 1;
            Color dim = t->fg;
            dim.r *= 0.45f; dim.g *= 0.45f; dim.b *= 0.45f;
            dim.a = 0.85f * app->config.opacity;
            draw_text_ex(r, app->autosuggest_suffix, gx, gy, dim, room, cw);
            renderer_flush_glyphs(r);
        }
    }
}

/* File-browser tab content area: list (left) + viewer (right, when a file
 * is open). Detached Markdown viewer tabs set fb_viewer_only and use the
 * whole tab body for the viewer. */
static void render_fb_tab(AppState *app, Tab *tab, f32 ox, f32 oy, f32 total_w, f32 total_h) {
    if (!tab->fb) return;
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme ? app->config.theme : &THEME_DARK;
    f32 dpi = app->dpi_scale;

    /* Background — solid theme bg so the terminal-padding strip doesn't show. */
    Color bg = t->bg;
    bg.a = 1.0f;
    renderer_draw_rect(r, ox, oy, total_w, total_h, bg);
    renderer_flush_rects(r);

    /* Decide split layout. Viewer present iff a file is open. Graph and
     * markdown open with a narrow list sidebar so the renderer fills the
     * window; on entry, snap the divider back rather than inheriting a wide
     * ratio left over from a previously-open file. */
    bool narrow_list = (tab->fb->view_mode == FVIEW_GRAPH ||
                        tab->fb->view_mode == FVIEW_MARKDOWN);
    if (narrow_list && tab->fb->reset_split_narrow) {
        tab->fb_viewer_ratio = 0.0f;
        tab->fb->reset_split_narrow = false;
    }
    /* The knowledge graph navigates via its own nodes, so its file-list pane is
     * redundant — render it full-width (no list). Without this a graph tab
     * opened while the docked sidebar is up shows a confusing second list pane
     * ("a 2nd sidebar"). Not a persisted flag (which would blank the tab on
     * graph-exit) — derived from the live view mode each frame. */
    bool viewer_only = tab->fb_viewer_only || tab->fb->view_mode == FVIEW_GRAPH;
    bool has_viewer = (tab->fb->view_mode != FVIEW_NONE);
    FbTabSplit split = fb_tab_split(ox, total_w, dpi, has_viewer,
                                    viewer_only, tab->fb_viewer_ratio,
                                    narrow_list);
    f32 list_x = ox;
    f32 list_y = oy;
    f32 list_h = total_h;
    f32 list_w = split.list_w;
    f32 view_x = split.view_x, view_y = oy, view_w = split.view_w, view_h = total_h;

    if (!viewer_only) {
        /* List pane — reuse fb_render_sidebar with the sidebar palette. */
        Color side_bg = t->sidebar_bg;
        side_bg.a = 1.0f;
        Color side_accent = theme_ui_accent(t);
        renderer_push_scissor(r, list_x, list_y, list_w, list_h);
        fb_render_sidebar(tab->fb, r,
                          list_x, list_y, list_w, list_h,
                          &side_bg, &t->sidebar_fg, &t->sidebar_active,
                          &t->border, &side_accent, dpi,
                          app->hover_x, app->hover_y);
        renderer_pop_scissor(r);
    }

    if (has_viewer) {
        if (!viewer_only) {
            /* Vertical divider between list and viewer. */
            f32 div_w = 1.0f * dpi;
            Color div_clr = t->border;
            div_clr.a = 0.9f;
            renderer_draw_rect(r, view_x - div_w, view_y, div_w, view_h, div_clr);
            renderer_flush_rects(r);
        }

        renderer_push_scissor(r, view_x, view_y, view_w, view_h);
        fb_render_viewer(tab->fb, r, view_x, view_y, view_w, view_h, dpi, t, app->config.opacity);
        renderer_pop_scissor(r);
    }
}

static void render_terminal(AppState *app) {
    Tab *tab = app_active_tab(app);
    if (!tab) return;

    f32 ox = app->sidebar_width + app->padding;
    f32 oy = app->tab_bar_height + app->config.style.terminal_top_gap * app->dpi_scale;
    f32 cw = app->renderer.font.cell_width;
    f32 ch = app->renderer.font.cell_height;
    f32 total_w = (f32)app->fb_width - ox - app->padding;
    f32 total_h = (f32)app->fb_height - oy - app->status_bar_height - app->padding;

    /* File-browser tabs short-circuit to their own render path; no sleep
     * card, split, terminal cells, scrollbar, etc. apply. */
    if (tab->kind == TAB_FILEBROWSER) {
        render_fb_tab(app, tab, ox, oy, total_w, total_h);
        return;
    }

    if (tab->sleeping) {
        Renderer *r = &app->renderer;
        const Theme *t = app->config.theme ? app->config.theme : &THEME_DARK;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi;
        f32 ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        f32 op = app->config.opacity;
        ChromePalette cp = chrome_palette_for(t);

        /* Dimmed terminal area behind the card. */
        renderer_draw_rect(r, ox, oy, total_w, total_h,
                          (Color){t->bg.r * 0.92f, t->bg.g * 0.92f, t->bg.b * 0.92f, op});
        renderer_flush_rects(r);

        /* Card sizing: keep the 540pt comfortable width when there's
         * room, scale down on narrow panes. Height shrinks too since
         * the redesign drops the redundant "visual placeholder" line. */
        f32 card_w = total_w > 540 * dpi ? 540 * dpi : total_w - 32 * dpi;
        if (card_w < 260 * dpi) card_w = total_w - 16 * dpi;
        f32 card_h = 156 * dpi;
        f32 card_x = ox + (total_w - card_w) / 2;
        f32 card_y = oy + (total_h - card_h) / 2;

        /* Settings-style panel chrome — single bordered SDF call with
         * theme.bg-derived surface, theme.border stroke, soft drop
         * shadow. Same idiom as the modal dialogs and Settings
         * panel. */
        f32 panel_radius = 14.0f * dpi;
        Color card_bg = cp.surface_neutral; card_bg.a = op;
        Color card_bd = t->border;
        card_bd.a = (card_bd.a > 0.05f ? card_bd.a : 1.0f) * op;
        renderer_draw_rrect_bordered(r,
            card_x, card_y, card_w, card_h,
            card_bg, card_bd, fmaxf(1.0f, dpi),
            panel_radius, panel_radius, panel_radius, panel_radius,
            20.0f * dpi, 0.30f * op, 0.0f, 5.0f * dpi);
        renderer_flush_rrects(r);

        /* Header height (kept for body layout); no hairline divider under the
         * title — the card reads cleaner without it. */
        f32 hdr_h = 38 * dpi;

        /* Sleeping artwork — right-aligned, vertically centred against
         * the full card height (not just the body), no backdrop tile.
         * The aggressive chroma key in liu_sleep_icon_rgba now kills
         * the asset's white halo cleanly, so the cat sits on the
         * card surface directly. */
        f32 img_size  = 132.0f * dpi;
        f32 img_pad_r = 18.0f * dpi;
        if (card_w < 340 * dpi) img_size = 0; /* drop on narrow cards */
        i32 sleep_iw = 0, sleep_ih = 0;
        const u8 *sleep_px = liu_sleep_icon_rgba(&sleep_iw, &sleep_ih);
        f32 img_x = card_x + card_w - img_size - img_pad_r;
        f32 img_y = card_y + (card_h - img_size) * 0.5f;
        if (img_size > 0 && sleep_px) {
            renderer_draw_image(r, sleep_px, sleep_iw, sleep_ih,
                                img_x, img_y, img_size, img_size);
        }

        char title[192];
        tab_format_display_title(tab, title, (i32)sizeof(title));
        const char *kind = tab_primary_session_type(tab) == SESSION_SSH ? "SSH tab"
                          : tab_primary_session_type(tab) == SESSION_MOSH ? "Mosh tab"
                          : "Local tab";
        char line2[128];
        if (app->config.tab_sleep_idle_minutes <= 0.0f)
            snprintf(line2, sizeof(line2), "%s · sleeping", kind);
        else
            snprintf(line2, sizeof(line2), "%s · idle for %.0f min",
                     kind, app->config.tab_sleep_idle_minutes);

        Color title_fg = t->fg;          title_fg.a *= op;
        Color val_fg   = t->fg;          val_fg.a   *= op;
        Color sub_fg   = t->sidebar_fg;  sub_fg.a   *= op;
        Color hint_fg  = (Color){sub_fg.r, sub_fg.g, sub_fg.b, sub_fg.a * 0.65f};

        f32 left_x   = card_x + 20 * dpi;
        f32 text_w   = (img_size > 0 ? img_x - left_x - 14 * dpi
                                      : card_w - 40 * dpi);
        i32 text_max = (i32)(text_w / ui_cw);
        if (text_max < 8) text_max = 8;

        /* Header — title flush in the bar, centred vertically. */
        draw_text_ex(r, "Sleeping Tab Preview",
                    left_x, card_y + (hdr_h - ui_ch) * 0.5f,
                    title_fg, 24, ui_cw);

        /* Body — tab name (loudest), session-kind subtitle (dimmed).
         * Symmetric 18pt top/bottom body padding and consistent 8pt
         * line spacing so the text block sits balanced in the card. */
        f32 body_y = card_y + hdr_h + 18 * dpi;
        i32 tlen = (i32)strlen(title);
        if (tlen > text_max) tlen = text_max;
        draw_text_ex(r, title, left_x, body_y, val_fg, tlen, ui_cw);

        i32 l2len = (i32)strlen(line2);
        if (l2len > text_max) l2len = text_max;
        draw_text_ex(r, line2, left_x, body_y + ui_ch + 8 * dpi,
                    sub_fg, l2len, ui_cw);

        /* Footer hint — single line, aligned with the bottom body pad. */
        const char *hint = "Click inside or press any key to wake";
        i32 hlen = (i32)strlen(hint);
        if (hlen > text_max) hlen = text_max;
        draw_text_ex(r, hint, left_x, card_y + card_h - 18 * dpi - ui_ch,
                    hint_fg, hlen, ui_cw);

        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
        return;
    }

    if (!tab->terminal) return;

    if (tab->split == SPLIT_NONE) {
        render_terminal_pane(app, tab->terminal, &tab->cache1, ox, oy, true);
    } else if (tab->split_root >= 0) {
        Renderer *r = &app->renderer;
        const Theme *theme = app->config.theme ? app->config.theme : &THEME_DARK;
        PaneRect rects[MAX_SPLIT_PANES];
        tab_split_layout_rects(tab, ox, oy, total_w, total_h,
                               app->dpi_scale, rects);

        bool move_active = false;
        f32 move_e = 1.0f;
        if (tab->split_move_animating) {
            f64 raw64 = (platform_time_sec() - tab->split_move_anim_start) / SPLIT_MOVE_DUR;
            if (raw64 >= 1.0) {
                tab->split_move_animating = false;
            } else {
                if (raw64 < 0.0) raw64 = 0.0;
                move_active = true;
                move_e = ease_apply((f32)raw64, MODAL_OPEN_EASE);
            }
        }

        i32 count = tab_split_pane_count(tab);
        for (i32 p = 0; p < count; p++) {
            Terminal *pt = tab_pane_terminal(tab, p);
            TermRenderCache *pc = tab_pane_cache(tab, p);
            if (!pt || !pc) continue;

            /* Open and close share one lightweight path: interpolate each pane's
             * rect from its stored "from" geometry to the final layout, content
             * moving with the rect (no GPU transform). Close shrinks the
             * survivors' gap; open shrinks the source and sweeps the new pane in
             * from the outer edge (its mirror). */
            PaneRect rr = rects[p];
            if (move_active) {
                SplitAnimRect from = tab->split_move_from[p];
                if (from.w > 1.0f && from.h > 1.0f) {
                    rr.x = anim_lerp(from.x, rr.x, move_e);
                    rr.y = anim_lerp(from.y, rr.y, move_e);
                    rr.w = anim_lerp(from.w, rr.w, move_e);
                    rr.h = anim_lerp(from.h, rr.h, move_e);
                }
            }

            renderer_push_scissor(r, rr.x, rr.y, rr.w, rr.h);
            render_terminal_pane(app, pt, pc, rr.x, rr.y,
                                 tab->active_pane == p);
            /* Focus indicator: dim overlay on inactive panes, accent
             * border on the active one. Drawn inside the scissor so the
             * dim doesn't bleed into the divider strip. */
            render_pane_focus_indicator(app, rr.x, rr.y, rr.w, rr.h,
                                        tab->active_pane == p);
            renderer_flush_rects(r);
            renderer_pop_scissor(r);
        }

        Color div_clr = theme->border;
        div_clr.a *= app->config.opacity;
        tab_split_draw_dividers(r, tab, tab->split_root,
                                (PaneRect){ox, oy, total_w, total_h},
                                app->dpi_scale, div_clr);

        /* Pane layout drag feedback: dim source, tint the target edge.
         * Each overlay is drawn inside its own pane scissor so a rect
         * with sub-pixel drift can't bleed past the pane boundary into
         * the divider strip / status bar. */
        if (app->pane_drag_active &&
            app->pane_drag_tab_index >= 0 &&
            app->pane_drag_tab_index < app->tab_count &&
            &app->tabs[app->pane_drag_tab_index] == tab) {
            i32 src = app->pane_drag_src_pane;
            i32 dst = app->pane_drag_hover_pane;
            if (src >= 0 && src < count) {
                PaneRect rr = rects[src];
                renderer_push_scissor(r, rr.x, rr.y, rr.w, rr.h);
                renderer_draw_rect(r, rr.x, rr.y, rr.w, rr.h,
                                   (Color){0.0f, 0.0f, 0.0f, 0.18f});
                renderer_flush_rects(r);
                renderer_pop_scissor(r);
            }
            if (dst >= 0 && dst < count && dst != src) {
                PaneRect rr = rects[dst];
                f32 thick = 2.0f * app->dpi_scale;
                Color a = theme->cursor;
                if (a.a < 0.05f) a = (Color){0.4f, 0.6f, 1.0f, 1.0f};
                a.a = 0.28f * app->config.opacity;
                Color edge = a;
                edge.a = 0.90f * app->config.opacity;
                PaneRect zr = rr;
                switch (app->pane_drag_drop_zone) {
                case 1: zr.w *= 0.45f; break;
                case 2: zr.x += zr.w * 0.55f; zr.w *= 0.45f; break;
                case 3: zr.h *= 0.45f; break;
                case 4: zr.y += zr.h * 0.55f; zr.h *= 0.45f; break;
                default: break;
                }
                renderer_push_scissor(r, rr.x, rr.y, rr.w, rr.h);
                renderer_draw_rect(r, zr.x, zr.y, zr.w, zr.h, a);
                if (app->pane_drag_drop_zone == 1) renderer_draw_rect(r, zr.x, zr.y, thick, zr.h, edge);
                else if (app->pane_drag_drop_zone == 2) renderer_draw_rect(r, zr.x + zr.w - thick, zr.y, thick, zr.h, edge);
                else if (app->pane_drag_drop_zone == 3) renderer_draw_rect(r, zr.x, zr.y, zr.w, thick, edge);
                else if (app->pane_drag_drop_zone == 4) renderer_draw_rect(r, zr.x, zr.y + zr.h - thick, zr.w, thick, edge);
                renderer_flush_rects(r);
                renderer_pop_scissor(r);
            }
        }
    } else if (tab->split == SPLIT_H) {
        f32 div = total_w * tab->split_ratio;
        f32 divider_w = 6 * app->dpi_scale;   /* gap: 2px + 2px line + 2px */
        f32 p0_w = div - divider_w * 0.5f;
        f32 p1_x = ox + div + divider_w * 0.5f;
        f32 p1_w = total_w - (p1_x - ox);
        render_terminal_pane(app, tab->terminal, &tab->cache1, ox, oy, tab->active_pane == 0);
        render_pane_focus_indicator(app, ox, oy, p0_w, total_h, tab->active_pane == 0);
        /* Divider — highlight on hover. Fades with window opacity so the
         * pane border doesn't pop opaque over translucent terminal cells. */
        {
            f32 div_x = ox + div - divider_w/2;
            bool hover = (app->hover_x >= div_x - 4*app->dpi_scale &&
                          app->hover_x <= div_x + divider_w + 4*app->dpi_scale &&
                          app->hover_y >= oy && app->hover_y <= oy + total_h);
            f32 op = app->config.opacity;
            Color div_clr = hover
                ? (Color){0.4f, 0.6f, 1.0f, 1.0f}
                : app->config.theme->border;
            div_clr.a *= op;
            f32 dw = hover ? 3 * app->dpi_scale : 2.0f * app->dpi_scale;
            renderer_draw_rect(&app->renderer, ox + div - dw/2, oy, dw, total_h, div_clr);
        }
        if (tab->terminal2) {
            render_terminal_pane(app, tab->terminal2, &tab->cache2, p1_x, oy, tab->active_pane == 1);
            render_pane_focus_indicator(app, p1_x, oy, p1_w, total_h, tab->active_pane == 1);
        }
    } else if (tab->split == SPLIT_V) {
        f32 div = total_h * tab->split_ratio;
        f32 divider_h = 6 * app->dpi_scale;   /* gap: 2px + 2px line + 2px */
        f32 p0_h = div - divider_h * 0.5f;
        f32 p1_y = oy + div + divider_h * 0.5f;
        f32 p1_h = total_h - (p1_y - oy);
        render_terminal_pane(app, tab->terminal, &tab->cache1, ox, oy, tab->active_pane == 0);
        render_pane_focus_indicator(app, ox, oy, total_w, p0_h, tab->active_pane == 0);
        /* Divider — highlight on hover. */
        {
            f32 div_y = oy + div - divider_h/2;
            bool hover = (app->hover_y >= div_y - 4*app->dpi_scale &&
                          app->hover_y <= div_y + divider_h + 4*app->dpi_scale &&
                          app->hover_x >= ox && app->hover_x <= ox + total_w);
            f32 op = app->config.opacity;
            Color div_clr = hover
                ? (Color){0.4f, 0.6f, 1.0f, 1.0f}
                : app->config.theme->border;
            div_clr.a *= op;
            f32 dh = hover ? 3 * app->dpi_scale : 2.0f * app->dpi_scale;
            renderer_draw_rect(&app->renderer, ox, oy + div - dh/2, total_w, dh, div_clr);
        }
        if (tab->terminal2) {
            render_terminal_pane(app, tab->terminal2, &tab->cache2, ox, p1_y, tab->active_pane == 1);
            render_pane_focus_indicator(app, ox, p1_y, total_w, p1_h, tab->active_pane == 1);
        }
    }

    /* Pane layout drag feedback for the legacy binary split path (when the
     * layout tree isn't being used). Mirrors the multi-pane version
     * above but reconstructs the two PaneRects from split_ratio. */
    if (tab->split != SPLIT_NONE && tab->split_root < 0 &&
        app->pane_drag_active &&
        app->pane_drag_tab_index >= 0 &&
        app->pane_drag_tab_index < app->tab_count &&
        &app->tabs[app->pane_drag_tab_index] == tab) {
        Renderer *r2 = &app->renderer;
        PaneRect pr[2] = {0};
        if (tab->split == SPLIT_H) {
            f32 div = total_w * tab->split_ratio;
            f32 dw  = 2.0f * app->dpi_scale;
            pr[0] = (PaneRect){ox, oy, div - dw * 0.5f, total_h};
            pr[1] = (PaneRect){ox + div + dw * 0.5f, oy,
                               total_w - (div + dw * 0.5f), total_h};
        } else {
            f32 div = total_h * tab->split_ratio;
            f32 dh  = 2.0f * app->dpi_scale;
            pr[0] = (PaneRect){ox, oy, total_w, div - dh * 0.5f};
            pr[1] = (PaneRect){ox, oy + div + dh * 0.5f, total_w,
                               total_h - (div + dh * 0.5f)};
        }
        i32 src = app->pane_drag_src_pane;
        i32 dst = app->pane_drag_hover_pane;
        if (src >= 0 && src < 2) {
            PaneRect rr = pr[src];
            renderer_draw_rect(r2, rr.x, rr.y, rr.w, rr.h,
                               (Color){0.0f, 0.0f, 0.0f, 0.18f});
        }
        if (dst >= 0 && dst < 2 && dst != src) {
            PaneRect rr = pr[dst];
            f32 thick = 2.0f * app->dpi_scale;
            Color a = app->config.theme ? app->config.theme->cursor
                                        : (Color){0.4f, 0.6f, 1.0f, 1.0f};
            if (a.a < 0.05f) a = (Color){0.4f, 0.6f, 1.0f, 1.0f};
            a.a = 0.28f * app->config.opacity;
            Color edge = a;
            edge.a = 0.90f * app->config.opacity;
            PaneRect zr = rr;
            switch (app->pane_drag_drop_zone) {
            case 1: zr.w *= 0.45f; break;
            case 2: zr.x += zr.w * 0.55f; zr.w *= 0.45f; break;
            case 3: zr.h *= 0.45f; break;
            case 4: zr.y += zr.h * 0.55f; zr.h *= 0.45f; break;
            default: break;
            }
            renderer_draw_rect(r2, zr.x, zr.y, zr.w, zr.h, a);
            if (app->pane_drag_drop_zone == 1) renderer_draw_rect(r2, zr.x, zr.y, thick, zr.h, edge);
            else if (app->pane_drag_drop_zone == 2) renderer_draw_rect(r2, zr.x + zr.w - thick, zr.y, thick, zr.h, edge);
            else if (app->pane_drag_drop_zone == 3) renderer_draw_rect(r2, zr.x, zr.y, zr.w, thick, edge);
            else if (app->pane_drag_drop_zone == 4) renderer_draw_rect(r2, zr.x, zr.y + zr.h - thick, zr.w, thick, edge);
        }
        renderer_flush_rects(r2);
    }
    (void)cw; (void)ch;
}

/* =========================================================================
 * Render: Tab drag split zone overlay
 * ========================================================================= */

static void render_split_drop_zones(AppState *app) {
    if (!app->tab_drag_into_split) return;

    Renderer *r = &app->renderer;
    f32 dpi = app->dpi_scale;

    f32 ox = (f32)(app->sidebar_visible ? app->sidebar_width : 0) + app->padding;
    f32 oy = app->tab_bar_height + app->config.style.terminal_top_gap * app->dpi_scale;
    f32 ow = (f32)app->fb_width - ox - app->padding;
    f32 oh = (f32)app->fb_height - oy - app->status_bar_height - app->padding;

    /* Dim the whole terminal area */
    renderer_draw_rect(r, ox, oy, ow, oh, (Color){0.0f, 0.0f, 0.0f, 0.35f});

    /* Edge fraction that counts as a drop zone */
    f32 edge = 0.25f;
    f32 ez_w = ow * edge;
    f32 ez_h = oh * edge;

    /* Accent color for highlighted zone, dim for inactive */
    Color active_clr = {0.38f, 0.07f, 0.76f, 0.55f};
    Color dim_clr    = {0.5f,  0.5f,  0.5f,  0.18f};
    Color border_clr = {0.7f,  0.7f,  1.0f,  0.5f};

    i32 zone = app->tab_drag_split_zone;

    /* Left zone */
    Color lc = (zone == 1) ? active_clr : dim_clr;
    renderer_draw_rect(r, ox, oy, ez_w, oh, lc);
    if (zone == 1) renderer_draw_rect(r, ox + ez_w - dpi, oy, dpi, oh, border_clr);

    /* Right zone */
    Color rc = (zone == 2) ? active_clr : dim_clr;
    renderer_draw_rect(r, ox + ow - ez_w, oy, ez_w, oh, rc);
    if (zone == 2) renderer_draw_rect(r, ox + ow - ez_w, oy, dpi, oh, border_clr);

    /* Top zone */
    Color tc = (zone == 3) ? active_clr : dim_clr;
    renderer_draw_rect(r, ox, oy, ow, ez_h, tc);
    if (zone == 3) renderer_draw_rect(r, ox, oy + ez_h - dpi, ow, dpi, border_clr);

    /* Bottom zone */
    Color bc = (zone == 4) ? active_clr : dim_clr;
    renderer_draw_rect(r, ox, oy + oh - ez_h, ow, ez_h, bc);
    if (zone == 4) renderer_draw_rect(r, ox, oy + oh - ez_h, ow, dpi, border_clr);

    renderer_flush_rects(r);

    /* Zone label glyphs */
    f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
    renderer_set_ui_scale(r, ui_cw, ui_ch);
    Color lbl = {1.0f, 1.0f, 1.0f, 0.9f};

    const char *labels[4] = {"<", ">", "^", "v"};
    f32 lx[4] = { ox + ez_w/2 - ui_cw/2,
                  ox + ow - ez_w/2 - ui_cw/2,
                  ox + ow/2 - ui_cw/2,
                  ox + ow/2 - ui_cw/2 };
    f32 ly[4] = { oy + oh/2 - ui_ch/2,
                  oy + oh/2 - ui_ch/2,
                  oy + ez_h/2 - ui_ch/2,
                  oy + oh - ez_h/2 - ui_ch/2 };
    for (i32 i = 0; i < 4; i++) {
        Color c = ((i + 1) == zone) ? (Color){1.0f, 1.0f, 1.0f, 1.0f} : lbl;
        draw_text_ex(r, labels[i], lx[i], ly[i], c, 1, ui_cw);
    }
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

/* =========================================================================
 * Render benchmark
 * ========================================================================= */

void app_bench_toggle(AppState *app) {
    if (!app) return;
    if (!app->bench_enabled && !app->bench_samples_ms) {
        app->bench_samples_ms = calloc(256, sizeof(f64));
        if (!app->bench_samples_ms) return;
    }
    app->bench_enabled = !app->bench_enabled;
    app->bench_head = 0;
    app->bench_count = 0;
    app->bench_fps_accum = 0;
    app->bench_fps_frames = 0;
    app->bench_fps_t0 = platform_time_sec();
    app_show_toast(app, app->bench_enabled
                   ? "Render benchmark: ON"
                   : "Render benchmark: OFF");
}

void app_bench_record(AppState *app, f64 frame_ms) {
    if (!app || !app->bench_enabled || !app->bench_samples_ms) return;
    app->bench_samples_ms[app->bench_head] = frame_ms;
    app->bench_head = (app->bench_head + 1) % 256;
    if (app->bench_count < 256) app->bench_count++;
    app->bench_fps_frames++;
}

static int bench_cmp_f64(const void *a, const void *b) {
    f64 x = *(const f64 *)a, y = *(const f64 *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void bench_stats(const AppState *app, f64 *avg, f64 *p50, f64 *p99,
                        f64 *maxv) {
    f64 a = 0, m = 0;
    if (app->bench_count <= 0 || !app->bench_samples_ms) { *avg = *p50 = *p99 = *maxv = 0; return; }
    f64 tmp[256];
    for (i32 i = 0; i < app->bench_count; i++) {
        f64 v = app->bench_samples_ms[i];
        a += v;
        if (v > m) m = v;
        tmp[i] = v;
    }
    qsort(tmp, (usize)app->bench_count, sizeof(f64), bench_cmp_f64);
    *avg = a / (f64)app->bench_count;
    *p50 = tmp[app->bench_count / 2];
    i32 p99_idx = (i32)((f64)app->bench_count * 0.99);
    if (p99_idx >= app->bench_count) p99_idx = app->bench_count - 1;
    *p99 = tmp[p99_idx];
    *maxv = m;
}

static void render_bench_overlay(AppState *app) {
    if (!app->bench_enabled) return;
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme ? app->config.theme : &THEME_DARK;
    f32 dpi = app->dpi_scale;
    f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
    renderer_set_ui_scale(r, ui_cw, ui_ch);

    f64 avg, p50, p99, maxv;
    bench_stats(app, &avg, &p50, &p99, &maxv);

    /* Rolling FPS over 1s windows */
    f64 now = platform_time_sec();
    f64 window = now - app->bench_fps_t0;
    if (window >= 1.0) {
        app->bench_fps_accum = (f64)app->bench_fps_frames / window;
        app->bench_fps_t0 = now;
        app->bench_fps_frames = 0;
    }
    f64 fps = app->bench_fps_accum;

    /* Refresh CPU/RAM at 1 Hz — Mach calls used to fire per frame here
     * (60-120 Hz with bench enabled), which itself was a major source of
     * the visible CPU sawtooth. Reuse the shared res_last_update gate. */
    if (now - app->res_last_update > 1.0) {
        query_resources(&app->res_cpu, &app->res_mem_mb);
        app->res_last_update = now;
    }

    /* Tab status breakdown */
    i32 sleeping = 0, alive = 0, hibernated_ssh = 0;
    for (i32 i = 0; i < app->tab_count; i++) {
        if (app->tabs[i].sleeping) {
            sleeping++;
            Session *s = app->tabs[i].session;
            if (s && session_is_suspended(s) && session_type(s) == SESSION_SSH)
                hibernated_ssh++;
        } else {
            alive++;
        }
    }

    Tab *active = app_active_tab(app);
    i32 cols = active && active->terminal ? active->terminal->cols : 0;
    i32 rows = active && active->terminal ? active->terminal->rows : 0;
    i32 sb   = active && active->terminal ? active->terminal->sb_count : 0;

    char l1[96], l2[96], l3[96], l4[96], l5[96], l6[96], l7[96];
    snprintf(l1, sizeof(l1), "RENDER BENCH  (Cmd+K -> toggle)");
    snprintf(l2, sizeof(l2), "FPS %6.1f    frames %d", fps, app->bench_count);
    snprintf(l3, sizeof(l3), "avg %6.2f ms   p50 %6.2f ms", avg, p50);
    snprintf(l4, sizeof(l4), "p99 %6.2f ms   max %6.2f ms", p99, maxv);
    snprintf(l5, sizeof(l5), "CPU %5.1f%%     RAM %6.1f MB", app->res_cpu, app->res_mem_mb);
    snprintf(l6, sizeof(l6), "tabs %d  alive %d  sleep %d  sshH %d",
             app->tab_count, alive, sleeping, hibernated_ssh);
    snprintf(l7, sizeof(l7), "active grid %dx%d   scrollback %d", cols, rows, sb);

    const char *lines[] = { l1, l2, l3, l4, l5, l6, l7 };
    const i32 nlines = (i32)(sizeof(lines) / sizeof(lines[0]));

    f32 pad = 8 * dpi;
    f32 w = 340 * dpi;
    f32 h = ui_ch * nlines + pad * 2;
    f32 x = (f32)app->fb_width - w - pad;
    f32 y = app->tab_bar_height + pad;

    /* Tooltip chrome — theme-driven so the panel reads on light themes
     * and the border picks up the user's ui_accent / chrome accent. */
    ChromePalette _cp_tt = chrome_palette_for(t);
    Color bg   = _cp_tt.surface_neutral; bg.a = 0.95f;
    Color _bd  = theme_ui_accent(t);
    Color bord = {_bd.r, _bd.g, _bd.b, 0.95f};
    draw_panel_chrome(r, x, y, w, h, bg, bord, 8.0f * dpi, dpi,
                      12.0f * dpi, 0.30f);
    renderer_flush_rrects(r);

    for (i32 i = 0; i < nlines; i++) {
        Color c = (i == 0) ? t->ansi[11] : t->fg;
        draw_text_ex(r, lines[i], x + pad, y + pad + i * ui_ch, c, 48, ui_cw);
    }
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

/* =========================================================================
 * Main render
 * ========================================================================= */

void app_render(AppState *app) {
    /* Synchronized output: skip render if BSU pending (with 1s timeout) */
    Tab *sync_tab = app_active_tab(app);
    if (sync_tab && sync_tab->terminal && sync_tab->terminal->sync_pending) {
        f64 now = platform_time_sec();
        if (sync_tab->terminal->sync_start_time == 0)
            sync_tab->terminal->sync_start_time = now;
        if (now - sync_tab->terminal->sync_start_time < 1.0)
            return; /* defer render */
        /* Timeout — force render */
        sync_tab->terminal->sync_pending = false;
    }

    /* Sidebar open/close animation.
     *
     * Toggle handlers in main.c flip `sidebar_visible` and slam
     * `sidebar_width` to the target (0 or SIDEBAR_DEFAULT*dpi) immediately.
     * We detect that visibility flip here, capture the previous frame's
     * width as the animation start, and let the eased interpolation drive
     * `sidebar_width` over the next ~200ms — every consumer (terminal
     * layout, hit-testing, status bar) reads the live animated width, so
     * the slide is automatic across the whole UI without scattered
     * tracking. Drag-resize is preserved: when visibility doesn't change
     * between frames, the user-set width passes through unchanged. */
    {
        static bool s_was_visible = false;
        static f32  s_prev_width  = 0.0f;
        static f32  s_anim_from   = 0.0f;
        static f32  s_anim_to     = 0.0f;
        static Anim s_sidebar_anim = {0};
        static bool s_inited      = false;

        if (!s_inited) {
            s_was_visible = app->sidebar_visible;
            s_prev_width  = app->sidebar_width;
            s_inited      = true;
        }

        if (app->sidebar_visible != s_was_visible) {
            s_was_visible = app->sidebar_visible;
            s_anim_from   = s_prev_width;
            /* Open to the user's remembered width (persisted in config,
             * points) so a resize survives close/reopen and restart. */
            s_anim_to     = app->sidebar_visible
                ? CLAMP(app->config.sidebar_width, SIDEBAR_MIN_PT, SIDEBAR_MAX_PT) * app->dpi_scale
                : 0.0f;
            anim_start(&s_sidebar_anim, 0.22);
        }

        if (!anim_done(&s_sidebar_anim)) {
            f32 e = anim_eased(&s_sidebar_anim, EASE_OUT_CUBIC);
            app->sidebar_width = anim_lerp(s_anim_from, s_anim_to, e);
        }
        s_prev_width = app->sidebar_width;
    }

    const Theme *t = app->config.theme;
    Renderer *r = &app->renderer;

    /* Set clear color from theme before begin_frame. The blend state uses
     * straight alpha (SourceAlpha/OneMinusSourceAlpha), so we must NOT
     * pre-multiply RGB by alpha here — renderer_set_clear_color handles
     * the sRGB→linear boundary. */
    f32 win_opacity = app->config.opacity;
    f32 bg_alpha = 1.0f;
    if (r->bg_texture)          bg_alpha = (1.0f - r->bg_opacity) * win_opacity;
    else if (win_opacity < 1.0f) bg_alpha = win_opacity;
    renderer_set_clear_color(r, t->bg.r, t->bg.g, t->bg.b, bg_alpha);
    renderer_begin_frame(r, app->fb_width, app->fb_height);

    /* Draw background image immediately after clear, before any UI */
    if (r->bg_texture) {
        renderer_draw_background_image(r);
    }

    /* pull any glyphs rasterized off-thread into the atlas before
     * the first glyph lookup of the frame.  If new glyphs landed, force a
     * full rebuild of every per-terminal row cache so cells that previously
     * resolved to the space placeholder pick up their real UVs. */
    if (font_drain_raster_completions(&r->font) > 0) {
        for (i32 i = 0; i < app->tab_count; i++) {
            app->tabs[i].cache1.all_rows_dirty = true;
            app->tabs[i].cache2.all_rows_dirty = true;
        }
    }

    render_toolbar(app);
    render_sidebar(app);
    render_terminal(app);
    render_split_drop_zones(app);

    /* Visual bell flash overlay (50ms white overlay at 15% opacity) */
    if (app->bell_flash_time > 0) {
        f64 now = platform_time_sec();
        f64 elapsed = now - app->bell_flash_time;
        if (elapsed < 0.05) {
            /* Fade out: full opacity at start, zero at 50ms */
            f32 alpha = 0.15f * (1.0f - (f32)(elapsed / 0.05));
            renderer_draw_rect(r, 0, app->tab_bar_height,
                              (f32)app->fb_width,
                              (f32)app->fb_height - app->tab_bar_height - app->status_bar_height,
                              (Color){1.0f, 1.0f, 1.0f, alpha});
            renderer_flush_rects(r);
        } else {
            app->bell_flash_time = 0;
        }
    }

    /* File viewer panel (right side, if a file is open) */
    if (app->filebrowser.view_mode != FVIEW_NONE) {
        f32 default_vw = (f32)app->fb_width * VIEWER_WIDTH_RATIO;
        f32 vw = app->viewer_width > 0 ? app->viewer_width : default_vw;
        /* Clamp: min width VIEWER_MIN_PT, max leaves room for the sidebar */
        f32 vmin = VIEWER_MIN_PT * app->dpi_scale;
        f32 vmax = (f32)app->fb_width
                 - (app->sidebar_visible ? app->sidebar_width : 0.0f)
                 - VIEWER_RESIZE_GRAB_PT * app->dpi_scale;
        if (vmax < vmin) vmax = vmin;
        if (vw < vmin) vw = vmin;
        if (vw > vmax) vw = vmax;
        app->viewer_width = vw;
        f32 vx = (f32)app->fb_width - vw;
        f32 vy = app->tab_bar_height;
        f32 vh = (f32)app->fb_height - vy - app->status_bar_height;
        fb_render_viewer(&app->filebrowser, r, vx, vy, vw, vh, app->dpi_scale, app->config.theme, app->config.opacity);

        /* Resize handle: vertical border on the viewer's left edge. Dim by
         * default, highlights blue on hover/drag to signal draggability. */
        const Theme *t = app->config.theme;
        bool hot = app->viewer_resizing ||
                   (app->hover_x >= vx - VIEWER_RESIZE_GRAB_PT * app->dpi_scale &&
                    app->hover_x <= vx + VIEWER_RESIZE_GRAB_PT * app->dpi_scale &&
                    app->hover_y >= vy && app->hover_y < vy + vh);
        Color edge = hot ? (Color){0.40f, 0.65f, 0.95f, 1.0f}
                         : (Color){t->border.r, t->border.g, t->border.b, 0.9f};
        f32 edge_w = hot ? 2.0f * app->dpi_scale : 1.0f * app->dpi_scale;
        renderer_draw_rect(r, vx - edge_w, vy, edge_w, vh, edge);
        renderer_flush_rects(r);
    }

    render_status_bar(app);

    /* Sites / dev-server manager overlay (Cmd+Shift+S) — internally gated by
     * its modal animation, so calling it every frame is cheap when closed. */
    render_sites_panel(app);

    /* Command palette overlay (Cmd+K) */
    {
        /* Edge-detection state for both open and close transitions.
         * The close path keeps rendering after palette_active flips back
         * to false until s_palette_close_anim drains, so dismissals fade
         * out instead of cutting hard. */
        static bool s_palette_was_open = false;
        static Anim s_palette_open_anim  = {0};
        static Anim s_palette_close_anim = {0};
        /* Selection slide tracking is hoisted here so we can reset it on the
         * open edge (otherwise reopening after the user picked row N causes
         * a long ghost slide from N → 0). */
        static i32  s_pal_last_selected = -1;
        static i32  s_pal_last_scroll   = -1;
        static f32  s_pal_last_y_offset = 0.0f;
        static Anim s_pal_sel_anim = {0};
        bool was_open_before = s_palette_was_open;

        /* Mode-change replay: when the user picks "Agent History..."
         * the palette stays open but its panel jumps from 500×300 to
         * 620×460. modal_anim_progress only fires on the open edge,
         * so without this the larger layout snaps in. Restart the
         * open anim on the transition so history mode gets the same
         * scale/slide entrance as a fresh open. */
        static bool s_pal_history_was = false;
        bool pm_history_now = (palette_mode() == PALETTE_MODE_HISTORY);
        if (app->palette_active && pm_history_now && !s_pal_history_was) {
            anim_start(&s_palette_open_anim, MODAL_OPEN_DUR_FAST);
            anim_reset(&s_palette_close_anim);
        }
        s_pal_history_was = app->palette_active ? pm_history_now : false;

        f32 dpi = app->dpi_scale;
        f32 panel_scale, panel_alpha, panel_yoff;
        bool render_palette = modal_anim_progress(
            app->palette_active,
            &s_palette_open_anim, &s_palette_close_anim,
            &s_palette_was_open, dpi, MODAL_OPEN_DUR_FAST,
            &panel_scale, &panel_alpha, &panel_yoff);

        /* Falling edge — close anim fully drained: release the palette's
         * item arrays (~198 KB) + history side tables (~156 KB) until the
         * next open. Deferred to here (not the palette_active flip) so the
         * fade-out still has rows to draw. Every open path re-ensures the
         * arrays lazily, and the selection flow copies sid/path out before
         * dismissing, so nothing dangles. */
        static bool s_pal_was_rendering = false;
        if (s_pal_was_rendering && !render_palette && !app->palette_active)
            palette_close();
        s_pal_was_rendering = render_palette;

        /* Reset selection tracker on the rising edge so the first frame of
         * a new open doesn't replay last session's slide. */
        if (app->palette_active && !was_open_before) {
            s_pal_last_selected = app->palette_selected;
            s_pal_last_scroll   = app->palette_scroll;
            s_pal_last_y_offset = 0.0f;
            anim_reset(&s_pal_sel_anim);
        }

    if (render_palette) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;

        /* Taller, slightly wider panel in Agent History mode so the 2-line
         * rows get proper breathing room. Clamp the base size before the
         * open animation; on narrow Retina windows the old fixed 500pt panel
         * exceeded the framebuffer and clipped the left side of the text. */
        bool pm_history = (palette_mode() == PALETTE_MODE_HISTORY);
        f32 margin_x = 16.0f * dpi;
        f32 margin_bottom = 16.0f * dpi + app->status_bar_height;
        f32 top_y = app->tab_bar_height + 20.0f * dpi;
        f32 pw = (pm_history ? 620.0f : 500.0f) * dpi;
        f32 ph = (pm_history ? 460.0f : 300.0f) * dpi;
        f32 max_pw = (f32)app->fb_width - 2.0f * margin_x;
        if (max_pw < 220.0f * dpi) max_pw = (f32)app->fb_width - 2.0f * dpi;
        if (pw > max_pw) pw = max_pw;
        if (pw < 1.0f) pw = 1.0f;
        f32 max_ph = (f32)app->fb_height - top_y - margin_bottom;
        if (max_ph < 96.0f * dpi) max_ph = (f32)app->fb_height - 2.0f * margin_x;
        if (ph > max_ph) ph = max_ph;
        if (ph < 80.0f * dpi) ph = 80.0f * dpi;
        f32 ppx = ((f32)app->fb_width - pw) / 2;
        f32 ppy = top_y;
        if (ppx < margin_x) ppx = margin_x;
        if (ppy + ph > (f32)app->fb_height - margin_bottom) {
            ppy = (f32)app->fb_height - margin_bottom - ph;
        }
        if (ppy < margin_x) ppy = margin_x;

        /* Open + close animation — scale, alpha and Y-slide all eased the
         * same way via modal_anim_progress. The alpha is then applied to
         * *every* drawable inside the panel so the whole modal fades
         * coherently — without this, the inner text/input/rows popped in
         * at full opacity while the panel was still scaling. */
        f32 sx = ppx + (pw - pw * panel_scale) * 0.5f;
        f32 sy = ppy + (ph - ph * panel_scale) * 0.5f - panel_yoff;
        ppx = sx; ppy = sy; pw *= panel_scale; ph *= panel_scale;

        /* Dim background — alpha fades in with the open animation. */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                           (Color){0, 0, 0, 0.3f * panel_alpha});

        /* Panel — rounded with soft drop shadow. Border drawn first as a
         * slightly larger rrect; the panel body sits on top. */
        f32 panel_radius = 10.0f * dpi;
        Color panel_border = t->border;
        panel_border.a = fmaxf(panel_border.a, 0.7f) * panel_alpha;
        renderer_draw_rrect(r,
            ppx - 1.0f * dpi, ppy - 1.0f * dpi,
            pw + 2.0f * dpi, ph + 2.0f * dpi,
            panel_border,
            panel_radius + 1.0f, panel_radius + 1.0f,
            panel_radius + 1.0f, panel_radius + 1.0f,
            22.0f * dpi, 0.42f * panel_alpha, 0.0f, 10.0f * dpi);
        Color panel_bg = {t->tab_inactive_bg.r, t->tab_inactive_bg.g, t->tab_inactive_bg.b, 0.98f * panel_alpha};
        renderer_draw_rrect_simple(r, ppx, ppy, pw, ph, panel_bg, panel_radius);

        /* Input field — top corners follow the panel's radius, bottom flat
         * since the row list begins immediately below. */
        f32 inp_h = 32 * dpi;
        Color inp_bg = t->sidebar_bg; inp_bg.a *= panel_alpha;
        renderer_draw_rrect(r, ppx, ppy, pw, inp_h, inp_bg,
            panel_radius, panel_radius, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f);
        Color ui_accent_clr = theme_ui_accent(t);
        Color inp_sep = ui_accent_clr; inp_sep.a *= panel_alpha;
        renderer_draw_rect(r, ppx, ppy + inp_h - 1, pw, 1, inp_sep);
        /* Flush BOTH rects and rrects here so the panel/border/input-field
         * shapes hit the framebuffer before the caret rect that follows.
         * Without this, the caret was queued first, then renderer_flush_glyphs
         * (called at end of the palette block) flushed the deferred rrect
         * batch on top — drawing the input field over the caret and making
         * the typing indicator invisible. */
        renderer_flush_rects(r);
        renderer_flush_rrects(r);

        f32 ty3 = ppy + (inp_h - ui_ch) / 2;
        /* Numeric input sub-mode shows a labeled prompt and hides the
         * fuzzy-match result list. Mode 1 = font size, mode 2 = opacity. */
        if (app->palette_input_mode == 1 || app->palette_input_mode == 2) {
            bool is_opacity = (app->palette_input_mode == 2);
            const char *prompt = is_opacity ? "Opacity (%): " : "Font size (pt): ";
            i32 plen = (i32)strlen(prompt);
            Color np_lbl = t->ansi[12];   np_lbl.a *= panel_alpha;
            Color np_val = t->fg;         np_val.a *= panel_alpha;
            Color np_caret_clr = t->cursor;
            if (np_caret_clr.a < 0.5f ||
                (np_caret_clr.r + np_caret_clr.g + np_caret_clr.b) < 0.3f) {
                np_caret_clr = ui_accent_clr;
            }
            f64 bnow = platform_time_sec();
            bool caret_on = fmod(bnow, 1.06) < 0.53;
            f32 caret_a = caret_on ? 1.0f : 0.85f;
            Color np_cur = (Color){np_caret_clr.r, np_caret_clr.g, np_caret_clr.b,
                                   caret_a * panel_alpha};
            f32 input_pad = 8.0f * dpi;
            draw_text_ex(r, prompt, ppx + input_pad, ty3, np_lbl, plen, ui_cw);
            i32 value_max = (i32)((pw - 2.0f * input_pad) / ui_cw) - plen - 1;
            if (value_max < 1) value_max = 1;
            draw_text_ex(r, app->palette_query, ppx + input_pad + plen * ui_cw, ty3,
                         np_val, value_max, ui_cw);
            f32 pcur = ppx + input_pad + (plen + app->palette_query_len) * ui_cw;
            f32 pcur_max = ppx + pw - input_pad - fmaxf(2.0f, 2.0f * dpi);
            if (pcur > pcur_max) pcur = pcur_max;
            renderer_draw_rect(r, pcur, ty3, fmaxf(2.0f, 2.0f * dpi), ui_ch, np_cur);

            /* Hint line under the input — shows current value plus the
             * accepted range so the user knows the bounds without
             * trial-and-error. */
            char hint[96];
            if (is_opacity) {
                snprintf(hint, sizeof(hint),
                         "current: %d%%   range 30..100   Enter to apply, Esc to cancel",
                         (i32)(app->config.opacity * 100.0f + 0.5f));
            } else {
                snprintf(hint, sizeof(hint),
                         "current: %.0fpt   range 6..72   Enter to apply, Esc to cancel",
                         app->config.font_size);
            }
            i32 hint_max = (i32)((pw - 24.0f * dpi) / ui_cw);
            if (hint_max < 1) hint_max = 1;
            draw_text_ex(r, hint, ppx + 12.0f * dpi, ppy + inp_h + 8.0f * dpi,
                         (Color){t->sidebar_fg.r, t->sidebar_fg.g, t->sidebar_fg.b, 0.6f * panel_alpha},
                         hint_max, ui_cw);

            renderer_flush_rects(r);
            renderer_flush_glyphs(r);
            renderer_reset_ui_scale(r);
            goto palette_done;
        }
        /* Prompt — "> " in root, "Agent History ◂  " in history mode so the
         * mode shift is visible. */
        bool history_mode  = (palette_mode() == PALETTE_MODE_HISTORY);
        bool outline_mode  = (palette_mode() == PALETTE_MODE_OUTLINE);
        bool switcher_mode = (palette_mode() == PALETTE_MODE_SWITCHER);
        bool search_mode   = (palette_mode() == PALETTE_MODE_SEARCH);
        bool backlinks_mode= (palette_mode() == PALETTE_MODE_BACKLINKS);
        bool folder_mode   = (palette_mode() == PALETTE_MODE_FOLDER);
        const char *prompt = history_mode   ? "Agent History  "
                           : outline_mode   ? "Headings  "
                           : switcher_mode  ? "Open Note  "
                           : search_mode    ? "Search  "
                           : backlinks_mode ? "Backlinks  "
                           : folder_mode    ? "Filter Folder  " : "> ";
        i32 prompt_len = (i32)strlen(prompt);
        /* Root prompt follows the theme's ui_accent so each palette
         * picks up its own identity colour — was hardcoded to ansi[12]
         * (bright blue) which fought every non-blue theme. History mode
         * keeps its warm orange so the two modes stay visually distinct
         * regardless of which theme is active. */
        Color prompt_fg = history_mode
            ? (Color){0.96f, 0.66f, 0.32f, 1.0f}
            : (outline_mode || switcher_mode || search_mode || backlinks_mode || folder_mode)
            ? (Color){0.45f, 0.78f, 0.95f, 1.0f}
            : ui_accent_clr;
        prompt_fg.a *= panel_alpha;
        Color query_fg = t->fg; query_fg.a *= panel_alpha;
        f32 input_pad = 8.0f * dpi;
        draw_text_ex(r, prompt, ppx + input_pad, ty3, prompt_fg, prompt_len, ui_cw);
        i32 query_max = (i32)((pw - 2.0f * input_pad) / ui_cw) - prompt_len - 1;
        if (query_max < 1) query_max = 1;
        draw_text_ex(r, app->palette_query, ppx + input_pad + prompt_len * ui_cw, ty3,
                     query_fg, query_max, ui_cw);
        /* Caret — discrete on/off blink (macOS cadence: ~530ms on, ~530ms
         * off) at full alpha. The earlier sine-pulse never reached zero
         * but its 25 % low alpha still read as "caret missing" on dark
         * themes; a square pulse with a strong floor (0.85 even when
         * "off") is far more legible. Falls back to the theme accent if
         * the cursor color is too dim/transparent to be visible. */
        f32 pcur = ppx + input_pad + (prompt_len + app->palette_query_len) * ui_cw;
        f32 caret_w = fmaxf(2.0f, 2.0f * dpi);
        f32 pcur_max = ppx + pw - input_pad - caret_w;
        if (pcur > pcur_max) pcur = pcur_max;
        Color caret_clr = t->cursor;
        if (caret_clr.a < 0.5f ||
            (caret_clr.r + caret_clr.g + caret_clr.b) < 0.3f) {
            caret_clr = ui_accent_clr;   /* theme-customisable fallback */
        }
        f64 bnow = platform_time_sec();
        bool caret_on = fmod(bnow, 1.06) < 0.53;  /* macOS-ish 530/530ms */
        f32 caret_a = caret_on ? 1.0f : 0.85f;
        renderer_draw_rect(r, pcur, ty3, caret_w, ui_ch,
                           (Color){caret_clr.r, caret_clr.g, caret_clr.b,
                                   caret_a * panel_alpha});

        /* Esc hint in any non-root mode (top-right of input field) */
        if (history_mode || outline_mode || switcher_mode || search_mode ||
            backlinks_mode || folder_mode) {
            const char *hint = "Esc  back";
            i32 hlen = (i32)strlen(hint);
            f32 hx = ppx + pw - (f32)hlen * ui_cw - 12 * dpi;
            Color hc = { t->sidebar_fg.r, t->sidebar_fg.g, t->sidebar_fg.b, 0.55f * panel_alpha };
            draw_text_ex(r, hint, hx, ty3, hc, hlen, ui_cw);
        }

        /* Results from palette backend */
        palette_set_query(app->palette_query);
        i32 count = palette_filtered_count();

        /* Row layout: single-line in root mode, 2-line (title + subtitle) in
         * Agent History mode with generous vertical padding. */
        f32 row_h_root    = ui_ch + 10.0f * dpi;
        f32 row_h_history = 2.0f * ui_ch + 18.0f * dpi;
        f32 row_h = pm_history ? row_h_history : row_h_root;
        f32 list_top = ppy + inp_h + 8.0f * dpi;
        i32 max_visible = (i32)((ph - (list_top - ppy) - 8.0f * dpi) / row_h);
        if (max_visible < 1) max_visible = 1;

        /* Clamp selected to valid range. */
        if (app->palette_selected < 0) app->palette_selected = 0;
        if (count > 0 && app->palette_selected >= count)
            app->palette_selected = count - 1;
        if (app->palette_scroll < 0) app->palette_scroll = 0;
        i32 scroll_max = count > max_visible ? count - max_visible : 0;
        if (app->palette_scroll > scroll_max) app->palette_scroll = scroll_max;

        /* Bidirectional sync of selection ↔ scroll.
         *
         * Two input sources can change these:
         *   (a) Arrow keys mutate palette_selected — we must scroll to keep
         *       the selection visible.
         *   (b) Mouse wheel / scrollbar drag mutate palette_scroll — we must
         *       slide the selection so it stays inside the visible window
         *       (otherwise the renderer's old "snap scroll back to selection"
         *       clamp would immediately undo the scroll, which is exactly
         *       why the wheel/scrollbar appeared to be no-ops).
         *
         * We tell the cases apart by comparing against the previous frame's
         * values: whichever moved is the source. If both look unchanged, do
         * nothing — count or max_visible may have shrunk after a filter and
         * the clamps above already brought things into range. */
        bool sel_changed    = (app->palette_selected != s_pal_last_selected);
        bool scroll_changed = (app->palette_scroll   != s_pal_last_scroll);
        if (sel_changed && !scroll_changed) {
            if (app->palette_selected < app->palette_scroll)
                app->palette_scroll = app->palette_selected;
            if (app->palette_selected >= app->palette_scroll + max_visible)
                app->palette_scroll = app->palette_selected - max_visible + 1;
            if (app->palette_scroll < 0) app->palette_scroll = 0;
            if (app->palette_scroll > scroll_max) app->palette_scroll = scroll_max;
        } else if (scroll_changed) {
            if (app->palette_selected < app->palette_scroll)
                app->palette_selected = app->palette_scroll;
            if (app->palette_selected >= app->palette_scroll + max_visible)
                app->palette_selected = app->palette_scroll + max_visible - 1;
            if (app->palette_selected < 0) app->palette_selected = 0;
            if (count > 0 && app->palette_selected >= count)
                app->palette_selected = count - 1;
        }
        s_pal_last_scroll = app->palette_scroll;

        /* Smooth selection slide: the highlight pill animates between the
         * previous selection's Y and the current one rather than snapping,
         * which makes arrow-key navigation feel fluid instead of jumpy.
         * Tracker state lives at the outer scope so reopen edges can clear
         * it before a stale offset replays.
         *
         * Skip the slide when the selection moved as a side effect of
         * wheel/scrollbar scrolling: the pill would otherwise appear to
         * lag far below its row during a fast flick (the visual content
         * has already shifted up by the same delta, so any easing reads as
         * the highlight overshooting downward). */
        if (app->palette_selected != s_pal_last_selected) {
            if (scroll_changed) {
                /* Scroll-driven move — snap, no slide. */
                s_pal_last_selected = app->palette_selected;
                s_pal_last_y_offset = 0.0f;
                anim_reset(&s_pal_sel_anim);
            } else {
                /* Distance just travelled in row-space; we ease this back to 0.
                 * Clamp to ±max_visible so a Page-Down or scroll-jump doesn't
                 * launch a giant cross-panel slide that fights the user. */
                f32 delta = (f32)(s_pal_last_selected - app->palette_selected);
                f32 max_slide = (f32)max_visible;
                if (delta >  max_slide) delta =  max_slide;
                if (delta < -max_slide) delta = -max_slide;
                s_pal_last_y_offset = delta;
                s_pal_last_selected = app->palette_selected;
                anim_start(&s_pal_sel_anim, 0.14);
            }
        }
        f32 sel_slide_rows = 0.0f;
        if (!anim_done(&s_pal_sel_anim)) {
            f32 e = anim_eased(&s_pal_sel_anim, EASE_OUT_CUBIC);
            sel_slide_rows = s_pal_last_y_offset * (1.0f - e);
        }

        f32 item_y = list_top;
        for (i32 vi = 0; vi < max_visible; vi++) {
            i32 pi = app->palette_scroll + vi;
            if (pi >= count) break;
            PaletteItem *item = palette_get_item(pi);
            if (!item) continue;
            bool sel = (pi == app->palette_selected);

            /* Row highlight covers the full row height (mode-aware) — rounded
             * pill shape with a sharp accent strip on the left edge. The
             * pill's Y is offset by the in-flight slide so it visually
             * tracks between the old and new row. */
            if (sel) {
                f32 pill_y = item_y + sel_slide_rows * row_h;
                renderer_draw_rrect_simple(r,
                    ppx + 6.0f * dpi, pill_y,
                    pw - 12.0f * dpi, row_h - 2.0f * dpi,
                    (Color){ui_accent_clr.r, ui_accent_clr.g, ui_accent_clr.b, 0.22f * panel_alpha},
                    6.0f * dpi);
                Color accent_fg = ui_accent_clr; accent_fg.a *= panel_alpha;
                renderer_draw_rrect(r,
                    ppx + 6.0f * dpi, pill_y,
                    3.0f * dpi, row_h - 2.0f * dpi,
                    accent_fg,
                    1.5f * dpi, 0.0f, 0.0f, 1.5f * dpi,
                    0.0f, 0.0f, 0.0f, 0.0f);
            }

            if (item->type == 4) {
                /* ---------- Agent-history row (2 lines) ---------- */
                ChatTool tool_e = (ChatTool)item->tool;
                AgentTint tint  = agent_icon_tint(tool_e);
                i32 iw = 0, ih = 0;
                const u8 *icon_px = agent_icon_rgba(tool_e, &iw, &ih);

                f32 bw = 26.0f * dpi;
                f32 bh = 26.0f * dpi;
                f32 bx = ppx + 16.0f * dpi;
                f32 by = item_y + (row_h - bh) * 0.5f;

                /* No tile backdrop — agent PNGs sit directly on the panel
                 * surface (each logo already carries its own contrast). */
                if (icon_px) {
                    f32 pad = 2.0f * dpi;
                    renderer_flush_rects(r);
                    renderer_draw_image(r, icon_px, iw, ih,
                                        bx + pad, by + pad,
                                        bw - pad * 2.0f, bh - pad * 2.0f);
                } else {
                    const char *letter = agent_icon_letter(tool_e);
                    f32 lx = bx + (bw - ui_cw) * 0.5f;
                    f32 ly = by + (bh - ui_ch) * 0.5f;
                    /* Brand-tinted letter on the white backdrop. */
                    draw_text_ex(r, letter, lx, ly,
                                 (Color){tint.r, tint.g, tint.b, panel_alpha}, 1, ui_cw);
                }

                f32 text_x = bx + bw + 14.0f * dpi;
                f32 text_right = ppx + pw - 16.0f * dpi;
                f32 title_y    = item_y + 6.0f * dpi;
                f32 sub_y      = item_y + 6.0f * dpi + ui_ch + 4.0f * dpi;

                /* Line 1: project name — primary, white. Truncated to the
                 * full right edge (no right-column steals space from it). */
                i32 title_max = (i32)((text_right - text_x) / ui_cw);
                if (title_max < 1) title_max = 1;
                draw_text_ex(r, item->text, text_x, title_y,
                             (Color){t->fg.r, t->fg.g, t->fg.b, 0.98f * panel_alpha},
                             title_max, ui_cw);

                /* Line 2: subtitle (tool · id · time · size) — dimmed. */
                Color dim = { t->sidebar_fg.r, t->sidebar_fg.g, t->sidebar_fg.b, 0.62f * panel_alpha };
                i32 sub_max = (i32)((text_right - text_x) / ui_cw);
                if (sub_max < 1) sub_max = 1;
                draw_text_ex(r, item->detail, text_x, sub_y, dim, sub_max, ui_cw);

            } else {
                /* ---------- Non-history (root) row — single line ---------- */
                f32 text_x = ppx + 14.0f * dpi;
                f32 baseline_y = item_y + (row_h - ui_ch) * 0.5f;

                Color ic = item->type == 0 ? t->ansi[12]       /* host   */
                         : item->type == 2 ? t->sidebar_fg      /* command */
                         : item->type == 8 ? t->fg              /* site row */
                         : t->ansi[10];                          /* snippet */
                ic.a *= panel_alpha;

                /* Reserve right-hand slot for detail so main text clips cleanly. */
                i32 dlen = item->detail[0] ? (i32)strlen(item->detail) : 0;
                f32 detail_w = (f32)dlen * ui_cw;
                f32 detail_x = ppx + pw - detail_w - 14.0f * dpi;
                f32 text_max_px = (dlen > 0 ? detail_x - 12.0f * dpi : ppx + pw - 14.0f * dpi) - text_x;
                i32 text_max = (i32)(text_max_px / ui_cw);
                if (text_max < 1) text_max = 1;
                draw_text_ex(r, item->text, text_x, baseline_y, ic, text_max, ui_cw);

                if (dlen > 0) {
                    Color dim = { t->sidebar_fg.r, t->sidebar_fg.g, t->sidebar_fg.b, 0.6f * panel_alpha };
                    draw_text_ex(r, item->detail, detail_x, baseline_y, dim, dlen, ui_cw);
                }
            }

            item_y += row_h;
        }

        /* Scrollbar — only when the list overflows. Anchored to the right
         * inner edge of the list area; thumb size is proportional to the
         * visible-fraction of the list with a sensible minimum so a 200-
         * row history is still grabbable. The track itself is mostly
         * transparent so it doesn't fight the row pills for attention.
         *
         * Geometry is cached on AppState so the mouse click/drag handler
         * can hit-test against the same numbers — recomputing them in
         * main.c would double the maintenance surface. The cache is left
         * stale (from the previous frame) when overflow drops to zero;
         * the click handler re-validates against `palette_sb_scroll_max`
         * before honouring a grab. */
        if (count > max_visible) {
            f32 sb_w        = 5.0f * dpi;
            f32 sb_pad_r    = 6.0f * dpi;
            f32 list_h      = (f32)max_visible * row_h;
            f32 sb_x        = ppx + pw - sb_w - sb_pad_r;
            f32 sb_y        = list_top;
            f32 sb_radius   = sb_w * 0.5f;
            Color track     = { t->sidebar_fg.r, t->sidebar_fg.g, t->sidebar_fg.b,
                                0.10f * panel_alpha };
            Color thumb     = { ui_accent_clr.r, ui_accent_clr.g, ui_accent_clr.b,
                                app->palette_sb_dragging ? 0.85f * panel_alpha
                                                         : 0.55f * panel_alpha };
            renderer_draw_rrect_simple(r, sb_x, sb_y, sb_w, list_h, track, sb_radius);

            f32 frac = (f32)max_visible / (f32)count;
            if (frac > 1.0f) frac = 1.0f;
            f32 thumb_h = list_h * frac;
            f32 min_thumb = 24.0f * dpi;
            if (thumb_h < min_thumb) thumb_h = min_thumb;
            if (thumb_h > list_h)    thumb_h = list_h;
            f32 thumb_range = list_h - thumb_h;
            f32 t_pos = (scroll_max > 0)
                ? ((f32)app->palette_scroll / (f32)scroll_max) : 0.0f;
            f32 thumb_y = sb_y + thumb_range * t_pos;
            renderer_draw_rrect_simple(r, sb_x, thumb_y, sb_w, thumb_h, thumb, sb_radius);

            /* Cache for click/drag (slightly inflated x range so the
             * click target isn't a 5-pixel hairline). */
            app->palette_sb_x          = sb_x - 6.0f * dpi;
            app->palette_sb_w          = sb_w + 12.0f * dpi;
            app->palette_sb_track_y    = sb_y;
            app->palette_sb_track_h    = list_h;
            app->palette_sb_thumb_h    = thumb_h;
            app->palette_sb_scroll_max = scroll_max;
        } else {
            app->palette_sb_scroll_max = 0;
        }

        renderer_flush_rects(r);
        renderer_flush_rrects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    palette_done: ;
    }
    }   /* close palette open-anim wrapper */

    {
        static bool s_broadcast_was_open = false;
        static Anim s_broadcast_open_anim  = {0};
        static Anim s_broadcast_close_anim = {0};
        f32 dpi = app->dpi_scale;
        f32 panel_scale, panel_alpha, panel_yoff;
        bool render_broadcast = modal_anim_progress(
            app->broadcast_overlay_active,
            &s_broadcast_open_anim, &s_broadcast_close_anim,
            &s_broadcast_was_open, dpi, MODAL_OPEN_DUR_FAST,
            &panel_scale, &panel_alpha, &panel_yoff);
    if (render_broadcast) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;

        i32 row_count = 0;
        for (i32 i = 0; i < app->tab_count; i++) {
            if (app->tabs[i].session) row_count++;
            if (app->tabs[i].session2) row_count++;
        }

        f32 row_h = ui_ch + 8 * dpi;
        i32 visible_rows = row_count < 12 ? row_count : 12;
        f32 pw = 520 * dpi;
        f32 ph = 132 * dpi + visible_rows * row_h;
        f32 ppx = ((f32)app->fb_width - pw) / 2;
        f32 ppy = app->tab_bar_height + 28 * dpi;
        f32 sx = ppx + (pw - pw * panel_scale) * 0.5f;
        f32 sy = ppy + (ph - ph * panel_scale) * 0.5f - panel_yoff * 0.8f;
        ppx = sx; ppy = sy; pw *= panel_scale; ph *= panel_scale;

        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height, (Color){0,0,0,0.35f * panel_alpha});
        f32 panel_radius = 10.0f * dpi;
        Color panel_bd = t->border; panel_bd.a = fmaxf(panel_bd.a, 0.7f) * panel_alpha;
        renderer_draw_rrect(r,
            ppx - 1.0f * dpi, ppy - 1.0f * dpi,
            pw + 2.0f * dpi, ph + 2.0f * dpi, panel_bd,
            panel_radius + 1.0f, panel_radius + 1.0f,
            panel_radius + 1.0f, panel_radius + 1.0f,
            18.0f * dpi, 0.40f * panel_alpha, 0.0f, 8.0f * dpi);
        ChromePalette _cp_bc = chrome_palette_for(t);
        Color panel_bg = _cp_bc.surface_neutral; panel_bg.a = 0.98f * panel_alpha;
        renderer_draw_rrect_simple(r, ppx, ppy, pw, ph, panel_bg, panel_radius);
        /* Top accent strip — theme-driven so warm themes don't get a
         * raw blue stripe on top of a warm panel. */
        Color _bc_acc = theme_ui_accent(t);
        renderer_draw_rrect(r, ppx, ppy, pw, 1.0f * dpi,
            (Color){_bc_acc.r, _bc_acc.g, _bc_acc.b, 0.85f * panel_alpha},
            panel_radius, panel_radius, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f);
        renderer_flush_rects(r);
        renderer_flush_rrects(r);

        Color hdr_fg  = t->fg;          hdr_fg.a  *= panel_alpha;
        Color hint_fg = t->sidebar_fg;  hint_fg.a *= panel_alpha;
        draw_text_ex(r, "Broadcast Targets", ppx + 14 * dpi, ppy + 12 * dpi,
                     hdr_fg, 32, ui_cw);
        draw_text_ex(r, "Focused pane always receives input.", ppx + 14 * dpi,
                     ppy + 12 * dpi + ui_ch + 4 * dpi,
                     hint_fg, 40, ui_cw);

        f32 list_y = ppy + 48 * dpi;
        Tab *active = app_active_tab(app);
        i32 active_pane = active ? active->active_pane : 0;
        i32 row = 0;

        for (i32 i = 0; i < app->tab_count && row < visible_rows; i++) {
            for (i32 pane = 0; pane < 2 && row < visible_rows; pane++) {
                Session *s = (pane == 0) ? app->tabs[i].session : app->tabs[i].session2;
                if (!s) continue;

                bool selected = app->broadcast_targets[i][pane];
                bool focused = (i == app->active_tab && pane == active_pane);
                f32 ry = list_y + row * row_h;
                Color row_bg = focused
                    ? (Color){t->sidebar_active.r, t->sidebar_active.g, t->sidebar_active.b, 0.7f * panel_alpha}
                    : (Color){t->sidebar_hover.r, t->sidebar_hover.g, t->sidebar_hover.b, 0.55f * panel_alpha};
                renderer_draw_rrect_simple(r, ppx + 12 * dpi, ry, pw - 24 * dpi,
                                   row_h - 4 * dpi, row_bg, 6 * dpi);

                f32 cb = 14 * dpi;
                f32 cbx = ppx + 20 * dpi;
                f32 cby = ry + (row_h - cb) / 2 - 2 * dpi;
                Color cb_color = selected ? theme_ui_accent(t) : t->tab_bg;
                cb_color.a *= panel_alpha;
                renderer_draw_rrect_simple(r, cbx, cby, cb, cb, cb_color, 4 * dpi);
                if (selected) {
                    /* Checkmark via SF Symbol — replaces the ASCII 'x'
                     * which read as "delete" in a positive-action
                     * (selected) state. */
                    f32 _cm = cb * 0.78f;
                    icon_draw(r, ICON_CHECK,
                              cbx + (cb - _cm) * 0.5f,
                              cby + (cb - _cm) * 0.5f,
                              _cm, (Color){1, 1, 1, panel_alpha});
                }

                char label[192];
                char display_title[160];
                tab_format_display_title(&app->tabs[i], display_title, (i32)sizeof(display_title));
                if (app->tabs[i].session2) {
                    snprintf(label, sizeof(label), "%s [%s]",
                             display_title,
                             pane == 0 ? "main" : "split");
                } else {
                    snprintf(label, sizeof(label), "%s", display_title);
                }
                Color row_fg = t->fg; row_fg.a *= panel_alpha;
                draw_text_ex(r, label, cbx + cb + 10 * dpi, ry + (row_h - ui_ch) / 2 - 1 * dpi,
                             row_fg, 40, ui_cw);

                const char *kind = session_type(s) == SESSION_SSH ? "ssh"
                                  : session_type(s) == SESSION_MOSH ? "mosh" : "local";
                Color kind_fg = focused ? t->ansi[11] : t->sidebar_fg;
                kind_fg.a *= panel_alpha;
                draw_text_ex(r, kind, ppx + pw - 70 * dpi, ry + (row_h - ui_ch) / 2 - 1 * dpi,
                             kind_fg, 8, ui_cw);
                row++;
            }
        }

        f32 btn_y = ppy + ph - 40 * dpi;
        Color btn_pri = theme_ui_accent(t); btn_pri.a *= panel_alpha;
        Color btn_sec = t->sidebar_hover;  btn_sec.a *= panel_alpha;
        Color btn_neu = t->tab_bg;         btn_neu.a *= panel_alpha;
        Color btn_fg  = t->fg;             btn_fg.a  *= panel_alpha;
        f32 all_x  = ppx + 14 * dpi;
        f32 oa_x   = ppx + 90 * dpi;
        f32 done_x = ppx + pw - 84 * dpi;
        /* Rounded pill buttons; rows/checkboxes queued above flush here too. */
        renderer_draw_rrect_simple(r, all_x,  btn_y, 68 * dpi,  24 * dpi, btn_pri, 7 * dpi);
        renderer_draw_rrect_simple(r, oa_x,   btn_y, 104 * dpi, 24 * dpi, btn_sec, 7 * dpi);
        renderer_draw_rrect_simple(r, done_x, btn_y, 68 * dpi,  24 * dpi, btn_neu, 7 * dpi);
        renderer_flush_rrects(r);
        Color all_fg = chrome_legible_on(btn_pri); all_fg.a *= panel_alpha;
        draw_text_ex(r, "All", all_x + 20 * dpi, btn_y + (24 * dpi - ui_ch) / 2,
                     all_fg, 3, ui_cw);
        draw_text_ex(r, "Only Active", oa_x + 12 * dpi, btn_y + (24 * dpi - ui_ch) / 2,
                     btn_fg, 12, ui_cw);
        draw_text_ex(r, "Done", done_x + 14 * dpi, btn_y + (24 * dpi - ui_ch) / 2,
                     btn_fg, 4, ui_cw);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close broadcast_overlay open-anim wrapper */

    /* Search bar overlay (Cmd+F) — animated open/close through the same
     * modal_anim_progress gate the dialogs use, so it fades + scales + slides
     * into place identically. No full-screen dim: it stays a lightweight top
     * bar, just no longer a hard cut. */
    {
        static bool s_search_was_open = false;
        static Anim s_search_open_anim  = {0};
        static Anim s_search_close_anim = {0};
        f32 dpi = app->dpi_scale;
        f32 sb_scale, sb_alpha, sb_yoff;
        bool render_search = modal_anim_progress(
            app->search_active,
            &s_search_open_anim, &s_search_close_anim,
            &s_search_was_open, dpi, MODAL_OPEN_DUR_FAST,
            &sb_scale, &sb_alpha, &sb_yoff);
    if (render_search) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;

        f32 bar_w = 400 * dpi;
        f32 bar_h = 32 * dpi;
        f32 bar_x = ((f32)app->fb_width - bar_w) / 2;
        f32 bar_y = app->tab_bar_height + 8;
        /* Scale about the bar's centre; slide down into place from behind the
         * tab bar (sb_yoff is +10dpi at open, easing to 0). */
        bar_x += (bar_w - bar_w * sb_scale) * 0.5f;
        bar_y += (bar_h - bar_h * sb_scale) * 0.5f - sb_yoff;
        bar_w *= sb_scale;
        bar_h *= sb_scale;

        Color bar_bg = (Color){ t->sidebar_hover.r, t->sidebar_hover.g,
                                t->sidebar_hover.b, 0.97f * sb_alpha };
        Color accent = theme_ui_accent(t);
        Color bar_bd = accent; bar_bd.a = fmaxf(bar_bd.a, 0.85f) * sb_alpha;

        /* Floating rounded bar: accent (focused) ring + soft shadow, with an
         * Esc keycap at the right edge instead of a flat bottom underline. */
        f32 ty = bar_y + (bar_h - ui_ch) / 2;
        f32 esc_w = 3.0f * ui_cw + 14.0f * dpi;
        f32 esc_x = bar_x + bar_w - esc_w - 8.0f * dpi;
        draw_panel_chrome(r, bar_x, bar_y, bar_w, bar_h, bar_bg, bar_bd,
                          10.0f * dpi, dpi, 14.0f * dpi, 0.30f * sb_alpha);
        draw_keycap(r, esc_x, ty, "Esc", t, ui_cw, ui_ch, dpi);
        renderer_flush_rrects(r);

        /* Accent caret, clamped so it never slides under the Esc keycap. */
        Color label_fg = t->sidebar_fg; label_fg.a *= sb_alpha;
        Color query_fg = t->fg;         query_fg.a *= sb_alpha;
        f32 cur_x = bar_x + 12.0f * dpi + (6 + app->search_query_len) * ui_cw;
        f32 cur_max = esc_x - 4.0f * dpi;
        if (cur_x > cur_max) cur_x = cur_max;
        Color caret = accent; caret.a = fmaxf(caret.a, 0.85f) * sb_alpha;
        renderer_draw_rect(r, cur_x, ty, 2.0f * dpi, ui_ch, caret);
        renderer_flush_rects(r);

        draw_text_ex(r, "Find: ", bar_x + 12.0f * dpi, ty, label_fg, 6, ui_cw);
        draw_text_ex(r, app->search_query, bar_x + 12.0f * dpi + 6 * ui_cw, ty,
                     query_fg, 36, ui_cw);

        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }

    {
        static bool s_pf_was_open = false;
        static Anim s_pf_open_anim  = {0};
        static Anim s_pf_close_anim = {0};
        f32 dpi = app->dpi_scale;
        f32 panel_scale, panel_alpha, panel_yoff;
        bool render_pf = modal_anim_progress(
            app->port_forward_dialog_active,
            &s_pf_open_anim, &s_pf_close_anim,
            &s_pf_was_open, dpi, MODAL_OPEN_DUR_FAST,
            &panel_scale, &panel_alpha, &panel_yoff);
    if (render_pf) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;

        Session *ps = app_focused_session(app);
        bool available = ps && session_type(ps) == SESSION_SSH &&
                         session_status(ps) == SESSION_CONNECTED;
        i32 list_count = available ? session_local_forward_count(ps) : 0;
        i32 visible = list_count < 8 ? list_count : 8;

        f32 dw = 520 * dpi;
        f32 dh = 222 * dpi + visible * (ui_ch + 8 * dpi);
        f32 dx = ((f32)app->fb_width - dw) / 2;
        f32 dy = ((f32)app->fb_height - dh) / 2;
        f32 sx = dx + (dw - dw * panel_scale) * 0.5f;
        f32 sy = dy + (dh - dh * panel_scale) * 0.5f - panel_yoff * 0.8f;
        dx = sx; dy = sy; dw *= panel_scale; dh *= panel_scale;

        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height, (Color){0,0,0,0.5f * panel_alpha});
        renderer_flush_rects(r);

        /* Panel shell mirrors the Settings panel — see create_theme /
         * SSH dialogs above for rationale. */
        ChromePalette cp = chrome_palette_for(t);
        f32 panel_radius = 12.0f * dpi;
        f32 hdr_h_pf = 30 * dpi;
        Color panel_bg = t->bg;
        Color panel_bd = t->border;
        panel_bd.a = (panel_bd.a > 0.05f ? panel_bd.a : 1.0f) * panel_alpha;
        panel_bg.a = panel_alpha;
        renderer_draw_rrect_bordered(r,
            dx, dy, dw, dh,
            panel_bg, panel_bd, fmaxf(1.0f, dpi),
            panel_radius, panel_radius, panel_radius, panel_radius,
            24.0f * dpi, 0.35f * panel_alpha, 0.0f, 6.0f * dpi);
        renderer_flush_rrects(r);

        Color hairline = cp.divider_subtle; hairline.a *= panel_alpha;
        renderer_draw_rect(r,
            dx + panel_radius * 0.5f, dy + hdr_h_pf - fmaxf(1.0f, dpi),
            dw - panel_radius, fmaxf(1.0f, dpi), hairline);
        renderer_flush_rects(r);

        Color title_fg = t->fg; title_fg.a *= panel_alpha;
        draw_text_ex(r, "Port Forwarding", dx + 12 * dpi, dy + (hdr_h_pf - ui_ch) / 2,
                     title_fg, 16, ui_cw);

        f32 cy = dy + 42 * dpi;
        Color lbl = t->sidebar_fg;     lbl.a *= panel_alpha;
        Color val = t->fg;             val.a *= panel_alpha;
        Color active_border = cp.btn_primary_bg; active_border.a *= panel_alpha;
        Color field_bg_pf   = cp.surface_sunken; field_bg_pf.a   *= panel_alpha;
        Color pf_focus = theme_ui_accent(t); pf_focus.a = fmaxf(pf_focus.a, 0.90f) * panel_alpha;
        Color pf_idle  = t->border;          pf_idle.a  = fmaxf(pf_idle.a, 0.45f) * panel_alpha;
        Color mode_sel_bg   = cp.btn_primary_bg; mode_sel_bg.a   *= panel_alpha;
        Color mode_sel_fg   = cp.btn_primary_fg; mode_sel_fg.a   *= panel_alpha;
        Color mode_idle_bg  = cp.btn_secondary_bg; mode_idle_bg.a *= panel_alpha;
        Color mode_idle_fg  = cp.btn_secondary_fg; mode_idle_fg.a *= panel_alpha;
        Color dim_lbl       = (Color){ lbl.r, lbl.g, lbl.b, lbl.a * 0.55f };

        renderer_draw_rrect_simple(r, dx + 16 * dpi, cy, 78 * dpi, 24 * dpi,
                                   app->port_forward_mode == 0 ? mode_sel_bg : mode_idle_bg,
                                   12.0f * dpi);
        renderer_draw_rrect_simple(r, dx + 100 * dpi, cy, 82 * dpi, 24 * dpi,
                                   app->port_forward_mode == 1 ? mode_sel_bg : mode_idle_bg,
                                   12.0f * dpi);
        renderer_flush_rrects(r);
        draw_text_ex(r, "Local", dx + 16 * dpi + 18 * dpi, cy + (24 * dpi - ui_ch) / 2,
                     app->port_forward_mode == 0 ? mode_sel_fg : mode_idle_fg, 5, ui_cw);
        draw_text_ex(r, "SOCKS5", dx + 100 * dpi + 12 * dpi, cy + (24 * dpi - ui_ch) / 2,
                     app->port_forward_mode == 1 ? mode_sel_fg : mode_idle_fg, 6, ui_cw);
        cy += 36 * dpi;

        draw_text_ex(r, "Local Port:", dx + 16 * dpi, cy + 6 * dpi, lbl, 10, ui_cw);
        draw_input_field(r, dx + 110 * dpi, cy, 88 * dpi, 28 * dpi, field_bg_pf,
                         app->port_forward_field == 0 ? pf_focus : pf_idle, dpi);
        draw_text_ex(r, app->port_forward_local_port, dx + 114 * dpi, cy + (28 * dpi - ui_ch) / 2,
                     val, 8, ui_cw);
        cy += 36 * dpi;

        draw_text_ex(r, "Remote Host:", dx + 16 * dpi, cy + 6 * dpi, lbl, 11, ui_cw);
        draw_input_field(r, dx + 110 * dpi, cy, 250 * dpi, 28 * dpi, field_bg_pf,
                         (app->port_forward_mode == 0 && app->port_forward_field == 1)
                             ? pf_focus : pf_idle, dpi);
        draw_text_ex(r, app->port_forward_mode == 1 ? "remote host from client"
                                                    : app->port_forward_remote_host,
                     dx + 114 * dpi, cy + (28 * dpi - ui_ch) / 2,
                     app->port_forward_mode == 1 ? dim_lbl : val, 28, ui_cw);

        draw_text_ex(r, "Port:", dx + 370 * dpi, cy + 6 * dpi, lbl, 5, ui_cw);
        draw_input_field(r, dx + 410 * dpi, cy, 80 * dpi, 28 * dpi, field_bg_pf,
                         (app->port_forward_mode == 0 && app->port_forward_field == 2)
                             ? pf_focus : pf_idle, dpi);
        draw_text_ex(r, app->port_forward_mode == 1 ? "client"
                                                    : app->port_forward_remote_port,
                     dx + 414 * dpi, cy + (28 * dpi - ui_ch) / 2,
                     app->port_forward_mode == 1 ? dim_lbl : val, 8, ui_cw);
        cy += 40 * dpi;

        if (available) {
            Color add_bg = cp.btn_primary_bg; add_bg.a *= panel_alpha;
            Color add_fg = cp.btn_primary_fg; add_fg.a *= panel_alpha;
            renderer_draw_rrect_simple(r, dx + dw - 112 * dpi, dy + 74 * dpi,
                                       92 * dpi, 28 * dpi, add_bg, 8.0f * dpi);
            renderer_flush_rrects(r);
            draw_text_ex(r, "Add Rule", dx + dw - 112 * dpi + 12 * dpi,
                         dy + 74 * dpi + (28 * dpi - ui_ch) / 2,
                         add_fg, 8, ui_cw);
        } else {
            draw_text_ex(r, "Active pane must be a connected SSH session.",
                         dx + 16 * dpi, cy, t->ansi[3], 44, ui_cw);
            cy += ui_ch + 8 * dpi;
        }

        f32 list_y = dy + 158 * dpi;
        draw_text_ex(r, "Active Rules", dx + 16 * dpi, list_y - 20 * dpi,
                     lbl, 12, ui_cw);

        for (i32 i = 0; i < visible; i++) {
            LocalForwardInfo info;
            if (!session_local_forward_get(ps, i, &info)) continue;

            f32 ry = list_y + i * (ui_ch + 8 * dpi);
            renderer_draw_rect(r, dx + 16 * dpi, ry - 2 * dpi, dw - 32 * dpi, ui_ch + 6 * dpi,
                              (Color){t->sidebar_hover.r, t->sidebar_hover.g, t->sidebar_hover.b, 0.7f});

            char row[256];
            if (info.type == 2) {
                snprintf(row, sizeof(row), "SOCKS5  127.0.0.1:%d", info.local_port);
            } else {
                snprintf(row, sizeof(row), "LOCAL   127.0.0.1:%d -> %s:%d",
                         info.local_port, info.remote_host, info.remote_port);
            }
            draw_text_ex(r, row, dx + 24 * dpi, ry, val, 48, ui_cw);
            draw_text_ex(r, "Remove", dx + dw - 76 * dpi, ry,
                         t->ansi[9], 6, ui_cw);
        }

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close port_forward open-anim wrapper */

    /* SSH connect dialog */
    {
        static bool s_ssh_was_open = false;
        static Anim s_ssh_open_anim  = {0};
        static Anim s_ssh_close_anim = {0};
        f32 dpi = app->dpi_scale;
        f32 panel_scale, panel_alpha, panel_yoff;
        bool render_ssh = modal_anim_progress(
            app->ssh_dialog_active,
            &s_ssh_open_anim, &s_ssh_close_anim,
            &s_ssh_was_open, dpi, MODAL_OPEN_DUR_FAST,
            &panel_scale, &panel_alpha, &panel_yoff);
    if (render_ssh) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;

        /* Larger canvas with breathing room. Tokens: 20 px outer padding,
         * 10 px gap between fields, 14 px gap before the options row. */
        f32 pad     = 20 * dpi;
        f32 gap     = 10 * dpi;
        f32 field_h = 30 * dpi;
        f32 label_h = 16 * dpi;
        f32 hdr_h   = 40 * dpi;

        /* Layout: two columns when there's any recent history — left column
         * keeps the existing form, right column renders a clickable list of
         * recent connections. The right column folds away when empty so a
         * pristine install still sees the compact form. */
        bool show_recent = app->ssh_history_count > 0;
        f32 recent_w   = show_recent ? 220 * dpi : 0;
        f32 recent_pad = show_recent ? pad       : 0;
        f32 left_w     = 500 * dpi;

        /* Content height: header + pad + 3 full-width fields + Port/Pass row
         * + separator gap + options row + hint + button row + bottom pad. */
        f32 row_full = label_h + 4*dpi + field_h;        /* label + small gap + field */
        f32 body_h = pad
                   + row_full                 /* Host (Device for serial) */
                   + gap + row_full           /* Port/Baud (+ Password row, SSH) */
                   + gap + ui_ch              /* hint */
                   + gap*2 + 32*dpi           /* button row */
                   + pad;
        if (app->ssh_dialog_proto == 0) {
            body_h += gap + row_full          /* User row */
                    + gap + gap               /* separator */
                    + field_h + gap;          /* options row */
        } else {
            body_h += gap/2;                  /* port-row trailing half-gap */
        }
        f32 dw = left_w + recent_pad + recent_w;
        f32 dh = hdr_h + body_h;
        f32 dx = ((f32)app->fb_width - dw) / 2;
        f32 dy = ((f32)app->fb_height - dh) / 2;
        f32 sx = dx + (dw - dw * panel_scale) * 0.5f;
        f32 sy = dy + (dh - dh * panel_scale) * 0.5f - panel_yoff;
        dx = sx; dy = sy; dw *= panel_scale; dh *= panel_scale;

        /* Dim backdrop — fades with the modal open animation. */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                          (Color){0,0,0,0.5f * panel_alpha});
        renderer_flush_rects(r);

        /* Panel shell — same chrome the Settings panel uses: a single
         * bordered SDF call with theme.bg fill + theme.border stroke,
         * plus a hairline divider beneath the title.  Drops the
         * previous raised header band + ansi[4] accent strip so the
         * dialog reads as part of the Settings-style surface family. */
        ChromePalette cp = chrome_palette_for(t);
        f32 panel_radius = 12.0f * dpi;
        Color panel_bg = t->bg;
        Color panel_bd = t->border;
        panel_bd.a = (panel_bd.a > 0.05f ? panel_bd.a : 1.0f) * panel_alpha;
        panel_bg.a = panel_alpha;
        renderer_draw_rrect_bordered(r,
            dx, dy, dw, dh,
            panel_bg, panel_bd, fmaxf(1.0f, dpi),
            panel_radius, panel_radius, panel_radius, panel_radius,
            24.0f * dpi, 0.35f * panel_alpha, 0.0f, 6.0f * dpi);
        renderer_flush_rrects(r);

        Color hairline = cp.divider_subtle; hairline.a *= panel_alpha;
        renderer_draw_rect(r,
            dx + panel_radius * 0.5f, dy + hdr_h - fmaxf(1.0f, dpi),
            dw - panel_radius, fmaxf(1.0f, dpi), hairline);
        renderer_flush_rects(r);

        Color title_fg = t->fg; title_fg.a *= panel_alpha;
        draw_text_ex(r, app->ssh_dialog_proto == 1 ? "Telnet Connect"
                      : app->ssh_dialog_proto == 2 ? "Serial Connect (device + baud)"
                      : "SSH Connect",
                     dx + pad, dy + (hdr_h - ui_ch)/2,
                     title_fg, 32, ui_cw);

        Color lbl         = t->sidebar_fg;     lbl.a *= panel_alpha;
        Color val         = t->fg;             val.a *= panel_alpha;
        Color dim         = (Color){ lbl.r, lbl.g, lbl.b, lbl.a * 0.55f };
        Color active      = cp.btn_primary_bg; active.a *= panel_alpha;
        Color field_bg    = cp.surface_sunken; field_bg.a *= panel_alpha;
        Color field_focus = theme_ui_accent(t); field_focus.a = fmaxf(field_focus.a, 0.90f) * panel_alpha;
        Color field_idle  = t->border;          field_idle.a  = fmaxf(field_idle.a, 0.45f) * panel_alpha;
        Color sep         = (Color){ lbl.r, lbl.g, lbl.b, lbl.a * 0.18f };

        /* Caret blink — same smooth sine curve as the terminal cursor so
         * dialog and terminal animations feel part of the same language. */
        f32 caret_alpha;
        {
            f64 bnow = platform_time_sec();
            f32 phase = (f32)fmod(bnow * 2.0, 2.0 * 3.14159265);
            caret_alpha = 0.15f + 0.85f * (0.5f + 0.5f * sinf(phase));
        }
        Color caret_clr = (Color){ active.r, active.g, active.b, active.a * caret_alpha };

        f32 y = dy + hdr_h + pad;

        /* Helper pattern inlined: one labelled field at (x, y, w).
         * When the field is focused we also render a blinking caret after
         * the last visible character (or at the left edge when empty). */
        #define LIU_FIELD(lx, ly, lw, fi, text, maxlen, caption, placeholder) do { \
            draw_text_ex(r, caption, (lx), (ly), lbl,                              \
                         (i32)strlen(caption), ui_cw);                             \
            f32 _field_top = (ly) + label_h + 4*dpi;                               \
            f32 _text_y = _field_top + (field_h - ui_ch)/2;                        \
            bool _focused = (app->ssh_field == (fi));                              \
            draw_input_field(r, (lx), _field_top, (lw), field_h, field_bg,         \
                             _focused ? field_focus : field_idle, dpi);            \
            const char *_tx = (text);                                              \
            i32 _tlen = (i32)strlen(_tx);                                          \
            bool _empty = (_tlen == 0);                                            \
            draw_text_ex(r, _empty ? (placeholder) : _tx,                          \
                         (lx) + 10*dpi, _text_y,                                   \
                         _empty ? dim : val, (maxlen), ui_cw);                     \
            if (_focused) {                                                        \
                f32 _cx = (lx) + 10*dpi + (_empty ? 0 : (f32)_tlen * ui_cw);       \
                /* Clamp caret inside the field so long text doesn't push         \
                 * it past the right edge. */                                      \
                f32 _cx_max = (lx) + (lw) - 4*dpi;                                 \
                if (_cx > _cx_max) _cx = _cx_max;                                  \
                renderer_draw_rect(r, _cx, _field_top + 4*dpi,                     \
                                  2*dpi, field_h - 8*dpi, caret_clr);              \
            }                                                                      \
        } while (0)

        /* Host (doubles as the device path in Serial mode). */
        LIU_FIELD(dx + pad, y, left_w - 2*pad, 0, app->ssh_host, 48,
                  app->ssh_dialog_proto == 2 ? "Device" : "Host",
                  app->ssh_dialog_proto == 2 ? "/dev/tty.usbserial"
                                             : "host.example.com");
        y += row_full + gap;

        /* User — SSH only; telnet logs in in-band, serial has no login. */
        if (app->ssh_dialog_proto == 0) {
            LIU_FIELD(dx + pad, y, left_w - 2*pad, 1, app->ssh_user, 48,
                      "User", "username");
            y += row_full + gap;
        }

        /* Port + Password on the same row. Port is fixed-width on the left,
         * password takes the remaining width to the right. */
        {
            f32 port_w = 96 * dpi;
            f32 pwd_x  = dx + pad + port_w + gap;
            f32 pwd_w  = left_w - 2*pad - port_w - gap;
            LIU_FIELD(dx + pad, y, port_w, 2,
                      app->ssh_port[0] ? app->ssh_port : "",
                      6,
                      app->ssh_dialog_proto == 2 ? "Baud" : "Port",
                      app->ssh_dialog_proto == 2 ? "115200"
                    : app->ssh_dialog_proto == 1 ? "23" : "22");

            if (app->ssh_dialog_proto != 0) goto ssh_pwd_done;
            /* Password field — masked display */
            draw_text_ex(r, "Password", pwd_x, y, lbl, 8, ui_cw);
            f32 pwd_top = y + label_h + 4*dpi;
            bool pwd_focused = (app->ssh_field == 3);
            draw_input_field(r, pwd_x, pwd_top, pwd_w, field_h, field_bg,
                             pwd_focused ? field_focus : field_idle, dpi);
            i32 pw_len = (i32)strlen(app->ssh_password);
            if (pw_len == 0) {
                draw_text_ex(r, "... or use key auth", pwd_x + 10*dpi,
                             pwd_top + (field_h - ui_ch)/2,
                             dim, 19, ui_cw);
            } else {
                char masked[128];
                i32 show = pw_len < (i32)sizeof(masked)-1 ? pw_len : (i32)sizeof(masked)-1;
                for (i32 i = 0; i < show; i++) masked[i] = '*';
                masked[show] = '\0';
                draw_text_ex(r, masked, pwd_x + 10*dpi,
                             pwd_top + (field_h - ui_ch)/2,
                             val, show, ui_cw);
            }
            if (pwd_focused) {
                f32 caret_x = pwd_x + 10*dpi + (pw_len > 0 ? (f32)pw_len * ui_cw : 0);
                f32 caret_x_max = pwd_x + pwd_w - 4*dpi;
                if (caret_x > caret_x_max) caret_x = caret_x_max;
                renderer_draw_rect(r, caret_x, pwd_top + 4*dpi,
                                  2*dpi, field_h - 8*dpi, caret_clr);
            }
ssh_pwd_done:;
        }
        y += row_full + gap + gap/2;

        /* Thin separator between fields and options — and the option
         * checkboxes themselves — only exist for SSH. */
        if (app->ssh_dialog_proto == 0) {
        renderer_draw_rect(r, dx + pad, y, left_w - 2*pad, 1*dpi, sep);
        renderer_flush_rects(r);
        y += gap;

        /* Options row: checkboxes */
        {
            Color chk_on  = active;
            Color chk_off = t->tab_bg;
            f32 chk_sz = 16 * dpi;
            f32 opt_y = y + (field_h - chk_sz)/2;

            /* Use Mosh */
            renderer_draw_rect(r, dx + pad, opt_y, chk_sz, chk_sz,
                              app->ssh_use_mosh ? chk_on : chk_off);
            renderer_draw_rect(r, dx + pad, opt_y, chk_sz, chk_sz,
                              (Color){lbl.r, lbl.g, lbl.b, 0.3f});
            renderer_draw_rect(r, dx + pad + 1*dpi, opt_y + 1*dpi,
                              chk_sz - 2*dpi, chk_sz - 2*dpi,
                              app->ssh_use_mosh ? chk_on : chk_off);
            if (app->ssh_use_mosh) {
                f32 _cm = chk_sz * 0.78f;
                icon_draw(r, ICON_CHECK,
                          dx + pad + (chk_sz - _cm) * 0.5f,
                          opt_y + (chk_sz - _cm) * 0.5f,
                          _cm, (Color){1,1,1,1});
            }
            draw_text_ex(r, "Use Mosh",
                         dx + pad + chk_sz + 8*dpi,
                         opt_y + (chk_sz - ui_ch)/2, lbl, 8, ui_cw);

            /* X11 Forward */
            f32 x11_x = dx + pad + 170 * dpi;
            renderer_draw_rect(r, x11_x, opt_y, chk_sz, chk_sz,
                              app->ssh_forward_x11 ? chk_on : chk_off);
            renderer_draw_rect(r, x11_x, opt_y, chk_sz, chk_sz,
                              (Color){lbl.r, lbl.g, lbl.b, 0.3f});
            renderer_draw_rect(r, x11_x + 1*dpi, opt_y + 1*dpi,
                              chk_sz - 2*dpi, chk_sz - 2*dpi,
                              app->ssh_forward_x11 ? chk_on : chk_off);
            if (app->ssh_forward_x11) {
                f32 _cm = chk_sz * 0.78f;
                icon_draw(r, ICON_CHECK,
                          x11_x + (chk_sz - _cm) * 0.5f,
                          opt_y + (chk_sz - _cm) * 0.5f,
                          _cm, (Color){1,1,1,1});
            }
            draw_text_ex(r, "X11 Forwarding",
                         x11_x + chk_sz + 8*dpi,
                         opt_y + (chk_sz - ui_ch)/2, lbl, 14, ui_cw);
        }
        y += field_h + gap;
        }   /* proto == 0 (SSH) */

        /* Hint row — keyboard shortcuts, muted. Swapped for an error banner
         * when submit refused the request (e.g. empty password). The banner
         * uses a red-tinted strip so it's legible even with the accent-color
         * Connect button right underneath. */
        bool show_err = app->ssh_dialog_error[0] &&
                        platform_time_sec() < app->ssh_dialog_error_until;
        if (show_err) {
            Color err_bg   = { 0.62f, 0.12f, 0.14f, 0.18f };
            Color err_bar  = { 0.95f, 0.35f, 0.35f, 1.0f };
            Color err_fg   = { 1.0f,  0.80f, 0.80f, 1.0f };
            f32 err_h = ui_ch + 10 * dpi;
            f32 err_w = left_w - 2*pad;
            renderer_draw_rect(r, dx + pad, y - 4*dpi, err_w, err_h, err_bg);
            renderer_draw_rect(r, dx + pad, y - 4*dpi, 3*dpi, err_h, err_bar);
            renderer_flush_rects(r);
            i32 elen = (i32)strlen(app->ssh_dialog_error);
            i32 emax = (i32)((err_w - 20*dpi) / ui_cw);
            if (elen > emax) elen = emax;
            /* Glyph marker: stylised warning triangle character (ASCII '!'). */
            draw_text_ex(r, "!", dx + pad + 10*dpi, y - 4*dpi + (err_h - ui_ch)/2,
                         err_bar, 1, ui_cw);
            draw_text_ex(r, app->ssh_dialog_error,
                         dx + pad + 10*dpi + 2*ui_cw,
                         y - 4*dpi + (err_h - ui_ch)/2,
                         err_fg, elen, ui_cw);
        } else {
            draw_text_ex(r, "Enter: Connect   Tab: Next field   Esc: Cancel",
                         dx + pad, y, dim, 44, ui_cw);
        }
        y += ui_ch + gap;

        /* Password field errored? Draw a red underline so the user sees
         * where to look when the banner mentions "Password required". */
        if (show_err && app->ssh_field == 3) {
            f32 port_w = 96 * dpi;
            f32 pwd_x  = dx + pad + port_w + gap;
            f32 pwd_w  = left_w - 2*pad - port_w - gap;
            f32 pwd_err_y = dy + hdr_h + pad + 2*row_full + 2*gap + label_h
                          + 4*dpi + field_h - 2*dpi;
            Color err_bar = { 0.95f, 0.35f, 0.35f, 1.0f };
            renderer_draw_rect(r, pwd_x, pwd_err_y, pwd_w, 2*dpi, err_bar);
        }

        /* Connect button — pill matching the Settings panel's primary
         * affordance, painted with the chrome-palette tokens so the
         * label stays legible on every theme. */
        f32 btn_h = 32 * dpi;
        f32 btn_w = 120 * dpi;
        f32 btn_y = dy + dh - pad - btn_h;
        f32 btn_x = dx + left_w - pad - btn_w;
        Color conn_bg = cp.btn_primary_bg; conn_bg.a *= panel_alpha;
        Color conn_fg = cp.btn_primary_fg; conn_fg.a *= panel_alpha;
        renderer_draw_rrect_simple(r, btn_x, btn_y, btn_w, btn_h, conn_bg, 8.0f * dpi);
        renderer_flush_rrects(r);
        draw_text_ex(r, "Connect",
                     btn_x + (btn_w - 7*ui_cw)/2,
                     btn_y + (btn_h - ui_ch)/2,
                     conn_fg, 7, ui_cw);

        #undef LIU_FIELD

        /* Right column — Recent connections list. Card-per-row layout with
         * hover + selected highlight states, a monogram avatar, and a muted
         * metadata line ("port · auth · 3h ago"). Passwords are never stored
         * in history (the table carries hostname/user/port/auth/timestamp
         * only), and selection always clears the password field so the user
         * re-enters it; vault entries with encrypted credentials live in a
         * separate table and aren't touched by this list. */
        if (show_recent) {
            f32 rx = dx + left_w;
            f32 ry = dy + hdr_h + pad;
            f32 rw = recent_w;
            f32 rh = dh - hdr_h - pad - pad;
            f32 inner_x = rx + 10*dpi;
            f32 card_w  = rw - 14*dpi;

            /* Vertical rule separating the form from the list */
            renderer_draw_rect(r, rx - 1*dpi, dy + hdr_h, 1*dpi, dh - hdr_h, sep);

            /* Section header: "Recent Sessions" + count badge */
            draw_text_ex(r, "Recent Sessions", inner_x, ry,
                         lbl, 15, ui_cw);
            {
                char count_str[12];
                snprintf(count_str, sizeof(count_str), "%d",
                         app->ssh_history_count);
                i32 clen = (i32)strlen(count_str);
                f32 badge_w = (f32)clen * ui_cw + 10*dpi;
                f32 badge_h = ui_ch + 2*dpi;
                f32 badge_x = rx + rw - badge_w - 10*dpi;
                f32 badge_y = ry - 1*dpi;
                Color badge_bg = { lbl.r, lbl.g, lbl.b, 0.12f };
                renderer_draw_rect(r, badge_x, badge_y, badge_w, badge_h, badge_bg);
                draw_text_ex(r, count_str,
                             badge_x + 5*dpi,
                             badge_y + (badge_h - ui_ch) * 0.5f,
                             lbl, clen, ui_cw);
            }
            ry += label_h + 6*dpi;
            /* Thin divider under the header */
            renderer_draw_rect(r, inner_x, ry, card_w, 1*dpi, sep);
            ry += 8*dpi;

            f32 card_h = 48 * dpi;   /* two-line card */
            f32 card_gap = 6 * dpi;
            i32 max_visible = (i32)((rh - (label_h + 14*dpi)) / (card_h + card_gap));
            if (max_visible < 0) max_visible = 0;
            i32 nrows = app->ssh_history_count < max_visible
                          ? app->ssh_history_count : max_visible;

            f64 now_t = platform_time_sec();
            for (i32 i = 0; i < nrows; i++) {
                VaultHistoryEntry *h = &app->ssh_history[i];
                f32 iry = ry + (f32)i * (card_h + card_gap);
                bool sel     = (app->ssh_history_selected == i);
                bool hovered = (app->ssh_history_hover    == i);

                /* Card background: hover tint, selected gets active fill. */
                if (sel) {
                    Color fill = { active.r, active.g, active.b, 0.18f };
                    renderer_draw_rect(r, inner_x, iry, card_w, card_h, fill);
                    renderer_draw_rect(r, inner_x, iry, 3*dpi, card_h, active);
                } else if (hovered) {
                    Color fill = { lbl.r, lbl.g, lbl.b, 0.08f };
                    renderer_draw_rect(r, inner_x, iry, card_w, card_h, fill);
                } else {
                    Color fill = { lbl.r, lbl.g, lbl.b, 0.04f };
                    renderer_draw_rect(r, inner_x, iry, card_w, card_h, fill);
                }

                /* Primary line: username (dimmed) + '@' + hostname + :port.
                 * Drawn in two segments so the hostname reads loudest. */
                f32 tx = inner_x + 14*dpi;
                f32 ty = iry + 8*dpi;
                i32 row_chars = (i32)((card_w - 24*dpi) / ui_cw);
                if (row_chars < 6) row_chars = 6;

                char userp[96];
                i32 user_len = 0;
                if (h->username[0]) {
                    snprintf(userp, sizeof(userp), "%s@", h->username);
                    user_len = (i32)strlen(userp);
                }
                char hostp[256];
                snprintf(hostp, sizeof(hostp), "%s", h->hostname);
                if (h->port > 0 && h->port != 22) {
                    usize hl = strlen(hostp);
                    snprintf(hostp + hl, sizeof(hostp) - hl, ":%d", h->port);
                }
                i32 host_len = (i32)strlen(hostp);
                i32 avail = row_chars - user_len;
                if (avail < 4) avail = 4;
                if (host_len > avail) host_len = avail;

                if (user_len > 0) {
                    draw_text_ex(r, userp, tx, ty, dim, user_len, ui_cw);
                }
                draw_text_ex(r, hostp, tx + (f32)user_len * ui_cw, ty,
                             val, host_len, ui_cw);

                /* Secondary line: status dot + auth method + relative time. */
                f32 sy = ty + ui_ch + 4*dpi;
                Color dot = h->succeeded
                              ? (Color){0.30f, 0.82f, 0.45f, 0.95f}
                              : (Color){0.90f, 0.40f, 0.40f, 0.95f};
                renderer_draw_rect(r, tx, sy + (ui_ch - 6*dpi) * 0.5f,
                                   6*dpi, 6*dpi, dot);

                char meta[96]; meta[0] = '\0';
                const char *auth = h->auth_method_str[0] ? h->auth_method_str : "auth";
                f64 dt = now_t - h->timestamp;
                const char *unit; i64 val_t;
                if (dt < 0)               { val_t = 0;          unit = "now";  }
                else if (dt < 60)         { val_t = (i64)dt;    unit = "s ago"; }
                else if (dt < 3600)       { val_t = (i64)(dt/60); unit = "m ago"; }
                else if (dt < 86400)      { val_t = (i64)(dt/3600); unit = "h ago"; }
                else if (dt < 86400*7)    { val_t = (i64)(dt/86400); unit = "d ago"; }
                else if (dt < 86400*30)   { val_t = (i64)(dt/(86400*7)); unit = "w ago"; }
                else                      { val_t = (i64)(dt/(86400*30)); unit = "mo ago"; }
                if (val_t == 0 && strcmp(unit, "now") != 0) val_t = 1;
                if (strcmp(unit, "now") == 0)
                    snprintf(meta, sizeof(meta), "%s · just now", auth);
                else
                    snprintf(meta, sizeof(meta), "%s · %lld%s",
                             auth, (long long)val_t, unit);

                i32 mlen = (i32)strlen(meta);
                i32 mmax = row_chars - 3;  /* leave room for dot */
                if (mmax < 4) mmax = 4;
                if (mlen > mmax) mlen = mmax;
                draw_text_ex(r, meta, tx + 10*dpi, sy, dim, mlen, ui_cw);
            }

            /* "+N more…" footer when the list is truncated */
            if (app->ssh_history_count > nrows) {
                char more[32];
                snprintf(more, sizeof(more), "+%d more session%s",
                         app->ssh_history_count - nrows,
                         app->ssh_history_count - nrows == 1 ? "" : "s");
                f32 mry = ry + (f32)nrows * (card_h + card_gap) + 4*dpi;
                draw_text_ex(r, more, inner_x + 10*dpi, mry,
                             dim, (i32)strlen(more), ui_cw);
            }
        }

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }

    /* IME composition (preedit) overlay near cursor */
    if (app->ime_active && app->ime_preedit[0]) {
        Terminal *ime_t = app_focused_terminal(app);
        if (ime_t) {
            f32 cw = r->font.cell_width;
            f32 ch = r->font.cell_height;
            f32 top_gap = app->config.style.terminal_top_gap * app->dpi_scale;
            f32 px = app->sidebar_width + app->padding + (f32)ime_t->cursor_x * cw;
            f32 py = app->tab_bar_height + top_gap + (f32)ime_t->cursor_y * ch;
            i32 plen = (i32)strlen(app->ime_preedit);
            f32 pw = (f32)plen * cw + 4;
            ChromePalette _cp_ime = chrome_palette_for(app->config.theme);
            Color ime_bg = _cp_ime.surface_raised; ime_bg.a = 0.95f;
            Color ime_underline = theme_ui_accent(app->config.theme);
            renderer_draw_rect(r, px - 2, py, pw, ch, ime_bg);
            renderer_draw_rect(r, px - 2, py + ch - 1, pw, 1, ime_underline);
            renderer_flush_rects(r);
            draw_text(r, app->ime_preedit, px, py,
                      (Color){0.95f, 0.85f, 0.4f, 1.0f}, plen);
            renderer_flush_glyphs(r);
        }
    }
    }   /* close ssh_dialog open-anim wrapper */

    /* Create Theme dialog (Cmd+K → "Create Theme..."). Three layout
     * variants driven by app->create_theme_phase:
     *   0 input        — name + desc + agent picker + Generate
     *   1 running      — spinner + tail of agent stdout + Cancel
     *   2 success      — checkmark + status + Close
     *   3 error        — bang + status + Back / Close
     * The modal is laid out the same way SSH dialog does: a centered
     * rounded panel with header strip and accent line, then a body of
     * labelled fields. */
    {
        static bool s_ct_was_open = false;
        static Anim s_ct_open_anim  = {0};
        static Anim s_ct_close_anim = {0};
        f32 dpi = app->dpi_scale;
        f32 panel_scale, panel_alpha, panel_yoff;
        bool render_ct = modal_anim_progress(
            app->create_theme_active,
            &s_ct_open_anim, &s_ct_close_anim,
            &s_ct_was_open, dpi, MODAL_OPEN_DUR_FAST,
            &panel_scale, &panel_alpha, &panel_yoff);
    if (render_ct) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);
        /* True drawn glyph height (UI text rasterizes to the font's real cell
         * aspect, taller than the nominal ui_ch for fonts like JetBrains Mono)
         * — used to vertically center field text on its underline. */
        f32 ui_gh = (r->font.cell_width > 0.0f)
                  ? ui_cw * (r->font.cell_height / r->font.cell_width) : ui_ch;

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;
        ChromePalette cp = chrome_palette_for(t);

        f32 pad     = 20 * dpi;
        f32 gap     = 10 * dpi;
        f32 field_h = 30 * dpi;
        f32 desc_h  = 64 * dpi;       /* taller — multi-line description (≈3 lines) */
        f32 label_h = 16 * dpi;
        f32 hdr_h   = 40 * dpi;
        f32 dw      = 540 * dpi;

        /* Body height — same in both phases so the panel doesn't
         * resize when the user hits Generate. Phase 1 just places a
         * spinning button in the centre of the same-sized body. */
        f32 body_h = pad
                   + label_h + 4*dpi + field_h    /* Name */
                   + gap + label_h + 4*dpi + desc_h /* Description */
                   + gap + label_h + 4*dpi
                   + 64 * dpi                     /* agent picker row */
                   + gap + ui_ch                  /* hint */
                   + gap*2 + 32*dpi               /* button row */
                   + pad;

        f32 dh = hdr_h + body_h;
        f32 dx = ((f32)app->fb_width - dw) / 2;
        f32 dy = ((f32)app->fb_height - dh) / 2;
        f32 sx = dx + (dw - dw * panel_scale) * 0.5f;
        f32 sy = dy + (dh - dh * panel_scale) * 0.5f - panel_yoff;
        dx = sx; dy = sy; dw *= panel_scale; dh *= panel_scale;

        /* Dim backdrop — fades in with the modal open animation. */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                          (Color){0,0,0,0.5f * panel_alpha});
        renderer_flush_rects(r);

        /* Panel shell mirrors the Settings panel: a single bordered
         * SDF call (flat theme.bg fill + theme.border stroke), then a
         * hairline divider beneath the title. No raised header band,
         * no accent strip — the dialog reads as part of the same
         * Settings-style chrome family rather than its own surface. */
        f32 panel_radius = 12.0f * dpi;
        Color panel_bg = t->bg;
        Color panel_bd = t->border;
        panel_bd.a = (panel_bd.a > 0.05f ? panel_bd.a : 1.0f) * panel_alpha;
        panel_bg.a = panel_alpha;
        renderer_draw_rrect_bordered(r,
            dx, dy, dw, dh,
            panel_bg, panel_bd, fmaxf(1.0f, dpi),
            panel_radius, panel_radius, panel_radius, panel_radius,
            24.0f * dpi, 0.35f * panel_alpha, 0.0f, 6.0f * dpi);
        renderer_flush_rrects(r);

        Color hairline = cp.divider_subtle; hairline.a *= panel_alpha;
        renderer_draw_rect(r,
            dx + panel_radius * 0.5f, dy + hdr_h - fmaxf(1.0f, dpi),
            dw - panel_radius, fmaxf(1.0f, dpi), hairline);
        renderer_flush_rects(r);

        Color title_fg = t->fg; title_fg.a *= panel_alpha;
        bool ct_doc = app->create_theme_doc_mode;
        const char *title = ct_doc ? "Create Note" : "Create Theme";
        draw_text_ex(r, title, dx + pad, dy + (hdr_h - ui_ch)/2,
                     title_fg, (i32)strlen(title), ui_cw);

        Color lbl   = t->sidebar_fg;       lbl.a *= panel_alpha;
        Color val   = t->fg;               val.a *= panel_alpha;
        Color dim   = (Color){ lbl.r, lbl.g, lbl.b, lbl.a * 0.55f };
        Color active = cp.btn_primary_bg;  active.a *= panel_alpha;
        Color ct_focus = theme_ui_accent(t); ct_focus.a = fmaxf(ct_focus.a, 0.90f) * panel_alpha;
        Color ct_idle  = t->border;          ct_idle.a  = fmaxf(ct_idle.a, 0.45f) * panel_alpha;

        /* Caret blink shared with the SSH dialog. */
        f32 caret_alpha;
        {
            f64 bnow = platform_time_sec();
            f32 phase = (f32)fmod(bnow * 2.0, 2.0 * 3.14159265);
            caret_alpha = 0.15f + 0.85f * (0.5f + 0.5f * sinf(phase));
        }
        Color caret_clr = (Color){ active.r, active.g, active.b, active.a * caret_alpha };

        f32 y = dy + hdr_h + pad;

        if (app->create_theme_phase == 0) {
            /* Name field */
            const char *name_lbl = ct_doc ? "Note Title" : "Theme Name";
            draw_text_ex(r, name_lbl, dx + pad, y, lbl, (i32)strlen(name_lbl), ui_cw);
            f32 nf_top = y + label_h + 4*dpi;
            f32 nf_w   = dw - 2*pad;
            f32 nf_text_x = dx + pad;
            f32 nf_text_y = nf_top + (field_h - ui_gh) * 0.5f;
            bool nf_focus = (app->create_theme_field == 0);
            /* Underline-style input: a single baseline rule rather than a boxed
             * field — accent when focused, a dim hairline when idle. Text sits
             * flush with the label, no inner box padding. */
            f32 nf_line_h = (nf_focus ? 1.5f : 1.0f) * dpi;
            renderer_draw_rect(r, nf_text_x, nf_top + field_h - nf_line_h,
                               nf_w, nf_line_h, nf_focus ? ct_focus : ct_idle);
            renderer_flush_rects(r);
            i32 nlen = app->create_theme_name_len;
            bool n_empty = (nlen == 0);
            const char *n_ph = ct_doc ? "Release Plan v0.2" : "Midnight Lagoon";
            const char *n_disp = n_empty ? n_ph : app->create_theme_name;
            i32 n_max = n_empty ? (i32)strlen(n_ph) : nlen;
            draw_text_ex(r, n_disp, nf_text_x, nf_text_y,
                         n_empty ? dim : val, n_max, ui_cw);
            if (nf_focus) {
                f32 cx = nf_text_x + (n_empty ? 0 : (f32)nlen * ui_cw);
                f32 cx_max = nf_text_x + nf_w - 4*dpi;
                if (cx > cx_max) cx = cx_max;
                renderer_draw_rect(r, cx, nf_text_y, 2*dpi, ui_ch, caret_clr);
            }
            y = nf_top + field_h + gap;

            /* Description field — taller box, naive wrapping renderer just
             * splits on spaces every cols-many characters. */
            const char *desc_lbl = ct_doc ? "What it should cover" : "Visual Description";
            draw_text_ex(r, desc_lbl, dx + pad, y, lbl, (i32)strlen(desc_lbl), ui_cw);
            f32 df_top = y + label_h + 4*dpi;
            f32 df_w   = dw - 2*pad;
            f32 df_text_x = dx + pad;
            bool df_focus = (app->create_theme_field == 1);
            /* Underline-style multi-line input — baseline rule under the text
             * block, matching the Name field. */
            f32 df_line_h = (df_focus ? 1.5f : 1.0f) * dpi;
            renderer_draw_rect(r, df_text_x, df_top + desc_h - df_line_h,
                               df_w, df_line_h, df_focus ? ct_focus : ct_idle);
            renderer_flush_rects(r);
            i32 dlen = app->create_theme_desc_len;
            bool d_empty = (dlen == 0);
            const char *d_placeholder = ct_doc
                ? "a checklist for shipping v0.2, grouped by area, with owners and dates"
                : "deep teal background with pale gold cursor, retro terminal vibe";
            i32 cols_per_line = (i32)((df_w - 8*dpi) / ui_cw);
            if (cols_per_line < 10) cols_per_line = 10;
            const char *d_text = d_empty ? d_placeholder : app->create_theme_desc;
            i32 d_total = d_empty ? (i32)strlen(d_placeholder) : dlen;
            /* Word-wrap by sliding to the previous space when we hit the
             * column limit. Three rendered rows max — beyond that the rest
             * is silently truncated visually (still kept in the buffer). */
            i32 line = 0;
            i32 i_d = 0;
            while (i_d < d_total && line < 3) {
                i32 take = d_total - i_d;
                if (take > cols_per_line) take = cols_per_line;
                if (i_d + take < d_total) {
                    /* try to break on a space */
                    i32 brk = take;
                    while (brk > 0 && d_text[i_d + brk] != ' ') brk--;
                    if (brk > cols_per_line / 3) take = brk;
                }
                draw_text_ex(r, d_text + i_d,
                             df_text_x,
                             df_top + 6*dpi + (f32)line * (ui_ch + 2*dpi),
                             d_empty ? dim : val, take, ui_cw);
                i_d += take;
                while (i_d < d_total && d_text[i_d] == ' ') i_d++;
                line++;
            }
            if (app->create_theme_field == 1) {
                /* Caret at end of last visible line. */
                i32 last_line = line > 0 ? line - 1 : 0;
                i32 last_used = 0;
                {
                    /* re-walk to compute last visible line length */
                    i32 j = 0; i32 l = 0;
                    while (j < d_total && l <= last_line) {
                        i32 take = d_total - j;
                        if (take > cols_per_line) take = cols_per_line;
                        if (j + take < d_total) {
                            i32 brk = take;
                            while (brk > 0 && d_text[j + brk] != ' ') brk--;
                            if (brk > cols_per_line / 3) take = brk;
                        }
                        if (l == last_line) last_used = take;
                        j += take;
                        while (j < d_total && d_text[j] == ' ') j++;
                        l++;
                    }
                }
                f32 cx = df_text_x + (d_empty ? 0 : (f32)last_used * ui_cw);
                f32 cy = df_top + 6*dpi + (f32)last_line * (ui_ch + 2*dpi);
                f32 cx_max = df_text_x + df_w - 4*dpi;
                if (cx > cx_max) cx = cx_max;
                renderer_draw_rect(r, cx, cy, 2*dpi, ui_ch, caret_clr);
            }
            y = df_top + desc_h + gap;

            /* Agent picker. Each detected agent is a chip; the selected
             * one is highlighted. When no agents are detected we show a
             * dimmed "No CLI agents found in PATH" line. */
            draw_text_ex(r, "Agent", dx + pad, y, lbl, 5, ui_cw);
            y += label_h + 4*dpi;

            if (app->create_theme_agent_count == 0) {
                draw_text_ex(r,
                             "No CLI agents found in PATH (claude, codex, copilot, ...).",
                             dx + pad, y + 16*dpi,
                             dim, 60, ui_cw);
            } else {
                f32 chip_h  = 32 * dpi;
                f32 chip_x  = dx + pad;
                f32 chip_pad = 12 * dpi;
                f32 chip_gap = 8 * dpi;
                Color chip_bg     = cp.btn_secondary_bg; chip_bg.a *= panel_alpha;
                Color chip_sel_bg = cp.btn_primary_bg;   chip_sel_bg.a *= panel_alpha;
                Color chip_fg     = lbl;
                Color chip_sel_fg = cp.btn_primary_fg;   chip_sel_fg.a *= panel_alpha;
                for (i32 ai = 0; ai < app->create_theme_agent_count && app->create_theme_agents; ai++) {
                    const AgentInfo *a = &app->create_theme_agents[ai];
                    i32 dlen2 = (i32)strlen(a->display);
                    f32 w = chip_pad * 2 + (f32)dlen2 * ui_cw;
                    bool sel = (ai == app->create_theme_agent_idx);
                    /* wrap to next row if we run out of width */
                    if (chip_x + w > dx + dw - pad) {
                        chip_x = dx + pad;
                        y += chip_h + chip_gap;
                    }
                    renderer_draw_rrect_simple(r, chip_x, y, w, chip_h,
                                               sel ? chip_sel_bg : chip_bg,
                                               chip_h * 0.5f);
                    draw_text_ex(r, a->display,
                                 chip_x + chip_pad,
                                 y + (chip_h - ui_ch)/2,
                                 sel ? chip_sel_fg : chip_fg,
                                 dlen2, ui_cw);
                    chip_x += w + chip_gap;
                }
                renderer_flush_rrects(r);
            }
            y = dy + dh - pad - 32*dpi - gap*2 - ui_ch;

            /* Hint / error banner */
            bool show_err = app->create_theme_error[0] &&
                            platform_time_sec() < app->create_theme_error_until;
            if (show_err) {
                Color err_bg  = cp.btn_destructive_bg; err_bg.a = 0.25f * panel_alpha;
                Color err_fg  = cp.btn_destructive_fg; err_fg.a *= panel_alpha;
                renderer_draw_rect(r, dx + pad, y, dw - 2*pad, ui_ch + 8*dpi, err_bg);
                draw_text_ex(r, app->create_theme_error,
                             dx + pad + 8*dpi, y + 4*dpi,
                             err_fg, (i32)strlen(app->create_theme_error), ui_cw);
            } else {
                Color hint = (Color){ lbl.r, lbl.g, lbl.b, lbl.a * 0.6f };
                draw_text_ex(r,
                    "Tab: cycle field   Enter: generate   Esc: close",
                    dx + pad, y, hint, 48, ui_cw);
            }
            renderer_flush_rects(r);

            /* Buttons row (Cancel / Generate). Generate is the primary
             * action and only enabled if name + desc + an agent are
             * present. */
            f32 btn_h = 32 * dpi;
            f32 btn_w = 140 * dpi;
            f32 btn_y = dy + dh - pad - btn_h;
            f32 gen_x = dx + dw - pad - btn_w;
            f32 cancel_x = gen_x - btn_w - gap;

            bool can_gen = (app->create_theme_name_len > 0 &&
                            app->create_theme_desc_len > 0 &&
                            app->create_theme_agent_count > 0);
            Color cancel_bg = cp.btn_secondary_bg; cancel_bg.a *= panel_alpha;
            Color gen_bg    = cp.btn_primary_bg;   gen_bg.a = gen_bg.a * panel_alpha * (can_gen ? 1.0f : 0.40f);
            renderer_draw_rrect_simple(r, cancel_x, btn_y, btn_w, btn_h,
                                       cancel_bg, 8.0f * dpi);
            renderer_draw_rrect_simple(r, gen_x, btn_y, btn_w, btn_h,
                                       gen_bg, 8.0f * dpi);
            renderer_flush_rrects(r);

            f32 cancel_text_x = cancel_x + (btn_w - 6 * ui_cw) / 2;
            f32 gen_text_x    = gen_x    + (btn_w - 8 * ui_cw) / 2;
            f32 btn_text_y    = btn_y + (btn_h - ui_ch) / 2;
            draw_text_ex(r, "Cancel",   cancel_text_x, btn_text_y,
                         cp.btn_secondary_fg, 6, ui_cw);
            Color gen_fg = cp.btn_primary_fg;
            gen_fg.a *= panel_alpha * (can_gen ? 1.0f : 0.50f);
            draw_text_ex(r, "Generate", gen_text_x, btn_text_y, gen_fg, 8, ui_cw);
        } else {
            /* Generating state — single centred pill with a comet
             * spinner.  No form, no fields, no log: phase 2/3 close
             * the dialog from main.c (the outcome is surfaced as a
             * toast), so this is the only non-form layout.  Esc still
             * cancels via the dialog's keybinding. */
            f32 btn_h = 44 * dpi;
            f32 btn_w = 200 * dpi;
            f32 btn_x = dx + (dw - btn_w) * 0.5f;
            /* Nudged up to leave room for the live status line below, so
             * the pill + status group reads as vertically centered. */
            f32 btn_y = dy + hdr_h + (body_h - btn_h) * 0.5f - 18.0f * dpi;
            Color spin_bg = cp.btn_primary_bg; spin_bg.a *= panel_alpha;
            Color spin_fg = cp.btn_primary_fg; spin_fg.a *= panel_alpha;
            renderer_draw_rrect_simple(r, btn_x, btn_y, btn_w, btn_h,
                                       spin_bg, 10.0f * dpi);
            renderer_flush_rrects(r);

            /* Comet spinner — 8 dots around a small circle. The "head"
             * sweeps one revolution per ~0.9s; dots fade with distance
             * from it. No arc-rendering primitive needed. */
            f64 spin_now = platform_time_sec();
            f32 head_t   = (f32)fmod(spin_now * 1.1, 1.0);
            f32 spin_r   = 9.0f * dpi;
            f32 dot_r    = 1.7f * dpi;
            f32 spin_cx  = btn_x + 24.0f * dpi;
            f32 spin_cy  = btn_y + btn_h * 0.5f;
            const i32 n_dots = 8;
            for (i32 si = 0; si < n_dots; si++) {
                f32 ti  = (f32)si / (f32)n_dots;
                f32 ang = ti * 6.2831853f - 1.5707963f;
                f32 sxd = spin_cx + cosf(ang) * spin_r;
                f32 syd = spin_cy + sinf(ang) * spin_r;
                f32 dst = fabsf(ti - head_t);
                if (dst > 0.5f) dst = 1.0f - dst;
                f32 da  = 1.0f - dst * 1.8f;
                if (da < 0.12f) da = 0.12f;
                Color dotc = {spin_fg.r, spin_fg.g, spin_fg.b,
                              spin_fg.a * da};
                renderer_draw_rrect_simple(r,
                    sxd - dot_r, syd - dot_r,
                    dot_r * 2.0f, dot_r * 2.0f, dotc, dot_r);
            }
            renderer_flush_rrects(r);

            const char *label = "Generating...";
            i32 lab_len = (i32)strlen(label);
            f32 lab_x = spin_cx + spin_r + 14.0f * dpi;
            draw_text_ex(r, label, lab_x, btn_y + (btn_h - ui_ch) * 0.5f,
                         spin_fg, lab_len, ui_cw);

            /* Live activity line under the pill: the agent's current stdout
             * tail (or a phase message), refreshed every tick by
             * app_tick_create_theme. Single line, centered, ellipsized to
             * the panel width so the user can watch what the agent (Claude
             * Code, Codex, …) is doing instead of staring at an opaque
             * spinner. */
            const char *act = app->create_theme_status[0]
                            ? app->create_theme_status : "Working…";
            i32 budget = (i32)((dw - 2.0f * pad) / ui_cw);
            if (budget < 8) budget = 8;
            i32 act_cp = (i32)utf8_len((const u8 *)act, strlen(act));
            f32 act_y  = btn_y + btn_h + 16.0f * dpi;
            if (act_cp <= budget) {
                f32 w = (f32)act_cp * ui_cw;
                draw_text_ex(r, act, dx + (dw - w) * 0.5f, act_y,
                             lbl, act_cp, ui_cw);
            } else {
                i32 show = budget - 1;
                f32 w  = (f32)budget * ui_cw;
                f32 x0 = dx + (dw - w) * 0.5f;
                draw_text_ex(r, act, x0, act_y, lbl, show, ui_cw);
                draw_text_ex(r, "…", x0 + (f32)show * ui_cw, act_y,
                             lbl, 1, ui_cw);
            }
        }

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close create_theme open-anim wrapper */

    /* Keyboard-interactive / 2FA dialog */
    {
        static bool s_kbi_was_open = false;
        static Anim s_kbi_open_anim  = {0};
        static Anim s_kbi_close_anim = {0};
        f32 dpi = app->dpi_scale;
        f32 panel_scale, panel_alpha, panel_yoff;
        bool render_kbi = modal_anim_progress(
            app->kbi_dialog_active,
            &s_kbi_open_anim, &s_kbi_close_anim,
            &s_kbi_was_open, dpi, MODAL_OPEN_DUR_FAST,
            &panel_scale, &panel_alpha, &panel_yoff);
    if (render_kbi) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;

        i32 np = app->kbi_num_prompts;
        if (np < 1) np = 1;
        bool has_name = app->kbi_name[0] != '\0';
        bool has_instr = app->kbi_instruction[0] != '\0';
        f32 header_h = 30 * dpi;
        f32 info_h = (has_name ? ui_ch + 4 : 0) + (has_instr ? ui_ch + 4 : 0);
        f32 field_h = 28 * dpi;
        f32 prompts_h = (f32)np * (field_h + 8);
        f32 button_h = 40 * dpi;
        f32 dh = header_h + 10 * dpi + info_h + prompts_h + button_h + 10 * dpi;
        f32 dw = 440 * dpi;
        f32 dx = ((f32)app->fb_width - dw) / 2;
        f32 dy = ((f32)app->fb_height - dh) / 2;
        f32 sx = dx + (dw - dw * panel_scale) * 0.5f;
        f32 sy = dy + (dh - dh * panel_scale) * 0.5f - panel_yoff * 0.8f;
        dx = sx; dy = sy; dw *= panel_scale; dh *= panel_scale;

        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height, (Color){0,0,0,0.5f * panel_alpha});
        renderer_flush_rects(r);

        /* Panel shell — Settings-style chrome (see create_theme above). */
        ChromePalette cp = chrome_palette_for(t);
        f32 panel_radius = 12.0f * dpi;
        Color panel_bg = t->bg;
        Color panel_bd = t->border;
        panel_bd.a = (panel_bd.a > 0.05f ? panel_bd.a : 1.0f) * panel_alpha;
        panel_bg.a = panel_alpha;
        renderer_draw_rrect_bordered(r,
            dx, dy, dw, dh,
            panel_bg, panel_bd, fmaxf(1.0f, dpi),
            panel_radius, panel_radius, panel_radius, panel_radius,
            24.0f * dpi, 0.35f * panel_alpha, 0.0f, 6.0f * dpi);
        renderer_flush_rrects(r);

        Color hairline = cp.divider_subtle; hairline.a *= panel_alpha;
        renderer_draw_rect(r,
            dx + panel_radius * 0.5f, dy + header_h - fmaxf(1.0f, dpi),
            dw - panel_radius, fmaxf(1.0f, dpi), hairline);
        renderer_flush_rects(r);

        f32 ty = dy + (header_h - ui_ch) / 2;
        Color title_fg = t->fg; title_fg.a *= panel_alpha;
        draw_text_ex(r, "Authentication Required", dx + 12, ty,
                     title_fg, 23, ui_cw);

        f32 cy = dy + header_h + 10 * dpi;
        Color info_clr = t->sidebar_fg; info_clr.a *= panel_alpha;
        if (has_name) {
            draw_text_ex(r, app->kbi_name, dx + 12, cy, info_clr,
                         (i32)strlen(app->kbi_name), ui_cw);
            cy += ui_ch + 4;
        }
        if (has_instr) {
            draw_text_ex(r, app->kbi_instruction, dx + 12, cy, info_clr,
                         (i32)strlen(app->kbi_instruction), ui_cw);
            cy += ui_ch + 4;
        }

        Color lbl = t->sidebar_fg;     lbl.a *= panel_alpha;
        Color val = t->fg;             val.a *= panel_alpha;
        Color active_border = cp.btn_primary_bg; active_border.a *= panel_alpha;
        Color field_bg_kbi  = cp.surface_sunken; field_bg_kbi.a  *= panel_alpha;
        Color kbi_focus = theme_ui_accent(t); kbi_focus.a = fmaxf(kbi_focus.a, 0.90f) * panel_alpha;
        Color kbi_idle  = t->border;          kbi_idle.a  = fmaxf(kbi_idle.a, 0.45f) * panel_alpha;

        for (i32 pi = 0;
             app->kbi_prompts && app->kbi_responses &&
             pi < app->kbi_num_prompts && pi < KBI_MAX_PROMPTS; pi++) {
            i32 lbl_len = (i32)strlen(app->kbi_prompts[pi]);
            if (lbl_len > 50) lbl_len = 50;
            draw_text_ex(r, app->kbi_prompts[pi], dx + 12,
                         cy + (field_h - ui_ch) / 2, lbl, lbl_len, ui_cw);

            f32 field_x = dx + 160 * dpi;
            f32 field_w = dw - 172 * dpi;
            draw_input_field(r, field_x, cy, field_w, field_h, field_bg_kbi,
                             app->kbi_field == pi ? kbi_focus : kbi_idle, dpi);

            if (app->kbi_echo[pi]) {
                draw_text_ex(r, app->kbi_responses[pi], field_x + 4,
                             cy + (field_h - ui_ch) / 2, val, 30, ui_cw);
            } else {
                char masked[128];
                i32 rlen = (i32)strlen(app->kbi_responses[pi]);
                i32 show = rlen < (i32)sizeof(masked) - 1 ? rlen : (i32)sizeof(masked) - 1;
                for (i32 mi = 0; mi < show; mi++) masked[mi] = '*';
                masked[show] = '\0';
                draw_text_ex(r, masked, field_x + 4,
                             cy + (field_h - ui_ch) / 2, val, 30, ui_cw);
            }
            cy += field_h + 8;
        }

        f32 btn_y = dy + dh - 40 * dpi;
        f32 btn_w = 100 * dpi;
        f32 btn_x = dx + dw - btn_w - 12;
        Color sub_bg = cp.btn_primary_bg;   sub_bg.a *= panel_alpha;
        Color sub_fg = cp.btn_primary_fg;   sub_fg.a *= panel_alpha;
        Color cnc_bg = cp.btn_secondary_bg; cnc_bg.a *= panel_alpha;
        Color cnc_fg = cp.btn_secondary_fg; cnc_fg.a *= panel_alpha;
        renderer_draw_rrect_simple(r, btn_x, btn_y, btn_w, 28 * dpi, sub_bg, 8.0f * dpi);
        f32 cbtn_x = btn_x - btn_w - 12;
        renderer_draw_rrect_simple(r, cbtn_x, btn_y, btn_w, 28 * dpi, cnc_bg, 8.0f * dpi);
        renderer_flush_rrects(r);
        draw_text_ex(r, "Submit", btn_x + (btn_w - 6 * ui_cw) / 2,
                     btn_y + (28 * dpi - ui_ch) / 2, sub_fg, 6, ui_cw);
        draw_text_ex(r, "Cancel", cbtn_x + (btn_w - 6 * ui_cw) / 2,
                     btn_y + (28 * dpi - ui_ch) / 2, cnc_fg, 6, ui_cw);

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close kbi_dialog open-anim wrapper */

    /* Key manager overlay */
    {
        static bool s_keymgr_was_open = false;
        static Anim s_keymgr_open_anim  = {0};
        static Anim s_keymgr_close_anim = {0};
        f32 dpi = app->dpi_scale;
        f32 panel_scale, panel_alpha, panel_yoff;
        bool render_keymgr = modal_anim_progress(
            app->key_manager_active,
            &s_keymgr_open_anim, &s_keymgr_close_anim,
            &s_keymgr_was_open, dpi, MODAL_OPEN_DUR_LARGE,
            &panel_scale, &panel_alpha, &panel_yoff);
    if (render_keymgr) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;

        f32 dw = 600 * dpi, dh = 480 * dpi;
        f32 dx = ((f32)app->fb_width - dw) / 2;
        f32 dy = ((f32)app->fb_height - dh) / 2;
        f32 sx = dx + (dw - dw * panel_scale) * 0.5f;
        f32 sy = dy + (dh - dh * panel_scale) * 0.5f - panel_yoff;
        dx = sx; dy = sy; dw *= panel_scale; dh *= panel_scale;

        /* Dim background */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height, (Color){0,0,0,0.5f * panel_alpha});
        renderer_flush_rects(r);

        /* Panel shell — Settings-style chrome (see create_theme). */
        ChromePalette cp = chrome_palette_for(t);
        f32 panel_radius = 12.0f * dpi;
        f32 title_h = 32 * dpi;
        Color panel_bg = t->bg;
        Color panel_bd = t->border;
        panel_bd.a = (panel_bd.a > 0.05f ? panel_bd.a : 1.0f);
        renderer_draw_rrect_bordered(r,
            dx, dy, dw, dh,
            panel_bg, panel_bd, fmaxf(1.0f, dpi),
            panel_radius, panel_radius, panel_radius, panel_radius,
            24.0f * dpi, 0.35f, 0.0f, 6.0f * dpi);
        renderer_flush_rrects(r);

        Color hairline = cp.divider_subtle;
        renderer_draw_rect(r,
            dx + panel_radius * 0.5f, dy + title_h - fmaxf(1.0f, dpi),
            dw - panel_radius, fmaxf(1.0f, dpi), hairline);
        renderer_flush_rects(r);

        f32 ty_title = dy + (title_h - ui_ch) / 2;
        draw_text_ex(r, "SSH Key Manager", dx + 12, ty_title, t->fg, 20, ui_cw);

        /* Close button [X] */
        f32 close_x = dx + dw - 28 * dpi;
        draw_text_ex(r, "X", close_x, ty_title, t->ansi[9], 1, ui_cw);

        if (!app->keygen_form_active) {
            /* ==== Key list view ==== */

            /* Action buttons row — pill-shaped chrome-palette buttons. */
            f32 btn_row_y = dy + title_h + 6;
            f32 btn_h = 24 * dpi;
            f32 btn_radius = 6.0f * dpi;

            f32 gen_btn_w = 140 * dpi;
            f32 gen_btn_x = dx + 12;
            renderer_draw_rrect_simple(r, gen_btn_x, btn_row_y, gen_btn_w, btn_h,
                                       cp.btn_primary_bg, btn_radius);

            f32 ref_btn_w = 70 * dpi;
            f32 ref_btn_x = gen_btn_x + gen_btn_w + 8;
            renderer_draw_rrect_simple(r, ref_btn_x, btn_row_y, ref_btn_w, btn_h,
                                       cp.btn_secondary_bg, btn_radius);
            renderer_flush_rrects(r);
            draw_text_ex(r, "+ Generate Key", gen_btn_x + 8, btn_row_y + (btn_h - ui_ch)/2,
                         cp.btn_primary_fg, 14, ui_cw);
            draw_text_ex(r, "Refresh", ref_btn_x + 8, btn_row_y + (btn_h - ui_ch)/2,
                         cp.btn_secondary_fg, 7, ui_cw);

            /* Column headers */
            f32 list_y = btn_row_y + btn_h + 8;
            Color hdr_col = t->sidebar_fg;
            draw_text_ex(r, "Name", dx + 12, list_y, hdr_col, 12, ui_cw);
            draw_text_ex(r, "Type", dx + 160 * dpi, list_y, hdr_col, 10, ui_cw);
            draw_text_ex(r, "Fingerprint", dx + 240 * dpi, list_y, hdr_col, 12, ui_cw);
            draw_text_ex(r, "Pass", dx + dw - 40 * dpi, list_y, hdr_col, 4, ui_cw);

            /* Separator line */
            list_y += ui_ch + 4;
            renderer_draw_rect(r, dx + 8, list_y, dw - 16, 1, t->border);
            list_y += 4;

            /* Key list */
            f32 item_h = ui_ch + 8;
            i32 max_visible = (i32)((dy + dh - list_y - 50 * dpi) / item_h);
            i32 start = app->key_list_scroll;
            if (start < 0) start = 0;

            for (i32 ki = start; ki < app->key_list_count && (ki - start) < max_visible; ki++) {
                KeyInfo *k = &app->key_list[ki];
                bool sel = (ki == app->key_list_selected);

                if (sel) {
                    renderer_draw_rect(r, dx + 4, list_y - 2, dw - 8, item_h,
                                      (Color){t->sidebar_active.r, t->sidebar_active.g, t->sidebar_active.b, 0.5f});
                }

                Color name_col = sel ? t->fg : t->sidebar_fg;
                draw_text_ex(r, k->name, dx + 12, list_y, name_col, 18, ui_cw);
                draw_text_ex(r, k->type, dx + 160 * dpi, list_y,
                             t->ansi[12], 10, ui_cw);

                /* Truncated fingerprint */
                char fp_short[32];
                if (k->fingerprint[0]) {
                    i32 fplen = (i32)strlen(k->fingerprint);
                    if (fplen > 20) {
                        snprintf(fp_short, sizeof(fp_short), "...%s", k->fingerprint + fplen - 16);
                    } else {
                        snprintf(fp_short, sizeof(fp_short), "%s", k->fingerprint);
                    }
                } else {
                    snprintf(fp_short, sizeof(fp_short), "-");
                }
                draw_text_ex(r, fp_short, dx + 240 * dpi, list_y,
                             t->sidebar_fg, 20, ui_cw);

                /* Passphrase indicator */
                draw_text_ex(r, k->has_passphrase ? "Yes" : "No",
                             dx + dw - 40 * dpi, list_y,
                             k->has_passphrase ? t->ansi[10] : t->sidebar_fg,
                             3, ui_cw);

                list_y += item_h;
            }
            renderer_flush_rects(r);

            if (app->key_list_count == 0) {
                draw_text_ex(r, "No SSH keys found in ~/.ssh/", dx + 12, list_y + 8,
                             t->sidebar_fg, 30, ui_cw);
            }

            /* Bottom action buttons for selected key */
            if (app->key_list_selected >= 0 && app->key_list_selected < app->key_list_count) {
                f32 bot_y = dy + dh - 40 * dpi;

                f32 copy_w = 120 * dpi;
                renderer_draw_rrect_simple(r, dx + 12, bot_y, copy_w, 28 * dpi,
                                           cp.btn_primary_bg, 8.0f * dpi);

                f32 del_w = 90 * dpi;
                f32 del_x = dx + dw - del_w - 12;
                Color del_bg = t->ansi[1];
                Color del_fg = chrome_legible_on(del_bg);
                renderer_draw_rrect_simple(r, del_x, bot_y, del_w, 28 * dpi,
                                           del_bg, 8.0f * dpi);
                renderer_flush_rrects(r);
                draw_text_ex(r, "Copy Pub Key", dx + 20, bot_y + (28*dpi - ui_ch)/2,
                             cp.btn_primary_fg, 12, ui_cw);
                draw_text_ex(r, "Delete", del_x + (del_w - 6*ui_cw)/2,
                             bot_y + (28*dpi - ui_ch)/2, del_fg, 6, ui_cw);
            }

            /* Delete confirmation — small Settings-style modal nested
             * inside the Key Manager.  Red accent hairline conveys the
             * destructive context without painting a full red band. */
            if (app->keygen_confirm_delete) {
                f32 cd_w = 320 * dpi, cd_h = 100 * dpi;
                f32 cd_x = ((f32)app->fb_width - cd_w) / 2;
                f32 cd_y = ((f32)app->fb_height - cd_h) / 2;
                f32 cd_radius = 10.0f * dpi;
                renderer_draw_rrect_bordered(r, cd_x, cd_y, cd_w, cd_h,
                    t->bg, t->border, fmaxf(1.0f, dpi),
                    cd_radius, cd_radius, cd_radius, cd_radius,
                    18.0f * dpi, 0.45f, 0.0f, 6.0f * dpi);
                renderer_flush_rrects(r);
                Color cd_warn = (Color){ t->ansi[1].r, t->ansi[1].g, t->ansi[1].b, 0.6f };
                renderer_draw_rect(r,
                    cd_x + cd_radius * 0.5f, cd_y + 36 * dpi,
                    cd_w - cd_radius, fmaxf(1.0f, dpi), cd_warn);
                renderer_flush_rects(r);

                draw_text_ex(r, "Delete this key pair?", cd_x + 12, cd_y + 12,
                             t->fg, 22, ui_cw);

                if (app->keygen_delete_idx >= 0 && app->keygen_delete_idx < app->key_list_count) {
                    draw_text_ex(r, app->key_list[app->keygen_delete_idx].name,
                                 cd_x + 12, cd_y + 12 + ui_ch + 4,
                                 t->sidebar_fg, 40, ui_cw);
                }

                /* Delete (destructive, red) + Cancel (secondary) — pill style. */
                f32 cd_btn_y = cd_y + cd_h - 32 * dpi;
                Color cd_del_bg = t->ansi[1];
                Color cd_del_fg = chrome_legible_on(cd_del_bg);
                renderer_draw_rrect_simple(r, cd_x + 12, cd_btn_y, 80 * dpi, 24 * dpi,
                                           cd_del_bg, 6.0f * dpi);
                renderer_draw_rrect_simple(r, cd_x + cd_w - 92 * dpi, cd_btn_y,
                                           80 * dpi, 24 * dpi,
                                           cp.btn_secondary_bg, 6.0f * dpi);
                renderer_flush_rrects(r);
                draw_text_ex(r, "Delete", cd_x + 20, cd_btn_y + (24*dpi - ui_ch)/2,
                             cd_del_fg, 6, ui_cw);
                draw_text_ex(r, "Cancel", cd_x + cd_w - 84 * dpi,
                             cd_btn_y + (24*dpi - ui_ch)/2,
                             cp.btn_secondary_fg, 6, ui_cw);
            }

            /* Status message */
            if (app->keygen_status[0]) {
                f32 stat_y = dy + dh - 12;
                draw_text_ex(r, app->keygen_status, dx + 150 * dpi, stat_y - ui_ch,
                             (Color){0.5f, 0.7f, 0.5f, 1.0f}, 50, ui_cw);
            }

        } else {
            /* ==== Key generation form ==== */
            f32 form_y = dy + title_h + 12;
            f32 field_h = 28 * dpi;
            f32 label_w = 100 * dpi;
            Color lbl = t->sidebar_fg;
            Color val = t->fg;
            Color field_bg = cp.surface_sunken;
            Color kg_focus = theme_ui_accent(t); kg_focus.a = fmaxf(kg_focus.a, 0.90f);
            Color kg_idle  = t->border;          kg_idle.a  = fmaxf(kg_idle.a, 0.45f);
            Color placeholder = (Color){t->sidebar_fg.r, t->sidebar_fg.g, t->sidebar_fg.b, 0.5f};

            draw_text_ex(r, "Generate New SSH Key", dx + 12, form_y,
                         t->fg, 22, ui_cw);
            form_y += ui_ch + 12;

            /* Key type selector — pill chips, primary fill for the
             * selected option, secondary fill for the rest. */
            draw_text_ex(r, "Type:", dx + 12, form_y + (field_h - ui_ch)/2, lbl, 6, ui_cw);

            static const char *type_names[] = {
                "Ed25519", "RSA 2048", "RSA 4096", "ECDSA 256", "ECDSA 384", "ECDSA 521"
            };
            {
                f32 type_x = dx + label_w;
                for (i32 ti = 0; ti < 6; ti++) {
                    f32 tw = ((i32)strlen(type_names[ti]) + 2) * ui_cw;
                    bool is_sel = (app->keygen_type == ti);
                    renderer_draw_rrect_simple(r, type_x, form_y, tw, field_h,
                                               is_sel ? cp.btn_primary_bg : cp.btn_secondary_bg,
                                               field_h * 0.5f);
                    type_x += tw + 4;
                }
                renderer_flush_rrects(r);

                type_x = dx + label_w;
                for (i32 ti = 0; ti < 6; ti++) {
                    f32 tw = ((i32)strlen(type_names[ti]) + 2) * ui_cw;
                    bool is_sel = (app->keygen_type == ti);
                    draw_text_ex(r, type_names[ti], type_x + ui_cw, form_y + (field_h - ui_ch)/2,
                                 is_sel ? cp.btn_primary_fg : cp.btn_secondary_fg,
                                 12, ui_cw);
                    type_x += tw + 4;
                }
            }
            form_y += field_h + 10;

            /* Filename field */
            draw_text_ex(r, "Filename:", dx + 12, form_y + (field_h - ui_ch)/2, lbl, 9, ui_cw);
            draw_input_field(r, dx + label_w, form_y, dw - label_w - 12, field_h, field_bg,
                             app->keygen_field == 0 ? kg_focus : kg_idle, dpi);
            {
                const char *fn_display = app->keygen_filename[0] ? app->keygen_filename : "id_ed25519";
                Color fn_col = app->keygen_filename[0] ? val : placeholder;
                draw_text_ex(r, fn_display, dx + label_w + 4, form_y + (field_h - ui_ch)/2,
                             fn_col, 40, ui_cw);
            }
            form_y += field_h + 8;

            /* Passphrase field */
            draw_text_ex(r, "Passphrase:", dx + 12, form_y + (field_h - ui_ch)/2, lbl, 11, ui_cw);
            draw_input_field(r, dx + label_w, form_y, dw - label_w - 12, field_h, field_bg,
                             app->keygen_field == 1 ? kg_focus : kg_idle, dpi);
            {
                char masked[128];
                i32 plen = (i32)strlen(app->keygen_passphrase);
                i32 show = plen < 127 ? plen : 127;
                for (i32 mi = 0; mi < show; mi++) masked[mi] = '*';
                masked[show] = '\0';
                if (show > 0)
                    draw_text_ex(r, masked, dx + label_w + 4, form_y + (field_h - ui_ch)/2,
                                 val, 40, ui_cw);
                else
                    draw_text_ex(r, "(optional)", dx + label_w + 4, form_y + (field_h - ui_ch)/2,
                                 placeholder, 10, ui_cw);
            }
            form_y += field_h + 8;

            /* Confirm passphrase */
            draw_text_ex(r, "Confirm:", dx + 12, form_y + (field_h - ui_ch)/2, lbl, 8, ui_cw);
            draw_input_field(r, dx + label_w, form_y, dw - label_w - 12, field_h, field_bg,
                             app->keygen_field == 2 ? kg_focus : kg_idle, dpi);
            {
                char masked2[128];
                i32 plen2 = (i32)strlen(app->keygen_passphrase2);
                i32 show2 = plen2 < 127 ? plen2 : 127;
                for (i32 mi = 0; mi < show2; mi++) masked2[mi] = '*';
                masked2[show2] = '\0';
                if (show2 > 0)
                    draw_text_ex(r, masked2, dx + label_w + 4, form_y + (field_h - ui_ch)/2,
                                 val, 40, ui_cw);
            }
            form_y += field_h + 8;

            /* Comment field */
            draw_text_ex(r, "Comment:", dx + 12, form_y + (field_h - ui_ch)/2, lbl, 8, ui_cw);
            draw_input_field(r, dx + label_w, form_y, dw - label_w - 12, field_h, field_bg,
                             app->keygen_field == 3 ? kg_focus : kg_idle, dpi);
            {
                const char *cmt_display = app->keygen_comment[0] ? app->keygen_comment : "user@hostname";
                Color cmt_col = app->keygen_comment[0] ? val : placeholder;
                draw_text_ex(r, cmt_display, dx + label_w + 4, form_y + (field_h - ui_ch)/2,
                             cmt_col, 40, ui_cw);
            }
            form_y += field_h + 16;

            renderer_flush_rects(r);

            /* Status/error message */
            if (app->keygen_status[0]) {
                bool is_err = (strstr(app->keygen_status, "Error") || strstr(app->keygen_status, "mismatch"));
                Color stat_col = is_err ? t->ansi[9] : t->ansi[10];
                draw_text_ex(r, app->keygen_status, dx + 12, form_y, stat_col, 60, ui_cw);
                form_y += ui_ch + 8;
            }

            /* Buttons: Generate (primary) / Cancel (secondary) — pill. */
            f32 bot_y = dy + dh - 44 * dpi;
            f32 gen_w = 100 * dpi;
            f32 cancel_w = 80 * dpi;
            renderer_draw_rrect_simple(r, dx + dw - gen_w - 12, bot_y, gen_w, 28 * dpi,
                                       cp.btn_primary_bg, 8.0f * dpi);
            renderer_draw_rrect_simple(r, dx + dw - gen_w - cancel_w - 24, bot_y,
                                       cancel_w, 28 * dpi,
                                       cp.btn_secondary_bg, 8.0f * dpi);
            renderer_flush_rrects(r);
            draw_text_ex(r, "Generate", dx + dw - gen_w - 4, bot_y + (28*dpi - ui_ch)/2,
                         cp.btn_primary_fg, 8, ui_cw);
            draw_text_ex(r, "Cancel", dx + dw - gen_w - cancel_w - 16,
                         bot_y + (28*dpi - ui_ch)/2,
                         cp.btn_secondary_fg, 6, ui_cw);
        }

        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close key_manager open-anim wrapper */

    /* Host key verification dialog */
    {
        static bool s_hk_was_open = false;
        static Anim s_hk_open_anim  = {0};
        static Anim s_hk_close_anim = {0};
        f32 dpi = app->dpi_scale;
        f32 panel_scale, panel_alpha, panel_yoff;
        bool render_hk = modal_anim_progress(
            app->hostkey_dialog_active,
            &s_hk_open_anim, &s_hk_close_anim,
            &s_hk_was_open, dpi, MODAL_OPEN_DUR_FAST,
            &panel_scale, &panel_alpha, &panel_yoff);
    if (render_hk) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;

        f32 dw = 500 * dpi, dh = 260 * dpi;
        f32 dx = ((f32)app->fb_width - dw) / 2;
        f32 dy = ((f32)app->fb_height - dh) / 2;
        f32 sx = dx + (dw - dw * panel_scale) * 0.5f;
        f32 sy = dy + (dh - dh * panel_scale) * 0.5f - panel_yoff * 0.8f;
        dx = sx; dy = sy; dw *= panel_scale; dh *= panel_scale;

        /* Dimmed backdrop */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height, (Color){0,0,0,0.5f * panel_alpha});
        renderer_flush_rects(r);

        /* Panel shell mirrors the Settings panel (see create_theme).
         * Security context is now conveyed by the title text colour
         * and a tinted hairline beneath it rather than a full red /
         * blue header band, so the dialog stays in the Settings
         * surface family while remaining unambiguous. */
        ChromePalette cp = chrome_palette_for(t);
        f32 panel_radius = 12.0f * dpi;
        Color panel_bg = t->bg;
        Color panel_bd = t->border;
        panel_bd.a = (panel_bd.a > 0.05f ? panel_bd.a : 1.0f);
        renderer_draw_rrect_bordered(r,
            dx, dy, dw, dh,
            panel_bg, panel_bd, fmaxf(1.0f, dpi),
            panel_radius, panel_radius, panel_radius, panel_radius,
            24.0f * dpi, 0.35f, 0.0f, 6.0f * dpi);
        renderer_flush_rrects(r);

        f32 hdr_h_hk = 32 * dpi;
        Color title_fg = app->hostkey_is_change ? t->ansi[1] : t->fg;
        Color title_div = app->hostkey_is_change
            ? (Color){ t->ansi[1].r, t->ansi[1].g, t->ansi[1].b, 0.6f }
            : cp.divider_subtle;
        renderer_draw_rect(r,
            dx + panel_radius * 0.5f, dy + hdr_h_hk - fmaxf(1.0f, dpi),
            dw - panel_radius, fmaxf(1.0f, dpi), title_div);
        renderer_flush_rects(r);

        f32 ty2 = dy + (hdr_h_hk - ui_ch) / 2;
        const char *title = app->hostkey_is_change
            ? "WARNING: Host Key Changed!"
            : "New SSH Host Key";
        draw_text_ex(r, title, dx + 12, ty2, title_fg, 40, ui_cw);

        f32 cy = dy + 44 * dpi;
        Color lbl = t->sidebar_fg;
        Color val = t->fg;
        Color warn = t->ansi[3]; /* yellow warning */

        /* Warning message for key change */
        if (app->hostkey_is_change) {
            draw_text_ex(r, "The host key has changed. This could indicate", dx + 12, cy, warn, 60, ui_cw);
            cy += ui_ch + 4;
            draw_text_ex(r, "a man-in-the-middle attack!", dx + 12, cy, warn, 40, ui_cw);
            cy += ui_ch + 12;
        }

        /* Host info */
        char host_info[300];
        snprintf(host_info, sizeof(host_info), "Host: %s:%d", app->hostkey_hostname, app->hostkey_port);
        draw_text_ex(r, host_info, dx + 12, cy, lbl, 60, ui_cw);
        cy += ui_ch + 8;

        /* New fingerprint */
        draw_text_ex(r, "Fingerprint:", dx + 12, cy, lbl, 12, ui_cw);
        cy += ui_ch + 2;
        draw_text_ex(r, app->hostkey_new_fp, dx + 16, cy, val, 60, ui_cw);
        cy += ui_ch + 16;

        /* Buttons: Accept and Reject */
        f32 btn_h = 28 * dpi;
        f32 btn_w_accept = 120 * dpi;
        f32 btn_w_reject = 80 * dpi;
        f32 btn_spacing = 12;

        /* Reject button — semantic red, pill-shaped to match Settings. */
        f32 reject_x = dx + dw - btn_w_reject - 12;
        f32 btn_y = dy + dh - btn_h - 12;
        Color rej_bg = t->ansi[1];
        Color rej_fg = chrome_legible_on(rej_bg);
        renderer_draw_rrect_simple(r, reject_x, btn_y, btn_w_reject, btn_h, rej_bg, 8.0f * dpi);

        /* Accept button. Yellow for dangerous accept, green for new key. */
        f32 accept_x = reject_x - btn_w_accept - btn_spacing;
        Color accept_bg = app->hostkey_is_change ? t->ansi[3] : t->ansi[2];
        Color accept_fg = chrome_legible_on(accept_bg);
        renderer_draw_rrect_simple(r, accept_x, btn_y, btn_w_accept, btn_h, accept_bg, 8.0f * dpi);
        renderer_flush_rrects(r);

        draw_text_ex(r, "Reject", reject_x + (btn_w_reject - 6*ui_cw)/2,
                     btn_y + (btn_h - ui_ch)/2, rej_fg, 6, ui_cw);
        const char *accept_label = app->hostkey_is_change ? "Accept New Key" : "Trust Key";
        i32 accept_len = (i32)strlen(accept_label);
        draw_text_ex(r, accept_label, accept_x + (btn_w_accept - (f32)accept_len*ui_cw)/2,
                     btn_y + (btn_h - ui_ch)/2, accept_fg, accept_len, ui_cw);

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close hostkey_dialog open-anim wrapper */

    /* Close confirm — an AI agent CLI is running in the close target.
     * Geometry is mirrored by the mouse handler in src/main.c — keep them
     * in sync when one moves. */
    {
        static bool s_cc_was_open = false;
        static Anim s_cc_open_anim  = {0};
        static Anim s_cc_close_anim = {0};
        f32 dpi = app->dpi_scale;
        f32 panel_scale, panel_alpha, panel_yoff;
        bool render_cc = modal_anim_progress(
            app->close_confirm_active,
            &s_cc_open_anim, &s_cc_close_anim,
            &s_cc_was_open, dpi, MODAL_OPEN_DUR_FAST,
            &panel_scale, &panel_alpha, &panel_yoff);
    if (render_cc) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;

        /* Generous, dpi-scaled padding (the old layout used unscaled 12px ⇒
         * cramped on retina). Geometry MUST stay in sync with the click
         * hit-test in src/main.c (CLOSE-CONFIRM GEOMETRY). */
        f32 pad = 22.0f * dpi;
        f32 dw = 480 * dpi, dh = 188 * dpi;
        f32 dx = ((f32)app->fb_width - dw) / 2;
        f32 dy = ((f32)app->fb_height - dh) / 2;
        f32 sx = dx + (dw - dw * panel_scale) * 0.5f;
        f32 sy = dy + (dh - dh * panel_scale) * 0.5f - panel_yoff * 0.8f;
        dx = sx; dy = sy; dw *= panel_scale; dh *= panel_scale;

        /* Dimmed backdrop */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                           (Color){0, 0, 0, 0.5f * panel_alpha});
        renderer_flush_rects(r);

        ChromePalette cp = chrome_palette_for(t);
        f32 panel_radius = 14.0f * dpi;
        Color panel_bg = t->bg;
        Color panel_bd = t->border;
        panel_bd.a = (panel_bd.a > 0.05f ? panel_bd.a : 1.0f);
        renderer_draw_rrect_bordered(r,
            dx, dy, dw, dh,
            panel_bg, panel_bd, fmaxf(1.0f, dpi),
            panel_radius, panel_radius, panel_radius, panel_radius,
            28.0f * dpi, 0.38f, 0.0f, 8.0f * dpi);
        renderer_flush_rrects(r);

        /* Title */
        const char *cc_title = app->close_confirm_group >= 0 ? "Close Group?"
                             : app->close_confirm_pane >= 0  ? "Close Pane?"
                                                             : "Close Tab?";
        f32 title_y = dy + pad;
        draw_text_ex(r, cc_title, dx + pad, title_y,
                     t->fg, (i32)strlen(cc_title), ui_cw);

        /* Divider under the title, inset by the content padding. */
        f32 divider_y = title_y + ui_ch + 12.0f * dpi;
        renderer_draw_rect(r, dx + pad, divider_y, dw - 2.0f * pad,
                           fmaxf(1.0f, dpi), cp.divider_subtle);
        renderer_flush_rects(r);

        /* Body — two lines, comfortable leading. */
        const char *cc_where = app->close_confirm_group >= 0 ? "group"
                             : app->close_confirm_pane >= 0  ? "pane"
                                                             : "tab";
        char cc_msg[96];
        snprintf(cc_msg, sizeof(cc_msg), "%s is running in this %s.",
                 app->close_confirm_agent, cc_where);
        f32 body_y = divider_y + 18.0f * dpi;
        draw_text_ex(r, cc_msg, dx + pad, body_y, t->fg, (i32)strlen(cc_msg), ui_cw);
        draw_text_ex(r, "Closing will end its session.",
                     dx + pad, body_y + ui_ch + 6.0f * dpi,
                     t->sidebar_fg, 29, ui_cw);

        /* Buttons: destructive Close (right), Cancel beside it. */
        f32 btn_h = 34 * dpi;
        f32 btn_w_close = 120 * dpi;
        f32 btn_w_cancel = 96 * dpi;
        f32 btn_radius = 9.0f * dpi;
        f32 close_x = dx + dw - pad - btn_w_close;
        f32 btn_y = dy + dh - pad - btn_h;
        f32 cancel_x = close_x - 12.0f * dpi - btn_w_cancel;

        renderer_draw_rrect_simple(r, cancel_x, btn_y, btn_w_cancel, btn_h,
                                   cp.btn_secondary_bg, btn_radius);
        Color close_bg = t->ansi[1];
        Color close_fg = chrome_legible_on(close_bg);
        renderer_draw_rrect_simple(r, close_x, btn_y, btn_w_close, btn_h,
                                   close_bg, btn_radius);
        renderer_flush_rrects(r);

        draw_text_ex(r, "Cancel", cancel_x + (btn_w_cancel - 6 * ui_cw) / 2,
                     btn_y + (btn_h - ui_ch) / 2, cp.btn_secondary_fg, 6, ui_cw);
        draw_text_ex(r, "Close", close_x + (btn_w_close - 5 * ui_cw) / 2,
                     btn_y + (btn_h - ui_ch) / 2, close_fg, 5, ui_cw);

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close close_confirm open-anim wrapper */

    /* Known hosts viewer */
    {
        static bool s_kh_was_open = false;
        static Anim s_kh_open_anim  = {0};
        static Anim s_kh_close_anim = {0};
        f32 dpi = app->dpi_scale;
        f32 panel_scale, panel_alpha, panel_yoff;
        bool render_kh = modal_anim_progress(
            app->known_hosts_open,
            &s_kh_open_anim, &s_kh_close_anim,
            &s_kh_was_open, dpi, MODAL_OPEN_DUR_LARGE,
            &panel_scale, &panel_alpha, &panel_yoff);
    if (render_kh) {
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        f32 dw = 600 * dpi, dh = 450 * dpi;
        f32 dx = ((f32)app->fb_width - dw) / 2;
        f32 dy = ((f32)app->fb_height - dh) / 2;

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;
        f32 sx = dx + (dw - dw * panel_scale) * 0.5f;
        f32 sy = dy + (dh - dh * panel_scale) * 0.5f - panel_yoff;
        dx = sx; dy = sy; dw *= panel_scale; dh *= panel_scale;

        /* Dimmed backdrop */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height, (Color){0,0,0,0.5f * panel_alpha});
        f32 panel_radius = 12.0f * dpi;
        Color panel_bd = t->border; panel_bd.a = fmaxf(panel_bd.a, 0.7f);
        renderer_draw_rrect(r,
            dx - 1.0f * dpi, dy - 1.0f * dpi,
            dw + 2.0f * dpi, dh + 2.0f * dpi, panel_bd,
            panel_radius + 1.0f, panel_radius + 1.0f,
            panel_radius + 1.0f, panel_radius + 1.0f,
            24.0f * dpi, 0.45f, 0.0f, 12.0f * dpi);
        renderer_draw_rrect_simple(r, dx, dy, dw, dh, t->tab_inactive_bg, panel_radius);
        renderer_draw_rrect(r, dx, dy, dw, 32*dpi, t->tab_active_bg,
            panel_radius, panel_radius, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f);
        renderer_flush_rects(r);
        renderer_flush_rrects(r);

        f32 ty2 = dy + (32*dpi - ui_ch) / 2;
        draw_text_ex(r, "Known Hosts", dx + 12, ty2, t->fg, 12, ui_cw);

        /* Remove All button in title bar — rounded destructive pill. */
        f32 ra_w = 90 * dpi;
        f32 ra_x = dx + dw - ra_w - 8;
        f32 ra_y = dy + 4;
        f32 ra_h = 24 * dpi;
        ChromePalette _cp_kh = chrome_palette_for(t);
        Color ra_bg = _cp_kh.btn_destructive_fg; ra_bg.a = 1.0f;
        renderer_draw_rrect_simple(r, ra_x, ra_y, ra_w, ra_h, ra_bg, 7 * dpi);

        /* Filter/search bar — recessed rounded field with accent border. */
        f32 filter_y = dy + 36 * dpi;
        f32 filter_h = 24 * dpi;
        Color kh_fbd = theme_ui_accent(t); kh_fbd.a = fmaxf(kh_fbd.a, 0.70f);
        renderer_draw_rrect_bordered(r, dx + 8, filter_y, dw - 16, filter_h,
                          t->sidebar_bg, kh_fbd, 1.5f * dpi,
                          7 * dpi, 7 * dpi, 7 * dpi, 7 * dpi, 0, 0, 0, 0);
        renderer_flush_rects(r);
        renderer_flush_rrects(r);
        draw_text_ex(r, "Remove All", ra_x + (ra_w - 10*ui_cw)/2,
                     ra_y + (ra_h - ui_ch)/2, chrome_legible_on(ra_bg), 10, ui_cw);

        Color lbl = t->sidebar_fg;
        Color val = t->fg;
        if (app->known_hosts_filter_len > 0) {
            draw_text_ex(r, app->known_hosts_filter, dx + 12,
                         filter_y + (filter_h - ui_ch)/2, val, 60, ui_cw);
        } else {
            draw_text_ex(r, "Filter hosts...", dx + 12,
                         filter_y + (filter_h - ui_ch)/2, lbl, 15, ui_cw);
        }

        /* List entries — cached and re-listed only when ~/.ssh/known_hosts
         * changes. This overlay re-renders at display rate, and
         * known_hosts_list() re-reads + re-parses the whole file every call;
         * an mtime check turns that into one stat() per frame. */
        static KnownHostEntry *kh_entries = NULL;
        static i32             kh_count   = 0;
        static time_t          kh_mtime   = (time_t)-1;
        {
            const char *kh_home = getenv("HOME");
            char kh_path[1024]; kh_path[0] = '\0';
            if (kh_home && kh_home[0])
                snprintf(kh_path, sizeof kh_path, "%s/.ssh/known_hosts", kh_home);
            struct stat kh_st;
            time_t m = (kh_path[0] && stat(kh_path, &kh_st) == 0) ? kh_st.st_mtime : 0;
            if (!kh_entries) kh_entries = calloc(256, sizeof(KnownHostEntry));
            if (kh_entries && m != kh_mtime) {
                kh_count = known_hosts_list(kh_entries, 256);
                kh_mtime = m;
            }
        }
        KnownHostEntry *entries = kh_entries;
        i32 total = kh_entries ? kh_count : 0;

        f32 list_y = filter_y + filter_h + 8;
        f32 row_h = 52 * dpi;
        f32 list_h = dy + dh - list_y - 8;
        i32 visible_rows = (i32)(list_h / row_h);
        if (visible_rows < 1) visible_rows = 1;

        /* Apply filter */
        KnownHostEntry filtered[256];
        i32 filtered_count = 0;
        for (i32 i = 0; i < total && filtered_count < 256; i++) {
            if (app->known_hosts_filter_len > 0) {
                bool match = false;
                char lower_host[256], lower_filter[128];
                for (i32 j = 0; entries[i].hostname[j]; j++)
                    lower_host[j] = (char)tolower((unsigned char)entries[i].hostname[j]);
                lower_host[strlen(entries[i].hostname)] = '\0';
                for (i32 j = 0; app->known_hosts_filter[j]; j++)
                    lower_filter[j] = (char)tolower((unsigned char)app->known_hosts_filter[j]);
                lower_filter[app->known_hosts_filter_len] = '\0';
                if (strstr(lower_host, lower_filter)) match = true;
                if (!match) {
                    char lower_type[32];
                    for (i32 j = 0; entries[i].key_type[j]; j++)
                        lower_type[j] = (char)tolower((unsigned char)entries[i].key_type[j]);
                    lower_type[strlen(entries[i].key_type)] = '\0';
                    if (strstr(lower_type, lower_filter)) match = true;
                }
                if (!match) continue;
            }
            filtered[filtered_count++] = entries[i];
        }

        /* Clamp scroll */
        i32 max_scroll = filtered_count - visible_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (app->known_hosts_scroll > max_scroll) app->known_hosts_scroll = max_scroll;
        if (app->known_hosts_scroll < 0) app->known_hosts_scroll = 0;

        for (i32 i = 0; i < visible_rows && (i + app->known_hosts_scroll) < filtered_count; i++) {
            i32 idx = i + app->known_hosts_scroll;
            KnownHostEntry *e = &filtered[idx];
            f32 ry = list_y + (f32)i * row_h;
            if (ry + row_h > dy + dh - 8) break;

            /* Row background (alternate + selection highlight) */
            Color row_bg = (idx % 2 == 0)
                ? (Color){t->tab_active_bg.r, t->tab_active_bg.g, t->tab_active_bg.b, 1.0f}
                : t->tab_inactive_bg;
            if (idx == app->known_hosts_selected) {
                row_bg = (Color){t->sidebar_active.r, t->sidebar_active.g, t->sidebar_active.b, 1.0f};
            }
            renderer_draw_rect(r, dx + 4, ry, dw - 8, row_h - 2, row_bg);
            renderer_flush_rects(r);

            /* Remove button per row — rounded destructive pill. */
            f32 rm_w = 60 * dpi;
            f32 rm_h = 20 * dpi;
            f32 rm_x = dx + dw - rm_w - 12;
            f32 rm_y = ry + (row_h - rm_h) / 2;
            Color rm_bg = _cp_kh.btn_destructive_fg; rm_bg.a = 1.0f;
            renderer_draw_rrect_simple(r, rm_x, rm_y, rm_w, rm_h, rm_bg, 6 * dpi);
            renderer_flush_rrects(r);

            draw_text_ex(r, "Remove", rm_x + (rm_w - 6*ui_cw)/2,
                         rm_y + (rm_h - ui_ch)/2, chrome_legible_on(rm_bg), 6, ui_cw);

            /* Hostname and port */
            char host_line[300];
            if (e->port != 22)
                snprintf(host_line, sizeof(host_line), "%s:%d", e->hostname, e->port);
            else
                snprintf(host_line, sizeof(host_line), "%s", e->hostname);
            draw_text_ex(r, host_line, dx + 12, ry + 6, val, 50, ui_cw);

            /* Key type */
            draw_text_ex(r, e->key_type, dx + 12, ry + 6 + ui_ch + 2, lbl, 30, ui_cw);

            /* Fingerprint */
            f32 fp_x = dx + 12 + (f32)(strlen(e->key_type) + 2) * ui_cw;
            draw_text_ex(r, e->fingerprint, fp_x, ry + 6 + ui_ch + 2, t->ansi[10], 50, ui_cw);
        }

        /* Count label */
        char count_buf[64];
        snprintf(count_buf, sizeof(count_buf), "%d host%s", filtered_count,
                 filtered_count == 1 ? "" : "s");
        draw_text_ex(r, count_buf, dx + 12, dy + dh - 8 - ui_ch, lbl,
                     (i32)strlen(count_buf), ui_cw);

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close known_hosts open-anim wrapper */

    /* Drag & drop overlay -- shown when dragging files over the terminal */
    if (app->drag_over_active) {
        f32 w = (f32)app->fb_width;
        f32 h = (f32)app->fb_height;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;

        /* Dim background */
        renderer_draw_rect(r, 0, 0, w, h, (Color){0, 0, 0, 0.3f});

        const Theme *td = app->config.theme;
        if (!td) td = &THEME_DARK;

        /* Border around the terminal area */
        f32 border = 4 * dpi;
        Color bc = (Color){td->ansi[4].r, td->ansi[4].g, td->ansi[4].b, 0.9f};
        renderer_draw_rect(r, 0, 0, w, border, bc);                  /* top */
        renderer_draw_rect(r, 0, h - border, w, border, bc);         /* bottom */
        renderer_draw_rect(r, 0, 0, border, h, bc);                  /* left */
        renderer_draw_rect(r, w - border, 0, border, h, bc);         /* right */

        /* Centered label — wording follows the drop target: a remote SSH
         * session uploads over SFTP, a local terminal types the file path
         * into the prompt (for agents that attach a dragged image). */
        Session *drop_fs = app_focused_session(app);
        const char *lbl = (drop_fs && session_type(drop_fs) == SESSION_SSH)
                          ? "Drop to upload" : "Drop file path";
        f32 lbl_w = (f32)strlen(lbl) * ui_cw;
        f32 lbl_x = (w - lbl_w) / 2;
        f32 lbl_y = h / 2 - ui_ch / 2;
        f32 pad = 12 * dpi;
        renderer_draw_rect(r, lbl_x - pad, lbl_y - pad/2,
                          lbl_w + pad*2, ui_ch + pad,
                          (Color){td->tab_inactive_bg.r, td->tab_inactive_bg.g, td->tab_inactive_bg.b, 0.9f});
        renderer_flush_rects(r);
        renderer_set_ui_scale(r, ui_cw, ui_ch);
        draw_text_ex(r, lbl, lbl_x, lbl_y,
                     td->ansi[12], 20, ui_cw);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }

    /* Passphrase prompt dialog */
    {
        static bool s_pp_was_open = false;
        static Anim s_pp_open_anim = {0};
        if (app->passphrase_dialog_active != s_pp_was_open) {
            s_pp_was_open = app->passphrase_dialog_active;
            if (app->passphrase_dialog_active) anim_start(&s_pp_open_anim, MODAL_OPEN_DUR_FAST);
        }
    if (app->passphrase_dialog_active) {
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;
        f32 anim_t = anim_progress(&s_pp_open_anim);
        f32 panel_scale = anim_lerp(MODAL_OPEN_SCALE_FROM, 1.0f, ease_apply(anim_t, MODAL_OPEN_EASE));
        f32 panel_alpha = ease_apply(anim_t, MODAL_OPEN_EASE);
        if (anim_done(&s_pp_open_anim)) { panel_scale = 1.0f; panel_alpha = 1.0f; }

        f32 dw = 440 * dpi, dh = 180 * dpi;
        f32 dx = ((f32)app->fb_width - dw) / 2;
        f32 dy = ((f32)app->fb_height - dh) / 2;
        f32 sx = dx + (dw - dw * panel_scale) * 0.5f;
        f32 sy = dy + (dh - dh * panel_scale) * 0.5f - (1.0f - ease_apply(anim_t, MODAL_OPEN_EASE)) * 8.0f * dpi;
        dx = sx; dy = sy; dw *= panel_scale; dh *= panel_scale;

        /* Dimmed background overlay */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                           (Color){0, 0, 0, 0.5f * panel_alpha});
        renderer_flush_rects(r);

        /* Panel shell — Settings-style chrome (see create_theme). */
        ChromePalette cp = chrome_palette_for(t);
        f32 panel_radius = 12.0f * dpi;
        f32 hdr_h_pp = 30 * dpi;
        Color panel_bg = t->bg;
        Color panel_bd = t->border;
        panel_bd.a = (panel_bd.a > 0.05f ? panel_bd.a : 1.0f) * panel_alpha;
        panel_bg.a = panel_alpha;
        renderer_draw_rrect_bordered(r,
            dx, dy, dw, dh,
            panel_bg, panel_bd, fmaxf(1.0f, dpi),
            panel_radius, panel_radius, panel_radius, panel_radius,
            24.0f * dpi, 0.35f * panel_alpha, 0.0f, 6.0f * dpi);
        renderer_flush_rrects(r);

        Color hairline = cp.divider_subtle; hairline.a *= panel_alpha;
        renderer_draw_rect(r,
            dx + panel_radius * 0.5f, dy + hdr_h_pp - fmaxf(1.0f, dpi),
            dw - panel_radius, fmaxf(1.0f, dpi), hairline);
        renderer_flush_rects(r);

        Color title_fg = t->fg; title_fg.a *= panel_alpha;
        f32 ty = dy + (hdr_h_pp - ui_ch) / 2;
        draw_text_ex(r, "Enter Passphrase", dx + 12, ty,
                     title_fg, 16, ui_cw);

        /* Key path label — show just the filename */
        f32 label_y = dy + 40 * dpi;
        Color lbl_clr = t->sidebar_fg; lbl_clr.a *= panel_alpha;
        draw_text_ex(r, "Key:", dx + 12, label_y, lbl_clr, 4, ui_cw);
        {
            const char *kp = app->passphrase_key_path;
            const char *slash = strrchr(kp, '/');
            const char *display = slash ? slash + 1 : kp;
            Color kp_clr = cp.btn_primary_bg; kp_clr.a *= panel_alpha;
            draw_text_ex(r, display, dx + 50 * dpi, label_y,
                         kp_clr, 40, ui_cw);
        }

        /* Passphrase input field */
        f32 field_y = label_y + ui_ch + 12 * dpi;
        f32 field_h = 28 * dpi;
        Color field_bg_pp = cp.surface_sunken; field_bg_pp.a *= panel_alpha;
        Color pp_focus    = theme_ui_accent(t); pp_focus.a = fmaxf(pp_focus.a, 0.90f) * panel_alpha;
        draw_input_field(r, dx + 12, field_y, dw - 24, field_h, field_bg_pp, pp_focus, dpi);
        Color val_clr = t->fg; val_clr.a *= panel_alpha;
        {
            char masked[128];
            i32 pw_len = (i32)strlen(app->passphrase_input);
            i32 show_len = pw_len < (i32)sizeof(masked) - 1 ? pw_len : (i32)sizeof(masked) - 1;
            for (i32 i = 0; i < show_len; i++) masked[i] = '*';
            masked[show_len] = '\0';
            draw_text_ex(r, masked, dx + 16, field_y + (field_h - ui_ch) / 2,
                         val_clr, 45, ui_cw);
        }

        /* OK and Cancel buttons — pill-shaped, chrome-palette tokens. */
        f32 btn_y = dy + dh - 40 * dpi;
        f32 btn_w = 80 * dpi;
        f32 cancel_x = dx + dw - btn_w * 2 - 24;
        f32 ok_x = dx + dw - btn_w - 12;
        Color ok_bg  = cp.btn_primary_bg;   ok_bg.a  *= panel_alpha;
        Color ok_fg  = cp.btn_primary_fg;   ok_fg.a  *= panel_alpha;
        Color cnc_bg = cp.btn_secondary_bg; cnc_bg.a *= panel_alpha;
        Color cnc_fg = cp.btn_secondary_fg; cnc_fg.a *= panel_alpha;
        renderer_draw_rrect_simple(r, cancel_x, btn_y, btn_w, 28 * dpi, cnc_bg, 8.0f * dpi);
        renderer_draw_rrect_simple(r, ok_x,     btn_y, btn_w, 28 * dpi, ok_bg,  8.0f * dpi);
        renderer_flush_rrects(r);
        draw_text_ex(r, "Cancel", cancel_x + (btn_w - 6 * ui_cw) / 2,
                     btn_y + (28 * dpi - ui_ch) / 2, cnc_fg, 6, ui_cw);
        draw_text_ex(r, "OK", ok_x + (btn_w - 2 * ui_cw) / 2,
                     btn_y + (28 * dpi - ui_ch) / 2, ok_fg, 2, ui_cw);

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close passphrase open-anim wrapper */

    /* Vault unlock overlay — stacked above the passphrase dialog so that
     * if a session triggers both at once (SSH key needs vault passphrase),
     * the vault unlock wins input focus. */
    app_vault_render_unlock(app);

    /* Vault browser overlay (M5) */
    app_vault_browser_render(app);

    /* Toast notification (top-right, auto-dismiss after 3s) */
    if (app->toast_start_time > 0) {
        f64 tnow = platform_time_sec();
        f64 telapsed = tnow - app->toast_start_time;
        if (telapsed < 3.5) {
            f32 dpi = app->dpi_scale;
            f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
            renderer_set_ui_scale(r, ui_cw, ui_ch);

            i32 msg_len = (i32)strlen(app->toast_message);
            f32 toast_w = (f32)(msg_len + 4) * ui_cw;
            f32 toast_h = ui_ch + 16.0f * dpi;
            f32 toast_x = (f32)app->fb_width - toast_w - 16.0f * dpi;
            f32 toast_y = app->tab_bar_height + 12.0f * dpi;

            /* Fade in (0..0.2s, eased) and fade out (2.5..3.5s, eased).
             * EASE_OUT_CUBIC on the entry feels less mechanical than the
             * linear ramp, and the fade out uses a matching curve so the
             * toast doesn't appear to "snap" off-screen. */
            f32 talpha = 1.0f;
            if (telapsed < 0.2) {
                f32 t = (f32)(telapsed / 0.2);
                talpha = ease_apply(t, EASE_OUT_CUBIC);
            } else if (telapsed > 2.5) {
                f32 t = (f32)((telapsed - 2.5) / 1.0);
                if (t > 1.0f) t = 1.0f;
                talpha = 1.0f - ease_apply(t, EASE_IN_OUT_CUBIC);
            }

            /* Theme-driven colors — works with every palette */
            const Theme *tt = app->config.theme;
            Color toast_bg = (Color){
                fminf(1.0f, tt->bg.r + 0.05f),
                fminf(1.0f, tt->bg.g + 0.05f),
                fminf(1.0f, tt->bg.b + 0.05f),
                0.95f * talpha
            };
            /* Border: prefer theme->border, fall back to a dimmed fg if the
             * theme's border is essentially invisible (alpha/luma near 0). */
            Color tb = tt->border;
            f32 b_lum = tb.r + tb.g + tb.b;
            if (tb.a < 0.1f || b_lum < 0.05f) {
                tb = (Color){tt->fg.r * 0.35f, tt->fg.g * 0.35f, tt->fg.b * 0.35f, 1.0f};
            }
            Color toast_bd = {tb.r, tb.g, tb.b, talpha};
            Color toast_fg = {tt->fg.r, tt->fg.g, tt->fg.b, talpha};

            /* Rounded, soft-shadowed panel (shared helper). Shadow alpha
             * follows the toast fade so it lifts off smoothly. */
            draw_panel_chrome(r, toast_x, toast_y, toast_w, toast_h,
                              toast_bg, toast_bd, 9.0f * dpi, dpi,
                              14.0f * dpi, 0.32f * talpha);
            renderer_flush_rrects(r);

            /* Text */
            f32 text_x = toast_x + 10.0f * dpi;
            f32 text_y = toast_y + (toast_h - ui_ch) / 2.0f;
            draw_text_ex(r, app->toast_message, text_x, text_y,
                toast_fg, msg_len, ui_cw);

            renderer_flush_rects(r);
            renderer_flush_glyphs(r);
            renderer_reset_ui_scale(r);
        } else {
            /* Auto-dismiss */
            app->toast_start_time = 0;
            app->toast_message[0] = '\0';
        }
    }

    /* Previous-run crash banner — on top, persists until dismissed. */
    if (app->crash_banner_active) {
        render_crash_banner(app);
        renderer_flush_rects(r);
        renderer_flush_rrects(r);
        renderer_flush_glyphs(r);
    }

    /* Command history popup (Option+Up) */
    if (app->cmd_history_active) {
        Terminal *ht = app_focused_terminal(app);
        if (ht) {
            const Theme *t = app->config.theme;
            f32 cw = r->font.cell_width;
            f32 ch = r->font.cell_height;
            f32 top_gap = app->config.style.terminal_top_gap * app->dpi_scale;
            f32 pane_x = app->sidebar_width + app->padding;
            f32 pane_y = app->tab_bar_height + top_gap;
            f32 pane_w = (f32)app->fb_width - pane_x - app->padding;
            f32 pane_h = (f32)app->fb_height - pane_y -
                         app->status_bar_height - app->padding;

            Tab *active_tab = app_active_tab(app);
            if (active_tab && active_tab->split != SPLIT_NONE) {
                if (active_tab->split_root >= 0) {
                    /* Multi-pane layout tree — look up the active pane's
                     * rect directly so the popup follows nested splits
                     * (binary split_ratio math doesn't generalise here
                     * and was the reason the popup snapped to the top-
                     * left when more than two panes were live). */
                    PaneRect rects[MAX_SPLIT_PANES];
                    tab_split_layout_rects(active_tab, pane_x, pane_y,
                                           pane_w, pane_h,
                                           app->dpi_scale, rects);
                    i32 ap = active_tab->active_pane;
                    i32 pc = tab_split_pane_count(active_tab);
                    if (ap < 0 || ap >= pc) ap = 0;
                    PaneRect ar = rects[ap];
                    if (ar.w > 1.0f && ar.h > 1.0f) {
                        pane_x = ar.x;
                        pane_y = ar.y;
                        pane_w = ar.w;
                        pane_h = ar.h;
                    }
                } else if (active_tab->split_pane_count >= 2) {
                    /* Only apply binary split-ratio math when the tab
                     * really still has ≥2 panes. A collapsed tree may
                     * leave `split` set with a stale split_ratio after
                     * the last sibling was removed; falling through
                     * would anchor the popup to a divider that no
                     * longer exists. The outer if(...) already requires
                     * split != SPLIT_NONE so this is the only extra
                     * guard needed. */
                    f32 divider = 2.0f * app->dpi_scale;
                    f32 ratio = active_tab->split_ratio;
                    if (ratio < 0.1f) ratio = 0.1f;
                    if (ratio > 0.9f) ratio = 0.9f;

                    if (active_tab->split == SPLIT_H) {
                        f32 div = pane_w * ratio;
                        if (active_tab->active_pane == 1) {
                            f32 x2 = pane_x + div + divider * 0.5f;
                            pane_w = pane_x + pane_w - x2;
                            pane_x = x2;
                        } else {
                            pane_w = div - divider * 0.5f;
                        }
                    } else if (active_tab->split == SPLIT_V) {
                        f32 div = pane_h * ratio;
                        if (active_tab->active_pane == 1) {
                            f32 y2 = pane_y + div + divider * 0.5f;
                            pane_h = pane_y + pane_h - y2;
                            pane_y = y2;
                        } else {
                            pane_h = div - divider * 0.5f;
                        }
                    }
                }
            }

            f32 base_x = pane_x;
            f32 base_y = pane_y + (f32)ht->cursor_y * ch;

            i32 count = app->cmd_history_count;
            f32 item_h = ch + 6;
            f32 popup_h = (count > 0 ? (f32)count * item_h : (ch + 10)) + 8;
            f32 popup_w = 0;
            for (i32 i = 0; i < count; i++) {
                f32 tw = (f32)strlen(app->cmd_history[i]) * cw + 24;
                if (tw > popup_w) popup_w = tw;
            }
            if (count == 0) popup_w = (f32)strlen("No command history yet") * cw + 24;
            if (popup_w < 200) popup_w = 200;
            f32 max_popup_w = pane_w - 8.0f * app->dpi_scale;
            if (max_popup_w < 120.0f * app->dpi_scale) max_popup_w = 120.0f * app->dpi_scale;
            if (popup_w > max_popup_w) popup_w = max_popup_w;

            f32 px = base_x;
            f32 py = base_y - popup_h - 4; /* above cursor */
            if (px + popup_w > pane_x + pane_w) px = pane_x + pane_w - popup_w;
            if (px < pane_x) px = pane_x;
            if (py < pane_y) py = base_y + ch + 4; /* below if no room */
            if (py + popup_h > pane_y + pane_h) py = pane_y + pane_h - popup_h;
            if (py < pane_y) py = pane_y;

            Color popup_border = t ? t->border : (Color){0.3f, 0.3f, 0.35f, 0.9f};
            popup_border.a = 0.95f;
            Color popup_bg = t ? t->sidebar_bg : (Color){0.12f, 0.12f, 0.15f, 0.97f};
            popup_bg.a = 0.97f;
            Color popup_selected = t ? t->sidebar_active : (Color){0.22f, 0.35f, 0.55f, 0.9f};
            popup_selected.a = 0.95f;
            Color popup_text = t ? t->sidebar_fg : (Color){0.7f, 0.7f, 0.73f, 1.0f};
            Color popup_text_selected = t ? t->fg : (Color){0.95f, 0.95f, 0.97f, 1.0f};

            /* Background with border */
            renderer_draw_rect(r, px - 1, py - 1, popup_w + 2, popup_h + 2,
                              popup_border);
            renderer_draw_rect(r, px, py, popup_w, popup_h,
                              popup_bg);
            renderer_flush_rects(r);

            f32 dpi = app->dpi_scale;
            f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
            renderer_set_ui_scale(r, ui_cw, ui_ch);

            if (count > 0) {
                for (i32 i = 0; i < count; i++) {
                    f32 iy = py + 4 + (f32)i * item_h;
                    bool selected = (i == app->cmd_history_selected);

                    if (selected) {
                        renderer_draw_rect(r, px + 2, iy, popup_w - 4, item_h,
                                          popup_selected);
                    }

                    /* Command text */
                    Color cmd_clr = selected ? popup_text_selected : popup_text;
                    i32 max_chars = (i32)((popup_w - 20) / ui_cw);
                    draw_text_ex(r, app->cmd_history[i], px + 10, iy + 3,
                                cmd_clr, max_chars, ui_cw);
                }
            } else {
                draw_text_ex(r, "No command history yet", px + 12, py + 7,
                            popup_text, 32, ui_cw);
            }

            renderer_flush_rects(r);
            renderer_flush_glyphs(r);
            renderer_reset_ui_scale(r);
        }
    }

    /* Settings overlay (on top of everything) */
    settings_render(&app->settings, r, (f32)app->fb_width, (f32)app->fb_height, app->dpi_scale);

    /* In-app transcript viewer — renders the parsed session events as a
     * scrollable list of role-colored rows with wrapping text. */
    {
        static bool s_tv_was_open = false;
        static Anim s_tv_open_anim = {0};
        if (app->transcript_viewer_active != s_tv_was_open) {
            s_tv_was_open = app->transcript_viewer_active;
            if (app->transcript_viewer_active) anim_start(&s_tv_open_anim, MODAL_OPEN_DUR_LARGE);
        }
    if (app->transcript_viewer_active) {
        const Theme *t = app->config.theme;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        f32 pw = (f32)app->fb_width  * 0.70f; if (pw < 600.0f * dpi) pw = 600.0f * dpi;
        f32 ph = (f32)app->fb_height * 0.80f; if (ph < 400.0f * dpi) ph = 400.0f * dpi;
        f32 ppx = ((f32)app->fb_width  - pw) * 0.5f;
        f32 ppy = ((f32)app->fb_height - ph) * 0.5f;
        f32 anim_t = anim_progress(&s_tv_open_anim);
        f32 panel_scale = anim_lerp(MODAL_OPEN_SCALE_FROM, 1.0f, ease_apply(anim_t, MODAL_OPEN_EASE));
        f32 panel_alpha = ease_apply(anim_t, MODAL_OPEN_EASE);
        if (anim_done(&s_tv_open_anim)) { panel_scale = 1.0f; panel_alpha = 1.0f; }
        f32 sx = ppx + (pw - pw * panel_scale) * 0.5f;
        f32 sy = ppy + (ph - ph * panel_scale) * 0.5f - (1.0f - ease_apply(anim_t, MODAL_OPEN_EASE)) * 12.0f * dpi;
        ppx = sx; ppy = sy; pw *= panel_scale; ph *= panel_scale;

        /* Backdrop + panel — rounded with soft drop shadow. */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                           (Color){0, 0, 0, 0.5f * panel_alpha});
        f32 panel_radius = 12.0f * dpi;
        Color panel_bd = t->border; panel_bd.a = fmaxf(panel_bd.a, 0.7f);
        renderer_draw_rrect(r,
            ppx - 1.0f * dpi, ppy - 1.0f * dpi,
            pw + 2.0f * dpi, ph + 2.0f * dpi, panel_bd,
            panel_radius + 1.0f, panel_radius + 1.0f,
            panel_radius + 1.0f, panel_radius + 1.0f,
            26.0f * dpi, 0.45f, 0.0f, 14.0f * dpi);
        Color panel_bg = {
            fminf(1.0f, t->bg.r + 0.05f),
            fminf(1.0f, t->bg.g + 0.05f),
            fminf(1.0f, t->bg.b + 0.05f),
            0.995f
        };
        renderer_draw_rrect_simple(r, ppx, ppy, pw, ph, panel_bg, panel_radius);

        /* Title bar (56dpi) */
        f32 title_h = 56.0f * dpi;
        Color divider = {panel_bd.r, panel_bd.g, panel_bd.b, 0.35f};
        renderer_draw_rect(r, ppx + 1.0f * dpi, ppy + title_h - 1.0f * dpi,
                           pw - 2.0f * dpi, 1.0f * dpi, divider);

        /* Tool icon */
        f32 icon_bw = 28.0f * dpi;
        f32 icon_by = ppy + (title_h - icon_bw) * 0.5f;
        f32 icon_bx = ppx + 18.0f * dpi;
        ChatTool vtool = (ChatTool)app->transcript_tool;
        renderer_draw_rect(r, icon_bx, icon_by, icon_bw, icon_bw,
                           agent_icon_backdrop(vtool, t));
        i32 viw = 0, vih = 0;
        const u8 *vpx = agent_icon_rgba(vtool, &viw, &vih);
        renderer_flush_rects(r);
        if (vpx) {
            f32 pad = 2.0f * dpi;
            renderer_draw_image(r, vpx, viw, vih,
                                icon_bx + pad, icon_by + pad,
                                icon_bw - pad * 2.0f, icon_bw - pad * 2.0f);
        }

        /* Title + meta */
        f32 tx = icon_bx + icon_bw + 14.0f * dpi;
        f32 ty = ppy + 10.0f * dpi;
        i32 tmax = (i32)((ppx + pw - 120.0f * dpi - tx) / ui_cw);
        if (tmax < 1) tmax = 1;
        draw_text_ex(r, app->transcript_title, tx, ty, t->fg, tmax, ui_cw);

        char meta[96];
        snprintf(meta, sizeof meta, "%s  ·  %d events",
                 chat_tool_name(vtool), app->transcript_count);
        Color dim = { t->sidebar_fg.r, t->sidebar_fg.g, t->sidebar_fg.b, 0.58f };
        draw_text_ex(r, meta, tx, ty + ui_ch + 4.0f * dpi, dim,
                     (i32)strlen(meta), ui_cw);

        /* Close hint (top-right) */
        const char *chint = "Esc  close";
        i32 clen = (i32)strlen(chint);
        draw_text_ex(r, chint,
                     ppx + pw - (f32)clen * ui_cw - 18.0f * dpi,
                     ty + (ui_ch * 0.5f),
                     (Color){dim.r, dim.g, dim.b, 0.55f}, clen, ui_cw);

        /* Content area */
        f32 cy0 = ppy + title_h + 8.0f * dpi;
        f32 cy1 = ppy + ph - 8.0f * dpi;
        f32 content_x  = ppx + 16.0f * dpi;
        f32 content_w  = pw - 32.0f * dpi;
        i32 char_max_w = (i32)(content_w / ui_cw);
        if (char_max_w < 10) char_max_w = 10;
        f32 row_gap = 10.0f * dpi;

        /* Scroll clamping */
        if (app->transcript_scroll < 0) app->transcript_scroll = 0;
        if (app->transcript_scroll > app->transcript_count)
            app->transcript_scroll = app->transcript_count;

        /* Render events top-down. Each event has up to 4 text lines plus a
         * role header; over-long text is truncated. */
        f32 cy = cy0;
        for (i32 i = app->transcript_scroll; i < app->transcript_count && cy < cy1; i++) {
            const struct TranscriptEvent *ev = &app->transcript_events[i];

            const char *role_label;
            Color role_clr;
            switch ((ChatRole)ev->role) {
            case CHAT_ROLE_USER:
                role_label = "USER"; role_clr = (Color){0.38f, 0.75f, 1.00f, 1.0f}; break;
            case CHAT_ROLE_ASSISTANT:
                role_label = "ASSISTANT"; role_clr = (Color){0.96f, 0.58f, 0.32f, 1.0f}; break;
            case CHAT_ROLE_TOOL_USE:
                role_label = "TOOL"; role_clr = (Color){0.48f, 0.82f, 0.58f, 1.0f}; break;
            case CHAT_ROLE_TOOL_RESULT:
                role_label = "RESULT"; role_clr = (Color){0.60f, 0.68f, 0.80f, 1.0f}; break;
            default:
                role_label = "SYS"; role_clr = (Color){0.70f, 0.70f, 0.74f, 1.0f}; break;
            }

            /* Header strip: left color bar + role label + optional tool name */
            renderer_draw_rect(r, content_x, cy, 3.0f * dpi, ui_ch, role_clr);
            f32 hx = content_x + 8.0f * dpi;
            draw_text_ex(r, role_label, hx, cy, role_clr,
                         (i32)strlen(role_label), ui_cw);
            hx += (f32)strlen(role_label) * ui_cw + 8.0f * dpi;
            if (ev->tool_name && ev->tool_name[0]) {
                char tbuf[96];
                snprintf(tbuf, sizeof tbuf, "[%s]", ev->tool_name);
                draw_text_ex(r, tbuf, hx, cy,
                             (Color){role_clr.r, role_clr.g, role_clr.b, 0.7f},
                             (i32)strlen(tbuf), ui_cw);
                hx += (f32)strlen(tbuf) * ui_cw + 8.0f * dpi;
            }

            cy += ui_ch + 3.0f * dpi;

            /* Body — up to 5 wrapped lines, truncate with "…" if longer. */
            const char *body = ev->text ? ev->text : "";
            i32 body_len = (i32)strlen(body);
            const i32 max_lines = 5;
            i32 lines_drawn = 0;
            i32 pos = 0;
            while (pos < body_len && lines_drawn < max_lines && cy + ui_ch < cy1) {
                /* Collect one display line — stop at newline or char_max_w. */
                i32 end = pos;
                while (end < body_len && body[end] != '\n' &&
                       (end - pos) < char_max_w) {
                    end++;
                }
                /* Peek if text is truncated on the last allowed line. */
                bool more = (end < body_len && body[end] != '\n') ||
                            (body[end] == '\n' && end + 1 < body_len);
                char line_buf[1024];
                i32 cp = end - pos;
                if (cp > (i32)sizeof(line_buf) - 4) cp = (i32)sizeof(line_buf) - 4;
                memcpy(line_buf, body + pos, (usize)cp);
                line_buf[cp] = '\0';
                if (lines_drawn == max_lines - 1 && more) {
                    /* Last allowed line but content continues → append ellipsis */
                    i32 tail = cp > 2 ? cp - 2 : 0;
                    snprintf(line_buf + tail, sizeof(line_buf) - (usize)tail, "…");
                }
                Color body_c = { t->fg.r, t->fg.g, t->fg.b, 0.90f };
                draw_text_ex(r, line_buf, content_x + 12.0f * dpi, cy,
                             body_c, char_max_w, ui_cw);
                cy += ui_ch + 1.0f * dpi;
                lines_drawn++;
                pos = (end < body_len && body[end] == '\n') ? end + 1 : end;
                if (lines_drawn == max_lines) break;
            }

            cy += row_gap;
        }

        /* Scrollbar on the right edge */
        if (app->transcript_count > 0) {
            f32 sb_x = ppx + pw - 6.0f * dpi;
            f32 sb_y = cy0;
            f32 sb_h = cy1 - cy0;
            f32 ratio = (f32)app->transcript_scroll / (f32)app->transcript_count;
            f32 thumb_h = fmaxf(24.0f * dpi, sb_h * 0.1f);
            renderer_draw_rect(r, sb_x, sb_y, 3.0f * dpi, sb_h,
                               (Color){panel_bd.r, panel_bd.g, panel_bd.b, 0.20f});
            renderer_draw_rect(r, sb_x, sb_y + ratio * (sb_h - thumb_h),
                               3.0f * dpi, thumb_h,
                               (Color){panel_bd.r, panel_bd.g, panel_bd.b, 0.60f});
        }

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close transcript_viewer open-anim wrapper */

    /* Agent resume picker (modal — shown after clicking a session in history) */
    {
        static bool s_picker_was_open = false;
        static Anim s_picker_open_anim = {0};
        if (app->agent_picker_active != s_picker_was_open) {
            s_picker_was_open = app->agent_picker_active;
            if (app->agent_picker_active) anim_start(&s_picker_open_anim, MODAL_OPEN_DUR_LARGE);
        }
    if (app->agent_picker_active) {
        const Theme *t = app->config.theme;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        f32 pw = 440.0f * dpi;
        f32 title_h = 52.0f * dpi;
        f32 row_h   = 42.0f * dpi;
        /* Reserve a real footer band: hint glyph height + breathing room above
         * (so the last row doesn't kiss the divider) and below (so the hint
         * doesn't sit on the rounded panel edge). The hint is drawn centred
         * inside this band. */
        f32 footer_h = ui_ch + 22.0f * dpi;
        i32 rows    = app->agent_picker_row_count;
        f32 ph      = title_h + (f32)rows * row_h + footer_h;
        f32 ppx     = ((f32)app->fb_width  - pw) * 0.5f;
        f32 ppy     = ((f32)app->fb_height - ph) * 0.5f;

        /* Open animation — same pattern as command palette. */
        f32 anim_t      = anim_progress(&s_picker_open_anim);
        f32 panel_scale = anim_lerp(MODAL_OPEN_SCALE_FROM, 1.0f, ease_apply(anim_t, MODAL_OPEN_EASE));
        f32 panel_alpha = ease_apply(anim_t, MODAL_OPEN_EASE);
        if (anim_done(&s_picker_open_anim)) { panel_scale = 1.0f; panel_alpha = 1.0f; }
        f32 sx = ppx + (pw - pw * panel_scale) * 0.5f;
        f32 sy = ppy + (ph - ph * panel_scale) * 0.5f - (1.0f - ease_apply(anim_t, MODAL_OPEN_EASE)) * 8.0f * dpi;
        f32 sw = pw * panel_scale;
        f32 sh = ph * panel_scale;
        ppx = sx; ppy = sy; pw = sw; ph = sh;

        /* Dim backdrop — alpha fades in. */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                           (Color){0, 0, 0, 0.45f * panel_alpha});

        /* Border + panel + soft drop shadow — single SDF pass each.
         * The border rrect sits 1dpi behind the panel and inherits the
         * shadow; the panel itself just rounds and fills. */
        f32 panel_radius = 10.0f * dpi;
        Color panel_bd = t->border; panel_bd.a = fmaxf(panel_bd.a, 0.7f);
        renderer_draw_rrect(r,
            ppx - 1.0f * dpi, ppy - 1.0f * dpi,
            pw + 2.0f * dpi, ph + 2.0f * dpi,
            panel_bd,
            panel_radius + 1.0f, panel_radius + 1.0f,
            panel_radius + 1.0f, panel_radius + 1.0f,
            18.0f * dpi, 0.45f, 0.0f, 8.0f * dpi);
        /* Opaque panel body — the picker shows decision-critical text
         * (resume command targets) over arbitrary terminal output, so
         * frosted-glass transparency hurts legibility. Match the title
         * bar bg rather than the dimmer chrome to keep the panel read
         * as a "fresh surface" floating above the terminal. */
        Color body = {
            fminf(1.0f, t->bg.r + 0.06f),
            fminf(1.0f, t->bg.g + 0.06f),
            fminf(1.0f, t->bg.b + 0.06f),
            1.0f
        };
        renderer_draw_rrect(r, ppx, ppy, pw, ph, body,
            panel_radius, panel_radius, panel_radius, panel_radius,
            0.0f, 0.0f, 0.0f, 0.0f);

        /* Title bar with session name + subtitle */
        Color title_fg = t->fg;
        Color sub_fg   = { t->sidebar_fg.r, t->sidebar_fg.g, t->sidebar_fg.b, 0.6f };
        f32 title_y = ppy + 10.0f * dpi;
        f32 sub_y   = ppy + 10.0f * dpi + ui_ch + 4.0f * dpi;
        i32 tmax    = (i32)((pw - 28.0f * dpi) / ui_cw);
        if (tmax < 1) tmax = 1;
        draw_text_ex(r, app->agent_picker_title, ppx + 16.0f * dpi, title_y,
                     title_fg, tmax, ui_cw);
        const char *sub = "Pick an agent to continue · ⌘+R to refresh";
        draw_text_ex(r, sub, ppx + 16.0f * dpi, sub_y, sub_fg,
                     (i32)strlen(sub), ui_cw);
        /* Title divider */
        renderer_draw_rect(r, ppx + 1.0f * dpi, ppy + title_h - 1.0f * dpi,
                           pw - 2.0f * dpi, 1.0f * dpi,
                           (Color){panel_bd.r, panel_bd.g, panel_bd.b, 0.35f});
        renderer_flush_rects(r);

        /* Rows */
        for (i32 i = 0; app->agent_picker_rows && i < rows; i++) {
            const struct AgentPickerRow *rr = &app->agent_picker_rows[i];
            f32 ry = ppy + title_h + (f32)i * row_h;
            bool sel = (i == app->agent_picker_selected);

            if (sel) {
                Color hl = {
                    fminf(1.0f, t->tab_active_bg.r + 0.05f),
                    fminf(1.0f, t->tab_active_bg.g + 0.05f),
                    fminf(1.0f, t->tab_active_bg.b + 0.05f),
                    0.85f
                };
                f32 sx = ppx + 4.0f * dpi;
                f32 sy = ry + 2.0f * dpi;
                f32 sw = pw - 8.0f * dpi;
                f32 sh = row_h - 4.0f * dpi;
                renderer_draw_rect(r, sx, sy, sw, sh, hl);
                /* 1px theme-accent border on all four sides — picks up
                 * ui_accent (warm-theme identity colour) when set, otherwise
                 * falls back to the resolved chrome accent. */
                f32 bw_px = fmaxf(1.0f, dpi);
                Color acc = theme_ui_accent(t);
                renderer_draw_rect(r, sx, sy, sw, bw_px, acc);
                renderer_draw_rect(r, sx, sy + sh - bw_px, sw, bw_px, acc);
                renderer_draw_rect(r, sx, sy, bw_px, sh, acc);
                renderer_draw_rect(r, sx + sw - bw_px, sy, bw_px, sh, acc);
            }

            /* Icon: same treatment as the history list — white bg + brand PNG. */
            f32 bw = 26.0f * dpi;
            f32 bh = 26.0f * dpi;
            f32 bx = ppx + 16.0f * dpi;
            f32 by = ry + (row_h - bh) * 0.5f;

            if (rr->tool > 0) {
                ChatTool tt = (ChatTool)rr->tool;
                AgentTint tint = agent_icon_tint(tt);
                i32 iw = 0, ih = 0;
                const u8 *px = agent_icon_rgba(tt, &iw, &ih);
                /* No tile backdrop — agent PNGs sit directly on the panel
                 * surface so the picker reads as a clean icon column without
                 * the bright squares clashing with the theme. */
                if (px) {
                    renderer_flush_rects(r);
                    f32 pad = 2.0f * dpi;
                    renderer_draw_image(r, px, iw, ih,
                                        bx + pad, by + pad,
                                        bw - pad * 2.0f, bh - pad * 2.0f);
                } else {
                    const char *ltr = agent_icon_letter(tt);
                    draw_text_ex(r, ltr,
                                 bx + (bw - ui_cw) * 0.5f,
                                 by + (bh - ui_ch) * 0.5f,
                                 (Color){tint.r, tint.g, tint.b, 1.0f}, 1, ui_cw);
                }
            } else {
                /* "View transcript" row — outline-only square placeholder
                 * (no fill, no tile backdrop) so it reads as a neutral icon
                 * matching the now-transparent agent rows above. */
                f32 pad = 6.0f * dpi;
                f32 ix = bx + pad, iy = by + pad;
                f32 iw2 = bw - pad * 2.0f, ih2 = bh - pad * 2.0f;
                f32 bw_px = fmaxf(1.0f, dpi);
                Color stroke = { t->fg.r, t->fg.g, t->fg.b, 0.55f };
                renderer_draw_rect(r, ix, iy, iw2, bw_px, stroke);
                renderer_draw_rect(r, ix, iy + ih2 - bw_px, iw2, bw_px, stroke);
                renderer_draw_rect(r, ix, iy, bw_px, ih2, stroke);
                renderer_draw_rect(r, ix + iw2 - bw_px, iy, bw_px, ih2, stroke);
            }
            renderer_flush_rects(r);

            f32 text_x  = bx + bw + 14.0f * dpi;
            f32 label_y = ry + (row_h - ui_ch) * 0.5f - 1.0f * dpi;
            i32 lmax    = (i32)((ppx + pw - 16.0f * dpi - text_x) / ui_cw);
            if (lmax < 1) lmax = 1;
            draw_text_ex(r, rr->label, text_x, label_y,
                         (Color){t->fg.r, t->fg.g, t->fg.b, 0.95f}, lmax, ui_cw);

            /* Kind tag (small, right side) */
            const char *tag =
                (rr->kind == 0) ? "resume"
              : (rr->kind == 1) ? "new"
              : (rr->kind == 3) ? "continue"
              :                   "view";
            i32 tlen = (i32)strlen(tag);
            f32 tx = ppx + pw - (f32)tlen * ui_cw - 14.0f * dpi;
            Color tcol = { t->sidebar_fg.r, t->sidebar_fg.g, t->sidebar_fg.b, 0.55f };
            draw_text_ex(r, tag, tx, label_y, tcol, tlen, ui_cw);
        }

        /* Footer separator + hint, centred inside the reserved footer band. */
        f32 footer_top = ppy + ph - footer_h;
        renderer_draw_rect(r, ppx + 1.0f * dpi, footer_top,
                           pw - 2.0f * dpi, 1.0f * dpi,
                           (Color){panel_bd.r, panel_bd.g, panel_bd.b, 0.25f});
        const char *hint = "Enter select  ·  Esc cancel  ·  ⌘+R refresh";
        i32 hlen = (i32)strlen(hint);
        f32 hx = ppx + (pw - (f32)hlen * ui_cw) * 0.5f;
        f32 hy = footer_top + (footer_h - ui_ch) * 0.5f;
        Color hcol = { t->sidebar_fg.r, t->sidebar_fg.g, t->sidebar_fg.b, 0.5f };
        draw_text_ex(r, hint, hx, hy, hcol, hlen, ui_cw);

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    }   /* close agent picker open-anim wrapper */

    /* Sidebar drag ghost — follows the cursor while dragging a file entry */
    if (app->fb_drag_active && app->fb_drag_src_entry >= 0 &&
        app->fb_drag_src_entry < app->filebrowser.entry_count) {
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);
        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;

        FileEntry *se = &app->filebrowser.entries[app->fb_drag_src_entry];
        const char *name = se->name;
        i32 nlen = (i32)strlen(name);
        if (nlen > 48) nlen = 48;
        f32 pad = 8.0f * dpi;
        f32 w = (f32)nlen * ui_cw + pad * 2.0f;
        f32 h = ui_ch + pad;
        f32 gx = app->fb_drag_cur_x + 12.0f * dpi;
        f32 gy = app->fb_drag_cur_y + 12.0f * dpi;
        Color bg = { 0.12f, 0.13f, 0.17f, 0.92f };
        Color bd = { t->ansi[4].r, t->ansi[4].g, t->ansi[4].b, 0.85f };
        renderer_draw_rect(r, gx - 1, gy - 1, w + 2, h + 2, bd);
        renderer_draw_rect(r, gx, gy, w, h, bg);
        renderer_flush_rects(r);
        draw_text_ex(r, name, gx + pad, gy + (h - ui_ch) * 0.5f, t->fg, nlen, ui_cw);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }

    /* File-browser context menu — rendered above the sidebar drag ghost so
     * a click anywhere on the menu still hits the menu. */
    if (app->fb_ctx_menu_active) {
        const Theme *t = app->config.theme;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        f32 menu_w = 200.0f * dpi;
        f32 row_h  = 26.0f * dpi;
        f32 vpad   = 6.0f * dpi;
        i32 menu_rows = 8;
        f32 menu_h = vpad * 2.0f + row_h * (f32)menu_rows;

        f32 mx = app->fb_ctx_menu_x;
        f32 my = app->fb_ctx_menu_y;
        if (mx + menu_w > (f32)app->fb_width  - 4.0f * dpi) mx = (f32)app->fb_width  - menu_w - 4.0f * dpi;
        if (my + menu_h > (f32)app->fb_height - 4.0f * dpi) my = (f32)app->fb_height - menu_h - 4.0f * dpi;
        if (mx < 4.0f * dpi) mx = 4.0f * dpi;
        if (my < 4.0f * dpi) my = 4.0f * dpi;
        app->fb_ctx_menu_x = mx;
        app->fb_ctx_menu_y = my;

        Color panel_bg = {
            fminf(1.0f, t->bg.r + 0.06f),
            fminf(1.0f, t->bg.g + 0.06f),
            fminf(1.0f, t->bg.b + 0.06f),
            0.985f
        };
        Color panel_bd = t->border;
        panel_bd.a = fmaxf(panel_bd.a, 0.55f);

        /* Border + soft drop shadow + rounded body in single SDF passes —
         * replaces the previous two stacked alpha rects fake-shadow trick. */
        f32 menu_radius = 8.0f * dpi;
        renderer_draw_rrect(r,
            mx - 1.0f * dpi, my - 1.0f * dpi,
            menu_w + 2.0f * dpi, menu_h + 2.0f * dpi, panel_bd,
            menu_radius + 1.0f, menu_radius + 1.0f,
            menu_radius + 1.0f, menu_radius + 1.0f,
            14.0f * dpi, 0.32f, 0.0f, 5.0f * dpi);
        renderer_draw_rrect_simple(r, mx, my, menu_w, menu_h, panel_bg, menu_radius);

        if (app->fb_ctx_menu_selected >= 0 && app->fb_ctx_menu_selected < menu_rows) {
            f32 ry = my + vpad + (f32)app->fb_ctx_menu_selected * row_h;
            Color hl = {
                fminf(1.0f, t->tab_active_bg.r + 0.05f),
                fminf(1.0f, t->tab_active_bg.g + 0.05f),
                fminf(1.0f, t->tab_active_bg.b + 0.05f),
                0.80f
            };
            renderer_draw_rect(r, mx + 3.0f * dpi, ry,
                               menu_w - 6.0f * dpi, row_h, hl);
        }
        renderer_flush_rects(r);

        struct { const char *label; const char *shortcut; } rows[8] = {
            { "Open",       ""           },
            { "Rename",     "F2"         },
            { "Delete",     "Del"        },
            { "Copy",       "Cmd C"      },
            { "Cut",        "Cmd X"      },
            { "Paste",      "Cmd V"      },
            { "New Folder", "Shift Cmd N"},
            { "New File",   "Cmd N"      },
        };
        for (i32 i = 0; i < menu_rows; i++) {
            f32 ry = my + vpad + (f32)i * row_h;
            f32 ty = ry + (row_h - ui_ch) * 0.5f;
            Color fg = t->fg; fg.a = 0.95f;
            /* Paste row is dimmed if clipboard empty */
            if (i == 5 && !app->file_clipboard.has) fg.a = 0.40f;
            draw_text_ex(r, rows[i].label, mx + 14.0f * dpi, ty, fg,
                         (i32)((menu_w - 28.0f * dpi) / ui_cw), ui_cw);
            i32 slen = (i32)strlen(rows[i].shortcut);
            if (slen > 0) {
                f32 sx = mx + menu_w - (f32)slen * ui_cw - 14.0f * dpi;
                Color dim = { fg.r, fg.g, fg.b, 0.45f };
                draw_text_ex(r, rows[i].shortcut, sx, ty, dim, slen, ui_cw);
            }
        }
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }

    /* File-browser modal prompt (rename / new file / new folder).
     * Rounded, soft-shadowed panel matching the command-palette design
     * language: accent header bar + title, focused-input border, and
     * keyboard keycap chips for the Enter/Esc hints. Flush order is the
     * canonical flat→rrect→glyph: shapes commit before the caret rect,
     * and all glyphs paint last so labels sit above caps and the field. */
    if (app->fb_prompt_active) {
        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        f32 dw  = 460.0f * dpi;
        f32 dh  = 156.0f * dpi;
        f32 dx  = ((f32)app->fb_width  - dw) * 0.5f;
        f32 dy  = ((f32)app->fb_height - dh) * 0.5f;
        f32 pad = 20.0f * dpi;
        f32 radius = 12.0f * dpi;

        Color accent = theme_ui_accent(t);

        /* ---- Shapes (flat backdrop + rrect panel/field/caps) ---- */
        renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                           (Color){0, 0, 0, 0.50f});

        /* Header hairline separator (flat, under the title row). */
        f32 sep_y = dy + pad + ui_ch + 9.0f * dpi;
        Color sep = t->border; sep.a = fmaxf(sep.a, 0.6f) * 0.35f;
        renderer_draw_rect(r, dx + pad, sep_y, dw - 2.0f * pad, 1.0f * dpi, sep);
        renderer_flush_rects(r);

        /* Panel: border underlay + soft-shadowed body. */
        Color panel_border = t->border; panel_border.a = fmaxf(panel_border.a, 0.7f);
        renderer_draw_rrect(r, dx - 1.0f * dpi, dy - 1.0f * dpi,
                            dw + 2.0f * dpi, dh + 2.0f * dpi, panel_border,
                            radius + 1.0f, radius + 1.0f, radius + 1.0f, radius + 1.0f,
                            24.0f * dpi, 0.42f, 0.0f, 12.0f * dpi);
        Color panel_bg = { t->tab_inactive_bg.r, t->tab_inactive_bg.g,
                           t->tab_inactive_bg.b, 0.99f };
        renderer_draw_rrect_simple(r, dx, dy, dw, dh, panel_bg, radius);

        /* Accent header bar to the left of the title. */
        renderer_draw_rrect_simple(r, dx + pad, dy + pad + 1.0f * dpi,
                                   3.0f * dpi, ui_ch - 2.0f * dpi, accent, 1.5f * dpi);

        /* Input field — recessed fill, accent (focused) border. */
        f32 fx = dx + pad;
        f32 fy = dy + pad + ui_ch + 20.0f * dpi;
        f32 fw = dw - 2.0f * pad;
        f32 fh = 38.0f * dpi;
        Color inp_bg = t->sidebar_bg; inp_bg.a = 1.0f;
        Color inp_border = accent; inp_border.a = fmaxf(inp_border.a, 0.85f);
        renderer_draw_rrect_bordered(r, fx, fy, fw, fh, inp_bg, inp_border,
                                     1.5f * dpi, 9.0f * dpi, 9.0f * dpi,
                                     9.0f * dpi, 9.0f * dpi, 0.0f, 0.0f, 0.0f, 0.0f);

        /* Footer keycap chips (Enter / Esc). Geometry computed once and
         * reused for both the chip rects (here) and labels (glyph phase). */
        const char *k1 = "Enter", *a1 = "confirm";
        const char *k2 = "Esc",   *a2 = "cancel";
        f32 cap_pad = 7.0f * dpi;
        f32 cap_h   = ui_ch + 7.0f * dpi;
        f32 hint_y  = fy + fh + 22.0f * dpi;
        f32 cap_y   = hint_y - 3.5f * dpi;
        f32 cap1_x  = dx + pad;
        f32 cap1_w  = (f32)strlen(k1) * ui_cw + cap_pad * 2.0f;
        f32 a1_x    = cap1_x + cap1_w + 8.0f * dpi;
        f32 a1_w    = (f32)strlen(a1) * ui_cw;
        f32 cap2_x  = a1_x + a1_w + 18.0f * dpi;
        f32 cap2_w  = (f32)strlen(k2) * ui_cw + cap_pad * 2.0f;
        f32 a2_x    = cap2_x + cap2_w + 8.0f * dpi;
        Color cap_bg = t->tab_active_bg;
        Color cap_bd = t->border; cap_bd.a = fmaxf(cap_bd.a, 0.6f);
        renderer_draw_rrect_bordered(r, cap1_x, cap_y, cap1_w, cap_h, cap_bg, cap_bd,
                                     1.0f * dpi, 5.0f * dpi, 5.0f * dpi,
                                     5.0f * dpi, 5.0f * dpi, 0.0f, 0.0f, 0.0f, 0.0f);
        renderer_draw_rrect_bordered(r, cap2_x, cap_y, cap2_w, cap_h, cap_bg, cap_bd,
                                     1.0f * dpi, 5.0f * dpi, 5.0f * dpi,
                                     5.0f * dpi, 5.0f * dpi, 0.0f, 0.0f, 0.0f, 0.0f);

        renderer_flush_rrects(r);

        /* ---- Caret (flat rect, above the committed field) ---- */
        f64 pnow = platform_time_sec();
        if (fmod(pnow, 1.06) < 0.53) {
            f32 cx = fx + 12.0f * dpi +
                     (f32)utf8_len((const u8 *)app->fb_prompt_buf,
                                   (usize)app->fb_prompt_len) * ui_cw;
            f32 cx_max = fx + fw - 6.0f * dpi;
            if (cx > cx_max) cx = cx_max;
            Color caret = accent;
            renderer_draw_rect(r, cx, fy + 7.0f * dpi,
                               2.0f * dpi, fh - 14.0f * dpi, caret);
            renderer_flush_rects(r);
        }

        /* ---- Text (glyph phase, paints last) ---- */
        const char *title = app->fb_prompt_mode == 0 ? "Rename"
                          : app->fb_prompt_mode == 2 ? "New File" : "New Folder";
        draw_text_ex(r, title, dx + pad + 11.0f * dpi, dy + pad,
                     t->fg, (i32)strlen(title), ui_cw);
        draw_text_ex(r, app->fb_prompt_buf, fx + 12.0f * dpi,
                     fy + (fh - ui_ch) * 0.5f, t->fg, app->fb_prompt_len, ui_cw);

        Color cap_fg = t->fg;        cap_fg.a *= 0.92f;
        Color act_fg = t->sidebar_fg; act_fg.a *= 0.60f;
        draw_text_ex(r, k1, cap1_x + cap_pad, hint_y, cap_fg, (i32)strlen(k1), ui_cw);
        draw_text_ex(r, a1, a1_x, hint_y, act_fg, (i32)strlen(a1), ui_cw);
        draw_text_ex(r, k2, cap2_x + cap_pad, hint_y, cap_fg, (i32)strlen(k2), ui_cw);
        draw_text_ex(r, a2, a2_x, hint_y, act_fg, (i32)strlen(a2), ui_cw);

        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }

    /* Transfer progress strip inside the sidebar (takes precedence over
     * the transient fb_status line because it carries live information).
     * The drag-out / paste flow flips sftp_transfer_active; this strip
     * stays visible for the whole transfer so the user can watch rate /
     * ETA without moving the cursor. The bottom-left overlay still
     * renders richer details for when the sidebar is closed. */
    if (app->sidebar_visible && sftp_transfer_active()) {
        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        f32 h = ui_ch * 2 + 20.0f * dpi;   /* two text rows + bar */
        f32 w = app->sidebar_width;
        f32 y = (f32)app->fb_height - h - app->status_bar_height;

        f32 op = app->config.opacity;
        Color panel_bg = {
            fminf(1.0f, t->bg.r + 0.05f),
            fminf(1.0f, t->bg.g + 0.05f),
            fminf(1.0f, t->bg.b + 0.09f),
            0.98f * op
        };
        bool up = sftp_transfer_is_upload();
        Color accent = t->ansi[up ? 1 : 4];
        Color accent_bar = accent; accent_bar.a *= op;
        /* Top accent line + panel body */
        renderer_draw_rect(r, 0, y, w, h, panel_bg);
        renderer_draw_rect(r, 0, y, w, 1.5f * dpi, accent_bar);

        const char *fname = sftp_transfer_filename();
        const char *arrow = up ? "up" : "dn";
        u64 done  = sftp_transfer_bytes_done();
        u64 total = sftp_transfer_bytes_total();
        f64 bps   = sftp_transfer_speed_bps();
        f64 eta   = sftp_transfer_eta_sec();
        f32 pct   = (total > 0) ? (f32)done / (f32)total : 0;
        if (pct > 1.0f) pct = 1.0f;
        if (pct < 0.0f) pct = 0.0f;

        /* Row 1: arrow chip + filename */
        f32 tx = 10.0f * dpi;
        f32 ty = y + 6.0f * dpi;
        f32 chip_w = 22.0f * dpi;
        f32 chip_h = ui_ch + 4.0f * dpi;
        renderer_draw_rect(r, tx, ty - 2.0f * dpi, chip_w, chip_h,
                           (Color){accent.r, accent.g, accent.b, 0.22f});
        draw_text_ex(r, arrow,
                     tx + (chip_w - 2*ui_cw) * 0.5f,
                     ty + (chip_h - ui_ch) * 0.5f - 2.0f * dpi,
                     accent, 2, ui_cw);

        f32 name_x = tx + chip_w + 6.0f * dpi;
        i32 name_max = (i32)((w - name_x - 8.0f * dpi) / ui_cw);
        if (name_max < 4) name_max = 4;
        i32 flen = (i32)strlen(fname);
        if (flen > name_max) flen = name_max;
        draw_text_ex(r, fname, name_x, ty, t->fg, flen, ui_cw);

        /* Row 2: "45 MB/s · 40% · ETA 2:35"  (or elapsed when unknown) */
        char line[96];
        char speed_str[24], eta_str[24];
        /* Inline bytes/second formatter */
        if (bps < 1024.0) snprintf(speed_str, sizeof(speed_str), "%.0f B/s", bps);
        else if (bps < 1024.0*1024) snprintf(speed_str, sizeof(speed_str), "%.1f KB/s", bps/1024.0);
        else if (bps < 1024.0*1024*1024) snprintf(speed_str, sizeof(speed_str), "%.1f MB/s", bps/(1024.0*1024));
        else snprintf(speed_str, sizeof(speed_str), "%.2f GB/s", bps/(1024.0*1024*1024));

        if (eta >= 0) {
            if (eta < 60) snprintf(eta_str, sizeof(eta_str), "ETA %ds", (int)(eta + 0.5));
            else if (eta < 3600) snprintf(eta_str, sizeof(eta_str), "ETA %d:%02d",
                                          (int)eta / 60, (int)eta % 60);
            else snprintf(eta_str, sizeof(eta_str), "ETA %dh %dm",
                           (int)eta / 3600, ((int)eta % 3600) / 60);
        } else {
            f64 el = sftp_transfer_elapsed_sec();
            if (el < 60) snprintf(eta_str, sizeof(eta_str), "%ds elapsed", (int)(el + 0.5));
            else snprintf(eta_str, sizeof(eta_str), "%d:%02d elapsed",
                           (int)el / 60, (int)el % 60);
        }

        if (total > 0)
            snprintf(line, sizeof(line), "%s · %d%% · %s",
                     speed_str, (int)(pct * 100 + 0.5f), eta_str);
        else
            snprintf(line, sizeof(line), "%s · %s", speed_str, eta_str);

        Color dim = t->sidebar_fg; dim.a *= 0.80f;
        i32 llen = (i32)strlen(line);
        i32 lmax = (i32)((w - 16.0f * dpi) / ui_cw);
        if (llen > lmax) llen = lmax;
        f32 ty2 = ty + ui_ch + 4.0f * dpi;
        draw_text_ex(r, line, tx, ty2, dim, llen, ui_cw);

        /* Row 3: progress bar */
        f32 bar_x = tx;
        f32 bar_y = y + h - 7.0f * dpi;
        f32 bar_w = w - tx - 10.0f * dpi;
        f32 bar_h = 4.0f * dpi;
        renderer_draw_rect(r, bar_x, bar_y, bar_w, bar_h,
                           (Color){t->sidebar_bg.r, t->sidebar_bg.g, t->sidebar_bg.b, 0.9f});
        if (total > 0) {
            renderer_draw_rect(r, bar_x, bar_y, bar_w * pct, bar_h, accent);
        } else {
            /* Indeterminate shuttle when size unknown */
            f64 phase = fmod(sftp_transfer_elapsed_sec() * 0.6, 1.0);
            f32 seg_w = bar_w * 0.25f;
            f32 seg_x = bar_x + (bar_w - seg_w) * (f32)phase;
            renderer_draw_rect(r, seg_x, bar_y, seg_w, bar_h, accent);
        }

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
    /* Transient filebrowser status line (bottom of sidebar) — only when
     * no live transfer is occupying the same footer row. */
    else if (app->fb_status[0] && platform_time_sec() < app->fb_status_until &&
        app->sidebar_visible) {
        const Theme *t = app->config.theme;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);
        f32 h = ui_ch + 10.0f * dpi;
        f32 w = app->sidebar_width;
        f32 y = (f32)app->fb_height - h - app->status_bar_height;
        f32 op = app->config.opacity;
        Color bg = t->tab_active_bg; bg.a *= op;
        Color rule = t->ansi[4];     rule.a *= op;
        renderer_draw_rect(r, 0, y, w, h, bg);
        renderer_draw_rect(r, 0, y, w, 1.5f * dpi, rule);
        renderer_flush_rects(r);
        i32 slen = (i32)strlen(app->fb_status);
        i32 maxc = (i32)((w - 16.0f * dpi) / ui_cw);
        if (slen > maxc) slen = maxc;
        draw_text_ex(r, app->fb_status, 8.0f * dpi, y + (h - ui_ch) * 0.5f,
                     t->fg, slen, ui_cw);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }

    /* Tab context menu (topmost UI, below the benchmark overlay) */
    if (app->tab_ctx_menu_active) {
        const Theme *t = app->config.theme;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        i32 ti = app->tab_ctx_menu_tab_index;
        bool tab_in_group = (ti >= 0 && ti < app->tab_count &&
                             app->tabs[ti].group_index >= 0 &&
                             app->tabs[ti].group_index < MAX_TAB_GROUPS &&
                             app->tab_groups[app->tabs[ti].group_index].used);

        /* 230pt: wide enough that "Change Title" + "Shift Cmd E" don't collide
         * (label ends ~110pt, shortcut starts ~128pt — see row draw below). */
        f32 menu_w = 230.0f * dpi;
        f32 row_h  = 26.0f * dpi;
        f32 vpad   = 6.0f * dpi;
        i32 menu_rows = tab_in_group ? 4 : 3;
        f32 menu_h = vpad * 2.0f + row_h * (f32)menu_rows;

        /* Clamp within window */
        f32 mx = app->tab_ctx_menu_x;
        f32 my = app->tab_ctx_menu_y;
        if (mx + menu_w > (f32)app->fb_width - 4.0f * dpi)
            mx = (f32)app->fb_width - menu_w - 4.0f * dpi;
        if (my + menu_h > (f32)app->fb_height - 4.0f * dpi)
            my = (f32)app->fb_height - menu_h - 4.0f * dpi;
        if (mx < 4.0f * dpi) mx = 4.0f * dpi;
        if (my < 4.0f * dpi) my = 4.0f * dpi;
        /* Persist the clamped origin so hit-testing in main.c sees the same rect */
        app->tab_ctx_menu_x = mx;
        app->tab_ctx_menu_y = my;

        /* Soft-shadowed rounded panel chrome (shared helper). Committed
         * immediately so the flat hover highlight below lands on top of it. */
        Color panel_bg = {
            fminf(1.0f, t->bg.r + 0.06f),
            fminf(1.0f, t->bg.g + 0.06f),
            fminf(1.0f, t->bg.b + 0.06f),
            0.985f
        };
        Color panel_bd = t->border;
        panel_bd.a = fmaxf(panel_bd.a, 0.55f);
        draw_panel_chrome(r, mx, my, menu_w, menu_h, panel_bg, panel_bd,
                          8.0f * dpi, dpi, 14.0f * dpi, 0.32f);
        renderer_flush_rrects(r);

        /* Hover highlight for selected row */
        if (app->tab_ctx_menu_selected >= 0 && app->tab_ctx_menu_selected < menu_rows) {
            f32 ry = my + vpad + (f32)app->tab_ctx_menu_selected * row_h;
            Color hl = {
                fminf(1.0f, t->tab_active_bg.r + 0.05f),
                fminf(1.0f, t->tab_active_bg.g + 0.05f),
                fminf(1.0f, t->tab_active_bg.b + 0.05f),
                0.80f
            };
            renderer_draw_rect(r, mx + 3.0f * dpi, ry,
                               menu_w - 6.0f * dpi, row_h, hl);
        }
        renderer_flush_rects(r);

        /* Rows: label (left) + shortcut hint (right). Layout is:
         *   0: Change Title
         *   1: Close Tab
         *   2: Add to New Group
         *   3: Remove from Group (only if grouped) */
        struct { const char *label; const char *shortcut; } rows[4] = {
            { "Change Title",      "Shift Cmd E" },
            { "Close Tab",         "Cmd W" },
            { "Add to New Group",  "" },
            { "Remove from Group", "" },
        };

        for (i32 i = 0; i < menu_rows; i++) {
            f32 ry = my + vpad + (f32)i * row_h;
            f32 ty = ry + (row_h - ui_ch) * 0.5f;

            Color fg = t->fg;
            fg.a = 0.95f;
            draw_text_ex(r, rows[i].label, mx + 14.0f * dpi, ty, fg,
                         (i32)((menu_w - 28.0f * dpi) / ui_cw), ui_cw);

            /* Shortcut hint aligned to the right */
            i32 slen = (i32)strlen(rows[i].shortcut);
            if (slen > 0) {
                f32 sx = mx + menu_w - (f32)slen * ui_cw - 14.0f * dpi;
                Color dim = { fg.r, fg.g, fg.b, 0.45f };
                draw_text_ex(r, rows[i].shortcut, sx, ty, dim, slen, ui_cw);
            }
        }

        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }

    /* Terminal context menu (right-click on a terminal pane). Same visual
     * language as the tab menu — single column, 200pt wide, 4 rows.
     * Each row has a small glyph hint to its left so the user can read
     * direction at a glance. */
    if (app->term_ctx_menu_active) {
        const Theme *t = app->config.theme;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        f32 menu_w = 200.0f * dpi;
        f32 row_h  = 26.0f * dpi;
        f32 vpad   = 6.0f * dpi;
        i32 menu_rows = 4;
        f32 menu_h = vpad * 2.0f + row_h * (f32)menu_rows;

        f32 mx = app->term_ctx_menu_x;
        f32 my = app->term_ctx_menu_y;
        if (mx + menu_w > (f32)app->fb_width  - 4.0f * dpi)
            mx = (f32)app->fb_width  - menu_w - 4.0f * dpi;
        if (my + menu_h > (f32)app->fb_height - 4.0f * dpi)
            my = (f32)app->fb_height - menu_h - 4.0f * dpi;
        if (mx < 4.0f * dpi) mx = 4.0f * dpi;
        if (my < 4.0f * dpi) my = 4.0f * dpi;
        app->term_ctx_menu_x = mx;
        app->term_ctx_menu_y = my;

        /* Soft-shadowed rounded panel chrome (shared helper). Committed
         * immediately so the flat hover + per-row glyph cells land on top. */
        Color panel_bg = {
            fminf(1.0f, t->bg.r + 0.06f),
            fminf(1.0f, t->bg.g + 0.06f),
            fminf(1.0f, t->bg.b + 0.06f),
            0.985f
        };
        Color panel_bd = t->border;
        panel_bd.a = fmaxf(panel_bd.a, 0.55f);
        draw_panel_chrome(r, mx, my, menu_w, menu_h, panel_bg, panel_bd,
                          8.0f * dpi, dpi, 14.0f * dpi, 0.32f);
        renderer_flush_rrects(r);

        if (app->term_ctx_menu_selected >= 0 &&
            app->term_ctx_menu_selected < menu_rows) {
            f32 ry = my + vpad + (f32)app->term_ctx_menu_selected * row_h;
            Color hl = {
                fminf(1.0f, t->tab_active_bg.r + 0.05f),
                fminf(1.0f, t->tab_active_bg.g + 0.05f),
                fminf(1.0f, t->tab_active_bg.b + 0.05f),
                0.80f
            };
            renderer_draw_rect(r, mx + 3.0f * dpi, ry,
                               menu_w - 6.0f * dpi, row_h, hl);
        }

        /* Per-row glyph hint: a small filled rectangle whose position inside
         * a 12×12 cell mirrors where the new pane will land. Cheap and
         * direction-readable without depending on icon assets. */
        Color glyph_fg = t->fg; glyph_fg.a = 0.85f;
        Color cell_bd  = t->border; cell_bd.a = fmaxf(cell_bd.a, 0.45f);
        for (i32 i = 0; i < menu_rows; i++) {
            f32 ry = my + vpad + (f32)i * row_h;
            f32 cell_sz = 14.0f * dpi;
            f32 cx = mx + 14.0f * dpi;
            f32 cy = ry + (row_h - cell_sz) * 0.5f;
            /* outline */
            renderer_draw_rect(r, cx, cy, cell_sz, 1.0f * dpi, cell_bd);
            renderer_draw_rect(r, cx, cy + cell_sz - 1.0f * dpi, cell_sz, 1.0f * dpi, cell_bd);
            renderer_draw_rect(r, cx, cy, 1.0f * dpi, cell_sz, cell_bd);
            renderer_draw_rect(r, cx + cell_sz - 1.0f * dpi, cy, 1.0f * dpi, cell_sz, cell_bd);
            /* fill: half the cell on the side the new pane will appear */
            f32 half = cell_sz * 0.5f;
            switch (i) {
            case 0: /* Right */
                renderer_draw_rect(r, cx + half, cy + 2.0f * dpi,
                                   half - 2.0f * dpi, cell_sz - 4.0f * dpi, glyph_fg);
                break;
            case 1: /* Left */
                renderer_draw_rect(r, cx + 2.0f * dpi, cy + 2.0f * dpi,
                                   half - 2.0f * dpi, cell_sz - 4.0f * dpi, glyph_fg);
                break;
            case 2: /* Down */
                renderer_draw_rect(r, cx + 2.0f * dpi, cy + half,
                                   cell_sz - 4.0f * dpi, half - 2.0f * dpi, glyph_fg);
                break;
            case 3: /* Up */
                renderer_draw_rect(r, cx + 2.0f * dpi, cy + 2.0f * dpi,
                                   cell_sz - 4.0f * dpi, half - 2.0f * dpi, glyph_fg);
                break;
            }
        }
        renderer_flush_rects(r);

        const char *labels[4] = { "Split Right", "Split Left", "Split Down", "Split Up" };
        for (i32 i = 0; i < menu_rows; i++) {
            f32 ry = my + vpad + (f32)i * row_h;
            f32 ty = ry + (row_h - ui_ch) * 0.5f;
            Color fg = t->fg; fg.a = 0.95f;
            draw_text_ex(r, labels[i],
                         mx + 14.0f * dpi + 14.0f * dpi + 10.0f * dpi, ty,
                         fg, (i32)strlen(labels[i]), ui_cw);
        }
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }

    /* Tab-group chip context menu (right-click on a workspace chip).
     * Layout: top palette strip (8 colour swatches) + 5 action rows. */
    if (app->group_ctx_menu_active &&
        app->group_ctx_menu_group_index >= 0 &&
        app->group_ctx_menu_group_index < MAX_TAB_GROUPS &&
        app->tab_groups[app->group_ctx_menu_group_index].used) {
        const Theme *t = app->config.theme;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        f32 menu_w  = 220.0f * dpi;
        f32 row_h   = 26.0f * dpi;
        f32 vpad    = 6.0f * dpi;
        f32 swatch  = 16.0f * dpi;
        f32 swatch_pad = 6.0f * dpi;
        f32 swatch_row_h = swatch + swatch_pad * 2.0f;
        i32 menu_rows = 5;
        f32 menu_h = swatch_row_h + 1.0f * dpi /* separator */ +
                     vpad * 2.0f + row_h * (f32)menu_rows;

        f32 mx = app->group_ctx_menu_x;
        f32 my = app->group_ctx_menu_y;
        if (mx + menu_w > (f32)app->fb_width  - 4.0f * dpi)
            mx = (f32)app->fb_width  - menu_w - 4.0f * dpi;
        if (my + menu_h > (f32)app->fb_height - 4.0f * dpi)
            my = (f32)app->fb_height - menu_h - 4.0f * dpi;
        if (mx < 4.0f * dpi) mx = 4.0f * dpi;
        if (my < 4.0f * dpi) my = 4.0f * dpi;
        app->group_ctx_menu_x = mx;
        app->group_ctx_menu_y = my;

        /* Soft-shadowed rounded panel chrome (shared helper). Committed
         * immediately so the swatches / separator / hover land on top. */
        Color panel_bg = {
            fminf(1.0f, t->bg.r + 0.06f),
            fminf(1.0f, t->bg.g + 0.06f),
            fminf(1.0f, t->bg.b + 0.06f),
            0.985f
        };
        Color panel_bd = t->border;
        panel_bd.a = fmaxf(panel_bd.a, 0.55f);
        draw_panel_chrome(r, mx, my, menu_w, menu_h, panel_bg, panel_bd,
                          8.0f * dpi, dpi, 14.0f * dpi, 0.32f);
        renderer_flush_rrects(r);

        /* Palette swatches: 8 dots evenly spaced across menu width */
        f32 sw_total = 8.0f * swatch + 7.0f * swatch_pad;
        f32 sw_x0 = mx + (menu_w - sw_total) * 0.5f;
        f32 sw_y  = my + swatch_pad;
        i32 cur_color_idx = app_group_palette_match(
            app->tab_groups[app->group_ctx_menu_group_index].color);
        for (i32 i = 0; i < 8; i++) {
            f32 sx = sw_x0 + (f32)i * (swatch + swatch_pad);
            Color sc = app_group_palette_color(i);
            /* Background ring on the currently-active colour */
            if (i == cur_color_idx) {
                Color ring = { sc.r, sc.g, sc.b, 0.45f };
                renderer_draw_rrect_simple(r, sx - 2.0f * dpi, sw_y - 2.0f * dpi,
                                   swatch + 4.0f * dpi, swatch + 4.0f * dpi, ring,
                                   (swatch + 4.0f * dpi) * 0.5f);
            }
            draw_soft_dot(r, sx, sw_y, swatch, sc);
        }

        /* Separator under the palette */
        f32 sep_y = my + swatch_row_h;
        Color sep = { panel_bd.r, panel_bd.g, panel_bd.b, 0.35f };
        renderer_draw_rect(r, mx + 8.0f * dpi, sep_y,
                           menu_w - 16.0f * dpi, 1.0f * dpi, sep);

        f32 rows_y0 = sep_y + 1.0f * dpi + vpad;

        /* Hover highlight */
        i32 sel = app->group_ctx_menu_selected;
        if (sel >= 0 && sel < menu_rows) {
            f32 ry = rows_y0 + (f32)sel * row_h;
            Color hl = {
                fminf(1.0f, t->tab_active_bg.r + 0.05f),
                fminf(1.0f, t->tab_active_bg.g + 0.05f),
                fminf(1.0f, t->tab_active_bg.b + 0.05f),
                0.80f
            };
            renderer_draw_rect(r, mx + 3.0f * dpi, ry,
                               menu_w - 6.0f * dpi, row_h, hl);
        }
        renderer_flush_rects(r);

        bool collapsed = app->tab_groups[app->group_ctx_menu_group_index].collapsed;
        const char *labels[5] = {
            "Rename",
            "New Tab in Group",
            collapsed ? "Open Group" : "Close Group",
            "Ungroup",
            "Delete Group",
        };
        for (i32 i = 0; i < menu_rows; i++) {
            f32 ry = rows_y0 + (f32)i * row_h;
            f32 ty = ry + (row_h - ui_ch) * 0.5f;
            Color fg = t->fg;
            fg.a = (i == 4) ? 0.85f : 0.95f;
            /* "Delete Group" tinted red to read as destructive */
            if (i == 4) { fg.r = 0.95f; fg.g = 0.45f; fg.b = 0.42f; fg.a = 0.95f; }
            draw_text_ex(r, labels[i], mx + 14.0f * dpi, ty, fg,
                         (i32)((menu_w - 28.0f * dpi) / ui_cw), ui_cw);
        }
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }

    /* Bottom-left SFTP transfer overlay — shows filename, speed, ETA,
     * bytes done/total, and a gradient progress bar while an upload or
     * download is running from the sidebar (or any other code path that
     * called sftp_transfer_begin). Sits above the status bar with a
     * small gutter so it doesn't collide with the bottom-of-sidebar
     * status toast. */
    if (sftp_transfer_active()) {
        const Theme *t = app->config.theme;
        if (!t) t = &THEME_DARK;
        f32 dpi = app->dpi_scale;
        f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        f32 pw = 380.0f * dpi;
        f32 ph = 76.0f * dpi;
        f32 px = 12.0f * dpi;
        f32 py = (f32)app->fb_height - app->status_bar_height - ph - 12.0f * dpi;

        /* Border (rrect with built-in shadow) + panel body. Single SDF pass
         * each replaces the previous flat-rect double-shadow stack. */
        f32 panel_radius = 8.0f * dpi;
        Color panel_bg = {
            fminf(1.0f, t->bg.r + 0.04f),
            fminf(1.0f, t->bg.g + 0.04f),
            fminf(1.0f, t->bg.b + 0.08f),
            0.985f
        };
        Color border = t->ansi[4]; border.a = 0.75f;
        renderer_draw_rrect(r,
            px - 1*dpi, py - 1*dpi, pw + 2*dpi, ph + 2*dpi,
            border,
            panel_radius + 1.0f, panel_radius + 1.0f,
            panel_radius + 1.0f, panel_radius + 1.0f,
            14.0f * dpi, 0.32f, 0.0f, 5.0f * dpi);
        renderer_draw_rrect_simple(r, px, py, pw, ph, panel_bg, panel_radius);

        /* Collect state */
        u64 done  = sftp_transfer_bytes_done();
        u64 total = sftp_transfer_bytes_total();
        f64 bps   = sftp_transfer_speed_bps();
        f64 eta   = sftp_transfer_eta_sec();
        f64 elaps = sftp_transfer_elapsed_sec();
        bool up   = sftp_transfer_is_upload();
        f32 pct   = (total > 0) ? (f32)done / (f32)total : 0;
        if (pct > 1.0f) pct = 1.0f;
        if (pct < 0.0f) pct = 0.0f;

        /* Formatting via file-level static helpers. */
        #define FMT_BYTES(out, cap, b) do { \
            u64 _b = (b); \
            if (_b < 1024ULL) snprintf(out, cap, "%llu B", (unsigned long long)_b); \
            else if (_b < 1024ULL*1024) snprintf(out, cap, "%.1f KB", (f64)_b/1024.0); \
            else if (_b < 1024ULL*1024*1024) snprintf(out, cap, "%.2f MB", (f64)_b/(1024.0*1024)); \
            else snprintf(out, cap, "%.2f GB", (f64)_b/(1024.0*1024*1024)); \
        } while (0)
        #define FMT_BPS(out, cap, b) do { \
            f64 _r = (b); \
            if (_r < 1024.0) snprintf(out, cap, "%.0f B/s", _r); \
            else if (_r < 1024.0*1024) snprintf(out, cap, "%.1f KB/s", _r/1024.0); \
            else if (_r < 1024.0*1024*1024) snprintf(out, cap, "%.1f MB/s", _r/(1024.0*1024)); \
            else snprintf(out, cap, "%.2f GB/s", _r/(1024.0*1024*1024)); \
        } while (0)
        #define FMT_TIME(out, cap, s) do { \
            f64 _s = (s); \
            if (_s < 0)       snprintf(out, cap, "--:--"); \
            else if (_s < 60) snprintf(out, cap, "%ds", (int)(_s + 0.5)); \
            else if (_s < 3600) { \
                int _m = (int)_s / 60; int _ss = (int)_s % 60; \
                snprintf(out, cap, "%d:%02d", _m, _ss); \
            } else { \
                int _h = (int)_s / 3600; int _m = ((int)_s % 3600) / 60; \
                snprintf(out, cap, "%dh %dm", _h, _m); \
            } \
        } while (0)

        /* Row 1: direction chip + filename (truncated) + percent right */
        Color accent = t->ansi[up ? 1 : 4];  /* red for upload, blue for download */
        f32 chip_w = 26.0f * dpi;
        f32 chip_h = ui_ch + 6.0f * dpi;
        f32 chip_x = px + 12.0f * dpi;
        f32 chip_y = py + 12.0f * dpi;
        renderer_draw_rrect_simple(r, chip_x, chip_y, chip_w, chip_h,
                           (Color){accent.r, accent.g, accent.b, 0.25f}, 5 * dpi);
        const char *arrow = up ? "up" : "dn";
        draw_text_ex(r, arrow,
                     chip_x + (chip_w - 2*ui_cw) * 0.5f,
                     chip_y + (chip_h - ui_ch) * 0.5f,
                     accent, 2, ui_cw);

        const char *fname = sftp_transfer_filename();
        i32 flen = (i32)strlen(fname);
        f32 name_x = chip_x + chip_w + 8.0f * dpi;
        i32 max_name_chars = (i32)((pw - (name_x - px) - 70.0f * dpi) / ui_cw);
        if (max_name_chars < 4) max_name_chars = 4;
        if (flen > max_name_chars) flen = max_name_chars;
        draw_text_ex(r, fname, name_x, py + 14.0f * dpi, t->fg, flen, ui_cw);

        if (total > 0) {
            char pct_buf[16];
            snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)(pct * 100 + 0.5f));
            i32 plen = (i32)strlen(pct_buf);
            draw_text_ex(r, pct_buf,
                         px + pw - (f32)plen * ui_cw - 12.0f * dpi,
                         py + 14.0f * dpi, t->fg, plen, ui_cw);
        }

        /* Row 2: speed · bytes done/total · ETA */
        char speed_str[32], size_str[64], eta_str[32];
        FMT_BPS(speed_str, sizeof(speed_str), bps);

        char done_str[32], total_str[32];
        FMT_BYTES(done_str, sizeof(done_str), done);
        if (total > 0) {
            FMT_BYTES(total_str, sizeof(total_str), total);
            snprintf(size_str, sizeof(size_str), "%s / %s", done_str, total_str);
        } else {
            snprintf(size_str, sizeof(size_str), "%s", done_str);
        }

        if (eta >= 0) FMT_TIME(eta_str, sizeof(eta_str), eta);
        else          FMT_TIME(eta_str, sizeof(eta_str), elaps);

        char meta[160];
        if (eta >= 0) {
            snprintf(meta, sizeof(meta), "%s   %s   ETA %s",
                     speed_str, size_str, eta_str);
        } else {
            /* No reliable ETA (unknown size) — show elapsed instead. */
            snprintf(meta, sizeof(meta), "%s   %s   %s elapsed",
                     speed_str, size_str, eta_str);
        }
        i32 mlen = (i32)strlen(meta);
        i32 mmax = (i32)((pw - 24.0f * dpi) / ui_cw);
        if (mlen > mmax) mlen = mmax;
        Color dim = t->sidebar_fg; dim.a *= 0.75f;
        draw_text_ex(r, meta, px + 12.0f * dpi, py + 14.0f * dpi + ui_ch + 6.0f * dpi,
                     dim, mlen, ui_cw);

        /* Row 3: progress bar (gradient approximation via two fills). */
        f32 bar_x = px + 12.0f * dpi;
        f32 bar_y = py + ph - 14.0f * dpi;
        f32 bar_w = pw - 24.0f * dpi;
        f32 bar_h = 6.0f * dpi;
        f32 bar_r = bar_h * 0.5f;   /* pill-shaped track + fill */
        renderer_draw_rrect_simple(r, bar_x, bar_y, bar_w, bar_h,
                           (Color){t->sidebar_bg.r, t->sidebar_bg.g, t->sidebar_bg.b, 0.85f}, bar_r);
        if (total > 0) {
            f32 fw = bar_w * pct;
            if (fw > 0.5f * dpi) {
                if (fw < bar_h) fw = bar_h;   /* keep the pill readable at low % */
                renderer_draw_rrect_simple(r, bar_x, bar_y, fw, bar_h, accent, bar_r);
            }
        } else {
            /* Indeterminate: a moving segment based on elapsed time. */
            f64 phase = fmod(elaps * 0.6, 1.0);
            f32 seg_w = bar_w * 0.22f;
            f32 seg_x = bar_x + (bar_w - seg_w) * (f32)phase;
            renderer_draw_rrect_simple(r, seg_x, bar_y, seg_w, bar_h, accent, bar_r);
        }

        renderer_flush_rects(r);
        renderer_flush_rrects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }

    /* Render benchmark overlay (topmost) */
    render_bench_overlay(app);

    renderer_end_frame(r);
    renderer_trim_idle_resources(r, platform_time_sec());

    /* Clear dirty state on all terminals after render */
    for (i32 i = 0; i < app->tab_count; i++) {
        if (app->tabs[i].terminal)  term_clear_dirty(app->tabs[i].terminal);
        if (app->tabs[i].terminal2) term_clear_dirty(app->tabs[i].terminal2);
        for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
            if (app->tabs[i].extra_terminals[p - 2])
                term_clear_dirty(app->tabs[i].extra_terminals[p - 2]);
        }
    }
}

/* =========================================================================
 * Session I/O polling
 * ========================================================================= */

/* Cap bytes consumed by terminal_feed per session per frame.  Heavy SSH
 * output (large ls -R, build logs) can otherwise monopolize a frame before
 * the renderer runs.  libssh2 keeps unread channel data buffered, so the
 * remainder drains on the next frame with no loss. */
#define MAX_READ_BYTES_PER_FRAME (256 * 1024)

/* Inline image previews are only supported for the handful of agents whose
 * normal (non-alt-screen) output references local image files in a way we can
 * render usefully — Claude Code, Codex, OpenCode and Grok. Every other agent
 * (and every plain shell) is excluded so the detector can't mistake a column
 * of `ls` *.png names for pictures or fight a TUI it doesn't understand. */
static bool chat_tool_inline_image_ok(ChatTool tool) {
    return tool == CHAT_TOOL_CLAUDE   || tool == CHAT_TOOL_CODEX ||
           tool == CHAT_TOOL_OPENCODE || tool == CHAT_TOOL_XAI;
}

/* Inline agent-image detection must only run while a supported AI agent owns
 * the foreground. Gate each terminal on its own session's foreground process
 * (session_fg_process has a 0.6 s TTL cache, so this is cheap per frame). */
static void term_gate_inline_images(Session *s, Terminal *t) {
    if (!t) return;
    t->inline_image_detect =
        (s && session_type(s) == SESSION_LOCAL &&
         chat_tool_inline_image_ok(chat_tool_for_process_basename(session_fg_process(s))));
}

bool app_poll_sessions(AppState *app) {
    bool got_data = false;
    u8 buf[16384];
    app->visible_session_data_this_frame = false;
    for (i32 i = 0; i < app->tab_count; i++) {
        Session *s = app->tabs[i].session;
        Terminal *t = app->tabs[i].terminal;
        /* Sleeping tab with a hibernated SSH session: drain channel reads so
         * libssh2 window keeps opening and keepalives continue flowing. */
        if (s && !t && session_is_suspended(s) && session_type(s) == SESSION_SSH) {
            for (i32 tries = 0; tries < 4; tries++) {
                if (session_read(s, buf, sizeof(buf)) <= 0) break;
            }
            session_local_forward_poll(s);
            session_remote_forward_poll(s);
            if (session_status(s) == SESSION_CONNECTED) session_x11_poll(s);
        }
        if (app->tabs[i].session2 && !app->tabs[i].terminal2 &&
            session_is_suspended(app->tabs[i].session2) &&
            session_type(app->tabs[i].session2) == SESSION_SSH) {
            for (i32 tries = 0; tries < 4; tries++) {
                if (session_read(app->tabs[i].session2, buf, sizeof(buf)) <= 0) break;
            }
            session_local_forward_poll(app->tabs[i].session2);
            session_remote_forward_poll(app->tabs[i].session2);
        }
        for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
            Session *xs = app->tabs[i].extra_sessions[p - 2];
            Terminal *xt = app->tabs[i].extra_terminals[p - 2];
            if (xs && !xt && session_is_suspended(xs) &&
                session_type(xs) == SESSION_SSH) {
                for (i32 tries = 0; tries < 4; tries++) {
                    if (session_read(xs, buf, sizeof(buf)) <= 0) break;
                }
                session_local_forward_poll(xs);
                session_remote_forward_poll(xs);
            }
        }
        if (!s || !t) continue;

        /* Refresh the inline-image gate for every live pane before any feed,
         * so detection is enabled iff that pane's foreground is an agent. */
        term_gate_inline_images(s, t);
        if (app->tabs[i].session2 && app->tabs[i].terminal2)
            term_gate_inline_images(app->tabs[i].session2, app->tabs[i].terminal2);
        for (i32 p = 2; p < MAX_SPLIT_PANES; p++)
            term_gate_inline_images(app->tabs[i].extra_sessions[p - 2],
                                    app->tabs[i].extra_terminals[p - 2]);

        /* Skip this session entirely when a background worker owns libssh2
         * for a transfer — otherwise session_read would block inside
         * libssh2_channel_read on the now-blocking session and freeze the
         * event loop. The worker clears the flag when done and data drains
         * on the following frame. */
        if (session_io_is_suspended(s)) {
            /* still let port-forward/X11 polling skip too */
        } else {

        /* Drain every frame — MAX_READ_BYTES_PER_FRAME already caps per-
         * session work, so a simple every-frame drain keeps total CPU bounded.
         * An earlier throttle (drain every 4th frame for background tabs)
         * caused a CPU-burn loop: platform_resume_watches re-arms suspended
         * dispatch sources unconditionally, so on skipped frames a level-
         * triggered READ source with buffered data fires immediately and
         * spins the main loop at max rate for any chatty background tab. */
        {
            i32 total = 0;
            while (total < MAX_READ_BYTES_PER_FRAME) {
                i32 want = (i32)sizeof(buf);
                if (MAX_READ_BYTES_PER_FRAME - total < want)
                    want = MAX_READ_BYTES_PER_FRAME - total;
                i32 n = session_read(s, buf, (usize)want);
                if (n <= 0) break;
                terminal_feed(t, buf, (usize)n);
                total += n;
                got_data = true;
                app->tabs[i].last_activity_time = platform_time_sec();
            }
            if (total > 0 && i == app->active_tab) {
                /* Both panes of an active split tab are on screen — any
                 * data drawn into either refreshes a visible region. */
                app->visible_session_data_this_frame = true;
            }

            /* Maintain the decaying output-byte count that gates the agent
             * accent animation. This drain runs every frame, so we decay each
             * frame: a brief idle cursor blink fades back below the threshold
             * while a sustained working burst keeps it above. */
            {
                f64 _now = platform_time_sec();
                f64 _dt  = _now - app->tabs[i].out_accum_t;
                if (_dt < 0.0) _dt = 0.0;
                if (_dt > 2.0) _dt = 2.0;        /* clamp after sleep/idle gaps */
                app->tabs[i].out_accum *= exp(-_dt / 0.5);   /* tau = 0.5 s */
                app->tabs[i].out_accum += (f64)total;
                app->tabs[i].out_accum_t = _now;
                /* A qualifying burst keeps the accent bar lit for the hold
                 * window past now, so bursty agent output doesn't blink it.
                 * Gate on keystroke latency: keystroke echo and per-key TUI
                 * repaints (agent input boxes) land within a few ms, while
                 * genuine agent generation has latency. Only output arriving
                 * >150 ms after the last keystroke in this tab counts as the
                 * agent "working" — typing must NOT light the bar. */
                f64 _key_age = _now - app->tabs[i].last_keystroke_time;
                if (_key_age > 0.150 &&
                    app->tabs[i].out_accum > TAB_ACCENT_WORK_THRESHOLD)
                    app->tabs[i].agent_work_until = _now + TAB_ACCENT_WORK_HOLD;
            }
        }

        /* Poll remote port forward connections for this session */
        session_local_forward_poll(s);
        session_remote_forward_poll(s);

        /* Poll X11 forwarding for SSH sessions */
        if (session_type(s) == SESSION_SSH && session_status(s) == SESSION_CONNECTED) {
            session_x11_poll(s);
        }
        }  /* end !session_io_is_suspended */

        /* Poll split pane 2 — same transfer-worker guard as the primary pane. */
        if (app->tabs[i].split != SPLIT_NONE && app->tabs[i].session2 &&
            app->tabs[i].terminal2 &&
            !session_io_is_suspended(app->tabs[i].session2)) {
            i32 total = 0;
            while (total < MAX_READ_BYTES_PER_FRAME) {
                i32 want = (i32)sizeof(buf);
                if (MAX_READ_BYTES_PER_FRAME - total < want)
                    want = MAX_READ_BYTES_PER_FRAME - total;
                i32 n = session_read(app->tabs[i].session2, buf, (usize)want);
                if (n <= 0) break;
                terminal_feed(app->tabs[i].terminal2, buf, (usize)n);
                total += n;
                got_data = true;
                app->tabs[i].last_activity_time = platform_time_sec();
            }
            if (total > 0 && i == app->active_tab) {
                app->visible_session_data_this_frame = true;
            }
            session_local_forward_poll(app->tabs[i].session2);
            session_remote_forward_poll(app->tabs[i].session2);
        }

        if (app->tabs[i].split != SPLIT_NONE) {
            i32 pane_count = tab_split_pane_count(&app->tabs[i]);
            for (i32 p = 2; p < pane_count; p++) {
                Session *ps = tab_pane_session(&app->tabs[i], p);
                Terminal *pt = tab_pane_terminal(&app->tabs[i], p);
                if (!ps || !pt || session_io_is_suspended(ps)) continue;
                i32 total = 0;
                while (total < MAX_READ_BYTES_PER_FRAME) {
                    i32 want = (i32)sizeof(buf);
                    if (MAX_READ_BYTES_PER_FRAME - total < want)
                        want = MAX_READ_BYTES_PER_FRAME - total;
                    i32 n = session_read(ps, buf, (usize)want);
                    if (n <= 0) break;
                    terminal_feed(pt, buf, (usize)n);
                    total += n;
                    got_data = true;
                    app->tabs[i].last_activity_time = platform_time_sec();
                }
                if (total > 0 && i == app->active_tab) {
                    app->visible_session_data_this_frame = true;
                }
                session_local_forward_poll(ps);
                session_remote_forward_poll(ps);
            }
        }

        /* Update tab title from terminal state. A known coding-agent CLI in
         * the foreground takes precedence: name the tab after the tool so it
         * matches the leading agent icon (e.g. "OpenCode", "Claude Code").
         * A user-set custom_title still wins downstream in tab_effective_title. */
        ChatTool fg_tool = (session_type(s) == SESSION_LOCAL)
                         ? tab_agent_tool(&app->tabs[i]) : CHAT_TOOL_UNKNOWN;
        if (fg_tool != CHAT_TOOL_UNKNOWN) {
            snprintf(app->tabs[i].title, sizeof(app->tabs[i].title),
                     "%s", chat_tool_display(fg_tool));
            t->title_changed = false;  /* don't let an OSC title override it */
        } else {
            /* When the agent just exited, force a re-derive so the tab stops
             * showing the agent name even if no new OSC title arrived. */
            if (app->tabs[i].last_fg_tool != CHAT_TOOL_UNKNOWN)
                t->title_changed = true;
            if (t->title_changed) {
                t->title_changed = false;
                if (t->title[0]) {
                    /* Shell set a title via OSC 0/2 */
                    snprintf(app->tabs[i].title, sizeof(app->tabs[i].title), "%s", t->title);
                } else if (t->cwd && t->cwd[0]) {
                    /* Use CWD from OSC 7 */
                    const char *dir = t->cwd;
                    /* Shorten home dir to ~ */
                    const char *home = getenv("HOME");
                    if (home && strncmp(dir, home, strlen(home)) == 0) {
                        const char *rest = dir + strlen(home);
                        if (*rest == '/' || *rest == '\0') {
                            snprintf(app->tabs[i].title, sizeof(app->tabs[i].title),
                                     "~%s", rest);
                        } else {
                            snprintf(app->tabs[i].title, sizeof(app->tabs[i].title), "%s", dir);
                        }
                    } else {
                        snprintf(app->tabs[i].title, sizeof(app->tabs[i].title), "%s", dir);
                    }
                }
            }
        }
        app->tabs[i].last_fg_tool = fg_tool;

        /* Fallback: get foreground process name for title */
        if (session_type(s) == SESSION_LOCAL && app->tabs[i].title[0] == '\0') {
            const char *proc = session_fg_process(s);
            if (proc && proc[0]) {
                snprintf(app->tabs[i].title, sizeof(app->tabs[i].title), "%s", proc);
            }
        }

        /* SESSION_CONNECTED edge detection for SSH — jump the filebrowser
         * to the remote home the first time a session comes up (and again
         * after a reconnect). Covers the case where the remote shell never
         * emits OSC 7, which would otherwise leave the sidebar pinned to
         * the local cwd it had before the connection attempt. */
        SessionStatus cur_status = session_status(s);
        /* Read-ready fd watch for ANY session type — local PTYs benefit
         * as much as SSH sockets because users routinely run `ssh host`
         * inside a local terminal. In raw mode every keystroke's echo
         * travels through the PTY master, and polling at 8 ms adds
         * exactly the same perceived typing lag.
         *
         * Only the fd owner registers a watch: ControlMaster-shared non-owner
         * sessions alias the owner's fd, and platform_watch_socket is already
         * a no-op when the fd is watched — explicit here to make intent clear.
         * The unwatch side is the responsibility of session_destroy /
         * session_reconnect (they close the fd and unwatch right before),
         * which is the only point where the fd lifetime invariant required
         * by libdispatch (fd must remain valid until cancel completes) holds. */
        if (cur_status == SESSION_CONNECTED &&
            app->tabs[i].prev_status != SESSION_CONNECTED &&
            session_is_shared_owner(s)) {
            int fd = session_io_fd(s);
            if (fd >= 0) platform_watch_socket(fd);
        }
        if (session_type(s) == SESSION_SSH) {
            /* CONNECTED → !CONNECTED (drop): the SFTP handle attached to
             * this session is about to be or already has been freed by
             * session_reconnect / shutdown. Detach the filebrowser so a
             * subsequent render_sidebar / click doesn't dereference a
             * dangling pointer. We only detach if the fb is actually
             * pointing at THIS session's handle. */
            if (app->tabs[i].prev_status == SESSION_CONNECTED &&
                cur_status != SESSION_CONNECTED) {
                if (app->filebrowser.source == FB_SOURCE_SFTP &&
                    app->filebrowser.session == (void *)s) {
                    fb_detach_sftp(&app->filebrowser);
                }
            }
            /* !CONNECTED → CONNECTED (up): force auto-wire to re-fire on
             * the next pass. Handles both initial connect and reconnect. */
            if (cur_status == SESSION_CONNECTED &&
                app->tabs[i].prev_status != SESSION_CONNECTED) {
                app->tabs[i].sftp_auto_wired = false;
            }
            if (cur_status == SESSION_CONNECTED && !app->tabs[i].sftp_auto_wired) {
                const char *home = session_initial_cwd(s);
                void *sftp = session_sftp_handle(s);
                if (sftp && home[0] && i == app->active_tab &&
                    app->sidebar_visible && app->filebrowser.open) {
                    app->filebrowser.session = (void *)s;
                    fb_navigate_sftp(&app->filebrowser, sftp, home);
                }
                /* Mark wired either way: the sidebar-open path (render_sidebar)
                 * reads session_initial_cwd directly when the user toggles the
                 * sidebar later — we don't need to re-navigate from here. */
                if (sftp) app->tabs[i].sftp_auto_wired = true;
            }
        }
        app->tabs[i].prev_status = cur_status;

        /* Mirror the read-ready watch registration for the right split pane.
         * Without this, keystroke echo on session2 falls back to the poll
         * cadence and feels noticeably laggier than the left pane. */
        if (app->tabs[i].session2) {
            SessionStatus cur2 = session_status(app->tabs[i].session2);
            if (cur2 == SESSION_CONNECTED &&
                app->tabs[i].prev_status2 != SESSION_CONNECTED &&
                session_is_shared_owner(app->tabs[i].session2)) {
                int fd2 = session_io_fd(app->tabs[i].session2);
                if (fd2 >= 0) platform_watch_socket(fd2);
            }
            app->tabs[i].prev_status2 = cur2;
        } else {
            app->tabs[i].prev_status2 = SESSION_DISCONNECTED;
        }

        /* Auto-update sidebar CWD when active tab changes directory */
        if (i == app->active_tab && app->sidebar_visible && app->filebrowser.open) {
            if (t->cwd && t->cwd[0] && strcmp(t->cwd, app->filebrowser.cwd) != 0) {
                if (session_type(s) == SESSION_SSH) {
                    void *sftp = session_sftp_handle(s);
                    if (sftp) {
                        app->filebrowser.session = (void *)s;
                        fb_navigate_sftp(&app->filebrowser, sftp, t->cwd);
                    }
                } else {
                    app->filebrowser.session = NULL;
                    fb_navigate(&app->filebrowser, t->cwd);
                }
            }
        }

        if (!session_is_alive(s) && session_status(s) == SESSION_CONNECTED) {
            if (session_type(s) == SESSION_SSH) {
                /* Auto-reconnect for SSH */
                if (session_reconnect(s)) {
                    /* Reconnected successfully */
                } else if (!strstr(app->tabs[i].title, "[closed]")) {
                    strncat(app->tabs[i].title, " [closed]",
                            sizeof(app->tabs[i].title) - strlen(app->tabs[i].title) - 1);
                }
            } else if (!strstr(app->tabs[i].title, "[closed]")) {
                strncat(app->tabs[i].title, " [closed]",
                        sizeof(app->tabs[i].title) - strlen(app->tabs[i].title) - 1);
            }
        }
    }
    return got_data;
}

/* =========================================================================
 * Smart Vault — UI helpers
 *
 * The unlock overlay is a clone of the passphrase dialog's styling so the
 * UX feels familiar. We intentionally do NOT wire in IME: master passwords
 * are plain ASCII.
 * ========================================================================= */

void app_vault_open_unlock(AppState *app, i32 pending_action) {
    if (!app) return;
    /* If the vault is already unlocked, dispatching a pending action is
     * the caller's job — here we only handle the "need master password"
     * path. */
    app->vault_unlock_is_init = app->vault
        ? !vault_is_initialized(app->vault) : false;
    app->vault_unlock_dialog_active = true;
    app->vault_unlock_pending_action = pending_action;
    crypto_secure_zero(app->vault_unlock_input, sizeof app->vault_unlock_input);
    app->vault_unlock_error[0] = '\0';
    if (app->vault) vault_touch_activity(app->vault);
}

void app_vault_cancel_unlock(AppState *app) {
    if (!app) return;
    crypto_secure_zero(app->vault_unlock_input, sizeof app->vault_unlock_input);
    app->vault_unlock_dialog_active = false;
    app->vault_unlock_pending_action = 0;
    app->vault_unlock_error[0] = '\0';
    app->vault_unlock_is_init = false;
}

bool app_vault_submit_unlock(AppState *app) {
    if (!app || !app->vault) return false;

    /* Rate-limit: after a few wrong attempts we add exponential backoff.
     * We don't hard-lock the app — just refuse submits until the timer
     * expires. Good against shoulder-typing, not a password oracle. */
    f64 now = platform_time_sec();
    if (app->vault_unlock_retry_after_ts > now) {
        i32 remaining = (i32)(app->vault_unlock_retry_after_ts - now + 0.99);
        snprintf(app->vault_unlock_error, sizeof app->vault_unlock_error,
                 "Wait %ds before retrying", remaining);
        return false;
    }

    bool ok;
    if (app->vault_unlock_is_init) {
        if (app->vault_unlock_input[0] == '\0') {
            snprintf(app->vault_unlock_error, sizeof app->vault_unlock_error,
                     "Master password required");
            return false;
        }
        ok = vault_init_master(app->vault, app->vault_unlock_input);
        if (ok) ok = vault_unlock(app->vault, app->vault_unlock_input);
        if (!ok) snprintf(app->vault_unlock_error, sizeof app->vault_unlock_error,
                          "Init failed");
    } else {
        ok = vault_unlock(app->vault, app->vault_unlock_input);
        if (!ok) {
            app->vault_unlock_attempts++;
            /* 1s, 2s, 4s, 8s, 16s caps at 30s. */
            i32 delay = 1 << (app->vault_unlock_attempts - 1);
            if (delay > 30) delay = 30;
            app->vault_unlock_retry_after_ts = now + (f64)delay;
            snprintf(app->vault_unlock_error, sizeof app->vault_unlock_error,
                     "Wrong password — wait %ds", delay);
        }
    }

    /* Always scrub the typed buffer (even on success — we're done with it). */
    crypto_secure_zero(app->vault_unlock_input, sizeof app->vault_unlock_input);

    if (ok) {
        app->vault_unlock_dialog_active = false;
        app->vault_unlock_attempts = 0;
        app->vault_unlock_retry_after_ts = 0;
        app->vault_unlock_error[0] = '\0';
        (void)now;
        vault_touch_activity(app->vault);
        /* Run the deferred action, if any. Browser open is the only
         * case today. */
        if (app->vault_unlock_pending_action == ACT_VAULT_BROWSER) {
            app_vault_browser_open(app);
        }
        app->vault_unlock_pending_action = 0;
        app_show_toast(app, "Vault unlocked");
    }
    return ok;
}

void app_vault_lock_now(AppState *app) {
    if (!app || !app->vault) return;
    vault_lock(app->vault);
    app->vault_browser_active = false;
    app->vault_browser_editing = false;
    crypto_secure_zero(app->vault_browser_edit_value,
                       sizeof app->vault_browser_edit_value);
    app_show_toast(app, "Vault locked");
}

void app_vault_auto_lock_tick(AppState *app) {
    if (!app || !app->vault) return;
    if (!vault_is_unlocked(app->vault)) return;
    /* config.vault_auto_lock_minutes == 0 means "never" */
    i32 mins = app->config.vault_auto_lock_minutes;
    if (mins <= 0) return;
    f64 last = vault_get_last_activity(app->vault);
    f64 now = platform_time_sec();
    if (last <= 0) return;
    if (now - last < (f64)mins * 60.0) return;
    app_vault_lock_now(app);
}

void app_vault_render_unlock(AppState *app) {
    if (!app || !app->vault_unlock_dialog_active) return;
    Renderer *r = &app->renderer;
    f32 dpi = app->dpi_scale;
    f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
    renderer_set_ui_scale(r, ui_cw, ui_ch);

    const Theme *t = app->config.theme;
    if (!t) t = &THEME_DARK;

    f32 dw = 460 * dpi, dh = 212 * dpi;
    f32 dx = ((f32)app->fb_width - dw) / 2;
    f32 dy = ((f32)app->fb_height - dh) / 2;
    f32 pad = 18 * dpi;
    f32 radius = 12 * dpi;
    Color accent = theme_ui_accent(t);

    /* Dim backdrop. */
    renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                       (Color){0, 0, 0, 0.6f});
    renderer_flush_rects(r);

    /* Rounded, soft-shadowed panel + accent header bar (shared chrome). */
    Color panel_bg = { t->tab_inactive_bg.r, t->tab_inactive_bg.g,
                       t->tab_inactive_bg.b, 0.99f };
    Color panel_bd = t->border; panel_bd.a = fmaxf(panel_bd.a, 0.70f);
    draw_panel_chrome(r, dx, dy, dw, dh, panel_bg, panel_bd, radius, dpi,
                      24.0f * dpi, 0.40f);
    renderer_draw_rrect_simple(r, dx + pad, dy + pad + 1.0f * dpi,
                               3.0f * dpi, ui_ch - 2.0f * dpi, accent, 1.5f * dpi);
    renderer_flush_rrects(r);

    const char *title = app->vault_unlock_is_init
        ? "Create Vault Master Password"
        : "Unlock Vault";
    draw_text_ex(r, title, dx + pad + 11.0f * dpi, dy + pad, t->fg,
                 (i32)strlen(title), ui_cw);

    /* Sub-label */
    f32 label_y = dy + pad + ui_ch + 12.0f * dpi;
    const char *sub = app->vault_unlock_is_init
        ? "Pick a strong passphrase — it cannot be recovered."
        : "Enter your master password.";
    Color sub_fg = t->sidebar_fg; sub_fg.a *= 0.80f;
    draw_text_ex(r, sub, dx + pad, label_y, sub_fg, 80, ui_cw);

    /* Recessed rounded input field with focused accent border. */
    f32 field_y = label_y + ui_ch + 12.0f * dpi;
    f32 field_h = 34 * dpi;
    Color inp_bg = t->sidebar_bg; inp_bg.a = 1.0f;
    Color inp_bd = accent; inp_bd.a = fmaxf(inp_bd.a, 0.90f);
    draw_input_field(r, dx + pad, field_y, dw - 2.0f * pad, field_h, inp_bg, inp_bd, dpi);

    /* Masked input + blinking accent caret. */
    i32 pw_len = (i32)strlen(app->vault_unlock_input);
    {
        char masked[128];
        i32 show_len = pw_len < (i32)sizeof masked - 1 ? pw_len : (i32)sizeof masked - 1;
        for (i32 i = 0; i < show_len; i++) masked[i] = '*';
        masked[show_len] = '\0';
        draw_text_ex(r, masked, dx + pad + 12.0f * dpi,
                     field_y + (field_h - ui_ch) / 2, t->fg, 45, ui_cw);
    }
    if (fmod(platform_time_sec(), 1.06) < 0.53) {
        f32 cx = dx + pad + 12.0f * dpi + (f32)pw_len * ui_cw;
        f32 cx_max = dx + pad + (dw - 2.0f * pad) - 6.0f * dpi;
        if (cx > cx_max) cx = cx_max;
        renderer_draw_rect(r, cx, field_y + 7.0f * dpi,
                           2.0f * dpi, field_h - 14.0f * dpi, accent);
        renderer_flush_rects(r);
    }

    /* Error / retry message */
    if (app->vault_unlock_error[0]) {
        f32 err_y = field_y + field_h + 8.0f * dpi;
        draw_text_ex(r, app->vault_unlock_error, dx + pad, err_y,
                     t->ansi[1], 80, ui_cw);
    }

    /* Buttons — rounded pills; primary filled with the UI accent. */
    f32 btn_y = dy + dh - 42.0f * dpi;
    f32 btn_w = 88 * dpi, btn_h = 30 * dpi;
    f32 ok_x = dx + dw - pad - btn_w;
    f32 cancel_x = ok_x - btn_w - 10.0f * dpi;
    Color cancel_bg = t->tab_bg;
    Color ok_bg = accent; ok_bg.a = fmaxf(ok_bg.a, 1.0f);
    renderer_draw_rrect_simple(r, cancel_x, btn_y, btn_w, btn_h, cancel_bg, 8.0f * dpi);
    renderer_draw_rrect_simple(r, ok_x, btn_y, btn_w, btn_h, ok_bg, 8.0f * dpi);
    /* Esc / Enter keycap hints on the left. */
    f32 hint_y = btn_y + (btn_h - ui_ch) / 2;
    f32 kx = dx + pad;
    kx = draw_keycap(r, kx, hint_y, "Esc", t, ui_cw, ui_ch, dpi) + 8.0f * dpi;
    draw_keycap(r, kx, hint_y, "Enter", t, ui_cw, ui_ch, dpi);
    renderer_flush_rrects(r);

    draw_text_ex(r, "Cancel", cancel_x + (btn_w - 6 * ui_cw) / 2,
                 btn_y + (btn_h - ui_ch) / 2, t->sidebar_fg, 6, ui_cw);
    const char *ok_label = app->vault_unlock_is_init ? "Create" : "Unlock";
    draw_text_ex(r, ok_label, ok_x + (btn_w - strlen(ok_label) * ui_cw) / 2,
                 btn_y + (btn_h - ui_ch) / 2, chrome_legible_on(ok_bg), 10, ui_cw);

    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

/* =========================================================================
 * Smart Vault — SSH integration helpers
 *
 * Reveals the three secret types that can be linked to an SSH session
 * (password, passphrase, private key) and populates the plaintext
 * buffers on SSHConfig. The worker thread never reaches into the vault
 * — it just reads the now-populated char[] fields.
 *
 * Private key secrets are decrypted into cfg->key_pem (heap buffer).
 * ssh_session code is responsible for freeing + secure_zero'ing the
 * buffer when the session is destroyed (M7 wires that up).
 * ========================================================================= */

bool app_vault_populate_ssh_secrets(AppState *app, SSHConfig *cfg,
                                    bool *need_unlock_out) {
    if (need_unlock_out) *need_unlock_out = false;
    if (!app || !cfg) return false;

    /* Nothing to do if no secret ids set. */
    bool need_pw  = cfg->password_secret_id[0]     != '\0';
    bool need_pp  = cfg->passphrase_secret_id[0]   != '\0';
    bool need_key = cfg->private_key_secret_id[0]  != '\0';
    if (!need_pw && !need_pp && !need_key) return true;

    if (!app->vault || !vault_is_unlocked(app->vault)) {
        if (need_unlock_out) *need_unlock_out = true;
        return false;
    }

    usize n = 0;
    if (need_pw) {
        u8 *pt = vault_secret_reveal(app->vault, cfg->password_secret_id, &n);
        if (!pt) return false;
        if (n >= sizeof cfg->password) { vault_secret_release(pt, n); return false; }
        memcpy(cfg->password, pt, n);
        cfg->password[n] = '\0';
        vault_secret_release(pt, n);
        cfg->auth_method = AUTH_PASSWORD;
    }
    if (need_pp) {
        u8 *pt = vault_secret_reveal(app->vault, cfg->passphrase_secret_id, &n);
        if (!pt) return false;
        if (n >= sizeof cfg->key_passphrase) { vault_secret_release(pt, n); return false; }
        memcpy(cfg->key_passphrase, pt, n);
        cfg->key_passphrase[n] = '\0';
        vault_secret_release(pt, n);
    }
    if (need_key) {
        u8 *pt = vault_secret_reveal(app->vault, cfg->private_key_secret_id, &n);
        if (!pt) return false;
        /* Hand over ownership — session_destroy will crypto_secure_zero +
         * free it. */
        if (cfg->key_pem) {
            crypto_secure_zero(cfg->key_pem, cfg->key_pem_len);
            free(cfg->key_pem);
        }
        cfg->key_pem = pt;
        cfg->key_pem_len = n;
        cfg->auth_method = AUTH_PUBLICKEY;
    }
    vault_touch_activity(app->vault);
    return true;
}

/* Smart Vault browser stubs — real implementation lives in vault_ui.c (M5).
 * These are __attribute__((weak)) so vault_ui.c overrides without linker
 * conflict when it ships. */
__attribute__((weak)) void app_vault_browser_open(AppState *app) {
    if (app) app_show_toast(app, "Vault browser coming in next build");
}
__attribute__((weak)) void app_vault_browser_close(AppState *app) { (void)app; }
__attribute__((weak)) void app_vault_browser_render(AppState *app) { (void)app; }
__attribute__((weak)) bool app_vault_browser_handle_key(AppState *app, u32 k, u32 m) {
    (void)app; (void)k; (void)m; return false;
}
__attribute__((weak)) bool app_vault_browser_handle_char(AppState *app, u32 c) {
    (void)app; (void)c; return false;
}
