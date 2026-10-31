/*
 * Liu — the subagents an agent in a pane starts itself, shown as job tabs.
 *
 * Claude Code's Agent tool, Codex's spawned agents, OpenCode's child
 * sessions, Grok's and Kimi's subagents, Oh My Pi's tasks and Command Code's
 * agent tool each run a subagent and record it in the agent's own store
 * (agentctl_subread.h lists where). This module watches the stores of the
 * panes the main thread names, and keeps each subagent it finds as a job of
 * its own: a log of its newest lines (agentctl_ring.h) and a state. It
 * registers itself as agent control's watched view (agentctl.h), so the job
 * tabs beside the pane's tab, their hover, the chat and the docked list show
 * these subagents as they show a delegated agent — read-only.
 *
 * What is shown: every subagent that starts while Liu watches the pane, and,
 * when Liu first sees a pane's session, the ones still running then. One that
 * has finished stays until the user sends the pane's agent its next prompt,
 * dismisses it, or the pane's agent changes session.
 *
 * Cost: a thread of its own, started with the first pane that runs a
 * supported agent, that sleeps on a condition variable while there is nothing
 * to look at. While a pane's agent works (or has just been sent a prompt) it
 * looks for new subagents once a second — a stat of a folder, a listing only
 * when the folder changed — and for AGENTCTL_SUB_LOOK_AFTER_S after it stops;
 * a running subagent's transcript is read twice a second, only the bytes it
 * grew by, and OpenCode's rows past the last one read. Every file is opened
 * O_NONBLOCK and read only when it is a regular file. Listed in the order
 * they started.
 */
#ifndef AGENTCTL_SUBAGENTS_H
#define AGENTCTL_SUBAGENTS_H

#include "core/types.h"
#include "agentctl/agentctl.h"

#include <stdbool.h>

#define AGENTCTL_SUB_LOOK_AFTER_S  20     /* new subagents are looked for this long after the agent stops */
#define AGENTCTL_SUB_BASELINE_S    120    /* when a session is first seen, a subagent quiet this long is past */
#define AGENTCTL_SUB_TAIL_BYTES    (384u * 1024u)   /* of a long transcript, the end is read first */

/* A pane as the main thread knows it. */
typedef struct {
    u64  pane;               /* the pane's serial (session_instance_id) */
    i32  tool;               /* the ChatTool of the agent in front; 0 = none */
    char session_id[128];    /* the store's id of the session it runs on; "" = not known yet */
    char session_path[1024]; /* its file, "" for a database row */
    char cwd[1024];
    char owner_label[96];    /* "Claude Code (tab 2)" */
    i64  since_ms;           /* wall clock: when the agent took the pane */
    bool busy;               /* the agent is at work */
} AgentctlSubPane;

/* Whether Liu can show the subagents of this ChatTool. */
bool agentctl_subagents_supported(i32 tool);

/* Registers the watched view. With `thread` the watcher runs on a thread of
 * its own, started with the first pane worth watching; `wake_main` (any
 * thread) is called when what a window shows changed. Without it nothing
 * runs until agentctl_subagents_poll_once is called. */
void agentctl_subagents_start(void (*wake_main)(void), bool thread);
/* Ends the thread and forgets everything. */
void agentctl_subagents_stop(void);
/* The exit path (Cmd+Q ends in exit()): tells the thread to stop, no join. */
void agentctl_subagents_exit(void);

/* The setting: OFF hides every subagent and stops looking; ON looks again. */
void agentctl_subagents_set_enabled(bool on);

/* Main thread: the pane's agent, session and work as known now; called when
 * any of it changes. tool 0 = the agent left the pane (its running
 * subagents are shown stopped). */
void agentctl_subagents_pane(const AgentctlSubPane *p);
/* The pane closed: its subagents go. Any thread. */
void agentctl_subagents_pane_gone(u64 pane);
/* The user sent the pane's agent a prompt: its finished subagents go. */
void agentctl_subagents_prompt(u64 pane);

/* One pass of the watcher on the caller's thread, `now_ms` wall clock: what
 * the thread does each time it wakes. */
void agentctl_subagents_poll_once(i64 now_ms);

/* How many subagents are shown now. Any thread. */
i32  agentctl_subagents_count(void);

#endif /* AGENTCTL_SUBAGENTS_H */
