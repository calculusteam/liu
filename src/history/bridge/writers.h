/*
 * liu-history/bridge - native session-store writers.
 *
 * Everything that serializes a BridgeHistory into another application's
 * private on-disk format lives behind this header, one file per agent. The
 * separation from the targets is not cosmetic: a target decides WHETHER to
 * write (version gate, availability, id minting, validation, launch command)
 * and a writer only knows HOW. When a format moves — and these formats move —
 * exactly one file changes and the gate that protects it is somewhere else.
 *
 * Every writer here obeys the same four rules:
 *   - a NEW file at a path that did not exist; never an append, never an
 *     overwrite of an existing session;
 *   - through jsonw_open_file, i.e. atomic temp + rename, 0600, symlink-
 *     refusing, so a half-written session is never visible to the agent;
 *   - no credential, permission or sandbox state, ever (the IR has no field
 *     that could carry one);
 *   - tool records written as HISTORY of calls that already ran.
 */
#ifndef HISTORY_BRIDGE_WRITERS_H
#define HISTORY_BRIDGE_WRITERS_H

#include "history/bridge/bridge.h"
#include "history/bridge/ir.h"

/* Claude Code: ~/.claude/projects/<slug>/<uuid>.jsonl.
 * `bridge_claude_project_dir` produces the slugged project directory for a
 * cwd with Claude's own rule (hist_slugify_claude). The scanner compares the
 * same way; it does not try to reverse a directory name — that is lossy. */
bool bridge_claude_project_dir(const char *cwd, char *out, usize cap);
bool bridge_write_claude_session(const BridgeHistory *h, const char *session_id,
                                 const char *path, u32 *out_warnings);

/* CommandCode: ~/.commandcode/projects/<lower-dash-slug>/<uuid>.jsonl. */
bool bridge_commandcode_project_dir(const char *cwd, char *out, usize cap);
bool bridge_write_commandcode_session(const BridgeHistory *h, const char *session_id,
                                      const char *path, u32 *out_warnings);

/* Qwen Code: ~/.qwen/projects/<slug>/chats/<name>.jsonl (Gemini Content). */
bool bridge_qwen_chats_dir(const char *cwd, char *out, usize cap);
bool bridge_write_qwen_session(const BridgeHistory *h, const char *session_id,
                               const char *path, u32 *out_warnings);

/* Grok: a session DIRECTORY. `dir` must already exist; the writer fills in
 * chat_history.jsonl and summary.json. */
bool bridge_grok_session_dir(const char *cwd, const char *session_id,
                             char *out, usize cap);
bool bridge_write_grok_session(const BridgeHistory *h, const char *session_id,
                               const char *dir, u32 *out_warnings);

/* Kimi CLI: <share>/sessions/<md5(cwd)>/<uuid>/context.jsonl. */
bool bridge_kimi_session_dir(const char *cwd, const char *session_id,
                             char *out, usize cap);
bool bridge_write_kimi_session(const BridgeHistory *h, const char *session_id,
                               const char *dir, u32 *out_warnings);

/* Cline CLI: ~/.cline/data/sessions/<id>/{<id>.json,<id>.messages.json}.
 * The id is `<unix-ms>_<5 base36 chars>` — the native shape, not a UUID: it
 * is the directory name AND what `cline --id` takes, and the picker sorts on
 * the leading timestamp. */
bool bridge_cline_new_id(char *out, usize cap);
bool bridge_cline_session_dir(const char *session_id, char *out, usize cap);
bool bridge_write_cline_session(const BridgeHistory *h, const char *session_id,
                                const char *dir, u32 *out_warnings);

/* True when `path` already exists in any form. Every writer checks this: a
 * generated id colliding with a real session is the one way this subsystem
 * could destroy a user's work, so a collision is a hard failure, never a
 * merge and never an overwrite. */
bool bridge_path_exists(const char *path);

/* ISO 8601 "YYYY-MM-DDTHH:MM:SS.mmmZ" for `ms` (0 -> now). */
void bridge_iso8601(char *out, usize cap, i64 ms);
/* `text` with every "</tag>" escaped so it cannot close a wrapper the writer
 * puts around it; malloc'd, caller frees. NULL on OOM or NULL text. */
char *bridge_escape_closing_tag(const char *text, const char *tag);

#endif /* HISTORY_BRIDGE_WRITERS_H */
