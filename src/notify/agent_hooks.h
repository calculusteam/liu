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
 *   grok      ~/.grok/hooks/liu-notify.json   (Liu's own file; always trusted)
 *   codex     ~/.codex/config.toml   one [[hooks.Stop]] entry, recognised by
 *             the command it runs — the comment fence around it is only
 *             decoration (needs a one-time `/hooks` trust inside Codex)
 *   opencode  ~/.config/opencode/plugins/liu-notify.js  (auto-loaded plugin;
 *             fires on session.idle, which is the top-level session only)
 *   pi, omp   ~/.pi/agent/extensions/liu-notify.ts and
 *             ~/.omp/agent/extensions/liu-notify.ts  (extensions their agent
 *             folders auto-load; Pi fires on agent_settled, omp on the main
 *             session's session_stop)
 *   devin     ~/.config/devin/config.json  hooks (claude_hooks.c's format)
 *   qoder     ~/.qoder/settings.json  hooks (claude_hooks.c's format)
 *   antigravity  ~/.gemini/config/hooks.json  "liu-notify" named hook on the
 *             `Stop` event (execution-loop terminated); a shared JSON Liu
 *             merges into. No --hook: the payload has no event-name field, so
 *             the Stop-only registration is itself the subagent guard.
 *
 * All installers are idempotent and reversible, and (like the Claude path)
 * are meant to be lifecycle-managed: installed when Liu launches, removed
 * when it quits, so Liu leaves no permanent footprint in other tools.
 *
 * Four of those files are the USER's as much as Liu's, and three rules follow
 * from that for every installer here and in claude_hooks.c:
 *
 *   - a hook is Liu's by what it RUNS — hook_cmd_is_liu() in hook_scan.h, the
 *     one rule install, uninstall, "is it installed" and Diagnostics share.
 *     Only such a hook is ever removed, and every such hook is: the user's
 *     hook in the same block, table or file stays, a copy of Liu's that an
 *     agent's own rewrite left somewhere else does not;
 *   - a file that cannot be rewritten without changing something that is not
 *     Liu's is REFUSED, byte for byte untouched, with the reason in the result
 *     (JSON that does not parse or has text after its value, an integer cJSON
 *     would re-spell, a Codex config whose hooks are a closed inline value);
 *   - a config that is a symbolic link is written THROUGH — see
 *     agent_hook_write_path().
 */
#ifndef NOTIFY_AGENT_HOOKS_H
#define NOTIFY_AGENT_HOOKS_H

#include "core/types.h"
#include "notify/claude_hooks.h"   /* reuse ClaudeHookResult */

typedef ClaudeHookResult AgentHookResult;

/* The agent ids this module can wire up (matches core/agent_detect ids). */
#define AGENT_HOOK_SUPPORTED { "claude", "grok", "codex", "opencode", "antigravity", "pi", "omp", "devin", "qoder" }

/* True if `agent_id` is one this module knows how to install. */
bool agent_hook_supported(const char *agent_id);

/* ---------------------------------------------------------------------------
 * Hook shim — the stable path every agent's config points at.
 *
 * Agents store the hook command as an absolute path. Pointing that at the real
 * liu-notify binary means the recorded command names a file that CAN VANISH:
 * the .app gets moved or deleted, a dev build tree is cleaned, the volume it
 * lives on isn't mounted. A hook left behind by an abnormal exit then runs a
 * command that no longer exists, the shell answers 127 on stderr, and the
 * agent reports a hook failure — in every terminal, not just Liu's.
 *
 * So the config gets `~/.liu/hooks/liu-notify` instead: a two-line script Liu
 * owns, which execs the real binary and exits 0 in silence when it is gone.
 * A leftover hook becomes a no-op rather than an error. It also makes the
 * recorded command CONSTANT across rebuilds and updates, which is what stops
 * Codex re-prompting "1 hook is new or changed" after every launch.
 * ------------------------------------------------------------------------- */

/* `~/.liu/hooks/liu-notify`. False only if $HOME is unusable. */
bool agent_hook_shim_path(char *out, usize cap);
/* '…'-quote a path for a command an agent hands to /bin/sh. */
bool agent_hook_shell_quote(const char *in, char *out, usize cap);

/* Write/refresh the shim so it targets `notify_bin`. Rewrites only when the
 * contents would change, so the file's mtime stays stable across launches.
 * Callers pass the shim path (not `notify_bin`) to agent_hook_install.
 * `notify_bin` without a slash is a NAME: the shim looks it up through $PATH
 * each time it runs. */
AgentHookResult agent_hook_shim_install(const char *notify_bin);

/* Where a write to the config at `path` has to land.
 *
 * Every installer replaces a file by renaming a temp file over it, and
 * renaming over a symbolic link replaces the LINK: a settings.json linked
 * into a dotfiles repository would become a regular file, and the repository
 * copy and the one the agent reads would drift apart. So when `path` is a
 * link to an existing file, `out` is that file — fully resolved, so the temp
 * file and the rename happen in ITS directory and the link is never touched —
 * and `*is_link` (optional) is true. Otherwise `out` is `path`; that includes
 * a dangling link, which has nothing behind it to preserve. False only when a
 * link cannot be followed or `out` is too small, and the caller must then
 * write nothing. */
bool agent_hook_write_path(const char *path, char *out, usize cap, bool *is_link);

/* Install / uninstall / query the notify hook for one agent. `notify_bin`
 * is the absolute path to the bundled liu-notify. Unknown ids return a
 * failed result (install/uninstall) or false (installed). */
AgentHookResult agent_hook_install(const char *agent_id, const char *notify_bin);
AgentHookResult agent_hook_uninstall(const char *agent_id);
bool            agent_hook_installed(const char *agent_id);

/* Install the shim before recording a hook in the agent configuration. If the
 * shim cannot be written, record notify_bin directly. This keeps the hook
 * pointed at the bundled binary when the app moves. */
AgentHookResult agent_hook_install_shimmed(const char *agent_id, const char *notify_bin);

#endif /* NOTIFY_AGENT_HOOKS_H */
