/*
 * Liu — create a directory Liu owns, with every missing parent.
 *
 * Crash capture and the vault both need ~/.config/Liu before anything else,
 * and one mkdir() cannot create a parent. A new macOS account has no
 * ~/.config, so with a single call the first launch of a fresh install would
 * make no crasherrors/ directory (nothing could be recorded for that whole
 * run, the first-run intro included) and the vault could not open its
 * database.
 *
 * Missing components are created with `mode`; a component that already exists
 * is left exactly as it is (somebody else's ~/.config keeps its own mode).
 */
#ifndef CORE_MKDIR_P_H
#define CORE_MKDIR_P_H

#include "core/types.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

/* True when `path` is a directory once this returns. False when a component
 * exists and is not a directory, cannot be created, or the path is too long. */
static inline bool liu_mkdir_p(const char *path, mode_t mode) {
    if (!path || !path[0]) return false;
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s", path);
    if (n <= 0 || (usize)n >= sizeof tmp) return false;

    for (char *p = tmp + 1; ; p++) {
        if (*p != '/' && *p != '\0') continue;
        char end = *p;
        *p = '\0';
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return false;
        if (!end) break;
        *p = '/';
        while (p[1] == '/') p++;            /* "a//b" names the same place */
        if (p[1] == '\0') break;            /* trailing slash */
    }

    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

#endif /* CORE_MKDIR_P_H */
