/*
 * Liu - Translate-on-Tab: agent backend.
 *
 * fork+exec the configured AI CLI agent with the translation prompt,
 * wire stdout/stderr into a non-blocking pipe. Mirrors the Create-Theme
 * spawn (main.c) without the JSON-extraction tail — translate output
 * is plain text and is only trimmed.
 */
#include "translate/translate_agent.h"
#include "core/agent_headless.h"
#include "core/checked.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef PLATFORM_WIN32
#include <sys/wait.h>
#endif

extern char **environ;

static int agent_fd_above_stdio(int fd) {
    if (fd < 0) return fd;
    if (fd < 3) {
        int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        close(fd);
        return moved;
    }
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool agent_write_all(int fd, const char *data, usize len) {
    usize off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += (usize)n;
    }
    return true;
}

static bool env_has_key(const char *entry, const char *key) {
    usize n = strlen(key);
    return entry && strncmp(entry, key, n) == 0 && entry[n] == '=';
}

/* Return a shallow environment vector with Grok's noisy tracing/update vars
 * overridden. Strings from environ remain owned by libc; only the pointer
 * vector is allocated. */
static char **agent_grok_env(void) {
    usize count = 0;
    if (environ) {
        while (environ[count]) {
            if (count >= 16384u) return NULL;
            count++;
        }
    }
    usize slots;
    if (!checked_add_usize(count, 3u, &slots)) return NULL;
    char **out = calloc(slots, sizeof *out);
    if (!out) return NULL;
    usize n = 0;
    for (usize i = 0; i < count; i++) {
        if (env_has_key(environ[i], "RUST_LOG") ||
            env_has_key(environ[i], "GROK_DISABLE_UPDATE_CHECK")) continue;
        out[n++] = environ[i];
    }
    out[n++] = (char *)"RUST_LOG=off";
    out[n++] = (char *)"GROK_DISABLE_UPDATE_CHECK=1";
    out[n] = NULL;
    return out;
}

static const AgentInfo *find_agent_by_id(const AgentInfo *agents, i32 count,
                                         const char *id) {
    if (!agents || count <= 0 || !id || !id[0]) return NULL;
    for (i32 i = 0; i < count; i++) {
        if (strcmp(agents[i].id, id) == 0) return &agents[i];
    }
    return NULL;
}

/* Remove the codex last-message scratch file when the spawn never got off the
 * ground — the caller only unlinks it after a child it actually started. */
static void spawn_discard_last_msg(TranslateAgentSpawn *out) {
    if (out && out->last_message_path[0]) {
        (void)unlink(out->last_message_path);
        out->last_message_path[0] = '\0';
    }
}

bool translate_agent_spawn(const TranslateConfig *cfg,
                           const char *text,
                           TranslateAgentSpawn *out) {
    if (!cfg || !text || !out) return false;
    out->child_pid = 0;
    out->stdout_fd = -1;
    out->last_message_path[0] = '\0';

    AgentInfo agents[AGENT_MAX] = {0};
    i32 n = agent_detect_available(agents, AGENT_MAX);
    if (n < 0 || n > AGENT_MAX) return false;
    const AgentInfo *ag = find_agent_by_id(agents, n, cfg->agent_id);
    if (!ag || !ag->headless) return false;

    /* Build canonical prompt. Cap at 64 KB to stay clear of ARG_MAX. */
    usize prompt_cap = 64 * 1024;
    char *prompt = (char *)malloc(prompt_cap);
    if (!prompt) return false;
    translate_build_prompt(cfg, text, prompt, prompt_cap);

    if (ag->args_count < 0 || ag->args_count > AGENT_MAX_ARGS) {
        free(prompt);
        return false;
    }

    char codex_model_arg[96] = {0};
    char *argv[AGENT_MAX_ARGS + 9] = {0};
    const i32 argv_cap = (i32)(sizeof argv / sizeof argv[0]);
    i32 ai = 0;
    #define PUSH_ARG(v) do { \
        if (ai >= argv_cap - 1) { spawn_discard_last_msg(out); free(prompt); return false; } \
        argv[ai++] = (char *)(v); \
    } while (0)
    PUSH_ARG(ag->path[0] ? ag->path : ag->binary);
    /* Antigravity's `-p` is a Go string flag, so it consumes the NEXT argv as
     * its value — the prompt. Its `--model` pair has to lead the prelude
     * instead of following it like every other agent's, or `-p` swallows the
     * flag and the prompt degrades into a stray positional argument:
     * `agy --model X -p "<prompt>"` translates, `agy -p --model X "<prompt>"`
     * does not. */
    if (cfg->agent_model[0] && strcmp(ag->id, "antigravity") == 0) {
        PUSH_ARG("--model"); PUSH_ARG(cfg->agent_model);
    }
    for (i32 k = 0; k < ag->args_count; k++) PUSH_ARG(ag->args[k]);
    /* A translation is text in, text out: no tools, no MCP servers (see
     * core/agent_headless.h — what a bare `claude -p` carries, and why the
     * prompt follows a `--`). */
    {
        const char *hf[AGENT_HEADLESS_MAX_FLAGS];
        i32 hn = agent_headless_flags(ag->id, hf, AGENT_HEADLESS_MAX_FLAGS);
        for (i32 k = 0; k < hn; k++) PUSH_ARG(hf[k]);
    }
    if (strcmp(ag->id, "claude") == 0) {
        PUSH_ARG("--no-session-persistence");
    } else if (strcmp(ag->id, "codex") == 0) {
        PUSH_ARG("--ephemeral");
    }
    /* Codex stdout can contain session banners, the echoed prompt,
     * configuration warnings, MCP diagnostics, and token summaries around the
     * response. Use -o <file> to write the final message and read that file
     * instead of trying to separate the answer from stdout. */
    char last_msg_tmpl[] = "/tmp/liu-agent-last-XXXXXX";
    if (strcmp(ag->id, "codex") == 0) {
        int lm_fd = mkstemp(last_msg_tmpl);
        if (lm_fd >= 0) {
            close(lm_fd);   /* codex opens the path itself */
            snprintf(out->last_message_path, sizeof out->last_message_path,
                     "%s", last_msg_tmpl);
            PUSH_ARG("-o"); PUSH_ARG(out->last_message_path);
        }
    }
    if (cfg->agent_model[0]) {
        if (strcmp(ag->id, "claude") == 0) {
            PUSH_ARG("--model"); PUSH_ARG(cfg->agent_model);
        } else if (strcmp(ag->id, "codex") == 0) {
            int mw = snprintf(codex_model_arg, sizeof codex_model_arg,
                              "model=%s", cfg->agent_model);
            if (mw < 0 || (usize)mw >= sizeof codex_model_arg) {
                spawn_discard_last_msg(out);
                free(prompt);
                return false;
            }
            PUSH_ARG("-c"); PUSH_ARG(codex_model_arg);
        } else if (strcmp(ag->id, "grok") == 0 ||
                   strcmp(ag->id, "opencode") == 0) {
            PUSH_ARG("-m"); PUSH_ARG(cfg->agent_model);
        } else if (strcmp(ag->id, "pi") == 0 || strcmp(ag->id, "omp") == 0) {
            /* Both take `--model <pattern>` ("provider/id", optionally with a
             * `:thinking` suffix); omp documents it as `--model=<value>` and
             * reads the spaced form the same way. */
            PUSH_ARG("--model"); PUSH_ARG(cfg->agent_model);
        }
    }
    if (!ag->stdin_prompt) {
        if (agent_headless_prompt_after_dashdash(ag->id)) PUSH_ARG("--");
        PUSH_ARG(prompt);
    }
    argv[ai] = NULL;
    #undef PUSH_ARG

    int pipefd[2];
    if (pipe(pipefd) != 0) { spawn_discard_last_msg(out); free(prompt); return false; }
    pipefd[0] = agent_fd_above_stdio(pipefd[0]);
    pipefd[1] = agent_fd_above_stdio(pipefd[1]);
    if (pipefd[0] < 0 || pipefd[1] < 0) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
        spawn_discard_last_msg(out);
        free(prompt);
        return false;
    }
    /* Mark both ends close-on-exec so the untrusted agent (and any other
     * fork+exec child that overlaps this spawn) does not inherit Liu's
     * descriptors — the agent's stdout/stderr survive because dup2() in the
     * child clears CLOEXEC on the new fds. (macOS has no pipe2.) */
    int prompt_fd = -1;
    if (ag->stdin_prompt) {
        char tmp[] = "/tmp/liu-agent-prompt-XXXXXX";
        prompt_fd = mkstemp(tmp);
        if (prompt_fd >= 0) (void)unlink(tmp);
        prompt_fd = agent_fd_above_stdio(prompt_fd);
        if (prompt_fd < 0 ||
            !agent_write_all(prompt_fd, prompt, strlen(prompt)) ||
            lseek(prompt_fd, 0, SEEK_SET) != 0) {
            if (prompt_fd >= 0) close(prompt_fd);
            close(pipefd[0]); close(pipefd[1]);
            spawn_discard_last_msg(out);
            free(prompt);
            return false;
        }
    }

    posix_spawn_file_actions_t fa;
    int rc = posix_spawn_file_actions_init(&fa);
    bool fa_inited = rc == 0;
    if (rc == 0 && ag->stdin_prompt)
        rc = posix_spawn_file_actions_adddup2(&fa, prompt_fd, STDIN_FILENO);
    else if (rc == 0)
        rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO,
                                               "/dev/null", O_RDONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    if (rc == 0 && prompt_fd >= 0)
        rc = posix_spawn_file_actions_addclose(&fa, prompt_fd);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    char **child_env = NULL;
    char **envp = environ;
    if (strcmp(ag->id, "grok") == 0) {
        child_env = agent_grok_env();
        if (!child_env) rc = ENOMEM;
        else envp = child_env;
    }

    pid_t pid = 0;
    if (rc == 0) {
        rc = ag->path[0]
           ? posix_spawn(&pid, ag->path, &fa, NULL, argv, envp)
           : posix_spawnp(&pid, ag->binary, &fa, NULL, argv, envp);
    }
    free(child_env);
    if (fa_inited) posix_spawn_file_actions_destroy(&fa);
    if (prompt_fd >= 0) close(prompt_fd);
    close(pipefd[1]);
    free(prompt);
    if (rc != 0 || pid <= 0 || (u64)pid > (u64)INT32_MAX) {
        close(pipefd[0]);
        if (rc == 0 && pid > 0) {
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        }
        spawn_discard_last_msg(out);
        return false;
    }
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(pipefd[0]);
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        spawn_discard_last_msg(out);
        return false;
    }

    out->child_pid = (i32)pid;
    out->stdout_fd = pipefd[0];
    return true;
}

/* Strip an ANSI CSI sequence (ESC '[' ... final-byte). Returns the
 * number of bytes consumed including the final byte, or 0 if the input
 * doesn't start with ESC '['. Tolerates truncation by returning the
 * remaining length so the trim loop can drop the rest. */
static i32 skip_ansi_csi(const char *p, i32 left) {
    if (left < 2 || p[0] != 0x1B || p[1] != '[') return 0;
    i32 i = 2;
    while (i < left) {
        unsigned char c = (unsigned char)p[i++];
        if (c >= 0x40 && c <= 0x7E) return i;
    }
    return left;
}

static i32 skip_ansi_esc(const char *p, i32 left) {
    if (left < 1 || p[0] != 0x1B) return 0;
    if (left >= 2 && p[1] == '[') return skip_ansi_csi(p, left);
    /* Drop ESC + one byte for short escapes (ESC c, ESC =, etc.). */
    if (left >= 2) return 2;
    return 1;
}

bool translate_agent_finalize(const char *log, i32 log_len,
                              char *out, usize out_cap) {
    if (!log || log_len <= 0 || !out || out_cap == 0) {
        if (out && out_cap > 0) out[0] = '\0';
        return false;
    }

    /* Phase 1 — strip ANSI escapes into a heap scratch buffer. */
    usize scratch_size;
    char *scratch = checked_add_usize((usize)log_len, 1u, &scratch_size)
        ? (char *)malloc(scratch_size) : NULL;
    if (!scratch) { out[0] = '\0'; return false; }
    i32 si = 0;
    for (i32 i = 0; i < log_len; ) {
        if ((unsigned char)log[i] == 0x1B) {
            i32 used = skip_ansi_esc(log + i, log_len - i);
            i += used > 0 ? used : 1;
            continue;
        }
        scratch[si++] = log[i++];
    }
    scratch[si] = '\0';

    /* Oh My Pi's print mode puts "Working..." on STDERR as a progress line
     * (stdout carries the answer), and the spawn gives the child one pipe for
     * both. A line that is nothing else is never part of a translation of
     * anything Liu sends, and is dropped wherever the two streams interleaved.
     */
    {
        static const char noise[] = "Working...";
        const usize nlen = sizeof noise - 1;
        i32 w = 0;
        for (i32 i = 0; i < si; ) {
            i32 e = i;
            while (e < si && scratch[e] != '\n') e++;
            i32 le = e;
            while (le > i && scratch[le - 1] == '\r') le--;
            if ((usize)(le - i) == nlen && memcmp(scratch + i, noise, nlen) == 0) {
                i = e < si ? e + 1 : e;
                continue;
            }
            i32 upto = e < si ? e + 1 : e;
            memmove(scratch + w, scratch + i, (usize)(upto - i));
            w += upto - i;
            i = upto;
        }
        si = w;
        scratch[si] = '\0';
    }

    /* Phase 2 — trim leading/trailing whitespace. */
    i32 start = 0;
    while (start < si && (unsigned char)scratch[start] <= ' ') start++;
    i32 end = si;
    while (end > start && (unsigned char)scratch[end - 1] <= ' ') end--;
    if (start < end && scratch[start] == '>') {
        i32 line_end = start;
        while (line_end < end && scratch[line_end] != '\n' && scratch[line_end] != '\r') {
            line_end++;
        }
        bool looks_like_opencode_banner =
            strstr(scratch + start, "build") == scratch + start + 2 ||
            memchr(scratch + start, (unsigned char)0xB7, (usize)(line_end - start)) != NULL;
        if (looks_like_opencode_banner) {
            start = line_end;
            while (start < end && (unsigned char)scratch[start] <= ' ') start++;
        }
    }
    i32 len = end - start;
    if (len <= 0) { free(scratch); out[0] = '\0'; return false; }

    if ((usize)len >= out_cap) { free(scratch); out[0] = '\0'; return false; }
    usize copy = (usize)len;
    memcpy(out, scratch + start, copy);
    out[copy] = '\0';
    free(scratch);
    return copy > 0;
}
