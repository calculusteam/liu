/*
 * Liu - Mosh wrapper implementation
 * Spawns `mosh` binary via forkpty, relays I/O.
 */
#include "ssh/mosh.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
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

bool mosh_available(void) {
    return access("/usr/bin/mosh", X_OK) == 0 ||
           access("/usr/local/bin/mosh", X_OK) == 0 ||
           access("/opt/homebrew/bin/mosh", X_OK) == 0;
}

MoshSession *mosh_create(const char *host, const char *user, i32 port,
                          i32 cols, i32 rows) {
    MoshSession *ms = calloc(1, sizeof(MoshSession));
    if (!ms) return NULL;
    ms->cols = cols;
    ms->rows = rows;

    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };

    pid_t pid = forkpty(&ms->pty_master, NULL, NULL, &ws);
    if (pid < 0) {
        free(ms);
        return NULL;
    }

    if (pid == 0) {
        /* Child: exec mosh */
        setenv("TERM", "xterm-256color", 1);

        /* Find mosh binary */
        const char *mosh_bin = "/usr/bin/mosh";
        if (access("/usr/local/bin/mosh", X_OK) == 0) mosh_bin = "/usr/local/bin/mosh";
        if (access("/opt/homebrew/bin/mosh", X_OK) == 0) mosh_bin = "/opt/homebrew/bin/mosh";

        /* Build args: mosh [--ssh="ssh -p PORT"] user@host */
        char target[384];
        if (user && user[0]) {
            snprintf(target, sizeof(target), "%s@%s", user, host);
        } else {
            snprintf(target, sizeof(target), "%s", host);
        }

        if (port > 0 && port != 22) {
            char ssh_arg[64];
            snprintf(ssh_arg, sizeof(ssh_arg), "ssh -p %d", port);
            char *argv[] = { (char *)mosh_bin, "--ssh", ssh_arg, target, NULL };
            execvp(mosh_bin, argv);
        } else {
            char *argv[] = { (char *)mosh_bin, target, NULL };
            execvp(mosh_bin, argv);
        }
        _exit(127);
    }

    /* Parent */
    ms->child_pid = pid;
    ms->alive = true;

    int flags = fcntl(ms->pty_master, F_GETFL, 0);
    fcntl(ms->pty_master, F_SETFL, flags | O_NONBLOCK);

    return ms;
}

void mosh_destroy(MoshSession *ms) {
    if (!ms) return;
#ifndef PLATFORM_WIN32
    if (ms->pty_master >= 0) close(ms->pty_master);
    if (ms->child_pid > 0) {
        /* Terminate then block-reap the child so it can't outlive us as an
         * orphan or linger as a zombie. A bare waitpid(WNOHANG) returns
         * immediately on a still-running child and never reaps it. Closing
         * the PTY master above already drops the controlling terminal, so
         * SIGTERM usually suffices; poll briefly and escalate to SIGKILL. */
        kill(ms->child_pid, SIGTERM);
        bool reaped = false;
        for (i32 i = 0; i < 50; i++) {            /* up to ~500ms */
            pid_t r = waitpid(ms->child_pid, NULL, WNOHANG);
            if (r == ms->child_pid || (r < 0 && errno == ECHILD)) {
                reaped = true;
                break;
            }
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (!reaped) {
            kill(ms->child_pid, SIGKILL);
            (void)waitpid(ms->child_pid, NULL, 0);  /* blocking reap */
        }
    }
#endif
    free(ms);
}

i32 mosh_read(MoshSession *ms, u8 *buf, i32 buf_size) {
    ssize_t n = read(ms->pty_master, buf, (size_t)buf_size);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        ms->alive = false;
        return -1;
    }
    if (n == 0) { ms->alive = false; return -1; }
    return (i32)n;
}

i32 mosh_write(MoshSession *ms, const u8 *data, i32 len) {
    ssize_t n = write(ms->pty_master, data, (size_t)len);
    return n > 0 ? (i32)n : -1;
}

void mosh_resize(MoshSession *ms, i32 cols, i32 rows) {
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

bool mosh_is_alive(MoshSession *ms) {
    if (!ms || !ms->alive) return false;
#ifndef PLATFORM_WIN32
    int status;
    pid_t r = waitpid(ms->child_pid, &status, WNOHANG);
    if (r != 0) { ms->alive = false; return false; }
#endif
    return true;
}

i32 mosh_child_pid(MoshSession *ms) {
    if (!ms) return -1;
    return (i32)ms->child_pid;
}
