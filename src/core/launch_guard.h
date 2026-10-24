/*
 * Prevent repeated launches from restoring a session that fails during
 * startup. Write a marker before restore and clear it after a healthy interval
 * or normal exit. After the configured number of consecutive early exits,
 * start with a fresh shell and preserve the saved session for later recovery.
 */
#ifndef CORE_LAUNCH_GUARD_H
#define CORE_LAUNCH_GUARD_H

#include "core/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Up this long = the launch succeeded. Long enough for the restore and the
 * first frames of every tab; short enough that a person who quits on purpose
 * has usually passed it (and an ordinary quit clears the mark anyway). */
#define LAUNCH_GUARD_HEALTHY_SEC   15.0
/* Consecutive launches that ended early before the restore is skipped. */
#define LAUNCH_GUARD_SKIP_AFTER    2
/* A mark whose process is alive is another Liu still starting — but only if
 * the mark is this young. Process ids are reused: after a reboot the id in an
 * old mark can belong to anything, and must not disable the guard for good. */
#define LAUNCH_GUARD_STARTING_SEC  60

typedef struct {
    bool present;   /* a well-formed mark was found */
    i64  pid;       /* the process that wrote it */
    i64  started;   /* when, seconds since the epoch */
    i32  failed;    /* early ends in a row BEFORE that launch */
} LaunchMark;

typedef struct {
    bool participate;    /* write this launch's mark and clear it when healthy */
    bool skip_restore;   /* start fresh, set the saved session aside, say so */
    i32  failed;         /* early ends in a row, the previous launch included */
} LaunchGuardPlan;

static inline LaunchGuardPlan launch_guard_plan(LaunchMark prev, bool prev_pid_alive, i64 now) {
    LaunchGuardPlan p = { true, false, 0 };
    if (!prev.present) return p;
    i64 age = now - prev.started;
    if (prev_pid_alive && age >= 0 && age < LAUNCH_GUARD_STARTING_SEC) {
        /* Another instance is inside its own first seconds. Its mark is its
         * own; this launch neither counts it nor replaces it. */
        p.participate = false;
        p.failed = prev.failed;
        return p;
    }
    p.failed = prev.failed < 0 ? 1 : (prev.failed >= 99 ? 99 : prev.failed + 1);
    p.skip_restore = p.failed >= LAUNCH_GUARD_SKIP_AFTER;
    return p;
}

/* The mark is one line: "liu-launch 1 pid=<n> started=<n> failed=<n>\n".
 * Anything else — a torn write, another version's format — is "no mark": a
 * file the guard cannot read must never be the reason a restore is skipped. */
static inline i32 launch_mark_format(char *out, usize cap, i64 pid, i64 started, i32 failed) {
    int n = snprintf(out, cap, "liu-launch 1 pid=%lld started=%lld failed=%d\n",
                     (long long)pid, (long long)started, (int)failed);
    if (n <= 0 || (usize)n >= cap) { if (cap) out[0] = '\0'; return 0; }
    return (i32)n;
}

static inline bool launch_mark_field(const char **p, const char *name, i64 *out) {
    usize len = strlen(name);
    if (strncmp(*p, name, len) != 0) return false;
    const char *s = *p + len;
    if (*s < '0' || *s > '9') return false;
    i64 v = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; s++, digits++) {
        if (digits >= 18) return false;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    *p = s;
    return true;
}

static inline LaunchMark launch_mark_parse(const char *text) {
    LaunchMark m = { false, 0, 0, 0 };
    if (!text) return m;
    const char *p = text;
    static const char head[] = "liu-launch 1 ";
    if (strncmp(p, head, sizeof head - 1) != 0) return m;
    p += sizeof head - 1;
    i64 pid = 0, started = 0, failed = 0;
    if (!launch_mark_field(&p, "pid=", &pid) || *p++ != ' ') return m;
    if (!launch_mark_field(&p, "started=", &started) || *p++ != ' ') return m;
    if (!launch_mark_field(&p, "failed=", &failed)) return m;
    if (*p != '\n' || p[1] != '\0') return m;
    if (pid <= 0 || failed > 99) return m;
    m.present = true;
    m.pid = pid;
    m.started = started;
    m.failed = (i32)failed;
    return m;
}

#endif /* CORE_LAUNCH_GUARD_H */
