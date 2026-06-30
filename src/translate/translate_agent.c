/*
 * Liu - Translate-on-Tab: agent backend.
 *
 * fork+exec the configured AI CLI agent with the translation prompt,
 * wire stdout/stderr into a non-blocking pipe. Mirrors the Create-Theme
 * spawn (main.c) without the JSON-extraction tail — translate output
 * is plain text and we just trim it.
 */
#include "translate/translate_agent.h"

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef PLATFORM_WIN32
#include <sys/wait.h>
#endif

static const AgentInfo *find_agent_by_id(const AgentInfo *agents, i32 count,
                                         const char *id) {
    if (!agents || count <= 0 || !id || !id[0]) return NULL;
    for (i32 i = 0; i < count; i++) {
        if (strcmp(agents[i].id, id) == 0) return &agents[i];
    }
    return NULL;
}

bool translate_agent_spawn(const TranslateConfig *cfg,
                           const char *text,
                           TranslateAgentSpawn *out) {
    if (!cfg || !text || !out) return false;
    out->child_pid = 0;
    out->stdout_fd = -1;

    AgentInfo agents[AGENT_MAX] = {0};
    i32 n = agent_detect_available(agents, AGENT_MAX);
    const AgentInfo *ag = find_agent_by_id(agents, n, cfg->agent_id);
    if (!ag) return false;

    /* Build canonical prompt. Cap at 64 KB to stay clear of ARG_MAX. */
    usize prompt_cap = 64 * 1024;
    char *prompt = (char *)malloc(prompt_cap);
    if (!prompt) return false;
    translate_build_prompt(cfg, text, prompt, prompt_cap);

    int pipefd[2];
    if (pipe(pipefd) != 0) { free(prompt); return false; }
    /* Mark both ends close-on-exec so the untrusted agent (and any other
     * fork+exec child that overlaps this spawn) does not inherit Liu's
     * descriptors — the agent's stdout/stderr survive because dup2() in the
     * child clears CLOEXEC on the new fds. (macOS has no pipe2.) */
    (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]); free(prompt); return false;
    }
    if (pid == 0) {
        /* Child — wire stdout/stderr into the pipe. */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        if (ag->stdin_prompt) {
            /* Cline-style: pump the prompt over stdin via a helper child. */
            int sp[2];
            if (pipe(sp) == 0) {
                pid_t gc = fork();
                if (gc == 0) {
                    close(sp[0]);
                    (void)!write(sp[1], prompt, strlen(prompt));
                    close(sp[1]);
                    _exit(0);
                }
                close(sp[1]);
                dup2(sp[0], STDIN_FILENO);
                close(sp[0]);
            } else {
                int devnull = open("/dev/null", O_RDONLY);
                if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
            }
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        }

        /* Grok streams Rust `tracing` worker/auth noise and update checks to
         * stderr, which we merge into the capture pipe above — left alone it
         * corrupts the translated text. Silence it for this child only (the
         * setenv touches just the forked process's environment). */
        if (strcmp(ag->id, "grok") == 0) {
            setenv("RUST_LOG", "off", 1);
            setenv("GROK_DISABLE_UPDATE_CHECK", "1", 1);
        }

        /* argv: binary, prelude args, optional model selector, then
         * prompt as final arg (unless stdin_prompt).
         *
         * Codex's `exec` subcommand takes the model via `-c model=<X>`
         * (a config override), not the positional `--model` that claude
         * accepts. Branch on agent id so each CLI receives
         * the flavour it actually parses. */
        char *argv[AGENT_MAX_ARGS + 6] = {0};
        i32 ai = 0;
        argv[ai++] = (char *)(ag->path[0] ? ag->path : ag->binary);
        for (i32 k = 0; k < ag->args_count && ai + 1 < (i32)(sizeof argv / sizeof argv[0]); k++) {
            argv[ai++] = (char *)ag->args[k];
        }
        /* Suppress the throwaway translate transcript at the source where the
         * CLI supports it, so it never lands in the agent's session store and
         * pollutes Agent History. Verified on the installed binaries:
         *   claude --no-session-persistence  (only with --print; -p is already
         *                                      in claude's prelude)
         *   codex exec --ephemeral           (an `exec` flag; exec is in the
         *                                      prelude)
         * grok and opencode have NO such flag — their one-shot transcripts are
         * removed post-exit by translate_history_cleanup instead. Pushed before
         * the model selector / positional prompt so it parses as an option. */
        if (strcmp(ag->id, "claude") == 0) {
            if (ai + 1 < (i32)(sizeof argv / sizeof argv[0]))
                argv[ai++] = (char *)"--no-session-persistence";
        } else if (strcmp(ag->id, "codex") == 0) {
            if (ai + 1 < (i32)(sizeof argv / sizeof argv[0]))
                argv[ai++] = (char *)"--ephemeral";
        }
        if (cfg->agent_model[0]) {
            if (strcmp(ag->id, "claude") == 0) {
                if (ai + 2 < (i32)(sizeof argv / sizeof argv[0])) {
                    argv[ai++] = (char *)"--model";
                    argv[ai++] = (char *)cfg->agent_model;
                }
            } else if (strcmp(ag->id, "codex") == 0) {
                /* `codex exec -c model=<X>` — the -c flag has to come
                 * before the positional prompt and is parsed as a
                 * KEY=VALUE pair. Build the pair into a small scratch
                 * buffer the child inherits via dup2'd pipes (alloc on
                 * the stack: prompt buffer outlives this scope inside
                 * the child until exec). */
                static char codex_model_arg[96];
                snprintf(codex_model_arg, sizeof codex_model_arg,
                         "model=%s", cfg->agent_model);
                if (ai + 2 < (i32)(sizeof argv / sizeof argv[0])) {
                    argv[ai++] = (char *)"-c";
                    argv[ai++] = codex_model_arg;
                }
            } else if (strcmp(ag->id, "grok") == 0) {
                /* `grok -p <prompt> -m <model>` — headless model selector. */
                if (ai + 2 < (i32)(sizeof argv / sizeof argv[0])) {
                    argv[ai++] = (char *)"-m";
                    argv[ai++] = (char *)cfg->agent_model;
                }
            } else if (strcmp(ag->id, "opencode") == 0) {
                /* `opencode run -m <provider/model> <prompt>` — the model
                 * id carries its provider prefix (the Settings presets are
                 * the OpenCode Go catalog: "opencode-go/<model>"). */
                if (ai + 2 < (i32)(sizeof argv / sizeof argv[0])) {
                    argv[ai++] = (char *)"-m";
                    argv[ai++] = (char *)cfg->agent_model;
                }
            }
        }
        if (!ag->stdin_prompt) {
            argv[ai++] = prompt;
        }
        argv[ai] = NULL;
        if (ag->path[0]) {
            execv(ag->path, argv);
        } else {
            execvp(ag->binary, argv);
        }
        const char *err = "[liu] failed to exec agent\n";
        (void)!write(STDERR_FILENO, err, strlen(err));
        _exit(127);
    }

    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    out->child_pid = (i32)pid;
    out->stdout_fd = pipefd[0];
    free(prompt);
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
    char *scratch = (char *)malloc((usize)log_len + 1);
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

    i32 copy = len < (i32)(out_cap - 1) ? len : (i32)(out_cap - 1);
    memcpy(out, scratch + start, (usize)copy);
    out[copy] = '\0';
    free(scratch);
    return copy > 0;
}
