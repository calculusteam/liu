/*
 * Liu — per-folder command history.
 *
 * Commands the user runs in a directory are appended to
 * "~/.config/Liu/projects/<slug>/cmdhistory" (one per line, oldest first). Inline autosuggest and
 * the Option+Up history popup draw their candidates from the *current* folder's
 * file, so suggestions are scoped to where you are working instead of one
 * global shell-history list shared across every project.
 *
 * Everything lives centrally under Liu's own config area — nothing is
 * created inside the user's folders. Pre-migration "<dir>/.liu/cmdhistory"
 * files are still read as a fallback, never written.
 */
#ifndef CORE_CMD_HISTORY_H
#define CORE_CMD_HISTORY_H

#include "core/types.h"

#define CMD_HISTORY_MAX_ENTRIES 512    /* most-recent N retained in memory */
#define CMD_HISTORY_ENTRY_CAP   1024   /* max bytes kept per command line  */

/* Append `command` to `dir`'s history (central file; the project dir chain
 * is created on demand). No-op when dir/command is empty, the command is all
 * whitespace, or it duplicates the newest recorded entry. */
void cmd_history_record(const char *dir, const char *command);

/* Opaque, cache-owned snapshot of one folder's history. */
typedef struct CmdHistory CmdHistory;

/* Cached, lazily-reloaded view of `dir`'s history. Reloads only when the
 * backing file's mtime changes or `dir` differs from the previous call.
 * Returns NULL when the folder has no readable, non-empty history file. The
 * pointer is owned by an internal single-slot cache — do not free it and do not
 * retain it past the next cmd_history_get() call. */
const CmdHistory *cmd_history_get(const char *dir);

i32         cmd_history_count(const CmdHistory *h);          /* 0 if h is NULL  */
const char *cmd_history_entry(const CmdHistory *h, i32 i);   /* 0 = oldest      */

/* Most-recent entry that is prefixed by, and strictly longer than, `prefix`
 * (newest -> oldest, case-sensitive). NULL if none match. */
const char *cmd_history_match(const CmdHistory *h, const char *prefix, i32 prefix_len);

/* The snapshot's backing-file freshness key (mtime seconds + byte size).
 * cmd_suggest mirrors this key so its derived model invalidates in
 * lockstep with the log cache. */
void cmd_history_cache_key(const CmdHistory *h, i64 *mtime, i64 *size);

/* Release the internal cache (process-exit cleanup; optional). */
void cmd_history_shutdown(void);

#endif /* CORE_CMD_HISTORY_H */
