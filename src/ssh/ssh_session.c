/*
 * Liu - SSH + local PTY session implementation
 */
#include "ssh/ssh_session.h"
#include "ssh/ssh_env_bootstrap.h"
#include "core/config.h"       /* liu_executable_dir() */
#include "core/memory.h"       /* secure_zero() */
#include "core/checked.h"
#include "core/path_escape.h"   /* liu_path_has_control_bytes() */
#include "vault/crypto.h"      /* crypto_random() — CSPRNG for X11 cookie */
#include "ssh/x11_auth.h"       /* who may use the forwarded display */
#include "platform/platform.h" /* platform_unwatch_socket — cancel dispatch source before close */
#include "core/pty_reader.h"   /* local PTY gather thread */
#include "core/mem_budget.h"   /* process-wide argv scratch is retained I/O */
#include "core/agent_names.h"   /* which agent a process name is */
#include "ssh/local_cwd_ready.h" /* the fork->chdir window the cwd probe must not read */
#include "ssh/local_lang.h"      /* which locale a pane's programs start in */
#include "ssh/child_signals.h"   /* the signals a pane's programs start with */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>   /* UINT_MAX — not transitively included on glibc */
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <strings.h>
#include <time.h>
#include <math.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <pwd.h>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>    /* newlocale / freelocale */
#endif
#if defined(PLATFORM_MACOS)
    #include <util.h>
    #include <libproc.h>
    #include <sys/proc.h>
    #include <sys/sysctl.h>   /* KERN_PROCARGS2 — resolve node-script CLIs */
#else
    #include <pty.h>
    #include <dirent.h>       /* /proc walk — session_background_jobs() */
#endif

/* Foreground-process cache timing. BASE is the responsive rate the agent icon
 * and the accent bar are tuned around; MAX is where the backoff for panes with
 * no agent stops (see Session.fg_cache_ttl). Both are seconds. */
#define SESSION_FG_TTL_BASE 0.6
#define SESSION_FG_TTL_MAX  4.8

/* Not macOS-only, despite living next to the macOS process helpers below:
 * core/pty_reader.c is plain pthreads over a pipe, and every call site is
 * unguarded. Leaving this inside that block built the callers against an
 * implicit declaration everywhere else. */
/* ON by default; LIU_PTY_THREAD=0 turns it off completely: session_read()
 * reads the pty master inline, session_io_fd() hands the fd back to
 * platform_watch_socket, session_read_pending() is 0 so no backlog is ever
 * latched, and no thread is spawned. That single NULL `s->reader` is what
 * every other site keys off, so the "off" arm is the inline reader exactly
 * rather than an approximation of it — the same role LIU_PACE=0 plays for the
 * present grid. */
static bool session_pty_thread_enabled(void) {
    static i32 v = -1;
    if (v < 0) { const char *e = getenv("LIU_PTY_THREAD"); v = (e && atoi(e) == 0) ? 0 : 1; }
    return v != 0;
}

/* Outside the macOS block below: the snapshot's TTL uses it there, and the
 * first-second exit check (born_mono / lived_sec) uses it on every platform. */
static f64 session_mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

#ifdef PLATFORM_MACOS

static bool session_proc_name_is_agent(const char *name) {
    return agent_names_id(name, false)[0] != '\0';
}

static bool session_proc_name_is_wrapper(const char *name) {
    if (!name || !name[0]) return true;
    /* python, python3, python3.14, Python (the framework build): Hermes Agent
     * is a Python program that does not rename its process, so the interpreter
     * is all the kernel names and the script is in argv. */
    if (strncasecmp(name, "python", 6) == 0) return true;
    return strcmp(name, "sh") == 0 ||
           strcmp(name, "bash") == 0 ||
           strcmp(name, "zsh") == 0 ||
           strcmp(name, "fish") == 0 ||
           strcmp(name, "node") == 0 ||
           strcmp(name, "npm") == 0 ||
           strcmp(name, "npx") == 0 ||
           strcmp(name, "bun") == 0 ||
           strcmp(name, "env") == 0;
}

#ifdef LIU_AGENT_ARGV_PROBE
static u64 g_argv_probe_count;
static u64 g_argv_probe_ns;
static u64 g_argv_probe_max_ns;
static u64 g_argv_argmax_count;
static u64 g_argv_procargs_count;
static u64 g_argv_malloc_bytes;

static u64 argv_probe_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

void session_argv_probe_stats(u64 *probe_count, u64 *probe_ns,
                              u64 *probe_max_ns, u64 *argmax_count,
                              u64 *procargs_count, u64 *malloc_bytes) {
    if (probe_count) *probe_count = g_argv_probe_count;
    if (probe_ns) *probe_ns = g_argv_probe_ns;
    if (probe_max_ns) *probe_max_ns = g_argv_probe_max_ns;
    if (argmax_count) *argmax_count = g_argv_argmax_count;
    if (procargs_count) *procargs_count = g_argv_procargs_count;
    if (malloc_bytes) *malloc_bytes = g_argv_malloc_bytes;
}
#endif

/* Case-insensitive substring search (ASCII). strcasestr is a BSD/GNU extension
 * and not portable across the platforms this file builds on. */
static char session_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool session_str_has_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && session_lower_ascii(*a) == session_lower_ascii(*b)) {
            a++; b++;
        }
        if (!*b) return true;
    }
    return false;
}

/* CLIs that rename their own process with setproctitle.
 *
 * Kimi Code and Command Code both do it, and it defeats every other probe:
 * the accounting name stays the interpreter's ("python3.13", "node"), the
 * executable path is the interpreter's too (…/cpython-3.13.9/bin/python3.13),
 * and the argv the kernel hands back has been overwritten with the product
 * title the tool paints — "Kimi Code", "⌘ Command Code · liuterm". There is no
 * basename left to match, so match the title and report the canonical id. */
static bool session_proc_title_agent(const char *arg, char *out, usize out_cap) {
    static const struct { const char *title, *id; } titles[] = {
        { "Kimi Code",    "kimi"        },
        { "Command Code", "commandcode" },
    };
    for (usize i = 0; i < sizeof titles / sizeof titles[0]; i++) {
        if (session_str_has_ci(arg, titles[i].title)) {
            snprintf(out, out_cap, "%s", titles[i].id);
            return true;
        }
    }

    /* Command Code 1.4x carries its product name only for the first few
     * seconds. Once a conversation has a name it retitles itself to THAT — an
     * argv such as "⌘ Count to 100" — so matching the product string alone
     * would lose the process exactly when the user starts working, and the tab
     * would drop its agent icon and activity bar mid-turn. (1.56 moved all of
     * that to the window title and pins argv[0] to the bare bin name
     * "command-code"; session_proc_argv_agent's argv[0] rung takes that shape.
     * Both are handled: either version may be installed.)
     *
     * What every one of its titles keeps is the leading ⌘ (U+2318) sigil, the
     * same mark Liu draws as Command Code's icon. Anchored at the start (not
     * searched for), so a title that merely mentions the character elsewhere —
     * a chat named after a keyboard shortcut — cannot claim the process. */
    if ((unsigned char)arg[0] == 0xE2 &&
        (unsigned char)arg[1] == 0x8C &&
        (unsigned char)arg[2] == 0x98) {
        snprintf(out, out_cap, "%s", "commandcode");
        return true;
    }
    return false;
}

/* A node/bun/python wrapper hides the real CLI behind the interpreter's
 * process name ("node"). Read the process arguments (KERN_PROCARGS2) and
 * return the basename of the first non-flag argv entry that names a known
 * agent — that's the script path, e.g. ".../commandcode" → "commandcode".
 * This is what lets script-based CLIs (codex, commandcode, grok …)
 * be recognized the same way native binaries (claude, opencode) already are. */
/* All callers are on the main thread (session_fg_process). A single process-wide
 * buffer therefore removes allocator churn without a lock or per-pane
 * duplication. KERN_ARGMAX is invariant for the process lifetime. */
static char  *g_procargs_scratch;
static usize  g_procargs_scratch_cap;
static bool   g_procargs_cleanup_registered;

static void session_procargs_cleanup(void) {
    if (g_procargs_scratch_cap)
        mem_budget_adjust(MEM_PTY_IO, -(isize)g_procargs_scratch_cap);
    free(g_procargs_scratch);
    g_procargs_scratch = NULL;
    g_procargs_scratch_cap = 0;
}

static char *session_procargs_buffer(usize *cap_out) {
    if (!g_procargs_scratch) {
        int argmax = 0;
        size_t am_sz = sizeof argmax;
        int mib_am[2] = { CTL_KERN, KERN_ARGMAX };
#ifdef LIU_AGENT_ARGV_PROBE
        g_argv_argmax_count++;
#endif
        if (sysctl(mib_am, 2, &argmax, &am_sz, NULL, 0) != 0 || argmax <= 0)
            argmax = 256 * 1024;
        char *buf = (char *)malloc((size_t)argmax);
#ifdef LIU_AGENT_ARGV_PROBE
        g_argv_malloc_bytes += (u64)argmax;
#endif
        if (!buf) return NULL;
        g_procargs_scratch = buf;
        g_procargs_scratch_cap = (usize)argmax;
        mem_budget_adjust(MEM_PTY_IO, (isize)g_procargs_scratch_cap);
        if (!g_procargs_cleanup_registered) {
            atexit(session_procargs_cleanup);
            g_procargs_cleanup_registered = true;
        }
    }
    if (cap_out) *cap_out = g_procargs_scratch_cap;
    return g_procargs_scratch;
}

static bool session_proc_argv_agent(pid_t pid, char *out, usize out_cap) {
#ifdef LIU_AGENT_ARGV_PROBE
    u64 probe_t0 = argv_probe_now_ns();
#endif
    usize scratch_cap = 0;
    char *buf = session_procargs_buffer(&scratch_cap);
    if (!buf) goto done_false;

    size_t bsz = scratch_cap;
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, (int)pid };
#ifdef LIU_AGENT_ARGV_PROBE
    g_argv_procargs_count++;
#endif
    if (sysctl(mib, 3, buf, &bsz, NULL, 0) != 0 || bsz < sizeof(int)) {
        goto done_false;
    }

    int argc = 0;
    memcpy(&argc, buf, sizeof argc);
    char *p   = buf + sizeof(int);
    char *end = buf + bsz;

    /* Layout: argc | exec_path\0 | \0-padding | argv[0]\0 argv[1]\0 … */
    while (p < end && *p != '\0') p++;   /* skip exec_path        */
    while (p < end && *p == '\0') p++;   /* skip padding to argv0 */

    bool found = false;
    bool py0 = false;       /* argv[0] is a Python interpreter */
    for (int ai = 0; ai < argc && p < end; ai++) {
        char *arg = p;
        while (p < end && *p != '\0') p++;
        if (p >= end) break;             /* truncated/non-NUL argument */
        p++;                             /* step past the NUL */
        /* argv[0] ONLY. setproctitle rewrites argv[0]; the rest of the vector
         * is the user's own command line, and matching there turns ordinary
         * work into a false agent: `git commit -m "Command Code hooks"` hits
         * the product-name search, and `rg '⌘' src/` hits the sigil. That is
         * not merely a wrong icon — the pane is then a candidate for another
         * tab's Command Code Stop signal, so the done panel and its sound fire
         * on the wrong terminal. */
        if (ai == 0 && session_proc_title_agent(arg, out, out_cap)) {
            found = true;
            break;
        }
        /* A CLI that retitles itself to its own BIN name. Command Code 1.56
         * sets process.title = "command-code" (ucomm "node", argv[0]
         * "command-code", argv[1] ""), where 1.4x wrote "⌘ Command Code ·
         * <dir>" — that string moved to the window title (OSC 0/2), which is
         * not visible from here. The product-name and sigil matches above
         * both miss it, the interpreter rung is skipped because pbi_name is
         * "node", and argv[1] is empty, so without this rung the pane would
         * read as a plain shell: no icon, no working bar, no done panel. The
         * basename of argv[0] against the same list the script-path rung uses
         * catches every spelling the package links (command-code,
         * commandcode, cmd, cmdc) and cannot fire for an interpreter, whose
         * argv[0] is "node"/"bun"/"python3". */
        if (ai == 0) {
            const char *bn0 = strrchr(arg, '/');
            bn0 = bn0 ? bn0 + 1 : arg;
            /* Liu's own helper: `liu-agent spawn codex …` names the agent it
             * asks Liu to start, it is not that agent. */
            if (!strcmp(bn0, "liu-agent") || !strcmp(bn0, "liu-notify")) break;
            if (session_proc_name_is_agent(bn0)) {
                snprintf(out, out_cap, "%s", bn0);
                found = true;
                break;
            }
            py0 = strncasecmp(bn0, "python", 6) == 0;
            continue;                    /* argv[0] = interpreter */
        }
        /* Hermes Agent never names itself in argv: its launcher execs
         * `<python> -I -c "<script>" args…` (hermes_cli/_launchers.py), the
         * script ending in `from hermes_cli.main import main`, and a source
         * checkout runs `python -m hermes_cli.main`. Only an interpreter's own
         * arguments are looked at, for the module's name alone — the rest of
         * a vector is the user's command line (see the argv[0] rule above). */
        if (py0 && strstr(arg, "hermes_cli.main")) {
            snprintf(out, out_cap, "%s", "hermes");
            found = true;
            break;
        }
        const char *bn = strrchr(arg, '/');
        bn = bn ? bn + 1 : arg;
        if (bn[0] == '-' || bn[0] == '\0') continue;   /* a flag */
        if (session_proc_name_is_agent(bn)) {
            snprintf(out, out_cap, "%s", bn);
            found = true;
            break;
        }
    }
#ifdef LIU_AGENT_ARGV_PROBE
    {
        u64 ns = argv_probe_now_ns() - probe_t0;
        g_argv_probe_count++; g_argv_probe_ns += ns;
        if (ns > g_argv_probe_max_ns) g_argv_probe_max_ns = ns;
    }
#endif
    return found;

done_false:
#ifdef LIU_AGENT_ARGV_PROBE
    {
        u64 ns = argv_probe_now_ns() - probe_t0;
        g_argv_probe_count++; g_argv_probe_ns += ns;
        if (ns > g_argv_probe_max_ns) g_argv_probe_max_ns = ns;
    }
#endif
    return false;
}

#ifdef LIU_AGENT_ARGV_PROBE
bool session_argv_probe_for_bench(i32 pid, char *out, usize out_cap) {
    return session_proc_argv_agent((pid_t)pid, out, out_cap);
}
#endif

/* A CLI compiled with Bun/PyInstaller re-execs into a versioned binary whose
 * accounting name is a bare version string — Claude Code installs at
 * ~/.local/share/claude/versions/<ver> and runs with comm "2.1.150", so it is
 * neither a known agent name nor an interpreter whose argv can be scanned. As
 * a last resort, inspect the executable path itself: if any path component
 * names a known agent (e.g. ".../claude/versions/2.1.150" → "claude"), use it.
 * The >=5-char guard keeps short, ambiguous ids (amp, cmd, gpt) from matching
 * an incidental directory name. */
static bool session_proc_path_agent(pid_t pid, char *out, usize out_cap) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof path) <= 0) return false;
    char *save = NULL;
    for (char *tok = strtok_r(path, "/", &save); tok;
         tok = strtok_r(NULL, "/", &save)) {
        if (strlen(tok) >= 5 && session_proc_name_is_agent(tok)) {
            /* Not these four: each runs behind an interpreter that names it in
             * argv (the rung that does that is above), and "hermes" or "letta"
             * is as likely a folder somebody's own project lives in — any
             * native binary built under it would be taken for the agent. */
            if (strcmp(tok, "hermes") == 0 || strcmp(tok, "openclaw") == 0 ||
                strcmp(tok, "letta") == 0 || strcmp(tok, "mastracode") == 0) continue;
            snprintf(out, out_cap, "%s", tok);
            return true;
        }
    }
    return false;
}

/* A CLI shipped as a single self-contained native binary often runs under a
 * versioned process name: grok installs to ~/.grok and runs as
 * "grok-0.2.22-macos-aarch64" (comm truncated to "grok-0.2.22-mac"). Match the
 * leading token — up to the first '-' or '.' — against the known-agent set.
 * The delimiter must be present (a bare exact name is already handled by
 * session_proc_name_is_agent), and the leading token must itself be an exact
 * agent id, so dashed ids like "amazon-q" / "command-code" can't partial-match
 * (their head "amazon" / "command" is not a known agent). */
static bool session_proc_name_versioned_agent(const char *name,
                                              char *out, usize out_cap) {
    if (!name || !name[0]) return false;
    char head[32];
    usize n = 0;
    while (name[n] && name[n] != '-' && name[n] != '.' && n + 1 < sizeof head) {
        head[n] = name[n];
        n++;
    }
    head[n] = '\0';
    if (name[n] != '-' && name[n] != '.') return false;   /* no version suffix */
    if (!session_proc_name_is_agent(head)) return false;
    snprintf(out, out_cap, "%s", head);
    return true;
}

/* `out_found_agent` (optional) distinguishes a real agent hit from the
 * name-of-something-else fallbacks at the bottom. The caller uses it to decide
 * how long to trust the answer: "some process named zsh" is the steady state of
 * an idle pane and is worth caching for much longer than an agent name is. */
/* ---------------------------------------------------------------------------
 * One process snapshot per scheduler interval
 *
 * The group sweep behind session_find_interesting_pgrp_process() is
 * proc_listpids(PROC_ALL_PIDS) followed by a proc_pidinfo per pid — one full
 * sweep of every process on the machine. It is the fallback, but it is the
 * fallback for the ordinary case: an idle shell leader is not an agent, not a
 * versioned agent, not a wrapper and has no agent in its argv, so the group
 * sweep runs. Run PER PANE, a sweep that costs far more than the leader fast
 * path next to it is multiplied by the number of panes in a single frame.
 *
 * The pid->(pgid,name) table is the same for every pane asked in the same
 * instant, so it is taken ONCE and shared. The snapshot's own TTL is well under
 * the per-session TTL that gates these calls, so the worst extra staleness is a
 * fraction of an answer that was already 0.6 s old. */
#define SESSION_PROC_SNAP_TTL 0.25

typedef struct {
    pid_t pid;
    pid_t pgid;
    char  name[MAXCOMLEN + 1];
} SessionProcRow;

static SessionProcRow *g_proc_snap      = NULL;
static i32             g_proc_snap_n    = 0;
static i32             g_proc_snap_cap  = 0;
static f64             g_proc_snap_at   = 0.0;
/* Diagnostics: how many sweeps actually happened, and how much they cost.
 * Main-thread only, like every caller of session_fg_process(). */
static u64             g_proc_snap_sweeps = 0;
static f64             g_proc_snap_secs   = 0.0;

void session_proc_scan_stats(u64 *out_sweeps, f64 *out_seconds);
void session_proc_scan_stats(u64 *out_sweeps, f64 *out_seconds) {
    if (out_sweeps)  *out_sweeps  = g_proc_snap_sweeps;
    if (out_seconds) *out_seconds = g_proc_snap_secs;
}

/* Refresh the snapshot if it is older than its TTL. Returns false only when
 * there is nothing usable to read. */
static bool session_proc_snapshot(void) {
    f64 now = session_mono_now();
    if (g_proc_snap && g_proc_snap_n > 0 &&
        now - g_proc_snap_at < SESSION_PROC_SNAP_TTL) return true;

    i32 bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bytes <= 0) return g_proc_snap_n > 0;    /* keep the last good one */
    pid_t *pids = (pid_t *)malloc((usize)bytes);
    if (!pids) return g_proc_snap_n > 0;

    i32 got = proc_listpids(PROC_ALL_PIDS, 0, pids, bytes);
    i32 count = got > 0 ? got / (i32)sizeof(pid_t) : 0;
    if (count > g_proc_snap_cap) {
        i32 cap = count + count / 4 + 64;
        SessionProcRow *rows = (SessionProcRow *)realloc(g_proc_snap,
                                   (usize)cap * sizeof *rows);
        if (!rows) { free(pids); return g_proc_snap_n > 0; }
        g_proc_snap = rows;
        g_proc_snap_cap = cap;
    }

    i32 n = 0;
    for (i32 i = 0; i < count && n < g_proc_snap_cap; i++) {
        if (pids[i] <= 0) continue;
        struct proc_bsdinfo bi;
        if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &bi, sizeof bi)
            != (i32)sizeof bi) continue;
        if (!bi.pbi_name[0]) continue;
        g_proc_snap[n].pid  = pids[i];
        g_proc_snap[n].pgid = (pid_t)bi.pbi_pgid;
        snprintf(g_proc_snap[n].name, sizeof g_proc_snap[n].name, "%s", bi.pbi_name);
        n++;
    }
    free(pids);
    g_proc_snap_n  = n;
    g_proc_snap_at = now;
    g_proc_snap_sweeps++;
    g_proc_snap_secs += session_mono_now() - now;
    return n > 0;
}

static bool session_find_interesting_pgrp_process(pid_t pgid,
                                                  const char *leader,
                                                  char *out, usize out_cap,
                                                  bool *out_found_agent) {
    if (out_found_agent) *out_found_agent = false;
    if (!out || out_cap == 0) return false;
    out[0] = '\0';
    if (pgid <= 0) return false;

    if (!session_proc_snapshot()) return false;
    i32 count = g_proc_snap_n;
    char fallback[256] = {0};

    for (i32 i = 0; i < count; i++) {
        const SessionProcRow *row = &g_proc_snap[i];
        if (row->pgid != pgid) continue;
        /* The snapshot row under the names of proc_pidinfo's fields
         * (`bi.pbi_name`), which the body below reads. */
        struct { const char *pbi_name; } bi = { row->name };
        pid_t pids_i = row->pid;

        if (session_proc_name_is_agent(bi.pbi_name)) {
            snprintf(out, out_cap, "%s", bi.pbi_name);
            if (out_found_agent) *out_found_agent = true;
            return true;
        }
        /* Versioned native binary (e.g. grok's "grok-0.2.22-macos-aarch64").
         * Recover the leading agent id before falling through to wrapper/argv. */
        if (session_proc_name_versioned_agent(bi.pbi_name, out, out_cap)) {
            if (out_found_agent) *out_found_agent = true;
            return true;
        }
        if (session_proc_name_is_wrapper(bi.pbi_name)) {
            /* node/bun/python interpreter → inspect its argv for the script. */
            if (session_proc_argv_agent(pids_i, out, out_cap)) {
                if (out_found_agent) *out_found_agent = true;
                return true;
            }
        } else {
            /* Opaque name (e.g. a Bun-compiled CLI re-exec'd as its bare
             * version string, like Claude Code's "2.1.150") → recover the
             * agent from the executable path before treating it as fallback. */
            if (session_proc_path_agent(pids_i, out, out_cap)) {
                if (out_found_agent) *out_found_agent = true;
                return true;
            }
            if (!fallback[0])
                snprintf(fallback, sizeof fallback, "%s", bi.pbi_name);
        }
    }

    if (fallback[0]) {
        snprintf(out, out_cap, "%s", fallback);
        return true;
    }
    if (leader && leader[0]) {
        snprintf(out, out_cap, "%s", leader);
        return true;
    }
    return false;
}
#else  /* !PLATFORM_MACOS */

/* The snapshot only exists on the macOS process-probing path. */
void session_proc_scan_stats(u64 *out_sweeps, f64 *out_seconds);
void session_proc_scan_stats(u64 *out_sweeps, f64 *out_seconds) {
    if (out_sweeps)  *out_sweeps  = 0;
    if (out_seconds) *out_seconds = 0.0;
}

#endif

#include <libssh2.h>
#include <libssh2_sftp.h>

#ifndef PLATFORM_WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#endif

#include "core/net.h"
#include "core/text_sanitize.h"
#include "ssh/link_policy.h"
#include "ssh/key_protection.h"
#include "ssh/known_hosts.h"
#include "ssh/mosh.h"
#include "ssh/telnet.h"
#include "ssh/serial.h"
#include "ssh/port_forward.h"
#include "ssh/sftp.h"          /* sftp_io_yield / sftp_io_breathe live here */
#include "ssh/streamlocal_open.h" /* a Unix-socket forward's channel open */
#include "ssh/auth_second_factor.h" /* a key plus a code: the second step */
#include "ssh/relay_watch.h"     /* when a relay's socket wakes the loop */
#include "ssh/link_sees.h"       /* a server prompt waiting on the user is not a failure */

#define SESSION_WRITE_BUFFER_LIMIT ((usize)(4u * 1024u * 1024u))
#define SESSION_WRITE_FLUSH_LIMIT  ((usize)(256u * 1024u))
/* Main-thread milliseconds one local flush may spend waiting for the pty to
 * become writable again (see session_flush_pending_write). */
#define SESSION_LOCAL_FLUSH_WAIT_MS 3

struct Session;
static void session_io_kick_init(struct Session *s);

#define SOCKET_RELAY_BUF_CAP 16384
/* One poll moves a relay along until nothing moves (at most this many steps,
 * a step being one chunk each way): what a step leaves undone — the chunk it
 * has just read from the channel, an EOF still to be passed on — is waiting
 * where no descriptor announces it, so a poll that took one step needed a
 * wake per step. */
#define SOCKET_RELAY_POLL_STEPS 16

/* A RemoteForward's destination is nearly always this machine or its LAN: a
 * target that has not answered in this long is not going to, and the remote
 * client is waiting on the other end of the channel meanwhile. */
#define REMOTE_FWD_CONNECT_TIMEOUT_MS 10000

/* A socket <-> SSH channel relay polled by the main thread. Keeping libssh2
 * calls on that thread avoids concurrent access to one LIBSSH2_SESSION. */
typedef struct {
    LIBSSH2_CHANNEL *channel;
    int              fd;
    bool             active;
    /* RemoteForward only: the connect to the local destination, while it is
     * still being made (fd is -1 until it lands). See
     * session_remote_forward_poll_locked. */
    NetConnect      *connecting;
    u8               to_fd[SOCKET_RELAY_BUF_CAP];
    usize            to_fd_off, to_fd_len;
    u8               to_channel[SOCKET_RELAY_BUF_CAP];
    usize            to_channel_off, to_channel_len;
    /* The two directions end separately (same rule as port_forward.c): an EOF
     * from one side is passed on once what it sent is delivered, and the relay
     * closes when both sides have said they are finished — not when the first
     * one does, which would drop the other direction's buffered bytes.
     *   fd_eof     the socket will send no more (recv returned 0)
     *   eof_sent   that was passed to the channel (send_eof done)
     *   chan_eof   the channel will send no more (read returned 0)
     *   shut_wr    the socket was told (shutdown(SHUT_WR) done) */
    bool             fd_eof, eof_sent, chan_eof, shut_wr;
    f64              linger_until;   /* a peer that never half-closes back */
    bool             fd_watched;     /* fd wakes the loop (ssh/relay_watch.h) */
    bool             more;           /* the last poll used its whole budget: come back */
} SocketChannelRelay;
#define SOCKET_RELAY_LINGER_SEC 30.0

static void socket_channel_relay_close(SocketChannelRelay *c);
static bool socket_channel_relay_poll(SocketChannelRelay *c);
static bool socket_channel_relay_step(SocketChannelRelay *c, bool *moved);

/* =========================================================================
 * X11 forwarding relay
 * ========================================================================= */

#define MAX_X11_CHANNELS 16

/* A forwarded-display connection. It is NOT relayed to the local X server until its
 * first packet has proven it knows the session's cookie (ssh/x11_auth.h): until then
 * `relay.fd` is -1 and the only thing read from the channel is the setup request. */
#define X11_AUTH_TIMEOUT_SEC 10.0
typedef struct {
    SocketChannelRelay relay;               /* first: socket_channel_relay_close(&x->relay) */
    bool               authed;
    f64                accepted_at;         /* monotonic seconds */
    usize              setup_len;
    u8                 setup[X11_SETUP_LEN];
} X11Relay;

/* Pending X11 channels received via callback but not yet connected locally */
typedef struct {
    Session         *owner;
    LIBSSH2_CHANNEL *channel;
} PendingX11;

static PendingX11 g_pending_x11[MAX_X11_CHANNELS];
static i32 g_pending_x11_count = 0;
/* The table is process-global and reached from three contexts that share no
 * other lock: the libssh2 callback (inline in whichever thread is driving
 * THAT session), the main thread's poll for a possibly different session, and
 * the tab-trash worker's cleanup. Every touch of the array or its count goes
 * under this mutex; the channel close/free itself runs outside it so a slow
 * close cannot stall the other two. */
static pthread_mutex_t g_pending_x11_mu = PTHREAD_MUTEX_INITIALIZER;

/* X11 callback invoked by libssh2 when the server opens an X11 channel */
static void x11_open_callback(LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
                                const char *shost, int sport, void **abstract) {
    (void)session; (void)shost; (void)sport;
    Session *owner = abstract && *abstract ? (Session *)*abstract : NULL;
    bool queued = false;
    if (owner) {
        pthread_mutex_lock(&g_pending_x11_mu);
        if (g_pending_x11_count < MAX_X11_CHANNELS) {
            g_pending_x11[g_pending_x11_count++] = (PendingX11){ owner, channel };
            queued = true;
        }
        pthread_mutex_unlock(&g_pending_x11_mu);
    }
    if (!queued) {
        /* Too many X11 channels -- reject */
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
    }
}

/* Detach the first pending channel for `owner`; NULL when there is none. */
static LIBSSH2_CHANNEL *x11_pending_take(Session *owner) {
    LIBSSH2_CHANNEL *ch = NULL;
    pthread_mutex_lock(&g_pending_x11_mu);
    for (i32 i = 0; i < g_pending_x11_count; i++) {
        if (g_pending_x11[i].owner != owner) continue;
        ch = g_pending_x11[i].channel;
        g_pending_x11[i] = g_pending_x11[--g_pending_x11_count];
        break;
    }
    pthread_mutex_unlock(&g_pending_x11_mu);
    return ch;
}

static void x11_pending_cleanup(Session *owner) {
    if (!owner) return;
    for (;;) {
        LIBSSH2_CHANNEL *channel = x11_pending_take(owner);
        if (!channel) break;
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
    }
}

/* =========================================================================
 * Agent forwarding relay (ForwardAgent yes)
 *
 * The request alone does nothing: it makes sshd create a socket for the remote
 * shell's SSH_AUTH_SOCK, and every program that connects to it makes sshd open
 * an auth-agent@openssh.com channel BACK to the client. libssh2 refuses that
 * channel unless a callback was registered for it, and ssh-add on the remote
 * host would see an agent that never answers. The callback queues the channel
 * (it runs inside whichever thread is driving this libssh2 session), and the
 * main thread's poll connects it to the local agent ($SSH_AUTH_SOCK) and
 * relays bytes both ways — the same shape as the X11 relay above, minus the
 * cookie: the agent protocol authenticates nothing here, which is why
 * forwarding is opt-in.
 * ========================================================================= */

#ifdef LIBSSH2_CALLBACK_AUTHAGENT
#define MAX_AGENT_CHANNELS 16   /* relayed at once, per connection */
/* Waiting for the poll. Larger than what is relayed: the poll closes the surplus
 * from the main thread, where the callback — which runs inside libssh2's packet
 * handling — would be closing a channel from inside the read it is part of. */
#define MAX_AGENT_PENDING 64

typedef struct {
    Session         *owner;
    LIBSSH2_CHANNEL *channel;
} PendingAgent;

static PendingAgent g_pending_agent[MAX_AGENT_PENDING];
static i32 g_pending_agent_count = 0;
/* Reached from the same three contexts as g_pending_x11 and guarded the same
 * way: the array and its count only under the mutex, the channel close/free
 * outside it. */
static pthread_mutex_t g_pending_agent_mu = PTHREAD_MUTEX_INITIALIZER;

/* Called by libssh2 when the server opens an agent channel. */
static void agent_open_callback(LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
                                void **abstract) {
    (void)session;
    Session *owner = abstract && *abstract ? (Session *)*abstract : NULL;
    bool queued = false;
    if (owner) {
        pthread_mutex_lock(&g_pending_agent_mu);
        if (g_pending_agent_count < MAX_AGENT_PENDING) {
            g_pending_agent[g_pending_agent_count++] = (PendingAgent){ owner, channel };
            queued = true;
        }
        pthread_mutex_unlock(&g_pending_agent_mu);
    }
    if (!queued) {
        /* Dozens of requests between two polls: the remote program gets a
         * closed agent and retries or fails, as with a busy agent. */
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
    }
}

/* Detach the first pending agent channel for `owner`; NULL when there is none. */
static LIBSSH2_CHANNEL *agent_pending_take(Session *owner) {
    LIBSSH2_CHANNEL *ch = NULL;
    pthread_mutex_lock(&g_pending_agent_mu);
    for (i32 i = 0; i < g_pending_agent_count; i++) {
        if (g_pending_agent[i].owner != owner) continue;
        ch = g_pending_agent[i].channel;
        g_pending_agent[i] = g_pending_agent[--g_pending_agent_count];
        break;
    }
    pthread_mutex_unlock(&g_pending_agent_mu);
    return ch;
}

static void agent_pending_cleanup(Session *owner) {
    if (!owner) return;
    for (;;) {
        LIBSSH2_CHANNEL *channel = agent_pending_take(owner);
        if (!channel) break;
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
    }
}
#endif /* LIBSSH2_CALLBACK_AUTHAGENT */

/* =========================================================================
 * Session struct
 * ========================================================================= */

/* Proxy hop runtime state for multi-hop connections */
typedef struct {
    int              sock;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *tunnel;   /* direct-tcpip channel to next hop */
} ProxyHopState;

/* Channel relay for proxying data between socketpair and SSH channel */
typedef struct {
    int              local_fd;   /* one end of socketpair */
    LIBSSH2_CHANNEL *channel;   /* direct-tcpip channel */
    LIBSSH2_SESSION *session;   /* owning session (for blocking mode) */
    int              session_sock; /* the socket `session` runs over */
    int              stop_rd, stop_wr; /* a byte here ends the thread's wait */
    atomic_bool      running;
    pthread_t        thread;
} ChannelRelay;

/* Remote forward listener state */
typedef struct {
    LIBSSH2_LISTENER *listener;
    char bind_host[256];
    i32  bind_port;
    char dest_host[256];
    i32  dest_port;
    bool active;
} RemoteForwardListener;

typedef SocketChannelRelay RemoteForwardConn;

typedef struct {
    int                listen_fd;
    int                pending_fd;   /* accepted; its channel is still opening */
    char               local_socket[512];
    char               remote_socket[512];
    SocketChannelRelay relay;
} StreamLocalRuntime;

#define MAX_REMOTE_LISTENERS  MAX_PORT_FORWARDS
#define MAX_REMOTE_FWD_CONNS  64

struct Session {
    u64           instance_id;
    SessionType   type;
    SessionStatus status;
    char          error_msg[256];
    i32           cols, rows;
    u16           cell_px_w, cell_px_h;   /* the pane's cell in pixels; 0 = not said */

    /* Local PTY */
    int           pty_master;
    pid_t         child_pid;
    f64           local_stop_deadline; /* zero until deferred close is committed */
    /* Latched once the child has replaced Liu's image with the shell's. Until
     * then the cwd probe below reports the directory the child INHERITED —
     * "/" for a Finder/Dock launch — and the sidebar would follow it to the
     * disk root. See ssh/local_cwd_ready.h. */
    bool          local_child_execd;
    /* Gather thread draining pty_master into a ring, or NULL. Non-NULL ONLY
     * for SESSION_LOCAL sessions built by the two local constructors — which
     * is also how mosh (its own pty, read via mosh_read) and SSH (libssh2 is
     * not thread-safe here, plus the session_io_is_suspended dance for
     * background transfers) are excluded: by never being started, not by a
     * type test at read time. */
    PtyReader    *reader;

    /* Exit-code capture for managed commands (Sites / dev-server manager).
     * session_is_alive() reaps the child with waitpid(WNOHANG) but otherwise
     * discards the status; it is stashed here so session_exited() can report
     * the real exit code without a second (racy) waitpid. */
    int           last_exit_status;
    bool          exited;
    /* When the local child was started and, once it is reaped, how long it
     * lived: what tells a shell the user left with `exit` from one that never
     * came up (ui/shell_exit.h). `exit_held` is the pane saying so and waiting
     * for Enter — it lives here so it travels with the session when a pane
     * moves, a tab is torn off or a split collapses. */
    f64           born_mono;
    f64           lived_sec;
    bool          exit_held;

    /* session_fg_process() TTL cache. That call runs on every typed character
     * (autosuggest) and on macOS scans every pid on the system, so cache the
     * result per session and re-scan only when the fg process group changes or
     * the short TTL lapses. */
    char          fg_cache[256];
    f64           fg_asked_at;     /* when a fresh foreground sample was last asked of the reader */
    f64           fg_cache_at;     /* monotonic seconds of last scan (0 = never) */
    pid_t         fg_cache_pgid;   /* fg pgid the cache was computed for */
    /* Current TTL. Doubles (up to SESSION_FG_TTL_MAX) each time a full scan of
     * an unchanged process group again finds no agent — the state of a pane
     * parked at a shell prompt, where the scan is pure cost. Any pgid change
     * resets it to SESSION_FG_TTL_BASE. */
    f64           fg_cache_ttl;

    /* Sleep/suspend (SIGSTOP/SIGCONT on child_pid) */
    bool          suspended;

    /* SSH */
    int             sock;
    LIBSSH2_SESSION *ssh_session;
    LIBSSH2_CHANNEL *ssh_channel;
    LIBSSH2_SFTP    *sftp_session;
    SSHConfig       *config;   /* NULL for local sessions */
    u8              *write_buf;
    u64              input_serial; /* UI-thread input writes, including mouse/paste */
    usize            write_len;
    usize            write_cap;
    /* Input queued BEHIND write_buf, in order: shared paste sources and the
     * bytes written while one was pending (see session_write_source). The
     * pull in session_flush_pending_write moves the head into write_buf a
     * chunk at a time, so the queue never holds more than a chunk or two of
     * a multi-megabyte paste. */
    struct SessionSrcRef *srcs;
    usize            src_count, src_cap;
    /* The SSH write in flight. libssh2_channel_write that answered EAGAIN
     * mid-packet must be called again with the SAME buffer address and
     * length (transport.c send_existing: "Address of buffer changed" is
     * LIBSSH2_ERROR_BAD_USE), so the bytes it was handed are COPIED out of
     * the queue into their own allocation and handed back unchanged until
     * libssh2 accepts them. The queue behind it is free to grow and move.
     * A local pty has no such contract and writes straight from the queue. */
    u8              *inflight_buf;
    usize            inflight_cap;
    usize            inflight_len;   /* bytes in the copy */
    usize            inflight_off;   /* bytes libssh2 has accepted so far */
    bool             inflight_active;
    /* The pty size the remote has NOT yet been told: a resize that arrives
     * while the io lock is busy or while libssh2 answers EAGAIN is kept here
     * and re-sent from the poll until libssh2 accepts it, or the remote shell
     * would keep drawing at the old geometry. The newest request wins
     * (coalesced). resize_inflight_* are the arguments of a request libssh2
     * has half-sent — its own state machine keeps the packet it built from
     * the FIRST call, so those, not the newest size, are what a rc==0 later
     * confirms. */
    i32              resize_want_cols, resize_want_rows;
    i32              resize_sent_cols, resize_sent_rows;
    i32              resize_inflight_cols, resize_inflight_rows;
    bool             resize_inflight;

    /* ControlMaster: session reuse. A session that borrows a connection holds
     * a COUNTED reference on the one that owns it (session_retain), released
     * when it lets go of the connection; so the owner's Session — and with it
     * the libssh2 session, the socket and the io lock — outlives every borrower.
     * The owner's pane can be closed while borrowers are using the connection:
     * session_destroy then ends only the pane (session_retire_pane) and the
     * rest waits for the last reference. */
    Session        *shared_session;   /* if non-NULL, this session's SSH conn is borrowed (retained) */
    bool            is_shared_owner;  /* true = owns ssh_session/sock, false = borrowed */
    bool            pane_retired;     /* the owner's pane is gone; only the connection is kept */
    /* On the owner: how many sessions borrow its connection. Not ref_count —
     * file transfers and the explorer's workers hold references too, and they
     * do not make the pane's close a "keep the connection" close. */
    atomic_uint     borrowers;
    /* On the owner: the server refused another channel on this connection
     * (sshd's MaxSessions); the pool offers it again once a borrower leaves. */
    atomic_bool     pool_full;
    /* On the owner: a channel on this connection asked for agent forwarding, so
     * the agent channels the server opens are relayed by whoever polls. */
    atomic_bool     agent_active;

    /* StreamLocal forwarding */
    StreamLocalRuntime *stream_fwds;     /* heap [4], main-thread polled */
    i32             stream_fwd_count;
    bool            stream_fwd_active;
    /* The config's LocalForward lines were started on this connection
     * (session_start_config_forwards_locked); a reconnect clears it. */
    bool            config_fwds_started;

    /* connect worker — runs TCP + libssh2 handshake + auth off the
     * main thread so the UI never freezes during connect.  After publishing
     * SESSION_CONNECTED the worker exits and libssh2 ownership passes to
     * the main thread, which does all subsequent non-blocking I/O. */
    pthread_t       connect_thread;
    bool            connect_thread_active;
    atomic_bool     connect_cancel;
    pthread_mutex_t status_lock;
    /* The sockets the connect worker may be parked on inside libssh2 RIGHT
     * NOW (each proxy hop's, then the destination's), under status_lock. The
     * cancel flag is only read between calls; what gets a worker out of a
     * blocking handshake is shutdown() on the fd it is reading, and `sock`
     * alone does not reach it — it is still -1 for the whole of a ProxyJump
     * chain. Emptied when the worker returns. */
    int             connect_fds[MAX_PROXY_HOPS + 1];
    i32             connect_fd_count;
    /* How long the next connect worker waits before it starts (a reconnect's
     * back-off, spent on the worker so the main loop needs no timer for it),
     * and whether the attempt got as far as talking SSH to the destination —
     * the difference between "the network is not there" and "the server said
     * no". Written by the worker, read by the main thread after it has ended. */
    i32             connect_delay_ms;
    atomic_bool     connect_past_network;
    char            mark_key[33];           /* this pane's key for its shell's OSC 133 marks (local sessions) */
    atomic_bool     connect_unattended;     /* this attempt is the policy's own retry, nobody asked for it */
    /* What this pane's connection has been through (ssh/link_policy.h). Main
     * thread only; it lives here so it moves with the session when panes are
     * closed, split or carried to another tab or window. */
    LinkState       link;
    char            link_reason[256];   /* why the link last went down, kept across the reconnect */
    char            auth_refused[256];  /* the server's refusal of the last credential offered (connect worker) */

    /* Workers keep cached SFTP handles alive via the suspend count. Each
     * libssh2 call owns io_lock; SFTP operation state additionally remains
     * owned across EAGAIN while shell reads are allowed through. */
    atomic_uint     ref_count;
    atomic_uint     io_suspend_count;
    pthread_mutex_t io_lock;       /* serializes every libssh2 call post-connect */
    pthread_mutex_t sftp_op_lock;  /* ticket admission, never held during I/O */
    pthread_cond_t  sftp_op_ready;
    u64             sftp_ticket_next, sftp_ticket_serving;
    atomic_uint     sftp_waiters;
    /* Set by the main thread when its trylock on io_lock failed: a transfer
     * worker holding the lock through a burst of ready data would otherwise
     * never give the shell a window (its trylock only wins if it lands in a
     * gap). sftp_io_breathe() honours it — S03. */
    atomic_bool     io_wanted;
    /* The main loop's read watch on this session's fd has been taken down
     * because the fd can only ever report the end from here on (see
     * session_retire_io_watch). MAIN THREAD ONLY; cleared when a reconnect
     * gives the session a new transport. */
    bool            io_watch_retired;
    /* Wakes a worker parked in sftp_io_yield when the MAIN thread has just been
     * inside libssh2: its channel read pulls every packet off the socket, the
     * worker's own SFTP packets included, and after that the socket is quiet
     * while the worker's data sits queued inside libssh2. Without this the
     * worker would find it only at its poll timeout, and a transfer would run
     * far below the link's speed. A byte in the pipe, written when io_yielding
     * is set. */
    int             io_kick[2];
    atomic_bool     io_yielding;

    /* Passphrase prompt: worker blocks on cond until UI supplies passphrase */
    bool            passphrase_pending;
    char            passphrase_prompt_path[512];
    pthread_cond_t  passphrase_cond;
    bool            passphrase_supplied;  /* UI set this before signal */
    bool            passphrase_cancelled; /* UI cancelled the prompt */

    /* Host key dialog — worker blocks on cond, UI wakes it after user decision */
    pthread_mutex_t hostkey_lock;
    pthread_cond_t  hostkey_cond;
    bool            hostkey_pending;   /* worker set: waiting for UI response */
    bool            hostkey_accepted;  /* UI set: user accepted the new key */
    bool            hostkey_is_change; /* true = key changed, false = new key */
    char            hostkey_old_fp[128];
    char            hostkey_new_fp[128];
    char            hostkey_host[256]; /* endpoint the pending dialog is about:
                                        * the destination or a ProxyJump hop */
    i32             hostkey_port;

    /* Keyboard-interactive (KBI / 2FA) dialog. Prompts/responses are heap-
     * allocated on first KBI challenge — local PTY sessions and SSH sessions
     * that never face 2FA never pay the 4 KB. */
    pthread_mutex_t kbi_lock;
    pthread_cond_t  kbi_cond;
    bool            kbi_ready;         /* UI set: responses are filled in */
    char            auth_peer[320];         /* "user@host" being authenticated to right now (a hop or the target) */
    char            kbi_peer[320];          /* ... as it stood when the prompt below was stored */
    bool            auth_peer_is_target;    /* false while a jump host is being authenticated to */
    char            kbi_name[256];
    char            kbi_instruction[512];
    i32             kbi_num_prompts;
    char          (*kbi_prompts)[256];     /* heap [KBI_MAX_PROMPTS][256] */
    bool            kbi_echo[KBI_MAX_PROMPTS];
    char          (*kbi_responses)[256];   /* heap [KBI_MAX_PROMPTS][256] */

    /* ProxyJump multi-hop state */
    ProxyHopState   proxy_hops[MAX_PROXY_HOPS];
    i32             proxy_hop_count;
    ChannelRelay   *proxy_relays[MAX_PROXY_HOPS];

    /* Keepalive */
    f64             last_keepalive_time;
    i32             keepalive_missed;
    i32             keepalive_interval;
    i32             keepalive_count_max;
    /* Silence detection: a keepalive that was SENT proves nothing about the
     * peer. The socket's own receive counter does — with want_reply on, a
     * live server answers every probe, and any byte at all resets the count.
     * rx_at_probe is the counter when the last probe went out. */
    bool            keepalive_probe_armed;
    u64             keepalive_rx_at_probe;

    /* Remote port forwarding. Both arrays are lazy — typical sessions use
     * zero remote forwards, and even forwarders use only a handful. The
     * worst-case slot pool is allocated on first use rather than baking
     * 8.5 KB + 2 KB into every Session. */
    RemoteForwardListener *remote_listeners;   /* heap [MAX_REMOTE_LISTENERS] when non-NULL */
    i32                    remote_listener_count;
    /* Slot table of POINTERS, not inline structs: each RemoteForwardConn
     * carries two 16 KB relay buffers, so an inline [64] pool costs 2 MB for
     * the whole session lifetime even when a single connection is live. The
     * table itself is 64 pointers; a relay is malloc'd on accept and freed on
     * close, so the footprint tracks the connections that actually exist. */
    RemoteForwardConn    **remote_fwd_conns;   /* heap [MAX_REMOTE_FWD_CONNS] slots when non-NULL */
    i32                    remote_fwd_conn_count;

    /* X11 forwarding (lazy — local sessions and SSH targets without X11
     * never alloc the relay slots). */
    X11Relay       *x11_relays;            /* heap [MAX_X11_CHANNELS] when non-NULL */
    i32             x11_relay_count;
    u8              x11_cookie[X11_COOKIE_LEN];   /* the FAKE cookie sshd was given; scrubbed at dispose */
    bool            x11_cookie_set;

    /* Agent forwarding (ForwardAgent yes): one relay per channel the server
     * opens to the agent, lazy like the X11 slots. */
    SocketChannelRelay *agent_relays;      /* heap [MAX_AGENT_CHANNELS] when non-NULL */
    i32             agent_relay_count;

    /* Mosh (SESSION_MOSH only) */
    MoshSession    *mosh;

    /* Telnet / Serial (their SESSION_* types only). Single-threaded like
     * LOCAL: no worker, so no status_lock involvement. */
    TelnetSession  *telnet;
    SerialSession  *serial;

    /* SFTP auto-init: populated by the connect worker before publishing
     * SESSION_CONNECTED. Readers on the main thread see these values
     * after an acquire-ordered read of s->status, so no extra lock. The
     * remote home path is heap-allocated when sftp_realpath returns; local
     * sessions and SSH targets without SFTP support stay at NULL. */
    char           *initial_cwd;
    bool            sftp_init_tried;
};

/* =========================================================================
 * Session pool for ControlMaster (connection multiplexing)
 *
 * When control_master is enabled, an existing LIBSSH2_SESSION to the same
 * host:port+user is reused rather than opening a new TCP connection.  The
 * pool is a simple array scanned linearly — the number of distinct SSH
 * destinations is tiny in practice.
 * ========================================================================= */

#define SESSION_POOL_MAX 64

static struct {
    Session *sessions[SESSION_POOL_MAX];
    i32      count;
    pthread_mutex_t lock;
    bool     initialized;
} g_session_pool = {0};

static pthread_once_t g_session_pool_once = PTHREAD_ONCE_INIT;

static void session_pool_init_impl(void) {
    g_session_pool.initialized =
        pthread_mutex_init(&g_session_pool.lock, NULL) == 0;
}

static bool session_pool_init_once(void) {
    return pthread_once(&g_session_pool_once, session_pool_init_impl) == 0 &&
           g_session_pool.initialized;
}

/* Does `c` ask for a connection another session may share, and can one be? A
 * ProxyJump chain is its own set of libssh2 sessions and relay threads owned by
 * the session that built it; sharing it would share all of that, so those hosts
 * keep a connection of their own. */
static bool session_config_shares(const SSHConfig *c) {
    return c && c->control_master && !c->sftp_only && c->proxy_chain_len == 0 && !c->jump_host[0] &&
           c->hostname[0] && c->username[0];
}

/* A connected, pooled session on the same user@host:port as `c`, RETAINED for
 * the caller (who owes it a session_release), or NULL. The reference is taken
 * under the pool lock: a session leaves the pool before its last reference is
 * dropped (session_destroy_final unregisters first), so a pointer found here is
 * still a session, and session_retain refuses one whose count already reached
 * zero. */
static Session *session_pool_take(const SSHConfig *c) {
    if (!session_config_shares(c) || !session_pool_init_once()) return NULL;
    const i32 port = c->port > 0 ? c->port : 22;
    Session *found = NULL;
    pthread_mutex_lock(&g_session_pool.lock);
    for (i32 i = 0; i < g_session_pool.count && !found; i++) {
        Session *s = g_session_pool.sessions[i];
        if (!s || !s->config || !s->is_shared_owner || s->pane_retired) continue;
        if (atomic_load_explicit(&s->pool_full, memory_order_acquire)) continue;
        if (session_status(s) != SESSION_CONNECTED) continue;
        if ((s->config->port > 0 ? s->config->port : 22) != port) continue;
        if (strcasecmp(s->config->hostname, c->hostname) != 0) continue;
        if (strcmp(s->config->username, c->username) != 0) continue;
        if (session_retain(s)) {
            /* Counted here, under the same lock session_pool_unregister takes:
             * once an owner is out of the pool, every borrower it will ever
             * have is already counted, which is what its pane close asks. */
            atomic_fetch_add_explicit(&s->borrowers, 1u, memory_order_acq_rel);
            found = s;
        }
    }
    pthread_mutex_unlock(&g_session_pool.lock);
    return found;
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

/* The connect worker changes what the main loop has to do — a session became
 * CONNECTED (its socket needs a read watch and a first drain), failed, or is
 * waiting on a prompt — and the loop only looks at sessions when something
 * announces work. Nothing else announces these: the watch is installed from
 * inside a poll, a poll needs an fd fire or an input event, and a connection
 * that finishes after the user's last key press has neither, so its output
 * would sit unread until the next key.
 *
 * platform_wake_event_loop() counts as an fd fire (that is how PTY reader
 * threads announce a ring), so one call opens the poll gate. */
static void session_notify_main(void) {
    platform_wake_event_loop();
}

/* ---- getting the connect worker out of a blocking call ---------------------
 *
 * How long ONE blocking libssh2 call may wait for the peer during the parts of
 * a connect where nobody is being asked anything: the banner / key exchange,
 * the auth-method list, a proxy hop's tunnel, the shell channel. Without a
 * limit, a host that completes the TCP handshake and then says nothing (a
 * half-dead middlebox, a port answering as something else) would leave the
 * tab on "Connecting" until it is closed. NOT applied while a credential is
 * being offered: libssh2 measures from the call's entry, and a
 * keyboard-interactive answer or an agent's touch-to-sign is typed/pressed
 * INSIDE that call — a limit there would fail every slow human
 * (ssh_wait_unbounded below). */
#define SSH_BLOCKING_CALL_TIMEOUT_MS 30000

/* LIU_SSH_CALL_TIMEOUT_MS overrides the default blocking-call timeout from 1
 * second to 10 minutes. Read it on each connect worker invocation so
 * different threads do not share a cached value. Credential prompts use
 * ssh_wait_unbounded and are not subject to this limit. */
static long ssh_call_timeout_ms(void) {
    const char *e = getenv("LIU_SSH_CALL_TIMEOUT_MS");
    if (e && e[0]) {
        char *end = NULL;
        long v = strtol(e, &end, 10);
        if (end && *end == '\0' && v >= 1000 && v <= 600000) return v;
    }
    return SSH_BLOCKING_CALL_TIMEOUT_MS;
}

static void ssh_wait_bounded(LIBSSH2_SESSION *ssh) {
    if (ssh) libssh2_session_set_timeout(ssh, ssh_call_timeout_ms());
}
static void ssh_wait_unbounded(LIBSSH2_SESSION *ssh) {
    if (ssh) libssh2_session_set_timeout(ssh, 0);
}

static void connect_fd_publish(Session *s, int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&s->status_lock);
    if (s->connect_fd_count < (i32)(sizeof s->connect_fds / sizeof s->connect_fds[0]))
        s->connect_fds[s->connect_fd_count++] = fd;
    pthread_mutex_unlock(&s->status_lock);
}

/* Before the worker itself closes one, so a canceller can never shut down a
 * number that has since been handed to somebody else. */
static void connect_fd_forget(Session *s, int fd) {
    pthread_mutex_lock(&s->status_lock);
    for (i32 i = 0; i < s->connect_fd_count; i++) {
        if (s->connect_fds[i] != fd) continue;
        s->connect_fds[i] = s->connect_fds[--s->connect_fd_count];
        break;
    }
    pthread_mutex_unlock(&s->status_lock);
}

static void connect_fds_clear(Session *s) {
    pthread_mutex_lock(&s->status_lock);
    s->connect_fd_count = 0;
    pthread_mutex_unlock(&s->status_lock);
}

/* Cancel the connect worker and wait for it. Bounded: the TCP stage watches the
 * cancel flag (net_tcp_connect_ex), a prompt the worker is waiting on is
 * answered "no", and a blocking libssh2 call is ended by shutting down the
 * socket it reads — every one it may be on, not just the destination's. The
 * callers — window close and quit (session_destroy_final) and a reconnect, on
 * the main thread — must not wait however long the network takes. */
static void ssh_connect_worker_stop(Session *s) {
    if (!s->connect_thread_active) return;
    atomic_store_explicit(&s->connect_cancel, true, memory_order_release);
    /* If worker is blocked on hostkey dialog, wake it with rejection */
    pthread_mutex_lock(&s->hostkey_lock);
    if (s->hostkey_pending) {
        s->hostkey_accepted = false;
        s->hostkey_pending = false;
        pthread_cond_signal(&s->hostkey_cond);
    }
    pthread_mutex_unlock(&s->hostkey_lock);
    /* If worker is blocked on KBI dialog, wake it */
    pthread_mutex_lock(&s->kbi_lock);
    s->kbi_ready = true;
    pthread_cond_signal(&s->kbi_cond);
    pthread_mutex_unlock(&s->kbi_lock);
    /* If worker is blocked on passphrase dialog, wake it; and unblock libssh2
     * reads on whatever it is connected to so far. */
    pthread_mutex_lock(&s->status_lock);
    if (s->passphrase_pending) {
        s->passphrase_cancelled = true;
        pthread_cond_signal(&s->passphrase_cond);
    }
    for (i32 i = 0; i < s->connect_fd_count; i++)
        shutdown(s->connect_fds[i], SHUT_RDWR);
    pthread_mutex_unlock(&s->status_lock);
    pthread_join(s->connect_thread, NULL);
    s->connect_thread_active = false;
}

static void set_error(Session *s, const char *msg) {
    if (!s) return;
    if (!msg) msg = "unknown session error";
    /* SSH/Mosh sessions may have a worker thread writing status while
     * main reads it. Serialize. Local sessions don't init the lock. */
    if (s->type == SESSION_SSH || s->type == SESSION_MOSH) {
        pthread_mutex_lock(&s->status_lock);
        bool was_live = s->status == SESSION_CONNECTED;
        s->status = SESSION_ERROR;
        snprintf(s->error_msg, sizeof(s->error_msg), "%s", msg);
        pthread_mutex_unlock(&s->status_lock);
        /* A link that just went down has something to say and a reconnect to
         * start, and the loop only looks at sessions when woken — a dead peer
         * sends nothing that would wake it. Once per fall, not per call. */
        if (was_live) session_notify_main();
    } else {
        s->status = SESSION_ERROR;
        snprintf(s->error_msg, sizeof(s->error_msg), "%s", msg);
    }
}

/* tcp_connect and set_nonblocking are in core/net.c */

/* A server's keyboard-interactive text (its name, instruction or prompt) as
 * one clean line: control bytes and line breaks become a space, the code
 * points that only deceive are dropped (core/text_sanitize.h). It is drawn
 * in a dialog of Liu's, and a server chooses every byte of it. */
static void kbi_text_copy(char *dst, usize cap, const char *src, int len) {
    dst[0] = '\0';
    if (!src || len <= 0 || cap == 0) return;
    char raw[512];
    usize n = (usize)len < sizeof raw - 1 ? (usize)len : sizeof raw - 1;
    memcpy(raw, src, n);
    raw[n] = '\0';
    liu_clean_dictation(raw, dst, (i32)cap);
}

/* Who is being authenticated to from here on — a jump host or the
 * destination. The prompt dialog shows it. */
static void ssh_auth_peer_set(Session *s, const char *user, const char *host, i32 port,
                              bool is_target) {
    pthread_mutex_lock(&s->kbi_lock);
    s->auth_peer_is_target = is_target;
    if (port > 0 && port != 22)
        snprintf(s->auth_peer, sizeof s->auth_peer, "%s@%s:%d", user ? user : "", host ? host : "", port);
    else
        snprintf(s->auth_peer, sizeof s->auth_peer, "%s@%s", user ? user : "", host ? host : "");
    pthread_mutex_unlock(&s->kbi_lock);
}

static LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(ssh_kbdint_callback) {
    Session *s = NULL;
    if (abstract && *abstract) {
        s = (Session *)*abstract;
    }
    if (!s) return;
    if (num_prompts <= 0 || !prompts || !responses) return;

    const SSHConfig *config = s->config;

    /* Fast path: single password-like prompt with a password in config —
     * auto-fill without blocking on UI. Only for the DESTINATION: the stored
     * password is that host's, and is never handed to a jump host that asks
     * "Password:". */
    if (num_prompts == 1 && config && config->password[0] && s->auth_peer_is_target) {
        /* Check if the prompt looks like a password prompt */
        bool looks_like_password = false;
        if (prompts[0].length == 0) {
            looks_like_password = true;
        } else if (prompts[0].text) {
            const char *p = (const char *)prompts[0].text;
            usize plen = prompts[0].length;
            /* Case-insensitive check for "password" in prompt text */
            for (usize j = 0; j + 8 <= plen; j++) {
                if (strncasecmp(p + j, "password", 8) == 0) {
                    looks_like_password = true;
                    break;
                }
            }
        }
        if (looks_like_password) {
            const char *reply = config->password;
            usize reply_len = strlen(reply);
            responses[0].text = malloc(reply_len + 1);
            if (responses[0].text) {
                memcpy(responses[0].text, reply, reply_len + 1);
                responses[0].length = (unsigned int)reply_len;
            } else {
                responses[0].length = 0;
            }
            return;
        }
    }

    /* Multi-prompt / OTP / 2FA path: store prompts in Session, set
     * SESSION_KBI_PENDING, and block until the UI fills in responses. */
    pthread_mutex_lock(&s->kbi_lock);

    /* Name, instruction and prompts are the SERVER's words, drawn in a Liu
     * dialog: one clean line each (kbi_text_copy), and the dialog says whose
     * they are. */
    kbi_text_copy(s->kbi_name, sizeof s->kbi_name, name, name_len);
    kbi_text_copy(s->kbi_instruction, sizeof s->kbi_instruction, instruction, instruction_len);
    snprintf(s->kbi_peer, sizeof s->kbi_peer, "%s", s->auth_peer);

    i32 count = (num_prompts > KBI_MAX_PROMPTS) ? KBI_MAX_PROMPTS : num_prompts;
    s->kbi_num_prompts = count;

    /* Lazy-alloc prompt + response pools the first time this session faces
     * a KBI challenge. Local sessions and pubkey-only SSH targets never
     * trigger this path. */
    if (!s->kbi_prompts) {
        s->kbi_prompts = calloc(KBI_MAX_PROMPTS, sizeof(*s->kbi_prompts));
    }
    if (!s->kbi_responses) {
        s->kbi_responses = calloc(KBI_MAX_PROMPTS, sizeof(*s->kbi_responses));
    }
    if (!s->kbi_prompts || !s->kbi_responses) {
        /* Out of memory: skip KBI, treat as auth failure downstream. The
         * caller will see kbi_num_prompts=0 plus empty responses[]. */
        s->kbi_num_prompts = 0;
        pthread_mutex_unlock(&s->kbi_lock);
        for (int i = 0; i < num_prompts; i++) {
            responses[i].text = NULL;
            responses[i].length = 0;
        }
        return;
    }

    for (i32 i = 0; i < count; i++) {
        kbi_text_copy(s->kbi_prompts[i], sizeof(s->kbi_prompts[i]),
                      (const char *)prompts[i].text, (int)prompts[i].length);
        s->kbi_echo[i] = prompts[i].echo != 0;
        s->kbi_responses[i][0] = '\0';
    }

    s->kbi_ready = false;

    /* Publish SESSION_KBI_PENDING so UI knows to show the dialog */
    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_KBI_PENDING;
    pthread_mutex_unlock(&s->status_lock);
    session_notify_main();

    /* Block until UI thread calls session_kbi_submit() */
    while (!s->kbi_ready) {
        pthread_cond_wait(&s->kbi_cond, &s->kbi_lock);
    }

    /* Restore status to authenticating */
    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_AUTHENTICATING;
    pthread_mutex_unlock(&s->status_lock);

    /* Fill libssh2 responses from what UI provided */
    for (int i = 0; i < num_prompts; i++) {
        const char *reply = "";
        if (i < count) reply = s->kbi_responses[i];
        usize reply_len = strlen(reply);
        responses[i].text = malloc(reply_len + 1);
        if (responses[i].text) {
            memcpy(responses[i].text, reply, reply_len + 1);
            responses[i].length = (unsigned int)reply_len;
        } else {
            responses[i].length = 0;
        }
    }

    pthread_mutex_unlock(&s->kbi_lock);
}

/* The key was taken but the server wants a second step (ssh/auth_second_factor.h):
 * ask what it still accepts and give it that — its own prompt through the
 * keyboard-interactive dialog, or the stored password. Returns libssh2's
 * answer, or the failure it was called with when nothing is left to try. */
static int ssh_auth_second_step(Session *s, const SSHConfig *config, int auth_rc) {
    s->auth_refused[0] = '\0';
    if (auth_rc == 0 || libssh2_userauth_authenticated(s->ssh_session)) return 0;
    /* Asking what the server still accepts is a call of its own, and it
     * replaces libssh2's last error: the refusal is kept to be told. */
    char *refusal = NULL;
    libssh2_session_last_error(s->ssh_session, &refusal, NULL, 0);
    snprintf(s->auth_refused, sizeof s->auth_refused, "%s", refusal ? refusal : "");
    char *rest = libssh2_userauth_list(s->ssh_session, config->username,
                                       (unsigned int)strlen(config->username));
    switch (ssh_auth_second_factor(rest, config->password[0] != '\0')) {
    case SSH_SECOND_FACTOR_KBI:
        ssh_wait_unbounded(s->ssh_session);    /* a person is answering */
        return libssh2_userauth_keyboard_interactive(s->ssh_session, config->username,
                                                     &ssh_kbdint_callback);
    case SSH_SECOND_FACTOR_PASSWORD:
        return libssh2_userauth_password(s->ssh_session, config->username, config->password);
    case SSH_SECOND_FACTOR_NONE:
        break;
    }
    return auth_rc;
}

__attribute__((unused)) static bool ssh_prompt_trust_hostkey(const char *hostname, i32 port, const char *fingerprint) {
    if (!isatty(STDIN_FILENO)) {
        return false;
    }

    fprintf(stderr,
            "\nTrust new SSH host key for %s:%d?\nFingerprint: %s\nType 'yes' to trust: ",
            hostname, port, fingerprint ? fingerprint : "(unknown)");
    fflush(stderr);

    char answer[32] = {0};
    if (!fgets(answer, sizeof(answer), stdin)) {
        return false;
    }
    return strcmp(answer, "yes\n") == 0 || strcmp(answer, "yes") == 0;
}

/* Verify the host key `ssh` negotiated against known_hosts, asking the user
 * through the hostkey dialog for a new or changed key. `hostname`/`port` name
 * the endpoint THIS libssh2 session talks to — the final destination, or a
 * ProxyJump hop. Every hop is verified too: a hop handshaken and
 * AUTHENTICATED without verification would hand a MITM on the path to the
 * jump host the credentials offered to it, and a transparent relay of the
 * (separately verified) final session. */
static int ssh_verify_hostkey_for(Session *s, LIBSSH2_SESSION *ssh,
                                  const char *hostname, i32 port) {
    HostKeyStatus hk = known_hosts_verify(ssh, hostname, port);
    if (hk == HOST_KEY_OK) return 0;
    if (hk == HOST_KEY_REVOKED) {
        set_error(s, "SSH host key is REVOKED - connection refused");
        return -1;
    }
    if (hk == HOST_KEY_ERROR) {
        set_error(s, known_hosts_store_unsafe()
            ? "SSH host key not checked: ~/.ssh/known_hosts can be changed by other users "
              "(run chmod go-w ~/.ssh ~/.ssh/known_hosts)"
            : "SSH host key verification failed");
        return -1;
    }

    char fp[128] = {0};
    known_hosts_fingerprint_sha256(ssh, fp, sizeof(fp));

    if (hk == HOST_KEY_CHANGED &&
        !link_hostkey_may_prompt(true, atomic_load_explicit(&s->connect_unattended,
                                                            memory_order_acquire))) {
        /* Not asked on a reconnect Liu started by itself (ssh/link_policy.h):
         * the attempt ends here, the pane says so, and Enter connects again —
         * with the dialog, in front of somebody who asked for it. */
        set_error(s, "SSH host key has CHANGED - not reconnecting by itself");
        /* A key was compared, so the network did its part — at the
         * destination or at a jump hop on the way. Marking it at a HOP too
         * keeps a changed key met there from being taken for a dropped link
         * and retried before the pane says what is wrong. */
        atomic_store_explicit(&s->connect_past_network, true, memory_order_release);
        return -1;
    }
    if (hk == HOST_KEY_CHANGED) {
        /* Host key has changed — present dialog to user.
         * The worker thread blocks on hostkey_cond until the UI responds. */
        pthread_mutex_lock(&s->hostkey_lock);
        s->hostkey_is_change = true;
        snprintf(s->hostkey_new_fp, sizeof(s->hostkey_new_fp), "%s", fp);
        s->hostkey_old_fp[0] = '\0'; /* the old fp is not readily available here */
        snprintf(s->hostkey_host, sizeof s->hostkey_host, "%s", hostname);
        s->hostkey_port = port;
        s->hostkey_pending = true;
        s->hostkey_accepted = false;
        session_notify_main();     /* the dialog is raised by the loop's scan */

        /* Wait for UI thread to signal a decision */
        while (s->hostkey_pending) {
            pthread_cond_wait(&s->hostkey_cond, &s->hostkey_lock);
        }
        bool accepted = s->hostkey_accepted;
        pthread_mutex_unlock(&s->hostkey_lock);

        if (!accepted) {
            set_error(s, "SSH host key mismatch - rejected by user");
            return -1;
        }
        /* User accepted: retire EVERY key the file holds for this host, then
         * add the new one. Removing only a key that MATCHES would remove
         * nothing on a mismatch, and the superseded key would stay a valid
         * trust anchor. Fails closed: a trust store that cannot be updated is
         * not one to keep connecting on. */
        if (!known_hosts_forget(ssh, hostname, port)) {
            set_error(s, "Failed to retire the old SSH host key");
            return -1;
        }
        if (!known_hosts_add(ssh, hostname, port)) {
            set_error(s, "Failed to persist new SSH host key");
            return -1;
        }
        return 0;
    }

    /* HOST_KEY_NEW — ask user to trust via GUI dialog */
    pthread_mutex_lock(&s->hostkey_lock);
    s->hostkey_is_change = false;
    snprintf(s->hostkey_new_fp, sizeof(s->hostkey_new_fp), "%s", fp);
    s->hostkey_old_fp[0] = '\0';
    snprintf(s->hostkey_host, sizeof s->hostkey_host, "%s", hostname);
    s->hostkey_port = port;
    s->hostkey_pending = true;
    s->hostkey_accepted = false;
    session_notify_main();

    while (s->hostkey_pending) {
        pthread_cond_wait(&s->hostkey_cond, &s->hostkey_lock);
    }
    bool accepted = s->hostkey_accepted;
    pthread_mutex_unlock(&s->hostkey_lock);

    if (!accepted) {
        set_error(s, "Untrusted new SSH host key");
        return -1;
    }
    if (!known_hosts_add(ssh, hostname, port)) {
        set_error(s, "Failed to persist SSH host key");
        return -1;
    }
    return 0;
}

static int ssh_verify_hostkey(Session *s) {
    return ssh_verify_hostkey_for(s, s->ssh_session, s->config->hostname,
                                  s->config->port);
}

/* The config's "ssh_legacy_algorithms": process-wide, like the pane locale. */
static atomic_bool g_ssh_legacy_algs;

void session_ssh_set_legacy_algorithms(bool allow) {
    atomic_store_explicit(&g_ssh_legacy_algs, allow, memory_order_release);
}

/* What Liu offers this server, set before the handshake (ssh/ssh_algs.h): an
 * allow-list per method, and the host-key types the store already holds for
 * this host first — so a host pinned with one type is not negotiated on
 * another and reported as changed. A list libssh2 will not take (it strips
 * the names it does not implement; none left is an error) leaves the
 * library's own default for that method. */
static void ssh_offer_algorithms(LIBSSH2_SESSION *ssh, const char *host, i32 port) {
    if (!ssh || !host) return;
    bool legacy = atomic_load_explicit(&g_ssh_legacy_algs, memory_order_acquire);
    char list[512];
    bool stored[SSH_HOSTKEY_KINDS];
    known_hosts_stored_kinds(ssh, host, port, stored);
    if (ssh_algs_hostkey_list(stored, legacy, list, sizeof list))
        (void)libssh2_session_method_pref(ssh, LIBSSH2_METHOD_HOSTKEY, list);
    if (ssh_algs_list(SSH_ALGS_KEX, SSH_ALGS_KEX_LEGACY, legacy, list, sizeof list))
        (void)libssh2_session_method_pref(ssh, LIBSSH2_METHOD_KEX, list);
    if (ssh_algs_list(SSH_ALGS_CIPHER, SSH_ALGS_CIPHER_LEGACY, legacy, list, sizeof list)) {
        (void)libssh2_session_method_pref(ssh, LIBSSH2_METHOD_CRYPT_CS, list);
        (void)libssh2_session_method_pref(ssh, LIBSSH2_METHOD_CRYPT_SC, list);
    }
    if (ssh_algs_list(SSH_ALGS_MAC, SSH_ALGS_MAC_LEGACY, legacy, list, sizeof list)) {
        (void)libssh2_session_method_pref(ssh, LIBSSH2_METHOD_MAC_CS, list);
        (void)libssh2_session_method_pref(ssh, LIBSSH2_METHOD_MAC_SC, list);
    }
}

/* A handshake that failed: libssh2's own words, and — when no algorithm
 * could be agreed and the legacy ones are off — where the switch is. Without
 * the hint an old switch or NAS simply stops connecting and says nothing a
 * person can act on. */
static void ssh_handshake_failed(Session *s, LIBSSH2_SESSION *ssh) {
    char *errmsg = NULL;
    int code = libssh2_session_last_error(ssh, &errmsg, NULL, 0);
    bool agree = code == LIBSSH2_ERROR_KEX_FAILURE || code == LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE ||
                 code == LIBSSH2_ERROR_METHOD_NONE;
    if (agree && !atomic_load_explicit(&g_ssh_legacy_algs, memory_order_acquire)) {
        char msg[512];
        snprintf(msg, sizeof msg, "%s (if this server only speaks old algorithms, "
                 "\"ssh_legacy_algorithms\": true in config.json allows them)",
                 errmsg && errmsg[0] ? errmsg : "No common SSH algorithm");
        set_error(s, msg);
        return;
    }
    set_error(s, errmsg);
}

/* =========================================================================
 * Local PTY session
 * ========================================================================= */

#ifndef PLATFORM_WIN32
extern char **environ;

typedef struct {
    char **items;
    usize count;
    usize cap;
} LocalEnvVec;

static void local_env_free(LocalEnvVec *v) {
    if (!v) return;
    for (usize i = 0; i < v->count; i++) free(v->items[i]);
    free(v->items);
    memset(v, 0, sizeof *v);
}

static bool local_env_name_valid(const char *key) {
    if (!key || !key[0] ||
        !((key[0] >= 'A' && key[0] <= 'Z') ||
          (key[0] >= 'a' && key[0] <= 'z') || key[0] == '_')) return false;
    for (usize i = 1; key[i]; i++) {
        char c = key[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    /* Names that make the dynamic loader or the shell run code before the
     * program does are not "environment", they are a payload: a vault row
     * called DYLD_INSERT_LIBRARIES or LD_PRELOAD executes its value in the
     * shell and every descendant. The vault is the user's own, so this is a
     * belt for an imported or shared store, not a wall. */
    if (strncmp(key, "DYLD_", 5) == 0 || strncmp(key, "LD_", 3) == 0 ||
        strcmp(key, "BASH_ENV") == 0 || strcmp(key, "ENV") == 0)
        return false;
    return true;
}

static bool local_env_matches(const char *entry, const char *key) {
    usize n = strlen(key);
    return entry && strncmp(entry, key, n) == 0 && entry[n] == '=';
}

static bool local_env_set(LocalEnvVec *v, const char *key, const char *value) {
    if (!v || !local_env_name_valid(key) || !value) return false;
    usize kn = strlen(key), vn = strlen(value), len, alloc;
    if (!checked_add_usize(kn, vn, &len) ||
        !checked_add_usize(len, 2u, &alloc)) return false;
    char *entry = malloc(alloc);
    if (!entry) return false;
    memcpy(entry, key, kn);
    entry[kn] = '=';
    memcpy(entry + kn + 1u, value, vn + 1u);
    for (usize i = 0; i < v->count; i++) {
        if (local_env_matches(v->items[i], key)) {
            free(v->items[i]);
            v->items[i] = entry;
            return true;
        }
    }
    if (v->count + 1u >= v->cap) { free(entry); return false; }
    v->items[v->count++] = entry;
    v->items[v->count] = NULL;
    return true;
}

/* Directory of HELPER binaries to expose on a child shell's PATH.
 *
 * Agent hooks need to find liu-notify, but the bundle's own MacOS/ directory
 * also holds the app's binary — and macOS filesystems are case-insensitive, so
 * with it on PATH `liu` would resolve to `Liu`, and typing it (or any
 * script/completion reaching for it) would launch a SECOND copy of the app.
 *
 * So a directory that contains only the helpers is exposed instead. The links
 * are refreshed on every call — cheap, and it means a moved, reinstalled or
 * updated bundle can never leave a dangling link behind. Returns NULL if the
 * directory can't be prepared, in which case PATH is simply left alone. */
static const char *liu_helper_bin_dir(void) {
    static char dir[1024];
    static bool tried = false, ok = false;
    if (tried) return ok ? dir : NULL;
    tried = true;

    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        struct passwd pw, *res = NULL;
        char scratch[4096];
        if (getpwuid_r(getuid(), &pw, scratch, sizeof scratch, &res) != 0 ||
            !res || !res->pw_dir) return NULL;
        home = res->pw_dir;
    }

    char base[1024];
    if ((usize)snprintf(base, sizeof base, "%s/.liu", home) >= sizeof base) return NULL;
    if (mkdir(base, 0755) != 0 && errno != EEXIST) return NULL;
    if ((usize)snprintf(dir, sizeof dir, "%s/bin", base) >= sizeof dir) return NULL;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return NULL;
    /* This folder goes FIRST in every pane's PATH. mkdir says nothing about
     * one that was already there: it is used only when it and ~/.liu are
     * directories owned by this user, reached through no link, that nobody
     * else can write. Otherwise the panes do without the helpers' short
     * names. */
    {
        struct stat st;
        const char *both[2] = { base, dir };
        for (int i = 0; i < 2; i++)
            if (lstat(both[i], &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
                (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) return NULL;
    }

    const char *exe_dir = liu_executable_dir();
    if (!exe_dir || !exe_dir[0]) return NULL;

    /* Every helper Liu ships. Missing ones are skipped rather than failing the
     * whole directory — a dev build has liu-history, a packaged .app may not. */
    /* `liu-agent` is liu-notify under another name: the binary picks its
     * commands by the name it was started as. */
    static const struct { const char *name, *binary; } helpers[] = {
        { "liu-notify",  "liu-notify"  },
        { "liu-history", "liu-history" },
        { "liu-agent",   "liu-notify"  },
    };
    for (usize i = 0; i < sizeof helpers / sizeof helpers[0]; i++) {
        char target[1200], link[1200];
        if ((usize)snprintf(target, sizeof target, "%s/%s", exe_dir, helpers[i].binary) >= sizeof target)
            continue;
        if (access(target, X_OK) != 0) continue;
        if ((usize)snprintf(link, sizeof link, "%s/%s", dir, helpers[i].name) >= sizeof link)
            continue;
        unlink(link);              /* replace whatever a previous run left */
        (void)symlink(target, link);
    }
    ok = true;
    return dir;
}

const char *session_helper_bin_dir(void) {
    return liu_helper_bin_dir();
}

static char g_local_shell_integration[1024];      /* "" = none */
static char g_local_agent_sock[256];               /* "" = the agent door is shut */

void session_local_set_agent_sock(const char *path) {
    snprintf(g_local_agent_sock, sizeof g_local_agent_sock, "%s", path ? path : "");
}

/* Which pane a local shell process is: the shell forkpty() started for a
 * session, and that session's serial. The agent-control server asks it from
 * its own thread to tell which pane a caller runs in — it walks the caller's
 * parents up to the process whose parent is Liu, which is a pane's shell — so
 * the answer comes from the process tree, not from anything the caller says. */
#define LOCAL_PANE_TABLE_CAP 1024
static struct { pid_t pid; u64 serial; } g_local_panes[LOCAL_PANE_TABLE_CAP];
static i32 g_local_pane_count = 0;
static pthread_mutex_t g_local_panes_mu = PTHREAD_MUTEX_INITIALIZER;

static void local_pane_note(pid_t pid, u64 serial) {
    if (pid <= 0 || serial == 0) return;
    pthread_mutex_lock(&g_local_panes_mu);
    if (g_local_pane_count < LOCAL_PANE_TABLE_CAP) {
        g_local_panes[g_local_pane_count].pid = pid;
        g_local_panes[g_local_pane_count].serial = serial;
        g_local_pane_count++;
    }
    pthread_mutex_unlock(&g_local_panes_mu);
}

static void (*g_local_pane_gone)(u64 serial);

void session_local_set_pane_gone_hook(void (*fn)(u64 serial)) {
    g_local_pane_gone = fn;
}

static void local_pane_forget(u64 serial) {
    if (serial == 0) return;
    bool was = false;
    pthread_mutex_lock(&g_local_panes_mu);
    for (i32 i = 0; i < g_local_pane_count; i++) {
        if (g_local_panes[i].serial != serial) continue;
        g_local_panes[i] = g_local_panes[--g_local_pane_count];
        was = true;
        break;
    }
    pthread_mutex_unlock(&g_local_panes_mu);
    if (was && g_local_pane_gone) g_local_pane_gone(serial);
}

bool session_local_pane_of_shell(i32 pid, u64 *serial) {
    bool found = false;
    pthread_mutex_lock(&g_local_panes_mu);
    for (i32 i = 0; i < g_local_pane_count; i++) {
        if (g_local_panes[i].pid != (pid_t)pid) continue;
        if (serial) *serial = g_local_panes[i].serial;
        found = true;
        break;
    }
    pthread_mutex_unlock(&g_local_panes_mu);
    return found;
}
static char g_local_term[48];                      /* "" = the default */
static char g_local_terminfo_dir[1024];

void session_local_set_shell_integration(const char *dir) {
    snprintf(g_local_shell_integration, sizeof g_local_shell_integration, "%s", dir ? dir : "");
}

void session_local_set_term(const char *term, const char *terminfo_dir) {
    bool own = term && strcmp(term, "xterm-liu") == 0 && terminfo_dir && terminfo_dir[0];
    snprintf(g_local_term, sizeof g_local_term, "%s", own ? "xterm-liu" : "");
    snprintf(g_local_terminfo_dir, sizeof g_local_terminfo_dir, "%s", own ? terminfo_dir : "");
}

/* The locale a pane's programs start in (ssh/local_lang.h has the rule). ""
 * = Liu's own environment already names one and nothing was asked: leave it.
 * Worked out once per change of the setting, on the main thread — newlocale()
 * reads the locale's files. */
static char g_local_lang[LOCAL_LANG_CAP];
static bool g_local_lang_known = false;

static bool local_lang_exists(const char *name) {
    locale_t l = newlocale(LC_ALL_MASK, name, (locale_t)0);
    if (!l) return false;
    freelocale(l);
    return true;
}

static bool local_env_names_locale(void) {
    static const char *const keys[] = { "LC_ALL", "LC_CTYPE", "LANG" };
    for (usize i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *v = getenv(keys[i]);
        if (v && v[0]) return true;
    }
    return false;
}

void session_local_set_lang(const char *asked, const char *system_locale) {
    static char last_asked[LOCAL_LANG_CAP], last_system[LOCAL_LANG_CAP];
    if (!asked) asked = "";
    if (!system_locale) system_locale = "";
    if (g_local_lang_known && strcmp(asked, last_asked) == 0 && strcmp(system_locale, last_system) == 0)
        return;                                   /* every config apply comes through here */
    snprintf(last_asked, sizeof last_asked, "%s", asked);
    snprintf(last_system, sizeof last_system, "%s", system_locale);
    char buf[LOCAL_LANG_CAP];
    const char *pick = local_lang_pick(asked, system_locale, local_env_names_locale(),
                                       local_lang_exists, buf, sizeof buf);
    snprintf(g_local_lang, sizeof g_local_lang, "%s", pick ? pick : "");
    g_local_lang_known = true;
}

const char *session_local_lang(void) {
    if (!g_local_lang_known) session_local_set_lang("", "");
    return g_local_lang;
}

/* Is `shell` (a path) the shell called `name`? */
static bool local_shell_is(const char *shell, const char *name) {
    const char *base = shell ? strrchr(shell, '/') : NULL;
    base = base ? base + 1 : shell;
    return base && strcmp(base, name) == 0;
}

static bool local_env_build(LocalEnvVec *v, const char *shell, const LocalEnvVar *extra,
                            i32 extra_count, const char *pwd, u64 pane_serial,
                            const char *mark_key) {
    if (!v || extra_count < 0 || extra_count > 64 ||
        (extra_count > 0 && !extra)) return false;
    memset(v, 0, sizeof *v);
    usize inherited = 0, total_bytes = 0;
    while (environ && environ[inherited]) {
        usize n = strlen(environ[inherited]);
        usize item_bytes;
        if (inherited >= 16384u || !checked_add_usize(n, 1u, &item_bytes) ||
            !checked_add_usize(total_bytes, item_bytes, &total_bytes) ||
            total_bytes > MB(4)) return false;
        inherited++;
    }
    if (!checked_add_usize(inherited, (usize)extra_count + 16u, &v->cap))
        return false;
    v->items = calloc(v->cap, sizeof *v->items);
    if (!v->items) return false;
    for (usize i = 0; i < inherited; i++) {
        /* A pane's first command is Liu's to give (session_create_local_first),
         * never something inherited from the environment Liu started in. */
        if (local_env_matches(environ[i], "LIU_RESUME_COMMAND")) continue;
        /* Nor which Liu, or which delegated job, a pane belongs to: a Liu
         * started from another Liu's pane inherits both. */
        if (local_env_matches(environ[i], "LIU_AGENT_SOCK") ||
            local_env_matches(environ[i], "LIU_AGENT_JOB")) continue;
        v->items[v->count] = strdup(environ[i]);
        if (!v->items[v->count]) { local_env_free(v); return false; }
        v->count++;
    }

    /* What a terminal announces: which terminal this is and that it draws
     * 24-bit colour. Nothing that tells a program to colour output that is NOT
     * going to a terminal, and the user's NO_COLOR is never removed.
     *
     * FORCE_COLOR, CLICOLOR_FORCE and npm_config_color reach past the pane:
     * with them, `ls | od -c` in a pane shows `033 [ 3 4 m s u b 033 [ 3 9 ; 4
     * 9 m` — escape codes in a pipe, so in `$(ls)`, in a log file, and in
     * every tool an agent runs and reads the output of. CLICOLOR alone colours
     * `ls` only when it writes to a terminal; it stays, unless the user has
     * said NO_COLOR. */
    const struct { const char *k, *val; } defaults[] = {
        {"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"},
        {"TERM_PROGRAM", "Liu"},
    };
    for (usize i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
        if (!local_env_set(v, defaults[i].k, defaults[i].val)) {
            local_env_free(v); return false;
        }
    }
    {
        const char *no_color = getenv("NO_COLOR");
        if (!(no_color && no_color[0]) && !local_env_set(v, "CLICOLOR", "1")) {
            local_env_free(v); return false;
        }
    }
    /* A UTF-8 locale, when nothing the pane inherits names one. */
    {
        const char *lang = session_local_lang();
        if (lang[0] && !local_env_set(v, "LANG", lang)) { local_env_free(v); return false; }
    }
    /* Liu's own terminfo entry, when the user asked for it and it is there. */
    if (g_local_term[0] &&
        (!local_env_set(v, "TERM", g_local_term) || !local_env_set(v, "TERMINFO", g_local_terminfo_dir))) {
        local_env_free(v); return false;
    }
    /* Shell integration (core/shell_integration.h). zsh is led to Liu's
     * .zshenv through ZDOTDIR, and the user's own ZDOTDIR — when they have one
     * — rides along so that file can put it back. Other shells only learn
     * where the folder is. */
    if (g_local_shell_integration[0]) {
        if (!local_env_set(v, "LIU_SHELL_INTEGRATION_DIR", g_local_shell_integration)) {
            local_env_free(v); return false;
        }
        if (local_shell_is(shell, "zsh")) {
            char zdot[1100];
            const char *orig = getenv("ZDOTDIR");
            int zn = snprintf(zdot, sizeof zdot, "%s/zsh", g_local_shell_integration);
            if (zn < 0 || (usize)zn >= sizeof zdot ||
                (orig && orig[0] && !local_env_set(v, "LIU_ORIG_ZDOTDIR", orig)) ||
                !local_env_set(v, "ZDOTDIR", zdot)) {
                local_env_free(v); return false;
            }
        }
    }

    {
        const char *helper_dir = liu_helper_bin_dir();
        const char *old = getenv("PATH");
        if (!old || !old[0]) old = "/usr/local/bin:/usr/bin:/bin";

        /* Rebuild PATH without any Liu bundle directory.
         *
         * Dropping it is not only about what WE add: a shell opened by an
         * older build inherits the entry, and every process started from that
         * shell — including another Liu — passes it down again. So the entry
         * outlives the change that introduced it, and `liu` (== `Liu` on a
         * case-insensitive filesystem) stays runnable until it is actively
         * removed. Two things are stripped: this process's own executable
         * directory, and anything ending in a bundle's MacOS/ path. */
        const char *exe_dir = liu_executable_dir();
        static const char *kBundleTail = ".app/Contents/MacOS";
        usize tail_len = strlen(kBundleTail);
        usize exe_len  = (exe_dir && exe_dir[0]) ? strlen(exe_dir) : 0;

        char path[4096];
        usize pn = 0;
        if (helper_dir && helper_dir[0]) {
            int n = snprintf(path, sizeof path, "%s", helper_dir);
            if (n < 0 || (usize)n >= sizeof path) { local_env_free(v); return false; }
            pn = (usize)n;
        }
        for (const char *p2 = old; *p2; ) {
            const char *sep = strchr(p2, ':');
            usize len = sep ? (usize)(sep - p2) : strlen(p2);
            bool drop = (len == 0);
            if (!drop && exe_len == len && !strncmp(p2, exe_dir, len)) drop = true;
            if (!drop && len >= tail_len &&
                !strncmp(p2 + len - tail_len, kBundleTail, tail_len)) drop = true;
            if (!drop) {
                if (pn + 1 + len + 1 >= sizeof path) break;   /* keep what fits */
                if (pn) path[pn++] = ':';
                memcpy(path + pn, p2, len);
                pn += len;
            }
            if (!sep) break;
            p2 = sep + 1;
        }
        path[pn] = '\0';
        if (pn && !local_env_set(v, "PATH", path)) { local_env_free(v); return false; }
    }
    if (pwd && pwd[0] && !local_env_set(v, "PWD", pwd)) {
        local_env_free(v); return false;
    }
    for (i32 i = 0; i < extra_count; i++) {
        if (!local_env_set(v, extra[i].name, extra[i].value)) {
            local_env_free(v); return false;
        }
    }
    /* Which pane this shell is in, for whatever it starts.
     *
     * An agent's notify hook is a grandchild of this shell and reports back
     * over a socket that says nothing about where it ran. A guess from (which
     * CLI, which directory) cannot tell two Claude panes in one repository
     * apart and stops matching the moment the agent changes directory, so a
     * turn's "finished" would be lost. `<pid>.<serial>`: the pid says WHICH
     * Liu (another instance's panes can report to the same socket and must
     * not be matched against this one's panes), the serial is
     * session_instance_id(). Set after `extra` so nothing a caller passes can
     * carry another pane's identity in. */
    /* The key this pane's shell puts on its OSC 133 marks (core/
     * shell_integration.h): Liu's zsh files take it out of the environment
     * into a variable of the shell before anything else runs, so the programs
     * started in the pane do not inherit it. Marks without it still shade the
     * prompt; they are not believed when a command is written to the history.
     * After `extra`, for the reason below. */
    if (mark_key && mark_key[0] && g_local_shell_integration[0] && local_shell_is(shell, "zsh") &&
        !local_env_set(v, "LIU_MARK_KEY", mark_key)) {
        local_env_free(v); return false;
    }
    {
        char pane_id[48];
        snprintf(pane_id, sizeof pane_id, "%u.%u", (u32)getpid(), (u32)pane_serial);
        if (!local_env_set(v, "LIU_PANE_ID", pane_id)) {
            local_env_free(v); return false;
        }
    }
    /* Where `liu-agent` reaches this Liu (agentctl). Only the path: who may
     * use it is decided by the server from the caller's process tree. */
    if (g_local_agent_sock[0] && !local_env_set(v, "LIU_AGENT_SOCK", g_local_agent_sock)) {
        local_env_free(v); return false;
    }
    v->items[v->count] = NULL;
    return true;
}

static bool local_resolve_shell(char *out, usize cap) {
    if (!out || cap == 0) return false;
    const char *shell = getenv("SHELL");
    /* The account's shell is a string inside `scratch`, so `scratch` lives as
     * long as `shell` is read: declared inside the branch, the copy below read
     * a buffer whose scope had ended. */
    struct passwd pw, *result = NULL;
    char scratch[16384];
    if (!shell || shell[0] != '/' || access(shell, X_OK) != 0) {
        if (getpwuid_r(getuid(), &pw, scratch, sizeof scratch, &result) == 0 &&
            result && result->pw_shell && result->pw_shell[0] == '/' &&
            access(result->pw_shell, X_OK) == 0) shell = result->pw_shell;
        else shell = "/bin/sh";
    }
    int n = snprintf(out, cap, "%s", shell);
    return n > 0 && (usize)n < cap;
}

/* Where a local shell starts, decided WITHOUT touching the folder asked for.
 *
 * realpath() or stat() of a folder on a network volume that has gone away
 * answers only when the volume's own timeout does, and done here, on the
 * caller's thread, for a restored, reopened, woken or split tab, that would
 * freeze the app before its first frame. So the child tries it
 * (local_child_chdir), where a wait holds that one shell and nothing else.
 *
 * `want` is the folder asked for when it can be named — absolute, no control
 * byte, short enough — else empty. `fallback` is where a new tab starts: $HOME
 * when Liu itself sits at "/" (opened from Finder or the Dock), else empty, the
 * folder Liu was started in. Neither is checked here: a `want` that is gone,
 * not a folder or not permitted to enter fails its chdir() and the child takes
 * the fallback. */
static void local_plan_cwd(const char *requested, char *want, usize want_cap,
                           char *fallback, usize fallback_cap) {
    if (want && want_cap) want[0] = '\0';
    if (fallback && fallback_cap) fallback[0] = '\0';
    if (!want || !want_cap || !fallback || !fallback_cap) return;
    if (requested && requested[0] == '/' && !liu_path_has_control_bytes(requested)) {
        int n = snprintf(want, want_cap, "%s", requested);
        if (n <= 0 || (usize)n >= want_cap) want[0] = '\0';
    }
    char current[4096];
    if (getcwd(current, sizeof current) && strcmp(current, "/") == 0) {
        const char *home = net_home_dir();
        struct stat st;
        if (home[0] && stat(home, &st) == 0 && S_ISDIR(st.st_mode)) {
            int n = snprintf(fallback, fallback_cap, "%s", home);
            if (n <= 0 || (usize)n >= fallback_cap) fallback[0] = '\0';
        }
    }
}

/* In the forked child, before exec (only chdir/_exit): the folder asked for,
 * else the fallback, else the folder inherited; a fallback that cannot be
 * entered ends the child. */
static void local_child_chdir(const char *want, const char *fallback) {
    if (want[0] && chdir(want) == 0) return;
    if (fallback[0] && chdir(fallback) != 0) _exit(127);
}

static f64 local_child_begin_stop(pid_t pid) {
    /* forkpty() makes the child a session/process-group leader. Signal the
     * group so shells cannot leave grandchildren (dev servers, compilers)
     * running after their tab is closed. Keep the leader unreaped until the
     * final signal, preventing its numeric pid/pgid from being reused. */
    if (kill(-pid, SIGHUP) != 0 && errno == ESRCH) (void)kill(pid, SIGHUP);
    return session_mono_now()+0.1;
}

static void local_child_stop(pid_t pid,f64 deadline) {
    if (pid <= 0) return;
    if (deadline<=0) deadline=local_child_begin_stop(pid);
    for (;;) {
        f64 remaining=deadline-session_mono_now();
        if (remaining<=0) break;
        time_t whole=(time_t)remaining;
        struct timespec delay={.tv_sec=whole,
            .tv_nsec=(long)((remaining-(f64)whole)*1e9)};
        if (nanosleep(&delay,NULL)==0 || errno!=EINTR) break;
    }
    (void)kill(-pid, SIGKILL);
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static bool fd_set_cloexec_checked(int fd) {
    if (fd < 0) return false;
    int flags;
    do { flags = fcntl(fd, F_GETFD, 0); }
    while (flags < 0 && errno == EINTR);
    if (flags < 0) return false;
    int rc;
    do { rc = fcntl(fd, F_SETFD, flags | FD_CLOEXEC); }
    while (rc < 0 && errno == EINTR);
    return rc == 0;
}

/* The state a forked shell must NOT inherit from Liu, set in the child between
 * forkpty() and execve() — async-signal-safe calls only (this process is
 * multithreaded: no malloc, no stdio).
 *
 *  - Ignored and blocked signals (ssh/child_signals.h). Liu ignores SIGPIPE for
 *    the in-process notify server (daemon.c), and an ignored disposition
 *    SURVIVES execve. Every shell, and every program run in it that does not
 *    reset it, would start with SIGPIPE ignored: `yes | head` then dies with
 *    "Broken pipe" write errors instead of quietly stopping, and a writer to a
 *    closed pipe spins instead of exiting. The same holds for what Liu's own
 *    launcher ignored — `Liu &` from a script ignores SIGINT and SIGQUIT, and
 *    then Ctrl+C would never stop a foreground job.
 *  - Open descriptors above stderr. CoreAnimation/Metal open the shader library
 *    and its compiler cache without O_CLOEXEC (newLibraryWithFile: is outside
 *    Liu's control), so descriptors into Liu's own bundle would be visible in
 *    every shell. Not secret, but a spawn path that relies on every library
 *    having remembered O_CLOEXEC is one library away from handing a child the
 *    vault or the notify socket. The child needs 0, 1 and 2 and nothing else.
 *
 * `maxfd` is computed BEFORE the fork (getdtablesize is not on the safe list). */
static int local_child_maxfd(void) {
    int n = getdtablesize();
    if (n < 64) n = 64;
    if (n > 65536) n = 65536;
    return n;
}

static void local_child_hygiene(int maxfd) {
    child_signals_default();
    for (int fd = 3; fd < maxfd; fd++) close(fd);
}

static bool local_parent_fd_setup(int fd) {
    return fd_set_cloexec_checked(fd) && net_set_nonblocking(fd);
}
#endif /* !PLATFORM_WIN32 */

static _Atomic u64 g_session_instance_next;

static u64 session_new_instance_id(void) {
    u64 id = atomic_fetch_add_explicit(&g_session_instance_next, 1u, memory_order_relaxed) + 1u;
    if (!id) id = atomic_fetch_add_explicit(&g_session_instance_next, 1u, memory_order_relaxed) + 1u;
    return id;
}

u64 session_instance_id(const Session *s) {
    return s ? s->instance_id : 0;
}

/* 128 random bits as hex, once per local session. No key (the RNG failed) is
 * a pane whose marks are simply never believed. */
static const char *session_make_mark_key(Session *s) {
    u8 rnd[16];
    if (!s->mark_key[0] && crypto_random(rnd, sizeof rnd)) {
        static const char hexd[] = "0123456789abcdef";
        for (int i = 0; i < 16; i++) {
            s->mark_key[i * 2]     = hexd[rnd[i] >> 4];
            s->mark_key[i * 2 + 1] = hexd[rnd[i] & 0xf];
        }
        s->mark_key[32] = '\0';
        secure_zero(rnd, sizeof rnd);
    }
    return s->mark_key;
}

const char *session_shell_mark_key(const Session *s) {
    return s && s->type == SESSION_LOCAL ? s->mark_key : "";
}

/* How a pane's shell is told to run a command before its first prompt.
 *
 * A zsh that reads Liu's integration takes it from LIU_RESUME_COMMAND: its
 * .zshenv moves the text into a shell variable before any startup file runs
 * (no program the shell starts inherits it), and the first prompt runs it as
 * a foreground job of the interactive shell — after every startup file, with
 * job control, and the prompt that follows is the shell's own. Any other shell
 * is started as `shell -l -i -c '<command>; exec shell -l'`: the command runs
 * with the user's interactive environment and the pane goes on as a login
 * shell when it ends. Nothing is typed into the pane either way. */
static bool local_first_command_script(const char *shell, const char *command,
                                       char *out, usize cap) {
    if (!shell || !command || !command[0] || strchr(shell, '\'')) return false;
    for (const char *c = command; *c; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7f) return false;
    int n = snprintf(out, cap, "%s; exec '%s' -l", command, shell);
    return n > 0 && (usize)n < cap;
}

static Session *session_create_local_impl(i32 cols, i32 rows,
                                          const LocalEnvVar *env, i32 env_count,
                                          const char *cwd, const char *first_command);

Session *session_create_local_with_env(i32 cols, i32 rows,
                                       const LocalEnvVar *env, i32 env_count,
                                       const char *cwd) {
    return session_create_local_impl(cols, rows, env, env_count, cwd, NULL);
}

Session *session_create_local_first(i32 cols, i32 rows, const char *cwd,
                                    const char *first_command) {
    return session_create_local_impl(cols, rows, NULL, 0, cwd, first_command);
}

static Session *session_create_local_impl(i32 cols, i32 rows,
                                          const LocalEnvVar *env, i32 env_count,
                                          const char *cwd, const char *first_command) {
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    atomic_init(&s->ref_count, 1u);
    s->instance_id = session_new_instance_id();
    atomic_init(&s->io_suspend_count, 0u);
    atomic_init(&s->io_wanted, false);
    atomic_init(&s->io_yielding, false);
    session_io_kick_init(s);
    s->type = SESSION_LOCAL;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;

    if (cols <= 0 || cols > (i32)UINT16_MAX ||
        rows <= 0 || rows > (i32)UINT16_MAX || env_count < 0 ||
        env_count > 64 || (env_count > 0 && !env)) {
        set_error(s, "invalid local session parameters");
        return s;
    }
    for (i32 i = 0; i < env_count; i++) {
        if (!memchr(env[i].name, '\0', sizeof env[i].name) ||
            !memchr(env[i].value, '\0', sizeof env[i].value)) {
            set_error(s, "invalid local environment");
            return s;
        }
    }

    char shell[512], child_cwd[4096], child_fallback[4096];
    LocalEnvVec child_env = {0};
    local_plan_cwd(cwd, child_cwd, sizeof child_cwd, child_fallback, sizeof child_fallback);
    const char *child_pwd = child_cwd[0] ? child_cwd : child_fallback;
    if (!local_resolve_shell(shell, sizeof shell) ||
        !local_env_build(&child_env, shell, env, env_count,
                         child_pwd[0] ? child_pwd : NULL, s->instance_id,
                         session_make_mark_key(s))) {
        set_error(s, "could not prepare local environment");
        return s;
    }

    /* A command to run before the first prompt (local_first_command_script). */
    char first_script[2048];
    bool first_via_env = false, first_via_argv = false;
    if (first_command && first_command[0]) {
        if (g_local_shell_integration[0] && local_shell_is(shell, "zsh")) {
            first_via_env = local_first_command_script(shell, first_command,
                                                       first_script, sizeof first_script) &&
                            local_env_set(&child_env, "LIU_RESUME_COMMAND", first_command);
        } else {
            first_via_argv = local_first_command_script(shell, first_command,
                                                        first_script, sizeof first_script);
        }
        if (!first_via_env && !first_via_argv) {
            local_env_free(&child_env);
            set_error(s, "could not prepare the pane's first command");
            return s;
        }
    }

    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };

    const int child_maxfd = local_child_maxfd();
    pid_t pid = forkpty(&s->pty_master, NULL, NULL, &ws);
    if (pid < 0) {
        local_env_free(&child_env);
        set_error(s, "forkpty failed");
        return s;
    }

    if (pid == 0) {
        /* All allocation, environment construction and path resolution was
         * completed before fork. In this multithreaded process the child may
         * safely do only chdir/execve/_exit. */
        local_child_hygiene(child_maxfd);
        local_child_chdir(child_cwd, child_fallback);
        if (first_via_argv) {
            char *argv[] = { shell, (char *)"-l", (char *)"-i", (char *)"-c", first_script, NULL };
            execve(shell, argv, child_env.items);
        } else {
            char *argv[] = { shell, (char *)"-l", NULL };
            execve(shell, argv, child_env.items);
        }
        _exit(127);
    }

    /* Parent */
    local_env_free(&child_env);
    s->child_pid = pid;
    s->born_mono = session_mono_now();
    if (!local_parent_fd_setup(s->pty_master)) {
        close(s->pty_master);
        s->pty_master = -1;
        local_child_stop(s->child_pid,0);
        s->child_pid = -1;
        set_error(s, "could not configure local PTY");
        return s;
    }
    /* NULL is a supported outcome (thread off, or the live-reader cap):
     * the session then behaves exactly as it did before. */
    if (session_pty_thread_enabled())
        s->reader = pty_reader_start(s->pty_master, 0 /* default ring */);
    s->status = SESSION_CONNECTED;
    local_pane_note(s->child_pid, s->instance_id);
    return s;
}

Session *session_create_command(i32 cols, i32 rows,
                                const LocalEnvVar *env, i32 env_count,
                                const char *cwd, const char *command) {
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    atomic_init(&s->ref_count, 1u);
    s->instance_id = session_new_instance_id();
    atomic_init(&s->io_suspend_count, 0u);
    atomic_init(&s->io_wanted, false);
    atomic_init(&s->io_yielding, false);
    session_io_kick_init(s);
    s->type = SESSION_LOCAL;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;

    if (!command || !command[0]) {
        set_error(s, "no command");
        return s;
    }
    if (strnlen(command, 64u * 1024u + 1u) > 64u * 1024u ||
        cols <= 0 || cols > (i32)UINT16_MAX ||
        rows <= 0 || rows > (i32)UINT16_MAX || env_count < 0 ||
        env_count > 64 || (env_count > 0 && !env)) {
        set_error(s, "invalid command session parameters");
        return s;
    }
    for (i32 i = 0; i < env_count; i++) {
        if (!memchr(env[i].name, '\0', sizeof env[i].name) ||
            !memchr(env[i].value, '\0', sizeof env[i].value)) {
            set_error(s, "invalid local environment");
            return s;
        }
    }

    char shell[512], child_cwd[4096], child_fallback[4096];
    LocalEnvVec child_env = {0};
    local_plan_cwd(cwd, child_cwd, sizeof child_cwd, child_fallback, sizeof child_fallback);
    const char *child_pwd = child_cwd[0] ? child_cwd : child_fallback;
    if (!local_resolve_shell(shell, sizeof shell) ||
        !local_env_build(&child_env, shell, env, env_count,
                         child_pwd[0] ? child_pwd : NULL, s->instance_id,
                         session_make_mark_key(s))) {
        set_error(s, "could not prepare command environment");
        return s;
    }
    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };

    const int child_maxfd = local_child_maxfd();
    pid_t pid = forkpty(&s->pty_master, NULL, NULL, &ws);
    if (pid < 0) {
        local_env_free(&child_env);
        set_error(s, "forkpty failed");
        return s;
    }

    if (pid == 0) {
        local_child_hygiene(child_maxfd);
        /* Child — run `command` through the user's LOGIN shell:
         *     shell -l -c "<command>"
         * -l loads the user's profile so PATH has node/npm even when liu was
         * Finder/Dock-launched with a minimal GUI PATH. The command is passed
         * VERBATIM (NOT prefixed with `exec`) so normal shell syntax works —
         * env-prefixed (`PORT=3000 npm run dev`) and chained
         * (`npm run build && npm run preview`) commands behave correctly. A
         * leading `exec` would mis-parse the env assignment as argv[0] and would
         * drop everything after the first `&&`. forkpty() already called
         * setsid(), so the child shell leads its own process group (pgid == pid);
         * the manager killpg(child_pid, …) to tree-kill the shell + node + its
         * esbuild/vite workers together, and `sh -c` propagates the dev server's
         * exit code. */
        local_child_chdir(child_cwd, child_fallback);
        char *argv[] = { shell, (char *)"-l", (char *)"-c",
                         (char *)command, NULL };
        execve(shell, argv, child_env.items);
        _exit(127);
    }

    /* Parent */
    local_env_free(&child_env);
    s->child_pid = pid;
    s->born_mono = session_mono_now();
    if (!local_parent_fd_setup(s->pty_master)) {
        close(s->pty_master);
        s->pty_master = -1;
        local_child_stop(s->child_pid,0);
        s->child_pid = -1;
        set_error(s, "could not configure command PTY");
        return s;
    }
    /* NULL is a supported outcome (thread off, or the live-reader cap):
     * the session then behaves exactly as it did before. */
    if (session_pty_thread_enabled())
        s->reader = pty_reader_start(s->pty_master, 0 /* default ring */);
    s->status = SESSION_CONNECTED;
    return s;
}

Session *session_create_local(i32 cols, i32 rows) {
    return session_create_local_with_env(cols, rows, NULL, 0, NULL);
}

/* =========================================================================
 * SSH session
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * ssh_authenticate — extracted auth logic, used for both proxy hops and
 * the final destination.  `session` must be blocking.
 * Returns 0 on success, -1 on failure (sets s->error_msg).
 * ------------------------------------------------------------------------- */
static int ssh_authenticate(Session *s, LIBSSH2_SESSION *session,
                            const char *username, const char *password,
                            const char *key_path, const char *key_passphrase,
                            AuthMethod method) {
    int auth_rc = -1;
    switch (method) {
    case AUTH_PASSWORD:
        auth_rc = libssh2_userauth_password(session, username, password);
        if (auth_rc != 0 && password[0]) {
            auth_rc = libssh2_userauth_keyboard_interactive(
                session, username, &ssh_kbdint_callback);
        }
        break;
    case AUTH_PUBLICKEY: {
        char pub_path[520];
        snprintf(pub_path, sizeof(pub_path), "%s.pub", key_path);
        const char *pp = (key_passphrase && key_passphrase[0]) ? key_passphrase : NULL;
        auth_rc = libssh2_userauth_publickey_fromfile(
            session, username, pub_path, key_path, pp);
        break;
    }
    case AUTH_AGENT: {
        LIBSSH2_AGENT *agent = libssh2_agent_init(session);
        if (agent && libssh2_agent_connect(agent) == 0) {
            libssh2_agent_list_identities(agent);
            struct libssh2_agent_publickey *identity = NULL;
            while (libssh2_agent_get_identity(agent, &identity, identity) == 0) {
                if (libssh2_agent_userauth(agent, username, identity) == 0) {
                    auth_rc = 0;
                    break;
                }
            }
            libssh2_agent_disconnect(agent);
        }
        if (agent) libssh2_agent_free(agent);
        break;
    }
    case AUTH_GSSAPI:
        /* Handled by the gssapi_auth path in ssh_do_connect; shouldn't reach here */
        break;
    }
    if (auth_rc != 0) {
        char *errmsg;
        libssh2_session_last_error(session, &errmsg, NULL, 0);
        set_error(s, errmsg);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Channel relay thread — bidirectionally copies data between a local
 * socketpair fd and a libssh2 direct-tcpip channel.  Used for ProxyJump
 * so the next hop's libssh2 can treat the socketpair fd as a "raw TCP"
 * socket.
 *
 * The thread sleeps in poll() on the descriptors that can let it move a
 * byte: the socketpair end, the socket its libssh2 session runs over, and a
 * stop pipe. It never puts a descriptor under the main loop's watch — the
 * main loop does not service this relay.
 * ------------------------------------------------------------------------- */

/* The longest wait with nothing to do. Every way a byte can move has a
 * descriptor in the wait, so this only bounds a readiness that was missed. */
#define PROXY_RELAY_BACKSTOP_MS 500
/* A wait that returned ready and then moved nothing (a side that can be read
 * while its bytes have nowhere to go yet, or a hang-up) backs off, doubling up
 * to this long, so the thread cannot spin on it. */
#define PROXY_RELAY_SPIN_MAX_MS 50

static void *channel_relay_thread(void *arg) {
    ChannelRelay *r = (ChannelRelay *)arg;
    libssh2_session_set_blocking(r->session, 0);
    SocketChannelRelay io = {
        .channel = r->channel,
        .fd = r->local_fd,
        .active = true,
    };
    bool woke_ready = false;
    i32 spin_ms = 0;
    while (atomic_load_explicit(&r->running, memory_order_acquire)) {
        bool moved_any = false, budget_spent = false;
        for (i32 step = 0; step < SOCKET_RELAY_POLL_STEPS; step++) {
            bool moved = false;
            if (!socket_channel_relay_step(&io, &moved)) goto done;
            if (!moved) break;
            moved_any = true;
            if (step + 1 == SOCKET_RELAY_POLL_STEPS) budget_spent = true;
        }
        if (budget_spent) { spin_ms = 0; woke_ready = false; continue; }
        if (moved_any) spin_ms = 0;
        else if (woke_ready) {
            spin_ms = spin_ms ? spin_ms * 2 : 1;
            if (spin_ms > PROXY_RELAY_SPIN_MAX_MS) spin_ms = PROXY_RELAY_SPIN_MAX_MS;
            struct timespec t = { .tv_sec = 0, .tv_nsec = (long)spin_ms * 1000000L };
            (void)nanosleep(&t, NULL);
        }

        /* Each side is asked only for what the next step would do with it:
         * the socketpair is read when the channel can take its bytes and
         * written while bytes for it wait; the session's socket is read while
         * a channel read, write or EOF is pending (a write may wait for the
         * server's window) and in whatever direction libssh2 last blocked. */
        struct pollfd p[3];
        nfds_t n = 0;
        p[n++] = (struct pollfd){ .fd = r->stop_rd, .events = POLLIN };
        short local_ev = 0;
        if (!io.fd_eof && io.to_channel_len == 0) local_ev |= POLLIN;
        if (io.to_fd_off < io.to_fd_len) local_ev |= POLLOUT;
        p[n++] = (struct pollfd){ .fd = io.fd, .events = local_ev };
        int dirs = libssh2_session_block_directions(r->session);
        short sess_ev = 0;
        if ((!io.chan_eof && io.to_fd_len == 0) || io.to_channel_len > 0 ||
            (io.fd_eof && !io.eof_sent) || (dirs & LIBSSH2_SESSION_BLOCK_INBOUND))
            sess_ev |= POLLIN;
        if (dirs & LIBSSH2_SESSION_BLOCK_OUTBOUND) sess_ev |= POLLOUT;
        if (r->session_sock >= 0)
            p[n++] = (struct pollfd){ .fd = r->session_sock, .events = sess_ev };

        int wait_ms = PROXY_RELAY_BACKSTOP_MS;
        if (io.shut_wr) {
            f64 left = io.linger_until - platform_time_sec();
            int left_ms = left <= 0.0 ? 0 : (int)(left * 1000.0) + 1;
            if (left_ms < wait_ms) wait_ms = left_ms;
        }
        int rc = poll(p, n, wait_ms);
        woke_ready = rc > 0 && p[0].revents == 0;
    }
done:
    atomic_store_explicit(&r->running, false, memory_order_release);
    return NULL;
}

/* A relay for `channel` on `session` (which runs over `session_sock`), with
 * its stop pipe; the thread is not started. NULL when out of memory or fds. */
static ChannelRelay *channel_relay_new(int local_fd, LIBSSH2_CHANNEL *channel,
                                       LIBSSH2_SESSION *session, int session_sock) {
    ChannelRelay *r = calloc(1, sizeof(ChannelRelay));
    if (!r) return NULL;
    int pp[2];
    if (pipe(pp) != 0) { free(r); return NULL; }
    if (!fd_set_cloexec_checked(pp[0]) || !fd_set_cloexec_checked(pp[1]) ||
        !net_set_nonblocking(pp[1])) {
        close(pp[0]); close(pp[1]); free(r); return NULL;
    }
    r->local_fd = local_fd;
    r->channel = channel;
    r->session = session;
    r->session_sock = session_sock;
    r->stop_rd = pp[0];
    r->stop_wr = pp[1];
    atomic_init(&r->running, true);
    return r;
}

/* Frees a relay whose thread never started (its local_fd is the caller's). */
static void channel_relay_discard(ChannelRelay *r) {
    if (!r) return;
    close(r->stop_rd);
    close(r->stop_wr);
    free(r);
}

static bool proxy_socketpair(int sv[2]) {
    if (!sv || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
    if (!fd_set_cloexec_checked(sv[0]) || !fd_set_cloexec_checked(sv[1]) ||
        !net_set_nonblocking(sv[0])) {
        close(sv[0]);
        close(sv[1]);
        sv[0] = sv[1] = -1;
        return false;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return true;
}

/* -------------------------------------------------------------------------
 * Proxy hop cleanup helper
 * ------------------------------------------------------------------------- */
static void proxy_hops_cleanup(Session *s) {
    for (i32 i = 0; i < s->proxy_hop_count; i++) {
        /* Stop relay thread */
        if (s->proxy_relays[i]) {
            ChannelRelay *r = s->proxy_relays[i];
            atomic_store_explicit(&r->running, false, memory_order_release);
            ssize_t w;
            do { w = write(r->stop_wr, "x", 1); } while (w < 0 && errno == EINTR);
            pthread_join(r->thread, NULL);
            close(r->local_fd);
            channel_relay_discard(r);
            s->proxy_relays[i] = NULL;
        }
        ProxyHopState *h = &s->proxy_hops[i];
        if (h->tunnel) {
            libssh2_channel_free(h->tunnel);
            h->tunnel = NULL;
        }
        if (h->session) {
            libssh2_session_disconnect(h->session, "proxy cleanup");
            libssh2_session_free(h->session);
            h->session = NULL;
        }
        if (h->sock >= 0) {
            close(h->sock);
            h->sock = -1;
        }
    }
    s->proxy_hop_count = 0;
}

/* Close every agent channel this session relays, and the ones still waiting to
 * be taken. While the libssh2 session is alive: closing a channel talks to the
 * server. */
static void session_agent_cleanup(Session *s) {
#ifdef LIBSSH2_CALLBACK_AUTHAGENT
    if (s->agent_relays) {
        for (i32 i = 0; i < s->agent_relay_count; i++)
            socket_channel_relay_close(&s->agent_relays[i]);
    }
    s->agent_relay_count = 0;
    agent_pending_cleanup(s);
#else
    (void)s;
#endif
}

/* -------------------------------------------------------------------------
 * Remote forwarding cleanup helper
 * ------------------------------------------------------------------------- */
static void remote_forward_cleanup_all(Session *s) {
    /* Stop all accepted main-thread relays. */
    if (s->remote_fwd_conns) {
        for (i32 i = 0; i < MAX_REMOTE_FWD_CONNS; i++) {
            RemoteForwardConn *c = s->remote_fwd_conns[i];
            if (!c) continue;
            socket_channel_relay_close(c);
            free(c);
            s->remote_fwd_conns[i] = NULL;
        }
    }
    s->remote_fwd_conn_count = 0;

    /* Cancel listeners */
    if (s->remote_listeners) {
        for (i32 i = 0; i < s->remote_listener_count; i++) {
            RemoteForwardListener *l = &s->remote_listeners[i];
            if (l->listener) {
                libssh2_channel_forward_cancel(l->listener);
                l->listener = NULL;
            }
            l->active = false;
        }
    }
    s->remote_listener_count = 0;
}

/* Body of the blocking connect path — shared by initial connect and
 * session_reconnect().  Must run on a thread that has exclusive access to
 * `s->ssh_session` / `s->ssh_channel` / `s->sock`.  Returns 0 on success
 * or sets s->status to SESSION_ERROR and returns -1. */

/* Start the interactive shell on an opened channel with the pty already
 * requested. See ssh_env_bootstrap.h for why this is not always a plain
 * shell request. Returns false when the server declined every route. */
/* `start_dir` ("" or NULL = none) is the folder a split asked for: it can only
 * be given through the exec request, so a usable one sends the bootstrap even
 * when the server took the variables by setenv. */
static bool ssh_start_shell(LIBSSH2_SESSION *ssh, LIBSSH2_CHANNEL *ch,
                            const char *start_dir) {
#ifdef LIU_VERSION
    const char *version = LIU_VERSION;
#else
    const char *version = "";
#endif
    char vars[4][80];
    i32 n = ssh_env_bootstrap_vars(version, vars, 4);
    bool colorterm_accepted = false;
    for (i32 i = 0; i < n; i++) {
        char *eq = strchr(vars[i], '=');
        if (!eq) continue;
        *eq = '\0';
        int rc = libssh2_channel_setenv(ch, vars[i], eq + 1);
        if (rc == 0 && strcmp(vars[i], "COLORTERM") == 0) colorterm_accepted = true;
        *eq = '=';
    }
    bool want_dir = ssh_start_dir_ok(start_dir);
    if ((!colorterm_accepted || want_dir) &&
        ssh_env_bootstrap_applicable(libssh2_session_banner_get(ssh))) {
        char cmd[1024];
        if (ssh_env_bootstrap_command_in(version, start_dir, cmd, sizeof cmd) &&
            libssh2_channel_exec(ch, cmd) == 0)
            return true;
        /* The exec request itself was refused (a server that only serves
         * shell requests): the plain shell is still better than no session. */
    }
    return libssh2_channel_shell(ch) == 0;
}

/* The session whose libssh2 session, socket and io lock this session's calls
 * go to: itself, or the one whose connection it borrows. */
static Session *ssh_connection_owner(Session *s) {
    if (s && !s->is_shared_owner && s->shared_session) return s->shared_session;
    return s;
}

/* Ask the server to forward the agent to this session's shell, and register
 * what answers its channels. Nothing is requested when there is no agent to
 * forward to (a socket path that is not set would make the remote SSH_AUTH_SOCK
 * point at something that can only fail) or when this libssh2 cannot take the
 * channel (older than 1.11.1 has no callback for it). Never fatal. */
static void ssh_request_agent_forwarding(Session *s) {
#ifdef LIBSSH2_CALLBACK_AUTHAGENT
    const char *sock = getenv("SSH_AUTH_SOCK");
    if (!sock || !sock[0]) {
        fprintf(stderr, "Agent forwarding skipped: no agent (SSH_AUTH_SOCK is not set)\n");
        return;
    }
    /* Before the request: the server may open a channel the moment the shell
     * starts. */
#if defined(LIBSSH2_VERSION_NUM) && LIBSSH2_VERSION_NUM >= 0x010b01
    libssh2_session_callback_set2(s->ssh_session, LIBSSH2_CALLBACK_AUTHAGENT,
                                  (libssh2_cb_generic *)agent_open_callback);
#else
    libssh2_session_callback_set(s->ssh_session, LIBSSH2_CALLBACK_AUTHAGENT,
                                 (void *)agent_open_callback);
#endif
    if (libssh2_channel_request_auth_agent(s->ssh_channel) != 0)
        fprintf(stderr, "Agent forwarding refused by the server\n");
    else
        atomic_store_explicit(&ssh_connection_owner(s)->agent_active, true, memory_order_release);
#else
    (void)s;
    fprintf(stderr, "Agent forwarding skipped: libssh2 older than 1.11.1 cannot take the channel\n");
#endif
}

static int ssh_do_connect(Session *s) {
    const SSHConfig *config = s->config;

    #define CANCELLED() atomic_load_explicit(&s->connect_cancel, memory_order_acquire)
    #define FAIL(msg) do { set_error(s, msg); return -1; } while (0)

    if (CANCELLED()) FAIL("cancelled");

    /* ==================================================================
     * ProxyJump / multi-hop chain
     * ==================================================================
     * If config->proxy_chain_len > 0, the connection is a chain:
     *   TCP -> hop0 -> direct-tcpip -> hop1 -> ... -> final destination
     * Each hop gets its own libssh2 session.  Between hops a socketpair +
     * relay thread lets the next hop's libssh2 do a normal handshake over
     * what looks like a plain fd.
     * ================================================================== */

    int final_sock = -1;  /* fd the final session will use */

    if (config->proxy_chain_len > 0) {
        LIBSSH2_SESSION *prev_session = NULL;

        for (i32 hop = 0; hop < config->proxy_chain_len; hop++) {
            if (CANCELLED()) FAIL("cancelled during proxy chain");

            const ProxyHop *ph = &config->proxy_chain[hop];
            ProxyHopState *hs = &s->proxy_hops[hop];
            memset(hs, 0, sizeof(*hs));
            hs->sock = -1;

            if (hop == 0) {
                /* First hop: plain TCP */
                hs->sock = net_tcp_connect_ex(ph->hostname, ph->port,
                                              NET_CONNECT_TIMEOUT_MS, &s->connect_cancel);
                if (hs->sock < 0)
                    FAIL(CANCELLED() ? "cancelled" : "TCP connection to proxy hop failed");
            } else {
                /* Subsequent hops: open direct-tcpip on previous session,
                 * create socketpair, spawn relay thread */
                libssh2_session_set_blocking(prev_session, 1);
                LIBSSH2_CHANNEL *tunnel = libssh2_channel_direct_tcpip(
                    prev_session, ph->hostname, ph->port);
                if (!tunnel) FAIL("Failed to open tunnel to next proxy hop");

                s->proxy_hops[hop - 1].tunnel = tunnel;

                int sv[2];
                if (!proxy_socketpair(sv))
                    FAIL("socketpair failed for proxy relay");

                ChannelRelay *relay = channel_relay_new(sv[0], tunnel, prev_session,
                                                        s->proxy_hops[hop - 1].sock);
                if (!relay) { close(sv[0]); close(sv[1]); FAIL("OOM"); }
                s->proxy_relays[hop - 1] = relay;

                if (pthread_create(&relay->thread, NULL, channel_relay_thread, relay) != 0) {
                    close(sv[0]); close(sv[1]); channel_relay_discard(relay);
                    s->proxy_relays[hop - 1] = NULL;
                    FAIL("Failed to spawn proxy relay thread");
                }

                hs->sock = sv[1]; /* the "remote" end looks like a TCP socket */
            }

            /* Count this hop as soon as its socket exists — BEFORE the handshake
             * — so a session_init / handshake / auth failure leaves the in-flight
             * session + socket covered by proxy_hops_cleanup (run by the teardown
             * and reconnect paths). Those paths pthread_join this worker first
             * (ssh_session.c:1964), so there is no concurrent free. Without this,
             * every failed proxy hop leaked a libssh2 session + a socket fd, and
             * a flaky-proxy auto-reconnect loop exhausted the fd table. */
            s->proxy_hop_count = hop + 1;

            /* SSH handshake on this hop */
            hs->session = libssh2_session_init();
            if (!hs->session) FAIL("libssh2_session_init failed for proxy hop");
            *libssh2_session_abstract(hs->session) = s;
            libssh2_session_set_blocking(hs->session, 1);
            ssh_wait_bounded(hs->session);
            connect_fd_publish(s, hs->sock);

            ssh_offer_algorithms(hs->session, ph->hostname, ph->port > 0 ? ph->port : 22);
            if (libssh2_session_handshake(hs->session, hs->sock) != 0) {
                ssh_handshake_failed(s, hs->session);
                return -1;
            }
            if (CANCELLED()) FAIL("cancelled");

            /* The hop's host key goes through the same known_hosts check and
             * the same accept/changed dialog as the destination, BEFORE any
             * credential is offered to it. */
            if (ssh_verify_hostkey_for(s, hs->session, ph->hostname,
                                       ph->port > 0 ? ph->port : 22) != 0)
                return -1;

            /* Authenticate proxy hop */
            ssh_wait_unbounded(hs->session);   /* a person may be answering */
            ssh_auth_peer_set(s, ph->username, ph->hostname, ph->port > 0 ? ph->port : 22, false);
            if (ssh_authenticate(s, hs->session, ph->username, "",
                                 ph->key_path, "", ph->auth_method) != 0) {
                return -1;
            }
            ssh_wait_bounded(hs->session);

            prev_session = hs->session;
            /* proxy_hop_count already bumped above, before the handshake. */
        }

        /* Open direct-tcpip from last proxy hop to the final destination */
        libssh2_session_set_blocking(prev_session, 1);
        LIBSSH2_CHANNEL *final_tunnel = libssh2_channel_direct_tcpip(
            prev_session, config->hostname, config->port);
        if (!final_tunnel) FAIL("Failed to open tunnel to final destination");

        s->proxy_hops[s->proxy_hop_count - 1].tunnel = final_tunnel;

        int sv[2];
        if (!proxy_socketpair(sv))
            FAIL("socketpair failed for final tunnel relay");

        ChannelRelay *relay = channel_relay_new(sv[0], final_tunnel, prev_session,
                                                s->proxy_hops[s->proxy_hop_count - 1].sock);
        if (!relay) { close(sv[0]); close(sv[1]); FAIL("OOM"); }
        s->proxy_relays[s->proxy_hop_count - 1] = relay;

        if (pthread_create(&relay->thread, NULL, channel_relay_thread, relay) != 0) {
            close(sv[0]); close(sv[1]); channel_relay_discard(relay);
            s->proxy_relays[s->proxy_hop_count - 1] = NULL;
            FAIL("Failed to spawn final tunnel relay thread");
        }

        final_sock = sv[1];
    } else {
        /* No proxy — plain TCP to destination */
        final_sock = net_tcp_connect_ex(config->hostname, config->port,
                                        NET_CONNECT_TIMEOUT_MS, &s->connect_cancel);
        if (final_sock < 0)
            FAIL(CANCELLED() ? "cancelled"
                             : "Could not reach the host (no answer, refused, or name not found)");
    }

    if (CANCELLED()) {
        close(final_sock);
        FAIL("cancelled");
    }

    s->sock = final_sock;
    connect_fd_publish(s, final_sock);

    /* ==================================================================
     * SSH handshake + auth on the final destination
     * ================================================================== */

    s->ssh_session = libssh2_session_init();
    if (!s->ssh_session) {
        connect_fd_forget(s, s->sock);
        close(s->sock); s->sock = -1;
        FAIL("libssh2_session_init failed");
    }
    *libssh2_session_abstract(s->ssh_session) = s;
    libssh2_session_set_blocking(s->ssh_session, 1);
    ssh_wait_bounded(s->ssh_session);

    ssh_offer_algorithms(s->ssh_session, config->hostname, config->port);
    if (libssh2_session_handshake(s->ssh_session, s->sock) != 0) {
        ssh_handshake_failed(s, s->ssh_session);
        return -1;
    }
    /* From here on a failure is the server's answer, not the network's. */
    atomic_store_explicit(&s->connect_past_network, true, memory_order_release);

    if (ssh_verify_hostkey(s) != 0) {
        return -1;
    }
    if (CANCELLED()) FAIL("cancelled");

    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_AUTHENTICATING;
    pthread_mutex_unlock(&s->status_lock);

    /* Check which auth methods the server supports */
    char *auth_list = libssh2_userauth_list(s->ssh_session, config->username,
                                             (unsigned int)strlen(config->username));
    ssh_wait_unbounded(s->ssh_session);        /* a person may be answering */
    ssh_auth_peer_set(s, config->username, config->hostname, config->port, true);

    int auth_rc = -1;
    s->auth_refused[0] = '\0';

    /* GSSAPI: when gssapi_auth flag is set OR auth_method is AUTH_GSSAPI,
     * try methods in order: agent -> keyboard-interactive (GSSAPI fallback)
     * -> publickey -> password.  Many Kerberos-enabled servers accept
     * keyboard-interactive or even empty-password auth when a valid ticket
     * exists in the user's credential cache. */
    if (config->auth_method == AUTH_GSSAPI || config->gssapi_auth) {
        bool server_has_kbdint = auth_list && strstr(auth_list, "keyboard-interactive");
        bool server_has_passwd = auth_list && strstr(auth_list, "password");
        bool server_has_pubkey = auth_list && strstr(auth_list, "publickey");

        /* 1) Try SSH agent first (tickets may be forwarded via agent) */
        if (auth_rc != 0) {
            LIBSSH2_AGENT *agent = libssh2_agent_init(s->ssh_session);
            if (agent && libssh2_agent_connect(agent) == 0) {
                libssh2_agent_list_identities(agent);
                struct libssh2_agent_publickey *identity = NULL;
                while (libssh2_agent_get_identity(agent, &identity, identity) == 0) {
                    if (libssh2_agent_userauth(agent, config->username, identity) == 0) {
                        auth_rc = 0;
                        break;
                    }
                }
                libssh2_agent_disconnect(agent);
            }
            if (agent) libssh2_agent_free(agent);
        }

        /* 2) keyboard-interactive — Kerberos servers commonly use this */
        if (auth_rc != 0 && server_has_kbdint) {
            auth_rc = libssh2_userauth_keyboard_interactive(
                s->ssh_session, config->username, &ssh_kbdint_callback);
        }

        /* 3) Try password with empty string (some GSSAPI setups accept this) */
        if (auth_rc != 0 && server_has_passwd) {
            auth_rc = libssh2_userauth_password(s->ssh_session, config->username, "");
        }

        /* 4) Public key if available — prefer in-memory PEM from the
         *    vault when present; fall back to on-disk key file. */
        if (auth_rc != 0 && server_has_pubkey && config->key_pem) {
            const char *pp = config->key_passphrase[0] ? config->key_passphrase : NULL;
            auth_rc = libssh2_userauth_publickey_frommemory(
                s->ssh_session,
                config->username, strlen(config->username),
                NULL, 0,  /* derive public key from private */
                (const char *)config->key_pem, config->key_pem_len,
                pp);
        }
        if (auth_rc != 0 && server_has_pubkey && config->key_path[0]) {
            char pub_path[520];
            snprintf(pub_path, sizeof(pub_path), "%s.pub", config->key_path);
            const char *pp = config->key_passphrase[0] ? config->key_passphrase : NULL;
            auth_rc = libssh2_userauth_publickey_fromfile(
                s->ssh_session, config->username, pub_path, config->key_path, pp);
        }

        /* 5) Password as last resort */
        if (auth_rc != 0 && server_has_passwd && config->password[0]) {
            auth_rc = libssh2_userauth_password(s->ssh_session, config->username,
                                                 config->password);
        }
    } else {
        /* Standard auth paths */
        switch (config->auth_method) {
        case AUTH_PASSWORD:
            /* Attempt password auth if a password is set; then always fall
             * back to keyboard-interactive on failure. The fallback is
             * unconditional so that a Recent-row reconnect with an empty
             * password field still gets a KBI prompt callback fired, which
             * the UI layer maps to the kbi_dialog overlay. */
            if (config->password[0]) {
                auth_rc = libssh2_userauth_password(s->ssh_session,
                                                    config->username,
                                                    config->password);
            } else {
                auth_rc = -1;  /* force KBI path */
            }
            if (auth_rc != 0) {
                auth_rc = libssh2_userauth_keyboard_interactive(
                    s->ssh_session, config->username, &ssh_kbdint_callback);
            }
            break;
        case AUTH_PUBLICKEY: {
            char pub_path[520];
            snprintf(pub_path, sizeof(pub_path), "%s.pub", config->key_path);
            const char *passphrase = config->key_passphrase[0] ? config->key_passphrase : NULL;
            /* Prefer the in-memory PEM when a Smart Vault-stored key is
             * attached: avoids touching the filesystem with decrypted
             * material. Falls back to the on-disk path when no vault key
             * is available. */
            if (config->key_pem) {
                auth_rc = libssh2_userauth_publickey_frommemory(
                    s->ssh_session,
                    config->username, strlen(config->username),
                    NULL, 0,
                    (const char *)config->key_pem, config->key_pem_len,
                    passphrase);
            } else {
                /* The agent's identities first when asked (the SSH form's key
                 * login): the order ssh uses, and what keeps a protected key
                 * that is already loaded from costing a passphrase prompt. */
                if (config->agent_before_key) {
                    LIBSSH2_AGENT *agent = libssh2_agent_init(s->ssh_session);
                    if (agent && libssh2_agent_connect(agent) == 0) {
                        libssh2_agent_list_identities(agent);
                        struct libssh2_agent_publickey *identity = NULL;
                        while (libssh2_agent_get_identity(agent, &identity, identity) == 0) {
                            if (libssh2_agent_userauth(agent, config->username, identity) == 0) {
                                auth_rc = 0;
                                break;
                            }
                        }
                        libssh2_agent_disconnect(agent);
                    }
                    if (agent) libssh2_agent_free(agent);
                }
                if (auth_rc != 0)
                    auth_rc = libssh2_userauth_publickey_fromfile(
                        s->ssh_session, config->username, pub_path, config->key_path, passphrase);
            }

            /* A key that needs a passphrase, and none was given: ask.
             *
             * Whether it needs one is read off the KEY (ssh/key_protection.h),
             * not off the error text: what libssh2 returns for this case is
             * "Callback returned error", which names no passphrase. The
             * error-text substrings serve only a key whose format the header
             * check does not recognise. */
            if (auth_rc != 0 && !passphrase) {
                SshKeyProtection prot = SSH_KEY_UNKNOWN;
                if (config->key_pem) {
                    prot = ssh_key_protection((const char *)config->key_pem, config->key_pem_len);
                } else {
                    char khead[2048];
                    FILE *kf = fopen(config->key_path, "r");
                    if (kf) {
                        size_t kn = fread(khead, 1, sizeof khead, kf);
                        fclose(kf);
                        prot = ssh_key_protection(khead, kn);
                        memset(khead, 0, sizeof khead);
                    }
                }
                bool needs_passphrase = (prot == SSH_KEY_ENCRYPTED);
                if (prot == SSH_KEY_UNKNOWN) {
                    char *errmsg = NULL;
                    libssh2_session_last_error(s->ssh_session, &errmsg, NULL, 0);
                    if (errmsg) {
                        needs_passphrase = (strstr(errmsg, "passphrase") != NULL ||
                                            strstr(errmsg, "decrypt") != NULL ||
                                            strstr(errmsg, "Unable to extract") != NULL ||
                                            strstr(errmsg, "Failed to parse") != NULL);
                    }
                }
                if (needs_passphrase) {
                    /* Signal the UI to show a passphrase prompt */
                    pthread_mutex_lock(&s->status_lock);
                    s->passphrase_pending = true;
                    s->passphrase_supplied = false;
                    s->passphrase_cancelled = false;
                    snprintf(s->passphrase_prompt_path, sizeof(s->passphrase_prompt_path),
                             "%s", config->key_path);
                    pthread_mutex_unlock(&s->status_lock);
                    session_notify_main();

                    /* Block until UI supplies passphrase or cancels */
                    pthread_mutex_lock(&s->status_lock);
                    while (!s->passphrase_supplied && !s->passphrase_cancelled &&
                           !CANCELLED()) {
                        pthread_cond_wait(&s->passphrase_cond, &s->status_lock);
                    }
                    s->passphrase_pending = false;
                    bool cancelled = s->passphrase_cancelled || CANCELLED();
                    pthread_mutex_unlock(&s->status_lock);

                    if (cancelled) FAIL("Authentication cancelled");

                    /* Retry with the supplied passphrase */
                    const char *pp = s->config->key_passphrase[0]
                                     ? s->config->key_passphrase : NULL;
                    if (config->key_pem) {
                        auth_rc = libssh2_userauth_publickey_frommemory(
                            s->ssh_session,
                            config->username, strlen(config->username),
                            NULL, 0,
                            (const char *)config->key_pem,
                            config->key_pem_len, pp);
                    } else {
                        auth_rc = libssh2_userauth_publickey_fromfile(
                            s->ssh_session, config->username, pub_path,
                            config->key_path, pp);
                    }
                }
            }
            auth_rc = ssh_auth_second_step(s, config, auth_rc);
            break;
        }
        case AUTH_AGENT: {
            LIBSSH2_AGENT *agent = libssh2_agent_init(s->ssh_session);
            if (agent && libssh2_agent_connect(agent) == 0) {
                libssh2_agent_list_identities(agent);
                struct libssh2_agent_publickey *identity = NULL;
                while (libssh2_agent_get_identity(agent, &identity, identity) == 0) {
                    if (libssh2_agent_userauth(agent, config->username, identity) == 0) {
                        auth_rc = 0;
                        break;
                    }
                }
                libssh2_agent_disconnect(agent);
            }
            if (agent) libssh2_agent_free(agent);
            auth_rc = ssh_auth_second_step(s, config, auth_rc);

            /* If agent auth failed, show a helpful error about ssh-add */
            if (auth_rc != 0 && config->key_path[0]) {
                char agent_msg[512];
                snprintf(agent_msg, sizeof(agent_msg),
                         "Agent auth failed. Try: ssh-add %s", config->key_path);
                set_error(s, agent_msg);
                return -1;
            }
            break;
        }
        case AUTH_GSSAPI:
            /* Handled by the gssapi_auth path above; shouldn't reach here */
            break;
        }
    }
    if (auth_rc != 0) {
        char *errmsg = NULL;
        libssh2_session_last_error(s->ssh_session, &errmsg, NULL, 0);
        set_error(s, errmsg && errmsg[0] ? errmsg :
                     s->auth_refused[0] ? s->auth_refused : "Authentication failed");
        return -1;
    }
    if (CANCELLED()) FAIL("cancelled");
    ssh_wait_bounded(s->ssh_session);

    /* A file-transfer connection asks for nothing a shell needs (see
     * SSHConfig.sftp_only): straight to SFTP, which it cannot do without. */
    if (config->sftp_only) {
        s->last_keepalive_time = 0;
        s->keepalive_missed = 0;
        goto sftp_init;
    }

    /* ==================================================================
     * Open channel, PTY, shell
     * ================================================================== */

    s->ssh_channel = libssh2_channel_open_session(s->ssh_session);
    if (!s->ssh_channel) FAIL("Failed to open SSH channel");

    if (libssh2_channel_request_pty_ex(s->ssh_channel, "xterm-256color", 14,
                                        NULL, 0, s->cols, s->rows, 0, 0) != 0)
        FAIL("PTY request failed");
    /* The remote now knows this geometry; a later session_resize is owed only
     * when it asks for something else. */
    s->resize_sent_cols = s->resize_want_cols = s->cols;
    s->resize_sent_rows = s->resize_want_rows = s->rows;
    s->resize_inflight = false;

    /* Agent forwarding is an explicit opt-in (ForwardAgent yes), never a
     * side effect of having authenticated through the agent: with it on,
     * root on the remote host can sign with every key the local agent holds.
     * Non-fatal if the server refuses. */
    if (config->forward_agent) ssh_request_agent_forwarding(s);

    /* Request X11 forwarding if configured (after PTY, before shell) */
    if (config->forward_x11) {
        /* Register the X11 open callback. libssh2_session_callback_set2 +
         * libssh2_cb_generic landed in libssh2 1.11.1; older libssh2 (e.g.
         * Ubuntu 24.04's 1.11.0) only has the void*-typed callback_set. */
#if defined(LIBSSH2_VERSION_NUM) && LIBSSH2_VERSION_NUM >= 0x010b01
        libssh2_session_callback_set2(s->ssh_session, LIBSSH2_CALLBACK_X11,
                                       (libssh2_cb_generic *)x11_open_callback);
#else
        libssh2_session_callback_set(s->ssh_session, LIBSSH2_CALLBACK_X11,
                                     (void *)x11_open_callback);
#endif

        /* Generate a random MIT-MAGIC-COOKIE-1 auth cookie (32 hex chars) from
         * a CSPRNG. rand() is unseeded and predictable here, which would let a
         * local attacker forge the cookie and connect to the forwarded X11
         * channel. */
        u8 cookie_rnd[16];
        if (!crypto_random(cookie_rnd, sizeof(cookie_rnd))) {
            fprintf(stderr, "X11 forwarding skipped: secure RNG unavailable\n");
        } else {
            char cookie[33];
            static const char hexd[] = "0123456789abcdef";
            for (int ci = 0; ci < 16; ci++) {
                cookie[ci * 2]     = hexd[(cookie_rnd[ci] >> 4) & 0xf];
                cookie[ci * 2 + 1] = hexd[cookie_rnd[ci] & 0xf];
            }
            cookie[32] = '\0';

            /* Keep it: every X11 channel the server opens is checked against it before
             * it gets anywhere near the local display. */
            memcpy(s->x11_cookie, cookie_rnd, sizeof s->x11_cookie);
            s->x11_cookie_set = true;
            int x11_rc = libssh2_channel_x11_req_ex(s->ssh_channel,
                                                      0,      /* single_connection */
                                                      "MIT-MAGIC-COOKIE-1",
                                                      cookie,
                                                      0);     /* screen_number */
            if (x11_rc != 0) {
                /* X11 forwarding is best-effort -- log but don't fail the session */
                fprintf(stderr, "X11 forwarding request failed (rc=%d)\n", x11_rc);
                s->x11_cookie_set = false;
                secure_zero(s->x11_cookie, sizeof s->x11_cookie);
            }
        }
    }

    /* Smart Vault — inject environment variables before starting the
     * shell. The server must have `AcceptEnv` configured for the var
     * name (or `AcceptEnv *`) or libssh2_channel_setenv returns an
     * error. That is non-fatal: missing env is a misconfig, not a
     * reason to refuse the session. */
    for (i32 ei = 0; config->env && ei < config->env_count; ei++) {
        const SSHEnvVar *ev = &config->env[ei];
        if (!ev->name[0]) continue;
        int erc = libssh2_channel_setenv(s->ssh_channel, ev->name, ev->value);
        if (erc != 0) {
            fprintf(stderr,
                    "vault env: server rejected %s (rc=%d); "
                    "check AcceptEnv in sshd_config\n",
                    ev->name, erc);
        }
    }

    if (!ssh_start_shell(s->ssh_session, s->ssh_channel, config->start_dir))
        FAIL("Shell request failed");

    /* Keepalive is configured at the END of this function, after the last
     * blocking request — see there for why. */
    s->last_keepalive_time = 0;
    s->keepalive_missed = 0;

    /* ==================================================================
     * Remote port forwarding setup from config
     * ================================================================== */
    for (i32 i = 0; i < config->remote_forward_count; i++) {
        const PortForwardSpec *spec = &config->remote_forwards[i];
        int bound_port = 0;
        LIBSSH2_LISTENER *listener = libssh2_channel_forward_listen_ex(
            s->ssh_session,
            /* No address given means the server's loopback, as OpenSSH asks.
             * libssh2 turns NULL into 0.0.0.0, which on a server with
             * GatewayPorts hands the user's local service to its network. */
            spec->bind_host[0] ? spec->bind_host : "localhost",
            spec->bind_port, &bound_port, 1);
        if (listener && s->remote_listener_count < MAX_REMOTE_LISTENERS) {
            if (!s->remote_listeners) {
                s->remote_listeners = calloc(MAX_REMOTE_LISTENERS,
                                             sizeof(*s->remote_listeners));
                if (!s->remote_listeners) continue;
            }
            RemoteForwardListener *l = &s->remote_listeners[s->remote_listener_count++];
            l->listener = listener;
            snprintf(l->bind_host, sizeof(l->bind_host), "%s",
                     spec->bind_host[0] ? spec->bind_host : "0.0.0.0");
            l->bind_port = bound_port;
            snprintf(l->dest_host, sizeof(l->dest_host), "%s", spec->dest_host);
            l->dest_port = spec->dest_port;
            l->active = true;
        }
    }

    /* Pre-init SFTP + resolve remote home while still in blocking mode.
     * Doing this here costs one SFTP handshake + one realpath round-trip on
     * the worker thread, which is already hidden behind the "connecting" UI
     * state. In return, the main thread never has to flip libssh2 to
     * blocking mode to get an SFTP handle, which would stall the first time
     * the sidebar is opened on an SSH tab. SFTP being unavailable is
     * non-fatal: the shell session works regardless — except on a
     * file-transfer connection, which is nothing else. */
sftp_init:
    if (!CANCELLED()) {
        s->sftp_init_tried = true;
        LIBSSH2_SFTP *sftp = libssh2_sftp_init(s->ssh_session);
        if (sftp) {
            s->sftp_session = sftp;
            char rp[1024];
            int n = libssh2_sftp_realpath(sftp, ".", rp, sizeof(rp) - 1);
            if (n > 0) {
                rp[n] = '\0';
                free(s->initial_cwd);
                s->initial_cwd = malloc((usize)n + 1);
                if (s->initial_cwd) memcpy(s->initial_cwd, rp, (usize)n + 1);
            }
        } else if (config->sftp_only) {
            FAIL("The server does not offer SFTP");
        }
    }
    if (config->sftp_only && CANCELLED()) FAIL("cancelled");

    /* Switch to non-blocking — main thread owns libssh2 from here on. */
    libssh2_session_set_blocking(s->ssh_session, 0);

    /* Keepalive, configured only now. While the session is in blocking mode
     * every wait inside libssh2 calls libssh2_keepalive_send itself, and its
     * "last sent" starts at zero — so configured before the forwards are
     * requested, it would put a keepalive@libssh2.org on the wire in the
     * middle of the FIRST tcpip-forward exchange. Its answer (REQUEST_FAILURE:
     * sshd does not know that request) would then be read as the answer to the
     * SECOND tcpip-forward: libssh2 would report failure and no listener would
     * be registered here, while the server had bound the port all the same and
     * refused every connection to it with "Forward not requested". */
    if (s->keepalive_interval > 0) {
        libssh2_keepalive_config(s->ssh_session, 1, (unsigned int)s->keepalive_interval);
    }

    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_CONNECTED;
    pthread_mutex_unlock(&s->status_lock);
    /* A host that asked for ControlMaster offers this connection to the
     * sessions opened after it (a no-op for one that did not). */
    session_pool_register(s);
    return 0;

    #undef CANCELLED
    #undef FAIL
}

static void *ssh_connect_worker(void *arg) {
    Session *s = (Session *)arg;
    /* A reconnect's back-off is waited out HERE, in slices so a close or a
     * newer reconnect is not held up by it. */
    for (i32 waited = 0; waited < s->connect_delay_ms &&
                         !atomic_load_explicit(&s->connect_cancel, memory_order_acquire);
         waited += 50) {
        struct timespec nap = { 0, 50 * 1000 * 1000 };
        nanosleep(&nap, NULL);
    }
    ssh_do_connect(s);
    connect_fds_clear(s);      /* nothing left for a canceller to interrupt */
    session_notify_main();     /* CONNECTED or failed: either way the loop must look */
    return NULL;
}

/* How long one call may wait for the server while a session borrows another's
 * connection. The sequence below holds the connection's io lock, which the
 * other panes only try: a healthy server answers a channel open in
 * milliseconds, and a dead one must not freeze them for the 30 s a connect
 * allows each call. */
#define SSH_SHARED_CALL_TIMEOUT_MS 8000

typedef struct { LIBSSH2_SESSION *ssh; long timeout; } SharedBlocking;

/* Blocking mode and a short bound for the next calls on a shared connection.
 * Caller holds its io lock, and ends with shared_blocking_end before letting go. */
static void shared_blocking_begin(SharedBlocking *b, LIBSSH2_SESSION *ssh) {
    b->ssh = ssh;
    b->timeout = libssh2_session_get_timeout(ssh);
    libssh2_session_set_blocking(ssh, 1);
    libssh2_session_set_timeout(ssh, SSH_SHARED_CALL_TIMEOUT_MS);
}
static void shared_blocking_end(SharedBlocking *b) {
    libssh2_session_set_blocking(b->ssh, 0);
    libssh2_session_set_timeout(b->ssh, b->timeout);
}

/* Close this session's SFTP handle and shell channel on a connection that goes
 * on being used by others: its own, with the pane that is leaving, or the one
 * it borrows. Under the connection's io lock and in blocking mode — a
 * non-blocking close can leave the channel half closed and allocated for as
 * long as the connection lives. When the pane is only leaving (`final` false)
 * and a worker still has a scope open on the connection, its SFTP handle is
 * left to the final teardown: the worker holds a reference and may be using it. */
static void session_close_pane_channels(Session *s, bool final) {
    Session *o = ssh_connection_owner(s);
    pthread_mutex_lock(&o->io_lock);
    LIBSSH2_SESSION *ssh = o->ssh_session;
    if (ssh && (s->sftp_session || s->ssh_channel)) {
        SharedBlocking blk;
        shared_blocking_begin(&blk, ssh);
        if (s->sftp_session &&
            (final || atomic_load_explicit(&o->io_suspend_count, memory_order_acquire) == 0)) {
            if (libssh2_sftp_shutdown(s->sftp_session) == LIBSSH2_ERROR_TIMEOUT)
                libssh2_session_set_timeout(ssh, 50);   /* a silent peer: do not wait the full bound again */
            s->sftp_session = NULL;
        }
        if (s->ssh_channel) {
            if (libssh2_channel_send_eof(s->ssh_channel) == LIBSSH2_ERROR_TIMEOUT)
                libssh2_session_set_timeout(ssh, 50);
            if (libssh2_channel_close(s->ssh_channel) == LIBSSH2_ERROR_TIMEOUT)
                libssh2_session_set_timeout(ssh, 50);
            libssh2_channel_free(s->ssh_channel);
            s->ssh_channel = NULL;
        }
        shared_blocking_end(&blk);
    }
    pthread_mutex_unlock(&o->io_lock);
}

/* Let go of a borrowed connection: forget its pointers (they were the owner's)
 * and give back the reference that kept the owner alive. The caller has closed
 * this session's channel and SFTP handle already (session_close_pane_channels),
 * and holds no io lock: the last reference tears the owner down. `freed_slot`:
 * this session had a channel on the connection, so there is room for another. */
static void session_detach_shared(Session *s, bool freed_slot) {
    Session *o = s->shared_session;
    s->shared_session = NULL;
    s->is_shared_owner = true;
    s->ssh_session = NULL;
    s->sock = -1;
    if (o) {
        if (freed_slot) atomic_store_explicit(&o->pool_full, false, memory_order_release);
        atomic_fetch_sub_explicit(&o->borrowers, 1u, memory_order_acq_rel);
        session_release(o);
    }
}

/* Open a channel + PTY + shell + SFTP on the connection `s->shared_session`
 * owns. Runs on a worker thread. 0: connected. -1: not done, and nothing of this
 * session is left on the owner's connection — the caller connects on its own,
 * which is also what happens when the server's MaxSessions is spent. */
static int ssh_do_reuse_connect(Session *s) {
    Session *owner = s->shared_session;
    if (!owner) return -1;

    /* A blocking lock: the main thread only ever tries this one. */
    pthread_mutex_lock(&owner->io_lock);
    LIBSSH2_SESSION *ssh = owner->ssh_session;
    if (!ssh || atomic_load_explicit(&s->connect_cancel, memory_order_acquire)) {
        pthread_mutex_unlock(&owner->io_lock);
        return -1;
    }
    SharedBlocking blk;
    shared_blocking_begin(&blk, ssh);

    int rc = -1;
    LIBSSH2_CHANNEL *ch = libssh2_channel_open_session(ssh);
    if (!ch) {
        atomic_store_explicit(&owner->pool_full, true, memory_order_release);
        goto done;
    }
    s->ssh_session = ssh;
    s->sock = owner->sock;
    s->ssh_channel = ch;

    if (libssh2_channel_request_pty_ex(ch, "xterm-256color", 14, NULL, 0,
                                       s->cols, s->rows, 0, 0) != 0)
        goto done;
    s->resize_sent_cols = s->resize_want_cols = s->cols;
    s->resize_sent_rows = s->resize_want_rows = s->rows;
    s->resize_inflight = false;
    if (atomic_load_explicit(&s->connect_cancel, memory_order_acquire)) goto done;

    /* sshd takes ONE agent request per connection ("authentication forwarding
     * requested twice") and gives every session started after it the same
     * SSH_AUTH_SOCK; so only the first pane to want it asks. */
    if (s->config && s->config->forward_agent &&
        !atomic_load_explicit(&owner->agent_active, memory_order_acquire))
        ssh_request_agent_forwarding(s);

    if (!ssh_start_shell(ssh, ch, s->config ? s->config->start_dir : NULL)) goto done;

    /* Its own SFTP handle: libssh2 allows several SFTP sub-channels per
     * session, and this one is shut down with the pane, not with the owner's. */
    if (!atomic_load_explicit(&s->connect_cancel, memory_order_acquire)) {
        s->sftp_init_tried = true;
        LIBSSH2_SFTP *sftp = libssh2_sftp_init(ssh);
        if (sftp) {
            s->sftp_session = sftp;
            char rp[1024];
            int n = libssh2_sftp_realpath(sftp, ".", rp, sizeof(rp) - 1);
            if (n > 0) {
                rp[n] = '\0';
                free(s->initial_cwd);
                s->initial_cwd = malloc((usize)n + 1);
                if (s->initial_cwd) memcpy(s->initial_cwd, rp, (usize)n + 1);
            }
        }
    }
    if (atomic_load_explicit(&s->connect_cancel, memory_order_acquire)) goto done;
    rc = 0;

done:
    if (rc != 0) {
        if (s->sftp_session) { libssh2_sftp_shutdown(s->sftp_session); s->sftp_session = NULL; }
        if (s->ssh_channel) { libssh2_channel_free(s->ssh_channel); s->ssh_channel = NULL; }
        s->ssh_session = NULL;
        s->sock = -1;
    }
    shared_blocking_end(&blk);
    pthread_mutex_unlock(&owner->io_lock);
    if (rc == 0) {
        pthread_mutex_lock(&s->status_lock);
        s->status = SESSION_CONNECTED;
        pthread_mutex_unlock(&s->status_lock);
    }
    return rc;
}

static void *ssh_reuse_connect_worker(void *arg) {
    Session *s = (Session *)arg;
    if (ssh_do_reuse_connect(s) != 0 &&
        !atomic_load_explicit(&s->connect_cancel, memory_order_acquire)) {
        /* Sharing is a convenience, never a requirement: the connection could
         * not take another channel (or is gone), so this one is made like any
         * other. It owns that connection and offers it to the next. */
        session_detach_shared(s, false);
        ssh_do_connect(s);
        connect_fds_clear(s);
    }
    session_notify_main();
    return NULL;
}

/* `owner` (or NULL): the session whose connection this one borrows, already
 * RETAINED for it — this takes the reference over on success; on a NULL return
 * the caller still owns it. */
static Session *session_create_ssh_on(const SSHConfig *config, i32 cols, i32 rows,
                                      Session *owner) {
    if (!config || cols <= 0 || cols > (i32)UINT16_MAX ||
        rows <= 0 || rows > (i32)UINT16_MAX) return NULL;
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    atomic_init(&s->ref_count, 1u);
    s->instance_id = session_new_instance_id();
    atomic_init(&s->io_suspend_count, 0u);
    atomic_init(&s->io_wanted, false);
    atomic_init(&s->io_yielding, false);
    s->type = SESSION_SSH;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;
    s->is_shared_owner = true; /* default: owns its own connection */
    s->config = calloc(1, sizeof(SSHConfig));
    if (!s->config) { free(s); return NULL; }
    if (!ssh_config_clone(s->config, config)) {
        free(s->config); free(s); return NULL;
    }
    s->status = SESSION_CONNECTING;
    bool status_ok = pthread_mutex_init(&s->status_lock, NULL) == 0;
    bool pass_ok = status_ok &&
                   pthread_cond_init(&s->passphrase_cond, NULL) == 0;
    bool host_lock_ok = pass_ok &&
                        pthread_mutex_init(&s->hostkey_lock, NULL) == 0;
    bool host_cond_ok = host_lock_ok &&
                        pthread_cond_init(&s->hostkey_cond, NULL) == 0;
    bool kbi_lock_ok = host_cond_ok &&
                       pthread_mutex_init(&s->kbi_lock, NULL) == 0;
    bool kbi_cond_ok = kbi_lock_ok &&
                       pthread_cond_init(&s->kbi_cond, NULL) == 0;
    bool io_lock_ok = kbi_cond_ok &&
                      pthread_mutex_init(&s->io_lock, NULL) == 0;
    bool sftp_lock_ok = io_lock_ok && pthread_mutex_init(&s->sftp_op_lock, NULL) == 0;
    bool sftp_cond_ok = sftp_lock_ok && pthread_cond_init(&s->sftp_op_ready, NULL) == 0;
    if (!sftp_cond_ok) {
        if (sftp_lock_ok) pthread_mutex_destroy(&s->sftp_op_lock);
        if (io_lock_ok) pthread_mutex_destroy(&s->io_lock);
        if (kbi_cond_ok) pthread_cond_destroy(&s->kbi_cond);
        if (kbi_lock_ok) pthread_mutex_destroy(&s->kbi_lock);
        if (host_cond_ok) pthread_cond_destroy(&s->hostkey_cond);
        if (host_lock_ok) pthread_mutex_destroy(&s->hostkey_lock);
        if (pass_ok) pthread_cond_destroy(&s->passphrase_cond);
        if (status_ok) pthread_mutex_destroy(&s->status_lock);
        ssh_config_dispose(s->config);
        free(s->config);
        free(s);
        return NULL;
    }
    s->hostkey_pending = false;
    atomic_init(&s->sftp_waiters, 0u);
    session_io_kick_init(s);
    s->hostkey_accepted = false;
    s->kbi_ready = false;
    s->kbi_num_prompts = 0;
    s->proxy_hop_count = 0;
    s->keepalive_interval = 30;   /* default: 30 seconds */
    s->keepalive_count_max = 3;   /* disconnect after 3 missed */
    s->keepalive_missed = 0;
    s->last_keepalive_time = 0;
    s->remote_listener_count = 0;
    s->remote_fwd_conn_count = 0;
    atomic_init(&s->connect_cancel, false);

    /* Borrowing a connection (ControlMaster): the owner is held by a counted
     * reference for as long as this session uses its libssh2 session, socket
     * and io lock — an uncounted pointer would leave a closed owner's
     * borrowers on freed memory. The borrower's calls go through the owner's
     * io lock (ssh_connection_owner), so the borrower's worker and the
     * owner's main thread never use the session at once, and anything that
     * goes wrong ends with a connection of its own. */
    if (owner) {
        s->shared_session = owner;
        s->is_shared_owner = false;
    }

    /* spawn the blocking connect on a dedicated thread so the UI
     * stays responsive during TCP connect, handshake, and authentication. */
    if (pthread_create(&s->connect_thread, NULL, owner ? ssh_reuse_connect_worker : ssh_connect_worker, s) != 0) {
        if (owner) session_detach_shared(s, false);   /* gives the reference back */
        set_error(s, "Failed to spawn SSH connect thread");
        return s;
    }
    s->connect_thread_active = true;
    return s;
}

Session *session_create_ssh(const SSHConfig *config, i32 cols, i32 rows) {
    /* A host that asks for ControlMaster joins a connection that is already up
     * to the same user@host:port, if there is one (session_pool_take). */
    Session *owner = session_pool_take(config);
    Session *s = session_create_ssh_on(config, cols, rows, owner);
    if (!s && owner) {
        atomic_fetch_sub_explicit(&owner->borrowers, 1u, memory_order_acq_rel);
        session_release(owner);
    }
    return s;
}

/* =========================================================================
 * Mosh session (wraps mosh-client via local PTY)
 * ========================================================================= */

Session *session_create_mosh(const SSHConfig *config, i32 cols, i32 rows) {
    if (!config || cols <= 0 || cols > (i32)UINT16_MAX ||
        rows <= 0 || rows > (i32)UINT16_MAX || !mosh_available()) return NULL;

    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    atomic_init(&s->ref_count, 1u);
    s->instance_id = session_new_instance_id();
    atomic_init(&s->io_suspend_count, 0u);
    atomic_init(&s->io_wanted, false);
    atomic_init(&s->io_yielding, false);
    session_io_kick_init(s);
    s->type = SESSION_MOSH;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;
    s->config = calloc(1, sizeof(SSHConfig));
    if (!s->config) { free(s); return NULL; }
    if (!ssh_config_clone(s->config, config)) {
        free(s->config); free(s); return NULL;
    }
    /* mosh asks for its own secrets in the pane (its ssh bootstrap): nothing
     * on this path reads the ones the form held, and a split of this pane is
     * mosh again. Kept, they sat in memory for the life of the tab. */
    secure_zero(s->config->password,       sizeof s->config->password);
    secure_zero(s->config->key_passphrase, sizeof s->config->key_passphrase);
    secure_zero(s->config->jump_password,  sizeof s->config->jump_password);
    if (pthread_mutex_init(&s->status_lock, NULL) != 0) {
        ssh_config_dispose(s->config);
        free(s->config);
        free(s);
        return NULL;
    }
    atomic_init(&s->connect_cancel, false);

    /* mosh-client is spawned as a child process with a local PTY.
     * The mosh binary handles the SSH bootstrap (key exchange via
     * mosh-server) and the UDP session internally. */
    s->mosh = mosh_create(s->config->hostname, s->config->username,
                          s->config->port, cols, rows);
    if (!s->mosh) {
        set_error(s, "Failed to start mosh-client");
        return s;
    }

    s->status = SESSION_CONNECTED;
    return s;
}

/* =========================================================================
 * Telnet / Serial sessions (single-threaded socket / device fd)
 * ========================================================================= */

Session *session_create_telnet(const char *host, i32 port, i32 cols, i32 rows) {
    if (!host || !host[0] || strnlen(host, 256) == 256 ||
        port < 0 || port > 65535 || cols <= 0 || cols > (i32)UINT16_MAX ||
        rows <= 0 || rows > (i32)UINT16_MAX) return NULL;
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    atomic_init(&s->ref_count, 1u);
    s->instance_id = session_new_instance_id();
    atomic_init(&s->io_suspend_count, 0u);
    atomic_init(&s->io_wanted, false);
    atomic_init(&s->io_yielding, false);
    session_io_kick_init(s);
    s->type = SESSION_TELNET;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;
    /* The connect is only STARTED here; session_status() collects it. The
     * tab opens at once in SESSION_CONNECTING instead of the dialog's key
     * handler sitting in getaddrinfo + connect on the main thread. */
    snprintf(s->error_msg, sizeof(s->error_msg),
             "Telnet: could not connect to %s:%d", host, port > 0 ? port : 23);
    s->telnet = telnet_create(host, port > 0 ? port : 23, cols, rows, session_notify_main);
    if (!s->telnet) {
        s->status = SESSION_ERROR;
        return s;
    }
    s->status = SESSION_CONNECTING;
    return s;
}

Session *session_create_serial(const SerialConfig *cfg, i32 cols, i32 rows) {
    if (!cfg || !cfg->port[0] || !memchr(cfg->port, '\0', sizeof cfg->port) ||
        cols <= 0 || cols > (i32)UINT16_MAX ||
        rows <= 0 || rows > (i32)UINT16_MAX) return NULL;
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    atomic_init(&s->ref_count, 1u);
    s->instance_id = session_new_instance_id();
    atomic_init(&s->io_suspend_count, 0u);
    atomic_init(&s->io_wanted, false);
    atomic_init(&s->io_yielding, false);
    session_io_kick_init(s);
    s->type = SESSION_SERIAL;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;
    s->serial = serial_create(cfg);
    if (!s->serial) {
        s->status = SESSION_ERROR;
        snprintf(s->error_msg, sizeof(s->error_msg),
                 "Serial: could not open %s @ %u", cfg->port, cfg->baud_rate);
        return s;
    }
    s->status = SESSION_CONNECTED;
    return s;
}

/* =========================================================================
 * Destroy
 * ========================================================================= */

static void session_sources_drop(Session *s);
static void session_flush_pending_write(Session *s);

/* Does the pty's reader thread make this session's write / tcgetpgrp /
 * TIOCSWINSZ calls? Yes whenever there is one: all three take the kernel tty
 * lock, which a program page-faulting inside its own tty write can hold for
 * longer than a frame. LIU_PTY_DIRECT=1 puts them back on the calling thread.
 */
static bool local_reader_requests(const Session *s) {
    static i32 direct = -1;
    if (direct < 0) { const char *e = getenv("LIU_PTY_DIRECT"); direct = (e && e[0] == '1') ? 1 : 0; }
    return s->reader != NULL && !direct;
}

static void session_retire_io_watch(Session *s);

static void session_destroy_final(Session *s) {
    if (!s) return;

    if (s->type == SESSION_LOCAL) {
        local_pane_forget(s->instance_id);
        /* Join the gather thread BEFORE the fd it is blocked on is closed:
         * closing an fd out from under a blocking read()/poll() is not a
         * wake-up on macOS, and the number can be recycled underneath it. The
         * ctl pipe breaks the poll unconditionally, so the child need not be
         * stopped first. */
        pty_reader_stop(s->reader);
        s->reader = NULL;
        if (s->pty_master >= 0) {
            platform_unwatch_socket(s->pty_master);
            close(s->pty_master);
        }
        if (s->child_pid > 0) {
            local_child_stop(s->child_pid,s->local_stop_deadline);
            s->child_pid = -1;
        }
    } else if (s->type == SESSION_MOSH) {
        /* Unwatch BEFORE the descriptor closes (libdispatch: the fd must stay
         * valid until the source is cancelled, and the number can be handed
         * to the next open). Idempotent — an ended session already did. */
        session_retire_io_watch(s);
        mosh_destroy(s->mosh);
        s->mosh = NULL;
        pthread_mutex_destroy(&s->status_lock);
    } else if (s->type == SESSION_TELNET) {
        session_retire_io_watch(s);
        telnet_destroy(s->telnet);
        s->telnet = NULL;
    } else if (s->type == SESSION_SERIAL) {
        session_retire_io_watch(s);
        serial_destroy(s->serial);
        s->serial = NULL;
    } else {
        /* Unregister from session pool before teardown */
        session_pool_unregister(s);

        /* Stop StreamLocal forwarding relays */
        session_stop_stream_forwards(s);

        /* Close X11 forwarding relays */
        if (s->x11_relays) {
            for (i32 i = 0; i < s->x11_relay_count; i++) {
                socket_channel_relay_close(&s->x11_relays[i].relay);
            }
        }
        /* shut down the connect worker before tearing libssh2 down. */
        ssh_connect_worker_stop(s);
        /* A session whose connect FAILED is still in libssh2's blocking mode:
         * with the socket shut, the channel close / disconnect below fail at
         * once instead of waiting on a peer. After the join, so the worker is
         * no longer the one writing `sock`. */
        if (s->is_shared_owner && s->sock >= 0) shutdown(s->sock, SHUT_RDWR);
        x11_pending_cleanup(s);
        session_agent_cleanup(s);
        /* Remote forwarding cleanup */
        remote_forward_cleanup_all(s);
        if (s->is_shared_owner && s->ssh_session) {
            forward_cleanup(s->ssh_session);
        }
        /* A borrower's channel is one of many on the owner's connection, which
         * stays up: closed under that connection's lock, not torn down with it. */
        if (!s->is_shared_owner && s->shared_session) session_close_pane_channels(s, true);
        if (s->sftp_session) libssh2_sftp_shutdown(s->sftp_session);
        if (s->ssh_channel) {
            libssh2_channel_send_eof(s->ssh_channel);
            libssh2_channel_close(s->ssh_channel);
            libssh2_channel_free(s->ssh_channel);
        }
        /* Only tear down the SSH session and socket if this session owns
         * them. Borrowed (shared) sessions leave the owner's connection
         * intact. */
        if (s->is_shared_owner) {
            if (s->ssh_session) {
                libssh2_session_disconnect(s->ssh_session, "Client disconnect");
                libssh2_session_free(s->ssh_session);
            }
            if (s->sock >= 0) {
                /* Cancel any dispatch_source_t watching this fd before the
                 * close(), per Apple's libdispatch rules (the source's fd
                 * must remain valid until cancellation completes). */
                platform_unwatch_socket(s->sock);
                close(s->sock);
            }
        }
        /* A borrower gives its owner back; the last reference tears it down. */
        if (!s->is_shared_owner && s->shared_session) session_detach_shared(s, true);
        /* Proxy chain cleanup */
        proxy_hops_cleanup(s);
        pthread_cond_destroy(&s->passphrase_cond);
        pthread_mutex_destroy(&s->status_lock);
        pthread_mutex_destroy(&s->hostkey_lock);
        pthread_cond_destroy(&s->hostkey_cond);
        pthread_mutex_destroy(&s->kbi_lock);
        pthread_cond_destroy(&s->kbi_cond);
        pthread_mutex_destroy(&s->io_lock);
        pthread_mutex_destroy(&s->sftp_op_lock);
        pthread_cond_destroy(&s->sftp_op_ready);
    }
    /* KBI prompt/response heap pools — responses can hold 2FA tokens / OTPs;
     * scrub before free. Prompts are public text (no scrub needed). */
    if (s->kbi_responses) {
        secure_zero(s->kbi_responses,
                    (usize)KBI_MAX_PROMPTS * sizeof(*s->kbi_responses));
        free(s->kbi_responses);
        s->kbi_responses = NULL;
    }
    free(s->kbi_prompts);
    s->kbi_prompts = NULL;
    free(s->initial_cwd);
    s->initial_cwd = NULL;
    free(s->remote_listeners);
    s->remote_listeners = NULL;
    if (s->remote_fwd_conns) {
        /* remote_forward_cleanup_all() already emptied every slot on the SSH
         * teardown path; free defensively so no relay can outlive the table. */
        for (i32 i = 0; i < MAX_REMOTE_FWD_CONNS; i++)
            free(s->remote_fwd_conns[i]);
    }
    free(s->remote_fwd_conns);
    s->remote_fwd_conns = NULL;
    free(s->x11_relays);
    s->x11_relays = NULL;
    free(s->agent_relays);
    s->agent_relays = NULL;
    secure_zero(s->x11_cookie, sizeof s->x11_cookie);
    s->x11_cookie_set = false;
    /* Scrub all credentials + dynamic arrays + key material via the canonical
     * dispose path (secure_zero on every secret), then release the heap shell. */
    if (s->config) {
        ssh_config_dispose(s->config);
        free(s->config);
    }
    free(s->write_buf);
    free(s->inflight_buf);
    session_sources_drop(s);
    if (s->io_kick[0] >= 0) close(s->io_kick[0]);
    if (s->io_kick[1] >= 0) close(s->io_kick[1]);
    free(s);
}

bool session_retain(Session *s) {
    if (!s) return false;
    unsigned count = atomic_load_explicit(&s->ref_count, memory_order_relaxed);
    while (count > 0 && count < UINT_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &s->ref_count, &count, count + 1u,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
    return false;
}

void session_release(Session *s) {
    if (!s) return;
    unsigned count = atomic_load_explicit(&s->ref_count, memory_order_relaxed);
    for (;;) {
        if (count == 0) return;
        if (atomic_compare_exchange_weak_explicit(
                &s->ref_count, &count, count - 1u,
                memory_order_release, memory_order_relaxed)) break;
    }
    if (count == 1u) {
        atomic_thread_fence(memory_order_acquire);
        session_destroy_final(s);
    }
}

/* The owner's pane is leaving, but other sessions still use its connection
 * (ControlMaster): end what belongs to the pane — its forwards, X11, shell
 * channel and SFTP handle — and keep the libssh2 session, the socket and the io
 * lock, which the last borrower's reference tears down. Forwards end with the
 * pane that started them: the relays are polled by their owner's pane. The
 * agent relays stay, they are served by whichever pane polls the connection. */
static void session_retire_pane(Session *s) {
    ssh_connect_worker_stop(s);   /* long finished; this joins it */
    pthread_mutex_lock(&s->io_lock);
    LIBSSH2_SESSION *ssh = s->ssh_session;
    if (ssh) {
        SharedBlocking blk;
        shared_blocking_begin(&blk, ssh);
        session_stop_stream_forwards(s);
        if (s->x11_relays) {
            for (i32 i = 0; i < s->x11_relay_count; i++)
                socket_channel_relay_close(&s->x11_relays[i].relay);
        }
        x11_pending_cleanup(s);
        remote_forward_cleanup_all(s);
        forward_cleanup(ssh);
        shared_blocking_end(&blk);
    }
    pthread_mutex_unlock(&s->io_lock);
    session_close_pane_channels(s, false);
    pthread_mutex_lock(&s->status_lock);
    s->pane_retired = true;
    s->status = SESSION_DISCONNECTED;
    pthread_mutex_unlock(&s->status_lock);
}

void session_destroy(Session *s) {
    if (s && s->type == SESSION_SSH && s->is_shared_owner) {
        /* Out of the pool first: from here no session can start borrowing. */
        session_pool_unregister(s);
        if (atomic_load_explicit(&s->borrowers, memory_order_acquire) > 0)
            session_retire_pane(s);
    }
    session_release(s);
}

bool session_prepare_destroy(Session *s) {
#ifndef PLATFORM_WIN32
    if (!s || s->type!=SESSION_LOCAL || s->child_pid<=0) return false;
    if (s->local_stop_deadline>0) return true;
    if (s->status!=SESSION_CONNECTED || atomic_load_explicit(&s->ref_count,memory_order_acquire)!=1u)
        return false;
    /* Keep the leader unreaped while the queue owns it. An automatic/global
     * reaper would invalidate that PID/PGID reservation, so it opts out. */
    struct sigaction action;
    if (sigaction(SIGCHLD,NULL,&action)!=0 || action.sa_handler!=SIG_DFL ||
        (action.sa_flags&SA_NOCLDWAIT)) return false;
    s->local_stop_deadline=local_child_begin_stop(s->child_pid);
    s->status=SESSION_DISCONNECTED;
    return true;
#else
    (void)s;return false;
#endif
}

/* =========================================================================
 * I/O
 * ========================================================================= */

static void session_service_resize(Session *s);
static Session *session_sftp_lock_owner(Session *s);

/* The mutex that guards the LIBSSH2_SESSION this Session's calls go to: its own,
 * or the owner's for a borrowed connection. Every trylock site resolves here so
 * an alias cannot take its own (free) lock and call into a session someone else
 * holds. Failure is recorded for sftp_io_breathe(). */
static bool session_io_trylock(Session *s) {
    Session *o = session_sftp_lock_owner(s);
    if (pthread_mutex_trylock(&o->io_lock) == 0) return true;
    atomic_store_explicit(&o->io_wanted, true, memory_order_release);
    return false;
}
static void session_io_unlock(Session *s) {
    Session *o = session_sftp_lock_owner(s);
    pthread_mutex_unlock(&o->io_lock);
    if (atomic_load_explicit(&o->io_yielding, memory_order_acquire) && o->io_kick[1] >= 0) {
        u8 b = 1;
        (void)!write(o->io_kick[1], &b, 1);
    }
}

static void session_io_kick_init(Session *s) {
    s->io_kick[0] = s->io_kick[1] = -1;
    int fds[2];
    if (pipe(fds) == 0) {
        fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
        fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        s->io_kick[0] = fds[0];
        s->io_kick[1] = fds[1];
    }
}

/* Bytes the kernel has received on this TCP socket, if the platform can say.
 * macOS: TCP_CONNECTION_INFO. Linux: TCP_INFO's tcpi_bytes_received (4.1+).
 * Anything else answers false and the keepalive falls back to counting sends. */
#ifdef __linux__
/* glibc's netinet/tcp.h exposes only the older TCP_INFO prefix. */
#include <linux/tcp.h>
#else
#include <netinet/tcp.h>
#endif
#ifdef SESSION_RX_BYTES_REAL_UNUSED
__attribute__((unused))
#endif
static bool session_socket_rx_bytes_real(int sock, u64 *out) {
    if (sock < 0 || !out) return false;
#if defined(__APPLE__) && defined(TCP_CONNECTION_INFO)
    struct tcp_connection_info ci;
    socklen_t len = sizeof ci;
    if (getsockopt(sock, IPPROTO_TCP, TCP_CONNECTION_INFO, &ci, &len) != 0) return false;
    *out = (u64)ci.tcpi_rxbytes;
    return true;
#elif defined(__linux__) && defined(TCP_INFO)
    struct tcp_info ti;
    socklen_t len = sizeof ti;
    memset(&ti, 0, sizeof ti);
    if (getsockopt(sock, IPPROTO_TCP, TCP_INFO, &ti, &len) != 0) return false;
    /* tcpi_bytes_received sits past the fields every kernel fills; an older
     * kernel returns a shorter struct and leaves it zero. */
    if (len < offsetof(struct tcp_info, tcpi_bytes_received) + sizeof ti.tcpi_bytes_received) return false;
    *out = (u64)ti.tcpi_bytes_received;
    return true;
#else
    (void)sock;
    return false;
#endif
}
#ifndef SESSION_RX_BYTES_FN
#define SESSION_RX_BYTES_FN session_socket_rx_bytes_real
#endif

/* ---- input sources: one copy of a paste, pulled into the queue in pieces --- */

struct SessionInputSource {
    u8   *data;
    usize len;
    i32   refs;          /* main-thread only, like every write-path field */
    bool  mutable;       /* an owned keystroke run this session may extend */
};

typedef struct SessionSrcRef {
    SessionInputSource *src;
    usize off, end;      /* the slice still to be pulled: [off, end) */
} SessionSrcRef;

/* Chunk moved from the head source into write_buf per pull, and the queue
 * level below which a pull happens. Two chunks of queue is what a paste ever
 * costs on top of its one copy. */
#define SESSION_WRITE_PULL_CHUNK ((usize)(64u * 1024u))
#define SESSION_WRITE_PULL_LOW   ((usize)(64u * 1024u))
/* Bytes written while a source is pending are copied into an owned source
 * behind it. A cap keeps a keystroke storm during a stuck paste bounded; past
 * it the write is refused like the queue's own overflow. */
#define SESSION_SRC_TAIL_LIMIT   ((usize)(1u * 1024u * 1024u))
#define SESSION_SRC_MAX          256u

SessionInputSource *session_input_source_create(const u8 *data, usize len) {
    if (!data && len) return NULL;
    SessionInputSource *src = calloc(1, sizeof *src);
    if (!src) return NULL;
    src->data = malloc(len ? len : 1);
    if (!src->data) { free(src); return NULL; }
    if (len) memcpy(src->data, data, len);
    src->len = len;
    src->refs = 1;
    return src;
}

void session_input_source_release(SessionInputSource *src) {
    if (!src) return;
    if (--src->refs > 0) return;
    free(src->data);
    free(src);
}

static void session_sources_drop(Session *s) {
    if (!s) return;
    for (usize i = 0; i < s->src_count; i++) session_input_source_release(s->srcs[i].src);
    free(s->srcs);
    s->srcs = NULL;
    s->src_count = s->src_cap = 0;
}

static bool session_src_push(Session *s, SessionInputSource *src, usize off, usize end) {
    if (s->src_count >= SESSION_SRC_MAX) return false;
    if (s->src_count == s->src_cap) {
        usize cap = s->src_cap ? s->src_cap * 2 : 8;
        SessionSrcRef *n = realloc(s->srcs, cap * sizeof *n);
        if (!n) return false;
        s->srcs = n;
        s->src_cap = cap;
    }
    src->refs++;
    s->srcs[s->src_count++] = (SessionSrcRef){ src, off, end };
    return true;
}

/* A write that arrives while a source is pending: it goes BEHIND the source,
 * as an owned run this session may extend with the next such write. */
static bool session_src_append_tail(Session *s, const u8 *data, usize len) {
    if (s->src_count > 0) {
        SessionSrcRef *last = &s->srcs[s->src_count - 1];
        SessionInputSource *t = last->src;
        if (t->mutable && t->refs == 1 && last->end == t->len) {
            if (len > SESSION_SRC_TAIL_LIMIT - t->len) return false;
            u8 *d = realloc(t->data, t->len + len);
            if (!d) return false;
            memcpy(d + t->len, data, len);
            t->data = d;
            t->len += len;
            last->end = t->len;
            return true;
        }
    }
    if (len > SESSION_SRC_TAIL_LIMIT) return false;
    SessionInputSource *t = session_input_source_create(data, len);
    if (!t) return false;
    t->mutable = true;
    bool ok = session_src_push(s, t, 0, len);
    session_input_source_release(t);          /* the session's ref is the one that counts */
    return ok;
}

static usize session_src_pending(const Session *s) {
    usize n = 0;
    for (usize i = 0; i < s->src_count; i++) n += s->srcs[i].end - s->srcs[i].off;
    return n;
}

static bool session_queue_write(Session *s, const u8 *data, usize len);
static void session_reply_hold(Session *s);

/* Move the head source into write_buf while the queue is low. Called at the
 * top of every flush, so a draining queue keeps pulling and an idle one
 * holds at most a couple of chunks. */
static void session_src_pull(Session *s) {
    while (s->src_count > 0 && s->write_len < SESSION_WRITE_PULL_LOW) {
        SessionSrcRef *head = &s->srcs[0];
        usize left = head->end - head->off;
        if (left > 0) {
            usize n = left < SESSION_WRITE_PULL_CHUNK ? left : SESSION_WRITE_PULL_CHUNK;
            if (!session_queue_write(s, head->src->data + head->off, n)) break;
            head->off += n;
            if (head->off < head->end) continue;
        }
        session_input_source_release(head->src);
        memmove(s->srcs, s->srcs + 1, (s->src_count - 1) * sizeof *s->srcs);
        s->src_count--;
    }
}

bool session_write_source(Session *s, SessionInputSource *src, usize off, usize len) {
    if (!s || !src || session_status(s) != SESSION_CONNECTED) return false;
    if (off > src->len || len > src->len - off) return false;
    if (len == 0) return true;
    session_reply_hold(s);
    /* Every transport goes through the queue: what a serial line, a pty or a
     * socket does not take now is owed and written on the next service, not
     * reported as accepted and forgotten with the caller's reference. */
    s->input_serial++;
    if (!session_src_push(s, src, off, off + len)) return false;
    session_flush_pending_write(s);
    return true;
}

static bool session_queue_write(Session *s, const u8 *data, usize len) {
    if (!s || !data || len == 0) return true;
    if (len > SIZE_MAX - s->write_len) return false;
    usize need = s->write_len + len;
    if (need > SESSION_WRITE_BUFFER_LIMIT) return false;
    if (need > s->write_cap) {
        usize new_cap = s->write_cap ? s->write_cap : 4096;
        while (new_cap < need) {
            if (new_cap > SIZE_MAX / 2) { new_cap = need; break; }
            new_cap *= 2;
        }
        u8 *new_buf = realloc(s->write_buf, new_cap);
        if (!new_buf) return false;
        s->write_buf = new_buf;
        s->write_cap = new_cap;
    }
    memcpy(s->write_buf + s->write_len, data, len);
    s->write_len += len;
    return true;
}

/* Returns bytes accepted, 0 for "not now" (EAGAIN / lock busy), -1 on a hard
 * error. For SESSION_SSH the caller MUST pass the same (data, len) again after
 * a 0 until a non-zero answer — see inflight_buf. */
static i32 session_try_write_now(Session *s, const u8 *data, usize len) {
    if (!s || !data || len == 0) return 0;
    if (s->type == SESSION_LOCAL) {
        if (local_reader_requests(s)) {
            /* The reader thread does the write(): this only hands it the bytes
             * (pty_reader.h says why the main thread must not call write() on
             * a pty). A short count is the queue being full — "not now" — and
             * the loop is woken when there is room again. */
            isize took = pty_reader_write(s->reader, data, len);
            return took < 0 ? -1 : (i32)took;
        }
        ssize_t n = write(s->pty_master, data, len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            return -1;
        }
        return (i32)n;
    }
    if (s->type == SESSION_SSH) {
        if (!s->ssh_channel) return -1;          /* a file-transfer connection takes no input */
        if (!session_io_trylock(s)) return 0;
        ssize_t n = libssh2_channel_write(s->ssh_channel, (const char *)data, len);
        session_io_unlock(s);
        if (n == LIBSSH2_ERROR_EAGAIN) return 0;
        if (n < 0) { set_error(s, "SSH connection write failed"); return -1; }
        return (i32)n;
    }
    if (s->type == SESSION_TELNET)
        return s->telnet ? telnet_write(s->telnet, data, (i32)len) : -1;
    if (s->type == SESSION_SERIAL)
        return s->serial ? serial_write(s->serial, data, (i32)len) : -1;
    return s->mosh ? mosh_write(s->mosh, data, (i32)len) : -1;
}

/* One chunk of the SSH write queue moved into the in-flight copy. Bounded so a
 * paste storm is sent in pieces the transport can take, and so the copy stays
 * small enough to keep around. */
#define SESSION_SSH_INFLIGHT_CHUNK ((usize)(32u * 1024u))

static bool session_inflight_load(Session *s) {
    if (s->inflight_active || s->write_len == 0) return s->inflight_active;
    usize n = s->write_len < SESSION_SSH_INFLIGHT_CHUNK ? s->write_len
                                                       : SESSION_SSH_INFLIGHT_CHUNK;
    if (s->inflight_cap < n) {
        u8 *nb = realloc(s->inflight_buf, n);
        if (!nb) return false;
        s->inflight_buf = nb;
        s->inflight_cap = n;
    }
    memcpy(s->inflight_buf, s->write_buf, n);
    s->write_len -= n;
    if (s->write_len > 0) memmove(s->write_buf, s->write_buf + n, s->write_len);
    s->inflight_len = n;
    s->inflight_off = 0;
    s->inflight_active = true;
    return true;
}

/* Push the in-flight copy, then further chunks, until libssh2 says EAGAIN or
 * the per-flush limit is reached. Every call into libssh2 for a chunk that
 * answered EAGAIN repeats its exact (pointer, length) — the contract a retry
 * from a moved, grown buffer would break. */
static void session_flush_ssh_write(Session *s) {
    usize sent_this_flush = 0;
    while (sent_this_flush < SESSION_WRITE_FLUSH_LIMIT) {
        if (!session_inflight_load(s)) break;          /* nothing queued, or OOM */
        const u8 *p = s->inflight_buf + s->inflight_off;
        usize left = s->inflight_len - s->inflight_off;
        i32 n = session_try_write_now(s, p, left);
        if (n == 0) break;                              /* EAGAIN or lock busy: same args next time */
        if (n < 0) {
            /* A hard error: the transport is gone and this packet with it.
             * Retrying it forever is what a stuck queue looks like; drop the
             * copy and let the status machinery report the disconnect. */
            s->inflight_active = false;
            s->inflight_len = s->inflight_off = 0;
            break;
        }
        s->inflight_off += (usize)n;
        sent_this_flush += (usize)n;
        if (s->inflight_off >= s->inflight_len) {
            s->inflight_active = false;
            s->inflight_len = s->inflight_off = 0;
        }
        /* A partial acceptance leaves libssh2 idle (it returns what it took),
         * so continuing from the accepted offset is a fresh call, not a
         * changed retry. */
    }
}

static void session_flush_pending_write(Session *s) {
    if (!s || session_status(s) != SESSION_CONNECTED) return;
    session_src_pull(s);
    if (s->type == SESSION_SSH) {
        if (s->write_len == 0 && !s->inflight_active) return;
        session_flush_ssh_write(s);
    } else {
        if (s->write_len == 0) return;
        usize flush_limit = s->write_len < SESSION_WRITE_FLUSH_LIMIT
                          ? s->write_len : SESSION_WRITE_FLUSH_LIMIT;
        usize total = 0;
        /* A pty master takes about a kilobyte per write while the slave's
         * reader is between reads (the tty input queue, TTYHOG), then answers
         * EAGAIN — and if the next chance were the NEXT loop iteration, 4 ms
         * away, a paste would drain at about a kilobyte per iteration however
         * fast the program read it. Waiting a moment for the fd to become
         * writable inside the flush turns that into a few kilobytes per
         * wake-up of the reader, bounded by SESSION_LOCAL_FLUSH_WAIT_MS of
         * main-thread time per flush and only while bytes are queued. */
        f64 wait_until = 0.0;           /* armed on the first EAGAIN */
        while (total < flush_limit) {
            i32 n = session_try_write_now(s, s->write_buf + total, flush_limit - total);
            if (n < 0) break;
            if (n == 0) {
                /* Wait only for a receiver that is DRAINING — one that took
                 * nothing at all this flush is stopped or busy, and 3 ms of
                 * every 4 ms iteration parked on it would be the main thread
                 * paying for a paste nobody is reading. */
                if (s->type != SESSION_LOCAL || total == 0) break;
                /* With a reader thread there is nothing to wait FOR here: it
                 * writes as the program reads, and wakes the loop when its
                 * queue has room. Only the reader-less path still polls. */
                if (local_reader_requests(s)) break;
                f64 now = platform_time_sec();
                if (wait_until == 0.0) wait_until = now + SESSION_LOCAL_FLUSH_WAIT_MS / 1000.0;
                if (now >= wait_until) break;
                /* The budget is wall time, not poll calls: a writable fd
                 * returns in microseconds, and counting each return as a
                 * millisecond capped a flush at three kilobytes. */
                struct pollfd pfd = { s->pty_master, POLLOUT, 0 };
                int pr = poll(&pfd, 1, 1);
                if (pr < 0 && errno != EINTR) break;
                continue;
            }
            total += (usize)n;
        }
        if (total > 0) {
            s->write_len -= total;
            if (s->write_len > 0) memmove(s->write_buf, s->write_buf + total, s->write_len);
        }
    }
    /* Hand the burst capacity back once the queue is empty. write_buf only
     * ever grew (up to SESSION_WRITE_BUFFER_LIMIT = 4 MB on a paste storm) and
     * nothing but session_destroy_final() ever released it, so one big paste
     * pinned megabytes per session for the rest of its life. Anything at or
     * under KB(64) is cheap enough to keep as the steady-state buffer;
     * session_queue_write() re-allocates from NULL (realloc(NULL, n)) on the
     * next spill, so the drop is invisible to callers. */
    if (s->write_len == 0 && s->write_cap > KB(64) && s->src_count == 0) {
        free(s->write_buf);
        s->write_buf = NULL;
        s->write_cap = 0;
    }
}

/* In debug builds, append a hex-escaped trace of PTY traffic to the path in
 * LIU_PTY_LOG. IN is child-to-Liu; OUT is Liu-to-child. */
#ifndef NDEBUG
static void pty_log_bytes(const char *dir, const u8 *data, i32 len) {
    static FILE *f = NULL;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        const char *p = getenv("LIU_PTY_LOG");
        if (p && *p) {
            int fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd >= 0) f = fdopen(fd, "a");
        }
    }
    if (!f || !data || len <= 0) return;
    fprintf(f, "[%s %d] ", dir, len);
    for (i32 i = 0; i < len; i++) {
        u8 c = data[i];
        if (c == 0x1b)                  fputs("\\e", f);
        else if (c >= 0x20 && c < 0x7f) fputc((int)c, f);
        else                            fprintf(f, "\\x%02x", c);
    }
    fputc('\n', f);
    fflush(f);
}
#else
static inline void pty_log_bytes(const char *dir, const u8 *data, i32 len) {
    (void)dir; (void)data; (void)len;
}
#endif

/* Bytes the session has already taken off its descriptor and not yet handed to
 * session_read().
 *
 * For a reader-backed PTY that is the ring. For SSH it is libssh2's own packet
 * queue, and it matters just as much: every libssh2_channel_read() first drains
 * the SOCKET into that queue ("process all pending incoming packets",
 * channel.c), so after one call the fd is empty and silent while megabytes of
 * shell output can still be waiting inside the library. The main loop only
 * polls a session when something announces work, and nothing else announces
 * this: answering 0 here would leave the tail of the output undisplayed, after
 * a drain that stopped on its time or byte budget, until the next key press or
 * mouse move.
 *
 * Telnet, mosh and serial keep 0: their unread bytes stay in a kernel fd, and a
 * level-triggered read watch announces those by itself.
 *
 * Answers 0 when the lock is busy (a transfer worker is inside libssh2 — it
 * tells the loop itself, see sftp_io_breathe) and for a session that is no
 * longer CONNECTED: session_read() refuses those outright, so reporting bytes
 * nobody can take would keep the loop awake for ever. */
static i32 ssh_channel_queued(Session *s) {
    if (session_status(s) != SESSION_CONNECTED || !s->ssh_channel) return 0;
    if (pthread_mutex_trylock(&ssh_connection_owner(s)->io_lock) != 0) return 0;
    unsigned long avail = 0;
    (void)libssh2_channel_window_read_ex(s->ssh_channel, &avail, NULL);
    pthread_mutex_unlock(&ssh_connection_owner(s)->io_lock);
    return avail > (unsigned long)INT32_MAX ? INT32_MAX : (i32)avail;
}

i32 session_read_pending(const Session *s) {
    if (!s) return 0;
    if (s->reader) return pty_reader_pending(s->reader);
    if (s->type != SESSION_SSH) return 0;
    return ssh_channel_queued((Session *)s);   /* the lock is not part of the value */
}

/* Take the main loop's read watch off a session whose fd has nothing left to
 * say but "it is over".
 *
 * The watch is level-triggered, and a socket at EOF or reset — like a pty
 * master whose slave side is gone — is readable for ever. session_read()
 * refuses to read a session that is not CONNECTED, so nothing consumes that
 * readiness: every platform_resume_watches() would re-arm the source, it
 * would fire at once, and each fire would force a full poll of every session
 * in every window. The fd must stay valid until the source is cancelled, and
 * it does: it is closed later, by session_destroy / session_reconnect.
 *
 * Idempotent, main thread only. Not for CONNECTING / AUTHENTICATING: the
 * worker owns the socket then and nothing watches it yet. */
static void session_retire_io_watch(Session *s) {
    if (!s || s->io_watch_retired) return;
    s->io_watch_retired = true;
    if (!session_is_shared_owner(s)) return;  /* a multiplexed sibling does not own the fd */
    int fd = session_io_fd(s);
    if (fd >= 0) platform_unwatch_socket(fd);
}

/* The connection's descriptor can only say "it is over" now, and the session
 * that registered its watch may be a pane that was closed (ControlMaster keeps
 * the connection for the others): take the watch down on its behalf. A session
 * that owns its connection retires its own, in session_retire_io_watch.
 * Main thread only, like the flag. */
static void session_retire_connection_watch(Session *s) {
    Session *o = ssh_connection_owner(s);
    if (!o || o == s || o->io_watch_retired) return;
    o->io_watch_retired = true;
    if (o->sock >= 0) platform_unwatch_socket(o->sock);
}

int session_watch_fd(const Session *s) {
    if (!s || s->io_watch_retired) return -1;
    if (session_status((Session *)s) != SESSION_CONNECTED) return -1;
    if (!session_is_shared_owner(s)) return -1;
    return session_io_fd(s);
}

/* Does a background thread own this session's fd?
 *
 * NOT answerable from session_read_pending: that returns 0 both for a session
 * with no reader and for one whose ring happens to be empty, which is the
 * common case. The two drains behave differently under a budget — see
 * poll_hold_if_saturated — so the question has to be asked directly. */
bool session_has_reader(const Session *s) {
    return s && s->reader != NULL;
}

static i32 session_read_local_fd(Session *s, u8 *buf, i32 cap) {
    ssize_t n = read(s->pty_master, buf, (usize)cap);
    if (n > 0) { pty_log_bytes("IN", buf, (i32)n); return (i32)n; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    session_retire_io_watch(s);
    return -1;
}

i32 session_read(Session *s, u8 *buf, i32 buf_size) {
    /* go through the locked accessor so the worker's release fence is
     * acquired before reading s->ssh_channel / ssh_session. */
    if (!s || !buf || buf_size <= 0) return -1;
    if (s->reader) {
        /* Deliberately AHEAD of the CONNECTED gate: these bytes are already
         * off the pty, and session_is_alive() flips the status to DISCONNECTED
         * the moment it reaps the child. Gating them would drop whatever the
         * ring still held between the last drain and the reap — that is, the
         * tail of every short command's output. */
        if (session_status(s) == SESSION_CONNECTED) session_flush_pending_write(s);
        i32 rn = pty_reader_read(s->reader, buf, buf_size);
        if (rn > 0) pty_log_bytes("IN", buf, rn);
        return rn;
    }
    /* Every -1 below means "this descriptor is finished", and each one takes
     * the read watch down with it (session_retire_io_watch): the modules answer
     * 0, never -1, for "nothing to read right now". */
    SessionStatus st = session_status(s);
    /* A reaped local child may still have bytes in its direct PTY read path.
     * Wake/teardown must feed them before closing the descriptor, just as the
     * reader-backed path above drains its ring after disconnect. No writes
     * are retried once the owner is disconnected. */
    if (st == SESSION_DISCONNECTED && s->type == SESSION_LOCAL && s->pty_master >= 0)
        return session_read_local_fd(s, buf, buf_size);
    if (st != SESSION_CONNECTED) {
        if (st == SESSION_ERROR || st == SESSION_DISCONNECTED) session_retire_io_watch(s);
        return -1;
    }
    session_flush_pending_write(s);
    st = session_status(s);
    if (st != SESSION_CONNECTED) {
        if (st == SESSION_ERROR || st == SESSION_DISCONNECTED) session_retire_io_watch(s);
        return -1;
    }
    if (s->type == SESSION_SSH) session_service_resize(s);

    i32 rc;
    if (s->type == SESSION_LOCAL) {
        return session_read_local_fd(s, buf, buf_size);
    } else if (s->type == SESSION_MOSH) {
        rc = s->mosh ? mosh_read(s->mosh, buf, buf_size) : -1;
    } else if (s->type == SESSION_TELNET) {
        rc = s->telnet ? telnet_read(s->telnet, buf, buf_size) : -1;
    } else if (s->type == SESSION_SERIAL) {
        rc = s->serial ? serial_read(s->serial, buf, buf_size) : -1;
    } else {
        /* Nothing to read on a file-transfer connection, and libssh2 calls a
         * read of no channel a misuse — which would end the session. */
        if (!s->ssh_channel) return 0;
        if (!session_io_trylock(s)) return 0;
        ssize_t n = libssh2_channel_read(s->ssh_channel, (char *)buf, (usize)buf_size);
        session_io_unlock(s);
        if (n == LIBSSH2_ERROR_EAGAIN) return 0;
        /* A reset need not contain SSH_CHANNEL_EOF. Publish the transport
         * failure so the UI stops polling a dead socket as a live session. */
        if (n < 0) {
            set_error(s, "SSH connection read failed");
            session_retire_connection_watch(s);
            rc = -1;
        }
        else rc = (i32)n;
    }
    if (rc < 0) session_retire_io_watch(s);
    return rc;
}

/* Input is about to go to the program: the pty reader may not answer a
 * question on its own until the next parse says nothing is owed
 * (pty_reader.h, session_reply_publish). */
static void session_reply_hold(Session *s) {
    if (s && s->type == SESSION_LOCAL && s->reader) pty_reader_reply_hold(s->reader);
}

u32 session_fast_replies(const Session *s) {
    return (s && s->type == SESSION_LOCAL && s->reader) ? pty_reader_fast_replies(s->reader) : 0u;
}

void session_reply_publish(Session *s, bool ok, i32 row, i32 col) {
    if (!s || s->type != SESSION_LOCAL || !s->reader) return;
    if (session_queued_write_bytes(s) > 0) ok = false;
    pty_reader_publish_reply(s->reader, ok, row, col);
}

i32 session_write(Session *s, const u8 *data, i32 len) {
    if (session_status(s) != SESSION_CONNECTED) return -1;
    if (!data || len <= 0) return 0;
    /* A file-transfer connection has no program to type into. Refused here,
     * before the queue: nothing is owed and the session is not harmed. */
    if (s->type == SESSION_SSH && s->config && s->config->sftp_only) return -1;
    session_reply_hold(s);
    s->input_serial++;
    pty_log_bytes("OUT", data, len);

    /* Serial, Mosh and Telnet are not special here: they go through the
     * queue below, which owes the remainder. Returning the transport's own
     * answer would lose the rest of a paste a device took only part of, and
     * all of it on "0, not now". */
    session_flush_pending_write(s);
    if (session_status(s) != SESSION_CONNECTED) return -1;
    if (s->src_count > 0) {
        /* A paste is still being pulled: these bytes belong after it. */
        return session_src_append_tail(s, data, (usize)len) ? len : -1;
    }
    if (s->type == SESSION_SSH) {
        /* Never from the caller's buffer: an EAGAIN there would require
         * handing libssh2 that same address again, and the caller is about
         * to reuse it. Queue, then flush — the in-flight copy is what
         * libssh2 sees, and it lives until libssh2 is done with it. */
        if (!session_queue_write(s, data, (usize)len)) return -1;
        session_flush_pending_write(s);
        return session_status(s) == SESSION_CONNECTED ? len : -1;
    }
    if (s->write_len > 0) {
        return session_queue_write(s, data, (usize)len) ? len : -1;
    }

    usize total = 0;
    while (total < (usize)len) {
        i32 n = session_try_write_now(s, data + total, (usize)len - total);
        if (n < 0) return total > 0 ? (i32)total : -1;
        if (n == 0) {
            if (!session_queue_write(s, data + total, (usize)len - total)) return -1;
            return len;
        }
        total += (usize)n;
    }
    return (i32)total;
}

/* Bytes this side still has to hand on: what the main loop must come back for. */
usize session_queued_write_bytes(const Session *s) {
    if (!s) return 0;
    return s->write_len + (s->inflight_active ? s->inflight_len - s->inflight_off : 0) +
           session_src_pending(s) +
           /* Telnet queues the escaped bytes inside its own module; they are
            * still owed, and nothing else would bring the loop back for them. */
           (s->type == SESSION_TELNET ? telnet_pending_bytes(s->telnet) : 0);
}

/* Everything typed or pasted that the program has not been given yet — the
 * queue above plus what the pty's reader thread is still writing. */
usize session_pending_write_bytes(const Session *s) {
    if (!s) return 0;
    return session_queued_write_bytes(s) +
           (s->type == SESSION_LOCAL && s->reader ? pty_reader_write_pending(s->reader) : 0);
}

u64 session_input_serial(const Session *s) {
    return s ? s->input_serial : 0;
}

bool session_write_local_batch(Session *s, const u8 *data, i32 len) {
    if (!s || s->type != SESSION_LOCAL || session_status(s) != SESSION_CONNECTED ||
        !data || len <= 0) return false;
    session_reply_hold(s);
    if (s->src_count > 0) {
        if (!session_src_append_tail(s, data, (usize)len)) return false;
    } else if (!session_queue_write(s, data, (usize)len)) return false;
    s->input_serial++;
    pty_log_bytes("OUT", data, len);
    session_flush_pending_write(s);
    return true;
}

void session_set_cell_pixels(Session *s, i32 w, i32 h) {
    if (!s || w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    if (s->cell_px_w == (u16)w && s->cell_px_h == (u16)h) return;
    s->cell_px_w = (u16)w;
    s->cell_px_h = (u16)h;
    if (s->type == SESSION_LOCAL && local_reader_requests(s))
        pty_reader_set_cell_pixels(s->reader, (u16)w, (u16)h);
    /* The size in cells may not change again (a pane created at its final
     * size, a font change): say the same size once more so the pixels reach
     * the tty. */
    if (s->cols > 0 && s->rows > 0) session_resize(s, s->cols, s->rows);
}

void session_resize(Session *s, i32 cols, i32 rows) {
    if (!s || cols <= 0 || rows <= 0 ||
        cols > (i32)UINT16_MAX || rows > (i32)UINT16_MAX) return;
    s->cols = cols;
    s->rows = rows;

    if (s->type == SESSION_LOCAL) {
        /* A program told of the new size asks where its cursor is now; the
         * answer is the parser's until the resized grid has been published. */
        session_reply_hold(s);
        if (local_reader_requests(s)) {
            /* TIOCSWINSZ takes the tty lock too — see pty_reader.h. */
            pty_reader_set_winsize(s->reader, (u16)cols, (u16)rows);
        } else {
            u32 xpix = (u32)cols * s->cell_px_w, ypix = (u32)rows * s->cell_px_h;
            struct winsize ws = {
                .ws_row = (unsigned short)rows,
                .ws_col = (unsigned short)cols,
                .ws_xpixel = (unsigned short)(xpix > 0xffffu ? 0xffffu : xpix),
                .ws_ypixel = (unsigned short)(ypix > 0xffffu ? 0xffffu : ypix),
            };
            ioctl(s->pty_master, TIOCSWINSZ, &ws);
        }
    } else if (s->type == SESSION_MOSH) {
        if (s->mosh) mosh_resize(s->mosh, cols, rows);
    } else if (s->type == SESSION_TELNET) {
        if (s->telnet) telnet_resize(s->telnet, cols, rows);
    } else if (s->type == SESSION_SERIAL) {
        /* fixed-geometry device; nothing to negotiate */
    } else {
        /* Record the wish first: the remote is told from here when the lock is
         * free and libssh2 takes the packet, and from every poll after that
         * until it has (S04). The newest size always wins. */
        s->resize_want_cols = cols;
        s->resize_want_rows = rows;
        session_service_resize(s);
    }
}

/* Tell the remote the pty size it has not heard yet. Called from the resize
 * itself and from every session_read, so a request the lock or the transport
 * turned away is not lost — it is retried until libssh2 reports it sent. */
static void session_service_resize(Session *s) {
    if (!s || s->type != SESSION_SSH) return;
    if (s->resize_want_cols <= 0 || s->resize_want_rows <= 0) return;
    bool owed = s->resize_inflight ||
                s->resize_want_cols != s->resize_sent_cols ||
                s->resize_want_rows != s->resize_sent_rows;
    if (!owed) return;
    /* wait until worker has published the channel. */
    if (session_status(s) != SESSION_CONNECTED || !s->ssh_channel) return;
    if (!session_io_trylock(s)) return;
    /* libssh2 builds the packet from the arguments of the FIRST call and then
     * only finishes sending it: a call while one is half-sent must repeat that
     * call's arguments, whatever is wanted by now. */
    if (!s->resize_inflight) {
        s->resize_inflight_cols = s->resize_want_cols;
        s->resize_inflight_rows = s->resize_want_rows;
    }
    int rc = libssh2_channel_request_pty_size(s->ssh_channel,
                                              s->resize_inflight_cols,
                                              s->resize_inflight_rows);
    session_io_unlock(s);
    if (rc == LIBSSH2_ERROR_EAGAIN) {
        s->resize_inflight = true;
        return;
    }
    s->resize_inflight = false;
    if (rc == 0) {
        s->resize_sent_cols = s->resize_inflight_cols;
        s->resize_sent_rows = s->resize_inflight_rows;
        /* A newer wish arrived while this one was on the wire: the next poll
         * sends it (owed stays true). */
    }
    /* Any other error: leave it owed; the next poll tries again, and a dead
     * transport is reported by the status machinery, not from here. */
}

/* =========================================================================
 * Status
 * ========================================================================= */

SessionStatus session_status(const Session *s) {
    /* SSH session's status is written by a worker during connect.
     * Lock ensures a consistent view when polled from main thread.  Local
     * sessions skip the lock since they have no worker. */
    if (!s) return SESSION_ERROR;
    if (s->type == SESSION_SSH || s->type == SESSION_MOSH) {
        Session *sm = (Session *)s;
        pthread_mutex_lock(&sm->status_lock);
        SessionStatus st = s->status;
        pthread_mutex_unlock(&sm->status_lock);
        return st;
    }
    if (s->type == SESSION_TELNET && s->status == SESSION_CONNECTING) {
        /* Telnet has no worker of its own: the connect runs in core/net.c and
         * whoever asks for the status on the main thread is who notices it has
         * ended. error_msg was written when the attempt began, so it is only
         * dropped on success. */
        Session *sm = (Session *)s;
        i32 r = telnet_connect_poll(sm->telnet);
        if (r > 0) { sm->status = SESSION_CONNECTED; sm->error_msg[0] = '\0'; }
        else if (r < 0) sm->status = SESSION_ERROR;
    }
    return s->status;
}
SessionType   session_type(const Session *s)   { return s ? s->type : SESSION_LOCAL; }
const char   *session_error(const Session *s)  { return s ? s->error_msg : ""; }
int           session_sock(const Session *s)   { return s ? s->sock : -1; }

int session_io_fd(const Session *s) {
    if (!s) return -1;
    if (s->type == SESSION_SSH) return s->sock;
    if (s->type == SESSION_TELNET) return s->telnet ? telnet_fd(s->telnet) : -1;
    if (s->type == SESSION_SERIAL) return s->serial ? serial_fd(s->serial) : -1;
    /* mosh_create wraps mosh-client in a PTY of its OWN: the master lives in
     * the MoshSession, and Session.pty_master stays -1 (answering that here
     * left a Mosh pane with nothing to watch). */
    if (s->type == SESSION_MOSH) return s->mosh ? mosh_fd(s->mosh) : -1;
    /* LOCAL uses a PTY master fd for the read path.
     *
     * A reader thread owns this fd, so the main loop must NOT watch it: the
     * reader keeps it drained, and while a ring is full the pty still has data,
     * so a level-triggered read source would re-fire on every
     * platform_resume_watches() and spin the loop. The wake comes from
     * platform_wake_event_loop() instead. */
    if (s->reader) return -1;
    return s->pty_master;
}

bool session_is_shared_owner(const Session *s) {
    if (!s) return false;
    if (s->type != SESSION_SSH) return true;
    return s->is_shared_owner;
}

/* A file-transfer connection has no shell channel whose EOF would say it is
 * over, and nothing reads its socket while no transfer runs: a server that
 * closed the connection, or the SFTP subsystem alone, went unnoticed until the
 * next listing failed. So the liveness question READS — whatever the server
 * sent is taken into libssh2's packet queue (the SFTP channel's own data stays
 * there for the SFTP call that is waiting for it; a terminal's session_read
 * does the same for its channel), and a transport that answers with an error,
 * or an SFTP channel the server has closed, is a link that went DOWN: the
 * error is set and the link policy retries as it does for a lost terminal. It
 * still answers "alive" either way — "ended" is a shell that exited. Under the
 * io lock it only tries for, as session_read does: a transfer in a call has it
 * and will meet the failure itself. */
static bool session_sftp_only_alive(Session *s) {
    if (!s->sftp_session) return false;
    if (!session_io_trylock(s)) return true;
    LIBSSH2_CHANNEL *ch = libssh2_sftp_get_channel(s->sftp_session);
    int rc = ch ? libssh2_channel_wait_eof(ch) : LIBSSH2_ERROR_EAGAIN;
    session_io_unlock(s);
    if (rc == LIBSSH2_ERROR_EAGAIN) return true;
    set_error(s, rc == 0 ? "The server closed the file transfer" : "SSH connection lost");
    return true;
}

bool session_is_alive(Session *s) {
    if (!s || session_status(s) != SESSION_CONNECTED) return false;
    if (s->type == SESSION_LOCAL) {
        int status;
        pid_t r = waitpid(s->child_pid, &status, WNOHANG);
        if (r == 0) return true; /* still running */
        if (r == s->child_pid) {
            /* Stash the wait status so session_exited() can report the real
             * exit code; this is the only place the child is reaped (no
             * SIGCHLD handler exists by design), so a second waitpid would
             * race to ECHILD. */
            s->last_exit_status = status;
            s->exited = true;
            s->lived_sec = session_mono_now() - s->born_mono;
            s->child_pid = -1;
            s->status = SESSION_DISCONNECTED;
        } else if (r < 0 && errno == ECHILD) {
            s->child_pid = -1;
            s->status = SESSION_DISCONNECTED;
        }
        return false;
    } else if (s->type == SESSION_MOSH) {
        return s->mosh ? mosh_is_alive(s->mosh) : false;
    } else if (s->type == SESSION_TELNET) {
        return s->telnet ? telnet_is_alive(s->telnet) : false;
    } else if (s->type == SESSION_SERIAL) {
        return s->serial ? serial_is_alive(s->serial) : false;
    } else {
        if (s->config && s->config->sftp_only) return session_sftp_only_alive(s);
        if (!s->ssh_channel) return false;
        if (pthread_mutex_trylock(&ssh_connection_owner(s)->io_lock) != 0) return true;
        bool alive = !libssh2_channel_eof(s->ssh_channel);
        pthread_mutex_unlock(&ssh_connection_owner(s)->io_lock);
        return alive;
    }
}

bool session_is_sftp_only(const Session *s) {
    return s && s->type == SESSION_SSH && s->config && s->config->sftp_only;
}

i32 session_child_pid(Session *s) {
    if (!s || s->type != SESSION_LOCAL || s->child_pid <= 0 ||
        (u64)s->child_pid > (u64)INT32_MAX) return -1;
    return (i32)s->child_pid;
}

f64 session_lived_sec(const Session *s) {
    return (s && s->exited) ? s->lived_sec : -1.0;
}

bool session_exit_held(const Session *s) { return s && s->exit_held; }
void session_set_exit_held(Session *s, bool held) { if (s) s->exit_held = held; }

bool session_exited(const Session *s, int *code_out) {
    if (!s || !s->exited) return false;
    if (code_out) {
        int st = s->last_exit_status;
        if (WIFEXITED(st))        *code_out = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) *code_out = -WTERMSIG(st);
        else                      *code_out = -1;
    }
    return true;
}

static bool session_suspend_impl(Session *s, bool wait_stop) {
    if (!s) return false;
    /* A prior incremental caller may only have posted SIGSTOP. Synchronous
     * callers still need the acknowledgement before assuming no more output. */
    if (s->suspended) return !wait_stop || session_wait_suspended(s, 50);

    /* SSH: no OS signal to send -- the remote shell keeps running server-side.
     * The session is only flagged as suspended, so the poll loop drains
     * channel reads (so libssh2 window stays open and keepalives keep
     * flowing). */
    if (s->type == SESSION_SSH) {
        if (session_status(s) != SESSION_CONNECTED) return false;
        s->suspended = true;
        return true;
    }

    pid_t target = -1;
    if (s->type == SESSION_LOCAL && s->child_pid > 0) {
        target = s->child_pid;
    } else if (s->type == SESSION_MOSH && s->mosh) {
        pid_t p = mosh_child_pid(s->mosh);
        if (p > 0) target = p;
    }
    if (target <= 0) return false;
    /* Signal the full process group so child processes stop with the shell.
     * forkpty calls setsid in the child, so immediately after fork
     * getpgid(target) can temporarily return Liu's process group. Never signal
     * that group: doing so would stop the application itself. In this race
     * window, signal only the child; after setsid, signal the child's process
     * group. The idle-sleep scan usually runs after setsid, but an immediate
     * request can arrive during the transition. */
    pid_t pgid = getpgid(target);
    bool signalled = false;
    if (pgid > 0 && pgid != getpgrp()) {
        if (killpg(pgid, SIGSTOP) == 0) signalled = true;
    }
    if (!signalled && kill(target, SIGSTOP) == 0) signalled = true;
    if (!signalled) return false;

    /* For a local shell, do not call the PTY quiescent merely because kill(2)
     * accepted SIGSTOP. waitpid(WUNTRACED) is the kernel acknowledgement that
     * the producer itself can no longer race one final write against the
     * reader's EAGAIN barrier. SIGSTOP is uncatchable, so this has no timing
     * heuristic and normally returns immediately. */
    if (wait_stop && s->type == SESSION_LOCAL && target == s->child_pid) {
        int status = 0;
        pid_t w;
        do { w = waitpid(target, &status, WUNTRACED); }
        while (w < 0 && errno == EINTR);
        if (w != target || !WIFSTOPPED(status)) {
            if (pgid > 0) killpg(pgid, SIGCONT);
            else          kill(target, SIGCONT);
            if (w == target && (WIFEXITED(status) || WIFSIGNALED(status))) {
                s->last_exit_status = status;
                s->exited = true;
                s->lived_sec = session_mono_now() - s->born_mono;
                s->child_pid = -1;
                s->status = SESSION_DISCONNECTED;
            }
            return false;
        }
    }
    s->suspended = true;
    return true;
}

bool session_suspend(Session *s) {
    session_reply_hold(s);
    return session_suspend_impl(s, true);
}

bool session_suspend_begin(Session *s) {
    if (s && s->type == SESSION_LOCAL && !session_local_sleep_is_safe(s)) return false;
    session_reply_hold(s);
    return session_suspend_impl(s, false);
}

typedef struct {
    SessionOutputConsumeFn fn;
    void *ctx;
} SessionDrainSink;

static void session_reader_consume(void *ctx, const u8 *data, usize len) {
    SessionDrainSink *sink = ctx;
    sink->fn(sink->ctx, data, len);
}

bool session_suspend_and_drain(Session *s, SessionOutputConsumeFn consume,
                               void *ctx) {
    if (!s || !consume) return false;
    /* A remote peer does not stop when Liu marks its Session suspended. The
     * caller's policy blocks these already, but keep the invariant local to
     * this API too: success always means a finite producer was stopped. */
    if (s->type != SESSION_LOCAL && s->type != SESSION_MOSH) return false;
    /* Re-check at the mutation boundary, not only when the UI paints/enables
     * the command. A background job may have appeared since that predicate
     * ran, and explicit sleep gets the same safety rule as automatic sleep. */
    if (s->type == SESSION_LOCAL && !session_local_sleep_is_safe(s)) return false;
    if (!session_suspend(s)) return false;

    bool ok = true;
    if (s->reader) {
        SessionDrainSink sink = { consume, ctx };
        ok = pty_reader_drain_barrier(s->reader, session_reader_consume, &sink);
    } else {
        /* Inline-reader path: after the producer is stopped, the first
         * EAGAIN is the same finite barrier the gather thread establishes. */
        u8 buf[16384];
        for (;;) {
            i32 n = session_read(s, buf, (i32)sizeof buf);
            if (n <= 0) break;
            consume(ctx, buf, (usize)n);
        }
    }
    if (!ok) session_resume(s);
    return ok;
}

bool session_resume(Session *s) {
    if (!s) return false;
    if (!s->suspended) return true;

    if (s->type == SESSION_SSH) {
        s->suspended = false;
        return true;
    }

    pid_t target = -1;
    if (s->type == SESSION_LOCAL && s->child_pid > 0) {
        target = s->child_pid;
    } else if (s->type == SESSION_MOSH && s->mosh) {
        pid_t p = mosh_child_pid(s->mosh);
        if (p > 0) target = p;
    }
    if (target <= 0) return false;
    /* Same guard as session_suspend, and it has to be here too even though
     * SIGCONT to Liu's own group is harmless: if suspend fell through to
     * kill(target) then resume must reach the same process, and a killpg to
     * the wrong group would report success while leaving the child stopped. */
    pid_t pgid = getpgid(target);
    bool ok = false;
    if (pgid > 0 && pgid != getpgrp() && killpg(pgid, SIGCONT) == 0) ok = true;
    if (!ok && kill(target, SIGCONT) == 0) ok = true;
    if (ok) s->suspended = false;
    return ok;
}

/* The pty's foreground process group, for everything that asks OFTEN (the
 * agent probe, the title, the file browser's cwd, the keystroke path).
 *
 * tcgetpgrp() on a pty master takes the kernel's tty lock, and the program in
 * the pane can hold that lock across a page fault — on a machine short of
 * memory, a read from swap. Asked from the main thread, that is the whole UI
 * waiting for one page of somebody else's heap. A pane with a reader thread
 * has the answer already (the reader samples it whenever the pty goes quiet,
 * and wakes the loop when it changes), so this only reads an atomic and, at
 * most ten times a second, asks for a fresher one.
 *
 * NOT for the callers that ACT on the answer — a signal (session_send_tstp,
 * session_kill_fg, session_local_sleep_is_safe, the last being what
 * session_suspend re-checks before its SIGSTOP) or text typed into the pane
 * (session_fg_is_shell): a group that is one sample old is the wrong target
 * for a kill and the wrong reader for a command, and those run on a user's
 * action or a 30 s timer, not per frame. They keep the direct call. */
static pid_t local_fg_pgrp(Session *s) {
    if (local_reader_requests(s)) {
        i32 pg = pty_reader_fg_pgid(s->reader);
        f64 now = platform_time_sec();
        if (now - s->fg_asked_at >= 0.1) {
            s->fg_asked_at = now;
            pty_reader_fg_refresh(s->reader);
        }
        if (pg != 0) return (pid_t)pg;
        /* Not sampled yet (the reader has only just started): the direct call,
         * this once. */
    }
    return tcgetpgrp(s->pty_master);
}

bool session_send_tstp(Session *s) {
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED)
        return false;

    if (s->pty_master < 0) return false;
    pid_t pgid = tcgetpgrp(s->pty_master);
    if (pgid <= 0) return false;
    return killpg(pgid, SIGTSTP) == 0;
}

bool session_is_suspended(const Session *s) {
    return s ? s->suspended : false;
}

/* "Will this pid produce another byte?" Read-only — see the header for why this
 * is not waitpid(). THREE answers, because two of them are not the same thing:
 *
 *    1  no. Stopped, a zombie, or no longer present.
 *    0  yes. Still running.
 *   -1  cannot tell: the query itself failed.
 *
 * The caller treats -1 like 0 and eventually vetoes the sleep, which is the
 * same direction sleep_pane_is_idle() takes for a foreground group it cannot
 * read, and for the same reason: being wrong loses output the user cannot get
 * back, being conservative costs one tab's memory.
 *
 * A zombie counts as quiescent. It has exited, so it cannot write again — and
 * answering "still running" there would make a tab whose command just finished
 * burn the whole stop budget and then refuse to sleep. */
static int pid_quiescent(pid_t pid) {
    if (pid <= 0) return 1;
#ifdef PLATFORM_MACOS
    struct kinfo_proc kp;
    usize len = sizeof kp;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0) return errno == ESRCH ? 1 : -1;
    if (len == 0) return 1;                 /* how macOS reports "no such pid" */
    return (kp.kp_proc.p_stat == SSTOP || kp.kp_proc.p_stat == SZOMB) ? 1 : 0;
#else
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 1 : -1;
    char line[512];
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (!got) return -1;
    /* `pid (comm) S ...`, and comm can itself contain ") ", so the state
     * character is the one two past the LAST ')'. */
    char *rp = strrchr(line, ')');
    if (!rp || !rp[1] || !rp[2]) return -1;
    char st = rp[2];
    return (st == 'T' || st == 't' || st == 'Z' || st == 'X' || st == 'x') ? 1 : 0;
#endif
}

bool session_wait_suspended(Session *s, i32 timeout_ms) {
    if (!s) return false;
    /* Nothing of ours to observe: MOSH stops mosh_client, whose pid this
     * session does not own, and the remote kinds have no local child. Both keep
     * exactly the behaviour they had before this function existed. */
    if (s->type != SESSION_LOCAL || s->child_pid <= 0) return true;

    /* The state query is the correctness mechanism; the sleep between two of
     * them is only backoff, starting well below the scheduling latency being
     * waited on. The budget is charged against a REAL clock rather than against
     * the nanosleep durations asked for — those undercount both the query and
     * nanosleep's own overshoot, which is exactly when the budget matters. */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long step_us = 100;
    for (;;) {
        if (pid_quiescent((pid_t)s->child_pid) == 1) return true;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        f64 spent_ms = (f64)(now.tv_sec - t0.tv_sec) * 1000.0 +
                       (f64)(now.tv_nsec - t0.tv_nsec) / 1.0e6;
        if (spent_ms >= (f64)timeout_ms) return false;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = step_us * 1000 };
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
        if (step_us < 2000) step_us *= 2;
    }
}

u32 session_quiesce_begin(Session *s) {
    return (s && s->reader) ? pty_reader_quiesce_request(s->reader) : 0;
}

bool session_quiesce_wait(Session *s, u32 token, i32 timeout_ms) {
    if (!s || !s->reader) return true;
    return pty_reader_quiesce_wait(s->reader, token, timeout_ms);
}

bool session_fg_is_shell(Session *s) {
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED) return true;
    if (s->pty_master < 0 || s->child_pid <= 0) return true;
    /* The direct call, on purpose: every caller ACTS on the answer (kills the
     * foreground job, types a command into the pane) — see local_fg_pgrp. */
    pid_t fg = tcgetpgrp(s->pty_master);
    if (fg <= 0) return true;
    pid_t shell_pgid = getpgid(s->child_pid);
    if (shell_pgid <= 0) return true;
    return fg == shell_pgid;
}

u32 session_fg_probe_begin(Session *s) {
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED ||
        s->pty_master < 0 || s->child_pid <= 0 || !s->reader) return 0;
    return pty_reader_fg_request(s->reader);
}

SessionFgProbeResult session_fg_probe_poll(Session *s, u32 token) {
    if (!s || s->status != SESSION_CONNECTED) return SESSION_FG_PROBE_UNAVAILABLE;
    /* Remote protocols do not participate in local job-control signaling. */
    if (s->type != SESSION_LOCAL) return SESSION_FG_PROBE_SHELL;
    if (s->pty_master < 0 || s->child_pid <= 0 || !s->reader || !token)
        return SESSION_FG_PROBE_UNAVAILABLE;
    i32 foreground = 0;
    if (!pty_reader_fg_result(s->reader, token, &foreground)) return SESSION_FG_PROBE_PENDING;
    pid_t shell = getpgid(s->child_pid);
    if (foreground <= 0 || shell <= 0) return SESSION_FG_PROBE_UNAVAILABLE;
    return foreground == (i32)shell ? SESSION_FG_PROBE_SHELL : SESSION_FG_PROBE_CHILD;
}

#ifdef PLATFORM_MACOS
static bool local_tty_groups_only_shell(Session *s, pid_t shell_pgid) {
    struct proc_bsdinfo shell;
    if (proc_pidinfo(s->child_pid, PROC_PIDTBSDINFO, 0, &shell,
                     sizeof shell) != (int)sizeof shell) return false;
    if (shell.e_tdev == 0 || shell.e_tdev == UINT_MAX) return false;

    i32 bytes = proc_listpids(PROC_TTY_ONLY, shell.e_tdev, NULL, 0);
    if (bytes <= 0) return false;
    pid_t *pids = malloc((usize)bytes);
    if (!pids) return false;
    i32 got = proc_listpids(PROC_TTY_ONLY, shell.e_tdev, pids, bytes);
    if (got <= 0) { free(pids); return false; }

    bool safe = true;
    bool saw_shell = false;
    i32 count = got / (i32)sizeof(pid_t);
    for (i32 i = 0; i < count; i++) {
        pid_t pid = pids[i];
        if (pid <= 0) continue;
        struct proc_bsdinfo bi;
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi,
                         sizeof bi) != (int)sizeof bi) {
            /* A process that exited between the list and lookup is harmless;
             * a still-live process that cannot be classified makes the answer
             * no. */
            if (kill(pid, 0) == 0 || errno == EPERM) { safe = false; break; }
            continue;
        }
        if (bi.e_tdev != shell.e_tdev || bi.pbi_status == SZOMB) continue;
        if (pid == s->child_pid) saw_shell = true;
        if ((pid_t)bi.pbi_pgid != shell_pgid) { safe = false; break; }
    }
    free(pids);
    return safe && saw_shell;
}
#else
static bool proc_stat_job_fields(pid_t pid, char *state, pid_t *pgid,
                                 pid_t *sid, long *tty_nr) {
    char path[64], line[4096];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok) return false;
    char *end = strrchr(line, ')');
    if (!end || end[1] != ' ') return false;
    long ppid = 0, group = 0, session = 0, tty = 0, tpgid = 0;
    if (sscanf(end + 2, "%c %ld %ld %ld %ld %ld", state, &ppid, &group,
               &session, &tty, &tpgid) != 6) return false;
    (void)ppid; (void)tpgid;
    *pgid = (pid_t)group;
    *sid = (pid_t)session;
    *tty_nr = tty;
    return true;
}

static bool local_tty_groups_only_shell(Session *s, pid_t shell_pgid) {
    pid_t shell_sid = getsid(s->child_pid);
    if (shell_sid <= 0) return false;
    DIR *dir = opendir("/proc");
    if (!dir) return false;
    bool safe = true;
    bool saw_shell = false;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        char *tail = NULL;
        long raw = strtol(de->d_name, &tail, 10);
        if (!de->d_name[0] || !tail || *tail || raw <= 0 || raw > INT_MAX)
            continue;
        pid_t pid = (pid_t)raw, pgid = -1, sid = -1;
        char state = 0;
        long tty = 0;
        if (!proc_stat_job_fields(pid, &state, &pgid, &sid, &tty)) {
            if (pid == s->child_pid) { safe = false; break; }
            continue;
        }
        if (pid == s->child_pid) saw_shell = true;
        if (state == 'Z' || sid != shell_sid || tty == 0) continue;
        if (pgid != shell_pgid) { safe = false; break; }
    }
    closedir(dir);
    return safe && saw_shell;
}
#endif

bool session_local_sleep_is_safe(Session *s) {
    if (!s || s->type != SESSION_LOCAL) return true;
    if (s->status != SESSION_CONNECTED || s->child_pid <= 0 ||
        s->pty_master < 0) return false;
    pid_t shell_pgid = getpgid(s->child_pid);
    pid_t fg_pgid = tcgetpgrp(s->pty_master);
    if (shell_pgid <= 0 || fg_pgid <= 0 || fg_pgid != shell_pgid) return false;
    return local_tty_groups_only_shell(s, shell_pgid);
}

bool session_kill_fg(Session *s, int sig) {
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED) return false;
    if (s->pty_master < 0 || s->child_pid <= 0) return false;
    pid_t fg = tcgetpgrp(s->pty_master);
    if (fg <= 0) return false;
    pid_t shell_pgid = getpgid(s->child_pid);
    if (shell_pgid > 0 && fg == shell_pgid) return false;
    return killpg(fg, sig) == 0;
}

bool session_password_prompt(Session *s) {
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED || s->pty_master < 0) return false;
    if (local_reader_requests(s)) {
        /* Keep the sample fresh while somebody is asking (the foreground
         * group's own 100 ms rhythm). */
        f64 now = platform_time_sec();
        if (now - s->fg_asked_at >= 0.1) {
            s->fg_asked_at = now;
            pty_reader_fg_refresh(s->reader);
        }
        return pty_reader_password_mode(s->reader);
    }
    /* No reader thread (LIU_PTY_THREAD=0 / LIU_PTY_DIRECT=1): the direct call. */
    struct termios tio;
    return tcgetattr(s->pty_master, &tio) == 0 && !(tio.c_lflag & ECHO) && (tio.c_lflag & ICANON);
}

i32 session_shell_pgid(Session *s) {
    if (!s || s->type != SESSION_LOCAL || s->child_pid <= 0) return -1;
    pid_t g = getpgid((pid_t)s->child_pid);
    return g > 0 ? (i32)g : -1;
}

i32 session_fg_pid(Session *s) {
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED) return -1;
    if (s->pty_master < 0) return -1;
    pid_t fg = local_fg_pgrp(s);
    return fg > 0 ? (i32)fg : -1;
}

/* ---- background jobs behind the prompt ----------------------------------
 *
 * session_fg_pid() vs session_shell_pgid() answers "does a job own the
 * terminal". It does NOT answer "is a job running", because a shell that has
 * returned to its prompt owns the terminal again while `npm run dev &` is
 * still going. session_suspend() signals the SHELL's process group and an
 * interactive shell puts every job in its own group, so that job is not
 * stopped by a tab sleep — it keeps writing into a pty nobody drains and
 * wedges in write() once the reader ring and the kernel tty queue are full.
 *
 * The set that matters is "everything alive on this pane's controlling
 * terminal, outside the shell's process group". The two platforms name it
 * differently and both are exact for a pty session:
 *
 *   macOS  the child's own e_tdev IS the pane's controlling terminal, and
 *          KERN_PROC_TTY filters on it inside the kernel, so the result is the
 *          handful of processes on this tty rather than the whole table. Read
 *          off the CHILD rather than off the master fd, because session_io_fd()
 *          returns -1 once a reader thread owns it.
 *
 *   Linux  forkpty()'s child calls setsid(), so the child's pid IS the session
 *          id and every job the shell starts inherits it. /proc/<pid>/stat
 *          field 6 is that session id.
 *
 * Both exclude a process that called setsid() itself — on macOS it drops the
 * controlling terminal, on Linux it leaves the session — which is correct:
 * that is what a daemon does, and it is no longer the shell's job.
 *
 * STOPPED and ZOMBIE processes are not counted. Neither can write another byte
 * while the tab sleeps (nothing resumes them), so counting them would refuse
 * sleep forever for a tab whose last `^Z` is still in the job table.
 */
i32 session_background_jobs(Session *s) {
    if (!s || s->type != SESSION_LOCAL) return 0;   /* nothing local to wedge */
    pid_t child = (pid_t)s->child_pid;
    if (child <= 0) return 0;
    pid_t shell_pgid = getpgid(child);
    if (shell_pgid <= 0) return 0;                  /* the shell itself is gone */

#ifdef PLATFORM_MACOS
    struct proc_bsdinfo bi;
    if (proc_pidinfo(child, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi)
        return -1;
    dev_t tdev = (dev_t)bi.e_tdev;
    if ((int)bi.e_tdev == -1) return 0;             /* no controlling terminal */

    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_TTY, (int)tdev };
    /* Size probe, then fetch with headroom: the table can grow between the two
     * calls and a short buffer answers ENOMEM. Bounded retries rather than a
     * loop, so a machine forking hard cannot hold the main thread here. */
    struct kinfo_proc *kp = NULL;
    usize sz = 0;
    i32 n = -1;
    for (i32 attempt = 0; attempt < 3; attempt++) {
        usize want = 0;
        if (sysctl(mib, 4, NULL, &want, NULL, 0) != 0) { free(kp); return -1; }
        if (want == 0) { free(kp); return 0; }
        want += want / 4 + 8 * sizeof *kp;
        if (want > sz) {
            struct kinfo_proc *nb = (struct kinfo_proc *)realloc(kp, want);
            if (!nb) { free(kp); return -1; }
            kp = nb;
            sz = want;
        }
        usize got = sz;
        if (sysctl(mib, 4, kp, &got, NULL, 0) == 0) { n = (i32)(got / sizeof *kp); break; }
        if (errno != ENOMEM) { free(kp); return -1; }
    }
    if (n < 0) { free(kp); return -1; }

    i32 jobs = 0;
    for (i32 i = 0; i < n; i++) {
        pid_t pid = kp[i].kp_proc.p_pid;
        if (pid <= 0 || pid == child) continue;
        if (kp[i].kp_eproc.e_pgid == shell_pgid) continue;
        char st = kp[i].kp_proc.p_stat;
        if (st == SZOMB || st == SSTOP) continue;
        jobs++;
    }
    free(kp);
    return jobs;
#else
    /* The child is the session leader (forkpty -> login_tty -> setsid). */
    DIR *d = opendir("/proc");
    if (!d) return -1;
    i32 jobs = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char *end = NULL;
        long pid = strtol(e->d_name, &end, 10);
        if (!end || *end || pid <= 0 || (pid_t)pid == child) continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/%ld/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;               /* exited between readdir and open */
        char line[512];
        char *got = fgets(line, sizeof line, f);
        fclose(f);
        if (!got) continue;
        /* `pid (comm) S ppid pgrp session ...`, and comm can contain ") ", so
         * everything after the LAST ')' is the fixed-position part. */
        char *rp = strrchr(line, ')');
        if (!rp || !rp[1]) continue;
        char st = 0; int ppid = 0, pgrp = 0, sess = 0;
        if (sscanf(rp + 1, " %c %d %d %d", &st, &ppid, &pgrp, &sess) != 4) continue;
        if ((pid_t)sess != child) continue;
        if ((pid_t)pgrp == shell_pgid) continue;
        if (st == 'Z' || st == 'X' || st == 'x' || st == 'T' || st == 't') continue;
        jobs++;
    }
    closedir(d);
    return jobs;
#endif
}

/* Drop the foreground-process TTL cache so the next session_fg_process()
 * scans live. Used at close time, where a 0.6 s stale window could miss a
 * just-launched agent (or linger on one that just exited). */
void session_invalidate_fg_cache(Session *s) {
    if (!s) return;
    s->fg_cache_at = 0.0;
    /* Also drop any accumulated backoff: callers invalidate precisely because
     * they need a fresh, responsive answer. */
    s->fg_cache_ttl = SESSION_FG_TTL_BASE;
}

const char *session_fg_process_cached(Session *s) {
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED ||
        s->fg_cache_at <= 0 || !s->fg_cache[0]) return "";
    pid_t fg = local_fg_pgrp(s);
    if (fg <= 0 || fg != s->fg_cache_pgid) return "";
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    f64 now = (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
    f64 ttl = s->fg_cache_ttl > 0 ? s->fg_cache_ttl : SESSION_FG_TTL_BASE;
    return now - s->fg_cache_at < ttl ? s->fg_cache : "";
}

const char *session_fg_process(Session *s) {
    static char empty[1] = {0};
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED) return empty;

    pid_t fg = local_fg_pgrp(s);   /* an atomic read; see local_fg_pgrp */

    /* TTL cache: at a shell prompt this fires on every keystroke (autosuggest);
     * the macOS lookup below scans every pid on the system. Re-scan only when
     * the fg process group changes (a command started/exited) or the TTL
     * lapses, so steady typing costs one ioctl instead of a full pid sweep.
     *
     * The TTL is adaptive. A pane sitting at a shell prompt asks the same
     * question every 0.6 s — sweep 600+ pids, find no agent, repeat — and with
     * a wall of tabs the poll pays that per pane. Each fruitless scan of an
     * UNCHANGED process group doubles the interval up to SESSION_FG_TTL_MAX;
     * anything that could introduce an agent (running a command, the agent
     * exiting back to the prompt) moves the fg pgid, which resets the backoff
     * below, so a launch is still picked up within one base TTL. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    f64 now = (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
    f64 ttl = s->fg_cache_ttl > 0.0 ? s->fg_cache_ttl : SESSION_FG_TTL_BASE;
    if (s->fg_cache_at > 0.0 && s->fg_cache_pgid == fg && now - s->fg_cache_at < ttl) {
        return s->fg_cache;
    }
    /* A different process group is a different question — start over at the
     * responsive rate. */
    bool same_pgid = (s->fg_cache_at > 0.0 && s->fg_cache_pgid == fg);
    if (!same_pgid) s->fg_cache_ttl = SESSION_FG_TTL_BASE;
    bool found_agent = false;

    s->fg_cache[0] = '\0';
#ifdef PLATFORM_MACOS
    if (fg > 0) {
        /* Fast path: the foreground pgrp leader's pid IS the pgid, so one
         * proc_pidinfo identifies it without session_find_interesting_pgrp_
         * process()'s proc_listpids(PROC_ALL_PIDS) sweep over every process on
         * the system — a recurring main-thread stall (re-armed each time the
         * 0.6s TTL lapses while frames render). A native agent (grok, a bun-
         * compiled claude, …) is the leader and gets resolved here directly.
         * Only a plain shell/wrapper leader — which may hide the real agent in
         * a child (node/npm-launched CLIs) — still needs the group sweep. */
        struct proc_bsdinfo bi;
        if (proc_pidinfo(fg, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) == (int)sizeof bi
            && bi.pbi_name[0]) {
            char picked[64] = {0};
            if (session_proc_name_is_agent(bi.pbi_name)) {
                snprintf(s->fg_cache, sizeof s->fg_cache, "%s", bi.pbi_name);
                found_agent = true;
            } else if (session_proc_name_versioned_agent(bi.pbi_name,
                                                         picked, sizeof picked)) {
                snprintf(s->fg_cache, sizeof s->fg_cache, "%s", picked);
                found_agent = true;
            } else if (!session_proc_name_is_wrapper(bi.pbi_name) &&
                       session_proc_path_agent(fg, picked, sizeof picked)) {
                snprintf(s->fg_cache, sizeof s->fg_cache, "%s", picked);
                found_agent = true;
            } else if (session_proc_argv_agent(fg, picked, sizeof picked)) {
                /* Script CLIs whose leader IS the interpreter — and the ones
                 * that renamed themselves, whose title only survives in argv
                 * (Kimi Code, Command Code). */
                snprintf(s->fg_cache, sizeof s->fg_cache, "%s", picked);
                found_agent = true;
            }
        }
        if (s->fg_cache[0] == '\0') {
            /* Leader is a shell/wrapper (or unidentified) — scan the group for a
             * child agent, keeping the leader-name fallback so shell tabs still
             * get a title. */
            char leader[256] = {0};
            proc_name(fg, leader, sizeof leader);
            (void)session_find_interesting_pgrp_process(fg, leader,
                                                        s->fg_cache, sizeof s->fg_cache,
                                                        &found_agent);
        }
    }
#elif defined(PLATFORM_LINUX)
    if (fg > 0) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/comm", fg);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(s->fg_cache, sizeof s->fg_cache, f)) {
                char *nl = strchr(s->fg_cache, '\n');
                if (nl) *nl = '\0';
            }
            fclose(f);
        }
    }
#endif
#ifdef PLATFORM_MACOS
    /* Backoff is a macOS concern: only that path can fall into the full-system
     * pid sweep. Linux reads one /proc/<pid>/comm — nothing worth deferring.
     *
     * An agent answer is worth re-checking often: it can exit within its own
     * process group (the pgid alone would not show it). A no-agent answer for a
     * group already scanned is the idle-prompt steady state — back off. */
    if (found_agent) {
        s->fg_cache_ttl = SESSION_FG_TTL_BASE;
    } else if (same_pgid) {
        f64 next = (s->fg_cache_ttl > 0.0 ? s->fg_cache_ttl : SESSION_FG_TTL_BASE) * 2.0;
        s->fg_cache_ttl = next > SESSION_FG_TTL_MAX ? SESSION_FG_TTL_MAX : next;
    } else {
        s->fg_cache_ttl = SESSION_FG_TTL_BASE;
    }
#else
    (void)found_agent; (void)same_pgid;
    s->fg_cache_ttl = SESSION_FG_TTL_BASE;
#endif
    s->fg_cache_at   = now;
    s->fg_cache_pgid = fg;
    return s->fg_cache;
}

const char *session_local_cwd(Session *s) {
    static char buf[1024];
    buf[0] = '\0';
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED) return buf;

    /* Nothing to report until the child is the SHELL rather than a copy of
     * Liu. forkpty() hands back a pid immediately, but the child chdirs and
     * execs only afterwards, so for a couple of milliseconds every probe below
     * — whichever pid it picks — answers with the cwd the child inherited from
     * this process. Launched from Finder or the Dock that is "/", and the file
     * browser, which follows this value, would walk to the disk root.
     * ssh/local_cwd_ready.h says why exec is the right edge to wait for. */
    if (!s->local_child_execd) {
        if (!liu_child_has_execd(s->child_pid)) return buf;
        s->local_child_execd = true;
    }

    /* Prefer the PTY foreground process group leader — that's the active
     * shell/editor/repl in this tab. Fall back to the shell child PID if the
     * pgrp lookup fails (e.g., PTY just spawned, no fg pgrp set yet). */
    pid_t target = local_fg_pgrp(s);
    if (target <= 0) target = s->child_pid;
    if (target <= 0) return buf;

#ifdef PLATFORM_MACOS
    struct proc_vnodepathinfo vpi;
    int n = proc_pidinfo(target, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi);
    if (n == (int)sizeof vpi && vpi.pvi_cdir.vip_path[0]) {
        snprintf(buf, sizeof buf, "%s", vpi.pvi_cdir.vip_path);
    }
#elif defined(PLATFORM_LINUX)
    char link[64];
    snprintf(link, sizeof link, "/proc/%d/cwd", (int)target);
    ssize_t n = readlink(link, buf, sizeof buf - 1);
    if (n > 0) buf[n] = '\0';
    else       buf[0] = '\0';
#else
    (void)target;
#endif
    return buf;
}

void *session_get_sftp(Session *s) {
    if (!s || s->type != SESSION_SSH) return NULL;
    if (session_status(s) != SESSION_CONNECTED) return NULL;
    return s->sftp_session;
}

void *session_sftp_handle(const Session *s) {
    if (!s || s->type != SESSION_SSH) return NULL;
    if (session_status(s) != SESSION_CONNECTED) return NULL;
    return s->sftp_session;
}

const char *session_initial_cwd(const Session *s) {
    if (!s || s->type != SESSION_SSH) return "";
    if (session_status(s) != SESSION_CONNECTED) return "";
    return s->initial_cwd ? s->initial_cwd : "";
}

/* The LIBSSH2_SESSION * to flip to blocking mode for an SFTP call on
 * this Session: its own if owner, or the shared owner's. Returns NULL if
 * the session isn't usable for SFTP right now. */
static LIBSSH2_SESSION *session_libssh2_for_sftp(Session *s) {
    if (!s || s->type != SESSION_SSH) return NULL;
    if (session_status(s) != SESSION_CONNECTED) return NULL;
    if (!s->sftp_session) return NULL;
    if (!s->is_shared_owner && s->shared_session)
        return s->shared_session->ssh_session;
    return s->ssh_session;
}

/* The Session whose io_lock guards the LIBSSH2_SESSION an SFTP call will use,
 * and whose suspend counter the main loop must therefore consult.
 *
 * For a borrowed connection that is the OWNER, not this Session: libssh2 is
 * not safe for concurrent use of one session, and locking this Session's own
 * io_lock while flipping the owner's session into blocking mode would leave
 * the owner's reader free to call into libssh2 at the same time (its trylock
 * on its own lock would succeed) — protocol corruption during a transfer, plus
 * a blocking read that can stall the main loop. `shared_session` is fixed at
 * connect, so begin/end always resolve to the same mutex.
 *
 * The suspend counter needs the identical resolution and for the identical
 * reason. session_read() trylocks the session it was handed, so on an ALIAS
 * that trylock takes the alias's own (free) lock and then calls libssh2 on the
 * owner's now-blocking session — the mutex does not protect it, only the
 * suspend check does. Both accessors therefore resolve to the owner, so every
 * session sharing one LIBSSH2_SESSION observes the owner's state. */
static Session *session_sftp_lock_owner(Session *s) {
    return ssh_connection_owner(s);
}

static void session_io_suspend_bump(Session *s, bool up) {
    if (!s) return;
    if (up) {
        unsigned count = atomic_load_explicit(&s->io_suspend_count,
                                               memory_order_relaxed);
        while (count < UINT_MAX && !atomic_compare_exchange_weak_explicit(
                   &s->io_suspend_count, &count, count + 1u,
                   memory_order_acq_rel, memory_order_relaxed)) {}
        return;
    }
    unsigned count = atomic_load_explicit(&s->io_suspend_count,
                                           memory_order_relaxed);
    while (count > 0 && !atomic_compare_exchange_weak_explicit(
               &s->io_suspend_count, &count, count - 1u,
               memory_order_acq_rel, memory_order_relaxed)) {}
}

/* The scope a worker's SFTP calls run in.
 *
 * The session stays NON-blocking: the SFTP_CALL macros in sftp.h retry an
 * EAGAIN through sftp_io_yield(), which drops the lock for the wait, and let
 * a shell that tried the lock in through sftp_io_breathe(). The lock is held
 * only while libssh2 is actually working on a call, so a network wait inside
 * libssh2 is never one the shell on the same session sits behind.
 *
 * Suspension (the reconnect gate protecting the cached SFTP handle) is
 * published BEFORE the lock is taken and cleared AFTER it is released. It
 * does not stop the main loop from polling: the trylock is what protects the
 * session, and with the lock free during waits there is something to poll. */
typedef struct {
    Session         *owner;   /* whose io_lock this thread holds inside the scope */
    Session         *session; /* the Session the scope was opened on (status source) */
    LIBSSH2_SESSION *ssh;
    int              depth;
} SftpScopeCtx;
static _Thread_local SftpScopeCtx g_sftp_scope;
#define LIU_SFTP_OPERATION_GATE 1 /* this file has the per-session SFTP operation gate */
static _Thread_local void *g_sftp_status_context;
static _Thread_local u64 g_sftp_status;

static void sftp_operation_lock(Session *owner) {
    pthread_mutex_lock(&owner->sftp_op_lock);
    u64 ticket = owner->sftp_ticket_next++;
    if (ticket != owner->sftp_ticket_serving) {
        atomic_fetch_add_explicit(&owner->sftp_waiters, 1u, memory_order_release);
        while (ticket != owner->sftp_ticket_serving)
            pthread_cond_wait(&owner->sftp_op_ready, &owner->sftp_op_lock);
        atomic_fetch_sub_explicit(&owner->sftp_waiters, 1u, memory_order_release);
    }
    pthread_mutex_unlock(&owner->sftp_op_lock);
}

static void sftp_operation_unlock(Session *owner) {
    pthread_mutex_lock(&owner->sftp_op_lock);
    owner->sftp_ticket_serving++;
    pthread_cond_broadcast(&owner->sftp_op_ready);
    pthread_mutex_unlock(&owner->sftp_op_lock);
}

void session_sftp_scope_begin(Session *s) {
    if (!s || s->type != SESSION_SSH) return;
    Session *owner = session_sftp_lock_owner(s);
    if (g_sftp_scope.depth > 0 && g_sftp_scope.owner == owner) {
        /* Nested on the same connection (a recursive copy): one lock, one
         * context, counted. */
        g_sftp_scope.depth++;
        session_io_suspend_bump(owner, true);
        return;
    }
    g_sftp_status_context = NULL;
    g_sftp_status = 0;
    session_io_suspend_bump(owner, true);
    sftp_operation_lock(owner);
    pthread_mutex_lock(&owner->io_lock);
    LIBSSH2_SESSION *ssh = session_libssh2_for_sftp(s);
    if (ssh) libssh2_session_set_blocking(ssh, 0);
    g_sftp_scope.owner = owner;
    g_sftp_scope.session = s;
    g_sftp_scope.ssh = ssh;
    g_sftp_scope.depth = 1;
}

void session_sftp_scope_end(Session *s) {
    if (!s || s->type != SESSION_SSH) return;
    Session *owner = session_sftp_lock_owner(s);
    if (g_sftp_scope.depth > 1 && g_sftp_scope.owner == owner) {
        g_sftp_scope.depth--;
        session_io_suspend_bump(owner, false);
        return;
    }
    g_sftp_scope.depth = 0;
    g_sftp_scope.owner = NULL;
    g_sftp_scope.session = NULL;
    g_sftp_scope.ssh = NULL;
    pthread_mutex_unlock(&owner->io_lock);
    sftp_operation_unlock(owner);
    session_io_suspend_bump(owner, false);
}

/* EAGAIN inside a scope: wait for the socket WITHOUT the lock, then take it
 * back. Short waits on purpose — the shell's own read may consume the very
 * bytes libssh2 was waiting for (they go to this channel's queue inside
 * libssh2), and then the socket never becomes readable here; the retry
 * finds them queued. Returns false once the session is no longer connected,
 * which turns the pending call into an error at the caller. */
/* Set once, at quit. Every SFTP wait in the tree goes through sftp_io_yield
 * (the SFTP_RETRY macros in sftp.h), and "false" already means "stop, the
 * session is gone" to all of them — so one flag ends every transfer at its
 * next wait, within the 20 ms the poll below can last. Waiting at quit for
 * transfers to FINISH would hold the app open for a large upload's whole
 * length, and for ever over a link that has stopped answering, because the
 * keepalive that would declare the session dead runs from the main loop, which
 * is not running any more. */
static atomic_bool g_sftp_io_abort;

void sftp_io_abort_all(void) {
    atomic_store_explicit(&g_sftp_io_abort, true, memory_order_release);
}

bool sftp_io_yield(void) {
    SftpScopeCtx *y = &g_sftp_scope;
    if (!y->owner || !y->ssh) return false;
    if (atomic_load_explicit(&g_sftp_io_abort, memory_order_acquire)) return false;
    Session *o = y->owner;
    int dirs = libssh2_session_block_directions(y->ssh);
    int sock = o->sock;
    atomic_store_explicit(&o->io_yielding, true, memory_order_release);
    pthread_mutex_unlock(&o->io_lock);
    if (atomic_exchange_explicit(&o->io_wanted, false, memory_order_acq_rel))
        platform_wake_event_loop();
    struct pollfd pfd[2];
    int n = 0;
    if (sock >= 0) {
        pfd[n].fd = sock;
        pfd[n].events = 0;
        pfd[n].revents = 0;
        if (dirs & LIBSSH2_SESSION_BLOCK_INBOUND)  pfd[n].events |= POLLIN;
        if (dirs & LIBSSH2_SESSION_BLOCK_OUTBOUND) pfd[n].events |= POLLOUT;
        if (!pfd[n].events) pfd[n].events = POLLIN;
        n++;
    }
    if (o->io_kick[0] >= 0) {
        pfd[n].fd = o->io_kick[0];
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
    }
    /* The timeout is a belt: the socket or the kick is what normally ends
     * this, and a peer that has gone quiet is caught by the keepalive. */
    if (n > 0) (void)poll(pfd, (nfds_t)n, 20);
    else usleep(2000);
    atomic_store_explicit(&o->io_yielding, false, memory_order_release);
    if (o->io_kick[0] >= 0) {
        u8 drain[64];
        while (read(o->io_kick[0], drain, sizeof drain) > 0) {}
    }
    bool alive = session_status(y->session) == SESSION_CONNECTED;
    pthread_mutex_lock(&o->io_lock);
    return alive;
}

/* After a call that did not need to wait: if the shell asked for the lock
 * meanwhile, hand it over for a moment. A burst of ready data would otherwise
 * keep the lock busy through the whole burst. */
void sftp_io_breathe(void) {
    SftpScopeCtx *y = &g_sftp_scope;
    if (!y->owner) return;
    /* An SFTP call can consume the socket packet carrying shell output too.
     * Those bytes now live inside libssh2, so no fd readiness need remain to
     * wake the UI. Notify only when a shell channel actually has queued data. */
    if ((y->owner->ssh_channel && libssh2_poll_channel_read(y->owner->ssh_channel, 0) > 0) ||
        (y->session && y->session != y->owner && y->session->ssh_channel &&
         libssh2_poll_channel_read(y->session->ssh_channel, 0) > 0))
        platform_wake_event_loop();
    /* The error belongs to the operation that just finished. Another SFTP
     * worker may change the handle's last-error field during the handoff. */
    if (y->session && y->session->sftp_session) {
        g_sftp_status_context = y->session->sftp_session;
        g_sftp_status = libssh2_sftp_last_error(y->session->sftp_session);
    }
    bool shell_waiting = atomic_load_explicit(&y->owner->io_wanted, memory_order_acquire);
    if (!shell_waiting &&
        !atomic_load_explicit(&y->owner->sftp_waiters, memory_order_acquire)) return;
    atomic_store_explicit(&y->owner->io_wanted, false, memory_order_release);
    pthread_mutex_unlock(&y->owner->io_lock);
    sftp_operation_unlock(y->owner);
    /* Tickets hand SFTP ownership directly to an existing waiter. Only the
     * shell's trylock needs the small scheduling gap used before this gate. */
    if (shell_waiting) {
        /* A queued shell WRITE may not have put any bytes on the socket yet.
         * Releasing this lock alone cannot wake a UI waiting for fd readiness. */
        platform_wake_event_loop();
        usleep(300);
    }
    sftp_operation_lock(y->owner);
    pthread_mutex_lock(&y->owner->io_lock);
}

u64 sftp_io_last_status(void *sftp) {
    if (sftp && g_sftp_status_context == sftp) return g_sftp_status;
    return sftp ? (u64)libssh2_sftp_last_error((LIBSSH2_SFTP *)sftp) : 0;
}

int sftp_io_last_errno(void) {
    return g_sftp_scope.ssh ? libssh2_session_last_errno(g_sftp_scope.ssh) : 0;
}

bool session_io_is_suspended(const Session *s) {
    if (!s) return false;
    const Session *owner = session_sftp_lock_owner((Session *)s);
    return atomic_load_explicit(&((Session *)owner)->io_suspend_count,
                                memory_order_acquire) > 0;
}

void session_io_set_suspended(Session *s, bool v) {
    session_io_suspend_bump(session_sftp_lock_owner(s), v);
}

const SSHConfig *session_get_config(const Session *s) {
    if (!s || (s->type != SESSION_SSH && s->type != SESSION_MOSH)) return NULL;
    return s->config;
}

/* Everything that belongs to ONE transport and must not cross into the next.
 * Called with io_lock held, after the old channel is gone and before the new
 * connection is attempted:
 *
 *   - the write queue and the in-flight copy: bytes typed at the OLD shell,
 *     never delivered. Replaying them into the new one would run a command
 *     line the user pressed Enter on for a session that no longer exists.
 *     They are dropped, not replayed; what the user types AFTER asking for
 *     the reconnect queues as usual and reaches the new shell.
 *   - the owed resize: the new channel's PTY is opened at s->cols x s->rows
 *     and ssh_do_connect re-baselines the sent size there.
 *   - the keepalive probe: an armed probe measured the old socket's counter;
 *     against the new socket it would be a phantom miss. */
static void session_transport_reset_locked(Session *s) {
    s->write_len = 0;
    session_sources_drop(s);
    s->inflight_active = false;
    s->inflight_len = s->inflight_off = 0;
    s->resize_inflight = false;
    s->resize_want_cols = s->resize_sent_cols = s->cols;
    s->resize_want_rows = s->resize_sent_rows = s->rows;
    s->last_keepalive_time = 0;
    s->keepalive_missed = 0;
    s->keepalive_probe_armed = false;
    s->keepalive_rx_at_probe = 0;
    s->io_watch_retired = false;   /* a new socket follows; it may be watched */
}

/* The remote shell is gone but the transport is fine: stop treating the
 * session as live (reads, the fd watch, writes all key off the status). The
 * connection itself is closed when the pane is, or replaced by a reconnect. */
static void session_mark_ended(Session *s) {
    pthread_mutex_lock(&s->status_lock);
    if (s->status == SESSION_CONNECTED) s->status = SESSION_DISCONNECTED;
    pthread_mutex_unlock(&s->status_lock);
    session_retire_io_watch(s);
}

/* LIU_SSH_RETRY_MS replaces the scheduled automatic retry delay when set to a
 * value from 1 through 60000 milliseconds. The value is read for each retry
 * attempt. */
static i32 link_retry_delay_override(i32 scheduled) {
    const char *e = getenv("LIU_SSH_RETRY_MS");
    if (e && e[0]) {
        char *end = NULL;
        long v = strtol(e, &end, 10);
        if (end && *end == '\0' && v >= 1 && v <= 60000) return (i32)v;
    }
    return scheduled;
}

_Static_assert((int)SESSION_LINK_CONNECTING == (int)LINK_SAY_CONNECTING &&
               (int)SESSION_LINK_UP == (int)LINK_SAY_UP &&
               (int)SESSION_LINK_RECONNECTED == (int)LINK_SAY_RECONNECTED &&
               (int)SESSION_LINK_ENDED == (int)LINK_SAY_ENDED &&
               (int)SESSION_LINK_LOST == (int)LINK_SAY_LOST &&
               (int)SESSION_LINK_FAILED == (int)LINK_SAY_FAILED &&
               (int)SESSION_LINK_GAVE_UP == (int)LINK_SAY_GAVE_UP,
               "SessionLinkEvent mirrors LinkSay");

SessionLinkEvent session_link_step(Session *s, i32 *retry_in_ms, i32 *attempt) {
    if (retry_in_ms) *retry_in_ms = 0;
    if (attempt) *attempt = 0;
    if (!s || s->type != SESSION_SSH) return SESSION_LINK_NOTHING;

    SessionStatus status = session_status(s);
    /* The server's prompt waiting on the dialog is an attempt in flight
     * (ssh/link_sees.h), never a failure the pane announces. */
    LinkSees sees = link_sees_session(status == SESSION_CONNECTED,
                                      status == SESSION_CONNECTING ||
                                      status == SESSION_AUTHENTICATING,
                                      status == SESSION_KBI_PENDING);
    /* session_is_alive answers "alive" when it cannot take the lock (a
     * transfer is mid-call), so a busy session is never called ended. */
    bool channel_open = true;
    if (sees == LINK_SEES_CONNECTED && s->link.phase == LINK_UP)
        channel_open = session_is_alive(s);
    bool network_failure =
        !atomic_load_explicit(&s->connect_past_network, memory_order_acquire);

    LinkState before = s->link;
    LinkStep step = link_policy_step(&s->link, sees, channel_open, network_failure);
    if (step.say == LINK_SAY_ENDED) session_mark_ended(s);
    if (step.retry_delay_ms > 0) {
        i32 wait = link_retry_delay_override(step.retry_delay_ms);
        /* The reason is about to be cleared by the reconnect; the caller reads
         * it from session_link_reason(), which keeps the last one. */
        snprintf(s->link_reason, sizeof s->link_reason, "%s", s->error_msg);
        if (!session_reconnect_after(s, wait)) {
            /* A transfer holds the session. Nothing is said and nothing is
             * counted; the pass that follows the transfer's end asks again. */
            link_policy_unstarted(&s->link, &before);
            return SESSION_LINK_NOTHING;
        }
        if (retry_in_ms) *retry_in_ms = wait;
    } else if (step.say == LINK_SAY_FAILED || step.say == LINK_SAY_GAVE_UP) {
        snprintf(s->link_reason, sizeof s->link_reason, "%s", s->error_msg);
    }
    if (attempt) *attempt = step.attempt;
    return (SessionLinkEvent)step.say;
}

const char *session_link_reason(const Session *s) {
    return s && s->type == SESSION_SSH ? s->link_reason : "";
}

bool session_link_wants_enter(const Session *s) {
    return s && s->type == SESSION_SSH &&
           (s->link.phase == LINK_ENDED || s->link.phase == LINK_DOWN);
}

bool session_link_retry_now(Session *s) {
    if (!session_link_wants_enter(s)) return false;
    LinkState before = s->link;
    if (!link_policy_manual(&s->link)) return false;
    if (!session_reconnect_after(s, 0)) { s->link = before; return false; }
    return true;
}

bool session_reconnect(Session *s) { return session_reconnect_after(s, 0); }

bool session_reconnect_after(Session *s, i32 delay_ms) {
    if (!s || s->type != SESSION_SSH || !s->config) return false;
    if (session_io_is_suspended(s)) return false;
    /* Other sessions are inside this one's libssh2 session (ControlMaster):
     * replacing the connection would free it under them. Like a busy lock this
     * is "not now" and nothing is counted; each of them reconnects on its own
     * once the connection is really down, and leaves it, and then this goes
     * ahead. */
    if (s->is_shared_owner && atomic_load_explicit(&s->borrowers, memory_order_acquire) > 0)
        return false;

    /* Any prior connect worker must be fully done before libssh2 state is
     * touched, or the worker and this thread would race. */
    ssh_connect_worker_stop(s);

    /* A transfer can claim the lock between the suspended check and here.
     * Never stall the UI behind a large transfer; reconnect can be retried.
     * The lock is the CONNECTION's: a borrower's own is not the one that keeps
     * the others out of the libssh2 session its channel is on. Kept in a
     * variable, because the borrower stops being one before it unlocks. */
    Session *old_owner = !s->is_shared_owner ? s->shared_session : NULL;
    pthread_mutex_t *held = &ssh_connection_owner(s)->io_lock;
    if (pthread_mutex_trylock(held) != 0) return false;

    /* The old connection is discarded, so its socket is shut first, as the
     * final teardown does. libssh2 closes a channel by WAITING for the
     * server's close, which a non-blocking call cannot do: with the link up, a
     * channel still open here (an agent channel a remote program holds, a
     * forward) would stay allocated, and the free below would then give up on
     * the whole libssh2 session, leaking it on every reconnect of a live link.
     */
    if (s->is_shared_owner && s->sock >= 0) shutdown(s->sock, SHUT_RDWR);

    /* Cleanup old connection */
    session_stop_stream_forwards(s);
    session_agent_cleanup(s);
    if (s->is_shared_owner) atomic_store_explicit(&s->agent_active, false, memory_order_release);
    remote_forward_cleanup_all(s);
    if (s->is_shared_owner && s->ssh_session) {
        forward_cleanup(s->ssh_session);
    }
    if (s->sftp_session) { libssh2_sftp_shutdown(s->sftp_session); s->sftp_session = NULL; }
    /* initial_cwd is a heap pointer that is only allocated once SFTP+realpath
     * succeed; it is NULL on SFTP-less hosts. Writing initial_cwd[0] here
     * dereferenced NULL on auto-reconnect. free()+NULL also drops the stale
     * cwd so the fresh connection re-resolves it. */
    free(s->initial_cwd);
    s->initial_cwd = NULL;
    s->sftp_init_tried = false;
    if (s->ssh_channel) {
        libssh2_channel_send_eof(s->ssh_channel);
        libssh2_channel_close(s->ssh_channel);
        libssh2_channel_free(s->ssh_channel);
        s->ssh_channel = NULL;
    }
    if (s->is_shared_owner) {
        if (s->ssh_session) {
            libssh2_session_disconnect(s->ssh_session, "reconnect");
            libssh2_session_free(s->ssh_session);
            s->ssh_session = NULL;
        }
        if (s->sock >= 0) {
            /* Unwatch before close — otherwise the UI's status-edge
             * detector (which fires next tick with s->sock==-1) has no
             * way to cancel the dispatch source, leaving a leaked
             * source watching a recycled fd. */
            platform_unwatch_socket(s->sock);
            close(s->sock);
            s->sock = -1;
        }
    } else {
        s->ssh_session = NULL;
        s->sock = -1;
    }
    s->shared_session = NULL;
    s->is_shared_owner = true; /* reconnect always creates its own connection */
    s->config_fwds_started = false;   /* the new connection starts them again */
    proxy_hops_cleanup(s);
    /* The old transport's generation ends here: nothing queued for it may
     * reach the next one. */
    session_transport_reset_locked(s);
    pthread_mutex_unlock(held);
    /* A borrower hands its owner back (the last reference tears it down), and
     * counts itself out. After the unlock: the owner's lock may go with it. */
    if (old_owner) {
        atomic_store_explicit(&old_owner->pool_full, false, memory_order_release);
        atomic_fetch_sub_explicit(&old_owner->borrowers, 1u, memory_order_acq_rel);
        session_release(old_owner);
    }

    /* Reset cancel flag for new attempt, set status, and spawn worker.
     * same async model as session_create_ssh — the caller returns
     * immediately with status == SESSION_CONNECTING, UI polls for transition. */
    atomic_store_explicit(&s->connect_cancel, false, memory_order_release);
    atomic_store_explicit(&s->connect_past_network, false, memory_order_release);
    /* Read by the worker when it meets a host key it does not know: a retry
     * nobody asked for does not put a changed key in front of the user. */
    atomic_store_explicit(&s->connect_unattended, s->link.auto_retry, memory_order_release);
    s->connect_delay_ms = delay_ms > 0 ? delay_ms : 0;
    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_CONNECTING;
    s->error_msg[0] = '\0';
    pthread_mutex_unlock(&s->status_lock);

    /* Reset hostkey dialog state for new connection attempt */
    pthread_mutex_lock(&s->hostkey_lock);
    s->hostkey_pending = false;
    s->hostkey_accepted = false;
    pthread_mutex_unlock(&s->hostkey_lock);

    /* Reset KBI dialog state */
    pthread_mutex_lock(&s->kbi_lock);
    s->kbi_ready = false;
    s->kbi_num_prompts = 0;
    pthread_mutex_unlock(&s->kbi_lock);

    /* (keepalive counters, the write queue and the owed resize were reset
     * under the lock above — session_transport_reset_locked) */

    if (pthread_create(&s->connect_thread, NULL, ssh_connect_worker, s) != 0) {
        set_error(s, "Failed to spawn SSH reconnect thread");
        return false;
    }
    s->connect_thread_active = true;
    return true;
}

/* =========================================================================
 * Host key dialog accessors (called from main/UI thread)
 * ========================================================================= */

bool session_hostkey_pending(Session *s) {
    if (!s || s->type != SESSION_SSH) return false;
    pthread_mutex_lock(&s->hostkey_lock);
    bool pending = s->hostkey_pending;
    pthread_mutex_unlock(&s->hostkey_lock);
    return pending;
}

void session_hostkey_get_info(Session *s, bool *is_change,
                              char *hostname, usize hostname_sz,
                              i32 *port,
                              char *old_fp, usize old_fp_sz,
                              char *new_fp, usize new_fp_sz) {
    if (!s) return;
    pthread_mutex_lock(&s->hostkey_lock);
    if (is_change)  *is_change = s->hostkey_is_change;
    if (hostname) {
        if (s->hostkey_host[0])
            snprintf(hostname, hostname_sz, "%s", s->hostkey_host);
        else if (s->config)
            snprintf(hostname, hostname_sz, "%s", s->config->hostname);
    }
    if (port) *port = s->hostkey_host[0] ? s->hostkey_port
                                         : (s->config ? s->config->port : 0);
    if (old_fp) snprintf(old_fp, old_fp_sz, "%s", s->hostkey_old_fp);
    if (new_fp) snprintf(new_fp, new_fp_sz, "%s", s->hostkey_new_fp);
    pthread_mutex_unlock(&s->hostkey_lock);
}

void session_hostkey_respond(Session *s, bool accept) {
    if (!s || s->type != SESSION_SSH) return;
    pthread_mutex_lock(&s->hostkey_lock);
    s->hostkey_accepted = accept;
    s->hostkey_pending = false;
    pthread_cond_signal(&s->hostkey_cond);
    pthread_mutex_unlock(&s->hostkey_lock);
}

/* =========================================================================
 * Keyboard-interactive (KBI / 2FA) accessors — called from UI thread
 * ========================================================================= */

bool session_kbi_pending(Session *s) {
    if (!s || s->type != SESSION_SSH) return false;
    pthread_mutex_lock(&s->status_lock);
    bool pending = (s->status == SESSION_KBI_PENDING);
    pthread_mutex_unlock(&s->status_lock);
    return pending;
}

i32 session_kbi_num_prompts(Session *s) {
    if (!s || s->type != SESSION_SSH) return 0;
    pthread_mutex_lock(&s->kbi_lock);
    i32 n = s->kbi_num_prompts;
    pthread_mutex_unlock(&s->kbi_lock);
    return n;
}

const char *session_kbi_name(Session *s) {
    if (!s || s->type != SESSION_SSH) return "";
    /* Caller must use result before next call; kbi_name is stable while
     * SESSION_KBI_PENDING (worker is blocked). */
    return s->kbi_name;
}

const char *session_kbi_instruction(Session *s) {
    if (!s || s->type != SESSION_SSH) return "";
    return s->kbi_instruction;
}

const char *session_kbi_peer(Session *s) {
    if (!s || s->type != SESSION_SSH) return "";
    return s->kbi_peer;
}

const char *session_kbi_prompt(Session *s, i32 index) {
    if (!s || s->type != SESSION_SSH || !s->kbi_prompts || index < 0 ||
        index >= s->kbi_num_prompts) return "";
    return s->kbi_prompts[index];
}

bool session_kbi_echo(Session *s, i32 index) {
    if (!s || s->type != SESSION_SSH || index < 0 ||
        index >= s->kbi_num_prompts) return true;
    return s->kbi_echo[index];
}

void session_kbi_submit(Session *s, const char **responses, i32 count) {
    if (!s || s->type != SESSION_SSH || count < 0 ||
        (count > 0 && !responses)) return;
    pthread_mutex_lock(&s->kbi_lock);
    pthread_mutex_lock(&s->status_lock);
    bool pending = s->status == SESSION_KBI_PENDING;
    pthread_mutex_unlock(&s->status_lock);
    if (!pending) {
        pthread_mutex_unlock(&s->kbi_lock);
        return;
    }
    if (s->kbi_responses) {
        i32 n = (count < s->kbi_num_prompts) ? count : s->kbi_num_prompts;
        for (i32 i = 0; i < n; i++) {
            if (responses[i]) {
                usize len = strnlen(responses[i], sizeof(s->kbi_responses[i]) - 1);
                memcpy(s->kbi_responses[i], responses[i], len);
                s->kbi_responses[i][len] = '\0';
            } else {
                s->kbi_responses[i][0] = '\0';
            }
        }
    }
    s->kbi_ready = true;
    pthread_cond_signal(&s->kbi_cond);
    pthread_mutex_unlock(&s->kbi_lock);
}

/* =========================================================================
 * Port forwarding implementations
 * ========================================================================= */

/* The config's local forward on `local_port`, or -1. */
static i32 config_local_forward_index(const SSHConfig *c, i32 local_port) {
    for (i32 i = 0; c && c->local_forwards && i < c->local_forward_count; i++)
        if (c->local_forwards[i].bind_port == local_port) return i;
    return -1;
}

bool session_local_forward_start(Session *s, i32 local_port,
                                  const char *remote_host, i32 remote_port) {
    if (!s || s->type != SESSION_SSH ||
        session_status(s) != SESSION_CONNECTED)
        return false;
    if (!s->ssh_session) return false;
    if (pthread_mutex_trylock(&ssh_connection_owner(s)->io_lock) != 0) return false;
    bool ok = forward_add_local(s->ssh_session, local_port,
                                remote_host, remote_port) >= 0;
    /* Kept with the connection's own LocalForward lines, so a reconnect puts
     * it back (session_start_config_forwards_locked). The forwards belong to
     * the LIBSSH2_SESSION and went with it: after the pane came back by
     * itself, the port answered "connection refused" and nothing said why.
     * Only while connected, so no connect worker is reading the config. */
    if (ok && s->config && s->config->local_forward_count < MAX_PORT_FORWARDS &&
        config_local_forward_index(s->config, local_port) < 0) {
        PortForwardSpec *p = ssh_config_push_local_forward(s->config);
        if (p) {
            memset(p, 0, sizeof *p);
            p->bind_port = local_port;
            snprintf(p->dest_host, sizeof p->dest_host, "%s", remote_host);
            p->dest_port = remote_port;
        }
    }
    pthread_mutex_unlock(&ssh_connection_owner(s)->io_lock);
    return ok;
}

bool session_dynamic_forward_start(Session *s, i32 local_port) {
    if (!s || s->type != SESSION_SSH ||
        session_status(s) != SESSION_CONNECTED)
        return false;
    if (!s->ssh_session) return false;
    if (pthread_mutex_trylock(&ssh_connection_owner(s)->io_lock) != 0) return false;
    bool ok = forward_add_socks5(s->ssh_session, local_port) >= 0;
    pthread_mutex_unlock(&ssh_connection_owner(s)->io_lock);
    return ok;
}

bool session_local_forward_poll(Session *s) {
    if (!s || s->type != SESSION_SSH ||
        session_status(s) != SESSION_CONNECTED)
        return false;
    if (!s->ssh_session) return false;
    if (!session_io_trylock(s)) return false;
    bool more = forward_poll(s->ssh_session);
    session_io_unlock(s);
    /* A relay that stopped on its byte budget has the rest inside libssh2 (its
     * channel read already emptied the socket): no descriptor announces it,
     * and the caller's "more" only counted as data for the frame. Latched here
     * so the loop does not sleep on it — a transfer through a forward stopped
     * at the first 64 KB until something else woke the loop. */
    if (more) pty_reader_note_backlog();
    return more;
}

i32 session_local_forward_count(Session *s) {
    if (!s || s->type != SESSION_SSH ||
        session_status(s) != SESSION_CONNECTED)
        return 0;
    if (!s->ssh_session) return 0;
    if (pthread_mutex_trylock(&ssh_connection_owner(s)->io_lock) != 0) return 0;
    i32 count = forward_count(s->ssh_session);
    pthread_mutex_unlock(&ssh_connection_owner(s)->io_lock);
    return count;
}

bool session_local_forward_get(Session *s, i32 index, PortForwardInfo *info) {
    if (!info) return false;
    if (!s || s->type != SESSION_SSH ||
        session_status(s) != SESSION_CONNECTED)
        return false;
    if (!s->ssh_session) return false;

    ForwardRule rule;
    if (pthread_mutex_trylock(&ssh_connection_owner(s)->io_lock) != 0) return false;
    bool found = forward_get(s->ssh_session, index, &rule);
    pthread_mutex_unlock(&ssh_connection_owner(s)->io_lock);
    if (!found) return false;

    memset(info, 0, sizeof(*info));
    snprintf(info->bind_host, sizeof(info->bind_host), "%s", "127.0.0.1");
    info->bind_port = rule.local_port;
    snprintf(info->remote_host, sizeof(info->remote_host), "%s", rule.remote_host);
    info->remote_port = rule.remote_port;
    info->local_port = rule.local_port;
    info->active = rule.active;
    info->type = rule.type;
    return true;
}

void session_local_forward_remove(Session *s, i32 index) {
    if (!s || s->type != SESSION_SSH || !s->ssh_session) return;
    if (pthread_mutex_trylock(&ssh_connection_owner(s)->io_lock) != 0) return;
    /* A removed forward is not started again by the next reconnect. */
    ForwardRule rule;
    if (forward_get(s->ssh_session, index, &rule) && rule.type == FWD_LOCAL && s->config) {
        i32 at = config_local_forward_index(s->config, rule.local_port);
        if (at >= 0) {
            SSHConfig *c = s->config;
            memmove(&c->local_forwards[at], &c->local_forwards[at + 1],
                    (usize)(c->local_forward_count - at - 1) * sizeof c->local_forwards[0]);
            c->local_forward_count--;
        }
    }
    forward_remove(s->ssh_session, index);
    pthread_mutex_unlock(&ssh_connection_owner(s)->io_lock);
}

/* =========================================================================
 * Remote port forwarding
 * ========================================================================= */

#ifndef PLATFORM_WIN32
static void session_streamlocal_poll(Session *s);
#endif
#ifdef LIBSSH2_CALLBACK_AUTHAGENT
static void session_agent_poll_locked(Session *s);
#endif

static ssize_t relay_send_socket(int fd, const void *buf, usize len) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_DONTWAIT
    flags |= MSG_DONTWAIT;
#endif
    return send(fd, buf, len, flags);
}

static ssize_t relay_recv_socket(int fd, void *buf, usize len) {
    int flags = 0;
#ifdef MSG_DONTWAIT
    flags |= MSG_DONTWAIT;
#endif
    return recv(fd, buf, len, flags);
}

/* The loop is woken for a relay's socket only while the relay can take what
 * arrives there (ssh/relay_watch.h). */
static void socket_channel_relay_watch(SocketChannelRelay *c) {
    bool want = relay_wants_read_watch(c->active && c->fd >= 0, c->fd_eof, c->to_channel_len);
    if (want == c->fd_watched) return;
    if (want) platform_watch_socket(c->fd);
    else      platform_unwatch_socket(c->fd);
    c->fd_watched = want;
}

static void socket_channel_relay_close(SocketChannelRelay *c) {
    if (!c || !c->active) return;
    if (c->connecting) { net_connect_abandon(c->connecting); c->connecting = NULL; }
    if (c->fd_watched && c->fd >= 0) platform_unwatch_socket(c->fd);
    c->fd_watched = false;
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    if (c->channel) {
        libssh2_channel_close(c->channel);
        libssh2_channel_free(c->channel);
        c->channel = NULL;
    }
    c->active = false;
    c->to_fd_off = c->to_fd_len = 0;
    c->to_channel_off = c->to_channel_len = 0;
}

static bool socket_channel_relay_step(SocketChannelRelay *c, bool *moved);
static bool socket_channel_relay_poll(SocketChannelRelay *c) {
    c->more = false;
    for (i32 step = 0; step < SOCKET_RELAY_POLL_STEPS; step++) {
        bool moved = false;
        if (!socket_channel_relay_step(c, &moved)) return false;
        if (!moved) break;            /* the rest waits on a socket: its watch wakes the loop */
        if (step + 1 == SOCKET_RELAY_POLL_STEPS) c->more = true;
    }
    socket_channel_relay_watch(c);
    return true;
}

/* The main thread's poll of a relay that stopped on its budget has work left
 * inside libssh2 (its channel read already emptied the socket) or in the
 * relay's buffers; nothing will announce it, so the loop is told not to sleep
 * (the same latch a pane's drain sets). Not for the ProxyJump thread, which
 * waits on its own descriptors. */
static void socket_channel_relay_note_more(const SocketChannelRelay *c) {
    if (c && c->active && c->more) pty_reader_note_backlog();
}

static bool socket_channel_relay_step(SocketChannelRelay *c, bool *moved) {
    *moved = false;
    if (!c || !c->active || c->fd < 0 || !c->channel) return false;

    if (c->to_fd_off < c->to_fd_len) {
        ssize_t n = relay_send_socket(c->fd, c->to_fd + c->to_fd_off,
                                      c->to_fd_len - c->to_fd_off);
        if (n > 0) { c->to_fd_off += (usize)n; *moved = true; }
        else if (n < 0 && errno != EINTR && errno != EAGAIN &&
                 errno != EWOULDBLOCK) return false;
        if (c->to_fd_off == c->to_fd_len)
            c->to_fd_off = c->to_fd_len = 0;
    }
    if (c->to_channel_off < c->to_channel_len) {
        ssize_t n = libssh2_channel_write(
            c->channel, (const char *)c->to_channel + c->to_channel_off,
            c->to_channel_len - c->to_channel_off);
        if (n > 0) { c->to_channel_off += (usize)n; *moved = true; }
        else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) return false;
        if (c->to_channel_off == c->to_channel_len)
            c->to_channel_off = c->to_channel_len = 0;
    }

    if (!c->chan_eof && c->to_fd_len == 0) {
        ssize_t n = libssh2_channel_read(c->channel, (char *)c->to_fd,
                                         sizeof c->to_fd);
        if (n > 0) { c->to_fd_len = (usize)n; *moved = true; }
        else if (n == 0) { c->chan_eof = true; *moved = true; }   /* the server is done sending */
        else if (n != LIBSSH2_ERROR_EAGAIN) return false;
    }
    if (!c->fd_eof && c->to_channel_len == 0) {
        ssize_t n = relay_recv_socket(c->fd, c->to_channel,
                                      sizeof c->to_channel);
        if (n > 0) { c->to_channel_len = (usize)n; *moved = true; }
        else if (n == 0) { c->fd_eof = true; *moved = true; }     /* the socket is done sending */
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return false;
    }

    /* Pass each EOF on once everything that side sent has been delivered. */
    if (c->fd_eof && !c->eof_sent && c->to_channel_len == 0) {
        int rc = libssh2_channel_send_eof(c->channel);
        if (rc == 0) { c->eof_sent = true; *moved = true; }
        else if (rc != LIBSSH2_ERROR_EAGAIN) return false;
    }
    if (c->chan_eof && !c->shut_wr && c->to_fd_len == 0) {
        (void)shutdown(c->fd, SHUT_WR);
        c->shut_wr = true;
        *moved = true;
        c->linger_until = platform_time_sec() + SOCKET_RELAY_LINGER_SEC;
    }
    /* Finished: both sides have said so and been heard. */
    if (c->shut_wr && (c->eof_sent || platform_time_sec() > c->linger_until))
        return false;
    return true;
}

/* The host's LocalForward lines (~/.ssh/config, through ssh_config_apply)
 * are started once per connection, by the first poll that finds it up: their
 * listeners belong to the main thread, like the ones the Port Forwarding
 * dialog adds. This covers forwards to a port and to a Unix socket. Loopback
 * only: a forward is never offered to the network (no GatewayPorts). */
static void session_start_config_forwards_locked(Session *s) {
    s->config_fwds_started = true;
    const SSHConfig *c = s->config;
    if (!c || !s->ssh_session) return;
    /* The connection's forwards belong to the session that made it (ControlMaster):
     * a pane borrowing it would bind the same ports a second time. */
    if (!s->is_shared_owner) return;
    for (i32 i = 0; i < c->local_forward_count; i++) {
        const PortForwardSpec *p = &c->local_forwards[i];
        if (forward_add_local(s->ssh_session, p->bind_port, p->dest_host, p->dest_port) < 0)
            fprintf(stderr, "liu: LocalForward %d %s:%d not started (is the port taken?)\n",
                    p->bind_port, p->dest_host, p->dest_port);
    }
#ifndef PLATFORM_WIN32
    if (c->stream_forward_count > 0) session_start_stream_forwards(s);
#endif
}

static void session_remote_forward_poll_locked(Session *s) {
    if (!s || s->type != SESSION_SSH ||
        session_status(s) != SESSION_CONNECTED) return;
    if (!s->config_fwds_started) session_start_config_forwards_locked(s);
#ifndef PLATFORM_WIN32
    session_streamlocal_poll(s);
#endif
#ifdef LIBSSH2_CALLBACK_AUTHAGENT
    /* Here rather than beside X11: this runs for every pane's session, split
     * panes included, which the X11 poll does not. */
    session_agent_poll_locked(s);
#endif
    if (s->remote_listener_count == 0 || !s->remote_listeners) return;

    /* Check each listener for accepted connections */
    for (i32 i = 0; i < s->remote_listener_count; i++) {
        RemoteForwardListener *l = &s->remote_listeners[i];
        if (!l->active || !l->listener) continue;

        LIBSSH2_CHANNEL *ch = libssh2_channel_forward_accept(l->listener);
        if (!ch) continue;

        /* A newly accepted remote-forward connection starts its local
         * destination connect here; the result is collected below. This runs
         * in the main-thread session poll with the session lock held, so
         * destination resolution and connection must remain asynchronous to
         * avoid blocking other windows. */
        if (s->remote_fwd_conn_count >= MAX_REMOTE_FWD_CONNS) {
            libssh2_channel_free(ch);
            continue;
        }
        if (!s->remote_fwd_conns) {
            s->remote_fwd_conns = calloc(MAX_REMOTE_FWD_CONNS,
                                         sizeof(*s->remote_fwd_conns));
            if (!s->remote_fwd_conns) {
                libssh2_channel_free(ch);
                continue;
            }
        }

        i32 slot = -1;
        for (i32 j = 0; j < MAX_REMOTE_FWD_CONNS; j++) {
            if (!s->remote_fwd_conns[j]) { slot = j; break; }
        }
        if (slot < 0) {
            libssh2_channel_free(ch);
            continue;
        }
        /* One relay (two 16 KB buffers) per live connection, freed on close. */
        RemoteForwardConn *c = calloc(1, sizeof *c);
        if (!c) {
            libssh2_channel_free(ch);
            continue;
        }
        c->channel = ch;
        c->fd = -1;
        c->active = true;
        c->connecting = net_connect_begin(l->dest_host, l->dest_port,
                                          REMOTE_FWD_CONNECT_TIMEOUT_MS,
                                          session_notify_main);
        if (!c->connecting) {
            socket_channel_relay_close(c);
            free(c);
            continue;
        }
        libssh2_channel_set_blocking(ch, 0);
        s->remote_fwd_conns[slot] = c;
        s->remote_fwd_conn_count++;
    }

    /* Relay and clean up finished connections on this same main thread. */
    if (!s->remote_fwd_conns) return;
    for (i32 i = 0; i < MAX_REMOTE_FWD_CONNS; i++) {
        RemoteForwardConn *c = s->remote_fwd_conns[i];
        if (!c || !c->active) continue;
        if (c->connecting) {
            int fd = net_connect_poll(c->connecting);
            if (fd == NET_CONNECT_PENDING) continue;   /* the channel waits; nothing is read from it yet */
            c->connecting = NULL;                      /* consumed either way */
            if (fd >= 0 && !net_set_nonblocking(fd)) { close(fd); fd = -1; }
            if (fd >= 0) {
#ifdef SO_NOSIGPIPE
                int one = 1;
                (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                c->fd = fd;
            }
            /* A failed connect leaves fd at -1: the relay poll below refuses
             * it and the ordinary close path frees the channel, which is what
             * tells the remote side its connection was not accepted. */
        }
        if (!socket_channel_relay_poll(c)) {
            socket_channel_relay_close(c);
            free(c);
            s->remote_fwd_conns[i] = NULL;
            s->remote_fwd_conn_count--;
        } else {
            socket_channel_relay_note_more(c);
        }
    }
}

void session_remote_forward_poll(Session *s) {
    if (!s || s->type != SESSION_SSH ||
        session_status(s) != SESSION_CONNECTED || !s->ssh_session) return;
    if (!session_io_trylock(s)) return;
    session_remote_forward_poll_locked(s);
    session_io_unlock(s);
}

/* =========================================================================
 * Keepalive
 * ========================================================================= */

void session_keepalive_check(Session *s, f64 now_sec) {
    if (!s || s->type != SESSION_SSH ||
        session_status(s) != SESSION_CONNECTED) return;
    if (!s->ssh_session) return;

    i32 ival = s->keepalive_interval;
    if (ival <= 0) return;
    if (!isfinite(now_sec) || now_sec <= 0) return;
    if (s->last_keepalive_time > 0 &&
        now_sec - s->last_keepalive_time < (f64)ival) {
        return;
    }

    /* Did anything arrive since the previous probe? That is the question a
     * keepalive answers, and libssh2_keepalive_send cannot: it reports that the
     * probe was handed to the socket, which a black-holed peer accepts for
     * minutes. The socket's receive counter moves for the reply (want_reply is
     * on) and for any other traffic; a counter that did not move across a whole
     * interval is a missed probe. Platforms without the counter count sends
     * instead, a weaker signal. */
    u64 rx_now = 0;
    bool have_rx = SESSION_RX_BYTES_FN(session_sftp_lock_owner(s)->sock, &rx_now);
    if (have_rx && s->keepalive_probe_armed) {
        /* ONE verdict per probe. The probe is disarmed the moment it is
         * judged, whatever happens to the send below: with the I/O lock held
         * by a transfer this function comes back on the next loop turn, and
         * a probe left armed would re-judge the SAME silent interval on
         * every such turn, counting several missed probes for one and
         * dropping a busy session. A new verdict needs a new probe, and a
         * new probe needs the send to have gone out. */
        s->keepalive_probe_armed = false;
        if (rx_now == s->keepalive_rx_at_probe) s->keepalive_missed++;
        else                                    s->keepalive_missed = 0;
        if (s->keepalive_missed >= s->keepalive_count_max) {
            set_error(s, "SSH keepalive timeout — connection lost");
            return;
        }
    }

    /* The lock is a transfer's for as long as it needs it: a probe that
     * cannot be sent this turn is retried on the next one, and
     * last_keepalive_time stays put so the retry is not deferred by a whole
     * interval. Nothing is counted for a turn that sent nothing. */
    int seconds_to_next = 0;
    if (!session_io_trylock(s)) return;
    int rc = libssh2_keepalive_send(s->ssh_session, &seconds_to_next);
    session_io_unlock(s);
    if (rc == LIBSSH2_ERROR_EAGAIN) return;   /* socket full: same as a busy lock */
    s->last_keepalive_time = now_sec;
    if (rc != 0) {
        /* The transport refused the probe outright. Nothing was learned
         * about the peer; on a platform that can watch the receive side the
         * next probe will, on one that cannot this is the only signal there
         * is, so it counts once per interval. */
        if (!have_rx) {
            s->keepalive_missed++;
            if (s->keepalive_missed >= s->keepalive_count_max)
                set_error(s, "SSH keepalive timeout — connection lost");
        }
        return;
    }
    if (have_rx) {
        s->keepalive_probe_armed = true;
        s->keepalive_rx_at_probe = rx_now;
    } else {
        s->keepalive_missed = 0;
    }
}

/* =========================================================================
 * ssh_config_default
 * ========================================================================= */

SSHConfig ssh_config_default(void) {
    SSHConfig c;
    memset(&c, 0, sizeof(c));
    c.port = 22;
    c.auth_method = AUTH_AGENT;
    return c;
}

/* ----- Dynamic array helpers --------------------------------------------- */

static bool grow_array(void **base, i32 *cap, i32 want, usize elem) {
    if (!base || !cap || want < 0 || *cap < 0 || elem == 0) return false;
    if (want <= *cap) return want == 0 || *base != NULL;
    i32 ncap = *cap > 0 ? *cap : 2;
    while (ncap < want) {
        if (ncap > INT32_MAX / 2) {
            ncap = want;
            break;
        }
        ncap *= 2;
    }
    usize new_bytes;
    usize old_bytes;
    if (!checked_mul_usize((usize)ncap, elem, &new_bytes) ||
        !checked_mul_usize((usize)*cap, elem, &old_bytes)) {
        return false;
    }
    void *p = realloc(*base, new_bytes);
    if (!p) return false;
    /* Zero the freshly grown tail so callers reading uninitialised slots
     * (e.g. before they've filled the new entry) see a stable value. */
    u8 *bytes = (u8 *)p;
    memset(bytes + old_bytes, 0, new_bytes - old_bytes);
    *base = p;
    *cap = ncap;
    return true;
}

static bool clone_array(void **dst, const void *src, i32 count, usize elem) {
    if (!dst || count < 0 || elem == 0) return false;
    *dst = NULL;
    if (count == 0) return true;
    if (!src) return false;
    usize bytes;
    if (!checked_mul_usize((usize)count, elem, &bytes)) return false;
    void *copy = malloc(bytes);
    if (!copy) return false;
    memcpy(copy, src, bytes);
    *dst = copy;
    return true;
}

#define SSH_CONFIG_MAX_ENV_VARS 256
#define SSH_CONFIG_MAX_KEY_PEM  (16u * 1024u * 1024u)

static bool ssh_config_strings_valid(const SSHConfig *c) {
    if (!c) return false;
#define SSH_FIELD_OK(field) memchr(c->field, '\0', sizeof(c->field))
    if (!SSH_FIELD_OK(hostname) || !SSH_FIELD_OK(username) ||
        !SSH_FIELD_OK(password) || !SSH_FIELD_OK(key_path) ||
        !SSH_FIELD_OK(key_passphrase) || !SSH_FIELD_OK(jump_host) ||
        !SSH_FIELD_OK(jump_username) || !SSH_FIELD_OK(jump_password) ||
        !SSH_FIELD_OK(control_path) || !SSH_FIELD_OK(start_dir) ||
        !SSH_FIELD_OK(password_secret_id) ||
        !SSH_FIELD_OK(passphrase_secret_id) ||
        !SSH_FIELD_OK(private_key_secret_id)) return false;
#undef SSH_FIELD_OK
    for (i32 i = 0; i < c->stream_forward_count; i++) {
        if (!memchr(c->stream_forwards[i].local_socket, '\0',
                    sizeof(c->stream_forwards[i].local_socket)) ||
            !memchr(c->stream_forwards[i].remote_socket, '\0',
                    sizeof(c->stream_forwards[i].remote_socket))) return false;
    }
    for (i32 i = 0; i < c->proxy_chain_len; i++) {
        const ProxyHop *p = &c->proxy_chain[i];
        if (!memchr(p->hostname, '\0', sizeof p->hostname) ||
            !memchr(p->username, '\0', sizeof p->username) ||
            !memchr(p->key_path, '\0', sizeof p->key_path) ||
            p->auth_method < AUTH_PASSWORD || p->auth_method > AUTH_GSSAPI)
            return false;
    }
    for (i32 i = 0; i < c->local_forward_count; i++) {
        const PortForwardSpec *p = &c->local_forwards[i];
        if (!memchr(p->bind_host, '\0', sizeof p->bind_host) ||
            !memchr(p->dest_host, '\0', sizeof p->dest_host)) return false;
    }
    for (i32 i = 0; i < c->remote_forward_count; i++) {
        const PortForwardSpec *p = &c->remote_forwards[i];
        if (!memchr(p->bind_host, '\0', sizeof p->bind_host) ||
            !memchr(p->dest_host, '\0', sizeof p->dest_host)) return false;
    }
    for (i32 i = 0; i < c->env_count; i++) {
        if (!memchr(c->env[i].name, '\0', sizeof(c->env[i].name)) ||
            !memchr(c->env[i].value, '\0', sizeof(c->env[i].value)))
            return false;
    }
    return true;
}

/* The struct uses both `_count` and `_len` for sizes and singular vs plural
 * field names (e.g. `local_forwards` array but `local_forward_count`), so each
 * helper expands the explicit field/cap/len triple instead of a macro. */
StreamLocalForward *ssh_config_push_stream_forward(SSHConfig *cfg) {
    if (!cfg || cfg->stream_forward_count < 0 ||
        cfg->stream_forward_count == INT32_MAX) return NULL;
    if (!grow_array((void **)&cfg->stream_forwards, &cfg->stream_forward_cap,
                    cfg->stream_forward_count + 1, sizeof(StreamLocalForward))) return NULL;
    StreamLocalForward *slot = &cfg->stream_forwards[cfg->stream_forward_count++];
    memset(slot, 0, sizeof *slot);
    return slot;
}

ProxyHop *ssh_config_push_proxy_hop(SSHConfig *cfg) {
    if (!cfg || cfg->proxy_chain_len < 0 ||
        cfg->proxy_chain_len == INT32_MAX) return NULL;
    if (!grow_array((void **)&cfg->proxy_chain, &cfg->proxy_chain_cap,
                    cfg->proxy_chain_len + 1, sizeof(ProxyHop))) return NULL;
    ProxyHop *slot = &cfg->proxy_chain[cfg->proxy_chain_len++];
    memset(slot, 0, sizeof *slot);
    return slot;
}

PortForwardSpec *ssh_config_push_local_forward(SSHConfig *cfg) {
    if (!cfg || cfg->local_forward_count < 0 ||
        cfg->local_forward_count == INT32_MAX) return NULL;
    if (!grow_array((void **)&cfg->local_forwards, &cfg->local_forward_cap,
                    cfg->local_forward_count + 1, sizeof(PortForwardSpec))) return NULL;
    PortForwardSpec *slot = &cfg->local_forwards[cfg->local_forward_count++];
    memset(slot, 0, sizeof *slot);
    return slot;
}

PortForwardSpec *ssh_config_push_remote_forward(SSHConfig *cfg) {
    if (!cfg || cfg->remote_forward_count < 0 ||
        cfg->remote_forward_count == INT32_MAX) return NULL;
    if (!grow_array((void **)&cfg->remote_forwards, &cfg->remote_forward_cap,
                    cfg->remote_forward_count + 1, sizeof(PortForwardSpec))) return NULL;
    PortForwardSpec *slot = &cfg->remote_forwards[cfg->remote_forward_count++];
    memset(slot, 0, sizeof *slot);
    return slot;
}

SSHEnvVar *ssh_config_push_env(SSHConfig *cfg) {
    if (!cfg || cfg->env_count < 0 || cfg->env_count == INT32_MAX) return NULL;
    if (!grow_array((void **)&cfg->env, &cfg->env_cap,
                    cfg->env_count + 1, sizeof(SSHEnvVar))) return NULL;
    SSHEnvVar *slot = &cfg->env[cfg->env_count++];
    memset(slot, 0, sizeof *slot);
    return slot;
}

bool ssh_config_clone(SSHConfig *dst, const SSHConfig *src) {
    if (!dst) return false;
    if (dst == src) return true;
    /* Free any existing arrays in dst before overwriting. */
    ssh_config_dispose(dst);
    if (!src) return true;
    if (src->stream_forward_count < 0 ||
        src->stream_forward_count > 4 ||
        src->proxy_chain_len < 0 || src->proxy_chain_len > MAX_PROXY_HOPS ||
        src->local_forward_count < 0 || src->remote_forward_count < 0 ||
        src->local_forward_count > MAX_PORT_FORWARDS ||
        src->remote_forward_count > MAX_PORT_FORWARDS ||
        src->env_count < 0 || src->env_count > SSH_CONFIG_MAX_ENV_VARS ||
        src->key_pem_len > SSH_CONFIG_MAX_KEY_PEM ||
        src->auth_method < AUTH_PASSWORD || src->auth_method > AUTH_GSSAPI ||
        (src->stream_forward_count > 0 && !src->stream_forwards) ||
        (src->proxy_chain_len > 0 && !src->proxy_chain) ||
        (src->local_forward_count > 0 && !src->local_forwards) ||
        (src->remote_forward_count > 0 && !src->remote_forwards) ||
        (src->env_count > 0 && !src->env) ||
        (src->key_pem_len > 0 && !src->key_pem)) {
        return false;
    }
    if (!ssh_config_strings_valid(src)) return false;

    /* Scalar copy: covers fixed-size strings, counts and the pointer fields
     * overwritten below. The dynamic-pointer fields are scratched
     * immediately so an early return cannot leave dst aliasing src. */
    *dst = *src;
    dst->stream_forwards = NULL; dst->stream_forward_cap = 0;
    dst->proxy_chain     = NULL; dst->proxy_chain_cap = 0;
    dst->local_forwards  = NULL; dst->local_forward_cap = 0;
    dst->remote_forwards = NULL; dst->remote_forward_cap = 0;
    dst->env             = NULL; dst->env_cap = 0;
    dst->key_pem         = NULL; dst->key_pem_len = 0;

    if (src->stream_forward_count > 0) {
        if (!clone_array((void **)&dst->stream_forwards, src->stream_forwards,
                         src->stream_forward_count,
                         sizeof(*dst->stream_forwards))) goto fail;
        dst->stream_forward_cap = src->stream_forward_count;
    }
    if (src->proxy_chain_len > 0) {
        if (!clone_array((void **)&dst->proxy_chain, src->proxy_chain,
                         src->proxy_chain_len,
                         sizeof(*dst->proxy_chain))) goto fail;
        dst->proxy_chain_cap = src->proxy_chain_len;
    }
    if (src->local_forward_count > 0) {
        if (!clone_array((void **)&dst->local_forwards, src->local_forwards,
                         src->local_forward_count,
                         sizeof(*dst->local_forwards))) goto fail;
        dst->local_forward_cap = src->local_forward_count;
    }
    if (src->remote_forward_count > 0) {
        if (!clone_array((void **)&dst->remote_forwards, src->remote_forwards,
                         src->remote_forward_count,
                         sizeof(*dst->remote_forwards))) goto fail;
        dst->remote_forward_cap = src->remote_forward_count;
    }
    if (src->env_count > 0) {
        if (!clone_array((void **)&dst->env, src->env, src->env_count,
                         sizeof(*dst->env))) goto fail;
        dst->env_cap = src->env_count;
    }
    if (src->key_pem && src->key_pem_len > 0) {
        dst->key_pem = malloc(src->key_pem_len);
        if (!dst->key_pem) goto fail;
        memcpy(dst->key_pem, src->key_pem, src->key_pem_len);
        dst->key_pem_len = src->key_pem_len;
    }
    return true;

fail:
    ssh_config_dispose(dst);
    return false;
}

void ssh_config_dispose(SSHConfig *cfg) {
    if (!cfg) return;
    /* Scrub credentials before releasing memory. */
    secure_zero(cfg->password,       sizeof cfg->password);
    secure_zero(cfg->key_passphrase, sizeof cfg->key_passphrase);
    secure_zero(cfg->jump_password,  sizeof cfg->jump_password);
    if (cfg->key_pem) {
        secure_zero(cfg->key_pem, cfg->key_pem_len);
        free(cfg->key_pem);
    }
    if (cfg->env) {
        for (i32 i = 0; i < cfg->env_count; i++) {
            secure_zero(cfg->env[i].value, sizeof cfg->env[i].value);
        }
        free(cfg->env);
    }
    free(cfg->stream_forwards);
    free(cfg->proxy_chain);
    free(cfg->local_forwards);
    free(cfg->remote_forwards);
    memset(cfg, 0, sizeof *cfg);
}

/* =========================================================================
 * X11 forwarding relay
 * ========================================================================= */

/* Connect to the local X11 display. Parses $DISPLAY to determine whether
 * to use a Unix socket (/tmp/.X11-unix/X<n>) or TCP (127.0.0.1:6000+n). */
static int x11_connect_display(void) {
    const char *display = getenv("DISPLAY");
    if (!display || !display[0]) return -1;

    int screen = 0;

    /* The path form XQuartz gives GUI apps, "/private/tmp/com.apple.launchd.XXXX/org.xquartz:0":
     * everything before the last ':' IS the socket. Without this a Finder-launched Liu fell through
     * to TCP port 6000, which XQuartz leaves closed. */
    {
        char upath[sizeof(((struct sockaddr_un *)0)->sun_path)];
        if (x11_display_socket_path(display, upath, sizeof upath)) {
            int ufd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (ufd < 0) return -1;
            struct sockaddr_un uaddr;
            memset(&uaddr, 0, sizeof uaddr);
            uaddr.sun_family = AF_UNIX;
            snprintf(uaddr.sun_path, sizeof uaddr.sun_path, "%s", upath);
            if (connect(ufd, (struct sockaddr *)&uaddr, sizeof uaddr) < 0 || !net_set_nonblocking(ufd)) {
                close(ufd);
                return -1;
            }
            return ufd;
        }
    }

    /* Unix socket path: $DISPLAY = :N or :N.S */
    if (display[0] == ':') {
        const char *end;
        if (!checked_parse_i32_prefix(display + 1, &screen, &end) ||
            screen < 0 || (*end != '\0' && *end != '.')) return -1;
        char sock_path[128];
        snprintf(sock_path, sizeof(sock_path), "/tmp/.X11-unix/X%d", screen);

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            /* Fall through to TCP */
        } else {
            if (net_set_nonblocking(fd)) return fd;
            close(fd);
        }
    }

    /* TCP: hostname:N or localhost:N -- connect to 127.0.0.1:6000+N */
    {
        const char *colon = strrchr(display, ':');
        if (colon) {
            const char *end;
            if (!checked_parse_i32_prefix(colon + 1, &screen, &end) ||
                screen < 0 || (*end != '\0' && *end != '.')) return -1;
        }
        if (screen > 65535 - 6000) return -1;
        int port = 6000 + screen;
        int fd = net_tcp_connect("127.0.0.1", port);
        if (fd >= 0 && !net_set_nonblocking(fd)) {
            close(fd);
            fd = -1;
        }
        return fd;
    }
}

/* The cookie the LOCAL X server expects, from the user's Xauthority file ($XAUTHORITY, else
 * ~/.Xauthority) — what the relay substitutes for the fake one so forwarding works with the
 * server's access control ON (ssh/x11_auth.h). false = nothing found, and nothing is substituted. */
static bool x11_load_real_cookie(u8 out[X11_COOKIE_LEN]) {
    char num[8];
    if (!x11_display_number(getenv("DISPLAY"), num, sizeof num)) return false;
    char path[1024];
    const char *xa = getenv("XAUTHORITY");
    const char *home = getenv("HOME");
    if (xa && xa[0]) snprintf(path, sizeof path, "%s", xa);
    else if (home && home[0]) snprintf(path, sizeof path, "%s/.Xauthority", home);
    else return false;
    char host[256] = "";
    if (gethostname(host, sizeof host) != 0) host[0] = '\0';
    host[sizeof host - 1] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    enum { XAUTH_MAX = 256 * 1024 };
    u8 *buf = malloc(XAUTH_MAX);
    bool found = false;
    if (buf) {
        usize n = fread(buf, 1, XAUTH_MAX, f);
        found = x11_xauth_find(buf, n, host, num, out);
        secure_zero(buf, XAUTH_MAX);       /* it held every cookie the user has */
        free(buf);
    }
    fclose(f);
    return found;
}

static f64 x11_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

/* Advance one unauthenticated X11 channel. true = keep it (waiting, or now relaying),
 * false = close it: wrong or missing cookie, the local display is unreachable, the
 * channel died, or it sat idle past X11_AUTH_TIMEOUT_SEC (sixteen idle connections
 * from another user on the server would otherwise hold every slot). Reads exactly
 * the bytes the request still needs, so nothing after it is consumed here. */
static bool x11_auth_step(Session *s, X11Relay *xr) {
    if (!s->x11_cookie_set) return false;
    if (x11_now() - xr->accepted_at > X11_AUTH_TIMEOUT_SEC) {
        fprintf(stderr, "X11 connection closed: no authentication within %.0f s\n", X11_AUTH_TIMEOUT_SEC);
        return false;
    }
    usize need = 0;
    X11AuthResult r = x11_setup_check(xr->setup, xr->setup_len, s->x11_cookie, &need);
    while (r == X11_AUTH_NEED_MORE) {
        ssize_t n = libssh2_channel_read(xr->relay.channel, (char *)xr->setup + xr->setup_len, need);
        if (n == LIBSSH2_ERROR_EAGAIN) return true;
        if (n <= 0) return false;
        xr->setup_len += (usize)n;
        r = x11_setup_check(xr->setup, xr->setup_len, s->x11_cookie, &need);
    }
    if (r != X11_AUTH_OK) {
        fprintf(stderr, "X11 connection refused: missing or wrong authentication cookie\n");
        return false;
    }
    int fd = x11_connect_display();
    if (fd < 0) return false;
    /* The channel proved it knows the FAKE cookie; the local server wants the REAL one. Swap it
     * in (same length, so the request is otherwise untouched). Not found: forward as it came. */
    {
        u8 real[X11_COOKIE_LEN];
        if (x11_load_real_cookie(real)) {
            memcpy(xr->setup + X11_HEADER_LEN + 20, real, X11_COOKIE_LEN);
            secure_zero(real, sizeof real);
        }
    }
    /* Hand the request on as the first thing the relay sends the local server. */
    memcpy(xr->relay.to_fd, xr->setup, X11_SETUP_LEN);
    xr->relay.to_fd_off = 0;
    xr->relay.to_fd_len = X11_SETUP_LEN;
    xr->relay.fd = fd;
    xr->authed = true;
    secure_zero(xr->setup, sizeof xr->setup);
    xr->setup_len = 0;
    return true;
}

static void session_x11_poll_locked(Session *s) {
    if (!s || s->type != SESSION_SSH ||
        session_status(s) != SESSION_CONNECTED) return;
    if (!s->ssh_session || !s->config || !s->config->forward_x11) return;

    /* Accept any pending X11 channels delivered via the callback */
    for (;;) {
        LIBSSH2_CHANNEL *x11ch = x11_pending_take(s);
        if (!x11ch) break;

        if (!s->x11_relays) {
            s->x11_relays = calloc(MAX_X11_CHANNELS, sizeof(*s->x11_relays));
            if (!s->x11_relays) {
                libssh2_channel_close(x11ch);
                libssh2_channel_free(x11ch);
                continue;
            }
        }
        i32 slot = -1;
        for (i32 i = 0; i < s->x11_relay_count; i++) {
            if (!s->x11_relays[i].relay.active) { slot = i; break; }
        }
        if (slot < 0 && s->x11_relay_count < MAX_X11_CHANNELS)
            slot = s->x11_relay_count++;
        if (slot < 0) {
            libssh2_channel_close(x11ch);
            libssh2_channel_free(x11ch);
            continue;
        }

        /* No local connection yet: x11_auth_step() opens it once this channel's first
         * packet has proven it knows the cookie. */
        X11Relay *relay = &s->x11_relays[slot];
        memset(relay, 0, sizeof(*relay));
        relay->relay.channel = x11ch;
        relay->relay.fd = -1;
        relay->relay.active = true;
        relay->accepted_at = x11_now();
    }

    /* Relay data bidirectionally for all active X11 channels */
    if (!s->x11_relays) return;
    for (i32 i = 0; i < s->x11_relay_count; i++) {
        X11Relay *xr = &s->x11_relays[i];
        if (!xr->relay.active) continue;
        if (!xr->authed) {
            if (!x11_auth_step(s, xr)) { socket_channel_relay_close(&xr->relay); continue; }
            /* Proven: the setup goes to the local server in this same pass —
             * nothing else is sure to wake the loop for it. */
            if (!xr->authed) continue;
        }
        if (!socket_channel_relay_poll(&xr->relay)) socket_channel_relay_close(&xr->relay);
        else socket_channel_relay_note_more(&xr->relay);
    }
}

void session_x11_poll(Session *s) {
    if (!s || s->type != SESSION_SSH ||
        session_status(s) != SESSION_CONNECTED || !s->ssh_session) return;
    if (!session_io_trylock(s)) return;
    session_x11_poll_locked(s);
    session_io_unlock(s);
}

/* =========================================================================
 * Agent forwarding relay — the local half
 * ========================================================================= */

#ifdef LIBSSH2_CALLBACK_AUTHAGENT
/* A connection to the agent the user's environment names. Local, so it
 * answers at once or not at all: this runs on the main thread. */
static int agent_connect_local(void) {
    const char *path = getenv("SSH_AUTH_SOCK");
    struct sockaddr_un addr;
    if (!path || !path[0] || strlen(path) >= sizeof addr.sun_path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    /* Not for a pane's child to inherit: it would hold the user's agent open. */
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || !net_set_nonblocking(fd)) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Runs under the connection's io lock, for ANY session on the connection: the
 * relays live with the session that owns it (the channels are its libssh2
 * session's, and the server does not say which pane's shell a program belongs
 * to), so a pane borrowing the connection serves them too, and they go on being
 * served after the owner's pane has been closed. */
static void session_agent_poll_locked(Session *s) {
    Session *o = ssh_connection_owner(s);
    if (!o || !o->ssh_session || !atomic_load_explicit(&o->agent_active, memory_order_acquire)) return;

    /* Take the channels the server opened since the last poll. */
    for (;;) {
        LIBSSH2_CHANNEL *ch = agent_pending_take(o);
        if (!ch) break;

        if (!o->agent_relays) {
            o->agent_relays = calloc(MAX_AGENT_CHANNELS, sizeof *o->agent_relays);
            if (!o->agent_relays) {
                libssh2_channel_close(ch);
                libssh2_channel_free(ch);
                continue;
            }
        }
        i32 slot = -1;
        for (i32 i = 0; i < o->agent_relay_count; i++) {
            if (!o->agent_relays[i].active) { slot = i; break; }
        }
        if (slot < 0 && o->agent_relay_count < MAX_AGENT_CHANNELS)
            slot = o->agent_relay_count++;
        int fd = slot >= 0 ? agent_connect_local() : -1;
        if (fd < 0) {
            /* No free slot, or the agent is not there: the remote program is
             * told its connection was closed, which is what a dead agent is. */
            libssh2_channel_close(ch);
            libssh2_channel_free(ch);
            continue;
        }
        SocketChannelRelay *r = &o->agent_relays[slot];
        memset(r, 0, sizeof *r);
        r->channel = ch;
        r->fd = fd;
        r->active = true;
    }

    if (!o->agent_relays) return;
    for (i32 i = 0; i < o->agent_relay_count; i++) {
        SocketChannelRelay *r = &o->agent_relays[i];
        if (!r->active) continue;
        if (!socket_channel_relay_poll(r)) socket_channel_relay_close(r);
        else socket_channel_relay_note_more(r);
    }
}
#endif /* LIBSSH2_CALLBACK_AUTHAGENT */

/* =========================================================================
 * Session pool (ControlMaster)
 * ========================================================================= */

void session_pool_register(Session *s) {
    /* Only a session that owns its connection and asked for sharing. */
    if (!s || s->type != SESSION_SSH || !s->is_shared_owner || s->pane_retired ||
        !session_config_shares(s->config)) return;
    if (!session_pool_init_once()) return;

    pthread_mutex_lock(&g_session_pool.lock);
    /* Avoid duplicates */
    for (i32 i = 0; i < g_session_pool.count; i++) {
        if (g_session_pool.sessions[i] == s) {
            pthread_mutex_unlock(&g_session_pool.lock);
            return;
        }
    }
    if (g_session_pool.count < SESSION_POOL_MAX) {
        g_session_pool.sessions[g_session_pool.count++] = s;
    }
    pthread_mutex_unlock(&g_session_pool.lock);
}

void session_pool_unregister(Session *s) {
    if (!s || !g_session_pool.initialized) return;

    pthread_mutex_lock(&g_session_pool.lock);
    for (i32 i = 0; i < g_session_pool.count; i++) {
        if (g_session_pool.sessions[i] == s) {
            /* Swap with last and shrink */
            g_session_pool.sessions[i] = g_session_pool.sessions[--g_session_pool.count];
            break;
        }
    }
    pthread_mutex_unlock(&g_session_pool.lock);
}

/* =========================================================================
 * StreamLocal (Unix socket forwarding)
 * ========================================================================= */

#ifndef PLATFORM_WIN32

/* Create a listening Unix domain socket at the given path.
 * Refuses to unlink anything except a socket owned by this uid. */
static int create_unix_listener(const char *path) {
    struct sockaddr_un addr;
    if (!path || !path[0] || strlen(path) >= sizeof(addr.sun_path)) return -1;

    struct stat old;
    if (lstat(path, &old) == 0) {
        if (!S_ISSOCK(old.st_mode) || old.st_uid != geteuid() || unlink(path) != 0)
            return -1;
    } else if (errno != ENOENT) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (chmod(path, 0600) != 0 || listen(fd, 4) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    /* Non-blocking for the accept loop */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    return fd;
}

/* A direct-streamlocal@openssh.com channel to `socket_path` on the server,
 * opened through libssh2_channel_open_ex with the request built in
 * ssh/streamlocal_open.h — on every libssh2, where the ready-made call exists
 * only from 1.11.0 (the Linux release builds against 1.10). Non-blocking:
 * NULL with `*would_block` set means "ask again on the next poll". */
static LIBSSH2_CHANNEL *streamlocal_channel_open(LIBSSH2_SESSION *session,
                                                 const char *socket_path,
                                                 bool *would_block) {
    *would_block = false;
    u8 req[STREAMLOCAL_OPEN_PAYLOAD_LEN(sizeof ((StreamLocalRuntime *)0)->remote_socket)];
    u32 req_len = streamlocal_open_payload(req, sizeof req, socket_path);
    if (req_len == 0) return NULL;
    LIBSSH2_CHANNEL *ch = libssh2_channel_open_ex(
        session, STREAMLOCAL_CHANNEL_TYPE, STREAMLOCAL_CHANNEL_TYPE_LEN,
        LIBSSH2_CHANNEL_WINDOW_DEFAULT, LIBSSH2_CHANNEL_PACKET_DEFAULT,
        (const char *)req, req_len);
    *would_block = !ch && libssh2_session_last_errno(session) == LIBSSH2_ERROR_EAGAIN;
    return ch;
}

static void session_streamlocal_poll(Session *s) {
    if (!s || !s->stream_fwd_active || !s->stream_fwds || !s->ssh_session)
        return;
    /* libssh2 keeps ONE channel open in progress per session, and the next
     * libssh2_channel_open_ex continues it whatever it is asked for: while one
     * rule's connection waits for its channel, no other rule takes one. */
    StreamLocalRuntime *opening = NULL;
    for (i32 i = 0; i < 4; i++)
        if (s->stream_fwds[i].pending_fd >= 0) { opening = &s->stream_fwds[i]; break; }

    for (i32 i = 0; i < 4; i++) {
        StreamLocalRuntime *rt = &s->stream_fwds[i];
        if (rt->listen_fd < 0) continue;
        if (rt->relay.active && !socket_channel_relay_poll(&rt->relay))
            socket_channel_relay_close(&rt->relay);
        if (rt->relay.active) {                /* one connection per rule at a time */
            socket_channel_relay_note_more(&rt->relay);
            continue;
        }

        if (rt->pending_fd < 0) {
            if (opening) continue;
            int client = accept(rt->listen_fd, NULL, NULL);
            if (client < 0) continue;
            int flags = fcntl(client, F_GETFL, 0);
            if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) != 0) {
                close(client);
                continue;
            }
            flags = fcntl(client, F_GETFD, 0);
            if (flags < 0 || fcntl(client, F_SETFD, flags | FD_CLOEXEC) != 0) {
                close(client);
                continue;
            }
#ifdef SO_NOSIGPIPE
            int one = 1;
            (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            rt->pending_fd = client;
            opening = rt;
        }

        bool would_block = false;
        LIBSSH2_CHANNEL *ch = streamlocal_channel_open(s->ssh_session, rt->remote_socket,
                                                       &would_block);
        StreamlocalOpenStep step = streamlocal_open_step(ch != NULL, would_block);
        if (step == STREAMLOCAL_OPEN_WAIT) continue;
        int client = rt->pending_fd;
        rt->pending_fd = -1;
        opening = NULL;
        if (step == STREAMLOCAL_OPEN_FAILED) { close(client); continue; }
        libssh2_channel_set_blocking(ch, 0);
        memset(&rt->relay, 0, sizeof rt->relay);
        rt->relay.channel = ch;
        rt->relay.fd = client;
        rt->relay.active = true;
        /* Moved along at once. The server answers an open with the confirmation,
         * the service's first bytes and often its EOF back to back, and the pass
         * that sees the confirmation may be the one that has already read the
         * rest off the socket (the pane's read comes first): no descriptor
         * announces what libssh2 now holds, so a connection left for the next
         * pass would wait until something else woke the loop. */
        if (!socket_channel_relay_poll(&rt->relay)) socket_channel_relay_close(&rt->relay);
        else socket_channel_relay_note_more(&rt->relay);
    }
}

#endif /* !PLATFORM_WIN32 */

void session_start_stream_forwards(Session *s) {
    if (!s || s->type != SESSION_SSH || !s->config) return;
    if (session_status(s) != SESSION_CONNECTED) return;
    if (s->config->stream_forward_count <= 0) return;
    if (!s->config->stream_forwards) return;
    if (s->stream_fwd_active) return;

#ifndef PLATFORM_WIN32
    StreamLocalRuntime *runtimes = calloc(4, sizeof(*runtimes));
    if (!runtimes) return;
    for (i32 i = 0; i < 4; i++) {
        runtimes[i].listen_fd = -1;
        runtimes[i].pending_fd = -1;
        runtimes[i].relay.fd = -1;
    }

    for (i32 i = 0; i < s->config->stream_forward_count && i < 4; i++) {
        const StreamLocalForward *fwd = &s->config->stream_forwards[i];
        usize local_len = strnlen(fwd->local_socket, sizeof(fwd->local_socket));
        usize remote_len = strnlen(fwd->remote_socket, sizeof(fwd->remote_socket));
        if (local_len == 0 || local_len == sizeof(fwd->local_socket) ||
            remote_len == 0 || remote_len == sizeof(fwd->remote_socket)) continue;

        if (fwd->is_local_to_remote) {
            /* Listen on local Unix socket, forward to remote */
            StreamLocalRuntime *rt = &runtimes[i];
            if (local_len >= sizeof(rt->local_socket) ||
                remote_len >= sizeof(rt->remote_socket)) continue;
            memcpy(rt->local_socket, fwd->local_socket, local_len + 1);
            memcpy(rt->remote_socket, fwd->remote_socket, remote_len + 1);

            int fd = create_unix_listener(rt->local_socket);
            if (fd < 0) continue;
            rt->listen_fd = fd;
            platform_watch_socket(fd);    /* a connection wakes the loop */
            s->stream_fwd_count++;
        }
        /* Remote->local forwarding requires libssh2_channel_forward_listen
         * on the remote side, which is more complex and currently not
         * implemented in this initial version. */
    }

    if (s->stream_fwd_count == 0) {
        free(runtimes);
        return;
    }
    s->stream_fwds = runtimes;
    s->stream_fwd_active = true;
#endif
}

void session_stop_stream_forwards(Session *s) {
    if (!s) return;
    s->stream_fwd_active = false;

#ifndef PLATFORM_WIN32
    if (s->stream_fwds) {
        for (i32 i = 0; i < 4; i++) {
            StreamLocalRuntime *rt = &s->stream_fwds[i];
            socket_channel_relay_close(&rt->relay);
            if (rt->pending_fd >= 0) {
                close(rt->pending_fd);
                rt->pending_fd = -1;
            }
            if (rt->listen_fd >= 0) {
                platform_unwatch_socket(rt->listen_fd);
                close(rt->listen_fd);
                rt->listen_fd = -1;
            }
            if (rt->local_socket[0]) {
                struct stat st;
                if (lstat(rt->local_socket, &st) == 0 && S_ISSOCK(st.st_mode) &&
                    st.st_uid == geteuid())
                    (void)unlink(rt->local_socket);
            }
        }
        free(s->stream_fwds);
        s->stream_fwds = NULL;
    }
#endif
    s->stream_fwd_count = 0;
}

/* =========================================================================
 * Passphrase prompt API
 * ========================================================================= */

bool session_needs_passphrase(const Session *s) {
    if (!s || s->type != SESSION_SSH) return false;
    Session *sm = (Session *)s;
    pthread_mutex_lock(&sm->status_lock);
    bool pending = sm->passphrase_pending;
    pthread_mutex_unlock(&sm->status_lock);
    return pending;
}

const char *session_passphrase_key_path(const Session *s) {
    if (!s || s->type != SESSION_SSH) return "";
    return s->passphrase_prompt_path;
}

void session_supply_passphrase(Session *s, const char *passphrase) {
    if (!s || s->type != SESSION_SSH || !s->config) return;
    pthread_mutex_lock(&s->status_lock);
    secure_zero(s->config->key_passphrase, sizeof(s->config->key_passphrase));
    snprintf(s->config->key_passphrase, sizeof(s->config->key_passphrase),
             "%s", passphrase ? passphrase : "");
    s->passphrase_supplied = true;
    pthread_cond_signal(&s->passphrase_cond);
    pthread_mutex_unlock(&s->status_lock);
}

void session_cancel_passphrase(Session *s) {
    if (!s || s->type != SESSION_SSH) return;
    pthread_mutex_lock(&s->status_lock);
    s->passphrase_cancelled = true;
    pthread_cond_signal(&s->passphrase_cond);
    pthread_mutex_unlock(&s->status_lock);
}
