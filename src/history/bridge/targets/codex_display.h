/*
 * The display half of a thread Codex's session import wrote.
 *
 * Codex's import (externalAgentConfig/import) keeps a session's tool calls as
 * text: a call becomes an "[external_agent_tool_call: <name>]" note and its
 * result an "[external_agent_tool_result]" note, in both layers of the
 * rollout — the Responses items the model reads and the event_msg records
 * Codex draws its screen from. This rewrites the second layer and nothing
 * else.
 *
 * A call Codex keeps a record of its own for in such a thread becomes that
 * record: a file edit `patch_apply_end` (drawn as "Edited", with the diff the
 * source recorded), an MCP call `mcp_tool_call_end`, a web search or an
 * opened page `web_search_end`. Codex keeps no record of a command, a read or
 * a search in an imported thread, so those become agent messages laid out as
 * Codex lays out its own cells: "Ran <command>" over the first and last lines
 * of the output, "Explored" over the reads and searches in a row, "Updated
 * Plan" over a todo list. The line Codex adds to say the session was imported
 * is left off the screen.
 *
 * The calls come from the IR, matched to the notes in order by name, so a
 * cell carries the whole call where the note carried a clipped summary. A
 * note that matches no call stays as Codex wrote it.
 */
#ifndef HISTORY_BRIDGE_TARGETS_CODEX_DISPLAY_H
#define HISTORY_BRIDGE_TARGETS_CODEX_DISPLAY_H

#include "history/bridge/ir.h"

typedef struct {
    u32 shown;            /* call notes replaced by a cell                    */
    u32 kept;             /* call notes left as text: no call matched         */
    u32 marker_dropped;   /* the import's own closing line, taken off screen  */
    /* What Codex's reader must find in the rewritten thread, kind by kind. */
    u32 user_messages;
    u32 agent_messages;
    u32 file_changes;
    u32 mcp_calls;
    u32 web_searches;
} CodexDisplayCounts;

/* Rewrite `rollout` (`len` bytes of the JSONL Codex's import wrote) for the
 * history `h` it was imported from. True when `*out` holds a changed rollout
 * (malloc'd, `*out_len` bytes, the caller frees it); false when there was
 * nothing to change or the rewrite could not be built, with `*out` NULL.
 * `counts` is filled either way. */
bool codex_display_rewrite(const char *rollout, usize len, const BridgeHistory *h,
                           char **out, usize *out_len, CodexDisplayCounts *counts);

#endif /* HISTORY_BRIDGE_TARGETS_CODEX_DISPLAY_H */
