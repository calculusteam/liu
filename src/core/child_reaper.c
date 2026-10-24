#include "core/child_reaper.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REAPER_MAX 16

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_pids[REAPER_MAX];     /* children a supervisor owns right now */
static i32   g_count;
static bool  g_atexit_registered;

static void sleep_ms(i32 ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void forget(pid_t pid) {
    pthread_mutex_lock(&g_mu);
    for (i32 i = 0; i < g_count; i++) {
        if (g_pids[i] == pid) { g_pids[i] = g_pids[--g_count]; break; }
    }
    pthread_mutex_unlock(&g_mu);
}

/* True once the child has been collected (or was never this module's to
 * collect). */
static bool collected(pid_t pid) {
    int st = 0;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return true;
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return true;           /* ECHILD: gone, or reaped elsewhere */
        return false;
    }
}

static void *supervise(void *arg) {
    pid_t pid = (pid_t)(intptr_t)arg;
    for (i32 waited = 0; waited < CHILD_REAPER_GRACE_MS; waited += 5) {
        if (collected(pid)) { forget(pid); return NULL; }
        sleep_ms(5);
    }
    kill(pid, SIGKILL);                   /* uncatchable: the wait below is bounded */
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    forget(pid);
    return NULL;
}

/* At exit nothing may be left running that this process started to end: an
 * orphan that ignored SIGTERM would outlive the app. No waiting — SIGKILL is
 * enough, and the kernel reparents what is left. */
static void sweep_at_exit(void) {
    pthread_mutex_lock(&g_mu);
    for (i32 i = 0; i < g_count; i++) kill(g_pids[i], SIGKILL);
    pthread_mutex_unlock(&g_mu);
}

void child_reaper_terminate(pid_t pid) {
    if (pid <= 0) return;

    pthread_mutex_lock(&g_mu);
    for (i32 i = 0; i < g_count; i++) {
        if (g_pids[i] == pid) { pthread_mutex_unlock(&g_mu); return; }
    }
    bool room = g_count < REAPER_MAX;
    if (room) g_pids[g_count++] = pid;
    if (!g_atexit_registered) { g_atexit_registered = true; atexit(sweep_at_exit); }
    pthread_mutex_unlock(&g_mu);

    kill(pid, SIGTERM);

    pthread_t th;
    pthread_attr_t attr;
    bool started = false;
    if (room && pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0)
            started = pthread_create(&th, &attr, supervise, (void *)(intptr_t)pid) == 0;
        pthread_attr_destroy(&attr);
    }
    if (started) return;

    /* No supervisor to hand it to: end it here, bounded. */
    kill(pid, SIGKILL);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (room) forget(pid);
}

i32 child_reaper_inflight(void) {
    pthread_mutex_lock(&g_mu);
    i32 n = g_count;
    pthread_mutex_unlock(&g_mu);
    return n;
}

bool child_reaper_wait_idle(i32 timeout_ms) {
    for (i32 waited = 0; ; waited += 5) {
        if (child_reaper_inflight() == 0) return true;
        if (waited >= timeout_ms) return false;
        sleep_ms(5);
    }
}
