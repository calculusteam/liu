/*
 * Liu - Sites (dev-server manager) — backend runtime.
 * See sites.h for the design overview.
 */
#include "core/sites.h"
#include "terminal/terminal.h"
#include "platform/platform.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <sys/types.h>

#define SITE_DRAIN_CAP      (1 << 18)   /* 256 KiB per site per poll */
#define SITE_KILL_GRACE_SEC 3.0         /* SIGTERM → SIGKILL escalation window */

/* ------------------------------------------------------------------------- */

static void site_killpg(i32 pid, int sig) {
    if (pid > 0) killpg((pid_t)pid, sig);
}

/* Append raw PTY bytes to the rolling failure tail, stripping ANSI escape
 * sequences so the failure panel shows plain, readable error text. The strip
 * state persists across calls because a CSI/OSC sequence can straddle a read. */
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
    s->port_carry_len = 0;          /* fresh run: no boundary carry yet */
}

/* Scan a freshly-read log chunk for a "scheme://host:PORT" URL and report the
 * port. Matching on "://" then the next ':' avoids false positives from ANSI
 * SGR colour codes (which never contain "://"). Vite/Next/CRA all print such a
 * URL when the server is ready. */
static bool site_scan_port(const u8 *buf, i32 len, i32 *out) {
    for (i32 i = 0; i + 2 < len; i++) {
        if (buf[i] != ':' || buf[i + 1] != '/' || buf[i + 2] != '/') continue;
        i32 j = i + 3;                       /* host starts after "://" */
        while (j < len) {
            u8 c = buf[j];
            if (c == ':' || c == '/' || c == ' ' ||
                c == '\r' || c == '\n' || c == '\t') break;
            j++;
        }
        if (j < len && buf[j] == ':') {
            i32 k = j + 1, v = 0, d = 0;
            while (k < len && buf[k] >= '0' && buf[k] <= '9' && d < 5) {
                v = v * 10 + (buf[k] - '0'); k++; d++;
            }
            if (d >= 1 && d <= 5 && v >= 1 && v <= 65535) { *out = v; return true; }
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

void sites_manager_load_from_config(SiteManager *m, const AppConfig *cfg) {
    if (!m || !cfg) return;
    m->count = 0;
    for (i32 i = 0; i < cfg->site_count && i < MAX_SITES; i++) {
        const SiteConfig *sc = &cfg->sites[i];
        Site *s = &m->sites[m->count++];
        memset(s, 0, sizeof *s);
        snprintf(s->name,    sizeof s->name,    "%s", sc->name);
        snprintf(s->path,    sizeof s->path,    "%s", sc->path);
        snprintf(s->command, sizeof s->command, "%s", sc->command);
        s->port   = sc->port;
        s->status = SITE_STOPPED;
    }
}

void sites_manager_sync_to_config(const SiteManager *m, AppConfig *cfg) {
    if (!m || !cfg) return;
    i32 n = m->count > MAX_SITES ? MAX_SITES : m->count;
    for (i32 i = 0; i < n; i++) {
        const Site *s = &m->sites[i];
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
    return &m->sites[idx];
}

i32 sites_running_count(const SiteManager *m) {
    if (!m) return 0;
    i32 n = 0;
    for (i32 i = 0; i < m->count; i++)
        if (m->sites[i].status == SITE_RUNNING || m->sites[i].status == SITE_STARTING) n++;
    return n;
}

Site *sites_add(SiteManager *m, const char *name, const char *path, const char *command) {
    if (!m || m->count >= MAX_SITES) return NULL;
    Site *s = &m->sites[m->count];
    memset(s, 0, sizeof *s);
    snprintf(s->name,    sizeof s->name,    "%s", name    ? name    : "");
    snprintf(s->path,    sizeof s->path,    "%s", path    ? path    : "");
    snprintf(s->command, sizeof s->command, "%s", command ? command : "");
    s->status = SITE_STOPPED;
    m->count++;
    return s;
}

/* Tear down a site's live process + log terminal immediately (used by remove
 * and shutdown — no graceful grace window, the caller is dropping the site). */
static void site_teardown_now(Site *s) {
    if (s->session) {
        site_killpg(session_child_pid(s->session), SIGTERM);
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
    site_teardown_now(&m->sites[idx]);
    for (i32 i = idx; i < m->count - 1; i++) m->sites[i] = m->sites[i + 1];
    m->count--;
    memset(&m->sites[m->count], 0, sizeof(Site));
    return true;
}

void sites_manager_shutdown(SiteManager *m) {
    if (!m) return;
    for (i32 i = 0; i < m->count; i++) site_teardown_now(&m->sites[i]);
    m->count = 0;
}

bool site_start(SiteManager *m, Site *s) {
    if (!m || !s) return false;
    if (s->session) return true;   /* already running/starting */
    if (!s->command[0]) { s->status = SITE_FAILED; s->exit_code = -1; return false; }

    site_log_tail_reset(s);        /* this run's output starts fresh */

    if (!s->term) {
        s->term = terminal_create(m->log_cols, m->log_rows);
        if (s->term && m->scrollback > 0)
            terminal_set_scrollback_limit(s->term, m->scrollback);
    } else {
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
    s->exit_code = 0;
    return true;
}

void site_stop(Site *s) {
    if (!s || !s->session) return;
    /* forkpty() setsid'd the child, so pgid == child_pid: kill the whole
     * group to take down node together with its esbuild/vite workers. */
    site_killpg(session_child_pid(s->session), SIGTERM);
    s->stop_requested_at = platform_time_sec();
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
    s->exit_code = code;

    if (s->pending_restart) {
        s->pending_restart = false;
        site_start(m, s);
        return;
    }
    if (was_user_stop)  s->status = SITE_STOPPED;
    else if (code == 0) s->status = SITE_EXITED;
    else                s->status = SITE_FAILED;
}

bool sites_poll(SiteManager *m) {
    if (!m) return false;
    bool got = false;
    f64 now = platform_time_sec();
    u8 buf[16384];

    for (i32 i = 0; i < m->count; i++) {
        Site *s = &m->sites[i];
        if (!s->session) continue;

        i32 total = 0;
        while (total < SITE_DRAIN_CAP) {
            i32 n = session_read(s->session, buf, (i32)sizeof buf);
            if (n <= 0) break;
            if (s->term) terminal_feed(s->term, buf, (usize)n);
            site_log_tail_append(s, buf, n);
            if (s->detected_port == 0) {
                /* Scan this chunk; if a 'scheme://host:PORT' URL was split across
                 * the previous read boundary, also scan the junction formed by the
                 * prior read's tail (port_carry) + this read's head. */
                if (!site_scan_port(buf, n, &s->detected_port) && s->port_carry_len > 0) {
                    u8 j[sizeof s->port_carry + 96];
                    i32 cl = s->port_carry_len;
                    i32 hl = n < 96 ? n : 96;
                    memcpy(j, s->port_carry, (usize)cl);
                    memcpy(j + cl, buf, (usize)hl);
                    site_scan_port(j, cl + hl, &s->detected_port);
                }
                /* Carry forward the tail of THIS chunk for the next boundary. */
                i32 keep = n < (i32)sizeof s->port_carry ? n : (i32)sizeof s->port_carry;
                memcpy(s->port_carry, buf + (n - keep), (usize)keep);
                s->port_carry_len = keep;
            }
            total += n;
            got = true;
        }
        if (s->status == SITE_STARTING && total > 0) s->status = SITE_RUNNING;

        /* Escalate a stuck graceful stop to SIGKILL after the grace window. */
        if (s->stop_requested_at != 0.0 &&
            (now - s->stop_requested_at) > SITE_KILL_GRACE_SEC) {
            site_killpg(session_child_pid(s->session), SIGKILL);
        }

        if (!session_is_alive(s->session)) site_finalize_exit(m, s);
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
    snprintf(file, sizeof file, "%s/package.json", path);
    FILE *f = fopen(file, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return false; }
    char *data = malloc((usize)sz + 1);
    if (!data) { fclose(f); return false; }
    usize rd = fread(data, 1, (usize)sz, f);
    fclose(f);
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
