/* glibc only exposes `struct ucred` / SO_PEERCRED from <sys/socket.h> when
 * _GNU_SOURCE is defined — must come before any system header include. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

/*
 * liu-notify - AF_UNIX socket helpers with strict safety posture:
 *   - ownership of the socket node is a LOCK, never an inference from connect()
 *     (see "who owns the node" below),
 *   - FD_CLOEXEC / SOCK_CLOEXEC on every fd,
 *   - the node lives in an owner-only directory and is chmod()ed 0600,
 *   - peer UID verification (getpeereid on BSD/macOS, SO_PEERCRED on Linux).
 */
#include "notify/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    #define NOTIFY_HAVE_GETPEEREID 1
#endif

#if defined(__linux__)
    #define NOTIFY_HAVE_SO_PEERCRED 1
    #include <sys/types.h>     /* ucred */
#endif

static const char SOCK_NAME[] = "liu-notify.sock";
static const char PID_NAME[]  = "liu-notify.pid";

/* True when `path` is a directory this user owns and nobody else can enter.
 * The socket node, its lock file and the debug log all rely on the DIRECTORY
 * for their privacy: bind() creates the node under the process umask (which is
 * not changed around it — see notify_socket_listen for why), so for the
 * instant before chmod() the directory is the only thing between the node and
 * another user. */
static bool dir_is_private(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)   return false;
    if (!S_ISDIR(st.st_mode))   return false;
    if (st.st_uid != geteuid()) return false;
    return (st.st_mode & 0777) == 0700;
}

#if defined(__APPLE__)
static bool darwin_user_tmp(char *out, usize out_sz) {
    /* Kernel-provided per-UID dir, not spoofable via $TMPDIR. It is created
     * 0700 and owned by the user — but that is checked, not assumed: the mode
     * can be changed by its owner like any other directory's, and a group- or
     * world-searchable one would expose the node for the moment it is 0755. A
     * dir that fails the check is not "repaired" (it is the whole account's
     * temp dir, not Liu's to chmod): the caller falls back to ~/.liu/run,
     * which this file creates itself. */
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, out, out_sz);
    if (n == 0 || n > out_sz) return false;
    /* Trailing slash already present on macOS; trim for uniformity. */
    size_t l = strlen(out);
    if (l > 1 && out[l - 1] == '/') out[l - 1] = '\0';
    return dir_is_private(out);
}
#endif

static bool ensure_dir_0700(const char *path) {
    /* Try to create atomically; fall back to validating an existing dir.
     * This avoids the classic stat-then-mkdir TOCTOU window. */
    if (mkdir(path, 0700) == 0) return true;
    if (errno != EEXIST) return false;

    struct stat st;
    if (stat(path, &st) != 0)       return false;
    if (!S_ISDIR(st.st_mode))       return false;
    if (st.st_uid != geteuid())     return false;
    if ((st.st_mode & 0777) != 0700) return chmod(path, 0700) == 0;
    return true;
}

static bool home_run_dir(char *out, usize out_sz) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(geteuid());
        if (!pw || !pw->pw_dir) return false;
        home = pw->pw_dir;
    }
    char base[256];
    if ((usize)snprintf(base, sizeof base, "%s/.liu", home) >= sizeof base) return false;
    if (!ensure_dir_0700(base)) return false;
    char run[320];
    if ((usize)snprintf(run, sizeof run, "%s/run", base) >= sizeof run) return false;
    if (!ensure_dir_0700(run)) return false;
    if ((usize)snprintf(out, out_sz, "%s", run) >= out_sz) return false;
    return true;
}

static bool socket_dir(char *out, usize out_sz) {
    const char *override = getenv("LIU_NOTIFY_RUN_DIR");
    if (override && *override == '/') {
        if (!ensure_dir_0700(override)) return false;
        if ((usize)snprintf(out, out_sz, "%s", override) >= out_sz) return false;
        return true;
    }
#if defined(__APPLE__)
    if (darwin_user_tmp(out, out_sz)) return true;
    return home_run_dir(out, out_sz);
#elif defined(__linux__)
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg == '/' && dir_is_private(xdg)) {
        if ((usize)snprintf(out, out_sz, "%s", xdg) >= out_sz) return false;
        return true;
    }
    return home_run_dir(out, out_sz);
#else
    return home_run_dir(out, out_sz);
#endif
}

static bool build_path(char *out, usize out_sz, const char *name) {
    char dir[256];
    if (!socket_dir(dir, sizeof dir)) return false;
    struct sockaddr_un probe;
    usize cap = sizeof probe.sun_path;
    int wrote = snprintf(out, out_sz, "%s/%s", dir, name);
    if (wrote < 0 || (usize)wrote >= out_sz || (usize)wrote >= cap) {
        fprintf(stderr, "liu-notify: socket path too long (%d bytes, cap=%zu). "
                        "Set $XDG_RUNTIME_DIR or ensure ~/.liu/run exists.\n",
                wrote, cap);
        return false;
    }
    return true;
}

bool notify_run_dir(char *out, usize out_sz) {
    return socket_dir(out, out_sz);
}

bool notify_socket_path(char *out, usize out_sz) {
    return build_path(out, out_sz, SOCK_NAME);
}

bool notify_pid_path(char *out, usize out_sz) {
    return build_path(out, out_sz, PID_NAME);
}

static bool set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static int make_socket(void) {
    int fd;
#if defined(SOCK_CLOEXEC)
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) return fd;
    if (errno != EINVAL && errno != EPROTOTYPE) return -1;
#endif
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 && !set_cloexec(fd)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static bool set_addr(struct sockaddr_un *addr, const char *path) {
    if (!addr || !path || !*path) return false;
    if (strlen(path) >= sizeof addr->sun_path) return false;
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    /* strlen checked above; snprintf guarantees NUL terminate. */
    snprintf(addr->sun_path, sizeof addr->sun_path, "%s", path);
    return true;
}

static int connect_fd(int fd, const char *path) {
    struct sockaddr_un addr;
    if (!set_addr(&addr, path)) { errno = ENAMETOOLONG; return -1; }
    int rc;
    do { rc = connect(fd, (struct sockaddr *)&addr, sizeof addr); }
    while (rc < 0 && errno == EINTR);
    return rc;
}

int notify_socket_connect(const char *path) {
    int fd = make_socket();
    if (fd < 0) return -1;

    if (connect_fd(fd, path) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int notify_socket_connect_timeout(const char *path, i32 timeout_ms) {
    int fd = make_socket();
    if (fd < 0) return -1;

    /* Both directions, set BEFORE connect(): on Linux an AF_UNIX connect() to a
     * listener whose accept queue is full BLOCKS (it waits out SO_SNDTIMEO, or
     * for ever without one) where macOS refuses at once, and the ack read
     * afterwards waits on a host that may never answer. A timeout of 0 would
     * mean "no timeout" to the kernel, so it is floored at 1 ms. */
    if (timeout_ms < 1) timeout_ms = 1;
    struct timeval tv = { .tv_sec  = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (connect_fd(fd, path) < 0) {
        int saved = errno;
        close(fd);
        /* Linux reports an expired SO_SNDTIMEO on connect() as EAGAIN. */
        errno = (saved == EAGAIN || saved == EWOULDBLOCK) ? ETIMEDOUT : saved;
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------------- */
/* who owns the node                                                         */
/*                                                                           */
/* connect() cannot tell whether a live host exists: a refusal is what a     */
/* stale node answers, but also what macOS answers for a LIVE listener whose */
/* accept queue is full, and for one that has bind()ed and not yet           */
/* listen()ed. Binding over a refused node would let a second instance       */
/* unlink the live host's node and bind its own, and whichever of the two    */
/* quit first would then unlink the survivor's. A full backlog of            */
/* unaccepted clients is enough to cause it.                                 */
/*                                                                           */
/* Ownership is a POSIX write lock on `<socket path>.lock`, taken before     */
/* anything at the path is touched and held for the listener's whole life:    */
/*   - it dies with the process, so a crashed host leaves nothing to clean,   */
/*   - it is not inherited across fork(), so a child never owns by accident,  */
/*   - whoever cannot take it KNOWS a live host exists and touches nothing,   */
/*   - whoever takes it knows no host that takes the lock is alive.           */
/*                                                                           */
/* The trap that shapes this code: a POSIX lock is dropped when the process   */
/* closes ANY descriptor of that file. So the lock file is opened in exactly  */
/* one place (owner_lock_take), and only when this process holds no lock on   */
/* it; the one descriptor lives in g_own until the lock is given up.          */
/*                                                                           */
/* The lock file itself is never unlinked: unlinking it lets a second process */
/* create and lock a NEW file while the first still holds the old inode.      */
/*                                                                           */
/* The lock is not the whole rule. A host from a Liu older than the lock      */
/* holds none, so after taking the lock the node is still PROBED before it is */
/* unlinked (node_host_alive), with a probe that tells a busy host from a    */
/* dead node. The probe is not redundant with the lock.                      */
/* ------------------------------------------------------------------------- */

static pthread_mutex_t g_own_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int   lock_fd;     /* the ONE descriptor of the lock file; -1 = not an owner */
    pid_t pid;         /* who took it — a fork()ed child inherits this struct, not the lock */
    int   listen_fd;   /* the listener bound under it (a number only, never closed here) */
    bool  node_known;  /* node_dev/node_ino describe the node WE bound */
    dev_t node_dev;
    ino_t node_ino;
    char  path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} g_own = { .lock_fd = -1, .listen_fd = -1 };

static void owner_reset(void) {
    g_own.lock_fd    = -1;
    g_own.pid        = 0;
    g_own.listen_fd  = -1;
    g_own.node_known = false;
    g_own.path[0]    = '\0';
}

/* A child made by fork() carries a copy of g_own that says "owner" — and holds
 * no lock, because POSIX locks stay with the parent. Left alone, that child's
 * exit path (atexit runs in it too if it ever calls exit()) would unlink the
 * parent's live node. Closing the inherited descriptor here is harmless for
 * the same reason: it can only drop locks THIS process holds, and it has none. */
static void owner_forget_if_forked(void) {
    if (g_own.lock_fd >= 0 && g_own.pid != getpid()) {
        close(g_own.lock_fd);
        owner_reset();
    }
}

/* Is `fd` (still) a listening AF_UNIX socket bound at `path`? Lets a second
 * listen in the SAME process tell "we are already hosting" from "the listener
 * was closed without notify_socket_release()" — the lock cannot, a process
 * never conflicts with itself. */
static bool fd_is_listener_at(int fd, const char *path) {
    if (fd < 0) return false;
    /* "A socket still bound at this path" is the test. SO_ACCEPTCONN would say
     * "listening" outright, but macOS does not answer it for AF_UNIX
     * (ENOPROTOOPT) — and reading that failure as "not our listener" would
     * make a second listen take the process's own lock over and then drop it.
     * The number can only still name such a socket if it is the one this file
     * bound: nothing else binds at the path. Where the option does exist, a
     * definite "not listening" is believed. */
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) return false;
    int on = 0;
    socklen_t len = sizeof on;
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &on, &len) == 0 && !on) return false;
    struct sockaddr_un sa;
    socklen_t sl = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) return false;
    if (sa.sun_family != AF_UNIX) return false;
    return strncmp(sa.sun_path, path, sizeof sa.sun_path) == 0;
}

/* THE one place the lock file is opened. Call only while g_own.lock_fd < 0.
 *   >= 0 : descriptor holding the write lock
 *   -2   : another process holds it — a live host exists
 *   -1   : hard error (errno set) */
static int owner_lock_take(const char *path) {
    char lock_path[sizeof(((struct sockaddr_un *)0)->sun_path) + 8];
    if ((usize)snprintf(lock_path, sizeof lock_path, "%s.lock", path) >= sizeof lock_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
        close(fd);
        errno = EPERM;
        return -1;
    }

    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;      /* l_start = l_len = 0: the whole file */
    int rc;
    do { rc = fcntl(fd, F_SETLK, &fl); } while (rc < 0 && errno == EINTR);
    if (rc != 0) {
        int e = errno;
        close(fd);               /* safe: this process holds no lock on the file */
        if (e == EAGAIN || e == EACCES) return -2;
        errno = e;
        return -1;
    }
    return fd;
}

/* Unlink the node (only the one this process bound, only if it is still a
 * socket), then give the lock up. That order is the contract: while the node
 * exists, the lock says who answers on it. Caller holds g_own_mu. `path` NULL
 * = whatever this process owns. */
static void owner_drop_locked(const char *path) {
    owner_forget_if_forked();
    if (g_own.lock_fd < 0) return;
    if (path && strcmp(path, g_own.path) != 0) return;

    struct stat st;
    if (lstat(g_own.path, &st) == 0 && S_ISSOCK(st.st_mode) &&
        (!g_own.node_known ||
         (st.st_dev == g_own.node_dev && st.st_ino == g_own.node_ino))) {
        unlink(g_own.path);
    }
    /* else: the node is gone or is no longer this process's (a Liu older than
     * the lock can still bind over a refused probe) — not this process's to
     * remove. */
    close(g_own.lock_fd);
    owner_reset();
}

/* Is a socket of ANOTHER type than ours bound at `path` right now?
 *    1 : yes — some live process has a stream socket bound there
 *    0 : no  — no node, or a node no socket is bound to (its maker is gone)
 *   -1 : this kernel did not give one of the answers below; ask another way
 *
 * A stream connect() cannot answer this: "refused" is what a dead node says,
 * AND what a live listener says while its accept queue is full (macOS) or
 * between its bind() and its listen(). A DATAGRAM socket aimed at the same
 * node can, because the kernel compares socket types before it ever looks at
 * the listen queue: EPROTOTYPE means "a socket is bound here and it is not a
 * datagram one" — i.e. alive, whatever its queue holds — and ECONNREFUSED
 * means nothing is bound at all. On Darwin, bound-not-listening, listening,
 * and listening with a full queue all answer EPROTOTYPE; a closed listener's
 * and a dead process's node answer ECONNREFUSED. Linux's unix_find_bsd()
 * checks in the same order. Nothing is connected by it, so the host never
 * sees the question. */
static int node_bound_probe(const char *path) {
    struct sockaddr_un addr;
    if (!set_addr(&addr, path)) return -1;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    (void)set_cloexec(fd);
    int rc;
    do { rc = connect(fd, (struct sockaddr *)&addr, sizeof addr); }
    while (rc < 0 && errno == EINTR);
    int e = errno;
    close(fd);
    if (rc == 0)           return -1;   /* a datagram socket lives here: not a host, not stale */
    if (e == EPROTOTYPE)   return 1;
    if (e == ECONNREFUSED) return 0;
    if (e == ENOENT)       return 0;
    return -1;
}

#define PROBE_CONNECT_MS  50    /* Linux blocks on a full queue; macOS never waits */
#define PROBE_RETRIES     5     /* fallback only: 5 more tries, 30 ms apart = 150 ms */
#define PROBE_RETRY_MS    30

/* One stream connect to the node:  1 a host answered, 0 nobody can, -1 error,
 * 2 refused (ambiguous — see above). */
static int node_stream_probe(const char *path) {
    int fd = notify_socket_connect_timeout(path, PROBE_CONNECT_MS);
    if (fd >= 0) { close(fd); return 1; }
    if (errno == ENOENT)       return 0;
    /* Only a listener has a queue to wait in. */
    if (errno == ETIMEDOUT)    return 1;
    if (errno == ECONNREFUSED) return 2;
    return -1;
}

/* Does a live host answer at `path`? 1 yes, 0 no, -1 hard error (errno set).
 * Called with the ownership lock already held, before the node is touched. */
static int node_host_alive(const char *path) {
    int bound = node_bound_probe(path);
    if (bound == 1) return 1;

    int st = node_stream_probe(path);
    if (st != 2) return st;

    /* Refused. If the kernel also said nothing is bound there, that is two
     * answers agreeing and one of them cannot lie: stale. */
    if (bound == 0) return 0;

    /* The type probe had no answer on this kernel, so a refusal is all there
     * is — and a busy old host refuses too. Ask again over ~150 ms and believe
     * "stale" only if not one attempt gets through. */
    for (int i = 0; i < PROBE_RETRIES; i++) {
        struct timespec ts = { 0, PROBE_RETRY_MS * 1000 * 1000 };
        nanosleep(&ts, NULL);
        st = node_stream_probe(path);
        if (st != 2) return st;
    }
    return 0;
}

static int listen_locked(const char *path, const struct sockaddr_un *addr) {
    owner_forget_if_forked();

    int lock_fd = -1;
    if (g_own.lock_fd >= 0) {
        bool same = strcmp(g_own.path, path) == 0;
        if (fd_is_listener_at(g_own.listen_fd, g_own.path)) {
            if (same) return -2;         /* the live host is this process */
            errno = EBUSY;               /* one hosted path per process */
            return -1;
        }
        /* This process's listener was closed without a release. The lock is
         * still held, so the node is still this process's to replace. */
        if (same) {
            lock_fd = g_own.lock_fd;
            owner_reset();
        } else {
            owner_drop_locked(NULL);
        }
    }
    if (lock_fd < 0) {
        lock_fd = owner_lock_take(path);
        if (lock_fd < 0) return lock_fd;     /* -2: live host; -1: errno set */
    }

    /* From here every exit that does not end in a listener closes lock_fd. */
    int fd = -1, err = 0, rc = -1;

    /* The lock settles it between two builds that both take it. It says nothing
     * about a host from a Liu OLDER than the lock: that one binds without a
     * lock and would be unlinked right here by the first newer build started
     * beside it — the mixed-version case, which an update with the old app
     * still running produces. So the node is asked too, and this probe is NOT
     * redundant with the lock. */
    int alive = node_host_alive(path);
    if (alive < 0) { err = errno; goto fail; }
    if (alive > 0) { rc = -2; goto fail; }

    /* Stale by proof (this process's by the lock, nobody else's by the probe).
     * Remove it — but only a socket: whatever else sits at the path was not
     * put there by a host and is not this process's to delete. */
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) { err = ENOTSOCK; goto fail; }
        if (unlink(path) < 0 && errno != ENOENT) { err = errno; goto fail; }
    } else if (errno != ENOENT) {
        err = errno;
        goto fail;
    }

    fd = make_socket();
    if (fd < 0) { err = errno; goto fail; }

    /* No umask() around bind(): the in-process server binds from the GUI's main
     * thread with other threads and forks alive, and umask is process-wide, so
     * files and directories another thread created meanwhile would come out
     * with the wrong mode (a 0644 file as 0600 is harmless; a directory asked
     * 0755 arriving 0600 cannot be entered). The node is private because its
     * DIRECTORY is (socket_dir verifies 0700 + owner on every branch); the
     * chmod() below is the second lock on the same door. macOS rejects fchmod()
     * on AF_UNIX fds (EINVAL), hence chmod() by path. */
    if (bind(fd, (const struct sockaddr *)addr, sizeof *addr) < 0) { err = errno; goto fail; }

    if (chmod(path, 0600) < 0) { err = errno; unlink(path); goto fail; }

    /* 64, not 8: every hook of every agent is one pending connection until the
     * serving thread gets to it, and that thread spends up to 500 ms on a
     * client that connected and went quiet. */
    if (listen(fd, 64) < 0) { err = errno; unlink(path); goto fail; }

    g_own.lock_fd   = lock_fd;
    g_own.pid       = getpid();
    g_own.listen_fd = fd;
    g_own.node_known = lstat(path, &st) == 0;
    if (g_own.node_known) { g_own.node_dev = st.st_dev; g_own.node_ino = st.st_ino; }
    snprintf(g_own.path, sizeof g_own.path, "%s", path);
    return fd;

fail:
    if (fd >= 0) close(fd);
    close(lock_fd);
    if (rc == -1) errno = err;
    return rc;
}

int notify_socket_listen(const char *path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    struct sockaddr_un addr;
    if (!set_addr(&addr, path)) { errno = ENAMETOOLONG; return -1; }

    pthread_mutex_lock(&g_own_mu);
    int rc = listen_locked(path, &addr);
    int e  = errno;
    pthread_mutex_unlock(&g_own_mu);
    errno = e;
    return rc;
}

void notify_socket_release(int listen_fd, const char *path) {
    if (listen_fd >= 0) close(listen_fd);
    if (!path || !*path) return;
    pthread_mutex_lock(&g_own_mu);
    owner_drop_locked(path);
    pthread_mutex_unlock(&g_own_mu);
}

void notify_socket_unlink_owned(const char *path) {
    if (!path || !*path) return;
    /* trylock, not lock: this is the atexit path, and exit() can be called
     * from a signal handler that interrupted a thread INSIDE one of the
     * functions above. Skipping is safe — the lock dies with the process and
     * the next host removes the node by right. */
    if (pthread_mutex_trylock(&g_own_mu) != 0) return;
    owner_drop_locked(path);
    pthread_mutex_unlock(&g_own_mu);
}

bool notify_peer_uid_ok(int fd, i32 *out_peer_pid) {
    uid_t me = geteuid();
    if (out_peer_pid) *out_peer_pid = -1;

#if defined(NOTIFY_HAVE_GETPEEREID)
    uid_t peer_uid; gid_t peer_gid;
    if (getpeereid(fd, &peer_uid, &peer_gid) != 0) return false;
    if (peer_uid != me) return false;
    #if defined(__APPLE__) && defined(LOCAL_PEERPID)
    if (out_peer_pid) {
        pid_t pid; socklen_t plen = sizeof pid;
        if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &plen) == 0) {
            /* pid_t is int32_t on Darwin but mask defensively for platforms
             * that may widen it. */
            *out_peer_pid = (i32)((u32)pid & 0x7fffffffu);
        }
    }
    #endif
    return true;

#elif defined(NOTIFY_HAVE_SO_PEERCRED)
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
    if ((uid_t)cred.uid != me) return false;
    /* Linux allows PIDs up to 2^22 by default, but some builds extend to 2^31;
     * mask to keep i32 well-defined. */
    if (out_peer_pid) *out_peer_pid = (i32)((u32)cred.pid & 0x7fffffffu);
    return true;

#else
    (void)fd;
    /* No credential primitive on this platform — fail closed. */
    return false;
#endif
}
