/*
 * Liu - Mosh wrapper implementation
 * Spawns `mosh` binary via forkpty, relays I/O.
 */
#include "ssh/mosh.h"
#include "ssh/ssh_target.h"
#include "ssh/child_signals.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#ifndef PLATFORM_WIN32
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <sys/ioctl.h>
    #if defined(PLATFORM_MACOS)
        #include <util.h>
    #else
        #include <pty.h>
    #endif
#endif

struct MoshSession {
    int    pty_master;
    pid_t  child_pid;
    i32    cols, rows;
    bool   alive;
};

static bool mosh_setup_fd(int fd) {
    int flags;
    do { flags = fcntl(fd, F_GETFL, 0); }
    while (flags < 0 && errno == EINTR);
    if (flags < 0) return false;
    int rc;
    do { rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK); }
    while (rc < 0 && errno == EINTR);
    if (rc != 0) return false;
    do { flags = fcntl(fd, F_GETFD, 0); }
    while (flags < 0 && errno == EINTR);
    if (flags < 0) return false;
    do { rc = fcntl(fd, F_SETFD, flags | FD_CLOEXEC); }
    while (rc < 0 && errno == EINTR);
    return rc == 0;
}

static void mosh_stop_child(pid_t pid) {
    if (pid <= 0) return;
    if (kill(-pid, SIGTERM) != 0 && errno == ESRCH) (void)kill(pid, SIGTERM);
    /* Do not reap before the final group signal: retaining the zombie leader
     * prevents pid/pgid reuse while descendants are being terminated. */
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    (void)kill(-pid, SIGKILL);
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

bool mosh_available(void) {
    return access("/usr/bin/mosh", X_OK) == 0 ||
           access("/usr/local/bin/mosh", X_OK) == 0 ||
           access("/opt/homebrew/bin/mosh", X_OK) == 0;
}

MoshSession *mosh_create(const char *host, const char *user, i32 port,
                          i32 cols, i32 rows) {
    if (!host || !host[0] || port < 0 || port > 65535 ||
        strnlen(host, 256) >= 256 || (user && strnlen(user, 128) >= 128) ||
        cols <= 0 || cols > (i32)UINT16_MAX ||
        rows <= 0 || rows > (i32)UINT16_MAX) return NULL;

    /* The host and user end up on mosh's command line, where a leading '-' is an OPTION
     * (-oProxyCommand=...), not a name. Checked before anything else so it holds whether or
     * not mosh is installed. */
    if (!ssh_target_is_safe(host, user)) return NULL;

    /* Resolve every string before forkpty. The child of a multithreaded
     * process may call only async-signal-safe functions before exec. */
    const char *mosh_bin = "/usr/bin/mosh";
    if (access("/usr/local/bin/mosh", X_OK) == 0)
        mosh_bin = "/usr/local/bin/mosh";
    if (access("/opt/homebrew/bin/mosh", X_OK) == 0)
        mosh_bin = "/opt/homebrew/bin/mosh";
    if (access(mosh_bin, X_OK) != 0) return NULL;

    char target[384];
    int n = user && user[0]
          ? snprintf(target, sizeof target, "%s@%s", user, host)
          : snprintf(target, sizeof target, "%s", host);
    if (n < 0 || (usize)n >= sizeof target) return NULL;

    char ssh_arg[64] = {0};
    if (port > 0 && port != 22) {
        n = snprintf(ssh_arg, sizeof ssh_arg, "ssh -p %d", port);
        if (n < 0 || (usize)n >= sizeof ssh_arg) return NULL;
    }

    MoshSession *ms = calloc(1, sizeof(MoshSession));
    if (!ms) return NULL;
    ms->pty_master = -1;
    ms->cols = cols;
    ms->rows = rows;

    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };

    /* Before the fork: getdtablesize is not async-signal-safe. */
    int maxfd = getdtablesize();
    if (maxfd < 64) maxfd = 64;
    if (maxfd > 65536) maxfd = 65536;

    pid_t pid = forkpty(&ms->pty_master, NULL, NULL, &ws);
    if (pid < 0) {
        free(ms);
        return NULL;
    }

    if (pid == 0) {
        /* Child: precomputed argv + async-signal-safe exec only.
         *
         * The same hygiene a local shell gets (ssh_session.c,
         * local_child_hygiene): the signals back to their defaults and none
         * blocked (an ignored disposition survives exec: ssh/child_signals.h),
         * and nothing open above stderr — mosh and the ssh it runs have no
         * business holding Liu's descriptors. */
        child_signals_default();
        for (int fd = 3; fd < maxfd; fd++) close(fd);
        if (port > 0 && port != 22) {
            char *argv[] = { (char *)mosh_bin, "--ssh", ssh_arg, target, NULL };
            execv(mosh_bin, argv);
        } else {
            char *argv[] = { (char *)mosh_bin, target, NULL };
            execv(mosh_bin, argv);
        }
        _exit(127);
    }

    /* Parent */
    ms->child_pid = pid;
    ms->alive = true;

    if (!mosh_setup_fd(ms->pty_master)) {
        close(ms->pty_master);
        ms->pty_master = -1;
        mosh_stop_child(ms->child_pid);
        free(ms);
        return NULL;
    }

    return ms;
}

void mosh_destroy(MoshSession *ms) {
    if (!ms) return;
#ifndef PLATFORM_WIN32
    if (ms->pty_master >= 0) { close(ms->pty_master); ms->pty_master = -1; }
    if (ms->child_pid > 0) {
        mosh_stop_child(ms->child_pid);
        ms->child_pid = 0;
    }
#endif
    free(ms);
}

i32 mosh_read(MoshSession *ms, u8 *buf, i32 buf_size) {
    if (!ms || ms->pty_master < 0 || !buf || buf_size <= 0) return -1;
    ssize_t n = read(ms->pty_master, buf, (size_t)buf_size);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        ms->alive = false;
        return -1;
    }
    if (n == 0) { ms->alive = false; return -1; }
    return (i32)n;
}

i32 mosh_write(MoshSession *ms, const u8 *data, i32 len) {
    if (!ms || ms->pty_master < 0 || !data || len <= 0) return -1;
    ssize_t n = write(ms->pty_master, data, (size_t)len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    return n > 0 ? (i32)n : -1;
}

void mosh_resize(MoshSession *ms, i32 cols, i32 rows) {
    if (!ms || ms->pty_master < 0 || cols <= 0 || rows <= 0 ||
        cols > (i32)UINT16_MAX || rows > (i32)UINT16_MAX) return;
    ms->cols = cols;
    ms->rows = rows;
#ifndef PLATFORM_WIN32
    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };
    ioctl(ms->pty_master, TIOCSWINSZ, &ws);
#endif
}

int mosh_fd(const MoshSession *ms) {
    return ms ? ms->pty_master : -1;
}

bool mosh_is_alive(MoshSession *ms) {
    if (!ms || !ms->alive || ms->child_pid <= 0) return false;
#ifndef PLATFORM_WIN32
    int status;
    pid_t r = waitpid(ms->child_pid, &status, WNOHANG);
    if (r == 0) return true;
    if (r < 0 && errno == EINTR) return true;
    ms->alive = false;
    ms->child_pid = 0;
    return false;
#endif
    return ms->alive;
}

i32 mosh_child_pid(MoshSession *ms) {
    if (!ms || ms->child_pid <= 0 || (u64)ms->child_pid > (u64)INT32_MAX)
        return -1;
    return (i32)ms->child_pid;
}
