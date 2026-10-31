/*
 * Liu — a split tab showing one of its panes alone (pane zoom).
 *
 * A zoom fills the tab with the pane that had the keyboard when it began. The
 * other panes keep running, hidden, at the size they come back to, so nothing
 * they print is reflowed twice. The zoom holds only while that pane is still
 * the focused one: when focus is moved anywhere else (a keyboard move, the
 * sidebar's agent row) the tab shows its layout again, so keys never reach a
 * pane that is not on screen.
 *
 * The renderer, the hit test and the pane sizing all read the zoom through
 * these two functions. Pure: reads the Tab, calls nothing.
 */
#ifndef UI_PANE_ZOOM_H
#define UI_PANE_ZOOM_H

#include "ui/pane_sessions.h"

/* The pane shown alone, or -1 when the tab shows its layout: a zoom of
 * `zoomed_pane` holds while it is the focused pane of a tab with at least two. */
static inline i32 pane_zoom_shown(bool zoomed, i32 zoomed_pane, i32 active_pane, i32 pane_count) {
    if (!zoomed || pane_count < 2) return -1;
    if (zoomed_pane < 0 || zoomed_pane >= pane_count) return -1;
    return zoomed_pane == active_pane ? zoomed_pane : -1;
}

/* Which pane of `tab` fills it, -1 when none does. Only a terminal tab with a
 * layout tree zooms; the two-pane fallback geometry never does. */
static inline i32 tab_zoomed_pane(const Tab *tab) {
    if (!tab || tab->kind != TAB_TERMINAL || tab->split == SPLIT_NONE || tab->split_root < 0)
        return -1;
    return pane_zoom_shown(tab->zoomed, tab->zoomed_pane, tab->active_pane,
                           tab_pane_slot_count(tab));
}

/* True when a zoom of another pane hides pane `pane` of `tab`. */
static inline bool tab_zoom_hides_pane(const Tab *tab, i32 pane) {
    i32 z = tab_zoomed_pane(tab);
    return z >= 0 && z != pane;
}

/* What the zoom makes of the rects the layout gave the panes inside `box`: the
 * shown pane takes the whole box. With `fit` the hidden panes keep theirs (the
 * size each terminal keeps while hidden); without it they get none (they are
 * not drawn and take no click). Returns the shown pane, -1 when nothing changed. */
static inline i32 pane_zoom_apply(const Tab *tab, PaneRect box,
                                  PaneRect rects[MAX_SPLIT_PANES], bool fit) {
    i32 z = tab_zoomed_pane(tab);
    if (z < 0) return -1;
    if (!fit)
        for (i32 i = 0; i < MAX_SPLIT_PANES; i++) rects[i] = (PaneRect){0};
    rects[z] = box;
    return z;
}

#endif /* UI_PANE_ZOOM_H */
