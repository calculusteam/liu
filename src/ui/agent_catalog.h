/*
 * Liu — the agent CLIs Liu can start, resume and hand a conversation to.
 *
 * One table, read by the Agent History picker (resume, start, continue in
 * another agent) and by session restore (an agent that was running in a pane
 * when Liu quit is started again on the session it had). A row's resume
 * format is the agent's own flag for opening a stored session; the argument
 * it takes is a session id, or the session file's path for a row that says
 * so.
 *
 * Every argument that reaches one of these command lines comes from an
 * agent's on-disk store or a hook payload, and passes agent_arg_is_safe()
 * first.
 */
#ifndef UI_AGENT_CATALOG_H
#define UI_AGENT_CATALOG_H

#include "core/types.h"
#include "history/event.h"
#include "ui/agent_resume.h"   /* codex_thread_uuid */

#include <stdio.h>
#include <string.h>

/* Agent session ids / paths are interpolated into shell command lines that are
 * then pasted and executed. They originate from on-disk agent-history files, so
 * a crafted id with shell metacharacters would be a command-injection vector
 * (bracketed paste does NOT shell-quote). Allow only the characters that
 * legitimately appear in session ids and resume paths (UUIDs, hashes,
 * timestamps, filesystem paths). */
static inline bool agent_arg_is_safe(const char *s) {
    if (!s || !s[0]) return false;
    /* A leading '-' would make a DB-derived id an OPTION to the agent CLI it
     * is pasted after (option injection); no session id or path starts so. */
    if (s[0] == '-') return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == '/' ||
                  c == '@' || c == ':' || c == '+' || c == '=';
        if (!ok) return false;
    }
    return true;
}

/* Catalog of CLI coding agents liu knows about. Kept in sync with
 * history/event.h ChatTool enum. `resume_fmt` may be NULL when the agent has
 * no documented native resume flag — then only "start new" is offered.
 * `resume_needs_path` toggles whether the %s is substituted with the
 * session-id or the full on-disk path (Command Code wants the path; see its
 * row). `binary` is the $PATH probe name; `new_cmd` runs a fresh session. */
typedef struct {
    u8          tool;              /* ChatTool */
    const char *display;           /* human-readable name */
    const char *binary;            /* $PATH probe target */
    const char *resume_fmt;        /* printf fmt with one %s; NULL → no resume */
    const char *new_cmd;           /* command that starts a fresh session */
    /* Cross-tool handoff: printf fmt with one %s that receives a shell-quoted
     * prompt (e.g. `'Read /path/handoff.md and continue from there'`). The
     * picker uses this to offer "Continue with <other>" for chats originating
     * in a different tool — the source transcript is dumped to a markdown
     * file under ~/.liu-handoffs/ and the target CLI is launched with the
     * pointer as its initial user message. NULL when no CLI exists that can
     * accept a prompt argv (editor extensions, GUI editors). */
    const char *cross_tool_fmt;
    bool        resume_needs_path; /* if true, resume_fmt's %s = session path (not id) */
} AgentCatalogEntry;

/* Each picker command is formatted into a fixed-size buffer. If the command
 * does not fit, truncation changes the command and may leave shell quoting
 * unbalanced. Reject a command that would be truncated instead of executing a
 * partial string. */
static inline bool agent_cmd_fits(int written, usize cap) {
    return written > 0 && (usize)written < cap;
}

static const AgentCatalogEntry AGENT_CATALOG[] = {
    /* tool                  display            binary         resume_fmt                               new_cmd        cross_tool_fmt        needs_path */
    { CHAT_TOOL_CLAUDE,      "Claude",          "claude",      "claude --resume %s",                    "claude",       "claude %s",          false },
    /* codex resume expects a thread UUID. The scanner identifies Codex sessions
     * by rollout filename, so codex_thread_uuid() extracts the UUID used by the
     * resume command; passing the full rollout filename does not match the
     * command's argument format. */
    { CHAT_TOOL_CODEX,       "Codex",           "codex",       "codex resume %s",                       "codex",        "codex %s",           false },
    { CHAT_TOOL_COPILOT,     "GitHub Copilot",  "copilot",     "copilot --resume %s",                   "copilot",      "copilot -i %s",      false },
    /* Probe the explicit cursor-agent executable so detection does not select
     * a different CLI that also provides the generic agent alias on PATH. */
    { CHAT_TOOL_CURSOR,      "Cursor",          "cursor-agent","cursor-agent --resume %s",              "cursor-agent", "cursor-agent %s",    false },
    { CHAT_TOOL_AMP,         "Amp",             "amp",         "amp --thread %s",                       "amp",          "amp %s",             false },
    /* Cline/Roo/Kilo are editor extensions — no CLI entry to hand a prompt to. */
    { CHAT_TOOL_CLINE,       "Cline",           "code",        NULL,                                    "code",         NULL,                 false },
    { CHAT_TOOL_ROO,         "Roo Code",        "code",        NULL,                                    "code",         NULL,                 false },
    { CHAT_TOOL_KILO,        "Kilo Code",       "code",        NULL,                                    "code",         NULL,                 false },
    { CHAT_TOOL_KIRO,        "Kiro",            "kiro",        NULL,                                    "kiro",         NULL,                 false },
    { CHAT_TOOL_CRUSH,       "Crush",           "crush",       "crush --session %s",                    "crush",        "crush %s",           false },
    { CHAT_TOOL_OPENCODE,    "OpenCode",        "opencode",    "opencode --session %s",                 "opencode",     "opencode run %s",    false },
    { CHAT_TOOL_DROID,       "Factory Droid",   "droid",       "droid -s %s",                           "droid",        "droid exec %s",      false },
    { CHAT_TOOL_ANTIGRAVITY, "Antigravity",     "agy",         "agy --conversation %s",                "agy",          "agy -i %s",          false },
    { CHAT_TOOL_KIMI,        "Kimi",            "kimi",        "kimi --session %s",                     "kimi",         "kimi --prompt %s",   false },
    { CHAT_TOOL_QWEN,        "Qwen",            "qwen",        "qwen --resume %s",                      "qwen",         "qwen %s",            false },
    { CHAT_TOOL_AMAZON_Q,    "Amazon Q",        "q",           NULL,                                    "q chat",       NULL,                 false },
    { CHAT_TOOL_CONTINUE,    "Continue",        "cn",          NULL,                                    "cn",           NULL,                 false },
    { CHAT_TOOL_WINDSURF,    "Windsurf",        "windsurf",    NULL,                                    "windsurf .",   NULL,                 false },
    /* Grok's TUI takes no positional prompt (only headless -p), so there is no
     * interactive cross-tool handoff target. */
    { CHAT_TOOL_XAI,         "Grok",            "grok",        "grok --resume %s",                      "grok",         NULL,                 false },
    /* --session takes a transcript path, which is what makes this exact: its
     * sibling --resume matches an id *or* a conversation name and only looks
     * inside the current directory's project bucket, so resuming from a tab
     * sitting anywhere else would silently drop into commandcode's own
     * history picker. */
    { CHAT_TOOL_COMMANDCODE, "Command Code",    "commandcode", "commandcode --session %s",              "commandcode",  "commandcode %s",     true  },
    /* The standalone Cline CLI, distinct from the CHAT_TOOL_CLINE row above
     * (that one is the VS Code extension, which has no CLI to resume into).
     * `--id` takes the session directory's name, which is exactly the id the
     * scanner records, so the %s needs no path. */
    { CHAT_TOOL_CLINE_CLI,   "Cline CLI",       "cline",       "cline --id %s",                         "cline",        "cline %s",           false },
    /* Pi and Oh My Pi share a session layout. Both resume by the session's
     * UUID (`pi --session <path|id>`, `omp --resume <id prefix|path>`), which
     * the scanner stores as the session id: unlike the file's path it is made
     * of characters agent_arg_is_safe() lets through (a folder called
     * "Arşiv 2" is in both stores), and the picker runs it in the folder the
     * session ran in, where each looks first. Both take the opening message as
     * a positional argument, which is what a handoff needs. omp's `--resume`
     * takes its value OPTIONALLY (a picker without one), so the id is glued on
     * with `=`: spaced, it could be read as the first message instead. */
    { CHAT_TOOL_PI,          "Pi",              "pi",          "pi --session %s",                       "pi",           "pi %s",              false },
    { CHAT_TOOL_OMP,         "Oh My Pi",        "omp",         "omp --resume=%s",                       "omp",          "omp %s",             false },
    /* OpenClaw: `tui --session <key>` selects a session by its key
     * ("agent:main:main" — colons are inside agent_arg_is_safe's set);
     * `chat --message` opens the embedded TUI with a first message and needs
     * no Gateway. Hermes: `--resume <id>` (id, title or "latest"); `chat -q`
     * submits the query as the first turn and keeps the session open on a TTY.
     */
    { CHAT_TOOL_OPENCLAW,    "OpenClaw",        "openclaw",    "openclaw tui --session %s",             "openclaw tui", "openclaw chat --message %s", false },
    { CHAT_TOOL_HERMES,      "Hermes",          "hermes",      "hermes --resume %s",                    "hermes",       "hermes chat -q %s",  false },
    /* Devin CLI: `--resume <id>` takes the session's id from its store
     * (sessions.id, a word pair). Qoder CLI: `--resume <uuid>`. Letta Code:
     * `--conversation <id>`, and no first message on its command line. Mastra
     * Code: `--thread <id>`, and `--tui-prompt` opens the interface with a
     * first message sent. Maki: `--resume <id>`, the opening message
     * positional. Prime Agent: `--resume <id|path>`. Muse Code resumes from
     * inside its own interface only (`/resume`), so its row starts a session
     * and resumes none. */
    { CHAT_TOOL_DEVIN,       "Devin",           "devin",       "devin --resume %s",                     "devin",        NULL,                 false },
    { CHAT_TOOL_QODER,       "Qoder",           "qodercli",    "qodercli --resume %s",                  "qodercli",     NULL,                 false },
    { CHAT_TOOL_LETTA,       "Letta Code",      "letta",       "letta --conversation %s",               "letta",        NULL,                 false },
    { CHAT_TOOL_MASTRACODE,  "Mastra Code",     "mastracode",  "mastracode --thread %s",                "mastracode",   "mastracode --tui-prompt %s", false },
    { CHAT_TOOL_MAKI,        "Maki",            "maki",        "maki --resume %s",                      "maki",         "maki %s",            false },
    { CHAT_TOOL_MUSE,        "Muse Code",       "muse",        NULL,                                    "muse",         NULL,                 false },
    { CHAT_TOOL_PRIME,       "Prime Agent",     "prime-agent", "prime-agent --resume %s",               "prime-agent",  NULL,                 false },
};
#define AGENT_CATALOG_COUNT ((i32)(sizeof AGENT_CATALOG / sizeof AGENT_CATALOG[0]))

/* The row for `tool`, or NULL when Liu has none. */
static inline const AgentCatalogEntry *agent_catalog_find(i32 tool) {
    for (i32 i = 0; i < AGENT_CATALOG_COUNT; i++)
        if (AGENT_CATALOG[i].tool == tool) return &AGENT_CATALOG[i];
    return NULL;
}

/* The argument `tool`'s resume flag takes, out of what is known about a
 * session: its id, or its file's path for a row that resumes by path. Codex
 * resumes by the thread UUID at the end of the rollout id the scanner
 * records. False when the needed half is missing, cannot be converted, or is
 * not an argument a command line may carry. */
static inline bool agent_resume_arg(const AgentCatalogEntry *e, const char *sid,
                                    const char *path, char *out, usize cap) {
    if (!e || !out || cap == 0) return false;
    out[0] = '\0';
    const char *arg = e->resume_needs_path ? path : sid;
    if (!arg || !arg[0]) return false;
    if (e->tool == CHAT_TOOL_CODEX) {
        char uuid[40];
        if (!codex_thread_uuid(arg, uuid, sizeof uuid)) return false;
        if ((usize)snprintf(out, cap, "%s", uuid) >= cap) { out[0] = '\0'; return false; }
        return true;
    }
    if (!agent_arg_is_safe(arg)) return false;
    if ((usize)snprintf(out, cap, "%s", arg) >= cap) { out[0] = '\0'; return false; }
    return true;
}

/* The command line that resumes a stored session in `tool`, into `out`.
 * False when the agent has no resume flag that names a session, the session
 * is not known well enough to name, or the command does not fit — a resume
 * is never degraded to a guessed argument or to the agent's own picker. A
 * row whose flag takes no argument resumes without one. */
static inline bool agent_resume_command(i32 tool, const char *sid, const char *path,
                                        char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    const AgentCatalogEntry *e = agent_catalog_find(tool);
    if (!e || !e->resume_fmt) return false;
    /* A flag with no argument reopens the one history the agent keeps for
     * the folder. */
    if (!strstr(e->resume_fmt, "%s")) {
        int n = snprintf(out, cap, "%s", e->resume_fmt);
        if (!agent_cmd_fits(n, cap)) { out[0] = '\0'; return false; }
        return true;
    }
    char arg[1024];
    if (!agent_resume_arg(e, sid, path, arg, sizeof arg)) return false;
    int n = snprintf(out, cap, e->resume_fmt, arg);
    if (!agent_cmd_fits(n, cap)) { out[0] = '\0'; return false; }
    return true;
}

/* The command that starts `tool` afresh in a terminal, for a pane whose agent
 * had not opened a session yet (agents that write their session only with the
 * first message). False for the rows whose start command opens an editor or
 * an IDE rather than a terminal program. */
static inline bool agent_start_command(i32 tool, char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    switch ((ChatTool)tool) {
        case CHAT_TOOL_CLINE: case CHAT_TOOL_ROO: case CHAT_TOOL_KILO:
        case CHAT_TOOL_KIRO:  case CHAT_TOOL_WINDSURF:
            return false;
        default:
            break;
    }
    const AgentCatalogEntry *e = agent_catalog_find(tool);
    if (!e || !e->new_cmd || !e->new_cmd[0]) return false;
    int n = snprintf(out, cap, "%s", e->new_cmd);
    if (!agent_cmd_fits(n, cap)) { out[0] = '\0'; return false; }
    return true;
}

#endif /* UI_AGENT_CATALOG_H */
