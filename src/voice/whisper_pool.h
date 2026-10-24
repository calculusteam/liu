/* Model-owned CPU lanes. Jobs finish before their stack context can expire. */
#ifndef LIU_WHISPER_POOL_H
#define LIU_WHISPER_POOL_H
#include "core/types.h"
#include <pthread.h>

#define WC_MAX_THREADS 32
typedef void (*WcParFn)(void *, i32, i32, i32);
typedef struct WcPool WcPool;
typedef struct { WcPool *pool; i32 lane; } WcPoolLane;
struct WcPool {
    pthread_mutex_t lock;
    pthread_cond_t wake, done;
    pthread_t threads[WC_MAX_THREADS - 1];
    WcPoolLane lanes[WC_MAX_THREADS - 1];
    i32 workers, remaining, active, chunk, count;
    u64 generation;
    bool initialized, stop;
    WcParFn fn;
    void *ctx;
};

static void *wc_pool_worker(void *arg) {
    WcPoolLane *lane = arg;
    WcPool *pool = lane->pool;
    u64 seen = 0;
    pthread_mutex_lock(&pool->lock);
    for (;;) {
        while (!pool->stop && pool->generation == seen)
            pthread_cond_wait(&pool->wake, &pool->lock);
        if (pool->stop) break;
        seen = pool->generation;
        WcParFn fn = pool->fn;
        void *ctx = pool->ctx;
        i32 begin = lane->lane * pool->chunk;
        i32 end = MIN(pool->count, begin + pool->chunk);
        bool active = lane->lane < pool->active && begin < end;
        pthread_mutex_unlock(&pool->lock);
        if (active) fn(ctx, begin, end, lane->lane);
        pthread_mutex_lock(&pool->lock);
        if (--pool->remaining == 0) pthread_cond_signal(&pool->done);
    }
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

static void wc_pool_init(WcPool *pool, i32 threads) {
    if (pool->initialized) return;
    pool->stop = false;
    pool->generation = 0;
    pool->workers = 0;
    if (threads <= 1) return;
    if (threads > WC_MAX_THREADS) threads = WC_MAX_THREADS;
    if (pthread_mutex_init(&pool->lock, NULL)) return;
    if (pthread_cond_init(&pool->wake, NULL)) { pthread_mutex_destroy(&pool->lock); return; }
    if (pthread_cond_init(&pool->done, NULL)) {
        pthread_cond_destroy(&pool->wake); pthread_mutex_destroy(&pool->lock); return;
    }
    pool->initialized = true;
    for (i32 i = 0; i < threads - 1; i++) {
        pool->lanes[i] = (WcPoolLane){pool, i + 1};
        if (pthread_create(&pool->threads[i], NULL, wc_pool_worker, &pool->lanes[i])) break;
        pool->workers++;
    }
}

static void wc_pool_destroy(WcPool *pool) {
    if (!pool->initialized) return;
    pthread_mutex_lock(&pool->lock);
    pool->stop = true;
    pthread_cond_broadcast(&pool->wake);
    pthread_mutex_unlock(&pool->lock);
    for (i32 i = 0; i < pool->workers; i++) pthread_join(pool->threads[i], NULL);
    pthread_cond_destroy(&pool->wake); pthread_cond_destroy(&pool->done);
    pthread_mutex_destroy(&pool->lock);
    pool->initialized = false;
    pool->workers = 0;
}

static void wc_par_for(WcPool *pool, i32 n, WcParFn fn, void *ctx) {
    if (n <= 0) return;
    if (!pool->workers || n == 1) { fn(ctx, 0, n, 0); return; }
    pthread_mutex_lock(&pool->lock);
    pool->active = MIN(pool->workers + 1, n);
    pool->chunk = (n + pool->active - 1) / pool->active;
    pool->count = n; pool->fn = fn; pool->ctx = ctx;
    pool->remaining = pool->workers;
    pool->generation++;
    i32 end = MIN(n, pool->chunk);
    pthread_cond_broadcast(&pool->wake);
    pthread_mutex_unlock(&pool->lock);
    fn(ctx, 0, end, 0);
    pthread_mutex_lock(&pool->lock);
    while (pool->remaining) pthread_cond_wait(&pool->done, &pool->lock);
    pool->ctx = NULL; pool->fn = NULL;
    pthread_mutex_unlock(&pool->lock);
}
#endif
