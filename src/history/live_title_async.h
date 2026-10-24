/* Bounded, single-worker facade for the disk-backed live-title probe. */
#ifndef HISTORY_LIVE_TITLE_ASYNC_H
#define HISTORY_LIVE_TITLE_ASYNC_H

#include "history/live_title.h"

enum { LIVE_TITLE_ASYNC_CAP = 128, LIVE_TITLE_CWD_CAP = 1024 };
typedef struct {
    ChatTool tool;
    i32 fg_pid;
    char cwd[LIVE_TITLE_CWD_CAP];
    char title[96];
    bool found;
} LiveTitleResult;
typedef enum { LIVE_TITLE_EXPIRED, LIVE_TITLE_PENDING, LIVE_TITLE_READY } LiveTitleStatus;

/* Init/shutdown on the UI thread. No worker until the first request. The
 * callback must outlive all windows. Jobs own copies, never UI pointers. */
void live_title_async_init(void (*wake)(void));
/* Every nonzero return owns one reference, including coalesced requests.
 * Release after reading the result or when the consumer closes/changes context.
 * Moving a token transfers that reference; copying it does not retain it. */
u64 live_title_async_request(ChatTool tool, i32 fg_pid, const char *cwd);
LiveTitleStatus live_title_async_result(u64 token, LiveTitleResult *out);
bool live_title_async_matches(u64 token, ChatTool tool, i32 fg_pid, const char *cwd);
/* Clears the caller's token. The last release cancels queued work and discards
 * an in-progress probe's answer; active filesystem calls finish on the worker. */
void live_title_async_release(u64 *request);
/* Once after the last window; an atexit fallback covers native app quit. */
void live_title_async_shutdown(void);

#endif
