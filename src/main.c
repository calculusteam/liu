/*
 * Liu - main entry point
 * Creates window, starts local shell, runs event loop.
 */
#include "platform/platform.h"
#include "renderer/renderer.h"
#include "terminal/terminal.h"
#include "terminal/url.h"
#include "ssh/ssh_session.h"
#include "ssh/sftp.h"          /* sftp_transfer_begin/tick/end — progress panel */
#include "ssh/mosh.h"
#include "ssh/ssh_config.h"
#include "ssh/keygen.h"
#include "ssh/known_hosts.h"
#include "ui/ui.h"
#include "ui/anim.h"
#include "ui/palette.h"
#include "ui/sites_ui.h"
#include "ui/markdown/md_render.h"   /* MD_LINKRECT_* kinds */
#include "ui/markdown/md_graph.h"    /* backlinks via the note link graph */
#include "history/parser.h"
#include "history/session.h"
#include "history/event.h"
#include "theming_guide_data.h"   /* build-time embedded THEMING.md */
#include "ui/hittest.h"
#include "ui/layout.h"
#include "core/utf8.h"
#include "core/theme_import.h"
#include "core/agent_detect.h"
#include "core/cmd_history.h"
#include "core/cmd_suggest.h"
#include "core/crashlog.h"
#include "notify/notify_server.h"
#include "notify/claude_hooks.h"
#include "notify/agent_hooks.h"
#include "notify/socket.h"
#include "translate/translate.h"
#include "translate/translate_agent.h"
#include "translate/translate_api.h"
#include "translate/translate_local.h"
#include "translate/translate_segment.h"
#include "translate/model_catalog.h"
#include "translate/model_download.h"
#include "translate/model_paths.h"
#include "vault/vault.h"
#include "vault/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <fcntl.h>
#include <errno.h>
#ifndef PLATFORM_WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif
#include <stdarg.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern i32 import_ssh_config(Vault *v, const char *config_path);

#include "ui/markdown/md_parse.h"
#include "cJSON.h"

extern void sidebar_load_hosts(Vault *v);

/* Forward declarations for native settings callbacks */
static AppState *g_app_ptr = NULL;
static PlatformWindow *g_window_ptr = NULL;

/* Handle a DPI / backing-scale change: rebuild the font atlas at the new
 * pixel density, rescale sidebar width, recalculate layout, and invalidate
 * all terminal render caches. Shared by the EVENT_DPI_CHANGE handler and
 * the live-resize render callback (which must handle cross-display DPI
 * changes that arrive mid-drag while the event loop is starved by
 * NSEventTrackingRunLoopMode). */
static void app_handle_dpi_change(AppState *app, PlatformWindow *window, f32 new_dpi) {
    if (!app || !window || new_dpi <= 0 || new_dpi == app->dpi_scale) return;
    app->dpi_scale = new_dpi;

    /* Clear the renderer's cached atlas pointer BEFORE destroying the old
     * atlas so any render-flush reached during rebuild sees NULL and
     * returns early. The dangling MTLTexture pointer otherwise causes
     * objc_retain to segfault on the next setFragmentTexture: call. */
#ifdef USE_METAL
    renderer_metal_set_atlas(&app->renderer, NULL);
#endif
    font_atlas_destroy(&app->renderer.font);
    font_atlas_create(&app->renderer.font, app->config.font_path,
                      app->config.font_size, app->dpi_scale,
                      app->config.font_weight);
#ifdef USE_METAL
    font_atlas_create_metal_texture(&app->renderer.font, platform_get_gpu_device(window));
    renderer_metal_set_atlas(&app->renderer, app->renderer.font.metal_texture);
#endif
    app->renderer.font.cell_width  *= app->config.cell_width_scale;
    app->renderer.font.cell_height *= app->config.cell_height_scale;

    /* Rescale sidebar width BEFORE layout so the grid cols account for
     * the correct sidebar pixel width at the new DPI. Doing this after
     * app_update_layout left the grid computed with the stale old-DPI
     * sidebar width, hiding columns under or spilling past the sidebar. */
    if (app->sidebar_visible)
        app->sidebar_width = CLAMP(app->config.sidebar_width,
                                   SIDEBAR_MIN_PT, SIDEBAR_MAX_PT) * app->dpi_scale;

    /* Recalculate layout at new DPI */
    i32 w, h, fw, fh;
    platform_window_get_size(window, &w, &h);
    platform_window_get_framebuffer_size(window, &fw, &fh);
    app_update_layout(app, w, h, fw, fh);

    /* Invalidate all terminal render caches */
    for (i32 ci = 0; ci < app->tab_count; ci++) {
        app->tabs[ci].cache1.all_rows_dirty = true;
        app->tabs[ci].cache2.all_rows_dirty = true;
    }
}

/* Live-resize render bridge.
 *
 * macOS pumps the runloop in NSEventTrackingRunLoopMode while the user is
 * dragging the window edge — our normal main loop is starved during the
 * drag, and the layer's drawable grows ahead of any rendered content,
 * leaving the new pixels black. The platform layer fires this callback at
 * ~60 Hz from a tracking-mode NSTimer; we re-sync layout to the current
 * window size and render synchronously. */
static void on_live_resize_render(void *user) {
    AppState *app = (AppState *)user;
    if (!app || !g_window_ptr) return;

    /* Check for DPI change (e.g. window dragged to a different-density
     * display mid-resize). The main event loop is starved during live
     * resize, so EVENT_DPI_CHANGE would be queued but never processed
     * until the drag ends — without this check the font atlas stays at
     * the old DPI while the layer's drawableSize and the projection move
     * to the new pixel density, making glyphs render at the wrong size. */
    f32 current_dpi = platform_window_get_dpi_scale(g_window_ptr);
    if (current_dpi > 0 && current_dpi != app->dpi_scale) {
        app_handle_dpi_change(app, g_window_ptr, current_dpi);
    }

    i32 w, h, fb_w, fb_h;
    platform_window_get_size(g_window_ptr, &w, &h);
    platform_window_get_framebuffer_size(g_window_ptr, &fb_w, &fb_h);
    if (w != app->win_width || h != app->win_height ||
        fb_w != app->fb_width || fb_h != app->fb_height) {
        app_update_layout(app, w, h, fb_w, fb_h);
    }
    platform_make_current(g_window_ptr);
    app_render(app);
    platform_window_swap_buffers(g_window_ptr);
}

/* Title-bar double-click zoom guard: zoom only when the double-click lands on
 * an empty, non-interactive part of the toolbar — never on a tab, its close
 * (×), the new-tab (+), a group chip, or a toolbar button. Stops an accidental
 * window zoom when the user quickly double-clicks a tab's × to close it. */
static bool on_titlebar_zoom_query(void *user, f32 fb_x, f32 fb_y) {
    AppState *app = (AppState *)user;
    if (!app) return true;
    HitResult h = ui_hit_test(app, fb_x, fb_y);
    return h.type == HIT_NONE;
}

/* UTF-8-aware text-input helpers shared by the inline rename / prompt fields
 * (tab rename, tab-group chip rename, file-browser name prompt). The old code
 * appended `(char)codepoint` gated on `cp < 127`, which silently DROPPED every
 * non-ASCII character (e.g. Turkish ç ş ı ğ ö ü) and, because backspace deleted
 * a single byte, CORRUPTED any multi-byte character already in the buffer. */
static void rename_buf_append_cp(char *buf, i32 *len, i32 cap, u32 cp) {
    if (cp < 32 || cp == 127) return;            /* skip control chars + DEL */
    u8 enc[4];
    u32 n = utf8_encode(cp, enc);
    if (n == 0 || *len + (i32)n >= cap) return;   /* invalid cp or no room (+NUL) */
    for (u32 k = 0; k < n; k++) buf[(*len)++] = (char)enc[k];
    buf[*len] = '\0';
}
static void rename_buf_backspace(char *buf, i32 *len) {
    i32 l = *len;
    if (l <= 0) return;
    l--;                                          /* drop one byte … */
    while (l > 0 && ((u8)buf[l] & 0xC0) == 0x80) l--;  /* …then any UTF-8 trail bytes */
    buf[l] = '\0';
    *len = l;
}
static void app_reset_ssh_dialog(AppState *app);
static void app_reset_port_forward_dialog(AppState *app);
static void app_ssh_dialog_submit(AppState *app);
static void app_switch_theme(AppState *app, const Theme *t);
static void app_open_create_theme(AppState *app);
static void app_open_create_doc(AppState *app);
static void app_create_theme_submit(AppState *app);
static void app_close_create_theme(AppState *app);
static void app_create_theme_insert_char(AppState *app, u32 cp);

static bool app_scroll_file_viewer(FileBrowser *fb, f32 dy, bool precise, f32 dpi) {
    if (!fb || fb->view_mode == FVIEW_NONE) return false;

    f32 line_h = 16.0f * (dpi > 0.0f ? dpi : 1.0f);
    f32 dy_px = precise ? dy : dy * line_h;

    /* Graph view: scroll = zoom about the viewport centre (pan scales with
     * zoom so the centred world point stays put).
     *
     * Sensitivity is split by input device so neither overshoots: a trackpad
     * sends pixel-precise dy (many small events per gesture), a wheel sends a
     * few discrete lines. The old path fed wheel lines through dy*line_h (~32
     * px each) into the same factor, so one wheel click jumped ~14% and a fast
     * flick ~50%. Now a wheel line is a controlled ~5.5% step and a trackpad
     * gesture eases in gently. */
    if (fb->view_mode == FVIEW_GRAPH) {
        f32 zoom_in = precise ? dy * 0.0020f    /* per trackpad pixel */
                              : dy * 0.055f;     /* per wheel line     */
        f32 factor = expf(zoom_in);
        f32 nz = fb->graph_zoom * factor;
        if (nz < 0.05f) nz = 0.05f;
        if (nz > 12.0f)  nz = 12.0f;
        f32 ratio = (fb->graph_zoom > 1e-6f) ? nz / fb->graph_zoom : 1.0f;
        fb->graph_pan_x *= ratio;
        fb->graph_pan_y *= ratio;
        fb->graph_zoom = nz;
        /* User took manual control — stop the entrance auto-fit camera glide,
         * which otherwise eases zoom/pan back to the fit every frame and undoes
         * this zoom (the bug where zoom "does nothing" until you first click). */
        fb->graph_user_zoomed = true;
        fb->graph_next_frame_at = platform_time_sec();
        return true;
    }

    bool md_render_mode =
        (fb->view_mode == FVIEW_MARKDOWN &&
         fb->md_doc &&
         !fb->md_raw_mode);

    if (md_render_mode) {
        fb->view_scroll_px -= dy_px;
        if (fb->view_scroll_px < 0.0f)
            fb->view_scroll_px = 0.0f;

        f32 max_y = fb->view_content_px - line_h * 4.0f;
        if (max_y < 0.0f) max_y = 0.0f;
        if (fb->view_scroll_px > max_y)
            fb->view_scroll_px = max_y;
    } else {
        i32 step = (i32)(dy_px / line_h);
        if (step == 0 && dy_px != 0.0f) step = dy_px > 0 ? 1 : -1;
        fb->view_scroll -= step;
        if (fb->view_scroll < 0)
            fb->view_scroll = 0;
        i32 max_scroll = fb->view_line_count > 10 ?
                         fb->view_line_count - 10 : 0;
        if (fb->view_scroll > max_scroll)
            fb->view_scroll = max_scroll;
    }

    return true;
}

/* Route a clicked markdown link/tag by its MdLinkRect kind. Wikilinks/embeds
 * and relative .md links open in-app (resolved against the doc's folder);
 * everything else (http, mailto, images, non-md files) goes to the OS. Tag
 * clicks are handled in a later phase (tag index/filter). */
static void app_open_md_link(FileBrowser *fb, const char *url, u8 kind) {
    if (!fb || !url || !url[0]) return;
    if (kind == MD_LINKRECT_TAG) {
        /* Open the knowledge graph for this folder — tags are pseudo-nodes,
         * so the clicked tag and the notes carrying it become visible. */
        if (!fb_graph_active(fb)) fb_toggle_graph(fb);
        return;
    }

    const char *base = (fb->md_doc) ? fb->md_doc->base_dir : NULL;
    char resolved[FB_MAX_PATH * 2];
    bool has_scheme = (strstr(url, "://") != NULL);
    usize ul = strlen(url);
    bool ends_md = (ul > 3 && strcasecmp(url + ul - 3, ".md") == 0) ||
                   (ul > 9 && strcasecmp(url + ul - 9, ".markdown") == 0);

    if (kind == MD_LINKRECT_WIKILINK || kind == MD_LINKRECT_EMBED) {
        /* Resolve a note name → .md relative to the current doc's folder
         * (same-folder is the common case; cross-folder needs a vault index). */
        if (url[0] == '/')
            snprintf(resolved, sizeof resolved, "%s%s", url, ends_md ? "" : ".md");
        else if (base)
            snprintf(resolved, sizeof resolved, "%s/%s%s", base, url, ends_md ? "" : ".md");
        else
            snprintf(resolved, sizeof resolved, "%s%s", url, ends_md ? "" : ".md");
        fb_open_md_path(fb, resolved);
        return;
    }

    /* URL / IMAGE: resolve a bare relative path against base_dir. */
    const char *target = url;
    if (!has_scheme && url[0] != '/' && base) {
        snprintf(resolved, sizeof resolved, "%s/%s", base, url);
        target = resolved;
    }
    /* A relative/absolute .md link opens in the in-app viewer. */
    if (kind == MD_LINKRECT_URL && !has_scheme && ends_md) {
        if (fb_open_md_path(fb, target)) return;
    }
    platform_open_url(target);
}

/* If a code-block Copy button sits under (mx,my), copy that block's text to the
 * clipboard and report the click as consumed. */
static bool app_md_try_copy(const FileBrowser *fb, f32 mx, f32 my) {
    const u8 *txt = NULL; u32 len = 0;
    if (!fb_md_hit_copy(fb, mx, my, &txt, &len) || !txt) return false;
    char *buf = (char *)malloc((usize)len + 1);
    if (buf) {
        memcpy(buf, txt, len);
        buf[len] = '\0';
        platform_clipboard_set(buf);
        free(buf);
    }
    return true;   /* consumed the click regardless of alloc outcome */
}

/* Identifies which UI surface currently owns scroll input. Used to discard
 * stale momentum-phase scroll events whose gesture began over a different
 * surface: a trackpad fling on the terminal keeps emitting inertial scroll
 * events for ~1 s after the fingers lift, and if a modal (e.g. the Cmd+K
 * palette) opens during that tail, those events would scroll the freshly
 * opened modal on their own. Any non-zero id is a modal/overlay; 0 is the
 * base terminal / file-browser surface. */
static i32 app_scroll_context_id(const AppState *app) {
    if (app->palette_active)             return 1;
    if (app->transcript_viewer_active)   return 2;
    if (app->settings.open)              return 3;
    if (app->sites.active)               return 4;
    if (app->ssh_dialog_active)          return 5;
    if (app->create_theme_active)        return 6;
    if (app->search_active)              return 7;
    if (app->broadcast_overlay_active)   return 8;
    if (app->port_forward_dialog_active) return 9;
    if (app->kbi_dialog_active)          return 10;
    if (app->key_manager_active)         return 11;
    if (app->known_hosts_open)           return 12;
    if (app->hostkey_dialog_active)      return 13;
    if (app->passphrase_dialog_active)   return 14;
    if (app->agent_picker_active)        return 15;
    if (app->cmd_history_active)         return 16;
    return 0;
}

static i32 app_open_markdown_viewer_tab(AppState *app, const char *path) {
    if (!app || !path || !path[0]) return -1;

    const char *slash = strrchr(path, '/');
    char dir[FB_MAX_PATH];
    char base[512];
    if (slash) {
        i32 dlen = (i32)(slash - path);
        if (dlen <= 0) {
            snprintf(dir, sizeof(dir), "/");
        } else {
            if ((usize)dlen >= sizeof(dir)) return -1;
            snprintf(dir, sizeof(dir), "%.*s", dlen, path);
        }
        snprintf(base, sizeof(base), "%s", slash + 1);
    } else {
        snprintf(dir, sizeof(dir), ".");
        snprintf(base, sizeof(base), "%s", path);
    }
    if (!base[0]) return -1;

    i32 tab_idx = app_new_filebrowser_tab(app, dir);
    if (tab_idx < 0 || tab_idx >= app->tab_count) return -1;

    Tab *tab = &app->tabs[tab_idx];
    if (!tab->fb) return tab_idx;
    bool opened = false;
    for (i32 i = 0; i < tab->fb->entry_count; i++) {
        if (tab->fb->entries[i].name &&
            strcmp(tab->fb->entries[i].name, base) == 0) {
            opened = fb_open_file(tab->fb, i);
            break;
        }
    }
    if (!opened || tab->fb->view_mode != FVIEW_MARKDOWN) {
        app_close_tab(app, tab_idx);
        return -1;
    }
    tab->fb_viewer_only = true;
    if (tab->fb->view_mode == FVIEW_MARKDOWN)
        tab->fb->md_raw_mode = false;
    snprintf(tab->title, sizeof(tab->title), "%s", base);
    return tab_idx;
}

/* Open `path` (an image file) full-size in a viewer-only file-browser tab.
 * Mirrors app_open_markdown_viewer_tab but accepts the FVIEW_IMAGE branch; the
 * viewer owns zoom/pan so the user gets the existing full-resolution viewer. */
static i32 app_open_image_viewer_tab(AppState *app, const char *path) {
    if (!app || !path || !path[0]) return -1;

    const char *slash = strrchr(path, '/');
    char dir[FB_MAX_PATH];
    char base[512];
    if (slash) {
        i32 dlen = (i32)(slash - path);
        if (dlen <= 0) {
            snprintf(dir, sizeof(dir), "/");
        } else {
            if ((usize)dlen >= sizeof(dir)) return -1;
            snprintf(dir, sizeof(dir), "%.*s", dlen, path);
        }
        snprintf(base, sizeof(base), "%s", slash + 1);
    } else {
        snprintf(dir, sizeof(dir), ".");
        snprintf(base, sizeof(base), "%s", path);
    }
    if (!base[0]) return -1;

    i32 tab_idx = app_new_filebrowser_tab(app, dir);
    if (tab_idx < 0 || tab_idx >= app->tab_count) return -1;

    Tab *tab = &app->tabs[tab_idx];
    if (!tab->fb) return tab_idx;
    bool opened = false;
    for (i32 i = 0; i < tab->fb->entry_count; i++) {
        if (tab->fb->entries[i].name &&
            strcmp(tab->fb->entries[i].name, base) == 0) {
            opened = fb_open_file(tab->fb, i);
            break;
        }
    }
    if (!opened || tab->fb->view_mode != FVIEW_IMAGE) {
        app_close_tab(app, tab_idx);
        return -1;
    }
    tab->fb_viewer_only = true;
    snprintf(tab->title, sizeof(tab->title), "%s", base);
    return tab_idx;
}

/* If a clickable agent-preview thumbnail in `term` sits under (mx,my) — in
 * framebuffer pixels — open its source image full-size and return true. The
 * thumbnail screen rects are cached by the renderer each frame. */
static bool app_term_try_open_image(AppState *app, Terminal *term, f32 mx, f32 my) {
    if (!app || !term || !term->images) return false;
    for (i32 i = 0; i < MAX_TERM_IMAGES; i++) {
        TermImage *im = &term->images[i];
        if (!im->valid || !im->clickable || !im->src_path) continue;
        if (im->scr_w <= 0.0f || im->scr_h <= 0.0f) continue;
        if (mx >= im->scr_x && mx < im->scr_x + im->scr_w &&
            my >= im->scr_y && my < im->scr_y + im->scr_h) {
            /* Copy the path first: opening a viewer tab can reallocate/scroll
             * the source terminal, which may free the slot under us. */
            char path[FB_MAX_PATH];
            snprintf(path, sizeof(path), "%s", im->src_path);
            i32 ti = app_open_image_viewer_tab(app, path);
            if (ti >= 0) {
                app_switch_tab(app, ti);
                if (g_window_ptr) {
                    i32 w, h, fw, fh;
                    platform_window_get_size(g_window_ptr, &w, &h);
                    platform_window_get_framebuffer_size(g_window_ptr, &fw, &fh);
                    app_update_layout(app, w, h, fw, fh);
                }
            }
            return true;
        }
    }
    return false;
}
static void app_create_theme_backspace(AppState *app);
static void app_tick_create_theme(AppState *app);
static void create_theme_kill_child(AppState *app);
static bool app_translate_try_intercept(AppState *app, Terminal *t, Session *s);
static void app_translate_clear_pending_chord(AppState *app);
static void app_tick_translate(AppState *app);
static bool app_translate_segments_advance(AppState *app);
static void app_translate_segments_restore_rest(AppState *app);
static void app_fb_ctx_action(AppState *app, i32 row, i32 entry_index);
static void app_fb_copy(AppState *app, i32 entry_index, bool is_cut);
static bool app_fb_paste(AppState *app);
static void app_fb_begin_rename(AppState *app, i32 entry_index);
static void app_fb_begin_new_folder(AppState *app);
static void app_fb_apply_prompt(AppState *app);
static void app_fb_set_status(AppState *app, const char *msg);
static void app_fb_poll_task(AppState *app);
static void app_tick_updater(AppState *app);
static void on_native_value_change(const char *key, f32 value);

#define FRAME_HZ_BOOST_SEC      0.20
/* While the user is actively typing, drop vsync so keystroke echo presents at
 * render pace instead of being pinned to ProMotion's idle refresh boundary
 * (~24 Hz = ~42 ms/frame). Keyboard-gated only (see app.last_key_time) — output
 * and mouse deliberately don't arm it, so visible streaming output never tears
 * a scroll. Slightly wider than the 0.20 s interaction boost so a key's echo
 * (including a LAN-SSH round trip) stays inside the window. */
#define TYPING_VSYNC_WINDOW_SEC 0.25
#define FRAME_DT_BURST          0.004
#define FRAME_DT_INTERACTIVE    0.008
#define FRAME_DT_ACTIVE         0.016
#define FRAME_DT_SOFT_ANIM      0.033   /* ~30 Hz: agent-accent breathing/sweep */
#define FRAME_DT_CONNECTED_IDLE 0.080
#define FRAME_DT_CURSOR_IDLE    0.125

static void app_note_interaction(AppState *app, f64 now) {
    if (!app) return;
    if (app->interaction_boost_until < now + FRAME_HZ_BOOST_SEC) {
        app->interaction_boost_until = now + FRAME_HZ_BOOST_SEC;
    }
}

typedef struct {
    f32 x, y, w, h;
} MainPaneRect;

static bool main_split_node_rect(const Tab *tab, i32 node, i32 target,
                                 MainPaneRect r, f32 dpi,
                                 MainPaneRect *out_rect) {
    if (!tab || node < 0 || node >= tab->split_next_node) return false;
    const SplitLayoutNode *n = &tab->split_nodes[node];
    if (!n->used) return false;
    if (node == target) {
        if (out_rect) *out_rect = r;
        return true;
    }
    if (n->leaf) return false;

    f32 ratio = n->ratio;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    f32 div = 2.0f * dpi;

    if (n->split == SPLIT_V) {
        f32 split = r.h * ratio;
        MainPaneRect first = {r.x, r.y, r.w, split - div * 0.5f};
        MainPaneRect second = {r.x, r.y + split + div * 0.5f,
                               r.w, r.h - split - div * 0.5f};
        if (main_split_node_rect(tab, n->first, target, first, dpi, out_rect)) return true;
        return main_split_node_rect(tab, n->second, target, second, dpi, out_rect);
    }

    f32 split = r.w * ratio;
    MainPaneRect first = {r.x, r.y, split - div * 0.5f, r.h};
    MainPaneRect second = {r.x + split + div * 0.5f, r.y,
                           r.w - split - div * 0.5f, r.h};
    if (main_split_node_rect(tab, n->first, target, first, dpi, out_rect)) return true;
    return main_split_node_rect(tab, n->second, target, second, dpi, out_rect);
}

static KeyCode quake_hotkey_key_from_name(const char *name) {
    if (!name || !name[0]) return KEY_UNKNOWN;

    if (strlen(name) == 1) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') return KEY_A + (KeyCode)(c - 'A');
        if (c >= 'a' && c <= 'z') return KEY_A + (KeyCode)(c - 'a');
        if (c >= '0' && c <= '9') return KEY_0 + (KeyCode)(c - '0');
        if (c == '`') return KEY_UNKNOWN;
    }

    if (strcasecmp(name, "Tab") == 0) return KEY_TAB;
    if (strcasecmp(name, "Enter") == 0) return KEY_ENTER;
    if (strcasecmp(name, "Esc") == 0 || strcasecmp(name, "Escape") == 0) return KEY_ESCAPE;
    if (strcasecmp(name, "Space") == 0) return KEY_SPACE;
    if (strcasecmp(name, "PageUp") == 0) return KEY_PAGE_UP;
    if (strcasecmp(name, "PageDn") == 0 || strcasecmp(name, "PageDown") == 0) return KEY_PAGE_DOWN;
    if (strcasecmp(name, "Home") == 0) return KEY_HOME;
    if (strcasecmp(name, "End") == 0) return KEY_END;
    if (strcasecmp(name, "Up") == 0) return KEY_UP;
    if (strcasecmp(name, "Down") == 0) return KEY_DOWN;
    if (strcasecmp(name, "Left") == 0) return KEY_LEFT;
    if (strcasecmp(name, "Right") == 0) return KEY_RIGHT;
    if (strcasecmp(name, "Backtick") == 0 || strcasecmp(name, "Grave") == 0) return KEY_UNKNOWN;
    return KEY_UNKNOWN;
}

static bool parse_quake_hotkey(const char *hotkey, u32 *key_out, u32 *mods_out) {
    if (!hotkey || !hotkey[0] || !key_out || !mods_out) return false;

    char hotkey_buf[64];
    snprintf(hotkey_buf, sizeof(hotkey_buf), "%s", hotkey);

    u32 mods = 0;
    char *key_name = hotkey_buf;
    char *last_plus = strrchr(hotkey_buf, '+');
    if (last_plus) {
        *last_plus = '\0';
        key_name = last_plus + 1;

        if (strstr(hotkey_buf, "Ctrl") || strstr(hotkey_buf, "ctrl")) mods |= MOD_CTRL;
        if (strstr(hotkey_buf, "Alt") || strstr(hotkey_buf, "alt")) mods |= MOD_ALT;
        if (strstr(hotkey_buf, "Shift") || strstr(hotkey_buf, "shift")) mods |= MOD_SHIFT;
        if (strstr(hotkey_buf, "Cmd") || strstr(hotkey_buf, "cmd") ||
            strstr(hotkey_buf, "Super") || strstr(hotkey_buf, "super")) mods |= MOD_SUPER;
    }

    KeyCode key = quake_hotkey_key_from_name(key_name);
    if (key == KEY_UNKNOWN &&
        strcmp(key_name, "`") != 0 &&
        strcasecmp(key_name, "Backtick") != 0 &&
        strcasecmp(key_name, "Grave") != 0) {
        return false;
    }

    *key_out = (u32)key;
    *mods_out = mods;
    return true;
}

static void app_register_quake_hotkey(const AppConfig *cfg, PlatformWindow *window) {
    u32 key = KEY_UNKNOWN;
    u32 mods = MOD_CTRL;

    if (cfg) {
        u32 parsed_key = KEY_UNKNOWN;
        u32 parsed_mods = MOD_NONE;
        if (parse_quake_hotkey(cfg->quake_hotkey, &parsed_key, &parsed_mods)) {
            key = parsed_key;
            mods = parsed_mods;
        }
    }

    platform_register_global_hotkey(key, mods,
        (GlobalHotkeyCallback)platform_toggle_quake_window, window);
}

/* Config hot-reload state */
static f64 g_config_changed_time = 0;   /* timestamp when file change detected, 0 = none */
#define CONFIG_RELOAD_DEBOUNCE 0.1       /* seconds to wait before reloading */

static void on_config_file_changed(const char *path, void *userdata) {
    (void)path; (void)userdata;
    /* Record the time; actual reload happens in main loop after debounce. */
    g_config_changed_time = platform_time_sec();
}

/* =========================================================================
 * Key manager helpers
 * ========================================================================= */

static void app_open_key_manager(AppState *app) {
    app->key_manager_active = true;
    app->keygen_form_active = false;
    app->keygen_confirm_delete = false;
    app->key_list_selected = -1;
    app->key_list_scroll = 0;
    app->keygen_status[0] = '\0';

    /* Lazy allocate the key_list pool on first open. Saves ~32 KB at idle. */
    if (!app->key_list) {
        if (app->key_list_cap <= 0) app->key_list_cap = 32;
        app->key_list = calloc((usize)app->key_list_cap, sizeof(*app->key_list));
        if (!app->key_list) { app->key_manager_active = false; return; }
    }

    /* Scan keys */
    char ssh_dir[512];
    ssh_get_default_dir(ssh_dir, sizeof(ssh_dir));
    ssh_scan_keys(ssh_dir, app->key_list, &app->key_list_count, app->key_list_cap);
}

static void app_open_keygen_form(AppState *app) {
    app->keygen_form_active = true;
    app->keygen_type = 0; /* Ed25519 */
    memset(app->keygen_filename, 0, sizeof(app->keygen_filename));
    memset(app->keygen_passphrase, 0, sizeof(app->keygen_passphrase));
    memset(app->keygen_passphrase2, 0, sizeof(app->keygen_passphrase2));
    memset(app->keygen_comment, 0, sizeof(app->keygen_comment));
    app->keygen_field = 0;
    app->keygen_status[0] = '\0';
}

static void app_keygen_submit(AppState *app) {
    /* Validate passphrase match */
    if (strcmp(app->keygen_passphrase, app->keygen_passphrase2) != 0) {
        snprintf(app->keygen_status, sizeof(app->keygen_status),
                 "Error: Passphrases do not match");
        return;
    }

    /* Determine algorithm */
    KeyAlgorithm algo;
    const char *default_name;
    switch (app->keygen_type) {
    case 0: algo = KEYGEN_ED25519;    default_name = "id_ed25519"; break;
    case 1: algo = KEYGEN_RSA_2048;   default_name = "id_rsa"; break;
    case 2: algo = KEYGEN_RSA_4096;   default_name = "id_rsa"; break;
    case 3: algo = KEYGEN_ECDSA_P256; default_name = "id_ecdsa"; break;
    case 4: algo = KEYGEN_ECDSA_P384; default_name = "id_ecdsa"; break;
    case 5: algo = KEYGEN_ECDSA_P521; default_name = "id_ecdsa"; break;
    default: algo = KEYGEN_ED25519;   default_name = "id_ed25519"; break;
    }

    const char *fname = app->keygen_filename[0] ? app->keygen_filename : default_name;
    const char *comment = app->keygen_comment[0] ? app->keygen_comment : NULL;
    const char *pass = app->keygen_passphrase[0] ? app->keygen_passphrase : NULL;

    /* Build full path */
    char ssh_dir[512];
    ssh_get_default_dir(ssh_dir, sizeof(ssh_dir));
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", ssh_dir, fname);

    /* Check if file already exists */
    struct stat st;
    if (stat(filepath, &st) == 0) {
        snprintf(app->keygen_status, sizeof(app->keygen_status),
                 "Error: File '%s' already exists", fname);
        return;
    }

    char fingerprint[128] = {0};
    bool ok = keygen_generate_to_file(algo, comment, pass, filepath,
                                       fingerprint, sizeof(fingerprint));
    if (ok) {
        snprintf(app->keygen_status, sizeof(app->keygen_status),
                 "Key generated: %s", fingerprint);
        app->keygen_form_active = false;
        /* Refresh key list */
        ssh_scan_keys(ssh_dir, app->key_list, &app->key_list_count, app->key_list_cap);
    } else {
        snprintf(app->keygen_status, sizeof(app->keygen_status),
                 "Error: Key generation failed");
    }
}

static void app_refresh_vault_views(AppState *app) {
    if (!app->vault) return;
    sidebar_load_hosts(app->vault);
    if (app->palette_active) {
        palette_open(app->vault);
    }
}

static void app_broadcast_select_all(AppState *app, bool enabled) {
    for (i32 i = 0; i < app->tab_count; i++) {
        app->broadcast_targets[i][0] = enabled && app->tabs[i].session;
        app->broadcast_targets[i][1] = enabled && app->tabs[i].session2;
    }
}

static bool app_broadcast_has_targets(const AppState *app) {
    for (i32 i = 0; i < app->tab_count; i++) {
        if (app->tabs[i].session && app->broadcast_targets[i][0]) return true;
        if (app->tabs[i].session2 && app->broadcast_targets[i][1]) return true;
    }
    return false;
}

static void app_broadcast_select_active(AppState *app) {
    app_broadcast_select_all(app, false);
    Tab *tab = app_active_tab(app);
    if (!tab) return;
    app->broadcast_targets[app->active_tab][tab->active_pane == 1 ? 1 : 0] = true;
}

static void app_enable_broadcast(AppState *app, bool open_overlay) {
    app->broadcast_mode = true;
    if (!app_broadcast_has_targets(app)) {
        app_broadcast_select_all(app, true);
    }
    app->broadcast_overlay_active = open_overlay;
}

static void app_disable_broadcast(AppState *app) {
    app->broadcast_mode = false;
    app->broadcast_overlay_active = false;
}

static void app_mark_session_activity(AppState *app, Session *target, f64 now) {
    if (!app || !target || now <= 0) return;
    for (i32 i = 0; i < app->tab_count; i++) {
        if (app->tabs[i].session == target || app->tabs[i].session2 == target) {
            app->tabs[i].last_activity_time = now;
            return;
        }
    }
}

static void app_send_input(AppState *app, Session *focused, const u8 *data, i32 len) {
    if (len <= 0 || !data) return;
    /* Typing returns the focused view to the live bottom — the user wants to
     * interact, so input cancels any scrollback they were reading. (Output
     * alone no longer snaps to bottom; see terminal_feed.) */
    Terminal *ft = app_focused_terminal(app);
    if (ft) terminal_scroll_to_bottom(ft);
    f64 _ks_now = platform_time_sec();
    /* Stamp the keystroke time on the tab that owns `s` so the agent accent-bar
     * gate can distinguish keystroke echo / TUI repaint from real agent output. */
    #define STAMP_KEYSTROKE_TAB(sess) do {                                  \
        for (i32 _t = 0; _t < app->tab_count; _t++) {                       \
            if (app->tabs[_t].session == (sess) ||                          \
                app->tabs[_t].session2 == (sess)) {                         \
                app->tabs[_t].last_keystroke_time = _ks_now; break;         \
            }                                                               \
        }                                                                   \
    } while (0)

    if (!app->broadcast_mode || !focused) {
        if (focused) {
            session_write(focused, data, len);
            app_mark_session_activity(app, focused, _ks_now);
            STAMP_KEYSTROKE_TAB(focused);
        }
        return;
    }

    Session *sent[MAX_TABS * 2] = {0};
    i32 sent_count = 0;
    session_write(focused, data, len);
    app_mark_session_activity(app, focused, _ks_now);
    STAMP_KEYSTROKE_TAB(focused);
    sent[sent_count++] = focused;

    for (i32 i = 0; i < app->tab_count; i++) {
        Session *targets[2] = { app->tabs[i].session, app->tabs[i].session2 };
        for (i32 ti = 0; ti < 2; ti++) {
            Session *s = targets[ti];
            if (!s || !app->broadcast_targets[i][ti]) continue;
            bool seen = false;
            for (i32 si = 0; si < sent_count; si++) {
                if (sent[si] == s) { seen = true; break; }
            }
            if (seen) continue;
            session_write(s, data, len);
            app_mark_session_activity(app, s, _ks_now);
            STAMP_KEYSTROKE_TAB(s);
            sent[sent_count++] = s;
        }
    }
    #undef STAMP_KEYSTROKE_TAB
}

static bool app_ctrl_z_should_signal(Session *s) {
    if (!s) return false;
    const char *fg = session_fg_process(s);
    if (!fg || !fg[0]) return false;
    return strcmp(fg, "sh") != 0 &&
           strcmp(fg, "bash") != 0 &&
           strcmp(fg, "zsh") != 0 &&
           strcmp(fg, "fish") != 0;
}

static void app_autosuggest_clear(AppState *app);

static void app_translate_shadow_clear(AppState *app) {
    if (!app) return;
    app->translate_input_shadow[0] = '\0';
    app->translate_input_shadow_len = 0;
    app->translate_input_shadow_term = NULL;
    app->translate_input_shadow_sess = NULL;
    app->translate_input_shadow_time = 0;
    app->translate_input_shadow_overflow = false;
    app_autosuggest_clear(app);
}

static void app_autosuggest_refresh(AppState *app, Terminal *t, Session *s);

static void app_translate_shadow_set(AppState *app, Terminal *t, Session *s,
                                     const char *text) {
    if (!app || !t || !s || !text) return;
    snprintf(app->translate_input_shadow, sizeof app->translate_input_shadow,
             "%s", text);
    app->translate_input_shadow_len = (i32)strlen(app->translate_input_shadow);
    app->translate_input_shadow_term = t;
    app->translate_input_shadow_sess = s;
    app->translate_input_shadow_time = platform_time_sec();
    app->translate_input_shadow_overflow =
        strlen(text) > sizeof(app->translate_input_shadow) - 1;
    /* Every shadow writer must end in refresh-or-clear, or the ghost
     * latches a stale suffix (paste path was the offender here). */
    app_autosuggest_refresh(app, t, s);
}

static void app_translate_shadow_append_bytes(AppState *app, Terminal *t,
                                              Session *s, const u8 *data,
                                              i32 len) {
    if (!app || !t || !s || !data || len <= 0) return;
    if (app->translate_input_shadow_term != t ||
        app->translate_input_shadow_sess != s) {
        app_translate_shadow_clear(app);
        app->translate_input_shadow_term = t;
        app->translate_input_shadow_sess = s;
    }
    i32 cap = (i32)sizeof(app->translate_input_shadow) - 1;
    i32 room = cap - app->translate_input_shadow_len;
    /* A clamped append means the shadow no longer mirrors the prompt —
     * flag it so capture falls back to the grid instead of computing a
     * too-small backspace count from a truncated prefix. */
    if (len > room) app->translate_input_shadow_overflow = true;
    if (room <= 0) return;
    if (len > room) len = room;
    memcpy(app->translate_input_shadow + app->translate_input_shadow_len,
           data, (usize)len);
    app->translate_input_shadow_len += len;
    app->translate_input_shadow[app->translate_input_shadow_len] = '\0';
    app->translate_input_shadow_time = platform_time_sec();
    app_autosuggest_refresh(app, t, s);
}

static void app_translate_shadow_append_cp(AppState *app, Terminal *t,
                                           Session *s, u32 cp) {
    u8 enc[4];
    u32 n = utf8_encode(cp, enc);
    if (n > 0) {
        app_translate_shadow_append_bytes(app, t, s, enc, (i32)n);
    }
}

static void app_translate_shadow_backspace(AppState *app, Terminal *t, Session *s) {
    if (!app || app->translate_input_shadow_term != t ||
        app->translate_input_shadow_sess != s ||
        app->translate_input_shadow_len <= 0) {
        return;
    }
    i32 i = app->translate_input_shadow_len - 1;
    while (i > 0 &&
           ((unsigned char)app->translate_input_shadow[i] & 0xC0) == 0x80) {
        i--;
    }
    app->translate_input_shadow_len = i;
    app->translate_input_shadow[i] = '\0';
    app->translate_input_shadow_time = platform_time_sec();
    app_autosuggest_refresh(app, t, s);
}

/* Record the just-submitted command (liu's live input shadow) into the current
 * folder's history, so autosuggest / the Option+Up popup can offer it next
 * time. Independent of OSC 133 shell integration — liu already mirrors what is
 * typed at the prompt. Gated to local shells on the primary screen so REPL /
 * TUI input never pollutes the history. */
static void app_record_submitted_command(AppState *app, Terminal *t, Session *s) {
    if (!app || !t || !s) return;
    if (app->translate_input_shadow_term != t ||
        app->translate_input_shadow_sess != s ||
        app->translate_input_shadow_len <= 0) return;
    if (session_type(s) != SESSION_LOCAL) return;
    if (t->mode & MODE_ALT_SCREEN) return;
    const char *fg = session_fg_process(s);
    bool is_shell = fg && (strcmp(fg, "zsh") == 0 || strcmp(fg, "bash") == 0 ||
                           strcmp(fg, "fish") == 0 || strcmp(fg, "sh") == 0);
    if (!is_shell) return;
    const char *cwd = app_terminal_cwd(t, s);
    if (cwd && cwd[0]) {
        /* Learning signal: a ghost was on screen for this prompt episode
         * but the user committed a different command — count one reject. */
        if (app->autosuggest_visible &&
            app->autosuggest_target_term == t &&
            app->autosuggest_full[0] &&
            strcmp(app->autosuggest_full, app->translate_input_shadow) != 0) {
            cmd_suggest_feedback(cwd, app->autosuggest_full, false);
        }
        cmd_history_record(cwd, app->translate_input_shadow);
    }
}

/* ===========================================================================
 * Inline autosuggest — fish-style ghost completion
 *
 * Matches the live typed prefix against the *current folder's* command history
 * (see core/cmd_history.h) and exposes a single helper (app_autosuggest_refresh)
 * the input + render paths call into. The ACT_ACCEPT_SUGGESTION keybinding
 * (default Right arrow, rebindable) accepts the suggestion by injecting the
 * still-untyped suffix into the PTY. Suggestions are scoped per directory, so
 * each project gets its own.
 * =========================================================================== */

static void app_autosuggest_clear(AppState *app) {
    if (!app) return;
    app->autosuggest_visible = false;
    app->autosuggest_full[0] = '\0';
    app->autosuggest_suffix[0] = '\0';
    app->autosuggest_target_term = NULL;
    app->autosuggest_target_sess = NULL;
}

/* Updates autosuggest_full / autosuggest_suffix based on the current
 * shadow input for (t, s). Skips when the terminal is on the alt screen
 * (TUI active) or the foreground isn't a shell. */
static void app_autosuggest_refresh(AppState *app, Terminal *t, Session *s) {
    if (!app || !t || !s) {
        if (app) app_autosuggest_clear(app);
        return;
    }
    /* Gate: only at the shell prompt, primary screen, sane fg process. */
    if (t->mode & MODE_ALT_SCREEN) { app_autosuggest_clear(app); return; }
    const char *fg = session_fg_process(s);
    bool is_shell = fg && (strcmp(fg, "zsh") == 0 ||
                           strcmp(fg, "bash") == 0 ||
                           strcmp(fg, "fish") == 0 ||
                           strcmp(fg, "sh") == 0);
    if (!is_shell) { app_autosuggest_clear(app); return; }
    if (app->translate_input_shadow_term != t ||
        app->translate_input_shadow_sess != s ||
        app->translate_input_shadow_len <= 0) {
        app_autosuggest_clear(app);
        return;
    }
    /* Source suggestions from the folder this shell is sitting in. The
     * learned engine ranks by decayed frequency + command transitions +
     * accept/reject feedback, and falls back to the plain newest-first
     * match whenever its confidence margin is small. */
    const char *cwd = app_terminal_cwd(t, s);
    if (!cwd) { app_autosuggest_clear(app); return; }

    const char *typed = app->translate_input_shadow;
    i32 typed_len     = app->translate_input_shadow_len;
    const char *match = cmd_suggest_best(cwd, typed, typed_len);
    if (!match) {
        app_autosuggest_clear(app);
        return;
    }
    snprintf(app->autosuggest_full, sizeof app->autosuggest_full,
             "%s", match);
    snprintf(app->autosuggest_suffix, sizeof app->autosuggest_suffix,
             "%s", match + typed_len);
    app->autosuggest_visible = (app->autosuggest_suffix[0] != '\0');
    app->autosuggest_target_term = t;
    app->autosuggest_target_sess = s;
}

static bool app_translate_shadow_get(AppState *app, Terminal *t, Session *s,
                                     char *out, usize out_cap, i32 *out_bs) {
    if (!app || !t || !s || !out || out_cap == 0) return false;
    if (app->translate_input_shadow_term != t ||
        app->translate_input_shadow_sess != s ||
        app->translate_input_shadow_len <= 0) {
        return false;
    }
    /* An overflowed shadow is a truncated prefix of the prompt: its
     * backspace count would erase the line mid-content. Let the caller
     * fall back to visual capture instead. */
    if (app->translate_input_shadow_overflow) return false;
    if (platform_time_sec() - app->translate_input_shadow_time > 900.0) {
        return false;
    }
    /* Copy the shadow, truncating only on a UTF-8 boundary so a split
     * multibyte sequence is never handed to the backend. */
    usize slen = (usize)app->translate_input_shadow_len;
    if (slen > out_cap - 1) {
        slen = out_cap - 1;
        while (slen > 0 &&
               ((unsigned char)app->translate_input_shadow[slen] & 0xC0) == 0x80) {
            slen--;
        }
    }
    memcpy(out, app->translate_input_shadow, slen);
    out[slen] = '\0';
    if (out_bs) {
        /* The backspace count must erase the FULL on-screen prompt (one
         * codepoint per editor cell), computed from the whole shadow — not
         * the possibly-truncated copy, or the tail of the user's text is
         * left behind concatenated with the translation. */
        usize cps = utf8_len((const u8 *)app->translate_input_shadow,
                             (usize)app->translate_input_shadow_len);
        *out_bs = cps > 0 ? (i32)cps : app->translate_input_shadow_len;
    }
    return out[0] != '\0';
}

static void app_send_bracketed_paste(AppState *app, Terminal *t, Session *s, const char *clip) {
    if (!t || !s || !clip || !clip[0]) return;
    if (app) {
        snprintf(app->translate_recent_paste,
                 sizeof app->translate_recent_paste, "%s", clip);
        app->translate_recent_paste_time = platform_time_sec();
        app->translate_recent_paste_term = t;
        app->translate_recent_paste_sess = s;
        if (!app->translate_active) {
            app_translate_shadow_set(app, t, s, clip);
        }
    }
    if (t->mode & MODE_BRACKETED_PASTE) {
        app_send_input(app, s, (const u8 *)"\x1b[200~", 6);
    }
    app_send_input(app, s, (const u8 *)clip, (i32)strlen(clip));
    if (t->mode & MODE_BRACKETED_PASTE) {
        app_send_input(app, s, (const u8 *)"\x1b[201~", 6);
    }
}

/* Set once we install the agent hooks at launch. The atexit handler below
 * reads it so we remove them no matter HOW the app exits — crucially including
 * Cmd+Q / Dock-quit, which call [NSApp terminate:] → exit() and never return
 * to main()'s cleanup label. A SIGKILL still can't run it, but the next launch
 * re-installs idempotently and a dead socket means no notification fires in the
 * meantime, so a crash leaves nothing that bothers the user. */
static bool g_liu_hooks_installed = false;
/* Agent ids whose notify hooks this instance installed, so atexit removes
 * exactly those (and nothing the user set up themselves). */
static char g_installed_hook_agents[8][AGENT_ID_CAP];
static i32  g_installed_hook_count = 0;
static bool g_lifecycle_atexit_registered = false;

static void liu_lifecycle_atexit(void) {
    if (g_liu_hooks_installed) {
        for (i32 i = 0; i < g_installed_hook_count; i++)
            agent_hook_uninstall(g_installed_hook_agents[i]);
        g_installed_hook_count = 0;
        g_liu_hooks_installed = false;
    }
    /* Drop the AF_UNIX socket too so a Cmd+Q (which skips notify_server_stop)
     * doesn't leave a stale node that confuses the next launch's bind probe. */
    char sock[256];
    if (notify_socket_path(sock, sizeof sock)) unlink(sock);
    /* SIGKILL+reap any in-flight Settings model-fetch child. Cmd+Q / Dock
     * quit ([NSApp terminate:] → exit()) never reaches app_destroy or the
     * main cleanup label, so without this an in-flight `grok models`
     * (no --max-time) would be orphaned to launchd. */
    model_catalog_shutdown();
}

/* Register liu_lifecycle_atexit exactly once, no matter how many subsystems
 * ask for it — atexit() has a small fixed table, so repeated calls must not
 * pile up registrations. */
static void ensure_lifecycle_atexit(void) {
    if (g_lifecycle_atexit_registered) return;
    atexit(liu_lifecycle_atexit);
    g_lifecycle_atexit_registered = true;
}

/* Resolve the liu-notify binary that ships next to the running executable
 * (Contents/MacOS/liu-notify in the bundle, build/liu-notify in dev). Used to
 * write absolute hook commands into ~/.claude/settings.json. Falls back to the
 * bare name (PATH lookup) when the executable path can't be read. */
static bool resolve_liu_notify_path(char *out, usize cap) {
    if (!out || cap < 4) return false;
#if defined(__APPLE__)
    char exe[1024]; u32 exe_sz = sizeof exe;
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

/* Open a new local PTY in the active tab's folder so a fresh tab lands where
 * the user currently is, not $HOME. Falls back to the default landing dir when
 * there's no inheritable local cwd (e.g. the active pane is an SSH session). */
static Session *app_new_local_session_here(AppState *app) {
    const char *cwd = app_active_local_cwd(app);
    return session_create_local_with_env(app->grid_cols, app->grid_rows,
                                         NULL, 0, cwd);
}

/* Backslash-escape a filesystem path for insertion into the terminal on a
 * drag-and-drop, matching the platform convention. A shell receives a single
 * token, and agent CLIs that pick up dropped image paths (Claude Code, Codex,
 * …) get exactly the path text they expect. A trailing space is appended so
 * consecutive drops stay separated. UTF-8 bytes (>= 0x80, e.g. Turkish file
 * names) pass through untouched. Returns bytes written (excluding the NUL). */
static usize drop_path_escape(char *out, usize cap, const char *path) {
    if (!out || cap == 0) return 0;
    usize j = 0;
    for (const char *p = path; p && *p && j + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' '  || c == '\t' || c == '\n' || c == '\\' || c == '"'  ||
            c == '\'' || c == '('  || c == ')'  || c == '['  || c == ']'  ||
            c == '{'  || c == '}'  || c == '<'  || c == '>'  || c == '|'  ||
            c == '&'  || c == ';'  || c == '$'  || c == '`'  || c == '!'  ||
            c == '*'  || c == '?') {
            out[j++] = '\\';
        }
        out[j++] = (char)c;
    }
    if (j + 1 < cap) out[j++] = ' ';   /* trailing space for command separation */
    out[j] = '\0';
    return j;
}

/* Paste a shell command AND submit it. The terminating CR lands outside the
 * bracketed-paste envelope so the shell treats it as a real newline (inside
 * the envelope it would be quoted literally). Used by the agent picker where
 * the user has already committed to running the command. */
static void app_send_command_and_execute(AppState *app, Terminal *t, Session *s, const char *cmd) {
    if (!t || !s || !cmd || !cmd[0]) return;
    app_send_bracketed_paste(app, t, s, cmd);
    app_send_input(app, s, (const u8 *)"\r", 1);
}

/* Agent session ids / paths are interpolated into shell command lines that are
 * then pasted and executed. They originate from on-disk agent-history files, so
 * a crafted id with shell metacharacters would be a command-injection vector
 * (bracketed paste does NOT shell-quote). Allow only the characters that
 * legitimately appear in session ids and resume paths (UUIDs, hashes,
 * timestamps, filesystem paths). */
static bool agent_arg_is_safe(const char *s) {
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == '/' ||
                  c == '@' || c == ':' || c == '+' || c == '=';
        if (!ok) return false;
    }
    return true;
}

static void app_import_default_ssh_config(AppState *app) {
    if (!app->vault) return;
    i32 imported = import_ssh_config(app->vault, NULL);
    if (imported > 0) {
        app_refresh_vault_views(app);
    }
}

static void app_open_host_from_palette(AppState *app, const PaletteItem *item) {
    if (!app->vault || !item) return;
    /* item->id is the host's ordinal position in vault_host_list() order, not a
     * string id — fetch just that row instead of materializing all hosts into a
     * ~144 KB stack array to read one. */
    VaultHost host;
    if (item->id < 0 || !vault_host_get_at(app->vault, item->id, &host)) return;
    const VaultHost *h = &host;
    if (h->protocol == PROTO_LOCAL) {
        /* Inject vault globals + any host-scoped env vars into the
         * child's environment. Silent no-op when the vault is locked. */
        LocalEnvVar envs[16];
        i32 nenv = 0;
        if (app->vault && vault_is_unlocked(app->vault)) {
            VaultEnvVar tmp[16];
            nenv = vault_env_list(app->vault, h->id, tmp, 16);
            for (i32 i = 0; i < nenv && i < 16; i++) {
                snprintf(envs[i].name, sizeof envs[i].name, "%s", tmp[i].name);
                snprintf(envs[i].value, sizeof envs[i].value, "%s", tmp[i].value);
                crypto_secure_zero(tmp[i].value, sizeof tmp[i].value);
            }
        }
        Session *s = session_create_local_with_env(app->grid_cols, app->grid_rows,
                                                    nenv ? envs : NULL, nenv, NULL);
        /* Once forked, the child has its own copy — wipe the parent's. */
        for (i32 i = 0; i < nenv; i++) {
            crypto_secure_zero(envs[i].value, sizeof envs[i].value);
        }
        if (s && session_status(s) == SESSION_CONNECTED) {
            app_new_tab(app, s, h->label[0] ? h->label : "Terminal");
        }
        return;
    }

    if (h->protocol != PROTO_SSH) return;

    /* If the host has any vault secret linked, attempt a direct connect.
     * Otherwise fall back to pre-filling the SSH dialog like before. */
    bool has_vault_secret =
        h->password_secret_id[0] || h->passphrase_secret_id[0] ||
        h->private_key_secret_id[0];
    if (!has_vault_secret) {
        app_reset_ssh_dialog(app);
        snprintf(app->ssh_host, sizeof app->ssh_host, "%s", h->hostname);
        snprintf(app->ssh_user, sizeof app->ssh_user, "%s", h->username);
        if (h->port > 0) snprintf(app->ssh_port, sizeof app->ssh_port, "%d", h->port);
        return;
    }

    /* Vault-backed host: reveal secrets, build SSHConfig, connect. */
    SSHConfig cfg = ssh_config_default();
    snprintf(cfg.hostname, sizeof cfg.hostname, "%s", h->hostname);
    snprintf(cfg.username, sizeof cfg.username, "%s", h->username);
    cfg.port = h->port > 0 ? h->port : 22;

    /* Populate env vars (globals + host-scoped) while the vault is unlocked.
     * Values are copied into SSHConfig; their plaintext lives until the
     * session is destroyed (see ssh_session.c session_destroy for wipe). */
    if (app->vault && vault_is_unlocked(app->vault)) {
        VaultEnvVar tmp[16];
        i32 ne = vault_env_list(app->vault, h->id, tmp, 16);
        for (i32 i = 0; i < ne && i < 16; i++) {
            SSHEnvVar *slot = ssh_config_push_env(&cfg);
            if (!slot) break;
            snprintf(slot->name,  sizeof slot->name,  "%s", tmp[i].name);
            snprintf(slot->value, sizeof slot->value, "%s", tmp[i].value);
            /* Scrub the stack-local plaintext copy now that it's in cfg. */
            crypto_secure_zero(tmp[i].value, sizeof tmp[i].value);
        }
    }
    snprintf(cfg.password_secret_id, sizeof cfg.password_secret_id,
             "%s", h->password_secret_id);
    snprintf(cfg.passphrase_secret_id, sizeof cfg.passphrase_secret_id,
             "%s", h->passphrase_secret_id);
    snprintf(cfg.private_key_secret_id, sizeof cfg.private_key_secret_id,
             "%s", h->private_key_secret_id);
    if (h->key_id[0])
        snprintf(cfg.key_path, sizeof cfg.key_path, "%s", h->key_id);
    cfg.auth_method = (h->auth_method == VAUTH_PUBLICKEY) ? AUTH_PUBLICKEY
                    : (h->auth_method == VAUTH_AGENT)     ? AUTH_AGENT
                    : AUTH_PASSWORD;

    bool need_unlock = false;
    if (!app_vault_populate_ssh_secrets(app, &cfg, &need_unlock)) {
        if (need_unlock) {
            app_vault_open_unlock(app, 0);
            app_show_toast(app, "Unlock vault and retry");
        } else {
            app_show_toast(app, "Failed to reveal host secrets");
        }
        /* Dispose handles secure_zero on credentials + frees dynamic arrays
         * + key_pem so nothing leaks no matter how far we got. */
        ssh_config_dispose(&cfg);
        return;
    }

    /* Resolve against ~/.ssh/config for ProxyJump / HostName overrides. */
    SSHResolvedConfig *resolved = ssh_config_resolve(cfg.hostname);
    if (resolved) {
        ssh_config_apply(resolved, &cfg, false);
        ssh_config_free(resolved);
    }

    Session *s = session_create_ssh(&cfg, app->grid_cols, app->grid_rows);
    if (s) {
        char title[256];
        snprintf(title, sizeof title, "%s@%s",
                 cfg.username, cfg.hostname);
        app_new_tab(app, s, title);
        vault_add_history(app->vault, cfg.hostname, cfg.port,
                          cfg.username, "password",
                          session_status(s) != SESSION_ERROR);
    } else {
        app_show_toast(app, "Connect failed");
    }
    /* The session owns its own deep clone; this stack copy is done.
     * Dispose scrubs credentials + frees the dynamic arrays/key_pem. */
    ssh_config_dispose(&cfg);
}

static void app_open_history_from_palette(AppState *app, const PaletteItem *item) {
    if (!app || !item) return;
    /* Parse the detail field "user@host:port" to fill SSH dialog */
    app_reset_ssh_dialog(app);

    /* The detail is in format "user@host:port" */
    char detail[128];
    snprintf(detail, sizeof(detail), "%s", item->detail);
    char *at = strchr(detail, '@');
    char *colon = strrchr(detail, ':');

    if (at) {
        *at = '\0';
        snprintf(app->ssh_user, sizeof(app->ssh_user), "%s", detail);
        char *host_start = at + 1;
        if (colon && colon > at) {
            *colon = '\0';
            snprintf(app->ssh_host, sizeof(app->ssh_host), "%s", host_start);
            snprintf(app->ssh_port, sizeof(app->ssh_port), "%s", colon + 1);
        } else {
            snprintf(app->ssh_host, sizeof(app->ssh_host), "%s", host_start);
        }
    } else {
        if (colon) {
            *colon = '\0';
            snprintf(app->ssh_host, sizeof(app->ssh_host), "%s", detail);
            snprintf(app->ssh_port, sizeof(app->ssh_port), "%s", colon + 1);
        } else {
            snprintf(app->ssh_host, sizeof(app->ssh_host), "%s", detail);
        }
    }
}

static bool app_snippet_lookup_value(AppState *app, Terminal *t, Session *s,
                                     const char *name, char *out, usize out_size) {
    const SSHConfig *cfg = s ? session_get_config(s) : NULL;

    if (strcmp(name, "host") == 0 || strcmp(name, "hostname") == 0) {
        if (cfg && cfg->hostname[0]) snprintf(out, out_size, "%s", cfg->hostname);
        else snprintf(out, out_size, "localhost");
        return true;
    }
    if (strcmp(name, "user") == 0 || strcmp(name, "username") == 0) {
        if (cfg && cfg->username[0]) snprintf(out, out_size, "%s", cfg->username);
        else snprintf(out, out_size, "%s", getenv("USER") ? getenv("USER") : "");
        return true;
    }
    if (strcmp(name, "port") == 0) {
        if (cfg && cfg->port > 0) snprintf(out, out_size, "%d", cfg->port);
        else out[0] = '\0';
        return true;
    }
    if (strcmp(name, "cwd") == 0) {
        snprintf(out, out_size, "%s", (t && t->cwd) ? t->cwd : "");
        return true;
    }
    if (strcmp(name, "title") == 0 || strcmp(name, "tab") == 0) {
        Tab *tab = app_active_tab(app);
        snprintf(out, out_size, "%s", tab ? tab_effective_title(tab) : "");
        return true;
    }
    if (strcmp(name, "shell") == 0) {
        snprintf(out, out_size, "%s", s ? session_fg_process(s) : "");
        return true;
    }
    if (strcmp(name, "clipboard") == 0) {
        const char *clip = platform_clipboard_get();
        snprintf(out, out_size, "%s", clip ? clip : "");
        return true;
    }
    if (strcmp(name, "selection") == 0) {
        char *sel = t ? selection_get_text(t) : NULL;
        if (sel) {
            snprintf(out, out_size, "%s", sel);
            free(sel);
        } else {
            out[0] = '\0';
        }
        return true;
    }
    if (strcmp(name, "newline") == 0 || strcmp(name, "enter") == 0) {
        snprintf(out, out_size, "\n");
        return true;
    }
    if (strcmp(name, "home") == 0) {
        snprintf(out, out_size, "%s", getenv("HOME") ? getenv("HOME") : "");
        return true;
    }
    return false;
}

static void app_expand_snippet_command(AppState *app, Terminal *t, Session *s,
                                       const char *command, char *out, usize out_size) {
    usize op = 0;
    for (usize i = 0; command && command[i] && op + 1 < out_size; ) {
        if (command[i] == '$' && command[i + 1] == '{') {
            usize start = i;
            i += 2;
            char name[64];
            usize np = 0;
            while (command[i] && command[i] != '}' && np + 1 < sizeof(name)) {
                name[np++] = command[i++];
            }
            name[np] = '\0';
            if (command[i] == '}') {
                char value[4096];
                if (app_snippet_lookup_value(app, t, s, name, value, sizeof(value))) {
                    usize vl = strlen(value);
                    if (vl > out_size - op - 1) vl = out_size - op - 1;
                    memcpy(out + op, value, vl);
                    op += vl;
                    i++;
                    continue;
                }
            }
            while (start < i && op + 1 < out_size) out[op++] = command[start++];
            if (command[i] == '}' && op + 1 < out_size) out[op++] = command[i++];
            continue;
        }
        out[op++] = command[i++];
    }
    out[op] = '\0';
}

/* Streaming snippet picker — match by index without building a full
 * VaultSnippet[128] (~600 KB stack frame). Snippet ID is the row index; we
 * count rows until we hit `target` and copy that one row's command. */
typedef struct {
    i32   target;
    i32   index;
    char *out;
    usize out_cap;
    bool  found;
} SnippetByIdCtx;

static bool snippet_by_id_cb(const VaultSnippet *snip, void *user) {
    SnippetByIdCtx *ctx = (SnippetByIdCtx *)user;
    if (ctx->index == ctx->target) {
        snprintf(ctx->out, ctx->out_cap, "%s", snip->command);
        ctx->found = true;
        ctx->index++;
        return false;   /* stop iteration */
    }
    ctx->index++;
    return true;
}

static void app_execute_snippet_from_palette(AppState *app, const PaletteItem *item) {
    if (!app->vault || !item || item->id < 0) return;

    /* Pull only the targeted snippet's command — no full-list array. */
    char command[4096];
    SnippetByIdCtx ctx = {
        .target = item->id, .index = 0,
        .out = command, .out_cap = sizeof command, .found = false
    };
    vault_snippet_for_each(app->vault, snippet_by_id_cb, &ctx);
    if (!ctx.found) return;

    Terminal *t = app_focused_terminal(app);
    Session *s = app_focused_session(app);
    if (!t || !s) return;

    char expanded[8192];
    app_expand_snippet_command(app, t, s, command, expanded, sizeof(expanded));
    if (expanded[0]) app_send_bracketed_paste(app, t, s, expanded);
}

/* Returns true when the app is currently tabless and a replacement local
 * terminal was spawned. Used by the "close tab" code paths so closing the
 * last tab keeps the window alive — the expected desktop-terminal behavior,
 * avoiding the dock-shrink-then-quit users see when count drops to 0. */
static bool app_respawn_on_empty(AppState *app) {
    if (!app || app->tab_count > 0) return false;
    Session *s = session_create_local(app->grid_cols, app->grid_rows);
    if (!s || session_status(s) != SESSION_CONNECTED) {
        if (s) session_destroy(s);
        return false;
    }
    app_new_tab(app, s, "Terminal");
    return true;
}

/* Gate a user-initiated close behind a confirmation when an AI agent CLI
 * (claude, codex, grok, opencode, commandcode, …) is sitting in the PTY
 * foreground of the close target. pane >= 0 checks one split pane,
 * group >= 0 checks every tab of that group, otherwise the whole tab.
 * Returns true when the close was deferred to the dialog — the caller must
 * skip the close; false lets the caller close immediately. */
static bool app_confirm_close_arm(AppState *app, i32 tab_index, i32 pane, i32 group) {
    if (!app->config.confirm_close_agent) return false;
    /* Don't arm behind another overlay: those interceptors run before the
     * close-confirm one and/or paint on top of it, so an armed dialog would
     * be invisible and unreachable. In that state the close proceeds without
     * a prompt (the pre-feature behavior). */
    if (app->settings.open || app->sites.active || app->key_manager_active ||
        app->cmd_history_active || app->transcript_viewer_active ||
        app->agent_picker_active || app->passphrase_dialog_active ||
        app->kbi_dialog_active || app->hostkey_dialog_active ||
        app->known_hosts_open || app->port_forward_dialog_active ||
        app->broadcast_overlay_active || app->ssh_dialog_active ||
        app->create_theme_active || app->vault_unlock_dialog_active ||
        app->vault_browser_active) {
        return false;
    }
    const char *disp = NULL;
    i32 tool = 0;
    if (group >= 0) {
        for (i32 i = 0; i < app->tab_count && !tool; i++) {
            if (app->tabs[i].group_index == group)
                tool = app_tab_running_agent(app, i, &disp);
        }
    } else if (pane >= 0) {
        tool = app_pane_running_agent(app, tab_index, pane, &disp);
    } else {
        tool = app_tab_running_agent(app, tab_index, &disp);
    }
    if (!tool) return false;
    app->close_confirm_active = true;
    app->close_confirm_tab = tab_index;
    app->close_confirm_pane = pane;
    app->close_confirm_group = group;
    snprintf(app->close_confirm_agent, sizeof(app->close_confirm_agent),
             "%s", (disp && disp[0]) ? disp : "An AI agent");
    return true;
}

/* Reload the font atlas + relayout after a font/cell-size change.
 *
 * IMPORTANT: clear the renderer's cached atlas pointer BEFORE destroying the
 * old atlas so any render-flush reached during rebuild sees NULL and returns
 * early. The dangling MTLTexture pointer otherwise causes objc_retain to
 * segfault on the next setFragmentTexture: call. */
static void app_reload_font(AppState *app, PlatformWindow *window) {
#ifdef USE_METAL
    renderer_metal_set_atlas(&app->renderer, NULL);
#endif
    font_atlas_destroy(&app->renderer.font);
    font_atlas_create(&app->renderer.font, app->config.font_path,
                      app->config.font_size, app->dpi_scale,
                      app->config.font_weight);
#ifdef USE_METAL
    font_atlas_create_metal_texture(&app->renderer.font, platform_get_gpu_device(window));
    renderer_metal_set_atlas(&app->renderer, app->renderer.font.metal_texture);
#endif
    app->renderer.font.cell_width  *= app->config.cell_width_scale;
    app->renderer.font.cell_height *= app->config.cell_height_scale;
    i32 w, h, fw, fh;
    platform_window_get_size(window, &w, &h);
    platform_window_get_framebuffer_size(window, &fw, &fh);
    app_update_layout(app, w, h, fw, fh);
}

/* ===========================================================================
 * Agent resume picker — detection + row assembly
 * =========================================================================== */

/* Check if `dir/name` exists and is executable. `dir` may be ~-prefixed or
 * start with $VAR; expanded inline. Returns true on a hit. */
static bool agent_probe(const char *dir, usize dlen, const char *name, usize nlen) {
    char buf[1024];
    const char *home = NULL;
    usize off = 0;

    /* Expand a leading ~ into $HOME. */
    if (dlen >= 1 && dir[0] == '~') {
        home = getenv("HOME");
        if (!home) return false;
        usize hn = strlen(home);
        if (hn + dlen + nlen + 2 >= sizeof buf) return false;
        memcpy(buf + off, home, hn); off += hn;
        memcpy(buf + off, dir + 1, dlen - 1); off += dlen - 1;
    } else {
        if (dlen + nlen + 2 >= sizeof buf) return false;
        memcpy(buf + off, dir, dlen); off += dlen;
    }
    buf[off++] = '/';
    memcpy(buf + off, name, nlen); off += nlen;
    buf[off] = '\0';
    return access(buf, X_OK) == 0;
}

/* "which" for agent binaries. Probes $PATH first, then a curated set of
 * common install locations that are invisible to GUI apps on macOS — launchd
 * hands out a minimal PATH (/usr/bin:/bin:...) that omits `/opt/homebrew/bin`,
 * `/usr/local/bin`, `~/.local/bin`, etc. Without these fallbacks, a binary
 * installed via brew / pipx / npm would be reported "not installed" here. */
static bool agent_binary_on_path(const char *name) {
    if (!name || !*name) return false;
    usize nlen = strlen(name);
    if (nlen >= 512) return false;

    const char *path = getenv("PATH");
    if (path) {
        const char *p = path;
        while (*p) {
            const char *sep = strchr(p, ':');
            usize dlen = sep ? (usize)(sep - p) : strlen(p);
            if (dlen > 0 && agent_probe(p, dlen, name, nlen)) return true;
            if (!sep) break;
            p = sep + 1;
        }
    }

    /* GUI-app PATH fallback: cover the directories a user's login shell would
     * normally add. Order matches typical priority (homebrew > /usr/local >
     * user-local > dotfile dirs). */
    static const char *const extras[] = {
        "/opt/homebrew/bin",
        "/opt/homebrew/sbin",
        "/usr/local/bin",
        "/usr/local/sbin",
        "~/.local/bin",
        "~/bin",
        "~/.cargo/bin",
        "~/.bun/bin",
        "~/.volta/bin",
        "~/.nvm/versions/node/*/bin",  /* glob not expanded — kept for docs */
        "~/go/bin",
    };
    for (usize i = 0; i < sizeof extras / sizeof extras[0]; i++) {
        const char *d = extras[i];
        /* Skip the nvm glob entry — we don't expand wildcards here; users
         * typically have a stable `~/.local/bin` or brew path for CLIs. */
        if (strchr(d, '*')) continue;
        if (agent_probe(d, strlen(d), name, nlen)) return true;
    }
    return false;
}

/* Catalog of CLI coding agents liu knows about. Kept in sync with
 * history/event.h ChatTool enum. Native resume commands are mirrored from
 * cli-continues registry.ts (nativeResumeArgs). `resume_fmt` may be NULL when
 * the agent has no documented native resume flag — then only "start new" is
 * offered. `resume_needs_path` toggles whether the %s is substituted with the
 * session-id or the full on-disk path (codex is the only one that wants the
 * path). `binary` is the $PATH probe name; `new_cmd` runs a fresh session. */
typedef struct {
    u8          tool;              /* ChatTool */
    const char *display;           /* human-readable name */
    const char *binary;            /* $PATH probe target */
    const char *resume_fmt;        /* printf fmt with one %s; NULL → no resume */
    const char *new_cmd;           /* command that starts a fresh session */
    /* Cross-tool handoff: printf fmt with one %s that receives a shell-quoted
     * prompt (e.g. `'Read /path/handoff.md and continue from there'`). The
     * picker uses this to offer "Continue with <other>" for chats originating
     * in a different tool — the source transcript is dumped to a markdown
     * file under ~/.liu-handoffs/ and the target CLI is launched with the
     * pointer as its initial user message. NULL when no CLI exists that can
     * accept a prompt argv (editor extensions, GUI editors). */
    const char *cross_tool_fmt;
    bool        resume_needs_path; /* if true, resume_fmt's %s = session path (not id) */
} AgentCatalogEntry;

static const AgentCatalogEntry AGENT_CATALOG[] = {
    /* tool                  display            binary         resume_fmt                               new_cmd        cross_tool_fmt        needs_path */
    { CHAT_TOOL_CLAUDE,      "Claude",          "claude",      "claude --resume %s",                    "claude",       "claude %s",          false },
    { CHAT_TOOL_CODEX,       "Codex",           "codex",       "codex -c experimental_resume=\"%s\"",  "codex",        "codex %s",           true  },
    { CHAT_TOOL_COPILOT,     "GitHub Copilot",  "copilot",     "copilot --resume %s",                   "copilot",      "copilot -i %s",      false },
    { CHAT_TOOL_CURSOR,      "Cursor",          "agent",       "agent --resume %s",                     "agent",        "agent %s",           false },
    { CHAT_TOOL_AMP,         "Amp",             "amp",         "amp --thread %s",                       "amp",          "amp %s",             false },
    /* Cline/Roo/Kilo are editor extensions — no CLI entry to hand a prompt to. */
    { CHAT_TOOL_CLINE,       "Cline",           "code",        NULL,                                    "code",         NULL,                 false },
    { CHAT_TOOL_ROO,         "Roo Code",        "code",        NULL,                                    "code",         NULL,                 false },
    { CHAT_TOOL_KILO,        "Kilo Code",       "code",        NULL,                                    "code",         NULL,                 false },
    { CHAT_TOOL_KIRO,        "Kiro",            "kiro",        NULL,                                    "kiro",         NULL,                 false },
    { CHAT_TOOL_CRUSH,       "Crush",           "crush",       "crush --session %s",                    "crush",        "crush %s",           false },
    { CHAT_TOOL_OPENCODE,    "OpenCode",        "opencode",    "opencode --session %s",                 "opencode",     "opencode run %s",    false },
    { CHAT_TOOL_DROID,       "Factory Droid",   "droid",       "droid -s %s",                           "droid",        "droid exec %s",      false },
    { CHAT_TOOL_ANTIGRAVITY, "Antigravity",     "antigravity", NULL,                                    "antigravity",  NULL,                 false },
    { CHAT_TOOL_KIMI,        "Kimi",            "kimi",        "kimi --session %s",                     "kimi",         "kimi --prompt %s",   false },
    { CHAT_TOOL_QWEN,        "Qwen",            "qwen",        "qwen --resume %s",                      "qwen",         "qwen %s",            false },
    /* Aider keeps a single rolling chat history — restore replays it. */
    { CHAT_TOOL_AIDER,       "Aider",           "aider",       "aider --restore-chat-history",          "aider",        NULL,                 false },
    { CHAT_TOOL_AMAZON_Q,    "Amazon Q",        "q",           NULL,                                    "q chat",       NULL,                 false },
    { CHAT_TOOL_CONTINUE,    "Continue",        "cn",          NULL,                                    "cn",           NULL,                 false },
    { CHAT_TOOL_WINDSURF,    "Windsurf",        "windsurf",    NULL,                                    "windsurf .",   NULL,                 false },
    { CHAT_TOOL_ZED,         "Zed",             "zed",         NULL,                                    "zed .",        NULL,                 false },
    /* Grok's TUI takes no positional prompt (only headless -p), so there is no
     * interactive cross-tool handoff target. */
    { CHAT_TOOL_XAI,         "Grok",            "grok",        "grok --resume %s",                      "grok",         NULL,                 false },
    /* --resume takes a conversation NAME; an unmatched session id falls back
     * to commandcode's own history picker, which is a fine degradation. */
    { CHAT_TOOL_COMMANDCODE, "CommandCode",     "commandcode", "commandcode --resume %s",               "commandcode",  "commandcode %s",     false },
};
#define AGENT_CATALOG_COUNT ((i32)(sizeof AGENT_CATALOG / sizeof AGENT_CATALOG[0]))

/* =========================================================================
 * Cross-tool handoff
 *
 * Picks up a chat that started in one agent (e.g. Codex) and hands it off
 * to another (e.g. Claude). The source session is parsed via the existing
 * per-tool parsers, dumped as user/assistant markdown to
 * `~/.liu-handoffs/<source-id>.md`, and the target CLI is launched with a
 * single-line positional prompt that asks it to read the file and resume.
 *
 * Design borrows from cli-continues (yigitkonur/cli-continues) — same
 * markdown handoff idea, but implemented in C without the Node toolchain.
 * Reference-style (file path in prompt) is preferred over inline markdown
 * to side-step ARG_MAX and shell-escaping limits with large transcripts.
 * ========================================================================= */

typedef struct {
    char *buf;
    usize len;
    usize cap;
    u32   events;
} HandoffBuf;

#define HANDOFF_MAX_BYTES MB(8)

static void hb_appendf(HandoffBuf *hb, const char *fmt, ...) {
    if (!hb || hb->len >= HANDOFF_MAX_BYTES) return;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    char small[512];
    int n = vsnprintf(small, sizeof small, fmt, ap2);
    va_end(ap2);
    if (n <= 0) { va_end(ap); return; }

    usize need = hb->len + (usize)n + 1;
    if (need > hb->cap) {
        usize new_cap = hb->cap > 0 ? hb->cap : KB(16);
        while (new_cap < need) new_cap *= 2;
        if (new_cap > HANDOFF_MAX_BYTES) new_cap = HANDOFF_MAX_BYTES;
        if (new_cap < need) { va_end(ap); return; }  /* cap reached */
        char *grown = realloc(hb->buf, new_cap);
        if (!grown) { va_end(ap); return; }
        hb->buf = grown;
        hb->cap = new_cap;
    }
    if ((usize)n < sizeof small) {
        memcpy(hb->buf + hb->len, small, (usize)n);
        hb->len += (usize)n;
        hb->buf[hb->len] = '\0';
    } else {
        int n2 = vsnprintf(hb->buf + hb->len, hb->cap - hb->len, fmt, ap);
        if (n2 > 0) {
            hb->len += (usize)n2;
            if (hb->len >= hb->cap) hb->len = hb->cap - 1;
            hb->buf[hb->len] = '\0';
        }
    }
    va_end(ap);
}

static bool handoff_collect_cb(const ChatEvent *e, void *user) {
    HandoffBuf *hb = (HandoffBuf *)user;
    if (hb->len > HANDOFF_MAX_BYTES - KB(64)) return false;
    const char *role = NULL;
    switch (e->role) {
        case CHAT_ROLE_USER:      role = "User";      break;
        case CHAT_ROLE_ASSISTANT: role = "Assistant"; break;
        /* Tool-use and tool-result lines bloat the markdown without
         * adding much context for a fresh agent that has its own tool
         * surface anyway — drop them. */
        default: return true;
    }
    if (!e->text || !e->text[0]) return true;
    hb_appendf(hb, "## %s\n\n%s\n\n", role, e->text);
    hb->events++;
    return true;
}

/* Arena size for the Cline/Roo/Kilo whole-file parser. That parser slurps the
 * entire session JSON into the arena (capped at 64 MiB) AND then dups per-event
 * strings out of it, so the arena must hold the slurp PLUS the dups. A fixed
 * MB(64) equal to the slurp cap leaves no headroom and silently drops events on
 * large sessions. Size from the real file (×3 for slurp + dups), clamped; mmap
 * is demand-zero so the headroom costs virtual address space, not RSS. */
static usize cline_parse_arena_bytes(i64 size_bytes) {
    i64 slurp = size_bytes;
    if (slurp < 0) slurp = 0;
    if (slurp > (i64)MB(64)) slurp = (i64)MB(64);  /* matches hist_slurp_file cap */
    usize want = (usize)slurp * 3u + MB(4);
    if (want < MB(8))   want = MB(8);
    if (want > MB(256)) want = MB(256);
    return want;
}

/* Build `~/.liu-handoffs/<sid>.md` from a session id. Returns the absolute
 * path (in a static buffer reused across calls — copy before re-invoking)
 * on success, NULL on failure. */
static const char *cross_tool_handoff_write(const char *source_sid,
                                            u8 source_tool,
                                            const char *project_label) {
    static char out_path[512];
    if (!source_sid || !*source_sid) return NULL;
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;

    char dir[512];
    snprintf(dir, sizeof dir, "%s/.liu-handoffs", home);
    (void)mkdir(dir, 0700);   /* EEXIST is fine */
    snprintf(out_path, sizeof out_path, "%s/%s.md", dir, source_sid);

    Arena scan_a = arena_create(MB(16));
    ChatSessionMeta meta;
    bool found = chat_find_session(&scan_a, source_sid, &meta);
    if (!found) { arena_destroy(&scan_a); return NULL; }

    HandoffBuf hb = {0};
    hb_appendf(&hb, "# Handoff from %s\n\n", chat_tool_name((ChatTool)source_tool));
    if (project_label && *project_label) {
        hb_appendf(&hb, "Project: %s\n\n", project_label);
    }
    hb_appendf(&hb, "Source session: %s\n\n---\n\n", source_sid);

    bool whole_file = (meta.tool == CHAT_TOOL_CLINE ||
                       meta.tool == CHAT_TOOL_ROO   ||
                       meta.tool == CHAT_TOOL_KILO);
    Arena parse_a = arena_create(whole_file ? cline_parse_arena_bytes(meta.size_bytes) : MB(4));
    parser_run_file(&parse_a, meta.tool, meta.path, handoff_collect_cb, &hb);
    arena_destroy(&parse_a);
    arena_destroy(&scan_a);

    FILE *f = fopen(out_path, "w");
    if (!f) { free(hb.buf); return NULL; }
    if (hb.buf && hb.len > 0) fwrite(hb.buf, 1, hb.len, f);
    fclose(f);
    free(hb.buf);
    return hb.events > 0 ? out_path : NULL;
}

/* Same path construction as cross_tool_handoff_write, but pure — used at
 * picker-construction time to pre-bake the shell command before the file
 * actually exists on disk (execute path materialises it). */
static void cross_tool_handoff_path(const char *source_sid, char *out, usize cap) {
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(out, cap, "%s/.liu-handoffs/%s.md", home,
                 source_sid && *source_sid ? source_sid : "session");
    } else {
        snprintf(out, cap, "/tmp/.liu-handoffs/%s.md",
                 source_sid && *source_sid ? source_sid : "session");
    }
}

/* Probe every catalog binary and populate `agent_has[]`. One-shot per process
 * unless `force` is true — users refresh manually via ⌘R in the agent picker
 * (the alternative of probing on every palette/picker open adds ~250 stat()
 * calls per user action, so it's opt-in).
 *
 * The probe runs ~250 synchronous access() syscalls, so we kick it to a
 * background thread at launch and have foreground callers join it on demand.
 * A foreground `force=true` call (⌘R refresh) joins the prior worker first
 * and then re-runs synchronously, so the picker shows fresh results. */
#include <pthread.h>

static pthread_t   s_agent_detect_tid;
static bool        s_agent_detect_thread_live = false;
static pthread_mutex_t s_agent_detect_mtx = PTHREAD_MUTEX_INITIALIZER;

static void agent_detect_run(AppState *app) {
    bool staging[sizeof app->agent_has / sizeof app->agent_has[0]] = {0};
    for (i32 i = 0; i < AGENT_CATALOG_COUNT; i++) {
        const AgentCatalogEntry *e = &AGENT_CATALOG[i];
        if (e->tool < (i32)(sizeof staging / sizeof staging[0])) {
            staging[e->tool] = agent_binary_on_path(e->binary);
        }
    }
    pthread_mutex_lock(&s_agent_detect_mtx);
    memcpy(app->agent_has, staging, sizeof staging);
    app->agent_detect_done = true;
    pthread_mutex_unlock(&s_agent_detect_mtx);
}

static void *agent_detect_thread(void *arg) {
    agent_detect_run((AppState *)arg);
    return NULL;
}

static void app_detect_cli_agents_async(AppState *app) {
    pthread_mutex_lock(&s_agent_detect_mtx);
    if (s_agent_detect_thread_live) {
        pthread_mutex_unlock(&s_agent_detect_mtx);
        return;
    }
    s_agent_detect_thread_live = true;
    pthread_mutex_unlock(&s_agent_detect_mtx);
    if (pthread_create(&s_agent_detect_tid, NULL, agent_detect_thread, app) != 0) {
        /* Thread launch failed — fall back to synchronous probe. */
        s_agent_detect_thread_live = false;
        agent_detect_run(app);
    }
}

static void app_detect_cli_agents(AppState *app, bool force) {
    if (s_agent_detect_thread_live) {
        pthread_join(s_agent_detect_tid, NULL);
        s_agent_detect_thread_live = false;
    }
    if (app->agent_detect_done && !force) return;
    agent_detect_run(app);
}

static bool app_agent_has(const AppState *app, u8 tool) {
    if (tool >= (u8)(sizeof app->agent_has / sizeof app->agent_has[0])) return false;
    return app->agent_has[tool];
}

static void app_open_agent_picker(AppState *app, u8 session_tool,
                                  const char *session_id,
                                  const char *session_path,
                                  const char *project_label) {
    app_detect_cli_agents(app, false);

    snprintf(app->agent_picker_session_id, sizeof app->agent_picker_session_id,
             "%s", session_id ? session_id : "");
    snprintf(app->agent_picker_session_path, sizeof app->agent_picker_session_path,
             "%s", session_path ? session_path : "");
    app->agent_picker_session_tool = session_tool;
    snprintf(app->agent_picker_title, sizeof app->agent_picker_title,
             "%s", (project_label && *project_label) ? project_label : "session");

    /* Build rows: (1) native resume for the origin agent if supported,
     * (2) launch entries for every other installed agent, (3) transcript. */
    if (!app->agent_picker_rows) {
        app->agent_picker_rows = calloc(AGENT_PICKER_ROW_CAP,
                                        sizeof *app->agent_picker_rows);
        if (!app->agent_picker_rows) return;
    }
    app->agent_picker_row_count = 0;
    struct AgentPickerRow *rows = app->agent_picker_rows;
    const i32 cap = AGENT_PICKER_ROW_CAP;
    const bool have_sid  = (session_id   && session_id[0]   != '\0');
    const bool have_path = (session_path && session_path[0] != '\0');

    /* Row 1: origin-tool row — only if the binary is actually installed.
     * Prefer native resume when both the flag and the required input (id or
     * path) are available; otherwise show "Continue". Uninstalled agents are
     * hidden entirely; the user surfaces them via ⌘R after installation. */
    for (i32 i = 0; i < AGENT_CATALOG_COUNT && app->agent_picker_row_count < cap; i++) {
        const AgentCatalogEntry *e = &AGENT_CATALOG[i];
        if (e->tool != session_tool) continue;
        if (!app_agent_has(app, e->tool)) break;
        struct AgentPickerRow *rr = &rows[app->agent_picker_row_count++];
        const char *arg = e->resume_needs_path ? session_path : session_id;
        bool have_arg = e->resume_needs_path ? have_path : have_sid;
        bool fmt_consumes_arg = (e->resume_fmt && strstr(e->resume_fmt, "%s") != NULL);
        /* Never interpolate an id/path with shell metacharacters into the
         * resume command; fall back to a plain "Continue" (new_cmd) instead. */
        if (fmt_consumes_arg && have_arg && !agent_arg_is_safe(arg)) have_arg = false;
        if (e->resume_fmt && (!fmt_consumes_arg || have_arg)) {
            rr->kind = 0;
            rr->tool = e->tool;
            snprintf(rr->label, sizeof rr->label, "Resume with %s", e->display);
            if (fmt_consumes_arg) {
                snprintf(rr->command, sizeof rr->command, e->resume_fmt, arg ? arg : "");
            } else {
                snprintf(rr->command, sizeof rr->command, "%s", e->resume_fmt);
            }
        } else {
            rr->kind = 1;
            rr->tool = e->tool;
            snprintf(rr->label, sizeof rr->label, "Continue with %s", e->display);
            snprintf(rr->command, sizeof rr->command, "%s", e->new_cmd);
        }
        break;
    }

    /* Other installed agents. When the agent has a cross_tool_fmt AND we
     * have a usable source session-id, offer "Continue with X" (kind=3):
     * the source transcript is dumped to ~/.liu-handoffs/<sid>.md at
     * execute time and the target CLI is launched with a single positional
     * arg telling it to read the file and pick up. Otherwise fall back to
     * "Start new X session" (kind=1) which just runs the launcher. */
    for (i32 i = 0; i < AGENT_CATALOG_COUNT && app->agent_picker_row_count < cap; i++) {
        const AgentCatalogEntry *e = &AGENT_CATALOG[i];
        if (e->tool == session_tool) continue;
        if (!app_agent_has(app, e->tool)) continue;
        struct AgentPickerRow *rr = &rows[app->agent_picker_row_count++];
        if (e->cross_tool_fmt && have_sid && agent_arg_is_safe(session_id)) {
            rr->kind = 3;
            rr->tool = e->tool;
            snprintf(rr->label, sizeof rr->label, "Continue with %s", e->display);
            char handoff_path[256];
            cross_tool_handoff_path(session_id, handoff_path, sizeof handoff_path);
            char quoted[400];
            snprintf(quoted, sizeof quoted,
                     "'Read %s for the prior conversation history (formatted as User/Assistant markdown) and continue from where it left off.'",
                     handoff_path);
            snprintf(rr->command, sizeof rr->command, e->cross_tool_fmt, quoted);
        } else {
            rr->kind = 1;
            rr->tool = e->tool;
            snprintf(rr->label, sizeof rr->label, "Start new %s session", e->display);
            snprintf(rr->command, sizeof rr->command, "%s", e->new_cmd);
        }
    }

    /* Always: view transcript (works regardless of installed CLIs). */
    if (app->agent_picker_row_count < cap) {
        struct AgentPickerRow *rr = &rows[app->agent_picker_row_count++];
        rr->kind = 2; rr->tool = 0;
        snprintf(rr->label, sizeof rr->label, "View transcript");
        /* Only pass the id through when it is shell-safe (see agent_arg_is_safe). */
        if (agent_arg_is_safe(session_id))
            snprintf(rr->command, sizeof rr->command, "agenthistory show %s", session_id);
        else
            snprintf(rr->command, sizeof rr->command, "agenthistory show");
    }

    app->agent_picker_selected = 0;
    app->agent_picker_active   = true;
}

/* ---------------------------------------------------------------------------
 * Transcript viewer — parse the picked session file into heap-owned events.
 * ------------------------------------------------------------------------- */
static char *ui_dup(const char *s) {
    if (!s) return NULL;
    usize n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void app_transcript_free(AppState *app) {
    if (!app->transcript_events) return;
    for (i32 i = 0; i < app->transcript_count; i++) {
        free(app->transcript_events[i].text);
        free(app->transcript_events[i].tool_name);
    }
    free(app->transcript_events);
    app->transcript_events = NULL;
    app->transcript_count  = 0;
    app->transcript_cap    = 0;
}

static bool transcript_collect_cb(const ChatEvent *e, void *user) {
    AppState *app = user;
    /* Grow geometrically on demand instead of pre-reserving 4096 slots
     * upfront — most sessions have far fewer events and the upfront
     * 128 KB calloc was wasted RAM until the slots got populated. */
    if (app->transcript_count >= app->transcript_cap) {
        i32 new_cap = app->transcript_cap > 0 ? app->transcript_cap * 2 : 64;
        if (new_cap > 65536) new_cap = 65536;   /* hard ceiling */
        if (new_cap == app->transcript_cap) return false;
        void *grown = realloc(app->transcript_events,
                              (usize)new_cap * sizeof(*app->transcript_events));
        if (!grown) return false;
        app->transcript_events = grown;
        /* Zero the newly grown tail so the renderer iterating up to
         * transcript_count never reads stale slot data after a future
         * partial fill. */
        memset(&app->transcript_events[app->transcript_cap], 0,
               (usize)(new_cap - app->transcript_cap) * sizeof(*app->transcript_events));
        app->transcript_cap = new_cap;
    }
    struct TranscriptEvent *ev = &app->transcript_events[app->transcript_count++];
    ev->role      = (u8)e->role;
    ev->ts_ms     = e->timestamp_ms;
    ev->text      = ui_dup(e->text);
    ev->tool_name = ui_dup(e->tool_name);
    return true;
}

/* File-browser auto-follows the active tab's local-PTY cwd. Called once per
 * main-loop tick from the session-poll path so updates ride alongside the
 * shell output that triggered them.
 *
 * Resolution order:
 *   1. Terminal->cwd  — populated by OSC 7 if the shell emits it.
 *   2. session_local_cwd(session)  — proc_pidinfo / readlink fallback for
 *      shells without OSC 7 (works on any unmodified bash/zsh/fish).
 *
 * Guards:
 *   - Only acts when the file browser is open and viewing a *local* source.
 *   - Only navigates on actual cwd transitions (the path differs from the
 *     last synced one OR the active tab changed) so a user's manual fb
 *     navigation isn't clobbered every frame.
 *   - Poll fallback runs at ~20 Hz; an OSC 7 arrival (cwd_seq bump) bypasses
 *     the limiter entirely so shells that emit it see the browser follow a
 *     `cd` on the same frame the prompt comes back. */
static void app_sync_filebrowser_cwd(AppState *app, f64 now_sec) {
    if (!app || !app->filebrowser.open) return;
    /* Skip when the browser is pointed at a remote SFTP source — terminal
     * cwd has no meaning over there. */
    if (app->filebrowser.source != FB_SOURCE_LOCAL) return;

    Tab *tab = app_active_tab(app);
    if (!tab) return;
    /* Follow the FOCUSED pane, not unconditionally pane 0. */
    Terminal *term = (tab->active_pane == 1 && tab->terminal2)
                   ? tab->terminal2 : tab->terminal;
    Session  *sess = (tab->active_pane == 1 && tab->session2)
                   ? tab->session2 : tab->session;
    if (!term || !sess) return;

    /* Push path: a new OSC 7 on the focused terminal (or a focus move to a
     * different terminal) bypasses the rate limiter so the browser follows a
     * `cd` the same frame the prompt returns. The 20 Hz poll below remains
     * the backstop for shells that never emit OSC 7. */
    bool pushed = (term != app->fb_sync_last_term) ||
                  (term->cwd_seq != app->fb_sync_last_cwd_seq);
    if (!pushed && now_sec - app->fb_sync_last_check_ts < 0.05) return;
    app->fb_sync_last_check_ts = now_sec;

    /* Only follow local PTY tabs (SSH/Mosh sessions live on a remote host;
     * cwd there isn't meaningful for the local fb). Seq still consumed at
     * the tail so a remote OSC 7 can't keep the bypass armed. */
    const char *cwd = NULL;
    if (session_type(sess) == SESSION_LOCAL) {
        if (term->cwd && term->cwd[0]) cwd = term->cwd;
        if (!cwd || !cwd[0]) {
            const char *probe = session_local_cwd(sess);
            if (probe && probe[0]) cwd = probe;
        }
    }

    if (cwd && cwd[0]) {
        bool tab_changed  = app->fb_sync_last_tab != app->active_tab;
        bool path_changed = strcmp(app->fb_sync_last_cwd, cwd) != 0;
        if (tab_changed || path_changed) {
            /* Avoid re-navigating to a path the user is already viewing —
             * protects against an unnecessary directory re-read when nothing
             * observable changed. */
            if (strcmp(app->filebrowser.cwd, cwd) != 0) {
                fb_navigate(&app->filebrowser, cwd);
            }
            snprintf(app->fb_sync_last_cwd, sizeof app->fb_sync_last_cwd,
                     "%s", cwd);
            app->fb_sync_last_tab = app->active_tab;
        }
    }

    /* Single tail: every exit from here on has consumed the push signal. */
    app->fb_sync_last_term    = term;
    app->fb_sync_last_cwd_seq = term->cwd_seq;
}

/* Open the transcript viewer for a session identified by session_id. Uses
 * chat_find_session() to locate the on-disk JSONL file, then streams it with
 * parser_run_file() while duplicating each event into heap storage. */
static void app_open_transcript_viewer(AppState *app, const char *session_id,
                                       const char *display_title) {
    if (!session_id || !*session_id) return;

    /* Resolve path + tool. The scan arena owns meta's string slices, so we
     * must keep it alive until we are done dereferencing meta.{path,session_id,...}.
     * Sized to MB(4) so the full set of agent scanners can each push their
     * meta entries — the old KB(64) cap silently truncated everything after
     * Claude+Codex exhausted the bump pointer, making transcript viewer
     * "session not found" for any qwen/opencode/cline-family session id. */
    Arena scan_a = arena_create(MB(16));
    ChatSessionMeta meta;
    bool found = chat_find_session(&scan_a, session_id, &meta);
    if (!found) {
        arena_destroy(&scan_a);
        app_show_toast(app, "Session file not found");
        return;
    }

    /* Reset any previous viewer state. The events array grows on demand
     * inside transcript_collect_cb; start empty so a tiny session doesn't
     * pay 4096 slots × ~32 B = ~128 KB upfront. */
    app_transcript_free(app);
    app->transcript_cap    = 0;
    app->transcript_events = NULL;
    app->transcript_count  = 0;
    app->transcript_scroll = 0;
    app->transcript_tool   = (u8)meta.tool;
    snprintf(app->transcript_title, sizeof app->transcript_title,
             "%s", display_title ? display_title : (meta.session_id ? meta.session_id : ""));

    /* JSONL parsers reset the arena after every line, so they only need one
     * line's worth of working memory. The cline-family whole-file parser must
     * keep the *previous* pending event alive for streaming dedup, so it can't
     * reset mid-parse — size the arena for the worst-case ui_messages.json
     * instead. */
    bool whole_file = (meta.tool == CHAT_TOOL_CLINE ||
                       meta.tool == CHAT_TOOL_ROO   ||
                       meta.tool == CHAT_TOOL_KILO);
    Arena parse_a = arena_create(whole_file ? cline_parse_arena_bytes(meta.size_bytes) : MB(4));
    parser_run_file(&parse_a, meta.tool, meta.path, transcript_collect_cb, app);
    arena_destroy(&parse_a);
    arena_destroy(&scan_a);

    app->transcript_viewer_active = true;
}

static void app_agent_picker_execute(AppState *app, i32 row_idx) {
    if (row_idx < 0 || row_idx >= app->agent_picker_row_count
        || !app->agent_picker_rows) {
        app->agent_picker_active = false;
        return;
    }
    const struct AgentPickerRow *rr = &app->agent_picker_rows[row_idx];

    /* "View transcript" → in-app modal, no shell paste. */
    if (rr->kind == 2) {
        char sid[96];
        snprintf(sid, sizeof sid, "%s", app->agent_picker_session_id);
        char title[160];
        snprintf(title, sizeof title, "%s", app->agent_picker_title);
        app->agent_picker_active = false;
        app_open_transcript_viewer(app, sid, title);
        return;
    }

    /* Cross-tool handoff: materialise ~/.liu-handoffs/<sid>.md from the
     * source session BEFORE the shell command (already pre-baked into
     * rr->command at picker construction) gets pasted. If the dump
     * yields no events (corrupt file, parser bail, etc.) we still send
     * the command — the target CLI will then see an empty/missing file
     * which is recoverable by the user re-typing; better than silently
     * doing nothing. */
    if (rr->kind == 3) {
        const char *p = cross_tool_handoff_write(app->agent_picker_session_id,
                                                 app->agent_picker_session_tool,
                                                 app->agent_picker_title);
        if (!p) {
            app_show_toast(app, "Handoff dump failed — running launcher without context");
        }
    }

    /* Resume / start new session: run the command in the *focused* terminal.
     * If that terminal is currently inside another agent CLI (the common
     * case — e.g., the user picked a Resume earlier and is now switching to
     * a different chat), pasting straight in would feed bytes to the
     * running agent's stdin instead of executing as a shell command. Fix:
     * SIGTERM the foreground process group, then defer the paste until the
     * shell is back in front (polled in the main-loop tick).
     *
     * When no tab/session is focused (zero-tab window), spawn a fresh
     * local PTY and use that as the target. */
    if (!rr->command[0]) {
        app->agent_picker_active = false;
        return;
    }

    Terminal *tm = app_focused_terminal(app);
    Session  *se = app_focused_session(app);
    if (!tm || !se) {
        Session *s = session_create_local(app->grid_cols, app->grid_rows);
        if (s && session_status(s) == SESSION_CONNECTED) {
            const char *disp = "Terminal";
            for (i32 i = 0; i < AGENT_CATALOG_COUNT; i++) {
                if (AGENT_CATALOG[i].tool == rr->tool) {
                    disp = AGENT_CATALOG[i].display;
                    break;
                }
            }
            i32 idx = app_new_tab(app, s, disp);
            if (idx >= 0) {
                tm = app->tabs[idx].terminal;
                se = s;
            } else {
                session_destroy(s);
            }
        } else if (s) {
            session_destroy(s);
        }
    }

    if (!tm || !se) {
        app->agent_picker_active = false;
        return;
    }

    /* If the focused PTY has an agent/REPL in foreground, SIGTERM it and
     * queue the paste; the main-loop tick fires it once the shell takes
     * the FG pgrp back. Otherwise send straight away. SSH/Mosh sessions
     * skip the kill dance — we can't signal remote-side processes from
     * here, and the local shell is what we'd need to be in front of. */
    if (!session_fg_is_shell(se)) {
        session_kill_fg(se, SIGTERM);
        app->pending_agent_session = se;
        snprintf(app->pending_agent_command,
                 sizeof app->pending_agent_command, "%s", rr->command);
        app->pending_agent_deadline_sec = platform_time_sec() + 1.5;
        app->pending_agent_active = true;
    } else {
        app_send_command_and_execute(app, tm, se, rr->command);
    }
    app->agent_picker_active = false;
}

/* ---- Markdown quick switcher (Open Note) ---------------------------------
 * A recursive *.md enumeration of the vault root (the active note's folder, or
 * the focused terminal's cwd). Paths + display names are stashed in static
 * tables; the palette item id indexes them so activation can open the file. */
#define APP_MD_FILES_MAX 2048
/* Must match palette.c's MAX_PALETTE_ITEMS — the most rows the palette holds. */
#define APP_MD_SWITCHER_SHOW 256
static char (*g_md_file_paths)[FB_MAX_PATH];   /* lazily heap-allocated */
static char (*g_md_file_names)[192];
static u32   g_md_file_count;

static void app_scan_md_files(const char *dir, int depth) {
    if (depth > 8 || g_md_file_count >= APP_MD_FILES_MAX) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g_md_file_count < APP_MD_FILES_MAX) {
        if (de->d_name[0] == '.') continue;                 /* hidden, ., .. */
        if (strcmp(de->d_name, "node_modules") == 0) continue;
        char full[FB_MAX_PATH];
        int wn = snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        if (wn <= 0 || wn >= (int)sizeof full) continue;
        bool is_dir;
#ifdef DT_DIR
        if (de->d_type != DT_UNKNOWN) is_dir = (de->d_type == DT_DIR);
        else
#endif
        { struct stat st; if (stat(full, &st) != 0) continue; is_dir = S_ISDIR(st.st_mode); }
        if (is_dir) {
            app_scan_md_files(full, depth + 1);
        } else {
            const char *dot = strrchr(de->d_name, '.');
            if (dot && (strcasecmp(dot, ".md") == 0 || strcasecmp(dot, ".markdown") == 0)) {
                snprintf(g_md_file_paths[g_md_file_count], FB_MAX_PATH, "%s", full);
                snprintf(g_md_file_names[g_md_file_count], 192, "%.*s",
                         (int)(dot - de->d_name), de->d_name);
                g_md_file_count++;
            }
        }
    }
    closedir(d);
}

/* Recursively collect subfolders of `dir` (relative paths in g_md_file_names,
 * absolute in g_md_file_paths) for the graph folder-filter picker. Skips hidden
 * and package-dependency directories. `rootlen` is the byte length of the scan
 * root so names can be stored relative to it. */
static void app_scan_folders(const char *dir, int depth, usize rootlen) {
    if (depth > 6 || g_md_file_count >= APP_MD_FILES_MAX) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g_md_file_count < APP_MD_FILES_MAX) {
        if (de->d_name[0] == '.') continue;                 /* hidden, ., .. */
        if (strcmp(de->d_name, "node_modules") == 0 ||
            strcmp(de->d_name, "bower_components") == 0 ||
            strcmp(de->d_name, "Pods") == 0 ||
            strcmp(de->d_name, "__pycache__") == 0) continue;
        char full[FB_MAX_PATH];
        int wn = snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        if (wn <= 0 || wn >= (int)sizeof full) continue;
        bool is_dir;
#ifdef DT_DIR
        if (de->d_type != DT_UNKNOWN) is_dir = (de->d_type == DT_DIR);
        else
#endif
        { struct stat st; if (stat(full, &st) != 0) continue; is_dir = S_ISDIR(st.st_mode); }
        if (!is_dir) continue;
        snprintf(g_md_file_paths[g_md_file_count], FB_MAX_PATH, "%s", full);
        const char *rel = full;
        if (rootlen && strlen(full) > rootlen) { rel = full + rootlen; while (*rel == '/') rel++; }
        snprintf(g_md_file_names[g_md_file_count], 192, "%s", rel);
        g_md_file_count++;
        app_scan_folders(full, depth + 1, rootlen);
    }
    closedir(d);
}

/* mkdir -p (each component, ignoring EEXIST). */
static void app_mkdir_p(const char *path) {
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", path);
    usize n = strlen(tmp);
    for (usize i = 1; i < n; i++)
        if (tmp[i] == '/') { tmp[i] = '\0'; mkdir(tmp, 0755); tmp[i] = '/'; }
    mkdir(tmp, 0755);
}

/* Resolve the configured notes-Vault root, computing a default when unset.
 * Default lives in the app-support dir — TCC-safe (no Photos/Music/Documents
 * prompt) and always writable. Never returns NULL. */
static const char *app_notes_vault_path(AppState *app) {
    static char vault[1024];
    const char *cfg = app->config.notes_vault_path;
    if (cfg && cfg[0]) { snprintf(vault, sizeof vault, "%s", cfg); }
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) home = "/tmp";
        snprintf(vault, sizeof vault, "%s/Library/Application Support/Liu/Vault", home);
    }
    return vault;
}

/* Create the Vault dir (mkdir -p) and seed a starter note once. Lazy — called
 * from the open/graph paths, not eagerly at startup. */
static void app_notes_vault_ensure(AppState *app) {
    const char *root = app_notes_vault_path(app);
    app_mkdir_p(root);
    char welcome[1300];
    snprintf(welcome, sizeof welcome, "%s/Welcome.md", root);
    if (access(welcome, F_OK) != 0) {
        FILE *f = fopen(welcome, "w");
        if (f) {
            fputs("# Liu Vault\n\nYour notes, docs and AI-generated plans live here.\n\n"
                  "- Create a note from a prompt: Cmd+K \xe2\x86\x92 \"Create Note\".\n"
                  "- Agents you run in the terminal inherit $LIU_VAULT (this folder) "
                  "and $LIU_VAULT_HINT (a ready directive) \xe2\x80\x94 point them here for "
                  "standalone notes/plans.\n", f);
            fclose(f);
        }
    }
}

/* Export the notes-vault convention into THIS process's environment so every
 * local PTY — and any AI agent the user runs inside one — inherits it (children
 * inherit environ across fork/exec). Two variables:
 *   LIU_VAULT       absolute vault path
 *   LIU_VAULT_HINT  a ready-to-use, agent-agnostic directive string
 * Universal + per-user, never hardcoded: the path resolves from this user's
 * config/HOME. The hint is deliberately scoped to standalone notes/plans (NOT
 * project files) so it doesn't wrongly redirect an agent's in-project docs. */
static void app_export_vault_env(AppState *app) {
    const char *vault = app_notes_vault_path(app);
    setenv("LIU_VAULT", vault, 1);
    char hint[1400];
    snprintf(hint, sizeof hint,
        "Liu notes vault is at %s (also $LIU_VAULT). When you generate "
        "standalone notes, plans, or documentation that are NOT part of the "
        "current project's repo, save them there as Markdown.", vault);
    setenv("LIU_VAULT_HINT", hint, 1);
}

/* Resolve the vault root for the switcher / search / graph. Scopes to the open
 * note's own folder when one is open (contextual backlinks), otherwise the
 * configured notes Vault — no longer the terminal cwd / $HOME, so the graph and
 * note commands never scan arbitrary (TCC-protected) locations. */
static const char *app_md_vault_root(AppState *app) {
    FileBrowser *fb = NULL;
    Tab *t = app_active_tab(app);
    if (t && t->kind == TAB_FILEBROWSER && t->fb &&
        t->fb->view_mode == FVIEW_MARKDOWN) fb = t->fb;
    else if (app->filebrowser.view_mode == FVIEW_MARKDOWN) fb = &app->filebrowser;
    if (fb && fb->md_doc && fb->md_doc->base_dir && fb->md_doc->base_dir[0])
        return fb->md_doc->base_dir;
    app_notes_vault_ensure(app);
    return app_notes_vault_path(app);
}

/* ---- Wikilink / tag autocomplete (markdown editor) ---------------------- */
static bool ac_ci_substr(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    usize nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        usize k = 0;
        while (k < nl && p[k] &&
               tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == nl) return true;
    }
    return false;
}
static void ac_add(FileBrowser *fb, const char *s) {
    if (fb->ac_count >= 64) return;          /* visible-candidate cap */
    if (fb->ac_count >= fb->ac_cap) {
        u32 nc = fb->ac_cap ? fb->ac_cap * 2 : 32;
        char (*ni)[160] = realloc(fb->ac_items, (usize)nc * 160);
        if (!ni) return;
        fb->ac_items = ni; fb->ac_cap = nc;
    }
    snprintf(fb->ac_items[fb->ac_count], 160, "%s", s);
    fb->ac_count++;
}
static void app_ac_ensure_vault(AppState *app) {
    if (g_md_file_count > 0) return;
    if (!g_md_file_paths) g_md_file_paths = calloc(APP_MD_FILES_MAX, FB_MAX_PATH);
    if (!g_md_file_names) g_md_file_names = calloc(APP_MD_FILES_MAX, 192);
    g_md_file_count = 0;
    const char *root = (g_md_file_paths && g_md_file_names) ? app_md_vault_root(app) : NULL;
    if (root) app_scan_md_files(root, 0);
}
static void app_ac_populate_wiki(AppState *app, FileBrowser *fb) {
    app_ac_ensure_vault(app);
    fb->ac_count = 0;
    for (u32 i = 0; i < g_md_file_count && fb->ac_count < 64; i++)
        if (ac_ci_substr(g_md_file_names[i], fb->ac_query)) ac_add(fb, g_md_file_names[i]);
}
static void app_ac_populate_tags(FileBrowser *fb) {
    fb->ac_count = 0;
    const char *s = fb->view_content;
    if (!s) return;
    usize n = fb->view_size;
    for (usize i = 0; i < n; i++) {
        if (s[i] == '#' && (i == 0 || s[i-1] == ' ' || s[i-1] == '\t' || s[i-1] == '\n')) {
            usize j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j]=='_' || s[j]=='-' || s[j]=='/')) j++;
            if (j > i + 1) {
                char tag[160]; usize tl = j - (i + 1);
                if (tl >= sizeof tag) tl = sizeof tag - 1;
                memcpy(tag, s + i + 1, tl); tag[tl] = 0;
                bool numeric = true;
                for (char *c = tag; *c; c++) if (!isdigit((unsigned char)*c)) { numeric = false; break; }
                if (!numeric && ac_ci_substr(tag, fb->ac_query)) {
                    bool dup = false;
                    for (u32 k = 0; k < fb->ac_count; k++)
                        if (strcmp(fb->ac_items[k], tag) == 0) { dup = true; break; }
                    if (!dup) ac_add(fb, tag);
                }
            }
            i = j;
        }
    }
}
static void app_ac_dismiss(FileBrowser *fb) { if (fb) fb->ac_active = false; }

/* Re-evaluate the autocomplete trigger from the cursor context. */
static void app_ac_update(AppState *app, FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) { if (fb) fb->ac_active = false; return; }
    usize off = fb_editor_cursor_offset(fb);
    const char *s = fb->view_content;

    /* wikilink: nearest "[[" before the cursor on this line, not yet closed */
    isize i = (isize)off - 1; bool wiki = false; usize anchor = 0;
    for (; i >= 0; i--) {
        char ch = s[i];
        if (ch == '\n' || ch == ']') break;
        if (ch == '[' && i > 0 && s[i-1] == '[') { anchor = (usize)i + 1; wiki = true; break; }
    }
    if (wiki && off >= anchor && (off - anchor) < sizeof fb->ac_query) {
        bool ok = true;
        for (usize k = anchor; k < off; k++) { char ch = s[k]; if (ch=='['||ch==']'||ch=='\n') { ok = false; break; } }
        if (ok) {
            fb->ac_kind = 0; fb->ac_anchor = anchor;
            memcpy(fb->ac_query, s + anchor, off - anchor); fb->ac_query[off - anchor] = 0;
            app_ac_populate_wiki(app, fb);
            if (fb->ac_count > 0) { if (fb->ac_sel < 0 || (u32)fb->ac_sel >= fb->ac_count) fb->ac_sel = 0; fb->ac_active = true; return; }
        }
    }

    /* tag: '#' at a boundary, tag chars up to the cursor */
    i = (isize)off - 1; usize tstart = off;
    while (i >= 0) {
        char ch = s[i];
        if (isalnum((unsigned char)ch) || ch=='_' || ch=='-' || ch=='/') { tstart = (usize)i; i--; }
        else break;
    }
    if (i >= 0 && s[i] == '#') {
        bool boundary = (i == 0) || s[i-1]==' ' || s[i-1]=='\t' || s[i-1]=='\n' || s[i-1]=='(' || s[i-1]=='[';
        usize anchor2 = (usize)i + 1;
        if (boundary && tstart >= anchor2 && (off - anchor2) < sizeof fb->ac_query) {
            fb->ac_kind = 1; fb->ac_anchor = anchor2;
            memcpy(fb->ac_query, s + anchor2, off - anchor2); fb->ac_query[off - anchor2] = 0;
            app_ac_populate_tags(fb);
            if (fb->ac_count > 0) { if (fb->ac_sel < 0 || (u32)fb->ac_sel >= fb->ac_count) fb->ac_sel = 0; fb->ac_active = true; return; }
        }
    }
    fb->ac_active = false;
}

/* Insert the selected candidate, replacing the typed query. */
static void app_ac_accept(FileBrowser *fb) {
    if (!fb->ac_active || fb->ac_sel < 0 || (u32)fb->ac_sel >= fb->ac_count) { app_ac_dismiss(fb); return; }
    const char *item = fb->ac_items[fb->ac_sel];
    usize off = fb_editor_cursor_offset(fb);
    if (off < fb->ac_anchor || off > fb->view_size) { app_ac_dismiss(fb); return; }
    usize oldlen = off - fb->ac_anchor;
    /* Re-validate the replaced range: only the typed query (no newline, and for
     * wikilinks no bracket) may sit between the anchor and the cursor. Guards
     * against a stale anchor after an untracked edit splicing real content. */
    for (usize k = fb->ac_anchor; k < off; k++) {
        char qc = fb->view_content[k];
        if (qc == '\n' || (fb->ac_kind == 0 && (qc == '[' || qc == ']'))) {
            app_ac_dismiss(fb);
            return;
        }
    }
    char buf[200];
    int bn = (fb->ac_kind == 0) ? snprintf(buf, sizeof buf, "%s]]", item)
                                : snprintf(buf, sizeof buf, "%s", item);
    if (bn < 0) { app_ac_dismiss(fb); return; }
    fb_editor_push_undo(fb);
    if (!fb_editor_splice(fb, fb->ac_anchor, oldlen, buf, (usize)bn)) { app_ac_dismiss(fb); return; }
    fb_editor_push_undo(fb);
    /* place the cursor after the insertion (no newlines inserted) */
    usize newoff = fb->ac_anchor + (usize)bn;
    i32 line = 0; usize ls = 0;
    for (usize k = 0; k < newoff && k < fb->view_size; k++)
        if (fb->view_content[k] == '\n') { line++; ls = k + 1; }
    fb->cursor_line = line; fb->cursor_col = (i32)(newoff - ls);
    app_ac_dismiss(fb);
}

/* Case-insensitive substring search (no locale, ASCII fold) — used by vault
 * search so we don't depend on the non-portable strcasestr. */
static const char *app_ci_find(const char *hay, const char *needle) {
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return hay;
    }
    return NULL;
}

/* Vault content search: grep the pre-scanned *.md files for `query`, emitting
 * one row per file whose first matching line is shown as the detail. Snippets
 * are copied into a static table so they outlive the per-file read buffer. */
static void app_vault_search(const char *query) {
    if (!query || strlen(query) < 2 || !g_md_file_paths || !g_md_file_names) {
        palette_set_picker_items(7, NULL, 0);
        return;
    }
    static char snippets[APP_MD_SWITCHER_SHOW][128];
    static PaletteListEntry entries[APP_MD_SWITCHER_SHOW];
    static char readbuf[65536];
    u32 n = 0;
    for (u32 fi = 0; fi < g_md_file_count && n < APP_MD_SWITCHER_SHOW; fi++) {
        FILE *f = fopen(g_md_file_paths[fi], "rb");
        if (!f) continue;
        size_t rd = fread(readbuf, 1, sizeof readbuf - 1, f);
        fclose(f);
        readbuf[rd] = '\0';
        const char *hit = app_ci_find(readbuf, query);
        if (!hit) continue;
        const char *ls = hit; while (ls > readbuf && ls[-1] != '\n') ls--;
        const char *le = hit; while (*le && *le != '\n') le++;
        while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '>' || *ls == '#')) ls++;
        usize linelen = (usize)(le - ls);
        if (linelen > sizeof snippets[0] - 1) linelen = sizeof snippets[0] - 1;
        memcpy(snippets[n], ls, linelen);
        snippets[n][linelen] = '\0';
        entries[n].name       = g_md_file_names[fi];
        entries[n].name_len   = (u32)strlen(g_md_file_names[fi]);
        entries[n].detail     = snippets[n];
        entries[n].detail_len = (u32)linelen;
        entries[n].id         = (i32)fi;
        n++;
    }
    palette_set_picker_items(7, entries, n);
}

/* The markdown viewer currently on screen, if any: a full-window file-browser
 * tab takes precedence over the sidebar file browser. NULL when neither shows
 * a rendered (non-raw) markdown doc. */
static FileBrowser *app_active_md_fb(AppState *app) {
    Tab *t = app_active_tab(app);
    if (t && t->kind == TAB_FILEBROWSER && t->fb &&
        t->fb->view_mode == FVIEW_MARKDOWN && !t->fb->md_raw_mode)
        return t->fb;
    if (app->filebrowser.view_mode == FVIEW_MARKDOWN && !app->filebrowser.md_raw_mode)
        return &app->filebrowser;
    return NULL;
}

/* The FileBrowser currently in text-editor mode (code/text/markdown-edit),
 * whether that's a full-window file-browser tab or the docked sidebar. NULL
 * when nothing is being edited. Editor key/char input routes through this so
 * editing works in both surfaces. */
static FileBrowser *app_editing_fb(AppState *app) {
    Tab *t = app_active_tab(app);
    if (t && t->kind == TAB_FILEBROWSER && t->fb &&
        t->fb->editor_mode && t->fb->view_mode != FVIEW_NONE)
        return t->fb;
    if (app->filebrowser.editor_mode && app->filebrowser.view_mode != FVIEW_NONE)
        return &app->filebrowser;
    return NULL;
}

/* The FileBrowser eligible for in-document Find/Replace: a code/markdown editor
 * if one is active, otherwise a rendered markdown viewer. */
static FileBrowser *app_find_fb(AppState *app) {
    FileBrowser *fb = app_editing_fb(app);
    if (fb) return fb;
    return app_active_md_fb(app);
}

/* "New Window": launch a second, fully independent Liu instance. The menu's
 * NEW_WINDOW falls back to a new tab; this one is the real thing — its own
 * process, its own window, its own session tree. The child is detached
 * (setsid) so closing the parent doesn't take it down. */
static void app_spawn_new_window(AppState *app) {
    char exe[1024];
#if defined(__APPLE__)
    u32 cap = (u32)sizeof exe;
    if (_NSGetExecutablePath(exe, &cap) != 0) {
        app_show_toast(app, "Could not open a new window");
        return;
    }
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) {
        app_show_toast(app, "Could not open a new window");
        return;
    }
    exe[n] = '\0';
#endif
    /* Double-fork so the new instance is reparented to init and the
     * intermediate child is reaped HERE — no zombie per spawned window.
     * Child side runs only async-signal-safe calls (fork/setsid/execl). */
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execl(exe, exe, (char *)NULL);
        }
        _exit(0);
    }
    if (pid < 0) {
        app_show_toast(app, "Could not open a new window");
    } else {
        int st;
        (void)waitpid(pid, &st, 0);   /* immediate: child exits instantly */
    }
}

/* app_active_graph_root() lives in ui.c (shared with render_sidebar's
 * auto-open); declared in ui.h. */

/* Reveal the docked sidebar. If a Vault graph is on screen, root the sidebar's
 * file browser at that same Vault so the panel and the graph agree on what
 * you're looking at; otherwise just show the sidebar with its existing content. */
static void app_open_sidebar_for_context(AppState *app) {
    app->sidebar_visible = true;
    app->sidebar_width   = SIDEBAR_DEFAULT_PT * app->dpi_scale;
    const char *groot = app_active_graph_root(app);
    if (groot) {
        fb_navigate(&app->filebrowser, groot);
        app->filebrowser.open = true;
    }
}

static void app_execute_palette_command(AppState *app, PlatformWindow *window, const PaletteItem *item) {
    if (!item) return;

    /* Markdown outline heading: scroll the remembered viewer to it and close.
     * Handled before MRU so heading labels don't pollute the command MRU. */
    if (item->type == 5) {
        if (app->outline_fb) fb_md_scroll_to_heading(app->outline_fb, (u32)item->id);
        app->outline_fb = NULL;
        app->palette_active = false;
        return;
    }

    /* Quick-switcher / vault-search note: open the selected .md file in the
     * active markdown viewer, or a new file-browser tab if none is open. */
    if (item->type == 6 || item->type == 7) {
        if (item->id >= 0 && (u32)item->id < g_md_file_count && g_md_file_paths) {
            FileBrowser *fb = app_active_md_fb(app);
            if (fb) fb_open_md_path(fb, g_md_file_paths[item->id]);
            else    app_open_markdown_viewer_tab(app, g_md_file_paths[item->id]);
        }
        app->palette_active = false;
        return;
    }

    /* Graph folder filter: scope the active file-browser tab's graph to the
     * chosen folder (entering graph mode if not already). */
    if (item->type == 8) {
        if (item->id >= 0 && (u32)item->id < g_md_file_count && g_md_file_paths) {
            Tab *t = app_active_tab(app);
            FileBrowser *fb = (t && t->kind == TAB_FILEBROWSER && t->fb) ? t->fb
                            : (app->filebrowser.open ? &app->filebrowser : NULL);
            if (fb) {
                fb_navigate(fb, g_md_file_paths[item->id]);   /* sync the list */
                fb_graph_rescope(fb, g_md_file_paths[item->id]);
            }
        }
        app->palette_active = false;
        return;
    }

    /* Bump MRU before branching so every activation counts. */
    palette_mru_record(item->text);

    /* Agent-history session: do NOT paste directly any more. Close the palette
     * and open the resume-picker modal so the user chooses which CLI agent
     * should continue the session. */
    /* Site-launcher row: toggle the site, then re-enter the mode so the
     * list refreshes in place (status dot flips) and the palette stays
     * open for launching several sites back to back. */
    if (item->type == 8) {
        bool restart_mode = palette_sites_restart_mode();
        Site *st = sites_get(&app->site_mgr, item->id);
        if (st) {
            char msg[128];
            if (restart_mode) {
                if (site_restart(&app->site_mgr, st))
                    snprintf(msg, sizeof msg, "Restarting site: %s", st->name);
                else
                    snprintf(msg, sizeof msg, "Could not restart site: %s", st->name);
            } else if (st->status == SITE_RUNNING || st->status == SITE_STARTING) {
                site_stop(st);
                snprintf(msg, sizeof msg, "Stopping site: %s", st->name);
            } else if (site_start(&app->site_mgr, st)) {
                snprintf(msg, sizeof msg, "Starting site: %s", st->name);
            } else {
                snprintf(msg, sizeof msg, "Could not start site: %s", st->name);
            }
            app_show_toast(app, msg);
        }
        if (restart_mode) palette_enter_sites_restart_mode(&app->site_mgr);
        else              palette_enter_sites_mode(&app->site_mgr);
        app->palette_selected  = item->id < app->site_mgr.count ? item->id : 0;
        app->palette_scroll    = 0;
        app->palette_query[0]  = '\0';
        app->palette_query_len = 0;
        return;
    }

    if (item->type == 4) {
        char title[96];
        snprintf(title, sizeof title, "%s", item->text[0] ? item->text : "session");
        u8 tool = item->tool;
        char sid[96];
        char sp[512];
        /* session_id + full path live in the palette's lazy side tables
         * keyed by id. Path is required by codex's `experimental_resume`. */
        snprintf(sid, sizeof sid, "%s", palette_history_session_id(item->id));
        snprintf(sp,  sizeof sp,  "%s", palette_history_session_path(item->id));
        app->palette_active = false;
        app_open_agent_picker(app, tool, sid, sp, title);
        return;
    }

    if (item->type == 0) {
        app_open_host_from_palette(app, item);
        app->palette_active = false;
        return;
    }

    /* History entry: fill SSH dialog with the connection details */
    if (item->type == 3) {
        app_open_history_from_palette(app, item);
        app->palette_active = false;
        return;
    }

    if (item->type == 1) {
        app_execute_snippet_from_palette(app, item);
        app->palette_active = false;
        return;
    }

    if (item->type != 2) {
        app->palette_active = false;
        return;
    }

    /* "Go to Heading" pushes the palette into outline mode for the markdown
     * viewer currently on screen. Stays open; selecting a heading scrolls. */
    if (strcmp(item->text, "Go to Heading") == 0) {
        FileBrowser *fb = app_active_md_fb(app);
        u32 n = fb ? fb_md_outline_count(fb) : 0;
        if (fb && n > 0) {
            static PaletteOutlineEntry entries[FB_MD_OUTLINE_CAP];
            if (n > FB_MD_OUTLINE_CAP) n = FB_MD_OUTLINE_CAP;
            /* The palette can only hold APP_MD_SWITCHER_SHOW rows; surface the
             * truncation rather than silently dropping later headings. */
            if (n > APP_MD_SWITCHER_SHOW) {
                fprintf(stderr, "outline: %u headings, showing first %d\n",
                        n, APP_MD_SWITCHER_SHOW);
                n = APP_MD_SWITCHER_SHOW;
            }
            for (u32 i = 0; i < n; i++) {
                entries[i].text     = (const char *)fb->md_outline[i].text;
                entries[i].text_len = fb->md_outline[i].text_len;
                entries[i].level    = fb->md_outline[i].level;
            }
            app->outline_fb = fb;
            palette_enter_outline_mode(entries, n);
            app->palette_selected  = 0;
            app->palette_scroll    = 0;
            app->palette_query[0]  = '\0';
            app->palette_query_len = 0;
        } else {
            /* Not viewing markdown, or no headings — nothing to navigate. */
            app->palette_active = false;
        }
        return;
    }

    /* "Open Note" pushes the palette into the markdown quick switcher: a
     * recursive *.md listing of the vault root. Selecting a note opens it. */
    if (strcmp(item->text, "Open Note") == 0) {
        if (!g_md_file_paths) g_md_file_paths = calloc(APP_MD_FILES_MAX, FB_MAX_PATH);
        if (!g_md_file_names) g_md_file_names = calloc(APP_MD_FILES_MAX, 192);
        g_md_file_count = 0;
        const char *root = (g_md_file_paths && g_md_file_names) ? app_md_vault_root(app) : NULL;
        if (root) app_scan_md_files(root, 0);
        if (g_md_file_count > 0) {
            usize rootlen = root ? strlen(root) : 0;
            u32 n = g_md_file_count < APP_MD_SWITCHER_SHOW ? g_md_file_count
                                                        : (u32)APP_MD_SWITCHER_SHOW;
            PaletteListEntry entries[APP_MD_SWITCHER_SHOW];
            for (u32 i = 0; i < n; i++) {
                entries[i].name     = g_md_file_names[i];
                entries[i].name_len = (u32)strlen(g_md_file_names[i]);
                entries[i].id       = (i32)i;
                const char *rel = g_md_file_paths[i];
                if (rootlen && strncmp(rel, root, rootlen) == 0) {
                    rel += rootlen;
                    while (*rel == '/') rel++;
                }
                entries[i].detail     = rel;
                entries[i].detail_len = (u32)strlen(rel);
            }
            if (g_md_file_count > APP_MD_SWITCHER_SHOW)
                fprintf(stderr, "switcher: %u .md files, showing first %d\n",
                        g_md_file_count, APP_MD_SWITCHER_SHOW);
            palette_enter_picker_mode(PALETTE_MODE_SWITCHER, 6, entries, n);
            app->palette_selected  = 0;
            app->palette_scroll    = 0;
            app->palette_query[0]  = '\0';
            app->palette_query_len = 0;
        } else {
            app->palette_active = false;
        }
        return;
    }

    /* "Search Notes" pushes the palette into vault content-search mode. Files
     * are scanned up front; results are (re-)grepped as the user types. */
    if (strcmp(item->text, "Search Notes") == 0) {
        if (!g_md_file_paths) g_md_file_paths = calloc(APP_MD_FILES_MAX, FB_MAX_PATH);
        if (!g_md_file_names) g_md_file_names = calloc(APP_MD_FILES_MAX, 192);
        g_md_file_count = 0;
        const char *root = (g_md_file_paths && g_md_file_names) ? app_md_vault_root(app) : NULL;
        if (root) app_scan_md_files(root, 0);
        palette_enter_picker_mode(PALETTE_MODE_SEARCH, 7, NULL, 0);
        app->palette_selected  = 0;
        app->palette_scroll    = 0;
        app->palette_query[0]  = '\0';
        app->palette_query_len = 0;
        return;
    }

    /* "Show Backlinks" lists the notes that link to the current note. Builds
     * the vault link graph, finds the open note's node, and collects every
     * edge target == this note (the source is a backlink). */
    /* "Filter Graph by Folder" lists the vault's subfolders; selecting one
     * scopes the knowledge graph to it (entering graph mode if needed). */
    if (strcmp(item->text, "Filter Graph by Folder") == 0) {
        Tab *t = app_active_tab(app);
        FileBrowser *fb = (t && t->kind == TAB_FILEBROWSER && t->fb) ? t->fb
                        : (app->filebrowser.open ? &app->filebrowser : NULL);
        /* Enumerate from the graph's stable root if in graph mode, else the
         * current folder (or the focused terminal cwd). */
        const char *root = NULL;
        if (fb && fb->view_mode == FVIEW_GRAPH && fb->graph_root[0]) root = fb->graph_root;
        else if (fb && fb->cwd[0]) root = fb->cwd;
        else root = app_md_vault_root(app);
        if (!g_md_file_paths) g_md_file_paths = calloc(APP_MD_FILES_MAX, FB_MAX_PATH);
        if (!g_md_file_names) g_md_file_names = calloc(APP_MD_FILES_MAX, 192);
        g_md_file_count = 0;
        if (fb && root && g_md_file_paths && g_md_file_names) {
            /* Entry 0: the whole vault (the root itself). */
            snprintf(g_md_file_paths[0], FB_MAX_PATH, "%s", root);
            snprintf(g_md_file_names[0], 192, "(whole vault)");
            g_md_file_count = 1;
            app_scan_folders(root, 0, strlen(root));
        }
        if (g_md_file_count > 0) {
            u32 n = g_md_file_count < APP_MD_SWITCHER_SHOW ? g_md_file_count
                                                          : (u32)APP_MD_SWITCHER_SHOW;
            PaletteListEntry entries[APP_MD_SWITCHER_SHOW];
            for (u32 i = 0; i < n; i++) {
                entries[i].name     = g_md_file_names[i];
                entries[i].name_len = (u32)strlen(g_md_file_names[i]);
                entries[i].detail   = "";
                entries[i].detail_len = 0;
                entries[i].id       = (i32)i;
            }
            palette_enter_picker_mode(PALETTE_MODE_FOLDER, 8, entries, n);
            app->palette_selected  = 0;
            app->palette_scroll    = 0;
            app->palette_query[0]  = '\0';
            app->palette_query_len = 0;
        } else {
            app->palette_active = false;
        }
        return;
    }

    if (strcmp(item->text, "Show Backlinks") == 0) {
        FileBrowser *fb = app_active_md_fb(app);
        const char *root = app_md_vault_root(app);
        if (!g_md_file_paths) g_md_file_paths = calloc(APP_MD_FILES_MAX, FB_MAX_PATH);
        if (!g_md_file_names) g_md_file_names = calloc(APP_MD_FILES_MAX, 192);
        g_md_file_count = 0;
        if (fb && fb->view_path[0] && root && g_md_file_paths && g_md_file_names) {
            MdGraph *g = md_graph_build(root);
            if (g) {
                i32 cur = md_graph_find_by_path(g, fb->view_path);
                if (cur >= 0) {
                    i32 ec = md_graph_dir_edge_count(g);
                    for (i32 e = 0; e < ec && g_md_file_count < APP_MD_FILES_MAX; e++) {
                        i32 src = -1, dst = -1;
                        md_graph_dir_edge(g, e, &src, &dst);
                        if (dst != cur || src == cur || src < 0) continue;
                        const char *sp = md_graph_node_path(g, src);
                        const char *sl = md_graph_node_label(g, src);
                        if (!sp) continue;
                        /* Dedup: a note that links to cur several times yields
                         * multiple directed edges — list it once. */
                        bool seen = false;
                        for (u32 k = 0; k < g_md_file_count; k++)
                            if (strcmp(g_md_file_paths[k], sp) == 0) { seen = true; break; }
                        if (seen) continue;
                        snprintf(g_md_file_paths[g_md_file_count], FB_MAX_PATH, "%s", sp);
                        snprintf(g_md_file_names[g_md_file_count], 192, "%s", sl ? sl : sp);
                        g_md_file_count++;
                    }
                }
                md_graph_free(g);
            }
        }
        if (g_md_file_count > 0) {
            usize rootlen = root ? strlen(root) : 0;
            u32 n = g_md_file_count < APP_MD_SWITCHER_SHOW ? g_md_file_count
                                                          : (u32)APP_MD_SWITCHER_SHOW;
            PaletteListEntry entries[APP_MD_SWITCHER_SHOW];
            for (u32 i = 0; i < n; i++) {
                entries[i].name     = g_md_file_names[i];
                entries[i].name_len = (u32)strlen(g_md_file_names[i]);
                entries[i].id       = (i32)i;
                const char *rel = g_md_file_paths[i];
                if (rootlen && strncmp(rel, root, rootlen) == 0) {
                    rel += rootlen;
                    while (*rel == '/') rel++;
                }
                entries[i].detail     = rel;
                entries[i].detail_len = (u32)strlen(rel);
            }
            palette_enter_picker_mode(PALETTE_MODE_BACKLINKS, 6, entries, n);
            app->palette_selected  = 0;
            app->palette_scroll    = 0;
            app->palette_query[0]  = '\0';
            app->palette_query_len = 0;
        } else {
            /* No backlinks (or not viewing a note) — close. */
            app->palette_active = false;
        }
        return;
    }

    /* "Agent History..." pushes the palette into history mode instead of
     * closing it. Subsequent selections pick a session; Esc returns to root.
     *
     * Filter behaviour: if the focused tab's cwd is exactly $HOME we pass
     * NULL (show every session anywhere on disk). Otherwise we pass the
     * cwd so the picker only lists sessions associated with the project
     * the user is currently inside — matches the expected workflow of
     * "I'm in this folder, show me what I did here". */
    if (strcmp(item->text, "Start Site") == 0 ||
        strcmp(item->text, "Restart Site") == 0) {
        if (strcmp(item->text, "Restart Site") == 0)
            palette_enter_sites_restart_mode(&app->site_mgr);
        else
            palette_enter_sites_mode(&app->site_mgr);
        app->palette_selected  = 0;
        app->palette_scroll    = 0;
        app->palette_query[0]  = '\0';
        app->palette_query_len = 0;
        return;
    }

    if (strcmp(item->text, "Agent History") == 0) {
        /* Lazy detect on first open; subsequent opens reuse the cached result.
         * User can force a refresh from the picker via ⌘R. */
        app_detect_cli_agents(app, false);
        /* Resolve cwd from two sources, in order:
         *   1. terminal->cwd  — set by OSC 7 (only if the shell emits it).
         *   2. session_local_cwd() — proc_pidinfo on the PTY fg pgrp, works
         *      with any unmodified shell.
         * If cwd resolves to $HOME (i.e., user is sitting at their home,
         * not inside a project), drop the filter and show everything. */
        const char *cwd_filter = NULL;
        Tab *atab = app_active_tab(app);
        const char *cwd = NULL;
        if (atab && atab->terminal && atab->terminal->cwd && atab->terminal->cwd[0]) {
            cwd = atab->terminal->cwd;
        } else if (atab && atab->session) {
            const char *probe = session_local_cwd(atab->session);
            if (probe && probe[0]) cwd = probe;
        }
        if (cwd && cwd[0]) {
            const char *home = getenv("HOME");
            if (!home || strcmp(cwd, home) != 0) {
                cwd_filter = cwd;
            }
        }
        palette_enter_history_mode(cwd_filter);
        app->palette_selected = 0;
        app->palette_scroll   = 0;
        app->palette_query[0] = '\0';
        app->palette_query_len = 0;
        return;
    }

    if (strcmp(item->text, "New Tab") == 0) {
        Session *s = app_new_local_session_here(app);
        if (s && session_status(s) == SESSION_CONNECTED) app_new_tab(app, s, "Terminal");
    } else if (strcmp(item->text, "Close Tab") == 0) {
        if (app->tab_count > 0 &&
            !app_confirm_close_arm(app, app->active_tab, -1, -1))
            app_close_tab(app, app->active_tab);
    } else if (strcmp(item->text, "Split Horizontal") == 0) {
        app_split_tab(app, SPLIT_H);
    } else if (strcmp(item->text, "Split Vertical") == 0) {
        app_split_tab(app, SPLIT_V);
    } else if (strcmp(item->text, "Toggle Sidebar") == 0) {
        if (app->sidebar_visible) {
            app->sidebar_visible = false;
            app->sidebar_width = 0;
            app->filebrowser.open = false;
            fb_close_viewer(&app->filebrowser);
        } else {
            app_open_sidebar_for_context(app);
        }
        i32 w2, h2, fw2, fh2;
        platform_window_get_size(window, &w2, &h2);
        platform_window_get_framebuffer_size(window, &fw2, &fh2);
        app_update_layout(app, w2, h2, fw2, fh2);
    } else if (strcmp(item->text, "Toggle File Browser") == 0) {
        /* Toggle: closes if already open, otherwise shows the sidebar and
         * navigates the file browser to the active terminal's CWD (falling
         * back to the process CWD). */
        if (app->filebrowser.open) {
            app->filebrowser.open = false;
            fb_close_viewer(&app->filebrowser);
        } else {
            if (!app->sidebar_visible) {
                app->sidebar_visible = true;
                app->sidebar_width  = SIDEBAR_DEFAULT_PT * app->dpi_scale;
            }
            const char *groot = app_active_graph_root(app);
            Tab *tab = app_active_tab(app);
            if (groot) {
                /* A Vault graph owns the window — list that same Vault. */
                fb_navigate(&app->filebrowser, groot);
            } else if (tab && tab->terminal && tab->terminal->cwd && tab->terminal->cwd[0]) {
                fb_navigate(&app->filebrowser, tab->terminal->cwd);
            } else {
                char cwd[1024];
                if (getcwd(cwd, sizeof(cwd))) fb_navigate(&app->filebrowser, cwd);
            }
            app->filebrowser.open = true;
        }
        i32 w2, h2, fw2, fh2;
        platform_window_get_size(window, &w2, &h2);
        platform_window_get_framebuffer_size(window, &fw2, &fh2);
        app_update_layout(app, w2, h2, fw2, fh2);
    } else if (strcmp(item->text, "Graph View") == 0) {
        /* Graph the notes Vault (a single known folder) — never the terminal
         * cwd / $HOME, so no arbitrary/TCC-protected scan. The graph owns the
         * whole window, so close the docked sidebar first: keeping both up reads
         * as two competing file panels, and closing the sidebar afterwards used
         * to strand the graph tab. Always ENTERS the vault graph (rescope, not
         * toggle) so re-issuing the command can't flip it back to a list. */
        app_notes_vault_ensure(app);
        const char *vault = app_notes_vault_path(app);
        if (app->sidebar_visible || app->filebrowser.open) {
            app->sidebar_visible  = false;
            app->sidebar_width    = 0;
            app->filebrowser.open = false;
            fb_close_viewer(&app->filebrowser);
        }
        Tab *gt = app_active_tab(app);
        FileBrowser *gfb = NULL;
        if (gt && gt->kind == TAB_FILEBROWSER && gt->fb) {
            gfb = gt->fb;
        } else {
            i32 ni = app_new_filebrowser_tab(app, vault);
            if (ni >= 0 && ni < app->tab_count && app->tabs[ni].fb) {
                app_switch_tab(app, ni);
                gfb = app->tabs[ni].fb;
                gfb->graph_owns_tab = true;  /* close graph → return to prev tab */
            }
        }
        if (gfb) fb_graph_rescope(gfb, vault);
        app_refresh_fb_tab_title(app_active_tab(app));   /* tab → graphed folder name */
        i32 w2, h2, fw2, fh2;
        platform_window_get_size(window, &w2, &h2);
        platform_window_get_framebuffer_size(window, &fw2, &fh2);
        app_update_layout(app, w2, h2, fw2, fh2);
    } else if (strcmp(item->text, "Graph Current Folder") == 0) {
        /* Escape hatch: graph the active folder (the old cwd-rooted behaviour). */
        Tab *gt = app_active_tab(app);
        if (gt && gt->kind == TAB_FILEBROWSER && gt->fb) {
            fb_toggle_graph(gt->fb);
            app_refresh_fb_tab_title(gt);                /* tab → graphed folder name */
        } else {
            const char *dir = NULL;
            if (app->filebrowser.open && app->filebrowser.cwd[0]) dir = app->filebrowser.cwd;
            else dir = app_active_local_cwd(app);   /* OSC 7 → proc_pidinfo live cwd */
            /* Never root the graph at "/" — a Finder/Dock launch leaves the app
             * process cwd at "/", which would scan the whole filesystem and show
             * "/" as the tab. Fall back to $HOME so the graph + title are the
             * user's actual folder. */
            if (!dir || !dir[0] || (dir[0] == '/' && dir[1] == '\0')) dir = getenv("HOME");
            if (!dir || !dir[0]) dir = ".";
            if (dir) {
                i32 ni = app_new_filebrowser_tab(app, dir);
                if (ni >= 0 && ni < app->tab_count && app->tabs[ni].fb) {
                    app_switch_tab(app, ni);
                    fb_toggle_graph(app->tabs[ni].fb);
                    app->tabs[ni].fb->graph_owns_tab = true; /* close → prev tab */
                    app_refresh_fb_tab_title(&app->tabs[ni]); /* tab → graphed folder name */
                    i32 w2, h2, fw2, fh2;
                    platform_window_get_size(window, &w2, &h2);
                    platform_window_get_framebuffer_size(window, &fw2, &fh2);
                    app_update_layout(app, w2, h2, fw2, fh2);
                }
            }
        }
    } else if (strcmp(item->text, "Open Vault") == 0) {
        /* Open the notes Vault folder as a full-window file browser. */
        app_notes_vault_ensure(app);
        const char *vault = app_notes_vault_path(app);
        i32 ni = app_new_filebrowser_tab(app, vault);
        if (ni >= 0 && ni < app->tab_count) {
            app_switch_tab(app, ni);
            i32 w2, h2, fw2, fh2;
            platform_window_get_size(window, &w2, &h2);
            platform_window_get_framebuffer_size(window, &fw2, &fh2);
            app_update_layout(app, w2, h2, fw2, fh2);
        }
    } else if (strcmp(item->text, "Toggle Broadcast") == 0) {
        if (app->broadcast_mode) app_disable_broadcast(app);
        else app_enable_broadcast(app, true);
    } else if (strcmp(item->text, "Broadcast Targets") == 0) {
        app_enable_broadcast(app, true);
    } else if (strcmp(item->text, "Port Forwarding") == 0) {
        app_reset_port_forward_dialog(app);
    } else if (strcmp(item->text, "SSH Connect") == 0) {
        app_reset_ssh_dialog(app);
    } else if (strcmp(item->text, "Import SSH Config") == 0) {
        app_import_default_ssh_config(app);
    } else if (strcmp(item->text, "Generate SSH Key") == 0) {
        app_open_key_manager(app);
        app_open_keygen_form(app);
    } else if (strcmp(item->text, "SSH Keys") == 0) {
        app_open_key_manager(app);
    } else if (strcmp(item->text, "Known Hosts") == 0) {
        app->known_hosts_open = true;
        app->known_hosts_selected = -1;
        app->known_hosts_scroll = 0;
        app->known_hosts_filter[0] = '\0';
        app->known_hosts_filter_len = 0;
    } else if (strcmp(item->text, "Clear History") == 0) {
        if (app->vault) {
            vault_clear_history(app->vault);
            sidebar_load_hosts(app->vault);
        }
    } else if (strcmp(item->text, "Settings") == 0) {
        settings_toggle(&app->settings);
    } else if (strcmp(item->text, "Site Manager") == 0) {
        app->sites.active = true;
        if (app->sites.selected < 0 && app->site_mgr.count > 0)
            app->sites.selected = 0;
        app->sites.hover_row = app->sites.hover_action = -1;
    } else if (strcmp(item->text, "Toggle Quake Mode") == 0) {
        if (platform_is_quake_mode(window)) {
            platform_set_quake_mode(window, false);
        } else {
            platform_set_quake_params(app->config.quake_height_ratio,
                                      app->config.quake_animation_duration);
            platform_set_quake_mode(window, true);
            app_register_quake_hotkey(&app->config, window);
        }
        app->quake_active = platform_is_quake_mode(window);
    } else if (strcmp(item->text, "Create Tab Group") == 0) {
        i32 gi = app_create_tab_group(app, NULL, (Color){0});
        if (gi >= 0 && app->active_tab >= 0 && app->active_tab < app->tab_count) {
            app_set_tab_group(app, app->active_tab, gi);
        }
    } else if (strcmp(item->text, "Ungroup Current Tab") == 0) {
        if (app->active_tab >= 0 && app->active_tab < app->tab_count) {
            app_remove_tab_from_group(app, app->active_tab);
        }
    } else if (strcmp(item->text, "Toggle Tab Sleep (this tab)") == 0) {
        if (app->active_tab >= 0 && app->active_tab < app->tab_count) {
            Tab *tab = &app->tabs[app->active_tab];
            tab->sleep_disabled = !tab->sleep_disabled;
            char msg[128];
            snprintf(msg, sizeof(msg), "Auto-sleep %s for \"%s\"",
                     tab->sleep_disabled ? "disabled" : "enabled",
                     tab_effective_title(tab));
            app_show_toast(app, msg);
        }
    } else if (strcmp(item->text, "Sleep This Tab Now") == 0) {
        if (app->active_tab >= 0 && app->active_tab < app->tab_count) {
            Tab *cur = &app->tabs[app->active_tab];
            if (cur->sleeping) {
                app_show_toast(app, "Tab is already asleep");
            } else if (!cur->session || !cur->terminal) {
                app_show_toast(app, "Nothing to sleep on this tab");
            } else {
                app_sleep_tab(app, app->active_tab);
            }
        }
    } else if (strcmp(item->text, "Toggle Render Benchmark") == 0) {
        app_bench_toggle(app);
    } else if (strcmp(item->text, "Toggle Tab Bar") == 0) {
        app->config.show_tab_bar = !app->config.show_tab_bar;
        i32 lw, lh, lfw, lfh;
        platform_window_get_size(window, &lw, &lh);
        platform_window_get_framebuffer_size(window, &lfw, &lfh);
        app_update_layout(app, lw, lh, lfw, lfh);
        app_show_toast(app, app->config.show_tab_bar ? "Tab bar: on" : "Tab bar: off");
    } else if (strcmp(item->text, "Toggle Toolbar Icons") == 0) {
        app->config.show_toolbar_icons = !app->config.show_toolbar_icons;
        i32 lw, lh, lfw, lfh;
        platform_window_get_size(window, &lw, &lh);
        platform_window_get_framebuffer_size(window, &lfw, &lfh);
        app_update_layout(app, lw, lh, lfw, lfh);
        app_show_toast(app, app->config.show_toolbar_icons ? "Toolbar icons: on" : "Toolbar icons: off");
    } else if (strcmp(item->text, "Toggle Scrollbar") == 0) {
        app->config.show_scrollbar = !app->config.show_scrollbar;
        app_show_toast(app, app->config.show_scrollbar ? "Scrollbar: on" : "Scrollbar: off");
    } else if (strcmp(item->text, "Toggle Status Bar") == 0) {
        app->config.show_status_bar = !app->config.show_status_bar;
        i32 lw, lh, lfw, lfh;
        platform_window_get_size(window, &lw, &lh);
        platform_window_get_framebuffer_size(window, &lfw, &lfh);
        app_update_layout(app, lw, lh, lfw, lfh);
        app_show_toast(app, app->config.show_status_bar ? "Status bar: on" : "Status bar: off");
    } else if (strcmp(item->text, "Window: Snap Left") == 0) {
        platform_window_snap(window, WIN_SNAP_LEFT_HALF);
    } else if (strcmp(item->text, "Window: Snap Right") == 0) {
        platform_window_snap(window, WIN_SNAP_RIGHT_HALF);
    } else if (strcmp(item->text, "Window: Snap Top") == 0) {
        platform_window_snap(window, WIN_SNAP_TOP_HALF);
    } else if (strcmp(item->text, "Window: Snap Bottom") == 0) {
        platform_window_snap(window, WIN_SNAP_BOTTOM_HALF);
    } else if (strcmp(item->text, "Window: Maximize") == 0) {
        platform_window_snap(window, WIN_SNAP_FULL);
    } else if (strcmp(item->text, "Window: Center") == 0) {
        platform_window_snap(window, WIN_SNAP_CENTER);
    } else if (strcmp(item->text, "Window: Snap Top-Left") == 0) {
        platform_window_snap(window, WIN_SNAP_TOP_LEFT);
    } else if (strcmp(item->text, "Window: Snap Top-Right") == 0) {
        platform_window_snap(window, WIN_SNAP_TOP_RIGHT);
    } else if (strcmp(item->text, "Window: Snap Bottom-Left") == 0) {
        platform_window_snap(window, WIN_SNAP_BOTTOM_LEFT);
    } else if (strcmp(item->text, "Window: Snap Bottom-Right") == 0) {
        platform_window_snap(window, WIN_SNAP_BOTTOM_RIGHT);
    } else if (strcmp(item->text, "Split: Divider Left") == 0) {
        Tab *t = app_active_tab(app);
        if (t && t->split != SPLIT_NONE) {
            t->split_ratio -= 0.05f;
            if (t->split_ratio < 0.15f) t->split_ratio = 0.15f;
            app_resize_tab_panes(app, t);
        }
    } else if (strcmp(item->text, "Split: Divider Right") == 0) {
        Tab *t = app_active_tab(app);
        if (t && t->split != SPLIT_NONE) {
            t->split_ratio += 0.05f;
            if (t->split_ratio > 0.85f) t->split_ratio = 0.85f;
            app_resize_tab_panes(app, t);
        }
    } else if (strcmp(item->text, "Split: Reset (50/50)") == 0) {
        Tab *t = app_active_tab(app);
        if (t && t->split != SPLIT_NONE) {
            t->split_ratio = 0.5f;
            app_resize_tab_panes(app, t);
        }
    } else if (strcmp(item->text, "Wake All Tabs") == 0) {
        i32 woken = 0;
        for (i32 i = 0; i < app->tab_count; i++) {
            if (app->tabs[i].sleeping && app_wake_tab(app, i)) woken++;
        }
        if (woken > 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Woke %d tab%s", woken, woken == 1 ? "" : "s");
            app_show_toast(app, msg);
        } else {
            app_show_toast(app, "No sleeping tabs");
        }
    } else if (strncmp(item->text, "Theme: ", 7) == 0) {
        const Theme *new_theme = theme_get_by_name(item->text + 7);
        if (new_theme) app_switch_theme(app, new_theme);
    } else if (strncmp(item->text, "Set Profile: ", 13) == 0) {
        const char *prof_name = item->text + 13;
        Tab *tab = app_active_tab(app);
        if (tab) {
            /* Find matching profile */
            for (i32 i = 0; i < app->config.profile_count; i++) {
                if (strcmp(app->config.profiles[i].name, prof_name) == 0) {
                    tab->profile_index = i;
                    const TermProfile *p = &app->config.profiles[i];
                    /* Apply profile overrides to config for this tab */
                    if (p->has_theme) {
                        const Theme *pt = theme_get_by_name(p->theme_name);
                        if (pt) {
                            app->config.theme = pt;
                            snprintf(app->config.theme_name, sizeof(app->config.theme_name),
                                     "%s", p->theme_name);
                        }
                    }
                    if (p->has_cursor) {
                        app->config.cursor_style = p->cursor_style;
                    }
                    if (p->has_opacity) {
                        app->config.opacity = p->opacity;
                    }
                    app_apply_config(app);
                    break;
                }
            }
        }
    } else if (strcmp(item->text, "Font Size") == 0) {
        /* Enter numeric-input sub-mode; user types size then Enter. */
        app->palette_input_mode = 1;
        app->palette_query_len = 0;
        app->palette_query[0] = '\0';
        app->palette_selected = 0;
        app->palette_scroll = 0;
        return; /* keep palette open */
    } else if (strcmp(item->text, "Opacity") == 0) {
        /* Numeric-input sub-mode for opacity (0–100 %). Prefill the input
         * with the current value so the user can lightly tweak instead of
         * retyping from scratch. */
        app->palette_input_mode = 2;
        i32 cur = (i32)(app->config.opacity * 100.0f + 0.5f);
        if (cur < 0)   cur = 0;
        if (cur > 100) cur = 100;
        snprintf(app->palette_query, sizeof(app->palette_query), "%d", cur);
        app->palette_query_len = (i32)strlen(app->palette_query);
        app->palette_selected = 0;
        app->palette_scroll = 0;
        return; /* keep palette open */
    } else if (strcmp(item->text, "Line Height: Increase") == 0) {
        if (app->config.cell_height_scale < 2.0f) {
            app->config.cell_height_scale += 0.05f;
            app_reload_font(app, window);
            char msg[48]; snprintf(msg, sizeof(msg), "Line height: %.2f", app->config.cell_height_scale);
            app_show_toast(app, msg);
        }
    } else if (strcmp(item->text, "Line Height: Decrease") == 0) {
        if (app->config.cell_height_scale > 0.8f) {
            app->config.cell_height_scale -= 0.05f;
            app_reload_font(app, window);
            char msg[48]; snprintf(msg, sizeof(msg), "Line height: %.2f", app->config.cell_height_scale);
            app_show_toast(app, msg);
        }
    } else if (strcmp(item->text, "Cursor: Cycle Style") == 0) {
        app->config.cursor_style = (u8)((app->config.cursor_style + 1) % 3);
        const char *names[] = {"Block", "Underline", "Bar"};
        char msg[48]; snprintf(msg, sizeof(msg), "Cursor: %s", names[app->config.cursor_style]);
        app_show_toast(app, msg);
    } else if (strcmp(item->text, "Toggle Cursor Blink") == 0) {
        app->config.cursor_blink = !app->config.cursor_blink;
        app_show_toast(app, app->config.cursor_blink ? "Cursor blink: on" : "Cursor blink: off");
    } else if (strcmp(item->text, "Toggle Cursor Animation") == 0) {
        app->config.cursor_animate = !app->config.cursor_animate;
        app_show_toast(app, app->config.cursor_animate ? "Cursor animation: on" : "Cursor animation: off");
    } else if (strcmp(item->text, "Toggle Bold is Bright") == 0) {
        app->config.bold_is_bright = !app->config.bold_is_bright;
        app_show_toast(app, app->config.bold_is_bright ? "Bold bright: on" : "Bold bright: off");
    } else if (strcmp(item->text, "Toggle Copy on Select") == 0) {
        app->config.copy_on_select = !app->config.copy_on_select;
        app_show_toast(app, app->config.copy_on_select ? "Copy on select: on" : "Copy on select: off");
    } else if (strcmp(item->text, "Toggle Option as Alt") == 0) {
        app->config.option_as_alt = !app->config.option_as_alt;
        app_show_toast(app, app->config.option_as_alt ? "Option=Alt: on" : "Option=Alt: off");
    } else if (strcmp(item->text, "Toggle Visual Bell") == 0) {
        app->config.visual_bell = !app->config.visual_bell;
        app_show_toast(app, app->config.visual_bell ? "Visual bell: on" : "Visual bell: off");
    } else if (strcmp(item->text, "Toggle Audible Bell") == 0) {
        app->config.audible_bell = !app->config.audible_bell;
        app_show_toast(app, app->config.audible_bell ? "Audible bell: on" : "Audible bell: off");
    } else if (strcmp(item->text, "Toggle Ligatures") == 0) {
        app->config.enable_ligatures = !app->config.enable_ligatures;
        app_show_toast(app, app->config.enable_ligatures ? "Ligatures: on" : "Ligatures: off");
    } else if (strcmp(item->text, "Scrollback: Increase") == 0) {
        app->config.scrollback_lines += 1000;
        if (app->config.scrollback_lines > CONFIG_SCROLLBACK_MAX_LINES)
            app->config.scrollback_lines = CONFIG_SCROLLBACK_MAX_LINES;
        for (i32 i = 0; i < app->tab_count; i++) {
            if (app->tabs[i].terminal)
                terminal_set_scrollback_limit(app->tabs[i].terminal, app->config.scrollback_lines);
            if (app->tabs[i].terminal2)
                terminal_set_scrollback_limit(app->tabs[i].terminal2, app->config.scrollback_lines);
        }
        char msg[48]; snprintf(msg, sizeof(msg), "Scrollback: %d lines", app->config.scrollback_lines);
        app_show_toast(app, msg);
    } else if (strcmp(item->text, "Scrollback: Decrease") == 0) {
        app->config.scrollback_lines -= 1000;
        if (app->config.scrollback_lines < CONFIG_SCROLLBACK_MIN_LINES)
            app->config.scrollback_lines = CONFIG_SCROLLBACK_MIN_LINES;
        for (i32 i = 0; i < app->tab_count; i++) {
            if (app->tabs[i].terminal)
                terminal_set_scrollback_limit(app->tabs[i].terminal, app->config.scrollback_lines);
            if (app->tabs[i].terminal2)
                terminal_set_scrollback_limit(app->tabs[i].terminal2, app->config.scrollback_lines);
        }
        char msg[48]; snprintf(msg, sizeof(msg), "Scrollback: %d lines", app->config.scrollback_lines);
        app_show_toast(app, msg);
    } else if (strncmp(item->text, "Tab Sleep: ", 11) == 0) {
        const char *v = item->text + 11;
        if      (strcmp(v, "Off") == 0)    app->config.tab_sleep_idle_minutes = 0.0f;
        else if (strcmp(v, "5 min") == 0)  app->config.tab_sleep_idle_minutes = 5.0f;
        else if (strcmp(v, "10 min") == 0) app->config.tab_sleep_idle_minutes = 10.0f;
        else if (strcmp(v, "20 min") == 0) app->config.tab_sleep_idle_minutes = 20.0f;
        else if (strcmp(v, "30 min") == 0) app->config.tab_sleep_idle_minutes = 30.0f;
        else if (strcmp(v, "60 min") == 0) app->config.tab_sleep_idle_minutes = 60.0f;
        char msg[64]; snprintf(msg, sizeof(msg), "Tab auto-sleep: %s", v);
        app_show_toast(app, msg);
    } else if (strcmp(item->text, "Opacity: Increase") == 0) {
        app->config.opacity += 0.05f;
        if (app->config.opacity > 1.0f) app->config.opacity = 1.0f;
        platform_window_set_opacity(window, app->config.opacity);
        char msg[48]; snprintf(msg, sizeof(msg), "Opacity: %.0f%%", app->config.opacity * 100.0f);
        app_show_toast(app, msg);
    } else if (strcmp(item->text, "Opacity: Decrease") == 0) {
        app->config.opacity -= 0.05f;
        if (app->config.opacity < 0.3f) app->config.opacity = 0.3f;
        platform_window_set_opacity(window, app->config.opacity);
        char msg[48]; snprintf(msg, sizeof(msg), "Opacity: %.0f%%", app->config.opacity * 100.0f);
        app_show_toast(app, msg);
    } else if (strcmp(item->text, "Settings: Appearance") == 0) {
        app->settings.active_tab = SETTINGS_TAB_APPEARANCE;
        if (!app->settings.open) settings_toggle(&app->settings);
    } else if (strcmp(item->text, "Settings: Translate") == 0) {
        app->settings.active_tab = SETTINGS_TAB_TRANSLATE;
        if (!app->settings.open) settings_toggle(&app->settings);
    } else if (strcmp(item->text, "Settings: Notifications") == 0) {
        app->settings.active_tab = SETTINGS_TAB_NOTIFY;
        if (!app->settings.open) settings_toggle(&app->settings);
    } else if (strcmp(item->text, "Toggle Translate") == 0) {
        app->config.translate.enabled = !app->config.translate.enabled;
        app_show_toast(app, app->config.translate.enabled
                       ? "Translate: on" : "Translate: off");
    } else if (strcmp(item->text, "Translate: Cycle Target Language") == 0) {
        translate_cycle_language(app->config.translate.target_lang,
                                 sizeof(app->config.translate.target_lang));
        translate_normalize_direction(&app->config.translate);
        char msg[64];
        snprintf(msg, sizeof msg, "Target language: %s",
                 app->config.translate.target_lang);
        app_show_toast(app, msg);
    } else if (strcmp(item->text, "Translate: Cycle Backend") == 0) {
        /* Agent CLI → Local model → API → Agent CLI. Local is skipped
         * when the build lacks the engine. */
        if (app->config.translate.backend == TRANSLATE_BACKEND_AGENT) {
#ifdef LIU_HAVE_LOCAL_LLM
            app->config.translate.backend = TRANSLATE_BACKEND_LOCAL;
            app_show_toast(app, "Translate backend: Local Model");
#else
            app->config.translate.backend = TRANSLATE_BACKEND_API;
            app_show_toast(app, "Translate backend: API");
#endif
        } else if (app->config.translate.backend == TRANSLATE_BACKEND_LOCAL) {
            app->config.translate.backend = TRANSLATE_BACKEND_API;
            app_show_toast(app, "Translate backend: API");
        } else {
            app->config.translate.backend = TRANSLATE_BACKEND_AGENT;
            app_show_toast(app, "Translate backend: Agent CLI");
        }
        /* This flips the backend behind the Settings panel's back; clear any
         * Translate-tab overlay/editor so a stale flag can't intercept input
         * against the wrong backend on the next open. */
        app->settings.translate_model_picker_open = false;
        app->settings.translate_model_menu_scroll = 0;
        app->settings.editing_api_key = false;
        app->settings.editing_api_url = false;
        app->settings.editing_api_model = false;
    } else if (strcmp(item->text, "Find in Terminal") == 0) {
        app->search_active = true;
        app->search_query[0] = '\0';
        app->search_query_len = 0;
    } else if (strcmp(item->text, "New Window") == 0) {
        app_spawn_new_window(app);
    } else if (strcmp(item->text, "Change Title") == 0) {
        if (app->active_tab >= 0 && app->active_tab < app->tab_count)
            app_start_tab_rename(app, app->active_tab);
    } else if (strcmp(item->text, "Check for Updates") == 0) {
        updater_begin_check(&app->updater, false);
        app->settings.active_tab = SETTINGS_TAB_ABOUT;   /* progress lives there */
        if (!app->settings.open) settings_toggle(&app->settings);
        app_show_toast(app, "Checking for updates\xe2\x80\xa6");
    } else if (strcmp(item->text, "Rename Group") == 0) {
        i32 gi = (app->active_tab >= 0 && app->active_tab < app->tab_count)
               ? app->tabs[app->active_tab].group_index : -1;
        if (gi >= 0 && gi < MAX_TAB_GROUPS && app->tab_groups[gi].used)
            app_start_chip_rename(app, gi);
        else
            app_show_toast(app, "Active tab is not in a group");
    } else if (strcmp(item->text, "Close Group") == 0) {
        i32 gi = (app->active_tab >= 0 && app->active_tab < app->tab_count)
               ? app->tabs[app->active_tab].group_index : -1;
        if (gi >= 0 && gi < MAX_TAB_GROUPS && app->tab_groups[gi].used) {
            if (!app_confirm_close_arm(app, app->active_tab, -1, gi)) {
                app_close_tab_group(app, gi);
                (void)app_respawn_on_empty(app);   /* never leave zero tabs */
            }
        } else {
            app_show_toast(app, "Active tab is not in a group");
        }
    } else if (strcmp(item->text, "Toggle Group Open/Close") == 0) {
        i32 gi = (app->active_tab >= 0 && app->active_tab < app->tab_count)
               ? app->tabs[app->active_tab].group_index : -1;
        if (gi >= 0 && gi < MAX_TAB_GROUPS && app->tab_groups[gi].used)
            app_toggle_tab_group_collapsed(app, gi);
        else
            app_show_toast(app, "Active tab is not in a group");
    } else if (strcmp(item->text, "Stop All Sites") == 0) {
        i32 stopped = 0;
        for (i32 i = 0; i < app->site_mgr.count; i++) {
            Site *st = &app->site_mgr.sites[i];
            if (st->status == SITE_RUNNING || st->status == SITE_STARTING) {
                site_stop(st);
                stopped++;
            }
        }
        char msg[64];
        snprintf(msg, sizeof msg, stopped == 1 ? "Stopping %d site"
                                               : "Stopping %d sites", stopped);
        app_show_toast(app, msg);
    } else if (strcmp(item->text, "Mosh Connect") == 0) {
        app_reset_ssh_dialog(app);
        app->ssh_use_mosh = true;
    } else if (strcmp(item->text, "Telnet Connect") == 0) {
        app_reset_ssh_dialog(app);
        app->ssh_dialog_proto = 1;
    } else if (strcmp(item->text, "Serial Connect") == 0) {
        app_reset_ssh_dialog(app);
        app->ssh_dialog_proto = 2;
    } else if (strcmp(item->text, "Settings: Terminal") == 0) {
        app->settings.active_tab = SETTINGS_TAB_TERMINAL;
        if (!app->settings.open) settings_toggle(&app->settings);
    } else if (strcmp(item->text, "Settings: Keys") == 0) {
        app->settings.active_tab = SETTINGS_TAB_KEYS;
        if (!app->settings.open) settings_toggle(&app->settings);
    } else if (strcmp(item->text, "Settings: About") == 0) {
        app->settings.active_tab = SETTINGS_TAB_ABOUT;
        if (!app->settings.open) settings_toggle(&app->settings);
    } else if (strcmp(item->text, "Settings: Vault") == 0) {
        app->settings.active_tab = SETTINGS_TAB_VAULT;
        if (!app->settings.open) settings_toggle(&app->settings);
    } else if (strcmp(item->text, "Vault: Open Browser") == 0) {
        if (app->vault && vault_is_unlocked(app->vault)) {
            app_vault_browser_open(app);
        } else {
            app_vault_open_unlock(app, ACT_VAULT_BROWSER);
        }
    } else if (strcmp(item->text, "Vault: Unlock") == 0) {
        if (app->vault && vault_is_unlocked(app->vault)) {
            app_show_toast(app, "Vault already unlocked");
        } else {
            app_vault_open_unlock(app, 0);
        }
    } else if (strcmp(item->text, "Vault: Lock Now") == 0) {
        app_vault_lock_now(app);
    } else if (strcmp(item->text, "Vault: Change Master Password") == 0) {
        if (app->vault && vault_is_unlocked(app->vault)) {
            app->settings.active_tab = SETTINGS_TAB_VAULT;
            if (!app->settings.open) settings_toggle(&app->settings);
        } else {
            app_show_toast(app, "Unlock vault first");
        }
    } else if (strcmp(item->text, "Import Theme") == 0) {
        /* Open native file dialog for theme import */
        const char *filepath = platform_open_file_dialog("Import Theme",
            "itermcolors,yml,yaml,toml,json");
        if (filepath) {
            Theme imported = {0};
            if (theme_import_file(filepath, &imported)) {
                /* Save to user themes dir */
                theme_save_user(&imported);
                /* Reload user themes */
                theme_load_user_themes();
                /* Apply immediately */
                const Theme *ti = theme_get_by_name(imported.name);
                if (ti) app_switch_theme(app, ti);
            }
        }
    } else if (strcmp(item->text, "Create Theme") == 0) {
        /* Spawn the AI-driven theme creation modal. The dialog itself
         * picks an agent and runs the generation; here we just open. */
        app_open_create_theme(app);
    } else if (strcmp(item->text, "Create Note") == 0) {
        /* Same modal in doc mode — generates a Markdown note into the Vault. */
        app_open_create_doc(app);
    }
    /* "Delete Theme: …" is no longer surfaced from the palette — Settings →
     * Appearance is the single destructive entry point. */

    app->palette_active = false;
}

static void app_reset_ssh_dialog(AppState *app) {
    app->ssh_dialog_active = true;
    memset(app->ssh_host, 0, sizeof(app->ssh_host));
    memset(app->ssh_user, 0, sizeof(app->ssh_user));
    memset(app->ssh_port, 0, sizeof(app->ssh_port));
    memset(app->ssh_password, 0, sizeof(app->ssh_password));
    app->ssh_field = 0;
    app->ssh_use_mosh = false;
    app->ssh_dialog_proto = 0;
    app->ssh_forward_x11 = false;

    /* Clear any stale inline error banner from a previous open. */
    app->ssh_dialog_error[0] = '\0';
    app->ssh_dialog_error_until = 0;

    /* Load recent history for SSH dialog suggestions.
     *
     * vault_get_history returns entries sorted newest-first. Multiple
     * re-connects to the same target show up as duplicates — collapse them
     * here, keeping only the most recent (host+user+port) tuple so each
     * unique destination appears once in the Recent list. Pull a wider
     * window than we display so we still have 10 unique entries even when
     * the raw stream is mostly repeats. */
    app->ssh_history_count = 0;
    app->ssh_history_selected = -1;
    app->ssh_history_hover = -1;
    if (app->vault) {
        if (!app->ssh_history) {
            app->ssh_history = calloc(SSH_HISTORY_CAP, sizeof(*app->ssh_history));
            if (!app->ssh_history) return;
        }
        VaultHistoryEntry raw[64];
        i32 raw_n = vault_get_history(app->vault, raw, 64);
        if (raw_n < 0) raw_n = 0;
        i32 cap = SSH_HISTORY_CAP;
        for (i32 i = 0; i < raw_n && app->ssh_history_count < cap; i++) {
            bool dup = false;
            for (i32 j = 0; j < app->ssh_history_count; j++) {
                if (app->ssh_history[j].port == raw[i].port &&
                    strcmp(app->ssh_history[j].hostname, raw[i].hostname) == 0 &&
                    strcmp(app->ssh_history[j].username, raw[i].username) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) app->ssh_history[app->ssh_history_count++] = raw[i];
        }
    }
}

static void app_ssh_dialog_submit(AppState *app) {
    /* Telnet / Serial reuse the dialog shell: host field doubles as the
     * device path for serial, port as the baud rate. No auth fields apply
     * (telnet servers log in in-band; serial lines have no login layer). */
    if (app->ssh_dialog_proto == 1) {
        if (!app->ssh_host[0]) {
            snprintf(app->ssh_dialog_error, sizeof(app->ssh_dialog_error),
                     "Host is required.");
            app->ssh_dialog_error_until = platform_time_sec() + 4.0;
            app->ssh_field = 0;
            return;
        }
        i32 tport = app->ssh_port[0] ? atoi(app->ssh_port) : 23;
        Session *ts = session_create_telnet(app->ssh_host, tport,
                                            app->grid_cols, app->grid_rows);
        if (ts) {
            char title[256];
            snprintf(title, sizeof title, "[telnet] %s", app->ssh_host);
            app_new_tab(app, ts, title);
        } else {
            app_show_toast(app, "Telnet connection failed");
        }
        app->ssh_dialog_active = false;
        return;
    }
    if (app->ssh_dialog_proto == 2) {
        if (!app->ssh_host[0]) {
            snprintf(app->ssh_dialog_error, sizeof(app->ssh_dialog_error),
                     "Device path is required (e.g. /dev/tty.usbserial).");
            app->ssh_dialog_error_until = platform_time_sec() + 4.0;
            app->ssh_field = 0;
            return;
        }
        SerialConfig sc = {0};
        snprintf(sc.port, sizeof sc.port, "%s", app->ssh_host);
        sc.baud_rate    = app->ssh_port[0] ? (u32)atoi(app->ssh_port) : 115200;
        sc.data_bits    = 8;
        sc.stop_bits    = 1;
        sc.parity       = PARITY_NONE;
        sc.flow_control = FLOW_NONE;
        Session *ss2 = session_create_serial(&sc, app->grid_cols, app->grid_rows);
        if (ss2) {
            char title[256];
            snprintf(title, sizeof title, "[serial] %s @%u", sc.port, sc.baud_rate);
            app_new_tab(app, ss2, title);
        } else {
            app_show_toast(app, "Could not open serial device");
        }
        app->ssh_dialog_active = false;
        return;
    }

    /* Refuse passwordless attempts. We don't want a Recent-row click or a
     * stray Enter to silently try agent auth — surface the missing field
     * as an inline banner and focus the password input. The banner expires
     * after a few seconds on its own so the dialog doesn't stay cluttered. */
    if (!app->ssh_host[0]) {
        snprintf(app->ssh_dialog_error, sizeof(app->ssh_dialog_error),
                 "Host is required.");
        app->ssh_dialog_error_until = platform_time_sec() + 4.0;
        app->ssh_field = 0;
        return;
    }
    if (!app->ssh_password[0]) {
        snprintf(app->ssh_dialog_error, sizeof(app->ssh_dialog_error),
                 "Password required — passwordless login is disabled.");
        app->ssh_dialog_error_until = platform_time_sec() + 4.0;
        app->ssh_field = 3;
        return;
    }

    SSHConfig cfg = ssh_config_default();
    snprintf(cfg.hostname, sizeof(cfg.hostname), "%s", app->ssh_host);
    snprintf(cfg.username, sizeof(cfg.username), "%s", app->ssh_user);
    cfg.port = app->ssh_port[0] ? atoi(app->ssh_port) : 22;
    snprintf(cfg.password, sizeof(cfg.password), "%s", app->ssh_password);
    cfg.auth_method = AUTH_PASSWORD;  /* always — agent fallback is gated */
    cfg.forward_x11 = app->ssh_forward_x11;

    SSHResolvedConfig *resolved = ssh_config_resolve(app->ssh_host);
    if (resolved) {
        ssh_config_apply(resolved, &cfg, false);
        ssh_config_free(resolved);
    }

    Session *s = NULL;
    if (app->ssh_use_mosh && mosh_available()) {
        s = session_create_mosh(&cfg, app->grid_cols, app->grid_rows);
    }
    if (!s) {
        s = session_create_ssh(&cfg, app->grid_cols, app->grid_rows);
    }
    if (s) {
        char title[256];
        const char *prefix = (session_type(s) == SESSION_MOSH) ? "[mosh] " : "";
        snprintf(title, sizeof(title), "%s%s@%s", prefix, cfg.username, cfg.hostname);
        app_new_tab(app, s, title);
        if (app->vault) {
            bool initial_ok = (session_status(s) != SESSION_ERROR);
            vault_add_history(app->vault, cfg.hostname, cfg.port,
                              cfg.username, "password", initial_ok);
            sidebar_load_hosts(app->vault);
        }
    } else if (app->vault) {
        vault_add_history(app->vault, cfg.hostname, cfg.port,
                          cfg.username, "password", false);
        sidebar_load_hosts(app->vault);
    }
    /* Session (if any) owns its own deep clone; release this stack copy. */
    ssh_config_dispose(&cfg);
    app->ssh_dialog_active = false;
}

static void app_fb_set_status(AppState *app, const char *msg) {
    if (!msg) msg = "";
    snprintf(app->fb_status, sizeof(app->fb_status), "%s", msg);
    app->fb_status_until = platform_time_sec() + 3.5;
}

/* Activate a theme: swap the live pointer + name, apply any style
 * overrides the theme carries (opacity, cursor style, …), refresh the
 * derived ANSI palette, and push window opacity down to the platform
 * layer if it changed. The intent is for callers to never write to
 * app->config.theme directly — going through this helper keeps the
 * style-override semantics consistent across the palette command, the
 * settings click path, theme creation, theme import, and theme
 * deletion fallback. */
static void app_switch_theme(AppState *app, const Theme *t) {
    if (!t) return;
    app->config.theme = t;
    snprintf(app->config.theme_name, sizeof(app->config.theme_name),
             "%s", t->name);
    bool style_changed = theme_apply_style_overrides(t, &app->config);
    app_apply_config(app);
    if (style_changed && g_window_ptr) {
        platform_window_set_opacity(g_window_ptr, app->config.opacity);
    }
}

/* =========================================================================
 * Create Theme dialog (Cmd+K → "Create Theme...")
 *
 * Spawns a chosen CLI agent with a prompt that quotes the on-disk theming
 * guide and asks the agent to produce a JSON theme file.  We watch the
 * target path each frame; when it appears (or its mtime advances), we
 * reload user themes and apply the result.  Stdout is captured into a
 * rolling 1 KB tail so the modal can show "thinking..." progress.
 * ========================================================================= */

/* Look up an on-disk THEMING.md once per process (a dev-time override so the
 * guide can be edited without rebuilding). Tries the executable's neighbour
 * first (release builds + .app bundle), then the source tree, then $PWD.
 * Returns "" when none is found, in which case the caller falls back to the
 * copy embedded at build time. */
static const char *create_theme_guide_path(void) {
    static char cached[1024] = {0};
    static bool resolved = false;
    if (resolved) return cached;
    resolved = true;

    const char *candidates[] = {
        "THEMING.md",
        "../THEMING.md",
        NULL,
    };
    /* Also probe next to the binary (Contents/Resources for .app, or the
     * repo root when running from build/). */
    char near_exe[1024] = {0};
    const char *exe_dir = liu_executable_dir();
    if (exe_dir && *exe_dir) {
        snprintf(near_exe, sizeof(near_exe), "%s/THEMING.md", exe_dir);
    }

    struct stat st;
    if (near_exe[0] && stat(near_exe, &st) == 0) {
        snprintf(cached, sizeof(cached), "%s", near_exe);
        return cached;
    }
    for (i32 i = 0; candidates[i]; i++) {
        if (stat(candidates[i], &st) == 0) {
            snprintf(cached, sizeof(cached), "%s", candidates[i]);
            return cached;
        }
    }
    cached[0] = '\0';
    return cached;
}

/* Return the theming guide in a heap buffer (caller frees), NUL-terminated.
 * Prefers an on-disk THEMING.md (dev override) and otherwise falls back to
 * the copy embedded at build time, so the Create-Theme agent always has the
 * guide in any install layout. Capped at 32 KB. Returns NULL only on OOM. */
static char *create_theme_read_guide(usize *out_len) {
    *out_len = 0;
    const usize cap = 32 * 1024;

    const char *path = create_theme_guide_path();
    if (path && *path) {
        FILE *f = fopen(path, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (sz >= 0) {
                if ((usize)sz > cap) sz = (long)cap;
                fseek(f, 0, SEEK_SET);
                char *buf = (char *)malloc((usize)sz + 1);
                if (buf) {
                    usize got = fread(buf, 1, (usize)sz, f);
                    buf[got] = '\0';
                    fclose(f);
                    *out_len = got;
                    return buf;
                }
            }
            fclose(f);
        }
    }

    /* Fall back to the build-time embedded guide (size is 0 when THEMING.md
     * was absent at build time). */
    if (THEMING_GUIDE_size <= 0) return NULL;
    usize n = (usize)THEMING_GUIDE_size;
    if (n > cap) n = cap;
    char *buf = (char *)malloc(n + 1);
    if (!buf) return NULL;
    memcpy(buf, THEMING_GUIDE_data, n);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

/* Slugify a theme display name into a filesystem-safe filename stem.
 * Lowercases, replaces anything outside [a-z0-9._-] with '-', collapses
 * runs of dashes. The output buffer must be at least cap bytes. */
static void create_theme_slugify(const char *name, char *out, usize cap) {
    if (!name || !cap) { if (cap) out[0] = '\0'; return; }
    usize j = 0;
    bool prev_dash = false;
    for (usize i = 0; name[i] && j + 1 < cap; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (ok) { out[j++] = c; prev_dash = (c == '-'); }
        else if (!prev_dash && j > 0) { out[j++] = '-'; prev_dash = true; }
    }
    /* trim trailing dash */
    while (j > 0 && out[j-1] == '-') j--;
    out[j] = '\0';
    if (out[0] == '\0') snprintf(out, cap, "user-theme");
}

/* Resolve the absolute target path the agent should write to. The
 * filesystem name is the slugified version of the display name; the
 * theme JSON's "name" field still holds the original name, and that's
 * what shows up in the picker. */
static void create_theme_resolve_target(const char *display_name,
                                        char *out, usize cap) {
    char slug[80];
    create_theme_slugify(display_name, slug, sizeof(slug));
    snprintf(out, cap, "%s/%s.json", theme_user_dir(), slug);
}

/* Create-Note target: <vault>/<slug>.md. Ensures the Vault exists so the
 * finalizer can fopen() the file straight away. */
static void create_doc_resolve_target(AppState *app, const char *display_name,
                                      char *out, usize cap) {
    char slug[80];
    create_theme_slugify(display_name, slug, sizeof(slug));
    /* slugify's theme-flavoured empty fallback ("user-theme") makes no sense
     * for a note — e.g. a CJK/emoji-only title that keeps no [a-z0-9._-]. */
    if (strcmp(slug, "user-theme") == 0) snprintf(slug, sizeof(slug), "note");
    app_notes_vault_ensure(app);
    snprintf(out, cap, "%s/%s.md", app_notes_vault_path(app), slug);
}

static void create_theme_set_error(AppState *app, const char *msg) {
    snprintf(app->create_theme_error, sizeof(app->create_theme_error), "%s",
             msg ? msg : "");
    app->create_theme_error_until = platform_time_sec() + 4.0;
}

/* Reset modal state and ensure the agent cache + log buffer are
 * allocated. Heap-allocates on first use; subsequent opens just refresh
 * the agent list and zero the log. */
static void app_open_create_theme(AppState *app) {
    if (!app->create_theme_agents) {
        app->create_theme_agents =
            (AgentInfo *)calloc((usize)AGENT_MAX, sizeof(AgentInfo));
    }
    /* Defensive: reap any prior child + fd before we overwrite the
     * handles below. With app_close_create_theme now killing on Esc
     * this is rarely hit, but it covers the edge case where the
     * dialog was reopened from a callback path that bypassed
     * close (and on cold open the kill is a no-op). */
    create_theme_kill_child(app);

    if (!app->create_theme_log) {
        /* 256 KB: large enough that a full Markdown note (Create Note keeps the
         * FRONT, where the H1 lives) fits without truncation, while a theme's
         * JSON tail still rolls comfortably within it. */
        app->create_theme_log_cap = 256 * 1024;
        app->create_theme_log = (char *)calloc(1, (usize)app->create_theme_log_cap);
        if (!app->create_theme_log) app->create_theme_log_cap = 0;
    }
    app->create_theme_agent_count =
        app->create_theme_agents
            ? agent_detect_available(app->create_theme_agents, AGENT_MAX)
            : 0;
    app->create_theme_agent_idx = 0;
    app->create_theme_active = true;
    app->create_theme_doc_mode = false;
    app->create_theme_phase  = 0;
    app->create_theme_field  = 0;
    app->create_theme_name[0] = '\0';
    app->create_theme_desc[0] = '\0';
    app->create_theme_name_len = 0;
    app->create_theme_desc_len = 0;
    app->create_theme_status[0] = '\0';
    app->create_theme_error[0]  = '\0';
    app->create_theme_error_until = 0;
    if (app->create_theme_log) app->create_theme_log[0] = '\0';
    app->create_theme_log_len = 0;
    app->create_theme_target_path[0] = '\0';
    app->create_theme_child_pid = 0;
    app->create_theme_stdout_fd = -1;
}

/* Open the same modal in "Create Note" mode — an AI generator that writes a
 * Markdown document into the notes Vault. Reuses the entire Create-Theme
 * pipeline (agent spawn, stdout drain, child reap, modal render); only the
 * prompt, target path, and finalizer branch on create_theme_doc_mode. */
static void app_open_create_doc(AppState *app) {
    app_open_create_theme(app);
    app->create_theme_doc_mode = true;
}

/* Terminate a spawned agent child without ever blocking the UI thread
 * indefinitely. SIGTERM is catchable — a Node/Python agent CLI may install
 * a handler and not exit promptly (or at all), so `waitpid(pid, .., 0)`
 * could hang the whole event loop. Poll non-blocking for a brief window,
 * then escalate to SIGKILL (which cannot be trapped) and reap that. */
static void kill_child_graceful(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 60; i++) {        /* up to ~300 ms */
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return;
        struct timespec ts = { 0, 5 * 1000 * 1000 };   /* 5 ms */
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);                 /* bounded: SIGKILL is uncatchable */
}

/* SIGTERM the running agent (if any) and close pipe. Used by Cancel and
 * by app_destroy. The caller is responsible for clearing the modal flag
 * separately if it wants the dialog to disappear. */
static void create_theme_kill_child(AppState *app) {
    if (app->create_theme_child_pid > 0) {
        kill_child_graceful((pid_t)app->create_theme_child_pid);
        app->create_theme_child_pid = 0;
    }
    if (app->create_theme_stdout_fd >= 0) {
        close(app->create_theme_stdout_fd);
        app->create_theme_stdout_fd = -1;
    }
}

/* Compose the prompt and spawn the agent. Returns true on success.
 * Layout of the spawned command depends on the agent's invoke_kind:
 *   0 — argv[1] = prompt (claude, gemini, cursor-agent, amp)
 *   1 — argv[1] = "exec", argv[2] = prompt (codex)
 *   2 — prompt is fed via stdin (cline)
 * Stdout/stderr are merged into a pipe that the main loop drains every
 * frame. The child's stdin is closed for kinds 0/1 so interactive
 * prompts don't hang the agent silently. */
static bool app_spawn_create_theme(AppState *app) {
    if (app->create_theme_agent_count <= 0) return false;
    if (app->create_theme_agent_idx < 0 ||
        app->create_theme_agent_idx >= app->create_theme_agent_count) return false;

    const AgentInfo *ag = &app->create_theme_agents[app->create_theme_agent_idx];

    /* Cap total prompt at 64 KB so we don't exhaust the agent's argv
     * limit on platforms with small ARG_MAX. */
    usize prompt_cap = 64 * 1024;
    char *prompt = (char *)malloc(prompt_cap);
    if (!prompt) return false;
    int wrote;

    if (app->create_theme_doc_mode) {
        /* Create Note: resolve <vault>/<slug>.md and ask the agent for a
         * raw Markdown document on stdout. As with themes, the agent must
         * NOT touch the filesystem — we write the file ourselves on exit. */
        create_doc_resolve_target(app, app->create_theme_name,
                                  app->create_theme_target_path,
                                  sizeof(app->create_theme_target_path));
        wrote = snprintf(prompt, prompt_cap,
            "You are writing a Markdown document for the user's personal notes "
            "vault in the Liu terminal.\n"
            "\n"
            "Document title: %s\n"
            "What the document should cover: %s\n"
            "\n"
            "OUTPUT FORMAT — STRICT:\n"
            "  - Print ONLY the Markdown document itself, nothing else.\n"
            "  - Begin with a single level-1 heading on the first line: \"# %s\".\n"
            "  - Use clean CommonMark: headings, paragraphs, '-' bullet lists,\n"
            "    numbered lists, > blockquotes, tables, [text](url) links, and\n"
            "    ``` fenced code blocks with a language tag where relevant.\n"
            "  - Write in the same language the user used in the description.\n"
            "  - Do NOT wrap the whole document in an outer code fence.\n"
            "  - Do NOT call any tools, do NOT read or write files, do NOT run "
            "shell commands.\n"
            "  - No preamble, no commentary, no sign-off — the first character "
            "MUST be '#'.\n",
            app->create_theme_name,
            app->create_theme_desc,
            app->create_theme_name);
    } else {
        /* Resolve the JSON destination. We make sure the themes dir exists
         * up front so the agent doesn't have to. */
        create_theme_resolve_target(app->create_theme_name,
                                    app->create_theme_target_path,
                                    sizeof(app->create_theme_target_path));
        /* mkdir -p on the user dir. Failures are ignored — if it exists
         * already, EEXIST is fine; if it can't be created, write will fail
         * with a clear error from the agent. */
        {
            char parent[1024];
            snprintf(parent, sizeof(parent), "%s", theme_user_dir());
            char *sl = strrchr(parent, '/');
            if (sl) { *sl = '\0'; mkdir(parent, 0755); }
            mkdir(theme_user_dir(), 0755);
        }

        /* Build prompt — embed the theming guide if we found it. The agent
         * is told to write the JSON directly to disk and not print it,
         * keeping the captured stdout small and free of confidential
         * content. */
        usize guide_len = 0;
        char *guide = create_theme_read_guide(&guide_len);

        /* The agent must NOT touch the filesystem — many CLI agents need an
         * extra flag (--yolo, --full-auto, ...) to write files in non-
         * interactive mode and silently chat instead of writing. We side-
         * step that entirely by asking for raw JSON on stdout and writing
         * the file ourselves once the agent exits.                          */
        wrote = snprintf(prompt, prompt_cap,
            "You are generating a Liu terminal theme.\n"
            "\n"
            "Theme display name: %s\n"
            "User's visual description: %s\n"
            "\n"
            "OUTPUT FORMAT — STRICT:\n"
            "  - Print ONLY the JSON object, starting with `{` and ending with `}`.\n"
            "  - No markdown fences, no commentary, no explanation, no preamble.\n"
            "  - Do NOT call any tools, do NOT read or write files, do NOT run shell commands.\n"
            "  - The JSON's \"name\" field MUST be exactly \"%s\".\n"
            "  - Every color is a four-element RGBA float array in [0.0, 1.0].\n"
            "  - The first character of your response MUST be `{`.\n"
            "\n"
            "COLOR CORRECTNESS — MANDATORY:\n"
            "  - The guide below contains a section titled \"Required color-pair contrast\".\n"
            "    Every pair listed there is a place the renderer actually paints one\n"
            "    of your colors on top of another. Walk the entire \"Pre-submit\n"
            "    checklist\" silently before emitting the JSON.\n"
            "  - In particular, `ansi_5` and `ansi_4` are used as button fills\n"
            "    behind WHITE labels (Generate / Connect / modal accent strips).\n"
            "    Their luminance MUST be low enough (≤ ~0.55) that white reads.\n"
            "  - Avoid every entry in the \"Forbidden combinations\" table —\n"
            "    no two of those fields may collide.\n"
            "  - If the user's description leans pastel/translucent, use that\n"
            "    palette ONLY for `bg` / `selection` / chrome — keep the ANSI\n"
            "    palette saturated enough to read on the (possibly light) bg.\n"
            "\n"
            "Use the user's description to choose colors. Follow the schema "
            "and the contrast rules from the theming guide below.\n"
            "\n"
            "==== BEGIN THEMING GUIDE ====\n"
            "%s"
            "\n==== END THEMING GUIDE ====\n",
            app->create_theme_name,
            app->create_theme_desc,
            app->create_theme_name,
            guide && guide_len > 0 ? guide :
                "(THEMING.md not found — follow the JSON schema described "
                "above and copy structure from a known terminal theme.)");
        free(guide);
    }
    if (wrote <= 0) { free(prompt); return false; }

    int pipefd[2];
    if (pipe(pipefd) != 0) { free(prompt); return false; }
    /* Close-on-exec on both ends: the agent's stdout/stderr survive via the
     * child's dup2 (which clears CLOEXEC), but the agent — and any other
     * spawn that overlaps — never inherits Liu's other descriptors. */
    (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]); free(prompt); return false;
    }
    if (pid == 0) {
        /* Child — wire stdout/stderr into the pipe; stdin from /dev/null
         * for kinds 0/1, from a memfd-style temp file for kind 2.       */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        if (ag->stdin_prompt) {
            /* Pipe the prompt via stdin. We already grabbed the parent's
             * end for stdout earlier, so create a fresh pipe here. */
            int sp[2];
            if (pipe(sp) == 0) {
                /* Fork a tiny grand-child to write the prompt — keeps
                 * us simple and avoids SIGPIPE if the agent doesn't
                 * read everything. */
                pid_t gc = fork();
                if (gc == 0) {
                    close(sp[0]);
                    (void)!write(sp[1], prompt, strlen(prompt));
                    close(sp[1]);
                    _exit(0);
                }
                close(sp[1]);
                dup2(sp[0], STDIN_FILENO);
                close(sp[0]);
            } else {
                int devnull = open("/dev/null", O_RDONLY);
                if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
            }
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        }

        char *argv[AGENT_MAX_ARGS + 3] = {0};
        i32 ai = 0;
        argv[ai++] = (char *)ag->binary;
        for (i32 k = 0; k < ag->args_count && ai + 1 < (i32)(sizeof argv / sizeof argv[0]); k++) {
            argv[ai++] = (char *)ag->args[k];
        }
        if (!ag->stdin_prompt) {
            argv[ai++] = prompt;
        }
        argv[ai] = NULL;
        execvp(ag->binary, argv);
        /* If exec fails, propagate a brief error to the parent's pipe so
         * the modal can surface it. */
        const char *err = "[liu] failed to exec agent\n";
        (void)!write(STDERR_FILENO, err, strlen(err));
        _exit(127);
    }

    /* Parent — keep the read end open, set non-blocking. */
    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    app->create_theme_child_pid = (i32)pid;
    app->create_theme_stdout_fd = pipefd[0];
    app->create_theme_phase = 1;
    if (app->create_theme_log) app->create_theme_log[0] = '\0';
    app->create_theme_log_len = 0;
    app->create_theme_total_bytes = 0;
    app->create_theme_prompt_bytes = (i64)strlen(prompt);
    app->create_theme_spawn_time  = platform_time_sec();
    snprintf(app->create_theme_status, sizeof(app->create_theme_status),
             app->create_theme_doc_mode
                 ? "Spawned %s — waiting for the document…"
                 : "Spawned %s — waiting for theme JSON…", ag->display);
    free(prompt);
    return true;
}

/* Validate inputs and kick off generation. */
static void app_create_theme_submit(AppState *app) {
    if (app->create_theme_phase != 0) return;

    if (app->create_theme_name_len == 0) {
        create_theme_set_error(app, app->create_theme_doc_mode
            ? "Note title is required." : "Theme name is required.");
        app->create_theme_field = 0; return;
    }
    if (app->create_theme_desc_len == 0) {
        create_theme_set_error(app, app->create_theme_doc_mode
            ? "Describe what the note should cover." : "Visual description is required.");
        app->create_theme_field = 1; return;
    }
    if (app->create_theme_agent_count == 0) {
        create_theme_set_error(app,
            "No CLI agents on PATH — install claude, codex, or copilot first.");
        return;
    }
    if (!app_spawn_create_theme(app)) {
        create_theme_set_error(app, "Failed to spawn agent.");
    }
}

/* Scan a captured agent stdout for the agent's response JSON. Walks the
 * log forward and returns the LAST top-level balanced `{…}` object — not
 * the first. The agent's response always follows any echoed prompt
 * fragments and embedded examples (Codex/Gemini routinely echo the user
 * prompt verbatim, and our prompt embeds THEMING.md which itself
 * contains a full example theme JSON). Picking the first `{` would
 * yank back that embedded example; picking the last one lands on the
 * agent's actual output. Tolerates string-internal braces. */
static bool create_theme_extract_json(const char *log, i32 log_len,
                                      i32 *out_start, i32 *out_len) {
    if (!log || log_len < 2) return false;

    i32 best_start = -1, best_len = 0;
    i32 i = 0;
    while (i < log_len) {
        if (log[i] != '{') { i++; continue; }

        i32 depth = 0;
        bool in_str = false;
        bool escaped = false;
        i32 end = -1;
        for (i32 j = i; j < log_len; j++) {
            char c = log[j];
            if (in_str) {
                if (escaped) { escaped = false; continue; }
                if (c == '\\') { escaped = true; continue; }
                if (c == '"')  { in_str = false; }
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) { end = j; break; }
            }
        }
        if (end >= 0) {
            best_start = i;
            best_len   = (end + 1) - i;
            i = end + 1;
        } else {
            i++;
        }
    }
    if (best_start < 0) return false;
    *out_start = best_start;
    *out_len   = best_len;
    return true;
}

/* Take the captured stdout, extract a JSON object, write it to the
 * resolved theme path, then reload user themes and apply.
 * Returns true on full success (theme is now active). On any failure
 * fills `app->create_theme_status` with a diagnostic. */
static bool create_theme_finalize_from_log(AppState *app) {
    i32 js_start = 0, js_len = 0;
    if (!create_theme_extract_json(app->create_theme_log,
                                   app->create_theme_log_len,
                                   &js_start, &js_len)) {
        snprintf(app->create_theme_status, sizeof(app->create_theme_status),
                 "Agent did not emit a JSON object on stdout.");
        return false;
    }

    /* Make sure the user themes dir exists. */
    {
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s", theme_user_dir());
        char *sl = strrchr(parent, '/');
        if (sl) { *sl = '\0'; mkdir(parent, 0755); }
        mkdir(theme_user_dir(), 0755);
    }

    /* Force-set the "name" field to the user's typed display name —
     * Codex / Gemini routinely slugify, lowercase, or skip it. The
     * agent's job is just colors; metadata is ours. */
    cJSON *root = cJSON_ParseWithLength(app->create_theme_log + js_start,
                                        (usize)js_len);
    if (!root) {
        snprintf(app->create_theme_status, sizeof(app->create_theme_status),
                 "Agent output is not valid JSON.");
        return false;
    }
    cJSON *fixed_name = cJSON_CreateString(app->create_theme_name);
    if (!fixed_name) { cJSON_Delete(root); return false; }
    if (cJSON_GetObjectItemCaseSensitive(root, "name")) {
        cJSON_ReplaceItemInObjectCaseSensitive(root, "name", fixed_name);
    } else {
        cJSON_AddItemToObject(root, "name", fixed_name);
    }

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) {
        snprintf(app->create_theme_status, sizeof(app->create_theme_status),
                 "Failed to serialize patched theme JSON.");
        return false;
    }

    bool ok = false;
    FILE *f = fopen(app->create_theme_target_path, "w");
    if (f) {
        usize out_len = strlen(out);
        ok = (fwrite(out, 1, out_len, f) == out_len);
        fclose(f);
    }
    free(out);
    if (!f) {
        snprintf(app->create_theme_status, sizeof(app->create_theme_status),
                 "Could not open %s for writing.",
                 app->create_theme_target_path);
        return false;
    }
    if (!ok) {
        snprintf(app->create_theme_status, sizeof(app->create_theme_status),
                 "Short write to %s.", app->create_theme_target_path);
        return false;
    }

    /* Reload and look up by the user-supplied name — we just set it, so
     * a miss here means the JSON structure was rejected by the theme
     * loader (e.g. missing required color fields). */
    theme_load_user_themes();
    const Theme *nt = theme_get_by_name(app->create_theme_name);
    if (!nt || strcmp(nt->name, app->create_theme_name) != 0) {
        snprintf(app->create_theme_status, sizeof(app->create_theme_status),
                 "Wrote %s but the loader rejected its structure.",
                 app->create_theme_target_path);
        return false;
    }
    app_switch_theme(app, nt);
    return true;
}

/* Create-Note finalizer: take the captured stdout as a Markdown document and
 * write it verbatim to <vault>/<slug>.md. Unlike the theme path there is no
 * schema — we only trim agent chatter: skip leading whitespace, unwrap a
 * single outer ``` fence if one was added, and start at the first heading
 * line when the agent prefixed a sentence before the document. */
static bool create_doc_finalize_from_log(AppState *app) {
    const char *log = app->create_theme_log;
    i32 len = app->create_theme_log_len;
    if (!log || len <= 0) {
        snprintf(app->create_theme_status, sizeof(app->create_theme_status),
                 "Agent produced no output.");
        return false;
    }

    i32 s = 0, e = len;
    while (s < e && (log[s]==' '||log[s]=='\t'||log[s]=='\r'||log[s]=='\n')) s++;

    /* Unwrap a single outer ```…``` fence if the agent wrapped the doc. */
    if (e - s >= 3 && log[s]=='`' && log[s+1]=='`' && log[s+2]=='`') {
        i32 nl = s;
        while (nl < e && log[nl] != '\n') nl++;
        if (nl < e) {
            i32 inner = nl + 1;
            i32 te = e;
            while (te > inner && (log[te-1]==' '||log[te-1]=='\t'||
                                  log[te-1]=='\r'||log[te-1]=='\n')) te--;
            if (te - inner >= 3 && log[te-1]=='`' && log[te-2]=='`' && log[te-3]=='`') {
                te -= 3;
                while (te > inner && (log[te-1]=='\r'||log[te-1]=='\n')) te--;
                s = inner; e = te;
            }
        }
    }

    /* If a prose lead-in precedes the document, skip forward to the first
     * heading. Only do this when the content actually starts with a prose
     * sentence (an ASCII letter) — when it already begins with '#', '---'
     * frontmatter, a list, a quote, or a code fence we keep it verbatim so a
     * legitimate non-heading opener isn't discarded. */
    {
        char c0 = log[s];
        bool prose_leadin = (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z');
        if (prose_leadin) {
            /* log[s] is a letter, so the first heading is necessarily a
             * line-start at some i > s — i-1 is always in range here. */
            for (i32 i = s + 1; i + 1 < e; i++) {
                if (log[i-1]=='\n' && log[i]=='#' &&
                    (log[i+1]==' '||log[i+1]=='#')) { s = i; break; }
            }
        }
    }

    if (e <= s) {
        snprintf(app->create_theme_status, sizeof(app->create_theme_status),
                 "Agent did not emit a Markdown document.");
        return false;
    }

    FILE *f = fopen(app->create_theme_target_path, "w");
    if (!f) {
        snprintf(app->create_theme_status, sizeof(app->create_theme_status),
                 "Could not open %s for writing.", app->create_theme_target_path);
        return false;
    }
    usize n = (usize)(e - s);
    bool ok = (fwrite(log + s, 1, n, f) == n);
    if (ok && log[e-1] != '\n') ok = (fputc('\n', f) != EOF);  /* trailing newline */
    /* If the capture buffer overflowed (output exceeded its 256 KB cap), the
     * note is a clean prefix but incomplete — record that in-band so it isn't
     * silently wrong. total_bytes counts everything the agent emitted. */
    if (ok && app->create_theme_total_bytes > (i64)app->create_theme_log_len) {
        (void)fputs("\n<!-- liu: note truncated — agent output exceeded the "
                    "capture buffer -->\n", f);
    }
    fclose(f);
    if (!ok) {
        snprintf(app->create_theme_status, sizeof(app->create_theme_status),
                 "Short write to %s.", app->create_theme_target_path);
        return false;
    }
    return true;
}

/* Drain the agent's stdout, reap the child, finalize on exit.
 * Called once per frame from the main loop while the dialog is open. */
/* Drain everything available on the agent's stdout into the rolling log
 * buffer. The buffer rolls because chatty agents (Claude prints reasoning,
 * tool calls, etc.) produce far more output than the theme JSON itself —
 * the JSON always lives at the tail. Cumulative bytes is tracked
 * separately so the UI's "12.3 KB" readout doesn't get clamped at cap. */
static void create_theme_drain_stdout(AppState *app) {
    if (app->create_theme_stdout_fd < 0 || !app->create_theme_log) return;
    char buf[1024];
    for (;;) {
        ssize_t n = read(app->create_theme_stdout_fd, buf, sizeof(buf));
        if (n <= 0) break;
        app->create_theme_total_bytes += (i64)n;
        i32 cap = app->create_theme_log_cap - 1;
        i32 cur = app->create_theme_log_len;
        i32 incoming = (i32)n;
        if (app->create_theme_doc_mode) {
            /* Create Note: the load-bearing content (the H1 + top of the
             * document) is at the FRONT, so keep the head — once the buffer
             * is full, drop further input instead of evicting the start.
             * total_bytes still counts everything so finalize can tell the
             * note was truncated and mark it in-band. */
            i32 copy = incoming;
            if (copy > cap - cur) copy = cap - cur;
            if (copy > 0) {
                memcpy(app->create_theme_log + cur, buf, (usize)copy);
                cur += copy;
                app->create_theme_log[cur] = '\0';
                app->create_theme_log_len = cur;
            }
            continue;
        }
        /* Theme mode: the JSON object lives at the TAIL, so keep the tail by
         * rolling the front off when full. */
        if (cur + incoming > cap) {
            i32 drop = cur + incoming - cap;
            if (drop > cur) drop = cur;
            memmove(app->create_theme_log,
                    app->create_theme_log + drop,
                    (usize)(cur - drop));
            cur -= drop;
        }
        i32 copy = incoming;
        if (copy > cap - cur) copy = cap - cur;
        memcpy(app->create_theme_log + cur, buf, (usize)copy);
        cur += copy;
        app->create_theme_log[cur] = '\0';
        app->create_theme_log_len = cur;
    }
}

/* Strip ANSI escape sequences (CSI/OSC) and control characters from
 * [src, src+len), collapse internal whitespace runs to a single space, and
 * trim both ends. Writes a NUL-terminated single-line result of at most
 * cap-1 bytes to `out`. UTF-8 lead/continuation bytes (>= 0x80) pass
 * through untouched so multibyte glyphs survive. */
static void create_theme_sanitize_line(const char *src, i32 len,
                                        char *out, usize cap) {
    if (cap == 0) return;
    usize o = 0;
    bool seen = false;           /* emitted a non-space char yet?          */
    bool pending_space = false;  /* a space owed before the next real char */
    for (i32 i = 0; i < len && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0x1b) {                                /* ESC — skip seq    */
            if (i + 1 < len && src[i + 1] == '[') {            /* CSI: …final */
                i += 2;
                while (i < len && !((unsigned char)src[i] >= 0x40 &&
                                    (unsigned char)src[i] <= 0x7e)) i++;
            } else if (i + 1 < len && src[i + 1] == ']') {     /* OSC: …ST    */
                i += 2;
                while (i < len && (unsigned char)src[i] != 0x07 &&
                                  (unsigned char)src[i] != 0x1b) i++;
            }
            continue;
        }
        if (c < 0x20 || c == 0x7f) { if (seen) pending_space = true; continue; }
        if (c == ' ' || c == '\t') { if (seen) pending_space = true; continue; }
        if (pending_space) { out[o++] = ' '; pending_space = false;
                             if (o + 1 >= cap) break; }
        out[o++] = (char)c;
        seen = true;
    }
    out[o] = '\0';
}

/* Derive a single-line "what is the agent doing right now" status from the
 * live stdout tail + timing and store it in create_theme_status (rendered
 * under the spinner while phase == 1).
 *
 * The agent (claude / codex / gemini …) streams to stdout as it works, so
 * the last non-empty line of that stream is its current activity. Once it
 * starts emitting the theme JSON the tail is just braces, so we swap in a
 * friendly phase message + byte counter rather than show raw JSON. */
static void create_theme_update_live_status(AppState *app) {
    const char *agent = "Agent";
    if (app->create_theme_agents &&
        app->create_theme_agent_idx >= 0 &&
        app->create_theme_agent_idx < app->create_theme_agent_count) {
        agent = app->create_theme_agents[app->create_theme_agent_idx].display;
    }

    f64 elapsed = platform_time_sec() - app->create_theme_spawn_time;
    if (elapsed < 0) elapsed = 0;

    char *st = app->create_theme_status;
    usize stcap = sizeof(app->create_theme_status);

    /* Nothing on stdout yet — the agent is still spinning up / thinking. */
    if (app->create_theme_total_bytes == 0 ||
        !app->create_theme_log || app->create_theme_log_len == 0) {
        snprintf(st, stcap, "%s is thinking… (%.0fs)", agent, elapsed);
        return;
    }

    /* Last non-empty line in the rolling log = the agent's current output. */
    const char *log = app->create_theme_log;
    i32 end = app->create_theme_log_len;
    while (end > 0 && (log[end - 1] == '\n' || log[end - 1] == '\r' ||
                       log[end - 1] == ' '  || log[end - 1] == '\t')) end--;
    i32 start = end;
    while (start > 0 && log[start - 1] != '\n' && log[start - 1] != '\r') start--;

    char line[200];
    create_theme_sanitize_line(log + start, end - start, line, sizeof(line));

    char sz[24];
    i64 b = app->create_theme_total_bytes;
    if (b < 1024) snprintf(sz, sizeof(sz), "%lld B", (long long)b);
    else          snprintf(sz, sizeof(sz), "%.1f KB", (double)b / 1024.0);

    char f = line[0];
    if (app->create_theme_doc_mode) {
        /* Markdown has no single leading sentinel; just show the rolling
         * tail line, or a generic "writing" hint while output is sparse. */
        if (line[0]) snprintf(st, stcap, "%s", line);
        else         snprintf(st, stcap, "%s is writing the note… (%s)", agent, sz);
        return;
    }
    bool looks_json = (f == '\0' || f == '{' || f == '}' || f == '[' ||
                       f == ']'  || f == '"' || f == ',' || f == ':');
    if (looks_json)
        snprintf(st, stcap, "%s is writing the theme… (%s)", agent, sz);
    else
        snprintf(st, stcap, "%s", line);
}

/* Per-frame auto-updater pump: advance the state machine, mirror its state into
 * the Settings About tab, consume the click-handler request flags, and surface a
 * background-check toast. The relaunch (process swap) is handled by the main
 * loop seeing app->updater.relaunch_requested. */
static void app_tick_updater(AppState *app) {
    UpdateState *u = &app->updater;
    SettingsPanel *sp = &app->settings;

    /* Notes Vault folder changed in Settings → seed the new root and re-export
     * $LIU_VAULT so terminal agents spawned afterwards write to the new place.
     * Handled before the updater-init gate so it works even if the updater
     * subsystem is disabled or failed to initialise — the two are unrelated. */
    if (sp->requests_vault_resync) {
        sp->requests_vault_resync = false;
        app_notes_vault_ensure(app);
        app_export_vault_env(app);
    }

    if (!u->initialized) return;
    updater_tick(u);

    int uphase = atomic_load(&u->phase);
    sp->update_phase = uphase;
    /* Only mirror the string-heavy detail when the About tab could actually
     * show it (panel open, or a check/download in flight) — skips six snprintf
     * on every idle main-loop wake. */
    if (app->settings.open || uphase != UPD_IDLE) {
        sp->update_auto_install = u->auto_install_allowed;
        snprintf(sp->update_status, sizeof sp->update_status, "%s", u->status);
        snprintf(sp->update_err, sizeof sp->update_err, "%s", u->err);
        snprintf(sp->update_avail_version, sizeof sp->update_avail_version, "%s", u->avail_version);
        snprintf(sp->update_notes, sizeof sp->update_notes, "%s", u->avail_notes);
        sp->update_bytes_done  = atomic_load(&u->bytes_done);
        sp->update_bytes_total = atomic_load(&u->bytes_total);
    }

    if (sp->requests_update_check)   { sp->requests_update_check = false;   updater_begin_check(u, false); }
    if (sp->requests_update_install) { sp->requests_update_install = false; updater_begin_install(u); }

    if (u->toast_pending) { u->toast_pending = false; app_show_toast(app, u->toast_msg); }
}

static void app_tick_create_theme(AppState *app) {
    if (!app->create_theme_active || app->create_theme_phase != 1) return;

    /* Throttle to ~10 Hz — without this, every vsync (120 Hz on
     * ProMotion) issues a `read()` + `waitpid(WNOHANG)` syscall pair
     * even though the UI only needs occasional progress updates. */
    static f64 last_tick = 0;
    f64 now = platform_time_sec();
    if (now - last_tick < 0.1) return;
    last_tick = now;

    create_theme_drain_stdout(app);
    create_theme_update_live_status(app);

    /* Has the child exited? */
    bool child_exited = false;
    int wstatus = 0;
    if (app->create_theme_child_pid > 0) {
        pid_t wp = waitpid(app->create_theme_child_pid, &wstatus, WNOHANG);
        if (wp == app->create_theme_child_pid) {
            child_exited = true;
        }
    }
    if (!child_exited) return;

    create_theme_drain_stdout(app);
    if (app->create_theme_stdout_fd >= 0) {
        close(app->create_theme_stdout_fd);
        app->create_theme_stdout_fd = -1;
    }
    app->create_theme_child_pid = 0;

    /* Try to extract + write the theme. Outcome is surfaced as a toast
     * and the dialog auto-closes — the modal no longer carries a
     * status panel, only the in-progress spinner. */
    bool doc = app->create_theme_doc_mode;
    bool finalized = doc ? create_doc_finalize_from_log(app)
                         : create_theme_finalize_from_log(app);
    if (finalized) {
        char msg[200];
        if (doc)
            snprintf(msg, sizeof(msg),
                     "Note \"%s\" saved to Vault.", app->create_theme_name);
        else
            snprintf(msg, sizeof(msg),
                     "Theme \"%s\" created.", app->create_theme_name);
        app_show_toast(app, msg);
        /* Open the freshly-written note so the user lands on it directly. */
        if (doc && app->create_theme_target_path[0])
            app_open_markdown_viewer_tab(app, app->create_theme_target_path);
    } else {
        char tail[64];
        if (WIFEXITED(wstatus)) {
            snprintf(tail, sizeof(tail), " (exit %d)", WEXITSTATUS(wstatus));
        } else {
            snprintf(tail, sizeof(tail), " (terminated)");
        }
        usize l = strlen(app->create_theme_status);
        if (l + strlen(tail) + 1 < sizeof(app->create_theme_status)) {
            memcpy(app->create_theme_status + l, tail, strlen(tail) + 1);
        }
        char err_toast[256];
        snprintf(err_toast, sizeof(err_toast),
                 doc ? "Note generation failed: %s"
                     : "Theme generation failed: %s",
                 app->create_theme_status[0] ? app->create_theme_status
                                             : "unknown error");
        app_show_toast(app, err_toast);
    }
    /* Close the dialog regardless of outcome — toast carries the
     * result, no need to leave a Close/Back button on screen. Phase
     * stays at 1 so the spinner keeps drawing during the close-anim
     * fade-out; the open handler resets phase to 0 on next entry.
     *
     * Release the 64 KB log scratch buffer now that we've extracted
     * the JSON. db7ade7 removed the verbose log panel that used to
     * keep this buffer relevant past finalize, so holding it pinned
     * until app_destroy is pure dead weight on long-running
     * sessions. Next open re-allocates fresh. */
    app->create_theme_active  = false;
    app->create_theme_log_len = 0;
    free(app->create_theme_log);
    app->create_theme_log     = NULL;
    app->create_theme_log_cap = 0;
}

/* =========================================================================
 * Translate-on-Tab
 * Prompt-line Ctrl+Cmd×2 within tab_window_sec while a known agent (claude /
 * codex / gemini / opencode) is the foreground process. On the second
 * Ctrl+Cmd chord we spawn the configured backend, drain its stdout, then
 * replace the captured prompt text by:
 *   1. counting codepoints from the prompt start to the cursor,
 *   2. sending that many Backspace keypresses (via terminal_key_input so
 *      modes like MODE_APP_CURSOR / Liu-kbd flags are honoured),
 *   3. emitting the translation via bracketed paste or streaming writes.
 * ========================================================================= */

static void app_translate_kill_child(AppState *app) {
    if (app->translate_child_pid > 0) {
        kill_child_graceful((pid_t)app->translate_child_pid);
        app->translate_child_pid = 0;
    }
    if (app->translate_stdout_fd >= 0) {
        close(app->translate_stdout_fd);
        app->translate_stdout_fd = -1;
    }
}

static bool app_translate_target_alive(AppState *app, Terminal *t, Session *s) {
    if (!app || !t || !s) return false;
    for (i32 i = 0; i < app->tab_count; i++) {
        Tab *tab = &app->tabs[i];
        if (tab->terminal == t && tab->session == s) return true;
        if (tab->terminal2 == t && tab->session2 == s) return true;
        for (i32 p = 2; p < MAX_SPLIT_PANES; p++) {
            if (tab->extra_terminals[p - 2] == t &&
                tab->extra_sessions[p - 2] == s) return true;
        }
    }
    return false;
}

static void app_translate_clear_pending_chord(AppState *app) {
    app->translate_tab_pending = 0;
    app->translate_tab_first_time = 0;
    app->translate_tab_last_time = 0;
    app->translate_tab_target_term = NULL;
    app->translate_tab_target_sess = NULL;
}

static bool app_translate_is_modifier_key(u32 key) {
    return key == KEY_LSHIFT || key == KEY_RSHIFT ||
           key == KEY_LCTRL  || key == KEY_RCTRL  ||
           key == KEY_LALT   || key == KEY_RALT   ||
           key == KEY_LSUPER || key == KEY_RSUPER;
}

static bool app_translate_is_ctrl_cmd_key(u32 key) {
    return key == KEY_LCTRL || key == KEY_RCTRL ||
           key == KEY_LSUPER || key == KEY_RSUPER;
}

static bool app_translate_ctrl_cmd_down(u32 mods) {
    return (mods & (MOD_CTRL | MOD_SUPER)) == (MOD_CTRL | MOD_SUPER) &&
           !(mods & (MOD_SHIFT | MOD_ALT));
}

static bool app_translate_is_border_cp(u32 cp) {
    return (cp >= 0x2500 && cp <= 0x257F) || cp == '|';
}

static bool app_translate_is_prompt_sigil(u32 cp) {
    return cp == '>' || cp == '$' || cp == '#' ||
           cp == 0x276F /* heavy > */ ||
           cp == 0x203A /* angle > */ ||
           cp == 0x25B6 /* play */ ||
           cp == 0x2022 /* bullet */;
}

static bool app_translate_extract_range(const u32 *cps, i32 ncp,
                                        bool require_sigil,
                                        i32 *out_start, i32 *out_fin) {
    i32 start = 0;
    while (start < ncp &&
           (cps[start] <= ' ' || app_translate_is_border_cp(cps[start]))) {
        start++;
    }

    i32 fin = ncp;
    while (fin > start &&
           (cps[fin - 1] <= ' ' || app_translate_is_border_cp(cps[fin - 1]))) {
        fin--;
    }
    if (fin <= start) return false;

    i32 sigil = -1;
    i32 sigil_scan_end = start + 24;
    if (sigil_scan_end > fin) sigil_scan_end = fin;
    for (i32 i = start; i < sigil_scan_end; i++) {
        if (app_translate_is_prompt_sigil(cps[i])) sigil = i;
    }
    if (sigil >= 0) {
        start = sigil + 1;
        while (start < fin && cps[start] <= ' ') start++;
    } else if (require_sigil) {
        return false;
    }

    if (fin <= start) return false;
    *out_start = start;
    *out_fin = fin;
    return true;
}

static i32 app_translate_row_cps(Terminal *t, i32 row, i32 end_col,
                                 u32 *cps, i32 cap) {
    if (!t || !cps || cap <= 0 || row < 0 || row >= t->rows) return 0;
    if (end_col <= 0) return 0;
    if (end_col > t->cols) end_col = t->cols;
    i32 ncp = 0;
    for (i32 col = 0; col < end_col && ncp < cap; col++) {
        Cell *c = terminal_cell_at(t, col, row);
        if (!c || (c->attr.flags & ATTR_WDUMMY)) continue;
        cps[ncp++] = c->codepoint ? c->codepoint : ' ';
    }
    return ncp;
}

static bool app_translate_cps_bounds(const u32 *cps, i32 ncp,
                                     i32 *out_start, i32 *out_fin) {
    i32 start = 0;
    while (start < ncp &&
           (cps[start] <= ' ' || app_translate_is_border_cp(cps[start]))) {
        start++;
    }
    i32 fin = ncp;
    while (fin > start &&
           (cps[fin - 1] <= ' ' || app_translate_is_border_cp(cps[fin - 1]))) {
        fin--;
    }
    if (fin <= start) return false;
    *out_start = start;
    *out_fin = fin;
    return true;
}

static bool app_translate_append_cp(char *out, usize out_cap, usize *len,
                                    u32 cp) {
    if (!out || !len || out_cap == 0) return false;
    u8 enc[4];
    u32 n = utf8_encode(cp, enc);
    if (n == 0 || *len + (usize)n + 1 > out_cap) return false;
    memcpy(out + *len, enc, (usize)n);
    *len += (usize)n;
    out[*len] = '\0';
    return true;
}

static bool app_translate_capture_row(Terminal *t, i32 row, i32 end_col,
                                      bool require_sigil, bool erase_to_end,
                                      char *out, usize out_cap, i32 *out_bs) {
    if (!t || row < 0 || row >= t->rows || end_col <= 0) return false;
    if (end_col > t->cols) end_col = t->cols;

    u32 cps[512];
    i32 ncp = app_translate_row_cps(t, row, end_col, cps,
                                    (i32)(sizeof cps / sizeof cps[0]));

    i32 start = 0, fin = 0;
    if (!app_translate_extract_range(cps, ncp, require_sigil, &start, &fin)) {
        return false;
    }

    usize len = 0;
    for (i32 i = start; i < fin; i++) {
        u8 enc[4];
        u32 n = utf8_encode(cps[i], enc);
        if (n == 0 || len + (usize)n + 1 > out_cap) break;
        memcpy(out + len, enc, (usize)n);
        len += (usize)n;
    }
    out[len] = '\0';
    if (out_bs) *out_bs = (erase_to_end ? (ncp - start) : (fin - start));
    return len > 0;
}

static bool app_translate_capture_visual_prompt(Terminal *t, char *out,
                                                usize out_cap, i32 *out_bs) {
    if (!t || !out || out_cap < 8) return false;
    i32 cursor_row = t->cursor_y;
    if (cursor_row < 0 || cursor_row >= t->rows) cursor_row = t->rows - 1;
    i32 first = cursor_row - 10;
    if (first < 0) first = 0;

    u32 cps[512];
    i32 start_row = -1;
    i32 start_cp = 0;
    for (i32 r = cursor_row; r >= first; r--) {
        i32 ncp = app_translate_row_cps(t, r, t->cols, cps,
                                        (i32)(sizeof cps / sizeof cps[0]));
        i32 s = 0, f = 0;
        if (app_translate_extract_range(cps, ncp, true, &s, &f)) {
            start_row = r;
            start_cp = s;
            break;
        }
    }
    if (start_row < 0) {
        i32 r = cursor_row;
        while (r > first) {
            i32 ncp = app_translate_row_cps(t, r - 1, t->cols, cps,
                                            (i32)(sizeof cps / sizeof cps[0]));
            i32 s = 0, f = 0;
            if (!app_translate_cps_bounds(cps, ncp, &s, &f)) break;
            r--;
        }
        i32 ncp = app_translate_row_cps(t, r, t->cols, cps,
                                        (i32)(sizeof cps / sizeof cps[0]));
        i32 s = 0, f = 0;
        if (app_translate_cps_bounds(cps, ncp, &s, &f)) {
            start_row = r;
            start_cp = s;
        }
    }
    if (start_row < 0 || start_row > cursor_row) return false;

    usize len = 0;
    i32 bs = 0;
    u32 prev_cp = 0;
    for (i32 r = start_row; r <= cursor_row; r++) {
        i32 ncp = app_translate_row_cps(t, r, t->cols, cps,
                                        (i32)(sizeof cps / sizeof cps[0]));
        i32 s = 0, f = 0;
        if (r == start_row) {
            s = start_cp;
            f = ncp;
            while (f > s &&
                   (cps[f - 1] <= ' ' || app_translate_is_border_cp(cps[f - 1]))) {
                f--;
            }
        } else if (!app_translate_cps_bounds(cps, ncp, &s, &f)) {
            continue;
        }
        if (f <= s) continue;

        u32 first_cp = cps[s];
        if (len > 0 && prev_cp > ' ' && first_cp > ' ' &&
            prev_cp != '-' && prev_cp != '/' && first_cp != '.' &&
            first_cp != ',' && first_cp != ';' && first_cp != ':') {
            if (len + 2 < out_cap) out[len++] = ' ';
        }
        for (i32 i = s; i < f; i++) {
            if (!app_translate_append_cp(out, out_cap, &len, cps[i])) break;
            if (cps[i] > ' ') prev_cp = cps[i];
            bs++;
        }
    }
    out[len] = '\0';
    while (len > 0 && (unsigned char)out[len - 1] <= ' ') out[--len] = '\0';
    if (out_bs) *out_bs = bs;
    return len > 0 && bs > 0;
}

/* Capture the agent prompt's current input line. Native shell-style prompts
 * use the real terminal cursor; full-screen TUIs such as opencode can draw
 * their own cursor, so we fall back to scanning the bottom prompt rows. */
static bool app_translate_capture_line(Terminal *t, char *out, usize out_cap,
                                       i32 *out_bs) {
    if (out_cap) out[0] = '\0';
    if (out_bs) *out_bs = 0;
    if (!t || out_cap < 8) return false;

    if (app_translate_capture_visual_prompt(t, out, out_cap, out_bs)) {
        return true;
    }

    i32 row = t->cursor_y;
    i32 end = t->cursor_x;
    if (end > t->cols) end = t->cols;
    if (row >= 0 && row < t->rows && end > 0) {
        if (app_translate_capture_row(t, row, end, false, true,
                                      out, out_cap, out_bs)) {
            return true;
        }
    }

    i32 first = t->rows - 10;
    if (first < 0) first = 0;
    for (i32 r = t->rows - 1; r >= first; r--) {
        if (app_translate_capture_row(t, r, t->cols, true, false,
                                      out, out_cap, out_bs)) {
            return true;
        }
    }
    return false;
}

static bool app_translate_is_pasted_placeholder(const char *s) {
    if (!s || !s[0]) return false;
    char lower[256];
    i32 n = 0;
    for (const char *p = s; *p && n < (i32)sizeof(lower) - 1; p++) {
        unsigned char c = (unsigned char)*p;
        lower[n++] = (char)((c >= 'A' && c <= 'Z') ? (c + 32) : c);
    }
    lower[n] = '\0';
    return strstr(lower, "pasted text") != NULL ||
           strstr(lower, "pasted") != NULL;
}

static bool app_translate_use_recent_paste(AppState *app, Terminal *t, Session *s,
                                           char *out, usize out_cap, i32 *out_bs) {
    if (!app || !t || !s || !out || out_cap == 0) return false;
    if (!app->translate_recent_paste[0]) return false;
    if (app->translate_recent_paste_term != t ||
        app->translate_recent_paste_sess != s) return false;
    if (platform_time_sec() - app->translate_recent_paste_time > 300.0) return false;

    snprintf(out, out_cap, "%s", app->translate_recent_paste);
    if (out_bs) {
        usize bytes = strlen(app->translate_recent_paste);
        usize cps = utf8_len((const u8 *)app->translate_recent_paste, bytes);
        *out_bs = cps > 0 ? (i32)cps : 1;
    }
    return out[0] != '\0';
}

static void app_translate_debug_log(const char *fmt, ...) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;

    char state_dir[1024];
    char liu_dir[1024];
    char log_path[1200];
    snprintf(state_dir, sizeof state_dir, "%s/.local/state", home);
    snprintf(liu_dir, sizeof liu_dir, "%s/liu", state_dir);
    snprintf(log_path, sizeof log_path, "%s/translate.log", liu_dir);
    (void)mkdir(state_dir, 0755);
    (void)mkdir(liu_dir, 0755);

    FILE *f = fopen(log_path, "a");
    if (!f) return;

    time_t now = time(NULL);
    struct tm tmv;
    char ts[32] = {0};
    if (localtime_r(&now, &tmv)) {
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    } else {
        snprintf(ts, sizeof ts, "unknown-time");
    }
    fprintf(f, "[%s] ", ts);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Erase the captured prompt text by emitting translate_typed_bs
 * Backspaces. The capture is column-based, so there is no selection. */
static void app_translate_delete_typed(AppState *app, Terminal *t, Session *s) {
    i32 n = app->translate_typed_bs;
    if (n <= 0) return;
    u8 endbuf[16];
    i32 endn = terminal_key_input(t, KEY_END, 0, endbuf, sizeof endbuf);
    if (endn > 0) app_send_input(app, s, endbuf, endn);
    u8 bsbuf[16];
    i32 bsn = terminal_key_input(t, KEY_BACKSPACE, 0, bsbuf, sizeof bsbuf);
    if (bsn > 0) {
        for (i32 i = 0; i < n; i++) app_send_input(app, s, bsbuf, bsn);
    }
    app_translate_shadow_clear(app);
}

/* Translate writes generated text as normal terminal input, not bracketed
 * paste, so TUIs do not wrap it as "Pasted text". */
static void app_translate_close_stream(AppState *app) {
    (void)app;
}

/* Shared end-of-translation state reset (all backends). */
static void app_translate_reset(AppState *app) {
    app->translate_active = false;
    app->translate_cancel_requested = false;
    app->translate_local_inflight = false;
    app->translate_api_inflight = false;
    app->translate_api_provider[0] = '\0';
    app->translate_write_started = false;
    app->translate_stream_line_len = 0;
    app->translate_stream_ready = false;
    app->translate_stream_emitted = false;
    app->translate_agent_first_bytes_announced = false;
    app->translate_seg_count = 0;
    app->translate_seg_cur = 0;
    app->translate_seg_text_emitted = false;
    app->translate_seg_pending_ws = false;
    app->translate_target_term = NULL;
    app->translate_target_sess = NULL;
    app->translate_fg_agent_id[0] = '\0';
    /* Drop any unconsumed translate-cleanup snapshot so a later API/local run
     * (which never snapshots) can't trigger a stale agent-store cleanup. */
    app->translate_cleanup_snap.valid = false;
    app->translate_cleanup_agent[0] = '\0';
    /* Drop the 64 KB debug-log scratch between translations instead of
     * pinning it for the app's lifetime; the start paths re-allocate it
     * lazily and the streaming append guards on NULL. */
    app->translate_log_len = 0;
    free(app->translate_log);
    app->translate_log     = NULL;
    app->translate_log_cap = 0;
}

static void app_translate_cancel(AppState *app) {
    app_translate_kill_child(app);
#ifdef LIU_HAVE_LOCAL_LLM
    /* If a local generation is in flight (user cancel or target tab died),
     * stop it promptly instead of letting it run to completion unwatched. */
    if (app->translate_local_inflight) translate_local_cancel();
#endif
    app_translate_close_stream(app);
    /* Segmented run: put the untranslated remainder back so a cancelled
     * run never eats part of the prompt. (Emits no-op if the target died.) */
    if (app->translate_seg_count > 0) app_translate_segments_restore_rest(app);
    app_translate_reset(app);
    app->toast_start_time = 0;
}

/* Human-friendly agent name used in Translate status toasts. The CLI ids
 * are lowercase ("claude" / "gemini" / …) but the user-facing notifications
 * should match what the agent actually calls itself. */
static const char *app_translate_agent_display(const char *id) {
    if (!id || !id[0]) return "Agent";
    if (strcmp(id, "claude") == 0)    return "Claude Code";
    if (strcmp(id, "opencode") == 0)  return "OpenCode";
    if (strcmp(id, "codex") == 0)     return "Codex";
    if (strcmp(id, "amp") == 0)       return "Amp";
    if (strcmp(id, "cline") == 0)     return "Cline";
    if (strcmp(id, "cursor") == 0)    return "Cursor";
    return id;
}

/* Who is doing the work right now — the API provider when the API child
 * is in flight, else the agent CLI. Used by every status toast; call
 * BEFORE app_translate_reset (which clears the inflight flag). */
static const char *app_translate_active_display(AppState *app) {
    if (app->translate_api_inflight) {
        /* Snapshot taken at spawn so a mid-flight provider change in
         * Settings can't relabel the in-flight request. */
        return translate_api_provider_display(app->translate_api_provider);
    }
    return app_translate_agent_display(
        app->translate_fg_agent_id[0] ? app->translate_fg_agent_id
                                      : app->config.translate.agent_id);
}

static bool app_translate_begin_agent_backend(AppState *app, const char *typed) {
    if (!app || !typed || !typed[0]) return false;

    /* Rolling log buffer (64 KB) - only the agent backend needs it:
     * agents echo preamble that translate_agent_finalize strips. */
    if (!app->translate_log) {
        app->translate_log_cap = 64 * 1024;
        app->translate_log = (char *)calloc(1, (usize)app->translate_log_cap);
        if (!app->translate_log) app->translate_log_cap = 0;
    }
    if (app->translate_log) app->translate_log[0] = '\0';
    app->translate_log_len = 0;

    TranslateConfig cfg = app->config.translate;
    TranslateConfig fallback_cfg = cfg;
    if (app->translate_fg_agent_id[0]) {
        snprintf(cfg.agent_id, sizeof(cfg.agent_id), "%s",
                 app->translate_fg_agent_id);
    }
    app_translate_debug_log("start backend=agent agent=%s fallback_agent=%s typed=\"%.240s\"",
                            cfg.agent_id, fallback_cfg.agent_id, typed);

    /* grok/opencode have no flag to suppress the throwaway translate transcript
     * (claude/codex do, applied in translate_agent_spawn). Snapshot that agent's
     * session store BEFORE the child runs — and record the cwd it will inherit
     * (the child does not chdir) — so the post-exit pass deletes only the new
     * one-shot session. Captured for the primary agent; the rare spawn-fallback
     * to a different agent skips cleanup (snapshot/agent mismatch → safe no-op).*/
    app->translate_spawn_time = platform_time_sec();
    if (!getcwd(app->translate_cleanup_cwd, sizeof app->translate_cleanup_cwd))
        app->translate_cleanup_cwd[0] = '\0';
    snprintf(app->translate_cleanup_agent, sizeof app->translate_cleanup_agent,
             "%s", cfg.agent_id);
    translate_cleanup_snapshot(cfg.agent_id, app->translate_cleanup_cwd,
                               &app->translate_cleanup_snap);

    TranslateAgentSpawn sp = {0};
    bool ok = translate_agent_spawn(&cfg, typed, &sp);
    if (!ok && strcmp(fallback_cfg.agent_id, cfg.agent_id) != 0) {
        ok = translate_agent_spawn(&fallback_cfg, typed, &sp);
        if (ok) app->translate_cleanup_snap.valid = false;  /* snapshot is stale */
    }
    if (!ok) {
        app_translate_debug_log("spawn-failed agent=%s fallback_agent=%s typed=\"%.240s\"",
                                cfg.agent_id, fallback_cfg.agent_id, typed);
        return false;
    }

    app->translate_active        = true;
    app->translate_cancel_requested = false;
    app->translate_local_inflight = false;
    app->translate_api_inflight  = false;
    app->translate_child_pid     = sp.child_pid;
    app->translate_stdout_fd     = sp.stdout_fd;
    app->translate_stream_line_len = 0;
    app->translate_stream_ready = false;
    app->translate_stream_emitted = false;
    app->translate_agent_first_bytes_announced = false;
    return true;
}

/* API backend — same child/pipe plumbing as the agent backend, but the
 * stdout is a buffered JSON response that gets finalized on exit (no
 * live streaming into the prompt). */
static bool app_translate_begin_api_backend(AppState *app, const char *typed) {
    if (!app || !typed || !typed[0]) return false;

    if (!app->translate_log) {
        app->translate_log_cap = 64 * 1024;
        app->translate_log = (char *)calloc(1, (usize)app->translate_log_cap);
        if (!app->translate_log) app->translate_log_cap = 0;
    }
    if (app->translate_log) app->translate_log[0] = '\0';
    app->translate_log_len = 0;

    TranslateConfig cfg = app->config.translate;
    translate_normalize_direction(&cfg);
    app_translate_debug_log("start backend=api provider=%s model=%s typed=\"%.240s\"",
                            cfg.api_provider,
                            cfg.api_model[0] ? cfg.api_model : "(default)",
                            typed);
    TranslateAgentSpawn sp = {0};
    if (!translate_api_spawn(&cfg, typed, &sp)) {
        app_translate_debug_log("api-spawn-failed provider=%s typed=\"%.240s\"",
                                cfg.api_provider, typed);
        return false;
    }

    app->translate_active        = true;
    app->translate_cancel_requested = false;
    app->translate_local_inflight = false;
    app->translate_api_inflight  = true;
    snprintf(app->translate_api_provider, sizeof app->translate_api_provider,
             "%s", cfg.api_provider);
    app->translate_child_pid     = sp.child_pid;
    app->translate_stdout_fd     = sp.stdout_fd;
    app->translate_stream_line_len = 0;
    app->translate_stream_ready = false;
    app->translate_stream_emitted = false;
    app->translate_agent_first_bytes_announced = false;
    return true;
}

static bool app_translate_start(AppState *app, Terminal *t, Session *s) {
    if (!app || !t || !s) return false;
    if (app->translate_active) return false;
    /* app_translate_try_intercept captured the prompt line into
     * app->translate_typed before the Ctrl+Cmd chord completed. */
    const char *typed = app->translate_typed;
    if (!typed[0]) return false;

    /* Split the prompt around KEEP tokens (dropped image paths, URLs,
     * attachment placeholders). One plain TEXT segment → the legacy
     * single-shot paths below run unchanged; otherwise the segmented
     * state machine translates each TEXT segment in place and re-emits
     * the KEEP tokens verbatim, preserving the original order. */
    TranslateSeg segs[TRANSLATE_MAX_SEGS];
    i32 nsegs = translate_segment_split(typed, segs, TRANSLATE_MAX_SEGS);
    if (nsegs > 0 && !translate_segment_has_text(segs, nsegs)) {
        app_show_toast(app, "Translate: nothing to translate");
        return false;
    }
    bool seg_mode = nsegs > 1;
    /* Mirror the legacy LOCAL-backend upfront refusals before erasing
     * anything: a local-only config without a usable engine must refuse
     * here (prompt intact), not silently fall through to an agent CLI
     * inside the segment loop. */
    if (seg_mode && app->config.translate.backend == TRANSLATE_BACKEND_LOCAL) {
#ifdef LIU_HAVE_LOCAL_LLM
        if (!liu_model_file_ok(app->config.translate.local_model_path)) {
            app_show_toast(app, "Translate: model indirilmedi — Ayarlar'dan indir");
            return false;
        }
#else
        app_show_toast(app, "Local backend not compiled");
        return false;
#endif
    }
    /* Same upfront refusal for an API config that can't run (no key /
     * no base URL) — refuse with the prompt intact rather than falling
     * through to an agent CLI mid-segment-loop. */
    if (seg_mode && app->config.translate.backend == TRANSLATE_BACKEND_API) {
        char why[96];
        if (!translate_api_ready(&app->config.translate, why, sizeof why)) {
            app_show_toast(app, why);
            return false;
        }
    }
    if (seg_mode) {
        memcpy(app->translate_segs, segs, sizeof(TranslateSeg) * (usize)nsegs);
        app->translate_seg_count = nsegs;
        app->translate_seg_cur = 0;
        app->translate_seg_text_emitted = false;
        app->translate_seg_pending_ws = false;
        app_translate_debug_log("segmented start nsegs=%d typed=\"%.240s\"",
                                nsegs, typed);

        app->translate_active           = true;
        app->translate_cancel_requested = false;
        app->translate_local_inflight   = false;
        app->translate_write_started    = false;
        app->translate_spawn_time       = platform_time_sec();
        app->translate_target_term      = t;
        app->translate_target_sess      = s;
        app_translate_delete_typed(app, t, s);
        app->translate_write_started    = true;

        /* Emits leading KEEP segments and submits the first TEXT segment.
         * On a first-submit failure the original text is restored and
         * state reset inside. */
        if (!app_translate_segments_advance(app)) return false;
    } else if (app->config.translate.backend == TRANSLATE_BACKEND_AGENT) {
        if (!app_translate_begin_agent_backend(app, typed)) {
            app_show_toast(app, "Translate: agent not installed");
            return false;
        }
    } else if (app->config.translate.backend == TRANSLATE_BACKEND_API) {
        char why[96];
        if (!translate_api_ready(&app->config.translate, why, sizeof why)) {
            app_show_toast(app, why);
            return false;
        }
        if (!app_translate_begin_api_backend(app, typed)) {
            app_show_toast(app, "Translate: API request failed to start");
            return false;
        }
    } else {
        /* Local-LLM backend. The worker thread + engine only exist when
         * the build opted into USE_LOCAL_LLM. */
#ifdef LIU_HAVE_LOCAL_LLM
        if (!liu_model_file_ok(app->config.translate.local_model_path)) {
            app_show_toast(app, "Translate: model indirilmedi — Ayarlar'dan indir");
            return false;
        }
        TranslateConfig cfg = app->config.translate;
        translate_normalize_direction(&cfg);
        app_translate_debug_log("start backend=local from=%s to=%s typed=\"%.240s\"",
                                cfg.source_lang, cfg.target_lang, typed);
        bool ok = translate_local_submit(&cfg, typed);
        if (!ok) {
            app_show_toast(app, "Translate: local engine busy");
            return false;
        }
        app->translate_active        = true;
        app->translate_cancel_requested = false;
        app->translate_local_inflight = true;
        app->translate_child_pid     = 0;
        app->translate_stdout_fd     = -1;
#else
        app_show_toast(app, "Local backend not compiled");
        return false;
#endif
    }

    if (!seg_mode) {
        app->translate_write_started = false;
        app->translate_spawn_time = platform_time_sec();
        app->translate_target_term = t;
        app->translate_target_sess = s;
        app_translate_delete_typed(app, t, s);
        app->translate_write_started = true;
    }

    /* Status toast — agent backend runs an INVISIBLE child process
     * (translate_agent_spawn already fork+execs `claude -p ...` /
     * `gemini -p ...` etc. with stdout piped to us) so the user has no
     * other visible signal that work is happening. Spell out which CLI
     * is being invoked so it's clear *why* there's a delay. */
    if (app->translate_local_inflight) {
        app_show_toast(app, "Translate engine queued");
    } else if (app->translate_api_inflight) {
        char msg[160];
        snprintf(msg, sizeof msg, "Translate: asking %s…",
                 app_translate_active_display(app));
        app_show_toast(app, msg);
    } else {
        const char *who = app_translate_agent_display(
            app->translate_fg_agent_id[0] ? app->translate_fg_agent_id
                                          : app->config.translate.agent_id);
        char msg[160];
        snprintf(msg, sizeof msg,
                 "Translate: asking %s in a hidden shell…", who);
        app_show_toast(app, msg);
    }
    return true;
}

static bool app_translate_stream_line_is_banner(const char *line, i32 len) {
    i32 s = 0;
    while (s < len && (unsigned char)line[s] <= ' ') s++;
    i32 e = len;
    while (e > s && (unsigned char)line[e - 1] <= ' ') e--;
    if (e <= s) return true;
    if (line[s] != '>') return false;
    if (e - s >= 7 && memcmp(line + s, "> build", 7) == 0) return true;
    for (i32 i = s; i + 1 < e; i++) {
        if ((unsigned char)line[i] == 0xC2 &&
            (unsigned char)line[i + 1] == 0xB7) {
            return true;
        }
    }
    return false;
}

static i32 app_translate_skip_ansi(const char *p, i32 left) {
    if (left < 1 || (unsigned char)p[0] != 0x1B) return 0;
    if (left >= 2 && p[1] == '[') {
        i32 i = 2;
        while (i < left) {
            unsigned char c = (unsigned char)p[i++];
            if (c >= 0x40 && c <= 0x7E) return i;
        }
        return left;
    }
    return left >= 2 ? 2 : 1;
}

static void app_translate_stream_emit(AppState *app, const char *data, i32 len) {
    if (!data || len <= 0) return;
    Terminal *t = app->translate_target_term;
    Session  *s = app->translate_target_sess;
    if (!app_translate_target_alive(app, t, s)) return;
    app_send_input(app, s, (const u8 *)data, len);
    app_translate_shadow_append_bytes(app, t, s, (const u8 *)data, len);
    terminal_scroll_to_bottom(t);
    app->translate_stream_emitted = true;
    if (!app->translate_local_inflight) app->toast_start_time = 0;
}

static void app_translate_stream_filtered_bytes(AppState *app, const char *data, i32 len) {
    /* Quote-stripping is an AGENT-backend heuristic only: CLIs sometimes
     * wrap their output in quotes, but the API backend already isolated
     * the text from JSON (translate_api_finalize) and the local engine
     * emits raw text — for both, a legitimate '"' in the translation
     * (e.g. He said "hi") must survive. Gate on the active backend. */
    bool strip_quotes = !app->translate_api_inflight &&
                        !app->translate_local_inflight;
    if (app->translate_seg_count > 0) {
        /* Segmented mode: this is the translation of one TEXT segment
         * being stitched between verbatim KEEP segments. Collapse
         * whitespace runs to a single space and hold the current run
         * back until a non-ws byte follows — leading/trailing whitespace
         * (e.g. the agent's final newline) must not leak between
         * segments; the KEEP segments carry the original spacing. */
        i32 start = 0;
        for (i32 i = 0; i < len; i++) {
            unsigned char ch = (unsigned char)data[i];
            if (ch > ' ' && (!strip_quotes || ch != '"')) {
                if (app->translate_seg_pending_ws) {
                    app->translate_seg_pending_ws = false;
                    app_translate_stream_emit(app, " ", 1);
                }
                app->translate_seg_text_emitted = true;
                continue;
            }
            if (i > start) app_translate_stream_emit(app, data + start, i - start);
            start = i + 1;
            if (ch <= ' ' && app->translate_seg_text_emitted) {
                app->translate_seg_pending_ws = true;
            }
        }
        if (len > start) app_translate_stream_emit(app, data + start, len - start);
        return;
    }

    i32 start = 0;
    for (i32 i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (!app->translate_stream_emitted && ch <= ' ') {
            if (i > start) app_translate_stream_emit(app, data + start, i - start);
            start = i + 1;
            continue;
        }
        if (strip_quotes && data[i] == '"') {
            if (i > start) app_translate_stream_emit(app, data + start, i - start);
            start = i + 1;
        }
    }
    if (len > start) app_translate_stream_emit(app, data + start, len - start);
}

static void app_translate_stream_flush_line(AppState *app) {
    if (app->translate_stream_line_len <= 0) return;
    if (app_translate_stream_line_is_banner(app->translate_stream_line,
                                            app->translate_stream_line_len)) {
        app->translate_stream_line_len = 0;
        return;
    }
    app->translate_stream_ready = true;
    app_translate_stream_filtered_bytes(app, app->translate_stream_line,
                                        app->translate_stream_line_len);
    app->translate_stream_line_len = 0;
}

/* Segmented run teardown helper: re-emit the untranslated remainder of
 * translate_typed verbatim so a failed or cancelled run never loses part
 * of the user's prompt. A half-streamed TEXT segment is skipped (its
 * translation already started writing — re-emitting the original would
 * duplicate it). Does not reset state or toast; callers do both. */
static void app_translate_segments_restore_rest(AppState *app) {
    i32 k = app->translate_seg_cur;
    if (k < app->translate_seg_count &&
        app->translate_segs[k].kind == TRANSLATE_SEG_TEXT &&
        app->translate_seg_text_emitted) {
        k++;
    }
    app->translate_seg_pending_ws = false;
    for (; k < app->translate_seg_count; k++) {
        TranslateSeg *sg = &app->translate_segs[k];
        app_translate_stream_emit(app, app->translate_typed + sg->off, sg->len);
    }
    app->translate_seg_cur = app->translate_seg_count;
}

/* Pump the segmented queue: emit KEEP segments at the cursor verbatim,
 * then hand the next TEXT segment to the configured backend (local
 * first, agent CLI as fallback — mirroring the single-shot paths).
 *
 * Returns true while a backend generation is in flight; false when the
 * run is over — either every segment was written (success toast) or a
 * submit failed (original text restored). Both end states reset the
 * translate state before returning, so callers just stop. */
static bool app_translate_segments_advance(AppState *app) {
    while (app->translate_seg_cur < app->translate_seg_count) {
        TranslateSeg *sg = &app->translate_segs[app->translate_seg_cur];
        if (sg->kind == TRANSLATE_SEG_KEEP) {
            app_translate_stream_emit(app, app->translate_typed + sg->off,
                                      sg->len);
            app->translate_seg_cur++;
            continue;
        }

        /* TEXT segment — copy out NUL-terminated and submit. The scratch
         * outlives the submit (both backends copy synchronously) and is
         * what the local→agent fallback in app_tick_translate resubmits. */
        i32 cl = sg->len;
        if (cl > (i32)sizeof(app->translate_seg_scratch) - 1) {
            cl = (i32)sizeof(app->translate_seg_scratch) - 1;
        }
        memcpy(app->translate_seg_scratch, app->translate_typed + sg->off,
               (usize)cl);
        app->translate_seg_scratch[cl] = '\0';
        app->translate_seg_text_emitted = false;
        app->translate_seg_pending_ws = false;

        bool submitted = false;
        if (app->config.translate.backend == TRANSLATE_BACKEND_LOCAL) {
#ifdef LIU_HAVE_LOCAL_LLM
            if (app->config.translate.local_model_path[0]) {
                TranslateConfig cfg = app->config.translate;
                translate_normalize_direction(&cfg);
                if (translate_local_submit(&cfg, app->translate_seg_scratch)) {
                    app->translate_local_inflight = true;
                    app->translate_child_pid = 0;
                    app->translate_stdout_fd = -1;
                    submitted = true;
                }
            }
#endif
        } else if (app->config.translate.backend == TRANSLATE_BACKEND_API) {
            /* API per segment; an unstartable request falls through to
             * the agent CLI below, mirroring the local→agent fallback. */
            if (app_translate_begin_api_backend(app,
                                                app->translate_seg_scratch)) {
                submitted = true;
            }
        }
        if (!submitted &&
            app_translate_begin_agent_backend(app, app->translate_seg_scratch)) {
            submitted = true;
        }
        if (!submitted) {
            app_translate_debug_log("segmented submit-failed seg=%d/%d",
                                    app->translate_seg_cur,
                                    app->translate_seg_count);
            /* Restore BEFORE toasting — app_translate_stream_emit clears
             * toast_start_time on every write, so the reverse order would
             * wipe the failure toast in the same frame. */
            app_translate_segments_restore_rest(app);
            app_show_toast(app, "Translate: agent not installed");
            app_translate_reset(app);
            return false;
        }
        app->translate_spawn_time = platform_time_sec();
        return true;
    }

    /* Every segment has been written. */
    app_translate_debug_log("segmented done nsegs=%d", app->translate_seg_count);
    app_show_toast(app, "Translate done");
    app_translate_reset(app);
    return false;
}

static void app_translate_stream_agent_bytes(AppState *app, const char *buf, i32 len) {
    for (i32 i = 0; i < len; ) {
        i32 esc = app_translate_skip_ansi(buf + i, len - i);
        if (esc > 0) {
            i += esc;
            continue;
        }

        char ch = buf[i++];
        if (!app->translate_stream_ready) {
            if (app->translate_stream_line_len == 0) {
                if (ch == '\r' || ch == '\n' || (unsigned char)ch <= ' ') continue;
                if (ch != '>') {
                    app->translate_stream_ready = true;
                    app_translate_stream_filtered_bytes(app, &ch, 1);
                    continue;
                }
            }
            if (ch == '\r') continue;
            if (ch == '\n') {
                app_translate_stream_flush_line(app);
                continue;
            }
            if (app->translate_stream_line_len <
                (i32)sizeof(app->translate_stream_line) - 1) {
                app->translate_stream_line[app->translate_stream_line_len++] = ch;
            } else {
                app->translate_stream_ready = true;
                app_translate_stream_filtered_bytes(app, app->translate_stream_line,
                                                    app->translate_stream_line_len);
                app->translate_stream_line_len = 0;
                app_translate_stream_filtered_bytes(app, &ch, 1);
            }
            continue;
        }

        app_translate_stream_filtered_bytes(app, &ch, 1);
    }
}

/* Drain the agent's stdout into the rolling log. Same pattern as
 * create_theme_drain_stdout — non-blocking pipe so we just read until
 * EAGAIN. */
static void translate_drain_stdout(AppState *app) {
    if (app->translate_stdout_fd < 0 || !app->translate_log) return;
    char buf[1024];
    bool saw_bytes = false;
    for (;;) {
        ssize_t n = read(app->translate_stdout_fd, buf, sizeof buf);
        if (n <= 0) break;
        saw_bytes = true;
        i32 cap = app->translate_log_cap - 1;
        i32 cur = app->translate_log_len;
        i32 incoming = (i32)n;
        if (cur + incoming > cap) {
            i32 drop = cur + incoming - cap;
            if (drop > cur) drop = cur;
            memmove(app->translate_log, app->translate_log + drop, (usize)(cur - drop));
            cur -= drop;
        }
        i32 copy = incoming;
        if (copy > cap - cur) copy = cap - cur;
        memcpy(app->translate_log + cur, buf, (usize)copy);
        cur += copy;
        app->translate_log[cur] = '\0';
        app->translate_log_len = cur;
        /* API responses are JSON envelopes — never stream them raw into
         * the prompt; the tick finalizes them on child exit instead. */
        if (!app->translate_local_inflight && !app->translate_api_inflight &&
            app->translate_write_started) {
            app_translate_stream_agent_bytes(app, buf, incoming);
        }
    }
    /* Fire the "receiving" notification once, the first time bytes show
     * up from the hidden child. Up to this point the user only saw the
     * spawn toast; the first byte means the agent actually accepted the
     * prompt and is generating output. */
    if (saw_bytes &&
        !app->translate_local_inflight &&
        !app->translate_agent_first_bytes_announced) {
        app->translate_agent_first_bytes_announced = true;
        const char *who = app_translate_active_display(app);
        char msg[160];
        snprintf(msg, sizeof msg, "Translate: %s is responding…", who);
        app_show_toast(app, msg);
    }
}

static void app_translate_log_failure(AppState *app, const char *reason, int status) {
    const char *agent = app->translate_fg_agent_id[0]
        ? app->translate_fg_agent_id
        : app->config.translate.agent_id;
    fprintf(stderr, "translate: failed reason=%s agent=%s status=%d log_len=%d typed=\"%.160s\"\n",
            reason ? reason : "unknown", agent ? agent : "", status,
            app->translate_log_len, app->translate_typed);
    app_translate_debug_log("failed reason=%s agent=%s status=%d log_len=%d typed=\"%.240s\"",
                            reason ? reason : "unknown", agent ? agent : "",
                            status, app->translate_log_len, app->translate_typed);
    if (app->translate_log && app->translate_log_len > 0) {
        i32 start = app->translate_log_len > 2048 ? app->translate_log_len - 2048 : 0;
        fprintf(stderr, "translate: child log tail:\n%.*s\n",
                app->translate_log_len - start, app->translate_log + start);
        app_translate_debug_log("child-log-tail %.*s",
                                app->translate_log_len - start,
                                app->translate_log + start);
    }
}

static void app_tick_translate(AppState *app) {
    /* Phase 1: clear pending Ctrl+Cmd chords that aged out. */
    if (app->translate_tab_pending > 0) {
        f64 now = platform_time_sec();
        f32 win = app->config.translate.tab_window_sec > 0 ?
                    app->config.translate.tab_window_sec : 0.4f;
        if (now - app->translate_tab_first_time > (f64)win) {
            app_translate_clear_pending_chord(app);
        }
    }

    if (!app->translate_active) return;
    if (app->translate_cancel_requested) {
        app_translate_cancel(app);
        return;
    }
    if (!app_translate_target_alive(app, app->translate_target_term,
                                    app->translate_target_sess)) {
        app_translate_cancel(app);
        return;
    }

    f64 now = platform_time_sec();

    /* Local-LLM backend: drain the worker thread's result buffer. The
     * worker can take seconds (model load + decode) so the timeout is
     * generous; the main thread is never blocked. */
    if (app->translate_local_inflight) {
#ifdef LIU_HAVE_LOCAL_LLM
        if (translate_local_consume_started()) {
            app_show_toast(app, "Translate engine started");
        }

        /* Stream the local LLM's tokens straight into the agent prompt:
         * drain whatever was generated since the last frame and write it
         * live. The prompt text was already deleted when translation
         * started, so every decoded chunk can go straight into the TUI. */
        Terminal *t = app->translate_target_term;
        char chunk[4096];
        bool done = false, ok = false;
        i32 n = translate_local_drain(chunk, (i32)sizeof chunk, &done, &ok);
        if (n > 0) {
            app_translate_stream_filtered_bytes(app, chunk, n);
            terminal_scroll_to_bottom(t);
        }
        if (done) {
            app_translate_close_stream(app);
            /* Segmented run: this generation covered only the current TEXT
             * segment — judge and resubmit per segment, not per prompt. */
            bool seg_mode = app->translate_seg_count > 0;
            bool emitted = seg_mode ? app->translate_seg_text_emitted
                                    : app->translate_stream_emitted;
            const char *src = seg_mode ? app->translate_seg_scratch
                                       : app->translate_typed;
            if (!ok || !emitted) {
                app_translate_debug_log("local-failed fallback_to_agent ok=%d emitted=%d typed=\"%.240s\"",
                                        ok ? 1 : 0,
                                        emitted ? 1 : 0,
                                        src);
                /* Segmented: never resubmit a half-streamed segment — the
                 * agent would re-emit the whole translation after the
                 * partial bytes already written into the prompt. */
                bool can_retry = !seg_mode || !emitted;
                if (can_retry && app_translate_begin_agent_backend(app, src)) {
                    app->translate_spawn_time = platform_time_sec();
                    app->translate_write_started = true;
                    const char *who = app_translate_agent_display(
                        app->translate_fg_agent_id[0] ? app->translate_fg_agent_id
                                                      : app->config.translate.agent_id);
                    char msg[176];
                    snprintf(msg, sizeof msg,
                             "Translate: local engine failed, asking %s...",
                             who);
                    app_show_toast(app, msg);
                    return;
                }
                /* Restore before toasting — stream emits clear the toast. */
                if (seg_mode) app_translate_segments_restore_rest(app);
                app_show_toast(app, ok ? "Translate: empty result"
                                       : "Translate failed");
                app_translate_reset(app);
                return;
            }
            if (seg_mode) {
                app->translate_seg_cur++;
                (void)app_translate_segments_advance(app);
                return;
            }
            app_translate_reset(app);
        } else if (now - app->translate_spawn_time > 120.0) {
            /* Stuck far past any reasonable decode — give up on this
             * attempt. Cancel the still-running generation so the backend
             * isn't left busy decoding to TL_MAX_TOKENS, which would reject
             * the next submit with "engine busy"; the worker frees the
             * engine afterward, as it does after every run. */
            translate_local_cancel();
            app_translate_close_stream(app);
            bool seg_mode = app->translate_seg_count > 0;
            bool emitted = seg_mode ? app->translate_seg_text_emitted
                                    : app->translate_stream_emitted;
            const char *src = seg_mode ? app->translate_seg_scratch
                                       : app->translate_typed;
            app_translate_debug_log("local-timeout fallback_to_agent emitted=%d typed=\"%.240s\"",
                                    emitted ? 1 : 0, src);
            if (!emitted &&
                app_translate_begin_agent_backend(app, src)) {
                app->translate_spawn_time = platform_time_sec();
                app->translate_write_started = true;
                const char *who = app_translate_agent_display(
                    app->translate_fg_agent_id[0] ? app->translate_fg_agent_id
                                                  : app->config.translate.agent_id);
                char msg[176];
                snprintf(msg, sizeof msg,
                         "Translate: local engine timed out, asking %s...",
                         who);
                app_show_toast(app, msg);
                return;
            }
            if (seg_mode) {
                /* Restore before toasting — stream emits clear the toast. */
                app_translate_segments_restore_rest(app);
                app_show_toast(app, "Translate: local engine timed out");
            } else if (!app->translate_write_started) {
                app_show_toast(app, "Translate failed");
            }
            app_translate_reset(app);
        }
#else
        app_show_toast(app, "Local backend not compiled");
        app_translate_reset(app);
#endif
        return;
    }

    /* Agent backend polling is only a fallback path. Local engine streaming
     * above is drained every frame so generated chunks hit the prompt live. */
    static f64 last_tick = 0;
    if (now - last_tick < 0.1) return;
    last_tick = now;

    /* Agent backend: drain the child's stdout, reap on exit. */
    translate_drain_stdout(app);

    /* 30 s hard timeout — kill the child and surface failure. */
    if (now - app->translate_spawn_time > 30.0) {
        app_translate_log_failure(app, "timeout", 0);
        const char *who = app_translate_active_display(app);
        app_translate_kill_child(app);
        app_translate_close_stream(app);
        char msg[160];
        snprintf(msg, sizeof msg, "Translate: %s timed out (30s)", who);
        /* Restore before toasting — stream emits clear the toast. */
        if (app->translate_seg_count > 0) app_translate_segments_restore_rest(app);
        app_show_toast(app, msg);
        app_translate_reset(app);
        return;
    }

    bool child_exited = false;
    int wstatus = 0;
    if (app->translate_child_pid > 0) {
        pid_t wp = waitpid(app->translate_child_pid, &wstatus, WNOHANG);
        if (wp == app->translate_child_pid) child_exited = true;
    }
    if (!child_exited) return;

    /* Final drain after exit so we don't lose the tail of the response. */
    translate_drain_stdout(app);
    if (app->translate_stdout_fd >= 0) {
        close(app->translate_stdout_fd);
        app->translate_stdout_fd = -1;
    }
    app->translate_child_pid = 0;

    /* The child has exited and written its session transcript (if any). For
     * grok/opencode delete that throwaway translate one-shot now — no-op for
     * claude/codex (their --no-session-persistence/--ephemeral already prevented
     * the write) and for API/local (snapshot was never taken → valid=false). */
    translate_cleanup_after_exit(app->translate_cleanup_agent,
                                 app->translate_cleanup_cwd,
                                 app->translate_spawn_time,
                                 &app->translate_cleanup_snap);
    app->translate_cleanup_snap.valid = false;   /* consumed */

    bool exit_ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
    app_translate_stream_flush_line(app);
    app_translate_close_stream(app);
    /* Segmented run: this child translated only the current TEXT segment. */
    bool seg_mode = app->translate_seg_count > 0;
    bool got = seg_mode ? app->translate_seg_text_emitted
                        : app->translate_stream_emitted;
    char api_err[160] = {0};
    if (exit_ok && !got) {
        char text[8 * 1024];
        if (app->translate_api_inflight) {
            /* API child: the buffered JSON envelope gets parsed here —
             * curl exits 0 even on HTTP 4xx, so the error envelope (and
             * api_err) is what actually distinguishes failure. Parse with
             * the provider snapshotted at spawn so a mid-flight Settings
             * change can't select the wrong response shape. */
            TranslateConfig fin_cfg = app->config.translate;
            snprintf(fin_cfg.api_provider, sizeof fin_cfg.api_provider, "%s",
                     app->translate_api_provider);
            got = translate_api_finalize(&fin_cfg,
                                         app->translate_log,
                                         app->translate_log_len,
                                         text, sizeof text,
                                         api_err, sizeof api_err);
        } else {
            got = translate_agent_finalize(app->translate_log,
                                           app->translate_log_len,
                                           text, sizeof text);
        }
        if (got && text[0]) {
            app_translate_stream_filtered_bytes(app, text, (i32)strlen(text));
        }
    }
    if (!exit_ok || !got) {
        app_translate_log_failure(app, got ? "child-exit" : "empty-output", wstatus);
        const char *who = app_translate_active_display(app);
        char msg[160];
        if (api_err[0]) {
            snprintf(msg, sizeof msg, "Translate: %s failed — %.96s",
                     who, api_err);
        } else {
            snprintf(msg, sizeof msg, "Translate: %s failed", who);
        }
        /* Restore before toasting — stream emits clear the toast. */
        if (seg_mode) app_translate_segments_restore_rest(app);
        app_show_toast(app, msg);
    } else if (seg_mode) {
        app->translate_seg_cur++;
        (void)app_translate_segments_advance(app);
        return;
    } else {
        /* Success — surface that the hidden worker actually finished. */
        const char *who = app_translate_active_display(app);
        f64 elapsed = platform_time_sec() - app->translate_spawn_time;
        char msg[160];
        snprintf(msg, sizeof msg, "Translate: %s done (%.1fs)", who, elapsed);
        app_show_toast(app, msg);
    }
    app_translate_reset(app);
}

/* Is the user about to invoke Ctrl+Cmd×2 translate? Returns true when this
 * modifier-only key event was part of the chord sequence. */
static bool app_translate_try_intercept(AppState *app, Terminal *t, Session *s) {
    if (!app || !t || !s) return false;
    if (!app->config.translate.enabled) return false;
    if (app->translate_active) return false;

    if (app->translate_tab_pending > 0 &&
        (app->translate_tab_target_term != t ||
         app->translate_tab_target_sess != s)) {
        app_translate_clear_pending_chord(app);
    }

    /* Gate on a known agent CLI being the foreground process. */
    const char *basename = session_fg_process(s);
    const char *id = agent_id_for_basename(basename);
    if (!id[0] || !translate_active_in(&app->config.translate, id)) {
        if (app->translate_tab_pending > 0) app_translate_clear_pending_chord(app);
        app->translate_fg_agent_id[0] = '\0';
        return false;
    }
    snprintf(app->translate_fg_agent_id, sizeof app->translate_fg_agent_id,
             "%s", id);

    /* Capture the current prompt line. Prefer the input shadow: agent TUIs
     * often draw their editor in ways that are hard to reconstruct from the
     * terminal grid, while the shadow is the exact bytes Liu sent. Fall back
     * to visual capture for older state or externally-mutated prompts. */
    i32 bs = 0;
    bool captured = false;
    if (app_translate_shadow_get(app, t, s, app->translate_typed,
                                 sizeof app->translate_typed, &bs)) {
        app_translate_debug_log("capture used input shadow len=%d",
                                (i32)strlen(app->translate_typed));
        captured = true;
    } else if (app_translate_capture_line(t, app->translate_typed,
                                          sizeof app->translate_typed, &bs)) {
        app_translate_debug_log("capture used visual line len=%d",
                                (i32)strlen(app->translate_typed));
        captured = true;
    }
    if (!captured) {
        if (app->translate_tab_pending > 0) app_translate_clear_pending_chord(app);
        return false;
    }
    if (app_translate_is_pasted_placeholder(app->translate_typed) &&
        app_translate_use_recent_paste(app, t, s, app->translate_typed,
                                       sizeof app->translate_typed, &bs)) {
        app_translate_debug_log("capture used recent paste len=%d",
                                (i32)strlen(app->translate_typed));
    }
    app->translate_typed_bs = bs;
    app_translate_debug_log("capture typed_len=%d bs=%d preview=\"%.180s\"",
                            (i32)strlen(app->translate_typed), bs,
                            app->translate_typed);

    f64 now = platform_time_sec();
    f32 win = app->config.translate.tab_window_sec > 0 ?
                app->config.translate.tab_window_sec : 0.4f;
    if (app->translate_tab_pending > 0 &&
        now - app->translate_tab_last_time > (f64)win) {
        app_translate_clear_pending_chord(app);
    }

    if (app->translate_tab_pending == 0) {
        app->translate_tab_first_time = now;
        app->translate_tab_target_term = t;
        app->translate_tab_target_sess = s;
    }
    app->translate_tab_pending++;
    app->translate_tab_last_time = now;

    if (app->translate_tab_pending >= 2) {
        app_translate_clear_pending_chord(app);
        (void)app_translate_start(app, t, s);
    }
    return true;
}

/* Insert a single character into the active create-theme field. Honours
 * the per-field cap, ignores control chars. */
static void app_create_theme_insert_char(AppState *app, u32 cp) {
    if (cp < 32 || cp >= 127) return;
    char *buf;
    i32 *len_ptr;
    i32 cap;
    if (app->create_theme_field == 0) {
        buf = app->create_theme_name;
        len_ptr = &app->create_theme_name_len;
        cap = (i32)sizeof(app->create_theme_name);
    } else if (app->create_theme_field == 1) {
        buf = app->create_theme_desc;
        len_ptr = &app->create_theme_desc_len;
        cap = (i32)sizeof(app->create_theme_desc);
    } else {
        return;
    }
    if (*len_ptr + 1 < cap) {
        buf[*len_ptr] = (char)cp;
        (*len_ptr)++;
        buf[*len_ptr] = '\0';
        if (app->create_theme_error[0]) {
            app->create_theme_error[0] = '\0';
            app->create_theme_error_until = 0;
        }
    }
}

static void app_create_theme_backspace(AppState *app) {
    char *buf;
    i32 *len_ptr;
    if (app->create_theme_field == 0) {
        buf = app->create_theme_name;
        len_ptr = &app->create_theme_name_len;
    } else if (app->create_theme_field == 1) {
        buf = app->create_theme_desc;
        len_ptr = &app->create_theme_desc_len;
    } else { return; }
    if (*len_ptr > 0) {
        (*len_ptr)--;
        buf[*len_ptr] = '\0';
    }
}

static void app_close_create_theme(AppState *app) {
    /* Esc/Close on the modal during phase 1: actively reap the agent.
     * The previous "continuing in background" message was a lie —
     * app_tick_create_theme early-returns when active == false, so
     * the pipe never drained, waitpid never fired, and the child
     * eventually became a zombie with a leaked fd. Killing here keeps
     * fd / pid / agent RSS (often 200-400 MB per claude/codex run)
     * bounded across repeated open→Esc cycles. */
    bool was_generating = (app->create_theme_phase == 1 &&
                           app->create_theme_child_pid > 0);
    app->create_theme_active = false;
    if (was_generating) {
        create_theme_kill_child(app);
        app_show_toast(app, app->create_theme_doc_mode
            ? "Note generation cancelled." : "Theme generation cancelled.");
    }
    /* Mirror the finalize path: drop the 256 KB log scratch buffer on Esc
     * too. app_tick_create_theme early-returns while inactive, so nothing
     * appends after this; the open handler re-allocates fresh. */
    app->create_theme_log_len = 0;
    free(app->create_theme_log);
    app->create_theme_log     = NULL;
    app->create_theme_log_cap = 0;
}

/* NSFilePromise provider context — one per dragged remote file. Lives on the
 * heap because Cocoa calls the provider on a background queue at an
 * arbitrary later time. Freed by the provider after the write completes. */
typedef struct {
    Session *session;
    char     remote_path[1024];
} SFTPPromiseCtx;

/* =========================================================================
 * Async file-browser paste task
 *
 * Pasting from the sidebar used to run synchronously on the main thread —
 * a multi-GB cross-source transfer froze keystroke echo for minutes. The
 * task below snapshots the clipboard, hands ownership to a detached worker,
 * and lets the main thread keep rendering / handling input. Completion is
 * picked up by `app_fb_poll_task` every frame, which joins the worker,
 * refreshes the listing, and flashes a status toast.
 * ========================================================================= */

typedef struct {
    FileClipboard      clip;           /* independent copy, survives UI state churn */
    FileBrowserSource  dst_source;
    char               dst_cwd[FB_MAX_PATH];
    void              *dst_session;    /* Session * for SFTP dst */
    void              *dst_sftp_handle;/* LIBSSH2_SFTP * cached at dispatch */
    pthread_t          tid;
    _Atomic bool       done;
    _Atomic bool       success;
} FileOpsPasteTask;

static void *app_fb_paste_worker(void *arg) {
    FileOpsPasteTask *t = (FileOpsPasteTask *)arg;

    /* Suspend main-thread libssh2 polling on both the source and dest
     * sessions (may be the same) so they don't deadlock while the worker
     * holds the blocking-mode scope. Skipped for local sources/dests. */
    Session *src_busy = NULL;
    for (i32 i = 0; i < t->clip.count; i++) {
        if (t->clip.items[i].source == FB_SOURCE_SFTP && t->clip.items[i].session) {
            src_busy = (Session *)t->clip.items[i].session;
            break;
        }
    }
    Session *dst_busy = (t->dst_source == FB_SOURCE_SFTP)
                          ? (Session *)t->dst_session : NULL;
    if (src_busy) session_io_set_suspended(src_busy, true);
    if (dst_busy && dst_busy != src_busy) session_io_set_suspended(dst_busy, true);

    /* Shimmed FileBrowser — fb_paste_item only reads source/cwd/session/
     * sftp_handle. The shim is stack-local to this worker; the real
     * FileBrowser is refreshed on the main thread after completion. */
    FileBrowser dst = (FileBrowser){0};
    dst.source      = t->dst_source;
    dst.session     = t->dst_session;
    dst.sftp_handle = t->dst_sftp_handle;
    snprintf(dst.cwd, sizeof(dst.cwd), "%s", t->dst_cwd);

    bool all_ok = true;
    for (i32 i = 0; i < t->clip.count; i++) {
        if (!fb_paste_item(&dst, &t->clip.items[i], t->clip.is_cut)) {
            all_ok = false;
            /* keep going — partial success matches user expectation for
             * multi-select paste where one entry fails (conflict, permission). */
        }
    }

    if (src_busy) session_io_set_suspended(src_busy, false);
    if (dst_busy && dst_busy != src_busy) session_io_set_suspended(dst_busy, false);

    atomic_store(&t->success, all_ok);
    atomic_store(&t->done, true);
    return NULL;
}

/* Called every frame from the main loop. Cheap when idle. */
static void app_fb_poll_task(AppState *app) {
    if (!app->fb_task_active || !app->fb_task_opaque) return;
    FileOpsPasteTask *t = (FileOpsPasteTask *)app->fb_task_opaque;
    if (!atomic_load(&t->done)) return;

    pthread_join(t->tid, NULL);
    bool ok = atomic_load(&t->success);
    bool was_cut = t->clip.is_cut;
    /* Clear clipboard on successful cut — matches pre-threading behavior. */
    if (ok && was_cut) memset(&app->file_clipboard, 0, sizeof(app->file_clipboard));
    fb_refresh(&app->filebrowser);
    if (ok) app_fb_set_status(app, was_cut ? "Moved" : "Pasted");
    else    app_fb_set_status(app, "Paste had errors");
    free(t);
    app->fb_task_opaque = NULL;
    app->fb_task_active = false;
}

static void sftp_promise_provider(void *vctx, const char *dst_path) {
    SFTPPromiseCtx *c = (SFTPPromiseCtx *)vctx;
    if (!c) return;
    if (!dst_path) { free(c); return; }

    /* The session may have been torn down between drag-start and drop —
     * in that case we silently write an empty file rather than crashing. */
    if (c->session && session_get_sftp(c->session)) {
        LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)session_get_sftp(c->session);
        /* Suspend the main loop's poll on this session while we hold the
         * blocking-mode scope — otherwise session_read on the main thread
         * would block inside libssh2 (no EAGAIN), freezing the UI. */
        session_io_set_suspended(c->session, true);
        session_sftp_scope_begin(c->session);

        /* Stat first so the UI panel has a denominator. */
        u64 total_size = 0;
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        if (libssh2_sftp_stat(sftp, c->remote_path, &attrs) == 0)
            total_size = attrs.filesize;

        LIBSSH2_SFTP_HANDLE *rh = libssh2_sftp_open(sftp, c->remote_path,
                                                    LIBSSH2_FXF_READ, 0);
        if (rh) {
            FILE *lf = fopen(dst_path, "wb");
            if (lf) {
                sftp_transfer_begin(c->remote_path, total_size, false);
                char buf[32768];
                u64 done = 0;
                for (;;) {
                    ssize_t n = libssh2_sftp_read(rh, buf, sizeof(buf));
                    if (n <= 0) break;
                    if (fwrite(buf, 1, (size_t)n, lf) != (size_t)n) break;
                    done += (u64)n;
                    sftp_transfer_tick(done);
                }
                sftp_transfer_end();
                fclose(lf);
            }
            libssh2_sftp_close(rh);
        }
        session_sftp_scope_end(c->session);
        session_io_set_suspended(c->session, false);
    }
    free(c);
}

static void app_fb_copy(AppState *app, i32 entry_index, bool is_cut) {
    FileBrowser *fb = &app->filebrowser;

    /* Build the batch: prefer the multi-selection when non-empty; otherwise
     * fall back to the single clicked entry_index (legacy keyboard path). */
    i32 sel_buf[FB_CLIP_MAX];
    i32 n = fb_sel_collect(fb, sel_buf, FB_CLIP_MAX);
    if (n == 0) {
        if (entry_index < 0 || entry_index >= fb->entry_count) return;
        sel_buf[0] = entry_index;
        n = 1;
    }

    /* Strip invalid entries ("..", out-of-range) while repacking into clip. */
    memset(&app->file_clipboard, 0, sizeof(app->file_clipboard));
    app->file_clipboard.is_cut = is_cut;
    for (i32 i = 0; i < n; i++) {
        i32 ei = sel_buf[i];
        if (ei < 0 || ei >= fb->entry_count) continue;
        FileEntry *e = &fb->entries[ei];
        if (strcmp(e->name, "..") == 0) continue;
        if (app->file_clipboard.count >= FB_CLIP_MAX) break;

        FileClipboardItem *it = &app->file_clipboard.items[app->file_clipboard.count++];
        it->source  = fb->source;
        it->is_dir  = e->is_dir;
        it->session = (fb->source == FB_SOURCE_SFTP) ? fb->session : NULL;
        fb_entry_path(fb, e, it->path, sizeof(it->path));
        snprintf(it->name, sizeof(it->name), "%s", e->name);
    }
    app->file_clipboard.has = (app->file_clipboard.count > 0);

    char msg[160];
    if (app->file_clipboard.count > 1) {
        snprintf(msg, sizeof(msg), "%s %d items",
                 is_cut ? "Cut" : "Copied", app->file_clipboard.count);
    } else if (app->file_clipboard.count == 1) {
        snprintf(msg, sizeof(msg), "%s: %s",
                 is_cut ? "Cut" : "Copied",
                 app->file_clipboard.items[0].name);
    } else {
        snprintf(msg, sizeof(msg), "Nothing to %s", is_cut ? "cut" : "copy");
    }
    app_fb_set_status(app, msg);
}

static bool app_fb_paste(AppState *app) {
    if (!app->file_clipboard.has) {
        app_fb_set_status(app, "Clipboard empty");
        return false;
    }
    if (app->fb_task_active) {
        app_fb_set_status(app, "Transfer already in progress");
        return false;
    }

    /* Dispatch the paste to a worker thread — GB-scale transfers used to
     * block the main event loop for minutes, freezing keystroke echo.
     * The worker reads libssh2 through session_sftp_scope_begin/end which
     * flips blocking mode atomically; the main thread's channel reads on
     * the same session may briefly contend, but the session's internal
     * lock serializes them without a deadlock. */
    FileOpsPasteTask *task = calloc(1, sizeof(FileOpsPasteTask));
    if (!task) { app_fb_set_status(app, "Out of memory"); return false; }
    task->clip        = app->file_clipboard;
    task->dst_source  = app->filebrowser.source;
    task->dst_session = app->filebrowser.session;
    task->dst_sftp_handle = app->filebrowser.sftp_handle;
    snprintf(task->dst_cwd, sizeof(task->dst_cwd), "%s", app->filebrowser.cwd);
    atomic_init(&task->done, false);
    atomic_init(&task->success, false);

    if (pthread_create(&task->tid, NULL, app_fb_paste_worker, task) != 0) {
        free(task);
        app_fb_set_status(app, "Failed to start transfer");
        return false;
    }
    app->fb_task_opaque = task;
    app->fb_task_active = true;

    char msg[96];
    if (task->clip.count == 1)
        snprintf(msg, sizeof(msg), "Transferring %s…", task->clip.items[0].name);
    else
        snprintf(msg, sizeof(msg), "Transferring %d items…", task->clip.count);
    app_fb_set_status(app, msg);
    return true;
}

static void app_fb_begin_rename(AppState *app, i32 entry_index) {
    FileBrowser *fb = &app->filebrowser;
    if (entry_index < 0 || entry_index >= fb->entry_count) return;
    FileEntry *e = &fb->entries[entry_index];
    if (strcmp(e->name, "..") == 0) return;

    app->fb_prompt_active = true;
    app->fb_prompt_mode   = 0;
    app->fb_prompt_index  = entry_index;
    snprintf(app->fb_prompt_buf, sizeof(app->fb_prompt_buf), "%s", e->name);
    app->fb_prompt_len = (i32)strlen(app->fb_prompt_buf);
}

static void app_fb_begin_new_folder(AppState *app) {
    app->fb_prompt_active = true;
    app->fb_prompt_mode   = 1;
    app->fb_prompt_index  = -1;
    app->fb_prompt_buf[0] = '\0';
    app->fb_prompt_len    = 0;
}

static void app_fb_begin_new_file(AppState *app) {
    app->fb_prompt_active = true;
    app->fb_prompt_mode   = 2;
    app->fb_prompt_index  = -1;
    app->fb_prompt_buf[0] = '\0';
    app->fb_prompt_len    = 0;
}

static void app_fb_apply_prompt(AppState *app) {
    if (!app->fb_prompt_active || app->fb_prompt_len <= 0) { app->fb_prompt_active = false; return; }
    const char *name = app->fb_prompt_buf;

    /* Refuse obviously bad names — no slashes (path traversal), no empty. */
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') { app_fb_set_status(app, "Invalid name"); app->fb_prompt_active = false; return; }
    }

    bool ok = false;
    if (app->fb_prompt_mode == 0) {
        ok = fb_rename(&app->filebrowser, app->fb_prompt_index, name);
        app_fb_set_status(app, ok ? "Renamed" : "Rename failed");
    } else if (app->fb_prompt_mode == 2) {
        ok = fb_create_file(&app->filebrowser, name);
        app_fb_set_status(app, ok ? "File created" : "Create failed");
    } else {
        ok = fb_mkdir(&app->filebrowser, name);
        app_fb_set_status(app, ok ? "Folder created" : "Create failed");
    }
    app->fb_prompt_active = false;
}

static void app_fb_ctx_action(AppState *app, i32 row, i32 entry_index) {
    /* Row layout (keep in sync with ui.c ctx-menu render):
     * 0 Open  1 Rename  2 Delete  3 Copy  4 Cut  5 Paste  6 New Folder
     * 7 New File */
    switch (row) {
        case 0: /* Open */
            if (entry_index >= 0) fb_open_file(&app->filebrowser, entry_index);
            break;
        case 1: /* Rename */
            app_fb_begin_rename(app, entry_index);
            break;
        case 2: /* Delete */
            if (fb_delete_file(&app->filebrowser, entry_index))
                app_fb_set_status(app, "Deleted");
            else
                app_fb_set_status(app, "Delete failed");
            break;
        case 3: /* Copy */
            app_fb_copy(app, entry_index, false);
            break;
        case 4: /* Cut */
            app_fb_copy(app, entry_index, true);
            break;
        case 5: /* Paste */
            app_fb_paste(app);
            break;
        case 6: /* New Folder */
            app_fb_begin_new_folder(app);
            break;
        case 7: /* New File */
            app_fb_begin_new_file(app);
            break;
        default: break;
    }
}

static void app_reset_port_forward_dialog(AppState *app) {
    app->port_forward_dialog_active = true;
    app->port_forward_mode = 0;
    app->port_forward_local_port[0] = '\0';
    snprintf(app->port_forward_remote_host, sizeof(app->port_forward_remote_host), "127.0.0.1");
    app->port_forward_remote_port[0] = '\0';
    app->port_forward_field = 0;
}

static void app_submit_port_forward(AppState *app) {
    Session *s = app_focused_session(app);
    if (!s || session_type(s) != SESSION_SSH || session_status(s) != SESSION_CONNECTED) return;

    i32 local_port = atoi(app->port_forward_local_port);
    if (local_port <= 0) return;

    bool ok = false;
    if (app->port_forward_mode == 1) {
        ok = session_dynamic_forward_start(s, local_port);
    } else {
        i32 remote_port = atoi(app->port_forward_remote_port);
        if (remote_port <= 0 || !app->port_forward_remote_host[0]) return;
        ok = session_local_forward_start(s, local_port,
                                         app->port_forward_remote_host, remote_port);
    }

    if (ok) {
        app->port_forward_dialog_active = false;
    }
}

static void on_native_font_change(const char *path) {
    if (!g_app_ptr || !g_window_ptr) return;
    snprintf(g_app_ptr->config.font_path, sizeof(g_app_ptr->config.font_path), "%s", path);
    font_atlas_destroy(&g_app_ptr->renderer.font);
    font_atlas_create(&g_app_ptr->renderer.font, g_app_ptr->config.font_path,
                      g_app_ptr->config.font_size, g_app_ptr->dpi_scale,
                      g_app_ptr->config.font_weight);
#ifdef USE_METAL
    font_atlas_create_metal_texture(&g_app_ptr->renderer.font, platform_get_gpu_device(g_window_ptr));
    renderer_metal_set_atlas(&g_app_ptr->renderer, g_app_ptr->renderer.font.metal_texture);
#endif
    g_app_ptr->renderer.font.cell_width *= g_app_ptr->config.cell_width_scale;
    g_app_ptr->renderer.font.cell_height *= g_app_ptr->config.cell_height_scale;
    i32 w2, h2, fw2, fh2;
    platform_window_get_size(g_window_ptr, &w2, &h2);
    platform_window_get_framebuffer_size(g_window_ptr, &fw2, &fh2);
    app_update_layout(g_app_ptr, w2, h2, fw2, fh2);
}

static void on_native_theme_change(const char *name) {
    if (!g_app_ptr) return;
    const Theme *t = theme_get_by_name(name);
    if (t) {
        g_app_ptr->config.theme = t;
        snprintf(g_app_ptr->config.theme_name, sizeof(g_app_ptr->config.theme_name), "%s", name);
        app_apply_config(g_app_ptr);
    }
}

#ifdef PLATFORM_MACOS
static void show_native_settings_panel(PlatformWindow *window, AppState *app) {
    if (!window || !app) return;

    settings_refresh_fonts();

    i32 tc;
    const char **tn = theme_list_names(&tc);
    i32 fc = settings_get_font_count();
    const char *fn[64];
    const char *fp[64];
    bool fi[64];

    for (i32 i = 0; i < fc && i < 64; i++) {
        fn[i] = settings_get_font_name(i);
        fp[i] = settings_get_font_path(i);
        fi[i] = settings_get_font_installed(i);
    }

    platform_show_settings(window, fn, fp, fi, fc, tn, tc,
        app->config.font_path, app->config.theme_name,
        app->config.font_size, app->config.font_weight,
        app->config.opacity, app->config.tab_sleep_idle_minutes,
        on_native_font_change, on_native_theme_change, on_native_value_change);
}
#endif

/* =========================================================================
 * SFTP drag & drop upload (background thread)
 * ========================================================================= */

typedef struct {
    Session *session;
    char local_path[1024];
    char remote_dir[1024];
} SFTPUploadTask;

extern i32 sftp_upload(void *sftp_ptr, const char *local_path, const char *remote_path);

static void *sftp_upload_thread(void *arg) {
    SFTPUploadTask *task = (SFTPUploadTask *)arg;
    void *sftp = session_get_sftp(task->session);
    if (sftp) {
        /* Extract filename from local path */
        const char *fname = strrchr(task->local_path, '/');
        fname = fname ? fname + 1 : task->local_path;

        /* Build remote path: remote_dir/filename */
        char remote_path[2048];
        usize dir_len = strlen(task->remote_dir);
        if (dir_len > 0 && task->remote_dir[dir_len - 1] == '/')
            snprintf(remote_path, sizeof(remote_path), "%s%s", task->remote_dir, fname);
        else
            snprintf(remote_path, sizeof(remote_path), "%s/%s", task->remote_dir, fname);

        sftp_upload(sftp, task->local_path, remote_path);
    }
    free(task);
    return NULL;
}

static void on_native_value_change(const char *key, f32 value) {
    if (!g_app_ptr || !g_window_ptr) return;
    if (strcmp(key, "font_size") == 0) {
        if (value < 6.0f)  value = 6.0f;
        if (value > 96.0f) value = 96.0f;
        g_app_ptr->config.font_size = value;
        on_native_font_change(g_app_ptr->config.font_path);
    } else if (strcmp(key, "font_weight") == 0) {
        if (value < 0.0f) value = 0.0f;
        if (value > 2.0f) value = 2.0f;
        g_app_ptr->config.font_weight = value;
        on_native_font_change(g_app_ptr->config.font_path);
    } else if (strcmp(key, "tab_sleep_idle_minutes") == 0) {
        if (value < 0.0f) value = 0.0f;
        g_app_ptr->config.tab_sleep_idle_minutes = value;
    } else if (strcmp(key, "import_font") == 0) {
        const char *filepath = platform_open_file_dialog("Add Custom Font", "ttf,otf,ttc,otc");
        if (!filepath) return;

        char imported_path[1024];
        if (!settings_import_font_file(filepath, imported_path, sizeof(imported_path))) return;

        on_native_font_change(imported_path);
#ifdef PLATFORM_MACOS
        show_native_settings_panel(g_window_ptr, g_app_ptr);
#endif
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* platform_time_sec() depends on platform_init so use clock_gettime
     * directly for the pre-init phase timing. */
    struct timespec _ts;
    #define _NOW_MS() (clock_gettime(CLOCK_MONOTONIC, &_ts), \
                       ((f64)_ts.tv_sec * 1000.0 + (f64)_ts.tv_nsec / 1e6))
    f64 t_main_start = _NOW_MS();

    /* Install crash capture first thing: POSIX signal handlers + (in
     * platform_init) the ObjC uncaught-exception hook both write a report into
     * ~/.config/Liu/crasherrors/, surfaced on the next launch. */
    crashlog_init();

    /* Init libssh2 globally */
    libssh2_init(0);
    f64 t_after_ssh = _NOW_MS();

    /* Init platform */
    if (!platform_init()) {
        fprintf(stderr, "Failed to initialize platform\n");
        return 1;
    }
    f64 t_after_platform = _NOW_MS();

    PlatformWindowConfig win_cfg = {
        .title    = "Liu",
        .width    = 1200,
        .height   = 800,
        .resizable = true,
        .vsync    = true,
    };

    PlatformWindow *window = platform_window_create(&win_cfg);
    if (!window) {
        fprintf(stderr, "Failed to create window\n");
        platform_shutdown();
        return 1;
    }

    platform_gl_load();

    f32 dpi = platform_window_get_dpi_scale(window);

    /* Load user-imported themes before config resolution so theme_name can
     * bind to imported themes during app_init. */
    theme_load_user_themes();

    f64 t_after_theme = _NOW_MS();

    /* Init app */
    AppState app;
    if (!app_init(&app, win_cfg.width, win_cfg.height, dpi)) {
        fprintf(stderr, "Failed to initialize app\n");
        platform_window_destroy(window);
        platform_shutdown();
        return 1;
    }
    /* Notes Vault: ensure it exists and export the vault env ($LIU_VAULT +
     * $LIU_VAULT_HINT) so AI agents the user runs in any terminal can target it
     * (every local PTY inherits this process's environment). Before first tab. */
    app_notes_vault_ensure(&app);
    app_export_vault_env(&app);

    /* If the previous run crashed, surface a dismissible banner naming the
     * reason and the on-disk report (written to ~/.config/Liu/crasherrors/). */
    {
        char creason[256] = {0}, cpath[512] = {0};
        if (crashlog_take_pending(creason, sizeof creason, cpath, sizeof cpath)) {
            app_show_crash_banner(&app, creason, cpath);
        }
    }

    /* Restore persisted workspace groups (names, colors, collapsed). Tabs
     * always start ungrouped — sessions don't survive restart. */
    app_load_workspaces(&app);
    /* Host the notification server in-process so agent hooks (liu-notify send)
     * have something to talk to without a separate persistent background
     * daemon. No-op if another instance already hosts the socket; the service
     * stops when the app exits. */
    bool hosting_notify = notify_server_start();
    if (!hosting_notify) {
        fprintf(stderr, "liu: notify server not hosted (another instance?)\n");
    }

    /* Lifecycle-managed hooks: the instance that hosts the service installs the
     * agent notify hooks so they fire *only while Liu runs*, and removes them on
     * quit (see cleanup). This keeps us from leaving a permanent footprint in
     * ~/.claude/settings.json that would keep notifying when Liu is closed.
     * Idempotent — re-running replaces any stale Liu entries (e.g. a dev-path
     * command from a prior build, or a leftover after a crash). */
    if (hosting_notify) {
        char notify_bin[1024];
        if (resolve_liu_notify_path(notify_bin, sizeof notify_bin)) {
            /* Install the notify hook for every supported agent the user
             * actually has on PATH (Claude/Codex/Grok/OpenCode). Each lands
             * in that agent's own config surface; the set we installed is
             * remembered so atexit removes exactly those. */
            AgentInfo agents[AGENT_MAX] = {0};
            i32 n = agent_detect_available(agents, AGENT_MAX);
            for (i32 i = 0; i < n && g_installed_hook_count <
                            (i32)(sizeof g_installed_hook_agents /
                                  sizeof g_installed_hook_agents[0]); i++) {
                if (!agent_hook_supported(agents[i].id)) continue;
                AgentHookResult hr = agent_hook_install(agents[i].id, notify_bin);
                if (hr.ok) {
                    snprintf(g_installed_hook_agents[g_installed_hook_count],
                             AGENT_ID_CAP, "%s", agents[i].id);
                    g_installed_hook_count++;
                }
            }
            if (g_installed_hook_count > 0) {
                g_liu_hooks_installed = true;
                ensure_lifecycle_atexit();   /* remove on ANY exit path */
            }
        }
    }

    /* Kick the CLI-agent probe off the main thread so its ~250 access()
     * syscalls overlap with window/Metal bring-up. Picker opens join the
     * worker on demand; ⌘R refreshes synchronously. */
    app_detect_cli_agents_async(&app);
    f64 t_after_app_init = _NOW_MS();

    if (getenv("LIU_STARTUP_TIMING")) {
        fprintf(stderr, "startup: ssh=%.1fms platform+win=%.1fms themes=%.1fms app_init=%.1fms total=%.1fms\n",
                (t_after_ssh - t_main_start),
                (t_after_platform - t_after_ssh),
                (t_after_theme - t_after_platform),
                (t_after_app_init - t_after_theme),
                (t_after_app_init - t_main_start));
    }
    #undef _NOW_MS

#ifdef USE_METAL
    renderer_set_gpu(&app.renderer,
                     platform_get_gpu_device(window),
                     platform_get_gpu_layer(window),
                     platform_get_gpu_queue(window));
    /* Create Metal texture for font atlas (after renderer_set_gpu provides the device) */
    font_atlas_create_metal_texture(&app.renderer.font, platform_get_gpu_device(window));
    renderer_metal_set_atlas(&app.renderer, app.renderer.font.metal_texture);

    /* Load background image if configured */
    if (app.config.background_image[0]) {
        app.renderer.bg_opacity = app.config.background_opacity;
        app.renderer.bg_mode    = app.config.background_mode;
        if (renderer_load_background_image(&app.renderer, app.config.background_image,
                                           platform_get_gpu_device(window))) {
            /* Enable window transparency for background blur */
            if (app.config.background_blur) {
                platform_window_set_transparent(window, true);
            }
        }
    }
#endif

    /* Set palette config for profile/theme enumeration */
    palette_set_config(&app.config);

    /* Restore previous sessions, or create default local tab */
    if (!app_restore_sessions(&app)) {
        Session *local_session = session_create_local(app.grid_cols, app.grid_rows);
        if (!local_session || session_status(local_session) != SESSION_CONNECTED) {
            fprintf(stderr, "Failed to create local session: %s\n",
                    local_session ? session_error(local_session) : "alloc failed");
            app_destroy(&app);
            platform_window_destroy(window);
            platform_shutdown();
            return 1;
        }
        app_new_tab(&app, local_session, "Terminal");
    }

    /* Set global pointers for native settings callbacks */
    g_app_ptr = &app;
    g_window_ptr = window;

    /* Live-resize render hook: keep painting while AppKit holds the runloop
     * in NSEventTrackingRunLoopMode (window-edge drag). Without this, the
     * newly-exposed pixels in the larger drawable show as black until the
     * drag ends. */
    platform_set_render_callback(on_live_resize_render, &app);
    platform_set_titlebar_zoom_query(on_titlebar_zoom_query, &app);

    /* Apply option_as_alt setting to platform */
    platform_set_option_as_alt(app.config.option_as_alt);

    /* Apply window opacity from config */
    if (app.config.opacity < 1.0f) {
        platform_window_set_opacity(window, app.config.opacity);
    }

    /* Watch config file for hot-reload */
    platform_watch_file(config_file_path(), on_config_file_changed, &app);

    /* Set quake mode parameters from config and enable if configured */
    platform_set_quake_params(app.config.quake_height_ratio,
                              app.config.quake_animation_duration);
    if (app.config.quake_mode) {
        platform_set_quake_mode(window, true);
        app.quake_active = true;
        app_register_quake_hotkey(&app.config, window);
    }

    /* Shared state for passing modifier keys from KEY_DOWN to CHAR_INPUT */
    static u32 last_key_mods = 0;

    /* Split divider drag state */
    static bool split_dragging = false;
    static i32  split_drag_node = -1;
    /* Active markdown text-selection drag target (NULL when not selecting). */
    static FileBrowser *md_sel_fb = NULL;

    /* Selection drag state */
    static bool mouse_selecting = false;
    /* Single-click selections start as "pending" — we don't paint a 1-cell
     * selection rect just because the user pressed the mouse. The selection
     * activates only after the cursor leaves the press cell, matching every
     * other terminal/text editor. Word/line selections (double/triple click)
     * skip this and activate immediately because they have meaningful range. */
    static bool mouse_selecting_pending = false;
    static i32  mouse_press_col = 0, mouse_press_row = 0;
    static bool mouse_press_alt = false;
    /* Click counting for double/triple click */
    static f64 last_click_time = 0;
    static i32 last_click_col = -1, last_click_row = -1;
    static i32 click_count = 0;

    /* Frame skip: only render when something changed */
    bool needs_redraw = true;

    /* Present-rate cap while vsync is OFF. The loop disables vsync during
     * sustained motion (typing, agent output, theme generation, force-graph /
     * GIF animation) to skip the vblank stall — but with displaySync=NO a
     * self-driven burst redraw would free-run at FRAME_DT_BURST (250 Hz),
     * churning GPU drawables/command buffers at ~2x the panel rate and inflating
     * the transient IOSurface footprint. We can't show more frames than the
     * panel refreshes, so throttle presents to the display's max rate: keeps the
     * latency win (no stall, and a real input event still wakes the loop early)
     * without the GPU/power waste. `vsync_off_now` mirrors the block-static
     * s_vsync_disabled into loop scope so the timeout + render gates can see it. */
    bool vsync_off_now = false;
    f64  last_present_t = 0.0;
    f64  min_present_dt = 1.0 / 120.0;   /* refreshed on each vsync-off edge */

    /* Track whether any session is connected (for idle timeout calculation) */
    bool had_session_data_last_frame = false;
    bool had_visible_session_data_last_frame = false;
    f64 next_tab_sleep_scan = 0;

    /* One throttled (24h) silent update check on launch — a newer version
     * surfaces as a non-intrusive toast pointing at Settings › About. */
    updater_maybe_autocheck(&app.updater);

    /* =====================================================================
     * Main event loop -- idle event-driven
     *
     * Instead of busy-polling, we calculate a timeout and sleep until:
     *   - A platform event arrives (keyboard, mouse, resize, menu)
     *   - The timeout expires (cursor blink, session poll, etc.)
     *
     * Timeout strategy:
     *   - Active session data flowing: 0.001s (1ms, rapid poll for throughput)
     *   - Any connected sessions:      0.016s (60fps cap)
     *   - Cursor blink enabled:        time until next blink toggle
     *   - Sync output pending:         0.008s (quick re-check)
     *   - No sessions / all idle:      1.0s   (save power)
     * ===================================================================== */

    while (!platform_window_should_close(window)) {
        /* ---- Calculate wait timeout ---- */
        f64 timeout = 1.0;  /* default: idle, 1 second */
        f64 frame_now = platform_time_sec();
        bool interaction_boost = frame_now < app.interaction_boost_until;
        bool any_session_active = false;
        bool any_write_pending = false;
        bool any_sync_pending = false;

        for (i32 si = 0; si < app.tab_count; si++) {
            Session *s = app.tabs[si].session;
            if (s && session_status(s) == SESSION_CONNECTED) {
                any_session_active = true;
                if (session_pending_write_bytes(s) > 0)
                    any_write_pending = true;
            }
            Session *s2 = app.tabs[si].session2;
            if (s2 && session_status(s2) == SESSION_CONNECTED) {
                any_session_active = true;
                if (session_pending_write_bytes(s2) > 0)
                    any_write_pending = true;
            }

            Terminal *st = app.tabs[si].terminal;
            if (st && st->sync_pending) any_sync_pending = true;
            Terminal *st2 = app.tabs[si].terminal2;
            if (st2 && st2->sync_pending) any_sync_pending = true;

            if (any_session_active && any_write_pending && any_sync_pending) break;
        }

        /* If we just received session data, poll aggressively for more.
         * Throughput burst (FRAME_DT_BURST) is gated on *visible* data —
         * a chatty background tab must not pin the loop at 250 Hz with
         * no on-screen change. Hidden chatter still keeps any_session_active,
         * so we drop to the moderate CONNECTED_IDLE / ACTIVE bucket. */
        if (any_write_pending) {
            timeout = FRAME_DT_BURST;
        } else if (had_visible_session_data_last_frame) {
            timeout = interaction_boost ? FRAME_DT_BURST : FRAME_DT_INTERACTIVE;
        } else if (any_session_active) {
            timeout = interaction_boost ? FRAME_DT_ACTIVE : FRAME_DT_CONNECTED_IDLE;
        }

        /* Interactive UI should feel immediate while the user is active. */
        if (interaction_boost ||
            app.palette_active || app.search_active || app.ssh_dialog_active ||
            app.port_forward_dialog_active || app.sidebar_resizing ||
            mouse_selecting || split_dragging) {
            if (timeout > FRAME_DT_INTERACTIVE) timeout = FRAME_DT_INTERACTIVE;
        }

        /* Sync output (mode 2026) pending: poll frequently */
        if (any_sync_pending) {
            if (timeout > FRAME_DT_INTERACTIVE) timeout = FRAME_DT_INTERACTIVE;
        }

        /* Time-based visuals still need regular redraw, but much slower when idle.
         *
         * UI animations (anim_global_active) drop the timeout to FRAME_DT_BURST
         * unconditionally — VSync clamps it back to the display's refresh rate,
         * so on a 120 Hz ProMotion panel we get full 120 fps motion, on a 60 Hz
         * panel we cap at 60 fps. Without the burst, interaction_boost would
         * push us to FRAME_DT_ACTIVE (60 Hz cap) right when the user just
         * triggered a modal — exactly when smoothness matters most. */
        /* Animated GIF in the active FB tab: schedule wake-up exactly at
         * the next frame boundary so playback is smooth without burning
         * cycles between frames.
         *
         * On macOS ProMotion the display refresh rate is adaptive — when
         * the user is idle (no mouse / keys) the panel drops to ~24 Hz,
         * dragging vsync-paced render rate with it. That throttles the
         * GIF too. While an animated GIF is the focused content we
         * disable vsync so render pace is wall-clock driven; on close /
         * tab switch we restore. The state is edge-tracked across
         * iterations so we toggle the platform call only when needed. */
        {
            /* Edge-tracked vsync override.
             *
             * macOS ProMotion is adaptive: when the user is idle, the
             * panel drops to ~24 Hz, dragging vsync-paced render with
             * it. Anything that *we* know is animating wall-clock time
             * (GIF playback, an in-flight agent, …) suffers visible
             * stutter even though we redraw on schedule, because the
             * present blocks at the slow vsync boundary.
             *
             * The fix: while a sustained motion-driven operation is
             * active, switch the layer to displaySync = NO so presents
             * happen at our render pace instead of the panel's idle
             * refresh rate. We restore vsync the instant the condition
             * clears so static UI keeps tear-free presentation and we
             * stop spinning the GPU. The flag is a single bit covering
             * all sources so toggles are coalesced — the panel doesn't
             * thrash through state changes when, say, an animation ends
             * and a GIF is still playing. */
            static bool s_vsync_disabled = false;
            /* Pick whichever filebrowser is currently driving an animated
             * image. The sidebar (`app.filebrowser`) and a dedicated
             * TAB_FILEBROWSER both render through fb_render_viewer; either
             * can hold a live GIF. The sidebar takes priority because that
             * is the visible viewer in the common open-sidebar-then-click
             * flow — if both happened to be playing, the on-screen one is
             * the one whose pacing matters. */
            FileBrowser *gif_fb = NULL;
            if (app.filebrowser.open &&
                app.filebrowser.view_image_is_animated) {
                gif_fb = &app.filebrowser;
            } else {
                Tab *_atab = app_active_tab(&app);
                if (_atab && _atab->kind == TAB_FILEBROWSER && _atab->fb &&
                    _atab->fb->view_image_is_animated) {
                    gif_fb = _atab->fb;
                }
            }
            bool gif_active = (gif_fb != NULL);
            /* Create Theme phase 1: agent is generating — the modal
             * shows an indeterminate progress bar driven by wall-clock
             * fmod() so it must redraw at a steady rate to avoid
             * stutter under ProMotion's idle throttle. */
            bool agent_running = (app.create_theme_active &&
                                  app.create_theme_phase == 1);

            if (gif_active && gif_fb->view_image_next_frame_at > 0.0) {
                /* Drive the loop off the GIF's own frame clock instead of the
                 * display's refresh cadence. The wall-clock catch-up inside
                 * fb_render_viewer decides which frame is current; we only
                 * need to redraw when crossing a frame boundary, so:
                 *  - clamp timeout to (next_frame_at - now): poll wakes us
                 *    at the right moment for the next advance,
                 *  - flag needs_redraw only once the boundary is reached,
                 *    so between frames we sit idle instead of rendering
                 *    duplicate frames at the host's refresh rate.
                 * Net effect: a 10 fps GIF presents 10×/s even when the
                 * panel is at 24 Hz ProMotion idle, and a 60 fps GIF still
                 * gets a steady 60 redraws/s while it's visible. */
                f64 dt = gif_fb->view_image_next_frame_at - frame_now;
                if (dt <= 0.0) {
                    needs_redraw = true;
                    dt = 0.001;
                }
                if (timeout > dt) timeout = dt;
            } else if (gif_active) {
                /* GIF is playing but next_frame_at hasn't been set yet —
                 * happens on the very first frame after open, before the
                 * first render computes it. Force one redraw so the anchor
                 * gets populated; subsequent iterations hit the path above. */
                needs_redraw = true;
            }
            if (agent_running) {
                /* Force a redraw every frame and clamp timeout to the
                 * burst rate so the elapsed timer + progress bar tick
                 * smoothly even when no other event is firing. */
                if (timeout > FRAME_DT_BURST) timeout = FRAME_DT_BURST;
                needs_redraw = true;
            }

            /* Force-directed graph layout still settling: drive redraws at the
             * burst rate so the animation is smooth, then idle once it settles
             * (fb_render_graph clears graph_next_frame_at when it stops). */
            FileBrowser *graph_fb = NULL;
            if (app.filebrowser.open && app.filebrowser.view_mode == FVIEW_GRAPH) {
                graph_fb = &app.filebrowser;
            } else {
                Tab *_gt = app_active_tab(&app);
                if (_gt && _gt->kind == TAB_FILEBROWSER && _gt->fb &&
                    _gt->fb->view_mode == FVIEW_GRAPH) {
                    graph_fb = _gt->fb;
                }
            }
            /* graph_next_frame_at is set while the layout is settling AND while
             * the async build's spinner is up, so the loop keeps polling. */
            bool graph_animating = (graph_fb && graph_fb->graph_next_frame_at > 0.0);
            if (graph_animating) {
                if (timeout > FRAME_DT_BURST) timeout = FRAME_DT_BURST;
                needs_redraw = true;
            }

            /* Split-divider / sidebar drags also want wall-clock pacing: the
             * vsync-locked present fights the drag event rate and the panel
             * oscillates 120<->60, dropping frames. Pace against the clock for
             * the duration of the drag. */
            /* Active typing also wants wall-clock pacing. With vsync on, each
         * keystroke echo stalls until the next display refresh — on a ProMotion
         * panel idle-throttled to ~24 Hz that's ~25-42 ms of perceived input
         * lag, and the echo frame (one iteration after the keypress) would wait
         * a second refresh. Keyboard-gated (app.last_key_time, set only on
         * key/char events) so visible streaming output is never disturbed and
         * cannot tear a scroll. The decision uses last_key_time from the prior
         * iteration, so the echo frame — the one that actually paints the typed
         * glyph — is the frame that presents vsync-off; the cursor-only keypress
         * frame painting one refresh slower is invisible. Edge-tracked below, so
         * a continuous burst toggles displaySync at most once, not per key. */
        bool typing_active =
            app.last_key_time > 0.0 &&
            (frame_now - app.last_key_time) < TYPING_VSYNC_WINDOW_SEC;
        bool wants_vsync_off = gif_active || agent_running || graph_animating ||
                                   split_dragging || app.sidebar_resizing ||
                                   typing_active;
            if (wants_vsync_off && !s_vsync_disabled) {
                platform_window_set_vsync(window, false);
                s_vsync_disabled = true;
                /* Sample the current panel's max rate so the present cap
                 * matches the display the window is actually on (120 on
                 * ProMotion, 60 otherwise). Only on the edge — cheap. */
                f32 hz = platform_window_max_refresh_hz(window);
                min_present_dt = (hz > 1.0f) ? (1.0 / (f64)hz) : (1.0 / 60.0);
            } else if (!wants_vsync_off && s_vsync_disabled) {
                /* Restore — Liu starts with vsync on (see WindowConfig
                 * in main()), so unconditionally re-enable here. If the
                 * user later gets a runtime vsync toggle, this state
                 * becomes the "previous user setting" tracker. */
                platform_window_set_vsync(window, true);
                s_vsync_disabled = false;
            }
            vsync_off_now = s_vsync_disabled;

            /* Split/sidebar drags never fire AppKit's viewWillStartLiveResize
             * (that's window-edge only), so manage CAMetalLayer.presentsWith-
             * Transaction here: it commits the layer-size change + the drawable
             * present in one Core Animation transaction, so the resized content
             * and the new size land atomically (no half-resized frame). Paired
             * with vsync-off above. No-op on non-Metal backends. */
            static bool s_presents_with_txn = false;
            bool wants_txn = split_dragging || app.sidebar_resizing;
            if (wants_txn && !s_presents_with_txn) {
                platform_window_set_presents_with_transaction(window, true);
                s_presents_with_txn = true;
            } else if (!wants_txn && s_presents_with_txn) {
                platform_window_set_presents_with_transaction(window, false);
                s_presents_with_txn = false;
            }
        }

        if (anim_global_active()) {
            if (timeout > FRAME_DT_BURST) timeout = FRAME_DT_BURST;
        } else if (anim_soft_active()) {
            /* Only a low-rate visual (agent accent) is running — wake at ~30 Hz
             * instead of pinning the panel rate for a streaming agent. */
            if (timeout > FRAME_DT_SOFT_ANIM) timeout = FRAME_DT_SOFT_ANIM;
        } else if (app.cursor_animating || app.toast_start_time > 0 || app.bell_flash_time > 0) {
            if (timeout > (interaction_boost ? FRAME_DT_ACTIVE : FRAME_DT_INTERACTIVE)) {
                timeout = interaction_boost ? FRAME_DT_ACTIVE : FRAME_DT_INTERACTIVE;
            }
        } else if (app.config.cursor_blink && app.window_focused) {
            if (interaction_boost) {
                if (timeout > FRAME_DT_ACTIVE) timeout = FRAME_DT_ACTIVE;
            } else {
                /* Sleep until the next discrete blink edge (0.5s grid).
                 * Wakes exactly twice per second instead of 8 Hz. */
                f64 phase = frame_now - floor(frame_now);
                f64 dt_to_edge = (phase < 0.5) ? (0.5 - phase) : (1.0 - phase);
                if (dt_to_edge < 0.001) dt_to_edge = 0.001;
                if (timeout > dt_to_edge) timeout = dt_to_edge;
            }
        }

        /* First frame or pending redraw from layout change: don't sleep */
        if (needs_redraw) {
            timeout = 0.0;
            /* Present cap (vsync off): if the last present was under one refresh
             * period ago, sleep until the next slot instead of spinning at the
             * burst rate. A genuine input event still interrupts the wait, so
             * echo latency is unaffected; only self-driven burst redraws throttle
             * to the panel rate. The render gate below enforces the same bound. */
            if (vsync_off_now) {
                f64 wait = min_present_dt - (frame_now - last_present_t);
                if (wait > 0.0) timeout = wait;
            }
        }

        /* ---- Wait for events or timeout ---- */
        if (timeout > 0.0) {
            platform_wait_events(timeout);
        }
        /* Always drain remaining platform events after waking.
         * platform_wait_events only dequeues one NSEvent; poll_events
         * drains all remaining events from the OS event queue. */
        platform_poll_events();
        platform_poll_file_watches();

        /* Config hot-reload: debounce then apply */
        if (g_config_changed_time > 0) {
            f64 now = platform_time_sec();
            if (now - g_config_changed_time >= CONFIG_RELOAD_DEBOUNCE) {
                g_config_changed_time = 0;

                /* Save values that need special handling on change */
                char old_font_path[512];
                f32  old_font_size = app.config.font_size;
                f32  old_cell_w_scale = app.config.cell_width_scale;
                f32  old_cell_h_scale = app.config.cell_height_scale;
                snprintf(old_font_path, sizeof(old_font_path), "%s", app.config.font_path);

                if (config_reload(&app.config)) {
                    /* Resolve theme pointer from name */
                    const Theme *t = theme_get_by_name(app.config.theme_name);
                    if (t) app.config.theme = t;

                    /* Check if font changed */
                    bool font_changed = (strcmp(old_font_path, app.config.font_path) != 0 ||
                                         old_font_size != app.config.font_size ||
                                         old_cell_w_scale != app.config.cell_width_scale ||
                                         old_cell_h_scale != app.config.cell_height_scale);

                    if (font_changed) {
                        font_atlas_destroy(&app.renderer.font);
                        font_atlas_create(&app.renderer.font, app.config.font_path,
                                          app.config.font_size, app.dpi_scale,
                                          app.config.font_weight);
#ifdef USE_METAL
                        font_atlas_create_metal_texture(&app.renderer.font, platform_get_gpu_device(window));
                        renderer_metal_set_atlas(&app.renderer, app.renderer.font.metal_texture);
#endif
                        app.renderer.font.cell_width *= app.config.cell_width_scale;
                        app.renderer.font.cell_height *= app.config.cell_height_scale;
                    }

                    /* Sync option_as_alt and window opacity */
                    platform_set_option_as_alt(app.config.option_as_alt);
                    platform_window_set_opacity(window, app.config.opacity);

                    /* Apply theme/color changes and invalidate render caches */
                    app_apply_config(&app);

                    /* Recalculate layout (grid dimensions may have changed) */
                    i32 w2, h2, fw2, fh2;
                    platform_window_get_size(window, &w2, &h2);
                    platform_window_get_framebuffer_size(window, &fw2, &fh2);
                    app_update_layout(&app, w2, h2, fw2, fh2);

                    needs_redraw = true;
                }
            }
        }

        /* Dev hook: LIU_OPEN_SETTINGS=<tab#> opens Settings on launch — lets
     * scripted screenshots reach a tab without driving the GUI. */
    {
        static bool s_settings_hook_done = false;
        const char *st = getenv("LIU_OPEN_SETTINGS");
        if (st && !s_settings_hook_done) {
            s_settings_hook_done = true;
            app.settings.active_tab = atoi(st);
            if (!app.settings.open) settings_toggle(&app.settings);
        }
    }

    /* Process platform events */
        PlatformEvent event;
        bool had_event = false;
        while (platform_next_event(&event)) {
            had_event = true;
            if (event.type == EVENT_KEY_DOWN || event.type == EVENT_CHAR_INPUT ||
                event.type == EVENT_MOUSE_DOWN || event.type == EVENT_MOUSE_UP ||
                event.type == EVENT_MOUSE_MOVE || event.type == EVENT_SCROLL ||
                event.type == EVENT_RESIZE) {
                app_note_interaction(&app, platform_time_sec());
            }
            /* Keyboard-only typing marker — drives the vsync-off-while-typing
             * path so keystroke echo isn't pinned to ProMotion's idle refresh.
             * Deliberately narrower than app_note_interaction (no mouse, no
             * output) to keep visible streaming output tear-free. */
            if (event.type == EVENT_KEY_DOWN || event.type == EVENT_CHAR_INPUT) {
                app.last_key_time = platform_time_sec();
            }
            /* Stale-momentum guard. A trackpad fling keeps delivering inertial
             * (momentum-phase) scroll events for ~1 s after the fingers lift.
             * If the UI surface that owns scrolling changes during that tail —
             * e.g. the user flings the terminal then opens Cmd+K — those events
             * belong to the old surface and would otherwise scroll the new one
             * on its own. Pin each gesture to the surface it began on: record
             * the context on every user-driven (non-momentum) scroll, and drop
             * momentum events once the context no longer matches. */
            if (event.type == EVENT_SCROLL) {
                static i32 scroll_gesture_ctx = -1;
                i32 ctx_now = app_scroll_context_id(&app);
                if (event.scroll.momentum) {
                    if (scroll_gesture_ctx >= 0 && ctx_now != scroll_gesture_ctx)
                        continue;   /* stale inertia from a different surface */
                } else {
                    scroll_gesture_ctx = ctx_now;
                }
            }
            /* Settings panel intercepts clicks and scroll when open */
            if (app.settings.open) {
                if (event.type == EVENT_MOUSE_DOWN) {
                    settings_handle_click(&app.settings, (f32)event.mouse.x,
                                          (f32)event.mouse.y,
                                          (f32)app.fb_width, (f32)app.fb_height,
                                          app.dpi_scale,
                                          8.0f * app.dpi_scale,
                                          16.0f * app.dpi_scale);
                    /* Check if settings changed font or theme */
                    if (app.settings.needs_font_reload) {
                        font_atlas_destroy(&app.renderer.font);
                        font_atlas_create(&app.renderer.font, app.config.font_path,
                                          app.config.font_size, app.dpi_scale,
                                          app.config.font_weight);
#ifdef USE_METAL
                        font_atlas_create_metal_texture(&app.renderer.font, platform_get_gpu_device(window));
                        renderer_metal_set_atlas(&app.renderer, app.renderer.font.metal_texture);
#endif
                        app.renderer.font.cell_width *= app.config.cell_width_scale;
                        app.renderer.font.cell_height *= app.config.cell_height_scale;
                        app.settings.needs_font_reload = false;
                        i32 w2, h2, fw2, fh2;
                        platform_window_get_size(window, &w2, &h2);
                        platform_window_get_framebuffer_size(window, &fw2, &fh2);
                        app_update_layout(&app, w2, h2, fw2, fh2);
                    }
                    if (app.settings.needs_layout) {
                        app_apply_config(&app);
                        platform_window_set_opacity(window, app.config.opacity);
                        i32 lw, lh, lfw, lfh;
                        platform_window_get_size(window, &lw, &lh);
                        platform_window_get_framebuffer_size(window, &lfw, &lfh);
                        app_update_layout(&app, lw, lh, lfw, lfh);
                        app.settings.needs_layout = false;
                    }
                    /* Appearance tab → "Create Theme..." button: open the
                     * modal once. The modal owns the rest of the flow.
                     * Close Settings first: the settings overlay paints AFTER
                     * the modal in app_render and owns input while open, so an
                     * opened-from-Settings modal was invisible + unreachable
                     * behind the panel (the button looked dead). */
                    if (app.settings.requests_create_theme) {
                        app.settings.requests_create_theme = false;
                        app.settings.open = false;
                        app_open_create_theme(&app);
                    }
                    /* Appearance tab → ✕ next to a user theme: delete the
                     * on-disk JSON, fall back to THEME_DARK if the active
                     * theme was the one we just removed. */
                    if (app.settings.theme_to_delete[0]) {
                        const char *name = app.settings.theme_to_delete;
                        bool was_active =
                            (strcmp(app.config.theme_name, name) == 0);
                        if (theme_delete_user(name)) {
                            if (was_active) app_switch_theme(&app, &THEME_DARK);
                            char msg[160];
                            snprintf(msg, sizeof(msg),
                                     "Deleted theme: %s", name);
                            app_show_toast(&app, msg);
                        } else {
                            app_show_toast(&app, "Could not delete theme");
                        }
                        app.settings.theme_to_delete[0] = '\0';
                    }
                    /* Vault-tab side-channel: "browse" means the user asked
                     * us to open the browser after closing Settings. */
                    if (!app.settings.open &&
                        strcmp(app.settings.vault_status, "browse") == 0) {
                        app.settings.vault_status[0] = '\0';
                        app_vault_browser_open(&app);
                    }
                    continue;
                }
                if (event.type == EVENT_SCROLL) {
                    settings_handle_scroll(&app.settings, event.scroll.dy,
                                           event.scroll.precise, app.dpi_scale);
                    continue;
                }
                if (event.type == EVENT_KEY_DOWN) {
                    u32 skey = event.key.key;
                    if (settings_handle_key(&app.settings, skey, event.key.mods)) continue;
                    if (skey == KEY_ESCAPE) {
                        if (app.settings.editing_opacity) app.settings.editing_opacity = false;
                        else app.settings.open = false;
                        continue;
                    }
                }
                if (event.type == EVENT_CHAR_INPUT) {
                    if (settings_handle_char(&app.settings, event.char_input.codepoint))
                        continue;
                }
                if (event.type != EVENT_RESIZE && event.type != EVENT_CLOSE &&
                    event.type != EVENT_DPI_CHANGE &&
                    event.type != EVENT_DROP_FILE &&
                    event.type != EVENT_DRAG_ENTER &&
                    event.type != EVENT_DRAG_EXIT &&
                    event.type >= EVENT_MENU_NEW_TAB) {
                    /* let menu events through */
                } else if (event.type != EVENT_RESIZE && event.type != EVENT_CLOSE &&
                           event.type != EVENT_DPI_CHANGE &&
                           event.type != EVENT_DROP_FILE &&
                           event.type != EVENT_DRAG_ENTER &&
                           event.type != EVENT_DRAG_EXIT) {
                    continue;
                }
            }

            /* Sites / dev-server manager overlay intercepts events when open */
            if (app.sites.active) {
                if (event.type == EVENT_MOUSE_DOWN) {
                    sites_handle_click(&app, (f32)event.mouse.x, (f32)event.mouse.y);
                    continue;
                }
                if (event.type == EVENT_MOUSE_MOVE) {
                    app.hover_x = (f32)event.mouse.x;
                    app.hover_y = (f32)event.mouse.y;
                    sites_handle_mouse_move(&app, (f32)event.mouse.x, (f32)event.mouse.y);
                    continue;
                }
                if (event.type == EVENT_SCROLL) {
                    sites_handle_scroll(&app, event.scroll.dy, event.scroll.precise,
                                        app.hover_x, app.hover_y);
                    continue;
                }
                if (event.type == EVENT_KEY_DOWN) {
                    u32 skey = event.key.key;
                    if (sites_handle_key(&app, skey, event.key.mods)) continue;
                    if (skey == KEY_ESCAPE) {
                        if (app.sites.addform_active) app.sites.addform_active = false;
                        else app.sites.active = false;
                        continue;
                    }
                    /* The toggle binding (Cmd+Shift+S) must also close the overlay —
                     * otherwise the swallow logic below eats it and only Esc works. */
                    if (keybind_lookup(&app.keybinds, (KeyCode)skey, event.key.mods)
                            == ACT_SITE_MANAGER) {
                        app.sites.active = false;
                        continue;
                    }
                }
                if (event.type == EVENT_CHAR_INPUT) {
                    if (sites_handle_char(&app, event.char_input.codepoint)) continue;
                }
                /* Let resize/close/dpi/drag + menu events through; swallow the rest. */
                if (event.type != EVENT_RESIZE && event.type != EVENT_CLOSE &&
                    event.type != EVENT_DPI_CHANGE &&
                    event.type != EVENT_DROP_FILE &&
                    event.type != EVENT_DRAG_ENTER &&
                    event.type != EVENT_DRAG_EXIT &&
                    event.type < EVENT_MENU_NEW_TAB) {
                    continue;
                }
            }

            /* Key manager overlay intercepts events when active */
            if (app.key_manager_active) {
                if (event.type == EVENT_KEY_DOWN) {
                    u32 key = event.key.key;
                    if (key == KEY_ESCAPE) {
                        if (app.keygen_confirm_delete) {
                            app.keygen_confirm_delete = false;
                        } else if (app.keygen_form_active) {
                            app.keygen_form_active = false;
                        } else {
                            app.key_manager_active = false;
                        }
                        continue;
                    }
                    if (app.keygen_confirm_delete) {
                        if (key == KEY_ENTER) {
                            if (app.keygen_delete_idx >= 0 && app.keygen_delete_idx < app.key_list_count) {
                                bool ok = ssh_delete_key(app.key_list[app.keygen_delete_idx].path);
                                if (ok) {
                                    snprintf(app.keygen_status, sizeof(app.keygen_status), "Key deleted");
                                    char ssh_dir[512];
                                    ssh_get_default_dir(ssh_dir, sizeof(ssh_dir));
                                    ssh_scan_keys(ssh_dir, app.key_list, &app.key_list_count, app.key_list_cap);
                                    app.key_list_selected = -1;
                                } else {
                                    snprintf(app.keygen_status, sizeof(app.keygen_status), "Error: Delete failed");
                                }
                            }
                            app.keygen_confirm_delete = false;
                        }
                        continue;
                    }
                    if (app.keygen_form_active) {
                        if (key == KEY_TAB) {
                            app.keygen_field = (app.keygen_field + 1) % 4;
                        } else if (key == KEY_BACKSPACE) {
                            char *field = app.keygen_field == 0 ? app.keygen_filename :
                                          app.keygen_field == 1 ? app.keygen_passphrase :
                                          app.keygen_field == 2 ? app.keygen_passphrase2 :
                                                                   app.keygen_comment;
                            i32 flen = (i32)strlen(field);
                            if (flen > 0) field[flen-1] = '\0';
                        } else if (key == KEY_ENTER) {
                            app_keygen_submit(&app);
                        }
                        continue;
                    }
                    /* Key list navigation */
                    if (key == KEY_UP) {
                        if (app.key_list_selected > 0) app.key_list_selected--;
                    } else if (key == KEY_DOWN) {
                        if (app.key_list_selected < app.key_list_count - 1)
                            app.key_list_selected++;
                    } else if (key == KEY_ENTER) {
                        if (app.key_list_selected >= 0 && app.key_list_selected < app.key_list_count) {
                            char *pub = ssh_read_public_key(app.key_list[app.key_list_selected].pub_path);
                            if (pub) {
                                platform_clipboard_set(pub);
                                free(pub);
                                snprintf(app.keygen_status, sizeof(app.keygen_status), "Public key copied to clipboard");
                            }
                        }
                    } else if (key == KEY_DELETE || key == KEY_BACKSPACE) {
                        if (app.key_list_selected >= 0 && app.key_list_selected < app.key_list_count) {
                            app.keygen_confirm_delete = true;
                            app.keygen_delete_idx = app.key_list_selected;
                        }
                    }
                    continue;
                }
                if (event.type == EVENT_CHAR_INPUT) {
                    if (app.keygen_form_active) {
                        u32 cp = event.char_input.codepoint;
                        if (cp >= 32 && cp < 127) {
                            char *field = app.keygen_field == 0 ? app.keygen_filename :
                                          app.keygen_field == 1 ? app.keygen_passphrase :
                                          app.keygen_field == 2 ? app.keygen_passphrase2 :
                                                                   app.keygen_comment;
                            i32 max_len = app.keygen_field == 0 ? 127 :
                                          app.keygen_field == 3 ? 127 : 255;
                            i32 flen = (i32)strlen(field);
                            if (flen < max_len) {
                                field[flen] = (char)cp;
                                field[flen+1] = '\0';
                            }
                        }
                    }
                    continue;
                }
                if (event.type == EVENT_MOUSE_DOWN) {
                    f32 mx = (f32)event.mouse.x;
                    f32 my = (f32)event.mouse.y;
                    f32 dpi = app.dpi_scale;
                    f32 ui_cw = 8.0f * dpi;
                    f32 dw = 600 * dpi, dh = 480 * dpi;
                    f32 ddx = ((f32)app.fb_width - dw) / 2;
                    f32 ddy = ((f32)app.fb_height - dh) / 2;
                    f32 title_h = 32 * dpi;

                    /* Click outside panel closes it */
                    if (mx < ddx || mx > ddx + dw || my < ddy || my > ddy + dh) {
                        if (!app.keygen_confirm_delete) {
                            app.key_manager_active = false;
                            continue;
                        }
                    }

                    /* Delete confirmation dialog click handling */
                    if (app.keygen_confirm_delete) {
                        f32 cd_w = 320 * dpi, cd_h = 100 * dpi;
                        f32 cd_x = ((f32)app.fb_width - cd_w) / 2;
                        f32 cd_y = ((f32)app.fb_height - cd_h) / 2;
                        f32 cd_btn_y = cd_y + cd_h - 32 * dpi;
                        f32 cd_btn_h = 24 * dpi;

                        if (mx >= cd_x + 12 && mx <= cd_x + 12 + 80 * dpi &&
                            my >= cd_btn_y && my <= cd_btn_y + cd_btn_h) {
                            if (app.keygen_delete_idx >= 0 && app.keygen_delete_idx < app.key_list_count) {
                                bool ok = ssh_delete_key(app.key_list[app.keygen_delete_idx].path);
                                if (ok) {
                                    snprintf(app.keygen_status, sizeof(app.keygen_status), "Key deleted");
                                    char ssh_dir[512];
                                    ssh_get_default_dir(ssh_dir, sizeof(ssh_dir));
                                    ssh_scan_keys(ssh_dir, app.key_list, &app.key_list_count, app.key_list_cap);
                                    app.key_list_selected = -1;
                                }
                            }
                            app.keygen_confirm_delete = false;
                        }
                        if (mx >= cd_x + cd_w - 92 * dpi && mx <= cd_x + cd_w - 12 &&
                            my >= cd_btn_y && my <= cd_btn_y + cd_btn_h) {
                            app.keygen_confirm_delete = false;
                        }
                        continue;
                    }

                    /* Close button [X] */
                    f32 close_x = ddx + dw - 28 * dpi;
                    if (mx >= close_x && mx <= ddx + dw && my >= ddy && my <= ddy + title_h) {
                        app.key_manager_active = false;
                        continue;
                    }

                    if (!app.keygen_form_active) {
                        f32 btn_row_y = ddy + title_h + 6;
                        f32 btn_h = 24 * dpi;
                        f32 gen_btn_w = 140 * dpi;
                        if (mx >= ddx + 12 && mx <= ddx + 12 + gen_btn_w &&
                            my >= btn_row_y && my <= btn_row_y + btn_h) {
                            app_open_keygen_form(&app);
                            continue;
                        }
                        f32 ref_btn_x = ddx + 12 + gen_btn_w + 8;
                        f32 ref_btn_w = 70 * dpi;
                        if (mx >= ref_btn_x && mx <= ref_btn_x + ref_btn_w &&
                            my >= btn_row_y && my <= btn_row_y + btn_h) {
                            char ssh_dir[512];
                            ssh_get_default_dir(ssh_dir, sizeof(ssh_dir));
                            ssh_scan_keys(ssh_dir, app.key_list, &app.key_list_count, app.key_list_cap);
                            app.key_list_selected = -1;
                            continue;
                        }
                        f32 ui_ch = 16.0f * dpi;
                        f32 list_y = btn_row_y + btn_h + 8 + ui_ch + 8;
                        f32 item_h = ui_ch + 8;
                        if (my >= list_y) {
                            i32 clicked = app.key_list_scroll + (i32)((my - list_y) / item_h);
                            if (clicked >= 0 && clicked < app.key_list_count) {
                                app.key_list_selected = clicked;
                            }
                        }
                        if (app.key_list_selected >= 0 && app.key_list_selected < app.key_list_count) {
                            f32 bot_y = ddy + dh - 40 * dpi;
                            f32 bot_h = 28 * dpi;
                            f32 copy_w = 120 * dpi;
                            if (mx >= ddx + 12 && mx <= ddx + 12 + copy_w &&
                                my >= bot_y && my <= bot_y + bot_h) {
                                char *pub = ssh_read_public_key(app.key_list[app.key_list_selected].pub_path);
                                if (pub) {
                                    platform_clipboard_set(pub);
                                    free(pub);
                                    snprintf(app.keygen_status, sizeof(app.keygen_status), "Public key copied to clipboard");
                                }
                            }
                            f32 del_w = 90 * dpi;
                            f32 del_x = ddx + dw - del_w - 12;
                            if (mx >= del_x && mx <= del_x + del_w &&
                                my >= bot_y && my <= bot_y + bot_h) {
                                app.keygen_confirm_delete = true;
                                app.keygen_delete_idx = app.key_list_selected;
                            }
                        }
                    } else {
                        /* Keygen form click handling */
                        f32 form_y = ddy + title_h + 12;
                        f32 field_h = 28 * dpi;
                        f32 ui_ch = 16.0f * dpi;
                        f32 label_w = 100 * dpi;
                        form_y += ui_ch + 12;
                        static const char *type_names[] = {
                            "Ed25519", "RSA 2048", "RSA 4096", "ECDSA 256", "ECDSA 384", "ECDSA 521"
                        };
                        if (my >= form_y && my <= form_y + field_h) {
                            f32 type_x = ddx + label_w;
                            for (i32 ti = 0; ti < 6; ti++) {
                                f32 tw = ((i32)strlen(type_names[ti]) + 2) * ui_cw;
                                if (mx >= type_x && mx <= type_x + tw) {
                                    app.keygen_type = ti;
                                    break;
                                }
                                type_x += tw + 4;
                            }
                        }
                        form_y += field_h + 10;
                        if (my >= form_y && my <= form_y + field_h && mx >= ddx + label_w)
                            app.keygen_field = 0;
                        form_y += field_h + 8;
                        if (my >= form_y && my <= form_y + field_h && mx >= ddx + label_w)
                            app.keygen_field = 1;
                        form_y += field_h + 8;
                        if (my >= form_y && my <= form_y + field_h && mx >= ddx + label_w)
                            app.keygen_field = 2;
                        form_y += field_h + 8;
                        if (my >= form_y && my <= form_y + field_h && mx >= ddx + label_w)
                            app.keygen_field = 3;

                        f32 bot_y = ddy + dh - 44 * dpi;
                        f32 btn_bh = 28 * dpi;
                        f32 gen_w = 100 * dpi;
                        f32 gen_x = ddx + dw - gen_w - 12;
                        if (mx >= gen_x && mx <= gen_x + gen_w &&
                            my >= bot_y && my <= bot_y + btn_bh) {
                            app_keygen_submit(&app);
                        }
                        f32 cancel_w = 80 * dpi;
                        f32 cancel_x = ddx + dw - gen_w - cancel_w - 24;
                        if (mx >= cancel_x && mx <= cancel_x + cancel_w &&
                            my >= bot_y && my <= bot_y + btn_bh) {
                            app.keygen_form_active = false;
                        }
                    }
                    continue;
                }
                if (event.type == EVENT_SCROLL) {
                    if (!app.keygen_form_active) {
                        /* Trackpad pixel deltas would teleport the row index;
                         * accumulate to one row tick per ~24 px (default row
                         * height). */
                        static f32 kl_accum = 0.0f;
                        i32 ticks = 0;
                        if (event.scroll.precise) {
                            kl_accum += event.scroll.dy;
                            const f32 step_px = 24.0f * app.dpi_scale;
                            while (kl_accum >=  step_px) { ticks--; kl_accum -= step_px; }
                            while (kl_accum <= -step_px) { ticks++; kl_accum += step_px; }
                        } else {
                            ticks = -(i32)event.scroll.dy;
                            if (ticks == 0 && event.scroll.dy != 0)
                                ticks = event.scroll.dy > 0 ? -1 : 1;
                            kl_accum = 0.0f;
                        }
                        app.key_list_scroll += ticks;
                        if (app.key_list_scroll < 0) app.key_list_scroll = 0;
                        i32 max_scroll = app.key_list_count > 10 ? app.key_list_count - 10 : 0;
                        if (app.key_list_scroll > max_scroll) app.key_list_scroll = max_scroll;
                    }
                    continue;
                }
                if (event.type != EVENT_RESIZE && event.type != EVENT_CLOSE &&
                    event.type < EVENT_MENU_NEW_TAB) {
                    continue;
                }
            }

            /* Host key verification dialog intercepts events */
            if (app.hostkey_dialog_active) {
                if (event.type == EVENT_KEY_DOWN) {
                    u32 hk_key = event.key.key;
                    if (hk_key == KEY_ESCAPE) {
                        session_hostkey_respond(app.hostkey_session, false);
                        app.hostkey_dialog_active = false;
                        app.hostkey_session = NULL;
                        continue;
                    }
                    if (hk_key == KEY_ENTER) {
                        session_hostkey_respond(app.hostkey_session, true);
                        app.hostkey_dialog_active = false;
                        app.hostkey_session = NULL;
                        continue;
                    }
                }
                if (event.type == EVENT_MOUSE_DOWN) {
                    f32 hx = (f32)event.mouse.x;
                    f32 hy = (f32)event.mouse.y;
                    f32 dpi = app.dpi_scale;
                    f32 dw = 500 * dpi, dh = 260 * dpi;
                    f32 ddx = ((f32)app.fb_width - dw) / 2;
                    f32 ddy = ((f32)app.fb_height - dh) / 2;

                    f32 btn_h = 28 * dpi;
                    f32 btn_w_accept = 120 * dpi;
                    f32 btn_w_reject = 80 * dpi;
                    f32 btn_spacing = 12;
                    f32 reject_x = ddx + dw - btn_w_reject - 12;
                    f32 btn_y = ddy + dh - btn_h - 12;
                    f32 accept_x = reject_x - btn_w_accept - btn_spacing;

                    if (hx >= accept_x && hx <= accept_x + btn_w_accept &&
                        hy >= btn_y && hy <= btn_y + btn_h) {
                        session_hostkey_respond(app.hostkey_session, true);
                        app.hostkey_dialog_active = false;
                        app.hostkey_session = NULL;
                        continue;
                    }
                    if (hx >= reject_x && hx <= reject_x + btn_w_reject &&
                        hy >= btn_y && hy <= btn_y + btn_h) {
                        session_hostkey_respond(app.hostkey_session, false);
                        app.hostkey_dialog_active = false;
                        app.hostkey_session = NULL;
                        continue;
                    }
                }
                if (event.type != EVENT_RESIZE && event.type != EVENT_CLOSE)
                    continue;
            }

            /* Close confirm (agent running) intercepts events. The panel and
             * button geometry mirrors the renderer in src/ui/ui.c — keep them
             * in sync when one moves. */
            if (app.close_confirm_active) {
                bool cc_do_close = false;
                if (event.type == EVENT_KEY_DOWN) {
                    u32 cc_key = event.key.key;
                    if (cc_key == KEY_ESCAPE) {
                        app.close_confirm_active = false;
                        continue;
                    }
                    if (cc_key == KEY_ENTER) cc_do_close = true;
                }
                if (event.type == EVENT_MOUSE_DOWN) {
                    f32 ccx = (f32)event.mouse.x;
                    f32 ccy = (f32)event.mouse.y;
                    f32 dpi = app.dpi_scale;
                    /* CLOSE-CONFIRM GEOMETRY — MUST match render in src/ui/ui.c. */
                    f32 pad = 22.0f * dpi;
                    f32 dw = 480 * dpi, dh = 188 * dpi;
                    f32 ddx = ((f32)app.fb_width - dw) / 2;
                    f32 ddy = ((f32)app.fb_height - dh) / 2;
                    f32 btn_h = 34 * dpi;
                    f32 btn_w_close = 120 * dpi;
                    f32 btn_w_cancel = 96 * dpi;
                    f32 close_x = ddx + dw - pad - btn_w_close;
                    f32 btn_y = ddy + dh - pad - btn_h;
                    f32 cancel_x = close_x - 12.0f * dpi - btn_w_cancel;

                    if (ccx >= close_x && ccx <= close_x + btn_w_close &&
                        ccy >= btn_y && ccy <= btn_y + btn_h) {
                        cc_do_close = true;
                    } else if ((ccx >= cancel_x && ccx <= cancel_x + btn_w_cancel &&
                                ccy >= btn_y && ccy <= btn_y + btn_h) ||
                               ccx < ddx || ccx > ddx + dw ||
                               ccy < ddy || ccy > ddy + dh) {
                        /* Cancel button or click outside the panel. */
                        app.close_confirm_active = false;
                        continue;
                    }
                }
                if (cc_do_close) {
                    app.close_confirm_active = false;
                    if (app.close_confirm_group >= 0) {
                        app_close_tab_group(&app, app.close_confirm_group);
                    } else if (app.close_confirm_pane >= 0) {
                        /* Pane close never empties the tab strip. */
                        if (app.close_confirm_tab >= 0 &&
                            app.close_confirm_tab < app.tab_count)
                            app_close_split_pane(&app, app.close_confirm_tab,
                                                 app.close_confirm_pane);
                        continue;
                    } else if (app.close_confirm_tab >= 0 &&
                               app.close_confirm_tab < app.tab_count) {
                        app_close_tab(&app, app.close_confirm_tab);
                    }
                    if (!app_respawn_on_empty(&app) && app.tab_count == 0)
                        goto cleanup;
                    continue;
                }
                /* Swallow input but let window/state-tracking events through
                 * (mirrors the settings interceptor) so resize, DPI changes
                 * and file drags still update while the prompt is up. */
                if (event.type != EVENT_RESIZE && event.type != EVENT_CLOSE &&
                    event.type != EVENT_DPI_CHANGE &&
                    event.type != EVENT_FOCUS && event.type != EVENT_BLUR &&
                    event.type != EVENT_DROP_FILE &&
                    event.type != EVENT_DRAG_ENTER &&
                    event.type != EVENT_DRAG_EXIT)
                    continue;
            }

            /* Known hosts viewer intercepts events */
            if (app.known_hosts_open) {
                if (event.type == EVENT_KEY_DOWN) {
                    u32 kh_key = event.key.key;
                    if (kh_key == KEY_ESCAPE) {
                        app.known_hosts_open = false;
                        continue;
                    }
                    if (kh_key == KEY_UP) {
                        if (app.known_hosts_selected > 0) app.known_hosts_selected--;
                        continue;
                    }
                    if (kh_key == KEY_DOWN) {
                        app.known_hosts_selected++;
                        continue;
                    }
                    if (kh_key == KEY_BACKSPACE) {
                        if (app.known_hosts_filter_len > 0)
                            app.known_hosts_filter[--app.known_hosts_filter_len] = '\0';
                        continue;
                    }
                }
                if (event.type == EVENT_CHAR_INPUT) {
                    u32 cp = event.char_input.codepoint;
                    if (cp >= 32 && cp < 127 && app.known_hosts_filter_len < 127) {
                        app.known_hosts_filter[app.known_hosts_filter_len++] = (char)cp;
                        app.known_hosts_filter[app.known_hosts_filter_len] = '\0';
                        app.known_hosts_scroll = 0;
                    }
                    continue;
                }
                if (event.type == EVENT_SCROLL) {
                    static f32 kh_accum = 0.0f;
                    i32 ticks = 0;
                    if (event.scroll.precise) {
                        kh_accum += event.scroll.dy;
                        const f32 step_px = 24.0f * app.dpi_scale;
                        while (kh_accum >=  step_px) { ticks--; kh_accum -= step_px; }
                        while (kh_accum <= -step_px) { ticks++; kh_accum += step_px; }
                    } else {
                        ticks = -(i32)event.scroll.dy;
                        if (ticks == 0 && event.scroll.dy != 0)
                            ticks = event.scroll.dy > 0 ? -1 : 1;
                        kh_accum = 0.0f;
                    }
                    app.known_hosts_scroll += ticks;
                    if (app.known_hosts_scroll < 0) app.known_hosts_scroll = 0;
                    continue;
                }
                if (event.type == EVENT_MOUSE_DOWN) {
                    f32 kx = (f32)event.mouse.x;
                    f32 ky = (f32)event.mouse.y;
                    f32 dpi = app.dpi_scale;
                    f32 dw = 600 * dpi, dh = 450 * dpi;
                    f32 ddx = ((f32)app.fb_width - dw) / 2;
                    f32 ddy = ((f32)app.fb_height - dh) / 2;

                    /* Check "Remove All" button */
                    f32 ra_w = 90 * dpi;
                    f32 ra_x = ddx + dw - ra_w - 8;
                    f32 ra_y = ddy + 4;
                    f32 ra_h = 24 * dpi;
                    if (kx >= ra_x && kx <= ra_x + ra_w &&
                        ky >= ra_y && ky <= ra_y + ra_h) {
                        known_hosts_remove_all();
                        continue;
                    }

                    /* Check per-row "Remove" buttons */
                    f32 filter_y = ddy + 36 * dpi;
                    f32 filter_h = 24 * dpi;
                    f32 list_y = filter_y + filter_h + 8;
                    f32 row_h = 52 * dpi;

                    KnownHostEntry kh_entries[256];
                    i32 kh_total = known_hosts_list(kh_entries, 256);
                    KnownHostEntry kh_filtered[256];
                    i32 kh_fcount = 0;
                    for (i32 fi = 0; fi < kh_total && kh_fcount < 256; fi++) {
                        if (app.known_hosts_filter_len > 0) {
                            bool match = false;
                            char lh[256], lf[128];
                            for (i32 j = 0; kh_entries[fi].hostname[j]; j++)
                                lh[j] = (char)(kh_entries[fi].hostname[j] >= 'A' && kh_entries[fi].hostname[j] <= 'Z'
                                    ? kh_entries[fi].hostname[j] + 32 : kh_entries[fi].hostname[j]);
                            lh[strlen(kh_entries[fi].hostname)] = '\0';
                            for (i32 j = 0; app.known_hosts_filter[j]; j++)
                                lf[j] = (char)(app.known_hosts_filter[j] >= 'A' && app.known_hosts_filter[j] <= 'Z'
                                    ? app.known_hosts_filter[j] + 32 : app.known_hosts_filter[j]);
                            lf[app.known_hosts_filter_len] = '\0';
                            if (strstr(lh, lf)) match = true;
                            if (!match) continue;
                        }
                        kh_filtered[kh_fcount++] = kh_entries[fi];
                    }

                    f32 list_h = ddy + dh - list_y - 8;
                    i32 vis_rows = (i32)(list_h / row_h);
                    for (i32 ri = 0; ri < vis_rows && (ri + app.known_hosts_scroll) < kh_fcount; ri++) {
                        i32 ridx = ri + app.known_hosts_scroll;
                        f32 ry = list_y + (f32)ri * row_h;
                        f32 rm_w = 60 * dpi;
                        f32 rm_h = 20 * dpi;
                        f32 rm_x = ddx + dw - rm_w - 12;
                        f32 rm_y = ry + (row_h - rm_h) / 2;

                        if (kx >= rm_x && kx <= rm_x + rm_w &&
                            ky >= rm_y && ky <= rm_y + rm_h) {
                            known_hosts_remove_entry(kh_filtered[ridx].hostname,
                                                     kh_filtered[ridx].port);
                            continue;
                        }
                        if (kx >= ddx + 4 && kx <= ddx + dw - 8 &&
                            ky >= ry && ky <= ry + row_h - 2) {
                            app.known_hosts_selected = ridx;
                        }
                    }

                    if (kx < ddx || kx > ddx + dw || ky < ddy || ky > ddy + dh) {
                        app.known_hosts_open = false;
                    }
                    continue;
                }
                if (event.type != EVENT_RESIZE && event.type != EVENT_CLOSE)
                    continue;
            }

            Tab *tab = app_active_tab(&app);

            switch (event.type) {
            case EVENT_CHAR_INPUT: {
                /* Swallow input while command history popup is active */
                if (app.cmd_history_active) break;

                /* File-browser inline prompt (rename / new folder) */
                if (app.fb_prompt_active) {
                    u32 cp = event.char_input.codepoint;
                    if (cp != '/' && cp != '\\') {   /* keep path-separator guard */
                        rename_buf_append_cp(app.fb_prompt_buf, &app.fb_prompt_len,
                                             (i32)sizeof(app.fb_prompt_buf), cp);
                    }
                    break;
                }

                /* Command palette char input */
                if (app.palette_active) {
                    u32 cp2 = event.char_input.codepoint;
                    /* Numeric sub-modes (font size / opacity) accept digits.
                     * 3 chars max — fits both 6..72 (font) and 0..100 (opacity). */
                    if (app.palette_input_mode == 1 || app.palette_input_mode == 2) {
                        if (cp2 >= '0' && cp2 <= '9' && app.palette_query_len < 3) {
                            app.palette_query[app.palette_query_len++] = (char)cp2;
                            app.palette_query[app.palette_query_len] = '\0';
                        }
                        break;
                    }
                    if (cp2 >= 32 && cp2 < 127 && app.palette_query_len < 127) {
                        app.palette_query[app.palette_query_len++] = (char)cp2;
                        app.palette_query[app.palette_query_len] = '\0';
                        app.palette_selected = 0;
                        app.palette_scroll = 0;
                        if (palette_mode() == PALETTE_MODE_SEARCH)
                            app_vault_search(app.palette_query);
                    }
                    break;
                }

                if (app.broadcast_overlay_active) {
                    break;
                }

                if (app.port_forward_dialog_active) {
                    u32 cp = event.char_input.codepoint;
                    char *field = app.port_forward_field == 0 ? app.port_forward_local_port
                                  : app.port_forward_field == 1 ? app.port_forward_remote_host
                                  : app.port_forward_remote_port;
                    i32 max_len = app.port_forward_field == 1 ? 255 : 7;
                    if (cp >= 32 && cp < 127) {
                        if (app.port_forward_field != 1 && (cp < '0' || cp > '9')) break;
                        i32 flen = (i32)strlen(field);
                        if (flen < max_len) {
                            field[flen] = (char)cp;
                            field[flen + 1] = '\0';
                        }
                    }
                    break;
                }

                /* Tab rename char input */
                if (app.tab_rename_active) {
                    rename_buf_append_cp(app.tab_rename_buf, &app.tab_rename_len,
                                         (i32)sizeof(app.tab_rename_buf),
                                         event.char_input.codepoint);
                    break;
                }

                /* Workspace chip rename char input */
                if (app.chip_rename_active) {
                    rename_buf_append_cp(app.chip_rename_buf, &app.chip_rename_len,
                                         (i32)sizeof(app.chip_rename_buf),
                                         event.char_input.codepoint);
                    break;
                }

                /* Search bar char input */
                if (app.search_active) {
                    u32 cp = event.char_input.codepoint;
                    if (cp >= 32 && cp < 127 && app.search_query_len < 255) {
                        app.search_query[app.search_query_len++] = (char)cp;
                        app.search_query[app.search_query_len] = '\0';
                    }
                    break;
                }

                /* Passphrase dialog char input */
                if (app.passphrase_dialog_active) {
                    u32 cp = event.char_input.codepoint;
                    if (cp >= 32 && cp < 127) {
                        i32 flen = (i32)strlen(app.passphrase_input);
                        if (flen < 255) {
                            app.passphrase_input[flen] = (char)cp;
                            app.passphrase_input[flen + 1] = '\0';
                        }
                    }
                    break;
                }

                /* Vault unlock dialog char input */
                if (app.vault_unlock_dialog_active) {
                    u32 cp = event.char_input.codepoint;
                    if (cp >= 32 && cp < 127) {
                        i32 flen = (i32)strlen(app.vault_unlock_input);
                        if (flen < (i32)sizeof(app.vault_unlock_input) - 1) {
                            app.vault_unlock_input[flen] = (char)cp;
                            app.vault_unlock_input[flen + 1] = '\0';
                        }
                    }
                    break;
                }

                /* Vault browser — when open, forward typed chars to its
                 * filter / edit-form handler. */
                if (app.vault_browser_active &&
                    app_vault_browser_handle_char(&app, event.char_input.codepoint)) {
                    break;
                }

                /* SSH dialog char input */
                if (app.ssh_dialog_active) {
                    u32 cp = event.char_input.codepoint;
                    if (cp >= 32 && cp < 127) {
                        char *field = app.ssh_field == 0 ? app.ssh_host :
                                      app.ssh_field == 1 ? app.ssh_user :
                                      app.ssh_field == 2 ? app.ssh_port :
                                                           app.ssh_password;
                        i32 max_len = app.ssh_field == 2 ? 7 : 255;
                        i32 flen = (i32)strlen(field);
                        if (flen < max_len) { field[flen] = (char)cp; field[flen+1] = '\0'; }
                        /* Clear the error banner as soon as the user
                         * starts fixing the problem (typing into a field). */
                        if (app.ssh_dialog_error[0]) {
                            app.ssh_dialog_error[0] = '\0';
                            app.ssh_dialog_error_until = 0;
                        }
                    }
                    break;
                }

                /* KBI dialog char input */
                if (app.kbi_dialog_active) {
                    u32 cp = event.char_input.codepoint;
                    if (cp >= 32 && cp < 127 && app.kbi_field >= 0 &&
                        app.kbi_field < app.kbi_num_prompts) {
                        char *field = app.kbi_responses[app.kbi_field];
                        i32 flen = (i32)strlen(field);
                        if (flen < 254) { field[flen] = (char)cp; field[flen+1] = '\0'; }
                    }
                    break;
                }

                /* Create Theme dialog — only consumes typing while in the
                 * input phase; running/success/error phases ignore keys
                 * other than Esc / Enter (handled in EVENT_KEY_DOWN). */
                if (app.create_theme_active && app.create_theme_phase == 0) {
                    app_create_theme_insert_char(&app, event.char_input.codepoint);
                    break;
                }

                if (tab && tab->sleeping) {
                    if (app_wake_tab(&app, app.active_tab)) {
                        needs_redraw = true;
                    }
                    last_key_mods = 0;
                    break;
                }

                /* In-document Find/Replace bar captures typing first (it can be
                 * open over a rendered viewer or while editing). */
                FileBrowser *ffb_char = app_find_fb(&app);
                if (ffb_char && ffb_char->find_active) {
                    u32 fcp = event.char_input.codepoint;
                    if (!(last_key_mods & (MOD_SUPER | MOD_CTRL)) && fcp >= 32)
                        fb_find_input_char(ffb_char, fcp);
                    last_key_mods = 0;
                    break;
                }

                /* Editor mode: send chars to the file editor (docked sidebar
                 * or a full-window file-browser tab). */
                FileBrowser *efb_char = app_editing_fb(&app);
                if (efb_char) {
                    u32 cp = event.char_input.codepoint;
                    if (last_key_mods & MOD_SUPER) {
                        /* Cmd+S = save (Cmd+E read/edit toggle is a key event,
                         * handled in the key handler below). */
                        if (cp == 's' || cp == 'S') fb_editor_save(efb_char);
                    } else if (!(last_key_mods & MOD_CTRL)) {
                        fb_editor_insert_char(efb_char, cp);
                        app_ac_update(&app, efb_char);   /* refresh wiki/tag autocomplete */
                    }
                    last_key_mods = 0;
                    break;
                }

                if (!tab) break;
                u32 cp = event.char_input.codepoint;
                u32 char_mods = event.char_input.mods;
                /* Large enough for the longest escape terminal_char_input emits
                 * (e.g. CSI "\x1b[27;15;1114111~"); the old 8-byte buffer
                 * truncated those sequences. */
                u8 out[32];
                Terminal *ft = app_focused_terminal(&app);
                Session *fs = app_focused_session(&app);
                if (!ft || !fs) break;
                /* Clear selection on typing */
                if (selection_active(ft)) selection_clear(ft);
                if ((char_mods & MOD_CTRL) && !(char_mods & (MOD_ALT | MOD_SUPER)) &&
                    (cp == 'z' || cp == 'Z') &&
                    app_ctrl_z_should_signal(fs) &&
                    session_send_tstp(fs)) {
                    app_mark_session_activity(&app, fs, platform_time_sec());
                    terminal_scroll_to_bottom(ft);
                    last_key_mods = 0;
                    break;
                }
                i32 n = terminal_char_input(ft, cp, char_mods, out, sizeof(out));
                if (n > 0) {
                    app_send_input(&app, fs, out, n);
                    if (!(char_mods & (MOD_CTRL | MOD_SUPER)) &&
                        !app.translate_active) {
                        app_translate_shadow_append_cp(&app, ft, fs, cp);
                    } else if (!app.translate_active) {
                        /* ^W/^A/^E-class line edits bypass the shadow —
                         * whatever ghost was showing no longer matches. */
                        app_autosuggest_clear(&app);
                    }
                    terminal_scroll_to_bottom(ft);
                }
                last_key_mods = 0;
                break;
            }
            case EVENT_KEY_DOWN: {
                u32 key = event.key.key;
                u32 mods = event.key.mods;

                /* Translate: count only physical Ctrl+Cmd combo transitions.
                 * macOS sends one flagsChanged event per modifier; the latch
                 * keeps a held combo from being counted more than once. */
                if (app_translate_is_ctrl_cmd_key(key) &&
                    app_translate_ctrl_cmd_down(mods) &&
                    !app.translate_chord_down &&
                    !event.key.repeat &&
                    !app.settings.open && !app.palette_active &&
                    !app.create_theme_active && !app.ssh_dialog_active &&
                    !app.cmd_history_active && !app.transcript_viewer_active &&
                    !app.agent_picker_active && !app.port_forward_dialog_active &&
                    !app.kbi_dialog_active && !app.hostkey_dialog_active &&
                    !app.close_confirm_active &&
                    !app.passphrase_dialog_active && !app.broadcast_overlay_active &&
                    !app.fb_prompt_active && !app.fb_ctx_menu_active &&
                    !app.tab_ctx_menu_active && !app.term_ctx_menu_active &&
                    !app.group_ctx_menu_active && !app.tab_rename_active &&
                    !app.chip_rename_active && !app.filebrowser.editor_mode) {
                    app.translate_chord_down = true;
                    Terminal *tft = app_focused_terminal(&app);
                    Session  *tfs = app_focused_session(&app);
                    if (tft && tfs && app_translate_try_intercept(&app, tft, tfs)) {
                        break;
                    }
                }
                if (app_translate_is_modifier_key(key)) break;

                /* Escape: cancel tab drag or close overlays */
                if (key == KEY_ESCAPE && !mods) {
                    if (app.crash_banner_active) {
                        app.crash_banner_active = false;
                        needs_redraw = true;
                        break;
                    }
                    if (app.pane_drag_pending || app.pane_drag_active) {
                        app.pane_drag_pending   = false;
                        app.pane_drag_active    = false;
                        app.pane_drag_tab_index = -1;
                        app.pane_drag_src_pane  = -1;
                        app.pane_drag_hover_pane = -1;
                        app.pane_drag_drop_zone = 0;
                        needs_redraw = true;
                        break;
                    }
                    if (app.tab_dragging || app.tab_drag_pending) {
                        app.tab_dragging = false;
                        app.tab_drag_pending = false;
                        app.tab_drag_index = -1;
                        app.tab_drag_target = -1;
                        app.tab_drag_target_group = -1;
                        app.tab_drag_offset_x = 0;
                        break;
                    }
                    if (app.cmd_history_active) { app.cmd_history_active = false; break; }
                    if (app.fb_prompt_active) { app.fb_prompt_active = false; break; }
                    if (app.fb_ctx_menu_active) { app.fb_ctx_menu_active = false; break; }
                    if (app.transcript_viewer_active) {
                        app.transcript_viewer_active = false;
                        app_transcript_free(&app);
                        break;
                    }
                    if (app.agent_picker_active) { app.agent_picker_active = false; break; }
                    if (app.tab_ctx_menu_active) { app.tab_ctx_menu_active = false; break; }
                    if (app.term_ctx_menu_active) { app.term_ctx_menu_active = false; break; }
                    if (app.group_ctx_menu_active) { app.group_ctx_menu_active = false; break; }
                    if (app.tab_rename_active) { app_cancel_tab_rename(&app); break; }
                    if (app.chip_rename_active) { app_cancel_chip_rename(&app); break; }
                    if (app.broadcast_overlay_active) { app.broadcast_overlay_active = false; break; }
                    if (app.port_forward_dialog_active) { app.port_forward_dialog_active = false; break; }
                    if (app.kbi_dialog_active) {
                        if (app.kbi_session) {
                            const char *empty[KBI_MAX_PROMPTS] = {0};
                            session_kbi_submit(app.kbi_session, empty, app.kbi_num_prompts);
                        }
                        app.kbi_dialog_active = false;
                        app.kbi_session = NULL;
                        break;
                    }
                    if (app.passphrase_dialog_active) {
                        session_cancel_passphrase(app.passphrase_session);
                        app.passphrase_dialog_active = false;
                        app.passphrase_session = NULL;
                        memset(app.passphrase_input, 0, sizeof(app.passphrase_input));
                        break;
                    }
                    if (app.vault_unlock_dialog_active) {
                        app_vault_cancel_unlock(&app);
                        break;
                    }
                    if (app.vault_browser_active) {
                        app_vault_browser_close(&app);
                        break;
                    }
                    if (app.palette_active) {
                        if (app.palette_input_mode) {
                            app.palette_input_mode = 0;
                            app.palette_query_len = 0;
                            app.palette_query[0] = '\0';
                            app.palette_selected = 0;
                        } else if (palette_mode() == PALETTE_MODE_HISTORY) {
                            /* Pop from history mode back to the root palette */
                            palette_exit_history_mode(app.vault);
                            app.palette_query_len = 0;
                            app.palette_query[0]  = '\0';
                            app.palette_selected  = 0;
                            app.palette_scroll    = 0;
                        } else if (palette_mode() == PALETTE_MODE_OUTLINE ||
                                   palette_mode() == PALETTE_MODE_SWITCHER ||
                                   palette_mode() == PALETTE_MODE_SEARCH ||
                                   palette_mode() == PALETTE_MODE_BACKLINKS ||
                                   palette_mode() == PALETTE_MODE_FOLDER ||
                                   palette_mode() == PALETTE_MODE_SITES) {
                            /* Pop from a markdown picker back to the root palette */
                            app.outline_fb = NULL;
                            palette_exit_history_mode(app.vault);
                            app.palette_query_len = 0;
                            app.palette_query[0]  = '\0';
                            app.palette_selected  = 0;
                            app.palette_scroll    = 0;
                        } else {
                            app.palette_active = false;
                        }
                        break;
                    }
                    if (app.search_active) { app.search_active = false; break; }
                    if (app.ssh_dialog_active) { app.ssh_dialog_active = false; break; }
                    if (app.create_theme_active) { app_close_create_theme(&app); break; }
                    {
                        /* Esc on a file-browser tab: a note opened from the graph
                         * returns to the (preserved) graph; the graph itself
                         * exits to its file list. */
                        Tab *et = app_active_tab(&app);
                        if (et && et->kind == TAB_FILEBROWSER && et->fb) {
                            if (fb_graph_can_return(et->fb)) {
                                fb_close_viewer_ex(et->fb, true);
                                fb_reenter_graph(et->fb);
                                break;
                            }
                            if (et->fb->view_mode == FVIEW_GRAPH) {
                                /* Esc first dismisses the controls panel. */
                                if (et->fb->graph_settings_open) {
                                    et->fb->graph_settings_open = false;
                                    break;
                                }
                                /* A tab opened specifically to host the graph
                                 * returns you where you came from (close it);
                                 * a graph toggled inside a real file-browser tab
                                 * just drops back to that tab's file list. */
                                if (et->fb->graph_owns_tab)
                                    app_close_tab(&app, app.active_tab);
                                else
                                    fb_close_viewer(et->fb);
                                break;
                            }
                        }
                    }
                    /* Docked sidebar viewer: same graph-return behavior, else close. */
                    if (fb_graph_can_return(&app.filebrowser)) {
                        fb_close_viewer_ex(&app.filebrowser, true);
                        fb_reenter_graph(&app.filebrowser);
                        break;
                    }
                    if (app.filebrowser.view_mode == FVIEW_GRAPH &&
                        app.filebrowser.graph_settings_open) {
                        app.filebrowser.graph_settings_open = false; break;
                    }
                    if (app.filebrowser.view_mode != FVIEW_NONE) { fb_close_viewer(&app.filebrowser); break; }
                }

                /* Passphrase dialog key handling */
                if (app.passphrase_dialog_active) {
                    if (key == KEY_BACKSPACE) {
                        i32 flen = (i32)strlen(app.passphrase_input);
                        if (flen > 0) app.passphrase_input[flen - 1] = '\0';
                    } else if (key == KEY_ENTER) {
                        /* Supply passphrase to session, cache it, close dialog */
                        session_supply_passphrase(app.passphrase_session,
                                                  app.passphrase_input);
                        passphrase_cache_store(&app, app.passphrase_key_path,
                                              app.passphrase_input);
                        app.passphrase_dialog_active = false;
                        app.passphrase_session = NULL;
                        memset(app.passphrase_input, 0, sizeof(app.passphrase_input));
                    }
                    break;
                }

                /* Vault unlock dialog key handling */
                if (app.vault_unlock_dialog_active) {
                    if (key == KEY_BACKSPACE) {
                        i32 flen = (i32)strlen(app.vault_unlock_input);
                        if (flen > 0) app.vault_unlock_input[flen - 1] = '\0';
                    } else if (key == KEY_ENTER) {
                        (void)app_vault_submit_unlock(&app);
                    }
                    break;
                }

                /* Vault browser — forward arrows / enter / delete etc. */
                if (app.vault_browser_active &&
                    app_vault_browser_handle_key(&app, key, mods)) {
                    break;
                }

                if (app.broadcast_overlay_active) {
                    if (key == KEY_ENTER) app.broadcast_overlay_active = false;
                    break;
                }

                /* Transcript viewer: arrow keys + PgUp/Dn + Home/End scroll. */
                if (app.transcript_viewer_active) {
                    i32 step = 1;
                    if (key == KEY_PAGE_UP)   step = -12;
                    if (key == KEY_PAGE_DOWN) step =  12;
                    if (key == KEY_UP)        step = -1;
                    if (key == KEY_DOWN)      step =  1;
                    if (key == KEY_HOME) { app.transcript_scroll = 0; break; }
                    if (key == KEY_END)  { app.transcript_scroll = app.transcript_count; break; }
                    if (step != 1 || key == KEY_UP || key == KEY_DOWN ||
                        key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
                        app.transcript_scroll += step;
                        if (app.transcript_scroll < 0) app.transcript_scroll = 0;
                        if (app.transcript_scroll > app.transcript_count)
                            app.transcript_scroll = app.transcript_count;
                    }
                    break;
                }

                /* Agent resume picker: arrow navigation + Enter to execute.
                 * ⌘R re-probes installed binaries and rebuilds the row list,
                 * so a user who installs a new agent can refresh without
                 * relaunching Liu. */
                if (app.agent_picker_active) {
                    /* Up/Down wrap around the row list — matches how the
                     * command palette and history picker behave, so the
                     * user can keep pressing Up to reach the last entry
                     * (e.g. "View transcript") without having to swap to
                     * Down. */
                    if (key == KEY_UP) {
                        if (app.agent_picker_row_count > 0)
                            app.agent_picker_selected =
                                (app.agent_picker_selected - 1 + app.agent_picker_row_count)
                                % app.agent_picker_row_count;
                    } else if (key == KEY_DOWN) {
                        if (app.agent_picker_row_count > 0)
                            app.agent_picker_selected =
                                (app.agent_picker_selected + 1)
                                % app.agent_picker_row_count;
                    } else if (key == KEY_ENTER) {
                        app_agent_picker_execute(&app, app.agent_picker_selected);
                    } else if (key == KEY_R && (mods & MOD_SUPER)) {
                        u8 tool = app.agent_picker_session_tool;
                        char sid[96];
                        char sp[512];
                        char title[160];
                        snprintf(sid,   sizeof sid,   "%s", app.agent_picker_session_id);
                        snprintf(sp,    sizeof sp,    "%s", app.agent_picker_session_path);
                        snprintf(title, sizeof title, "%s", app.agent_picker_title);
                        app_detect_cli_agents(&app, true);
                        app_open_agent_picker(&app, tool, sid, sp, title);
                        app_show_toast(&app, "Agents refreshed");
                    }
                    break;
                }

                if (app.port_forward_dialog_active) {
                    char *field = app.port_forward_field == 0 ? app.port_forward_local_port
                                  : app.port_forward_field == 1 ? app.port_forward_remote_host
                                  : app.port_forward_remote_port;
                    i32 flen = (i32)strlen(field);

                    if (key == KEY_TAB) {
                        i32 field_count = (app.port_forward_mode == 1) ? 1 : 3;
                        app.port_forward_field = (app.port_forward_field + 1) % field_count;
                    } else if (key == KEY_BACKSPACE) {
                        if (flen > 0) field[flen - 1] = '\0';
                    } else if (key == KEY_ENTER) {
                        app_submit_port_forward(&app);
                    }
                    break;
                }

                /* Tab rename key handling */
                if (app.tab_rename_active) {
                    if (key == KEY_ENTER) {
                        app_confirm_tab_rename(&app);
                    } else if (key == KEY_BACKSPACE) {
                        rename_buf_backspace(app.tab_rename_buf, &app.tab_rename_len);
                    }
                    break; /* consume all keys while rename active */
                }

                /* Workspace chip rename key handling */
                if (app.chip_rename_active) {
                    if (key == KEY_ENTER) {
                        app_confirm_chip_rename(&app);
                    } else if (key == KEY_BACKSPACE) {
                        rename_buf_backspace(app.chip_rename_buf, &app.chip_rename_len);
                    }
                    break;
                }

                /* Command history popup (Option+Up triggered) */
                if (app.cmd_history_active) {
                    if (key == KEY_UP) {
                        if (app.cmd_history_selected > 0) app.cmd_history_selected--;
                    } else if (key == KEY_DOWN) {
                        if (app.cmd_history_selected < app.cmd_history_count - 1)
                            app.cmd_history_selected++;
                    } else if (key == KEY_ENTER) {
                        /* Replay the selected command and execute it. */
                        if (app.cmd_history_selected >= 0 &&
                            app.cmd_history_selected < app.cmd_history_count) {
                            Terminal *ht = app_focused_terminal(&app);
                            Session *hs = app_focused_session(&app);
                            if (hs) {
                                const char *cmd = app.cmd_history_raw[app.cmd_history_selected][0]
                                                ? app.cmd_history_raw[app.cmd_history_selected]
                                                : app.cmd_history[app.cmd_history_selected];
                                if (cmd[0]) {
                                    app_send_input(&app, hs, (const u8 *)cmd, (i32)strlen(cmd));
                                    app_send_input(&app, hs, (const u8 *)"\n", 1);
                                    if (ht) terminal_scroll_to_bottom(ht);
                                }
                            }
                        }
                        app.cmd_history_active = false;
                    } else {
                        app.cmd_history_active = false; /* any other key closes */
                    }
                    break;
                }

                /* Command palette key handling */
                if (app.palette_active) {
                    /* Numeric input sub-modes (font size = 1, opacity = 2). */
                    if (app.palette_input_mode == 1 || app.palette_input_mode == 2) {
                        if (key == KEY_BACKSPACE) {
                            if (app.palette_query_len > 0) {
                                app.palette_query[--app.palette_query_len] = '\0';
                            }
                        } else if (key == KEY_ESCAPE) {
                            /* Exit input mode back to normal palette */
                            app.palette_input_mode = 0;
                            app.palette_query_len = 0;
                            app.palette_query[0] = '\0';
                            app.palette_selected = 0;
                        } else if (key == KEY_ENTER) {
                            if (app.palette_query_len > 0) {
                                i32 n = atoi(app.palette_query);
                                if (app.palette_input_mode == 1) {
                                    /* Font size: 6..72 pt */
                                    if (n < 6) n = 6;
                                    if (n > 72) n = 72;
                                    app.config.font_size = (f32)n;
                                    app_reload_font(&app, window);
                                    char msg[48];
                                    snprintf(msg, sizeof(msg), "Font size: %dpt", n);
                                    app_show_toast(&app, msg);
                                } else {
                                    /* Opacity: 30..100 % (matches the
                                     * Increase/Decrease command floor). */
                                    if (n < 30)  n = 30;
                                    if (n > 100) n = 100;
                                    app.config.opacity = (f32)n / 100.0f;
                                    platform_window_set_opacity(window, app.config.opacity);
                                    char msg[48];
                                    snprintf(msg, sizeof(msg), "Opacity: %d%%", n);
                                    app_show_toast(&app, msg);
                                }
                            }
                            app.palette_input_mode = 0;
                            app.palette_active = false;
                            app.palette_query_len = 0;
                            app.palette_query[0] = '\0';
                        }
                        needs_redraw = true;
                        break;
                    }
                    /* Sync filtered list with current query before bounds check */
                    palette_set_query(app.palette_query);
                    i32 palette_count = palette_filtered_count();
                    if (key == KEY_UP) {
                        if (app.palette_selected > 0) app.palette_selected--;
                        else if (palette_count > 0) app.palette_selected = palette_count - 1; /* wrap */
                    } else if (key == KEY_DOWN) {
                        if (app.palette_selected + 1 < palette_count) app.palette_selected++;
                        else app.palette_selected = 0; /* wrap */
                    }
                    else if (key == KEY_PAGE_UP) {
                        app.palette_selected -= 10;
                        if (app.palette_selected < 0) app.palette_selected = 0;
                    }
                    else if (key == KEY_PAGE_DOWN) {
                        app.palette_selected += 10;
                        if (app.palette_selected >= palette_count)
                            app.palette_selected = palette_count - 1;
                        if (app.palette_selected < 0) app.palette_selected = 0;
                    }
                    else if (key == KEY_HOME) {
                        app.palette_selected = 0;
                    }
                    else if (key == KEY_END) {
                        app.palette_selected = palette_count > 0 ? palette_count - 1 : 0;
                    }
                    else if (key == KEY_BACKSPACE) {
                        if (app.palette_query_len > 0) {
                            app.palette_query[--app.palette_query_len] = '\0';
                            app.palette_selected = 0;
                            app.palette_scroll = 0;
                            if (palette_mode() == PALETTE_MODE_SEARCH)
                                app_vault_search(app.palette_query);
                        }
                    }
                    else if (key == KEY_ENTER) {
                        PaletteItem *item = palette_get_item(app.palette_selected);
                        app_execute_palette_command(&app, window, item);
                    }
                    needs_redraw = true;
                    break;
                }

                /* Search bar input handling */
                if (app.search_active) {
                    if (key == KEY_ENTER) {
                        /* Execute search */
                        Tab *stab = app_active_tab(&app);
                        if (stab && stab->terminal && app.search_query_len > 0) {
                            /* Auto-detect regex: /pattern/ syntax */
                            bool use_regex = false;
                            const char *pattern = app.search_query;
                            i32 plen = app.search_query_len;
                            char regex_buf[256];
                            if (plen >= 2 && pattern[0] == '/' && pattern[plen - 1] == '/') {
                                use_regex = true;
                                /* Extract pattern between slashes */
                                plen -= 2;
                                if (plen > 0 && plen < (i32)sizeof(regex_buf)) {
                                    memcpy(regex_buf, pattern + 1, (size_t)plen);
                                    regex_buf[plen] = '\0';
                                    pattern = regex_buf;
                                } else {
                                    use_regex = false; /* empty or too long */
                                }
                            }
                            terminal_search_start(stab->terminal, pattern, false, use_regex);
                        }
                        app.search_active = false;
                    } else if (key == KEY_BACKSPACE) {
                        if (app.search_query_len > 0)
                            app.search_query[--app.search_query_len] = '\0';
                    }
                    break; /* consume all keys while search active */
                }

                /* File-browser inline prompt (rename / new folder). */
                if (app.fb_prompt_active) {
                    if (key == KEY_ENTER) {
                        app_fb_apply_prompt(&app);
                    } else if (key == KEY_BACKSPACE) {
                        rename_buf_backspace(app.fb_prompt_buf, &app.fb_prompt_len);
                    }
                    break;
                }

                /* Cmd+C copies a rendered-markdown text selection when one is
                 * active in the focused viewer (filebrowser tab or sidebar).
                 * Must precede the file-list copy below so it isn't shadowed. */
                if ((mods & MOD_SUPER) && key == KEY_C) {
                    FileBrowser *vfb = NULL;
                    Tab *vt = app_active_tab(&app);
                    if (vt && vt->kind == TAB_FILEBROWSER && vt->fb &&
                        fb_md_selection_active(vt->fb)) {
                        vfb = vt->fb;
                    } else if (fb_md_selection_active(&app.filebrowser)) {
                        vfb = &app.filebrowser;
                    }
                    if (vfb) { fb_md_selection_copy(vfb); break; }
                }

                /* File-browser shortcuts when the sidebar is focused. Must
                 * precede terminal key handling so ^C/^V don't leak to the
                 * shell, AND must yield to text-input modals — otherwise
                 * Backspace is stolen for fb_delete_file. Crucially it must
                 * also yield while a file is open in the editor: there the
                 * same keys mean text editing (Backspace deletes a char,
                 * Cmd+C/X/V act on the buffer), so file-list copy/cut/paste/
                 * delete must NOT fire — the editor block below owns them. */
                if (app.sidebar_focused && app.filebrowser.open &&
                    !app.filebrowser.editor_mode &&
                    !app.fb_ctx_menu_active && !app.ssh_dialog_active &&
                    !app.palette_active && !app.search_active &&
                    !app.create_theme_active && !app.kbi_dialog_active) {
                    bool consumed = false;
                    if (mods & MOD_SUPER) {
                        if (key == KEY_C) {
                            app_fb_copy(&app, app.filebrowser.selected, false);
                            consumed = true;
                        } else if (key == KEY_X) {
                            app_fb_copy(&app, app.filebrowser.selected, true);
                            consumed = true;
                        } else if (key == KEY_V) {
                            app_fb_paste(&app);
                            consumed = true;
                        } else if ((mods & MOD_SHIFT) && key == KEY_N) {
                            app_fb_begin_new_folder(&app);
                            consumed = true;
                        } else if (key == KEY_N) {
                            app_fb_begin_new_file(&app);
                            consumed = true;
                        }
                    } else if (!mods) {
                        if (key == KEY_DELETE || key == KEY_BACKSPACE) {
                            if (fb_delete_file(&app.filebrowser, app.filebrowser.selected))
                                app_fb_set_status(&app, "Deleted");
                            else
                                app_fb_set_status(&app, "Delete failed");
                            consumed = true;
                        } else if (key == KEY_F2) {
                            app_fb_begin_rename(&app, app.filebrowser.selected);
                            consumed = true;
                        }
                    }
                    if (consumed) break;
                }

                /* SSH dialog input handling */
                if (app.ssh_dialog_active) {
                    char *field = app.ssh_field == 0 ? app.ssh_host :
                                  app.ssh_field == 1 ? app.ssh_user :
                                  app.ssh_field == 2 ? app.ssh_port :
                                                       app.ssh_password;
                    i32 field_cap = app.ssh_field == 0 ? (i32)sizeof(app.ssh_host) :
                                    app.ssh_field == 1 ? (i32)sizeof(app.ssh_user) :
                                    app.ssh_field == 2 ? (i32)sizeof(app.ssh_port) :
                                                         (i32)sizeof(app.ssh_password);
                    /* Port is numeric-only, capped tight so atoi stays within
                     * uint16 range. Others are text-ish (hostnames, usernames,
                     * passwords) and allow the full buffer. */
                    i32 field_max = app.ssh_field == 2 ? 7 : (field_cap - 1);
                    i32 flen = (i32)strlen(field);

                    if (key == KEY_TAB) {
                        if (app.ssh_dialog_proto != 0) {
                            /* Telnet/Serial expose only Host(0) and Port(2). */
                            app.ssh_field = (app.ssh_field == 0) ? 2 : 0;
                        } else {
                            app.ssh_field = (app.ssh_field + 1) % 4;
                        }
                    }
                    else if (key == KEY_BACKSPACE) { if (flen > 0) field[flen-1] = '\0'; }
                    else if (key == KEY_V && (mods & MOD_SUPER)) {
                        /* Cmd+V — paste clipboard into the active field.
                         * Strips control characters and newlines (hostnames,
                         * usernames, passwords don't contain them); honours
                         * the per-field cap. Truncates silently on overflow. */
                        const char *clip = platform_clipboard_get();
                        if (clip) {
                            for (const char *p = clip; *p && flen < field_max; p++) {
                                u8 c = (u8)*p;
                                if (c == '\r' || c == '\n' || c == '\t') continue;
                                if (c < 32 || c >= 127) continue;
                                /* Port field: digits only */
                                if (app.ssh_field == 2 && !(c >= '0' && c <= '9')) continue;
                                field[flen++] = (char)c;
                            }
                            field[flen] = '\0';
                        }
                    }
                    else if (key == KEY_C && (mods & MOD_SUPER)) {
                        /* Cmd+C — copy current field value. Password field
                         * excluded so copy-paste doesn't leak it to the
                         * pasteboard unexpectedly. */
                        if (app.ssh_field != 3 && flen > 0) {
                            platform_clipboard_set(field);
                        }
                    }
                    else if (key == KEY_X && (mods & MOD_SUPER)) {
                        /* Cmd+X — cut: copy then clear. Same password
                         * exclusion as Cmd+C. */
                        if (app.ssh_field != 3 && flen > 0) {
                            platform_clipboard_set(field);
                            field[0] = '\0';
                        } else if (app.ssh_field == 3) {
                            field[0] = '\0';
                        }
                    }
                    else if (key == 'm' && (mods & MOD_ALT)) {
                        /* Alt+M toggles mosh */
                        app.ssh_use_mosh = !app.ssh_use_mosh;
                    }
                    else if (key == 'x' && (mods & MOD_ALT)) {
                        /* Alt+X toggles X11 forwarding */
                        app.ssh_forward_x11 = !app.ssh_forward_x11;
                    }
                    /* Navigate history suggestions with Up/Down. A selection
                     * fills host/user/port and CLEARS the password, matching
                     * the mouse path — the user always re-enters the secret. */
                    else if (key == KEY_UP && app.ssh_history_count > 0) {
                        if (app.ssh_history_selected < 0)
                            app.ssh_history_selected = 0;
                        else if (app.ssh_history_selected > 0)
                            app.ssh_history_selected--;
                        VaultHistoryEntry *he = &app.ssh_history[app.ssh_history_selected];
                        snprintf(app.ssh_host, sizeof(app.ssh_host), "%s", he->hostname);
                        snprintf(app.ssh_user, sizeof(app.ssh_user), "%s", he->username);
                        if (he->port > 0 && he->port != 22)
                            snprintf(app.ssh_port, sizeof(app.ssh_port), "%d", he->port);
                        else
                            app.ssh_port[0] = '\0';
                        memset(app.ssh_password, 0, sizeof(app.ssh_password));
                        app.ssh_field = 3;
                    }
                    else if (key == KEY_DOWN && app.ssh_history_count > 0) {
                        if (app.ssh_history_selected < app.ssh_history_count - 1)
                            app.ssh_history_selected++;
                        VaultHistoryEntry *he = &app.ssh_history[app.ssh_history_selected];
                        snprintf(app.ssh_host, sizeof(app.ssh_host), "%s", he->hostname);
                        snprintf(app.ssh_user, sizeof(app.ssh_user), "%s", he->username);
                        if (he->port > 0 && he->port != 22)
                            snprintf(app.ssh_port, sizeof(app.ssh_port), "%d", he->port);
                        else
                            app.ssh_port[0] = '\0';
                        memset(app.ssh_password, 0, sizeof(app.ssh_password));
                        app.ssh_field = 3;
                    }
                    else if (key == KEY_ENTER) {
                        app_ssh_dialog_submit(&app);
                    }
                    break;
                }

                /* KBI (keyboard-interactive / 2FA) dialog input handling */
                if (app.kbi_dialog_active) {
                    if (key == KEY_TAB) {
                        if (app.kbi_num_prompts > 0)
                            app.kbi_field = (app.kbi_field + 1) % app.kbi_num_prompts;
                    } else if (key == KEY_BACKSPACE) {
                        if (app.kbi_field >= 0 && app.kbi_field < app.kbi_num_prompts) {
                            char *field = app.kbi_responses[app.kbi_field];
                            i32 flen = (i32)strlen(field);
                            if (flen > 0) field[flen - 1] = '\0';
                        }
                    } else if (key == KEY_ENTER) {
                        if (app.kbi_session) {
                            const char *resp_ptrs[KBI_MAX_PROMPTS];
                            for (i32 ri = 0; ri < app.kbi_num_prompts; ri++)
                                resp_ptrs[ri] = app.kbi_responses[ri];
                            session_kbi_submit(app.kbi_session, resp_ptrs, app.kbi_num_prompts);
                        }
                        app.kbi_dialog_active = false;
                        app.kbi_session = NULL;
                    }
                    break;
                }

                /* Create Theme dialog key handling */
                if (app.create_theme_active) {
                    if (app.create_theme_phase == 0) {
                        if (key == KEY_TAB) {
                            /* Cycle name → desc → agent → name. */
                            i32 next = app.create_theme_field + 1;
                            if (next > 2) next = 0;
                            /* Skip agent step when no agents detected. */
                            if (next == 2 && app.create_theme_agent_count == 0) next = 0;
                            app.create_theme_field = next;
                        } else if (key == KEY_BACKSPACE) {
                            app_create_theme_backspace(&app);
                        } else if (key == KEY_LEFT && app.create_theme_field == 2) {
                            if (app.create_theme_agent_idx > 0)
                                app.create_theme_agent_idx--;
                        } else if (key == KEY_RIGHT && app.create_theme_field == 2) {
                            if (app.create_theme_agent_idx <
                                app.create_theme_agent_count - 1)
                                app.create_theme_agent_idx++;
                        } else if (key == KEY_ENTER) {
                            app_create_theme_submit(&app);
                        }
                    } else if (app.create_theme_phase == 3) {
                        if (key == KEY_ENTER) {
                            /* Back to input on Enter. */
                            app.create_theme_phase = 0;
                        }
                    } else if (app.create_theme_phase == 2) {
                        if (key == KEY_ENTER) app_close_create_theme(&app);
                    }
                    break;
                }

                /* Option+Up → open command history popup */
                if (key == KEY_UP && (mods & MOD_ALT) && !(mods & MOD_SUPER) && !(mods & MOD_CTRL)) {
                    app_extract_cmd_history(&app);
                    app.cmd_history_active = true;
                    app.cmd_history_selected = 0;
                    break;
                }

                /* Cmd+Ctrl+Arrow: window snap to screen halves / maximize */
                if ((mods & MOD_SUPER) && (mods & MOD_CTRL) && !(mods & MOD_SHIFT) && !(mods & MOD_ALT)) {
                    if (key == KEY_LEFT)      { platform_window_snap(window, WIN_SNAP_LEFT_HALF);  break; }
                    if (key == KEY_RIGHT)     { platform_window_snap(window, WIN_SNAP_RIGHT_HALF); break; }
                    if (key == KEY_UP)        { platform_window_snap(window, WIN_SNAP_FULL);       break; }
                    if (key == KEY_DOWN)      { platform_window_snap(window, WIN_SNAP_CENTER);     break; }
                }
                /* Cmd+Ctrl+Shift+Arrow: quadrant snap (top-left / top-right / bottom-left / bottom-right) */
                if ((mods & MOD_SUPER) && (mods & MOD_CTRL) && (mods & MOD_SHIFT) && !(mods & MOD_ALT)) {
                    if (key == KEY_LEFT)  { platform_window_snap(window, WIN_SNAP_TOP_LEFT);     break; }
                    if (key == KEY_RIGHT) { platform_window_snap(window, WIN_SNAP_TOP_RIGHT);    break; }
                    if (key == KEY_UP)    { platform_window_snap(window, WIN_SNAP_TOP_HALF);     break; }
                    if (key == KEY_DOWN)  { platform_window_snap(window, WIN_SNAP_BOTTOM_HALF);  break; }
                }
                /* Cmd+Alt+Arrow: split divider nudge in active tab */
                if ((mods & MOD_SUPER) && (mods & MOD_ALT) && !(mods & MOD_CTRL) && !(mods & MOD_SHIFT)) {
                    Tab *at = app_active_tab(&app);
                    if (at && at->split != SPLIT_NONE) {
                        SplitLayoutNode *root = NULL;
                        SplitType split = at->split;
                        f32 *ratio = &at->split_ratio;
                        if (at->split_root >= 0 && at->split_root < at->split_next_node &&
                            at->split_nodes[at->split_root].used &&
                            !at->split_nodes[at->split_root].leaf) {
                            root = &at->split_nodes[at->split_root];
                            split = root->split;
                            ratio = &root->ratio;
                        }
                        if (split == SPLIT_H && key == KEY_LEFT)  { *ratio -= 0.05f; needs_redraw = true; }
                        if (split == SPLIT_H && key == KEY_RIGHT) { *ratio += 0.05f; needs_redraw = true; }
                        if (split == SPLIT_V && key == KEY_UP)    { *ratio -= 0.05f; needs_redraw = true; }
                        if (split == SPLIT_V && key == KEY_DOWN)  { *ratio += 0.05f; needs_redraw = true; }
                        if (*ratio < 0.15f) *ratio = 0.15f;
                        if (*ratio > 0.85f) *ratio = 0.85f;
                        if (root) {
                            at->split = root->split;
                            at->split_ratio = root->ratio;
                        }
                        if (needs_redraw) {
                            app_resize_tab_panes(&app, at);
                            break;
                        }
                    }
                }

                if (tab && tab->sleeping) {
                    if (app_wake_tab(&app, app.active_tab)) {
                        needs_redraw = true;
                    }
                    break;
                }

                /* In-document Find / Replace: opening (Cmd+F / Cmd+Shift+F /
                 * Cmd+Alt+F) and, while open, its navigation keys take priority
                 * over the editor key handling below. */
                {
                    FileBrowser *ffb = app_find_fb(&app);
                    if (ffb && (mods & MOD_SUPER) && key == KEY_F) {
                        bool want_replace = (mods & (MOD_ALT | MOD_SHIFT)) != 0;
                        fb_find_open(ffb, want_replace);
                        break;
                    }
                    if (ffb && ffb->find_active) {
                        bool fconsumed = true;
                        switch (key) {
                        case KEY_ESCAPE: fb_find_close(ffb); break;
                        case KEY_ENTER:
                            if (mods & MOD_SUPER) {
                                if (ffb->find_replace_mode) fb_find_replace_all(ffb);
                            } else if (mods & MOD_SHIFT) {
                                fb_find_prev(ffb);
                            } else if (ffb->find_replace_mode && ffb->find_focus_replace) {
                                if (!fb_find_replace_one(ffb)) fb_find_next(ffb);
                            } else {
                                fb_find_next(ffb);
                            }
                            break;
                        case KEY_TAB:
                            if (ffb->find_replace_mode)
                                ffb->find_focus_replace = !ffb->find_focus_replace;
                            break;
                        case KEY_BACKSPACE: fb_find_input_backspace(ffb); break;
                        case KEY_G:
                            if (mods & MOD_SUPER) {
                                if (mods & MOD_SHIFT) fb_find_prev(ffb);
                                else                  fb_find_next(ffb);
                            } else fconsumed = false;
                            break;
                        default: fconsumed = false; break;
                        }
                        if (fconsumed) break;
                    }
                }

                /* Editor mode: keyboard shortcuts and navigation (docked
                 * sidebar or a full-window file-browser tab). */
                FileBrowser *efb_key = app_editing_fb(&app);
                if (efb_key) {
                    bool consumed = true;

                    /* Autocomplete popup steals navigation/accept keys. */
                    if (efb_key->ac_active && !(mods & (MOD_SUPER | MOD_CTRL | MOD_ALT))) {
                        bool aconsumed = true;
                        switch (key) {
                        case KEY_ESCAPE: app_ac_dismiss(efb_key); break;
                        case KEY_UP:     if (efb_key->ac_sel > 0) efb_key->ac_sel--; break;
                        case KEY_DOWN:   if ((u32)(efb_key->ac_sel + 1) < efb_key->ac_count) efb_key->ac_sel++; break;
                        case KEY_ENTER:
                        case KEY_TAB:    app_ac_accept(efb_key); break;
                        default: aconsumed = false; break;
                        }
                        if (aconsumed) break;
                    }

                    /* Any Cmd/Alt-modified key (paste, undo, word-delete, …)
                     * mutates the buffer outside app_ac_update's tracked paths,
                     * so dismiss the autocomplete popup first — otherwise a later
                     * Tab/Enter would splice a now-stale range. */
                    if (efb_key->ac_active && (mods & (MOD_SUPER | MOD_ALT)))
                        app_ac_dismiss(efb_key);

                    /* Cmd+key shortcuts */
                    if (mods & MOD_SUPER) {
                        switch (key) {
                        case KEY_Z:
                            if (mods & MOD_SHIFT) fb_editor_redo(efb_key);
                            else fb_editor_undo(efb_key);
                            break;
                        case KEY_S: fb_editor_save(efb_key); break;
                        case KEY_D: fb_editor_duplicate_line(efb_key); break;
                        case KEY_C:
                            if ((mods & MOD_SHIFT) && fb_md_edit_active(efb_key))
                                fb_editor_wrap_or_insert(efb_key, "`", "`"); /* inline code */
                            else if (!(mods & MOD_SHIFT))
                                fb_editor_copy_line(efb_key);
                            else consumed = false;
                            break;
                        case KEY_X: fb_editor_cut_line(efb_key); break;
                        /* Markdown formatting shortcuts — only while editing a
                         * markdown doc, so code/text editing keeps Cmd+B etc.
                         * for their global bindings (e.g. sidebar toggle). */
                        case KEY_B:
                            if (fb_md_edit_active(efb_key)) fb_editor_wrap_or_insert(efb_key, "**", "**");
                            else consumed = false;
                            break;
                        case KEY_I:
                            if (fb_md_edit_active(efb_key)) fb_editor_wrap_or_insert(efb_key, "*", "*");
                            else consumed = false;
                            break;
                        case KEY_K:
                            if (fb_md_edit_active(efb_key)) fb_editor_make_link(efb_key);
                            else consumed = false;
                            break;
                        case KEY_7:
                            if ((mods & MOD_SHIFT) && fb_md_edit_active(efb_key))
                                fb_editor_toggle_line_prefix(efb_key, "1. ");
                            else consumed = false;
                            break;
                        case KEY_8:
                            if ((mods & MOD_SHIFT) && fb_md_edit_active(efb_key))
                                fb_editor_toggle_line_prefix(efb_key, "- ");
                            else consumed = false;
                            break;
                        case KEY_A: fb_editor_goto_line_start(efb_key); break; /* Cmd+A = line start */
                        /* macOS line/document navigation + line-kill. */
                        case KEY_LEFT:      fb_editor_goto_line_start(efb_key);    break; /* Cmd+← */
                        case KEY_RIGHT:     fb_editor_goto_line_end(efb_key);      break; /* Cmd+→ */
                        case KEY_UP:        fb_editor_goto_doc_start(efb_key);     break; /* Cmd+↑ */
                        case KEY_DOWN:      fb_editor_goto_doc_end(efb_key);       break; /* Cmd+↓ */
                        case KEY_BACKSPACE: fb_editor_delete_to_line_start(efb_key); break; /* Cmd+⌫ */
                        case KEY_E:
                            /* Cmd+E toggles markdown back to Read;
                             * in a code/text editor it stays "go to line end". */
                            if (fb_md_edit_active(efb_key)) fb_md_toggle_edit(efb_key);
                            else fb_editor_goto_line_end(efb_key);
                            break;
                        case KEY_V:
                            /* Paste in editor — one coalesced, undo-able edit
                             * that preserves multi-line text and UTF-8. */
                            fb_editor_paste(efb_key);
                            break;
                        default: consumed = false; break;
                        }
                        if (consumed) break;
                    }

                    /* Option (Alt) — word-wise navigation + delete (macOS native). */
                    if ((mods & MOD_ALT) && !(mods & (MOD_SUPER | MOD_CTRL))) {
                        consumed = true;
                        switch (key) {
                        case KEY_LEFT:      fb_editor_move_word_left(efb_key);   break; /* ⌥← */
                        case KEY_RIGHT:     fb_editor_move_word_right(efb_key);  break; /* ⌥→ */
                        case KEY_BACKSPACE: fb_editor_delete_word_left(efb_key); break; /* ⌥⌫ */
                        case KEY_DELETE:    fb_editor_delete_word_right(efb_key);break; /* ⌥⌦ */
                        default: consumed = false; break;
                        }
                        if (consumed) break;
                    }

                    /* Navigation and editing keys */
                    consumed = true;
                    switch (key) {
                    case KEY_UP:        fb_editor_move(efb_key, 0, -1); break;
                    case KEY_DOWN:      fb_editor_move(efb_key, 0, 1); break;
                    case KEY_LEFT:      fb_editor_move(efb_key, -1, 0); break;
                    case KEY_RIGHT:     fb_editor_move(efb_key, 1, 0); break;
                    case KEY_HOME:      fb_editor_goto_line_start(efb_key); break;
                    case KEY_END:       fb_editor_goto_line_end(efb_key); break;
                    case KEY_PAGE_UP:   fb_editor_move(efb_key, 0, -20); break;
                    case KEY_PAGE_DOWN: fb_editor_move(efb_key, 0, 20); break;
                    case KEY_BACKSPACE: fb_editor_backspace(efb_key); break;
                    case KEY_DELETE:    fb_editor_delete(efb_key); break;
                    case KEY_ENTER:
                        if (fb_md_edit_active(efb_key)) fb_md_editor_newline(efb_key);
                        else fb_editor_newline(efb_key);
                        break;
                    case KEY_TAB:
                        fb_editor_insert_char(efb_key, ' ');
                        fb_editor_insert_char(efb_key, ' ');
                        fb_editor_insert_char(efb_key, ' ');
                        fb_editor_insert_char(efb_key, ' ');
                        break;
                    default: consumed = false; break;
                    }
                    if (consumed) {
                        /* Editing/navigation may move the cursor on or off a
                         * [[ / # trigger — refresh the autocomplete popup. */
                        if (key == KEY_BACKSPACE || key == KEY_DELETE ||
                            key == KEY_LEFT || key == KEY_RIGHT)
                            app_ac_update(&app, efb_key);
                        else if (efb_key->ac_active)
                            app_ac_dismiss(efb_key);
                        break;
                    }
                    /* Fall through for other keybindings */
                }

                /* Cmd+E enters Edit mode from a rendered markdown preview (the
                 * Edit→Read direction is handled inside the editor block above,
                 * which only runs while already editing). */
                if (key == KEY_E &&
                    (mods & (MOD_SUPER | MOD_SHIFT | MOD_CTRL | MOD_ALT)) == MOD_SUPER) {
                    FileBrowser *mfb = app_active_md_fb(&app);
                    if (mfb) { fb_md_toggle_edit(mfb); break; }
                }

                /* Lookup action from keybinding table. Keep the canonical
                 * split shortcuts as a fallback when a stale keybindings.json
                 * omits them; user-defined bindings still win because this
                 * only runs after lookup returns ACT_NONE. */
                Action act = keybind_lookup(&app.keybinds, (KeyCode)key, mods);
                if (act == ACT_NONE && key == KEY_D &&
                    (mods & (MOD_SUPER | MOD_SHIFT | MOD_CTRL | MOD_ALT)) == MOD_SUPER) {
                    act = ACT_SPLIT_HORIZONTAL;
                } else if (act == ACT_NONE && key == KEY_D &&
                           (mods & (MOD_SUPER | MOD_SHIFT | MOD_CTRL | MOD_ALT)) ==
                               (MOD_SUPER | MOD_SHIFT)) {
                    act = ACT_SPLIT_VERTICAL;
                }

                /* Cmd+R: close whatever the mouse is currently hovering over.
                 *   - Tab bar → close that tab
                 *   - Split pane → collapse that side back into the tab
                 * Runs before keybind dispatch so it doesn't clash with a
                 * remapped ACT_* that happens to land on R; if nothing
                 * closable is under the cursor we fall through to the table. */
                if (key == KEY_R &&
                    (mods & (MOD_SUPER | MOD_SHIFT | MOD_CTRL | MOD_ALT)) == MOD_SUPER) {
                    HitResult htab = ui_hit_test(&app, app.hover_x, app.hover_y);
                    if ((htab.type == HIT_TAB || htab.type == HIT_TAB_CLOSE) &&
                        htab.index >= 0 && htab.index < app.tab_count) {
                        if (app_confirm_close_arm(&app, htab.index, -1, -1)) {
                            needs_redraw = true;
                            break;
                        }
                        app_close_tab(&app, htab.index);
                        if (!app_respawn_on_empty(&app) && app.tab_count == 0)
                            goto cleanup;
                        needs_redraw = true;
                        break;
                    }
                    /* Terminal area: figure out which split pane the cursor
                     * is over and collapse it. Single-pane tabs fall through
                     * to ACT_CLOSE_TAB territory via the keybind table. */
                    if (htab.type == HIT_TERMINAL) {
                        Tab *act = app_active_tab(&app);
                        if (act && act->split != SPLIT_NONE) {
                            i32 pane = 0;
                            if (act->split_root >= 0) {
                                pane = (i32)htab.rel_x;
                            } else {
                                f32 sb_w = app.sidebar_visible ? app.sidebar_width : 0.0f;
                                f32 ox = sb_w + app.padding;
                                f32 oy = app.tab_bar_height + app.padding +
                                         app.config.style.terminal_top_gap * app.dpi_scale;
                                f32 tw = (f32)app.fb_width - ox - app.padding;
                                f32 th = (f32)app.fb_height - oy -
                                         app.status_bar_height - app.padding;
                                if (act->split == SPLIT_H) {
                                    f32 div_x = ox + tw * act->split_ratio;
                                    pane = (app.hover_x > div_x) ? 1 : 0;
                                } else {
                                    f32 div_y = oy + th * act->split_ratio;
                                    pane = (app.hover_y > div_y) ? 1 : 0;
                                }
                            }
                            if (!app_confirm_close_arm(&app, app.active_tab,
                                                       pane, -1))
                                app_close_split_pane(&app, app.active_tab, pane);
                            needs_redraw = true;
                            break;
                        }
                    }
                }

                #define KB_RELAYOUT() do { \
                    i32 _w,_h,_fw,_fh; \
                    platform_window_get_size(window,&_w,&_h); \
                    platform_window_get_framebuffer_size(window,&_fw,&_fh); \
                    app_update_layout(&app,_w,_h,_fw,_fh); \
                } while(0)

                /* Inline autosuggest accept (default Right arrow, rebindable
                 * as ACT_ACCEPT_SUGGESTION). Consume the key only when a ghost
                 * suggestion is actually showing for the focused terminal;
                 * otherwise drop to ACT_NONE so the key keeps its normal
                 * meaning (e.g. cursor movement) at the prompt. */
                if (act == ACT_ACCEPT_SUGGESTION) {
                    Terminal *fta = app_focused_terminal(&app);
                    Session  *fsa = app_focused_session(&app);
                    if (fta && fsa && app.autosuggest_visible &&
                        app.autosuggest_target_term == fta &&
                        app.autosuggest_target_sess == fsa &&
                        app.autosuggest_suffix[0]) {
                        i32 sufn = (i32)strlen(app.autosuggest_suffix);
                        /* Learning signal: the ghost was taken. */
                        {
                            const char *fcwd = app_terminal_cwd(fta, fsa);
                            if (fcwd && fcwd[0])
                                cmd_suggest_feedback(fcwd,
                                                     app.autosuggest_full, true);
                        }
                        app_send_input(&app, fsa,
                                       (const u8 *)app.autosuggest_suffix, sufn);
                        app_translate_shadow_append_bytes(&app, fta, fsa,
                            (const u8 *)app.autosuggest_suffix, sufn);
                        app_autosuggest_clear(&app);
                        terminal_scroll_to_bottom(fta);
                        break; /* consumed */
                    }
                    act = ACT_NONE; /* no suggestion — pass through to terminal */
                }

                if (act != ACT_NONE) {
                    switch (act) {
                    case ACT_NEW_TAB: {
                        Session *s = app_new_local_session_here(&app);
                        if (s && session_status(s) == SESSION_CONNECTED)
                            app_new_tab(&app, s, "Terminal");
                        break;
                    }
                    case ACT_CLOSE_TAB:
                        if (app.tab_count > 0) {
                            if (app_confirm_close_arm(&app, app.active_tab, -1, -1))
                                break;
                            app_close_tab(&app, app.active_tab);
                            if (!app_respawn_on_empty(&app) && app.tab_count == 0)
                                goto cleanup;
                        }
                        break;
                    case ACT_UNDO_CLOSE_TAB:
                        app_undo_close_tab(&app);
                        break;
                    case ACT_NEXT_TAB:
                        app_switch_tab(&app, (app.active_tab + 1) % app.tab_count);
                        break;
                    case ACT_PREV_TAB:
                        app_switch_tab(&app, (app.active_tab - 1 + app.tab_count) % app.tab_count);
                        break;
                    case ACT_TAB_1: case ACT_TAB_2: case ACT_TAB_3: case ACT_TAB_4:
                    case ACT_TAB_5: case ACT_TAB_6: case ACT_TAB_7: case ACT_TAB_8:
                    case ACT_TAB_9: {
                        i32 idx = (i32)(act - ACT_TAB_1);
                        if (idx < app.tab_count) app_switch_tab(&app, idx);
                        break;
                    }
                    case ACT_TOGGLE_SIDEBAR:
                        if (app.sidebar_visible) {
                            app.sidebar_visible = false;
                            app.sidebar_width = 0;
                            app.filebrowser.open = false;
                            fb_close_viewer(&app.filebrowser);
                        } else {
                            app_open_sidebar_for_context(&app);
                        }
                        KB_RELAYOUT();
                        break;
                    case ACT_PASTE:
                        if (tab) {
                            const char *clip = platform_clipboard_get();
                            app_send_bracketed_paste(&app, app_focused_terminal(&app),
                                                     app_focused_session(&app), clip);
                        }
                        break;
                    case ACT_COPY:
                        if (tab) {
                            Terminal *ct = app_focused_terminal(&app);
                            char *sel = ct ? terminal_select_text(ct) : NULL;
                            if (sel) { platform_clipboard_set(sel); free(sel); }
                        }
                        break;
                    case ACT_SELECT_ALL:
                        if (tab) {
                            Terminal *sat = app_focused_terminal(&app);
                            if (sat) {
                                terminal_select_start(sat, 0, 0);
                                terminal_select_update(sat, sat->cols - 1, sat->rows - 1);
                            }
                        }
                        break;
                    case ACT_SETTINGS: {
#ifdef PLATFORM_MACOS
                        show_native_settings_panel(window, &app);
#else
                        settings_toggle(&app.settings);
#endif
                        break;
                    }
                    case ACT_SITE_MANAGER:
                        app.sites.active = !app.sites.active;
                        if (app.sites.active) {
                            if (app.sites.selected < 0 && app.site_mgr.count > 0)
                                app.sites.selected = 0;
                            app.sites.hover_row = app.sites.hover_action = -1;
                        }
                        break;
                    case ACT_FONT_BIGGER:
                        if (app.config.font_size < 72.0f) {
                            app.config.font_size += 1.0f;
                            app_reload_font(&app, window);
                        }
                        break;
                    case ACT_FONT_SMALLER:
                        if (app.config.font_size > 6.0f) {
                            app.config.font_size -= 1.0f;
                            app_reload_font(&app, window);
                        }
                        break;
                    case ACT_FONT_RESET:
                        app.config.font_size = 12.0f;
                        app_reload_font(&app, window);
                        break;
                    case ACT_SCROLL_UP_PAGE:
                        if (tab) terminal_scroll_up(tab->terminal, tab->terminal->rows / 2);
                        break;
                    case ACT_SCROLL_DOWN_PAGE:
                        if (tab) terminal_scroll_down(tab->terminal, tab->terminal->rows / 2);
                        break;
                    case ACT_SCROLL_TO_TOP:
                        if (tab) terminal_scroll_up(tab->terminal, tab->terminal->sb_count);
                        break;
                    case ACT_SCROLL_TO_BOTTOM:
                        if (tab) terminal_scroll_to_bottom(tab->terminal);
                        break;
                    case ACT_CLEAR_SCREEN: {
                        /* Send Ctrl-L (form feed) to the focused pane so the
                         * shell's line editor clears the screen — same effect
                         * as pressing Ctrl-L, but on the Cmd+Shift+K binding. */
                        Session *cs = app_focused_session(&app);
                        if (cs) { u8 ff = 0x0C; session_write(cs, &ff, 1); }
                        break;
                    }
                    case ACT_BROADCAST_TOGGLE:
                        if (app.broadcast_mode) app_disable_broadcast(&app);
                        else app_enable_broadcast(&app, true);
                        break;
                    case ACT_PREV_PROMPT: {
                        /* Guard and act on the SAME terminal — with split
                         * panes the primary tab->terminal and the focused
                         * pane's terminal can differ (e.g. alternate-screen app
                         * on one side, zsh on the other). The old version
                         * checked tab->terminal's mode but jumped in the
                         * focused pane, so prev-prompt could fire inside an app
                         * and trash the cursor. */
                        Terminal *ft = app_focused_terminal(&app);
                        if (tab && ft && !(ft->mode & MODE_ALT_SCREEN))
                            terminal_goto_prev_prompt(ft);
                        break;
                    }
                    case ACT_NEXT_PROMPT: {
                        Terminal *ft = app_focused_terminal(&app);
                        if (tab && ft && !(ft->mode & MODE_ALT_SCREEN))
                            terminal_goto_next_prompt(ft);
                        break;
                    }
                    case ACT_IMPORT_SSH_CONFIG:
                        app_import_default_ssh_config(&app);
                        break;
                    case ACT_SAVE_FILE: {
                        FileBrowser *sfb = app_editing_fb(&app);
                        if (sfb) fb_editor_save(sfb);
                        break;
                    }
                    case ACT_QUAKE_TOGGLE:
                        if (platform_is_quake_mode(window)) {
                            platform_toggle_quake_window(window);
                        } else {
                            platform_set_quake_mode(window, true);
                        }
                        app.quake_active = platform_is_quake_mode(window);
                        break;
                    case ACT_CREATE_TAB_GROUP: {
                        i32 gi = app_create_tab_group(&app, NULL, (Color){0});
                        if (gi >= 0 && app.active_tab >= 0 && app.active_tab < app.tab_count) {
                            app_set_tab_group(&app, app.active_tab, gi);
                        }
                        break;
                    }
                    case ACT_TOGGLE_TAB_GROUP: {
                        if (app.active_tab >= 0 && app.active_tab < app.tab_count) {
                            i32 gi = app.tabs[app.active_tab].group_index;
                            if (gi >= 0) app_toggle_tab_group_collapsed(&app, gi);
                        }
                        break;
                    }
                    case ACT_FIND: {
                        /* In a markdown/editor viewer, Find means in-document
                         * find rather than terminal search. */
                        FileBrowser *ffb = app_find_fb(&app);
                        if (ffb) { fb_find_open(ffb, false); break; }
                        app.search_active = !app.search_active;
                        if (app.search_active) {
                            app.search_query[0] = '\0';
                            app.search_query_len = 0;
                        }
                        break;
                    }
                    case ACT_SSH_CONNECT:
                        app.ssh_dialog_active = !app.ssh_dialog_active;
                        if (app.ssh_dialog_active) {
                            app_reset_ssh_dialog(&app);
                        }
                        break;
                    case ACT_COMMAND_PALETTE:
                        app.palette_active = !app.palette_active;
                        if (app.palette_active) {
                            app.palette_query[0] = '\0';
                            app.palette_query_len = 0;
                            app.palette_selected = 0;
                            app.palette_scroll = 0;
                            app.palette_input_mode = 0;
                            /* Open palette with vault for host search */
                            palette_open(app.vault);
                        }
                        break;
                    case ACT_SPLIT_HORIZONTAL:
                        app_split_tab(&app, SPLIT_H);
                        break;
                    case ACT_SPLIT_VERTICAL:
                        app_split_tab(&app, SPLIT_V);
                        break;
                    case ACT_RENAME_TAB:
                        app_start_tab_rename(&app, app.active_tab);
                        break;
                    case ACT_VAULT_BROWSER:
                        if (app.vault && vault_is_unlocked(app.vault)) {
                            app_vault_browser_open(&app);
                        } else {
                            app_vault_open_unlock(&app, ACT_VAULT_BROWSER);
                        }
                        break;
                    case ACT_VAULT_UNLOCK:
                        if (app.vault && !vault_is_unlocked(app.vault)) {
                            app_vault_open_unlock(&app, 0);
                        }
                        break;
                    case ACT_VAULT_LOCK:
                        app_vault_lock_now(&app);
                        break;
                    case ACT_VAULT_CHANGE_MASTER:
                        if (app.vault && vault_is_unlocked(app.vault)) {
                            app.settings.active_tab = SETTINGS_TAB_VAULT;
                            if (!app.settings.open) settings_toggle(&app.settings);
                        }
                        break;
                    default:
                        break;
                    }
                    #undef KB_RELAYOUT
                    break; /* consumed by keybinding */
                }

                /* Not a keybinding — pass to focused terminal pane */
                last_key_mods = mods;
                if (!tab) break;

                Terminal *ft2 = app_focused_terminal(&app);
                Session *fs2 = app_focused_session(&app);
                if (!ft2 || !fs2) break;

                u8 out[32];
                i32 n = terminal_key_input(ft2, (u32)key, mods, out, sizeof(out));
                if (n > 0) {
                    app_send_input(&app, fs2, out, n);
                    if (!app.translate_active) {
                        if (key == KEY_ENTER) {
                            app_record_submitted_command(&app, ft2, fs2);
                            app_translate_shadow_clear(&app);
                        } else if (key == KEY_BACKSPACE &&
                                   !(mods & (MOD_ALT | MOD_SUPER))) {
                            app_translate_shadow_backspace(&app, ft2, fs2);
                        } else if ((mods & MOD_CTRL) &&
                                   !(mods & (MOD_ALT | MOD_SUPER)) &&
                                   (key == KEY_U || key == KEY_K)) {
                            app_translate_shadow_clear(&app);
                        } else {
                            /* Tab completion / history recall rewrite the
                             * shell line; arrows & friends move the cursor.
                             * Either way the latched ghost is stale now. */
                            app_autosuggest_clear(&app);
                            if (key == KEY_TAB || key == KEY_UP ||
                                key == KEY_DOWN) {
                                app_translate_shadow_clear(&app);
                            }
                        }
                    }
                    terminal_scroll_to_bottom(ft2);
                }
                break;
            }
            case EVENT_KEY_UP: {
                u32 key = event.key.key;
                u32 mods = event.key.mods;
                if (app_translate_is_ctrl_cmd_key(key) &&
                    !app_translate_ctrl_cmd_down(mods)) {
                    app.translate_chord_down = false;
                }
                break;
            }
            case EVENT_MOUSE_DOWN: {
                /* Coordinates are already in framebuffer pixels (platform layer converts) */
                f32 mx = (f32)event.mouse.x;
                f32 my = (f32)event.mouse.y;

                /* Crash banner: a click on it dismisses it and is consumed. */
                if (app.crash_banner_active &&
                    mx >= app.crash_banner_x &&
                    mx <  app.crash_banner_x + app.crash_banner_w &&
                    my >= app.crash_banner_y &&
                    my <  app.crash_banner_y + app.crash_banner_h) {
                    app.crash_banner_active = false;
                    needs_redraw = true;
                    break;
                }

                /* Palette scrollbar — grab the thumb (or page-jump on track).
                 * Geometry was cached by the renderer last frame; we trust it
                 * because the palette stays open while the modal animates. */
                if (app.palette_active && app.palette_sb_scroll_max > 0 &&
                    mx >= app.palette_sb_x &&
                    mx <  app.palette_sb_x + app.palette_sb_w &&
                    my >= app.palette_sb_track_y &&
                    my <  app.palette_sb_track_y + app.palette_sb_track_h) {
                    f32 thumb_range = app.palette_sb_track_h - app.palette_sb_thumb_h;
                    f32 thumb_y = app.palette_sb_track_y +
                                  ((f32)app.palette_scroll /
                                   (f32)app.palette_sb_scroll_max) * thumb_range;
                    if (my >= thumb_y && my < thumb_y + app.palette_sb_thumb_h) {
                        /* Grabbed the thumb — drag from here. */
                        app.palette_sb_grab_offset = my - thumb_y;
                    } else {
                        /* Clicked the track outside the thumb — jump so the
                         * thumb centres under the cursor, then continue as a
                         * drag for one continuous gesture. */
                        f32 want_thumb_y = my - app.palette_sb_thumb_h * 0.5f;
                        f32 t = (thumb_range > 0)
                            ? (want_thumb_y - app.palette_sb_track_y) / thumb_range
                            : 0.0f;
                        if (t < 0.0f) t = 0.0f;
                        if (t > 1.0f) t = 1.0f;
                        app.palette_scroll = (i32)(t * (f32)app.palette_sb_scroll_max + 0.5f);
                        app.palette_sb_grab_offset = app.palette_sb_thumb_h * 0.5f;
                    }
                    app.palette_sb_dragging = true;
                    break;
                }

                if (app.broadcast_overlay_active) {
                    f32 dpi = app.dpi_scale;
                    i32 row_count = 0;
                    for (i32 i = 0; i < app.tab_count; i++) {
                        if (app.tabs[i].session) row_count++;
                        if (app.tabs[i].session2) row_count++;
                    }

                    f32 ui_ch = 16.0f * dpi;
                    f32 row_h = ui_ch + 8 * dpi;
                    i32 visible_rows = row_count < 12 ? row_count : 12;
                    f32 pw = 520 * dpi;
                    f32 ph = 132 * dpi + visible_rows * row_h;
                    f32 ppx = ((f32)app.fb_width - pw) / 2;
                    f32 ppy = app.tab_bar_height + 28 * dpi;
                    f32 btn_y = ppy + ph - 40 * dpi;

                    if (mx < ppx || mx > ppx + pw || my < ppy || my > ppy + ph) {
                        app.broadcast_overlay_active = false;
                        break;
                    }

                    if (my >= btn_y && my <= btn_y + 24 * dpi) {
                        if (mx >= ppx + 14 * dpi && mx <= ppx + 82 * dpi) {
                            app_broadcast_select_all(&app, true);
                            break;
                        }
                        if (mx >= ppx + 90 * dpi && mx <= ppx + 194 * dpi) {
                            app_broadcast_select_active(&app);
                            break;
                        }
                        if (mx >= ppx + pw - 84 * dpi && mx <= ppx + pw - 16 * dpi) {
                            app.broadcast_overlay_active = false;
                            break;
                        }
                    }

                    f32 list_y = ppy + 48 * dpi;
                    i32 row = 0;
                    bool handled_row = false;
                    for (i32 i = 0; i < app.tab_count && row < visible_rows && !handled_row; i++) {
                        for (i32 pane = 0; pane < 2 && row < visible_rows; pane++) {
                            Session *s = (pane == 0) ? app.tabs[i].session : app.tabs[i].session2;
                            if (!s) continue;
                            f32 ry = list_y + row * row_h;
                            if (mx >= ppx + 12 * dpi && mx <= ppx + pw - 12 * dpi &&
                                my >= ry && my <= ry + row_h - 4 * dpi) {
                                app.broadcast_targets[i][pane] = !app.broadcast_targets[i][pane];
                                handled_row = true;
                                break;
                            }
                            row++;
                        }
                    }
                    break;
                }

                if (app.port_forward_dialog_active) {
                    f32 dpi = app.dpi_scale;
                    Session *ps = app_focused_session(&app);
                    bool available = ps && session_type(ps) == SESSION_SSH &&
                                     session_status(ps) == SESSION_CONNECTED;
                    i32 list_count = available ? session_local_forward_count(ps) : 0;
                    i32 visible = list_count < 8 ? list_count : 8;
                    f32 ui_ch = 16.0f * dpi;
                    f32 dw = 520 * dpi;
                    f32 dh = 222 * dpi + visible * (ui_ch + 8 * dpi);
                    f32 dx = ((f32)app.fb_width - dw) / 2;
                    f32 dy = ((f32)app.fb_height - dh) / 2;

                    if (mx < dx || mx > dx + dw || my < dy || my > dy + dh) {
                        app.port_forward_dialog_active = false;
                        break;
                    }

                    if (my >= dy + 42 * dpi && my <= dy + 66 * dpi) {
                        if (mx >= dx + 16 * dpi && mx <= dx + 94 * dpi) {
                            app.port_forward_mode = 0;
                            if (app.port_forward_field > 2) app.port_forward_field = 0;
                            break;
                        }
                        if (mx >= dx + 100 * dpi && mx <= dx + 182 * dpi) {
                            app.port_forward_mode = 1;
                            app.port_forward_field = 0;
                            break;
                        }
                    }

                    if (my >= dy + 78 * dpi && my <= dy + 106 * dpi) {
                        if (mx >= dx + 110 * dpi && mx <= dx + 198 * dpi) {
                            app.port_forward_field = 0;
                            break;
                        }
                    }

                    if (app.port_forward_mode == 0 && my >= dy + 114 * dpi && my <= dy + 142 * dpi) {
                        if (mx >= dx + 110 * dpi && mx <= dx + 360 * dpi) {
                            app.port_forward_field = 1;
                            break;
                        }
                        if (mx >= dx + 410 * dpi && mx <= dx + 490 * dpi) {
                            app.port_forward_field = 2;
                            break;
                        }
                    }

                    if (available &&
                        mx >= dx + dw - 112 * dpi && mx <= dx + dw - 20 * dpi &&
                        my >= dy + 74 * dpi && my <= dy + 102 * dpi) {
                        app_submit_port_forward(&app);
                        break;
                    }

                    if (available) {
                        f32 list_y = dy + 158 * dpi;
                        for (i32 i = 0; i < visible; i++) {
                            f32 ry = list_y + i * (ui_ch + 8 * dpi);
                            if (mx >= dx + dw - 82 * dpi && mx <= dx + dw - 20 * dpi &&
                                my >= ry - 2 * dpi && my <= ry + ui_ch + 4 * dpi) {
                                session_local_forward_remove(ps, i);
                                break;
                            }
                        }
                    }
                    break;
                }

                if (app.ssh_dialog_active) {
                    f32 dpi = app.dpi_scale;
                    f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
                    f32 pad     = 20 * dpi;
                    f32 gap     = 10 * dpi;
                    f32 field_h = 30 * dpi;
                    f32 label_h = 16 * dpi;
                    f32 hdr_h   = 40 * dpi;
                    f32 row_full = label_h + 4*dpi + field_h;
                    f32 body_h = pad
                               + row_full
                               + gap + row_full
                               + gap + row_full
                               + gap + gap
                               + field_h
                               + gap + ui_ch
                               + gap*2 + 32*dpi
                               + pad;
                    bool show_recent = app.ssh_history_count > 0;
                    f32 recent_w   = show_recent ? 220 * dpi : 0;
                    f32 recent_pad = show_recent ? pad       : 0;
                    f32 left_w     = 500 * dpi;
                    f32 dw = left_w + recent_pad + recent_w;
                    f32 dh = hdr_h + body_h;
                    f32 dx = ((f32)app.fb_width - dw) / 2;
                    f32 dy = ((f32)app.fb_height - dh) / 2;

                    /* Click outside the dialog dismisses it */
                    if (mx < dx || mx > dx + dw || my < dy || my > dy + dh) {
                        app.ssh_dialog_active = false;
                        break;
                    }

                    /* Right-column recent list: clicking a row starts a
                     * reconnect attempt immediately — password is cleared
                     * so the session's KBI fallback opens the kbi_dialog
                     * and the user re-enters the secret there. Nothing is
                     * ever read from plaintext history storage. */
                    if (show_recent && mx >= dx + left_w) {
                        f32 rw = recent_w;
                        f32 rh = dh - hdr_h - pad - pad;
                        f32 rx = dx + left_w;
                        f32 inner_x = rx + 10*dpi;
                        f32 card_w  = rw - 14*dpi;
                        f32 ry = dy + hdr_h + pad + label_h + 6*dpi + 8*dpi;
                        f32 card_h = 48 * dpi;
                        f32 card_gap = 6 * dpi;
                        i32 max_visible = (i32)((rh - (label_h + 14*dpi)) /
                                                (card_h + card_gap));
                        if (max_visible < 0) max_visible = 0;
                        i32 nrows = app.ssh_history_count < max_visible
                                      ? app.ssh_history_count : max_visible;
                        if (mx >= inner_x && mx <= inner_x + card_w) {
                            i32 row = (i32)((my - ry) / (card_h + card_gap));
                            if (row >= 0 && row < nrows) {
                                /* reject clicks in the gap between cards */
                                f32 within = (my - ry) - (f32)row * (card_h + card_gap);
                                if (within <= card_h) {
                                    VaultHistoryEntry *he = &app.ssh_history[row];
                                    app.ssh_history_selected = row;
                                    snprintf(app.ssh_host, sizeof(app.ssh_host), "%s", he->hostname);
                                    snprintf(app.ssh_user, sizeof(app.ssh_user), "%s", he->username);
                                    if (he->port > 0 && he->port != 22)
                                        snprintf(app.ssh_port, sizeof(app.ssh_port), "%d", he->port);
                                    else
                                        app.ssh_port[0] = '\0';
                                    memset(app.ssh_password, 0, sizeof(app.ssh_password));
                                    /* Recents are SSH history — never submit
                                     * them through a lingering telnet/serial
                                     * proto selection. */
                                    app.ssh_dialog_proto = 0;
                                    /* Kick off the connection — KBI dialog
                                     * will pop up from the session thread
                                     * when the server asks for a password. */
                                    app_ssh_dialog_submit(&app);
                                    break;
                                }
                            }
                        }
                    }

                    /* Field hit-test: each field's clickable rect spans the
                     * label + the field body so clicks on the caption above
                     * the input still focus it. */
                    f32 fy = dy + hdr_h + pad;

                    /* Host */
                    if (mx >= dx + pad && mx <= dx + left_w - pad &&
                        my >= fy && my <= fy + row_full) {
                        app.ssh_field = 0;
                        break;
                    }
                    fy += row_full + gap;
                    /* User — rendered (and clickable) for SSH only; the
                     * telnet/serial layout collapses this row entirely, so
                     * the zones below shift up to mirror the render. */
                    if (app.ssh_dialog_proto == 0) {
                        if (mx >= dx + pad && mx <= dx + left_w - pad &&
                            my >= fy && my <= fy + row_full) {
                            app.ssh_field = 1;
                            break;
                        }
                        fy += row_full + gap;
                    }
                    /* Port (+ Password row, SSH only) */
                    {
                        f32 port_w = 96 * dpi;
                        f32 pwd_x  = dx + pad + port_w + gap;
                        f32 pwd_w  = left_w - 2*pad - port_w - gap;
                        if (my >= fy && my <= fy + row_full) {
                            if (mx >= dx + pad && mx <= dx + pad + port_w) {
                                app.ssh_field = 2;
                                break;
                            }
                            if (app.ssh_dialog_proto == 0 &&
                                mx >= pwd_x && mx <= pwd_x + pwd_w) {
                                app.ssh_field = 3;
                                break;
                            }
                        }
                    }
                    fy += row_full + gap + gap/2;
                    /* Separator + options exist only in the SSH layout. */
                    if (app.ssh_dialog_proto == 0) {
                    fy += gap;

                    /* Options row (checkboxes) */
                    {
                        f32 chk_sz = 16 * dpi;
                        f32 opt_y = fy + (field_h - chk_sz)/2;
                        f32 mosh_label_w = 8 * ui_cw;   /* "Use Mosh" */
                        f32 x11_x = dx + pad + 170 * dpi;
                        f32 x11_label_w = 14 * ui_cw;   /* "X11 Forwarding" */

                        if (my >= opt_y && my <= opt_y + chk_sz) {
                            /* Use Mosh — box and label both toggle */
                            if (mx >= dx + pad &&
                                mx <= dx + pad + chk_sz + 8*dpi + mosh_label_w) {
                                app.ssh_use_mosh = !app.ssh_use_mosh;
                                break;
                            }
                            /* X11 Forwarding */
                            if (mx >= x11_x &&
                                mx <= x11_x + chk_sz + 8*dpi + x11_label_w) {
                                app.ssh_forward_x11 = !app.ssh_forward_x11;
                                break;
                            }
                        }
                    }
                    }

                    /* Connect button — bottom-right of form column */
                    {
                        f32 btn_h = 32 * dpi;
                        f32 btn_w = 120 * dpi;
                        f32 btn_y = dy + dh - pad - btn_h;
                        f32 btn_x = dx + left_w - pad - btn_w;
                        if (mx >= btn_x && mx <= btn_x + btn_w &&
                            my >= btn_y && my <= btn_y + btn_h) {
                            app_ssh_dialog_submit(&app);
                            break;
                        }
                    }
                    break;
                }

                /* Create Theme dialog click handling. The layout here
                 * mirrors the renderer in src/ui/ui.c — keep them in
                 * sync when one moves. */
                if (app.create_theme_active) {
                    f32 dpi = app.dpi_scale;
                    f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
                    f32 pad     = 20 * dpi;
                    f32 gap     = 10 * dpi;
                    f32 field_h = 30 * dpi;
                    f32 desc_h  = 70 * dpi;
                    f32 label_h = 16 * dpi;
                    f32 hdr_h   = 40 * dpi;
                    f32 dw      = 540 * dpi;

                    f32 body_h;
                    if (app.create_theme_phase == 0) {
                        body_h = pad
                               + label_h + 4*dpi + field_h
                               + gap + label_h + 4*dpi + desc_h
                               + gap + label_h + 4*dpi
                               + 64 * dpi
                               + gap + ui_ch
                               + gap*2 + 32*dpi
                               + pad;
                    } else {
                        body_h = pad
                               + 28 * dpi
                               + gap
                               + 2 * (ui_ch + 4*dpi)
                               + 4 * dpi + 8*dpi
                               + 96 * dpi
                               + gap*2 + 32*dpi
                               + pad;
                    }
                    f32 dh = hdr_h + body_h;
                    f32 dx = ((f32)app.fb_width - dw) / 2;
                    f32 dy = ((f32)app.fb_height - dh) / 2;

                    /* Click outside dismisses */
                    if (mx < dx || mx > dx + dw || my < dy || my > dy + dh) {
                        app_close_create_theme(&app);
                        break;
                    }

                    if (app.create_theme_phase == 0) {
                        /* Field hit-tests */
                        f32 y = dy + hdr_h + pad;
                        f32 nf_top = y + label_h + 4*dpi;
                        if (mx >= dx + pad && mx <= dx + dw - pad &&
                            my >= y && my <= nf_top + field_h) {
                            app.create_theme_field = 0;
                            break;
                        }
                        y = nf_top + field_h + gap;
                        f32 df_top = y + label_h + 4*dpi;
                        if (mx >= dx + pad && mx <= dx + dw - pad &&
                            my >= y && my <= df_top + desc_h) {
                            app.create_theme_field = 1;
                            break;
                        }
                        y = df_top + desc_h + gap;

                        /* Agent picker chips */
                        f32 ay = y + label_h + 4*dpi;
                        f32 chip_h = 32 * dpi;
                        f32 chip_pad = 12 * dpi;
                        f32 chip_gap = 8 * dpi;
                        f32 chip_x = dx + pad;
                        f32 row_y = ay;
                        for (i32 ai = 0;
                             app.create_theme_agents &&
                             ai < app.create_theme_agent_count; ai++) {
                            const AgentInfo *a = &app.create_theme_agents[ai];
                            i32 dlen = (i32)strlen(a->display);
                            f32 w = chip_pad * 2 + (f32)dlen * ui_cw;
                            if (chip_x + w > dx + dw - pad) {
                                chip_x = dx + pad;
                                row_y += chip_h + chip_gap;
                            }
                            if (mx >= chip_x && mx <= chip_x + w &&
                                my >= row_y && my <= row_y + chip_h) {
                                app.create_theme_agent_idx = ai;
                                app.create_theme_field = 2;
                                break;
                            }
                            chip_x += w + chip_gap;
                        }
                        /* If a chip handled it, we already broke above. */

                        /* Buttons (Cancel / Generate) at the bottom-right. */
                        f32 btn_h = 32 * dpi;
                        f32 btn_w = 140 * dpi;
                        f32 btn_y = dy + dh - pad - btn_h;
                        f32 gen_x = dx + dw - pad - btn_w;
                        f32 cancel_x = gen_x - btn_w - gap;
                        if (my >= btn_y && my <= btn_y + btn_h) {
                            if (mx >= gen_x && mx <= gen_x + btn_w) {
                                app_create_theme_submit(&app);
                                break;
                            }
                            if (mx >= cancel_x && mx <= cancel_x + btn_w) {
                                app_close_create_theme(&app);
                                break;
                            }
                        }
                    } else {
                        /* Running / success / error: single primary button
                         * on the right (Cancel/Close), and a Back button
                         * to the left in error mode. */
                        f32 btn_h = 32 * dpi;
                        f32 btn_w = 140 * dpi;
                        f32 btn_y = dy + dh - pad - btn_h;
                        f32 right_x = dx + dw - pad - btn_w;
                        if (my >= btn_y && my <= btn_y + btn_h) {
                            if (mx >= right_x && mx <= right_x + btn_w) {
                                if (app.create_theme_phase == 1) {
                                    /* Cancel = SIGTERM the agent and reset
                                     * to the input phase so the user can
                                     * try again. */
                                    create_theme_kill_child(&app);
                                    app.create_theme_phase = 0;
                                } else {
                                    app_close_create_theme(&app);
                                }
                                break;
                            }
                            if (app.create_theme_phase == 3) {
                                f32 back_x = right_x - btn_w - gap;
                                if (mx >= back_x && mx <= back_x + btn_w) {
                                    app.create_theme_phase = 0;
                                    break;
                                }
                            }
                        }
                    }
                    break;
                }

                /* Right-click: tab context menu, URL copy, or paste */
                if (event.mouse.button == 1) {
                    HitResult rc_hit = ui_hit_test(&app, mx, my);

                    /* Right-click on a tab opens the tab context menu. */
                    if (rc_hit.type == HIT_TAB) {
                        /* Cancel any ongoing rename first */
                        if (app.tab_rename_active) app_confirm_tab_rename(&app);
                        app.tab_ctx_menu_active    = true;
                        app.tab_ctx_menu_tab_index = rc_hit.index;
                        app.tab_ctx_menu_x         = mx;
                        app.tab_ctx_menu_y         = my;
                        app.tab_ctx_menu_selected  = -1;
                        if (app.group_ctx_menu_active) app.group_ctx_menu_active = false;
                        break;
                    }
                    /* Right-click on a workspace chip opens the group ctx menu. */
                    if (rc_hit.type == HIT_TAB_GROUP_HEADER) {
                        if (app.tab_rename_active) app_confirm_tab_rename(&app);
                        app.group_ctx_menu_active       = true;
                        app.group_ctx_menu_group_index  = rc_hit.index;
                        app.group_ctx_menu_x            = mx;
                        app.group_ctx_menu_y            = my;
                        app.group_ctx_menu_selected     = -1;
                        if (app.tab_ctx_menu_active) app.tab_ctx_menu_active = false;
                        break;
                    }
                    /* Any right-click elsewhere also closes the group menu. */
                    if (app.group_ctx_menu_active) app.group_ctx_menu_active = false;
                    /* Any right-click elsewhere also closes an open tab menu. */
                    if (app.tab_ctx_menu_active) app.tab_ctx_menu_active = false;

                    /* Right-click on a sidebar file entry opens the fb ctx menu */
                    if (rc_hit.type == HIT_SIDEBAR_ITEM && app.filebrowser.open) {
                        app.sidebar_focused           = true;
                        app.filebrowser.selected      = rc_hit.index;
                        app.fb_ctx_menu_active        = true;
                        app.fb_ctx_menu_entry         = rc_hit.index;
                        app.fb_ctx_menu_x             = mx;
                        app.fb_ctx_menu_y             = my;
                        app.fb_ctx_menu_selected      = -1;
                        break;
                    }
                    /* Right-click on sidebar background (or anywhere else) with
                     * the fb menu open closes it. */
                    if (app.fb_ctx_menu_active) app.fb_ctx_menu_active = false;

                    /* If hovering a URL, copy it to clipboard instead of pasting */
                    if (app.url_hover_active && app.hover_url.url[0]) {
                        platform_clipboard_set(app.hover_url.url);
                        break;
                    }
                    /* Check if right-click is on a URL even without Cmd held */
                    if (rc_hit.type == HIT_TERMINAL) {
                        Terminal *rt = app_focused_terminal(&app);
                        if (rt) {
                            i32 rcol = rc_hit.index;
                            i32 rrow = rc_hit.sub_index;
                            if (rcol >= 0 && rcol < rt->cols && rrow >= 0 && rrow < rt->rows) {
                                TermURL rc_url;
                                if (url_detect_at(rt, rcol, rrow, &rc_url)) {
                                    platform_clipboard_set(rc_url.url);
                                    break;
                                }
                            }
                        }

                        /* No URL under cursor — open the terminal context menu
                         * (Split Right / Left / Down / Up). Paste is still
                         * available via Cmd+V or middle-click. */
                        app.term_ctx_menu_active   = true;
                        app.term_ctx_menu_x        = mx;
                        app.term_ctx_menu_y        = my;
                        app.term_ctx_menu_selected = -1;
                        break;
                    }

                    /* Outside the terminal area: no menu, no paste. */
                    break;
                }

                HitResult hit = ui_hit_test(&app, mx, my);

                /* Transcript viewer intercepts clicks — click outside panel closes. */
                if (app.transcript_viewer_active) {
                    f32 dpi = app.dpi_scale;
                    f32 pw = (f32)app.fb_width  * 0.70f; if (pw < 600.0f * dpi) pw = 600.0f * dpi;
                    f32 ph = (f32)app.fb_height * 0.80f; if (ph < 400.0f * dpi) ph = 400.0f * dpi;
                    f32 ppx = ((f32)app.fb_width  - pw) * 0.5f;
                    f32 ppy = ((f32)app.fb_height - ph) * 0.5f;
                    bool inside = (mx >= ppx && mx < ppx + pw && my >= ppy && my < ppy + ph);
                    if (!inside) {
                        app.transcript_viewer_active = false;
                        app_transcript_free(&app);
                    }
                    break;
                }

                /* Agent resume picker takes precedence over everything else. */
                if (app.agent_picker_active) {
                    f32 dpi = app.dpi_scale;
                    f32 pw = 440.0f * dpi;
                    f32 title_h = 52.0f * dpi;
                    f32 row_h   = 42.0f * dpi;
                    i32 rows = app.agent_picker_row_count;
                    f32 ph = title_h + (f32)rows * row_h + 14.0f * dpi;
                    f32 ppx = ((f32)app.fb_width  - pw) * 0.5f;
                    f32 ppy = ((f32)app.fb_height - ph) * 0.5f;
                    bool inside = (mx >= ppx && mx < ppx + pw && my >= ppy && my < ppy + ph);
                    if (!inside) { app.agent_picker_active = false; break; }
                    if (my >= ppy + title_h) {
                        i32 ri = (i32)((my - ppy - title_h) / row_h);
                        if (ri >= 0 && ri < rows) app_agent_picker_execute(&app, ri);
                    }
                    break;
                }

                /* Tab context menu takes precedence: execute the hovered row
                 * if the click is inside the menu, otherwise dismiss it. */
                if (app.tab_ctx_menu_active) {
                    f32 dpi = app.dpi_scale;
                    i32 ti = app.tab_ctx_menu_tab_index;
                    bool tab_in_group = (ti >= 0 && ti < app.tab_count &&
                                         app.tabs[ti].group_index >= 0 &&
                                         app.tabs[ti].group_index < MAX_TAB_GROUPS &&
                                         app.tab_groups[app.tabs[ti].group_index].used);
                    f32 menu_w = 230.0f * dpi; /* must match render width in ui.c */
                    f32 row_h  = 26.0f * dpi;
                    f32 vpad   = 6.0f * dpi;
                    i32 menu_rows = tab_in_group ? 4 : 3;
                    f32 menu_h = vpad * 2.0f + row_h * (f32)menu_rows;
                    f32 mxm = app.tab_ctx_menu_x;
                    f32 mym = app.tab_ctx_menu_y;

                    bool inside = (mx >= mxm && mx < mxm + menu_w &&
                                   my >= mym && my < mym + menu_h);
                    if (inside) {
                        i32 row = (i32)((my - mym - vpad) / row_h);
                        if (row >= 0 && row < menu_rows) {
                            switch (row) {
                            case 0: /* Change Title */
                                app_start_tab_rename(&app, ti);
                                break;
                            case 1: /* Close Tab */
                                if (app_confirm_close_arm(&app, ti, -1, -1))
                                    break;
                                app_close_tab(&app, ti);
                                if (!app_respawn_on_empty(&app) && app.tab_count == 0) {
                                    app.tab_ctx_menu_active = false;
                                    goto cleanup;
                                }
                                break;
                            case 2: /* Add to New Group */
                                {
                                    i32 gi = app_create_tab_group(&app, NULL, (Color){0});
                                    if (gi >= 0) app_set_tab_group(&app, ti, gi);
                                }
                                break;
                            case 3: /* Remove from Group (only when grouped) */
                                if (tab_in_group) app_remove_tab_from_group(&app, ti);
                                break;
                            }
                        }
                    }
                    app.tab_ctx_menu_active = false;
                    break;
                }

                /* Workspace chip context menu */
                if (app.group_ctx_menu_active) {
                    f32 dpi = app.dpi_scale;
                    f32 menu_w = 220.0f * dpi;
                    f32 row_h  = 26.0f * dpi;
                    f32 vpad   = 6.0f * dpi;
                    f32 swatch = 16.0f * dpi;
                    f32 swatch_pad = 6.0f * dpi;
                    f32 swatch_row_h = swatch + swatch_pad * 2.0f;
                    i32 menu_rows = 5;
                    f32 menu_h = swatch_row_h + 1.0f * dpi +
                                 vpad * 2.0f + row_h * (f32)menu_rows;
                    f32 mxm = app.group_ctx_menu_x;
                    f32 mym = app.group_ctx_menu_y;
                    i32 gi  = app.group_ctx_menu_group_index;

                    bool inside = (mx >= mxm && mx < mxm + menu_w &&
                                   my >= mym && my < mym + menu_h);
                    if (inside) {
                        bool consumed = false;
                        /* Palette strip */
                        if (my >= mym && my < mym + swatch_row_h) {
                            f32 sw_total = 8.0f * swatch + 7.0f * swatch_pad;
                            f32 sw_x0 = mxm + (menu_w - sw_total) * 0.5f;
                            f32 sw_y  = mym + swatch_pad;
                            for (i32 i = 0; i < 8; i++) {
                                f32 sx = sw_x0 + (f32)i * (swatch + swatch_pad);
                                if (mx >= sx - 2.0f * dpi &&
                                    mx <  sx + swatch + 2.0f * dpi &&
                                    my >= sw_y - 2.0f * dpi &&
                                    my <  sw_y + swatch + 2.0f * dpi) {
                                    app_recolor_tab_group(&app, gi,
                                                          app_group_palette_color(i));
                                    consumed = true;
                                    /* keep menu open after recolor */
                                    break;
                                }
                            }
                            if (consumed) break;
                        }
                        /* Action rows */
                        f32 rows_y0 = mym + swatch_row_h + 1.0f * dpi + vpad;
                        if (my >= rows_y0) {
                            i32 row = (i32)((my - rows_y0) / row_h);
                            if (row >= 0 && row < menu_rows) {
                                switch (row) {
                                case 0: /* Rename — enter inline chip rename */
                                    app_start_chip_rename(&app, gi);
                                    break;
                                case 1: /* New Tab in Group */
                                    {
                                        Session *s = app_new_local_session_here(&app);
                                        if (s && session_status(s) == SESSION_CONNECTED) {
                                            i32 idx = app_new_tab(&app, s, "Terminal");
                                            if (idx >= 0) app_set_tab_group(&app, idx, gi);
                                        }
                                    }
                                    break;
                                case 2: /* Toggle Open/Close (collapse + sleep) */
                                    app_toggle_tab_group_collapsed(&app, gi);
                                    break;
                                case 3: /* Ungroup — keep tabs, drop group */
                                    app_ungroup_tab_group(&app, gi);
                                    break;
                                case 4: /* Delete Group — close all tabs in group */
                                    if (app_confirm_close_arm(&app, app.active_tab,
                                                              -1, gi))
                                        break;
                                    app_close_tab_group(&app, gi);
                                    if (!app_respawn_on_empty(&app) && app.tab_count == 0) {
                                        app.group_ctx_menu_active = false;
                                        goto cleanup;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    app.group_ctx_menu_active = false;
                    break;
                }

                /* File-browser context menu */
                if (app.fb_ctx_menu_active) {
                    f32 dpi = app.dpi_scale;
                    f32 menu_w = 200.0f * dpi;
                    f32 row_h  = 26.0f * dpi;
                    f32 vpad   = 6.0f * dpi;
                    i32 menu_rows = 8;
                    f32 menu_h = vpad * 2.0f + row_h * (f32)menu_rows;
                    f32 mxm = app.fb_ctx_menu_x;
                    f32 mym = app.fb_ctx_menu_y;

                    bool inside = (mx >= mxm && mx < mxm + menu_w &&
                                   my >= mym && my < mym + menu_h);
                    if (inside) {
                        i32 row = (i32)((my - mym - vpad) / row_h);
                        if (row >= 0 && row < menu_rows) {
                            i32 ei = app.fb_ctx_menu_entry;
                            app_fb_ctx_action(&app, row, ei);
                        }
                    }
                    app.fb_ctx_menu_active = false;
                    break;
                }

                /* Terminal area context menu — Split Right/Left/Down/Up. */
                if (app.term_ctx_menu_active) {
                    f32 dpi = app.dpi_scale;
                    f32 menu_w = 200.0f * dpi;
                    f32 row_h  = 26.0f * dpi;
                    f32 vpad   = 6.0f * dpi;
                    i32 menu_rows = 4;
                    f32 menu_h = vpad * 2.0f + row_h * (f32)menu_rows;
                    f32 mxm = app.term_ctx_menu_x;
                    f32 mym = app.term_ctx_menu_y;

                    bool inside = (mx >= mxm && mx < mxm + menu_w &&
                                   my >= mym && my < mym + menu_h);
                    if (inside) {
                        i32 row = (i32)((my - mym - vpad) / row_h);
                        switch (row) {
                        case 0: app_split_tab_dir(&app, SPLIT_H, false); break; /* Right */
                        case 1: app_split_tab_dir(&app, SPLIT_H, true);  break; /* Left */
                        case 2: app_split_tab_dir(&app, SPLIT_V, false); break; /* Down */
                        case 3: app_split_tab_dir(&app, SPLIT_V, true);  break; /* Up */
                        default: break;
                        }
                    }
                    app.term_ctx_menu_active = false;
                    break;
                }

                /* File-browser inline prompt (rename / new folder): outside
                 * click dismisses without applying, keypresses are handled
                 * in the KEY_DOWN path. */
                if (app.fb_prompt_active) {
                    f32 dpi = app.dpi_scale;
                    f32 dw = 420.0f * dpi;
                    f32 dh = 120.0f * dpi;
                    f32 dx = ((f32)app.fb_width  - dw) * 0.5f;
                    f32 dy = ((f32)app.fb_height - dh) * 0.5f;
                    if (mx < dx || mx > dx + dw || my < dy || my > dy + dh) {
                        app.fb_prompt_active = false;
                    }
                    break;
                }

                /* Cancel rename if clicking outside the renamed tab.
                 *
                 * Grace window: the editor opens on the FIRST mouse-down of a
                 * context-menu row click; a habitual double-click delivers a
                 * second down ~200 ms later that lands on whatever sat under
                 * the (now closed) menu and would commit + close the editor
                 * before any typing happened — the keystrokes then leak into
                 * the terminal and Enter "doesn't save". Ignore outside
                 * clicks for a beat after the rename starts. */
                if (app.tab_rename_active &&
                    platform_time_sec() - app.tab_rename_started_at > 0.40) {
                    if (hit.type != HIT_TAB || hit.index != app.tab_rename_index) {
                        app_confirm_tab_rename(&app);
                    }
                }
                /* Same for chip rename — clicking off the chip commits */
                if (app.chip_rename_active &&
                    platform_time_sec() - app.chip_rename_started_at > 0.40) {
                    if (hit.type != HIT_TAB_GROUP_HEADER ||
                        hit.index != app.chip_rename_group_index) {
                        app_confirm_chip_rename(&app);
                    }
                }

                #define RELAYOUT() do { \
                    i32 _w,_h,_fw,_fh; \
                    platform_window_get_size(window,&_w,&_h); \
                    platform_window_get_framebuffer_size(window,&_fw,&_fh); \
                    app_update_layout(&app,_w,_h,_fw,_fh); \
                } while(0)

#ifdef USE_METAL
                /* After re-creating the atlas we MUST re-point the renderer's
                 * cached MTLTexture pointer at the new one; otherwise the next
                 * renderer_flush_glyphs() tries to bind a freed texture and
                 * segfaults inside objc_retain. */
                #define METAL_RECREATE_ATLAS() do { \
                    font_atlas_create_metal_texture(&app.renderer.font, platform_get_gpu_device(window)); \
                    renderer_metal_set_atlas(&app.renderer, app.renderer.font.metal_texture); \
                } while (0)
#else
                #define METAL_RECREATE_ATLAS() ((void)0)
#endif

                /* Null the cached atlas pointer BEFORE destroy so any renderer
                 * path reached during rebuild sees NULL and early-returns
                 * instead of touching a freed MTLTexture. */
                #define RELOAD_FONT() do { \
                    renderer_metal_set_atlas(&app.renderer, NULL); \
                    font_atlas_destroy(&app.renderer.font); \
                    font_atlas_create(&app.renderer.font, app.config.font_path, \
                                      app.config.font_size, app.dpi_scale, \
                                      app.config.font_weight); \
                    METAL_RECREATE_ATLAS(); \
                    app.renderer.font.cell_width *= app.config.cell_width_scale; \
                    app.renderer.font.cell_height *= app.config.cell_height_scale; \
                    RELAYOUT(); \
                } while(0)

                /* Track sidebar focus so file-browser keyboard shortcuts
                 * only fire when the user is actively working in the sidebar. */
                app.sidebar_focused = (hit.type == HIT_SIDEBAR_ITEM ||
                                       hit.type == HIT_SIDEBAR ||
                                       hit.type == HIT_SIDEBAR_RESIZE);

                switch (hit.type) {
                case HIT_NONE: {
                    if (my < (TOOLBAR_HEIGHT_PT * app.dpi_scale)) {
                        platform_begin_window_drag(g_window_ptr);
                    }
                    break;
                }
                case HIT_TAB: {
                    /* Remember the tab that was active BEFORE this click so that a
                     * drag-to-split drop has a valid destination (the dragged tab
                     * itself becomes active below for visual feedback, so we need
                     * the prior tab as the split target). */
                    app.tab_drag_prev_active = (app.active_tab != hit.index) ? app.active_tab : -1;
                    app.tab_drag_pending = true;
                    app.tab_drag_index = hit.index;
                    app.tab_drag_start_x = mx;
                    app.tab_drag_offset_x = 0;
                    app.tab_drag_target = -1;
                    app.tab_drag_target_group = -1;
                    app.tab_dragging = false;
                    /* Select immediately so the user gets visual feedback */
                    app_switch_tab(&app, hit.index);
                    break;
                }
                case HIT_TAB_CLOSE:
                    app.tab_drag_pending = false;
                    app.tab_dragging = false;
                    app.tab_drag_index = -1;
                    app.tab_drag_target = -1;
                    app.tab_drag_target_group = -1;
                    if (app_confirm_close_arm(&app, hit.index, -1, -1))
                        break;
                    app_close_tab(&app, hit.index);
                    if (!app_respawn_on_empty(&app) && app.tab_count == 0)
                        goto cleanup;
                    break;
                case HIT_TAB_NEW: {
                    Session *s = app_new_local_session_here(&app);
                    if (s && session_status(s) == SESSION_CONNECTED) {
                        app_new_tab(&app, s, "Terminal");
                        RELAYOUT();
                    }
                    break;
                }
                case HIT_TAB_GROUP_HEADER: {
                    /* Toggle collapsed state of the clicked tab group */
                    app_toggle_tab_group_collapsed(&app, hit.index);
                    break;
                }
                case HIT_TOOLBAR_BTN:
                    switch (hit.sub_index) {
                    case TB_SIDEBAR:
                        app.sidebar_visible = !app.sidebar_visible;
                        app.sidebar_width = app.sidebar_visible ? SIDEBAR_DEFAULT_PT * app.dpi_scale : 0;
                        if (!app.sidebar_visible) { app.filebrowser.open = false; fb_close_viewer(&app.filebrowser); }
                        RELAYOUT();
                        break;
                    case TB_SETTINGS:
                        settings_toggle(&app.settings);
                        break;
                    case TB_FONT_UP:
                        if (app.config.font_size < 72.0f) {
                            app.config.font_size += 1.0f;
                            app_reload_font(&app, window);
                        }
                        break;
                    case TB_FONT_DOWN:
                        if (app.config.font_size > 6.0f) { app.config.font_size -= 1.0f; app_reload_font(&app, window); }
                        break;
                    case TB_SSH:
                        app_reset_ssh_dialog(&app);
                        break;
                    default: break;
                    }
                    break;
                case HIT_SIDEBAR_ITEM: {
                    i32 idx = hit.index;
                    u32 cmods = event.mouse.mods;

                    /* Update multi-selection.
                     *   Cmd+click  → toggle only this entry
                     *   Shift+click→ range from anchor..idx (preserve others)
                     *   plain click→ clear + select just this one
                     * Either way, the primary `selected` follows the click. */
                    if (cmods & MOD_SUPER) {
                        fb_sel_toggle(&app.filebrowser, idx);
                        app.filebrowser.selection_anchor = idx;
                    } else if (cmods & MOD_SHIFT) {
                        i32 anchor = app.filebrowser.selection_anchor;
                        if (anchor < 0) anchor = idx;
                        fb_sel_range(&app.filebrowser, anchor, idx);
                    } else {
                        /* Keep multi-selection if user pressed on an already-
                         * selected row: the drag that follows should carry
                         * the whole batch. Otherwise reset to a single pick. */
                        if (!fb_sel_has(&app.filebrowser, idx)) {
                            fb_sel_clear(&app.filebrowser);
                            fb_sel_set(&app.filebrowser, idx, true);
                        }
                        app.filebrowser.selection_anchor = idx;
                    }
                    app.filebrowser.selected = idx;

                    /* Arm a potential drag. If the mouse moves before release
                     * we begin a drag; otherwise MOUSE_UP opens the entry. */
                    app.fb_drag_src_entry = idx;
                    app.fb_drag_start_x   = mx;
                    app.fb_drag_start_y   = my;
                    app.fb_drag_cur_x     = mx;
                    app.fb_drag_cur_y     = my;
                    app.fb_drag_active    = false;
                    break;
                }
                case HIT_SIDEBAR:
                    /* Header click — three right-aligned buttons: up / refresh / close.
                     * Layout mirrors fb_render_sidebar — 3*cw zone each, rightmost = close.
                     * Clicking on the breadcrumb path (left of the buttons) arms a
                     * potential header-drag for detaching the sidebar into an FB tab. */
                    if (app.filebrowser.open) {
                        f32 cw_ui = 8.0f * app.dpi_scale;
                        f32 hdr_pad = 16.0f * app.dpi_scale;
                        f32 w = app.sidebar_width;
                        f32 close_end   = w - hdr_pad;
                        f32 close_start = close_end - 3 * cw_ui;
                        f32 refresh_start = close_start - 3 * cw_ui;
                        f32 up_start = refresh_start - 3 * cw_ui;
                        f32 rx = hit.rel_x;
                        if (rx >= close_start && rx < close_end) {
                            app.sidebar_visible = false;
                            app.sidebar_width = 0;
                            app.filebrowser.open = false;
                            fb_close_viewer(&app.filebrowser);
                            i32 _w,_h,_fw,_fh;
                            platform_window_get_size(window,&_w,&_h);
                            platform_window_get_framebuffer_size(window,&_fw,&_fh);
                            app_update_layout(&app,_w,_h,_fw,_fh);
                        } else if (rx >= refresh_start && rx < close_start) {
                            bool wg = (app.filebrowser.view_mode == FVIEW_GRAPH);
                            fb_refresh(&app.filebrowser);
                            if (wg) fb_graph_rescope(&app.filebrowser, app.filebrowser.cwd);
                        } else if (rx >= up_start && rx < refresh_start) {
                            bool wg = (app.filebrowser.view_mode == FVIEW_GRAPH);
                            fb_go_up(&app.filebrowser);
                            if (wg) fb_graph_rescope(&app.filebrowser, app.filebrowser.cwd);
                        } else {
                            /* Path/breadcrumb area — arm header drag. */
                            app.fb_header_drag_pending = true;
                            app.fb_header_drag_active  = false;
                            app.fb_header_drag_start_x = mx;
                            app.fb_header_drag_start_y = my;
                        }
                    }
                    break;
                case HIT_SIDEBAR_RESIZE:
                    app.sidebar_resizing = true;
                    break;
                case HIT_TERMINAL: {
                    Tab *active = app_active_tab(&app);
                    if (active) {
                        if (active->sleeping) {
                            if (app_wake_tab(&app, app.active_tab)) {
                                needs_redraw = true;
                            }
                            break;
                        }
                        /* Cmd+click on terminal area:
                         *   - In a split: arm a pane layout drag and pick
                         *     the active pane from the live geometry
                         *     (app_pane_index_at). Suppress the fallthrough
                         *     pane-focus block below so hit.rel_x can't
                         *     overwrite the choice for multi-pane trees.
                         *   - In a single-pane tab: straight to URL-open.
                         * The pending arm is consumed by MOUSE_UP. */
                        bool cmd_click_handled = false;
                        if (event.mouse.mods & MOD_SUPER) {
                            if (active->split != SPLIT_NONE) {
                                i32 src_pane = app_pane_index_at(&app, active, mx, my);
                                if (src_pane >= 0) {
                                    app.pane_drag_pending  = true;
                                    app.pane_drag_active   = false;
                                    app.pane_drag_tab_index = app.active_tab;
                                    app.pane_drag_src_pane = src_pane;
                                    app.pane_drag_hover_pane = src_pane;
                                    app.pane_drag_drop_zone = 0;
                                    app.pane_drag_start_x  = mx;
                                    app.pane_drag_start_y  = my;
                                    active->active_pane = src_pane;
                                    cmd_click_handled = true;
                                }
                            }
                            Terminal *ut = app_focused_terminal(&app);
                            if (ut) {
                                i32 ucol = hit.index;
                                i32 urow = hit.sub_index;
                                if (ucol >= 0 && ucol < ut->cols && urow >= 0 && urow < ut->rows) {
                                    TermURL clicked_url;
                                    if (url_detect_at(ut, ucol, urow, &clicked_url)) {
                                        /* URL won — discard the armed drag so
                                         * a stray mousemove doesn't promote
                                         * it into a phantom swap UI. */
                                        app.pane_drag_pending  = false;
                                        app.pane_drag_active   = false;
                                        app.pane_drag_tab_index = -1;
                                        app.pane_drag_src_pane  = -1;
                                        app.pane_drag_hover_pane = -1;
                                        app.pane_drag_drop_zone = 0;
                                        platform_open_url(clicked_url.url);
                                        break;
                                    }
                                }
                            }
                        }

                        /* Switch active pane on click if split — skipped
                         * when Cmd+click already set active_pane via
                         * app_pane_index_at so hit.rel_x's binary geometry
                         * can't clobber the multi-pane choice. */
                        if (cmd_click_handled) {
                            /* no-op: pane focus already set above */
                        } else if (active->split != SPLIT_NONE && active->split_root >= 0) {
                            i32 pane = (i32)hit.rel_x;
                            if (pane >= 0 && pane < active->split_pane_count)
                                active->active_pane = pane;
                        } else if (active->split == SPLIT_H) {
                            f32 div_x = app.sidebar_width + app.padding +
                                ((f32)app.fb_width - app.sidebar_width - app.padding*2) * active->split_ratio;
                            active->active_pane = (mx > div_x) ? 1 : 0;
                        } else if (active->split == SPLIT_V) {
                            f32 div_y = app.tab_bar_height + app.padding +
                                ((f32)app.fb_height - app.tab_bar_height - app.status_bar_height - app.padding*2) * active->split_ratio;
                            active->active_pane = (my > div_y) ? 1 : 0;
                        }

                        Terminal *ct = app_focused_terminal(&app);
                        Session *cs = app_focused_session(&app);
                        /* Cmd+click on a split armed a pane layout drag; skip
                         * the text-selection arm path so the user's mouse
                         * move doesn't ALSO start a selection underneath
                         * the drag overlay. Selection + pane drag rendering
                         * on top of each other was the "huge dark rectangle"
                         * artefact. URL clicks still fall through into the
                         * mouse-passthrough branch above if MODE_MOUSE_*
                         * isn't set, which is fine since the URL branch
                         * already discarded the pane drag. */
                        /* Click on an agent-preview thumbnail → open the full
                         * image. The thumbnail is Liu chrome painted over the
                         * agent's output, so it wins ahead of text selection
                         * and mouse-mode forwarding. Plain left-click only. */
                        if (ct && !cmd_click_handled &&
                            !(event.mouse.mods & (MOD_SUPER | MOD_ALT | MOD_SHIFT | MOD_CTRL)) &&
                            app_term_try_open_image(&app, ct, mx, my)) {
                            mouse_selecting = false;
                            mouse_selecting_pending = false;
                            break;
                        }

                        if (ct && cs && !cmd_click_handled) {
                            i32 col = hit.index;
                            i32 row = hit.sub_index;
                            if (col >= 0 && col < ct->cols && row >= 0 && row < ct->rows) {
                                if (ct->mode & (MODE_MOUSE_BTN | MODE_MOUSE_MOTION)) {
                                    u8 mouse_buf[32];
                                    i32 mn = terminal_mouse_encode(ct, 0, col, row, true, 0,
                                                                    mouse_buf, sizeof(mouse_buf));
                                    if (mn > 0) session_write(cs, mouse_buf, mn);
                                } else {
                                    /* Text selection */
                                    f64 now = platform_time_sec();
                                    if (now - last_click_time < 0.4 &&
                                        col == last_click_col && row == last_click_row) {
                                        click_count++;
                                    } else {
                                        click_count = 1;
                                    }
                                    last_click_time = now;
                                    last_click_col = col;
                                    last_click_row = row;

                                    bool alt = (event.mouse.mods & MOD_ALT) != 0;
                                    if (click_count >= 2) {
                                        /* Word / line — activate immediately so the
                                         * highlight is visible without needing to drag. */
                                        selection_start(ct, col, row, click_count, alt);
                                        mouse_selecting = true;
                                        mouse_selecting_pending = false;
                                        if (app.config.copy_on_select) {
                                            char *sel = selection_get_text(ct);
                                            if (sel) {
                                                if (sel[0]) {
                                                    platform_clipboard_set(sel);
                                                    app_show_toast(&app, "Copied to clipboard");
                                                }
                                                free(sel);
                                            }
                                        }
                                    } else {
                                        /* Single-click: clear any prior selection but
                                         * defer creating a new one until the user actually
                                         * drags. Press-and-hold-in-place leaves no rect. */
                                        if (selection_active(ct)) selection_clear(ct);
                                        mouse_selecting_pending = true;
                                        mouse_selecting        = false;
                                        mouse_press_col        = col;
                                        mouse_press_row        = row;
                                        mouse_press_alt        = alt;
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
                case HIT_VIEWER_CLOSE: {
                    /* Closing a note opened from the graph returns to the graph
                     * (kept alive across the note); otherwise closes normally. */
                    bool back = fb_graph_can_return(&app.filebrowser);
                    fb_close_viewer_ex(&app.filebrowser, back);
                    if (back) fb_reenter_graph(&app.filebrowser);
                    break;
                }
                case HIT_VIEWER: {
                    if (fb_graph_active(&app.filebrowser)) {
                        if (fb_graph_panel_press(&app.filebrowser, mx, my)) {
                            needs_redraw = true; break;
                        }
                        fb_graph_press(&app.filebrowser,
                                       mx - app.filebrowser.graph_view_x,
                                       my - app.filebrowser.graph_view_y,
                                       app.filebrowser.graph_view_w,
                                       app.filebrowser.graph_view_h);
                        needs_redraw = true;
                        break;
                    }
                    /* Markdown link click — open URL or relative file. We
                     * try this first because the markdown viewer is not in
                     * editor_mode (the cursor-positioning branch below is
                     * a no-op there anyway), and links should win over a
                     * stray focus click. */
                    char link_url[FB_MAX_PATH];
                    {
                        if (app_md_try_copy(&app.filebrowser, mx, my)) break;
                        /* Clickable task checkbox — toggle [ ]<->[x] in the file. */
                        if (fb_md_toggle_task_at(&app.filebrowser, mx, my)) {
                            needs_redraw = true;
                            break;
                        }
                        u8 lk = 0;
                        if (fb_md_hit_link(&app.filebrowser, mx, my,
                                           link_url, sizeof(link_url), &lk)) {
                            app_open_md_link(&app.filebrowser, link_url, lk);
                            break;
                        }
                    }
                    /* Click in file viewer — position editor cursor */
                    if (app.filebrowser.editor_mode &&
                        app.filebrowser.view_mode == FVIEW_MARKDOWN &&
                        app.filebrowser.md_raw_mode) {
                        /* Live-preview editor: variable line heights, so replay
                         * the layout to map the click to (line, col). */
                        fb_md_live_locate(&app.filebrowser, mx, my);
                    } else if (app.filebrowser.editor_mode && app.filebrowser.view_mode != FVIEW_NONE) {
                        f32 vw = app.viewer_width > 0
                               ? app.viewer_width
                               : (f32)app.fb_width * VIEWER_WIDTH_RATIO;
                        f32 vx = (f32)app.fb_width - vw;
                        f32 vy = app.tab_bar_height;
                        f32 title_h = 22 * app.dpi_scale;
                        f32 content_y = vy + title_h + 4;
                        f32 pad = 12 * app.dpi_scale;
                        f32 ui_cw = 8.0f * app.dpi_scale;
                        f32 ui_ch = 16.0f * app.dpi_scale;
                        /* Line number gutter width */
                        f32 gutter_w = 5 * ui_cw;

                        i32 click_line = app.filebrowser.view_scroll +
                            (i32)((my - content_y) / ui_ch);
                        i32 click_col = (i32)((mx - vx - pad - gutter_w) / ui_cw);

                        if (click_line >= 0 && click_line < app.filebrowser.view_line_count) {
                            app.filebrowser.cursor_line = click_line;
                            if (click_col < 0) click_col = 0;
                            app.filebrowser.cursor_col = click_col;
                        }
                    } else if (app.filebrowser.view_mode == FVIEW_MARKDOWN &&
                               !app.filebrowser.md_raw_mode) {
                        /* Begin a text selection in the rendered markdown. */
                        fb_md_selection_begin(&app.filebrowser, mx, my);
                        md_sel_fb = &app.filebrowser;
                    }
                    break;
                }
                case HIT_VIEWER_RESIZE:
                    app.viewer_resizing = true;
                    break;
                case HIT_VIEWER_TITLE:
                    if (app.filebrowser.view_mode == FVIEW_MARKDOWN &&
                        app.filebrowser.view_path[0]) {
                        /* Arm detach drag — promotes to real drag once the mouse
                         * moves past a small threshold (so a plain click still
                         * works like "focus title"). */
                        app.viewer_drag_pending = true;
                        app.viewer_drag_start_x = mx;
                        app.viewer_drag_start_y = my;
                        app.viewer_drag_source = &app.filebrowser;
                        snprintf(app.viewer_drag_path, sizeof(app.viewer_drag_path),
                                 "%s", app.filebrowser.view_path);
                    }
                    break;
                case HIT_VIEWER_MD_TOGGLE:
                    fb_md_selection_clear(&app.filebrowser);
                    fb_md_toggle_edit(&app.filebrowser);
                    break;
                case HIT_SPLIT_DIVIDER: {
                    split_drag_node = hit.index;
                    /* Opt+click on divider: toggle split orientation (H <-> V).
                     * Normal click: start drag-to-resize. */
                    if (event.mouse.mods & MOD_ALT) {
                        Tab *st = app_active_tab(&app);
                        if (st && st->split != SPLIT_NONE) {
                            SplitType new_split = SPLIT_H;
                            if (st->split_root >= 0 &&
                                split_drag_node >= 0 &&
                                split_drag_node < st->split_next_node &&
                                st->split_nodes[split_drag_node].used &&
                                !st->split_nodes[split_drag_node].leaf) {
                                SplitLayoutNode *sn = &st->split_nodes[split_drag_node];
                                sn->split = (sn->split == SPLIT_H) ? SPLIT_V : SPLIT_H;
                                new_split = sn->split;
                                if (split_drag_node == st->split_root) st->split = new_split;
                            } else {
                                st->split = (st->split == SPLIT_H) ? SPLIT_V : SPLIT_H;
                                new_split = st->split;
                            }
                            app_resize_tab_panes(&app, st);
                            app_show_toast(&app, new_split == SPLIT_H
                                           ? "Split: horizontal (side-by-side)"
                                           : "Split: vertical (top-bottom)");
                        }
                    } else {
                        split_dragging = true;
                    }
                    break;
                }
                case HIT_SCROLLBAR: {
                    /* Click-to-scroll: calculate scroll position from click Y */
                    Terminal *st = app_focused_terminal(&app);
                    if (st && st->sb_count > 0) {
                        /* hit.rel_y is [0..1] from top of scrollbar area */
                        f32 click_ratio = 1.0f - hit.rel_y; /* invert: top=max scroll, bottom=0 */
                        i32 new_offset = (i32)(click_ratio * (f32)st->sb_count);
                        if (new_offset < 0) new_offset = 0;
                        if (new_offset > st->sb_count) new_offset = st->sb_count;
                        st->scroll_offset = new_offset;
                        TERM_DIRTY_ALL(st);
                        app.scrollbar_last_activity = platform_time_sec();
                    }
                    break;
                }
                case HIT_FBTAB_HEADER: {
                    /* Header buttons (up / refresh / close) for the FB tab —
                     * mirrors the docked sidebar's header click logic. The
                     * close button here closes the *FB tab itself* (since the
                     * embedded FB has nowhere to "hide").
                     * After up/refresh the tab title is re-derived from the
                     * (possibly new) cwd via app_refresh_fb_tab_title. */
                    Tab *atab = app_active_tab(&app);
                    if (!atab || atab->kind != TAB_FILEBROWSER || !atab->fb) break;
                    f32 cw_ui   = 8.0f * app.dpi_scale;
                    f32 hdr_pad = 16.0f * app.dpi_scale;
                    /* The header lives at x = ox..ox+list_w. Re-derive list_w. */
                    f32 ox = (app.sidebar_visible ? app.sidebar_width : 0) + app.padding;
                    f32 total_w = (f32)app.fb_width - ox - app.padding;
                    bool has_viewer = (atab->fb->view_mode != FVIEW_NONE);
                    f32 list_w = total_w;
                    if (has_viewer) {
                        f32 vmin = VIEWER_MIN_PT * app.dpi_scale;
                        f32 list_min = 200.0f * app.dpi_scale;
                        f32 ratio = atab->fb_viewer_ratio;
                        if (ratio <= 0.0f) ratio = VIEWER_WIDTH_RATIO;
                        f32 view_w = total_w * ratio;
                        if (view_w < vmin) view_w = vmin;
                        if (total_w - view_w < list_min) view_w = total_w - list_min;
                        if (view_w < vmin) view_w = vmin;
                        list_w = total_w - view_w;
                    }
                    f32 close_end   = list_w - hdr_pad;
                    f32 close_start = close_end - 3 * cw_ui;
                    f32 refresh_start = close_start - 3 * cw_ui;
                    f32 up_start = refresh_start - 3 * cw_ui;
                    f32 rx = hit.rel_x;
                    if (rx >= close_start && rx < close_end) {
                        app_close_tab(&app, app.active_tab);
                        if (!app_respawn_on_empty(&app) && app.tab_count == 0)
                            goto cleanup;
                    } else if (rx >= refresh_start && rx < close_start) {
                        bool wg = (atab->fb->view_mode == FVIEW_GRAPH);
                        fb_refresh(atab->fb);
                        if (wg) fb_graph_rescope(atab->fb, atab->fb->cwd);
                    } else if (rx >= up_start && rx < refresh_start) {
                        bool wg = (atab->fb->view_mode == FVIEW_GRAPH);
                        fb_go_up(atab->fb);
                        if (wg) fb_graph_rescope(atab->fb, atab->fb->cwd);
                        app_refresh_fb_tab_title(atab);
                    }
                    break;
                }
                case HIT_FBTAB_ITEM: {
                    Tab *atab = app_active_tab(&app);
                    if (!atab || atab->kind != TAB_FILEBROWSER || !atab->fb) break;
                    FileBrowser *fb = atab->fb;
                    i32 idx = hit.index;
                    if (idx < 0 || idx >= fb->entry_count) break;
                    if (idx == fb->selected) {
                        /* Capture before fb_open_file: navigating into a dir
                         * resets the entry array, so read these now. Gate on the
                         * view mode (not fb_graph_active, which is false once the
                         * graph is NULL) so an empty-folder graph isn't a dead
                         * end — the next folder click still re-scopes. */
                        bool was_graph = (fb->view_mode == FVIEW_GRAPH);
                        bool is_dir    = fb->entries[idx].is_dir;
                        /* fb_open_file may navigate (dir click) or open the
                         * viewer (file click). Either way, refresh the tab
                         * title — for files cwd is unchanged so it's a
                         * cheap no-op, for dirs the title now matches. */
                        bool opened = fb_open_file(fb, idx);
                        if (was_graph && is_dir) {
                            /* Folder filter: entering a folder re-scopes the
                             * graph to it (recursively), so a codebase's many
                             * .md files narrow to the marked folder. `..`
                             * widens back to the parent. */
                            fb_graph_rescope(fb, fb->cwd);
                        } else if (was_graph && opened && fb->graph &&
                                   fb->view_mode != FVIEW_GRAPH) {
                            /* Opened a note from the graph: keep the graph alive
                             * (pan/zoom/layout preserved) so closing the note
                             * returns to it. Freed on tab close (fb_destroy). */
                            fb->graph_return = true;
                        }
                        app_refresh_fb_tab_title(atab);
                    } else {
                        fb->selected = idx;
                        fb_sel_clear(fb);
                        fb_sel_set(fb, idx, true);
                        fb->selection_anchor = idx;
                    }
                    break;
                }
                case HIT_FBTAB_BODY: {
                    Tab *atab = app_active_tab(&app);
                    if (!atab || atab->kind != TAB_FILEBROWSER || !atab->fb) break;
                    fb_sel_clear(atab->fb);
                    atab->fb->selected = -1;
                    break;
                }
                case HIT_FBTAB_DIVIDER:
                    /* Click+drag the divider — set a flag re-using viewer_resizing
                     * since semantically it's the same operation; the MOUSE_MOVE
                     * handler reads active tab kind to decide which ratio to
                     * update. */
                    app.viewer_resizing = true;
                    break;
                case HIT_FBTAB_VIEWER:
                case HIT_FBTAB_VIEWER_TITLE: {
                    Tab *atab = app_active_tab(&app);
                    if (hit.type == HIT_FBTAB_VIEWER_TITLE) {
                        if (atab && atab->kind == TAB_FILEBROWSER && atab->fb &&
                            atab->fb->view_mode == FVIEW_MARKDOWN &&
                            atab->fb->view_path[0]) {
                            app.viewer_drag_pending = true;
                            app.viewer_drag_start_x = mx;
                            app.viewer_drag_start_y = my;
                            app.viewer_drag_source = atab->fb;
                            snprintf(app.viewer_drag_path, sizeof(app.viewer_drag_path),
                                     "%s", atab->fb->view_path);
                        }
                    } else if (atab && atab->kind == TAB_FILEBROWSER && atab->fb) {
                        if (fb_graph_active(atab->fb)) {
                            if (fb_graph_panel_press(atab->fb, mx, my)) {
                                needs_redraw = true; break;
                            }
                            fb_graph_press(atab->fb,
                                           mx - atab->fb->graph_view_x,
                                           my - atab->fb->graph_view_y,
                                           atab->fb->graph_view_w,
                                           atab->fb->graph_view_h);
                            needs_redraw = true;
                            break;
                        }
                        /* Content click: copy button, else open a markdown
                         * link, else begin a text selection. */
                        char link_url[FB_MAX_PATH];
                        u8 lk = 0;
                        if (app_md_try_copy(atab->fb, mx, my)) {
                            /* consumed */
                        } else if (fb_md_hit_link(atab->fb, mx, my, link_url, sizeof link_url, &lk)) {
                            app_open_md_link(atab->fb, link_url, lk);
                        } else if (atab->fb->view_mode == FVIEW_MARKDOWN &&
                                   !atab->fb->md_raw_mode) {
                            fb_md_selection_begin(atab->fb, mx, my);
                            md_sel_fb = atab->fb;
                        } else if (atab->fb->view_mode == FVIEW_MARKDOWN &&
                                   atab->fb->md_raw_mode) {
                            fb_md_live_locate(atab->fb, mx, my);
                        }
                    }
                    break;
                }
                case HIT_FBTAB_VIEWER_CLOSE: {
                    Tab *atab = app_active_tab(&app);
                    if (atab && atab->kind == TAB_FILEBROWSER && atab->fb &&
                        atab->fb_viewer_only) {
                        app_close_tab(&app, app.active_tab);
                        if (!app_respawn_on_empty(&app) && app.tab_count == 0)
                            goto cleanup;
                    } else if (atab && atab->kind == TAB_FILEBROWSER && atab->fb) {
                        /* A note opened from the graph returns to it on close. */
                        bool back = fb_graph_can_return(atab->fb);
                        fb_close_viewer_ex(atab->fb, back);
                        if (back) fb_reenter_graph(atab->fb);
                    }
                    break;
                }
                case HIT_FBTAB_VIEWER_MD_TOGGLE: {
                    Tab *atab = app_active_tab(&app);
                    if (atab && atab->kind == TAB_FILEBROWSER && atab->fb &&
                        atab->fb->view_mode == FVIEW_MARKDOWN) {
                        fb_md_selection_clear(atab->fb);
                        fb_md_toggle_edit(atab->fb);
                    }
                    break;
                }
                default:
                    break;
                }
                #undef RELAYOUT
                #undef RELOAD_FONT
                break;
            }
            case EVENT_MOUSE_MOVE: {
                app.hover_x = (f32)event.mouse.x;
                app.hover_y = (f32)event.mouse.y;

                /* Graph view: pan / node-drag while a gesture is active, else
                 * hover-highlight a node under the cursor. */
                {
                    FileBrowser *gfb = NULL;
                    Tab *gt = app_active_tab(&app);
                    if (gt && gt->kind == TAB_FILEBROWSER && gt->fb && fb_graph_active(gt->fb)) gfb = gt->fb;
                    else if (app.filebrowser.open && fb_graph_active(&app.filebrowser)) gfb = &app.filebrowser;
                    if (gfb) {
                        f32 rx = app.hover_x - gfb->graph_view_x;
                        f32 ry = app.hover_y - gfb->graph_view_y;
                        if (gfb->graph_drag_slider >= 0) {
                            fb_graph_panel_drag(gfb, app.hover_x, app.hover_y);
                            needs_redraw = true; break;
                        }
                        if (fb_graph_gesture(gfb)) {
                            fb_graph_move(gfb, rx, ry, gfb->graph_view_w, gfb->graph_view_h);
                            needs_redraw = true; break;
                        }
                        if (fb_graph_panel_hit(gfb, app.hover_x, app.hover_y)) {
                            gfb->graph_hover = -1;   /* don't highlight nodes under the panel */
                            needs_redraw = true; break;
                        }
                        if (rx >= 0 && rx < gfb->graph_view_w &&
                            ry >= 0 && ry < gfb->graph_view_h) {
                            fb_graph_hover(gfb, rx, ry, gfb->graph_view_w, gfb->graph_view_h);
                            needs_redraw = true; break;
                        }
                    }
                }

                /* Palette scrollbar drag — map mouse y back through the
                 * cached geometry to a scroll index. */
                if (app.palette_sb_dragging) {
                    f32 thumb_range = app.palette_sb_track_h - app.palette_sb_thumb_h;
                    if (thumb_range > 0 && app.palette_sb_scroll_max > 0) {
                        f32 want_thumb_y = (f32)event.mouse.y - app.palette_sb_grab_offset;
                        f32 t = (want_thumb_y - app.palette_sb_track_y) / thumb_range;
                        if (t < 0.0f) t = 0.0f;
                        if (t > 1.0f) t = 1.0f;
                        app.palette_scroll =
                            (i32)(t * (f32)app.palette_sb_scroll_max + 0.5f);
                        needs_redraw = true;
                    }
                    break;
                }

                /* Sidebar header drag — promote past threshold so a tiny
                 * wiggle on the breadcrumb path doesn't trigger detach. */
                if (app.fb_header_drag_pending && !app.fb_header_drag_active) {
                    f32 dx = (f32)event.mouse.x - app.fb_header_drag_start_x;
                    f32 dy = (f32)event.mouse.y - app.fb_header_drag_start_y;
                    f32 thresh = 5.0f * app.dpi_scale;
                    if (dx*dx + dy*dy > thresh*thresh) {
                        app.fb_header_drag_active = true;
                    }
                }

                /* Sidebar drag tracking — promote armed entry to an active
                 * drag once the mouse has moved past a small threshold, and
                 * hand off to the OS drag system once the cursor exits the
                 * sidebar (so Finder / other apps can accept the drop). */
                if (app.fb_drag_src_entry >= 0 && !app.fb_os_drag_handed_off) {
                    f32 mvx = (f32)event.mouse.x;
                    f32 mvy = (f32)event.mouse.y;
                    app.fb_drag_cur_x = mvx;
                    app.fb_drag_cur_y = mvy;
                    if (!app.fb_drag_active) {
                        f32 dx = mvx - app.fb_drag_start_x;
                        f32 dy = mvy - app.fb_drag_start_y;
                        f32 thresh = 5.0f * app.dpi_scale;
                        if (dx*dx + dy*dy > thresh*thresh) {
                            app.fb_drag_active = true;
                        }
                    }

                    /* Determine drag entry kind: directories stay in-app
                     * (so the user can drop them on the tab bar / terminal
                     * to spawn a file-browser tab), files behave as before
                     * and hand off to Finder past the sidebar edge. */
                    bool _drag_is_dir = false;
                    if (app.fb_drag_src_entry >= 0 &&
                        app.fb_drag_src_entry < app.filebrowser.entry_count) {
                        _drag_is_dir = app.filebrowser.entries[app.fb_drag_src_entry].is_dir;
                    }

                    /* Cursor over the tab bar always keeps the drag in-app
                     * regardless of file/dir, since dropping there spawns a
                     * new tab. */
                    f32 _tab_bar_h = (f32)(app.config.show_tab_bar || app.config.show_toolbar_icons)
                                     ? (TOOLBAR_HEIGHT_PT * app.dpi_scale) : 0.0f;
                    bool _over_tab_bar = (mvy < _tab_bar_h);

                    /* Handoff: once dragging and past the sidebar's right edge,
                     * promote to an OS-level drag so Finder etc. can receive.
                     * Skipped for directories and while hovering the tab bar. */
                    if (app.fb_drag_active && app.sidebar_visible &&
                        mvx > app.sidebar_width + 4.0f * app.dpi_scale &&
                        !_drag_is_dir && !_over_tab_bar) {
                        /* Build the promise list from the selection (or the
                         * single src entry if no multi-selection). */
                        PlatformFilePromise promises[FB_CLIP_MAX];
                        i32 pn = 0;
                        i32 sel_buf[FB_CLIP_MAX];
                        i32 sn = fb_sel_collect(&app.filebrowser, sel_buf, FB_CLIP_MAX);
                        if (sn == 0) { sel_buf[0] = app.fb_drag_src_entry; sn = 1; }
                        for (i32 i = 0; i < sn && pn < FB_CLIP_MAX; i++) {
                            i32 ei = sel_buf[i];
                            if (ei < 0 || ei >= app.filebrowser.entry_count) continue;
                            FileEntry *e = &app.filebrowser.entries[ei];
                            if (strcmp(e->name, "..") == 0) continue;
                            PlatformFilePromise *pp = &promises[pn++];
                            memset(pp, 0, sizeof(*pp));
                            snprintf(pp->display_name, sizeof(pp->display_name), "%s", e->name);
                            char path[FB_MAX_PATH * 2];
                            fb_entry_path(&app.filebrowser, e, path, sizeof(path));
                            if (app.filebrowser.source == FB_SOURCE_LOCAL) {
                                pp->is_remote = false;
                                snprintf(pp->local_path, sizeof(pp->local_path), "%s", path);
                            } else {
                                pp->is_remote = true;
                                /* ctx owns the info; freed by provider */
                                SFTPPromiseCtx *ctx = calloc(1, sizeof(*ctx));
                                if (!ctx) { pn--; continue; }
                                ctx->session = (Session *)app.filebrowser.session;
                                snprintf(ctx->remote_path, sizeof(ctx->remote_path), "%s", path);
                                pp->ctx = ctx;
                                pp->provider = sftp_promise_provider;
                            }
                        }
                        if (pn > 0 && platform_begin_file_drag(window, promises, pn)) {
                            app.fb_os_drag_handed_off = true;
                            app.fb_drag_active = false;
                            app.fb_drag_src_entry = -1;
                            app_fb_set_status(&app, "Dragging to Finder…");
                        } else {
                            /* Failed (rare); free any ctx we allocated. */
                            for (i32 i = 0; i < pn; i++) {
                                if (promises[i].is_remote && promises[i].ctx) free(promises[i].ctx);
                            }
                        }
                    }
                }

                /* Update fb context-menu hovered row for highlight. */
                if (app.fb_ctx_menu_active) {
                    f32 dpi = app.dpi_scale;
                    f32 menu_w = 200.0f * dpi;
                    f32 row_h  = 26.0f * dpi;
                    f32 vpad   = 6.0f * dpi;
                    f32 mxm = app.fb_ctx_menu_x;
                    f32 mym = app.fb_ctx_menu_y;
                    f32 mvx = (f32)event.mouse.x;
                    f32 mvy = (f32)event.mouse.y;
                    if (mvx >= mxm && mvx < mxm + menu_w &&
                        mvy >= mym + vpad && mvy < mym + vpad + row_h * 8.0f) {
                        i32 row = (i32)((mvy - mym - vpad) / row_h);
                        app.fb_ctx_menu_selected = (row >= 0 && row < 8) ? row : -1;
                    } else {
                        app.fb_ctx_menu_selected = -1;
                    }
                }

                /* SSH dialog recent list: update hover row for highlight. */
                if (app.ssh_dialog_active && app.ssh_history_count > 0) {
                    f32 dpi = app.dpi_scale;
                    f32 pad     = 20 * dpi;
                    f32 label_h = 16 * dpi;
                    f32 hdr_h   = 40 * dpi;
                    f32 field_h = 30 * dpi;
                    f32 ui_ch_l = 16.0f * dpi;
                    f32 gap     = 10 * dpi;
                    f32 row_full = label_h + 4*dpi + field_h;
                    f32 body_h = pad + row_full + gap + row_full + gap + row_full
                               + gap + gap + field_h + gap + ui_ch_l
                               + gap*2 + 32*dpi + pad;
                    f32 recent_w = 220 * dpi;
                    f32 left_w   = 500 * dpi;
                    f32 dw = left_w + pad + recent_w;
                    f32 dh = hdr_h + body_h;
                    f32 dx = ((f32)app.fb_width  - dw) / 2;
                    f32 dy = ((f32)app.fb_height - dh) / 2;
                    f32 rx = dx + left_w;
                    f32 inner_x = rx + 10*dpi;
                    f32 card_w  = recent_w - 14*dpi;
                    f32 ry = dy + hdr_h + pad + label_h + 14*dpi;
                    f32 card_h = 48 * dpi;
                    f32 card_gap = 6 * dpi;
                    f32 rh = dh - hdr_h - pad - pad;
                    i32 max_visible = (i32)((rh - (label_h + 14*dpi)) /
                                            (card_h + card_gap));
                    if (max_visible < 0) max_visible = 0;
                    i32 nrows = app.ssh_history_count < max_visible
                                  ? app.ssh_history_count : max_visible;
                    i32 new_hover = -1;
                    if (app.hover_x >= inner_x && app.hover_x <= inner_x + card_w) {
                        f32 dy_h = app.hover_y - ry;
                        i32 row = (i32)(dy_h / (card_h + card_gap));
                        f32 within = dy_h - (f32)row * (card_h + card_gap);
                        if (row >= 0 && row < nrows && within <= card_h)
                            new_hover = row;
                    }
                    app.ssh_history_hover = new_hover;
                    if (new_hover >= 0) platform_set_cursor(CURSOR_POINTER);
                }

                /* Agent resume picker: track hovered row for highlight. */
                if (app.agent_picker_active) {
                    f32 dpi = app.dpi_scale;
                    f32 pw = 440.0f * dpi;
                    f32 title_h = 52.0f * dpi;
                    f32 row_h   = 42.0f * dpi;
                    i32 rows = app.agent_picker_row_count;
                    f32 ph = title_h + (f32)rows * row_h + 14.0f * dpi;
                    f32 ppx = ((f32)app.fb_width  - pw) * 0.5f;
                    f32 ppy = ((f32)app.fb_height - ph) * 0.5f;
                    if (app.hover_x >= ppx && app.hover_x < ppx + pw &&
                        app.hover_y >= ppy + title_h && app.hover_y < ppy + ph) {
                        i32 ri = (i32)((app.hover_y - ppy - title_h) / row_h);
                        if (ri >= 0 && ri < rows) app.agent_picker_selected = ri;
                        platform_set_cursor(CURSOR_POINTER);
                    }
                }

                /* Terminal context menu: hover row tracking. */
                if (app.term_ctx_menu_active) {
                    f32 dpi = app.dpi_scale;
                    f32 menu_w = 200.0f * dpi;
                    f32 row_h  = 26.0f * dpi;
                    f32 vpad   = 6.0f * dpi;
                    i32 menu_rows = 4;
                    f32 menu_h = vpad * 2.0f + row_h * (f32)menu_rows;
                    f32 mxm = app.term_ctx_menu_x;
                    f32 mym = app.term_ctx_menu_y;
                    if (app.hover_x >= mxm && app.hover_x < mxm + menu_w &&
                        app.hover_y >= mym && app.hover_y < mym + menu_h) {
                        i32 row = (i32)((app.hover_y - mym - vpad) / row_h);
                        if (row < 0 || row >= menu_rows) row = -1;
                        app.term_ctx_menu_selected = row;
                        platform_set_cursor(CURSOR_POINTER);
                    } else {
                        app.term_ctx_menu_selected = -1;
                    }
                }

                /* Tab context menu: track which row is under the cursor. */
                if (app.tab_ctx_menu_active) {
                    f32 dpi = app.dpi_scale;
                    i32 ti2 = app.tab_ctx_menu_tab_index;
                    bool tab_in_group2 = (ti2 >= 0 && ti2 < app.tab_count &&
                                          app.tabs[ti2].group_index >= 0 &&
                                          app.tabs[ti2].group_index < MAX_TAB_GROUPS &&
                                          app.tab_groups[app.tabs[ti2].group_index].used);
                    f32 menu_w = 230.0f * dpi; /* must match render width in ui.c */
                    f32 row_h  = 26.0f * dpi;
                    f32 vpad   = 6.0f * dpi;
                    i32 menu_rows = tab_in_group2 ? 4 : 3;
                    f32 menu_h = vpad * 2.0f + row_h * (f32)menu_rows;
                    f32 mxm = app.tab_ctx_menu_x;
                    f32 mym = app.tab_ctx_menu_y;
                    if (app.hover_x >= mxm && app.hover_x < mxm + menu_w &&
                        app.hover_y >= mym && app.hover_y < mym + menu_h) {
                        i32 row = (i32)((app.hover_y - mym - vpad) / row_h);
                        if (row < 0 || row >= menu_rows) row = -1;
                        app.tab_ctx_menu_selected = row;
                        platform_set_cursor(CURSOR_POINTER);
                    } else {
                        app.tab_ctx_menu_selected = -1;
                    }
                }

                /* Workspace chip context menu: hover row. */
                if (app.group_ctx_menu_active) {
                    f32 dpi = app.dpi_scale;
                    f32 menu_w = 220.0f * dpi;
                    f32 row_h  = 26.0f * dpi;
                    f32 vpad   = 6.0f * dpi;
                    f32 swatch = 16.0f * dpi;
                    f32 swatch_pad = 6.0f * dpi;
                    f32 swatch_row_h = swatch + swatch_pad * 2.0f;
                    i32 menu_rows = 5;
                    f32 menu_h = swatch_row_h + 1.0f * dpi +
                                 vpad * 2.0f + row_h * (f32)menu_rows;
                    f32 mxm = app.group_ctx_menu_x;
                    f32 mym = app.group_ctx_menu_y;
                    if (app.hover_x >= mxm && app.hover_x < mxm + menu_w &&
                        app.hover_y >= mym && app.hover_y < mym + menu_h) {
                        f32 rows_y0 = mym + swatch_row_h + 1.0f * dpi + vpad;
                        if (app.hover_y >= rows_y0) {
                            i32 row = (i32)((app.hover_y - rows_y0) / row_h);
                            if (row < 0 || row >= menu_rows) row = -1;
                            app.group_ctx_menu_selected = row;
                        } else {
                            app.group_ctx_menu_selected = -1;
                        }
                        platform_set_cursor(CURSOR_POINTER);
                    } else {
                        app.group_ctx_menu_selected = -1;
                    }
                }

                /* Pane layout drag (Cmd held) — promote to active once the
                 * cursor crosses the 6dpi threshold, then track the pane
                 * under the cursor as the drop target. */
                if (app.pane_drag_pending || app.pane_drag_active) {
                    f32 px = (f32)event.mouse.x;
                    f32 py = (f32)event.mouse.y;
                    f32 dxp = px - app.pane_drag_start_x;
                    f32 dyp = py - app.pane_drag_start_y;
                    f32 thr = 6.0f * app.dpi_scale;
                    if (!app.pane_drag_active &&
                        (dxp * dxp + dyp * dyp) > (thr * thr)) {
                        app.pane_drag_active = true;
                        app.pane_drag_pending = false;
                        platform_set_cursor(CURSOR_POINTER);
                    }
                    if (app.pane_drag_active) {
                        Tab *dtab = (app.pane_drag_tab_index >= 0 &&
                                     app.pane_drag_tab_index < app.tab_count)
                                  ? &app.tabs[app.pane_drag_tab_index] : NULL;
                        if (dtab) {
                            i32 hp = app_pane_index_at(&app, dtab, px, py);
                            app.pane_drag_hover_pane = hp;
                            app.pane_drag_drop_zone =
                                (hp >= 0 && hp != app.pane_drag_src_pane)
                              ? app_pane_drop_zone_at(&app, dtab, hp, px, py)
                              : 0;
                        }
                        needs_redraw = true;
                    }
                }

                /* Tab drag reorder */
                if (app.tab_drag_pending || app.tab_dragging) {
                    f32 dx = app.hover_x - app.tab_drag_start_x;

                    if (!app.tab_dragging) {
                        /* Check if drag threshold exceeded (5px in fb pixels) */
                        f32 threshold = 5.0f * app.dpi_scale;
                        if (dx > threshold || dx < -threshold) {
                            app.tab_dragging = true;
                            app.tab_drag_pending = false;
                        }
                    }

                    if (app.tab_dragging) {
                        app.tab_drag_offset_x = dx;
                        platform_set_cursor(CURSOR_DEFAULT);

                        /* Hover over a workspace chip → mark as group drop target.
                         * Cleared every frame; final value used at mouse-up. */
                        {
                            HitResult dh = ui_hit_test(&app, app.hover_x, app.hover_y);
                            if (dh.type == HIT_TAB_GROUP_HEADER) {
                                app.tab_drag_target_group = dh.index;
                            } else {
                                app.tab_drag_target_group = -1;
                            }
                        }

                        /* Detect if cursor has moved below the tab bar into
                         * the terminal area -- switch to split-drop mode */
                        f32 split_threshold = app.tab_bar_height + 20.0f * app.dpi_scale;
                        if (app.hover_y > split_threshold && app.tab_count > 1) {
                            app.tab_drag_into_split = true;

                            f32 sb_w = app.sidebar_visible ? app.sidebar_width : 0;
                            f32 tx = sb_w + app.padding;
                            f32 ty = app.tab_bar_height + app.padding;
                            f32 tw2 = (f32)app.fb_width - tx - app.padding;
                            f32 th2 = (f32)app.fb_height - ty - app.status_bar_height - app.padding;

                            f32 rx = tw2 > 0 ? (app.hover_x - tx) / tw2 : 0.5f;
                            f32 ry = th2 > 0 ? (app.hover_y - ty) / th2 : 0.5f;

                            f32 edge = 0.25f;
                            if      (rx < edge)       app.tab_drag_split_zone = 1; /* left  */
                            else if (rx > 1.0f - edge) app.tab_drag_split_zone = 2; /* right */
                            else if (ry < edge)       app.tab_drag_split_zone = 3; /* top   */
                            else if (ry > 1.0f - edge) app.tab_drag_split_zone = 4; /* bottom*/
                            else                      app.tab_drag_split_zone = 0;
                            app.tab_drag_target = -1;
                            needs_redraw = true;
                            break;
                        } else {
                            app.tab_drag_into_split = false;
                            app.tab_drag_split_zone = 0;
                        }

                        /* Tab reorder target — walk the visual order (grouped
                         * tabs by group, then ungrouped) so chip widths are
                         * accounted for correctly. Drop target = linear tabs[]
                         * slot the cursor is over (or the gap to its right). */
                        f32 tbx = app._tab_bar_x;
                        f32 tw  = app._tab_w;
                        f32 tg  = app._tab_gap;
                        
                        f32 mx_local = app.hover_x;
                        i32 target = -1;
                        f32 walk_x = tbx;

                        /* Grouped pass */
                        for (i32 gi = 0; gi < MAX_TAB_GROUPS && target < 0; gi++) {
                            if (!app.tab_groups[gi].used) continue;
                            bool has_tabs = false;
                            for (i32 i = 0; i < app.tab_count; i++)
                                if (app.tabs[i].group_index == gi) { has_tabs = true; break; }
                            if (!has_tabs) continue;
                            f32 chip_w = app_get_group_chip_width(&app, gi);
                            walk_x += chip_w + tg;
                            if (app.tab_groups[gi].collapsed) continue;
                            for (i32 i = 0; i < app.tab_count; i++) {
                                if (app.tabs[i].group_index != gi) continue;
                                if (mx_local < walk_x + tw * 0.5f) { target = i; break; }
                                walk_x += tw + tg;
                            }
                        }
                        /* Ungrouped pass */
                        if (target < 0) {
                            for (i32 i = 0; i < app.tab_count; i++) {
                                i32 gi2 = app.tabs[i].group_index;
                                if (gi2 >= 0 && gi2 < MAX_TAB_GROUPS &&
                                    app.tab_groups[gi2].used) continue;
                                if (mx_local < walk_x + tw * 0.5f) { target = i; break; }
                                walk_x += tw + tg;
                            }
                        }
                        if (target < 0) target = app.tab_count - 1;
                        if (target < 0) target = 0;
                        if (target >= app.tab_count) target = app.tab_count - 1;

                        if (target != app.tab_drag_index && !app.tab_drag_into_split &&
                            app.tab_drag_target_group < 0) {
                            /* Live array reorder: swap dragged tab visually & logically */
                            i32 src_gi = app.tabs[app.tab_drag_index].group_index;
                            i32 dst_gi = app.tabs[target].group_index;
                            
                            /* Only swap immediately if they are in the same group context, 
                             * or both ungrouped. Moving across groups should still be handled on drop 
                             * to avoid jarring jumps, but if no groups are involved, it's fine. */
                            if (src_gi == dst_gi) {
                                app_move_tab(&app, app.tab_drag_index, target);
                                /* Since we shifted the array, update the dragged index to match its new physical location */
                                app.tab_drag_index = target;
                                needs_redraw = true;
                            }
                        }

                        app.tab_drag_target = target;
                        break;
                    }
                }

                /* Split divider drag */
                if (split_dragging) {
                    Tab *active = app_active_tab(&app);
                    if (active && active->split != SPLIT_NONE) {
                        f32 sb_w = app.sidebar_visible ? app.sidebar_width : 0;
                        f32 term_x = sb_w + app.padding;
                        f32 term_y = app.tab_bar_height + app.padding +
                                      app.config.style.terminal_top_gap * app.dpi_scale;
                        f32 term_w = (f32)app.fb_width - sb_w - app.padding * 2;
                        f32 term_h = (f32)app.fb_height - term_y - app.status_bar_height - app.padding;

                        f32 ratio;
                        if (active->split_root >= 0 &&
                            split_drag_node >= 0 &&
                            split_drag_node < active->split_next_node &&
                            active->split_nodes[split_drag_node].used &&
                            !active->split_nodes[split_drag_node].leaf) {
                            MainPaneRect nr = {0};
                            bool found = main_split_node_rect(active, active->split_root,
                                                              split_drag_node,
                                                              (MainPaneRect){term_x, term_y, term_w, term_h},
                                                              app.dpi_scale, &nr);
                            SplitLayoutNode *sn = &active->split_nodes[split_drag_node];
                            if (found && nr.w > 1.0f && nr.h > 1.0f) {
                                if (sn->split == SPLIT_H) {
                                    ratio = (app.hover_x - nr.x) / nr.w;
                                    platform_set_cursor(CURSOR_RESIZE_H);
                                } else {
                                    ratio = (app.hover_y - nr.y) / nr.h;
                                    platform_set_cursor(CURSOR_RESIZE_V);
                                }
                                if (ratio < 0.15f) ratio = 0.15f;
                                if (ratio > 0.85f) ratio = 0.85f;
                                sn->ratio = ratio;
                                if (split_drag_node == active->split_root) {
                                    active->split_ratio = ratio;
                                    active->split = sn->split;
                                }
                                app_resize_tab_panes(&app, active);
                                needs_redraw = true;
                            }
                        } else {
                            if (active->split == SPLIT_H) {
                                ratio = (app.hover_x - term_x) / term_w;
                                platform_set_cursor(CURSOR_RESIZE_H);
                            } else {
                                ratio = (app.hover_y - term_y) / term_h;
                                platform_set_cursor(CURSOR_RESIZE_V);
                            }
                            if (ratio < 0.15f) ratio = 0.15f;
                            if (ratio > 0.85f) ratio = 0.85f;
                            active->split_ratio = ratio;
                            app_resize_tab_panes(&app, active);
                            needs_redraw = true;
                        }
                    }
                    break;
                }

                /* Update cursor based on hover position */
                if (!app.sidebar_resizing) {
                    HitResult hover_hit = ui_hit_test(&app, app.hover_x, app.hover_y);
                    bool url_cursor = false;

                    /* URL detection: only when Cmd is held and mouse is over terminal */
                    if ((event.mouse.mods & MOD_SUPER) && hover_hit.type == HIT_TERMINAL) {
                        Terminal *ut = app_focused_terminal(&app);
                        if (ut) {
                            i32 ucol = hover_hit.index;
                            i32 urow = hover_hit.sub_index;
                            if (ucol >= 0 && ucol < ut->cols && urow >= 0 && urow < ut->rows) {
                                /* Only re-scan if the cell changed */
                                if (ucol != app.url_hover_col || urow != app.url_hover_row || !app.url_hover_active) {
                                    app.url_hover_col = ucol;
                                    app.url_hover_row = urow;
                                    app.url_hover_active = url_detect_at(ut, ucol, urow, &app.hover_url);
                                }
                                if (app.url_hover_active) {
                                    url_cursor = true;
                                    platform_set_cursor(CURSOR_POINTER);
                                    /* Force redraw to show underline */
                                    if (ut->dirty_rows)
                                        TERM_DIRTY_ALL(ut);
                                }
                            }
                        }
                    } else if (app.url_hover_active) {
                        /* Cmd released or moved away from terminal — clear URL hover */
                        app.url_hover_active = false;
                        app.url_hover_col = -1;
                        app.url_hover_row = -1;
                        memset(&app.hover_url, 0, sizeof(app.hover_url));
                        Terminal *ut = app_focused_terminal(&app);
                        if (ut && ut->dirty_rows)
                            TERM_DIRTY_ALL(ut);
                    }

                    if (!url_cursor) {
                        /* Modals that accept keyboard input show the I-beam
                         * over their content area — same "this is a text
                         * field" affordance as native fields. Hit-test for
                         * those modals isn't per-field, so we fall through
                         * to CURSOR_TEXT on the default branch when one is
                         * open. Known non-text spots (toolbar, tab bar,
                         * resize edges) still take precedence via the
                         * cases below. */
                        bool text_modal_active =
                            app.palette_active ||
                            app.search_active ||
                            app.ssh_dialog_active ||
                            app.port_forward_dialog_active ||
                            app.passphrase_dialog_active ||
                            app.known_hosts_open ||
                            app.settings.open ||
                            app.filebrowser.editor_mode ||
                            app.cmd_history_active;
                        switch (hover_hit.type) {
                        /* Liu's read-heavy flow: skip the permanent I-beam
                         * many terminals use; arrow by default, I-beam only
                         * during an active selection drag. */
                        case HIT_TERMINAL:
                            platform_set_cursor(mouse_selecting ? CURSOR_TEXT : CURSOR_DEFAULT);
                            break;
                        case HIT_SIDEBAR_RESIZE:
                        case HIT_VIEWER_RESIZE:  platform_set_cursor(CURSOR_RESIZE_H); break;
                        case HIT_SPLIT_DIVIDER: {
                            Tab *st = app_active_tab(&app);
                            SplitType split = st ? st->split : SPLIT_V;
                            if (st && st->split_root >= 0 &&
                                hover_hit.index >= 0 &&
                                hover_hit.index < st->split_next_node &&
                                st->split_nodes[hover_hit.index].used &&
                                !st->split_nodes[hover_hit.index].leaf) {
                                split = st->split_nodes[hover_hit.index].split;
                            }
                            if (split == SPLIT_H) platform_set_cursor(CURSOR_RESIZE_H);
                            else platform_set_cursor(CURSOR_RESIZE_V);
                            break;
                        }
                        case HIT_TAB: case HIT_TAB_CLOSE: case HIT_TAB_NEW:
                        case HIT_TAB_GROUP_HEADER:
                        case HIT_TOOLBAR_BTN:    platform_set_cursor(CURSOR_POINTER); break;
                        case HIT_VIEWER: {
                            /* Hand cursor when hovering a markdown link or the
                             * code-block Copy button; fall back to the editor's
                             * text caret otherwise. */
                            char hover_url[FB_MAX_PATH];
                            const u8 *ctxt = NULL; u32 clen = 0;
                            if (fb_md_hit_copy(&app.filebrowser, app.hover_x, app.hover_y, &ctxt, &clen) ||
                                fb_md_hit_link(&app.filebrowser, app.hover_x, app.hover_y,
                                               hover_url, sizeof(hover_url), NULL)) {
                                platform_set_cursor(CURSOR_POINTER);
                            } else if (app.filebrowser.editor_mode) {
                                platform_set_cursor(CURSOR_TEXT);
                            } else {
                                platform_set_cursor(CURSOR_DEFAULT);
                            }
                            break;
                        }
                        default:
                            /* Settings panel publishes per-frame hover rects
                             * for its clickable controls (nav rows, buttons,
                             * chips). When the cursor is over one of those,
                             * show a pointing-hand so users can tell what's
                             * interactive — overlay panels weren't covered
                             * by the HIT_* path because they don't register
                             * a HitResult type. */
                            if (app.settings.open &&
                                settings_point_clickable(&app.settings,
                                                         (f32)event.mouse.x,
                                                         (f32)event.mouse.y)) {
                                platform_set_cursor(CURSOR_POINTER);
                            } else {
                                platform_set_cursor(text_modal_active ? CURSOR_TEXT : CURSOR_DEFAULT);
                            }
                            break;
                        }
                    }

                    /* Selection drag */
                    if (mouse_selecting && hover_hit.type == HIT_TERMINAL) {
                        Terminal *dt = app_focused_terminal(&app);
                        if (dt) {
                            i32 col = hover_hit.index;
                            i32 row = hover_hit.sub_index;
                            if (col >= 0 && col < dt->cols && row >= 0 && row < dt->rows)
                                selection_update(dt, col, row);
                        }
                    }
                    /* Promote a pending single-click to an active selection
                     * once the cursor leaves the press cell. This is what
                     * keeps a plain press-and-hold from leaving a 1-cell
                     * highlight stuck on screen. */
                    if (mouse_selecting_pending && hover_hit.type == HIT_TERMINAL) {
                        Terminal *dt = app_focused_terminal(&app);
                        if (dt) {
                            i32 col = hover_hit.index;
                            i32 row = hover_hit.sub_index;
                            if (col >= 0 && col < dt->cols && row >= 0 && row < dt->rows &&
                                (col != mouse_press_col || row != mouse_press_row)) {
                                selection_start(dt, mouse_press_col, mouse_press_row,
                                                1, mouse_press_alt);
                                selection_update(dt, col, row);
                                mouse_selecting         = true;
                                mouse_selecting_pending = false;
                            }
                        }
                    }
                }

                if (app.sidebar_resizing) {
                    f32 new_w = app.hover_x;
                    if (new_w < SIDEBAR_MIN_PT * app.dpi_scale) new_w = SIDEBAR_MIN_PT * app.dpi_scale;
                    if (new_w > SIDEBAR_MAX_PT * app.dpi_scale) new_w = SIDEBAR_MAX_PT * app.dpi_scale;
                    app.sidebar_width = new_w;
                    /* Remember the chosen width (points) so it survives a
                     * close/reopen and an app restart. Persisted on mouse-up. */
                    app.config.sidebar_width = new_w / app.dpi_scale;
                    i32 _w,_h,_fw,_fh;
                    platform_window_get_size(window,&_w,&_h);
                    platform_window_get_framebuffer_size(window,&_fw,&_fh);
                    app_update_layout(&app,_w,_h,_fw,_fh);
                }
                if (app.viewer_resizing) {
                    /* If the active tab is an FB tab, the divider lives
                     * inside the tab content area — adjust that tab's
                     * fb_viewer_ratio instead of the global sidebar viewer
                     * width. */
                    Tab *vat = app_active_tab(&app);
                    if (vat && vat->kind == TAB_FILEBROWSER) {
                        f32 ox = (app.sidebar_visible ? app.sidebar_width : 0) + app.padding;
                        f32 total_w = (f32)app.fb_width - ox - app.padding;
                        if (total_w > 1.0f) {
                            f32 view_w = (f32)app.fb_width - app.padding - app.hover_x;
                            f32 vmin = VIEWER_MIN_PT * app.dpi_scale;
                            f32 list_min = 200.0f * app.dpi_scale;
                            if (view_w < vmin) view_w = vmin;
                            if (total_w - view_w < list_min) view_w = total_w - list_min;
                            if (view_w < vmin) view_w = vmin;
                            vat->fb_viewer_ratio = view_w / total_w;
                            if (vat->fb_viewer_ratio < 0.1f) vat->fb_viewer_ratio = 0.1f;
                            if (vat->fb_viewer_ratio > 0.9f) vat->fb_viewer_ratio = 0.9f;
                        }
                    } else {
                        f32 new_w = (f32)app.fb_width - app.hover_x;
                        f32 vmin = VIEWER_MIN_PT * app.dpi_scale;
                        f32 vmax = (f32)app.fb_width
                                 - (app.sidebar_visible ? app.sidebar_width : 0.0f)
                                 - VIEWER_RESIZE_GRAB_PT * app.dpi_scale;
                        if (vmax < vmin) vmax = vmin;
                        if (new_w < vmin) new_w = vmin;
                        if (new_w > vmax) new_w = vmax;
                        app.viewer_width = new_w;
                    }
                }
                /* Viewer detach drag — promote pending to dragging past 6px */
                if (app.viewer_drag_pending && !app.viewer_dragging) {
                    f32 threshold = 6.0f * app.dpi_scale;
                    f32 dx = app.hover_x - app.viewer_drag_start_x;
                    f32 dy = app.hover_y - app.viewer_drag_start_y;
                    if (dx*dx + dy*dy > threshold*threshold) {
                        app.viewer_dragging = true;
                    }
                }
                /* Extend a rendered-markdown text selection while dragging.
                 * The cached fb may have been freed if its owning tab closed
                 * mid-drag (e.g. Cmd+W before the mouse-up arrives), so
                 * re-validate it against the live file browsers before
                 * dereferencing — otherwise this is a use-after-free. The
                 * pointer match alone isn't enough: if a new tab's fb malloc
                 * happens to land at the freed address, the comparison
                 * spuriously succeeds. After the pointer check (which proves
                 * md_sel_fb names a currently-allocated FB) we also gate on
                 * md_sel_active, which only a FB that actually had
                 * fb_md_selection_begin called on it will have set. */
                if (md_sel_fb) {
                    bool sel_fb_live = (md_sel_fb == &app.filebrowser);
                    for (i32 ti = 0; !sel_fb_live && ti < app.tab_count; ti++)
                        if (app.tabs[ti].fb == md_sel_fb) sel_fb_live = true;
                    if (sel_fb_live && md_sel_fb->md_sel_active) {
                        fb_md_selection_update(md_sel_fb, app.hover_x, app.hover_y);
                        needs_redraw = true;
                    } else {
                        md_sel_fb = NULL;
                    }
                }
                break;
            }
            case EVENT_MOUSE_UP: {
                /* Graph view: end pan / node-drag; a click (no drag) on a node
                 * opens that .md file in the markdown viewer. */
                {
                    FileBrowser *gfb = NULL;
                    Tab *gt = app_active_tab(&app);
                    if (gt && gt->kind == TAB_FILEBROWSER && gt->fb && fb_graph_active(gt->fb)) gfb = gt->fb;
                    else if (app.filebrowser.open && fb_graph_active(&app.filebrowser)) gfb = &app.filebrowser;
                    if (gfb && gfb->graph_drag_slider >= 0) {
                        fb_graph_panel_release(gfb);
                        needs_redraw = true; break;
                    }
                    if (gfb && fb_graph_gesture(gfb)) {
                        const char *open = fb_graph_release(gfb,
                            app.hover_x - gfb->graph_view_x,
                            app.hover_y - gfb->graph_view_y,
                            gfb->graph_view_w, gfb->graph_view_h);
                        if (open && open[0]) {
                            /* Opened a note by clicking a graph node — keep the
                             * graph alive so closing the note returns to it.
                             * fb_open_md_path preserves it when graph_return is
                             * set, and re-pins the flag for the new note. */
                            gfb->graph_return = true;
                            fb_open_md_path(gfb, open);
                        }
                        needs_redraw = true;
                        break;
                    }
                }
                app.palette_sb_dragging = false;
                if (app.sidebar_resizing) {
                    /* Persist the resized sidebar width to config on release. */
                    config_save(&app.config, config_file_path());
                }
                app.sidebar_resizing = false;
                app.viewer_resizing = false;
                split_dragging = false;
                split_drag_node = -1;
                md_sel_fb = NULL;  /* end any markdown selection drag */

                /* Pane layout drag drop. Only honors when:
                 *   - drag was active (threshold crossed)
                 *   - hover pane/edge is valid and differs from source
                 *   - tab index is still valid (not deleted mid-drag) */
                if (app.pane_drag_active) {
                    if (app.pane_drag_tab_index >= 0 &&
                        app.pane_drag_tab_index < app.tab_count &&
                        app.pane_drag_hover_pane >= 0 &&
                        app.pane_drag_hover_pane != app.pane_drag_src_pane &&
                        app.pane_drag_drop_zone >= 1 &&
                        app.pane_drag_drop_zone <= 4) {
                        Tab *dtab = &app.tabs[app.pane_drag_tab_index];
                        app_move_pane_to_zone(&app, dtab,
                                              app.pane_drag_src_pane,
                                              app.pane_drag_hover_pane,
                                              app.pane_drag_drop_zone);
                    }
                    needs_redraw = true;
                }
                app.pane_drag_pending  = false;
                app.pane_drag_active   = false;
                app.pane_drag_src_pane = -1;
                app.pane_drag_hover_pane = -1;
                app.pane_drag_drop_zone = 0;
                app.pane_drag_tab_index = -1;

                /* Sidebar header drag resolution. If the user dragged the
                 * breadcrumb area out and released over the tab bar, detach
                 * the sidebar's filebrowser into a fresh FB tab and close
                 * the docked sidebar. Below threshold = no-op (a stray
                 * click on the path stays a click, opening nothing). */
                if (app.fb_header_drag_pending) {
                    bool was_active = app.fb_header_drag_active;
                    app.fb_header_drag_pending = false;
                    app.fb_header_drag_active  = false;
                    if (was_active && app.filebrowser.cwd[0]) {
                        f32 mu_x = (f32)event.mouse.x;
                        f32 mu_y = (f32)event.mouse.y;
                        f32 tb_h = (app.config.show_tab_bar || app.config.show_toolbar_icons)
                                   ? (TOOLBAR_HEIGHT_PT * app.dpi_scale) : 0.0f;
                        bool on_tabbar = (mu_y < tb_h);
                        HitResult mu_hit = ui_hit_test(&app, mu_x, mu_y);
                        bool on_termarea = (mu_hit.type == HIT_TERMINAL);
                        /* Only LOCAL sidebars detach (FB tabs are local-only). */
                        if (app.filebrowser.source == FB_SOURCE_LOCAL &&
                            (on_tabbar || on_termarea)) {
                            i32 ni = app_new_filebrowser_tab(&app, app.filebrowser.cwd);
                            if (ni >= 0) {
                                app_switch_tab(&app, ni);
                                /* Close the docked sidebar so the user's
                                 * "detach" gesture feels final. */
                                app.sidebar_visible = false;
                                app.sidebar_width = 0;
                                app.filebrowser.open = false;
                                fb_close_viewer(&app.filebrowser);
                                i32 _w,_h,_fw,_fh;
                                platform_window_get_size(window,&_w,&_h);
                                platform_window_get_framebuffer_size(window,&_fw,&_fh);
                                app_update_layout(&app,_w,_h,_fw,_fh);
                                break;
                            }
                        }
                    }
                }

                /* If macOS already took over the drag, the mouseUp we see is
                 * Cocoa notifying us that the OS drag session ended. Clear
                 * state and skip the in-app drop path. */
                if (app.fb_os_drag_handed_off) {
                    app.fb_os_drag_handed_off = false;
                    app.fb_drag_src_entry     = -1;
                    app.fb_drag_active        = false;
                    break;
                }

                /* Sidebar drag resolution. If a drag was armed and:
                 *   - stayed below threshold → treat as single click (open)
                 *   - was active and released on a folder row → move
                 *   - was active and released over the terminal → insert
                 *     the path into the terminal input (quoted) */
                if (app.fb_drag_src_entry >= 0) {
                    i32 src_idx = app.fb_drag_src_entry;
                    bool was_drag = app.fb_drag_active;
                    f32 mx = (f32)event.mouse.x;
                    f32 my = (f32)event.mouse.y;

                    /* Clear state up front so nested ops don't re-enter drag. */
                    app.fb_drag_src_entry = -1;
                    app.fb_drag_active    = false;

                    if (!was_drag) {
                        /* Single click commits: open the entry. */
                        if (src_idx >= 0 && src_idx < app.filebrowser.entry_count) {
                            bool was_graph = (app.filebrowser.view_mode == FVIEW_GRAPH);
                            bool is_dir = app.filebrowser.entries[src_idx].is_dir;
                            bool opened = fb_open_file(&app.filebrowser, src_idx);
                            if (was_graph && is_dir) {
                                fb_graph_rescope(&app.filebrowser, app.filebrowser.cwd);
                            } else if (was_graph && opened && app.filebrowser.graph &&
                                       app.filebrowser.view_mode != FVIEW_GRAPH) {
                                /* Opened a note from the graph: keep it alive so
                                 * closing the note returns to the graph. */
                                app.filebrowser.graph_return = true;
                            }
                        }
                    } else {
                        HitResult drop = ui_hit_test(&app, mx, my);

                        /* SFTP entries can't seed an FB-tab yet (the FB-tab
                         * doesn't know how to navigate over SFTP). For those
                         * we still want the existing paste-path behavior. */
                        bool entry_local = (app.filebrowser.source == FB_SOURCE_LOCAL);

                        /* Tab-bar drop (any kind) → if it's a directory and
                         * a local entry, spawn a new FB-tab rooted at it. */
                        f32 tb_h_drop = (app.config.show_tab_bar || app.config.show_toolbar_icons)
                                        ? (TOOLBAR_HEIGHT_PT * app.dpi_scale) : 0.0f;
                        bool over_tab_bar_drop = (my < tb_h_drop);

                        if (entry_local && over_tab_bar_drop &&
                            src_idx >= 0 && src_idx < app.filebrowser.entry_count &&
                            app.filebrowser.entries[src_idx].is_dir) {
                            FileEntry *se = &app.filebrowser.entries[src_idx];
                            char path[FB_MAX_PATH * 2];
                            fb_entry_path(&app.filebrowser, se, path, sizeof(path));
                            i32 ni = app_new_filebrowser_tab(&app, path);
                            if (ni >= 0) {
                                app_switch_tab(&app, ni);
                                i32 _w,_h,_fw,_fh;
                                platform_window_get_size(window,&_w,&_h);
                                platform_window_get_framebuffer_size(window,&_fw,&_fh);
                                app_update_layout(&app,_w,_h,_fw,_fh);
                            }
                        }
                        /* Terminal-area drop on a directory (local) →
                         * also spawns a new FB-tab. The historical paste
                         * behavior is preserved for files only. */
                        else if (entry_local && drop.type == HIT_TERMINAL &&
                                 src_idx >= 0 && src_idx < app.filebrowser.entry_count &&
                                 app.filebrowser.entries[src_idx].is_dir) {
                            FileEntry *se = &app.filebrowser.entries[src_idx];
                            char path[FB_MAX_PATH * 2];
                            fb_entry_path(&app.filebrowser, se, path, sizeof(path));
                            i32 ni = app_new_filebrowser_tab(&app, path);
                            if (ni >= 0) {
                                app_switch_tab(&app, ni);
                                i32 _w,_h,_fw,_fh;
                                platform_window_get_size(window,&_w,&_h);
                                platform_window_get_framebuffer_size(window,&_fw,&_fh);
                                app_update_layout(&app,_w,_h,_fw,_fh);
                            }
                        }
                        else if (drop.type == HIT_TERMINAL) {
                            /* Paste the entry's path into the terminal input
                             * (single-quoted). Works for local and SFTP. */
                            if (src_idx >= 0 && src_idx < app.filebrowser.entry_count) {
                                FileEntry *se = &app.filebrowser.entries[src_idx];
                                char path[FB_MAX_PATH * 2];
                                fb_entry_path(&app.filebrowser, se, path, sizeof(path));
                                char quoted[FB_MAX_PATH * 2 + 4];
                                snprintf(quoted, sizeof(quoted), " '%s' ", path);
                                Session *s = app_focused_session(&app);
                                if (s) session_write(s, (const u8 *)quoted, strlen(quoted));
                            }
                        } else if (drop.type == HIT_SIDEBAR_ITEM &&
                                   drop.index != src_idx &&
                                   drop.index >= 0 &&
                                   drop.index < app.filebrowser.entry_count) {
                            /* Drop on a folder row: move the source into it.
                             * Cross-source moves aren't supported here (too
                             * much state to marshal) — use the context menu's
                             * Cut/Paste combo instead. */
                            FileEntry *de = &app.filebrowser.entries[drop.index];
                            if (de->is_dir) {
                                FileEntry *se = &app.filebrowser.entries[src_idx];
                                char src_path[FB_MAX_PATH * 2];
                                char dst_path[FB_MAX_PATH * 2];
                                fb_entry_path(&app.filebrowser, se, src_path, sizeof(src_path));
                                fb_entry_path(&app.filebrowser, de, dst_path, sizeof(dst_path));
                                size_t dlen = strlen(dst_path);
                                if (dlen + 1 + strlen(se->name) < sizeof(dst_path)) {
                                    if (dst_path[dlen-1] != '/') { dst_path[dlen++] = '/'; dst_path[dlen] = '\0'; }
                                    snprintf(dst_path + dlen, sizeof(dst_path) - dlen, "%s", se->name);

                                    bool ok = false;
                                    if (app.filebrowser.source == FB_SOURCE_SFTP &&
                                        app.filebrowser.sftp_handle) {
                                        Session *owner = (Session *)app.filebrowser.session;
                                        session_sftp_scope_begin(owner);
                                        ok = libssh2_sftp_rename((LIBSSH2_SFTP *)app.filebrowser.sftp_handle,
                                                                 src_path, dst_path) == 0;
                                        session_sftp_scope_end(owner);
                                    } else {
                                        ok = rename(src_path, dst_path) == 0;
                                    }
                                    if (ok) {
                                        fb_refresh(&app.filebrowser);
                                        app_fb_set_status(&app, "Moved");
                                    } else {
                                        app_fb_set_status(&app, "Move failed");
                                    }
                                }
                            }
                        }
                    }
                    /* Drag handled — consume the mouse-up so the terminal
                     * mouse-release code below doesn't fire a spurious
                     * button-release over the alternate-screen session that the
                     * cursor happened to be hovering. */
                    break;
                }

                /* Tab drag: drop or cancel */
                if (app.tab_dragging) {
                    if (app.tab_drag_target_group >= 0 &&
                        app.tab_drag_target_group < MAX_TAB_GROUPS &&
                        app.tab_groups[app.tab_drag_target_group].used &&
                        app.tab_drag_index >= 0 &&
                        app.tab_drag_index < app.tab_count) {
                        /* Drop onto a workspace chip → assign tab to that group.
                         * Wakes the tab if the group is currently expanded so the
                         * user sees it land in place. */
                        app_set_tab_group(&app, app.tab_drag_index,
                                          app.tab_drag_target_group);
                        if (!app.tab_groups[app.tab_drag_target_group].collapsed) {
                            app_wake_tab(&app, app.tab_drag_index);
                        }
                    } else if (app.tab_drag_into_split && app.tab_drag_split_zone != 0) {
                        /* The drag click already switched active_tab to the dragged
                         * tab for visual feedback; app_split_tab_from_drag() uses
                         * app->active_tab as the split destination and bails out
                         * when dst == src. Restore the pre-drag active tab so the
                         * drop has a distinct destination. */
                        if (app.tab_drag_prev_active >= 0 &&
                            app.tab_drag_prev_active < app.tab_count &&
                            app.tab_drag_prev_active != app.tab_drag_index) {
                            app_switch_tab(&app, app.tab_drag_prev_active);
                        }
                        app_split_tab_from_drag(&app, app.tab_drag_index,
                                                app.tab_drag_split_zone);
                    } else if (app.tab_drag_target >= 0 &&
                               app.tab_drag_target != app.tab_drag_index &&
                               app.tab_drag_index >= 0 &&
                               app.tab_drag_index < app.tab_count &&
                               app.tab_drag_target < app.tab_count) {
                        /* Cross-group drop: tab inherits the target tab's group
                         * so reordering through another group's section moves
                         * the tab into that group (Chrome semantics). */
                        i32 src_gi = app.tabs[app.tab_drag_index].group_index;
                        i32 dst_gi = app.tabs[app.tab_drag_target].group_index;
                        if (src_gi != dst_gi) {
                            if (dst_gi >= 0 && dst_gi < MAX_TAB_GROUPS &&
                                app.tab_groups[dst_gi].used) {
                                app_set_tab_group(&app, app.tab_drag_index, dst_gi);
                            } else {
                                app_remove_tab_from_group(&app, app.tab_drag_index);
                            }
                        }
                        app_move_tab(&app, app.tab_drag_index, app.tab_drag_target);
                    }
                    app.tab_dragging          = false;
                    app.tab_drag_pending      = false;
                    app.tab_drag_index        = -1;
                    app.tab_drag_target       = -1;
                    app.tab_drag_target_group = -1;
                    app.tab_drag_offset_x     = 0;
                    app.tab_drag_into_split   = false;
                    app.tab_drag_split_zone   = 0;
                    break;
                }
                if (app.tab_drag_pending) {
                    /* Drag threshold was never exceeded -- just a click (tab already switched) */
                    app.tab_drag_pending    = false;
                    app.tab_drag_index      = -1;
                    app.tab_drag_into_split = false;
                    app.tab_drag_split_zone = 0;
                }

                /* Markdown viewer detach drop: title drag onto the tab bar
                 * opens the same .md file in a dedicated FileBrowser tab
                 * with the viewer already visible. */
                if (app.viewer_dragging) {
                    bool show_top = app.config.show_tab_bar || app.config.show_toolbar_icons;
                    f32 tb_h = show_top ? (TOOLBAR_HEIGHT_PT * app.dpi_scale) : 0.0f;
                    bool dropped_on_tabs = (app.hover_y < tb_h + 6.0f * app.dpi_scale);
                    if (dropped_on_tabs && app.viewer_drag_path[0]) {
                        FileBrowser *src_fb = app.viewer_drag_source;
                        i32 ni = app_open_markdown_viewer_tab(&app, app.viewer_drag_path);
                        if (ni >= 0 && src_fb && src_fb->view_path[0] &&
                            strcmp(src_fb->view_path, app.viewer_drag_path) == 0) {
                            fb_close_viewer(src_fb);
                        }
                    }
                    app.viewer_dragging = false;
                }
                app.viewer_drag_pending = false;
                app.viewer_drag_source = NULL;
                app.viewer_drag_path[0] = '\0';

                /* Finalize selection and copy */
                if (mouse_selecting) {
                    mouse_selecting = false;
                    Terminal *st = app_focused_terminal(&app);
                    if (st && selection_active(st)) {
                        char *sel = selection_get_text(st);
                        if (sel) {
                            if (sel[0]) {
                                platform_clipboard_set(sel);
                                /* Right-side toast only when the user opted
                                 * into copy-on-select; otherwise the drag
                                 * still populates the clipboard quietly
                                 * (preserves the existing behaviour). */
                                if (app.config.copy_on_select)
                                    app_show_toast(&app, "Copied to clipboard");
                            }
                            free(sel);
                        }
                    }
                }
                /* A press-and-release without any drag never promoted to an
                 * active selection — drop the pending state silently. */
                mouse_selecting_pending = false;

                /* Send mouse release to terminal if mouse tracking active */
                Tab *mu_tab = app_active_tab(&app);
                if (mu_tab && mu_tab->terminal &&
                    (mu_tab->terminal->mode & (MODE_MOUSE_BTN | MODE_MOUSE_MOTION))) {
                    f32 mu_x = (f32)event.mouse.x;
                    f32 mu_y = (f32)event.mouse.y;
                    HitResult mu_hit = ui_hit_test(&app, mu_x, mu_y);
                    if (mu_hit.type == HIT_TERMINAL && mu_hit.index >= 0) {
                        u8 mbuf[32];
                        Terminal *mt = app_focused_terminal(&app);
                        Session *ms = app_focused_session(&app);
                        if (mt && ms) {
                            i32 mn = terminal_mouse_encode(mt, 0, mu_hit.index, mu_hit.sub_index,
                                                            false, 0, mbuf, sizeof(mbuf));
                            if (mn > 0) session_write(ms, mbuf, mn);
                        }
                    }
                }
                break;
            }
            case EVENT_SCROLL: {
                f32 mx = app.hover_x;
                f32 my = app.hover_y;
                HitResult scroll_hit = ui_hit_test(&app, mx, my);

                /* Command palette: wheel → list scroll.
                 *
                 * macOS trackpad fires precise=true events at ~120 Hz with
                 * sub-pixel deltas (dy ≈ 0.3 — 2.0 each). Truncating each
                 * to an integer row tick and applying it raw makes a
                 * gentle two-finger swipe instantly fire 60+ row jumps —
                 * the list teleports to the end, which is what looks like
                 * "scroll doesn't work" from the user's side. We
                 * accumulate the pixel delta and only emit a row tick
                 * every ~24 px (a nominal row-height). For non-precise
                 * mouse wheels (line-unit deltas) we fall through to the
                 * legacy 1-tick-per-event behaviour. */
                if (app.palette_active) {
                    static f32 sb_accum = 0.0f;
                    i32 ticks = 0;
                    if (event.scroll.precise) {
                        sb_accum += event.scroll.dy;
                        const f32 line_px = 24.0f * app.dpi_scale;
                        while (sb_accum >=  line_px) { ticks--; sb_accum -= line_px; }
                        while (sb_accum <= -line_px) { ticks++; sb_accum += line_px; }
                    } else {
                        ticks = -(i32)event.scroll.dy;
                        if (ticks == 0 && event.scroll.dy != 0)
                            ticks = event.scroll.dy > 0 ? -1 : 1;
                        sb_accum = 0.0f;
                    }
                    if (ticks != 0) {
                        app.palette_scroll += ticks;
                        if (app.palette_scroll < 0) app.palette_scroll = 0;
                        /* Upper clamp lives in the renderer (it knows
                         * max_visible); we accept transient overshoot
                         * for the one frame in between. */
                    }
                    needs_redraw = true;
                    break;
                }

                /* Transcript viewer: wheel → event-index scroll.
                 *
                 * Same trackpad-vs-mousewheel split as the command palette
                 * above. macOS trackpad fires precise=true events at
                 * ~120 Hz with sub-pixel deltas; treating each raw dy as a
                 * message index made one casual two-finger swipe jump 40+
                 * messages, which is what "scroll is way too fast" looked
                 * like. Accumulate pixel deltas and only emit one event
                 * tick every ~40 px (roughly half a message's visual
                 * height); for line-unit mouse wheels (non-precise), keep
                 * the legacy 1-tick-per-event behaviour. */
                if (app.transcript_viewer_active) {
                    static f32 tv_accum = 0.0f;
                    i32 dl = 0;
                    if (event.scroll.precise) {
                        tv_accum += event.scroll.dy;
                        const f32 step_px = 40.0f * app.dpi_scale;
                        while (tv_accum >=  step_px) { dl--; tv_accum -= step_px; }
                        while (tv_accum <= -step_px) { dl++; tv_accum += step_px; }
                    } else {
                        dl = -(i32)event.scroll.dy;
                        if (dl == 0 && event.scroll.dy != 0)
                            dl = event.scroll.dy > 0 ? -1 : 1;
                        tv_accum = 0.0f;
                    }
                    if (dl != 0) {
                        app.transcript_scroll += dl;
                        if (app.transcript_scroll < 0) app.transcript_scroll = 0;
                        if (app.transcript_scroll > app.transcript_count)
                            app.transcript_scroll = app.transcript_count;
                    }
                    needs_redraw = true;
                    break;
                }

                /* (Cmd+Scroll → opacity gesture removed by request — opacity is
                 * adjusted only via Settings; no scroll/drag shortcut.) */

                /* Tab strip: wheel / trackpad over the toolbar → horizontal
                 * scroll of the tab row. Consumed whenever the cursor is over
                 * the tab bar (so it never leaks to the terminal beneath), but
                 * only moves when the strip actually overflows. Prefer the
                 * horizontal delta; fall back to the vertical wheel for mice
                 * without an h-wheel. Pixel (precise) deltas feed straight in;
                 * line deltas get a per-notch step. Sign matches the palette /
                 * file-viewer convention (positive delta → reveal earlier). */
                if (app.tab_bar_height > 0.0f &&
                    my >= 0.0f && my < app.tab_bar_height) {
                    if (app._tab_scroll_max > 0.0f) {
                        f32 d  = event.scroll.dx != 0.0f ? event.scroll.dx
                                                         : event.scroll.dy;
                        f32 px = event.scroll.precise ? d
                                                      : d * (40.0f * app.dpi_scale);
                        app._tab_scroll_x -= px;
                        if (app._tab_scroll_x < 0.0f) app._tab_scroll_x = 0.0f;
                        if (app._tab_scroll_x > app._tab_scroll_max)
                            app._tab_scroll_x = app._tab_scroll_max;
                        needs_redraw = true;
                    }
                    break;
                }

                /* File viewer scroll — pixel-based, OS-native feel.
                 *
                 * macOS NSEvent.scrollingDeltaY is already in the right unit:
                 * pixels for trackpad/Magic Mouse (precise=true), line units
                 * for legacy mouse wheel (precise=false). We just feed it
                 * straight into a pixel offset, multiplying by line height
                 * for non-precise wheels so one click ≈ one line. This
                 * matches every native macOS scroll view and inherits the
                 * user's System Settings → Trackpad → Scroll Speed naturally. */
                if (scroll_hit.type == HIT_VIEWER ||
                    scroll_hit.type == HIT_VIEWER_TITLE) {
                    bool over_panel = fb_graph_active(&app.filebrowser) &&
                        fb_graph_panel_hit(&app.filebrowser, app.hover_x, app.hover_y);
                    if (over_panel) break;   /* don't zoom under the controls panel */
                    if (app_scroll_file_viewer(&app.filebrowser, event.scroll.dy,
                                               event.scroll.precise, app.dpi_scale)) {
                        needs_redraw = true;
                        break;
                    }
                }

                if (scroll_hit.type == HIT_FBTAB_VIEWER ||
                    scroll_hit.type == HIT_FBTAB_VIEWER_TITLE) {
                    Tab *atab = app_active_tab(&app);
                    bool over_panel = atab && atab->kind == TAB_FILEBROWSER && atab->fb &&
                        fb_graph_active(atab->fb) &&
                        fb_graph_panel_hit(atab->fb, app.hover_x, app.hover_y);
                    if (!over_panel && atab && atab->kind == TAB_FILEBROWSER && atab->fb &&
                        app_scroll_file_viewer(atab->fb, event.scroll.dy,
                                               event.scroll.precise,
                                               app.dpi_scale)) {
                        needs_redraw = true;
                    }
                    break;
                }

                /* File-browser tab scroll — applies when the active tab
                 * is a FB tab AND the cursor is over the list area (not
                 * the viewer pane). */
                {
                    Tab *atab = app_active_tab(&app);
                    if (atab && atab->kind == TAB_FILEBROWSER && atab->fb &&
                        !atab->fb_viewer_only) {
                        f32 ox = (app.sidebar_visible ? app.sidebar_width : 0) + app.padding;
                        f32 oy = app.tab_bar_height + app.padding +
                                 app.config.style.terminal_top_gap * app.dpi_scale;
                        f32 total_w = (f32)app.fb_width - ox - app.padding;
                        f32 total_h = (f32)app.fb_height - oy -
                                      app.status_bar_height - app.padding;
                        bool has_viewer = (atab->fb->view_mode != FVIEW_NONE);
                        f32 list_w = total_w;
                        if (has_viewer) {
                            f32 vmin = VIEWER_MIN_PT * app.dpi_scale;
                            f32 list_min = 200.0f * app.dpi_scale;
                            f32 ratio = atab->fb_viewer_ratio;
                            if (ratio <= 0.0f) ratio = VIEWER_WIDTH_RATIO;
                            f32 view_w = total_w * ratio;
                            if (view_w < vmin) view_w = vmin;
                            if (total_w - view_w < list_min) view_w = total_w - list_min;
                            if (view_w < vmin) view_w = vmin;
                            list_w = total_w - view_w;
                        }
                        if (app.hover_x >= ox && app.hover_x < ox + list_w &&
                            app.hover_y >= oy && app.hover_y < oy + total_h) {
                            fb_handle_scroll(atab->fb, event.scroll.dy, event.scroll.precise);
                            needs_redraw = true;
                            break;
                        }
                    }
                }

                /* Sidebar scroll */
                if (app.sidebar_visible && mx < app.sidebar_width) {
                    fb_handle_scroll(&app.filebrowser, event.scroll.dy, event.scroll.precise);
                    break;
                }

                /* Terminal scroll */
                if (!tab) break;
                {
                    Terminal *st = app_focused_terminal(&app);
                    Session *ss = app_focused_session(&app);
                    if (!st) break;

                    /* Apps that enable mouse tracking (less, TUIs, agent
                     * CLIs) normally receive the wheel as mouse events — which
                     * makes Liu's own scrollback unreachable and reads as
                     * "scroll is stuck". Shift+scroll always bypasses mouse mode
                     * and drives the scrollback, matching the platform convention. */
                    bool mouse_mode = (st->mode & (MODE_MOUSE_BTN | MODE_MOUSE_MOTION)) != 0;
                    if (mouse_mode && !(event.scroll.mods & MOD_SHIFT)) {
                        /* Translate the wheel into mouse-wheel reports for the
                         * running TUI. macOS trackpads fire precise=true events
                         * at ~120 Hz with sub-pixel deltas (dy ~0.3-2.0 each);
                         * sending one wheel report per raw event floods the app
                         * (agent CLIs, terminal applications) and scrolls absurdly fast and
                         * erratically. Accumulate the pixel delta and emit one
                         * report per ~24 px (a nominal row height), matching the
                         * palette/transcript handlers above; non-precise mouse
                         * wheels send one report per notch. */
                        static f32 term_wheel_accum = 0.0f;
                        i32 reps = 0;
                        bool up = event.scroll.dy > 0;
                        if (event.scroll.precise) {
                            term_wheel_accum += event.scroll.dy;
                            const f32 STEP = 24.0f * app.dpi_scale;
                            while (term_wheel_accum >= STEP)  { reps++; up = true;  term_wheel_accum -= STEP; }
                            while (term_wheel_accum <= -STEP) { reps++; up = false; term_wheel_accum += STEP; }
                        } else {
                            reps = (i32)(event.scroll.dy > 0 ? event.scroll.dy : -event.scroll.dy);
                            if (reps < 1) reps = 1;
                        }
                        for (i32 i = 0; i < reps; i++) {
                            u8 sbuf[32];
                            i32 sn = terminal_mouse_scroll(st, st->cols/2, st->rows/2, up, 0, sbuf, sizeof(sbuf));
                            if (sn > 0 && ss) app_send_input(&app, ss, sbuf, sn);
                        }
                    } else {
                        /* Liu's own scrollback. Same trackpad-flooding caveat:
                         * accumulate precise pixel deltas to ~one row per 24 px
                         * instead of 3 rows per 120 Hz event. Non-precise mouse
                         * wheels keep the 3-rows-per-notch feel. */
                        static f32 term_sb_accum = 0.0f;
                        i32 ticks = 0;
                        if (event.scroll.precise) {
                            term_sb_accum += event.scroll.dy;
                            const f32 STEP = 24.0f * app.dpi_scale;
                            while (term_sb_accum >= STEP)  { ticks++; term_sb_accum -= STEP; }
                            while (term_sb_accum <= -STEP) { ticks--; term_sb_accum += STEP; }
                        } else {
                            ticks = (i32)event.scroll.dy * 3;
                            if (ticks == 0 && event.scroll.dy != 0)
                                ticks = event.scroll.dy > 0 ? 3 : -3;
                        }
                        if (ticks > 0) terminal_scroll_up(st, ticks);
                        else if (ticks < 0) terminal_scroll_down(st, -ticks);
                        /* Update scrollbar auto-hide timer */
                        if (ticks != 0) app.scrollbar_last_activity = platform_time_sec();
                    }
                }
                break;
            }
            case EVENT_RESIZE: {
                i32 w, h, fb_w, fb_h;
                platform_window_get_size(window, &w, &h);
                platform_window_get_framebuffer_size(window, &fb_w, &fb_h);
                app_update_layout(&app, w, h, fb_w, fb_h);
                break;
            }
            case EVENT_CLOSE:
                goto cleanup;

            case EVENT_FOCUS:
            case EVENT_BLUR: {
                /* Track window focus state for bell notifications */
                app.window_focused = (event.type == EVENT_FOCUS);
                if (event.type == EVENT_BLUR) {
                    app.translate_chord_down = false;
                    app_translate_clear_pending_chord(&app);
                    /* Cancel any in-flight pane layout drag — the user
                     * left the window without a MOUSE_UP we can act on
                     * (the up landed in another app), so don't leave
                     * the drag overlay armed on return. */
                    if (app.pane_drag_pending || app.pane_drag_active) {
                        app.pane_drag_pending  = false;
                        app.pane_drag_active   = false;
                        app.pane_drag_tab_index = -1;
                        app.pane_drag_src_pane  = -1;
                        app.pane_drag_hover_pane = -1;
                        app.pane_drag_drop_zone = 0;
                    }
                }
                /* Clear dock badge and bell indicators when app gains focus */
                if (app.window_focused) {
                    platform_set_dock_badge(NULL);
                    if (app.active_tab >= 0 && app.active_tab < app.tab_count)
                        app.tabs[app.active_tab].has_bell = false;
                }
                /* Send focus events to terminal if mode 1004 enabled */
                Terminal *ft3 = app_focused_terminal(&app);
                Session *fs3 = app_focused_session(&app);
                if (ft3 && fs3 && (ft3->mode & MODE_FOCUS_EVENT)) {
                    const char *seq = (event.type == EVENT_FOCUS) ? "\x1b[I" : "\x1b[O";
                    /* Write directly, not via app_send_input: focus events are
                     * protocol responses, not user input. Routing them through
                     * the input funnel would snap scrollback to the bottom on
                     * every Cmd+Tab (defeating no-snap-on-output) and broadcast
                     * the sequence to other panes in broadcast mode. */
                    session_write(fs3, (const u8 *)seq, 3);
                }
                /* Quake mode: hide window on focus loss */
                if (event.type == EVENT_BLUR && platform_is_quake_mode(window)) {
                    platform_toggle_quake_window(window);
                }
                break;
            }

            /* ---- Menu events ---- */
            case EVENT_MENU_NEW_WINDOW:
                /* Cmd+N: in the focused file browser this means "new file";
                 * everywhere else keep the historical new-tab behaviour.
                 * sidebar_focused is sticky (only cleared on mouse-down), so we
                 * must also yield while any text-input overlay is open or the
                 * new-file prompt would be raised hidden behind it. Mirrors the
                 * keyboard fb-block guard (main.c) plus the settings overlay. */
                if (app.sidebar_focused && app.filebrowser.open &&
                    !app.filebrowser.editor_mode && !app.fb_prompt_active &&
                    !app.fb_ctx_menu_active && !app.palette_active &&
                    !app.search_active && !app.create_theme_active &&
                    !app.ssh_dialog_active && !app.kbi_dialog_active &&
                    !app.settings.open) {
                    app_fb_begin_new_file(&app);
                    break;
                }
                /* fall through */
            case EVENT_MENU_NEW_TAB: {
                Session *s = session_create_local(app.grid_cols, app.grid_rows);
                if (s && session_status(s) == SESSION_CONNECTED) {
                    app_new_tab(&app, s, "Terminal");
                    i32 w2, h2, fw2, fh2;
                    platform_window_get_size(window, &w2, &h2);
                    platform_window_get_framebuffer_size(window, &fw2, &fh2);
                    app_update_layout(&app, w2, h2, fw2, fh2);
                }
                break;
            }
            case EVENT_MENU_CLOSE_TAB:
                if (app.tab_count > 0) {
                    if (app_confirm_close_arm(&app, app.active_tab, -1, -1))
                        break;
                    app_close_tab(&app, app.active_tab);
                    if (!app_respawn_on_empty(&app) && app.tab_count == 0)
                        goto cleanup;
                }
                break;
            case EVENT_MENU_TOGGLE_SIDEBAR:
                app.sidebar_visible = !app.sidebar_visible;
                app.sidebar_width = app.sidebar_visible ? SIDEBAR_DEFAULT_PT * app.dpi_scale : 0;
                if (!app.sidebar_visible) {
                    app.filebrowser.open = false;
                    fb_close_viewer(&app.filebrowser);
                }
                { i32 w2, h2, fw2, fh2;
                  platform_window_get_size(window, &w2, &h2);
                  platform_window_get_framebuffer_size(window, &fw2, &fh2);
                  app_update_layout(&app, w2, h2, fw2, fh2); }
                break;
            case EVENT_MENU_FONT_BIGGER:
                app.config.font_size += 1.0f;
                app_reload_font(&app, window);
                break;
            case EVENT_MENU_FONT_SMALLER:
                if (app.config.font_size > 6.0f) {
                    app.config.font_size -= 1.0f;
                    app_reload_font(&app, window);
                }
                break;
            case EVENT_MENU_FONT_RESET:
                app.config.font_size = 12.0f;
                app_reload_font(&app, window);
                break;
            case EVENT_MENU_THEME: {
                const Theme *new_theme = theme_get_by_name(event.theme.name);
                if (new_theme) app_switch_theme(&app, new_theme);
                break;
            }
            case EVENT_MENU_SETTINGS:
                settings_toggle(&app.settings);
                break;
            case EVENT_MENU_SSH_CONNECT:
                app_reset_ssh_dialog(&app);
                break;
            case EVENT_MENU_FIND: {
                FileBrowser *ffb = app_find_fb(&app);
                if (ffb) { fb_find_open(ffb, false); break; }
                app.search_active = true;
                app.search_query[0] = '\0';
                app.search_query_len = 0;
                break;
            }
            case EVENT_MENU_IMPORT_SSH:
                app_import_default_ssh_config(&app);
                break;
            case EVENT_MENU_SSH_KEYS:
                app_open_key_manager(&app);
                break;

            case EVENT_QUAKE_TOGGLE:
                if (platform_is_quake_mode(window)) {
                    platform_toggle_quake_window(window);
                }
                break;

            case EVENT_IME_COMPOSE:
                if (event.ime.text && event.ime.text[0]) {
                    snprintf(app.ime_preedit, sizeof(app.ime_preedit), "%s", event.ime.text);
                    app.ime_cursor = event.ime.cursor;
                    app.ime_active = true;
                } else {
                    app.ime_preedit[0] = '\0';
                    app.ime_cursor = 0;
                    app.ime_active = false;
                }
                break;

            /* ---- Drag & drop file upload ---- */
            case EVENT_DRAG_ENTER:
                app.drag_over_active = true;
                /* macOS doesn't fire mouse-move during a drag, so the dragging
                 * location is the only fresh pointer position — keep hover in
                 * sync so a drop can target the pane under the cursor. */
                app.hover_x = (f32)event.drag.x;
                app.hover_y = (f32)event.drag.y;
                break;
            case EVENT_DRAG_EXIT:
                app.drag_over_active = false;
                break;
            case EVENT_DROP_FILE: {
                app.drag_over_active = false;
                if (!event.drop.path) break;

                /* Drop position in framebuffer px. macOS now carries the
                 * dragging location on the drop; fall back to the last hover
                 * (kept in sync from DRAG_ENTER) when both are zero — the only
                 * case where it matters is the (0,0) corner, which resolves to
                 * no pane and falls back to the focused one anyway. */
                f32 drop_fx = (f32)event.drop.x;
                f32 drop_fy = (f32)event.drop.y;
                if (event.drop.x == 0 && event.drop.y == 0) {
                    drop_fx = app.hover_x;
                    drop_fy = app.hover_y;
                }

                /* External Finder drag — if it's a directory and the drop
                 * lands on the tab bar (or active tab is already an FB tab,
                 * or there's no live SSH session to upload to), spawn a
                 * new FB-tab rooted at it. */
                {
                    struct stat _st;
                    if (stat(event.drop.path, &_st) == 0 && S_ISDIR(_st.st_mode)) {
                        f32 _tb_h = (app.config.show_tab_bar || app.config.show_toolbar_icons)
                                    ? (TOOLBAR_HEIGHT_PT * app.dpi_scale) : 0.0f;
                        bool _on_tabbar = (drop_fy < _tb_h);
                        if (_on_tabbar) {
                            i32 ni = app_new_filebrowser_tab(&app, event.drop.path);
                            if (ni >= 0) {
                                app_switch_tab(&app, ni);
                                i32 _w,_h,_fw,_fh;
                                platform_window_get_size(window,&_w,&_h);
                                platform_window_get_framebuffer_size(window,&_fw,&_fh);
                                app_update_layout(&app,_w,_h,_fw,_fh);
                            }
                            break;
                        }
                    }
                }

                /* Target the pane under the drop point, not whichever pane
                 * happens to be focused. In a split, the pane that received the
                 * drop also takes focus so a follow-up Enter lands there. */
                Session  *drop_session = NULL;
                Terminal *drop_term    = NULL;
                {
                    Tab *atab = app_active_tab(&app);
                    if (atab) {
                        i32 dpane = app_pane_index_at(&app, atab, drop_fx, drop_fy);
                        if (dpane >= 0) {
                            drop_session = app_pane_session(atab, dpane);
                            drop_term    = app_pane_terminal(atab, dpane);
                            if (drop_session && atab->split != SPLIT_NONE)
                                atab->active_pane = dpane;
                        }
                    }
                }
                if (!drop_session) drop_session = app_focused_session(&app);
                if (!drop_term)    drop_term    = app_focused_terminal(&app);
                if (!drop_session) break;

                /* Local terminal (e.g. an agent CLI like Claude Code / Codex):
                 * a drop types the file path into the prompt — the standard
                 * the platform behavior agents rely on to attach a
                 * dragged image. No SFTP, no newline; the user hits Enter. */
                if (session_type(drop_session) != SESSION_SSH) {
                    Terminal *drop_term_l = drop_term;
                    if (drop_term_l) {
                        char esc[2048];
                        /* Detect agent CLI context — agent TUIs parse their own
                         * input and don't understand shell escape sequences
                         * (backslash before spaces, parens, etc.). Sending an
                         * escaped path like "/path/My\ File.png" means the CLI
                         * tries to open a file with a literal backslash in the
                         * name, fails, and shows the raw path text instead of
                         * the expected attachment chip like "[Image #1]". */
                        bool is_agent = false;
                        if (drop_session) {
                            const char *fg = session_fg_process(drop_session);
                            if (fg && fg[0] && agent_id_for_basename(fg)[0])
                                is_agent = true;
                        }
                        usize en;
                        if (is_agent) {
                            en = (usize)snprintf(esc, sizeof esc, "%s ",
                                                 event.drop.path);
                            if (en >= sizeof esc) en = sizeof esc - 1;
                        } else {
                            en = drop_path_escape(esc, sizeof esc,
                                                  event.drop.path);
                        }
                        if (en > 0) {
                            app_send_input(&app, drop_session,
                                           (const u8 *)esc, (i32)en);
                            /* Mirror the path into the translate input shadow,
                             * like typed keys (see EVENT_CHAR). Without this the
                             * shadow misses the path: Translate-on-Tab then
                             * computes too few backspaces (erasing the prompt
                             * mid-path) and the translation prompt never sees
                             * the attachment. The segmenter keeps the path
                             * verbatim when the line is translated. */
                            if (!app.translate_active) {
                                app_translate_shadow_append_bytes(
                                    &app, drop_term_l, drop_session,
                                    (const u8 *)esc, (i32)en);
                            }
                        }
                    }
                    break;
                }

                /* Remote SSH session: upload the file over SFTP instead. */
                void *sftp_handle = session_get_sftp(drop_session);
                if (!sftp_handle) break;

                /* Determine remote directory: use terminal CWD (from OSC 7) or home */
                const char *remote_dir = "/";
                if (drop_term && drop_term->cwd && drop_term->cwd[0])
                    remote_dir = drop_term->cwd;

                /* Spawn background upload thread */
                SFTPUploadTask *task = calloc(1, sizeof(SFTPUploadTask));
                if (task) {
                    task->session = drop_session;
                    snprintf(task->local_path, sizeof(task->local_path), "%s", event.drop.path);
                    snprintf(task->remote_dir, sizeof(task->remote_dir), "%s", remote_dir);
                    pthread_t tid;
                    if (pthread_create(&tid, NULL, sftp_upload_thread, task) == 0) {
                        pthread_detach(tid);
                    } else {
                        free(task);
                    }
                }
                break;
            }

            /* ---- DPI change (multi-monitor) ---- */
            case EVENT_DPI_CHANGE: {
                app_handle_dpi_change(&app, window, event.dpi.scale);
                break;
            }

            default:
                break;
            }
        }

        /* Poll session I/O — but only if a watched fd actually fired since
         * last drain (or a UI event came through, which can dirty terminal
         * state via key input that needs an echo flush). On idle frames
         * (cursor-blink edge wake-ups, time-based tickles) every session
         * fd is by definition empty, and the per-session session_read +
         * forward_poll + x11_poll syscalls were a measurable jitter floor. */
        i32 fd_fires = platform_take_fd_fire_count();
        bool needs_session_poll = (fd_fires > 0) || had_event;
        if (needs_session_poll) {
            had_session_data_last_frame = app_poll_sessions(&app);
        } else {
            had_session_data_last_frame = false;
            app.visible_session_data_this_frame = false;
        }

        /* Pump background dev-server sites (Sites panel): drain their output
         * into per-site log terminals and detect run→exit transitions. Runs
         * every frame because a site can exit or print while the UI is idle;
         * it's a cheap early-out when no sites are running. */
        if (sites_poll(&app.site_mgr) && app.sites.active) needs_redraw = true;

        /* Sync file browser with the active tab's PTY cwd. Cheap (one
         * stat / proc query at most every 200 ms) and a no-op when the
         * browser is closed or pointed at a remote source. */
        app_sync_filebrowser_cwd(&app, platform_time_sec());
        /* Drive the Create Theme generation pipeline regardless of
         * session activity — the agent runs in its own process and
         * progress shows up via stdout reads + stat() polling. */
        app_tick_create_theme(&app);
        app_tick_translate(&app);
        /* Pump the Settings model-catalog fetches (models.dev /
         * `grok models` / API /v1/models children). No-op while idle. */
        model_catalog_tick();

        /* Pending Agent-History resume: a previous click sent SIGTERM to
         * the focused terminal's foreground agent and queued the new
         * shell command. Fire it as soon as the shell is back in the FG
         * pgrp, or when the deadline expires (best-effort fallback). */
        if (app.pending_agent_active) {
            Session *ps = app.pending_agent_session;
            /* Validate ps via tabs[] before dereferencing — if the user
             * closed the tab between picker and tick, ps would dangle.
             * Lookup also yields the Terminal we need for the paste. */
            Terminal *pt = NULL;
            /* Search every pane, not just the tab's primary session — a resume
             * targeting a split pane was silently dropped because only
             * tabs[ti].session was compared. Unused pane slots are NULL and
             * never match the (non-NULL) target. */
            for (i32 ti = 0; ti < app.tab_count && ps && !pt; ti++) {
                Tab *tb = &app.tabs[ti];
                for (i32 pane = 0; pane < MAX_SPLIT_PANES; pane++) {
                    if (app_pane_session(tb, pane) == ps) {
                        pt = app_pane_terminal(tb, pane);
                        break;
                    }
                }
            }
            bool drop = (!ps || !pt || session_status(ps) != SESSION_CONNECTED);
            if (!drop) {
                bool ready = session_fg_is_shell(ps);
                bool timeout = platform_time_sec() > app.pending_agent_deadline_sec;
                if (ready || timeout) {
                    if (timeout && !ready) {
                        /* Agent didn't yield to SIGTERM within the window
                         * — escalate so the shell can take FG. Bytes
                         * already buffered in the PTY get drained by the
                         * dying process; the post-prompt shell still sees
                         * and runs the queued command. */
                        session_kill_fg(ps, SIGKILL);
                    }
                    if (app.pending_agent_command[0]) {
                        app_send_command_and_execute(&app, pt, ps,
                                                     app.pending_agent_command);
                    }
                    drop = true;
                }
            }
            if (drop) {
                app.pending_agent_active = false;
                app.pending_agent_session = NULL;
                app.pending_agent_command[0] = '\0';
            }
        }
        /* Local model download: when it finishes, record + persist the path
         * so the local backend can use it immediately (and on next launch),
         * then toast. Polled regardless of whether Settings is open, so a
         * download the user started then navigated away from still lands. */
        {
            static bool md_was_active = false;
            bool md_active = model_download_active();
            if (md_active || md_was_active) {
                ModelDownloadStatus md;
                model_download_poll(&md);
                if (md.state == MODEL_DL_DONE && md.dest_path[0] &&
                    strcmp(app.config.translate.local_model_path, md.dest_path) != 0) {
                    snprintf(app.config.translate.local_model_path,
                             sizeof app.config.translate.local_model_path,
                             "%s", md.dest_path);
                    config_save(&app.config, config_file_path());
                    app_show_toast(&app, "Model indirildi");
                }
            }
            md_was_active = md_active;
        }
        had_visible_session_data_last_frame = app.visible_session_data_this_frame;
        if (had_session_data_last_frame && app.visible_session_data_this_frame) {
            /* Only data destined for the currently visible terminal both
             * triggers a repaint and arms the 200 ms ProMotion boost.
             * Background tabs receive their bytes (terminal_feed already
             * ran above), but no pixel on screen changes — repainting at
             * 120 Hz for a hidden tab burning logs is exactly the
             * sawtooth this commit is removing. The tab bar's title /
             * activity indicator catches up on the next visible repaint
             * (at most 0.5 s away on the cursor-blink edge). */
            app_note_interaction(&app, platform_time_sec());
            needs_redraw = true;
        }

        /* Pick up any completed background paste/transfer worker so the
         * sidebar can refresh and the status toast flips to Pasted/Moved. */
        app_fb_poll_task(&app);
        /* Drive the in-app auto-updater (feed check / download / verify /
         * install). When it has installed a new build and spawned the relaunch
         * helper, quit gracefully so the helper can start the new app. */
        app_tick_updater(&app);
        if (app.updater.relaunch_requested) goto cleanup;
        /* Re-arm any read-ready watches that fired this tick. Must happen
         * AFTER the drain above — otherwise the source sees the fd still
         * has unread data (EV_DISPATCH + unread = immediate re-fire) and
         * pegs a CPU core spamming wake events. See platform.h. */
        platform_resume_watches();
        if (had_event)
            needs_redraw = true;

        /* Smart Vault auto-lock. Already cheap, but minute-resolution by
         * design: 5 s polling is more than enough and removes another
         * unconditional per-frame call from the loop. NOTE: activity is
         * tracked separately from terminal I/O on purpose — see the
         * vault.h docs on vault_touch_activity. */
        {
            static f64 next_vault_tick = 0;
            f64 vt_now = platform_time_sec();
            if (vt_now >= next_vault_tick) {
                app_vault_auto_lock_tick(&app);
                next_vault_tick = vt_now + 5.0;
            }
        }

        /* Git status for the focused local tab — throttled to ~2 Hz. */
        app_git_status_tick(&app);

        /* Check if any session needs a passphrase prompt */
        if (!app.passphrase_dialog_active) {
            for (i32 ti = 0; ti < app.tab_count; ti++) {
                Session *ps = app.tabs[ti].session;
                if (ps && session_needs_passphrase(ps)) {
                    const char *kpath = session_passphrase_key_path(ps);

                    /* Check cache first */
                    const char *cached = passphrase_cache_lookup(&app, kpath);
                    if (cached) {
                        /* Supply cached passphrase without showing dialog */
                        session_supply_passphrase(ps, cached);
                    } else {
                        /* Show passphrase dialog */
                        app.passphrase_dialog_active = true;
                        app.passphrase_session = ps;
                        snprintf(app.passphrase_key_path,
                                 sizeof(app.passphrase_key_path), "%s", kpath);
                        memset(app.passphrase_input, 0, sizeof(app.passphrase_input));
                        needs_redraw = true;
                    }
                    break;
                }
                /* Also check second pane session */
                Session *ps2 = app.tabs[ti].session2;
                if (ps2 && session_needs_passphrase(ps2)) {
                    const char *kpath = session_passphrase_key_path(ps2);
                    const char *cached = passphrase_cache_lookup(&app, kpath);
                    if (cached) {
                        session_supply_passphrase(ps2, cached);
                    } else {
                        app.passphrase_dialog_active = true;
                        app.passphrase_session = ps2;
                        snprintf(app.passphrase_key_path,
                                 sizeof(app.passphrase_key_path), "%s", kpath);
                        memset(app.passphrase_input, 0, sizeof(app.passphrase_input));
                        needs_redraw = true;
                    }
                    break;
                }
            }
        }

        /* Check if any SSH session is waiting for host key confirmation.
         * Defer while a close-confirm is up so the two modals never stack
         * (the SSH worker blocks on hostkey_cond until we respond, so the
         * prompt simply arms on the next frame after the close-confirm
         * resolves). */
        if (!app.hostkey_dialog_active && !app.close_confirm_active) {
            for (i32 i = 0; i < app.tab_count; i++) {
                Session *sess = app.tabs[i].session;
                if (sess && session_hostkey_pending(sess)) {
                    app.hostkey_dialog_active = true;
                    app.hostkey_session = sess;
                    bool is_change = false;
                    session_hostkey_get_info(sess, &is_change,
                                             app.hostkey_hostname, sizeof(app.hostkey_hostname),
                                             &app.hostkey_port,
                                             app.hostkey_old_fp, sizeof(app.hostkey_old_fp),
                                             app.hostkey_new_fp, sizeof(app.hostkey_new_fp));
                    app.hostkey_is_change = is_change;
                    needs_redraw = true;
                    break;
                }
                Session *sess2 = app.tabs[i].session2;
                if (sess2 && session_hostkey_pending(sess2)) {
                    app.hostkey_dialog_active = true;
                    app.hostkey_session = sess2;
                    bool is_change = false;
                    session_hostkey_get_info(sess2, &is_change,
                                             app.hostkey_hostname, sizeof(app.hostkey_hostname),
                                             &app.hostkey_port,
                                             app.hostkey_old_fp, sizeof(app.hostkey_old_fp),
                                             app.hostkey_new_fp, sizeof(app.hostkey_new_fp));
                    app.hostkey_is_change = is_change;
                    needs_redraw = true;
                    break;
                }
            }
        }

        /* Track SSH session connection results for history */
        if (app.vault) {
            for (i32 ti = 0; ti < app.tab_count; ti++) {
                Tab *t = &app.tabs[ti];
                if (!t->session || t->history_tracked) continue;
                if (session_type(t->session) != SESSION_SSH) continue;
                SessionStatus st = session_status(t->session);
                if (st == SESSION_CONNECTED) {
                    const SSHConfig *cfg = session_get_config(t->session);
                    if (cfg) {
                        vault_update_history_result(app.vault, cfg->hostname,
                                                    cfg->port, cfg->username, true);
                    }
                    t->history_tracked = true;
                } else if (st == SESSION_ERROR) {
                    const SSHConfig *cfg = session_get_config(t->session);
                    if (cfg) {
                        vault_update_history_result(app.vault, cfg->hostname,
                                                    cfg->port, cfg->username, false);
                    }
                    t->history_tracked = true;
                }
            }
        }

        /* SSH keepalive — interval is measured in seconds inside libssh2,
         * so checking every frame served no purpose. Once a second is
         * fine and lifts the per-tab iteration off every wake. */
        {
            static f64 next_keepalive_tick = 0;
            f64 ka_now = platform_time_sec();
            if (ka_now >= next_keepalive_tick) {
                for (i32 i = 0; i < app.tab_count; i++) {
                    if (app.tabs[i].session)
                        session_keepalive_check(app.tabs[i].session, ka_now);
                    if (app.tabs[i].session2)
                        session_keepalive_check(app.tabs[i].session2, ka_now);
                }
                next_keepalive_tick = ka_now + 1.0;
            }
        }

        /* Sleep inactive tabs after long idle periods. */
        {
            f64 sleep_now = platform_time_sec();
            if (next_tab_sleep_scan == 0 || sleep_now >= next_tab_sleep_scan) {
                app_sleep_inactive_tabs(&app, sleep_now);
                next_tab_sleep_scan = sleep_now + 30.0;
            }
        }

        /* Time-based visuals redraw on adaptive timeout ticks instead of
         * forcing a continuous idle render loop. Cursor blink is now
         * discrete on/off (period 1.0s) — only redraw on the toggle edge,
         * not on every wake-up.
         *
         * UI animations (modal open, hover fade, palette selection slide,
         * tab hover, toolbar button hover, etc.) MUST force a redraw on
         * every wake while running. Without this the main loop wakes at
         * FRAME_DT_BURST (set above when anim_global_active) but only
         * renders when an incidental event arrives — making animations
         * stutter / freeze whenever the mouse stops moving. */
        /* Sites overlay: keep repainting across its ENTIRE close transition and
         * for one final frame after the close animation drains. app.sites.active
         * is already false the moment it's dismissed, so the caret_anim clause
         * below won't cover the fade-out; and anim_global_active() flips false on
         * the very frame the animation finishes — so the last (fully cleared)
         * frame would never be drawn. The screen would then freeze on the
         * half-faded panel, whose opaque terminal glyphs linger until some
         * unrelated event (a dev-server log line) triggers a repaint seconds
         * later. The trailing-edge latch forces that one extra clean frame. */
        {
            static bool s_sites_was_visible = false;
            bool sites_visible = app.sites.active ||
                                 !anim_done(&app.sites.close_anim);
            if (sites_visible || s_sites_was_visible) needs_redraw = true;
            s_sites_was_visible = sites_visible;
        }

        if (!had_event && !had_session_data_last_frame) {
            f64 now = platform_time_sec();
            /* Palette + SSH-like dialogs paint a sine-wave blinking caret
             * that demands continuous redraws to look smooth — without
             * this the caret would only update on incidental events. */
            bool caret_anim = app.palette_active || app.ssh_dialog_active ||
                              app.kbi_dialog_active || app.port_forward_dialog_active ||
                              app.sites.active;
            if (anim_global_active() ||
                anim_soft_active() ||
                (app.bell_flash_time > 0 && now - app.bell_flash_time < 0.05) ||
                app.cursor_animating ||
                (app.toast_start_time > 0 && now - app.toast_start_time < 3.5) ||
                caret_anim) {
                needs_redraw = true;
            } else if (app.config.cursor_blink && app.window_focused) {
                /* Redraw only when the blink edge has been crossed since
                 * the last paint. Tracked via a static bool that flips at
                 * the half-second boundary. */
                static bool last_blink_on = true;
                f64 phase = now - floor(now);
                bool blink_on = (phase < 0.5);
                if (blink_on != last_blink_on) {
                    last_blink_on = blink_on;
                    needs_redraw = true;
                }
            }
        }

        /* Update window title from active tab */
        {
            Tab *active = app_active_tab(&app);
            const char *eff = active ? tab_effective_title(active) : NULL;
            if (eff && eff[0]) {
                static char last_title[256] = {0};
                if (strcmp(last_title, eff) != 0) {
                    char win_title[300];
                    snprintf(win_title, sizeof(win_title), "%s — Liu", eff);
                    platform_window_set_title(window, win_title);
                    snprintf(last_title, sizeof(last_title), "%s", eff);
                }
            }
        }

        /* Layout updates are now driven exclusively by EVENT_RESIZE and
         * EVENT_DPI_CHANGE — windowDidResize fires for every step of a
         * live resize on macOS, so the previous per-frame size syscall
         * was a safety net that no longer pulls its weight. Removing it
         * eliminates two syscalls from every wake-up. */

        /* Update IME cursor position for candidate window placement */
        {
            Terminal *ime_t = app_focused_terminal(&app);
            if (ime_t) {
                f32 cw = app.renderer.font.cell_width;
                f32 ch = app.renderer.font.cell_height;
                f32 top_gap = app.config.style.terminal_top_gap * app.dpi_scale;
                f32 cx = app.sidebar_width + app.padding + (f32)ime_t->cursor_x * cw;
                f32 cy = app.tab_bar_height + app.padding + top_gap + (f32)ime_t->cursor_y * ch;
                platform_set_ime_cursor_pos(cx, cy, cw, ch);
            }
        }

        /* Check for keyboard-interactive prompts from any SSH session */
        if (!app.kbi_dialog_active) {
            for (i32 ki = 0; ki < app.tab_count; ki++) {
                Session *ks = app.tabs[ki].session;
                Session *ks2 = (ks && session_status(ks) == SESSION_KBI_PENDING) ? NULL : app.tabs[ki].session2;
                Session *target = (ks && session_status(ks) == SESSION_KBI_PENDING) ? ks
                               : (ks2 && session_status(ks2) == SESSION_KBI_PENDING) ? ks2
                               : NULL;
                if (!target) continue;

                /* Lazy-allocate the prompt + response pools (~4 KB) on first
                 * KBI challenge — keeps idle Liu off the BSS line for users
                 * who never hit a 2FA-protected SSH host. */
                if (!app.kbi_prompts) {
                    app.kbi_prompts = calloc(KBI_MAX_PROMPTS, sizeof(*app.kbi_prompts));
                    if (!app.kbi_prompts) continue;
                }
                if (!app.kbi_responses) {
                    app.kbi_responses = calloc(KBI_MAX_PROMPTS, sizeof(*app.kbi_responses));
                    if (!app.kbi_responses) continue;
                }

                app.kbi_dialog_active = true;
                app.kbi_session = target;
                i32 np = session_kbi_num_prompts(target);
                app.kbi_num_prompts = np;
                snprintf(app.kbi_name, sizeof(app.kbi_name), "%s", session_kbi_name(target));
                snprintf(app.kbi_instruction, sizeof(app.kbi_instruction), "%s", session_kbi_instruction(target));
                for (i32 pi = 0; pi < np && pi < KBI_MAX_PROMPTS; pi++) {
                    snprintf(app.kbi_prompts[pi], sizeof(app.kbi_prompts[pi]), "%s", session_kbi_prompt(target, pi));
                    app.kbi_echo[pi] = session_kbi_echo(target, pi);
                    app.kbi_responses[pi][0] = '\0';
                }
                app.kbi_field = 0;
                needs_redraw = true;
                break;
            }
        }

        /* Update native tab bar -- only when something might have changed.
         * Tab title/state can only mutate from: a UI event, a session
         * read (which feeds OSC titles / cwd / fg-process changes), or
         * an explicit AppState mutation that already runs in this frame.
         * On pure idle wakes (cursor blink edge, time-based ticks) every
         * tab field is byte-identical to last frame — skip the format
         * pass + memcmp entirely. */
        if (had_event || had_session_data_last_frame) {
            static NativeTab last_ntabs[MAX_TABS];
            static i32       last_ntab_count = -1;
            static i32       last_active = -1;

            NativeTab ntabs[MAX_TABS];
            for (i32 ti = 0; ti < app.tab_count; ti++) {
                memset(&ntabs[ti], 0, sizeof(ntabs[ti]));
                tab_format_display_title(&app.tabs[ti],
                                         ntabs[ti].title,
                                         (i32)sizeof(ntabs[ti].title));
                ntabs[ti].active = (ti == app.active_tab);
                SessionType st = tab_primary_session_type(&app.tabs[ti]);
                ntabs[ti].is_ssh = (st == SESSION_SSH || st == SESSION_MOSH);
                ntabs[ti].is_sleeping = app.tabs[ti].sleeping;
            }
            bool tabs_changed = (last_ntab_count != app.tab_count ||
                                 last_active != app.active_tab ||
                                 memcmp(last_ntabs, ntabs,
                                        (usize)app.tab_count * sizeof(NativeTab)) != 0);
            if (tabs_changed) {
                platform_update_tabs(window, ntabs, app.tab_count, app.active_tab);
                memcpy(last_ntabs, ntabs, (usize)app.tab_count * sizeof(NativeTab));
                last_ntab_count = app.tab_count;
                last_active = app.active_tab;
            }
        }

        /* Render — only when something changed. The bench overlay used to
         * force a continuous redraw which itself kept the loop hot and
         * inflated the very metric it was meant to measure. Now bench
         * reports the real organic frame cadence; it just refreshes its
         * own overlay panel at 2 Hz so the FPS readout updates visibly. */
        if (app.bench_enabled) {
            static f64 last_bench_panel = 0;
            f64 bnow = platform_time_sec();
            if (bnow - last_bench_panel >= 0.5) {
                needs_redraw = true;
                last_bench_panel = bnow;
            }
        }
        if (needs_redraw) {
            /* Present cap while vsync is off: never present faster than the
             * panel refreshes. Defer (keep needs_redraw pending) if the last
             * present was under one refresh period ago — the timeout gate above
             * already armed a wake-up at the next slot, and a real input event
             * wakes us sooner, so this only trims wasted super-refresh presents
             * during sustained motion. */
            f64 present_now = platform_time_sec();
            if (vsync_off_now && (present_now - last_present_t) < min_present_dt) {
                /* too soon — leave needs_redraw set; render on the next slot */
            } else {
                platform_make_current(window);
                f64 t_render_start = app.bench_enabled ? platform_time_sec() : 0;
                app_render(&app);
                platform_window_swap_buffers(window);
                if (app.bench_enabled) {
                    f64 ms = (platform_time_sec() - t_render_start) * 1000.0;
                    app_bench_record(&app, ms);
                }
                last_present_t = present_now;
                needs_redraw = false;
            }
        }
    }

cleanup:
    platform_unwatch_file();
    /* Cancel any in-flight updater network task so a late NSURLSession callback
     * can't write into the UpdateState as app_destroy tears the app down. */
    updater_cancel(&app.updater);
    app_save_sessions(&app);

    /* Drain the in-flight sidebar paste worker so the orphan thread
     * can't UAF on Session / FileBrowser fields that app_destroy is
     * about to free. Polling is cheap (~5 ms per iteration) and the
     * worker checks atomically each chunk, so cancellation is fast. */
    while (app.fb_task_active && app.fb_task_opaque) {
        FileOpsPasteTask *_pt = (FileOpsPasteTask *)app.fb_task_opaque;
        if (atomic_load(&_pt->done)) {
            pthread_join(_pt->tid, NULL);
            free(_pt);
            app.fb_task_opaque = NULL;
            app.fb_task_active = false;
            break;
        }
        struct timespec _ts = {0, 5 * 1000 * 1000};
        nanosleep(&_ts, NULL);
    }

    /* Reap the async CLI-agent detection thread before tearing down
     * AppState. Without this the thread can still be writing into
     * app->agent_has[] while app_destroy is freeing fields around it. */
    pthread_mutex_lock(&s_agent_detect_mtx);
    bool _ad_live = s_agent_detect_thread_live;
    pthread_mutex_unlock(&s_agent_detect_mtx);
    if (_ad_live) {
        pthread_join(s_agent_detect_tid, NULL);
        s_agent_detect_thread_live = false;
    }

    /* Remove the hooks we installed at launch so a closed Liu leaves no
     * permanent footprint — Claude (and other agents) won't fire notify hooks
     * into a daemon that isn't running once we're gone. Shared with the atexit
     * handler (clears the flag, so the atexit re-run is a no-op). */
    liu_lifecycle_atexit();

    /* Stop the in-process notify server and join its thread before tearing
     * down the app, so the socket is released cleanly. */
    notify_server_stop();

    app_destroy(&app);

    /* Static caches released after app_destroy so any shutdown hook still has
     * access to the data, then freed cleanly so leak-checkers see a quiescent
     * process at exit. */
    cmd_history_shutdown();
    cmd_suggest_shutdown();
    free(g_md_file_paths); g_md_file_paths = NULL;
    free(g_md_file_names); g_md_file_names = NULL;
    g_md_file_count = 0;

    platform_window_destroy(window);
    platform_shutdown();
    libssh2_exit();
    return 0;
}
