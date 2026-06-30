/*
 * Liu - Translate-on-Tab: agent session-store cleanup.
 *
 * claude/codex suppress their throwaway translate transcript at the source
 * (--no-session-persistence / --ephemeral, see translate_agent.c). grok and
 * opencode have NO such flag, so their one-shot translate sessions land in the
 * agent's store and pollute Agent History. This module removes exactly those:
 * snapshot the store at spawn, then after the child exits delete only the
 * session that (a) is NEW since the snapshot, (b) is newer than spawn time, and
 * (c) matches the strict translate one-shot signature (canonical prompt prefix
 * + single user turn). Any one condition failing ⇒ keep-and-never-touch.
 *
 *   grok     — <GROK_HOME|~/.grok>/sessions/<pct-encoded-cwd>/<uuid>/ dirs;
 *              signature checked against chat_history.jsonl + summary.json,
 *              the whole session dir is unlinked.
 *   opencode — <XDG_DATA_HOME|~/.local/share>/opencode/opencode.db rows;
 *              signature checked via message/part, removed with the CLI's
 *              `opencode session delete <id>` (FK cascade).
 */
#ifndef TRANSLATE_HISTORY_CLEANUP_H
#define TRANSLATE_HISTORY_CLEANUP_H

#include "core/types.h"

#define TRANSLATE_CLEANUP_MAX 256
#define TRANSLATE_CLEANUP_ID  64

typedef struct {
    /* Session dir UUIDs (grok) or ses_ ids (opencode) present at spawn time. */
    char entries[TRANSLATE_CLEANUP_MAX][TRANSLATE_CLEANUP_ID];
    i32  count;
    bool valid;        /* false → after_exit is a no-op (snapshot failed/N-A) */
} TranslateCleanupSnap;

/* Snapshot the agent's session store for `cwd`. Only acts for grok/opencode;
 * leaves snap->valid=false otherwise. Cheap; safe to call every translate. */
void translate_cleanup_snapshot(const char *agent_id, const char *cwd,
                                TranslateCleanupSnap *snap);

/* After the translate child has been reaped: find session(s) created since the
 * snapshot, and delete ONLY those matching the translate one-shot signature and
 * with a mtime/created-at at or after `spawn_time`. No-op unless the agent is
 * grok/opencode and snap->valid. Never deletes a real coding session. */
void translate_cleanup_after_exit(const char *agent_id, const char *cwd,
                                  f64 spawn_time, const TranslateCleanupSnap *snap);

#endif /* TRANSLATE_HISTORY_CLEANUP_H */
