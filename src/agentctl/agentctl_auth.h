/*
 * Liu — which pane a process connecting to the agent-control socket runs in.
 *
 * The socket's directory and mode keep other users out; this decides, for a
 * process of the same user, whether it may act and as which pane. The answer
 * comes from the process tree: the caller's parents are walked up until the
 * one whose parent is Liu itself. That process is a pane's shell (forkpty's
 * child), and the shell's pid names the pane (session_local_pane_of_shell).
 *
 * The other children of Liu that may connect are the runners it started for
 * delegated agents; a process below a runner is the delegated agent's own
 * and is refused (it may not start agents of its own).
 *
 * A process that does not descend from this Liu — started by launchd, by
 * another terminal, or detached from a pane by a double fork — gets no pane
 * and is refused, so nothing outside Liu can have Liu start a program (which
 * would run with the permissions macOS granted Liu). Nothing the caller says
 * about itself is believed: an environment variable can be read and copied by
 * any process of the user, a parent chain cannot be rewritten by one.
 *
 * Pure: the two lookups are passed in, so the walk is tested on a made-up
 * process table.
 */
#ifndef AGENTCTL_AUTH_H
#define AGENTCTL_AUTH_H

#include "core/types.h"

#include <stdbool.h>

/* A process's parent; false when the process is gone or cannot be read. */
typedef bool (*AgentctlParentFn)(i32 pid, i32 *ppid, void *ctx);
/* The pane whose shell is `pid`; false when it is no pane's shell. */
typedef bool (*AgentctlShellFn)(i32 pid, u64 *serial, void *ctx);

/* Deeper than any real chain (shell, agent, its tool runner, a sandbox
 * wrapper, a build tool and its children) and short enough that a loop in a
 * corrupted table ends quickly. */
#define AGENTCTL_MAX_HOPS 64

/* The ancestor of `peer` (or `peer` itself) whose parent is `self`: a pane's
 * shell, or a runner Liu started. False when the chain reaches launchd, loops
 * or cannot be read before it reaches `self`. */
static inline bool agentctl_child_of_self(i32 peer, i32 self, AgentctlParentFn parent,
                                          void *ctx, i32 *top) {
    if (top) *top = 0;
    if (peer <= 1 || self <= 1 || peer == self || !parent) return false;
    i32 p = peer;
    for (i32 hops = 0; hops < AGENTCTL_MAX_HOPS; hops++) {
        i32 pp = 0;
        if (!parent(p, &pp, ctx) || pp <= 0) return false;
        if (pp == self) {
            if (top) *top = p;
            return true;
        }
        if (pp == 1 || pp == p) return false;
        p = pp;
    }
    return false;
}

static inline bool agentctl_pane_of_pid(i32 peer, i32 self, AgentctlParentFn parent,
                                        AgentctlShellFn shell, void *ctx, u64 *serial) {
    if (serial) *serial = 0;
    i32 top = 0;
    if (!shell || !agentctl_child_of_self(peer, self, parent, ctx, &top)) return false;
    u64 s = 0;
    if (!shell(top, &s, ctx) || s == 0) return false;
    if (serial) *serial = s;
    return true;
}

#endif /* AGENTCTL_AUTH_H */
