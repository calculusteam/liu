/* Worker-only, bounded image input and allocation accounting.
 *
 * A private encoded snapshot makes header inspection and decoding observe the
 * same bytes. An open descriptor alone would not protect against in-place
 * writes. Metadata before/after the copy and before publication rejects files
 * replaced or edited during the request. This is not a filesystem transaction:
 * the path can change after the final check; the next refresh discovers that.
 *
 * Header-only so every image producer shares the policy without adding a
 * decoder dependency to every imgproc.c consumer.
 */
#ifndef CORE_IMAGE_WORK_H
#define CORE_IMAGE_WORK_H

#include "core/imgproc.h"
#include "core/checked.h"
#include "core/mem_budget.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    u64 device, inode;
    i64 size, mtime_sec, mtime_ns, ctime_sec, ctime_ns;
    bool valid;
} ImgFileId;

typedef bool (*ImgWorkCancel)(void *ctx);
typedef bool (*ImgWorkResize)(void *ctx, usize before, usize after);
typedef struct {
    usize bytes;
    ImgWorkResize resize;       /* optional admission quota, not the ledger */
    void *ctx;
} ImgWork;

typedef struct {
    u8 *data;
    usize len;
    ImgFileId id;
} ImgInput;

static inline ImgFileId img_file_id(const struct stat *st) {
    ImgFileId id = {0};
    if (!st || !S_ISREG(st->st_mode)) return id;
    id.valid = true;
    id.device = (u64)st->st_dev;
    id.inode = (u64)st->st_ino;
    id.size = (i64)st->st_size;
    id.mtime_sec = (i64)st->st_mtime;
    id.ctime_sec = (i64)st->st_ctime;
#if defined(__APPLE__)
    id.mtime_ns = (i64)st->st_mtimespec.tv_nsec;
    id.ctime_ns = (i64)st->st_ctimespec.tv_nsec;
#elif !defined(_WIN32)
    id.mtime_ns = (i64)st->st_mtim.tv_nsec;
    id.ctime_ns = (i64)st->st_ctim.tv_nsec;
#endif
    return id;
}

static inline ImgFileId img_file_stat(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return (ImgFileId){0};
    return img_file_id(&st);
}

static inline bool img_file_same(ImgFileId a, ImgFileId b) {
    if (!a.valid || !b.valid) return a.valid == b.valid;
    return a.device == b.device && a.inode == b.inode && a.size == b.size &&
           a.mtime_sec == b.mtime_sec && a.mtime_ns == b.mtime_ns &&
           a.ctime_sec == b.ctime_sec && a.ctime_ns == b.ctime_ns;
}

static inline bool img_work_cancelled(ImgWorkCancel cancel, void *ctx) {
    return cancel && cancel(ctx);
}

/* Reserve before allocating, reconcile immediately after freeing. The quota
 * callback MUST always accept decreases. Counters cover requested encoded and
 * RGBA allocations, not allocator rounding, thread stacks or decoder scratch. */
static inline bool img_work_set(ImgWork *work, usize bytes) {
    if (!work || bytes > (usize)PTRDIFF_MAX) return false;
    usize old = work->bytes;
    if (old == bytes) return true;
    if (work->resize && !work->resize(work->ctx, old, bytes)) return false;
    work->bytes = bytes;
    if (bytes > old)
        mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)(bytes - old));
    else
        mem_budget_transient_adjust(MEM_IMAGE_CPU, -(isize)(old - bytes));
    return true;
}

static inline bool img_work_add(ImgWork *work, usize bytes) {
    usize total;
    return checked_add_usize(work->bytes, bytes, &total) &&
           img_work_set(work, total);
}

static inline void img_work_drop(ImgWork *work, usize bytes) {
    /* Every drop is paired with a successful reservation. */
    if (bytes <= work->bytes) (void)img_work_set(work, work->bytes - bytes);
}

static inline void img_input_free(ImgInput *input, ImgWork *work) {
    if (!input || !input->data) return;
    free(input->data);
    img_work_drop(work, input->len);
    input->data = NULL;
    input->len = 0;
}

static inline bool img_input_current(const char *path, const ImgInput *input) {
    return input->id.valid && img_file_same(input->id, img_file_stat(path));
}

/* No unbounded stream/FIFO reads. Cancellation is checked between 64 KiB
 * reads; it cannot interrupt a kernel read already blocked on a filesystem. */
static inline bool img_input_read(const char *path, usize max_bytes,
                                  ImgWork *work, ImgWorkCancel cancel,
                                  void *ctx, ImgInput *out) {
    memset(out, 0, sizeof *out);
    if (img_work_cancelled(cancel, ctx)) return false;
    int flags = O_RDONLY;
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags);
    if (fd < 0) return false;
    struct stat before, after;
    bool ok = false;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 || (u64)before.st_size > (u64)max_bytes ||
        (u64)before.st_size > (u64)INT_MAX ||
        img_work_cancelled(cancel, ctx)) goto done;
    out->id = img_file_id(&before);
    usize len = (usize)before.st_size;
    if (!img_work_add(work, len)) goto done;
    out->data = malloc(len);
    if (!out->data) { img_work_drop(work, len); goto done; }
    out->len = len;
    usize off = 0;
    while (off < len) {
        if (img_work_cancelled(cancel, ctx)) goto done;
        usize n = len - off;
        if (n > 64u * 1024u) n = 64u * 1024u;
        ssize_t got = read(fd, out->data + off, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) goto done;
        off += (usize)got;
    }
    ok = !img_work_cancelled(cancel, ctx) && fstat(fd, &after) == 0 &&
         img_file_same(out->id, img_file_id(&after)) &&
         img_input_current(path, out);
done:
    close(fd);
    if (!ok) img_input_free(out, work);
    return ok;
}

/* Charge source + destination while the downscale owns both; release the
 * source charge immediately, including while a done queue is not being read. */
static inline bool img_work_shrink(ImgWork *work, u8 **px,
                                   i32 *w, i32 *h, i32 max_dim) {
    i32 nw = *w, nh = *h;
    img_shrink_dims(&nw, &nh, max_dim);
    if (nw == *w && nh == *h) return true;
    usize source = 0, target = 0;
    if (!img_rgba_bytes_checked(*w, *h, &source) ||
        !img_rgba_bytes_checked(nw, nh, &target) ||
        !img_work_add(work, target)) return false;
    if (!img_shrink_to_max_dim(px, w, h, max_dim)) {
        img_work_drop(work, target);
        return false;
    }
    img_work_drop(work, source);
    return true;
}

#endif /* CORE_IMAGE_WORK_H */
