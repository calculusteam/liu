/*
 * Liu - in-app settings panel
 * OpenGL-rendered overlay for font, theme, terminal config.
 */
#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include "core/types.h"
#include "ui/diagnostics.h"
#include "core/config.h"
#include "core/keybind.h"
#include "renderer/renderer.h"
#include "vault/vault.h"
#include "notify/config.h"
#include "platform/platform.h"   /* PlatformAudioDevice — the mic picker's list */
#include "ui/anim.h"      /* Anim — the panel owns its own modal animation */

typedef enum {
    SETTINGS_TAB_APPEARANCE,
    SETTINGS_TAB_TERMINAL,
    SETTINGS_TAB_TRANSLATE,
    SETTINGS_TAB_VOICE,
    SETTINGS_TAB_NOTIFY,
    SETTINGS_TAB_KEYS,
    SETTINGS_TAB_VAULT,
    SETTINGS_TAB_DIAGNOSTICS,
    SETTINGS_TAB_ABOUT,
    SETTINGS_TAB_BROWSER, /* append: preserve existing shortcuts and dev-hook IDs */
    SETTINGS_TAB_COUNT,
} SettingsTab;

enum {
    SETTINGS_LANG_PICKER_CLOSED = 0,
    SETTINGS_LANG_PICKER_SOURCE = 1,
    SETTINGS_LANG_PICKER_TARGET = 2,
};

/* Forward decl — the full struct lives in ui/ui.h, which includes THIS
 * header, so settings.h cannot include it back. Settings entry points take
 * an AppState* purely to reach the shared single-line editing state
 * (AppState.te_*), which gives the Settings text fields the same caret /
 * selection / word-and-line deletion / undo / clipboard behaviour as every
 * other chrome field. NULL is accepted: those fields then draw without a
 * caret and edit without undo. */
struct AppState;

typedef struct {
    bool         open;
    SettingsTab  active_tab;
    i32          scroll_y;
    i32          hover_item;    /* -1 = none */
    i32          selected_theme;
    AppConfig   *config;       /* pointer to live config */
    bool         needs_font_reload;
    bool         needs_layout;
    bool         config_dirty;       /* committed preferences need save/broadcast */
    bool         keybindings_dirty;
    bool         requests_vault_unlock;
    bool         requests_vault_browser;
    /* Set by the Appearance tab when the user clicks the "Create Theme"
     * button. Main loop polls this flag, opens the modal, and clears it. */
    bool         requests_create_theme;
    /* Set by the Appearance tab when the user clicks the ✕ next to a
     * user theme. Holds the display name; main loop calls
     * theme_delete_user, applies the fallback theme if needed, and
     * clears the buffer. */
    char         theme_to_delete[64];

    /* "Reset to Defaults" is two-step: the first click arms it, the second
     * commits. A single click would throw away every preference the user has
     * ever set, and there is no undo — the confirm step is the undo. Cleared
     * whenever the panel closes or the section changes. */
    bool         reset_armed;
    /* Model deletion requires two clicks: the first arms deletion and
     * the second confirms it. Store an armed flag per row so one
     * model's confirmation state cannot affect another row. */
    bool         vo_model_del_armed;
    bool         tr_model_del_armed;

    /* Inline text editing for opacity */
    /* Which continuous control the pointer is currently scrubbing, and the
     * track it was pressed on: these carry the gesture between the press and
     * the moves that follow. -1 = nothing being dragged. */
    i32          drag_slider;          /* SettingsSlider */
    f32          drag_track_x, drag_track_w, drag_track_h;
    /* Set when an Appearance click changed a window-background setting
     * (opacity/blur/image). main.c consumes and clears it, then re-pushes the
     * whole background state — the panel itself has no window handle, and the
     * image load needs the GPU device. */
    bool         bg_dirty;
    /* A Text Weight stepper moved. Main pushes the config to the renderer
     * (app_apply_config), hands it to the other windows and saves — no relayout
     * and no atlas rebuild, which is why it is not needs_layout. */
    bool         text_weight_dirty;

    bool         browser_policy_dirty;
    bool         browser_rule_editing;
    i32          browser_rule_index; /* -1 = add, otherwise replace this row */
    char         browser_rule_buf[BROWSER_RULE_CAP];
    char         browser_rule_status[128];
    bool         browser_rule_status_error;

    f32          nav_scroll;
    f32          nav_max_scroll;
    f32          nav_clip_x, nav_clip_y, nav_clip_w, nav_clip_h;
    bool         nav_reveal_selection;

    /* Translate tab language picker: 0=closed, 1=source, 2=target. */
    i32          translate_lang_picker;
    /* Translate tab model picker — dropdown over the current backend's
     * model list (agent CLI presets / live model_catalog fetch / API
     * provider list). Mutually-exclusive with the language picker (only
     * one popup overlays the tab at a time). */
    bool         translate_model_picker_open;
    /* Rows scrolled above the menu's window (live lists can exceed it).
     * Placed when the menu opens so the configured model is in view. */
    i32          translate_model_menu_scroll;
    /* The menu's ACTIVE row — what Enter takes, what the arrows move and what
     * the pointer moves onto: an index into the rows the menu shows (the
     * pinned Default row is 0 when it is there), -1 for none. */
    i32          model_menu_cursor;
    /* Where the pointer was when a key last moved the cursor. A pointer that
     * has not moved since does not take the cursor back — or a row under a
     * resting mouse would undo every arrow press. */
    f32          model_menu_key_px, model_menu_key_py;
    /* What is typed into the menu's search field (long lists only). */
    char         model_menu_filter[64];
    /* The window's rows as the last frame placed them: the keys reveal the
     * cursor against the window the user is looking at. 0 = not painted yet. */
    i32          model_menu_visible;
    /* The Model chip the menu hangs from, as the last frame painted it (the
     * click has to find the menu BEFORE it walks the rows the menu covers).
     * w == 0: the row was not drawn. */
    f32          tr_model_chip_x, tr_model_chip_y, tr_model_chip_w, tr_model_chip_h;
    /* A popover painted over the tab this frame, and the first hover area that
     * belongs to it: under it, only its own rows are clickable — the controls
     * it covers must not turn the pointer into a hand. w == 0: none. */
    f32          overlay_x, overlay_y, overlay_w, overlay_h;
    i32          overlay_first_area;
    /* API backend inline editors (key is masked while typing; Cmd+V pastes).
     * Enter commits into config, Escape cancels, clicking away commits.
     *
     * Shared by the Translate tab and the Voice tab: both have an API provider
     * block, only one tab is on screen at a time, and one set of buffers means
     * one place where the "no quotes or backslashes, they break both
     * config.json and the curl config" rule is enforced. `api_edit_tab` is
     * which tab opened the editor — the commit has to know which config to
     * write into, and a flag left set by the other tab must not swallow keys
     * (which is the failure mode the palette's Cycle Backend already caused
     * once). */
    bool         editing_api_key;
    char         api_key_buf[256];
    bool         editing_api_url;
    char         api_url_buf[256];
    /* Free-text model editor. Used by the custom Translate provider (whose
     * server may expose no /v1/models list to pick from) and by every Voice
     * provider — a speech model id is one of two or three per provider, so a
     * live catalog fetch would be more machinery than the choice deserves. */
    bool         editing_api_model;
    char         api_model_buf[64];
    /* ---- Diagnostics tab -------------------------------------------------
     * The report is held per panel (so two windows can each have one open) and
     * is only ever produced by the user pressing Run: diag_run is read-only,
     * but a panel that scanned on open would still make "opening Settings"
     * walk every agent history store on the UI thread. */
    DiagReport   diag;
    i32          diag_last_fixed;     /* summary of the last Fix All, -1 = none */
    i32          diag_last_failed;
    i32          diag_expanded;       /* item index whose detail is open, -1 none */

    SettingsTab  api_edit_tab;

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
    i32          vault_stats_counts[5], vault_stats_total;
    f64          vault_stats_at;
    bool         vault_stats_valid;

    /* Notifications tab — owns a working copy of liu-notify's config. Loaded
     * lazily the first time the panel becomes visible so it does not race
     * the daemon's own load at startup. Per-row click hit-test rectangles
     * are captured by the renderer each frame; the handler reads them out to
     * dispatch Browse / Test / Clear actions. */
    NotifyConfig notify_cfg;
    bool         notify_cfg_loaded;
    /* Generation of notify.conf this copy was read at. Every window edits its
     * own NotifyConfig but they all write the one file, so a copy older than
     * the newest in-process save must be re-read before the next edit —
     * otherwise changing a sound in one window and a voice in another means
     * the second save silently drops the first change. */
    u32          notify_cfg_gen;
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
     * machine; only agents with a hook installer get a button rect
     * here. */
    f32          notify_hook_x[16], notify_hook_y[16];
    f32          notify_hook_w[16], notify_hook_h[16];
    char         notify_hook_id[16][16];   /* agent id, e.g. "claude" */
    i32          notify_hook_count;
    /* The agent whose hook the Install button just put in, "" when none: the
     * main loop adds it to the set this instance removes on quit, then clears
     * it. Without the hand-over a hook installed here outlived Liu. */
    char         notify_hook_installed[16];
    /* "Install at launch" (AppConfig.agent_hooks) was just switched: 1 = on,
     * 2 = off. main.c puts the hooks in or takes them out and clears it. */
    i32          notify_hooks_request;
    f32          notify_auto_x, notify_auto_y, notify_auto_w, notify_auto_h;
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
    /* prompt_recall changed → main calls app_prompt_recall_apply_mode() so the
     * open cards match the new mode this frame. The panel has no AppState, and
     * the ⌘K commands must not be the only surface where the control takes
     * effect immediately. */
    bool         requests_prompt_recall_sync;
    /* Terminal tab → "Customize Sidebar": main closes the panel and opens the
     * sidebar popover (sbc_open), the same object Cmd+K reaches. Closing first
     * matters: the settings overlay paints after everything else and owns
     * input while open, so a popover opened behind it would be unreachable. */
    bool         requests_customize_sidebar;

    /* Translate tab — local model download. Cached hit rects for the
     * download / cancel / file-picker buttons (w == 0 → not shown this
     * frame, so the click hit-test misses it). Live progress is read from
     * model_download_poll() each render, not cached here. */
    f32          tr_model_dl_x, tr_model_dl_y, tr_model_dl_w, tr_model_dl_h;
    f32          tr_model_cancel_x, tr_model_cancel_y, tr_model_cancel_w, tr_model_cancel_h;
    f32          tr_model_pick_x, tr_model_pick_y, tr_model_pick_w, tr_model_pick_h;
    f32          tr_model_del_x, tr_model_del_y, tr_model_del_w, tr_model_del_h;

    /* Voice tab — the language picker. Whisper knows 100 languages, which is
     * a scrolling popup rather than a segmented control (the previous four
     * segments covered Auto/TR/EN/DE and nothing else). Row-major over three
     * columns so scrolling walks the alphabet in reading order. */
    bool         voice_lang_picker_open;
    i32          voice_lang_menu_scroll;   /* rows scrolled above the window */
    f32          voice_lang_menu_accum;    /* sub-tick wheel remainder       */

    /* Voice tab — the microphone picker, and the device list behind it.
     *
     * The list is CACHED rather than enumerated on demand, for two reasons.
     * Enumerating is a walk of CoreAudio properties per device and the render
     * path would do it every frame; and, more importantly, the click handler
     * has to hit-test against the rows the user actually SAW. Re-enumerating
     * at click time means a device arriving between the paint and the click
     * shifts every row under the pointer. Refreshed on a ~1 s clock by the
     * render pass, and immediately when the picker opens. */
#define SETTINGS_MIC_MAX 16
    bool                voice_mic_picker_open;
    PlatformAudioDevice voice_mics[SETTINGS_MIC_MAX];
    i32                 voice_mic_count;
    f64                 voice_mic_scanned_at;   /* 0 = never scanned */

    /* Voice tab — same three buttons for the Whisper model. Separate rects
     * rather than shared ones because both tabs can be rendered across a
     * single session and a stale rect from the other tab would stay
     * clickable. */
    f32          vo_model_dl_x, vo_model_dl_y, vo_model_dl_w, vo_model_dl_h;
    f32          vo_model_cancel_x, vo_model_cancel_y, vo_model_cancel_w, vo_model_cancel_h;
    f32          vo_model_pick_x, vo_model_pick_y, vo_model_pick_w, vo_model_pick_h;
    f32          vo_model_del_x, vo_model_del_y, vo_model_del_w, vo_model_del_h;

    /* Hover-area registry. The renderer pushes a rectangle into this
     * array for every clickable element (nav tabs, buttons, chips,
     * toggles). Mouse-move asks settings_point_clickable() whether to
     * show CURSOR_POINTER. Cleared at the top of every settings_render
     * call so stale frame data can't leak hover state forward.
     *
     * Sized for the worst case. Appearance pushes one rect per installed
     * FONT and one per THEME, both of which the user can add to without
     * limit, and spillover is dropped silently — so an undersized array
     * doesn't fail loudly, it just stops the pointing hand appearing on
     * whatever was pushed last. That tail is the user themes, i.e.
     * exactly the cards carrying the delete button. */
    struct { f32 x, y, w, h; } hover_areas[192];
    i32          hover_area_count;

    /* The scrolling content's clip band, published by settings_render while it
     * walks the scrolled body (see the scissor it pushes). A hover rect is only
     * as clickable as it is VISIBLE: the body scrolls under a pinned section
     * header, and rows that slide beneath it are drawn-clipped; pushed at full
     * size, they would show the pointing hand over the header and let a click
     * through to a control nobody can see. sp_push_hover() clips against this
     * while the flag is up; panel chrome (nav tabs, Reset) pushes with it down
     * and is unaffected. */
    f32          clip_y0, clip_y1;
    bool         clip_active;

    /* Current pointer position in framebuffer pixels, refreshed by the
     * renderer each frame from app->hover_x/hover_y. Feeds ui_hover_amount()
     * so every clickable control in settings gets a smooth hover highlight
     * (settings_render has no other access to the mouse position). */
    f32          ptr_x, ptr_y;

    /* Modal open/close animation, held per panel like SiteManagerUI's.
     * settings_render() is called unconditionally for every window, so
     * function-scope statics would be shared: one window's call would flip
     * `was_open` on another's behalf, painting a dead panel in one window
     * while the real one stays invisible in the other. */
    Anim         open_anim;
    Anim         close_anim;
    bool         was_open;

    /* Sub-tick wheel accumulators. Trackpads report pixels; the fractional
     * remainder has to survive between events or slow pans round away to
     * nothing. Per panel so two windows don't drain each other's remainder. */
    f32          scroll_accum;        /* panel body, half-cell units */
    f32          model_menu_accum;    /* Translate model dropdown, rows */

    /* Body scrollbar. Geometry is published by the render pass (the hit test
     * and the drag both need the exact track the user sees), and the bar
     * auto-hides the same way the file-browser sidebar's does: `sb_active_t`
     * is stamped while the pointer is over the body or the body scrolls, then
     * ui_autohide_alpha() fades it out. `sb_seen_scroll` is last frame's
     * scroll_y, which is what makes wheel, keyboard and drag all count as
     * activity without each of them stamping the clock itself. */
    f64          sb_active_t;
    i32          sb_seen_scroll;
    f32          sb_track_x, sb_track_y, sb_track_w, sb_track_h;
    f32          sb_thumb_h;
    bool         sb_dragging;
    f32          sb_drag_grab;        /* pointer offset inside the thumb, px */
} SettingsPanel;

/* Returns true when (x,y) lies over a clickable region recorded by the
 * most recent render. Used by the main loop's mouse-move handler to flip
 * the cursor to a pointing hand over buttons inside the settings panel. */
bool settings_point_clickable(const SettingsPanel *sp, f32 x, f32 y);

void settings_init(SettingsPanel *sp, AppConfig *config, KeyBindTable *keybinds,
                   Vault *vault);
void settings_toggle(SettingsPanel *sp);
void settings_close(SettingsPanel *sp);
void settings_select_tab(SettingsPanel *sp, SettingsTab tab);
/* Pure editing-state helpers, also used by the lifecycle tests. */
void settings_clear_focus(SettingsPanel *sp);
bool settings_commit_api_editors(SettingsPanel *sp);
bool settings_capture_binding(SettingsPanel *sp, u32 key, u32 mods);
/* `app` supplies the shared field-editing state (caret / selection / scroll)
 * for the Settings text inputs; may be NULL, in which case those fields draw
 * without a caret. */
void settings_render(SettingsPanel *sp, struct AppState *app, Renderer *r,
                     f32 screen_w, f32 screen_h, f32 dpi);

/* Returns true if settings consumed the event.
 * cw/ch = actual font cell dimensions in framebuffer pixels. */
bool settings_handle_click(SettingsPanel *sp, f32 x, f32 y, f32 screen_w, f32 screen_h,
                            f32 dpi, f32 cw, f32 ch);
/* dy is NSEvent.scrollingDeltaY semantics (pixels when precise, line units
 * otherwise). dpi converts wheel-line units to a sensible pixel step. */
bool settings_handle_scroll(SettingsPanel *sp, f32 dy, bool precise, f32 dpi);

/* Continuous controls the pointer can scrub. */
typedef enum {
    SETTINGS_SLIDER_NONE = -1,
    SETTINGS_SLIDER_OPACITY = 0,
    SETTINGS_SLIDER_FADE,
} SettingsSlider;

/* Pointer moved while a slider or the body scrollbar was held: apply the new
 * value. Returns true when a drag was in progress (so the caller repaints).
 * Sliders scrub horizontally and the scrollbar vertically, hence both coords. */
bool settings_handle_drag(SettingsPanel *sp, f32 x, f32 y);
/* Pointer released — ends any scrub. */
void settings_end_drag(SettingsPanel *sp);
bool settings_handle_char(SettingsPanel *sp, struct AppState *app, u32 codepoint);
bool settings_handle_key(SettingsPanel *sp, struct AppState *app, u32 key, u32 mods);

/* The Translate tab's Model menu as it stands: what it lists and where the
 * last frame put it. Read-only, so automation can address "the third row"
 * without knowing a pixel. Rects are in framebuffer pixels and zero while the
 * menu is closed or not painted yet. */
typedef struct {
    bool open, searchable, pinned, above;
    i32  rows;            /* rows listed (after the search), the pinned one included */
    i32  total;           /* ...and before it                                        */
    i32  cursor, current; /* the active row and the configured model's row; -1 none  */
    i32  scroll, visible;
    f32  chip_x, chip_y, chip_w, chip_h;
    f32  card_x, card_y, card_w, card_h;
    f32  row_x, row_y, row_w, row_h;      /* the window's first row                  */
    f32  pinned_y;                        /* the pinned row's top                    */
    const char *model;    /* the configured model id ("" = the agent's default)      */
    const char *filter;
    const char *status;   /* "live", "updating", "cached", "offline" or ""           */
} SettingsModelMenuInfo;
void settings_model_menu_info(const SettingsPanel *sp, f32 screen_w, f32 screen_h,
                              f32 dpi, SettingsModelMenuInfo *out);

i32 settings_get_font_count(void);
const char *settings_get_font_name(i32 idx);
const char *settings_get_font_path(i32 idx);
bool settings_get_font_installed(i32 idx);
void settings_refresh_fonts(void);
bool settings_import_font_file(const char *source_path, char *out_path, usize out_size);

#endif
