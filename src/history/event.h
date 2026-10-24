/*
 * liu-history - unified event model for AI-agent chat history.
 *
 * Every agent (Claude Code, Codex, Copilot, …) stores sessions in its own
 * per-tool format. `ChatEvent` is the normalised record after parsing.
 */
#ifndef HISTORY_EVENT_H
#define HISTORY_EVENT_H

#include "core/types.h"

typedef enum {
    CHAT_TOOL_UNKNOWN = 0,
    CHAT_TOOL_CLAUDE,
    CHAT_TOOL_CODEX,
    CHAT_TOOL_COPILOT,
    /* Extra agents recognized for launch/resume only (no transcript parsers). */
    CHAT_TOOL_CURSOR,
    CHAT_TOOL_AMP,
    CHAT_TOOL_CLINE,
    CHAT_TOOL_ROO,
    CHAT_TOOL_KILO,
    CHAT_TOOL_KIRO,
    CHAT_TOOL_CRUSH,
    CHAT_TOOL_OPENCODE,
    CHAT_TOOL_DROID,
    CHAT_TOOL_ANTIGRAVITY,
    CHAT_TOOL_KIMI,
    CHAT_TOOL_QWEN,
    CHAT_TOOL_AIDER,
    CHAT_TOOL_AMAZON_Q,
    CHAT_TOOL_CONTINUE,
    CHAT_TOOL_WINDSURF,
    CHAT_TOOL_COMMANDCODE,
    CHAT_TOOL_XAI,
    /* The standalone `cline` CLI, which is a DIFFERENT product from the
     * VS Code extension above: its own store (~/.cline/data/sessions) and
     * its own resume flag (`cline --id`). One enum value cannot serve both,
     * because resuming an extension task with the CLI flag would silently
     * start a new session. */
    CHAT_TOOL_CLINE_CLI,
    /* Four terminal harnesses, each with a session store of its own: Pi
     * (pi-coding-agent) and its fork Oh My Pi (`omp`), OpenClaw and Hermes
     * Agent. The scan mask is `1u << tool`, so this enum has to stay under 32. */
    CHAT_TOOL_PI,
    CHAT_TOOL_OMP,
    CHAT_TOOL_OPENCLAW,
    CHAT_TOOL_HERMES,
    CHAT_TOOL_COUNT_,
} ChatTool;

typedef enum {
    CHAT_ROLE_SYSTEM = 0,    /* snapshots, permission events, metadata */
    CHAT_ROLE_USER,
    CHAT_ROLE_ASSISTANT,
    CHAT_ROLE_TOOL_USE,
    CHAT_ROLE_TOOL_RESULT,
} ChatRole;

typedef struct {
    ChatTool    origin_tool;   /* which agent produced this */
    ChatRole    role;
    i64         timestamp_ms;  /* 0 if unknown */
    const char *text;          /* arena-allocated, may be NULL */
    const char *tool_name;     /* "Bash"/"Edit"/…; NULL unless role=TOOL_USE|TOOL_RESULT */
    const char *session_id;    /* arena-allocated, same lifetime as parse arena */
} ChatEvent;

const char *chat_tool_name(ChatTool t);    /* "claude" / "codex" / "copilot" / "unknown" */

/* Does the tool keep its sessions as rows of one SQLite file rather than a
 * file each? Then a session has no byte size worth measuring (summing message
 * lengths reads the largest value whole) and its message count stands in for
 * it: scanner, palette and `liu-history` all branch on this one answer. */
static inline bool chat_tool_is_db_store(ChatTool t) {
    return t == CHAT_TOOL_OPENCODE || t == CHAT_TOOL_HERMES || t == CHAT_TOOL_OPENCLAW;
}

/* Inverse of chat_tool_name(), for agent ids as they appear on the notify wire
 * and in agent_detect's table. Handles the one id that doesn't round-trip:
 * Grok ships its CLI as `grok` while the transcript store — and so this enum —
 * calls it xai. Returns CHAT_TOOL_UNKNOWN for anything unrecognised. */
ChatTool chat_tool_from_agent_id(const char *id);
const char *chat_role_name(ChatRole r);    /* "user" / "assistant" / ... */
const char *chat_role_icon(ChatRole r);    /* UTF-8 emoji prefix for text output */

#endif
