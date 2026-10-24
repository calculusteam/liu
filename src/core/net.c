/*
 * Liu - shared networking helpers
 */
#include "core/net.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <pwd.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <poll.h>
#include <time.h>

bool net_set_nonblocking(int fd);

static bool net_set_cloexec(int fd) {
    int flags;
    do { flags = fcntl(fd, F_GETFD, 0); }
    while (flags < 0 && errno == EINTR);
    if (flags < 0) return false;
    int rc;
    do { rc = fcntl(fd, F_SETFD, flags | FD_CLOEXEC); }
    while (rc < 0 && errno == EINTR);
    return rc == 0;
}

/* ---- a connect that can be given up on ------------------------------------
 *
 * One detached thread per attempt does the lookup and the connect; the handle
 * is shared between it and whoever asked (two references), so either side may
 * finish first. The connect itself is non-blocking with a deadline, each
 * address getting an equal share of what is left — a first address that
 * swallows SYNs (a dead AAAA route is the usual one) does not cost the whole
 * budget before the next is tried. The socket is handed back BLOCKING, which
 * is what every caller expects. */
struct NetConnect {
    char   host[1025];
    int    port;
    int    timeout_ms;
    void (*wake)(void);
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int    state;              /* 0 running, 1 connected, 2 failed — under mu */
    int    fd;                 /* the won socket until someone takes it — under mu */
    bool   abandoned;          /* under mu */
    int    refs;               /* under mu */
    /* The CALLER's deadline, taken when the attempt began (monotonic ms) and
     * never moved, so a name that will not resolve counts against it too
     * rather than only against the resolver's own timeout. Immutable, so it
     * is read without the lock. */
    f64    deadline_ms;
    bool   counted;            /* holds a slot in g_resolver_inflight */
};

/* Threads inside a hostname lookup. getaddrinfo cannot be interrupted, so an
 * attempt that times out leaves its thread there until the resolver answers;
 * with the resolver down, every attempt is one more such thread. Bounded here,
 * and begin() refuses a non-numeric host past the limit. */
static _Atomic int g_resolver_inflight;

int net_resolver_inflight(void) {
    return atomic_load_explicit(&g_resolver_inflight, memory_order_acquire);
}

static bool net_host_is_numeric(const char *host) {
    unsigned char buf[sizeof(struct in6_addr)];
    return inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1;
}

static f64 net_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (f64)t.tv_sec * 1000.0 + (f64)t.tv_nsec / 1e6;
}

static void net_connect_unref(NetConnect *c) {
    pthread_mutex_lock(&c->mu);
    int left = --c->refs;
    int fd = left == 0 ? c->fd : -1;
    if (left == 0) c->fd = -1;
    pthread_mutex_unlock(&c->mu);
    if (left > 0) return;
    if (fd >= 0) close(fd);            /* won, and nobody came for it */
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    free(c);
}

static bool net_connect_given_up(NetConnect *c) {
    pthread_mutex_lock(&c->mu);
    bool gone = c->abandoned;
    pthread_mutex_unlock(&c->mu);
    return gone;
}

/* One address: connected fd (still non-blocking), or -1. */
static int net_connect_one(NetConnect *c, const struct addrinfo *ai, f64 until_ms) {
    int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) return -1;
    if (!net_set_cloexec(sock) || !net_set_nonblocking(sock)) { close(sock); return -1; }
    int rc;
    do { rc = connect(sock, ai->ai_addr, ai->ai_addrlen); } while (rc < 0 && errno == EINTR);
    if (rc == 0) return sock;
    if (errno != EINPROGRESS) { close(sock); return -1; }
    for (;;) {
        f64 left = until_ms - net_now_ms();
        if (left <= 0.0 || net_connect_given_up(c)) { close(sock); return -1; }
        struct pollfd pfd = { .fd = sock, .events = POLLOUT };
        int pr = poll(&pfd, 1, left > 100.0 ? 100 : (int)left + 1);
        if (pr < 0 && errno != EINTR) { close(sock); return -1; }
        if (pr <= 0) continue;
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) {
            close(sock);
            return -1;
        }
        return sock;
    }
}

static void *net_connect_thread(void *arg) {
    NetConnect *c = (NetConnect *)arg;
    f64 deadline = c->deadline_ms;
    int won = -1;

    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", c->port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    /* The one call here that nothing can interrupt. It is why this is a thread:
     * a caller that stops waiting leaves it to finish alone. Not even started
     * for an attempt that was given up on before this thread got to run. */
    bool resolved = !net_connect_given_up(c) &&
                    getaddrinfo(c->host, port_str, &hints, &res) == 0;
    if (c->counted)
        atomic_fetch_sub_explicit(&g_resolver_inflight, 1, memory_order_acq_rel);
    if (resolved) {
        int total = 0;
        for (const struct addrinfo *ai = res; ai; ai = ai->ai_next) total++;
        int index = 0;
        for (const struct addrinfo *ai = res; ai && won < 0; ai = ai->ai_next, index++) {
            f64 now = net_now_ms();
            if (now >= deadline || net_connect_given_up(c)) break;
            f64 share = (deadline - now) / (f64)(total - index);
            won = net_connect_one(c, ai, now + share);
        }
        freeaddrinfo(res);
    }
    if (won >= 0) {
        /* Restore blocking mode and disable Nagle so single-byte interactive
         * input is sent without waiting for a batch. */
        int flags = fcntl(won, F_GETFL, 0);
        if (flags >= 0) (void)fcntl(won, F_SETFL, flags & ~O_NONBLOCK);
        int one = 1;
        (void)setsockopt(won, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    }

    pthread_mutex_lock(&c->mu);
    c->fd = won;
    c->state = won >= 0 ? 1 : 2;
    void (*wake)(void) = c->abandoned ? NULL : c->wake;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    if (wake) wake();
    net_connect_unref(c);
    return NULL;
}

NetConnect *net_connect_begin(const char *hostname, int port, int timeout_ms,
                              void (*wake)(void)) {
    if (!hostname || !hostname[0] || strnlen(hostname, 1024) >= 1024 ||
        port <= 0 || port > 65535) return NULL;
    NetConnect *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->host, sizeof c->host, "%s", hostname);
    c->port = port;
    c->timeout_ms = timeout_ms > 0 ? timeout_ms : NET_CONNECT_TIMEOUT_MS;
    c->deadline_ms = net_now_ms() + (f64)c->timeout_ms;
    c->wake = wake;
    c->fd = -1;
    c->refs = 2;                       /* the thread's and the caller's */
    if (!net_host_is_numeric(hostname)) {
        /* A name needs the resolver. Refuse rather than add one more thread to
         * a pile that is not draining. */
        int now = atomic_fetch_add_explicit(&g_resolver_inflight, 1, memory_order_acq_rel);
        if (now >= NET_RESOLVER_MAX) {
            atomic_fetch_sub_explicit(&g_resolver_inflight, 1, memory_order_acq_rel);
            free(c);
            return NULL;
        }
        c->counted = true;
    }
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    int rc = pthread_create(&tid, &attr, net_connect_thread, c);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        if (c->counted)
            atomic_fetch_sub_explicit(&g_resolver_inflight, 1, memory_order_acq_rel);
        pthread_mutex_destroy(&c->mu);
        pthread_cond_destroy(&c->cv);
        free(c);
        return NULL;
    }
    return c;
}

int net_connect_poll(NetConnect *c) {
    if (!c) return NET_CONNECT_FAILED;
    pthread_mutex_lock(&c->mu);
    int state = c->state;
    int fd = -1;
    if (state == 1) { fd = c->fd; c->fd = -1; }
    if (state == 0 && net_now_ms() >= c->deadline_ms) {
        /* The caller's time is up whether or not the resolver has answered.
         * Walk away exactly as net_connect_abandon() does: whatever the thread
         * wins later is closed by the last reference, never delivered. */
        c->abandoned = true;
        pthread_mutex_unlock(&c->mu);
        net_connect_unref(c);
        return NET_CONNECT_FAILED;
    }
    pthread_mutex_unlock(&c->mu);
    if (state == 0) return NET_CONNECT_PENDING;
    net_connect_unref(c);
    return state == 1 && fd >= 0 ? fd : NET_CONNECT_FAILED;
}

void net_connect_abandon(NetConnect *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    c->abandoned = true;
    pthread_mutex_unlock(&c->mu);
    net_connect_unref(c);              /* closes a socket that was already won */
}

int net_tcp_connect_ex(const char *hostname, int port, int timeout_ms,
                       const atomic_bool *cancel) {
    NetConnect *c = net_connect_begin(hostname, port, timeout_ms, NULL);
    if (!c) return -1;
    for (;;) {
        pthread_mutex_lock(&c->mu);
        if (c->state == 0) {
            /* Short slices only so `cancel` is noticed — and never longer than
             * what is left of the deadline; the thread signals the moment it
             * is done, so a connect is not made to wait for one. */
            f64 left = c->deadline_ms - net_now_ms();
            long slice_ms = left < 50.0 ? (left > 1.0 ? (long)left : 1) : 50;
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += slice_ms * 1000 * 1000;
            while (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
            (void)pthread_cond_timedwait(&c->cv, &c->mu, &until);
        }
        bool running = c->state == 0;
        pthread_mutex_unlock(&c->mu);
        if (!running) return net_connect_poll(c);
        if (net_now_ms() >= c->deadline_ms) {
            /* The resolver is still inside getaddrinfo and the caller's time is
             * up: leave the lookup to finish alone. */
            net_connect_abandon(c);
            return -1;
        }
        if (cancel && atomic_load_explicit(cancel, memory_order_acquire)) {
            net_connect_abandon(c);
            return -1;
        }
    }
}

int net_tcp_connect(const char *hostname, int port) {
    return net_tcp_connect_ex(hostname, port, NET_CONNECT_TIMEOUT_MS, NULL);
}

bool net_set_nonblocking(int fd) {
    if (fd < 0) return false;
    int flags;
    do { flags = fcntl(fd, F_GETFL, 0); } while (flags < 0 && errno == EINTR);
    if (flags < 0) return false;
    int rc;
    do { rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK); }
    while (rc < 0 && errno == EINTR);
    return rc == 0;
}

#define NET_PATH_CAP 4096
static char g_home_dir[NET_PATH_CAP];
static char g_ssh_dir[NET_PATH_CAP];
static pthread_once_t g_home_once = PTHREAD_ONCE_INIT;
static pthread_once_t g_ssh_once = PTHREAD_ONCE_INIT;

static void net_init_home_dir(void) {
    const char *home = getenv("HOME");
    if (home && home[0] == '/') {
        int n = snprintf(g_home_dir, sizeof g_home_dir, "%s", home);
        if (n >= 0 && (usize)n < sizeof g_home_dir) return;
        g_home_dir[0] = '\0';
    }

    /* HOME may be absent in launchd/system contexts. Never fall back to the
     * shared /tmp directory for private keys; use the account database. */
    struct passwd pw, *result = NULL;
    char scratch[16384];
    if (getpwuid_r(getuid(), &pw, scratch, sizeof scratch, &result) == 0 &&
        result && result->pw_dir && result->pw_dir[0] == '/') {
        int n = snprintf(g_home_dir, sizeof g_home_dir, "%s", result->pw_dir);
        if (n < 0 || (usize)n >= sizeof g_home_dir) g_home_dir[0] = '\0';
    }
}

const char *net_home_dir(void) {
    (void)pthread_once(&g_home_once, net_init_home_dir);
    return g_home_dir;
}

static void net_init_ssh_dir(void) {
    const char *home = net_home_dir();
    if (!home[0]) return;
    int n = snprintf(g_ssh_dir, sizeof g_ssh_dir, "%s/.ssh/", home);
    if (n < 0 || (usize)n >= sizeof g_ssh_dir) g_ssh_dir[0] = '\0';
}

const char *net_ssh_dir(void) {
    (void)pthread_once(&g_ssh_once, net_init_ssh_dir);
    return g_ssh_dir;
}
