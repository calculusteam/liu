/* updater.h — in-app auto-updater.
 *
 * Checks a JSON feed (LIU_UPDATE_FEED_URL), and on a newer version downloads a
 * zip-of-.app artifact, verifies its Ed25519 signature against the embedded
 * public key (src/core/update_pubkey.h), then atomically replaces the running
 * .app and relaunches.
 *
 * Threading: the platform shim (updater_macos.m) does all network I/O on a
 * background queue and hands results back through the _Atomic fields below; ALL
 * parsing / decisions / frees happen on the main thread in updater_tick(). The
 * C logic (updater.c) never touches Cocoa; the .m shim never parses JSON or
 * touches crypto. Decoupled from AppState — the UI mirrors UpdateState and sets
 * request flags (see settings panel), the way Create-Theme already works.
 */
#ifndef LIU_UPDATER_H
#define LIU_UPDATER_H

#include "core/types.h"
#include <stdatomic.h>
#include <stddef.h>

typedef enum {
    UPD_IDLE = 0,
    UPD_CHECKING,     /* fetching the feed */
    UPD_UPTODATE,     /* feed fetched, no newer version */
    UPD_AVAILABLE,    /* newer version found, awaiting user "Install" */
    UPD_DOWNLOADING,  /* downloading the artifact */
    UPD_VERIFYING,    /* hashing + signature check (brief, main thread) */
    UPD_INSTALLING,   /* extract + swap + relaunch */
    UPD_RELAUNCHING,  /* relaunch helper spawned; app should quit */
    UPD_ERROR,        /* see err[] */
} UpdatePhase;

typedef struct UpdateState {
    _Atomic int  phase;           /* UpdatePhase; written by .m + main */

    /* Background-op handoff (writer: .m completion; reader: main tick). */
    _Atomic bool io_done;         /* a fetch/download finished */
    _Atomic bool io_ok;
    char        *feed_body;       /* malloc'd feed JSON from the fetch (.m owns alloc, main frees) */
    _Atomic long feed_len;
    _Atomic long long bytes_done; /* download progress */
    _Atomic long long bytes_total;
    char         io_err[200];     /* error string set by .m on failure (read on main) */
    void        *io_task;         /* opaque, retained NSTask* (curl) for cancel (.m) */

    /* Parsed "available update" (filled on the main thread from the feed). */
    char  avail_version[64];
    char  avail_url[1024];
    char  avail_notes[1024];
    char  avail_sha256[80];
    char  avail_sig_b64[256];
    char  avail_min_os[32];
    long long avail_size;

    char  downloaded_path[1024];  /* temp zip path (set by .m on download done) */

    /* UI-facing strings. */
    char  status[200];            /* human status line */
    char  err[200];               /* error message when phase==UPD_ERROR */

    /* Behavior flags. */
    bool  auto_install_allowed;   /* false for dev/bare-binary runs → "Open Releases" */
    bool  silent;                 /* background autocheck → toast, not UI */
    bool  relaunch_requested;     /* main loop should quit so the helper can relaunch */

    /* Toast handoff for the silent path (main.c reads + clears, calls app_show_toast). */
    char  toast_msg[160];
    bool  toast_pending;

    bool  initialized;
    double last_check_epoch;
} UpdateState;

/* --- lifecycle / control (updater.c, called on the main thread) --- */
void updater_init(UpdateState *st);
void updater_begin_check(UpdateState *st, bool silent);   /* user (silent=false) or autocheck */
void updater_maybe_autocheck(UpdateState *st);            /* throttled (24h) silent check */
void updater_begin_install(UpdateState *st);              /* user clicked Install & Relaunch */
void updater_cancel(UpdateState *st);                     /* abort + cleanup */
void updater_tick(UpdateState *st);                       /* per-frame; advances the state machine */
const char *updater_current_version(void);               /* "0.1.0" (LIU_VERSION) */

/* --- semver (pure, unit-tested) --- */
int  liu_semver_cmp(const char *a, const char *b);        /* <0 / 0 / >0 */
bool liu_update_is_newer(const char *feed_ver, const char *cur_ver);

/* --- platform shim (updater_macos.m on Apple, updater_stub.c elsewhere) --- */
const char *updater_plat_arch(void);                      /* "arm64" / "x86_64" */
bool updater_plat_os_at_least(const char *min_os);        /* running OS >= min_os */
bool updater_plat_can_autoinstall(void);                  /* proper .app, not from build dir */
void updater_plat_fetch_feed(UpdateState *st, const char *url);            /* async */
void updater_plat_download(UpdateState *st, const char *url, long long sz);/* async */
void updater_plat_cancel(UpdateState *st);
/* Synchronous, fast, main-thread install helpers (called after verification): */
bool updater_plat_extract(const char *zip_path, char *out_app_path, size_t cap);
bool updater_plat_bundle_version(const char *app_path, char *out, size_t cap);
void updater_plat_strip_quarantine(const char *path);
bool updater_plat_install(const char *new_app_path, char *out_installed, size_t cap);
void updater_plat_relaunch_and_quit(const char *installed_app_path);
void updater_plat_cleanup_temp(const char *path);          /* rm -rf a temp dir/file */

#endif /* LIU_UPDATER_H */
