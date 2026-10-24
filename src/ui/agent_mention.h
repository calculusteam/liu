/*
 * Add Vault Source types a path to the running agent. Agents that support
 * @path mentions can open the referenced file; other agents receive a plain
 * path. The allow-list enables @path for Claude, OpenCode, CommandCode, Cline,
 * and Cline CLI. Other agents use the form without a sigil.
 */
#ifndef UI_AGENT_MENTION_H
#define UI_AGENT_MENTION_H

#include "core/types.h"
#include "history/event.h"

/* Does `tool` resolve an `@<path>` mention in its prompt? */
static inline bool agent_uses_at_mention(ChatTool tool) {
    switch (tool) {
    case CHAT_TOOL_CLAUDE:
    case CHAT_TOOL_OPENCODE:
    case CHAT_TOOL_COMMANDCODE:
    /* The VS Code extension and the standalone CLI are different products (see
     * the note in history/event.h) but share the mention syntax; the CLI is
     * the one that runs in a pane. */
    case CHAT_TOOL_CLINE:
    case CHAT_TOOL_CLINE_CLI:
        return true;
    default:
        return false;
    }
}

#endif /* UI_AGENT_MENTION_H */
