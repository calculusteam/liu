/*
 * Liu — which agent a process NAME is.
 *
 * A pane's foreground process is recognised by name (session_fg_process), and
 * three places then ask "which agent is that": the tab (icon, working bar,
 * hooks), the Translate chord and the file drop. All three read this one
 * table, so a spelling known to one is known to all (Command Code 1.56+ names
 * its process `command-code`, while its bins include `commandcode`).
 *
 * `id` is what the notify wire and chat_tool_name() call the agent — except
 * Grok, whose CLI is `grok` while the transcript store (and the ChatTool) is
 * `xai`; chat_tool_from_agent_id() takes both.
 *
 * Add a spelling here and nowhere else. A name is matched whole (the basename
 * of a script path, argv[0], the process name): never a prefix, never a
 * substring.
 */
#ifndef CORE_AGENT_NAMES_H
#define CORE_AGENT_NAMES_H

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *name;     /* a process name, as the kernel or argv[0] spells it */
    const char *id;       /* the agent it is */
} AgentName;

static const AgentName g_agent_names[] = {
    { "claude",       "claude"      },
    { "codex",        "codex"       },
    { "gpt",          "codex"       },
    { "chatgpt",      "codex"       },
    { "openai",       "codex"       },
    { "copilot",      "copilot"     },
    { "cursor-agent", "cursor"      },
    { "cursor",       "cursor"      },
    { "amp",          "amp"         },
    { "cline",        "cline"       },
    { "roo",          "roo"         },
    { "kilo",         "kilo"        },
    { "kiro",         "kiro"        },
    { "crush",        "crush"       },
    { "opencode",     "opencode"    },
    { "droid",        "droid"       },
    { "antigravity",  "antigravity" },
    { "agy",          "antigravity" },
    { "kimi",         "kimi"        },
    { "qwen",         "qwen"        },
    { "aider",        "aider"       },
    { "amazon-q",     "amazon-q"    },
    { "amazonq",      "amazon-q"    },
    { "continue",     "continue"    },
    { "windsurf",     "windsurf"    },
    /* The four bins the command-code package links (package.json "bin"); 1.56+
     * titles the process `command-code` whichever of them was typed. */
    { "commandcode",  "commandcode" },
    { "command-code", "commandcode" },
    { "cmd",          "commandcode" },
    { "cmdc",         "commandcode" },
    { "grok",         "grok"        },
    { "xai",          "grok"        },
    { "pi",           "pi"          },
    { "omp",          "omp"         },
    { "openclaw",     "openclaw"    },
    { "hermes",       "hermes"      },
};

static inline bool agent_names_equal(const char *a, const char *b, bool fold_case) {
    while (*a && *b) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (fold_case) { x = (unsigned char)tolower(x); y = (unsigned char)tolower(y); }
        if (x != y) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* The agent id of a process name, or "" when it is not an agent's. */
static inline const char *agent_names_id(const char *name, bool fold_case) {
    if (!name || !name[0]) return "";
    for (size_t i = 0; i < sizeof g_agent_names / sizeof g_agent_names[0]; i++)
        if (agent_names_equal(name, g_agent_names[i].name, fold_case)) return g_agent_names[i].id;
    return "";
}

#endif /* CORE_AGENT_NAMES_H */
