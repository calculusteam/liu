/* Synchronized terminal frames across a tab's visible panes.
 * The window deadline bounds overlapping updates: staggered agents must not
 * keep the whole window waiting forever by taking turns opening BSU. */
#ifndef LIU_UI_PANE_SYNC_H
#define LIU_UI_PANE_SYNC_H
#include "terminal/terminal.h"

typedef struct {
    f64 started;
    bool waiting;
} PaneSyncGate;

/* Zero means ready; a positive deadline means the pane is still updating. */
static inline f64 pane_sync_deadline(Terminal *term, f64 now, f64 timeout) {
    if (!term || !term->sync_pending) return 0;
    if (term->sync_start_time == 0) term->sync_start_time = now;
    f64 deadline = term->sync_start_time + timeout;
    if (now >= deadline) {
        term->sync_pending = false;
        term->sync_start_time = 0;
        return 0;
    }
    return deadline;
}

static inline bool pane_sync_wait(PaneSyncGate *gate, Terminal *const *panes,
                                  i32 count, f64 now, f64 timeout) {
    bool pending = false;
    for (i32 i = 0; i < count; i++)
        if (pane_sync_deadline(panes[i], now, timeout) > 0) pending = true;
    if (!pending) { gate->waiting = false; return false; }
    if (!gate->waiting) { gate->waiting = true; gate->started = now; }
    if (now - gate->started < timeout) return true;
    /* The oldest blocked window frame has reached the same timeout used by
     * individual panes. Publish and reset the group even if a newer pane's
     * transaction started in the meantime. */
    for (i32 i = 0; i < count; i++) if (panes[i]) {
        panes[i]->sync_pending = false;
        panes[i]->sync_start_time = 0;
    }
    gate->waiting = false;
    return false;
}
#endif
