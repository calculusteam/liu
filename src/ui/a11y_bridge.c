/*
 * Liu - the accessibility provider: which terminal a window describes, where
 * its grid sits, and what the text area is called. The text itself is
 * a11y_text.c's; this file only resolves AppState into it.
 */
#include "ui/a11y_text.h"

#include "ui/ui.h"
#include "ui/hittest.h"
#include "ui/layout.h"

#include <stdio.h>
#include <string.h>

/* One buffer for the whole process: providers run on the main thread only,
 * and the contract (platform_a11y.h) is that a snapshot lives until the next
 * provider call of ANY window — the platform copies what it keeps. Allocated
 * the first time an assistive app asks, never before. */
static A11yTextBuf g_a11y_buf;
static char        g_a11y_label[192];

/* The focused pane of the active tab, and its pane index. */
static Terminal *a11y_focused(AppState *app, const Tab **out_tab, i32 *out_pane) {
    Tab *tab = app_active_tab(app);
    *out_tab  = tab;
    *out_pane = 0;
    if (!tab || tab->kind != TAB_TERMINAL) return NULL;
    if (tab->split != SPLIT_NONE && tab->split_root >= 0) {
        Terminal *t = app_tab_pane_terminal(tab, tab->active_pane);
        if (t) { *out_pane = tab->active_pane; return t; }
    }
    return tab->terminal;
}

/* The grid's top-left in framebuffer pixels — the steps ui_terminal_pane_cell
 * takes (terminal body, the pane's slice, the centring leftover on both
 * axes), so a frame VoiceOver draws lands on the cells the renderer drew. */
static bool a11y_grid_origin(const AppState *app, const Tab *tab, i32 pane,
                             const Terminal *t, f32 cw, f32 ch,
                             f32 *out_x, f32 *out_y) {
    f32 ox, w, oy, bottom;
    ui_terminal_hspan(app, &ox, &w);
    ui_terminal_vspan(app, &oy, &bottom);
    f32 h = bottom - oy;
    if (tab->split != SPLIT_NONE && tab->split_root >= 0) {
        PaneRect rects[MAX_SPLIT_PANES];
        memset(rects, 0, sizeof(rects));
        app_tab_pane_layout(tab, ox, oy, w, h, app->dpi_scale, rects);
        if (pane < 0 || pane >= MAX_SPLIT_PANES ||
            rects[pane].w <= 0.0f || rects[pane].h <= 0.0f) return false;
        ox = rects[pane].x; oy = rects[pane].y;
        w  = rects[pane].w; h  = rects[pane].h;
    }
    *out_x = ox + term_grid_pad_x(w, t->cols, cw);
    *out_y = oy + term_grid_pad_y(h, t->rows, ch);
    return true;
}

bool app_a11y_provider(PlatformWindow *w, void *user, PlatformA11yText *out,
                       bool generation_only) {
    (void)w;
    AppState *app = (AppState *)user;
    if (!app || !out) return false;
    memset(out, 0, sizeof(*out));

    const Tab *tab = NULL;
    i32 pane = 0;
    Terminal *t = a11y_focused(app, &tab, &pane);

    /* The pane's identity for a tree that names it (AT-SPI): the Terminal it
     * reads, which changes exactly when the focus moves to another pane. */
    out->source = (u64)(uintptr_t)t;

    if (generation_only) {
        a11y_text_generation(t, &out->gen_value, &out->gen_selection);
        return t != NULL;
    }

    const char *title = NULL;
    if (tab) title = tab->custom_title[0] ? tab->custom_title : tab->title;
    if (title && title[0])
        snprintf(g_a11y_label, sizeof(g_a11y_label), "Terminal \xE2\x80\x94 %s", title);
    else
        snprintf(g_a11y_label, sizeof(g_a11y_label), "Terminal");

    bool ok = a11y_text_build(t, &g_a11y_buf, out);   /* starts from a clean `out` */
    out->source = (u64)(uintptr_t)t;
    out->label = g_a11y_label;
    if (!ok) return false;

    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    f32 cw = app->renderer.font.cell_width, ch = app->renderer.font.cell_height;
    f32 gx, gy;
    if (tab && cw > 0.0f && ch > 0.0f &&
        a11y_grid_origin(app, tab, pane, t, cw, ch, &gx, &gy)) {
        out->has_geometry = true;
        out->grid_x = gx / dpi;
        out->grid_y = gy / dpi;
        out->cell_w = cw / dpi;
        out->cell_h = ch / dpi;
    }
    return true;
}
