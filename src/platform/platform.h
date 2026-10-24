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
    /* Order the window in at alpha 0. The first-run intro (src/intro/) owns
     * the fade from there; without this the window would be on screen for a
     * frame ahead of its own introduction. macOS only — ignored elsewhere. */
    bool start_transparent;
    /* Open the window with its top-left HERE (screen points, origin at the top
     * of the main display, y down) instead of centred. A tab torn off into a
     * window is born under the pointer that is holding it: created centred and
     * moved afterwards it is on screen for a frame in the wrong place, in the
     * middle of a drag. Backends that cannot place a window ignore it. */
    bool has_origin;
    f32  origin_x, origin_y;
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
/* The close the window's button asked for was refused (an unsaved document):
 * the request is taken back, so the loop's reaper does not carry it out. */
void             platform_window_cancel_close(PlatformWindow *w);
/* Request the same close action as clicking the window's close button. */
void             platform_window_perform_close(PlatformWindow *w);
void             platform_window_swap_buffers(PlatformWindow *w);
void             platform_make_current(PlatformWindow *w);
/* True while the window occupies its own full-screen space (macOS native
 * fullscreen). The traffic lights move into AppKit's auto-hiding overlay there,
 * so the toolbar must drop the left zone it normally reserves for them.
 * Backends without a fullscreen concept return false. */
bool             platform_window_is_fullscreen(PlatformWindow *w);

#ifdef PLATFORM_LINUX
/* Enter/leave fullscreen. Linux-only because macOS reaches AppKit's native
 * fullscreen through the View menu (toggleFullScreen:) and has no need for a
 * platform entry point; X11 has no menu bar, so ACT_TOGGLE_FULLSCREEN is
 * dispatched here. */
void             platform_window_set_fullscreen(PlatformWindow *w, bool enable);
#endif

/* Right edge (in points, from the window's left edge) of the rightmost native
 * window button — the zoom light on macOS. The toolbar derives its left inset
 * from this instead of a hardcoded zone, because AppKit lays the lights out
 * differently depending on which SDK the binary was linked against: a 14.5-SDK
 * build gets x = 7/27/47 (right edge 61pt), a 26-SDK build x = 9/32/55 (right
 * edge 69pt), so a fixed zone would leave a different gap in front of the
 * first icon per toolchain. Returns 0 when there are no such buttons to
 * measure — native fullscreen (they move into AppKit's overlay), a backend
 * without window decorations, or a window whose buttons haven't been laid out
 * yet — and the caller falls back to TRAFFIC_LIGHT_ZONE_PT. */
f32              platform_traffic_light_right_pt(PlatformWindow *w);

/* False when the window is fully covered by other windows, minimised, or on a
 * space that isn't showing — i.e. when nothing drawn could reach a pixel. The
 * render path uses this to skip the frame entirely (the pending-redraw flag
 * stays set, so the first visible frame repaints everything). Sessions keep
 * being polled either way, so a hidden tab's PTY never stalls.
 *
 * Backends without an occlusion signal must return true. */
bool             platform_window_visible(PlatformWindow *w);

/* Minimize (macOS: the real -miniaturize:, animated and asynchronous — ask
 * platform_window_is_minimized a moment later) or restore a window. Linux
 * iconifies through the WM. */
void             platform_window_set_minimized(PlatformWindow *w, bool minimized);
bool             platform_window_is_minimized(PlatformWindow *w);

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
#ifndef PLATFORM_MACOS
/* The X11 window has no window-manager title bar or borders: Liu's toolbar is
 * the title bar, and Liu draws minimize / maximize / close at its right end
 * and resizes from a band along the edges (the platform does that part on its
 * own). macOS has the system's traffic lights and none of this. */
bool             platform_draws_own_window_controls(void);
void             platform_window_minimize(PlatformWindow *w);
void             platform_window_toggle_maximize(PlatformWindow *w);
bool             platform_window_is_maximized(PlatformWindow *w);
/* What the window manager's close does: EVENT_CLOSE for that window. */
void             platform_window_request_close(PlatformWindow *w);
#endif
/* Position a torn-off window at the pointer's drop location so it remains
 * visually distinct from its source. */
void             platform_window_set_position(PlatformWindow *w, f32 x, f32 y);
/* Window top-left in the same top-origin screen-point space. */
void             platform_window_get_position(PlatformWindow *w, f32 *x, f32 *y);

/* Pointer geometry ACROSS windows, for dragging a tab from one window onto
 * another's tab strip. During a drag the source window holds mouse capture, so
 * it is the only one that hears the pointer and its coordinates keep arriving
 * (out of range, negative) after the pointer has left it — the target has to be
 * found from the pointer's SCREEN position.
 *
 * Screen points are the space platform_window_get_position() uses (points,
 * origin at the top of the main display, y down). Window-local points are the
 * framebuffer pixels mouse events carry (top-left of the window, y down), and
 * the two conversions are exact inverses of each other and of the one the mouse
 * event path applies, so a point round-trips.
 *
 * Both return false — leaving the outputs zeroed — when the window has no view
 * yet or the backend cannot say. */
bool             platform_window_local_to_screen(PlatformWindow *w, f32 fx, f32 fy,
                                                 f32 *sx, f32 *sy);
bool             platform_window_screen_to_local(PlatformWindow *w, f32 sx, f32 sy,
                                                 f32 *fx, f32 *fy);
/* The Liu window that is FRONTMOST at a screen point, or NULL when that point
 * belongs to another application's window, the desktop, or nothing of ours.
 * Front-most, not "any window containing it": two Liu windows overlap all the
 * time, and the one a drop lands on is the one the user can see. */
PlatformWindow  *platform_window_at_screen_point(f32 sx, f32 sy);
/* The same question asked THROUGH one of Liu's windows: `through` is looked
 * past when it is the frontmost thing at the point, and the answer is whatever
 * sits directly under it. A tab travelling as its own window is always under
 * the pointer that carries it, so "what is frontmost here" would only ever
 * answer "the thing being dragged". Anything else in the way — another
 * application's window between the two — still answers NULL. */
PlatformWindow  *platform_window_at_screen_point_below(f32 sx, f32 sy,
                                                       PlatformWindow *through);
/* Bring a window to the front and make it the key window (a tab dropped into it
 * should be what the user is looking at afterwards). */
void             platform_window_activate(PlatformWindow *w);
/* The usable rectangle (menu bar and Dock excluded) of the screen a point is on
 * — the nearest screen when the point is on none — in screen points. False,
 * outputs zeroed, when the backend cannot say. */
bool             platform_screen_visible_frame_at(f32 sx, f32 sy,
                                                  f32 *x, f32 *y, f32 *w, f32 *h);
/* Fade a whole window (content, chrome and shadow) while it is being DRAGGED.
 *
 * This is NSWindow.alphaValue and it is not the window's translucency: that is
 * rendered (the clear's alpha, platform_window_set_transparent) and must stay
 * the only route for the look of the window at rest. This one is transient —
 * a tab carried as a window is faded while it hovers a strip it would drop
 * into, so the seam under it can be seen — and whoever sets it puts 1.0 back.
 * A no-op where the backend has no such thing. */
void             platform_window_set_drag_alpha(PlatformWindow *w, f32 alpha);
/* Is the primary button physically down right now? Asked of the system, not of
 * the event stream: a gesture that outlives its own window (a tab dragged out
 * into a new one) must be able to notice a release it was never sent. Backends
 * that cannot ask answer true — "no evidence it ended". */
bool             platform_mouse_primary_down(void);

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
    /* Window became visible/hidden (occlusion). Carries no payload — it exists
     * so the loop wakes and repaints the frame it skipped while covered. */
    EVENT_VISIBILITY,
    /* Menu actions */
    EVENT_MENU_NEW_TAB,
    EVENT_MENU_NEW_WINDOW,   /* Cmd+N — new file when the sidebar/file browser is focused, else new tab */
    EVENT_MENU_CLOSE_TAB,
    EVENT_MENU_SETTINGS,
    EVENT_MENU_SSH_CONNECT,
    EVENT_MENU_IMPORT_SSH,
    EVENT_MENU_MOSH_CONNECT,
    EVENT_MENU_TELNET_CONNECT,
    EVENT_MENU_SERIAL_CONNECT,
    EVENT_MENU_PORT_FORWARDING,
    EVENT_MENU_SSH_KEYS,
    EVENT_MENU_KNOWN_HOSTS,
    EVENT_MENU_TOGGLE_SIDEBAR,
    EVENT_MENU_FONT_BIGGER,
    EVENT_MENU_FONT_SMALLER,
    EVENT_MENU_FONT_RESET,
    EVENT_MENU_FIND,
    EVENT_MENU_THEME,
    EVENT_MENU_SECURE_KEYBOARD,   /* the app menu's "Secure Keyboard Entry" switch */
    /* Finder Services: "New Liu Window/Tab Here". Carries the folder the user
     * right-clicked (or the parent of a right-clicked file) in
     * `open_path.path`; `open_path.new_window` picks window vs tab. Sits inside
     * the menu block on purpose — main.c gates that whole range as "commands
     * that run regardless of which overlay has focus". */
    EVENT_MENU_OPEN_PATH,
    /* A window was dragged by its title strip and dropped onto ANOTHER Liu
     * window's tab strip — the inverse of tearing a tab off. `window` is the
     * one that was dragged; `merge.target` is the one it landed on. */
    EVENT_WINDOW_MERGE,
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
    /* Punctuation needed for shortcut matching. Appended at the end so the
     * values above keep the numbers any persisted keybinding already uses. */
    KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_PERIOD, KEY_COMMA,
    /* The font size chords on Linux (Ctrl+= / Ctrl+-), where no View menu
     * carries them. The macOS backend does not produce these. */
    KEY_MINUS, KEY_EQUAL,
    /* The editor's Cmd+/ (toggle comment). Appended, like the rest. */
    KEY_SLASH,
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
    f64 timestamp; /* monotonic event time; zero for unstamped synthetic events */
    /* When native event time cannot be mapped reliably, this timestamp records
     * dispatch time and is marked uncertain. Latency consumers should count the
     * event but exclude it from measurements. macOS leaves the field unset. */
    bool timestamp_uncertain;
    /* Which window produced this event. NULL means "not attributable to one
     * window" — menu commands and application-level notifications — and the
     * app routes those to whichever window is key.
     *
     * Without this field a key press in one window cannot be told from a key
     * press in another, and no amount of app-side bookkeeping can recover it
     * after the fact. */
    PlatformWindow *window;
    union {
        struct { KeyCode key; u32 mods; bool repeat; }  key;
        /* ime_reset: text an X11 input method committed after
         * platform_ime_reset and before the next keystroke — the ended
         * composition's, which belongs where it was composed (ui/ime_target.h).
         * Always false on macOS. */
        struct { u32 codepoint; u32 mods; bool ime_reset; } char_input;
        /* button: 0 = left, 1 = RIGHT, 2 = middle, -1 = no button (motion).
         * Right is 1, not 2 — the numbering follows AppKit's separate
         * left/right handlers rather than X11's 1/2/3 (where 2 is middle), and
         * every backend must translate into it. Passing X11's raw index
         * through made right-click dead and middle-click open the right-click
         * menu. */
        /* `sx`/`sy` are the pointer in SCREEN points (the space
         * platform_window_get_position uses) when `has_screen` is set. A drag
         * that moves the very window its events arrive in cannot use x/y: they
         * are relative to a frame that has moved since the event was made. */
        struct { i32 button; i32 x; i32 y; u32 mods;
                 bool has_screen; f32 sx; f32 sy; }      mouse;
        struct { f32 dx; f32 dy; u32 mods;
                 bool precise; /* macOS hasPreciseScrollingDeltas:
                                * true = trackpad/Magic Mouse (delta in pixels)
                                * false = mouse wheel (delta in line units).
                                *
                                * A non-precise delta is a LINE COUNT, already
                                * carrying the desktop's lines-per-notch — one
                                * click is 1.0 on macOS and normally 3.0 on
                                * X11, where nothing upstream applies it. See
                                * PLATFORM_VIEW_WHEEL_LINES before multiplying
                                * it by anything per-notch. */
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
        struct { PlatformWindow *target; }               merge;
        struct { const char *path; bool new_window; }    open_path;
    };
} PlatformEvent;

/* Per-notch line multiplier a handful of views apply on top of a non-precise
 * scroll delta.
 *
 * macOS pushes a bare one-line notch through — AppKit's scroll setting shapes
 * the pixel-precise path, not this one — so those views scale it themselves to
 * get a usable step. The X11 layer resolves the desktop's lines-per-notch
 * before the event is ever pushed (wheel_lines_init), so scaling a second time
 * there would move three times what every other window on that desktop does.
 *
 * Views that simply multiply the delta by a row height want nothing from this:
 * they are already correct on both platforms. */
#ifdef PLATFORM_MACOS
#define PLATFORM_VIEW_WHEEL_LINES 3.0f
#else
#define PLATFORM_VIEW_WHEEL_LINES 1.0f
#endif

/* Window translucency is rendered through the clear color's alpha, not
 * NSWindow.alphaValue. The macOS and X11 backends each expose one setter; on
 * X11, _NET_WM_WINDOW_OPACITY would also dim the already-composited text.
 *
 * On macOS, render blur with a CABackdropLayer so it follows the rounded
 * window corners; the window-server blur API affects the rectangular frame.
 * The layer adds no material tint. The private AppKit classes are resolved by
 * name; if unavailable, blur has no effect, and
 * platform_window_blur_is_exact() reports whether the requested radius is
 * supported. Other backends do not apply blur. */
void platform_window_set_transparent(PlatformWindow *w, bool transparent);

/* Blur whatever shows THROUGH a translucent window, in points; 0 turns it off.
 *
 * Only meaningful together with an opacity below 1 — with an opaque window
 * there is nothing behind to blur.
 *
 * On macOS this is a CABackdropLayer under the terminal's own layer, filtered
 * with a plain gaussian. It is drawn by CoreAnimation rather than the window
 * server ON PURPOSE: the server's blur (CGSSetWindowBackgroundBlurRadius)
 * covers the window's frame RECTANGLE, so it showed as a square patch through
 * the rounded corners and nothing on the client side could reshape it. A layer
 * is clipped by the same rounded container as everything else.
 *
 * The backdrop carries no material, which is what separates it from an
 * NSVisualEffectView: that class tints what it blurs, and the terminal's
 * colours go with it.
 *
 * There is NO fallback. Both classes involved are private and resolved by
 * name; where they are missing the setting does nothing at all.
 * platform_window_blur_is_exact() reports whether the radius can be honoured,
 * so a UI can say the control is inert here rather than showing one that
 * silently does nothing. Non-macOS backends no-op. */
void platform_window_set_blur(PlatformWindow *w, f32 radius);

/* Ask the WINDOW SERVER to fade the whole rendered frame, instead of the
 * renderer clearing to a translucent colour.
 *
 * These are not the same picture. Clearing translucent leaves the framebuffer
 * holding "black at 61% alpha", and text drawn over that composites its
 * coverage against an alpha that starts at 0.61 and a colour that starts at
 * ~0: every antialiased edge pixel ends up dark AND nearly opaque, so it
 * blocks the bright surface behind the window and rings each glyph with a dark
 * outline. Rendering opaque and fading afterwards keeps colour and alpha in
 * step — every pixel carries the same alpha, so nothing behind the window can
 * distort a glyph edge.
 *
 * Returns true when the platform took over the fade (the caller must then
 * clear FULLY OPAQUE), false when it cannot and the translucent-clear path has
 * to stand in. */
bool platform_window_set_content_opacity(PlatformWindow *w, f32 opacity);
bool platform_window_blur_is_exact(void);

/* The window the OS considers focused, or NULL when the app is not frontmost.
 * Used to aim window-less events (menu commands) at what the user was looking
 * at. Backends without a concept of key window return NULL. */
PlatformWindow *platform_key_window(void);

/* ---- Blur diagnostics (LIU_BLUR_DEBUG=1) -------------------------------
 *
 * "The blur vanishes for a moment and comes back" has two entirely different
 * causes that look identical on screen: the backdrop LAYER stopped blurring
 * (hidden, filters dropped, moved in the layer tree, window turned opaque), or
 * the layer is fine and Liu painted something opaque over it (a file-browser
 * or browser tab background, a full-window overlay, a clear at alpha 1). These
 * report which one it was.
 *
 * platform_blur_debug_sample() snapshots the window/layer state and logs only
 * the fields that CHANGED since the previous sample, tagged with what
 * triggered it — so a per-loop call is a diff stream, not a firehose. Called
 * from the main loop and from every AppKit hook that could disturb the layer.
 * platform_blur_debug_note() puts an app-side line (render alpha, active tab
 * kind, config reload decisions) in the same stream so the two sides can be
 * lined up by timestamp. Both are no-ops unless the env var is set; the
 * enabled() check is a cached bool, cheap enough to call per frame. */
bool platform_blur_debug_enabled(void);
void platform_blur_debug_sample(PlatformWindow *w, const char *tag);
void platform_blur_debug_note(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));


/* Toggle vsync at runtime. Disabling it lets animated-image
 * presentation follow elapsed time while the display reduces its
 * refresh rate during idle periods. */
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

/* Event-queue diagnostics. Monotonic for the life of the process; any
 * argument may be NULL. `dropped_critical` is the one that matters: a key,
 * close, focus or drop event lost because every one of the 256 queued events
 * ahead of it was also critical. It must stay 0. See platform/event_queue.h
 * for the overflow policy. */
void platform_event_queue_stats(i32 *high_water, u32 *coalesced, u32 *evicted,
                                u32 *dropped_lowpri, u32 *dropped_critical);

/* Live-resize render hook.
 *
 * macOS pumps the runloop in NSEventTrackingRunLoopMode while the user drags
 * the window edge — the main loop is blocked, so without this hook the newly
 * exposed pixels render as black until the drag ends. The platform layer fires
 * `cb(w, user)` at ~display rate while live-resize is in progress; the
 * callback is expected to update layout from the current window size and
 * re-render synchronously. Pass NULL to clear. Safe to call before window
 * creation.
 *
 * `w` is the window BEING RESIZED and the callback must render THAT one. It is
 * not necessarily the key window: rendering the key window instead would leave
 * the dragged one showing stale pixels for the whole drag. The registration
 * stays global (one renderer entry point); only the dispatch is per window.
 * The platform never passes NULL. */
typedef void (*PlatformRenderCallback)(PlatformWindow *w, void *user);
void platform_set_render_callback(PlatformRenderCallback cb, void *user);

/* Title-bar double-click zoom guard. macOS zooms the window on a double-click
 * in the title-bar strip; since Liu's tabs/close-buttons live there, a fast
 * double-click on the × would zoom by accident. The platform calls this query
 * (if set) with framebuffer-pixel coords and only zooms when it returns true —
 * i.e. the point is an empty, non-interactive part of the title bar. Pass NULL
 * to restore the unconditional zoom.
 *
 * `w` is the window that was double-clicked, in the window's own framebuffer
 * pixels. As with the render hook, the hit test must run on THAT window's
 * layout, not the key window's. The platform never passes NULL. */
typedef bool (*PlatformTitlebarZoomQuery)(PlatformWindow *w, void *user,
                                          f32 fb_x, f32 fb_y);
void platform_set_titlebar_zoom_query(PlatformTitlebarZoomQuery fn, void *user);

/* Accessibility text bridge (VoiceOver): a per-window provider describes the
 * focused pane's text in UTF-16 units. platform_window_set_a11y_provider and
 * the snapshot type live in platform/platform_a11y.h. */
#include "platform/platform_a11y.h"

/* Begin an OS-level file drag out of the window (e.g. to Finder).
 *
 * Each item is either local (already on disk — `local_path` valid, `is_remote`
 * false) or remote (`is_remote` true; Finder calls `provider(ctx, dst_path)`
 * when it wants the bytes, e.g. on drop into a Finder window). Provider runs
 * on a dedicated background queue and is free to block. When a drag starts,
 * the platform owns each remote ctx and calls `cleanup(ctx)` exactly once,
 * after provider returns or when an unfulfilled/cancelled promise is released.
 * If this function returns false, ownership stays with the caller.
 *
 * Returns true if the platform layer started a drag session; false if the
 * build or runtime doesn't support it (non-macOS, OpenGL path, no pending
 * mouse event to seed from). */
typedef struct PlatformFilePromise {
    bool        is_remote;
    char        display_name[256];
    char        local_path[2048];   /* valid when !is_remote */
    void       *ctx;                /* opaque, forwarded to provider */
    bool      (*provider)(void *ctx, const char *dst_path);
    void      (*cleanup)(void *ctx);
} PlatformFilePromise;

bool platform_begin_file_drag(PlatformWindow *w,
                              const PlatformFilePromise *items, i32 count);

/* Register a socket / file descriptor for read-ready wake-ups. When the kernel
 * has data available on `fd`, the platform wakes `platform_wait_events`
 * immediately (via a dummy UI event on macOS, pipe on Linux, etc.) instead of
 * waiting for the poll timeout. This is what makes typed input feel
 * instantaneous on SSH sessions: echo bytes arriving at the kernel socket do
 * not wait up to FRAME_DT_INTERACTIVE (~8 ms) for the next scheduled poll. The
 * watch is purely a wake-up signal — the main thread still reads the data via
 * session_read. Safe to call from any thread; calling with an already-watched
 * fd is a no-op.
 *
 * After firing, a watch becomes SUSPENDED and will not fire again until the
 * main thread calls platform_resume_watches(). This breaks the infinite-fire
 * cycle that level-triggered DISPATCH_SOURCE_TYPE_READ would otherwise
 * produce: as long as the fd has unread data the source would re-fire
 * immediately, spinning the global queue at 100 %. The main loop must call
 * platform_resume_watches() AFTER draining session I/O each frame — resuming
 * before the drain would re-arm the source while data is still buffered,
 * reinstating the tight loop. */
void platform_watch_socket(int fd);
void platform_unwatch_socket(int fd);
void platform_resume_watches(void);

/* Backpressure: hold ONE fd's read watch disarmed for exactly one cycle.
 *
 * Call it for a pane that spent its whole per-frame read budget: re-arming a
 * source that still has buffered data only makes it fire again immediately,
 * which is the loop-spinning failure core/poll_budget.h describes. The hold is
 * a single-use token — platform_resume_watches() (macOS) / platform_wait_events()
 * (Linux) consumes it, so the watch is armed again one cycle later.
 *
 * A held pane MUST still be drained on the next frame by the caller: its watch
 * can no longer wake the loop for it. main.c does that from
 * AppState.poll_saturated_panes, and that forced drain is ALSO what keeps every
 * ControlMaster sibling multiplexed over a held socket draining. MAIN THREAD
 * ONLY. A hold on an unwatched or out-of-range fd is a harmless no-op.
 * See src/platform/watch_hold.h. */
void platform_hold_watch(int fd);

/* Wake platform_wait_events from ANY thread, and make the main loop run a
 * session poll on the turn it wakes into (i.e. it also counts toward
 * platform_take_fd_fire_count). This is the fd-less sibling of
 * platform_watch_socket: a PTY reader thread has no descriptor for the loop to
 * watch — the bytes are already in its ring — but the loop still has to be
 * told they arrived. Coalesced, because a flooding pane calls this once per
 * 1 KiB kernel read (~100k/s).
 *
 * Deliberately does NOT count toward the resume-watches fast path: no dispatch
 * source suspended itself here, so there is nothing to re-arm. */
void platform_wake_event_loop(void);
/* The same, never put off: the loop batches a stream of PTY wakes into one per
 * quantum (LIU_WAKE_QUANTUM_US, 1 ms), and the first bytes after a quiet pty —
 * a program's answer, a key's echo — are not a stream. pty_reader_set_wake_now. */
void platform_wake_event_loop_now(void);
/* True once after platform_wake_event_loop_now: the loop drains the focused
 * pane before its event pass, so the answer goes out first. */
bool platform_take_wake_now(void);
/* Runs `fn` on the main thread as soon as a platform_wake_event_loop_now arrives
 * while the loop is parked in platform_wait_events — before the wait returns,
 * which on macOS happens a little later (AppKit hands the wake event back
 * through -nextEventMatchingMask:). Never anywhere else, never nested. The app
 * passes its early drain. Linux returns from the wait at once and ignores it. */
void platform_set_wake_now_handler(void (*fn)(void));
/* Return false to keep the app running when a quit request arrives. The app
 * uses this guard when documents have unsaved edits. */
void platform_set_quit_guard(bool (*allow)(void));

/* OS memory pressure, as the platform reports it — a signal about the
 * MACHINE, which the process-wide ledger (core/mem_budget.h) cannot see: a
 * process well under its own soft ceiling can still be the one asked to
 * give memory back. macOS delivers it through a DISPATCH_SOURCE_TYPE_MEMORY-
 * PRESSURE source (on a background queue, so the handler only records the
 * level and wakes the loop); Linux has no callback and reads PSI
 * (/proc/pressure/memory) at a bounded interval from the take() below.
 *
 * platform_memory_pressure_take returns the HIGHEST level recorded since the
 * previous call and clears it. Main thread. Nothing heavy happens in the
 * notification itself — the main loop hands the level to
 * mem_budget_reclaim_pressure, which runs the registered reclaimers once. */
typedef enum {
    PLATFORM_MEM_PRESSURE_NONE = 0,
    PLATFORM_MEM_PRESSURE_WARN = 1,
    PLATFORM_MEM_PRESSURE_CRITICAL = 2,
} PlatformMemPressure;
PlatformMemPressure platform_memory_pressure_take(void);
/* Record a memory-pressure level as if the OS had reported it.
 * Available to the memory-pressure demo and diagnostics. */
void platform_memory_pressure_inject(PlatformMemPressure level);

/* Tell the OS that the app is doing work the user started, or that it has
 * stopped. macOS: an NSProcessInfo activity, which keeps the process at its
 * own priorities while its windows are covered, for as long as it is held
 * (the system may still idle-sleep); see core/activity_hold.h for what a
 * covered window costs the program in a pane without it. Elsewhere there is
 * nothing to tell. Idempotent. Main thread. */
void platform_set_user_activity(bool active);
bool platform_user_activity_held(void);

/* Keep the keyboard from every other program, or let it go (macOS Secure
 * Event Input; ui/secure_input.h decides when). Idempotent and balanced: one
 * Enable for one Disable, however often it is called. Never taken under
 * LIU_NO_ACTIVATE — a test window in the background must not take the
 * keyboard from the user's other apps. Elsewhere there is no such thing and
 * it only keeps the flag. Main thread. */
void platform_secure_input_set(bool on);
bool platform_secure_input_held(void);

/* The check mark on the menu bar's "Secure Keyboard Entry" item: the user's
 * switch, not the held state. Called every loop turn; only a change touches
 * the menu. No menu bar of Liu's own on Linux. */
void platform_menu_set_secure_keyboard(bool on);
bool platform_menu_secure_keyboard_checked(void);   /* what the menu shows now */

/* The system's language and region as `language_COUNTRY` ("tr_TR"), or "" when
 * it is not known. The system setting, not the environment: an app opened
 * from Finder has no LANG to read. */
void platform_system_locale(char *out, usize cap);

#ifdef USE_METAL
/* Wake a pending paint from a Metal worker, without manufacturing session
 * I/O or applying the PTY ring's deferred-wake quantum. ANY THREAD. */
void platform_wake_render_loop(void);
#endif

/* Total wake-ups requested since the process started. Monotonic and never
 * reset: several consumers may read it (the bench HUD does) and one that
 * zeroed it would steal the interval from the others — each keeps its own
 * last-seen value and subtracts. Bumped from whichever thread asked for the
 * wake, so the read is atomic too. */
u64  platform_wake_count(void);

/* ---- Display link: the panel's own cadence, not a software model of it ----
 *
 * A software grid derived from platform_window_max_refresh_hz() only models
 * the vblank; the display link follows it. The three rules that make it work
 * are:
 *
 *   1. The callback NEVER draws. It bumps a counter and wakes the loop; the
 *      frame is still built on the main thread where every Terminal/renderer
 *      single-writer invariant already lives.
 *   2. The link runs only while the window is VISIBLE AND FOCUSED — plus, for
 *      Liu, only while something actually wants frames (`wants_frames`).
 *      An idle focused window paints nothing and sleeps between loop turns,
 *      so an always-on link would replace that with 60 (or 120) empty
 *      wake-ups a second.
 *   3. It is bound to the display the window is ON, and re-bound when the
 *      window moves. That is what makes ProMotion work without asking anyone
 *      what the refresh rate is: the callback rate IS the panel rate, at
 *      whatever rate the panel is currently running.
 *
 * platform_window_vsync_ticks() is a monotonic counter, so the caller compares
 * it against the value it last painted at rather than consuming an event.
 * Where there is no such facility (X11), the link never runs and the caller
 * keeps its software grid — see platform_window_max_refresh_hz(). */
void platform_window_display_link_sync(PlatformWindow *w, bool visible,
                                       bool focused, bool wants_frames);
bool platform_window_display_link_running(PlatformWindow *w);
u64  platform_window_vsync_ticks(PlatformWindow *w);
/* Seconds between the most recent two display-link callbacks. Zero until
 * enough ticks arrive; this is a cadence observation, not scanout latency. */
f64  platform_window_display_link_period_sec(PlatformWindow *w);
/* Seconds since a RUNNING link last ticked (or was started, before its first
 * tick); 0 when it is not running. The paint gate uses it to stop waiting on a
 * link that has gone quiet — frame_pacing_link_live(). */
f64  platform_window_display_link_silence_sec(PlatformWindow *w);

/* Returns the number of fd-watch handlers run since the previous call and
 * resets the counter. The main loop uses it to skip session draining when
 * no descriptor became readable and no UI event is pending. */
i32 platform_take_fd_fire_count(void);

/* =========================================================================
 * Clipboard
 * ========================================================================= */

/* What a clipboard write did, so a caller says "Copied to clipboard" only
 * when it was: a selection over the size ceiling, or text that is not valid
 * UTF-8, is refused and the pasteboard still holds whatever the user had
 * copied before. A refusal is decided BEFORE the pasteboard is cleared, so
 * the previous contents survive it. */
typedef enum PlatformClipResult {
    PLATFORM_CLIP_OK = 0,
    PLATFORM_CLIP_TOO_LARGE,   /* over PLATFORM_CLIPBOARD_MAX_BYTES */
    PLATFORM_CLIP_BAD_TEXT,    /* NULL or not valid UTF-8 */
    PLATFORM_CLIP_FAILED,      /* the system refused, or out of memory */
} PlatformClipResult;

/* The largest text a clipboard write accepts, on every platform. */
#define PLATFORM_CLIPBOARD_MAX_BYTES (16u * 1024u * 1024u)

const char        *platform_clipboard_get(void);
PlatformClipResult platform_clipboard_set(const char *text);
/* Write secret text to the clipboard using the same validation as a regular
 * clipboard write. On macOS, mark the item with ConcealedType and clear it
 * after a delay only if the pasteboard still contains this write. */
PlatformClipResult platform_clipboard_set_secret(const char *text, f64 clear_after_sec);
/* PNG bytes are copied; ownership remains with the caller. Main thread only. */
PlatformClipResult platform_clipboard_set_png(const u8 *png, usize length);
#ifndef PLATFORM_MACOS
/* X11's PRIMARY selection: the text of the last selection made, pasted by a
 * middle click. Separate from the clipboard, as every X application keeps it.
 * Same refusals as the clipboard. Main thread only. */
const char        *platform_primary_get(void);
PlatformClipResult platform_primary_set(const char *text);
#endif

/* =========================================================================
 * URL opening (system default handler)
 * ========================================================================= */

void platform_open_url(const char *url);

/* Report unrecoverable failures in a platform-visible dialog because the app
 * may be launched without an accessible stderr. */
void platform_fatal_alert(const char *title, const char *detail);

/* Move a local file or folder to the user's Trash: the delete that can be
 * taken back. `supported` is false where Liu has no Trash to reach; `path`
 * returns false when this one could not be moved there — a volume without a
 * Trash, a permission — and the caller then ASKS before it deletes anything
 * for good. */
bool platform_trash_supported(void);
bool platform_trash_path(const char *path);

/* =========================================================================
 * Native view / key injection (browser WKWebView embed)
 * ========================================================================= */

/* Returns the window content view pointer (SSHView* on macOS, opaque
 * elsewhere). Used to attach native subviews (WKWebView). NULL if unavailable. */
void *platform_window_native_view(PlatformWindow *w);

/* Floating notices above native subviews (WKWebView sits above Metal). When
 * the in-app browser is visible, Metal-drawn notices are covered — call this
 * to present the same message as an NSView on top. Coordinates are AppKit
 * points (origin bottom-left of the content view). alpha is 0..1. On Linux
 * this is a no-op (the GPU notice is never covered). `message` is pre-wrapped
 * to at most three lines using the UI's notification grid. `slot` is its place
 * in the stack (0 = newest, on top); `kind` is a NotificationKind and picks
 * the mark in the tile. A native notice passes clicks through to what is
 * beneath it, so over the page it neither holds nor dismisses on a pointer.
 *
 * The notices belong to `w` and to no other window: show, hide and teardown
 * all act on that window alone. */
#define PLATFORM_TOAST_SLOTS 3
void platform_toast_sync(PlatformWindow *w, i32 slot, i32 kind, const char *message,
                         f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt, f32 alpha,
                         f32 bg_r, f32 bg_g, f32 bg_b,
                         f32 fg_r, f32 fg_g, f32 fg_b,
                         f32 bd_r, f32 bd_g, f32 bd_b,
                         f32 accent_r, f32 accent_g, f32 accent_b,
                         f32 shadow_alpha,
                         const char *font_path, f32 font_px);
/* Hides every slot of `w`, or (trim) the slots from `keep` up. */
void platform_toast_hide(PlatformWindow *w);
void platform_toast_trim(PlatformWindow *w, i32 keep);

/* Enqueue EVENT_KEY_DOWN on the same ring as SSHView. */
void platform_push_key_down(i32 key /* KeyCode */, u32 mods, bool repeat);

/* Liu chrome Cmd subset for page-focused WebView (K14 table). */
bool platform_is_liu_chrome_cmd_key(i32 key, u32 mods);

/* Map NSEvent* → KeyCode / KeyMod (macOS). Linux stubs return 0. */
i32  platform_map_nsevent_key(void *nsevent);
u32  platform_map_nsevent_mods(void *nsevent);

/* Open a local file in the system's default text editor.
 * Path is passed as an argv element, never interpolated into a shell string. */
void platform_open_path(const char *path);

/* macOS: which Option key acts as Alt/Meta for the terminal.
 * `mode` is an OptionAsAlt (0 = off, 1 = left, 2 = right, 3 = both); passed as
 * i32 so platform.h stays independent of config.h. */
void platform_set_option_as_alt(i32 mode);

/* =========================================================================
 * Frame-scoped autorelease pool
 *
 * Liu drives its own event loop instead of handing control to
 * NSApplicationMain, so nothing ever drains the main thread's ROOT autorelease
 * pool. Every Cocoa/Metal/CoreAnimation call that hands back an autoreleased
 * object — the per-frame drawable bookkeeping in particular — would therefore
 * pile up in that root pool for the entire lifetime of the process and never
 * be given back.
 *
 * Wrapping one iteration of the main loop in a pool fixes that at the source.
 * Push at the top of the iteration, pop at the top of the NEXT one (and once
 * more after the loop), so the pair survives `continue` / `goto cleanup`
 * without leaking a pool level.
 *
 * Objects autoreleased inside a frame must NOT be retained across frames —
 * every such site in the tree already copies into storage it owns (see
 * platform_clipboard_get) or retains explicitly (the Metal drawable, command
 * buffer and encoder in renderer_metal.m).
 *
 * No-op on platforms without Objective-C: push returns NULL, pop ignores it.
 * ========================================================================= */

void *platform_autorelease_pool_push(void);
void  platform_autorelease_pool_pop(void *pool);

/* =========================================================================
 * Time
 * ========================================================================= */

f64  platform_time_sec(void);   /* monotonic seconds since init */
/* Host clock at init; native event/presentation times must use this origin. */
f64  platform_time_origin(void);
void platform_sleep_ms(u32 ms);

/* =========================================================================
 * OpenGL loader (minimal — only what Liu needs for GL 3.3 core)
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

/* Tell the platform the painted tab-strip height (in points) of ONE window.
 *
 * macOS keeps THAT window's traffic-light buttons vertically centered on it.
 * X11 reads it as the band in which a double-click counts as the title-bar
 * maximize gesture — the app paints over the title bar, so the WM never sees
 * that click itself.
 *
 * Per window, not global: the strip is TOOLBAR_HEIGHT_PT normally but a short
 * ~30pt drag strip when a window hides both its tab bar and its toolbar icons.
 * A single global would flip on every layout pass once two windows have
 * different chrome settings, leaving the non-key window's lights at the wrong
 * height — and the window-merge drop measures the TARGET window's tab strip
 * with it, so the drop band would be wrong too. */
void platform_window_set_titlebar_height(PlatformWindow *w, f32 points);

/* DEPRECATED window-less bridge, kept only so the tree builds while the one
 * remaining call site (app_update_layout in src/ui/ui.c) still passes no
 * window. It applies to the key window, which is the wrong window whenever a
 * BACKGROUND window re-lays out. Switch that call to
 * platform_window_set_titlebar_height(app->host_window, …) and delete this. */
void platform_set_titlebar_height(f32 points);
void platform_set_dock_badge(const char *text);         /* set dock badge label (NULL/"" to clear) */
/* Linux only (notify-send). On macOS the app's banners are app_post_banner()
 * (ui.c) → the notify module's platform_notify_desktop(); there is no macOS
 * function of this name, so a caller fails to link instead of silently showing
 * nothing. */
#ifndef PLATFORM_MACOS
void platform_post_notification(const char *title, const char *body);
#endif
/* Ask for the user's attention for `w`, the window something happened in: the
 * Dock bounces once on macOS (the request is the app's there, `w` is not
 * read); X11 marks that window (_NET_WM_STATE_DEMANDS_ATTENTION and the
 * urgency hint), which is what the taskbar flashes and what activating the
 * entry raises. NULL = the window that last had the keyboard. */
void platform_request_attention(PlatformWindow *w);
/* Is the app the frontmost application? Safe to call from any thread — macOS
 * mirrors AppKit's activation state into an atomic rather than touching NSApp,
 * because the notify server reads this from its own thread. */
bool platform_is_app_focused(void);

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

/* End whatever window `w`'s input method is composing: the keyboard moved to
 * another place in the window (ui/ime_target.h). What the input method commits
 * for it arrives as CHAR_INPUT marked ime_reset. No-op where the platform
 * keeps no input context of its own (macOS). */
void platform_ime_reset(PlatformWindow *w);

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

/* -------------------------------------------------------------------------
 * Live PCM capture (speech input)
 *
 * Separate from platform_audio_record_start above, which encodes AAC into a
 * file for the notification-sound recorder. Speech recognition needs the raw
 * samples at the rate the model was trained on, and the UI needs a level to
 * draw while the user is still speaking — neither is reachable through a
 * finished .m4a.
 *
 * Samples arrive as 16 kHz mono f32 in [-1, 1] regardless of the input
 * device's native format; the backend resamples. Both share the same
 * microphone-permission plumbing, and only one of the two may run at a time.
 * ------------------------------------------------------------------------- */

/* Sample rate every capture backend delivers. */
#define PLATFORM_CAPTURE_RATE 16000

/* Begin capturing. Requests microphone permission if undetermined, in which
 * case capture starts once the user grants it (and platform_audio_capturing()
 * stays false until then). Returns false on hard failure: permission denied,
 * a capture or recording already running, or no input device. */
bool platform_audio_capture_start(void);

/* Stop capturing and release the input device. Returns true if a capture was
 * active or pending. Samples not yet read are dropped. */
bool platform_audio_capture_stop(void);

/* True while samples are actually being delivered. */
bool platform_audio_capturing(void);

/* Move up to `max_samples` newly captured samples into `out` and return how
 * many were written. Non-blocking; the caller is expected to drain once per
 * frame. Samples that overflow the backend's ring between two calls are
 * dropped rather than blocking the audio thread — see
 * platform_audio_capture_overflowed(). */
i32 platform_audio_capture_read(f32 *out, i32 max_samples);

/* True (and cleared) if any samples were dropped since the last call. A
 * caller that sees this has a gap in its audio and should say so rather than
 * transcribe a recording with a hole in it. */
bool platform_audio_capture_overflowed(void);

/* Smoothed 0..1 level of the most recent audio, for a live meter. Returns 0
 * when not capturing. */
f32 platform_audio_capture_level(void);

/* -------------------------------------------------------------------------
 * Short sounds the app makes itself (the dictation cues)
 *
 * A clip is handed over once and played any number of times: the player is
 * built and primed at `set`, so `play` is not the call that wakes the audio
 * hardware — a cue that arrives a quarter of a second after the key is a cue
 * for something else. Output only, the default device, main thread.
 *
 * `set` copies the samples (mono, -1..1) and returns false when the platform
 * has no player (Linux) or the slot is out of range; `play` answers
 * whether the output took it (false for an empty slot). `gain` is 0..1.
 * Playing a slot that is still sounding starts it again from the top. */
#define PLATFORM_AUDIO_CLIPS 4
bool platform_audio_clip_set(i32 slot, const f32 *mono, i32 frames, i32 sample_rate);
bool platform_audio_clip_play(i32 slot, f32 gain);

/* -------------------------------------------------------------------------
 * Input-device selection
 *
 * Capture follows the system default input unless one is pinned here. Which
 * matters more than it sounds: the default is whatever macOS last decided, so
 * plugging in headphones, joining a call or docking a monitor with a webcam
 * silently moves dictation to a different microphone — and the failure is
 * invisible, because a wrong-but-working input still produces a level and a
 * waveform, just a transcript of the wrong room.
 *
 * Devices are identified by their persistent UID, not by index or name: index
 * changes as devices come and go, and name is not unique (two identical USB
 * mics report the same string). A pinned device that is absent falls back to
 * the default rather than failing to record.
 * ------------------------------------------------------------------------- */

#define PLATFORM_AUDIO_DEV_UID_CAP  128
#define PLATFORM_AUDIO_DEV_NAME_CAP 96

typedef struct {
    char uid[PLATFORM_AUDIO_DEV_UID_CAP];
    char name[PLATFORM_AUDIO_DEV_NAME_CAP];
    bool is_default;      /* this is what the system would pick on its own */
} PlatformAudioDevice;

/* List the input devices, newest system state each call (there is no cache to
 * go stale, and enumerating a handful of devices is a few CoreAudio queries).
 * Returns how many entries were written, at most `max`. */
i32 platform_audio_input_devices(PlatformAudioDevice *out, i32 max);

/* Pin capture to `uid`; NULL or "" restores the system default. Takes effect
 * on the next platform_audio_capture_start() — a device cannot be swapped
 * under a running engine without a glitch, and dictation is short. */
void platform_audio_set_input_device(const char *uid);

/* The UID currently pinned, or "" for the system default. */
const char *platform_audio_input_device(void);

#endif /* PLATFORM_H */
