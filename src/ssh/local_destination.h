/* Local transfer destinations: hold directories, and never follow a link the
 * transfer itself named (the folder the user chose is theirs to link). */
#ifndef LIU_LOCAL_DESTINATION_H
#define LIU_LOCAL_DESTINATION_H
#include "core/types.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
extern long syscall(long, ...);
#endif

static inline bool local_destination_component(const char *name) {
    return name && name[0] && strcmp(name, ".") && strcmp(name, "..") &&
           !strchr(name, '/');
}

/* Walk `rel` down from the directory `root`, never through a link: every
 * component is opened O_NOFOLLOW, and with `create` a missing one is made
 * first. This is the rule for what a TRANSFER names — a remote listing's
 * sub-directories, the tree a recursive copy lays down — where a link already
 * sitting in the destination would send the bytes somewhere the user never
 * pointed at. An empty `rel` is `root` itself. The descriptor is the caller's. */
static inline int local_destination_subdir(int root, const char *rel, bool create) {
    if (root < 0 || !rel || strlen(rel) >= 4096) { errno = EINVAL; return -1; }
    char copy[4096];
    memcpy(copy, rel, strlen(rel) + 1);
    int fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *p = copy;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *end = strchr(p, '/');
        if (end) *end = '\0';
        if (strcmp(p, ".") != 0) {
            if (!local_destination_component(p)) { close(fd); errno = EINVAL; return -1; }
            if (create && mkdirat(fd, p, 0755) != 0 && errno != EEXIST) {
                close(fd); return -1;
            }
            int next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) { close(fd); return -1; }
            close(fd);
            fd = next;
        }
        if (!end) break;
        p = end + 1;
    }
    return fd;
}

/* The directory the USER named — the folder a paste lands in, a download's
 * folder, the root of a sync. It is opened the way every other program opens
 * it: links in its path are followed.
 *
 * Refusing a link in ANY component, the user's own included, would refuse most
 * of a Mac: /tmp, /var and /etc are links there (so is every $TMPDIR), and so
 * is a project reached through a linked folder in the home directory. Who
 * chose the path is what matters, not whether a link is in it — the transfer's
 * own names go through local_destination_subdir and the no-follow writer
 * below, and those never follow one.
 *
 * With `create`, the part of the path that does not exist yet is made, and
 * made under the same no-follow rule (a directory this call just created
 * cannot be a link unless someone raced it in, and then it is refused). */
static inline int local_destination_dir(const char *path, bool create) {
    if (!path || !*path || strlen(path) >= 4096) { errno = EINVAL; return -1; }
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0 || !create || errno != ENOENT) return fd;

    /* The deepest ancestor that exists, then the rest beneath it. */
    char copy[4096];
    memcpy(copy, path, strlen(path) + 1);
    usize cut = strlen(copy);
    int base = -1;
    while (base < 0) {
        while (cut > 0 && copy[cut - 1] == '/') cut--;       /* trailing slashes */
        while (cut > 0 && copy[cut - 1] != '/') cut--;       /* drop one component */
        if (cut == 0) {
            base = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (base < 0) return -1;
            break;
        }
        char saved = copy[cut];
        copy[cut] = '\0';
        base = open(copy, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        copy[cut] = saved;
        if (base < 0 && errno != ENOENT) return -1;
    }
    fd = local_destination_subdir(base, path + cut, true);
    close(base);
    return fd;
}

static inline int local_destination_parent(const char *path, char *name, usize cap) {
    if (!path || !*path || strlen(path) >= 4096) { errno = EINVAL; return -1; }
    char copy[4096];
    memcpy(copy, path, strlen(path) + 1);
    char *slash = strrchr(copy, '/');
    const char *leaf = slash ? slash + 1 : copy;
    if (!local_destination_component(leaf) || strlen(leaf) >= cap) {
        errno = EINVAL; return -1;
    }
    memcpy(name, leaf, strlen(leaf) + 1);
    if (!slash) return local_destination_dir(".", false);
    if (slash == copy) slash[1] = '\0';
    else *slash = '\0';
    return local_destination_dir(copy, false);
}

typedef struct {
    int parent;
    FILE *stream;
    bool exclusive;
    char name[NAME_MAX + 1];
    char temp[80];
} LocalDestinationWriter;

/* Stage bytes in a unique sibling. A replacement does not truncate an old
 * regular file or follow a hard link; exclusive paste commits atomically.
 *
 * `new_mode` is the permission a file that does not exist yet is CREATED with
 * (the kernel applies the umask, as it does for any creat): a copy passes its
 * source's rwx bits so an executable stays one. Only the low nine bits are
 * used — setuid, setgid and sticky are never carried to a new file. A file
 * that already exists keeps its own mode. */
static inline bool local_destination_begin_mode(LocalDestinationWriter *w, int parent,
                                                const char *name, bool exclusive,
                                                mode_t new_mode) {
    if (!w || parent < 0 || !local_destination_component(name) ||
        strlen(name) >= sizeof w->name) return false;
    memset(w, 0, sizeof *w);
    w->parent = parent;
    w->exclusive = exclusive;
    memcpy(w->name, name, strlen(name) + 1);
    struct stat old;
    bool exists = fstatat(parent, name, &old, AT_SYMLINK_NOFOLLOW) == 0;
    if (exists && (exclusive || !S_ISREG(old.st_mode))) { errno = EEXIST; return false; }
    if (!exists && errno != ENOENT) return false;
    static _Atomic u32 seq;
    int fd = -1;
    for (i32 attempt = 0; attempt < 128; attempt++) {
        u32 id = atomic_fetch_add_explicit(&seq, 1, memory_order_relaxed);
        snprintf(w->temp, sizeof w->temp, ".liu-part-%ld-%u", (long)getpid(), id);
        fd = openat(parent, w->temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    exists ? 0600 : (new_mode & 0777));
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return false;
    if (exists && fchmod(fd, old.st_mode & 0777) != 0) {
        close(fd); unlinkat(parent, w->temp, 0); return false;
    }
    w->stream = fdopen(fd, "wb");
    if (!w->stream) { close(fd); unlinkat(parent, w->temp, 0); return false; }
    return true;
}

static inline bool local_destination_begin(LocalDestinationWriter *w, int parent,
                                            const char *name, bool exclusive) {
    return local_destination_begin_mode(w, parent, name, exclusive, 0666);
}

static inline bool local_destination_finish(LocalDestinationWriter *w, bool ok) {
    if (!w || !w->stream) return false;
    if (ok && (fflush(w->stream) != 0 || fsync(fileno(w->stream)) != 0)) ok = false;
    if (fclose(w->stream) != 0) ok = false;
    w->stream = NULL;
    if (ok) {
        if (w->exclusive) {
            /* Native no-replace rename also works on filesystems without hard
             * links. The fallback stays atomic, including dangling links. */
#if defined(__APPLE__) && defined(RENAME_EXCL)
            int rc = renameatx_np(w->parent, w->temp, w->parent, w->name, RENAME_EXCL);
            if (rc == 0) ok = true;
            else if (errno == ENOTSUP || errno == EINVAL)
                ok = linkat(w->parent, w->temp, w->parent, w->name, 0) == 0;
            else ok = false;
#elif defined(__linux__) && defined(SYS_renameat2)
            long rc = syscall(SYS_renameat2, w->parent, w->temp, w->parent, w->name, 1u);
            if (rc == 0) ok = true;
            else if (errno == ENOSYS || errno == ENOTSUP || errno == EINVAL)
                ok = linkat(w->parent, w->temp, w->parent, w->name, 0) == 0;
            else ok = false;
#else
            ok = linkat(w->parent, w->temp, w->parent, w->name, 0) == 0;
#endif
        } else {
            struct stat st;
            int rc = fstatat(w->parent, w->name, &st, AT_SYMLINK_NOFOLLOW);
            ok = (rc == 0 ? S_ISREG(st.st_mode) : errno == ENOENT) &&
                 renameat(w->parent, w->temp, w->parent, w->name) == 0;
        }
    }
    if (unlinkat(w->parent, w->temp, 0) != 0 && errno != ENOENT) ok = false;
    return ok;
}

/* Open an existing or new resume target without truncating it before proving
 * that the descriptor is regular. O_NONBLOCK prevents a FIFO open from hanging. */
static inline int local_destination_resume(int parent, const char *name, struct stat *st) {
    if (!local_destination_component(name)) { errno = EINVAL; return -1; }
    int fd = openat(parent, name, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0666);
    if (fd < 0) return -1;
    if (fstat(fd, st) != 0 || !S_ISREG(st->st_mode) || st->st_size < 0) {
        close(fd); errno = EINVAL; return -1;
    }
    return fd;
}
#endif
