/*
 * Liu Browser — Linux: the page drawn in the pane by WebKitGTK.
 *
 * The engine is the system's, loaded when the first browser is made
 * (libwebkit2gtk-4.1, else 4.0). Nothing here links against it: every call
 * goes through a pointer resolved from the library and typed by its own
 * header. Without it a page is handed to the system's browser, as the stub in
 * browser_linux.c does.
 *
 * The page lives in a GtkPlug inside an X window of Liu's own (the socket), a
 * child of the pane's window that is exactly the part of the page Liu leaves
 * visible (the clip); the plug is the whole page, placed inside the socket,
 * so a clip reads as Liu's panel covering the page. GTK's main context is
 * driven from the loop's wait (platform/x11_embed.h).
 *
 * The rules are browser_macos.m's: the local-only policy, Liu's own start and
 * error pages, and the picker and the page's tint in a script world the page
 * cannot reach; a click is WebKitGTK's own user gesture. WebKitGTK says neither which frame a navigation
 * is for nor which frame a script message came from, so Liu's scripts go to
 * the top frame only, and a load that is not a click is judged twice: as the
 * subframe it may be when it starts, and as the main document when its
 * response arrives.
 */
#include "browser/browser.h"
#include "browser/browser_nav.h"
#include "browser/pick_element_js.h"
#include "browser/browser_chrome_js.h"
#include "browser/browser_tint_js.h"
#include "core/keybind.h"
#include "core/text_sanitize.h"
#include "platform/platform.h"
#include "platform/sfnt_family.h"
#include "platform/x11_embed.h"

#include "cJSON.h"

/* X11 typedefs `KeyCode`, which collides with Liu's own enum: X11's is
 * renamed across its headers (and GDK's, which include them). */
#define KeyCode XKeyCode
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <webkit2/webkit2.h>
#undef KeyCode

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIU_WORLD "LiuNativeBrowser"

/* ---------------------------------------------------------------------------
 * The library, loaded once
 * ------------------------------------------------------------------------- */

#define WK_FUNCS(X) \
    X(gdk_set_allowed_backends) X(gtk_init_check) X(gtk_plug_new) X(gtk_plug_get_id) \
    X(gtk_container_add) X(gtk_widget_show_all) X(gtk_widget_destroy) X(gtk_widget_grab_focus) \
    X(gtk_widget_get_scale_factor) X(gtk_settings_get_default) X(g_object_get) X(gtk_widget_queue_draw) \
    X(gtk_css_provider_new) X(gtk_css_provider_load_from_data) X(gtk_widget_get_style_context) \
    X(gtk_style_context_add_provider) \
    X(g_main_context_default) X(g_main_context_acquire) X(g_main_context_prepare) \
    X(g_main_context_query) X(g_main_context_check) X(g_main_context_dispatch) \
    X(g_main_context_iteration) X(g_signal_connect_data) X(g_signal_handlers_disconnect_matched) \
    X(g_object_ref) X(g_object_unref) X(g_free) X(g_idle_add) \
    X(webkit_user_content_manager_new) X(webkit_user_content_manager_add_script) \
    X(webkit_user_content_manager_register_script_message_handler_in_world) \
    X(webkit_user_script_new_for_world) X(webkit_user_script_unref) \
    X(webkit_web_view_new_with_user_content_manager) X(webkit_web_view_get_settings) \
    X(webkit_settings_set_javascript_can_open_windows_automatically) \
    X(webkit_web_view_load_uri) X(webkit_web_view_load_html) X(webkit_web_view_reload) \
    X(webkit_web_view_reload_bypass_cache) X(webkit_web_view_stop_loading) \
    X(webkit_web_view_go_back) X(webkit_web_view_go_forward) \
    X(webkit_web_view_can_go_back) X(webkit_web_view_can_go_forward) \
    X(webkit_web_view_get_title) X(webkit_web_view_get_uri) \
    X(webkit_web_view_evaluate_javascript) X(webkit_web_view_set_zoom_level) \
    X(webkit_web_view_get_context) \
    X(webkit_web_view_get_website_data_manager) X(webkit_website_data_manager_clear) \
    X(webkit_web_context_allow_tls_certificate_for_host) \
    X(webkit_policy_decision_use) X(webkit_policy_decision_ignore) \
    X(webkit_navigation_policy_decision_get_navigation_action) \
    X(webkit_navigation_action_get_request) X(webkit_navigation_action_get_navigation_type) \
    X(webkit_navigation_action_is_user_gesture) X(webkit_uri_request_get_uri) \
    X(webkit_response_policy_decision_get_request) X(webkit_response_policy_decision_get_response) \
    X(webkit_response_policy_decision_is_mime_type_supported) \
    X(webkit_response_policy_decision_is_main_frame_main_resource) \
    X(webkit_uri_response_get_http_headers) X(soup_message_headers_get_one) \
    X(webkit_permission_request_deny) X(webkit_javascript_result_get_js_value) \
    X(jsc_value_is_string) X(jsc_value_is_number) X(jsc_value_is_object) X(jsc_value_is_array) \
    X(jsc_value_to_string) X(jsc_value_to_double) X(jsc_value_object_get_property) \
    X(jsc_value_object_get_property_at_index) \
    X(gtk_disable_setlocale) X(webkit_context_menu_get_n_items) \
    X(webkit_context_menu_get_item_at_position) X(webkit_context_menu_item_get_stock_action) \
    X(webkit_context_menu_remove) X(webkit_download_cancel) X(webkit_download_get_web_view) \
    X(gdk_display_get_default) X(gdk_display_sync) X(gdk_x11_display_error_trap_push) \
    X(gdk_x11_display_error_trap_pop_ignored)

static struct {
#define WK_PTR(fn) __typeof__(&fn) fn;
    WK_FUNCS(WK_PTR)
#undef WK_PTR
    __typeof__(&webkit_web_view_get_theme_color) webkit_web_view_get_theme_color;   /* 2.42+, optional */
} W;

/* The X shape extension, from the library's own dependencies (GDK links it):
 * a device's rounded display cuts the page's corners. NULL: square corners. */
typedef void (*XShapeCombineMaskFn)(Display *, Window, int, int, int, Pixmap, int);
static XShapeCombineMaskFn g_shape_mask;
enum { WK_SHAPE_BOUNDING = 0, WK_SHAPE_SET = 0 };

/* Cairo, also a dependency: a device's status bar text in the system's sans
 * face (browser_native_mask). Any missing: the caller draws its own. */
#define WK_CAIRO(X) \
    X(cairo_image_surface_create) X(cairo_image_surface_get_data) X(cairo_image_surface_get_stride) \
    X(cairo_surface_flush) X(cairo_surface_destroy) X(cairo_create) X(cairo_destroy) \
    X(cairo_select_font_face) X(cairo_set_font_size) X(cairo_font_extents) X(cairo_text_extents) \
    X(cairo_set_source_rgba) X(cairo_move_to) X(cairo_show_text) X(cairo_status)
static struct {
#define WK_PTR(fn) __typeof__(&fn) fn;
    WK_CAIRO(WK_PTR)
#undef WK_PTR
    bool ok;
} C;

static int   g_wk_state;           /* 0 not tried, 1 ready, -1 not there */
static void *g_wk_lib;
static const char *g_wk_why = "";   /* why not, for the tab's status */

/* GDK installs an X error handler for the whole process and drops every error
 * on a display it did not open — Liu's own among them. Errors go back to the
 * handler of the display they happened on. */
static XErrorHandler   g_liu_xerr, g_gdk_xerr;
static XIOErrorHandler g_liu_xioerr, g_gdk_xioerr;

static int wk_x_error(Display *d, XErrorEvent *e) {
    if (d == (Display *)platform_x11_display() && g_liu_xerr) return g_liu_xerr(d, e);
    return g_gdk_xerr ? g_gdk_xerr(d, e) : 0;
}
static int wk_x_io_error(Display *d) {
    if (d == (Display *)platform_x11_display() && g_liu_xioerr) return g_liu_xioerr(d);
    return g_gdk_xioerr ? g_gdk_xioerr(d) : 0;
}

/* GTK's main context in the loop's wait: prepared and queried before it,
 * checked and dispatched after (x11_embed.h). */
#define WK_POLL_FDS 64
static gint    g_wk_prio;
static GPollFD g_wk_fds[WK_POLL_FDS];
static gint    g_wk_nfds;

static void holes_settle(void);

static i32 wk_prepare(void *ctx, struct pollfd *fds, i32 cap, i32 *timeout_ms) {
    (void)ctx;
    holes_settle();
    GMainContext *c = (W.g_main_context_default)();
    (W.g_main_context_prepare)(c, &g_wk_prio);
    gint timeout = -1;
    gint n = (W.g_main_context_query)(c, g_wk_prio, &timeout, g_wk_fds, WK_POLL_FDS);
    if (n > WK_POLL_FDS) n = WK_POLL_FDS;
    if (n > cap) n = cap;
    if (n < 0) n = 0;
    g_wk_nfds = n;
    for (gint i = 0; i < n; i++)
        fds[i] = (struct pollfd){ .fd = g_wk_fds[i].fd, .events = (short)g_wk_fds[i].events };
    *timeout_ms = timeout;
    return n;
}

static void wk_dispatch(void *ctx, const struct pollfd *fds, i32 n) {
    (void)ctx;
    GMainContext *c = (W.g_main_context_default)();
    for (gint i = 0; i < g_wk_nfds; i++)
        g_wk_fds[i].revents = (gushort)(i < n ? fds[i].revents : 0);
    if ((W.g_main_context_check)(c, g_wk_prio, g_wk_fds, g_wk_nfds))
        (W.g_main_context_dispatch)(c);
}

static void wk_pump(void *ctx) {
    (void)ctx;
    holes_settle();
    for (i32 i = 0; i < 64 && (W.g_main_context_iteration)(NULL, FALSE); i++) {}
}

static bool wk_load(void) {
    if (g_wk_state) return g_wk_state > 0;
    g_wk_state = -1;
    const char *off = getenv("LIU_BROWSER_EMBED");
    g_wk_why = "turned off by LIU_BROWSER_EMBED=0";
    if (off && off[0] == '0') return false;
    g_wk_why = "no X display";
    if (!platform_x11_display()) return false;
    static const char *const libs[] = { "libwebkit2gtk-4.1.so.0", "libwebkit2gtk-4.0.so.37" };
    for (usize i = 0; i < sizeof libs / sizeof libs[0] && !g_wk_lib; i++)
        g_wk_lib = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
    g_wk_why = "WebKitGTK is not installed";
    if (!g_wk_lib) return false;
#define WK_SYM(fn) *(void **)(&W.fn) = dlsym(g_wk_lib, #fn); if (!W.fn) goto missing;
    WK_FUNCS(WK_SYM)
#undef WK_SYM
    *(void **)(&W.webkit_web_view_get_theme_color) = dlsym(g_wk_lib, "webkit_web_view_get_theme_color");
    *(void **)(&g_shape_mask) = dlsym(g_wk_lib, "XShapeCombineMask");
    C.ok = true;
#define WK_SYM(fn) *(void **)(&C.fn) = dlsym(g_wk_lib, #fn); if (!C.fn) C.ok = false;
    WK_CAIRO(WK_SYM)
#undef WK_SYM

    /* X11 only: the page is a child of Liu's X window. */
    (W.gdk_set_allowed_backends)("x11");
    g_liu_xerr = XSetErrorHandler(NULL);
    XSetErrorHandler(g_liu_xerr);
    g_liu_xioerr = XSetIOErrorHandler(NULL);
    XSetIOErrorHandler(g_liu_xioerr);
    /* GTK's init sets the WHOLE locale from the environment unless told not
     * to: the numbers Liu writes with printf (config.json, sessions.json)
     * would take a decimal comma and dates a local month name. Liu sets
     * LC_CTYPE itself (platform_init). */
    (W.gtk_disable_setlocale)();
    g_wk_why = "GTK could not start";
    if (!(W.gtk_init_check)(NULL, NULL)) {
        XSetErrorHandler(g_liu_xerr);
        XSetIOErrorHandler(g_liu_xioerr);
        return false;
    }
    g_gdk_xerr = XSetErrorHandler(wk_x_error);
    g_gdk_xioerr = XSetIOErrorHandler(wk_x_io_error);
    (W.g_main_context_acquire)((W.g_main_context_default)());
    static const PlatformPollSource src = {
        .prepare = wk_prepare, .dispatch = wk_dispatch, .pump = wk_pump, .ctx = NULL };
    platform_x11_set_poll_source(&src);
    g_wk_state = 1;
    return true;
missing:
    /* An engine without the script worlds (before 2.40) is not used: the
     * picker and Liu's own pages would share the page's world. */
    memset(&W, 0, sizeof W);
    g_wk_why = "WebKitGTK is older than 2.40";
    return false;
}


/* ---------------------------------------------------------------------------
 * One browser
 * ------------------------------------------------------------------------- */

struct BrowserNative {
    PlatformWindow *window;
    const char *why;            /* why the page is not drawn here, when it is not */
    bool            embedded;          /* false: the system's browser gets the page */
    const BrowserAllowlist *allowlist;

    /* The page: Liu's socket window (the visible part), the plug in it (the
     * whole page) and the view. */
    Window          socket;
    GtkWidget      *plug;
    WebKitWebView  *view;
    WebKitUserContentManager *ucm;
    Window          plug_xid;
    bool            keyboard;          /* the page has the X focus */

    /* Geometry as the app gives it: window points, y up from the bottom. */
    i32  last_visible;                 /* -1 never told, 0 hidden, 1 shown */
    bool have_frame, have_clip;
    f32  fx, fy, fw, fh;
    f32  cx, cy, cw, ch;
    i32  vp_w, vp_h;                   /* a device's CSS viewport, 0 = fill */
    bool have_shape;
    BrowserScreenShape shape;
    bool placed;                       /* the page's last rect, for the probe */
    /* Liu's drawing seen through the page: what the frame on screen has under
     * it (holes), what the next one will (want), and what is cut now. */
    BrowserHole holes[BROWSER_HOLES_MAX], want[BROWSER_HOLES_MAX];
    BrowserHole cut[BROWSER_HOLES_MAX * BROWSER_HOLES_MAX];
    i32  nholes, nwant, ncut, shaped_holes;
    u64  shape_sig;                    /* the cut last made, to repaint what it uncovers */
    GtkCssProvider *backdrop;          /* the plug's own background: the theme's */
    bool holes_pending;
    f32  shaped_r;                     /* the radius cut now, points, and its corners */
    u32  shaped_corners;
    f32  px, py, pw, ph;
    f64  zoom;

    /* What poll copies out; `revision` moves with every change of it. */
    u64  revision, polled_rev;
    bool loading, can_back, can_fwd, pick_mode;
    bool has_pending_pick, has_pending_ask, has_pending_nav;
    char title[BROWSER_TITLE_CAP];
    char url[BROWSER_URL_CAP];
    char status[BROWSER_STATUS_CAP];
    u32  page_tint, theme_tint, page_bg;
    bool page_tint_known, theme_tint_known, page_bg_known;

    /* Liu's own pages: the start page and the connection error page. */
    bool start_pending, showing_start, error_pending, showing_error, internal_pending;
    bool internal_started;             /* Liu's own page has begun loading */
    u64  serial;                       /* which browser this is, for late callbacks */
    char err_url[BROWSER_URL_CAP];
    char err_reason[256], err_domain[128], err_desc[512], err_title[128], err_hint[256];
    i32  err_code;
    char theme_css[4096];
    char mono[256];
    char *recents_json;

    /* What belongs to the document on screen. */
    char  pick_token[40], chrome_token[40];
    char *pending_json;
    char  pending_nav[BROWSER_URL_CAP];

    /* The policy: a counter every main navigation moves, and the main URL
     * a click already judged (its response is not judged again). */
    u64  policy_seq;
    char judged_main[BROWSER_URL_CAP];
};

/* Natives alive now: a resolver thread's answer finds its browser here or
 * belongs to none. */
#define WK_LIVE_MAX 64
static BrowserNative *g_live[WK_LIVE_MAX];

static u64 g_serial;

static bool live(const BrowserNative *n) {
    if (!n) return false;
    for (i32 i = 0; i < WK_LIVE_MAX; i++) if (g_live[i] == n) return true;
    return false;
}

/* The same browser still: a later one can be given the freed one's address. */
static bool live_as(const BrowserNative *n, u64 serial) {
    return live(n) && n->serial == serial;
}

static void bump(BrowserNative *n) { n->revision++; }

static void set_str(BrowserNative *n, char *dst, usize cap, const char *v) {
    if (!v) v = "";
    if (!strncmp(dst, v, cap)) return;
    snprintf(dst, cap, "%s", v);
    bump(n);
}

static void set_bool(BrowserNative *n, bool *dst, bool v) {
    if (*dst == v) return;
    *dst = v;
    bump(n);
}

static void make_token(char *out, usize cap) {
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[16];
    FILE *f = fopen("/dev/urandom", "rb");
    bool ok = f && fread(raw, 1, sizeof raw, f) == sizeof raw;
    if (f) fclose(f);
    if (!ok) for (usize i = 0; i < sizeof raw; i++) raw[i] = (unsigned char)(rand() & 0xff);
    usize o = 0;
    for (usize i = 0; i < sizeof raw && o + 2 < cap; i++) {
        out[o++] = hex[raw[i] >> 4];
        out[o++] = hex[raw[i] & 15];
    }
    out[o] = '\0';
}

static void run_js(BrowserNative *n, const char *js) {
    if (!n->view || !js) return;
    (W.webkit_web_view_evaluate_javascript)(n->view, js, -1, LIU_WORLD, NULL, NULL, NULL, NULL);
}

/* What the document on screen was given, taken away when it goes. */
static void clear_document(BrowserNative *n) {
    n->pick_token[0] = '\0';
    n->chrome_token[0] = '\0';
    n->internal_pending = false;
    set_bool(n, &n->has_pending_ask, false);
    set_bool(n, &n->has_pending_nav, false);
    n->pending_nav[0] = '\0';
    free(n->pending_json);
    n->pending_json = NULL;
    set_bool(n, &n->has_pending_pick, false);
}

/* --- geometry ------------------------------------------------------------ */

static void xembed_send(Window to, long message, long detail) {
    Display *d = (Display *)platform_x11_display();
    if (!d || !to) return;
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = to;
    ev.xclient.message_type = XInternAtom(d, "_XEMBED", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = message;
    ev.xclient.data.l[2] = detail;
    XSendEvent(d, to, False, NoEventMask, &ev);
}
enum { XEMBED_WINDOW_ACTIVATE = 1, XEMBED_WINDOW_DEACTIVATE = 2, XEMBED_REQUEST_FOCUS = 3,
       XEMBED_FOCUS_IN = 4, XEMBED_FOCUS_OUT = 5, XEMBED_FOCUS_CURRENT = 0 };

static void give_keyboard(BrowserNative *n) {
    Display *d = (Display *)platform_x11_display();
    if (!d || !n->plug_xid || n->last_visible != 1) return;
    XSetInputFocus(d, n->plug_xid, RevertToParent, CurrentTime);
    xembed_send(n->plug_xid, XEMBED_WINDOW_ACTIVATE, 0);
    xembed_send(n->plug_xid, XEMBED_FOCUS_IN, XEMBED_FOCUS_CURRENT);
    XFlush(d);
    n->keyboard = true;
}

static void take_keyboard(BrowserNative *n) {
    Display *d = (Display *)platform_x11_display();
    if (!d || !n->plug_xid) return;
    if (n->keyboard) {
        xembed_send(n->plug_xid, XEMBED_FOCUS_OUT, 0);
        xembed_send(n->plug_xid, XEMBED_WINDOW_DEACTIVATE, 0);
    }
    Window focus = None;
    int revert = 0;
    XGetInputFocus(d, &focus, &revert);
    unsigned long liu = platform_x11_key_sink(n->window);
    if (liu && (focus == n->plug_xid || !n->keyboard))
        XSetInputFocus(d, (Window)liu, RevertToParent, CurrentTime);
    XFlush(d);
    n->keyboard = false;
}

/* A rounded rectangle in the mask, in the GC's current colour. */
static void mask_rrect(Display *d, Pixmap m, GC gc, i32 x, i32 y, i32 w, i32 h, i32 r) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { XFillRectangle(d, m, gc, x, y, (unsigned)w, (unsigned)h); return; }
    XFillRectangle(d, m, gc, x + r, y, (unsigned)(w - 2 * r), (unsigned)h);
    XFillRectangle(d, m, gc, x, y + r, (unsigned)w, (unsigned)(h - 2 * r));
    i32 ax[4] = { x, x + w - 2 * r, x + w - 2 * r, x }, ay[4] = { y, y, y + h - 2 * r, y + h - 2 * r };
    for (i32 i = 0; i < 4; i++)
        XFillArc(d, m, gc, ax[i], ay[i], (unsigned)(2 * r), (unsigned)(2 * r), 0, 360 * 64);
}

/* The socket's shape: a device's rounded display cuts the corners of the page
 * that the clip leaves whole (one the clip cuts through is the panel's edge,
 * not the display's), and Liu's drawing that stands over the page (the
 * notices) is seen through it. Socket pixels; the page is at (px, py) inside
 * it, the socket at (sx, sy) in the window, whose height is `view_h` points. */
static void apply_shape(BrowserNative *n, Display *d, i32 sx, i32 sy, i32 sw, i32 sh,
                        i32 px, i32 py, i32 pw, i32 ph, f32 view_h, f32 dpi) {
    if (!g_shape_mask) return;
    bool keep[4] = { px >= 0 && py >= 0, px + pw <= sw && py >= 0,
                     px + pw <= sw && py + ph <= sh, px >= 0 && py + ph <= sh };
    f32 radius = 0.0f;
    u32 corners = 0;
    for (i32 i = 0; i < 4; i++) {
        if (!n->have_shape || !keep[i] || !(n->shape.r[i] > 0.0f)) continue;
        corners |= 1u << i;
        if (n->shape.r[i] > radius) radius = n->shape.r[i];
    }
    f32 lim = fminf((f32)sw, (f32)sh) * 0.5f / dpi;
    if (radius > lim) radius = lim;
    if (radius <= 0.0f || !corners) { radius = 0.0f; corners = 0; }
    n->shaped_r = radius;
    n->shaped_corners = corners;
    i32 hole[BROWSER_HOLES_MAX * BROWSER_HOLES_MAX][5];
    i32 nh = 0;
    for (i32 i = 0; i < n->ncut && nh < BROWSER_HOLES_MAX * BROWSER_HOLES_MAX; i++) {
        const BrowserHole *h = &n->cut[i];
        i32 hx = (i32)lroundf(h->x * dpi) - sx, hy = (i32)lroundf((view_h - (h->y + h->h)) * dpi) - sy;
        i32 hw = (i32)lroundf(h->w * dpi), hh = (i32)lroundf(h->h * dpi);
        if (hx >= sw || hy >= sh || hx + hw <= 0 || hy + hh <= 0 || hw <= 0 || hh <= 0) continue;
        hole[nh][0] = hx; hole[nh][1] = hy; hole[nh][2] = hw; hole[nh][3] = hh;
        hole[nh][4] = (i32)lroundf(h->r * dpi);
        nh++;
    }
    n->shaped_holes = nh;
    /* What a cut gives back is the page's again: it is drawn at once, not when
     * the page next paints by itself. */
    u64 sig = 1469598103934665603ull ^ corners ^ ((u64)lroundf(radius * 64.0f) << 8);
    for (i32 i = 0; i < nh; i++)
        for (i32 k = 0; k < 5; k++) sig = (sig ^ (u64)(u32)hole[i][k]) * 1099511628211ull;
    if (sig != n->shape_sig) {
        n->shape_sig = sig;
        (W.gtk_widget_queue_draw)((GtkWidget *)n->view);
    }
    if (!corners && !nh) { g_shape_mask(d, n->socket, WK_SHAPE_BOUNDING, 0, 0, None, WK_SHAPE_SET); return; }
    Pixmap mask = XCreatePixmap(d, n->socket, (unsigned)sw, (unsigned)sh, 1);
    if (!mask) return;
    GC gc = XCreateGC(d, mask, 0, NULL);
    XSetForeground(d, gc, 0);
    XFillRectangle(d, mask, gc, 0, 0, (unsigned)sw, (unsigned)sh);
    XSetForeground(d, gc, 1);
    if (corners) {
        i32 r = (i32)lroundf(radius * dpi);
        /* The page's rect rounded, then its uncut corners squared again. */
        mask_rrect(d, mask, gc, px, py, pw, ph, r);
        i32 cx[4] = { px, px + pw - r, px + pw - r, px };
        i32 cy[4] = { py, py, py + ph - r, py + ph - r };
        for (i32 i = 0; i < 4; i++)
            if (!(corners & (1u << i))) XFillRectangle(d, mask, gc, cx[i], cy[i], (unsigned)r, (unsigned)r);
    } else {
        XFillRectangle(d, mask, gc, 0, 0, (unsigned)sw, (unsigned)sh);
    }
    XSetForeground(d, gc, 0);
    for (i32 i = 0; i < nh; i++) mask_rrect(d, mask, gc, hole[i][0], hole[i][1], hole[i][2], hole[i][3], hole[i][4]);
    XFreeGC(d, gc);
    g_shape_mask(d, n->socket, WK_SHAPE_BOUNDING, 0, 0, mask, WK_SHAPE_SET);
    XFreePixmap(d, mask);
}

/* The app's rects, window points up from the bottom, as X pixels down from the
 * top: the socket is the visible part, the plug the whole page inside it. */
static void apply_geometry(BrowserNative *n) {
    Display *d = (Display *)platform_x11_display();
    if (!n->embedded || !d || !n->socket || !n->have_frame) return;
    f32 dpi = platform_window_get_dpi_scale(n->window);
    if (!(dpi > 0.0f)) dpi = 1.0f;
    i32 ww = 0, wh = 0;
    platform_window_get_size(n->window, &ww, &wh);
    f32 view_h = (f32)wh / dpi;
    f32 fx = n->fx, fy = view_h - (n->fy + n->fh), fw = n->fw, fh = n->fh;
    n->placed = true;
    n->px = n->fx; n->py = n->fy; n->pw = n->fw; n->ph = n->fh;
    /* The page in pixels. A device's viewport: CSS pixels per pixel from the
     * width (a hair under, so the page never counts one short), and as many
     * pixels down as its CSS height then takes, rounded up. */
    i32 px0 = (i32)lroundf(fx * dpi), py0 = (i32)lroundf(fy * dpi);
    i32 pw = (i32)lroundf(fw * dpi), ph = (i32)lroundf(fh * dpi);
    if (pw < 1) pw = 1;
    bool device = n->vp_w > 0 && n->vp_h > 0;
    f64 k = device ? (f64)pw / ((f64)n->vp_w + 0.01) : (f64)dpi;
    if (device) ph = (i32)ceil((f64)n->vp_h * k - 1e-6);
    if (ph < 1) ph = 1;
    /* The socket is the part of the page the clip leaves. */
    i32 sx = px0, sy = py0, sx1 = px0 + pw, sy1 = py0 + ph;
    if (n->have_clip) {
        f32 ox = n->cx, oy = view_h - (n->cy + n->ch);
        sx = (i32)fmaxf((f32)sx, lroundf(ox * dpi));
        sy = (i32)fmaxf((f32)sy, lroundf(oy * dpi));
        sx1 = (i32)fminf((f32)sx1, lroundf((ox + n->cw) * dpi));
        sy1 = (i32)fminf((f32)sy1, lroundf((oy + n->ch) * dpi));
    }
    i32 sw = sx1 - sx, sh = sy1 - sy;
    if (n->last_visible != 1 || sw < 1 || sh < 1) {
        if (n->keyboard) take_keyboard(n);
        XUnmapWindow(d, n->socket);
        XFlush(d);
        return;
    }
    i32 pxp = px0 - sx, pyp = py0 - sy;
    XMoveResizeWindow(d, n->socket, sx, sy, (unsigned)sw, (unsigned)sh);
    if (n->plug_xid) {
        XMoveResizeWindow(d, n->plug_xid, pxp, pyp, (unsigned)pw, (unsigned)ph);
        XMapWindow(d, n->plug_xid);
    }
    apply_shape(n, d, sx, sy, sw, sh, pxp, pyp, pw, ph, view_h, dpi);
    XMapRaised(d, n->socket);
    XFlush(d);
    /* A CSS pixel is a point, as in the macOS view; a device's viewport is
     * its CSS width scaled into the page's width. GTK scales by the desktop's
     * factor itself (from the display's Xft.dpi): the zoom is what is left. */
    i32 gscale = (W.gtk_widget_get_scale_factor)((GtkWidget *)n->view);
    if (gscale < 1) gscale = 1;
    /* ...and WebKit scales its page by the font DPI over 96 on top. */
    gint xft = 0;
    GtkSettings *gs = (W.gtk_settings_get_default)();
    if (gs) (W.g_object_get)(gs, "gtk-xft-dpi", &xft, NULL);
    f64 font_scale = xft > 0 ? ((f64)xft / 1024.0) / 96.0 : 1.0;
    if (!(font_scale > 0.25 && font_scale < 8.0)) font_scale = 1.0;
    f64 zoom = k / ((f64)gscale * font_scale);
    if (!(zoom > 0.05)) zoom = 1.0;
    if (fabs(zoom - n->zoom) > 1e-4) {
        n->zoom = zoom;
        (W.webkit_web_view_set_zoom_level)(n->view, zoom);
    }
}

/* --- the socket hears from its plug ------------------------------------- */

static bool wk_child_event(void *xevent, void *ctx) {
    (void)ctx;
    XEvent *e = xevent;
    if (e->type != ClientMessage) return false;
    for (i32 i = 0; i < WK_LIVE_MAX; i++) {
        BrowserNative *n = g_live[i];
        if (!n || n->socket != e->xclient.window) continue;
        Display *d = (Display *)platform_x11_display();
        if (d && e->xclient.message_type == XInternAtom(d, "_XEMBED", False) &&
            e->xclient.data.l[1] == XEMBED_REQUEST_FOCUS)
            give_keyboard(n);
        return true;
    }
    return false;
}

/* --- Liu's own pages ------------------------------------------------------ */

/* The `:root` declarations an internal page gets before the first theme push
 * (THEME_Liu's colours); the real ones come from the terminal theme. */
#define WK_FALLBACK_THEME \
    "--bg:#121212;--surface:#1a1a1a;--border:#2e2e2e;" \
    "--fg:#e6e6e6;--fg-strong:#cdcdcd;--dim:#8a8a8a;--faint:#6f6f6f;" \
    "--fg-rgb:230,230,230;--accent:#c273ef;--accent-rgb:194,115,239;color-scheme:dark"

#define WK_PAGE_BASE_CSS \
    "html,body{height:100%;margin:0}" \
    "body{background:var(--bg);color:var(--fg);" \
    "font:400 14px/1.5 system-ui,sans-serif;" \
    "display:flex;align-items:center;justify-content:center;" \
    "-webkit-user-select:none;user-select:none}" \
    "::selection{background:rgba(var(--accent-rgb),.30)}"

static const char WK_ERROR_PAGE[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; img-src data:; form-action 'none'; base-uri 'none'\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>__HOSTPORT__</title><style>"
    ":root{__THEME__}"
    WK_PAGE_BASE_CSS
    ".wrap{text-align:center;max-width:540px;padding:24px}"
    "h1{margin:0 0 12px;font-size:30px;font-weight:600;letter-spacing:-.01em;color:var(--fg-strong)}"
    "p.sub{margin:0 0 28px;font-size:15px;color:var(--faint);word-break:break-word}"
    ".btns{display:flex;gap:12px;justify-content:center;flex-wrap:wrap}"
    "button{font:inherit;font-size:14px;color:var(--fg-strong);background:transparent;"
    "border:1px solid var(--border);border-radius:9px;padding:8px 18px;cursor:pointer;"
    "transition:border-color .12s,color .12s,background .12s}"
    "button:hover{border-color:var(--accent);color:var(--fg);background:rgba(var(--accent-rgb),.10)}"
    "button:disabled{opacity:.75;cursor:default;border-color:var(--accent);color:var(--fg)}"
    "#details{display:none;margin:24px auto 0;max-width:520px;text-align:left;"
    "background:var(--surface);border:1px solid var(--border);border-radius:10px;"
    "padding:14px 16px;font-family:var(--mono);font-size:12px;line-height:1.6;color:var(--dim);"
    "white-space:pre-wrap;word-break:break-word}"
    "#details b{color:var(--fg-strong);font-weight:600}"
    "#details .hint{color:var(--faint)}"
    "@media (max-width:420px){h1{font-size:22px}p.sub{font-size:13px}}"
    "</style></head><body><div class=\"wrap\">"
    "<h1>__TITLE__</h1>"
    "<p class=\"sub\">__HOSTPORT__ __REASON__. (__CODE__)</p>"
    "<div class=\"btns\"><button id=\"ask\">Ask Agent</button>"
    "<button id=\"show\">Show Details</button></div>"
    "<div id=\"details\"><b>URL</b>      __URL__\n<b>Error</b>    __REASON__ (__CODE__)\n"
    "<b>Domain</b>   __DOMAIN__\n<b>Message</b>  __DESC__\n\n"
    "<span class=\"hint\">__HINT__</span></div>"
    "</div></body></html>";

#define WK_GLOBE_SVG \
    "<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" " \
    "stroke=\"currentColor\" stroke-width=\"1.6\"><circle cx=\"12\" cy=\"12\" r=\"9\"/>" \
    "<path d=\"M3 12h18\"/><ellipse cx=\"12\" cy=\"12\" rx=\"4\" ry=\"9\"/></svg>"

static const char WK_START_SHELL[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; img-src data:; form-action 'none'; base-uri 'none'\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Liu Browser</title><style>"
    ":root{__THEME__}"
    WK_PAGE_BASE_CSS
    ".card{width:70%;max-width:900px;box-sizing:border-box;border:1px solid var(--border);"
    "border-radius:16px;padding:18px 22px;background:var(--surface)}"
    ".card h2{margin:0 0 6px;font-size:15px;font-weight:500;color:var(--dim)}"
    ".row{display:flex;align-items:center;gap:12px;padding:10px 8px;border-radius:9px;"
    "cursor:pointer;color:var(--fg);text-decoration:none;transition:background .12s}"
    ".row:hover{background:rgba(var(--accent-rgb),.10)}"
    ".row svg{flex:0 0 auto;color:var(--faint)}"
    ".row:hover svg{color:var(--accent)}"
    ".row span{font-size:17px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".intro{text-align:center;max-width:520px;padding:24px}"
    ".intro .globe{color:var(--accent);opacity:.9;margin-bottom:16px}"
    ".intro h1{margin:0 0 10px;font-size:26px;font-weight:600;color:var(--fg)}"
    ".intro p{margin:0 0 8px;font-size:14px;color:var(--dim);line-height:1.6}"
    ".intro p b{color:var(--fg-strong);font-weight:600}"
    ".intro .hint{margin-top:18px;font-size:13px;color:var(--faint)}"
    ".intro code{font-family:var(--mono);color:var(--accent);"
    "background:rgba(var(--accent-rgb),.10);padding:1px 6px;border-radius:5px}"
    "</style></head><body>__BODY__</body></html>";

static const char WK_INTRO_BODY[] =
    "<div class=\"intro\"><div class=\"globe\">"
    "<svg width=\"56\" height=\"56\" viewBox=\"0 0 24 24\" fill=\"none\" "
    "stroke=\"currentColor\" stroke-width=\"1.3\"><circle cx=\"12\" cy=\"12\" r=\"9\"/>"
    "<path d=\"M3 12h18\"/><ellipse cx=\"12\" cy=\"12\" rx=\"4\" ry=\"9\"/></svg></div>"
    "<h1>Liu Browser</h1>"
    "<p>Preview your local dev servers right inside Liu \xe2\x80\x94 no switching to a "
    "separate app while you build.</p>"
    "<p>Then use <b>Select Element</b> to hand any part of the page to your AI "
    "agent as context for the change you want.</p>"
    "<div class=\"hint\">Type a URL above, e.g. <code>localhost:5173</code>, to get started.</div>"
    "</div>";

/* A growing string: the internal pages are built from these. */
typedef struct { char *p; usize n, cap; bool bad; } Buf;

static void buf_add(Buf *b, const char *s, usize len) {
    if (b->bad || !s) return;
    if (b->n + len + 1 > b->cap) {
        usize cap = b->cap ? b->cap : 4096;
        while (cap < b->n + len + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) { b->bad = true; return; }
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = '\0';
}
static void buf_str(Buf *b, const char *s) { if (s) buf_add(b, s, strlen(s)); }
static void buf_html(Buf *b, const char *s) {
    for (; s && *s; s++) {
        switch (*s) {
        case '&': buf_str(b, "&amp;"); break;
        case '<': buf_str(b, "&lt;"); break;
        case '>': buf_str(b, "&gt;"); break;
        case '"': buf_str(b, "&quot;"); break;
        default:  buf_add(b, s, 1); break;
        }
    }
}

/* The page's theme: the pushed `:root` declarations (or the fallback) and the
 * monospace family, kept inside its CSS string. */
static void buf_theme(Buf *b, const BrowserNative *n) {
    buf_str(b, n->theme_css[0] ? n->theme_css : WK_FALLBACK_THEME);
    buf_str(b, ";--mono:");
    if (n->mono[0]) {
        buf_str(b, "\"");
        for (const char *s = n->mono; *s; s++) {
            if (*s == '\\' || *s == '"') { buf_str(b, "\\"); buf_add(b, s, 1); }
            else if (*s == '<') buf_str(b, "\\3c ");
            else if (*s == '\n' || *s == '\r') buf_str(b, " ");
            else buf_add(b, s, 1);
        }
        buf_str(b, "\",");
    }
    buf_str(b, "ui-monospace,monospace");
}

/* `tpl` with each __TOKEN__ replaced: the theme, or a value HTML-escaped. */
static char *fill(const BrowserNative *n, const char *tpl, const char *const *keys,
                  const char *const *vals, usize count, bool raw_last) {
    Buf b = {0};
    for (const char *s = tpl; *s; ) {
        if (s[0] == '_' && s[1] == '_') {
            if (!strncmp(s, "__THEME__", 9)) { buf_theme(&b, n); s += 9; continue; }
            bool done = false;
            for (usize i = 0; i < count && !done; i++) {
                usize kl = strlen(keys[i]);
                if (!strncmp(s, keys[i], kl)) {
                    if (raw_last && i == count - 1) buf_str(&b, vals[i]);
                    else buf_html(&b, vals[i]);
                    s += kl;
                    done = true;
                }
            }
            if (done) continue;
        }
        buf_add(&b, s, 1);
        s++;
    }
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

static void load_internal(BrowserNative *n, const char *html) {
    clear_document(n);
    ++n->policy_seq;
    n->internal_pending = true;
    n->internal_started = false;
    (W.webkit_web_view_load_html)(n->view, html, NULL);
}

static void render_error_page(BrowserNative *n) {
    char hostport[512] = "";
    char host[256];
    snprintf(hostport, sizeof hostport, "%s", n->err_url);
    if (n->err_url[0] && browser_nav_host(n->err_url, host, sizeof host)) {
        const char *after = strstr(n->err_url, "://");
        const char *auth = after ? after + 3 : n->err_url;
        const char *end = auth + strcspn(auth, "/?#");
        const char *colon = NULL;
        for (const char *p = auth; p < end; p++) if (*p == ':' && (p == auth || p[-1] != ']')) colon = p;
        if (host[0] != '[' && colon && colon + 1 < end)
            snprintf(hostport, sizeof hostport, "%s:%.*s", host, (int)(end - colon - 1), colon + 1);
        else snprintf(hostport, sizeof hostport, "%s", host);
    }
    char code[24];
    snprintf(code, sizeof code, "%d", n->err_code);
    const char *keys[] = { "__TITLE__", "__HINT__", "__HOSTPORT__", "__REASON__", "__CODE__",
                           "__URL__", "__DOMAIN__", "__DESC__" };
    const char *vals[] = {
        n->err_title[0] ? n->err_title : "Can't connect to server",
        n->err_hint[0] ? n->err_hint
            : "Tip: make sure your dev server is running (e.g. npm run dev), then reload this page.",
        hostport, n->err_reason, code, n->err_url, n->err_domain, n->err_desc };
    char *html = fill(n, WK_ERROR_PAGE, keys, vals, 8, false);
    if (!html) return;
    n->showing_error = false;
    n->showing_start = false;
    n->start_pending = false;
    n->error_pending = true;
    load_internal(n, html);
    free(html);
    char st[BROWSER_STATUS_CAP];
    snprintf(st, sizeof st, "%s %s", hostport, n->err_reason);
    set_str(n, n->status, sizeof n->status, st);
}

static void show_start_page(BrowserNative *n) {
    Buf rows = {0};
    cJSON *arr = n->recents_json ? cJSON_Parse(n->recents_json) : NULL;
    if (cJSON_IsArray(arr)) {
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0]) continue;
            const char *u = it->valuestring;
            const char *shown = !strncmp(u, "https://", 8) ? u + 8 : !strncmp(u, "http://", 7) ? u + 7 : u;
            usize sl = strlen(shown);
            char label[BROWSER_URL_CAP];
            snprintf(label, sizeof label, "%.*s", (int)(sl > 1 && shown[sl - 1] == '/' ? sl - 1 : sl), shown);
            buf_str(&rows, "<a class=\"row\" data-url=\"");
            buf_html(&rows, u);
            buf_str(&rows, "\">" WK_GLOBE_SVG "<span>");
            buf_html(&rows, label);
            buf_str(&rows, "</span></a>");
        }
    }
    cJSON_Delete(arr);
    Buf body = {0};
    if (!rows.n) buf_str(&body, WK_INTRO_BODY);
    else {
        buf_str(&body, "<div class=\"card\"><h2>Recents</h2>");
        buf_add(&body, rows.p, rows.n);
        buf_str(&body, "</div>");
    }
    free(rows.p);
    if (body.bad) { free(body.p); return; }
    const char *keys[] = { "__BODY__" };
    const char *vals[] = { body.p ? body.p : "" };
    char *html = fill(n, WK_START_SHELL, keys, vals, 1, true);
    free(body.p);
    if (!html) return;
    n->start_pending = true;
    n->error_pending = false;
    n->showing_start = false;
    n->showing_error = false;
    load_internal(n, html);
    free(html);
    set_str(n, n->status, sizeof n->status, "");
    set_str(n, n->url, sizeof n->url, "");
    set_str(n, n->title, sizeof n->title, "");
}

static void install_chrome(BrowserNative *n) {
    make_token(n->chrome_token, sizeof n->chrome_token);
    usize len = strlen(BROWSER_CHROME_JS) + 64;
    char *js = malloc(len);
    if (!js) return;
    snprintf(js, len, "(%s)('%s');", BROWSER_CHROME_JS, n->chrome_token);
    run_js(n, js);
    free(js);
}

/* Refused, it is OFF: Liu's start and error pages are no page to pick from. */
static bool set_pick_mode(BrowserNative *n, bool on) {
    bool refused = on && (n->showing_start || n->showing_error || n->start_pending || n->error_pending);
    if (refused) on = false;
    set_bool(n, &n->pick_mode, on);
    if (on) make_token(n->pick_token, sizeof n->pick_token);
    else n->pick_token[0] = '\0';
    char js[160];
    if (on) snprintf(js, sizeof js, "window.__liuPickSetMode && window.__liuPickSetMode(true, '%s');", n->pick_token);
    else snprintf(js, sizeof js, "window.__liuPickSetMode && window.__liuPickSetMode(false);");
    run_js(n, js);
    if (on) set_str(n, n->status, sizeof n->status, "Select an element\xe2\x80\xa6");
    else if (!strcmp(n->status, "Select an element\xe2\x80\xa6")) set_str(n, n->status, sizeof n->status, "");
    return !refused;
}

/* --- script messages (top frame, Liu's world) ------------------------------ */

/* A string property of a message, clipped to `cap` bytes on a character; NULL
 * when it is absent or not a string. g_free'd by the caller via free_js. */
static char *js_str(JSCValue *obj, const char *key, usize cap) {
    JSCValue *v = (W.jsc_value_object_get_property)(obj, key);
    if (!v) return NULL;
    char *out = NULL;
    if ((W.jsc_value_is_string)(v)) {
        char *s = (W.jsc_value_to_string)(v);
        if (s) {
            usize len = strlen(s);
            if (len > cap) {
                len = cap;
                while (len && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
            }
            out = malloc(len + 1);
            if (out) { memcpy(out, s, len); out[len] = '\0'; }
            (W.g_free)(s);
        }
    }
    (W.g_object_unref)(v);
    return out;
}

static bool js_num(JSCValue *obj, const char *key, f64 *out) {
    JSCValue *v = (W.jsc_value_object_get_property)(obj, key);
    if (!v) return false;
    bool ok = (W.jsc_value_is_number)(v);
    if (ok) *out = (W.jsc_value_to_double)(v);
    (W.g_object_unref)(v);
    return ok && isfinite(*out);
}

static bool on_internal_doc(const BrowserNative *n) {
    const char *u = (W.webkit_web_view_get_uri)(n->view);
    return u && !strcmp(u, "about:blank");
}

static void msg_tint(WebKitUserContentManager *m, WebKitJavascriptResult *r, gpointer data) {
    (void)m;
    BrowserNative *n = data;
    JSCValue *v = (W.webkit_javascript_result_get_js_value)(r);
    f64 rgb;
    if (!v || !(W.jsc_value_is_object)(v) || !js_num(v, "rgb", &rgb) || rgb < 0 || rgb > 0xFFFFFF) return;
    n->page_bg = (u32)rgb;
    n->page_bg_known = true;
    u32 tint = n->theme_tint_known ? n->theme_tint : n->page_bg;
    if (n->page_tint != tint || !n->page_tint_known) { n->page_tint = tint; n->page_tint_known = true; bump(n); }
}

static void msg_browser(WebKitUserContentManager *m, WebKitJavascriptResult *r, gpointer data) {
    (void)m;
    BrowserNative *n = data;
    JSCValue *v = (W.webkit_javascript_result_get_js_value)(r);
    if (!v || !(W.jsc_value_is_object)(v) || !n->chrome_token[0] || !on_internal_doc(n)) return;
    char *token = js_str(v, "requestToken", 64);
    char *type = js_str(v, "type", 32);
    if (token && type && !strcmp(token, n->chrome_token)) {
        if (!strcmp(type, "askAgent") && n->showing_error) {
            set_bool(n, &n->has_pending_ask, true);
        } else if (!strcmp(type, "navigate") && n->showing_start) {
            char *u = js_str(v, "url", BROWSER_URL_CAP);
            if (u && u[0] && strlen(u) < BROWSER_URL_CAP - 1 &&
                browser_nav_is_allowed_with_list(u, n->allowlist)) {
                snprintf(n->pending_nav, sizeof n->pending_nav, "%s", u);
                set_bool(n, &n->has_pending_nav, true);
            }
            free(u);
        }
    }
    free(token);
    free(type);
}

/* The picker's message, reduced to the fields Liu reads, each clipped: the
 * work is bounded by the schema, not by what the page made the object. */
static void msg_pick(WebKitUserContentManager *m, WebKitJavascriptResult *r, gpointer data) {
    (void)m;
    BrowserNative *n = data;
    if (!n->pick_mode || !n->pick_token[0]) return;
    JSCValue *v = (W.webkit_javascript_result_get_js_value)(r);
    if (!v || !(W.jsc_value_is_object)(v)) return;
    char *token = js_str(v, "requestToken", 64);
    char *type = js_str(v, "type", 32);
    bool mine = token && type && !strcmp(token, n->pick_token);
    free(token);
    if (!mine) { free(type); return; }
    if (!strcmp(type, "cancel")) { free(type); set_pick_mode(n, false); return; }
    bool pick = !strcmp(type, "pick");
    free(type);
    if (!pick) return;

    static const struct { const char *key; usize cap; } fields[] = {
        { "type", 32 }, { "tag", 63 }, { "id", 127 }, { "classes", 255 },
        { "cssSelector", BROWSER_SEL_CAP - 1 }, { "cssPath", BROWSER_PATH_CAP - 1 },
        { "text", BROWSER_TEXT_CAP - 1 }, { "ariaLabel", 255 }, { "role", 63 },
        { "dataTestid", 127 }, { "html", BROWSER_HTML_CAP - 1 },
    };
    cJSON *out = cJSON_CreateObject();
    if (!out) return;
    for (usize i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        char *s = js_str(v, fields[i].key, fields[i].cap);
        if (s) cJSON_AddStringToObject(out, fields[i].key, s);
        free(s);
    }
    /* The page's address is the view's, not what the message says. */
    const char *page = (W.webkit_web_view_get_uri)(n->view);
    if (page) {
        char u[BROWSER_URL_CAP];
        snprintf(u, sizeof u, "%s", page);
        cJSON_AddStringToObject(out, "url", u);
    }
    char *full = js_str(v, "html", BROWSER_HTML_CAP);
    if (full && strlen(full) > BROWSER_HTML_CAP - 1) cJSON_AddTrueToObject(out, "htmlTruncated");
    free(full);
    JSCValue *rect = (W.jsc_value_object_get_property)(v, "rect");
    if (rect && (W.jsc_value_is_object)(rect)) {
        cJSON *rc = cJSON_CreateObject();
        static const char *const rk[] = { "x", "y", "w", "h" };
        for (i32 i = 0; rc && i < 4; i++) {
            f64 d;
            if (js_num(rect, rk[i], &d)) cJSON_AddNumberToObject(rc, rk[i], d);
        }
        if (rc && rc->child) cJSON_AddItemToObject(out, "rect", rc);
        else cJSON_Delete(rc);
    }
    if (rect) (W.g_object_unref)(rect);
    JSCValue *lim = (W.jsc_value_object_get_property)(v, "limitations");
    if (lim && (W.jsc_value_is_array)(lim)) {
        cJSON *la = cJSON_CreateArray();
        for (u32 i = 0; la && i < 8; i++) {
            JSCValue *e = (W.jsc_value_object_get_property_at_index)(lim, i);
            if (!e) break;
            if ((W.jsc_value_is_string)(e)) {
                char *s = (W.jsc_value_to_string)(e);
                if (s) {
                    char clip[129];
                    usize len = strlen(s);
                    if (len > 128) { len = 128; while (len && ((unsigned char)s[len] & 0xC0) == 0x80) len--; }
                    memcpy(clip, s, len); clip[len] = '\0';
                    cJSON_AddItemToArray(la, cJSON_CreateString(clip));
                    (W.g_free)(s);
                }
            }
            (W.g_object_unref)(e);
        }
        if (la && la->child) cJSON_AddItemToObject(out, "limitations", la);
        else cJSON_Delete(la);
    }
    if (lim) (W.g_object_unref)(lim);
    char *json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!json) return;
    if (strlen(json) > 64u * 1024u) { cJSON_free(json); return; }
    free(n->pending_json);
    n->pending_json = strdup(json);
    cJSON_free(json);
    if (!n->pending_json) return;
    set_bool(n, &n->has_pending_pick, true);
    set_bool(n, &n->pick_mode, false);
    n->pick_token[0] = '\0';
    set_str(n, n->status, sizeof n->status, "Element picked");
}

/* --- the policy ------------------------------------------------------------- */

static void refuse_url(BrowserNative *n, const char *url, BrowserNavClass verdict) {
    bool web = url && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8));
    if (!web) return;
    set_bool(n, &n->loading, false);
    snprintf(n->err_url, sizeof n->err_url, "%s", url);
    n->err_code = -20;
    snprintf(n->err_domain, sizeof n->err_domain, "LiuBrowserPolicy");
    if (verdict == BROWSER_NAV_PUBLIC) {
        snprintf(n->err_title, sizeof n->err_title, "Site not allowed");
        snprintf(n->err_reason, sizeof n->err_reason, "is on the public internet");
        snprintf(n->err_desc, sizeof n->err_desc,
                 "Add this site in Settings > Browser > Allowed sites to open it in Liu.");
    } else {
        snprintf(n->err_title, sizeof n->err_title, "Can't reach local address");
        snprintf(n->err_reason, sizeof n->err_reason, "did not resolve to a local address");
        snprintf(n->err_desc, sizeof n->err_desc, "The name could not be resolved.");
    }
    snprintf(n->err_hint, sizeof n->err_hint,
             "Localhost, private networks, and Tailscale work automatically. "
             "Other sites need an entry in Settings > Browser > Allowed sites.");
    render_error_page(n);
}

/* What a decision does, once it is known: used, ignored and shown, or
 * ignored and handed to the system's browser. `main` = the document's own. */
static void apply_action(BrowserNative *n, WebKitPolicyDecision *d, BrowserNavAction act,
                         const char *url, BrowserNavClass cls, bool main) {
    if (act == BROWSER_NAV_ALLOW) { (W.webkit_policy_decision_use)(d); return; }
    (W.webkit_policy_decision_ignore)(d);
    if (main) {
        set_bool(n, &n->loading, false);
        set_str(n, n->status, sizeof n->status, act == BROWSER_NAV_HANDOFF ? "" : "Navigation blocked");
    }
    if (act == BROWSER_NAV_HANDOFF) { platform_open_url(url); return; }
    if (main) refuse_url(n, url, cls);
}

/* A name: local only when every address it resolves to is. getaddrinfo can
 * block, so it runs on a thread and the decision waits for it. */
typedef struct {
    BrowserNative *n;
    u64  serial, seq;
    WebKitPolicyDecision *d;
    char url[BROWSER_URL_CAP];
    char host[256];
    bool click, response;
    BrowserNavClass verdict;
} DnsJob;

static gboolean dns_done(gpointer data) {
    DnsJob *j = data;
    BrowserNative *n = j->n;
    if (!live_as(n, j->serial) || j->seq != n->policy_seq) {
        (W.webkit_policy_decision_ignore)(j->d);
    } else {
        BrowserNavAction act = browser_nav_decide_with_list(j->url, j->verdict, true, j->click, n->allowlist);
        if (act == BROWSER_NAV_RESOLVE) act = BROWSER_NAV_REFUSE;
        if (act == BROWSER_NAV_ALLOW) snprintf(n->judged_main, sizeof n->judged_main, "%s", j->url);
        apply_action(n, j->d, act, j->url, j->verdict, true);
    }
    (W.g_object_unref)(j->d);
    free(j);
    return FALSE;
}

static void *dns_thread(void *data) {
    DnsJob *j = data;
    j->verdict = browser_nav_resolve_host(j->host);
    (W.g_idle_add)(dns_done, j);
    return NULL;
}

static bool resolve_later(BrowserNative *n, WebKitPolicyDecision *d, const char *url, bool click, bool response) {
    DnsJob *j = calloc(1, sizeof *j);
    if (!j || !browser_nav_host(url, j->host, sizeof j->host)) { free(j); return false; }
    j->n = n;
    j->serial = n->serial;
    j->seq = n->policy_seq;
    j->d = (W.g_object_ref)(d);
    j->click = click;
    j->response = response;
    snprintf(j->url, sizeof j->url, "%s", url);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    bool ok = pthread_create(&t, &a, dns_thread, j) == 0;
    pthread_attr_destroy(&a);
    if (!ok) { (W.g_object_unref)(j->d); free(j); return false; }
    return true;
}

static gboolean on_policy_navigation(BrowserNative *n, WebKitPolicyDecision *d, bool new_window) {
    WebKitNavigationAction *a =
        (W.webkit_navigation_policy_decision_get_navigation_action)((WebKitNavigationPolicyDecision *)d);
    WebKitURIRequest *req = a ? (W.webkit_navigation_action_get_request)(a) : NULL;
    const char *url = req ? (W.webkit_uri_request_get_uri)(req) : NULL;
    if (!url || !url[0] || strlen(url) >= BROWSER_URL_CAP) { (W.webkit_policy_decision_ignore)(d); return TRUE; }
    bool click = (W.webkit_navigation_action_get_navigation_type)(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED &&
                 (W.webkit_navigation_action_is_user_gesture)(a);

    /* target=_blank / window.open: only a real click, only http(s), and it
     * stays in this preview when the page may be shown here. */
    if (new_window) {
        (W.webkit_policy_decision_ignore)(d);
        if (click && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))) {
            if (browser_nav_url_classify(url) == BROWSER_NAV_LOCAL ||
                browser_nav_allowlist_matches(n->allowlist, url))
                (W.webkit_web_view_load_uri)(n->view, url);
            else platform_open_url(url);
        }
        return TRUE;
    }
    /* Liu's own page, loaded by Liu. */
    if (n->internal_pending && !strcmp(url, "about:blank")) {
        n->internal_pending = false;
        (W.webkit_policy_decision_use)(d);
        return TRUE;
    }
    BrowserNavClass cls = browser_nav_url_classify(url);
    BrowserNavAction main = browser_nav_decide_with_list(url, cls, true, click, n->allowlist);
    if (click) {
        /* A click is the document's (a frame's link reads the same rules). */
        ++n->policy_seq;
        if (main == BROWSER_NAV_RESOLVE) {
            if (!resolve_later(n, d, url, true, false)) apply_action(n, d, BROWSER_NAV_REFUSE, url, cls, true);
            return TRUE;
        }
        if (main == BROWSER_NAV_ALLOW) snprintf(n->judged_main, sizeof n->judged_main, "%s", url);
        apply_action(n, d, main, url, cls, true);
        return TRUE;
    }
    if (main == BROWSER_NAV_ALLOW) { (W.webkit_policy_decision_use)(d); return TRUE; }
    /* Not a click and not allowed as the document: allowed as a frame's (an
     * embed), it goes on and its response is judged as the document's if it
     * turns out to be one. */
    BrowserNavAction sub = browser_nav_decide_with_list(url, cls, false, false, n->allowlist);
    if (sub == BROWSER_NAV_ALLOW) { (W.webkit_policy_decision_use)(d); return TRUE; }
    (W.webkit_policy_decision_ignore)(d);
    return TRUE;
}

static gboolean on_policy_response(BrowserNative *n, WebKitPolicyDecision *d) {
    WebKitResponsePolicyDecision *rd = (WebKitResponsePolicyDecision *)d;
    bool main = (W.webkit_response_policy_decision_is_main_frame_main_resource)(rd);
    WebKitURIResponse *resp = (W.webkit_response_policy_decision_get_response)(rd);
    /* What the server asked to be saved is not shown, and neither is what
     * WebKit cannot show: Liu's browser downloads nothing. */
    bool attachment = false;
    SoupMessageHeaders *h = resp ? (W.webkit_uri_response_get_http_headers)(resp) : NULL;
    const char *disp = h ? (W.soup_message_headers_get_one)(h, "Content-Disposition") : NULL;
    if (disp) {
        while (*disp == ' ' || *disp == '\t') disp++;
        attachment = !strncasecmp(disp, "attachment", 10);
    }
    if (attachment || !(W.webkit_response_policy_decision_is_mime_type_supported)(rd)) {
        if (main) set_str(n, n->status, sizeof n->status, "Not shown: the server sent this as a download");
        (W.webkit_policy_decision_ignore)(d);
        return TRUE;
    }
    if (!main) { (W.webkit_policy_decision_use)(d); return TRUE; }
    WebKitURIRequest *req = (W.webkit_response_policy_decision_get_request)(rd);
    const char *url = req ? (W.webkit_uri_request_get_uri)(req) : NULL;
    if (!url || !strcmp(url, "about:blank") || !strcmp(url, n->judged_main)) {
        (W.webkit_policy_decision_use)(d);
        return TRUE;
    }
    BrowserNavClass cls = browser_nav_url_classify(url);
    BrowserNavAction act = browser_nav_decide_with_list(url, cls, true, false, n->allowlist);
    if (act == BROWSER_NAV_RESOLVE) {
        if (!resolve_later(n, d, url, false, true)) apply_action(n, d, BROWSER_NAV_REFUSE, url, cls, true);
        return TRUE;
    }
    /* Not a click: nothing is handed to the system's browser from here. */
    if (act == BROWSER_NAV_HANDOFF) act = BROWSER_NAV_REFUSE;
    apply_action(n, d, act, url, cls, true);
    return TRUE;
}

static gboolean on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType type,
                                 gpointer data) {
    (void)v;
    BrowserNative *n = data;
    switch (type) {
    case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION: return on_policy_navigation(n, d, false);
    case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION: return on_policy_navigation(n, d, true);
    case WEBKIT_POLICY_DECISION_TYPE_RESPONSE:          return on_policy_response(n, d);
    default: (W.webkit_policy_decision_ignore)(d); return TRUE;
    }
}

/* --- loading ----------------------------------------------------------------- */

static void read_state(BrowserNative *n) {
    set_bool(n, &n->can_back, (W.webkit_web_view_can_go_back)(n->view));
    set_bool(n, &n->can_fwd, (W.webkit_web_view_can_go_forward)(n->view));
}

/* Liu's own pages are loaded from markup with no address: about:blank. */
static bool view_on_liu_page(BrowserNative *n) {
    const char *uri = (W.webkit_web_view_get_uri)(n->view);
    return uri && !strcmp(uri, "about:blank");
}

static void on_load_changed(WebKitWebView *v, WebKitLoadEvent e, gpointer data) {
    (void)v;
    BrowserNative *n = data;
    /* An address asked for while Liu's own page was still loading takes the
     * view: its events are the page's, not Liu's. */
    if ((n->error_pending || n->start_pending) && (e == WEBKIT_LOAD_STARTED || n->internal_started) &&
        !view_on_liu_page(n)) {
        n->error_pending = n->start_pending = n->internal_started = false;
    }
    if (e == WEBKIT_LOAD_STARTED) {
        if (n->error_pending || n->start_pending) {             /* Liu's own page */
            n->internal_started = true;
            return;
        }
        clear_document(n);
        n->showing_error = false;
        n->showing_start = false;
        set_bool(n, &n->loading, true);
        set_str(n, n->status, sizeof n->status, "Loading\xe2\x80\xa6");
        return;
    }
    if (e != WEBKIT_LOAD_FINISHED) return;
    /* A load that failed or was cancelled finishes after Liu's own page was
     * asked for, and before that page starts: it is not that page's end. */
    if ((n->error_pending || n->start_pending) && !n->internal_started) return;
    set_bool(n, &n->loading, false);
    read_state(n);
    if (n->error_pending) {
        n->error_pending = false;
        n->showing_error = true;
        install_chrome(n);
        if (n->err_url[0]) set_str(n, n->url, sizeof n->url, n->err_url);
        set_str(n, n->status, sizeof n->status, "");
        if (n->pick_mode) set_pick_mode(n, true);
        return;
    }
    if (n->start_pending) {
        n->start_pending = false;
        n->showing_start = true;
        install_chrome(n);
        set_str(n, n->url, sizeof n->url, "");
        set_str(n, n->title, sizeof n->title, "");
        set_str(n, n->status, sizeof n->status, "");
        if (n->pick_mode) set_pick_mode(n, true);
        return;
    }
    n->showing_error = false;
    n->showing_start = false;
    set_str(n, n->title, sizeof n->title, (W.webkit_web_view_get_title)(n->view));
    set_str(n, n->url, sizeof n->url, (W.webkit_web_view_get_uri)(n->view));
    if (!n->pick_mode) set_str(n, n->status, sizeof n->status, "");
    else set_pick_mode(n, true);
}

/* A failure as words a web developer knows, and the net error code Liu's
 * error page shows for it. */
static void map_error(const GError *e, const char **reason, i32 *code) {
    *reason = e && e->message && e->message[0] ? e->message : "couldn't be loaded";
    *code = e ? e->code : 0;
    if (!e) return;
    const char *m = e->message ? e->message : "";
    if (strstr(m, "refused")) { *reason = "refused to connect"; *code = -102; }
    else if (strstr(m, "timed out") || strstr(m, "Timeout")) { *reason = "took too long to respond"; *code = -7; }
    else if (strstr(m, "resolve") || strstr(m, "not known")) { *reason = "server address could not be found"; *code = -105; }
    else if (strstr(m, "reset")) { *reason = "the connection was reset"; *code = -101; }
    else if (strstr(m, "TLS") || strstr(m, "certificate")) { *reason = "the secure connection failed"; *code = -113; }
}

static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent e, gchar *uri, GError *err, gpointer data) {
    (void)v; (void)e;
    BrowserNative *n = data;
    set_bool(n, &n->loading, false);
    /* A load cancelled by a policy decision or by the next navigation is no
     * failure: no error page. */
    if (err && err->domain && (err->code == WEBKIT_NETWORK_ERROR_CANCELLED ||
                               err->code == WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE))
        return TRUE;
    if (n->error_pending) return TRUE;
    const char *reason; i32 code;
    map_error(err, &reason, &code);
    snprintf(n->err_url, sizeof n->err_url, "%s", uri ? uri : n->url);
    snprintf(n->err_reason, sizeof n->err_reason, "%s", reason);
    n->err_code = code;
    snprintf(n->err_domain, sizeof n->err_domain, "%s", "WebKitNetworkError");
    snprintf(n->err_desc, sizeof n->err_desc, "%s", err && err->message ? err->message : "");
    n->err_title[0] = '\0';
    n->err_hint[0] = '\0';
    render_error_page(n);
    return TRUE;
}

/* The loopback trust exception belongs to this view alone: a certificate a
 * local dev server made for itself. Anything else fails as WebKit decides. */
static gboolean on_tls_error(WebKitWebView *v, gchar *uri, GTlsCertificate *cert, GTlsCertificateFlags f,
                             gpointer data) {
    (void)f;
    BrowserNative *n = data;
    char host[256];
    if (!uri || !browser_nav_host(uri, host, sizeof host) || !browser_nav_host_is_loopback(host)) return FALSE;
    (W.webkit_web_context_allow_tls_certificate_for_host)((W.webkit_web_view_get_context)(v), cert, host);
    (W.webkit_web_view_load_uri)(n->view, uri);
    return TRUE;
}

/* No device permission is ever given: there is no prompt for one. */
static gboolean on_permission(WebKitWebView *v, WebKitPermissionRequest *r, gpointer data) {
    (void)v; (void)data;
    (W.webkit_permission_request_deny)(r);
    return TRUE;
}

/* window.print(): WebKit's own handler runs the print dialog in a nested main
 * loop inside Liu's wait, and the whole app stops until it closes. Nothing is
 * printed, as on macOS. */
static gboolean on_print(WebKitWebView *v, gpointer op, gpointer data) {
    (void)v; (void)op; (void)data;
    return TRUE;
}

/* Liu's browser downloads nothing (on_policy_response). The page's menu
 * offers no save, and a download started any other way is cancelled before
 * WebKit picks a file for it in the user's Downloads folder. */
static gboolean on_context_menu(WebKitWebView *v, WebKitContextMenu *menu, GdkEvent *ev,
                                WebKitHitTestResult *hit, gpointer data) {
    (void)v; (void)ev; (void)hit; (void)data;
    for (guint i = (W.webkit_context_menu_get_n_items)(menu); i-- > 0;) {
        WebKitContextMenuItem *it = (W.webkit_context_menu_get_item_at_position)(menu, i);
        switch (it ? (W.webkit_context_menu_item_get_stock_action)(it) : WEBKIT_CONTEXT_MENU_ACTION_NO_ACTION) {
        case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_LINK_TO_DISK:
        case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK:
        case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_VIDEO_TO_DISK:
        case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_AUDIO_TO_DISK:
            (W.webkit_context_menu_remove)(menu, it);
            break;
        default: break;
        }
    }
    return FALSE;
}

static void on_download_started(WebKitWebContext *ctx, WebKitDownload *dl, gpointer data) {
    (void)ctx; (void)data;
    WebKitWebView *v = (W.webkit_download_get_web_view)(dl);
    (W.webkit_download_cancel)(dl);
    for (i32 i = 0; v && i < WK_LIVE_MAX; i++)
        if (g_live[i] && g_live[i]->view == v)
            set_str(g_live[i], g_live[i]->status, sizeof g_live[i]->status,
                    "Not saved: the browser downloads nothing");
}

/* A popup the page asks for is never a new view (NEW_WINDOW_ACTION judged it). */
static GtkWidget *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer data) {
    (void)v; (void)a; (void)data;
    return NULL;
}

/* The title is the document's, Liu's own pages included (the error page's is
 * the address it could not open); the address of Liu's own page is not. */
static void on_notify(GObject *o, GParamSpec *p, gpointer data) {
    (void)o; (void)p;
    BrowserNative *n = data;
    set_str(n, n->title, sizeof n->title, (W.webkit_web_view_get_title)(n->view));
    read_state(n);
    if (n->start_pending || n->showing_start || n->error_pending || n->showing_error) return;
    set_str(n, n->url, sizeof n->url, (W.webkit_web_view_get_uri)(n->view));
}

static void on_theme_color(GObject *o, GParamSpec *p, gpointer data) {
    (void)o; (void)p;
    BrowserNative *n = data;
    if (!W.webkit_web_view_get_theme_color) return;
    GdkRGBA c;
    n->theme_tint_known = false;
    if ((W.webkit_web_view_get_theme_color)(n->view, &c) && c.alpha >= 0.5) {
        f64 comp[3] = { c.red, c.green, c.blue };
        u32 v = 0;
        for (i32 i = 0; i < 3; i++) v = (v << 8) | (u32)(fmin(1.0, fmax(0.0, comp[i])) * 255.0 + 0.5);
        n->theme_tint = v;
        n->theme_tint_known = true;
    }
    bool known = n->theme_tint_known || n->page_bg_known;
    u32 tint = n->theme_tint_known ? n->theme_tint : n->page_bg;
    if (known != n->page_tint_known || (known && tint != n->page_tint)) {
        n->page_tint = tint;
        n->page_tint_known = known;
        bump(n);
    }
}

/* --- keys --------------------------------------------------------------------- */

/* A key the page took that is Liu's: the binding table's chords (Ctrl+Shift,
 * Ctrl+Alt, Alt+digit, Ctrl+Tab, F11) and the browser's own (Ctrl+L, Ctrl+R,
 * Ctrl+., Ctrl+[ / ], Ctrl+,). Ctrl+C / V / X / A / Z / F stay the page's. */
static i32 liu_key_of(guint kv) {
    if (kv >= GDK_KEY_a && kv <= GDK_KEY_z) return KEY_A + (i32)(kv - GDK_KEY_a);
    if (kv >= GDK_KEY_A && kv <= GDK_KEY_Z) return KEY_A + (i32)(kv - GDK_KEY_A);
    if (kv >= GDK_KEY_0 && kv <= GDK_KEY_9) return KEY_0 + (i32)(kv - GDK_KEY_0);
    if (kv >= GDK_KEY_F1 && kv <= GDK_KEY_F12) return KEY_F1 + (i32)(kv - GDK_KEY_F1);
    switch (kv) {
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: return KEY_TAB;
    case GDK_KEY_bracketleft: case GDK_KEY_braceleft: return KEY_LEFT_BRACKET;
    case GDK_KEY_bracketright: case GDK_KEY_braceright: return KEY_RIGHT_BRACKET;
    case GDK_KEY_comma: case GDK_KEY_less: return KEY_COMMA;
    case GDK_KEY_period: case GDK_KEY_greater: return KEY_PERIOD;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: return KEY_ENTER;
    case GDK_KEY_Left: return KEY_LEFT;
    case GDK_KEY_Right: return KEY_RIGHT;
    case GDK_KEY_Up: return KEY_UP;
    case GDK_KEY_Down: return KEY_DOWN;
    default: return KEY_UNKNOWN;
    }
}

static gboolean on_key(GtkWidget *w, GdkEvent *ev, gpointer data) {
    (void)w; (void)data;
    const GdkEventKey *k = (const GdkEventKey *)ev;
    i32 key = liu_key_of(k->keyval);
    if (key == KEY_UNKNOWN) return FALSE;
    u32 mods = 0;
    if (k->state & GDK_CONTROL_MASK) mods |= MOD_CTRL;
    if (k->state & GDK_SHIFT_MASK)   mods |= MOD_SHIFT;
    if (k->state & GDK_MOD1_MASK)    mods |= MOD_ALT;
    if (k->state & GDK_SUPER_MASK)   mods |= MOD_SUPER;
    bool ctrl = mods & MOD_CTRL, shift = mods & MOD_SHIFT, alt = mods & MOD_ALT;
    bool liu = (ctrl && (shift || alt)) || (mods & MOD_SUPER) ||
               (alt && !ctrl && key >= KEY_0 && key <= KEY_9) ||
               (alt && shift && !ctrl && (key == KEY_LEFT || key == KEY_RIGHT ||
                                          key == KEY_UP || key == KEY_DOWN)) ||
               (ctrl && key == KEY_TAB) || (!mods && key == KEY_F11) ||
               (ctrl && !shift && !alt && (key == KEY_L || key == KEY_R || key == KEY_PERIOD ||
                                           key == KEY_LEFT_BRACKET || key == KEY_RIGHT_BRACKET ||
                                           key == KEY_COMMA));
    if (!liu) return FALSE;
    platform_push_key_down(key, mods, false);
    return TRUE;
}

/* --- the browser.h surface ---------------------------------------------------------- */

static void connect(gpointer obj, const char *sig, GCallback cb, BrowserNative *n) {
    (W.g_signal_connect_data)(obj, sig, cb, n, NULL, 0);
}

static bool make_view(BrowserNative *n) {
    Display *d = (Display *)platform_x11_display();
    unsigned long parent = platform_x11_window(n->window);
    if (!d || !parent) return false;
    n->ucm = (W.webkit_user_content_manager_new)();
    if (!n->ucm) return false;
    static const struct { const char *src; WebKitUserScriptInjectionTime when; } scripts[] = {
        { BROWSER_PICK_ELEMENT_JS, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START },
        { BROWSER_TINT_JS,         WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END },
    };
    for (usize i = 0; i < sizeof scripts / sizeof scripts[0]; i++) {
        WebKitUserScript *s = (W.webkit_user_script_new_for_world)(scripts[i].src,
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, scripts[i].when, LIU_WORLD, NULL, NULL);
        if (s) { (W.webkit_user_content_manager_add_script)(n->ucm, s); (W.webkit_user_script_unref)(s); }
    }
    static const struct { const char *name; GCallback cb; } handlers[] = {
        { "liuPick", G_CALLBACK(msg_pick) }, { "liuBrowser", G_CALLBACK(msg_browser) },
        { "liuTint", G_CALLBACK(msg_tint) },
    };
    for (usize i = 0; i < sizeof handlers / sizeof handlers[0]; i++) {
        char sig[64];
        snprintf(sig, sizeof sig, "script-message-received::%s", handlers[i].name);
        (W.webkit_user_content_manager_register_script_message_handler_in_world)(n->ucm, handlers[i].name, LIU_WORLD);
        connect(n->ucm, sig, handlers[i].cb, n);
    }
    n->view = (WebKitWebView *)(W.webkit_web_view_new_with_user_content_manager)(n->ucm);
    if (!n->view) return false;
    (W.webkit_settings_set_javascript_can_open_windows_automatically)((W.webkit_web_view_get_settings)(n->view), FALSE);
    connect(n->view, "decide-policy", G_CALLBACK(on_decide_policy), n);
    connect(n->view, "load-changed", G_CALLBACK(on_load_changed), n);
    connect(n->view, "load-failed", G_CALLBACK(on_load_failed), n);
    connect(n->view, "load-failed-with-tls-errors", G_CALLBACK(on_tls_error), n);
    connect(n->view, "permission-request", G_CALLBACK(on_permission), n);
    connect(n->view, "create", G_CALLBACK(on_create), n);
    connect(n->view, "notify::title", G_CALLBACK(on_notify), n);
    connect(n->view, "notify::uri", G_CALLBACK(on_notify), n);
    if (W.webkit_web_view_get_theme_color) connect(n->view, "notify::theme-color", G_CALLBACK(on_theme_color), n);
    connect(n->view, "key-press-event", G_CALLBACK(on_key), n);
    connect(n->view, "print", G_CALLBACK(on_print), n);
    connect(n->view, "context-menu", G_CALLBACK(on_context_menu), n);
    static bool downloads_hooked;      /* one context for every view; never disconnected */
    if (!downloads_hooked) {
        (W.g_signal_connect_data)((W.webkit_web_view_get_context)(n->view), "download-started",
                                  G_CALLBACK(on_download_started), NULL, NULL, 0);
        downloads_hooked = true;
    }

    n->socket = XCreateSimpleWindow(d, (Window)parent, 0, 0, 1, 1, 0, 0, 0);
    if (!n->socket) return false;
    /* GTK looks the socket up on its own connection: it must exist on the
     * server first, or the plug becomes a window of its own. */
    XSync(d, False);
    n->plug = (W.gtk_plug_new)(n->socket);
    if (!n->plug) return false;
    n->backdrop = (W.gtk_css_provider_new)();
    if (n->backdrop)
        (W.gtk_style_context_add_provider)((W.gtk_widget_get_style_context)(n->plug),
                                           (GtkStyleProvider *)n->backdrop,
                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    (W.gtk_container_add)((GtkContainer *)n->plug, (GtkWidget *)n->view);
    (W.gtk_widget_show_all)(n->plug);
    wk_pump(NULL);
    n->plug_xid = (Window)(W.gtk_plug_get_id)((GtkPlug *)n->plug);
    XFlush(d);
    return true;
}

/* The plug's windows are GDK's, made on GDK's own connection, and its unmap
 * and destroy of them are still in GDK's buffer when the plug is destroyed.
 * They reach the server before the socket goes, or they name windows already
 * gone with it: an X error, and GDK ends the process on one it did not trap. */
static void page_windows_destroy(BrowserNative *n) {
    Display *d = (Display *)platform_x11_display();
    GdkDisplay *gd = (W.gdk_display_get_default)();
    if (gd) (W.gdk_x11_display_error_trap_push)(gd);
    if (n->plug) (W.gtk_widget_destroy)(n->plug);
    if (gd) {
        (W.gdk_display_sync)(gd);
        (W.gdk_x11_display_error_trap_pop_ignored)(gd);
    }
    if (d && n->socket) { XDestroyWindow(d, n->socket); XFlush(d); }
    n->plug = NULL;
    n->socket = 0;
}

BrowserNative *browser_native_create(PlatformWindow *window) {
    BrowserNative *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->window = window;
    n->last_visible = -1;
    if (wk_load()) {
        i32 slot = -1;
        for (i32 i = 0; i < WK_LIVE_MAX && slot < 0; i++) if (!g_live[i]) slot = i;
        if (slot >= 0 && make_view(n)) {
            g_live[slot] = n;
            n->serial = ++g_serial;
            n->embedded = true;
            platform_x11_set_child_event_handler(wk_child_event, NULL);
        } else {
            page_windows_destroy(n);
            n->view = NULL;
            n->why = slot < 0 ? "too many pages are open" : "the page could not be made";
        }
    } else {
        n->why = g_wk_why;
    }
    return n;
}

void browser_native_destroy(BrowserNative *n) {
    if (!n) return;
    for (i32 i = 0; i < WK_LIVE_MAX; i++) if (g_live[i] == n) g_live[i] = NULL;
    if (n->embedded) {
        if (n->keyboard) take_keyboard(n);
        (W.g_signal_handlers_disconnect_matched)(n->view, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
        (W.g_signal_handlers_disconnect_matched)(n->ucm, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
        (W.webkit_web_view_stop_loading)(n->view);
        page_windows_destroy(n);
        (W.g_object_unref)(n->ucm);
        if (n->backdrop) (W.g_object_unref)(n->backdrop);
    }
    free(n->pending_json);
    free(n->recents_json);
    free(n);
}

/* A page moved to another window is reparented under it, hidden until placed. */
bool browser_native_attach_window(BrowserNative *n, PlatformWindow *window) {
    if (!n) return false;
    if (n->window == window) return true;
    if (n->embedded) {
        Display *d = (Display *)platform_x11_display();
        unsigned long parent = platform_x11_window(window);
        if (!d || !parent) return false;
        if (n->keyboard) take_keyboard(n);
        XUnmapWindow(d, n->socket);
        XReparentWindow(d, n->socket, (Window)parent, 0, 0);
        XFlush(d);
    }
    n->window = window;
    n->last_visible = -1;
    n->have_frame = false;
    n->have_clip = false;
    return true;
}

bool browser_native_in_window(BrowserNative *n, PlatformWindow *window) {
    return n && n->window == window;
}

void browser_native_set_visible(BrowserNative *n, bool visible) {
    if (!n || !n->embedded) return;
    if (n->last_visible == (visible ? 1 : 0)) return;
    n->last_visible = visible ? 1 : 0;
    if (visible && n->have_frame) apply_geometry(n);
    else if (!visible) {
        Display *d = (Display *)platform_x11_display();
        if (n->keyboard) take_keyboard(n);
        if (d) { XUnmapWindow(d, n->socket); XFlush(d); }
    }
}

/* X11 has no Cmd-drag over a page: a pane is picked up by its chrome. */
void browser_native_set_cmd_passthrough(BrowserNative *n, bool enabled) {
    (void)n; (void)enabled;
}

void browser_native_set_clip_pt(BrowserNative *n, bool clipped, f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt) {
    if (!n) return;
    bool want = clipped && isfinite(x_pt) && isfinite(y_pt) && isfinite(w_pt) && isfinite(h_pt);
    if (w_pt < 0.0f) w_pt = 0.0f;
    if (h_pt < 0.0f) h_pt = 0.0f;
    if (n->have_clip == want && (!want || (n->cx == x_pt && n->cy == y_pt && n->cw == w_pt && n->ch == h_pt)))
        return;
    n->have_clip = want;
    n->cx = x_pt; n->cy = y_pt; n->cw = w_pt; n->ch = h_pt;
    if (n->have_frame) apply_geometry(n);
}

void browser_native_set_frame_pt(BrowserNative *n, f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt) {
    if (!n || !isfinite(x_pt) || !isfinite(y_pt) || !isfinite(w_pt) || !isfinite(h_pt)) return;
    if (w_pt < 1.0f) w_pt = 1.0f;
    if (h_pt < 1.0f) h_pt = 1.0f;
    if (n->have_frame && n->fx == x_pt && n->fy == y_pt && n->fw == w_pt && n->fh == h_pt && !n->vp_w && !n->vp_h)
        return;
    n->have_frame = true;
    n->fx = x_pt; n->fy = y_pt; n->fw = w_pt; n->fh = h_pt;
    n->vp_w = n->vp_h = 0;
    apply_geometry(n);
}

void browser_native_set_viewport_pt(BrowserNative *n, f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt, i32 vw, i32 vh) {
    if (!n || !isfinite(x_pt) || !isfinite(y_pt) || !isfinite(w_pt) || !isfinite(h_pt)) return;
    if (w_pt < 1.0f) w_pt = 1.0f;
    if (h_pt < 1.0f) h_pt = 1.0f;
    if (vw < 0) vw = 0;
    if (vh < 0) vh = 0;
    if (n->have_frame && n->fx == x_pt && n->fy == y_pt && n->fw == w_pt && n->fh == h_pt &&
        n->vp_w == vw && n->vp_h == vh)
        return;
    n->have_frame = true;
    n->fx = x_pt; n->fy = y_pt; n->fw = w_pt; n->fh = h_pt;
    n->vp_w = vw; n->vp_h = vh;
    apply_geometry(n);
}

/* A device's rounded display: the socket's corners are cut (apply_shape). */
void browser_native_set_screen_shape(BrowserNative *n, const BrowserScreenShape *shape) {
    if (!n) return;
    memset(&n->shape, 0, sizeof n->shape);
    n->have_shape = false;
    if (shape)
        for (i32 i = 0; i < 4; i++)
            if (isfinite(shape->r[i]) && shape->r[i] > 0.0f) { n->shape.r[i] = shape->r[i]; n->have_shape = true; }
    if (n->have_frame) apply_geometry(n);
}

/* A hole opens where Liu's next frame draws a notice, but that frame is not
 * on screen yet: until it is presented the page is cut only where the frame
 * on screen and the next both have one, and the rest follows once the loop
 * has presented (holes_settle, from the next wait or pass). */
static void holes_cut_now(BrowserNative *n) {
    n->ncut = 0;
    for (i32 i = 0; i < n->nholes; i++)
        for (i32 j = 0; j < n->nwant; j++) {
            const BrowserHole *a = &n->holes[i], *b = &n->want[j];
            f32 x0 = fmaxf(a->x, b->x), y0 = fmaxf(a->y, b->y);
            f32 x1 = fminf(a->x + a->w, b->x + b->w), y1 = fminf(a->y + a->h, b->y + b->h);
            if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) continue;
            n->cut[n->ncut++] = (BrowserHole){ x0, y0, x1 - x0, y1 - y0, fmaxf(a->r, b->r) };
        }
}

static bool g_holes_pending;

static void holes_settle(void) {
    if (!g_holes_pending) return;
    g_holes_pending = false;
    for (i32 i = 0; i < WK_LIVE_MAX; i++) {
        BrowserNative *n = g_live[i];
        if (!n || !n->holes_pending) continue;
        n->holes_pending = false;
        memcpy(n->holes, n->want, (usize)n->nwant * sizeof n->want[0]);
        n->nholes = n->nwant;
        memcpy(n->cut, n->want, (usize)n->nwant * sizeof n->want[0]);
        n->ncut = n->nwant;
        if (n->have_frame && n->last_visible == 1) apply_geometry(n);
    }
}

void browser_native_set_holes(BrowserNative *n, const BrowserHole *holes, i32 count) {
    if (!n) return;
    BrowserHole next[BROWSER_HOLES_MAX];
    i32 k = 0;
    for (i32 i = 0; holes && i < count && k < BROWSER_HOLES_MAX; i++) {
        const BrowserHole *h = &holes[i];
        if (!isfinite(h->x) || !isfinite(h->y) || !isfinite(h->w) || !isfinite(h->h) || !isfinite(h->r) ||
            !(h->w > 0.0f) || !(h->h > 0.0f))
            continue;
        next[k] = *h;
        if (next[k].r < 0.0f) next[k].r = 0.0f;
        k++;
    }
    const BrowserHole *cur = n->holes_pending ? n->want : n->holes;
    i32 ncur = n->holes_pending ? n->nwant : n->nholes;
    if (k == ncur && (!k || !memcmp(next, cur, (usize)k * sizeof next[0]))) return;
    memcpy(n->want, next, (usize)k * sizeof next[0]);
    n->nwant = k;
    n->holes_pending = true;
    g_holes_pending = true;
    i32 before = n->ncut;
    holes_cut_now(n);
    if ((before || n->ncut) && n->have_frame && n->last_visible == 1) apply_geometry(n);
}

bool browser_native_screen_probe(BrowserNative *n, BrowserScreenProbe *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!n || !n->embedded) return false;
    out->radius = n->shaped_r;
    out->corners = n->shaped_corners;
    out->holes = n->shaped_holes;
    /* Placed means inside the pane: the page's window is the socket's child. */
    Display *d = (Display *)platform_x11_display();
    Window root = 0, parent = 0, *kids = NULL;
    unsigned nkids = 0;
    bool inside = d && n->plug_xid && XQueryTree(d, n->plug_xid, &root, &parent, &kids, &nkids) &&
                  parent == n->socket;
    if (kids) XFree(kids);
    if (n->placed && inside) {
        out->have_page = true;
        out->page_x = n->px; out->page_y = n->py; out->page_w = n->pw; out->page_h = n->ph;
    }
    return true;
}

/* Text masks, kept by what they say: the clock and the percentage are asked
 * for every frame a device is drawn. */
#define WK_MASK_CACHE 8
typedef struct {
    char  text[64];
    f32   size_px, weight;
    u8   *pixels;
    i32   w, h;
    f32   baseline, cap;
    u64   used;
} WkMask;
static WkMask g_masks[WK_MASK_CACHE];
static u64    g_mask_clock;

static bool mask_text(const BrowserMaskSpec *s, WkMask *e) {
    usize len = strlen(s->text);
    cairo_surface_t *probe = (C.cairo_image_surface_create)(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = (C.cairo_create)(probe);
    cairo_font_weight_t bold = s->weight >= 0.2f ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL;
    (C.cairo_select_font_face)(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, bold);
    (C.cairo_set_font_size)(cr, s->size_px);
    cairo_font_extents_t fe;
    cairo_text_extents_t te, he;
    (C.cairo_font_extents)(cr, &fe);
    (C.cairo_text_extents)(cr, s->text, &te);
    (C.cairo_text_extents)(cr, "H", &he);
    bool good = (C.cairo_status)(cr) == CAIRO_STATUS_SUCCESS && len > 0;
    (C.cairo_destroy)(cr);
    (C.cairo_surface_destroy)(probe);
    if (!good) return false;
    f64 asc = ceil(fe.ascent), desc = ceil(fe.descent);
    i32 w = (i32)ceil(te.x_advance) + 2, h = (i32)(asc + desc) + 2;
    if (w <= 2 || w > 4096 || h < 3 || h > 1024) return false;
    cairo_surface_t *sf = (C.cairo_image_surface_create)(CAIRO_FORMAT_A8, w, h);
    cr = (C.cairo_create)(sf);
    (C.cairo_select_font_face)(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, bold);
    (C.cairo_set_font_size)(cr, s->size_px);
    (C.cairo_set_source_rgba)(cr, 1.0, 1.0, 1.0, 1.0);
    (C.cairo_move_to)(cr, 1.0, asc + 1.0);
    (C.cairo_show_text)(cr, s->text);
    (C.cairo_surface_flush)(sf);
    good = (C.cairo_status)(cr) == CAIRO_STATUS_SUCCESS;
    u8 *px = good ? malloc((usize)w * (usize)h * 4u) : NULL;
    if (px) {
        const u8 *a = (C.cairo_image_surface_get_data)(sf);
        i32 stride = (C.cairo_image_surface_get_stride)(sf);
        for (i32 y = 0; y < h; y++)
            for (i32 x = 0; x < w; x++) {
                u8 v = a[(usize)y * (usize)stride + (usize)x];
                memset(px + ((usize)y * (usize)w + (usize)x) * 4u, v, 4);
            }
    }
    (C.cairo_destroy)(cr);
    (C.cairo_surface_destroy)(sf);
    if (!px) return false;
    free(e->pixels);
    snprintf(e->text, sizeof e->text, "%s", s->text);
    e->size_px = s->size_px;
    e->weight = s->weight;
    e->pixels = px;
    e->w = w;
    e->h = h;
    e->baseline = (f32)(asc + 1.0);
    e->cap = (f32)fmax(0.0, -he.y_bearing);
    return true;
}

/* Text only: symbols, fans and wedges are drawn by the caller. */
bool browser_native_mask(const BrowserMaskSpec *spec, BrowserMask *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!spec || !out || spec->kind != BROWSER_MASK_TEXT || !spec->text || !spec->text[0]) return false;
    if (strlen(spec->text) >= sizeof g_masks[0].text) return false;
    if (!(spec->size_px > 1.0f) || spec->size_px > 512.0f || !isfinite(spec->weight)) return false;
    if (g_wk_state != 1 || !C.ok) return false;
    WkMask *e = NULL, *oldest = &g_masks[0];
    for (i32 i = 0; i < WK_MASK_CACHE && !e; i++) {
        WkMask *m = &g_masks[i];
        if (m->pixels && m->size_px == spec->size_px && m->weight == spec->weight && !strcmp(m->text, spec->text))
            e = m;
        else if (m->used < oldest->used) oldest = m;
    }
    if (!e) {
        if (!mask_text(spec, oldest)) return false;
        e = oldest;
    }
    e->used = ++g_mask_clock;
    out->pixels = e->pixels;
    out->w = e->w;
    out->h = e->h;
    out->baseline = e->baseline;
    out->cap = e->cap;
    return true;
}

bool browser_native_device_art(const BrowserDeviceLayout *d, f32 dpi, BrowserDeviceArt *out) {
    (void)d; (void)dpi;
    if (out) memset(out, 0, sizeof *out);
    return false;
}

void browser_native_make_first_responder(BrowserNative *n) {
    if (n && n->embedded) give_keyboard(n);
}

void browser_native_resign_first_responder(BrowserNative *n) {
    if (n && n->embedded) take_keyboard(n);
}

void browser_native_set_allowlist(BrowserNative *n, const BrowserAllowlist *list) {
    if (n) n->allowlist = list;
}

bool browser_native_navigate(BrowserNative *n, const char *url) {
    if (!n || !url || !url[0] || strnlen(url, BROWSER_URL_CAP) >= BROWSER_URL_CAP) return false;
    if (!n->embedded) {
        /* The full, resolving gate: nothing else stands between a typed name
         * and the system's browser here. */
        if (!browser_nav_is_allowed_resolved_with_list(url, n->allowlist)) return false;
        set_str(n, n->url, sizeof n->url, url);
        platform_open_url(url);
        return true;
    }
    if (!browser_nav_is_allowed_with_list(url, n->allowlist)) return false;
    /* Judged by the policy like any load that is not a click: a public site
     * typed here is refused at its response, as the document's. */
    n->judged_main[0] = '\0';
    (W.webkit_web_view_load_uri)(n->view, url);
    set_str(n, n->url, sizeof n->url, url);
    set_bool(n, &n->loading, true);
    set_str(n, n->status, sizeof n->status, "Loading\xe2\x80\xa6");
    return true;
}

static bool retry_error_page(BrowserNative *n) {
    if (!n->showing_error || !n->err_url[0]) return false;
    n->judged_main[0] = '\0';
    (W.webkit_web_view_load_uri)(n->view, n->err_url);
    set_bool(n, &n->loading, true);
    set_str(n, n->status, sizeof n->status, "Loading\xe2\x80\xa6");
    return true;
}

void browser_native_reload(BrowserNative *n) {
    if (!n) return;
    if (!n->embedded) {
        if (n->url[0] && browser_nav_is_allowed_resolved_with_list(n->url, n->allowlist)) platform_open_url(n->url);
        return;
    }
    if (!retry_error_page(n)) (W.webkit_web_view_reload)(n->view);
}

void browser_native_reload_hard(BrowserNative *n) {
    if (!n || !n->embedded) return;
    if (retry_error_page(n)) return;
    (W.webkit_web_view_reload_bypass_cache)(n->view);
    set_str(n, n->status, sizeof n->status, "Hard reload\xe2\x80\xa6");
}

typedef struct { BrowserNative *n; u64 serial; } WkRef;

static void cleared(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src; (void)res;
    WkRef ref = *(WkRef *)data;
    free(data);
    BrowserNative *n = ref.n;
    if (!live_as(n, ref.serial)) return;
    if (retry_error_page(n)) return;
    (W.webkit_web_view_reload_bypass_cache)(n->view);
    set_bool(n, &n->loading, true);
    set_str(n, n->status, sizeof n->status, "Site data cleared - reloading\xe2\x80\xa6");
}

void browser_native_reset_and_reload(BrowserNative *n) {
    if (!n || !n->embedded) return;
    WkRef *ref = malloc(sizeof *ref);
    if (!ref) return;
    *ref = (WkRef){ n, n->serial };
    set_str(n, n->status, sizeof n->status, "Clearing site data\xe2\x80\xa6");
    (W.webkit_website_data_manager_clear)((W.webkit_web_view_get_website_data_manager)(n->view),
                                        WEBKIT_WEBSITE_DATA_ALL, 0, NULL, cleared, ref);
}

void browser_native_go_back(BrowserNative *n)    { if (n && n->embedded) (W.webkit_web_view_go_back)(n->view); }
void browser_native_go_forward(BrowserNative *n) { if (n && n->embedded) (W.webkit_web_view_go_forward)(n->view); }

void browser_native_stop(BrowserNative *n) {
    if (!n) return;
    if (n->embedded) (W.webkit_web_view_stop_loading)(n->view);
    set_bool(n, &n->loading, false);
}

bool browser_native_poll(BrowserNative *n, BrowserTab *tab) {
    if (!n || !tab) return false;
    if (!n->embedded) {
        char status[sizeof tab->status];
        snprintf(status, sizeof status, "Pages open in the system browser (%s)",
                 n->why ? n->why : g_wk_why);
        bool changed = tab->loading || (n->url[0] && strcmp(tab->url, n->url) != 0) ||
                       strcmp(tab->status, status) != 0;
        tab->loading = false;
        if (n->url[0]) snprintf(tab->url, sizeof tab->url, "%s", n->url);
        snprintf(tab->status, sizeof tab->status, "%s", status);
        return changed;
    }
    /* The X focus can leave the page by a press in Liu's window. */
    if (n->keyboard) {
        Display *d = (Display *)platform_x11_display();
        Window focus = None;
        int revert = 0;
        if (d) XGetInputFocus(d, &focus, &revert);
        if (d && focus != n->plug_xid) {
            xembed_send(n->plug_xid, XEMBED_FOCUS_OUT, 0);
            n->keyboard = false;
        }
    }
    if (n->revision == n->polled_rev) return false;
    n->polled_rev = n->revision;
    tab->loading = n->loading;
    tab->can_go_back = n->can_back;
    tab->can_go_forward = n->can_fwd;
    tab->pick_mode = n->pick_mode;
    if (n->title[0]) (void)liu_clean_dictation(n->title, tab->title, (i32)sizeof tab->title);
    else tab->title[0] = '\0';
    snprintf(tab->url, sizeof tab->url, "%s", n->url);
    snprintf(tab->status, sizeof tab->status, "%s", n->status);
    tab->pending_pick = n->has_pending_pick;
    tab->pending_ask = n->has_pending_ask;
    tab->pending_nav = n->has_pending_nav;
    tab->page_tint = n->page_tint;
    tab->page_tint_known = n->page_tint_known;
    return true;
}

bool browser_native_set_pick_mode(BrowserNative *n, bool enabled) {
    if (!n || !n->embedded) return !enabled;
    return set_pick_mode(n, enabled);
}

void browser_native_set_pick_theme(BrowserNative *n, const char *theme_json) {
    if (!n || !n->embedded || !theme_json || !theme_json[0]) return;
    usize len = strlen(theme_json) + 96;
    char *js = malloc(len);
    if (!js) return;
    snprintf(js, len, "window.__liuPickSetTheme && window.__liuPickSetTheme(%s);", theme_json);
    run_js(n, js);
    free(js);
}

void browser_native_set_page_theme(BrowserNative *n, const char *css_decls, const char *font_path) {
    if (!n || !n->embedded || !css_decls || !css_decls[0]) return;
    /* The declarations go into a <style> block: nothing may leave the rule. */
    if (strpbrk(css_decls, "<>{}")) return;
    snprintf(n->theme_css, sizeof n->theme_css, "%s", css_decls);
    n->mono[0] = '\0';
    if (font_path && font_path[0]) (void)sfnt_family_file(font_path, n->mono, sizeof n->mono);
    /* What shows before a page has painted is the theme's background, not the
     * toolkit's. Liu themes its own documents, never the user's: a page that
     * paints no background of its own stays on white, as a browser shows it. */
    const char *bg = strstr(css_decls, "--bg:#");
    if (bg && n->backdrop && strspn(bg + 6, "0123456789abcdefABCDEF") >= 6) {
        char rule[96];
        snprintf(rule, sizeof rule, "window { background-color: #%.6s; }", bg + 6);
        (W.gtk_css_provider_load_from_data)(n->backdrop, rule, -1, NULL);
    }
    if (n->showing_start || n->start_pending) show_start_page(n);
    else if (n->showing_error || n->error_pending) render_error_page(n);
}

static void pick_copy(const cJSON *body, const char *key, char *out, usize cap) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, key);
    if (!cJSON_IsString(v) || !v->valuestring) return;
    usize len = strlen(v->valuestring);
    if (len >= cap) { len = cap - 1; while (len && ((unsigned char)v->valuestring[len] & 0xC0) == 0x80) len--; }
    memcpy(out, v->valuestring, len);
    out[len] = '\0';
}

bool browser_native_take_pick(BrowserNative *n, BrowserPick *out) {
    if (!n || !out || !n->has_pending_pick || !n->pending_json) return false;
    memset(out, 0, sizeof *out);
    cJSON *body = cJSON_Parse(n->pending_json);
    free(n->pending_json);
    n->pending_json = NULL;
    set_bool(n, &n->has_pending_pick, false);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(body, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "pick")) { cJSON_Delete(body); return false; }
    pick_copy(body, "url", out->page_url, sizeof out->page_url);
    pick_copy(body, "tag", out->tag, sizeof out->tag);
    pick_copy(body, "id", out->id, sizeof out->id);
    pick_copy(body, "classes", out->classes, sizeof out->classes);
    pick_copy(body, "cssSelector", out->selector, sizeof out->selector);
    pick_copy(body, "cssPath", out->css_path, sizeof out->css_path);
    pick_copy(body, "text", out->text, sizeof out->text);
    pick_copy(body, "ariaLabel", out->aria_label, sizeof out->aria_label);
    pick_copy(body, "role", out->role, sizeof out->role);
    pick_copy(body, "dataTestid", out->data_testid, sizeof out->data_testid);
    pick_copy(body, "html", out->html, sizeof out->html);
    out->html_len = (i32)strlen(out->html);
    out->html_truncated = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "htmlTruncated")) ||
                          strstr(out->html, "<!-- truncated -->") != NULL;
    const cJSON *rect = cJSON_GetObjectItemCaseSensitive(body, "rect");
    static const char *const rk[] = { "x", "y", "w", "h" };
    f32 *rc[] = { &out->x, &out->y, &out->w, &out->h };
    for (i32 i = 0; cJSON_IsObject(rect) && i < 4; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(rect, rk[i]);
        if (cJSON_IsNumber(v) && isfinite(v->valuedouble))
            *rc[i] = (f32)fmax(-10000000.0, fmin(10000000.0, v->valuedouble));
    }
    const cJSON *lim = cJSON_GetObjectItemCaseSensitive(body, "limitations");
    if (cJSON_IsArray(lim)) {
        Buf b = {0};
        const cJSON *it;
        cJSON_ArrayForEach(it, lim) {
            if (!cJSON_IsString(it)) continue;
            if (b.n) buf_str(&b, ", ");
            buf_str(&b, it->valuestring);
        }
        if (b.p) {
            usize len = strlen(b.p);
            if (len >= sizeof out->limitations) {
                len = sizeof out->limitations - 1;
                while (len && ((unsigned char)b.p[len] & 0xC0) == 0x80) len--;
            }
            memcpy(out->limitations, b.p, len);
            out->limitations[len] = '\0';
        }
        free(b.p);
    }
    cJSON_Delete(body);
    return out->tag[0] != '\0' || out->selector[0] != '\0' || out->html[0] != '\0';
}

bool browser_native_take_ask(BrowserNative *n, char *url, usize url_cap, char *reason, usize reason_cap,
                             i32 *code) {
    if (!n || !n->has_pending_ask) return false;
    if ((url && url_cap == 0) || (reason && reason_cap == 0)) return false;
    if (url && strlen(n->err_url) >= url_cap) { set_bool(n, &n->has_pending_ask, false); return false; }
    if (url) snprintf(url, url_cap, "%s", n->err_url);
    if (reason) {
        usize len = strlen(n->err_reason);
        if (len >= reason_cap) { len = reason_cap - 1; while (len && ((unsigned char)n->err_reason[len] & 0xC0) == 0x80) len--; }
        memcpy(reason, n->err_reason, len);
        reason[len] = '\0';
    }
    if (code) *code = n->err_code;
    set_bool(n, &n->has_pending_ask, false);
    return true;
}

void browser_native_show_start_page(BrowserNative *n, const char *recents_json) {
    if (!n || !n->embedded) return;
    if (recents_json && strnlen(recents_json, 256u * 1024u + 1u) > 256u * 1024u) return;
    free(n->recents_json);
    n->recents_json = recents_json && recents_json[0] ? strdup(recents_json) : NULL;
    show_start_page(n);
}

bool browser_native_take_nav(BrowserNative *n, char *url, usize url_cap) {
    if (!n || !url || url_cap == 0) return false;
    url[0] = '\0';
    if (!n->has_pending_nav) return false;
    usize len = strlen(n->pending_nav);
    set_bool(n, &n->has_pending_nav, false);
    if (!len || len >= url_cap || !browser_nav_is_allowed_with_list(n->pending_nav, n->allowlist)) return false;
    memcpy(url, n->pending_nav, len + 1);
    return true;
}
