#ifndef LIU_TAB_DROP_H
#define LIU_TAB_DROP_H

/* Where a tab dragged in from ANOTHER window lands on this window's strip.
 *
 * Pure arithmetic over the pills as they are painted, so it works without a
 * window. The caller hands over each tab's painted left edge and slot width
 * (AppState's Tab.render_x and _tab_w + _tab_extra_w[i]).
 *
 * The answer is a tabs[] index the tab is inserted BEFORE, or -1 to append — the
 * contract the pane-detach drag already uses, so both drops share one caret.
 *
 * It is judged against the pills' painted POSITIONS and not against the hit
 * test, on purpose: ui_hit_test answers HIT_NONE in the gap between two pills
 * and HIT_TAB_CLOSE over a pill's right end, and either reads as "append", so a
 * tab dropped between two others would land at the far end of the strip.
 * Positions have no gaps. Walking by painted x also keeps grouped tabs right:
 * the strip paints groups first, so tabs[] order and left-to-right order differ,
 * and "the first pill whose midpoint is right of the pointer" is the same
 * question in either order.
 *
 * `skip[i]` marks a tab that is not on the strip: inside a collapsed group, or
 * not laid out yet (left <= 0 before its first paint). */

#include "core/types.h"

static inline i32 tab_drop_slot(f32 x, const f32 *left, const f32 *width,
                                const u8 *skip, i32 n) {
    i32 best = -1;
    f32 best_left = 0.0f;
    for (i32 i = 0; i < n; i++) {
        if (skip && skip[i]) continue;
        f32 mid = left[i] + width[i] * 0.5f;
        if (x >= mid) continue;               /* pointer is right of this pill's middle */
        if (best < 0 || left[i] < best_left) { /* first such pill in painted order */
            best = i;
            best_left = left[i];
        }
    }
    return best;
}

/* Where a tab dragged INSIDE its own strip belongs, as a position in the strip.
 *
 * `left`/`width` are the strip's slots left to right as they are laid out — the
 * dragged tab's own slot included, at `self`, still there as a hole. `ghost_left`
 * and `ghost_w` are the dragged pill where the pointer is HOLDING it (the pill
 * keeps the offset it was grabbed at; it is not re-centred under the pointer). The
 * answer is the position, 0..n-1, the dragged tab should occupy; `self` means
 * "stay".
 *
 * The rule: a neighbour is passed when the pill's LEADING edge crosses that
 * neighbour's midpoint — the right edge going right, the left edge going left —
 * with both edges pulled in by `inset`.
 *
 * Judging the pill and not the pointer makes a pick-up a no-op wherever the pill is
 * held. "The first slot whose midpoint is right of the POINTER" would count the
 * dragged tab's own slot: a tab picked up by its right half is already "past its
 * own midpoint" and would jump one place to the right on the first pixel of the
 * drag.
 *
 * Why edges against midpoints and not centre against the slot boundary: pills are
 * not all one width (a tab running several agents is wider), and swapping a narrow
 * tab with a wide one moves the boundary between them to the far side of the
 * pointer — the boundary rule swaps straight back, every motion event. After a swap
 * by this rule the passed neighbour's midpoint is behind the pill's trailing edge
 * by the gap plus twice the inset, which is the hysteresis. */
static inline i32 tab_reorder_pos(f32 ghost_left, f32 ghost_w, f32 inset,
                                  const f32 *left, const f32 *width, i32 n, i32 self) {
    if (n <= 0) return -1;
    if (inset < 0.0f) inset = 0.0f;
    if (inset > ghost_w * 0.25f) inset = ghost_w * 0.25f;
    f32 gl = ghost_left + inset;
    f32 gr = ghost_left + ghost_w - inset;
    i32 pos = 0;
    for (i32 k = 0; k < n; k++) {
        if (k == self) continue;
        f32 mid = left[k] + width[k] * 0.5f;
        /* A tab that is not on the strip (self < 0: inside a collapsed group)
         * has no side to be on; it is placed by its centre. */
        bool before = self < 0 ? (mid <= ghost_left + ghost_w * 0.5f)
                    : k < self ? (mid <= gl)     /* still left of the pill unless its left edge passed it */
                               : (mid <= gr);    /* right of the pill until its right edge passes it */
        if (before) pos++;
    }
    return pos < n ? pos : n - 1;
}

/* ---- The strip as it is laid out, and which group a dragged pill is over ----
 *
 * The strip paints its groups first — each one a chip, then its members packed
 * against each other, then ONE gap — and the loose tabs after them, a gap after
 * each. render_toolbar and ui_hit_test walk it that way; the drag code walked
 * it a third way (a gap after the chip and after every member), so inside a
 * group the seam it reordered against drifted a gap further off the pills with
 * each member. tab_strip_layout() is that walk once, as arithmetic, for the
 * drag to use.
 *
 * `tab_group[i]` is a tab's group or -1; a group that is not `group_used`, or
 * has no tab, is not on the strip, and its tabs are loose. A collapsed group is
 * its chip alone. `group_chip_w` is the chip's SLOT (app_get_group_chip_width:
 * the chip plus the gap to its first member). */
#define TAB_STRIP_MAX_TABS    64
#define TAB_STRIP_MAX_GROUPS  16

typedef struct {
    /* slots, left to right */
    i32 tab[TAB_STRIP_MAX_TABS];
    f32 left[TAB_STRIP_MAX_TABS];
    f32 width[TAB_STRIP_MAX_TABS];
    i32 n;
    /* groups on the strip, left to right */
    i32 g_id[TAB_STRIP_MAX_GROUPS];
    f32 g_left[TAB_STRIP_MAX_GROUPS];          /* the chip's left edge             */
    f32 g_members_left[TAB_STRIP_MAX_GROUPS];  /* where its first member starts    */
    f32 g_right[TAB_STRIP_MAX_GROUPS];         /* right edge of its last member    */
    u8  g_open[TAB_STRIP_MAX_GROUPS];          /* members are on the strip         */
    i32 gn;
    f32 loose_left;                            /* where the loose tabs start       */
} TabStripLayout;

static inline void tab_strip_layout(TabStripLayout *L, f32 origin, f32 gap,
                                    const i32 *tab_group, const f32 *tab_w, i32 ntabs,
                                    const u8 *group_used, const u8 *group_collapsed,
                                    const f32 *group_chip_w, i32 ngroups) {
    L->n = 0; L->gn = 0;
    f32 x = origin;
    if (ntabs > TAB_STRIP_MAX_TABS) ntabs = TAB_STRIP_MAX_TABS;
    if (ngroups > TAB_STRIP_MAX_GROUPS) ngroups = TAB_STRIP_MAX_GROUPS;
    for (i32 g = 0; g < ngroups; g++) {
        if (!group_used[g]) continue;
        bool has = false;
        for (i32 i = 0; i < ntabs; i++) if (tab_group[i] == g) { has = true; break; }
        if (!has) continue;
        i32 k = L->gn++;
        L->g_id[k] = g;
        L->g_left[k] = x;
        x += group_chip_w[g];
        L->g_members_left[k] = x;
        L->g_open[k] = group_collapsed[g] ? 0 : 1;
        if (L->g_open[k]) {
            for (i32 i = 0; i < ntabs; i++) {
                if (tab_group[i] != g) continue;
                L->tab[L->n] = i; L->left[L->n] = x; L->width[L->n] = tab_w[i]; L->n++;
                x += tab_w[i];
            }
        }
        L->g_right[k] = x;
        x += gap;
    }
    L->loose_left = x;
    for (i32 i = 0; i < ntabs; i++) {
        i32 g = tab_group[i];
        if (g >= 0 && g < ngroups && group_used[g]) continue;
        L->tab[L->n] = i; L->left[L->n] = x; L->width[L->n] = tab_w[i]; L->n++;
        x += tab_w[i] + gap;
    }
}

/* Which group a dragged pill belongs to by where its CENTRE is: a group's id
 * while it is over that group's members, TAB_ZONE_LOOSE once it is past the last
 * group, TAB_ZONE_KEEP anywhere else (over a chip — a drop there has its own
 * rule — in the gap after a group, left of everything).
 *
 * Membership follows the answer while the drag is still going on, and the
 * layout moves with it, which is the hysteresis: a tab that joins a group
 * lengthens that group by its own slot, so the edge it has to cross to get out
 * again is a whole slot (plus the gap) further on than the one it crossed to
 * get in. There is no position where the two answers alternate. */
#define TAB_ZONE_LOOSE (-1)
#define TAB_ZONE_KEEP  (-2)

static inline i32 tab_drag_zone(const TabStripLayout *L, f32 center) {
    for (i32 k = 0; k < L->gn; k++) {
        if (L->g_open[k] && center >= L->g_members_left[k] && center < L->g_right[k])
            return L->g_id[k];
    }
    if (center >= L->loose_left) return TAB_ZONE_LOOSE;
    return TAB_ZONE_KEEP;
}

#endif
