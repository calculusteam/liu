/*
 * Liu Browser — Linux: hand the URL to the system browser.
 *
 * The tab, its chrome and its policy gate are all real:
 * browser_native_navigate runs the same local/tailnet-only policy the
 * WKWebView path runs, then opens the URL with xdg-open, and poll() reports
 * that plainly instead of leaving a blank pane that appears to load. ui.c has
 * a matching `#if !LIU_BROWSER` branch that creates this backend and
 * navigates through it, so the flow is wired end to end. What is absent is an
 * embedded view — so back/forward, element pick, page theming and viewport
 * emulation are inert here.
 *
 * Embedding one needs WebKitGTK (a large new dependency), a GTK main loop
 * cohabiting with the X11/GLX loop this app runs, and an XReparentWindow of
 * the web view into the GL window — and then all 23 entry points, including
 * the JS injection behind pick mode and page theming.
 */
#include "browser/browser.h"
#include "browser/browser_nav.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct BrowserNative {
    char url[BROWSER_URL_CAP];
    bool loading;
    const BrowserAllowlist *allowlist;
};

BrowserNative *browser_native_create(PlatformWindow *window) {
    (void)window;
    return calloc(1, sizeof(BrowserNative));
}

void browser_native_destroy(BrowserNative *n) { free(n); }

/* Nothing native to move: the stub hands URLs to the system browser. */
bool browser_native_attach_window(BrowserNative *n, PlatformWindow *window) {
    (void)window;
    return n != NULL;
}
bool browser_native_in_window(BrowserNative *n, PlatformWindow *window) {
    (void)window;
    return n != NULL;
}

void browser_native_set_visible(BrowserNative *n, bool visible) {
    (void)n; (void)visible;
}

void browser_native_set_frame_pt(BrowserNative *n,
                                 f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt) {
    (void)n; (void)x_pt; (void)y_pt; (void)w_pt; (void)h_pt;
}

void browser_native_make_first_responder(BrowserNative *n) { (void)n; }
void browser_native_resign_first_responder(BrowserNative *n) { (void)n; }

void browser_native_set_cmd_passthrough(BrowserNative *n, bool enabled) {
    (void)n; (void)enabled;   /* no in-app WebView to pass through */
}

void browser_native_set_clip_pt(BrowserNative *n, bool clipped,
                                f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt) {
    (void)n; (void)clipped; (void)x_pt; (void)y_pt; (void)w_pt; (void)h_pt;
}

void browser_native_set_allowlist(BrowserNative *n, const BrowserAllowlist *list) {
    if (n) n->allowlist = list;
}

bool browser_native_navigate(BrowserNative *n, const char *url) {
    if (!n || !url) return false;
    /* The full, resolving gate: nothing here runs it off the main thread the
     * way the WKWebView delegate does, and a public site must not be handed
     * on just because it was spelled as a name. */
    if (!browser_nav_is_allowed_resolved_with_list(url, n->allowlist)) return false;
    int wrote = snprintf(n->url, sizeof n->url, "%s", url);
    if (wrote < 0 || (usize)wrote >= sizeof n->url) {
        n->url[0] = '\0';
        return false;
    }
    platform_open_url(url);
    n->loading = false;
    return true;
}

void browser_native_reload(BrowserNative *n) {
    if (n && n->url[0] && browser_nav_is_allowed_resolved_with_list(n->url, n->allowlist))
        platform_open_url(n->url);
}

void browser_native_go_back(BrowserNative *n)    { (void)n; }
void browser_native_go_forward(BrowserNative *n) { (void)n; }
void browser_native_stop(BrowserNative *n)       { if (n) n->loading = false; }

bool browser_native_poll(BrowserNative *n, BrowserTab *tab) {
    if (!n || !tab) return false;
    static const char *kStatus =
        "Linux: opened in system browser (in-app WebView deferred)";
    bool changed = (tab->loading != n->loading) ||
                   (n->url[0] && strcmp(tab->url, n->url) != 0) ||
                   (strcmp(tab->status, kStatus) != 0);
    tab->loading = n->loading;
    if (n->url[0]) snprintf(tab->url, sizeof tab->url, "%s", n->url);
    snprintf(tab->status, sizeof tab->status, "%s", kStatus);
    return changed;
}

bool browser_native_set_pick_mode(BrowserNative *n, bool enabled) {
    (void)n;
    return !enabled;                /* no picker here: arming is refused */
}

void browser_native_set_pick_theme(BrowserNative *n, const char *theme_json) {
    (void)n; (void)theme_json;
}

void browser_native_set_page_theme(BrowserNative *n, const char *css_decls,
                                   const char *font_path) {
    (void)n; (void)css_decls; (void)font_path;
}

void browser_native_reload_hard(BrowserNative *n) { (void)n; }
void browser_native_reset_and_reload(BrowserNative *n) { (void)n; }

void browser_native_set_viewport_pt(BrowserNative *n,
                                    f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt,
                                    i32 vw, i32 vh) {
    (void)n; (void)x_pt; (void)y_pt; (void)w_pt; (void)h_pt; (void)vw; (void)vh;
}

void browser_native_set_screen_shape(BrowserNative *n, const BrowserScreenShape *shape) {
    (void)n; (void)shape;
}

bool browser_native_screen_probe(BrowserNative *n, BrowserScreenProbe *out) {
    (void)n;
    if (out) memset(out, 0, sizeof *out);
    return false;
}

bool browser_native_mask(const BrowserMaskSpec *spec, BrowserMask *out) {
    (void)spec;
    if (out) memset(out, 0, sizeof *out);
    return false;
}

bool browser_native_device_art(const BrowserDeviceLayout *d, f32 dpi, BrowserDeviceArt *out) {
    (void)d; (void)dpi;
    if (out) memset(out, 0, sizeof *out);
    return false;
}

bool browser_native_take_pick(BrowserNative *n, BrowserPick *out) {
    (void)n; (void)out;
    return false;
}

bool browser_native_take_ask(BrowserNative *n, char *url, usize url_cap,
                             char *reason, usize reason_cap, i32 *code) {
    (void)n; (void)url; (void)url_cap; (void)reason; (void)reason_cap; (void)code;
    return false;
}

void browser_native_show_start_page(BrowserNative *n, const char *recents_json) {
    (void)n; (void)recents_json;
}

bool browser_native_take_nav(BrowserNative *n, char *url, usize url_cap) {
    (void)n; (void)url; (void)url_cap;
    return false;
}
