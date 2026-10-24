/*
 * Liu — is the file on disk still the one that was loaded?
 *
 * The editor and the Markdown reader read a file once and keep their copy for
 * as long as it stays open. Without a check, Cmd+S, or a click on a task
 * checkbox in a note, would write that copy back over whatever the file has
 * become meanwhile — and in a terminal built around agents, "meanwhile" is the
 * agent rewriting the very file being read.
 *
 * So the open document remembers WHICH file it holds, and asks before it
 * writes. The identity is the transclusion cache's key (md_embed.c), for the
 * same two reasons: the inode catches a rename-over save (an editor's, an
 * agent's, Liu's own), and nanoseconds with the size catch an in-place one —
 * whole seconds would miss a rewrite to the same length inside the second.
 *
 * Header-only, one stat and no state of its own. A stat that fails for any
 * reason other than "not there" answers UNKNOWN, which callers treat as "do
 * not get in the way": a file that cannot be asked about is not thereby
 * changed.
 */
#ifndef UI_FILE_IDENTITY_H
#define UI_FILE_IDENTITY_H

#include "core/types.h"

#include <errno.h>
#include <sys/stat.h>

typedef struct {
    bool valid;          /* false: nothing was recorded, so nothing can differ */
    u64  dev, ino;
    u64  size;
    i64  mtime_sec;
    i64  mtime_nsec;
} FileIdentity;

typedef enum {
    FILE_DISK_UNKNOWN = 0,   /* no identity recorded, or the file cannot be asked */
    FILE_DISK_SAME,
    FILE_DISK_CHANGED,       /* another inode, another size, or written since */
    FILE_DISK_MISSING,       /* nothing at that path any more */
} FileDiskState;

static inline FileIdentity file_identity_of(const struct stat *st) {
    FileIdentity id = {0};
    if (!st) return id;
    id.valid     = true;
    id.dev       = (u64)st->st_dev;
    id.ino       = (u64)st->st_ino;
    id.size      = (u64)st->st_size;
    id.mtime_sec = (i64)st->st_mtime;
#ifdef __APPLE__
    id.mtime_nsec = (i64)st->st_mtimespec.tv_nsec;
#else
    id.mtime_nsec = (i64)st->st_mtim.tv_nsec;
#endif
    return id;
}

static inline bool file_identity_same(const FileIdentity *a, const FileIdentity *b) {
    if (!a || !b || !a->valid || !b->valid) return false;
    return a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
           a->mtime_sec == b->mtime_sec && a->mtime_nsec == b->mtime_nsec;
}

/* The same FILE, whatever was written to it since: what "this path now names
 * the held document" means after a rename. */
static inline bool file_identity_same_inode(const FileIdentity *a, const FileIdentity *b) {
    if (!a || !b || !a->valid || !b->valid) return false;
    return a->dev == b->dev && a->ino == b->ino;
}

/* One stat of `path` (links followed, as the read that loaded it did) against
 * what was recorded. */
static inline FileDiskState file_identity_check(const char *path, const FileIdentity *known) {
    if (!path || !path[0] || !known || !known->valid) return FILE_DISK_UNKNOWN;
    struct stat st;
    if (stat(path, &st) != 0)
        return (errno == ENOENT || errno == ENOTDIR) ? FILE_DISK_MISSING : FILE_DISK_UNKNOWN;
    FileIdentity now = file_identity_of(&st);
    return file_identity_same(known, &now) ? FILE_DISK_SAME : FILE_DISK_CHANGED;
}

#endif /* UI_FILE_IDENTITY_H */
