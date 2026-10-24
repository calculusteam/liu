/*
 * Liu — the "Customize sidebar" popover: the event half.
 *
 * Geometry is ui/sidebar_customize.h (pure) filled by app_sbc_layout(); the
 * painter is render_sidebar_customize() in ui.c. This file owns what a press,
 * a drag and a release DO: every control writes the config the sidebar reads
 * (sidebar_tabs / sidebar_sections / sidebar_compact / sidebar_width /
 * sidebar_collapse_below), applies it to the live window and persists it —
 * the same fields Settings › Terminal edits, so the two surfaces cannot
 * disagree.
 *
 * Interaction rules, each from the design:
 *   - Changes apply live; "Done" and the × only close. A click outside the
 *     panel dismisses it and is swallowed (it was aimed at the panel's edge,
 *     not at what is behind it).
 *   - A section row is one big switch: pressing anywhere on it toggles the
 *     section — unless the pointer moves SBC_DRAG_SLOP_PT first, which turns
 *     the press into a reorder drag. The grip column is drag-only, so a hand
 *     resting on it never flips a switch by accident.
 *   - The width slider scrubs: the value under the pointer is the value
 *     drawn there (sbc_slider_t_at shares the knob radius with the painter),
 *     the bar follows every move, and the config is written on release.
 *   - Esc closes. Other keys pass through — the popover is not a modal.
 */
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/filebrowser.h"
#include "ui/sidebar_customize.h"
#include "core/config.h"
#include "platform/platform.h"

#include <math.h>
#include <string.h>

/* Saved, and given to the other windows, by the loop (liu_config_commit in
 * main.c): saved here alone, a later change in another window wrote that
 * window's stale copy over it. */
static void sbc_save(AppState *app) {
    app->config_commit_pending = true;
    app->sbc_dirty = false;
}

/* The width and the docked/undocked switch both move the grid: re-derive the
 * layout the way the divider drag does (app_apply_sidebar_resize in main.c). */
static void sbc_relayout(AppState *app) {
    if (!app->host_window) return;
    i32 w, h, fw, fh;
    platform_window_get_size(app->host_window, &w, &h);
    platform_window_get_framebuffer_size(app->host_window, &fw, &fh);
    app_update_layout(app, w, h, fw, fh);
}

static void sbc_clear_press(AppState *app) {
    app->sbc_press_slot = -1;
    app->sbc_press_grip = false;
    app->sbc_drag_id = -1;
    app->sbc_width_drag = false;
}

void sbc_open(AppState *app) {
    if (!app) return;
    app->sbc_open = true;
    app->sbc_dirty = false;
    app->sbc_anchor_set = false;   /* the first frame captures where the bar is */
    sbc_clear_press(app);
}

void sbc_close(AppState *app) {
    if (!app) return;
    if (app->sbc_dirty) sbc_save(app);
    app->sbc_open = false;
    sbc_clear_press(app);
}

void sbc_toggle(AppState *app) {
    if (!app) return;
    if (app->sbc_open) sbc_close(app); else sbc_open(app);
}

/* ---- the settings themselves ------------------------------------------- */

static void sbc_set_docked(AppState *app, bool on) {
    if (app->config.sidebar_tabs == on) return;
    app->config.sidebar_tabs = on;
    /* The tab list is where the tabs live while docked; a "-tabs" left in
     * the sections string would hide them everywhere. */
    app->config.sidebar_enabled[SB_SEC_TABS] = true;
    /* Undocking takes the tab list off the bar; if it was the only section
     * showing, the explorer comes back rather than an empty column. */
    config_sidebar_ensure_section(&app->config);
    sbc_relayout(app);
    sbc_save(app);
}

static void sbc_set_enabled(AppState *app, i32 id, bool on) {
    if (id < 0 || id >= SB_SEC_COUNT) return;
    if (id == SB_SEC_TABS) { sbc_set_docked(app, on); return; }
    if (app->config.sidebar_enabled[id] == on) return;
    /* The last visible section cannot be switched off: the bar must never
     * be empty, so the switch simply stays on. */
    if (!on && config_sidebar_visible_sections(&app->config) <= 1) return;
    app->config.sidebar_enabled[id] = on;
    /* Switching the explorer off closes what it had open, as the section's
     * own × does; switching it back on lets render_sidebar reopen it at the
     * focused pane's folder. */
    if (id == SB_SEC_FILES && !on) fb_close_viewer(&app->filebrowser);
    sbc_save(app);
}

static void sbc_set_compact(AppState *app, bool on) {
    if (app->config.sidebar_compact == on) return;
    app->config.sidebar_compact = on;
    sbc_save(app);
}

/* Width in points → config, and the live bar when it is showing its full
 * width (the rail has one width; a hidden bar takes the value on reopen). */
static void sbc_apply_width_pt(AppState *app, f32 pt) {
    pt = CLAMP(pt, SIDEBAR_MIN_PT, SIDEBAR_MAX_PT);
    if (fabsf(app->config.sidebar_width - pt) < 0.5f) return;
    app->config.sidebar_width = pt;
    app->sbc_dirty = true;
    if (app->sidebar_visible && !app_sidebar_rail(app)) {
        app->sidebar_width = pt * app->dpi_scale;
        sbc_relayout(app);
    }
}

static void sbc_set_collapse(AppState *app, bool on) {
    bool cur = app->config.sidebar_collapse_below_pt > 0.0f;
    if (cur == on) return;
    /* The threshold itself is Settings › Terminal's (Never / 1100 / 1300);
     * the popover's switch keeps the last non-zero value or the default. */
    app->config.sidebar_collapse_below_pt = on ? 1100.0f : 0.0f;
    sbc_save(app);
}

void sbc_reset_defaults(AppState *app) {
    if (!app) return;
    AppConfig *c = &app->config;
    c->sidebar_tabs = false;
    for (i32 i = 0; i < SB_SEC_COUNT; i++) { c->sidebar_order[i] = (u8)i; c->sidebar_enabled[i] = true; }
    c->sidebar_enabled[SB_SEC_NOTES] = false;
    c->sidebar_compact = false;
    c->sidebar_collapse_below_pt = 1100.0f;
    c->sidebar_width = SIDEBAR_DEFAULT_PT;
    for (i32 i = 0; i < SB_SEC_COUNT; i++) app->sidebar_sec_open[i] = true;
    if (app->sidebar_visible && !app_sidebar_rail(app))
        app->sidebar_width = SIDEBAR_DEFAULT_PT * app->dpi_scale;
    sbc_relayout(app);
    sbc_save(app);
}

/* ---- events -------------------------------------------------------------- */

bool sbc_key(AppState *app, i32 key, u32 mods) {
    (void)mods;
    if (!app || !app->sbc_open) return false;
    if (key == KEY_ESCAPE) { sbc_close(app); return true; }
    return false;
}

bool sbc_mouse_down(AppState *app, f32 x, f32 y) {
    if (!app || !app->sbc_open) return false;
    SbcLayout L;
    app_sbc_layout(app, &L);
    SbcHit h = sbc_hit(&L, x, y, app->dpi_scale);
    sbc_clear_press(app);
    switch (h.kind) {
    case SBC_HIT_OUTSIDE:
    case SBC_HIT_CLOSE:
    case SBC_HIT_DONE:
        sbc_close(app);
        return true;
    case SBC_HIT_TABS_SEG:
        sbc_set_docked(app, h.index == 1);
        return true;
    case SBC_HIT_ROW_SWITCH: {
        u8 id = app->config.sidebar_order[h.index];
        if (id < SB_SEC_COUNT) {
            bool on = (id == SB_SEC_TABS) ? app->config.sidebar_tabs
                                          : app->config.sidebar_enabled[id];
            sbc_set_enabled(app, id, !on);
        }
        return true;
    }
    case SBC_HIT_ROW:
    case SBC_HIT_ROW_GRIP: {
        /* Arm: the release decides between a click (the row's switch) and
         * the end of a drag. The grip never clicks. */
        SbcRect row = sbc_row_rect(&L, h.index);
        app->sbc_press_slot = h.index;
        app->sbc_press_y = y;
        app->sbc_drag_grab = y - row.y;
        app->sbc_drag_y = y;
        app->sbc_press_grip = (h.kind == SBC_HIT_ROW_GRIP);
        return true;
    }
    case SBC_HIT_DENSITY_SEG:
        sbc_set_compact(app, h.index == 1);
        return true;
    case SBC_HIT_WIDTH:
        app->sbc_width_drag = true;
        sbc_apply_width_pt(app, sbc_width_pt_from_t(h.t));
        return true;
    case SBC_HIT_COLLAPSE:
        sbc_set_collapse(app, !(app->config.sidebar_collapse_below_pt > 0.0f));
        return true;
    case SBC_HIT_RESET:
        sbc_reset_defaults(app);
        return true;
    case SBC_HIT_PANEL:
        return true;
    }
    return true;
}

bool sbc_mouse_move(AppState *app, f32 x, f32 y) {
    if (!app || !app->sbc_open) return false;
    if (app->sbc_width_drag) {
        SbcLayout L;
        app_sbc_layout(app, &L);
        sbc_apply_width_pt(app, sbc_width_pt_from_t(sbc_slider_t_at(L.width_slider, x, app->dpi_scale)));
        return true;
    }
    if (app->sbc_press_slot >= 0) {
        SbcLayout L;
        app_sbc_layout(app, &L);
        i32 slot = app->sbc_press_slot;
        if (app->sbc_drag_id < 0) {
            if (fabsf(y - app->sbc_press_y) < SBC_DRAG_SLOP_PT * app->dpi_scale) return true;
            if (slot < 0 || slot >= SB_SEC_COUNT) return true;
            app->sbc_drag_id = app->config.sidebar_order[slot];
        }
        app->sbc_drag_y = y;
        /* Reorder live: the dragged row's top picks its slot, the others
         * shift out of its way. Tracked by section id, since the order it is
         * in moves under it. */
        i32 cur = -1;
        for (i32 i = 0; i < SB_SEC_COUNT; i++)
            if (app->config.sidebar_order[i] == app->sbc_drag_id) { cur = i; break; }
        i32 want = sbc_slot_for_top(&L, y - app->sbc_drag_grab);
        if (cur >= 0 && want != cur) {
            sbc_order_move(app->config.sidebar_order, SB_SEC_COUNT, cur, want);
            app->sbc_dirty = true;
        }
        return true;
    }
    SbcLayout L;
    app_sbc_layout(app, &L);
    return sbc_rect_has(L.panel, x, y);
}

bool sbc_mouse_up(AppState *app, f32 x, f32 y) {
    (void)x; (void)y;
    if (!app || !app->sbc_open) return false;
    if (app->sbc_width_drag) {
        app->sbc_width_drag = false;
        if (app->sbc_dirty) sbc_save(app);
        return true;
    }
    if (app->sbc_press_slot >= 0) {
        bool grip = app->sbc_press_grip;
        i32 slot = app->sbc_press_slot;
        bool dragged = app->sbc_drag_id >= 0;
        sbc_clear_press(app);
        if (dragged) {
            if (app->sbc_dirty) sbc_save(app);
        } else if (!grip && slot >= 0 && slot < SB_SEC_COUNT) {
            u8 id = app->config.sidebar_order[slot];
            bool on = (id == SB_SEC_TABS) ? app->config.sidebar_tabs
                                          : app->config.sidebar_enabled[id];
            sbc_set_enabled(app, id, !on);
        }
        return true;
    }
    return false;
}

bool sbc_scroll(AppState *app, f32 x, f32 y) {
    if (!app || !app->sbc_open) return false;
    SbcLayout L;
    app_sbc_layout(app, &L);
    /* Nothing in the panel scrolls; a wheel over it must not scroll what is
     * behind it. */
    return sbc_rect_has(L.panel, x, y);
}
