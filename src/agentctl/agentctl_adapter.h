/*
 * Liu — the agents a pane may start through agentctl, and how each is run.
 *
 * A delegated agent runs headless, one process per turn, in its structured
 * output mode: `codex exec --json`, `claude -p --output-format stream-json`,
 * `opencode run --format json` and `gemini -o stream-json`. The runner reads
 * that stream and draws it as plain lines in the job's tab; the first turn
 * starts a session and every later turn resumes it, so nothing of the agent
 * stays in memory between turns.
 *
 * The prompt never appears on a command line: it is written to the agent's
 * stdin (each of the four reads a piped prompt there). Every other argument
 * is either a literal of this table or a value checked here — the agent from
 * this list, a model name of a closed character set, an effort from a closed
 * list, a session id from the agent's own stream — and the vector goes to
 * posix_spawnp, never through a shell.
 *
 * Permission levels and what each agent is told:
 *
 *            codex exec -s        claude --permission-mode   opencode run     gemini --approval-mode
 *   read     read-only            plan                       --agent plan     plan
 *   write    workspace-write      acceptEdits                (its default)    auto_edit
 *   full     danger-full-access   bypassPermissions          --auto           yolo
 *
 * What a level does not allow fails inside the turn, except for Claude with
 * `ask_tools`: its stdin is then a stream of JSON messages and every tool its
 * level would refuse is asked of the host (`--permission-prompt-tool stdio`),
 * which puts it to the user. Any allow-lists in the user's own agent settings
 * still apply.
 *
 * Pure: no I/O, no globals. Shared by Liu and the runner.
 */
#ifndef AGENTCTL_ADAPTER_H
#define AGENTCTL_ADAPTER_H

#include "core/types.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    AGENTCTL_AGENT_CODEX = 0,
    AGENTCTL_AGENT_CLAUDE,
    AGENTCTL_AGENT_OPENCODE,
    AGENTCTL_AGENT_GEMINI,
    AGENTCTL_AGENT_COUNT,
} AgentctlAgent;

typedef struct {
    const char *id;        /* what a command line says: "codex" */
    const char *display;   /* what a person reads: "Codex" */
    const char *binary;    /* looked up on the pane's PATH */
} AgentctlAgentInfo;

static const AgentctlAgentInfo AGENTCTL_AGENTS[AGENTCTL_AGENT_COUNT] = {
    { "codex",    "Codex",    "codex"    },
    { "claude",   "Claude",   "claude"   },
    { "opencode", "OpenCode", "opencode" },
    { "gemini",   "Gemini",   "gemini"   },
};

/* The agents as a command line lists them: "codex, claude, opencode, gemini". */
#define AGENTCTL_AGENT_LIST "codex, claude, opencode, gemini"

static inline const AgentctlAgentInfo *agentctl_agent_info(i32 agent) {
    return (agent >= 0 && agent < AGENTCTL_AGENT_COUNT) ? &AGENTCTL_AGENTS[agent] : NULL;
}

/* -1 when `s` names no agent this table runs. */
static inline i32 agentctl_agent_parse(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "codex")) return AGENTCTL_AGENT_CODEX;
    if (!strcmp(s, "claude") || !strcmp(s, "claude-code")) return AGENTCTL_AGENT_CLAUDE;
    if (!strcmp(s, "opencode")) return AGENTCTL_AGENT_OPENCODE;
    if (!strcmp(s, "gemini") || !strcmp(s, "gemini-cli")) return AGENTCTL_AGENT_GEMINI;
    return -1;
}

/* Whether the agent takes a reasoning effort on its command line. */
static inline bool agentctl_agent_has_effort(i32 agent) {
    return agent == AGENTCTL_AGENT_CODEX || agent == AGENTCTL_AGENT_CLAUDE;
}

/* Whether the agent can put a tool its level refuses to the host: Claude's
 * permission prompts over stdio. */
static inline bool agentctl_agent_can_ask(i32 agent) {
    return agent == AGENTCTL_AGENT_CLAUDE;
}

typedef enum {
    AGENTCTL_PERM_READ = 0,
    AGENTCTL_PERM_WRITE,
    AGENTCTL_PERM_FULL,
    AGENTCTL_PERM_COUNT,
} AgentctlPerm;

static inline const char *agentctl_perm_name(i32 p) {
    switch (p) {
        case AGENTCTL_PERM_READ:  return "read";
        case AGENTCTL_PERM_WRITE: return "write";
        case AGENTCTL_PERM_FULL:  return "full";
        default:                  return "";
    }
}

/* What the approval dialog and the tab say about a level. */
static inline const char *agentctl_perm_describe(i32 p) {
    switch (p) {
        case AGENTCTL_PERM_READ:  return "Read only";
        case AGENTCTL_PERM_WRITE: return "Can write in the folder";
        case AGENTCTL_PERM_FULL:  return "Full access, no sandbox";
        default:                  return "";
    }
}

static inline i32 agentctl_perm_parse(const char *s) {
    if (!s) return -1;
    for (i32 p = 0; p < AGENTCTL_PERM_COUNT; p++)
        if (!strcmp(s, agentctl_perm_name(p))) return p;
    return -1;
}

#define AGENTCTL_MODEL_CAP   65
#define AGENTCTL_EFFORT_CAP  16
#define AGENTCTL_LABEL_CAP   64
#define AGENTCTL_SESSION_CAP 128

/* A model name as both CLIs spell them ("gpt-6.1-sol", "claude-opus-5-5",
 * "openai/gpt-5"): letters, digits and . _ : / @ + -, at most 64 bytes, and
 * never a leading '-' (it would be read as an option). "" = the agent's own
 * default. */
static inline bool agentctl_model_ok(const char *s) {
    if (!s) return false;
    usize n = strlen(s);
    if (n == 0) return true;
    if (n >= AGENTCTL_MODEL_CAP || s[0] == '-') return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == ':' || c == '/' || c == '@' || c == '+' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/* Reasoning effort, from the words either CLI takes. "" = the default. */
static inline bool agentctl_effort_ok(const char *s) {
    if (!s) return false;
    if (!s[0]) return true;
    static const char *const words[] = { "none", "minimal", "low", "medium", "high", "xhigh", "max" };
    for (usize i = 0; i < sizeof words / sizeof words[0]; i++)
        if (!strcmp(s, words[i])) return true;
    return false;
}

/* A session id out of the agent's stream, before it goes back on a command
 * line: a UUID or a word of the same characters, never a leading '-'. */
static inline bool agentctl_session_ok(const char *s) {
    if (!s || !s[0] || s[0] == '-' || strlen(s) >= AGENTCTL_SESSION_CAP) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

/* Storage for the composed arguments an argv points into. */
typedef struct {
    char effort_kv[48];   /* model_reasoning_effort="xhigh" */
    char model_kv[96];    /* model="gpt-6.1-sol" */
    char budget[32];      /* --max-budget-usd's value */
} AgentctlArgvScratch;

#define AGENTCTL_ARGV_CAP 32

/* The commands a delegated Claude may run without asking: the ones that read
 * and write its team (`liu-agent team`, `post`, `inbox`, `owns`). */
static const char *const AGENTCTL_TEAM_TOOLS[] = {
    "Bash(liu-agent team:*)", "Bash(liu-agent post:*)", "Bash(liu-agent inbox:*)", "Bash(liu-agent owns:*)",
};

/* What one turn is run with beyond the job's agent, model, effort, level
 * and session. Zero = none of it. */
typedef struct {
    bool        ask_tools;      /* Claude: tools its level refuses are asked of the host */
    const char *settings_json;  /* Claude: `--settings`, the guard hook of its team ("" or NULL = none) */
    u64         budget_micro;   /* Claude: what the turn may still spend, millionths of a dollar */
} AgentctlTurnOpts;

/* The argv of one turn: a new session when `session` is NULL or "", the
 * continuation of `session` otherwise. Returns argc (out[argc] = NULL), or 0
 * when an input is refused or `cap` is too small. */
static inline i32 agentctl_turn_argv_ex(i32 agent, const char *model, const char *effort, i32 perm,
                                        const char *cwd, const char *session, const AgentctlTurnOpts *opts,
                                        AgentctlArgvScratch *sc, const char **out, i32 cap) {
    if (!sc || !out || cap < AGENTCTL_ARGV_CAP) return 0;
    if (!model) model = "";
    if (!effort) effort = "";
    AgentctlTurnOpts none = { false, NULL, 0 };
    if (!opts) opts = &none;
    bool resume = session && session[0];
    if (!agentctl_model_ok(model) || !agentctl_effort_ok(effort) ||
        perm < 0 || perm >= AGENTCTL_PERM_COUNT || (resume && !agentctl_session_ok(session)))
        return 0;
    i32 n = 0;
    if (agent == AGENTCTL_AGENT_CODEX) {
        static const char *const sandbox[] = { "read-only", "workspace-write", "danger-full-access" };
        if (!cwd || cwd[0] != '/') return 0;
        out[n++] = "codex";
        out[n++] = "exec";
        out[n++] = "--json";
        out[n++] = "--skip-git-repo-check";
        out[n++] = "-C";
        out[n++] = cwd;
        out[n++] = "-s";
        out[n++] = sandbox[perm];
        if (model[0]) { out[n++] = "-m"; out[n++] = model; }
        if (effort[0]) {
            int w = snprintf(sc->effort_kv, sizeof sc->effort_kv, "model_reasoning_effort=\"%s\"", effort);
            if (w <= 0 || (usize)w >= sizeof sc->effort_kv) return 0;
            out[n++] = "-c";
            out[n++] = sc->effort_kv;
        }
        /* exec's options belong before its subcommand. */
        if (resume) { out[n++] = "resume"; out[n++] = session; }
        out[n++] = "-";
    } else if (agent == AGENTCTL_AGENT_CLAUDE) {
        static const char *const mode[] = { "plan", "acceptEdits", "bypassPermissions" };
        out[n++] = "claude";
        out[n++] = "-p";
        out[n++] = "--output-format";
        out[n++] = "stream-json";
        out[n++] = "--verbose";
        if (opts->ask_tools) {
            /* the message comes as a JSON line on stdin, and the permission
             * prompts go out on stdout and are answered on stdin */
            out[n++] = "--input-format";
            out[n++] = "stream-json";
            out[n++] = "--permission-prompt-tool";
            out[n++] = "stdio";
        }
        /* A headless turn runs no command it was not allowed: the team's
         * own (agentctl_team.h) are, and nothing else is added. */
        out[n++] = "--allowedTools";
        for (usize t = 0; t < sizeof AGENTCTL_TEAM_TOOLS / sizeof AGENTCTL_TEAM_TOOLS[0]; t++)
            out[n++] = AGENTCTL_TEAM_TOOLS[t];
        out[n++] = "--permission-mode";
        out[n++] = mode[perm];
        if (opts->settings_json && opts->settings_json[0]) { out[n++] = "--settings"; out[n++] = opts->settings_json; }
        if (opts->budget_micro) {
            int w = snprintf(sc->budget, sizeof sc->budget, "%llu.%06llu",
                             (unsigned long long)(opts->budget_micro / 1000000u),
                             (unsigned long long)(opts->budget_micro % 1000000u));
            if (w <= 0 || (usize)w >= sizeof sc->budget) return 0;
            out[n++] = "--max-budget-usd";
            out[n++] = sc->budget;
        }
        if (model[0])  { out[n++] = "--model";  out[n++] = model; }
        if (effort[0]) { out[n++] = "--effort"; out[n++] = effort; }
        if (resume)    { out[n++] = "--resume"; out[n++] = session; }
    } else if (agent == AGENTCTL_AGENT_OPENCODE) {
        out[n++] = "opencode";
        out[n++] = "run";
        out[n++] = "--format";
        out[n++] = "json";
        if (perm == AGENTCTL_PERM_READ)      { out[n++] = "--agent"; out[n++] = "plan"; }
        else if (perm == AGENTCTL_PERM_FULL) out[n++] = "--auto";
        if (model[0])  { out[n++] = "-m"; out[n++] = model; }
        if (resume)    { out[n++] = "-s"; out[n++] = session; }
    } else if (agent == AGENTCTL_AGENT_GEMINI) {
        static const char *const mode[] = { "plan", "auto_edit", "yolo" };
        out[n++] = "gemini";
        out[n++] = "-o";
        out[n++] = "stream-json";
        out[n++] = "--approval-mode";
        out[n++] = mode[perm];
        if (model[0])  { out[n++] = "-m"; out[n++] = model; }
        if (resume)    { out[n++] = "--resume"; out[n++] = session; }
    } else {
        return 0;
    }
    if (n > cap) return 0;
    out[n] = NULL;
    return n;
}

static inline i32 agentctl_turn_argv(i32 agent, const char *model, const char *effort, i32 perm,
                                     const char *cwd, const char *session,
                                     AgentctlArgvScratch *sc, const char **out, i32 cap) {
    return agentctl_turn_argv_ex(agent, model, effort, perm, cwd, session, NULL, sc, out, cap);
}

/* The argv that opens `session` in the agent's own interactive interface,
 * with the job's model, effort and permission level: what the tab runs when
 * the user takes the session over. */
static inline i32 agentctl_takeover_argv(i32 agent, const char *model, const char *effort, i32 perm,
                                         const char *session, AgentctlArgvScratch *sc,
                                         const char **out, i32 cap) {
    if (!sc || !out || cap < AGENTCTL_ARGV_CAP) return 0;
    if (!model) model = "";
    if (!effort) effort = "";
    if (!agentctl_session_ok(session) || !agentctl_model_ok(model) || !agentctl_effort_ok(effort) ||
        perm < 0 || perm >= AGENTCTL_PERM_COUNT)
        return 0;
    i32 n = 0;
    if (agent == AGENTCTL_AGENT_CODEX) {
        static const char *const sandbox[] = { "read-only", "workspace-write", "danger-full-access" };
        out[n++] = "codex";
        /* Root options, so they apply to the subcommand that follows. */
        out[n++] = "-s";
        out[n++] = sandbox[perm];
        if (model[0]) {
            int w = snprintf(sc->model_kv, sizeof sc->model_kv, "model=\"%s\"", model);
            if (w <= 0 || (usize)w >= sizeof sc->model_kv) return 0;
            out[n++] = "-c";
            out[n++] = sc->model_kv;
        }
        if (effort[0]) {
            int w = snprintf(sc->effort_kv, sizeof sc->effort_kv, "model_reasoning_effort=\"%s\"", effort);
            if (w <= 0 || (usize)w >= sizeof sc->effort_kv) return 0;
            out[n++] = "-c";
            out[n++] = sc->effort_kv;
        }
        out[n++] = "resume";
        out[n++] = session;
    } else if (agent == AGENTCTL_AGENT_CLAUDE) {
        static const char *const mode[] = { "plan", "acceptEdits", "bypassPermissions" };
        out[n++] = "claude";
        out[n++] = "--resume";
        out[n++] = session;
        out[n++] = "--permission-mode";
        out[n++] = mode[perm];
        if (model[0])  { out[n++] = "--model";  out[n++] = model; }
        if (effort[0]) { out[n++] = "--effort"; out[n++] = effort; }
    } else if (agent == AGENTCTL_AGENT_OPENCODE) {
        out[n++] = "opencode";
        out[n++] = "-s";
        out[n++] = session;
        if (model[0]) { out[n++] = "-m"; out[n++] = model; }
    } else if (agent == AGENTCTL_AGENT_GEMINI) {
        static const char *const mode[] = { "plan", "auto_edit", "yolo" };
        out[n++] = "gemini";
        out[n++] = "--resume";
        out[n++] = session;
        out[n++] = "--approval-mode";
        out[n++] = mode[perm];
        if (model[0]) { out[n++] = "-m"; out[n++] = model; }
    } else {
        return 0;
    }
    out[n] = NULL;
    return n;
}

/* An argv as one shell line, every word in single quotes. False when a word
 * holds a single quote or a control byte (no argv this file builds does) or
 * the line does not fit. */
static inline bool agentctl_shell_join(const char *const *argv, char *out, usize cap) {
    if (!argv || !out || cap == 0) return false;
    usize o = 0;
    for (i32 i = 0; argv[i]; i++) {
        for (const unsigned char *p = (const unsigned char *)argv[i]; *p; p++)
            if (*p == '\'' || *p < 0x20 || *p == 0x7f) return false;
        usize n = strlen(argv[i]);
        if (o + n + 4 >= cap) return false;
        if (i) out[o++] = ' ';
        out[o++] = '\'';
        memcpy(out + o, argv[i], n);
        o += n;
        out[o++] = '\'';
    }
    out[o] = '\0';
    return o > 0;
}

/* ---------------------------------------------------------- job states --- */

typedef enum {
    AGJ_FREE = 0,
    AGJ_PENDING_APPROVAL,   /* the dialog is up (or queued) in the owner's window */
    AGJ_DECLINED,
    AGJ_QUEUED,             /* approved; waits for a slot under the concurrency limit */
    AGJ_STARTING,           /* the runner is started, not connected yet */
    AGJ_RUNNING,            /* a turn is in flight */
    AGJ_IDLE,               /* the last turn ended (see AgentctlOutcome); takes a message */
    AGJ_ENDED,              /* closed, taken over, or its runner is gone */
} AgentctlState;

typedef enum {
    AGO_NONE = 0,
    AGO_DONE,
    AGO_FAILED,
    AGO_STOPPED,
} AgentctlOutcome;

typedef enum {
    AGE_NONE = 0,
    AGE_CLOSED,
    AGE_TAKEN_OVER,
    AGE_RUNNER_EXITED,
    AGE_COULD_NOT_START,
    AGE_MERGED,             /* an isolated job's work went into its folder */
    AGE_DISCARDED,          /* an isolated job's work was thrown away */
} AgentctlEnd;

static inline const char *agentctl_state_name(i32 s) {
    switch (s) {
        case AGJ_PENDING_APPROVAL: return "awaiting_approval";
        case AGJ_DECLINED:         return "declined";
        case AGJ_QUEUED:           return "queued";
        case AGJ_STARTING:         return "starting";
        case AGJ_RUNNING:          return "running";
        case AGJ_IDLE:             return "idle";
        case AGJ_ENDED:            return "ended";
        default:                   return "free";
    }
}

static inline const char *agentctl_outcome_name(i32 o) {
    switch (o) {
        case AGO_DONE:    return "done";
        case AGO_FAILED:  return "failed";
        case AGO_STOPPED: return "stopped";
        default:          return "none";
    }
}

static inline const char *agentctl_end_name(i32 e) {
    switch (e) {
        case AGE_CLOSED:          return "closed";
        case AGE_TAKEN_OVER:      return "taken_over";
        case AGE_RUNNER_EXITED:   return "runner_exited";
        case AGE_COULD_NOT_START: return "could_not_start";
        case AGE_MERGED:          return "merged";
        case AGE_DISCARDED:       return "discarded";
        default:                  return "none";
    }
}

/* A WAIT on the job is answered once it is in one of these states. */
static inline bool agentctl_state_settled(i32 s) {
    return s == AGJ_DECLINED || s == AGJ_IDLE || s == AGJ_ENDED;
}

#endif /* AGENTCTL_ADAPTER_H */
