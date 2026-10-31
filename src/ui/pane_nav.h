/*
 * Liu — moving the keyboard between the panes of a split tab.
 *
 * A directional move goes to the pane that lies on that side of the focused
 * one, judged on the rects the layout gives the panes (the tree's, never the
 * zoomed ones). The ranking, in order:
 *   1. the nearest: a pane across the divider beats one further along;
 *   2. the one that shares the most of the focused pane's span across the
 *      move (moving right from a pane that faces two, the one beside most of it);
 *   3. the one whose span's middle is nearest the focused pane's middle;
 *   4. the top one (a sideways move) or the left one (an up or down move).
 * There is no wrap: at the edge of the tab nothing lies that way.
 *
 * A cycle walks the panes in the layout tree's reading order, and wraps.
 * Pure: arithmetic on rects and indices.
 */
#ifndef UI_PANE_NAV_H
#define UI_PANE_NAV_H

#include "ui/ui.h"

typedef enum {
    PANE_NAV_LEFT = 0,
    PANE_NAV_RIGHT,
    PANE_NAV_UP,
    PANE_NAV_DOWN,
} PaneNavDir;

static inline f32 pane_nav_overlap(f32 a0, f32 a1, f32 b0, f32 b1) {
    f32 lo = a0 > b0 ? a0 : b0;
    f32 hi = a1 < b1 ? a1 : b1;
    return hi - lo;
}

/* The pane a move from `from` in `dir` lands on among the `count` panes laid
 * out as `rects`, or -1 when none lies that way. A pane with an empty rect is
 * not a candidate. */
static inline i32 pane_nav_neighbor(const PaneRect *rects, i32 count, i32 from, PaneNavDir dir) {
    if (!rects || from < 0 || from >= count) return -1;
    const PaneRect f = rects[from];
    if (f.w <= 0.0f || f.h <= 0.0f) return -1;
    const f32 eps = 0.5f;   /* the layout's rects are fractional pixels */
    bool horiz = dir == PANE_NAV_LEFT || dir == PANE_NAV_RIGHT;
    f32 f_mid = horiz ? f.y + f.h * 0.5f : f.x + f.w * 0.5f;

    i32 best = -1;
    f32 best_gap = 0.0f, best_share = 0.0f, best_mid = 0.0f, best_pos = 0.0f;
    for (i32 i = 0; i < count; i++) {
        if (i == from) continue;
        const PaneRect r = rects[i];
        if (r.w <= 0.0f || r.h <= 0.0f) continue;
        f32 gap;
        switch (dir) {
        case PANE_NAV_LEFT:  gap = f.x - (r.x + r.w); break;
        case PANE_NAV_RIGHT: gap = r.x - (f.x + f.w); break;
        case PANE_NAV_UP:    gap = f.y - (r.y + r.h); break;
        default:             gap = r.y - (f.y + f.h); break;
        }
        if (gap < -eps) continue;                    /* not wholly on that side */
        f32 share = horiz ? pane_nav_overlap(f.y, f.y + f.h, r.y, r.y + r.h)
                          : pane_nav_overlap(f.x, f.x + f.w, r.x, r.x + r.w);
        if (share <= eps) continue;                  /* beside it, not across from it */
        f32 r_mid = horiz ? r.y + r.h * 0.5f : r.x + r.w * 0.5f;
        f32 mid = r_mid > f_mid ? r_mid - f_mid : f_mid - r_mid;
        f32 pos = horiz ? r.y : r.x;
        bool better;
        if (best < 0)                               better = true;
        else if (gap < best_gap - eps)              better = true;
        else if (gap > best_gap + eps)              better = false;
        else if (share > best_share + eps)          better = true;
        else if (share < best_share - eps)          better = false;
        else if (mid < best_mid - eps)              better = true;
        else if (mid > best_mid + eps)              better = false;
        else                                        better = pos < best_pos - eps;
        if (better) {
            best = i; best_gap = gap; best_share = share; best_mid = mid; best_pos = pos;
        }
    }
    return best;
}

/* The pane `step` places on from `from` in `order` (the `n` panes in reading
 * order), wrapping at either end; `from` itself when it is not in the order. */
static inline i32 pane_nav_cycle(const i32 *order, i32 n, i32 from, i32 step) {
    if (!order || n <= 0) return from;
    i32 at = -1;
    for (i32 i = 0; i < n; i++)
        if (order[i] == from) { at = i; break; }
    if (at < 0) return from;
    i32 k = (at + step % n + n) % n;
    return order[k];
}

#endif /* UI_PANE_NAV_H */
