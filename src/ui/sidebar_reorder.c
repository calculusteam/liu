/*
 * Liu — moving a sidebar section by dragging its header: the event half.
 *
 * The arithmetic (which slot the pointer is over, where its line is drawn,
 * what the drop does to the configured order, the slide that follows) is
 * ui/sidebar_layout.h; the painter is render_sidebar() in ui.c. This file
 * owns what a press, a move, a release and Esc do:
 *   - A press arms. A release before the pointer has moved
 *     SIDEBAR_DRAG_SLOP_PT is the press's click: a header folds or opens its
 *     section, a rail button asks the caller to unfold the bar on its
 *     section (the explorer's breadcrumb has no click of its own).
 *   - Past the slop it is a drag: the painter lifts the header (or the rail
 *     button) under the pointer and draws the line where it would land.
 *   - The release drops it there and writes `sidebar_sections`, the same
 *     order the Customize popover edits, and the sections glide to their new
 *     places. The explorer reorders only when it is released inside the bar;
 *     outside, its breadcrumb drag detaches it into a tab (main.c), and that
 *     gesture is left alone.
 *   - Esc forgets the press, whatever it would have become.
 *
 * On the rail the section buttons stand for the sections: they are measured
 * as a stack of headers with no bodies, so the bar and the rail share every
 * rule of the layout header.
 */
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/sidebar_layout.h"
#include "platform/platform.h"

#include <string.h>

void app_side_drag_cancel(AppState *app) {
    if (!app) return;
    app->side_drag_armed = false;
    app->side_drag_live = false;
    app->side_drag_toggles = false;
    app->side_drag_rail = false;
    app->side_drag_id = -1;
}

void app_side_drag_stack(const AppState *app, SidebarLayout *out) {
    app_sidebar_layout(app, out);
    if (!out->rail) return;
    f32 top = app->tab_bar_height;
    f32 h = (f32)app->fb_height - top - app->status_bar_height;
    SideRailItem items[MAX_TABS + 2 * MAX_TAB_GROUPS + SB_SEC_COUNT + 2];
    i32 n = app_side_rail_items(app, top, h, items, (i32)(sizeof items / sizeof items[0]));
    if (n > (i32)(sizeof items / sizeof items[0])) n = (i32)(sizeof items / sizeof items[0]);
    SidebarLayout R;
    memset(&R, 0, sizeof R);
    R.rail = true;
    R.row_h = out->row_h;
    R.hdr_h = SIDEBAR_RAIL_BTN_PT * app->dpi_scale;
    R.bottom = top + h;
    for (i32 i = 0; i < n && R.n < SB_SEC_COUNT; i++) {
        if (items[i].kind != SIDE_RAIL_SECTION) continue;
        if (items[i].index < 0 || items[i].index >= SB_SEC_COUNT) continue;
        SidebarSectionRect *s = &R.sec[R.n++];
        s->id = (SidebarSection)items[i].index;
        s->enabled = true;
        s->open = true;
        s->hdr_y = items[i].y;
        s->hdr_h = items[i].h;
        s->body_y = items[i].y + items[i].h;
    }
    *out = R;
}

/* The height of the copy that follows the pointer: a header's, or a rail
 * button's. */
static f32 side_drag_lift_h(const AppState *app) {
    return (app->side_drag_rail ? SIDEBAR_RAIL_BTN_PT : SIDE_DRAG_LIFT_H_PT) * app->dpi_scale;
}

void app_side_drag_press(AppState *app, i32 section, f32 x, f32 y, bool toggles) {
    if (!app) return;
    app_side_drag_cancel(app);
    if (section < 0 || section >= SB_SEC_COUNT) return;
    SidebarLayout S;
    app_side_drag_stack(app, &S);
    i32 at = sidebar_layout_index(&S, section);
    if (at < 0 || !sidebar_sec_drawn(&S.sec[at])) return;
    app->side_drag_armed = true;
    app->side_drag_toggles = toggles;
    app->side_drag_rail = S.rail;
    app->side_drag_id = section;
    app->side_drag_x0 = app->side_drag_x = x;
    app->side_drag_y0 = app->side_drag_y = y;
    app->side_drag_grab = y - S.sec[at].hdr_y;
}

bool app_side_drag_move(AppState *app, f32 x, f32 y) {
    if (!app || !app->side_drag_armed) return false;
    app->side_drag_x = x;
    app->side_drag_y = y;
    if (!app->side_drag_live) {
        f32 dx = x - app->side_drag_x0, dy = y - app->side_drag_y0;
        f32 slop = SIDEBAR_DRAG_SLOP_PT * app->dpi_scale;
        if (dx * dx + dy * dy <= slop * slop) return false;
        app->side_drag_live = true;
    }
    return true;
}

/* The explorer's breadcrumb drag means "detach" once it leaves the bar, so
 * its drop counts only inside it; anything else is placed by its height
 * alone, so a hand that drifts past the bar's edge still drops where it
 * points. */
static bool side_drag_over_bar(const AppState *app) {
    if (app->side_drag_rail || app->side_drag_id != SB_SEC_FILES) return true;
    f32 top = app->tab_bar_height;
    f32 bottom = (f32)app->fb_height - app->status_bar_height;
    return app->side_drag_x >= 0.0f && app->side_drag_x < app->sidebar_width &&
           app->side_drag_y >= top && app->side_drag_y < bottom;
}

i32 app_side_drag_slot(const AppState *app, const SidebarLayout *stack) {
    if (!app || !stack || !app->side_drag_armed || !app->side_drag_live) return -1;
    /* The bar folded to its rail (or unfolded) under the drag: what was
     * taken is no longer there to drop. */
    if (stack->rail != app->side_drag_rail) return -1;
    if (sidebar_layout_index(stack, app->side_drag_id) < 0) return -1;
    if (!side_drag_over_bar(app)) return -1;
    return sidebar_drop_slot(stack, app->side_drag_y);
}

void app_side_slide_begin(AppState *app, i32 lead, f32 lead_y) {
    if (!app) return;
    SidebarLayout S;
    app_side_drag_stack(app, &S);
    sidebar_slide_begin(&app->side_slide, &S, lead, lead_y, platform_time_sec());
}

bool app_side_drag_release(AppState *app, f32 x, f32 y, i32 *rail_open) {
    if (rail_open) *rail_open = -1;
    if (!app || !app->side_drag_armed) return false;
    app->side_drag_x = x;
    app->side_drag_y = y;
    bool live = app->side_drag_live;
    i32 id = app->side_drag_id;
    if (live) {
        SidebarLayout S;
        app_side_drag_stack(app, &S);
        i32 slot = app_side_drag_slot(app, &S);
        i32 from = sidebar_layout_index(&S, id);
        if (slot >= 0 && sidebar_drop_moves(&S, from, slot)) {
            /* The section settles from where its lifted copy was let go. */
            f32 lead_y = sidebar_lift_top(y, app->side_drag_grab, side_drag_lift_h(app),
                                          app->tab_bar_height, S.bottom);
            sidebar_slide_begin(&app->side_slide, &S, id, lead_y, platform_time_sec());
            /* Saved, and given to the other windows, by the loop
             * (liu_config_commit), like every other change to the sidebar. */
            if (sidebar_order_drop(app->config.sidebar_order, &S, id, slot))
                app->config_commit_pending = true;
        }
    } else if (app->side_drag_toggles) {
        if (app->side_drag_rail) {
            if (rail_open) *rail_open = id;
        } else {
            if (!app->sidebar_sec_inited) {
                for (i32 i = 0; i < SB_SEC_COUNT; i++) app->sidebar_sec_open[i] = true;
                app->sidebar_sec_inited = true;
            }
            app->sidebar_sec_open[id] = !app->sidebar_sec_open[id];
        }
    }
    bool repaint = live || app->side_drag_toggles;
    app_side_drag_cancel(app);
    return repaint;
}

bool app_side_drag_escape(AppState *app) {
    if (!app || !app->side_drag_armed) return false;
    if (app->side_drag_id == SB_SEC_FILES && !app->side_drag_rail) {
        app->fb_header_drag_pending = false;
        app->fb_header_drag_active = false;
    }
    app_side_drag_cancel(app);
    return true;
}
