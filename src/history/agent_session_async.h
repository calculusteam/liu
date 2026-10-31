/* Bounded, single-worker facade for agent_session_locate(). The same shape as
 * live_title_async.h: a request is a token that owns one reference, an answer
 * is read back by token, and a released token cancels its work. */
#ifndef HISTORY_AGENT_SESSION_ASYNC_H
#define HISTORY_AGENT_SESSION_ASYNC_H

#include "history/agent_session.h"

enum { AGENT_SESSION_ASYNC_CAP = 64 };

typedef enum {
    AGENT_SESSION_EXPIRED,   /* no such request (released, or never made) */
    AGENT_SESSION_PENDING,
    AGENT_SESSION_READY,
} AgentSessionStatus;

typedef struct {
    AgentSessionQuery query;   /* what was asked */
    AgentSessionRef   ref;     /* the answer, when `found` */
    bool              found;
} AgentSessionResult;

/* UI thread, once. `wake` is called (from the worker) when an answer lands;
 * it must be callable from any thread. No worker until the first request. */
void agent_session_async_init(void (*wake)(void));
/* 0 when the request could not be queued. */
u64  agent_session_async_request(const AgentSessionQuery *q);
AgentSessionStatus agent_session_async_result(u64 token, AgentSessionResult *out);
/* Clears the caller's token; queued work is dropped, a running lookup's
 * answer discarded. */
void agent_session_async_release(u64 *token);
/* After the last window; an atexit fallback covers a native quit. */
void agent_session_async_shutdown(void);

#endif
