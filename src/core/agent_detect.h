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
    /* Can this agent be run as a text function (Create Theme, Create Note, the
     * Translate agent backend): a prompt in argv, only the answer on stdout, and
     * a flag set that keeps its tools out of it (core/agent_headless.h)? An agent
     * is listed here for other reasons too — its notify hook, the Translate
     * chord in its pane — and those do not make it safe to hand a prompt to. */
    bool headless;
};

/* Repair the process PATH once, early in startup, so agents Liu posix_spawns
 * (Create Theme, Create Note, the Translate agent backend) inherit a usable
 * PATH. macOS GUI launches (.app / Finder / Dock / `open -a`) inherit launchd's
 * stripped PATH, which omits Homebrew, npm-global, nvm, etc.; a spawned
 * `claude` (a `#!/usr/bin/env node` wrapper) then can't find `node` and dies
 * silently. This pulls the login shell's real PATH and folds in curated
 * install dirs, then setenv()s the result. No-op on non-macOS. */
void agent_ensure_process_path(void);

/* How long the login shell gets to answer. A plain zsh or bash takes 0.1-0.3 s;
 * past this the probe is abandoned and Liu starts with the PATH it has plus
 * the curated install dirs. */
#define AGENT_LOGIN_PATH_DEADLINE_MS 3000

/* The PATH a login+interactive `shell` ends up with, or 0 when there is no
 * usable answer inside `deadline_ms` (the shell is killed then). Runs the
 * user's rc files, so it is bounded and takes only what it asked for. */
usize agent_login_shell_path(const char *shell, char *out, usize cap, i32 deadline_ms);

/* The answer inside a probe's raw output: the text between the last begin
 * marker and the end marker after it, refused when it is cut short, empty,
 * too long for `out` or holds a control byte. Pure. */
usize agent_login_path_extract(const char *buf, usize len, char *out, usize cap);

/* Fill `out` with up to `cap` detected agents. Returns the count.
 * Order is the canonical preference order (claude first, then codex, …)
 * filtered down to what's actually on PATH. */
i32 agent_detect_available(AgentInfo *out, i32 cap);

/* The same list, narrowed to the agents that can be handed a prompt and asked
 * for text (AgentInfo.headless): Create Theme / Create Note choose among
 * these. */
i32 agent_detect_headless(AgentInfo *out, i32 cap);

/* Look up the canonical agent id for a process basename (as reported by
 * session_fg_process). Returns "" if `name` is NULL or not a known agent.
 * Returned pointer is a static string and stays valid for the program's
 * lifetime — callers must not free or modify it. */
const char *agent_id_for_basename(const char *name);

#endif /* CORE_AGENT_DETECT_H */
