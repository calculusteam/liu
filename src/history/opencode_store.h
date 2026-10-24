/*
 * OpenCode 1.x stores sessions, messages, and parts in session, message, and
 * part tables. The 2.x format uses session_v2 and session_message, with
 * message type, sequence, and JSON data. Scanners, continuation, and history
 * cleanup use this schema check; if both table sets exist, the 2.x tables are
 * treated as live.
 *
 * A 2.x session with parent_id is a sub-agent run and is omitted from the
 * session list, like Claude sidechains and Codex sub-agent rollouts.
 */
#ifndef HISTORY_OPENCODE_STORE_H
#define HISTORY_OPENCODE_STORE_H

#include "sqlite3.h"

#include <stdbool.h>

static inline bool opencode_store_is_v2(sqlite3 *db) {
    if (!db) return false;
    sqlite3_stmt *s = NULL;
    bool v2 = false;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='session_v2'",
            -1, &s, NULL) == SQLITE_OK) {
        v2 = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_finalize(s);
    }
    return v2;
}

#endif /* HISTORY_OPENCODE_STORE_H */
