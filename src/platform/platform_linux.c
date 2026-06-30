/*
 * Liu - Linux platform layer (X11 + GLX)
 */
#ifdef PLATFORM_LINUX

#include "platform/platform.h"   /* project KeyCode enum — must precede X11 */

/* X11 typedefs `KeyCode` (unsigned char), which collides with platform.h's
 * KeyCode enum. Rename X11's to XKeyCode across the X11 headers; in the body we
 * pass keycodes to Xlib as XKeyCode and keep KeyCode for our own enum. */
#define KeyCode XKeyCode
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>        /* XK_a..XK_z for the Ctrl+letter path */
#include <X11/cursorfont.h>    /* XC_* cursor shapes */
#include <X11/Xresource.h>     /* Xrm* for Xft.dpi */
#undef KeyCode

#include <GL/gl.h>
#include <GL/glx.h>
#include "renderer/gl_loader.h"   /* after <GL/gl.h>: its typedefs use GL types */
#include <locale.h>            /* setlocale — required for XIM to open */
#include <time.h>
#include <stdio.h>             /* fprintf — XIM / gl_loader diagnostics */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>

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

static void linux_spawn_opener(const char *arg) {
    if (!arg || !arg[0]) return;
    pid_t pid;
    pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild < 0) _exit(127);
        if (grandchild > 0) _exit(0);

        (void)setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        char *argv[] = { "xdg-open", (char *)arg, NULL };
        execvp("xdg-open", argv);
        _exit(127);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
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
     * platform_take_fd_fire_count(). Plain i32 is fine — both the poll() wait
     * and the take happen on the single-threaded main loop. */
    struct pollfd *poll_fds;
    i32         poll_cap;
    i32         fd_fire_count;

    XIM         xim;            /* input method (connection-scoped); NULL if none */

    /* Clipboard (X11 CLIPBOARD selection): we cache our copy and answer
     * SelectionRequest from it; _get pumps for SelectionNotify into clip_recv. */
    Atom        a_clipboard, a_targets, a_utf8, a_text, a_incr, a_liu_sel;
    char       *clip_text;      /* what we currently own on CLIPBOARD (malloc'd) */
    char       *clip_recv;      /* persistent buffer returned by _get (malloc'd) */
    bool        clip_recv_done; /* set by SelectionNotify; polled by _get pump */

    Cursor      cur[5];         /* XCreateFontCursor cache, indexed by CursorType */
    bool        cur_init;
} g_platform = {0};

#define MAX_EVENTS 256
static PlatformEvent g_events[MAX_EVENTS];
static i32 g_event_read = 0, g_event_write = 0, g_event_count = 0;

static PlatformWindow *g_active_window = NULL;  /* single-window backend */
static void linux_dispatch_xevent(XEvent *xep); /* fwd: reused by clipboard pump */

static void push_event(PlatformEvent e) {
    if (g_event_count >= MAX_EVENTS) {
        if (e.type == EVENT_SCROLL && g_event_count > 0) {
            i32 last = (g_event_write + MAX_EVENTS - 1) % MAX_EVENTS;
            PlatformEvent *prev = &g_events[last];
            if (prev->type == EVENT_SCROLL &&
                prev->scroll.mods == e.scroll.mods &&
                prev->scroll.precise == e.scroll.precise) {
                prev->scroll.dx += e.scroll.dx;
                prev->scroll.dy += e.scroll.dy;
            }
        }
        return;
    }
    g_events[g_event_write] = e;
    g_event_write = (g_event_write + 1) % MAX_EVENTS;
    g_event_count++;
}

static void linux_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void linux_wake_waiter(void) {
    if (g_platform.wake_pipe[1] >= 0) {
        u8 b = 1;
        (void)write(g_platform.wake_pipe[1], &b, 1);
    }
}

static void linux_drain_wake_pipe(void) {
    if (g_platform.wake_pipe[0] < 0) return;
    u8 buf[64];
    while (read(g_platform.wake_pipe[0], buf, sizeof(buf)) > 0) {}
}

struct PlatformWindow {
    Window      xwindow;
    GLXContext  glx_context;
    Atom        wm_delete;
    i32         width, height;
    bool        should_close;
    XIC         xic;            /* per-window input context; NULL = no XIM */
};

bool platform_init(void) {
    if (g_platform.initialized) return true;
    g_platform.wake_pipe[0] = -1;
    g_platform.wake_pipe[1] = -1;
    g_platform.display = XOpenDisplay(NULL);
    if (!g_platform.display) return false;
    g_platform.screen = DefaultScreen(g_platform.display);
    clock_gettime(CLOCK_MONOTONIC, &g_platform.time_base);
    pthread_mutex_init(&g_platform.watch_lock, NULL);
    if (pipe(g_platform.wake_pipe) == 0) {
        linux_set_nonblock(g_platform.wake_pipe[0]);
        linux_set_nonblock(g_platform.wake_pipe[1]);
    }

    /* Input method (XIM) for correct multi-byte / dead-key / compose input.
     * Needs a locale + X locale-modifier support; failure is non-fatal (we fall
     * back to XLookupString without an input context). */
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
    g_platform.a_utf8      = XInternAtom(d, "UTF8_STRING", False);
    g_platform.a_text      = XInternAtom(d, "TEXT", False);
    g_platform.a_incr      = XInternAtom(d, "INCR", False);
    g_platform.a_liu_sel   = XInternAtom(d, "LIU_SELECTION", False);

    g_platform.initialized = true;
    return true;
}

void platform_shutdown(void) {
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
    g_platform.fd_fire_count = 0;
    pthread_mutex_destroy(&g_platform.watch_lock);
    if (g_platform.display && g_platform.cur_init) {
        for (int i = 0; i < 5; i++)
            if (g_platform.cur[i]) XFreeCursor(g_platform.display, g_platform.cur[i]);
        g_platform.cur_init = false;
    }
    if (g_platform.xim) { XCloseIM(g_platform.xim); g_platform.xim = NULL; }
    free(g_platform.clip_text); g_platform.clip_text = NULL;
    free(g_platform.clip_recv); g_platform.clip_recv = NULL;
    if (g_platform.display) XCloseDisplay(g_platform.display);
    g_platform.display = NULL;
    g_platform.initialized = false;
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
    GLXFBConfig chosen = fbc[0];
    XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, chosen);
    if (!vi) { XFree(fbc); free(w); return NULL; }

    Window root = RootWindow(dpy, screen);
    XSetWindowAttributes swa = {0};
    swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.background_pixel = BlackPixel(dpy, screen);
    /* PropertyChangeMask lets us serve SelectionRequest cleanly. */
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask | FocusChangeMask | PropertyChangeMask;

    w->xwindow = XCreateWindow(dpy, root, 0, 0,
                                (unsigned)cfg->width, (unsigned)cfg->height, 0,
                                vi->depth, InputOutput, vi->visual,
                                CWColormap | CWEventMask | CWBackPixel, &swa);
    w->width = cfg->width;
    w->height = cfg->height;

    XStoreName(dpy, w->xwindow, cfg->title);
    w->wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, w->xwindow, &w->wm_delete, 1);

    /* Per-window input context (root style: no inline preedit, but compose/
     * dead-key/CJK still commit via Xutf8LookupString). NULL => XLookupString. */
    w->xic = NULL;
    if (g_platform.xim) {
        w->xic = XCreateIC(g_platform.xim,
                           XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                           XNClientWindow, w->xwindow,
                           XNFocusWindow,  w->xwindow,
                           (void *)NULL);
        if (w->xic) XSetICFocus(w->xic);
    }

    XMapWindow(dpy, w->xwindow);

    /* GL 3.3 CORE via the ARB entrypoint; legacy fallback on old drivers. */
    PFN_glXCreateContextAttribsARB create_ctx =
        (PFN_glXCreateContextAttribsARB)glXGetProcAddressARB(
            (const GLubyte *)"glXCreateContextAttribsARB");
    w->glx_context = NULL;
    if (create_ctx) {
        int ctx_attrs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };
        w->glx_context = create_ctx(dpy, chosen, NULL, True, ctx_attrs);
        XSync(dpy, False);   /* surface a NULL context before we test it */
    }
    if (!w->glx_context)
        w->glx_context = glXCreateNewContext(dpy, chosen, GLX_RGBA_TYPE, NULL, True);
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

    g_active_window = w;   /* single-window backend: poll loop reaches xic/size */

    XFree(vi);
    XFree(fbc);
    return w;
}

void platform_window_destroy(PlatformWindow *w) {
    if (!w) return;
    Display *dpy = g_platform.display;
    if (w->xic) { XUnsetICFocus(w->xic); XDestroyIC(w->xic); w->xic = NULL; }
    if (g_active_window == w) g_active_window = NULL;
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, w->glx_context);
    XDestroyWindow(dpy, w->xwindow);
    free(w);
}

void platform_window_set_title(PlatformWindow *w, const char *title) {
    XStoreName(g_platform.display, w->xwindow, title);
}

void platform_window_set_transparent(PlatformWindow *w, bool transparent) {
    (void)w; (void)transparent;
}

void platform_window_set_opacity(PlatformWindow *w, f32 opacity) {
    (void)w; (void)opacity;
}

void platform_window_set_vsync(PlatformWindow *w, bool enabled) {
    (void)w; (void)enabled;
}

f32 platform_window_max_refresh_hz(PlatformWindow *w) {
    (void)w;   /* X11/GLX: no cheap per-window query; 60 Hz is a safe floor */
    return 60.0f;
}

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

f32 platform_window_get_dpi_scale(PlatformWindow *w) {
    (void)w;
    f32 scale = 1.0f;
    char *rms = XResourceManagerString(g_platform.display);
    if (rms) {
        XrmDatabase db = XrmGetStringDatabase(rms);
        if (db) {
            char *type = NULL; XrmValue val;
            if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &val) && val.addr) {
                double dpi = atof(val.addr);
                if (dpi > 1.0) scale = (f32)(dpi / 96.0);
            }
            XrmDestroyDatabase(db);
        }
    }
    if (scale < 1.0f) scale = 1.0f;   /* never downscale our point dimensions */
    return scale;
}

void platform_window_snap(PlatformWindow *w, WindowSnap pos) {
    (void)w; (void)pos;
    /* TODO: implement X11 window repositioning via XMoveResizeWindow */
}

bool platform_window_should_close(PlatformWindow *w) { return w->should_close; }

void platform_window_swap_buffers(PlatformWindow *w) {
    glXSwapBuffers(g_platform.display, w->xwindow);
}

void platform_make_current(PlatformWindow *w) {
    glXMakeCurrent(g_platform.display, w->xwindow, w->glx_context);
    g_active_window = w;
}

bool platform_begin_file_drag(PlatformWindow *w,
                              const PlatformFilePromise *items, i32 count) {
    (void)w; (void)items; (void)count;
    return false;  /* X11 XDND support is future work */
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
        case XK_F1:  return KEY_F1;  case XK_F2:  return KEY_F2;
        case XK_F3:  return KEY_F3;  case XK_F4:  return KEY_F4;
        case XK_F5:  return KEY_F5;  case XK_F6:  return KEY_F6;
        case XK_F7:  return KEY_F7;  case XK_F8:  return KEY_F8;
        case XK_F9:  return KEY_F9;  case XK_F10: return KEY_F10;
        case XK_F11: return KEY_F11; case XK_F12: return KEY_F12;
        default: return KEY_UNKNOWN;
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

void platform_set_titlebar_zoom_query(PlatformTitlebarZoomQuery fn, void *user) {
    /* No title-bar double-click zoom on X11. Stub for clean cross-platform link. */
    (void)fn;
    (void)user;
}

void platform_set_render_callback(PlatformRenderCallback cb, void *user) {
    /* X11 lacks the macOS event-tracking-mode trap that necessitates the
     * timer-driven render hook. Stub so cross-platform code links cleanly. */
    (void)cb;
    (void)user;
}

static void linux_dispatch_xevent(XEvent *xep) {
    Display *dpy = g_platform.display;
    PlatformWindow *win = g_active_window;
    XEvent xe = *xep;

    /* IM first refusal: a key that's part of a compose/dead-key/CJK sequence is
     * swallowed here and must not be processed further. */
    if (XFilterEvent(&xe, None)) return;

    switch (xe.type) {
    case KeyPress: {
        unsigned int kc_raw = xe.xkey.keycode;
        KeySym ks = NoSymbol;
        char buf[64];
        int len = 0;
        Status status = 0;

        if (win && win->xic) {
            len = Xutf8LookupString(win->xic, &xe.xkey,
                                    buf, (int)sizeof(buf) - 1, &ks, &status);
            if (status == XBufferOverflow) len = 0;   /* >63 bytes: skip text */
        } else {
            len = XLookupString(&xe.xkey, buf, (int)sizeof(buf) - 1, &ks, NULL);
        }
        if (ks == NoSymbol)
            ks = XkbKeycodeToKeysym(dpy, (XKeyCode)kc_raw, 0, 0);
        buf[len > 0 ? len : 0] = '\0';

        u32 mods = get_mods(xe.xkey.state);

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
            .key = { .key = map_keysym(ks), .mods = mods, .repeat = repeat }
        });

        /* Super (Cmd-equivalent) combos: KEY_DOWN only — shortcut handled in
         * the main loop (mirrors macOS keyDown early-return on MOD_SUPER). */
        if (mods & MOD_SUPER) break;

        /* Ctrl+a..z: emit the control codepoint directly (the IM produces no
         * text for control chars) and stop, so we don't double-emit. */
        if ((mods & MOD_CTRL) && ks >= XK_a && ks <= XK_z) {
            push_event((PlatformEvent){
                .type = EVENT_CHAR_INPUT,
                .char_input = { .codepoint = (u32)(ks - XK_a + 1), .mods = mods }
            });
            break;
        }
        if (mods & MOD_CTRL) break;   /* other Ctrl combos: no text */

        /* Decode committed UTF-8 into codepoints; one CHAR_INPUT per codepoint
         * (NOT per byte — the old bug). Skip C0/DEL: Enter/Tab/Esc/Backspace go
         * via the KEY_DOWN channel. */
        if ((status == XLookupChars || status == XLookupBoth || !win || !win->xic)
            && len > 0) {
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
                    .char_input = { .codepoint = cp, .mods = mods }
                });
            }
        }
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
        push_event((PlatformEvent){
            .type = EVENT_KEY_UP,
            .key = { .key = map_keysym(ks), .mods = get_mods(xe.xkey.state) }
        });
        break;
    }
    case ButtonPress:
        if (xe.xbutton.button >= 4 && xe.xbutton.button <= 7) {
            push_event((PlatformEvent){
                .type = EVENT_MOUSE_MOVE,
                .mouse = { .button = -1, .x = xe.xbutton.x, .y = xe.xbutton.y,
                           .mods = get_mods(xe.xbutton.state) }
            });
            f32 dx = 0.0f, dy = 0.0f;
            if (xe.xbutton.button == 4) dy =  1.0f;
            if (xe.xbutton.button == 5) dy = -1.0f;
            if (xe.xbutton.button == 6) dx = -1.0f;
            if (xe.xbutton.button == 7) dx =  1.0f;
            push_event((PlatformEvent){
                .type = EVENT_SCROLL,
                .scroll = { .dx = dx, .dy = dy,
                            .mods = get_mods(xe.xbutton.state),
                            .precise = false }
            });
            break;
        }
        push_event((PlatformEvent){
            .type = EVENT_MOUSE_DOWN,
            .mouse = { .button = xe.xbutton.button - 1,
                       .x = xe.xbutton.x, .y = xe.xbutton.y }
        });
        break;
    case ButtonRelease:
        if (xe.xbutton.button >= 4 && xe.xbutton.button <= 7) break;
        push_event((PlatformEvent){
            .type = EVENT_MOUSE_UP,
            .mouse = { .button = xe.xbutton.button - 1,
                       .x = xe.xbutton.x, .y = xe.xbutton.y }
        });
        break;
    case MotionNotify:
        push_event((PlatformEvent){
            .type = EVENT_MOUSE_MOVE,
            .mouse = { .button = -1, .x = xe.xmotion.x, .y = xe.xmotion.y }
        });
        break;
    case ConfigureNotify: {
        /* THE RESIZE FIX: write back the cached size and only emit on a real
         * change so a pure-move ConfigureNotify doesn't spam a re-layout. */
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
    case ClientMessage:
        push_event((PlatformEvent){ .type = EVENT_CLOSE });
        break;
    case FocusIn:
        if (win && win->xic) XSetICFocus(win->xic);
        push_event((PlatformEvent){ .type = EVENT_FOCUS, .focus.focused = true });
        break;
    case FocusOut:
        if (win && win->xic) XUnsetICFocus(win->xic);
        push_event((PlatformEvent){ .type = EVENT_BLUR, .focus.focused = false });
        break;

    case SelectionRequest: {
        /* Another client wants our CLIPBOARD. Serve UTF8_STRING/TEXT/STRING,
         * answer TARGETS, refuse the rest (ICCCM). */
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

        if (req->selection == g_platform.a_clipboard && g_platform.clip_text) {
            if (req->target == g_platform.a_targets) {
                Atom offer[] = { g_platform.a_targets, g_platform.a_utf8,
                                 g_platform.a_text, XA_STRING };
                XChangeProperty(dpy, req->requestor, prop, XA_ATOM, 32,
                                PropModeReplace, (const unsigned char *)offer,
                                (int)(sizeof(offer) / sizeof(offer[0])));
                ev.property = prop;
            } else if (req->target == g_platform.a_utf8 ||
                       req->target == g_platform.a_text ||
                       req->target == XA_STRING) {
                XChangeProperty(dpy, req->requestor, prop, req->target, 8,
                                PropModeReplace,
                                (const unsigned char *)g_platform.clip_text,
                                (int)strlen(g_platform.clip_text));
                ev.property = prop;
            }
        }
        XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&ev);
        XFlush(dpy);
        break;
    }
    case SelectionClear:
        if (xe.xselectionclear.selection == g_platform.a_clipboard) {
            free(g_platform.clip_text);
            g_platform.clip_text = NULL;
        }
        break;
    case SelectionNotify: {
        XSelectionEvent *sel = &xe.xselection;
        free(g_platform.clip_recv);
        g_platform.clip_recv = NULL;
        if (sel->property != None) {
            Atom actual_type = None; int actual_fmt = 0;
            unsigned long nitems = 0, bytes_after = 0;
            unsigned char *data = NULL;
            if (XGetWindowProperty(dpy, sel->requestor, sel->property,
                                   0, (~0L), True /*delete*/, AnyPropertyType,
                                   &actual_type, &actual_fmt, &nitems,
                                   &bytes_after, &data) == Success && data) {
                if (actual_type != g_platform.a_incr) {   /* INCR: skip (oversize) */
                    g_platform.clip_recv = malloc((usize)nitems + 1);
                    if (g_platform.clip_recv) {
                        memcpy(g_platform.clip_recv, data, (usize)nitems);
                        g_platform.clip_recv[nitems] = '\0';
                    }
                }
                XFree(data);
            }
        }
        g_platform.clip_recv_done = true;
        break;
    }
    }
}

void platform_poll_events(void) {
    Display *dpy = g_platform.display;
    while (XPending(dpy)) {
        XEvent xe;
        XNextEvent(dpy, &xe);
        linux_dispatch_xevent(&xe);
    }
}

void platform_wait_events(f64 timeout_sec) {
    if (!g_platform.display) return;
    if (XPending(g_platform.display) > 0) return;

    int xfd = ConnectionNumber(g_platform.display);
    i32 watch_count = 0;
    pthread_mutex_lock(&g_platform.watch_lock);
    watch_count = g_platform.watch_count;
    i32 nfds = 1 + (g_platform.wake_pipe[0] >= 0 ? 1 : 0) + watch_count;
    if (nfds > g_platform.poll_cap) {
        i32 new_cap = g_platform.poll_cap ? g_platform.poll_cap : 16;
        while (new_cap < nfds) new_cap *= 2;
        struct pollfd *grown = realloc(g_platform.poll_fds,
                                       (usize)new_cap * sizeof(struct pollfd));
        if (!grown) {
            pthread_mutex_unlock(&g_platform.watch_lock);
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
    for (i32 i = 0; i < watch_count; i++)
        pfds[idx++] = (struct pollfd){ .fd = g_platform.watch_fds[i], .events = POLLIN };
    pthread_mutex_unlock(&g_platform.watch_lock);

    int timeout_ms;
    if (timeout_sec < 0.0) timeout_ms = -1;
    else {
        f64 ms = timeout_sec * 1000.0;
        timeout_ms = ms > 2147483647.0 ? 2147483647 : (int)ceil(ms);
    }
    int ready = poll(pfds, (nfds_t)nfds, timeout_ms);
    if (ready > 0) {
        /* Count only the watched fds for platform_take_fd_fire_count(). The X11
         * connection fd and the internal wake pipe still wake event processing
         * (we return and the main loop pumps), but they are not session I/O so
         * they must not defeat the idle drain skip. */
        i32 watch_base = nfds - watch_count;
        for (i32 i = watch_base; i < nfds; i++)
            if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP))
                g_platform.fd_fire_count++;
    }
    linux_drain_wake_pipe();
}

bool platform_next_event(PlatformEvent *event) {
    if (g_event_count == 0) return false;
    *event = g_events[g_event_read];
    g_event_read = (g_event_read + 1) % MAX_EVENTS;
    g_event_count--;
    return true;
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
        i32 new_cap = g_platform.watch_cap ? g_platform.watch_cap * 2 : 32;
        int *new_fds = realloc(g_platform.watch_fds, (usize)new_cap * sizeof(int));
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
    /* Plain (non-atomic) read-and-zero: the accumulator is only touched from
     * the main loop (poll() wait in platform_wait_events and this take). */
    i32 n = g_platform.fd_fire_count;
    g_platform.fd_fire_count = 0;
    return n;
}

const char *platform_clipboard_get(void) {
    Display *dpy = g_platform.display;
    PlatformWindow *win = g_active_window;
    if (!dpy || !win) return "";

    /* Fast path: we own CLIPBOARD — return our copy (a client can't reliably
     * XConvertSelection from itself). */
    if (XGetSelectionOwner(dpy, g_platform.a_clipboard) == win->xwindow)
        return g_platform.clip_text ? g_platform.clip_text : "";

    /* Ask the owner to write UTF8_STRING into our transfer property, then pump
     * the normal dispatch path until SelectionNotify (or a ~0.5s timeout). */
    g_platform.clip_recv_done = false;
    XConvertSelection(dpy, g_platform.a_clipboard, g_platform.a_utf8,
                      g_platform.a_liu_sel, win->xwindow, CurrentTime);
    XFlush(dpy);

    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    while (!g_platform.clip_recv_done) {
        struct pollfd pfd = { .fd = ConnectionNumber(dpy), .events = POLLIN };
        (void)poll(&pfd, 1, 50);
        platform_poll_events();   /* drains SelectionNotify into clip_recv */
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        f64 el = (now.tv_sec - start.tv_sec) +
                 (now.tv_nsec - start.tv_nsec) / 1e9;
        if (el > 0.5) break;       /* owner unresponsive / no owner — give up */
    }
    return g_platform.clip_recv ? g_platform.clip_recv : "";
}

void platform_clipboard_set(const char *text) {
    Display *dpy = g_platform.display;
    PlatformWindow *win = g_active_window;
    if (!dpy || !win) return;
    free(g_platform.clip_text);
    g_platform.clip_text = text ? strdup(text) : NULL;
    /* Own CLIPBOARD; we serve bytes from the SelectionRequest case. */
    XSetSelectionOwner(dpy, g_platform.a_clipboard,
                       g_platform.clip_text ? win->xwindow : None, CurrentTime);
    XFlush(dpy);
}

void platform_set_option_as_alt(bool enable) { (void)enable; }

void platform_open_url(const char *url) {
    if (!url || !url[0]) return;
    linux_spawn_opener(url);
}

void platform_open_path(const char *path) {
    if (!path || !path[0]) return;
    linux_spawn_opener(path);
}

f64 platform_time_sec(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_platform.time_base.tv_sec) +
           (now.tv_nsec - g_platform.time_base.tv_nsec) / 1e9;
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

void platform_set_cursor(CursorType type) {
    Display *dpy = g_platform.display;
    PlatformWindow *win = g_active_window;
    if (!dpy || !win) return;
    if (!g_platform.cur_init) {
        g_platform.cur[CURSOR_DEFAULT]  = XCreateFontCursor(dpy, XC_left_ptr);
        g_platform.cur[CURSOR_TEXT]     = XCreateFontCursor(dpy, XC_xterm);
        g_platform.cur[CURSOR_RESIZE_H] = XCreateFontCursor(dpy, XC_sb_h_double_arrow);
        g_platform.cur[CURSOR_RESIZE_V] = XCreateFontCursor(dpy, XC_sb_v_double_arrow);
        g_platform.cur[CURSOR_POINTER]  = XCreateFontCursor(dpy, XC_hand2);
        g_platform.cur_init = true;
    }
    if ((int)type >= 0 && (int)type < 5 && g_platform.cur[type])
        XDefineCursor(dpy, win->xwindow, g_platform.cur[type]);
}
void platform_set_ime_cursor_pos(f32 x, f32 y, f32 cell_w, f32 cell_h) {
    (void)x; (void)y; (void)cell_w; (void)cell_h;
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

/* Bell / Notifications — stubs for Linux */
void platform_play_bell(void) {}
void platform_set_dock_badge(const char *text) { (void)text; }
void platform_post_notification(const char *title, const char *body) {
    /* TODO: Use libnotify or notify-send for Linux desktop notifications */
    (void)title; (void)body;
}
void platform_request_attention(void) {
    /* TODO: Use _NET_WM_STATE_DEMANDS_ATTENTION X11 hint */
}
bool platform_is_app_focused(void) {
    /* TODO: Check X11 _NET_ACTIVE_WINDOW */
    return true;
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

bool platform_watch_file(const char *path, FileWatchCallback cb, void *userdata) {
    (void)path;
    (void)cb;
    (void)userdata;
    return false;
}

/* Custom-chrome window drag: X11 WMs move windows via their own title-bar /
 * borders, so this is a no-op (a borderless custom chrome would use the
 * _NET_WM_MOVERESIZE client message). */
void platform_begin_window_drag(PlatformWindow *w) { (void)w; }

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

void platform_poll_file_watches(void) {}

void platform_unwatch_file(void) {}

const char *platform_open_file_dialog(const char *title, const char *extensions) {
    (void)title; (void)extensions; return NULL;
}
const char *platform_open_folder_dialog(const char *title) { (void)title; return NULL; }

/* Audio recording — not implemented on Linux yet */
bool platform_audio_record_start(const char *out_path) { (void)out_path; return false; }
bool platform_audio_record_stop(void) { return false; }
bool platform_audio_recording(void) { return false; }
f64  platform_audio_record_elapsed(void) { return 0.0; }
PlatformMicPermission platform_audio_mic_permission(void) { return PLATFORM_MIC_UNKNOWN; }
void platform_open_microphone_settings(void) {}
void platform_set_titlebar_height(f32 points) { (void)points; }

/* Quake mode — stubs for Linux */
void platform_register_global_hotkey(u32 key, u32 mods, GlobalHotkeyCallback cb, void *userdata) {
    (void)key; (void)mods; (void)cb; (void)userdata;
}
void platform_set_quake_params(f32 height_ratio, f32 anim_duration) { (void)height_ratio; (void)anim_duration; }
void platform_set_quake_mode(PlatformWindow *w, bool enable) { (void)w; (void)enable; }
void platform_toggle_quake_window(PlatformWindow *w) { (void)w; }
bool platform_is_quake_mode(PlatformWindow *w) { (void)w; return false; }

#endif /* PLATFORM_LINUX */
