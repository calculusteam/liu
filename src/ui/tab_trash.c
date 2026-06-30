/*
 * tab_trash — background queue for Terminal/Session destruction.
 *
 * Single producer (main thread) → single consumer (worker thread). The
 * queue is a plain singly-linked list under a mutex; this is not a hot
 * path (tab closes happen at human speed) so lock contention is fine.
 *
 * Lifetime contract:
 *   - The caller has already removed the Terminal/Session from app state
 *     (tabs array shifted, broadcast targets cleared, translate target
 *     nulled, render caches freed) before pushing.
 *   - Background thread owns the objects from push to free; main thread
 *     must never read them again.
 */

#include "ui/tab_trash.h"

#include <pthread.h>
#include <stdlib.h>

#include "terminal/terminal.h"
#include "ssh/ssh_session.h"

typedef struct TrashItem {
    struct Terminal  *t;
    struct Session   *s;
    struct TrashItem *next;
} TrashItem;

static pthread_mutex_t g_mu  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv  = PTHREAD_COND_INITIALIZER;
static TrashItem      *g_head = NULL;
static TrashItem      *g_tail = NULL;
static pthread_t       g_thread;
static bool            g_running = false;
static bool            g_stop    = false;

static void *trash_worker(void *arg) {
    (void)arg;
    /* macOS names the calling thread (1 arg); glibc takes (thread, name). */
#ifdef __APPLE__
    pthread_setname_np("liu-tab-trash");
#else
    pthread_setname_np(pthread_self(), "liu-tab-trash");
#endif
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_head && !g_stop) {
            pthread_cond_wait(&g_cv, &g_mu);
        }
        if (g_stop && !g_head) {
            pthread_mutex_unlock(&g_mu);
            return NULL;
        }
        TrashItem *it = g_head;
        g_head = it->next;
        if (!g_head) g_tail = NULL;
        pthread_mutex_unlock(&g_mu);

        /* terminal_destroy walks scrollback freeing every row;
         * session_destroy may pthread_join the connect worker. Both heavy
         * for SSH / large scrollback. Off the main thread, neither shows. */
        if (it->t) terminal_destroy(it->t);
        if (it->s) session_destroy(it->s);
        free(it);
    }
}

void tab_trash_init(void) {
    if (g_running) return;
    g_stop = false;
    if (pthread_create(&g_thread, NULL, trash_worker, NULL) == 0) {
        g_running = true;
    }
}

void tab_trash_shutdown(void) {
    if (!g_running) return;
    pthread_mutex_lock(&g_mu);
    g_stop = true;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_thread, NULL);
    g_running = false;
}

bool tab_trash_defer(struct Terminal *t, struct Session *s) {
    if (!t && !s) return true;
    if (!g_running) return false;
    TrashItem *it = (TrashItem *)malloc(sizeof(TrashItem));
    if (!it) return false;
    it->t = t;
    it->s = s;
    it->next = NULL;
    pthread_mutex_lock(&g_mu);
    if (g_tail) g_tail->next = it;
    else        g_head = it;
    g_tail = it;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return true;
}
