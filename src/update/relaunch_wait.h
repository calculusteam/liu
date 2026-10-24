/*
 * relaunch_wait — the updated copy of Liu (Linux) starts once the copy it
 * replaces has gone.
 *
 * The replacement takes over what the old process holds until its very last
 * moment: the notify socket's lock (released at exit), the agent hooks (taken
 * out at exit, put back by the new copy), the saved sessions. A copy that
 * comes up beside a living one finds the socket owned and runs without
 * notifications for its whole life, so the new copy waits for the old process
 * to exit rather than for a fixed delay.
 *
 * Linux only: the answer is read from /proc.
 */
#ifndef UPDATE_RELAUNCH_WAIT_H
#define UPDATE_RELAUNCH_WAIT_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

/* How long the new copy waits for the old one, at most, before it starts anyway. */
#define RELAUNCH_WAIT_MAX_MS 15000
#define RELAUNCH_WAIT_STEP_MS 50

/* Has process `pid` ended? Gone, or a zombie its parent has not collected yet
 * (a zombie holds nothing any more). The state letter of /proc/<pid>/stat
 * follows the parenthesised command name, which may itself hold ')'. */
static inline bool relaunch_pid_ended(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    FILE *f = fopen(path, "r");
    if (!f) return true;
    char buf[512];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char *paren = strrchr(buf, ')');
    if (!paren || paren[1] != ' ') return false;
    return paren[2] == 'Z' || paren[2] == 'X';
}

/* Waits until `pid` has ended or `max_ms` passed; true when it ended. */
static inline bool relaunch_wait_for_exit(pid_t pid, int max_ms) {
    for (int waited = 0; waited < max_ms; waited += RELAUNCH_WAIT_STEP_MS) {
        if (relaunch_pid_ended(pid)) return true;
        struct timespec ts = { 0, RELAUNCH_WAIT_STEP_MS * 1000L * 1000L };
        nanosleep(&ts, NULL);
    }
    return relaunch_pid_ended(pid);
}

#endif /* UPDATE_RELAUNCH_WAIT_H */
