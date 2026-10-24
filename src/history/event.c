#include "history/event.h"

#include <string.h>

const char *chat_tool_name(ChatTool t) {
    switch (t) {
        case CHAT_TOOL_CLAUDE:      return "claude";
        case CHAT_TOOL_CODEX:       return "codex";
        case CHAT_TOOL_COPILOT:     return "copilot";
        case CHAT_TOOL_CURSOR:      return "cursor";
        case CHAT_TOOL_AMP:         return "amp";
        case CHAT_TOOL_CLINE:       return "cline";
        case CHAT_TOOL_CLINE_CLI:   return "cline-cli";
        case CHAT_TOOL_ROO:         return "roo";
        case CHAT_TOOL_KILO:        return "kilo";
        case CHAT_TOOL_KIRO:        return "kiro";
        case CHAT_TOOL_CRUSH:       return "crush";
        case CHAT_TOOL_OPENCODE:    return "opencode";
        case CHAT_TOOL_DROID:       return "droid";
        case CHAT_TOOL_ANTIGRAVITY: return "antigravity";
        case CHAT_TOOL_KIMI:        return "kimi";
        case CHAT_TOOL_QWEN:        return "qwen";
        case CHAT_TOOL_AIDER:       return "aider";
        case CHAT_TOOL_AMAZON_Q:    return "amazon-q";
        case CHAT_TOOL_CONTINUE:    return "continue";
        case CHAT_TOOL_WINDSURF:    return "windsurf";
        case CHAT_TOOL_COMMANDCODE: return "commandcode";
        case CHAT_TOOL_XAI:         return "xai";
        case CHAT_TOOL_PI:          return "pi";
        case CHAT_TOOL_OMP:         return "omp";
        case CHAT_TOOL_OPENCLAW:    return "openclaw";
        case CHAT_TOOL_HERMES:      return "hermes";
        default:                    return "unknown";
    }
}

const char *chat_role_name(ChatRole r) {
    switch (r) {
        case CHAT_ROLE_USER:        return "user";
        case CHAT_ROLE_ASSISTANT:   return "assistant";
        case CHAT_ROLE_TOOL_USE:    return "tool_use";
        case CHAT_ROLE_TOOL_RESULT: return "tool_result";
        case CHAT_ROLE_SYSTEM:      return "system";
        default:                    return "?";
    }
}

/* UTF-8 emoji prefixes — kept to single-codepoint-per-role so column math in
 * the formatter is predictable. */
const char *chat_role_icon(ChatRole r) {
    switch (r) {
        case CHAT_ROLE_USER:        return "🙋";
        case CHAT_ROLE_ASSISTANT:   return "🤖";
        case CHAT_ROLE_TOOL_USE:    return "🛠";
        case CHAT_ROLE_TOOL_RESULT: return "📎";
        case CHAT_ROLE_SYSTEM:      return "⚙️";
        default:                    return "·";
    }
}

ChatTool chat_tool_from_agent_id(const char *id) {
    if (!id || !id[0]) return CHAT_TOOL_UNKNOWN;
    if (strcmp(id, "grok") == 0) return CHAT_TOOL_XAI;
    for (i32 t = CHAT_TOOL_UNKNOWN + 1; t < CHAT_TOOL_COUNT_; t++) {
        const char *name = chat_tool_name((ChatTool)t);
        if (name && strcmp(name, id) == 0) return (ChatTool)t;
    }
    return CHAT_TOOL_UNKNOWN;
}
