/*
 * Liu — a poll interval that belongs to the job it paces.
 *
 * "At most every 100 ms" for a function that polls an external helper (the
 * theme agent, the translation agent) cannot be a function-local static: that
 * is one clock for the whole PROCESS, and every window calls the function. The
 * first window of a pass would consume the tick and the others would be told
 * "not yet", every pass, in a fixed order; with two windows each running a
 * job, the second one's stdout would never be drained, its exit never reaped
 * and its timeout never checked while the first kept the clock busy.
 *
 * The clock is a field of the job (AppState), initialised to zero = due now.
 */
#ifndef CORE_POLL_GATE_H
#define CORE_POLL_GATE_H

#include "core/types.h"

typedef struct {
    f64 next_at;      /* monotonic seconds; 0 = due */
} PollGate;

/* True when the poll is due at `now`; the gate then moves `period` on. */
static inline bool poll_gate_due(PollGate *g, f64 now, f64 period) {
    if (now < g->next_at) return false;
    g->next_at = now + period;
    return true;
}

#endif /* CORE_POLL_GATE_H */
