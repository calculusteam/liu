/*
 * liu-history/bridge - source parsers: agent session store -> BridgeHistory.
 *
 * Two families, and the split is deliberate.
 *
 * (1) The eight agents Liu already parses (Claude, Codex, Copilot,
 *     Antigravity, Grok, CommandCode, Cline extension, Cline CLI) keep their
 *     existing ChatEvent parsers untouched and reach the IR through
 *     bridge_source_from_chatevents(). Rewriting them would risk the
 *     transcript viewer for no gain — except where ChatEvent is structurally
 *     unable to carry what a resume needs, which is the tool call/result
 *     pairing. For those, a bridge-native parser reads the same file again;
 *     see the per-tool notes in sources.c.
 *
 * (2) The seven agents Liu SCANS but never parsed (Amp, Kiro, Cursor, Droid,
 *     OpenCode, Kimi, Qwen) get bridge-native parsers here. They produce
 *     BridgeHistory directly and are not wired into ChatEvent, because the
 *     transcript viewer's needs and a migration's needs have diverged and one
 *     parser serving both would be the worse for it.
 */
#ifndef HISTORY_BRIDGE_SOURCES_H
#define HISTORY_BRIDGE_SOURCES_H

#include "core/memory.h"
#include "history/bridge/ir.h"
#include "history/session.h"

/* Drive the EXISTING ChatEvent parser for `meta->tool` over `meta->path` and
 * fold the result into `h`. Returns the number of events pushed.
 *
 * The fold is not a rename: ChatEvent stores a tool's NAME on a TOOL_USE and
 * the call's ID on the matching TOOL_RESULT, in the same `tool_name` field.
 * This reconstructs the pairing by remembering the last call, which is exact
 * for every parser in the tree because they all emit a result immediately
 * after its call within one line, or in the next line of the same file. */
u32 bridge_source_from_chatevents(Arena *arena, const ChatSessionMeta *meta,
                                  BridgeHistory *h);

/* Bridge-native parsers. Each fills an already-initialized `h` and returns
 * true when it produced at least one message. */
bool bridge_parse_opencode(Arena *a, const ChatSessionMeta *m, BridgeHistory *h);
bool bridge_parse_kimi    (Arena *a, const ChatSessionMeta *m, BridgeHistory *h);
bool bridge_parse_cursor  (Arena *a, const ChatSessionMeta *m, BridgeHistory *h);
bool bridge_parse_qwen    (Arena *a, const ChatSessionMeta *m, BridgeHistory *h);
bool bridge_parse_amp     (Arena *a, const ChatSessionMeta *m, BridgeHistory *h);
bool bridge_parse_kiro    (Arena *a, const ChatSessionMeta *m, BridgeHistory *h);
bool bridge_parse_droid   (Arena *a, const ChatSessionMeta *m, BridgeHistory *h);

/* Bridge-native re-reads of stores Liu already parses into ChatEvent, added
 * only where the pairing or the tool arguments cannot survive that model. */
bool bridge_parse_claude  (Arena *a, const ChatSessionMeta *m, BridgeHistory *h);
bool bridge_parse_commandcode(Arena *a, const ChatSessionMeta *m, BridgeHistory *h);
bool bridge_parse_codex   (Arena *a, const ChatSessionMeta *m, BridgeHistory *h);
bool bridge_parse_grok    (Arena *a, const ChatSessionMeta *m, BridgeHistory *h);

/* Shared helpers for the parsers above. */

/* Flatten an Anthropic-shaped content value (string, or an array of
 * {type:text|thinking|tool_use|tool_result} blocks) into `h`, emitting one IR
 * event per block. `role_type` is the event type for plain text blocks.
 * Returns the number of events pushed.
 *
 * `thinking` blocks are DROPPED. Hidden reasoning is not something the origin
 * tool intended to be replayed into a different model — Grok even stores it
 * encrypted — and a target that re-serializes it as assistant text would
 * present another model's private scratchpad as this conversation's record. */
u32 bridge_push_anthropic_content(BridgeHistory *h, ChatTool src,
                                  void *content_cjson, BridgeEventType role_type,
                                  i64 ts_ms);

/* The short note that stands in a transcript for an image or a document
 * block — "[image: image/png, 48213 bytes]" — because the payload cannot go
 * to another agent and a run of base64 is of no use to it. Reads the
 * Anthropic shape ({type:"image"|"document", source:{media_type, data|url}})
 * and the Responses one ({type:"input_image", image_url:"data:…"}). Returns
 * false, and writes nothing, when `part` is some other kind of block.
 *
 * Replacing a block with its note is a LOSS: the caller counts it
 * (events_dropped for something the user sent, tool_output_truncated for a
 * tool's result). */
bool bridge_media_note(void *part_cjson, char *out, usize cap);

/* Compact-serialize a cJSON node into `scratch` (arena) for use as a tool
 * input. NULL when the node is absent or empty. */
const char *bridge_json_compact(Arena *a, void *node_cjson);

#endif /* HISTORY_BRIDGE_SOURCES_H */
