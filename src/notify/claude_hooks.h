/*
 * Claude Code <-> liu-notify wiring.
 *
 * Writes/removes hook entries in ~/.claude/settings.json that pipe Claude's
 * Notification and Stop events to `liu-notify send`. The hook entries are
 * identified by a "liu-notify" substring in their command, so user-managed
 * Claude hooks are preserved across install/uninstall cycles.
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
 * entries but never duplicates them. Returns false when settings.json
 * couldn't be created/written. */
ClaudeHookResult claude_hooks_install(const char *notify_bin_path);

/* Strip all Liu-installed hook entries. Other user-managed hooks are kept. */
ClaudeHookResult claude_hooks_uninstall(void);

/* True when at least one Liu-installed hook entry is present. */
bool claude_hooks_installed(void);

#endif
