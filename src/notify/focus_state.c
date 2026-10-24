/*
 * The notification server suppresses events when Liu is frontmost. The
 * application publishes this state from its main loop; the in-process server
 * reads it through an atomic. Querying `CGWindowListCopyWindowInfo` requires
 * screen-recording access on macOS 15+ and displays a capture indicator, so the
 * app does not use that API. The standalone `liu-notify` daemon has no app
 * process to publish state, so the publisher mirrors the flag to a state file
 * beside `notify.conf`. The file includes the publisher PID, and the flag is
 * valid only while that process is alive.
 */
#include "core/types.h"
#include "notify/notify_server.h"   /* notify_publish_frontmost */
#include "notify/platform_notify.h" /* notify_target_active */

#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Written only by notify_publish_frontmost (the app's main loop), read by the
 * server thread. `have_local` latches on the first publish: it marks this
 * process as the one holding the first-hand answer, so the in-process server
 * never falls back to parsing the file it wrote itself. */
static atomic_bool g_have_local = false;
static atomic_bool g_frontmost  = false;

static bool resolve_state_path(char *out, usize out_sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/') {
        int n = snprintf(out, out_sz, "%s/liu/frontmost", xdg);
        return n > 0 && (usize)n < out_sz;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(geteuid());
        if (!pw || !pw->pw_dir) return false;
        home = pw->pw_dir;
    }
    int n = snprintf(out, out_sz, "%s/.config/liu/frontmost", home);
    return n > 0 && (usize)n < out_sz;
}

/* mkdir -p for the state file's parent: on a fresh install the config dir does
 * not exist yet (notify.conf is written lazily too), and a missing directory
 * would silently turn every publish into a no-op. */
static void ensure_parent_dir(const char *path) {
    char dir[1024];
    int n = snprintf(dir, sizeof dir, "%s", path);
    if (n <= 0 || (usize)n >= sizeof dir) return;
    char *leaf = strrchr(dir, '/');
    if (!leaf || leaf == dir) return;
    *leaf = '\0';
    char *parent = strrchr(dir, '/');
    if (parent && parent != dir) {
        *parent = '\0';
        (void)mkdir(dir, 0700);   /* ~/.config */
        *parent = '/';
    }
    (void)mkdir(dir, 0700);       /* ~/.config/liu */
}

/* ---- the state file is written OFF the publisher's thread ------------------
 *
 * The publisher is the app's MAIN LOOP, and the mirror is six metadata calls
 * (two mkdir, unlink, an exclusive create, close, rename). Run inline, at the
 * moment the app is activated or deactivated, they would park the main thread
 * in the file system on a busy disk before the next frame is drawn. Nobody in
 * the process reads the file (the server thread reads the atomic); only a
 * standalone daemon does, and it does not need it this millisecond.
 *
 * One short-lived writer at a time, latest value wins: a publish records the
 * value and the path (resolved on the caller, where the environment is read)
 * and starts a writer unless one is running; the writer keeps going until
 * nothing newer is pending. */
static pthread_mutex_t g_mirror_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_mirror_cv = PTHREAD_COND_INITIALIZER;
static char g_mirror_path[1024];
static bool g_mirror_value   = false;
static bool g_mirror_pending = false;   /* a value newer than the one on disk */
static bool g_mirror_running = false;   /* a writer thread exists */

static void mirror_write(const char *path, bool frontmost) {
    ensure_parent_dir(path);

    /* tmp + rename: a standalone daemon must never read a half-written line.
     * The temp is created O_EXCL|O_NOFOLLOW after removing any leftover, so
     * a symlink a same-UID process planted at "<path>.tmp" cannot make the
     * write truncate whatever it points at. */
    char tmp[1040];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n <= 0 || (usize)n >= sizeof tmp) return;
    (void)unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); (void)unlink(tmp); return; }
    bool ok = fprintf(f, "%d %ld\n", frontmost ? 1 : 0, (long)getpid()) > 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) (void)unlink(tmp);
}

static void *mirror_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mirror_mu);
    while (g_mirror_pending) {
        char path[sizeof g_mirror_path];
        snprintf(path, sizeof path, "%s", g_mirror_path);
        bool value = g_mirror_value;
        g_mirror_pending = false;
        pthread_mutex_unlock(&g_mirror_mu);
        mirror_write(path, value);
        pthread_mutex_lock(&g_mirror_mu);
    }
    g_mirror_running = false;
    pthread_cond_broadcast(&g_mirror_cv);
    pthread_mutex_unlock(&g_mirror_mu);
    return NULL;
}

void notify_publish_frontmost(bool frontmost) {
    bool had  = atomic_exchange_explicit(&g_have_local, true, memory_order_release);
    bool prev = atomic_exchange_explicit(&g_frontmost, frontmost, memory_order_release);
    /* The in-process server reads the atomic, so the file write only exists for
     * a standalone daemon — skip it when nothing actually changed. */
    if (had && prev == frontmost) return;

    char path[1024];
    if (!resolve_state_path(path, sizeof path)) return;

    pthread_mutex_lock(&g_mirror_mu);
    snprintf(g_mirror_path, sizeof g_mirror_path, "%s", path);
    g_mirror_value   = frontmost;
    g_mirror_pending = true;
    bool spawn = !g_mirror_running;
    if (spawn) g_mirror_running = true;
    pthread_mutex_unlock(&g_mirror_mu);
    if (!spawn) return;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    int rc = pthread_create(&tid, &attr, mirror_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        /* No thread to be had: the mirror still has to be right. */
        pthread_mutex_lock(&g_mirror_mu);
        g_mirror_pending = false;
        g_mirror_running = false;
        pthread_cond_broadcast(&g_mirror_cv);
        pthread_mutex_unlock(&g_mirror_mu);
        mirror_write(path, frontmost);
    }
}

void notify_focus_state_flush(void) {
    pthread_mutex_lock(&g_mirror_mu);
    while (g_mirror_running) pthread_cond_wait(&g_mirror_cv, &g_mirror_mu);
    pthread_mutex_unlock(&g_mirror_mu);
}

bool notify_target_active(void) {
    if (atomic_load_explicit(&g_have_local, memory_order_acquire))
        return atomic_load_explicit(&g_frontmost, memory_order_acquire);

    char path[1024];
    if (!resolve_state_path(path, sizeof path)) return false;
    /* O_NONBLOCK|O_NOFOLLOW + fstat: a FIFO with no writer at this path
     * would block the single-threaded daemon in open forever, silencing
     * every notification from then on, and a symlink would be read for
     * whatever it names. Same discipline as notify/config.c. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_size > 256) {
        close(fd);
        return false;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return false; }
    int  flag = 0;
    long pid  = 0;
    bool parsed = fscanf(f, "%d %ld", &flag, &pid) == 2;
    fclose(f);
    if (!parsed || flag != 1 || pid <= 0) return false;

    /* Liveness check: a stale flag left by a crashed app would suppress every
     * notification from then on. EPERM means the pid exists but is owned by
     * another user — still alive, so the claim stands. */
    if (kill((pid_t)pid, 0) != 0 && errno != EPERM) return false;
    return true;
}
