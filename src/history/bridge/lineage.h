/*
 * Each migration creates a new target session and leaves the source
 * unchanged, so a chain of migrations spans several stores. This append-only
 * journal links source and target session IDs for Agent History. Each line
 * is one JSON object at ~/.config/Liu/agent-bridges.jsonl. Appending avoids
 * loading and rewriting the full journal, so an interrupted write does not
 * replace earlier records.
 */
#ifndef HISTORY_BRIDGE_LINEAGE_H
#define HISTORY_BRIDGE_LINEAGE_H

#include "core/types.h"
#include "history/bridge/bridge.h"
#include "history/event.h"

#define BRIDGE_LINEAGE_ID_CAP 128

typedef struct {
    ChatTool source_tool;
    char     source_session_id[BRIDGE_LINEAGE_ID_CAP];
    ChatTool target_tool;
    char     target_session_id[BRIDGE_LINEAGE_ID_CAP];
    i64      created_at_ms;
    BridgeImportMode mode;
    BridgeFidelity   fidelity;
} BridgeLineage;

/* Append one record. Never fails loudly: a migration that worked must not be
 * reported as failed because a journal line could not be written. Returns
 * false so the caller can count it as a warning. */
bool bridge_lineage_record(const BridgeLineage *rec);

/* Walk the journal newest-first, at most `limit` records. Returning false
 * from `cb` stops the walk. Returns the number visited. */
typedef bool (*bridge_lineage_cb)(const BridgeLineage *rec, void *user);
u32 bridge_lineage_scan(bridge_lineage_cb cb, void *user, u32 limit);

/* What was `session_id` (belonging to `tool`) continued INTO? Fills `out`
 * with the most recent migration whose source matches. False when none. */
bool bridge_lineage_find_forward(ChatTool tool, const char *session_id,
                                 BridgeLineage *out);

/* Where did `session_id` come FROM? The inverse lookup, for a target session
 * that wants to name its ancestor. */
bool bridge_lineage_find_back(ChatTool tool, const char *session_id,
                              BridgeLineage *out);

/* Write the absolute path of the lineage journal to out. */
bool bridge_lineage_path(char *out, usize cap);

#endif /* HISTORY_BRIDGE_LINEAGE_H */
