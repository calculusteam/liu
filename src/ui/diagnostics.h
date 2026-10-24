/*
 * Liu manages notification hooks in agent configuration files, updates the
 * hook shim path at launch and removes hooks at quit. Failures can leave
 * notifications unavailable or make an agent configuration unreadable.
 * Diagnostics checks hook paths and session stores and reports the affected
 * path and available remediation.
 *
 * Repairs are initiated by the user. diag_run only reads, stats and parses; it
 * does not modify files. History stores are checked through filesystem
 * metadata rather than the history parser, so diagnostics does not depend on
 * transcript parsing. The history check uses the same file extensions as the
 * session scanner and does not parse the records.
 *
 * If an issue cannot be repaired here, the report includes its path and the
 * action required from the user.
 */
#ifndef UI_DIAGNOSTICS_H
#define UI_DIAGNOSTICS_H

#include "core/types.h"

typedef enum {
    DIAG_OK = 0,     /* checked, nothing wrong */
    DIAG_INFO,       /* checked, nothing to do — the tool is simply not in use */
    DIAG_WARN,       /* works, but not the way Liu intends to leave it */
    DIAG_FAIL,       /* broken: something outside Liu is degraded right now */
    DIAG_STATUS_COUNT,
} DiagStatus;

typedef enum {
    DIAG_GROUP_HOOKS = 0,   /* what Liu writes into agent CLIs */
    DIAG_GROUP_HISTORY,     /* the on-disk stores Agent History reads */
    DIAG_GROUP_LIU,         /* Liu's own files */
    DIAG_GROUP_COUNT,
} DiagGroup;

/* The action a repair would perform. An enum keeps the report a plain value
 * and provides the label shown on the repair button. */
typedef enum {
    DIAG_FIX_NONE = 0,      /* no button; `remedy` tells the user what to do */
    DIAG_FIX_SHIM,          /* rewrite ~/.liu/hooks/liu-notify at the real binary */
    DIAG_FIX_HOOK_REINSTALL,/* uninstall + install this agent's hook */
    DIAG_FIX_HOOK_REMOVE,   /* strip Liu's entry and leave it out */
} DiagFixKind;

#define DIAG_MAX_ITEMS 64
#define DIAG_ID_CAP    40
#define DIAG_TITLE_CAP 88
#define DIAG_TEXT_CAP  320
#define DIAG_PATH_CAP  512

typedef struct {
    char        id[DIAG_ID_CAP];        /* Stable identifier for this diagnostic item. */
    char        title[DIAG_TITLE_CAP];  /* one line: what was checked */
    char        detail[DIAG_TEXT_CAP];  /* what was found */
    char        remedy[DIAG_TEXT_CAP];  /* what the USER must do; "" when fixable */
    char        path[DIAG_PATH_CAP];    /* the file this is about; "" if none */
    char        agent[32];              /* agent id for the hook fixes; "" otherwise */
    DiagGroup   group;
    DiagStatus  status;
    DiagFixKind fix;
    bool        fix_attempted;
    bool        fix_ok;
    char        fix_msg[DIAG_TEXT_CAP];
} DiagItem;

typedef struct {
    DiagItem items[DIAG_MAX_ITEMS];
    i32      count;
    i32      n_ok, n_info, n_warn, n_fail;
    i32      n_fixable;                 /* how many carry a working Fix button */
    bool     ran;
    char     ran_at[32];                /* wall clock, so a stale report says so */
} DiagReport;

/* Where this build's liu-notify sits — beside the running executable
 * (Contents/MacOS/liu-notify in a bundle, build/liu-notify in a dev tree),
 * falling back to the bare name for a PATH lookup when the executable path
 * cannot be read. Lives here rather than privately in main.c because the
 * hook installer and this panel must agree on the answer: a diagnostics
 * report about a different binary than the one the installer writes is worse
 * than no report. Returns false only when `out` is unusable. */
bool diag_notify_bin(char *out, usize cap);

/* Run every check. Read-only. `notify_bin` is the absolute path to the
 * liu-notify this build ships (NULL is allowed — the checks that need it then
 * report that they could not verify the target rather than guessing). */
void diag_run(DiagReport *r, const char *notify_bin);

/* Attempt one repair. Returns true on success; either way `fix_attempted`,
 * `fix_ok` and `fix_msg` are filled in, and `status` is refreshed by re-running
 * that single check so the row cannot claim a fix that did not take. */
bool diag_fix(DiagItem *it, const char *notify_bin);

/* Every fixable item, in order. Counts are written to the out params. Items
 * that fail keep their FAIL status and their `fix_msg` says why — this is the
 * path that produces the list of what could not be fixed. */
void diag_fix_all(DiagReport *r, const char *notify_bin,
                  i32 *out_fixed, i32 *out_failed);

/* Recompute n_ok, n_info, n_warn, n_fail and n_fixable from items. Called by
 * diag_run and after each fix. */
void diag_recount(DiagReport *r);

const char *diag_status_label(DiagStatus s);   /* "OK" / "Info" / "Warning" / "Problem" */
const char *diag_group_label(DiagGroup g);
const char *diag_fix_label(DiagFixKind k);     /* the button's verb, "" for NONE */

/* True when the report has something the user must act on themselves — a FAIL
 * or WARN with no repair. The panel badges on this, and it is the question
 * "did Liu break something it cannot undo" in one call. */
bool diag_needs_user(const DiagReport *r);

#endif /* UI_DIAGNOSTICS_H */
