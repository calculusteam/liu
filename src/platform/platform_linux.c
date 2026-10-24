/*
 * Liu - Linux platform layer (X11 + GLX)
 */
#ifdef PLATFORM_LINUX

#include "platform/platform.h"
#include "platform/xdnd_uri.h"
#include "platform/event_payload.h"   /* a queued drop OWNS its path */
#include "platform/clip_request.h"    /* one clipboard request, its own answer */
#include "platform/clipboard_png.h"
#include "platform/x_time_map.h"
#include "platform/xim_commit.h"
#include "platform/event_queue.h"
#include <stdatomic.h>
#include "platform/watch_hold.h"   /* project KeyCode enum — must precede X11 */
#include "core/checked.h"
#include "core/utf8.h"             /* utf8_validate: clipboard text is UTF8_STRING */
#include "platform/xdg_trash.h"       /* the desktop's Trash */
#include "platform/x11_monitor.h"     /* a point's monitor and visible frame */
#include "platform/open_url_policy.h"
#include "platform/atspi_bridge.h" /* screen readers: the AT-SPI2 bridge */
#include "platform/key_mods.h"     /* a key event's modifiers after it */
#include "platform/x11_keypos.h"   /* a chord's key on any layout */
#include "platform/xim_preedit.h"  /* an on-the-spot composition's text */

/* X11 typedefs `KeyCode` (unsigned char), which collides with platform.h's
 * KeyCode enum. Rename X11's to XKeyCode across the X11 headers; the body
 * passes keycodes to Xlib as XKeyCode and keeps KeyCode for Liu's own enum. */
#define KeyCode XKeyCode
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>        /* XK_* for map_keysym */
#include <X11/cursorfont.h>    /* XC_* cursor shapes */
#include <X11/Xresource.h>     /* Xrm* for Xft.dpi */
#ifdef LIU_HAVE_XRANDR
#include <X11/extensions/Xrandr.h>   /* per-monitor refresh rate */
#endif
#ifdef LIU_HAVE_XINPUT2
#include <X11/extensions/XInput2.h>  /* smooth scroll valuators */
#endif
#undef KeyCode
#ifdef LIU_HAVE_DBUS
#include <dbus/dbus.h>   /* taskbar badge signal */
#endif
#define KeyCode XKeyCode
#undef KeyCode

#include <GL/gl.h>
#include <GL/glx.h>
#include "renderer/gl_loader.h"   /* after <GL/gl.h>: its typedefs use GL types */
#include "stb_image.h"         /* _NET_WM_ICON: decode assets/appicon.png */
#include <locale.h>            /* setlocale — required for XIM to open */
#include <time.h>
#include <stdio.h>             /* fprintf — XIM / gl_loader diagnostics */
#include <string.h>
#include <stdlib.h>
#include <wchar.h>             /* mbrtowc — an on-the-spot preedit's multi-byte text */
#include <math.h>
#include <errno.h>
#include <sys/wait.h>
/* kill()/SIGTERM explicitly, rather than through <sys/wait.h>: glibc pulls
 * <signal.h> in from there and musl does not, so the recorder and capture paths
 * below must not rely on one libc's include graph. */
#include <signal.h>
#include <sys/stat.h>
#include <sys/inotify.h>       /* platform_watch_file: config hot-reload */
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <limits.h>
#include <ctype.h>

#define PLATFORM_MAX_CLIPBOARD_BYTES PLATFORM_CLIPBOARD_MAX_BYTES
/* PNG ownership and INCR transfers are main-thread-only. Transfers retain
 * immutable blobs, so replacing the clipboard cannot invalidate a paste. */
#define LINUX_PNG_TRANSFERS 4
typedef struct LinuxPngBlob { u32 refs; usize length; u8 bytes[]; } LinuxPngBlob;
typedef struct { Window requestor; long original_mask; u32 refs; } LinuxPngWatch;
typedef struct {
    Window requestor;
    Atom property;
    LinuxPngBlob *blob;
    usize offset;
    f64 deadline;
} LinuxPngTransfer;

#define PLATFORM_MAX_WATCH_FDS 65536

/* _NET_WM_STATE ClientMessage actions (EWMH §_NET_WM_STATE). */
#define LIU_NET_WM_STATE_REMOVE 0
#define LIU_NET_WM_STATE_ADD    1
#define LIU_NET_WM_STATE_TOGGLE 2

/* _NET_WM_MOVERESIZE direction for a title-bar drag (EWMH). */
#define LIU_NET_WM_MOVERESIZE_MOVE 8

/* glXCreateContextAttribsARB isn't in <GL/glx.h>'s stable ABI surface; declare
 * the prototype + enum tokens and resolve the entrypoint at runtime. */
#ifndef GLX_CONTEXT_MAJOR_VERSION_ARB
#define GLX_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB    0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB     0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif
typedef GLXContext (*PFN_glXCreateContextAttribsARB)(
    Display *, GLXFBConfig, GLXContext, Bool, const int *);

/* Adapter: gl_loader_init wants char*->void*; glXGetProcAddressARB is GLubyte*. */
static void *liu_glx_getproc(const char *n) {
    return (void *)glXGetProcAddressARB((const GLubyte *)n);
}

static void linux_spawn_detached(char *const argv[]);   /* fwd */
static bool linux_which(const char *name, char *out, usize cap);   /* fwd */
/* OUTSIDE the XInput2 guard below, because platform_init() calls it
 * unconditionally: a declaration only inside that guard would leave the call an
 * implicit declaration (an error in C99 and later) when XInput2 is unavailable,
 * and the non-XInput2 configuration would not build. The definition is further
 * down and does not depend on XI2 either — it reads the desktop's
 * lines-per-notch setting. */
static void wheel_lines_init(void);                     /* fwd */
#ifdef LIU_HAVE_XINPUT2
static void xi2_init(Display *dpy);                     /* fwd */
static void xi2_select_window(Display *dpy, Window w);  /* fwd */

#define XI2_MAX_AXES 32
#define XI2_MIN_PX_PER_NOTCH 10.0

typedef struct {
    int    deviceid;
    int    number;      /* valuator index within the device */
    int    type;        /* XIScrollTypeVertical | XIScrollTypeHorizontal */
    double increment;   /* valuator units per wheel notch */
    double last;
    bool   have_last;   /* first event only establishes the baseline */
} Xi2ScrollAxis;

static struct {
    bool          available;
    int           opcode;
    Xi2ScrollAxis axes[XI2_MAX_AXES];
    int           axis_count;
} g_xi2;
#endif

static void linux_spawn_opener(const char *arg) {
    if (!arg || !arg[0]) return;
    /* Resolve xdg-open explicitly rather than trusting PATH: this runs with
     * whatever environment the desktop handed the process, and a URL is
     * attacker-influenced input in the OSC-8 / clickable-link path. */
    const char *opener = access("/usr/bin/xdg-open", X_OK) == 0
                       ? "/usr/bin/xdg-open"
                       : access("/bin/xdg-open", X_OK) == 0
                       ? "/bin/xdg-open" : NULL;
    if (!opener) return;
    char *argv[] = { (char *)opener, (char *)arg, NULL };
    linux_spawn_detached(argv);
}

static struct {
    Display    *display;
    i32         screen;
    bool        initialized;
    struct timespec time_base;
    int         wake_pipe[2];
    pthread_mutex_t watch_lock;
    int        *watch_fds;
    i32         watch_count;
    i32         watch_cap;

    /* Wait-path scratch (main thread only): persistent pollfd buffer, grown
     * geometrically, plus the watched-fd readiness accumulator drained by
     * platform_take_fd_fire_count(). The accumulator is ATOMIC because
     * platform_wake_event_loop() bumps it from PTY reader threads; the poll()
     * wait and the take are still main-thread only, which is why those two use
     * relaxed ordering. */
    struct pollfd *poll_fds;
    i32         poll_cap;
    _Atomic i32 fd_fire_count;

    XIM         xim;            /* input method (connection-scoped); NULL if none */

    /* Clipboard (X11 CLIPBOARD selection): Liu caches its copy and answers
     * SelectionRequest from it; _get pumps for SelectionNotify into clip_req. */
    Atom        a_clipboard, a_targets, a_utf8, a_text, a_incr, a_png;
    /* The properties a clipboard read asks the owner to fill — one per request,
     * rotating, so a reply names the request it answers (clip_request.h). */
    Atom        a_liu_sel[CLIP_REQ_SLOTS];
    LinuxPngBlob *clip_png;
    LinuxPngTransfer png_transfers[LINUX_PNG_TRANSFERS];
    LinuxPngWatch png_watches[LINUX_PNG_TRANSFERS];
    /* The property the server-time round trip touches (see linux_xtime_*). */
    Atom        a_liu_time_ping;
    /* An unmapped window of Liu's own that no window closing takes with it:
     * it owns the selections, receives the replies to clipboard reads and
     * carries the server-time ping. If a terminal window owned CLIPBOARD,
     * closing that window would empty the clipboard. */
    Window      helper;
    char       *clip_text;      /* what Liu currently owns on CLIPBOARD (malloc'd) */
    /* clip_text is a secret (platform_clipboard_set_secret): it is offered with
     * the x-kde-passwordManagerHint target that clipboard histories skip, and
     * given up at clip_secret_until (0 = no timer) if it is still ours. */
    bool        clip_secret;
    f64         clip_secret_until;
    Atom        a_kde_pw_hint;
    /* What Liu owns on PRIMARY: the text of the last selection made, which
     * a middle click pastes (platform_primary_*). */
    char       *primary_text;
    Atom        clip_req_selection;  /* the selection the read in flight asked */
    ClipRequest clip_req;       /* the read in flight or just finished; _get
                                 * returns ITS text, valid until the next read */

    /* EWMH / ICCCM atoms interned once at init. The window-manager protocol is
     * the only way to reach the affordances AppKit gives macOS for free —
     * fullscreen state, urgency, stacking, work area, per-window opacity. */
    Atom        a_net_wm_state, a_net_wm_state_fullscreen,
                a_net_wm_state_hidden, a_net_wm_state_demands_attention,
                a_net_wm_state_above, a_net_wm_state_skip_taskbar,
                a_net_wm_state_skip_pager, a_net_wm_state_max_vert,
                a_net_wm_state_max_horz,
                a_net_active_window, a_net_wm_pid, a_net_wm_icon,
                a_net_wm_name, a_net_wm_window_opacity, a_net_wm_moveresize,
                a_net_workarea, a_net_current_desktop, a_net_frame_extents,
                a_net_supported, a_wm_state, a_wm_protocols, a_net_wm_ping;

    /* XDND (drag and drop). X11 has no built-in drop support — the whole
     * handshake is ClientMessages between the source and the target window,
     * so every step below is spelled out. It is what lets a file dropped on
     * the terminal paste its path, as on macOS. */
    Atom        a_xdnd_aware, a_xdnd_enter, a_xdnd_position, a_xdnd_status,
                a_xdnd_leave, a_xdnd_drop, a_xdnd_finished, a_xdnd_selection,
                a_xdnd_type_list, a_xdnd_action_copy, a_uri_list, a_xdnd_prop;
    /* Cached Xft.dpi/96; 0 = not read yet. Invalidated by the root
     * RESOURCE_MANAGER PropertyNotify, which also raises EVENT_DPI_CHANGE. */
    f32         dpi_scale;

    Window      xdnd_source;      /* None when no drag is in flight */
    long        xdnd_version;
    i32         xdnd_x, xdnd_y;   /* last position, window-relative px */
    bool        xdnd_inside;      /* DRAG_ENTER emitted, EXIT still owed */

    /* Global hotkey (Quake). X11 has no per-application hotkey API — the
     * key is grabbed with XGrabKey on the root window, so the grab must be
     * re-issued for every lock-modifier permutation (Num/Caps/Scroll). */
    unsigned int hotkey_keycode;       /* 0 = no hotkey registered */
    unsigned int hotkey_modmask;
    GlobalHotkeyCallback hotkey_cb;
    void       *hotkey_user;

    /* Config hot-reload. Editors save by writing a temp file and rename()ing it
     * over the target, which detaches any watch held on the original inode — so
     * the PARENT DIRECTORY is watched and events are filtered on the basename
     * instead. */
    int         inotify_fd;     /* -1 when idle */
    int         inotify_wd;
    char        watch_path[PATH_MAX];
    char        watch_base[NAME_MAX + 1];
    FileWatchCallback watch_cb;
    void       *watch_user;

    Cursor      cur[5];         /* XCreateFontCursor cache, indexed by CursorType */
    bool        cur_init;
    /* The resize cursors of a window Liu draws the frame of, indexed by the
     * _NET_WM_MOVERESIZE direction (0 = top-left, clockwise to 7 = left). */
    Cursor      edge_cur[8];
    bool        edge_cur_init;
} g_platform = {0};

/* Shared with the macOS backend: the overflow policy (which events may be
 * folded, which must never be dropped) lives in platform/event_queue.h so the
 * two cannot drift. */
static PlatformEventQueue g_evq;
/* Strings queued events point at (dropped paths): owned from push until the
 * event has been handled, or discarded by the queue. See event_payload.h. */
static PlatformPayloads g_payloads;

/* Every window this process has open, in creation order. X11 hands each event
 * to the window it happened in: linux_dispatch_xevent() finds its
 * PlatformWindow here and push_event() stamps it on the event, which is how the
 * app routes it to that window's state (PlatformEvent.window). */
#define LINUX_MAX_WINDOWS 16
static PlatformWindow *g_windows[LINUX_MAX_WINDOWS];
static i32             g_window_count;
/* The window with the keyboard (its last FocusIn), or the first one before any
 * has had it. Which window is GL-current is another question entirely
 * (platform_make_current): the loop makes every window current in turn. */
static PlatformWindow *g_key_window = NULL;
/* The window the last pointer event came from: the cursor shape is set there. */
static PlatformWindow *g_pointer_window = NULL;
/* The window whose X event is being dispatched; push_event() stamps it. */
static PlatformWindow *g_dispatch_window = NULL;
static void linux_dispatch_xevent(XEvent *xep); /* fwd: reused by clipboard pump */
static void linux_clip_secret_tick(f64 now);    /* fwd: the poll runs it */

/* Convert the X server event timestamp to the application's monotonic clock.
 * Mark timestamps uncertain when no recent calibration can validate the
 * mapping. */
static XTimeMap g_xtime;
static struct {
    bool pending;
    f64  t_send;          /* app clock just before XChangeProperty */
    f64  deadline;        /* give up waiting for the notify after this */
    u32  sent, landed, timed_out;
} g_xtime_ping;

/* LIU_XTIME_PROBE=1 prints every key stamp mapped and every calibration, so a
 * run on a real X server can show the mapping doing what the header claims —
 * including the queue wait of a stalled main thread — without the HUD. */
static bool linux_xtime_probe_on(void) {
    static i32 v = -1;
    if (v < 0) {
        const char *e = getenv("LIU_XTIME_PROBE");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v == 1;
}

static bool linux_xim_probe_on(void) {
    static i32 v = -1;
    if (v < 0) {
        const char *e = getenv("LIU_XIM_PROBE");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v == 1;
}

static f64 linux_map_x_time(Time t, bool *out_certain) {
    f64  now = platform_time_sec();
    bool certain = false;
    f64  mapped = x_time_map_ex(&g_xtime, (u32)t, now, &certain);
    if (out_certain) *out_certain = certain;
    if (linux_xtime_probe_on())
        fprintf(stderr, "[xtime] key server_ms=%u now=%.6f mapped=%.6f "
                        "queue_wait_ms=%.3f certain=%d cal_age=%.3f\n",
                (unsigned)(u32)t, now, mapped, (now - mapped) * 1000.0,
                certain ? 1 : 0, g_xtime.calibrated ? now - g_xtime.cal_at : -1.0);
    return mapped;
}

static void push_event(PlatformEvent e) {
    if (e.timestamp <= 0) e.timestamp = platform_time_sec();
    if (!e.window) e.window = g_dispatch_window;
    platform_event_queue_push(&g_evq, e);
}

static bool linux_setup_pipe_fd(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;
    flags = fcntl(fd, F_GETFD, 0);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

/* Set while a wake byte is in flight; cleared by linux_drain_wake_pipe(). */
static _Atomic bool g_wake_pending = false;

static void linux_wake_waiter(void) {
    if (g_platform.wake_pipe[1] < 0) return;
    /* Coalesce: a PTY reader thread can call this once per kernel read, so
     * only the first call since the last drain writes a byte. */
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &g_wake_pending, &expected, true,
            memory_order_acquire, memory_order_relaxed)) return;
    u8 b = 1;
    (void)write(g_platform.wake_pipe[1], &b, 1);
}

/* See platform.h: monotonic, read by observers that subtract their own last
 * snapshot. */
static _Atomic unsigned long long g_wake_total = 0;

u64 platform_wake_count(void) {
    return (u64)atomic_load_explicit(&g_wake_total, memory_order_relaxed);
}

void platform_set_quit_guard(bool (*allow)(void)) { (void)allow; }

void platform_wake_event_loop(void) {
    atomic_fetch_add_explicit(&g_wake_total, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_platform.fd_fire_count, 1, memory_order_release);
    linux_wake_waiter();
}

/* X11 has no batching quantum to skip, and no event pass worth going around. */
void platform_wake_event_loop_now(void) { platform_wake_event_loop(); }
bool platform_take_wake_now(void) { return false; }
void platform_set_wake_now_handler(void (*fn)(void)) { (void)fn; }

static void linux_drain_wake_pipe(void) {
    if (g_platform.wake_pipe[0] < 0) return;
    u8 buf[64];
    while (read(g_platform.wake_pipe[0], buf, sizeof(buf)) > 0) {}
    atomic_store_explicit(&g_wake_pending, false, memory_order_release);
}

struct PlatformWindow {
    Window      xwindow;
    GLXContext  glx_context;
    Atom        wm_delete;
    i32         width, height;
    bool        should_close;
    XIC         xic;            /* per-window input context; NULL = no XIM */
    bool        xic_over_the_spot;  /* IC negotiated XIMPreeditPosition */
    bool        xic_on_the_spot;    /* IC negotiated XIMPreeditCallbacks */
    /* The composition an on-the-spot IM is building, as code points, and its
     * caret; mirrored to the app as EVENT_IME_COMPOSE on every change. */
    u32         pre_cp[256];
    i32         pre_len, pre_caret;
    f64         pre_escape_at;  /* an Escape the IM kept, unanswered (xim_preedit.h) */
    XPoint      xic_spot;       /* last spot pushed, to skip no-op round trips */
    /* platform_ime_reset ran and no keystroke has come since: input-method
     * commits until then are the ended composition's (char_input.ime_reset). */
    bool        ime_reset_pending;

    /* Mirrors of window-manager state, refreshed from PropertyNotify on
     * _NET_WM_STATE / WM_STATE rather than round-tripped per query — the
     * render gate calls platform_window_visible() every frame. */
    bool        fullscreen;
    bool        mapped;         /* WM_STATE == NormalState (not iconified) */
    bool        obscured;       /* VisibilityFullyObscured */
    bool        argb_visual;    /* 32-bit visual: per-pixel alpha is real */
    bool        translucent;    /* anything shows through? (see set_transparent) */
    f32         blur_radius;    /* last requested; re-applied when the above flips */

    /* Last non-zero _NET_FRAME_EXTENTS. The WM only publishes the property
     * for a mapped, decorated window, so a query made while the Quake window
     * is hidden reads zeros and the next placement would size the frame one
     * title-bar too tall. Remembering the last real answer keeps show/hide
     * cycles stable. */
    i32         frame_l, frame_r, frame_t, frame_b;

    /* Quake (drop-down) mode. */
    bool        quake;
    bool        quake_shown;
    f32         quake_height_ratio;
    i32         saved_x, saved_y, saved_w, saved_h;  /* geometry before quake */

    bool        focused;        /* between its FocusIn and FocusOut */

    /* Liu draws this window's frame: no window-manager title bar or borders
     * (x11_set_borderless), its own minimize / maximize / close, and resize
     * from a band along the edges. */
    bool        borderless;
    bool        maximized;      /* _NET_WM_STATE has both MAXIMIZED atoms */
    i32         edge_dir;       /* resize edge under the pointer, -1 = none */
    i32         app_cursor;     /* the shape the app last asked for */
    /* A press on the title strip that becomes a move once the pointer has
     * travelled (platform_begin_window_drag), in root pixels. */
    bool        move_pending;
    i32         move_x, move_y;

    /* This window's frame on the accessibility bus (atspi_bridge.h). */
    AtspiWindow a11y;
};

/* The PlatformWindow an X window id belongs to; NULL for the root, the helper
 * and anything that is not ours. */
static PlatformWindow *linux_window_of(Window xid) {
    if (xid == None) return NULL;
    for (i32 i = 0; i < g_window_count; i++)
        if (g_windows[i]->xwindow == xid) return g_windows[i];
    return NULL;
}

/* Xlib's default error handler calls exit(1) on ANY protocol error, which
 * would take every running shell down over a BadWindow from a property write
 * that raced the window manager, or a BadAccess because another client
 * already owns the Quake hotkey. Log once per error and keep going: every
 * request Liu makes is advisory. */
static int linux_x_error_handler(Display *dpy, XErrorEvent *e) {
    char text[256];
    XGetErrorText(dpy, e->error_code, text, (int)sizeof text);
    fprintf(stderr, "liu: X11 error %s (request %u.%u, resource 0x%lx)\n",
            text, (unsigned)e->request_code, (unsigned)e->minor_code,
            (unsigned long)e->resourceid);
    return 0;
}

/* A fatal I/O error means the X connection is gone (server exited, session
 * ended). Xlib never returns from this handler, so there is nothing to clean
 * up beyond leaving a breadcrumb — returning would be undefined behaviour. */
static int linux_x_io_error_handler(Display *dpy) {
    (void)dpy;
    fprintf(stderr, "liu: X11 connection lost\n");
    _exit(1);
}

/* ---- OS memory pressure (see platform.h) ------------------------------
 *
 * No callback on Linux without a cgroup v2 memory.events watcher, so PSI is
 * READ instead: /proc/pressure/memory's "some avg10" (share of the last
 * 10 s in which at least one task stalled on memory) is WARN past 10%, and
 * "full avg10" (every task stalled) is CRITICAL past 5%. Sampled at most
 * every PRESSURE_POLL_SEC from take(), so the cost is one small read every
 * couple of seconds; a kernel without PSI (or a container that hides it)
 * reads as no pressure. An injected level takes precedence, as on macOS. */
#define PRESSURE_POLL_SEC 2.0
static _Atomic int g_mem_pressure_level = 0;
static f64 g_mem_pressure_polled_at = -1e300;

void platform_memory_pressure_inject(PlatformMemPressure level) {
    int want = (int)level, have = atomic_load_explicit(&g_mem_pressure_level, memory_order_relaxed);
    while (want > have &&
           !atomic_compare_exchange_weak_explicit(&g_mem_pressure_level, &have, want,
                                                  memory_order_acq_rel, memory_order_relaxed)) {}
    platform_wake_event_loop();
}

/* Nothing naps a process here; the flag is kept so both platforms answer the
 * same question (platform.h). */
static bool g_user_activity = false;
void platform_set_user_activity(bool active) { g_user_activity = active; }
bool platform_user_activity_held(void) { return g_user_activity; }

/* X11 has no Secure Event Input: any client can read the keyboard. Nothing is
 * ever HELD, and the toolbar's chip (shown exactly while this says true) must
 * not claim a protection that is not there. */
void platform_secure_input_set(bool on) { (void)on; }
bool platform_secure_input_held(void) { return false; }
void platform_menu_set_secure_keyboard(bool on) { (void)on; }
bool platform_menu_secure_keyboard_checked(void) { return false; }
/* On X11 the session's own environment names the locale; there is no second
 * place to ask. */
void platform_system_locale(char *out, usize cap) { if (out && cap) out[0] = '\0'; }

static PlatformMemPressure linux_psi_level(void) {
    FILE *f = fopen("/proc/pressure/memory", "r");
    if (!f) return PLATFORM_MEM_PRESSURE_NONE;
    char line[256];
    PlatformMemPressure level = PLATFORM_MEM_PRESSURE_NONE;
    while (fgets(line, sizeof line, f)) {
        double avg10 = 0.0;
        if (sscanf(line, "some avg10=%lf", &avg10) == 1 && avg10 > 10.0 &&
            level < PLATFORM_MEM_PRESSURE_WARN)
            level = PLATFORM_MEM_PRESSURE_WARN;
        if (sscanf(line, "full avg10=%lf", &avg10) == 1 && avg10 > 5.0)
            level = PLATFORM_MEM_PRESSURE_CRITICAL;
    }
    fclose(f);
    return level;
}

PlatformMemPressure platform_memory_pressure_take(void) {
    int level = atomic_exchange_explicit(&g_mem_pressure_level, 0, memory_order_acq_rel);
    f64 now = platform_time_sec();
    if (now - g_mem_pressure_polled_at >= PRESSURE_POLL_SEC) {
        g_mem_pressure_polled_at = now;
        PlatformMemPressure psi = linux_psi_level();
        if ((int)psi > level) level = (int)psi;
    }
    return (PlatformMemPressure)level;
}

bool platform_init(void) {
    if (g_platform.initialized) return true;
    g_platform.wake_pipe[0] = -1;
    g_platform.wake_pipe[1] = -1;
    XSetErrorHandler(linux_x_error_handler);
    XSetIOErrorHandler(linux_x_io_error_handler);
    g_platform.display = XOpenDisplay(NULL);
    if (!g_platform.display) return false;
    g_platform.screen = DefaultScreen(g_platform.display);
    clock_gettime(CLOCK_MONOTONIC, &g_platform.time_base);
    pthread_mutex_init(&g_platform.watch_lock, NULL);
    if (pipe(g_platform.wake_pipe) == 0) {
        if (!linux_setup_pipe_fd(g_platform.wake_pipe[0]) ||
            !linux_setup_pipe_fd(g_platform.wake_pipe[1])) {
            close(g_platform.wake_pipe[0]);
            close(g_platform.wake_pipe[1]);
            g_platform.wake_pipe[0] = g_platform.wake_pipe[1] = -1;
        }
    }

    /* Input method (XIM) for correct multi-byte / dead-key / compose input.
     * Needs a locale + X locale-modifier support; failure is non-fatal (input
     * falls back to XLookupString without an input context). */
    if (!setlocale(LC_CTYPE, "")) setlocale(LC_CTYPE, "C");
    if (XSupportsLocale()) {
        XSetLocaleModifiers("");   /* honor XMODIFIERS from the environment */
        g_platform.xim = XOpenIM(g_platform.display, NULL, NULL, NULL);
    }
    if (!g_platform.xim)
        fprintf(stderr, "liu: XIM unavailable; falling back to XLookupString "
                        "(Latin-1 keys only)\n");

    /* Intern clipboard atoms once (reused per SelectionRequest). */
    Display *d = g_platform.display;
    g_platform.a_clipboard = XInternAtom(d, "CLIPBOARD", False);
    g_platform.a_targets   = XInternAtom(d, "TARGETS", False);
    g_platform.a_png       = XInternAtom(d, "image/png", False);
    g_platform.a_utf8      = XInternAtom(d, "UTF8_STRING", False);
    g_platform.a_text      = XInternAtom(d, "TEXT", False);
    g_platform.a_incr      = XInternAtom(d, "INCR", False);
    g_platform.a_kde_pw_hint = XInternAtom(d, "x-kde-passwordManagerHint", False);
    for (i32 slot = 0; slot < CLIP_REQ_SLOTS; slot++) {
        char name[32];
        snprintf(name, sizeof name, "LIU_SELECTION_%d", slot);
        g_platform.a_liu_sel[slot] = XInternAtom(d, name, False);
    }
    /* No request yet (CLIP_REQ_IDLE): a SelectionNotify arriving now is
     * somebody else's, and the handler drops it. */
    clip_request_reset(&g_platform.clip_req);
    /* An event the queue destroys undelivered gives back what it owns. */
    g_evq.discard = platform_payload_discard;
    g_evq.discard_ctx = &g_payloads;
    g_platform.a_liu_time_ping = XInternAtom(d, "_LIU_TIME_PING", False);

    /* EWMH / ICCCM. False (not True) so the atoms exist even before a
     * conforming WM has published them — a client may legally set properties
     * the current WM ignores, and interning is what makes the query cheap. */
    g_platform.a_net_wm_state       = XInternAtom(d, "_NET_WM_STATE", False);
    g_platform.a_net_wm_state_fullscreen =
        XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
    g_platform.a_net_wm_state_hidden =
        XInternAtom(d, "_NET_WM_STATE_HIDDEN", False);
    g_platform.a_net_wm_state_demands_attention =
        XInternAtom(d, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
    g_platform.a_net_wm_state_above =
        XInternAtom(d, "_NET_WM_STATE_ABOVE", False);
    g_platform.a_net_wm_state_skip_taskbar =
        XInternAtom(d, "_NET_WM_STATE_SKIP_TASKBAR", False);
    g_platform.a_net_wm_state_skip_pager =
        XInternAtom(d, "_NET_WM_STATE_SKIP_PAGER", False);
    g_platform.a_net_wm_state_max_vert =
        XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    g_platform.a_net_wm_state_max_horz =
        XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    g_platform.a_net_active_window  = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
    g_platform.a_net_wm_pid         = XInternAtom(d, "_NET_WM_PID", False);
    g_platform.a_net_wm_icon        = XInternAtom(d, "_NET_WM_ICON", False);
    g_platform.a_net_wm_name        = XInternAtom(d, "_NET_WM_NAME", False);
    g_platform.a_net_wm_window_opacity =
        XInternAtom(d, "_NET_WM_WINDOW_OPACITY", False);
    g_platform.a_net_wm_moveresize  = XInternAtom(d, "_NET_WM_MOVERESIZE", False);
    g_platform.a_net_workarea       = XInternAtom(d, "_NET_WORKAREA", False);
    g_platform.a_net_current_desktop =
        XInternAtom(d, "_NET_CURRENT_DESKTOP", False);
    g_platform.a_net_frame_extents  = XInternAtom(d, "_NET_FRAME_EXTENTS", False);
    g_platform.a_net_supported      = XInternAtom(d, "_NET_SUPPORTED", False);
    g_platform.a_wm_state           = XInternAtom(d, "WM_STATE", False);
    g_platform.a_wm_protocols       = XInternAtom(d, "WM_PROTOCOLS", False);
    g_platform.a_net_wm_ping        = XInternAtom(d, "_NET_WM_PING", False);

    g_platform.a_xdnd_aware       = XInternAtom(d, "XdndAware", False);
    g_platform.a_xdnd_enter       = XInternAtom(d, "XdndEnter", False);
    g_platform.a_xdnd_position    = XInternAtom(d, "XdndPosition", False);
    g_platform.a_xdnd_status      = XInternAtom(d, "XdndStatus", False);
    g_platform.a_xdnd_leave       = XInternAtom(d, "XdndLeave", False);
    g_platform.a_xdnd_drop        = XInternAtom(d, "XdndDrop", False);
    g_platform.a_xdnd_finished    = XInternAtom(d, "XdndFinished", False);
    g_platform.a_xdnd_selection   = XInternAtom(d, "XdndSelection", False);
    g_platform.a_xdnd_type_list   = XInternAtom(d, "XdndTypeList", False);
    g_platform.a_xdnd_action_copy = XInternAtom(d, "XdndActionCopy", False);
    g_platform.a_uri_list         = XInternAtom(d, "text/uri-list", False);
    g_platform.a_xdnd_prop        = XInternAtom(d, "LIU_XDND", False);

    g_platform.inotify_fd = -1;
    g_platform.inotify_wd = -1;

    /* Watch the root window's properties so a desktop-wide Xft.dpi change is
     * seen. X11 has no per-window scale-change notification: RESOURCE_MANAGER
     * is where the setting lives, so watching it is the only way to notice a
     * scale change without restarting.
     *
     * Root property traffic is user-driven (focus, workspace, window list), so
     * an idle desktop produces none; the dispatcher filters by atom. */
    XSelectInput(d, RootWindow(d, g_platform.screen), PropertyChangeMask);

    {
        XSetWindowAttributes hattr = {0};
        hattr.event_mask = PropertyChangeMask;   /* clipboard replies, time ping */
        g_platform.helper = XCreateWindow(d, RootWindow(d, g_platform.screen),
                                          -10, -10, 1, 1, 0, CopyFromParent,
                                          InputOnly, CopyFromParent,
                                          CWEventMask, &hattr);
    }

#ifdef LIU_HAVE_XINPUT2
    xi2_init(d);   /* after the root XSelectInput: it adds its own XI2 mask */
#endif
    wheel_lines_init();

    g_platform.initialized = true;
    return true;
}

/* =========================================================================
 * EWMH helpers
 *
 * Everything the window manager owns — fullscreen, urgency, stacking, work
 * area, per-window opacity — is reached through properties and root-window
 * ClientMessages. These four helpers are the whole protocol surface Liu needs.
 * ========================================================================= */

/* Read a property into caller-XFree()'d storage. Returns false (and leaves the
 * out params untouched) when the property is absent or has the wrong type/format,
 * which is the normal case under a WM that doesn't implement the hint. */
static bool x11_get_prop(Window win, Atom prop, Atom type, int want_format,
                         unsigned long max_items,
                         unsigned char **data_out, unsigned long *count_out) {
    Display *dpy = g_platform.display;
    if (!dpy || win == None || prop == None) return false;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, prop, 0, (long)max_items, False, type,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &data) != Success)
        return false;
    if (!data) return false;
    if (actual_type == None || actual_format != want_format || nitems == 0) {
        XFree(data);
        return false;
    }
    *data_out = data;
    *count_out = nitems;
    return true;
}

/* Ask the WM to act on a window. EWMH requires these to be sent to the ROOT
 * window with SubstructureNotify|SubstructureRedirect, not to the client
 * window: a WM only watches the root for client requests. */
static void x11_send_root_msg(Window win, Atom msg_type,
                              long d0, long d1, long d2, long d3, long d4) {
    Display *dpy = g_platform.display;
    if (!dpy || win == None || msg_type == None) return;
    XEvent ev = {0};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = win;
    ev.xclient.message_type = msg_type;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = d0;
    ev.xclient.data.l[1]    = d1;
    ev.xclient.data.l[2]    = d2;
    ev.xclient.data.l[3]    = d3;
    ev.xclient.data.l[4]    = d4;
    XSendEvent(dpy, RootWindow(dpy, g_platform.screen), False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(dpy);
}

/* Add/remove/toggle up to two _NET_WM_STATE atoms in one request. Source
 * indication 1 = "normal application", which is what a WM expects from a
 * direct user action and what keeps focus-stealing prevention from
 * second-guessing the request. */
static void x11_wm_state(Window win, long action, Atom p1, Atom p2) {
    x11_send_root_msg(win, g_platform.a_net_wm_state,
                      action, (long)p1, (long)p2, 1, 0);
}

/* Is `atom` currently present in the window's _NET_WM_STATE list? */
static bool x11_has_wm_state(Window win, Atom atom) {
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (!x11_get_prop(win, g_platform.a_net_wm_state, XA_ATOM, 32, 64,
                      &data, &n))
        return false;
    const Atom *atoms = (const Atom *)(const void *)data;
    bool found = false;
    for (unsigned long i = 0; i < n; i++) {
        if (atoms[i] == atom) { found = true; break; }
    }
    XFree(data);
    return found;
}

static void linux_png_release(LinuxPngBlob *b) { if(b && --b->refs == 0) free(b); }
static void linux_png_watch_release(Window requestor) {
    for(i32 i=0;i<LINUX_PNG_TRANSFERS;i++) {
        LinuxPngWatch *w=&g_platform.png_watches[i];
        if(!w->refs || w->requestor!=requestor)continue;
        if(--w->refs==0) {
            if(g_platform.display)XSelectInput(g_platform.display,w->requestor,w->original_mask);
            memset(w,0,sizeof *w);
        }
        return;
    }
}
static bool linux_png_watch_acquire(Display *dpy,Window requestor) {
    for(i32 i=0;i<LINUX_PNG_TRANSFERS;i++) {
        LinuxPngWatch *w=&g_platform.png_watches[i];
        if(w->refs && w->requestor==requestor){w->refs++;return true;}
    }
    for(i32 i=0;i<LINUX_PNG_TRANSFERS;i++) {
        LinuxPngWatch *w=&g_platform.png_watches[i];if(w->refs)continue;
        XWindowAttributes attrs;if(!XGetWindowAttributes(dpy,requestor,&attrs))return false;
        *w=(LinuxPngWatch){requestor,attrs.your_event_mask,1};
        XSelectInput(dpy,requestor,attrs.your_event_mask|PropertyChangeMask);
        return true;
    }
    return false;
}
static void linux_png_transfer_clear(LinuxPngTransfer *t) {
    if(t->blob)linux_png_watch_release(t->requestor);
    linux_png_release(t->blob); memset(t,0,sizeof *t);
}
static void linux_png_expire(f64 now) {
    for(i32 i=0;i<LINUX_PNG_TRANSFERS;i++) {
        LinuxPngTransfer *t=&g_platform.png_transfers[i];
        if(t->blob && now >= t->deadline) linux_png_transfer_clear(t);
    }
}
static usize linux_png_chunk_size(Display *dpy) {
    long words=XExtendedMaxRequestSize(dpy);
    if(words<=0)words=XMaxRequestSize(dpy);
    if(words<=64)return 0;
    return MIN((usize)(words-64)*4u,(usize)65536);
}
static bool linux_png_property(Display *dpy,const XPropertyEvent *ev) {
    if(ev->state != PropertyDelete)return false;
    for(i32 i=0;i<LINUX_PNG_TRANSFERS;i++) {
        LinuxPngTransfer *t=&g_platform.png_transfers[i];
        if(!t->blob || t->requestor!=ev->window || t->property!=ev->atom)continue;
        usize n=MIN(linux_png_chunk_size(dpy),t->blob->length-t->offset);
        XChangeProperty(dpy,t->requestor,t->property,g_platform.a_png,8,PropModeReplace,
                        t->blob->bytes+t->offset,(int)n);
        t->offset+=n;t->deadline=platform_time_sec()+15;
        if(!n)linux_png_transfer_clear(t);
        XFlush(dpy);return true;
    }
    return false;
}
static bool linux_png_send(Display *dpy,const XSelectionRequestEvent *req,Atom property) {
    LinuxPngBlob *b=g_platform.clip_png;
    if(!b)return false;
    long words=XExtendedMaxRequestSize(dpy);if(words<=0)words=XMaxRequestSize(dpy);
    usize maximum=words>64?(usize)(words-64)*4u:0;
    if(!maximum)return false;
    if(b->length<=maximum) {
        XChangeProperty(dpy,req->requestor,property,g_platform.a_png,8,PropModeReplace,b->bytes,(int)b->length);
        return true;
    }
    linux_png_expire(platform_time_sec());
    for(i32 i=0;i<LINUX_PNG_TRANSFERS;i++)
        if(g_platform.png_transfers[i].blob && g_platform.png_transfers[i].requestor==req->requestor &&
           g_platform.png_transfers[i].property==property)return false;
    for(i32 i=0;i<LINUX_PNG_TRANSFERS;i++) {
        LinuxPngTransfer *t=&g_platform.png_transfers[i];
        if(t->blob)continue;
        if(!linux_png_watch_acquire(dpy,req->requestor))return false;
        b->refs++;
        *t=(LinuxPngTransfer){req->requestor,property,b,0,platform_time_sec()+15};
        unsigned long length=(unsigned long)b->length;
        XChangeProperty(dpy,req->requestor,property,g_platform.a_incr,32,PropModeReplace,(u8 *)&length,1);
        return true;
    }
    return false;
}

void platform_shutdown(void) {
    atspi_shutdown();
    platform_event_queue_clear(&g_evq);
    platform_payloads_reset(&g_payloads);
    if (g_platform.wake_pipe[0] >= 0) close(g_platform.wake_pipe[0]);
    if (g_platform.wake_pipe[1] >= 0) close(g_platform.wake_pipe[1]);
    g_platform.wake_pipe[0] = -1;
    g_platform.wake_pipe[1] = -1;
    free(g_platform.watch_fds);
    g_platform.watch_fds = NULL;
    g_platform.watch_count = 0;
    g_platform.watch_cap = 0;
    free(g_platform.poll_fds);
    g_platform.poll_fds = NULL;
    g_platform.poll_cap = 0;
    atomic_store_explicit(&g_platform.fd_fire_count, 0, memory_order_relaxed);
    platform_unwatch_file();
    pthread_mutex_destroy(&g_platform.watch_lock);
    if (g_platform.display && g_platform.cur_init) {
        for (int i = 0; i < 5; i++)
            if (g_platform.cur[i]) XFreeCursor(g_platform.display, g_platform.cur[i]);
        g_platform.cur_init = false;
    }
    if (g_platform.xim) { XCloseIM(g_platform.xim); g_platform.xim = NULL; }
    free(g_platform.clip_text); g_platform.clip_text = NULL;
    free(g_platform.primary_text); g_platform.primary_text = NULL;
    linux_png_release(g_platform.clip_png); g_platform.clip_png=NULL;
    for(i32 i=0;i<LINUX_PNG_TRANSFERS;i++)linux_png_transfer_clear(&g_platform.png_transfers[i]);
    clip_request_reset(&g_platform.clip_req);
    if (g_platform.display) XCloseDisplay(g_platform.display);
    g_platform.display = NULL;
    g_platform.initialized = false;
}

/* =========================================================================
 * Window identity (WM_CLASS / _NET_WM_PID / _NET_WM_ICON)
 *
 * Unlike a macOS .app bundle, an X11 client carries no metadata of its own —
 * a bare window shows up in the taskbar as an untitled generic box. These
 * properties are what let a panel group Liu's windows, match the .desktop
 * entry, and show the right icon.
 * ========================================================================= */

/* Locate a file under the installed/dev asset tree. Search order mirrors how
 * the binary can be reached: next to the executable (tarball / AppImage /
 * build dir), then the compiled-in dev path, then the FHS install prefixes. */
static bool linux_asset_path(const char *rel, char *out, usize cap) {
    if (!rel || !out || cap == 0) return false;
    struct stat st;

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            /* build/Liu + ../assets, and <prefix>/bin/liu + ../share/liu/assets */
            const char *rels[] = { "%s/assets/%s", "%s/../assets/%s",
                                   "%s/../share/liu/assets/%s" };
            for (usize i = 0; i < sizeof(rels) / sizeof(rels[0]); i++) {
                int w = snprintf(out, cap, rels[i], exe, rel);
                if (w > 0 && (usize)w < cap && stat(out, &st) == 0 &&
                    S_ISREG(st.st_mode))
                    return true;
            }
        }
    }

#ifdef LIU_ASSETS_DIR
    {
        int w = snprintf(out, cap, "%s/%s", LIU_ASSETS_DIR, rel);
        if (w > 0 && (usize)w < cap && stat(out, &st) == 0 &&
            S_ISREG(st.st_mode))
            return true;
    }
#endif

    static const char *prefixes[] = {
        "/usr/share/liu/assets", "/usr/local/share/liu/assets",
        "/opt/liu/assets",
    };
    for (usize i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        int w = snprintf(out, cap, "%s/%s", prefixes[i], rel);
        if (w > 0 && (usize)w < cap && stat(out, &st) == 0 &&
            S_ISREG(st.st_mode))
            return true;
    }

    out[0] = '\0';
    return false;
}

/* Round the corners of one size x size block of the _NET_WM_ICON payload.
 *
 * appicon.png is a full-bleed opaque square with no alpha channel, so the
 * taskbar, alt-tab switcher and window decoration would all draw a hard
 * rectangle. The in-app copy of the same mark is rounded
 * (app_icon_round_corners in ui/agent_icons.c); this is that mask for the
 * window's own icon, at the same 22.4 % of the side, so the two read as one
 * icon.
 *
 * Applied per SIZE, after the downsample, for the reason the in-app one gives:
 * masking the 1024 px master and then box-filtering it would smear the corner
 * ramp across several output pixels instead of landing one crisp AA edge.
 *
 * Only the alpha moves. Unlike the in-app buffer, _NET_WM_ICON is STRAIGHT
 * ARGB, not premultiplied (Qt reads the property into QImage::Format_ARGB32 and
 * GTK into a GdkPixbuf, both non-premultiplied), so scaling the colour channels
 * alongside it would drag the antialiased ramp toward black on every desktop
 * that reads the spec correctly. */
static void icon_round_corners(long *px, int size) {
    const f32 r = (f32)size * 0.224f;
    if (r < 1.0f) return;                    /* below ~5 px there is no arc */
    const f32 half = (f32)size * 0.5f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            /* Distance outside the rounded rect, measured from the pixel
             * CENTRE so the ramp straddles the true edge. */
            f32 dx = fabsf((f32)x + 0.5f - half) - (half - r);
            f32 dy = fabsf((f32)y + 0.5f - half) - (half - r);
            if (dx <= 0.0f && dy <= 0.0f) continue;   /* deep inside */
            if (dx < 0.0f) dx = 0.0f;
            if (dy < 0.0f) dy = 0.0f;
            f32 d = sqrtf(dx * dx + dy * dy) - r;
            if (d <= -0.5f) continue;                 /* fully covered */
            f32 cov = 0.5f - d;
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;
            long *p = &px[(usize)y * (usize)size + (usize)x];
            u32 argb = (u32)*p;
            u32 a = (u32)((f32)((argb >> 24) & 0xFFu) * cov + 0.5f);
            *p = (long)((a << 24) | (argb & 0x00FFFFFFu));
        }
    }
}

/* Box-downsample `src` (src_w x src_h RGBA8) into `size` x `size` packed
 * 0xAARRGGBB, one pixel per long — the format _NET_WM_ICON wants. A plain box
 * average is enough: the source is a 1024px master and the targets are 32-128px
 * taskbar icons, where the sample grid is dense enough that the difference from
 * a windowed filter is invisible. Returns the number of longs written. */
static usize icon_downsample(long *dst, const u8 *src, int src_w, int src_h,
                             int size) {
    dst[0] = size;
    dst[1] = size;
    long *px = dst + 2;
    for (int y = 0; y < size; y++) {
        int y0 = (int)((i64)y * src_h / size);
        int y1 = (int)((i64)(y + 1) * src_h / size);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > src_h) y1 = src_h;
        for (int x = 0; x < size; x++) {
            int x0 = (int)((i64)x * src_w / size);
            int x1 = (int)((i64)(x + 1) * src_w / size);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > src_w) x1 = src_w;
            u32 r = 0, g = 0, b = 0, a = 0, count = 0;
            for (int sy = y0; sy < y1; sy++) {
                const u8 *row = src + ((usize)sy * (usize)src_w + (usize)x0) * 4;
                for (int sx = x0; sx < x1; sx++, row += 4) {
                    r += row[0]; g += row[1]; b += row[2]; a += row[3];
                    count++;
                }
            }
            if (!count) count = 1;
            px[(usize)y * (usize)size + (usize)x] =
                (long)(((u32)(a / count) << 24) | ((u32)(r / count) << 16) |
                       ((u32)(g / count) << 8)  |  (u32)(b / count));
        }
    }
    icon_round_corners(px, size);
    return 2 + (usize)size * (usize)size;
}

/* Publish assets/appicon.png as _NET_WM_ICON at several sizes so the WM can
 * pick the one matching its panel/alt-tab/decoration scale. Best-effort: a
 * missing or undecodable PNG just leaves the window iconless, exactly as
 * before. */
static void linux_set_window_icon(Window win) {
    char path[PATH_MAX];
    if (!linux_asset_path("appicon.png", path, sizeof path)) return;

    int iw = 0, ih = 0, channels = 0;
    u8 *pixels = stbi_load(path, &iw, &ih, &channels, 4);
    if (!pixels || iw <= 0 || ih <= 0) { stbi_image_free(pixels); return; }

    static const int sizes[] = { 128, 64, 32 };
    usize total = 0;
    for (usize i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        total += 2 + (usize)sizes[i] * (usize)sizes[i];

    /* XChangeProperty with format 32 reads an array of `long`, NOT u32 — on
     * LP64 each element occupies 8 bytes and Xlib narrows them on the wire. */
    long *buf = calloc(total, sizeof(long));
    if (!buf) { stbi_image_free(pixels); return; }

    usize off = 0;
    for (usize i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        off += icon_downsample(buf + off, pixels, iw, ih, sizes[i]);

    XChangeProperty(g_platform.display, win, g_platform.a_net_wm_icon,
                    XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)buf, (int)off);
    free(buf);
    stbi_image_free(pixels);
}

/* True when a compositing manager owns _NET_WM_CM_S<screen>. Only then is an
 * ARGB visual worth taking: without a compositor the alpha channel is never
 * blended and the window renders against uninitialised backing store — i.e.
 * garbage — instead of the desktop. */
/* ---------------------------------------------------------------------------
 * XIM input style
 *
 * XIMPreeditNothing | XIMStatusNothing is the "root window" style: the input
 * method draws its own preedit and candidate list wherever IT decides, because
 * the client never tells it where the text cursor is. For CJK input the
 * candidate list then lands in a corner instead of under the character being
 * typed.
 *
 * XIMPreeditPosition ("over-the-spot") avoids that with no drawing work on
 * Liu's side: the IM still renders the preedit and is handed a coordinate. It
 * does require a font set, so the negotiation can fail two ways and both fall
 * back to root style.
 *
 * The style is picked from the list the server advertises rather than
 * hardcoded, so an IM that only offers the "None" variants still gets an input
 * context.
 * ------------------------------------------------------------------------- */
static int xim_style_rank(XIMStyle s) {
    XIMStyle pre = s & (XIMPreeditCallbacks | XIMPreeditPosition |
                        XIMPreeditNothing | XIMPreeditNone);
    XIMStyle sta = s & (XIMStatusNothing | XIMStatusNone);
    /* Area style asks the client to lay out a preedit AREA of its own, which
     * this backend does not do — refused rather than render nothing. */
    if (!pre || !sta) return 0;
    /* Callbacks (on-the-spot): the composition is handed to Liu and drawn
     * inline at the cursor, in the terminal's own font (xim_preedit_*). */
    if (pre & XIMPreeditCallbacks) return 4;
    if (pre & XIMPreeditPosition) return 3;
    if (pre & XIMPreeditNothing)  return 2;
    return 1;   /* PreeditNone: input works, there is just no preedit at all */
}

static XIMStyle xim_pick_style(XIM im, bool *over_the_spot, bool *on_the_spot) {
    *over_the_spot = false;
    *on_the_spot = false;
    XIMStyles *styles = NULL;
    if (XGetIMValues(im, XNQueryInputStyle, &styles, (void *)NULL) || !styles)
        return XIMPreeditNothing | XIMStatusNothing;

    XIMStyle best = 0;
    int best_rank = 0;
    for (unsigned short i = 0; i < styles->count_styles; i++) {
        XIMStyle s = styles->supported_styles[i];
        int rank = xim_style_rank(s);
        if (linux_xim_probe_on())
            fprintf(stderr, "[xim] style 0x%04lx rank %d\n", (unsigned long)s, rank);
        if (rank > best_rank) { best_rank = rank; best = s; }
    }
    XFree(styles);

    if (!best) return XIMPreeditNothing | XIMStatusNothing;
    *on_the_spot   = (best & XIMPreeditCallbacks) != 0;
    *over_the_spot = !*on_the_spot && (best & XIMPreeditPosition) != 0;
    /* Keep only the one preedit and one status bit we settled on: a style
     * value with extra bits set is not one the server advertised. */
    XIMStyle pre = best & (XIMPreeditCallbacks | XIMPreeditPosition |
                           XIMPreeditNothing | XIMPreeditNone);
    XIMStyle sta = best & (XIMStatusNothing | XIMStatusNone);
    if (pre & XIMPreeditCallbacks) pre = XIMPreeditCallbacks;
    else if (pre & XIMPreeditPosition) pre = XIMPreeditPosition;
    else if (pre & XIMPreeditNothing) pre = XIMPreeditNothing;
    else pre = XIMPreeditNone;
    sta = (sta & XIMStatusNothing) ? XIMStatusNothing : XIMStatusNone;
    return pre | sta;
}

/* One font set for the whole connection; over-the-spot needs it and the IM
 * only uses it to size its own preedit. Core X fonts are not installed
 * everywhere any more, so a NULL here is expected, not an error. */
static XFontSet xim_fontset(Display *dpy) {
    static XFontSet fs = NULL;
    static bool tried = false;
    if (tried) return fs;
    tried = true;
    char **missing = NULL;
    int missing_count = 0;
    char *def = NULL;
    fs = XCreateFontSet(dpy, "-*-*-*-*-*-*-*-*-*-*-*-*-*-*",
                        &missing, &missing_count, &def);
    if (missing) XFreeStringList(missing);
    return fs;
}

static bool linux_compositor_running(void) {
    char sel[32];
    int n = snprintf(sel, sizeof sel, "_NET_WM_CM_S%d", g_platform.screen);
    if (n <= 0 || (usize)n >= sizeof sel) return false;
    Atom a = XInternAtom(g_platform.display, sel, False);
    return a != None &&
           XGetSelectionOwner(g_platform.display, a) != None;
}

/* WM_CLASS + _NET_WM_PID + WM_CLIENT_MACHINE. EWMH requires WM_CLIENT_MACHINE
 * alongside _NET_WM_PID: the pid alone is meaningless to a WM that can't tell
 * whether the client is even on this host, so conforming WMs ignore an
 * unaccompanied _NET_WM_PID (and with it, "force quit" and process grouping). */
static void linux_set_window_identity(Window win) {
    Display *dpy = g_platform.display;

    XClassHint *ch = XAllocClassHint();
    if (ch) {
        /* res_name matches the .desktop file's StartupWMClass / basename
         * convention (lowercase), res_class is the human-facing app name. */
        ch->res_name  = (char *)"liu";
        ch->res_class = (char *)"Liu";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    pid_t pid = getpid();
    long pid_l = (long)pid;
    XChangeProperty(dpy, win, g_platform.a_net_wm_pid, XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *)&pid_l, 1);

    char host[256];
    if (gethostname(host, sizeof host) == 0) {
        host[sizeof host - 1] = '\0';
        XTextProperty tp;
        char *hp = host;
        if (XStringListToTextProperty(&hp, 1, &tp)) {
            XSetWMClientMachine(dpy, win, &tp);
            XFree(tp.value);
        }
    }

    linux_set_window_icon(win);
}

/* No window-manager title bar or borders: Liu's tab strip is where the title
 * bar would be, and a WM title bar above it gave the window two stacked
 * headers. _MOTIF_WM_HINTS is not in any current spec, but it is the one
 * decoration switch every X11 window manager still honours (mutter, KWin,
 * Xfwm, openbox, i3); the struct is five 32-bit values in the WM's layout,
 * declared here rather than pulling in Motif headers. Set before the window is
 * mapped: most window managers read it once, at map time. */
static void x11_set_borderless(Display *dpy, Window win) {
    Atom motif = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    if (motif == None) return;
    struct {
        unsigned long flags, functions, decorations;
        long          input_mode;
        unsigned long status;
    } hints = {0};
    hints.flags = 1UL << 1;          /* MWM_HINTS_DECORATIONS */
    hints.decorations = 0;           /* none */
    XChangeProperty(dpy, win, motif, motif, 32, PropModeReplace,
                    (const unsigned char *)&hints, 5);
}

/* ---- On-the-spot preedit (XIMPreeditCallbacks) ------------------------------
 * The input method tells Liu what is being composed and Liu draws it inline at
 * the cursor (the app's preedit overlay, fed by EVENT_IME_COMPOSE); the IM draws
 * nothing of its own. The four callbacks keep one buffer per window. */
#define LINUX_PREEDIT_MAX ((i32)(sizeof ((PlatformWindow *)0)->pre_cp / sizeof(u32)))

static void linux_preedit_push(PlatformWindow *w) {
    w->pre_escape_at = 0.0;   /* the input method answered */
    u8 utf8[sizeof w->pre_cp + 1];
    usize n = 0;
    for (i32 i = 0; i < w->pre_len; i++) {
        u8 tmp[4];
        u32 k = utf8_encode(w->pre_cp[i], tmp);
        if (k == 0 || n + k >= sizeof utf8) break;
        memcpy(utf8 + n, tmp, k);
        n += k;
    }
    utf8[n] = '\0';
    if (linux_xim_probe_on())
        fprintf(stderr, "[xim] composition \"%s\" caret %d\n", (const char *)utf8, w->pre_caret);
    /* The text belongs to the queued event until the app has handled it. */
    const char *kept = n ? platform_payload_keep(&g_payloads, (const char *)utf8, n) : NULL;
    if (n && !kept) return;
    push_event((PlatformEvent){
        .type = EVENT_IME_COMPOSE, .window = w,
        .ime = { .text = kept, .cursor = w->pre_caret }
    });
}

/* An XIMText as code points: wide characters as they are, a multi-byte string
 * through the locale XIM was opened in. At most `cap`; returns how many. */
static i32 xim_text_codepoints(const XIMText *t, u32 *out, i32 cap) {
    i32 n = 0;
    if (!t || t->length == 0) return 0;
    if (t->encoding_is_wchar) {
        if (!t->string.wide_char) return 0;
        for (unsigned short i = 0; i < t->length && n < cap; i++)
            out[n++] = (u32)t->string.wide_char[i];
        return n;
    }
    const char *mb = t->string.multi_byte;
    if (!mb) return 0;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    usize left = strlen(mb);
    while (left > 0 && n < cap) {
        wchar_t wc = 0;
        usize k = mbrtowc(&wc, mb, left, &st);
        if (k == (usize)-1 || k == (usize)-2) break;
        if (k == 0) break;
        out[n++] = (u32)wc;
        mb += k;
        left -= k;
    }
    return n;
}

static i32 xim_preedit_start(XIC xic, XPointer client, XPointer call) {
    (void)xic; (void)call;
    PlatformWindow *w = (PlatformWindow *)client;
    if (w) w->pre_len = w->pre_caret = 0;
    if (linux_xim_probe_on()) fprintf(stderr, "[xim] preedit start\n");
    return LINUX_PREEDIT_MAX;   /* the longest composition kept */
}

static void xim_preedit_done(XIC xic, XPointer client, XPointer call) {
    (void)xic; (void)call;
    PlatformWindow *w = (PlatformWindow *)client;
    if (!w) return;
    if (linux_xim_probe_on()) fprintf(stderr, "[xim] preedit done\n");
    w->pre_len = w->pre_caret = 0;
    linux_preedit_push(w);
}

static void xim_preedit_draw(XIC xic, XPointer client, XPointer call) {
    (void)xic;
    PlatformWindow *w = (PlatformWindow *)client;
    const XIMPreeditDrawCallbackStruct *d = (const XIMPreeditDrawCallbackStruct *)call;
    if (!w || !d) return;
    if (linux_xim_probe_on())
        fprintf(stderr, "[xim] preedit draw first=%d len=%d text=%s(%d) caret=%d\n",
                d->chg_first, d->chg_length, d->text ? "yes" : "null",
                d->text ? (int)d->text->length : -1, d->caret);
    u32 ins[LINUX_PREEDIT_MAX];
    i32 nins = xim_text_codepoints(d->text, ins, LINUX_PREEDIT_MAX);
    xim_preedit_edit(w->pre_cp, &w->pre_len, LINUX_PREEDIT_MAX,
                     d->chg_first, d->chg_length, ins, nins);
    w->pre_caret = d->caret < 0 ? 0 : d->caret > w->pre_len ? w->pre_len : d->caret;
    linux_preedit_push(w);
}

static void xim_preedit_caret(XIC xic, XPointer client, XPointer call) {
    (void)xic;
    PlatformWindow *w = (PlatformWindow *)client;
    XIMPreeditCaretCallbackStruct *c = (XIMPreeditCaretCallbackStruct *)call;
    if (!w || !c) return;
    switch (c->direction) {
    case XIMAbsolutePosition: w->pre_caret = c->position; break;
    case XIMForwardChar:      w->pre_caret++;             break;
    case XIMBackwardChar:     w->pre_caret--;             break;
    case XIMLineStart:        w->pre_caret = 0;           break;
    case XIMLineEnd:          w->pre_caret = w->pre_len;  break;
    default: break;
    }
    if (w->pre_caret < 0) w->pre_caret = 0;
    if (w->pre_caret > w->pre_len) w->pre_caret = w->pre_len;
    c->position = w->pre_caret;
    linux_preedit_push(w);
}

PlatformWindow *platform_window_create(const PlatformWindowConfig *cfg) {
    PlatformWindow *w = calloc(1, sizeof(PlatformWindow));
    if (!w) return NULL;

    Display *dpy = g_platform.display;
    int screen = g_platform.screen;

    /* Double-buffered RGBA8 FBConfig (required for ARB context creation). */
    int fb_attrs[] = {
        GLX_X_RENDERABLE,  True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 0,
        GLX_DOUBLEBUFFER, True,
        None
    };
    int fbcount = 0;
    GLXFBConfig *fbc = glXChooseFBConfig(dpy, screen, fb_attrs, &fbcount);
    if (!fbc || fbcount == 0) { free(w); return NULL; }

    /* Prefer a 32-bit (ARGB) visual so window opacity/background transparency
     * is real per-pixel alpha rather than a uniform _NET_WM_WINDOW_OPACITY
     * wash — but only under a compositor, which is the only thing that blends
     * it. glXChooseFBConfig sorts by its own criteria and does not rank on
     * visual depth, so the ARGB config is rarely fbc[0]: scan for it. */
    GLXFBConfig chosen = fbc[0];
    XVisualInfo *vi = NULL;
    bool want_argb = linux_compositor_running();
    if (want_argb) {
        for (int i = 0; i < fbcount; i++) {
            XVisualInfo *cand = glXGetVisualFromFBConfig(dpy, fbc[i]);
            if (!cand) continue;
            if (cand->depth == 32) { chosen = fbc[i]; vi = cand; break; }
            XFree(cand);
        }
    }
    if (!vi) vi = glXGetVisualFromFBConfig(dpy, chosen);
    if (!vi) { XFree(fbc); free(w); return NULL; }
    w->argb_visual = (vi->depth == 32);

    Window root = RootWindow(dpy, screen);
    XSetWindowAttributes swa = {0};
    swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.background_pixel = BlackPixel(dpy, screen);
    /* CWBorderPixel is mandatory whenever the window's depth differs from its
     * parent's (the ARGB case): without it XCreateWindow inherits the root's
     * border pixmap and fails with BadMatch. */
    swa.border_pixel = 0;
    /* PropertyChangeMask is needed to serve SelectionRequest cleanly and to
     * observe _NET_WM_STATE / WM_STATE transitions; VisibilityChangeMask
     * drives the render gate (platform_window_visible). */
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask | FocusChangeMask | PropertyChangeMask |
                     VisibilityChangeMask;

    w->xwindow = XCreateWindow(dpy, root, 0, 0,
                                (unsigned)cfg->width, (unsigned)cfg->height, 0,
                                vi->depth, InputOutput, vi->visual,
                                CWColormap | CWEventMask | CWBackPixel |
                                CWBorderPixel, &swa);
    w->width = cfg->width;
    w->height = cfg->height;
    w->mapped = true;      /* assume visible until WM_STATE says otherwise */
    w->quake_height_ratio = 0.5f;

#ifdef LIU_HAVE_XINPUT2
    /* Takes over pointer delivery for this window (see xi2_select_window). */
    xi2_select_window(dpy, w->xwindow);
#endif

    XStoreName(dpy, w->xwindow, cfg->title);
    /* _NET_WM_NAME (UTF-8) is what every modern WM actually reads; the legacy
     * WM_NAME set above is Latin-1 and would mangle any non-ASCII title. */
    if (cfg->title)
        XChangeProperty(dpy, w->xwindow, g_platform.a_net_wm_name,
                        g_platform.a_utf8, 8, PropModeReplace,
                        (const unsigned char *)cfg->title,
                        (int)strlen(cfg->title));
    linux_set_window_identity(w->xwindow);

    /* Advertise XDND v5 so file managers offer drops at all. */
    {
        long xdnd_ver = 5;
        XChangeProperty(dpy, w->xwindow, g_platform.a_xdnd_aware, XA_ATOM, 32,
                        PropModeReplace, (const unsigned char *)&xdnd_ver, 1);
    }

    /* StaticGravity: a position asked for is where the CLIENT area goes, the
     * same point platform_window_get_position() reports. Under the default
     * gravity the window manager places its frame there instead, so every
     * get-then-set (a window opened beside another, a tab torn off) would
     * move the window down by the frame's title bar. */
    {
        XSizeHints *sh = XAllocSizeHints();
        if (sh) {
            sh->flags = PWinGravity;
            sh->win_gravity = StaticGravity;
            if (!cfg->resizable) {
                sh->flags |= PMinSize | PMaxSize;
                sh->min_width  = sh->max_width  = cfg->width;
                sh->min_height = sh->max_height = cfg->height;
            }
            if (cfg->has_origin) {
                sh->flags |= USPosition | PPosition;
                sh->x = (int)lroundf(cfg->origin_x);
                sh->y = (int)lroundf(cfg->origin_y);
            }
            XSetWMNormalHints(dpy, w->xwindow, sh);
            XFree(sh);
        }
        if (cfg->has_origin)
            XMoveWindow(dpy, w->xwindow, (int)lroundf(cfg->origin_x),
                        (int)lroundf(cfg->origin_y));
    }

    /* The close request, and the liveness ping: a window manager only sends
     * _NET_WM_PING to a window that lists it here (the reply is in
     * platform_poll_events), and only then can it offer "not responding /
     * force quit" for a hung window. */
    w->wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    Atom wm_protocols[2] = { w->wm_delete, g_platform.a_net_wm_ping };
    XSetWMProtocols(dpy, w->xwindow, wm_protocols, 2);

    x11_set_borderless(dpy, w->xwindow);
    w->borderless = true;
    w->edge_dir   = -1;
    w->app_cursor = CURSOR_DEFAULT;

    /* Per-window input context. Over-the-spot when the IM offers it (the
     * candidate window then follows the terminal cursor), root style
     * otherwise. Either way compose / dead keys / CJK commit through
     * Xutf8LookupString; NULL => plain XLookupString. */
    w->xic = NULL;
    w->xic_over_the_spot = false;
    w->xic_spot.x = w->xic_spot.y = -1;
    if (g_platform.xim) {
        bool want_spot = false, want_callbacks = false;
        XIMStyle style = xim_pick_style(g_platform.xim, &want_spot, &want_callbacks);

        if (want_callbacks) {
            /* XIMProc is the type the struct holds; start returns the longest
             * preedit it accepts, which Xlib reads back through it. */
            XIMCallback start = { (XPointer)w, (XIMProc)(void (*)(void))xim_preedit_start };
            XIMCallback done  = { (XPointer)w, (XIMProc)(void (*)(void))xim_preedit_done };
            XIMCallback draw  = { (XPointer)w, (XIMProc)(void (*)(void))xim_preedit_draw };
            XIMCallback caret = { (XPointer)w, (XIMProc)(void (*)(void))xim_preedit_caret };
            XVaNestedList pre = XVaCreateNestedList(
                0, XNPreeditStartCallback, &start, XNPreeditDoneCallback, &done,
                XNPreeditDrawCallback, &draw, XNPreeditCaretCallback, &caret,
                (void *)NULL);
            if (pre) {
                w->xic = XCreateIC(g_platform.xim,
                                   XNInputStyle, style,
                                   XNClientWindow, w->xwindow,
                                   XNFocusWindow,  w->xwindow,
                                   XNPreeditAttributes, pre,
                                   (void *)NULL);
                XFree(pre);
            }
            w->xic_on_the_spot = (w->xic != NULL);
        } else if (want_spot) {
            XFontSet fs = xim_fontset(dpy);   /* over-the-spot needs one */
            if (fs) {
                XPoint spot = { 0, 0 };
                XVaNestedList pre = XVaCreateNestedList(
                    0, XNSpotLocation, &spot, XNFontSet, fs, (void *)NULL);
                if (pre) {
                    w->xic = XCreateIC(g_platform.xim,
                                       XNInputStyle, style,
                                       XNClientWindow, w->xwindow,
                                       XNFocusWindow,  w->xwindow,
                                       XNPreeditAttributes, pre,
                                       (void *)NULL);
                    XFree(pre);
                }
            }
            w->xic_over_the_spot = (w->xic != NULL);
        }

        /* Whatever went wrong above — no font set, a server that advertises a
         * style it won't actually create — root style is the floor. */
        if (!w->xic) {
            w->xic = XCreateIC(g_platform.xim,
                               XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                               XNClientWindow, w->xwindow,
                               XNFocusWindow,  w->xwindow,
                               (void *)NULL);
        }
        if (w->xic) XSetICFocus(w->xic);
    }

    XMapWindow(dpy, w->xwindow);

    /* GL 3.3 CORE via the ARB entrypoint; legacy fallback on old drivers. */
    PFN_glXCreateContextAttribsARB create_ctx =
        (PFN_glXCreateContextAttribsARB)glXGetProcAddressARB(
            (const GLubyte *)"glXCreateContextAttribsARB");
    w->glx_context = NULL;
    /* Every window's context shares objects with the first one, so a texture
     * made in one (the glyph atlas, an image) is a texture in all of them. */
    GLXContext share = g_window_count > 0 ? g_windows[0]->glx_context : NULL;
    if (g_window_count >= LINUX_MAX_WINDOWS) {
        if (w->xic) XDestroyIC(w->xic);
        XFree(vi); XFree(fbc);
        XDestroyWindow(dpy, w->xwindow);
        free(w);
        return NULL;
    }
    if (create_ctx) {
        int ctx_attrs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };
        w->glx_context = create_ctx(dpy, chosen, share, True, ctx_attrs);
        XSync(dpy, False);   /* surface a NULL context before it is tested */
    }
    if (!w->glx_context)
        w->glx_context = glXCreateNewContext(dpy, chosen, GLX_RGBA_TYPE, share, True);
    if (!w->glx_context || !glXMakeCurrent(dpy, w->xwindow, w->glx_context)) {
        if (w->xic) XDestroyIC(w->xic);
        if (w->glx_context) glXDestroyContext(dpy, w->glx_context);
        XFree(vi); XFree(fbc);
        XDestroyWindow(dpy, w->xwindow);
        free(w);
        return NULL;
    }

    /* Resolve modern GL entrypoints now that a core context is current. */
    if (!gl_loader_init(liu_glx_getproc)) {
        fprintf(stderr, "liu: gl_loader_init failed — GL 3.3 entry points "
                        "missing; renderer will not function\n");
        glXMakeCurrent(dpy, None, NULL);
        if (w->xic) XDestroyIC(w->xic);
        glXDestroyContext(dpy, w->glx_context);
        XFree(vi); XFree(fbc);
        XDestroyWindow(dpy, w->xwindow);
        free(w);
        return NULL;
    }

    if (cfg->vsync) {
        typedef void (*PFNGLXSWAPINTERVALEXTPROC)(Display*, GLXDrawable, int);
        PFNGLXSWAPINTERVALEXTPROC swapInterval =
            (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");
        if (swapInterval) swapInterval(dpy, w->xwindow, 1);
    }

    g_windows[g_window_count++] = w;
    if (!g_key_window) g_key_window = w;
    atspi_window_attach(&w->a11y, w);

    XFree(vi);
    XFree(fbc);
    return w;
}

void platform_window_destroy(PlatformWindow *w) {
    if (!w) return;
    atspi_window_detach(&w->a11y);
    Display *dpy = g_platform.display;
    if (w->xic) { XUnsetICFocus(w->xic); XDestroyIC(w->xic); w->xic = NULL; }
    for (i32 i = 0; i < g_window_count; i++) {
        if (g_windows[i] != w) continue;
        memmove(&g_windows[i], &g_windows[i + 1],
                (usize)(g_window_count - i - 1) * sizeof g_windows[0]);
        g_window_count--;
        break;
    }
    if (g_key_window == w) g_key_window = g_window_count > 0 ? g_windows[0] : NULL;
    if (g_pointer_window == w) g_pointer_window = NULL;
    if (g_dispatch_window == w) g_dispatch_window = NULL;
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, w->glx_context);
    XDestroyWindow(dpy, w->xwindow);
    free(w);
}

void platform_window_set_title(PlatformWindow *w, const char *title) {
    if (!w || !title) return;
    XStoreName(g_platform.display, w->xwindow, title);
    /* Tab titles routinely carry non-ASCII (paths, agent names, CJK), which
     * WM_NAME's Latin-1 encoding cannot represent — _NET_WM_NAME is the one
     * a modern WM prefers, so keep both in sync. */
    XChangeProperty(g_platform.display, w->xwindow, g_platform.a_net_wm_name,
                    g_platform.a_utf8, 8, PropModeReplace,
                    (const unsigned char *)title, (int)strlen(title));
    atspi_window_set_title(&w->a11y, title);   /* the frame's accessible name */
}

/* On macOS this flips NSWindow.opaque. X11 has no such switch: whether alpha
 * means anything is a property of the VISUAL, fixed when the window is created
 * (see the ARGB scan in platform_window_create), and what actually varies at
 * runtime is the renderer's clear alpha — which reads the config every frame,
 * so an opacity change already applies live with no restart.
 *
 * What the flag is still good for is knowing whether anything can show through
 * at all, which is the precondition for the blur region below. */
void platform_window_set_transparent(PlatformWindow *w, bool transparent) {
    if (!w || w->translucent == transparent) return;
    w->translucent = transparent;
    /* ui.c calls set_opacity → set_transparent → set_blur, so the blur that
     * follows picks this up on its own. Re-applying here covers the reverse
     * order and the case where only the opacity moved. */
    platform_window_set_blur(w, w->blur_radius);
}

/* No blur on the X11 backend: the compositor owns background blur there and
 * exposes it per-window only through compositor-specific protocols (KWin's
 * property, Hyprland's rule), none of which is portable across the desktops
 * this build targets. Reported as inexact so the UI can say so rather than
 * showing a slider that does nothing. */
bool platform_window_set_content_opacity(PlatformWindow *w, f32 opacity) {
    /* No equivalent here: X11/Wayland translucency goes through the
     * framebuffer's own alpha, so the renderer keeps clearing translucent and
     * the glyph-alpha boost stays in play. */
    (void)w; (void)opacity;
    return false;
}

/* Blur whatever shows through the window.
 *
 * X11 has no standard for this. KWin's _KDE_NET_WM_BLUR_BEHIND_REGION is the
 * one convention adopted beyond its author (Plasma, and the wlroots
 * compositors that copied it), so it is the one used. The property holds a
 * list of x,y,w,h rectangles to blur; an EMPTY list means the whole window,
 * which is exactly right here: Liu's translucency is per-pixel and covers the
 * entire client area. Deleting the property turns it back off.
 *
 * The radius cannot be chosen: KWin blurs with a single global strength from
 * its own effect settings, so "on" and "off" are honoured but not the four
 * presets the UI offers. platform_window_blur_is_exact() reports that.
 *
 * As on macOS, this only shows where the window is actually translucent: at
 * opacity 1.0 there is nothing behind to blur. */
void platform_window_set_blur(PlatformWindow *w, f32 radius) {
    Display *dpy = g_platform.display;
    if (!w || !dpy) return;
    w->blur_radius = radius;
    Atom a = XInternAtom(dpy, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
    if (a == None) return;
    /* An opaque window covers every pixel of what is behind it, so asking the
     * compositor to blur that region buys a full-screen blur pass per frame
     * and shows exactly nothing. */
    if (w->translucent && isfinite(radius) && radius > 0.0f) {
        /* Xlib copies from the pointer even when the array length is zero, so
         * pass a valid pointer. */
        unsigned char none = 0;
        XChangeProperty(dpy, w->xwindow, a, XA_CARDINAL, 32,
                        PropModeReplace, &none, 0);
    } else {
        XDeleteProperty(dpy, w->xwindow, a);
    }
    XFlush(dpy);
}

/* The X11 compositor controls the blur radius, so the backend cannot
 * guarantee the requested value. */
bool platform_window_blur_is_exact(void) { return false; }

/* The window with the keyboard. Returning the first window rather than NULL
 * before any has had focus is what keeps the window-less compatibility shims
 * (the deprecated platform_set_titlebar_height, for one) pointed at a real
 * window. */
PlatformWindow *platform_key_window(void) { return g_key_window; }

/* _NET_WM_WINDOW_OPACITY is never set. The translucency is RENDERED (the
 * clear's alpha), so the property would apply it a second time (0.75 would
 * land at 0.56) and also makes KWin refuse to blur behind the window. Since
 * nothing sets the property, nothing has to clear it.
 *
 * A window created before a compositor started has a 24-bit visual with no
 * per-pixel alpha, so opacity there is inert. The property would not help in
 * that case either: it is the COMPOSITOR that implements it. */

void platform_window_get_position(PlatformWindow *w, f32 *x, f32 *y) {
    if (x) *x = 0; if (y) *y = 0;
    if (!w || !g_platform.display) return;
    Window root; int rx = 0, ry = 0; unsigned bw, d, ww, wh;
    if (XGetGeometry(g_platform.display, w->xwindow, &root, &rx, &ry,
                     &ww, &wh, &bw, &d)) {
        Window child;
        int ax = 0, ay = 0;
        XTranslateCoordinates(g_platform.display, w->xwindow, root, 0, 0,
                              &ax, &ay, &child);
        if (x) *x = (f32)ax;
        if (y) *y = (f32)ay;
    }
}

void platform_window_set_position(PlatformWindow *w, f32 x, f32 y) {
    if (!w || !g_platform.display) return;
    XMoveWindow(g_platform.display, w->xwindow, (int)x, (int)y);
}

/* Cross-window pointer geometry, for a tab dragged between two windows of this
 * process: ROOT coordinates in pixels, the unit every other position on this
 * backend is in. */
static void linux_work_area(i32 *x, i32 *y, i32 *w, i32 *h);   /* fwd */

bool platform_window_local_to_screen(PlatformWindow *w, f32 fx, f32 fy,
                                     f32 *sx, f32 *sy) {
    if (sx) *sx = 0;
    if (sy) *sy = 0;
    Display *dpy = g_platform.display;
    if (!w || !dpy) return false;
    Window child = None;
    int rx = 0, ry = 0;
    if (!XTranslateCoordinates(dpy, w->xwindow, RootWindow(dpy, g_platform.screen),
                               (int)lroundf(fx), (int)lroundf(fy), &rx, &ry, &child))
        return false;
    if (sx) *sx = (f32)rx;
    if (sy) *sy = (f32)ry;
    return true;
}

bool platform_window_screen_to_local(PlatformWindow *w, f32 sx, f32 sy,
                                     f32 *fx, f32 *fy) {
    if (fx) *fx = 0;
    if (fy) *fy = 0;
    Display *dpy = g_platform.display;
    if (!w || !dpy) return false;
    Window child = None;
    int lx = 0, ly = 0;
    if (!XTranslateCoordinates(dpy, RootWindow(dpy, g_platform.screen), w->xwindow,
                               (int)lroundf(sx), (int)lroundf(sy), &lx, &ly, &child))
        return false;
    if (fx) *fx = (f32)lx;
    if (fy) *fy = (f32)ly;
    return true;
}

static bool linux_window_holds_point(PlatformWindow *w, f32 sx, f32 sy) {
    if (!w->mapped) return false;
    f32 ox = 0, oy = 0;
    if (!platform_window_local_to_screen(w, 0, 0, &ox, &oy)) return false;
    return sx >= ox && sy >= oy && sx < ox + (f32)w->width && sy < oy + (f32)w->height;
}

/* Our windows, topmost first: the window manager's stacking list when it
 * publishes one, then any it does not list, newest first. */
static i32 linux_windows_top_down(PlatformWindow **out, i32 cap) {
    Display *dpy = g_platform.display;
    i32 n = 0;
    if (!dpy) return 0;
    Atom stacking = XInternAtom(dpy, "_NET_CLIENT_LIST_STACKING", True);
    unsigned char *data = NULL;
    unsigned long count = 0;
    if (stacking != None &&
        x11_get_prop(RootWindow(dpy, g_platform.screen), stacking, XA_WINDOW, 32,
                     4096, &data, &count)) {
        const Window *list = (const Window *)(const void *)data;
        for (unsigned long i = count; i-- > 0 && n < cap; ) {
            PlatformWindow *w = linux_window_of(list[i]);
            if (w) out[n++] = w;
        }
        XFree(data);
    }
    for (i32 i = g_window_count - 1; i >= 0 && n < cap; i--) {
        bool listed = false;
        for (i32 j = 0; j < n && !listed; j++) listed = out[j] == g_windows[i];
        if (!listed) out[n++] = g_windows[i];
    }
    return n;
}

PlatformWindow *platform_window_at_screen_point(f32 sx, f32 sy) {
    return platform_window_at_screen_point_below(sx, sy, NULL);
}

PlatformWindow *platform_window_at_screen_point_below(f32 sx, f32 sy,
                                                      PlatformWindow *through) {
    PlatformWindow *order[LINUX_MAX_WINDOWS];
    i32 n = linux_windows_top_down(order, LINUX_MAX_WINDOWS);
    for (i32 i = 0; i < n; i++)
        if (order[i] != through && linux_window_holds_point(order[i], sx, sy))
            return order[i];
    return NULL;
}

/* The monitor holding (or nearest to) a root point, less the panels: the work
 * area clipped to it (x11_monitor.h). The whole screen is the monitor when
 * RandR lists none. */
static X11Rect linux_visible_frame(i32 px, i32 py) {
    Display *dpy = g_platform.display;
    X11Rect mon = { 0, 0, DisplayWidth(dpy, g_platform.screen),
                    DisplayHeight(dpy, g_platform.screen) };
#ifdef LIU_HAVE_XRANDR
    {
        int nmon = 0;
        XRRMonitorInfo *mons = XRRGetMonitors(dpy, RootWindow(dpy, g_platform.screen),
                                              True, &nmon);
        X11Rect r[16];
        i32 n = 0;
        for (int i = 0; mons && i < nmon && n < 16; i++)
            r[n++] = (X11Rect){ mons[i].x, mons[i].y, mons[i].width, mons[i].height };
        if (mons) XRRFreeMonitors(mons);
        i32 at = x11_monitor_at(r, n, px, py);
        if (at >= 0) mon = r[at];
    }
#endif
    i32 ax, ay, aw, ah;
    linux_work_area(&ax, &ay, &aw, &ah);
    return x11_visible_frame(mon, (X11Rect){ ax, ay, aw, ah });
}

bool platform_screen_visible_frame_at(f32 sx, f32 sy,
                                      f32 *x, f32 *y, f32 *w, f32 *h) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!g_platform.display || !isfinite(sx) || !isfinite(sy)) return false;
    X11Rect f = linux_visible_frame((i32)lroundf(sx), (i32)lroundf(sy));
    if (x) *x = (f32)f.x;
    if (y) *y = (f32)f.y;
    if (w) *w = (f32)f.w;
    if (h) *h = (f32)f.h;
    return true;
}

/* The window a tab travels as is see-through while it is aimed at a strip:
 * _NET_WM_WINDOW_OPACITY, which a compositor applies (without one it stays
 * opaque, the only loss). 1 removes it. */
void platform_window_set_drag_alpha(PlatformWindow *w, f32 alpha) {
    Display *dpy = g_platform.display;
    if (!w || !dpy) return;
    if (!isfinite(alpha) || alpha >= 1.0f) {
        XDeleteProperty(dpy, w->xwindow, g_platform.a_net_wm_window_opacity);
    } else {
        if (alpha < 0.1f) alpha = 0.1f;
        unsigned long op = (unsigned long)((f64)alpha * 4294967295.0);
        XChangeProperty(dpy, w->xwindow, g_platform.a_net_wm_window_opacity,
                        XA_CARDINAL, 32, PropModeReplace,
                        (const unsigned char *)&op, 1);
    }
    XFlush(dpy);
}

/* Asked by the travelling tab, which ends when the button is up even if the
 * release was delivered to no window of ours. */
bool platform_mouse_primary_down(void) {
    Display *dpy = g_platform.display;
    if (!dpy) return false;
    Window root = None, child = None;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(dpy, RootWindow(dpy, g_platform.screen), &root, &child,
                       &rx, &ry, &wx, &wy, &mask))
        return false;
    return (mask & Button1Mask) != 0;
}

/* Raise AND give it the keyboard: a window manager ignores an application's
 * XSetInputFocus on a window it manages, so the request goes through it
 * (_NET_ACTIVE_WINDOW, source 1 = an application). */
void platform_window_activate(PlatformWindow *w) {
    Display *dpy = g_platform.display;
    if (!w || !dpy) return;
    XRaiseWindow(dpy, w->xwindow);
    XEvent ev = {0};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = w->xwindow;
    ev.xclient.message_type = g_platform.a_net_active_window;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 1;
    ev.xclient.data.l[1]    = CurrentTime;
    XSendEvent(dpy, RootWindow(dpy, g_platform.screen), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

/* No backdrop layer to diagnose here — the probe is a macOS instrument. */
bool platform_blur_debug_enabled(void) { return false; }
void platform_blur_debug_sample(PlatformWindow *w, const char *tag) {
    (void)w; (void)tag;
}
void platform_blur_debug_note(const char *fmt, ...) { (void)fmt; }

void platform_window_set_vsync(PlatformWindow *w, bool enabled) {
    if (!w || !g_platform.display) return;
    typedef void (*PFNGLXSWAPINTERVALEXTPROC)(Display *, GLXDrawable, int);
    PFNGLXSWAPINTERVALEXTPROC swap_interval =
        (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddress(
            (const GLubyte *)"glXSwapIntervalEXT");
    if (swap_interval) {
        swap_interval(g_platform.display, w->xwindow, enabled ? 1 : 0);
        return;
    }
    /* MESA/SGI variants act on the current drawable and take only the
     * interval; they are what Mesa exposes when the EXT flavour is absent. */
    typedef int (*PFNGLXSWAPINTERVALPROC)(int);
    PFNGLXSWAPINTERVALPROC si =
        (PFNGLXSWAPINTERVALPROC)glXGetProcAddress(
            (const GLubyte *)"glXSwapIntervalMESA");
    if (!si)
        si = (PFNGLXSWAPINTERVALPROC)glXGetProcAddress(
            (const GLubyte *)"glXSwapIntervalSGI");
    if (si) si(enabled ? 1 : 0);
}

f32 platform_window_max_refresh_hz(PlatformWindow *w) {
#ifdef LIU_HAVE_XRANDR
    /* The rate of the CRTC this window is actually on, not the X screen's
     * nominal one: a laptop panel next to a 144 Hz monitor gives two
     * different answers and the software present grid is built from this
     * number. Cheap enough for the once-a-second resample the caller does:
     * two round trips, no allocation beyond XRandR's own.
     *
     * macOS does not need this at all: there the display link IS the panel
     * (platform_window_display_link_sync). X11 has no callback-driven vblank
     * that can be used without parking a thread in glXWaitVideoSyncSGI, so
     * here the rate feeds the software grid instead. */
    if (!w || !g_platform.display) return 60.0f;

    int event_base = 0, error_base = 0;
    if (!XRRQueryExtension(g_platform.display, &event_base, &error_base)) return 60.0f;

    Window root = DefaultRootWindow(g_platform.display);
    XWindowAttributes wa;
    if (!XGetWindowAttributes(g_platform.display, w->xwindow, &wa)) return 60.0f;
    /* Window centre in root coordinates — the CRTC that owns the centre is the
     * one the compositor will sync this window to. */
    int wx = 0, wy = 0;
    Window child;
    if (!XTranslateCoordinates(g_platform.display, w->xwindow, root, 0, 0, &wx, &wy, &child))
        return 60.0f;
    const int cx = wx + wa.width / 2;
    const int cy = wy + wa.height / 2;

    XRRScreenResources *res = XRRGetScreenResourcesCurrent(g_platform.display, root);
    if (!res) return 60.0f;

    f32 hz = 0.0f;
    for (int i = 0; i < res->ncrtc && hz == 0.0f; i++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(g_platform.display, res, res->crtcs[i]);
        if (!ci) continue;
        if (ci->mode != None && ci->width > 0 && ci->height > 0 &&
            cx >= ci->x && cx < ci->x + (int)ci->width &&
            cy >= ci->y && cy < ci->y + (int)ci->height) {
            for (int m = 0; m < res->nmode; m++) {
                const XRRModeInfo *mi = &res->modes[m];
                if (mi->id != ci->mode) continue;
                /* Vertical refresh = dotClock / (hTotal * vTotal), with the
                 * interlace/doublescan corrections XRandR documents. */
                double vtotal = (double)mi->vTotal;
                if (mi->modeFlags & RR_DoubleScan) vtotal *= 2.0;
                if (mi->modeFlags & RR_Interlace)  vtotal /= 2.0;
                double denom = (double)mi->hTotal * vtotal;
                if (mi->dotClock > 0 && denom > 0.0)
                    hz = (f32)((double)mi->dotClock / denom);
                break;
            }
        }
        XRRFreeCrtcInfo(ci);
    }
    XRRFreeScreenResources(res);
    /* Anything outside a sane range means the mode was misread; the
     * caller's grid is safer with the 60 Hz floor than with a wrong number.
     */
    return (hz >= 20.0f && hz <= 1000.0f) ? hz : 60.0f;
#else
    (void)w;   /* built without XRandR: 60 Hz is the safe floor */
    return 60.0f;
#endif
}

/* ---- Display link: not available on X11 -----------------------------------
 * There is no callback-driven vblank here that does not cost a parked thread
 * (GLX_OML_sync_control / glXWaitVideoSyncSGI both BLOCK), so the link never
 * runs and the main loop keeps its software present grid, built from the
 * panel's real rate (the function above asks XRandR). Reporting "not running"
 * is what makes the caller take that path; see platform.h. */
void platform_window_display_link_sync(PlatformWindow *w, bool visible,
                                       bool focused, bool wants_frames) {
    (void)w; (void)visible; (void)focused; (void)wants_frames;
}
bool platform_window_display_link_running(PlatformWindow *w) { (void)w; return false; }
u64  platform_window_vsync_ticks(PlatformWindow *w) { (void)w; return 0; }
f64  platform_window_display_link_period_sec(PlatformWindow *w) { (void)w; return 0.0; }
f64  platform_window_display_link_silence_sec(PlatformWindow *w) { (void)w; return 0.0; }

void platform_window_set_presents_with_transaction(PlatformWindow *w, bool enabled) {
    (void)w; (void)enabled;   /* Metal-only; no-op on X11/GLX */
}

void platform_window_get_size(PlatformWindow *w, i32 *width, i32 *height) {
    *width = w->width; *height = w->height;
}

void platform_window_get_framebuffer_size(PlatformWindow *w, i32 *width, i32 *height) {
    /* Linux typically 1:1 unless HiDPI is configured */
    *width = w->width; *height = w->height;
}

/* Read the desktop's scale factor from Xft.dpi in the root RESOURCE_MANAGER.
 * That property is the single place X11 publishes it — there is no per-window
 * backing scale to query the way macOS has one. */
static f32 linux_compute_dpi_scale(void) {
    f32 scale = 1.0f;
    /* Read RESOURCE_MANAGER off the root directly rather than calling
     * XResourceManagerString(): that helper returns the copy Xlib cached when
     * the Display was opened and never refetches, so it would report the
     * startup value forever and miss a live Xft.dpi change. */
    unsigned char *prop = NULL;
    unsigned long nitems = 0;
    char *rms = NULL;
    if (x11_get_prop(RootWindow(g_platform.display, g_platform.screen),
                     XA_RESOURCE_MANAGER, XA_STRING, 8, 256 * 1024,
                     &prop, &nitems))
        rms = (char *)prop;
    if (rms) {
        XrmDatabase db = XrmGetStringDatabase(rms);
        if (db) {
            char *type = NULL; XrmValue val;
            if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &val) && val.addr) {
                errno = 0;
                char *end = NULL;
                double dpi = strtod(val.addr, &end);
                if (end != val.addr && errno != ERANGE && isfinite(dpi) && dpi > 1.0) {
                    if (dpi > 768.0) dpi = 768.0;
                    scale = (f32)(dpi / 96.0);
                }
            }
            XrmDestroyDatabase(db);
        }
    }
    if (prop) XFree(prop);
    if (scale < 1.0f) scale = 1.0f;   /* never downscale point dimensions */
    return scale;
}

f32 platform_window_get_dpi_scale(PlatformWindow *w) {
    (void)w;
    /* Cached: the layout pass asks every frame, and each miss meant parsing the
     * whole X resource database. The cache is refreshed by the RESOURCE_MANAGER
     * PropertyNotify that also raises EVENT_DPI_CHANGE. */
    if (g_platform.dpi_scale <= 0.0f)
        g_platform.dpi_scale = linux_compute_dpi_scale();
    return g_platform.dpi_scale;
}

/* Usable desktop rectangle for the current virtual desktop: the screen minus
 * panels/docks/reserved struts, published by the WM as _NET_WORKAREA (a flat
 * list of x,y,w,h quads, one per desktop). Falls back to the full screen when
 * no conforming WM is running. */
static void linux_work_area(i32 *x, i32 *y, i32 *w, i32 *h) {
    Display *dpy = g_platform.display;
    int screen = g_platform.screen;
    *x = 0; *y = 0;
    *w = DisplayWidth(dpy, screen);
    *h = DisplayHeight(dpy, screen);

    Window root = RootWindow(dpy, screen);
    unsigned long desktop = 0;
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (x11_get_prop(root, g_platform.a_net_current_desktop, XA_CARDINAL, 32, 1,
                     &data, &n)) {
        desktop = ((const unsigned long *)(const void *)data)[0];
        XFree(data);
    }

    data = NULL; n = 0;
    if (!x11_get_prop(root, g_platform.a_net_workarea, XA_CARDINAL, 32,
                      4 * 64, &data, &n))
        return;
    const unsigned long *wa = (const unsigned long *)(const void *)data;
    unsigned long base = desktop * 4;
    if (base + 3 < n) {
        *x = (i32)wa[base + 0];
        *y = (i32)wa[base + 1];
        *w = (i32)wa[base + 2];
        *h = (i32)wa[base + 3];
    }
    XFree(data);
}

/* Space the WM's decorations take on each side, so a placement sizes the FRAME
 * to the work area rather than the client area — otherwise every snapped
 * window overhangs by exactly one title bar and border.
 *
 * Falls back to the last non-zero answer when the property is missing, which
 * is what an unmapped (hidden Quake) window reads. */
static void linux_frame_extents(PlatformWindow *w, i32 *l, i32 *r, i32 *t, i32 *b) {
    *l = *r = *t = *b = 0;
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (x11_get_prop(w->xwindow, g_platform.a_net_frame_extents, XA_CARDINAL,
                     32, 4, &data, &n)) {
        if (n >= 4) {
            const unsigned long *fe = (const unsigned long *)(const void *)data;
            *l = (i32)fe[0]; *r = (i32)fe[1]; *t = (i32)fe[2]; *b = (i32)fe[3];
        }
        XFree(data);
    }
    if (*l || *r || *t || *b) {
        w->frame_l = *l; w->frame_r = *r; w->frame_t = *t; w->frame_b = *b;
        return;
    }
    *l = w->frame_l; *r = w->frame_r; *t = w->frame_t; *b = w->frame_b;
}

void platform_window_snap(PlatformWindow *w, WindowSnap pos) {
    Display *dpy = g_platform.display;
    if (!w || !dpy) return;

    /* Leave any maximized state first: a WM will refuse (or immediately undo)
     * a geometry change on a maximized window, so every snap starts from a
     * plain floating window. */
    x11_wm_state(w->xwindow, LIU_NET_WM_STATE_REMOVE,
                 g_platform.a_net_wm_state_max_vert,
                 g_platform.a_net_wm_state_max_horz);

    if (pos == WIN_SNAP_FULL) {
        /* "Full" means maximize, not fullscreen: the tab strip and toolbar
         * must stay reachable, and the WM's own maximize honours panel struts
         * better than any computed rectangle. */
        x11_wm_state(w->xwindow, LIU_NET_WM_STATE_ADD,
                     g_platform.a_net_wm_state_max_vert,
                     g_platform.a_net_wm_state_max_horz);
        return;
    }

    /* The monitor the window is on (its centre's), not the whole screen: with
     * two monitors side by side, the left half of the screen would be all of
     * the first monitor, whichever one the window is on. */
    f32 cx = 0.0f, cy = 0.0f;
    platform_window_local_to_screen(w, (f32)w->width * 0.5f, (f32)w->height * 0.5f,
                                    &cx, &cy);
    X11Rect vis = linux_visible_frame((i32)lroundf(cx), (i32)lroundf(cy));
    i32 ax = vis.x, ay = vis.y, aw = vis.w, ah = vis.h;
    i32 fl, fr, ft, fb;
    linux_frame_extents(w, &fl, &fr, &ft, &fb);

    /* Under the default NorthWest gravity a WM reads the requested x,y as the
     * FRAME's top-left (ICCCM 4.1.2.3) but width/height as the CLIENT area.
     * Positions are therefore plain work-area coordinates, while sizes have
     * the decoration subtracted — otherwise every snapped window overhangs the
     * work area by exactly one title bar and border. */
    i32 deco_w = fl + fr, deco_h = ft + fb;
    i32 half_w_frame = aw / 2, half_h_frame = ah / 2;

    i32 x = ax, y = ay;
    i32 frame_w = aw, frame_h = ah;

    switch (pos) {
    case WIN_SNAP_LEFT_HALF:    frame_w = half_w_frame; break;
    case WIN_SNAP_RIGHT_HALF:   frame_w = half_w_frame; x = ax + (aw - half_w_frame); break;
    case WIN_SNAP_TOP_HALF:     frame_h = half_h_frame; break;
    case WIN_SNAP_BOTTOM_HALF:  frame_h = half_h_frame; y = ay + (ah - half_h_frame); break;
    case WIN_SNAP_TOP_LEFT:     frame_w = half_w_frame; frame_h = half_h_frame; break;
    case WIN_SNAP_TOP_RIGHT:    frame_w = half_w_frame; frame_h = half_h_frame;
                                x = ax + (aw - half_w_frame); break;
    case WIN_SNAP_BOTTOM_LEFT:  frame_w = half_w_frame; frame_h = half_h_frame;
                                y = ay + (ah - half_h_frame); break;
    case WIN_SNAP_BOTTOM_RIGHT: frame_w = half_w_frame; frame_h = half_h_frame;
                                x = ax + (aw - half_w_frame);
                                y = ay + (ah - half_h_frame); break;
    case WIN_SNAP_CENTER:
        /* Two thirds of the work area, centred — the "restore to a comfortable
         * size" gesture, matching what the macOS backend does for CENTER. */
        frame_w = (aw * 2) / 3;
        frame_h = (ah * 2) / 3;
        x = ax + (aw - frame_w) / 2;
        y = ay + (ah - frame_h) / 2;
        break;
    case WIN_SNAP_FULL: return;   /* handled above */
    }

    i32 cw  = frame_w - deco_w;
    i32 chh = frame_h - deco_h;
    if (cw  < 160) cw  = 160;
    if (chh < 120) chh = 120;

    XMoveResizeWindow(dpy, w->xwindow, x, y, (unsigned)cw, (unsigned)chh);
    XFlush(dpy);
}

bool platform_window_should_close(PlatformWindow *w) { return w->should_close; }
void platform_window_cancel_close(PlatformWindow *w) { if (w) w->should_close = false; }
void platform_window_perform_close(PlatformWindow *w) { if (w) w->should_close = true; }

/* Tracked from _NET_WM_STATE (PropertyNotify keeps the mirror fresh) rather
 * than round-tripped: the layout pass queries this on every frame. */
bool platform_window_is_fullscreen(PlatformWindow *w) {
    return w && w->fullscreen;
}

void platform_window_set_fullscreen(PlatformWindow *w, bool enable) {
    if (!w || !g_platform.display) return;
    /* Ask the WM rather than resizing to the screen ourselves: only it can
     * take the window off the panel struts, hide its own decorations, and
     * restore the previous geometry on the way out. The `fullscreen` mirror is
     * updated by the PropertyNotify that follows, so the state stays the WM's
     * to own even if it refuses. */
    x11_wm_state(w->xwindow,
                 enable ? LIU_NET_WM_STATE_ADD : LIU_NET_WM_STATE_REMOVE,
                 g_platform.a_net_wm_state_fullscreen, None);
}

/* Liu draws its own window controls on X11 (see the borderless chrome), so
 * there are no platform-owned buttons to measure — the toolbar keeps its
 * compiled-in zone. */
f32 platform_traffic_light_right_pt(PlatformWindow *w) { (void)w; return 0.0f; }

/* X11 has no compositor-level occlusion signal, but the two cases that
 * actually matter for the render gate DO have one: iconified (WM_STATE leaves
 * NormalState) and fully covered by an opaque window (VisibilityFullyObscured).
 * Partial obscuring still renders — the exposed part is real. */
bool platform_window_visible(PlatformWindow *w) {
    if (!w) return true;
    return w->mapped && !w->obscured;
}

void platform_window_set_minimized(PlatformWindow *w, bool minimized) {
    if (!w || !g_platform.display) return;
    if (minimized)
        XIconifyWindow(g_platform.display, w->xwindow, DefaultScreen(g_platform.display));
    else
        XMapRaised(g_platform.display, w->xwindow);
    XFlush(g_platform.display);
}

bool platform_window_is_minimized(PlatformWindow *w) {
    return w && !w->mapped;
}

void platform_window_swap_buffers(PlatformWindow *w) {
    glXSwapBuffers(g_platform.display, w->xwindow);
    /* A presented frame is when the terminal may have changed: the screen
     * reader's change check rides it (one pointer test with nobody
     * listening), and the first one starts joining the accessibility bus. */
    atspi_window_frame_presented(&w->a11y);
}

void platform_make_current(PlatformWindow *w) {
    if (!w) return;
    glXMakeCurrent(g_platform.display, w->xwindow, w->glx_context);
}

/* =========================================================================
 * XDND — dragging files OUT
 *
 * The source half of the protocol. Where macOS hands NSDraggingSession a list
 * of promises and lets AppKit run the loop, X11 makes the source do all of it:
 * own the selection, grab the pointer, walk the window tree to find whatever is
 * under the cursor, drive Enter/Position/Drop by hand, and serve the payload.
 *
 * The loop below is modal, like the AppKit session it mirrors — the app does
 * not redraw while a drag is in flight, and the grab means no other client
 * sees the pointer either.
 * ========================================================================= */

#define DRAG_MAX_ITEMS 64

static struct {
    bool   active;
    Window source;                 /* the source window, the selection owner */
    Window target;                 /* current XDND-aware target, None if none */
    long   target_version;
    bool   target_accepts;

    i32    count;
    PlatformFilePromise items[DRAG_MAX_ITEMS];

    char   uri_list[DRAG_MAX_ITEMS * (PATH_MAX + 16)];
    bool   uris_built;
    char   tmp_dir[PATH_MAX];      /* materialised remote files; "" if unused */
} g_drag;

/* Percent-encode everything outside the unreserved set, so a path containing a
 * space, '#' or non-ASCII survives the text/uri-list round trip. '/' is kept
 * literal — it is the path separator, not data. */
static usize drag_uri_escape(const char *path, char *out, usize cap) {
    static const char hex[] = "0123456789ABCDEF";
    usize o = 0;
    for (const unsigned char *p = (const unsigned char *)path; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (safe) { out[o++] = (char)c; continue; }
        out[o++] = '%';
        out[o++] = hex[c >> 4];
        out[o++] = hex[c & 0xF];
    }
    out[o] = '\0';
    return o;
}

/* Build the text/uri-list payload, materialising remote items on the way.
 *
 * XDND has no equivalent of a macOS file promise: the target expects a URI it
 * can open right now. So a remote item is fetched to a temp file at the moment
 * the target asks for the data — which is the latest possible point, and the
 * only one where the user has actually committed to the drop. */
static void drag_build_uris(void) {
    if (g_drag.uris_built) return;
    g_drag.uris_built = true;
    g_drag.uri_list[0] = '\0';
    usize off = 0;

    for (i32 i = 0; i < g_drag.count; i++) {
        PlatformFilePromise *it = &g_drag.items[i];
        char path[PATH_MAX];
        path[0] = '\0';

        if (!it->is_remote) {
            snprintf(path, sizeof path, "%s", it->local_path);
        } else {
            if (!g_drag.tmp_dir[0]) {
                const char *base = getenv("XDG_RUNTIME_DIR");
                if (!base || !base[0]) base = "/tmp";
                snprintf(g_drag.tmp_dir, sizeof g_drag.tmp_dir,
                         "%s/liu-drag-XXXXXX", base);
                if (!mkdtemp(g_drag.tmp_dir)) { g_drag.tmp_dir[0] = '\0'; continue; }
            }
            const char *name = it->display_name[0] ? it->display_name : "file";
            /* Basename only: a display name is not a path, and a '/' in it
             * would write outside the temp directory. */
            const char *slash = strrchr(name, '/');
            if (slash) name = slash + 1;
            if (!name[0] || strcmp(name, "..") == 0) name = "file";
            int n = snprintf(path, sizeof path, "%s/%s", g_drag.tmp_dir, name);
            if (n <= 0 || (usize)n >= sizeof path) { path[0] = '\0'; continue; }
            if (!it->provider || !it->provider(it->ctx, path)) { path[0] = '\0'; continue; }
        }
        if (!path[0]) continue;

        char esc[PATH_MAX * 3];
        drag_uri_escape(path, esc, sizeof esc);
        int n = snprintf(g_drag.uri_list + off, sizeof(g_drag.uri_list) - off,
                         "file://%s\r\n", esc);
        if (n <= 0 || (usize)n >= sizeof(g_drag.uri_list) - off) break;
        off += (usize)n;
    }
}

/* Deepest window under the pointer, then the nearest ancestor that declares
 * XdndAware — the aware window is often a parent of the one the pointer is
 * actually over (client vs. WM frame). */
static Window drag_find_target(Window root, long *out_version) {
    Display *dpy = g_platform.display;
    *out_version = 0;

    Window child = root, w = root, r = root;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    for (int depth = 0; depth < 32; depth++) {
        if (!XQueryPointer(dpy, w, &r, &child, &rx, &ry, &wx, &wy, &mask)) break;
        if (child == None) break;
        w = child;
    }

    for (int up = 0; up < 32 && w != None && w != root; up++) {
        unsigned char *data = NULL;
        unsigned long n = 0;
        if (x11_get_prop(w, g_platform.a_xdnd_aware, AnyPropertyType, 32, 1,
                         &data, &n)) {
            long ver = (long)((const unsigned long *)(const void *)data)[0];
            XFree(data);
            if (ver > 0) { *out_version = ver > 5 ? 5 : ver; return w; }
        }
        Window rr = None, parent = None, *kids = NULL;
        unsigned int nk = 0;
        if (!XQueryTree(dpy, w, &rr, &parent, &kids, &nk)) break;
        if (kids) XFree(kids);
        w = parent;
    }
    return None;
}

static void drag_send(Window to, Atom msg, long a, long b, long c, long d, long e) {
    XEvent ev = {0};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = to;
    ev.xclient.message_type = msg;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0] = a; ev.xclient.data.l[1] = b;
    ev.xclient.data.l[2] = c; ev.xclient.data.l[3] = d; ev.xclient.data.l[4] = e;
    XSendEvent(g_platform.display, to, False, 0, &ev);
    XFlush(g_platform.display);
}

/* Answer a target asking for the dragged data. */
static void drag_serve_selection(const XSelectionRequestEvent *rq) {
    Display *dpy = g_platform.display;
    XSelectionEvent se = {0};
    se.type      = SelectionNotify;
    se.display   = rq->display;
    se.requestor = rq->requestor;
    se.selection = rq->selection;
    se.target    = rq->target;
    se.time      = rq->time;
    se.property  = None;

    Atom prop = (rq->property != None) ? rq->property : rq->target;
    if (rq->target == g_platform.a_targets) {
        Atom offer[] = { g_platform.a_targets, g_platform.a_uri_list };
        XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)offer, 2);
        se.property = prop;
    } else if (rq->target == g_platform.a_uri_list) {
        drag_build_uris();
        XChangeProperty(dpy, rq->requestor, prop, g_platform.a_uri_list, 8,
                        PropModeReplace,
                        (const unsigned char *)g_drag.uri_list,
                        (int)strlen(g_drag.uri_list));
        se.property = prop;
    }
    XSendEvent(dpy, rq->requestor, False, 0, (XEvent *)&se);
    XFlush(dpy);
}

static void drag_cleanup(void) {
    for (i32 i = 0; i < g_drag.count; i++) {
        PlatformFilePromise *it = &g_drag.items[i];
        if (it->is_remote && it->ctx && it->cleanup) it->cleanup(it->ctx);
    }
    /* The materialised files stay until the process exits: the target may still
     * be reading them when XdndFinished arrives, and $XDG_RUNTIME_DIR is
     * cleared at logout anyway. */
    memset(&g_drag, 0, sizeof g_drag);
}

bool platform_begin_file_drag(PlatformWindow *w,
                              const PlatformFilePromise *items, i32 count) {
    Display *dpy = g_platform.display;
    if (!w || !dpy || !items || count <= 0 || g_drag.active) return false;
    if (count > DRAG_MAX_ITEMS) count = DRAG_MAX_ITEMS;

    memset(&g_drag, 0, sizeof g_drag);
    g_drag.active = true;
    g_drag.source = w->xwindow;
    g_drag.count  = count;
    memcpy(g_drag.items, items, (usize)count * sizeof(*items));

    XSetSelectionOwner(dpy, g_platform.a_xdnd_selection, w->xwindow, CurrentTime);
    if (XGetSelectionOwner(dpy, g_platform.a_xdnd_selection) != w->xwindow) {
        g_drag.active = false;
        memset(&g_drag, 0, sizeof g_drag);
        return false;   /* caller still owns the promises */
    }

    /* Advertise the one type served, so a target with >3 types still works. */
    Atom type_list[] = { g_platform.a_uri_list };
    XChangeProperty(dpy, w->xwindow, g_platform.a_xdnd_type_list, XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)type_list, 1);

    Window root = RootWindow(dpy, g_platform.screen);
    /* A button is down here, so the server already holds an implicit grab that
     * lasts exactly as long as the drag: every motion and the release arrive
     * either way. The explicit grab is still asked for (it also confines the
     * cursor and would cover a drag started without a held button), but it is
     * NOT required: under XInput2 the implicit grab is a *device* grab, and a
     * core XGrabPointer on top of it fails with AlreadyGrabbed. Treating that
     * failure as fatal would make a drag to the desktop do nothing on
     * XI2-capable servers. */
    (void)XGrabPointer(dpy, w->xwindow, False,
                       ButtonReleaseMask | PointerMotionMask,
                       GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

    bool dropped = false;
    bool running = true;
    f64  started = platform_time_sec();

    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (ev.type == SelectionRequest &&
            ev.xselectionrequest.selection == g_platform.a_xdnd_selection) {
            drag_serve_selection(&ev.xselectionrequest);
            continue;
        }

        /* Normalise: with XI2 selected on the window the pointer arrives as
         * XI_Motion / XI_ButtonRelease cookies instead of core events. Copy
         * out the three fields the protocol needs and release the cookie
         * immediately, so the rest of the loop is shape-agnostic. */
        bool is_motion = false, is_release = false;
        i32  rx = 0, ry = 0;
        Time etime = CurrentTime;

        if (ev.type == MotionNotify) {
            is_motion = true;
            rx = ev.xmotion.x_root; ry = ev.xmotion.y_root;
            etime = ev.xmotion.time;
        } else if (ev.type == ButtonRelease) {
            is_release = true;
            etime = ev.xbutton.time;
        }
#ifdef LIU_HAVE_XINPUT2
        else if (g_xi2.available && ev.type == GenericEvent &&
                 ev.xcookie.extension == g_xi2.opcode) {
            if (XGetEventData(dpy, &ev.xcookie)) {
                const XIDeviceEvent *de = (const XIDeviceEvent *)ev.xcookie.data;
                if (ev.xcookie.evtype == XI_Motion) {
                    is_motion = true;
                    rx = (i32)de->root_x; ry = (i32)de->root_y;
                    etime = de->time;
                } else if (ev.xcookie.evtype == XI_ButtonRelease &&
                           !(de->flags & XIPointerEmulated)) {
                    is_release = true;
                    etime = de->time;
                }
                XFreeEventData(dpy, &ev.xcookie);
            }
            if (!is_motion && !is_release) continue;   /* scroll, etc. */
        }
#endif

        if (is_motion) {
            long ver = 0;
            Window t = drag_find_target(root, &ver);
            if (t != g_drag.target) {
                if (g_drag.target != None)
                    drag_send(g_drag.target, g_platform.a_xdnd_leave,
                              (long)g_drag.source, 0, 0, 0, 0);
                g_drag.target = t;
                g_drag.target_version = ver;
                g_drag.target_accepts = false;
                if (t != None)
                    drag_send(t, g_platform.a_xdnd_enter, (long)g_drag.source,
                              (ver << 24) | 1 /* type list in property */,
                              (long)g_platform.a_uri_list, 0, 0);
            }
            if (g_drag.target != None)
                drag_send(g_drag.target, g_platform.a_xdnd_position,
                          (long)g_drag.source, 0,
                          (rx << 16) | (ry & 0xFFFF), (long)etime,
                          (long)g_platform.a_xdnd_action_copy);
            continue;
        }

        if (ev.type == ClientMessage &&
            ev.xclient.message_type == g_platform.a_xdnd_status) {
            if ((Window)ev.xclient.data.l[0] == g_drag.target)
                g_drag.target_accepts = (ev.xclient.data.l[1] & 1) != 0;
            continue;
        }

        if (is_release) {
            if (g_drag.target != None && g_drag.target_accepts) {
                drag_send(g_drag.target, g_platform.a_xdnd_drop,
                          (long)g_drag.source, 0, (long)etime, 0, 0);
                dropped = true;
            } else if (g_drag.target != None) {
                drag_send(g_drag.target, g_platform.a_xdnd_leave,
                          (long)g_drag.source, 0, 0, 0, 0);
            }
            running = false;
            continue;
        }

        /* Anything else belongs to the app; queue it for after the drag. */
        linux_dispatch_xevent(&ev);

        if (platform_time_sec() - started > 60.0) running = false;  /* stuck grab */
    }

    XUngrabPointer(dpy, CurrentTime);
    XFlush(dpy);

    /* After the drop the target fetches the data, so keep serving selection
     * requests until XdndFinished (or a short timeout — not every target
     * bothers to send it). */
    if (dropped) {
        f64 t0 = platform_time_sec();
        bool finished = false;
        while (!finished && platform_time_sec() - t0 < 5.0) {
            while (XPending(dpy)) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == SelectionRequest &&
                    ev.xselectionrequest.selection == g_platform.a_xdnd_selection) {
                    drag_serve_selection(&ev.xselectionrequest);
                } else if (ev.type == ClientMessage &&
                           ev.xclient.message_type == g_platform.a_xdnd_finished) {
                    finished = true;
                } else {
                    linux_dispatch_xevent(&ev);
                }
            }
            if (!finished) platform_sleep_ms(10);
        }
    }

    XSetSelectionOwner(dpy, g_platform.a_xdnd_selection, None, CurrentTime);
    drag_cleanup();
    return true;   /* the promises were owned for the duration */
}

static KeyCode map_keysym(KeySym ks) {
    if (ks >= 'a' && ks <= 'z') return KEY_A + (KeyCode)(ks - 'a');
    if (ks >= 'A' && ks <= 'Z') return KEY_A + (KeyCode)(ks - 'A');
    if (ks >= '0' && ks <= '9') return KEY_0 + (KeyCode)(ks - '0');
    switch (ks) {
        case XK_Return:    return KEY_ENTER;
        case XK_Tab:       return KEY_TAB;
        case XK_BackSpace: return KEY_BACKSPACE;
        case XK_Escape:    return KEY_ESCAPE;
        case XK_Delete:    return KEY_DELETE;
        case XK_space:     return KEY_SPACE;
        case XK_Shift_L:   return KEY_LSHIFT;
        case XK_Shift_R:   return KEY_RSHIFT;
        case XK_Control_L: return KEY_LCTRL;
        case XK_Control_R: return KEY_RCTRL;
        case XK_Alt_L:     return KEY_LALT;
        case XK_Alt_R:     return KEY_RALT;
        case XK_Super_L:   return KEY_LSUPER;
        case XK_Super_R:   return KEY_RSUPER;
        case XK_Up:        return KEY_UP;
        case XK_Down:      return KEY_DOWN;
        case XK_Left:      return KEY_LEFT;
        case XK_Right:     return KEY_RIGHT;
        case XK_Home:      return KEY_HOME;
        case XK_End:       return KEY_END;
        case XK_Page_Up:   return KEY_PAGE_UP;
        case XK_Page_Down: return KEY_PAGE_DOWN;
        case XK_Insert:    return KEY_INSERT;
        /* Shift+Tab arrives as ISO_Left_Tab: without this the backtab never
         * reached the encoder (and Ctrl+Shift+Tab never matched). */
        case XK_ISO_Left_Tab: return KEY_TAB;
        /* Punctuation the binding table names. Without these the KEY_*
         * members exist but X11 never produces them, so Ctrl+, (Settings) and
         * Ctrl+= / Ctrl+- (font size) could not match. The keysym is the
         * shifted one, so Ctrl++ on a US layout arrives as `plus`. */
        case XK_comma:        return KEY_COMMA;
        case XK_period:       return KEY_PERIOD;
        case XK_bracketleft:  return KEY_LEFT_BRACKET;
        case XK_bracketright: return KEY_RIGHT_BRACKET;
        case XK_minus:        return KEY_MINUS;
        case XK_equal:        return KEY_EQUAL;
        case XK_plus:         return KEY_EQUAL;
        case XK_slash:        return KEY_SLASH;
        case XK_F1:  return KEY_F1;  case XK_F2:  return KEY_F2;
        case XK_F3:  return KEY_F3;  case XK_F4:  return KEY_F4;
        case XK_F5:  return KEY_F5;  case XK_F6:  return KEY_F6;
        case XK_F7:  return KEY_F7;  case XK_F8:  return KEY_F8;
        case XK_F9:  return KEY_F9;  case XK_F10: return KEY_F10;
        case XK_F11: return KEY_F11; case XK_F12: return KEY_F12;
        default: return KEY_UNKNOWN;
    }
}

static u32 linux_keysym_known(u32 ks) { return (u32)map_keysym((KeySym)ks); }

/* The key a chord names when the symbol the layout typed is none Liu knows
 * (x11_keypos.h): XKB is asked what else the same physical key carries — the
 * Shift level of the active layout, then the keymap's other layouts. */
static KeyCode linux_key_by_position(Display *dpy, unsigned int keycode, unsigned int state) {
    X11KeySyms k;
    memset(&k, 0, sizeof k);
    for (i32 g = 0; g < X11_KEYPOS_GROUPS; g++) {
        for (i32 l = 0; l < X11_KEYPOS_LEVELS; l++) {
            KeySym sym = XkbKeycodeToKeysym(dpy, (XKeyCode)keycode, g, l);
            k.syms[g][l] = (u32)sym;
            if (sym != NoSymbol) k.groups = g + 1;
        }
    }
    return (KeyCode)x11_key_by_position(&k, (i32)XkbGroupForCoreState(state), linux_keysym_known);
}

/* X11 numbers its buttons 1=left, 2=MIDDLE, 3=right; PlatformEvent uses
 * 0=left, 1=right, 2=middle (see platform.h), so the mapping is explicit
 * rather than a subtraction. Buttons 4-7 are wheel events and are handled
 * before this is reached; 8/9 are back/forward, which nothing binds yet. */
static i32 map_button(unsigned int x_button) {
    switch (x_button) {
    case 1:  return 0;   /* left   */
    case 3:  return 1;   /* right  */
    case 2:  return 2;   /* middle */
    default: return -1;  /* unbound (back/forward, extra buttons) */
    }
}

static u32 get_mods(unsigned int state) {
    u32 m = 0;
    if (state & ShiftMask)   m |= MOD_SHIFT;
    if (state & ControlMask) m |= MOD_CTRL;
    if (state & Mod1Mask)    m |= MOD_ALT;
    if (state & Mod4Mask)    m |= MOD_SUPER;
    return m;
}

/* Because Liu draws the tab strip in the client area, the window manager does
 * not receive title-bar double-clicks. Detect the gesture in the app, and
 * retain the zoom-query guard so a double-click on a tab's close button does
 * not maximize the window. */
static PlatformTitlebarZoomQuery g_zoom_query = NULL;
static void                     *g_zoom_user  = NULL;
static f32                       g_titlebar_h_pt = 0.0f;

void platform_set_titlebar_zoom_query(PlatformTitlebarZoomQuery fn, void *user) {
    g_zoom_query = fn;
    g_zoom_user  = user;
}

/* The focused pane's text for a screen reader: the AT-SPI2 bridge answers
 * from this provider (atspi_bridge.c). Without D-Bus it is kept and unused. */
void platform_window_set_a11y_provider(PlatformWindow *w,
                                       PlatformA11yProviderFn fn, void *user) {
    if (!w) return;
    atspi_window_set_provider(&w->a11y, fn, user);
}

#ifdef LIU_HAVE_DBUS
/* The bridge's questions about a window (atspi_bridge.h). */

/* Its size, and its top-left on the screen when asked for (a round trip). */
bool linux_a11y_window_rect(PlatformWindow *w, i32 *x, i32 *y, i32 *width, i32 *height) {
    if (!w || !g_platform.display) return false;
    if (width)  *width = w->width;
    if (height) *height = w->height;
    if (x || y) {
        int sx = 0, sy = 0;
        Window child = None;
        Display *dpy = g_platform.display;
        if (!XTranslateCoordinates(dpy, w->xwindow, RootWindow(dpy, g_platform.screen),
                                   0, 0, &sx, &sy, &child))
            return false;
        if (x) *x = sx;
        if (y) *y = sy;
    }
    return true;
}

/* Provider geometry is in points; a window coordinate here is a pixel. */
f32 linux_a11y_window_scale(PlatformWindow *w) {
    f32 s = platform_window_get_dpi_scale(w);
    return s > 0.0f ? s : 1.0f;
}

bool linux_a11y_window_mapped(PlatformWindow *w) {
    return w && w->mapped;
}

/* Whether X has the keyboard on this window now (a round trip, asked once
 * when the bridge joins; FocusIn / FocusOut keep it after that). */
bool linux_a11y_window_has_focus(PlatformWindow *w) {
    if (!w || !g_platform.display) return false;
    Window focus = None;
    int revert = 0;
    XGetInputFocus(g_platform.display, &focus, &revert);
    return focus == w->xwindow;
}

/* The accessibility bus address a session publishes on the root window,
 * when it does. The atom is only looked up, never created. */
bool linux_a11y_bus_property(char *out, usize cap) {
    if (!out || cap == 0 || !g_platform.display) return false;
    out[0] = '\0';
    Display *dpy = g_platform.display;
    Atom a = XInternAtom(dpy, "AT_SPI_BUS", True);
    if (a == None) return false;
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (!x11_get_prop(RootWindow(dpy, g_platform.screen), a, XA_STRING, 8, 1024, &data, &n))
        return false;
    usize len = n < cap - 1 ? (usize)n : cap - 1;
    memcpy(out, data, len);
    out[len] = '\0';
    XFree(data);
    return out[0] != '\0';
}
#endif

void platform_set_render_callback(PlatformRenderCallback cb, void *user) {
    /* X11 lacks the macOS event-tracking-mode trap that necessitates the
     * timer-driven render hook. Stub so cross-platform code links cleanly. */
    (void)cb;
    (void)user;
}

/* =========================================================================
 * XDND — receiving file drops
 * ========================================================================= */

/* The file:// -> path conversion lives in platform/xdnd_uri.h (pure, tested). */

/* Split a text/uri-list payload and queue one EVENT_DROP_FILE per local file,
 * matching what the macOS backend produces.
 *
 * The splitting, the validation and the ownership of each path are
 * platform_drop_emit_uri_list (platform/event_payload.h). Each event owns a
 * copy of its path, released when the app has handled it (platform_next_event)
 * or when the queue discards it, so neither a long drop nor a refused line can
 * overwrite a path that is still queued. */
static void xdnd_emit_drops(const char *data, usize len) {
    i32 refused = 0;
    i32 queued = platform_drop_emit_uri_list(&g_evq, &g_payloads, NULL, data, len,
                                             g_platform.xdnd_x, g_platform.xdnd_y,
                                             platform_time_sec(), &refused);
    if (refused > 0)
        fprintf(stderr, "liu: drop: %d of %d item(s) ignored (not a local file, "
                        "an unusable path, or more than can be queued)\n",
                refused, queued + refused);
}

/* Tell the source whether the drop is accepted, and over what rectangle.
 * A source that gets no XdndStatus shows the "rejected" cursor and never
 * sends XdndDrop. */
static void xdnd_send_status(Window target, Window source, bool accept) {
    XEvent ev = {0};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = source;
    ev.xclient.message_type = g_platform.a_xdnd_status;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = (long)target;
    ev.xclient.data.l[1]    = accept ? 1 : 0;
    ev.xclient.data.l[2]    = 0;   /* empty rect = ask again on every move, so
                                    * the hover position stays live */
    ev.xclient.data.l[3]    = 0;
    ev.xclient.data.l[4]    = accept ? (long)g_platform.a_xdnd_action_copy : None;
    XSendEvent(g_platform.display, source, False, 0, &ev);
    XFlush(g_platform.display);
}

static void xdnd_send_finished(Window target, Window source, bool accepted) {
    XEvent ev = {0};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = source;
    ev.xclient.message_type = g_platform.a_xdnd_finished;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = (long)target;
    ev.xclient.data.l[1]    = accepted ? 1 : 0;
    ev.xclient.data.l[2]    = accepted ? (long)g_platform.a_xdnd_action_copy : None;
    XSendEvent(g_platform.display, source, False, 0, &ev);
    XFlush(g_platform.display);
}

/* Does the source offer text/uri-list? Types 1-3 ride in the XdndEnter message;
 * a longer list lives in the source's XdndTypeList property. */
static bool xdnd_source_has_uri_list(const XClientMessageEvent *cm) {
    if (cm->data.l[1] & 1) {
        unsigned char *data = NULL;
        unsigned long n = 0;
        if (!x11_get_prop((Window)cm->data.l[0], g_platform.a_xdnd_type_list,
                          XA_ATOM, 32, 64, &data, &n))
            return false;
        const Atom *types = (const Atom *)(const void *)data;
        bool found = false;
        for (unsigned long i = 0; i < n; i++)
            if (types[i] == g_platform.a_uri_list) { found = true; break; }
        XFree(data);
        return found;
    }
    for (int i = 2; i <= 4; i++)
        if ((Atom)cm->data.l[i] == g_platform.a_uri_list) return true;
    return false;
}

/* ---------------------------------------------------------------------------
 * Pointer buttons
 *
 * Shared by the core ButtonPress/ButtonRelease path and the XInput2 path, so
 * the wheel fallback and the title-bar double-click live in exactly one place.
 * `state` is an X modifier mask (core xbutton.state / XI2 mods.effective) —
 * i.e. the mods as they were BEFORE this press, which is what block-select
 * (Alt+drag), open-link (Ctrl+click) and word-select want.
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Wheel notch → line count
 *
 * X11 hands a wheel click through as exactly one notch. AppKit applies the
 * system's scroll setting before the event reaches the app, which is why the
 * UI layer can treat a non-precise delta as a count of *lines* on macOS (see
 * the file-viewer handler in main.c). The X server has no such notion, so the
 * notch is scaled here.
 *
 * Three lines per notch is the usual desktop default. KDE makes it
 * configurable through kcminputrc: that file is read when it exists, whatever
 * desktop is running, because a value set there applies to every application
 * on the machine.
 * ------------------------------------------------------------------------- */
#define WHEEL_LINES_DEFAULT 3.0f
static f32 g_wheel_lines = WHEEL_LINES_DEFAULT;

static void wheel_lines_init(void) {
    char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)       snprintf(path, sizeof path, "%s/kcminputrc", xdg);
    else if (home && *home) snprintf(path, sizeof path, "%s/.config/kcminputrc", home);
    else return;

    FILE *f = fopen(path, "r");
    if (!f) return;

    /* WheelScrollLines is a [Mouse] key, and the same file carries unrelated
     * sections — track the section rather than grepping the whole file. */
    bool in_mouse = false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { in_mouse = (strncmp(s, "[Mouse]", 7) == 0); continue; }
        if (!in_mouse) continue;
        if (strncmp(s, "WheelScrollLines", 16) != 0) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        double v = atof(eq + 1);
        /* A malformed or absurd value must not make the wheel useless in
         * either direction; 0 in particular would freeze scrolling. */
        if (v >= 1.0 && v <= 100.0) g_wheel_lines = (f32)v;
        break;
    }
    fclose(f);
}

/* The resize edge under (x, y) in a window Liu draws the frame of, as a
 * _NET_WM_MOVERESIZE direction (0 = top-left, clockwise to 7 = left), or -1.
 * The band is inside the window — there is no border outside it to grab — and
 * a corner reaches 16 pt along both edges, so it can be hit. None while the
 * window is maximized, full screen or dropped down. */
static i32 linux_resize_edge(PlatformWindow *w, i32 x, i32 y) {
    if (!w || !w->borderless || w->maximized || w->fullscreen || w->quake) return -1;
    f32 scale = platform_window_get_dpi_scale(w);
    i32 band = (i32)lroundf(5.0f * scale);
    if (band < 3) band = 3;
    i32 corner = (i32)lroundf(16.0f * scale);
    bool l = x < band, r = x >= w->width - band;
    bool t = y < band, b = y >= w->height - band;
    if (!(l || r || t || b)) return -1;
    bool cl = x < corner, cr = x >= w->width - corner;
    bool ct = y < corner, cb = y >= w->height - corner;
    if ((t && cl) || (l && ct)) return 0;
    if ((t && cr) || (r && ct)) return 2;
    if ((b && cr) || (r && cb)) return 4;
    if ((b && cl) || (l && cb)) return 6;
    return t ? 1 : r ? 3 : b ? 5 : 7;
}

static void linux_apply_cursor(PlatformWindow *w, i32 type);   /* fwd */

/* The pointer moved: show the edge's resize cursor while it is on one, and
 * the app's own shape again when it leaves. No event and no frame: raw
 * pointer motion makes no frame. */
static void linux_edge_hover(PlatformWindow *w, i32 x, i32 y) {
    if (!w) return;
    i32 dir = linux_resize_edge(w, x, y);
    if (dir == w->edge_dir) return;
    w->edge_dir = dir;
    Display *dpy = g_platform.display;
    if (dir < 0) { linux_apply_cursor(w, w->app_cursor); return; }
    if (!g_platform.edge_cur_init) {
        static const unsigned int shapes[8] = {
            XC_top_left_corner, XC_top_side, XC_top_right_corner, XC_right_side,
            XC_bottom_right_corner, XC_bottom_side, XC_bottom_left_corner, XC_left_side,
        };
        for (i32 i = 0; i < 8; i++) g_platform.edge_cur[i] = XCreateFontCursor(dpy, shapes[i]);
        g_platform.edge_cur_init = true;
    }
    XDefineCursor(dpy, w->xwindow, g_platform.edge_cur[dir]);
    XFlush(dpy);
}

static void linux_begin_moveresize(PlatformWindow *w, long direction);   /* fwd */
static void linux_move_pending_motion(PlatformWindow *w);               /* fwd */

static void handle_pointer_button(PlatformWindow *win, bool press,
                                  unsigned int button, i32 x, i32 y,
                                  unsigned int state, Time t) {
    if (button >= 4 && button <= 7) {
        /* Core wheel notches. Only reached when the device exposes no scroll
         * valuators (or XInput2 is unavailable) — otherwise these arrive
         * flagged XIPointerEmulated and are dropped in favour of the
         * higher-resolution valuator deltas. */
        if (!press) return;
        push_event((PlatformEvent){
            .type = EVENT_MOUSE_MOVE,
            .mouse = { .button = -1, .x = x, .y = y, .mods = get_mods(state) }
        });
        f32 dx = 0.0f, dy = 0.0f;
        if (button == 4) dy =  g_wheel_lines;
        if (button == 5) dy = -g_wheel_lines;
        if (button == 6) dx = -g_wheel_lines;
        if (button == 7) dx =  g_wheel_lines;
        push_event((PlatformEvent){
            .type = EVENT_SCROLL,
            .scroll = { .dx = dx, .dy = dy, .mods = get_mods(state),
                        .precise = false }
        });
        return;
    }

    i32 btn = map_button(button);
    if (btn < 0) return;
    if (!press && win) win->move_pending = false;

    /* A press on the resize band is the window manager's from here on: the
     * app never sees it, as it never saw a press on a WM-drawn border. */
    if (press && btn == 0 && win) {
        i32 dir = linux_resize_edge(win, x, y);
        if (dir >= 0) { linux_begin_moveresize(win, dir); return; }
    }

    /* Double-click in the strip Liu paints over the title bar = maximize,
     * matching what the WM would do on a real title bar. X11 has no
     * clickCount, so the presses are paired here: same button, within the
     * 400 ms the desktop conventionally uses, and close enough that it is a
     * click rather than a small drag. */
    if (press && btn == 0 && win && g_titlebar_h_pt > 0.0f) {
        static Time last_time = 0;
        static i32  last_x = 0, last_y = 0;
        f32   scale = platform_window_get_dpi_scale(win);
        bool  in_strip = (f32)y < g_titlebar_h_pt * scale;
        bool  paired = (last_time != 0) && (t - last_time) <= 400 &&
                       abs(x - last_x) <= 4 && abs(y - last_y) <= 4;
        if (in_strip && paired) {
            last_time = 0;   /* consume: a triple click is not two zooms */
            bool zoomable = !g_zoom_query ||
                            g_zoom_query(win, g_zoom_user, (f32)x, (f32)y);
            if (zoomable) {
                bool maxed = x11_has_wm_state(
                    win->xwindow, g_platform.a_net_wm_state_max_vert);
                x11_wm_state(win->xwindow,
                             maxed ? LIU_NET_WM_STATE_REMOVE
                                   : LIU_NET_WM_STATE_ADD,
                             g_platform.a_net_wm_state_max_vert,
                             g_platform.a_net_wm_state_max_horz);
                return;   /* swallow: the app must not also see this */
            }
        } else {
            last_time = t; last_x = x; last_y = y;
        }
    }

    push_event((PlatformEvent){
        .type = press ? EVENT_MOUSE_DOWN : EVENT_MOUSE_UP,
        .mouse = { .button = btn, .x = x, .y = y, .mods = get_mods(state) }
    });
}

#ifdef LIU_HAVE_XINPUT2
/* ---------------------------------------------------------------------------
 * XInput2 — smooth (pixel-resolution) scrolling
 *
 * Core X11 encodes scrolling as button 4/5/6/7 presses: whole wheel notches,
 * nothing finer. XI2 2.1 instead exposes a *scroll valuator* per axis whose
 * value accumulates continuously; the delta between two motion events is the
 * scroll amount, and `increment` is how much of it makes one notch.
 *
 * A wheel always steps by an exact multiple of `increment`, a touchpad by
 * fractions of it — which is the split the app cares about, because the
 * palette / viewer / terminal scroll paths run precise (pixel) and non-precise
 * (line) deltas through completely different accumulators. Feeding a 0.05-notch
 * touchpad delta down the line path would round to zero and then get forced to
 * a whole line, so a gentle swipe would teleport. Hence: exact multiples stay
 * notches, everything else becomes pixels.
 *
 * Notches → pixels needs one constant, and the drivers disagree on units:
 * xf86-input-libinput sets increment ≈ 15 and reports valuator units that are
 * already pixels of finger travel, while XWayland sets increment = 1 and
 * divides the compositor's pixel delta by the conventional 10 units-per-notch
 * first. max(increment, 10) recovers pixels in both cases, i.e. content
 * tracks the finger 1:1 the way it does on macOS.
 * ------------------------------------------------------------------------- */
/* (Re)build the scroll-axis table. Also called on every device change, which
 * conveniently drops the stale baselines: a device reset restarts its valuator
 * from an arbitrary value, and differencing across that would fire one huge
 * bogus scroll. */
static void xi2_scan_devices(Display *dpy) {
    g_xi2.axis_count = 0;
    int ndev = 0;
    XIDeviceInfo *devs = XIQueryDevice(dpy, XIAllDevices, &ndev);
    if (!devs) return;
    for (int d = 0; d < ndev && g_xi2.axis_count < XI2_MAX_AXES; d++) {
        for (int c = 0; c < devs[d].num_classes; c++) {
            if (devs[d].classes[c]->type != XIScrollClass) continue;
            const XIScrollClassInfo *s =
                (const XIScrollClassInfo *)devs[d].classes[c];
            if (s->increment == 0.0) continue;   /* would divide by zero */
            if (g_xi2.axis_count >= XI2_MAX_AXES) break;
            g_xi2.axes[g_xi2.axis_count++] = (Xi2ScrollAxis){
                .deviceid  = devs[d].deviceid,
                .number    = s->number,
                .type      = s->scroll_type,
                .increment = s->increment,
            };
        }
    }
    XIFreeDeviceInfo(devs);
}

static Xi2ScrollAxis *xi2_find_axis(int deviceid, int number) {
    for (int i = 0; i < g_xi2.axis_count; i++)
        if (g_xi2.axes[i].deviceid == deviceid && g_xi2.axes[i].number == number)
            return &g_xi2.axes[i];
    return NULL;
}

static void xi2_init(Display *dpy) {
    int first_event = 0, first_error = 0;
    if (!XQueryExtension(dpy, "XInputExtension", &g_xi2.opcode,
                         &first_event, &first_error))
        return;
    /* Ask for 2.2; accept anything >= 2.1, the version that added smooth
     * scrolling. XIQueryVersion writes back what the server actually offers. */
    int major = 2, minor = 2;
    if (XIQueryVersion(dpy, &major, &minor) != Success) return;
    if (major < 2 || (major == 2 && minor < 1)) return;
    g_xi2.available = true;
    xi2_scan_devices(dpy);

    /* Hierarchy changes (a device plugged in or removed) are only selectable
     * on the root window, and only for XIAllDevices. */
    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(mask, XI_HierarchyChanged);
    XIEventMask em = { .deviceid = XIAllDevices,
                       .mask_len = (int)sizeof mask, .mask = mask };
    XISelectEvents(dpy, RootWindow(dpy, g_platform.screen), &em, 1);
}

/* Selecting an XI2 event type on a window replaces core delivery of the
 * equivalent event for that window, so once XI_Motion/XI_Button* are
 * selected this backend owns the whole pointer path and must synthesise
 * every event core would have delivered. Keys stay on the core path (XIM
 * filtering needs them there). */
static void xi2_select_window(Display *dpy, Window w) {
    if (!g_xi2.available) return;
    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(mask, XI_Motion);
    XISetMask(mask, XI_ButtonPress);
    XISetMask(mask, XI_ButtonRelease);
    XISetMask(mask, XI_DeviceChanged);
    XIEventMask em = { .deviceid = XIAllMasterDevices,
                       .mask_len = (int)sizeof mask, .mask = mask };
    XISelectEvents(dpy, w, &em, 1);
}

static void xi2_handle_motion(PlatformWindow *win, const XIDeviceEvent *ev) {
    linux_edge_hover(win, (i32)ev->event_x, (i32)ev->event_y);
    linux_move_pending_motion(win);
    push_event((PlatformEvent){
        .type = EVENT_MOUSE_MOVE,
        .mouse = { .button = -1,
                   .x = (i32)ev->event_x, .y = (i32)ev->event_y,
                   .mods = get_mods((unsigned)ev->mods.effective) }
    });

    /* Pixel deltas and notch deltas are different units, so they cannot be
     * summed into one event; accumulate separately and emit up to two. */
    f32 px_dx = 0.0f, px_dy = 0.0f, ln_dx = 0.0f, ln_dy = 0.0f;

    const double *v = ev->valuators.values;
    int nbits = ev->valuators.mask_len * 8;
    for (int i = 0; i < nbits; i++) {
        if (!XIMaskIsSet(ev->valuators.mask, i)) continue;
        double value = *v++;                       /* values are packed */
        Xi2ScrollAxis *ax = xi2_find_axis(ev->sourceid, i);
        if (!ax) continue;                         /* pointer x/y, pressure, … */
        if (!ax->have_last) { ax->last = value; ax->have_last = true; continue; }
        double raw = value - ax->last;
        ax->last = value;
        if (raw == 0.0) continue;

        double notches = raw / ax->increment;
        bool   whole   = fabs(notches - round(notches)) < 0.01;

        /* X counts a growing vertical valuator as scrolling *down* and a
         * growing horizontal one as scrolling *right*; Liu's dy is
         * positive-up (matching the button-4 fallback above) and dx
         * positive-right. */
        if (whole) {
            /* Line units, so the desktop's lines-per-notch applies here and
             * not in the pixel branch below: a touchpad already reports the
             * distance the finger travelled. */
            if (ax->type == XIScrollTypeVertical) ln_dy -= (f32)notches * g_wheel_lines;
            else                                  ln_dx += (f32)notches * g_wheel_lines;
        } else {
            double px = notches * (ax->increment > XI2_MIN_PX_PER_NOTCH
                                   ? ax->increment : XI2_MIN_PX_PER_NOTCH);
            if (ax->type == XIScrollTypeVertical) px_dy -= (f32)px;
            else                                  px_dx += (f32)px;
        }
    }

    u32 mods = get_mods((unsigned)ev->mods.effective);
    if (px_dx != 0.0f || px_dy != 0.0f)
        push_event((PlatformEvent){
            .type = EVENT_SCROLL,
            .scroll = { .dx = px_dx, .dy = px_dy, .mods = mods, .precise = true }
        });
    if (ln_dx != 0.0f || ln_dy != 0.0f)
        push_event((PlatformEvent){
            .type = EVENT_SCROLL,
            .scroll = { .dx = ln_dx, .dy = ln_dy, .mods = mods, .precise = false }
        });
}

/* Returns true when the event was an XI2 one and has been consumed. */
/* An XI2 event names its window inside the cookie, so the window is found
 * here rather than by the dispatcher. */
static PlatformWindow *xi2_event_window(const XIDeviceEvent *ev) {
    PlatformWindow *w = linux_window_of(ev->event);
    g_dispatch_window = w;
    if (w) g_pointer_window = w;
    return w;
}

static bool xi2_dispatch(Display *dpy, XEvent *xep) {
    if (!g_xi2.available || xep->type != GenericEvent ||
        xep->xcookie.extension != g_xi2.opcode)
        return false;
    if (!XGetEventData(dpy, &xep->xcookie)) return true;   /* consumed, no data */

    switch (xep->xcookie.evtype) {
    case XI_Motion: {
        const XIDeviceEvent *ev = (const XIDeviceEvent *)xep->xcookie.data;
        xi2_handle_motion(xi2_event_window(ev), ev);
        break;
    }
    case XI_ButtonPress:
    case XI_ButtonRelease: {
        const XIDeviceEvent *ev = (const XIDeviceEvent *)xep->xcookie.data;
        PlatformWindow *win = xi2_event_window(ev);
        /* Scroll valuators are also replayed as legacy button 4-7 presses for
         * core clients. The valuators are read here, so the emulation is
         * dropped. */
        if (!(ev->flags & XIPointerEmulated))
            handle_pointer_button(win, xep->xcookie.evtype == XI_ButtonPress,
                                  (unsigned)ev->detail,
                                  (i32)ev->event_x, (i32)ev->event_y,
                                  (unsigned)ev->mods.effective, ev->time);
        break;
    }
    case XI_DeviceChanged:
    case XI_HierarchyChanged:
        xi2_scan_devices(dpy);
        break;
    default: break;
    }

    XFreeEventData(dpy, &xep->xcookie);
    return true;
}
#endif /* LIU_HAVE_XINPUT2 */

/* Send the round trip when the map asks for one. Called from the poll, i.e.
 * once per loop iteration; a request is only owed every X_TIME_CAL_INTERVAL_SEC
 * (or right away after a stamp that could not be vouched for), so the steady
 * state cost is one tiny request every ten seconds. */
static void linux_xtime_service(f64 now) {
    Display *dpy = g_platform.display;
    if (!dpy || !g_platform.helper) return;
    if (g_xtime_ping.pending) {
        if (now < g_xtime_ping.deadline) return;
        /* No notify inside the deadline: the server dropped it or the window
         * went away. Forget it so the next owed ping can go out. */
        g_xtime_ping.pending = false;
        g_xtime_ping.timed_out++;
    }
    if (!x_time_map_wants_calibration(&g_xtime, now)) return;
    unsigned long token = (unsigned long)g_xtime_ping.sent;
    g_xtime_ping.t_send   = platform_time_sec();
    XChangeProperty(dpy, g_platform.helper, g_platform.a_liu_time_ping, XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *)&token, 1);
    XFlush(dpy);
    g_xtime_ping.pending  = true;
    g_xtime_ping.deadline = g_xtime_ping.t_send + 2.0;
    g_xtime_ping.sent++;
}

/* The notify came back: `time` is the server's stamp for the moment it processed
 * the request, bracketed by t_send and now. */
static void linux_xtime_ping_landed(Time server_time) {
    if (!g_xtime_ping.pending) return;
    f64 now = platform_time_sec();
    u64 rebases_before = g_xtime.rebases;
    x_time_map_calibrate(&g_xtime, (u32)server_time, g_xtime_ping.t_send, now);
    g_xtime_ping.pending = false;
    g_xtime_ping.landed++;
    if (linux_xtime_probe_on())
        fprintf(stderr, "[xtime] cal server_ms=%u offset=%.6f +-%.6f rtt_ms=%.3f "
                        "%s(n=%llu rebases=%llu)\n",
                (unsigned)(u32)server_time, g_xtime.offset, g_xtime.cal_uncertainty,
                (now - g_xtime_ping.t_send) * 1000.0,
                g_xtime.rebases != rebases_before ? "REBASE " : "",
                (unsigned long long)g_xtime.calibrations,
                (unsigned long long)g_xtime.rebases);
}

static void linux_dispatch_xevent_for(XEvent *xep, PlatformWindow *win);

/* Every event is handled as the event of the window it names: its size, its
 * focus, its input context, and the PlatformEvent it becomes carries it. Events
 * of the root window and the helper have none. Re-entered by the clipboard
 * pump, hence the save and restore. */
static void linux_dispatch_xevent(XEvent *xep) {
    PlatformWindow *saved = g_dispatch_window;
    PlatformWindow *win = xep->type == GenericEvent ? NULL
                                                    : linux_window_of(xep->xany.window);
    g_dispatch_window = win;
    linux_dispatch_xevent_for(xep, win);
    g_dispatch_window = saved;
}

static void linux_dispatch_xevent_for(XEvent *xep, PlatformWindow *win) {
    Display *dpy = g_platform.display;
    if(xep->type==PropertyNotify) {
        if(linux_png_property(dpy,&xep->xproperty))return;
        if(xep->xproperty.window!=RootWindow(dpy,g_platform.screen) &&
           xep->xproperty.window!=g_platform.helper && !win)return;
    }
    if((xep->type==ConfigureNotify || xep->type==MapNotify ||
        xep->type==UnmapNotify) && !win)return;
    if(xep->type==DestroyNotify) {
        for(i32 i=0;i<LINUX_PNG_TRANSFERS;i++)
            if(g_platform.png_transfers[i].requestor==xep->xdestroywindow.window)
                linux_png_transfer_clear(&g_platform.png_transfers[i]);
    }

#ifdef LIU_HAVE_XINPUT2
    /* XI2 rides in on GenericEvent and carries its payload in a cookie that
     * has to be claimed before the next XNextEvent, so it is handled off the
     * original event rather than the copy below. */
    if (xi2_dispatch(dpy, xep)) return;
#endif

    XEvent xe = *xep;

    /* Global hotkey (Quake). The grab is on the ROOT window, so these arrive
     * regardless of focus and must be consumed before the normal key path —
     * otherwise the toggle would also type into the terminal whenever Liu
     * happens to be the focused application. Lock modifiers (Num/Caps/Scroll)
     * are masked out because the grab was registered for all of their
     * permutations. */
    if (xe.type == KeyPress && g_platform.hotkey_keycode &&
        xe.xkey.keycode == g_platform.hotkey_keycode) {
        unsigned int locks = LockMask | Mod2Mask;   /* == kLockPermutations */
        if ((xe.xkey.state & ~locks) == g_platform.hotkey_modmask) {
            if (g_platform.hotkey_cb)
                g_platform.hotkey_cb(g_platform.hotkey_user);
            return;
        }
    }

    /* A real keystroke ends what a reset left to commit: input-method text
     * after it is what was typed next (an IM's own commit has keycode 0). */
    if (xe.type == KeyPress && xe.xkey.keycode != 0 && win) win->ime_reset_pending = false;
    /* An Escape in the middle of a composition: the input method may end it
     * without a word (xim_preedit.h), so its silence is timed from here. */
    if (xe.type == KeyPress && xe.xkey.keycode != 0 && win && win->xic_on_the_spot &&
        win->pre_len > 0 && XLookupKeysym(&xe.xkey, 0) == XK_Escape)
        win->pre_escape_at = platform_time_sec();

    /* IM first refusal: a key that's part of a compose/dead-key/CJK sequence is
     * swallowed here and must not be processed further. */
    if (XFilterEvent(&xe, None)) return;

    switch (xe.type) {
    case KeyPress: {
        unsigned int kc_raw = xe.xkey.keycode;
        if (win) win->pre_escape_at = 0.0;   /* given back, or a commit: an answer */
        KeySym ks = NoSymbol;
        char stack_buf[64];
        char *buf = stack_buf;
        int   cap = (int)sizeof(stack_buf);
        char *heap_buf = NULL;
        int len = 0;
        Status status = 0;

        i32 xim_lookups = 0;                  /* for the probe below */
        i32 typed_chars = 0;                  /* characters this keystroke typed */
        if (win && win->xic) {
            len = Xutf8LookupString(win->xic, &xe.xkey, buf, cap - 1, &ks, &status);
            xim_lookups = 1;
            /* XBufferOverflow is not "too much text to care about": the return
             * value is the number of BYTES the commit needs, and Xlib documents
             * the client re-calling with a buffer that size. An IME can commit
             * more than the 63 bytes the stack buffer holds at once (22
             * three-byte characters, which a CJK or Hangul candidate list easily
             * produces), and that commit must still arrive.
             *
             * A bounded LOOP rather than one retry: an IM that reports one length
             * and then overflows again at that size (the length changed between
             * the two calls) gets a second chance at the new size before the
             * commit is dropped, and a drop is always the whole commit, never a
             * truncated UTF-8 sequence. The cap keeps a hostile or broken IM from
             * turning one keystroke into an unbounded allocation; past 64 KB the
             * commit is dropped. */
            for (i32 attempt = 0; status == XBufferOverflow && attempt < 3; attempt++) {
                XimRetry again = xim_overflow_retry((i32)len);
                if (!again.retry) { len = 0; break; }   /* nothing to fetch, or absurd */
                char *grown = (char *)realloc(heap_buf, (usize)again.cap);
                if (!grown) { len = 0; break; }         /* out of memory — drop, do not truncate */
                heap_buf = grown;
                buf = heap_buf;
                cap = again.cap;
                /* The SAME event, re-looked-up. That is the documented recovery:
                 * the IM's compose state is not consumed by a lookup that
                 * overflowed. */
                len = Xutf8LookupString(win->xic, &xe.xkey, buf, cap - 1, &ks, &status);
                xim_lookups++;
            }
            /* Still short after the retries: give up rather than emit a
             * truncated sequence. */
            if (status == XBufferOverflow) len = 0;
        } else {
            len = XLookupString(&xe.xkey, buf, cap - 1, &ks, NULL);
        }
        if (len < 0) len = 0;
        if (len > cap - 1) len = cap - 1;     /* belt: Xlib must not exceed this */
        /* LIU_XIM_PROBE=1: log what the input method handed over, per KeyPress (a
         * commit over 63 bytes must arrive whole, in one KeyPress, through a
         * second lookup). */
        if (linux_xim_probe_on())
            fprintf(stderr, "[xim] status=%d len=%d cap=%d lookups=%d heap=%d\n",
                    (int)status, len, cap, xim_lookups, heap_buf ? 1 : 0);
        if (ks == NoSymbol)
            ks = XkbKeycodeToKeysym(dpy, (XKeyCode)kc_raw, 0, 0);
        buf[len] = '\0';

        KeyCode key = map_keysym(ks);
        /* The state after this key: X11 sends the one before it (key_mods.h). */
        u32 mods = key_mods_after(get_mods(xe.xkey.state), key, true);
        /* The key a chord names: the layout's symbol, or — when that names no
         * key Liu knows and a modifier makes it a chord — the same physical
         * key's other symbols (x11_keypos.h): Ctrl+Shift+T on a Cyrillic
         * layout, Alt+1 on AZERTY, Ctrl+C in a shell on either. A plain key
         * keeps its own symbol: it types text, it names no chord. */
        if (key == KEY_UNKNOWN && (mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)))
            key = linux_key_by_position(dpy, kc_raw, xe.xkey.state);
        /* ONE reading of the native event's time, shared by the KEY_DOWN and every
         * CHAR_INPUT it produces. That shared value is the only thing tying them
         * together downstream: bench_key_begin() treats a repeated timestamp as
         * the same keystroke, so separate clock readings would turn one
         * multi-codepoint IME commit into several independent latency samples. It
         * is the SERVER's time for the keystroke, mapped onto the local clock, so
         * the wait in the X queue is inside the measurement. */
        bool ev_certain = false;
        f64 ev_time = linux_map_x_time(xe.xkey.time, &ev_certain);

        /* Autorepeat: X delivers a synthetic KeyRelease+KeyPress pair at the
         * same timestamp for a held key. Peek to flag it. */
        bool repeat = false;
        if (XEventsQueued(dpy, QueuedAfterReading)) {
            XEvent nxt;
            XPeekEvent(dpy, &nxt);
            if (nxt.type == KeyPress &&
                nxt.xkey.time == xe.xkey.time &&
                nxt.xkey.keycode == kc_raw)
                repeat = true;
        }

        push_event((PlatformEvent){
            .type = EVENT_KEY_DOWN,
            .timestamp = ev_time,
            .timestamp_uncertain = !ev_certain,
            .key = { .key = key, .mods = mods, .repeat = repeat }
        });

        /* Super (Cmd-equivalent) combos: KEY_DOWN only — shortcut handled in
         * the main loop (mirrors macOS keyDown early-return on MOD_SUPER).
         *
         * `goto`, not `break`: an oversized IME commit is looked up into a heap
         * buffer and every exit from this case has to release it. */
        if (mods & MOD_SUPER) goto key_press_done;

        /* Ctrl+a..z: emit the control codepoint directly (the IM produces no
         * text for control chars) and stop, so nothing is emitted twice. By the
         * key, not the keysym: Ctrl+C is ^C on a Cyrillic layout too, and with
         * Caps Lock on (whose keysym is the capital). Shift keeps it a chord. */
        if ((mods & MOD_CTRL) && !(mods & MOD_SHIFT) && key >= KEY_A && key <= KEY_Z) {
            push_event((PlatformEvent){
                .type = EVENT_CHAR_INPUT,
                .timestamp = ev_time,
                .timestamp_uncertain = !ev_certain,
                .char_input = { .codepoint = (u32)(key - KEY_A + 1), .mods = mods }
            });
            goto key_press_done;
        }
        /* Other Ctrl chords: the control character Xlib's own translation
         * gives the keysym, when it gives one. Ctrl+[ is ESC (vi's), Ctrl+\
         * and Ctrl+] are FS and GS (a shell's Ctrl+\), Ctrl+Space and Ctrl+@
         * NUL, Ctrl+_ and Ctrl+/ US, the same on any layout that types those
         * symbols. Anything else a Ctrl chord produces is no text. */
        if (mods & MOD_CTRL) {
            u8 c0 = (u8)buf[0];
            if (len == 1 && (c0 < 0x20 || c0 == 0x7F))
                push_event((PlatformEvent){
                    .type = EVENT_CHAR_INPUT,
                    .timestamp = ev_time,
                    .timestamp_uncertain = !ev_certain,
                    .char_input = { .codepoint = c0, .mods = mods }
                });
            goto key_press_done;
        }

        /* Decode committed UTF-8 into codepoints; one CHAR_INPUT per codepoint
         * (NOT per byte). Skip C0/DEL: Enter/Tab/Esc/Backspace go via the
         * KEY_DOWN channel. */
        if ((status == XLookupChars || status == XLookupBoth || !win || !win->xic)
            && len > 0) {
            i32 emitted = 0;
            /* Xlib hands an input method's commit over as a KeyPress of
             * keycode 0; after platform_ime_reset it is the ended composition. */
            bool reset_commit = xe.xkey.keycode == 0 && win && win->ime_reset_pending;
            u32 committed[LINUX_PREEDIT_MAX];
            i32 ncommitted = 0;
            for (int i = 0; i < len; ) {
                u32 cp = 0; int n = 0;
                unsigned char c = (unsigned char)buf[i];
                if      (c < 0x80)          { cp = c;        n = 1; }
                else if ((c & 0xE0) == 0xC0){ cp = c & 0x1F; n = 2; }
                else if ((c & 0xF0) == 0xE0){ cp = c & 0x0F; n = 3; }
                else if ((c & 0xF8) == 0xF0){ cp = c & 0x07; n = 4; }
                else { i++; continue; }                /* invalid lead byte */
                if (i + n > len) break;                 /* truncated */
                for (int k = 1; k < n; k++)
                    cp = (cp << 6) | ((unsigned char)buf[i + k] & 0x3F);
                i += n;
                if (cp < 0x20 || cp == 0x7F) continue;  /* skip C0 / DEL */
                push_event((PlatformEvent){
                    .type = EVENT_CHAR_INPUT,
                    .timestamp = ev_time,
                    .timestamp_uncertain = !ev_certain,
                    .char_input = { .codepoint = cp, .mods = mods, .ime_reset = reset_commit }
                });
                emitted++;
                if (ncommitted < LINUX_PREEDIT_MAX) committed[ncommitted++] = cp;
            }
            typed_chars = emitted;
            /* The composition this commit was (xim_preedit.h): it is over,
             * whether or not the input method says so. */
            if (xe.xkey.keycode == 0 && win && win->xic_on_the_spot &&
                xim_commit_ends_composition(win->pre_cp, win->pre_len, committed, ncommitted)) {
                win->pre_len = win->pre_caret = 0;
                linux_preedit_push(win);
            }
            if (linux_xim_probe_on())
                fprintf(stderr, "[xim] emitted %d codepoint(s) from %d byte(s), "
                                "queue count=%d spill=%u dropped_critical=%u\n",
                        emitted, len, g_evq.count, g_evq.overflow_count,
                        g_evq.dropped_critical);
        }
    key_press_done:
        /* A screen reader hears the echo of this key right behind it, one
         * keystroke an insertion (atspi_typing.h). */
        if (win) atspi_window_key(&win->a11y, (u32)typed_chars);
        free(heap_buf);
        break;
    }
    case KeyRelease: {
        /* Swallow the synthetic release half of an autorepeat. */
        if (XEventsQueued(dpy, QueuedAfterReading)) {
            XEvent nxt;
            XPeekEvent(dpy, &nxt);
            if (nxt.type == KeyPress &&
                nxt.xkey.time == xe.xkey.time &&
                nxt.xkey.keycode == xe.xkey.keycode)
                break;
        }
        KeySym ks = XkbKeycodeToKeysym(dpy, (XKeyCode)xe.xkey.keycode, 0, 0);
        bool up_certain = false;
        f64  up_time = linux_map_x_time(xe.xkey.time, &up_certain);
        push_event((PlatformEvent){
            .type = EVENT_KEY_UP,
            .timestamp = up_time,
            .timestamp_uncertain = !up_certain,
            .key = { .key = map_keysym(ks),
                     .mods = key_mods_after(get_mods(xe.xkey.state), map_keysym(ks), false) }
        });
        break;
    }
    case ButtonPress:
    case ButtonRelease:
        if (win) g_pointer_window = win;
        handle_pointer_button(win, xe.type == ButtonPress,
                              xe.xbutton.button, xe.xbutton.x, xe.xbutton.y,
                              xe.xbutton.state, xe.xbutton.time);
        break;
    case MotionNotify:
        if (win) g_pointer_window = win;
        linux_edge_hover(win, xe.xmotion.x, xe.xmotion.y);
        linux_move_pending_motion(win);
        push_event((PlatformEvent){
            .type = EVENT_MOUSE_MOVE,
            .mouse = { .button = -1, .x = xe.xmotion.x, .y = xe.xmotion.y,
                       .mods = get_mods(xe.xmotion.state) }
        });
        break;
    case ConfigureNotify: {
        /* Update the cached size and emit EVENT_RESIZE only when dimensions
         * change; a move-only ConfigureNotify must not trigger a layout
         * pass. */
        i32 nw = xe.xconfigure.width;
        i32 nh = xe.xconfigure.height;
        if (win && (win->width != nw || win->height != nh)) {
            win->width  = nw;
            win->height = nh;
            push_event((PlatformEvent){
                .type = EVENT_RESIZE,
                .resize = { .width = nw, .height = nh }
            });
        }
        break;
    }
    case ClientMessage: {
        const XClientMessageEvent *cm = &xe.xclient;

        /* WM_PROTOCOLS carries both the close request and the liveness ping,
         * so only WM_DELETE_WINDOW is a close: a ping, or the XDND handshake
         * below, must never quit the terminal. */
        if (cm->message_type == g_platform.a_wm_protocols) {
            Atom proto = (Atom)cm->data.l[0];
            if (win && proto == win->wm_delete) {
                push_event((PlatformEvent){ .type = EVENT_CLOSE });
            } else if (proto == g_platform.a_net_wm_ping) {
                /* Echo back to the root so the WM does not treat the window as
                 * hung. */
                XEvent reply = xe;
                reply.xclient.window = RootWindow(dpy, g_platform.screen);
                XSendEvent(dpy, reply.xclient.window, False,
                           SubstructureNotifyMask | SubstructureRedirectMask, &reply);
            }
            break;
        }

        if (!win) break;

        if (cm->message_type == g_platform.a_xdnd_enter) {
            g_platform.xdnd_source  = (Window)cm->data.l[0];
            g_platform.xdnd_version = (cm->data.l[1] >> 24) & 0xFF;
            if (!xdnd_source_has_uri_list(cm)) g_platform.xdnd_source = None;
            break;
        }

        if (cm->message_type == g_platform.a_xdnd_position) {
            Window source = (Window)cm->data.l[0];
            bool accept = (g_platform.xdnd_source == source && source != None);
            if (accept) {
                /* The position is in ROOT coordinates; every other mouse event
                 * this backend emits is window-relative, so translate. */
                i32 rx = (i32)((cm->data.l[2] >> 16) & 0xFFFF);
                i32 ry = (i32)(cm->data.l[2] & 0xFFFF);
                Window child = None;
                int lx = 0, ly = 0;
                XTranslateCoordinates(dpy, RootWindow(dpy, g_platform.screen),
                                      win->xwindow, rx, ry, &lx, &ly, &child);
                g_platform.xdnd_x = lx;
                g_platform.xdnd_y = ly;
                if (!g_platform.xdnd_inside) {
                    g_platform.xdnd_inside = true;
                    push_event((PlatformEvent){
                        .type = EVENT_DRAG_ENTER, .drag = { .x = lx, .y = ly }
                    });
                } else {
                    /* Keep hover live: no motion events arrive during a drag. */
                    push_event((PlatformEvent){
                        .type = EVENT_DRAG_ENTER, .drag = { .x = lx, .y = ly }
                    });
                }
            }
            xdnd_send_status(win->xwindow, source, accept);
            break;
        }

        if (cm->message_type == g_platform.a_xdnd_leave) {
            if (g_platform.xdnd_inside)
                push_event((PlatformEvent){ .type = EVENT_DRAG_EXIT });
            g_platform.xdnd_inside = false;
            g_platform.xdnd_source = None;
            break;
        }

        if (cm->message_type == g_platform.a_xdnd_drop) {
            Window source = (Window)cm->data.l[0];
            if (g_platform.xdnd_source != source || source == None) {
                xdnd_send_finished(win->xwindow, source, false);
                g_platform.xdnd_inside = false;
                break;
            }
            /* Ask for the payload; the bytes arrive as SelectionNotify, which
             * is where the drop events are actually emitted. Version 0 sources
             * predate the timestamp field. */
            Time t = (g_platform.xdnd_version >= 1)
                   ? (Time)cm->data.l[2] : CurrentTime;
            XConvertSelection(dpy, g_platform.a_xdnd_selection,
                              g_platform.a_uri_list, g_platform.a_xdnd_prop,
                              win->xwindow, t);
            XFlush(dpy);
            break;
        }
        break;
    }

    /* Render gate: a fully covered window produces no visible pixels, so the
     * main loop skips the frame entirely. Emit VISIBILITY so it wakes and
     * repaints the moment the window is uncovered again. */
    case VisibilityNotify: {
        if (!win || xe.xvisibility.window != win->xwindow) break;
        bool now_obscured =
            (xe.xvisibility.state == VisibilityFullyObscured);
        if (now_obscured != win->obscured) {
            win->obscured = now_obscured;
            push_event((PlatformEvent){ .type = EVENT_VISIBILITY });
        }
        break;
    }
    case UnmapNotify:
        if (win && xe.xunmap.window == win->xwindow && win->mapped) {
            win->mapped = false;
            atspi_window_set_showing(&win->a11y, false);
            push_event((PlatformEvent){ .type = EVENT_VISIBILITY });
        }
        break;
    case MapNotify:
        if (win && xe.xmap.window == win->xwindow && !win->mapped) {
            win->mapped = true;
            atspi_window_set_showing(&win->a11y, true);
            push_event((PlatformEvent){ .type = EVENT_VISIBILITY });
        }
        break;

    case PropertyNotify: {
        /* Desktop scale changed (display settings, or a WM that rewrites
         * Xft.dpi when the window moves to a differently-scaled monitor).
         * Re-read and tell the app, which rebuilds the font atlas at the new
         * density, so the terminal follows the new scale without a restart.
         */
        if (xe.xproperty.window == RootWindow(dpy, g_platform.screen)) {
            if (xe.xproperty.atom == XA_RESOURCE_MANAGER) {
                f32 fresh = linux_compute_dpi_scale();
                if (fabsf(fresh - g_platform.dpi_scale) > 0.001f) {
                    g_platform.dpi_scale = fresh;
                    /* The setting is the desktop's, so it is every window's. */
                    for (i32 i = 0; i < g_window_count; i++)
                        push_event((PlatformEvent){
                            .type = EVENT_DPI_CHANGE, .window = g_windows[i],
                            .dpi = { .scale = fresh }
                        });
                }
            }
            break;
        }

        if (xe.xproperty.window == g_platform.helper) {
            if (xe.xproperty.atom == g_platform.a_liu_time_ping)
                linux_xtime_ping_landed(xe.xproperty.time);
            break;
        }
        if (!win) break;
        if (xe.xproperty.atom == g_platform.a_net_wm_state) {
            /* _NET_WM_STATE is rewritten wholesale by the WM, so re-read the
             * list rather than trying to diff it. */
            bool fs = x11_has_wm_state(win->xwindow,
                                       g_platform.a_net_wm_state_fullscreen);
            if (fs != win->fullscreen) {
                win->fullscreen = fs;
                /* The toolbar's left inset is derived from this; a resize
                 * always accompanies the transition, so no extra event is
                 * needed beyond keeping the mirror correct. */
            }
            bool maxed = x11_has_wm_state(win->xwindow, g_platform.a_net_wm_state_max_vert) &&
                         x11_has_wm_state(win->xwindow, g_platform.a_net_wm_state_max_horz);
            if (maxed != win->maximized) {
                win->maximized = maxed;
                /* The maximize button draws the other glyph. */
                push_event((PlatformEvent){ .type = EVENT_VISIBILITY });
            }
            bool hidden = x11_has_wm_state(win->xwindow,
                                           g_platform.a_net_wm_state_hidden);
            if (hidden == win->mapped) {
                win->mapped = !hidden;
                atspi_window_set_showing(&win->a11y, !hidden);
                push_event((PlatformEvent){ .type = EVENT_VISIBILITY });
            }
        }
        break;
    }

    /* X reports grabs through the focus events: activating ANY keyboard or
     * pointer grab (Liu's own Quake hotkey, a WM shortcut, another client's
     * passive grab, opening a menu) emits FocusOut with mode NotifyGrab, and
     * releasing it emits FocusIn with NotifyUngrab. Neither is a focus change
     * (the window keeps the focus throughout), so forwarding them would make
     * the app think it was backgrounded for the duration of every grab. With
     * Quake mode on that would break the toggle: the hotkey's grab would emit
     * a blur, the blur handler would hide the window, and the hotkey would
     * show it again, so the toggle would appear to do nothing.
     *
     * NotifyInferior is the same story for a different reason: focus merely
     * moved between Liu's own window and a child of it. */
    case FocusIn:
    case FocusOut: {
        if (xe.xfocus.mode == NotifyGrab || xe.xfocus.mode == NotifyUngrab)
            break;
        if (xe.xfocus.detail == NotifyInferior ||
            xe.xfocus.detail == NotifyPointer ||
            xe.xfocus.detail == NotifyPointerRoot)
            break;

        bool focused = (xe.type == FocusIn);
        if (win) {
            win->focused = focused;
            if (focused) g_key_window = win;
        }
        if (win && win->xic) {
            if (focused) XSetICFocus(win->xic);
            else         XUnsetICFocus(win->xic);
        }

        if (focused && win) {
            /* Clear any pending attention request: the window has the focus
             * now, and a stale urgency hint keeps the taskbar entry flashing
             * forever under WMs that don't drop it on focus themselves. */
            x11_wm_state(win->xwindow, LIU_NET_WM_STATE_REMOVE,
                         g_platform.a_net_wm_state_demands_attention, None);
            XWMHints *hints = XGetWMHints(dpy, win->xwindow);
            if (hints) {
                if (hints->flags & XUrgencyHint) {
                    hints->flags &= ~(long)XUrgencyHint;
                    XSetWMHints(dpy, win->xwindow, hints);
                }
                XFree(hints);
            }
        }

        /* A screen reader follows the keyboard: window activation and the
         * terminal's focused state. */
        if (win) atspi_window_set_active(&win->a11y, focused);

        /* Hiding the Quake window is itself what takes the focus away, and the
         * app's blur handler responds to EVENT_BLUR by toggling Quake, so
         * forwarding this one would immediately re-show the window just
         * hidden, then hide it again, ad infinitum. A hidden window has no
         * focus to lose, so the event carries no information: drop it. */
        if (!focused && win && win->quake && !win->quake_shown) break;

        push_event((PlatformEvent){
            .type = focused ? EVENT_FOCUS : EVENT_BLUR,
            .focus.focused = focused
        });
        break;
    }

    case SelectionRequest: {
        /* Another client wants the CLIPBOARD Liu owns. Serve
         * UTF8_STRING/TEXT/STRING, answer TARGETS, refuse the rest (ICCCM).
         */
        XSelectionRequestEvent *req = &xe.xselectionrequest;
        XSelectionEvent ev = {0};
        ev.type      = SelectionNotify;
        ev.display   = req->display;
        ev.requestor = req->requestor;
        ev.selection = req->selection;
        ev.target    = req->target;
        ev.time      = req->time;
        Atom prop = (req->property != None) ? req->property : req->target;
        ev.property  = None;   /* default: refused */

        if(req->selection==g_platform.a_clipboard && g_platform.clip_png) {
            if(req->target==g_platform.a_targets) {
                Atom offer[]={g_platform.a_targets,g_platform.a_png};
                XChangeProperty(dpy,req->requestor,prop,XA_ATOM,32,PropModeReplace,(u8 *)offer,2);
                ev.property=prop;
            } else if(req->target==g_platform.a_png && linux_png_send(dpy,req,prop)) ev.property=prop;
        } else {
            /* CLIPBOARD and PRIMARY are served alike, each from its own text. */
            const char *serve = req->selection == g_platform.a_clipboard
                              ? g_platform.clip_text
                              : req->selection == XA_PRIMARY ? g_platform.primary_text : NULL;
            bool hint = serve && req->selection == g_platform.a_clipboard &&
                        g_platform.clip_secret;
            if (serve && req->target == g_platform.a_targets) {
                Atom offer[5] = { g_platform.a_targets, g_platform.a_utf8,
                                  g_platform.a_text, XA_STRING };
                int n = 4;
                if (hint) offer[n++] = g_platform.a_kde_pw_hint;
                XChangeProperty(dpy, req->requestor, prop, XA_ATOM, 32,
                                PropModeReplace, (const unsigned char *)offer, n);
                ev.property = prop;
            } else if (serve && (req->target == g_platform.a_utf8 ||
                                 req->target == g_platform.a_text ||
                                 req->target == XA_STRING)) {
                XChangeProperty(dpy, req->requestor, prop, req->target, 8,
                                PropModeReplace, (const unsigned char *)serve,
                                (int)strlen(serve));
                ev.property = prop;
            } else if (hint && req->target == g_platform.a_kde_pw_hint) {
                static const char secret[] = "secret";
                XChangeProperty(dpy, req->requestor, prop, XA_STRING, 8,
                                PropModeReplace, (const unsigned char *)secret,
                                (int)(sizeof secret - 1));
                ev.property = prop;
            }
        }
        XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&ev);
        XFlush(dpy);
        break;
    }
    case SelectionClear:
        if (xe.xselectionclear.selection == XA_PRIMARY) {
            free(g_platform.primary_text);
            g_platform.primary_text = NULL;
        }
        if (xe.xselectionclear.selection == g_platform.a_clipboard) {
            free(g_platform.clip_text);
            g_platform.clip_text = NULL;
            g_platform.clip_secret = false;
            linux_png_release(g_platform.clip_png);g_platform.clip_png=NULL;
        }
        break;
    case SelectionNotify: {
        XSelectionEvent *sel = &xe.xselection;

        /* Drop payload, not a clipboard paste — same event, different
         * selection, so it must be split off before the clipboard path
         * consumes (and frees) it. */
        if (sel->selection == g_platform.a_xdnd_selection) {
            bool accepted = false;
            if (sel->property != None && win) {
                Atom actual_type = None; int actual_fmt = 0;
                unsigned long nitems = 0, bytes_after = 0;
                unsigned char *data = NULL;
                if (XGetWindowProperty(dpy, win->xwindow, g_platform.a_xdnd_prop,
                                       0, (~0L), True /*delete*/, AnyPropertyType,
                                       &actual_type, &actual_fmt, &nitems,
                                       &bytes_after, &data) == Success && data) {
                    if (actual_fmt == 8 && nitems > 0 &&
                        nitems <= PLATFORM_MAX_CLIPBOARD_BYTES) {
                        xdnd_emit_drops((const char *)data, (usize)nitems);
                        accepted = true;
                    }
                    XFree(data);
                }
            }
            if (g_platform.xdnd_inside)
                push_event((PlatformEvent){ .type = EVENT_DRAG_EXIT });
            g_platform.xdnd_inside = false;
            if (win) xdnd_send_finished(win->xwindow, g_platform.xdnd_source, accepted);
            g_platform.xdnd_source = None;
            break;
        }

        /* Only the answer to Liu's outstanding XConvertSelection counts: the
         * requestor must be this window, the selection CLIPBOARD, and the
         * property one of the few Liu ever asks an owner to fill (or None = a
         * refusal). Any X client can XSendEvent a SelectionNotify naming its
         * own window and property; reading it verbatim would publish
         * attacker-chosen bytes as the clipboard on the next paste. A property
         * is only ever read from Liu's own window, and only after it has been
         * matched against Liu's own atoms. */
        if (sel->requestor != g_platform.helper || sel->requestor == None ||
            sel->selection != g_platform.clip_req_selection)
            break;
        bool our_property = sel->property == None;
        for (i32 slot = 0; slot < CLIP_REQ_SLOTS && !our_property; slot++)
            our_property = sel->property == g_platform.a_liu_sel[slot];
        if (!our_property) break;

        /* Is it the answer to the request that is waiting NOW? A read that
         * timed out is over; its answer, arriving late, must not be taken
         * as the answer to whichever read comes next (clip_request.h). */
        ClipRequest *req = &g_platform.clip_req;
        if (!clip_request_accepts(req, true,
                                  sel->property == None ? 0ul : (unsigned long)sel->property,
                                  (unsigned long)sel->time)) {
            clip_request_stale(req);
            /* The late owner still wrote into the helper window: do not keep
             * it. */
            if (sel->property != None) XDeleteProperty(dpy, g_platform.helper, sel->property);
            break;
        }
        if (sel->property == None) {                 /* the owner refused */
            clip_request_complete(req, NULL, 0, true);
            break;
        }
        {
            Atom actual_type = None; int actual_fmt = 0;
            unsigned long nitems = 0, bytes_after = 0;
            unsigned char *data = NULL;
            if (XGetWindowProperty(dpy, g_platform.helper, sel->property,
                                   0, (~0L), True /*delete*/, AnyPropertyType,
                                   &actual_type, &actual_fmt, &nitems,
                                   &bytes_after, &data) == Success && data) {
                /* INCR (a transfer in pieces) is not supported: it is a
                 * refusal, said as one, and the property is already deleted. */
                bool usable = actual_type != g_platform.a_incr && actual_fmt == 8 &&
                              nitems <= PLATFORM_MAX_CLIPBOARD_BYTES;
                clip_request_complete(req, usable ? data : NULL,
                                      usable ? (usize)nitems : 0, !usable);
                XFree(data);
            } else {
                clip_request_complete(req, NULL, 0, true);
            }
        }
        break;
    }
    }
}

void platform_poll_events(void) {
    Display *dpy = g_platform.display;
    if (!dpy) return;
    linux_xtime_service(platform_time_sec());
    linux_png_expire(platform_time_sec());
    linux_clip_secret_tick(platform_time_sec());
    /* The accessibility bus: what a screen reader asked since the last turn
     * is answered now, without blocking. */
    atspi_service();
    /* A composition whose Escape went unanswered is over (xim_preedit.h). */
    f64 now = platform_time_sec();
    for (i32 i = 0; i < g_window_count; i++) {
        PlatformWindow *w = g_windows[i];
        if (!xim_escape_cancelled(w->pre_escape_at, now)) continue;
        w->pre_escape_at = 0.0;
        if (w->pre_len) {
            w->pre_len = w->pre_caret = 0;
            linux_preedit_push(w);
        }
    }
    while (XPending(dpy) && g_evq.count < 128 && !g_evq.overflow_head) {
        XEvent xe;
        XNextEvent(dpy, &xe);
        linux_dispatch_xevent(&xe);
    }
}

void platform_wait_events(f64 timeout_sec) {
    if (g_evq.count || g_evq.overflow_head) return;
    /* Every exit expires the pending holds: this function is NOT called on
     * every iteration (main.c skips it whenever timeout == 0), so a hold left
     * behind here could survive several frames and cost a pane up to one
     * idle-ladder tick of read latency. */
    if (!g_platform.display) { watch_hold_pass_end(false); return; }
    if (XPending(g_platform.display) > 0) { watch_hold_pass_end(false); return; }

    int xfd = ConnectionNumber(g_platform.display);
    i32 watch_count = 0;
    pthread_mutex_lock(&g_platform.watch_lock);
    watch_count = g_platform.watch_count;
    if (watch_count < 0 || watch_count > PLATFORM_MAX_WATCH_FDS) {
        pthread_mutex_unlock(&g_platform.watch_lock);
        watch_hold_pass_end(false);
        return;
    }
    /* X connection + wake pipe + (optional) inotify + (optional) the
     * accessibility bus + the session fds. The first four are wake-up-only:
     * they must NOT feed fd_fire_count, or an idle terminal whose config file
     * lives in a busy directory would drain sessions every frame and defeat
     * the zero-cost idle path. */
    short a11y_events = 0;
    int   a11y_fd = atspi_poll_fd(&a11y_events);
    i32 nfds = 1 + (g_platform.wake_pipe[0] >= 0 ? 1 : 0) +
               (g_platform.inotify_fd >= 0 ? 1 : 0) + (a11y_fd >= 0 ? 1 : 0) +
               watch_count;
    if (nfds > g_platform.poll_cap) {
        i32 new_cap = g_platform.poll_cap ? g_platform.poll_cap : 16;
        while (new_cap < nfds) {
            if (new_cap > PLATFORM_MAX_WATCH_FDS / 2) {
                new_cap = nfds;
                break;
            }
            new_cap *= 2;
        }
        usize bytes;
        if (!checked_mul_usize((usize)new_cap, sizeof(struct pollfd), &bytes)) {
            pthread_mutex_unlock(&g_platform.watch_lock);
            watch_hold_pass_end(false);
            return;
        }
        struct pollfd *grown = realloc(g_platform.poll_fds, bytes);
        if (!grown) {
            pthread_mutex_unlock(&g_platform.watch_lock);
            watch_hold_pass_end(false);
            return;
        }
        g_platform.poll_fds = grown;
        g_platform.poll_cap = new_cap;
    }
    struct pollfd *pfds = g_platform.poll_fds;

    i32 idx = 0;
    pfds[idx++] = (struct pollfd){ .fd = xfd, .events = POLLIN };
    if (g_platform.wake_pipe[0] >= 0)
        pfds[idx++] = (struct pollfd){ .fd = g_platform.wake_pipe[0], .events = POLLIN };
    if (g_platform.inotify_fd >= 0)
        pfds[idx++] = (struct pollfd){ .fd = g_platform.inotify_fd, .events = POLLIN };
    if (a11y_fd >= 0)
        pfds[idx++] = (struct pollfd){ .fd = a11y_fd, .events = a11y_events };
    for (i32 i = 0; i < watch_count; i++) {
        int wfd = g_platform.watch_fds[i];
        /* Structural answer in Liu's shape: a pane that spent its
         * whole read budget last frame is masked OUT of this one poll(), so a
         * level-triggered fd with data still buffered cannot spin the loop.
         * POLLERR/POLLHUP are reported regardless of .events, so a child that
         * dies while held is still noticed by the fire count below. */
        pfds[idx++] = (struct pollfd){
            .fd = wfd, .events = watch_hold_is_set(wfd) ? 0 : POLLIN };
    }
    pthread_mutex_unlock(&g_platform.watch_lock);
    /* Consume the holds HERE, where the mask was applied — not in
     * platform_resume_watches (a no-op on X11), which would expire them before
     * the poll() they are meant to affect. */
    watch_hold_pass_end(false);

    /* A screen reader's trailing change check (only while one listens and
     * something changed inside the post interval) ends the wait on time. */
    timeout_sec = atspi_wait_limit(timeout_sec);
    /* ...and so does an Escape the input method has not answered yet. */
    for (i32 i = 0; i < g_window_count; i++) {
        f64 at = g_windows[i]->pre_escape_at;
        if (at <= 0.0) continue;
        f64 left = at + XIM_ESCAPE_QUIET_SEC - platform_time_sec();
        if (left < 0.0) left = 0.0;
        if (timeout_sec < 0.0 || left < timeout_sec) timeout_sec = left;
    }

    int timeout_ms;
    if (!isfinite(timeout_sec)) timeout_ms = 0;
    else if (timeout_sec < 0.0) timeout_ms = -1;
    else {
        f64 ms = timeout_sec * 1000.0;
        timeout_ms = ms > 2147483647.0 ? 2147483647 : (int)ceil(ms);
    }
    int ready = poll(pfds, (nfds_t)nfds, timeout_ms);
    if (ready > 0) {
        /* Count only the watched fds for platform_take_fd_fire_count(). The X11
         * connection fd and the internal wake pipe still wake event processing
         * (the call returns and the main loop pumps), but they are not session
         * I/O so they must not defeat the idle drain skip. */
        i32 watch_base = nfds - watch_count;
        for (i32 i = watch_base; i < nfds; i++)
            if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP))
                atomic_fetch_add_explicit(&g_platform.fd_fire_count, 1,
                                          memory_order_relaxed);
    }
    linux_drain_wake_pipe();
}

bool platform_next_event(PlatformEvent *event) {
    bool got = platform_event_queue_pop(&g_evq, event);
    /* The app is done with the event it was handed last time; the one it gets
     * now keeps what it owns until the next call. */
    platform_payload_delivered(&g_payloads, got ? event : NULL);
    return got;
}

void platform_event_queue_stats(i32 *high_water, u32 *coalesced, u32 *evicted,
                                u32 *dropped_lowpri, u32 *dropped_critical) {
    if (high_water)       *high_water       = g_evq.high_water;
    if (coalesced)        *coalesced        = g_evq.coalesced;
    if (evicted)          *evicted          = g_evq.evicted_lowpri;
    if (dropped_lowpri)   *dropped_lowpri   = g_evq.dropped_lowpri;
    if (dropped_critical) *dropped_critical = g_evq.dropped_critical;
}

void platform_watch_socket(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_platform.watch_lock);
    for (i32 i = 0; i < g_platform.watch_count; i++) {
        if (g_platform.watch_fds[i] == fd) {
            pthread_mutex_unlock(&g_platform.watch_lock);
            return;
        }
    }
    if (g_platform.watch_count == g_platform.watch_cap) {
        if (g_platform.watch_cap >= PLATFORM_MAX_WATCH_FDS) {
            pthread_mutex_unlock(&g_platform.watch_lock);
            return;
        }
        i32 new_cap = g_platform.watch_cap
                    ? g_platform.watch_cap * 2 : 32;
        if (new_cap > PLATFORM_MAX_WATCH_FDS) new_cap = PLATFORM_MAX_WATCH_FDS;
        usize bytes;
        if (!checked_mul_usize((usize)new_cap, sizeof(int), &bytes)) {
            pthread_mutex_unlock(&g_platform.watch_lock);
            return;
        }
        int *new_fds = realloc(g_platform.watch_fds, bytes);
        if (!new_fds) {
            pthread_mutex_unlock(&g_platform.watch_lock);
            return;
        }
        g_platform.watch_fds = new_fds;
        g_platform.watch_cap = new_cap;
    }
    g_platform.watch_fds[g_platform.watch_count++] = fd;
    pthread_mutex_unlock(&g_platform.watch_lock);
    linux_wake_waiter();
}

void platform_unwatch_socket(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_platform.watch_lock);
    for (i32 i = 0; i < g_platform.watch_count; i++) {
        if (g_platform.watch_fds[i] == fd) {
            g_platform.watch_fds[i] = g_platform.watch_fds[g_platform.watch_count - 1];
            g_platform.watch_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_platform.watch_lock);
    linux_wake_waiter();
}

void platform_resume_watches(void) {
    /* Linux uses poll() level-triggered fd readiness, so there is no suspended
     * dispatch source to re-arm after the session drain. */
}

i32 platform_take_fd_fire_count(void) {
    return atomic_exchange_explicit(&g_platform.fd_fire_count, 0,
                                    memory_order_acq_rel);
}

/* Read a selection (CLIPBOARD or PRIMARY) as UTF-8. `ours` is the text Liu
 * serves on it, returned as-is when Liu owns it (a client cannot reliably
 * XConvertSelection from itself). */
static const char *linux_selection_read(Atom selection, const char *ours) {
    Display *dpy = g_platform.display;
    Window helper = g_platform.helper;
    if (!dpy || !helper) return "";

    if (XGetSelectionOwner(dpy, selection) == helper)
        return ours ? ours : "";

    /* Ask the owner to write UTF8_STRING into a transfer property, then pump
     * the normal dispatch path until ITS SelectionNotify (or a ~0.5s timeout).
     *
     * Every read is a request of its own (clip_request.h): its own property,
     * its own result. Starting one discards the previous read's text, so a read
     * nobody answers returns "", never whatever the last successful read left
     * in the buffer (an old paste). */
    ClipRequest *req = &g_platform.clip_req;
    Atom prop = g_platform.a_liu_sel[clip_request_slot(req->generation + 1)];
    /* Whatever a reply to a request CLIP_REQ_SLOTS reads ago left there. */
    XDeleteProperty(dpy, helper, prop);
    clip_request_begin(req, (unsigned long)prop, 0 /* CurrentTime */);
    g_platform.clip_req_selection = selection;
    XConvertSelection(dpy, selection, g_platform.a_utf8, prop, helper, CurrentTime);
    XFlush(dpy);

    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    while (req->state == CLIP_REQ_PENDING) {
        struct pollfd pfd = { .fd = ConnectionNumber(dpy), .events = POLLIN };
        (void)poll(&pfd, 1, 50);
        platform_poll_events();   /* drains SelectionNotify into the request */
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        f64 el = (now.tv_sec - start.tv_sec) +
                 (now.tv_nsec - start.tv_nsec) / 1e9;
        if (el > 0.5) {            /* owner unresponsive / no owner — give up */
            clip_request_timeout(req);
            fprintf(stderr, "liu: the selection owner did not answer; nothing to paste\n");
        }
    }
    return clip_request_text(req);
}

const char *platform_clipboard_get(void) {
    return linux_selection_read(g_platform.a_clipboard, g_platform.clip_text);
}

const char *platform_primary_get(void) {
    return linux_selection_read(XA_PRIMARY, g_platform.primary_text);
}

/* Own `selection` with a copy of `text` (validated like a clipboard write);
 * the copy replaces *slot only once the server has made this client the
 * owner. */
static PlatformClipResult linux_selection_own(Atom selection, const char *text,
                                              char **slot) {
    Display *dpy = g_platform.display;
    Window helper = g_platform.helper;
    if (!dpy || !helper) return PLATFORM_CLIP_FAILED;
    if (!text) return PLATFORM_CLIP_BAD_TEXT;
    usize len = strnlen(text, PLATFORM_MAX_CLIPBOARD_BYTES + 1u);
    if (len > PLATFORM_MAX_CLIPBOARD_BYTES) return PLATFORM_CLIP_TOO_LARGE;
    /* Served as UTF8_STRING, so it has to be UTF-8 — the same refusal the
     * macOS pasteboard makes, decided before anything is replaced. */
    if (!utf8_validate((const u8 *)text, len)) return PLATFORM_CLIP_BAD_TEXT;
    char *copy = malloc(len + 1u);
    if (!copy) return PLATFORM_CLIP_FAILED;
    memcpy(copy, text, len + 1u);
    XSetSelectionOwner(dpy, selection, helper, CurrentTime);
    if (XGetSelectionOwner(dpy, selection) != helper) {
        /* Another client kept the selection: the new text is not what a
         * paste will get, and the copy already being served is still
         * valid. */
        free(copy);
        return PLATFORM_CLIP_FAILED;
    }
    free(*slot);
    *slot = copy;
    XFlush(dpy);
    return PLATFORM_CLIP_OK;
}

PlatformClipResult platform_primary_set(const char *text) {
    return linux_selection_own(XA_PRIMARY, text, &g_platform.primary_text);
}

PlatformClipResult platform_clipboard_set(const char *text) {
    /* Own CLIPBOARD; the bytes are served from the SelectionRequest
     * case. */
    PlatformClipResult r = linux_selection_own(g_platform.a_clipboard, text,
                                               &g_platform.clip_text);
    if (r == PLATFORM_CLIP_OK) {
        g_platform.clip_secret = false;
        linux_png_release(g_platform.clip_png);g_platform.clip_png=NULL;
    }
    return r;
}

/* A secret is offered with x-kde-passwordManagerHint = "secret", which the
 * desktop clipboard histories that keep one honour, and is given up
 * `clear_after_sec` later if Liu still owns it (linux_clip_secret_tick). */
PlatformClipResult platform_clipboard_set_secret(const char *text, f64 clear_after_sec) {
    PlatformClipResult r = platform_clipboard_set(text);
    if (r != PLATFORM_CLIP_OK) return r;
    g_platform.clip_secret = true;
    g_platform.clip_secret_until = clear_after_sec > 0.0
                                 ? platform_time_sec() + clear_after_sec : 0.0;
    return r;
}

/* The secret's time is up: stop owning CLIPBOARD, so a paste after this gets
 * nothing rather than the password. A copy made since is left alone: it
 * replaced the secret and reset the flag. */
static void linux_clip_secret_tick(f64 now) {
    if (!g_platform.clip_secret || g_platform.clip_secret_until <= 0.0 ||
        now < g_platform.clip_secret_until)
        return;
    Display *dpy = g_platform.display;
    if (dpy && XGetSelectionOwner(dpy, g_platform.a_clipboard) == g_platform.helper) {
        XSetSelectionOwner(dpy, g_platform.a_clipboard, None, CurrentTime);
        XFlush(dpy);
    }
    free(g_platform.clip_text);
    g_platform.clip_text = NULL;
    g_platform.clip_secret = false;
}

PlatformClipResult platform_clipboard_set_png(const u8 *png,usize length) {
    if(!clipboard_png_header_valid(png,length))
        return length>PLATFORM_CLIPBOARD_PNG_MAX_BYTES?PLATFORM_CLIP_TOO_LARGE:PLATFORM_CLIP_BAD_TEXT;
    Display *dpy=g_platform.display;Window helper=g_platform.helper;
    if(!dpy||!helper)return PLATFORM_CLIP_FAILED;
    LinuxPngBlob *b=malloc(sizeof *b+length);
    if(!b)return PLATFORM_CLIP_FAILED;
    b->refs=1;b->length=length;memcpy(b->bytes,png,length);
    XSetSelectionOwner(dpy,g_platform.a_clipboard,helper,CurrentTime);
    if(XGetSelectionOwner(dpy,g_platform.a_clipboard)!=helper){free(b);return PLATFORM_CLIP_FAILED;}
    free(g_platform.clip_text);g_platform.clip_text=NULL;g_platform.clip_secret=false;
    linux_png_release(g_platform.clip_png);g_platform.clip_png=b;
    XFlush(dpy);return PLATFORM_CLIP_OK;
}

void platform_set_option_as_alt(i32 mode) { (void)mode; }

/* Same policy as the macOS opener (open_url_policy.h): allow-listed schemes
 * only, and a local target that could execute on open is not opened —
 * xdg-open hands an executable to whatever the desktop registered for
 * application/x-executable, which on several of them is "run it". There is
 * no portable "reveal" here, so the containing directory is opened instead. */
static void linux_open_local_path(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return;
    switch (liu_open_path_verdict(path, &st, false)) {
    case LIU_OPEN_PATH_OPEN:
        linux_spawn_opener(path);
        break;
    case LIU_OPEN_PATH_REVEAL: {
        char dir[PATH_MAX];
        int n = snprintf(dir, sizeof dir, "%s", path);
        if (n <= 0 || (usize)n >= sizeof dir) return;
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) *slash = '\0';
        else if (slash == dir) dir[1] = '\0';
        else snprintf(dir, sizeof dir, ".");
        linux_spawn_opener(dir);
        break;
    }
    case LIU_OPEN_PATH_REFUSE:
    default:
        break;
    }
}

/* The desktop's Trash (platform/xdg_trash.h). A file no Trash can take — a
 * read-only mount, no permission — returns false, and the file browser asks
 * before deleting it for good. */
bool platform_trash_supported(void) { return true; }
bool platform_trash_path(const char *path) {
    char data_home[PATH_MAX];
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0] == '/') snprintf(data_home, sizeof data_home, "%s", xdg);
    else if (home && home[0] == '/') snprintf(data_home, sizeof data_home, "%s/.local/share", home);
    else return false;
    /* ~/.local/share may not exist yet on a fresh account. */
    if (!xdg || xdg[0] != '/') {
        char local[PATH_MAX];
        snprintf(local, sizeof local, "%s/.local", home);
        (void)mkdir(local, 0700);
        (void)mkdir(data_home, 0700);
    }
    return xdg_trash_path(path, data_home, getuid(), time(NULL));
}

void platform_fatal_alert(const char *title, const char *detail) {
    const char *what = (title && title[0]) ? title : "cannot continue";
    bool has_detail = detail && detail[0];
    fprintf(stderr, "liu: %s\n", what);
    if (has_detail) fprintf(stderr, "liu: %s\n", detail);

    /* When Liu starts from a desktop menu, stderr may not be visible. If
     * zenity, kdialog, or xmessage is available, show the error in a detached
     * dialog. Pass the text as one argv element rather than through a shell;
     * disable Pango markup for zenity. */
    char text[1200];
    snprintf(text, sizeof text, "%s%s%s", what, has_detail ? "\n\n" : "",
             has_detail ? detail : "");
    char tool[512];
    if (linux_which("zenity", tool, sizeof tool)) {
        char arg[1300];
        snprintf(arg, sizeof arg, "--text=%s", text);
        char *argv[] = { tool, (char *)"--error", (char *)"--no-markup",
                         (char *)"--title=Liu", arg, NULL };
        linux_spawn_detached(argv);
    } else if (linux_which("kdialog", tool, sizeof tool)) {
        char *argv[] = { tool, (char *)"--title", (char *)"Liu",
                         (char *)"--error", text, NULL };
        linux_spawn_detached(argv);
    } else if (linux_which("xmessage", tool, sizeof tool)) {
        char *argv[] = { tool, (char *)"-center", text, NULL };
        linux_spawn_detached(argv);
    }
}

void platform_open_url(const char *url) {
    if (!url || !url[0]) return;
    char scheme[32];
    if (!liu_url_scheme(url, scheme, sizeof scheme)) {
        linux_open_local_path(url);
        return;
    }
    if (!liu_open_url_scheme_allowed(scheme)) {
        fprintf(stderr, "liu: refusing to open URL with scheme '%s'\n", scheme);
        return;
    }
    if (strcmp(scheme, "file") == 0) {
        /* file://host/path or file:///path → the path part, percent-decoded. */
        const char *p = url + 5;
        if (p[0] == '/' && p[1] == '/') { p += 2; while (*p && *p != '/') p++; }
        char path[PATH_MAX];
        usize o = 0;
        for (; *p && o + 1 < sizeof path; p++) {
            if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                char hex[3] = { p[1], p[2], 0 };
                path[o++] = (char)strtol(hex, NULL, 16);
                p += 2;
            } else path[o++] = *p;
        }
        path[o] = '\0';
        linux_open_local_path(path);
        return;
    }
    linux_spawn_opener(url);
}

void platform_open_path(const char *path) {
    if (!path || !path[0]) return;
    linux_spawn_opener(path);
}

void *platform_window_native_view(PlatformWindow *w) {
    (void)w;
    return NULL;
}

void platform_toast_sync(PlatformWindow *w, i32 slot, i32 kind, const char *message,
                         f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt, f32 alpha,
                         f32 bg_r, f32 bg_g, f32 bg_b,
                         f32 fg_r, f32 fg_g, f32 fg_b,
                         f32 bd_r, f32 bd_g, f32 bd_b,
                         f32 accent_r, f32 accent_g, f32 accent_b,
                         f32 shadow_alpha,
                         const char *font_path, f32 font_px) {
    (void)w; (void)slot; (void)kind; (void)message;
    (void)x_pt; (void)y_pt; (void)w_pt; (void)h_pt; (void)alpha;
    (void)bg_r; (void)bg_g; (void)bg_b;
    (void)fg_r; (void)fg_g; (void)fg_b;
    (void)bd_r; (void)bd_g; (void)bd_b;
    (void)accent_r; (void)accent_g; (void)accent_b;
    (void)shadow_alpha;
    (void)font_path; (void)font_px;
}

void platform_toast_hide(PlatformWindow *w) {
    (void)w;
}

void platform_toast_trim(PlatformWindow *w, i32 keep) {
    (void)w; (void)keep;
}

void platform_push_key_down(i32 key, u32 mods, bool repeat) {
    (void)key; (void)mods; (void)repeat;
}

bool platform_is_liu_chrome_cmd_key(i32 key, u32 mods) {
    (void)key; (void)mods;
    return false;
}

i32 platform_map_nsevent_key(void *nsevent) {
    (void)nsevent;
    return 0;
}

u32 platform_map_nsevent_mods(void *nsevent) {
    (void)nsevent;
    return 0;
}

/* No Objective-C runtime here — the main loop's frame pool is a no-op. */
void *platform_autorelease_pool_push(void) { return NULL; }
void  platform_autorelease_pool_pop(void *pool) { (void)pool; }

f64 platform_time_sec(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_platform.time_base.tv_sec) +
           (now.tv_nsec - g_platform.time_base.tv_nsec) / 1e9;
}

f64 platform_time_origin(void) {
    return (f64)g_platform.time_base.tv_sec + (f64)g_platform.time_base.tv_nsec / 1e9;
}

void platform_sleep_ms(u32 ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

bool platform_gl_load(void) {
    /* Modern GL entrypoints are resolved in platform_window_create via
     * gl_loader_init once the GLX core context is current; GL 1.1 links from
     * libGL. Nothing to do here. */
    return true;
}

void *platform_get_gpu_device(PlatformWindow *w) { (void)w; return NULL; }
void *platform_get_gpu_layer(PlatformWindow *w) { (void)w; return NULL; }
void *platform_get_gpu_queue(PlatformWindow *w) { (void)w; return NULL; }

/* X11 cursors are per window: the shape is set on the window the pointer was
 * last seen in, which is the window whose hover asked for it. */
void platform_set_cursor(CursorType type) {
    Display *dpy = g_platform.display;
    PlatformWindow *win = g_pointer_window ? g_pointer_window : g_key_window;
    if (!dpy || !win) return;
    win->app_cursor = (i32)type;
    /* On a resize edge the edge's cursor stands (linux_edge_hover). */
    if (win->edge_dir >= 0) return;
    linux_apply_cursor(win, (i32)type);
}

static void linux_apply_cursor(PlatformWindow *w, i32 type) {
    Display *dpy = g_platform.display;
    if (!dpy || !w) return;
    if (!g_platform.cur_init) {
        g_platform.cur[CURSOR_DEFAULT]  = XCreateFontCursor(dpy, XC_left_ptr);
        g_platform.cur[CURSOR_TEXT]     = XCreateFontCursor(dpy, XC_xterm);
        g_platform.cur[CURSOR_RESIZE_H] = XCreateFontCursor(dpy, XC_sb_h_double_arrow);
        g_platform.cur[CURSOR_RESIZE_V] = XCreateFontCursor(dpy, XC_sb_v_double_arrow);
        g_platform.cur[CURSOR_POINTER]  = XCreateFontCursor(dpy, XC_hand2);
        g_platform.cur_init = true;
    }
    if (type >= 0 && type < 5 && g_platform.cur[type])
        XDefineCursor(dpy, w->xwindow, g_platform.cur[type]);
}
void platform_set_ime_cursor_pos(f32 x, f32 y, f32 cell_w, f32 cell_h) {
    (void)cell_w;
    PlatformWindow *w = g_key_window;
    /* An on-the-spot IM places its candidate list from the same spot where it
     * reads one; one that does not take it for this style refuses the value
     * and nothing else changes. */
    if (!w || !w->xic || !(w->xic_over_the_spot || w->xic_on_the_spot)) return;
    if (!isfinite(x) || !isfinite(y) || !isfinite(cell_h)) return;

    /* The caller passes the cursor cell's top-left; XIM wants the point the
     * preedit sits on, so drop to the cell's bottom edge, which is where an
     * input method hangs its candidate list from. */
    XPoint spot;
    spot.x = (short)lroundf(x);
    spot.y = (short)lroundf(y + cell_h);

    /* This runs once per frame from the main loop. XSetICValues is a round
     * trip to the IM server, so only spend it when the cursor actually moved. */
    if (spot.x == w->xic_spot.x && spot.y == w->xic_spot.y) return;
    w->xic_spot = spot;

    XVaNestedList pre = XVaCreateNestedList(0, XNSpotLocation, &spot, (void *)NULL);
    if (!pre) return;
    char *bad = XSetICValues(w->xic, XNPreeditAttributes, pre, (void *)NULL);
    if (linux_xim_probe_on())
        fprintf(stderr, "[xim] spot %d,%d%s%s\n", spot.x, spot.y,
                bad ? " refused: " : "", bad ? bad : "");
    XFree(pre);
}
/* XmbResetIC's contract: the input method ends the composition and may hand
 * back what it would commit; ibus and fcitx5 send their commit as an ordinary
 * one instead (it arrives after this returns). Both are the ended
 * composition's, so both go out marked ime_reset. */
void platform_ime_reset(PlatformWindow *w) {
    if (!w || !w->xic) return;
    char *left = Xutf8ResetIC(w->xic);
    w->ime_reset_pending = true;
    if (linux_xim_probe_on())
        fprintf(stderr, "[xim] reset: handed back \"%s\"\n", left ? left : "");
    if (left) {
        f64 now = platform_time_sec();
        usize n = strlen(left);
        for (usize i = 0; i < n; ) {
            u32 cp = 0;
            u32 k = utf8_decode((const u8 *)left + i, n - i, &cp);
            if (k == 0) break;
            i += k;
            if (cp < 0x20 || cp == 0x7F) continue;
            push_event((PlatformEvent){
                .type = EVENT_CHAR_INPUT, .window = w, .timestamp = now,
                .char_input = { .codepoint = cp, .mods = 0, .ime_reset = true }
            });
        }
        XFree(left);
    }
    /* The composition is gone whatever the input method says next. */
    if (w->pre_len) {
        w->pre_len = w->pre_caret = 0;
        linux_preedit_push(w);
    }
}

bool platform_get_system_symbol_rgba(const char *name, i32 pixel_size,
                                     u8 r, u8 g, u8 b, u8 a,
                                     const u8 **pixels, i32 *width, i32 *height) {
    (void)name; (void)pixel_size; (void)r; (void)g; (void)b; (void)a;
    (void)pixels; (void)width; (void)height;
    return false;
}
void platform_update_tabs(PlatformWindow *w, const NativeTab *tabs, i32 count, i32 active) {
    (void)w; (void)tabs; (void)count; (void)active;
}
f32 platform_tab_bar_height(PlatformWindow *w) { (void)w; return 28.0f; }

/* =========================================================================
 * Bell / Notifications
 * ========================================================================= */

void platform_play_bell(void) {
    Display *dpy = g_platform.display;
    if (!dpy) return;
    /* XkbBell routes through the X server's bell, which desktops remap onto
     * the system event sound. Deliberately not a spawned player process: BEL
     * can arrive hundreds of times a second from a runaway program, and one
     * fork+exec per byte would take the machine down with it. */
    if (g_key_window)
        XkbBell(dpy, g_key_window->xwindow, 100, None);
    else
        XBell(dpy, 100);
    XFlush(dpy);
}

/* No X11/EWMH equivalent of a Dock badge — a panel shows unread state from
 * the urgency hint (platform_request_attention) instead, and the per-launcher
 * count protocols (Unity's com.canonical.Unity.LauncherEntry) need a D-Bus
 * dependency for something only a couple of desktops render. */
/* ---------------------------------------------------------------------------
 * Taskbar badge
 *
 * The freedesktop world has no dock, and no standard for this. What it has is
 * com.canonical.Unity.LauncherEntry: a session-bus signal carrying a count and
 * a visibility flag, read by Plasma's task manager and by GNOME with
 * Dash-to-Dock. Nothing listens on a bare WM, and that is fine — the signal
 * goes nowhere and costs one message.
 *
 * The first argument is the desktop-file id the count belongs to, so it has to
 * match the entry scripts/install.sh writes (liu.desktop) — that file also
 * carries StartupWMClass=liu so the shell can tie the running window to it.
 *
 * Callers only ever pass "!" or NULL, but a numeric string is the natural
 * meaning of a count, so both are handled.
 * ------------------------------------------------------------------------- */
#ifdef LIU_HAVE_DBUS
static DBusConnection *linux_session_bus(void) {
    static DBusConnection *conn = NULL;
    static bool tried = false;
    if (tried) return conn;
    tried = true;

    DBusError err;
    dbus_error_init(&err);
    conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        conn = NULL;
    }
    /* dbus_bus_get hands back the SHARED connection, and its default is to
     * abort the process when the bus drops. A terminal must outlive its
     * session bus, not follow it out. */
    if (conn) dbus_connection_set_exit_on_disconnect(conn, FALSE);
    return conn;
}

static void dbus_dict_append(DBusMessageIter *dict, const char *key,
                             int type, const char *sig, const void *value) {
    DBusMessageIter entry, var;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    if (dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &var)) {
        dbus_message_iter_append_basic(&var, type, value);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(dict, &entry);
}
#endif

void platform_set_dock_badge(const char *text) {
#ifndef LIU_HAVE_DBUS
    (void)text;
#else
    DBusConnection *conn = linux_session_bus();
    if (!conn) return;

    dbus_int64_t count = 0;
    dbus_bool_t  visible = FALSE;
    if (text && text[0]) {
        char *end = NULL;
        long n = strtol(text, &end, 10);
        /* A non-numeric badge ("!") still means "one thing wants you". */
        count = (end && *end == '\0' && n > 0) ? (dbus_int64_t)n : 1;
        visible = TRUE;
    }

    DBusMessage *msg = dbus_message_new_signal(
        "/liu", "com.canonical.Unity.LauncherEntry", "Update");
    if (!msg) return;

    DBusMessageIter it, dict;
    dbus_message_iter_init_append(msg, &it);
    const char *uri = "application://liu.desktop";
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &uri);
    if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict)) {
        dbus_dict_append(&dict, "count", DBUS_TYPE_INT64, "x", &count);
        dbus_dict_append(&dict, "count-visible", DBUS_TYPE_BOOLEAN, "b", &visible);
        dbus_message_iter_close_container(&it, &dict);
    }

    dbus_connection_send(conn, msg, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(msg);
#endif
}

/* Detached spawn of a fire-and-forget helper. Double-forks so the grandchild
 * is reparented to init and the intermediate child is reaped here: Liu never
 * accumulates zombies, and a helper that outlives the notification (a dialog
 * the user leaves open) cannot block it. argv is passed to execvp directly:
 * nothing is interpolated into a shell string, so a window/tab title
 * containing `$(...)` or `;` is inert. */
static void linux_spawn_detached(char *const argv[]) {
    if (!argv || !argv[0]) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild < 0) _exit(127);
        if (grandchild > 0) _exit(0);
        (void)setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0 ||
            dup2(devnull, STDOUT_FILENO) < 0 ||
            dup2(devnull, STDERR_FILENO) < 0) _exit(127);
        if (devnull > STDERR_FILENO) close(devnull);
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

/* fork+execv a helper with its stdout captured into a FILE the caller reads
 * to EOF and closes with linux_capture_close(). argv goes straight to execv
 * and nothing is parsed by a shell, so a PATH entry with a shell
 * metacharacter in its name, or a directory an attacker put in PATH, is never
 * interpreted as syntax. */
static FILE *linux_capture_open(const char *path, char *const argv[], pid_t *pid_out) {
    if (!path || !argv || !pid_out) return NULL;
    int pfd[2];
    if (pipe(pfd) != 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return NULL; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0 ||
            dup2(pfd[1], STDOUT_FILENO) < 0 ||
            dup2(devnull, STDERR_FILENO) < 0) _exit(127);
        if (devnull > STDERR_FILENO) close(devnull);
        close(pfd[0]); close(pfd[1]);
        execv(path, argv);
        _exit(127);
    }
    close(pfd[1]);
    FILE *f = fdopen(pfd[0], "r");
    if (!f) { close(pfd[0]); while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {} return NULL; }
    *pid_out = pid;
    return f;
}

static void linux_capture_close(FILE *f, pid_t pid) {
    if (f) fclose(f);
    if (pid > 0) while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

void platform_post_notification(const char *title, const char *body) {
    if (!title || !title[0]) return;
    /* notify-send (libnotify) is the lowest common denominator across desktops
     * and is present wherever org.freedesktop.Notifications is; speaking D-Bus
     * directly would mean linking libdbus for one call. --app-name lets the
     * desktop match the .desktop entry for the icon. `--` ends the options: a
     * title a program chose (OSC 777) that starts with '-' is text, not a
     * notify-send flag (liu-notify does the same). */
    char *argv[] = {
        (char *)"notify-send",
        (char *)"--app-name=Liu",
        (char *)"--icon=liu",
        (char *)"--",
        (char *)title,
        (char *)(body ? body : ""),
        NULL
    };
    linux_spawn_detached(argv);
}

/* The mark goes on the window that asked: with two windows open, a bell in the
 * one behind marks that one, not the one the keyboard was last in (usually the
 * one in front, where a window manager drops the mark at once), so the window
 * that rang is the one that flashes and the one its taskbar entry raises. With
 * no window named it is the one the user was last in. */
void platform_request_attention(PlatformWindow *w) {
    Display *dpy = g_platform.display;
    PlatformWindow *win = w ? w : g_key_window;
    if (!dpy || !win) return;

    /* EWMH: the modern signal a panel/taskbar highlights on. */
    x11_wm_state(win->xwindow, LIU_NET_WM_STATE_ADD,
                 g_platform.a_net_wm_state_demands_attention, None);

    /* ICCCM urgency hint, for WMs that predate (or ignore) the EWMH atom.
     * Read-modify-write so the input/state hints, which XSetWMHints would
     * otherwise reset, are kept. */
    XWMHints *hints = XGetWMHints(dpy, win->xwindow);
    if (!hints) hints = XAllocWMHints();
    if (hints) {
        hints->flags |= XUrgencyHint;
        XSetWMHints(dpy, win->xwindow, hints);
        XFree(hints);
    }
    XFlush(dpy);
}

/* Is any of Liu's windows the active one? */
bool platform_is_app_focused(void) {
    Display *dpy = g_platform.display;
    if (!dpy || g_window_count == 0) return false;

    /* _NET_ACTIVE_WINDOW is the WM's own answer and stays correct even while a
     * menu or override-redirect popup holds the X input focus. */
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (x11_get_prop(RootWindow(dpy, g_platform.screen),
                     g_platform.a_net_active_window, XA_WINDOW, 32, 1,
                     &data, &n)) {
        Window active = ((const Window *)(const void *)data)[0];
        XFree(data);
        return linux_window_of(active) != NULL;
    }

    /* No conforming WM: fall back to the input focus, walking up from a child
     * window (XIM/menu) to the Liu top-level. */
    Window focus = None;
    int revert = 0;
    XGetInputFocus(dpy, &focus, &revert);
    if (focus == None || focus == PointerRoot) return false;
    Window root = None, parent = None, *children = NULL;
    unsigned int nchildren = 0;
    for (int depth = 0; depth < 8 && focus != None; depth++) {
        if (linux_window_of(focus)) return true;
        if (!XQueryTree(dpy, focus, &root, &parent, &children, &nchildren))
            break;
        if (children) XFree(children);
        if (parent == root || parent == None) break;
        focus = parent;
    }
    return false;
}

bool platform_utf8_normalize_nfc(const char *src, char *dst, usize dstcap) {
    /* Linux filesystems hand back whatever bytes are stored; ext4/btrfs/xfs
     * don't normalize and the convention is NFC. No-op passthrough copy. */
    if (!dst || dstcap == 0) return false;
    if (!src) { dst[0] = '\0'; return false; }
    usize n = strlen(src);
    if (n >= dstcap) n = dstcap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

/* =========================================================================
 * File watcher (config hot-reload)
 *
 * Watches the file's PARENT DIRECTORY, not the file. Most editors save by
 * writing a temporary file and rename()ing it into place, which swaps the
 * inode: a watch held on the original inode survives the save but points at
 * an unlinked file that will never change again, so an inode watch silently
 * reports "no changes" forever after the first save. The directory watch sees
 * the rename, and events are filtered on the basename.
 * ========================================================================= */

void platform_unwatch_file(void) {
    if (g_platform.inotify_fd >= 0) {
        if (g_platform.inotify_wd >= 0)
            inotify_rm_watch(g_platform.inotify_fd, g_platform.inotify_wd);
        close(g_platform.inotify_fd);
    }
    g_platform.inotify_fd = -1;
    g_platform.inotify_wd = -1;
    g_platform.watch_cb = NULL;
    g_platform.watch_user = NULL;
    g_platform.watch_path[0] = '\0';
    g_platform.watch_base[0] = '\0';
}

bool platform_watch_file(const char *path, FileWatchCallback cb, void *userdata) {
    if (!path || !path[0] || !cb) return false;

    usize plen = strlen(path);
    if (plen >= sizeof g_platform.watch_path) return false;

    const char *slash = strrchr(path, '/');
    if (!slash || slash == path + plen - 1) return false;   /* need dir + name */
    usize dlen = (usize)(slash - path);
    const char *base = slash + 1;
    if (strlen(base) >= sizeof g_platform.watch_base) return false;

    char dir[PATH_MAX];
    if (dlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
    else {
        if (dlen >= sizeof dir) return false;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
    }

    platform_unwatch_file();   /* single-slot watcher, as documented */

    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return false;

    /* IN_CLOSE_WRITE covers in-place writes, IN_MOVED_TO the rename-replace
     * save, IN_CREATE a file recreated from scratch. */
    int wd = inotify_add_watch(fd, dir,
                               IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd < 0) { close(fd); return false; }

    g_platform.inotify_fd = fd;
    g_platform.inotify_wd = wd;
    g_platform.watch_cb = cb;
    g_platform.watch_user = userdata;
    memcpy(g_platform.watch_path, path, plen + 1);
    snprintf(g_platform.watch_base, sizeof g_platform.watch_base, "%s", base);
    linux_wake_waiter();   /* re-poll so the new fd joins the wait set */
    return true;
}

/* Custom-chrome window drag. Liu paints its own tab strip over the title-bar
 * area, so dragging it must hand the move back to the window manager: moving
 * the window with XMoveWindow here would fight the WM's snapping,
 * multi-monitor handling and edge tiling. _NET_WM_MOVERESIZE is the EWMH
 * request for exactly that: "take over this drag as if the user had grabbed my
 * title bar".
 *
 * The pointer must be ungrabbed first; the WM cannot start its own grab while
 * Liu's, from the ButtonPress that started the drag, is outstanding. */
/* The move is handed to the window manager only once the pointer has
 * travelled a few pixels with the button down (linux_move_pending_motion):
 * handed over at the press, the WM would take the pointer, and the second
 * click of a double click (maximize) would go to it instead of to Liu. */
void platform_begin_window_drag(PlatformWindow *w) {
    Display *dpy = g_platform.display;
    if (!w || !dpy) return;
    Window root = None, child = None;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(dpy, w->xwindow, &root, &child, &rx, &ry, &wx, &wy, &mask))
        return;
    w->move_pending = true;
    w->move_x = rx;
    w->move_y = ry;
}

/* Pointer motion while a strip press is pending: start the move past 4 px,
 * forget it once the button is up. */
static void linux_move_pending_motion(PlatformWindow *w) {
    if (!w || !w->move_pending) return;
    Display *dpy = g_platform.display;
    Window root = None, child = None;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(dpy, w->xwindow, &root, &child, &rx, &ry, &wx, &wy, &mask) ||
        !(mask & (Button1Mask | Button2Mask | Button3Mask))) {
        w->move_pending = false;
        return;
    }
    if (abs(rx - w->move_x) + abs(ry - w->move_y) < 4) return;
    w->move_pending = false;
    linux_begin_moveresize(w, LIU_NET_WM_MOVERESIZE_MOVE);
}

/* A move (LIU_NET_WM_MOVERESIZE_MOVE) or a resize from one edge (0..7, the
 * direction table linux_resize_edge() answers in), handed to the WM. */
static void linux_begin_moveresize(PlatformWindow *w, long direction) {
    Display *dpy = g_platform.display;
    if (!w || !dpy) return;

    Window root = RootWindow(dpy, g_platform.screen);
    Window child = None;
    int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(dpy, w->xwindow, &root, &child,
                       &root_x, &root_y, &win_x, &win_y, &mask))
        return;

    /* Which button is held — the WM ends the move when THAT button is
     * released, so guessing button 1 would leave a drag stuck to the cursor
     * when the gesture started on another button. */
    long button = 1;
    if (mask & Button2Mask) button = 2;
    else if (mask & Button3Mask) button = 3;

    XUngrabPointer(dpy, CurrentTime);
    XFlush(dpy);

    x11_send_root_msg(w->xwindow, g_platform.a_net_wm_moveresize,
                      root_x, root_y, direction, button, 1);
}

/* Liu draws minimize / maximize / close at the right end of its toolbar
 * (the window has no WM title bar, x11_set_borderless) and drives them here. */
bool platform_draws_own_window_controls(void) { return true; }

void platform_window_minimize(PlatformWindow *w) {
    Display *dpy = g_platform.display;
    if (!w || !dpy) return;
    XIconifyWindow(dpy, w->xwindow, g_platform.screen);
    XFlush(dpy);
}

bool platform_window_is_maximized(PlatformWindow *w) { return w && w->maximized; }

void platform_window_toggle_maximize(PlatformWindow *w) {
    if (!w || !g_platform.display) return;
    x11_wm_state(w->xwindow, w->maximized ? LIU_NET_WM_STATE_REMOVE : LIU_NET_WM_STATE_ADD,
                 g_platform.a_net_wm_state_max_vert, g_platform.a_net_wm_state_max_horz);
}

/* The close button is the window manager's close: the same EVENT_CLOSE, so
 * the same unsaved-document guard and the same teardown. */
void platform_window_request_close(PlatformWindow *w) {
    if (!w) return;
    push_event((PlatformEvent){ .type = EVENT_CLOSE, .window = w });
}

/* Native settings window is a macOS-only affordance; Linux uses the portable
 * in-app settings overlay, so these are no-ops. */
void platform_show_settings(PlatformWindow *w,
                            const char **font_names, const char **font_paths,
                            const bool *font_installed, i32 font_count,
                            const char **theme_names, i32 theme_count,
                            const char *current_font, const char *current_theme,
                            f32 font_size, f32 font_weight,
                            f32 opacity, f32 tab_sleep_minutes,
                            SettingsFontCallback on_font,
                            SettingsThemeCallback on_theme,
                            SettingsValueCallback on_value) {
    (void)w; (void)font_names; (void)font_paths; (void)font_installed;
    (void)font_count; (void)theme_names; (void)theme_count;
    (void)current_font; (void)current_theme; (void)font_size; (void)font_weight;
    (void)opacity; (void)tab_sleep_minutes; (void)on_font; (void)on_theme;
    (void)on_value;
}
void platform_close_settings(void) {}
bool platform_settings_visible(void) { return false; }

void platform_poll_file_watches(void) {
    if (g_platform.inotify_fd < 0 || !g_platform.watch_cb) return;

    /* inotify_event is variable-length (a flexible name[] tail); the buffer is
     * aligned to the struct so the walk below can cast each record in place. */
    union {
        struct inotify_event ev;
        char buf[4096];
    } u;

    bool hit = false;
    for (;;) {
        ssize_t n = read(g_platform.inotify_fd, u.buf, sizeof u.buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;   /* EAGAIN: drained */
        }
        for (char *p = u.buf; p < u.buf + n; ) {
            const struct inotify_event *e = (const struct inotify_event *)(void *)p;
            if (e->len > 0 && strcmp(e->name, g_platform.watch_base) == 0)
                hit = true;
            p += sizeof(struct inotify_event) + e->len;
        }
    }

    /* Coalesce: a single save can produce CREATE + MOVED_TO + CLOSE_WRITE, and
     * re-parsing the config three times would be visible as a flicker. */
    if (hit) g_platform.watch_cb(g_platform.watch_path, g_platform.watch_user);
}

/* =========================================================================
 * File dialogs
 *
 * X11 has no native file picker; the desktop-agnostic answer is to run the
 * portable helper the user's desktop already ships (kdialog on KDE, zenity on
 * GNOME and everything derived from it) and read the chosen path off its
 * stdout. Blocking the main loop for the duration matches the macOS backend,
 * where NSOpenPanel runs a modal session.
 *
 * Every argument is passed as a separate execvp argv element — no path or
 * title is ever interpolated into a shell command line.
 * ========================================================================= */

/* Locate `name` on PATH. Returns false when it isn't installed/executable. */
static bool linux_which(const char *name, char *out, usize cap) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    for (const char *p = path; *p; ) {
        const char *sep = strchr(p, ':');
        usize seg = sep ? (usize)(sep - p) : strlen(p);
        if (seg > 0 && seg < cap) {
            int n = snprintf(out, cap, "%.*s/%s", (int)seg, p, name);
            if (n > 0 && (usize)n < cap && access(out, X_OK) == 0) return true;
        }
        if (!sep) break;
        p = sep + 1;
    }
    out[0] = '\0';
    return false;
}

/* Run `argv` and capture the first line of its stdout into `out`. Returns
 * false on spawn failure, non-zero exit (the user cancelled) or empty output. */
static bool linux_capture_line(char *const argv[], char *out, usize cap) {
    if (!argv || !argv[0] || !out || cap == 0) return false;
    out[0] = '\0';

    int fds[2];
    if (pipe(fds) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        if (fds[1] > STDERR_FILENO) close(fds[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);

    usize len = 0;
    for (;;) {
        ssize_t n = read(fds[0], out + len, cap - 1 - len);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        len += (usize)n;
        if (len >= cap - 1) break;
    }
    out[len] = '\0';
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    /* Trim at the first newline: multi-select would hand back several paths
     * and the callers all take exactly one. */
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    while (*out && (out[strlen(out) - 1] == '\r' || out[strlen(out) - 1] == ' '))
        out[strlen(out) - 1] = '\0';

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { out[0] = '\0'; return false; }
    return out[0] != '\0';
}

/* Turn "ttf,otf,ttc" into a glob list ("*.ttf *.otf *.ttc"), the shape both
 * kdialog and zenity accept. Empty/NULL extensions means "any file". */
static void linux_ext_globs(const char *extensions, char *out, usize cap) {
    out[0] = '\0';
    if (!extensions || !extensions[0]) {
        snprintf(out, cap, "*");
        return;
    }
    usize off = 0;
    for (const char *p = extensions; *p; ) {
        while (*p == ',' || *p == ' ' || *p == '.') p++;
        const char *comma = strchr(p, ',');
        usize seg = comma ? (usize)(comma - p) : strlen(p);
        while (seg > 0 && (p[seg - 1] == ' ' || p[seg - 1] == '.')) seg--;
        if (seg > 0) {
            int n = snprintf(out + off, cap - off, "%s*.%.*s",
                             off ? " " : "", (int)seg, p);
            if (n < 0 || (usize)n >= cap - off) break;
            off += (usize)n;
        }
        if (!comma) break;
        p = comma + 1;
    }
    if (!out[0]) snprintf(out, cap, "*");
}

#ifdef LIU_HAVE_DBUS
/* ---------------------------------------------------------------------------
 * XDG Desktop Portal file chooser
 *
 * The subprocess pickers below need a helper to be installed, and which helper
 * that is depends on the desktop. A minimal Hyprland, sway or i3 install
 * commonly has neither zenity nor kdialog, and there no file dialog would open.
 *
 * org.freedesktop.portal.FileChooser is the freedesktop answer: one D-Bus
 * interface, implemented by a backend that each desktop supplies for itself
 * (xdg-desktop-portal-gtk, -kde, -hyprland, -wlr), so the dialog looks native
 * everywhere without Liu knowing which desktop it is on. GTK and Qt both route
 * their own dialogs through it. Tried first; the helpers stay as the fallback
 * for a machine with no portal at all.
 *
 * The call is two-part: OpenFile returns a Request object path immediately,
 * and the user's answer arrives later as a Response signal on it. The path is
 * derived from Liu's own bus name, so it can be (and is) subscribed to before
 * the call goes out; a backend that answers instantly would otherwise beat the
 * match rule into place.
 * ------------------------------------------------------------------------- */

static int portal_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* file:///home/a%20b.md → /home/a b.md. False for anything not a local file:
 * the portal can hand back a URI for a remote mount, which nothing downstream
 * of here knows how to open. */
static bool portal_uri_to_path(const char *uri, char *out, usize cap) {
    if (!uri || strncmp(uri, "file://", 7) != 0) return false;
    const char *s = uri + 7;
    usize o = 0;
    for (; *s && o + 1 < cap; s++) {
        int hi, lo;
        if (*s == '%' && (hi = portal_hexval(s[1])) >= 0 &&
                         (lo = portal_hexval(s[2])) >= 0) {
            out[o++] = (char)((hi << 4) | lo);
            s += 2;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
    /* A "file://host/path" authority would land here as a relative-looking
     * path; only an absolute one is ours to open. */
    return out[0] == '/';
}

/* Append the "filters" option: a(sa(us)), an array of (label, patterns) where
 * each pattern is (0 = glob, text). `globs` is the space-separated list
 * linux_ext_globs built. */
static void portal_append_filters(DBusMessageIter *dict, const char *globs) {
    DBusMessageIter entry, var, arr, filt, pats;
    const char *key = "filters";
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    if (dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a(sa(us))", &var)) {
        if (dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(sa(us))", &arr)) {
            /* Supported files */
            if (dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &filt)) {
                const char *label = "Supported files";
                dbus_message_iter_append_basic(&filt, DBUS_TYPE_STRING, &label);
                if (dbus_message_iter_open_container(&filt, DBUS_TYPE_ARRAY, "(us)", &pats)) {
                    for (const char *p = globs; *p; ) {
                        while (*p == ' ') p++;
                        if (!*p) break;
                        const char *sp = strchr(p, ' ');
                        usize seg = sp ? (usize)(sp - p) : strlen(p);
                        char one[128];
                        if (seg > 0 && seg < sizeof one) {
                            snprintf(one, sizeof one, "%.*s", (int)seg, p);
                            DBusMessageIter pat;
                            if (dbus_message_iter_open_container(&pats, DBUS_TYPE_STRUCT,
                                                                 NULL, &pat)) {
                                dbus_uint32_t kind = 0;   /* 0 = glob, 1 = mime type */
                                const char *val = one;
                                dbus_message_iter_append_basic(&pat, DBUS_TYPE_UINT32, &kind);
                                dbus_message_iter_append_basic(&pat, DBUS_TYPE_STRING, &val);
                                dbus_message_iter_close_container(&pats, &pat);
                            }
                        }
                        if (!sp) break;
                        p = sp + 1;
                    }
                    dbus_message_iter_close_container(&filt, &pats);
                }
                dbus_message_iter_close_container(&arr, &filt);
            }
            /* All files, so the filter never traps the user. */
            if (dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &filt)) {
                const char *label = "All files";
                dbus_message_iter_append_basic(&filt, DBUS_TYPE_STRING, &label);
                if (dbus_message_iter_open_container(&filt, DBUS_TYPE_ARRAY, "(us)", &pats)) {
                    DBusMessageIter pat;
                    if (dbus_message_iter_open_container(&pats, DBUS_TYPE_STRUCT, NULL, &pat)) {
                        dbus_uint32_t kind = 0;
                        const char *val = "*";
                        dbus_message_iter_append_basic(&pat, DBUS_TYPE_UINT32, &kind);
                        dbus_message_iter_append_basic(&pat, DBUS_TYPE_STRING, &val);
                        dbus_message_iter_close_container(&pats, &pat);
                    }
                    dbus_message_iter_close_container(&filt, &pats);
                }
                dbus_message_iter_close_container(&arr, &filt);
            }
            dbus_message_iter_close_container(&var, &arr);
        }
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(dict, &entry);
}

/* Pull the first local path out of a Response signal's (u, a{sv}) body.
 * Returns 0 = picked, 1 = cancelled, -1 = malformed. */
static int portal_parse_response(DBusMessage *sig, char *out, usize cap) {
    DBusMessageIter it;
    if (!dbus_message_iter_init(sig, &it)) return -1;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return -1;
    dbus_uint32_t response = 0;
    dbus_message_iter_get_basic(&it, &response);
    if (response != 0) return 1;          /* 1 = user cancelled, 2 = ended other ways */

    if (!dbus_message_iter_next(&it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return -1;

    DBusMessageIter results;
    dbus_message_iter_recurse(&it, &results);
    while (dbus_message_iter_get_arg_type(&results) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&results, &entry);
        const char *key = NULL;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&entry, &key);
        if (key && strcmp(key, "uris") == 0 && dbus_message_iter_next(&entry)) {
            DBusMessageIter var;
            dbus_message_iter_recurse(&entry, &var);
            if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
                DBusMessageIter uris;
                dbus_message_iter_recurse(&var, &uris);
                /* "multiple" was never requested, so the first entry is the
                 * only one, and the callers take exactly one path. */
                if (dbus_message_iter_get_arg_type(&uris) == DBUS_TYPE_STRING) {
                    const char *uri = NULL;
                    dbus_message_iter_get_basic(&uris, &uri);
                    if (portal_uri_to_path(uri, out, cap)) return 0;
                }
            }
            return 1;   /* answered, but nothing that can be opened */
        }
        dbus_message_iter_next(&results);
    }
    return 1;
}

/* Returns true when a path was picked. *reached tells the caller whether the
 * portal answered at all: on false/false the subprocess helpers still get
 * their turn, on false/true the user cancelled and nothing more should open. */
static bool portal_file_chooser(const char *title, const char *globs,
                                bool directory, char *out, usize cap,
                                bool *reached) {
    *reached = false;
    if (out && cap) out[0] = '\0';

    DBusConnection *conn = linux_session_bus();
    if (!conn) return false;
    const char *unique = dbus_bus_get_unique_name(conn);
    if (!unique) return false;

    static unsigned seq = 0;
    char token[64];
    snprintf(token, sizeof token, "liu%u", ++seq);

    /* The Request path the portal will use: the connection's unique name
     * minus the leading colon, dots turned into underscores, plus the
     * request's token. */
    char sender[256];
    snprintf(sender, sizeof sender, "%s", unique[0] == ':' ? unique + 1 : unique);
    for (char *p = sender; *p; p++) if (*p == '.') *p = '_';
    char req_path[512];
    snprintf(req_path, sizeof req_path,
             "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);

    char match[700];
    snprintf(match, sizeof match,
             "type='signal',interface='org.freedesktop.portal.Request',"
             "member='Response',path='%s'", req_path);
    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(conn, match, &err);
    if (dbus_error_is_set(&err)) { dbus_error_free(&err); return false; }
    dbus_connection_flush(conn);

    bool picked = false;
    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.FileChooser", "OpenFile");
    if (!msg) goto done;

    {
        /* Lets the backend parent the dialog on the terminal window instead of
         * dropping it wherever the compositor pleases. */
        char parent[64] = "";
        if (g_key_window)
            snprintf(parent, sizeof parent, "x11:%lx",
                     (unsigned long)g_key_window->xwindow);
        const char *parent_s = parent;
        const char *title_s = title;

        DBusMessageIter it, dict;
        dbus_message_iter_init_append(msg, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent_s);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &title_s);
        if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict)) {
            const char *tok = token;
            dbus_dict_append(&dict, "handle_token", DBUS_TYPE_STRING, "s", &tok);
            dbus_bool_t dir_b = directory ? TRUE : FALSE;
            /* "directory" is a FileChooser v3 option; older backends ignore it
             * and hand back a file, which the caller then rejects — still
             * better than not opening a dialog at all. */
            if (directory)
                dbus_dict_append(&dict, "directory", DBUS_TYPE_BOOLEAN, "b", &dir_b);
            else if (globs && strcmp(globs, "*") != 0)
                portal_append_filters(&dict, globs);
            dbus_message_iter_close_container(&it, &dict);
        }
    }

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        /* No portal on this machine — the helpers below are the fallback. */
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        goto done;
    }
    dbus_message_unref(reply);
    *reached = true;

    /* The dialog is the user's now. Blocking here matches the subprocess
     * pickers (which waitpid) and NSOpenPanel's modal session on macOS. The
     * cap only exists so a portal that dies mid-dialog cannot wedge the
     * terminal for good.
     *
     * Popping messages off the shared connection is safe because nothing else
     * in Liu reads it — the taskbar badge only ever sends. */
    {
        f64 deadline = platform_time_sec() + 600.0;
        while (!picked && platform_time_sec() < deadline) {
            if (!dbus_connection_read_write_dispatch(conn, 200)) break;  /* bus dropped */
            DBusMessage *sig;
            bool answered = false;
            while ((sig = dbus_connection_pop_message(conn)) != NULL) {
                if (dbus_message_is_signal(sig, "org.freedesktop.portal.Request",
                                           "Response") &&
                    dbus_message_has_path(sig, req_path)) {
                    int r = portal_parse_response(sig, out, cap);
                    picked = (r == 0);
                    answered = true;
                }
                dbus_message_unref(sig);
            }
            if (answered) break;
        }
    }

done:
    dbus_bus_remove_match(conn, match, NULL);
    dbus_connection_flush(conn);
    return picked;
}
#endif /* LIU_HAVE_DBUS */

static const char *linux_file_dialog(const char *title, const char *extensions,
                                     bool directory) {
    static char result[PATH_MAX];
    result[0] = '\0';

    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/";
    if (!title || !title[0]) title = directory ? "Choose a folder" : "Open";

    char globs[512];
    linux_ext_globs(extensions, globs, sizeof globs);

#ifdef LIU_HAVE_DBUS
    /* The portable path first — every desktop that ships a portal backend gets
     * its own native dialog out of this, with no helper installed. */
    {
        bool reached = false;
        if (portal_file_chooser(title, globs, directory, result, sizeof result,
                                &reached))
            return result;
        if (reached) return NULL;   /* the portal ran and the user cancelled */
    }
#endif

    /* Prefer the picker that matches the running desktop: a Qt dialog on KDE
     * and a GTK one elsewhere, so the file browser looks and behaves like
     * every other dialog the user sees. */
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    bool kde_first = desktop && (strstr(desktop, "KDE") || strstr(desktop, "LXQt"));

    static const char *zenity_like[] = { "zenity", "qarma", "matedialog" };
    char tool[PATH_MAX];

    for (int pass = 0; pass < 2; pass++) {
        bool try_kde = (pass == 0) == kde_first;

        if (try_kde) {
            if (!linux_which("kdialog", tool, sizeof tool)) continue;
            char title_arg[512];
            snprintf(title_arg, sizeof title_arg, "%s", title);
            if (directory) {
                char *argv[] = { tool, (char *)"--title", title_arg,
                                 (char *)"--getexistingdirectory",
                                 (char *)home, NULL };
                if (linux_capture_line(argv, result, sizeof result)) return result;
            } else {
                /* kdialog's filter is "<globs>|<label>". */
                char filter[600];
                snprintf(filter, sizeof filter, "%s|Supported files", globs);
                char *argv[] = { tool, (char *)"--title", title_arg,
                                 (char *)"--getopenfilename",
                                 (char *)home, filter, NULL };
                if (linux_capture_line(argv, result, sizeof result)) return result;
            }
            return NULL;   /* the tool ran and the user cancelled */
        }

        for (usize i = 0; i < sizeof(zenity_like) / sizeof(zenity_like[0]); i++) {
            if (!linux_which(zenity_like[i], tool, sizeof tool)) continue;
            char title_arg[560];
            snprintf(title_arg, sizeof title_arg, "--title=%s", title);
            if (directory) {
                char *argv[] = { tool, (char *)"--file-selection",
                                 (char *)"--directory", title_arg, NULL };
                if (linux_capture_line(argv, result, sizeof result)) return result;
            } else {
                /* zenity's filter is "--file-filter=<label>|<globs>". */
                char filter[640];
                snprintf(filter, sizeof filter,
                         "--file-filter=Supported files|%s", globs);
                char *argv[] = { tool, (char *)"--file-selection", title_arg,
                                 filter, (char *)"--file-filter=All files|*",
                                 NULL };
                if (linux_capture_line(argv, result, sizeof result)) return result;
            }
            return NULL;   /* the tool ran and the user cancelled */
        }
    }

    fprintf(stderr, "liu: no file chooser available — install an "
                    "xdg-desktop-portal backend for your desktop "
                    "(xdg-desktop-portal-gtk, -kde, -hyprland, -wlr), "
                    "or zenity/kdialog as a fallback\n");
    return NULL;
}

const char *platform_open_file_dialog(const char *title, const char *extensions) {
    return linux_file_dialog(title, extensions, false);
}

const char *platform_open_folder_dialog(const char *title) {
    return linux_file_dialog(title, NULL, true);
}

/* =========================================================================
 * Audio recording (custom notification sounds)
 *
 * No AVAudioRecorder here, and no library linked for one feature: the desktop
 * audio stacks all ship a capture CLI that writes a finished WAV, so recording
 * is a supervised child process. WAV rather than AAC because the playback side
 * of this same feature is paplay/aplay (see notify/platform_notify_linux.c),
 * which cannot open an .m4a.
 *
 * Linux has no per-application microphone permission outside a sandbox, so the
 * permission query answers the question the UI is really asking: can Liu
 * record at all, i.e. is a capture tool installed.
 * ========================================================================= */

/* Preference order: pw-record, then arecord, then parec. parec discards
 * whatever is still in its buffer when it exits (on either SIGINT or
 * SIGTERM), so it truncates the end of every recording and is only used when
 * nothing else is installed. */
static bool linux_find_recorder(char *tool, usize cap, const char **argv_style) {
    if (linux_which("pw-record", tool, cap)){ *argv_style = "pw-record";return true; }
    if (linux_which("arecord", tool, cap))  { *argv_style = "arecord";  return true; }
    if (linux_which("parec", tool, cap))    { *argv_style = "parec";    return true; }
    return false;
}

static struct {
    pid_t pid;          /* 0 = not recording */
    f64   started;
    char  path[PATH_MAX];
} g_rec = { 0, 0.0, {0} };

bool platform_audio_record_start(const char *out_path) {
    if (!out_path || !out_path[0] || g_rec.pid > 0) return false;

    char tool[PATH_MAX];
    const char *style = NULL;
    if (!linux_find_recorder(tool, sizeof tool, &style)) return false;

    /* Mono 44.1 kHz: a notification chime, not a studio take — and the smaller
     * file is what gets copied into the config directory. */
    char *argv_parec[] = { tool, (char *)"--file-format=wav",
                           (char *)"--channels=1", (char *)"--rate=44100",
                           (char *)out_path, NULL };
    char *argv_pw[]    = { tool, (char *)"--channels=1", (char *)"--rate=44100",
                           (char *)out_path, NULL };
    char *argv_arec[]  = { tool, (char *)"-q", (char *)"-t", (char *)"wav",
                           (char *)"-f", (char *)"S16_LE", (char *)"-c", (char *)"1",
                           (char *)"-r", (char *)"44100", (char *)out_path, NULL };
    char *const *argv = (strcmp(style, "parec") == 0)     ? argv_parec
                      : (strcmp(style, "pw-record") == 0) ? argv_pw
                                                          : argv_arec;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        (void)setsid();   /* detach from the controlling terminal's job control */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execv(tool, argv);
        _exit(127);
    }

    g_rec.pid = pid;
    g_rec.started = platform_time_sec();
    snprintf(g_rec.path, sizeof g_rec.path, "%s", out_path);
    return true;
}

bool platform_audio_record_stop(void) {
    if (g_rec.pid <= 0) return false;

    /* SIGINT first: parec, pw-record and arecord all treat it as "stop", and
     * flush the WAV header's final length on the way out. A plain SIGKILL
     * would leave the header claiming zero frames and the file unplayable.
     * Escalate only if the tool ignores it. */
    kill(g_rec.pid, SIGINT);
    bool reaped = false;
    for (int i = 0; i < 150 && !reaped; i++) {          /* up to ~1.5 s */
        int status = 0;
        pid_t r = waitpid(g_rec.pid, &status, WNOHANG);
        if (r == g_rec.pid) { reaped = true; break; }
        if (r < 0 && errno != EINTR) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (!reaped) {
        kill(g_rec.pid, SIGTERM);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (waitpid(g_rec.pid, NULL, WNOHANG) != g_rec.pid) {
            kill(g_rec.pid, SIGKILL);
            while (waitpid(g_rec.pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }

    g_rec.pid = 0;
    g_rec.started = 0.0;
    return true;
}

bool platform_audio_recording(void) {
    if (g_rec.pid <= 0) return false;
    /* The tool can die on its own (device grabbed, disk full). Reap it so the
     * UI stops showing a recording that is not happening. */
    if (waitpid(g_rec.pid, NULL, WNOHANG) == g_rec.pid) {
        g_rec.pid = 0;
        g_rec.started = 0.0;
        return false;
    }
    return true;
}

f64 platform_audio_record_elapsed(void) {
    if (g_rec.pid <= 0) return 0.0;
    return platform_time_sec() - g_rec.started;
}

PlatformMicPermission platform_audio_mic_permission(void) {
    /* There is no per-app microphone gate on a normal X11 desktop, so the only
     * thing that can prevent recording is having no capture tool installed.
     * Reporting DENIED in that case is what lets the UI say something
     * actionable instead of failing silently at record time. */
    char tool[PATH_MAX];
    const char *style = NULL;
    return linux_find_recorder(tool, sizeof tool, &style) ? PLATFORM_MIC_GRANTED
                                                          : PLATFORM_MIC_DENIED;
}

void platform_open_microphone_settings(void) {
    /* Closest equivalent to the macOS privacy pane: the mixer, where the input
     * device and its level live. Best-effort — nothing to open if it is absent. */
    char tool[PATH_MAX];
    if (linux_which("pavucontrol", tool, sizeof tool)) {
        char *argv[] = { tool, NULL };
        linux_spawn_detached(argv);
    }
}

/* =========================================================================
 * Live PCM capture (voice input)
 *
 * The recorder above spawns a tool that writes a finished WAV; voice input
 * needs the samples WHILE they are being spoken, so this is a second, separate
 * path: the same child-process approach, but with the tool writing raw f32
 * mono 16 kHz to a pipe that a reader thread drains into a ring buffer.
 *
 * 16 kHz because that is what Whisper's mel front-end resamples to anyway —
 * asking the capture stack for it directly saves a resample and three quarters
 * of the bytes. Raw f32 rather than WAV because there is no header to wait for
 * on a stream that never ends.
 *
 * A thread, not a poll from the main loop: the pipe fills at 64 KB/s and the
 * main loop can stall for a whole frame on a slow render, which would show up
 * as a dropout in the middle of a word.
 * ========================================================================= */

/* Two seconds at 16 kHz. The consumer drains this every frame; the size is
 * headroom for a stalled frame, not a buffer to accumulate in. */
#define LIU_CAP_RING_FRAMES (16000 * 2)

static struct {
    pid_t   pid;                    /* 0 = not capturing */
    int     fd;                     /* read end of the child's stdout */
    pthread_t thread;
    bool    thread_live;
    _Atomic bool  stop;
    _Atomic bool  overflowed;       /* ring wrapped: the consumer fell behind */
    _Atomic u32   level_q16;        /* peak since last read, 16.16 fixed point */
    f32     ring[LIU_CAP_RING_FRAMES];
    _Atomic u64   wpos;             /* monotonic write cursor, in frames */
    u64     rpos;                   /* read cursor; main thread only */
    char    device[256];            /* pinned input device, "" = default */
} g_cap = {0};

/* Peak is published as fixed point through an atomic because the UI meter
 * reads it from the main thread while the reader thread writes it; a float
 * would be a torn read on nothing that guarantees otherwise. */
static void cap_publish_level(const f32 *s, i32 n) {
    f32 peak = 0.0f;
    for (i32 i = 0; i < n; i++) {
        f32 a = fabsf(s[i]);
        if (a > peak) peak = a;
    }
    if (peak > 1.0f) peak = 1.0f;
    u32 q = (u32)(peak * 65536.0f);
    u32 prev = atomic_load_explicit(&g_cap.level_q16, memory_order_relaxed);
    if (q > prev) atomic_store_explicit(&g_cap.level_q16, q, memory_order_relaxed);
}

static void *cap_reader(void *arg) {
    (void)arg;
    f32 buf[2048];
    while (!atomic_load_explicit(&g_cap.stop, memory_order_relaxed)) {
        ssize_t got = read(g_cap.fd, buf, sizeof buf);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;                    /* tool exited */
        i32 frames = (i32)(got / (ssize_t)sizeof(f32));
        if (frames <= 0) continue;

        cap_publish_level(buf, frames);

        u64 w = atomic_load_explicit(&g_cap.wpos, memory_order_relaxed);
        for (i32 i = 0; i < frames; i++)
            g_cap.ring[(w + (u64)i) % LIU_CAP_RING_FRAMES] = buf[i];
        atomic_store_explicit(&g_cap.wpos, w + (u64)frames, memory_order_release);
    }
    return NULL;
}

/* Same preference order as the recorder, and for the same reason, except that
 * parec's truncate-on-exit flaw does not apply to a stream nobody closes
 * gracefully, so it stays a usable last resort here too. */
static bool cap_find_tool(char *tool, usize cap, const char **style) {
    if (linux_which("pw-record", tool, cap)) { *style = "pw-record"; return true; }
    if (linux_which("parec", tool, cap))     { *style = "parec";     return true; }
    if (linux_which("arecord", tool, cap))   { *style = "arecord";   return true; }
    return false;
}

bool platform_audio_capture_start(void) {
    if (g_cap.pid > 0) return true;             /* already running */

    char tool[PATH_MAX];
    const char *style = NULL;
    if (!cap_find_tool(tool, sizeof tool, &style)) return false;

    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    const char *dev = g_cap.device[0] ? g_cap.device : NULL;

    /* Built by hand rather than as three fixed arrays: the device flag is
     * optional and each tool spells it differently, and a fixed array with a
     * conditional NULL in the middle is the kind of thing that silently
     * truncates the argument list. */
    char *argv[16];
    i32 a = 0;
    argv[a++] = tool;
    if (strcmp(style, "pw-record") == 0) {
        argv[a++] = (char *)"--rate=16000";
        argv[a++] = (char *)"--channels=1";
        argv[a++] = (char *)"--format=f32";
        argv[a++] = (char *)"--raw";
        if (dev) { argv[a++] = (char *)"--target"; argv[a++] = (char *)dev; }
        argv[a++] = (char *)"-";              /* "-" is stdout */
    } else if (strcmp(style, "parec") == 0) {
        argv[a++] = (char *)"--rate=16000";
        argv[a++] = (char *)"--channels=1";
        argv[a++] = (char *)"--format=float32le";
        argv[a++] = (char *)"--raw";
        argv[a++] = (char *)"--latency-msec=50";
        if (dev) { argv[a++] = (char *)"-d"; argv[a++] = (char *)dev; }
        /* parec writes to stdout when given no file. */
    } else {
        argv[a++] = (char *)"-q";
        argv[a++] = (char *)"-t"; argv[a++] = (char *)"raw";
        argv[a++] = (char *)"-f"; argv[a++] = (char *)"FLOAT_LE";
        argv[a++] = (char *)"-c"; argv[a++] = (char *)"1";
        argv[a++] = (char *)"-r"; argv[a++] = (char *)"16000";
        if (dev) { argv[a++] = (char *)"-D"; argv[a++] = (char *)dev; }
        /* arecord writes to stdout when given no file. */
    }
    argv[a] = NULL;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return false; }
    if (pid == 0) {
        (void)setsid();
        (void)dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execv(tool, argv);
        _exit(127);
    }
    close(pipefd[1]);

    g_cap.pid = pid;
    g_cap.fd  = pipefd[0];
    g_cap.rpos = 0;
    atomic_store_explicit(&g_cap.wpos, 0, memory_order_relaxed);
    atomic_store_explicit(&g_cap.stop, false, memory_order_relaxed);
    atomic_store_explicit(&g_cap.overflowed, false, memory_order_relaxed);
    atomic_store_explicit(&g_cap.level_q16, 0, memory_order_relaxed);

    if (pthread_create(&g_cap.thread, NULL, cap_reader, NULL) != 0) {
        close(g_cap.fd); g_cap.fd = -1;
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        g_cap.pid = 0;
        return false;
    }
    g_cap.thread_live = true;
    return true;
}

bool platform_audio_capture_stop(void) {
    if (g_cap.pid <= 0) return false;

    /* Kill the child first: the reader thread is parked in read(), and the
     * EOF that follows the child's exit is what wakes it. Setting the flag
     * alone would leave it blocked until the next sample arrived. */
    atomic_store_explicit(&g_cap.stop, true, memory_order_relaxed);
    kill(g_cap.pid, SIGTERM);
    if (g_cap.thread_live) {
        pthread_join(g_cap.thread, NULL);
        g_cap.thread_live = false;
    }
    while (waitpid(g_cap.pid, NULL, 0) < 0 && errno == EINTR) {}
    if (g_cap.fd >= 0) { close(g_cap.fd); g_cap.fd = -1; }
    g_cap.pid = 0;
    return true;
}

bool platform_audio_capturing(void) { return g_cap.pid > 0; }

i32 platform_audio_capture_read(f32 *out, i32 max_samples) {
    if (!out || max_samples <= 0 || g_cap.pid <= 0) return 0;

    u64 w = atomic_load_explicit(&g_cap.wpos, memory_order_acquire);
    u64 avail = w - g_cap.rpos;
    if (avail == 0) return 0;

    /* More than a ring's worth went by since the last drain: the oldest
     * samples are already overwritten, so skip to what is still intact and
     * say so rather than handing back a buffer with a seam in it. */
    if (avail > LIU_CAP_RING_FRAMES) {
        atomic_store_explicit(&g_cap.overflowed, true, memory_order_relaxed);
        g_cap.rpos = w - LIU_CAP_RING_FRAMES;
        avail = LIU_CAP_RING_FRAMES;
    }
    i32 n = (i32)(avail < (u64)max_samples ? avail : (u64)max_samples);
    for (i32 i = 0; i < n; i++)
        out[i] = g_cap.ring[(g_cap.rpos + (u64)i) % LIU_CAP_RING_FRAMES];
    g_cap.rpos += (u64)n;
    return n;
}

bool platform_audio_capture_overflowed(void) {
    return atomic_exchange_explicit(&g_cap.overflowed, false, memory_order_relaxed);
}

f32 platform_audio_capture_level(void) {
    u32 q = atomic_exchange_explicit(&g_cap.level_q16, 0, memory_order_relaxed);
    return (f32)q / 65536.0f;
}

/* No player on Linux yet: the dictation cues are silent here, and `set`
 * says so. */
bool platform_audio_clip_set(i32 slot, const f32 *mono, i32 frames, i32 sample_rate) {
    (void)slot; (void)mono; (void)frames; (void)sample_rate;
    return false;
}
bool platform_audio_clip_play(i32 slot, f32 gain) { (void)slot; (void)gain; return false; }

/* Enumerate PulseAudio/PipeWire sources through pactl, which speaks to both
 * (PipeWire ships pipewire-pulse, and its pactl is the same command). The
 * short-format output is one tab-separated record per line:
 *     index <TAB> name <TAB> driver <TAB> spec <TAB> state
 * Monitor sources are skipped: they are loopbacks of an OUTPUT, so picking one
 * records what the machine is playing rather than what is being said. */
i32 platform_audio_input_devices(PlatformAudioDevice *out, i32 max) {
    if (!out || max <= 0) return 0;
    char tool[PATH_MAX];
    if (!linux_which("pactl", tool, sizeof tool)) return 0;

    /* Which one the system would pick on its own. Asked first so the flag can
     * be filled in during the single pass below; an older pactl without the
     * verb just leaves it empty and nothing is marked default, rather than a
     * guess. */
    char def_name[256] = {0};
    {
        char *dargv[] = { tool, (char *)"get-default-source", NULL };
        pid_t dpid = 0;
        FILE *dp = linux_capture_open(tool, dargv, &dpid);
        if (dp) {
            if (fgets(def_name, sizeof def_name, dp)) {
                usize dl = strlen(def_name);
                while (dl && (def_name[dl-1] == '\n' || def_name[dl-1] == '\r'))
                    def_name[--dl] = '\0';
            }
            linux_capture_close(dp, dpid);
        }
    }

    char *largv[] = { tool, (char *)"list", (char *)"short", (char *)"sources", NULL };
    pid_t lpid = 0;
    FILE *p = linux_capture_open(tool, largv, &lpid);
    if (!p) return 0;

    i32 n = 0;
    char line[1024];
    while (n < max && fgets(line, sizeof line, p)) {
        char *name = strchr(line, '\t');
        if (!name) continue;
        name++;
        char *end = strchr(name, '\t');
        if (end) *end = '\0';
        usize len = strlen(name);
        while (len && (name[len-1] == '\n' || name[len-1] == '\r')) name[--len] = '\0';
        if (!len) continue;
        if (len >= 8 && strcmp(name + len - 8, ".monitor") == 0) continue;

        snprintf(out[n].uid,  sizeof out[n].uid,  "%s", name);
        /* No friendly name in the short listing, and the verbose one costs a
         * second process per refresh — the node name is what pactl itself
         * shows, so it is at least the name the user will recognise. */
        snprintf(out[n].name, sizeof out[n].name, "%s", name);
        /* Initialize is_default for every listed device because callers
         * inspect the field even when no default device is available. */
        out[n].is_default = def_name[0] && strcmp(name, def_name) == 0;
        n++;
    }
    linux_capture_close(p, lpid);
    return n;
}

void platform_audio_set_input_device(const char *uid) {
    /* Takes effect on the next capture_start, matching the documented macOS
     * behaviour — a device cannot be swapped under a running stream. */
    snprintf(g_cap.device, sizeof g_cap.device, "%s", uid ? uid : "");
}

const char *platform_audio_input_device(void) { return g_cap.device; }
/* The reported strip height defines the region where a double-click triggers the
 * title-bar gesture. On X11, the per-window and global entry points update the
 * same height because the backend has one window. */
void platform_window_set_titlebar_height(PlatformWindow *w, f32 points) {
    (void)w;
    g_titlebar_h_pt = points;
}
void platform_set_titlebar_height(f32 points) { g_titlebar_h_pt = points; }

/* =========================================================================
 * Global hotkey + Quake (drop-down) mode
 * ========================================================================= */

/* Inverse of map_keysym for the subset a hotkey can name. Only the keys
 * parse_quake_hotkey() in main.c can produce need to round-trip.
 *
 * KEY_UNKNOWN is not "no key" here: the KeyCode enum has no backtick member,
 * so parse_quake_hotkey deliberately returns KEY_UNKNOWN as the sentinel for
 * grave/backtick — which is the DEFAULT quake hotkey ("Ctrl+`"). The macOS
 * backend does the same thing against MACOS_KEYCODE_GRAVE. Treating it as
 * "nothing to bind" silently leaves the default hotkey unregistered. */
static KeySym keycode_to_keysym(u32 key) {
    if (key == KEY_UNKNOWN) return XK_grave;
    if (key >= KEY_A && key <= KEY_Z) return XK_a + (KeySym)(key - KEY_A);
    if (key >= KEY_0 && key <= KEY_9) return XK_0 + (KeySym)(key - KEY_0);
    if (key >= KEY_F1 && key <= KEY_F12) return XK_F1 + (KeySym)(key - KEY_F1);
    switch (key) {
    case KEY_SPACE:         return XK_space;
    case KEY_ESCAPE:        return XK_Escape;
    case KEY_ENTER:         return XK_Return;
    case KEY_TAB:           return XK_Tab;
    case KEY_BACKSPACE:     return XK_BackSpace;
    case KEY_INSERT:        return XK_Insert;
    case KEY_DELETE:        return XK_Delete;
    case KEY_HOME:          return XK_Home;
    case KEY_END:           return XK_End;
    case KEY_PAGE_UP:       return XK_Page_Up;
    case KEY_PAGE_DOWN:     return XK_Page_Down;
    case KEY_UP:            return XK_Up;
    case KEY_DOWN:          return XK_Down;
    case KEY_LEFT:          return XK_Left;
    case KEY_RIGHT:         return XK_Right;
    case KEY_LEFT_BRACKET:  return XK_bracketleft;
    case KEY_RIGHT_BRACKET: return XK_bracketright;
    case KEY_PERIOD:        return XK_period;
    case KEY_COMMA:         return XK_comma;
    case KEY_SLASH:         return XK_slash;
    default:                return NoSymbol;
    }
}

/* Caps Lock and Num Lock show up in the event state and in the grab, so a grab
 * registered for the bare modifier mask simply never fires while either is on.
 * The usual remedy is to register the same grab once per lock permutation.
 * Keep this set in sync with the mask stripped in linux_dispatch_xevent. */
static const unsigned int kLockPermutations[] = {
    0, LockMask, Mod2Mask, LockMask | Mod2Mask
};

static void linux_ungrab_hotkey(void) {
    Display *dpy = g_platform.display;
    if (!dpy || !g_platform.hotkey_keycode) return;
    Window root = RootWindow(dpy, g_platform.screen);
    for (usize i = 0; i < sizeof(kLockPermutations) / sizeof(kLockPermutations[0]); i++)
        XUngrabKey(dpy, (int)g_platform.hotkey_keycode,
                   g_platform.hotkey_modmask | kLockPermutations[i], root);
    g_platform.hotkey_keycode = 0;
    g_platform.hotkey_modmask = 0;
    XFlush(dpy);
}

void platform_register_global_hotkey(u32 key, u32 mods, GlobalHotkeyCallback cb,
                                     void *userdata) {
    Display *dpy = g_platform.display;
    if (!dpy) return;

    linux_ungrab_hotkey();
    g_platform.hotkey_cb = cb;
    g_platform.hotkey_user = userdata;
    if (!cb) return;   /* KEY_UNKNOWN means backtick — see keycode_to_keysym */

    KeySym ks = keycode_to_keysym(key);
    if (ks == NoSymbol) return;
    XKeyCode kc = XKeysymToKeycode(dpy, ks);
    if (kc == 0) return;

    unsigned int mask = 0;
    if (mods & MOD_SHIFT) mask |= ShiftMask;
    if (mods & MOD_CTRL)  mask |= ControlMask;
    if (mods & MOD_ALT)   mask |= Mod1Mask;
    if (mods & MOD_SUPER) mask |= Mod4Mask;

    Window root = RootWindow(dpy, g_platform.screen);
    for (usize i = 0; i < sizeof(kLockPermutations) / sizeof(kLockPermutations[0]); i++)
        XGrabKey(dpy, (int)kc, mask | kLockPermutations[i], root, True,
                 GrabModeAsync, GrabModeAsync);
    XSync(dpy, False);   /* flush any BadAccess through the error handler now */

    g_platform.hotkey_keycode = (unsigned int)kc;
    g_platform.hotkey_modmask = mask;
}

void platform_set_quake_params(f32 height_ratio, f32 anim_duration) {
    (void)anim_duration;   /* the slide-down is a compositor effect on X11 */
    if (!isfinite(height_ratio)) return;
    if (height_ratio < 0.1f) height_ratio = 0.1f;
    if (height_ratio > 1.0f) height_ratio = 1.0f;
    for (i32 i = 0; i < g_window_count; i++) g_windows[i]->quake_height_ratio = height_ratio;
}

/* Park the window as a drop-down: full work-area width, anchored to the top
 * edge, always on top, and absent from the taskbar and pager (it is summoned
 * by hotkey, so a taskbar button for it would be noise). */
static void linux_quake_place(PlatformWindow *w) {
    Display *dpy = g_platform.display;
    /* Across the monitor the pointer is on: the work area alone is the whole
     * desktop, so with two monitors the drop-down spanned both. */
    i32 ax = 0, ay = 0, aw = 0, ah = 0;
    {
        Window root = None, child = None;
        int rx = 0, ry = 0, wx = 0, wy = 0;
        unsigned int mask = 0;
        if (!XQueryPointer(dpy, RootWindow(dpy, g_platform.screen), &root, &child,
                           &rx, &ry, &wx, &wy, &mask))
            rx = ry = 0;
        X11Rect vis = linux_visible_frame(rx, ry);
        ax = vis.x; ay = vis.y; aw = vis.w; ah = vis.h;
    }
    i32 fl, fr, ft, fb;
    linux_frame_extents(w, &fl, &fr, &ft, &fb);

    /* Asymmetry worth stating, because getting it backwards is silent: under
     * the default NorthWest gravity a WM interprets the requested x,y as the
     * FRAME's top-left (ICCCM 4.1.2.3), but width/height as the CLIENT area.
     * So the position needs no decoration offset, while the size must have the
     * decoration subtracted or the frame overflows the work area. */
    i32 width  = aw - (fl + fr);
    i32 height = (i32)((f32)ah * w->quake_height_ratio) - (ft + fb);
    if (width  < 160) width  = 160;
    if (height < 120) height = 120;
    if (height > ah)  height = ah;
    XMoveResizeWindow(dpy, w->xwindow, ax, ay,
                      (unsigned)width, (unsigned)height);
    XFlush(dpy);
}

void platform_set_quake_mode(PlatformWindow *w, bool enable) {
    Display *dpy = g_platform.display;
    if (!w || !dpy || w->quake == enable) return;

    if (enable) {
        /* Remember the ordinary geometry so leaving quake mode restores the
         * window the user had, not an arbitrary default. */
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dpy, w->xwindow, &attrs)) {
            Window child = None;
            int rx = 0, ry = 0;
            XTranslateCoordinates(dpy, w->xwindow,
                                  RootWindow(dpy, g_platform.screen),
                                  0, 0, &rx, &ry, &child);
            w->saved_x = rx; w->saved_y = ry;
            w->saved_w = attrs.width; w->saved_h = attrs.height;
        }
        w->quake = true;
        w->quake_shown = true;
        x11_wm_state(w->xwindow, LIU_NET_WM_STATE_ADD,
                     g_platform.a_net_wm_state_above,
                     g_platform.a_net_wm_state_skip_taskbar);
        x11_wm_state(w->xwindow, LIU_NET_WM_STATE_ADD,
                     g_platform.a_net_wm_state_skip_pager, None);
        linux_quake_place(w);
    } else {
        w->quake = false;
        x11_wm_state(w->xwindow, LIU_NET_WM_STATE_REMOVE,
                     g_platform.a_net_wm_state_above,
                     g_platform.a_net_wm_state_skip_taskbar);
        x11_wm_state(w->xwindow, LIU_NET_WM_STATE_REMOVE,
                     g_platform.a_net_wm_state_skip_pager, None);
        if (!w->quake_shown) { XMapWindow(dpy, w->xwindow); w->quake_shown = true; }
        if (w->saved_w > 0 && w->saved_h > 0)
            XMoveResizeWindow(dpy, w->xwindow, w->saved_x, w->saved_y,
                              (unsigned)w->saved_w, (unsigned)w->saved_h);
        XFlush(dpy);
    }
}

void platform_toggle_quake_window(PlatformWindow *w) {
    Display *dpy = g_platform.display;
    if (!w || !dpy) return;

    /* The hotkey is registered before quake mode is switched on, so the first
     * press is what enables it. */
    if (!w->quake) { platform_set_quake_mode(w, true); return; }

    if (w->quake_shown) {
        XUnmapWindow(dpy, w->xwindow);
        w->quake_shown = false;
    } else {
        XMapWindow(dpy, w->xwindow);
        linux_quake_place(w);
        XRaiseWindow(dpy, w->xwindow);
        /* Ask the WM to focus the window rather than calling XSetInputFocus:
         * focus stealing prevention would otherwise refuse, and the summoned
         * window would appear without a cursor. */
        x11_send_root_msg(w->xwindow, g_platform.a_net_active_window,
                          1, CurrentTime, 0, 0, 0);
        w->quake_shown = true;
    }
    XFlush(dpy);
}

bool platform_is_quake_mode(PlatformWindow *w) { return w && w->quake; }

#endif /* PLATFORM_LINUX */
