/*
 * Liu Browser — shared types (C API surface used by UI + platform bridge).
 */
#ifndef BROWSER_TYPES_H
#define BROWSER_TYPES_H

#include "core/types.h"

#define BROWSER_URL_CAP       2048
#define BROWSER_TITLE_CAP      256
#define BROWSER_STATUS_CAP     256
#define BROWSER_SITE_PATH_CAP 1024
#define BROWSER_SITE_NAME_CAP   64
#define BROWSER_MAX_TABS         4
#define BROWSER_HTML_CAP     (16 * 1024)
#define BROWSER_SEL_CAP        512
#define BROWSER_PATH_CAP      1024
#define BROWSER_TEXT_CAP       512
#define BROWSER_ATTR_CAP      2048
/* Metadata + 16 KiB HTML + two worst-case fences must fit without leaving an
 * open data fence in the agent prompt. */
#define BROWSER_PASTE_CAP    (64 * 1024)

/* Structured DOM pick payload from Select Element. */
typedef struct {
    char page_url[BROWSER_URL_CAP];
    char tag[64];
    char id[128];
    char classes[256];
    char selector[BROWSER_SEL_CAP];   /* short preferred selector */
    char css_path[BROWSER_PATH_CAP];
    char text[BROWSER_TEXT_CAP];
    char aria_label[256];
    char role[64];
    char data_testid[128];
    char attrs[BROWSER_ATTR_CAP];     /* "k=v; k2=v2" flat form */
    char html[BROWSER_HTML_CAP];
    i32  html_len;
    bool html_truncated;
    f32  x, y, w, h;
    char limitations[128];
} BrowserPick;

typedef struct {
    f32 x_px, y_px, w_px, h_px;
    f32 x_pt, y_pt, w_pt, h_pt;
    f32 chrome_h_px;
    f32 page_y_px, page_h_px;
    f32 page_x_pt, page_y_pt, page_w_pt, page_h_pt;
} BrowserContentRect;

typedef struct {
    f32 back_x, fwd_x, reload_x, device_x, pick_x, attach_x, ext_x, close_x;
    f32 sep_x;           /* hairline before the close button; <0 = none */
    f32 url_x, url_w, url_h, url_y;
    f32 btn_y, btn_s;
    f32 chrome_h;
    f32 icon_s;          /* glyph size inside button hit box */
    f32 pad;
    /* Responsive visibility — false means not laid out / not hit-testable. */
    bool show_back, show_fwd, show_reload;
    bool show_url, show_device, show_pick, show_attach, show_ext, show_close;
    bool show_sep;
    f32  scale;          /* 0.75–1.0 based on pane width */
} BrowserChromeLayout;

typedef struct BrowserNative BrowserNative;

typedef struct BrowserTab {
    char     url[BROWSER_URL_CAP];
    char     url_edit[BROWSER_URL_CAP];
    i32      url_edit_len;
    bool     url_focused;
    char     title[BROWSER_TITLE_CAP];
    char     status[BROWSER_STATUS_CAP];

    bool     loading;
    bool     can_go_back;
    bool     can_go_forward;
    bool     pick_mode;

    char     site_path[BROWSER_SITE_PATH_CAP];
    char     site_name[BROWSER_SITE_NAME_CAP];

    BrowserNative *native;

    bool     has_pick;
    BrowserPick last_pick;
    bool     pending_pick;   /* native has a pick waiting for take */
    bool     pending_ask;    /* native has an "Ask Agent" (error page) waiting */
    bool     pending_nav;    /* native has a start-page click nav waiting */

    /* Device viewport emulation (see browser/browser_viewport.h).
     * `viewport_idx` indexes the preset table — BVP_RESPONSIVE (0) means fill
     * the pane. `viewport_scale` is the fit scale the renderer computed, kept
     * so the chrome can label it ("Fit 62%") without recomputing. */
    i32      viewport_idx;
    bool     viewport_landscape;
    f32      viewport_scale;
    /* The colour the page asks to be framed in (its theme-color, else the
     * colour under it), 0xRRGGBB: a device's status bar is painted in it, as a
     * phone's browser does. False while the page gives none. */
    u32      page_tint;
    bool     page_tint_known;
    /* Viewport picker popover state. */
    bool     viewport_menu_open;
    i32      viewport_menu_hover;   /* preset index, -1 = none */
    f32      viewport_menu_scroll;  /* pixels scrolled within the popover */

    /* Name of the theme whose colours were last pushed into the native page
     * chrome. The render loop compares it against the active theme and
     * re-pushes on a mismatch, so a theme switch in Settings recolours an open
     * start / error page without a dedicated notification path. */
    char     themed_as[64];

    BrowserChromeLayout layout;
} BrowserTab;

#endif /* BROWSER_TYPES_H */
