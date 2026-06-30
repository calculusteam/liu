/*
 * Liu — learned command suggestion engine.
 *
 * Sits ON TOP of cmd_history (which stays the single durable, append-only
 * log). This module derives an in-memory model per directory and ranks
 * prefix candidates by what the user actually does, not just what they did
 * last:
 *
 *   score(c) = 2.0·log1p(freq_decayed)            exponential recency decay
 *            + 3.0·log1p(P(c | previous command)) Markov transition counts
 *            + 1.5·λ^(events since last use)
 *            + 2.5·accept_rate                    learned from ghost accepts
 *            − 2.0·reject_rate                    learned from ignored ghosts
 *
 * Online learning, all O(1) per event: every executed command updates the
 * counters (observe), every accepted/ignored ghost updates accept/reject
 * (feedback). The model is always reconstructible from the log — rebuilding
 * replays the CmdHistory snapshot through the same observe path, so live
 * state and rebuilt state are identical by construction.
 *
 * Safety contract: when the model's confidence margin over the plain
 * newest-first prefix match is small, the newest-first match wins — the
 * engine is never worse than the old behavior for prefixes >= 2 chars.
 * (For 1-char prefixes a confidence gate may show NOTHING where the old
 * code guessed — a deliberate "no ghost beats a wrong ghost" trade.)
 *
 * Feedback counters that can't be derived from the log persist in a small
 * sidecar (~/.config/Liu/projects/<slug>/cmdsuggest), written atomically
 * (tmp + rename); a missing or corrupt sidecar just degrades to pure
 * log-derived scoring.
 */
#ifndef CORE_CMD_SUGGEST_H
#define CORE_CMD_SUGGEST_H

#include "core/types.h"
#include <stdbool.h>

/* Best full command for the typed prefix, or NULL when nothing confident
 * enough exists. The returned pointer lives in the engine's arena — copy it
 * before the next observe/rebuild. `dir` is the shell's cwd (history key). */
const char *cmd_suggest_best(const char *dir, const char *prefix, i32 prefix_len);

/* Feed one executed command. Called from cmd_history_record AFTER the log
 * append + cache refresh; mtime/size are the post-append stat so the model
 * adopts the same freshness key and the next keystroke stays a cache hit. */
void cmd_suggest_observe(const char *dir, const char *cmd,
                         i64 log_mtime, i64 log_size);

/* Ghost outcome for the suggestion last surfaced in `dir`:
 * accepted=true  → the user took it (accepts++, shows++)
 * accepted=false → the user submitted something else (rejects++, shows++)
 * Episode-granular by construction: called once per prompt episode. */
void cmd_suggest_feedback(const char *dir, const char *cmd, bool accepted);

/* Flush the dirty sidecar + free the model. Call once at shutdown. */
void cmd_suggest_shutdown(void);

#endif /* CORE_CMD_SUGGEST_H */
