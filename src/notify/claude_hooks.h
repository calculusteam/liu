/*
 * Claude Code <-> liu-notify wiring.
 *
 * Writes/removes hook entries in ~/.claude/settings.json that pipe Claude's
 * Notification and Stop events to `liu-notify send`. A hook entry is Liu's
 * when the command it runs is a liu-notify (hook_cmd_is_liu in hook_scan.h —
 * the rule every installer and Diagnostics share), and only such an entry is
 * ever removed: a hook of the user's stays, also when it sits in the same
 * matcher block as Liu's or merely mentions liu-notify in an argument.
 *
 * The file is the user's. One that cannot be rewritten without changing
 * something that is not Liu's — it does not parse, it has text after its JSON
 * value, it holds an integer cJSON would re-spell — is left byte for byte as
 * it is and the result says why; an uninstall that finds no Liu hook does not
 * rewrite it at all; and a settings.json that is a symbolic link is written
 * through to the file it points at (agent_hook_write_path).
 */
#ifndef NOTIFY_CLAUDE_HOOKS_H
#define NOTIFY_CLAUDE_HOOKS_H

#include "core/types.h"

/* Result of an install/uninstall call. Diagnostic message lives in
 * `out_msg` (may be empty on success). */
typedef struct {
    bool ok;
    char msg[256];
} ClaudeHookResult;

/* Install hooks pointing at `notify_bin_path` (absolute path to a built
 * liu-notify binary). Idempotent — re-running replaces any prior Liu
 * entries but never duplicates them. `ok` is false when settings.json
 * couldn't be created/written, or was refused for one of the reasons above. */
ClaudeHookResult claude_hooks_install(const char *notify_bin_path);

/* Strip all Liu-installed hook entries. Other user-managed hooks are kept. */
ClaudeHookResult claude_hooks_uninstall(void);

/* True when at least one Liu-installed hook entry is present. */
bool claude_hooks_installed(void);

/* Command Code — a Claude Code fork with the same settings format, read from
 * ~/.commandcode/settings.json. It fires only PreToolUse/PostToolUse/Stop/
 * SessionStart, so Liu registers Stop alone: there is no Notification event
 * and no per-turn start (SessionStart is CLI boot, not a turn). The pane
 * therefore animates from output volume like Codex/Grok, and the hook is what
 * ends the turn and opens the done panel. */
ClaudeHookResult commandcode_hooks_install(const char *notify_bin_path);
ClaudeHookResult commandcode_hooks_uninstall(void);
bool commandcode_hooks_installed(void);

#endif
