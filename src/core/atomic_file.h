/*
 * Liu - crash-safe writes for small persistent state files.
 */
#ifndef LIU_CORE_ATOMIC_FILE_H
#define LIU_CORE_ATOMIC_FILE_H

#include "core/types.h"

#include <stdio.h>
#include <sys/stat.h>

#define ATOMIC_FILE_PATH_CAP 4096

/* Controls which properties of the replaced file a commit preserves; request
 * them with atomic_file_keep().
 *
 * A commit renames a new inode over the destination. For Liu-owned state
 * files, replacing the inode is sufficient. For editor documents, callers may
 * preserve owner and group, extended attributes, ACLs, hard links, and
 * read-only behavior.
 *
 * ATOMIC_KEEP_METADATA copies owner and group when permitted, plus extended
 * attributes and the ACL. Metadata copying is best effort.
 *
 * ATOMIC_KEEP_LINKS avoids replacing a destination with multiple hard links,
 * or a destination whose owner cannot be preserved when metadata is
 * requested. It writes the completed temporary file into the existing inode
 * instead; this is not atomic, but keeps other hard-link names attached to
 * the same file.
 *
 * ATOMIC_KEEP_READONLY leaves a destination the user cannot write unchanged,
 * fails with EACCES, and sets refused_readonly. Omitting this flag allows the
 * caller to force the commit. */
#define ATOMIC_KEEP_METADATA 0x1u
#define ATOMIC_KEEP_LINKS    0x2u
#define ATOMIC_KEEP_READONLY 0x4u
#define ATOMIC_KEEP_DOCUMENT (ATOMIC_KEEP_METADATA | ATOMIC_KEEP_LINKS | ATOMIC_KEEP_READONLY)

typedef struct AtomicFileWriter {
    FILE *stream;
    /* Identity of the committed file, taken with fstat() from the descriptor
     * that was actually written — see atomic_file_commit(). Only meaningful
     * when `committed_valid` is set, which happens on a successful commit and
     * never otherwise, so a caller that forgets to check cannot mistake a
     * failed write for a real identity. */
    bool committed_valid;
    struct stat committed_st;
    /* ATOMIC_KEEP_* bits. Zero after every atomic_file_begin*: the plain
     * rename. Set through atomic_file_keep(), between begin and commit. */
    u32  keep;
    /* Why a commit failed, when the reason is ATOMIC_KEEP_READONLY's. */
    bool refused_readonly;
    /* The commit rewrote the old inode instead of renaming over it. */
    bool wrote_in_place;
    /* Set through atomic_file_skip_sync(): the commit renames without
     * flushing the file or its folder to the disk. */
    bool no_sync;
    char final_path[ATOMIC_FILE_PATH_CAP];
    char temp_path[ATOMIC_FILE_PATH_CAP];
} AtomicFileWriter;

/* Creates the direct parent directory when needed and opens a temporary file
 * alongside the destination. Existing destination permissions are preserved. */
bool atomic_file_begin(AtomicFileWriter *writer, const char *path);
/* For a file the USER owns and Liu only edits (an agent's settings): the
 * replacement carries the permission bits the file already had, a new file is
 * created owner-only (0600) whatever the umask, an ACL the new inode would
 * inherit from its directory is cleared, and the parent directory's own
 * permissions are not changed. `path` is the file to replace — a caller that
 * must write THROUGH a symbolic link resolves it first (atomic_file_write_path). */
bool atomic_file_begin_personal(AtomicFileWriter *writer, const char *path);

/* Variant for app-private state: requires an owner-controlled direct parent,
 * restricts it to 0700, and always writes the destination as 0600. */
bool atomic_file_begin_private(AtomicFileWriter *writer, const char *path);

/* Where a write to `path` has to land. A rename replaces whatever the name is,
 * a symbolic link included: a config kept in a dotfiles folder and linked into
 * place would become a plain file at the first save, and the folder would no
 * longer hold what is in use. So when `path` is a link to an existing file,
 * `out` is that file — fully resolved, so the temporary file and the rename
 * happen in ITS directory and the link is never touched — and `*is_link`
 * (optional) is true. Otherwise `out` is `path`; that includes a dangling
 * link, which has nothing behind it to keep. False only when a link cannot be
 * followed (a loop, a folder that may not be searched) or `out` is too small,
 * and the caller must then write nothing. */
bool atomic_file_write_path(const char *path, char *out, usize cap, bool *is_link);

/* For Liu's own settings files, which a user may link in from a dotfiles
 * folder (config.json, keybindings.json): a link to an existing file is
 * written THROUGH (atomic_file_write_path) the way atomic_file_begin_personal
 * writes — the file keeps its permission bits and its folder is left as it
 * is, both being the user's; anything else is atomic_file_begin_private.
 * `writer->final_path` is where the bytes land. */
bool atomic_file_begin_private_through(AtomicFileWriter *writer, const char *path);

/* Every begin clears keep; a commit without atomic_file_keep() uses a plain
 * rename. */
static inline void atomic_file_keep(AtomicFileWriter *writer, u32 what) {
    if (writer && writer->stream) writer->keep = what;
}

/* For a file whose loss in a system crash or a power cut costs nothing a
 * reader cannot tell (it checks what it reads): the commit is still an atomic
 * rename — another process, or this one after a crash of its own, sees the old
 * file or the new one — but nothing waits for the disk. Every begin clears it. */
static inline void atomic_file_skip_sync(AtomicFileWriter *writer) {
    if (writer && writer->stream) writer->no_sync = true;
}

/* Flushes, fsyncs, closes, and atomically renames the temporary file.
 *
 * On success `writer->committed_st` holds the fstat() of the bytes that were
 * written, and `writer->committed_valid` is true. Callers that need to
 * recognise their own write later (Liu's config file watcher does) must use
 * that instead of stat()ing the destination afterwards: rename(2) preserves
 * the inode, size and mtime, so this identity is exactly what a later stat()
 * of the path reports — but with no window in which another writer could
 * replace the file and have ITS identity recorded as this write's. */
bool atomic_file_commit(AtomicFileWriter *writer);

/* Closes and removes an unfinished temporary file. */
void atomic_file_abort(AtomicFileWriter *writer);

/* True when `path` already holds exactly `data` as an app-private file: a
 * regular file (not a link) owned by the effective user, mode 0600, `len`
 * bytes long, byte for byte the same. A periodic writer asks this first and
 * leaves an unchanged file alone, so nothing is written or synced to disk for
 * a state that did not move. Any doubt — a missing file, another owner or
 * mode, a short read — answers false and the caller writes. */
bool atomic_file_holds_private(const char *path, const void *data, usize len);

/* `path` opened to READ ("rb"), judged on the descriptor: only a regular file
 * is opened, and a FIFO or a device in its place is refused at once (opened
 * without blocking) instead of holding the caller until something writes to
 * it. A symbolic link is followed. NULL with errno ENOENT when there is no
 * file and EINVAL when it is not a regular one. */
FILE *atomic_file_open_regular(const char *path);

#endif
