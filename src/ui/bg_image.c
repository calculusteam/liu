/*
 * Liu — process-wide background-image decode. See bg_image.h.
 */
#include "ui/bg_image.h"

#include "core/imgproc.h"
#include "core/mem_budget.h"
#include "platform/platform.h"

#include "stb_image.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <time.h>

/* Same ceilings the per-window decoder carried. 16384 is the largest
 * dimension any GPU here will take as a single texture; 128 MiB of RGBA is
 * roughly a 5800x5800 image and well past any real wallpaper. */
#define BG_IMAGE_MAX_DIM   16384
#define BG_IMAGE_MAX_BYTES ((usize)128 * 1024 * 1024)

/* One per distinct wallpaper being decoded at once. Windows cap at 8 and
 * they nearly always want the same file, so this is only ever exercised
 * during a change-over, when the old job is still held by a window that has
 * not repainted yet. */
#define BG_IMAGE_MAX_JOBS 8

struct BgImageJob {
    char         path[1024];
    FileStamp    source;

    /* Read by the decode without the lock, between its stages. */
    _Atomic bool cancel;

    /* Everything below is guarded by g_mu. */
    bool         in_use;
    bool         done;
    bool         ok;
    bool         stale;
    i32          refs;          /* subscribers, plus 1 for a running worker */
    u8          *rgba;
    i32          w, h;
    usize        bytes;         /* accounted in MEM_IMAGE_CPU while non-zero */
    f64          start_sec;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static BgImageJob      g_jobs[BG_IMAGE_MAX_JOBS];
static _Atomic u32     g_decode_count;

/* The `static i32 v = -1` cached-getenv idiom used elsewhere in this tree is
 * main-thread-only by construction, and the decode publishes from a worker.
 * pthread_once makes the same caching safe for both. */
static pthread_once_t g_probe_once = PTHREAD_ONCE_INIT;
static bool           g_probe_on;
static void probe_resolve(void) {
    const char *s = liu_dev_env("LIU_WALLPAPER_PROBE");
    g_probe_on = (s && *s && *s != '0');
}
static bool probe_on(void) {
    pthread_once(&g_probe_once, probe_resolve);
    return g_probe_on;
}

/* Frees the pixels and the slot. Caller holds g_mu. */
static void bg_job_reclaim_locked(BgImageJob *j) {
    if (j->bytes) {
        /* transient_adjust, not adjust: the ledger's plain counters are
         * main-thread-only and this runs on the decode worker too. */
        mem_budget_transient_adjust(MEM_IMAGE_CPU, -(isize)j->bytes);
        j->bytes = 0;
    }
    if (j->rgba) { stbi_image_free(j->rgba); j->rgba = NULL; }
    memset(j, 0, sizeof(*j));   /* clears in_use */
}

static void *bg_image_worker(void *arg) {
    BgImageJob *j = (BgImageJob *)arg;
#ifdef __APPLE__
    pthread_setname_np("liu-wallpaper");
#else
    pthread_setname_np(pthread_self(), "liu-wallpaper");
#endif

    /* The path is immutable for the job's life, so it is read without the
     * lock; nothing else here is. */
    u8 *px = NULL;
    i32 w = 0, h = 0;
    usize bytes = 0, accounted = 0;
    bool stale = false;
    int iw = 0, ih = 0, ch = 0;

    /* stb_image cannot be interrupted, so cancellation is checked at the
     * stage boundaries around it. Skipping the load outright is the case
     * that matters: a window closing before its decode started pays nothing.
     */
    if (atomic_load_explicit(&j->cancel, memory_order_acquire)) goto publish;
    if (!stbi_info(j->path, &iw, &ih, &ch) ||
        iw <= 0 || ih <= 0 || iw > BG_IMAGE_MAX_DIM || ih > BG_IMAGE_MAX_DIM ||
        !img_rgba_bytes_checked(iw, ih, &bytes) || bytes > BG_IMAGE_MAX_BYTES)
        goto publish;
    if (atomic_load_explicit(&j->cancel, memory_order_acquire)) goto publish;

    /* On the ledger BEFORE the allocation exists, so memory pressure sees
     * the decode while it runs — up to BG_IMAGE_MAX_BYTES of it, which for a
     * wallpaper is the entire cost. `accounted` is reconciled to the real
     * figure at publish, so a decode that fails or is cancelled takes its
     * estimate back off. */
    accounted = bytes;
    mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)accounted);

    px = stbi_load(j->path, &iw, &ih, &ch, 4);
    if (!px || iw <= 0 || ih <= 0 || iw > BG_IMAGE_MAX_DIM || ih > BG_IMAGE_MAX_DIM ||
        !img_rgba_bytes_checked(iw, ih, &bytes) || bytes > BG_IMAGE_MAX_BYTES) {
        if (px) { stbi_image_free(px); px = NULL; }
        goto publish;
    }
    if (!file_stamp_matches(j->path, &j->source)) {
        stbi_image_free(px); px = NULL; stale = true;
        goto publish;
    }
    w = iw; h = ih;

publish: {
    pthread_mutex_lock(&g_mu);
    j->stale = stale;
    if (px) {
        j->rgba = px; j->w = w; j->h = h; j->bytes = bytes; j->ok = true;
    }
    /* Reconcile the estimate to what actually survived: the real bytes on
     * success, nothing on failure or cancellation. One adjustment covers
     * every exit, so no path can leave the estimate stranded. */
    {
        usize final_bytes = px ? bytes : 0;
        if (final_bytes != accounted)
            mem_budget_transient_adjust(MEM_IMAGE_CPU,
                                        (isize)final_bytes - (isize)accounted);
    }
    j->done = true;
    if (probe_on()) {
        fprintf(stderr, "[wallpaper] decode %s %s %dx%d %.2f MiB in %.1f ms"
                        " (subscribers %d, decode #%u)\n",
                j->ok ? "ok  " : "fail", j->path, j->w, j->h,
                (f64)j->bytes / (1024.0 * 1024.0),
                (platform_time_sec() - j->start_sec) * 1000.0,
                j->refs - 1, atomic_load(&g_decode_count));
    }
    bool abandoned = (--j->refs == 0);
    if (abandoned) bg_job_reclaim_locked(j);
    pthread_mutex_unlock(&g_mu);

    /* Only worth a wake-up if a window is still waiting to install it. */
    if (!abandoned) platform_wake_event_loop();
}
    return NULL;
}

BgImageJob *bg_image_request(const char *path) {
    if (!path || !path[0]) return NULL;
    usize len = strlen(path);
    if (len >= sizeof(((BgImageJob *)0)->path)) return NULL;

    FileStamp source;
    if (!file_stamp_read(path, &source)) return NULL;

    pthread_mutex_lock(&g_mu);

    /* Join an existing job for the same file identity — in flight or already
     * decoded, both save the work. */
    for (i32 i = 0; i < BG_IMAGE_MAX_JOBS; i++) {
        BgImageJob *j = &g_jobs[i];
        if (!j->in_use) continue;
        if (!file_stamp_equal(&j->source, &source) ||
            strcmp(j->path, path) != 0) continue;
        /* A job whose last subscriber left is on its way out; do not revive
         * it, because its cancel flag is already set. */
        if (atomic_load_explicit(&j->cancel, memory_order_acquire)) continue;
        j->refs++;
        pthread_mutex_unlock(&g_mu);
        return j;
    }

    BgImageJob *j = NULL;
    for (i32 i = 0; i < BG_IMAGE_MAX_JOBS; i++)
        if (!g_jobs[i].in_use) { j = &g_jobs[i]; break; }
    if (!j) { pthread_mutex_unlock(&g_mu); return NULL; }

    memset(j, 0, sizeof(*j));
    memcpy(j->path, path, len + 1);
    j->source     = source;
    j->in_use     = true;
    j->refs       = 2;                  /* this subscriber, and the worker */
    j->start_sec  = platform_time_sec();
    atomic_init(&j->cancel, false);
    atomic_fetch_add(&g_decode_count, 1);

    pthread_t th;
    if (pthread_create(&th, NULL, bg_image_worker, j) != 0) {
        bg_job_reclaim_locked(j);
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }
    /* Nobody ever joins it: the job outlives its subscribers if it has to. */
    pthread_detach(th);
    pthread_mutex_unlock(&g_mu);
    return j;
}

bool bg_image_done(const BgImageJob *j) {
    if (!j) return false;
    pthread_mutex_lock(&g_mu);
    bool d = j->done;
    pthread_mutex_unlock(&g_mu);
    return d;
}

const u8 *bg_image_pixels(const BgImageJob *j, i32 *w, i32 *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!j) return NULL;
    pthread_mutex_lock(&g_mu);
    const u8 *px = (j->done && j->ok) ? j->rgba : NULL;
    if (px) { if (w) *w = j->w; if (h) *h = j->h; }
    pthread_mutex_unlock(&g_mu);
    return px;
}

bool bg_image_source_stamp(const BgImageJob *j, FileStamp *out) {
    if (!j || !out) return false;
    pthread_mutex_lock(&g_mu);
    bool valid = j->in_use;
    if (valid) *out = j->source;
    pthread_mutex_unlock(&g_mu);
    return valid;
}

bool bg_image_stale(const BgImageJob *j) {
    if (!j) return false;
    pthread_mutex_lock(&g_mu);
    bool stale = j->stale;
    FileStamp source = j->source;
    pthread_mutex_unlock(&g_mu);
    /* path/stamp are immutable while this subscriber owns its reference. */
    return stale || !file_stamp_matches(j->path, &source);
}

void bg_image_release(BgImageJob *j) {
    if (!j) return;
    pthread_mutex_lock(&g_mu);
    if (j->refs > 0) j->refs--;
    /* No subscribers remain. If the worker is still running, it holds the
     * remaining reference and reclaims the slot when it finishes;
     * cancellation shortens that period as far as an uninterruptible decode
     * allows. */
    if (j->refs == 1 && !j->done)
        atomic_store_explicit(&j->cancel, true, memory_order_release);
    if (j->refs == 0) bg_job_reclaim_locked(j);
    pthread_mutex_unlock(&g_mu);
}

u32 bg_image_decode_count(void) {
    return atomic_load(&g_decode_count);
}

i32 bg_image_subscriber_count(void) {
    pthread_mutex_lock(&g_mu);
    i32 n = 0;
    for (i32 i = 0; i < BG_IMAGE_MAX_JOBS; i++)
        if (g_jobs[i].in_use) n += g_jobs[i].refs - (g_jobs[i].done ? 0 : 1);
    pthread_mutex_unlock(&g_mu);
    return n;
}
