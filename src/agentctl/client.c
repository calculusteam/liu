/*
 * Liu — `liu-agent`: what an agent in a Liu pane runs to start and drive
 * another agent. Every command is one request to the Liu that owns the pane
 * (LIU_AGENT_SOCK) and prints plain text meant to be read by an agent.
 *
 * Exit status: 0 done, 1 usage or no Liu, 2 refused or declined, 3 still
 * waiting when --timeout ran out, 4 the job failed, was stopped or ended.
 */
#include "agentctl/agentctl_adapter.h"
#include "agentctl/agentctl_proto.h"
#include "agentctl/agentctl_stream.h"
#include "agentctl/agentctl_team.h"
#include "notify/socket.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

int agentctl_runner_main(void);

enum { EX_DONE = 0, EX_USAGE = 1, EX_REFUSED = 2, EX_WAITING = 3, EX_FAILED = 4 };

static int g_fd = -1;

static void usage(FILE *o) {
    fputs(
        "liu-agent: start another coding agent through Liu and drive it (works only in a Liu pane;\n"
        "Liu shows each one as a small tab beside this pane's tab)\n"
        "\n"
        "  liu-agent spawn <agent>[,<agent>…] [options] [--] <task | ->\n"
        "      agents: " AGENTCTL_AGENT_LIST " (several, each in its own worktree, with --isolate)\n"
        "      -m, --model NAME        e.g. gpt-6.1-sol, claude-opus-5-5 (default: the agent's own)\n"
        "      -e, --effort LEVEL      none minimal low medium high xhigh max\n"
        "      -p, --permission LEVEL  read | write (default) | full\n"
        "      -C, --cwd DIR           the folder it works in (default: this one)\n"
        "      -l, --label TEXT        a short name for it\n"
        "      --role NAME             a preset (agent, model, effort, level, instructions); `liu-agent roles`\n"
        "      --isolate               work in a git worktree of its own; `merge` or `discard` it after\n"
        "      --after JOB             start once that job's turn has finished (repeatable)\n"
        "      --from JOB              continue that job's work: its history, or its report and files\n"
        "      --owns PATH             a path the job owns (repeatable)\n"
        "      --max-turns N, --max-tokens N, --budget-usd X, --turn-timeout DURATION\n"
        "                              what it may spend; a turn past its time is stopped\n"
        "      --no-ask                a tool its level refuses fails instead of being put to the user\n"
        "      -w, --wait              wait for its first turn and print the result\n"
        "      -t, --timeout SECONDS   how long to wait (approval: 100 by default)\n"
        "      The task is the remaining words, or stdin with '-'. Liu asks the user first.\n"
        "  liu-agent review <job> [<agent>] [options] [--] [what to look at]   a read-only review of its work\n"
        "  liu-agent wait <job>... [--any] [--timeout SECONDS]   wait, then print the result and its evidence\n"
        "  liu-agent send <job> [--wait] [--timeout S] [--] <message | ->   the next message\n"
        "  liu-agent status [<job>]     one job, or every job this pane started\n"
        "  liu-agent list\n"
        "  liu-agent result <job> [--full]\n"
        "  liu-agent log <job> [--tail N]\n"
        "  liu-agent compare <job> <job>...   their reports, files and spend side by side\n"
        "  liu-agent merge <job>        an --isolate job's work into this folder (ends the job)\n"
        "  liu-agent discard <job>      an --isolate job's work thrown away (ends the job)\n"
        "  liu-agent stop <job>         interrupt the running turn\n"
        "  liu-agent close <job>        end the job\n"
        "  liu-agent focus <job>        open its log for the user\n"
        "  liu-agent agents             which agents can be started\n"
        "  liu-agent models [<agent>]   the models Liu last read from each agent\n"
        "  liu-agent roles              the presets --role takes\n"
        "\n"
        "The team: the agent in this pane and the jobs it started, working at the same time.\n"
        "  liu-agent team               who is in it, what each does and owns, the newest messages\n"
        "  liu-agent post [--to all|lead|<job>|<label>] [--] <message | ->   tell the team (or one member)\n"
        "  liu-agent inbox [--wait] [--timeout SECONDS]   the messages for you not read yet\n"
        "  liu-agent owns <path>... | --clear   the paths you call your own\n", o);
}

/* ------------------------------------------------------------ transport --- */

static bool connect_liu_as(bool quiet) {
    const char *path = getenv("LIU_AGENT_SOCK");
    if (!path || !path[0]) {
        if (!quiet)
            fprintf(stderr, "liu-agent: not in a Liu pane (LIU_AGENT_SOCK is not set), "
                            "or Liu's agent control is turned off.\n");
        return false;
    }
    g_fd = notify_socket_connect_timeout(path, 5000);
    if (g_fd < 0) {
        if (!quiet) fprintf(stderr, "liu-agent: could not reach Liu at %s: %s\n", path, strerror(errno));
        return false;
    }
    struct timeval none = {0};
    setsockopt(g_fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
    setsockopt(g_fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(g_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return true;
}

static bool connect_liu(void) { return connect_liu_as(false); }

/* The team file Liu keeps for a delegated agent (LIU_AGENT_TEAM_FILE), or
 * NULL: what a team command falls back on where a sandbox keeps the agent
 * from Liu's socket. */
static const char *team_file(void) {
    const char *f = getenv("LIU_AGENT_TEAM_FILE");
    return f && f[0] == '/' ? f : NULL;
}

/* A connection for a team command: quiet when there is a team file to fall
 * back on. */
static bool connect_team(void) { return connect_liu_as(team_file() != NULL); }

static const char *const NO_SOCKET_NOTE =
    "Liu's socket is out of reach from here (the agent's sandbox); `liu-agent post` still reaches the team "
    "through this job's outbox, and your final message is passed on when your turn ends.";

static bool send_msg(AgentctlMsg *m) {
    bool ok = agentctl_msg_end(m) && agentctl_write_all(g_fd, m->buf, m->len);
    agentctl_msg_free(m);
    if (!ok) fprintf(stderr, "liu-agent: the request could not be sent to Liu.\n");
    return ok;
}

/* 1 = a reply, 0 = timeout, -1 = Liu went away. */
static int recv_reply(i32 timeout_ms, AgentctlFrame *f, u8 **store) {
    int rc = agentctl_read_frame(g_fd, timeout_ms, f, store);
    if (rc < 0) fprintf(stderr, "liu-agent: Liu closed the connection.\n");
    return rc;
}

static u64 reply_code(const AgentctlFrame *f) {
    u64 c = AGC_ERROR;
    agentctl_get_u64(f, AGK_CODE, &c);
    return c;
}

static void print_text(FILE *o, const AgentctlFrame *f) {
    char t[1024];
    if (agentctl_get_str(f, AGK_TEXT, t, sizeof t)) {
        char clean[1024];
        agentctl_sanitize(t, strlen(t), clean, sizeof clean, true);
        fprintf(o, "%s\n", clean);
    }
}

/* The team text a reply carries (AGK_TEAM), sanitized. */
static bool print_team_text(const AgentctlFrame *f) {
    const u8 *d; u32 n;
    if (!agentctl_get(f, AGK_TEAM, &d, &n) || n == 0) return false;
    char *clean = malloc((usize)n * 3 + 1);
    if (!clean) return false;
    usize w = agentctl_sanitize((const char *)d, n, clean, (usize)n * 3 + 1, true);
    fwrite(clean, 1, w, stdout);
    if (w && clean[w - 1] != '\n') fputc('\n', stdout);
    free(clean);
    return true;
}

static void print_file(const char *dir, const char *name, usize max_bytes, i32 tail_lines);

/* The team as Liu last wrote it to the job's team file. */
static bool print_team_file(void) {
    const char *path = team_file();
    if (!path) return false;
    const char *slash = strrchr(path, '/');
    char dir[PATH_MAX];
    usize dl = (usize)(slash - path);
    if (!slash || dl >= sizeof dir) return false;
    memcpy(dir, path, dl);
    dir[dl] = '\0';
    print_file(dir, slash + 1, AGT_LISTING_MAX, 0);
    printf("\n(read from the team file Liu keeps for this job. %s)\n", NO_SOCKET_NOTE);
    return true;
}

/* --------------------------------------------------------------- files --- */

/* A file of the job's folder, sanitized, whole or its last `tail_lines`. */
static void print_file(const char *dir, const char *name, usize max_bytes, i32 tail_lines) {
    if (!dir || !dir[0]) return;
    char path[PATH_MAX];
    if ((usize)snprintf(path, sizeof path, "%s/%s", dir, name) >= sizeof path) return;
    /* not waiting on the open: a FIFO in its place must not hold the command */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return; }
    usize size = (usize)st.st_size;
    usize want = size;
    off_t from = 0;
    bool cut = false;
    if (tail_lines <= 0 && max_bytes && size > max_bytes) { want = max_bytes; cut = true; }
    if (tail_lines > 0 && size > 256u * 1024u) { from = (off_t)(size - 256u * 1024u); want = 256u * 1024u; }
    char *buf = malloc(want + 1);
    if (!buf) { close(fd); return; }
    usize got = 0;
    if (lseek(fd, from, SEEK_SET) == from) {
        while (got < want) {
            ssize_t r = read(fd, buf + got, want - got);
            if (r <= 0) break;
            got += (usize)r;
        }
    }
    close(fd);
    buf[got] = '\0';
    const char *start = buf;
    if (tail_lines > 0) {
        i32 seen = 0;
        for (usize i = got; i > 0; i--) {
            if (buf[i - 1] == '\n' && i != got && ++seen == tail_lines) { start = buf + i; break; }
        }
    }
    usize n = strlen(start);
    char *clean = malloc(n * 3 + 1);
    if (clean) {
        usize w = agentctl_sanitize(start, n, clean, n * 3 + 1, true);
        fwrite(clean, 1, w, stdout);
        if (w && clean[w - 1] != '\n') fputc('\n', stdout);
        free(clean);
    }
    if (cut) printf("[… cut at %zu KB; `liu-agent result <job> --full` prints all of it]\n", max_bytes / 1024);
    free(buf);
}

/* ---------------------------------------------------------- job output --- */

typedef struct {
    u64  id;
    char agent[32], model[AGENTCTL_MODEL_CAP], effort[AGENTCTL_EFFORT_CAP], perm[16];
    char cwd[1024], label[AGENTCTL_LABEL_CAP], state[32], outcome[16], end[32];
    char error[512], session[AGENTCTL_SESSION_CAP], usage[128], dir[512];
    u64  turns, elapsed_ms;
    char spend[160], limits[160];
    char worktree[1024], branch[128], base[64], merged[600], asking[700];
    char *files;            /* one absolute path a line, "-" ahead of a removed one; malloc'd */
    char clashes[2048];
    u64  review_of;
    bool isolate;
} JobInfo;

static void job_info_free(JobInfo *j) { free(j->files); j->files = NULL; }

static void job_from(const AgentctlFrame *f, JobInfo *j) {
    memset(j, 0, sizeof *j);
    agentctl_get_u64(f, AGK_JOB, &j->id);
    agentctl_get_str(f, AGK_AGENT, j->agent, sizeof j->agent);
    agentctl_get_str(f, AGK_MODEL, j->model, sizeof j->model);
    agentctl_get_str(f, AGK_EFFORT, j->effort, sizeof j->effort);
    agentctl_get_str(f, AGK_PERMISSION, j->perm, sizeof j->perm);
    agentctl_get_str(f, AGK_CWD, j->cwd, sizeof j->cwd);
    agentctl_get_str(f, AGK_LABEL, j->label, sizeof j->label);
    agentctl_get_str(f, AGK_STATE, j->state, sizeof j->state);
    agentctl_get_str(f, AGK_OUTCOME, j->outcome, sizeof j->outcome);
    agentctl_get_str(f, AGK_END, j->end, sizeof j->end);
    agentctl_get_str(f, AGK_ERROR, j->error, sizeof j->error);
    agentctl_get_str(f, AGK_SESSION, j->session, sizeof j->session);
    agentctl_get_str(f, AGK_USAGE, j->usage, sizeof j->usage);
    agentctl_get_str(f, AGK_DIR, j->dir, sizeof j->dir);
    agentctl_get_u64(f, AGK_TURNS, &j->turns);
    agentctl_get_u64(f, AGK_ELAPSED_MS, &j->elapsed_ms);
    agentctl_get_str(f, AGK_SPEND, j->spend, sizeof j->spend);
    agentctl_get_str(f, AGK_LIMITS, j->limits, sizeof j->limits);
    agentctl_get_str(f, AGK_WORKTREE, j->worktree, sizeof j->worktree);
    agentctl_get_str(f, AGK_BRANCH, j->branch, sizeof j->branch);
    agentctl_get_str(f, AGK_BASE, j->base, sizeof j->base);
    agentctl_get_str(f, AGK_MERGED, j->merged, sizeof j->merged);
    agentctl_get_str(f, AGK_ASKING, j->asking, sizeof j->asking);
    agentctl_get_str(f, AGK_CLASHES, j->clashes, sizeof j->clashes);
    agentctl_get_u64(f, AGK_REVIEW, &j->review_of);
    char flag[4];
    j->isolate = agentctl_get_str(f, AGK_ISOLATE, flag, sizeof flag) && flag[0] == '1';
    const u8 *d; u32 n;
    if (agentctl_get(f, AGK_FILES, &d, &n) && n > 0 && !memchr(d, '\0', n) && (j->files = malloc((usize)n * 3 + 1)))
        agentctl_sanitize((const char *)d, n, j->files, (usize)n * 3 + 1, true);
}

static const char *display_of(const char *agent) {
    i32 a = agentctl_agent_parse(agent);
    const AgentctlAgentInfo *ai = agentctl_agent_info(a);
    return ai ? ai->display : agent;
}

static void fmt_ms(u64 ms, char *out, usize cap) {
    u64 s = ms / 1000;
    if (s < 60)        snprintf(out, cap, "%llus", (unsigned long long)s);
    else if (s < 3600) snprintf(out, cap, "%llum%02llus", (unsigned long long)(s / 60), (unsigned long long)(s % 60));
    else               snprintf(out, cap, "%lluh%02llum", (unsigned long long)(s / 3600), (unsigned long long)((s / 60) % 60));
}

static void print_head(const JobInfo *j) {
    char took[32];
    fmt_ms(j->elapsed_ms, took, sizeof took);
    const char *what = j->state;
    char buf[160];
    if (!strcmp(j->state, "idle")) {
        snprintf(buf, sizeof buf, "%s (turn %llu, %s%s%s)", j->outcome, (unsigned long long)j->turns, took,
                 j->usage[0] ? " \xC2\xB7 " : "", j->usage);
        what = buf;
    } else if (!strcmp(j->state, "running")) {
        snprintf(buf, sizeof buf, "running (turn %llu, %s so far)", (unsigned long long)(j->turns ? j->turns : 1), took);
        what = buf;
    } else if (!strcmp(j->state, "ended")) {
        const char *why = !strcmp(j->end, "taken_over") ? "the user took the session over"
                        : !strcmp(j->end, "closed") ? "closed"
                        : !strcmp(j->end, "could_not_start") ? "could not start"
                        : "its runner stopped";
        snprintf(buf, sizeof buf, "ended (%s)", why);
        what = buf;
    } else if (!strcmp(j->state, "awaiting_approval")) {
        what = "waiting for the user to allow it in Liu";
    }
    printf("job %llu \xC2\xB7 %s%s%s%s%s \xC2\xB7 %s\n", (unsigned long long)j->id, display_of(j->agent),
           j->model[0] ? " " : "", j->model, j->effort[0] ? " " : "", j->effort, what);
    if (j->label[0]) printf("label   %s\n", j->label);
    printf("folder  %s\n", j->cwd);
    if (j->worktree[0]) printf("worktree %s (branch %s)\n", j->worktree, j->branch);
    if (j->session[0]) printf("session %s\n", j->session);
    if (j->error[0] && strcmp(j->state, "running")) printf("error   %s\n", j->error);
    if (j->asking[0]) printf("waiting for the user to allow: %s\n", j->asking);
    if (j->merged[0]) printf("merge   %s\n", j->merged);
}

/* What a job spent, as a line: tokens, money, turns, and its limits. */
static void print_spend(const JobInfo *j) {
    unsigned long long in = 0, cached = 0, out = 0, cost = 0, mtok = 0, budget = 0;
    unsigned turns = 0, mturns = 0, dl = 0;
    if (sscanf(j->spend, "%llu %llu %llu %llu %u", &in, &cached, &out, &cost, &turns) != 5) return;
    sscanf(j->limits, "%u %llu %llu %u", &mturns, &mtok, &budget, &dl);
    char line[400];
    usize w = (usize)snprintf(line, sizeof line, "spent   %u turn%s \xC2\xB7 in %llu (cached %llu) \xC2\xB7 out %llu",
                              turns, turns == 1 ? "" : "s", in, cached, out);
    if (cost && w < sizeof line) w += (usize)snprintf(line + w, sizeof line - w, " \xC2\xB7 $%.4f", (double)cost / 1e6);
    if ((mturns || mtok || budget || dl) && w < sizeof line) {
        w += (usize)snprintf(line + w, sizeof line - w, "  (limits:");
        if (mturns && w < sizeof line) w += (usize)snprintf(line + w, sizeof line - w, " %u turns", mturns);
        if (mtok && w < sizeof line)   w += (usize)snprintf(line + w, sizeof line - w, " %llu tokens", mtok);
        if (budget && w < sizeof line) w += (usize)snprintf(line + w, sizeof line - w, " $%.2f", (double)budget / 1e6);
        if (dl && w < sizeof line)     w += (usize)snprintf(line + w, sizeof line - w, " %us a turn", dl);
        if (w < sizeof line) snprintf(line + w, sizeof line - w, ")");
    }
    printf("%s\n", line);
}

int agentctl_git(const char *dir, const char *const *args, char *out, usize ocap, char *err, usize ecap, bool whole);

/* The evidence under a job's report: the files it changed with what git
 * says of each (status and lines added / removed), the clashes it was in,
 * what it spent, and for an isolated job how to take its work. */
static void print_evidence(const JobInfo *j) {
    const char *root = j->worktree[0] ? j->worktree : j->cwd;
    if (j->files && j->files[0]) {
        /* the files, relative to the job's folder */
        const char *paths[48];
        char *copy = strdup(j->files);
        i32 n = 0, total = 0;
        for (char *line = copy; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (*line) { total++; if (n < 48) paths[n++] = line; }
            line = nl ? nl + 1 : NULL;
        }
        printf("\nfiles it changed (%d):\n", total);
        /* git's word on them, from the job's base (an isolated job) or HEAD */
        char *numstat = malloc(65536), *status = malloc(65536);
        bool git = false;
        if (numstat && status) {
            const char *args[64];
            i32 k = 0;
            args[k++] = "diff"; args[k++] = "--numstat"; args[k++] = "--no-ext-diff"; args[k++] = "--no-textconv";
            args[k++] = j->base[0] ? j->base : "HEAD";
            args[k++] = "--";
            for (i32 i = 0; i < n; i++) args[k++] = paths[i][0] == '-' ? paths[i] + 1 : paths[i];
            args[k] = NULL;
            char err[256];
            git = agentctl_git(root, args, numstat, 65536, err, sizeof err, true) == 0;
            k = 0;
            args[k++] = "status"; args[k++] = "--porcelain=v1"; args[k++] = "--untracked-files=all"; args[k++] = "--";
            for (i32 i = 0; i < n; i++) args[k++] = paths[i][0] == '-' ? paths[i] + 1 : paths[i];
            args[k] = NULL;
            if (agentctl_git(root, args, status, 65536, err, sizeof err, true) != 0) status[0] = '\0';
        }
        for (i32 i = 0; i < n; i++) {
            bool gone = paths[i][0] == '-';
            const char *abs = gone ? paths[i] + 1 : paths[i];
            const char *rel = agentctl_team_rel(abs, root);
            if (rel == abs) rel = agentctl_team_rel(abs, j->cwd);
            char what[64] = "";
            if (git) {
                /* numstat: "<added>\t<removed>\t<path>", relative to the repository's top */
                for (char *l = numstat; l && *l; ) {
                    char *nl = strchr(l, '\n');
                    usize len = nl ? (usize)(nl - l) : strlen(l);
                    const char *tab2 = NULL;
                    const char *t1 = memchr(l, '\t', len);
                    if (t1) tab2 = memchr(t1 + 1, '\t', len - (usize)(t1 + 1 - l));
                    if (tab2) {
                        usize pl = len - (usize)(tab2 + 1 - l);
                        usize al = strlen(abs);
                        if (pl <= al && !strncmp(abs + al - pl, tab2 + 1, pl) && (pl == al || abs[al - pl - 1] == '/')) {
                            snprintf(what, sizeof what, " +%.*s -%.*s", (int)(t1 - l), l, (int)(tab2 - t1 - 1), t1 + 1);
                            break;
                        }
                    }
                    l = nl ? nl + 1 : NULL;
                }
                for (char *l = status; l && *l; ) {
                    char *nl = strchr(l, '\n');
                    usize len = nl ? (usize)(nl - l) : strlen(l);
                    if (len > 3) {
                        usize pl = len - 3, al = strlen(abs);
                        if (pl <= al && !strncmp(abs + al - pl, l + 3, pl) && (pl == al || abs[al - pl - 1] == '/')) {
                            if (l[0] == '?' ) snprintf(what, sizeof what, " (new, not tracked)");
                            break;
                        }
                    }
                    l = nl ? nl + 1 : NULL;
                }
            }
            printf("  %s%s%s\n", rel, gone ? " (removed)" : "", what);
        }
        if (total > n) printf("  \xE2\x80\xA6 and %d more\n", total - n);
        if (git) printf("(`git -C %s diff %s -- <file>` shows a change)\n", root, j->base[0] ? j->base : "HEAD");
        free(numstat);
        free(status);
        free(copy);
    } else if (!strcmp(j->state, "idle")) {
        printf("\nfiles it changed: none that Liu saw\n");
    }
    if (j->clashes[0]) printf("\nclashes:\n%s", j->clashes);
    printf("\n");
    print_spend(j);
    if (j->worktree[0] && !strcmp(j->state, "idle"))
        printf("Its work is in its own worktree: `liu-agent merge %llu` takes it into %s, "
               "`liu-agent discard %llu` throws it away.\n", (unsigned long long)j->id, j->cwd,
               (unsigned long long)j->id);
}

/* The whole answer for a settled job; returns the exit status. */
static int print_settled(const JobInfo *j, bool full) {
    print_head(j);
    if (!strcmp(j->state, "declined")) return EX_REFUSED;
    if (!strcmp(j->state, "idle") && !strcmp(j->outcome, "done")) {
        printf("\n");
        print_file(j->dir, "result.md", full ? 0 : 64u * 1024u, 0);
        print_evidence(j);
        return EX_DONE;
    }
    if (!strcmp(j->state, "idle")) {
        printf("\nlast lines of its log:\n");
        print_file(j->dir, "events.log", 0, 15);
        print_evidence(j);
        return EX_FAILED;
    }
    if (!strcmp(j->state, "ended")) return EX_FAILED;
    return EX_WAITING;
}

static bool fetch_status(u64 job, JobInfo *out) {
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_STATUS, (u32)job) || !send_msg(&m)) return false;
    AgentctlFrame f; u8 *store = NULL;
    if (recv_reply(10000, &f, &store) != 1) return false;
    bool ok = reply_code(&f) == AGC_OK;
    if (ok) job_from(&f, out);
    else    print_text(stderr, &f);
    free(store);
    return ok;
}

/* ---------------------------------------------------------------- args --- */

static bool parse_job(const char *s, u64 *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end || v == 0 || v > 0xffffffffULL) return false;
    *out = v;
    return true;
}

static bool parse_secs(const char *s, i32 *ms) {
    char *end = NULL;
    double v = s ? strtod(s, &end) : -1;
    if (!s || !end || *end || v < 0 || v > 86400.0 * 7) return false;
    *ms = (i32)(v * 1000.0);
    return true;
}

/* The task: the words after the options joined by spaces, or stdin. */
static char *read_task(int argc, char **argv, int i, usize *len) {
    if (i >= argc || (i == argc - 1 && !strcmp(argv[i], "-"))) {
        if (isatty(STDIN_FILENO) && i >= argc) return NULL;
        usize cap = 4096, n = 0;
        char *b = malloc(cap);
        if (!b) return NULL;
        for (;;) {
            if (n + 4096 > cap) {
                if (cap >= AGENTCTL_MAX_PROMPT + 8192) break;
                cap *= 2;
                char *nb = realloc(b, cap);
                if (!nb) { free(b); return NULL; }
                b = nb;
            }
            ssize_t r = read(STDIN_FILENO, b + n, cap - n - 1);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            n += (usize)r;
        }
        b[n] = '\0';
        *len = n;
        return b;
    }
    usize total = 0;
    for (int k = i; k < argc; k++) total += strlen(argv[k]) + 1;
    char *b = malloc(total + 1);
    if (!b) return NULL;
    b[0] = '\0';
    usize n = 0;
    for (int k = i; k < argc; k++) {
        usize l = strlen(argv[k]);
        if (n) b[n++] = ' ';
        memcpy(b + n, argv[k], l);
        n += l;
    }
    b[n] = '\0';
    *len = n;
    return b;
}

/* ------------------------------------------------------------ commands --- */

static int do_wait(u64 *jobs, int count, bool any, i32 timeout_ms, bool full) {
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_WAIT, 0)) return EX_USAGE;
    for (int i = 0; i < count; i++) agentctl_msg_put_u64(&m, AGK_JOB, jobs[i]);
    if (any) agentctl_msg_put_str(&m, AGK_ANY, "1");
    if (!send_msg(&m)) return EX_USAGE;
    AgentctlFrame f; u8 *store = NULL;
    int rc = recv_reply(timeout_ms, &f, &store);
    if (rc < 0) return EX_USAGE;
    if (rc == 0) {
        /* still running: say how far each one is */
        close(g_fd);
        g_fd = -1;
        if (!connect_liu()) return EX_USAGE;
        for (int i = 0; i < count; i++) {
            JobInfo j;
            if (fetch_status(jobs[i], &j)) { print_head(&j); job_info_free(&j); }
        }
        printf("\nStill working. Run `liu-agent wait");
        for (int i = 0; i < count; i++) printf(" %llu", (unsigned long long)jobs[i]);
        printf("` again to keep waiting.\n");
        return EX_WAITING;
    }
    u64 code = reply_code(&f);
    if (code != AGC_OK) {
        print_text(stderr, &f);
        free(store);
        return code == AGC_NOTFOUND ? EX_REFUSED : EX_USAGE;
    }
    u64 settled[16];
    int ns = 0;
    u32 pos = 0; u16 k; const u8 *d; u32 n;
    while (agentctl_next(&f, &pos, &k, &d, &n) && ns < 16) {
        if (k != AGK_JOB) continue;
        u64 v = 0;
        for (u32 x = 0; x < n && x < 12; x++) v = v * 10u + (u64)(d[x] - '0');
        settled[ns++] = v;
    }
    free(store);
    int worst = EX_DONE;
    for (int i = 0; i < ns; i++) {
        JobInfo j;
        if (i) printf("\n");
        if (!fetch_status(settled[i], &j)) { worst = EX_FAILED; continue; }
        int ex = print_settled(&j, full);
        job_info_free(&j);
        if (ex > worst) worst = ex;
    }
    return worst;
}

/* "90", "90s", "20m", "1.5h": seconds. */
static bool parse_duration(const char *s, u32 *secs) {
    char *end = NULL;
    double v = s ? strtod(s, &end) : -1;
    if (!s || !end || end == s || v <= 0) return false;
    double mul = 1;
    if (*end == 's') end++;
    else if (*end == 'm') { mul = 60; end++; }
    else if (*end == 'h') { mul = 3600; end++; }
    if (*end) return false;
    double t = v * mul;
    if (t < 1 || t > 7 * 86400.0) return false;
    *secs = (u32)(t + 0.5);
    return true;
}

/* "5000", "200k", "1.5m": a count. */
static bool parse_count(const char *s, u64 *out) {
    char *end = NULL;
    double v = s ? strtod(s, &end) : -1;
    if (!s || !end || end == s || v <= 0) return false;
    if (*end == 'k' || *end == 'K') { v *= 1e3; end++; }
    else if (*end == 'm' || *end == 'M') { v *= 1e6; end++; }
    if (*end || v > 1e15) return false;
    *out = (u64)(v + 0.5);
    return *out > 0;
}

/* "2", "$0.50": dollars, in millionths. */
static bool parse_money(const char *s, u64 *micro) {
    if (s && *s == '$') s++;
    char *end = NULL;
    double v = s ? strtod(s, &end) : -1;
    if (!s || !end || end == s || *end || v <= 0 || v > 1e6) return false;
    *micro = (u64)(v * 1e6 + 0.5);
    return *micro > 0;
}

/* ---- roles: presets for --role ------------------------------------------ */

typedef struct {
    char name[32];
    char agent[16];          /* "" = the one named on the command line, else codex */
    char model[AGENTCTL_MODEL_CAP];
    char effort[AGENTCTL_EFFORT_CAP];
    char perm[8];
    char text[1024];         /* instructions put ahead of the task */
} Role;

static const Role BUILTIN_ROLES[] = {
    { "reviewer", "", "", "high", "read",
      "You are reviewing work another agent did. Do not change any file. Read the code and run what checks it "
      "(tests, the build), then report concrete problems with the file and line and what to do about each; say "
      "plainly if you find none." },
    { "planner", "", "", "high", "read",
      "Do not change any file. Read what you need and write a plan: the steps in order, the files each one "
      "touches, the interfaces between them, and the risks. Keep it short enough to act on." },
    { "tester", "", "", "medium", "write",
      "Write tests for what is described below, run them, and fix what is yours to fix. Report what you "
      "covered, what failed and why, and the command that runs the tests." },
    { "fixer", "", "", "medium", "write",
      "Find the cause of the problem below before you change anything, make the smallest change that fixes "
      "it, and show that it is fixed (a test, or the command and its output)." },
};

/* The user's own roles, ~/.config/Liu/agent-roles.json:
 * {"<name>": {"agent", "model", "effort", "permission", "instructions"}}. A
 * name of their own replaces a built-in one. */
static bool role_find(const char *name, Role *out) {
    bool found = false;
    for (usize i = 0; i < sizeof BUILTIN_ROLES / sizeof BUILTIN_ROLES[0]; i++)
        if (!strcmp(BUILTIN_ROLES[i].name, name)) { *out = BUILTIN_ROLES[i]; found = true; }
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    if (!home || (usize)snprintf(path, sizeof path, "%s/.config/Liu/agent-roles.json", home) >= sizeof path) return found;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return found;
    struct stat st;
    char *buf = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 256 * 1024 &&
        (buf = malloc((usize)st.st_size + 1))) {
        ssize_t n = read(fd, buf, (usize)st.st_size);
        buf[n > 0 ? n : 0] = '\0';
    }
    close(fd);
    if (!buf) return found;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    const cJSON *r = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, name) : NULL;
    if (cJSON_IsObject(r)) {
        Role mine;
        memset(&mine, 0, sizeof mine);
        snprintf(mine.name, sizeof mine.name, "%s", name);
        const cJSON *v;
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(r, "agent")))        snprintf(mine.agent, sizeof mine.agent, "%s", v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(r, "model")))        snprintf(mine.model, sizeof mine.model, "%s", v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(r, "effort")))       snprintf(mine.effort, sizeof mine.effort, "%s", v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(r, "permission")))   snprintf(mine.perm, sizeof mine.perm, "%s", v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(r, "instructions"))) snprintf(mine.text, sizeof mine.text, "%s", v->valuestring);
        if ((!mine.agent[0] || agentctl_agent_parse(mine.agent) >= 0) && agentctl_model_ok(mine.model) &&
            agentctl_effort_ok(mine.effort) && (!mine.perm[0] || agentctl_perm_parse(mine.perm) >= 0)) {
            *out = mine;
            found = true;
        }
    }
    cJSON_Delete(root);
    return found;
}

static int cmd_roles(void) {
    printf("Presets for `liu-agent spawn --role NAME` (yours in ~/.config/Liu/agent-roles.json replace these):\n");
    for (usize i = 0; i < sizeof BUILTIN_ROLES / sizeof BUILTIN_ROLES[0]; i++) {
        Role r;
        role_find(BUILTIN_ROLES[i].name, &r);
        printf("  %-9s %s%s%s level %s%s%s: %.90s\n", r.name, r.agent[0] ? r.agent : "(the agent you name)",
               r.model[0] ? " " : "", r.model, r.perm[0] ? r.perm : "write", r.effort[0] ? ", effort " : "", r.effort, r.text);
    }
    printf("\nA role of your own: {\"name\": {\"agent\": \"claude\", \"model\": \"...\", \"effort\": \"high\", "
           "\"permission\": \"read\", \"instructions\": \"...\"}}\n");
    return EX_DONE;
}

/* ---- models: what Liu last read from each agent ------------------------- */

/* The cached list for `agent` (codex, claude, opencode), one id a line, and
 * its age in days; false when there is none. */
static bool models_cached(const char *agent, char *out, usize cap, i32 *age_days) {
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    if (!home || (usize)snprintf(path, sizeof path, "%s/.config/Liu/agent_models.json", home) >= sizeof path) return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    char *buf = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 1024 * 1024 &&
        (buf = malloc((usize)st.st_size + 1))) {
        ssize_t n = read(fd, buf, (usize)st.st_size);
        buf[n > 0 ? n : 0] = '\0';
    }
    close(fd);
    if (!buf) return false;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    const cJSON *e = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, agent) : NULL;
    const cJSON *ids = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "ids") : NULL;
    const cJSON *ts = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "fetched_at") : NULL;
    bool ok = cJSON_IsArray(ids) && cJSON_GetArraySize(ids) > 0;
    if (ok) {
        usize w = 0;
        out[0] = '\0';
        const cJSON *id;
        cJSON_ArrayForEach(id, ids)
            if (cJSON_IsString(id) && w < cap) w += (usize)snprintf(out + w, cap - w, "%s\n", id->valuestring);
        *age_days = cJSON_IsNumber(ts) ? (i32)((time(NULL) - (time_t)ts->valuedouble) / 86400) : -1;
    }
    cJSON_Delete(root);
    return ok;
}

static bool line_list_has(const char *list, const char *word) {
    usize wl = strlen(word);
    for (const char *l = list; l && *l; ) {
        const char *nl = strchr(l, '\n');
        usize len = nl ? (usize)(nl - l) : strlen(l);
        if (len == wl && !strncmp(l, word, wl)) return true;
        l = nl ? nl + 1 : NULL;
    }
    return false;
}

static int cmd_models(int argc, char **argv) {
    static const char *const agents[] = { "codex", "claude", "opencode" };
    char list[16384];
    bool any = false;
    for (usize i = 0; i < sizeof agents / sizeof agents[0]; i++) {
        if (argc > 0 && strcmp(argv[0], agents[i])) continue;
        i32 age = -1;
        if (!models_cached(agents[i], list, sizeof list, &age)) continue;
        any = true;
        if (age >= 0) printf("%s (read %d day%s ago; Settings > Translate reads it again):\n", agents[i], age, age == 1 ? "" : "s");
        else          printf("%s:\n", agents[i]);
        for (char *l = list; *l; ) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = '\0';
            printf("  %s\n", l);
            if (!nl) break;
            l = nl + 1;
        }
    }
    if (!any) printf("Liu has no model list for %s yet. A model the agent does not know fails its first turn at once.\n",
                     argc > 0 ? argv[0] : "these agents");
    return EX_DONE;
}

/* ---- --from: a job's work carried into a new one ------------------------ */

/* The folder this command's own binary is in (liu-history lives beside it). */
static bool self_dir(char *out, usize cap) {
    char path[PATH_MAX], real[PATH_MAX];
#if defined(__APPLE__)
    uint32_t size = sizeof path;
    extern int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
    if (_NSGetExecutablePath(path, &size) != 0) return false;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return false;
    path[n] = '\0';
#endif
    if (!realpath(path, real)) return false;
    char *slash = strrchr(real, '/');
    if (!slash) return false;
    *slash = '\0';
    return (usize)snprintf(out, cap, "%s", real) < cap;
}

/* `liu-history continue <session> --to=<agent>`: the job's history in the
 * new agent's own store, when Liu can write it there. The new session's id,
 * or "" when it could only be a briefing. */
static void carry_history(const char *session, const char *to, char *out, usize cap) {
    out[0] = '\0';
    char dir[PATH_MAX], bin[PATH_MAX + 32], to_arg[64];
    if (!self_dir(dir, sizeof dir) || (usize)snprintf(bin, sizeof bin, "%s/liu-history", dir) >= sizeof bin ||
        access(bin, X_OK) != 0)
        return;
    snprintf(to_arg, sizeof to_arg, "--to=%s", to);
    int pfd[2];
    if (pipe(pfd) != 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return; }
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); dup2(nul, STDERR_FILENO); }
        close(pfd[0]);
        close(pfd[1]);
        execl(bin, "liu-history", "continue", session, to_arg, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char buf[4096];
    usize n = 0;
    for (;;) {
        ssize_t r = read(pfd[0], buf + n, sizeof buf - 1 - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (usize)r;
        if (n >= sizeof buf - 1) break;
    }
    close(pfd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    buf[n] = '\0';
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return;
    /* "<mode> session <id>": only a native one is a session to resume */
    char mode[64], id[AGENTCTL_SESSION_CAP];
    if (sscanf(buf, "%63s session %127s", mode, id) == 2 && strcmp(mode, "context_handoff") != 0 &&
        agentctl_session_ok(id))
        snprintf(out, cap, "%s", id);
}

/* The new job's task when it continues job `from`: what that one was asked,
 * what it reported and the files it changed, ahead of `task`. malloc'd. */
static char *carry_brief(const JobInfo *f, const char *task, usize tlen, usize *out_len) {
    char result[8192] = "";
    char path[PATH_MAX];
    if (f->dir[0] && (usize)snprintf(path, sizeof path, "%s/result.md", f->dir) < sizeof path) {
        int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat st;
        if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            char raw[8192];
            ssize_t n = read(fd, raw, sizeof raw - 1);
            if (n > 0) agentctl_sanitize(raw, (usize)n, result, sizeof result, true);
        }
        if (fd >= 0) close(fd);
    }
    usize cap = tlen + sizeof result + 20000, w = 0;
    char *p = malloc(cap);
    if (!p) return NULL;
    w += (usize)snprintf(p + w, cap - w, "You continue the work %s did (Liu job %llu) in %s.\n",
                         display_of(f->agent), (unsigned long long)f->id, f->worktree[0] ? f->worktree : f->cwd);
    if (result[0]) w += (usize)snprintf(p + w, cap - w, "What it reported last:\n%s\n", result);
    if (f->files && f->files[0]) {
        w += (usize)snprintf(p + w, cap - w, "The files it changed:\n");
        for (const char *l = f->files; *l && w + 1100 < cap; ) {
            const char *nl = strchr(l, '\n');
            usize len = nl ? (usize)(nl - l) : strlen(l);
            w += (usize)snprintf(p + w, cap - w, "- %.*s\n", (int)len, l);
            if (!nl) break;
            l = nl + 1;
        }
    }
    w += (usize)snprintf(p + w, cap - w, "\n");
    memcpy(p + w, task, tlen);
    w += tlen;
    p[w] = '\0';
    *out_len = w;
    return p;
}

/* ---- spawn --------------------------------------------------------------- */

typedef struct {
    const char *model, *effort, *perm, *label;
    const char *owns[AGT_OWNS_MAX];
    int  nowns;
    u64  after[4];
    int  nafter;
    u64  review;
    u64  max_tokens, budget;
    u32  max_turns, deadline;
    bool isolate, no_ask, wait;
    i32  timeout_ms;
    char cwd[PATH_MAX];
    const char *session;            /* the first turn resumes it */
} SpawnOpts;

/* One spawn request and what came of it; returns the exit status. */
static int spawn_one(const char *agent, const SpawnOpts *o, const char *task, usize tlen, u64 *job_out) {
    if (!connect_liu()) return EX_USAGE;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_SPAWN, 0)) return EX_USAGE;
    agentctl_msg_put_str(&m, AGK_AGENT, agent);
    agentctl_msg_put_str(&m, AGK_MODEL, o->model);
    agentctl_msg_put_str(&m, AGK_EFFORT, o->effort);
    agentctl_msg_put_str(&m, AGK_PERMISSION, o->perm);
    agentctl_msg_put_str(&m, AGK_CWD, o->cwd);
    agentctl_msg_put_str(&m, AGK_LABEL, o->label);
    for (int k = 0; k < o->nowns; k++) agentctl_msg_put_str(&m, AGK_PATH, o->owns[k]);
    for (int k = 0; k < o->nafter; k++) agentctl_msg_put_u64(&m, AGK_AFTER, o->after[k]);
    if (o->review) agentctl_msg_put_u64(&m, AGK_REVIEW, o->review);
    if (o->isolate) agentctl_msg_put_str(&m, AGK_ISOLATE, "1");
    if (o->no_ask) agentctl_msg_put_str(&m, AGK_ASK_TOOLS, "0");
    if (o->max_turns) agentctl_msg_put_u64(&m, AGK_MAX_TURNS, o->max_turns);
    if (o->max_tokens) agentctl_msg_put_u64(&m, AGK_MAX_TOKENS, o->max_tokens);
    if (o->budget) agentctl_msg_put_u64(&m, AGK_BUDGET, o->budget);
    if (o->deadline) agentctl_msg_put_u64(&m, AGK_DEADLINE, o->deadline);
    if (o->session && o->session[0]) agentctl_msg_put_str(&m, AGK_SESSION, o->session);
    agentctl_msg_put(&m, AGK_PROMPT, task, (u32)tlen);
    if (!send_msg(&m)) return EX_USAGE;

    AgentctlFrame f; u8 *store = NULL;
    if (recv_reply(15000, &f, &store) != 1) return EX_USAGE;
    u64 code = reply_code(&f);
    JobInfo j;
    memset(&j, 0, sizeof j);
    job_from(&f, &j);
    if (code != AGC_PENDING && code != AGC_OK) {
        print_text(stderr, &f);
        free(store);
        job_info_free(&j);
        return code == AGC_REFUSED || code == AGC_DECLINED ? EX_REFUSED : EX_USAGE;
    }
    bool trusted = code == AGC_OK;
    free(store);
    if (code == AGC_PENDING) {
        fprintf(stderr, "job %llu: waiting for the user to allow it in Liu\xE2\x80\xA6\n", (unsigned long long)j.id);
        i32 approval_ms = o->timeout_ms >= 0 ? o->timeout_ms : 100000;
        int rc = recv_reply(approval_ms, &f, &store);
        if (rc < 0) { job_info_free(&j); return EX_USAGE; }
        if (rc == 0) {
            printf("job %llu is waiting for the user to allow it in Liu.\n"
                   "Run `liu-agent wait %llu` to wait for it.\n", (unsigned long long)j.id, (unsigned long long)j.id);
            if (job_out) *job_out = j.id;
            job_info_free(&j);
            return EX_WAITING;
        }
        code = reply_code(&f);
        job_info_free(&j);
        memset(&j, 0, sizeof j);
        job_from(&f, &j);
        free(store);
        if (code == AGC_DECLINED) {
            printf("job %llu was not started: %s\n", (unsigned long long)j.id, j.error[0] ? j.error : "the user declined");
            printf("Do not ask again unless the user says so.\n");
            job_info_free(&j);
            return EX_REFUSED;
        }
        if (code != AGC_OK) { job_info_free(&j); return EX_USAGE; }
    }
    if (job_out) *job_out = j.id;
    const char *what = !strcmp(j.state, "queued")
        ? (o->nafter ? "queued (it starts once the jobs it follows have finished)"
                     : "queued (others are working; it starts when one finishes)")
        : "started";
    printf("job %llu %s%s: %s%s%s%s%s in %s\n", (unsigned long long)j.id, what, trusted ? " without asking (this tab is trusted)" : "",
           display_of(j.agent), j.model[0] ? " " : "", j.model, j.effort[0] ? " " : "", j.effort, j.cwd);
    job_info_free(&j);
    return EX_DONE;
}

static int cmd_spawn(int argc, char **argv) {
    if (argc < 1) { usage(stderr); return EX_USAGE; }
    SpawnOpts o;
    memset(&o, 0, sizeof o);
    o.model = ""; o.effort = ""; o.perm = NULL; o.label = "";
    o.timeout_ms = -1;
    if (!getcwd(o.cwd, sizeof o.cwd)) o.cwd[0] = '\0';
    const char *agents_arg = NULL, *role_name = NULL;
    u64 from = 0;
    int i = 0;
    if (argv[0][0] != '-') agents_arg = argv[i++];
    for (; i < argc; i++) {
        const char *a = argv[i];
        bool has_next = i + 1 < argc;
        if (!strcmp(a, "--")) { i++; break; }
        else if ((!strcmp(a, "-m") || !strcmp(a, "--model")) && has_next) o.model = argv[++i];
        else if ((!strcmp(a, "-e") || !strcmp(a, "--effort")) && has_next) o.effort = argv[++i];
        else if ((!strcmp(a, "-p") || !strcmp(a, "--permission")) && has_next) o.perm = argv[++i];
        else if ((!strcmp(a, "-l") || !strcmp(a, "--label")) && has_next) o.label = argv[++i];
        else if ((!strcmp(a, "-C") || !strcmp(a, "--cwd")) && has_next) {
            char real[PATH_MAX];
            if (!realpath(argv[++i], real)) { fprintf(stderr, "liu-agent: no such folder: %s\n", argv[i]); return EX_USAGE; }
            snprintf(o.cwd, sizeof o.cwd, "%s", real);
        }
        else if (!strcmp(a, "-w") || !strcmp(a, "--wait")) o.wait = true;
        else if ((!strcmp(a, "-t") || !strcmp(a, "--timeout")) && has_next) {
            if (!parse_secs(argv[++i], &o.timeout_ms)) { fprintf(stderr, "liu-agent: bad --timeout\n"); return EX_USAGE; }
        }
        else if (!strcmp(a, "--owns") && has_next) {
            if (o.nowns >= AGT_OWNS_MAX) { fprintf(stderr, "liu-agent: at most %d --owns paths\n", AGT_OWNS_MAX); return EX_USAGE; }
            o.owns[o.nowns++] = argv[++i];
        }
        else if (!strcmp(a, "--role") && has_next) role_name = argv[++i];
        else if (!strcmp(a, "--isolate")) o.isolate = true;
        else if (!strcmp(a, "--no-ask")) o.no_ask = true;
        else if (!strcmp(a, "--after") && has_next) {
            if (o.nafter >= 4 || !parse_job(argv[++i], &o.after[o.nafter])) { fprintf(stderr, "liu-agent: --after takes up to 4 job numbers\n"); return EX_USAGE; }
            o.nafter++;
        }
        else if (!strcmp(a, "--from") && has_next) {
            if (!parse_job(argv[++i], &from)) { fprintf(stderr, "liu-agent: --from takes a job number\n"); return EX_USAGE; }
        }
        else if (!strcmp(a, "--review") && has_next) {
            if (!parse_job(argv[++i], &o.review)) { fprintf(stderr, "liu-agent: --review takes a job number\n"); return EX_USAGE; }
        }
        else if (!strcmp(a, "--max-turns") && has_next) {
            u64 v = 0;
            if (!parse_count(argv[++i], &v) || v > 100000) { fprintf(stderr, "liu-agent: bad --max-turns\n"); return EX_USAGE; }
            o.max_turns = (u32)v;
        }
        else if (!strcmp(a, "--max-tokens") && has_next) {
            if (!parse_count(argv[++i], &o.max_tokens)) { fprintf(stderr, "liu-agent: bad --max-tokens (e.g. 500k)\n"); return EX_USAGE; }
        }
        else if (!strcmp(a, "--budget-usd") && has_next) {
            if (!parse_money(argv[++i], &o.budget)) { fprintf(stderr, "liu-agent: bad --budget-usd (e.g. 2.50)\n"); return EX_USAGE; }
        }
        else if (!strcmp(a, "--turn-timeout") && has_next) {
            if (!parse_duration(argv[++i], &o.deadline)) { fprintf(stderr, "liu-agent: bad --turn-timeout (e.g. 20m)\n"); return EX_USAGE; }
        }
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "liu-agent: unknown option %s\n", a); return EX_USAGE; }
        else break;
    }
    /* a role fills in what the command line left out */
    Role role;
    memset(&role, 0, sizeof role);
    if (role_name && !role_find(role_name, &role)) {
        fprintf(stderr, "liu-agent: no role named %s (see `liu-agent roles`)\n", role_name);
        return EX_USAGE;
    }
    if (!agents_arg) agents_arg = role.agent[0] ? role.agent : NULL;
    if (!agents_arg) { fprintf(stderr, "liu-agent: start which agent? (%s)\n", AGENTCTL_AGENT_LIST); return EX_USAGE; }
    if (!o.model[0]) o.model = role.model;
    if (!o.effort[0]) o.effort = role.effort;
    if (!o.perm) o.perm = role.perm[0] ? role.perm : "write";
    if (!o.label[0] && role_name) o.label = role_name;
    if (!agentctl_model_ok(o.model)) { fprintf(stderr, "liu-agent: that model name is not allowed\n"); return EX_USAGE; }
    if (!agentctl_effort_ok(o.effort)) { fprintf(stderr, "liu-agent: effort must be none, minimal, low, medium, high, xhigh or max\n"); return EX_USAGE; }
    if (agentctl_perm_parse(o.perm) < 0) { fprintf(stderr, "liu-agent: permission must be read, write or full\n"); return EX_USAGE; }

    /* the agents: one, or several working the same task side by side */
    char agents_buf[128];
    snprintf(agents_buf, sizeof agents_buf, "%s", agents_arg);
    const char *agents[4];
    int nagents = 0;
    for (char *t = strtok(agents_buf, ",+"); t && nagents < 4; t = strtok(NULL, ",+")) {
        if (agentctl_agent_parse(t) < 0) { fprintf(stderr, "liu-agent: unknown agent '%s'. Liu can start: %s.\n", t, AGENTCTL_AGENT_LIST); return EX_USAGE; }
        agents[nagents++] = t;
    }
    if (nagents == 0) { fprintf(stderr, "liu-agent: start which agent? (%s)\n", AGENTCTL_AGENT_LIST); return EX_USAGE; }
    if (nagents > 1 && !o.isolate) {
        fprintf(stderr, "liu-agent: several agents on one task need --isolate, so each works in a worktree of its own "
                        "and none overwrites another; then `liu-agent compare` and `merge` the best.\n");
        return EX_USAGE;
    }

    usize tlen = 0;
    char *task = read_task(argc, argv, i, &tlen);
    if ((!task || tlen == 0) && o.review) { free(task); task = strdup("Review it."); tlen = task ? strlen(task) : 0; }
    if (!task || tlen == 0) { free(task); fprintf(stderr, "liu-agent: no task given\n"); return EX_USAGE; }
    if (role.text[0]) {
        usize rl = strlen(role.text);
        char *both = malloc(rl + 2 + tlen + 1);
        if (both) {
            memcpy(both, role.text, rl);
            memcpy(both + rl, "\n\n", 2);
            memcpy(both + rl + 2, task, tlen + 1);
            free(task);
            task = both;
            tlen += rl + 2;
        }
    }
    if (tlen > AGENTCTL_MAX_PROMPT) { free(task); fprintf(stderr, "liu-agent: the task is longer than 256 KB\n"); return EX_USAGE; }

    /* the model: a name the agent's last known list does not have is said,
     * not refused (lists go stale; a new model is not on it yet) */
    if (o.model[0]) {
        for (int k = 0; k < nagents; k++) {
            char list[16384];
            i32 age = -1;
            if (models_cached(agents[k], list, sizeof list, &age) && !line_list_has(list, o.model))
                fprintf(stderr, "liu-agent: note: %s is not in the model list Liu last read for %s%s; if its first turn "
                                "fails on the model, `liu-agent models %s` shows the names it knew.\n",
                        o.model, agents[k], age > 7 ? " (an old list)" : "", agents[k]);
        }
    }

    /* --from: the earlier job's history in the new agent's store when Liu
     * can write it there, its report and files ahead of the task always */
    char carried[AGENTCTL_SESSION_CAP] = "";
    if (from) {
        if (nagents > 1) { free(task); fprintf(stderr, "liu-agent: --from takes one agent\n"); return EX_USAGE; }
        if (!connect_liu()) { free(task); return EX_USAGE; }
        JobInfo fj;
        memset(&fj, 0, sizeof fj);
        if (!fetch_status(from, &fj)) { free(task); return EX_REFUSED; }
        close(g_fd);
        g_fd = -1;
        i32 to = agentctl_agent_parse(agents[0]);
        if (fj.session[0] && agentctl_agent_parse(fj.agent) != to) {
            carry_history(fj.session, agentctl_agent_info(to)->id, carried, sizeof carried);
            if (carried[0]) fprintf(stderr, "liu-agent: job %llu's history is now %s session %s; the new job resumes it.\n",
                                    (unsigned long long)from, display_of(agents[0]), carried);
        }
        usize blen = 0;
        char *brief = carry_brief(&fj, task, tlen, &blen);
        job_info_free(&fj);
        if (brief) { free(task); task = brief; tlen = blen; }
        if (carried[0]) o.session = carried;
    }

    int worst = EX_DONE;
    u64 ids[4] = { 0 };
    char labels[4][AGENTCTL_LABEL_CAP];
    const char *base_label = o.label;
    for (int k = 0; k < nagents; k++) {
        if (nagents > 1) {
            snprintf(labels[k], sizeof labels[k], "%s%s%s", base_label[0] ? base_label : "", base_label[0] ? " " : "", agents[k]);
            o.label = labels[k];
        }
        int rc = spawn_one(agents[k], &o, task, tlen, &ids[k]);
        if (g_fd >= 0) { close(g_fd); g_fd = -1; }
        if (rc > worst) worst = rc;
    }
    free(task);
    if (worst != EX_DONE) return worst;
    if (nagents > 1) {
        printf("They work on the same task, each in its own worktree. `liu-agent wait");
        for (int k = 0; k < nagents; k++) printf(" %llu", (unsigned long long)ids[k]);
        printf("` waits for all; `liu-agent compare");
        for (int k = 0; k < nagents; k++) printf(" %llu", (unsigned long long)ids[k]);
        printf("` puts their work side by side; then `merge` one and `discard` the rest.\n");
    }
    if (!o.wait) {
        if (nagents == 1)
            printf("Liu shows it as a tab beside this one. `liu-agent wait %llu` prints its result when it finishes.\n",
                   (unsigned long long)ids[0]);
        return EX_DONE;
    }
    printf("\n");
    if (!connect_liu()) return EX_USAGE;
    return do_wait(ids, nagents, false, o.timeout_ms, false);
}

/* `liu-agent review <job> [<agent>] [options] [--] [what to look at]`: a
 * read-only review of a job's work by another agent. */
static int cmd_review(int argc, char **argv) {
    u64 job;
    if (argc < 1 || !parse_job(argv[0], &job)) { fprintf(stderr, "liu-agent: review which job?\n"); return EX_USAGE; }
    const char *agent = NULL;
    int i = 1;
    if (i < argc && argv[i][0] != '-' && agentctl_agent_parse(argv[i]) >= 0) agent = argv[i++];
    if (!agent) {
        /* another model than the one that did the work */
        if (!connect_liu()) return EX_USAGE;
        JobInfo j;
        memset(&j, 0, sizeof j);
        if (!fetch_status(job, &j)) return EX_REFUSED;
        close(g_fd);
        g_fd = -1;
        agent = agentctl_agent_parse(j.agent) == AGENTCTL_AGENT_CLAUDE ? "codex" : "claude";
        job_info_free(&j);
    }
    char job_s[24];
    snprintf(job_s, sizeof job_s, "%llu", (unsigned long long)job);
    /* spawn <agent> --review <job> -p read [the rest] */
    char **av = calloc((usize)argc + 8, sizeof *av);
    if (!av) return EX_USAGE;
    int n = 0;
    av[n++] = (char *)agent;
    av[n++] = (char *)"--review";
    av[n++] = job_s;
    bool has_perm = false;
    for (int k = i; k < argc; k++) if (!strcmp(argv[k], "-p") || !strcmp(argv[k], "--permission")) has_perm = true;
    if (!has_perm) { av[n++] = (char *)"-p"; av[n++] = (char *)"read"; }
    for (int k = i; k < argc; k++) av[n++] = argv[k];
    int rc = cmd_spawn(n, av);
    free(av);
    return rc;
}

static int cmd_wait(int argc, char **argv) {
    u64 jobs[16];
    int n = 0;
    bool any = false, full = false;
    i32 timeout_ms = -1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--any")) any = true;
        else if (!strcmp(argv[i], "--full")) full = true;
        else if ((!strcmp(argv[i], "-t") || !strcmp(argv[i], "--timeout")) && i + 1 < argc) {
            if (!parse_secs(argv[++i], &timeout_ms)) { fprintf(stderr, "liu-agent: bad --timeout\n"); return EX_USAGE; }
        } else if (n < 16 && parse_job(argv[i], &jobs[n])) n++;
        else { fprintf(stderr, "liu-agent: not a job number: %s\n", argv[i]); return EX_USAGE; }
    }
    if (n == 0) { fprintf(stderr, "liu-agent: wait for which job?\n"); return EX_USAGE; }
    if (!connect_liu()) return EX_USAGE;
    return do_wait(jobs, n, any, timeout_ms, full);
}

static int cmd_send(int argc, char **argv) {
    u64 job;
    if (argc < 1 || !parse_job(argv[0], &job)) { fprintf(stderr, "liu-agent: send to which job?\n"); return EX_USAGE; }
    bool wait = false;
    i32 timeout_ms = -1;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--wait")) wait = true;
        else if ((!strcmp(argv[i], "-t") || !strcmp(argv[i], "--timeout")) && i + 1 < argc) {
            if (!parse_secs(argv[++i], &timeout_ms)) { fprintf(stderr, "liu-agent: bad --timeout\n"); return EX_USAGE; }
        } else break;
    }
    usize len = 0;
    char *msg = read_task(argc, argv, i, &len);
    if (!msg || len == 0) { free(msg); fprintf(stderr, "liu-agent: no message given\n"); return EX_USAGE; }
    if (len > AGENTCTL_MAX_PROMPT) { free(msg); fprintf(stderr, "liu-agent: the message is longer than 256 KB\n"); return EX_USAGE; }
    if (!connect_liu()) { free(msg); return EX_USAGE; }
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_SEND, (u32)job)) { free(msg); return EX_USAGE; }
    agentctl_msg_put(&m, AGK_PROMPT, msg, (u32)len);
    free(msg);
    if (!send_msg(&m)) return EX_USAGE;
    AgentctlFrame f; u8 *store = NULL;
    if (recv_reply(10000, &f, &store) != 1) return EX_USAGE;
    u64 code = reply_code(&f);
    if (code != AGC_OK) {
        print_text(stderr, &f);
        free(store);
        return code == AGC_BUSY ? EX_WAITING : EX_REFUSED;
    }
    free(store);
    printf("job %llu: message sent; it is working on it.\n", (unsigned long long)job);
    if (!wait) return EX_DONE;
    printf("\n");
    return do_wait(&job, 1, false, timeout_ms, false);
}

static int cmd_simple(u16 op, int argc, char **argv, const char *done_text) {
    u64 job;
    if (argc < 1 || !parse_job(argv[0], &job)) { fprintf(stderr, "liu-agent: which job?\n"); return EX_USAGE; }
    if (!connect_liu()) return EX_USAGE;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, op, (u32)job) || !send_msg(&m)) return EX_USAGE;
    AgentctlFrame f; u8 *store = NULL;
    if (recv_reply(10000, &f, &store) != 1) return EX_USAGE;
    u64 code = reply_code(&f);
    if (code != AGC_OK) {
        print_text(stderr, &f);
        free(store);
        return EX_REFUSED;
    }
    free(store);
    printf("job %llu: %s\n", (unsigned long long)job, done_text);
    return EX_DONE;
}

static int cmd_list(void) {
    if (!connect_liu()) return EX_USAGE;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_LIST, 0) || !send_msg(&m)) return EX_USAGE;
    AgentctlFrame f; u8 *store = NULL;
    if (recv_reply(10000, &f, &store) != 1) return EX_USAGE;
    if (reply_code(&f) != AGC_OK) {
        print_text(stderr, &f);
        free(store);
        return EX_REFUSED;
    }
    u32 pos = 0; u16 k; const u8 *d; u32 n;
    int lines = 0;
    while (agentctl_next(&f, &pos, &k, &d, &n)) {
        if (k != AGK_LINE) continue;
        char raw[512], clean[512];
        usize l = n < sizeof raw - 1 ? n : sizeof raw - 1;
        memcpy(raw, d, l);
        raw[l] = '\0';
        agentctl_sanitize(raw, l, clean, sizeof clean, false);
        if (!lines) printf("job\tagent\tstate\tlabel\n");
        printf("%s\n", clean);
        lines++;
    }
    if (!lines) printf("No jobs started from this pane.\n");
    free(store);
    return EX_DONE;
}

static int cmd_status(int argc, char **argv) {
    if (argc < 1) return cmd_list();
    u64 job;
    if (!parse_job(argv[0], &job)) { fprintf(stderr, "liu-agent: not a job number: %s\n", argv[0]); return EX_USAGE; }
    if (!connect_liu()) return EX_USAGE;
    JobInfo j;
    if (!fetch_status(job, &j)) return EX_REFUSED;
    print_head(&j);
    print_spend(&j);
    job_info_free(&j);
    return EX_DONE;
}

static int cmd_result(int argc, char **argv, bool log) {
    u64 job;
    if (argc < 1 || !parse_job(argv[0], &job)) { fprintf(stderr, "liu-agent: which job?\n"); return EX_USAGE; }
    bool full = false;
    i32 tail = 40;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--full")) full = true;
        else if (!strcmp(argv[i], "--tail") && i + 1 < argc) tail = atoi(argv[++i]);
    }
    if (!connect_liu()) return EX_USAGE;
    JobInfo j;
    if (!fetch_status(job, &j)) return EX_REFUSED;
    if (log) { print_file(j.dir, "events.log", 0, tail > 0 ? tail : 40); job_info_free(&j); return EX_DONE; }
    print_head(&j);
    printf("\n");
    print_file(j.dir, "result.md", full ? 0 : 64u * 1024u, 0);
    print_evidence(&j);
    job_info_free(&j);
    return EX_DONE;
}

static int cmd_agents(void) {
    if (!connect_liu()) return EX_USAGE;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_AGENTS, 0) || !send_msg(&m)) return EX_USAGE;
    AgentctlFrame f; u8 *store = NULL;
    if (recv_reply(10000, &f, &store) != 1) return EX_USAGE;
    if (reply_code(&f) != AGC_OK) {
        print_text(stderr, &f);
        free(store);
        return EX_REFUSED;
    }
    u32 pos = 0; u16 k; const u8 *d; u32 n;
    while (agentctl_next(&f, &pos, &k, &d, &n)) {
        if (k == AGK_LINE) printf("%.*s\n", (int)n, (const char *)d);
    }
    print_text(stdout, &f);
    free(store);
    return EX_DONE;
}

/* `liu-agent merge|discard <job>`: an isolated job's work taken into the
 * folder or thrown away; the command waits until its runner has done it. */
static int cmd_merge(int argc, char **argv, bool merge) {
    u64 job;
    if (argc < 1 || !parse_job(argv[0], &job)) { fprintf(stderr, "liu-agent: which job?\n"); return EX_USAGE; }
    if (!connect_liu()) return EX_USAGE;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, merge ? AGOP_MERGE : AGOP_DISCARD, (u32)job) || !send_msg(&m)) return EX_USAGE;
    AgentctlFrame f; u8 *store = NULL;
    if (recv_reply(10000, &f, &store) != 1) return EX_USAGE;
    u64 code = reply_code(&f);
    if (code != AGC_OK) { print_text(stderr, &f); free(store); return EX_REFUSED; }
    free(store);
    close(g_fd);
    g_fd = -1;
    printf("job %llu: %s\xE2\x80\xA6\n", (unsigned long long)job, merge ? "merging its work" : "discarding its work");
    fflush(stdout);
    for (int spins = 0; spins < 3000; spins++) {           /* up to 10 minutes */
        if (!connect_liu()) return EX_USAGE;
        JobInfo j;
        memset(&j, 0, sizeof j);
        bool ok = fetch_status(job, &j);
        close(g_fd);
        g_fd = -1;
        if (!ok) return EX_FAILED;
        bool ended = !strcmp(j.state, "ended");
        if (ended || j.merged[0]) {
            printf("%s\n", j.merged[0] ? j.merged : "done");
            int rc = ended && (!strcmp(j.end, "merged") || !strcmp(j.end, "discarded")) ? EX_DONE : EX_FAILED;
            job_info_free(&j);
            return rc;
        }
        job_info_free(&j);
        usleep(200000);
    }
    printf("Still at it; `liu-agent status %llu` says when it is done.\n", (unsigned long long)job);
    return EX_WAITING;
}

/* `liu-agent compare <job> <job>...`: each job's report head, files and
 * spend, one after another, to choose between them. */
static int cmd_compare(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "liu-agent: compare which jobs?\n"); return EX_USAGE; }
    for (int i = 0; i < argc && i < 8; i++) {
        u64 job;
        if (!parse_job(argv[i], &job)) { fprintf(stderr, "liu-agent: not a job number: %s\n", argv[i]); return EX_USAGE; }
        if (!connect_liu()) return EX_USAGE;
        JobInfo j;
        memset(&j, 0, sizeof j);
        bool ok = fetch_status(job, &j);
        close(g_fd);
        g_fd = -1;
        if (!ok) continue;
        printf("%s======== job %llu \xC2\xB7 %s%s%s ========\n", i ? "\n" : "", (unsigned long long)job,
               display_of(j.agent), j.label[0] ? " \xC2\xB7 " : "", j.label);
        print_head(&j);
        printf("\n");
        print_file(j.dir, "result.md", 4096, 0);
        print_evidence(&j);
        job_info_free(&j);
    }
    return EX_DONE;
}

/* -------------------------------------------------------------- team --- */

static void put_cwd(AgentctlMsg *m) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) agentctl_msg_put_str(m, AGK_CWD, cwd);
}

static int cmd_team(void) {
    if (!connect_team()) return print_team_file() ? EX_DONE : EX_USAGE;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_TEAM, 0)) return EX_USAGE;
    put_cwd(&m);
    if (!send_msg(&m)) return EX_USAGE;
    AgentctlFrame f; u8 *store = NULL;
    if (recv_reply(10000, &f, &store) != 1) return EX_USAGE;
    int rc = EX_DONE;
    if (reply_code(&f) != AGC_OK || !print_team_text(&f)) { print_text(stderr, &f); rc = EX_REFUSED; }
    free(store);
    return rc;
}

/* A post as one line of the job's outbox (LIU_AGENT_OUTBOX): "to<TAB>text",
 * the text on one line and cleaned, appended in one write. EX_DONE when it
 * was written. */
static int post_to_outbox(const char *to, const char *msg, usize len) {
    const char *path = getenv("LIU_AGENT_OUTBOX");
    if (!path || path[0] != '/') return EX_USAGE;
    char clean[AGT_POST_MAX];
    usize n = agentctl_sanitize(msg, len, clean, sizeof clean, false);
    for (usize i = 0; i < n; i++) if (clean[i] == '\t') clean[i] = ' ';
    if (n == 0) return EX_USAGE;
    char line[AGT_POST_MAX + 48];
    int w = snprintf(line, sizeof line, "%s\t%s\n", to, clean);
    if (w <= 0 || (usize)w >= sizeof line) return EX_USAGE;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return EX_USAGE;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size + w > (off_t)AGT_OUTBOX_CAP) {
        close(fd);
        return EX_USAGE;
    }
    ssize_t put = write(fd, line, (usize)w);
    close(fd);
    return put == w ? EX_DONE : EX_USAGE;
}

/* A `--to` value Liu can pass on: all, lead, a job number or a job's label —
 * one word of printable characters, which Liu resolves in the team. */
static bool post_to_ok(const char *to) {
    usize n = strlen(to);
    if (n == 0 || n > 63) return false;
    for (usize k = 0; k < n; k++)
        if ((unsigned char)to[k] <= ' ' || to[k] == 0x7F) return false;
    return true;
}

static int cmd_post(int argc, char **argv) {
    const char *to = "all";
    /* `--to` before or after the message words; after `--` every word is the
     * message's */
    char **words = calloc((usize)argc + 1, sizeof *words);
    if (!words) return EX_USAGE;
    int nw = 0;
    bool opts = true;
    for (int k = 0; k < argc; k++) {
        if (opts && !strcmp(argv[k], "--")) { opts = false; continue; }
        if (opts && !strcmp(argv[k], "--to") && k + 1 < argc) {
            to = argv[++k];
            if (!post_to_ok(to)) {
                free(words);
                fprintf(stderr, "liu-agent: --to takes all, lead, a job number or a job's label\n");
                return EX_USAGE;
            }
            continue;
        }
        words[nw++] = argv[k];
    }
    usize len = 0;
    char *msg = read_task(nw, words, 0, &len);
    free(words);
    if (!msg || len == 0) { free(msg); fprintf(stderr, "liu-agent: no message given\n"); return EX_USAGE; }
    if (len > AGT_POST_MAX - 1) {
        free(msg);
        fprintf(stderr, "liu-agent: a post holds at most 2 KB; put more in a file and post its path\n");
        return EX_USAGE;
    }
    if (!connect_team()) {
        /* out of the socket's reach (a sandbox): the job's outbox, which its
         * runner passes on to Liu */
        int rc = post_to_outbox(to, msg, len);
        free(msg);
        if (rc == EX_DONE) {
            printf("Posted to %s (through this job's outbox; Liu passes it on).\n",
                   !strcmp(to, "all") || !strcmp(to, "team") ? "the team" : to);
            return EX_DONE;
        }
        if (team_file()) { fprintf(stderr, "liu-agent: %s\n", NO_SOCKET_NOTE); return EX_REFUSED; }
        return EX_USAGE;
    }
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_POST, 0)) { free(msg); return EX_USAGE; }
    agentctl_msg_put_str(&m, AGK_TO, to);
    agentctl_msg_put(&m, AGK_TEXT, msg, (u32)len);
    free(msg);
    if (!send_msg(&m)) return EX_USAGE;
    AgentctlFrame f; u8 *store = NULL;
    if (recv_reply(10000, &f, &store) != 1) return EX_USAGE;
    u64 code = reply_code(&f);
    if (code != AGC_OK) { print_text(stderr, &f); free(store); return EX_REFUSED; }
    free(store);
    printf("Posted to %s.\n", !strcmp(to, "all") || !strcmp(to, "team") ? "the team" : to);
    return EX_DONE;
}

static int cmd_inbox(int argc, char **argv) {
    bool wait = false;
    i32 timeout_ms = -1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--wait")) wait = true;
        else if ((!strcmp(argv[i], "-t") || !strcmp(argv[i], "--timeout")) && i + 1 < argc) {
            if (!parse_secs(argv[++i], &timeout_ms)) { fprintf(stderr, "liu-agent: bad --timeout\n"); return EX_USAGE; }
        } else { fprintf(stderr, "liu-agent: unknown option %s\n", argv[i]); return EX_USAGE; }
    }
    if (!connect_team()) return print_team_file() ? EX_DONE : EX_USAGE;
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_INBOX, 0)) return EX_USAGE;
    if (wait) agentctl_msg_put_str(&m, AGK_BLOCK, "1");
    if (!send_msg(&m)) return EX_USAGE;
    AgentctlFrame f; u8 *store = NULL;
    int rc = recv_reply(wait ? timeout_ms : 10000, &f, &store);
    if (rc < 0) return EX_USAGE;
    if (rc == 0) { printf("No new messages yet. Run `liu-agent inbox --wait` again to keep waiting.\n"); return EX_WAITING; }
    int ex = EX_DONE;
    if (reply_code(&f) != AGC_OK || !print_team_text(&f)) { print_text(stderr, &f); ex = EX_REFUSED; }
    free(store);
    return ex;
}

static int cmd_owns(int argc, char **argv) {
    bool clear = argc == 1 && !strcmp(argv[0], "--clear");
    if (argc < 1) { fprintf(stderr, "liu-agent: owns which paths? (or --clear)\n"); return EX_USAGE; }
    if (!clear && argc > AGT_OWNS_MAX) { fprintf(stderr, "liu-agent: at most %d paths\n", AGT_OWNS_MAX); return EX_USAGE; }
    if (!connect_team()) {
        if (team_file()) { fprintf(stderr, "liu-agent: %s\n", NO_SOCKET_NOTE); return EX_REFUSED; }
        return EX_USAGE;
    }
    AgentctlMsg m;
    if (!agentctl_msg_init(&m, AGOP_OWNS, 0)) return EX_USAGE;
    put_cwd(&m);
    if (!clear) for (int i = 0; i < argc; i++) agentctl_msg_put_str(&m, AGK_PATH, argv[i]);
    if (!send_msg(&m)) return EX_USAGE;
    AgentctlFrame f; u8 *store = NULL;
    if (recv_reply(10000, &f, &store) != 1) return EX_USAGE;
    u64 code = reply_code(&f);
    print_text(code == AGC_OK ? stdout : stderr, &f);
    free(store);
    return code == AGC_OK ? EX_DONE : EX_REFUSED;
}

/* ------------------------------------------------------ the lead's tools --- */

/* The tool hooks of the agent in a pane (`liu-notify send --hook
 * --event=tool`, run on PreToolUse and PostToolUse with the agent's JSON on
 * stdin): when the pane leads a team — the marker Liu keeps beside its socket
 * says so, and a pane that does not pays for no connection — Liu hears which
 * tool runs when, its command and the files it names, so what the lead's
 * agent changes is the lead's. Always 0: a hook never fails its agent. */

#define TOOL_HOOK_MAX   (1u << 20)        /* read of the payload; the rest is drained */
#define TOOL_HOOK_PATHS 16

static void hook_path(AgentctlMsg *m, u32 *n, const char *p, usize len) {
    if (!p || !len || *n >= TOOL_HOOK_PATHS || len >= AGT_PATH_MAX || memchr(p, '\0', len)) return;
    agentctl_msg_put(m, AGK_PATH, p, (u32)len);
    (*n)++;
}

/* The files a patch in the `*** Begin Patch` form adds, updates, deletes or
 * moves to. */
static void hook_patch_paths(AgentctlMsg *m, u32 *n, const char *patch) {
    static const char *const heads[] = { "*** Add File: ", "*** Update File: ", "*** Delete File: ", "*** Move to: " };
    for (const char *line = patch; line && *line; ) {
        const char *end = strchr(line, '\n');
        usize len = end ? (usize)(end - line) : strlen(line);
        for (usize h = 0; h < sizeof heads / sizeof heads[0]; h++) {
            usize hl = strlen(heads[h]);
            if (len > hl && !strncmp(line, heads[h], hl)) {
                usize pl = len - hl;
                while (pl > 0 && (line[hl + pl - 1] == '\r' || line[hl + pl - 1] == ' ')) pl--;
                hook_path(m, n, line + hl, pl);
            }
        }
        line = end ? end + 1 : NULL;
    }
}

typedef struct { AgentctlMsg *m; u32 *n; } HookPaths;
static void hook_shell_path(void *ctx, const char *path) {
    HookPaths *hp = ctx;
    hook_path(hp->m, hp->n, path, strlen(path));
}

static void drain_stdin(void) {
    char buf[16384];
    for (;;) {
        ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return;
    }
}

/* What a hook prints for the agent that ran it, in Claude Code's hook
 * output form: a PreToolUse decision with its reason, or context added
 * after a tool. */
static void hook_print(const char *event, const char *decision, const char *reason, const char *context) {
    cJSON *root = cJSON_CreateObject(), *h = cJSON_CreateObject();
    if (!root || !h) { cJSON_Delete(root); cJSON_Delete(h); return; }
    cJSON_AddStringToObject(h, "hookEventName", event);
    if (decision) {
        cJSON_AddStringToObject(h, "permissionDecision", decision);
        cJSON_AddStringToObject(h, "permissionDecisionReason", reason ? reason : "");
    }
    if (context) cJSON_AddStringToObject(h, "additionalContext", context);
    cJSON_AddItemToObject(root, "hookSpecificOutput", h);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out) { fputs(out, stdout); fputc('\n', stdout); free(out); }
}

/* A tool hook's payload on stdin, read whole (bounded) and parsed. */
static cJSON *hook_read(void) {
    char *buf = malloc(TOOL_HOOK_MAX + 1);
    if (!buf) { drain_stdin(); return NULL; }
    usize off = 0;
    for (;;) {
        if (off >= TOOL_HOOK_MAX) { drain_stdin(); break; }
        ssize_t r = read(STDIN_FILENO, buf + off, TOOL_HOOK_MAX - off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        off += (usize)r;
    }
    buf[off] = '\0';
    cJSON *root = cJSON_ParseWithLength(buf, off);
    free(buf);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    return root;
}

/* The tool call of `root` as an AGOP_LEAD_TOOL frame: before or after, its
 * id, folder, command and the files it names. */
static bool hook_frame(const cJSON *root, bool pre, AgentctlMsg *m) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "tool_use_id");
    const cJSON *cwd = cJSON_GetObjectItemCaseSensitive(root, "cwd");
    const cJSON *in = cJSON_GetObjectItemCaseSensitive(root, "tool_input");
    if (!agentctl_msg_init(m, AGOP_LEAD_TOOL, 0)) return false;
    agentctl_msg_put_str(m, AGK_STATE, pre ? "1" : "0");
    agentctl_msg_put_str(m, AGK_TEXT, cJSON_IsString(id) ? id->valuestring : "");
    if (cJSON_IsString(cwd) && cwd->valuestring[0] == '/') agentctl_msg_put_str(m, AGK_CWD, cwd->valuestring);
    u32 npaths = 0;
    if (cJSON_IsObject(in)) {
        /* the command: one string, or argv words joined */
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(in, "command");
        char line[4096];
        line[0] = '\0';
        if (cJSON_IsString(cmd)) {
            snprintf(line, sizeof line, "%s", cmd->valuestring);
        } else if (cJSON_IsArray(cmd)) {
            usize w = 0;
            const cJSON *a;
            cJSON_ArrayForEach(a, cmd) {
                if (!cJSON_IsString(a) || w >= sizeof line - 1) continue;
                w += (usize)snprintf(line + w, sizeof line - w, "%s%s", w ? " " : "", a->valuestring);
            }
            if (w >= sizeof line) w = sizeof line - 1;
        }
        if (line[0]) {
            agentctl_msg_put(m, AGK_PROMPT, line, (u32)strnlen(line, sizeof line));
            HookPaths hp = { m, &npaths };
            agentctl_shell_writes(line, "", hook_shell_path, &hp);
        }
        static const char *const keys[] = { "file_path", "notebook_path", "path" };
        for (usize k = 0; k < sizeof keys / sizeof keys[0]; k++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(in, keys[k]);
            if (cJSON_IsString(v)) hook_path(m, &npaths, v->valuestring, strlen(v->valuestring));
        }
        /* a patch, wherever the tool keeps it */
        const cJSON *v;
        cJSON_ArrayForEach(v, in) {
            if (cJSON_IsString(v) && strstr(v->valuestring, "*** Begin Patch")) hook_patch_paths(m, &npaths, v->valuestring);
        }
        if (cJSON_IsArray(cmd)) {
            const cJSON *a;
            cJSON_ArrayForEach(a, cmd)
                if (cJSON_IsString(a) && strstr(a->valuestring, "*** Begin Patch")) hook_patch_paths(m, &npaths, a->valuestring);
        }
    }
    return agentctl_msg_end(m);
}

/* Sends the frame and takes Liu's answer: its decision ("" for none), why,
 * and the team's news for the agent ("" for none). */
static void hook_exchange(const char *sock, AgentctlMsg *m, char *decision, usize dcap, char *why, usize wcap,
                          char **news) {
    decision[0] = why[0] = '\0';
    *news = NULL;
    int fd = notify_socket_connect_timeout(sock, 300);
    if (fd < 0) return;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    /* Liu answers once it has taken the call in, so a tool about to run
     * is on record before it writes anything */
    if (agentctl_write_all(fd, m->buf, m->len)) {
        AgentctlFrame f;
        u8 *store = NULL;
        if (agentctl_read_frame(fd, 300, &f, &store) == 1) {
            agentctl_get_str(&f, AGK_DECISION, decision, dcap);
            if (decision[0]) {
                char raw[1024];
                if (agentctl_get_str(&f, AGK_TEXT, raw, sizeof raw)) agentctl_sanitize(raw, strlen(raw), why, wcap, false);
            }
            const u8 *d; u32 n;
            if (agentctl_get(&f, AGK_TEAM, &d, &n) && n > 0 && (*news = malloc((usize)n * 3 + 1)))
                agentctl_sanitize((const char *)d, n, *news, (usize)n * 3 + 1, true);
        }
        free(store);
    }
    close(fd);
}

int agentctl_tool_hook(const char *tool) {
    const char *sock = getenv("LIU_AGENT_SOCK"), *pane = getenv("LIU_PANE_ID");
    const char *dot = pane ? strchr(pane, '.') : NULL;
    char marker[1100];
    if (isatty(STDIN_FILENO)) return 0;
    if (!sock || sock[0] != '/' || !dot || !dot[1] ||
        (usize)snprintf(marker, sizeof marker, "%s.lead-%s", sock, dot + 1) >= sizeof marker ||
        access(marker, F_OK) != 0) {
        drain_stdin();                                  /* the agent's write must not meet a closed pipe */
        return 0;
    }
    cJSON *root = hook_read();
    if (!root) return 0;
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(root, "hook_event_name");
    bool pre = cJSON_IsString(ev) && !strcmp(ev->valuestring, "PreToolUse");
    bool post = cJSON_IsString(ev) && !strcmp(ev->valuestring, "PostToolUse");
    if (!pre && !post) { cJSON_Delete(root); return 0; }
    AgentctlMsg m;
    bool built = hook_frame(root, pre, &m);
    cJSON_Delete(root);
    if (!built) { agentctl_msg_free(&m); return 0; }
    char decision[16], why[1024];
    char *news = NULL;
    hook_exchange(sock, &m, decision, sizeof decision, why, sizeof why, &news);
    agentctl_msg_free(&m);
    /* only Claude Code reads this form back: another agent's hook prints nothing */
    bool speaks = tool && !strcmp(tool, "claude");
    if (speaks && pre && decision[0]) hook_print("PreToolUse", decision, why, NULL);
    if (speaks && post && news) hook_print("PostToolUse", NULL, NULL, news);
    free(news);
    return 0;
}

/* `liu-agent guard`: a delegated Claude's PreToolUse hook (its runner puts
 * it in the agent's --settings). Liu refuses a write into a path another
 * member of its team owns, and the agent reads why. Always exits 0. */
int agentctl_guard_hook(void) {
    const char *sock = getenv("LIU_AGENT_SOCK");
    if (isatty(STDIN_FILENO)) return 0;
    if (!sock || sock[0] != '/') { drain_stdin(); return 0; }
    cJSON *root = hook_read();
    if (!root) return 0;
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(root, "hook_event_name");
    if (!cJSON_IsString(ev) || strcmp(ev->valuestring, "PreToolUse")) { cJSON_Delete(root); return 0; }
    AgentctlMsg m;
    bool built = hook_frame(root, true, &m);
    cJSON_Delete(root);
    if (!built) { agentctl_msg_free(&m); return 0; }
    char decision[16], why[1024];
    char *news = NULL;
    hook_exchange(sock, &m, decision, sizeof decision, why, sizeof why, &news);
    agentctl_msg_free(&m);
    free(news);
    if (!strcmp(decision, "deny")) hook_print("PreToolUse", "deny", why, NULL);
    return 0;
}

int agentctl_client_main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? EX_USAGE : EX_DONE;
    }
    const char *cmd = argv[1];
    int rest = argc - 2;
    char **rv = argv + 2;
    int rc;
    if (!strcmp(cmd, "run"))          rc = agentctl_runner_main();
    else if (!strcmp(cmd, "spawn"))   rc = cmd_spawn(rest, rv);
    else if (!strcmp(cmd, "wait"))    rc = cmd_wait(rest, rv);
    else if (!strcmp(cmd, "send"))    rc = cmd_send(rest, rv);
    else if (!strcmp(cmd, "status"))  rc = cmd_status(rest, rv);
    else if (!strcmp(cmd, "list"))    rc = cmd_list();
    else if (!strcmp(cmd, "result"))  rc = cmd_result(rest, rv, false);
    else if (!strcmp(cmd, "log"))     rc = cmd_result(rest, rv, true);
    else if (!strcmp(cmd, "stop"))    rc = cmd_simple(AGOP_STOP, rest, rv, "asked to stop its turn.");
    else if (!strcmp(cmd, "close"))   rc = cmd_simple(AGOP_CLOSE, rest, rv, "closed.");
    else if (!strcmp(cmd, "focus"))   rc = cmd_simple(AGOP_FOCUS, rest, rv, "its log is open in Liu.");
    else if (!strcmp(cmd, "agents"))  rc = cmd_agents();
    else if (!strcmp(cmd, "models"))  rc = cmd_models(rest, rv);
    else if (!strcmp(cmd, "roles"))   rc = cmd_roles();
    else if (!strcmp(cmd, "review"))  rc = cmd_review(rest, rv);
    else if (!strcmp(cmd, "compare")) rc = cmd_compare(rest, rv);
    else if (!strcmp(cmd, "merge"))   rc = cmd_merge(rest, rv, true);
    else if (!strcmp(cmd, "discard")) rc = cmd_merge(rest, rv, false);
    else if (!strcmp(cmd, "guard"))   rc = agentctl_guard_hook();
    else if (!strcmp(cmd, "team"))    rc = cmd_team();
    else if (!strcmp(cmd, "post"))    rc = cmd_post(rest, rv);
    else if (!strcmp(cmd, "inbox"))   rc = cmd_inbox(rest, rv);
    else if (!strcmp(cmd, "owns"))    rc = cmd_owns(rest, rv);
    else { fprintf(stderr, "liu-agent: unknown command '%s'\n\n", cmd); usage(stderr); rc = EX_USAGE; }
    if (g_fd >= 0) close(g_fd);
    fflush(stdout);
    return rc;
}
