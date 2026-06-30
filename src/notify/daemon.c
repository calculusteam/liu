/*
 * liu-notify - daemon event loop.
 *
 * Single-threaded poll() loop. Each client handled synchronously (accept →
 * read → validate → enqueue → ack → close); frames are small (<4 KB) and
 * infrequent. Signals funnelled through a self-pipe; SIGPIPE ignored.
 * RLIMIT_NOFILE capped to 64. Pidfile held under LOCK_EX|LOCK_NB for life.
 */
#include "notify/protocol.h"
#include "notify/socket.h"
#include "notify/queue.h"
#include "notify/platform_notify.h"
#include "notify/config.h"

#include "notify/notify_server.h"
#include "core/net.h"
#include "core/types.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static int g_sig_pipe[2] = { -1, -1 };

/* ------------------------------------------------------------------------- */
/* monotonic time in seconds                                                 */
/* ------------------------------------------------------------------------- */
static f64 mono_sec(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------------- */
/* signal handling (async-signal-safe via self-pipe)                         */
/* ------------------------------------------------------------------------- */
static volatile sig_atomic_t g_reload = 0;

static void sig_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    g_stop = 1;
    if (g_sig_pipe[1] >= 0) {
        const char b = 'x';
        ssize_t n;
        do { n = write(g_sig_pipe[1], &b, 1); }
        while (n < 0 && errno == EINTR);
        /* On any non-EINTR error (pipe closed during shutdown) we just drop
         * the wakeup — g_stop is already set, loop will notice next tick. */
        (void)n;
    }
    errno = saved_errno;  /* don't perturb interrupted syscall's errno */
}

/* SIGHUP variant: set g_reload (not g_stop) so the main loop re-reads
 * notify.conf without dropping in-flight messages. Wakes the loop the same
 * way as sig_handler. */
static void sig_handler_reload(int sig) {
    (void)sig;
    int saved_errno = errno;
    g_reload = 1;
    if (g_sig_pipe[1] >= 0) {
        const char b = 'x';
        ssize_t n;
        do { n = write(g_sig_pipe[1], &b, 1); } while (n < 0 && errno == EINTR);
        (void)n;
    }
    errno = saved_errno;
}

static bool install_signals(void) {
    if (pipe(g_sig_pipe) != 0) return false;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(g_sig_pipe[i], F_GETFL, 0);
        if (fl >= 0) fcntl(g_sig_pipe[i], F_SETFL, fl | O_NONBLOCK);
        int fd = fcntl(g_sig_pipe[i], F_GETFD, 0);
        if (fd >= 0) fcntl(g_sig_pipe[i], F_SETFD, fd | FD_CLOEXEC);
    }

    struct sigaction ign = { .sa_handler = SIG_IGN };
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGPIPE, &ign, NULL) != 0) return false;

    struct sigaction stop = { .sa_handler = sig_handler };
    sigemptyset(&stop.sa_mask);
    stop.sa_flags = 0;  /* no SA_RESTART — poll() should return EINTR */
    if (sigaction(SIGTERM, &stop, NULL) != 0) return false;
    if (sigaction(SIGINT,  &stop, NULL) != 0) return false;

    /* SIGHUP no longer terminates — it asks the loop to re-read notify.conf
     * so the Settings UI can push sound-rule changes live without restarting
     * the daemon. */
    struct sigaction reload = { .sa_handler = sig_handler_reload };
    sigemptyset(&reload.sa_mask);
    reload.sa_flags = 0;
    if (sigaction(SIGHUP, &reload, NULL) != 0) return false;
    return true;
}

static void drain_sig_pipe(void) {
    char buf[64];
    for (;;) {
        ssize_t n = read(g_sig_pipe[0], buf, sizeof buf);
        if (n > 0) continue;
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* pidfile                                                                   */
/* ------------------------------------------------------------------------- */
static int g_pidfile_fd = -1;
static char g_pidfile_path[256];

static struct {
    dev_t dev;
    ino_t ino;
    bool  have;
} g_pidfile_id;

static bool acquire_pidfile(void) {
    if (!notify_pid_path(g_pidfile_path, sizeof g_pidfile_path)) return false;
    /* O_NOFOLLOW defeats a same-UID symlink attack that would aim ftruncate
     * at an arbitrary file the user cares about. */
    int fd = open(g_pidfile_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }

    /* Remember the inode we locked so release_pidfile can refuse to unlink
     * a swapped file. */
    struct stat st;
    if (fstat(fd, &st) == 0) {
        g_pidfile_id.dev = st.st_dev;
        g_pidfile_id.ino = st.st_ino;
        g_pidfile_id.have = true;
    }

    if (ftruncate(fd, 0) != 0) { close(fd); return false; }
    char line[32];
    int n = snprintf(line, sizeof line, "%ld\n", (long)getpid());
    if (n > 0) {
        ssize_t w;
        do { w = write(fd, line, (size_t)n); } while (w < 0 && errno == EINTR);
        (void)w;
    }
    g_pidfile_fd = fd;
    return true;
}

static void release_pidfile(void) {
    if (g_pidfile_fd >= 0) {
        (void)ftruncate(g_pidfile_fd, 0);
        close(g_pidfile_fd);
        g_pidfile_fd = -1;
    }
    if (g_pidfile_path[0] && g_pidfile_id.have) {
        struct stat st;
        if (lstat(g_pidfile_path, &st) == 0 &&
            S_ISREG(st.st_mode) &&
            st.st_dev == g_pidfile_id.dev &&
            st.st_ino == g_pidfile_id.ino) {
            unlink(g_pidfile_path);
        }
        /* otherwise: path was swapped by another process — leave it alone */
    }
}

/* ------------------------------------------------------------------------- */
/* per-client: read with a single cumulative deadline                        */
/* ------------------------------------------------------------------------- */
#define CLIENT_DEADLINE_MS 500

static i64 mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1000 + (i64)ts.tv_nsec / 1000000;
}

/* Read exactly `want` bytes by the shared `deadline_ms` instant. Returns:
 *     = want : all bytes read
 *     <  want: deadline or EOF reached (partial or zero)
 *     <     0: syscall error (errno set) */
static ssize_t read_until(int fd, u8 *buf, size_t want, i64 deadline_ms) {
    size_t got = 0;
    while (got < want) {
        i64 remain = deadline_ms - mono_ms();
        if (remain <= 0) return (ssize_t)got;
        if (remain > INT32_MAX) remain = INT32_MAX;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)remain);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (pr == 0) return (ssize_t)got;

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            ssize_t r = read(fd, buf + got, want - got);
            if (r > 0) got += (size_t)r;
            return (ssize_t)got;
        }
        if (pfd.revents & POLLIN) {
            ssize_t r = read(fd, buf + got, want - got);
            if (r < 0) { if (errno == EINTR) continue; return -1; }
            if (r == 0) return (ssize_t)got;
            got += (size_t)r;
        }
    }
    return (ssize_t)got;
}

static u64 g_read_err_log_throttle = 0;
static NotifyConfig g_cfg;

/* Returns true if an accept() succeeded (whether or not the frame was valid),
 * false if the listen fd had no pending connection (EAGAIN) or a hard error. */
static bool handle_client(int listen_fd, NotifyQueue *q) {
    int cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        fprintf(stderr, "liu-notify: accept: %s\n", strerror(errno));
        return false;
    }

    /* FD_CLOEXEC + non-blocking on platforms where accept did not inherit them */
    int fl = fcntl(cfd, F_GETFD, 0);
    if (fl >= 0) fcntl(cfd, F_SETFD, fl | FD_CLOEXEC);
    net_set_nonblocking(cfd);

    i32 peer_pid = -1;
    if (!notify_peer_uid_ok(cfd, &peer_pid)) {
        close(cfd);
        return true;
    }

    /* Single cumulative deadline for header + payload read. Prevents a
     * slow-loris client from tying up the daemon for 2× the deadline. */
    i64 deadline = mono_ms() + CLIENT_DEADLINE_MS;

    u8 frame_buf[NOTIFY_MAX_FRAME];
    ssize_t got = read_until(cfd, frame_buf, sizeof(NotifyHeader), deadline);
    if (got < 0) {
        if ((g_read_err_log_throttle++ & 0x3fu) == 0)
            fprintf(stderr, "liu-notify: read(header): %s\n", strerror(errno));
        close(cfd); return true;
    }
    if (got != (ssize_t)sizeof(NotifyHeader)) { close(cfd); return true; }

    /* The first 4 bytes are the frame's magic. Only NOTIFY_MAGIC frames are
     * served; anything else is an unknown/garbage frame and is rejected. */
    u32 maybe_magic = 0;
    memcpy(&maybe_magic, frame_buf, sizeof maybe_magic);
    if (maybe_magic != NOTIFY_MAGIC) { close(cfd); return true; }

    NotifyHeader probe;
    memcpy(&probe, frame_buf, sizeof probe);

    /* Minimal pre-check only to bound the follow-up read() size;
     * notify_validate() is the authoritative validator. */
    if (probe.title_len > NOTIFY_MAX_TITLE ||
        probe.body_len  > NOTIFY_MAX_BODY) {
        u8 nack = NOTIFY_ACK_REJECT;
        (void)write(cfd, &nack, 1);
        close(cfd); return true;
    }
    u32 payload = probe.title_len + probe.body_len;  /* both capped above */

    if (payload > 0) {
        ssize_t pg = read_until(cfd, frame_buf + sizeof(NotifyHeader), payload, deadline);
        if (pg < 0) {
            if ((g_read_err_log_throttle++ & 0x3fu) == 0)
                fprintf(stderr, "liu-notify: read(payload): %s\n", strerror(errno));
            close(cfd); return true;
        }
        if (pg != (ssize_t)payload) { close(cfd); return true; }
    }

    NotifyFrame frame;
    if (!notify_validate(frame_buf, sizeof(NotifyHeader) + payload, &frame)) {
        u8 nack = NOTIFY_ACK_REJECT;
        (void)write(cfd, &nack, 1);
        close(cfd);
        return true;
    }

    /* Config gate: global enable + per-tool enable. Gated frames are acked
     * as DROPPED so the CLI exits 0 (consistent with rate-limit/dedup). */
    if (!g_cfg.enabled || !notify_config_tool_enabled(&g_cfg, frame.header.tool_id)) {
        u8 ack = NOTIFY_ACK_DROPPED;
        (void)write(cfd, &ack, 1);
        close(cfd);
        return true;
    }

    f64 now = mono_sec();
    QueuePushResult pr = queue_push(q, &frame, now);
    u8 ack = (pr == Q_PUSH_OK) ? NOTIFY_ACK_OK : NOTIFY_ACK_DROPPED;
    (void)write(cfd, &ack, 1);
    close(cfd);
    return true;
}

/* ------------------------------------------------------------------------- */
/* main loop                                                                 */
/* ------------------------------------------------------------------------- */
static int listen_fd = -1;

/* Shared poll loop. Assumes the socket is bound + nonblocking (listen_fd), the
 * wake pipe (g_sig_pipe) exists, and tts_init() + config load have already run.
 * Owns the message queue; runs until g_stop.
 * Tears down TTS/sound on exit but leaves socket/pipe/pidfile to the caller.
 * Returns the exit code. */
static int daemon_run_loop(void) {
    /* Static singleton: there is only ever one server per process (single-
     * instance via socket bind, single-threaded loop), so keeping the ~213 KB
     * queue in BSS instead of on the 512 KB server-thread stack is safe and
     * removes stack pressure. queue_init re-initialises it on each (re)start. */
    static NotifyQueue q;
    queue_init(&q, mono_sec());
    queue_set_limits(&q,
                     g_cfg.rate_limit_per_sec,
                     g_cfg.rate_burst,
                     (f64)g_cfg.dedup_window_sec);
    int rc = 0;

    while (!g_stop) {
        struct pollfd pfds[2] = {
            { .fd = listen_fd,     .events = POLLIN, .revents = 0 },
            { .fd = g_sig_pipe[0], .events = POLLIN, .revents = 0 },
        };
        nfds_t poll_count = 2;
        int timeout_ms = (queue_empty(&q) && !tts_busy() && !notify_sound_busy()) ? 500 : 50;
        int pr = poll(pfds, poll_count, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "liu-notify: poll: %s\n", strerror(errno));
            rc = 1;
            break;
        }

        if (pfds[1].revents & POLLIN) {
            drain_sig_pipe();
            if (g_stop) break;
            if (g_reload) {
                g_reload = 0;
                /* Re-load from disk on top of defaults so removed keys revert
                 * to their built-in defaults instead of sticking around. */
                notify_config_defaults(&g_cfg);
                notify_config_load(&g_cfg);
                if (g_cfg.sound_count == 0) notify_config_seed_bundled_sounds(&g_cfg);
                queue_set_limits(&q,
                                 g_cfg.rate_limit_per_sec,
                                 g_cfg.rate_burst,
                                 (f64)g_cfg.dedup_window_sec);
            }
        }

        if (pfds[0].revents & POLLIN) {
            /* Drain: accept's EAGAIN (returned false) is our stop signal.
             * Bounded to avoid a burst starving the speech drain. */
            for (int i = 0; i < 8; i++) {
                if (!handle_client(listen_fd, &q)) break;
            }
        }

        if (!tts_busy() && !notify_sound_busy()) {
            const NotifyMsg *m = queue_peek(&q);
            if (m) {
                /* "Don't notify what the user is already watching" gate.
                 * When Liu is the frontmost app we silently drain the
                 * message — no sound, no banner, no TTS. The whole point
                 * of the daemon is to break the user out of context they
                 * weren't already in. */
                bool suppress = notify_target_active();
                /* Per-event master switch (Settings "On/Off" pill): when the
                 * event is turned off, suppress everything — sound, TTS and
                 * banner alike — exactly like the frontmost-app gate. */
                if (m->event_id < (u8)(sizeof g_cfg.event_enabled /
                                       sizeof g_cfg.event_enabled[0]) &&
                    !g_cfg.event_enabled[m->event_id]) {
                    suppress = true;
                }
                /* Sound file mapped for this (tool, event)? Prefer it over TTS.
                 * Falls back to speech only when no rule matches and the user
                 * hasn't disabled tts_fallback. */
                const char *sound_path = notify_config_sound_for(&g_cfg, m->tool_id, m->event_id);
                bool played = false;
                if (!suppress && sound_path && *sound_path) {
                    played = notify_play_sound(sound_path);
                }
                if (!suppress && !played && g_cfg.tts_fallback) {
                    const char *voice = g_cfg.voice[0] ? g_cfg.voice : NULL;
                    tts_speak(m->text, voice, g_cfg.rate);
                }
                /* event_banner[] is the single source of truth. The legacy
                 * global `desktop_notification` is folded into every slot
                 * at config-load time so existing configs still work, but
                 * we never OR it in here — that's what made a per-event
                 * "false" toggle invisible to the daemon when the user
                 * still had the global set. */
                bool show_banner = !suppress &&
                    (m->event_id < (u8)(sizeof g_cfg.event_banner /
                                       sizeof g_cfg.event_banner[0]) &&
                     g_cfg.event_banner[m->event_id]);
                if (show_banner) {
                    /* title = text up to first colon, body = rest; keeps the
                     * banner short (macOS collapses long titles anyway). */
                    const char *colon = strchr(m->text, ':');
                    if (colon && colon[1] == ' ') {
                        char title_tmp[96];
                        size_t tlen = (size_t)(colon - m->text);
                        if (tlen >= sizeof title_tmp) tlen = sizeof title_tmp - 1;
                        memcpy(title_tmp, m->text, tlen);
                        title_tmp[tlen] = '\0';
                        platform_notify_desktop(title_tmp, colon + 2);
                    } else {
                        platform_notify_desktop("liu-notify", m->text);
                    }
                }
                queue_pop_commit(&q);
            }
        }
    }

    tts_cancel();
    tts_shutdown();
    notify_sound_cancel();
    return rc;
}

/* ========================================================================= *
 * In-process server — hosted by the Liu GUI so notifications work without a
 * separate persistent background daemon. Runs the same poll
 * loop on a dedicated thread; stopped (and joined) when the app shuts down.
 * Single-instance is enforced by the socket bind itself (notify_socket_listen
 * returns -2 if another listener is already up); no pidfile is taken so
 * `liu-notify stop` can never SIGTERM the GUI by reading its PID.
 * ========================================================================= */
static pthread_t g_srv_thread;
static bool      g_srv_running = false;
static char      g_srv_sock_path[256];

static bool srv_make_wake_pipe(void) {
    if (pipe(g_sig_pipe) != 0) return false;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(g_sig_pipe[i], F_GETFL, 0);
        if (fl >= 0) fcntl(g_sig_pipe[i], F_SETFL, fl | O_NONBLOCK);
        int fd = fcntl(g_sig_pipe[i], F_GETFD, 0);
        if (fd >= 0) fcntl(g_sig_pipe[i], F_SETFD, fd | FD_CLOEXEC);
    }
    return true;
}

static void srv_close_wake_pipe(void) {
    if (g_sig_pipe[0] >= 0) { close(g_sig_pipe[0]); g_sig_pipe[0] = -1; }
    if (g_sig_pipe[1] >= 0) { close(g_sig_pipe[1]); g_sig_pipe[1] = -1; }
}

static void srv_wake(void) {
    if (g_sig_pipe[1] < 0) return;
    const char b = 'x';
    ssize_t n;
    do { n = write(g_sig_pipe[1], &b, 1); } while (n < 0 && errno == EINTR);
    (void)n;
}

static void *srv_thread_fn(void *arg) {
    (void)arg;
    /* Init that must live on the serving thread, matching the standalone
     * daemon's single-thread model (the speech synth is created and used by
     * the same thread). */
    tts_init();
    notify_config_defaults(&g_cfg);
    notify_config_load(&g_cfg);
    /* No sound rules configured ⇒ seed the bundled wavs so notifications
     * play a shipped sound rather than falling back to (now default-off) TTS. */
    if (g_cfg.sound_count == 0) notify_config_seed_bundled_sounds(&g_cfg);
    daemon_run_loop();
    return NULL;
}

bool notify_server_start(void) {
    if (g_srv_running) return true;

    /* A client vanishing mid-write must never take the whole GUI down. */
    signal(SIGPIPE, SIG_IGN);

    if (!srv_make_wake_pipe()) return false;

    if (!notify_socket_path(g_srv_sock_path, sizeof g_srv_sock_path)) {
        srv_close_wake_pipe();
        return false;
    }

    listen_fd = notify_socket_listen(g_srv_sock_path);
    if (listen_fd < 0) {
        /* -2: another instance already hosts the service. <0: bind error.
         * Either way we don't host; close the pipe and return quietly. */
        srv_close_wake_pipe();
        return false;
    }
    net_set_nonblocking(listen_fd);

    g_stop = 0;
    g_reload = 0;

    /* The default pthread stack is only ~512 KB on macOS — give the server
     * thread a main-thread-sized 8 MB stack (defense-in-depth alongside the
     * static buffers above). Fall back to the default attr if anything fails. */
    pthread_attr_t attr;
    pthread_attr_t *attrp = NULL;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, 8u * 1024u * 1024u) == 0) attrp = &attr;
    }
    int prc = pthread_create(&g_srv_thread, attrp, srv_thread_fn, NULL);
    if (attrp) pthread_attr_destroy(&attr);
    if (prc != 0) {
        close(listen_fd);
        listen_fd = -1;
        unlink(g_srv_sock_path);
        srv_close_wake_pipe();
        return false;
    }
    g_srv_running = true;
    return true;
}

void notify_server_stop(void) {
    if (!g_srv_running) return;
    g_stop = 1;
    srv_wake();
    pthread_join(g_srv_thread, NULL);
    g_srv_running = false;

    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
        unlink(g_srv_sock_path);
    }
    srv_close_wake_pipe();
}

void notify_server_reload(void) {
    if (!g_srv_running) return;
    g_reload = 1;
    srv_wake();
}

bool notify_server_running(void) {
    return g_srv_running;
}

int notify_daemon_main(void) {
    /* contain fd leak damage */
    struct rlimit rl = { .rlim_cur = 64, .rlim_max = 64 };
    (void)setrlimit(RLIMIT_NOFILE, &rl);

    if (!install_signals()) {
        fprintf(stderr, "liu-notify: signal setup failed\n");
        return 1;
    }

    if (!acquire_pidfile()) {
        /* another daemon live; exit quietly */
        return 0;
    }

    char sock_path[256];
    if (!notify_socket_path(sock_path, sizeof sock_path)) {
        release_pidfile();
        fprintf(stderr, "liu-notify: socket path resolution failed\n");
        return 1;
    }

    listen_fd = notify_socket_listen(sock_path);
    if (listen_fd == -2) {
        /* Another daemon raced ahead of us. Clean exit. */
        release_pidfile();
        return 0;
    }
    if (listen_fd < 0) {
        fprintf(stderr, "liu-notify: listen: %s\n", strerror(errno));
        release_pidfile();
        return 1;
    }

    net_set_nonblocking(listen_fd);
    tts_init();

    /* Load user config (safe no-op if file absent — defaults already set). */
    notify_config_defaults(&g_cfg);
    notify_config_load(&g_cfg);
    /* No sound rules configured ⇒ seed the bundled wavs so notifications
     * play a shipped sound rather than falling back to (now default-off) TTS. */
    if (g_cfg.sound_count == 0) notify_config_seed_bundled_sounds(&g_cfg);

    int rc = daemon_run_loop();

    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
        unlink(sock_path);
    }
    if (g_sig_pipe[0] >= 0) close(g_sig_pipe[0]);
    if (g_sig_pipe[1] >= 0) close(g_sig_pipe[1]);
    release_pidfile();
    return rc;
}
