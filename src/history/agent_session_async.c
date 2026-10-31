#include "history/agent_session_async.h"
#include "core/memory.h"
#include "core/mem_budget.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How long shutdown waits for the worker to leave. A lookup stops at its next
 * store boundary once cancelled, but a read the kernel holds (a store on a
 * volume that went away) does not; quitting never waits on that. */
#define AGENT_SESSION_SHUTDOWN_WAIT_MS 1000

typedef enum { SLOT_EMPTY, SLOT_QUEUED, SLOT_RUNNING, SLOT_DONE } SlotState;
typedef struct {
    u64 token;
    SlotState state;
    _Atomic bool cancel;      /* set when the slot is released while running */
    AgentSessionResult value;
} Slot;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_cond_t left;      /* the current worker returned */
    pthread_t worker;
    Arena arena;
    Slot *slots;
    u64 next_token;
    /* Bumped by every shutdown. A worker shutdown stopped waiting for runs
     * under an older epoch: it keeps the slots it was given (they are not
     * freed) and leaves without touching the current ones. */
    u64 epoch;
    bool started, stop, exit_registered, worker_live;
    void (*wake)(void);
} g = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER,
       .left = PTHREAD_COND_INITIALIZER};

static void *worker_main(void *arg) {
    u64 epoch = (u64)(uintptr_t)arg;
#ifdef __APPLE__
    (void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
    for (;;) {
        pthread_mutex_lock(&g.lock);
        i32 index = -1;
        while (!g.stop && g.epoch == epoch) {
            /* Oldest request first. */
            for (i32 i = 0; i < AGENT_SESSION_ASYNC_CAP; i++)
                if (g.slots[i].state == SLOT_QUEUED &&
                    (index < 0 || g.slots[i].token < g.slots[index].token)) index = i;
            if (index >= 0) break;
            pthread_cond_wait(&g.changed, &g.lock);
        }
        if (g.stop || g.epoch != epoch) {
            if (g.epoch == epoch) {
                g.worker_live = false;
                pthread_cond_broadcast(&g.left);
            }
            pthread_mutex_unlock(&g.lock);
            break;
        }
        Slot *slot = &g.slots[index];
        slot->state = SLOT_RUNNING;
        atomic_store(&slot->cancel, false);
        u64 token = slot->token;
        AgentSessionResult answer = slot->value;
        pthread_mutex_unlock(&g.lock);

        /* No store is read while the mailbox is held. */
        answer.found = agent_session_locate(&answer.query, &answer.ref, &slot->cancel);

        pthread_mutex_lock(&g.lock);
        void (*wake)(void) = NULL;
        if (!g.stop && g.epoch == epoch && slot->token == token && slot->state == SLOT_RUNNING) {
            slot->value = answer;
            slot->state = SLOT_DONE;
            wake = g.wake;
        }
        pthread_mutex_unlock(&g.lock);
        if (wake) wake();
    }
    return NULL;
}

void agent_session_async_init(void (*wake)(void)) {
    pthread_mutex_lock(&g.lock);
    g.wake = wake;
    g.stop = false;
    if (!g.exit_registered) g.exit_registered = atexit(agent_session_async_shutdown) == 0;
    pthread_mutex_unlock(&g.lock);
}

u64 agent_session_async_request(const AgentSessionQuery *q) {
    if (!q || !agent_session_tool_supported(q->tool) || q->cwd[0] != '/') return 0;
    pthread_mutex_lock(&g.lock);
    if (g.stop) { pthread_mutex_unlock(&g.lock); return 0; }
    if (!g.slots) {
        g.arena = arena_create(sizeof(Slot) * AGENT_SESSION_ASYNC_CAP);
        g.slots = g.arena.base ? arena_alloc(&g.arena, sizeof(Slot) * AGENT_SESSION_ASYNC_CAP) : NULL;
        if (!g.slots) {
            arena_destroy(&g.arena);
            pthread_mutex_unlock(&g.lock);
            return 0;
        }
        memset(g.slots, 0, sizeof(Slot) * AGENT_SESSION_ASYNC_CAP);
        mem_budget_transient_adjust(MEM_HISTORY, (isize)g.arena.size);
    }
    if (!g.started) {
        if (pthread_create(&g.worker, NULL, worker_main, (void *)(uintptr_t)g.epoch) != 0) {
            pthread_mutex_unlock(&g.lock);
            return 0;
        }
        g.started = true;
        g.worker_live = true;
    }
    i32 vacant = -1;
    for (i32 i = 0; i < AGENT_SESSION_ASYNC_CAP; i++)
        if (g.slots[i].state == SLOT_EMPTY) { vacant = i; break; }
    if (vacant < 0) { pthread_mutex_unlock(&g.lock); return 0; }
    Slot *slot = &g.slots[vacant];
    slot->state = SLOT_QUEUED;
    atomic_store(&slot->cancel, false);
    memset(&slot->value, 0, sizeof slot->value);
    slot->value.query = *q;
    /* The slot is part of the token, so a lookup is one index, not a search. */
    slot->token = ++g.next_token * AGENT_SESSION_ASYNC_CAP + (u64)vacant;
    u64 token = slot->token;
    pthread_cond_signal(&g.changed);
    pthread_mutex_unlock(&g.lock);
    return token;
}

AgentSessionStatus agent_session_async_result(u64 token, AgentSessionResult *out) {
    if (!token || !out) return AGENT_SESSION_EXPIRED;
    AgentSessionStatus status = AGENT_SESSION_EXPIRED;
    pthread_mutex_lock(&g.lock);
    if (g.slots) {
        Slot *slot = &g.slots[token % AGENT_SESSION_ASYNC_CAP];
        if (slot->token == token && slot->state != SLOT_EMPTY) {
            status = slot->state == SLOT_DONE ? AGENT_SESSION_READY : AGENT_SESSION_PENDING;
            if (status == AGENT_SESSION_READY) *out = slot->value;
        }
    }
    pthread_mutex_unlock(&g.lock);
    return status;
}

void agent_session_async_release(u64 *token) {
    if (!token || !*token) return;
    u64 t = *token;
    *token = 0;
    pthread_mutex_lock(&g.lock);
    if (g.slots) {
        Slot *slot = &g.slots[t % AGENT_SESSION_ASYNC_CAP];
        if (slot->token == t) {
            /* A running lookup stops at its next store boundary; its answer
             * is dropped by the token check when it returns. */
            atomic_store(&slot->cancel, true);
            slot->state = SLOT_EMPTY;
            slot->token = 0;
        }
    }
    pthread_mutex_unlock(&g.lock);
}

void agent_session_async_shutdown(void) {
    pthread_mutex_lock(&g.lock);
    g.stop = true;
    g.wake = NULL;
    bool started = g.started;
    pthread_t worker = g.worker;
    if (g.slots)
        for (i32 i = 0; i < AGENT_SESSION_ASYNC_CAP; i++) atomic_store(&g.slots[i].cancel, true);
    pthread_cond_broadcast(&g.changed);
    if (started) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec  += AGENT_SESSION_SHUTDOWN_WAIT_MS / 1000;
        until.tv_nsec += (long)(AGENT_SESSION_SHUTDOWN_WAIT_MS % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        int rc = 0;
        while (g.worker_live && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&g.left, &g.lock, &until);
    }
    bool left = !started || !g.worker_live;
    g.started = false;
    g.worker_live = false;
    g.epoch++;
    if (left) {
        if (g.arena.base) mem_budget_transient_adjust(MEM_HISTORY, -(isize)g.arena.size);
        arena_destroy(&g.arena);
    } else {
        /* The worker is still inside a lookup and reads its slot's cancel
         * flag: its slots stay allocated for it, and it is not waited for. */
        memset(&g.arena, 0, sizeof g.arena);
    }
    g.slots = NULL;
    pthread_mutex_unlock(&g.lock);
    if (started) {
        if (left) pthread_join(worker, NULL);
        else      pthread_detach(worker);
    }
}
