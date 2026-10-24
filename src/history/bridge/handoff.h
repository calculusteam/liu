/*
 * liu-history/bridge - structured context handoff.
 *
 * The fallback for agents with no safe way to seed native history. It is NOT
 * "paste the transcript into a prompt": a conversation of many megabytes
 * concatenated into one user message is unusable, blows past every context
 * limit, and buries the two things the next agent actually needs — what the
 * user asked for and what has already been changed on disk.
 *
 * What is produced instead is a compact briefing document, written to Liu's
 * own state directory and handed to the target as a FILE REFERENCE rather
 * than as inline text (which also side-steps ARG_MAX and shell quoting):
 *
 *     Session objective          the first user turn, verbatim
 *     Important user requests    every later user turn, clipped per turn
 *     Current task               the last user turn
 *     Decisions made             the assistant's closing conclusions
 *     Files touched              derived from mutating tool calls
 *     Tool actions               counts per semantic kind, not a log
 *     Unresolved issues          tool calls with no recorded result
 *     Relevant output            the tail of the last few tool outputs
 *     Git state                  branch / head / root when the source had it
 *
 * The UI must label this "Context Handoff" and never "Resume". A handoff
 * gives the next agent a briefing; it does not give it the conversation.
 */
#ifndef HISTORY_BRIDGE_HANDOFF_H
#define HISTORY_BRIDGE_HANDOFF_H

#include "history/bridge/bridge.h"
#include "history/bridge/ir.h"

/* Write the briefing for `h` to `out_path` (filled by the call, under Liu's
 * state dir, 0600). Returns false when nothing could be written. */
bool bridge_handoff_write(const BridgeHistory *h, char *out_path, usize cap);

/* The one-line instruction a target CLI is launched with, pointing at the
 * document. Plain text — the caller is responsible for getting it onto the
 * command line safely. */
void bridge_handoff_prompt(const char *doc_path, ChatTool source,
                           char *out, usize cap);

/* The shared CONTEXT_HANDOFF import_history body: writes the document, fills
 * `r`, and builds `r->launch_cmd` from `cmd_fmt` (a printf format with one
 * %s that receives the single-quoted prompt). Every handoff-only adapter is
 * three lines around this. */
bool bridge_handoff_import(const BridgeHistory *h, BridgeImportResult *r,
                           const char *cmd_fmt);

/* Validation for a handoff: the document exists and is non-empty. There is
 * no target session to check, and claiming otherwise would be the "fake
 * success" this subsystem exists to avoid. */
bool bridge_handoff_validate(const BridgeImportResult *r);

#endif /* HISTORY_BRIDGE_HANDOFF_H */
