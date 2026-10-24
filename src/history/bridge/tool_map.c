/*
 * Map supported agent tool names and argument fields to Liu's normalized
 * history representation. Keep aliases in the tables below so parsing and
 * target serialization use the same names.
 */
#include "history/bridge/tool_map.h"

#include "cJSON.h"

#include <string.h>
#include <strings.h>

typedef struct {
    const char    *name;
    BridgeToolKind kind;
} ToolAlias;

/* Names shared across the Claude-shaped family (Claude Code, Cursor Agent,
 * Cline CLI and anything else that speaks Anthropic tool blocks). */
static const ToolAlias kClaudeLike[] = {
    { "Read",          BRIDGE_TOOL_FILE_READ   },
    { "NotebookRead",  BRIDGE_TOOL_FILE_READ   },
    { "Write",         BRIDGE_TOOL_FILE_WRITE  },
    { "NotebookEdit",  BRIDGE_TOOL_FILE_PATCH  },
    { "Edit",          BRIDGE_TOOL_FILE_PATCH  },
    { "MultiEdit",     BRIDGE_TOOL_FILE_PATCH  },
    { "StrReplace",    BRIDGE_TOOL_FILE_PATCH  },   /* cursor */
    { "Bash",          BRIDGE_TOOL_SHELL       },
    { "Shell",         BRIDGE_TOOL_SHELL       },   /* cursor */
    { "BashOutput",    BRIDGE_TOOL_SHELL       },
    { "KillShell",     BRIDGE_TOOL_SHELL       },
    { "Grep",          BRIDGE_TOOL_TEXT_SEARCH },
    { "Glob",          BRIDGE_TOOL_FILE_SEARCH },
    { "LS",            BRIDGE_TOOL_FILE_SEARCH },
    { "WebSearch",     BRIDGE_TOOL_WEB_SEARCH  },
    { "WebFetch",      BRIDGE_TOOL_WEB_FETCH   },
    { "TodoWrite",     BRIDGE_TOOL_TASK        },
    { "Task",          BRIDGE_TOOL_SUBAGENT    },
    { "Agent",         BRIDGE_TOOL_SUBAGENT    },
    { "CallMcpTool",   BRIDGE_TOOL_MCP         },   /* cursor */
};

static const ToolAlias kCodex[] = {
    { "shell",           BRIDGE_TOOL_SHELL      },
    { "shell_command",   BRIDGE_TOOL_SHELL      },
    { "exec_command",    BRIDGE_TOOL_SHELL      },
    { "write_stdin",     BRIDGE_TOOL_SHELL      },
    { "apply_patch",     BRIDGE_TOOL_FILE_PATCH },
    { "update_plan",     BRIDGE_TOOL_TASK       },
    { "web_search",      BRIDGE_TOOL_WEB_SEARCH },
    { "view_image",      BRIDGE_TOOL_FILE_READ  },
};

static const ToolAlias kOpenCode[] = {
    { "read",        BRIDGE_TOOL_FILE_READ   },
    { "write",       BRIDGE_TOOL_FILE_WRITE  },
    { "edit",        BRIDGE_TOOL_FILE_PATCH  },
    { "patch",       BRIDGE_TOOL_FILE_PATCH  },
    { "apply_patch", BRIDGE_TOOL_FILE_PATCH  },
    { "bash",        BRIDGE_TOOL_SHELL       },
    { "grep",        BRIDGE_TOOL_TEXT_SEARCH },
    { "glob",        BRIDGE_TOOL_FILE_SEARCH },
    { "list",        BRIDGE_TOOL_FILE_SEARCH },
    { "websearch",   BRIDGE_TOOL_WEB_SEARCH  },
    { "webfetch",    BRIDGE_TOOL_WEB_FETCH   },
    { "todowrite",   BRIDGE_TOOL_TASK        },
    { "todoread",    BRIDGE_TOOL_TASK        },
    { "task",        BRIDGE_TOOL_SUBAGENT    },
    { "skill",       BRIDGE_TOOL_SUBAGENT    },
    /* 2.x, as a 2.0.32 store spells them: read, shell, webfetch, execute (a
     * code tool: no kind), grep, patch, glob, websearch, edit, subagent,
     * skill, write, ls. Its shell is `shell` and its sub-agent `subagent`.
     */
    { "shell",       BRIDGE_TOOL_SHELL       },
    { "subagent",    BRIDGE_TOOL_SUBAGENT    },
    { "ls",          BRIDGE_TOOL_FILE_SEARCH },
};

/* snake_case family: grok, qwen (gemini-cli lineage), commandcode, cline,
 * amp, antigravity. They overlap heavily, so one table serves all of them
 * and each agent's own extras are appended. */
static const ToolAlias kSnake[] = {
    { "read_file",           BRIDGE_TOOL_FILE_READ   },
    { "view_file",           BRIDGE_TOOL_FILE_READ   },
    { "read_many_files",     BRIDGE_TOOL_FILE_READ   },
    { "write_file",          BRIDGE_TOOL_FILE_WRITE  },
    { "create_file",         BRIDGE_TOOL_FILE_WRITE  },
    { "write_to_file",       BRIDGE_TOOL_FILE_WRITE  },
    { "edit_file",           BRIDGE_TOOL_FILE_PATCH  },
    { "replace",             BRIDGE_TOOL_FILE_PATCH  },
    { "replace_in_file",     BRIDGE_TOOL_FILE_PATCH  },
    { "str_replace",         BRIDGE_TOOL_FILE_PATCH  },
    { "str_replace_editor",  BRIDGE_TOOL_FILE_PATCH  },
    { "apply_patch",         BRIDGE_TOOL_FILE_PATCH  },
    { "bash",                BRIDGE_TOOL_SHELL       },
    { "shell",               BRIDGE_TOOL_SHELL       },
    { "shell_command",       BRIDGE_TOOL_SHELL       },
    { "run_command",         BRIDGE_TOOL_SHELL       },
    { "run_terminal_cmd",    BRIDGE_TOOL_SHELL       },
    { "run_shell_command",   BRIDGE_TOOL_SHELL       },
    { "execute_command",     BRIDGE_TOOL_SHELL       },
    { "execute_bash",        BRIDGE_TOOL_SHELL       },
    { "terminal",            BRIDGE_TOOL_SHELL       },
    { "kill_shell",          BRIDGE_TOOL_SHELL       },
    { "grep",                BRIDGE_TOOL_TEXT_SEARCH },
    { "search_files",        BRIDGE_TOOL_TEXT_SEARCH },
    { "search_file_content", BRIDGE_TOOL_TEXT_SEARCH },
    { "codebase_search",     BRIDGE_TOOL_TEXT_SEARCH },
    { "glob",                BRIDGE_TOOL_FILE_SEARCH },
    { "find_files",          BRIDGE_TOOL_FILE_SEARCH },
    { "list_files",          BRIDGE_TOOL_FILE_SEARCH },
    { "list_directory",      BRIDGE_TOOL_FILE_SEARCH },
    { "read_directory",      BRIDGE_TOOL_FILE_SEARCH },
    { "explore",             BRIDGE_TOOL_FILE_SEARCH },
    { "web_search",          BRIDGE_TOOL_WEB_SEARCH  },
    { "google_web_search",   BRIDGE_TOOL_WEB_SEARCH  },
    { "web_fetch",           BRIDGE_TOOL_WEB_FETCH   },
    { "browse_page",         BRIDGE_TOOL_WEB_FETCH   },
    { "browser_action",      BRIDGE_TOOL_WEB_FETCH   },
    { "todo_write",          BRIDGE_TOOL_TASK        },
    { "update_plan",         BRIDGE_TOOL_TASK        },
    { "save_memory",         BRIDGE_TOOL_TASK        },
    { "task",                BRIDGE_TOOL_SUBAGENT    },
    { "agent",               BRIDGE_TOOL_SUBAGENT    },
    { "subagent",            BRIDGE_TOOL_SUBAGENT    },
    { "use_mcp_tool",        BRIDGE_TOOL_MCP         },
};

/* Kimi CLI uses PascalCase names of its own. */
static const ToolAlias kKimi[] = {
    { "ReadFile",   BRIDGE_TOOL_FILE_READ   },
    { "WriteFile",  BRIDGE_TOOL_FILE_WRITE  },
    { "EditFile",   BRIDGE_TOOL_FILE_PATCH  },
    { "Shell",      BRIDGE_TOOL_SHELL       },
    { "Grep",       BRIDGE_TOOL_TEXT_SEARCH },
    { "Glob",       BRIDGE_TOOL_FILE_SEARCH },
    { "LS",         BRIDGE_TOOL_FILE_SEARCH },
    { "WebSearch",  BRIDGE_TOOL_WEB_SEARCH  },
    { "WebFetch",   BRIDGE_TOOL_WEB_FETCH   },
    { "Agent",      BRIDGE_TOOL_SUBAGENT    },
    { "TodoWrite",  BRIDGE_TOOL_TASK        },
};

static BridgeToolKind lookup(const ToolAlias *tab, usize n, const char *name) {
    for (usize i = 0; i < n; i++)
        if (strcasecmp(tab[i].name, name) == 0) return tab[i].kind;
    return BRIDGE_TOOL_UNKNOWN;
}

#define LOOKUP(tab, name) lookup((tab), sizeof(tab)/sizeof((tab)[0]), (name))

BridgeToolKind bridge_tool_kind_for(ChatTool tool, const char *name) {
    if (!name || !name[0]) return BRIDGE_TOOL_UNKNOWN;

    /* An MCP call is an MCP call in every agent that speaks the protocol, and
     * the server/tool halves are user-installed so no table can enumerate
     * them. Checked first because a name such as `mcp__<server>__<tool>`
     * would otherwise fall through to UNKNOWN and lose the one thing known
     * about it. */
    if (strncmp(name, "mcp__", 5) == 0) return BRIDGE_TOOL_MCP;

    BridgeToolKind k = BRIDGE_TOOL_UNKNOWN;
    switch (tool) {
        case CHAT_TOOL_CODEX:
            k = LOOKUP(kCodex, name);
            break;
        case CHAT_TOOL_OPENCODE:
            k = LOOKUP(kOpenCode, name);
            break;
        case CHAT_TOOL_KIMI:
            k = LOOKUP(kKimi, name);
            break;
        case CHAT_TOOL_CLAUDE:
        case CHAT_TOOL_CURSOR:
        case CHAT_TOOL_CLINE_CLI:
        case CHAT_TOOL_COPILOT:
            k = LOOKUP(kClaudeLike, name);
            break;
        default:
            break;
    }
    if (k != BRIDGE_TOOL_UNKNOWN) return k;

    /* Second pass over the two big vocabularies regardless of source tool.
     * Forks routinely inherit their upstream's names (CommandCode is a Claude
     * Code fork that renamed everything to snake_case; Grok answers to both
     * `bash` and `execute_bash`), and a name that is unambiguous in one table
     * is unambiguous everywhere — none of the tables disagree about a name. */
    k = LOOKUP(kSnake, name);
    if (k != BRIDGE_TOOL_UNKNOWN) return k;
    return LOOKUP(kClaudeLike, name);
}

const char *bridge_tool_name_for(ChatTool tool, BridgeToolKind kind) {
    if (kind == BRIDGE_TOOL_UNKNOWN || kind >= BRIDGE_TOOL_KIND_COUNT_)
        return NULL;

    switch (tool) {
        case CHAT_TOOL_CLAUDE:
        case CHAT_TOOL_CURSOR:
        case CHAT_TOOL_CLINE_CLI:
        case CHAT_TOOL_COPILOT:
            switch (kind) {
                case BRIDGE_TOOL_FILE_READ:   return "Read";
                case BRIDGE_TOOL_FILE_WRITE:  return "Write";
                case BRIDGE_TOOL_FILE_PATCH:  return "Edit";
                case BRIDGE_TOOL_SHELL:       return "Bash";
                case BRIDGE_TOOL_TEXT_SEARCH: return "Grep";
                case BRIDGE_TOOL_FILE_SEARCH: return "Glob";
                case BRIDGE_TOOL_WEB_SEARCH:  return "WebSearch";
                case BRIDGE_TOOL_WEB_FETCH:   return "WebFetch";
                case BRIDGE_TOOL_TASK:        return "TodoWrite";
                case BRIDGE_TOOL_SUBAGENT:    return "Task";
                default:                      return NULL;   /* MCP: keep the original */
            }
        case CHAT_TOOL_CODEX:
            switch (kind) {
                case BRIDGE_TOOL_SHELL:       return "shell";
                case BRIDGE_TOOL_FILE_PATCH:  return "apply_patch";
                case BRIDGE_TOOL_TASK:        return "update_plan";
                case BRIDGE_TOOL_WEB_SEARCH:  return "web_search";
                /* Codex has no read / write / grep / glob tool — it does all
                 * of those through `shell`, which means there is no name here
                 * whose ARGUMENTS would match. Returning "shell" would produce
                 * a visibly wrong record:
                 * `{"name":"shell","arguments":{"file_path":"…"}}`, a shell
                 * call that takes a command and was handed a path. NULL
                 * instead, so the adapter keeps the source's own name — `Read`
                 * with a file_path is at least internally consistent, and
                 * these are records of calls that already ran, not tools the
                 * model is being offered. */
                default:                      return NULL;
            }
        case CHAT_TOOL_OPENCODE:
            switch (kind) {
                case BRIDGE_TOOL_FILE_READ:   return "read";
                case BRIDGE_TOOL_FILE_WRITE:  return "write";
                case BRIDGE_TOOL_FILE_PATCH:  return "edit";
                case BRIDGE_TOOL_SHELL:       return "bash";
                case BRIDGE_TOOL_TEXT_SEARCH: return "grep";
                case BRIDGE_TOOL_FILE_SEARCH: return "glob";
                case BRIDGE_TOOL_WEB_SEARCH:  return "websearch";
                case BRIDGE_TOOL_WEB_FETCH:   return "webfetch";
                case BRIDGE_TOOL_TASK:        return "todowrite";
                case BRIDGE_TOOL_SUBAGENT:    return "task";
                default:                      return NULL;
            }
        case CHAT_TOOL_KIMI:
            switch (kind) {
                case BRIDGE_TOOL_FILE_READ:   return "ReadFile";
                case BRIDGE_TOOL_FILE_WRITE:  return "WriteFile";
                case BRIDGE_TOOL_FILE_PATCH:  return "EditFile";
                case BRIDGE_TOOL_SHELL:       return "Shell";
                case BRIDGE_TOOL_TEXT_SEARCH: return "Grep";
                case BRIDGE_TOOL_FILE_SEARCH: return "Glob";
                case BRIDGE_TOOL_WEB_SEARCH:  return "WebSearch";
                case BRIDGE_TOOL_WEB_FETCH:   return "WebFetch";
                case BRIDGE_TOOL_SUBAGENT:    return "Agent";
                case BRIDGE_TOOL_TASK:        return "TodoWrite";
                default:                      return NULL;
            }
        default:
            /* snake_case family: grok, qwen, commandcode, cline, amp,
             * antigravity, droid, kiro. */
            switch (kind) {
                case BRIDGE_TOOL_FILE_READ:   return "read_file";
                case BRIDGE_TOOL_FILE_WRITE:  return "write_file";
                case BRIDGE_TOOL_FILE_PATCH:  return "edit_file";
                case BRIDGE_TOOL_SHELL:       return "shell_command";
                case BRIDGE_TOOL_TEXT_SEARCH: return "search_files";
                case BRIDGE_TOOL_FILE_SEARCH: return "find_files";
                case BRIDGE_TOOL_WEB_SEARCH:  return "web_search";
                case BRIDGE_TOOL_WEB_FETCH:   return "web_fetch";
                case BRIDGE_TOOL_TASK:        return "update_plan";
                case BRIDGE_TOOL_SUBAGENT:    return "task";
                default:                      return NULL;
            }
    }
}

const char *bridge_tool_kind_id(BridgeToolKind kind) {
    switch (kind) {
        case BRIDGE_TOOL_FILE_READ:   return "file_read";
        case BRIDGE_TOOL_FILE_WRITE:  return "file_write";
        case BRIDGE_TOOL_FILE_PATCH:  return "file_patch";
        case BRIDGE_TOOL_SHELL:       return "shell";
        case BRIDGE_TOOL_TEXT_SEARCH: return "text_search";
        case BRIDGE_TOOL_FILE_SEARCH: return "file_search";
        case BRIDGE_TOOL_WEB_SEARCH:  return "web_search";
        case BRIDGE_TOOL_WEB_FETCH:   return "web_fetch";
        case BRIDGE_TOOL_TASK:        return "task";
        case BRIDGE_TOOL_SUBAGENT:    return "subagent";
        case BRIDGE_TOOL_MCP:         return "mcp";
        default:                      return "unknown";
    }
}

const char *bridge_tool_kind_verb(BridgeToolKind kind) {
    switch (kind) {
        case BRIDGE_TOOL_FILE_READ:   return "read";
        case BRIDGE_TOOL_FILE_WRITE:  return "wrote";
        case BRIDGE_TOOL_FILE_PATCH:  return "edited";
        case BRIDGE_TOOL_SHELL:       return "ran a shell command";
        case BRIDGE_TOOL_TEXT_SEARCH: return "searched text in";
        case BRIDGE_TOOL_FILE_SEARCH: return "listed files in";
        case BRIDGE_TOOL_WEB_SEARCH:  return "searched the web for";
        case BRIDGE_TOOL_WEB_FETCH:   return "fetched";
        case BRIDGE_TOOL_TASK:        return "updated the plan";
        case BRIDGE_TOOL_SUBAGENT:    return "delegated to a subagent";
        case BRIDGE_TOOL_MCP:         return "called an MCP tool";
        default:                      return "called";
    }
}

bool bridge_tool_kind_is_mutating(BridgeToolKind kind) {
    switch (kind) {
        case BRIDGE_TOOL_FILE_WRITE:
        case BRIDGE_TOOL_FILE_PATCH:
        case BRIDGE_TOOL_SHELL:
            return true;
        /* MCP is deliberately NOT here. Its effect is whatever the server
         * does, which is unknowable from the name — and the only consumer of
         * this predicate is a summary, where over-reporting a read-only MCP
         * call as a mutation would be misleading in the direction that makes
         * a user distrust the summary. Nothing gates execution on it: no
         * adapter may execute ANY historical call. */
        default:
            return false;
    }
}

/* ------------------------------------------------------- a call's subject - */

/* Copy `v` into out[cap], NUL-terminated, never ending inside a code point. */
static void copy_clipped(char *out, usize cap, const char *v) {
    usize n = strlen(v);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)v[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, v, n);
    out[n] = '\0';
}

const char *bridge_tool_input_path(const cJSON *input, char *out, usize cap) {
    static const char *const keys[] = {
        "file_path", "filePath", "path", "file", "filename", "target_file",
        "absolute_path", "notebook_path", "old_path", "new_path", "uri",
    };
    if (!input || !out || cap == 0) return NULL;
    for (usize i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(input, keys[i]);
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
            copy_clipped(out, cap, v->valuestring);
            return keys[i];
        }
    }
    return NULL;
}

const char *bridge_tool_input_command(const cJSON *input, char *out, usize cap) {
    static const char *const keys[] = { "command", "cmd", "script", "shell" };
    if (!input || !out || cap == 0) return NULL;
    for (usize i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(input, keys[i]);
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
            copy_clipped(out, cap, v->valuestring);
            return keys[i];
        }
        /* Codex passes `command` as an ARRAY of argv, which is why a plain
         * string read returns nothing for a codex source. */
        if (cJSON_IsArray(v)) {
            usize off = 0;
            const cJSON *it = NULL;
            out[0] = '\0';
            cJSON_ArrayForEach(it, v) {
                if (!cJSON_IsString(it) || !it->valuestring) continue;
                if (off + 2 >= cap) break;      /* no room for " x" */
                if (off) out[off++] = ' ';
                copy_clipped(out + off, cap - off, it->valuestring);
                off += strlen(out + off);
            }
            if (off) return keys[i];
        }
    }
    return NULL;
}
