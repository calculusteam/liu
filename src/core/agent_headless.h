/*
 * Liu — what a HEADLESS helper run of an agent CLI is allowed to do.
 *
 * Translate and Create Theme borrow the user's installed agent as a text
 * function: a prompt goes in, text comes out. Nothing in either needs a tool.
 * But `claude -p` with no further flags starts the whole agent, with its
 * built-in tools (Agent, Bash, Edit, Read, Write, Workflow) and every MCP tool
 * the user has configured. The text it is handed includes whatever the user
 * pasted into the prompt (a web page, a log, a forum post), and a pasted
 * instruction is exactly the thing that tells an agent with a Bash tool to use
 * it. Which of those calls are then allowed is the user's permission config,
 * not Liu's; a helper that has no tools cannot be talked into using them.
 *
 * `--tools ""` turns the built-in tools off, and `--strict-mcp-config` (with no
 * --mcp-config) drops every MCP server — which also saves the seconds Claude
 * spends starting them for a one-line translation. Two traps:
 *
 *   - `--tools` is VARIADIC. `claude -p --tools "" "<prompt>"` swallows the
 *     prompt as a tool name and fails with "Input must be provided"; Create
 *     Theme's argv (`-p`, then the prompt) would break that way. The
 *     prompt therefore follows a `--` (which also means a prompt that starts
 *     with '-' is never read as a flag), and another flag sits between.
 *   - Only flags the CLI is known to accept are added; an unknown option makes
 *     the CLI exit before it reads the prompt. Agents not listed here run with
 *     their own headless defaults (OpenCode, Grok, Cursor, Amp, Cline,
 *     Antigravity, Command Code, OpenClaw, Hermes).
 *
 * Pi and Oh My Pi (`pi -p`, `omp -p`; every flag below is in their --help):
 * no tools, no session on disk, and NO EXTENSIONS — Liu's own notify extension
 * lives in the user's extensions folder and would otherwise report a
 * translation as a finished turn. Pi adds no skills, prompt templates or
 * AGENTS.md; omp adds no skills, rules, LSP or title generation (a title is a
 * model call of its own). omp has no switch that drops the user's MCP servers
 * for one run: they still start, and its print mode waits for them, which is
 * why a translation through omp can take seconds longer than one through pi.
 *
 * Codex runs `exec` in a read-only sandbox, stated explicitly so a
 * `sandbox_mode = "danger-full-access"` in the user's config.toml does not
 * reach a translation.
 *
 * LIU_AGENT_HARDEN=0 turns all of it off, for a CLI version that rejects a
 * flag. Pure, so it can be tested without an agent.
 */
#ifndef CORE_AGENT_HEADLESS_H
#define CORE_AGENT_HEADLESS_H

#include "core/types.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline bool agent_headless_enabled(void) {
    const char *e = getenv("LIU_AGENT_HARDEN");
    return !(e && e[0] == '0');
}

/* The most flags agent_headless_flags() ever writes: callers size their array by it. */
#define AGENT_HEADLESS_MAX_FLAGS 8

/* Flags to put AFTER the agent's own prelude (`-p`, `exec ...`) and BEFORE the
 * prompt. Returns how many were written into `out` (0 = none, or the switch is
 * off, or `cap` is too small to hold them all — never a partial pair). */
static inline i32 agent_headless_flags(const char *agent_id, const char **out, i32 cap) {
    if (!agent_id || !out || cap <= 0 || !agent_headless_enabled()) return 0;
    if (strcmp(agent_id, "claude") == 0) {
        if (cap < 3) return 0;
        out[0] = "--tools"; out[1] = ""; out[2] = "--strict-mcp-config";
        return 3;
    }
    if (strcmp(agent_id, "codex") == 0) {
        if (cap < 2) return 0;
        out[0] = "-s"; out[1] = "read-only";
        return 2;
    }
    if (strcmp(agent_id, "pi") == 0) {
        static const char *const f[] = { "--no-session", "--no-tools", "--no-extensions", "--no-skills",
                                         "--no-prompt-templates", "--no-context-files" };
        enum { N = sizeof f / sizeof f[0] };
        if (cap < N) return 0;
        for (int i = 0; i < N; i++) out[i] = f[i];
        return N;
    }
    if (strcmp(agent_id, "omp") == 0) {
        static const char *const f[] = { "--no-session", "--no-title", "--no-tools", "--no-extensions",
                                         "--no-skills", "--no-rules", "--no-lsp" };
        enum { N = sizeof f / sizeof f[0] };
        if (cap < N) return 0;
        for (int i = 0; i < N; i++) out[i] = f[i];
        return N;
    }
    return 0;
}

/* True when the prompt must be preceded by a `--` (see the variadic trap
 * above). Only meaningful together with a non-empty agent_headless_flags(). */
static inline bool agent_headless_prompt_after_dashdash(const char *agent_id) {
    return agent_id && agent_headless_enabled() &&
           (strcmp(agent_id, "claude") == 0 || strcmp(agent_id, "pi") == 0 || strcmp(agent_id, "omp") == 0);
}

#endif /* CORE_AGENT_HEADLESS_H */
