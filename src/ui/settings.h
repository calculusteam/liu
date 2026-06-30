/*
 * Liu - in-app settings panel
 * OpenGL-rendered overlay for font, theme, terminal config.
 */
#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include "core/types.h"
#include "core/config.h"
#include "core/keybind.h"
#include "renderer/renderer.h"
#include "vault/vault.h"
#include "notify/config.h"

typedef enum {
    SETTINGS_TAB_APPEARANCE,
    SETTINGS_TAB_TERMINAL,
    SETTINGS_TAB_TRANSLATE,
    SETTINGS_TAB_NOTIFY,
    SETTINGS_TAB_KEYS,
    SETTINGS_TAB_VAULT,
    SETTINGS_TAB_ABOUT,
    SETTINGS_TAB_COUNT,
} SettingsTab;

typedef struct {
    bool         open;
    SettingsTab  active_tab;
    i32          scroll_y;
    i32          hover_item;    /* -1 = none */
    i32          selected_theme;
    AppConfig   *config;       /* pointer to live config */
    bool         needs_font_reload;
    bool         needs_layout;
    /* Set by the Appearance tab when the user clicks the "Create Theme"
     * button. Main loop polls this flag, opens the modal, and clears it. */
    bool         requests_create_theme;
    /* Set by the Appearance tab when the user clicks the ✕ next to a
     * user theme. Holds the display name; main loop calls
     * theme_delete_user, applies the fallback theme if needed, and
     * clears the buffer. */
    char         theme_to_delete[64];

    /* Inline text editing for opacity */
    bool         editing_opacity;
    char         opacity_buf[8];
    i32          opacity_buf_len;

    /* Translate tab language picker: 0=closed, 1=source, 2=target. */
    i32          translate_lang_picker;
    /* Translate tab model picker — dropdown over the current backend's
     * model list (agent CLI presets / live model_catalog fetch / API
     * provider list). Mutually-exclusive with the language picker (only
     * one popup overlays the tab at a time). */
    bool         translate_model_picker_open;
    /* Rows scrolled above the dropdown's visible window (live lists can
     * exceed the popup height). Reset whenever the picker opens or the
     * agent/provider changes. */
    i32          translate_model_menu_scroll;
    /* Translate API backend inline editors (key is masked while typing;
     * Cmd+V pastes). Enter commits into config, Escape cancels, clicking
     * away commits. */
    bool         editing_api_key;
    char         api_key_buf[256];
    bool         editing_api_url;
    char         api_url_buf[256];
    /* Custom API provider: when its server exposes no /v1/models list the
     * dropdown has nothing to offer, so the Model row falls back to this
     * free-text editor (the only way to populate api_model). */
    bool         editing_api_model;
    char         api_model_buf[64];

    /* Smart Vault tab state */
    bool         vault_editing_old_pw;     /* change-master flow */
    bool         vault_editing_new_pw;
    bool         vault_editing_new_pw2;
    char         vault_old_pw[256];
    char         vault_new_pw[256];
    char         vault_new_pw2[256];
    char         vault_status[256];        /* transient status/error message */
    f64          vault_status_ts;          /* platform_time_sec when set; 0 = none */

    /* Scroll limit — recomputed each render frame from actual content height */
    i32          max_scroll;

    /* Key rebinding state */
    bool         rebinding;         /* waiting for key press */
    i32          rebind_action;     /* Action enum being rebound */
    KeyBindTable *keybinds;         /* pointer to live keybinds */

    /* Smart Vault — pointer to the app's vault. Lets the Vault tab read
     * state (initialized/unlocked) and call vault_change_master / lock
     * without reaching back into AppState. */
    Vault       *vault;

    /* Notifications tab — owns a working copy of liu-notify's config. Loaded
     * lazily the first time the panel becomes visible so we don't fight
     * with the daemon's own load at startup. Per-row click hit-test
     * rectangles are captured by the renderer each frame; the handler
     * reads them out to dispatch Browse / Test / Clear actions. */
    NotifyConfig notify_cfg;
    bool         notify_cfg_loaded;
    /* Cached row rects for hit-test: 4 events × {browse, test, clear, banner}. */
    f32          notify_browse_x[4], notify_browse_y[4];
    f32          notify_browse_w[4], notify_browse_h[4];
    f32          notify_test_x[4],   notify_test_y[4];
    f32          notify_test_w[4],   notify_test_h[4];
    f32          notify_clear_x[4],  notify_clear_y[4];
    f32          notify_clear_w[4],  notify_clear_h[4];
    f32          notify_banner_x[4], notify_banner_y[4];
    f32          notify_banner_w[4], notify_banner_h[4];
    /* Mic record/stop button per event row. notify_recording_row is the
     * index (0..3) of the row whose recording is in flight, or -1 when
     * nothing is recording. notify_record_status surfaces denial / errors
     * under the rows. */
    f32          notify_mic_x[4], notify_mic_y[4];
    f32          notify_mic_w[4], notify_mic_h[4];
    i32          notify_recording_row;
    char         notify_record_status[160];
    /* "Reset to bundled defaults" button rect cache. */
    f32          notify_reset_x, notify_reset_y;
    f32          notify_reset_w, notify_reset_h;
    /* Per-agent hook install/uninstall button cache. The notify panel
     * lists every CLI agent that agent_detect_available() finds on the
     * machine; today only Claude has a working hook installer, the
     * other rows are present so users can see what's coming. */
    f32          notify_hook_x[16], notify_hook_y[16];
    f32          notify_hook_w[16], notify_hook_h[16];
    char         notify_hook_id[16][16];   /* agent id, e.g. "claude" */
    i32          notify_hook_count;
    char         notify_claude_status[256];

    /* About-tab auto-update: state mirrored from AppState.updater each frame,
     * request flags the main loop consumes, and cached hit rects. */
    i32          update_phase;            /* UpdatePhase */
    bool         update_auto_install;     /* false → "Open Releases Page" */
    char         update_status[200];
    char         update_err[200];
    char         update_avail_version[64];
    char         update_notes[1024];
    long long    update_bytes_done, update_bytes_total;
    bool         requests_update_check;
    bool         requests_update_install;
    f32          update_check_x, update_check_y, update_check_w, update_check_h;
    f32          update_notes_x, update_notes_y, update_notes_w, update_notes_h;
    f32          about_export_x, about_export_y, about_export_w, about_export_h;
    f32          about_edit_x, about_edit_y, about_edit_w, about_edit_h;
    f32          about_made_x, about_made_y, about_made_w, about_made_h;  /* "made by calculus.team" link */
    f32          vault_btn_x, vault_btn_y, vault_btn_w, vault_btn_h;
    bool         requests_vault_resync;   /* notes_vault_path changed → main re-exports $LIU_VAULT */

    /* Translate tab — local model download. Cached hit rects for the
     * download / cancel / file-picker buttons (w == 0 → not shown this
     * frame, so the click hit-test misses it). Live progress is read from
     * model_download_poll() each render, not cached here. */
    f32          tr_model_dl_x, tr_model_dl_y, tr_model_dl_w, tr_model_dl_h;
    f32          tr_model_cancel_x, tr_model_cancel_y, tr_model_cancel_w, tr_model_cancel_h;
    f32          tr_model_pick_x, tr_model_pick_y, tr_model_pick_w, tr_model_pick_h;

    /* Hover-area registry. The renderer pushes a rectangle into this
     * array for every clickable element (nav tabs, buttons, chips,
     * toggles). Mouse-move asks settings_point_clickable() whether to
     * show CURSOR_POINTER. Cleared at the top of every settings_render
     * call so stale frame data can't leak hover state forward. 64 slots
     * is enough for the densest tab today (Appearance: ~30 control
     * rectangles); spillover is silently dropped — those areas just
     * won't trigger the pointer cursor. */
    struct { f32 x, y, w, h; } hover_areas[64];
    i32          hover_area_count;
} SettingsPanel;

/* Returns true when (x,y) lies over a clickable region recorded by the
 * most recent render. Used by the main loop's mouse-move handler to flip
 * the cursor to a pointing hand over buttons inside the settings panel. */
bool settings_point_clickable(const SettingsPanel *sp, f32 x, f32 y);

void settings_init(SettingsPanel *sp, AppConfig *config, KeyBindTable *keybinds,
                   Vault *vault);
void settings_toggle(SettingsPanel *sp);
void settings_render(SettingsPanel *sp, Renderer *r, f32 screen_w, f32 screen_h, f32 dpi);

/* Returns true if settings consumed the event.
 * cw/ch = actual font cell dimensions in framebuffer pixels. */
bool settings_handle_click(SettingsPanel *sp, f32 x, f32 y, f32 screen_w, f32 screen_h,
                            f32 dpi, f32 cw, f32 ch);
/* dy is NSEvent.scrollingDeltaY semantics (pixels when precise, line units
 * otherwise). dpi lets us convert wheel-line units to a sensible pixel step. */
bool settings_handle_scroll(SettingsPanel *sp, f32 dy, bool precise, f32 dpi);
bool settings_handle_char(SettingsPanel *sp, u32 codepoint);
bool settings_handle_key(SettingsPanel *sp, u32 key, u32 mods);

i32 settings_get_font_count(void);
const char *settings_get_font_name(i32 idx);
const char *settings_get_font_path(i32 idx);
bool settings_get_font_installed(i32 idx);
void settings_refresh_fonts(void);
bool settings_import_font_file(const char *source_path, char *out_path, usize out_size);

#endif
