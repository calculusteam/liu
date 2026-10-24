/*
 * liu-history - session metadata and discovery callback API.
 */
#ifndef HISTORY_SESSION_H
#define HISTORY_SESSION_H

#include "core/memory.h"
#include "core/types.h"
#include "history/event.h"

#include <stdatomic.h>

/* Internal scratch buffer cap used while building paths inside the scanner.
 * The public ChatSessionMeta strings are arena-owned slices of variable
 * length, so consumers don't see this cap. */
#define CHAT_PATH_CAP      4096

/* Canonical capacity for chat_scan / chat_find_session arenas.
 *
 * A heavy user can easily hit 500+ sessions × ~600 B of
 * path/project/session_id/cwd strings. When the arena is exhausted,
 * arena_alloc returns NULL and later entries are silently dropped. 12 MB
 * is the single size policy for every UI/CLI scan site; a smaller arena
 * truncates the list after the first stores. */
#define CHAT_SCAN_ARENA_BYTES  MB(12)

/* Session metadata. The string fields are arena-allocated by the scanner /
 * find helper; their lifetime is tied to the Arena passed in by the caller.
 * After the caller calls arena_destroy (or the next chat_scan_arena_borrow)
 * on that arena, these pointers are invalid — copy what you need first. */
typedef struct {
    ChatTool    tool;
    const char *session_id;        /* file-basename minus .jsonl extension */
    const char *project;           /* project slug (parent dir name); "-" if n/a */
    const char *path;              /* absolute path to the session file */
    /* Best-effort absolute working directory of the agent session, when
     * the underlying tool stores it in a cheaply-readable place:
     *   - Claude: decoded from the slugged project directory name
     *   - Codex:  parsed from the session_meta header on the first JSONL line
     *   - Other tools: NULL (not extractable without parsing whole file)
     * Filters that key on cwd should fall back to other heuristics when
     * this is NULL. */
    const char *cwd;
    i64         last_modified_ms;
    i64         size_bytes;
    u32         event_count;       /* 0 if not yet computed */
} ChatSessionMeta;

/* Return false from the callback to stop iteration early. */
typedef bool (*chat_session_cb)(const ChatSessionMeta *meta, void *user);

/* Enumerate sessions from all known agents on the local machine.
 *   tools_mask: bitwise OR of (1 << ChatTool). 0 → all.
 *   arena is used for transient allocations during enumeration. */
void chat_scan(Arena *arena, u32 tools_mask,
               chat_session_cb cb, void *user);

/* As chat_scan, with a cancel flag checked between the per-tool stores: a
 * scan a worker was asked to abandon stops at the next store boundary rather
 * than walking every directory it was going to. NULL = never cancelled.
 * A cancelled scan still calls `cb` for nothing and returns normally. */
bool chat_scan_cancellable(Arena *arena, u32 tools_mask,
                           chat_session_cb cb, void *user,
                           const _Atomic bool *cancel);

/* Create a fresh arena of CHAT_SCAN_ARENA_BYTES. Caller owns arena_destroy.
 * Prefer chat_scan_arena_borrow() on the UI main thread to avoid mmap/munmap
 * on every palette open / transcript resolve. */
Arena chat_scan_arena_create(void);

/* Main-thread reusable scan arena: maps CHAT_SCAN_ARENA_BYTES once, then
 * arena_reset on each call. NOT thread-safe; do not nest while holding
 * ChatSessionMeta string pointers from a previous borrow (they are
 * invalidated by the next call). Returns NULL on allocation failure.
 * Mapping is freed by chat_scan_shutdown(). */
Arena *chat_scan_arena_borrow(void);

/* chat_scan using the borrowed scan arena. Meta string fields are only
 * valid during `cb` (copy what you need) or until the next borrow. */
void chat_scan_with_arena(u32 tools_mask, chat_session_cb cb, void *user);

/* Free process-lifetime scanner state (reusable scan arena, etc.). Call
 * once at shutdown; idempotent. */
void chat_scan_shutdown(void);

/* Does this session belong to `cwd`? Use this instead of comparing
 * meta->cwd yourself: it is the one place that knows a NULL cwd is not
 * always "unknown". Tools that slug the working directory into their
 * project dir name (CommandCode) are matched in the slug direction, which
 * survives directory names containing dashes.
 *
 * `cwd` must be absolute; trailing slashes are ignored. Returns false when
 * the session's directory genuinely cannot be established. */
bool chat_session_matches_cwd(const ChatSessionMeta *meta, const char *cwd);

/* Convenience: find the first session whose id or path matches `needle`.
 * `needle` may be a UUID, a bare basename, a relative path, or an absolute path.
 * On success fills `out` and returns true. */
bool chat_find_session(Arena *arena, const char *needle,
                       ChatSessionMeta *out);

#endif
