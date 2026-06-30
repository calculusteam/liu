/*
 * Liu - file operations (upload, download, delete, rename)
 * Works for both local filesystem and SFTP.
 */
#include "ui/filebrowser.h"
#include "ssh/ssh_session.h"
#include "ssh/sftp.h"       /* transfer progress (overlay UX) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

/* SFTP download: remote_path → local_dir/filename */
bool fb_download_file(FileBrowser *fb, i32 index, const char *local_dir) {
    if (index < 0 || index >= fb->entry_count) return false;
    if (fb->source != FB_SOURCE_SFTP || !fb->sftp_handle) return false;

    FileEntry *e = &fb->entries[index];
    if (e->is_dir) return false;
    char remote_path[FB_MAX_PATH * 2];
    fb_entry_path(fb, e, remote_path, sizeof(remote_path));

    Session *owner = (Session *)fb->session;
    session_sftp_scope_begin(owner);
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)fb->sftp_handle;
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(sftp, remote_path,
        LIBSSH2_FXF_READ, 0);
    if (!handle) {
        session_sftp_scope_end(owner);
        return false;
    }

    char local_path[2048];
    snprintf(local_path, sizeof(local_path), "%s/%s", local_dir, e->name);

    FILE *f = fopen(local_path, "wb");
    if (!f) {
        libssh2_sftp_close(handle);
        session_sftp_scope_end(owner);
        return false;
    }

    char buf[32768];
    for (;;) {
        ssize_t n = libssh2_sftp_read(handle, buf, sizeof(buf));
        if (n <= 0) break;
        fwrite(buf, 1, (size_t)n, f);
    }

    fclose(f);
    libssh2_sftp_close(handle);
    session_sftp_scope_end(owner);
    return true;
}

/* SFTP upload: local_path → remote cwd */
bool fb_upload_file(FileBrowser *fb, const char *local_path) {
    if (fb->source != FB_SOURCE_SFTP || !fb->sftp_handle) return false;

    const char *fname = strrchr(local_path, '/');
    fname = fname ? fname + 1 : local_path;

    char remote_path[2048];
    snprintf(remote_path, sizeof(remote_path), "%s/%s", fb->cwd, fname);

    FILE *f = fopen(local_path, "rb");
    if (!f) return false;

    Session *owner = (Session *)fb->session;
    session_sftp_scope_begin(owner);
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)fb->sftp_handle;
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(sftp, remote_path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!handle) {
        fclose(f);
        session_sftp_scope_end(owner);
        return false;
    }

    char buf[32768];
    size_t nread;
    while ((nread = fread(buf, 1, sizeof(buf), f)) > 0) {
        ssize_t total = 0;
        while ((size_t)total < nread) {
            ssize_t n = libssh2_sftp_write(handle, buf + total, nread - (size_t)total);
            if (n < 0) {
                fclose(f);
                libssh2_sftp_close(handle);
                session_sftp_scope_end(owner);
                return false;
            }
            total += n;
        }
    }

    fclose(f);
    libssh2_sftp_close(handle);
    session_sftp_scope_end(owner);
    return true;
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
        if (e->is_dir)
            ok = libssh2_sftp_rmdir(sftp, path) == 0;
        else
            ok = libssh2_sftp_unlink(sftp, path) == 0;
        session_sftp_scope_end(owner);
    } else {
        if (e->is_dir)
            ok = rmdir(path) == 0;
        else
            ok = unlink(path) == 0;
    }

    if (ok) fb_refresh(fb);
    return ok;
}

/* Create directory (local or SFTP) */
bool fb_mkdir(FileBrowser *fb, const char *name) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", fb->cwd, name);

    bool ok = false;
    if (fb->source == FB_SOURCE_SFTP && fb->sftp_handle) {
        Session *owner = (Session *)fb->session;
        session_sftp_scope_begin(owner);
        LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)fb->sftp_handle;
        ok = libssh2_sftp_mkdir(sftp, path,
            LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP) == 0;
        session_sftp_scope_end(owner);
    } else {
        ok = mkdir(path, 0755) == 0;
    }

    if (ok) fb_refresh(fb);
    return ok;
}

/* Create an empty file in the current directory (local or SFTP). Fails if a
 * file with that name already exists so an existing file is never truncated. */
bool fb_create_file(FileBrowser *fb, const char *name) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", fb->cwd, name);

    bool ok = false;
    if (fb->source == FB_SOURCE_SFTP && fb->sftp_handle) {
        Session *owner = (Session *)fb->session;
        session_sftp_scope_begin(owner);
        LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)fb->sftp_handle;
        LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(sftp, path,
            LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL,
            LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
            LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
        if (h) {
            libssh2_sftp_close(h);
            ok = true;
        }
        session_sftp_scope_end(owner);
    } else {
        /* O_EXCL via fopen "wx" — refuse to clobber an existing file. */
        FILE *f = fopen(path, "wx");
        if (f) { fclose(f); ok = true; }
    }

    if (ok) fb_refresh(fb);
    return ok;
}

/* Rename file (local or SFTP) */
bool fb_rename(FileBrowser *fb, i32 index, const char *new_name) {
    if (index < 0 || index >= fb->entry_count) return false;
    FileEntry *e = &fb->entries[index];
    char old_path[FB_MAX_PATH * 2];
    fb_entry_path(fb, e, old_path, sizeof(old_path));

    char new_path[2048];
    snprintf(new_path, sizeof(new_path), "%s/%s", fb->cwd, new_name);

    bool ok = false;
    if (fb->source == FB_SOURCE_SFTP && fb->sftp_handle) {
        Session *owner = (Session *)fb->session;
        session_sftp_scope_begin(owner);
        LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)fb->sftp_handle;
        ok = libssh2_sftp_rename(sftp, old_path, new_path) == 0;
        session_sftp_scope_end(owner);
    } else {
        ok = rename(old_path, new_path) == 0;
    }

    if (ok) fb_refresh(fb);
    return ok;
}

/* Local copy: copy file to destination */
bool fb_copy_local(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }

    char buf[32768];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return true;
}

/* =========================================================================
 * Clipboard paste
 *
 * Handles all 4 source×dest combos, including recursive directory transfers.
 * Transfers are synchronous on the main thread — matches the single-file
 * upload/download helpers above. Large trees block the UI; moving them to
 * a worker thread is the next step, not this one.
 * ========================================================================= */

static bool sftp_download_to_local(Session *src_session, LIBSSH2_SFTP *src_sftp,
                                   const char *remote_path, const char *local_path) {
    session_sftp_scope_begin(src_session);
    /* Stat for total size so the overlay knows the denominator. Non-fatal
     * if stat fails — we fall back to total=0 (overlay shows MB transferred
     * and hides the ETA/%). */
    u64 total_size = 0;
    {
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        if (libssh2_sftp_stat(src_sftp, remote_path, &attrs) == 0)
            total_size = attrs.filesize;
    }
    LIBSSH2_SFTP_HANDLE *rh = libssh2_sftp_open(src_sftp, remote_path,
                                                LIBSSH2_FXF_READ, 0);
    if (!rh) { session_sftp_scope_end(src_session); return false; }

    FILE *lf = fopen(local_path, "wb");
    if (!lf) { libssh2_sftp_close(rh); session_sftp_scope_end(src_session); return false; }

    sftp_transfer_begin(remote_path, total_size, false);
    char buf[32768];
    bool ok = true;
    u64 done = 0;
    for (;;) {
        ssize_t n = libssh2_sftp_read(rh, buf, sizeof(buf));
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (fwrite(buf, 1, (size_t)n, lf) != (size_t)n) { ok = false; break; }
        done += (u64)n;
        sftp_transfer_tick(done);
    }
    sftp_transfer_end();
    fclose(lf);
    libssh2_sftp_close(rh);
    session_sftp_scope_end(src_session);
    return ok;
}

static bool sftp_upload_from_local(Session *dst_session, LIBSSH2_SFTP *dst_sftp,
                                   const char *local_path, const char *remote_path) {
    FILE *lf = fopen(local_path, "rb");
    if (!lf) return false;

    /* Measure local file size for overlay denominator. */
    u64 total_size = 0;
    {
        struct stat st;
        if (stat(local_path, &st) == 0) total_size = (u64)st.st_size;
    }

    session_sftp_scope_begin(dst_session);
    LIBSSH2_SFTP_HANDLE *wh = libssh2_sftp_open(dst_sftp, remote_path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!wh) { fclose(lf); session_sftp_scope_end(dst_session); return false; }

    sftp_transfer_begin(local_path, total_size, true);
    char buf[32768];
    size_t nread;
    bool ok = true;
    u64 done = 0;
    while ((nread = fread(buf, 1, sizeof(buf), lf)) > 0) {
        ssize_t total = 0;
        while ((size_t)total < nread) {
            ssize_t n = libssh2_sftp_write(wh, buf + total, nread - (size_t)total);
            if (n < 0) { ok = false; break; }
            total += n;
        }
        if (!ok) break;
        done += (u64)nread;
        sftp_transfer_tick(done);
    }
    sftp_transfer_end();
    fclose(lf);
    libssh2_sftp_close(wh);
    session_sftp_scope_end(dst_session);
    return ok;
}

/* --- Recursive local copy --------------------------------------------- */

static bool copy_local_recursive(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST) return false;
        DIR *d = opendir(src);
        if (!d) return false;
        bool ok = true;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' ||
                 (de->d_name[1] == '.' && de->d_name[2] == '\0'))) continue;
            char sc[FB_MAX_PATH * 2], dc[FB_MAX_PATH * 2];
            snprintf(sc, sizeof(sc), "%s/%s", src, de->d_name);
            snprintf(dc, sizeof(dc), "%s/%s", dst, de->d_name);
            if (!copy_local_recursive(sc, dc)) { ok = false; break; }
        }
        closedir(d);
        return ok;
    } else {
        return fb_copy_local(src, dst);
    }
}

/* --- Recursive local remove ------------------------------------------- */

/* In-process `rm -rf` equivalent. Never shells out: the path is built from
 * attacker-influenced on-disk filenames, so `system("rm -rf ...")` is a
 * command-injection vector. lstat() + unlink() also means we never descend
 * into a symlinked directory (matching rm -rf's no-follow behavior). */
static bool remove_local_recursive(const char *path) {
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
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            if (!remove_local_recursive(child)) { ok = false; break; }
        }
        closedir(d);
        if (ok && rmdir(path) != 0) ok = false;
        return ok;
    }
    return unlink(path) == 0;
}

/* --- Recursive SFTP → local ------------------------------------------- */

static bool sftp_download_recursive(Session *src_session, LIBSSH2_SFTP *src_sftp,
                                    const char *remote_path, const char *local_path) {
    session_sftp_scope_begin(src_session);
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    if (libssh2_sftp_stat(src_sftp, remote_path, &attrs) != 0) {
        session_sftp_scope_end(src_session);
        return false;
    }
    bool is_dir = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
    session_sftp_scope_end(src_session);

    if (!is_dir) {
        return sftp_download_to_local(src_session, src_sftp, remote_path, local_path);
    }

    /* Make local dir, read remote dir, recurse. */
    mkdir(local_path, 0755);

    session_sftp_scope_begin(src_session);
    LIBSSH2_SFTP_HANDLE *dh = libssh2_sftp_opendir(src_sftp, remote_path);
    if (!dh) { session_sftp_scope_end(src_session); return false; }

    /* Batch the directory listing before recursing — if we recurse while the
     * SFTP handle is still open, libssh2 reuses the channel and interleaves
     * reads, which breaks. */
    typedef struct { char name[256]; bool is_dir; } Child;
    Child *children = NULL;
    i32 child_count = 0, child_cap = 0;
    char name[256];
    LIBSSH2_SFTP_ATTRIBUTES a;
    for (;;) {
        int n = libssh2_sftp_readdir(dh, name, sizeof(name), &a);
        if (n <= 0) break;
        name[n] = '\0';
        if (name[0] == '.' && (name[1] == '\0' ||
            (name[1] == '.' && name[2] == '\0'))) continue;
        if (child_count >= child_cap) {
            i32 nc = child_cap ? child_cap * 2 : 32;
            Child *grown = realloc(children, (usize)nc * sizeof(Child));
            if (!grown) break;
            children = grown; child_cap = nc;
        }
        snprintf(children[child_count].name, sizeof(children[child_count].name), "%s", name);
        children[child_count].is_dir = LIBSSH2_SFTP_S_ISDIR(a.permissions);
        child_count++;
    }
    libssh2_sftp_closedir(dh);
    session_sftp_scope_end(src_session);

    bool ok = true;
    for (i32 i = 0; i < child_count; i++) {
        char rc[FB_MAX_PATH * 2], lc[FB_MAX_PATH * 2];
        snprintf(rc, sizeof(rc), "%s/%s", remote_path, children[i].name);
        snprintf(lc, sizeof(lc), "%s/%s", local_path, children[i].name);
        if (!sftp_download_recursive(src_session, src_sftp, rc, lc)) { ok = false; break; }
    }
    free(children);
    return ok;
}

/* --- Recursive local → SFTP ------------------------------------------- */

static bool sftp_upload_recursive(Session *dst_session, LIBSSH2_SFTP *dst_sftp,
                                  const char *local_path, const char *remote_path) {
    struct stat st;
    if (lstat(local_path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) {
        return sftp_upload_from_local(dst_session, dst_sftp, local_path, remote_path);
    }

    session_sftp_scope_begin(dst_session);
    libssh2_sftp_mkdir(dst_sftp, remote_path,
        LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP);
    session_sftp_scope_end(dst_session);

    DIR *d = opendir(local_path);
    if (!d) return false;
    bool ok = true;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0'))) continue;
        char lc[FB_MAX_PATH * 2], rc[FB_MAX_PATH * 2];
        snprintf(lc, sizeof(lc), "%s/%s", local_path, de->d_name);
        snprintf(rc, sizeof(rc), "%s/%s", remote_path, de->d_name);
        if (!sftp_upload_recursive(dst_session, dst_sftp, lc, rc)) { ok = false; break; }
    }
    closedir(d);
    return ok;
}

/* --- Recursive SFTP delete (used by cut-paste cleanup) ---------------- */

static bool sftp_remove_recursive(Session *src_session, LIBSSH2_SFTP *src_sftp,
                                  const char *remote_path) {
    session_sftp_scope_begin(src_session);
    LIBSSH2_SFTP_ATTRIBUTES a;
    if (libssh2_sftp_stat(src_sftp, remote_path, &a) != 0) {
        session_sftp_scope_end(src_session);
        return false;
    }
    bool is_dir = LIBSSH2_SFTP_S_ISDIR(a.permissions);
    if (!is_dir) {
        int rc = libssh2_sftp_unlink(src_sftp, remote_path);
        session_sftp_scope_end(src_session);
        return rc == 0;
    }

    /* Gather children first (same reason as download_recursive). */
    LIBSSH2_SFTP_HANDLE *dh = libssh2_sftp_opendir(src_sftp, remote_path);
    if (!dh) { session_sftp_scope_end(src_session); return false; }
    typedef struct { char name[256]; } Child;
    Child *children = NULL; i32 n = 0, cap = 0;
    char nm[256]; LIBSSH2_SFTP_ATTRIBUTES ca;
    for (;;) {
        int k = libssh2_sftp_readdir(dh, nm, sizeof(nm), &ca);
        if (k <= 0) break;
        nm[k] = '\0';
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
        if (n >= cap) {
            i32 nc = cap ? cap * 2 : 32;
            Child *grown = realloc(children, (usize)nc * sizeof(Child));
            if (!grown) break;
            children = grown; cap = nc;
        }
        snprintf(children[n].name, sizeof(children[n].name), "%s", nm);
        n++;
    }
    libssh2_sftp_closedir(dh);
    session_sftp_scope_end(src_session);

    bool ok = true;
    for (i32 i = 0; i < n; i++) {
        char rc[FB_MAX_PATH * 2];
        snprintf(rc, sizeof(rc), "%s/%s", remote_path, children[i].name);
        if (!sftp_remove_recursive(src_session, src_sftp, rc)) { ok = false; break; }
    }
    free(children);
    if (ok) {
        session_sftp_scope_begin(src_session);
        ok = libssh2_sftp_rmdir(src_sftp, remote_path) == 0;
        session_sftp_scope_end(src_session);
    }
    return ok;
}

bool fb_paste_item(FileBrowser *dst_fb, const FileClipboardItem *item, bool is_cut) {
    if (!dst_fb || !item || !item->name[0]) return false;

    char dst_path[FB_MAX_PATH * 2];
    if (strcmp(dst_fb->cwd, "/") == 0)
        snprintf(dst_path, sizeof(dst_path), "/%s", item->name);
    else
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_fb->cwd, item->name);

    /* Refuse self-paste (same cwd, copy mode would overwrite itself). */
    if (!is_cut && strcmp(dst_path, item->path) == 0) return false;

    bool ok = false;

    /* --- Local → Local ---------------------------------------------- */
    if (item->source == FB_SOURCE_LOCAL && dst_fb->source == FB_SOURCE_LOCAL) {
        /* Never silently clobber an existing destination — that destroys the
         * user's data with no undo. (Same-dir self-paste is refused above.) */
        if (access(dst_path, F_OK) == 0) return false;
        if (is_cut) {
            ok = rename(item->path, dst_path) == 0;
        } else if (item->is_dir) {
            ok = copy_local_recursive(item->path, dst_path);
        } else {
            ok = fb_copy_local(item->path, dst_path);
        }
    }
    /* --- Local → SFTP ----------------------------------------------- */
    else if (item->source == FB_SOURCE_LOCAL && dst_fb->source == FB_SOURCE_SFTP) {
        Session *dst_session = (Session *)dst_fb->session;
        LIBSSH2_SFTP *dst_sftp = (LIBSSH2_SFTP *)dst_fb->sftp_handle;
        if (!dst_session || !dst_sftp) return false;
        if (item->is_dir) {
            ok = sftp_upload_recursive(dst_session, dst_sftp, item->path, dst_path);
            if (ok && is_cut) {
                /* Remove local tree after successful upload (in-process; never
                 * shell out — item->path is an attacker-influenced filename). */
                if (!remove_local_recursive(item->path)) ok = false;
            }
        } else {
            ok = sftp_upload_from_local(dst_session, dst_sftp, item->path, dst_path);
            if (ok && is_cut) unlink(item->path);
        }
    }
    /* --- SFTP → Local ----------------------------------------------- */
    else if (item->source == FB_SOURCE_SFTP && dst_fb->source == FB_SOURCE_LOCAL) {
        Session *src_session = (Session *)item->session;
        LIBSSH2_SFTP *src_sftp = src_session ? (LIBSSH2_SFTP *)session_get_sftp(src_session) : NULL;
        if (!src_session || !src_sftp) return false;
        if (item->is_dir) {
            ok = sftp_download_recursive(src_session, src_sftp, item->path, dst_path);
            if (ok && is_cut) ok = sftp_remove_recursive(src_session, src_sftp, item->path);
        } else {
            ok = sftp_download_to_local(src_session, src_sftp, item->path, dst_path);
            if (ok && is_cut) {
                session_sftp_scope_begin(src_session);
                libssh2_sftp_unlink(src_sftp, item->path);
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

        if (src_session == dst_session && is_cut) {
            /* Same session move = server-side rename, no transfer needed. */
            LIBSSH2_SFTP *src_sftp = (LIBSSH2_SFTP *)session_get_sftp(src_session);
            if (!src_sftp) return false;
            session_sftp_scope_begin(src_session);
            ok = libssh2_sftp_rename(src_sftp, item->path, dst_path) == 0;
            session_sftp_scope_end(src_session);
        } else {
            /* Copy or cross-session move: download to tmp, upload, cleanup. */
            LIBSSH2_SFTP *src_sftp = (LIBSSH2_SFTP *)session_get_sftp(src_session);
            if (!src_sftp) return false;
            char tmp_local[512];
            snprintf(tmp_local, sizeof(tmp_local), "/tmp/liu_sftp_copy_%d_%ld",
                     (int)getpid(), (long)time(NULL));
            if (item->is_dir) {
                /* Use a temp dir for recursive copy. */
                mkdir(tmp_local, 0700);
                ok = sftp_download_recursive(src_session, src_sftp, item->path, tmp_local);
                if (ok) ok = sftp_upload_recursive(dst_session, dst_sftp, tmp_local, dst_path);
                remove_local_recursive(tmp_local); /* best-effort temp cleanup */
            } else {
                ok = sftp_download_to_local(src_session, src_sftp, item->path, tmp_local);
                if (ok) ok = sftp_upload_from_local(dst_session, dst_sftp, tmp_local, dst_path);
                unlink(tmp_local);
            }
            if (ok && is_cut) {
                if (item->is_dir) ok = sftp_remove_recursive(src_session, src_sftp, item->path);
                else {
                    session_sftp_scope_begin(src_session);
                    libssh2_sftp_unlink(src_sftp, item->path);
                    session_sftp_scope_end(src_session);
                }
            }
        }
    }

    return ok;
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
