/*
 * Liu — moving a file to the desktop's Trash (the freedesktop.org Trash
 * specification), for the X11 backend's platform_trash_path().
 *
 * A Trash is two folders: files/ holds what was trashed, info/ holds one
 * <name>.trashinfo per item saying where it came from and when, which is what
 * a file manager's "Restore" reads. The info file is created FIRST, with
 * O_EXCL, to claim the name; the item is renamed in after it, and a failed
 * rename takes the claim back.
 *
 *   home trash   $XDG_DATA_HOME/Trash (~/.local/share/Trash), for anything on
 *                the same file system; Path= is absolute.
 *   top trash    for a file on another file system (a second disk, a USB
 *                stick): $topdir/.Trash/$uid when an administrator made a
 *                shared .Trash (a directory, sticky, not a link), otherwise
 *                $topdir/.Trash-$uid; Path= is relative to $topdir.
 *
 * A rename is the only move: nothing is copied across file systems, so a
 * file is either in a Trash or where it was. False when no Trash can take it
 * (a read-only mount, no permission); the caller then asks before deleting.
 *
 * Pure POSIX: no X11, so it builds and runs on any host.
 */
#ifndef LIU_PLATFORM_XDG_TRASH_H
#define LIU_PLATFORM_XDG_TRASH_H

#include "core/types.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Path= is escaped as a URL path: everything but the unreserved characters
 * and '/' becomes %XX. False when it does not fit. */
static inline bool xdg_trash_escape(const char *in, char *out, usize cap) {
    static const char hex[] = "0123456789ABCDEF";
    usize n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
                    c == '.' || c == '~' || c == '!' || c == '*' || c == '\'' ||
                    c == '(' || c == ')';
        if (keep) {
            if (n + 1 >= cap) return false;
            out[n++] = (char)c;
        } else {
            if (n + 3 >= cap) return false;
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    if (n >= cap) return false;
    out[n] = '\0';
    return true;
}

/* mkdir that is fine with the folder existing, mode 0700 like the spec asks. */
static inline bool xdg_trash_mkdir(const char *dir) {
    if (mkdir(dir, 0700) == 0) return true;
    struct stat st;
    return errno == EEXIST && lstat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

/* `root`/files and `root`/info, made when missing. */
static inline bool xdg_trash_prepare(const char *root) {
    char sub[PATH_MAX];
    if (!xdg_trash_mkdir(root)) return false;
    if (snprintf(sub, sizeof sub, "%s/files", root) >= (int)sizeof sub || !xdg_trash_mkdir(sub))
        return false;
    if (snprintf(sub, sizeof sub, "%s/info", root) >= (int)sizeof sub || !xdg_trash_mkdir(sub))
        return false;
    return true;
}

/* Claim a name in `root`, write its info file, rename `abs` in. `info_path` is
 * what Path= says (absolute, or relative to a top directory). */
static inline bool xdg_trash_into(const char *root, const char *abs, const char *info_path,
                                  time_t now) {
    const char *base = strrchr(abs, '/');
    base = base ? base + 1 : abs;
    if (!base[0]) return false;
    char escaped[PATH_MAX * 3];
    if (!xdg_trash_escape(info_path, escaped, sizeof escaped)) return false;
    char date[32];
    struct tm tmv;
    if (!localtime_r(&now, &tmv) || !strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tmv))
        return false;

    /* name, name.2, name.3 …: the first one nobody has claimed. */
    for (i32 attempt = 1; attempt <= 1000; attempt++) {
        char name[NAME_MAX + 1];
        int k = attempt == 1 ? snprintf(name, sizeof name, "%s", base)
                             : snprintf(name, sizeof name, "%s.%d", base, (int)attempt);
        if (k <= 0 || k >= (int)sizeof name) return false;
        char info[PATH_MAX], dest[PATH_MAX];
        if (snprintf(info, sizeof info, "%s/info/%s.trashinfo", root, name) >= (int)sizeof info ||
            snprintf(dest, sizeof dest, "%s/files/%s", root, name) >= (int)sizeof dest)
            return false;
        int fd = open(info, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        struct stat st;
        if (lstat(dest, &st) == 0) {            /* files/ has it without an info */
            close(fd);
            unlink(info);
            continue;
        }
        char body[sizeof escaped + 96];
        int blen = snprintf(body, sizeof body, "[Trash Info]\nPath=%s\nDeletionDate=%s\n",
                            escaped, date);
        bool wrote = blen > 0 && blen < (int)sizeof body &&
                     write(fd, body, (usize)blen) == (ssize_t)blen;
        if (close(fd) != 0) wrote = false;
        if (!wrote || rename(abs, dest) != 0) {
            unlink(info);
            return false;
        }
        return true;
    }
    return false;
}

/* The top directory of the file system `dir` is on: the last ancestor with
 * the same device. */
static inline bool xdg_trash_topdir(const char *dir, dev_t dev, char *out, usize cap) {
    char cur[PATH_MAX];
    if (snprintf(cur, sizeof cur, "%s", dir) >= (int)sizeof cur) return false;
    for (;;) {
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) {           /* reached "/" */
            struct stat rs;
            if (lstat("/", &rs) == 0 && rs.st_dev == dev) { snprintf(out, cap, "/"); return true; }
            break;
        }
        char parent[PATH_MAX];
        usize plen = (usize)(slash - cur);
        memcpy(parent, cur, plen);
        parent[plen] = '\0';
        struct stat ps;
        if (lstat(parent, &ps) != 0 || ps.st_dev != dev) break;
        memcpy(cur, parent, plen + 1);
    }
    return snprintf(out, cap, "%s", cur) < (int)cap;
}

/* Move `path` (absolute) to a Trash. `data_home` is $XDG_DATA_HOME (or
 * ~/.local/share), `uid` names the per-user top trash. */
static inline bool xdg_trash_path(const char *path, const char *data_home, uid_t uid,
                                  time_t now) {
    if (!path || path[0] != '/' || !data_home || !data_home[0]) return false;
    /* The file itself is not followed (a link is trashed as a link); its
     * folder is, so Path= names where it really is. */
    char dir[PATH_MAX], real_dir[PATH_MAX], abs[PATH_MAX];
    const char *slash = strrchr(path, '/');
    usize dlen = (usize)(slash - path);
    if (dlen == 0) dlen = 1;
    if (dlen >= sizeof dir) return false;
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';
    if (!realpath(dir, real_dir)) return false;
    if (!slash[1]) return false;
    if (snprintf(abs, sizeof abs, "%s/%s", strcmp(real_dir, "/") ? real_dir : "", slash + 1)
            >= (int)sizeof abs)
        return false;
    struct stat fst;
    if (lstat(abs, &fst) != 0) return false;

    char home_trash[PATH_MAX];
    if (snprintf(home_trash, sizeof home_trash, "%s/Trash", data_home) >= (int)sizeof home_trash)
        return false;
    struct stat ds;
    if (stat(data_home, &ds) == 0 && ds.st_dev == fst.st_dev) {
        /* The home trash: same file system, so the rename can work. Never
         * the Trash itself or something inside it. */
        if (strncmp(abs, home_trash, strlen(home_trash)) == 0) return false;
        return xdg_trash_prepare(home_trash) && xdg_trash_into(home_trash, abs, abs, now);
    }

    char top[PATH_MAX];
    if (!xdg_trash_topdir(real_dir, fst.st_dev, top, sizeof top)) return false;
    usize tlen = strlen(top);
    const char *rel = abs + (strcmp(top, "/") ? tlen : 0);
    while (*rel == '/') rel++;
    char root[PATH_MAX];
    /* An administrator's shared $topdir/.Trash (sticky, a real directory)
     * holds one folder per user; otherwise the user's own .Trash-$uid. */
    char shared[PATH_MAX];
    struct stat ss;
    if (snprintf(shared, sizeof shared, "%s/.Trash", strcmp(top, "/") ? top : "")
            < (int)sizeof shared &&
        lstat(shared, &ss) == 0 && S_ISDIR(ss.st_mode) && (ss.st_mode & S_ISVTX) &&
        snprintf(root, sizeof root, "%s/%u", shared, (unsigned)uid) < (int)sizeof root &&
        xdg_trash_prepare(root) && xdg_trash_into(root, abs, rel, now))
        return true;
    if (snprintf(root, sizeof root, "%s/.Trash-%u", strcmp(top, "/") ? top : "",
                 (unsigned)uid) >= (int)sizeof root)
        return false;
    if (strncmp(abs, root, strlen(root)) == 0) return false;
    return xdg_trash_prepare(root) && xdg_trash_into(root, abs, rel, now);
}

#endif
