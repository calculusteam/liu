/*
 * liu-notify - CLI entry + client-side subcommand dispatch.
 *
 *   liu-notify daemon
 *   liu-notify send   --tool=... --event=... [--title=...] [--body=...] [--flags=speak,bell]
 *   liu-notify status
 *   liu-notify stop
 *
 * `send` is what every agent's hook runs, so its contract is the agent's, not
 * ours: it talks to whatever Liu is hosting the socket, spawns nothing, is done
 * inside SEND_DEADLINE_MS however the host behaves, and — in --hook mode —
 * never prints and never exits non-zero. An undeliverable notification is not
 * the agent's problem.
 */
#include "notify/protocol.h"
#include "notify/socket.h"
#include "notify/hook_paths.h"
#include "notify/hook_scan.h"

#include "core/types.h"
#include "core/checked.h"
#include "core/utf8.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

extern int notify_daemon_main(void);   /* daemon.c */

/* ------------------------------------------------------------------------- */
/* debug log                                                                 */
/*                                                                           */
/* A hook fires inside somebody else's TUI: nothing it prints is visible, and */
/* anything it prints to stderr is reported BY that agent as a hook failure.  */
/* So the only way to find out why a notification did or didn't arrive is a   */
/* log. Off unless LIU_NOTIFY_DEBUG is set to something other than "0", and   */
/* every failure inside the logger is swallowed — diagnostics must never be   */
/* the reason a hook starts erroring.                                         */
/* ------------------------------------------------------------------------- */

static bool dbg_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        /* A documented switch of the helper's (its --help names it), not a dev
         * hook: the log holds event names, folders and session ids, no text. */
        const char *v = getenv("LIU_NOTIFY_DEBUG");
        cached = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached == 1;
}

/* Same directory the socket lives in — socket.c owns that decision (macOS
 * per-user tmp, XDG_RUNTIME_DIR, ~/.liu/run) and only exposes it through the
 * paths it builds, so derive the log path from the socket's rather than
 * re-implementing the search and risking a different answer. */
static bool dbg_log_path(char *out, usize cap) {
    char sock[256];
    if (!notify_socket_path(sock, sizeof sock)) return false;
    char *slash = strrchr(sock, '/');
    if (!slash) return false;
    *slash = '\0';
    return (usize)snprintf(out, cap, "%s/notify-debug.log", sock) < cap;
}

static void dbg(const char *fmt, ...) {
    if (!dbg_enabled()) return;
    char path[320];
    if (!dbg_log_path(path, sizeof path)) return;
    /* O_APPEND so concurrent hooks from several agents interleave by line
     * instead of overwriting each other. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;

    char line[1024];
    int n = 0;
    {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d pid=%ld ",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (long)getpid());
        if (n < 0 || (usize)n >= sizeof line) { close(fd); return; }
    }
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (usize)n, fmt, ap);
    va_end(ap);
    if (m < 0) { close(fd); return; }
    usize len = strlen(line);
    if (len + 1 < sizeof line) { line[len++] = '\n'; line[len] = '\0'; }

    usize off = 0;
    while (off < len) {
        ssize_t w = write(fd, line + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        off += (usize)w;
    }
    close(fd);
}

/* ------------------------------------------------------------------------- */
/* argv parsing                                                              */
/* ------------------------------------------------------------------------- */

static const char *arg_value(const char *arg, const char *key) {
    size_t klen = strlen(key);
    if (strncmp(arg, key, klen) != 0) return NULL;
    if (arg[klen] != '=') return NULL;
    return arg + klen + 1;
}

static u8 parse_tool(const char *s) {
    if (!s) return TOOL_CUSTOM;
    if (!strcmp(s, "claude"))  return TOOL_CLAUDE;
    if (!strcmp(s, "copilot")) return TOOL_COPILOT;
    if (!strcmp(s, "codex"))   return TOOL_CODEX;
    if (!strcmp(s, "custom"))  return TOOL_CUSTOM;
    return TOOL_CUSTOM;
}

static u8 parse_event(const char *s) {
    if (!s) return EVT_NOTIFY;
    if (!strcasecmp(s, "start")         || !strcasecmp(s, "Start"))         return EVT_START;
    if (!strcasecmp(s, "stop")          || !strcasecmp(s, "Stop"))          return EVT_STOP;
    if (!strcasecmp(s, "notify")        || !strcasecmp(s, "Notification"))  return EVT_NOTIFY;
    if (!strcasecmp(s, "error"))                                            return EVT_ERROR;
    if (!strcasecmp(s, "complete")      || !strcasecmp(s, "PostToolUse"))   return EVT_COMPLETE;
    return EVT_NOTIFY;
}

static u16 parse_flags(const char *s) {
    if (!s || !*s) return FLAG_SPEAK;
    u16 f = 0;
    const char *p = s;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        if (n == 5 && !strncmp(p, "speak", 5)) f |= FLAG_SPEAK;
        else if (n == 4 && !strncmp(p, "bell", 4)) f |= FLAG_BELL;
        if (!comma) break;
        p = comma + 1;
    }
    return f ? f : FLAG_SPEAK;
}

/* ------------------------------------------------------------------------- */
/* frame send                                                                */
/* ------------------------------------------------------------------------- */

/* The whole exchange — connect, write, wait for the ack, including the one
 * retry — ends by one instant. connect() and the ack read would otherwise
 * block, so a host that has the socket open and is not serving it (stopped
 * under a debugger, wedged) would hold every agent's hook for as long as the
 * agent waits, and Claude Code waits 600 s. */
#define SEND_DEADLINE_MS       2000
/* macOS refuses a connect() to a LIVE listener whose accept queue is full, with
 * the same errno a dead node gives. A refusal is therefore asked again a few
 * times over ~100 ms before it is read as "nobody is hosting". */
#define CONNECT_REFUSED_TRIES  5
#define CONNECT_REFUSED_GAP_MS 25

typedef enum {
    SEND_OK = 0,     /* acked: queued */
    SEND_DECLINED,   /* acked: valid; the host suppressed the notification (duplicate, rate limit, notifications disabled, or tool disabled) */
    SEND_NO_HOST,    /* nothing listening — Liu is not running */
    SEND_CLOSED,     /* the host took the connection and let it go unanswered */
    SEND_REJECTED,   /* acked: the host could not parse the frame */
    SEND_TIMEOUT,    /* the deadline passed */
    SEND_ERROR,      /* anything else (errno says what) */
} SendResult;

static const char *send_result_name(SendResult r) {
    switch (r) {
        case SEND_OK:       return "ok";
        case SEND_DECLINED: return "declined by the host";
        case SEND_NO_HOST:  return "no host";
        case SEND_CLOSED:   return "host closed without answering";
        case SEND_REJECTED: return "frame rejected";
        case SEND_TIMEOUT:  return "timed out";
        default:            return "i/o error";
    }
}

static i64 mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1000 + (i64)ts.tv_nsec / 1000000;
}

/* Wait until `fd` is ready for `events` or the deadline passes.
 * 1 ready, 0 deadline, -1 error. */
static int wait_fd(int fd, short events, i64 deadline_ms) {
    for (;;) {
        i64 left = deadline_ms - mono_ms();
        if (left <= 0) return 0;
        if (left > 60 * 1000) left = 60 * 1000;
        struct pollfd pfd = { .fd = fd, .events = events, .revents = 0 };
        int pr = poll(&pfd, 1, (int)left);
        if (pr > 0) return 1;       /* readable/writable, or HUP/ERR: let the syscall say which */
        if (pr == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

/* Connect to the host, retrying a refusal (see CONNECT_REFUSED_TRIES). The
 * returned descriptor is non-blocking: every later step waits in wait_fd(),
 * against the same deadline. -1 with errno set on failure. */
static int connect_host(const char *path, i64 deadline_ms) {
    for (int attempt = 0; ; attempt++) {
        i64 left = deadline_ms - mono_ms();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        int fd = notify_socket_connect_timeout(path, (i32)left);
        if (fd >= 0) {
            int fl = fcntl(fd, F_GETFL, 0);
            if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
                int e = errno; close(fd); errno = e;
                return -1;
            }
            return fd;
        }
        if (errno != ECONNREFUSED || attempt + 1 >= CONNECT_REFUSED_TRIES) return -1;
        struct timespec ts = { 0, CONNECT_REFUSED_GAP_MS * 1000 * 1000 };
        nanosleep(&ts, NULL);
        errno = ECONNREFUSED;
    }
}

/* 1 all written, 0 deadline, -1 error (EPIPE/ECONNRESET = the host hung up). */
static int write_all(int fd, const void *buf, size_t n, i64 deadline_ms) {
    const u8 *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int r = wait_fd(fd, POLLOUT, deadline_ms);
            if (r <= 0) return r;
            continue;
        }
        if (w == 0) errno = EPIPE;
        return -1;
    }
    return 1;
}

/* Read the host's one-byte answer and say what it means. */
static SendResult read_ack(int fd, i64 deadline_ms) {
    for (;;) {
        u8 ack;
        ssize_t r = read(fd, &ack, 1);
        if (r == 1) {
            if (ack == NOTIFY_ACK_OK)      return SEND_OK;
            if (ack == NOTIFY_ACK_DROPPED) return SEND_DECLINED;
            if (ack == NOTIFY_ACK_REJECT)  return SEND_REJECTED;
            errno = EPROTO;
            return SEND_ERROR;
        }
        if (r == 0) return SEND_CLOSED;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int w = wait_fd(fd, POLLIN, deadline_ms);
            if (w == 0) return SEND_TIMEOUT;
            if (w < 0)  return SEND_ERROR;
            continue;
        }
        return (errno == ECONNRESET || errno == EPIPE) ? SEND_CLOSED : SEND_ERROR;
    }
}

/* Clamp a C string to at most cap bytes without cutting a UTF-8 sequence.
 * Invalid input is truncated immediately before the invalid byte so the
 * daemon's strict validator never rejects a client-created frame. */
static usize utf8_prefix_len(const char *s, usize cap) {
    if (!s) return 0;
    usize n = strlen(s), off = 0;
    if (n > cap) n = cap;
    while (off < n) {
        u32 cp = 0;
        u32 used = utf8_decode((const u8 *)s + off, n - off, &cp);
        if (used == 0 || used > n - off) break;
        off += used;
    }
    return off;
}

static SendResult send_frame(int fd, u8 tool, u8 event, u16 flags, const char *title,
                             const char *body, const char *meta, i64 deadline_ms) {
    usize tlen = utf8_prefix_len(title, NOTIFY_MAX_TITLE);
    usize blen = utf8_prefix_len(body, NOTIFY_MAX_BODY);
    usize mlen = utf8_prefix_len(meta, NOTIFY_MAX_META);

    NotifyHeaderV2 h = {0};
    h.base.magic     = NOTIFY_MAGIC;
    h.base.version   = NOTIFY_VERSION;
    h.base.flags     = flags;
    h.base.tool_id   = tool;
    h.base.event_id  = event;
    h.base.priority  = 0;
    h.base.source    = SRC_CLI;
    h.base.seq       = (u32)time(NULL) ^ (u32)getpid();
    h.base.title_len = (u32)tlen;
    h.base.body_len  = (u32)blen;
    h.meta_len       = (u32)mlen;

    u8 frame[NOTIFY_MAX_FRAME];
    memcpy(frame, &h, sizeof h);
    if (tlen) memcpy(frame + sizeof h, title, tlen);
    if (blen) memcpy(frame + sizeof h + tlen, body, blen);
    if (mlen) memcpy(frame + sizeof h + tlen + blen, meta, mlen);
    size_t total = sizeof h + tlen + blen + mlen;

    int w = write_all(fd, frame, total, deadline_ms);
    if (w == 0) return SEND_TIMEOUT;
    if (w < 0 && errno != EPIPE && errno != ECONNRESET) return SEND_ERROR;
    /* A host that hung up mid-write may still have answered first (a REJECT
     * goes out as soon as the header is read), so the ack is read either way:
     * what is in the buffer survives the close. */
    return read_ack(fd, deadline_ms);
}

/* The notification server is hosted in-process by the Liu GUI (see
 * notify/notify_server.h). The CLI therefore never spawns a detached
 * background daemon: it just connects to whatever Liu is hosting. When Liu is
 * not running there is nothing to talk to, so sends drop silently. */

/* Stop a running daemon, safely. The daemon holds LOCK_EX on the pidfile for
 * its whole lifetime, so the recorded PID is sent SIGTERM only when that lock
 * is held — i.e. a live daemon owns it — and never a recycled PID from a stale
 * pidfile. Returns 1 if a live daemon was signalled (after waiting up to ~1 s
 * for it to exit), 0 if none was running (stale pidfile cleared), -1 on a hard
 * error. */
static int stop_running_daemon(void) {
    char pidpath[256];
    if (!notify_pid_path(pidpath, sizeof pidpath)) return -1;

    int fd = open(pidpath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;   /* no pidfile → nothing to stop */

    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        flock(fd, LOCK_UN);
        close(fd);
        unlink(pidpath);
        return 0;            /* lock free → daemon already dead */
    }
    if (errno != EWOULDBLOCK) { close(fd); return -1; }

    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;

    i32 pid;
    if (!checked_parse_i32(buf, &pid)) return -1;
    if (pid <= 1) return -1;
    if (kill((pid_t)pid, SIGTERM) != 0) return -1;

    /* Wait (≤ ~1 s) for it to release the socket + flock so the follow-up
     * spawn binds a clean socket. */
    for (int i = 0; i < 100; i++) {
        if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 1;
}

/* Connect to whatever is hosting the socket and send one frame, all of it
 * inside `deadline_ms`. Nothing is ever spawned: with no host the notification
 * has nowhere to go and is dropped (SEND_NO_HOST), which is not a failure. */
static SendResult connect_and_send(const char *sock_path, u8 tool, u8 event, u16 flags,
                                   const char *title, const char *body, const char *meta,
                                   i64 deadline_ms) {
    int fd = connect_host(sock_path, deadline_ms);
    if (fd < 0) {
        if (errno == ECONNREFUSED || errno == ENOENT) return SEND_NO_HOST;
        if (errno == ETIMEDOUT) return SEND_TIMEOUT;
        return SEND_ERROR;
    }
    SendResult r = send_frame(fd, tool, event, flags, title, body, meta, deadline_ms);
    int e = errno;
    close(fd);
    errno = e;
    return r;
}

/* ------------------------------------------------------------------------- */
/* cmd: send                                                                 */
/* ------------------------------------------------------------------------- */

static bool json_hex4(const char *p, u32 *out) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        u32 d;
        if      (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A') + 10u;
        else return false;             /* also stops at the buffer's NUL */
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

/* Copy the quoted string value for `key` out of the JSON text `buf` into `out`.
 * Non-destructive (the caller pulls several fields out of one payload), so it
 * copies instead of NUL-terminating in place — a scanner that cut the buffer
 * short would hide every field that came after the first one it found.
 *
 * All or nothing: `out` holds the complete decoded value, or it is EMPTY and
 * the function returns false. Encoders escape freely (Go writes `&` as &,
 * others every non-ASCII letter), and a value decoded only up to its first
 * escape is a real-looking path that is not the agent's —
 * "/Users/me/Masaüstü/proje" would become "/Users/me/Masa" — where falling back
 * to getcwd() is right. So \uXXXX is decoded (surrogate pairs included), and
 * everything that cannot yield the whole value fails clean: an unterminated
 * string, a value that does not fit `cap` (a cut id is a different id), an
 * unpaired surrogate, and any control character — written raw or as \n, \t,
 * \u000a — because these values become lines of the frame's meta block and a
 * newline there would begin a line of its own.
 *
 * A key is the first occurrence of `key` that a colon follows; the same text
 * appearing as a VALUE (`"kind":"cwd"`) is passed over. */
static bool json_str_copy(const char *buf, const char *key, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!buf || !key || !*key) return false;

    const char *p = NULL;
    for (const char *k = strstr(buf, key); k; k = strstr(k + 1, key)) {
        const char *q = k + strlen(key);
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q == ':') { p = q + 1; break; }
    }
    if (!p) return false;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;

    size_t o = 0;
    for (;;) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') break;
        if (c < 0x20 || c == 0x7f) goto fail;      /* includes the NUL of a cut payload */
        if (c != '\\') {
            if (o + 2 > cap) goto fail;
            out[o++] = (char)c;
            p++;
            continue;
        }
        u32 cp;
        p++;
        if (*p == '"' || *p == '\\' || *p == '/') {
            cp = (u32)(unsigned char)*p++;
        } else if (*p == 'u') {
            u32 hi;
            if (!json_hex4(p + 1, &hi)) goto fail;
            p += 5;
            if (hi >= 0xD800u && hi <= 0xDBFFu) {
                u32 lo;
                if (p[0] != '\\' || p[1] != 'u' || !json_hex4(p + 2, &lo) ||
                    lo < 0xDC00u || lo > 0xDFFFu) goto fail;
                p += 6;
                cp = 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (hi >= 0xDC00u && hi <= 0xDFFFu) {
                goto fail;                          /* low half with no high half */
            } else {
                cp = hi;
            }
        } else {
            goto fail;                              /* \n \r \t \b \f, or not JSON */
        }
        if (cp < 0x20u || cp == 0x7fu) goto fail;
        u8 enc[4];
        u32 n = utf8_encode(cp, enc);
        if (n == 0 || o + n + 1 > cap) goto fail;
        memcpy(out + o, enc, n);
        o += n;
    }
    out[o] = '\0';
    if (o > 0) return true;
fail:
    out[0] = '\0';
    return false;
}

/* What a hook's stdin JSON says about the event and where it happened. */
typedef struct {
    char event[64];      /* hook event name, verbatim */
    char cwd[512];       /* directory the agent runs in — attributes the hook to a pane */
    char session[128];   /* agent's own session id, when the payload carries one */
} HookPayload;

/* Read the agent's hook JSON from stdin (bounded) into `hp`. Returns false when
 * there is no event name to act on; cwd/session stay empty when absent (they
 * are identity hints for the GUI, never a gate on notifying). Handles both
 * naming conventions: Claude/Codex use snake_case "hook_event_name", Grok uses
 * camelCase "hookEventName". liu-notify links no JSON parser by design, so this
 * is a minimal field scanner — these values are bare ASCII in practice. */
static bool read_hook_payload(HookPayload *hp) {
    if (!hp) return false;
    hp->event[0] = hp->cwd[0] = hp->session[0] = '\0';

    /* Never block on an interactive terminal — `--hook` always runs with
     * the agent's JSON piped to stdin, but guard the manual-invocation case. */
    if (isatty(STDIN_FILENO)) return false;

    static char buf[16384];
    size_t off = 0;
    for (;;) {
        ssize_t r = read(STDIN_FILENO, buf + off, sizeof buf - 1 - off);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        off += (size_t)r;
        if (off >= sizeof buf - 1) break;   /* event name is near the top */
    }
    buf[off] = '\0';
    if (off == 0) return false;

    if (!json_str_copy(buf, "\"hook_event_name\"", hp->event, sizeof hp->event))
        json_str_copy(buf, "\"hookEventName\"", hp->event, sizeof hp->event);

    json_str_copy(buf, "\"cwd\"", hp->cwd, sizeof hp->cwd);
    if (!hp->cwd[0]) json_str_copy(buf, "\"workspace\"", hp->cwd, sizeof hp->cwd);

    if (!json_str_copy(buf, "\"session_id\"", hp->session, sizeof hp->session))
        json_str_copy(buf, "\"sessionID\"", hp->session, sizeof hp->session);

    return hp->event[0] != '\0';
}

/* Normalize a hook event name for matching: lowercase, drop separators, so
 * "SubagentStop", "subagent_stop" and "subagent-stop" all compare equal. */
static void normalize_event_name(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '_' || c == '-' || c == ' ') continue;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* Append `key` + `val` to the frame's meta block as ONE WHOLE LINE, or not at
 * all. A line cut to fit would still parse — as a different directory, a
 * different session, another pane's id — so a value that does not fit, is not
 * valid UTF-8 (the frame builder would cut the block at the bad byte) or
 * carries a control character (a newline would start a line of its own) is
 * left out and the reader sees "unknown" instead of something wrong. */
static void meta_add(char *meta, usize cap, usize *len, const char *key, const char *val) {
    if (!val || !*val) return;
    usize klen = strlen(key), vlen = strlen(val);
    for (usize i = 0; i < vlen; i++) {
        unsigned char c = (unsigned char)val[i];
        if (c < 0x20 || c == 0x7f) return;
    }
    if (utf8_prefix_len(val, vlen) != vlen) return;
    usize sep = *len ? 1 : 0;
    if (*len + sep + klen + vlen > cap) return;
    if (sep) meta[(*len)++] = '\n';
    memcpy(meta + *len, key, klen); *len += klen;
    memcpy(meta + *len, val, vlen); *len += vlen;
    meta[*len] = '\0';
}

static int cmd_send(int argc, char **argv) {
    const char *tool_s = NULL, *event_s = NULL, *title = "", *body = "", *flags_s = NULL;
    bool hook = false;

    /* The host may close its end while the frame is still being written (its
     * read deadline is 500 ms and it serves one client at a time). With the
     * default disposition that write kills this process with SIGPIPE — and a
     * hook killed by a signal is a hook failure in the agent's eyes. */
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < argc; i++) {
        const char *v;
        if ((v = arg_value(argv[i], "--tool"))  ) tool_s  = v;
        else if ((v = arg_value(argv[i], "--event")) ) event_s = v;
        else if ((v = arg_value(argv[i], "--title")) ) title   = v;
        else if ((v = arg_value(argv[i], "--body"))  ) body    = v;
        else if ((v = arg_value(argv[i], "--flags")) ) flags_s = v;
        else if (!strcmp(argv[i], "--hook")          ) hook    = true;
    }

    u8  tool  = parse_tool(tool_s);
    u8  event = parse_event(event_s);
    u16 flags = parse_flags(flags_s);

    /* Hook mode: the agent (Claude / Codex / Grok) pipes its event JSON to
     * liu-notify. Decide what (if anything) to fire from the *actual* event,
     * so subagent completions never notify — only the top-level agent's Stop
     * does. This is the single point that keeps finished subagents from
     * notifying, normalized across every agent's naming:
     *   - SubagentStop  → suppress entirely (a subagent/Task finished, not
     *                     the chat). Liu does not register this hook either,
     *                     so this is the second, payload-side guard.
     *   - Stop          → top-level turn ended → completion notification.
     *   - Notification  → the agent needs attention (permission / idle).
     * A missing/garbled payload falls back to the --event arg. */
    HookPayload hp = {0};
    if (hook) {
        /* WHITELIST: the agent pipes its event JSON on stdin. ONLY a confirmed
         * top-level Stop (→ completion), a Notification (→ attention) or a
         * UserPromptSubmit (→ the turn STARTED) is allowed through. Every other
         * event — SubagentStop, PostToolUse, SessionStart, any
         * tool/process/subagent completion, any unknown name — and any
         * missing/garbled/empty payload is dropped. This is the single gate
         * guaranteeing subagent/tool/process completions never notify; it also
         * fails CLOSED instead of falling back to the verbatim --event arg,
         * which would let every non-matched event through.
         *
         * EVT_START is a state signal, not a notification: the daemon routes it
         * to the GUI's agent state and never speaks or plays a sound for it. */
        if (!read_hook_payload(&hp)) {
            dbg("send tool=%s hook=1 -> DROP (unreadable/empty payload)",
                tool_s ? tool_s : "?");
            return 0;                              /* unreadable payload → drop */
        }
        char norm[64];
        normalize_event_name(hp.event, norm, sizeof norm);
        if (!strcmp(norm, "stop"))                   event = EVT_COMPLETE;
        else if (!strcmp(norm, "notification"))      event = EVT_NOTIFY;
        else if (!strcmp(norm, "userpromptsubmit"))  event = EVT_START;
        else {
            dbg("send tool=%s hook=1 payload-event=%s -> DROP (not whitelisted)",
                tool_s ? tool_s : "?", hp.event);
            return 0;                              /* subagentstop + all else → drop */
        }
        /* cwd + session identify WHICH agent run produced this, which the
         * --tool= label alone does not: a CLI that also reads another
         * agent's settings file will run that agent's hook and report its
         * label. Without these two fields the log cannot tell the two apart. */
        dbg("send tool=%s hook=1 payload-event=%s -> wire-event=%u cwd=%s session=%s",
            tool_s ? tool_s : "?", hp.event, (unsigned)event,
            hp.cwd[0] ? hp.cwd : "-", hp.session[0] ? hp.session : "-");
    } else {
        dbg("send tool=%s event=%s (no --hook)",
            tool_s ? tool_s : "?", event_s ? event_s : "?");
    }

    /* Identity for the GUI: which pane this hook was started in, which agent it
     * was (the wire tool id collapses everything but claude/copilot/codex into
     * TOOL_CUSTOM), which agent session it belongs to and which directory it
     * fired in. The hook payload wins for the last two; agents that run
     * liu-notify without one (the OpenCode plugin, Antigravity) still run it in
     * the agent's own cwd, so getcwd() is the right fallback.
     *
     * Short, bounded lines first and the path last: the block is capped, every
     * line goes in whole or not at all (meta_add), and a 500-byte cwd must not
     * be what pushes the pane id out. */
    char meta[NOTIFY_MAX_META + 1];
    {
        usize n = 0;
        meta[0] = '\0';

        /* The pane's shell exported LIU_PANE_ID; the agent and its hooks
         * inherit it. Anything but a well-formed id is treated as absent —
         * the variable is plain environment, and a damaged id must not be
         * forwarded as if it named a pane. Re-printed from the parsed numbers
         * so what goes on the wire is the canonical text. */
        u32 pane_pid = 0, pane_serial = 0;
        if (notify_pane_id_parse(getenv(NOTIFY_PANE_ENV), &pane_pid, &pane_serial)) {
            char pane[NOTIFY_PANE_ID_CAP];
            snprintf(pane, sizeof pane, "%u.%u", pane_pid, pane_serial);
            meta_add(meta, NOTIFY_MAX_META, &n, NOTIFY_META_PANE, pane);
        }

        if (tool_s && *tool_s) {
            char agent[32];              /* AGENT_SIGNAL_AGENT_CAP: all a reader keeps */
            snprintf(agent, sizeof agent, "%s", tool_s);
            agent[utf8_prefix_len(agent, sizeof agent - 1)] = '\0';
            meta_add(meta, NOTIFY_MAX_META, &n, NOTIFY_META_AGENT, agent);
        }

        meta_add(meta, NOTIFY_MAX_META, &n, NOTIFY_META_SESSION, hp.session);

        char cwd[512];
        if (!hp.cwd[0]) {
            if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
        } else {
            snprintf(cwd, sizeof cwd, "%s", hp.cwd);
        }
        meta_add(meta, NOTIFY_MAX_META, &n, NOTIFY_META_CWD, cwd);
    }

    char sock_path[256];
    if (!notify_socket_path(sock_path, sizeof sock_path)) {
        dbg("send -> FAIL (socket path resolution)");
        /* Same rule as the end of this function: a hook never fails its agent
         * and never writes to the agent's stderr. */
        if (hook) return 0;
        fprintf(stderr, "liu-notify: socket path resolution failed\n");
        return 74;
    }

    i64 deadline = mono_ms() + SEND_DEADLINE_MS;
    SendResult r = connect_and_send(sock_path, tool, event, flags, title, body, meta, deadline);

    /* Reachable, and the frame was not taken. Two causes worth one more try:
     * the host could not parse it — in practice a standalone daemon left over
     * from an older build whose protocol predates this client, which rejects
     * for its whole lifetime, so it is stopped first (stop_running_daemon only
     * ever signals a process that holds the daemon's pidfile lock; the GUI
     * holds none) — or the host let the connection go unanswered, which is
     * what its 500 ms read deadline does to a client that was descheduled
     * between connect() and write(). The retry shares the deadline above; the
     * only time spent outside it is the wait (up to 1 s) for a stale daemon
     * that really was signalled to exit, which happens once per such daemon. */
    if (r == SEND_REJECTED || r == SEND_CLOSED) {
        dbg("send -> %s, stopping any stale daemon and retrying once", send_result_name(r));
        stop_running_daemon();
        r = connect_and_send(sock_path, tool, event, flags, title, body, meta, deadline);
    }

    switch (r) {
    case SEND_OK:
        dbg("send -> ok (sock=%s)", sock_path);
        return 0;
    case SEND_NO_HOST:
        dbg("send -> dropped, no host (sock=%s)", sock_path);
        return 0;
    case SEND_DECLINED:
    case SEND_CLOSED:
        /* The host's own decision — a duplicate, the rate limit, notifications
         * or this tool switched off, or a connection it gave up on — is not a
         * failure of the sender. This matters most without --hook: OpenCode
         * and Antigravity are installed that way (their payload is not
         * Claude's JSON), and a frame the host turned away must not be
         * reported as a failed hook. */
        dbg("send -> %s (sock=%s)", send_result_name(r), sock_path);
        return 0;
    default:
        break;
    }

    dbg("send -> FAIL: %s (sock=%s)", send_result_name(r), sock_path);
    /* Hook mode has no stderr the user can read — it belongs to the agent's
     * TUI, which reports anything written there (and any non-zero status) as a
     * hook failure. An undeliverable notification is not the agent's problem,
     * so a hook always ends quiet and successful; the reason is in the debug
     * log. Only a `liu-notify send` run without --hook reports what is left:
     * a frame the host cannot parse, a host that does not answer in time, a
     * socket error. */
    if (hook) return 0;
    fprintf(stderr, "liu-notify: send failed (%s)\n", send_result_name(r));
    return 74;
}

/* ------------------------------------------------------------------------- */
/* cmd: status                                                               */
/* ------------------------------------------------------------------------- */

static int cmd_status(void) {
    char sock_path[256];
    if (!notify_socket_path(sock_path, sizeof sock_path)) return 74;

    /* Through connect_host so a busy host is not reported as absent (a full
     * accept queue refuses on macOS). */
    int fd = connect_host(sock_path, mono_ms() + 500);
    if (fd >= 0) { close(fd); printf("running\n"); return 0; }
    printf("not running\n");
    return 1;
}

/* ------------------------------------------------------------------------- */
/* cmd: doctor                                                               */
/*                                                                           */
/* Answers the one question a leftover hook raises: WHICH agent still has a   */
/* Liu hook, and does the command it records still exist? A hook whose binary */
/* is gone is what makes an agent print a failure in a terminal that has      */
/* nothing to do with Liu.                                                    */
/*                                                                           */
/* Deliberately a text scan, not a parse: this binary links neither cJSON nor */
/* the hook installers, and every surface (JSON, TOML, JS) records the        */
/* command as a plain absolute path, so finding it needs no schema. Being     */
/* schema-free also means it still finds hooks written by an OLDER Liu whose  */
/* layout this build no longer knows.                                         */
/* ------------------------------------------------------------------------- */

/* The scan itself lives in notify/hook_scan.h so Settings › Diagnostics can
 * run exactly the same one from inside Liu. What stays here is the PRINTING
 * — doctor is the terminal-shaped view of that report. */

/* Returns the number of STALE hooks found (command path not executable). */
static int doctor_scan_agent(const char *label, const char *rel_path,
                             const char *home) {
    HookScanResult r;
    hook_scan_file(home, rel_path, &r);

    if (!r.file_present) { printf("  %-12s no hook file\n", label); return 0; }
    if (r.read_failed)   { printf("  %-12s file exists, unreadable\n", label); return 0; }
    if (r.count == 0)    { printf("  %-12s file exists, no Liu hook\n", label); return 0; }

    for (i32 i = 0; i < r.count; i++)
        printf("  %-12s %s\n               %s\n", label,
               r.cmd_ok[i] ? "installed" : "STALE — command not executable",
               r.cmd[i]);
    if (r.overflow)
        printf("  %-12s (more entries than this report lists)\n", label);
    return r.stale;
}

static int cmd_doctor(void) {
    char home[512];
    if (!hook_scan_home(home, sizeof home)) {
        fprintf(stderr, "liu-notify: $HOME not set\n");
        return 74;
    }

    printf("liu-notify doctor\n\n");

    char sock[256];
    if (notify_socket_path(sock, sizeof sock)) {
        int fd = connect_host(sock, mono_ms() + 500);
        bool live = (fd >= 0);
        if (fd >= 0) close(fd);
        printf("service      %s\n             %s\n",
               live ? "running (Liu is up)"
                    : "not running — notifications are dropped silently", sock);
    } else {
        printf("service      socket path unresolved\n");
    }

    char logp[320];
    if (dbg_log_path(logp, sizeof logp))
        printf("debug log    %s%s\n", logp,
               dbg_enabled() ? " (LIU_NOTIFY_DEBUG on)"
                             : " (set LIU_NOTIFY_DEBUG=1 to record)");

    /* The shim every hook should point at. */
    {
        char shim[1024];
        if ((usize)snprintf(shim, sizeof shim, "%s/" LIU_HOOK_SHIM_REL, home) < sizeof shim) {
            if (access(shim, X_OK) == 0) {
                char target[512] = "?";
                hook_scan_shim_target(shim, target, sizeof target);
                printf("shim         ok — %s\n             -> %s%s\n", shim,
                       target,
                       access(target, X_OK) == 0 ? "" : "  (TARGET MISSING)");
            } else {
                printf("shim         absent (%s)\n", shim);
            }
        }
    }

    printf("\nagent hooks\n");
    int stale = 0;
    stale += doctor_scan_agent("claude",      LIU_HOOK_CLAUDE_REL,      home);
    stale += doctor_scan_agent("commandcode", LIU_HOOK_COMMANDCODE_REL, home);
    stale += doctor_scan_agent("grok",        LIU_HOOK_GROK_REL,        home);
    stale += doctor_scan_agent("opencode",    LIU_HOOK_OPENCODE_REL,    home);
    stale += doctor_scan_agent("codex",       LIU_HOOK_CODEX_REL,       home);
    stale += doctor_scan_agent("antigravity", LIU_HOOK_ANTIGRAVITY_REL, home);
    stale += doctor_scan_agent("pi",          LIU_HOOK_PI_REL,          home);
    stale += doctor_scan_agent("omp",         LIU_HOOK_OMP_REL,         home);

    if (stale > 0) {
        printf("\n%d stale hook(s). Launching Liu once rewrites every hook to the\n"
               "shim above, which exits quietly when Liu is gone; quitting Liu\n"
               "normally then removes them.\n", stale);
        return 1;
    }
    printf("\nno stale hooks.\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* cmd: stop                                                                 */
/* ------------------------------------------------------------------------- */

static int cmd_stop(void) {
    int r = stop_running_daemon();
    if (r > 0) return 0;
    if (r == 0) {
        fprintf(stderr, "liu-notify: daemon was not running\n");
        return 1;
    }
    fprintf(stderr, "liu-notify: stop failed\n");
    return 1;
}

/* ------------------------------------------------------------------------- */
/* usage                                                                     */
/* ------------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  liu-notify daemon\n"
        "  liu-notify send --tool=<claude|copilot|codex|custom> --event=<start|stop|notify|error|complete>\n"
        "                  [--title=TEXT] [--body=TEXT] [--flags=speak[,bell]]\n"
        "  liu-notify status\n"
        "  liu-notify stop\n"
        "  liu-notify doctor      diagnose the service and any leftover agent hooks\n"
        "\n"
        "  LIU_NOTIFY_DEBUG=1     log every hook decision next to the socket\n");
}

int main(int argc, char **argv) {
    /* Force the C locale early — strcasecmp() is locale-dependent, and the
     * Turkish locale famously makes 'i' vs 'I' non-ASCII-folding.  All event
     * name parsing below must remain ASCII-case-insensitive regardless of the
     * user's LC_CTYPE. */
    setlocale(LC_ALL, "C");

    if (argc < 2) { usage(); return 64; }

    const char *cmd = argv[1];
    if (!strcmp(cmd, "daemon")) return notify_daemon_main();
    if (!strcmp(cmd, "send"))   return cmd_send(argc - 2, argv + 2);
    if (!strcmp(cmd, "status")) return cmd_status();
    if (!strcmp(cmd, "stop"))   return cmd_stop();
    if (!strcmp(cmd, "doctor")) return cmd_doctor();
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) { usage(); return 0; }

    usage();
    return 64;
}
