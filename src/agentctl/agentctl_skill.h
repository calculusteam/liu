/*
 * Liu — the skill file that tells Claude Code and Codex about `liu-agent`.
 *
 * Both agents load SKILL.md files from their skills folder and read only a
 * skill's one-line description until it applies, so the file costs them
 * nothing until the user asks for another agent's help. While agent control
 * is on, Liu writes `<agent home>/skills/liu-agents/SKILL.md` for each of the
 * two whose folder exists, and removes it when it quits or the setting goes
 * off: outside Liu the command it describes does not work.
 *
 * The file is Liu's by a marker line inside it. A file at that path without
 * the marker is the user's: it is never replaced and never removed.
 */
#ifndef AGENTCTL_SKILL_H
#define AGENTCTL_SKILL_H

#include "core/types.h"

#include <stdbool.h>

/* The skill's text, as it is written. */
const char *agentctl_skill_text(void);

/* `agent` is "claude" or "codex". Install: true when Liu's file is in place
 * afterwards. Uninstall: removes only Liu's own file, and the folder when it
 * is left empty. `home` may be NULL ($HOME). */
bool agentctl_skill_install(const char *home, const char *agent);
void agentctl_skill_uninstall(const char *home, const char *agent);

#endif /* AGENTCTL_SKILL_H */
