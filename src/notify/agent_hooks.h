/*
 * Liu - multi-agent notify-hook installers.
 *
 * Wires each supported coding-agent CLI to `liu-notify send` so a finished
 * top-level turn fires a notification — and, crucially, a finished SUBAGENT
 * does not (the per-agent payload carries the event name; liu-notify's
 * `--hook` filter drops SubagentStop). Each agent has its own integration
 * surface:
 *
 *   claude    ~/.claude/settings.json  hooks (delegated to claude_hooks.c)
 *   grok      ~/.grok/hooks/liu-notify.json   (our own file; always trusted)
 *   codex     ~/.codex/config.toml   [[hooks.Stop]] block between sentinels
 *             (needs a one-time `/hooks` trust inside Codex)
 *   opencode  ~/.config/opencode/plugins/liu-notify.js  (auto-loaded plugin;
 *             fires on session.idle, which is the top-level session only)
 *
 * All installers are idempotent and reversible, and (like the Claude path)
 * are meant to be lifecycle-managed: installed when Liu launches, removed
 * when it quits, so Liu leaves no permanent footprint in other tools.
 */
#ifndef NOTIFY_AGENT_HOOKS_H
#define NOTIFY_AGENT_HOOKS_H

#include "core/types.h"
#include "notify/claude_hooks.h"   /* reuse ClaudeHookResult */

typedef ClaudeHookResult AgentHookResult;

/* The agent ids this module can wire up (matches core/agent_detect ids). */
#define AGENT_HOOK_SUPPORTED { "claude", "grok", "codex", "opencode" }

/* True if `agent_id` is one this module knows how to install. */
bool agent_hook_supported(const char *agent_id);

/* Install / uninstall / query the notify hook for one agent. `notify_bin`
 * is the absolute path to the bundled liu-notify. Unknown ids return a
 * failed result (install/uninstall) or false (installed). */
AgentHookResult agent_hook_install(const char *agent_id, const char *notify_bin);
AgentHookResult agent_hook_uninstall(const char *agent_id);
bool            agent_hook_installed(const char *agent_id);

#endif /* NOTIFY_AGENT_HOOKS_H */
