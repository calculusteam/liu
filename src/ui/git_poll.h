/*
 * Git status polling adapts to pane activity and sample cost.
 *
 * QUIET: An unchanged sample doubles the wait up to GIT_POLL_QUIET_MAX_SEC.
 * Pane activity resets it to GIT_POLL_BASE_SEC.
 *
 * COST: The cost component of the next wait is the last sample duration
 * multiplied by GIT_POLL_COST_FACTOR, capped at GIT_POLL_COST_MAX_SEC. The
 * quiet wait is also capped by GIT_POLL_QUIET_MAX_SEC.
 *
 * UNREAD: Do not sample when no visible reader needs the Git status. The call
 * site determines whether a window or status segment is active.
 *
 * The timeline can be evaluated without a repository.
 */
#ifndef UI_GIT_POLL_H
#define UI_GIT_POLL_H

#include "core/types.h"

#define GIT_POLL_BASE_SEC       2.0
#define GIT_POLL_QUIET_MAX_SEC 30.0
#define GIT_POLL_COST_FACTOR   10.0
#define GIT_POLL_COST_MAX_SEC  30.0   /* however slow a sample is, look again within this */

typedef struct {
    f64 quiet;      /* the wait while nothing happens; 0 = never sampled */
    u64 activity;   /* the pane's activity counter at the last sample */
} GitPoll;

/* How long after the last sample the next one is due. `last_cost` is how long
 * that sample took, 0 when there has been none. */
static inline f64 git_poll_wait(const GitPoll *p, f64 last_cost) {
    f64 wait = p->quiet > GIT_POLL_BASE_SEC ? p->quiet : GIT_POLL_BASE_SEC;
    f64 afford = last_cost * GIT_POLL_COST_FACTOR;
    if (afford > GIT_POLL_COST_MAX_SEC) afford = GIT_POLL_COST_MAX_SEC;
    return wait > afford ? wait : afford;
}

/* Every tick, with the pane's activity counter (anything that moves when the
 * pane's content does). Activity since the last sample ends the back-off. */
static inline void git_poll_observe(GitPoll *p, u64 activity) {
    if (activity != p->activity) p->quiet = GIT_POLL_BASE_SEC;
}

/* A sample was taken. One that found the pane exactly as the previous sample
 * left it doubles the wait; any other starts again from the base. */
static inline void git_poll_sampled(GitPoll *p, u64 activity) {
    if (p->quiet > 0.0 && activity == p->activity) {
        p->quiet *= 2.0;
        if (p->quiet > GIT_POLL_QUIET_MAX_SEC) p->quiet = GIT_POLL_QUIET_MAX_SEC;
    } else {
        p->quiet = GIT_POLL_BASE_SEC;
    }
    p->activity = activity;
}

/* The directory changed: whatever was learned about the old one is void. */
static inline void git_poll_reset(GitPoll *p) {
    p->quiet = 0.0;
}

#endif /* UI_GIT_POLL_H */
