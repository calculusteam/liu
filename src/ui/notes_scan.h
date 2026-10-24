/* Sidebar note metadata is collected without borrowing an AppState. */
#ifndef LIU_NOTES_SCAN_H
#define LIU_NOTES_SCAN_H

#include "core/types.h"
#include "core/mem_budget.h"
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define NOTES_SCAN_ROWS 6
#define NOTES_SCAN_MAX_JOBS 16
static _Atomic i32 notes_scan_jobs;
typedef struct {
    char name[NOTES_SCAN_ROWS][96];
    char path[NOTES_SCAN_ROWS][1024];
    f64 mtime[NOTES_SCAN_ROWS];
    i32 count;
} NotesScanResult;

typedef struct {
    _Atomic u32 refs;
    _Atomic bool cancelled;
    _Atomic bool done;
    char root[1024];
    NotesScanResult result;
    void (*wake)(void);
} NotesScanJob;

static void notes_scan_unref(NotesScanJob *job) {
    if (job && atomic_fetch_sub_explicit(&job->refs, 1, memory_order_acq_rel) == 1) {
        mem_budget_transient_adjust(MEM_EDITOR, -(isize)sizeof(*job));
        atomic_fetch_sub_explicit(&notes_scan_jobs, 1, memory_order_relaxed);
        free(job);
    }
}

static void notes_scan_cancel(NotesScanJob **owner) {
    NotesScanJob *job = owner ? *owner : NULL;
    if (!job) return;
    *owner = NULL;
    atomic_store_explicit(&job->cancelled, true, memory_order_release);
    notes_scan_unref(job);
}

static void *notes_scan_worker(void *arg) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
    NotesScanJob *job = arg;
    DIR *dir = opendir(job->root);
    if (dir) {
        struct dirent *entry;
        while (!atomic_load_explicit(&job->cancelled, memory_order_acquire) &&
               (entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            usize len = strlen(name);
            if (name[0] == '.' || len < 4 || strcasecmp(name + len - 3, ".md")) continue;
            char path[1024];
            int n = snprintf(path, sizeof(path), "%s/%s", job->root, name);
            if (n < 0 || (usize)n >= sizeof(path)) continue;
            struct stat st;
            if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
            NotesScanResult *r = &job->result;
            f64 mtime = (f64)st.st_mtime;
            i32 pos = r->count;
            while (pos > 0 && r->mtime[pos - 1] < mtime) pos--;
            if (pos >= NOTES_SCAN_ROWS) continue;
            i32 last = MIN(r->count, NOTES_SCAN_ROWS - 1);
            for (i32 i = last; i > pos; i--) {
                r->mtime[i] = r->mtime[i - 1];
                memcpy(r->name[i], r->name[i - 1], sizeof(r->name[i]));
                memcpy(r->path[i], r->path[i - 1], sizeof(r->path[i]));
            }
            r->mtime[pos] = mtime;
            snprintf(r->name[pos], sizeof(r->name[pos]), "%.*s", (int)(len - 3), name);
            memcpy(r->path[pos], path, (usize)n + 1);
            if (r->count < NOTES_SCAN_ROWS) r->count++;
        }
        closedir(dir);
    }
    atomic_store_explicit(&job->done, true, memory_order_release);
    if (!atomic_load_explicit(&job->cancelled, memory_order_acquire) && job->wake)
        job->wake();
    notes_scan_unref(job);
    return NULL;
}

static NotesScanJob *notes_scan_start(const char *root, void (*wake)(void)) {
    if (!root || !*root || strlen(root) >= 1024) return NULL;
    i32 count = atomic_load_explicit(&notes_scan_jobs, memory_order_relaxed);
    do {
        if (count >= NOTES_SCAN_MAX_JOBS) return NULL;
    } while (!atomic_compare_exchange_weak_explicit(&notes_scan_jobs, &count, count + 1,
                  memory_order_relaxed, memory_order_relaxed));
    NotesScanJob *job = calloc(1, sizeof(*job));
    if (!job) {
        atomic_fetch_sub_explicit(&notes_scan_jobs, 1, memory_order_relaxed);
        return NULL;
    }
    mem_budget_transient_adjust(MEM_EDITOR, (isize)sizeof(*job));
    atomic_init(&job->refs, 2);
    atomic_init(&job->cancelled, false);
    atomic_init(&job->done, false);
    snprintf(job->root, sizeof(job->root), "%s", root);
    job->wake = wake;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, KB(256));
    int rc = pthread_create(&thread, &attr, notes_scan_worker, job);
    pthread_attr_destroy(&attr);
    if (rc) { notes_scan_unref(job); notes_scan_unref(job); return NULL; }
    return job;
}

static bool notes_scan_take(NotesScanJob **owner, NotesScanResult *out) {
    NotesScanJob *job = owner ? *owner : NULL;
    if (!job || !atomic_load_explicit(&job->done, memory_order_acquire)) return false;
    if (out) *out = job->result;
    *owner = NULL;
    notes_scan_unref(job);
    return true;
}

#endif
