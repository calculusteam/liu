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
    f64         started_at;       /* platform_time_sec() of last start */
    f64         stop_requested_at;/* >0 while a graceful stop is in flight */
    int         watched_fd;       /* fd registered with platform_watch_socket */
    bool        watch_registered;
    bool        pending_restart;  /* restart once the current run finalizes */

    /* Rolling ANSI-stripped tail of this run's output. Kept so the failure
     * panel can show WHY a run died even when the dev tool used the alternate
     * screen (which the live log terminal restores to blank on exit). Reset on
     * each (re)start. */
    char        log_tail[2048];
    i32         log_tail_len;
    u8          log_tail_ansi;    /* ANSI-strip state machine, spans reads */

    /* Port-detection carry: the tail of the previous PTY read, prepended to the
     * next read's head so a "scheme://host:PORT" URL split across a session_read()
     * boundary is still matched. Reset on each (re)start (site_log_tail_reset). */
    u8          port_carry[96];
    i32         port_carry_len;
} Site;

typedef struct SiteManager {
    Site sites[MAX_SITES];
    i32  count;
    i32  log_cols, log_rows;      /* geometry for newly-created log terminals */
    i32  scrollback;              /* scrollback limit for log terminals */
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
void  sites_manager_sync_to_config(const SiteManager *m, AppConfig *cfg);

/* In-memory registry CRUD (caller persists via config_save). */
Site *sites_add(SiteManager *m, const char *name, const char *path, const char *command);
bool  sites_remove(SiteManager *m, i32 idx);    /* stops first if running */
Site *sites_get(SiteManager *m, i32 idx);
i32   sites_running_count(const SiteManager *m);

/* Lifecycle of a single site. */
bool  site_start(SiteManager *m, Site *s);
void  site_stop(Site *s);                       /* graceful tree-kill */
bool  site_restart(SiteManager *m, Site *s);
void  site_resize_log(Site *s, i32 cols, i32 rows);

/* Main-loop pump: drain output → log terminals, detect ports + exits.
 * Returns true if any site produced output this call. Cheap when idle. */
bool  sites_poll(SiteManager *m);

/* Effective port for "open in browser": detected port, else user hint, else 0. */
i32   site_effective_port(const Site *s);

/* Parse <path>/package.json for a name + dev script suggestion. Always fills
 * *out with sensible fallbacks (folder basename, "npm run dev"); returns true
 * when a package.json was found and parsed. */
bool  sites_detect_package_json(const char *path, SiteDetect *out);

#endif /* CORE_SITES_H */
