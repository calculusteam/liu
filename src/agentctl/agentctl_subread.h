/*
 * Liu — reading a subagent from its agent's own store.
 *
 * An agent CLI that runs subagents of its own keeps a record of each:
 *
 *   Claude Code  <projects>/<slug>/<session>/subagents/agent-<id>.jsonl, with
 *                agent-<id>.meta.json beside it (its description, type, and
 *                `finished` once done). The transcript's lines are the
 *                stream-json lines of `claude -p`, so the delegated reader
 *                (agentctl_stream.h) shows them.
 *   Codex        a rollout of its own in the day's folder, whose header names
 *                the thread that spawned it (`parent_thread_id`); a forked one
 *                carries its parent's history before
 *                `subagent_history_start_ordinal`.
 *   OpenCode     a session row whose `parent_id` is the parent session, its
 *                messages rows of `session_message` (2.x).
 *   Grok         <session>/subagents/<child>/meta.json (status, description),
 *                the child's transcript a session folder of its own beside the
 *                parent's.
 *   Kimi         <session>/subagents/<id>/{meta.json, context.jsonl}.
 *   Oh My Pi     <session file without .jsonl>/<Name>.jsonl, whose `session`
 *                line names the parent file.
 *   Command Code no transcript of its own: the parent's `agent` tool calls,
 *                their results, and `agent_output` for one run in the
 *                background.
 *   Qwen Code    <project>/subagents/<session>/agent-<id>.jsonl, with
 *                agent-<id>.meta.json beside it (its status).
 *   Droid        a session file of its own beside the parent's, whose
 *                `session_start` line names the calling session.
 *   Cline CLI    <session folder>/<agent id>.messages.json beside the root's
 *                own messages (the whole file, rewritten as it grows).
 *   Crush        a `sessions` row whose `parent_session_id` is the parent,
 *                its `messages` rows' parts.
 *   Hermes       a `sessions` row of source "subagent" under the parent, its
 *                `messages` rows (OpenAI-shaped calls).
 *   OpenClaw     a `session_nodes` row under the parent's key, its transcript
 *                events (Pi's lines; those kept compressed are not read).
 *   Copilot CLI  `subagent.*` events in the parent's events.jsonl, and the
 *                subagent's own events there, marked with its call.
 *   Pi           its `subagent` extension's calls and their results.
 *   Devin        `run_subagent` calls of the main conversation and their
 *                results.
 *   Qoder CLI    as Claude Code: <session>/subagents/agent-<id>.jsonl.
 *   Cursor       <project>/agent-transcripts/<parent>/subagents/<id>.jsonl.
 *   Kilo CLI     a `session` row whose `parent_id` is the parent, its
 *                `message` and `part` rows (OpenCode 1.x's tables).
 *   Amazon Q     <folder>/.amazonq/.subagents/<agent>.json, one task each.
 *
 * This module is pure: it turns a record into the lines a job tab shows (the
 * styles of agentctl_stream.h, the same marks: "▸ $" a command, "✎" an edit,
 * "⚙" a tool) and keeps what it says about the subagent's state. The reading
 * of files and rows is agentctl_subagents.c's. Every piece of text passes
 * agentctl_sanitize() before it is handed on.
 */
#ifndef AGENTCTL_SUBREAD_H
#define AGENTCTL_SUBREAD_H

#include "core/types.h"
#include "agentctl/agentctl_stream.h"

#include <stdbool.h>

/* A subagent's state, as its own record says it. */
typedef enum {
    AGSUB_UNKNOWN = 0,
    AGSUB_RUNNING,
    AGSUB_DONE,
    AGSUB_FAILED,
    AGSUB_STOPPED,
} AgentctlSubStatus;

typedef struct {
    i32   tool;                      /* the agent's ChatTool */
    AgentctlStream claude;           /* Claude Code: its transcript is stream-json */
    bool  claude_on;
    u8    status;                    /* AgentctlSubStatus */
    bool  settle_shown;              /* the frame line for the current status was emitted */
    bool  task_shown;
    i64   start_ms, last_ms, end_ms; /* wall clock, from the records; 0 = not known */
    char  model[96];
    char  kind[64];                  /* its type, when the transcript says it (Oh My Pi) */
    char  task_head[160];            /* the first line of its task, once shown */
    char  error[256];
    u64   tok_in, tok_out;
    u32   ordinal_from;              /* Codex: records before this are the parent's */
    bool  codex_items;               /* Codex writes item_completed: its older events are not read */
    bool  claude_answered;           /* Claude: the last message was a final answer */
    u32   lines;                     /* lines emitted */
} AgentctlSubRead;

void agentctl_subread_init(AgentctlSubRead *r, i32 tool);
void agentctl_subread_free(AgentctlSubRead *r);

/* The task it was given, at the head of its log: the first line strong, a
 * few more, then how many were left out. Shown once; later calls are no-ops. */
void agentctl_subread_task(AgentctlSubRead *r, const char *task, usize len, AgentctlEmit emit, void *ctx);

/* One whole line of the subagent's transcript (Claude, Codex, Grok, Kimi,
 * Oh My Pi). An empty or unreadable line is skipped. */
void agentctl_subread_line(AgentctlSubRead *r, const char *line, usize len, AgentctlEmit emit, void *ctx);

/* One Crush `messages` row: its role, its parts (JSON) and whether it is
 * finished. False when it is an assistant message still being written. */
bool agentctl_subread_crush_row(AgentctlSubRead *r, const char *role, const char *parts, usize len, bool finished,
                                i64 ts_ms, AgentctlEmit emit, void *ctx);

/* One message as a chat row keeps it (Hermes): a role, its text and its
 * calls as an OpenAI-shaped JSON array. */
void agentctl_subread_chat_row(AgentctlSubRead *r, const char *role, const char *content, const char *tool_calls,
                               i64 ts_ms, AgentctlEmit emit, void *ctx);

/* A Cline CLI messages file, whole: the messages from index `from` on.
 * Returns how many it holds (the next `from`). */
u32 agentctl_subread_cline_messages(AgentctlSubRead *r, const char *json, usize len, u32 from,
                                    AgentctlEmit emit, void *ctx);

/* An OpenCode 1.x `message` row's data (Kilo CLI): its role into `role`;
 * false when it is an assistant message still being written. */
bool agentctl_subread_oc1_message(AgentctlSubRead *r, const char *data, usize len, char *role, usize cap,
                                  AgentctlEmit emit, void *ctx);
/* One of its `part` rows. */
void agentctl_subread_oc1_part(AgentctlSubRead *r, const char *role, const char *data, usize len,
                               AgentctlEmit emit, void *ctx);

/* Amazon Q's `.amazonq/.subagents/<agent>.json`, whole: its task, and once it
 * ended its summary (or output) and how. */
void agentctl_subread_amazonq(AgentctlSubRead *r, const char *json, usize len, AgentctlEmit emit, void *ctx);

/* A result of Pi's `subagent` call for one run (its `details.results[i]`):
 * the run's messages, then its end. */
void agentctl_subread_pi_result(AgentctlSubRead *r, const char *json, usize len, i64 ts_ms,
                                AgentctlEmit emit, void *ctx);

/* One OpenCode 2.x message row (`session_message.type`, `.data`). False when
 * it is an assistant message still being written: the caller reads it again
 * later and goes no further now. */
bool agentctl_subread_opencode_row(AgentctlSubRead *r, const char *type, const char *data, usize len,
                                   AgentctlEmit emit, void *ctx);

/* What a record outside the transcript says (its meta file, its row, the
 * parent's word): the status changes, and a settled one gets its frame line
 * ("── done · 3m12s", "── failed · why", "── stopped"). `why` may be NULL.
 * `when_ms` 0 = now as the records last said it. A running one again after
 * it settled (a follow-up message) just runs. */
void agentctl_subread_status(AgentctlSubRead *r, AgentctlSubStatus st, const char *why, i64 when_ms,
                             AgentctlEmit emit, void *ctx);

/* A finished subagent's whole answer, for the agents whose transcript Liu
 * cannot read (Command Code): shown as its own words. */
void agentctl_subread_answer(AgentctlSubRead *r, const char *text, usize len, AgentctlEmit emit, void *ctx);

/* ---- records about a subagent ------------------------------------------- */

typedef struct {
    char id[128];          /* the subagent's own id */
    char parent[1024];     /* the call, thread, session or file that started it */
    char label[160];       /* what it was asked to do, in a few words */
    char kind[64];         /* its type: "general-purpose", "explore", a nickname */
    char model[96];
    u8   status;           /* AgentctlSubStatus; UNKNOWN when the record does not say */
    i64  start_ms, end_ms; /* wall clock; 0 = not said */
    u32  ordinal_from;     /* Codex */
    i32  depth;            /* how deep below the pane's agent; 0 = not said */
} AgentctlSubMeta;

/* Claude Code's agent-<id>.meta.json. */
bool agentctl_submeta_claude(const char *json, usize len, AgentctlSubMeta *m);
/* Grok's subagents/<child>/meta.json. */
bool agentctl_submeta_grok(const char *json, usize len, AgentctlSubMeta *m);
/* Kimi's subagents/<id>/meta.json. */
bool agentctl_submeta_kimi(const char *json, usize len, AgentctlSubMeta *m);
/* A Codex rollout's first line: true only for a subagent's thread. */
bool agentctl_submeta_codex(const char *line, usize len, AgentctlSubMeta *m);
/* An Oh My Pi file's `session` line: true only when it names a parent. */
bool agentctl_submeta_omp(const char *line, usize len, AgentctlSubMeta *m);
/* Qwen Code's agent-<id>.meta.json, and the error it says it ended with. */
bool agentctl_submeta_qwen(const char *json, usize len, AgentctlSubMeta *m);
bool agentctl_submeta_qwen_error(const char *json, usize len, char *out, usize cap);
/* Amazon Q's `.amazonq/.subagents/<agent>.json`: `id` is the agent and
 * when it was launched (one task at a time an agent). */
bool agentctl_submeta_amazonq(const char *json, usize len, AgentctlSubMeta *m);
/* A Droid session file's first line (`session_start`); `parent` is the
 * calling session, "" for a session the user started. */
bool agentctl_submeta_droid(const char *line, usize len, AgentctlSubMeta *m);

/* ---- subagents in the parent's own log ---------------------------------- */

typedef enum {
    AGCC_SPAWN = 1,        /* a subagent's call: call_id, label, kind, text = its prompt, background */
    AGCC_RESULT,           /* a tool's result: call_id, text, failed (the caller keeps its own calls);
                            * `line` = a Pi run's whole result */
    AGCC_SETTLE,           /* the log says how it ended: call_id, status, text = why */
    AGCC_CHILD,            /* one of a subagent's own lines: `line`, by call_id and/or agent_id */
} AgentctlCcKind;

typedef struct {
    u8          kind;
    const char *call_id;
    const char *agent_id;
    const char *label;
    const char *agent_type;
    const char *model;
    const char *text;
    usize       text_len;
    const char *line;
    usize       line_len;
    u8          status;    /* AgentctlSubStatus, for AGCC_SETTLE */
    bool        background;
    bool        failed;
    i64         ts_ms;
} AgentctlCcEvent;

typedef void (*AgentctlCcFn)(void *ctx, const AgentctlCcEvent *e);
/* One line of a Command Code session; every `agent` call and every tool
 * result in it goes to `fn`. */
void agentctl_subread_cc_line(const char *line, usize len, AgentctlCcFn fn, void *ctx);

/* One line of a Pi session: its `subagent` calls and their results. */
void agentctl_subread_pi_line(const char *line, usize len, AgentctlCcFn fn, void *ctx);
/* One line of a Copilot CLI events.jsonl. */
void agentctl_subread_copilot_line(const char *line, usize len, AgentctlCcFn fn, void *ctx);
/* A Copilot CLI events.jsonl's first line (`session.start`): the folder it
 * was started in. */
bool agentctl_subread_copilot_start(const char *line, usize len, char *cwd, usize cap);
/* One Devin `message_nodes.chat_message`. */
void agentctl_subread_devin_row(const char *json, usize len, i64 ts_ms, AgentctlCcFn fn, void *ctx);

/* What a Command Code result says of a background agent: "Background agent
 * launched … agent_id: bg-…" names it (`bg_id`, status RUNNING); an
 * `agent_output` answer "agent_id: bg-… status: completed\n\n<report>" settles
 * it (`report` points into `text`). False when it says neither. */
bool agentctl_subread_cc_background(const char *text, usize len, char *bg_id, usize bg_cap,
                                    u8 *status, const char **report, usize *report_len);

/* ---- small things the reader and the watcher share ----------------------- */

/* "2026-10-29T17:42:56.444Z" (or with an offset) as wall-clock ms; 0 when it
 * is not one. */
i64  agentctl_iso_ms(const char *s);
/* A tool call as one line of the log: a shell command "▸ $ cmd", an edit
 * "✎ path", anything else "⚙ Name subject". `args` is the call's arguments as
 * JSON text (an object, or a string holding one); may be NULL. */
void agentctl_subread_call(const char *name, const char *args, usize args_len, AgentctlEmit emit, void *ctx);

#endif /* AGENTCTL_SUBREAD_H */
