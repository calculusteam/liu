/*
 * liu — live chat-title probe for foreground agent CLIs.
 *
 * Some agent CLIs title the conversation they are running and store that
 * title on disk while the session is live. This module resolves the title
 * for the agent attached to a tab so the tab bar can show the chat name
 * instead of the bare tool name.
 *
 * Sources:
 *   Claude Code — ~/.claude/sessions/<pid>.json live registry ("name")
 *   Codex       — newest ~/.codex/sessions/YYYY/MM/DD rollout whose
 *                 session_meta cwd matches the tab, then thread_name from
 *                 ~/.codex/session_index.jsonl
 *   Kimi        — newest ~/.kimi/sessions/<md5(cwd)>/<id>/wire.jsonl, whose
 *                 first TurnBegin record supplies the title (Kimi derives it
 *                 at runtime and never persists one)
 *   Command Code— newest ~/.commandcode/projects/<slug(cwd)>/<id>.jsonl, with
 *                 the title read from its <id>.meta.json sidecar
 *   Pi, Oh My Pi— newest ~/.{pi,omp}/agent/sessions/--<cwd>--/<id>.jsonl: omp's
 *                 title is the file's first line, Pi's the last session_info
 *                 name the user set
 * Other tools return false (caller falls back to the tool display name).
 */
#ifndef HISTORY_LIVE_TITLE_H
#define HISTORY_LIVE_TITLE_H

#include "core/types.h"
#include "history/event.h"

/* Best-effort lookup of the chat title the running agent generated for the
 * session on this tab.
 *   tool:   foreground agent (from tab_agent_tool / process-name match)
 *   fg_pid: PTY foreground process-group leader pid (-1 if unknown)
 *   cwd:    tab working directory (OSC 7 or proc-derived); may be NULL
 * Fills `out` (NUL-terminated, control chars stripped, never ending in a
 * half-written UTF-8 sequence) and returns true when a non-empty title
 * exists. Cheap enough for a seconds-scale poll, but NOT for a per-frame
 * call — throttle at the call site. */
/* True when the probe for `tool` attributes a session by the pane's
 * foreground pid: only Claude, whose session registry is keyed by pid. Every
 * other probe answers from (tool, cwd) alone — Cline's metadata pid is its
 * daemon's — so requests differing just by pid are ONE question: the async
 * queue runs it once, and a pane accepts the shared answer without comparing
 * pids (live_title_async.c, ui.c). */
static inline bool live_title_tool_keys_pid(ChatTool tool) {
    return tool == CHAT_TOOL_CLAUDE;
}

/* Tests: bytes read from Codex's session_index.jsonl; Cline metadata files
 * opened and parsed. Cumulative, process-wide. */
u64 live_title_codex_index_bytes_read(void);
u64 live_title_cline_meta_reads(void);
u64 live_title_cline_meta_parses(void);

bool chat_live_title(ChatTool tool, i32 fg_pid, const char *cwd,
                     char *out, usize cap);

/* Cooperative cancellation between filesystem/parse steps. Does not interrupt
 * an OS call already in progress. The callback and context are borrowed only
 * for this call; a cancelled probe always returns an empty answer. */
typedef bool (*LiveTitleCancelled)(void *ctx);
bool chat_live_title_cancellable(ChatTool tool, i32 fg_pid, const char *cwd,
                                 char *out, usize cap,
                                 LiveTitleCancelled cancelled, void *ctx);

#endif /* HISTORY_LIVE_TITLE_H */
