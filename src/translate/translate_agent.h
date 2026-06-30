/*
 * Liu - Translate-on-Tab: agent (fork+exec) backend.
 *
 * Spawns the configured CLI agent (`claude -p PROMPT`, `codex exec …`,
 * etc.) and returns a non-blocking pipe FD + pid. Mirrors the
 * Create-Theme pattern in main.c — the caller drains and reaps in
 * app_tick_translate.
 */
#ifndef TRANSLATE_TRANSLATE_AGENT_H
#define TRANSLATE_TRANSLATE_AGENT_H

#include "core/agent_detect.h"
#include "core/types.h"
#include "translate/translate.h"

typedef struct TranslateAgentSpawn {
    i32 child_pid;       /* >0 on success */
    i32 stdout_fd;       /* read end of the pipe; non-blocking. -1 on failure */
} TranslateAgentSpawn;

/* Look up the configured agent in the detected-agents list and fork+exec
 * it with the canonical translation prompt. Returns true on success and
 * fills `out` with pid/fd. Returns false if the agent is not installed
 * (caller surfaces a toast).
 *
 * The agent CLI is resolved via agent_detect_available() so we honour
 * GUI-launch PATH quirks (Finder hands apps a stripped PATH). */
bool translate_agent_spawn(const TranslateConfig *cfg,
                           const char *text,
                           TranslateAgentSpawn *out);

/* Trim leading/trailing whitespace + ANSI escape sequences from the
 * agent's captured stdout and copy the result into `out`. Always
 * NUL-terminates. Returns false if the trimmed string is empty. */
bool translate_agent_finalize(const char *log, i32 log_len,
                              char *out, usize out_cap);

#endif /* TRANSLATE_TRANSLATE_AGENT_H */
