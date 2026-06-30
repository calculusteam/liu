/*
 * Liu — lightweight git status sampler for the bottom status bar.
 *
 * For the focused tab (local sessions only — SSH/Mosh tabs skip, since
 * their cwd refers to a remote path), we periodically run:
 *
 *   git -C <cwd> rev-parse --git-dir
 *   git -C <cwd> diff --numstat          (unstaged changes)
 *   git -C <cwd> diff --cached --numstat (staged changes)
 *   git -C <cwd> rev-list --count @{u}..HEAD  (commits ahead of upstream)
 *
 * Results feed `tab->git_status`, which the status bar renderer reads to
 * paint "+N -M ↑K" after the cwd label.
 *
 * Implementation detail: we fork+execvp git directly (no shell) to avoid
 * any risk of argv injection from a terminal-supplied cwd (OSC 7). The
 * child's stderr is sent to /dev/null; stdout is piped back. A single
 * call suite takes ~20-100 ms on a typical repo, so we throttle to one
 * refresh every GIT_STATUS_THROTTLE_SEC per focused tab.
 */
#include "ui/ui.h"
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

#define GIT_STATUS_THROTTLE_SEC 2.0

/* Off-thread worker state. A single worker services the focused tab — on
 * spawn it captures the cwd and a monotonic token; on completion it
 * publishes results to `g_pending` which the next tick picks up and copies
 * into the tab iff its cwd still matches (otherwise the result is stale). */
typedef struct {
    char       cwd[1024];
    bool       is_repo;
    i32        added, removed, ahead;
    i32        files_changed;       /* distinct files with uncommitted edits
                                     * (working tree + index, deduplicated) */
    bool       valid;
} GitStatusResult;

static pthread_t        g_worker_tid;
static pthread_mutex_t  g_worker_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool             g_worker_live    = false;  /* thread handle live & unjoined */
static bool             g_result_pending = false;  /* result waiting for main-thread pickup */
static GitStatusResult  g_pending;
static char             g_worker_cwd[1024];

/* Run `git <argv...>` with working directory `cwd`. Captures up to
 * `out_size` bytes of stdout into `out`. Returns the process exit
 * status (0 = success) or -1 on fork / pipe error. */
static int run_git(const char *cwd, const char *const *argv,
                   char *out, usize out_size) {
    if (!cwd || !argv || !out || out_size == 0) return -1;

    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect stdout to pipe, stderr to /dev/null, chdir,
         * exec git. No shell — argv is passed through. */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(pipefd[1]);

        if (chdir(cwd) != 0) _exit(127);
        execvp("git", (char *const *)argv);
        _exit(127);
    }

    close(pipefd[1]);
    usize total = 0;
    while (total < out_size - 1) {
        ssize_t r = read(pipefd[0], out + total, out_size - 1 - total);
        if (r <= 0) break;
        total += (usize)r;
    }
    out[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static void parse_numstat(const char *buf, i32 *added, i32 *removed) {
    const char *p = buf;
    while (*p) {
        /* Each line: "<added>\t<removed>\t<path>\n". Binary files may
         * report "-\t-\t…" — we skip those. */
        i32 a = 0, r = 0;
        const char *start = p;
        if (*p == '-') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        while (*p >= '0' && *p <= '9') { a = a * 10 + (*p - '0'); p++; }
        if (*p != '\t') { /* malformed line, skip */
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        p++; /* tab */
        if (*p == '-') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        while (*p >= '0' && *p <= '9') { r = r * 10 + (*p - '0'); p++; }
        *added += a;
        *removed += r;
        /* Skip to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (p == start) break; /* defensive: avoid infinite loop */
    }
}

static bool is_local_session(const Tab *tab) {
    if (!tab || !tab->session) return false;
    return session_type(tab->session) == SESSION_LOCAL;
}

static void compute_status(const char *cwd, GitStatusResult *r) {
    char out[4096];
    memset(r, 0, sizeof *r);
    snprintf(r->cwd, sizeof r->cwd, "%s", cwd);
    r->valid = true;

    const char *argv1[] = {"git", "-C", cwd, "rev-parse", "--git-dir", NULL};
    int rc = run_git(cwd, argv1, out, sizeof out);
    r->is_repo = (rc == 0 && out[0] != '\0');
    if (!r->is_repo) return;

    const char *argv2[] = {"git", "-C", cwd, "diff", "--numstat", NULL};
    if (run_git(cwd, argv2, out, sizeof out) == 0) {
        parse_numstat(out, &r->added, &r->removed);
    }

    const char *argv3[] = {"git", "-C", cwd, "diff", "--cached",
                           "--numstat", NULL};
    if (run_git(cwd, argv3, out, sizeof out) == 0) {
        parse_numstat(out, &r->added, &r->removed);
    }

    const char *argv4[] = {"git", "-C", cwd, "rev-list", "--count",
                           "@{u}..HEAD", NULL};
    if (run_git(cwd, argv4, out, sizeof out) == 0) {
        r->ahead = atoi(out);
    }

    /* Distinct uncommitted files via porcelain v1. Each non-empty line is
     * one path; "XY path" — we only count lines, ignore status codes. */
    const char *argv5[] = {"git", "-C", cwd, "status", "--porcelain", NULL};
    if (run_git(cwd, argv5, out, sizeof out) == 0) {
        i32 files = 0;
        for (const char *p = out; *p; ) {
            const char *line_start = p;
            while (*p && *p != '\n') p++;
            if (p > line_start) files++;
            if (*p == '\n') p++;
        }
        r->files_changed = files;
    }
}

static void *git_status_worker(void *arg) {
    (void)arg;
    GitStatusResult r;
    compute_status(g_worker_cwd, &r);

    pthread_mutex_lock(&g_worker_mtx);
    g_pending = r;
    g_result_pending = true;
    pthread_mutex_unlock(&g_worker_mtx);
    return NULL;
}

/* Apply a pending worker result to `tab` iff the tab's cwd still matches. */
static void apply_pending_result(Tab *tab) {
    pthread_mutex_lock(&g_worker_mtx);
    bool have = g_result_pending;
    GitStatusResult r = g_pending;
    g_result_pending = false;
    pthread_mutex_unlock(&g_worker_mtx);

    /* Only join when the worker has actually published a result; g_result_pending
     * is set just before the worker returns (see git_status_worker), so `have`
     * implies the thread is about to exit and pthread_join returns immediately.
     * Joining on `g_worker_live` alone would block the main thread for the
     * full 20-100 ms compute_status runtime, defeating the off-thread design. */
    if (have && g_worker_live) {
        pthread_join(g_worker_tid, NULL);
        g_worker_live = false;
    }
    if (!have || !tab->terminal || !tab->terminal->cwd) return;
    if (strcmp(r.cwd, tab->terminal->cwd) != 0) return;  /* stale */

    tab->git_status.is_repo       = r.is_repo;
    tab->git_status.added         = r.added;
    tab->git_status.removed       = r.removed;
    tab->git_status.ahead         = r.ahead;
    tab->git_status.files_changed = r.files_changed;
    tab->git_status.valid         = true;
    tab->git_status.last_check_ts = platform_time_sec();
    free(tab->git_status.cached_cwd);
    usize n = strlen(r.cwd);
    tab->git_status.cached_cwd = malloc(n + 1);
    if (tab->git_status.cached_cwd) memcpy(tab->git_status.cached_cwd, r.cwd, n + 1);
}

static void spawn_worker(const char *cwd) {
    pthread_mutex_lock(&g_worker_mtx);
    bool busy = g_worker_live;
    pthread_mutex_unlock(&g_worker_mtx);
    if (busy) return;

    snprintf(g_worker_cwd, sizeof g_worker_cwd, "%s", cwd);
    if (pthread_create(&g_worker_tid, NULL, git_status_worker, NULL) == 0) {
        g_worker_live = true;
    }
}

void app_git_status_tick(AppState *app) {
    if (!app || app->tab_count == 0) return;
    if (app->active_tab < 0 || app->active_tab >= app->tab_count) return;

    Tab *tab = &app->tabs[app->active_tab];

    /* Collect any result the worker left for us regardless of tab state —
     * drops it on the floor if the tab is no longer eligible. */
    apply_pending_result(tab);

    if (!is_local_session(tab)) {
        tab->git_status.valid = false;
        return;
    }
    /* Terminal.cwd is heap-allocated and NULL until the shell emits OSC 7. */
    if (!tab->terminal || !tab->terminal->cwd || !tab->terminal->cwd[0]) {
        tab->git_status.valid = false;
        return;
    }

    f64 now = platform_time_sec();
    bool cwd_changed = tab->git_status.valid &&
        (!tab->git_status.cached_cwd ||
         strcmp(tab->git_status.cached_cwd, tab->terminal->cwd) != 0);
    bool due = !tab->git_status.valid ||
               (now - tab->git_status.last_check_ts) > GIT_STATUS_THROTTLE_SEC;

    if (cwd_changed || due) {
        /* Mark last_check_ts now so we don't re-spawn every tick while the
         * worker is running; the actual timestamp is refreshed on publish. */
        tab->git_status.last_check_ts = now;
        tab->git_status.valid = true;
        spawn_worker(tab->terminal->cwd);
    }
}
