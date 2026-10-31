/*
 * Liu - SFTP operations via libssh2
 * Basic transfers, resume, directory sync, symlink management.
 */
#include "ssh/sftp.h"
#include "ssh/transfer_mode.h"
#include "ssh/local_destination.h"
#include "ssh/ssh_session.h"
#include "core/checked.h"
#include "platform/platform.h"   /* platform_time_sec for transfer ticks */
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

/* Transfer buffer size (32 KB) */
#define SFTP_BUF_SIZE  32768

/* Max path length for constructed paths */
#define SFTP_MAX_PATH  4096

/* Max recursion depth for the sync/remove directory walkers. A hostile SFTP
 * server can return an arbitrarily deep tree; each recursion frame carries
 * up to two SFTP_MAX_PATH buffers (~8.7 KB), so unbounded recursion overflows
 * the C stack (~58 frames on a 512 KB thread stack) long before the path-length
 * bound would stop it. Cap depth conservatively so it stays safe even on the
 * smallest thread stack. */
#define SFTP_MAX_SYNC_DEPTH  40

/* Per-transfer progress state for UI display.
 *
 * Transfers run on several worker threads (fileops' recursive helpers start
 * one per file), so each transfer owns a slot and reports against its own id.
 * With one shared record every transfer would write its byte count into the
 * same field: the panel's progress bar would jump between them, and speed and
 * ETA, which are derived from consecutive samples, would be computed across
 * unrelated streams. The panel shows the most recently started live transfer,
 * plus the count and the aggregate.
 *
 * One mutex for the table. It is held for a handful of field writes; the
 * expensive part of a tick (the speed EMA) is throttled to ~0.2 s and the rest
 * is two stores. With SFTP_MAX_TRANSFERS bounded at 16 there is nothing here
 * worth sharding a lock for. */
#define SFTP_MAX_TRANSFERS 16

typedef struct {
    u32  id;                 /* 0 = free slot */
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
    u64  seq;                /* start order, for picking the focused one */
} SftpTransferSlot;

static struct {
    SftpTransferSlot slot[SFTP_MAX_TRANSFERS];
    u32 next_id;
    u64 next_seq;
    unsigned active_count;
} g_sftp_transfer = {0};
static pthread_mutex_t g_sftp_transfer_lock = PTHREAD_MUTEX_INITIALIZER;

/* The transfer the panel describes: the most recently started live one. Call
 * with the lock held; the pointer is valid only while it is. */
static SftpTransferSlot *sftp_focus_locked(void) {
    SftpTransferSlot *best = NULL;
    for (i32 i = 0; i < SFTP_MAX_TRANSFERS; i++) {
        SftpTransferSlot *t = &g_sftp_transfer.slot[i];
        if (!t->id) continue;
        if (!best || t->seq > best->seq) best = t;
    }
    return best;
}

static SftpTransferSlot *sftp_slot_locked(u32 id) {
    if (!id) return NULL;
    for (i32 i = 0; i < SFTP_MAX_TRANSFERS; i++)
        if (g_sftp_transfer.slot[i].id == id) return &g_sftp_transfer.slot[i];
    return NULL;
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Stat a remote path. Returns false when it doesn't exist (or can't be
 * stat'd). Callers that need to tell "absent" from "zero bytes" apart must
 * use this rather than the size-only helper below — a resume path that read
 * an empty file as missing would refuse to transfer it. */
static bool sftp_remote_stat_ex(LIBSSH2_SFTP *sftp, const char *path,
                                u64 *out_size, u64 *out_perms) {
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int rc;
    SFTP_CALL(rc, libssh2_sftp_stat(sftp, path, &attrs));
    if (rc != 0) return false;
    if (out_size)  *out_size  = attrs.filesize;
    if (out_perms) *out_perms = attrs.permissions;
    return true;
}

const char *sftp_last_error_str(void *sftp_ptr) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return "SFTP is not available on this session";
    switch (sftp_io_last_status(sftp)) {
        case LIBSSH2_FX_OK:                     return "Transfer failed";
        case LIBSSH2_FX_NO_SUCH_FILE:           /* fallthrough */
        case LIBSSH2_FX_NO_SUCH_PATH:           return "No such file or directory";
        case LIBSSH2_FX_PERMISSION_DENIED:      return "Permission denied";
        case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM: return "No space left on the server";
        case LIBSSH2_FX_QUOTA_EXCEEDED:         return "Quota exceeded";
        case LIBSSH2_FX_WRITE_PROTECT:          return "Write-protected";
        case LIBSSH2_FX_FILE_ALREADY_EXISTS:    return "File already exists";
        case LIBSSH2_FX_NOT_A_DIRECTORY:        return "Not a directory";
        case LIBSSH2_FX_DIR_NOT_EMPTY:          return "Directory not empty";
        case LIBSSH2_FX_INVALID_FILENAME:       return "Invalid file name";
        case LIBSSH2_FX_OP_UNSUPPORTED:         return "Operation not supported by the server";
        case LIBSSH2_FX_CONNECTION_LOST:        return "Connection lost";
        case LIBSSH2_FX_NO_CONNECTION:          return "Not connected";
        default:                                return "Server refused the transfer";
    }
}

/* Get remote file attributes. Returns true on success. */
__attribute__((unused)) static bool sftp_remote_stat(LIBSSH2_SFTP *sftp, const char *path, LIBSSH2_SFTP_ATTRIBUTES *attrs) {
    int rc;
    SFTP_CALL(rc, libssh2_sftp_stat(sftp, path, attrs));
    return rc == 0;
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

/* A directory entry name must be a single, safe path component. A hostile
 * SFTP server can return "..", embedded slashes, or backslashes in a filename;
 * joined into a local path these escape whatever directory the user is looking
 * at (path traversal). Exported so every consumer of a remote listing — sync
 * walker, file browser, clipboard paste — gates on the same rule. */
bool sftp_name_is_safe_component(const char *name) {
    if (!name || !name[0]) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') return false;
    return true;
}

/* Public transfer progress API — exported via sftp.h so fileops.c's
 * recursive helpers and any other transfer path can participate in the
 * shared overlay. `sftp_transfer_begin` resets speed tracking; `tick` is
 * meant to be called after every chunk and updates the smoothed speed at
 * most every ~0.2 s. `end` clears `active`; leave filename/bytes alone so
 * the UI can hold the panel for a moment after completion if desired. */
u32 sftp_transfer_begin(const char *path, u64 total, bool upload) {
    if (!path) path = "";
    const char *fname = strrchr(path, '/');
    f64 now = platform_time_sec();
    u32 id = 0;
    pthread_mutex_lock(&g_sftp_transfer_lock);
    for (i32 i = 0; i < SFTP_MAX_TRANSFERS; i++) {
        SftpTransferSlot *t = &g_sftp_transfer.slot[i];
        if (t->id) continue;
        memset(t, 0, sizeof *t);
        /* Ids never repeat within a run, so a stale handle from a finished
         * transfer can never land on somebody else's slot. */
        if (++g_sftp_transfer.next_id == 0) g_sftp_transfer.next_id = 1;
        t->id = g_sftp_transfer.next_id;
        snprintf(t->filename, sizeof t->filename, "%s", fname ? fname + 1 : path);
        t->bytes_total     = total;
        t->is_upload       = upload;
        t->start_time      = now;
        t->last_tick_time  = now;
        t->seq             = g_sftp_transfer.next_seq++;
        if (g_sftp_transfer.active_count < UINT_MAX)
            g_sftp_transfer.active_count++;
        id = t->id;
        break;
    }
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    /* The table is full: the transfer still runs, it just goes unreported.
     * Reporting is a UI convenience, never a precondition for moving bytes. */
    return id;
}

void sftp_transfer_tick(u32 id, u64 new_bytes_done) {
    f64 now = platform_time_sec();
    pthread_mutex_lock(&g_sftp_transfer_lock);
    SftpTransferSlot *t = sftp_slot_locked(id);
    if (t) {
        t->bytes_done = new_bytes_done;
        f64 dt = now - t->last_tick_time;
        if (dt >= 0.2) {
            u64 db = new_bytes_done > t->last_tick_bytes
                       ? new_bytes_done - t->last_tick_bytes : 0;
            f64 inst = dt > 0 ? (f64)db / dt : 0;
            /* EMA: weight new sample 0.4, history 0.6. First sample seeds. */
            if (t->speed_bps <= 0) t->speed_bps = inst;
            else t->speed_bps = t->speed_bps * 0.6 + inst * 0.4;
            t->last_tick_time  = now;
            t->last_tick_bytes = new_bytes_done;
        }
    }
    pthread_mutex_unlock(&g_sftp_transfer_lock);
}

void sftp_transfer_end(u32 id) {
    pthread_mutex_lock(&g_sftp_transfer_lock);
    SftpTransferSlot *t = sftp_slot_locked(id);
    if (t) {
        memset(t, 0, sizeof *t);
        if (g_sftp_transfer.active_count > 0) g_sftp_transfer.active_count--;
    }
    pthread_mutex_unlock(&g_sftp_transfer_lock);
}

u32 sftp_transfer_count(void) {
    pthread_mutex_lock(&g_sftp_transfer_lock);
    u32 n = g_sftp_transfer.active_count;
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    return n;
}

void sftp_transfer_totals(u64 *out_done, u64 *out_total) {
    u64 done = 0, total = 0;
    pthread_mutex_lock(&g_sftp_transfer_lock);
    for (i32 i = 0; i < SFTP_MAX_TRANSFERS; i++) {
        const SftpTransferSlot *t = &g_sftp_transfer.slot[i];
        if (!t->id) continue;
        done  += t->bytes_done;
        total += t->bytes_total;
    }
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    if (out_done)  *out_done  = done;
    if (out_total) *out_total = total;
}

/* Internal shorthand. Each transfer keeps its own handle in a local `xfer_id`
 * so concurrent transfers cannot report against each other. */
#define transfer_state_begin(p, t, u) (xfer_id = sftp_transfer_begin(p, t, u))
#define transfer_state_end()          sftp_transfer_end(xfer_id)

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
            int ignored;
            SFTP_CALL(ignored, libssh2_sftp_mkdir(sftp, tmp,
                LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP));
            (void)ignored;
            *p = '/';
        }
    }
    i32 rc;
    SFTP_CALL(rc, libssh2_sftp_mkdir(sftp, tmp,
        LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP));
    /* Success or already exists */
    if (rc == 0) return 0;
    u64 err = sftp_io_last_status(sftp);
    return (err == LIBSSH2_FX_FILE_ALREADY_EXISTS) ? 0 : -1;
}

/* =========================================================================
 * Directory listing
 * ========================================================================= */

i32 sftp_list_dir(void *sftp_ptr, const char *path, SftpEntry *entries, i32 max_entries) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;

    LIBSSH2_SFTP_HANDLE *dir;
    SFTP_CALL_PTR(dir, libssh2_sftp_opendir(sftp, path));
    if (!dir) return -1;

    i32 count = 0;
    char buf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (count < max_entries) {
        /* readdir does NOT NUL-terminate; cap at sizeof(buf)-1 and terminate so
         * a max-length hostile filename can't leave buf unterminated. */
        int rc;
        SFTP_CALL(rc, libssh2_sftp_readdir(dir, buf, sizeof(buf) - 1, &attrs));
        if (rc <= 0) break;
        if ((usize)rc >= sizeof(buf)) rc = sizeof(buf) - 1;
        buf[rc] = '\0';

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

    int crc;
    SFTP_CALL(crc, libssh2_sftp_closedir(dir));
    (void)crc;
    return count;
}

/* =========================================================================
 * Basic file operations
 * ========================================================================= */

i32 sftp_download_at(void *sftp_ptr, const char *remote_path, int parent,
                     const char *name, bool exclusive) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;

    u64 total = 0, perms = 0;              /* both stay 0 when stat fails */
    bool perms_known = sftp_remote_stat_ex(sftp, remote_path, &total, &perms) &&
                       LIBSSH2_SFTP_S_ISREG(perms);

    LIBSSH2_SFTP_HANDLE *handle;
    SFTP_CALL_PTR(handle, libssh2_sftp_open(sftp, remote_path, LIBSSH2_FXF_READ, 0));
    if (!handle) return -1;

    /* The remote file's own bits (ssh/transfer_mode.h): a script stays one. */
    LocalDestinationWriter writer;
    if (!local_destination_begin_mode(&writer, parent, name, exclusive,
                                      (mode_t)transfer_create_mode(perms_known, (u32)perms, 0666))) {
        int crc;
        SFTP_CALL(crc, libssh2_sftp_close(handle));
        (void)crc;
        return -1;
    }

    u32 xfer_id = 0;
    transfer_state_begin(remote_path, total, false);

    char buf[SFTP_BUF_SIZE];
    i32 result = 0;
    u64 done = 0;
    for (;;) {
        ssize_t n;
        SFTP_CALL(n, libssh2_sftp_read(handle, buf, sizeof(buf)));
        if (n == 0) break;
        if (n < 0) { result = -1; break; }
        if (fwrite(buf, 1, (usize)n, writer.stream) != (usize)n) { result = -1; break; } /* disk full / write error */
        done += (u64)n;
        sftp_transfer_tick(xfer_id, done);
    }

    transfer_state_end();
    /* fclose flushes the tail of the stream, so a write error (a full disk
     * almost always fails at flush, not at fwrite) surfaces HERE. Ignoring it
     * reported a truncated download as a success. */
    {
        int crc;
        SFTP_CALL(crc, libssh2_sftp_close(handle));
        if (crc != 0) result = -1;
    }
    return local_destination_finish(&writer, result == 0) ? 0 : -1;
}

i32 sftp_download(void *sftp_ptr, const char *remote_path, const char *local_path) {
    char name[NAME_MAX + 1];
    int parent = local_destination_parent(local_path, name, sizeof name);
    if (parent < 0) return -1;
    i32 rc = sftp_download_at(sftp_ptr, remote_path, parent, name, false);
    close(parent);
    return rc;
}

/* Sequence counter for staged upload names — two concurrent uploads of the
 * same remote path (two sessions to the same host) must not share a temp. */
static _Atomic unsigned g_sftp_stage_seq = 0;

i32 sftp_upload(void *sftp_ptr, const char *local_path, const char *remote_path) {
    if (!sftp_ptr || !local_path || !remote_path) return -1;
    /* O_NONBLOCK: a FIFO at the path is refused instead of waiting for a
     * writer while the caller holds the session's I/O scope. */
    int fd = open(local_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    return sftp_upload_fd(sftp_ptr, fd, local_path, remote_path);
}

i32 sftp_upload_fd(void *sftp_ptr, int fd, const char *label, const char *remote_path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (fd < 0) return -1;
    if (!sftp || !remote_path) { close(fd); return -1; }

    FILE *f = fdopen(fd, "rb");
    if (!f) { close(fd); return -1; }

    struct stat local_st;
    bool local_known = fstat(fileno(f), &local_st) == 0;
    u64 total = local_known && local_st.st_size >= 0 ? (u64)local_st.st_size : 0;

    /* Replacing an existing remote file is staged through a sibling temp path.
     * Opening the target with FXF_TRUNC would destroy it the moment the
     * transfer starts, so a transfer that failed part-way (link drop, server
     * full, session closed) would leave the user with nothing. Stage first,
     * swap only after the bytes are there. */
    u64 raw_perms = 0;
    bool existed = sftp_remote_stat_ex(sftp, remote_path, NULL, &raw_perms);
    if (existed && LIBSSH2_SFTP_S_ISDIR(raw_perms)) {
        /* The target is a directory. Refuse up front instead of staging a
         * temp that could never be renamed over it. */
        fclose(f);
        return -1;
    }
    u64 old_perms = raw_perms & 07777;
    char staged[SFTP_MAX_PATH];
    const char *write_path = remote_path;
    bool staged_write = false;
    if (existed) {
        unsigned seq = atomic_fetch_add(&g_sftp_stage_seq, 1u);
        int n = snprintf(staged, sizeof staged, "%s.liu-part-%ld-%u",
                         remote_path, (long)getpid(), seq);
        if (n > 0 && (usize)n < sizeof staged) {
            write_path = staged;
            staged_write = true;
        }
        /* Temp sibling wouldn't fit in a path buffer: fall back to writing in
         * place. The failure-unlink below is then skipped too, so a failed
         * transfer leaves a truncated file rather than no file at all. */
    }

    LIBSSH2_SFTP_HANDLE *handle;
    /* A new file gets the local file's bits (ssh/transfer_mode.h); one that
     * replaces a remote file is given that file's mode after the swap. */
    SFTP_CALL_PTR(handle, libssh2_sftp_open(sftp, write_path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        (long)transfer_create_mode(local_known, (u32)local_st.st_mode,
                                   LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                                   LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH)));
    if (!handle) {
        fclose(f);
        return -1;
    }

    u32 xfer_id = 0;
    transfer_state_begin(label ? label : "", total, true);

    char buf[SFTP_BUF_SIZE];
    u64 done = 0;
    bool ok = true;
    for (;;) {
        usize nread = fread(buf, 1, sizeof(buf), f);
        bool read_error = ferror(f) != 0;
        ssize_t written = 0;
        while ((usize)written < nread) {
            ssize_t n;
            SFTP_CALL(n, libssh2_sftp_write(handle, buf + written, nread - (usize)written));
            if (n <= 0) {
                ok = false;
                break;
            }
            written += n;
            done += (u64)n;
            sftp_transfer_tick(xfer_id, done);
        }
        if (!ok) break;
        if (read_error) { ok = false; break; }
        if (nread < sizeof(buf)) break;
    }

    transfer_state_end();
    if (fclose(f) != 0) ok = false;
    {
        int crc;
        SFTP_CALL(crc, libssh2_sftp_close(handle));
        if (crc != 0) ok = false;
    }

    bool swap_failed = false;
    if (ok && staged_write) {
        /* Swap the staged file over the original. A plain rename is tried
         * first — servers honouring LIBSSH2_SFTP_RENAME_OVERWRITE do it
         * atomically — but OpenSSH's SFTP v3 refuses to replace an existing
         * name, so fall back to unlink + rename. */
        int rrc;
        SFTP_CALL(rrc, libssh2_sftp_rename(sftp, staged, remote_path));
        if (rrc != 0) {
            int urc;
            SFTP_CALL(urc, libssh2_sftp_unlink(sftp, remote_path));
            (void)urc;
            SFTP_CALL(rrc, libssh2_sftp_rename(sftp, staged, remote_path));
            if (rrc != 0) {
                /* Both attempts failed, and the second one may already have
                 * removed the original: the fully transferred payload only
                 * exists under the staged name now, so it must survive the
                 * cleanup below. Leaving a stray temp beats deleting the only
                 * copy of what the user asked to send. */
                ok = false;
                swap_failed = true;
            }
        }
        if (ok && old_perms) {
            /* Carry over the replaced file's mode. A freshly created handle
             * is 0644, which would silently strip the exec bit every time a
             * script is re-uploaded over itself. */
            LIBSSH2_SFTP_ATTRIBUTES set;
            memset(&set, 0, sizeof set);
            set.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
            set.permissions = old_perms;
            int src_;
            SFTP_CALL(src_, libssh2_sftp_setstat(sftp, remote_path, &set));
            (void)src_;
        }
    }
    /* Clean up only what this call created: the staged temp, or a brand-new
     * target. Never the pre-existing file being replaced, and never a staged
     * file that is now the only copy of the payload. */
    if (!ok && !swap_failed && (staged_write || !existed)) {
        int urc;
        SFTP_CALL(urc, libssh2_sftp_unlink(sftp, write_path));
        (void)urc;
    }
    return ok ? 0 : -1;
}

i32 sftp_mkdir(void *sftp_ptr, const char *path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;
    int rc;
    SFTP_CALL(rc, libssh2_sftp_mkdir(sftp, path,
        LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP));
    return rc;
}

i32 sftp_remove(void *sftp_ptr, const char *path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;
    int rc;
    SFTP_CALL(rc, libssh2_sftp_unlink(sftp, path));
    return rc;
}

i32 sftp_rename(void *sftp_ptr, const char *old_path, const char *new_path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;
    int rc;
    SFTP_CALL(rc, libssh2_sftp_rename(sftp, old_path, new_path));
    return rc;
}

/* =========================================================================
 * Transfer progress (global state for UI)
 * ========================================================================= */

/* Every reader describes the FOCUSED transfer — the most recently started live
 * one — so with several running the panel gives a coherent description of one
 * of them instead of fields sampled from different streams. Aggregate figures
 * come from sftp_transfer_count / sftp_transfer_totals. */

bool sftp_transfer_active(void) {
    pthread_mutex_lock(&g_sftp_transfer_lock);
    bool v = g_sftp_transfer.active_count > 0;
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    return v;
}

const char *sftp_transfer_filename(void) {
    static _Thread_local char snapshot[256];
    pthread_mutex_lock(&g_sftp_transfer_lock);
    const SftpTransferSlot *t = sftp_focus_locked();
    if (t) memcpy(snapshot, t->filename, sizeof snapshot);
    else   snapshot[0] = '\0';
    snapshot[sizeof snapshot - 1u] = '\0';
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    return snapshot;
}

u64 sftp_transfer_bytes_done(void) {
    pthread_mutex_lock(&g_sftp_transfer_lock);
    const SftpTransferSlot *t = sftp_focus_locked();
    u64 v = t ? t->bytes_done : 0;
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    return v;
}

u64 sftp_transfer_bytes_total(void) {
    pthread_mutex_lock(&g_sftp_transfer_lock);
    const SftpTransferSlot *t = sftp_focus_locked();
    u64 v = t ? t->bytes_total : 0;
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    return v;
}

bool sftp_transfer_is_upload(void) {
    pthread_mutex_lock(&g_sftp_transfer_lock);
    const SftpTransferSlot *t = sftp_focus_locked();
    bool v = t ? t->is_upload : false;
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    return v;
}

f64 sftp_transfer_speed_bps(void) {
    pthread_mutex_lock(&g_sftp_transfer_lock);
    const SftpTransferSlot *t = sftp_focus_locked();
    f64 v = t ? t->speed_bps : 0.0;
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    return v;
}

f64 sftp_transfer_elapsed_sec(void) {
    pthread_mutex_lock(&g_sftp_transfer_lock);
    const SftpTransferSlot *t = sftp_focus_locked();
    f64 start = t ? t->start_time : 0.0;
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    return start > 0.0 ? platform_time_sec() - start : 0.0;
}

f64 sftp_transfer_eta_sec(void) {
    pthread_mutex_lock(&g_sftp_transfer_lock);
    const SftpTransferSlot *t = sftp_focus_locked();
    u64 total = t ? t->bytes_total : 0;
    u64 done  = t ? t->bytes_done  : 0;
    f64 speed = t ? t->speed_bps   : 0.0;
    pthread_mutex_unlock(&g_sftp_transfer_lock);
    if (total == 0 || speed <= 1.0) return -1;
    if (done >= total) return 0;
    return (f64)(total - done) / speed;
}

/* =========================================================================
 * Resume (partial file transfer)
 * ========================================================================= */

i64 sftp_download_resume(void *sftp_ptr, const char *remote_path, const char *local_path,
                         SFTPProgressCallback progress, void *userdata) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;

    /* Stat (not "size or 0") so an existing empty remote file is a real,
     * transferable file instead of looking like a missing one. */
    u64 remote_size = 0;
    if (!sftp_remote_stat_ex(sftp, remote_path, &remote_size, NULL)) return -1;

    char name[NAME_MAX + 1];
    int parent = local_destination_parent(local_path, name, sizeof name);
    if (parent < 0) return -1;
    struct stat st;
    int fd = local_destination_resume(parent, name, &st);
    close(parent);
    if (fd < 0) return -1;
    u64 local_size = (u64)st.st_size;
    u64 offset = 0;

    if (remote_size == 0 || local_size == 0) {
        if (ftruncate(fd, 0) != 0) { close(fd); return -1; }
    } else if (local_size < remote_size) {
        offset = local_size;
    } else {
        close(fd);
        if (progress) {
            const char *fname = strrchr(remote_path, '/');
            progress(fname ? fname + 1 : remote_path, (i64)remote_size, (i64)remote_size, userdata);
        }
        return 0;
    }
    if (remote_size == 0) {
        bool ok = close(fd) == 0;
        if (ok && progress) {
            const char *fname = strrchr(remote_path, '/');
            progress(fname ? fname + 1 : remote_path, 0, 0, userdata);
        }
        return ok ? 0 : -1;
    }

    /* Every network call below goes through SFTP_CALL like the plain
     * transfers: on a non-blocking session an EAGAIN releases the session's io
     * lock and waits on the socket for the direction libssh2 asked for. A raw
     * libssh2 call would read EAGAIN as failure, and a sleep-and-retry that
     * kept the lock would spin with the shell's echo waiting behind it. */
    LIBSSH2_SFTP_HANDLE *handle;
    SFTP_CALL_PTR(handle, libssh2_sftp_open(sftp, remote_path, LIBSSH2_FXF_READ, 0));
    if (!handle) { close(fd); return -1; }

    /* Seek to resume position */
    if (offset > 0) {
        libssh2_sftp_seek64(handle, offset);
    }

    /* Open local file */
    FILE *f = lseek(fd, (off_t)offset, SEEK_SET) >= 0 ? fdopen(fd, "wb") : NULL;
    if (!f) {
        close(fd);
        int crc;
        SFTP_CALL(crc, libssh2_sftp_close(handle));
        (void)crc;
        return -1;
    }

    u32 xfer_id = 0;
    transfer_state_begin(remote_path, remote_size, false);
    sftp_transfer_tick(xfer_id, offset);

    const char *fname = strrchr(remote_path, '/');
    const char *display_name = fname ? fname + 1 : remote_path;

    char buf[SFTP_BUF_SIZE];
    i64 bytes_transferred = 0;

    bool ok = true;
    for (;;) {
        ssize_t n;
        SFTP_CALL(n, libssh2_sftp_read(handle, buf, sizeof(buf)));
        if (n == 0) break;  /* EOF */
        if (n < 0) { ok = false; break; }

        usize written = fwrite(buf, 1, (usize)n, f);
        if (written != (usize)n) { ok = false; break; }

        bytes_transferred += (i64)n;
        sftp_transfer_tick(xfer_id, offset + (u64)bytes_transferred);

        if (progress) {
            progress(display_name, (i64)(offset + (u64)bytes_transferred),
                     (i64)remote_size, userdata);
        }
    }

    transfer_state_end();
    /* Same flush-at-close trap as sftp_download: an unchecked fclose reports
     * a short file as a completed resume. The partial file stays on disk on
     * purpose here — that is what the next resume picks up from. */
    if (fclose(f) != 0) ok = false;
    {
        int crc;
        SFTP_CALL(crc, libssh2_sftp_close(handle));
        if (crc != 0) ok = false;
    }
    return ok ? bytes_transferred : -1;
}

i64 sftp_upload_resume(void *sftp_ptr, const char *local_path, const char *remote_path,
                       SFTPProgressCallback progress, void *userdata) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp) return -1;

    /* Get local file size */
    FILE *f = fopen(local_path, "rb");
    if (!f) return -1;

    struct stat local_st;
    if (fstat(fileno(f), &local_st) != 0 || local_st.st_size < 0) {
        fclose(f);
        return -1;
    }
    u64 local_size = (u64)local_st.st_size;

    /* Check remote file size for resume. Stat, not "size or 0": an existing
     * empty remote file must be distinguishable from a missing one. */
    u64 remote_size = 0;
    bool remote_exists = sftp_remote_stat_ex(sftp, remote_path, &remote_size, NULL);
    u64 offset = 0;
    u32 open_flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT;

    if (remote_exists && remote_size == local_size) {
        /* Same length — treat as already transferred (this API resumes by
         * size; it has no content check). An empty local file lands here too
         * once the remote empty file exists, so 0-byte uploads terminate. */
        fclose(f);
        if (progress) {
            const char *fname = strrchr(local_path, '/');
            progress(fname ? fname + 1 : local_path, (i64)local_size, (i64)local_size, userdata);
        }
        return 0;
    } else if (remote_exists && remote_size < local_size && remote_size > 0) {
        /* Resume: append from the remote file's current size (don't
         * truncate). */
        offset = remote_size;
    } else {
        /* No remote file, or a remote file LONGER than the local one — that
         * is not a partial upload of this file (a stale or entirely different
         * file shares the name), and skipping it as "already complete" meant
         * the upload silently never happened. Start over from byte 0. */
        open_flags |= LIBSSH2_FXF_TRUNC;
    }

    /* Open remote file (SFTP_CALL: see sftp_download_resume). */
    LIBSSH2_SFTP_HANDLE *handle;
    SFTP_CALL_PTR(handle, libssh2_sftp_open(sftp, remote_path, open_flags,
        (long)transfer_create_mode(true, (u32)local_st.st_mode, 0)));   /* ssh/transfer_mode.h */
    if (!handle) {
        fclose(f);
        return -1;
    }

    /* Seek both local and remote to resume position */
    if (offset > 0) {
        if (offset > (u64)LLONG_MAX ||
            fseeko(f, (off_t)offset, SEEK_SET) != 0) {
            fclose(f);
            int crc;
            SFTP_CALL(crc, libssh2_sftp_close(handle));
            (void)crc;
            return -1;
        }
        clearerr(f);
        libssh2_sftp_seek64(handle, offset);
    }

    u32 xfer_id = 0;
    transfer_state_begin(local_path, local_size, true);
    sftp_transfer_tick(xfer_id, offset);

    const char *fname = strrchr(local_path, '/');
    const char *display_name = fname ? fname + 1 : local_path;

    char buf[SFTP_BUF_SIZE];
    i64 bytes_transferred = 0;
    bool ok = true;
    for (;;) {
        usize nread = fread(buf, 1, sizeof(buf), f);
        bool read_error = ferror(f) != 0;
        ssize_t total_written = 0;
        while ((usize)total_written < nread) {
            ssize_t n;
            SFTP_CALL(n, libssh2_sftp_write(handle, buf + total_written,
                                            nread - (usize)total_written));
            if (n <= 0) {
                ok = false;
                break;
            }
            total_written += n;
            bytes_transferred += (i64)n;
            sftp_transfer_tick(xfer_id, offset + (u64)bytes_transferred);

            if (progress) {
                progress(display_name, (i64)(offset + (u64)bytes_transferred),
                         (i64)local_size, userdata);
            }
        }
        if (!ok) break;
        if (read_error) { ok = false; break; }
        if (nread < sizeof(buf)) break;
    }

    transfer_state_end();
    if (fclose(f) != 0) ok = false;
    {
        int crc;
        SFTP_CALL(crc, libssh2_sftp_close(handle));
        if (crc != 0) ok = false;
    }
    return ok ? bytes_transferred : -1;
}

/* =========================================================================
 * Sync helpers (internal)
 * ========================================================================= */

/* Recursive SFTP directory listing for sync.
 * Calls callback for each file found (not directories).
 * rel_path is the path relative to the sync root. */
typedef void (*SyncFileVisitor)(const char *rel_path, u64 size, u64 mtime,
                                bool is_dir, void *ctx);

/* Returns true only when EVERY directory it entered was read to the end and nothing
 * was skipped for want of room: a failed opendir (permission denied, a vanished
 * directory), a readdir error, a name or path too long to carry, and a tree deeper
 * than SFTP_MAX_SYNC_DEPTH all return false. Names refused on purpose (an unsafe
 * path component, a dotfile) do not count — they are absent by policy on both walks.
 * A sync that deletes "extra" files from this listing must not run on an incomplete
 * one: whatever the walk could not see would read as extra. */
static bool sftp_walk_remote(LIBSSH2_SFTP *sftp, const char *base_path,
                             const char *rel_prefix, SyncFileVisitor visitor, void *ctx,
                             int depth) {
    bool complete = true;
    if (depth > SFTP_MAX_SYNC_DEPTH) return false;
    /* SFTP_CALL everywhere below: on a non-blocking session a raw readdir's
     * EAGAIN read as "end of directory" and the sync silently skipped the
     * rest of the tree. */
    LIBSSH2_SFTP_HANDLE *dir;
    SFTP_CALL_PTR(dir, libssh2_sftp_opendir(sftp, base_path));
    if (!dir) return false;

    char buf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (1) {
        int rc;
        SFTP_CALL(rc, libssh2_sftp_readdir(dir, buf, sizeof(buf) - 1, &attrs));
        if (rc < 0) { complete = false; break; }     /* an error is not the end of the directory */
        if (rc == 0) break;
        if ((usize)rc >= sizeof(buf)) { complete = false; continue; }
        buf[rc] = '\0';

        /* Reject anything that is not a single safe path component (".", "..",
         * or a name containing a path separator) — these would let a hostile
         * server escape the local sync root via path traversal. */
        if (!sftp_name_is_safe_component(buf))
            continue;

        /* Skip dotfiles/hidden files */
        if (is_dotfile(buf))
            continue;

        char rel_path[SFTP_MAX_PATH];
        int rel_n = rel_prefix[0] != '\0'
                  ? snprintf(rel_path, sizeof(rel_path), "%s/%s",
                             rel_prefix, buf)
                  : snprintf(rel_path, sizeof(rel_path), "%s", buf);
        if (rel_n < 0 || (usize)rel_n >= sizeof(rel_path)) { complete = false; continue; }

        bool dir_entry = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        visitor(rel_path, attrs.filesize, attrs.mtime, dir_entry, ctx);

        if (dir_entry) {
            char full_path[SFTP_MAX_PATH];
            int full_n = snprintf(full_path, sizeof(full_path), "%s/%s",
                                  base_path, buf);
            if (full_n < 0 || (usize)full_n >= sizeof(full_path)) { complete = false; continue; }
            if (!sftp_walk_remote(sftp, full_path, rel_path, visitor, ctx, depth + 1))
                complete = false;
        }
    }

    { int crc; SFTP_CALL(crc, libssh2_sftp_closedir(dir)); (void)crc; }
    return complete;
}

/* Same contract as sftp_walk_remote: true only for a complete listing. Symlinks are
 * skipped by policy and do not make it incomplete; an unreadable directory, a failed
 * lstat and a path too long to carry do. */
static bool local_walk(const char *base_path, const char *rel_prefix,
                       SyncFileVisitor visitor, void *ctx, int depth) {
    bool complete = true;
    if (depth > SFTP_MAX_SYNC_DEPTH) return false;
    DIR *d = opendir(base_path);
    if (!d) return false;

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
        int full_n = snprintf(full_path, sizeof(full_path), "%s/%s",
                              base_path, de->d_name);
        if (full_n < 0 || (usize)full_n >= sizeof(full_path)) { complete = false; continue; }

        char rel_path[SFTP_MAX_PATH];
        if (rel_prefix[0] != '\0')
            {
                int rel_n = snprintf(rel_path, sizeof(rel_path), "%s/%s",
                                     rel_prefix, de->d_name);
                if (rel_n < 0 || (usize)rel_n >= sizeof(rel_path)) { complete = false; continue; }
            }
        else
            {
                int rel_n = snprintf(rel_path, sizeof(rel_path), "%s",
                                     de->d_name);
                if (rel_n < 0 || (usize)rel_n >= sizeof(rel_path)) { complete = false; continue; }
            }

        /* lstat (not stat) so the symlink itself is seen, not its target.
         * Following symlinks here is dangerous: a symlink inside the sync
         * root could point outside of it, and on the upload side those
         * files would leak to the remote; on the download side delete_extra
         * could walk through a symlink and remove unrelated local files. */
        struct stat st;
        if (lstat(full_path, &st) != 0) { complete = false; continue; }

        /* Skip symlinks entirely. Only regular files and directories that
         * genuinely live inside the sync root are synced. */
        if (S_ISLNK(st.st_mode)) continue;

        bool is_dir_entry = S_ISDIR(st.st_mode);
        visitor(rel_path, (u64)st.st_size, (u64)st.st_mtime, is_dir_entry, ctx);

        if (is_dir_entry) {
            if (!local_walk(full_path, rel_path, visitor, ctx, depth + 1)) complete = false;
        }
    }

    closedir(d);
    return complete;
}

/* File entry for sync comparison. `path` is heap-duplicated (not a 4 KB inline
 * SFTP_MAX_PATH buffer): real relative paths are tens of bytes, and an inline
 * buffer would make a 10k-file tree ~41 MB of mostly-empty path storage. All
 * readers treat it as a plain C string, so only push/free below own the
 * allocation. */
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
    if (!list || list->count < 0 || list->capacity < 0 ||
        list->count == INT32_MAX) {
        if (list) list->oom = true;
        return;
    }
    if (list->count >= list->capacity) {
        if (list->capacity > INT32_MAX / 2) { list->oom = true; return; }
        i32 new_cap = list->capacity == 0 ? 64 : list->capacity * 2;
        usize bytes;
        if (!checked_mul_usize((usize)new_cap, sizeof(SyncEntry), &bytes)) {
            list->oom = true;
            return;
        }
        SyncEntry *new_items = realloc(list->items, bytes);
        if (!new_items) {
            /* Record the truncation so the caller can refuse delete_extra — a
             * dropped entry on the comparison side would otherwise delete a
             * file whose counterpart was simply not listed. */
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
 * Linear find/contains helpers would make a full sync O(n^2) in entry
 * count. This hash index gives O(1) average lookup, so sync scales linearly
 * with directory size. The index is rebuilt per sync call (it's cheap) and
 * the path strings keep living in SyncEntryList.
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
    if (!ix) return;
    memset(ix, 0, sizeof *ix);
    if (!list || list->count < 0 || list->count > INT32_MAX / 2) return;
    ix->list = list;
    i32 cap = 16;
    i32 wanted = list->count * 2;
    while (cap < wanted) {
        if (cap > INT32_MAX / 2) { ix->list = NULL; return; }
        cap <<= 1;
    }
    ix->cap = cap;
    usize bytes;
    if (!checked_mul_usize((usize)cap, sizeof(i32), &bytes)) {
        ix->cap = 0;
        ix->list = NULL;
        return;
    }
    ix->slots = malloc(bytes);
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
static void sftp_remove_recursive(LIBSSH2_SFTP *sftp, const char *path, bool is_dir_entry,
                                  int depth) {
    if (!is_dir_entry) {
        int urc;
        SFTP_CALL(urc, libssh2_sftp_unlink(sftp, path));
        (void)urc;
        return;
    }
    if (depth > SFTP_MAX_SYNC_DEPTH) return;

    /* Recursively remove directory contents first */
    LIBSSH2_SFTP_HANDLE *dir;
    SFTP_CALL_PTR(dir, libssh2_sftp_opendir(sftp, path));
    if (!dir) return;

    char buf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (1) {
        /* readdir does NOT NUL-terminate; cap the read and terminate before
         * using buf as a C string (max-length filename would otherwise leave
         * it unterminated). */
        int rc;
        SFTP_CALL(rc, libssh2_sftp_readdir(dir, buf, sizeof(buf) - 1, &attrs));
        if (rc <= 0) break;
        if ((usize)rc >= sizeof(buf)) rc = sizeof(buf) - 1;
        buf[rc] = '\0';

        if (buf[0] == '.' && (buf[1] == '\0' || (buf[1] == '.' && buf[2] == '\0')))
            continue;

        char child[SFTP_MAX_PATH];
        snprintf(child, sizeof(child), "%s/%s", path, buf);
        sftp_remove_recursive(sftp, child, LIBSSH2_SFTP_S_ISDIR(attrs.permissions), depth + 1);
    }

    { int crc; SFTP_CALL(crc, libssh2_sftp_closedir(dir)); (void)crc; }
    { int rrc; SFTP_CALL(rrc, libssh2_sftp_rmdir(sftp, path)); (void)rrc; }
}

/* Remove local file or directory recursively.
 *
 * Symlink safety: every entry is lstat()ed and a symlink's target is never
 * traversed. A symlink itself is removed with unlink() (which operates on
 * the link, not the target). This matches the symlink-skip policy in
 * local_walk — the walker never records symlinks as entries to reconcile,
 * so the only way one gets here is inside a directory being removed that
 * happens to contain an unrecorded link, and it is still not descended
 * through. */
static void local_remove_recursive(const char *path, bool is_dir_entry, int depth) {
    if (!is_dir_entry) {
        unlink(path);
        return;
    }
    if (depth > SFTP_MAX_SYNC_DEPTH) return;

    /* Re-check the path on disk — if it turned into a symlink since it
     * was listed, refuse to recurse. */
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
        local_remove_recursive(child, S_ISDIR(st.st_mode), depth + 1);
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

    /* Create the local destination root if needed and hold it open for the
     * sync. Create subdirectories relative to this descriptor without following
     * symlinks, so downloads remain inside the selected root. */
    int local_root = local_destination_dir(local_dir, true);
    if (local_root < 0) { result.files_failed = -1; return result; }

    /* Collect remote file listing */
    SyncEntryList remote_list;
    sync_entry_list_init(&remote_list);
    bool remote_complete = sftp_walk_remote(sftp, remote_dir, "", sync_visitor, &remote_list, 0);

    /* Collect local file listing */
    SyncEntryList local_list;
    sync_entry_list_init(&local_list);
    bool local_complete = local_walk(local_dir, "", sync_visitor, &local_list, 0);

    /* A truncated listing (allocation failure) must never drive deletions:
     * proceeding with delete_extra could remove a local file whose remote
     * counterpart was simply not recorded. Disable deletion and flag the
     * sync as incomplete; the already-collected transfers are still safe. */
    if (remote_list.oom || local_list.oom) {
        delete_extra = false;
        result.files_failed = -1;
    }
    /* Nor may an INCOMPLETE walk (an unreadable subtree, a readdir error, a name too long
     * to carry): everything it could not see would read as "extra" and be deleted — a
     * hostile or merely flaky server could empty the local mirror by failing a listing. */
    if (!remote_complete || !local_complete) {
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

        if (re->is_dir) {
            /* Create local directory if needed */
            int made = local_destination_subdir(local_root, re->path, true);
            if (made < 0) result.files_failed++;
            else close(made);
            continue;
        }

        /* Check if local file needs updating */
        const SyncEntry *le = sync_index_find(&local_ix, re->path);
        if (le && le->size == re->size && le->mtime >= re->mtime) {
            result.files_skipped++;
            continue;
        }

        /* Its directory, beneath the root (made if needed), and its name. */
        char rel_dir[SFTP_MAX_PATH];
        snprintf(rel_dir, sizeof(rel_dir), "%s", re->path);
        char *slash = strrchr(rel_dir, '/');
        const char *leaf = slash ? slash + 1 : re->path;
        if (slash) *slash = '\0'; else rel_dir[0] = '\0';
        int parent_fd = local_destination_subdir(local_root, rel_dir, true);
        if (parent_fd < 0) { result.files_failed++; continue; }

        /* Download the file */
        i32 rc = sftp_download_at(sftp_ptr, remote_full, parent_fd, leaf, false);
        close(parent_fd);
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
                local_remove_recursive(local_full, le->is_dir, 0);
            }
        }
    }

    sync_index_free(&remote_ix);
    sync_index_free(&local_ix);
    sync_entry_list_free(&remote_list);
    sync_entry_list_free(&local_list);
    close(local_root);
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
    bool local_complete = local_walk(local_dir, "", sync_visitor, &local_list, 0);

    /* Collect remote file listing */
    SyncEntryList remote_list;
    sync_entry_list_init(&remote_list);
    bool remote_complete = sftp_walk_remote(sftp, remote_dir, "", sync_visitor, &remote_list, 0);

    /* A truncated listing (allocation failure) must never drive deletions —
     * see sftp_sync_download. Disable delete_extra and flag incompleteness. */
    if (remote_list.oom || local_list.oom) {
        delete_extra = false;
        result.files_failed = -1;
    }
    /* Nor may an INCOMPLETE walk (an unreadable subtree, a readdir error, a name too long
     * to carry): everything it could not see would read as "extra" and be deleted — a
     * hostile or merely flaky server could empty the local mirror by failing a listing. */
    if (!remote_complete || !local_complete) {
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
                sftp_remove_recursive(sftp, remote_full, re->is_dir, 0);
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
    int rc;
    SFTP_CALL(rc, libssh2_sftp_symlink_ex(sftp,
        target, (u32)strlen(target),
        (char *)link_path, (u32)strlen(link_path),
        LIBSSH2_SFTP_SYMLINK));
    return rc == 0;
}

bool sftp_read_symlink(void *sftp_ptr, const char *link_path, char *target, i32 target_size) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_ptr;
    if (!sftp || !target || target_size <= 0) return false;

    int rc;
    SFTP_CALL(rc, libssh2_sftp_symlink_ex(sftp,
        link_path, (u32)strlen(link_path),
        target, (u32)target_size,
        LIBSSH2_SFTP_READLINK));

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
    int rc;
    SFTP_CALL(rc, libssh2_sftp_unlink(sftp, link_path));
    return rc == 0;
}
