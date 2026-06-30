/*
 * liu-notify - CLI entry + client-side subcommand dispatch.
 *
 *   liu-notify daemon
 *   liu-notify send   --tool=... --event=... [--title=...] [--body=...] [--flags=speak,bell]
 *   liu-notify status
 *   liu-notify stop
 *
 * `send` tries connect() first; on ECONNREFUSED/ENOENT it spawns the daemon
 * via double-fork (under a flock on the pidfile to avoid multiple simultaneous
 * spawners) and polls for socket readiness with exponential backoff.
 */
#include "notify/protocol.h"
#include "notify/socket.h"

#include "core/types.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
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

static bool write_all(int fd, const void *buf, size_t n) {
    const u8 *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static ssize_t read_some(int fd, void *buf, size_t n) {
    for (;;) {
        ssize_t r = read(fd, buf, n);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return -1;
    }
}

static int send_frame(int fd, u8 tool, u8 event, u16 flags, const char *title, const char *body) {
    size_t tlen = title ? strlen(title) : 0;
    size_t blen = body  ? strlen(body)  : 0;
    if (tlen > NOTIFY_MAX_TITLE) tlen = NOTIFY_MAX_TITLE;
    if (blen > NOTIFY_MAX_BODY)  blen = NOTIFY_MAX_BODY;

    NotifyHeader h = {0};
    h.magic     = NOTIFY_MAGIC;
    h.version   = NOTIFY_VERSION;
    h.flags     = flags;
    h.tool_id   = tool;
    h.event_id  = event;
    h.priority  = 0;
    h.source    = SRC_CLI;
    h.seq       = (u32)time(NULL) ^ (u32)getpid();
    h.title_len = (u32)tlen;
    h.body_len  = (u32)blen;

    u8 frame[NOTIFY_MAX_FRAME];
    memcpy(frame, &h, sizeof h);
    if (tlen) memcpy(frame + sizeof h, title, tlen);
    if (blen) memcpy(frame + sizeof h + tlen, body, blen);
    size_t total = sizeof h + tlen + blen;

    if (!write_all(fd, frame, total)) return -1;

    /* read one-byte ack so we know the daemon at least validated. */
    u8 ack;
    ssize_t r = read_some(fd, &ack, 1);
    if (r != 1) return -1;
    if (ack == NOTIFY_ACK_OK || ack == NOTIFY_ACK_DROPPED) return 0;
    return -1;  /* NOTIFY_ACK_REJECT → caller's frame was malformed */
}

/* The notification server is now hosted in-process by the Liu GUI (see
 * notify/notify_server.h). The CLI therefore never spawns a detached
 * background daemon: it just connects to whatever Liu is hosting. When Liu is
 * not running there is nothing to talk to, so sends drop silently. */

/* Stop a running daemon, safely. The daemon holds LOCK_EX on the pidfile for
 * its whole lifetime, so we only SIGTERM the recorded PID when that lock is
 * held — i.e. a live daemon owns it — and never a recycled PID from a stale
 * pidfile. Returns 1 if a live daemon was signalled (and we waited for it to
 * exit), 0 if none was running (stale pidfile cleared), -1 on a hard error. */
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

    long pid = strtol(buf, NULL, 10);
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

/* Connect to the daemon (spawning one if none is listening) and send a frame.
 * Returns send_frame's result: 0 = queued/dropped (success), nonzero = the
 * daemon rejected the frame or the I/O failed. */
static int connect_and_send(const char *sock_path, u8 tool, u8 event, u16 flags,
                            const char *title, const char *body) {
    int fd = notify_socket_connect(sock_path);
    if (fd < 0) {
        /* No server listening (Liu not running) → drop the notification
         * silently. We never spawn a background daemon. Other connect errors
         * are real failures. */
        if (errno == ECONNREFUSED || errno == ENOENT) return 0;
        return -1;
    }
    int rc = send_frame(fd, tool, event, flags, title, body);
    close(fd);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* cmd: send                                                                 */
/* ------------------------------------------------------------------------- */

/* Extract a quoted string value for `key` from the JSON text `buf`.
 * Returns a pointer into buf (NUL-terminated in place) or NULL. */
static char *json_str_value(char *buf, const char *key) {
    char *k = strstr(buf, key);
    if (!k) return NULL;
    char *p = k + strlen(key);
    while (*p && *p != ':') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    char *start = p;
    while (*p && *p != '"') p++;
    if (*p != '"') return NULL;
    *p = '\0';
    return start;
}

/* Read the agent's hook JSON from stdin (bounded) and return the hook
 * event-name value, or NULL if absent. Handles both naming conventions:
 * Claude/Codex use snake_case "hook_event_name", Grok uses camelCase
 * "hookEventName". liu-notify links no JSON parser by design, so this is a
 * minimal single-field scanner — hook event names are bare ASCII, never
 * escaped. The returned pointer is into a static buffer valid for the call. */
static const char *read_hook_event_name(void) {
    /* Never block on an interactive terminal — `--hook` always runs with
     * the agent's JSON piped to stdin, but guard the manual-invocation case. */
    if (isatty(STDIN_FILENO)) return NULL;

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
    if (off == 0) return NULL;

    char *v = json_str_value(buf, "\"hook_event_name\"");   /* Claude, Codex */
    if (!v) v = json_str_value(buf, "\"hookEventName\"");    /* Grok */
    return v;
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

static int cmd_send(int argc, char **argv) {
    const char *tool_s = NULL, *event_s = NULL, *title = "", *body = "", *flags_s = NULL;
    bool hook = false;
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
     * us. Decide what (if anything) to fire from the *actual* event, so
     * subagent completions never notify — only the top-level agent's Stop
     * does. This is the single point that breaks the "every finished
     * subagent dings" spam, normalized across every agent's naming:
     *   - SubagentStop  → suppress entirely (a subagent/Task finished, not
     *                     the chat). We also don't register this hook, so
     *                     this is the belt-and-suspenders payload guard.
     *   - Stop          → top-level turn ended → completion notification.
     *   - Notification  → the agent needs attention (permission / idle).
     * A missing/garbled payload falls back to the --event arg. */
    if (hook) {
        /* WHITELIST: the agent pipes its event JSON on stdin. ONLY a confirmed
         * top-level Stop (→ completion) or a Notification (→ attention) is
         * allowed to fire. Every other event — SubagentStop, PostToolUse,
         * SessionStart, any tool/process/subagent completion, any unknown name —
         * and any missing/garbled/empty payload is dropped. This is the single
         * gate guaranteeing subagent/tool/process completions never notify;
         * it also fails CLOSED instead of falling back to the verbatim --event
         * arg (which used to leak every non-matched event). */
        const char *hen = read_hook_event_name();
        if (!hen) return 0;                         /* unreadable payload → drop */
        char norm[64];
        normalize_event_name(hen, norm, sizeof norm);
        if (!strcmp(norm, "stop"))              event = EVT_COMPLETE;
        else if (!strcmp(norm, "notification")) event = EVT_NOTIFY;
        else return 0;                              /* subagentstop + all else → drop */
    }

    char sock_path[256];
    if (!notify_socket_path(sock_path, sizeof sock_path)) {
        fprintf(stderr, "liu-notify: socket path resolution failed\n");
        return 74;
    }

    if (connect_and_send(sock_path, tool, event, flags, title, body) == 0)
        return 0;

    /* Reachable but the frame wasn't accepted. The usual cause is a daemon
     * left over from an older build whose protocol predates this client (e.g.
     * it never learned EVT_COMPLETE), so it rejects frames it can't parse and
     * keeps doing so for its whole lifetime. Stop that stale daemon and retry
     * once with a freshly-spawned one. A genuine frame bug would just fail
     * again here, so there's no spawn loop. */
    stop_running_daemon();
    if (connect_and_send(sock_path, tool, event, flags, title, body) == 0)
        return 0;

    fprintf(stderr, "liu-notify: send failed\n");
    return 74;
}

/* ------------------------------------------------------------------------- */
/* cmd: status                                                               */
/* ------------------------------------------------------------------------- */

static int cmd_status(void) {
    char sock_path[256];
    if (!notify_socket_path(sock_path, sizeof sock_path)) return 74;

    int fd = notify_socket_connect(sock_path);
    if (fd >= 0) { close(fd); printf("running\n"); return 0; }
    printf("not running\n");
    return 1;
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
        "  liu-notify stop\n");
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
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) { usage(); return 0; }

    usage();
    return 64;
}
