/*
 * Liu - Sites (dev-server manager) — backend runtime.
 * See sites.h for the design overview.
 */
#include "core/sites.h"
#include "core/mem_budget.h"
#include "core/pty_reader.h"
#include "core/checked.h"
#include "terminal/terminal.h"
#include "platform/platform.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>

#define SITE_DRAIN_CAP      (1 << 18)   /* 256 KiB per site per poll */
#define SITE_KILL_GRACE_SEC 3.0         /* SIGTERM → SIGKILL escalation window */
#define SITE_ALIVE_CHECK_SEC 0.1       /* match normal terminal liveness cadence */

/* Scrollback a site's log terminal is trimmed down to once the run has ended.
 * A stopped site keeps its log readable (the failure output is what the user
 * comes back for) but has no reason to hold the user's FULL scrollback — at a
 * 20k-line setting that is ~3.4 MB of retained cells per stopped site. The
 * live limit is restored on the next start/restart. */
#define SITE_IDLE_SCROLLBACK 500

/* ------------------------------------------------------------------------- */

static void site_teardown_now(Site *s);  /* defined below; used by load/remove */

static void site_killpg(i32 pid, int sig) {
    if (pid > 0) killpg((pid_t)pid, sig);
}

/* Append raw PTY bytes to the rolling failure tail, stripping ANSI escape
 * sequences so the failure panel shows plain, readable error text. The strip
 * state persists across calls because a CSI/OSC sequence can straddle a read. */
static bool site_scan_port(const u8 *buf, i32 len, i32 *out, bool *tls);

static void site_log_tail_append(Site *s, const u8 *data, i32 n) {
    enum { ST_NORM, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC };
    const i32 cap = (i32)sizeof s->log_tail;
    for (i32 i = 0; i < n; i++) {
        u8 c = data[i];
        switch (s->log_tail_ansi) {
        case ST_ESC:
            if      (c == '[') s->log_tail_ansi = ST_CSI;
            else if (c == ']') s->log_tail_ansi = ST_OSC;
            else               s->log_tail_ansi = ST_NORM;  /* 2-byte/charset esc */
            continue;
        case ST_CSI:
            if (c >= 0x40 && c <= 0x7e) s->log_tail_ansi = ST_NORM;  /* final byte */
            continue;
        case ST_OSC:
            if      (c == 0x07) s->log_tail_ansi = ST_NORM;     /* BEL terminator */
            else if (c == 0x1b) s->log_tail_ansi = ST_OSC_ESC;  /* ST = ESC \     */
            continue;
        case ST_OSC_ESC:
            s->log_tail_ansi = ST_NORM;                         /* consume the \  */
            continue;
        default: break;
        }
        /* ST_NORM */
        if (c == 0x1b) { s->log_tail_ansi = ST_ESC; continue; }
        u8 out;
        if      (c == '\n') out = '\n';
        else if (c == '\r') continue;            /* fold CR into the LF */
        else if (c == '\t') out = ' ';
        else if (c <  0x20) continue;            /* drop other C0 controls */
        else                out = c;             /* printable ASCII / UTF-8 byte */
        if (s->log_tail_len >= cap - 1) {        /* keep the most recent bytes */
            /* A read longer than the tail would push its URL out before the
             * scan after it: look at what is here before it goes. */
            if (s->detected_port == 0)
                (void)site_scan_port((const u8 *)s->log_tail, s->log_tail_len,
                                     &s->detected_port, &s->detected_tls);
            i32 drop = (cap - 1) / 4;
            memmove(s->log_tail, s->log_tail + drop, (usize)(s->log_tail_len - drop));
            s->log_tail_len -= drop;
        }
        s->log_tail[s->log_tail_len++] = (char)out;
    }
    s->log_tail[s->log_tail_len] = '\0';
}

static void site_log_tail_reset(Site *s) {
    s->log_tail_len = 0;
    s->log_tail_ansi = 0;
    s->log_tail[0] = '\0';
}

/* Find an "http(s)://host:PORT" URL in the run's log tail — the output with its
 * escape sequences taken out (site_log_tail_append) — and report the port, and
 * whether the scheme was https. Vite/Next/CRA all print such a URL when the
 * server is ready, and Vite prints its port BOLD inside a cyan URL
 * (`http://localhost:\e[1m5173\e[22m/`): read raw, the ':' is followed by an
 * escape, not a digit. The tail is also what carries a URL split across two reads
 * (its stripper keeps its state between them), and a port is taken only once
 * something follows its digits — "http://localhost:51" at the end of a read may
 * be 5173 in the next.
 *
 * Only http and https, as a word of their own: the Node inspector's
 * `ws://127.0.0.1:9229`, a database URL or `xhttps://` printed first are not the
 * site. Six digits are no port. A bracketed IPv6 host ("http://[::1]:5173") is
 * read to its ']' over the characters an address has, 64 at most: walking to any
 * ']' would make `://[://[…` (a request log can carry it) quadratic in the tail.
 */
static bool site_scan_scheme(const u8 *buf, i32 at, bool *tls) {
    i32 n = 0;                               /* letters before "://" */
    while (n < at && n < 6 && (((buf[at - 1 - n] | 0x20) >= 'a' && (buf[at - 1 - n] | 0x20) <= 'z'))) n++;
    if (n != 4 && n != 5) return false;
    i32 b = at - n;
    if (b > 0) {                             /* a word of its own */
        u8 c = buf[b - 1];
        if ((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ||
            c == '+' || c == '-' || c == '.') return false;
    }
    static const char http[] = "https";
    for (i32 k = 0; k < n; k++) if ((buf[b + k] | 0x20) != http[k]) return false;
    *tls = n == 5;
    return true;
}

static bool site_scan_port(const u8 *buf, i32 len, i32 *out, bool *tls) {
    for (i32 i = 0; i + 2 < len; i++) {
        if (buf[i] != ':' || buf[i + 1] != '/' || buf[i + 2] != '/') continue;
        bool https = false;
        if (!site_scan_scheme(buf, i, &https)) continue;
        i32 j = i + 3;                       /* host starts after "://" */
        if (j < len && buf[j] == '[') {
            i32 stop = j + 64;
            j++;
            while (j < len && j < stop) {
                u8 c = buf[j];
                bool addr = (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') ||
                            c == ':' || c == '.' || c == '%';
                if (!addr) break;
                j++;
            }
            if (j >= len) return false;      /* the address may go on in the next read */
            if (buf[j] != ']') continue;
            j++;
        }
        while (j < len) {
            u8 c = buf[j];
            if (c == ':' || c == '/' || c == ' ' ||
                c == '\r' || c == '\n' || c == '\t') break;
            j++;
        }
        if (j < len && buf[j] == ':') {
            i32 k = j + 1, v = 0, d = 0;
            while (k < len && buf[k] >= '0' && buf[k] <= '9' && d < 6) {
                v = v * 10 + (buf[k] - '0'); k++; d++;
            }
            if (k >= len) return false;      /* the port may go on in the next read */
            if (d >= 1 && d <= 5 && v >= 1 && v <= 65535) {
                *out = v;
                *tls = https;
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */

void sites_manager_init(SiteManager *m, i32 log_cols, i32 log_rows, i32 scrollback) {
    if (!m) return;
    memset(m, 0, sizeof *m);
    m->log_cols    = log_cols  > 0 ? log_cols  : 120;
    m->log_rows    = log_rows  > 0 ? log_rows  : 40;
    m->scrollback  = scrollback > 0 ? scrollback : 5000;
}

/* One place a Site record is created and one place it is released, because
 * there are THREE call sites for each and the ledger has to see all of them:
 * a missed one leaves a window's sites unreported, or the level permanently
 * high after a shutdown.
 *
 * What is charged is sizeof(Site) alone: a site's log Terminal and scrollback
 * are reported by ui.c's per-frame traversal, and its PTY reader ring by
 * pty_reader_start. */
static Site *site_record_new(void) {
    Site *s = (Site *)calloc(1, sizeof(Site));
    if (s) mem_budget_adjust(MEM_SITES, (isize)sizeof(Site));
    return s;
}

static void site_record_free(Site *s) {
    if (!s) return;
    site_teardown_now(s);
    free(s);
    mem_budget_adjust(MEM_SITES, -(isize)sizeof(Site));
}

void sites_manager_load_from_config(SiteManager *m, const AppConfig *cfg) {
    if (!m || !cfg) return;
    /* Drop any previously allocated entries (safe on a fresh init — count is 0). */
    for (i32 i = 0; i < m->count; i++) {
        site_record_free(m->sites[i]);
        m->sites[i] = NULL;
    }
    m->count = 0;
    for (i32 i = 0; i < cfg->site_count && i < MAX_SITES; i++) {
        const SiteConfig *sc = &cfg->sites[i];
        Site *s = site_record_new();
        if (!s) break;
        snprintf(s->name,    sizeof s->name,    "%s", sc->name);
        snprintf(s->path,    sizeof s->path,    "%s", sc->path);
        snprintf(s->command, sizeof s->command, "%s", sc->command);
        s->port   = sc->port;
        s->status = SITE_STOPPED;
        m->sites[m->count++] = s;
    }
}

void sites_manager_merge_from_config(SiteManager *m, const AppConfig *cfg) {
    if (!m || !cfg) return;
    i32 n = cfg->site_count < 0 ? 0 : (cfg->site_count > MAX_SITES ? MAX_SITES : cfg->site_count);
    bool used[MAX_SITES] = {0};
    for (i32 i = 0; i < m->count; ) {
        Site *s = m->sites[i];
        i32 hit = -1;
        for (i32 j = 0; s && j < n && hit < 0; j++)
            if (!used[j] && strcmp(cfg->sites[j].path, s->path) == 0) hit = j;
        if (hit >= 0) {
            const SiteConfig *sc = &cfg->sites[hit];
            used[hit] = true;
            snprintf(s->name,    sizeof s->name,    "%s", sc->name);
            snprintf(s->command, sizeof s->command, "%s", sc->command);
            s->port = sc->port;
            i++;
        } else if (s && s->session) {
            i++;                          /* running here: kept */
        } else {
            sites_remove(m, i);
        }
    }
    for (i32 j = 0; j < n; j++) {
        if (used[j]) continue;
        const SiteConfig *sc = &cfg->sites[j];
        Site *s = sites_add(m, sc->name, sc->path, sc->command);
        if (!s) break;
        s->port = sc->port;
    }
}

void sites_manager_sync_to_config(const SiteManager *m, AppConfig *cfg) {
    if (!m || !cfg) return;
    i32 n = m->count > MAX_SITES ? MAX_SITES : m->count;
    for (i32 i = 0; i < n; i++) {
        const Site *s = m->sites[i];
        if (!s) continue;
        SiteConfig *sc = &cfg->sites[i];
        snprintf(sc->name,    sizeof sc->name,    "%s", s->name);
        snprintf(sc->path,    sizeof sc->path,    "%s", s->path);
        snprintf(sc->command, sizeof sc->command, "%s", s->command);
        sc->port = s->port;
    }
    cfg->site_count = n;
}

Site *sites_get(SiteManager *m, i32 idx) {
    if (!m || idx < 0 || idx >= m->count) return NULL;
    return m->sites[idx];
}

i32 sites_running_count(const SiteManager *m) {
    if (!m) return 0;
    i32 n = 0;
    for (i32 i = 0; i < m->count; i++) {
        const Site *s = m->sites[i];
        if (s && (s->status == SITE_RUNNING || s->status == SITE_STARTING)) n++;
    }
    return n;
}

void sites_terminal_memory(const SiteManager *m, usize *scrollback_bytes,
                           usize *grid_bytes) {
    usize sb = 0, grid = 0;
    if (m) {
        for (i32 i = 0; i < m->count; i++) {
            const Site *s = m->sites[i];
            if (!s || !s->term) continue;
            usize next;
            usize one_sb = terminal_scrollback_bytes(s->term);
            usize one_grid = terminal_grid_bytes(s->term);
            sb = checked_add_usize(sb, one_sb, &next) ? next : (usize)-1;
            grid = checked_add_usize(grid, one_grid, &next) ? next : (usize)-1;
        }
    }
    if (scrollback_bytes) *scrollback_bytes = sb;
    if (grid_bytes) *grid_bytes = grid;
}

Site *sites_add(SiteManager *m, const char *name, const char *path, const char *command) {
    if (!m || m->count >= MAX_SITES) return NULL;
    Site *s = site_record_new();
    if (!s) return NULL;
    snprintf(s->name,    sizeof s->name,    "%s", name    ? name    : "");
    snprintf(s->path,    sizeof s->path,    "%s", path    ? path    : "");
    snprintf(s->command, sizeof s->command, "%s", command ? command : "");
    s->status = SITE_STOPPED;
    m->sites[m->count++] = s;
    return s;
}

/* Tear down a site's live process + log terminal immediately (used by remove
 * and shutdown — no graceful grace window, the caller is dropping the site). */
static void site_teardown_now(Site *s) {
    if (s->session) {
        site_killpg(s->process_group > 0 ? s->process_group :
                    session_child_pid(s->session), SIGTERM);
        if (s->watch_registered) {
            platform_unwatch_socket(s->watched_fd);
            s->watch_registered = false;
        }
        session_destroy(s->session);   /* SIGHUP fallback + closes PTY */
        s->session = NULL;
    }
    if (s->term) {
        terminal_destroy(s->term);
        s->term = NULL;
    }
}

bool sites_remove(SiteManager *m, i32 idx) {
    if (!m || idx < 0 || idx >= m->count) return false;
    site_record_free(m->sites[idx]);
    for (i32 i = idx; i < m->count - 1; i++) m->sites[i] = m->sites[i + 1];
    m->count--;
    m->sites[m->count] = NULL;
    return true;
}

void sites_manager_shutdown(SiteManager *m) {
    if (!m) return;
    for (i32 i = 0; i < m->count; i++) {
        site_record_free(m->sites[i]);
        m->sites[i] = NULL;
    }
    m->count = 0;
}

bool site_start(SiteManager *m, Site *s) {
    if (!m || !s) return false;
    if (s->session) return true;   /* already running/starting */
    if (!s->command[0]) { s->status = SITE_FAILED; s->exit_code = -1; return false; }

    site_log_tail_reset(s);        /* this run's output starts fresh */
    s->process_group = 0;
    s->draining_exit = false;
    s->output_eof_seen = false;
    s->kill_sent = false;
    s->next_alive_check = 0.0;

    if (!s->term) {
        s->term = terminal_create(m->log_cols, m->log_rows);
        if (s->term && m->scrollback > 0)
            terminal_set_scrollback_limit(s->term, m->scrollback);
    } else {
        /* Restore the live scrollback the reap trimmed away — this run gets
         * the user's full setting again. */
        if (m->scrollback > 0 && s->term->scrollback_limit < m->scrollback)
            terminal_set_scrollback_limit(s->term, m->scrollback);
        /* Visually delimit a restart in the retained log. */
        static const char sep[] = "\r\n\x1b[2m── restart ──\x1b[0m\r\n";
        terminal_feed(s->term, (const u8 *)sep, sizeof sep - 1);
    }

    s->session = session_create_command(m->log_cols, m->log_rows, NULL, 0,
                                        s->path[0] ? s->path : NULL, s->command);
    if (!s->session || session_status(s->session) != SESSION_CONNECTED) {
        const char *err = s->session ? session_error(s->session) : "out of memory";
        char msg[400];
        snprintf(msg, sizeof msg,
                 "\r\n\x1b[31mFailed to start: %s\x1b[0m\r\n", err ? err : "");
        if (s->term) terminal_feed(s->term, (const u8 *)msg, strlen(msg));
        site_log_tail_append(s, (const u8 *)msg, (i32)strlen(msg));
        if (s->session) { session_destroy(s->session); s->session = NULL; }
        s->status = SITE_FAILED;
        s->exit_code = -1;
        return false;
    }

    s->process_group = session_child_pid(s->session);
    int fd = session_io_fd(s->session);
    if (fd >= 0) {
        platform_watch_socket(fd);
        s->watched_fd = fd;
        s->watch_registered = true;
    }
    s->status = SITE_STARTING;
    s->started_at = platform_time_sec();
    s->stop_requested_at = 0.0;
    s->detected_port = 0;
    s->detected_tls = false;
    s->address_told = false;
    s->exit_code = 0;
    return true;
}

void site_stop(Site *s) {
    if (!s || !s->session) return;
    /* forkpty() setsid'd the child, so pgid == child_pid: kill the whole
     * group to take down node together with its esbuild/vite workers. */
    if (s->stop_requested_at == 0.0) {
        /* The leader may already be reaped while a descendant still owns
         * the PTY. A restart must be able to terminate that producer too. */
        site_killpg(s->process_group > 0 ? s->process_group :
                    session_child_pid(s->session), SIGTERM);
        /* Zero is the "not stopping" sentinel, including in clock-model tests. */
        f64 now = platform_time_sec();
        s->stop_requested_at = now > 0.0 ? now : 1e-9;
    }
    s->next_alive_check = 0.0;
}

bool site_restart(SiteManager *m, Site *s) {
    if (!m || !s) return false;
    if (s->session) {
        s->pending_restart = true;
        site_stop(s);
        return true;
    }
    return site_start(m, s);
}

void site_resize_log(Site *s, i32 cols, i32 rows) {
    if (!s || !s->term || cols < 1 || rows < 1) return;
    /* Only act on a real geometry change — site_resize_log runs every frame
     * the panel is open, and an unconditional session_resize would spam
     * SIGWINCH to the dev server. */
    if (s->term->cols == cols && s->term->rows == rows) return;
    terminal_resize(s->term, cols, rows);
    if (s->session) session_resize(s->session, cols, rows);
}

i32 site_effective_port(const Site *s) {
    if (!s) return 0;
    if (s->detected_port) return s->detected_port;
    return s->port;
}

bool site_address(const Site *s, char *out, usize cap) {
    if (out && cap) out[0] = '\0';
    i32 port = site_effective_port(s);
    if (port <= 0 || !out || cap == 0) return false;
    bool tls = s->detected_port && s->detected_tls;
    snprintf(out, cap, "%s://localhost:%d", tls ? "https" : "http", (int)port);
    return true;
}

bool site_take_address(Site *s, char *out, usize cap) {
    if (out && cap) out[0] = '\0';
    if (!s || !s->session || s->address_told || s->detected_port <= 0 ||
        (s->status != SITE_STARTING && s->status != SITE_RUNNING))
        return false;
    if (!site_address(s, out, cap)) return false;
    s->address_told = true;
    return true;
}

/* Reap a finished run: capture exit code, drop the session, decide the next
 * state (a user stop → STOPPED, a clean exit → EXITED, otherwise FAILED),
 * honoring a pending restart. The log terminal is kept so output stays
 * visible. */
static void site_finalize_exit(SiteManager *m, Site *s) {
    int code = -1;
    session_exited(s->session, &code);
    if (s->watch_registered) {
        platform_unwatch_socket(s->watched_fd);
        s->watch_registered = false;
    }
    session_destroy(s->session);
    s->session = NULL;

    bool was_user_stop = (s->stop_requested_at != 0.0);
    s->stop_requested_at = 0.0;
    s->process_group = 0;
    s->draining_exit = false;
    s->output_eof_seen = false;
    s->kill_sent = false;
    s->exit_code = code;

    if (s->pending_restart) {
        s->pending_restart = false;
        site_start(m, s);
        return;
    }
    if (was_user_stop)  s->status = SITE_STOPPED;
    else if (code == 0) s->status = SITE_EXITED;
    else                s->status = SITE_FAILED;

    /* The run is over and nothing will append to this log again until a
     * restart, so drop it to a tail instead of pinning the user's full
     * scrollback for as long as the window lives. Trim only — the log stays
     * browsable, and site_start raises the limit again on restart. Never
     * raises: a user scrollback below the idle cap is left alone. */
    if (s->term && s->term->scrollback_limit > SITE_IDLE_SCROLLBACK)
        terminal_set_scrollback_limit(s->term, SITE_IDLE_SCROLLBACK);
}

/* session_read() preserves reader-ring bytes after reap, but its direct-PTY
 * path is gated on SESSION_CONNECTED. Sites own only LOCAL command sessions:
 * while draining a reaped, non-threaded session, the still-registered borrowed
 * fd is the PTY master. Read it without the status gate, and retain the watch
 * and Session until EOF. EAGAIN/EINTR are NOT EOF. Never read a reader-owned fd
 * here: session_io_fd() returns -1 for it, so no watch was registered. */
static i32 site_read_output(Site *s, u8 *buf, i32 want, bool *eof) {
    *eof = false;
    if (s->draining_exit && s->watch_registered) {
        ssize_t n = read(s->watched_fd, buf, (usize)want);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        *eof = n <= 0;  /* PTY EOF is 0 on macOS, commonly EIO on Linux. */
        return n < 0 ? -1 : (i32)n;
    }
    i32 n = session_read(s->session, buf, want);
    /* The reader's EOF acquire must precede this final pending check: bytes
     * can be published between its initial head load and its EOF load. */
    *eof = n < 0 && session_read_pending(s->session) == 0;
    return n;
}

bool sites_poll(SiteManager *m, PollLedger *ledger) {
    if (!m) return false;
    bool got = false;
    f64 now = platform_time_sec();
    u8 buf[16384];

    u64 turn = ledger ? poll_rotation_child_turn(ledger->turn, ledger->rotation_windows)
                      : m->poll_turn++;
    for (i32 step = 0; step < m->count; step++) {
        i32 i = poll_rotating_index(step, m->count, turn);
        Site *s = m->sites[i];
        if (!s || !s->session) continue;
        bool was_draining = s->draining_exit;
        bool eof = false;
        bool new_eof = false;

        /* A site is never "the focused pane" — it has no cursor in it — so it
         * always draws from the shared pool rather than the exemption. */
        i32 cap = ledger ? poll_ledger_take(ledger, false) : SITE_DRAIN_CAP;
        if (cap > SITE_DRAIN_CAP) cap = SITE_DRAIN_CAP;
        i32 total = 0;
        f64 pane_seconds = 0;
        bool time_yield = false;
        while (total < cap) {
            i32 want = ledger ? poll_ledger_read_size(ledger, false, total, pane_seconds)
                              : (i32)sizeof buf;
            if (want == 0) { time_yield = true; break; }
            if (cap - total < want) want = cap - total;
            f64 chunk_start = platform_time_sec();
            i32 n = site_read_output(s, buf, want, &eof);
            if (n <= 0) {
                new_eof = eof && !s->output_eof_seen;
                if (eof) s->output_eof_seen = true;
                break;
            }
            s->output_eof_seen = false;
            if (s->term) terminal_feed(s->term, buf, (usize)n);
            site_log_tail_append(s, buf, n);
            /* The URL is looked for in the tail, not in the read: escapes taken
             * out, and a URL split across reads whole (site_scan_port). At most
             * 2 KB a read, and only until the server has said where it is. */
            if (s->detected_port == 0)
                (void)site_scan_port((const u8 *)s->log_tail, s->log_tail_len,
                                     &s->detected_port, &s->detected_tls);
            f64 elapsed = platform_time_sec() - chunk_start;
            pane_seconds += elapsed;
            poll_ledger_work(ledger, elapsed);
            total += n;
            got = true;
        }
        bool limited = time_yield || total >= cap;
        i32 pending = limited ? session_read_pending(s->session) : 0;
        if (pending > 0) pty_reader_note_backlog();
        if (ledger) poll_ledger_spend(ledger, total, limited, pending);
        if (s->status == SITE_STARTING && total > 0) s->status = SITE_RUNNING;

        /* Signal escalation must not wait for read budget or the alive timer.
         * Repeated stop/restart clicks neither restart the grace window nor
         * turn SIGKILL into a per-frame system call. */
        bool killed = false;
        if (!s->kill_sent && s->stop_requested_at != 0.0 &&
            (now - s->stop_requested_at) > SITE_KILL_GRACE_SEC) {
            site_killpg(s->process_group > 0 ? s->process_group :
                        session_child_pid(s->session), SIGKILL);
            s->kill_sent = true;
            killed = true;
        }

        if (!s->draining_exit &&
            (new_eof || killed || now >= s->next_alive_check)) {
            s->next_alive_check = now + SITE_ALIVE_CHECK_SEC;
            /* waitpid can be interrupted without changing the session state.
             * Do not enter the drain state on that transient false return. */
            if (!session_is_alive(s->session) &&
                session_status(s->session) != SESSION_CONNECTED) {
                s->draining_exit = true;
                got = true;
            }
        }

        /* Reaping is not proof that the ring/kernel queue is empty. All final
         * reads use the SAME loop, byte ledger and time budget on later turns.
         * Even an empty read just before reap is insufficient: the child could
         * write its last bytes between that read and waitpid. Require a fresh
         * EOF observation after entering the draining state. */
        if (was_draining && eof && session_read_pending(s->session) == 0) {
            f64 finish_start = platform_time_sec();
            site_finalize_exit(m, s);
            poll_ledger_work(ledger, platform_time_sec() - finish_start);
            got = true;
        }
    }
    return got;
}

/* ------------------------------------------------------------------------- */

static const char *cj_str(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

bool sites_detect_package_json(const char *path, SiteDetect *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);

    /* Default name = last path segment; default command = npm run dev. */
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++) if (*p == '/' && p[1]) base = p + 1;
    snprintf(out->name, sizeof out->name, "%s", base);
    snprintf(out->dev_cmd, sizeof out->dev_cmd, "npm run dev");
    if (!path || !path[0]) return false;

    char file[1100];
    int file_n = snprintf(file, sizeof file, "%s/package.json", path);
    if (file_n < 0 || (usize)file_n >= sizeof file) return false;
    FILE *f = fopen(file, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 4 * 1024 * 1024 ||
        fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    char *data = malloc((usize)sz + 1);
    if (!data) { fclose(f); return false; }
    usize rd = fread(data, 1, (usize)sz, f);
    bool read_ok = rd == (usize)sz && !ferror(f);
    fclose(f);
    if (!read_ok) { free(data); return false; }
    data[rd] = '\0';

    cJSON *root = cJSON_ParseWithLength(data, rd);
    free(data);
    if (!root) return false;
    out->has_package_json = true;

    const char *nm = cj_str(root, "name");
    if (nm && nm[0]) snprintf(out->name, sizeof out->name, "%s", nm);

    cJSON *scripts = cJSON_GetObjectItemCaseSensitive(root, "scripts");
    if (cJSON_IsObject(scripts)) {
        for (cJSON *it = scripts->child; it && out->script_count < 8; it = it->next)
            if (it->string)
                snprintf(out->scripts[out->script_count++], 48, "%s", it->string);

        const char *pref[] = { "dev", "start", "serve", "develop" };
        for (i32 k = 0; k < 4; k++) {
            if (cJSON_GetObjectItemCaseSensitive(scripts, pref[k])) {
                snprintf(out->dev_cmd, sizeof out->dev_cmd, "npm run %s", pref[k]);
                break;
            }
        }
    }
    cJSON_Delete(root);
    return true;
}
