/*
 * Liu — centralized UI hit testing
 * All pixel math uses layout.h constants.
 */
#include "ui/hittest.h"
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/filebrowser.h"
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
    f32 div = 2.0f * dpi;
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
    f32 div = 2.0f * dpi;

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

HitResult ui_hit_test(const struct AppState *app, f32 x, f32 y) {
    HitResult r = { .type = HIT_NONE, .index = -1, .sub_index = -1 };
    f32 dpi = app->dpi_scale;
    bool show_tabs  = app->config.show_tab_bar;
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
        f32 bx = TRAFFIC_LIGHT_ZONE_PT * dpi;

        /* Sidebar toggle button (part of the action-icons cluster). Uses the
         * enlarged sidebar box so the click target matches render_toolbar. */
        f32 sb_box = TB_SIDEBAR_ICON_SIZE_PT * dpi;
        if (show_icons && x >= bx && x < bx + sb_box + TB_BTN_GAP_PT * dpi) {
            r.type = HIT_TOOLBAR_BTN;
            r.sub_index = TB_SIDEBAR;
            return r;
        }
        if (show_icons) bx += sb_box + 8.0f * dpi; /* past separator */

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
        f32 tab_scroll = app->_tab_scroll_x;
        f32 view_x     = app->_tab_view_x > 0.0f ? app->_tab_view_x : bx;
        f32 view_w     = app->_tab_view_w > 0.0f ? app->_tab_view_w
                                                 : ((f32)app->fb_width - bx);
        /* Tabs are clipped to [view_x, view_x+view_w]; a click outside that band
         * (the scrolled-off region, or the pinned "+" slot) must not match a
         * tab even though the walk position math would otherwise land on one. */
        bool in_strip = (x >= view_x && x < view_x + view_w);

        /* Walk through tabs in the same order as render_toolbar (grouped tabs
         * with their headers, then ungrouped), offset left by the scroll. */
        f32 cur_x = bx - tab_scroll;

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
            /* Chip + members pack flush (no inner gap, no capsule pad); one
             * tab_gap after the whole group — mirror render_toolbar() exactly. */
            cur_x += chip_w;

            /* Tabs in this group (if not collapsed) */
            if (!app->tab_groups[gi].collapsed) {
                for (i32 i = 0; i < app->tab_count; i++) {
                    if (app->tabs[i].group_index != gi) continue;
                    if (in_strip && x >= cur_x && x < cur_x + tab_w) {
                        f32 close_x = cur_x + tab_w - PILL_INSET_X_PT * dpi - PILL_INNER_PAD_PT * dpi - TAB_CLOSE_SIZE_PT * dpi;
                        /* Suppress the close zone on narrow tabs (active too) — mirror render. */
                        bool close_room = tab_w >= TAB_CLOSE_MIN_W_PT * dpi;
                        if (close_room && x >= close_x) {
                            r.type = HIT_TAB_CLOSE;
                            r.index = i;
                        } else {
                            r.type = HIT_TAB;
                            r.index = i;
                        }
                        return r;
                    }
                    cur_x += tab_w;
                }
            }
            cur_x += tab_gap;
        }

        /* Ungrouped tabs */
        for (i32 i = 0; i < app->tab_count; i++) {
            i32 gi = app->tabs[i].group_index;
            if (gi >= 0 && gi < MAX_TAB_GROUPS && app->tab_groups[gi].used) continue;
            if (in_strip && x >= cur_x && x < cur_x + tab_w) {
                f32 close_x = cur_x + tab_w - PILL_INSET_X_PT * dpi - PILL_INNER_PAD_PT * dpi - TAB_CLOSE_SIZE_PT * dpi;
                /* Suppress the close zone on narrow tabs (active too) — mirror render. */
                bool close_room = tab_w >= TAB_CLOSE_MIN_W_PT * dpi;
                if (close_room && x >= close_x) {
                    r.type = HIT_TAB_CLOSE;
                    r.index = i;
                } else {
                    r.type = HIT_TAB;
                    r.index = i;
                }
                return r;
            }
            cur_x += tab_w + tab_gap;
        }

        /* New tab (+) button — at the pinned position render_toolbar() used
         * (it clamps to the viewport's right edge when the strip overflows). */
        f32 plus_x = app->_tab_plus_x > 0.0f ? app->_tab_plus_x : cur_x;
        if (show_tabs && x >= plus_x && x < plus_x + 24.0f * dpi) {
            r.type = HIT_TAB_NEW;
            return r;
        }

        /* Right-side action-icons hit zone removed alongside the
         * matching render block in ui.c — only the resource monitor
         * lives on the right of the toolbar now. */
        (void)icon_sz;

        return r;
    }

    /* ---- Status bar ---- */
    if (y >= status_y) {
        r.type = HIT_STATUS_BAR;
        return r;
    }

    /* ---- Sidebar resize border ---- */
    if (app->sidebar_visible) {
        f32 grab = SIDEBAR_RESIZE_GRAB_PT * dpi;
        if (x >= sb_w - grab && x <= sb_w + grab && y > sidebar_top) {
            r.type = HIT_SIDEBAR_RESIZE;
            return r;
        }
    }

    /* ---- Sidebar content ---- */
    if (app->sidebar_visible && x < sb_w && app->filebrowser.entries) {
        f32 rel_y = y - sidebar_top;
        f32 hdr_h = TOOLBAR_HEIGHT_PT * dpi;
        /* Use UI scale metrics, not terminal font — sidebar renders at fixed 16*dpi */
        f32 ch = 16.0f * dpi;
        f32 entry_h = ch + 6.0f; /* must match fb_render_sidebar */

        if (rel_y < hdr_h) {
            r.type = HIT_SIDEBAR;
            r.rel_x = x;       /* sidebar origin is at x=0, so x == rel_x */
            r.rel_y = rel_y;
            return r;
        }

        /* Entry i top (in sidebar-local coords) = hdr_h + i*entry_h - scroll_offset_px */
        i32 idx = (i32)((rel_y - hdr_h + app->filebrowser.scroll_offset_px) / entry_h);
        if (idx >= 0 && idx < app->filebrowser.entry_count) {
            r.type = HIT_SIDEBAR_ITEM;
            r.index = idx;
            r.rel_x = x;
            r.rel_y = rel_y;
        } else {
            r.type = HIT_SIDEBAR;
            r.rel_x = x;
            r.rel_y = rel_y;
        }
        return r;
    }

    /* ---- File viewer ---- */
    if (app->filebrowser.view_mode != FVIEW_NONE) {
        f32 vw = app->viewer_width > 0
               ? app->viewer_width
               : (f32)app->fb_width * VIEWER_WIDTH_RATIO;
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
            } else if (in_title && app->filebrowser.view_mode == FVIEW_MARKDOWN) {
                f32 toggle_w = VIEWER_MD_TOGGLE_W_PT * dpi;
                f32 tx = close_x - 6.0f * dpi - toggle_w;
                if (x >= tx && x < tx + toggle_w) {
                    r.type = HIT_VIEWER_MD_TOGGLE;
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
            f32 grab = 4.0f * dpi;
            f32 term_x = sb_w + app->padding;
            f32 term_y = tb_h + app->padding +
                         app->config.style.terminal_top_gap * dpi;
            f32 term_w = (f32)app->fb_width - sb_w - app->padding * 2;
            f32 term_h = status_y - term_y - app->padding;

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

    /* ---- Scrollbar ---- */
    if (app->config.show_scrollbar) {
        f32 bar_w = 8 * dpi;
        f32 bar_x = (f32)app->fb_width - bar_w - app->padding;
        f32 term_y = tb_h + app->padding + TERMINAL_TOP_GAP_PT * dpi;
        if (x >= bar_x && x <= bar_x + bar_w && y >= term_y && y < status_y) {
            r.type = HIT_SCROLLBAR;
            /* rel_y: normalized position within scrollbar area [0..1] */
            r.rel_y = (y - term_y) / (status_y - term_y);
            return r;
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
        if (atab && atab->kind == TAB_FILEBROWSER && atab->fb) {
            f32 ox = sb_w + app->padding;
            f32 oy = app->tab_bar_height + app->padding +
                     app->config.style.terminal_top_gap * dpi;
            f32 total_w = (f32)app->fb_width - ox - app->padding;
            f32 total_h = status_y - oy - app->padding;

            if (x >= ox && y >= oy && x < ox + total_w && y < oy + total_h) {
                bool has_viewer = (atab->fb->view_mode != FVIEW_NONE);
                /* Graph renders full-width (no list pane) — mirror render_fb_tab
                 * so click routing matches the drawn pixels. */
                bool viewer_only = atab->fb_viewer_only ||
                                   atab->fb->view_mode == FVIEW_GRAPH;
                FbTabSplit split = fb_tab_split(ox, total_w, dpi, has_viewer,
                                                viewer_only,
                                                atab->fb_viewer_ratio,
                                                atab->fb->view_mode == FVIEW_GRAPH ||
                                                atab->fb->view_mode == FVIEW_MARKDOWN);
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
                    } else if (in_title && atab->fb->view_mode == FVIEW_MARKDOWN) {
                        f32 toggle_w = VIEWER_MD_TOGGLE_W_PT * dpi;
                        f32 tx = close_x - 6.0f * dpi - toggle_w;
                        if (x >= tx && x < tx + toggle_w) r.type = HIT_FBTAB_VIEWER_MD_TOGGLE;
                        else                        r.type = HIT_FBTAB_VIEWER_TITLE;
                    } else if (in_title) {
                        r.type = HIT_FBTAB_VIEWER_TITLE;
                    } else {
                        r.type = HIT_FBTAB_VIEWER;
                    }
                    return r;
                }

                /* List area */
                f32 hdr_h = TOOLBAR_HEIGHT_PT * dpi;
                f32 ch    = 16.0f * dpi;
                f32 entry_h = ch + 6.0f;
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
    /* Origin MUST mirror render_terminal() exactly or every click maps to the
     * wrong cell. Render uses ox = sidebar_width + padding (left padding) and
     * oy = tab_bar_height + terminal_top_gap*dpi (NO top padding — the top gap
     * stands in for it). A stray `+ app->padding` here shifted hit-testing
     * down by ~1/3 cell, so selections highlighted the wrong row. */
    r.type = HIT_TERMINAL;
    f32 origin_x = sb_w + app->padding;
    f32 origin_y = app->tab_bar_height +
                   app->config.style.terminal_top_gap * dpi;
    f32 term_w = (f32)app->fb_width - origin_x - app->padding;
    f32 term_h = status_y - origin_y - app->padding;
    const Tab *atab = (app->tab_count > 0 && app->active_tab >= 0)
                      ? &app->tabs[app->active_tab] : NULL;
    if (atab && atab->split != SPLIT_NONE && atab->split_root >= 0) {
        HitPaneRect pane_rect = {origin_x, origin_y, term_w, term_h};
        i32 pane = hit_split_pane_at(atab, x, y, origin_x, origin_y,
                                     term_w, term_h, dpi, &pane_rect);
        origin_x = pane_rect.x;
        origin_y = pane_rect.y;
        r.rel_x = (f32)pane;
    } else {
        r.rel_x = 0.0f;
    }
    f32 cw = app->renderer.font.cell_width;
    f32 ch = app->renderer.font.cell_height;
    if (cw > 0 && ch > 0) {
        r.index = (i32)((x - origin_x) / cw);     /* col */
        r.sub_index = (i32)((y - origin_y) / ch);  /* row */
    }
    return r;
}
