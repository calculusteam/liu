/*
 * Liu — per-folder command history.
 *
 * Commands the user runs in a directory are appended to
 * "~/.config/Liu/projects/<slug>/cmdhistory" (one execution per record, oldest first; control bytes are encoded). Inline autosuggest and
 * the Option+Up history popup draw their candidates from the *current* folder's
 * file, so suggestions are scoped to where you are working instead of one
 * global shell-history list shared across every project.
 *
 * Everything lives centrally under Liu's own config area — nothing is
 * created inside the user's folders, and nothing is READ from them: a
 * "<dir>/.liu/cmdhistory" that a repository ships is not the user's history.
 * The file is cut back to its newest half when it passes 4 MiB.
 */
#ifndef CORE_CMD_HISTORY_H
#define CORE_CMD_HISTORY_H

#include "core/types.h"

#define CMD_HISTORY_MAX_ENTRIES 512    /* most-recent N retained in memory */
#define CMD_HISTORY_ENTRY_CAP   4096   /* record capacity; oversized commands are refused  */

/* Append `command` to `dir`'s history (central file; the project dir chain
 * is created on demand). No-op when dir/command is empty, the command is all
 * whitespace, or it exceeds ENTRY_CAP - 1 bytes. Repeated executions are retained. */
void cmd_history_record(const char *dir, const char *command);
/* Completed shell execution; persist status, elapsed seconds and completion
 * timestamp alongside the command. Plain records have unknown outcome. */
void cmd_history_record_result(const char *dir, const char *command,
                               i32 exit_code, f64 duration);

/* Opaque, cache-owned snapshot of one folder's history. */
typedef struct CmdHistory CmdHistory;

/* Identity of the backing file a snapshot was read from. Seconds-resolution
 * mtime plus size would miss a same-length rewrite inside one second and an
 * atomic replace that kept the old timestamp; the inode catches the replace,
 * the nanosecond ctime catches both (utimes cannot set it back). A missing
 * file is the all-zero key. Metadata cannot prove that a rewrite which leaves
 * every one of these fields identical changed nothing — no such guarantee is
 * made. */
typedef struct {
    u64 dev, ino;
    i64 size;
    i64 mtime_sec, mtime_ns;
    i64 ctime_sec, ctime_ns;
} CmdHistoryKey;

bool cmd_history_key_equal(const CmdHistoryKey *a, const CmdHistoryKey *b);

/* Cached, lazily-reloaded view of `dir`'s history. Reloads only when the
 * backing file's identity (CmdHistoryKey) changes or `dir` differs from the
 * previous call. A read that fails part-way is never published as a fresh
 * snapshot: it is retried on a bounded backoff while the file is unchanged,
 * and at once when it changes.
 * Returns NULL when the folder has no readable, non-empty history file. The
 * pointer is owned by an internal single-slot cache — do not free it and do not
 * retain it past the next cmd_history_get() call. */
const CmdHistory *cmd_history_get(const char *dir);

i32         cmd_history_count(const CmdHistory *h);          /* 0 if h is NULL  */
const char *cmd_history_entry(const CmdHistory *h, i32 i);   /* 0 = oldest      */

/* Most-recent entry that is prefixed by, and strictly longer than, `prefix`
 * (newest -> oldest, case-sensitive). NULL if none match. */
const char *cmd_history_match(const CmdHistory *h, const char *prefix, i32 prefix_len);

/* The snapshot's backing-file freshness key. cmd_suggest mirrors this key so
 * its derived model invalidates in lockstep with the log cache. NULL h gives
 * the all-zero key. */
void cmd_history_cache_key(const CmdHistory *h, CmdHistoryKey *out);

/* Release the internal cache (process-exit cleanup; optional). */
void cmd_history_shutdown(void);

#endif /* CORE_CMD_HISTORY_H */
