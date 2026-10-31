/*
 * Liu — sidebar sections: the pure half.
 *
 * Which sections exist, in what order, how tall, whether the bar is docked or
 * folded to its rail, and the row walks the tab list and the rail are built
 * from — everything the painter (ui.c), the hit test (hittest.c) and the tab
 * popup all need to agree on, kept out of ui.c so code that links hittest.c
 * does not have to link the renderer to get the same answers. Nothing here
 * draws; the arithmetic itself is ui/sidebar_layout.h. The ownership hand-over
 * of the sidebar's CONTENT between AppState and the tab (app_sidebar_stow /
 * restore / sync) is at the bottom, for the same reason.
 */
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/icon.h"
#include "ui/sidebar_customize.h"
#include "ui/sftp_tab.h"

#include <string.h>

bool app_sidebar_tabs_docked(const AppState *app) {
    return app && app->config.sidebar_tabs;
}

/* Auto-fold: below the configured window width the docked sidebar keeps
 * only its icon rail, whatever the toggle says — a 900pt window with a
 * 240pt bar of tab titles leaves no terminal worth the name. */
static bool app_sidebar_auto_rail(const AppState *app) {
    if (!app_sidebar_tabs_docked(app)) return false;
    f32 below = app->config.sidebar_collapse_below_pt;
    return below > 0.0f && (f32)app->fb_width / app->dpi_scale < below;
}

bool app_sidebar_rail(const AppState *app) {
    return app_sidebar_tabs_docked(app) &&
           (!app->sidebar_visible || app_sidebar_auto_rail(app));
}

/* The width the open/close slide animates TO. Docked, "closed" is the rail,
 * never nothing: the tabs have to live somewhere. */
f32 app_sidebar_target_width(const AppState *app) {
    if (app_sidebar_rail(app)) return SIDEBAR_RAIL_PT * app->dpi_scale;
    if (!app->sidebar_visible) return 0.0f;
    return CLAMP(app->config.sidebar_width, SIDEBAR_MIN_PT, SIDEBAR_MAX_PT) * app->dpi_scale;
}

/* The docked list walks the tabs in the SAME order the strip draws them —
 * every used group (header, then its members unless collapsed), then the
 * ungrouped tabs, then the new-tab row — so Cmd+1..9 and the eye agree. */
i32 app_side_tab_rows(const AppState *app, SideTabRow *rows, i32 cap) {
    i32 n = 0;
    #define SIDE_PUSH(k, i) do { if (rows && n < cap) { rows[n].kind = (k); rows[n].index = (i); } n++; } while (0)
    for (i32 gi = 0; gi < MAX_TAB_GROUPS; gi++) {
        if (!app->tab_groups[gi].used) continue;
        bool has = false;
        for (i32 i = 0; i < app->tab_count; i++)
            if (app->tabs[i].group_index == gi) { has = true; break; }
        if (!has) continue;
        SIDE_PUSH(SIDE_ROW_GROUP, gi);
        if (app->tab_groups[gi].collapsed) continue;
        for (i32 i = 0; i < app->tab_count; i++)
            if (app->tabs[i].group_index == gi) SIDE_PUSH(SIDE_ROW_TAB, i);
    }
    for (i32 i = 0; i < app->tab_count; i++) {
        i32 g = app->tabs[i].group_index;
        if (g >= 0 && g < MAX_TAB_GROUPS && app->tab_groups[g].used) continue;
        SIDE_PUSH(SIDE_ROW_TAB, i);
    }
    SIDE_PUSH(SIDE_ROW_NEW, -1);
    #undef SIDE_PUSH
    return n;
}

/* The tabs section of `L`, NULL when the bar has none (undocked, the rail). */
static const SidebarSectionRect *side_tabs_section(const SidebarLayout *L) {
    if (!L || L->rail) return NULL;
    i32 i = sidebar_layout_index(L, SB_SEC_TABS);
    return i >= 0 ? &L->sec[i] : NULL;
}

void app_side_jumps(const AppState *app, const SidebarLayout *L, SidebarJump out[2]) {
    memset(out, 0, 2 * sizeof *out);
    const SidebarSectionRect *s = side_tabs_section(L);
    if (!app || !s) return;
    SideTabRow rows[MAX_TABS + MAX_TAB_GROUPS + 1];
    i32 n = app_side_tab_rows(app, rows, (i32)(sizeof rows / sizeof rows[0]));
    if (n > (i32)(sizeof rows / sizeof rows[0])) n = (i32)(sizeof rows / sizeof rows[0]);
    bool notable[MAX_TABS + MAX_TAB_GROUPS + 1];
    i32 active_row = -1;
    for (i32 r = 0; r < n; r++) {
        notable[r] = false;
        if (rows[r].kind != SIDE_ROW_TAB) continue;
        i32 ti = rows[r].index;
        if (ti < 0 || ti >= app->tab_count) continue;
        const Tab *t = &app->tabs[ti];
        if (ti == app->active_tab) active_row = r;
        notable[r] = ti == app->active_tab || t->has_bell || t->agent_working_count > 0;
    }
    sidebar_jumps_compute(s, L->row_h, app->side_tabs_scroll, notable, n, active_row,
                          0.0f, app->sidebar_width, app->dpi_scale, out);
    for (i32 d = 0; d < 2; d++)
        for (i32 k = 0; k < out[d].n_icons; k++)
            out[d].tabs[k] = rows[out[d].rows[k]].index;
}

void app_side_tabs_glide_to(AppState *app, i32 tab, f64 now) {
    if (!app) return;
    SidebarLayout L;
    app_sidebar_layout(app, &L);
    const SidebarSectionRect *s = side_tabs_section(&L);
    if (!s) return;
    SideTabRow rows[MAX_TABS + MAX_TAB_GROUPS + 1];
    i32 n = app_side_tab_rows(app, rows, (i32)(sizeof rows / sizeof rows[0]));
    if (n > (i32)(sizeof rows / sizeof rows[0])) n = (i32)(sizeof rows / sizeof rows[0]);
    for (i32 r = 0; r < n; r++) {
        if (rows[r].kind != SIDE_ROW_TAB || rows[r].index != tab) continue;
        app->side_tabs_glide = true;
        app->side_tabs_glide_from = app->side_tabs_scroll;
        app->side_tabs_glide_to = sidebar_scroll_to_row(s, L.row_h, r);
        app->side_tabs_glide_t0 = now;
        return;
    }
}

/* Header actions per section. The tab list carries a "+" (new tab); the
 * others have none yet. ICON_* values, as i32 so code linking hittest.c
 * need not pull in the icon table. */
i32 app_side_section_actions(i32 section, i32 *act1, i32 *act2) {
    i32 a1 = ICON_NONE, a2 = ICON_NONE;
    if (section == SB_SEC_TABS) a1 = ICON_PLUS;
    if (act1) *act1 = a1;
    if (act2) *act2 = a2;
    return (a1 != ICON_NONE) + (a2 != ICON_NONE);
}

/* Every pane in this window with a known foreground agent. */
i32 app_side_agent_count(const AppState *app) {
    i32 n = 0;
    for (i32 i = 0; i < app->tab_count; i++) {
        const Tab *tab = &app->tabs[i];
        if (tab->kind != TAB_TERMINAL) continue;
        i32 panes = app_tab_pane_count(tab);
        for (i32 p = 0; p < panes && p < MAX_SPLIT_PANES; p++)
            if (tab->pane_agents[p].tool) n++;
    }
    return n;
}

void app_sidebar_layout(const AppState *app, SidebarLayout *out) {
    SidebarLayoutIn in;
    memset(&in, 0, sizeof in);
    in.x = 0.0f;
    in.y = app->tab_bar_height;
    in.w = app->sidebar_width;
    in.h = (f32)app->fb_height - in.y - app->status_bar_height;
    in.dpi = app->dpi_scale;
    in.docked = app_sidebar_tabs_docked(app);
    in.compact = app->config.sidebar_compact;
    for (i32 i = 0; i < SB_SEC_COUNT; i++) {
        in.order[i]   = app->config.sidebar_order[i];
        in.enabled[i] = app->config.sidebar_enabled[i];
        /* Until the first frame initialises them, every section is open. */
        in.open[i]    = app->sidebar_sec_inited ? app->sidebar_sec_open[i] : true;
    }
    /* When tabs are not docked, the explorer is shown by itself unless
     * other sidebar sections are enabled. */
    in.tab_rows   = app_side_tab_rows(app, NULL, 0);
    in.agent_rows = app_side_agent_count(app);
    in.note_rows  = app->side_notes.count;
    in.files_has_content = app->filebrowser.open;
    sidebar_layout_compute(&in, out);
    out->rail = app_sidebar_rail(app);
}


/* The rail lists tab groups and their tabs, the new-tab button, then enabled
 * sections pinned to the bottom in configured order. Tabs that do not fit
 * above the section block are omitted. The rail has no Customize item; Cmd+K
 * and Settings open that popover. */
i32 app_side_rail_items(const AppState *app, f32 y, f32 h, SideRailItem *out, i32 cap) {
    f32 dpi = app->dpi_scale;
    f32 btn = SIDEBAR_RAIL_BTN_PT * dpi;
    f32 step = btn + 2.0f * dpi;
    i32 n = 0;
    #define RAIL_PUSH(k, i, yy, hh) do { if (out && n < cap) { out[n].kind = (k); out[n].index = (i); out[n].y = (yy); out[n].h = (hh); } n++; } while (0)
    f32 sec_h = 0.0f;
    for (i32 i = 0; i < SB_SEC_COUNT; i++)
        if (app->config.sidebar_enabled[i] && i != SB_SEC_TABS) sec_h += step;
    f32 tabs_limit = y + h - sec_h - 12.0f * dpi;

    SideTabRow rows[MAX_TABS + MAX_TAB_GROUPS + 1];
    i32 rn = app_side_tab_rows(app, rows, (i32)(sizeof rows / sizeof rows[0]));
    if (rn > (i32)(sizeof rows / sizeof rows[0])) rn = (i32)(sizeof rows / sizeof rows[0]);
    f32 cy = y + 4.0f * dpi;
    i32 prev_group = -1;
    for (i32 i = 0; i < rn; i++) {
        SideTabRow row = rows[i];
        if (row.kind == SIDE_ROW_GROUP) {
            if (cy + 6.0f * dpi > tabs_limit) break;
            RAIL_PUSH(SIDE_ROW_GROUP, row.index, cy, 6.0f * dpi);
            cy += 6.0f * dpi;
            prev_group = row.index;
            continue;
        }
        if (row.kind == SIDE_ROW_TAB && prev_group >= 0 &&
            app->tabs[row.index].group_index != prev_group) {
            RAIL_PUSH(SIDE_ROW_GROUP, -1, cy, 6.0f * dpi);   /* end-of-group divider */
            cy += 6.0f * dpi;
            prev_group = -1;
        }
        if (cy + btn > tabs_limit) break;
        RAIL_PUSH(row.kind, row.index, cy, btn);
        cy += step;
    }
    f32 sy = y + h - sec_h - 6.0f * dpi;
    for (i32 k = 0; k < SB_SEC_COUNT; k++) {
        u8 id = app->config.sidebar_order[k];
        if (id >= SB_SEC_COUNT || id == SB_SEC_TABS || !app->config.sidebar_enabled[id]) continue;
        RAIL_PUSH(SIDE_RAIL_SECTION, id, sy, btn);
        sy += step;
    }
    #undef RAIL_PUSH
    return n;
}

/* The customize popover hangs off the sidebar it edits: right edge of the
 * bar (or its rail), bottom on the bar's bottom edge — captured once, on its
 * first frame, and held while it is open. It edits the very width it hangs off:
 * following the bar live would move the slider under the pointer scrubbing
 * it, so each move would read a different value at the same pixel and the
 * width would never settle. No visible bar (opened from the palette with the
 * sidebar hidden) → centred. */
static void sbc_live_anchor(const AppState *app, f32 *ax, f32 *ab) {
    bool bar = (app->sidebar_visible || app_sidebar_rail(app)) && app->sidebar_width > 0.5f;
    *ax = bar ? app->sidebar_width : -1.0f;
    *ab = (f32)app->fb_height - app->status_bar_height;
}

void app_sbc_anchor_capture(AppState *app) {
    sbc_live_anchor(app, &app->sbc_anchor_x, &app->sbc_anchor_bottom);
    app->sbc_anchor_set = true;
}

void app_sbc_layout(const AppState *app, SbcLayout *out) {
    SbcIn in;
    memset(&in, 0, sizeof in);
    in.win_w = (f32)app->fb_width;
    in.win_h = (f32)app->fb_height;
    in.dpi   = app->dpi_scale;
    in.top_min = app->tab_bar_height + 8.0f * app->dpi_scale;
    if (app->sbc_anchor_set) {
        in.anchor_x = app->sbc_anchor_x;
        in.anchor_bottom = app->sbc_anchor_bottom;
    } else {
        sbc_live_anchor(app, &in.anchor_x, &in.anchor_bottom);
    }
    in.rows = SB_SEC_COUNT;
    sbc_layout_compute(&in, out);
}

/* Move sidebar content between `AppState` and its owning tab, then zero the
 * source so `FileBrowser` allocations have only one owner and cannot be
 * destroyed twice.
 *
 * Visibility and width belong to the window; content belongs to the tab.
 * Copying visibility from a new or restored tab could hide or misplace the
 * sidebar during synchronization. */
void app_sidebar_stow(AppState *app, i32 index) {
    if (!app || index < 0 || index >= app->tab_count) return;
    Tab *t = &app->tabs[index];
    t->sidebar_fb       = app->filebrowser;
    /* list_paused freezes listing installs while a drag, menu or prompt is
     * open ON that browser. None can be open on a stowed one, and the pump
     * that clears the flag only ever touches the live browser — so a copy
     * stowed mid-menu would never poll its finished listing again. */
    t->sidebar_fb.list_paused = false;
    t->sidebar_mode     = app->sidebar_mode;
    t->sidebar_focused  = app->sidebar_focused;
    memset(&app->filebrowser, 0, sizeof app->filebrowser);
    app->sidebar_focused = false;
}

void app_sidebar_restore(AppState *app, i32 index) {
    if (!app || index < 0 || index >= app->tab_count) return;
    Tab *t = &app->tabs[index];
    app->filebrowser     = t->sidebar_fb;
    app->sidebar_mode    = t->sidebar_mode;
    app->sidebar_focused = t->sidebar_focused;
    memset(&t->sidebar_fb, 0, sizeof t->sidebar_fb);
    /* A document that comes back with its tab does not take the keyboard:
     * switching to a tab is going to its terminal. A click in the document, or
     * opening a file, hands it back (FileBrowser.kbd_away). */
    app->filebrowser.kbd_away = true;
}

void app_sidebar_sync(AppState *app) {
    if (!app || app->sidebar_owner_tab == app->active_tab) return;
    app_sidebar_stow(app, app->sidebar_owner_tab);   /* -1 → no-op */
    app_sidebar_restore(app, app->active_tab);
    app->sidebar_owner_tab = app->active_tab;
}

/* Every FileBrowser this window owns, exactly once.
 *
 * Three places hold one: AppState.filebrowser (the sidebar's content while the
 * ACTIVE tab owns it), Tab.fb (a file-browser or graph tab's own), and
 * Tab.sidebar_fb (the sidebar's content parked in a tab that does NOT own the
 * bar right now). app_sidebar_sync MOVES the sidebar's content between the
 * first and the third and ZEROES whichever side is not holding it, so visiting
 * all three cannot double count — the vacated side has a zero view_cap and no
 * caches, and every visitor already treats a zeroed browser as empty.
 *
 * Every consumer that has to see "all the browsers in this process" — the
 * memory accounting, the pressure reclaimers, the completion pump — goes
 * through here, so an explorer stowed in another tab still has its finished
 * listings, markdown parses and thumbnails consumed and its editor buffer
 * counted in the ledger, and none of them can drift about what is owned. */
void app_for_each_browser_in(AppState *a, void (*fn)(FileBrowser *, void *), void *ctx) {
    if (!a || !fn) return;
    fn(&a->filebrowser, ctx);
    for (i32 t = 0; t < a->tab_count; t++) {
        if (a->tabs[t].fb) fn(a->tabs[t].fb, ctx);
        fn(&a->tabs[t].sidebar_fb, ctx);
        /* An SFTP tab's two panes list through browsers of their own. */
        if (a->tabs[t].kind == TAB_SFTP && a->tabs[t].sftp) {
            fn(&a->tabs[t].sftp->pane[0].fb, ctx);
            fn(&a->tabs[t].sftp->pane[1].fb, ctx);
        }
    }
}
