#ifndef UI_BROWSER_UI_H
#define UI_BROWSER_UI_H

#include "core/types.h"
#include "core/config.h"
#include "renderer/renderer.h"
#include "browser/browser_types.h"
#include "browser/browser_device.h"

struct AppState;
struct PlatformWindow;

/* True when any Metal modal must cover the content rect (WebView must hide). */
bool browser_any_metal_overlay(const struct AppState *app);

/* Active tab is TAB_BROWSER and no overlay is covering it. */
bool browser_should_show(const struct AppState *app);

/* Content + page rects for the browser tab body. */
void browser_content_rect(const struct AppState *app, BrowserContentRect *out);

/* The device drawn around the active browser's emulated viewport, laid out in
 * its page area (points, y down), or false when the page fills the pane. The
 * painter and the web view's placement use the same layout. */
bool browser_device_now(const struct AppState *app, BrowserDeviceLayout *out);

/* Draw Metal chrome (URL bar / buttons) for the active browser tab. */
void browser_ui_render(struct AppState *app, f32 ox, f32 oy, f32 w, f32 h);

/* Apply WebView visibility + frame for all browser tabs (hide inactive). */
void browser_apply_visibility_and_frame(struct AppState *app,
                                        struct PlatformWindow *window);

/* Per-frame poll of native state. Returns true if UI should redraw. */
bool app_browser_tick(struct AppState *app);

/* An element picked with Select Element in `browser`'s page: kept as its
 * last pick, pick mode ended, and sent to the agent (app_browser_inject_pick). */
void app_browser_pick_arrived(struct AppState *app, BrowserTab *browser,
                              const BrowserPick *pick);

/* Hit-test helpers for chrome strip (returns sub-index; -1 = none). */
enum {
    BROWSER_HIT_NONE = 0,
    BROWSER_HIT_BACK,
    BROWSER_HIT_FWD,
    BROWSER_HIT_RELOAD,
    BROWSER_HIT_URL,
    BROWSER_HIT_DEVICE,
    BROWSER_HIT_PICK,
    BROWSER_HIT_ATTACH,
    BROWSER_HIT_EXTERNAL,
    BROWSER_HIT_CLOSE,
};

i32 browser_chrome_hit(const struct AppState *app, f32 mx, f32 my);

/* Viewport picker popover — device presets with resolution + pixel ratio.
 * Render after the browser chrome so it floats above it. The click handler
 * returns true when it consumed the event (a row was picked, or the click was
 * inside/outside the open popover, which closes it); the hover handler returns
 * true when the highlighted row changed and a redraw is needed. */
void browser_render_viewport_menu(struct AppState *app, Renderer *r);
bool browser_viewport_menu_click(struct AppState *app, f32 mx, f32 my);
bool browser_viewport_menu_hover(struct AppState *app, f32 mx, f32 my);
/* Centre of preset `idx`'s row in the open popover (frame pixels), false
 * when the menu is closed or the row is scrolled out of the list. */
bool browser_viewport_menu_row_center(const struct AppState *app, i32 idx,
                                      f32 *cx, f32 *cy);
/* The open popover's rectangle (frame pixels), false when it is closed. */
bool browser_viewport_menu_rect(const struct AppState *app, f32 *x, f32 *y,
                                f32 *w, f32 *h);
/* Wheel/trackpad over the open popover. Returns true when consumed. */
bool browser_viewport_menu_scroll(struct AppState *app, f32 mx, f32 my,
                                  f32 dy, bool precise);

#endif
