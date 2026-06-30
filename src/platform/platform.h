/*
 * Liu - platform abstraction layer
 * Uniform interface for window management, OpenGL context, and input.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include "core/types.h"

/* =========================================================================
 * Window
 * ========================================================================= */

typedef struct {
    const char *title;
    i32  width;
    i32  height;
    bool resizable;
    bool vsync;
} PlatformWindowConfig;

typedef struct PlatformWindow PlatformWindow;

bool             platform_init(void);
void             platform_shutdown(void);
PlatformWindow  *platform_window_create(const PlatformWindowConfig *cfg);
void             platform_window_destroy(PlatformWindow *w);
void             platform_window_set_title(PlatformWindow *w, const char *title);
void             platform_window_get_size(PlatformWindow *w, i32 *width, i32 *height);
void             platform_window_get_framebuffer_size(PlatformWindow *w, i32 *width, i32 *height);
/* Scale factor mapping points to framebuffer pixels; implementations return
 * >= 1.0 (macOS backingScaleFactor, X11 Xft.dpi/96 clamped, Windows
 * GetDpiForWindow/96). */
f32              platform_window_get_dpi_scale(PlatformWindow *w);
bool             platform_window_should_close(PlatformWindow *w);
void             platform_window_swap_buffers(PlatformWindow *w);
void             platform_make_current(PlatformWindow *w);

/* Window snap / reposition */
typedef enum {
    WIN_SNAP_LEFT_HALF = 0,
    WIN_SNAP_RIGHT_HALF,
    WIN_SNAP_TOP_HALF,
    WIN_SNAP_BOTTOM_HALF,
    WIN_SNAP_FULL,
    WIN_SNAP_CENTER,
    WIN_SNAP_TOP_LEFT,
    WIN_SNAP_TOP_RIGHT,
    WIN_SNAP_BOTTOM_LEFT,
    WIN_SNAP_BOTTOM_RIGHT,
} WindowSnap;
void             platform_window_snap(PlatformWindow *w, WindowSnap pos);
void             platform_begin_window_drag(PlatformWindow *w);

/* =========================================================================
 * Event system
 * ========================================================================= */

typedef enum {
    EVENT_NONE = 0,
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_CHAR_INPUT,
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_UP,
    EVENT_MOUSE_MOVE,
    EVENT_SCROLL,
    EVENT_RESIZE,
    EVENT_FOCUS,
    EVENT_BLUR,
    EVENT_CLOSE,
    EVENT_DROP_FILE,
    EVENT_DPI_CHANGE,
    EVENT_DRAG_ENTER,
    EVENT_DRAG_EXIT,
    /* Menu actions */
    EVENT_MENU_NEW_TAB,
    EVENT_MENU_NEW_WINDOW,   /* Cmd+N — new file when the sidebar/file browser is focused, else new tab */
    EVENT_MENU_CLOSE_TAB,
    EVENT_MENU_SETTINGS,
    EVENT_MENU_SSH_CONNECT,
    EVENT_MENU_IMPORT_SSH,
    EVENT_MENU_TOGGLE_SIDEBAR,
    EVENT_MENU_FONT_BIGGER,
    EVENT_MENU_FONT_SMALLER,
    EVENT_MENU_FONT_RESET,
    EVENT_MENU_FIND,
    EVENT_MENU_THEME,
    EVENT_MENU_SSH_KEYS,
    /* Quake mode */
    EVENT_QUAKE_TOGGLE,
    /* IME composition */
    EVENT_IME_COMPOSE,
} EventType;

/* Key codes (subset — extend as needed) */
typedef enum {
    KEY_UNKNOWN = 0,
    KEY_BACKSPACE = 8,
    KEY_TAB = 9,
    KEY_ENTER = 13,
    KEY_ESCAPE = 27,
    KEY_SPACE = 32,
    KEY_DELETE = 127,
    /* Function keys */
    KEY_F1 = 256, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* Navigation */
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN,
    KEY_INSERT,
    /* Modifiers */
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL,
    KEY_LALT, KEY_RALT, KEY_LSUPER, KEY_RSUPER,
    /* Letters (for shortcut matching) */
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
    KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
    KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    /* Digits */
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
} KeyCode;

typedef enum {
    MOD_NONE  = 0,
    MOD_SHIFT = 1 << 0,
    MOD_CTRL  = 1 << 1,
    MOD_ALT   = 1 << 2,
    MOD_SUPER = 1 << 3,
} KeyMod;

typedef struct {
    EventType type;
    union {
        struct { KeyCode key; u32 mods; bool repeat; }  key;
        struct { u32 codepoint; u32 mods; }              char_input;
        struct { i32 button; i32 x; i32 y; u32 mods; }  mouse;
        struct { f32 dx; f32 dy; u32 mods;
                 bool precise; /* macOS hasPreciseScrollingDeltas:
                                * true = trackpad/Magic Mouse (delta in pixels)
                                * false = mouse wheel (delta in line units) */
                 bool momentum; /* macOS momentumPhase != none: the OS-generated
                                 * inertial tail after the fingers lift. Stale
                                 * if the target context changed since the
                                 * gesture began (e.g. a modal opened). */
               }                                            scroll;
        struct { i32 width; i32 height; }                resize;
        struct { bool focused; }                         focus;
        struct { const char *path; i32 x; i32 y; }       drop;
        struct { i32 x; i32 y; }                         drag;   /* drag hover position (framebuffer px) */
        struct { f32 scale; }                            dpi;
        struct { const char *name; }                     theme;
        struct { const char *text; i32 cursor; }         ime;
    };
} PlatformEvent;

/* Window transparency and opacity */
void platform_window_set_transparent(PlatformWindow *w, bool transparent);
void platform_window_set_opacity(PlatformWindow *w, f32 opacity); /* 0.0-1.0 */

/* Toggle vsync at runtime. Used to opt out of ProMotion's adaptive
 * throttling while playing an animated image — the display drops to
 * ~24 Hz when there's no user input, which would also drop the GIF's
 * effective FPS. Disabling vsync lets us pace renders against wall
 * clock instead of display refresh; the small risk of tearing is
 * acceptable for an inline preview pane. Restore on close. */
void platform_window_set_vsync(PlatformWindow *w, bool enabled);

/* Max refresh rate (Hz) of the display currently showing the window — the
 * panel's ProMotion ceiling (e.g. 120) or a plain 60. Used to cap the present
 * rate while vsync is OFF: vsync-off exists to skip the vblank stall, not to
 * present more frames than the panel can show, so a sustained-motion redraw
 * loop should still throttle to this rate instead of free-running at 250 Hz.
 * Returns 60.0 as a safe floor if the rate can't be determined. */
f32 platform_window_max_refresh_hz(PlatformWindow *w);

/* Toggle CAMetalLayer.presentsWithTransaction so a layer-size change and the
 * drawable present commit in one Core Animation transaction — used to make
 * non-window-edge resizes (split divider / sidebar drag, which don't trigger
 * AppKit's viewWillStartLiveResize) atomic. No-op on non-Metal backends. */
void platform_window_set_presents_with_transaction(PlatformWindow *w, bool enabled);

void platform_poll_events(void);
void platform_wait_events(f64 timeout_sec);  /* block until event or timeout */
bool platform_next_event(PlatformEvent *event);

/* Live-resize render hook.
 *
 * macOS pumps the runloop in NSEventTrackingRunLoopMode while the user drags
 * the window edge — our main loop is blocked, so without this hook the newly
 * exposed pixels render as black until the drag ends. The platform layer
 * fires `cb(user)` at ~display rate while live-resize is in progress; the
 * callback is expected to update layout from the current window size and
 * re-render synchronously. Pass NULL to clear. Safe to call before window
 * creation. */
typedef void (*PlatformRenderCallback)(void *user);
void platform_set_render_callback(PlatformRenderCallback cb, void *user);

/* Title-bar double-click zoom guard. macOS zooms the window on a double-click
 * in the title-bar strip; since our tabs/close-buttons live there, a fast
 * double-click on the × would zoom by accident. The platform calls this query
 * (if set) with framebuffer-pixel coords and only zooms when it returns true —
 * i.e. the point is an empty, non-interactive part of the title bar. Pass NULL
 * to restore the unconditional zoom. */
typedef bool (*PlatformTitlebarZoomQuery)(void *user, f32 fb_x, f32 fb_y);
void platform_set_titlebar_zoom_query(PlatformTitlebarZoomQuery fn, void *user);

/* Begin an OS-level file drag out of the window (e.g. to Finder).
 *
 * Each item is either local (already on disk — `local_path` valid, `is_remote`
 * false) or remote (`is_remote` true; Finder calls `provider(ctx, dst_path)`
 * when it wants the bytes, e.g. on drop into a Finder window). Provider runs
 * on a dedicated background queue and is free to block; ctx lifetime is the
 * caller's responsibility — typically heap-allocated and freed inside
 * `provider` after the write completes.
 *
 * Returns true if the platform layer started a drag session; false if the
 * build or runtime doesn't support it (non-macOS, OpenGL path, no pending
 * mouse event to seed from). */
typedef struct PlatformFilePromise {
    bool        is_remote;
    char        display_name[256];
    char        local_path[2048];   /* valid when !is_remote */
    void       *ctx;                /* opaque, forwarded to provider */
    void      (*provider)(void *ctx, const char *dst_path);
} PlatformFilePromise;

bool platform_begin_file_drag(PlatformWindow *w,
                              const PlatformFilePromise *items, i32 count);

/* Register a socket / file descriptor for read-ready wake-ups. When the
 * kernel has data available on `fd`, the platform wakes `platform_wait_events`
 * immediately (via a dummy UI event on macOS, pipe on Linux, etc.) instead
 * of waiting for the poll timeout. This is what makes typed input feel
 * instantaneous on SSH sessions: echo bytes arriving at the kernel socket
 * no longer need to wait up to FRAME_DT_INTERACTIVE (~8 ms) for the next
 * scheduled poll. The watch is purely a wake-up signal — the main thread
 * still reads the data via session_read. Safe to call from any thread;
 * calling with an already-watched fd is a no-op.
 *
 * After firing, a watch becomes SUSPENDED and will not fire again until
 * the main thread calls platform_resume_watches(). This breaks the
 * infinite-fire cycle that level-triggered DISPATCH_SOURCE_TYPE_READ
 * would otherwise produce: as long as the fd has unread data the source
 * would re-fire immediately, spinning the global queue at 100 %. The
 * main loop must call platform_resume_watches() AFTER draining session
 * I/O each frame — resuming before the drain would re-arm the source
 * while data is still buffered, reinstating the tight loop. */
void platform_watch_socket(int fd);
void platform_unwatch_socket(int fd);
void platform_resume_watches(void);

/* Returns the number of fd-watch handlers that fired since the last call,
 * and atomically resets the counter to zero. The main loop uses this to
 * skip session draining when no fd became readable — combined with no
 * pending UI events, this lets a 100 % idle terminal sit at zero session
 * I/O cost between cursor-blink edges. Safe to call only from the main
 * thread. */
i32 platform_take_fd_fire_count(void);

/* =========================================================================
 * Clipboard
 * ========================================================================= */

const char *platform_clipboard_get(void);
void        platform_clipboard_set(const char *text);

/* =========================================================================
 * URL opening (system default handler)
 * ========================================================================= */

void platform_open_url(const char *url);

/* Open a local file in the system's default text editor.
 * Path is passed as an argv element, never interpolated into a shell string. */
void platform_open_path(const char *path);

/* macOS: Option key behavior — when true, Option acts as Alt for terminal */
void platform_set_option_as_alt(bool enable);

/* =========================================================================
 * Time
 * ========================================================================= */

f64  platform_time_sec(void);   /* monotonic seconds since init */
void platform_sleep_ms(u32 ms);

/* =========================================================================
 * OpenGL loader (minimal — only what we need for GL 3.3 core)
 * ========================================================================= */

bool platform_gl_load(void);

/* =========================================================================
 * GPU device accessors (Metal backend)
 * ========================================================================= */

void *platform_get_gpu_device(PlatformWindow *w);
void *platform_get_gpu_layer(PlatformWindow *w);
void *platform_get_gpu_queue(PlatformWindow *w);

/* =========================================================================
 * Global hotkey (Quake mode)
 * ========================================================================= */

typedef void (*GlobalHotkeyCallback)(void *userdata);
void platform_register_global_hotkey(u32 key, u32 mods, GlobalHotkeyCallback cb, void *userdata);

/* =========================================================================
 * Quake mode (drop-down terminal)
 * ========================================================================= */

void platform_set_quake_params(f32 height_ratio, f32 anim_duration);
void platform_set_quake_mode(PlatformWindow *w, bool enable);
void platform_toggle_quake_window(PlatformWindow *w);
bool platform_is_quake_mode(PlatformWindow *w);

/* =========================================================================
 * Bell / Notifications
 * ========================================================================= */

void platform_play_bell(void);                          /* play system alert sound */
/* Tell the platform the painted tab-strip height (in points) so macOS can keep
 * the traffic-light buttons vertically centered on it. No-op off macOS. */
void platform_set_titlebar_height(f32 points);
void platform_set_dock_badge(const char *text);         /* set dock badge label (NULL/"" to clear) */
void platform_post_notification(const char *title, const char *body); /* post user notification */
void platform_request_attention(void);                  /* bounce dock icon once */
bool platform_is_app_focused(void);                     /* is the app the frontmost application? */

/* =========================================================================
 * Unicode normalization
 * ========================================================================= */

/* Normalize a UTF-8 string to NFC (Normalization Form Composed) into `dst`.
 * Source is read until NUL. Output is NUL-terminated and capped at dstcap-1
 * bytes. Returns true on success; returns false (and leaves dst empty) if
 * the input isn't valid UTF-8 or dstcap is 0. macOS HFS+ and some non-Apple
 * filesystems hand back NFD-decomposed filenames (base char + combining
 * marks) — rendering them codepoint-by-codepoint at fixed cell width drops
 * the marks into their own cells where they look invisible, so the file
 * browser normalizes everything it reads from readdir() / cwd before
 * storing it. Linux and Win32 stubs do a plain copy: the rest of the
 * codebase already assumes NFC there. */
bool platform_utf8_normalize_nfc(const char *src, char *dst, usize dstcap);

/* =========================================================================
 * File watcher (config hot-reload)
 * ========================================================================= */

typedef void (*FileWatchCallback)(const char *path, void *userdata);

/* Start watching a file for modifications. Returns true on success.
 * Only one file can be watched at a time (sufficient for config). */
bool platform_watch_file(const char *path, FileWatchCallback cb, void *userdata);

/* Non-blocking poll for file changes. Call once per frame from main loop. */
void platform_poll_file_watches(void);

/* Stop watching and release resources. */
void platform_unwatch_file(void);

/* =========================================================================
 * IME cursor position (set by main loop so platform can position candidate window)
 * ========================================================================= */

void platform_set_ime_cursor_pos(f32 x, f32 y, f32 cell_w, f32 cell_h);

/* =========================================================================
 * Cursor
 * ========================================================================= */

typedef enum {
    CURSOR_DEFAULT,
    CURSOR_TEXT,
    CURSOR_RESIZE_H,
    CURSOR_RESIZE_V,
    CURSOR_POINTER,
} CursorType;

void platform_set_cursor(CursorType type);

/* macOS: rasterize an SF Symbol into RGBA pixels. Returns false on unsupported
 * platforms or when the symbol cannot be created. The returned pixel pointer is
 * owned by the platform layer and remains valid until shutdown. */
bool platform_get_system_symbol_rgba(const char *name, i32 pixel_size,
                                     u8 r, u8 g, u8 b, u8 a,
                                     const u8 **pixels, i32 *width, i32 *height);

/* =========================================================================
 * Native tab bar (macOS: NSView-based, Linux/Win: OpenGL fallback)
 * ========================================================================= */

typedef struct {
    char  title[128];
    bool  active;
    bool  is_ssh;
    bool  is_sleeping;
} NativeTab;

/* Update the native tab bar with current tab state.
 * Returns: index of tab that was clicked to close (-1 if none) */
void platform_update_tabs(PlatformWindow *w, const NativeTab *tabs, i32 count, i32 active);

/* Get the height of the native tab bar in points */
f32 platform_tab_bar_height(PlatformWindow *w);

/* =========================================================================
 * Native settings panel (macOS: NSPanel, others: OpenGL fallback)
 * ========================================================================= */

/* Show native settings window. Config changes are applied via callbacks.
 * font_names/font_paths: available fonts, installed: which are installed
 * theme_names: available theme names
 * on_font_change(path): called when user selects a font
 * on_theme_change(name): called when user selects a theme */
typedef void (*SettingsFontCallback)(const char *font_path);
typedef void (*SettingsThemeCallback)(const char *theme_name);
typedef void (*SettingsValueCallback)(const char *key, f32 value);

void platform_show_settings(PlatformWindow *w,
                             const char **font_names, const char **font_paths,
                             const bool *font_installed, i32 font_count,
                             const char **theme_names, i32 theme_count,
                             const char *current_font, const char *current_theme,
                             f32 font_size, f32 font_weight,
                             f32 opacity, f32 tab_sleep_minutes,
                             SettingsFontCallback on_font,
                             SettingsThemeCallback on_theme,
                             SettingsValueCallback on_value);
void platform_close_settings(void);
bool platform_settings_visible(void);

/* =========================================================================
 * File dialog (native open panel)
 * ========================================================================= */

/* Show a native file open dialog. extensions is a comma-separated list of
 * allowed file extensions (e.g., "itermcolors,yml,yaml").
 * Returns a static buffer with the selected path, or NULL if cancelled. */
const char *platform_open_file_dialog(const char *title, const char *extensions);

/* Native folder picker. Returns a static buffer with the chosen directory, or
 * NULL if cancelled. */
const char *platform_open_folder_dialog(const char *title);

/* =========================================================================
 * Audio recording (microphone capture)
 *
 * Used by Settings → Notify to let users record their own notification
 * sounds. macOS captures via AVAudioRecorder (AAC/.m4a) and is gated by the
 * microphone-privacy permission; the .app bundle and the dev binary both
 * carry NSMicrophoneUsageDescription so the system prompt can appear. Linux
 * and Win32 are stubs today.
 * ========================================================================= */

typedef enum {
    PLATFORM_MIC_UNKNOWN = 0,   /* not yet determined / request in flight */
    PLATFORM_MIC_GRANTED,
    PLATFORM_MIC_DENIED,
} PlatformMicPermission;

/* Begin recording from the default input device into `out_path` (absolute;
 * the encoder/container is chosen by the backend — .m4a/AAC on macOS).
 * Requests microphone permission if it hasn't been determined yet, in which
 * case recording starts as soon as the user grants access. Returns true if a
 * recording was started (or a permission request is pending), false on hard
 * failure (denied, already recording, bad path). Only one recording at a
 * time. */
bool platform_audio_record_start(const char *out_path);

/* Stop the in-flight recording and finalize the file on disk. Returns true
 * if a recording was active. */
bool platform_audio_record_stop(void);

/* True while a recording is actively capturing. */
bool platform_audio_recording(void);

/* Seconds elapsed in the current recording (0 when not recording). */
f64 platform_audio_record_elapsed(void);

/* Current microphone permission state (non-blocking). */
PlatformMicPermission platform_audio_mic_permission(void);

/* Open the OS privacy settings for the microphone (macOS: System Settings →
 * Privacy & Security → Microphone). Used to recover from a denied state,
 * which the app cannot re-prompt for. No-op where unsupported. */
void platform_open_microphone_settings(void);

#endif /* PLATFORM_H */
