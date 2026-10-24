/*
 * liu-history/bridge - orchestrator. See bridge.h.
 *
 * This file must stay free of per-agent behaviour: a new agent is a new
 * BridgeTarget in the registry below plus its own source parser, and nothing
 * here or in the UI changes. The only thing it knows about individual agents
 * is which adapter object to hand a history to.
 */
#include "history/bridge/bridge.h"
#include "core/atomic_file.h"
#include "history/bridge/compatibility.h"
#include "history/bridge/lineage.h"
#include "history/bridge/sources.h"
#include "history/bridge/targets.h"
#include "history/util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ---------------------------------------------------------------- naming -- */

const char *bridge_import_mode_name(BridgeImportMode m) {
    switch (m) {
        case BRIDGE_IMPORT_NATIVE_HISTORY:  return "native_history";
        case BRIDGE_IMPORT_NATIVE_FILE:     return "native_file";
        case BRIDGE_IMPORT_INTERNAL_WRITER: return "internal_writer";
        case BRIDGE_IMPORT_CONTEXT_HANDOFF: return "context_handoff";
        default:                            return "unsupported";
    }
}

const char *bridge_import_mode_badge(BridgeImportMode m) {
    switch (m) {
        /* The badges are a promise to the user about fidelity, so they are
         * deliberately not one word per enum value: "the agent imported this
         * itself" and "the agent read a file it documents" are both Native,
         * while a store Liu wrote by hand is Experimental however carefully
         * it was verified. */
        case BRIDGE_IMPORT_NATIVE_HISTORY:  return "Native";
        case BRIDGE_IMPORT_NATIVE_FILE:     return "Native";
        case BRIDGE_IMPORT_INTERNAL_WRITER: return "Experimental";
        case BRIDGE_IMPORT_CONTEXT_HANDOFF: return "Handoff";
        default:                            return "Unsupported";
    }
}

/* ---------------------------------------------------------------- helpers - */

i64 bridge_now_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (i64)tv.tv_sec * 1000 + (i64)(tv.tv_usec / 1000);
}

bool bridge_state_dir(char *out, usize cap) {
    if (!out || cap == 0) return false;
    if (!hist_xdg_state_path(out, cap, "liu/bridge")) return false;
    /* 0700 the whole way down: these files hold whole conversations. The
     * chain matters because a user may have neither ~/.local/state nor
     * ~/.local/state/liu. */
    return bridge_mkdir_p(out);
}

/* Random bytes, urandom with a deterministic-but-unpredictable-enough
 * fallback. Session ids only need to not COLLIDE — they are not secrets and
 * nothing authenticates with them — but a predictable id in another app's
 * store is still a bad shape, so urandom is tried first. */
static void fill_random(u8 *buf, usize n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    usize got = 0;
    if (fd >= 0) {
        while (got < n) {
            ssize_t r = read(fd, buf + got, n - got);
            if (r <= 0) break;
            got += (usize)r;
        }
        close(fd);
    }
    if (got < n) {
        u64 s = (u64)bridge_now_ms() ^ ((u64)getpid() << 32) ^ (u64)(uintptr_t)buf;
        for (; got < n; got++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            buf[got] = (u8)(s >> 24);
        }
    }
}

bool bridge_new_uuid(char *out, usize cap) {
    if (!out || cap < 37) return false;
    u8 b[16];
    fill_random(b, sizeof b);
    b[6] = (u8)((b[6] & 0x0F) | 0x40);   /* version 4 */
    b[8] = (u8)((b[8] & 0x3F) | 0x80);   /* variant 1 */
    snprintf(out, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9],
             b[10],b[11],b[12],b[13],b[14],b[15]);
    return true;
}

bool bridge_new_short_id(char *out, usize cap) {
    if (!out || cap < 27) return false;
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz234567";
    u8 b[26];
    fill_random(b, sizeof b);
    for (usize i = 0; i < 26; i++) out[i] = alpha[b[i] & 31];
    out[26] = '\0';
    return true;
}

/* ------------------------------------------------------------- subprocess - */

bool bridge_binary_available(const char *binary) {
    if (!binary || !binary[0]) return false;
    if (strchr(binary, '/')) return access(binary, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path || !path[0]) return false;
    char cand[ATOMIC_FILE_PATH_CAP];
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        usize len = colon ? (usize)(colon - p) : strlen(p);
        if (len > 0 && len < sizeof cand - strlen(binary) - 2) {
            memcpy(cand, p, len);
            cand[len] = '/';
            memcpy(cand + len + 1, binary, strlen(binary) + 1);
            if (access(cand, X_OK) == 0) return true;
        }
        if (!colon) break;
        p = colon + 1;
    }
    return false;
}

i32 bridge_run_capture(const char *const argv[], char *out, usize out_cap,
                       i32 timeout_sec) {
    return bridge_run_capture_stdin(argv, NULL, 0, out, out_cap, timeout_sec);
}

i32 bridge_run_capture_stdin(const char *const argv[],
                             const char *stdin_data, usize stdin_len,
                             char *out, usize out_cap, i32 timeout_sec) {
    if (out && out_cap) out[0] = '\0';
    if (!argv || !argv[0]) return -1;

    int inp[2]  = { -1, -1 };
    int outp[2] = { -1, -1 };
    if (pipe(outp) != 0) return -1;
    if (stdin_data && pipe(inp) != 0) {
        close(outp[0]); close(outp[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(outp[0]); close(outp[1]);
        if (inp[0] >= 0) { close(inp[0]); close(inp[1]); }
        return -1;
    }
    if (pid == 0) {
        /* Child. No shell anywhere in this subsystem: a session id or a path
         * that happens to contain a quote must never be able to become a
         * command. execvp resolves through PATH, which agent_ensure_process_path
         * repaired at startup so a .app launch finds Homebrew/npm binaries. */
        if (inp[0] >= 0) {
            dup2(inp[0], STDIN_FILENO);
            close(inp[0]); close(inp[1]);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        }
        dup2(outp[1], STDOUT_FILENO);
        dup2(outp[1], STDERR_FILENO);   /* version banners land on stderr */
        close(outp[0]); close(outp[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent.
     *
     * stdin and stdout are pumped TOGETHER. Writing all of stdin first and only
     * then reading stdout deadlocks the moment the child answers while it is
     * still being fed: the child fills the 64 KB stdout pipe, stops reading
     * stdin to wait for it to drain, and the parent blocks in write() on a full
     * stdin pipe waiting for the child to read. Neither side moves.
     * `opencode import` is handed whole migrated sessions, which run to
     * megabytes, and prints as it validates them.
     *
     * Two rules keep it out of the deadlock: both ends are non-blocking and
     * driven by one poll(), and stdout is drained even after the caller's
     * capture buffer is full, because a child blocked on a pipe the parent
     * stopped reading is the same deadlock by another name. */
    close(outp[1]);
    if (inp[0] >= 0) close(inp[0]);

    int wfd = inp[1];                       /* -1 once stdin is done */
    int rfd = outp[0];
    if (!stdin_data || stdin_len == 0) {
        if (wfd >= 0) { close(wfd); wfd = -1; }
    }

    {
        int fl = fcntl(rfd, F_GETFL, 0);
        if (fl >= 0) fcntl(rfd, F_SETFL, fl | O_NONBLOCK);
        if (wfd >= 0) {
            fl = fcntl(wfd, F_GETFL, 0);
            if (fl >= 0) fcntl(wfd, F_SETFL, fl | O_NONBLOCK);
        }
    }

    /* A child that exits early turns the next write into SIGPIPE, which would
     * kill Liu; the write's EPIPE reports it instead. Only a call that WRITES
     * touches the disposition, which is the whole process's: the version
     * probes run on a worker thread with nothing to write, and one of them
     * putting back "default" in the middle of a migration's write would undo
     * that write's guard. */
    const bool guard_pipe = wfd >= 0;
    void (*old_pipe)(int) = guard_pipe ? signal(SIGPIPE, SIG_IGN) : SIG_DFL;

    usize sent = 0, got = 0;
    i64 deadline = bridge_now_ms() + (i64)(timeout_sec > 0 ? timeout_sec : 30) * 1000;
    bool timed_out = false;

    while (rfd >= 0) {
        struct pollfd pfd[2];
        nfds_t nfd = 0;
        i32 ri = -1, wi = -1;
        if (rfd >= 0) { pfd[nfd].fd = rfd; pfd[nfd].events = POLLIN;  ri = (i32)nfd; nfd++; }
        if (wfd >= 0) { pfd[nfd].fd = wfd; pfd[nfd].events = POLLOUT; wi = (i32)nfd; nfd++; }

        i64 left = deadline - bridge_now_ms();
        if (left <= 0) { timed_out = true; break; }
        if (left > 1000) left = 1000;       /* re-check the deadline each second */

        int pr = poll(pfd, nfd, (int)left);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;              /* nothing ready; deadline re-checked */

        if (wi >= 0 && (pfd[wi].revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
            if (pfd[wi].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(wfd); wfd = -1;       /* child is gone or closed stdin */
            } else {
                ssize_t n = write(wfd, stdin_data + sent, stdin_len - sent);
                if (n > 0) {
                    sent += (usize)n;
                    if (sent >= stdin_len) { close(wfd); wfd = -1; }  /* EOF */
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                                     errno == EINTR)) {
                    /* try again on the next poll */
                } else {
                    close(wfd); wfd = -1;
                }
            }
        }

        if (ri >= 0 && (pfd[ri].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            for (;;) {
                char chunk[4096];
                ssize_t n = read(rfd, chunk, sizeof chunk);
                if (n > 0) {
                    /* Truncation is the documented contract of `out`/`out_cap`,
                     * but the DRAIN is unconditional: stopping here would park
                     * the child on a full pipe. */
                    if (out && out_cap && got + 1 < out_cap) {
                        usize room = out_cap - 1 - got;
                        usize take = (usize)n < room ? (usize)n : room;
                        memcpy(out + got, chunk, take);
                        got += take;
                    }
                    continue;
                }
                if (n == 0) { close(rfd); rfd = -1; break; }   /* child closed stdout */
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close(rfd); rfd = -1;
                break;
            }
        }
    }

    if (guard_pipe) signal(SIGPIPE, old_pipe);
    if (out && out_cap) out[got < out_cap ? got : out_cap - 1] = '\0';
    if (wfd >= 0) close(wfd);
    if (rfd >= 0) close(rfd);

    if (timed_out) {
        kill(pid, SIGKILL);
        /* Still reap: a zombie per timed-out probe would accumulate for the
         * life of the process. */
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
        return -1;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

/* ------------------------------------------------------------- registry --- */

/* The whole per-agent surface of the subsystem: one row per adapter. Order is
 * irrelevant — lookup is by tool — but it is kept in the spec's preference
 * order so the file reads as a capability summary. */
static const BridgeTarget *const kTargets[] = {
    &g_bridge_target_codex,
    &g_bridge_target_opencode,
    &g_bridge_target_claude,
    &g_bridge_target_commandcode,
    &g_bridge_target_qwen,
    &g_bridge_target_grok,
    &g_bridge_target_kimi,
    &g_bridge_target_cline_cli,
    &g_bridge_target_cursor,
    &g_bridge_target_copilot,
    &g_bridge_target_droid,
    &g_bridge_target_kiro,
    &g_bridge_target_amp,
    &g_bridge_target_antigravity,
    &g_bridge_target_pi,
    &g_bridge_target_omp,
    &g_bridge_target_openclaw,
    &g_bridge_target_hermes,
};
#define TARGET_COUNT ((usize)(sizeof kTargets / sizeof kTargets[0]))

const BridgeTarget *bridge_target_for(ChatTool tool) {
    for (usize i = 0; i < TARGET_COUNT; i++)
        if (kTargets[i]->tool == tool) return kTargets[i];
    return NULL;
}

BridgeImportMode bridge_capability(ChatTool tool) {
    const BridgeTarget *t = bridge_target_for(tool);
    if (!t) return BRIDGE_IMPORT_UNSUPPORTED;
    if (t->available && !t->available()) return BRIDGE_IMPORT_UNSUPPORTED;
    if (t->mode == BRIDGE_IMPORT_CONTEXT_HANDOFF ||
        t->mode == BRIDGE_IMPORT_UNSUPPORTED) {
        return t->mode;
    }
    /* Version gate. A native-import target passes with an unknown version
     * (the agent validates the payload); a writer does not, and degrades to
     * a handoff rather than writing a store it cannot vouch for. */
    if (!bridge_compat_writer_ok(tool, NULL, 0)) return BRIDGE_IMPORT_CONTEXT_HANDOFF;
    return t->mode;
}

BridgeImportMode bridge_capability_peek(ChatTool tool, bool *pending) {
    if (pending) *pending = false;
    const BridgeTarget *t = bridge_target_for(tool);
    if (!t) return BRIDGE_IMPORT_UNSUPPORTED;
    if (t->available && !t->available()) return BRIDGE_IMPORT_UNSUPPORTED;
    if (t->mode == BRIDGE_IMPORT_CONTEXT_HANDOFF ||
        t->mode == BRIDGE_IMPORT_UNSUPPORTED) {
        return t->mode;
    }
    /* The same gate, never waiting: while the version is still being asked
     * for, the row reads as a version inside the range would make it. */
    if (!bridge_compat_writer_ok_peek(tool, pending)) return BRIDGE_IMPORT_CONTEXT_HANDOFF;
    return t->mode;
}

/* ------------------------------------------------------------ orchestrate - */

static void result_init(BridgeImportResult *r, ChatTool target) {
    memset(r, 0, sizeof *r);
    r->target_tool = target;
    r->mode        = BRIDGE_IMPORT_UNSUPPORTED;
    r->fidelity    = BRIDGE_FIDELITY_HANDOFF;
}

static void result_fail(BridgeImportResult *r, const char *msg) {
    r->success = false;
    r->launch_cmd[0] = '\0';
    if (msg && !r->error[0]) snprintf(r->error, sizeof r->error, "%s", msg);
}

bool bridge_migrate_history(const BridgeHistory *history,
                            ChatTool target_tool,
                            BridgeImportResult *result) {
    if (!result) return false;
    result_init(result, target_tool);

    if (!history || bridge_history_is_empty(history)) {
        result_fail(result, "Source session has no conversation to continue.");
        return false;
    }

    const BridgeTarget *t = bridge_target_for(target_tool);
    if (!t || !t->import_history) {
        result_fail(result, "No adapter for this agent.");
        return false;
    }
    if (t->available && !t->available()) {
        result_fail(result, "That agent is not installed on this machine.");
        return false;
    }

    result->mode = bridge_capability(target_tool);
    if (result->mode == BRIDGE_IMPORT_UNSUPPORTED) {
        result_fail(result, "That agent cannot accept an imported session.");
        return false;
    }

    if (!t->import_history(history, result)) {
        result_fail(result, result->error[0] ? NULL : "Import failed.");
        return false;
    }

    /* Files on disk are not a session. An adapter that only checked its own
     * fwrite return would hand the user a launch command for something the
     * target agent then refuses to open. Validation is mandatory and a
     * failure is a failure. */
    if (t->validate_import && !t->validate_import(result)) {
        result_fail(result, result->error[0] ? NULL
                          : "Generated session failed validation; nothing was launched.");
        return false;
    }

    result->success = true;
    result->fidelity = bridge_fidelity_of(&history->fidelity);
    result->history_complete      = history->fidelity.history_complete;
    result->metadata_complete     = history->fidelity.metadata_complete;
    result->tool_history_complete = history->fidelity.history_complete &&
                                    (t->supports_full_history
                                       ? t->supports_full_history() : false);
    if (result->mode == BRIDGE_IMPORT_CONTEXT_HANDOFF)
        result->fidelity = BRIDGE_FIDELITY_HANDOFF;

    /* Lineage is best-effort: a journal line that could not be written is a
     * warning, never a reason to call a completed migration a failure. */
    BridgeLineage rec = {0};
    rec.source_tool = history->source_tool;
    rec.target_tool = target_tool;
    hist_copy_bounded(rec.source_session_id, sizeof rec.source_session_id,
                      history->source_session_id ? history->source_session_id : "");
    hist_copy_bounded(rec.target_session_id, sizeof rec.target_session_id,
                      result->session_id);
    rec.created_at_ms = bridge_now_ms();
    rec.mode     = result->mode;
    rec.fidelity = result->fidelity;
    if (!bridge_lineage_record(&rec)) result->warnings++;

    return true;
}

bool bridge_migrate(const ChatSessionMeta *source, ChatTool target_tool,
                    BridgeImportResult *result) {
    if (!result) return false;
    result_init(result, target_tool);
    if (!source) {
        result_fail(result, "No source session.");
        return false;
    }

    /* Sized from the source transcript: the parse dups every kept string into
     * this arena, and the IR's own byte budget caps the total, so the arena
     * only has to cover that plus the cJSON churn of a whole-file parser.
     * mmap is demand-zero, so the headroom costs address space, not RSS. */
    usize want = MB(8);
    if (source->size_bytes > 0) {
        usize from_file = (usize)source->size_bytes * 3u + MB(4);
        if (from_file > want) want = from_file;
    }
    if (want > BRIDGE_TOTAL_MAX + MB(32)) want = BRIDGE_TOTAL_MAX + MB(32);

    Arena arena = arena_create(want);
    if (!arena.base) {
        result_fail(result, "Out of memory reading the source session.");
        return false;
    }

    BridgeHistory h;
    bool parsed = bridge_parse_source(&arena, source, &h);
    bool ok = false;
    if (!parsed) {
        result_fail(result, "Liu cannot read that agent's session format yet.");
    } else {
        ok = bridge_migrate_history(&h, target_tool, result);
        bridge_history_free(&h);
    }
    arena_destroy(&arena);
    return ok;
}

/* ------------------------------------------------- interactive subprocess - */

struct BridgeProc {
    pid_t pid;
    int   in_fd;
    int   out_fd;
    /* Partial line carried across reads. The app-server writes one JSON
     * object per line but a read boundary lands wherever the pipe buffer
     * says, so a line assembled per-read would be cut in half. */
    char  buf[MB(2)];
    usize len;
    bool  eof;
};

BridgeProc *bridge_proc_start(const char *const argv[]) {
    if (!argv || !argv[0]) return NULL;
    int inp[2], outp[2];
    if (pipe(inp) != 0) return NULL;
    if (pipe(outp) != 0) { close(inp[0]); close(inp[1]); return NULL; }

    pid_t pid = fork();
    if (pid < 0) {
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        return NULL;
    }
    if (pid == 0) {
        dup2(inp[0], STDIN_FILENO);
        dup2(outp[1], STDOUT_FILENO);
        /* stderr to /dev/null, NOT merged into stdout: this is a line
         * protocol and a banner or a deprecation notice on the same stream
         * would be read as a malformed message. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(inp[0]);
    close(outp[1]);

    BridgeProc *p = calloc(1, sizeof *p);
    if (!p) {
        close(inp[1]); close(outp[0]);
        kill(pid, SIGKILL);
        int st; waitpid(pid, &st, 0);
        return NULL;
    }
    p->pid    = pid;
    p->in_fd  = inp[1];
    p->out_fd = outp[0];
    int flags = fcntl(p->out_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(p->out_fd, F_SETFL, flags | O_NONBLOCK);
    return p;
}

bool bridge_proc_send(BridgeProc *p, const char *data, usize len) {
    if (!p || p->in_fd < 0 || !data) return false;
    /* A child that died mid-write would raise SIGPIPE and take Liu with it;
     * the write's return value is what reports that instead. */
    void (*old)(int) = signal(SIGPIPE, SIG_IGN);
    usize sent = 0;
    bool ok = true;
    while (sent < len) {
        ssize_t n = write(p->in_fd, data + sent, len - sent);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            ok = false;
            break;
        }
        sent += (usize)n;
    }
    signal(SIGPIPE, old);
    return ok;
}

char *bridge_proc_await_line(BridgeProc *p, const char *needle, i32 timeout_sec) {
    if (!p || p->out_fd < 0 || !needle) return NULL;
    i64 deadline = bridge_now_ms() + (i64)(timeout_sec > 0 ? timeout_sec : 30) * 1000;

    for (;;) {
        /* Consume whole lines already buffered before reading more, so a
         * single read that delivered several messages is fully processed. */
        for (;;) {
            char *nl = memchr(p->buf, '\n', p->len);
            if (!nl) break;
            usize line_len = (usize)(nl - p->buf);
            char *line = NULL;
            if (line_len > 0) {
                line = malloc(line_len + 1);
                if (line) {
                    memcpy(line, p->buf, line_len);
                    line[line_len] = '\0';
                }
            }
            usize rest = p->len - line_len - 1;
            memmove(p->buf, nl + 1, rest);
            p->len = rest;
            if (line) {
                if (strstr(line, needle)) return line;
                free(line);   /* a notification, not the response */
            }
        }

        if (p->eof) return NULL;
        if (bridge_now_ms() > deadline) return NULL;

        if (p->len >= sizeof p->buf - 1) {
            /* One line longer than the buffer. Dropping the buffer would
             * resynchronise on a random byte, so give up on this exchange
             * rather than return a fragment as if it were a message. */
            return NULL;
        }
        ssize_t n = read(p->out_fd, p->buf + p->len, sizeof p->buf - 1 - p->len);
        if (n > 0) {
            p->len += (usize)n;
            continue;
        }
        if (n == 0) { p->eof = true; continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        if (errno == EINTR) continue;
        p->eof = true;
    }
}

void bridge_proc_stop(BridgeProc *p) {
    if (!p) return;
    if (p->in_fd >= 0) { close(p->in_fd); p->in_fd = -1; }

    /* Give it a moment to exit on its own now that stdin is closed — that is
     * the app-server's own shutdown signal — then take it out. Either way the
     * child is reaped: a zombie per migration would accumulate for the life
     * of the process. */
    for (i32 i = 0; i < 50; i++) {
        int st;
        pid_t r = waitpid(p->pid, &st, WNOHANG);
        if (r == p->pid) { p->pid = -1; break; }
        if (r < 0) { p->pid = -1; break; }
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (p->pid > 0) {
        kill(p->pid, SIGKILL);
        int st;
        while (waitpid(p->pid, &st, 0) < 0 && errno == EINTR) { }
    }
    if (p->out_fd >= 0) close(p->out_fd);
    free(p);
}
