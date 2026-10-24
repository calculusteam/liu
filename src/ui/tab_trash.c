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

#include "core/mem_budget.h"
#include "platform/platform.h"
#include "terminal/terminal.h"
#include "ssh/ssh_session.h"

/* The ceiling on what may sit in the queue waiting for one serial worker.
 *
 * terminal_destroy is slow for a deep scrollback, so the worker drains far
 * slower than the main thread can queue: a whole window of tabs can be queued
 * in one frame. Unbounded, closing 100 heavy tabs pins every one of their
 * scrollbacks until the worker catches up — and the per-frame ledger walk
 * cannot even see them, because they are no longer in any tab.
 *
 * 256 MiB is a sixth of MEM_BUDGET_SOFT_TOTAL: comfortably more than any
 * ordinary close (a 10k-line scrollback is a few MiB), and small enough that
 * the pathological case degrades to a synchronous destroy instead of a memory
 * spike. Counted in bytes, because the cost being bounded is memory, and a
 * tab's scrollback varies by three orders of magnitude; TAB_TRASH_MAX_ITEMS
 * bounds the count separately. */

typedef struct TrashItem {
    struct Terminal  *t;
    struct Session   *s;
    usize             bytes;   /* scrollback this item is holding */
    f64               queued;  /* platform_time_sec at push */
    struct TrashItem *next;
    usize            ledger_bytes;  /* the part charged to MEM_SCROLLBACK */
} TrashItem;

static pthread_mutex_t g_mu  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv  = PTHREAD_COND_INITIALIZER;
static TrashItem      *g_head = NULL;
static TrashItem      *g_tail = NULL;
static pthread_t       g_thread;
static bool            g_running = false;
static bool            g_stop    = false;
static i32             g_depth   = 0;
static usize           g_bytes   = 0;   /* retained by everything queued */

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
        /* Depth drops HERE, not at the dequeue above: an item that has left
         * the list but is still being destroyed is still outstanding, and its
         * scrollback is still held. Reporting depth 0 with the last teardown
         * in flight would make `tab_trash_stats` say "drained" while up to a
         * whole terminal's worth of memory is still on the ledger. */
        pthread_mutex_lock(&g_mu);
        g_depth--;
        if (it->bytes) g_bytes -= it->bytes < g_bytes ? it->bytes : g_bytes;
        pthread_mutex_unlock(&g_mu);
        if (it->ledger_bytes)
            mem_budget_transient_adjust(MEM_SCROLLBACK, -(isize)it->ledger_bytes);
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

    /* Read before the item is queued: after the push the worker owns it and the
     * main thread must not touch it again.
     *
     * EVERYTHING the Terminal owns, not just its scrollback: `t->sb_byte_usage`
     * is zero for a terminal that has scrolled nothing, yet such a terminal can
     * hold megabytes of images, and a queue of them is exactly the transient
     * the bound exists to stop. */
    usize bytes = terminal_owned_bytes(t);
    /* The LEDGER charge stays what it was: the Terminal has not been destroyed
     * yet, so its grid, images and scrollback are still charged to their own
     * categories, and this transient is already a double count for the window
     * between the push and the teardown. Widening it to the full footprint
     * would widen the double count too. The full figure is for the ADMISSION
     * decision — `g_bytes`, the queue's own counter. */
    usize ledger_bytes = t ? t->sb_byte_usage : 0;

    pthread_mutex_lock(&g_mu);
    bool full = (g_bytes + bytes > TAB_TRASH_MAX_BYTES ||
                 g_depth >= TAB_TRASH_MAX_ITEMS) && g_depth > 0;
    pthread_mutex_unlock(&g_mu);
    /* `g_depth > 0` keeps a single enormous tab from being refused forever:
     * one item is always allowed through, so the bound throttles a BURST
     * rather than making a big scrollback permanently undeferrable. */
    if (full) return false;

    TrashItem *it = (TrashItem *)malloc(sizeof(TrashItem));
    if (!it) return false;
    it->t = t;
    it->s = s;
    it->bytes = bytes;            /* admission / g_bytes */
    it->ledger_bytes = ledger_bytes;
    it->queued = platform_time_sec();
    it->next = NULL;
    /* Overlap local-child grace periods across queued closes. Starting the
     * grace only when the serial worker reaches each item would add a whole
     * grace period per item. */
    if (s) (void)session_prepare_destroy(s);
    if (ledger_bytes) mem_budget_transient_adjust(MEM_SCROLLBACK, (isize)ledger_bytes);
    pthread_mutex_lock(&g_mu);
    if (g_tail) g_tail->next = it;
    else        g_head = it;
    g_tail = it;
    g_depth++;
    g_bytes += bytes;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return true;
}

void tab_trash_stats(i32 *out_depth, usize *out_bytes, f64 *out_oldest_sec) {
    pthread_mutex_lock(&g_mu);
    if (out_depth) *out_depth = g_depth;
    if (out_bytes) *out_bytes = g_bytes;
    if (out_oldest_sec)
        *out_oldest_sec = g_head ? platform_time_sec() - g_head->queued : 0.0;
    pthread_mutex_unlock(&g_mu);
}
