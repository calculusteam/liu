/* Liu — prompt-attachment thumbnails. Cache publication is main-thread-only;
 * one bounded serial worker owns ALL filesystem access, including TTL checks.
 * Refreshes retain the last image until a generation-guarded result arrives. */
#include "ui/prompt_thumb.h"
#include "terminal/prompt_capture.h"
#include "core/image_work.h"
#include "platform/platform.h"

#include <pthread.h>
#include <stdatomic.h>
#include "stb_image.h"

#define PT_MAX_SOURCE_PIXELS ((usize)8 * 1024 * 1024)
#define PT_MAX_INPUT_BYTES ((usize)128 * 1024 * 1024)
#define PT_STAT_TTL_SEC 2.0
#define PT_IDLE_SEC 30.0
#define PT_QUEUE_CAP (PROMPT_THUMB_SLOTS * 2)

typedef struct {
    char path[PROMPT_RECALL_IMG_PATH];
    ImgFileId id;
    bool id_known;
    f64 stat_at, used_at;
    u64 used, token;
    bool live, loading;
    PromptThumb pub;
} PtSlot;

typedef struct {
    char path[PROMPT_RECALL_IMG_PATH];
    ImgFileId id;
    bool id_known;
    i32 slot;
    u64 token;
} PtJob;

typedef struct {
    u8 *rgba;
    i32 w, h, slot;
    u64 token;
    usize transient_bytes;
    ImgFileId id;
    bool id_known, unchanged, ok;
} PtDone;

static PtSlot g_slots[PROMPT_THUMB_SLOTS];
static u64 g_tick, g_serial, g_token;
static f64 g_last_trim;
static pthread_t g_worker;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static bool g_worker_started, g_worker_stop;
static _Atomic bool g_cancel;
static PtJob g_jobs[PT_QUEUE_CAP];
static u32 g_job_head, g_job_tail;
static PtDone g_done[PT_QUEUE_CAP];
static u32 g_done_head, g_done_tail;
static _Atomic bool g_done_ready;

static inline u32 pt_ring_next(u32 i) { return (i + 1u) % PT_QUEUE_CAP; }

static void pt_pixels_free(PtSlot *s) {
    usize bytes = 0;
    if (s->pub.rgba) {
        (void)img_rgba_bytes_checked(s->pub.w, s->pub.h, &bytes);
        stbi_image_free((void *)s->pub.rgba);
        mem_budget_adjust(MEM_IMAGE_CPU, -(isize)bytes);
    }
    memset(&s->pub, 0, sizeof s->pub);
}

static void pt_slot_free(PtSlot *s) {
    pt_pixels_free(s);
    memset(s, 0, sizeof *s);
    s->token = ++g_token;
}

static bool pt_cancelled(void *unused) {
    (void)unused;
    return atomic_load_explicit(&g_cancel, memory_order_acquire);
}

static void pt_done_free(PtDone *d) {
    if (d->rgba) stbi_image_free(d->rgba);
    if (d->transient_bytes)
        mem_budget_transient_adjust(MEM_IMAGE_CPU, -(isize)d->transient_bytes);
    d->rgba = NULL;
    d->transient_bytes = 0;
}

static PtDone pt_decode(const PtJob *j) {
    PtDone d = { .slot = j->slot, .token = j->token };
    ImgWork work = {0};
    ImgInput input = {0};
    const ImgLimits limits = { 0, PT_MAX_SOURCE_PIXELS, PT_MAX_SOURCE_PIXELS * 4u };
    u8 *px = NULL;
    int w = 0, h = 0, comp = 0;
    usize bytes = 0;
    if (pt_cancelled(NULL)) return d;
    d.id = img_file_stat(j->path);
    d.id_known = true;
    if (j->id_known && img_file_same(j->id, d.id)) {
        d.unchanged = true;
        return d;
    }
    if (!d.id.valid) return d;
    if (!img_input_read(j->path, PT_MAX_INPUT_BYTES, &work,
                        pt_cancelled, NULL, &input)) {
        d.id_known = false;       /* retry transient read failures */
        goto done;
    }
    d.id = input.id;
    if (!stbi_info_from_memory(input.data, (int)input.len, &w, &h, &comp) ||
        !img_limits_ok(w, h, &limits) ||
        !img_rgba_bytes_checked(w, h, &bytes)) goto done;
    int header_w = w, header_h = h;
    if (pt_cancelled(NULL) || !img_input_current(j->path, &input)) {
        d.id_known = false;
        goto done;
    }
    if (!img_work_add(&work, bytes)) goto done;
    px = stbi_load_from_memory(input.data, (int)input.len, &w, &h, &comp, 4);
    if (!px || !img_limits_ok(w, h, &limits) || w != header_w || h != header_h)
        goto done;
    if (pt_cancelled(NULL) || !img_input_current(j->path, &input)) {
        d.id_known = false;
        goto done;
    }
    img_input_free(&input, &work);
    img_premultiply_rgba(px, w, h);
    if (pt_cancelled(NULL) ||
        !img_work_shrink(&work, &px, &w, &h, PROMPT_THUMB_MAX_DIM)) goto done;
    if (pt_cancelled(NULL) || !img_input_current(j->path, &input)) {
        d.id_known = false;
        goto done;
    }
    d.rgba = px;
    px = NULL;
    d.w = w;
    d.h = h;
    d.ok = true;
done:
    if (px) stbi_image_free(px);
    img_input_free(&input, &work);
    if (!d.rgba) (void)img_work_set(&work, 0);
    d.transient_bytes = work.bytes; /* retained result, never source estimate */
    return d;
}

static void *pt_worker_main(void *unused) {
    (void)unused;
#ifdef __APPLE__
    pthread_setname_np("liu-prompt-thumb");
#else
    pthread_setname_np(pthread_self(), "liu-prompt-thumb");
#endif
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_worker_stop && g_job_head == g_job_tail)
            pthread_cond_wait(&g_cv, &g_mu);
        if (g_worker_stop) { pthread_mutex_unlock(&g_mu); break; }
        PtJob job = g_jobs[g_job_head];
        g_job_head = pt_ring_next(g_job_head);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);

        PtDone done = pt_decode(&job);
        pthread_mutex_lock(&g_mu);
        while (!g_worker_stop && pt_ring_next(g_done_tail) == g_done_head)
            pthread_cond_wait(&g_cv, &g_mu);
        if (g_worker_stop) {
            pthread_mutex_unlock(&g_mu);
            pt_done_free(&done);
            break;
        }
        g_done[g_done_tail] = done;
        g_done_tail = pt_ring_next(g_done_tail);
        atomic_store_explicit(&g_done_ready, true, memory_order_release);
        pthread_mutex_unlock(&g_mu);
        platform_wake_event_loop();
    }
    return NULL;
}

static bool pt_worker_start(void) {
    if (g_worker_started) return true;
    g_worker_stop = false;
    atomic_store_explicit(&g_cancel, false, memory_order_release);
    if (pthread_create(&g_worker, NULL, pt_worker_main, NULL) != 0) return false;
    g_worker_started = true;
    return true;
}

static bool pt_enqueue(i32 slot) {
    if (!pt_worker_start()) return false;
    PtSlot *s = &g_slots[slot];
    pthread_mutex_lock(&g_mu);
    u32 next = pt_ring_next(g_job_tail);
    if (next == g_job_head) { pthread_mutex_unlock(&g_mu); return false; }
    PtJob *j = &g_jobs[g_job_tail];
    memset(j, 0, sizeof *j);
    snprintf(j->path, sizeof j->path, "%s", s->path);
    j->slot = slot;
    j->token = s->token;
    j->id = s->id;
    j->id_known = s->id_known;
    g_job_tail = next;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return true;
}

bool prompt_thumb_pump(void) {
    if (!atomic_load_explicit(&g_done_ready, memory_order_acquire)) return false;
    PtDone local[PT_QUEUE_CAP];
    i32 n = 0;
    pthread_mutex_lock(&g_mu);
    while (g_done_head != g_done_tail && n < PT_QUEUE_CAP) {
        local[n++] = g_done[g_done_head];
        memset(&g_done[g_done_head], 0, sizeof g_done[g_done_head]);
        g_done_head = pt_ring_next(g_done_head);
    }
    atomic_store_explicit(&g_done_ready, g_done_head != g_done_tail, memory_order_release);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);

    bool changed = false;
    for (i32 i = 0; i < n; i++) {
        PtDone *d = &local[i];
        PtSlot *s = d->slot >= 0 && d->slot < PROMPT_THUMB_SLOTS
                  ? &g_slots[d->slot] : NULL;
        if (!s || !s->live || !s->loading || s->token != d->token) {
            pt_done_free(d);
            continue;
        }
        s->loading = false;
        s->stat_at = platform_time_sec();
        s->id = d->id;
        s->id_known = d->id_known;
        if (d->unchanged) { pt_done_free(d); continue; }
        pt_pixels_free(s);
        s->pub.failed = !d->ok;
        s->pub.generation = ++g_serial;
        if (d->ok && d->rgba) {
            /* A paired main-thread ownership transfer: no intervening main-
             * thread ledger sampling, and no double-counted peak. */
            mem_budget_transient_adjust(MEM_IMAGE_CPU, -(isize)d->transient_bytes);
            mem_budget_adjust(MEM_IMAGE_CPU, (isize)d->transient_bytes);
            d->transient_bytes = 0;
            s->pub.rgba = d->rgba;
            d->rgba = NULL;
            s->pub.w = d->w;
            s->pub.h = d->h;
        }
        pt_done_free(d);
        changed = true;
    }
    return changed;
}

bool prompt_thumb_completion_ready(void) {
    return atomic_load_explicit(&g_done_ready, memory_order_acquire);
}

u64 prompt_thumb_revision(void) { return g_serial; }

void prompt_thumb_trim_idle(f64 now_sec) {
    if (now_sec <= 0.0 || now_sec - g_last_trim < 1.0) return;
    g_last_trim = now_sec;
    (void)prompt_thumb_pump();
    for (i32 i = 0; i < PROMPT_THUMB_SLOTS; i++) {
        PtSlot *s = &g_slots[i];
        if (s->live && !s->loading && s->used_at > 0.0 &&
            now_sec - s->used_at > PT_IDLE_SEC) pt_slot_free(s);
    }
}

void prompt_thumb_shutdown(void) {
    if (g_worker_started) {
        atomic_store_explicit(&g_cancel, true, memory_order_release);
        pthread_mutex_lock(&g_mu);
        g_worker_stop = true;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        pthread_join(g_worker, NULL);
        g_worker_started = false;
    }
    pthread_mutex_lock(&g_mu);
    while (g_done_head != g_done_tail) {
        pt_done_free(&g_done[g_done_head]);
        memset(&g_done[g_done_head], 0, sizeof g_done[g_done_head]);
        g_done_head = pt_ring_next(g_done_head);
    }
    g_job_head = g_job_tail = g_done_head = g_done_tail = 0;
    g_worker_stop = false;
    atomic_store_explicit(&g_done_ready, false, memory_order_relaxed);
    pthread_mutex_unlock(&g_mu);
    for (i32 i = 0; i < PROMPT_THUMB_SLOTS; i++) pt_slot_free(&g_slots[i]);
    g_tick = 0;
    /* Keep texture generations monotone even across worker restarts. */
    g_last_trim = 0.0;
}

static void pt_request(i32 slot, f64 now) {
    PtSlot *s = &g_slots[slot];
    s->token = ++g_token;
    s->loading = true;
    s->stat_at = now;
    if (!pt_enqueue(slot)) {
        s->loading = false;
        /* Backpressure is not a negative file cache entry. Retry on next use. */
        s->stat_at = now - PT_STAT_TTL_SEC;
    }
}

const PromptThumb *prompt_thumb_get(const char *path) {
    static const PromptThumb none = { NULL, 0, 0, 0, true };
    (void)prompt_thumb_pump();
    if (!path || !path[0] || strlen(path) + 1 > sizeof g_slots[0].path) return &none;
    f64 now = platform_time_sec();
    i32 victim = -1;
    for (i32 i = 0; i < PROMPT_THUMB_SLOTS; i++) {
        PtSlot *s = &g_slots[i];
        if (!s->live) { if (victim < 0) victim = i; continue; }
        if (strcmp(s->path, path) != 0) {
            if (!s->loading && (victim < 0 ||
                (g_slots[victim].live && s->used < g_slots[victim].used))) victim = i;
            continue;
        }
        s->used = ++g_tick;
        s->used_at = now;
        if (!s->loading && now - s->stat_at >= PT_STAT_TTL_SEC) pt_request(i, now);
        return &s->pub;
    }
    if (victim < 0) return &none;
    PtSlot *s = &g_slots[victim];
    pt_slot_free(s);
    snprintf(s->path, sizeof s->path, "%s", path);
    s->live = true;
    s->used = ++g_tick;
    s->used_at = now;
    pt_request(victim, now);
    return &s->pub;
}
