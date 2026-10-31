/*
 * Liu — which stored session a running agent is writing.
 *
 * An agent CLI started in a pane keeps its conversation in its own store
 * (history/scanner.c lists them). To start the same conversation again after
 * Liu quits or crashes, the pane has to know which entry of that store is
 * its agent's; this module answers that from the store itself.
 *
 *   - Claude Code registers every interactive run in
 *     `<config>/sessions/<pid>.json`, with the session id inside: the answer
 *     is exact, keyed by the pane's foreground process.
 *   - Codex appends to one rollout file per thread, sharded by day: the
 *     newest rollout of today or yesterday whose header names the pane's
 *     folder.
 *   - Every other agent: the newest session of that agent's store whose
 *     recorded folder is the pane's, written since the agent started.
 *
 * Two panes running the same agent in one folder cannot be told apart by the
 * store (except Claude Code's); the caller passes the session ids its other
 * panes already hold, and those are not answered twice.
 *
 * Blocking file and database reads: run on a worker
 * (agent_session_async.h), never on the UI thread.
 */
#ifndef HISTORY_AGENT_SESSION_H
#define HISTORY_AGENT_SESSION_H

#include "core/types.h"
#include "history/event.h"

#include <stdatomic.h>

#define AGENT_SESSION_ID_CAP    128
#define AGENT_SESSION_PATH_CAP  1024
#define AGENT_SESSION_EXCLUDE   8

typedef struct {
    char id[AGENT_SESSION_ID_CAP];      /* the store's session id */
    char path[AGENT_SESSION_PATH_CAP];  /* the session's file, "" for a database row */
} AgentSessionRef;

typedef struct {
    ChatTool tool;
    i32      fg_pid;       /* the pane's foreground process group leader, -1 if unknown */
    char     cwd[1024];    /* the pane's folder (absolute) */
    i64      since_ms;     /* wall clock, ms: when the agent took the pane */
    /* Sessions other panes of the same agent and folder already hold. */
    char     exclude[AGENT_SESSION_EXCLUDE][AGENT_SESSION_ID_CAP];
    i32      exclude_count;
} AgentSessionQuery;

/* Fills `out` and returns true when the session is found. `cancel` (may be
 * NULL) is checked between store reads. */
bool agent_session_locate(const AgentSessionQuery *q, AgentSessionRef *out,
                          const _Atomic bool *cancel);

/* agent_session_locate on the caller's thread, given up after `seconds`: a
 * clock thread cancels it at the deadline, so it ends at the next store read
 * after that, however large the store. false when the time ran out (or was
 * not positive) before the session was found. */
bool agent_session_locate_within(const AgentSessionQuery *q, AgentSessionRef *out,
                                 f64 seconds);

/* The agents this module can answer for: those whose store records the
 * folder a session ran in, or that register a run by pid. */
bool agent_session_tool_supported(ChatTool tool);

#endif /* HISTORY_AGENT_SESSION_H */
