/*
 * Liu — `liu-agent run`: the runner of a delegated agent.
 *
 * Liu starts it as a background process — no terminal, /dev/null on its
 * standard descriptors — with the job's number in LIU_AGENT_JOB. The runner
 * asks Liu for the job (agent, model, effort, permission level, folder, the
 * task), then runs the agent one turn at a time in its structured mode
 * (agentctl_adapter.h) and sends Liu one line per event (agentctl_stream.h),
 * which Liu shows in the job tab's hover and its log. Between turns no agent
 * process exists; the runner sleeps in poll() until Liu hands it the next
 * message, a stop, or the end.
 *
 * The agent runs in a process group of its own, so a stop reaches it and
 * everything it started. Every piece of text from the agent is sanitized
 * before it leaves the runner. When Liu's socket closes (Liu quit, crashed, or
 * ended the job) the agent is stopped and the runner exits.
 *
 * Files, in the job's private folder (0700, made by Liu): result.md — the
 * last turn's final message, replaced whole after every turn —, events.log,
 * every line sent, without styling, and turn.state, how the last turn ended
 * (its session, outcome and spend), which the next Liu reads when this one
 * quit while the turn ran and the job was to finish it.
 *
 * An isolated job (`--isolate`) works in a git worktree of its own, made here
 * before its first turn from the folder's current state (tracked changes
 * included, `git stash create`); its work is merged back or thrown away on
 * Liu's word between turns, which ends the job.
 *
 * A Claude job that may ask about tools reads its message as a JSON line on
 * stdin and writes its permission prompts on stdout; each is passed to Liu,
 * which puts it to the user, and the answer goes back on the agent's stdin.
 */
#include "agentctl/agentctl_adapter.h"
#include "agentctl/agentctl_proto.h"
#include "agentctl/agentctl_stream.h"
#include "agentctl/agentctl_team.h"
#include "notify/socket.h"

#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

typedef struct {
    u32  job;
    i32  agent, perm;
    char model[AGENTCTL_MODEL_CAP];
    char effort[AGENTCTL_EFFORT_CAP];
    char cwd[1024];
    char label[AGENTCTL_LABEL_CAP];
    char owner[96];
    char dir[512];
    char session[AGENTCTL_SESSION_CAP];
    char *prompt;
    char *team;             /* the first turn's team notes, NULL for none */
    u32  turns;
    bool isolate;           /* works in a worktree of its own */
    bool keep;              /* a turn running when Liu quits is finished */
    bool ask_tools;         /* Claude: tools its level refuses are asked of the user */
    u32  deadline_s;        /* one turn's time limit, 0 = none */
    u64  budget_micro;      /* what it may still spend, 0 = no limit */
    char helpers[1024];     /* where `liu-agent` is, for its hooks */
    char wt[1024];          /* its worktree's top, "" = none */
    char wt_branch[128];
    char wt_base[64];
    char origin[1024];      /* the folder it was started from (an isolated job's own top is wt) */
    char origin_top[1024];  /* that folder's repository top */
} Spec;

static int  g_sock = -1;
static int  g_sig[2] = { -1, -1 };
static int  g_log = -1;
static volatile sig_atomic_t g_got_int = 0, g_got_quit = 0, g_got_chld = 0;
static u32  g_job = 0;

/* -------------------------------------------------------------- output --- */

static void log_line(const char *text, usize len) {
    if (g_log < 0) return;
    (void)!write(g_log, text, len);
    (void)!write(g_log, "\n", 1);
}

/* Large enough for a long agent message; anything past it is in events.log. */
#define LINE_SEND_MAX (64u * 1024u)

/* The stream's emit: to Liu as a line of the job's log, and into events.log.
 * The text is already sanitized by the stream. */
static void emit(void *ctx, AgentctlStyle st, const char *text, usize len) {
    (void)ctx;
    log_line(text, len);
    if (g_sock < 0) return;
    usize n = len < LINE_SEND_MAX ? len : LINE_SEND_MAX;
    while (n > 0 && n < len && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_R_LINE, g_job)) return;
    agentctl_msg_put_u64(&m, AGK_STYLE, (u64)st);
    agentctl_msg_put(&m, AGK_TEXT, text, (u32)n);
    if (agentctl_msg_end(&m)) (void)agentctl_write_all(g_sock, m.buf, m.len);
    agentctl_msg_free(&m);
}

static void say(AgentctlStyle st, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(AgentctlStyle st, const char *fmt, ...) {
    char raw[2048], clean[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof raw, fmt, ap);
    va_end(ap);
    usize n = agentctl_sanitize(raw, strlen(raw), clean, sizeof clean, true);
    emit(NULL, st, clean, n);
}

/* ------------------------------------------------------------ signals --- */

static void on_signal(int sig) {
    int saved = errno;
    if (sig == SIGINT) g_got_int = 1;
    else if (sig == SIGCHLD) g_got_chld = 1;
    else g_got_quit = 1;
    if (g_sig[1] >= 0) { u8 b = (u8)sig; (void)!write(g_sig[1], &b, 1); }
    errno = saved;
}

static void signals_setup(void) {
    if (pipe(g_sig) == 0) {
        for (int i = 0; i < 2; i++) {
            fcntl(g_sig[i], F_SETFL, fcntl(g_sig[i], F_GETFL) | O_NONBLOCK);
            fcntl(g_sig[i], F_SETFD, FD_CLOEXEC);
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
}

static void drain_sig(void) {
    u8 b[32];
    while (read(g_sig[0], b, sizeof b) > 0) {}
}

/* ------------------------------------------------------------ messages --- */

static bool send_frame_text(u16 op, u32 job, const char *session, const char *usage,
                            const char *error, bool stopped, const char *text) {
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, op, job)) return false;
    if (text && text[0])       agentctl_msg_put_str(&m, AGK_TEXT, text);
    if (session && session[0]) agentctl_msg_put_str(&m, AGK_SESSION, session);
    if (usage && usage[0])     agentctl_msg_put_str(&m, AGK_USAGE, usage);
    if (error && error[0])     agentctl_msg_put_str(&m, AGK_ERROR, error);
    if (stopped)               agentctl_msg_put_str(&m, AGK_STOPPED, "1");
    bool ok = agentctl_msg_end(&m) && agentctl_write_all(g_sock, m.buf, m.len);
    agentctl_msg_free(&m);
    return ok;
}

static bool send_frame(u16 op, u32 job, const char *session, const char *usage,
                       const char *error, bool stopped) {
    return send_frame_text(op, job, session, usage, error, stopped, NULL);
}

/* The job's outbox (LIU_AGENT_OUTBOX): where `liu-agent post` puts a post
 * when the agent's sandbox keeps it from Liu's socket — one "to<TAB>text"
 * line each, appended. What has been relayed is remembered by its offset;
 * the file is never cut, so a line written while it is read is not lost. */
static char  g_outbox[600];
static off_t g_outbox_off = 0;

static void relay_outbox(void) {
    if (!g_outbox[0] || g_sock < 0) return;
    int fd = open(g_outbox, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= g_outbox_off ||
        lseek(fd, g_outbox_off, SEEK_SET) != g_outbox_off) {
        close(fd);
        return;
    }
    char buf[AGT_OUTBOX_MAX];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char *line = buf;
    for (;;) {
        char *nl = strchr(line, '\n');
        if (!nl) break;                               /* a line still being written: next time */
        *nl = '\0';
        g_outbox_off += (off_t)(nl - line) + 1;
        char *tab = strchr(line, '\t');
        if (tab && tab[1]) {
            *tab = '\0';
            AgentctlMsg m;
            if (agentctl_msg_init(&m, AGOP_R_POST, g_job)) {
                agentctl_msg_put_str(&m, AGK_TO, line);
                agentctl_msg_put_str(&m, AGK_TEXT, tab + 1);
                if (agentctl_msg_end(&m)) (void)agentctl_write_all(g_sock, m.buf, m.len);
                agentctl_msg_free(&m);
            }
        }
        line = nl + 1;
    }
}

/* A file the agent changed, for its team (agentctl_team.h): named by the
 * agent's own tool, or `seen` changing while one of its commands ran. */
static void send_file(const char *path, usize len, bool seen) {
    if (g_sock < 0 || len == 0) return;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_R_FILE, g_job)) return;
    agentctl_msg_put(&m, AGK_PATH, path, (u32)len);
    if (seen) agentctl_msg_put_str(&m, AGK_SEEN, "1");
    if (agentctl_msg_end(&m)) (void)agentctl_write_all(g_sock, m.buf, m.len);
    agentctl_msg_free(&m);
}

static void on_file(void *ctx, const char *path, usize len) {
    (void)ctx;
    send_file(path, len, false);
}

/* ------------------------------------------------ files a command wrote --- */

/* A command an agent runs through its shell can change files no tool names
 * (a script, `sed -i`, `cp`). When one ends, each path its line names — in
 * the folder its shell was in — that lies in the job's folder and whose
 * status changed while it ran counts as changed by it; a named folder that
 * changed gives its files that did. A line that only reads is not looked at,
 * nor is anything it does not name, so what another member writes in the
 * same seconds stays theirs. */
#define OBS_WINS       8
#define OBS_SLACK_NS   500000000LL      /* before the start Liu heard of */
#define OBS_DIR_SCAN   512              /* entries read from one named folder */
#define OBS_REPORTS    32               /* files one command can be credited with */

typedef struct {
    char id[64];
    i64  start_ns;
    bool used;
} ObsWin;

static ObsWin      g_obs[OBS_WINS];
static const char *g_obs_cwd = "";

typedef struct {
    char (*names)[AGT_PATH_MAX];
    u32   n;
    char (*told)[AGT_PATH_MAX];
    u32   ntold;
    i64   from_ns, to_ns;
} ObsRun;

static i64 real_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (i64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static i64 status_ns(const struct stat *st) {
#ifdef __APPLE__
    return (i64)st->st_ctimespec.tv_sec * 1000000000LL + st->st_ctimespec.tv_nsec;
#else
    return (i64)st->st_ctim.tv_sec * 1000000000LL + st->st_ctim.tv_nsec;
#endif
}

static void obs_name(void *ctx, const char *path) {
    ObsRun *r = ctx;
    char abs[AGT_PATH_MAX];
    if (r->n >= AGC_NAMES_MAX || !agentctl_team_path(g_obs_cwd, path, strlen(path), abs, sizeof abs)) return;
    if (!agentctl_team_under(abs, g_obs_cwd) || !strcmp(abs, g_obs_cwd)) return;
    for (u32 i = 0; i < r->n; i++) if (!strcmp(r->names[i], abs)) return;
    memcpy(r->names[r->n++], abs, strlen(abs) + 1);
}

static void obs_tell(ObsRun *r, const char *abs) {
    if (r->ntold >= OBS_REPORTS) return;
    for (u32 i = 0; i < r->ntold; i++) if (!strcmp(r->told[i], abs)) return;
    memcpy(r->told[r->ntold++], abs, strlen(abs) + 1);
    send_file(abs, strlen(abs), true);
}

static bool obs_fresh(const struct stat *st, const ObsRun *r) {
    i64 t = status_ns(st);
    return t >= r->from_ns && t <= r->to_ns;
}

static void obs_check(const char *cmd, const char *dir, i64 start_ns) {
    if (g_sock < 0 || !cmd || !g_obs_cwd[0] || agentctl_command_reads_only(cmd)) return;
    ObsRun r = { 0 };
    r.names = malloc(sizeof(char[AGT_PATH_MAX]) * AGC_NAMES_MAX);
    r.told = malloc(sizeof(char[AGT_PATH_MAX]) * OBS_REPORTS);
    if (!r.names || !r.told) { free(r.names); free(r.told); return; }
    r.from_ns = start_ns - OBS_SLACK_NS;
    r.to_ns = real_ns() + OBS_SLACK_NS / 5;
    agentctl_command_names(cmd, dir, obs_name, &r);
    for (u32 i = 0; i < r.n && r.ntold < OBS_REPORTS; i++) {
        struct stat st;
        if (lstat(r.names[i], &st) != 0 || !obs_fresh(&st, &r)) continue;
        if (!S_ISDIR(st.st_mode)) { obs_tell(&r, r.names[i]); continue; }
        /* a named folder whose entries changed: the files in it that did */
        const char *base = strrchr(r.names[i], '/');
        if (base && !strcmp(base + 1, ".git")) continue;
        DIR *d = opendir(r.names[i]);
        if (!d) continue;
        struct dirent *e;
        u32 seen = 0;
        while ((e = readdir(d)) && seen++ < OBS_DIR_SCAN && r.ntold < OBS_REPORTS) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".git")) continue;
            char full[AGT_PATH_MAX];
            int w = snprintf(full, sizeof full, "%s/%s", r.names[i], e->d_name);
            struct stat es;
            if (w <= 0 || (usize)w >= sizeof full || lstat(full, &es) != 0) continue;
            if (!S_ISDIR(es.st_mode) && obs_fresh(&es, &r)) obs_tell(&r, full);
        }
        closedir(d);
    }
    free(r.names);
    free(r.told);
}

/* Liu hears when each command ran, to weigh what changed in the folder
 * meanwhile (agentctl_watch.h). */
static void send_command(const char *id, bool done, const char *cmd) {
    if (g_sock < 0) return;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_R_CMD, g_job)) return;
    agentctl_msg_put_str(&m, AGK_TEXT, id);
    agentctl_msg_put_str(&m, AGK_STATE, done ? "0" : "1");
    if (cmd) {
        usize n = strlen(cmd);
        agentctl_msg_put(&m, AGK_PROMPT, cmd, (u32)(n < 4096 ? n : 4096));
    }
    if (agentctl_msg_end(&m)) (void)agentctl_write_all(g_sock, m.buf, m.len);
    agentctl_msg_free(&m);
}

static void on_command(void *ctx, const char *id, bool done, const char *cmd, const char *dir) {
    (void)ctx;
    if (!id) id = "";
    send_command(id, done, cmd);
    if (!done) {
        ObsWin *w = NULL;
        for (u32 i = 0; i < OBS_WINS && !w; i++) if (!g_obs[i].used) w = &g_obs[i];
        if (!w) {                                         /* full: the one started longest ago goes */
            w = &g_obs[0];
            for (u32 i = 1; i < OBS_WINS; i++) if (g_obs[i].start_ns < w->start_ns) w = &g_obs[i];
        }
        snprintf(w->id, sizeof w->id, "%s", id);
        w->start_ns = real_ns();
        w->used = true;
        return;
    }
    for (u32 i = 0; i < OBS_WINS; i++) {
        if (!g_obs[i].used || strcmp(g_obs[i].id, id)) continue;
        g_obs[i].used = false;
        obs_check(cmd, dir, g_obs[i].start_ns);
        return;
    }
}

/* The head of a final message, one line, for the team to hear when a turn
 * ends: cut on a character boundary to `cap`. */
static void summary_of(const char *text, usize len, char *out, usize cap) {
    out[0] = '\0';
    if (!text || !len || cap < 8) return;
    usize n = agentctl_sanitize(text, len, out, cap, false);
    if (n + 1 >= cap) {
        usize k = cap - 4;
        while (k > 0 && ((unsigned char)out[k] & 0xC0) == 0x80) k--;
        memcpy(out + k, "\xE2\x80\xA6", 3);
        out[k + 3] = '\0';
    }
}

static i64 mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void fmt_duration(i64 ms, char *out, usize cap) {
    i64 s = ms / 1000;
    if (s < 60)        snprintf(out, cap, "%llds", (long long)s);
    else if (s < 3600) snprintf(out, cap, "%lldm%02llds", (long long)(s / 60), (long long)(s % 60));
    else               snprintf(out, cap, "%lldh%02lldm", (long long)(s / 3600), (long long)((s / 60) % 60));
}

/* Replaces result.md whole: written beside it, then renamed over it. */
static void write_result(const Spec *sp, const char *text, usize len) {
    if (!sp->dir[0]) return;
    char tmp[600], dst[600];
    if ((usize)snprintf(tmp, sizeof tmp, "%s/result.md.tmp", sp->dir) >= sizeof tmp) return;
    if ((usize)snprintf(dst, sizeof dst, "%s/result.md", sp->dir) >= sizeof dst) return;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    bool ok = true;
    while (len > 0 && ok) {
        ssize_t w = write(fd, text, len);
        if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
        text += w;
        len -= (usize)w;
    }
    if (close(fd) != 0) ok = false;
    if (ok) rename(tmp, dst);
    else    unlink(tmp);
}

/* How the last turn ended, for the Liu that adopts a job its predecessor
 * left running: one key=value a line, replaced whole. */
static void write_turn_state(const Spec *sp, const char *outcome, const char *error, u64 tin, u64 tcached,
                             u64 tout, u64 cost) {
    if (!sp->dir[0]) return;
    char tmp[600], dst[600], body[2048], err[600];
    if ((usize)snprintf(tmp, sizeof tmp, "%s/turn.state.tmp", sp->dir) >= sizeof tmp) return;
    if ((usize)snprintf(dst, sizeof dst, "%s/turn.state", sp->dir) >= sizeof dst) return;
    agentctl_sanitize(error ? error : "", error ? strlen(error) : 0, err, sizeof err, false);
    int w = snprintf(body, sizeof body,
                     "session=%s\noutcome=%s\nturns=%u\ntokens=%llu %llu %llu\ncost=%llu\nerror=%s\nended=%lld\n",
                     sp->session, outcome, sp->turns, (unsigned long long)tin, (unsigned long long)tcached,
                     (unsigned long long)tout, (unsigned long long)cost, err, (long long)time(NULL));
    if (w <= 0 || (usize)w >= sizeof body) return;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    bool ok = write(fd, body, (usize)w) == w;
    if (close(fd) != 0) ok = false;
    if (ok) rename(tmp, dst);
    else    unlink(tmp);
}

/* ------------------------------------------------------------- git --- */

static int git_run(const char *dir, const char *const *args, char *out, usize ocap, char *err, usize ecap);

/* Runs `git -C dir <args…>` without a shell, with nothing of the
 * repository's own able to run (no hooks, no fsmonitor, no external diff),
 * and returns its exit status (-1 = it could not run). Its first output line
 * goes to `out` (all of it, sanitized, with `whole`); the last error line to
 * `err`. Shared with the command (client.c). */
int agentctl_git(const char *dir, const char *const *args, char *out, usize ocap, char *err, usize ecap, bool whole);
int agentctl_git(const char *dir, const char *const *args, char *out, usize ocap, char *err, usize ecap, bool whole) {
    if (out && ocap) out[0] = '\0';
    if (err && ecap) err[0] = '\0';
    const char *argv[96];
    i32 n = 0;
    argv[n++] = "git";
    argv[n++] = "-C";
    argv[n++] = dir;
    argv[n++] = "-c"; argv[n++] = "core.fsmonitor=false";
    argv[n++] = "-c"; argv[n++] = "core.hooksPath=/dev/null";
    argv[n++] = "-c"; argv[n++] = "diff.external=";
    for (i32 i = 0; args[i] && n < 94; i++) argv[n++] = args[i];
    argv[n] = NULL;
    int o[2], e[2];
    if (pipe(o) != 0) return -1;
    if (pipe(e) != 0) { close(o[0]); close(o[1]); return -1; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, o[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, e[1], STDERR_FILENO);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    sigset_t all;
    sigfillset(&all);
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setsigdefault(&attr, &all);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "git", &fa, &attr, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(o[1]);
    close(e[1]);
    if (rc != 0) { close(o[0]); close(e[0]); return -1; }
    char ob[8192], eb[8192];
    usize on = 0, en = 0;
    struct pollfd p[2] = { { o[0], POLLIN, 0 }, { e[0], POLLIN, 0 } };
    bool oo = true, eo = true;
    while (oo || eo) {
        p[0].fd = oo ? o[0] : -1;
        p[1].fd = eo ? e[0] : -1;
        if (poll(p, 2, -1) < 0 && errno != EINTR) break;
        if (oo && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            char tmp[4096];
            ssize_t r = read(o[0], tmp, sizeof tmp);
            if (r <= 0) oo = false;
            else { usize take = (usize)r < sizeof ob - 1 - on ? (usize)r : sizeof ob - 1 - on; memcpy(ob + on, tmp, take); on += take; }
        }
        if (eo && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char tmp[4096];
            ssize_t r = read(e[0], tmp, sizeof tmp);
            if (r <= 0) eo = false;
            else { usize take = (usize)r < sizeof eb - 1 - en ? (usize)r : sizeof eb - 1 - en; memcpy(eb + en, tmp, take); en += take; }
        }
    }
    close(o[0]);
    close(e[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    ob[on] = '\0';
    eb[en] = '\0';
    if (out && ocap) {
        if (!whole) {
            char *nl = strchr(ob, '\n');
            if (nl) *nl = '\0';
        }
        agentctl_sanitize(ob, strlen(ob), out, ocap, whole);
    }
    if (err && ecap) {
        while (en > 0 && (eb[en - 1] == '\n' || eb[en - 1] == ' ')) eb[--en] = '\0';
        char *last = strrchr(eb, '\n');
        const char *line = last ? last + 1 : eb;
        agentctl_sanitize(line, strlen(line), err, ecap, false);
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* The job's worktree: made from the folder's repository at its current
 * state, on a branch of its own, under ~/.liu/worktrees. The job's folder
 * moves into it (the same place inside the repository). False with `why`. */
static bool make_worktree(Spec *sp, char *why, usize cap) {
    char top[1024], err[512];
    const char *a_top[] = { "rev-parse", "--show-toplevel", NULL };
    if (git_run(sp->cwd, a_top, top, sizeof top, err, sizeof err) != 0 || top[0] != '/') {
        snprintf(why, cap, "--isolate needs a git repository: %s is not in one", sp->cwd);
        return false;
    }
    char base[64];
    const char *a_stash[] = { "stash", "create", NULL };
    (void)git_run(top, a_stash, base, sizeof base, err, sizeof err);       /* "" when nothing changed */
    if (!base[0]) {
        const char *a_head[] = { "rev-parse", "--verify", "HEAD", NULL };
        if (git_run(top, a_head, base, sizeof base, err, sizeof err) != 0 || !base[0]) {
            snprintf(why, cap, "--isolate needs a commit to start from; the repository has none yet");
            return false;
        }
    }
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') { snprintf(why, cap, "no home folder for the worktree"); return false; }
    char root[1100], wt[1100], branch[128];
    snprintf(root, sizeof root, "%s/.liu", home);
    mkdir(root, 0700);
    snprintf(root, sizeof root, "%s/.liu/worktrees", home);
    mkdir(root, 0700);
    const char *name = strrchr(top, '/');
    name = name && name[1] ? name + 1 : "repo";
    unsigned ppid = (unsigned)getppid();
    if ((usize)snprintf(wt, sizeof wt, "%s/%s-job%u-%u", root, name, sp->job, ppid) >= sizeof wt) {
        snprintf(why, cap, "the worktree's path is too long");
        return false;
    }
    snprintf(branch, sizeof branch, "liu/job-%u-%u", sp->job, ppid);
    const char *a_add[] = { "worktree", "add", "--quiet", "-b", branch, wt, base, NULL };
    if (git_run(top, a_add, NULL, 0, err, sizeof err) != 0) {
        snprintf(why, cap, "git could not make the worktree: %s", err[0] ? err : "unknown error");
        return false;
    }
    /* the job's folder, at the same place inside the worktree */
    const char *rel = agentctl_team_rel(sp->cwd, top);
    char inner[1100];
    if (!strcmp(rel, ".") || rel == sp->cwd) snprintf(inner, sizeof inner, "%s", wt);
    else snprintf(inner, sizeof inner, "%s/%s", wt, rel);
    mkdir(inner, 0755);
    snprintf(sp->origin, sizeof sp->origin, "%s", sp->cwd);
    snprintf(sp->origin_top, sizeof sp->origin_top, "%s", top);
    snprintf(sp->wt, sizeof sp->wt, "%s", wt);
    snprintf(sp->wt_branch, sizeof sp->wt_branch, "%s", branch);
    snprintf(sp->wt_base, sizeof sp->wt_base, "%s", base);
    snprintf(sp->cwd, sizeof sp->cwd, "%s", inner);
    return true;
}

/* The worktree and its branch, gone. */
static void drop_worktree(const Spec *sp) {
    char err[512];
    const char *a_rm[] = { "worktree", "remove", "--force", sp->wt, NULL };
    (void)git_run(sp->origin_top, a_rm, NULL, 0, err, sizeof err);
    const char *a_br[] = { "branch", "-D", sp->wt_branch, NULL };
    (void)git_run(sp->origin_top, a_br, NULL, 0, err, sizeof err);
}

/* The job's work (everything it changed since its base, new files
 * included) applied to the folder it was started from; on success the
 * worktree goes. Returns true when the job is over (merged, or nothing to
 * merge); `msg` says what happened. */
static bool merge_worktree(const Spec *sp, char *msg, usize cap) {
    char err[512], patch[600];
    const char *a_add[] = { "add", "-A", NULL };
    if (git_run(sp->wt, a_add, NULL, 0, err, sizeof err) != 0) {
        snprintf(msg, cap, "git add failed in the worktree: %s", err);
        return false;
    }
    if ((usize)snprintf(patch, sizeof patch, "%s/work.patch", sp->dir[0] ? sp->dir : "/tmp") >= sizeof patch) {
        snprintf(msg, cap, "no room for the patch");
        return false;
    }
    char out_arg[700];
    snprintf(out_arg, sizeof out_arg, "--output=%s", patch);
    const char *a_diff[] = { "diff", "--cached", "--binary", "--no-ext-diff", "--no-textconv", out_arg, sp->wt_base, NULL };
    if (git_run(sp->wt, a_diff, NULL, 0, err, sizeof err) != 0) {
        snprintf(msg, cap, "git diff failed in the worktree: %s", err);
        return false;
    }
    struct stat st;
    if (stat(patch, &st) != 0 || st.st_size == 0) {
        unlink(patch);
        drop_worktree(sp);
        snprintf(msg, cap, "Nothing to merge: the job changed no file. Its worktree is removed.");
        return true;
    }
    const char *a_check[] = { "apply", "--check", "--whitespace=nowarn", patch, NULL };
    if (git_run(sp->origin_top, a_check, NULL, 0, err, sizeof err) == 0) {
        const char *a_apply[] = { "apply", "--whitespace=nowarn", patch, NULL };
        if (git_run(sp->origin_top, a_apply, NULL, 0, err, sizeof err) == 0) {
            unlink(patch);
            drop_worktree(sp);
            snprintf(msg, cap, "Merged into %s: the changes are in the working tree, not committed.", sp->origin_top);
            return true;
        }
    }
    /* the folder moved on where the job worked: a three-way merge, which
     * stages what it applies and leaves conflict markers where it cannot */
    const char *a_3way[] = { "apply", "--3way", "--whitespace=nowarn", patch, NULL };
    int rc = git_run(sp->origin_top, a_3way, NULL, 0, err, sizeof err);
    if (rc == 0) {
        unlink(patch);
        drop_worktree(sp);
        snprintf(msg, cap, "Merged into %s with a three-way merge; the merged files are staged.", sp->origin_top);
        return true;
    }
    snprintf(msg, cap, "Merged with conflicts (%s): resolve them in %s. The worktree %s is kept.",
             err[0] ? err : "git apply --3way failed", sp->origin_top, sp->wt);
    unlink(patch);
    return true;
}

static int git_run(const char *dir, const char *const *args, char *out, usize ocap, char *err, usize ecap) {
    return agentctl_git(dir, args, out, ocap, err, ecap, false);
}

/* ---------------------------------------------------------------- turn --- */

typedef enum { TURN_DONE, TURN_FAILED, TURN_STOPPED, TURN_LIU_GONE, TURN_CLOSE } TurnEnd;

static void interrupt_child(pid_t pid, int *level, i64 *next_ms) {
    if (pid <= 0) return;
    int sig = *level == 0 ? SIGINT : *level == 1 ? SIGTERM : SIGKILL;
    kill(-pid, sig);
    if (*level < 2) (*level)++;
    *next_ms = mono_ms() + (*level == 1 ? 3000 : 5000);
}

/* ------------------------------------------------- tools to be allowed --- */

/* A Claude tool request waiting for the user's word: its id and the input
 * to hand back when it is allowed. `always` rules let a kind of call
 * through for the rest of the job without asking again: a tool by its name,
 * a shell command by its first word. */
#define ASK_MAX    8
#define ALWAYS_MAX 32
typedef struct {
    char  id[136];
    char  tool[64];
    char *input;
    char  rule[128];        /* what "always" would allow */
} PendingAsk;

static PendingAsk g_asks[ASK_MAX];
static char       g_always[ALWAYS_MAX][128];
static u32        g_nalways;

/* The stdin of the turn's agent, for the lines written to it after the
 * prompt (answers to its requests). */
static char  *g_inq;
static usize  g_inq_len, g_inq_cap;
static bool   g_inq_on;             /* the agent reads JSON lines: answers can be written */

static void inq_push(const char *line) {
    if (!line || !g_inq_on) return;
    usize n = strlen(line);
    if (g_inq_len + n > g_inq_cap) {
        usize cap = g_inq_cap ? g_inq_cap : 4096;
        while (cap < g_inq_len + n) cap *= 2;
        char *nb = realloc(g_inq, cap);
        if (!nb) return;
        g_inq = nb;
        g_inq_cap = cap;
    }
    memcpy(g_inq + g_inq_len, line, n);
    g_inq_len += n;
}

/* "Bash npm" for a shell command, the tool's name otherwise. */
static void ask_rule(const char *tool, const char *input, char *out, usize cap) {
    snprintf(out, cap, "%s", tool);
    if (strcmp(tool, "Bash") != 0 || !input) return;
    cJSON *in = cJSON_Parse(input);
    const cJSON *c = cJSON_IsObject(in) ? cJSON_GetObjectItemCaseSensitive(in, "command") : NULL;
    if (cJSON_IsString(c)) {
        const char *p = c->valuestring;
        while (*p == ' ') p++;
        usize n = strcspn(p, " \t\n;&|");
        if (n > 0 && n < 64) snprintf(out, cap, "Bash %.*s", (int)n, p);
    }
    cJSON_Delete(in);
}

static void ask_send(const char *id, const char *tool, const char *what, bool on) {
    if (g_sock < 0) return;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_R_ASK, g_job)) return;
    agentctl_msg_put_str(&m, AGK_REQ, id);
    agentctl_msg_put_str(&m, AGK_STATE, on ? "1" : "0");
    if (tool) agentctl_msg_put_str(&m, AGK_TOOL, tool);
    if (what) agentctl_msg_put_str(&m, AGK_TEXT, what);
    if (agentctl_msg_end(&m)) (void)agentctl_write_all(g_sock, m.buf, m.len);
    agentctl_msg_free(&m);
}

static void ask_answer(PendingAsk *a, bool allow, const char *why) {
    char *line = agentctl_control_answer(a->id, allow, a->input, why);
    if (!line && allow) line = agentctl_control_answer(a->id, false, NULL, "The input could not be handed back.");
    inq_push(line);
    free(line);
    free(a->input);
    memset(a, 0, sizeof *a);
}

static void on_ask(void *ctx, const char *id, const char *tool, const char *what, const char *input) {
    (void)ctx;
    char rule[128];
    ask_rule(tool, input, rule, sizeof rule);
    for (u32 i = 0; i < g_nalways; i++) {
        if (strcmp(g_always[i], rule) != 0) continue;
        PendingAsk tmp = { 0 };
        snprintf(tmp.id, sizeof tmp.id, "%s", id);
        tmp.input = strdup(input);
        ask_answer(&tmp, true, NULL);
        say(AGS_DIM, "  \xE2\x9C\x93 allowed (always for %s)", rule);
        return;
    }
    PendingAsk *slot = NULL;
    for (u32 i = 0; i < ASK_MAX && !slot; i++) if (!g_asks[i].id[0]) slot = &g_asks[i];
    if (!slot || g_sock < 0) {
        PendingAsk tmp = { 0 };
        snprintf(tmp.id, sizeof tmp.id, "%s", id);
        ask_answer(&tmp, false, g_sock < 0 ? "Liu is not running, so nobody can allow it." : "Too many requests at once.");
        return;
    }
    snprintf(slot->id, sizeof slot->id, "%s", id);
    snprintf(slot->tool, sizeof slot->tool, "%s", tool);
    snprintf(slot->rule, sizeof slot->rule, "%s", rule);
    slot->input = strdup(input);
    ask_send(id, tool, what, true);
}

static void on_ask_cancel(void *ctx, const char *id) {
    (void)ctx;
    for (u32 i = 0; i < ASK_MAX; i++) {
        if (!g_asks[i].id[0] || strcmp(g_asks[i].id, id)) continue;
        ask_send(id, NULL, NULL, false);
        free(g_asks[i].input);
        memset(&g_asks[i], 0, sizeof g_asks[i]);
    }
}

static void on_control(void *ctx, const char *id) {
    (void)ctx;
    char *line = agentctl_control_error(id, "Liu does not answer this request.");
    inq_push(line);
    free(line);
}

/* Liu's word on a request: allow once, always, or deny. */
static void ask_decide(const char *id, const char *decision) {
    for (u32 i = 0; i < ASK_MAX; i++) {
        PendingAsk *a = &g_asks[i];
        if (!a->id[0] || strcmp(a->id, id)) continue;
        bool allow = !strcmp(decision, "allow") || !strcmp(decision, "always");
        if (!strcmp(decision, "always") && g_nalways < ALWAYS_MAX)
            snprintf(g_always[g_nalways++], sizeof g_always[0], "%s", a->rule);
        if (allow) say(AGS_OK, "  \xE2\x9C\x93 allowed%s", !strcmp(decision, "always") ? " (always, for this job)" : "");
        else       say(AGS_ERR, "  \xE2\x9C\x97 not allowed");
        ask_answer(a, allow, allow ? NULL : "The user did not allow it.");
        return;
    }
}

/* Every request still open: refused (the turn is ending, or Liu is gone). */
static void ask_refuse_all(const char *why) {
    for (u32 i = 0; i < ASK_MAX; i++) {
        if (!g_asks[i].id[0]) continue;
        ask_send(g_asks[i].id, NULL, NULL, false);
        ask_answer(&g_asks[i], false, why);
    }
}

/* The guard hook a delegated Claude gets in a team: before an edit or a
 * command, `liu-agent guard` asks Liu whether the paths are another
 * member's. "" when it cannot be spelled safely. */
static void guard_settings(const Spec *sp, char *out, usize cap) {
    out[0] = '\0';
    if (!sp->helpers[0] || sp->helpers[0] != '/' || strchr(sp->helpers, '\'') || strchr(sp->helpers, '"') ||
        strchr(sp->helpers, '\\'))
        return;
    int w = snprintf(out, cap,
                     "{\"hooks\":{\"PreToolUse\":[{\"matcher\":\"Bash|Edit|Write|MultiEdit|NotebookEdit\","
                     "\"hooks\":[{\"type\":\"command\",\"command\":\"'%s/liu-agent' guard\",\"timeout\":5}]}]}}",
                     sp->helpers);
    if (w <= 0 || (usize)w >= cap) out[0] = '\0';
}

static TurnEnd run_turn(Spec *sp, const char *prompt, usize prompt_len) {
    AgentctlArgvScratch sc;
    const char *argv[AGENTCTL_ARGV_CAP + 1];
    char guard[1400] = "";
    bool asks = sp->ask_tools && agentctl_agent_can_ask(sp->agent);
    if (sp->agent == AGENTCTL_AGENT_CLAUDE) guard_settings(sp, guard, sizeof guard);
    AgentctlTurnOpts opts = { asks, guard, sp->budget_micro };
    i32 argc = agentctl_turn_argv_ex(sp->agent, sp->model, sp->effort, sp->perm, sp->cwd,
                                     sp->session[0] ? sp->session : NULL, &opts, &sc, argv, AGENTCTL_ARGV_CAP);
    sp->turns++;
    send_frame(AGOP_R_TURN_START, sp->job, NULL, NULL, NULL, false);
    i64 started = mono_ms();
    if (argc <= 0) {
        say(AGS_ERR, "! Liu could not build the command for this job.");
        send_frame(AGOP_R_TURN_FAIL, sp->job, sp->session, NULL, "could not build the agent's command", false);
        write_turn_state(sp, "failed", "could not build the agent's command", 0, 0, 0, 0);
        return TURN_FAILED;
    }
    if (sp->effort[0] && !agentctl_agent_has_effort(sp->agent))
        say(AGS_DIM, "\xC2\xB7 effort %s is not a setting %s takes; its own is used", sp->effort,
            agentctl_agent_info(sp->agent) ? agentctl_agent_info(sp->agent)->display : "this agent");

    /* what goes on its stdin first: the prompt, or the user message that
     * carries it when the agent reads JSON lines */
    char *json_prompt = NULL;
    if (asks) {
        json_prompt = agentctl_user_message_json(prompt, prompt_len);
        if (!json_prompt) {
            say(AGS_ERR, "! out of memory");
            send_frame(AGOP_R_TURN_FAIL, sp->job, sp->session, NULL, "out of memory", false);
            return TURN_FAILED;
        }
        prompt = json_prompt;
        prompt_len = strlen(json_prompt);
    }
    g_inq_len = 0;
    g_inq_on = asks;
    memset(g_asks, 0, sizeof g_asks);

    int in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 };
    if (pipe(in) != 0 || pipe(out) != 0 || pipe(err) != 0) {
        say(AGS_ERR, "! could not create pipes: %s", strerror(errno));
        send_frame(AGOP_R_TURN_FAIL, sp->job, sp->session, NULL, "could not create pipes", false);
        free(json_prompt);
        return TURN_FAILED;
    }
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, err[1], STDERR_FILENO);
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;   /* only the three descriptors above reach the agent */
#endif
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setsigmask(&attr, &none);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], &fa, &attr, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(in[0]); close(out[1]); close(err[1]);
    if (rc != 0) {
        close(in[1]); close(out[0]); close(err[0]);
        free(json_prompt);
        const AgentctlAgentInfo *ai = agentctl_agent_info(sp->agent);
        char why[256];
        if (rc == ENOENT) snprintf(why, sizeof why, "%s is not installed, or not on this shell's PATH", ai ? ai->binary : "the agent");
        else              snprintf(why, sizeof why, "could not start %s: %s", ai ? ai->binary : "the agent", strerror(rc));
        say(AGS_ERR, "! %s", why);
        send_frame(AGOP_R_TURN_FAIL, sp->job, sp->session, NULL, why, false);
        write_turn_state(sp, "failed", why, 0, 0, 0, 0);
        return TURN_FAILED;
    }
    fcntl(in[1], F_SETFL, fcntl(in[1], F_GETFL) | O_NONBLOCK);
    fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
    fcntl(err[0], F_SETFL, fcntl(err[0], F_GETFL) | O_NONBLOCK);
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    fcntl(err[0], F_SETFD, FD_CLOEXEC);

    AgentctlStream st;
    agentctl_stream_init(&st, sp->agent);
    st.on_file = on_file;
    st.on_command = on_command;
    if (asks) {
        st.on_ask = on_ask;
        st.on_ask_cancel = on_ask_cancel;
        st.on_control = on_control;
    }
    memset(g_obs, 0, sizeof g_obs);
    g_obs_cwd = sp->cwd;
    usize sent = 0;
    char errtail[1024] = "";
    char errlast[3][512];             /* the last stderr lines, for a turn that fails */
    u32  errlast_n = 0;
    char errline[512];
    usize errline_len = 0;
    bool out_open = true, err_open = true, exited = false, stop_req = false, close_req = false, liu_gone = false;
    bool timed_out = false, prompt_done = false;
    int status = 0, kill_level = 0;
    i64 next_kill = -1;
    i64 deadline = sp->deadline_s ? started + (i64)sp->deadline_s * 1000 : -1;
    /* A process the agent left behind can hold its stdout open after the
     * agent itself has exited: the turn ends anyway, a moment later. */
    i64 drain_until = -1;
    AgentctlReader rd = {0};
    g_got_int = 0;

    while (!exited || out_open || err_open) {
        struct pollfd p[6];
        int n = 0, i_out = -1, i_err = -1, i_in = -1, i_sock = -1, i_sig = -1;
        bool want_in = in[1] >= 0 && (!prompt_done || g_inq_len > 0);
        if (out_open) { p[n].fd = out[0]; p[n].events = POLLIN; i_out = n++; }
        if (err_open) { p[n].fd = err[0]; p[n].events = POLLIN; i_err = n++; }
        if (want_in) { p[n].fd = in[1]; p[n].events = POLLOUT; i_in = n++; }
        if (g_sock >= 0) { p[n].fd = g_sock; p[n].events = POLLIN; i_sock = n++; }
        p[n].fd = g_sig[0]; p[n].events = POLLIN; i_sig = n++;
        for (int k = 0; k < n; k++) p[k].revents = 0;
        int timeout = -1;
        i64 due = next_kill >= 0 ? next_kill : drain_until;
        if (next_kill >= 0 && drain_until >= 0 && drain_until < next_kill) due = drain_until;
        if (deadline >= 0 && !timed_out && (due < 0 || deadline < due)) due = deadline;
        if (due >= 0) {
            i64 left = due - mono_ms();
            timeout = left < 0 ? 0 : (int)left;
        }
        int pr = poll(p, (nfds_t)n, timeout);
        if (pr < 0 && errno != EINTR) break;

        if (i_sig >= 0 && (p[i_sig].revents & POLLIN)) drain_sig();
        if (g_got_int) {
            g_got_int = 0;
            if (!stop_req) say(AGS_DIM, "\xC2\xB7 stopping\xE2\x80\xA6");
            stop_req = true;
            interrupt_child(exited ? -1 : pid, &kill_level, &next_kill);
        }
        if (g_got_quit) {
            close_req = true;
            interrupt_child(exited ? -1 : pid, &kill_level, &next_kill);
            g_got_quit = 0;
        }
        if (deadline >= 0 && !timed_out && !exited && mono_ms() >= deadline) {
            timed_out = true;
            say(AGS_ERR, "! the turn ran past its time limit (%us); stopping it", sp->deadline_s);
            interrupt_child(pid, &kill_level, &next_kill);
        }
        if (next_kill >= 0 && mono_ms() >= next_kill && !exited) interrupt_child(pid, &kill_level, &next_kill);
        else if (exited) next_kill = -1;

        if (i_in >= 0 && (p[i_in].revents & (POLLOUT | POLLERR | POLLHUP))) {
            bool broken = false;
            while (!prompt_done && sent < prompt_len) {
                ssize_t w = write(in[1], prompt + sent, prompt_len - sent);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    if (errno != EAGAIN) { sent = prompt_len; broken = true; }   /* the agent closed its stdin */
                    break;
                }
                sent += (usize)w;
            }
            if (sent >= prompt_len) prompt_done = true;
            while (prompt_done && !broken && g_inq_len > 0) {
                ssize_t w = write(in[1], g_inq, g_inq_len);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    if (errno != EAGAIN) broken = true;
                    break;
                }
                memmove(g_inq, g_inq + w, g_inq_len - (usize)w);
                g_inq_len -= (usize)w;
            }
            /* plain input ends with the prompt; JSON input stays open for
             * the answers until the turn's result has come */
            if (broken || (prompt_done && !asks)) { close(in[1]); in[1] = -1; g_inq_len = 0; }
        }
        if (i_out >= 0 && (p[i_out].revents & (POLLIN | POLLHUP | POLLERR))) {
            u8 buf[16384];
            for (;;) {
                ssize_t r = read(out[0], buf, sizeof buf);
                if (r > 0) { agentctl_stream_feed(&st, buf, (usize)r, emit, NULL); relay_outbox(); continue; }
                if (r < 0 && (errno == EINTR)) continue;
                if (r < 0 && errno == EAGAIN) break;
                out_open = false;
                close(out[0]);
                break;
            }
            /* the turn's result is in: a JSON-fed agent waits for more input
             * until its stdin closes */
            if (asks && in[1] >= 0 && prompt_done && g_inq_len == 0 && (st.turn_completed || st.turn_failed)) {
                ask_refuse_all("The turn has ended.");
                close(in[1]);
                in[1] = -1;
            }
        }
        if (i_err >= 0 && (p[i_err].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[4096];
            for (;;) {
                ssize_t r = read(err[0], buf, sizeof buf);
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) {
                    if (r < 0 && errno == EAGAIN) break;
                    err_open = false;
                    close(err[0]);
                    break;
                }
                for (ssize_t k = 0; k < r; k++) {
                    char ch = buf[k];
                    if (ch == '\n' || errline_len + 1 >= sizeof errline) {
                        errline[errline_len] = '\0';
                        char clean[512];
                        usize cl = agentctl_sanitize(errline, errline_len, clean, sizeof clean, false);
                        if (cl) {
                            /* what the agent writes to stderr is its own
                             * logging (its tool servers, its settings): in
                             * events.log, and shown only if the turn fails */
                            log_line(clean, cl);
                            snprintf(errtail, sizeof errtail, "%s", clean);
                            snprintf(errlast[errlast_n % 3], sizeof errlast[0], "%s", clean);
                            errlast_n++;
                        }
                        errline_len = 0;
                        if (ch == '\n') continue;
                    }
                    errline[errline_len++] = ch;
                }
            }
        }
        if (i_sock >= 0 && (p[i_sock].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (;;) {
                int frc = agentctl_reader_feed(&rd, g_sock);
                if (frc == 0) break;
                if (frc < 0) {
                    liu_gone = true;
                    close(g_sock);
                    g_sock = -1;
                    /* nobody is left to allow anything */
                    ask_refuse_all("Liu is not running, so nobody can allow it.");
                    if (sp->keep) say(AGS_DIM, "\xC2\xB7 Liu quit; finishing this turn");
                    else          interrupt_child(exited ? -1 : pid, &kill_level, &next_kill);
                    break;
                }
                AgentctlFrame f;
                agentctl_reader_frame(&rd, &f);
                if (f.op == AGOP_S_STOP) {
                    if (!stop_req) say(AGS_DIM, "\xC2\xB7 stopping (asked by %s)\xE2\x80\xA6", sp->owner[0] ? sp->owner : "the agent that started this");
                    stop_req = true;
                    interrupt_child(exited ? -1 : pid, &kill_level, &next_kill);
                } else if (f.op == AGOP_S_CLOSE) {
                    close_req = true;
                    interrupt_child(exited ? -1 : pid, &kill_level, &next_kill);
                } else if (f.op == AGOP_S_ANSWER) {
                    char id[136], dec[16];
                    if (agentctl_get_str(&f, AGK_REQ, id, sizeof id) && agentctl_get_str(&f, AGK_DECISION, dec, sizeof dec))
                        ask_decide(id, dec);
                }
                agentctl_reader_reset(&rd);
            }
        }
        if (!exited) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) { exited = true; drain_until = mono_ms() + 2000; }
            g_got_chld = 0;
        }
        if (exited && drain_until >= 0 && mono_ms() >= drain_until) {
            if (out_open) { close(out[0]); out_open = false; }
            if (err_open) { close(err[0]); err_open = false; }
        }
    }
    agentctl_reader_reset(&rd);
    if (in[1] >= 0) close(in[1]);
    ask_refuse_all("The turn has ended.");
    g_inq_on = false;
    g_inq_len = 0;
    free(json_prompt);
    agentctl_stream_finish(&st, emit, NULL);
    relay_outbox();
    if (st.session[0]) snprintf(sp->session, sizeof sp->session, "%s", st.session);

    char took[32], tokens[96], cost[32];
    fmt_duration(mono_ms() - started, took, sizeof took);
    snprintf(tokens, sizeof tokens, "%llu %llu %llu", (unsigned long long)st.tok_in,
             (unsigned long long)st.tok_cached, (unsigned long long)st.tok_out);
    snprintf(cost, sizeof cost, "%llu", (unsigned long long)st.cost_micro);
    if (sp->budget_micro) sp->budget_micro = st.cost_micro >= sp->budget_micro ? 1 : sp->budget_micro - st.cost_micro;
    TurnEnd end;
    bool clean_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (liu_gone && !sp->keep) {
        end = TURN_LIU_GONE;
        write_turn_state(sp, "stopped", "Liu quit", st.tok_in, st.tok_cached, st.tok_out, st.cost_micro);
    } else if (clean_exit && st.turn_completed && !st.turn_failed && !stop_req && !timed_out) {
        write_result(sp, st.final_text ? st.final_text : "", st.final_len);
        say(AGS_HEAD, "\xE2\x94\x80\xE2\x94\x80 done \xC2\xB7 %s%s%s", took, st.usage[0] ? " \xC2\xB7 " : "", st.usage);
        char head[AGT_SUMMARY_MAX + 1];
        summary_of(st.final_text, st.final_len, head, sizeof head);
        AgentctlMsg m;
        if (g_sock >= 0 && agentctl_msg_init(&m, AGOP_R_TURN_DONE, sp->job)) {
            if (head[0]) agentctl_msg_put_str(&m, AGK_TEXT, head);
            if (sp->session[0]) agentctl_msg_put_str(&m, AGK_SESSION, sp->session);
            if (st.usage[0]) agentctl_msg_put_str(&m, AGK_USAGE, st.usage);
            agentctl_msg_put_str(&m, AGK_TOKENS, tokens);
            agentctl_msg_put_str(&m, AGK_COST, cost);
            if (agentctl_msg_end(&m)) (void)agentctl_write_all(g_sock, m.buf, m.len);
            agentctl_msg_free(&m);
        }
        write_turn_state(sp, "done", "", st.tok_in, st.tok_cached, st.tok_out, st.cost_micro);
        end = liu_gone ? TURN_LIU_GONE : TURN_DONE;
    } else {
        char why[512];
        if (timed_out)                 snprintf(why, sizeof why, "it ran past its time limit (%us)", sp->deadline_s);
        else if (stop_req)             snprintf(why, sizeof why, "stopped");
        else if (st.error[0])          snprintf(why, sizeof why, "%s", st.error);
        else if (WIFSIGNALED(status))  snprintf(why, sizeof why, "the agent was killed by signal %d", WTERMSIG(status));
        else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            snprintf(why, sizeof why, "the agent exited with status %d%s%s", WEXITSTATUS(status),
                     errtail[0] ? ": " : "", errtail);
        else                           snprintf(why, sizeof why, "the agent ended without finishing its turn");
        if (st.final_text) write_result(sp, st.final_text, st.final_len);
        if (!stop_req)
            for (u32 k = errlast_n > 3 ? errlast_n - 3 : 0; k < errlast_n; k++)
                emit(NULL, AGS_DIM, errlast[k % 3], strlen(errlast[k % 3]));
        if (stop_req) say(AGS_HEAD, "\xE2\x94\x80\xE2\x94\x80 stopped \xC2\xB7 %s", took);
        else          say(AGS_ERR, "\xE2\x94\x80\xE2\x94\x80 failed \xC2\xB7 %s \xC2\xB7 %s", took, why);
        AgentctlMsg m;
        if (g_sock >= 0 && agentctl_msg_init(&m, AGOP_R_TURN_FAIL, sp->job)) {
            if (sp->session[0]) agentctl_msg_put_str(&m, AGK_SESSION, sp->session);
            if (st.usage[0]) agentctl_msg_put_str(&m, AGK_USAGE, st.usage);
            agentctl_msg_put_str(&m, AGK_ERROR, why);
            if (stop_req) agentctl_msg_put_str(&m, AGK_STOPPED, "1");
            agentctl_msg_put_str(&m, AGK_TOKENS, tokens);
            agentctl_msg_put_str(&m, AGK_COST, cost);
            if (agentctl_msg_end(&m)) (void)agentctl_write_all(g_sock, m.buf, m.len);
            agentctl_msg_free(&m);
        }
        write_turn_state(sp, stop_req ? "stopped" : "failed", why, st.tok_in, st.tok_cached, st.tok_out, st.cost_micro);
        end = liu_gone ? TURN_LIU_GONE : stop_req ? TURN_STOPPED : TURN_FAILED;
    }
    if (close_req) end = TURN_CLOSE;
    agentctl_stream_free(&st);
    return end;
}

/* ---------------------------------------------------------------- idle --- */

typedef enum { IDLE_TURN, IDLE_END } IdleEnd;

/* Liu's word on an isolated job's work, between turns: merged into the
 * folder it came from, or thrown away. Either ends the job; the answer says
 * how it went. */
static bool idle_merge(Spec *sp, const char *decision) {
    char msg[1600];
    bool over;
    u64 code = AGC_OK;
    if (!sp->wt[0]) {
        snprintf(msg, sizeof msg, "This job has no worktree of its own.");
        over = false;
        code = AGC_REFUSED;
    } else if (!strcmp(decision, "merge")) {
        say(AGS_DIM, "\xC2\xB7 merging the work into %s\xE2\x80\xA6", sp->origin_top);
        over = merge_worktree(sp, msg, sizeof msg);
        if (!over) code = AGC_ERROR;
    } else {
        drop_worktree(sp);
        snprintf(msg, sizeof msg, "Discarded: the worktree %s and its branch %s are removed.", sp->wt, sp->wt_branch);
        over = true;
    }
    say(code == AGC_OK ? AGS_OK : AGS_ERR, "%s %s", code == AGC_OK ? "\xE2\x9C\x93" : "\xE2\x9C\x97", msg);
    AgentctlMsg m;
    if (g_sock >= 0 && agentctl_msg_init(&m, AGOP_R_MERGED, sp->job)) {
        agentctl_msg_put_u64(&m, AGK_CODE, code);
        agentctl_msg_put_str(&m, AGK_TEXT, msg);
        agentctl_msg_put_str(&m, AGK_DECISION, decision);
        if (agentctl_msg_end(&m)) (void)agentctl_write_all(g_sock, m.buf, m.len);
        agentctl_msg_free(&m);
    }
    return over;
}

/* Between turns: the next message, or the end (Liu closed the job, its socket
 * went, or a signal). */
static IdleEnd wait_idle(Spec *sp, char **prompt, usize *prompt_len, char **team) {
    AgentctlReader rd = {0};
    for (;;) {
        struct pollfd p[2];
        int n = 0, i_sock = -1;
        if (g_sock >= 0) { p[n].fd = g_sock; p[n].events = POLLIN; p[n].revents = 0; i_sock = n++; }
        p[n].fd = g_sig[0]; p[n].events = POLLIN; p[n].revents = 0; n++;
        if (poll(p, (nfds_t)n, -1) < 0 && errno != EINTR) { agentctl_reader_reset(&rd); return IDLE_END; }
        drain_sig();
        if (g_got_int || g_got_quit || g_sock < 0) { agentctl_reader_reset(&rd); return IDLE_END; }
        if (i_sock >= 0 && (p[i_sock].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (;;) {
                int frc = agentctl_reader_feed(&rd, g_sock);
                if (frc == 0) break;
                if (frc < 0) { agentctl_reader_reset(&rd); return IDLE_END; }
                AgentctlFrame f;
                agentctl_reader_frame(&rd, &f);
                if (f.op == AGOP_S_CLOSE) { agentctl_reader_reset(&rd); return IDLE_END; }
                if (f.op == AGOP_S_MERGE) {
                    char dec[16];
                    bool over = agentctl_get_str(&f, AGK_DECISION, dec, sizeof dec) && idle_merge(sp, dec);
                    agentctl_reader_reset(&rd);
                    if (over) return IDLE_END;
                    continue;
                }
                if (f.op == AGOP_S_TURN) {
                    u64 v = 0;
                    if (agentctl_get_u64(&f, AGK_BUDGET, &v)) sp->budget_micro = v;
                    const u8 *d; u32 len;
                    if (agentctl_get(&f, AGK_PROMPT, &d, &len) && len > 0) {
                        char *msg = malloc(len + 1);
                        if (msg) {
                            memcpy(msg, d, len);
                            msg[len] = '\0';
                            *prompt = msg;
                            *prompt_len = len;
                            *team = NULL;
                            const u8 *td; u32 tl;
                            if (agentctl_get(&f, AGK_TEAM, &td, &tl) && tl > 0 && (*team = malloc(tl + 1))) {
                                memcpy(*team, td, tl);
                                (*team)[tl] = '\0';
                            }
                            agentctl_reader_reset(&rd);
                            return IDLE_TURN;
                        }
                    }
                }
                agentctl_reader_reset(&rd);
            }
        }
    }
}

/* ------------------------------------------------------------ the task --- */

/* The task (or the next message) at the head of its turn in the log: the
 * first line strong, a few more, then how many were left out. events.log has
 * all of it. */
static void show_task(const char *prompt) {
    const char *p = prompt;
    int lines = 0;
    while (*p && lines < 8) {
        const char *nl = strchr(p, '\n');
        usize len = nl ? (usize)(nl - p) : strlen(p);
        char clean[1024];
        agentctl_sanitize(p, len, clean, sizeof clean, false);
        char line[1100];
        int w = snprintf(line, sizeof line, "%s%s", lines == 0 ? "\xE2\x80\xBA " : "  ", clean);
        if (w > 0) emit(NULL, lines == 0 ? AGS_HEAD : AGS_DIM, line, (usize)w < sizeof line ? (usize)w : sizeof line - 1);
        lines++;
        if (!nl) { p += len; break; }
        p = nl + 1;
    }
    if (*p) {
        int more = 0;
        for (const char *q = p; *q; q++) if (*q == '\n') more++;
        say(AGS_DIM, "  \xE2\x80\xA6 (%d more line%s)", more + 1, more ? "s" : "");
    }
}

static bool read_spec(Spec *sp, u32 job) {
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_R_HELLO, job)) return false;
    bool ok = agentctl_msg_end(&m) && agentctl_write_all(g_sock, m.buf, m.len);
    agentctl_msg_free(&m);
    if (!ok) return false;
    AgentctlFrame f;
    u8 *store = NULL;
    if (agentctl_read_frame(g_sock, 10000, &f, &store) != 1) return false;
    if (f.op != AGOP_S_SPEC) {
        free(store);
        return false;
    }
    char agent[32], perm[16];
    agentctl_get_str(&f, AGK_AGENT, agent, sizeof agent);
    agentctl_get_str(&f, AGK_PERMISSION, perm, sizeof perm);
    sp->job = job;
    sp->agent = agentctl_agent_parse(agent);
    sp->perm = agentctl_perm_parse(perm);
    agentctl_get_str(&f, AGK_MODEL, sp->model, sizeof sp->model);
    agentctl_get_str(&f, AGK_EFFORT, sp->effort, sizeof sp->effort);
    agentctl_get_str(&f, AGK_CWD, sp->cwd, sizeof sp->cwd);
    agentctl_get_str(&f, AGK_LABEL, sp->label, sizeof sp->label);
    agentctl_get_str(&f, AGK_OWNER, sp->owner, sizeof sp->owner);
    agentctl_get_str(&f, AGK_DIR, sp->dir, sizeof sp->dir);
    char flag[4];
    sp->isolate   = agentctl_get_str(&f, AGK_ISOLATE, flag, sizeof flag) && flag[0] == '1';
    sp->keep      = agentctl_get_str(&f, AGK_KEEP, flag, sizeof flag) && flag[0] == '1';
    sp->ask_tools = agentctl_get_str(&f, AGK_ASK_TOOLS, flag, sizeof flag) && flag[0] == '1';
    u64 v = 0;
    if (agentctl_get_u64(&f, AGK_DEADLINE, &v) && v < 0x7fffffffu) sp->deadline_s = (u32)v;
    if (agentctl_get_u64(&f, AGK_BUDGET, &v)) sp->budget_micro = v;
    agentctl_get_str(&f, AGK_HELPERS, sp->helpers, sizeof sp->helpers);
    char sess[AGENTCTL_SESSION_CAP];
    if (agentctl_get_str(&f, AGK_SESSION, sess, sizeof sess) && agentctl_session_ok(sess))
        snprintf(sp->session, sizeof sp->session, "%s", sess);
    /* a job adopted with a worktree it already has */
    agentctl_get_str(&f, AGK_WORKTREE, sp->wt, sizeof sp->wt);
    agentctl_get_str(&f, AGK_BRANCH, sp->wt_branch, sizeof sp->wt_branch);
    agentctl_get_str(&f, AGK_BASE, sp->wt_base, sizeof sp->wt_base);
    char origin[1024];
    if (agentctl_get_str(&f, AGK_PATH, origin, sizeof origin)) snprintf(sp->origin_top, sizeof sp->origin_top, "%s", origin);
    const u8 *d; u32 len;
    if (agentctl_get(&f, AGK_PROMPT, &d, &len) && len > 0) {
        sp->prompt = malloc(len + 1);
        if (sp->prompt) { memcpy(sp->prompt, d, len); sp->prompt[len] = '\0'; }
    }
    if (agentctl_get(&f, AGK_TEAM, &d, &len) && len > 0) {
        sp->team = malloc(len + 1);
        if (sp->team) { memcpy(sp->team, d, len); sp->team[len] = '\0'; }
    }
    free(store);
    /* no task: a job taken over from an earlier Liu, idle until its next message */
    return sp->agent >= 0 && sp->perm >= 0 && sp->cwd[0] == '/';
}

/* What the agent finds in its environment for its team: `liu-agent` first
 * on its PATH, and the team as a file it can read where its sandbox keeps it
 * from Liu's socket (LIU_AGENT_TEAM_FILE). */
static void agent_env(const Spec *sp) {
    const char *helpers = getenv("LIU_AGENT_HELPERS");
    if (helpers && helpers[0] == '/') {
        const char *path = getenv("PATH");
        usize need = strlen(helpers) + 1 + (path ? strlen(path) : 0) + 1;
        char *np = malloc(need);
        if (np) {
            snprintf(np, need, "%s%s%s", helpers, path && path[0] ? ":" : "", path ? path : "");
            setenv("PATH", np, 1);
            free(np);
        }
    }
    unsetenv("LIU_AGENT_HELPERS");
    char team[600];
    if (sp->dir[0] && (usize)snprintf(team, sizeof team, "%s/team.md", sp->dir) < sizeof team)
        setenv("LIU_AGENT_TEAM_FILE", team, 1);
    if (sp->dir[0] && (usize)snprintf(g_outbox, sizeof g_outbox, "%s/" AGT_OUTBOX_NAME, sp->dir) < sizeof g_outbox)
        setenv("LIU_AGENT_OUTBOX", g_outbox, 1);
    else
        g_outbox[0] = '\0';
}

int agentctl_runner_main(void) {
    const char *job_s = getenv("LIU_AGENT_JOB");
    const char *sock = getenv("LIU_AGENT_SOCK");
    char *end = NULL;
    unsigned long job = job_s ? strtoul(job_s, &end, 10) : 0;
    if (!job_s || !sock || !sock[0] || !end || *end || job == 0 || job > 0xffffffffUL) {
        fprintf(stderr, "liu-agent run: Liu starts this for a delegated agent; it is not run by hand.\n");
        return 64;
    }
    signals_setup();
    g_sock = notify_socket_connect_timeout(sock, 5000);
    if (g_sock < 0) return 1;
    /* the deadlines of connect_timeout are for the handshake only */
    struct timeval none = {0};
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif

    Spec sp;
    memset(&sp, 0, sizeof sp);
    if (!read_spec(&sp, (u32)job)) return 1;
    g_job = sp.job;
    fcntl(g_sock, F_SETFL, fcntl(g_sock, F_GETFL) | O_NONBLOCK);
    if (sp.dir[0]) {
        char path[600];
        if ((usize)snprintf(path, sizeof path, "%s/events.log", sp.dir) < sizeof path)
            g_log = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    }
    agent_env(&sp);
    if (sp.isolate && !sp.wt[0]) {
        char why[1200];
        if (!make_worktree(&sp, why, sizeof why)) {
            say(AGS_ERR, "! %s", why);
            send_frame(AGOP_R_TURN_FAIL, sp.job, NULL, NULL, why, false);
            write_turn_state(&sp, "failed", why, 0, 0, 0, 0);
            free(sp.prompt);
            free(sp.team);
            if (g_sock >= 0) close(g_sock);
            return 1;
        }
        say(AGS_DIM, "\xC2\xB7 working in its own worktree %s (branch %s)", sp.wt, sp.wt_branch);
        AgentctlMsg m;
        if (agentctl_msg_init(&m, AGOP_R_WORKTREE, sp.job)) {
            agentctl_msg_put_str(&m, AGK_WORKTREE, sp.wt);
            agentctl_msg_put_str(&m, AGK_CWD, sp.cwd);
            agentctl_msg_put_str(&m, AGK_BRANCH, sp.wt_branch);
            agentctl_msg_put_str(&m, AGK_BASE, sp.wt_base);
            agentctl_msg_put_str(&m, AGK_PATH, sp.origin_top);
            if (agentctl_msg_end(&m)) (void)agentctl_write_all(g_sock, m.buf, m.len);
            agentctl_msg_free(&m);
        }
    }
    if (chdir(sp.cwd) != 0) {
        say(AGS_ERR, "! the folder %s is not there: %s", sp.cwd, strerror(errno));
        send_frame(AGOP_R_TURN_FAIL, sp.job, NULL, NULL, "the job's folder is not there", false);
        return 1;
    }
    /* what a program asks for its folder through the environment says the
     * job's folder, not the one Liu was started in */
    setenv("PWD", sp.cwd, 1);

    char *prompt = sp.prompt, *team = sp.team;
    usize prompt_len = prompt ? strlen(prompt) : 0;
    sp.prompt = NULL;
    sp.team = NULL;
    if (!prompt) {
        free(team);
        team = NULL;
        if (wait_idle(&sp, &prompt, &prompt_len, &team) == IDLE_END) {
            if (g_log >= 0) close(g_log);
            if (g_sock >= 0) close(g_sock);
            return 0;
        }
    }
    for (;;) {
        show_task(prompt);
        /* the team notes go ahead of the message; the log shows the message */
        char *input = prompt;
        usize input_len = prompt_len;
        if (team) {
            usize tl = strlen(team);
            while (tl > 0 && team[tl - 1] == '\n') tl--;   /* one blank line between them */
            char *both = malloc(tl + 2 + prompt_len + 1);
            if (both) {
                memcpy(both, team, tl);
                memcpy(both + tl, "\n\n", 2);
                memcpy(both + tl + 2, prompt, prompt_len + 1);
                input = both;
                input_len = tl + 2 + prompt_len;
            }
        }
        TurnEnd te = run_turn(&sp, input, input_len);
        if (input != prompt) free(input);
        free(prompt);
        free(team);
        prompt = NULL;
        team = NULL;
        if (te == TURN_LIU_GONE || te == TURN_CLOSE) break;
        if (wait_idle(&sp, &prompt, &prompt_len, &team) == IDLE_END) break;
    }
    free(prompt);
    free(team);
    if (g_log >= 0) close(g_log);
    if (g_sock >= 0) close(g_sock);
    return 0;
}
