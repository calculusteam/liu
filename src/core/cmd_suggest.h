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
 * Online learning: every executed command updates the
 * counters (observe), every accepted/ignored ghost updates accept/reject
 * (feedback). The model is always reconstructible from the log — rebuilding
 * replays the CmdHistory snapshot through the same observe path, so live
 * state is reconstructed over the most recent history window.
 *
 * Safety contract: when the model's confidence margin over the plain
 * newest-first prefix match is small, the newest-first match wins — the
 * engine never does worse than that match for prefixes >= 2 chars.
 * (For 1-char prefixes a confidence gate may show NOTHING where newest-first
 * would guess: no ghost is preferred to a wrong ghost.)
 *
 * Feedback counters that can't be derived from the log persist in a small
 * sidecar (~/.config/Liu/projects/<slug>/cmdsuggest), written atomically
 * (tmp + rename); a missing or corrupt sidecar just degrades to pure
 * log-derived scoring.
 */
#ifndef CORE_CMD_SUGGEST_H
#define CORE_CMD_SUGGEST_H

#include "core/types.h"
#include "core/cmd_history.h"   /* CmdHistoryKey */
#include <stdbool.h>

/* Best full command for the typed prefix, or NULL when nothing confident
 * enough exists. The returned pointer lives in the engine's arena — copy it
 * before the next observe/rebuild. `dir` is the shell's cwd (history key). */
const char *cmd_suggest_best(const char *dir, const char *prefix, i32 prefix_len);

/* Feed one executed command after a successful append. Both stat keys are
 * sampled under the writer lock. Incremental learning is safe only if the
 * model already includes the pre-append log; otherwise best() must replay it. */
void cmd_suggest_observe(const char *dir, const char *cmd,
                         const CmdHistoryKey *previous,
                         const CmdHistoryKey *now);

/* Ghost outcome for the suggestion last surfaced in `dir`:
 * accepted=true  → the user took it (accepts++, shows++)
 * accepted=false → the user submitted something else (rejects++, shows++)
 * Episode-granular by construction: called once per prompt episode. */
void cmd_suggest_feedback(const char *dir, const char *cmd, bool accepted);

/* Flush the dirty sidecar + free the model. Call once at shutdown. */
void cmd_suggest_shutdown(void);

/* Nonblocking UI service. Results are copied and keyed to the complete
 * request; superseded queries never replace a newer prompt's suggestion.
 * Same-key polls coalesce through slow I/O. Cached answers refresh after one
 * second from completion, or when a command changes that directory. Under
 * continuous writes, replacements publish a snapshot through a fixed write
 * fence and schedule newer snapshots without discarding every answer. */
bool cmd_suggest_async_best(const char *dir, const char *prefix, i32 len,
                            char *out, usize cap);
void cmd_suggest_async_record(const char *dir, const char *command);
void cmd_suggest_async_record_result(const char *dir, const char *command,
                                     i32 exit_code, f64 duration);
void cmd_suggest_async_feedback(const char *dir, const char *command, bool accepted);
/* Copies five distinct commands from the latest completed snapshot. Repeated
 * refreshes coalesce at a fixed write fence; later accepted writes requested
 * by a refresh get a follow-up. A snapshot may be served while that follow-up
 * is pending. false means no completed snapshot for this directory yet. */
bool cmd_suggest_async_history(const char *dir, char out[5][4096], i32 *count, bool refresh);
/* Optional thread-safe event-loop wake, invoked after publishing results. */
void cmd_suggest_async_set_wake(void (*wake)(void));
bool cmd_suggest_async_notice(void);
void cmd_suggest_async_shutdown(void);
#endif /* CORE_CMD_SUGGEST_H */
