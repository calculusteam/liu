/*
 * Liu — the agent-control server: socket, job table, connections, runners.
 * See agentctl.h for the shape and agentctl_proto.h for the frames.
 *
 * One thread, one poll() over the wake pipe, the listener and at most
 * AGENTCTL_MAX_CONNS connections. A connection is a `liu-agent` command (it
 * asks and, for a wait, stays until it is answered) or a job's runner (it
 * stays for the job's life). The thread also starts each runner and collects
 * it when it exits; only while one is starting or exiting does poll() get a
 * timeout. All socket I/O and every runner process are here; the job table is
 * shared with the main thread under one mutex, held only for memory work —
 * never across a read, a write or a spawn.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "agentctl/agentctl.h"
#include "agentctl/agentctl_auth.h"
#include "agentctl/agentctl_delegated.h"
#include "agentctl/agentctl_proto.h"
#include "agentctl/agentctl_ring.h"
#include "agentctl/agentctl_stream.h"
#include "agentctl/agentctl_team.h"
#include "agentctl/agentctl_watch.h"
#include "core/private_tmp.h"
#include "notify/socket.h"
#include "ssh/ssh_session.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#endif

extern char **environ;

#define AGENTCTL_MAX_CONNS 64
#define AGENTCTL_MAX_WAIT  16
#define AGT_AFTER_MAX      4    /* jobs one job may start after */
#define AGT_WATCH_TAIL_MS  5000 /* a folder is watched this long after its job's turn, or its lead's work, ends */
#define AGT_JOB_CLASHES    8    /* clashes remembered per job */

/* A peer that went away must not raise SIGPIPE in Liu. */
#ifdef MSG_NOSIGNAL
#define AGENTCTL_SEND_FLAGS MSG_NOSIGNAL
#else
#define AGENTCTL_SEND_FLAGS 0
#endif

/* How long a runner may take to say hello, and to go once it was told to. */
#define RUNNER_START_MS 15000
#define RUNNER_EXIT_MS  5000
/* An idle job's runner is let go after this long; its next message starts one. */
#define AGT_PARK_MS     (10 * 60 * 1000)

typedef AgentctlRing     Ring;
typedef AgentctlRingLine RingLine;

/* The files a member of a team changed, as absolute paths in the file
 * system's spelling; at most AGT_TOUCHED_MAX, the one changed longest ago
 * going first. */
typedef struct {
    char *path[AGT_TOUCHED_MAX];
    u64   seq[AGT_TOUCHED_MAX];       /* g_touch_seq when it last changed */
    i64   ns[AGT_TOUCHED_MAX];        /* the file's status time then, 0 = gone */
    bool  seen[AGT_TOUCHED_MAX];      /* only seen changing on disk; no tool named it */
    u32   n;
} Changes;

/* A command or tool call of a member, by the wall clock: what the folder
 * watch weighs a change against. A job's come from its runner (AGOP_R_CMD),
 * the lead's from its agent's tool hooks (AGOP_LEAD_TOOL). */
#define AGT_CMD_WINS 16
typedef struct {
    char  id[64];
    i64   start_ns, end_ns;           /* end 0 = still running; start 0 = a free slot */
    char *cmd;                        /* its line, or the files it names; NULL when unknown */
} CmdWin;

typedef struct {
    u32  id;                /* 0 = a free slot */
    u8   state, outcome, end;
    i32  agent, perm;
    char model[AGENTCTL_MODEL_CAP];
    char effort[AGENTCTL_EFFORT_CAP];
    char cwd[1024];
    char label[AGENTCTL_LABEL_CAP];
    char owner_label[96];
    char *prompt;           /* the next turn's message until the runner has it */
    u32  prompt_len;
    char preview[AGENTCTL_PREVIEW_CAP];
    u64  owner_serial;
    i32  runner;            /* connection index of the job's runner, -1 */
    pid_t runner_pid;       /* the runner process, 0 = none yet */
    i64  runner_due_ms;     /* monotonic: the runner must have said hello by then */
    char session[AGENTCTL_SESSION_CAP];
    char error[512];
    char usage[128];
    char dir[512];
    u32  turns;
    i64  created_ms, turn_start_ms, turn_end_ms;
    u64  seq;               /* approval order, for the queue */
    Ring ring;
    /* main-thread work */
    bool ask, dismiss, focus, notice;
    char notice_text[AGT_POST_MAX];   /* a clash to show the user (AGACT_NOTICE) */
    /* runner work */
    bool send_turn, send_stop, send_close;
    /* its team (agentctl_team.h) */
    char  owns[AGT_OWNS_MAX][AGT_PATH_MAX];
    u8    nowns;
    Changes ch;                       /* the files it changed */
    CmdWin  wins[AGT_CMD_WINS];       /* its agent's recent commands */
    u64   board_seen;                 /* the newest post it has been shown */
    u64   touch_seen;                 /* the newest change of another member it has been shown */
    /* how it runs */
    bool  isolate;                    /* in a worktree of its own (made by its runner) */
    bool  ask_tools;                  /* a tool its level refuses is put to the user */
    u32   after[AGT_AFTER_MAX];       /* jobs it starts after */
    u8    nafter;
    u32   review_of;                  /* the job whose work it reviews */
    /* what it may spend, 0 = no limit, and what it spent */
    u32   max_turns, deadline_s;
    u64   max_tokens, budget_micro;
    u64   spent_in, spent_cached, spent_out, spent_cost;
    char  summary[AGT_SUMMARY_MAX + 4];   /* the head of its last final message */
    u64   settle_seq;                 /* g_settle_seq when its last turn ended */
    /* an isolated job's worktree, once its runner made it */
    char  wt[1024], wt_branch[128], wt_base[64], origin_top[1024];
    char  origin_cwd[1024];           /* the folder it was started in */
    bool  merge_req, merging;
    char  merge_dec[12];
    char  merged[600];
    /* a tool it waits for the user to allow */
    bool  asking;
    char  ask_req[136], ask_tool[64], ask_what[700];
    bool  answer_req;
    char  answer_dec[12];
    /* the clashes it was in, the newest last */
    char  clashes[AGT_JOB_CLASHES][240];
    u8    nclash;
    /* main-thread work */
    bool  ended_notice, ask_notice, started_notice;
    /* its lead's own agent session: who it belongs to after a restart */
    char  owner_key[AGENTCTL_SESSION_CAP];
    bool  restored;                   /* taken over from an earlier Liu */
    pid_t detached_pid;               /* that Liu's runner, still finishing a turn */
    bool  meta_dirty;                 /* job.meta is behind */
    bool  parked;                     /* idle so long its runner was let go; one starts with its next message */
    bool  session_listed;             /* its session is in the delegated list */
} Job;

typedef struct {
    int  fd;                /* -1 = free */
    u64  pane;              /* the pane the peer runs in, 0 for a runner */
    i32  peer_pid;
    bool runner;            /* a runner that has said hello */
    u32  runner_job;
    AgentctlReader rd;
    u8  *out;
    u32  out_len, out_off, out_cap;
    bool close_after_flush;
    u8   wait_kind;         /* WAIT_NONE / WAIT_APPROVAL / WAIT_SETTLE / WAIT_INBOX */
    u32  wait_jobs[AGENTCTL_MAX_WAIT];
    u8   wait_count;
    bool wait_any;
    /* A job's agent (a process below its runner): it may only read and write
     * its team. 0 for a pane's process and a runner. */
    u32  member_job;
    u64  inbox_owner;       /* WAIT_INBOX: the team, and who in it waits */
    u32  inbox_me;
} Conn;

enum { WAIT_NONE = 0, WAIT_APPROVAL, WAIT_SETTLE, WAIT_INBOX };

/* Runners that were told to go (or went) and have not been collected. */
typedef struct { pid_t pid; i64 due_ms; bool killed; } Reap;
#define REAP_CAP 64

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static Job      g_jobs[AGENTCTL_MAX_JOBS];
static u32      g_next_id = 1;
static u64      g_next_seq = 1;
static AgentctlPolicy g_policy = { true, 3, AGENTCTL_PERM_WRITE, true, true, true };
static u64      g_settle_seq;                 /* turns ended so far */

/* Panes the user trusted to start agents without asking, and up to which
 * level (agentctl_answer_ex). */
#define AGT_TRUST_MAX 16
static struct { u64 pane; i32 perm; } g_trust[AGT_TRUST_MAX];

/* Each local pane's agent session as the main thread knows it
 * (agentctl_pane_identity): what a job is filed under for a restart. */
#define AGT_IDENT_MAX 64
static struct { u64 pane; char session[AGENTCTL_SESSION_CAP]; } g_ident[AGT_IDENT_MAX];

static char g_state_dir[1024];                /* Liu's config folder: the delegated list */
static volatile bool g_exiting;               /* the exit path ran: panes going now are the quit's */
static Reap     g_reap[REAP_CAP];
static i32      g_reap_count = 0;

static atomic_bool g_main_flag;
static atomic_bool g_log_watched;             /* a window shows a log or a hover: lines wake the main loop */
static atomic_ullong g_gen;
static atomic_ullong g_state_gen;
static atomic_int  g_job_count;
static void (*g_wake_main)(void);

static Conn       g_conns[AGENTCTL_MAX_CONNS];   /* server thread only */
static int        g_listen = -1;
static int        g_wake[2] = { -1, -1 };
static pthread_t  g_thread;
static bool       g_running = false;
static atomic_bool g_stop;
static char       g_sock_path[256];
static char       g_jobs_root[512];
static char       g_runner_bin[1024];
static char       g_helpers[1024];            /* where `liu-agent` is, for the agents' PATH */
static char     **g_runner_env = NULL;      /* this process's environment at start */
static pid_t      g_owner_pid = 0;

static i64 now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static i64 mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int fd_fl = fcntl(fd, F_GETFD, 0);
    if (fd_fl >= 0) fcntl(fd, F_SETFD, fd_fl | FD_CLOEXEC);
}

static void kick_server(void) {
    if (g_wake[1] < 0) return;
    u8 b = 1;
    (void)!write(g_wake[1], &b, 1);
}

static void wake_main(void) {
    atomic_store_explicit(&g_main_flag, true, memory_order_release);
    if (g_wake_main) g_wake_main();
}

/* Under g_mu: a job appeared, went or changed state (which also changes
 * what its log shows). */
static void bump_gen(void) {
    atomic_fetch_add_explicit(&g_state_gen, 1, memory_order_release);
    atomic_fetch_add_explicit(&g_gen, 1, memory_order_release);
}

/* Under g_mu: a job's log grew; its state is what it was. */
static void bump_line_gen(void) {
    atomic_fetch_add_explicit(&g_gen, 1, memory_order_release);
}

/* ------------------------------------------------------------ the tree --- */

static bool proc_parent(i32 pid, i32 *ppid, void *ctx) {
    (void)ctx;
#if defined(__APPLE__)
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi) return false;
    *ppid = (i32)bi.pbi_ppid;
    return true;
#elif defined(__linux__)
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    /* "pid (comm) state ppid …": comm may hold spaces and parentheses, so the
     * fields are counted from the LAST ')'. */
    char *rp = strrchr(buf, ')');
    if (!rp) return false;
    char state;
    int pp;
    if (sscanf(rp + 1, " %c %d", &state, &pp) != 2) return false;
    *ppid = pp;
    return true;
#else
    (void)pid; (void)ppid;
    return false;
#endif
}

/* --------------------------------------------------------------- files --- */

static void remove_job_dir(const char *dir) {
    if (!dir || !dir[0]) return;
    static const char *const names[] = { "result.md", "events.log", "result.md.tmp", "team.md", "team.md.tmp",
                                         AGT_OUTBOX_NAME, "turn.state", "turn.state.tmp", "job.meta",
                                         "job.meta.tmp", "work.patch" };
    char path[600];
    for (usize i = 0; i < sizeof names / sizeof names[0]; i++) {
        if ((usize)snprintf(path, sizeof path, "%s/%s", dir, names[i]) < sizeof path) unlink(path);
    }
    rmdir(dir);
}

/* ------------------------------------------------- jobs across launches --- */

static Job *job_alloc(void);
static void recount_jobs(void);
static usize result_head(const char *dir, char *out, usize cap);

/* A job's description in its folder (job.meta, key=value a line), rewritten
 * whenever it changes: what the next Liu needs to take the job over when this
 * one quits or crashes while the job has a session. */
static void meta_kv(char *out, usize cap, usize *w, const char *key, const char *val) {
    if (*w >= cap) return;
    char clean[1100];
    agentctl_sanitize(val ? val : "", val ? strlen(val) : 0, clean, sizeof clean, false);
    *w += (usize)snprintf(out + *w, cap - *w, "%s=%s\n", key, clean);
}

static usize job_meta_text(const Job *j, char *out, usize cap) {
    usize w = 0;
    char num[160];
    const AgentctlAgentInfo *ai = agentctl_agent_info(j->agent);
    snprintf(num, sizeof num, "%u", j->id);                  meta_kv(out, cap, &w, "id", num);
    meta_kv(out, cap, &w, "agent", ai ? ai->id : "");
    meta_kv(out, cap, &w, "model", j->model);
    meta_kv(out, cap, &w, "effort", j->effort);
    meta_kv(out, cap, &w, "perm", agentctl_perm_name(j->perm));
    meta_kv(out, cap, &w, "cwd", j->cwd);
    meta_kv(out, cap, &w, "origin", j->origin_cwd);
    meta_kv(out, cap, &w, "label", j->label);
    meta_kv(out, cap, &w, "owner_label", j->owner_label);
    meta_kv(out, cap, &w, "owner_key", j->owner_key);
    meta_kv(out, cap, &w, "session", j->session);
    meta_kv(out, cap, &w, "state", agentctl_state_name(j->state));
    meta_kv(out, cap, &w, "outcome", agentctl_outcome_name(j->outcome));
    meta_kv(out, cap, &w, "summary", j->summary);
    meta_kv(out, cap, &w, "error", j->error);
    meta_kv(out, cap, &w, "worktree", j->wt);
    meta_kv(out, cap, &w, "branch", j->wt_branch);
    meta_kv(out, cap, &w, "base", j->wt_base);
    meta_kv(out, cap, &w, "origin_top", j->origin_top);
    meta_kv(out, cap, &w, "isolate", j->isolate ? "1" : "0");
    meta_kv(out, cap, &w, "ask_tools", j->ask_tools ? "1" : "0");
    snprintf(num, sizeof num, "%u", j->turns);               meta_kv(out, cap, &w, "turns", num);
    snprintf(num, sizeof num, "%llu %llu %llu %llu", (unsigned long long)j->spent_in, (unsigned long long)j->spent_cached,
             (unsigned long long)j->spent_out, (unsigned long long)j->spent_cost);
    meta_kv(out, cap, &w, "spent", num);
    snprintf(num, sizeof num, "%u %llu %llu %u", j->max_turns, (unsigned long long)j->max_tokens,
             (unsigned long long)j->budget_micro, j->deadline_s);
    meta_kv(out, cap, &w, "limits", num);
    snprintf(num, sizeof num, "%lld", (long long)j->created_ms); meta_kv(out, cap, &w, "created", num);
    snprintf(num, sizeof num, "%d", (int)j->runner_pid);      meta_kv(out, cap, &w, "runner", num);
    return w < cap ? w : 0;
}

static void file_replace(const char *path, const char *text, usize len) {
    char tmp[700];
    if ((usize)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    bool ok = true;
    while (len > 0) {
        ssize_t w = write(fd, text, len);
        if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
        text += w;
        len -= (usize)w;
    }
    if (close(fd) != 0) ok = false;
    if (ok) rename(tmp, path);
    else    unlink(tmp);
}

/* key=value text: the value of `key`, "" when absent. */
static void kv_get(const char *text, const char *key, char *out, usize cap) {
    out[0] = '\0';
    usize kl = strlen(key);
    for (const char *line = text; line && *line; ) {
        const char *nl = strchr(line, '\n');
        usize len = nl ? (usize)(nl - line) : strlen(line);
        if (len > kl && !strncmp(line, key, kl) && line[kl] == '=') {
            usize vl = len - kl - 1;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, line + kl + 1, vl);
            out[vl] = '\0';
            return;
        }
        line = nl ? nl + 1 : NULL;
    }
}

static char *read_small(const char *path, usize max) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (usize)st.st_size > max) { close(fd); return NULL; }
    char *buf = malloc((usize)st.st_size + 1);
    if (!buf) { close(fd); return NULL; }
    ssize_t n = read(fd, buf, (usize)st.st_size);
    close(fd);
    if (n < 0) { free(buf); return NULL; }
    buf[n] = '\0';
    return buf;
}

/* Whether `pid` is still a runner (liu-notify, started as liu-agent). */
static bool pid_is_runner(pid_t pid) {
    if (pid <= 1 || kill(pid, 0) != 0) return false;
#if defined(__APPLE__)
    char name[64] = { 0 };
    if (proc_name(pid, name, sizeof name) <= 0) return false;
    return !strcmp(name, "liu-notify") || !strcmp(name, "liu-agent");
#else
    return true;
#endif
}

/* The tail of a job's events.log as its log's first lines. */
static void ring_load_tail(Ring *r, const char *dir) {
    char path[600];
    if ((usize)snprintf(path, sizeof path, "%s/events.log", dir) >= sizeof path) return;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return; }
    usize want = (usize)st.st_size < 64u * 1024u ? (usize)st.st_size : 64u * 1024u;
    char *buf = malloc(want + 1);
    if (!buf) { close(fd); return; }
    ssize_t n = pread(fd, buf, want, st.st_size - (off_t)want);
    close(fd);
    if (n <= 0) { free(buf); return; }
    buf[n] = '\0';
    const char *start = buf;
    if ((usize)st.st_size > want) { const char *nl = strchr(buf, '\n'); start = nl ? nl + 1 : buf; }
    char *clean = malloc((usize)n * 3 + 1);
    if (clean) {
        usize w = agentctl_sanitize(start, strlen(start), clean, (usize)n * 3 + 1, true);
        agentctl_ring_push(r, AGS_DIM, (const u8 *)clean, (u32)w);
        free(clean);
    }
    free(buf);
}

/* A job's running total: held at UINT64_MAX rather than wrapping, so a limit
 * is never passed by an overflow. */
static void spend_add(u64 *to, u64 v) {
    *to = *to > UINT64_MAX - v ? UINT64_MAX : *to + v;
}

/* Under g_mu: the job's last turn as its runner left it (turn.state), after
 * a Liu that quit while it ran. */
static void job_read_turn_state(Job *j) {
    char path[600];
    if ((usize)snprintf(path, sizeof path, "%s/turn.state", j->dir) >= sizeof path) return;
    char *t = read_small(path, 16384);
    if (!t) { j->outcome = AGO_FAILED; snprintf(j->error, sizeof j->error, "its turn ended while Liu was closed"); return; }
    char v[600];
    kv_get(t, "session", v, sizeof v);
    if (v[0] && agentctl_session_ok(v)) snprintf(j->session, sizeof j->session, "%s", v);
    kv_get(t, "outcome", v, sizeof v);
    j->outcome = !strcmp(v, "done") ? AGO_DONE : !strcmp(v, "stopped") ? AGO_STOPPED : AGO_FAILED;
    kv_get(t, "error", j->error, sizeof j->error);
    kv_get(t, "turns", v, sizeof v);
    if (v[0]) j->turns = (u32)strtoul(v, NULL, 10);
    kv_get(t, "tokens", v, sizeof v);
    unsigned long long a = 0, b = 0, c = 0;
    if (sscanf(v, "%llu %llu %llu", &a, &b, &c) == 3) {
        spend_add(&j->spent_in, a); spend_add(&j->spent_cached, b); spend_add(&j->spent_out, c);
    }
    kv_get(t, "cost", v, sizeof v);
    if (v[0]) spend_add(&j->spent_cost, strtoull(v, NULL, 10));
    free(t);
    if (j->outcome == AGO_DONE) {
        char head[AGT_SUMMARY_MAX + 4];
        result_head(j->dir, head, sizeof head);
        snprintf(j->summary, sizeof j->summary, "%s", head);
        for (char *q = j->summary; *q; q++) if (*q == '\n') *q = ' ';
    }
}

/* At start: the jobs of Liu processes that are gone. One that had a session
 * and its lead's (an agent session the lead resumes after a restart) is
 * taken over — idle, or still finishing the turn the old Liu left it in —
 * and owned by nobody until its lead's pane is known; the rest are
 * removed. Under g_mu is not needed: the server thread does not run yet. */
static void adopt_dead_roots(const char *base) {
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char *end = NULL;
        long pid = strtol(e->d_name, &end, 10);
        if (!end || *end || pid <= 1 || pid == (long)getpid()) continue;
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) continue;
        char root[600];
        if ((usize)snprintf(root, sizeof root, "%s/%s", base, e->d_name) >= sizeof root) continue;
        DIR *jd = opendir(root);
        if (jd) {
            struct dirent *je;
            while ((je = readdir(jd)) != NULL) {
                if (je->d_name[0] == '.') continue;
                char old[700], meta_path[760];
                if ((usize)snprintf(old, sizeof old, "%s/%s", root, je->d_name) >= sizeof old) continue;
                snprintf(meta_path, sizeof meta_path, "%s/job.meta", old);
                char *meta = read_small(meta_path, 64 * 1024);
                char v[1100], key[AGENTCTL_SESSION_CAP], st[32];
                bool adopt = false;
                if (meta) {
                    kv_get(meta, "owner_key", key, sizeof key);
                    kv_get(meta, "state", st, sizeof st);
                    kv_get(meta, "session", v, sizeof v);
                    bool running = !strcmp(st, "running") || !strcmp(st, "starting");
                    adopt = key[0] && agentctl_session_ok(key) && (running || (!strcmp(st, "idle") && v[0]));
                }
                Job *j = adopt ? job_alloc() : NULL;
                if (!j) { free(meta); remove_job_dir(old); continue; }
                j->id = g_next_id++;
                char newdir[600];
                snprintf(newdir, sizeof newdir, "%s/%u", g_jobs_root, j->id);
                if (rename(old, newdir) != 0) { memset(j, 0, sizeof *j); j->runner = -1; free(meta); remove_job_dir(old); continue; }
                snprintf(j->dir, sizeof j->dir, "%s", newdir);
                kv_get(meta, "agent", v, sizeof v);       j->agent = agentctl_agent_parse(v);
                kv_get(meta, "perm", v, sizeof v);        j->perm = agentctl_perm_parse(v);
                if (j->agent < 0 || j->perm < 0) { remove_job_dir(newdir); memset(j, 0, sizeof *j); j->runner = -1; free(meta); continue; }
                kv_get(meta, "model", j->model, sizeof j->model);
                if (!agentctl_model_ok(j->model)) j->model[0] = '\0';
                kv_get(meta, "effort", j->effort, sizeof j->effort);
                if (!agentctl_effort_ok(j->effort)) j->effort[0] = '\0';
                kv_get(meta, "cwd", j->cwd, sizeof j->cwd);
                kv_get(meta, "origin", j->origin_cwd, sizeof j->origin_cwd);
                kv_get(meta, "label", j->label, sizeof j->label);
                kv_get(meta, "owner_label", j->owner_label, sizeof j->owner_label);
                snprintf(j->owner_key, sizeof j->owner_key, "%s", key);
                kv_get(meta, "session", v, sizeof v);
                if (agentctl_session_ok(v)) snprintf(j->session, sizeof j->session, "%s", v);
                kv_get(meta, "summary", j->summary, sizeof j->summary);
                kv_get(meta, "error", j->error, sizeof j->error);
                kv_get(meta, "worktree", j->wt, sizeof j->wt);
                kv_get(meta, "branch", j->wt_branch, sizeof j->wt_branch);
                kv_get(meta, "base", j->wt_base, sizeof j->wt_base);
                kv_get(meta, "origin_top", j->origin_top, sizeof j->origin_top);
                kv_get(meta, "isolate", v, sizeof v);     j->isolate = v[0] == '1';
                kv_get(meta, "ask_tools", v, sizeof v);   j->ask_tools = v[0] == '1';
                kv_get(meta, "turns", v, sizeof v);       j->turns = (u32)strtoul(v, NULL, 10);
                kv_get(meta, "spent", v, sizeof v);
                unsigned long long a = 0, b = 0, c2 = 0, dd = 0;
                if (sscanf(v, "%llu %llu %llu %llu", &a, &b, &c2, &dd) == 4) {
                    j->spent_in = a; j->spent_cached = b; j->spent_out = c2; j->spent_cost = dd;
                }
                kv_get(meta, "limits", v, sizeof v);
                unsigned mt = 0, dl = 0;
                if (sscanf(v, "%u %llu %llu %u", &mt, &a, &b, &dl) == 4) {
                    j->max_turns = mt; j->max_tokens = a; j->budget_micro = b; j->deadline_s = dl;
                }
                kv_get(meta, "created", v, sizeof v);     j->created_ms = strtoll(v, NULL, 10);
                kv_get(meta, "runner", v, sizeof v);
                pid_t rp = (pid_t)strtol(v, NULL, 10);
                j->runner = -1;
                j->restored = true;
                j->seq = g_next_seq++;
                ring_load_tail(&j->ring, j->dir);
                const char *head = "\xC2\xB7 taken over from an earlier Liu";
                if ((!strcmp(st, "running") || !strcmp(st, "starting")) && pid_is_runner(rp)) {
                    /* its old runner is still finishing that turn */
                    j->state = AGJ_RUNNING;
                    j->detached_pid = rp;
                    j->turn_start_ms = now_ms();
                    head = "\xC2\xB7 taken over from an earlier Liu; its turn is still finishing";
                } else {
                    if (!strcmp(st, "running") || !strcmp(st, "starting")) job_read_turn_state(j);
                    else { kv_get(meta, "outcome", v, sizeof v);
                           j->outcome = !strcmp(v, "done") ? AGO_DONE : !strcmp(v, "stopped") ? AGO_STOPPED :
                                        !strcmp(v, "failed") ? AGO_FAILED : AGO_NONE; }
                    j->state = AGJ_IDLE;
                    j->turn_end_ms = now_ms();
                }
                agentctl_ring_push(&j->ring, AGS_HEAD, (const u8 *)head, (u32)strlen(head));
                j->meta_dirty = true;
                free(meta);
            }
            closedir(jd);
        }
        rmdir(root);
    }
    closedir(d);
    recount_jobs();
}

/* Server thread: a job whose old runner was finishing a turn when this Liu
 * took it over, once that runner is gone. True when one settled. */
static bool detached_tick(bool *any_left) {
    bool settled = false;
    *any_left = false;
    pthread_mutex_lock(&g_mu);
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        Job *j = &g_jobs[i];
        if (!j->id || !j->detached_pid) continue;
        if (pid_is_runner(j->detached_pid)) { *any_left = true; continue; }
        j->detached_pid = 0;
        job_read_turn_state(j);
        j->state = AGJ_IDLE;
        j->turn_end_ms = now_ms();
        j->settle_seq = ++g_settle_seq;
        j->ended_notice = j->outcome != AGO_STOPPED;
        j->meta_dirty = true;
        char line[160];
        snprintf(line, sizeof line, "\xE2\x94\x80\xE2\x94\x80 %s while Liu was closed", agentctl_outcome_name(j->outcome));
        agentctl_ring_push(&j->ring, j->outcome == AGO_DONE ? AGS_HEAD : AGS_ERR, (const u8 *)line, (u32)strlen(line));
        bump_gen();
        settled = true;
    }
    pthread_mutex_unlock(&g_mu);
    return settled;
}

/* Under g_mu: the job.meta files that are behind, for the server loop to
 * write outside the lock. */
typedef struct { char path[600]; char *text; usize len; } MetaFile;

static i32 metas_take(MetaFile *out, i32 cap) {
    i32 n = 0;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS && n < cap; i++) {
        Job *j = &g_jobs[i];
        if (!j->id || !j->meta_dirty || !j->dir[0]) continue;
        j->meta_dirty = false;
        if ((usize)snprintf(out[n].path, sizeof out[n].path, "%s/job.meta", j->dir) >= sizeof out[n].path) continue;
        char *t = malloc(16384);
        if (!t) continue;
        usize len = job_meta_text(j, t, 16384);
        if (!len) { free(t); continue; }
        out[n].text = t;
        out[n].len = len;
        n++;
    }
    return n;
}

static bool make_jobs_root(void) {
    char base[512];
    if (!liu_private_tmp_dir(base, sizeof base, "liu-agents")) return false;
    char agents[560];
    if ((usize)snprintf(agents, sizeof agents, "%s/liu-agents", base) >= sizeof agents) return false;
    if (mkdir(agents, 0700) != 0 && errno != EEXIST) return false;
    if (!liu_private_tmp_ours(agents)) return false;
    if ((usize)snprintf(g_jobs_root, sizeof g_jobs_root, "%s/%d", agents, (int)getpid()) >= sizeof g_jobs_root)
        return false;
    if (mkdir(g_jobs_root, 0700) != 0 && errno != EEXIST) return false;
    if (!liu_private_tmp_ours(g_jobs_root)) return false;
    adopt_dead_roots(agents);
    return true;
}

/* -------------------------------------------------------- job table --- */

static Job *job_find(u32 id) {
    if (id == 0) return NULL;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++)
        if (g_jobs[i].id == id) return &g_jobs[i];
    return NULL;
}

static Job *job_of_runner_pid(pid_t pid) {
    if (pid <= 0) return NULL;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++)
        if (g_jobs[i].id && g_jobs[i].runner_pid == pid) return &g_jobs[i];
    return NULL;
}

static void changes_free(Changes *c);
static void wins_free(CmdWin *w);

static void job_clear(Job *j) {
    free(j->prompt);
    agentctl_ring_free(&j->ring);
    changes_free(&j->ch);
    wins_free(j->wins);
    remove_job_dir(j->dir);
    memset(j, 0, sizeof *j);
    j->runner = -1;
}

static void recount_jobs(void) {
    i32 n = 0;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) if (g_jobs[i].id) n++;
    atomic_store_explicit(&g_job_count, n, memory_order_release);
}

/* A free slot, or the oldest job that has finished for good. */
static Job *job_alloc(void) {
    Job *oldest = NULL;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        Job *j = &g_jobs[i];
        if (j->id == 0) { memset(j, 0, sizeof *j); j->runner = -1; return j; }
        if ((j->state == AGJ_ENDED || j->state == AGJ_DECLINED) && j->runner < 0 && j->runner_pid == 0 &&
            (!oldest || j->created_ms < oldest->created_ms))
            oldest = j;
    }
    if (oldest) { job_clear(oldest); return oldest; }
    return NULL;
}

static bool job_active(const Job *j) {
    return j->id && (j->state == AGJ_STARTING || j->state == AGJ_RUNNING);
}

static i32 active_count(void) {
    i32 n = 0;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) if (job_active(&g_jobs[i])) n++;
    return n;
}

static void job_end(Job *j, u8 end, const char *why);
static void member_name(u64 owner, u32 id, char *out, usize cap);
static usize result_head(const char *dir, char *out, usize cap);

/* Under g_mu: whether the jobs `j` starts after are done: 1 = all of them,
 * 0 = still working, -1 = one will not finish (`*which` names it). */
static i32 deps_state(const Job *j, u32 *which) {
    i32 st = 1;
    for (u8 k = 0; k < j->nafter; k++) {
        const Job *d = job_find(j->after[k]);
        if (!d || d->state == AGJ_DECLINED || (d->state == AGJ_ENDED && d->outcome != AGO_DONE) ||
            (d->state == AGJ_IDLE && d->outcome != AGO_DONE)) {
            if (which) *which = j->after[k];
            return -1;
        }
        if (!(d->state == AGJ_IDLE || d->state == AGJ_ENDED)) st = 0;
    }
    return st;
}

/* Under g_mu: what the jobs `j` waited for left it, ahead of its task: each
 * one's report and the files it changed. */
static void deps_brief(Job *j) {
    if (!j->nafter || !j->prompt) return;
    usize cap = 16u * 1024u + j->prompt_len, w = 0;
    char *p = malloc(cap);
    if (!p) return;
    for (u8 k = 0; k < j->nafter; k++) {
        const Job *d = job_find(j->after[k]);
        if (!d) continue;
        char name[160], head[4096];
        member_name(d->owner_serial, d->id, name, sizeof name);
        result_head(d->dir, head, sizeof head);
        w += (usize)snprintf(p + w, cap - w, "%s finished before you started. It reported:\n%s\n",
                             name, head[0] ? head : d->summary);
        if (d->wt[0]) w += (usize)snprintf(p + w, cap - w, "Its work is in the worktree %s (branch %s).\n", d->wt, d->wt_branch);
        if (d->ch.n) {
            w += (usize)snprintf(p + w, cap - w, "Files it changed:");
            for (u32 i = 0; i < d->ch.n && i < 40 && w + 1100 < cap; i++)
                w += (usize)snprintf(p + w, cap - w, " %s", agentctl_team_rel(d->ch.path[i], j->cwd));
            w += (usize)snprintf(p + w, cap - w, "\n");
        }
        w += (usize)snprintf(p + w, cap - w, "\n");
    }
    if (w == 0 || w + j->prompt_len + 1 > cap) { free(p); return; }
    memcpy(p + w, j->prompt, j->prompt_len);
    w += j->prompt_len;
    p[w] = '\0';
    free(j->prompt);
    j->prompt = p;
    j->prompt_len = (u32)w;
}

/* Queued jobs take the slots that are free, oldest approval first; the
 * service pass starts their runners. A job that starts after others waits
 * for them, and ends when one of them will not finish. */
static void promote_queue(void) {
    i32 limit = g_policy.max_running < 1 ? 1 : g_policy.max_running;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        Job *j = &g_jobs[i];
        u32 which = 0;
        if (j->id && j->state == AGJ_QUEUED && j->nafter && deps_state(j, &which) < 0) {
            char why[160];
            snprintf(why, sizeof why, "job %u, which it was to start after, did not finish", which);
            job_end(j, AGE_COULD_NOT_START, why);
            j->ended_notice = true;
        }
    }
    while (active_count() < limit) {
        Job *next = NULL;
        for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
            Job *j = &g_jobs[i];
            if (!j->id || j->state != AGJ_QUEUED || (j->nafter && deps_state(j, NULL) != 1)) continue;
            if (!next || j->seq < next->seq) next = j;
        }
        if (!next) break;
        deps_brief(next);
        next->state = AGJ_STARTING;
        next->runner_pid = 0;
        next->meta_dirty = true;
        bump_gen();
    }
}

/* Under g_mu: the runner of `j` is to be collected (it was told to go, or it
 * already went). */
static void reap_later(Job *j) {
    if (j->runner_pid <= 0) return;
    if (g_reap_count < REAP_CAP) {
        g_reap[g_reap_count].pid = j->runner_pid;
        g_reap[g_reap_count].due_ms = mono_ms() + RUNNER_EXIT_MS;
        g_reap[g_reap_count].killed = false;
        g_reap_count++;
    } else {
        kill(j->runner_pid, SIGKILL);
        (void)waitpid(j->runner_pid, NULL, 0);
    }
    j->runner_pid = 0;
}

static void lead_mark(u64 owner, bool on);

/* Under g_mu: a pane leads a team only while one of its jobs lives; with
 * none left its agent's tool hooks stop at the shell (hook_scan.h) and its
 * screen is not read. */
static void lead_settle(u64 owner) {
    if (!owner) return;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        const Job *o = &g_jobs[i];
        if (o->id && o->owner_serial == owner && o->state != AGJ_ENDED && o->state != AGJ_DECLINED) return;
    }
    lead_mark(owner, false);
}

/* Under g_mu: the job is over. A runner still connected is told to go. */
static void job_end(Job *j, u8 end, const char *why) {
    if (j->state == AGJ_ENDED) return;
    j->state = AGJ_ENDED;
    j->end = end;
    j->meta_dirty = true;
    if (why && why[0] && !j->error[0]) snprintf(j->error, sizeof j->error, "%s", why);
    if (j->runner >= 0) j->send_close = true;
    else reap_later(j);
    lead_settle(j->owner_serial);
    bump_gen();
    promote_queue();
}

/* A label from the prompt's first line when the caller gave none. */
static void derive_label(const char *prompt, char *out, usize cap) {
    usize o = 0;
    for (const unsigned char *p = (const unsigned char *)prompt; *p && *p != '\n' && o + 1 < cap; p++) {
        if (*p < 0x20 || *p == 0x7f) continue;
        out[o++] = (char)*p;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    /* do not end inside a UTF-8 sequence */
    while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) {
        usize k = o - 1;
        while (k > 0 && ((unsigned char)out[k] & 0xC0) == 0x80) k--;
        unsigned char lead = (unsigned char)out[k];
        usize need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (o - k == need) break;
        o = k;
    }
    out[o] = '\0';
}

/* ---------------------------------------------------------- connections --- */

static void conn_queue(Conn *c, const AgentctlMsg *m) {
    if (c->fd < 0 || !m || !m->buf || m->failed) return;
    if (c->out_len - c->out_off + m->len > c->out_cap || c->out_off) {
        u32 pending = c->out_len - c->out_off;
        if (c->out_off && pending) memmove(c->out, c->out + c->out_off, pending);
        c->out_len = pending;
        c->out_off = 0;
        if (c->out_len + m->len > c->out_cap) {
            u32 cap = c->out_cap ? c->out_cap : 1024u;
            while (cap < c->out_len + m->len) cap *= 2u;
            u8 *nb = realloc(c->out, cap);
            if (!nb) { c->close_after_flush = true; return; }
            c->out = nb;
            c->out_cap = cap;
        }
    }
    memcpy(c->out + c->out_len, m->buf, m->len);
    c->out_len += m->len;
}

static void conn_reply(Conn *c, u32 job, AgentctlCode code, const char *text) {
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_REPLY, job)) return;
    agentctl_msg_put_u64(&m, AGK_CODE, (u64)code);
    if (text && text[0]) agentctl_msg_put_str(&m, AGK_TEXT, text);
    if (agentctl_msg_end(&m)) conn_queue(c, &m);
    agentctl_msg_free(&m);
}

static void conn_free(Conn *c) {
    if (c->fd >= 0) close(c->fd);
    agentctl_reader_reset(&c->rd);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

/* Under g_mu: a runner that goes away ends its job. */
static void conn_detach_locked(i32 idx) {
    Conn *c = &g_conns[idx];
    if (!c->runner) return;
    Job *j = job_find(c->runner_job);
    if (j && j->runner == idx) {
        j->runner = -1;
        if (j->parked && j->state == AGJ_IDLE) {
            reap_later(j);                   /* let go between turns: the job stays */
        } else if (j->state != AGJ_ENDED) {
            if (!j->error[0]) snprintf(j->error, sizeof j->error, "its runner stopped");
            job_end(j, AGE_RUNNER_EXITED, NULL);
        } else {
            reap_later(j);
        }
    }
}

static void flush_conn(Conn *c) {
    while (c->fd >= 0 && c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, AGENTCTL_SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            c->close_after_flush = true;
            c->out_off = c->out_len;
            break;
        }
        c->out_off += (u32)n;
    }
    if (c->out_off == c->out_len) c->out_off = c->out_len = 0;
}

/* --------------------------------------------------------- job fields --- */

static void put_job(AgentctlMsg *m, const Job *j) {
    const AgentctlAgentInfo *ai = agentctl_agent_info(j->agent);
    agentctl_msg_put_u64(m, AGK_JOB, j->id);
    agentctl_msg_put_str(m, AGK_AGENT, ai ? ai->id : "");
    agentctl_msg_put_str(m, AGK_MODEL, j->model);
    agentctl_msg_put_str(m, AGK_EFFORT, j->effort);
    agentctl_msg_put_str(m, AGK_PERMISSION, agentctl_perm_name(j->perm));
    agentctl_msg_put_str(m, AGK_CWD, j->cwd);
    agentctl_msg_put_str(m, AGK_LABEL, j->label);
    agentctl_msg_put_str(m, AGK_STATE, agentctl_state_name(j->state));
    agentctl_msg_put_str(m, AGK_OUTCOME, agentctl_outcome_name(j->outcome));
    agentctl_msg_put_str(m, AGK_END, agentctl_end_name(j->end));
    if (j->error[0])   agentctl_msg_put_str(m, AGK_ERROR, j->error);
    if (j->session[0]) agentctl_msg_put_str(m, AGK_SESSION, j->session);
    if (j->usage[0])   agentctl_msg_put_str(m, AGK_USAGE, j->usage);
    agentctl_msg_put_str(m, AGK_DIR, j->dir);
    agentctl_msg_put_u64(m, AGK_TURNS, j->turns);
    i64 elapsed = 0;
    if (j->turn_start_ms) {
        i64 end = j->state == AGJ_RUNNING ? now_ms() : j->turn_end_ms;
        if (end > j->turn_start_ms) elapsed = end - j->turn_start_ms;
    }
    agentctl_msg_put_u64(m, AGK_ELAPSED_MS, (u64)elapsed);
    char v[160];
    snprintf(v, sizeof v, "%llu %llu %llu %llu %u", (unsigned long long)j->spent_in,
             (unsigned long long)j->spent_cached, (unsigned long long)j->spent_out,
             (unsigned long long)j->spent_cost, j->turns);
    agentctl_msg_put_str(m, AGK_SPEND, v);
    snprintf(v, sizeof v, "%u %llu %llu %u", j->max_turns, (unsigned long long)j->max_tokens,
             (unsigned long long)j->budget_micro, j->deadline_s);
    agentctl_msg_put_str(m, AGK_LIMITS, v);
    if (j->wt[0]) {
        agentctl_msg_put_str(m, AGK_WORKTREE, j->wt);
        agentctl_msg_put_str(m, AGK_BRANCH, j->wt_branch);
        agentctl_msg_put_str(m, AGK_BASE, j->wt_base);
    }
    if (j->merged[0]) agentctl_msg_put_str(m, AGK_MERGED, j->merged);
    if (j->asking) agentctl_msg_put_str(m, AGK_ASKING, j->ask_what);
    if (j->review_of) agentctl_msg_put_u64(m, AGK_REVIEW, j->review_of);
    if (j->isolate) agentctl_msg_put_str(m, AGK_ISOLATE, "1");
    for (u8 k = 0; k < j->nafter; k++) agentctl_msg_put_u64(m, AGK_AFTER, j->after[k]);
    /* the files it changed and the clashes it was in, one a line */
    if (j->ch.n) {
        usize cap = 16u * 1024u, w = 0;
        char *files = malloc(cap);
        if (files) {
            for (u32 i = 0; i < j->ch.n && w + strlen(j->ch.path[i]) + 2 < cap; i++)
                w += (usize)snprintf(files + w, cap - w, "%s%s\n", j->ch.ns[i] ? "" : "-", j->ch.path[i]);
            files[w] = '\0';
            agentctl_msg_put_str(m, AGK_FILES, files);
            free(files);
        }
    }
    if (j->nclash) {
        char cl[AGT_JOB_CLASHES * 242];
        usize w = 0;
        for (u8 k = 0; k < j->nclash && w < sizeof cl; k++)
            w += (usize)snprintf(cl + w, sizeof cl - w, "%s\n", j->clashes[k]);
        agentctl_msg_put_str(m, AGK_CLASHES, cl);
    }
}

static void reply_job(Conn *c, const Job *j, AgentctlCode code, const char *text) {
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_REPLY, j->id)) return;
    agentctl_msg_put_u64(&m, AGK_CODE, (u64)code);
    if (text && text[0]) agentctl_msg_put_str(&m, AGK_TEXT, text);
    put_job(&m, j);
    if (agentctl_msg_end(&m)) conn_queue(c, &m);
    agentctl_msg_free(&m);
}

/* ---------------------------------------------------------------- team --- */

#define AGT_LEAD_NAMES 512              /* file names remembered from a lead's screen */

/* A team is the lead (the pane that started jobs, AGT_LEAD) and its jobs
 * (agentctl_team.h). The lead has no Job of its own: what it has read and
 * the paths it owns are kept here. */
typedef struct {
    u64  owner;             /* 0 = a free slot */
    u64  seen;              /* the newest post it has read */
    char owns[AGT_OWNS_MAX][AGT_PATH_MAX];
    u8   nowns;
    char cwd[AGT_PATH_MAX]; /* its folder, as its last request named it */
    Changes ch;             /* the files its agent changed */
    CmdWin  wins[AGT_CMD_WINS];   /* its agent's tool calls, from its hooks */
    i64  hooked_ns;         /* when its agent's tool hooks last spoke, 0 = never */
    u64  settle_seen;       /* the newest turn end of its jobs its hooks have told it of */
    /* The file names its pane's screen showed (agentctl_lead_screen): every
     * agent shows the file it edits and the command it runs, so a name that
     * appeared there just then says the change may be its agent's. */
    struct {
        u64 hash;           /* of the name, 0 = a free slot */
        u16 count;          /* times on the last screen */
        i64 rise_ns;        /* when it last appeared anew (more of it than before) */
    } names[AGT_LEAD_NAMES];
} Lead;
#define AGT_LEADS 16

typedef struct {
    u64   seq;              /* 0 = a free slot */
    u64   owner;
    u32   from, to;
    char *text;             /* sanitized, one line */
} Post;

static Lead g_leads[AGT_LEADS];
static Post g_posts[AGT_POSTS];
static u32  g_post_next;              /* the slot the next post takes; the oldest is there */
static u64  g_post_seq;               /* the newest post's */
static u64  g_touch_seq;              /* the newest file change's */
static bool g_team_files_dirty;       /* some job's team.md is behind */

static bool job_shown(const Job *j);

static void lead_mark(u64 owner, bool on);

/* Under g_mu: the lead of `owner`'s team; with `make`, made when missing
 * (a slot whose pane has no job left is taken over). */
static Lead *lead_of(u64 owner, bool make) {
    if (!owner) return NULL;
    Lead *free_slot = NULL;
    for (i32 i = 0; i < AGT_LEADS; i++) {
        if (g_leads[i].owner == owner) return &g_leads[i];
        if (!free_slot && !g_leads[i].owner) free_slot = &g_leads[i];
    }
    if (!make) return NULL;
    for (i32 i = 0; i < AGT_LEADS && !free_slot; i++) {
        bool used = false;
        for (i32 k = 0; k < AGENTCTL_MAX_JOBS && !used; k++)
            used = g_jobs[k].id && g_jobs[k].owner_serial == g_leads[i].owner;
        if (!used) free_slot = &g_leads[i];
    }
    if (!free_slot) return NULL;
    if (free_slot->owner) lead_mark(free_slot->owner, false);
    changes_free(&free_slot->ch);
    wins_free(free_slot->wins);
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->owner = owner;
    lead_mark(owner, true);
    return free_slot;
}

/* Under g_mu: whether `owner`'s team has a job at all. */
static bool team_exists(u64 owner) {
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++)
        if (g_jobs[i].owner_serial == owner && job_shown(&g_jobs[i])) return true;
    return false;
}

/* Under g_mu: how the team names `id`. */
static void member_name(u64 owner, u32 id, char *out, usize cap) {
    if (id == AGT_LIU) { snprintf(out, cap, "Liu"); return; }
    if (id == AGT_ALL) { snprintf(out, cap, "all"); return; }
    if (id == AGT_LEAD) {
        for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++)
            if (g_jobs[i].id && g_jobs[i].owner_serial == owner && g_jobs[i].owner_label[0]) {
                snprintf(out, cap, "%s", g_jobs[i].owner_label);
                return;
            }
        snprintf(out, cap, "the lead");
        return;
    }
    const Job *j = job_find(id);
    if (!j) { snprintf(out, cap, "job %u", id); return; }
    const AgentctlAgentInfo *ai = agentctl_agent_info(j->agent);
    snprintf(out, cap, "%s%s%s (job %u)", ai ? ai->display : "agent", j->label[0] ? " \xC2\xB7 " : "",
             j->label, j->id);
}

/* Under g_mu: whether the post is one `me` is to read (to everyone or to it,
 * from someone else). */
static bool post_for(const Post *p, u64 owner, u32 me) {
    return p->seq && p->owner == owner && p->from != me && (p->to == AGT_ALL || p->to == me);
}

/* Under g_mu: posts `text` to `owner`'s team. Returns its seq, 0 when there
 * was nothing to post. */
static u64 post_add(u64 owner, u32 from, u32 to, const char *text, usize len) {
    char clean[AGT_POST_MAX];
    usize n = agentctl_sanitize(text, len, clean, sizeof clean, false);
    while (n > 0 && clean[n - 1] == ' ') n--;
    usize lead_blank = 0;
    while (lead_blank < n && clean[lead_blank] == ' ') lead_blank++;
    if (n <= lead_blank) return 0;
    char *t = malloc(n - lead_blank + 1);
    if (!t) return 0;
    memcpy(t, clean + lead_blank, n - lead_blank);
    t[n - lead_blank] = '\0';
    Post *p = &g_posts[g_post_next];
    free(p->text);
    p->text = t;
    p->seq = ++g_post_seq;
    p->owner = owner;
    p->from = from;
    p->to = to;
    g_post_next = (g_post_next + 1) % AGT_POSTS;
    lead_of(owner, true);
    g_team_files_dirty = true;
    bump_line_gen();
    return p->seq;
}

/* Under g_mu: the paths `owns` joined for a reader in `root`. */
static void owns_text(char owns[][AGT_PATH_MAX], u8 n, const char *root, char *out, usize cap) {
    usize w = 0;
    out[0] = '\0';
    for (u8 k = 0; k < n && w < cap; k++)
        w += (usize)snprintf(out + w, cap - w, "%s%s", k ? ", " : "", agentctl_team_rel(owns[k], root));
}

/* `path` (absolute, normalised) as the file system names it: the part of it
 * that exists resolved through its symbolic links, the rest kept as written,
 * so two spellings of one file (/tmp and /private/tmp, a linked folder) are
 * one path. Left as it is when nothing of it resolves or the result does not
 * fit. */
static void canon_path(char *path, usize cap) {
    if (!path || path[0] != '/') return;
    char real[PATH_MAX], head[AGT_PATH_MAX];
    if (realpath(path, real)) {
        if (strlen(real) < cap) memcpy(path, real, strlen(real) + 1);
        return;
    }
    usize len = strlen(path);
    if (len >= sizeof head) return;
    memcpy(head, path, len + 1);
    for (;;) {
        char *slash = strrchr(head, '/');
        if (!slash) return;
        *slash = '\0';
        if (!realpath(head[0] ? head : "/", real)) {
            if (!head[0]) return;
            continue;
        }
        const char *rest = path + (slash - head);              /* "/name…" */
        usize rl = strlen(real);
        if (rl == 1) rl = 0;                                   /* the root: no "//" */
        if (rl + strlen(rest) + 1 > cap || rl + strlen(rest) + 1 > sizeof head) return;
        char out[AGT_PATH_MAX];
        memcpy(out, real, rl);
        memcpy(out + rl, rest, strlen(rest) + 1);
        memcpy(path, out, strlen(out) + 1);
        return;
    }
}

static i64 status_ns(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
#ifdef __APPLE__
    return (i64)st.st_ctimespec.tv_sec * 1000000000LL + st.st_ctimespec.tv_nsec;
#else
    return (i64)st.st_ctim.tv_sec * 1000000000LL + st.st_ctim.tv_nsec;
#endif
}

static i64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (i64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void changes_free(Changes *c) {
    for (u32 i = 0; i < c->n; i++) free(c->path[i]);
    memset(c, 0, sizeof *c);
}

static i32 changes_find(const Changes *c, const char *path) {
    for (u32 i = 0; i < c->n; i++)
        if (!strcmp(c->path[i], path)) return (i32)i;
    return -1;
}

static void changes_drop(Changes *c, u32 i) {
    free(c->path[i]);
    u32 last = c->n - 1;
    c->path[i] = c->path[last];
    c->seq[i] = c->seq[last];
    c->ns[i] = c->ns[last];
    c->seen[i] = c->seen[last];
    c->path[last] = NULL;
    c->n--;
}

/* Under g_mu: `path` changed again (`ns` its status time). Returns whether
 * it is new to `c`. */
static bool changes_put(Changes *c, const char *path, i64 ns, bool seen, bool *failed) {
    *failed = false;
    i32 at = changes_find(c, path);
    if (at >= 0) {
        c->seq[at] = ++g_touch_seq;
        c->ns[at] = ns;
        c->seen[at] = c->seen[at] && seen;
        return false;
    }
    char *copy = strdup(path);
    if (!copy) { *failed = true; return false; }
    if (c->n == AGT_TOUCHED_MAX) {
        /* the one changed longest ago goes */
        u32 old = 0;
        for (u32 i = 1; i < c->n; i++) if (c->seq[i] < c->seq[old]) old = i;
        changes_drop(c, old);
    }
    c->path[c->n] = copy;
    c->seq[c->n] = ++g_touch_seq;
    c->ns[c->n] = ns;
    c->seen[c->n] = seen;
    c->n++;
    return true;
}

/* Under g_mu: the newest files in `c` (at most `max`), for a reader in
 * `root`, with how many more there are. */
static void changed_text(const Changes *c, const char *root, i32 max, char *out, usize cap) {
    out[0] = '\0';
    if (c->n == 0) return;
    bool used[AGT_TOUCHED_MAX] = { false };
    usize w = 0;
    i32 listed = 0;
    while (listed < max && (u32)listed < c->n) {
        i32 best = -1;
        for (u32 i = 0; i < c->n; i++)
            if (!used[i] && (best < 0 || c->seq[i] > c->seq[best])) best = (i32)i;
        if (best < 0) break;
        used[best] = true;
        if (w < cap) w += (usize)snprintf(out + w, cap - w, "%s%s", listed ? ", " : "", agentctl_team_rel(c->path[best], root));
        listed++;
    }
    if ((u32)listed < c->n && w < cap) snprintf(out + w, cap - w, " and %u more", c->n - (u32)listed);
}

static void wins_free(CmdWin *w) {
    for (i32 i = 0; i < AGT_CMD_WINS; i++) free(w[i].cmd);
    memset(w, 0, sizeof(CmdWin) * AGT_CMD_WINS);
}

/* Under g_mu: a command `id` started now (its line `cmd`, may be NULL). A
 * window that ended over a minute ago, or else the oldest, makes room. */
static void win_open(CmdWin *w, const char *id, const char *cmd, i64 now) {
    CmdWin *slot = NULL;
    for (i32 i = 0; i < AGT_CMD_WINS && !slot; i++)
        if (w[i].start_ns && !strcmp(w[i].id, id ? id : "")) slot = &w[i];
    for (i32 i = 0; i < AGT_CMD_WINS && !slot; i++)
        if (!w[i].start_ns || (w[i].end_ns && now - w[i].end_ns > 60000000000LL)) slot = &w[i];
    if (!slot) {
        slot = &w[0];
        for (i32 i = 1; i < AGT_CMD_WINS; i++) if (w[i].start_ns < slot->start_ns) slot = &w[i];
    }
    free(slot->cmd);
    memset(slot, 0, sizeof *slot);
    snprintf(slot->id, sizeof slot->id, "%s", id ? id : "");
    slot->start_ns = now;
    if (cmd && cmd[0]) slot->cmd = strndup(cmd, 4096);
}

/* Under g_mu: command `id` ended now; `cmd`, when given, is its line. */
static void win_close(CmdWin *w, const char *id, const char *cmd, i64 now) {
    for (i32 i = 0; i < AGT_CMD_WINS; i++) {
        if (!w[i].start_ns || w[i].end_ns || strcmp(w[i].id, id ? id : "")) continue;
        w[i].end_ns = now;
        if (cmd && cmd[0] && !w[i].cmd) w[i].cmd = strndup(cmd, 4096);
        return;
    }
}

/* Under g_mu: every command still open ends now (its turn ended). */
static void wins_close_all(CmdWin *w, i64 now) {
    for (i32 i = 0; i < AGT_CMD_WINS; i++) if (w[i].start_ns && !w[i].end_ns) w[i].end_ns = now;
}

/* Under g_mu: the window of `w` that was open at `t` (give or take
 * `slack`), or NULL. */
static const CmdWin *wins_at(const CmdWin *w, i64 t, i64 slack, i64 now) {
    for (i32 i = 0; i < AGT_CMD_WINS; i++) {
        if (!w[i].start_ns) continue;
        i64 end = w[i].end_ns ? w[i].end_ns : now;
        if (t >= w[i].start_ns - slack && t <= end + slack) return &w[i];
    }
    return NULL;
}

/* Under g_mu: a member's state as the team reads it. */
static void member_state(const Job *j, char *out, usize cap) {
    switch (j->state) {
    case AGJ_PENDING_APPROVAL: snprintf(out, cap, "waiting for the user's OK"); break;
    case AGJ_QUEUED:           snprintf(out, cap, "queued"); break;
    case AGJ_STARTING:         snprintf(out, cap, "starting"); break;
    case AGJ_RUNNING: {
        i64 m = j->turn_start_ms ? (now_ms() - j->turn_start_ms) / 60000 : 0;
        if (m > 0) snprintf(out, cap, "working, %lldm", (long long)m);
        else       snprintf(out, cap, "working");
        break;
    }
    case AGJ_IDLE:
        snprintf(out, cap, "between turns, last one %s", agentctl_outcome_name(j->outcome));
        break;
    default: snprintf(out, cap, "ended"); break;
    }
}

/* Scratch for one listing of a team: its members' strings. */
typedef struct {
    AgtMember m[AGENTCTL_MAX_JOBS + 1];
    char name[AGENTCTL_MAX_JOBS + 1][160];
    char state[AGENTCTL_MAX_JOBS + 1][64];
    char doing[AGENTCTL_MAX_JOBS + 1][160];
    char owns[AGENTCTL_MAX_JOBS + 1][512];
    char changed[AGENTCTL_MAX_JOBS + 1][512];
    i32  n;
    AgtPost p[AGT_LIST_POSTS > AGT_NOTES_POSTS ? AGT_LIST_POSTS : AGT_NOTES_POSTS];
    char from[AGT_LIST_POSTS > AGT_NOTES_POSTS ? AGT_LIST_POSTS : AGT_NOTES_POSTS][160];
    char to[AGT_LIST_POSTS > AGT_NOTES_POSTS ? AGT_LIST_POSTS : AGT_NOTES_POSTS][160];
    i32  np;
} TeamScratch;

/* Under g_mu: `owner`'s members into `ts`, as `me` reads them from `root`. */
static void team_members(TeamScratch *ts, u64 owner, u32 me, const char *root) {
    ts->n = 0;
    i32 k = ts->n++;
    member_name(owner, AGT_LEAD, ts->name[k], sizeof ts->name[k]);
    ts->state[k][0] = ts->doing[k][0] = ts->changed[k][0] = '\0';
    Lead *l = lead_of(owner, false);
    if (l) owns_text(l->owns, l->nowns, root, ts->owns[k], sizeof ts->owns[k]);
    else ts->owns[k][0] = '\0';
    if (l) changed_text(&l->ch, root, 6, ts->changed[k], sizeof ts->changed[k]);
    ts->m[k] = (AgtMember){ AGT_LEAD, ts->name[k], ts->state[k], ts->doing[k], ts->owns[k], ts->changed[k], true, me == AGT_LEAD };
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        Job *j = &g_jobs[i];
        if (j->owner_serial != owner || !job_shown(j)) continue;
        k = ts->n++;
        member_name(owner, j->id, ts->name[k], sizeof ts->name[k]);
        member_state(j, ts->state[k], sizeof ts->state[k]);
        ts->doing[k][0] = '\0';
        if (j->state == AGJ_RUNNING && j->ring.count) {
            const RingLine *rl = agentctl_ring_at(&j->ring, j->ring.count - 1);
            usize n = rl->len < 120 ? rl->len : 120;
            while (n > 0 && n < rl->len && ((unsigned char)rl->text[n] & 0xC0) == 0x80) n--;
            memcpy(ts->doing[k], rl->text, n);
            ts->doing[k][n] = '\0';
        }
        owns_text(j->owns, j->nowns, root, ts->owns[k], sizeof ts->owns[k]);
        changed_text(&j->ch, root, 6, ts->changed[k], sizeof ts->changed[k]);
        ts->m[k] = (AgtMember){ j->id, ts->name[k], ts->state[k], ts->doing[k], ts->owns[k], ts->changed[k],
                                false, me == j->id };
    }
}

/* Under g_mu: the newest posts of `owner`'s team after `after`, oldest
 * first, at most `max`, into `ts`: the ones for `me` to read, or with
 * `only_for_me` false every one it may see (its own too). */
static void team_posts(TeamScratch *ts, u64 owner, u32 me, u64 after, bool only_for_me, i32 max) {
    ts->np = 0;
    i32 cap = (i32)(sizeof ts->p / sizeof ts->p[0]);
    if (max > cap) max = cap;
    /* newest first, then turned around */
    const Post *pick[AGT_POSTS];
    i32 n = 0;
    for (i32 k = AGT_POSTS - 1; k >= 0 && n < max; k--) {
        const Post *p = &g_posts[(g_post_next + (u32)k) % AGT_POSTS];
        if (!p->seq || p->owner != owner || p->seq <= after) continue;
        if (only_for_me ? !post_for(p, owner, me) : !(p->to == AGT_ALL || p->to == me || p->from == me)) continue;
        pick[n++] = p;
    }
    for (i32 i = n - 1; i >= 0; i--) {
        i32 k = ts->np++;
        member_name(owner, pick[i]->from, ts->from[k], sizeof ts->from[k]);
        member_name(owner, pick[i]->to, ts->to[k], sizeof ts->to[k]);
        ts->p[k] = (AgtPost){ ts->from[k], ts->to[k], pick[i]->text };
    }
}

/* Under g_mu: the newest post `me` may read; its read mark moves there. */
static void team_mark_read(u64 owner, u32 me) {
    if (me == AGT_LEAD) {
        Lead *l = lead_of(owner, true);
        if (l) l->seen = g_post_seq;
    } else {
        Job *j = job_find(me);
        if (j) j->board_seen = g_post_seq;
    }
}

/* Under g_mu: `liu-agent team` for `me`, malloc'd. Its posts count as read. */
static char *team_listing(u64 owner, u32 me, const char *root) {
    TeamScratch *ts = calloc(1, sizeof *ts);
    char *out = malloc(AGT_LISTING_MAX);
    if (!ts || !out) { free(ts); free(out); return NULL; }
    if (!team_exists(owner)) {
        agentctl_team_listing(out, AGT_LISTING_MAX, root, NULL, 0, NULL, 0);
    } else {
        team_members(ts, owner, me, root);
        team_posts(ts, owner, me, 0, false, AGT_LIST_POSTS);
        agentctl_team_listing(out, AGT_LISTING_MAX, root, ts->m, ts->n, ts->p, ts->np);
        team_mark_read(owner, me);
    }
    free(ts);
    return out;
}

/* Under g_mu: the team notes `j`'s next turn starts with, malloc'd, or NULL
 * when it has no one to know about and nothing to read. What it is given is
 * also shown in its log, and its read marks move forward. */
static char *team_notes_for(Job *j) {
    u64 owner = j->owner_serial;
    TeamScratch *ts = calloc(1, sizeof *ts);
    char *out = malloc(AGT_NOTES_MAX);
    AgtChange *ch = calloc(AGT_NOTES_FILES, sizeof *ch);
    char (*chby)[160] = calloc(AGT_NOTES_FILES, sizeof *chby);
    if (!ts || !out || !ch || !chby) { free(ts); free(out); free(ch); free(chby); return NULL; }
    team_members(ts, owner, j->id, j->cwd);
    team_posts(ts, owner, j->id, j->board_seen, true, AGT_NOTES_POSTS);
    /* the files the others changed since this job's last turn, newest first */
    i32 nch = 0;
    u64 below = ~(u64)0;
    const Lead *lead = lead_of(owner, false);
    while (nch < AGT_NOTES_FILES) {
        const Changes *bc = NULL;
        u32 bi = 0, bwho = 0;
        for (i32 i = -1; i < AGENTCTL_MAX_JOBS; i++) {
            const Changes *c;
            u32 who;
            if (i < 0) {
                if (!lead) continue;
                c = &lead->ch;
                who = AGT_LEAD;
            } else {
                const Job *o = &g_jobs[i];
                if (o == j || o->owner_serial != owner || !job_shown(o)) continue;
                c = &o->ch;
                who = o->id;
            }
            for (u32 t = 0; t < c->n; t++) {
                u64 sq = c->seq[t];
                if (sq <= j->touch_seen || sq >= below) continue;
                if (!bc || sq > bc->seq[bi]) { bc = c; bi = t; bwho = who; }
            }
        }
        if (!bc) break;
        below = bc->seq[bi];
        member_name(owner, bwho, chby[nch], sizeof chby[nch]);
        ch[nch] = (AgtChange){ agentctl_team_rel(bc->path[bi], j->cwd), chby[nch] };
        nch++;
    }
    bool others_working = false;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        const Job *o = &g_jobs[i];
        if (o != j && o->id && o->owner_serial == owner && o->state == AGJ_RUNNING) others_working = true;
    }
    char you[160], mine[512];
    member_name(owner, j->id, you, sizeof you);
    owns_text(j->owns, j->nowns, j->cwd, mine, sizeof mine);
    usize len = agentctl_team_notes(out, AGT_NOTES_MAX, you, ts->m, ts->n, ts->p, ts->np, ch, nch, mine,
                                    others_working);
    if (len > 0 && ts->np + nch > 0) {
        char line[AGT_POST_MAX + 256];
        snprintf(line, sizeof line, "\xE2\x86\xB3 team notes: %d message%s, %d file%s changed by others",
                 ts->np, ts->np == 1 ? "" : "s", nch, nch == 1 ? "" : "s");
        agentctl_ring_push(&j->ring, AGS_DIM, (const u8 *)line, (u32)strlen(line));
        for (i32 i = 0; i < ts->np; i++) {
            snprintf(line, sizeof line, "\xE2\x9C\x89 %s \xE2\x86\x92 %s: %s", ts->p[i].from, ts->p[i].to, ts->p[i].text);
            agentctl_ring_push(&j->ring, AGS_DIM, (const u8 *)line, (u32)strlen(line));
        }
        bump_line_gen();
    }
    j->board_seen = g_post_seq;
    j->touch_seen = g_touch_seq;
    free(ts);
    free(ch);
    free(chby);
    if (len == 0) { free(out); return NULL; }
    return out;
}

/* Clash notices one batch of the folder watch may raise before the rest are
 * summed up in one post; -1 = no limit. */
static i32 g_clash_budget = -1;
static u32 g_clash_held;

/* Under g_mu: a member of `owner`'s team: its changed files and its paths. */
static Changes *member_changes(u64 owner, u32 who) {
    if (who == AGT_LEAD) { Lead *l = lead_of(owner, false); return l ? &l->ch : NULL; }
    Job *j = job_find(who);
    return j && j->owner_serial == owner ? &j->ch : NULL;
}

/* Under g_mu: tells the team, the members' logs and the user that `who`
 * changing `path` clashes with `other` (`root`: the folder the path is told
 * from). */
static void team_clash(u64 owner, u32 who, u32 other, const char *path, const char *root, AgtClash kind) {
    if (g_clash_budget == 0) { g_clash_held++; return; }
    if (g_clash_budget > 0) g_clash_budget--;
    char a[160], b[160], msg[AGT_POST_MAX];
    member_name(owner, who, a, sizeof a);
    member_name(owner, other, b, sizeof b);
    const char *rel = agentctl_team_rel(path, root);
    if (kind == AGT_CLASH_BOTH) snprintf(msg, sizeof msg, "%s and %s both changed %s.", a, b, rel);
    else                        snprintf(msg, sizeof msg, "%s changed %s, which %s owns.", a, rel, b);
    post_add(owner, AGT_LIU, AGT_ALL, msg, strlen(msg));
    char line[AGT_POST_MAX + 8];
    snprintf(line, sizeof line, "\xE2\x9A\xA0 %s", msg);
    Job *jw = who != AGT_LEAD ? job_find(who) : NULL;
    Job *jo = other != AGT_LEAD ? job_find(other) : NULL;
    if (jw) agentctl_ring_push(&jw->ring, AGS_ERR, (const u8 *)line, (u32)strlen(line));
    if (jo) agentctl_ring_push(&jo->ring, AGS_ERR, (const u8 *)line, (u32)strlen(line));
    for (i32 q = 0; q < 2; q++) {
        Job *cj = q == 0 ? jw : jo;
        if (!cj) continue;
        if (cj->nclash == AGT_JOB_CLASHES) {
            memmove(cj->clashes[0], cj->clashes[1], sizeof cj->clashes[0] * (AGT_JOB_CLASHES - 1));
            cj->nclash--;
        }
        snprintf(cj->clashes[cj->nclash++], sizeof cj->clashes[0], "%s", msg);
    }
    /* the user hears it through whichever job is in it */
    Job *tell = jw ? jw : jo;
    if (tell) {
        snprintf(tell->notice_text, sizeof tell->notice_text, "%s", msg);
        tell->notice = true;
    }
    bump_line_gen();
}

/* Under g_mu: `who` of `owner`'s team changed `raw` (as it named it,
 * relative to `cwd` or absolute) — named by its own tool, or `seen` changing
 * on disk while one of its commands ran. Returns true when the main thread
 * has a notice to show. */
static bool member_touch(u64 owner, u32 who, const char *cwd, const char *raw, usize len, bool seen) {
    Changes *mc = member_changes(owner, who);
    if (!mc) return false;
    char path[AGT_PATH_MAX];
    if (!agentctl_team_path(cwd, raw, len, path, sizeof path)) return false;
    canon_path(path, sizeof path);
    /* One write claimed twice — a command of one member was running while
     * another member's tool wrote the file it names: the file's status time
     * says it is the same write, and the tool that named it is believed. */
    i64 ns = status_ns(path);
    if (ns) {
        for (i32 i = -1; i < AGENTCTL_MAX_JOBS; i++) {
            Changes *oc;
            if (i < 0) {
                if (who == AGT_LEAD) continue;
                Lead *l = lead_of(owner, false);
                if (!l) continue;
                oc = &l->ch;
            } else {
                Job *o = &g_jobs[i];
                if (o->id == who || !o->id || o->owner_serial != owner) continue;
                oc = &o->ch;
            }
            i32 t = changes_find(oc, path);
            if (t < 0 || oc->ns[t] != ns) continue;
            if (seen) return false;                      /* theirs already */
            if (oc->seen[t]) { changes_drop(oc, (u32)t); g_team_files_dirty = true; }
            break;
        }
    }
    bool failed = false;
    bool fresh = changes_put(mc, path, ns, seen, &failed);
    if (!failed) g_team_files_dirty = true;
    if (!fresh) return false;
    /* new to it: against every other member */
    const char *mine[AGT_OWNS_MAX];
    u8 nmine = 0;
    if (who == AGT_LEAD) {
        Lead *l = lead_of(owner, false);
        for (u8 k = 0; l && k < l->nowns; k++) mine[nmine++] = l->owns[k];
    } else {
        Job *j = job_find(who);
        for (u8 k = 0; j && k < j->nowns; k++) mine[nmine++] = j->owns[k];
    }
    bool told = false;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        Job *o = &g_jobs[i];
        if (o->id == who || o->owner_serial != owner || !job_shown(o)) continue;
        const char *theirs[AGT_OWNS_MAX];
        for (u8 k = 0; k < o->nowns; k++) theirs[k] = o->owns[k];
        AgtClash c = agentctl_team_clash(path, mine, nmine, theirs, o->nowns, changes_find(&o->ch, path) >= 0);
        if (c != AGT_CLASH_NONE) { team_clash(owner, who, o->id, path, cwd, c); told = true; }
    }
    if (who != AGT_LEAD) {
        Lead *l = lead_of(owner, false);
        if (l) {
            const char *theirs[AGT_OWNS_MAX];
            for (u8 k = 0; k < l->nowns; k++) theirs[k] = l->owns[k];
            AgtClash c = agentctl_team_clash(path, mine, nmine, theirs, l->nowns, changes_find(&l->ch, path) >= 0);
            if (c != AGT_CLASH_NONE) { team_clash(owner, who, AGT_LEAD, path, cwd, c); told = true; }
        }
    }
    return told;
}

static bool team_touch(Job *j, const char *raw, usize len, bool seen) {
    return member_touch(j->owner_serial, j->id, j->cwd, raw, len, seen);
}

/* Under g_mu: the paths in `f` (AGK_PATH, repeated), normalised against
 * `cwd`, into `owns`. False with `why` when one is not a usable path or
 * there are too many. */
static bool owns_from(const AgentctlFrame *f, const char *cwd, char owns[][AGT_PATH_MAX], u8 *n, const char **why) {
    u32 pos = 0; u16 k; const u8 *d; u32 len;
    u8 got = 0;
    while (agentctl_next(f, &pos, &k, &d, &len)) {
        if (k != AGK_PATH) continue;
        if (got >= AGT_OWNS_MAX) { *why = "At most 8 paths."; return false; }
        if (!agentctl_team_path(cwd, (const char *)d, len, owns[got], AGT_PATH_MAX)) {
            *why = "A path is not usable.";
            return false;
        }
        canon_path(owns[got], AGT_PATH_MAX);
        if (!strcmp(owns[got], "/")) { *why = "A path is not usable."; return false; }
        got++;
    }
    *n = got;
    return true;
}

/* Under g_mu: AGOP_TEAM, POST, INBOX and OWNS for `me` in `owner`'s team. */
/* ------------------------------------------------- the lead's activity --- */

/* Under g_mu: the file beside the socket that tells the tool hooks of a
 * pane's agent that the pane leads a team; a pane that does not pays for no
 * connection. */
/* The panes that lead a team, for the main thread to ask without a lock
 * (agentctl_pane_leads). */
static _Atomic u64 g_lead_panes[AGT_LEADS];

static void lead_mark(u64 owner, bool on) {
    if (!owner) return;
    i32 have = -1, free_slot = -1;
    for (i32 i = 0; i < AGT_LEADS; i++) {
        u64 cur = atomic_load_explicit(&g_lead_panes[i], memory_order_relaxed);
        if (cur == owner && have < 0) have = i;
        if (cur == 0 && free_slot < 0) free_slot = i;
    }
    if (!on && have >= 0) atomic_store_explicit(&g_lead_panes[have], 0, memory_order_release);
    if (on && have < 0 && free_slot >= 0) atomic_store_explicit(&g_lead_panes[free_slot], owner, memory_order_release);
    if (!g_sock_path[0]) return;
    char p[320];
    int w = snprintf(p, sizeof p, "%s.lead-%u", g_sock_path, (u32)owner);
    if (w <= 0 || (usize)w >= sizeof p) return;
    if (!on) { unlink(p); return; }
    int fd = open(p, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
}

/* When each pane's agent was at work, as the tab's own reading of it says
 * (agentctl_pane_busy): the lead of a team whose agent has no tool hooks is
 * credited with a change no job made while it worked. */
#define AGT_BUSY_PANES 32
typedef struct {
    u64 pane;
    i64 since_ns, until_ns;           /* the current or last stretch; until 0 = still at work */
    i64 prev_since_ns, prev_until_ns; /* the one before */
} PaneBusy;
static PaneBusy g_busy[AGT_BUSY_PANES];

/* Under g_mu: whether `owner`'s lead is at work, or was a moment ago: a tool
 * call its hooks reported, or its pane's agent busy. */
static bool lead_at_work(u64 owner) {
    i64 now = now_ns(), tail = (i64)AGT_WATCH_TAIL_MS * 1000000LL;
    const Lead *l = lead_of(owner, false);
    for (i32 i = 0; l && i < AGT_CMD_WINS; i++)
        if (l->wins[i].start_ns && (!l->wins[i].end_ns || now - l->wins[i].end_ns < tail)) return true;
    for (i32 i = 0; i < AGT_BUSY_PANES; i++) {
        const PaneBusy *b = &g_busy[i];
        if (b->pane == owner && b->since_ns && (!b->until_ns || now - b->until_ns < tail)) return true;
    }
    return false;
}

/* Under g_mu: whether the lead's agent reports its tools: a hook spoke in
 * the stretch of work that is going on (or the last one) — another agent
 * started in the pane since has no hooks of its own — or, where the pane's
 * work is not known, in the last 30 s. */
static bool lead_hooked(u64 pane, const Lead *l, i64 now) {
    if (!l || !l->hooked_ns) return false;
    for (i32 i = 0; i < AGT_BUSY_PANES; i++)
        if (g_busy[i].pane == pane && g_busy[i].since_ns) return l->hooked_ns >= g_busy[i].since_ns;
    return now - l->hooked_ns < 30000000000LL;
}

static bool pane_busy_at(u64 pane, i64 t, i64 slack, i64 now) {
    for (i32 i = 0; i < AGT_BUSY_PANES; i++) {
        const PaneBusy *b = &g_busy[i];
        if (b->pane != pane) continue;
        if (b->since_ns && t >= b->since_ns - slack && t <= (b->until_ns ? b->until_ns : now) + slack) return true;
        if (b->prev_since_ns && t >= b->prev_since_ns - slack && t <= b->prev_until_ns + slack) return true;
        return false;
    }
    return false;
}

/* --------------------------------------------- the folders' changes --- */

/* Every folder a job works in is watched (agentctl_watch.h). A change is
 * weighed AGT_WATCH_DELAY_MS after it happened, once the tool that made it
 * has had time to say so, and credited to:
 *   - nobody new, when a member already has it with this very status time
 *     (its tool named it, or its runner saw its command name it);
 *   - the one member whose command or tool call was running then (a job's
 *     commands from its runner, the lead's tool calls from its hooks);
 *   - of several, the one whose line names the file, else the one that owns
 *     the path; still several, nobody;
 *   - with none running, the lead, if its agent reports no tools and was at
 *     work then.
 * Folders and files nobody edits by hand (.git, node_modules, caches, editor
 * swap files) are not weighed, nor a file made and gone again before it was
 * weighed (an atomic write's temporary file). */
#define AGT_WATCH_DELAY_MS 800
#define AGT_WATCH_PENDING  2048
#define AGT_WATCH_CLASHES  5            /* notices one batch raises before it sums up the rest */
#define AGT_WATCH_WAIT_MS  5000         /* how long the lead's screen may take to name a change */

typedef struct {
    char *path;
    u32   flags;
    i64   at_ns;        /* the newest event for it */
    i64   first_ns;     /* the first: a change nobody can be told for yet waits AGT_WATCH_WAIT_MS from it */
} WatchEvent;

static WatchEvent g_wev[AGT_WATCH_PENDING];   /* on the watch queue only */
static u32        g_wev_n;
static bool       g_wev_armed;
static bool       g_watching;                 /* the module is up */
static char       g_watch_dirs[AGENTCTL_WATCH_MAX][AGT_PATH_MAX];   /* under g_mu */
static u32        g_watch_ndirs;

/* Paths the watch saw to exist, by hash, the oldest giving way (the watch
 * queue only). */
#define AGT_WATCH_KNOWN 4096
static u64 g_known[AGT_WATCH_KNOWN];
static u32 g_known_next;

static u64 path_hash(const char *p) {
    u64 h = 1469598103934665603ULL;
    for (; *p; p++) h = (h ^ (u8)*p) * 1099511628211ULL;
    return h ? h : 1;
}

static void known_add(const char *path) {
    u64 h = path_hash(path);
    for (u32 i = 0; i < AGT_WATCH_KNOWN; i++) if (g_known[i] == h) return;
    g_known[g_known_next] = h;
    g_known_next = (g_known_next + 1) % AGT_WATCH_KNOWN;
}

static bool known_has(const char *path) {
    u64 h = path_hash(path);
    for (u32 i = 0; i < AGT_WATCH_KNOWN; i++) if (g_known[i] == h) return true;
    return false;
}

static bool watch_ignored(const char *path) {
    static const char *const names[] = {
        ".git", "node_modules", "__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache", ".next",
        ".turbo", ".parcel-cache", ".gradle", "DerivedData", ".venv", ".idea", ".DS_Store", "4913",
        "Library", ".cache", ".Trash",
    };
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/');
        usize len = e ? (usize)(e - p) : strlen(p);
        for (usize i = 0; i < sizeof names / sizeof names[0]; i++)
            if (strlen(names[i]) == len && !strncmp(p, names[i], len)) return true;
        if (!e) {
            /* the file itself: an editor's swap, backup or lock file */
            if ((len > 1 && p[len - 1] == '~') || !strncmp(p, ".#", 2)) return true;
            if (len > 4 && (!strncmp(p + len - 4, ".swp", 4) || !strncmp(p + len - 4, ".swx", 4) ||
                            !strncmp(p + len - 4, ".swo", 4) || !strncmp(p + len - 4, ".pyc", 4)))
                return true;
        }
        p += len;
    }
    return false;
}

/* A folder is watched while a job of it works, and a little after its turn
 * ends (its last writes are weighed AGT_WATCH_DELAY_MS after they happen):
 * an idle or finished job's folder costs nothing. */
static bool lead_at_work(u64 owner);

static bool watching_job(const Job *j) {
    if (!j->id || !job_shown(j) || j->cwd[0] != '/') return false;
    if (j->state == AGJ_STARTING || j->state == AGJ_RUNNING) return true;
    if (j->state == AGJ_IDLE && j->turn_end_ms && now_ms() - j->turn_end_ms < AGT_WATCH_TAIL_MS) return true;
    /* its lead at work: what the lead's script writes into the job's paths */
    return (j->state == AGJ_IDLE || j->state == AGJ_QUEUED) && lead_at_work(j->owner_serial);
}

/* Under g_mu: ms until the first watched tail ends, -1 for none: the server
 * wakes then to let the folder go. */
static i64 watch_tail_left(void) {
    i64 best = -1, now = now_ms();
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        const Job *j = &g_jobs[i];
        if (!j->id || j->state != AGJ_IDLE || !j->turn_end_ms || !job_shown(j)) continue;
        i64 left = j->turn_end_ms + AGT_WATCH_TAIL_MS - now;
        if (left > 0 && (best < 0 || left < best)) best = left;
    }
    return best;
}

/* Under g_mu: the folders watched follow the jobs. */
static void watch_sync(void) {
    if (!g_watching) return;
    char dirs[AGENTCTL_WATCH_MAX][AGT_PATH_MAX];
    u32 n = 0;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS && n < AGENTCTL_WATCH_MAX; i++) {
        const Job *j = &g_jobs[i];
        if (!watching_job(j)) continue;
        bool have = false;
        for (u32 k = 0; k < n && !have; k++) have = !strcmp(dirs[k], j->cwd);
        if (!have) snprintf(dirs[n++], AGT_PATH_MAX, "%s", j->cwd);
    }
    bool same = n == g_watch_ndirs;
    for (u32 k = 0; k < n && same; k++) {
        bool found = false;
        for (u32 m = 0; m < g_watch_ndirs && !found; m++) found = !strcmp(dirs[k], g_watch_dirs[m]);
        same = found;
    }
    if (same) return;
    const char *ptr[AGENTCTL_WATCH_MAX];
    for (u32 k = 0; k < n; k++) { memcpy(g_watch_dirs[k], dirs[k], AGT_PATH_MAX); ptr[k] = g_watch_dirs[k]; }
    g_watch_ndirs = n;
    agentctl_watch_folders(ptr, n);
}

/* Whether `cmd` names the file at `path`: its name as a word of the line,
 * not a part of a longer name or of a folder's. */
static bool line_names_file(const char *cmd, const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    usize bl = strlen(base);
    if (!cmd || !bl) return false;
    for (const char *at = strstr(cmd, base); at; at = strstr(at + 1, base)) {
        char before = at > cmd ? at[-1] : ' ', after = at[bl];
        bool word_before = (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') ||
                           (before >= '0' && before <= '9') || before == '.' || before == '_' || before == '-';
        bool word_after = (after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
                          (after >= '0' && after <= '9') || after == '.' || after == '_' || after == '-' || after == '/';
        if (!word_before && !word_after) return true;
    }
    return false;
}

/* Under g_mu: whether member `who` of `owner`'s team owns `path`. */
static bool member_owns(u64 owner, u32 who, const char *path) {
    if (who == AGT_LEAD) {
        const Lead *l = lead_of(owner, false);
        for (u8 k = 0; l && k < l->nowns; k++) if (agentctl_team_under(path, l->owns[k])) return true;
        return false;
    }
    const Job *j = job_find(who);
    for (u8 k = 0; j && k < j->nowns; k++) if (agentctl_team_under(path, j->owns[k])) return true;
    return false;
}

/* Under g_mu: whether some member already has `path` at status time `t`. */
static bool credited_at(const char *path, i64 t) {
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        i32 k = g_jobs[i].id ? changes_find(&g_jobs[i].ch, path) : -1;
        if (k >= 0 && g_jobs[i].ch.ns[k] == t) return true;
    }
    for (i32 i = 0; i < AGT_LEADS; i++) {
        i32 k = g_leads[i].owner ? changes_find(&g_leads[i].ch, path) : -1;
        if (k >= 0 && g_leads[i].ch.ns[k] == t) return true;
    }
    return false;
}

/* Under g_mu: whether the lead's screen showed the name of `path` anew
 * around `t` (from 20 s before to 1.5 s after: an agent shows a tool call
 * before it runs and its result right after). */
static bool lead_screen_names(const Lead *l, const char *path, i64 t) {
    if (!l) return false;
    const char *base = strrchr(path, '/');
    u64 h = path_hash(base ? base + 1 : path);
    for (i32 i = 0; i < AGT_LEAD_NAMES; i++)
        if (l->names[i].hash == h)
            return l->names[i].rise_ns >= t - 20000000000LL && l->names[i].rise_ns <= t + 1500000000LL;
    return false;
}

typedef enum { CREDIT_NONE = 0, CREDIT_DONE, CREDIT_LATER } CreditOutcome;

/* Under g_mu: `path` of `owner`'s folder changed at `t`: to whom.
 *
 * The members who may have made it: each job whose command was running then
 * (its runner says when), and the lead — exactly, while a tool call its hooks
 * reported was running, or loosely, while its pane's agent worked (an agent
 * without tool hooks). Of those, the one that NAMED the file — a job's
 * command line, the lead's tool call or its screen, where every agent shows
 * the file it edits — then the one that owns it. Named by none: a job alone
 * (a script, a build; the lead's own edit would be on its screen), or the
 * lead alone. Otherwise nobody: a wrong name in a warning is worse than none.
 * CREDIT_LATER: nobody yet, but the lead was at work and its screen may still
 * show the name. */
static CreditOutcome watch_credit(u64 owner, const char *path, i64 t, i64 slack, i64 now, const char *root,
                                  bool *told) {
    u32 cand[AGENTCTL_MAX_JOBS + 1];
    const CmdWin *cw[AGENTCTL_MAX_JOBS + 1];
    i32 nc = 0, njobs = 0;
    Lead *l = lead_of(owner, false);
    bool hooked = lead_hooked(owner, l, now);
    bool lead_exact = false, lead_loose = false;
    /* the windows that hold the moment itself first; the slack only when
     * none does (commands of two members can run a few ms apart) */
    for (i32 pass = 0; pass < 2 && nc == 0; pass++) {
        i64 sl = pass == 0 ? 0 : slack;
        for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
            const Job *j = &g_jobs[i];
            if (j->owner_serial != owner || !watching_job(j) || !agentctl_team_under(path, j->cwd)) continue;
            const CmdWin *w = wins_at(j->wins, t, sl, now);
            if (w) { cand[nc] = j->id; cw[nc] = w; nc++; }
        }
        njobs = nc;
        if (hooked) {
            const CmdWin *w = wins_at(l->wins, t, sl, now);
            if (w) { cand[nc] = AGT_LEAD; cw[nc] = w; nc++; lead_exact = true; }
        }
    }
    if (!hooked && pane_busy_at(owner, t, slack, now)) lead_loose = true;
    bool lead_at_work = lead_exact || lead_loose;

    /* who named it */
    i32 named = -1, nnamed = 0;
    bool lead_named = false;
    for (i32 i = 0; i < njobs; i++) if (line_names_file(cw[i]->cmd, path)) { named = i; nnamed++; }
    if (lead_at_work && ((lead_exact && line_names_file(cw[nc - 1]->cmd, path)) || lead_screen_names(l, path, t))) {
        lead_named = true;
        nnamed++;
    }
    u32 who = 0;
    bool found = false;
    if (nnamed == 1) {
        who = lead_named ? AGT_LEAD : cand[named];
        found = true;
    } else if (nnamed > 1) {
        /* named by more than one: the one that owns it */
        i32 hits = 0;
        for (i32 i = 0; i < njobs; i++)
            if (line_names_file(cw[i]->cmd, path) && member_owns(owner, cand[i], path)) { who = cand[i]; hits++; }
        if (lead_named && member_owns(owner, AGT_LEAD, path)) { who = AGT_LEAD; hits++; }
        found = hits == 1;
    } else if (njobs == 1 && !lead_exact) {
        who = cand[0];                           /* a job's script or build */
        found = true;
    } else if (njobs == 0 && lead_at_work) {
        who = AGT_LEAD;
        found = true;
    } else if (njobs + (lead_exact ? 1 : 0) > 1) {
        i32 hits = 0;
        for (i32 i = 0; i < njobs; i++) if (member_owns(owner, cand[i], path)) { who = cand[i]; hits++; }
        if (lead_exact && member_owns(owner, AGT_LEAD, path)) { who = AGT_LEAD; hits++; }
        found = hits == 1;
    }
    if (!found) return lead_at_work ? CREDIT_LATER : CREDIT_NONE;
    if (who == AGT_LEAD && !lead_of(owner, true)) return CREDIT_NONE;
    const char *cwd = root;
    if (who == AGT_LEAD) { l = lead_of(owner, false); if (l && l->cwd[0] == '/') cwd = l->cwd; }
    else { const Job *j = job_find(who); if (j) cwd = j->cwd; }
    *told |= member_touch(owner, who, cwd, path, strlen(path), true);
    return CREDIT_DONE;
}

/* Under g_mu: one changed path, against every team working in a folder
 * that holds it. */
static CreditOutcome watch_judge(const char *path, u32 flags, i64 at, i64 now, bool *told, u64 *last_owner) {
    struct stat st;
    bool exists = lstat(path, &st) == 0;
    if (exists ? S_ISDIR(st.st_mode) : (flags & AGW_DIR) != 0) return CREDIT_NONE;
    /* made and gone again without ever being seen: a temporary file (an
     * atomic write's, a lock), not a change anyone made to the folder. The
     * system can carry a file's "made" mark into the event of its removal
     * long after, so a file seen to exist before is removed for real. */
    if (exists) known_add(path);
    else if ((flags & AGW_CREATED) && !known_has(path)) return CREDIT_NONE;
    i64 t, slack;
    if (exists) {
#ifdef __APPLE__
        t = (i64)st.st_ctimespec.tv_sec * 1000000000LL + st.st_ctimespec.tv_nsec;
#else
        t = (i64)st.st_ctim.tv_sec * 1000000000LL + st.st_ctim.tv_nsec;
#endif
        slack = 100000000LL;
        if (credited_at(path, t)) return CREDIT_DONE;
    } else {
        t = at - 150000000LL;                  /* a removal has no status time: when it was heard of */
        slack = 500000000LL;
    }
    CreditOutcome out = CREDIT_NONE;
    u64 done[AGENTCTL_MAX_JOBS];
    i32 ndone = 0;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        const Job *j = &g_jobs[i];
        if (!watching_job(j) || !agentctl_team_under(path, j->cwd)) continue;
        bool seen_owner = false;
        for (i32 k = 0; k < ndone && !seen_owner; k++) seen_owner = done[k] == j->owner_serial;
        if (seen_owner) continue;
        done[ndone++] = j->owner_serial;
        CreditOutcome c = watch_credit(j->owner_serial, path, t, slack, now, j->cwd, told);
        if (c == CREDIT_DONE) *last_owner = j->owner_serial;
        if (c > out) out = c;
    }
    return out;
}

static void watch_resolve(void *ctx);

static void watch_cb(void *ctx, const char *const *paths, const u32 *flags, usize n) {
    (void)ctx;
    i64 now = now_ns();
    for (usize i = 0; i < n; i++) {
        if ((flags[i] & AGW_LOST) || !paths[i] || paths[i][0] != '/' || watch_ignored(paths[i])) continue;
        i32 at = -1;
        for (u32 k = 0; k < g_wev_n && at < 0; k++) if (!strcmp(g_wev[k].path, paths[i])) at = (i32)k;
        if (at >= 0) { g_wev[at].flags |= flags[i]; g_wev[at].at_ns = now; continue; }
        if (g_wev_n >= AGT_WATCH_PENDING) continue;
        char *copy = strdup(paths[i]);
        if (!copy) continue;
        g_wev[g_wev_n++] = (WatchEvent){ copy, flags[i], now, now };
    }
    if (g_wev_n && !g_wev_armed) {
        g_wev_armed = true;
        agentctl_watch_later(AGT_WATCH_DELAY_MS, watch_resolve, NULL);
    }
}

static void watch_resolve(void *ctx) {
    (void)ctx;
    g_wev_armed = false;
    i64 now = now_ns();
    bool told = false, any = false;
    u64 last_owner = 0;
    u32 keep = 0;
    pthread_mutex_lock(&g_mu);
    g_clash_budget = AGT_WATCH_CLASHES;
    g_clash_held = 0;
    for (u32 i = 0; i < g_wev_n; i++) {
        WatchEvent *e = &g_wev[i];
        if (now - e->at_ns < (i64)(AGT_WATCH_DELAY_MS - 50) * 1000000LL) { g_wev[keep++] = *e; continue; }
        CreditOutcome c = watch_judge(e->path, e->flags, e->at_ns, now, &told, &last_owner);
        if (c == CREDIT_DONE) any = true;
        if (c == CREDIT_LATER && now - e->first_ns < (i64)AGT_WATCH_WAIT_MS * 1000000LL) {
            e->at_ns = now - (i64)(AGT_WATCH_DELAY_MS / 2) * 1000000LL;    /* weighed again in a moment */
            g_wev[keep++] = *e;
            continue;
        }
        free(e->path);
    }
    g_wev_n = keep;
    if (g_clash_held && last_owner) {
        char msg[160];
        snprintf(msg, sizeof msg, "%u more files were changed by more than one member just now; see `liu-agent team`.",
                 g_clash_held);
        post_add(last_owner, AGT_LIU, AGT_ALL, msg, strlen(msg));
    }
    g_clash_budget = -1;
    pthread_mutex_unlock(&g_mu);
    if (g_wev_n) {
        g_wev_armed = true;
        agentctl_watch_later(AGT_WATCH_DELAY_MS / 2, watch_resolve, NULL);
    }
    if (any) kick_server();                 /* the team files the server writes */
    if (told || any) wake_main();
}

/* Under g_mu: who a post's `--to` names in `owner`'s team — all, lead, a job
 * number, or one job's label or agent ("alpha", "codex") when only one job
 * answers to it. */
static bool team_resolve_to(u64 owner, const char *s, u32 *to) {
    if (agentctl_team_parse_to(s, to)) return true;
    if (!s || !s[0]) return false;
    u32 by_label = 0, by_agent = 0;
    i32 labels = 0, agents = 0;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        const Job *o = &g_jobs[i];
        if (!o->id || o->owner_serial != owner || !job_shown(o)) continue;
        if (!strcasecmp(o->label, s)) { by_label = o->id; labels++; }
        const AgentctlAgentInfo *ai = agentctl_agent_info(o->agent);
        if (ai && !strcasecmp(ai->id, s)) { by_agent = o->id; agents++; }
    }
    if (labels == 1) { *to = by_label; return true; }
    if (labels == 0 && agents == 1) { *to = by_agent; return true; }
    return false;
}

static bool team_request(Conn *c, const AgentctlFrame *f, u64 owner, u32 me) {
    char root[1024];
    if (!agentctl_get_str(f, AGK_CWD, root, sizeof root) || root[0] != '/') {
        const Job *mj = me != AGT_LEAD ? job_find(me) : NULL;
        snprintf(root, sizeof root, "%s", mj ? mj->cwd : "");
    }
    canon_path(root, sizeof root);
    if (me == AGT_LEAD && root[0] == '/') {
        Lead *ml = lead_of(owner, team_exists(owner));
        if (ml) snprintf(ml->cwd, sizeof ml->cwd, "%s", root);
    }
    switch (f->op) {
    case AGOP_TEAM: {
        char *text = team_listing(owner, me, root);
        AgentctlMsg m;
        if (agentctl_msg_init(&m, AGOP_REPLY, 0)) {
            agentctl_msg_put_u64(&m, AGK_CODE, text ? AGC_OK : AGC_ERROR);
            if (text) agentctl_msg_put_str(&m, AGK_TEAM, text);
            if (agentctl_msg_end(&m)) conn_queue(c, &m);
            agentctl_msg_free(&m);
        }
        free(text);
        return false;
    }
    case AGOP_POST: {
        if (!team_exists(owner)) {
            conn_reply(c, 0, AGC_REFUSED, "No team yet: it forms when an agent in this pane starts another.");
            return false;
        }
        const u8 *d; u32 n;
        if (!agentctl_get(f, AGK_TEXT, &d, &n) || n == 0 || memchr(d, '\0', n)) {
            conn_reply(c, 0, AGC_REFUSED, "The message is empty.");
            return false;
        }
        if (n > AGT_POST_MAX - 1) {
            conn_reply(c, 0, AGC_REFUSED, "A post holds at most 2 KB; put more in a file and post its path.");
            return false;
        }
        char to_s[64];
        u32 to = AGT_ALL;
        if (agentctl_get_str(f, AGK_TO, to_s, sizeof to_s) && !team_resolve_to(owner, to_s, &to)) {
            conn_reply(c, 0, AGC_REFUSED, "--to takes all, lead, a job number, or the label of one job in this team.");
            return false;
        }
        if (to != AGT_ALL && to != AGT_LEAD) {
            const Job *tj = job_find(to);
            if (!tj || tj->owner_serial != owner || !job_shown(tj)) {
                conn_reply(c, 0, AGC_NOTFOUND, "No such job in this team.");
                return false;
            }
        }
        if (to == me) { conn_reply(c, 0, AGC_REFUSED, "That is you."); return false; }
        if (!post_add(owner, me, to, (const char *)d, n)) {
            conn_reply(c, 0, AGC_REFUSED, "The message is empty.");
            return false;
        }
        Job *mj = me != AGT_LEAD ? job_find(me) : NULL;
        if (mj) {
            char who[160], line[AGT_POST_MAX + 200];
            member_name(owner, to, who, sizeof who);
            const Post *p = &g_posts[(g_post_next + AGT_POSTS - 1) % AGT_POSTS];
            snprintf(line, sizeof line, "\xE2\x9C\x89 to %s: %s", who, p->text);
            agentctl_ring_push(&mj->ring, AGS_DIM, (const u8 *)line, (u32)strlen(line));
        }
        conn_reply(c, 0, AGC_OK, NULL);
        return false;
    }
    case AGOP_INBOX: {
        u64 after = 0;
        if (me == AGT_LEAD) { Lead *l = lead_of(owner, true); after = l ? l->seen : g_post_seq; }
        else { const Job *mj = job_find(me); after = mj ? mj->board_seen : g_post_seq; }
        bool any = false;
        for (i32 k = 0; k < AGT_POSTS && !any; k++)
            any = post_for(&g_posts[k], owner, me) && g_posts[k].seq > after;
        char block[4];
        if (!any && agentctl_get_str(f, AGK_BLOCK, block, sizeof block) && block[0] == '1') {
            c->wait_kind = WAIT_INBOX;
            c->inbox_owner = owner;
            c->inbox_me = me;
            return false;   /* service_locked answers it once there is a post */
        }
        TeamScratch *ts = calloc(1, sizeof *ts);
        char *out = malloc(AGT_LISTING_MAX);
        AgentctlMsg m;
        if (ts && out && agentctl_msg_init(&m, AGOP_REPLY, 0)) {
            team_posts(ts, owner, me, after, true, AGT_LIST_POSTS);
            AgtText t;
            agt_text_init(&t, out, AGT_LISTING_MAX);
            if (ts->np == 0) agt_line(&t, "No new messages.");
            for (i32 i = 0; i < ts->np; i++) agt_line(&t, "- %s \xE2\x86\x92 %s: %s", ts->p[i].from, ts->p[i].to, ts->p[i].text);
            agt_text_finish(&t, "(more: `liu-agent team`)");
            team_mark_read(owner, me);
            agentctl_msg_put_u64(&m, AGK_CODE, AGC_OK);
            agentctl_msg_put_str(&m, AGK_TEAM, out);
            if (agentctl_msg_end(&m)) conn_queue(c, &m);
            agentctl_msg_free(&m);
        } else {
            conn_reply(c, 0, AGC_ERROR, "out of memory");
        }
        free(ts);
        free(out);
        return false;
    }
    case AGOP_OWNS: {
        if (!root[0]) { conn_reply(c, 0, AGC_REFUSED, "The folder is not known."); return false; }
        char (*owns)[AGT_PATH_MAX] = calloc(AGT_OWNS_MAX, sizeof *owns);
        if (!owns) { conn_reply(c, 0, AGC_ERROR, "out of memory"); return false; }
        u8 n = 0;
        const char *why = NULL;
        if (!owns_from(f, root, owns, &n, &why)) { free(owns); conn_reply(c, 0, AGC_REFUSED, why); return false; }
        char (*dst)[AGT_PATH_MAX] = NULL;
        u8 *dn = NULL;
        if (me == AGT_LEAD) {
            Lead *l = lead_of(owner, true);
            if (l) { dst = l->owns; dn = &l->nowns; }
        } else {
            Job *mj = job_find(me);
            if (mj) { dst = mj->owns; dn = &mj->nowns; }
        }
        if (!dst) { free(owns); conn_reply(c, 0, AGC_ERROR, "no room for the team"); return false; }
        memcpy(dst, owns, sizeof(char[AGT_PATH_MAX]) * n);
        *dn = n;
        free(owns);
        /* the team hears it in the member's own name, so it is not news to
         * the member itself */
        char list[512], msg[AGT_POST_MAX], said[AGT_POST_MAX];
        owns_text(dst, n, root, list, sizeof list);
        if (n) snprintf(msg, sizeof msg, "My paths now: %s.", list);
        else   snprintf(msg, sizeof msg, "I own no path now.");
        bool told = team_exists(owner) && post_add(owner, me, AGT_ALL, msg, strlen(msg));
        g_team_files_dirty = true;
        const char *tail = told ? "; the team is told." : ".";
        if (n) snprintf(said, sizeof said, "You own %s%s", list, tail);
        else   snprintf(said, sizeof said, "You own no path now%s", tail);
        conn_reply(c, 0, AGC_OK, said);
        return false;
    }
    default:
        return false;
    }
}

/* Under g_mu: every job's team.md, for an agent whose sandbox keeps it from
 * Liu's socket (it can read the file). Filled here, written by the caller
 * outside the lock. */
typedef struct { char path[600]; char *text; } TeamFile;

static i32 team_files_take(TeamFile *out, i32 cap) {
    if (!g_team_files_dirty) return 0;
    g_team_files_dirty = false;
    i32 n = 0;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS && n < cap; i++) {
        Job *j = &g_jobs[i];
        if (!j->dir[0] || !job_shown(j) || j->state == AGJ_ENDED || j->state == AGJ_PENDING_APPROVAL) continue;
        if ((usize)snprintf(out[n].path, sizeof out[n].path, "%s/team.md", j->dir) >= sizeof out[n].path) continue;
        TeamScratch *ts = calloc(1, sizeof *ts);
        char *text = malloc(AGT_LISTING_MAX);
        if (!ts || !text) { free(ts); free(text); continue; }
        team_members(ts, j->owner_serial, j->id, j->cwd);
        team_posts(ts, j->owner_serial, j->id, 0, false, AGT_LIST_POSTS);
        agentctl_team_listing(text, AGT_LISTING_MAX, j->cwd, ts->m, ts->n, ts->p, ts->np);
        free(ts);
        out[n++].text = text;
    }
    return n;
}

/* Replaces `path` whole: written beside it, then renamed over it. */
static void team_file_write(const TeamFile *tf) {
    char tmp[620];
    if ((usize)snprintf(tmp, sizeof tmp, "%s.tmp", tf->path) >= sizeof tmp) return;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    usize len = strlen(tf->text);
    const char *p = tf->text;
    bool ok = true;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
        p += w;
        len -= (usize)w;
    }
    if (close(fd) != 0) ok = false;
    if (ok) rename(tmp, tf->path);
    else    unlink(tmp);
}

/* ------------------------------------------------------------- service --- */

/* Under g_mu: answers every waiter whose jobs have settled and hands every
 * runner what is queued for it; the folders watched follow the jobs. */
static void service_locked(void) {
    watch_sync();
    for (i32 i = 0; i < AGENTCTL_MAX_CONNS; i++) {
        Conn *c = &g_conns[i];
        if (c->fd < 0 || c->wait_kind == WAIT_NONE) continue;
        if (c->wait_kind == WAIT_INBOX) {
            /* answered as a fresh AGOP_INBOX would be, once a post is there */
            u64 after = 0;
            if (c->inbox_me == AGT_LEAD) { Lead *l = lead_of(c->inbox_owner, false); after = l ? l->seen : 0; }
            else { const Job *mj = job_find(c->inbox_me); after = mj ? mj->board_seen : g_post_seq; }
            bool any = false;
            for (i32 k = 0; k < AGT_POSTS && !any; k++)
                any = post_for(&g_posts[k], c->inbox_owner, c->inbox_me) && g_posts[k].seq > after;
            if (!any) continue;
            c->wait_kind = WAIT_NONE;
            AgentctlFrame ask = { AGOP_INBOX, 0, NULL, 0 };
            team_request(c, &ask, c->inbox_owner, c->inbox_me);
            continue;
        }
        if (c->wait_kind == WAIT_APPROVAL) {
            Job *j = job_find(c->wait_jobs[0]);
            if (!j) { conn_reply(c, c->wait_jobs[0], AGC_NOTFOUND, "the job is gone"); c->wait_kind = WAIT_NONE; continue; }
            if (j->state == AGJ_PENDING_APPROVAL) continue;
            if (j->state == AGJ_DECLINED) reply_job(c, j, AGC_DECLINED, j->error[0] ? j->error : "declined");
            else                          reply_job(c, j, AGC_OK, NULL);
            c->wait_kind = WAIT_NONE;
            continue;
        }
        u32 settled[AGENTCTL_MAX_WAIT];
        u8 n_settled = 0;
        bool all = true;
        for (u8 k = 0; k < c->wait_count; k++) {
            Job *j = job_find(c->wait_jobs[k]);
            if (!j || agentctl_state_settled(j->state)) settled[n_settled++] = c->wait_jobs[k];
            else all = false;
        }
        if ((c->wait_any && n_settled > 0) || all) {
            AgentctlMsg m;
            if (agentctl_msg_init(&m, AGOP_REPLY, 0)) {
                agentctl_msg_put_u64(&m, AGK_CODE, AGC_OK);
                for (u8 k = 0; k < n_settled; k++) agentctl_msg_put_u64(&m, AGK_JOB, settled[k]);
                if (agentctl_msg_end(&m)) conn_queue(c, &m);
                agentctl_msg_free(&m);
            }
            c->wait_kind = WAIT_NONE;
        }
    }
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        Job *j = &g_jobs[i];
        if (!j->id || j->runner < 0) continue;
        Conn *c = &g_conns[j->runner];
        if (c->fd < 0) continue;
        if (j->send_turn && j->prompt) {
            AgentctlMsg m;
            if (agentctl_msg_init(&m, AGOP_S_TURN, j->id)) {
                agentctl_msg_put(&m, AGK_PROMPT, j->prompt, j->prompt_len);
                if (j->budget_micro)
                    agentctl_msg_put_u64(&m, AGK_BUDGET, j->spent_cost >= j->budget_micro ? 1 : j->budget_micro - j->spent_cost);
                char *notes = team_notes_for(j);
                if (notes) agentctl_msg_put_str(&m, AGK_TEAM, notes);
                free(notes);
                if (agentctl_msg_end(&m)) conn_queue(c, &m);
                agentctl_msg_free(&m);
            }
            free(j->prompt);
            j->prompt = NULL;
            j->prompt_len = 0;
            j->send_turn = false;
        }
        if (j->send_stop) {
            AgentctlMsg m;
            if (agentctl_msg_init(&m, AGOP_S_STOP, j->id) && agentctl_msg_end(&m)) conn_queue(c, &m);
            agentctl_msg_free(&m);
            j->send_stop = false;
        }
        if (j->send_close) {
            AgentctlMsg m;
            if (agentctl_msg_init(&m, AGOP_S_CLOSE, j->id) && agentctl_msg_end(&m)) conn_queue(c, &m);
            agentctl_msg_free(&m);
            j->send_close = false;
        }
        if (j->answer_req) {
            AgentctlMsg m;
            if (agentctl_msg_init(&m, AGOP_S_ANSWER, j->id)) {
                agentctl_msg_put_str(&m, AGK_REQ, j->ask_req);
                agentctl_msg_put_str(&m, AGK_DECISION, j->answer_dec);
                if (agentctl_msg_end(&m)) conn_queue(c, &m);
                agentctl_msg_free(&m);
            }
            j->answer_req = false;
            j->asking = false;
            j->ask_req[0] = j->ask_tool[0] = j->ask_what[0] = '\0';
        }
        if (j->merge_req) {
            AgentctlMsg m;
            if (agentctl_msg_init(&m, AGOP_S_MERGE, j->id)) {
                agentctl_msg_put_str(&m, AGK_DECISION, j->merge_dec);
                if (agentctl_msg_end(&m)) conn_queue(c, &m);
                agentctl_msg_free(&m);
            }
            j->merge_req = false;
            j->merging = true;
        }
    }
}

/* ------------------------------------------------------------- runners --- */

/* The environment a runner starts with: this process's, without anything
 * that names a pane, another job or another Liu, plus the job's own. Built
 * once at start (the main thread may setenv later; a runner never sees a
 * half-written variable). */
static char **runner_env_snapshot(void) {
    usize n = 0;
    while (environ && environ[n]) n++;
    char **e = calloc(n + 4, sizeof *e);
    if (!e) return NULL;
    usize k = 0;
    static const char *const drop[] = { "LIU_PANE_ID=", "LIU_AGENT_SOCK=", "LIU_AGENT_JOB=",
                                        "LIU_RESUME_COMMAND=", "LIU_MARK_KEY=", "LIU_AGENT_HELPERS=",
                                        "LIU_AGENT_TEAM_FILE=" };
    for (usize i = 0; i < n; i++) {
        bool skip = false;
        for (usize d = 0; d < sizeof drop / sizeof drop[0]; d++)
            if (!strncmp(environ[i], drop[d], strlen(drop[d]))) { skip = true; break; }
        if (skip) continue;
        e[k] = strdup(environ[i]);
        if (!e[k]) { for (usize x = 0; x < k; x++) free(e[x]); free(e); return NULL; }
        k++;
    }
    e[k] = NULL;
    return e;
}

/* Starts the runner of job `id`. The runner is this process's child, in a
 * process group of its own, with no terminal and nothing but /dev/null on its
 * standard descriptors; it finds its job by LIU_AGENT_JOB and the socket by
 * LIU_AGENT_SOCK. Returns the pid, or -1. */
static pid_t spawn_runner(u32 id) {
    if (!g_runner_bin[0] || !g_runner_env) return -1;
    usize n = 0;
    while (g_runner_env[n]) n++;
    char job_e[48], sock_e[300], helpers_e[1100];
    snprintf(job_e, sizeof job_e, "%s=%u", AGENTCTL_JOB_ENV, id);
    snprintf(sock_e, sizeof sock_e, "%s=%s", AGENTCTL_SOCK_ENV, g_sock_path);
    snprintf(helpers_e, sizeof helpers_e, "LIU_AGENT_HELPERS=%s", g_helpers);
    char **envp = calloc(n + 4, sizeof *envp);
    if (!envp) return -1;
    memcpy(envp, g_runner_env, n * sizeof *envp);
    usize e = n;
    envp[e++] = job_e;
    envp[e++] = sock_e;
    if (g_helpers[0]) envp[e++] = helpers_e;
    envp[e] = NULL;
    char *argv[] = { (char *)"liu-agent", (char *)"run", NULL };

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setsigmask(&attr, &none);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, g_runner_bin, &fa, &attr, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    free(envp);
    return rc == 0 ? pid : -1;
}

/* Starts the runners of jobs that were let through and collects the ones
 * that went. Returns the poll timeout the loop needs for what is still in
 * flight (-1 = none). */
static int runners_tick(bool *main_work) {
    /* to start: taken under the lock, spawned outside it */
    u32 want[AGENTCTL_MAX_JOBS];
    i32 nwant = 0;
    pthread_mutex_lock(&g_mu);
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        Job *j = &g_jobs[i];
        if (j->id && j->state == AGJ_STARTING && j->runner_pid == 0) want[nwant++] = j->id;
    }
    pthread_mutex_unlock(&g_mu);
    for (i32 k = 0; k < nwant; k++) {
        pid_t pid = spawn_runner(want[k]);
        pthread_mutex_lock(&g_mu);
        Job *j = job_find(want[k]);
        if (j && j->state == AGJ_STARTING && j->runner_pid == 0) {
            if (pid > 0) {
                j->runner_pid = pid;
                j->runner_due_ms = mono_ms() + RUNNER_START_MS;
            } else {
                job_end(j, AGE_COULD_NOT_START, "Liu could not start its runner");
                *main_work = true;
            }
        } else if (pid > 0) {
            kill(pid, SIGTERM);
            Job tmp = { .runner_pid = pid };
            reap_later(&tmp);
        }
        pthread_mutex_unlock(&g_mu);
    }

    int timeout = -1;
    i64 now = mono_ms();
    pthread_mutex_lock(&g_mu);
    /* a runner that has not said hello: gone already, or late */
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        Job *j = &g_jobs[i];
        if (!j->id || j->state != AGJ_STARTING || j->runner_pid <= 0 || j->runner >= 0) continue;
        int st = 0;
        pid_t w = waitpid(j->runner_pid, &st, WNOHANG);
        if (w == j->runner_pid) {
            j->runner_pid = 0;
            job_end(j, AGE_COULD_NOT_START, "its runner exited before it started");
            *main_work = true;
            continue;
        }
        if (now >= j->runner_due_ms) {
            kill(j->runner_pid, SIGKILL);
            job_end(j, AGE_COULD_NOT_START, "its runner did not start in time");
            *main_work = true;
            continue;
        }
        i64 left = j->runner_due_ms - now;
        int t = left > 250 ? 250 : (int)left;
        if (timeout < 0 || t < timeout) timeout = t;
    }
    /* runners told to go */
    for (i32 i = 0; i < g_reap_count; ) {
        Reap *r = &g_reap[i];
        pid_t w = waitpid(r->pid, NULL, WNOHANG);
        if (w == r->pid || (w < 0 && errno == ECHILD)) {
            g_reap[i] = g_reap[--g_reap_count];
            continue;
        }
        if (!r->killed && now >= r->due_ms) {
            kill(r->pid, SIGKILL);
            r->killed = true;
        }
        if (timeout < 0 || timeout > 100) timeout = 100;
        i++;
    }
    /* a job idle for AGT_PARK_MS lets its runner go (2 MB each); the next
     * message starts one that resumes the session */
    {
        i64 wall = now_ms();
        for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
            Job *j = &g_jobs[i];
            if (!j->id || j->state != AGJ_IDLE || j->runner < 0 || j->parked || !j->session[0] ||
                j->send_turn || j->merge_req || j->merging || j->answer_req || !j->turn_end_ms)
                continue;
            i64 left = j->turn_end_ms + AGT_PARK_MS - wall;
            if (left <= 0) {
                j->parked = true;
                j->send_close = true;
                continue;
            }
            if (timeout < 0 || left < timeout) timeout = left > 0x7fffffff ? 0x7fffffff : (int)left;
        }
        i64 tail = watch_tail_left();
        if (tail >= 0 && (timeout < 0 || tail < timeout)) timeout = (int)tail + 10;
    }
    /* a promotion just now: the next pass starts its runner */
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++)
        if (g_jobs[i].id && g_jobs[i].state == AGJ_STARTING && g_jobs[i].runner_pid == 0) { timeout = 0; break; }
    pthread_mutex_unlock(&g_mu);
    return timeout;
}

/* ------------------------------------------------------------ requests --- */

/* Under g_mu: why `j` may run no more turns — the turns, tokens (input not
 * read from a cache, and output) or money it was allowed are spent — or
 * false. */
static bool job_over_limit(const Job *j, char *why, usize cap) {
    u64 fresh = (j->spent_in > j->spent_cached ? j->spent_in - j->spent_cached : 0) + j->spent_out;
    if (j->max_turns && j->turns >= j->max_turns) {
        snprintf(why, cap, "It has run the %u turn%s it was allowed.", j->max_turns, j->max_turns == 1 ? "" : "s");
        return true;
    }
    if (j->max_tokens && fresh >= j->max_tokens) {
        snprintf(why, cap, "It has spent the %llu tokens it was allowed (%llu so far).",
                 (unsigned long long)j->max_tokens, (unsigned long long)fresh);
        return true;
    }
    if (j->budget_micro && j->spent_cost >= j->budget_micro) {
        snprintf(why, cap, "It has spent the $%.2f it was allowed ($%.2f so far).",
                 (double)j->budget_micro / 1e6, (double)j->spent_cost / 1e6);
        return true;
    }
    return false;
}

/* Under g_mu: a turn ended; one that used up what the job may spend is said
 * once, to the lead and in its log. */
static void job_check_limits(Job *j) {
    char why[200];
    if (!job_over_limit(j, why, sizeof why)) return;
    char line[300];
    snprintf(line, sizeof line, "\xE2\x96\xA0 limit reached: %s It takes no more messages.", why);
    agentctl_ring_push(&j->ring, AGS_ERR, (const u8 *)line, (u32)strlen(line));
    char msg[AGT_POST_MAX];
    snprintf(msg, sizeof msg, "Reached its limit: %s It takes no more messages.", why);
    post_add(j->owner_serial, j->id, AGT_LEAD, msg, strlen(msg));
    bump_line_gen();
}

/* Under g_mu: the delegated session list (agentctl_session_delegated) gets
 * `j`'s session the first time it is known. Written outside the lock by the
 * server loop. */
static bool g_delegated_dirty;
static char g_delegated_line[AGENTCTL_MAX_JOBS][512];
static i32  g_delegated_n;
static void delegated_list_add(Job *j) {
    if (j->session_listed || !j->session[0] || !g_state_dir[0] || g_delegated_n >= AGENTCTL_MAX_JOBS) return;
    const AgentctlAgentInfo *ai = agentctl_agent_info(j->agent);
    char label[AGENTCTL_LABEL_CAP], owner[96];
    agentctl_sanitize(j->label, strlen(j->label), label, sizeof label, false);
    agentctl_sanitize(j->owner_label, strlen(j->owner_label), owner, sizeof owner, false);
    for (char *q = label; *q; q++) if (*q == '\t') *q = ' ';
    for (char *q = owner; *q; q++) if (*q == '\t') *q = ' ';
    snprintf(g_delegated_line[g_delegated_n++], sizeof g_delegated_line[0], "%s\t%s\t%s\t%s\t%lld\n",
             j->session, ai ? ai->id : "agent", label, owner, (long long)(now_ms() / 1000));
    j->session_listed = true;
    g_delegated_dirty = true;
}

/* Under g_mu. Returns true when the main thread has work. */
/* The next message of an idle job: its turn is handed to the runner on the
 * server thread's next pass. AGC_OK, or a refusal with `*why` saying it.
 * Under g_mu. */
static u64 job_queue_turn(Job *j, const u8 *prompt, u32 plen, const char **why) {
    if (!prompt || plen == 0 || memchr(prompt, '\0', plen)) {
        *why = "The message is empty.";
        return AGC_REFUSED;
    }
    if (plen > AGENTCTL_MAX_PROMPT) {
        *why = "The message is too long.";
        return AGC_REFUSED;
    }
    if (j->state == AGJ_ENDED || j->state == AGJ_DECLINED) {
        *why = "The job has ended.";
        return AGC_REFUSED;
    }
    char limit[200];
    if (job_over_limit(j, limit, sizeof limit)) {
        static char said[220];
        snprintf(said, sizeof said, "%s", limit);
        *why = said;
        return AGC_REFUSED;
    }
    if (j->merging) {
        *why = "Its work is being merged.";
        return AGC_BUSY;
    }
    /* a job taken over from an earlier Liu has no runner until its next
     * message: one is started, and its first turn resumes the session */
    bool restart = j->state == AGJ_IDLE && j->runner < 0 && j->runner_pid == 0 && (j->restored || j->parked) &&
                   !j->detached_pid;
    if (!restart && (j->state != AGJ_IDLE || j->runner < 0)) {
        *why = "The job is still working; wait for it first.";
        return AGC_BUSY;
    }
    char *copy = malloc(plen + 1);
    if (!copy) { *why = "out of memory"; return AGC_ERROR; }
    memcpy(copy, prompt, plen);
    copy[plen] = '\0';
    free(j->prompt);
    j->prompt = copy;
    j->prompt_len = plen;
    if (restart) {
        j->seq = g_next_seq++;
        j->state = AGJ_QUEUED;
        j->outcome = AGO_NONE;
        j->error[0] = '\0';
        j->restored = false;
        j->parked = false;
        bump_gen();
        promote_queue();
        return AGC_OK;
    }
    j->send_turn = true;
    j->state = AGJ_RUNNING;
    j->outcome = AGO_NONE;
    j->error[0] = '\0';
    j->turn_start_ms = now_ms();
    j->turn_end_ms = 0;
    bump_gen();
    return AGC_OK;
}

/* Under g_mu: the level `pane` may start agents at without asking, -1. */
static i32 trust_of(u64 pane) {
    for (i32 i = 0; i < AGT_TRUST_MAX; i++) if (pane && g_trust[i].pane == pane) return g_trust[i].perm;
    return -1;
}

/* Under g_mu: the agent session `pane`'s agent runs on, "" when unknown. */
static const char *ident_of(u64 pane) {
    for (i32 i = 0; i < AGT_IDENT_MAX; i++) if (pane && g_ident[i].pane == pane) return g_ident[i].session;
    return "";
}

/* The head of a job's last final message, read from its result file. */
static usize result_head(const char *dir, char *out, usize cap) {
    out[0] = '\0';
    if (!dir || !dir[0] || cap < 8) return 0;
    char path[600];
    if ((usize)snprintf(path, sizeof path, "%s/result.md", dir) >= sizeof path) return 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return 0; }
    char *raw = malloc(cap);
    if (!raw) { close(fd); return 0; }
    ssize_t n = read(fd, raw, cap - 1);
    close(fd);
    if (n <= 0) { free(raw); return 0; }
    usize w = agentctl_sanitize(raw, (usize)n, out, cap, true);
    free(raw);
    return w;
}

/* Under g_mu: a review of `t`'s work, put ahead of `extra` (the caller's own
 * words, may be empty): what it was asked, what it said, the files it
 * changed and where to look. malloc'd. */
static char *review_prompt(const Job *t, const u8 *extra, u32 elen, u32 *out_len) {
    usize cap = 24u * 1024u + elen;
    char *p = malloc(cap);
    if (!p) return NULL;
    char name[160], head[6144];
    member_name(t->owner_serial, t->id, name, sizeof name);
    result_head(t->dir, head, sizeof head);
    usize w = 0;
    w += (usize)snprintf(p + w, cap - w,
        "Review the work %s did. Do not change any file: read the code, run what checks the code, and report "
        "concrete problems with the file and line and what to do about each. Say plainly if you find none.\n\n"
        "Its task was:\n%.*s\n\n", name, (int)strnlen(t->preview, 4000), t->preview);
    if (head[0]) w += (usize)snprintf(p + w, cap - w, "What it reported:\n%s\n\n", head);
    if (t->wt[0])
        w += (usize)snprintf(p + w, cap - w,
                             "Its changes are in its own worktree %s (branch %s); `git -C %s diff %s` shows them.\n",
                             t->wt, t->wt_branch, t->wt, t->wt_base);
    if (t->ch.n) {
        w += (usize)snprintf(p + w, cap - w, "The files it changed:\n");
        for (u32 i = 0; i < t->ch.n && w + 1100 < cap; i++)
            w += (usize)snprintf(p + w, cap - w, "- %s%s\n", agentctl_team_rel(t->ch.path[i], t->cwd),
                                 t->ch.ns[i] ? "" : " (removed)");
    }
    if (elen && w + elen + 4 < cap) {
        w += (usize)snprintf(p + w, cap - w, "\n");
        memcpy(p + w, extra, elen);
        w += elen;
    }
    p[w] = '\0';
    *out_len = (u32)w;
    return p;
}

/* Under g_mu: a new job from `owner`'s pane, waiting for the user's answer
 * (or started at once when `approved`). NULL with `*why` and `*code` when it
 * is refused. The caller has checked the agent, model, effort, level and
 * folder. */
typedef struct {
    u64 owner;
    i32 agent, perm;
    const char *model, *effort, *cwd, *label;
    const u8 *prompt; u32 plen;
    char (*owns)[AGT_PATH_MAX]; u8 nowns;
    const u32 *after; u8 nafter;
    u32 review_of;
    bool isolate, ask_tools;
    u32 max_turns, deadline_s;
    u64 max_tokens, budget_micro;
    const char *session;            /* a session the first turn resumes ("" = a new one) */
    bool approved;
    const char *owner_label;        /* for an approved job */
} JobNew;

static Job *job_new(const JobNew *n, u64 *code, const char **why) {
    for (u8 k = 0; k < n->nafter; k++) {
        const Job *d = job_find(n->after[k]);
        if (!d || d->owner_serial != n->owner || d->state == AGJ_DECLINED ||
            (d->state == AGJ_ENDED && d->outcome != AGO_DONE)) {
            *code = AGC_REFUSED;
            *why = "--after names a job that is not this pane's, or one that will not finish.";
            return NULL;
        }
    }
    const Job *rt = NULL;
    if (n->review_of) {
        rt = job_find(n->review_of);
        if (!rt || rt->owner_serial != n->owner || !job_shown(rt)) {
            *code = AGC_NOTFOUND;
            *why = "--review names no job of this pane.";
            return NULL;
        }
    }
    u32 plen = n->plen;
    char *prompt = NULL;
    if (rt) prompt = review_prompt(rt, n->prompt, n->plen, &plen);
    else if ((prompt = malloc(plen + 1))) { memcpy(prompt, n->prompt, plen); prompt[plen] = '\0'; }
    if (!prompt) { *code = AGC_ERROR; *why = "out of memory"; return NULL; }
    if (plen == 0 || plen > AGENTCTL_MAX_PROMPT) {
        free(prompt);
        *code = AGC_REFUSED;
        *why = "The task is empty.";
        return NULL;
    }
    Job *j = job_alloc();
    if (!j) {
        free(prompt);
        *code = AGC_BUSY;
        *why = "Liu is holding as many delegated agents as it can; close one first.";
        return NULL;
    }
    if (n->nowns) memcpy(j->owns, n->owns, sizeof(char[AGT_PATH_MAX]) * n->nowns);
    j->nowns = n->nowns;
    j->prompt = prompt;
    j->prompt_len = plen;
    usize pv = plen < AGENTCTL_PREVIEW_CAP - 1 ? plen : AGENTCTL_PREVIEW_CAP - 1;
    memcpy(j->preview, prompt, pv);
    j->preview[pv] = '\0';
    j->id = g_next_id++;
    j->agent = n->agent;
    j->perm = n->perm;
    snprintf(j->model, sizeof j->model, "%s", n->model ? n->model : "");
    snprintf(j->effort, sizeof j->effort, "%s", n->effort ? n->effort : "");
    snprintf(j->cwd, sizeof j->cwd, "%s", n->cwd);
    snprintf(j->origin_cwd, sizeof j->origin_cwd, "%s", n->cwd);
    char label[AGENTCTL_LABEL_CAP];
    if (n->label && n->label[0]) snprintf(label, sizeof label, "%s", n->label);
    else if (rt) snprintf(label, sizeof label, "review of job %u", rt->id);
    else derive_label(j->prompt, label, sizeof label);
    derive_label(label, j->label, sizeof j->label);
    j->owner_serial = n->owner;
    snprintf(j->owner_key, sizeof j->owner_key, "%s", ident_of(n->owner));
    if (n->session && n->session[0] && agentctl_session_ok(n->session))
        snprintf(j->session, sizeof j->session, "%s", n->session);
    for (u8 k = 0; k < n->nafter && k < AGT_AFTER_MAX; k++) j->after[j->nafter++] = n->after[k];
    j->review_of = n->review_of;
    j->isolate = n->isolate;
    j->ask_tools = n->ask_tools && agentctl_agent_can_ask(n->agent);
    j->max_turns = n->max_turns;
    j->deadline_s = n->deadline_s;
    j->max_tokens = n->max_tokens;
    j->budget_micro = n->budget_micro;
    lead_of(n->owner, true);
    lead_mark(n->owner, true);
    j->runner = -1;
    j->created_ms = now_ms();
    if (g_jobs_root[0]) {
        snprintf(j->dir, sizeof j->dir, "%s/%u", g_jobs_root, j->id);
        if (mkdir(j->dir, 0700) != 0 && errno != EEXIST) j->dir[0] = '\0';
    }
    if (n->approved) {
        snprintf(j->owner_label, sizeof j->owner_label, "%s", n->owner_label ? n->owner_label : "");
        if (!j->owner_label[0]) member_name(n->owner, AGT_LEAD, j->owner_label, sizeof j->owner_label);
        j->seq = g_next_seq++;
        j->state = AGJ_QUEUED;
        j->meta_dirty = true;
        promote_queue();
    } else {
        j->state = AGJ_PENDING_APPROVAL;
        j->ask = true;
    }
    recount_jobs();
    bump_gen();
    *code = AGC_OK;
    return j;
}

/* Whether `bin` is on the PATH the runners start with. */
static bool agent_installed(const char *bin) {
    const char *path = NULL;
    for (usize i = 0; g_runner_env && g_runner_env[i]; i++)
        if (!strncmp(g_runner_env[i], "PATH=", 5)) { path = g_runner_env[i] + 5; break; }
    if (!path || !bin) return false;
    char dir[1024], full[1300];
    for (const char *p = path; *p; ) {
        const char *c = strchr(p, ':');
        usize n = c ? (usize)(c - p) : strlen(p);
        if (n && n < sizeof dir) {
            memcpy(dir, p, n);
            dir[n] = '\0';
            snprintf(full, sizeof full, "%s/%s", dir, bin);
            if (access(full, X_OK) == 0) return true;
        }
        if (!c) break;
        p = c + 1;
    }
    return false;
}

static bool guard_check(u64 owner, u32 me, const char *cwd, const AgentctlFrame *f, char *why, usize cap);
static char *lead_news(u64 owner);

/* Under g_mu: `j`'s work merged or thrown away by its runner, between
 * turns. A job taken over from an earlier Liu gets a runner for it. */
static bool merge_request_locked(Job *j, bool merge, char *why, usize cap) {
    if (!j->wt[0]) { snprintf(why, cap, "This job works in the folder itself (it was not started with --isolate)."); return false; }
    if (j->merging || j->merge_req) { snprintf(why, cap, "Its work is being merged already."); return false; }
    if (j->state != AGJ_IDLE) { snprintf(why, cap, "It is still working; wait for its turn to end."); return false; }
    snprintf(j->merge_dec, sizeof j->merge_dec, "%s", merge ? "merge" : "discard");
    j->merge_req = true;
    if (j->runner < 0 && j->runner_pid == 0) {
        j->seq = g_next_seq++;
        j->state = AGJ_QUEUED;
        j->restored = false;
        j->parked = false;
        promote_queue();
    }
    bump_gen();
    return true;
}

static bool handle_client(Conn *c, const AgentctlFrame *f) {
    char buf[512];
    switch (f->op) {
    case AGOP_SPAWN: {
        if (!g_policy.enabled) {
            conn_reply(c, 0, AGC_REFUSED, "Starting agents from an agent is turned off in Liu (Settings > Terminal).");
            return false;
        }
        char agent_s[32], model[AGENTCTL_MODEL_CAP], effort[AGENTCTL_EFFORT_CAP], perm_s[16];
        char cwd[1024], label[AGENTCTL_LABEL_CAP];
        agentctl_get_str(f, AGK_AGENT, agent_s, sizeof agent_s);
        i32 agent = agentctl_agent_parse(agent_s);
        if (agent < 0) {
            conn_reply(c, 0, AGC_REFUSED, "Unknown agent. Liu can start: " AGENTCTL_AGENT_LIST ".");
            return false;
        }
        if (!agentctl_get_str(f, AGK_MODEL, model, sizeof model)) model[0] = '\0';
        if (!agentctl_model_ok(model)) { conn_reply(c, 0, AGC_REFUSED, "That model name is not allowed."); return false; }
        if (!agentctl_get_str(f, AGK_EFFORT, effort, sizeof effort)) effort[0] = '\0';
        if (!agentctl_effort_ok(effort)) {
            conn_reply(c, 0, AGC_REFUSED, "Effort must be one of: none, minimal, low, medium, high, xhigh, max.");
            return false;
        }
        i32 perm = AGENTCTL_PERM_WRITE;
        if (agentctl_get_str(f, AGK_PERMISSION, perm_s, sizeof perm_s) && perm_s[0]) {
            perm = agentctl_perm_parse(perm_s);
            if (perm < 0) { conn_reply(c, 0, AGC_REFUSED, "Permission must be read, write or full."); return false; }
        }
        if (perm > g_policy.perm_ceiling) {
            snprintf(buf, sizeof buf, "Liu allows delegated agents at most '%s' (Settings > Terminal).",
                     agentctl_perm_name(g_policy.perm_ceiling));
            conn_reply(c, 0, AGC_REFUSED, buf);
            return false;
        }
        struct stat st;
        if (!agentctl_get_str(f, AGK_CWD, cwd, sizeof cwd) || cwd[0] != '/' ||
            stat(cwd, &st) != 0 || !S_ISDIR(st.st_mode)) {
            conn_reply(c, 0, AGC_REFUSED, "The folder does not exist.");
            return false;
        }
        canon_path(cwd, sizeof cwd);           /* the spelling its agent's own paths will have */
        const u8 *prompt; u32 plen;
        if (!agentctl_get(f, AGK_PROMPT, &prompt, &plen) || plen == 0 || plen > AGENTCTL_MAX_PROMPT ||
            memchr(prompt, '\0', plen)) {
            conn_reply(c, 0, AGC_REFUSED, "The task is empty.");
            return false;
        }
        char (*owns)[AGT_PATH_MAX] = calloc(AGT_OWNS_MAX, sizeof *owns);
        u8 nowns = 0;
        const char *owns_why = NULL;
        if (!owns) { conn_reply(c, 0, AGC_ERROR, "out of memory"); return false; }
        if (!owns_from(f, cwd, owns, &nowns, &owns_why)) {
            snprintf(buf, sizeof buf, "--owns: %s", owns_why);
            free(owns);
            conn_reply(c, 0, AGC_REFUSED, buf);
            return false;
        }
        JobNew jn;
        memset(&jn, 0, sizeof jn);
        jn.owner = c->pane;
        jn.agent = agent;
        jn.perm = perm;
        jn.model = model;
        jn.effort = effort;
        jn.cwd = cwd;
        if (!agentctl_get_str(f, AGK_LABEL, label, sizeof label)) label[0] = '\0';
        jn.label = label;
        jn.prompt = prompt;
        jn.plen = plen;
        jn.owns = owns;
        jn.nowns = nowns;
        u32 after[AGT_AFTER_MAX];
        u8 nafter = 0;
        {
            u32 pos = 0; u16 k; const u8 *d; u32 n;
            while (agentctl_next(f, &pos, &k, &d, &n)) {
                if (k != AGK_AFTER) continue;
                u64 id = 0;
                for (u32 x = 0; x < n && x < 12; x++) {
                    if (d[x] < '0' || d[x] > '9') { id = 0; break; }
                    id = id * 10u + (u64)(d[x] - '0');
                }
                if (!id || id > 0xffffffffu || nafter >= AGT_AFTER_MAX) {
                    free(owns);
                    conn_reply(c, 0, AGC_REFUSED, "--after takes at most 4 job numbers.");
                    return false;
                }
                after[nafter++] = (u32)id;
            }
        }
        jn.after = after;
        jn.nafter = nafter;
        u64 v = 0;
        if (agentctl_get_u64(f, AGK_REVIEW, &v) && v <= 0xffffffffu) jn.review_of = (u32)v;
        if (agentctl_get_u64(f, AGK_MAX_TURNS, &v) && v <= 100000u) jn.max_turns = (u32)v;
        if (agentctl_get_u64(f, AGK_DEADLINE, &v) && v <= 7u * 86400u) jn.deadline_s = (u32)v;
        if (agentctl_get_u64(f, AGK_MAX_TOKENS, &v)) jn.max_tokens = v;
        if (agentctl_get_u64(f, AGK_BUDGET, &v)) jn.budget_micro = v;
        char flag[4];
        jn.isolate = agentctl_get_str(f, AGK_ISOLATE, flag, sizeof flag) && flag[0] == '1';
        jn.ask_tools = g_policy.ask_tools;
        if (agentctl_get_str(f, AGK_ASK_TOOLS, flag, sizeof flag)) jn.ask_tools = jn.ask_tools && flag[0] == '1';
        char sess[AGENTCTL_SESSION_CAP];
        if (agentctl_get_str(f, AGK_SESSION, sess, sizeof sess)) {
            if (!agentctl_session_ok(sess)) { free(owns); conn_reply(c, 0, AGC_REFUSED, "That session id is not allowed."); return false; }
            jn.session = sess;
        }
        /* a pane the user trusted starts what its level allows at once */
        i32 trusted = trust_of(c->pane);
        jn.approved = trusted >= 0 && perm <= trusted;
        u64 code = AGC_OK;
        const char *why = NULL;
        Job *j = job_new(&jn, &code, &why);
        free(owns);
        if (!j) { conn_reply(c, 0, (AgentctlCode)code, why); return false; }
        if (jn.approved) {
            j->started_notice = true;
            reply_job(c, j, AGC_OK, "Started without asking: the user trusts this tab.");
            return true;
        }
        reply_job(c, j, AGC_PENDING, "Waiting for the user to allow it in Liu.");
        c->wait_kind = WAIT_APPROVAL;
        c->wait_jobs[0] = j->id;
        c->wait_count = 1;
        return true;
    }
    case AGOP_WAIT: {
        u32 pos = 0; u16 k; const u8 *d; u32 n;
        c->wait_count = 0;
        c->wait_any = false;
        while (agentctl_next(f, &pos, &k, &d, &n)) {
            if (k == AGK_ANY) c->wait_any = (n == 1 && d[0] == '1');
            if (k != AGK_JOB || c->wait_count >= AGENTCTL_MAX_WAIT) continue;
            u64 id = 0;
            for (u32 x = 0; x < n && x < 12; x++) {
                if (d[x] < '0' || d[x] > '9') { id = 0; break; }
                id = id * 10u + (u64)(d[x] - '0');
            }
            Job *j = job_find((u32)id);
            if (!j || j->owner_serial != c->pane) {
                snprintf(buf, sizeof buf, "No job %llu from this pane.", (unsigned long long)id);
                conn_reply(c, (u32)id, AGC_NOTFOUND, buf);
                c->wait_count = 0;
                return false;
            }
            c->wait_jobs[c->wait_count++] = j->id;
        }
        if (c->wait_count == 0) { conn_reply(c, 0, AGC_REFUSED, "No job to wait for."); return false; }
        c->wait_kind = WAIT_SETTLE;
        return false;   /* service_locked answers it, now or later */
    }
    case AGOP_SEND: {
        Job *j = job_find(f->job);
        if (!j || j->owner_serial != c->pane) { conn_reply(c, f->job, AGC_NOTFOUND, "No such job from this pane."); return false; }
        const u8 *prompt = NULL; u32 plen = 0;
        if (!agentctl_get(f, AGK_PROMPT, &prompt, &plen)) { prompt = NULL; plen = 0; }
        const char *why = NULL;
        u64 code = job_queue_turn(j, prompt, plen, &why);
        if (code != AGC_OK) {
            if (code == AGC_REFUSED && (!prompt || plen == 0)) conn_reply(c, j->id, code, why);
            else reply_job(c, j, code, why);
            return false;
        }
        reply_job(c, j, AGC_OK, NULL);
        return true;
    }
    case AGOP_STATUS: {
        Job *j = job_find(f->job);
        if (!j || j->owner_serial != c->pane) { conn_reply(c, f->job, AGC_NOTFOUND, "No such job from this pane."); return false; }
        reply_job(c, j, AGC_OK, NULL);
        return false;
    }
    case AGOP_LIST: {
        AgentctlMsg m;
        if (!agentctl_msg_init(&m, AGOP_REPLY, 0)) return false;
        agentctl_msg_put_u64(&m, AGK_CODE, AGC_OK);
        u32 listed = 0;
        for (i32 i = 0; i < AGENTCTL_MAX_JOBS && listed < AGENTCTL_MAX_FIELDS - 2; i++) {
            const Job *j = &g_jobs[i];
            if (!j->id || j->owner_serial != c->pane) continue;
            const AgentctlAgentInfo *ai = agentctl_agent_info(j->agent);
            char line[256];
            snprintf(line, sizeof line, "%u\t%s\t%s%s%s\t%s", j->id, ai ? ai->id : "?",
                     agentctl_state_name(j->state),
                     j->state == AGJ_IDLE ? "/" : "",
                     j->state == AGJ_IDLE ? agentctl_outcome_name(j->outcome) : "", j->label);
            agentctl_msg_put_str(&m, AGK_LINE, line);
            listed++;
        }
        if (agentctl_msg_end(&m)) conn_queue(c, &m);
        agentctl_msg_free(&m);
        return false;
    }
    case AGOP_STOP:
    case AGOP_CLOSE: {
        Job *j = job_find(f->job);
        if (!j || j->owner_serial != c->pane) { conn_reply(c, f->job, AGC_NOTFOUND, "No such job from this pane."); return false; }
        bool main_work = false;
        if (j->state == AGJ_PENDING_APPROVAL) {
            /* withdrawn before the user answered */
            j->state = AGJ_DECLINED;
            snprintf(j->error, sizeof j->error, "withdrawn by the agent that asked");
            j->ask = false;
            j->dismiss = true;
            bump_gen();
            main_work = true;
        } else if (j->state == AGJ_QUEUED) {
            job_end(j, AGE_CLOSED, NULL);
            main_work = true;
        } else if (f->op == AGOP_STOP) {
            if (j->state == AGJ_RUNNING && j->runner >= 0) j->send_stop = true;
        } else if (j->state != AGJ_ENDED) {
            job_end(j, AGE_CLOSED, NULL);
            main_work = true;
        }
        reply_job(c, j, AGC_OK, NULL);
        return main_work;
    }
    case AGOP_FOCUS: {
        Job *j = job_find(f->job);
        if (!j || j->owner_serial != c->pane) { conn_reply(c, f->job, AGC_NOTFOUND, "No such job from this pane."); return false; }
        j->focus = true;
        reply_job(c, j, AGC_OK, NULL);
        return true;
    }
    case AGOP_MERGE:
    case AGOP_DISCARD: {
        Job *j = job_find(f->job);
        if (!j || j->owner_serial != c->pane) { conn_reply(c, f->job, AGC_NOTFOUND, "No such job from this pane."); return false; }
        if (!merge_request_locked(j, f->op == AGOP_MERGE, buf, sizeof buf)) { reply_job(c, j, AGC_REFUSED, buf); return false; }
        reply_job(c, j, AGC_OK, NULL);
        return true;
    }
    case AGOP_TEAM:
    case AGOP_POST:
    case AGOP_INBOX:
    case AGOP_OWNS:
        return team_request(c, f, c->pane, AGT_LEAD);
    case AGOP_LEAD_TOOL: {
        /* the pane's agent is about to run, or has run, a tool: its hooks say
         * which and with what, so what it changes is the lead's. The answer
         * may stop an edit of another member's path first, and after the
         * tool it carries what the team did meanwhile. */
        if (!team_exists(c->pane)) { conn_reply(c, 0, AGC_OK, NULL); return false; }
        Lead *l = lead_of(c->pane, true);
        char st[4], id[64], cwd[AGT_PATH_MAX];
        if (!l || !agentctl_get_str(f, AGK_STATE, st, sizeof st) || !agentctl_get_str(f, AGK_TEXT, id, sizeof id)) {
            conn_reply(c, 0, AGC_OK, NULL);
            return false;
        }
        if (agentctl_get_str(f, AGK_CWD, cwd, sizeof cwd) && cwd[0] == '/') {
            canon_path(cwd, sizeof cwd);
            snprintf(l->cwd, sizeof l->cwd, "%s", cwd);
        }
        {
            AgentctlMsg rm;
            if (agentctl_msg_init(&rm, AGOP_REPLY, 0)) {
                agentctl_msg_put_u64(&rm, AGK_CODE, AGC_OK);
                char why[900];
                if (st[0] == '1' && l->cwd[0] == '/' && guard_check(c->pane, AGT_LEAD, l->cwd, f, why, sizeof why)) {
                    agentctl_msg_put_str(&rm, AGK_DECISION, "ask");
                    agentctl_msg_put_str(&rm, AGK_TEXT, why);
                }
                char *news = st[0] == '0' ? lead_news(c->pane) : NULL;
                if (news) { agentctl_msg_put_str(&rm, AGK_TEAM, news); free(news); }
                if (agentctl_msg_end(&rm)) conn_queue(c, &rm);
                agentctl_msg_free(&rm);
            }
        }
        if (!l->cwd[0]) {
            for (i32 i = 0; i < AGENTCTL_MAX_JOBS && !l->cwd[0]; i++)
                if (g_jobs[i].id && g_jobs[i].owner_serial == c->pane) snprintf(l->cwd, sizeof l->cwd, "%s", g_jobs[i].cwd);
        }
        /* what tells its change from another's: its line, or the files it names */
        char what[4096];
        usize w = 0;
        what[0] = '\0';
        const u8 *d; u32 n;
        if (agentctl_get(f, AGK_PROMPT, &d, &n) && n > 0 && !memchr(d, '\0', n))
            w += (usize)snprintf(what, sizeof what, "%.*s", (int)(n < 3000 ? n : 3000), (const char *)d);
        u32 pos = 0; u16 k;
        while (agentctl_next(f, &pos, &k, &d, &n))
            if (k == AGK_PATH && n > 0 && !memchr(d, '\0', n) && w + n + 2 < sizeof what)
                w += (usize)snprintf(what + w, sizeof what - w, " %.*s", (int)n, (const char *)d);
        i64 now = now_ns();
        l->hooked_ns = now;
        if (st[0] == '1') { win_open(l->wins, id, what, now); return false; }
        win_close(l->wins, id, what, now);
        bool told = false;
        pos = 0;
        while (agentctl_next(f, &pos, &k, &d, &n))
            if (k == AGK_PATH && n > 0 && n < AGT_PATH_MAX && !memchr(d, '\0', n) && l->cwd[0] == '/')
                told |= member_touch(c->pane, AGT_LEAD, l->cwd, (const char *)d, n, false);
        return told;
    }
    case AGOP_AGENTS: {
        AgentctlMsg m;
        if (!agentctl_msg_init(&m, AGOP_REPLY, 0)) return false;
        agentctl_msg_put_u64(&m, AGK_CODE, AGC_OK);
        for (i32 a = 0; a < AGENTCTL_AGENT_COUNT; a++) {
            char line[160];
            snprintf(line, sizeof line, "%s\t%s\t%s%s", AGENTCTL_AGENTS[a].id, AGENTCTL_AGENTS[a].display,
                     agent_installed(AGENTCTL_AGENTS[a].binary) ? "installed" : "not found on PATH",
                     agentctl_agent_can_ask(a) ? ", asks before tools its level refuses" : "");
            agentctl_msg_put_str(&m, AGK_LINE, line);
        }
        snprintf(buf, sizeof buf, "%s; at most %d working at once; permission up to '%s'",
                 g_policy.enabled ? "allowed" : "turned off", g_policy.max_running,
                 agentctl_perm_name(g_policy.perm_ceiling));
        agentctl_msg_put_str(&m, AGK_TEXT, buf);
        if (agentctl_msg_end(&m)) conn_queue(c, &m);
        agentctl_msg_free(&m);
        return false;
    }
    default:
        conn_reply(c, 0, AGC_REFUSED, "Unknown request.");
        c->close_after_flush = true;
        return false;
    }
}

/* Under g_mu: before member `me` of `owner`'s team writes the paths in `f`
 * (AGK_PATH, relative to `cwd`): whether one of them is a path another
 * member owns and `me` does not. `why` names the path and its owner. */
static bool guard_check(u64 owner, u32 me, const char *cwd, const AgentctlFrame *f, char *why, usize cap) {
    if (!g_policy.guard || !cwd || cwd[0] != '/' || !team_exists(owner)) return false;
    u32 pos = 0; u16 k; const u8 *d; u32 n;
    while (agentctl_next(f, &pos, &k, &d, &n)) {
        if (k != AGK_PATH || n == 0 || n >= AGT_PATH_MAX || memchr(d, '\0', n)) continue;
        char path[AGT_PATH_MAX];
        if (!agentctl_team_path(cwd, (const char *)d, n, path, sizeof path)) continue;
        canon_path(path, sizeof path);
        if (member_owns(owner, me, path)) continue;
        u32 other = 0;
        bool owned = false;
        if (me != AGT_LEAD && member_owns(owner, AGT_LEAD, path)) { other = AGT_LEAD; owned = true; }
        for (i32 i = 0; i < AGENTCTL_MAX_JOBS && !owned; i++) {
            const Job *o = &g_jobs[i];
            if (o->id == me || o->owner_serial != owner || !job_shown(o) || o->state == AGJ_ENDED) continue;
            if (member_owns(owner, o->id, path)) { other = o->id; owned = true; }
        }
        if (!owned) continue;
        char name[160], to[16];
        member_name(owner, other, name, sizeof name);
        if (other == AGT_LEAD) snprintf(to, sizeof to, "lead");
        else                   snprintf(to, sizeof to, "%u", other);
        snprintf(why, cap,
                 "%s belongs to %s in this team (Liu). Tell it first with `liu-agent post --to %s \"...\"`, "
                 "or leave the change to it%s.",
                 agentctl_team_rel(path, cwd), name, to,
                 me == AGT_LEAD ? "; `liu-agent owns` takes a path over" : "");
        return true;
    }
    return false;
}

/* Under g_mu: what `owner`'s lead has not heard of yet, for its tool hook
 * to put in front of its agent: jobs whose turn ended (their report's head),
 * a job waiting for the user, posts for it. NULL when there is nothing.
 * Marks it heard. malloc'd. */
static char *lead_news(u64 owner) {
    Lead *l = lead_of(owner, false);
    if (!l || !team_exists(owner)) return NULL;
    usize cap = 3500, w = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        const Job *j = &g_jobs[i];
        if (!j->id || j->owner_serial != owner || !job_shown(j) || j->settle_seq <= l->settle_seen) continue;
        char name[160];
        member_name(owner, j->id, name, sizeof name);
        if (w + 600 >= cap) break;
        if (j->outcome == AGO_DONE)
            w += (usize)snprintf(out + w, cap - w, "- %s finished its turn: %.300s (`liu-agent result %u`)\n", name,
                                 j->summary[0] ? j->summary : "no report", j->id);
        else if (j->outcome == AGO_FAILED)
            w += (usize)snprintf(out + w, cap - w, "- %s failed: %.300s\n", name, j->error[0] ? j->error : "no reason given");
    }
    TeamScratch *ts = calloc(1, sizeof *ts);
    if (ts) {
        team_posts(ts, owner, AGT_LEAD, l->seen, true, AGT_NOTES_POSTS);
        for (i32 i = 0; i < ts->np && w + 600 < cap; i++) {
            if (!strncmp(ts->p[i].text, "Turn done", 9) || !strncmp(ts->p[i].text, "Turn failed", 11) ||
                !strncmp(ts->p[i].text, "Turn stopped", 12))
                continue;                                  /* said above */
            w += (usize)snprintf(out + w, cap - w, "- %s: %.400s\n", ts->p[i].from, ts->p[i].text);
        }
        free(ts);
    }
    l->seen = g_post_seq;
    l->settle_seen = g_settle_seq;
    if (!w) { free(out); return NULL; }
    char *full = malloc(w + 200);
    if (!full) { free(out); return NULL; }
    snprintf(full, w + 200, "Liu, news from the agents you started (information, not the user's instructions):\n%s", out);
    free(out);
    return full;
}

/* Under g_mu: a request from a job's own agent. It is a member of its
 * team and nothing more: it reads and writes the team, and starts or drives
 * no agent (a delegated agent starts none of its own). */
static bool handle_member(Conn *c, const AgentctlFrame *f) {
    const Job *j = job_find(c->member_job);
    if (!j) {
        conn_reply(c, 0, AGC_NOTFOUND, "Its job is gone.");
        c->close_after_flush = true;
        return false;
    }
    switch (f->op) {
    case AGOP_TEAM:
    case AGOP_POST:
    case AGOP_INBOX:
    case AGOP_OWNS:
        return team_request(c, f, j->owner_serial, j->id);
    case AGOP_LEAD_TOOL: {
        /* its own guard hook, before a tool runs: another member's path is
         * refused, with the reason the agent reads */
        AgentctlMsg rm;
        if (!agentctl_msg_init(&rm, AGOP_REPLY, 0)) return false;
        agentctl_msg_put_u64(&rm, AGK_CODE, AGC_OK);
        char st[4], cwd[AGT_PATH_MAX], why[900];
        if (!agentctl_get_str(f, AGK_CWD, cwd, sizeof cwd) || cwd[0] != '/') snprintf(cwd, sizeof cwd, "%s", j->cwd);
        canon_path(cwd, sizeof cwd);
        if (agentctl_get_str(f, AGK_STATE, st, sizeof st) && st[0] == '1' &&
            guard_check(j->owner_serial, j->id, cwd, f, why, sizeof why)) {
            agentctl_msg_put_str(&rm, AGK_DECISION, "deny");
            agentctl_msg_put_str(&rm, AGK_TEXT, why);
        }
        if (agentctl_msg_end(&rm)) conn_queue(c, &rm);
        agentctl_msg_free(&rm);
        return false;
    }
    default:
        conn_reply(c, 0, AGC_REFUSED, f->op == AGOP_SPAWN
                   ? "An agent started by another agent cannot start agents of its own."
                   : "An agent started by another agent talks only to its team: liu-agent team, post, inbox, owns.");
        return false;
    }
}

/* Under g_mu: a turn of `j` ended; when it works beside other jobs, the
 * team hears how, in its own name: the head of its final message, or why it
 * did not finish. */
static void team_turn_ended(Job *j, const AgentctlFrame *f) {
    bool peers = false;
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS && !peers; i++) {
        const Job *o = &g_jobs[i];
        peers = o != j && o->owner_serial == j->owner_serial && job_shown(o) && o->state != AGJ_ENDED;
    }
    if (!peers) return;
    char msg[AGT_POST_MAX], head[AGT_SUMMARY_MAX + 8];
    if (j->outcome == AGO_DONE) {
        if (!agentctl_get_str(f, AGK_TEXT, head, sizeof head)) head[0] = '\0';
        snprintf(msg, sizeof msg, "Turn done%s%s", head[0] ? ": " : ".", head);
    } else if (j->outcome == AGO_STOPPED) {
        snprintf(msg, sizeof msg, "Turn stopped.");
    } else {
        snprintf(msg, sizeof msg, "Turn failed: %s", j->error[0] ? j->error : "no reason given");
    }
    post_add(j->owner_serial, j->id, AGT_ALL, msg, strlen(msg));
}

/* Under g_mu. Returns true when what a job shows changed. */
static bool handle_runner(i32 idx, const AgentctlFrame *f) {
    Conn *c = &g_conns[idx];
    if (f->op == AGOP_R_HELLO) {
        Job *j = job_find(f->job);
        if (c->pane != 0 || !j || j->runner_pid != (pid_t)c->peer_pid || j->runner >= 0 ||
            j->state != AGJ_STARTING) {
            conn_reply(c, f->job, AGC_REFUSED, "Liu did not start a runner for that job.");
            c->close_after_flush = true;
            return false;
        }
        c->runner = true;
        c->runner_job = j->id;
        j->runner = idx;
        AgentctlMsg m;
        if (agentctl_msg_init(&m, AGOP_S_SPEC, j->id)) {
            put_job(&m, j);
            agentctl_msg_put_str(&m, AGK_OWNER, j->owner_label);
            if (j->isolate) agentctl_msg_put_str(&m, AGK_ISOLATE, "1");
            if (g_policy.keep && j->owner_key[0]) agentctl_msg_put_str(&m, AGK_KEEP, "1");
            if (j->ask_tools) agentctl_msg_put_str(&m, AGK_ASK_TOOLS, "1");
            if (j->deadline_s) agentctl_msg_put_u64(&m, AGK_DEADLINE, j->deadline_s);
            if (j->budget_micro)
                agentctl_msg_put_u64(&m, AGK_BUDGET, j->spent_cost >= j->budget_micro ? 1 : j->budget_micro - j->spent_cost);
            if (g_helpers[0]) agentctl_msg_put_str(&m, AGK_HELPERS, g_helpers);
            if (j->wt[0]) agentctl_msg_put_str(&m, AGK_PATH, j->origin_top);
            if (j->prompt) agentctl_msg_put(&m, AGK_PROMPT, j->prompt, j->prompt_len);
            char *notes = team_notes_for(j);
            if (notes) agentctl_msg_put_str(&m, AGK_TEAM, notes);
            free(notes);
            if (agentctl_msg_end(&m)) conn_queue(c, &m);
            agentctl_msg_free(&m);
        }
        bool had_task = j->prompt != NULL;
        free(j->prompt);
        j->prompt = NULL;
        j->prompt_len = 0;
        /* a job taken over from an earlier Liu waits for its next message */
        j->state = had_task ? AGJ_RUNNING : AGJ_IDLE;
        if (had_task) j->turn_start_ms = now_ms();
        bump_gen();
        return true;
    }
    if (!c->runner) {
        conn_reply(c, f->job, AGC_REFUSED, "Say hello first.");
        c->close_after_flush = true;
        return false;
    }
    Job *j = job_find(c->runner_job);
    if (!j || j->runner != idx || f->job != j->id) return false;
    char s[AGENTCTL_SESSION_CAP];
    switch (f->op) {
    case AGOP_R_LINE: {
        u64 style = 0;
        const u8 *d; u32 n;
        if (!agentctl_get(f, AGK_TEXT, &d, &n)) return false;
        agentctl_get_u64(f, AGK_STYLE, &style);
        if (style > AGS_HEAD) style = AGS_DIM;
        /* The runner sanitized it; a NUL would still end the stored string
         * early, so a line carrying one is cut there. */
        const u8 *z = memchr(d, '\0', n);
        if (z) n = (u32)(z - d);
        agentctl_ring_push(&j->ring, (u8)style, d, n);
        bump_line_gen();
        /* a line wakes the main loop only for a window that shows it */
        return atomic_load_explicit(&g_log_watched, memory_order_acquire);
    }
    case AGOP_R_POST: {
        /* what the agent wrote to its outbox where its sandbox kept it from
         * the socket: posted in its own name, as `liu-agent post` would */
        const u8 *d; u32 n;
        char to_s[64];
        u32 to = AGT_ALL;
        if (!agentctl_get(f, AGK_TEXT, &d, &n) || n == 0 || n >= AGT_POST_MAX || memchr(d, '\0', n)) return false;
        if (agentctl_get_str(f, AGK_TO, to_s, sizeof to_s) && !team_resolve_to(j->owner_serial, to_s, &to)) return false;
        if (to != AGT_ALL && to != AGT_LEAD) {
            const Job *tj = job_find(to);
            if (!tj || tj->owner_serial != j->owner_serial || !job_shown(tj) || to == j->id) return false;
        }
        if (!post_add(j->owner_serial, j->id, to, (const char *)d, n)) return false;
        char who[160], line[AGT_POST_MAX + 200];
        member_name(j->owner_serial, to, who, sizeof who);
        const Post *p = &g_posts[(g_post_next + AGT_POSTS - 1) % AGT_POSTS];
        snprintf(line, sizeof line, "\xE2\x9C\x89 to %s: %s", who, p->text);
        agentctl_ring_push(&j->ring, AGS_DIM, (const u8 *)line, (u32)strlen(line));
        return true;
    }
    case AGOP_R_FILE: {
        const u8 *d; u32 n;
        if (!agentctl_get(f, AGK_PATH, &d, &n) || n == 0 || n >= AGT_PATH_MAX || memchr(d, '\0', n)) return false;
        char seen[4];
        return team_touch(j, (const char *)d, n, agentctl_get_str(f, AGK_SEEN, seen, sizeof seen) && seen[0] == '1');
    }
    case AGOP_R_TURN_START:
        if (j->state == AGJ_ENDED) return false;
        j->state = AGJ_RUNNING;
        j->turns++;
        j->turn_start_ms = now_ms();
        j->turn_end_ms = 0;
        bump_gen();
        return true;
    case AGOP_R_CMD: {
        char id[64], st[4];
        if (!agentctl_get_str(f, AGK_TEXT, id, sizeof id) || !agentctl_get_str(f, AGK_STATE, st, sizeof st)) return false;
        const u8 *d; u32 n;
        char *cmd = NULL;
        if (agentctl_get(f, AGK_PROMPT, &d, &n) && n > 0 && !memchr(d, '\0', n)) cmd = strndup((const char *)d, n < 4096 ? n : 4096);
        if (st[0] == '1') win_open(j->wins, id, cmd, now_ns());
        else              win_close(j->wins, id, cmd, now_ns());
        free(cmd);
        return false;
    }
    case AGOP_R_TURN_DONE:
    case AGOP_R_TURN_FAIL: {
        wins_close_all(j->wins, now_ns());
        if (agentctl_get_str(f, AGK_SESSION, s, sizeof s) && agentctl_session_ok(s))
            snprintf(j->session, sizeof j->session, "%s", s);
        if (!agentctl_get_str(f, AGK_USAGE, j->usage, sizeof j->usage)) j->usage[0] = '\0';
        char tk[96];
        unsigned long long tin = 0, tcached = 0, tout = 0;
        if (agentctl_get_str(f, AGK_TOKENS, tk, sizeof tk) && sscanf(tk, "%llu %llu %llu", &tin, &tcached, &tout) == 3) {
            spend_add(&j->spent_in, tin);
            spend_add(&j->spent_cached, tcached);
            spend_add(&j->spent_out, tout);
        }
        u64 cost = 0;
        if (agentctl_get_u64(f, AGK_COST, &cost)) spend_add(&j->spent_cost, cost);
        j->turn_end_ms = now_ms();
        j->meta_dirty = true;
        if (j->state == AGJ_ENDED) return false;
        j->state = AGJ_IDLE;
        if (j->asking) { j->asking = false; j->ask_req[0] = j->ask_tool[0] = j->ask_what[0] = '\0'; }
        if (f->op == AGOP_R_TURN_DONE) {
            j->outcome = AGO_DONE;
            j->error[0] = '\0';
            if (!agentctl_get_str(f, AGK_TEXT, j->summary, sizeof j->summary)) j->summary[0] = '\0';
        } else {
            char stopped[4];
            bool was_stopped = agentctl_get_str(f, AGK_STOPPED, stopped, sizeof stopped) && stopped[0] == '1';
            j->outcome = was_stopped ? AGO_STOPPED : AGO_FAILED;
            if (!agentctl_get_str(f, AGK_ERROR, j->error, sizeof j->error)) j->error[0] = '\0';
        }
        j->settle_seq = ++g_settle_seq;
        j->ended_notice = j->outcome != AGO_STOPPED;
        team_turn_ended(j, f);
        job_check_limits(j);
        delegated_list_add(j);
        bump_gen();
        promote_queue();
        return true;
    }
    case AGOP_R_ASK: {
        char st[4], req[136];
        if (!agentctl_get_str(f, AGK_REQ, req, sizeof req) || !agentctl_get_str(f, AGK_STATE, st, sizeof st)) return false;
        if (st[0] == '0') {
            if (j->asking && !strcmp(j->ask_req, req)) {
                j->asking = false;
                j->ask_req[0] = j->ask_tool[0] = j->ask_what[0] = '\0';
                bump_gen();
            }
            return true;
        }
        if (j->state != AGJ_RUNNING) return false;
        snprintf(j->ask_req, sizeof j->ask_req, "%s", req);
        if (!agentctl_get_str(f, AGK_TOOL, j->ask_tool, sizeof j->ask_tool)) j->ask_tool[0] = '\0';
        char what[700];
        if (!agentctl_get_str(f, AGK_TEXT, what, sizeof what)) snprintf(what, sizeof what, "%s", j->ask_tool);
        agentctl_sanitize(what, strlen(what), j->ask_what, sizeof j->ask_what, false);
        j->asking = true;
        j->ask_notice = true;
        /* the lead hears it too: its job is held until the user answers */
        char msg[AGT_POST_MAX];
        snprintf(msg, sizeof msg, "Waiting for the user to allow %s.", j->ask_what);
        post_add(j->owner_serial, j->id, AGT_LEAD, msg, strlen(msg));
        bump_gen();
        return true;
    }
    case AGOP_R_WORKTREE: {
        char wt[1024], cwd[1024], top[1024];
        if (!agentctl_get_str(f, AGK_WORKTREE, wt, sizeof wt) || wt[0] != '/') return false;
        snprintf(j->wt, sizeof j->wt, "%s", wt);
        if (agentctl_get_str(f, AGK_CWD, cwd, sizeof cwd) && cwd[0] == '/') {
            canon_path(cwd, sizeof cwd);
            snprintf(j->cwd, sizeof j->cwd, "%s", cwd);
        }
        agentctl_get_str(f, AGK_BRANCH, j->wt_branch, sizeof j->wt_branch);
        agentctl_get_str(f, AGK_BASE, j->wt_base, sizeof j->wt_base);
        if (agentctl_get_str(f, AGK_PATH, top, sizeof top)) snprintf(j->origin_top, sizeof j->origin_top, "%s", top);
        j->meta_dirty = true;
        bump_gen();
        return true;
    }
    case AGOP_R_MERGED: {
        u64 code = AGC_ERROR;
        char dec[16] = "";
        agentctl_get_u64(f, AGK_CODE, &code);
        agentctl_get_str(f, AGK_DECISION, dec, sizeof dec);
        char text[600];
        if (!agentctl_get_str(f, AGK_TEXT, text, sizeof text)) text[0] = '\0';
        agentctl_sanitize(text, strlen(text), j->merged, sizeof j->merged, false);
        j->merging = false;
        if (code == AGC_OK) {
            char msg[AGT_POST_MAX];
            snprintf(msg, sizeof msg, "%s", j->merged);
            post_add(j->owner_serial, j->id, AGT_ALL, msg, strlen(msg));
            job_end(j, !strcmp(dec, "merge") ? AGE_MERGED : AGE_DISCARDED, NULL);
        }
        j->ended_notice = true;
        bump_gen();
        return true;
    }
    default:
        return false;
    }
}

/* ---------------------------------------------------------------- loop --- */

static void accept_all(void) {
    for (;;) {
        int fd = accept(g_listen, NULL, NULL);
        if (fd < 0) return;
        set_nonblock_cloexec(fd);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        i32 slot = -1;
        for (i32 i = 0; i < AGENTCTL_MAX_CONNS; i++) if (g_conns[i].fd < 0) { slot = i; break; }
        if (slot < 0) { close(fd); continue; }
        Conn *c = &g_conns[slot];
        memset(c, 0, sizeof *c);
        c->fd = fd;
        i32 peer = -1, top = 0;
        u64 pane = 0;
        const char *refuse = "liu-agent works only in a Liu pane.";
        bool ok = false;
        if (notify_peer_uid_ok(fd, &peer) && peer > 1 &&
            agentctl_child_of_self(peer, (i32)getpid(), proc_parent, NULL, &top)) {
            if (session_local_pane_of_shell(top, &pane) && pane) {
                ok = true;                       /* a pane's process */
            } else {
                pthread_mutex_lock(&g_mu);
                const Job *rj = job_of_runner_pid((pid_t)top);
                u32 member = rj ? rj->id : 0;
                pthread_mutex_unlock(&g_mu);
                if (member && top == peer) ok = true;          /* a runner itself */
                else if (member) { ok = true; c->member_job = member; }   /* its agent: the team only */
            }
        }
        if (!ok) {
            conn_reply(c, 0, AGC_REFUSED, refuse);
            c->close_after_flush = true;
            flush_conn(c);
            conn_free(c);
            continue;
        }
        c->pane = pane;
        c->peer_pid = peer;
    }
}

static void *server_main(void *arg) {
    (void)arg;
    struct pollfd pfd[AGENTCTL_MAX_CONNS + 2];
    i32 map[AGENTCTL_MAX_CONNS + 2];
    int timeout = -1;
    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        nfds_t n = 0;
        pfd[n].fd = g_wake[0]; pfd[n].events = POLLIN; pfd[n].revents = 0; map[n++] = -1;
        pfd[n].fd = g_listen;  pfd[n].events = POLLIN; pfd[n].revents = 0; map[n++] = -2;
        for (i32 i = 0; i < AGENTCTL_MAX_CONNS; i++) {
            Conn *c = &g_conns[i];
            if (c->fd < 0) continue;
            pfd[n].fd = c->fd;
            pfd[n].events = (short)(POLLIN | (c->out_off < c->out_len ? POLLOUT : 0));
            pfd[n].revents = 0;
            map[n++] = i;
        }
        int pr = poll(pfd, n, timeout);
        if (pr < 0 && errno != EINTR) break;
        if (pr > 0 && (pfd[0].revents & POLLIN)) {
            u8 drain[64];
            while (read(g_wake[0], drain, sizeof drain) > 0) {}
        }
        if (pr > 0 && (pfd[1].revents & POLLIN)) accept_all();

        bool main_work = false;
        for (nfds_t k = 2; pr > 0 && k < n; k++) {
            i32 idx = map[k];
            Conn *c = &g_conns[idx];
            if (c->fd < 0 || !pfd[k].revents) continue;
            if (pfd[k].revents & POLLOUT) flush_conn(c);
            if (pfd[k].revents & (POLLIN | POLLHUP | POLLERR)) {
                for (;;) {
                    int rc = agentctl_reader_feed(&c->rd, c->fd);
                    if (rc == 0) break;
                    if (rc < 0) {
                        pthread_mutex_lock(&g_mu);
                        conn_detach_locked(idx);
                        pthread_mutex_unlock(&g_mu);
                        conn_free(c);
                        main_work = true;
                        break;
                    }
                    AgentctlFrame f;
                    agentctl_reader_frame(&c->rd, &f);
                    pthread_mutex_lock(&g_mu);
                    if (f.op >= AGOP_R_HELLO && f.op < AGOP_S_SPEC) main_work |= handle_runner(idx, &f);
                    else if (!c->runner && c->pane)                 main_work |= handle_client(c, &f);
                    else if (!c->runner && c->member_job)           main_work |= handle_member(c, &f);
                    else { conn_reply(c, f.job, AGC_REFUSED, "Not a request a runner makes."); c->close_after_flush = true; }
                    pthread_mutex_unlock(&g_mu);
                    agentctl_reader_reset(&c->rd);
                    if (c->close_after_flush) break;
                }
            }
        }
        timeout = runners_tick(&main_work);
        bool detached_left = false;
        if (detached_tick(&detached_left)) main_work = true;
        if (detached_left && (timeout < 0 || timeout > 1000)) timeout = 1000;
        TeamFile files[AGENTCTL_MAX_JOBS];
        MetaFile metas[AGENTCTL_MAX_JOBS];
        char delegated[AGENTCTL_MAX_JOBS][512];
        i32 ndelegated = 0;
        pthread_mutex_lock(&g_mu);
        service_locked();
        i32 nfiles = team_files_take(files, AGENTCTL_MAX_JOBS);
        i32 nmetas = metas_take(metas, AGENTCTL_MAX_JOBS);
        if (g_delegated_dirty) {
            ndelegated = g_delegated_n;
            memcpy(delegated, g_delegated_line, sizeof delegated[0] * (usize)ndelegated);
            g_delegated_n = 0;
            g_delegated_dirty = false;
        }
        pthread_mutex_unlock(&g_mu);
        for (i32 i = 0; i < nfiles; i++) { team_file_write(&files[i]); free(files[i].text); }
        for (i32 i = 0; i < nmetas; i++) { file_replace(metas[i].path, metas[i].text, metas[i].len); free(metas[i].text); }
        if (ndelegated && g_state_dir[0]) {
            char path[1200];
            snprintf(path, sizeof path, "%s/delegated-sessions.tsv", g_state_dir);
            int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd >= 0) {
                struct stat dst;
                if (fstat(fd, &dst) == 0 && S_ISREG(dst.st_mode))
                    for (i32 i = 0; i < ndelegated; i++) (void)!write(fd, delegated[i], strlen(delegated[i]));
                close(fd);
            }
        }
        for (i32 i = 0; i < AGENTCTL_MAX_CONNS; i++) {
            Conn *c = &g_conns[i];
            if (c->fd < 0) continue;
            flush_conn(c);
            if (c->close_after_flush && c->out_off >= c->out_len) {
                pthread_mutex_lock(&g_mu);
                conn_detach_locked(i);
                pthread_mutex_unlock(&g_mu);
                conn_free(c);
                main_work = true;
            }
        }
        if (main_work) wake_main();
    }
    return NULL;
}

/* ------------------------------------------------------------ lifecycle --- */

void agentctl_set_runner(const char *path) {
    snprintf(g_runner_bin, sizeof g_runner_bin, "%s", path ? path : "");
}

void agentctl_set_helpers(const char *dir) {
    snprintf(g_helpers, sizeof g_helpers, "%s", dir && dir[0] == '/' ? dir : "");
}

bool agentctl_start(void (*wake)(void)) {
    if (g_running) return true;
    g_wake_main = wake;
    for (i32 i = 0; i < AGENTCTL_MAX_CONNS; i++) { memset(&g_conns[i], 0, sizeof g_conns[i]); g_conns[i].fd = -1; }
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) g_jobs[i].runner = -1;

    char dir[256];
    if (!notify_run_dir(dir, sizeof dir)) return false;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    int w = snprintf(g_sock_path, sizeof g_sock_path, "%s/liu-agent-%d.sock", dir, (int)getpid());
    if (w <= 0 || (usize)w >= sizeof g_sock_path || (usize)w >= sizeof addr.sun_path) {
        g_sock_path[0] = '\0';
        return false;
    }
    memcpy(addr.sun_path, g_sock_path, (usize)w + 1);
    /* The name carries this process's pid, so a node already there belongs
     * to a process that had this pid before and is gone. */
    unlink(g_sock_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { g_sock_path[0] = '\0'; return false; }
    set_nonblock_cloexec(fd);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || chmod(g_sock_path, 0600) != 0 ||
        listen(fd, 16) != 0) {
        close(fd);
        unlink(g_sock_path);
        g_sock_path[0] = '\0';
        return false;
    }
    if (pipe(g_wake) != 0) {
        close(fd);
        unlink(g_sock_path);
        g_sock_path[0] = '\0';
        return false;
    }
    set_nonblock_cloexec(g_wake[0]);
    set_nonblock_cloexec(g_wake[1]);
    g_listen = fd;
    g_owner_pid = getpid();
    g_runner_env = runner_env_snapshot();
    if (!make_jobs_root()) g_jobs_root[0] = '\0';
    atomic_store(&g_stop, false);
    if (pthread_create(&g_thread, NULL, server_main, NULL) != 0) {
        close(g_listen); g_listen = -1;
        close(g_wake[0]); close(g_wake[1]); g_wake[0] = g_wake[1] = -1;
        unlink(g_sock_path);
        g_sock_path[0] = '\0';
        return false;
    }
    g_running = true;
    g_watching = agentctl_watch_start(watch_cb, NULL);
    session_local_set_agent_sock(g_sock_path);
    return true;
}

/* Whether the next Liu takes `j` over: the setting says so, it belongs to
 * a lead with a session, and it has a session or a turn under way. */
static bool job_outlives(const Job *j) {
    if (!g_policy.keep || !j->id || !j->owner_key[0] || !j->owner_serial) return false;
    if (j->state == AGJ_RUNNING || j->state == AGJ_STARTING) return true;
    return j->state == AGJ_IDLE && j->session[0];
}

static void remove_files(bool quitting) {
    if (g_owner_pid != getpid()) return;   /* a forked child never cleans up */
    for (i32 i = 0; i < AGT_LEADS; i++) if (g_leads[i].owner) lead_mark(g_leads[i].owner, false);
    if (g_sock_path[0]) unlink(g_sock_path);
    if (g_jobs_root[0]) {
        bool kept = false;
        for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
            if (quitting && job_outlives(&g_jobs[i])) { kept = true; continue; }
            remove_job_dir(g_jobs[i].dir);
        }
        if (!kept) rmdir(g_jobs_root);
    }
}

void agentctl_stop(void) {
    if (!g_running) return;
    session_local_set_agent_sock(NULL);
    atomic_store(&g_stop, true);
    kick_server();
    pthread_join(g_thread, NULL);
    g_running = false;
    /* the watch's queue runs nothing of ours once this returns */
    agentctl_watch_stop();
    g_watching = false;
    for (u32 i = 0; i < g_wev_n; i++) free(g_wev[i].path);
    g_wev_n = 0;
    g_wev_armed = false;
    g_watch_ndirs = 0;
    memset(g_known, 0, sizeof g_known);
    g_known_next = 0;
    for (i32 i = 0; i < AGENTCTL_MAX_CONNS; i++) if (g_conns[i].fd >= 0) conn_free(&g_conns[i]);
    if (g_listen >= 0) { close(g_listen); g_listen = -1; }
    if (g_wake[0] >= 0) { close(g_wake[0]); close(g_wake[1]); g_wake[0] = g_wake[1] = -1; }
    pthread_mutex_lock(&g_mu);
    /* every runner: its connection is closed, so it is ending its agent */
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        if (g_jobs[i].runner_pid > 0) { kill(g_jobs[i].runner_pid, SIGTERM); reap_later(&g_jobs[i]); }
    }
    for (i32 i = 0; i < g_reap_count; i++) {
        pid_t pid = g_reap[i].pid;
        for (int t = 0; t < 50 && waitpid(pid, NULL, WNOHANG) == 0; t++) usleep(20000);
        if (kill(pid, 0) == 0) { kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    }
    g_reap_count = 0;
    remove_files(false);
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        free(g_jobs[i].prompt);
        agentctl_ring_free(&g_jobs[i].ring);
        changes_free(&g_jobs[i].ch);
        wins_free(g_jobs[i].wins);
        memset(&g_jobs[i], 0, sizeof g_jobs[i]);
        g_jobs[i].runner = -1;
    }
    for (i32 i = 0; i < AGT_POSTS; i++) free(g_posts[i].text);
    memset(g_posts, 0, sizeof g_posts);
    for (i32 i = 0; i < AGT_LEADS; i++) {
        changes_free(&g_leads[i].ch);
        wins_free(g_leads[i].wins);
        atomic_store_explicit(&g_lead_panes[i], 0, memory_order_release);
    }
    memset(g_leads, 0, sizeof g_leads);
    memset(g_busy, 0, sizeof g_busy);
    g_post_next = 0;
    recount_jobs();
    pthread_mutex_unlock(&g_mu);
    if (g_runner_env) {
        for (usize i = 0; g_runner_env[i]; i++) free(g_runner_env[i]);
        free(g_runner_env);
        g_runner_env = NULL;
    }
    g_sock_path[0] = '\0';
    g_jobs_root[0] = '\0';
}

void agentctl_exit_cleanup(void) {
    /* No lock: the exit path may run while the server thread holds it, and
     * these are only unlinks of names this process made. What the next Liu
     * takes over stays; its job.meta is kept current as the job changes. */
    g_exiting = true;
    remove_files(true);
}

const char *agentctl_socket_path(void) { return g_sock_path; }

/* Whether process `pid` is a Liu (by its name). */
static bool proc_is_liu(i32 pid) {
    char name[64] = {0};
#if defined(__APPLE__)
    if (proc_name(pid, name, sizeof name) <= 0) return false;
#elif defined(__linux__)
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, name, sizeof name - 1);
    close(fd);
    if (n <= 0) return false;
    name[n] = '\0';
    char *nl = strchr(name, '\n');
    if (nl) *nl = '\0';
#else
    (void)pid;
    return false;
#endif
    return strcmp(name, "Liu") == 0;
}

bool agentctl_other_live(void) {
    char dir[256];
    if (!notify_run_dir(dir, sizeof dir)) return false;
    DIR *d = opendir(dir);
    if (!d) return false;
    bool live = false;
    struct dirent *e;
    while (!live && (e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        if (strncmp(n, "liu-agent-", 10) != 0) continue;
        char *end = NULL;
        long pid = strtol(n + 10, &end, 10);
        if (!end || strcmp(end, ".sock") != 0 || pid <= 1 || pid == (long)getpid()) continue;
        if (kill((pid_t)pid, 0) == 0 && proc_is_liu((i32)pid)) live = true;
    }
    closedir(d);
    return live;
}

void agentctl_set_policy(const AgentctlPolicy *p) {
    if (!p) return;
    pthread_mutex_lock(&g_mu);
    g_policy.enabled = p->enabled;
    g_policy.max_running = p->max_running < 1 ? 1 : p->max_running > AGENTCTL_MAX_RUNNING ? AGENTCTL_MAX_RUNNING : p->max_running;
    g_policy.perm_ceiling = (p->perm_ceiling < 0 || p->perm_ceiling >= AGENTCTL_PERM_COUNT) ? AGENTCTL_PERM_WRITE
                                                                                          : p->perm_ceiling;
    g_policy.guard = p->guard;
    g_policy.ask_tools = p->ask_tools;
    g_policy.keep = p->keep;
    promote_queue();
    pthread_mutex_unlock(&g_mu);
    kick_server();
}

/* ------------------------------------------------------- main thread --- */

/* The subagents agents start themselves (agentctl_subagents.h): read
 * through this view, so every reader of jobs below sees both kinds. */
static _Atomic(const AgentctlWatchedView *) g_view;

void agentctl_set_watched_view(const AgentctlWatchedView *v) {
    atomic_store_explicit(&g_view, v, memory_order_release);
}

static const AgentctlWatchedView *watched_view(void) {
    return atomic_load_explicit(&g_view, memory_order_acquire);
}

void agentctl_set_log_watched(bool on) {
    atomic_store_explicit(&g_log_watched, on, memory_order_release);
    const AgentctlWatchedView *v = watched_view();
    if (v && v->log_watched) v->log_watched(on);
}

bool agentctl_main_pending(void) {
    return atomic_load_explicit(&g_main_flag, memory_order_acquire);
}

/* Both counters only ever grow, so their sum moves whenever either does. */
u64 agentctl_cards_gen(void) {
    const AgentctlWatchedView *v = watched_view();
    return (u64)atomic_load_explicit(&g_gen, memory_order_acquire) + (v && v->cards_gen ? v->cards_gen() : 0);
}

u64 agentctl_jobs_gen(void) {
    const AgentctlWatchedView *v = watched_view();
    return (u64)atomic_load_explicit(&g_state_gen, memory_order_acquire) + (v && v->jobs_gen ? v->jobs_gen() : 0);
}

static bool any_delegated(void) {
    return atomic_load_explicit(&g_job_count, memory_order_acquire) > 0;
}

bool agentctl_any_jobs(void) {
    if (any_delegated()) return true;
    const AgentctlWatchedView *v = watched_view();
    return v && v->any && v->any();
}

static void fill_action(AgentctlAction *a, const Job *j, AgentctlActionKind kind) {
    memset(a, 0, sizeof *a);
    a->kind = kind;
    a->job = j->id;
    a->owner_serial = j->owner_serial;
    a->agent = j->agent;
    a->perm = j->perm;
    snprintf(a->model, sizeof a->model, "%s", j->model);
    snprintf(a->effort, sizeof a->effort, "%s", j->effort);
    snprintf(a->cwd, sizeof a->cwd, "%s", j->cwd);
    snprintf(a->label, sizeof a->label, "%s", j->label);
    a->outcome = j->outcome;
    a->review_of = j->review_of;
    a->isolate = j->isolate;
    if (kind == AGACT_ASK) {
        memcpy(a->preview, j->preview, sizeof a->preview);
        a->prompt_len = j->prompt_len;
    } else if (kind == AGACT_NOTICE) {
        snprintf(a->preview, sizeof a->preview, "%s", j->notice_text);
    } else if (kind == AGACT_ENDED) {
        if (j->merged[0])                 snprintf(a->preview, sizeof a->preview, "%s", j->merged);
        else if (j->outcome == AGO_DONE)  snprintf(a->preview, sizeof a->preview, "%s", j->summary);
        else                              snprintf(a->preview, sizeof a->preview, "%s", j->error);
    } else if (kind == AGACT_TOOL_ASK) {
        snprintf(a->preview, sizeof a->preview, "%s", j->ask_what);
    } else if (kind == AGACT_STARTED) {
        usize pv = j->prompt_len < AGENTCTL_PREVIEW_CAP - 1 ? j->prompt_len : AGENTCTL_PREVIEW_CAP - 1;
        memcpy(a->preview, j->preview, pv < sizeof a->preview ? pv : sizeof a->preview - 1);
    }
}

bool agentctl_take_action(AgentctlAction *out) {
    if (!out) return false;
    /* Cleared first, so work posted while this runs sets it again. */
    atomic_store_explicit(&g_main_flag, false, memory_order_release);
    bool found = false;
    pthread_mutex_lock(&g_mu);
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS && !found; i++) {
        Job *j = &g_jobs[i];
        if (!j->id) continue;
        if (j->dismiss)    { j->dismiss = false; fill_action(out, j, AGACT_DISMISS); found = true; }
        else if (j->ask)   { j->ask = false;     fill_action(out, j, AGACT_ASK);     found = true; }
        else if (j->focus) { j->focus = false;   fill_action(out, j, AGACT_FOCUS);   found = true; }
        else if (j->notice) { j->notice = false; fill_action(out, j, AGACT_NOTICE);  found = true; }
        else if (j->started_notice && j->owner_serial) { j->started_notice = false; fill_action(out, j, AGACT_STARTED); found = true; }
        else if (j->ask_notice && j->owner_serial) {
            j->ask_notice = false;
            if (j->asking) { fill_action(out, j, AGACT_TOOL_ASK); found = true; }
        }
        else if (j->ended_notice && j->owner_serial) { j->ended_notice = false; fill_action(out, j, AGACT_ENDED); found = true; }
    }
    pthread_mutex_unlock(&g_mu);
    if (found) atomic_store_explicit(&g_main_flag, true, memory_order_release);
    return found;
}

bool agentctl_job_describe(u32 job, AgentctlAction *out) {
    if (!out) return false;
    bool ok = false;
    pthread_mutex_lock(&g_mu);
    Job *j = job_find(job);
    if (j && j->state == AGJ_PENDING_APPROVAL) {
        fill_action(out, j, AGACT_ASK);
        ok = true;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void agentctl_answer(u32 job, bool approved, const char *owner_label) {
    agentctl_answer_ex(job, approved, owner_label, false);
}

/* Under g_mu: `j`, waiting for the user, is let through. */
static void approve_locked(Job *j, const char *owner_label) {
    snprintf(j->owner_label, sizeof j->owner_label, "%s", owner_label ? owner_label : "");
    j->seq = g_next_seq++;
    j->state = AGJ_QUEUED;
    j->meta_dirty = true;
}

void agentctl_answer_ex(u32 job, bool approved, const char *owner_label, bool trust_pane) {
    bool woke = false;
    pthread_mutex_lock(&g_mu);
    Job *j = job_find(job);
    if (j && j->state == AGJ_PENDING_APPROVAL) {
        if (!approved) {
            snprintf(j->owner_label, sizeof j->owner_label, "%s", owner_label ? owner_label : "");
            j->state = AGJ_DECLINED;
            snprintf(j->error, sizeof j->error, "The user declined.");
            free(j->prompt);
            j->prompt = NULL;
            j->prompt_len = 0;
        } else {
            approve_locked(j, owner_label);
            if (trust_pane) {
                u64 pane = j->owner_serial;
                i32 slot = -1;
                for (i32 i = 0; i < AGT_TRUST_MAX && slot < 0; i++) if (g_trust[i].pane == pane) slot = i;
                for (i32 i = 0; i < AGT_TRUST_MAX && slot < 0; i++) if (!g_trust[i].pane) slot = i;
                if (slot >= 0) {
                    g_trust[slot].pane = pane;
                    if (g_trust[slot].perm < j->perm) g_trust[slot].perm = j->perm;
                    /* what this pane asked for meanwhile goes too, up to that level */
                    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
                        Job *o = &g_jobs[i];
                        if (o == j || !o->id || o->owner_serial != pane || o->state != AGJ_PENDING_APPROVAL ||
                            o->perm > g_trust[slot].perm)
                            continue;
                        o->ask = false;
                        o->dismiss = true;
                        approve_locked(o, owner_label);
                        o->started_notice = true;
                        woke = true;
                    }
                }
            }
            promote_queue();
        }
        bump_gen();
    }
    pthread_mutex_unlock(&g_mu);
    kick_server();
    if (woke) wake_main();
}

i32 agentctl_pane_trust(u64 pane) {
    pthread_mutex_lock(&g_mu);
    i32 t = trust_of(pane);
    pthread_mutex_unlock(&g_mu);
    return t;
}

void agentctl_pane_untrust(u64 pane) {
    pthread_mutex_lock(&g_mu);
    for (i32 i = 0; i < AGT_TRUST_MAX; i++) if (g_trust[i].pane == pane) memset(&g_trust[i], 0, sizeof g_trust[i]);
    pthread_mutex_unlock(&g_mu);
}

bool agentctl_tool_answer(u32 job, const char *decision) {
    if (!decision || (strcmp(decision, "allow") && strcmp(decision, "always") && strcmp(decision, "deny"))) return false;
    bool ok = false;
    pthread_mutex_lock(&g_mu);
    Job *j = job_find(job);
    if (j && j->asking && !j->answer_req && j->runner >= 0) {
        snprintf(j->answer_dec, sizeof j->answer_dec, "%s", decision);
        j->answer_req = true;
        bump_gen();
        ok = true;
    }
    pthread_mutex_unlock(&g_mu);
    if (ok) kick_server();
    return ok;
}

bool agentctl_request_merge(u32 job, bool merge, char *why, usize why_cap) {
    char tmp[200];
    pthread_mutex_lock(&g_mu);
    Job *j = job_find(job);
    bool ok = j && merge_request_locked(j, merge, tmp, sizeof tmp);
    if (!j) snprintf(tmp, sizeof tmp, "No such job.");
    pthread_mutex_unlock(&g_mu);
    if (!ok && why && why_cap) snprintf(why, why_cap, "%s", tmp);
    if (ok) kick_server();
    return ok;
}

u32 agentctl_user_spawn(const AgentctlUserSpawn *u, char *why, usize why_cap) {
    const char *said = NULL;
    char buf[200];
    if (!u || !u->owner_serial) said = "Start it from a local terminal pane.";
    else if (!agentctl_agent_info(u->agent)) said = "Unknown agent.";
    else if (u->perm < 0 || u->perm >= AGENTCTL_PERM_COUNT) said = "Unknown permission level.";
    else if (!agentctl_model_ok(u->model ? u->model : "") || !agentctl_effort_ok(u->effort ? u->effort : ""))
        said = "That model or effort is not allowed.";
    else if (!u->cwd || u->cwd[0] != '/') said = "The folder is not known.";
    else if ((!u->prompt || !u->prompt_len) && !u->review_of) said = "Write what it should do.";
    u32 id = 0;
    if (!said) {
        char cwd[1024];
        snprintf(cwd, sizeof cwd, "%s", u->cwd);
        canon_path(cwd, sizeof cwd);
        pthread_mutex_lock(&g_mu);
        if (!g_policy.enabled) said = "Agent workflows are turned off (Settings > Terminal).";
        else if (u->perm > g_policy.perm_ceiling) {
            snprintf(buf, sizeof buf, "Liu allows delegated agents at most '%s' (Settings > Terminal).",
                     agentctl_perm_name(g_policy.perm_ceiling));
            said = buf;
        } else {
            JobNew jn;
            memset(&jn, 0, sizeof jn);
            jn.owner = u->owner_serial;
            jn.agent = u->agent;
            jn.perm = u->perm;
            jn.model = u->model;
            jn.effort = u->effort;
            jn.cwd = cwd;
            jn.label = u->label;
            jn.prompt = (const u8 *)(u->prompt ? u->prompt : "");
            jn.plen = (u32)(u->prompt_len < AGENTCTL_MAX_PROMPT ? u->prompt_len : AGENTCTL_MAX_PROMPT);
            jn.review_of = u->review_of;
            jn.isolate = u->isolate;
            jn.ask_tools = g_policy.ask_tools;
            jn.approved = true;
            jn.owner_label = "you";
            u64 code = AGC_OK;
            Job *j = job_new(&jn, &code, &said);
            if (j) { id = j->id; said = NULL; }
            else if (said) { snprintf(buf, sizeof buf, "%s", said); said = buf; }
        }
        pthread_mutex_unlock(&g_mu);
    }
    if (said && why && why_cap) snprintf(why, why_cap, "%s", said);
    if (id) kick_server();
    return id;
}

void agentctl_pane_identity(u64 pane, const char *session) {
    if (!pane || !g_running) return;
    char sess[AGENTCTL_SESSION_CAP] = "";
    if (session && agentctl_session_ok(session)) snprintf(sess, sizeof sess, "%s", session);
    bool adopted = false;
    pthread_mutex_lock(&g_mu);
    i32 slot = -1, free_slot = -1;
    for (i32 i = 0; i < AGT_IDENT_MAX; i++) {
        if (g_ident[i].pane == pane) { slot = i; break; }
        if (!g_ident[i].pane && free_slot < 0) free_slot = i;
    }
    if (slot < 0 && sess[0]) slot = free_slot >= 0 ? free_slot : 0;
    if (slot >= 0) {
        if (sess[0]) { g_ident[slot].pane = pane; snprintf(g_ident[slot].session, sizeof g_ident[slot].session, "%s", sess); }
        else memset(&g_ident[slot], 0, sizeof g_ident[slot]);
    }
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS && sess[0]; i++) {
        Job *j = &g_jobs[i];
        if (!j->id) continue;
        if (j->owner_serial == pane && strcmp(j->owner_key, sess)) {
            snprintf(j->owner_key, sizeof j->owner_key, "%s", sess);   /* its lead moved to a new session */
            j->meta_dirty = true;
        }
        /* a job an earlier Liu left: it joins the pane whose agent resumed
         * the session that led it */
        if (!j->owner_serial && j->restored && !strcmp(j->owner_key, sess)) {
            j->owner_serial = pane;
            lead_of(pane, true);
            adopted = true;
        }
    }
    if (adopted) bump_gen();
    pthread_mutex_unlock(&g_mu);
    if (adopted) { kick_server(); wake_main(); }
}

void agentctl_set_state_dir(const char *dir) {
    snprintf(g_state_dir, sizeof g_state_dir, "%s", dir && dir[0] == '/' ? dir : "");
}

bool agentctl_session_delegated(const char *dir, const char *session, char *out, usize cap) {
    if (out && cap) out[0] = '\0';
    char *list = agentctl_delegated_read(dir);
    if (!list) return false;
    char what[300], agent[32];
    bool found = agentctl_delegated_lookup(list, session, what, sizeof what, agent, sizeof agent);
    free(list);
    if (found && out && cap) {
        const AgentctlAgentInfo *ai = agentctl_agent_info(agentctl_agent_parse(agent));
        snprintf(out, cap, "%s%s%s", ai ? ai->display : "agent", what[0] ? " \xC2\xB7 " : "", what);
    }
    return found;
}

bool agentctl_pane_leads(u64 pane) {
    if (!pane) return false;
    for (i32 i = 0; i < AGT_LEADS; i++)
        if (atomic_load_explicit(&g_lead_panes[i], memory_order_acquire) == pane) return true;
    return false;
}

typedef struct { u64 h[AGT_LEAD_NAMES]; u16 n[AGT_LEAD_NAMES]; i32 count; } ScreenNames;

static void screen_name(void *ctx, const char *path) {
    ScreenNames *sn = ctx;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!base[0] || !strchr(base, '.')) return;            /* a file's name, not a word */
    u64 h = path_hash(base);
    for (i32 i = 0; i < sn->count; i++) if (sn->h[i] == h) { if (sn->n[i] < 0xFFFF) sn->n[i]++; return; }
    if (sn->count < AGT_LEAD_NAMES) { sn->h[sn->count] = h; sn->n[sn->count] = 1; sn->count++; }
}

void agentctl_lead_screen(u64 pane, const char *text, usize len) {
    if (!pane || !text || !g_running) return;
    ScreenNames *sn = calloc(1, sizeof *sn);
    char *copy = malloc(len + 1);
    if (!sn || !copy) { free(sn); free(copy); return; }
    memcpy(copy, text, len);
    copy[len] = '\0';
    agentctl_text_names(copy, screen_name, sn);
    free(copy);
    i64 now = now_ns();
    pthread_mutex_lock(&g_mu);
    Lead *l = lead_of(pane, false);
    if (l) {
        /* names gone from the screen count from zero; one shown more often
         * than before appeared anew */
        for (i32 i = 0; i < AGT_LEAD_NAMES; i++) {
            if (!l->names[i].hash) continue;
            bool on = false;
            for (i32 k = 0; k < sn->count && !on; k++) on = sn->h[k] == l->names[i].hash;
            if (!on) l->names[i].count = 0;
        }
        for (i32 k = 0; k < sn->count; k++) {
            i32 at = -1, free_slot = -1, oldest = 0;
            for (i32 i = 0; i < AGT_LEAD_NAMES; i++) {
                if (l->names[i].hash == sn->h[k]) { at = i; break; }
                if (!l->names[i].hash && free_slot < 0) free_slot = i;
                if (l->names[i].rise_ns < l->names[oldest].rise_ns) oldest = i;
            }
            if (at < 0) {
                at = free_slot >= 0 ? free_slot : oldest;
                l->names[at].hash = sn->h[k];
                l->names[at].count = 0;
                l->names[at].rise_ns = 0;
            }
            if (sn->n[k] > l->names[at].count) l->names[at].rise_ns = now;
            l->names[at].count = sn->n[k];
        }
    }
    pthread_mutex_unlock(&g_mu);
    free(sn);
}

void agentctl_pane_busy(u64 pane, bool busy) {
    if (!pane || !g_running) return;
    pthread_mutex_lock(&g_mu);
    PaneBusy *b = NULL;
    for (i32 i = 0; i < AGT_BUSY_PANES && !b; i++) if (g_busy[i].pane == pane) b = &g_busy[i];
    if (!b) {
        if (!busy) { pthread_mutex_unlock(&g_mu); return; }
        /* a free slot, else the one quiet longest */
        b = &g_busy[0];
        for (i32 i = 0; i < AGT_BUSY_PANES; i++) {
            if (!g_busy[i].pane) { b = &g_busy[i]; break; }
            i64 last = g_busy[i].until_ns ? g_busy[i].until_ns : INT64_MAX;
            i64 bl = b->until_ns ? b->until_ns : INT64_MAX;
            if (last < bl) b = &g_busy[i];
        }
        memset(b, 0, sizeof *b);
        b->pane = pane;
    }
    i64 now = now_ns();
    if (busy && (!b->since_ns || b->until_ns)) {
        b->prev_since_ns = b->since_ns;
        b->prev_until_ns = b->until_ns;
        b->since_ns = now;
        b->until_ns = 0;
    } else if (!busy && b->since_ns && !b->until_ns) {
        b->until_ns = now;
    }
    pthread_mutex_unlock(&g_mu);
}

void agentctl_pane_gone(u64 pane_serial) {
    if (!pane_serial || g_exiting) return;
    bool main_work = false;
    pthread_mutex_lock(&g_mu);
    lead_mark(pane_serial, false);
    for (i32 i = 0; i < AGT_TRUST_MAX; i++) if (g_trust[i].pane == pane_serial) memset(&g_trust[i], 0, sizeof g_trust[i]);
    for (i32 i = 0; i < AGT_IDENT_MAX; i++) if (g_ident[i].pane == pane_serial) memset(&g_ident[i], 0, sizeof g_ident[i]);
    for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
        Job *j = &g_jobs[i];
        if (!j->id || j->owner_serial != pane_serial) continue;
        if (j->state == AGJ_PENDING_APPROVAL) {
            j->state = AGJ_DECLINED;
            snprintf(j->error, sizeof j->error, "the pane that asked was closed");
            j->ask = false;
            j->dismiss = true;
            bump_gen();
            main_work = true;
        } else if (j->state != AGJ_ENDED && j->state != AGJ_DECLINED) {
            job_end(j, AGE_CLOSED, "the pane that asked was closed");
            main_work = true;
        }
    }
    pthread_mutex_unlock(&g_mu);
    kick_server();
    if (main_work) wake_main();
}

void agentctl_request_stop(u32 job) {
    pthread_mutex_lock(&g_mu);
    Job *j = job_find(job);
    if (j && j->state == AGJ_RUNNING && j->runner >= 0) j->send_stop = true;
    pthread_mutex_unlock(&g_mu);
    kick_server();
}

AgentctlSendResult agentctl_request_send(u32 job, const char *text, usize len, char *why, usize why_cap) {
    if (why && why_cap) why[0] = '\0';
    if (agentctl_job_watched(job)) {
        if (why && why_cap) snprintf(why, why_cap, "Its own agent runs this subagent; Liu only shows it.");
        return AGENTCTL_SEND_REFUSED;
    }
    /* what was typed, without the blank lines and spaces around it */
    while (len > 0 && (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')) { text++; len--; }
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    const char *said = NULL;
    u64 code = AGC_NOTFOUND;
    pthread_mutex_lock(&g_mu);
    Job *j = job_find(job);
    if (!j) said = "No such job.";
    else code = job_queue_turn(j, (const u8 *)text, (u32)(len > AGENTCTL_MAX_PROMPT + 1u ? AGENTCTL_MAX_PROMPT + 1u : len), &said);
    pthread_mutex_unlock(&g_mu);
    if (code == AGC_OK) { kick_server(); return AGENTCTL_SENT; }
    if (why && why_cap && said) snprintf(why, why_cap, "%s", said);
    return code == AGC_BUSY ? AGENTCTL_SEND_BUSY : AGENTCTL_SEND_REFUSED;
}

void agentctl_request_close(u32 job) {
    if (agentctl_job_watched(job)) {
        /* only its tab goes: the agent that runs it carries on */
        const AgentctlWatchedView *v = watched_view();
        if (v && v->dismiss) v->dismiss(job);
        return;
    }
    pthread_mutex_lock(&g_mu);
    Job *j = job_find(job);
    if (j) {
        if (j->state == AGJ_PENDING_APPROVAL) {
            j->state = AGJ_DECLINED;
            snprintf(j->error, sizeof j->error, "The user declined.");
            bump_gen();
        } else if (j->state == AGJ_ENDED) {
            j->end = AGE_CLOSED;      /* dismissed: its tab goes */
            bump_gen();
        } else {
            job_end(j, AGE_CLOSED, NULL);
        }
    }
    pthread_mutex_unlock(&g_mu);
    kick_server();
}

bool agentctl_takeover(u32 job, AgentctlTakeover *out) {
    if (!out) return false;
    bool ok = false;
    pthread_mutex_lock(&g_mu);
    Job *j = job_find(job);
    if (j && j->state == AGJ_IDLE && j->session[0]) {
        memset(out, 0, sizeof *out);
        out->agent = j->agent;
        out->perm = j->perm;
        snprintf(out->model, sizeof out->model, "%s", j->model);
        snprintf(out->effort, sizeof out->effort, "%s", j->effort);
        snprintf(out->session, sizeof out->session, "%s", j->session);
        snprintf(out->cwd, sizeof out->cwd, "%s", j->cwd);
        out->owner_serial = j->owner_serial;
        job_end(j, AGE_TAKEN_OVER, NULL);
        ok = true;
    }
    pthread_mutex_unlock(&g_mu);
    kick_server();
    return ok;
}

/* Under g_mu: whether the job is shown. Declined, closed and taken-over
 * jobs are not; one that ended otherwise is, until it is dismissed. */
static bool job_shown(const Job *j) {
    if (!j->id || j->state == AGJ_DECLINED || !j->owner_serial) return false;
    return !(j->state == AGJ_ENDED && (j->end == AGE_CLOSED || j->end == AGE_TAKEN_OVER ||
                                       j->end == AGE_MERGED || j->end == AGE_DISCARDED));
}

i32 agentctl_jobs_brief(AgentctlJobBrief *out, i32 cap) {
    if (!out || cap <= 0) return 0;
    i32 n = 0;
    const AgentctlWatchedView *v = watched_view();
    if (!any_delegated()) return v && v->brief ? v->brief(out, cap) : 0;
    pthread_mutex_lock(&g_mu);
    u32 last = 0;
    for (;;) {
        Job *best = NULL;
        for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
            Job *j = &g_jobs[i];
            if (!job_shown(j) || j->id <= last) continue;
            if (!best || j->id < best->id) best = j;
        }
        if (!best || n >= cap) break;
        last = best->id;
        AgentctlJobBrief *b = &out[n++];
        memset(b, 0, sizeof *b);
        b->job = best->id;
        b->owner_serial = best->owner_serial;
        b->agent = best->agent;
        b->state = best->state;
        b->outcome = best->outcome;
        b->end = best->end;
        snprintf(b->label, sizeof b->label, "%s", best->label);
        b->turn_start_ms = best->turn_start_ms;
        b->turn_end_ms = best->turn_end_ms;
        b->asking = best->asking;
        b->isolate = best->isolate;
    }
    pthread_mutex_unlock(&g_mu);
    if (v && v->brief && n < cap) n += v->brief(out + n, cap - n);
    return n;
}

bool agentctl_job_card(u32 job, AgentctlCard *out) {
    if (!job || !out) return false;
    if (agentctl_job_watched(job)) {
        const AgentctlWatchedView *v = watched_view();
        return v && v->card && v->card(job, out);
    }
    if (!any_delegated()) return false;
    bool found = false;
    pthread_mutex_lock(&g_mu);
    Job *j = job_find(job);
    if (j && job_shown(j)) {
        found = true;
        memset(out, 0, sizeof *out);
        out->job = j->id;
        out->agent = j->agent;
        out->state = j->state;
        out->outcome = j->outcome;
        out->end = j->end;
        snprintf(out->label, sizeof out->label, "%s", j->label);
        out->created_ms = j->created_ms;
        out->turn_start_ms = j->turn_start_ms;
        out->turn_end_ms = j->turn_end_ms;
        agentctl_ring_card_lines(&j->ring, out);
    }
    pthread_mutex_unlock(&g_mu);
    return found;
}

i32 agentctl_owner_cards(u64 owner, AgentctlCard *out, i32 cap) {
    if (!owner || !out || cap <= 0) return 0;
    i32 n = 0;
    const AgentctlWatchedView *v = watched_view();
    if (!any_delegated()) return v && v->owner_cards ? v->owner_cards(owner, out, cap) : 0;
    pthread_mutex_lock(&g_mu);
    /* oldest first: by id */
    u32 last = 0;
    for (;;) {
        Job *best = NULL;
        for (i32 i = 0; i < AGENTCTL_MAX_JOBS; i++) {
            Job *j = &g_jobs[i];
            if (!job_shown(j) || j->owner_serial != owner || j->id <= last) continue;
            if (!best || j->id < best->id) best = j;
        }
        if (!best || n >= cap) break;
        last = best->id;
        AgentctlCard *c = &out[n++];
        memset(c, 0, sizeof *c);
        c->job = best->id;
        c->agent = best->agent;
        c->state = best->state;
        c->outcome = best->outcome;
        c->end = best->end;
        snprintf(c->label, sizeof c->label, "%s", best->label);
        c->created_ms = best->created_ms;
        c->turn_start_ms = best->turn_start_ms;
        c->turn_end_ms = best->turn_end_ms;
        agentctl_ring_card_lines(&best->ring, c);
    }
    pthread_mutex_unlock(&g_mu);
    if (v && v->owner_cards && n < cap) n += v->owner_cards(owner, out + n, cap - n);
    return n;
}

bool agentctl_job_snapshot(u32 job, AgentctlSnapshot *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (agentctl_job_watched(job)) {
        const AgentctlWatchedView *v = watched_view();
        return v && v->snapshot && v->snapshot(job, out);
    }
    bool ok = false;
    pthread_mutex_lock(&g_mu);
    Job *j = job_find(job);
    if (j) {
        out->job = j->id;
        out->agent = j->agent;
        out->perm = j->perm;
        out->state = j->state;
        out->outcome = j->outcome;
        out->end = j->end;
        snprintf(out->model, sizeof out->model, "%s", j->model);
        snprintf(out->effort, sizeof out->effort, "%s", j->effort);
        snprintf(out->cwd, sizeof out->cwd, "%s", j->cwd);
        snprintf(out->label, sizeof out->label, "%s", j->label);
        snprintf(out->owner_label, sizeof out->owner_label, "%s", j->owner_label);
        snprintf(out->session, sizeof out->session, "%s", j->session);
        snprintf(out->error, sizeof out->error, "%s", j->error);
        snprintf(out->usage, sizeof out->usage, "%s", j->usage);
        out->turns = j->turns;
        out->turn_start_ms = j->turn_start_ms;
        out->turn_end_ms = j->turn_end_ms;
        out->owner_serial = j->owner_serial;
        out->spent_in = j->spent_in;
        out->spent_cached = j->spent_cached;
        out->spent_out = j->spent_out;
        out->spent_cost_micro = j->spent_cost;
        out->max_turns = j->max_turns;
        out->deadline_s = j->deadline_s;
        out->max_tokens = j->max_tokens;
        out->budget_micro = j->budget_micro;
        snprintf(out->worktree, sizeof out->worktree, "%s", j->wt);
        snprintf(out->branch, sizeof out->branch, "%s", j->wt_branch);
        snprintf(out->merged, sizeof out->merged, "%s", j->merged);
        if (j->asking) snprintf(out->asking, sizeof out->asking, "%s", j->ask_what);
        out->review_of = j->review_of;
        for (u8 k = 0; k < j->nafter && k < 4; k++) out->after[k] = j->after[k];
        out->nfiles = (i32)j->ch.n;
        out->nclashes = j->nclash;
        out->restored = j->restored;
        snprintf(out->dir, sizeof out->dir, "%s", j->dir);
        usize tw = 0;
        for (i32 i = 0; i < AGENTCTL_MAX_JOBS && tw < sizeof out->team; i++) {
            const Job *o = &g_jobs[i];
            if (o == j || o->owner_serial != j->owner_serial || !job_shown(o)) continue;
            char name[160];
            member_name(j->owner_serial, o->id, name, sizeof name);
            tw += (usize)snprintf(out->team + tw, sizeof out->team - tw, "%s%s", tw ? ", " : "", name);
        }
        agentctl_ring_snapshot_lines(&j->ring, out);
        ok = true;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void agentctl_snapshot_free(AgentctlSnapshot *s) {
    if (!s) return;
    free(s->lines);
    free(s->bytes);
    s->lines = NULL;
    s->bytes = NULL;
    s->nlines = 0;
}
