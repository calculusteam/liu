/*
 * Liu — lightweight git status sampler for the bottom status bar.
 *
 * The sample follows the FOCUSED PANE of the focused tab, not pane 0. A split
 * tab is routinely two different repos (or two different subtrees of one), and
 * pane 0 may be a shell the user is not typing into. Everything below — the
 * cwd probe, the staleness check, the worker's captured cwd — reads the active
 * pane's terminal/session.
 *
 * Local sessions only — SSH/Mosh panes skip, since their cwd refers to a
 * remote path. For the sampled pane the sampler periodically runs:
 *
 *   git -C <cwd> rev-parse --is-bare-repository --show-toplevel
 *   git -C <cwd> symbolic-ref --quiet --short HEAD
 *   git -C <cwd> diff --numstat HEAD    (combined tracked changes)
 *   git -C <root> ls-files --others --exclude-standard -z
 *   git -C <cwd> rev-list --count @{u}..HEAD
 *
 * Unborn/detached repositories use the corresponding fallback queries.
 * A completed result is reusable by another local tab in the same cwd for
 * the existing two-second sampling interval; reuse never renews its age.
 *
 * Results feed `tab->git_status`, which the status bar renderer reads to
 * paint "+N -M ↑K" after the cwd label.
 *
 * Implementation detail: git is spawned/exec'd directly (no shell) to avoid
 * any risk of argv injection from a terminal-supplied cwd (OSC 7). The
 * child's stderr is sent to /dev/null; stdout is piped back. A single
 * call suite takes ~20-100 ms on a typical repo, so the focused tab is
 * sampled on a schedule — ui/git_poll.h decides when the next one is due.
 */
#include "ui/ui.h"
#include "core/checked.h"
#include "ui/git_exec.h"
#include "ui/git_poll.h"
#include <signal.h>
#include <poll.h>
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#ifndef PLATFORM_WIN32
#include <sys/wait.h>
#endif
#include <sys/stat.h>
#include <errno.h>
#ifdef PLATFORM_MACOS
#include <spawn.h>
#endif


extern char **environ;

/* The environment Liu's own git runs get: the user's, plus settings a
 * repository's config is not asked about. Liu runs git by itself, in whatever
 * folder a pane is in — a folder the user may only have `cd`ed into, or one
 * another program chose for them (the "New Liu Tab Here" service) — and
 * `core.fsmonitor` in that folder's config names a PROGRAM git runs on the
 * first status query. So: no fsmonitor, no hooks, and no optional locks (a
 * status bar must not take the index lock from the user's own git).
 * GIT_CONFIG_COUNT / KEY / VALUE outrank every config file.
 *
 * Built for EACH run, as one block that holds copies of the strings: pointers
 * into `environ` kept from one run would go stale — git would keep the
 * environment of the first run (from before the login shell's PATH was merged
 * in), and a variable Liu sets twice would leave a pointer to a string
 * `setenv` has already released. The caller frees it. */
static char **git_environ_dup(void) {
    static const char *const extra[] = {
        "GIT_OPTIONAL_LOCKS=0",
        "GIT_CONFIG_COUNT=2",
        "GIT_CONFIG_KEY_0=core.fsmonitor", "GIT_CONFIG_VALUE_0=false",
        "GIT_CONFIG_KEY_1=core.hooksPath", "GIT_CONFIG_VALUE_1=/dev/null",
    };
    enum { EXTRA = (int)(sizeof extra / sizeof extra[0]) };
    usize n = 0, bytes = 0;
    while (environ && environ[n]) { bytes += strlen(environ[n]) + 1; n++; }
    for (int i = 0; i < EXTRA; i++) bytes += strlen(extra[i]) + 1;
    usize vec = (n + EXTRA + 1) * sizeof(char *);
    char **env = malloc(vec + bytes);
    if (!env) return NULL;                          /* the caller falls back to environ */
    char *str = (char *)env + vec;
    usize w = 0;
    for (usize i = 0; i < n; i++) {
        /* ours win: drop what the inherited environment says for the same names */
        if (strncmp(environ[i], "GIT_CONFIG_COUNT=", 17) == 0 ||
            strncmp(environ[i], "GIT_CONFIG_KEY_", 15) == 0 ||
            strncmp(environ[i], "GIT_CONFIG_VALUE_", 17) == 0 ||
            strncmp(environ[i], "GIT_OPTIONAL_LOCKS=", 19) == 0) continue;
        usize len = strlen(environ[i]) + 1;
        memcpy(str, environ[i], len);
        env[w++] = str;
        str += len;
    }
    for (int i = 0; i < EXTRA; i++) {
        usize len = strlen(extra[i]) + 1;
        memcpy(str, extra[i], len);
        env[w++] = str;
        str += len;
    }
    env[w] = NULL;
    return env;
}

/* One finished run: what the worker publishes into the result table, keyed
 * by the cwd it sampled (see "Scheduling" below). */
typedef struct {
    char       cwd[1024];
    char       root[1024];          /* worktree root, empty when unavailable */
    bool       is_repo;
    i32        added, removed, ahead;
    bool       partial;             /* added/removed are a lower bound: an answer outgrew the capture */
    char       branch[80];          /* branch, or "@<short-sha>" when detached */
    bool       valid;
    f64        cost;                /* seconds the run took (ui/git_poll.h) */
} GitStatusResult;

/* Untracked scan budget. A file git has never seen counts every one of its
 * lines as an addition (that is what makes the total match what the user sees
 * in a diff), so the scan has to open them — and a repo can always sprout a
 * directory of generated files nobody thought to ignore. These bound the walk
 * to something that stays inside the worker's 2 s window. */
#define GIT_UNTRACKED_MAX_FILES  2000
#define GIT_UNTRACKED_MAX_BYTES  (64u * 1024u * 1024u)

/* stdout capture for one git call. `git status --porcelain` and
 * `ls-files --others` are proportional to the size of the change, not a fixed
 * few lines, so a small buffer would silently truncate both on any sizeable
 * branch — run_git discards the overflow and the counts would come up short
 * with nothing to indicate it. Heap, and big enough for a large refactor.
 *
 * It is still a cap, and it lands wherever the byte count says — mid-path,
 * mid-line. ~1800 files with 600-byte paths reach it well inside the 2000-file
 * quota. git_capture_len() reports the cut and callers count whole records
 * only, then mark the result partial (GitStatusResult.partial). */
#define GIT_OUT_CAP (1u << 20)

/* Scheduling. One worker serves every window. A request is a cwd in a FIFO
 * (deduplicated: a cwd already queued or running is not queued twice, so N
 * windows in one repo cost one run), the worker drains it in arrival order,
 * and each finished run lands in a small table keyed by cwd that every tab
 * reads from. Nothing a window fails to collect blocks anybody: results age
 * out of the table on their own, so a result whose window has cd'd away
 * neither holds the worker nor blanks another window's git segment. */
#define GIT_QUEUE_CAP    8
#define GIT_RESULTS_CAP  8
/* A result older than this is not shown to a tab arriving in that cwd — it
 * would paint figures from a tree that has had time to change. */
#define GIT_RESULT_KEEP_SEC 10.0

typedef struct {
    GitStatusResult r;
    f64             ts;          /* publish time; 0 = empty slot */
} GitResultSlot;

static pthread_mutex_t  g_worker_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool             g_worker_live = false;   /* a (detached) worker runs */
static char             g_queue[GIT_QUEUE_CAP][1024];
static i32              g_queue_head = 0, g_queue_count = 0;
static char             g_running[1024];         /* "" when idle */
static GitResultSlot    g_results[GIT_RESULTS_CAP];
static u64              g_runs_started = 0;      /* Git run count, not request count. */

static bool git_pipe_cloexec(int p[2]) {
    if (!p || pipe(p) != 0) return false;
    for (i32 i = 0; i < 2; i++) {
        if (p[i] < 3) {
            int moved = fcntl(p[i], F_DUPFD_CLOEXEC, 3);
            if (moved < 0) {
                close(p[0]); close(p[1]);
                p[0] = p[1] = -1;
                return false;
            }
            close(p[i]);
            p[i] = moved;
        } else {
            int flags = fcntl(p[i], F_GETFD, 0);
            if (flags < 0 || fcntl(p[i], F_SETFD, flags | FD_CLOEXEC) != 0) {
                close(p[0]); close(p[1]);
                p[0] = p[1] = -1;
                return false;
            }
        }
    }
    return true;
}

/* Run `git <argv...>` with working directory `cwd`. Captures up to
 * `out_size` bytes of stdout into `out`. Returns the process exit
 * status (0 = success) or -1 on process / pipe setup error.
 *
 * Shared with the diff view (see ui/git_exec.h) — deliberately one
 * implementation, because the cwd is terminal-supplied and the no-shell
 * posture has to hold for every caller. */
void git_cancel_request(GitCancel *c) {
    if (!c) return;
    atomic_store_explicit(&c->cancel, true, memory_order_release);
    int pgid = atomic_load_explicit(&c->child_pgid, memory_order_acquire);
    /* The whole group, not the one pid: git forks its own helpers, and a
     * SIGTERM to the leader alone can leave those behind. */
    if (pgid > 0) killpg((pid_t)pgid, SIGTERM);
}

/* Reap `pid`, escalating to SIGKILL if it ignores the SIGTERM it was already
 * sent. Bounded: at most GIT_CANCEL_GRACE_MS of polling, on the worker
 * thread, never on the caller that asked for the cancel. */
#define GIT_CANCEL_GRACE_MS 100
static void git_reap_cancelled(pid_t pid) {
    int status = 0;
    for (i32 waited_ms = 0; waited_ms < GIT_CANCEL_GRACE_MS; waited_ms += 5) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { killpg(pid, SIGKILL); return; }
        if (r < 0 && errno != EINTR) return;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    killpg(pid, SIGKILL);        /* pid is its own group leader */
    pid_t r;
    do { r = waitpid(pid, &status, 0); } while (r < 0 && errno == EINTR);
}

/* Where a run's stdout goes. Called from the capturing thread with each read;
 * returns false once it wants no more (the rest is read and DISCARDED so git
 * never blocks on a full pipe — the same rule as the fixed capture). One read
 * loop serves the fixed buffer and the growing one, so the spawn, the cancel
 * handshake and the reap cannot drift between them. */
typedef bool (*GitSink)(void *ctx, const char *bytes, usize n);

static int git_run_sink(const char *cwd, const char *const *argv,
                        GitSink sink, void *ctx, GitCancel *c) {
    if (!cwd || !argv) return -1;
    if (c && atomic_load_explicit(&c->cancel, memory_order_acquire)) return -1;

    int pipefd[2] = {-1, -1};
    if (!git_pipe_cloexec(pipefd)) return -1;

    pid_t pid;
#ifdef PLATFORM_MACOS
    const char *legacy=getenv("LIU_GIT_FORK");
    if(!legacy || legacy[0]!='1') {
        /* Spawn avoids copying the renderer's address space and running the
         * framework atfork handlers for each read-only status query. */
        posix_spawn_file_actions_t actions;posix_spawnattr_t attr;
        bool have_actions=false,have_attr=false;
        int rc=posix_spawn_file_actions_init(&actions);
        if(!rc){have_actions=true;rc=posix_spawnattr_init(&attr);}
        if(!rc){have_attr=true;rc=posix_spawnattr_setpgroup(&attr,0);}
        if(!rc)rc=posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);
        if(!rc)rc=posix_spawn_file_actions_addclose(&actions,pipefd[0]);
        if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDOUT_FILENO);
        if(!rc)rc=posix_spawn_file_actions_addclose(&actions,pipefd[1]);
        if(!rc)rc=posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
        if(!rc)rc=posix_spawn_file_actions_addchdir_np(&actions,cwd);
        char **spawn_env = rc ? NULL : git_environ_dup();
        if(!rc)rc=posix_spawn(&pid,"/usr/bin/git",&actions,&attr,(char *const *)argv,spawn_env ? spawn_env : environ);
        free(spawn_env);
        if(have_attr)posix_spawnattr_destroy(&attr);
        if(have_actions)posix_spawn_file_actions_destroy(&actions);
        if(rc){close(pipefd[0]);close(pipefd[1]);errno=rc;return -1;}
    } else
#endif
    {
    char **child_env = git_environ_dup();  /* before the fork: not async-signal-safe */
    pid = fork();
    if (pid < 0) {
        free(child_env);
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect stdout to pipe, stderr to /dev/null, chdir,
         * exec git. No shell — argv is passed through. */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull < 0) _exit(127);
        if (devnull != STDERR_FILENO) {
            if (dup2(devnull, STDERR_FILENO) < 0) _exit(127);
            close(devnull);
        }

        /* Its own process group, so a cancel can signal git AND whatever git
         * forked with one killpg, and nothing is left orphaned. Failure is
         * not fatal — the run still works, only the cancel is coarser. */
        (void)setpgid(0, 0);
        if (chdir(cwd) != 0) _exit(127);
        execve("/usr/bin/git", (char *const *)argv, child_env ? child_env : environ);
        _exit(127);
    }
    free(child_env);                       /* the child has its own copy */
    }

    /* Also from the parent: setpgid is racy from the child alone, and the
     * cancel below reads this the moment it is published. */
    (void)setpgid(pid, pid);
    if (c) {
        atomic_store_explicit(&c->child_pgid, (int)pid, memory_order_release);
        /* A cancel that landed between the check above and here would have
         * found child_pgid == 0 and signalled nothing. */
        if (atomic_load_explicit(&c->cancel, memory_order_acquire))
            killpg(pid, SIGTERM);
    }

    close(pipefd[1]);
    bool cancelled = false, io_failed = false;
    char chunk[16384]; bool want = true;
    for (;;) {
        if (c && atomic_load_explicit(&c->cancel, memory_order_acquire)) { cancelled = true; break; }
        struct pollfd fd = { .fd = pipefd[0], .events = POLLIN };
        int ready = poll(&fd, 1, c ? 10 : -1);
        if (ready < 0) { if (errno == EINTR) continue; io_failed = true; break; }
        if (!ready) continue;
        ssize_t n = read(pipefd[0], chunk, sizeof chunk);
        if (n < 0) { if (errno == EINTR) continue; io_failed = true; break; }
        if (!n) break;
        if (want) want = sink(ctx, chunk, (usize)n);
    }
    close(pipefd[0]);
    int status = 0;
    for (;;) {
        cancelled = cancelled || (c && atomic_load_explicit(&c->cancel, memory_order_acquire));
        if (cancelled || io_failed) {
            killpg(pid, SIGTERM);
            git_reap_cancelled(pid);
            break;
        }
        pid_t waited = waitpid(pid, &status, c ? WNOHANG : 0);
        if (waited == pid) break;
        if (waited < 0) {
            if (errno == EINTR) continue;
            if (c) atomic_store_explicit(&c->child_pgid, 0, memory_order_release);
            return -1;
        }
        struct timespec tick = {0, 1000000}; nanosleep(&tick, NULL);
    }
    if (c) atomic_store_explicit(&c->child_pgid, 0, memory_order_release);
    if (cancelled || io_failed) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* The fixed-size capture: fill `out` up to out_size-1, then stop taking.
 * `truncated` is set only when a byte arrived that did not fit — an answer of
 * exactly the buffer's size is whole, and the caller must be able to tell. */
typedef struct { char *out; usize size, total; bool truncated; } GitFixedSink;
static bool git_sink_fixed(void *ctx, const char *bytes, usize n) {
    GitFixedSink *f = (GitFixedSink *)ctx;
    usize room = f->size - 1 - f->total;
    if (n > room) { n = room; f->truncated = true; }
    memcpy(f->out + f->total, bytes, n);
    f->total += n;
    f->out[f->total] = '\0';
    return !f->truncated;
}

int git_run_capture_cancellable(const char *cwd, const char *const *argv,
                                char *out, usize out_size, GitCancel *c) {
    if (!cwd || !argv || !out || out_size == 0) return -1;
    out[0] = '\0';
    GitFixedSink f = { out, out_size, 0, false };
    return git_run_sink(cwd, argv, git_sink_fixed, &f, c);
}

/* The same capture, and it says how much it took and whether git had more.
 *
 * `*len` is the byte count, because a `-z` listing is full of NULs and
 * strlen() is only its first record. `*truncated` means the last record may be
 * CUT — half a path, half a numstat line — and is not a record at all. Callers
 * walk [out, out+len) and stop at the last whole record. */
static int git_capture_len(const char *cwd, const char *const *argv,
                           char *out, usize out_size,
                           usize *len, bool *truncated) {
    *len = 0;
    *truncated = false;
    if (!cwd || !argv || !out || out_size == 0) return -1;
    out[0] = '\0';
    GitFixedSink f = { out, out_size, 0, false };
    int rc = git_run_sink(cwd, argv, git_sink_fixed, &f, NULL);
    *len = f.total;
    *truncated = f.truncated;
    return rc;
}

/* Cut a truncated capture back to its last whole record: whatever follows the
 * final `sep` is a record git never finished. Returns the new length. */
static usize git_whole_records(char *buf, usize len, char sep) {
    while (len > 0 && buf[len - 1] != sep) len--;
    buf[len] = '\0';
    return len;
}

int git_run_capture(const char *cwd, const char *const *argv,
                    char *out, usize out_size) {
    return git_run_capture_cancellable(cwd, argv, out, out_size, NULL);
}

/* ---- the growing capture --------------------------------------------- */

/* First allocation. Small enough that the common case — a status-bar-sized
 * diff, an empty untracked list — costs a page, not the 8 MiB the dock used
 * to reserve for every run. */
#define GIT_GROW_MIN_CAP 4096u

static void git_grow_charge(GitGrowBuf *g, usize old_cap, usize new_cap) {
    if (g->charge && old_cap != new_cap)
        g->charge(g->charge_ctx, (isize)new_cap - (isize)old_cap);
}

bool git_grow_reserve(GitGrowBuf *g, usize need) {
    if (!g) return false;
    usize want = g->len + need + 1;
    if (want < g->len) return false;                    /* overflow */
    if (g->data && want <= g->cap) return true;
    usize cap = g->cap ? g->cap : GIT_GROW_MIN_CAP;
    while (cap < want) {
        if (cap > ((usize)-1) / 2) { cap = want; break; }
        cap *= 2;
    }
    char *d = realloc(g->data, cap);
    if (!d) return false;
    if (!g->data) d[0] = '\0';
    usize old = g->cap;
    g->data = d;
    g->cap  = cap;
    git_grow_charge(g, old, cap);
    return true;
}

void git_grow_trim(GitGrowBuf *g) {
    if (!g || !g->data) return;
    usize want = g->len + 1;
    if (want >= g->cap) return;
    char *d = realloc(g->data, want);
    if (!d) return;                                     /* keep the larger one */
    usize old = g->cap;
    g->data = d;
    g->cap  = want;
    git_grow_charge(g, old, want);
}

void git_grow_free(GitGrowBuf *g) {
    if (!g) return;
    usize old = g->cap;
    free(g->data);
    g->data = NULL;
    g->len  = 0;
    g->cap  = 0;
    git_grow_charge(g, old, 0);
}

typedef struct {
    GitGrowBuf *g;
    usize       limit;     /* this run's own byte allowance */
    usize       taken;     /* this run's bytes appended so far */
    bool        oom;
} GitGrowSink;

static bool git_sink_grow(void *ctx, const char *bytes, usize n) {
    GitGrowSink *s = (GitGrowSink *)ctx;
    usize room = s->limit - s->taken;
    if (n > room) n = room;
    if (n == 0) return false;
    if (!git_grow_reserve(s->g, n)) { s->oom = true; return false; }
    memcpy(s->g->data + s->g->len, bytes, n);
    s->g->len += n;
    s->g->data[s->g->len] = '\0';
    s->taken += n;
    return s->taken < s->limit;
}

int git_run_capture_grow(const char *cwd, const char *const *argv,
                         GitGrowBuf *g, usize limit, GitCancel *c) {
    if (!cwd || !argv || !g) return -1;
    GitGrowSink s = { g, limit, 0, false };
    int rc = git_run_sink(cwd, argv, git_sink_grow, &s, c);
    if (s.oom) return -2;
    return rc;
}

static void parse_numstat(const char *buf, i32 *added, i32 *removed) {
    if (!buf || !added || !removed) return;
    const char *p = buf;
    while (*p) {
        /* Each line: "<added>\t<removed>\t<path>\n". Binary files may
         * report "-\t-\t…" — those are skipped. */
        i32 a = 0, r = 0;
        const char *start = p;
        if (*p == '-') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        const char *end = NULL;
        if (!checked_parse_i32_prefix(p, &a, &end) || a < 0 || *end != '\t') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        p = end + 1; /* tab */
        if (*p == '-') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        if (!checked_parse_i32_prefix(p, &r, &end) || r < 0 ||
            (*end != '\t' && *end != '\n' && *end != '\0')) {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        p = end;
        *added = a > INT32_MAX - *added ? INT32_MAX : *added + a;
        *removed = r > INT32_MAX - *removed ? INT32_MAX : *removed + r;
        /* Skip to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (p == start) break; /* defensive: avoid infinite loop */
    }
}

/* The pane the bar describes: whichever one has focus. A pane slot that holds
 * no terminal (a browser pane in a split) falls back to pane 0 rather than
 * blanking the bar — there is still a shell in the tab to report on. */
static Terminal *focus_terminal(const Tab *tab) {
    if (!tab) return NULL;
    Terminal *t = app_pane_terminal(tab, tab->active_pane);
    return t ? t : tab->terminal;
}

static Session *focus_session(const Tab *tab) {
    if (!tab) return NULL;
    Session *s = app_pane_session(tab, tab->active_pane);
    return s ? s : tab->session;
}

static bool is_local_session(const Tab *tab) {
    Session *s = focus_session(tab);
    if (!s) return false;
    return session_type(s) == SESSION_LOCAL;
}

/* Replace a heap-owned string field in place.
 *
 * `repo_root` and `probe_cwd` are heap pointers rather than inline buffers in
 * every Tab, so a tab that never sits in a worktree holds nothing for them.
 * They are on the same terms as the neighbouring `cached_cwd`: NULL means "not
 * known yet", and every reader must test for it. On OOM the slot stays NULL,
 * which readers already treat as "no repo" — the badge just misses a frame. */
static void gs_set_str(char **slot, const char *s) {
    free(*slot);
    *slot = NULL;
    if (!s) return;
    usize n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) { memcpy(p, s, n + 1); *slot = p; }
}

/* Strip the trailing newline git puts after every single-value answer. */
static void chomp(char *s) {
    usize n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

/* Newlines in one untracked file, decrementing the shared byte budget.
 *
 * Only regular files are opened: `ls-files --others` can name a FIFO, and
 * opening one without O_NONBLOCK parks the worker thread until someone writes
 * to it — the git badge would simply stop updating, with no error anywhere. */
static i32 count_file_lines(const char *cwd, const char *rel, u32 *budget) {
    char path[2048];
    if (rel[0] == '/') snprintf(path, sizeof path, "%s", rel);
    else               snprintf(path, sizeof path, "%s/%s", cwd, rel);

    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if ((u64)st.st_size > (u64)*budget) { *budget = 0; return 0; }

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;

    char buf[65536];
    i32 lines = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        if ((u32)n >= *budget) { *budget = 0; break; }
        *budget -= (u32)n;
        for (ssize_t i = 0; i < n; i++) if (buf[i] == '\n') lines++;
    }
    close(fd);
    return lines;
}

/* One `diff --numstat` call, added into `r`. Output that outgrew the capture is
 * cut back to its last whole line first — the line git was still writing would
 * otherwise be counted with whatever digits arrived — and `r` is marked
 * partial. False when git failed, so the caller can try its fallback. */
static bool numstat_into(const char *cwd, const char *const *argv, char *out,
                         GitStatusResult *r) {
    usize len = 0;
    bool  cut = false;
    if (git_capture_len(cwd, argv, out, GIT_OUT_CAP, &len, &cut) != 0) return false;
    if (cut) {
        git_whole_records(out, len, '\n');
        r->partial = true;
    }
    parse_numstat(out, &r->added, &r->removed);
    return true;
}

static void compute_status(const char *cwd, GitStatusResult *r) {
    memset(r, 0, sizeof *r);
    snprintf(r->cwd, sizeof r->cwd, "%s", cwd);
    r->valid = true;

    char *out = malloc(GIT_OUT_CAP);
    if (!out) return;

    /* A fixed boolean prefix proves repo membership even when show-toplevel
     * fails in a bare repo or .git directory. Root paths may contain newlines,
     * so do not split two variable path answers at an arbitrary newline. */
    const char *argv_top[]={"git","-C",cwd,"rev-parse","--is-bare-repository","--show-toplevel",NULL};
    int rc=git_run_capture(cwd,argv_top,out,GIT_OUT_CAP);
    usize prefix=strncmp(out,"false\n",6)==0?6:strncmp(out,"true\n",5)==0?5:0;
    r->is_repo=prefix!=0;
    if(rc==0 && prefix && out[prefix]) {
        char *root=out+prefix;usize n=strlen(root);
        if(n && root[n-1]=='\n')root[n-1]='\0';
        snprintf(r->root,sizeof(r->root),"%s",root);
    }
    if(!r->is_repo){free(out);return;}

    /* Branch. symbolic-ref fails on a detached HEAD, which is exactly how
     * the two are told apart — there the short SHA stands in, marked with
     * '@' so it can never be mistaken for a branch actually named like a
     * hash. */
    const char *argv_br[] = {"git", "-C", cwd, "symbolic-ref", "--quiet",
                             "--short", "HEAD", NULL};
    if (git_run_capture(cwd, argv_br, out, GIT_OUT_CAP) == 0 && out[0]) {
        chomp(out);
        snprintf(r->branch, sizeof r->branch, "%s", out);
    } else {
        const char *argv_sha[] = {"git", "-C", cwd, "rev-parse", "--short",
                                  "HEAD", NULL};
        if (git_run_capture(cwd, argv_sha, out, GIT_OUT_CAP) == 0 && out[0]) {
            chomp(out);
            snprintf(r->branch, sizeof r->branch, "@%s", out);
        }
    }

    /* Tracked changes measured against HEAD, so staged and unstaged edits to
     * one file count once rather than twice. (Summing `diff` and `diff
     * --cached` double-counts a hunk that is staged and then edited again.)
     * A repo with no commits yet has no HEAD to diff against; there the index
     * is the only reference available. */
    const char *argv_h[] = {"git", "-C", cwd, "diff", "--no-ext-diff", "--no-textconv", "--numstat", "HEAD", NULL};
    if (numstat_into(cwd, argv_h, out, r)) {
        /* counted */
    } else {
        const char *argv2[] = {"git", "-C", cwd, "diff", "--no-ext-diff", "--no-textconv", "--numstat", NULL};
        (void)numstat_into(cwd, argv2, out, r);
        const char *argv3[] = {"git", "-C", cwd, "diff", "--no-ext-diff", "--no-textconv",
                               "--cached", "--numstat", NULL};
        (void)numstat_into(cwd, argv3, out, r);
    }

    /* Untracked files are additions too. `git diff` cannot see them — a brand
     * new file is invisible to it until it is added — so a branch whose work is
     * mostly new files would report almost nothing. Counting their lines is
     * what makes the total agree with what the diff actually contains.
     *
     * Listed from the WORKTREE ROOT, not the cwd: `ls-files --others` is scoped
     * to the directory it runs in, while every other figure here is repo-wide,
     * so scanning from the cwd would make the badge's "+N" shrink as the user
     * cds deeper into the same project — and disagree with the diff dock, which
     * renders the whole worktree. */
    const char *scan = r->root[0] ? r->root : cwd;
    const char *argv_un[] = {"git", "-C", scan, "ls-files", "--others",
                             "--exclude-standard", "-z", NULL};
    usize un_len = 0;
    bool  un_cut = false;
    if (git_capture_len(scan, argv_un, out, GIT_OUT_CAP, &un_len, &un_cut) == 0) {
        u32 budget = GIT_UNTRACKED_MAX_BYTES;
        i32 files  = 0;
        /* -z: a record is a name and its NUL, never quoted. Walked by LENGTH:
         * the capture ends where git's answer was cut, and a name without its
         * NUL is half a path, not a file — stepping past it by strlen()+1
         * would read one byte beyond the allocation. */
        const char *p = out, *end = out + un_len;
        while (p < end && files < GIT_UNTRACKED_MAX_FILES) {
            const char *nul = memchr(p, '\0', (usize)(end - p));
            if (!nul) { r->partial = true; break; }
            if (nul > p) {
                i32 n = count_file_lines(scan, p, &budget);
                r->added = n > INT32_MAX - r->added ? INT32_MAX : r->added + n;
                files++;
            }
            p = nul + 1;
            if (budget == 0) break;
        }
        if (un_cut) r->partial = true;
    }

    const char *argv4[] = {"git", "-C", cwd, "rev-list", "--count",
                           "@{u}..HEAD", NULL};
    if (git_run_capture(cwd, argv4, out, GIT_OUT_CAP) == 0) {
        i32 ahead;
        if (checked_parse_i32_prefix(out, &ahead, NULL) && ahead >= 0)
            r->ahead = ahead;
    }

    free(out);
}

static void git_result_store_locked(const GitStatusResult *r, f64 ts) {
    i32 slot = -1, oldest = 0;
    for (i32 i = 0; i < GIT_RESULTS_CAP; i++) {
        if (g_results[i].ts != 0.0 && strcmp(g_results[i].r.cwd, r->cwd) == 0) {
            slot = i; break;
        }
        if (g_results[i].ts < g_results[oldest].ts) oldest = i;
    }
    if (slot < 0) slot = oldest;             /* an empty slot has ts 0 */
    g_results[slot].r  = *r;
    g_results[slot].ts = ts;
}

static void *git_status_worker(void *arg) {
    (void)arg;
    char cwd[1024];
    pthread_mutex_lock(&g_worker_mtx);
    for (;;) {
        if (g_queue_count == 0) {
            g_running[0] = '\0';
            g_worker_live = false;
            break;
        }
        snprintf(cwd, sizeof cwd, "%s", g_queue[g_queue_head]);
        g_queue_head = (g_queue_head + 1) % GIT_QUEUE_CAP;
        g_queue_count--;
        snprintf(g_running, sizeof g_running, "%s", cwd);
        g_runs_started++;
        pthread_mutex_unlock(&g_worker_mtx);

        GitStatusResult r;
        f64 t0 = platform_time_sec();
        compute_status(cwd, &r);
        f64 ts = platform_time_sec();
        r.cost = ts - t0;

        pthread_mutex_lock(&g_worker_mtx);
        git_result_store_locked(&r, ts);
        g_running[0] = '\0';
    }
    pthread_mutex_unlock(&g_worker_mtx);
    return NULL;
}

/* Number of Git subprocess runs started. Requests from windows sharing a
 * working directory can reuse a run. */
u64 git_status_runs_started(void) {
    pthread_mutex_lock(&g_worker_mtx);
    u64 n = g_runs_started;
    pthread_mutex_unlock(&g_worker_mtx);
    return n;
}

/* Query the process working directory first. Terminal.cwd comes from OSC 7
 * output and can be spoofed; trusting it could direct Git status to a
 * different repository whose configuration may invoke external commands. The
 * process working directory cannot be changed by terminal output. Cache the
 * system query because it runs per frame, and use OSC 7 only when the process
 * directory cannot be queried. */
static bool tab_git_cwd(Tab *tab, char *out, usize cap) {
    if (!tab || !out || cap == 0) return false;
    out[0] = '\0';
    Terminal *term = focus_terminal(tab);
    Session  *sess = focus_session(tab);
    if (!sess) {
        /* No process to ask. OSC 7 is the only answer there is. */
        if (term && term->cwd && term->cwd[0]) {
            snprintf(out, cap, "%s", term->cwd);
            return true;
        }
        return false;
    }

    /* Memoised on the TAB (git_status.probe_*), keyed on the pane it belongs
     * to so moving focus to another pane discards it immediately instead of
     * reporting the previous pane's directory for up to half a second.
     *
     * Not a file static keyed on the raw Session pointer: that pointer is
     * compared, never dereferenced, and so is an ABA across a free — a window
     * closing with its pane in repo B, then a new tab in another window inside
     * the same 0.5 s, can get the same address from malloc and inherit B's cwd
     * (and so B's branch and counts). A slot inside the asking tab cannot be
     * handed to anyone else. */
    f64 now = platform_time_sec();
    if (tab->git_status.probe_ts == 0.0 ||
        tab->git_status.probe_pane != tab->active_pane ||
        now - tab->git_status.probe_ts > 0.5) {
        const char *live = session_local_cwd(sess);
        gs_set_str(&tab->git_status.probe_cwd, live ? live : "");
        tab->git_status.probe_pane = tab->active_pane;
        tab->git_status.probe_ts   = now;
    }
    if (tab->git_status.probe_cwd && tab->git_status.probe_cwd[0]) {
        snprintf(out, cap, "%s", tab->git_status.probe_cwd);
        return true;
    }
    /* The probe could not say (the shell is not up yet, or has exited). OSC 7 is
     * the last resort, never the first choice — see the note above. */
    if (term && term->cwd && term->cwd[0]) {
        snprintf(out, cap, "%s", term->cwd);
        return true;
    }
    return false;
}

/* Show the newest finished run for the tab's cwd, if the tab has not shown
 * it yet. The sample time travels with it: a second window in the same cwd
 * takes the completed run immediately and does not extend its lifetime, so
 * both come due together on the normal interval. */
static void apply_result(Tab *tab) {
    char cur[1024];
    if (!tab_git_cwd(tab, cur, sizeof cur)) return;

    GitStatusResult r;
    f64 sampled_at = 0.0;
    f64 now = platform_time_sec();
    pthread_mutex_lock(&g_worker_mtx);
    for (i32 i = 0; i < GIT_RESULTS_CAP; i++) {
        if (g_results[i].ts == 0.0) continue;
        if (now - g_results[i].ts > GIT_RESULT_KEEP_SEC) {   /* aged out */
            g_results[i].ts = 0.0;
            continue;
        }
        if (strcmp(g_results[i].r.cwd, cur) == 0) {
            r = g_results[i].r;
            sampled_at = g_results[i].ts;
        }
    }
    pthread_mutex_unlock(&g_worker_mtx);
    if (sampled_at == 0.0) return;

    bool same_cwd = tab->git_status.cached_cwd &&
                    strcmp(tab->git_status.cached_cwd, cur) == 0;
    if (same_cwd && tab->git_status.valid &&
        tab->git_status.last_check_ts >= sampled_at) return;   /* shown */

    tab->git_status.is_repo       = r.is_repo;
    tab->git_status.added         = r.added;
    tab->git_status.removed       = r.removed;
    tab->git_status.ahead         = r.ahead;
    tab->git_status.partial       = r.partial;
    snprintf(tab->git_status.branch, sizeof tab->git_status.branch, "%s", r.branch);
    gs_set_str(&tab->git_status.repo_root, r.root);
    tab->git_status.valid         = true;
    tab->git_status.last_check_ts = sampled_at;
    tab->git_status.cost          = r.cost;
    gs_set_str(&tab->git_status.cached_cwd, r.cwd);
}

/* Queue a run for `cwd`. True when one is queued or running for it (the
 * caller has been served); false only when the queue is full or no thread
 * could be started, which the caller must not record as a refresh — it
 * retries on the next tick. */
static bool request_run(const char *cwd) {
    if (!cwd || !cwd[0] || strnlen(cwd, sizeof g_running) >= sizeof g_running)
        return false;
    pthread_mutex_lock(&g_worker_mtx);
    bool have = g_running[0] && strcmp(g_running, cwd) == 0;
    for (i32 i = 0; i < g_queue_count && !have; i++)
        if (strcmp(g_queue[(g_queue_head + i) % GIT_QUEUE_CAP], cwd) == 0) have = true;
    bool ok = true;
    if (!have) {
        if (g_queue_count >= GIT_QUEUE_CAP) {
            ok = false;
        } else {
            i32 tail = (g_queue_head + g_queue_count) % GIT_QUEUE_CAP;
            snprintf(g_queue[tail], sizeof g_queue[tail], "%s", cwd);
            g_queue_count++;
            if (!g_worker_live) {
                pthread_t tid;
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                if (pthread_create(&tid, &attr, git_status_worker, NULL) == 0) {
                    g_worker_live = true;
                } else {
                    g_queue_count--;         /* nobody would run it */
                    ok = false;
                }
                pthread_attr_destroy(&attr);
            }
        }
    }
    pthread_mutex_unlock(&g_worker_mtx);
    return ok;
}

void app_git_status_tick(AppState *app) {
    if (!app || app->tab_count == 0) return;
    if (app->active_tab < 0 || app->active_tab >= app->tab_count) return;

    Tab *tab = &app->tabs[app->active_tab];

    /* 20 Hz, not once per loop iteration. Both halves below resolve the
     * pane's cwd (tab_git_cwd, a copy plus a 0.5 s-cached proc probe) and
     * apply_result takes the worker mutex, which at the loop rate a
     * flooding pane drives is a real share of the main thread for a badge
     * that changes on a 2 s throttle. A worker result waits at most 50 ms
     * longer to be applied. */
    {
        f64 tnow = platform_time_sec();
        if (tab->git_status.tick_ts != 0.0 && tnow - tab->git_status.tick_ts < 0.05)
            return;
        tab->git_status.tick_ts = tnow;
    }

    /* Show whatever finished for this cwd, whichever window asked for it. */
    apply_result(tab);

    if (!is_local_session(tab)) {
        tab->git_status.valid = false;
        return;
    }
    /* OSC 7 when the shell offers it, the process's real cwd otherwise. */
    char cwd[1024];
    if (!tab_git_cwd(tab, cwd, sizeof cwd)) {
        tab->git_status.valid = false;
        return;
    }

    f64 now = platform_time_sec();
    bool cwd_changed = tab->git_status.valid &&
        (!tab->git_status.cached_cwd ||
         strcmp(tab->git_status.cached_cwd, cwd) != 0);
    /* How long to wait is ui/git_poll.h's decision: two seconds while the pane
     * is doing something, longer and longer while it is not, and never more
     * often than the last run's own cost allows. The pane's activity is its
     * content changing — the count the agent-busy heuristic already reads. */
    Terminal *term = focus_terminal(tab);
    u64 activity = term ? term->content_writes : 0;
    if (cwd_changed) git_poll_reset(&tab->git_status.poll);
    git_poll_observe(&tab->git_status.poll, activity);
    bool due = !tab->git_status.valid ||
               (now - tab->git_status.last_check_ts) >
                   git_poll_wait(&tab->git_status.poll, tab->git_status.cost);

    if (cwd_changed || due) {
        /* Mark last_check_ts now so the request is not repeated every tick
         * while the run is queued; the result carries its own sample time.
         * ONLY when the request was taken: a full queue has sampled nothing,
         * and stamping it anyway would make the window sit out a whole
         * throttle period per attempt. */
        if (request_run(cwd)) {
            tab->git_status.last_check_ts = now;
            tab->git_status.valid = true;
            git_poll_sampled(&tab->git_status.poll, activity);
        }
    }
}
