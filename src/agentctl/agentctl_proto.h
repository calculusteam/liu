/*
 * Liu — the agent-control wire format.
 *
 * An agent running in a Liu pane asks Liu to start another agent in a tab of
 * its own, waits for it, reads what it answered and sends it the next message
 * (`liu-agent`, agentctl/client.c). The agent started that way is driven by a
 * small runner with no terminal (agentctl/runner.c) that reports each turn,
 * and each line of it, back.
 * Both talk to the agentctl server inside Liu (agentctl_server.c) over one
 * AF_UNIX stream socket per Liu process.
 *
 * A frame is a fixed 16-byte header and a payload of fields:
 *
 *   header  u32 magic ("LIUA"), u16 version, u16 op, u32 job, u32 length
 *   field   u16 key, u32 length, bytes        (repeated, at most 64)
 *
 * Integers are in host order: both ends are on the same machine and the
 * socket never leaves it. A string field holds no NUL; a number is written as
 * decimal text, so a field is always printable for a log. Keys a reader does
 * not know are skipped, so new fields are additive. No JSON on the wire: the
 * validator is small enough to fuzz on its own.
 */
#ifndef AGENTCTL_PROTO_H
#define AGENTCTL_PROTO_H

#include "core/types.h"

#include <stdbool.h>
#include <stddef.h>

#define AGENTCTL_MAGIC        0x4155494Cu          /* 'L' 'I' 'U' 'A' in memory order */
#define AGENTCTL_VERSION      1u
#define AGENTCTL_HEADER_SIZE  16u
#define AGENTCTL_FIELD_HEAD   6u
#define AGENTCTL_MAX_FIELDS   64u
/* A prompt is the largest field; the rest of a frame is a few short strings. */
#define AGENTCTL_MAX_PROMPT   (256u * 1024u)
#define AGENTCTL_MAX_PAYLOAD  (AGENTCTL_MAX_PROMPT + 16u * 1024u)

typedef enum {
    AGOP_NONE = 0,

    /* liu-agent -> Liu */
    AGOP_SPAWN  = 1,   /* start an agent in a new tab (asks the user first) */
    AGOP_WAIT   = 2,   /* reply once the listed jobs settle */
    AGOP_SEND   = 3,   /* the next message for an idle job */
    AGOP_STATUS = 4,   /* one job's state */
    AGOP_LIST   = 5,   /* every job this pane started */
    AGOP_STOP   = 6,   /* interrupt the running turn */
    AGOP_CLOSE  = 7,   /* end the job and close its tab */
    AGOP_FOCUS  = 8,   /* bring the job's tab to the front */
    AGOP_AGENTS = 9,   /* which agents can be started, and the limits */
    /* the team (agentctl_team.h): from the lead's pane or from a job's agent */
    AGOP_TEAM   = 10,  /* who is in the team, what each does, the newest posts */
    AGOP_POST   = 11,  /* a post to the team: AGK_TEXT, AGK_TO */
    AGOP_INBOX  = 12,  /* the posts for the caller it has not read; AGK_BLOCK waits for one */
    AGOP_OWNS   = 13,  /* the paths the caller calls its own: AGK_PATH, repeated */
    AGOP_LEAD_TOOL = 14, /* the pane's agent runs a tool (its hooks): AGK_STATE "1" before / "0" after,
                          * AGK_TEXT its call id, AGK_PROMPT its command, AGK_PATH the files it names, AGK_CWD.
                          * The reply may carry AGK_DECISION ("ask" / "deny", with AGK_TEXT why) before a
                          * tool runs and AGK_TEAM (what the team did since the last one) after it. */
    AGOP_MERGE   = 15,  /* an isolated job's work into the folder it was started from */
    AGOP_DISCARD = 16,  /* an isolated job's work thrown away */

    /* runner -> Liu */
    AGOP_R_HELLO      = 32,   /* the runner of `job` is up: send the spec */
    AGOP_R_TURN_START = 33,
    AGOP_R_TURN_DONE  = 34,
    AGOP_R_TURN_FAIL  = 35,
    AGOP_R_TAKEOVER   = 36,   /* the user took the session over */
    AGOP_R_LINE       = 37,   /* one line of the job's log: AGK_STYLE + AGK_TEXT */
    AGOP_R_FILE       = 38,   /* the agent changed the file AGK_PATH names (AGK_SEEN: as seen on disk) */
    AGOP_R_POST       = 39,   /* the agent posted to its team through its outbox: AGK_TO, AGK_TEXT */
    AGOP_R_CMD        = 40,   /* a command of the agent's began (AGK_STATE "1") or ended ("0"): AGK_TEXT its id, AGK_PROMPT its line */
    AGOP_R_ASK        = 41,   /* the agent asks to use a tool: AGK_REQ its id, AGK_TOOL, AGK_TEXT what it would do;
                               * AGK_STATE "0" withdraws it */
    AGOP_R_WORKTREE   = 42,   /* an isolated job's worktree is ready: AGK_WORKTREE, AGK_BRANCH, AGK_BASE */
    AGOP_R_MERGED     = 43,   /* a merge or discard ended: AGK_CODE, AGK_TEXT what happened */

    /* Liu -> runner */
    AGOP_S_SPEC  = 48,
    AGOP_S_TURN  = 49,        /* run another turn with AGK_PROMPT (and AGK_TEAM, its team notes) */
    AGOP_S_STOP  = 50,
    AGOP_S_CLOSE = 51,
    AGOP_S_ANSWER = 52,       /* the answer to an AGOP_R_ASK: AGK_REQ, AGK_DECISION "allow" / "always" / "deny" */
    AGOP_S_MERGE  = 53,       /* AGK_DECISION "merge" or "discard" the isolated job's work */

    /* Liu -> liu-agent */
    AGOP_REPLY = 64,
} AgentctlOp;

typedef enum {
    AGK_CODE       = 1,    /* AgentctlCode */
    AGK_TEXT       = 2,    /* a sentence for a person */
    AGK_AGENT      = 3,    /* "codex", "claude" */
    AGK_MODEL      = 4,
    AGK_EFFORT     = 5,
    AGK_PERMISSION = 6,    /* "read", "write", "full" */
    AGK_CWD        = 7,
    AGK_LABEL      = 8,
    AGK_PROMPT     = 9,
    AGK_STATE      = 10,   /* agentctl_state_name() */
    AGK_OUTCOME    = 11,   /* agentctl_outcome_name() */
    AGK_ERROR      = 12,
    AGK_SESSION    = 13,   /* the agent's own session id */
    AGK_DIR        = 14,   /* the job's private folder (result.md, events.log) */
    AGK_USAGE      = 15,   /* token counts, as the runner summarised them */
    AGK_TURNS      = 16,
    AGK_ELAPSED_MS = 17,   /* the last turn's length */
    AGK_JOB        = 18,   /* a job id; repeated in AGOP_WAIT */
    AGK_ANY        = 19,   /* "1": AGOP_WAIT settles on the first job */
    AGK_OWNER      = 20,   /* who asked: "Claude (tab 2)" */
    AGK_LINE       = 21,   /* one line of a listing; repeated */
    AGK_STOPPED    = 22,   /* "1": the failed turn was interrupted on request */
    AGK_END        = 23,   /* why an ended job ended */
    AGK_STYLE      = 24,   /* AgentctlStyle of an AGOP_R_LINE */
    AGK_TO         = 25,   /* who a post is for: "all", "lead", a job id */
    AGK_PATH       = 26,   /* a path; repeated in AGOP_OWNS and AGOP_SPAWN (--owns) */
    AGK_TEAM       = 27,   /* the team as text: a listing, posts, a turn's team notes */
    AGK_BLOCK      = 28,   /* "1": AGOP_INBOX waits until there is a post */
    AGK_SEEN       = 29,   /* "1": AGOP_R_FILE's file was seen changing while a command ran, not named by a tool */
    AGK_ISOLATE    = 30,   /* "1": the job works in a worktree of its own */
    AGK_AFTER      = 31,   /* a job id this one starts after; repeated */
    AGK_MAX_TURNS  = 32,   /* turns the job may run, 0 = no limit */
    AGK_MAX_TOKENS = 33,   /* input + output tokens the job may spend, 0 = no limit */
    AGK_BUDGET     = 34,   /* dollars the job may spend, in millionths, 0 = no limit */
    AGK_DEADLINE   = 35,   /* seconds one turn may take, 0 = no limit */
    AGK_TOKENS     = 36,   /* "in cached out": a turn's token counts */
    AGK_COST       = 37,   /* a turn's cost in millionths of a dollar */
    AGK_WORKTREE   = 38,   /* an isolated job's folder */
    AGK_BRANCH     = 39,   /* the branch its work is on */
    AGK_BASE       = 40,   /* the commit its work is measured from */
    AGK_REVIEW     = 41,   /* the job a new job reviews */
    AGK_DECISION   = 42,
    AGK_REQ        = 43,   /* a tool request's id */
    AGK_TOOL       = 44,   /* a tool's name */
    AGK_FILES      = 45,   /* the files a job changed, one a line, absolute */
    AGK_CLASHES    = 46,   /* the clashes a job was in, one a line */
    AGK_SPEND      = 47,   /* "in cached out cost_micro turns": what a job has spent so far */
    AGK_LIMITS     = 48,   /* "turns tokens budget_micro deadline_s": what it may spend */
    AGK_ASK_TOOLS  = 49,   /* "1": a tool its level does not allow is put to the user */
    AGK_KEEP       = 50,   /* "1": a turn running when Liu quits is finished */
    AGK_HELPERS    = 51,   /* the folder holding `liu-agent`, for the agent's hooks */
    AGK_ASKING     = 52,   /* what a job waits for the user to allow, "" = nothing */
    AGK_FROM       = 53,   /* the job whose work a new job continues */
    AGK_MERGED     = 54,   /* what merging or discarding an isolated job did */
    AGK_OWNER_KEY  = 55,   /* the lead's own agent session: who a job belongs to after a restart */
} AgentctlKey;

typedef enum {
    AGC_OK       = 0,
    AGC_PENDING  = 1,   /* the job exists and waits for the user's answer */
    AGC_REFUSED  = 2,   /* not allowed: setting, permission ceiling, depth, owner */
    AGC_DECLINED = 3,   /* the user said no */
    AGC_NOTFOUND = 4,
    AGC_BUSY     = 5,   /* the job is in the middle of something */
    AGC_ERROR    = 6,
} AgentctlCode;

/* A validated frame. `payload` points into the caller's buffer. */
typedef struct {
    u16       op;
    u32       job;
    const u8 *payload;
    u32       len;
} AgentctlFrame;

/* The header's fields, once its magic, version and length are acceptable. */
bool agentctl_header_parse(const u8 *hdr, u16 *op, u32 *job, u32 *len);

/* The payload is a whole number of fields, at most AGENTCTL_MAX_FIELDS. */
bool agentctl_payload_valid(const u8 *payload, u32 len);

/* A whole frame (header + payload) in one buffer. Pure; the fuzz entry. */
bool agentctl_validate(const u8 *buf, usize len, AgentctlFrame *out);

/* The first field `key`. False when absent. */
bool agentctl_get(const AgentctlFrame *f, u16 key, const u8 **data, u32 *len);

/* The first field `key` as a NUL-terminated string. False (and "") when it is
 * absent, holds a NUL or does not fit: a string cut to fit would be another,
 * valid-looking value. */
bool agentctl_get_str(const AgentctlFrame *f, u16 key, char *out, usize cap);

/* The first field `key` as a decimal u64; false when absent or not a number. */
bool agentctl_get_u64(const AgentctlFrame *f, u16 key, u64 *out);

/* Walks every field in order: start with *pos = 0. */
bool agentctl_next(const AgentctlFrame *f, u32 *pos, u16 *key, const u8 **data, u32 *len);

/* A frame being written. Grows on the heap up to the protocol's limit; any
 * field that would pass it marks the message failed instead of cutting it. */
typedef struct {
    u8  *buf;
    u32  len, cap;
    bool failed;
} AgentctlMsg;

bool agentctl_msg_init(AgentctlMsg *m, u16 op, u32 job);
void agentctl_msg_put(AgentctlMsg *m, u16 key, const void *data, u32 len);
void agentctl_msg_put_str(AgentctlMsg *m, u16 key, const char *s);
void agentctl_msg_put_u64(AgentctlMsg *m, u16 key, u64 v);
/* Writes the payload length into the header. False when the message failed. */
bool agentctl_msg_end(AgentctlMsg *m);
void agentctl_msg_free(AgentctlMsg *m);

/* Reads frames off a non-blocking descriptor a piece at a time. */
typedef struct {
    u8   hdr[AGENTCTL_HEADER_SIZE];
    u32  hdr_have;
    u8  *payload;      /* NULL until the header has been read */
    u32  need, have;
    u16  op;
    u32  job;
} AgentctlReader;

/* 1 = a frame is complete (agentctl_reader_frame, then agentctl_reader_reset);
 * 0 = more bytes are needed (EAGAIN); -1 = end of stream, a read error or a
 * frame the protocol refuses. */
int  agentctl_reader_feed(AgentctlReader *r, int fd);
void agentctl_reader_frame(const AgentctlReader *r, AgentctlFrame *out);
void agentctl_reader_reset(AgentctlReader *r);

/* Blocking helpers for the command-line ends. `timeout_ms` < 0 waits for ever.
 * agentctl_read_frame returns 1 with a frame (its payload in *storage, which
 * the caller frees), 0 on timeout and -1 on end of stream or error. */
bool agentctl_write_all(int fd, const u8 *buf, usize len);
int  agentctl_read_frame(int fd, i32 timeout_ms, AgentctlFrame *out, u8 **storage);

#endif /* AGENTCTL_PROTO_H */
