/*
 * liu-history - session metadata and discovery callback API.
 */
#ifndef HISTORY_SESSION_H
#define HISTORY_SESSION_H

#include "core/memory.h"
#include "core/types.h"
#include "history/event.h"

/* Internal scratch buffer cap used while building paths inside the scanner.
 * The public ChatSessionMeta strings are arena-owned slices of variable
 * length, so consumers don't see this cap. */
#define CHAT_PATH_CAP      4096

/* Session metadata. The string fields are arena-allocated by the scanner /
 * find helper; their lifetime is tied to the Arena passed in by the caller.
 * After the caller calls arena_destroy on that arena, these pointers are
 * invalid — copy what you need before destroying. */
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

/* Free process-lifetime scanner caches (e.g. the Gemini projects map). Call
 * once at shutdown; idempotent. */
void chat_scan_shutdown(void);

/* Convenience: find the first session whose id or path matches `needle`.
 * `needle` may be a UUID, a bare basename, a relative path, or an absolute path.
 * On success fills `out` and returns true. */
bool chat_find_session(Arena *arena, const char *needle,
                       ChatSessionMeta *out);

#endif
