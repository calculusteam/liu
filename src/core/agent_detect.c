/*
 * Liu - CLI agent autodetect
 * Scans PATH for known coding-agent binaries.
 */
#include "core/agent_detect.h"
#include "core/agent_names.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>

extern char **environ;

/* The probe's answer is the text between these two markers and nothing else.
 * The shell runs interactive (-i), so the user's rc file runs too, and what it
 * prints goes to the same stdout: a greeting, a version manager's notice, a
 * prompt theme's warning. Captured whole, that text would become the head of
 * PATH, and the panes opened with it could fail to find ordinary commands. */
#define LOGIN_PATH_BEGIN "__LIU_PATH_BEGIN__"
#define LOGIN_PATH_END   "__LIU_PATH_END__"

static i64 login_path_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1000 + (i64)(ts.tv_nsec / 1000000);
}

usize agent_login_path_extract(const char *buf, usize len, char *out, usize cap) {
    if (!buf || !out || cap == 0) return 0;
    out[0] = '\0';
    const usize bl = sizeof(LOGIN_PATH_BEGIN) - 1, el = sizeof(LOGIN_PATH_END) - 1;
    if (len < bl + el) return 0;

    /* The LAST begin marker: ours is the last thing the shell is told to
     * print, and an rc file cannot have printed one after it. */
    const char *begin = NULL;
    for (usize i = len - bl + 1; i-- > 0; ) {
        if (memcmp(buf + i, LOGIN_PATH_BEGIN, bl) == 0) { begin = buf + i + bl; break; }
    }
    if (!begin) return 0;
    const char *end = NULL;
    for (const char *p = begin; (usize)(buf + len - p) >= el; p++) {
        if (memcmp(p, LOGIN_PATH_END, el) == 0) { end = p; break; }
    }
    if (!end) return 0;                       /* cut short: not an answer */

    usize n = (usize)(end - begin);
    if (n == 0 || n >= cap) return 0;
    /* A PATH is one line of printable text. Anything else between the
     * markers is not something to hand to setenv(). */
    for (usize i = 0; i < n; i++) {
        unsigned char c = (unsigned char)begin[i];
        if (c < 0x20 || c == 0x7F) return 0;
    }
    memcpy(out, begin, n);
    out[n] = '\0';
    return n;
}

/* Ask `shell` for the PATH a login+interactive instance of it ends up with.
 * Returns the length written to `out` (NUL-terminated), 0 when there is no
 * usable answer — the caller then keeps the PATH it already has.
 *
 * This runs before the first window exists, and it runs the user's rc files.
 * An rc file can wait on something (a prompt, a network mount, an agent
 * socket), and one that leaves a background job holding stdout never reaches
 * end-of-file even after the shell itself has exited. So:
 *   - the whole probe has `deadline_ms`; past it the shell is killed and the
 *     answer is "none";
 *   - once the shell has exited, what is already in the pipe is taken and
 *     nobody waits for whoever else still holds the other end.
 *
 * The shell goes through posix_spawn, never a shell string, so a quote inside
 * $SHELL cannot break any quoting. It must be an absolute path to an
 * executable; anything else is not a login shell. */
usize agent_login_shell_path(const char *shell, char *out, usize cap, i32 deadline_ms) {
    if (out && cap) out[0] = '\0';
    if (!shell || shell[0] != '/' || access(shell, X_OK) != 0 || !out || cap == 0) return 0;
    int pfd[2];
    if (pipe(pfd) != 0) return 0;
    (void)fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL, 0) | O_NONBLOCK);

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) { close(pfd[0]); close(pfd[1]); return 0; }
    int rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    char *argv[] = { (char *)shell, (char *)"-lic",
                     (char *)"printf '%s%s%s' '" LOGIN_PATH_BEGIN "' \"$PATH\" '" LOGIN_PATH_END "'",
                     NULL };
    pid_t pid = -1;
    if (rc == 0) rc = posix_spawn(&pid, shell, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[1]);
    if (rc != 0) { close(pfd[0]); return 0; }

    /* Room for the PATH plus whatever the rc files say around it; only the
     * tail is kept once it is full, because the answer is printed last. */
    char buf[16384];
    usize got = 0;
    const i64 deadline = login_path_now_ms() + (deadline_ms > 0 ? deadline_ms : 1);
    bool exited = false, timed_out = false, eof = false;
    while (!eof) {
        for (;;) {                               /* take what is there */
            if (got == sizeof buf) {
                memmove(buf, buf + sizeof buf / 2, sizeof buf / 2);
                got = sizeof buf / 2;
            }
            ssize_t n = read(pfd[0], buf + got, sizeof buf - got);
            if (n > 0) { got += (usize)n; continue; }
            if (n == 0) eof = true;
            else if (errno == EINTR) continue;
            else if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
            break;
        }
        if (eof || exited) break;                /* exited: that was the last look */

        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) { exited = true; continue; }

        i64 left = deadline - login_path_now_ms();
        if (left <= 0) { timed_out = true; break; }
        struct pollfd p = { .fd = pfd[0], .events = POLLIN };
        (void)poll(&p, 1, (int)(left < 50 ? left : 50));
    }
    close(pfd[0]);

    if (!exited) {
        /* Still running (the deadline), or it closed its stdout and lives on:
         * either way it is ours to end, and SIGKILL is the one signal an rc
         * file cannot have arranged to ignore. */
        if (timed_out) (void)kill(pid, SIGKILL);
        int status = 0;
        i64 give_up = login_path_now_ms() + 2000;
        for (;;) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || (r < 0 && errno != EINTR)) break;
            if (login_path_now_ms() >= give_up) { (void)kill(pid, SIGKILL); give_up += 2000; }
            struct timespec ts = { 0, 5 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    if (timed_out) return 0;
    return agent_login_path_extract(buf, got, out, cap);
}

typedef struct {
    const char *id;
    const char *display;
    const char *binary;
    /* NULL-terminated prelude args inserted between binary and prompt.
     * Codex needs --color/--skip-git-repo-check so it doesn't emit ANSI
     * escapes (which break Liu's JSON brace-walker) or bail when
     * launched outside a git repo. */
    const char *args[AGENT_MAX_ARGS];
    bool        stdin_prompt;
    bool        headless;       /* see AgentInfo.headless */
} AgentSpec;

static const AgentSpec g_known_agents[] = {
    { "claude", "Claude Code", "claude",
      { "-p", NULL }, false, true },
    { "codex",  "Codex",       "codex",
      { "exec", "--color", "never", "--skip-git-repo-check", NULL }, false, true },
    { "cursor", "Cursor Agent","cursor-agent",
      { "-p", NULL }, false, true },
    { "amp",    "Amp",         "amp",
      { NULL }, false, true },
    /* Prompt goes in argv, NOT on stdin. Piping the prompt in makes cline
     * decide it is starting an interactive session and refuse:
     *   "error: interactive mode requires a TTY (stdin/stdout must both be
     *    terminals)"  → exit 1
     * and every headless spawn Liu does (Create Theme, Create Note, Translate)
     * hands the child a pipe for stdout. As a positional argument the same
     * prompt runs fine and answers on stdout (cline 3.x). */
    { "cline",  "Cline",       "cline",
      { NULL }, false, true },
    { "opencode", "OpenCode",  "opencode",
      { "run", NULL }, false, true },
    { "grok",   "Grok",        "grok",
      { "-p", NULL }, false, true },
    { "antigravity", "Antigravity", "agy",
      { "-p", NULL }, false, true },
    /* Command Code forks Claude Code, down to the -p/--print non-interactive
     * flag. Listing it here is what makes Liu install its notify hook at
     * launch — the installer set is driven by agent_detect_available(). */
    { "commandcode", "Command Code", "commandcode",
      { "-p", NULL }, false, true },
    /* Pi and Oh My Pi accept -p to print the final response and exit; the prompt
     * is positional. */
    { "pi",  "Pi",       "pi",  { "-p", NULL }, false, true },
    { "omp", "Oh My Pi", "omp", { "-p", NULL }, false, true },
    /* OpenClaw and Hermes are listed for what they do in a PANE (the Translate
     * chord, a dropped file's path) and are NOT handed a prompt: their
     * headless modes are unverified, and Hermes' one-shot (`hermes -z`) takes
     * the user's toolsets, memory and rules with approvals bypassed, with no
     * known switch that turns them off. headless = false keeps both out of
     * Create Theme and out of the Translate agent backend. */
    { "openclaw", "OpenClaw", "openclaw", { NULL }, false, false },
    { "hermes",   "Hermes",   "hermes",   { NULL }, false, false },
    /* Listed for their panes the same way, and handed no prompt: their
     * one-shot modes are not verified here. */
    { "devin",      "Devin",       "devin",       { NULL }, false, false },
    { "qoder",      "Qoder",       "qodercli",    { NULL }, false, false },
    { "letta",      "Letta Code",  "letta",       { NULL }, false, false },
    { "mastracode", "Mastra Code", "mastracode",  { NULL }, false, false },
    { "maki",       "Maki",        "maki",        { NULL }, false, false },
    { "muse",       "Muse Code",   "muse",        { NULL }, false, false },
    { "prime",      "Prime Agent", "prime-agent", { NULL }, false, false },
};

static const i32 g_known_agent_count =
    (i32)(sizeof(g_known_agents) / sizeof(g_known_agents[0]));

/* Build "<dir>/<binary>" with leading '~' expanded, then check execute. */
static bool probe_dir(const char *dir, usize dlen, const char *binary,
                       char *out_path, usize cap) {
    char buf[AGENT_PATH_CAP];
    usize off = 0;
    if (dlen >= 1 && dir[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) return false;
        usize hn = strlen(home);
        if (hn + dlen + strlen(binary) + 2 >= sizeof(buf)) return false;
        memcpy(buf + off, home, hn); off += hn;
        memcpy(buf + off, dir + 1, dlen - 1); off += dlen - 1;
    } else {
        if (dlen + strlen(binary) + 2 >= sizeof(buf)) return false;
        memcpy(buf + off, dir, dlen); off += dlen;
    }
    buf[off++] = '/';
    snprintf(buf + off, sizeof(buf) - off, "%s", binary);
    struct stat st;
    if (stat(buf, &st) != 0) return false;
    if ((st.st_mode & S_IFMT) != S_IFREG) return false;
    if (access(buf, X_OK) != 0) return false;
    snprintf(out_path, cap, "%s", buf);
    return true;
}

/* Curated set of common CLI install dirs that GUI launches (Finder, Dock,
 * `open -a`) miss because launchd hands apps a stripped PATH
 * (`/usr/bin:/bin:...`) without `/opt/homebrew/bin`, `~/.local/bin`, etc.
 * Shared by find_in_path() (agent autodetect) and agent_ensure_process_path()
 * (which folds these into the process PATH so spawned agents inherit them).
 * Entries beginning with '~' are $HOME-expanded at use. */
static const char *const g_path_extras[] = {
    "/opt/homebrew/bin",
    "/opt/homebrew/sbin",
    "/usr/local/bin",
    "/usr/local/sbin",
    "~/.local/bin",
    "~/bin",
    "~/.cargo/bin",
    "~/.bun/bin",
    "~/.volta/bin",
    "~/go/bin",
    "~/.grok/bin",
    "~/.opencode/bin",
    "~/.deno/bin",
    "~/.npm-global/bin",
    "~/.npm/bin",
    "~/.yarn/bin",
    "~/.kiro/bin",
    "~/.crush/bin",
    "~/.kimi/bin",
    "~/.qwen/bin",
    "~/.antigravity/bin",
};
static const usize g_path_extras_count =
    sizeof(g_path_extras) / sizeof(g_path_extras[0]);

/* Resolve a binary by walking $PATH, then the curated fallback dirs above.
 * Without these fallbacks an installed `claude` appears as "not installed"
 * the moment Liu is launched as an .app. */
static bool find_in_path(const char *binary, char *out_path, usize cap) {
    const char *path = getenv("PATH");
    if (path && *path) {
        const char *p = path;
        while (*p) {
            const char *colon = strchr(p, ':');
            usize dlen = colon ? (usize)(colon - p) : strlen(p);
            if (dlen > 0 && probe_dir(p, dlen, binary, out_path, cap)) return true;
            if (!colon) break;
            p = colon + 1;
        }
    }
    for (usize i = 0; i < g_path_extras_count; i++) {
        if (probe_dir(g_path_extras[i], strlen(g_path_extras[i]), binary, out_path, cap))
            return true;
    }
    return false;
}

/* True if `path` (a ':'-joined PATH) already contains the exact directory
 * `seg` as one of its segments. */
static bool path_has_segment(const char *path, const char *seg, usize slen) {
    if (!path || !*path || !slen) return false;
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        usize dlen = colon ? (usize)(colon - p) : strlen(p);
        if (dlen == slen && strncmp(p, seg, slen) == 0) return true;
        if (!colon) break;
        p = colon + 1;
    }
    return false;
}

/* Expand a leading '~' to $HOME. Returns the written length (0 on failure). */
static usize expand_tilde(const char *dir, char *out, usize cap) {
    if (dir[0] == '~') {
        const char *home = getenv("HOME");
        if (!home || !*home) return 0;
        int n = snprintf(out, cap, "%s%s", home, dir + 1);
        return (n > 0 && (usize)n < cap) ? (usize)n : 0;
    }
    int n = snprintf(out, cap, "%s", dir);
    return (n > 0 && (usize)n < cap) ? (usize)n : 0;
}

/* Repair the process PATH so agents Liu posix_spawns (Create Theme, Create Note,
 * the Translate agent backend, …) can actually run.
 *
 * On macOS a GUI launch (Finder / Dock / `open -a` / a .app bundle) inherits
 * launchd's stripped PATH — "/usr/bin:/bin:/usr/sbin:/sbin" — which omits Homebrew,
 * npm-global, nvm, cargo, and every user-local bin dir. Each spawned child is
 * handed `environ` verbatim, so a spawned `claude` (itself a `#!/usr/bin/env node`
 * wrapper) can't find `node` on that PATH and dies before printing a byte — the
 * flow just silently fails. Local PTY tabs escape this because the login shell
 * rebuilds PATH from the user's dotfiles, but direct spawns don't get that. So the
 * symptom is "works from the terminal, fails as an installed .app / on another
 * machine."
 *
 * Fix it once, centrally and early: ask the user's login shell for its real PATH
 * (captures nvm/asdf/fnm/volta dynamic dirs that a static list can't), and fold in
 * the curated static install dirs as a fallback, then setenv() the result so every
 * subsequent child — and every PATH-based lookup in-process — sees the full PATH.
 * No-op on non-macOS, where GUI launches keep a usable PATH. */
void agent_ensure_process_path(void) {
#ifdef __APPLE__
    /* Must start terminated: when neither the login shell nor $PATH yields a
     * base (step 1 writes nothing), the very first path_has_segment() below
     * still reads this buffer — over uninitialised stack if it isn't NUL'd. */
    char merged[8192] = {0};
    usize off = 0;

    /* 1) Base: the login shell's PATH when it answers, else the current one. */
    char shell_path[6144] = {0};
    const char *shell = getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/zsh";
    {
        (void)agent_login_shell_path(shell, shell_path, sizeof shell_path,
                                     AGENT_LOGIN_PATH_DEADLINE_MS);
    }

    const char *base = shell_path[0] ? shell_path : getenv("PATH");
    if (base && *base) {
        int n = snprintf(merged + off, sizeof(merged) - off, "%s", base);
        if (n > 0 && (usize)n < sizeof(merged) - off) off += (usize)n;
    }

    /* 2) Fold in curated static dirs not already present (so a broken/empty
     *    shell PATH still gets Homebrew, npm-global, cargo, etc.). */
    for (usize i = 0; i < g_path_extras_count; i++) {
        char dir[1024];
        usize dlen = expand_tilde(g_path_extras[i], dir, sizeof dir);
        if (!dlen) continue;
        if (path_has_segment(merged, dir, dlen)) continue;
        int n = snprintf(merged + off, sizeof(merged) - off,
                         off ? ":%s" : "%s", dir);
        if (n > 0 && (usize)n < sizeof(merged) - off) off += (usize)n;
    }

    if (off > 0) setenv("PATH", merged, 1);
#endif
}

const char *agent_id_for_basename(const char *name) {
    if (!name || !*name) return "";
    for (i32 i = 0; i < g_known_agent_count; i++) {
        if (strcmp(name, g_known_agents[i].binary) == 0) return g_known_agents[i].id;
    }
    /* The other spellings of a process an agent runs as (core/agent_names.h):
     * Command Code 1.56+ titles itself `command-code`, and a table that knew
     * only the bin `commandcode` left the chord dead in its pane. */
    const char *id = agent_names_id(name, false);
    if (id[0]) {
        for (i32 i = 0; i < g_known_agent_count; i++) {
            if (strcmp(id, g_known_agents[i].id) == 0) return g_known_agents[i].id;
        }
    }
    return "";
}

i32 agent_detect_headless(AgentInfo *out, i32 cap) {
    i32 n = agent_detect_available(out, cap);
    i32 kept = 0;
    for (i32 i = 0; i < n; i++) {
        if (!out[i].headless) continue;
        if (kept != i) out[kept] = out[i];
        kept++;
    }
    return kept;
}

i32 agent_detect_available(AgentInfo *out, i32 cap) {
    if (!out || cap <= 0) return 0;

    i32 n = 0;
    for (i32 i = 0; i < g_known_agent_count && n < cap; i++) {
        const AgentSpec *s = &g_known_agents[i];
        char resolved[AGENT_PATH_CAP];
        if (!find_in_path(s->binary, resolved, sizeof(resolved))) continue;

        AgentInfo *a = &out[n++];
        snprintf(a->id,         sizeof(a->id),         "%s", s->id);
        snprintf(a->display,    sizeof(a->display),    "%s", s->display);
        snprintf(a->binary,     sizeof(a->binary),     "%s", s->binary);
        snprintf(a->path,       sizeof(a->path),       "%s", resolved);
        a->args_count = 0;
        for (i32 k = 0; k < AGENT_MAX_ARGS && s->args[k]; k++) {
            snprintf(a->args[k], sizeof(a->args[k]), "%s", s->args[k]);
            a->args_count++;
        }
        a->stdin_prompt = s->stdin_prompt;
        a->headless     = s->headless;
    }
    return n;
}
