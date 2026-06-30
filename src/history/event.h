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
    CHAT_TOOL_ZED,
    CHAT_TOOL_COMMANDCODE,
    CHAT_TOOL_XAI,
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
const char *chat_role_name(ChatRole r);    /* "user" / "assistant" / ... */
const char *chat_role_icon(ChatRole r);    /* UTF-8 emoji prefix for text output */

#endif
