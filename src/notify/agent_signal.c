/*
 * Liu — agent lifecycle signal ring (see agent_signal.h for the contract).
 */
#include "notify/agent_signal.h"

#include <pthread.h>
#include <string.h>

#define RING_CAP 32   /* power of two: mask instead of modulo */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static AgentSignal     g_ring[RING_CAP];
static u32             g_head;   /* next write */
static u32             g_tail;   /* next read */
static void          (*g_wake)(void);   /* guarded by g_lock; called outside it */

void agent_signal_set_wake(void (*wake)(void)) {
    pthread_mutex_lock(&g_lock);
    g_wake = wake;
    pthread_mutex_unlock(&g_lock);
}

void agent_signal_push(const AgentSignal *sig) {
    if (!sig) return;
    pthread_mutex_lock(&g_lock);
    /* Full → drop the oldest so the newest (usually the "finished" the UI is
     * waiting for) always lands. */
    if (g_head - g_tail >= RING_CAP) g_tail++;
    g_ring[g_head & (RING_CAP - 1)] = *sig;
    g_head++;
    void (*wake)(void) = g_wake;
    pthread_mutex_unlock(&g_lock);
    /* After the unlock, never under it: the woken thread's first act is
     * agent_signal_pop(), and a wake that takes its own lock must not be able
     * to order itself against ours. */
    if (wake) wake();
}

bool agent_signal_pop(AgentSignal *out) {
    if (!out) return false;
    bool got = false;
    pthread_mutex_lock(&g_lock);
    if (g_head != g_tail) {
        *out = g_ring[g_tail & (RING_CAP - 1)];
        g_tail++;
        got = true;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}
