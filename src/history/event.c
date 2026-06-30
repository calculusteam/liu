#include "history/event.h"

const char *chat_tool_name(ChatTool t) {
    switch (t) {
        case CHAT_TOOL_CLAUDE:      return "claude";
        case CHAT_TOOL_CODEX:       return "codex";
        case CHAT_TOOL_COPILOT:     return "copilot";
        case CHAT_TOOL_CURSOR:      return "cursor";
        case CHAT_TOOL_AMP:         return "amp";
        case CHAT_TOOL_CLINE:       return "cline";
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
        case CHAT_TOOL_ZED:         return "zed";
        case CHAT_TOOL_COMMANDCODE: return "commandcode";
        case CHAT_TOOL_XAI:         return "xai";
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
