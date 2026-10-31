/*
 * Liu — the sessions delegated agents ran, for Agent History.
 *
 * A job's agent writes its session into the agent's own store like any
 * other, so Agent History would list it as a session the user had. Liu keeps
 * a list beside its config (delegated-sessions.tsv, one line a session:
 * "<session>\t<agent>\t<label>\t<who asked>\t<unix time>", appended by the
 * agent-control server) and the history list names those rows for what they
 * were.
 *
 * Pure: the caller reads the file (agentctl_delegated_read) and looks rows
 * up in what it read; nothing here touches anything but its arguments.
 */
#ifndef AGENTCTL_DELEGATED_H
#define AGENTCTL_DELEGATED_H

#include "core/types.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AGENTCTL_DELEGATED_FILE "delegated-sessions.tsv"
#define AGENTCTL_DELEGATED_MAX  (256u * 1024u)   /* the newest part read */

/* The list in `dir`, NUL-terminated and malloc'd (its newest
 * AGENTCTL_DELEGATED_MAX bytes, starting at a line), or NULL when there is
 * none. Opened without waiting, and read only when it is a regular file. */
static inline char *agentctl_delegated_read(const char *dir) {
    if (!dir || dir[0] != '/') return NULL;
    char path[1200];
    if ((usize)snprintf(path, sizeof path, "%s/%s", dir, AGENTCTL_DELEGATED_FILE) >= sizeof path) return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) { close(fd); return NULL; }
    off_t from = st.st_size > (off_t)AGENTCTL_DELEGATED_MAX ? st.st_size - (off_t)AGENTCTL_DELEGATED_MAX : 0;
    usize want = (usize)(st.st_size - from);
    char *buf = malloc(want + 1);
    if (!buf) { close(fd); return NULL; }
    ssize_t n = pread(fd, buf, want, from);
    close(fd);
    if (n <= 0) { free(buf); return NULL; }
    buf[n] = '\0';
    if (from > 0) {
        /* the first line may be a part of one */
        char *nl = strchr(buf, '\n');
        if (!nl) { free(buf); return NULL; }
        memmove(buf, nl + 1, strlen(nl + 1) + 1);
    }
    return buf;
}

/* Whether `session` is in `list`; `out` gets "<label>, for <who asked>"
 * (either may be missing) and `agent` the agent's id. */
static inline bool agentctl_delegated_lookup(const char *list, const char *session, char *out, usize cap,
                                             char *agent, usize acap) {
    if (out && cap) out[0] = '\0';
    if (agent && acap) agent[0] = '\0';
    if (!list || !session || !session[0]) return false;
    usize sl = strlen(session);
    for (const char *line = list; line && *line; ) {
        const char *nl = strchr(line, '\n');
        usize len = nl ? (usize)(nl - line) : strlen(line);
        if (len > sl && !strncmp(line, session, sl) && line[sl] == '\t') {
            /* the fields after the session */
            const char *f[4] = { NULL, NULL, NULL, NULL };
            usize fl[4] = { 0, 0, 0, 0 };
            const char *p = line + sl + 1, *end = line + len;
            for (i32 k = 0; k < 4 && p <= end; k++) {
                const char *t = memchr(p, '\t', (usize)(end - p));
                f[k] = p;
                fl[k] = t ? (usize)(t - p) : (usize)(end - p);
                if (!t) break;
                p = t + 1;
            }
            if (agent && acap && f[0]) snprintf(agent, acap, "%.*s", (int)fl[0], f[0]);
            if (out && cap) {
                if (f[1] && fl[1] && f[2] && fl[2])
                    snprintf(out, cap, "%.*s, for %.*s", (int)fl[1], f[1], (int)fl[2], f[2]);
                else if (f[1] && fl[1])
                    snprintf(out, cap, "%.*s", (int)fl[1], f[1]);
                else if (f[2] && fl[2])
                    snprintf(out, cap, "for %.*s", (int)fl[2], f[2]);
            }
            return true;
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

#endif /* AGENTCTL_DELEGATED_H */
