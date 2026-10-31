/*
 * Liu - file operations (upload, download, delete, rename)
 * Works for both local filesystem and SFTP.
 */
#include "ui/filebrowser.h"
#include "core/checked.h"
#include "ssh/ssh_session.h"
#include "ssh/local_destination.h"
#include "ui/file_ref.h"
#include "ssh/sftp.h"       /* transfer progress (overlay UX) */
#include "ssh/transfer_mode.h" /* a copied file keeps its permission bits */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/param.h>
#ifdef __linux__
#include <sys/syscall.h>
extern long syscall(long, ...);
#endif

#include <stdatomic.h>
#include <libssh2.h>
#include <libssh2_sftp.h>

#define FB_RECURSIVE_CHILD_LIMIT 65536

static bool fileops_join(char *out, usize cap, const char *dir, const char *name) {
    if (!out || cap == 0 || !dir || !name) return false;
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return n >= 0 && (usize)n < cap;
}

static bool fileops_grow(void **base, i32 count, i32 *cap, usize elem_size) {
    if (!base || !cap || count < 0 || *cap < 0 || count < *cap) return count < *cap;
    if (*cap >= FB_RECURSIVE_CHILD_LIMIT) return false;
    i32 next = *cap ? (*cap > FB_RECURSIVE_CHILD_LIMIT / 2
                         ? FB_RECURSIVE_CHILD_LIMIT : *cap * 2) : 32;
    usize bytes;
    if (!checked_mul_usize((usize)next, elem_size, &bytes)) return false;
    void *grown = realloc(*base, bytes);
    if (!grown) return false;
    *base = grown;
    *cap = next;
    return true;
}

/* SFTP download: remote_path → local_dir/filename */
bool fb_download_file(FileBrowser *fb, i32 index, const char *local_dir) {
    if (!fb || !local_dir || index < 0 || index >= fb->entry_count) return false;
    if (fb->source != FB_SOURCE_SFTP || !fb->sftp_handle) return false;
    FileEntry *e = &fb->entries[index];
    if (e->is_dir || !sftp_name_is_safe_component(e->name)) return false;
    int parent = local_destination_dir(local_dir, false);
    if (parent < 0) return false;
    char remote_path[FB_MAX_PATH * 2];
    fb_entry_path(fb, e, remote_path, sizeof remote_path);
    Session *owner = (Session *)fb->session;
    session_sftp_scope_begin(owner);
    bool ok = sftp_download_at(fb->sftp_handle, remote_path, parent, e->name, true) == 0;
    session_sftp_scope_end(owner);
    close(parent);
    return ok;
}

/* SFTP upload: local_path → remote cwd */
bool fb_upload_file(FileBrowser *fb, const char *local_path) {
    if (!fb || !local_path || fb->source != FB_SOURCE_SFTP || !fb->sftp_handle) return false;

    const char *fname = strrchr(local_path, '/');
    fname = fname ? fname + 1 : local_path;

    char directory[FB_MAX_PATH];
    fb_operation_dir(fb, directory, sizeof directory);
    if (!directory[0]) return false;
    char remote_path[2048];
    if (!fileops_join(remote_path, sizeof(remote_path), directory, fname)) return false;

    FILE *f = fopen(local_path, "rb");
    if (!f) return false;
    struct stat local_st;
    bool local_known = fstat(fileno(f), &local_st) == 0;

    Session *owner = (Session *)fb->session;
    session_sftp_scope_begin(owner);
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)fb->sftp_handle;
    LIBSSH2_SFTP_HANDLE *handle;
    /* The local file's bits, as sftp_upload creates a file (ssh/transfer_mode.h). */
    SFTP_CALL_PTR(handle, libssh2_sftp_open(sftp, remote_path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        (long)transfer_create_mode(local_known, (u32)local_st.st_mode,
                                   LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                                   LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH)));
    if (!handle) {
        fclose(f);
        session_sftp_scope_end(owner);
        return false;
    }

    char buf[32768];
    bool ok = true;
    for (;;) {
        size_t nread = fread(buf, 1, sizeof(buf), f);
        bool read_error = ferror(f) != 0;
        ssize_t total = 0;
        while ((size_t)total < nread) {
            ssize_t n;
            SFTP_CALL(n, libssh2_sftp_write(handle, buf + total, nread - (size_t)total));
            if (n <= 0) {
                ok = false;
                break;
            }
            total += n;
        }
        if (!ok) break;
        if (read_error) { ok = false; break; }
        if (nread < sizeof(buf)) break;
    }
    if (fclose(f) != 0) ok = false;
    int crc;
    SFTP_CALL(crc, libssh2_sftp_close(handle));
    if (crc != 0) ok = false;
    if (!ok) { int urc; SFTP_CALL(urc, libssh2_sftp_unlink(sftp, remote_path)); (void)urc; }
    session_sftp_scope_end(owner);
    return ok;
}

/* Delete file (local or SFTP) */
bool fb_delete_file(FileBrowser *fb, i32 index) {
    if (index < 0 || index >= fb->entry_count) return false;
    FileEntry *e = &fb->entries[index];
    if (strcmp(e->name, "..") == 0) return false;
    char path[FB_MAX_PATH * 2];
    fb_entry_path(fb, e, path, sizeof(path));

    bool ok = false;
    if (fb->source == FB_SOURCE_SFTP && fb->sftp_handle) {
        Session *owner = (Session *)fb->session;
        session_sftp_scope_begin(owner);
        LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)fb->sftp_handle;
        int rc;
        if (e->is_dir) SFTP_CALL(rc, libssh2_sftp_rmdir(sftp, path));
        else           SFTP_CALL(rc, libssh2_sftp_unlink(sftp, path));
        ok = rc == 0;
        session_sftp_scope_end(owner);
    } else {
        if (e->is_dir) {
            ok = rmdir(path) == 0;
            /* A link to a folder is listed as a folder and removed as a
             * LINK: rmdir does not follow it (ENOTDIR), and what it points
             * at keeps everything in it. */
            if (!ok && errno == ENOTDIR) ok = unlink(path) == 0;
        } else {
            ok = unlink(path) == 0;
        }
        /* A document open from that path has no file any more. */
        if (ok) fb_view_path_gone(fb, path);
    }

    /* Nothing to select — the row is gone — but the list stays where it was. */
    if (ok) fb_refresh_keep(fb, "");
    return ok;
}

/* Create directory (local or SFTP) */
bool fb_mkdir(FileBrowser *fb, const char *name) {
    if (!fb || !name || !name[0]) return false;
    char directory[FB_MAX_PATH];
    fb_operation_dir(fb, directory, sizeof directory);
    if (!directory[0]) return false;
    char path[2048];
    if (!fileops_join(path, sizeof(path), directory, name)) return false;

    bool ok = false;
    if (fb->source == FB_SOURCE_SFTP && fb->sftp_handle) {
        Session *owner = (Session *)fb->session;
        session_sftp_scope_begin(owner);
        LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)fb->sftp_handle;
        int rc;
        SFTP_CALL(rc, libssh2_sftp_mkdir(sftp, path,
            LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP));
        ok = rc == 0;
        session_sftp_scope_end(owner);
    } else {
        ok = mkdir(path, 0755) == 0;
    }

    if (ok) fb_refresh_keep(fb, path);      /* and select what was made */
    return ok;
}

/* Create an empty file in the current directory (local or SFTP). Fails if a
 * file with that name already exists so an existing file is never truncated. */
bool fb_create_file(FileBrowser *fb, const char *name) {
    if (!fb || !name || !name[0]) return false;
    char directory[FB_MAX_PATH];
    fb_operation_dir(fb, directory, sizeof directory);
    if (!directory[0]) return false;
    char path[2048];
    if (!fileops_join(path, sizeof(path), directory, name)) return false;

    bool ok = false;
    if (fb->source == FB_SOURCE_SFTP && fb->sftp_handle) {
        Session *owner = (Session *)fb->session;
        session_sftp_scope_begin(owner);
        LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)fb->sftp_handle;
        LIBSSH2_SFTP_HANDLE *h;
        SFTP_CALL_PTR(h, libssh2_sftp_open(sftp, path,
            LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL,
            LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
            LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH));
        if (h) {
            int crc;
            SFTP_CALL(crc, libssh2_sftp_close(h));
            (void)crc;
            ok = true;
        }
        session_sftp_scope_end(owner);
    } else {
        /* O_EXCL via fopen "wx" — refuse to clobber an existing file. */
        FILE *f = fopen(path, "wx");
        if (f) { fclose(f); ok = true; }
    }

    if (ok) fb_refresh_keep(fb, path);
    return ok;
}

/* On macOS, `renamex_np(RENAME_EXCL)` prevents replacing an existing
 * destination atomically. On unsupported filesystems and Linux, the
 * preceding existence check is used, leaving a race if another process
 * creates the path before `rename`. A case-only rename is allowed when
 * source and destination identify the same file. */
bool fb_rename_local_noreplace(const char *from, const char *to) {
    if (!from || !to || !from[0] || !to[0]) { errno = EINVAL; return false; }
    struct stat a, b;
    if (lstat(from, &a) != 0) return false;
    if (lstat(to, &b) == 0) {
        if (a.st_dev == b.st_dev && a.st_ino == b.st_ino) return rename(from, to) == 0;
        errno = EEXIST;
        return false;
    }
#if defined(__APPLE__) && defined(RENAME_EXCL)
    if (renamex_np(from, to, RENAME_EXCL) == 0) return true;
    if (errno != ENOTSUP && errno != EINVAL) return false;
#endif
    return rename(from, to) == 0;
}

/* Rename file (local or SFTP). A local rename never replaces another file;
 * on failure errno says why. */
bool fb_rename(FileBrowser *fb, i32 index, const char *new_name) {
    if (!fb || !new_name || !new_name[0] || index < 0 || index >= fb->entry_count)
        return false;
    FileEntry *e = &fb->entries[index];
    char old_path[FB_MAX_PATH * 2];
    fb_entry_path(fb, e, old_path, sizeof(old_path));

    char parent[sizeof old_path];
    snprintf(parent, sizeof parent, "%s", old_path);
    char *slash = strrchr(parent, '/');
    if (!slash) return false;
    if (slash == parent) parent[1] = '\0'; else *slash = '\0';
    char new_path[2048];
    if (!fileops_join(new_path, sizeof(new_path), parent, new_name)) return false;

    bool ok = false;
    if (fb->source == FB_SOURCE_SFTP && fb->sftp_handle) {
        Session *owner = (Session *)fb->session;
        session_sftp_scope_begin(owner);
        LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)fb->sftp_handle;
        int rc;
        SFTP_CALL(rc, libssh2_sftp_rename(sftp, old_path, new_path));
        ok = rc == 0;
        session_sftp_scope_end(owner);
    } else {
        ok = fb_rename_local_noreplace(old_path, new_path);
        /* The document open from the old name — or from inside a folder
         * that was renamed — goes on under the new one, so a save after the
         * rename writes the file that is there. */
        if (ok) fb_view_path_moved(fb, old_path, new_path);
    }

    if (ok) fb_refresh_keep(fb, new_path);  /* the row under its new name */
    return ok;
}

/* Open the source with `O_NONBLOCK`, then use `fstat` on that descriptor for
 * its type and permissions. This avoids a path/open race and prevents opening a
 * FIFO from waiting for a writer. The destination uses the source's rwx bits as
 * its creation mode, so the process umask applies. */

/* A regular file, opened. `dir` + `name` name it (AT_FDCWD + a path for the
 * item the user picked). `follow` is false for an entry found inside a tree —
 * the caller has lstat'ed it and decides about links itself. Returns -1 when
 * it cannot be opened or is not a regular file. */
static int fileops_open_regular(int dir, const char *name, bool follow, struct stat *st) {
    int fd = openat(dir, name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW));
    if (fd < 0) return -1;
    if (fstat(fd, st) != 0 || !S_ISREG(st->st_mode)) { close(fd); errno = EINVAL; return -1; }
    return fd;
}

/* Copy an opened regular file to `parent`/`name` (never replacing). Takes `fd`. */
static bool copy_local_stream(int fd, const struct stat *st, int parent, const char *name) {
    FILE *in = fdopen(fd, "rb");
    if (!in) { close(fd); return false; }
    LocalDestinationWriter writer;
    if (!local_destination_begin_mode(&writer, parent, name, true, st->st_mode & 0777)) {
        fclose(in);
        return false;
    }
    char buf[8192];
    bool ok = true;
    for (;;) {
        usize n = fread(buf, 1, sizeof buf, in);
        if (fwrite(buf, 1, n, writer.stream) != n || ferror(in)) { ok = false; break; }
        if (n < sizeof buf) break;
    }
    if (fclose(in) != 0) ok = false;
    return local_destination_finish(&writer, ok);
}

static bool copy_local_file_at(const char *src, int parent, const char *name) {
    struct stat st;
    /* Follow a symbolic link when it is the selected source item. Links found
     * inside recursive walks follow the walk's separate traversal policy. */
    int fd = fileops_open_regular(AT_FDCWD, src, true, &st);
    if (fd < 0) return false;
    return copy_local_stream(fd, &st, parent, name);
}

bool fb_copy_local(const char *src, const char *dst) {
    if (!src || !dst) return false;
    char name[NAME_MAX + 1];
    int parent = local_destination_parent(dst, name, sizeof name);
    if (parent < 0) return false;
    bool ok = copy_local_file_at(src, parent, name);
    close(parent);
    return ok;
}

/* =========================================================================
 * Clipboard paste
 *
 * Handles all 4 source×dest combos, including recursive directory transfers.
 * Transfers are synchronous on the main thread — matches the single-file
 * upload/download helpers above. Large trees block the UI; moving them to
 * a worker thread is the next step, not this one.
 * ========================================================================= */

/* Both directions delegate to the single transfer implementation in
 * src/ssh/sftp.c: a download checks fclose (a flush error at close is a
 * failure, not a truncated file reported as success), and an upload that
 * replaces a remote file is staged through a sibling temp path, so a
 * transfer that fails part-way keeps the file it was replacing. One
 * implementation keeps the two directions from drifting apart. The scope
 * (blocking mode + session io_lock) stays here because it is per-Session
 * and sftp.c only ever sees the raw handle. */
static bool sftp_download_to_local_at(Session *src_session, LIBSSH2_SFTP *src_sftp,
                                      const char *remote_path, int parent, const char *name) {
    session_sftp_scope_begin(src_session);
    bool ok = sftp_download_at(src_sftp, remote_path, parent, name, true) == 0;
    session_sftp_scope_end(src_session);
    return ok;
}

static bool sftp_download_to_local(Session *src_session, LIBSSH2_SFTP *src_sftp,
                                   const char *remote_path, const char *local_path) {
    char name[NAME_MAX + 1];
    int parent = local_destination_parent(local_path, name, sizeof name);
    if (parent < 0) return false;
    bool ok = sftp_download_to_local_at(src_session, src_sftp, remote_path, parent, name);
    close(parent);
    return ok;
}

/* Send the regular file open at `fd` (taken) to `remote_path`. `label` names
 * it in the progress line. */
static bool sftp_upload_opened(Session *dst_session, LIBSSH2_SFTP *dst_sftp, int fd,
                               const char *label, const char *remote_path) {
    session_sftp_scope_begin(dst_session);
    bool ok = sftp_upload_fd(dst_sftp, fd, label, remote_path) == 0;
    session_sftp_scope_end(dst_session);
    return ok;
}

static bool sftp_upload_from_local(Session *dst_session, LIBSSH2_SFTP *dst_sftp,
                                   const char *local_path, const char *remote_path) {
    /* Read only regular files and links to regular files, and send the file
     * that was opened and checked. Opening with `O_NONBLOCK` rejects a FIFO
     * without waiting for a writer while the session I/O scope is held;
     * otherwise the SSH pane and later transfers could remain blocked. */
    struct stat st;
    int fd = fileops_open_regular(AT_FDCWD, local_path, true, &st);
    if (fd < 0) return false;
    return sftp_upload_opened(dst_session, dst_sftp, fd, local_path, remote_path);
}

/* Recursive walkers run on the paste worker's stack (FB_PASTE_WORKER_STACK,
 * filebrowser.h). Two path buffers make a frame of about 6.5 KiB; the depth
 * limit of 64 keeps the deepest walk near 430 KiB before the file copy and
 * libssh2 below it.
 *
 * Local entry types are checked with `lstat`. SFTP stat follows symbolic
 * links, so recursive walks do not traverse links found inside the tree. Links
 * to regular files are copied as file contents; other links are skipped and
 * make the copy incomplete, so a move retains its source. Copy follows the
 * selected top-level link. Delete unlinks links without traversing them. */
#define FILEOPS_MAX_DEPTH 64

/* Entries a remote walk left behind on purpose (a link to a folder, a dangling
 * link, a device or FIFO). One paste runs at a time, so a plain counter the
 * caller drains is enough; it is what turns "Paste had errors" into a reason. */
static _Atomic i32 g_fileops_skipped;

i32 fb_paste_take_skipped(void) {
    return atomic_exchange(&g_fileops_skipped, 0);
}

/* --- Recursive local copy --------------------------------------------- */

/* Walk parent descriptors from dir and compare device/inode pairs to determine
 * whether it is the target directory or a descendant. This avoids path-prefix
 * and symlink ambiguity. If traversal cannot prove the directory is outside,
 * treat it as inside so the caller refuses the write. */
static bool fileops_dir_is_within(int dir, dev_t dev, ino_t ino) {
    struct stat cur;
    if (fstat(dir, &cur) != 0) return true;
    char up[3 * 256 + 4];
    usize len = 0;
    for (i32 level = 0; level < 256; level++) {
        if (cur.st_dev == dev && cur.st_ino == ino) return true;
        if (len) up[len++] = '/';
        up[len++] = '.'; up[len++] = '.';
        up[len] = '\0';
        struct stat parent;
        if (fstatat(dir, up, &parent, 0) != 0) return true;
        if (parent.st_dev == cur.st_dev && parent.st_ino == cur.st_ino) return false;  /* the root */
        cur = parent;
    }
    return true;
}

/* The directory a copy is being written INTO, so the walk can refuse to read
 * it back as part of its own source if something moves it there meanwhile. */
typedef struct {
    bool  have;
    dev_t dev;
    ino_t ino;
} LocalCopyRoot;

/* How a directory copy ended. LEFT_BEHIND is not an error to stop on — the
 * rest of the tree is still copied — but it is not success either. */
typedef enum { LOCAL_COPY_OK = 0, LOCAL_COPY_LEFT_BEHIND, LOCAL_COPY_FAILED } LocalCopyEnd;

/* Copy the directory open at `srcfd` (taken) to `parent`/`name`.
 *
 * Everything is named relative to a held directory descriptor — the entry that
 * was examined is the entry that is opened. What each kind of entry becomes:
 *
 *   regular file        copied, keeping its rwx bits
 *   directory           entered (never through a link); created u+rwx so its
 *                       own children can be written even when the source
 *                       folder was read-only
 *   link to a file      copied as that file
 *   anything else       left behind, counted (fb_paste_take_skipped) — a link
 *                       to a folder, a cycle, a dangling link, a FIFO, a
 *                       socket, a device
 *
 * A walk that left something behind reports failure, which is what keeps a
 * MOVE from deleting a source it only partly copied — but it carries on with
 * the rest, so one socket in a project folder does not stop the copy at that
 * entry. A real error (the disk, a permission) still stops it. */
static LocalCopyEnd copy_local_dir(int srcfd, const struct stat *st, int parent,
                                   const char *name, LocalCopyRoot *root, i32 depth) {
    if (depth > FILEOPS_MAX_DEPTH) { close(srcfd); errno = ELOOP; return LOCAL_COPY_FAILED; }
    if (mkdirat(parent, name, (st->st_mode & 0777) | 0700) != 0) {
        int why = errno;
        close(srcfd);
        errno = why;                     /* EEXIST: the name is taken — and is not ours */
        return LOCAL_COPY_FAILED;
    }
    int dst = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat dst_st;
    if (dst < 0 || fstat(dst, &dst_st) != 0) {
        int why = errno;
        if (dst >= 0) close(dst);
        close(srcfd);
        /* The directory just made is empty and nobody will fill it. */
        unlinkat(parent, name, AT_REMOVEDIR);
        errno = why;
        return LOCAL_COPY_FAILED;
    }
    if (!root->have) {
        root->have = true;
        root->dev = dst_st.st_dev;
        root->ino = dst_st.st_ino;
    }
    DIR *d = fdopendir(srcfd);
    if (!d) { close(dst); close(srcfd); return LOCAL_COPY_FAILED; }
    int sfd = dirfd(d);
    LocalCopyEnd end = LOCAL_COPY_OK;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!local_destination_component(de->d_name)) { end = LOCAL_COPY_FAILED; break; }
        struct stat cst;
        if (fstatat(sfd, de->d_name, &cst, AT_SYMLINK_NOFOLLOW) != 0) { end = LOCAL_COPY_FAILED; break; }

        if (S_ISDIR(cst.st_mode)) {
            /* The destination itself turning up inside the source. */
            if (cst.st_dev == root->dev && cst.st_ino == root->ino) continue;
            int cfd = openat(sfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat ost;
            if (cfd < 0 || fstat(cfd, &ost) != 0 ||
                ost.st_dev != cst.st_dev || ost.st_ino != cst.st_ino) {
                if (cfd >= 0) close(cfd);
                end = LOCAL_COPY_FAILED;
                break;
            }
            LocalCopyEnd child = copy_local_dir(cfd, &ost, dst, de->d_name, root, depth + 1);
            if (child == LOCAL_COPY_FAILED) { end = LOCAL_COPY_FAILED; break; }
            if (child == LOCAL_COPY_LEFT_BEHIND) end = LOCAL_COPY_LEFT_BEHIND;
            continue;
        }

        bool is_link = S_ISLNK(cst.st_mode);
        if (!S_ISREG(cst.st_mode) && !is_link) {
            atomic_fetch_add(&g_fileops_skipped, 1);     /* FIFO, socket, device */
            end = LOCAL_COPY_LEFT_BEHIND;
            continue;
        }
        struct stat fst;
        int fd = fileops_open_regular(sfd, de->d_name, is_link, &fst);
        if (fd < 0) {
            if (is_link) {               /* to a folder, to nothing, to itself */
                atomic_fetch_add(&g_fileops_skipped, 1);
                end = LOCAL_COPY_LEFT_BEHIND;
                continue;
            }
            end = LOCAL_COPY_FAILED;
            break;
        }
        if (!copy_local_stream(fd, &fst, dst, de->d_name)) { end = LOCAL_COPY_FAILED; break; }
    }
    int why = errno;                     /* the failure's, not the cleanup's */
    closedir(d);                         /* closes srcfd */
    close(dst);
    if (end == LOCAL_COPY_FAILED) errno = why;
    return end;
}

/* Remove the tree at `dir`/`name`: a directory this process made and was
 * filling. Everything is examined and removed relative to a held descriptor;
 * a link is unlinked, never entered. The directory is read again until a pass
 * removes nothing — a readdir that is being deleted under may skip entries. */
static bool remove_tree_at(int dir, const char *name, i32 depth) {
    if (depth > FILEOPS_MAX_DEPTH) return false;
    int fd = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return false; }
    bool removed;
    do {
        removed = false;
        rewinddir(d);
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            struct stat st;
            if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
            if (S_ISDIR(st.st_mode) ? remove_tree_at(fd, de->d_name, depth + 1)
                                    : unlinkat(fd, de->d_name, 0) == 0)
                removed = true;
        }
    } while (removed);
    closedir(d);                         /* closes fd */
    return unlinkat(dir, name, AT_REMOVEDIR) == 0;
}

/* Copy the file or folder at `src` to `dst`, never replacing.
 *
 * A folder copy that FAILS removes the tree it had begun, so no half tree is
 * left under the final name, looking like the copy and refused by the next
 * paste as something to "replace". Only what this copy made is removed: the
 * root exists only because mkdirat here created it (a name already taken
 * fails before anything is written), and it is removed only if the directory
 * there is still that one. A copy that merely left links behind is kept — it
 * is the copy, minus what was never going to be in it. */
static LocalCopyEnd copy_local_tree(const char *src, const char *dst) {
    char name[NAME_MAX + 1];
    int parent = local_destination_parent(dst, name, sizeof name);
    if (parent < 0) return LOCAL_COPY_FAILED;
    LocalCopyEnd end = LOCAL_COPY_FAILED;
    struct stat st;
    bool have = lstat(src, &st) == 0;
    bool through_link = false;
    if (have && S_ISLNK(st.st_mode)) {
        /* The item the user picked is a link: that link IS the request (the
         * rules above), so a folder behind it is copied as that folder,
         * just as a link to a folder is listed as a folder. Links met
         * INSIDE the tree are still never entered. */
        struct stat target;
        if (stat(src, &target) == 0 && S_ISDIR(target.st_mode)) { st = target; through_link = true; }
    }
    if (!have) {
        /* nothing to copy */
    } else if (!S_ISDIR(st.st_mode)) {
        end = copy_local_file_at(src, parent, name) ? LOCAL_COPY_OK : LOCAL_COPY_FAILED;
    } else if (fileops_dir_is_within(parent, st.st_dev, st.st_ino)) {
        /* Into itself, or under itself: refused BEFORE the first write. The
         * walk would otherwise make the destination inside the source and then
         * meet it as a child. */
        errno = EINVAL;
    } else {
        int srcfd = open(src, O_RDONLY | O_DIRECTORY | O_CLOEXEC | (through_link ? 0 : O_NOFOLLOW));
        struct stat ost;
        if (srcfd >= 0 && fstat(srcfd, &ost) == 0 &&
            ost.st_dev == st.st_dev && ost.st_ino == st.st_ino) {
            LocalCopyRoot root = {0};
            end = copy_local_dir(srcfd, &ost, parent, name, &root, 0);
            if (end == LOCAL_COPY_FAILED && root.have) {
                int why = errno;
                struct stat now;
                if (fstatat(parent, name, &now, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(now.st_mode) &&
                    now.st_dev == root.dev && now.st_ino == root.ino)
                    (void)remove_tree_at(parent, name, 0);
                errno = why;
            }
        } else if (srcfd >= 0) {
            close(srcfd);
        }
    }
    close(parent);
    return end;
}

/* --- Recursive local remove ------------------------------------------- */

/* In-process `rm -rf` equivalent. Never shells out: the path is built from
 * attacker-influenced on-disk filenames, so `system("rm -rf ...")` is a
 * command-injection vector. lstat() + unlink() also means the walk never
 * descends into a symlinked directory (matching rm -rf's no-follow
 * behavior). */
static bool remove_local_walk(const char *path, i32 depth);
static bool remove_local_recursive(const char *path) {
    return remove_local_walk(path, 0);
}
static bool remove_local_walk(const char *path, i32 depth) {
    if (depth > FILEOPS_MAX_DEPTH) return false;
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT; /* already gone == ok */
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return false;
        bool ok = true;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' ||
                 (de->d_name[1] == '.' && de->d_name[2] == '\0'))) continue;
            char child[FB_MAX_PATH * 2];
            if (!fileops_join(child, sizeof(child), path, de->d_name)) {
                ok = false;
                break;
            }
            if (!remove_local_walk(child, depth + 1)) { ok = false; break; }
        }
        closedir(d);
        if (ok && rmdir(path) != 0) ok = false;
        return ok;
    }
    return unlink(path) == 0;
}

/* --- Recursive SFTP → local ------------------------------------------- */

/* What a remote path is. `follow` asks about the link's target instead of the
 * link (libssh2_sftp_stat vs lstat). A server that answers without permission
 * bits has said nothing that can be acted on safely: OTHER. */
typedef enum { REMOTE_MISSING = 0, REMOTE_FILE, REMOTE_DIR, REMOTE_LINK, REMOTE_OTHER } RemoteKind;

static RemoteKind remote_kind(Session *session, LIBSSH2_SFTP *sftp, const char *path, bool follow) {
    LIBSSH2_SFTP_ATTRIBUTES a;
    memset(&a, 0, sizeof a);
    int rc;
    session_sftp_scope_begin(session);
    if (follow) SFTP_CALL(rc, libssh2_sftp_stat(sftp, path, &a));
    else        SFTP_CALL(rc, libssh2_sftp_lstat(sftp, path, &a));
    session_sftp_scope_end(session);
    if (rc != 0) return REMOTE_MISSING;
    if (!(a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)) return REMOTE_OTHER;
    if (LIBSSH2_SFTP_S_ISLNK(a.permissions)) return REMOTE_LINK;
    if (LIBSSH2_SFTP_S_ISDIR(a.permissions)) return REMOTE_DIR;
    if (LIBSSH2_SFTP_S_ISREG(a.permissions)) return REMOTE_FILE;
    return REMOTE_OTHER;
}

/* The remote walk answers like the local one (LocalCopyEnd): OK, LEFT_BEHIND
 * when all it did not copy was left on purpose — which a paste reports as
 * "copied, N links not followed" — and FAILED for anything else. One bool
 * cannot tell a folder that lost a FILE from one that only skipped a link. */
static LocalCopyEnd sftp_download_walk(Session *src_session, LIBSSH2_SFTP *src_sftp,
                                       const char *remote_path, int parent, const char *dest_name, i32 depth);
static LocalCopyEnd sftp_download_recursive(Session *src_session, LIBSSH2_SFTP *src_sftp,
                                            const char *remote_path, const char *local_path) {
    char name[NAME_MAX + 1];
    int parent = local_destination_parent(local_path, name, sizeof name);
    if (parent < 0) return LOCAL_COPY_FAILED;
    LocalCopyEnd end = sftp_download_walk(src_session, src_sftp, remote_path, parent, name, 0);
    close(parent);
    return end;
}

static LocalCopyEnd sftp_download_walk(Session *src_session, LIBSSH2_SFTP *src_sftp,
                                       const char *remote_path, int parent, const char *dest_name, i32 depth) {
    if (depth > FILEOPS_MAX_DEPTH) return LOCAL_COPY_FAILED;
    RemoteKind kind = remote_kind(src_session, src_sftp, remote_path, false);
    if (kind == REMOTE_LINK) {
        /* See the rules above: a file behind a link is copied as that file; a
         * folder behind a link is entered only when the link is the item the
         * user picked; everything else is left and counted. */
        RemoteKind target = remote_kind(src_session, src_sftp, remote_path, true);
        if (target == REMOTE_FILE) kind = REMOTE_FILE;
        else if (target == REMOTE_DIR && depth == 0) kind = REMOTE_DIR;
        else kind = REMOTE_OTHER;
    }
    if (kind == REMOTE_MISSING) return LOCAL_COPY_FAILED;
    if (kind == REMOTE_OTHER) {
        /* Not a file, not a folder we may enter: a nested link to a folder, a
         * dangling link, a device, a FIFO (reading one would never return). */
        atomic_fetch_add(&g_fileops_skipped, 1);
        return LOCAL_COPY_LEFT_BEHIND;
    }
    if (kind == REMOTE_FILE) {
        return sftp_download_to_local_at(src_session, src_sftp, remote_path, parent, dest_name)
                   ? LOCAL_COPY_OK : LOCAL_COPY_FAILED;
    }

    /* Make local dir, read remote dir, recurse. */
    if (mkdirat(parent, dest_name, 0755) != 0) return LOCAL_COPY_FAILED;
    int dst = openat(parent, dest_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dst < 0) return LOCAL_COPY_FAILED;

    session_sftp_scope_begin(src_session);
    LIBSSH2_SFTP_HANDLE *dh;
    SFTP_CALL_PTR(dh, libssh2_sftp_opendir(src_sftp, remote_path));
    if (!dh) { session_sftp_scope_end(src_session); close(dst); return LOCAL_COPY_FAILED; }

    /* Batch the directory listing before recursing — recursing while the
     * SFTP handle is still open makes libssh2 reuse the channel and
     * interleave reads, which breaks. */
    typedef struct { char name[256]; bool is_dir; } Child;
    Child *children = NULL;
    i32 child_count = 0, child_cap = 0;
    char name[256];
    LIBSSH2_SFTP_ATTRIBUTES a;
    bool listing_ok = true;
    for (;;) {
        int n;
        SFTP_CALL(n, libssh2_sftp_readdir(dh, name, sizeof(name) - 1u, &a));
        if (n < 0) { listing_ok = false; break; }
        if (n == 0) break;
        if ((usize)n >= sizeof(name)) { listing_ok = false; break; }
        name[n] = '\0';
        if (name[0] == '.' && (name[1] == '\0' ||
            (name[1] == '.' && name[2] == '\0'))) continue;
        /* Every level of the listing is server-chosen, not just the entry
         * the user clicked: a child named "../../.ssh/authorized_keys" is
         * joined onto the LOCAL destination below. The same gate the
         * top-level item passed applies to each readdir result. */
        if (!sftp_name_is_safe_component(name)) { listing_ok = false; break; }
        if (child_count >= child_cap) {
            if (!fileops_grow((void **)&children, child_count,
                              &child_cap, sizeof(Child))) {
                listing_ok = false;
                break;
            }
        }
        snprintf(children[child_count].name, sizeof(children[child_count].name), "%s", name);
        children[child_count].is_dir = LIBSSH2_SFTP_S_ISDIR(a.permissions);
        child_count++;
    }
    { int crc; SFTP_CALL(crc, libssh2_sftp_closedir(dh)); (void)crc; }
    session_sftp_scope_end(src_session);

    /* A child that could not be copied does not stop its siblings: the caller
     * is told the copy is incomplete (and so never deletes the source), but
     * everything that can be copied is. The worst answer of any child is the
     * folder's. */
    LocalCopyEnd end = listing_ok ? LOCAL_COPY_OK : LOCAL_COPY_FAILED;
    for (i32 i = 0; i < child_count; i++) {
        char rc[FB_MAX_PATH * 2];
        if (!fileops_join(rc, sizeof(rc), remote_path, children[i].name)) {
            end = LOCAL_COPY_FAILED;
            continue;
        }
        LocalCopyEnd child = sftp_download_walk(src_session, src_sftp, rc, dst, children[i].name, depth + 1);
        if (child > end) end = child;
    }
    free(children);
    close(dst);
    return end;
}

/* --- Recursive local → SFTP ------------------------------------------- */

/* The path of the file open at `fd`, as the kernel names it now. A check made
 * on it is a check on the file that will be read, whatever its name pointed
 * at a moment before. */
static bool fileops_fd_path(int fd, char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
#if defined(__APPLE__)
    char got[MAXPATHLEN];
    if (fcntl(fd, F_GETPATH, got) == -1) return false;
    usize n = strnlen(got, sizeof got);
    if (n == 0 || n >= cap || got[0] != '/') return false;
    memcpy(out, got, n + 1);
    return true;
#elif defined(__linux__)
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, out, cap - 1);
    if (n <= 0 || (usize)n >= cap - 1) { out[0] = '\0'; return false; }
    out[n] = '\0';
    return out[0] == '/';
#else
    (void)fd;
    return false;
#endif
}

/* Whether `path` is `root` or inside it; both are kernel names of open files. */
static bool fileops_path_inside(const char *path, const char *root) {
    if (!root || !root[0] || !path || !path[0]) return false;
    usize rl = strlen(root);
    if (rl == 1 && root[0] == '/') return true;
    return strncmp(path, root, rl) == 0 && (path[rl] == '/' || path[rl] == '\0');
}

/* Upload the directory open at `fd` (taken) to `remote_path`.
 *
 * Every entry is named relative to the held descriptor, as the local copy
 * does, so the entry that was examined is the entry that is read: a folder is
 * entered only as the folder fstatat saw (never through a link), a file is
 * opened without following a link, and a link is followed only to a regular
 * file — judged inside or outside the picked folder (`root`) by the kernel's
 * name for the file that was OPENED. This is the copy that leaves the
 * machine, and a tree from somewhere else (a cloned repository) can hold
 * `img/a.png -> ../../.ssh/id_ed25519`, or have it swapped in while the walk
 * runs; such a link, and anything that is not a file or folder, is left
 * behind and counted. `label` is the local path, for the progress line only. */
static LocalCopyEnd sftp_upload_dir(Session *dst_session, LIBSSH2_SFTP *dst_sftp, int fd,
                                    const char *label, const char *remote_path,
                                    const char *root, i32 depth) {
    if (depth > FILEOPS_MAX_DEPTH) { close(fd); return LOCAL_COPY_FAILED; }

    session_sftp_scope_begin(dst_session);
    { int mrc; SFTP_CALL(mrc, libssh2_sftp_mkdir(dst_sftp, remote_path,
        LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP)); (void)mrc; }
    session_sftp_scope_end(dst_session);

    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return LOCAL_COPY_FAILED; }
    int sfd = dirfd(d);
    LocalCopyEnd end = LOCAL_COPY_OK;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0'))) continue;
        char lc[FB_MAX_PATH * 2], rc[FB_MAX_PATH * 2];
        if (!fileops_join(lc, sizeof(lc), label, de->d_name) ||
            !fileops_join(rc, sizeof(rc), remote_path, de->d_name)) {
            end = LOCAL_COPY_FAILED;
            break;
        }
        struct stat cst;
        if (fstatat(sfd, de->d_name, &cst, AT_SYMLINK_NOFOLLOW) != 0) { end = LOCAL_COPY_FAILED; break; }

        if (S_ISDIR(cst.st_mode)) {
            int cfd = openat(sfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat ost;
            if (cfd < 0 || fstat(cfd, &ost) != 0 ||
                ost.st_dev != cst.st_dev || ost.st_ino != cst.st_ino) {
                if (cfd >= 0) close(cfd);
                end = LOCAL_COPY_FAILED;
                break;
            }
            LocalCopyEnd child = sftp_upload_dir(dst_session, dst_sftp, cfd, lc, rc, root, depth + 1);
            if (child == LOCAL_COPY_FAILED) { end = LOCAL_COPY_FAILED; break; }
            if (child == LOCAL_COPY_LEFT_BEHIND) end = LOCAL_COPY_LEFT_BEHIND;
            continue;
        }

        bool is_link = S_ISLNK(cst.st_mode);
        if (!S_ISREG(cst.st_mode) && !is_link) {
            atomic_fetch_add(&g_fileops_skipped, 1);     /* FIFO, socket, device */
            end = LOCAL_COPY_LEFT_BEHIND;
            continue;
        }
        struct stat fst;
        int ffd = fileops_open_regular(sfd, de->d_name, is_link, &fst);
        if (ffd < 0) {
            if (is_link) {               /* to a folder, to nothing, to itself */
                atomic_fetch_add(&g_fileops_skipped, 1);
                end = LOCAL_COPY_LEFT_BEHIND;
                continue;
            }
            end = LOCAL_COPY_FAILED;
            break;
        }
        if (is_link) {
            char where[PATH_MAX];
            if (!fileops_fd_path(ffd, where, sizeof where) || !fileops_path_inside(where, root)) {
                close(ffd);
                atomic_fetch_add(&g_fileops_skipped, 1);
                end = LOCAL_COPY_LEFT_BEHIND;
                continue;
            }
        }
        if (!sftp_upload_opened(dst_session, dst_sftp, ffd, lc, rc)) { end = LOCAL_COPY_FAILED; break; }
    }
    closedir(d);                         /* closes fd */
    return end;
}

/* Upload the folder at `local_path` — through a link when the picked item is
 * one: that link IS the request, as in the local copy. Links met inside the
 * tree follow sftp_upload_dir's rule. */
static LocalCopyEnd sftp_upload_recursive(Session *dst_session, LIBSSH2_SFTP *dst_sftp,
                                          const char *local_path, const char *remote_path) {
    int fd = open(local_path, O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOTDIR) return LOCAL_COPY_FAILED;
        return sftp_upload_from_local(dst_session, dst_sftp, local_path, remote_path)
            ? LOCAL_COPY_OK : LOCAL_COPY_FAILED;
    }
    char root[PATH_MAX];
    if (!fileops_fd_path(fd, root, sizeof root)) root[0] = '\0';   /* then no link inside is followed */
    return sftp_upload_dir(dst_session, dst_sftp, fd, local_path, remote_path, root, 0);
}

/* --- Recursive SFTP delete (used by cut-paste cleanup) ---------------- */

static bool sftp_remove_walk(Session *src_session, LIBSSH2_SFTP *src_sftp,
                             const char *remote_path, i32 depth);
static bool sftp_remove_recursive(Session *src_session, LIBSSH2_SFTP *src_sftp,
                                  const char *remote_path) {
    return sftp_remove_walk(src_session, src_sftp, remote_path, 0);
}

static bool sftp_remove_walk(Session *src_session, LIBSSH2_SFTP *src_sftp,
                             const char *remote_path, i32 depth) {
    if (depth > FILEOPS_MAX_DEPTH) return false;
    /* Use `lstat` and never descend through a symbolic link. Remove a link
     * itself only when it points to a regular file; leave links to directories
     * and unsupported targets in place. */
    RemoteKind kind = remote_kind(src_session, src_sftp, remote_path, false);
    if (kind == REMOTE_MISSING) return false;
    if (kind == REMOTE_LINK) {
        RemoteKind target = remote_kind(src_session, src_sftp, remote_path, true);
        if (target != REMOTE_FILE && !(target == REMOTE_DIR && depth == 0)) return false;
        kind = REMOTE_FILE;                 /* unlink the LINK below */
    }
    if (kind == REMOTE_OTHER) return false;
    session_sftp_scope_begin(src_session);
    if (kind == REMOTE_FILE) {
        int rc;
        SFTP_CALL(rc, libssh2_sftp_unlink(src_sftp, remote_path));
        session_sftp_scope_end(src_session);
        return rc == 0;
    }

    /* Gather children first (same reason as download_recursive). */
    LIBSSH2_SFTP_HANDLE *dh;
    SFTP_CALL_PTR(dh, libssh2_sftp_opendir(src_sftp, remote_path));
    if (!dh) { session_sftp_scope_end(src_session); return false; }
    typedef struct { char name[256]; } Child;
    Child *children = NULL; i32 n = 0, cap = 0;
    char nm[256]; LIBSSH2_SFTP_ATTRIBUTES ca;
    bool listing_ok = true;
    for (;;) {
        int k;
        SFTP_CALL(k, libssh2_sftp_readdir(dh, nm, sizeof(nm) - 1u, &ca));
        if (k < 0) { listing_ok = false; break; }
        if (k == 0) break;
        if ((usize)k >= sizeof(nm)) { listing_ok = false; break; }
        nm[k] = '\0';
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
        /* Reject traversal components before remote deletion so a name such
         * as `..` cannot escape the requested directory. */
        if (!sftp_name_is_safe_component(nm)) { listing_ok = false; break; }
        if (n >= cap) {
            if (!fileops_grow((void **)&children, n, &cap, sizeof(Child))) {
                listing_ok = false;
                break;
            }
        }
        snprintf(children[n].name, sizeof(children[n].name), "%s", nm);
        n++;
    }
    { int crc; SFTP_CALL(crc, libssh2_sftp_closedir(dh)); (void)crc; }
    session_sftp_scope_end(src_session);

    bool ok = listing_ok;
    for (i32 i = 0; i < n; i++) {
        char rc[FB_MAX_PATH * 2];
        if (!fileops_join(rc, sizeof(rc), remote_path, children[i].name)) {
            ok = false;
            break;
        }
        if (!sftp_remove_walk(src_session, src_sftp, rc, depth + 1)) { ok = false; break; }
    }
    free(children);
    if (ok) {
        session_sftp_scope_begin(src_session);
        int rc;
        SFTP_CALL(rc, libssh2_sftp_rmdir(src_sftp, remote_path));
        ok = rc == 0;
        session_sftp_scope_end(src_session);
    }
    return ok;
}

/* Does a remote path already exist? Used to keep the SFTP destinations under
 * the same no-silent-clobber rule as the local one. */
static bool sftp_path_exists(Session *session, LIBSSH2_SFTP *sftp, const char *path) {
    if (!sftp) return false;
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    session_sftp_scope_begin(session);
    int rc;
    /* lstat: a dangling link IS something at that path, and writing "to" it
     * would create whatever it names. */
    SFTP_CALL(rc, libssh2_sftp_lstat(sftp, path, &attrs));
    session_sftp_scope_end(session);
    return rc == 0;
}

/* Tests: answer the next local moves' rename with EXDEV, as two volumes
 * would. Everything after that answer is the shipping code. */
static _Atomic bool g_fileops_force_exdev;
void fb_fileops_test_force_exdev(bool on) { atomic_store(&g_fileops_force_exdev, on); }

/* When a cross-volume rename cannot be performed, copy without replacing
 * the destination and remove the source only after the copy completes. If
 * unsupported entries make the copy incomplete, return `LEFT_BEHIND` and
 * retain the source; a failed copy removes its partial destination. A
 * selected symbolic link is recreated at the destination and removed from
 * the source after success. */
static LocalCopyEnd move_local_by_copy(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) return LOCAL_COPY_FAILED;
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0) return LOCAL_COPY_FAILED;
        target[n] = '\0';
        if (symlink(target, dst) != 0) return LOCAL_COPY_FAILED;
        if (unlink(src) == 0) return LOCAL_COPY_OK;
        int why = errno;
        unlink(dst);                     /* the link just made: back to one copy */
        errno = why;
        return LOCAL_COPY_FAILED;
    }
    LocalCopyEnd end = S_ISDIR(st.st_mode)
        ? copy_local_tree(src, dst)
        : (fb_copy_local(src, dst) ? LOCAL_COPY_OK : LOCAL_COPY_FAILED);
    if (end != LOCAL_COPY_OK) return end;         /* the source stays whole */
    /* Everything is at the destination. Now, and only now, the source goes. */
    bool gone = S_ISDIR(st.st_mode) ? remove_local_recursive(src) : unlink(src) == 0;
    return gone ? LOCAL_COPY_OK : LOCAL_COPY_FAILED;
}

/* Move `src` to `dst` without ever replacing what is there.
 *
 * On one volume that is a rename, made relative to both parents so the entry
 * examined is the entry moved. Across volumes the rename answers EXDEV, which
 * is the cue for the copy-then-remove above. A file system without the atomic
 * no-replace rename (ENOTSUP / EINVAL; ENOSYS on an old Linux) gets the
 * check-then-rename fb_rename_local_noreplace falls back to — and that rename
 * can answer EXDEV too. */
static LocalCopyEnd move_local_noreplace(const char *src, const char *dst) {
    char src_name[NAME_MAX + 1], dst_name[NAME_MAX + 1];
    int src_dir = local_destination_parent(src, src_name, sizeof src_name);
    if (src_dir < 0) return LOCAL_COPY_FAILED;
    int dst_dir = local_destination_parent(dst, dst_name, sizeof dst_name);
    if (dst_dir < 0) { close(src_dir); return LOCAL_COPY_FAILED; }
    int rc = -1;
    if (atomic_load(&g_fileops_force_exdev)) {
        errno = EXDEV;
    } else {
#if defined(__APPLE__) && defined(RENAME_EXCL)
        rc = renameatx_np(src_dir, src_name, dst_dir, dst_name, RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
        rc = (int)syscall(SYS_renameat2, src_dir, src_name, dst_dir, dst_name, 1u);
#else
        errno = ENOTSUP;
#endif
    }
    int why = errno;
    close(src_dir);
    close(dst_dir);
    if (rc == 0) return LOCAL_COPY_OK;
    if (why == ENOTSUP || why == EINVAL || why == ENOSYS) {
        if (fb_rename_local_noreplace(src, dst)) return LOCAL_COPY_OK;
        why = errno;
    }
    if (why == EXDEV) return move_local_by_copy(src, dst);
    errno = why;
    return LOCAL_COPY_FAILED;
}

/* The same move for a caller that is not a paste (a row dragged onto a folder):
 * true only when all of it is at `to` and gone from `from`. */
bool fb_move_local_noreplace(const char *from, const char *to) {
    if (!from || !to || !from[0] || !to[0]) { errno = EINVAL; return false; }
    return move_local_noreplace(from, to) == LOCAL_COPY_OK;
}

/* One item of a paste. `end` (may be NULL) receives the three-way answer
 * where the transfer has one — LEFT_BEHIND is "it is there, minus entries
 * left on purpose" — `err` the errno of a failure this side knows the reason
 * of, and `dst_path` (FB_MAX_PATH * 2) where it was to land. */
static bool paste_item(FileBrowser *dst_fb, const FileClipboardItem *item, bool is_cut,
                       LocalCopyEnd *end, int *err, char *dst_path) {
    if (end) *end = LOCAL_COPY_FAILED;
    if (err) *err = 0;
    dst_path[0] = '\0';
    if (!dst_fb || !item || !item->name[0]) return false;

    /* The name came from a remote directory listing for SFTP items. A hostile
     * server can put "../.." in it; joined onto the destination cwd below that
     * writes outside the folder the user is looking at. fb_navigate_sftp
     * filters the same way — this is the second gate, on the path that
     * actually creates files. */
    if (!sftp_name_is_safe_component(item->name)) { if (err) *err = EINVAL; return false; }

    char destination[FB_MAX_PATH];
    fb_operation_dir(dst_fb, destination, sizeof destination);
    if (!destination[0]) return false;
    int path_len = strcmp(destination, "/") == 0
        ? snprintf(dst_path, FB_MAX_PATH * 2, "/%s", item->name)
        : snprintf(dst_path, FB_MAX_PATH * 2, "%s/%s", destination, item->name);
    if (path_len < 0 || (usize)path_len >= FB_MAX_PATH * 2) {
        dst_path[0] = '\0';
        if (err) *err = ENAMETOOLONG;
        return false;
    }

    /* Refuse a copy onto itself — the same NAMESPACE and the same path
     * (ui/file_ref.h). The path alone is not the file: the same text on this
     * machine and on a server, or on two servers, names different files, and
     * comparing only the strings would refuse those transfers as
     * "self-paste". An item's session is retained by the clipboard, so the
     * pointer identifies the session it was copied from for as long as the
     * item exists. */
    if (!is_cut && file_ref_same(item->source == FB_SOURCE_SFTP, item->session, item->path,
                                 dst_fb->source == FB_SOURCE_SFTP, dst_fb->session, dst_path)) {
        if (err) *err = EEXIST;
        return false;
    }

    bool ok = false;
    /* The transfer's own three-way answer, where it has one. */
    LocalCopyEnd how = LOCAL_COPY_FAILED;

    /* --- Local → Local ---------------------------------------------- */
    if (item->source == FB_SOURCE_LOCAL && dst_fb->source == FB_SOURCE_LOCAL) {
        /* Never silently clobber an existing destination — that destroys the
         * user's data with no undo. (Same-dir self-paste is refused above.) */
        errno = 0;
        if (is_cut) {
            how = move_local_noreplace(item->path, dst_path);
        } else if (item->is_dir) {
            how = copy_local_tree(item->path, dst_path);
        } else {
            how = fb_copy_local(item->path, dst_path) ? LOCAL_COPY_OK : LOCAL_COPY_FAILED;
        }
        ok = how == LOCAL_COPY_OK;
        /* Both ends are this machine's: errno is the reason. */
        if (how == LOCAL_COPY_FAILED && err) *err = errno;
    }
    /* --- Local → SFTP ----------------------------------------------- */
    else if (item->source == FB_SOURCE_LOCAL && dst_fb->source == FB_SOURCE_SFTP) {
        Session *dst_session = (Session *)dst_fb->session;
        LIBSSH2_SFTP *dst_sftp = (LIBSSH2_SFTP *)dst_fb->sftp_handle;
        if (!dst_session || !dst_sftp) return false;
        /* Local-to-remote paste never replaces an existing destination.
         * Drag-and-drop onto a terminal stages the upload and preserves the
         * existing file if the transfer fails. */
        if (sftp_path_exists(dst_session, dst_sftp, dst_path)) { if (err) *err = EEXIST; return false; }
        if (item->is_dir) {
            how = sftp_upload_recursive(dst_session, dst_sftp, item->path, dst_path);
            ok = how == LOCAL_COPY_OK;
            if (ok && is_cut) {
                /* Remove local tree after successful upload (in-process; never
                 * shell out — item->path is an attacker-influenced filename). */
                if (!remove_local_recursive(item->path)) ok = false;
            }
        } else {
            ok = sftp_upload_from_local(dst_session, dst_sftp, item->path, dst_path);
            if (ok && is_cut && unlink(item->path) != 0) ok = false;
        }
    }
    /* --- SFTP → Local ----------------------------------------------- */
    else if (item->source == FB_SOURCE_SFTP && dst_fb->source == FB_SOURCE_LOCAL) {
        Session *src_session = (Session *)item->session;
        LIBSSH2_SFTP *src_sftp = src_session ? (LIBSSH2_SFTP *)session_get_sftp(src_session) : NULL;
        if (!src_session || !src_sftp) return false;
        if (item->is_dir) {
            how = sftp_download_recursive(src_session, src_sftp, item->path, dst_path);
            ok = how == LOCAL_COPY_OK;
            if (ok && is_cut) ok = sftp_remove_recursive(src_session, src_sftp, item->path);
        } else {
            ok = sftp_download_to_local(src_session, src_sftp, item->path, dst_path);
            if (ok && is_cut) {
                session_sftp_scope_begin(src_session);
                int urc;
                SFTP_CALL(urc, libssh2_sftp_unlink(src_sftp, item->path));
                ok = urc == 0;
                session_sftp_scope_end(src_session);
            }
        }
    }
    /* --- SFTP → SFTP ------------------------------------------------ */
    else if (item->source == FB_SOURCE_SFTP && dst_fb->source == FB_SOURCE_SFTP) {
        Session *src_session = (Session *)item->session;
        Session *dst_session = (Session *)dst_fb->session;
        LIBSSH2_SFTP *dst_sftp = (LIBSSH2_SFTP *)dst_fb->sftp_handle;
        if (!src_session || !dst_session || !dst_sftp) return false;
        if (sftp_path_exists(dst_session, dst_sftp, dst_path)) { if (err) *err = EEXIST; return false; }

        if (src_session == dst_session && is_cut) {
            /* Same session move = server-side rename, no transfer needed. */
            LIBSSH2_SFTP *src_sftp = (LIBSSH2_SFTP *)session_get_sftp(src_session);
            if (!src_sftp) return false;
            session_sftp_scope_begin(src_session);
            int rrc;
            SFTP_CALL(rrc, libssh2_sftp_rename(src_sftp, item->path, dst_path));
            ok = rrc == 0;
            session_sftp_scope_end(src_session);
        } else {
            /* Copy or cross-session move: download to tmp, upload, cleanup. */
            LIBSSH2_SFTP *src_sftp = (LIBSSH2_SFTP *)session_get_sftp(src_session);
            if (!src_sftp) return false;
            char tmp_dir[] = "/tmp/liu_sftp_copy_XXXXXX";
            if (!mkdtemp(tmp_dir)) return false;
            char canonical_tmp[PATH_MAX];
            if (!realpath(tmp_dir, canonical_tmp)) { rmdir(tmp_dir); return false; }
            char tmp_local[PATH_MAX + 16];
            if (!fileops_join(tmp_local, sizeof(tmp_local), canonical_tmp, "payload")) {
                rmdir(tmp_dir);
                return false;
            }
            if (item->is_dir) {
                how = sftp_download_recursive(src_session, src_sftp, item->path, tmp_local);
                /* What came down goes up, links left behind or not, and
                 * an upload that fails makes the item FAILED: "links not
                 * followed" is never the answer for a folder that was
                 * not uploaded. */
                if (how != LOCAL_COPY_FAILED) {
                    LocalCopyEnd up = sftp_upload_recursive(dst_session, dst_sftp, tmp_local, dst_path);
                    if (up > how) how = up;
                }
                ok = how == LOCAL_COPY_OK;
                remove_local_recursive(tmp_local); /* best-effort temp cleanup */
            } else {
                ok = sftp_download_to_local(src_session, src_sftp, item->path, tmp_local);
                if (ok) ok = sftp_upload_from_local(dst_session, dst_sftp, tmp_local, dst_path);
                unlink(tmp_local);
            }
            rmdir(tmp_dir);
            if (ok && is_cut) {
                if (item->is_dir) ok = sftp_remove_recursive(src_session, src_sftp, item->path);
                else {
                    /* As everywhere else in this function: a move whose
                     * source is still there did not finish, and says so. */
                    session_sftp_scope_begin(src_session);
                    { int urc; SFTP_CALL(urc, libssh2_sftp_unlink(src_sftp, item->path)); ok = urc == 0; }
                    session_sftp_scope_end(src_session);
                }
            }
        }
    }

    if (end) *end = ok ? LOCAL_COPY_OK : how;
    return ok;
}

bool fb_paste_item(FileBrowser *dst_fb, const FileClipboardItem *item, bool is_cut) {
    char dst_path[FB_MAX_PATH * 2];
    return paste_item(dst_fb, item, is_cut, NULL, NULL, dst_path);
}

/* fb_paste_item, with the answer the status line needs: not "did all of it
 * land" but which of three things happened (ui/paste_report.h). */
FbPasteOutcome fb_paste_item_report(FileBrowser *dst_fb, const FileClipboardItem *item,
                                    bool is_cut, FbPasteReport *report) {
    FbPasteReport local;
    if (!report) report = &local;
    memset(report, 0, sizeof *report);
    i32 before = atomic_load(&g_fileops_skipped);
    LocalCopyEnd end = LOCAL_COPY_FAILED;
    bool ok = paste_item(dst_fb, item, is_cut, &end, &report->err, report->dst);
    report->links = atomic_load(&g_fileops_skipped) - before;
    report->outcome = ok ? FB_PASTE_OK
                    : end == LOCAL_COPY_LEFT_BEHIND ? FB_PASTE_LINKS : FB_PASTE_FAILED;
    if (report->outcome != FB_PASTE_FAILED) report->err = 0;
    return report->outcome;
}

bool fb_paste(FileBrowser *dst_fb, const FileClipboard *clip) {
    if (!dst_fb || !clip || !clip->has || clip->count <= 0) return false;

    bool all_ok = true;
    for (i32 i = 0; i < clip->count; i++) {
        if (!fb_paste_item(dst_fb, &clip->items[i], clip->is_cut)) {
            all_ok = false;
            /* Continue — partial success is the UX users expect with
             * multi-select (skip conflict, copy the rest). */
        }
    }
    fb_refresh(dst_fb);
    return all_ok;
}
