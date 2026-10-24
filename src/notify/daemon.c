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
#include "notify/agent_signal.h"
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
#include <stdatomic.h>

static volatile sig_atomic_t g_stop = 0;
/* The GUI-hosted server is stopped/reloaded from the main thread. Keep those
 * flags separate from signal-handler state so the C memory model sees real
 * atomics instead of a volatile cross-thread data race. */
static atomic_bool g_thread_stop = false;
static atomic_bool g_thread_reload = false;
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

static bool configure_nonblocking_cloexec(int fd) {
    int flags;
    do { flags = fcntl(fd, F_GETFL, 0); }
    while (flags < 0 && errno == EINTR);
    if (flags < 0) return false;
    int rc;
    do { rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK); }
    while (rc < 0 && errno == EINTR);
    if (rc != 0) return false;
    do { flags = fcntl(fd, F_GETFD, 0); }
    while (flags < 0 && errno == EINTR);
    if (flags < 0) return false;
    do { rc = fcntl(fd, F_SETFD, flags | FD_CLOEXEC); }
    while (rc < 0 && errno == EINTR);
    return rc == 0;
}

static void sig_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    g_stop = 1;
    if (g_sig_pipe[1] >= 0) {
        const char b = 'x';
        ssize_t n;
        do { n = write(g_sig_pipe[1], &b, 1); }
        while (n < 0 && errno == EINTR);
        /* On any non-EINTR error (pipe closed during shutdown) the wakeup is
         * just dropped — g_stop is already set, the loop will notice next
         * tick. */
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
        if (!configure_nonblocking_cloexec(g_sig_pipe[i])) {
            close(g_sig_pipe[0]); close(g_sig_pipe[1]);
            g_sig_pipe[0] = g_sig_pipe[1] = -1;
            return false;
        }
    }

    struct sigaction ign = { .sa_handler = SIG_IGN };
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGPIPE, &ign, NULL) != 0) return false;

    struct sigaction stop = { .sa_handler = sig_handler };
    sigemptyset(&stop.sa_mask);
    stop.sa_flags = 0;  /* no SA_RESTART — poll() should return EINTR */
    if (sigaction(SIGTERM, &stop, NULL) != 0) return false;
    if (sigaction(SIGINT,  &stop, NULL) != 0) return false;

    /* SIGHUP does not terminate — it asks the loop to re-read notify.conf so
     * the Settings UI can push sound-rule changes live without restarting
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
    g_pidfile_id.have = false;
    if (!notify_pid_path(g_pidfile_path, sizeof g_pidfile_path)) return false;
    /* `O_NOFOLLOW` prevents a same-UID symlink from redirecting `ftruncate`
     * to an arbitrary file. */
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
        usize off = 0;
        while (off < (usize)n) {
            ssize_t w = write(fd, line + off, (usize)n - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { close(fd); return false; }
            off += (usize)w;
        }
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
/* Publish validated lifecycle frames to the GUI; the standalone daemon has no
 * ring consumer. New senders provide the agent id in metadata. Older v1 frames
 * identify Claude, Codex, or Copilot by tool id and have no cwd; the UI treats
 * a missing cwd as the only pane running that tool. */
static void publish_agent_signal(const NotifyFrame *f) {
    if (!f) return;
    AgentSignal sig = {0};
    sig.event = f->header.event_id;
    if (!notify_meta_get(f, NOTIFY_META_AGENT, sig.agent, sizeof sig.agent)) {
        const char *label = notify_tool_label(f->header.tool_id);
        if (label && strcmp(label, "unknown") != 0)
            snprintf(sig.agent, sizeof sig.agent, "%s", label);
    }
    notify_meta_get(f, NOTIFY_META_CWD,     sig.cwd,     sizeof sig.cwd);
    notify_meta_get(f, NOTIFY_META_SESSION, sig.session, sizeof sig.session);
    /* Which pane, when the hook's environment said so. The buffer is larger
     * than any valid id, so a value notify_meta_get had to cut is still too
     * long to parse: a damaged id reads as "no pane" (0.0), never as another
     * pane's. */
    char pane[NOTIFY_PANE_ID_CAP + 8];
    if (notify_meta_get(f, NOTIFY_META_PANE, pane, sizeof pane))
        notify_pane_id_parse(pane, &sig.pane_pid, &sig.pane_serial);
    agent_signal_push(&sig);
}

/* frame_buf below is read into at hdr_len + every capped section, so the
 * largest legal v2 frame must fit it with no bounds check of its own. */
_Static_assert(sizeof(NotifyHeaderV2) + NOTIFY_MAX_TITLE + NOTIFY_MAX_BODY +
                   NOTIFY_MAX_META <= NOTIFY_MAX_FRAME,
               "largest v2 frame must fit NOTIFY_MAX_FRAME");

/* accept() failing for a reason that is not "nobody is waiting" — the process
 * is out of descriptors (EMFILE), the system is (ENFILE), or out of buffers.
 * The connection that woke poll() is still in the queue on Linux, so the
 * listener stays readable and a loop that just tries again spins a core and
 * writes a log line per turn for as long as the shortage lasts. Instead the
 * listener is left out of poll() for ACCEPT_BACKOFF_MS after each failure and
 * the line is logged once per 64; stop and reload still arrive at once through
 * the wake pipe. (macOS drops the pending connection on a failed accept, so
 * there it is one refused client and one line per client, never a spin.) */
#define ACCEPT_BACKOFF_MS 100
static i64 g_accept_backoff_until_ms = 0;
static u64 g_accept_err_log_throttle = 0;

static bool handle_client(int listen_fd, NotifyQueue *q) {
    int cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        /* That one client gave up between connect() and here; the next is fine. */
        if (errno == EINTR || errno == ECONNABORTED) return true;
        if ((g_accept_err_log_throttle++ & 0x3fu) == 0)
            fprintf(stderr, "liu-notify: accept: %s\n", strerror(errno));
        g_accept_backoff_until_ms = mono_ms() + ACCEPT_BACKOFF_MS;
        return false;
    }

    /* FD_CLOEXEC + non-blocking on platforms where accept did not inherit them */
    if (!configure_nonblocking_cloexec(cfd)) {
        close(cfd);
        return true;
    }

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

    /* The version decides how much header there is: v1 ends at the 24-byte
     * base, v2 appends meta_len. Those 4 bytes MUST be read here, before the
     * payload — reading a v2 frame with the v1 header length mis-slices every
     * field after it (meta_len lands where the title should start) and
     * notify_validate() then rejects a perfectly good frame. */
    if (probe.version < NOTIFY_VERSION_MIN || probe.version > NOTIFY_VERSION) {
        u8 nack = NOTIFY_ACK_REJECT;
        (void)write(cfd, &nack, 1);
        close(cfd); return true;
    }

    size_t hdr_len  = sizeof(NotifyHeader);
    u32    meta_len = 0;
    if (probe.version >= 2u) {
        const size_t extra = sizeof(NotifyHeaderV2) - sizeof(NotifyHeader);
        ssize_t hg = read_until(cfd, frame_buf + sizeof(NotifyHeader), extra, deadline);
        if (hg < 0) {
            if ((g_read_err_log_throttle++ & 0x3fu) == 0)
                fprintf(stderr, "liu-notify: read(header/v2): %s\n", strerror(errno));
            close(cfd); return true;
        }
        if (hg != (ssize_t)extra) { close(cfd); return true; }

        NotifyHeaderV2 probe2;
        memcpy(&probe2, frame_buf, sizeof probe2);
        meta_len = probe2.meta_len;
        hdr_len  = sizeof(NotifyHeaderV2);
    }

    /* Minimal pre-check only to bound the follow-up read() size;
     * notify_validate() is the authoritative validator. */
    if (probe.title_len > NOTIFY_MAX_TITLE ||
        probe.body_len  > NOTIFY_MAX_BODY  ||
        meta_len        > NOTIFY_MAX_META) {
        u8 nack = NOTIFY_ACK_REJECT;
        (void)write(cfd, &nack, 1);
        close(cfd); return true;
    }
    u32 payload = probe.title_len + probe.body_len + meta_len;  /* all capped above */

    if (payload > 0) {
        ssize_t pg = read_until(cfd, frame_buf + hdr_len, payload, deadline);
        if (pg < 0) {
            if ((g_read_err_log_throttle++ & 0x3fu) == 0)
                fprintf(stderr, "liu-notify: read(payload): %s\n", strerror(errno));
            close(cfd); return true;
        }
        if (pg != (ssize_t)payload) { close(cfd); return true; }
    }

    NotifyFrame frame;
    if (!notify_validate(frame_buf, hdr_len + payload, &frame)) {
        u8 nack = NOTIFY_ACK_REJECT;
        (void)write(cfd, &nack, 1);
        close(cfd);
        return true;
    }

    /* Lifecycle first: the GUI's agent state is fed by EVERY hook frame, before
     * — and independently of — the notification config. A user who muted sounds
     * still wants the tab to show that its agent is working, and the frame's
     * meta is the only place that says which pane the agent runs in. */
    publish_agent_signal(&frame);

    /* A start is pure state: the turn began, nothing has happened worth
     * announcing. It never reaches the speech/sound queue. */
    if (frame.header.event_id == EVT_START) {
        u8 ack = NOTIFY_ACK_OK;
        (void)write(cfd, &ack, 1);
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
    /* Static singleton: there is only ever one server per process
     * (single-instance via socket bind, single-threaded loop). Speech text
     * is heap per message; the ring itself is small in BSS so idle
     * footprint stays low. queue_init re-inits on each (re)start and frees
     * any in-flight heap strings. */
    static NotifyQueue q;
    queue_init(&q, mono_sec());
    queue_set_limits(&q,
                     g_cfg.rate_limit_per_sec,
                     g_cfg.rate_burst,
                     (f64)g_cfg.dedup_window_sec);
    g_accept_backoff_until_ms = 0;
    int rc = 0;

    while (!g_stop && !atomic_load_explicit(&g_thread_stop, memory_order_acquire)) {
        struct pollfd pfds[2] = {
            { .fd = listen_fd,     .events = POLLIN, .revents = 0 },
            { .fd = g_sig_pipe[0], .events = POLLIN, .revents = 0 },
        };
        nfds_t poll_count = 2;
        /* Idle means BLOCK, not tick.
         *
         * Nothing in this loop is time-driven when the queue is empty and
         * nothing is playing: the token bucket refills from `now` at push time,
         * dedup windows expire on comparison, and the only two things the loop
         * must notice — a client connecting, and stop / reload — are the two
         * fds already in the set. Every state change that matters writes the
         * wake pipe (srv_wake() follows every g_thread_stop / g_thread_reload
         * store, and the signal handler writes it too), so blocking
         * indefinitely cannot miss one.
         *
         * The 50 ms tick STAYS while TTS or a sound is playing: that is real
         * periodic work — polling those for completion — and it stops as soon
         * as they do. */
        bool idle = queue_empty(&q) && !tts_busy() && !notify_sound_busy();
        int timeout_ms = idle ? -1 : 50;
        if (g_accept_backoff_until_ms) {
            i64 left = g_accept_backoff_until_ms - mono_ms();
            if (left > 0) {
                /* A negative fd is skipped by poll(): the listener is not
                 * looked at until the back-off is over (see handle_client). */
                pfds[0].fd = -1;
                if (timeout_ms < 0 || (i64)timeout_ms > left) timeout_ms = (int)left;
            } else {
                g_accept_backoff_until_ms = 0;
            }
        }
        int pr = poll(pfds, poll_count, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "liu-notify: poll: %s\n", strerror(errno));
            rc = 1;
            break;
        }

        if (pfds[1].revents & POLLIN) {
            drain_sig_pipe();
            if (g_stop || atomic_load_explicit(&g_thread_stop, memory_order_acquire)) break;
            bool thread_reload = atomic_exchange_explicit(&g_thread_reload, false,
                                                          memory_order_acq_rel);
            if (g_reload || thread_reload) {
                g_reload = 0;
                /* The whole config again, from defaults: a key removed from the
                 * file is back at its default afterwards, and that includes the
                 * three queue limits — queue_set_limits() reads a 0 as "the
                 * built-in value", so a deleted `rate_burst` really is undone. */
                notify_config_load_effective(&g_cfg);
                queue_set_limits(&q,
                                 g_cfg.rate_limit_per_sec,
                                 g_cfg.rate_burst,
                                 (f64)g_cfg.dedup_window_sec);
            }
        }

        if (pfds[0].revents & POLLIN) {
            /* Drain: accept's EAGAIN (returned false) ends it. Bounded to
             * avoid a burst starving the speech drain. */
            for (int i = 0; i < 8; i++) {
                if (!handle_client(listen_fd, &q)) break;
            }
        }

        if (!tts_busy() && !notify_sound_busy()) {
            const NotifyMsg *m = queue_peek(&q);
            if (m) {
                /* Suppress notifications while Liu is frontmost. Drain
                 * the message without sound, banner, or TTS. */
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
                 * it is never ORed in here — that would make a per-event
                 * "false" toggle invisible to the daemon while the global
                 * is still set. */
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
 * Single-instance is enforced by the socket's ownership lock
 * (notify_socket_listen returns -2 while another process holds it — see
 * socket.c); no pidfile is taken so `liu-notify stop` can never SIGTERM the
 * GUI by reading its PID.
 * ========================================================================= */
static pthread_t g_srv_thread;
static bool      g_srv_running = false;
static char      g_srv_sock_path[256];

static bool srv_make_wake_pipe(void) {
    if (pipe(g_sig_pipe) != 0) return false;
    for (int i = 0; i < 2; i++) {
        if (!configure_nonblocking_cloexec(g_sig_pipe[i])) {
            close(g_sig_pipe[0]); close(g_sig_pipe[1]);
            g_sig_pipe[0] = g_sig_pipe[1] = -1;
            return false;
        }
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
    /* Load defaults, file settings, and bundled sounds only when sound
     * rules are unconfigured. This is shared with reload and the done
     * panel. */
    notify_config_load_effective(&g_cfg);
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
         * Either way this process does not host; close the pipe and
         * return quietly. */
        srv_close_wake_pipe();
        return false;
    }
    if (!net_set_nonblocking(listen_fd)) {
        notify_socket_release(listen_fd, g_srv_sock_path);
        listen_fd = -1;
        srv_close_wake_pipe();
        return false;
    }

    g_stop = 0;
    g_reload = 0;
    atomic_store_explicit(&g_thread_stop, false, memory_order_release);
    atomic_store_explicit(&g_thread_reload, false, memory_order_release);

    /* The default pthread stack is only ~512 KB on macOS — give the server
     * thread a main-thread-sized 8 MB stack (defense-in-depth alongside the
     * static buffers above). Fall back to the default attr if anything fails. */
    pthread_attr_t attr;
    pthread_attr_t *attrp = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized) {
        if (pthread_attr_setstacksize(&attr, 8u * 1024u * 1024u) == 0) attrp = &attr;
    }
    int prc = pthread_create(&g_srv_thread, attrp, srv_thread_fn, NULL);
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (prc != 0) {
        notify_socket_release(listen_fd, g_srv_sock_path);
        listen_fd = -1;
        srv_close_wake_pipe();
        return false;
    }
    g_srv_running = true;
    return true;
}

void notify_server_stop(void) {
    if (!g_srv_running) return;
    atomic_store_explicit(&g_thread_stop, true, memory_order_release);
    srv_wake();
    pthread_join(g_srv_thread, NULL);
    g_srv_running = false;

    /* Node first, ownership lock second, and only because this process holds
     * the lock: a bare unlink() here could remove the node another instance
     * is serving on. */
    if (listen_fd >= 0) {
        notify_socket_release(listen_fd, g_srv_sock_path);
        listen_fd = -1;
    }
    srv_close_wake_pipe();
}

void notify_server_reload(void) {
    if (!g_srv_running) return;
    atomic_store_explicit(&g_thread_reload, true, memory_order_release);
    srv_wake();
}

bool notify_server_running(void) {
    return g_srv_running;
}

int notify_daemon_main(void) {
    atomic_store_explicit(&g_thread_stop, false, memory_order_release);
    atomic_store_explicit(&g_thread_reload, false, memory_order_release);
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
        /* Another daemon got there first. Clean exit. */
        release_pidfile();
        return 0;
    }
    if (listen_fd < 0) {
        fprintf(stderr, "liu-notify: listen: %s\n", strerror(errno));
        release_pidfile();
        return 1;
    }

    if (!net_set_nonblocking(listen_fd)) {
        notify_socket_release(listen_fd, sock_path);
        listen_fd = -1;
        release_pidfile();
        return 1;
    }
    tts_init();

    /* User config as it takes effect (see notify_config_load_effective). */
    notify_config_load_effective(&g_cfg);

    int rc = daemon_run_loop();

    if (listen_fd >= 0) {
        notify_socket_release(listen_fd, sock_path);
        listen_fd = -1;
    }
    if (g_sig_pipe[0] >= 0) close(g_sig_pipe[0]);
    if (g_sig_pipe[1] >= 0) close(g_sig_pipe[1]);
    release_pidfile();
    return rc;
}
