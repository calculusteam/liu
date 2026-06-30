/*
 * Liu - SFTP operations via libssh2
 * Basic transfers, resume, directory sync, symlink management.
 */
#include "ssh/sftp.h"
#include "ssh/ssh_session.h"
#include "platform/platform.h"   /* platform_time_sec for transfer ticks */
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

/* Transfer buffer size (32 KB) */
#define SFTP_BUF_SIZE  32768

/* Max path length for constructed paths */
#define SFTP_MAX_PATH  4096

/* Global transfer state for UI display.
 *
 * Writers (SFTP/fileops helpers running on the main thread) call
 * sftp_transfer_begin / sftp_transfer_tick / sftp_transfer_end. Readers
 * (the sidebar-overlay renderer) read the bag via the small accessor API
 * in sftp.h. No lock: single-threaded on the main thread. */
static struct {
    bool active;
    char filename[256];
    u64  bytes_done;
    u64  bytes_total;
    bool is_upload;
    /* Speed / ETA tracking — a rolling-window sample every ~0.2 s with
     * mild EMA smoothing so the number doesn't flicker at high rates. */
    f64  start_time;
    f64  last_tick_time;
    u64  last_tick_bytes;
    f64  speed_bps;
} g_sftp_transfer = {0};

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Get remote file size via SFTP stat. Returns 0 if file doesn't exist. */
static u64 sftp_remote_file_size(LIBSSH2_SFTP *sftp, const char *path) {
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    if (libssh2_sftp_stat(sftp, path, &attrs) == 0)
        return attrs.filesize;
    return 0;
}

/* Get remote file attributes. Returns true on success. */
__attribute__((unused)) static bool sftp_remote_stat(LIBSSH2_SFTP *sftp, const char *path, LIBSSH2_SFTP_ATTRIBUTES *attrs) {
    return libssh2_sftp_stat(sftp, path, attrs) == 0;
}

/* Get local file size. Returns 0 if file doesn't exist. */
static u64 sftp_local_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0)
        return (u64)st.st_size;
    return 0;
}

/* Get local file modification time. Returns 0 if file doesn't exist. */
__attribute__((unused)) static u64 sftp_local_file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0)
        return (u64)st.st_mtime;
    return 0;
}

/* Check if local path is a directory */
__attribute__((unused)) static bool sftp_local_is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    return false;
}

/* Check if a filename is a dotfile/hidden file */
static bool is_dotfile(const char *name) {
    return name[0] == '.';
}

/* Public transfer progress API — exported via sftp.h so fileops.c's
 * recursive helpers and any other transfer path can participate in the
 * shared overlay. `sftp_transfer_begin` resets speed tracking; `tick` is
 * meant to be called after every chunk and updates the smoothed speed at
 * most every ~0.2 s. `end` clears `active`; leave filename/bytes alone so
 * the UI can hold the panel for a moment after completion if desired. */
void sftp_transfer_begin(const char *path, u64 total, bool upload) {
    const char *fname = strrchr(path, '/');
    snprintf(g_sftp_transfer.filename, sizeof(g_sftp_transfer.filename),
             "%s", fname ? fname + 1 : path);
    g_sftp_transfer.bytes_total = total;
    g_sftp_transfer.bytes_done = 0;
    g_sftp_transfer.is_upload = upload;
    g_sftp_transfer.active = true;
    g_sftp_transfer.start_time = platform_time_sec();
    g_sftp_transfer.last_tick_time = g_sftp_transfer.start_time;
    g_sftp_transfer.last_tick_bytes = 0;
    g_sftp_transfer.speed_bps = 0;
}

void sftp_transfer_tick(u64 new_bytes_done) {
    g_sftp_transfer.bytes_done = new_bytes_done;
    f64 now = platform_time_sec();
    f64 dt = now - g_sftp_transfer.last_tick_time;
    if (dt >= 0.2) {
        u64 db = new_bytes_done > g_sftp_transfer.last_tick_bytes
                   ? new_bytes_done - g_sftp_transfer.last_tick_bytes : 0;
        f64 inst = dt > 0 ? (f64)db / dt : 0;
        /* EMA: weight new sample 0.4, history 0.6. First sample seeds. */
        if (g_sftp_transfer.speed_bps <= 0) g_sftp_transfer.speed_bps = inst;
        else g_sftp_transfer.speed_bps = g_sftp_transfer.speed_bps * 0.6 + inst * 0.4;
        g_sftp_transfer.last_tick_time = now;
        g_sftp_transfer.last_tick_bytes = new_bytes_done;
    }
}

void sftp_transfer_end(void) {
    g_sftp_transfer.active = false;
}

/* Legacy alias kept for internal callers below. */
#define transfer_state_begin(p, t, u) sftp_transfer_begin(p, t, u)
#define transfer_state_end()          sftp_transfer_end()

/* Create local directory (and parents) recursively. Returns 0 on success. */
static i32 mkdir_recursive(const char *path) {
    char tmp[SFTP_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    usize len = strlen(tmp);

    /* Remove trailing slash */
    if (len > 0 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* Create remote directory (and parents) recursively. Returns 0 on success. */
static i32 sftp_mkdir_recursive(LIBSSH2_SFTP *sftp, const char *path) {
    char tmp[SFTP_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    usize len = strlen(tmp);

    if (len > 0 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            /* Try to create; ignore EEXIST-like errors */
            libssh2_sftp_mkdir(sftp, tmp,
                LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP);
            *p = '/';
        }
    }
    i32 rc = libssh2_sftp_mkdir(sftp, tmp,
        LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP);
    /* Success or already exists */
    if (rc == 0) return 0;
    u64 err = libssh2_sftp_last_error(sftp);
    return (err == LIBSSH2_FX_FILE_ALREADY_EXISTS) ? 0 : -1;
}

/* =========================================================================
 * Directory listing
 * ========================================================================= */

i32 sftp_list_dir(void *sftp_ptr, const char *path, SftpEntry *entries, i32 max_entries) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;

    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(sftp, path);
    if (!dir) return -1;

    i32 count = 0;
    char buf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (count < max_entries) {
        int rc = libssh2_sftp_readdir(dir, buf, sizeof(buf), &attrs);
        if (rc <= 0) break;

        if (buf[0] == '.' && (buf[1] == '\0' || (buf[1] == '.' && buf[2] == '\0')))
            continue;

        SftpEntry *e = &entries[count];
        snprintf(e->name, sizeof(e->name), "%s", buf);
        e->size = attrs.filesize;
        e->permissions = attrs.permissions;
        e->mtime = attrs.mtime;
        e->is_dir = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        count++;
    }

    libssh2_sftp_closedir(dir);
    return count;
}

/* =========================================================================
 * Basic file operations
 * ========================================================================= */

i32 sftp_download(void *sftp_ptr, const char *remote_path, const char *local_path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;

    u64 total = sftp_remote_file_size(sftp, remote_path);

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(sftp, remote_path,
        LIBSSH2_FXF_READ, 0);
    if (!handle) return -1;

    FILE *f = fopen(local_path, "wb");
    if (!f) {
        libssh2_sftp_close(handle);
        return -1;
    }

    transfer_state_begin(remote_path, total, false);

    char buf[SFTP_BUF_SIZE];
    i32 result = 0;
    u64 done = 0;
    for (;;) {
        ssize_t n = libssh2_sftp_read(handle, buf, sizeof(buf));
        if (n == 0) break;
        if (n == LIBSSH2_ERROR_EAGAIN) { usleep(1000); continue; }
        if (n < 0) { result = -1; break; }
        if (fwrite(buf, 1, (usize)n, f) != (usize)n) { result = -1; break; } /* disk full / write error */
        done += (u64)n;
        sftp_transfer_tick(done);
    }

    transfer_state_end();
    fclose(f);
    libssh2_sftp_close(handle);
    return result;
}

i32 sftp_upload(void *sftp_ptr, const char *local_path, const char *remote_path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;

    FILE *f = fopen(local_path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    u64 total = (u64)ftell(f);
    fseek(f, 0, SEEK_SET);

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(sftp, remote_path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!handle) {
        fclose(f);
        return -1;
    }

    transfer_state_begin(local_path, total, true);

    char buf[SFTP_BUF_SIZE];
    usize nread;
    u64 done = 0;
    while ((nread = fread(buf, 1, sizeof(buf), f)) > 0) {
        ssize_t written = 0;
        while ((usize)written < nread) {
            ssize_t n = libssh2_sftp_write(handle, buf + written, nread - (usize)written);
            if (n < 0) {
                transfer_state_end();
                fclose(f);
                libssh2_sftp_close(handle);
                return -1;
            }
            written += n;
            done += (u64)n;
            sftp_transfer_tick(done);
        }
    }

    transfer_state_end();
    fclose(f);
    libssh2_sftp_close(handle);
    return 0;
}

i32 sftp_mkdir(void *sftp_ptr, const char *path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;
    return libssh2_sftp_mkdir(sftp, path,
        LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP);
}

i32 sftp_remove(void *sftp_ptr, const char *path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;
    return libssh2_sftp_unlink(sftp, path);
}

i32 sftp_rename(void *sftp_ptr, const char *old_path, const char *new_path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;
    return libssh2_sftp_rename(sftp, old_path, new_path);
}

/* =========================================================================
 * Transfer progress (global state for UI)
 * ========================================================================= */

bool        sftp_transfer_active(void)   { return g_sftp_transfer.active; }
const char *sftp_transfer_filename(void) { return g_sftp_transfer.filename; }
u64         sftp_transfer_bytes_done(void)  { return g_sftp_transfer.bytes_done; }
u64         sftp_transfer_bytes_total(void) { return g_sftp_transfer.bytes_total; }
bool        sftp_transfer_is_upload(void)   { return g_sftp_transfer.is_upload; }
f64         sftp_transfer_speed_bps(void)   { return g_sftp_transfer.speed_bps; }

f64 sftp_transfer_elapsed_sec(void) {
    if (!g_sftp_transfer.active && g_sftp_transfer.start_time == 0) return 0;
    return platform_time_sec() - g_sftp_transfer.start_time;
}

f64 sftp_transfer_eta_sec(void) {
    if (g_sftp_transfer.bytes_total == 0) return -1;  /* unknown size */
    if (g_sftp_transfer.speed_bps <= 1.0) return -1;  /* no rate yet */
    if (g_sftp_transfer.bytes_done >= g_sftp_transfer.bytes_total) return 0;
    u64 remaining = g_sftp_transfer.bytes_total - g_sftp_transfer.bytes_done;
    return (f64)remaining / g_sftp_transfer.speed_bps;
}

/* =========================================================================
 * Resume (partial file transfer)
 * ========================================================================= */

i64 sftp_download_resume(void *sftp_ptr, const char *remote_path, const char *local_path,
                         SFTPProgressCallback progress, void *userdata) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;

    /* Get remote file size */
    u64 remote_size = sftp_remote_file_size(sftp, remote_path);
    if (remote_size == 0) return -1;

    /* Check if local file exists and get its size */
    u64 local_size = sftp_local_file_size(local_path);
    u64 offset = 0;
    const char *mode = "wb";

    if (local_size > 0 && local_size < remote_size) {
        /* Resume: append from where we left off */
        offset = local_size;
        mode = "ab";
    } else if (local_size >= remote_size) {
        /* File already complete */
        if (progress) {
            const char *fname = strrchr(remote_path, '/');
            progress(fname ? fname + 1 : remote_path, (i64)remote_size, (i64)remote_size, userdata);
        }
        return 0;
    }

    /* Open remote file */
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(sftp, remote_path,
        LIBSSH2_FXF_READ, 0);
    if (!handle) return -1;

    /* Seek to resume position */
    if (offset > 0) {
        libssh2_sftp_seek64(handle, offset);
    }

    /* Open local file */
    FILE *f = fopen(local_path, mode);
    if (!f) {
        libssh2_sftp_close(handle);
        return -1;
    }

    transfer_state_begin(remote_path, remote_size, false);
    g_sftp_transfer.bytes_done = offset;

    const char *fname = strrchr(remote_path, '/');
    const char *display_name = fname ? fname + 1 : remote_path;

    char buf[SFTP_BUF_SIZE];
    i64 bytes_transferred = 0;

    for (;;) {
        ssize_t n = libssh2_sftp_read(handle, buf, sizeof(buf));
        if (n == 0) break;  /* EOF */
        if (n == LIBSSH2_ERROR_EAGAIN) { usleep(1000); continue; }
        if (n < 0) {
            transfer_state_end();
            fclose(f);
            libssh2_sftp_close(handle);
            return -1;
        }

        usize written = fwrite(buf, 1, (usize)n, f);
        if (written != (usize)n) {
            transfer_state_end();
            fclose(f);
            libssh2_sftp_close(handle);
            return -1;
        }

        bytes_transferred += (i64)n;
        g_sftp_transfer.bytes_done += (u64)n;

        if (progress) {
            progress(display_name, (i64)(offset + (u64)bytes_transferred),
                     (i64)remote_size, userdata);
        }
    }

    transfer_state_end();
    fclose(f);
    libssh2_sftp_close(handle);
    return bytes_transferred;
}

i64 sftp_upload_resume(void *sftp_ptr, const char *local_path, const char *remote_path,
                       SFTPProgressCallback progress, void *userdata) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;

    /* Get local file size */
    FILE *f = fopen(local_path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    u64 local_size = (u64)ftell(f);
    fseek(f, 0, SEEK_SET);

    if (local_size == 0) {
        fclose(f);
        return -1;
    }

    /* Check remote file size for resume */
    u64 remote_size = sftp_remote_file_size(sftp, remote_path);
    u64 offset = 0;
    u32 open_flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT;

    if (remote_size > 0 && remote_size < local_size) {
        /* Resume: append from where we left off */
        offset = remote_size;
        /* Don't truncate, we want to append */
    } else if (remote_size >= local_size) {
        /* File already complete */
        fclose(f);
        if (progress) {
            const char *fname = strrchr(local_path, '/');
            progress(fname ? fname + 1 : local_path, (i64)local_size, (i64)local_size, userdata);
        }
        return 0;
    } else {
        /* New file: truncate */
        open_flags |= LIBSSH2_FXF_TRUNC;
    }

    /* Open remote file */
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(sftp, remote_path, open_flags,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!handle) {
        fclose(f);
        return -1;
    }

    /* Seek both local and remote to resume position */
    if (offset > 0) {
        fseek(f, (long)offset, SEEK_SET);
        libssh2_sftp_seek64(handle, offset);
    }

    transfer_state_begin(local_path, local_size, true);
    g_sftp_transfer.bytes_done = offset;

    const char *fname = strrchr(local_path, '/');
    const char *display_name = fname ? fname + 1 : local_path;

    char buf[SFTP_BUF_SIZE];
    i64 bytes_transferred = 0;
    usize nread;

    while ((nread = fread(buf, 1, sizeof(buf), f)) > 0) {
        ssize_t total_written = 0;
        while ((usize)total_written < nread) {
            ssize_t n = libssh2_sftp_write(handle, buf + total_written,
                                           nread - (usize)total_written);
            if (n < 0) {
                transfer_state_end();
                fclose(f);
                libssh2_sftp_close(handle);
                return -1;
            }
            total_written += n;
            bytes_transferred += (i64)n;
            g_sftp_transfer.bytes_done += (u64)n;

            if (progress) {
                progress(display_name, (i64)(offset + (u64)bytes_transferred),
                         (i64)local_size, userdata);
            }
        }
    }

    transfer_state_end();
    fclose(f);
    libssh2_sftp_close(handle);
    return bytes_transferred;
}

/* =========================================================================
 * Sync helpers (internal)
 * ========================================================================= */

/* Recursive SFTP directory listing for sync.
 * Calls callback for each file found (not directories).
 * rel_path is the path relative to the sync root. */
typedef void (*SyncFileVisitor)(const char *rel_path, u64 size, u64 mtime,
                                bool is_dir, void *ctx);

/* A directory entry name must be a single, safe path component. A hostile
 * SFTP server can return "..", embedded slashes, or backslashes in a filename;
 * joined into a local path these escape the sync root (path traversal). */
static bool sftp_name_is_component(const char *name) {
    if (!name || !name[0]) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') return false;
    return true;
}

static void sftp_walk_remote(LIBSSH2_SFTP *sftp, const char *base_path,
                             const char *rel_prefix, SyncFileVisitor visitor, void *ctx) {
    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(sftp, base_path);
    if (!dir) return;

    char buf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (1) {
        int rc = libssh2_sftp_readdir(dir, buf, sizeof(buf), &attrs);
        if (rc <= 0) break;

        /* Reject anything that is not a single safe path component (".", "..",
         * or a name containing a path separator) — these would let a hostile
         * server escape the local sync root via path traversal. */
        if (!sftp_name_is_component(buf))
            continue;

        /* Skip dotfiles/hidden files */
        if (is_dotfile(buf))
            continue;

        char rel_path[SFTP_MAX_PATH];
        if (rel_prefix[0] != '\0')
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, buf);
        else
            snprintf(rel_path, sizeof(rel_path), "%s", buf);

        bool dir_entry = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        visitor(rel_path, attrs.filesize, attrs.mtime, dir_entry, ctx);

        if (dir_entry) {
            char full_path[SFTP_MAX_PATH];
            snprintf(full_path, sizeof(full_path), "%s/%s", base_path, buf);
            sftp_walk_remote(sftp, full_path, rel_path, visitor, ctx);
        }
    }

    libssh2_sftp_closedir(dir);
}

static void local_walk(const char *base_path, const char *rel_prefix,
                       SyncFileVisitor visitor, void *ctx) {
    DIR *d = opendir(base_path);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d))) {
        /* Skip . and .. */
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;

        /* Skip dotfiles/hidden files */
        if (is_dotfile(de->d_name))
            continue;

        char full_path[SFTP_MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, de->d_name);

        char rel_path[SFTP_MAX_PATH];
        if (rel_prefix[0] != '\0')
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, de->d_name);
        else
            snprintf(rel_path, sizeof(rel_path), "%s", de->d_name);

        /* lstat (not stat) so we see the symlink itself, not its target.
         * Following symlinks here is dangerous: a symlink inside the sync
         * root could point outside of it and on the upload side we would
         * leak those files to the remote; on the download side delete_extra
         * could walk through a symlink and remove unrelated local files. */
        struct stat st;
        if (lstat(full_path, &st) != 0) continue;

        /* Skip symlinks entirely. We only sync regular files and directories
         * that genuinely live inside the sync root. */
        if (S_ISLNK(st.st_mode)) continue;

        bool is_dir_entry = S_ISDIR(st.st_mode);
        visitor(rel_path, (u64)st.st_size, (u64)st.st_mtime, is_dir_entry, ctx);

        if (is_dir_entry) {
            local_walk(full_path, rel_path, visitor, ctx);
        }
    }

    closedir(d);
}

/* File entry for sync comparison. `path` is heap-duplicated (not a 4 KB inline
 * SFTP_MAX_PATH buffer) — a 10k-file tree was ~41 MB of mostly-empty inline
 * path storage; real relative paths are tens of bytes. All readers treat it as
 * a plain C string, so only push/free below own the allocation. */
typedef struct SyncEntry {
    char *path;
    u64  size;
    u64  mtime;
    bool is_dir;
} SyncEntry;

/* Dynamic list of sync entries */
typedef struct {
    SyncEntry *items;
    i32        count;
    i32        capacity;
    bool       oom;        /* set if a push was dropped by a realloc failure */
} SyncEntryList;

static void sync_entry_list_init(SyncEntryList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    list->oom = false;
}

static void sync_entry_list_free(SyncEntryList *list) {
    for (i32 i = 0; i < list->count; i++) free(list->items[i].path);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void sync_entry_list_push(SyncEntryList *list, const char *path,
                                  u64 size, u64 mtime, bool is_dir_entry) {
    if (list->count >= list->capacity) {
        i32 new_cap = list->capacity == 0 ? 64 : list->capacity * 2;
        SyncEntry *new_items = realloc(list->items, (usize)new_cap * sizeof(SyncEntry));
        if (!new_items) {
            /* Record the truncation so the caller can refuse delete_extra — a
             * dropped entry on the comparison side would otherwise delete a
             * file whose counterpart we simply failed to list. */
            list->oom = true;
            return;
        }
        list->items = new_items;
        list->capacity = new_cap;
    }
    char *dup = strdup(path ? path : "");
    if (!dup) { list->oom = true; return; }
    SyncEntry *e = &list->items[list->count++];
    e->path = dup;
    e->size = size;
    e->mtime = mtime;
    e->is_dir = is_dir_entry;
}

static void sync_visitor(const char *rel_path, u64 size, u64 mtime,
                          bool is_dir_entry, void *ctx) {
    SyncEntryList *list = (SyncEntryList *)ctx;
    sync_entry_list_push(list, rel_path, size, mtime, is_dir_entry);
}

/* =========================================================================
 * Sync index: open-addressed hash table keyed by relative path
 *
 * The original find/contains helpers were O(n) each, so a full sync was
 * O(n^2) in entry count — syncing a tree with a few thousand files spent
 * real time in strcmp loops. This hash index gives O(1) average lookup;
 * sync now scales linearly with directory size. We rebuild the index per
 * sync call (it's cheap) and keep the path strings living in SyncEntryList.
 * ========================================================================= */

typedef struct {
    i32       *slots;     /* -1 empty, else index into SyncEntryList::items */
    i32        cap;       /* power of two */
    const SyncEntryList *list;
} SyncIndex;

static u64 fnv1a64(const char *s) {
    u64 h = 0xcbf29ce484222325ULL;
    for (; *s; s++) {
        h ^= (u8)*s;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static void sync_index_build(SyncIndex *ix, const SyncEntryList *list) {
    ix->list = list;
    i32 cap = 16;
    while (cap < list->count * 2) cap <<= 1;
    ix->cap = cap;
    ix->slots = malloc((usize)cap * sizeof(i32));
    if (!ix->slots) { ix->cap = 0; return; }
    for (i32 i = 0; i < cap; i++) ix->slots[i] = -1;
    u64 mask = (u64)(cap - 1);
    for (i32 i = 0; i < list->count; i++) {
        u64 h = fnv1a64(list->items[i].path);
        u64 s = h & mask;
        while (ix->slots[s] != -1) s = (s + 1) & mask;
        ix->slots[s] = i;
    }
}

static void sync_index_free(SyncIndex *ix) {
    free(ix->slots);
    ix->slots = NULL;
    ix->cap = 0;
    ix->list = NULL;
}

static const SyncEntry *sync_index_find(const SyncIndex *ix, const char *rel_path) {
    if (!ix->slots || !ix->list) return NULL;
    u64 mask = (u64)(ix->cap - 1);
    u64 s = fnv1a64(rel_path) & mask;
    while (ix->slots[s] != -1) {
        const SyncEntry *e = &ix->list->items[ix->slots[s]];
        if (strcmp(e->path, rel_path) == 0) return e;
        s = (s + 1) & mask;
    }
    return NULL;
}

static bool sync_index_contains(const SyncIndex *ix, const char *rel_path) {
    return sync_index_find(ix, rel_path) != NULL;
}

/* Remove remote file or directory recursively */
static void sftp_remove_recursive(LIBSSH2_SFTP *sftp, const char *path, bool is_dir_entry) {
    if (!is_dir_entry) {
        libssh2_sftp_unlink(sftp, path);
        return;
    }

    /* Recursively remove directory contents first */
    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(sftp, path);
    if (!dir) return;

    char buf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (1) {
        int rc = libssh2_sftp_readdir(dir, buf, sizeof(buf), &attrs);
        if (rc <= 0) break;

        if (buf[0] == '.' && (buf[1] == '\0' || (buf[1] == '.' && buf[2] == '\0')))
            continue;

        char child[SFTP_MAX_PATH];
        snprintf(child, sizeof(child), "%s/%s", path, buf);
        sftp_remove_recursive(sftp, child, LIBSSH2_SFTP_S_ISDIR(attrs.permissions));
    }

    libssh2_sftp_closedir(dir);
    libssh2_sftp_rmdir(sftp, path);
}

/* Remove local file or directory recursively.
 *
 * Symlink safety: we lstat every entry and never traverse into a symlink's
 * target. A symlink itself is removed with unlink() (which operates on the
 * link, not the target). This matches the symlink-skip policy in local_walk
 * — if the walker never recorded symlinks as entries to reconcile, the only
 * way one gets here is through a directory we own that happens to contain
 * a link we didn't record, and we still refuse to descend through it. */
static void local_remove_recursive(const char *path, bool is_dir_entry) {
    if (!is_dir_entry) {
        unlink(path);
        return;
    }

    /* Re-check the path on disk — if it turned into a symlink since we
     * listed it, refuse to recurse. */
    struct stat top;
    if (lstat(path, &top) != 0) return;
    if (!S_ISDIR(top.st_mode)) {
        unlink(path);
        return;
    }

    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;

        char child[SFTP_MAX_PATH];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);

        struct stat st;
        if (lstat(child, &st) != 0) continue;

        if (S_ISLNK(st.st_mode)) {
            /* Unlink the symlink itself — do not follow it. */
            unlink(child);
            continue;
        }
        local_remove_recursive(child, S_ISDIR(st.st_mode));
    }

    closedir(d);
    rmdir(path);
}

/* =========================================================================
 * Sync: download (remote -> local)
 * ========================================================================= */

SFTPSyncResult sftp_sync_download(void *sftp_ptr, const char *remote_dir, const char *local_dir,
                                  bool delete_extra, SFTPProgressCallback progress, void *userdata) {
    SFTPSyncResult result = {0};
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;

    if (!sftp) {
        result.files_failed = -1;
        return result;
    }

    /* Ensure local directory exists */
    mkdir_recursive(local_dir);

    /* Collect remote file listing */
    SyncEntryList remote_list;
    sync_entry_list_init(&remote_list);
    sftp_walk_remote(sftp, remote_dir, "", sync_visitor, &remote_list);

    /* Collect local file listing */
    SyncEntryList local_list;
    sync_entry_list_init(&local_list);
    local_walk(local_dir, "", sync_visitor, &local_list);

    /* A truncated listing (allocation failure) must never drive deletions:
     * proceeding with delete_extra could remove a local file whose remote
     * counterpart we simply failed to record. Disable deletion and flag the
     * sync as incomplete; the already-collected transfers are still safe. */
    if (remote_list.oom || local_list.oom) {
        delete_extra = false;
        result.files_failed = -1;
    }

    /* Hash indexes so the reconcile loop is O(n) instead of O(n^2). */
    SyncIndex remote_ix, local_ix;
    sync_index_build(&remote_ix, &remote_list);
    sync_index_build(&local_ix,  &local_list);

    /* Transfer new/changed files from remote to local */
    for (i32 i = 0; i < remote_list.count; i++) {
        const SyncEntry *re = &remote_list.items[i];

        char remote_full[SFTP_MAX_PATH];
        snprintf(remote_full, sizeof(remote_full), "%s/%s", remote_dir, re->path);

        char local_full[SFTP_MAX_PATH];
        snprintf(local_full, sizeof(local_full), "%s/%s", local_dir, re->path);

        if (re->is_dir) {
            /* Create local directory if needed */
            mkdir_recursive(local_full);
            continue;
        }

        /* Check if local file needs updating */
        const SyncEntry *le = sync_index_find(&local_ix, re->path);
        if (le && le->size == re->size && le->mtime >= re->mtime) {
            result.files_skipped++;
            continue;
        }

        /* Ensure parent directory exists */
        char parent[SFTP_MAX_PATH];
        snprintf(parent, sizeof(parent), "%s", local_full);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            mkdir_recursive(parent);
        }

        /* Download the file */
        i32 rc = sftp_download(sftp_ptr, remote_full, local_full);
        if (rc == 0) {
            result.files_transferred++;
            result.bytes_transferred += (i64)re->size;
            if (progress) {
                progress(re->path, (i64)re->size, (i64)re->size, userdata);
            }
        } else {
            result.files_failed++;
        }
    }

    /* Delete extra local files not present on remote */
    if (delete_extra) {
        for (i32 i = 0; i < local_list.count; i++) {
            const SyncEntry *le = &local_list.items[i];
            if (!sync_index_contains(&remote_ix, le->path)) {
                char local_full[SFTP_MAX_PATH];
                snprintf(local_full, sizeof(local_full), "%s/%s", local_dir, le->path);
                local_remove_recursive(local_full, le->is_dir);
            }
        }
    }

    sync_index_free(&remote_ix);
    sync_index_free(&local_ix);
    sync_entry_list_free(&remote_list);
    sync_entry_list_free(&local_list);
    return result;
}

/* =========================================================================
 * Sync: upload (local -> remote)
 * ========================================================================= */

SFTPSyncResult sftp_sync_upload(void *sftp_ptr, const char *local_dir, const char *remote_dir,
                                bool delete_extra, SFTPProgressCallback progress, void *userdata) {
    SFTPSyncResult result = {0};
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;

    if (!sftp) {
        result.files_failed = -1;
        return result;
    }

    /* Ensure remote directory exists */
    sftp_mkdir_recursive(sftp, remote_dir);

    /* Collect local file listing */
    SyncEntryList local_list;
    sync_entry_list_init(&local_list);
    local_walk(local_dir, "", sync_visitor, &local_list);

    /* Collect remote file listing */
    SyncEntryList remote_list;
    sync_entry_list_init(&remote_list);
    sftp_walk_remote(sftp, remote_dir, "", sync_visitor, &remote_list);

    /* A truncated listing (allocation failure) must never drive deletions —
     * see sftp_sync_download. Disable delete_extra and flag incompleteness. */
    if (remote_list.oom || local_list.oom) {
        delete_extra = false;
        result.files_failed = -1;
    }

    /* Hash indexes so reconcile is O(n), not O(n^2). */
    SyncIndex local_ix, remote_ix;
    sync_index_build(&local_ix,  &local_list);
    sync_index_build(&remote_ix, &remote_list);

    /* Transfer new/changed files from local to remote */
    for (i32 i = 0; i < local_list.count; i++) {
        const SyncEntry *le = &local_list.items[i];

        char local_full[SFTP_MAX_PATH];
        snprintf(local_full, sizeof(local_full), "%s/%s", local_dir, le->path);

        char remote_full[SFTP_MAX_PATH];
        snprintf(remote_full, sizeof(remote_full), "%s/%s", remote_dir, le->path);

        if (le->is_dir) {
            sftp_mkdir_recursive(sftp, remote_full);
            continue;
        }

        /* Check if remote file needs updating */
        const SyncEntry *re = sync_index_find(&remote_ix, le->path);
        if (re && re->size == le->size && re->mtime >= le->mtime) {
            result.files_skipped++;
            continue;
        }

        /* Ensure parent remote directory exists */
        char parent[SFTP_MAX_PATH];
        snprintf(parent, sizeof(parent), "%s", remote_full);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            sftp_mkdir_recursive(sftp, parent);
        }

        /* Upload the file */
        i32 rc = sftp_upload(sftp_ptr, local_full, remote_full);
        if (rc == 0) {
            result.files_transferred++;
            result.bytes_transferred += (i64)le->size;
            if (progress) {
                progress(le->path, (i64)le->size, (i64)le->size, userdata);
            }
        } else {
            result.files_failed++;
        }
    }

    /* Delete extra remote files not present locally */
    if (delete_extra) {
        for (i32 i = 0; i < remote_list.count; i++) {
            const SyncEntry *re = &remote_list.items[i];
            if (!sync_index_contains(&local_ix, re->path)) {
                char remote_full[SFTP_MAX_PATH];
                snprintf(remote_full, sizeof(remote_full), "%s/%s", remote_dir, re->path);
                sftp_remove_recursive(sftp, remote_full, re->is_dir);
            }
        }
    }

    sync_index_free(&local_ix);
    sync_index_free(&remote_ix);
    sync_entry_list_free(&local_list);
    sync_entry_list_free(&remote_list);
    return result;
}

/* =========================================================================
 * Symlink operations
 * ========================================================================= */

bool sftp_create_symlink(void *sftp_ptr, const char *target, const char *link_path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return false;

    /* libssh2_sftp_symlink_ex with LIBSSH2_SFTP_SYMLINK creates a symlink.
     * Parameters: sftp, path (target), path_len, target (link_path), target_len, link_type
     * Note: libssh2 naming is confusing — the first path arg is the target of the link,
     * and the second is where the symlink itself is created. */
    int rc = libssh2_sftp_symlink_ex(sftp,
        target, (u32)strlen(target),
        (char *)link_path, (u32)strlen(link_path),
        LIBSSH2_SFTP_SYMLINK);
    return rc == 0;
}

bool sftp_read_symlink(void *sftp_ptr, const char *link_path, char *target, i32 target_size) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp || !target || target_size <= 0) return false;

    int rc = libssh2_sftp_symlink_ex(sftp,
        link_path, (u32)strlen(link_path),
        target, (u32)target_size,
        LIBSSH2_SFTP_READLINK);

    if (rc < 0) return false;

    /* Ensure null termination */
    if (rc < target_size)
        target[rc] = '\0';
    else
        target[target_size - 1] = '\0';

    return true;
}

bool sftp_remove_symlink(void *sftp_ptr, const char *link_path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return false;

    /* unlink works on symlinks — removes the link itself, not the target */
    return libssh2_sftp_unlink(sftp, link_path) == 0;
}
