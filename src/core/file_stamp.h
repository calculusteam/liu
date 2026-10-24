/* File revision captured before asynchronous work; never relabel old bytes
 * with a newer stat result when publishing them to a cache. */
#ifndef LIU_FILE_STAMP_H
#define LIU_FILE_STAMP_H
#include "core/types.h"
#include <sys/stat.h>

typedef struct {
    u64 size;
    i64 mtime_sec, mtime_nsec;
    u64 device, inode;
    i64 ctime_sec, ctime_nsec;
} FileStamp;

static inline bool file_stamp_read(const char *path, FileStamp *out) {
    struct stat st;
    if (!path || !out || stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0)
        return false;
    out->size = (u64)st.st_size;
    out->device = (u64)st.st_dev;
    out->inode = (u64)st.st_ino;
#ifdef __APPLE__
    out->mtime_sec = (i64)st.st_mtimespec.tv_sec;
    out->mtime_nsec = (i64)st.st_mtimespec.tv_nsec;
    out->ctime_sec = (i64)st.st_ctimespec.tv_sec;
    out->ctime_nsec = (i64)st.st_ctimespec.tv_nsec;
#elif defined(_WIN32)
    out->mtime_sec = (i64)st.st_mtime;
    out->mtime_nsec = 0;
    out->ctime_sec = (i64)st.st_ctime;
    out->ctime_nsec = 0;
#else
    out->mtime_sec = (i64)st.st_mtim.tv_sec;
    out->mtime_nsec = (i64)st.st_mtim.tv_nsec;
    out->ctime_sec = (i64)st.st_ctim.tv_sec;
    out->ctime_nsec = (i64)st.st_ctim.tv_nsec;
#endif
    return true;
}

static inline bool file_stamp_equal(const FileStamp *a, const FileStamp *b) {
    return a && b && a->size == b->size && a->mtime_sec == b->mtime_sec &&
           a->mtime_nsec == b->mtime_nsec && a->device == b->device &&
           a->inode == b->inode && a->ctime_sec == b->ctime_sec &&
           a->ctime_nsec == b->ctime_nsec;
}

static inline bool file_stamp_matches(const char *path, const FileStamp *stamp) {
    FileStamp current;
    return stamp && file_stamp_read(path, &current) && file_stamp_equal(stamp, &current);
}
#endif
