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

/* Drive parsing over an entire file via a callback. `tool` selects the parser.
 * The callback is invoked once per emitted event; arena is reset periodically
 * to cap memory usage. Returns total events emitted. Callback returning false
 * stops iteration. */
typedef bool (*chat_event_cb)(const ChatEvent *e, void *user);
u64 parser_run_file(Arena *arena, ChatTool tool, const char *path,
                    chat_event_cb cb, void *user);

/* Whole-file parser for Cline/Roo-Code/Kilo-Code (single JSON array, not JSONL).
 * Emits events directly via the callback — does not share the line-oriented
 * pipeline. `tool` should be one of CHAT_TOOL_CLINE/ROO/KILO so origin_tool
 * is tagged correctly. */
u64 parser_cline_run(Arena *arena, ChatTool tool, const char *path,
                     chat_event_cb cb, void *user);

#endif
