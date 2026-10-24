/*
 * Liu — how long the loop may sleep while a session is connected and nothing
 * is happening.
 *
 * A pane's bytes, a hook's signal, a finished connect and every key or click
 * wake the loop on their own. What the connected-idle tick still serves is the
 * work that runs on the loop's own clock: the agent and title probes (0.75 s
 * and 2.5 s for the focused tab, 2 s and 8 s behind it), the SSH keepalive
 * check (1 s), the explorer's kernel-cwd backstop, and the workers that
 * publish a result without a wake (git status and diff, a model download's
 * progress). 80 ms keeps all of them on time, but it also wakes a window that
 * nobody is using 12.5 times a second, for no frame.
 *
 * After LOOP_QUIET_AFTER_SEC with no output and no input the tick is 250 ms:
 * 4 wakes a second, every probe at most a quarter of a second later than its
 * schedule. The first byte or key puts it back on 80 ms, and the 3 s that
 * follow keep it there, so a command that is still printing, an agent at
 * work and a person typing never see the slower tick.
 */
#ifndef CORE_LOOP_IDLE_H
#define CORE_LOOP_IDLE_H

#include "core/types.h"

#define LOOP_CONNECTED_IDLE_DT  0.080   /* a session is connected, recently busy */
#define LOOP_CONNECTED_QUIET_DT 0.250   /* ...and quiet for LOOP_QUIET_AFTER_SEC */
#define LOOP_QUIET_AFTER_SEC    3.0

/* The wait while a session is connected and nothing more urgent applies.
 * `last_activity` is when the loop last drained a pane's output or took the
 * user's input; 0 (never) counts as quiet. */
static inline f64 loop_connected_idle_dt(f64 now, f64 last_activity) {
    return (last_activity > 0.0 && now - last_activity < LOOP_QUIET_AFTER_SEC)
         ? LOOP_CONNECTED_IDLE_DT : LOOP_CONNECTED_QUIET_DT;
}

#endif
