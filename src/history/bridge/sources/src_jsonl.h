/*
 * liu-history/bridge - line driver shared by the bridge-native JSONL parsers.
 *
 * Six of the seven stores here are JSONL and every one of them wants the same
 * three things: O_NOFOLLOW so a symlink under the agent's store cannot
 * redirect the read, a bounded line so a corrupt file cannot be turned into
 * an allocation, and a scratch arena that is reset per line so peak memory is
 * one line rather than one file. parser_run.c already does exactly this for
 * ChatEvent; this is the same driver with a callback that can see the
 * BridgeHistory.
 */
#ifndef HISTORY_BRIDGE_SRC_JSONL_H
#define HISTORY_BRIDGE_SRC_JSONL_H

#include "core/memory.h"
#include "core/types.h"
#include "history/bridge/ir.h"

/* Called once per non-empty line. `root` is the parsed cJSON for that line
 * (never NULL — unparseable lines are skipped and counted, not delivered).
 * `scratch` is reset before each call. Return false to stop reading. */
typedef bool (*bridge_jsonl_cb)(void *root_cjson, Arena *scratch,
                                BridgeHistory *h, void *user);

/* Read `path` line by line. Returns the number of lines DELIVERED; lines that
 * failed to parse are counted into `*out_bad` (may be NULL) so a caller can
 * tell "empty file" from "truncated JSONL", which the IR reports as a
 * fidelity loss rather than pretending the session simply ended there. */
u64 bridge_jsonl_run(const char *path, BridgeHistory *h,
                     bridge_jsonl_cb cb, void *user, u64 *out_bad);

/* Slurp a whole JSON document from `path` into a heap buffer and parse it.
 * Returns the cJSON root (caller must cJSON_Delete) or NULL. Enforces
 * O_NOFOLLOW and `max_bytes`. Used by the non-JSONL stores (Amp, Kiro,
 * Droid, Cursor metadata). */
void *bridge_json_slurp(const char *path, usize max_bytes);

#endif /* HISTORY_BRIDGE_SRC_JSONL_H */
