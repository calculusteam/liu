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
 * Directory operations
 * ========================================================================= */

/* List directory contents. Returns number of entries, -1 on error. */
i32 sftp_list_dir(void *sftp, const char *path, SftpEntry *entries, i32 max_entries);

/* Create directory. Returns 0 on success. */
i32 sftp_mkdir(void *sftp, const char *path);

/* =========================================================================
 * Basic file operations
 * ========================================================================= */

/* Download file. Returns 0 on success, -1 on error. */
i32 sftp_download(void *sftp, const char *remote_path, const char *local_path);

/* Upload file. Returns 0 on success, -1 on error. */
i32 sftp_upload(void *sftp, const char *local_path, const char *remote_path);

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
void        sftp_transfer_begin(const char *path, u64 total, bool upload);
void        sftp_transfer_tick(u64 new_bytes_done);
void        sftp_transfer_end(void);

#endif /* SSH_SFTP_H */
