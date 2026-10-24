/*
 * Liu — centralized UI hit testing
 * All pixel math uses layout.h constants.
 */
#include "ui/hittest.h"
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/icon.h"
#include "ui/filebrowser.h"
#include "ui/browser_ui.h"
#include "ui/sftp_tab.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    f32 x, y, w, h;
} HitPaneRect;

static void hit_split_layout_node(const Tab *tab, i32 node, HitPaneRect r,
                                  f32 dpi, HitPaneRect out[MAX_SPLIT_PANES]) {
    if (!tab || node < 0 || node >= tab->split_next_node) return;
    const SplitLayoutNode *n = &tab->split_nodes[node];
    if (!n->used) return;
    if (n->leaf) {
        if (n->pane >= 0 && n->pane < MAX_SPLIT_PANES) out[n->pane] = r;
        return;
    }
    f32 ratio = n->ratio;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    /* Inter-pane gap — must match tab_split_layout_node()'s 6*dpi, which is
     * what the renderer lays panes out with. A smaller constant shifts every
     * box after the first split and the error compounds per nesting level. */
    f32 div = 6.0f * dpi;
    if (n->split == SPLIT_V) {
        f32 split = r.h * ratio;
        hit_split_layout_node(tab, n->first,
                              (HitPaneRect){r.x, r.y, r.w, split - div * 0.5f},
                              dpi, out);
        hit_split_layout_node(tab, n->second,
                              (HitPaneRect){r.x, r.y + split + div * 0.5f,
                                            r.w, r.h - split - div * 0.5f},
                              dpi, out);
    } else {
        f32 split = r.w * ratio;
        hit_split_layout_node(tab, n->first,
                              (HitPaneRect){r.x, r.y, split - div * 0.5f, r.h},
                              dpi, out);
        hit_split_layout_node(tab, n->second,
                              (HitPaneRect){r.x + split + div * 0.5f, r.y,
                                            r.w - split - div * 0.5f, r.h},
                              dpi, out);
    }
}

static i32 hit_split_pane_at(const Tab *tab, f32 x, f32 y,
                             f32 term_x, f32 term_y, f32 term_w, f32 term_h,
                             f32 dpi, HitPaneRect *out_rect) {
    if (!tab || tab->split == SPLIT_NONE || tab->split_root < 0) return 0;
    HitPaneRect rects[MAX_SPLIT_PANES];
    for (i32 i = 0; i < MAX_SPLIT_PANES; i++) rects[i] = (HitPaneRect){0};
    hit_split_layout_node(tab, tab->split_root,
                          (HitPaneRect){term_x, term_y, term_w, term_h},
                          dpi, rects);
    i32 count = tab->split_pane_count;
    if (count < 2) count = 2;
    if (count > MAX_SPLIT_PANES) count = MAX_SPLIT_PANES;
    for (i32 i = 0; i < count; i++) {
        HitPaneRect pr = rects[i];
        if (x >= pr.x && x < pr.x + pr.w && y >= pr.y && y < pr.y + pr.h) {
            if (out_rect) *out_rect = pr;
            return i;
        }
    }
    return 0;
}

static bool hit_split_divider_node(const Tab *tab, i32 node, HitPaneRect r,
                                   f32 x, f32 y, f32 dpi, f32 grab,
                                   i32 *out_node) {
    if (!tab || node < 0 || node >= tab->split_next_node) return false;
    const SplitLayoutNode *n = &tab->split_nodes[node];
    if (!n->used || n->leaf) return false;

    f32 ratio = n->ratio;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    f32 div = 6.0f * dpi;   /* matches tab_split_layout_node / the renderer */

    if (n->split == SPLIT_V) {
        f32 split = r.h * ratio;
        f32 div_y = r.y + split;
        if (y >= div_y - grab && y <= div_y + grab &&
            x > r.x && x < r.x + r.w) {
            if (out_node) *out_node = node;
            return true;
        }
        HitPaneRect first = {r.x, r.y, r.w, split - div * 0.5f};
        HitPaneRect second = {r.x, r.y + split + div * 0.5f,
                              r.w, r.h - split - div * 0.5f};
        if (hit_split_divider_node(tab, n->first, first, x, y, dpi, grab, out_node)) return true;
        if (hit_split_divider_node(tab, n->second, second, x, y, dpi, grab, out_node)) return true;
    } else {
        f32 split = r.w * ratio;
        f32 div_x = r.x + split;
        if (x >= div_x - grab && x <= div_x + grab &&
            y > r.y && y < r.y + r.h) {
            if (out_node) *out_node = node;
            return true;
        }
        HitPaneRect first = {r.x, r.y, split - div * 0.5f, r.h};
        HitPaneRect second = {r.x + split + div * 0.5f, r.y,
                              r.w - split - div * 0.5f, r.h};
        if (hit_split_divider_node(tab, n->first, first, x, y, dpi, grab, out_node)) return true;
        if (hit_split_divider_node(tab, n->second, second, x, y, dpi, grab, out_node)) return true;
    }

    return false;
}

/* The vertical band the terminal body occupies. Kept here, next to the hit
 * test that derives every row from it, because a second copy of this
 * arithmetic somewhere else drifts the moment the toolbar or the status bar
 * changes height — and the drag-to-scroll gate asks the same question the hit
 * test does ("is the pointer above or below the grid?"). */
void ui_terminal_vspan(const struct AppState *app, f32 *out_top, f32 *out_bottom) {
    f32 dpi = app->dpi_scale;
    if (out_top)
        *out_top = app->tab_bar_height + app->config.style.terminal_top_gap * dpi;
    if (out_bottom)
        *out_bottom = (f32)app->fb_height - app->status_bar_height -
                      TERM_GRID_MIN_VPAD_PT * dpi;
}

/* The horizontal band, same reason — and the ONE place the sidebar's slide is
 * accounted for. `sidebar_width` is the bar's live edge, eased over ~0.22 s by
 * app_render's slide; `layout_sidebar_w` is the width the grid was last laid
 * out AGAINST (app_update_layout records it, and app_render relayouts the
 * moment the two differ while the slide is at rest). So they differ only
 * mid-slide, and there the grid must keep the width it was SIZED for and move
 * with the edge as one body: the toggle handlers lay it out for the
 * destination before the slide starts, and it translates into place.
 *
 * Deriving the box from the live edge would re-centre a fixed column count
 * (term_grid_pad_x) inside a box that grows every frame, so the prompt would
 * drift sideways over the slide and snap back when the relayout lands. The hit
 * test takes the same box, so a click mid-slide lands on the cell being drawn.
 */
void ui_terminal_hspan(const struct AppState *app, f32 *out_left, f32 *out_w) {
    f32 left  = app->sidebar_width + app->padding;
    f32 sized = app->layout_sidebar_w + app->padding;
    if (out_left) *out_left = left;
    if (out_w) {
        f32 w = (f32)app->fb_width - sized - app->padding - app_right_reserve(app);
        *out_w = w > 0.0f ? w : 0.0f;
    }
}

bool ui_terminal_pane_cell(const struct AppState *app, i32 pane, f32 x, f32 y,
                           i32 *out_col, i32 *out_row, bool *out_inside,
                           f32 *out_top, f32 *out_bottom) {
    return ui_terminal_pane_point(app, pane, x, y, out_col, out_row, NULL,
                                  out_inside, out_top, out_bottom);
}

/* One pane's place in the terminal body: its slice of it, and where its grid
 * sits inside that slice. The projection below and the pane's scrollbar both
 * start from here, so the bar cannot drift from the cells it scrolls. */
typedef struct {
    const Terminal *pt;
    f32 pane_x, pane_y, pane_w, pane_h;   /* the pane's slice of the body */
    f32 gx, gy;                           /* grid origin, centred in the slice */
    f32 cw, ch;
} PaneGeom;

static bool pane_geom(const struct AppState *app, i32 pane, PaneGeom *out) {
    if (!app || pane < 0 || pane >= MAX_SPLIT_PANES) return false;
    const Tab *atab = (app->tab_count > 0 && app->active_tab >= 0)
                      ? &app->tabs[app->active_tab] : NULL;
    if (!atab) return false;
    const Terminal *pt = app_tab_pane_terminal(atab, pane);
    f32 cw = app->renderer.font.cell_width;
    f32 ch = app->renderer.font.cell_height;
    if (!pt || pt->cols <= 0 || pt->rows <= 0 || cw <= 0.0f || ch <= 0.0f)
        return false;

    /* The terminal body, then the pane's slice of it — the same steps
     * ui_hit_test takes on its way to HIT_TERMINAL. */
    f32 dpi = app->dpi_scale;
    f32 origin_x, term_w;
    ui_terminal_hspan(app, &origin_x, &term_w);
    f32 origin_y, term_bottom;
    ui_terminal_vspan(app, &origin_y, &term_bottom);
    f32 term_h = term_bottom - origin_y;
    f32 pane_w = term_w;
    f32 pane_h = term_h;
    if (atab->split != SPLIT_NONE && atab->split_root >= 0) {
        HitPaneRect rects[MAX_SPLIT_PANES];
        for (i32 i = 0; i < MAX_SPLIT_PANES; i++) rects[i] = (HitPaneRect){0};
        hit_split_layout_node(atab, atab->split_root,
                              (HitPaneRect){origin_x, origin_y, term_w, term_h},
                              dpi, rects);
        if (rects[pane].w <= 0.0f || rects[pane].h <= 0.0f) return false;
        origin_x = rects[pane].x;
        origin_y = rects[pane].y;
        pane_w   = rects[pane].w;
        pane_h   = rects[pane].h;
    } else if (pane != 0) {
        return false;
    }
    out->pt = pt;
    out->pane_x = origin_x;
    out->pane_y = origin_y;
    out->pane_w = pane_w;
    out->pane_h = pane_h;
    out->cw = cw;
    out->ch = ch;
    /* The grid is centred in its pane on BOTH axes — the renderer offsets by
     * the same two, so a projection that took only one lands the drag half a
     * column off at the left edge of the pane. */
    out->gx = origin_x + term_grid_pad_x(pane_w, pt->cols, cw);
    out->gy = origin_y + term_grid_pad_y(pane_h, pt->rows, ch);
    return true;
}

bool ui_terminal_pane_scrollbar(const struct AppState *app, i32 pane,
                                f32 *out_right, f32 *out_top, f32 *out_bottom) {
    PaneGeom g;
    if (!pane_geom(app, pane, &g)) return false;
    if (out_right)  *out_right  = g.pane_x + g.pane_w;
    if (out_top)    *out_top    = g.gy;
    if (out_bottom) *out_bottom = g.gy + (f32)g.pt->rows * g.ch;
    return true;
}

bool ui_terminal_pane_point(const struct AppState *app, i32 pane, f32 x, f32 y,
                            i32 *out_col, i32 *out_row, bool *out_right_half,
                            bool *out_inside, f32 *out_top, f32 *out_bottom) {
    PaneGeom pg;
    if (!pane_geom(app, pane, &pg)) return false;
    const Terminal *pt = pg.pt;
    f32 cw = pg.cw, ch = pg.ch;
    f32 origin_x = pg.gx;
    f32 origin_y = pg.gy;
    f32 grid_right  = origin_x + (f32)pt->cols * cw;
    f32 grid_bottom = origin_y + (f32)pt->rows * ch;

    /* floorf, not a cast: a cast truncates toward zero, so a point half a
     * cell above the grid reads as row 0 rather than -1. The clamp below
     * would hide that here, but `inside` must not. */
    f32 fcol = (x - origin_x) / cw;
    i32 col = (i32)floorf(fcol);
    i32 row = (i32)floorf((y - origin_y) / ch);
    bool inside = x >= origin_x && x < grid_right &&
                  y >= origin_y && y < grid_bottom;
    /* Nearest edge: the right half of a cell rounds to the edge after it.
     * Past either side of the grid the clamp below pins the cell, and the
     * half follows the side — left of the grid is the grid's first edge,
     * right of it the last. */
    bool right_half = (fcol - floorf(fcol)) >= 0.5f;
    if (col < 0) right_half = false;
    if (col >= pt->cols) right_half = true;
    if (out_right_half) *out_right_half = right_half;
    if (col < 0) col = 0;
    if (col >= pt->cols) col = pt->cols - 1;
    if (row < 0) row = 0;
    if (row >= pt->rows) row = pt->rows - 1;

    if (out_col)    *out_col = col;
    if (out_row)    *out_row = row;
    if (out_inside) *out_inside = inside;
    if (out_top)    *out_top = origin_y;
    if (out_bottom) *out_bottom = grid_bottom;
    return true;
}

FindBarGeom ui_find_bar_geom(const struct AppState *app) {
    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    /* The bar's text runs at the fixed 8x16 pt UI cell (render_search). */
    f32 cw = 8.0f * dpi, ch = 16.0f * dpi;
    FindRect bar = find_bar_rest_rect((f32)app->fb_width, app->tab_bar_height, dpi);
    return find_bar_layout(bar, dpi, cw, ch);
}

HitResult ui_hit_test(const struct AppState *app, f32 x, f32 y) {
    HitResult r = { .type = HIT_NONE, .index = -1, .sub_index = -1 };
    f32 dpi = app->dpi_scale;

    /* The Find bar floats over the top of the terminal body: it claims its
     * own rect first, or a press on a toggle would start a selection in the
     * grid underneath. */
    if (app->search_active) {
        FindBarGeom fg = ui_find_bar_geom(app);
        FindBarPart part = find_bar_part_at(&fg, x, y);
        if (part != FIND_PART_NONE) {
            r.type = HIT_FIND_BAR;
            r.sub_index = (i32)part;
            r.rel_x = x - fg.bar.x;
            r.rel_y = y - fg.bar.y;
            return r;
        }
    }
    bool docked     = app_sidebar_tabs_docked(app);
    bool show_tabs  = app->config.show_tab_bar && !docked;
    bool show_icons = app->config.show_toolbar_icons;
    /* Sidebar / viewer math must use the SAME top origin as render_sidebar,
     * which places its content at y = app->tab_bar_height. Toolbar region
     * check below still uses the nominal TOOLBAR_HEIGHT_PT so icon/tab hit
     * zones stay aligned with render_toolbar. */
    f32 tb_h = (show_tabs || show_icons) ? (TOOLBAR_HEIGHT_PT * dpi) : 0.0f;
    f32 sidebar_top = app->tab_bar_height;
    f32 sb_w = app->sidebar_width;
    f32 icon_sz = TB_ICON_SIZE_PT * dpi;
    f32 status_y = (f32)app->fb_height - app->status_bar_height;

    /* macOS drag strip: when both tab bar and toolbar icons are toggled off,
     * `app->tab_bar_height` still reserves a small empty band at the top so
     * the window remains draggable (set in app_update_layout). Without this
     * early-out the band would fall through to HIT_TERMINAL below, the
     * mouse-down handler would treat the click as terminal input, and
     * `platform_begin_window_drag` would never fire — i.e. the window
     * could not be moved unless the user re-enabled the tab bar. */
    if (tb_h == 0.0f && app->tab_bar_height > 0.0f && y < app->tab_bar_height) {
        return r;   /* HIT_NONE → window drag */
    }

    /* ---- Toolbar region (top 28pt) ---- */
    if (tb_h > 0 && y < tb_h) {
        /* Same left cursor render_toolbar starts from — zero in fullscreen,
         * where the traffic-light zone is not reserved. */
        f32 bx = app->titlebar_left_inset;

#ifndef PLATFORM_MACOS
        /* Liu's own window controls at the far right (X11), through the
         * geometry render_window_controls paints. */
        if (app->titlebar_right_inset > 0.0f) {
            f32 box = WIN_CTL_BTN_PT * dpi;
            f32 cy  = roundf(ui_bar_mid_px(tb_h) - box * 0.5f);
            for (i32 i = 0; i < WIN_CTL_COUNT; i++) {
                f32 cx = roundf(ui_win_ctl_x(i, (f32)app->fb_width, dpi));
                if (x >= cx && x < cx + box && y >= cy && y < cy + box) {
                    r.type = HIT_TOOLBAR_BTN;
                    r.sub_index = i == 0 ? TB_WIN_CLOSE : i == 1 ? TB_WIN_MAXIMIZE
                                                                 : TB_WIN_MINIMIZE;
                    return r;
                }
            }
        }
#endif

        /* Update-available chip — far right; rect published by
         * render_toolbar (w == 0 while hidden). Checked first: it sits
         * outside the tab viewport, and it must work with tabs hidden. */
        if (app->_upd_btn_w > 0.0f && x >= app->_upd_btn_x &&
            x < app->_upd_btn_x + app->_upd_btn_w) {
            r.type = HIT_TOOLBAR_BTN;
            r.sub_index = TB_UPDATE;
            return r;
        }

        /* Broadcast indicator chip — sits left of the update chip; same
         * outside-the-tab-viewport reasoning. Click disables broadcast. */
        if (app->_bcast_btn_w > 0.0f && x >= app->_bcast_btn_x &&
            x < app->_bcast_btn_x + app->_bcast_btn_w) {
            r.type = HIT_TOOLBAR_BTN;
            r.sub_index = TB_BROADCAST;
            return r;
        }

        /* Secure-input chip — left of the other two, same reasoning. A click
         * explains what it means. */
        if (app->_sec_btn_w > 0.0f && x >= app->_sec_btn_x &&
            x < app->_sec_btn_x + app->_sec_btn_w) {
            r.type = HIT_TOOLBAR_BTN;
            r.sub_index = TB_SECURE_INPUT;
            return r;
        }

        /* Sidebar toggle button (part of the action-icons cluster). Uses the
         * enlarged sidebar box so the click target matches render_toolbar. */
        f32 sb_box = ui_icon_box_px(TB_SIDEBAR_ICON_SIZE_PT, dpi);
        /* Exactly the box render_toolbar paints the hover pill on, so the
         * control never reacts where it does not light up. */
        if (show_icons && x >= bx && x < bx + sb_box) {
            r.type = HIT_TOOLBAR_BTN;
            r.sub_index = TB_SIDEBAR;
            return r;
        }
        /* Past the separator, through the same helper the painter uses. */
        if (show_icons) bx = tb_after_sep_x(bx + sb_box, dpi);

        /* Docked tabs: the strip is empty but its "+" survived at the right
         * end (render_toolbar publishes the rect). */
        if (docked && app->_dock_new_btn_w > 0.0f && x >= app->_dock_new_btn_x &&
            x < app->_dock_new_btn_x + app->_dock_new_btn_w) {
            r.type = HIT_TAB_NEW;
            return r;
        }

        /* When the tab strip is hidden, the toolbar still exists for icons
         * (sidebar toggle, resource monitor) but the tab/group area is not
         * drawn — drop out of the toolbar block here so a click on those
         * pixels falls through to no-hit (window drag region) instead of
         * activating an invisible tab. */
        if (!show_tabs) {
            return r;
        }

        /* Tabs area — mirror render_toolbar()'s horizontally-scrolling viewport.
         * Reuse the geometry it publishes (tab_w / gap / scroll offset / clip
         * window / pinned "+") rather than recomputing, so hit boxes track
         * exactly where the pills paint after scrolling. */
        f32 tab_gap    = app->_tab_gap > 0.0f ? app->_tab_gap : TAB_GAP_PT * dpi;
        f32 tab_w      = app->_tab_w   > 0.0f ? app->_tab_w   : TAB_MIN_W_PT * dpi;
        /* A tab showing several agent icons is painted wider than the base
         * width; render_toolbar publishes that surplus per tab. Zero for the
         * usual tab, and zero before the first paint, which degrades to a
         * uniform-width walk. */
        #define TAB_SLOT_W(idx) (tab_w + (((idx) >= 0 && (idx) < app->tab_count) \
                                          ? app->_tab_extra_w[(idx)] : 0.0f))
        f32 tab_scroll = app->_tab_scroll_x;
        f32 view_x     = app->_tab_view_x > 0.0f ? app->_tab_view_x : bx;
        f32 view_w     = app->_tab_view_w > 0.0f ? app->_tab_view_w
                                                 : ((f32)app->fb_width - bx);
        /* Tabs are clipped to [view_x, view_x+view_w]; a click outside that band
         * (the scrolled-off region, or the pinned "+" slot) must not match a
         * tab even though the walk position math would otherwise land on one. */
        bool in_strip = (x >= view_x && x < view_x + view_w);
        /* The ✕ is optically anchored (ui_tab_close_x); its click zone has to
         * move with the glyph, so probe the same ink inset the renderer uses.
         * Memoised per (kind, size) — cheap enough for a per-event hit test. */
        f32 close_ink_r = 0.0f;
        icon_ink_insets(ICON_CLOSE,
                        TAB_CLOSE_SIZE_PT * dpi * TAB_CLOSE_ICON_RATIO,
                        NULL, &close_ink_r);

        /* Walk through tabs in the same order as render_toolbar (grouped tabs
         * with their headers, then ungrouped), offset left by the scroll.
         * The strip origin is whatever render_toolbar published (it varies with
         * the traffic-light zone, which collapses in fullscreen); `bx` is only
         * the pre-first-paint fallback. */
        f32 strip_x = app->_tab_bar_x > 0.0f ? app->_tab_bar_x : bx;
        f32 cur_x = strip_x - tab_scroll;

        /* Grouped tabs */
        for (i32 gi = 0; gi < MAX_TAB_GROUPS; gi++) {
            if (!app->tab_groups[gi].used) continue;
            bool has_tabs = false;
            for (i32 i = 0; i < app->tab_count; i++) {
                if (app->tabs[i].group_index == gi) { has_tabs = true; break; }
            }
            if (!has_tabs) continue;

            /* Group header chip */
            f32 chip_w = app_get_group_chip_width(app, gi);
            if (in_strip && x >= cur_x && x < cur_x + chip_w) {
                r.type = HIT_TAB_GROUP_HEADER;
                r.index = gi;
                return r;
            }
            /* chip_w is the chip's SLOT: it already carries the gap between
             * the chip and its first member (GROUP_CHIP_GAP_PT), so a click in
             * that gap is the chip's. Members then pack against each other; one
             * tab_gap after the whole group — mirror render_toolbar() exactly. */
            cur_x += chip_w;

            /* Tabs in this group (if not collapsed) */
            if (!app->tab_groups[gi].collapsed) {
                for (i32 i = 0; i < app->tab_count; i++) {
                    if (app->tabs[i].group_index != gi) continue;
                    f32 slot_w = TAB_SLOT_W(i);
                    if (in_strip && x >= cur_x && x < cur_x + slot_w) {
                        /* A group member's pill is inset GROUP_MEMBER_INSET_X_PT
                         * in its slot (render_toolbar), not PILL_INSET_X_PT. */
                        f32 close_x = ui_tab_close_x(cur_x + GROUP_MEMBER_INSET_X_PT * dpi,
                                                     slot_w - 2.0f * GROUP_MEMBER_INSET_X_PT * dpi,
                                                     TAB_CLOSE_SIZE_PT * dpi, close_ink_r, dpi);
                        /* Suppress the close zone on narrow tabs (active too) — mirror render. */
                        bool close_room = slot_w >= TAB_CLOSE_MIN_W_PT * dpi;
                        if (close_room && x >= close_x) {
                            r.type = HIT_TAB_CLOSE;
                            r.index = i;
                        } else {
                            r.type = HIT_TAB;
                            r.index = i;
                        }
                        return r;
                    }
                    cur_x += slot_w;
                }
            }
            cur_x += tab_gap;
        }

        /* Ungrouped tabs */
        for (i32 i = 0; i < app->tab_count; i++) {
            i32 gi = app->tabs[i].group_index;
            if (gi >= 0 && gi < MAX_TAB_GROUPS && app->tab_groups[gi].used) continue;
            f32 slot_w = TAB_SLOT_W(i);
            if (in_strip && x >= cur_x && x < cur_x + slot_w) {
                f32 close_x = ui_tab_close_x(cur_x + PILL_INSET_X_PT * dpi,
                                             slot_w - 2.0f * PILL_INSET_X_PT * dpi,
                                             TAB_CLOSE_SIZE_PT * dpi, close_ink_r, dpi);
                /* Suppress the close zone on narrow tabs (active too) — mirror render. */
                bool close_room = slot_w >= TAB_CLOSE_MIN_W_PT * dpi;
                if (close_room && x >= close_x) {
                    r.type = HIT_TAB_CLOSE;
                    r.index = i;
                } else {
                    r.type = HIT_TAB;
                    r.index = i;
                }
                return r;
            }
            cur_x += slot_w + tab_gap;
        }
        #undef TAB_SLOT_W

        /* New tab (+) button — at the pinned position render_toolbar() used
         * (it clamps to the viewport's right edge when the strip overflows). */
        f32 plus_x = app->_tab_plus_x > 0.0f ? app->_tab_plus_x : cur_x;
        if (show_tabs && x >= plus_x && x < plus_x + 24.0f * dpi) {
            r.type = HIT_TAB_NEW;
            return r;
        }

        /* Right-side action-icons hit zone removed alongside the matching
         * render block in ui.c. The resource monitor is gone too; the only
         * right-side element left is the update chip, handled above. */
        (void)icon_sz;

        return r;
    }

    /* ---- Status bar ---- */
    if (y >= status_y) {
        /* Git segment — rect published by render_status_bar (w == 0 while it
         * isn't painted). It's the only clickable thing in the bar; everything
         * else there is a read-only readout. */
        if (app->_git_btn_w > 0.0f && x >= app->_git_btn_x &&
            x < app->_git_btn_x + app->_git_btn_w) {
            r.type = HIT_STATUS_GIT;
            return r;
        }
        r.type = HIT_STATUS_BAR;
        return r;
    }

    /* ---- Sidebar resize border (not on the rail: it has one width) ---- */
    bool rail = app_sidebar_rail(app);
    if (app->sidebar_visible && !rail) {
        f32 grab = SIDEBAR_RESIZE_GRAB_PT * dpi;
        if (x >= sb_w - grab && x <= sb_w + grab && y > sidebar_top) {
            r.type = HIT_SIDEBAR_RESIZE;
            return r;
        }
    }

    /* ---- Sidebar content: the same section rects render_sidebar painted ---- */
    if ((app->sidebar_visible || rail) && sb_w > 0.5f && x < sb_w && y >= sidebar_top) {
        SidebarLayout L;
        app_sidebar_layout(app, &L);
        r.rel_x = x;       /* sidebar origin is at x=0, so x == rel_x */
        r.rel_y = y - sidebar_top;

        if (L.rail) {
            f32 sb_h = (f32)app->fb_height - sidebar_top - app->status_bar_height;
            SideRailItem items[MAX_TABS + 2 * MAX_TAB_GROUPS + SB_SEC_COUNT + 2];
            i32 n = app_side_rail_items(app, sidebar_top, sb_h, items,
                                        (i32)(sizeof items / sizeof items[0]));
            if (n > (i32)(sizeof items / sizeof items[0])) n = (i32)(sizeof items / sizeof items[0]);
            for (i32 i = 0; i < n; i++) {
                if (y < items[i].y || y >= items[i].y + items[i].h) continue;
                switch (items[i].kind) {
                case SIDE_ROW_TAB:        r.type = HIT_TAB; r.index = items[i].index; break;
                case SIDE_ROW_NEW:        r.type = HIT_TAB_NEW; break;
                case SIDE_RAIL_SECTION:   r.type = HIT_SIDEBAR_SECTION; r.index = items[i].index; r.sub_index = 3; break;
                default:                  r.type = HIT_SIDEBAR; break;   /* a divider */
                }
                return r;
            }
            r.type = HIT_SIDEBAR;
            return r;
        }

        for (i32 i = 0; i < L.n; i++) {
            const SidebarSectionRect *s = &L.sec[i];
            /* Header: its actions are 22pt boxes packed from the right edge,
             * action 1 outermost (render_side_header draws the same packing
             * from app_side_section_actions). A header with no action has no
             * action zone — the whole band toggles. */
            if (s->hdr_h > 0.0f && y >= s->hdr_y && y < s->hdr_y + s->hdr_h) {
                f32 box = 22.0f * dpi;
                f32 a1_x = sb_w - 6.0f * dpi - box;   /* action 1 (outer) */
                f32 a2_x = a1_x - box;                 /* action 2 */
                i32 n = app_side_section_actions(s->id, NULL, NULL);
                r.type = HIT_SIDEBAR_SECTION;
                r.index = s->id;
                r.sub_index = (n >= 1 && x >= a1_x) ? 1 : (n >= 2 && x >= a2_x) ? 2 : 0;
                return r;
            }
            if (!s->open || s->body_h <= 0.0f || y < s->body_y || y >= s->body_y + s->body_h) continue;
            switch (s->id) {
            case SB_SEC_TABS: {
                SideTabRow rows[MAX_TABS + MAX_TAB_GROUPS + 1];
                i32 n = app_side_tab_rows(app, rows, (i32)(sizeof rows / sizeof rows[0]));
                if (n > (i32)(sizeof rows / sizeof rows[0])) n = (i32)(sizeof rows / sizeof rows[0]);
                i32 ri = sidebar_row_at(s, L.row_h, app->side_tabs_scroll, y, n);
                if (ri < 0) { r.type = HIT_SIDEBAR; return r; }
                SideTabRow row = rows[ri];
                if (row.kind == SIDE_ROW_GROUP) { r.type = HIT_TAB_GROUP_HEADER; r.index = row.index; return r; }
                if (row.kind == SIDE_ROW_NEW)   { r.type = HIT_TAB_NEW; return r; }
                /* Tab row: the close × zone is the pill's right padding, exactly
                 * where render_side_tab_row paints it (ui_tab_close_x). */
                r.type = HIT_TAB;
                r.index = row.index;
                f32 inset = 6.0f * dpi;
                bool grouped = false;
                {
                    i32 g = app->tabs[row.index].group_index;
                    grouped = g >= 0 && g < MAX_TAB_GROUPS && app->tab_groups[g].used &&
                              !app->tab_groups[g].collapsed;
                }
                f32 indent = grouped ? 12.0f * dpi : 0.0f;
                f32 px = inset + indent, pw = sb_w - 2.0f * inset - indent;
                f32 close_sz = app->config.style.tab_close_size * dpi;
                f32 ink_r = 0.0f;
                icon_ink_insets(ICON_CLOSE, close_sz * TAB_CLOSE_ICON_RATIO, NULL, &ink_r);
                f32 cx = ui_tab_close_x(px, pw, close_sz, ink_r, dpi);
                bool active = (row.index == app->active_tab);
                bool hovered = (app->_tab_hover_index == row.index);
                if ((active || hovered) && pw >= TAB_CLOSE_MIN_W_PT * dpi &&
                    x >= cx - 2.0f * dpi && x < cx + close_sz + 2.0f * dpi)
                    r.type = HIT_TAB_CLOSE;
                return r;
            }
            case SB_SEC_AGENTS: {
                i32 ri = (i32)((y - s->body_y) / (SIDEBAR_AGENT_ROW_PT * dpi));
                if (ri >= 0 && ri < app->side_agent_rows) {
                    r.type = HIT_SIDEBAR_AGENT;
                    r.index = app->side_agent_tab[ri];
                    r.sub_index = app->side_agent_pane[ri];
                } else r.type = HIT_SIDEBAR;
                return r;
            }
            case SB_SEC_NOTES: {
                i32 ri = (i32)((y - s->body_y) / (SIDEBAR_NOTE_ROW_PT * dpi));
                if (ri >= 0 && ri < app->side_notes.count) { r.type = HIT_SIDEBAR_NOTE; r.index = ri; }
                else r.type = HIT_SIDEBAR;
                return r;
            }
            case SB_SEC_FILES: {
                /* The explorer's rect starts at this section's body, not at the
                 * sidebar's top: rel_y is measured from there so fb's own header
                 * and row math (mirrored from fb_render_sidebar) hold. */
                f32 rel_y = y - s->body_y;
                f32 hdr_h = SIDEBAR_HEADER_PT * dpi;   /* matches fb_render_sidebar */
                /* sub_index 2 marks the explorer's BODY where no row is — under
                 * the last row, or all of it in an empty folder. A right click
                 * there opens the folder's own menu (main.c). */
                if (!app->filebrowser.entries) {
                    r.type = HIT_SIDEBAR;
                    if (rel_y >= hdr_h) r.sub_index = 2;
                    return r;
                }
                /* Use UI scale metrics, not terminal font — sidebar renders at fixed 16*dpi */
                f32 ch = 16.0f * dpi;
                f32 entry_h = FB_ENTRY_H(ch, dpi);   /* single source of truth: layout.h */
                r.rel_y = rel_y;
                /* sub_index 1 marks the explorer's HEADER row — main.c's up /
                 * refresh / close buttons live there and nowhere else. */
                if (rel_y < hdr_h) { r.type = HIT_SIDEBAR; r.sub_index = 1; return r; }
                /* Entry i top (in sidebar-local coords) = hdr_h + i*entry_h - scroll_offset_px */
                i32 idx = (i32)((rel_y - hdr_h + app->filebrowser.scroll_offset_px) / entry_h);
                if (idx >= 0 && idx < app->filebrowser.entry_count) {
                    r.type = HIT_SIDEBAR_ITEM;
                    r.index = idx;
                } else {
                    r.type = HIT_SIDEBAR;
                    r.sub_index = 2;
                }
                return r;
            }
            default: break;
            }
        }
        r.type = HIT_SIDEBAR;
        return r;
    }

    /* ---- File viewer ---- */
    if (app->filebrowser.view_mode != FVIEW_NONE) {
        /* Same clamp the panel is drawn with and the grid reserves against;
         * an independent copy here put the resize grab and the close button a
         * few pixels off whenever either limit bit. */
        f32 vw = app_file_viewer_w(app);
        f32 vx = (f32)app->fb_width - vw;

        /* Left-edge resize grab — sits just outside the panel so it doesn't
         * eat clicks meant for viewer content. */
        if (y >= sidebar_top && y < status_y) {
            f32 grab = VIEWER_RESIZE_GRAB_PT * dpi;
            if (x >= vx - grab && x <= vx + grab) {
                r.type = HIT_VIEWER_RESIZE;
                return r;
            }
        }

        if (x >= vx && y >= sidebar_top && y < status_y) {
            f32 vtitle_h = VIEWER_TITLE_H_PT * dpi;
            bool in_title = y < sidebar_top + vtitle_h;
            f32 close_w = VIEWER_CLOSE_W_PT * dpi;
            f32 close_x = vx + vw - close_w;
            if (in_title && x >= close_x) {
                r.type = HIT_VIEWER_CLOSE;
            } else if (in_title && fb_viewer_toggle_kind(&app->filebrowser) != FB_TOGGLE_NONE) {
                f32 toggle_w = VIEWER_TOGGLE_W_PT * dpi;
                f32 tx = close_x - 6.0f * dpi - toggle_w;
                if (x >= tx && x < tx + toggle_w) {
                    r.type = HIT_VIEWER_TOGGLE;
                } else {
                    r.type = HIT_VIEWER_TITLE;
                }
            } else if (in_title) {
                r.type = HIT_VIEWER_TITLE;
            } else {
                r.type = HIT_VIEWER;
            }
            return r;
        }
    }

    /* ---- Split divider ---- */
    {
        const Tab *atab = (app->tab_count > 0 && app->active_tab >= 0)
                          ? &app->tabs[app->active_tab] : NULL;
        if (atab && atab->split != SPLIT_NONE) {
            /* Mirror render_terminal's content box EXACTLY — the divider is
             * drawn at a ratio of this rect, so any disagreement moves the
             * grab zone off the painted line. The origin takes no
             * app->padding at the top, as the renderer applies none there,
             * and the width loses the git-diff dock's reserve, or with the
             * dock open the grab zone would sit right of the line by half the
             * dock. */
            f32 grab = 4.0f * dpi;
            f32 term_x = sb_w + app->padding;
            f32 term_y = app->tab_bar_height +
                         app->config.style.terminal_top_gap * dpi;
            f32 term_w = (f32)app->fb_width - term_x - app->padding -
                         app_right_reserve(app);
            f32 term_h = status_y - term_y - TERM_GRID_MIN_VPAD_PT * dpi;

            if (atab->split_root >= 0) {
                i32 node = -1;
                if (hit_split_divider_node(atab, atab->split_root,
                                           (HitPaneRect){term_x, term_y, term_w, term_h},
                                           x, y, dpi, grab, &node)) {
                    r.type = HIT_SPLIT_DIVIDER;
                    r.index = node;
                    return r;
                }
            } else if (atab->split == SPLIT_H) {
                f32 div_x = term_x + term_w * atab->split_ratio;
                if (x >= div_x - grab && x <= div_x + grab && y > term_y && y < term_y + term_h) {
                    r.type = HIT_SPLIT_DIVIDER;
                    return r;
                }
            } else if (atab->split == SPLIT_V) {
                f32 div_y = term_y + term_h * atab->split_ratio;
                if (y >= div_y - grab && y <= div_y + grab && x > term_x && x < term_x + term_w) {
                    r.type = HIT_SPLIT_DIVIDER;
                    return r;
                }
            }
        }
    }

    /* ---- Scroll-to-bottom button ----
     * Tested before the scrollbar and long before HIT_TERMINAL: the button
     * floats over the cell grid, so whichever check runs first owns the
     * click. The rects come straight from the renderer (app->scroll_btn[]),
     * so there is no pane geometry to re-derive here. */
    {
        i32 sb = app_scroll_btn_hit(app, x, y);
        if (sb >= 0) {
            r.type = HIT_SCROLL_BOTTOM;
            r.index = sb;
            return r;
        }
    }

    /* ---- Scrollbar ----
     * The same geometry the renderer paints (scrollbar_geom_at): every pane
     * carries its OWN bar, a column of SCROLLBAR_W_PT against the pane's right
     * edge (ui_terminal_pane_scrollbar), right of the git-diff dock and the
     * file viewer's reserve for a lone pane. `index` is the pane; the press
     * handler asks scrollbar_press() what the y means against its thumb. A pane
     * with no history paints no bar, so it claims no column either — the grid's
     * right edge stays clickable. */
    if (app->config.show_scrollbar) {
        f32 zone_w = SCROLLBAR_W_PT * dpi;
        const Tab *stab = (app->tab_count > 0 && app->active_tab >= 0)
                          ? &app->tabs[app->active_tab] : NULL;
        i32 npanes = !stab ? 0 : (stab->split != SPLIT_NONE ? MAX_SPLIT_PANES : 1);
        for (i32 p = 0; p < npanes; p++) {
            const Terminal *pt = app_tab_pane_terminal(stab, p);
            f32 right = 0.0f, band_top = 0.0f, band_bottom = 0.0f;
            if (!pt || pt->sb_count <= 0 ||
                !ui_terminal_pane_scrollbar(app, p, &right, &band_top, &band_bottom))
                continue;
            if (x >= right - zone_w && x < right && y >= band_top && y < band_bottom) {
                r.type = HIT_SCROLLBAR;
                r.index = p;
                r.rel_y = y;                /* framebuffer y; the handler maps it */
                return r;
            }
        }
    }

    /* ---- Browser chrome (page area is native WKWebView) ---- */
    {
        const Tab *atab = (app->tab_count > 0 && app->active_tab >= 0)
                          ? &app->tabs[app->active_tab] : NULL;
        bool has_browser = atab && atab->browser &&
            (atab->kind == TAB_BROWSER || atab->browser_pane >= 0);
        if (has_browser) {
            i32 bh = browser_chrome_hit(app, x, y);
            if (bh != BROWSER_HIT_NONE) {
                switch (bh) {
                    case BROWSER_HIT_BACK:     r.type = HIT_BROWSER_BACK; break;
                    case BROWSER_HIT_FWD:      r.type = HIT_BROWSER_FWD; break;
                    case BROWSER_HIT_RELOAD:   r.type = HIT_BROWSER_RELOAD; break;
                    case BROWSER_HIT_URL:      r.type = HIT_BROWSER_URL; break;
                    case BROWSER_HIT_DEVICE:   r.type = HIT_BROWSER_DEVICE; break;
                    case BROWSER_HIT_PICK:     r.type = HIT_BROWSER_PICK; break;
                    case BROWSER_HIT_ATTACH:   r.type = HIT_BROWSER_ATTACH; break;
                    case BROWSER_HIT_EXTERNAL: r.type = HIT_BROWSER_EXTERNAL; break;
                    case BROWSER_HIT_CLOSE:    r.type = HIT_BROWSER_CLOSE; break;
                    default: r.type = HIT_BROWSER_CHROME; break;
                }
                return r;
            }
            /* Content under chrome is WebView — don't claim as HIT_TERMINAL. */
            BrowserContentRect cr;
            browser_content_rect(app, &cr);
            if (x >= cr.x_px && x < cr.x_px + cr.w_px &&
                y >= cr.y_px && y < cr.y_px + cr.h_px) {
                r.type = HIT_BROWSER_CHROME; /* swallow page area */
                r.index = atab->browser_pane; /* for focus */
                return r;
            }
        }
    }

    /* ---- File-browser tab content (overrides terminal hit) ----
     * When the active tab holds a FileBrowser, the area normally taken
     * by the terminal hosts a list (left) + optional viewer (right).
     * Routes match render_fb_tab() pixel math — must use the SAME
     * top offset as render_terminal (`tab_bar_height` + config-driven
     * `terminal_top_gap`), not the layout.h constants, because the
     * user can override either of those via settings. */
    {
        const Tab *atab = (app->tab_count > 0 && app->active_tab >= 0)
                          ? &app->tabs[app->active_tab] : NULL;
        /* An SFTP tab: the rectangles its last paint laid out. */
        if (atab && atab->kind == TAB_SFTP && atab->sftp && atab->sftp->geom_valid) {
            const SftpGeom *g = &atab->sftp->geom;
            f32 x0 = g->p[0].pane.x, y0 = g->p[0].pane.y;
            f32 x1 = g->p[1].pane.x + g->p[1].pane.w, y1 = y0 + g->p[0].pane.h;
            if (x >= x0 && x < x1 && y >= y0 && y < y1) {
                r.type = HIT_SFTP;
                return r;
            }
        }
        if (atab && atab->kind == TAB_FILEBROWSER && atab->fb) {
            /* Same box render_terminal hands render_fb_tab. */
            f32 ox = sb_w + app->padding;
            f32 oy = app->tab_bar_height +
                     app->config.style.terminal_top_gap * dpi;
            f32 total_w = (f32)app->fb_width - ox - app->padding;
            f32 total_h = status_y - oy - TERM_GRID_MIN_VPAD_PT * dpi;

            if (x >= ox && y >= oy && x < ox + total_w && y < oy + total_h) {
                bool has_viewer = (atab->fb->view_mode != FVIEW_NONE);
                /* Graph renders full-width (no list pane) — mirror render_fb_tab
                 * so click routing matches the drawn pixels. */
                bool viewer_only = atab->fb_viewer_only ||
                                   atab->fb->view_mode == FVIEW_GRAPH;
                FbTabSplit split = fb_tab_split(ox, total_w, dpi, has_viewer,
                                                viewer_only,
                                                atab->fb_viewer_ratio,
                                                fb_view_wants_rail(atab->fb->view_mode));
                f32 view_w = split.view_w, view_x = split.view_x;

                /* Divider grab between list and viewer. */
                if (has_viewer && !viewer_only) {
                    f32 grab = VIEWER_RESIZE_GRAB_PT * dpi;
                    if (x >= view_x - grab && x <= view_x + grab) {
                        r.type = HIT_FBTAB_DIVIDER;
                        return r;
                    }
                }

                if (has_viewer && x >= view_x) {
                    /* Viewer area */
                    f32 vtitle_h = VIEWER_TITLE_H_PT * dpi;
                    bool in_title = y < oy + vtitle_h;
                    f32 close_w = VIEWER_CLOSE_W_PT * dpi;
                    f32 close_x = view_x + view_w - close_w;
                    if (in_title && x >= close_x) {
                        r.type = HIT_FBTAB_VIEWER_CLOSE;
                    } else if (in_title && fb_viewer_toggle_kind(atab->fb) != FB_TOGGLE_NONE) {
                        f32 toggle_w = VIEWER_TOGGLE_W_PT * dpi;
                        f32 tx = close_x - 6.0f * dpi - toggle_w;
                        if (x >= tx && x < tx + toggle_w) r.type = HIT_FBTAB_VIEWER_TOGGLE;
                        else                        r.type = HIT_FBTAB_VIEWER_TITLE;
                    } else if (in_title) {
                        r.type = HIT_FBTAB_VIEWER_TITLE;
                    } else {
                        r.type = HIT_FBTAB_VIEWER;
                    }
                    return r;
                }

                /* List area */
                f32 hdr_h = SIDEBAR_HEADER_PT * dpi;   /* matches fb_render_sidebar */
                f32 ch    = 16.0f * dpi;
                f32 entry_h = FB_ENTRY_H(ch, dpi);   /* single source of truth: layout.h */
                f32 rel_x = x - ox;
                f32 rel_y = y - oy;
                if (rel_y < hdr_h) {
                    r.type = HIT_FBTAB_HEADER;
                } else {
                    i32 idx = (i32)((rel_y - hdr_h + atab->fb->scroll_offset_px) / entry_h);
                    if (idx >= 0 && idx < atab->fb->entry_count) {
                        r.type = HIT_FBTAB_ITEM;
                        r.index = idx;
                    } else {
                        r.type = HIT_FBTAB_BODY;
                    }
                }
                r.rel_x = rel_x;
                r.rel_y = rel_y;
                return r;
            }
        }
    }

    /* ---- Terminal ---- */
    /* The origin must match `render_terminal()`: x is sidebar width plus left
     * padding; y is tab-bar height plus `terminal_top_gap * dpi`, with no top
     * padding because the gap already includes it. Pane widths also exclude
     * the Git diff dock reserve so hit rectangles align with rendered panes.
     */
    r.type = HIT_TERMINAL;
    f32 origin_x, term_w;
    ui_terminal_hspan(app, &origin_x, &term_w);
    f32 origin_y, term_bottom;
    ui_terminal_vspan(app, &origin_y, &term_bottom);
    f32 term_h = term_bottom - origin_y;
    const Tab *atab = (app->tab_count > 0 && app->active_tab >= 0)
                      ? &app->tabs[app->active_tab] : NULL;
    i32 hit_pane = 0;
    f32 pane_w = term_w;
    f32 pane_h = term_h;
    if (atab && atab->split != SPLIT_NONE && atab->split_root >= 0) {
        HitPaneRect pane_rect = {origin_x, origin_y, term_w, term_h};
        i32 pane = hit_split_pane_at(atab, x, y, origin_x, origin_y,
                                     term_w, term_h, dpi, &pane_rect);
        origin_x = pane_rect.x;
        origin_y = pane_rect.y;
        pane_w   = pane_rect.w;
        pane_h   = pane_rect.h;
        hit_pane = pane;
        r.rel_x = (f32)pane;
    } else {
        r.rel_x = 0.0f;
    }
    f32 cw = app->renderer.font.cell_width;
    f32 ch = app->renderer.font.cell_height;
    if (cw > 0 && ch > 0) {
        /* The grid is centred in its pane on both axes (term_grid_pad_x /
         * term_grid_pad_y) — the same two offsets the renderer applies have to
         * come off here, or a click lands a row above and a column left of
         * where the user aimed. */
        const Terminal *pt = atab ? app_tab_pane_terminal(atab, hit_pane) : NULL;
        if (pt) {
            origin_x += term_grid_pad_x(pane_w, pt->cols, cw);
            origin_y += term_grid_pad_y(pane_h, pt->rows, ch);
        }
        r.index = (i32)((x - origin_x) / cw);     /* col */
        r.sub_index = (i32)((y - origin_y) / ch);  /* row */
    }
    return r;
}
