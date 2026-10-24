/* The remote shell's terminal environment, for SSH sessions.
 *
 * A local tab's child is started with COLORTERM=truecolor and TERM_PROGRAM=Liu
 * (ssh_session.c, local_env defaults). Over SSH the pty request carries TERM
 * alone, so everything that decides truecolor from COLORTERM (chalk/ink: Claude
 * Code, Command Code; termenv and lipgloss: opencode; most Rust
 * `supports-color` crates) would downgrade to the 256-colour palette on the
 * remote side — Claude Code, for one, draws its header in `38;5;220` (palette
 * yellow) where a local tab receives `38;2;215;119;87` (its orange). The remote
 * programs pick a smaller palette because the terminal never announced the
 * bigger one.
 *
 * The polite route is a `setenv` channel request, and it is tried first — but
 * sshd's default `AcceptEnv` is `LANG LC_*`, so a stock server refuses every
 * variable here. When COLORTERM is refused the shell is started through an
 * `exec` request running the bootstrap below: `env` sets the variables and
 * execs the user's login shell, which is what sshd's own shell request does
 * (`$SHELL` with `-l`), so rc files, shell integration and the login semantics
 * are unchanged. `env` rather than `VAR=value cmd` because csh/tcsh (FreeBSD's
 * root) cannot parse the assignment prefix; `"$SHELL"` because sshd always sets
 * it from passwd. A server whose banner names Windows keeps the plain shell
 * request — cmd.exe and PowerShell do not speak the bootstrap — and so does one
 * that declines the exec request.
 *
 * Pure, so the string and the decision need no server.
 */
#ifndef LIU_SSH_ENV_BOOTSTRAP_H
#define LIU_SSH_ENV_BOOTSTRAP_H

#include "core/types.h"
#include <stdio.h>
#include <string.h>

/* The variables a Liu terminal announces to the programs it hosts: what
 * colour and terminal detection read, and nothing that forces colour into a
 * pipe (the local child's environment is held to the same rule,
 * local_env_build in ssh_session.c). */
static inline i32 ssh_env_bootstrap_vars(const char *version,
                                         char out[][80], i32 cap) {
    i32 n = 0;
    if (n < cap) snprintf(out[n++], 80, "COLORTERM=truecolor");
    if (n < cap) snprintf(out[n++], 80, "TERM_PROGRAM=Liu");
    if (n < cap && version && version[0]) {
        /* A version that cannot be spelled as a single shell word is left
         * out: it would otherwise become a second argument to env. */
        bool clean = true;
        for (const char *p = version; *p; p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= 'A' && *p <= 'Z') || *p == '.' || *p == '-' || *p == '+' || *p == '_'))
                clean = false;
        if (clean) snprintf(out[n++], 80, "TERM_PROGRAM_VERSION=%s", version);
    }
    return n;
}

/* The exec-request command line. Returns false if it does not fit. */
static inline bool ssh_env_bootstrap_command(const char *version, char *out, usize cap) {
    char vars[4][80];
    i32 n = ssh_env_bootstrap_vars(version, vars, 4);
    usize used = 0;
    int w = snprintf(out, cap, "exec env");
    if (w < 0 || (usize)w >= cap) return false;
    used = (usize)w;
    for (i32 i = 0; i < n; i++) {
        w = snprintf(out + used, cap - used, " %s", vars[i]);
        if (w < 0 || (usize)w >= cap - used) return false;
        used += (usize)w;
    }
    w = snprintf(out + used, cap - used, " \"$SHELL\" -l");
    return w >= 0 && (usize)w < cap - used;
}

/* A folder the remote shell may be started in. The path is written into the
 * exec-request line inside single quotes, and that line is read by whatever
 * login shell the server's account has (sh, bash, zsh, fish, csh, tcsh), which
 * disagree about what a quote protects: `'`, `\` (an escape in fish's quotes)
 * and `!` (csh's history) are refused rather than spelled, and so is anything
 * that is not a plain absolute path. A path the user's own remote shell
 * reported (OSC 7) is a program's output: this is the one place it is trusted
 * to become part of a command, and it is trusted only as far as it is tame. */
static inline bool ssh_start_dir_ok(const char *dir) {
    if (!dir || dir[0] != '/') return false;
    usize n = strlen(dir);
    if (n >= 400) return false;
    for (const char *p = dir; *p; p++) {
        u8 c = (u8)*p;
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '\'' || c == '\\' || c == '!') return false;
    }
    return true;
}

/* The same line, started in `start_dir`: the folder is entered only when it is
 * one (`[ -d … ] &&` — a failed `cd` aborts a csh command line, and a shell
 * that never starts is worse than one that starts at home), then the shell is
 * exec'd exactly as ssh_env_bootstrap_command() does. A NULL or refused folder
 * gives that function's line unchanged. */
static inline bool ssh_env_bootstrap_command_in(const char *version,
                                                const char *start_dir,
                                                char *out, usize cap) {
    if (!ssh_start_dir_ok(start_dir))
        return ssh_env_bootstrap_command(version, out, cap);
    int w = snprintf(out, cap, "[ -d '%s' ] && cd '%s'; ", start_dir, start_dir);
    if (w < 0 || (usize)w >= cap) return false;
    return ssh_env_bootstrap_command(version, out + w, cap - (usize)w);
}

/* Does this server get the bootstrap when it refuses setenv? The banner is
 * what the server said in its version line ("SSH-2.0-OpenSSH_for_Windows_9.5");
 * NULL or empty is treated as a POSIX host, which is what every other server
 * in the wild is. */
static inline bool ssh_env_bootstrap_applicable(const char *banner) {
    if (!banner || !banner[0]) return true;
    return strstr(banner, "Windows") == NULL && strstr(banner, "windows") == NULL;
}

#endif /* LIU_SSH_ENV_BOOTSTRAP_H */
