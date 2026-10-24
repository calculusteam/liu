#include "ui/sleep_io.h"
#include "ui/sleep_snap.h"
#include "core/mem_budget.h"
#include "core/checked.h"
#include "core/config.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLEEP_IO_PATH_MAX 4096u

struct SleepIOJob {
    pthread_mutex_t mutex;
    pthread_cond_t collected;
    atomic_int refs;
    atomic_bool ready, cancel;
    bool taken, abandoned;
    SleepIOKind kind;
    i32 count;
    char *paths[SLEEP_IO_MAX_PANES];
    SleepIOPane specs[SLEEP_IO_MAX_PANES];
    Terminal *terms[SLEEP_IO_MAX_PANES];
    SleepIOResult result;
    usize grid_charge, sb_charge;
    usize reserved;
    SleepIOWakeFn wake;
};

static pthread_mutex_t g_admission = PTHREAD_MUTEX_INITIALIZER;
static i32 g_jobs;
static usize g_reserved;
static _Atomic(SleepIOWakeFn) g_wake;

void sleep_io_set_wake(SleepIOWakeFn wake) { atomic_store_explicit(&g_wake, wake, memory_order_release); }
void sleep_io_stats(i32 *jobs, usize *bytes) {
    pthread_mutex_lock(&g_admission);
    if (jobs) *jobs = g_jobs;
    if (bytes) *bytes = g_reserved;
    pthread_mutex_unlock(&g_admission);
}

static void sleep_job_free(SleepIOJob *job) {
    for (i32 p = 0; p < job->count; p++) free(job->paths[p]);
    pthread_cond_destroy(&job->collected);
    pthread_mutex_destroy(&job->mutex);
    free(job);
}
static void sleep_job_drop(SleepIOJob *job) {
    if (atomic_fetch_sub_explicit(&job->refs, 1, memory_order_acq_rel) == 1) sleep_job_free(job);
}

/* Terminal owns no UI callback while a worker owns it. Its paste-log
 * destructor is an ownership destructor rather than a UI callback. */
static void sleep_detach_callbacks(Terminal *t) {
    if (!t) return;
    t->on_bell = NULL; t->on_title = NULL; t->on_cwd = NULL;
    t->on_response = NULL; t->on_clipboard_set = NULL; t->on_clipboard_get = NULL;
    t->on_long_command = NULL; t->on_command_prompt = NULL; t->on_command_finished = NULL;
    t->userdata = NULL; t->long_cmd_userdata = NULL; t->command_userdata = NULL;
}

/* Updates occur on the worker until publication. The waiter owns no Terminal
 * pointer until take(), and a published worker sleeps until that handoff or
 * abandonment, so even an uncollected heavy result is destroyed off-thread. */
static void sleep_account(void *context, const Terminal *unused, usize staging) {
    (void)unused;
    SleepIOJob *job = context;
    usize grid = staging, sb = 0;
    for (i32 p = 0; p < job->count; p++) {
        grid += terminal_grid_bytes(job->terms[p]);
        sb += terminal_scrollback_bytes(job->terms[p]);
    }
    mem_budget_transient_adjust(MEM_TERMINAL, (isize)grid - (isize)job->grid_charge);
    mem_budget_transient_adjust(MEM_SCROLLBACK, (isize)sb - (isize)job->sb_charge);
    job->grid_charge = grid; job->sb_charge = sb;
    usize actual = grid + sb;
    if (actual > job->reserved) {
        pthread_mutex_lock(&g_admission);
        g_reserved += actual - job->reserved;
        job->reserved = actual;
        pthread_mutex_unlock(&g_admission);
    }
}

static void sleep_dispose_terms(SleepIOJob *job) {
    for (i32 p = 0; p < job->count; p++) {
        if (job->terms[p]) terminal_destroy(job->terms[p]);
        job->terms[p] = NULL;
    }
    sleep_account(job, NULL, 0);
}
static void sleep_remove_paths(SleepIOJob *job) {
    for (i32 p = 0; p < job->count; p++) if (job->paths[p] && job->paths[p][0]) unlink(job->paths[p]);
}

static void *sleep_worker(void *argument) {
    SleepIOJob *job = argument;
#ifdef __APPLE__
    pthread_setname_np("liu-snapshot-io");
#endif
    /* The starter keeps this locked until ownership and charges have moved. */
    pthread_mutex_lock(&job->mutex);
    pthread_mutex_unlock(&job->mutex);
    SleepSnapControl control = { .cancel = &job->cancel, .preserve_file = true, .progress = sleep_account, .context = job };
    for (i32 p = 0; p < job->count; p++) {
        if (atomic_load_explicit(&job->cancel, memory_order_acquire)) break;
        SleepIOPane *spec = &job->specs[p];
        if (job->kind == SLEEP_IO_WRITE) {
            job->result.ok[p] = sleep_snap_write_control(job->paths[p], job->terms[p], &control);
        } else if (job->kind == SLEEP_IO_READ) {
            Terminal *term = terminal_create(spec->cols, spec->rows);
            job->terms[p] = term;
            if (term) {
                terminal_set_scrollback_limit(term, spec->scrollback_lines);
                term->bidi_enabled = spec->bidi_enabled;
                sleep_account(job, NULL, 0);
                job->result.ok[p] = !job->paths[p][0] || sleep_snap_read_control(job->paths[p], term, &control);
            }
            /* A valid snapshot remains available for retry on allocation/IO
             * failure. A partially restored Terminal is never published. */
            if (term && !job->result.ok[p]) {
                terminal_destroy(term); job->terms[p] = NULL;
            }
        } else {
            Terminal *term = job->terms[p];
            terminal_resize(term, spec->cols, spec->rows);
            terminal_set_scrollback_limit(term, spec->scrollback_lines);
            term->bidi_enabled = spec->bidi_enabled;
            job->result.ok[p] = term->cols == spec->cols && term->rows == spec->rows;
        }
        sleep_account(job, NULL, 0);
        if ((job->kind == SLEEP_IO_READ || job->kind == SLEEP_IO_WRITE) && !job->result.ok[p]) break;
    }

    bool cancelled = atomic_load_explicit(&job->cancel, memory_order_acquire);
    if (cancelled) { sleep_dispose_terms(job); sleep_remove_paths(job); }
    else if (job->kind == SLEEP_IO_READ || job->kind == SLEEP_IO_WRITE) {
        bool all_ok = true;
        for (i32 p = 0; p < job->count; p++) if (!job->result.ok[p]) all_ok = false;
        if (all_ok) {
            if (job->kind == SLEEP_IO_READ) sleep_remove_paths(job);
            else sleep_dispose_terms(job);
        } else {
            if (job->kind == SLEEP_IO_READ) sleep_dispose_terms(job);
            else sleep_remove_paths(job);
            memset(job->result.ok, 0, sizeof job->result.ok);
        }
    }
    pthread_mutex_lock(&job->mutex);
    job->result.kind = job->kind; job->result.count = job->count;
    job->result.cancelled = cancelled;
    job->result.terminal_bytes = job->grid_charge;
    job->result.scrollback_bytes = job->sb_charge;
    for (i32 p = 0; p < job->count; p++) job->result.terminals[p] = job->terms[p];
    atomic_store_explicit(&job->ready, true, memory_order_release);
    pthread_mutex_unlock(&job->mutex);
    if (job->wake) job->wake();

    pthread_mutex_lock(&job->mutex);
    while (!job->taken && !job->abandoned && !atomic_load_explicit(&job->cancel, memory_order_acquire))
        pthread_cond_wait(&job->collected, &job->mutex);
    bool discard = job->abandoned || !job->taken;
    pthread_mutex_unlock(&job->mutex);
    /* Heavy result destruction never runs in release() on the UI thread. */
    sleep_dispose_terms(job);
    if (discard) sleep_remove_paths(job);
    pthread_mutex_lock(&g_admission);
    g_jobs--; g_reserved -= job->reserved;
    pthread_mutex_unlock(&g_admission);
    sleep_job_drop(job);
    return NULL;
}

static usize sleep_read_reservation(const SleepIOPane *p) {
    usize cells, grid, history;
    if (!checked_mul_usize((usize)p->cols, (usize)p->rows, &cells) ||
        !checked_mul_usize(cells, sizeof(Cell), &grid) ||
        !checked_mul_usize((usize)p->cols, (usize)p->scrollback_lines, &cells) ||
        !checked_mul_usize(cells, sizeof(Cell), &history)) return SIZE_MAX;
    if (history > MB(16)) history = MB(16); /* Terminal's own byte budget */
    usize ring = (usize)p->scrollback_lines * sizeof(TermLine);
    if (grid > SIZE_MAX - history - ring - MB(1)) return SIZE_MAX;
    return grid + history + ring + MB(1);
}

static SleepIOJob *sleep_start(SleepIOKind kind, const SleepIOPane *panes, i32 count) {
    if (!panes || count < 1 || count > SLEEP_IO_MAX_PANES) return NULL;
    SleepIOJob *job = calloc(1, sizeof *job);
    if (!job) return NULL;
    if (pthread_mutex_init(&job->mutex, NULL) != 0) { free(job); return NULL; }
    if (pthread_cond_init(&job->collected, NULL) != 0) { pthread_mutex_destroy(&job->mutex); free(job); return NULL; }
    job->kind = kind; job->count = count;
    atomic_init(&job->refs, 2); atomic_init(&job->ready, false); atomic_init(&job->cancel, false);
    job->wake = atomic_load_explicit(&g_wake, memory_order_acquire);
    for (i32 p = 0; p < count; p++) {
        const SleepIOPane *src = &panes[p];
        if (kind != SLEEP_IO_WRITE && (src->cols < 1 || src->cols > 4096 || src->rows < 1 || src->rows > 4096 ||
            src->scrollback_lines < 0 || src->scrollback_lines > CONFIG_SCROLLBACK_MAX_LINES)) goto fail;
        if (kind != SLEEP_IO_READ && !src->terminal) goto fail;
        for (i32 q = 0; kind != SLEEP_IO_READ && q < p; q++)
            if (src->terminal && src->terminal == panes[q].terminal) goto fail;
        const char *path = kind != SLEEP_IO_RESIZE && src->path ? src->path : "";
        usize n = strnlen(path, SLEEP_IO_PATH_MAX);
        if (n == SLEEP_IO_PATH_MAX || (kind == SLEEP_IO_WRITE && !n)) goto fail;
        if (kind != SLEEP_IO_RESIZE && n) for (i32 q = 0; q < p; q++)
            if (job->paths[q] && !strcmp(job->paths[q], path)) goto fail;
        job->paths[p] = malloc(n + 1);
        if (!job->paths[p]) goto fail;
        memcpy(job->paths[p], path, n + 1);
        job->specs[p] = *src; job->specs[p].path = job->paths[p]; job->specs[p].terminal = NULL;
        job->terms[p] = kind == SLEEP_IO_READ ? NULL : src->terminal;
        usize bytes = kind == SLEEP_IO_READ ? sleep_read_reservation(src) :
            terminal_owned_bytes(src->terminal) + sizeof(Terminal) +
                (usize)src->terminal->sb_capacity * sizeof(TermLine);
        if (bytes == SIZE_MAX || job->reserved > SIZE_MAX - bytes) goto fail;
        job->reserved += bytes;
    }
    pthread_mutex_lock(&g_admission);
    bool full = g_jobs >= SLEEP_IO_MAX_JOBS ||
        (g_jobs > 0 && (job->reserved > SLEEP_IO_MAX_BYTES || g_reserved > SLEEP_IO_MAX_BYTES - job->reserved));
    if (full) { pthread_mutex_unlock(&g_admission); goto fail; }
    g_jobs++; g_reserved += job->reserved;
    pthread_mutex_unlock(&g_admission);

    pthread_attr_t attr;
    bool attributes = pthread_attr_init(&attr) == 0;
    if (attributes && pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        pthread_attr_destroy(&attr); attributes = false;
    }
    pthread_mutex_lock(&job->mutex);
    pthread_t thread;
    int rc = attributes ? pthread_create(&thread, &attr, sleep_worker, job) : -1;
    if (attributes) pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_unlock(&job->mutex);
        pthread_mutex_lock(&g_admission); g_jobs--; g_reserved -= job->reserved; pthread_mutex_unlock(&g_admission);
        goto fail;
    }
    for (i32 p = 0; p < count; p++) {
        Terminal *term = job->terms[p];
        sleep_detach_callbacks(term);
        job->grid_charge += terminal_grid_bytes(term);
        job->sb_charge += terminal_scrollback_bytes(term);
    }
    /* These grids are charged to the transient worker until the next frame
     * recomputes the main retained inventory; release their charge here. */
    mem_budget_adjust(MEM_TERMINAL, -(isize)job->grid_charge);
    mem_budget_adjust(MEM_SCROLLBACK, -(isize)job->sb_charge);
    mem_budget_transient_adjust(MEM_TERMINAL, (isize)job->grid_charge);
    mem_budget_transient_adjust(MEM_SCROLLBACK, (isize)job->sb_charge);
    pthread_mutex_unlock(&job->mutex);
    return job;
fail:
    /* No terminal ownership was transferred on any failure path. */
    sleep_job_free(job);
    return NULL;
}

SleepIOJob *sleep_io_write(const SleepIOPane *p, i32 n) { return sleep_start(SLEEP_IO_WRITE, p, n); }
SleepIOJob *sleep_io_read(const SleepIOPane *p, i32 n) { return sleep_start(SLEEP_IO_READ, p, n); }
SleepIOJob *sleep_io_resize(const SleepIOPane *p, i32 n) { return sleep_start(SLEEP_IO_RESIZE, p, n); }
bool sleep_io_ready(const SleepIOJob *job) { return job && atomic_load_explicit(&job->ready, memory_order_acquire); }

bool sleep_io_take(SleepIOJob *job, SleepIOResult *out) {
    if (!job || !out || !sleep_io_ready(job)) return false;
    pthread_mutex_lock(&job->mutex);
    if (job->taken || job->abandoned) { pthread_mutex_unlock(&job->mutex); return false; }
    *out = job->result;
    if (atomic_load_explicit(&job->cancel, memory_order_acquire)) {
        out->cancelled = true; out->terminal_bytes = out->scrollback_bytes = 0;
        memset(out->terminals, 0, sizeof out->terminals);
        memset(out->ok, 0, sizeof out->ok);
    } else {
        for (i32 p = 0; p < job->count; p++) job->terms[p] = NULL;
        mem_budget_transient_adjust(MEM_TERMINAL, -(isize)job->grid_charge);
        mem_budget_transient_adjust(MEM_SCROLLBACK, -(isize)job->sb_charge);
        mem_budget_adjust(MEM_TERMINAL, (isize)job->grid_charge);
        mem_budget_adjust(MEM_SCROLLBACK, (isize)job->sb_charge);
        job->grid_charge = job->sb_charge = 0;
    }
    job->taken = true;
    pthread_cond_signal(&job->collected);
    pthread_mutex_unlock(&job->mutex);
    return true;
}
void sleep_io_cancel(SleepIOJob *job) {
    if (!job) return;
    atomic_store_explicit(&job->cancel, true, memory_order_release);
    pthread_mutex_lock(&job->mutex); pthread_cond_signal(&job->collected); pthread_mutex_unlock(&job->mutex);
}
void sleep_io_release(SleepIOJob *job) {
    if (!job) return;
    pthread_mutex_lock(&job->mutex);
    if (!job->taken) {
        job->abandoned = true;
        atomic_store_explicit(&job->cancel, true, memory_order_release);
    }
    pthread_cond_signal(&job->collected);
    pthread_mutex_unlock(&job->mutex);
    sleep_job_drop(job);
}
