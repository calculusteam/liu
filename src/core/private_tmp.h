/*
 * private_tmp — a temporary directory that is this user's alone.
 *
 * $TMPDIR is that on macOS (the per-user folder under /var/folders, mode
 * 0700), and it is not that anywhere $TMPDIR is unset or points at a shared
 * /tmp: a name built there can be read by another user, and created by them
 * first. Whoever keeps something of the user's in a temporary file — a
 * sleeping tab's scrollback (ui/sleep_snap.c) — asks here instead of reading
 * $TMPDIR:
 *
 *   - $TMPDIR itself, when it is a directory this user owns that is closed to
 *     group and others;
 *   - otherwise "<$TMPDIR or /tmp>/<leaf>-<uid>", made 0700, and refused when
 *     what is found under that name is not a directory of this user's with that
 *     mode (somebody else's, a symbolic link, a looser mode).
 *
 * False means there is no such place; the caller does without the file.
 */
#ifndef CORE_PRIVATE_TMP_H
#define CORE_PRIVATE_TMP_H

#include "core/types.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline bool liu_private_tmp_ours(const char *dir) {
    struct stat st;
    return lstat(dir, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
           (st.st_mode & 077) == 0;
}

static inline bool liu_private_tmp_dir(char *out, usize cap, const char *leaf) {
    if (!out || cap == 0 || !leaf || !leaf[0]) return false;
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    usize bl = strlen(base);
    while (bl > 1 && base[bl - 1] == '/') bl--;
    if (bl >= cap) return false;
    memcpy(out, base, bl);
    out[bl] = '\0';
    if (liu_private_tmp_ours(out)) return true;
    int w = snprintf(out + bl, cap - bl, "/%s-%u", leaf, (unsigned)geteuid());
    if (w <= 0 || (usize)w >= cap - bl) { out[0] = '\0'; return false; }
    if (mkdir(out, 0700) != 0 && errno != EEXIST) { out[0] = '\0'; return false; }
    if (!liu_private_tmp_ours(out)) { out[0] = '\0'; return false; }
    return true;
}

#endif /* CORE_PRIVATE_TMP_H */
