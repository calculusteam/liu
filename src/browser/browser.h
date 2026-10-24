/*
 * Liu Browser — public C API (WKWebView on macOS, stub on Linux).
 *
 * PR1: navigate loopback URLs in a native WebView attached as a subview of
 * SSHView, with overlay hide + chrome Cmd routing.
 * PR2+: Select Element pick bridge.
 */
#ifndef BROWSER_H
#define BROWSER_H

#include "browser/browser_types.h"
#include "browser/browser_nav.h"
#include "platform/platform.h"

/* Lifecycle for one tab's native host. window may be NULL on Linux stub. */
BrowserNative *browser_native_create(PlatformWindow *window);
/* Main-thread live config, owned by the app and valid until this view is
 * destroyed. NULL uses the built-in local-network policy. */
void browser_native_set_allowlist(BrowserNative *n, const BrowserAllowlist *list);
void           browser_native_destroy(BrowserNative *n);
/* Move the native view into another window. A tab that changes windows (torn
 * off, or dropped on another window's strip) is a struct copy as far as the app
 * is concerned, but the WKWebView is a real subview of the window that made it
 * and stays behind unless it is re-parented. The caller must also re-point the
 * allowlist at the new owner's config (browser_native_set_allowlist): the old
 * pointer dies with the window the tab left. True when the view now lives in
 * `window` (always true where there is no native view to move). */
bool           browser_native_attach_window(BrowserNative *n, PlatformWindow *window);
/* Is the native view parented to `window`? For probes and tests. */
bool           browser_native_in_window(BrowserNative *n, PlatformWindow *window);

void browser_native_set_visible(BrowserNative *n, bool visible);
void browser_native_set_frame_pt(BrowserNative *n,
                                 f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt);
void browser_native_make_first_responder(BrowserNative *n);
/* The keyboard back to Liu's own view when the page has it (Cmd+L). */
void browser_native_resign_first_responder(BrowserNative *n);

/* Let a Cmd-held click/drag fall THROUGH the native WebView to the Metal view
 * underneath, so Liu's Cmd+drag pane-move gesture works over a browser pane
 * exactly as it does over a terminal pane. Without it WebKit eats the whole
 * gesture and the app's hit test never runs.
 *
 * Enable only while the browser actually sits in a split (there is a pane to
 * move): in a full-tab browser Cmd+click keeps its web meaning — WebKit's
 * "open in a new window", which Liu routes to the system browser. */
void browser_native_set_cmd_passthrough(BrowserNative *n, bool enabled);

/* Restrict the page to the part of the given rect (window points, AppKit
 * bottom-left origin) it overlaps — everything outside is masked away without
 * moving, resizing or re-laying-out the page. `clipped == false` removes the
 * restriction; a clipped rect that the page misses entirely hides it.
 *
 * This is how Liu's Metal panels "cover" the browser. They can't: the WebView
 * is a native subview stacked above the Metal layer, so a panel drawn over the
 * page area (the file viewer/editor is the one that really overlaps) ends up
 * *under* it and the two flicker over the same pixels. Clipping the WebView to
 * the region the panel leaves free reproduces the intended stacking, and —
 * because native views also own their events — is what lets clicks in that
 * region reach the panel at all. */
void browser_native_set_clip_pt(BrowserNative *n, bool clipped,
                                f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt);

/* Constrain the page to a device viewport instead of filling the pane.
 *
 * `x/y/w/h` is the on-screen rect (points, AppKit bottom-left origin) the
 * scaled device should occupy — the caller has already centred it. `vw/vh` is
 * the CSS-pixel viewport the page must believe it has; pass 0 to go back to
 * filling the rect.
 *
 * The scaling is AppKit's, not a crop or a zoom: the view's frame is the
 * on-screen rect while its bounds are vw × vh, so the render is scaled down
 * and `window.innerWidth`, media queries and `vw` units all still report the
 * device. */
void browser_native_set_viewport_pt(BrowserNative *n,
                                    f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt,
                                    i32 vw, i32 vh);

/* Navigation */
bool browser_native_navigate(BrowserNative *n, const char *url);
void browser_native_reload(BrowserNative *n);
/* Hard reload — re-fetch from the origin, ignoring the cache. For a dev server
 * that rebuilt while the page keeps showing the old bundle. */
void browser_native_reload_hard(BrowserNative *n);
/* Hardest reset: purge this WebView's website data (cache, storage, service
 * workers) and then reload. A stale service worker survives even a hard
 * reload, and on a local dev server that is the usual reason a change refuses
 * to appear. Asynchronous — the reload happens once the purge completes. */
void browser_native_reset_and_reload(BrowserNative *n);
void browser_native_go_back(BrowserNative *n);
void browser_native_go_forward(BrowserNative *n);
void browser_native_stop(BrowserNative *n);

/* State poll (call from main thread tick). O(1) when nothing changed since
 * the last poll (revision-counter early-out); returns true only when new
 * state was copied into `tab` — the caller's "needs redraw" signal. */
bool browser_native_poll(BrowserNative *n, BrowserTab *tab);

/* PR2 stubs — no-ops in PR1. */
/* False when arming is refused: Liu's own start or error page is showing.
 * Arming does not take the keyboard (browser_native_make_first_responder). */
bool browser_native_set_pick_mode(BrowserNative *n, bool enabled);
bool browser_native_take_pick(BrowserNative *n, BrowserPick *out);

/* Push the element-picker inspector theme into the page: `theme_json` is a
 * JS object literal (see pick_element.js `theme` for the keys — accent as
 * "r,g,b", the rest CSS color strings). Call BEFORE enabling pick mode so
 * the overlay mounts already themed; safe to call any time (the injected
 * script re-stamps a mounted overlay live). The caller builds the literal
 * from the ACTIVE terminal theme, so the picker follows Liu's theme. */
void browser_native_set_pick_theme(BrowserNative *n, const char *theme_json);

/* Theme the browser's OWN surfaces from the active terminal theme: the
 * internal start page (intro / Recents), the connection-error page, and the
 * WebView's under-page colour (what shows through an overscroll rubber band
 * and between navigations — black by default, which flashed against every
 * theme). `css_decls` is a CSS declaration list dropped verbatim into the
 * internal pages' `:root{…}` — see app_browser_send_page_theme() in ui.c for
 * the variable contract. `font_path` is the terminal font file (may be NULL);
 * its family is used for the pages' monospace runs when that family is also
 * installed system-wide, so the WebContent process can actually resolve it.
 *
 * Pushing a new theme re-renders whichever internal page is on screen, so a
 * theme switch in Settings recolours an open start page live. Real websites
 * are untouched — only Liu's own chrome documents. */
void browser_native_set_page_theme(BrowserNative *n, const char *css_decls,
                                   const char *font_path);

/* Take a pending "Ask Agent" request raised from the connection-error page.
 * Fills the failed url + mapped reason/code and returns true once. */
bool browser_native_take_ask(BrowserNative *n, char *url, usize url_cap,
                             char *reason, usize reason_cap, i32 *code);

/* Show the internal start page instead of a live site: the intro/welcome
 * screen when recents_json is an empty array (or NULL), else a "Recents" list.
 * recents_json is a JSON array of URL strings, e.g. ["http://localhost:5173/"]. */
void browser_native_show_start_page(BrowserNative *n, const char *recents_json);

/* Take a pending navigation raised by clicking a Recents entry. */
bool browser_native_take_nav(BrowserNative *n, char *url, usize url_cap);

/* Feature compile-time: LIU_BROWSER=1 on macOS when WebKit linked. */
#ifndef LIU_BROWSER
#define LIU_BROWSER 0
#endif

#endif /* BROWSER_H */
