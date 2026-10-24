/*
 * liu-history - per-agent JSONL line parsers.
 *
 * Each parser consumes exactly one line of the session file and emits 0..N
 * `ChatEvent` records into caller-provided buffer. Non-event record types
 * (snapshots, permission flips, etc.) return 0.
 *
 * All strings written into the event array are owned by `arena` and remain
 * valid until `arena_reset(arena)`.
 */
#ifndef HISTORY_PARSER_H
#define HISTORY_PARSER_H

#include "core/memory.h"
#include "core/types.h"
#include "history/event.h"

#include <stddef.h>

u32 parser_claude_line      (Arena *arena, const char *line, usize len,
                             ChatEvent *out, u32 out_cap);

u32 parser_codex_line       (Arena *arena, const char *line, usize len,
                             ChatEvent *out, u32 out_cap);

u32 parser_copilot_line     (Arena *arena, const char *line, usize len,
                             ChatEvent *out, u32 out_cap);

u32 parser_antigravity_line (Arena *arena, const char *line, usize len,
                             ChatEvent *out, u32 out_cap);

u32 parser_grok_line        (Arena *arena, const char *line, usize len,
                             ChatEvent *out, u32 out_cap);

u32 parser_commandcode_line (Arena *arena, const char *line, usize len,
                             ChatEvent *out, u32 out_cap);

/* Pi and Oh My Pi (CHAT_TOOL_PI / CHAT_TOOL_OMP) write one layout; `tool` tags
 * the events with whichever wrote the file. */
u32 parser_pi_line          (Arena *arena, ChatTool tool, const char *line, usize len,
                             ChatEvent *out, u32 out_cap);

/* Drive parsing over an entire file via a callback. `tool` selects the parser.
 * The callback is invoked once per emitted event; arena is reset periodically
 * to cap memory usage. Returns total events emitted. Callback returning false
 * stops iteration. */
typedef bool (*chat_event_cb)(const ChatEvent *e, void *user);
u64 parser_run_file(Arena *arena, ChatTool tool, const char *path,
                    chat_event_cb cb, void *user);

/* What a session is called in a list: the first thing the user asked in it,
 * as one line (history/session_title.h has the rule). Reads the START of the
 * file only — it stops at the first real prompt, or after
 * CHAT_TITLE_MAX_EVENTS records, CHAT_TITLE_MAX_LINES lines or
 * CHAT_TITLE_MAX_BYTES bytes, whichever comes first. The line and byte limits
 * are what bound a session with NO prompt: a Codex sub-agent's rollout is
 * records the parser drops, so the record limit is never reached and without
 * them the whole file would be read to name nothing. In real Codex and Claude
 * Code stores the first prompt sits well inside these limits. `line_arena` is
 * the caller's scratch (reset per line; 4 MB is what the transcript loader
 * uses).
 *
 * False, with `out` empty, when: the tool keeps no file of its own this can
 * read (OpenCode's sessions are rows of one SQLite file, and reading THAT
 * line by line would read gigabytes); the session is one of the whole-file
 * kinds and larger than CHAT_TITLE_WHOLE_FILE_MAX; or nothing the user typed
 * was found. */
#define CHAT_TITLE_MAX_EVENTS      400u
#define CHAT_TITLE_MAX_LINES       2000u
#define CHAT_TITLE_MAX_BYTES       (4u * 1024u * 1024u)
#define CHAT_TITLE_WHOLE_FILE_MAX  (8 * 1024 * 1024)
bool chat_session_title(Arena *line_arena, ChatTool tool, const char *path, i64 size_bytes,
                        char *out, usize cap);
/* The same with the limits spelled out (0 = none) and a report of how far the
 * read went, for measuring a real store. */
typedef struct { u32 lines; u64 bytes, events; } ChatTitleStats;
bool chat_session_title_ex(Arena *line_arena, ChatTool tool, const char *path, i64 size_bytes,
                           char *out, usize cap, u32 max_lines, u64 max_bytes, ChatTitleStats *st);

/* Whole-file parser for Cline/Roo-Code/Kilo-Code (single JSON array, not JSONL).
 * Emits events directly via the callback — does not share the line-oriented
 * pipeline. `tool` should be one of CHAT_TOOL_CLINE/ROO/KILO so origin_tool
 * is tagged correctly. */
u64 parser_cline_run(Arena *arena, ChatTool tool, const char *path,
                     chat_event_cb cb, void *user);

/* Whole-file parser for the standalone Cline CLI (~/.cline/data/sessions).
 * An Anthropic-shaped {messages:[…]} object, unrelated to the extension's
 * ui_messages.json above — see parser_cline_cli.c. */
u64 parser_cline_cli_run(Arena *arena, const char *path,
                         chat_event_cb cb, void *user);

/* Whole-file parser for Antigravity (SQLite DB with protobuf-encoded steps).
 * Opens the .db at `path`, reads the steps table, and extracts user/assistant
 * text from the protobuf blobs. */
u64 parser_antigravity_run(Arena *arena, const char *path,
                           chat_event_cb cb, void *user);

#endif
