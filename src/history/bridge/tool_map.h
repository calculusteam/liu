/*
 * Normalize tool names and arguments across supported agent session formats so
 * history can preserve file operations, shell commands, and tool-call results.
 */
#ifndef HISTORY_BRIDGE_TOOL_MAP_H
#define HISTORY_BRIDGE_TOOL_MAP_H

#include "history/bridge/ir.h"
#include "history/event.h"

/* Forward-declared so a caller need not pull in cJSON just to pass a pointer. */
typedef struct cJSON cJSON;

/* Normalize a tool call name as written by `tool`. `name` may be NULL.
 * Unrecognised names return BRIDGE_TOOL_UNKNOWN — never a guess, because a
 * wrong guess makes a target rename a call to something it does not do. */
BridgeToolKind bridge_tool_kind_for(ChatTool tool, const char *name);

/* The name `tool` would itself use for `kind`, or NULL when that agent has no
 * equivalent (or the kind is UNKNOWN). Adapters that get NULL must fall back
 * to the event's original tool_name — the record is history, so an unfamiliar
 * name is still better than a call with no name at all. */
const char *bridge_tool_name_for(ChatTool tool, BridgeToolKind kind);

/* Stable lowercase identifier, for lineage records, handoff documents and
 * tests: "file_read", "shell", … Never NULL. */
const char *bridge_tool_kind_id(BridgeToolKind kind);

/* Human-readable verb used in the context-handoff document: "read", "ran a
 * shell command", … Never NULL. */
const char *bridge_tool_kind_verb(BridgeToolKind kind);

/* True when a call of this kind mutates the user's machine. The handoff
 * writer surfaces these separately (what the previous agent actually CHANGED
 * is the part a fresh agent cannot re-derive), and no adapter may ever
 * execute one — see bridge.h. */
bool bridge_tool_kind_is_mutating(BridgeToolKind kind);

/* Which file a recorded tool input names, and which command it ran. Every
 * agent's tools spell the argument differently, so each is a sweep over the
 * known spellings rather than a per-agent table: the tool KIND already says
 * the call touched a file or ran a command, and the only question left is
 * which one.
 *
 * `input` is the call's parsed arguments object. Returns the KEY the value
 * was found under (a static string) or NULL when the input has none; the
 * value is copied to `out`, cut on a code-point boundary when it does not
 * fit. A command passed as argv (Codex) comes back joined with spaces.
 *
 * Two readers share these so they cannot disagree: ir.c, which keeps the pair
 * beside the body of an input too large to store, and the handoff, which
 * lists them. */
const char *bridge_tool_input_path   (const cJSON *input, char *out, usize cap);
const char *bridge_tool_input_command(const cJSON *input, char *out, usize cap);

#endif /* HISTORY_BRIDGE_TOOL_MAP_H */
