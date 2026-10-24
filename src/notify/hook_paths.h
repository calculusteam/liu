/*
 * Liu — where every notify-hook artifact lives, relative to $HOME.
 *
 * `liu-notify doctor` has to look in the same places as the installers from
 * the OTHER binary — and liu-notify links neither agent_hooks.c nor
 * claude_hooks.c, so it cannot ask them. With two independent copies of a
 * path, a change to one would leave the tool that reports "no stale hooks"
 * looking somewhere the installer no longer writes.
 *
 * Macros rather than functions precisely so this header carries no link
 * dependency — anything that needs a path joins it to its own $HOME.
 *
 * No leading slash: every consumer joins as "<home>/" + REL, except
 * claude_hooks.c's schema table which wants the slash and concatenates it.
 */
#ifndef NOTIFY_HOOK_PATHS_H
#define NOTIFY_HOOK_PATHS_H

/* The launcher every agent's hook command names. See agent_hooks.h for why
 * the config must not name the real binary. */
#define LIU_HOOK_SHIM_REL         ".liu/hooks/liu-notify"

/* Per-agent integration surfaces. Claude, Command Code, Codex, and
 * Antigravity configuration files can contain entries managed by other
 * tools, so Liu updates only its own hooks. Liu owns the Grok and OpenCode
 * hook files. */
#define LIU_HOOK_CLAUDE_REL       ".claude/settings.json"
#define LIU_HOOK_COMMANDCODE_REL  ".commandcode/settings.json"
#define LIU_HOOK_GROK_REL         ".grok/hooks/liu-notify.json"
#define LIU_HOOK_OPENCODE_REL     ".config/opencode/plugins/liu-notify.js"
#define LIU_HOOK_CODEX_REL        ".codex/config.toml"
#define LIU_HOOK_ANTIGRAVITY_REL  ".gemini/config/hooks.json"
/* Pi and Oh My Pi load every .ts in their extensions folder: files Liu owns. */
#define LIU_HOOK_PI_REL           ".pi/agent/extensions/liu-notify.ts"
#define LIU_HOOK_OMP_REL          ".omp/agent/extensions/liu-notify.ts"

#endif /* NOTIFY_HOOK_PATHS_H */
