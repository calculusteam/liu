/*
 * Liu - Markdown image cache implementation.
 */
#include "ui/markdown/md_image.h"
#include "core/checked.h"
#include "core/imgproc.h"
#include "core/image_work.h"
#include "core/mem_budget.h"
#include "platform/platform.h"
#include "stb_image.h"
#include "platform/platform.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Total RGBA-byte budget across all live cache entries. When exceeded after an
 * insert, cold LRU entries lose their pixels but remain reloadable. The drawn
 * set may overshoot until it leaves the render-pass grace. */
#define MD_IMAGE_CACHE_BUDGET ((usize)32 * 1024 * 1024)
/* Hard cap on entry count so adversarial / auto-generated markdown
 * with thousands of distinct image refs can't grow bucket-chain
 * metadata (struct + strdup'd abs_path, ~150 B–2 KB each) without
 * bound. Pixels are already capped by MD_IMAGE_CACHE_BUDGET; this
 * caps the *metadata* tail that evicted entries leave behind. */
#define MD_IMAGE_CACHE_MAX_ENTRIES 2048

/* Oversized inline images are box-filter downscaled at decode so the GPU never
 * minifies a multi-thousand-px screenshot down to the <=480pt display size with
 * a single bilinear tap (which shimmers on scroll), and so the cache/texture
 * footprint tracks display size rather than source size. 1536 px keeps a sharp
 * margin above the 480pt display cap even at 3x backing scale. */
#define MD_IMAGE_MAX_DIM 1536
/* Decode happens before downscale. Bound the one serial worker to a 32 MiB RGBA
 * source (8 Mi pixels, enough for 3840x2160). Larger originals remain available
 * through the file viewer/lightbox; an inline reading surface does not need an
 * 8K/camera source to produce a 1536 px retained image. */
#define MD_IMAGE_MAX_SOURCE_PIXELS ((usize)8 * 1024 * 1024)
/* The ENCODED file is read whole into a private snapshot before anything looks
 * at it (see md_image_decode_job), so it needs a bound of its own: a file past
 * this is refused from its size alone, before a byte is allocated. Generous —
 * 8 Mi pixels of PNG is a few MiB — because it only has to stop a mistake or a
 * hostile path, not shape the cache. */
#define MD_IMAGE_MAX_ENCODED_BYTES ((usize)48 * 1024 * 1024)
#define MD_IMAGE_QUEUE_CAP 16
/* Retry ladder for a failed entry (see MdImageEntry.fail_next). */
#ifndef MD_IMAGE_RETRY_MIN_SEC
#define MD_IMAGE_RETRY_MIN_SEC 2.0
#endif
#ifndef MD_IMAGE_RETRY_MAX_SEC
#define MD_IMAGE_RETRY_MAX_SEC 60.0
#endif

/* Process-global, monotonic id stamped on every successful rgba load. Used as
 * the GPU image-cache generation: MdImageEntry structs are freed and their
 * addresses malloc-reused across document switches, so keying the renderer's
 * pointer-addressed image cache by the struct address alone (at a constant
 * generation) let a recycled address alias a previous document's texture.
 * A unique generation per load makes the renderer re-upload instead. */
static u64 g_md_image_serial = 0;

typedef struct {
    MdImageEntry *entry;
    u64           token;
    char          path[2048];
    bool          header_only, async_headers;
    MdImageEntry  metadata;     /* copied values; worker never reads entry */
} MdImageJob;

typedef struct {
    MdImageEntry *entry;
    u64           token;
    u8           *rgba;
    i32           w, h;
    usize         bytes;
    usize         transient_bytes;
    bool          ok;
    bool          header_only, failure_recorded;
    MdImageEntry  metadata;
} MdImageDone;

static bool md_image_probe_header(MdImageEntry *e);
static bool md_image_fail_retry_due(MdImageEntry *e);
static void md_image_fail(MdImageEntry *e);
static void md_image_fail_from(MdImageEntry *e, const struct stat *st, bool exists);

typedef struct {
    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            started;
    bool            stop;
    /* True while the worker is inside md_image_decode_job, i.e. exactly when
     * a join would have to wait. Guarded by `mu`, so destroy reads a value
     * that cannot change under it: the worker only flips it while holding
     * the same mutex. */
    bool            busy;
    /* Read by the decode between its stages, so it must be lock-free: the
     * worker holds no mutex while it is inside stb_image. */
    _Atomic bool    cancel;
    /* The cache let go without joining; this thread owns its own teardown.
     * Set under `mu` in the same critical section as `stop`, which is the
     * only thing that ever ends the loop, so a worker that observed `stop`
     * has necessarily observed this too. */
    bool            orphan;
    MdImageJob      jobs[MD_IMAGE_QUEUE_CAP];
    u32             job_head, job_tail;
    MdImageDone     done[MD_IMAGE_QUEUE_CAP];
    u32             done_head, done_tail;
    _Atomic bool    done_ready;
} MdImageAsync;

/* Process-wide gate on the HEAVY part of a decode — stbi_load, the
 * premultiply, the downscale — and therefore on how much source RGBA can
 * exist at once.
 *
 * Every MdImageCache owns a worker, and there is one cache per FileBrowser,
 * so the count follows how many Markdown views are open rather than anything
 * about the machine. Each cache obeys its own 32 MiB retained budget, but no
 * per-cache limit sees the process total: a theme reload that repaints every
 * open note asks every cache to decode at once, and with many views open
 * that transient decode memory reaches a large share of
 * MEM_BUDGET_SOFT_TOTAL.
 *
 * Two permits rather than one: a single decoder makes the second of two
 * visible notes wait out the first, and two is enough to keep a core busy
 * while another thread is in the read. The bound is what matters — two
 * permits cap the source RGBA at 2 x MD_IMAGE_MAX_SOURCE_PIXELS x 4, i.e. 64
 * MiB, whatever the tab count.
 *
 * A gate, not a thread pool: the cache still owns its worker, so
 * cancellation and orphaning stay per cache — a destroy cancels its own
 * decode and never waits on an unrelated one. Waiting for a permit is itself
 * cancellable, so a cache torn down while queued does not first have to
 * reach the front. */
#define MD_DECODE_PERMITS 2
static pthread_mutex_t g_md_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_md_gate_cv = PTHREAD_COND_INITIALIZER;
static i32             g_md_gate_free = MD_DECODE_PERMITS;

/* Returns false when the caller was cancelled while queuing. */
static bool md_decode_gate_acquire(MdImageAsync *a) {
    pthread_mutex_lock(&g_md_gate_mu);
    while (g_md_gate_free <= 0) {
        if (atomic_load_explicit(&a->cancel, memory_order_acquire)) {
            pthread_mutex_unlock(&g_md_gate_mu);
            return false;
        }
        /* Timed, so a cancel that lands while this thread is parked is
         * noticed without the canceller having to know about this gate —
         * md_image_async_destroy already broadcasts on the cache's own
         * condvar, not this one. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_md_gate_cv, &g_md_gate_mu, &ts);
    }
    g_md_gate_free--;
    pthread_mutex_unlock(&g_md_gate_mu);
    return true;
}

static void md_decode_gate_release(void) {
    pthread_mutex_lock(&g_md_gate_mu);
    g_md_gate_free++;
    pthread_cond_signal(&g_md_gate_cv);
    pthread_mutex_unlock(&g_md_gate_mu);
}

/* How many workers, process-wide, may be INSIDE a job at once — header probe
 * or decode, owned by a live cache or orphaned.
 *
 * This is what lets md_image_async_destroy never join. A cache that goes away
 * while its worker is inside a job orphans it (the worker finishes on its own
 * and frees itself), so destroy never waits on the UI thread (a tab close, an
 * Edit->Read toggle) for a worker's read. A storm of reparses must still not
 * leave unbounded work running, so the bound is taken at ADMISSION: a worker
 * may not start a job without a slot, so no more than this many can be stuck
 * anywhere, an orphan is by definition one of them, and a worker still waiting
 * for a slot holds nothing and leaves the moment its cache cancels it. */
#define MD_IMAGE_MAX_ACTIVE 8
static pthread_mutex_t g_md_active_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_md_active_cv = PTHREAD_COND_INITIALIZER;
static i32             g_md_active_free = MD_IMAGE_MAX_ACTIVE;

/* Process-wide count of live orphaned workers, bounded by `g_md_active_free`.
 */
static _Atomic i32 g_md_orphan_workers;

static u64 g_md_load_token;
static inline u32 mdq_next(u32 i) { return (i + 1u) % MD_IMAGE_QUEUE_CAP; }

/* Returns false when the caller was cancelled while waiting for a slot. */
static bool md_active_acquire(MdImageAsync *a) {
    pthread_mutex_lock(&g_md_active_mu);
    while (g_md_active_free <= 0) {
        if (atomic_load_explicit(&a->cancel, memory_order_acquire)) {
            pthread_mutex_unlock(&g_md_active_mu);
            return false;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_md_active_cv, &g_md_active_mu, &ts);
    }
    g_md_active_free--;
    pthread_mutex_unlock(&g_md_active_mu);
    return true;
}

static void md_active_release(void) {
    pthread_mutex_lock(&g_md_active_mu);
    g_md_active_free++;
    pthread_cond_signal(&g_md_active_cv);
    pthread_mutex_unlock(&g_md_active_mu);
}

/* Read image dimensions from the file header without decoding the image. Open
 * non-blocking and verify with `fstat` that the descriptor is a regular file
 * before reading; `stbi_info(path)` opens the path directly and can block on a
 * FIFO, socket, or device when called on the main thread. */
static int md_image_file_info(const char *path, int *w, int *h, int *comp) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return 0; }
    FILE *f = fdopen(fd, "rb");
    if (!f) { close(fd); return 0; }
    int ok = stbi_info_from_file(f, w, h, comp);
    fclose(f);
    return ok;
}
#ifndef MD_IMAGE_FILE_INFO
#define MD_IMAGE_FILE_INFO md_image_file_info
#endif
/* Optional callback invoked on the worker after the encoded image snapshot is
 * taken. */
#ifndef MD_IMAGE_TEST_AFTER_SNAPSHOT
#define MD_IMAGE_TEST_AFTER_SNAPSHOT(path) ((void)0)
#endif

/* Premultiply and box-downscale live in core/imgproc.c so every image producer
 * in the tree shares one implementation. The premultiply happens in LINEAR
 * light rather than in the stored sRGB byte domain (`c * a / 255` on the sRGB
 * byte stores srgb_decode(c*a) where the sampler needs srgb_decode(c)*a, which
 * darkens every soft edge), and it does not depend on the source having
 * reported 4 channels, so grayscale+alpha PNGs are premultiplied too.
 */

/* LRU helpers: head = most-recently-used, tail = eviction victim. */
static void lru_unlink(MdImageCache *c, MdImageEntry *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else if (c->lru_head == e) c->lru_head = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else if (c->lru_tail == e) c->lru_tail = e->lru_prev;
    e->lru_prev = e->lru_next = NULL;
}

static void lru_push_head(MdImageCache *c, MdImageEntry *e) {
    e->lru_prev = NULL;
    e->lru_next = c->lru_head;
    if (c->lru_head) c->lru_head->lru_prev = e;
    c->lru_head = e;
    if (!c->lru_tail) c->lru_tail = e;
}

/* FNV-1a 32-bit hash for ASCII paths. */
static u32 fnv1a(const char *s) {
    u32 h = 0x811c9dc5u;
    for (; *s; s++) {
        h ^= (u8)*s;
        h *= 0x01000193u;
    }
    return h;
}

/* Concat base_dir + "/" + ref → absolute-ish path. ref may already be
 * absolute (starts with '/') in which case base_dir is ignored. ref is a
 * length-bounded slice (ref_len bytes — not NUL-terminated). */
static bool resolve_path(const char *base_dir, const char *ref, u32 ref_len,
                         char *out, usize out_sz) {
    if (!out || out_sz == 0 || (!ref && ref_len != 0)) return false;
    out[0] = '\0';
    if (ref_len == 0) return true;
    if (ref[0] == '/') {
        if ((usize)ref_len >= out_sz) return false;
        memcpy(out, ref, ref_len);
        out[ref_len] = '\0';
        return true;
    }
    if (!base_dir || !*base_dir) {
        if ((usize)ref_len >= out_sz) return false;
        memcpy(out, ref, ref_len);
        out[ref_len] = '\0';
        return true;
    }
    usize base_len = strlen(base_dir), need;
    if (!checked_add_usize(base_len, 1, &need) ||
        !checked_add_usize(need, (usize)ref_len, &need) ||
        !checked_add_usize(need, 1, &need) || need > out_sz) return false;
    memcpy(out, base_dir, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, ref, ref_len);
    out[need - 1] = '\0';
    return true;
}

/* stb_image offers no way in, so the decode itself cannot be interrupted —
 * cancellation is checked at the stage boundaries around it. That is enough
 * to make the tail cheap: the premultiply and the box downscale together are
 * the same order of magnitude as the decode, and skipping the whole job when
 * the cancel landed before stbi_load started costs nothing at all. */
static bool md_image_job_cancelled(void *ctx) {
    return atomic_load_explicit(&((MdImageAsync *)ctx)->cancel, memory_order_acquire);
}

/* ONE read of the file, and everything is decided from those bytes.
 *
 * Opening the file twice by NAME — stbi_info(path), then stbi_load(path) —
 * would let the header that passed the size check and the bytes that are
 * decoded be two different files (a rename in between), and neither open
 * would ask what kind of thing the path is: a FIFO would hold the worker in
 * fopen for as long as nobody wrote to it. The shared input model
 * (core/image_work.h) opens non-blocking, checks the DESCRIPTOR is a regular
 * file within the encoded bound, reads a private snapshot with cancellation
 * between chunks, and rejects a file that changed while it was being read.
 * The header check and the decoder then see the same immutable bytes, and a
 * file that changed during the decode is not published.
 *
 * Accounting: the encoded snapshot and the RGBA are both reserved on the
 * transient ledger BEFORE they are allocated (ImgWork). Whatever is still
 * reserved when this returns travels in `transient_bytes`, and the pump — or
 * whoever drops the result — settles it. */
static MdImageDone md_image_decode_job(MdImageAsync *a, const MdImageJob *j) {
    MdImageDone d = { .entry = j->entry, .token = j->token };
    if (atomic_load_explicit(&a->cancel, memory_order_acquire)) return d;
    if (j->header_only) {
        MdImageEntry info = j->metadata;
        info.abs_path = (char *)j->path;
        if (!info.failed || md_image_fail_retry_due(&info))
            (void)md_image_probe_header(&info);
        info.abs_path = NULL;
        d.header_only = true; d.metadata = info;
        return d;
    }
    /* The whole decode is inside the gate, the read included: the encoded
     * snapshot is an allocation too, and outside the gate every open note
     * could hold one while waiting its turn. */
    if (!md_decode_gate_acquire(a)) return d;
    /* ONE exit from here on — every path must hand the permit back. */
    ImgWork work = {0};
    ImgInput input = {0};
    u8 *px = NULL;
    int w = 0, h = 0, ch = 0;
    usize source_pixels = 0, source_bytes = 0;

    if (!img_input_read(j->path, MD_IMAGE_MAX_ENCODED_BYTES, &work,
                        md_image_job_cancelled, a, &input)) goto release;
    MD_IMAGE_TEST_AFTER_SNAPSHOT(j->path);
    if (!stbi_info_from_memory(input.data, (int)input.len, &w, &h, &ch) ||
        w <= 0 || h <= 0 ||
        !checked_mul_usize((usize)w, (usize)h, &source_pixels) ||
        source_pixels > MD_IMAGE_MAX_SOURCE_PIXELS ||
        !checked_mul_usize(source_pixels, 4u, &source_bytes)) goto release;
    int header_w = w, header_h = h;
    /* Reserve the RGBA before the decoder allocates it. */
    if (md_image_job_cancelled(a) || !img_work_add(&work, source_bytes)) goto release;

    px = stbi_load_from_memory(input.data, (int)input.len, &w, &h, &ch, 4);
    if (!px) goto release;
    /* A decoder that disagrees with its own header, a cancel, or a file that
     * is no longer the one that was read: nothing is published. */
    if (w != header_w || h != header_h || md_image_job_cancelled(a) ||
        !img_input_current(j->path, &input)) goto release;
    img_input_free(&input, &work);               /* the encoded bytes are done with */

    img_premultiply_rgba(px, w, h);
    if (!img_work_shrink(&work, &px, &w, &h, MD_IMAGE_MAX_DIM) ||
        !img_rgba_bytes_checked(w, h, &d.bytes)) goto release;
    d.rgba = px; px = NULL;
    d.w = w; d.h = h; d.ok = true;

release:
    if (px) stbi_image_free(px);
    img_input_free(&input, &work);
    if (!d.ok) (void)img_work_set(&work, 0);     /* a failed job keeps nothing reserved */
    d.transient_bytes = work.bytes;
    md_decode_gate_release();
    return d;
}

/* Release the queued results and the async state itself. The caller must own
 * `a` outright: either the cache before the worker was ever started, or the
 * worker on its way out after the cache orphaned it. */
static void md_image_async_free(MdImageAsync *a) {
    for (u32 i = a->done_head; i != a->done_tail; i = mdq_next(i)) {
        if (a->done[i].rgba) stbi_image_free(a->done[i].rgba);
        if (a->done[i].transient_bytes)
            mem_budget_transient_adjust(MEM_IMAGE_CPU,
                                        -(isize)a->done[i].transient_bytes);
    }
    pthread_cond_destroy(&a->cv);
    pthread_mutex_destroy(&a->mu);
    free(a);
}

static void *md_image_worker(void *arg) {
    MdImageAsync *a = arg;
#ifdef __APPLE__
    pthread_setname_np("liu-md-image");
#else
    pthread_setname_np(pthread_self(), "liu-md-image");
#endif
    for (;;) {
        pthread_mutex_lock(&a->mu);
        while (!a->stop && a->job_head == a->job_tail)
            pthread_cond_wait(&a->cv, &a->mu);
        if (a->stop) { pthread_mutex_unlock(&a->mu); break; }
        MdImageJob job = a->jobs[a->job_head];
        a->job_head = mdq_next(a->job_head);
        a->busy = true;
        pthread_cond_broadcast(&a->cv);
        pthread_mutex_unlock(&a->mu);

        /* No job starts without a slot (MD_IMAGE_MAX_ACTIVE). A worker whose
         * cache cancelled it while it waited gets none and has done nothing:
         * the empty result below is dropped at the `stop` check. */
        MdImageDone done = { .entry = job.entry, .token = job.token };
        if (md_active_acquire(a)) {
            struct stat before;
            bool existed = job.async_headers && !job.header_only && stat(job.path, &before) == 0;
            done = md_image_decode_job(a, &job);
            if (job.async_headers && !done.header_only && !done.ok) {
                MdImageEntry info = job.metadata;
                info.abs_path = job.path;
                md_image_fail_from(&info, &before, existed);
                info.abs_path = NULL;
                done.metadata = info; done.failure_recorded = true;
            }
            md_active_release();
        }
        pthread_mutex_lock(&a->mu);
        a->busy = false;
        while (!a->stop && mdq_next(a->done_tail) == a->done_head)
            pthread_cond_wait(&a->cv, &a->mu);
        if (a->stop) {
            pthread_mutex_unlock(&a->mu);
            if (done.rgba) stbi_image_free(done.rgba);
            if (done.transient_bytes)
                mem_budget_transient_adjust(MEM_IMAGE_CPU,
                                            -(isize)done.transient_bytes);
            break;
        }
        a->done[a->done_tail] = done;
        a->done_tail = mdq_next(a->done_tail);
        atomic_store_explicit(&a->done_ready, true, memory_order_release);
        pthread_mutex_unlock(&a->mu);
        platform_wake_event_loop();
    }

    /* The loop only ends on `stop`, which only md_image_async_destroy sets,
     * and it sets `orphan` in the same critical section. So an orphaned
     * worker always reaches here knowing it owns the teardown. */
    pthread_mutex_lock(&a->mu);
    bool orphan = a->orphan;
    pthread_mutex_unlock(&a->mu);
    if (orphan) {
        md_image_async_free(a);
        atomic_fetch_sub_explicit(&g_md_orphan_workers, 1,
                                  memory_order_release);
    }
    return NULL;
}

static MdImageAsync *md_image_async_get(MdImageCache *c) {
    MdImageAsync *a = c ? c->async : NULL;
    if (a) return a;
    a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    pthread_mutex_init(&a->mu, NULL);
    pthread_cond_init(&a->cv, NULL);
    atomic_init(&a->done_ready, false);
    atomic_init(&a->cancel, false);
    if (pthread_create(&a->thread, NULL, md_image_worker, a) != 0) {
        pthread_cond_destroy(&a->cv);
        pthread_mutex_destroy(&a->mu);
        free(a);
        return NULL;
    }
    a->started = true;
    c->async = a;
    return a;
}

static bool md_image_enqueue(MdImageCache *c, MdImageEntry *e, bool header_only) {
    MdImageAsync *a = md_image_async_get(c);
    if (!a || !e || !e->abs_path) return false;
    pthread_mutex_lock(&a->mu);
    u32 next = mdq_next(a->job_tail);
    if (next == a->job_head) { pthread_mutex_unlock(&a->mu); return false; }
    MdImageJob *j = &a->jobs[a->job_tail];
    memset(j, 0, sizeof(*j));
    j->entry = e;
    j->token = e->load_token;
    j->header_only = header_only; j->async_headers = c->async_headers;
    j->metadata = *e;
    snprintf(j->path, sizeof(j->path), "%s", e->abs_path);
    a->job_tail = next;
    pthread_cond_signal(&a->cv);
    pthread_mutex_unlock(&a->mu);
    return true;
}

/* The cache is going away. Joining here would park the caller inside
 * stb_image for the length of whatever decode is in flight — and the callers
 * are interactive: fb_md_reparse runs on an Edit->Read toggle and on every
 * markdown checkbox click, and fb_destroy runs on a tab or window close.
 *
 * So the worker is CANCELLED and ORPHANED instead: it owns its own teardown
 * and this returns as soon as the queues are drained. That is safe because
 * the worker never dereferences MdImageEntry — it copies the pointer and the
 * token through to the done queue, and only md_image_cache_pump (main thread,
 * on a cache that is still alive) ever follows them. An orphan's results are
 * simply dropped: c->async is cleared here, so nothing can pump them.
 *
 * A worker that is inside a job is ALWAYS orphaned, however many orphans
 * already exist. Their number is bounded where the work starts
 * (MD_IMAGE_MAX_ACTIVE): an orphan is a worker that held a slot. */
static void md_image_async_destroy(MdImageCache *c) {
    MdImageAsync *a = c ? c->async : NULL;
    if (!a) return;
    c->async = NULL;

    if (!a->started) {          /* pthread_create failed — nothing to cancel */
        md_image_async_free(a);
        return;
    }

    pthread_mutex_lock(&a->mu);
    /* Orphaning is only worth its bookkeeping when a join would actually
     * block. An idle worker is parked in the condvar, so joining it after the
     * broadcast below returns immediately — and not spending a slot there
     * keeps them for the case that does stall. */
    bool orphan = a->busy;
    if (orphan) {
        atomic_fetch_add_explicit(&g_md_orphan_workers, 1,
                                  memory_order_acq_rel);
        /* Detach HERE, under the lock, while this thread still owns `a`.
         *
         * The worker's exit path frees `a`, and the only thing standing
         * between it and that free is this mutex — so a detach placed after
         * the unlock would read a->thread out of memory the worker may
         * already have released. Detaching while the worker cannot yet reach
         * its epilogue closes that window, and detaching a still-running
         * thread is exactly what pthread_detach is for. */
        pthread_detach(a->thread);
    }
    a->stop = true;
    a->orphan = orphan;
    atomic_store_explicit(&a->cancel, true, memory_order_release);
    /* Drop the jobs the worker has not started: it must not decode for a
     * cache that no longer exists. */
    a->job_head = a->job_tail;
    /* The results already queued are ours to release — the worker will not
     * touch them again, and after the unlock below it may free `a` itself. */
    for (u32 i = a->done_head; i != a->done_tail; i = mdq_next(i)) {
        if (a->done[i].rgba) stbi_image_free(a->done[i].rgba);
        if (a->done[i].transient_bytes)
            mem_budget_transient_adjust(MEM_IMAGE_CPU,
                                        -(isize)a->done[i].transient_bytes);
        memset(&a->done[i], 0, sizeof(a->done[i]));
    }
    a->done_head = a->done_tail;
    atomic_store_explicit(&a->done_ready, false, memory_order_release);
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);

    /* NOTHING may touch `a` past this point on the orphan path: the worker
     * owns it from the unlock above, and it is already detached. On the
     * non-orphan path the worker leaves `a` alone (it only frees when
     * a->orphan is set), so the join and the free below are ours to make. */
    if (orphan) return;
    pthread_join(a->thread, NULL);
    md_image_async_free(a);
}

MdImageCache *md_image_cache_create(void) {
    MdImageCache *c = calloc(1, sizeof(*c));
    /* Epoch starts at 1 so calloc'd (never-drawn) entries at epoch 0 are not
     * accidentally pinned — see md_image_enforce_budget. */
    if (c) c->epoch = 1;
    return c;
}

void md_image_cache_destroy(MdImageCache *c) {
    if (!c) return;
    md_image_async_destroy(c);
    if (c->total_bytes)
        mem_budget_adjust(MEM_IMAGE_CPU, -(isize)c->total_bytes);
    for (i32 i = 0; i < (i32)(sizeof c->buckets / sizeof c->buckets[0]); i++) {
        MdImageEntry *e = c->buckets[i];
        while (e) {
            MdImageEntry *next = e->next;
            free(e->abs_path);
            if (e->rgba) stbi_image_free(e->rgba);
            free(e);
            e = next;
        }
    }
    free(c);
}

/* Evict LRU tails until the byte budget is met.
 *
 * `keep` is never evicted: it is the entry that was just loaded, and freeing it
 * on the very call that decoded it would leave it blank forever. */
static void md_image_enforce_budget(MdImageCache *c, MdImageEntry *keep) {
    MdImageEntry *victim = c->lru_tail;
    while (c->total_bytes > MD_IMAGE_CACHE_BUDGET && victim) {
        MdImageEntry *prev = victim->lru_prev;
        /* During a render pass, protect entries drawn in the current or
         * previous epoch from eviction. This prevents repeated decoding when
         * the visible set exceeds the budget; entries no longer drawn become
         * eligible once they are two epochs old. Async completions are inserted
         * at the list head independently of draw order, so scan past protected
         * entries to find an evictable one. */
        if (victim == keep || victim->epoch + 1 >= c->epoch) {
            victim = prev;
            continue;
        }
        lru_unlink(c, victim);
        if (victim->rgba) {
            stbi_image_free(victim->rgba);
            victim->rgba = NULL;
        }
        c->total_bytes = victim->bytes <= c->total_bytes
                       ? c->total_bytes - victim->bytes : 0;
        if (victim->bytes)
            mem_budget_adjust(MEM_IMAGE_CPU, -(isize)victim->bytes);
        victim->bytes   = 0;
        /* Mark this entry as evicted, not failed. Its decode succeeded; budget
         * pressure removed its pixels. A failed entry is sticky and renders
         * the not-found placeholder, while an evicted entry can be loaded
         * again on the next request. */
        victim->evicted = true;
        victim = prev;
    }
}

/* How recently a render pass must have happened for the draw grace to apply.
 * One frame at 60 Hz is 16.7 ms; a tenth of a second is comfortably longer than
 * any gap between passes in a window that is actually painting, and far shorter
 * than any interval that matters to a window that is not. */
#ifndef MD_IMAGE_PASS_FRESH_SEC
#define MD_IMAGE_PASS_FRESH_SEC 0.1
#endif

usize md_image_cache_reclaim(MdImageCache *c) {
    if (!c) return 0;
    usize freed = 0;
    /* Walk the LRU tail while retaining entries drawn within the grace
     * interval; they are visible or likely to be needed by a nearby scroll.
     * Older entries can be evicted and decoded again on the next request. */
    /* Apply draw-epoch grace only while the owner has rendered recently. Hidden
     * tabs and idle windows stop advancing epochs, so under memory pressure
     * undrawn entries can be evicted and decoded again on the next request.
     * Eviction does not mark a decode as failed. */
    bool drawing_now = (platform_time_sec() - c->last_pass_time) < MD_IMAGE_PASS_FRESH_SEC;
    MdImageEntry *victim = c->lru_tail;
    while (victim) {
        MdImageEntry *prev = victim->lru_prev;
        if (drawing_now && victim->epoch + 1 >= c->epoch) {
            victim = prev;
            continue;
        }
        lru_unlink(c, victim);
        if (victim->rgba) { stbi_image_free(victim->rgba); victim->rgba = NULL; }
        c->total_bytes = victim->bytes <= c->total_bytes
                       ? c->total_bytes - victim->bytes : 0;
        if (victim->bytes) {
            mem_budget_adjust(MEM_IMAGE_CPU, -(isize)victim->bytes);
            freed += victim->bytes;
        }
        victim->bytes = 0;
        victim->evicted = true;   /* NOT failed — see md_image_enforce_budget */
        victim = prev;
    }
    return freed;
}

static void md_image_fail(MdImageEntry *e);

static bool md_image_schedule(MdImageCache *c, MdImageEntry *e) {
    if (!c || !e || e->loading || e->failed) return false;
    e->loading = true;
    e->evicted = false;
    /* The caller is drawing this entry now. Stamp before the worker finishes,
     * otherwise a later completion can look stale to the budget sweep and
     * evict a sibling from the same visible pass (async reload treadmill). */
    e->epoch = c->epoch;
    e->load_token = ++g_md_load_token;
    if (!md_image_enqueue(c, e, false)) {
        e->loading = false;
        e->evicted = true; /* retry on a later visible frame */
        return false;
    }
    return true;
}

static void md_image_schedule_header(MdImageCache *c, MdImageEntry *e) {
    if (e->loading || (e->failed && platform_time_sec() < e->fail_next)) return;
    e->load_token = ++g_md_load_token;
    e->loading = true;
    if (!md_image_enqueue(c, e, true)) e->loading = false;
}

static void md_image_adopt_metadata(MdImageEntry *e, const MdImageEntry *m) {
    e->w = m->w; e->h = m->h; e->failed = m->failed; e->evicted = m->evicted;
    e->fail_next = m->fail_next; e->fail_backoff = m->fail_backoff;
    e->fail_stat_ok = m->fail_stat_ok; e->fail_size = m->fail_size;
    e->fail_mtime_sec = m->fail_mtime_sec; e->fail_mtime_nsec = m->fail_mtime_nsec;
}

bool md_image_cache_completion_ready(const MdImageCache *c) {
    const MdImageAsync *a = c ? c->async : NULL;
    return a && atomic_load_explicit(&a->done_ready, memory_order_acquire);
}

bool md_image_cache_pump(MdImageCache *c) {
    MdImageAsync *a = c ? c->async : NULL;
    if (!a || !atomic_load_explicit(&a->done_ready, memory_order_acquire))
        return false;
    MdImageDone local[MD_IMAGE_QUEUE_CAP];
    i32 n = 0;
    pthread_mutex_lock(&a->mu);
    while (a->done_head != a->done_tail && n < MD_IMAGE_QUEUE_CAP) {
        local[n++] = a->done[a->done_head];
        memset(&a->done[a->done_head], 0, sizeof(a->done[a->done_head]));
        a->done_head = mdq_next(a->done_head);
    }
    atomic_store_explicit(&a->done_ready, a->done_head != a->done_tail,
                          memory_order_release);
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);

    bool changed = false;
    for (i32 i = 0; i < n; i++) {
        MdImageDone *d = &local[i];
        if (d->transient_bytes)
            mem_budget_transient_adjust(MEM_IMAGE_CPU,
                                        -(isize)d->transient_bytes);
        MdImageEntry *e = d->entry;
        if (!e || !e->loading || e->load_token != d->token) {
            if (d->rgba) stbi_image_free(d->rgba);
            continue;
        }
        e->loading = false;
        if (d->header_only) {
            bool layout_changed = e->w != d->metadata.w || e->h != d->metadata.h ||
                                  e->failed != d->metadata.failed;
            md_image_adopt_metadata(e, &d->metadata);
            if (layout_changed) { c->layout_revision++; changed = true; }
            continue;
        }
        if (!d->ok || !d->rgba) {
            if (d->rgba) stbi_image_free(d->rgba);
            if (d->failure_recorded) md_image_adopt_metadata(e, &d->metadata);
            else md_image_fail(e);
            c->layout_revision++;
            changed = true;
            continue;
        }
        usize total = 0;
        if (!checked_add_usize(c->total_bytes, d->bytes, &total)) {
            stbi_image_free(d->rgba);
            md_image_fail(e);
            changed = true;
            continue;
        }
        e->rgba = d->rgba;
        e->generation = ++g_md_image_serial;
        e->w = d->w; e->h = d->h; e->bytes = d->bytes;
        c->layout_revision++;
        c->total_bytes = total;
        mem_budget_adjust(MEM_IMAGE_CPU, (isize)d->bytes);
        lru_push_head(c, e);
        md_image_enforce_budget(c, e);
        changed = true;
    }
    return changed;
}

void md_image_cache_begin_pass(MdImageCache *c) {
    if (!c) return;
    (void)md_image_cache_pump(c);
    c->last_pass_time = platform_time_sec();
    c->epoch++;
    /* Entries are calloc'd to epoch 0 and the counter starts at 1, so a
     * never-drawn entry is never mistaken for a pinned one. */

    /* Reclaim any overshoot the previous pass was allowed to run up. Without a
     * sweep here the budget is only ever checked while loading, so a document
     * whose visible set went over budget and then stopped loading anything —
     * the user scrolls to a text-only region, or simply stops — would hold the
     * excess until the viewer closed. The grace period inside the sweep is what
     * keeps this from evicting the set that is still on screen. */
    md_image_enforce_budget(c, NULL);
}

/* Record a failure with the identity of the file that produced it, and
 * schedule the first retry. Called for a header probe that did not parse, a
 * file that was not there and a decode the worker gave up on. */
static void md_image_fail_from(MdImageEntry *e, const struct stat *st, bool exists) {
    e->failed  = true;
    e->evicted = false;
    e->loading = false;
    e->fail_stat_ok = exists;
    if (e->fail_stat_ok) {
        e->fail_size = (i64)st->st_size;
        e->fail_mtime_sec = (i64)st->st_mtime;
#if defined(__APPLE__)
        e->fail_mtime_nsec = (i64)st->st_mtimespec.tv_nsec;
#else
        e->fail_mtime_nsec = (i64)st->st_mtim.tv_nsec;
#endif
    } else {
        e->fail_size = e->fail_mtime_sec = e->fail_mtime_nsec = 0;
    }
    f64 b = e->fail_backoff * 2.0;
    if (b < MD_IMAGE_RETRY_MIN_SEC) b = MD_IMAGE_RETRY_MIN_SEC;
    if (b > MD_IMAGE_RETRY_MAX_SEC) b = MD_IMAGE_RETRY_MAX_SEC;
    e->fail_backoff = b;
    e->fail_next = platform_time_sec() + b;
}
static void md_image_fail(MdImageEntry *e) {
    struct stat st;
    bool exists=stat(e->abs_path,&st)==0;
    md_image_fail_from(e,&st,exists);
}

/* Is a failed entry due for another look, and did the file change since it
 * failed? One stat() per backoff interval, never per frame. An unchanged file
 * only pushes the next look further out; a changed (or newly present) one
 * clears the failure so the caller re-probes the header. */
static bool md_image_fail_retry_due(MdImageEntry *e) {
    if (!e->failed) return false;
    f64 now = platform_time_sec();
    if (now < e->fail_next) return false;
    struct stat st;
    bool exists = stat(e->abs_path, &st) == 0;
    bool same;
    if (!exists) same = !e->fail_stat_ok;
    else {
        i64 nsec;
#if defined(__APPLE__)
        nsec = (i64)st.st_mtimespec.tv_nsec;
#else
        nsec = (i64)st.st_mtim.tv_nsec;
#endif
        same = e->fail_stat_ok && e->fail_size == (i64)st.st_size &&
               e->fail_mtime_sec == (i64)st.st_mtime && e->fail_mtime_nsec == nsec;
    }
    if (same) {
        f64 b = e->fail_backoff * 2.0;
        if (b > MD_IMAGE_RETRY_MAX_SEC) b = MD_IMAGE_RETRY_MAX_SEC;
        e->fail_backoff = b;
        e->fail_next = now + b;
        return false;
    }
    e->failed = false;
    e->w = e->h = 0;
    return true;
}

/* Header metadata for a fresh (or retried) entry: the size the LOADED entry
 * will have, so layout gets a stable box, with the pixels left to the worker.
 * Returns false — and records the failure — for a missing, unreadable or
 * oversized file. */
static bool md_image_probe_header(MdImageEntry *e) {
    int iw = 0, ih = 0, ich = 0;
    usize src_pixels = 0;
    /* A writer can finish/replace the file while the read fails. Remember
     * the identity before reading, so that new file is retried rather than
     * permanently associated with the previous failed read. */
    struct stat before;
    bool existed=stat(e->abs_path,&before)==0;
    if (!MD_IMAGE_FILE_INFO(e->abs_path, &iw, &ih, &ich) || iw <= 0 || ih <= 0 ||
        !checked_mul_usize((usize)iw, (usize)ih, &src_pixels) ||
        src_pixels > MD_IMAGE_MAX_SOURCE_PIXELS) {
        md_image_fail_from(e,&before,existed);
        return false;
    }
    /* Report the size the loaded entry will have, not the file's native size —
     * the load path shrinks anything past MD_IMAGE_MAX_DIM, and measuring the
     * pre-shrink size would lay the document out to a box the drawn image never
     * fills. */
    img_shrink_dims(&iw, &ih, MD_IMAGE_MAX_DIM);
    e->w = iw;
    e->h = ih;
    /* Metadata only so far: no pixels have ever been loaded. Flagging it
     * `evicted` routes the next md_image_cache_get through the same reload path
     * a budget-evicted entry takes. */
    e->evicted = true;
    return true;
}

/* Append a bucket entry for `abs_path` in bucket `bi`. Returns NULL if the
 * metadata cap is hit or allocation fails. */
static MdImageEntry *md_image_entry_new(MdImageCache *c, u32 bi,
                                        const char *abs_path) {
    if (c->entry_count >= MD_IMAGE_CACHE_MAX_ENTRIES) return NULL;
    MdImageEntry *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->abs_path = strdup(abs_path);
    if (!e->abs_path) { free(e); return NULL; }
    e->next = c->buckets[bi];
    c->buckets[bi] = e;
    c->entry_count++;
    return e;
}

bool md_image_cache_dims(MdImageCache *c, const char *base_dir,
                         const char *ref, u32 ref_len, i32 *w, i32 *h) {
    if (!c || !w || !h) return false;
    char abs_path[2048];
    if (!resolve_path(base_dir, ref, ref_len, abs_path, sizeof abs_path) ||
        abs_path[0] == '\0') return false;

    u32 hh = fnv1a(abs_path);
    u32 bi = hh % (u32)(sizeof c->buckets / sizeof c->buckets[0]);
    for (MdImageEntry *e = c->buckets[bi]; e; e = e->next) {
        if (strcmp(e->abs_path, abs_path) == 0) {
            /* No LRU bump on purpose — see the header comment. w/h survive an
             * eviction, so an evicted entry still measures correctly. */
            if (c->async_headers && (e->failed || e->w <= 0 || e->h <= 0))
                md_image_schedule_header(c, e);
            else if (e->failed && md_image_fail_retry_due(e))
                (void)md_image_probe_header(e);
            if (e->failed || e->w <= 0 || e->h <= 0) return false;
            *w = e->w;
            *h = e->h;
            return true;
        }
    }

    /* The entry FIRST, the disk second. With the metadata cap reached the
     * probe's answer would have nowhere to be memoised, and a path past
     * the 2048th distinct image would be header-read on every measure
     * pass, so no entry means no probe. */
    MdImageEntry *e = md_image_entry_new(c, bi, abs_path);
    if (!e) return false;
    if (c->async_headers) { md_image_schedule_header(c, e); return false; }
    if (!md_image_probe_header(e)) return false;
    *w = e->w;
    *h = e->h;
    return true;
}

const MdImageEntry *md_image_cache_peek(const MdImageCache *c, const char *base_dir,
                                      const char *ref, u32 ref_len) {
    if (!c) return NULL;
    char path[2048];
    if (!resolve_path(base_dir, ref, ref_len, path, sizeof path) || !path[0]) return NULL;
    u32 bi=fnv1a(path) % (u32)(sizeof c->buckets/sizeof c->buckets[0]);
    for (const MdImageEntry *e=c->buckets[bi];e;e=e->next)
        if (!strcmp(e->abs_path,path)) return e;
    return NULL;
}

const MdImageEntry *md_image_cache_get(MdImageCache *c, const char *base_dir,
                                       const char *ref, u32 ref_len) {
    if (!c) return NULL;
    char abs_path[2048];
    if (!resolve_path(base_dir, ref, ref_len, abs_path, sizeof abs_path) ||
        abs_path[0] == '\0') return NULL;

    u32 h = fnv1a(abs_path);
    u32 bi = h % (u32)(sizeof c->buckets / sizeof c->buckets[0]);
    for (MdImageEntry *e = c->buckets[bi]; e; e = e->next) {
        if (strcmp(e->abs_path, abs_path) == 0) {
            /* A visible request is a draw even while its header or pixels are
             * in flight. Otherwise a slow decode arrives with the epoch of
             * its first request and can immediately look off screen. */
            e->epoch = c->epoch;
            if (e->rgba) {
                /* Live: bump to LRU head and pin for this pass. */
                lru_unlink(c, e);
                lru_push_head(c, e);
            } else if (c->async_headers && (e->failed || e->w <= 0 || e->h <= 0)) {
                md_image_schedule_header(c, e);
            } else if (e->evicted && !e->failed) {
                /* Budget-evicted but perfectly loadable — queue it once. */
                (void)md_image_schedule(c, e);
            } else if (e->failed && md_image_fail_retry_due(e) &&
                       md_image_probe_header(e)) {
                /* The file changed (or appeared) since it failed: back on
                 * the ordinary load path. An unchanged one stays `failed`
                 * on its backoff, so a broken file costs one stat() per
                 * interval, never a decode per frame. */
                (void)md_image_schedule(c, e);
            }
            return e;
        }
    }

    /* Miss: load. Refuse new entries once the metadata cap is hit;
     * caller sees NULL and renders a placeholder. Without this, an
     * auto-generated .md with thousands of distinct image refs
     * grows bucket-chain metadata unboundedly within one viewer
     * session — pixel budget alone won't catch it because evicted
     * entries keep their struct + abs_path in the bucket.
     *
     * The entry is linked into its bucket before loading: md_image_load runs
     * the budget sweep, which walks the LRU list, and the entry has to be
     * reachable for a later get() whichever way the load goes. */
    MdImageEntry *e = md_image_entry_new(c, bi, abs_path);
    if (!e) return NULL;
    e->epoch = c->epoch;
    if (c->async_headers) { md_image_schedule_header(c, e); return e; }

    /* Header metadata gives layout a stable box immediately; pixel decode is
     * queued and the renderer shows its existing placeholder for this frame. */
    if (!md_image_probe_header(e)) return e;
    (void)md_image_schedule(c, e);
    return e;
}
