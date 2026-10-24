/*
 * Liu - AT-SPI2 bridge (X11): a Linux screen reader reads the focused pane.
 *
 * The X11 backend draws the terminal with GL into one window, which on its
 * own gives the desktop's accessibility tree nothing to read. This bridge
 * puts Liu on the accessibility bus as an AT-SPI2 application and answers
 * from the same per-window provider the macOS view uses (platform_a11y.h):
 *
 *   application "Liu"            Accessible, Application
 *     frame  (one per window)    Accessible, Component     name = window title
 *       terminal                 Accessible, Component, Text
 *                                the active tab's focused pane; a NEW object
 *                                (another path) whenever that pane changes
 *
 * libdbus only, no toolkit. The bus is found and joined on a detached worker
 * (every round trip there has a timeout); the main thread only ever reads and
 * writes the connection without blocking, from the loop's own turn. Without a
 * session bus, without an accessibility bus or without a registry nothing is
 * connected and every call below is one pointer test.
 *
 * Events are sent only while an assistive app has asked the registry for
 * them, and the change detector rides frames the app presents anyway — see
 * atspi_bridge.c for the pacing.
 *
 * Every function is main-thread only.
 */
#ifndef LIU_PLATFORM_ATSPI_BRIDGE_H
#define LIU_PLATFORM_ATSPI_BRIDGE_H

#include "core/types.h"
#include "platform/platform_a11y.h"
#include "platform/atspi_text.h"
#include "platform/atspi_typing.h"

/* Per window, embedded in the X11 backend's PlatformWindow, so a second
 * window is a second frame with its own state. Zeroed with the window. */
typedef struct AtspiWindow {
    PlatformWindow        *pw;
    struct AtspiWindow    *next;          /* the application's frames, in creation order */
    PlatformA11yProviderFn fn;
    void                  *user;
    u32                    id;            /* the frame's path number; 0 = not attached */
    u32                    term_serial;   /* the terminal object's path number */
    u64                    source;        /* the pane it describes; 0 = no terminal object */
    bool                   active;        /* the window has the keyboard */
    bool                   name_dirty;    /* the title moved: check the terminal's label */
    char                   title[256];

    /* Change detection: what assistive apps were last told, and when. */
    u64                    seen_value, seen_sel;
    f64                    last_post;
    f64                    post_due;      /* a trailing check is owed at this time; 0 = none */
    AtspiTyping            typing;        /* keystrokes since the last post (atspi_typing.h) */

    /* Two snapshots: `cur` answers queries, `told` is the text the last
     * events described (the base of the next diff). They may be the same. */
    AtspiText              snap[2];
    u8                     cur, told;
    bool                   have_cur, have_told;
} AtspiWindow;

#ifdef LIU_HAVE_DBUS

void atspi_window_attach(AtspiWindow *aw, PlatformWindow *pw);
void atspi_window_detach(AtspiWindow *aw);
void atspi_window_set_provider(AtspiWindow *aw, PlatformA11yProviderFn fn, void *user);
void atspi_window_set_title(AtspiWindow *aw, const char *title);
void atspi_window_set_active(AtspiWindow *aw, bool active);
void atspi_window_set_showing(AtspiWindow *aw, bool showing);
/* After a frame of this window was presented (platform_window_swap_buffers). */
void atspi_window_frame_presented(AtspiWindow *aw);
/* A keystroke reached this window and typed `chars` characters (0 for a key
 * that types nothing). Its echo is posted with the next presented frame. */
void atspi_window_key(AtspiWindow *aw, u32 chars);

/* The connection's fd for the loop's wait, -1 when there is none. `events`
 * gets POLLIN, plus POLLOUT while replies wait for the socket. */
int  atspi_poll_fd(short *events);
/* `timeout` shortened to the next trailing change check (never below 0). */
f64  atspi_wait_limit(f64 timeout);
/* Once per loop turn (platform_poll_events): adopt a finished bootstrap,
 * read and answer what arrived, send what is owed. Never blocks. */
void atspi_service(void);
void atspi_shutdown(void);

#else /* no D-Bus: nothing to bridge to */

static inline void atspi_window_attach(AtspiWindow *aw, PlatformWindow *pw) { (void)aw; (void)pw; }
static inline void atspi_window_detach(AtspiWindow *aw) { (void)aw; }
static inline void atspi_window_set_provider(AtspiWindow *aw, PlatformA11yProviderFn fn, void *user) {
    (void)aw; (void)fn; (void)user;
}
static inline void atspi_window_set_title(AtspiWindow *aw, const char *title) { (void)aw; (void)title; }
static inline void atspi_window_set_active(AtspiWindow *aw, bool active) { (void)aw; (void)active; }
static inline void atspi_window_set_showing(AtspiWindow *aw, bool showing) { (void)aw; (void)showing; }
static inline void atspi_window_frame_presented(AtspiWindow *aw) { (void)aw; }
static inline void atspi_window_key(AtspiWindow *aw, u32 chars) { (void)aw; (void)chars; }
static inline int  atspi_poll_fd(short *events) { (void)events; return -1; }
static inline f64  atspi_wait_limit(f64 timeout) { return timeout; }
static inline void atspi_service(void) {}
static inline void atspi_shutdown(void) {}

#endif

/* The X11 backend's half (platform_linux.c), for the bridge only. */
bool linux_a11y_window_rect(PlatformWindow *w, i32 *x, i32 *y, i32 *width, i32 *height);
f32  linux_a11y_window_scale(PlatformWindow *w);
bool linux_a11y_window_mapped(PlatformWindow *w);
bool linux_a11y_window_has_focus(PlatformWindow *w);
bool linux_a11y_bus_property(char *out, usize cap);

#endif /* LIU_PLATFORM_ATSPI_BRIDGE_H */
