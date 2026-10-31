/*
 * Liu - crash-safe writes for small persistent state files.
 */
#include "core/atomic_file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <copyfile.h>
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

static bool atomic_file_ensure_dir_chain(char *path, mode_t create_mode) {
    char *p = path;
    if (*p == '/') p++;
    for (; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (path[0]) {
            struct stat st;
            if (mkdir(path, create_mode) != 0 && errno != EEXIST) {
                *p = '/';
                return false;
            }
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                *p = '/';
                return false;
            }
        }
        *p = '/';
    }
    return true;
}

static bool atomic_file_prepare_parent(const char *path, bool private_parent) {
    char parent[ATOMIC_FILE_PATH_CAP];
    int n = snprintf(parent, sizeof(parent), "%s", path);
    if (n < 0 || (usize)n >= sizeof(parent)) return false;

    char *slash = strrchr(parent, '/');
    if (!slash) return true;
    if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }

    mode_t create_mode = private_parent ? 0700 : 0755;
    if (!atomic_file_ensure_dir_chain(parent, create_mode)) return false;
    if (mkdir(parent, create_mode) != 0 && errno != EEXIST) return false;

    struct stat st;
    int stat_rc = private_parent ? lstat(parent, &st) : stat(parent, &st);
    if (stat_rc != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    if (private_parent) {
        if (st.st_uid != geteuid()) return false;
        /* Persistent state can contain API keys, hostnames, and paths. */
        if ((st.st_mode & 0777) != 0700 && chmod(parent, 0700) != 0)
            return false;
    }
    return true;
}

static bool atomic_file_begin_impl(AtomicFileWriter *writer, const char *path,
                                   bool private_file) {
    if (!writer || !path || !path[0]) return false;
    memset(writer, 0, sizeof(*writer));

    int n = snprintf(writer->final_path, sizeof(writer->final_path), "%s", path);
    if (n < 0 || (usize)n >= sizeof(writer->final_path)) return false;
    if (!atomic_file_prepare_parent(writer->final_path, private_file)) return false;

    n = snprintf(writer->temp_path, sizeof(writer->temp_path), "%s.tmp.XXXXXX",
                 writer->final_path);
    if (n < 0 || (usize)n >= sizeof(writer->temp_path)) {
        writer->temp_path[0] = '\0';
        return false;
    }

    int fd = mkstemp(writer->temp_path);
    if (fd < 0) {
        writer->temp_path[0] = '\0';
        return false;
    }
    mode_t file_mode = 0600;
    if (!private_file) {
        struct stat existing;
        if (stat(writer->final_path, &existing) == 0 &&
            S_ISREG(existing.st_mode)) {
            file_mode = existing.st_mode & 0777;
        }
    }
    if (fchmod(fd, file_mode) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd);
        unlink(writer->temp_path);
        writer->temp_path[0] = '\0';
        return false;
    }

    writer->stream = fdopen(fd, "w");
    if (!writer->stream) {
        close(fd);
        unlink(writer->temp_path);
        writer->temp_path[0] = '\0';
        return false;
    }
    return true;
}

bool atomic_file_begin(AtomicFileWriter *writer, const char *path) {
    return atomic_file_begin_impl(writer, path, false);
}

bool atomic_file_begin_personal(AtomicFileWriter *writer, const char *path) {
    if (!atomic_file_begin_impl(writer, path, false)) return false;
    struct stat old;
    /* A file that exists keeps the permission bits it has — it is the user's,
     * and neither widening nor narrowing it is the writer's call. A new one is
     * owner-only. */
    mode_t mode = 0600;
    if (lstat(path, &old) == 0 && S_ISREG(old.st_mode)) mode = old.st_mode & 0777;
    int fd = fileno(writer->stream);
    /* A new inode can inherit permissions not represented by mode bits. */
#ifdef __APPLE__
    acl_t empty = acl_init(0);
    if (!empty) { atomic_file_abort(writer); return false; }
    int rc = acl_set_fd_np(fd, empty, ACL_TYPE_EXTENDED);
    int saved_errno = errno;
    acl_free(empty);
    if (rc != 0 && saved_errno != ENOTSUP && saved_errno != EOPNOTSUPP) {
        atomic_file_abort(writer); return false;
    }
#elif defined(__linux__)
    if (fremovexattr(fd, "system.posix_acl_access") != 0 &&
        errno != ENODATA && errno != ENOTSUP && errno != EOPNOTSUPP) {
        atomic_file_abort(writer); return false;
    }
#endif
    if (fchmod(fd, mode) != 0) {
        atomic_file_abort(writer); return false;
    }
    return true;
}

bool atomic_file_begin_private(AtomicFileWriter *writer, const char *path) {
    return atomic_file_begin_impl(writer, path, true);
}

bool atomic_file_write_path(const char *path, char *out, usize cap, bool *is_link) {
    if (is_link) *is_link = false;
    if (!path || !out || cap == 0) return false;
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        char real[PATH_MAX];
        if (realpath(path, real)) {
            if (is_link) *is_link = true;
            return (usize)snprintf(out, cap, "%s", real) < cap;
        }
        /* A dangling link has no file to preserve: the write replaces it.
         * Anything else (a loop, a directory that may not be searched) is a
         * link that cannot be followed, and guessing would mean replacing it.
         */
        if (errno != ENOENT) return false;
    }
    return (usize)snprintf(out, cap, "%s", path) < cap;
}

bool atomic_file_begin_private_through(AtomicFileWriter *writer, const char *path) {
    if (!writer) return false;
    memset(writer, 0, sizeof *writer);
    char target[ATOMIC_FILE_PATH_CAP];
    bool is_link = false;
    if (!atomic_file_write_path(path, target, sizeof target, &is_link)) return false;
    return is_link ? atomic_file_begin_personal(writer, target)
                   : atomic_file_begin_private(writer, path);
}

void atomic_file_abort(AtomicFileWriter *writer) {
    if (!writer) return;
    if (writer->stream) {
        fclose(writer->stream);
        writer->stream = NULL;
    }
    if (writer->temp_path[0]) {
        unlink(writer->temp_path);
        writer->temp_path[0] = '\0';
    }
}

/* ATOMIC_KEEP_METADATA: give the new inode (open at `fd`) what the file it is
 * about to replace has beyond its mode, which begin already carried — owner
 * and group, extended attributes, ACL. Returns whether the new inode ends up
 * with the old one's owner AND group; the rest is best effort, because a save
 * that cannot carry a Finder tag is still a save.
 *
 * Nothing here touches the new file's data or its mtime, so the identity the
 * commit records afterwards is still the identity of the bytes written. */
static bool atomic_file_carry_metadata(int fd, const char *old_path, const struct stat *old) {
    struct stat cur;
    bool owner_kept = fstat(fd, &cur) == 0;
    if (owner_kept && (cur.st_uid != old->st_uid || cur.st_gid != old->st_gid)) {
        /* Both at once is what only root (or the owner, within its groups)
         * may do; the group alone is still worth having when the owner is
         * already right. */
        if (fchown(fd, old->st_uid, old->st_gid) != 0) {
            if (cur.st_uid != old->st_uid) owner_kept = false;
            else if (fchown(fd, (uid_t)-1, old->st_gid) != 0) owner_kept = false;
        }
        /* A chown clears setuid/setgid and may be refused its mode: put the
         * bits back as begin set them. */
        (void)fchmod(fd, cur.st_mode & 07777);
    }
#ifdef __APPLE__
    int src = open(old_path, O_RDONLY | O_CLOEXEC);
    if (src >= 0) {
        (void)fcopyfile(src, fd, NULL, COPYFILE_XATTR | COPYFILE_ACL);
        close(src);
    }
#else
    (void)old_path;                     /* extended attributes: macOS only so far */
#endif
    return owner_kept;
}

/* ATOMIC_KEEP_LINKS: put the finished temporary file's bytes (open at `src`,
 * flushed) into the EXISTING file at `path`, keeping its inode — and so its
 * other names, its owner and everything else an inode carries. The bytes go
 * down first and the length is cut afterwards, so a failure part-way never
 * leaves the file shorter than both versions. `*out` gets the result's stat. */
static bool atomic_file_write_in_place(int src, const char *path, struct stat *out) {
    int dst = open(path, O_WRONLY | O_CLOEXEC);
    if (dst < 0) return false;
    bool ok = lseek(src, 0, SEEK_SET) == 0;
    off_t total = 0;
    char buf[65536];
    while (ok) {
        ssize_t n = read(src, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
        if (n == 0) break;
        for (ssize_t done = 0; done < n; ) {
            ssize_t w = write(dst, buf + done, (usize)(n - done));
            if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
            done += w;
        }
        total += n;
    }
    if (ok && ftruncate(dst, total) != 0) ok = false;
    if (ok && fsync(dst) != 0) ok = false;
    if (ok && fstat(dst, out) != 0) ok = false;
    int saved = errno;
    if (close(dst) != 0 && ok) { ok = false; saved = errno; }
    errno = saved;
    return ok;
}

bool atomic_file_commit(AtomicFileWriter *writer) {
    if (!writer || !writer->stream || !writer->temp_path[0] ||
        !writer->final_path[0]) {
        return false;
    }

    FILE *stream = writer->stream;
    int fd = fileno(stream);
    bool ok = ferror(stream) == 0;
    if (ok && fflush(stream) != 0) ok = false;

    /* Apply the configured keep policy to the existing file at commit time.
     * Writers with keep == 0 skip this step and commit the temporary file
     * with rename.
     */
    bool in_place = false;
    struct stat old;
    if (ok && writer->keep && stat(writer->final_path, &old) == 0 && S_ISREG(old.st_mode)) {
        if ((writer->keep & ATOMIC_KEEP_READONLY) &&
            faccessat(AT_FDCWD, writer->final_path, W_OK, AT_EACCESS) != 0) {
            /* The user took write permission away from this file. A rename
             * would not have noticed — it asks the directory. */
            atomic_file_abort(writer);
            writer->refused_readonly = true;
            errno = EACCES;
            return false;
        }
        bool owner_kept = true;
        if (writer->keep & ATOMIC_KEEP_METADATA)
            owner_kept = atomic_file_carry_metadata(fd, writer->final_path, &old);
        in_place = (writer->keep & ATOMIC_KEEP_LINKS) && (old.st_nlink > 1 || !owner_kept);
    }

    if (ok && !writer->no_sync && fsync(fd) != 0) ok = false;

    /* Identity of the bytes written, read off the descriptor while it is
     * still open. Taking it from the destination PATH after the rename
     * instead would race any other writer that replaces the file in between,
     * and would then attribute that writer's file to this one. rename(2)
     * changes only the directory entry, so the inode, size and mtime
     * captured here are what a later stat() of the final path reports. */
    struct stat committed;
    bool have_stat = ok && fstat(fd, &committed) == 0;

    if (ok && in_place) {
        if (atomic_file_write_in_place(fd, writer->final_path, &committed)) {
            fclose(stream);
            writer->stream = NULL;
            unlink(writer->temp_path);
            writer->temp_path[0] = '\0';
            writer->wrote_in_place = true;
            writer->committed_st = committed;
            writer->committed_valid = true;
            return true;
        }
        /* A file that cannot be opened for writing, and a caller that did
         * not ask for ATOMIC_KEEP_READONLY: it is forcing the save, and the
         * rename below is the only way left to make it. Any other failure
         * is the save's failure. */
        if (errno != EACCES || (writer->keep & ATOMIC_KEEP_READONLY)) ok = false;
        else have_stat = fstat(fd, &committed) == 0;
    }

    int failed_with = errno;
    if (fclose(stream) != 0) { if (ok) failed_with = errno; ok = false; }
    writer->stream = NULL;

    if (ok && rename(writer->temp_path, writer->final_path) != 0) { failed_with = errno; ok = false; }
    if (!ok) {
        unlink(writer->temp_path);
        writer->temp_path[0] = '\0';
        errno = failed_with;             /* the reason, not the cleanup's */
        return false;
    }

    /* Persist the directory entry too. A failure here cannot safely undo the
     * already-visible rename, but it still tells the caller durability was not
     * guaranteed. */
    char parent[ATOMIC_FILE_PATH_CAP];
    int n = writer->no_sync ? -1 : snprintf(parent, sizeof(parent), "%s", writer->final_path);
    if (n > 0 && (usize)n < sizeof(parent)) {
        char *slash = strrchr(parent, '/');
        if (slash) {
            if (slash == parent) parent[1] = '\0';
            else *slash = '\0';
            int dir_fd = open(parent, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
            if (dir_fd >= 0) {
                if (fsync(dir_fd) != 0) ok = false;
                close(dir_fd);
            } else {
                ok = false;
            }
        }
    }
    writer->temp_path[0] = '\0';
    /* Published only for a fully successful commit: a caller must never be
     * able to record the identity of a write that did not land. */
    if (ok && have_stat) {
        writer->committed_st = committed;
        writer->committed_valid = true;
    }
    return ok;
}

bool atomic_file_holds_private(const char *path, const void *data, usize len) {
    if (!path || !path[0] || (!data && len > 0)) return false;
    /* O_NONBLOCK: a FIFO at the path opens at once and fails the S_ISREG test
     * instead of holding the caller until something writes to it. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool same = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
                (st.st_mode & 0777) == 0600 && st.st_size >= 0 && (u64)st.st_size == (u64)len;
    const u8 *want = (const u8 *)data;
    usize at = 0;
    u8 chunk[16384];
    while (same && at < len) {
        usize ask = len - at < sizeof chunk ? len - at : sizeof chunk;
        ssize_t n = read(fd, chunk, ask);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || memcmp(chunk, want + at, (usize)n) != 0) { same = false; break; }
        at += (usize)n;
    }
    /* Nothing past `len`: a file that grew after the fstat is not the same. */
    if (same) {
        u8 extra;
        ssize_t n;
        do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
        if (n != 0) same = false;
    }
    close(fd);
    return same;
}

FILE *atomic_file_open_regular(const char *path) {
    if (!path || !path[0]) { errno = ENOENT; return NULL; }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        int e = errno;
        close(fd);
        errno = e;
    }
    return f;
}
