/*
 * Liu - Sites (dev-server manager)
 *
 * Backend runtime for the Herd-style Sites panel: registers project folders,
 * runs each one's dev command (e.g. `npm run dev`) as a managed background
 * process via session_create_command(), and captures the output into a
 * headless Terminal so the UI can render full-color logs with scrollback.
 *
 * UI-free: this layer owns the process + the headless Terminal and knows
 * nothing about the renderer. The UI (src/ui/sites_ui.c) drives it and renders
 * each site's Terminal grid via the existing render_terminal_pane().
 */
#ifndef CORE_SITES_H
#define CORE_SITES_H

#include "core/types.h"
#include "core/config.h"        /* SiteConfig, AppConfig, MAX_SITES */
#include "core/poll_budget.h"   /* PollLedger — sites share the process drain budget */
#include "ssh/ssh_session.h"    /* Session */

/* Headless log terminal — full definition pulled in by sites.c only. */
typedef struct Terminal Terminal;

typedef enum {
    SITE_STOPPED = 0,
    SITE_STARTING,   /* spawned, no output yet */
    SITE_RUNNING,    /* spawned and producing output */
    SITE_EXITED,     /* clean exit (code 0) */
    SITE_FAILED,     /* nonzero exit / spawn failure */
} SiteStatus;

typedef struct {
    /* ---- persisted (mirrors SiteConfig) ---- */
    char        name[64];
    char        path[1024];
    char        command[512];
    i32         port;             /* user-pinned port hint, 0 = auto */

    /* ---- live (never persisted) ---- */
    Session    *session;          /* NULL when stopped */
    Terminal   *term;             /* headless log terminal; kept after exit */
    SiteStatus  status;
    i32         exit_code;        /* valid when EXITED/FAILED */
    i32         detected_port;    /* scanned from output, 0 until seen */
    bool        detected_tls;     /* the URL it was read from said https:// */
    bool        address_told;     /* this run's address was announced (site_take_address) */
    f64         started_at;       /* platform_time_sec() of last start */
    f64         stop_requested_at;/* >0 while a graceful stop is in flight */
    int         watched_fd;       /* fd registered with platform_watch_socket */
    bool        watch_registered;
    bool        pending_restart;  /* restart once the current run finalizes */
    i32         process_group;   /* retain pgid after waitpid clears the child pid */
    bool        draining_exit;    /* child reaped; retain output until EOF */
    bool        output_eof_seen;  /* only the EOF transition bypasses the timer */
    bool        kill_sent;        /* send the grace-window SIGKILL only once */
    f64         next_alive_check; /* idle liveness checks are time-, not frame-based */

    /* Rolling ANSI-stripped tail of this run's output. Kept so the failure
     * panel can show WHY a run died even when the dev tool used the alternate
     * screen (which the live log terminal restores to blank on exit), and it
     * is where the server's URL is looked for: escapes out (Vite bolds its
     * port inside the URL), a URL split across reads whole. Reset on each
     * (re)start. */
    char        log_tail[2048];
    i32         log_tail_len;
    u8          log_tail_ansi;    /* ANSI-strip state machine, spans reads */
} Site;

typedef struct SiteManager {
    /* On-demand heap sites: unused slots are NULL (~8 B each). A full Site is
     * ~3.8 KB (path/command/log_tail), so embedding MAX_SITES of them would
     * add ~120 KB to AppState even when no sites are registered. */
    Site *sites[MAX_SITES];
    i32   count;
    i32   log_cols, log_rows;     /* geometry for newly-created log terminals */
    i32   scrollback;             /* scrollback limit for log terminals */
    u64   poll_turn;              /* rotation fallback for callers without a ledger */
} SiteManager;

/* Detection result for an "Add folder" pick. */
typedef struct {
    char name[64];
    char dev_cmd[512];            /* suggested command, e.g. "npm run dev" */
    bool has_package_json;
    char scripts[8][48];          /* discovered npm script names */
    i32  script_count;
} SiteDetect;

/* Lifecycle of the manager. */
void  sites_manager_init(SiteManager *m, i32 log_cols, i32 log_rows, i32 scrollback);
void  sites_manager_shutdown(SiteManager *m);   /* stops all + frees terminals */

/* Registry ⇄ persisted config. */
void  sites_manager_load_from_config(SiteManager *m, const AppConfig *cfg);
/* Bring a manager in line with a list another window saved, without stopping
 * anything: a site the list has and the manager lacks is added (stopped), one
 * the list lacks is removed unless it is running here, and the ones both have
 * (by folder) take the list's name, command and port. */
void  sites_manager_merge_from_config(SiteManager *m, const AppConfig *cfg);
void  sites_manager_sync_to_config(const SiteManager *m, AppConfig *cfg);

/* In-memory registry CRUD (caller persists via config_save). */
Site *sites_add(SiteManager *m, const char *name, const char *path, const char *command);
bool  sites_remove(SiteManager *m, i32 idx);    /* stops first if running */
Site *sites_get(SiteManager *m, i32 idx);
i32   sites_running_count(const SiteManager *m);

/* Authoritative retained bytes for every headless Site log Terminal owned by
 * this manager. Running, stopped, exited and failed Sites all count: each may
 * retain a real grid and scrollback. */
void  sites_terminal_memory(const SiteManager *m, usize *scrollback_bytes,
                            usize *grid_bytes);

/* Lifecycle of a single site. */
bool  site_start(SiteManager *m, Site *s);
void  site_stop(Site *s);                       /* graceful tree-kill */
bool  site_restart(SiteManager *m, Site *s);
void  site_resize_log(Site *s, i32 cols, i32 rows);

/* Main-loop pump: drain output → log terminals, detect ports + exits.
 * Returns true on output or a lifecycle transition. Cheap when idle. */
/* Drain running site PTYs under the process-wide read budget so their output
 * shares the same pre-paint limit as terminal panes. */
bool  sites_poll(SiteManager *m, PollLedger *ledger);

/* Effective port for "open in browser": detected port, else user hint, else 0. */
i32   site_effective_port(const Site *s);

/* The site's address as Liu opens it — "http://localhost:<port>", https when
 * the server's own URL said so — from site_effective_port. The notice and
 * every "open in browser" read it here. False (and `out` empty) while no
 * port is known. */
bool  site_address(const Site *s, char *out, usize cap);

/* Once a run: true (and the address in `out`) when the server has printed its
 * URL, which is when the site is really there — a pinned port is only a hint
 * until then, and the server may have taken another. site_start resets it. */
bool  site_take_address(Site *s, char *out, usize cap);

/* Parse <path>/package.json for a name + dev script suggestion. Always fills
 * *out with sensible fallbacks (folder basename, "npm run dev"); returns true
 * when a package.json was found and parsed. */
bool  sites_detect_package_json(const char *path, SiteDetect *out);

#endif /* CORE_SITES_H */
