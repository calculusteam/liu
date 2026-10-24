/*
 * Liu - SFTP operations via libssh2
 * File transfer, directory sync, symlink management.
 */
#ifndef SSH_SFTP_H
#define SSH_SFTP_H

#include "core/types.h"

/* =========================================================================
 * Types
 * ========================================================================= */

/* SFTP directory entry */
typedef struct {
    char  name[256];
    u64   size;
    u32   permissions;
    u64   mtime;
    bool  is_dir;
} SftpEntry;

/* Transfer progress callback */
typedef void (*SftpProgressFn)(u64 bytes_transferred, u64 total_bytes, void *userdata);

/* Progress callback with filename (for sync and resume operations) */
typedef void (*SFTPProgressCallback)(const char *filename, i64 bytes_done, i64 bytes_total, void *userdata);

/* Sync result statistics */
typedef struct {
    i32 files_transferred;
    i32 files_skipped;
    i32 files_failed;
    i64 bytes_transferred;
} SFTPSyncResult;

/* =========================================================================
 * Fair scheduling of SFTP calls on a shared SSH session
 * =========================================================================
 *
 * Every SFTP call runs on the same LIBSSH2_SESSION as the shell, and libssh2
 * allows one thread in at a time, so a transfer holding the session's io
 * lock in BLOCKING mode for the whole file would make every network wait of
 * the transfer a wait the shell's echo sits behind. The session is kept
 * NON-blocking instead and each call goes through these macros: an EAGAIN
 * releases the lock, waits on the socket for the direction libssh2 asked
 * for, re-takes the lock and repeats the call with the same arguments (the
 * contract every libssh2 SFTP call has). After any call, sftp_io_breathe()
 * lets a shell that tried the lock meanwhile in. Both are no-ops when no
 * session scope is installed on this thread, so a caller that holds a
 * blocking session makes plain blocking calls.
 *
 * Installed per thread by session_sftp_scope_begin(); ssh_session.c owns them. */
bool sftp_io_yield(void);                /* EAGAIN: wait without the lock; false = session gone */
void sftp_io_abort_all(void);            /* quit: every later sftp_io_yield answers false. Not undone. */
void sftp_io_breathe(void);              /* after a call: give a waiting shell the lock */
int  sftp_io_last_errno(void);           /* libssh2_session_last_errno of the scope's session, else 0 */
u64  sftp_io_last_status(void *sftp);     /* this thread's completed operation status */

/* `var = call`, retried across EAGAIN with the same arguments. For calls that
 * return a pointer and report EAGAIN through the session errno, the _PTR form. */
#define SFTP_CALL(var, call)                                                   \
    do {                                                                       \
        (var) = (call);                                                        \
        if ((var) == LIBSSH2_ERROR_EAGAIN) { if (!sftp_io_yield()) break; continue; } \
        sftp_io_breathe();                                                     \
        break;                                                                 \
    } while (1)
#define SFTP_CALL_PTR(var, call)                                               \
    do {                                                                       \
        (var) = (call);                                                        \
        if (!(var) && sftp_io_last_errno() == LIBSSH2_ERROR_EAGAIN) {          \
            if (!sftp_io_yield()) break;                                       \
            continue;                                                          \
        }                                                                      \
        sftp_io_breathe();                                                     \
        break;                                                                 \
    } while (1)

/* =========================================================================
 * Directory operations
 * ========================================================================= */

/* List directory contents. Returns number of entries, -1 on error. */
i32 sftp_list_dir(void *sftp, const char *path, SftpEntry *entries, i32 max_entries);

/* Create directory. Returns 0 on success. */
i32 sftp_mkdir(void *sftp, const char *path);

/* =========================================================================
 * Basic file operations
 * ========================================================================= */

/* Download file. Returns 0 on success, -1 on error.
 * Stages bytes before replacing an existing regular file. No destination
 * path component may be a symlink; failed transfers preserve the old file. */
i32 sftp_download(void *sftp, const char *remote_path, const char *local_path);
/* Destination relative to a held directory. exclusive=true is paste's atomic
 * no-replace contract. The caller retains ownership of the directory fd. */
i32 sftp_download_at(void *sftp, const char *remote_path, int parent,
                     const char *name, bool exclusive);

/* Upload file. Returns 0 on success, -1 on error.
 * Overwrites the remote path. When it already exists the bytes go to a
 * sibling temp file that is swapped in only after the transfer completed, so
 * a failure part-way through leaves the previous remote file intact; the
 * replaced file's permission bits are carried over to the new one. */
i32 sftp_upload(void *sftp, const char *local_path, const char *remote_path);

/* Human-readable reason for the last failed SFTP operation on this handle
 * ("Permission denied", "No space left on the server", …). Never NULL.
 * Valid only inside the caller's SFTP scope, right after the failure. */
const char *sftp_last_error_str(void *sftp);

/* True when `name` is a single, safe path component — non-empty, not "." or
 * "..", and free of path separators. A hostile or compromised SFTP server can
 * return anything in a directory listing; joined onto a local path, "../.."
 * escapes whatever directory the user is looking at. Every consumer of a
 * remote directory entry must gate on this before building a path from it. */
bool sftp_name_is_safe_component(const char *name);

/* Remove file or symlink. */
i32 sftp_remove(void *sftp, const char *path);

/* Rename file. */
i32 sftp_rename(void *sftp, const char *old_path, const char *new_path);

/* =========================================================================
 * Resume (partial file transfer)
 * ========================================================================= */

/* Resume an interrupted download. Returns bytes transferred or -1 on error.
 * If local file exists and is smaller than remote, seeks to local size and appends. */
i64 sftp_download_resume(void *sftp, const char *remote_path, const char *local_path,
                         SFTPProgressCallback progress, void *userdata);

/* Resume an interrupted upload. Returns bytes transferred or -1 on error.
 * If remote file exists and is smaller than local, seeks to remote size and appends. */
i64 sftp_upload_resume(void *sftp, const char *local_path, const char *remote_path,
                       SFTPProgressCallback progress, void *userdata);

/* =========================================================================
 * Sync (rsync-like directory synchronization)
 * ========================================================================= */

/* Synchronize remote directory to local directory.
 * Recursively downloads new/changed files.
 * If delete_extra is true, removes local files not present on remote.
 * Skips dotfiles/hidden files by default. */
SFTPSyncResult sftp_sync_download(void *sftp, const char *remote_dir, const char *local_dir,
                                  bool delete_extra, SFTPProgressCallback progress, void *userdata);

/* Synchronize local directory to remote directory.
 * Recursively uploads new/changed files.
 * If delete_extra is true, removes remote files not present locally.
 * Skips dotfiles/hidden files by default. */
SFTPSyncResult sftp_sync_upload(void *sftp, const char *local_dir, const char *remote_dir,
                                bool delete_extra, SFTPProgressCallback progress, void *userdata);

/* =========================================================================
 * Symlink operations
 * ========================================================================= */

/* Create a symbolic link. Returns true on success. */
bool sftp_create_symlink(void *sftp, const char *target, const char *link_path);

/* Read the target of a symbolic link. Returns true on success.
 * target buffer receives the null-terminated path. */
bool sftp_read_symlink(void *sftp, const char *link_path, char *target, i32 target_size);

/* Remove a symbolic link. Returns true on success.
 * (Uses sftp_remove/unlink internally, which works on symlinks.) */
bool sftp_remove_symlink(void *sftp, const char *link_path);

/* =========================================================================
 * Transfer progress (global state for UI)
 * ========================================================================= */

bool        sftp_transfer_active(void);
const char *sftp_transfer_filename(void);
u64         sftp_transfer_bytes_done(void);
u64         sftp_transfer_bytes_total(void);
bool        sftp_transfer_is_upload(void);

/* Speed / timing — smoothed over a rolling window by sftp_transfer_tick.
 * Returns 0 when no progress has been recorded yet (avoid division by 0
 * in the UI layer; treat as "calculating…"). */
f64         sftp_transfer_speed_bps(void);
f64         sftp_transfer_elapsed_sec(void);
f64         sftp_transfer_eta_sec(void);     /* -1 when total is unknown */

/* Low-level helpers used by transfer implementations to announce progress.
 * Call order: begin() once → tick(done) many → end() once. `tick` is
 * cheap; invoke it after every chunk, the helper throttles internally. */
/* Start reporting a transfer. Returns a handle to pass to tick/end, or 0 when
 * the (bounded) table is full — the transfer still runs, it just goes
 * unreported, because progress display is never a precondition for moving
 * bytes. Each transfer owns its own record, so concurrent transfers do not
 * overwrite one another's byte counts, speed and ETA. */
u32         sftp_transfer_begin(const char *path, u64 total, bool upload);
void        sftp_transfer_tick(u32 id, u64 new_bytes_done);
void        sftp_transfer_end(u32 id);

/* Aggregates across every live transfer. The accessors above describe one
 * transfer (the most recently started); these describe the set. */
u32         sftp_transfer_count(void);
void        sftp_transfer_totals(u64 *out_done, u64 *out_total);

#endif /* SSH_SFTP_H */
