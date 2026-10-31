/*
 * Liu — agent control, the app's side.
 *
 * An agent in a Liu pane runs `liu-agent spawn codex …`; Liu asks the user and
 * then runs the delegated agent through a runner (agentctl/runner.c), a
 * background process with no terminal that runs the agent one turn at a time
 * and sends Liu one line per event. Each job is a small tab holding the
 * agent's logo, joined to the right of the tab whose pane asked for it
 * (ui/agentctl_card.h); a click opens the job's log. The asking agent waits
 * on the job, reads its result and sends the next message.
 *
 * The server runs on a thread of its own and owns the socket, the job table,
 * every connection and every runner process (it starts them and collects
 * them). It sleeps in poll() with no timeout unless a runner is starting or
 * exiting. The main thread does only what needs the UI — the approval prompt,
 * the log panel, the job tabs — and learns that there is something to do from
 * agentctl_main_pending() (one atomic load per loop turn), that a job tab
 * changed from agentctl_jobs_gen() and that a log grew from
 * agentctl_cards_gen().
 *
 *   server thread                         main thread
 *   ------------------------------------  -------------------------------------
 *   SPAWN → job AWAITING_APPROVAL ──────▶ AGACT_ASK: prompt → agentctl_answer
 *   approved → STARTING (or QUEUED):
 *     posix_spawn the runner
 *   runner HELLO → spec, RUNNING
 *   runner LINE → the job's ring ───────▶ the log (and its hover) redrawn
 *   TURN_DONE → IDLE, waiters answered
 *
 * Every function here is called from the main thread unless it says
 * otherwise.
 */
#ifndef AGENTCTL_H
#define AGENTCTL_H

#include "core/types.h"
#include "agentctl/agentctl_adapter.h"

#include <stdbool.h>

#define AGENTCTL_JOB_ENV     "LIU_AGENT_JOB"
#define AGENTCTL_SOCK_ENV    "LIU_AGENT_SOCK"

#define AGENTCTL_MAX_JOBS     32
#define AGENTCTL_MAX_RUNNING  8

typedef struct {
    bool enabled;         /* Settings › Terminal › Agents start agents */
    i32  max_running;     /* jobs with a turn in flight; the rest queue */
    i32  perm_ceiling;    /* AgentctlPerm: the most a spawn may ask for */
    bool guard;           /* an edit of a path another member owns is asked about first */
    bool ask_tools;       /* a tool a job's level refuses is put to the user (agents that can) */
    bool keep;            /* a turn running when Liu quits is finished, and the job comes back */
} AgentctlPolicy;

/* The runner binary (liu-notify, which takes the runner's role as
 * `liu-agent run`). Set before agentctl_start. */
void agentctl_set_runner(const char *path);
/* The folder that holds the `liu-agent` command, put first on a delegated
 * agent's PATH so it can read and write its team. Before agentctl_start. */
void agentctl_set_helpers(const char *dir);

/* Opens the socket in Liu's private run folder and starts the thread.
 * Jobs an earlier Liu left (it quit or crashed while they had a session)
 * are taken over: idle, or still finishing a turn, and owned by nobody until
 * agentctl_pane_identity names the pane whose agent leads them.
 * `wake_main` must be callable from any thread (platform_wake_event_loop).
 * New local panes learn the socket's path (session_local_set_agent_sock). The
 * environment runners start with is this process's at this moment. */
bool agentctl_start(void (*wake_main)(void));
/* Stops the thread, closes every connection, ends every runner, removes the
 * socket and the jobs' files. */
void agentctl_stop(void);
/* The exit-path form: removes the socket node and the jobs' files without
 * joining the thread (Cmd+Q ends in exit(), never in a join). Runners see the
 * socket close and end their agent. Idempotent. */
void agentctl_exit_cleanup(void);

const char *agentctl_socket_path(void);
/* Whether another Liu of this user's is serving agent control now: a
 * liu-agent-<pid>.sock beside this one whose pid is a live process named Liu.
 * Files that outlive one instance (the skills) stay while it is true. */
bool agentctl_other_live(void);
void agentctl_set_policy(const AgentctlPolicy *p);

typedef enum {
    AGACT_NONE = 0,
    AGACT_ASK,          /* raise the approval prompt in the owner's window */
    AGACT_DISMISS,      /* the request was withdrawn: take its prompt down */
    AGACT_FOCUS,        /* open the job's log in the owner's window */
    AGACT_NOTICE,       /* tell the user in the owner's window: `preview` holds the text
                         * (two members of a team changed one file) */
    AGACT_ENDED,        /* a turn of the job ended: `preview` says how, `outcome` which way */
    AGACT_TOOL_ASK,     /* the job waits for the user to allow a tool: `preview` says which */
    AGACT_STARTED,      /* a job started without asking (the tab was trusted): `preview` says which */
} AgentctlActionKind;

#define AGENTCTL_PREVIEW_CAP 4096

typedef struct {
    AgentctlActionKind kind;
    u32  job;
    u64  owner_serial;      /* the pane that asked */
    i32  agent;             /* AgentctlAgent */
    i32  perm;              /* AgentctlPerm */
    char model[AGENTCTL_MODEL_CAP];
    char effort[AGENTCTL_EFFORT_CAP];
    char cwd[1024];
    char label[AGENTCTL_LABEL_CAP];
    char preview[AGENTCTL_PREVIEW_CAP];   /* the task's head as sent (not sanitized) */
    u32  prompt_len;
    u8   outcome;                         /* AGACT_ENDED */
    u32  review_of;                       /* the job a request reviews, 0 = none */
    bool isolate;                         /* the request works in a worktree of its own */
} AgentctlAction;

/* Something waits for the main thread. One atomic load. Any thread. */
bool agentctl_main_pending(void);
/* The next piece of main-thread work, or false when there is none (which
 * also clears the pending flag). */
bool agentctl_take_action(AgentctlAction *out);

/* The request of a job still waiting for the user's answer, as AGACT_ASK
 * carried it; false once it has been answered or withdrawn. */
bool agentctl_job_describe(u32 job, AgentctlAction *out);

/* The user's answer to AGACT_ASK. `owner_label` names the asking pane for the
 * job's log ("Claude Code (tab 2)"). */
void agentctl_answer(u32 job, bool approved, const char *owner_label);
/* The same, and with `trust_pane` the pane that asked is trusted until it
 * closes: its requests up to this one's permission level start without
 * asking, those waiting now included (each says so with AGACT_STARTED). */
void agentctl_answer_ex(u32 job, bool approved, const char *owner_label, bool trust_pane);
/* Whether `pane` is trusted to start agents without asking, and up to which
 * level (AgentctlPerm, -1 = not trusted). Any thread. */
i32  agentctl_pane_trust(u64 pane);
void agentctl_pane_untrust(u64 pane);

/* The user's word on a tool a job asked for (AGACT_TOOL_ASK): "allow",
 * "always" (that kind of call, for the rest of the job) or "deny". False
 * when the job asks for nothing now. */
bool agentctl_tool_answer(u32 job, const char *decision);

/* An isolated job's work merged into its folder (`merge`) or thrown away;
 * either ends the job once done. False with `why` when the job is not idle
 * or has no worktree. */
bool agentctl_request_merge(u32 job, bool merge, char *why, usize why_cap);

/* A job the user starts from Liu itself (the palette, a job's chat): no
 * prompt is raised, the user just asked for it. Returns the job's id, 0 with
 * `why` filled when it is refused. */
typedef struct {
    u64         owner_serial;   /* the local pane it belongs to (its team's lead) */
    i32         agent, perm;
    const char *model, *effort, *cwd, *label;
    const char *prompt;
    usize       prompt_len;
    u32         review_of;      /* a review of this job's work: its task and result head the prompt */
    bool        isolate;
} AgentctlUserSpawn;
u32  agentctl_user_spawn(const AgentctlUserSpawn *u, char *why, usize why_cap);

/* Main thread: the agent in local pane `pane` runs on its own session
 * `session` (its id as the agent's store names it; "" = none known). A job
 * belongs to its lead's session: after a restart, the jobs an earlier Liu
 * left behind join the pane whose agent resumed that session. */
void agentctl_pane_identity(u64 pane, const char *session);
/* Where Liu keeps what outlives a launch (its config folder): the list of
 * sessions delegated agents ran, for Agent History. Before agentctl_start. */
void agentctl_set_state_dir(const char *dir);
/* Whether `session` of an agent was a delegated job's, and if so its label
 * and who asked ("Codex · backend for Claude Code (tab 2)"). Reads the list
 * agentctl_set_state_dir names; any thread. */
bool agentctl_session_delegated(const char *dir, const char *session, char *out, usize cap);
/* A pane closed: its jobs end, a request from it is withdrawn. Any thread. */
void agentctl_pane_gone(u64 pane_serial);
/* Main thread: the agent in local pane `pane` started or stopped working (the
 * tab's own reading of it). A team's lead whose agent reports no tools is
 * credited with the changes no job made while it worked. */
void agentctl_pane_busy(u64 pane, bool busy);
/* Main thread: whether local pane `pane` leads a team (no lock taken). */
bool agentctl_pane_leads(u64 pane);
/* Main thread: what a team lead's pane shows now (its visible rows, one a
 * line). The file names on it, as they appear anew, tell the lead's changes
 * from the others' (agentctl_watch.h). */
void agentctl_lead_screen(u64 pane, const char *text, usize len);

/* What the user does from the log: stop the running turn, end the job. */
void agentctl_request_stop(u32 job);
void agentctl_request_close(u32 job);
/* The user's own next message to an idle job, typed into its chat; the turn
 * runs as one the asking agent sent would (its `wait` sees it). Leading and
 * trailing blanks are dropped. BUSY while a turn runs or before the first
 * one (the caller may try again when the job changes); REFUSED, with `why`
 * filled, when the job has ended or the message is empty. Any thread. */
typedef enum {
    AGENTCTL_SENT = 0,
    AGENTCTL_SEND_BUSY,
    AGENTCTL_SEND_REFUSED,
} AgentctlSendResult;
AgentctlSendResult agentctl_request_send(u32 job, const char *text, usize len, char *why, usize why_cap);

/* Hands an idle job's session to the user: the job ends ("taken over") and
 * `out` says how to open the session in the agent's own interface. False when
 * the job is not idle or has no session yet. */
typedef struct {
    i32  agent, perm;
    char model[AGENTCTL_MODEL_CAP];
    char effort[AGENTCTL_EFFORT_CAP];
    char session[AGENTCTL_SESSION_CAP];
    char cwd[1024];
    u64  owner_serial;
} AgentctlTakeover;
bool agentctl_takeover(u32 job, AgentctlTakeover *out);

/* ---- what the job tabs and the log show --------------------------------- */

/* Whether a window shows a job's lines (its chat, a job tab's hover, the team
 * panel): only then does a new line wake the main loop. Main thread, each
 * loop turn it changes. */
void agentctl_set_log_watched(bool on);

/* Changes whenever a job's state or lines change. Any thread. */
u64  agentctl_cards_gen(void);
/* Changes when a job appears, goes or changes state, and not for a new line
 * of its log: what a job tab shows. Any thread. */
u64  agentctl_jobs_gen(void);
/* Whether any job exists at all: the tab strip asks this first. */
bool agentctl_any_jobs(void);

/* One job as its tab shows it. */
typedef struct {
    u32  job;
    u64  owner_serial;
    i32  agent;
    u8   state, outcome, end;
    char label[AGENTCTL_LABEL_CAP];
    i64  turn_start_ms, turn_end_ms;   /* wall clock */
    bool asking;                       /* it waits for the user to allow a tool */
    bool isolate;                      /* it works in a worktree of its own */
} AgentctlJobBrief;

/* Every job that is shown (not declined, closed or taken over), oldest
 * first, at most `cap`. One lock for all of them. */
i32 agentctl_jobs_brief(AgentctlJobBrief *out, i32 cap);

#define AGENTCTL_CARD_LINES    6      /* the hover's screen: its rows, the cursor's among them */
#define AGENTCTL_CARD_LINE_CAP 192

typedef struct {
    u8   style;                         /* AgentctlStyle */
    char text[AGENTCTL_CARD_LINE_CAP];
} AgentctlCardLine;

typedef struct {
    u32  job;
    i32  agent;
    u8   state, outcome, end;
    char label[AGENTCTL_LABEL_CAP];
    i64  created_ms, turn_start_ms, turn_end_ms;   /* wall clock */
    i32  nlines;
    AgentctlCardLine lines[AGENTCTL_CARD_LINES];   /* the newest last */
} AgentctlCard;

/* The jobs `owner` started, oldest first, at most `cap`. */
i32 agentctl_owner_cards(u64 owner, AgentctlCard *out, i32 cap);
/* One shown job with its newest lines (the job tab's hover). */
bool agentctl_job_card(u32 job, AgentctlCard *out);

/* A job's details and its whole log, copied out for the log panel (the
 * panel draws from the copy and takes a new one when agentctl_cards_gen
 * changes). Lines are already sanitized. */
typedef struct {
    u8  style;
    u32 off, len;    /* into `bytes` */
} AgentctlSnapLine;

typedef struct {
    u32  job;
    i32  agent, perm;
    u8   state, outcome, end;
    char model[AGENTCTL_MODEL_CAP];
    char effort[AGENTCTL_EFFORT_CAP];
    char cwd[1024];
    char label[AGENTCTL_LABEL_CAP];
    char owner_label[96];
    char session[AGENTCTL_SESSION_CAP];
    char error[512];
    char usage[128];
    u32  turns;
    i64  turn_start_ms, turn_end_ms;
    u64  owner_serial;
    u32  dropped;               /* lines that fell off the front of the ring */
    i32  nlines;
    AgentctlSnapLine *lines;
    char *bytes;
    char team[256];                    /* the other jobs of its team: "Codex · tests (job 3)", "" for none */
    /* what it spent and may spend */
    u64  spent_in, spent_cached, spent_out, spent_cost_micro;
    u32  max_turns, deadline_s;
    u64  max_tokens, budget_micro;
    /* an isolated job */
    char worktree[1024];
    char branch[128];
    char merged[600];                  /* what merging or discarding did, "" = nothing yet */
    /* a tool it waits for the user to allow, "" = none */
    char asking[700];
    u32  review_of;
    u32  after[4];                     /* jobs it starts after; 0 = none */
    i32  nfiles;                       /* files it changed */
    i32  nclashes;
    bool restored;                     /* taken over from an earlier Liu */
    char dir[512];                     /* its private folder: result.md, events.log */
    bool watched;                      /* a subagent its own agent runs: Liu only shows it */
    char kind[64];                     /* a watched subagent's type ("explore", "general-purpose"), "" if none */
} AgentctlSnapshot;

bool agentctl_job_snapshot(u32 job, AgentctlSnapshot *out);
void agentctl_snapshot_free(AgentctlSnapshot *s);

/* ---- subagents the agent in a pane started itself ------------------------
 *
 * An agent CLI that runs subagents of its own (Claude Code's Agent tool,
 * Codex's spawned agents, OpenCode's child sessions…) records each in its own
 * store; agentctl_subagents.h reads them and shows each as a job tab beside
 * the pane's tab, the way a delegated job is shown. Liu only watches them:
 * their chat is read-only, and nothing here can stop or message them. Their
 * ids carry AGENTCTL_WATCHED_BIT, and the functions above hand them to the
 * view registered here, so the job tabs, their hover, the chat and the docked
 * list read both kinds through one door. */
#define AGENTCTL_WATCHED_BIT  0x40000000u
#define AGENTCTL_MAX_WATCHED  32
/* Every job a window may list at once: delegated and watched. */
#define AGENTCTL_MAX_SHOWN    (AGENTCTL_MAX_JOBS + AGENTCTL_MAX_WATCHED)
/* A watched subagent's `agent`: its agent's ChatTool past this base, so it
 * never reads as one of the delegated agents (AgentctlAgent). */
#define AGENTCTL_AGENT_TOOL_BASE 64

static inline bool agentctl_job_watched(u32 job) { return (job & AGENTCTL_WATCHED_BIT) != 0; }
static inline bool agentctl_agent_is_tool(i32 agent) {
    return agent >= AGENTCTL_AGENT_TOOL_BASE && agent < AGENTCTL_AGENT_TOOL_BASE + 64;
}

typedef struct {
    u64  (*jobs_gen)(void);
    u64  (*cards_gen)(void);
    bool (*any)(void);
    i32  (*brief)(AgentctlJobBrief *out, i32 cap);
    bool (*card)(u32 job, AgentctlCard *out);
    i32  (*owner_cards)(u64 owner, AgentctlCard *out, i32 cap);
    bool (*snapshot)(u32 job, AgentctlSnapshot *out);
    void (*dismiss)(u32 job);
    void (*log_watched)(bool on);
} AgentctlWatchedView;

/* Where the watched subagents come from; set once, before the first frame.
 * NULL (the default) shows none. */
void agentctl_set_watched_view(const AgentctlWatchedView *v);

#endif /* AGENTCTL_H */
