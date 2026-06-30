/*
 * Liu - CLI agent autodetect
 *
 * Scans PATH for known coding-agent binaries (claude, codex, cursor-agent,
 * amp, cline) and reports which ones are usable.  Used by
 * the Cmd+K "Create Theme..." flow to populate the agent picker — only
 * agents the user actually has installed are shown.
 */
#ifndef CORE_AGENT_DETECT_H
#define CORE_AGENT_DETECT_H

#include "core/types.h"

#define AGENT_MAX         16
#define AGENT_ID_CAP      16
#define AGENT_DISPLAY_CAP 32
#define AGENT_BINARY_CAP  32
#define AGENT_PATH_CAP    1024
#define AGENT_ARG_CAP     32
#define AGENT_MAX_ARGS    8

typedef struct AgentInfo AgentInfo;
struct AgentInfo {
    char id[AGENT_ID_CAP];          /* short id, e.g. "claude"     */
    char display[AGENT_DISPLAY_CAP];/* user-facing label           */
    char binary[AGENT_BINARY_CAP];  /* binary name to invoke       */
    char path[AGENT_PATH_CAP];      /* resolved absolute path      */
    /* Argv prelude: argv[0]=binary, then args[0..args_count-1], then the
     * prompt (or stdin if stdin_prompt). Codex needs multiple flags
     * (`exec --color never --skip-git-repo-check`); single-flag agents
     * just use args_count == 1. */
    char args[AGENT_MAX_ARGS][AGENT_ARG_CAP];
    i32  args_count;
    bool stdin_prompt;
};

/* Fill `out` with up to `cap` detected agents. Returns the count.
 * Order is the canonical preference order (claude first, then codex, …)
 * filtered down to what's actually on PATH. */
i32 agent_detect_available(AgentInfo *out, i32 cap);

/* Look up the canonical agent id for a process basename (as reported by
 * session_fg_process). Returns "" if `name` is NULL or not a known agent.
 * Returned pointer is a static string and stays valid for the program's
 * lifetime — callers must not free or modify it. */
const char *agent_id_for_basename(const char *name);

#endif /* CORE_AGENT_DETECT_H */
