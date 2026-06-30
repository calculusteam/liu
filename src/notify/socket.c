/* glibc only exposes `struct ucred` / SO_PEERCRED from <sys/socket.h> when
 * _GNU_SOURCE is defined — must come before any system header include. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

/*
 * liu-notify - AF_UNIX socket helpers with strict safety posture:
 *   - connect()-probe before any unlink() (no kill-the-live-daemon hazard),
 *   - FD_CLOEXEC / SOCK_CLOEXEC on every fd,
 *   - fchmod(fd, 0600) before listen() (belt & braces over dir mode),
 *   - peer UID verification (getpeereid on BSD/macOS, SO_PEERCRED on Linux).
 *
 * See plan.md §5 Security Hardening / Socket.
 */
#include "notify/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

#if defined(__APPLE__)
static bool darwin_user_tmp(char *out, usize out_sz) {
    /* Kernel-provided per-UID dir, mode 0700. Not spoofable via $TMPDIR. */
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, out, out_sz);
    return n > 0 && n <= out_sz;
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
    if (darwin_user_tmp(out, out_sz)) {
        /* Trailing slash already present on macOS; trim for uniformity. */
        size_t l = strlen(out);
        if (l > 0 && out[l - 1] == '/') out[l - 1] = '\0';
        return true;
    }
    return home_run_dir(out, out_sz);
#elif defined(__linux__)
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg == '/') {
        struct stat st;
        if (stat(xdg, &st) == 0 && S_ISDIR(st.st_mode) &&
            st.st_uid == geteuid() && (st.st_mode & 0777) == 0700) {
            if ((usize)snprintf(out, out_sz, "%s", xdg) >= out_sz) return false;
            return true;
        }
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

bool notify_socket_path(char *out, usize out_sz) {
    return build_path(out, out_sz, SOCK_NAME);
}

bool notify_pid_path(char *out, usize out_sz) {
    return build_path(out, out_sz, PID_NAME);
}

static void set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int make_socket(void) {
    int fd;
#if defined(SOCK_CLOEXEC)
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) return fd;
    if (errno != EINVAL && errno != EPROTOTYPE) return -1;
#endif
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) set_cloexec(fd);
    return fd;
}

static bool set_addr(struct sockaddr_un *addr, const char *path) {
    if (strlen(path) >= sizeof addr->sun_path) return false;
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    /* strlen checked above; snprintf guarantees NUL terminate. */
    snprintf(addr->sun_path, sizeof addr->sun_path, "%s", path);
    return true;
}

int notify_socket_connect(const char *path) {
    struct sockaddr_un addr;
    if (!set_addr(&addr, path)) { errno = ENAMETOOLONG; return -1; }

    int fd = make_socket();
    if (fd < 0) return -1;

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int notify_socket_listen(const char *path) {
    /* 1. Probe: is a daemon already listening? */
    int probe = notify_socket_connect(path);
    if (probe >= 0) {
        close(probe);
        return -2;
    }
    if (errno != ECONNREFUSED && errno != ENOENT) return -1;

    /* 2. Safe to remove the stale socket file if any. */
    if (unlink(path) < 0 && errno != ENOENT) return -1;

    /* 3. Bind + chmod + listen.
     * macOS rejects fchmod() on AF_UNIX fds (EINVAL), so chmod() the path.
     * umask(0177) is our primary control — chmod is belt-and-braces. */
    struct sockaddr_un addr;
    if (!set_addr(&addr, path)) { errno = ENAMETOOLONG; return -1; }

    int fd = make_socket();
    if (fd < 0) return -1;

    /* umask() is process-global and NOT thread-safe — this daemon is
     * single-threaded by design, so this is safe. Must run before any
     * future worker thread is spawned. */
    mode_t saved_umask = umask(0177);
    int bind_rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    int bind_err = errno;
    umask(saved_umask);
    if (bind_rc < 0) { close(fd); errno = bind_err; return -1; }

    if (chmod(path, 0600) < 0) {
        int e = errno; unlink(path); close(fd); errno = e; return -1;
    }

    if (listen(fd, 8) < 0) {
        int e = errno; unlink(path); close(fd); errno = e; return -1;
    }

    return fd;
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
