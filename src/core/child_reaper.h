/*
 * Liu — ending a child process without making anyone wait for it.
 *
 * A helper can ignore SIGTERM (a CLI mid-request, a shell wrapper), so a
 * grace wait and a SIGKILL run on the thread that handles keyboard input
 * would hold the UI for the whole grace period every time a translation is
 * cancelled by typing.
 *
 * child_reaper_terminate() sends the SIGTERM and returns. A short-lived
 * supervisor thread owns the rest: the grace wait, the SIGKILL and the final
 * reap, so the pid is waited for exactly once and never signalled again after
 * it has been collected (a recycled pid is somebody else's process).
 *
 * The caller has already forgotten the pid (its job state moved on at once);
 * nothing here calls back.
 */
#ifndef CORE_CHILD_REAPER_H
#define CORE_CHILD_REAPER_H

#include "core/types.h"
#include <sys/types.h>

/* SIGTERM now; SIGKILL after the grace period if it is still there; reap. Safe
 * from any thread, returns at once. A pid already being ended is ignored. When
 * the supervisor cannot be started the child is killed outright and reaped
 * before returning — a bounded wait, never a leak. */
void child_reaper_terminate(pid_t pid);

/* How long a child gets between SIGTERM and SIGKILL. */
#define CHILD_REAPER_GRACE_MS 300

/* Supervisors still running (tests, diagnostics). */
i32 child_reaper_inflight(void);

/* Wait, up to `timeout_ms`, for the supervisors to finish. For shutdown and
 * tests; returns true when none remain. */
bool child_reaper_wait_idle(i32 timeout_ms);

#endif /* CORE_CHILD_REAPER_H */
