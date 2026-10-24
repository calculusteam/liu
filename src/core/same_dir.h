/*
 * Liu — "is the shell sitting in the home folder?" is a question about the
 * FOLDER, not about how two strings spell it.
 *
 * Agent History shows every session when the pane is at $HOME and only the
 * pane's own folder's otherwise. The cwd comes from the kernel and is fully
 * resolved; $HOME is whatever the environment says. Whenever $HOME reaches
 * the folder through a symbolic link (a home on another volume, /home on
 * macOS, any path under /var or /tmp — /var is a link to /private/var) or
 * carries a trailing slash, strcmp(cwd, $HOME) never matches: the list would
 * be filtered to "sessions recorded for the home folder itself", which is
 * none, and Agent History would open empty at the one place it should show
 * everything.
 *
 * Same text is the fast answer; otherwise two existing directories are the
 * same when the file system says they are one.
 */
#ifndef CORE_SAME_DIR_H
#define CORE_SAME_DIR_H

#include "core/types.h"

#include <string.h>
#include <sys/stat.h>

static inline bool liu_same_dir(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) return false;
    if (strcmp(a, b) == 0) return true;
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return false;
    return S_ISDIR(sa.st_mode) && S_ISDIR(sb.st_mode) &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

#endif /* CORE_SAME_DIR_H */
