/*
 * Liu — the live sessions of a tab, whichever field holds them.
 *
 * A tab keeps its panes in three places: `session` (pane 0), `session2` (pane
 * 1) and `extra_sessions[]` (panes 2..MAX_SPLIT_PANES-1). Every service the
 * main loop gives an SSH session — the passphrase and host-key prompts, the
 * keyboard-interactive (2FA) challenge, the keepalive — must reach every pane:
 * a pane whose prompt is never raised leaves the SSH worker blocked until the
 * modal answers it, a connection that hangs with nothing on screen to explain
 * it.
 *
 * ONE place knows the layout, so no service is written against only two panes.
 * Pure: reads the Tab, calls nothing.
 */
#ifndef UI_PANE_SESSIONS_H
#define UI_PANE_SESSIONS_H

#include "ui/ui.h"

/* How many panes the tab has: one when it is not split, otherwise the split's
 * own count held to [2, MAX_SPLIT_PANES]. The same clamp every caller gets
 * (tab_split_pane_count returns this). */
static inline i32 tab_pane_slot_count(const Tab *tab) {
    if (!tab || tab->split == SPLIT_NONE) return 1;
    i32 count = tab->split_pane_count;
    if (count < 2) count = 2;
    if (count > MAX_SPLIT_PANES) count = MAX_SPLIT_PANES;
    return count;
}

/* The session in pane `pane` (NULL when there is none, or `pane` is out of
 * range), regardless of which of the three fields it lives in. */
static inline Session *tab_pane_session_at(const Tab *tab, i32 pane) {
    if (!tab || pane < 0) return NULL;
    if (pane == 0) return tab->session;
    if (pane == 1) return tab->session2;
    if (pane < MAX_SPLIT_PANES) return tab->extra_sessions[pane - 2];
    return NULL;
}

/* Every session the tab holds, in pane order. Empty slots and panes past the
 * tab's own count are skipped — a pointer left in a slot the split no longer
 * uses is not a pane. Returns how many were written to `out`. */
static inline i32 pane_sessions_collect(const Tab *tab, Session *out[MAX_SPLIT_PANES]) {
    i32 n = 0;
    i32 slots = tab_pane_slot_count(tab);
    for (i32 p = 0; p < slots; p++) {
        Session *s = tab_pane_session_at(tab, p);
        if (s) out[n++] = s;
    }
    return n;
}

#endif /* UI_PANE_SESSIONS_H */
