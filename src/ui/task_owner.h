/* Shared ownership for a one-shot worker task used by a window and its worker
 * thread.
 *
 * The worker releases its share when it finishes. The window releases its
 * share after collecting the joined result, or after detaching a
 * still-running worker during close. The final share release owns the free.
 * If thread creation fails, the starter remains the only owner. This prevents
 * a closed window from leaking the task and retained Session. */
#ifndef LIU_TASK_OWNER_H
#define LIU_TASK_OWNER_H

#include "core/types.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef struct {
    atomic_int  owners;    /* shares outstanding: 2 while both hold one */
    atomic_bool done;      /* the worker's result is published */
    pthread_t   tid;
    bool        started;   /* a worker thread exists (tid valid) */
} TaskOwner;

static inline void task_owner_init(TaskOwner *o) {
    atomic_init(&o->owners, 1);      /* the starter's share */
    atomic_init(&o->done, false);
    o->started = false;
}

/* Give the worker its share BEFORE the thread is created — a worker that
 * finishes before pthread_create returns must find two shares, or its drop
 * would free the task under the window. The caller stores the thread id
 * into `tid` after a successful create; on a failed create the task has no
 * second owner and the caller frees it outright. */
static inline void task_owner_worker_share(TaskOwner *o) {
    o->started = true;
    atomic_fetch_add(&o->owners, 1);
}

/* One share goes. True when it was the last — the caller frees. */
static inline bool task_owner_drop(TaskOwner *o) {
    return atomic_fetch_sub(&o->owners, 1) == 1;
}

/* The worker's last act: publish, then drop. `done` is set with release
 * semantics by the atomic store; the window's poll reads it with acquire. */
static inline bool task_owner_worker_finish(TaskOwner *o) {
    atomic_store(&o->done, true);
    return task_owner_drop(o);
}

/* The window's poll: collect a FINISHED worker (join) and drop the window's
 * share. False when the worker is not done yet — call again later. Sets
 * *free_it when the caller now owns the free. */
static inline bool task_owner_window_collect(TaskOwner *o, bool *free_it) {
    if (!atomic_load(&o->done)) return false;
    if (o->started) pthread_join(o->tid, NULL);
    *free_it = task_owner_drop(o);
    return true;
}

/* The window is going away NOW, finished or not: join a finished worker,
 * detach a running one, drop the window's share. Returns whether the caller
 * owns the free (never while the worker still runs — it will free). */
static inline bool task_owner_window_release(TaskOwner *o) {
    if (o->started) {
        if (atomic_load(&o->done)) pthread_join(o->tid, NULL);
        else                       pthread_detach(o->tid);
    }
    return task_owner_drop(o);
}

#endif /* LIU_TASK_OWNER_H */
