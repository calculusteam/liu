/*
 * Liu — Settings › Diagnostics engine. See diagnostics.h for why this exists
 * and for the two constraints that shape it (repair is user-initiated; history
 * is checked through the filesystem, never through chat_scan).
 */
#include "ui/diagnostics.h"

#include "notify/agent_hooks.h"
#include "notify/hook_paths.h"
#include "notify/hook_scan.h"
#include "core/config.h"
#include "core/atomic_file.h"

#include "cJSON.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

bool diag_notify_bin(char *out, usize cap) {
    if (!out || cap < 4) return false;
#if defined(__APPLE__)
    char exe[1024]; u32 exe_sz = (u32)sizeof exe;
    if (_NSGetExecutablePath(exe, &exe_sz) == 0) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            int n = snprintf(out, cap, "%s/liu-notify", exe);
            if (n > 0 && (usize)n < cap) return true;
        }
    }
#elif defined(__linux__)
    /* Same requirement as the bundle path above: an agent spawns the hook with
     * whatever PATH it inherited, which for a desktop-launched agent rarely
     * includes a self-built or tarball-installed Liu. */
    char exe[1024];
    isize elen = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (elen > 0) {
        exe[elen] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            int n = snprintf(out, cap, "%s/liu-notify", exe);
            if (n > 0 && (usize)n < cap && access(out, X_OK) == 0) return true;
        }
    }
#endif
    int n = snprintf(out, cap, "liu-notify");
    return n > 0 && (usize)n < cap;
}

/* =========================================================================
 * Report building
 * ========================================================================= */

static DiagItem *diag_push(DiagReport *r, DiagGroup g, const char *id,
                           const char *title) {
    if (!r || r->count >= DIAG_MAX_ITEMS) return NULL;
    DiagItem *it = &r->items[r->count++];
    memset(it, 0, sizeof *it);
    it->group  = g;
    it->status = DIAG_OK;
    it->fix    = DIAG_FIX_NONE;
    snprintf(it->id,    sizeof it->id,    "%s", id ? id : "");
    snprintf(it->title, sizeof it->title, "%s", title ? title : "");
    return it;
}

static void diag_set(DiagItem *it, DiagStatus st, const char *detail) {
    if (!it) return;
    it->status = st;
    snprintf(it->detail, sizeof it->detail, "%s", detail ? detail : "");
}

static void diag_setf(DiagItem *it, DiagStatus st, const char *fmt, ...) {
    if (!it) return;
    it->status = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it->detail, sizeof it->detail, fmt, ap);
    va_end(ap);
}

void diag_recount(DiagReport *r) {
    if (!r) return;
    r->n_ok = r->n_info = r->n_warn = r->n_fail = r->n_fixable = 0;
    for (i32 i = 0; i < r->count; i++) {
        switch (r->items[i].status) {
        case DIAG_OK:   r->n_ok++;   break;
        case DIAG_INFO: r->n_info++; break;
        case DIAG_WARN: r->n_warn++; break;
        case DIAG_FAIL: r->n_fail++; break;
        default: break;
        }
        if (r->items[i].fix != DIAG_FIX_NONE &&
            (r->items[i].status == DIAG_WARN || r->items[i].status == DIAG_FAIL))
            r->n_fixable++;
    }
}

bool diag_needs_user(const DiagReport *r) {
    if (!r) return false;
    for (i32 i = 0; i < r->count; i++) {
        const DiagItem *it = &r->items[i];
        if ((it->status == DIAG_FAIL || it->status == DIAG_WARN) &&
            it->fix == DIAG_FIX_NONE)
            return true;
    }
    return false;
}

const char *diag_status_label(DiagStatus s) {
    switch (s) {
    case DIAG_OK:   return "OK";
    case DIAG_INFO: return "Info";
    case DIAG_WARN: return "Warning";
    case DIAG_FAIL: return "Problem";
    default:        return "?";
    }
}

const char *diag_group_label(DiagGroup g) {
    switch (g) {
    case DIAG_GROUP_HOOKS:   return "Agent CLI hooks";
    case DIAG_GROUP_HISTORY: return "Agent History stores";
    case DIAG_GROUP_LIU:     return "Liu's own files";
    default:                 return "Other";
    }
}

const char *diag_fix_label(DiagFixKind k) {
    switch (k) {
    case DIAG_FIX_SHIM:           return "Rewrite shim";
    case DIAG_FIX_HOOK_REINSTALL: return "Reinstall hook";
    case DIAG_FIX_HOOK_REMOVE:    return "Remove hook";
    default:                      return "";
    }
}

/* =========================================================================
 * Small filesystem helpers
 * ========================================================================= */

static bool path_exists(const char *p) { return p && *p && access(p, F_OK) == 0; }

static bool home_join(char *out, usize cap, const char *home, const char *rel) {
    return (usize)snprintf(out, cap, "%s/%s", home, rel) < cap;
}

/* Read a whole file, capped. Returns NULL when absent or too large — the
 * caller distinguishes those with access() first, because "absent" and
 * "unreadable" are different findings and must not collapse into one. */
static char *slurp(const char *path, usize cap_bytes) {
    FILE *f = atomic_file_open_regular(path);   /* a FIFO is refused, not waited on */
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || (usize)sz > cap_bytes) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((usize)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    usize got = fread(buf, 1, (usize)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* Bounded recursive count of regular files under `root`.
 *
 * Bounded on purpose: this runs on the UI thread when Run is pressed, and a
 * session store is user data with no size contract. The cap is reported rather
 * than silently applied, because "counting stopped here" and "there are this
 * many" are different answers. */
typedef struct {
    i32  files;        /* regular files seen */
    i32  empty;        /* zero-byte ones — a parser's usual first surprise */
    i32  unreadable;   /* present but access(R_OK) fails */
    bool capped;
    char first_bad[DIAG_PATH_CAP];
} DirCount;

#define DIAG_WALK_CAP 20000

/* Accept the file extensions handled by the history scanner. Ignore other files
 * in these roots because they are not session records. */
static bool walk_is_session_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcmp(dot, ".jsonl") || !strcmp(dot, ".json") || !strcmp(dot, ".db");
}

static void walk_count(const char *dir, i32 depth, DirCount *c) {
    if (depth < 0 || c->capped) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        /* Skip every hidden entry, not just . and .. — a store is full of
         * sidecars the scanner never opens (`.lock` files, cleanup markers),
         * and counting them made a healthy Grok store report "104 of 601
         * session files are empty". A diagnostic that cries wolf about normal
         * state is worse than one that says nothing. */
        if (de->d_name[0] == '.') continue;
        if (c->files >= DIAG_WALK_CAP) { c->capped = true; break; }

        char p[DIAG_PATH_CAP];
        if ((usize)snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= sizeof p) continue;

        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { walk_count(p, depth - 1, c); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        if (!walk_is_session_file(de->d_name)) continue;

        c->files++;
        if (st.st_size == 0) {
            c->empty++;
            if (!c->first_bad[0]) snprintf(c->first_bad, sizeof c->first_bad, "%s", p);
        } else if (access(p, R_OK) != 0) {
            c->unreadable++;
            if (!c->first_bad[0]) snprintf(c->first_bad, sizeof c->first_bad, "%s", p);
        }
    }
    closedir(d);
}

/* =========================================================================
 * Group 1 — the agent hook surfaces
 * ========================================================================= */

typedef struct {
    const char *agent;      /* agent_hook_* id; "" would mean Liu cannot install it */
    const char *label;      /* what the panel calls it */
    const char *rel;        /* config path under $HOME */
    bool        is_json;    /* a JSON file Liu splices into: parse-check it too */
    bool        shared;     /* True when the path contains shared user configuration that must not be overwritten if repair cannot merge it safely. */
} HookSurface;

/* Every surface hook_paths.h names, in the order doctor prints them. The
 * `shared` flag is the one that decides whether a broken file may be repaired:
 * grok, opencode, pi and omp are files Liu owns outright, the other six are
 * the user's and a merge failure there is reported, never overwritten.
 *
 * Every row carries its installer's id, so a stale hook on any surface is
 * repaired through that installer (agent_hook_uninstall) rather than left for
 * deletion by hand. */
static const HookSurface g_surfaces[] = {
    { "claude",      "Claude Code",  LIU_HOOK_CLAUDE_REL,      true,  true  },
    { "commandcode", "Command Code", LIU_HOOK_COMMANDCODE_REL, true,  true  },
    { "grok",        "Grok",         LIU_HOOK_GROK_REL,        true,  false },
    { "opencode",    "OpenCode",     LIU_HOOK_OPENCODE_REL,    false, false },
    { "codex",       "Codex",        LIU_HOOK_CODEX_REL,       false, true  },
    { "antigravity", "Antigravity",  LIU_HOOK_ANTIGRAVITY_REL, true,  true  },
    { "pi",          "Pi",           LIU_HOOK_PI_REL,          false, false },
    { "omp",         "Oh My Pi",     LIU_HOOK_OMP_REL,         false, false },
    { "devin",       "Devin",        LIU_HOOK_DEVIN_REL,       true,  true  },
    { "qoder",       "Qoder",        LIU_HOOK_QODER_REL,       true,  true  },
};
#define G_SURFACE_COUNT ((i32)(sizeof g_surfaces / sizeof g_surfaces[0]))

/* The shim: one file, and everything else in this group points at it. */
static void check_shim(DiagReport *r, const char *home, const char *notify_bin) {
    (void)home;
    DiagItem *it = diag_push(r, DIAG_GROUP_HOOKS, "shim",
                             "Hook shim — the path every agent records");
    if (!it) return;

    char shim[DIAG_PATH_CAP];
    if (!agent_hook_shim_path(shim, sizeof shim)) {
        diag_set(it, DIAG_FAIL, "Could not resolve $HOME, so the shim path is unknown.");
        snprintf(it->remedy, sizeof it->remedy,
                 "Launch Liu with a usable $HOME; nothing here can be checked without one.");
        return;
    }
    snprintf(it->path, sizeof it->path, "%s", shim);

    /* hook_scan_resolves, not access(): the binary and the shim's target may
     * be a bare NAME (the PATH-lookup fallback of diag_notify_bin), and a name
     * tested with access() is looked for in the current directory. */
    bool bin_ok = hook_scan_resolves(notify_bin);

    if (access(shim, X_OK) != 0) {
        /* Absent is not automatically wrong — it is the correct state after a
         * clean quit, which removes the hooks. It is only a fault when an agent
         * still records it, and that is the per-agent check's job. */
        diag_set(it, DIAG_INFO,
                 "Not present. That is the normal state when Liu is not running "
                 "or has quit cleanly.");
        if (bin_ok) { it->fix = DIAG_FIX_SHIM; }
        return;
    }

    char target[DIAG_PATH_CAP];
    if (!hook_scan_shim_target(shim, target, sizeof target)) {
        diag_setf(it, DIAG_WARN, "Present, but its target line could not be read.");
        if (bin_ok) it->fix = DIAG_FIX_SHIM;
        else snprintf(it->remedy, sizeof it->remedy,
                      "Liu cannot rewrite it: no usable liu-notify was found to point at.");
        return;
    }

    if (hook_scan_resolves(target)) {
        diag_setf(it, DIAG_OK, "Points at %s", target);
        return;
    }

    /* This is the failure the shim's own silence hides, and the one
     * `liu-notify doctor` passes: the recorded command resolves (the shim is
     * right there), so nothing looks stale, but the binary it execs is gone.
     * The shim then exits 0 without a word and every agent notification is
     * dropped — no error in Liu, none in the agent, none in the log. */
    diag_setf(it, DIAG_FAIL,
              "Its target is missing, so every agent notification is silently "
              "dropped: %s", target);
    if (bin_ok) {
        it->fix = DIAG_FIX_SHIM;
    } else {
        snprintf(it->remedy, sizeof it->remedy,
                 "No usable liu-notify to point at. Rebuild it (cmake --build build "
                 "--target liu-notify) or reinstall Liu, then run this again.");
    }
}

static void check_notify_bin(DiagReport *r, const char *notify_bin) {
    DiagItem *it = diag_push(r, DIAG_GROUP_HOOKS, "notify-bin",
                             "liu-notify binary");
    if (!it) return;
    if (!notify_bin || !*notify_bin) {
        diag_set(it, DIAG_FAIL, "Liu could not work out where its liu-notify should be.");
        snprintf(it->remedy, sizeof it->remedy,
                 "Reinstall Liu. Without this binary no agent notification can fire.");
        return;
    }
    snprintf(it->path, sizeof it->path, "%s", notify_bin);
    if (hook_scan_resolves(notify_bin)) { diag_set(it, DIAG_OK, "Present and executable."); return; }

    diag_set(it, DIAG_FAIL, "Missing from this build's bundle.");
    snprintf(it->remedy, sizeof it->remedy,
             "Build it with: cmake --build build --target liu-notify — or reinstall "
             "Liu. Until then the shim above has nothing to exec.");
}

static void check_hook_surface(DiagReport *r, const char *home,
                               const HookSurface *s, const char *shim) {
    char id[DIAG_ID_CAP];
    snprintf(id, sizeof id, "hook.%s", s->agent[0] ? s->agent : "commandcode");
    char title[DIAG_TITLE_CAP];
    snprintf(title, sizeof title, "%s — notify hook", s->label);

    DiagItem *it = diag_push(r, DIAG_GROUP_HOOKS, id, title);
    if (!it) return;
    snprintf(it->agent, sizeof it->agent, "%s", s->agent);
    home_join(it->path, sizeof it->path, home, s->rel);

    HookScanResult hs;
    hook_scan_file(home, s->rel, &hs);

    if (!hs.file_present) {
        diag_set(it, DIAG_INFO, "This CLI has no config here — nothing for Liu to touch.");
        return;
    }
    if (hs.read_failed) {
        diag_set(it, DIAG_FAIL, "The config exists but Liu cannot read it.");
        snprintf(it->remedy, sizeof it->remedy,
                 "Check the file's permissions. Liu cannot tell whether it left a "
                 "hook behind in a file it cannot open.");
        return;
    }

    /* A JSON surface Liu merges into must still parse. This is the most severe
     * check in the panel: if the merge produced invalid JSON, that agent does
     * not start — and it is NOT repairable from here, because the file is the
     * user's and everything else in it would be lost by a rewrite. */
    if (s->is_json) {
        char *text = slurp(it->path, 4u * 1024u * 1024u);
        if (text) {
            /* The text has to END with the value, as the installers require
             * (load_or_empty): a file with something after its closing brace
             * is one the agent's own parser rejects, and one Liu refuses to
             * merge into — so it must not read as "valid" here. */
            cJSON *j = cJSON_ParseWithOpts(text, NULL, true);
            if (!j) {
                const char *err = cJSON_GetErrorPtr();
                usize off = (err && err >= text) ? (usize)(err - text) : 0;
                diag_setf(it, DIAG_FAIL,
                          "This file is not valid JSON (first error at byte %zu). "
                          "%s will not start until it is fixed.", off, s->label);
                snprintf(it->remedy, sizeof it->remedy,
                         "Open %s and repair the JSON by hand. Liu deliberately will "
                         "not rewrite it: the file is yours and a rewrite would drop "
                         "everything in it that is not Liu's.", it->path);
                free(text);
                return;
            }
            cJSON_Delete(j);
        }
        free(text);
    }

    if (hs.count == 0) {
        diag_set(it, DIAG_OK, "No Liu hook recorded — clean.");
        return;
    }

    /* Everything below has at least one Liu command recorded. */
    i32 not_shim = 0;
    for (i32 i = 0; i < hs.count; i++)
        if (shim[0] && strcmp(hs.cmd[i], shim) != 0) not_shim++;

    if (hs.stale > 0) {
        diag_setf(it, DIAG_FAIL,
                  "%d recorded command%s no longer exist%s. %s prints a hook "
                  "failure every turn, in every terminal — not just Liu's:\n%s",
                  hs.stale, hs.stale == 1 ? "" : "s", hs.stale == 1 ? "s" : "",
                  s->label, hs.cmd[0]);
        it->fix = s->agent[0] ? DIAG_FIX_HOOK_REMOVE : DIAG_FIX_NONE;
        if (!s->agent[0])
            snprintf(it->remedy, sizeof it->remedy,
                     "Liu has no installer for this CLI, so it cannot remove the entry. "
                     "Delete the liu-notify hook from %s by hand.", it->path);
        return;
    }

    /* The same hook recorded more than once. An agent that rewrites its own
     * config (Codex does, to store hook trust) can strand copies Liu no longer
     * recognises, one more per launch, and each copy fires every notification
     * again. Reinstalling removes every Liu hook by its command and writes one
     * back. */
    if (hs.dups > 0) {
        diag_setf(it, DIAG_WARN,
                  "%d duplicate Liu hook entr%s recorded beside the one that "
                  "belongs here. Each copy fires the notification again.",
                  hs.dups, hs.dups == 1 ? "y is" : "ies are");
        it->fix = s->agent[0] ? DIAG_FIX_HOOK_REINSTALL : DIAG_FIX_NONE;
        if (!s->agent[0])
            snprintf(it->remedy, sizeof it->remedy,
                     "Remove the extra liu-notify entries from %s by hand.", it->path);
        return;
    }

    if (hs.count > 1) {
        diag_setf(it, DIAG_WARN,
                  "%d different Liu commands are recorded where there should be one. "
                  "A duplicate fires the notification twice.", hs.count);
        it->fix = s->agent[0] ? DIAG_FIX_HOOK_REINSTALL : DIAG_FIX_NONE;
        if (!s->agent[0])
            snprintf(it->remedy, sizeof it->remedy,
                     "Remove the extra liu-notify entries from %s by hand.", it->path);
        return;
    }

    if (not_shim > 0) {
        /* An older Liu wrote the real binary path instead of the shim. It works
         * until that binary moves, and it is what makes Codex re-prompt
         * "1 hook is new or changed" on every launch. */
        diag_setf(it, DIAG_WARN,
                  "Records the binary directly instead of the stable shim:\n%s",
                  hs.cmd[0]);
        it->fix = s->agent[0] ? DIAG_FIX_HOOK_REINSTALL : DIAG_FIX_NONE;
        if (!s->agent[0])
            snprintf(it->remedy, sizeof it->remedy,
                     "Point the hook in %s at %s instead.", it->path, shim);
        return;
    }

    diag_set(it, DIAG_OK, "Installed and pointing at the shim.");
}

/* =========================================================================
 * Group 2 — Agent History stores (filesystem only; see diagnostics.h)
 * ========================================================================= */

typedef struct {
    const char *label;
    const char *rel;    /* under $HOME */
    i32         depth;  /* how deep the scanner walks this one */
} HistRoot;

/* The roots src/history/scanner.c walks, as plain paths. Kept as data rather
 * than asked of the scanner, so the panel does not depend on the scanner's
 * API; a path list is the part that does not move. */
static const HistRoot g_hist_roots[] = {
    { "Claude Code",  ".claude/projects",                       3 },
    { "Codex",        ".codex/sessions",                        4 },
    { "Codex (archived)", ".codex/archived_sessions",           4 },
    { "Copilot",      ".copilot/session-state",                 4 },
    { "Command Code", ".commandcode/projects",                  3 },
    { "Grok",         ".grok/sessions",                         3 },
    { "Qwen",         ".qwen/projects",                         3 },
    { "Kimi",         ".kimi/sessions",                         3 },
    { "Kiro",         ".kiro/sessions",                         3 },
    { "Factory/Droid",".factory/sessions",                      3 },
    { "Cline CLI",    ".cline/data/sessions",                   3 },
    { "Antigravity",  ".gemini/antigravity/conversations",      3 },
    { "Antigravity (CLI)", ".gemini/antigravity-cli/conversations", 3 },
    { "Cursor",       ".cursor/projects",                       3 },
};
#define G_HIST_ROOT_COUNT ((i32)(sizeof g_hist_roots / sizeof g_hist_roots[0]))

static void check_hist_root(DiagReport *r, const char *home, const HistRoot *h) {
    char id[DIAG_ID_CAP];
    snprintf(id, sizeof id, "hist.%.30s", h->rel);
    for (char *p = id; *p; p++) if (*p == '/' || *p == '.') *p = '-';

    char title[DIAG_TITLE_CAP];
    snprintf(title, sizeof title, "%s — session store", h->label);

    DiagItem *it = diag_push(r, DIAG_GROUP_HISTORY, id, title);
    if (!it) return;
    home_join(it->path, sizeof it->path, home, h->rel);

    struct stat st;
    if (stat(it->path, &st) != 0) {
        diag_set(it, DIAG_INFO, "Not present — this tool has not been used on this machine.");
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        diag_set(it, DIAG_WARN, "Exists but is not a directory, so it will never be scanned.");
        snprintf(it->remedy, sizeof it->remedy,
                 "Move or remove %s; Agent History expects a directory here.", it->path);
        return;
    }
    if (access(it->path, R_OK | X_OK) != 0) {
        diag_set(it, DIAG_FAIL, "Present but not readable, so Agent History shows nothing from it.");
        snprintf(it->remedy, sizeof it->remedy,
                 "Fix the permissions on %s (it needs read + execute for you).", it->path);
        return;
    }

    DirCount c; memset(&c, 0, sizeof c);
    walk_count(it->path, h->depth, &c);

    if (c.capped) {
        diag_setf(it, DIAG_WARN,
                  "Over %d files — Liu stopped counting. A store this large makes "
                  "every open of Agent History slow, because each open rescans from disk.",
                  DIAG_WALK_CAP);
        snprintf(it->remedy, sizeof it->remedy,
                 "Archive or delete old sessions under %s if the picker feels slow.",
                 it->path);
        return;
    }
    if (c.unreadable > 0) {
        diag_setf(it, DIAG_FAIL,
                  "%d of %d session files cannot be read, so they are missing from the "
                  "picker. First: %s", c.unreadable, c.files, c.first_bad);
        snprintf(it->remedy, sizeof it->remedy,
                 "Fix the permissions on those files, or delete them if they are junk.");
        return;
    }
    /* Empty files are NOT a fault: every tool here writes the session file up
     * front and fills it as the turn runs, so a store in use always has a few.
     * They are counted because a parser surprise usually starts there, and
     * reported on the OK line rather than as a warning. */
    if (c.empty > 0)
        diag_setf(it, DIAG_OK, "%d session file%s (%d still empty).",
                  c.files, c.files == 1 ? "" : "s", c.empty);
    else
        diag_setf(it, DIAG_OK, "%d session file%s.", c.files, c.files == 1 ? "" : "s");
}

/* =========================================================================
 * Group 3 — Liu's own files
 * ========================================================================= */

static void check_json_file(DiagReport *r, const char *id, const char *title,
                            const char *path, bool required) {
    DiagItem *it = diag_push(r, DIAG_GROUP_LIU, id, title);
    if (!it) return;
    snprintf(it->path, sizeof it->path, "%s", path);

    if (!path_exists(path)) {
        diag_set(it, required ? DIAG_WARN : DIAG_INFO,
                 required ? "Missing — Liu is running on built-in defaults."
                          : "Not present yet, which is normal on a fresh install.");
        return;
    }
    char *text = slurp(path, 8u * 1024u * 1024u);
    if (!text) {
        diag_set(it, DIAG_FAIL, "Present but unreadable.");
        snprintf(it->remedy, sizeof it->remedy, "Check the permissions on %s.", path);
        return;
    }
    cJSON *j = cJSON_Parse(text);
    if (!j) {
        const char *err = cJSON_GetErrorPtr();
        usize off = (err && err >= text) ? (usize)(err - text) : 0;
        diag_setf(it, DIAG_FAIL,
                  "Not valid JSON (first error at byte %zu), so Liu fell back to "
                  "defaults and every setting in it is being ignored.", off);
        snprintf(it->remedy, sizeof it->remedy,
                 "Repair %s by hand, or delete it to start from defaults.", path);
    } else {
        cJSON_Delete(j);
        diag_set(it, DIAG_OK, "Valid.");
    }
    free(text);
}

/* There is deliberately NO terminfo check here.
 *
 * `session_create_local_with_env` hands every shell TERM set to the standard 256-color entry and
 * nothing in the tree ever sets TERM=Liu, so whether Liu's own entry is compiled changes nothing
 * about any program Liu launches; truecolor is already advertised through COLORTERM=truecolor.
 *
 * A check that cannot fire usefully only teaches people to ignore the panel. If TERM ever becomes
 * Liu, the check belongs back here — and it must honour $TERMINFO/$TERMINFO_DIRS and both hex
 * spellings of the directory (`~/.terminfo/4C/` and `~/.terminfo/4c/`, which ncurses on macOS
 * writes). */


/* =========================================================================
 * Entry points
 * ========================================================================= */

void diag_run(DiagReport *r, const char *notify_bin) {
    if (!r) return;
    memset(r, 0, sizeof *r);
    r->ran = true;
    {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(r->ran_at, sizeof r->ran_at, "%H:%M:%S", &tmv);
    }

    char home[DIAG_PATH_CAP];
    if (!hook_scan_home(home, sizeof home)) {
        DiagItem *it = diag_push(r, DIAG_GROUP_LIU, "home", "Home directory");
        diag_set(it, DIAG_FAIL, "$HOME is not set, so nothing can be checked.");
        if (it) snprintf(it->remedy, sizeof it->remedy,
                         "Launch Liu from a session with a valid $HOME.");
        diag_recount(r);
        return;
    }

    /* --- hooks --------------------------------------------------------- */
    check_notify_bin(r, notify_bin);
    check_shim(r, home, notify_bin);

    char shim[DIAG_PATH_CAP] = {0};
    agent_hook_shim_path(shim, sizeof shim);
    for (i32 i = 0; i < G_SURFACE_COUNT; i++)
        check_hook_surface(r, home, &g_surfaces[i], shim);

    /* --- history ------------------------------------------------------- */
    for (i32 i = 0; i < G_HIST_ROOT_COUNT; i++)
        check_hist_root(r, home, &g_hist_roots[i]);

    /* --- Liu ----------------------------------------------------------- */
    {
        char p[DIAG_PATH_CAP];
        snprintf(p, sizeof p, "%s", config_file_path());
        check_json_file(r, "config", "config.json", p, true);

        if ((usize)snprintf(p, sizeof p, "%s/sessions.json", config_user_dir()) < sizeof p)
            check_json_file(r, "sessions", "sessions.json (restored tabs)", p, false);
    }

    diag_recount(r);
}

/* Re-run the single check an item came from, so a Fix cannot leave a row
 * claiming a repair that did not take. Cheaper and less error-prone than
 * mutating the row in place: the check is the only thing that knows what "OK"
 * means for it. */
static void diag_refresh_item(DiagItem *it, const char *notify_bin) {
    DiagReport tmp;
    memset(&tmp, 0, sizeof tmp);
    char home[DIAG_PATH_CAP];
    if (!hook_scan_home(home, sizeof home)) return;

    char shim[DIAG_PATH_CAP] = {0};
    agent_hook_shim_path(shim, sizeof shim);

    if (!strcmp(it->id, "shim")) {
        check_shim(&tmp, home, notify_bin);
    } else if (!strncmp(it->id, "hook.", 5)) {
        for (i32 i = 0; i < G_SURFACE_COUNT; i++) {
            const char *a = g_surfaces[i].agent[0] ? g_surfaces[i].agent : "commandcode";
            if (!strcmp(it->id + 5, a)) { check_hook_surface(&tmp, home, &g_surfaces[i], shim); break; }
        }
    }
    if (tmp.count != 1) return;

    /* Keep the fix bookkeeping; take everything the check re-derived. */
    bool attempted = it->fix_attempted, ok = it->fix_ok;
    char msg[DIAG_TEXT_CAP];
    snprintf(msg, sizeof msg, "%s", it->fix_msg);
    *it = tmp.items[0];
    it->fix_attempted = attempted;
    it->fix_ok = ok;
    snprintf(it->fix_msg, sizeof it->fix_msg, "%s", msg);
}

bool diag_fix(DiagItem *it, const char *notify_bin) {
    if (!it || it->fix == DIAG_FIX_NONE) return false;
    it->fix_attempted = true;
    it->fix_ok = false;
    it->fix_msg[0] = '\0';

    AgentHookResult res;
    memset(&res, 0, sizeof res);

    switch (it->fix) {
    case DIAG_FIX_SHIM:
        if (!hook_scan_resolves(notify_bin)) {
            snprintf(it->fix_msg, sizeof it->fix_msg,
                     "There is no usable liu-notify to point the shim at.");
            return false;
        }
        res = agent_hook_shim_install(notify_bin);
        break;

    case DIAG_FIX_HOOK_REINSTALL: {
        if (!it->agent[0]) {
            snprintf(it->fix_msg, sizeof it->fix_msg, "No installer for this CLI.");
            return false;
        }
        /* Uninstall first so a duplicate entry cannot survive the reinstall —
         * the installers are idempotent for THEIR shape, not for one an older
         * Liu wrote. */
        agent_hook_uninstall(it->agent);
        char shim[DIAG_PATH_CAP];
        if (!agent_hook_shim_path(shim, sizeof shim)) {
            snprintf(it->fix_msg, sizeof it->fix_msg, "Could not resolve the shim path.");
            return false;
        }
        if (notify_bin && *notify_bin) agent_hook_shim_install(notify_bin);
        res = agent_hook_install(it->agent, shim);
        break;
    }

    case DIAG_FIX_HOOK_REMOVE:
        if (!it->agent[0]) {
            snprintf(it->fix_msg, sizeof it->fix_msg, "No installer for this CLI.");
            return false;
        }
        res = agent_hook_uninstall(it->agent);
        break;

    default:
        return false;
    }

    it->fix_ok = res.ok;
    snprintf(it->fix_msg, sizeof it->fix_msg, "%s",
             res.msg[0] ? res.msg : (res.ok ? "Done." : "Failed."));
    diag_refresh_item(it, notify_bin);
    return it->fix_ok;
}

void diag_fix_all(DiagReport *r, const char *notify_bin,
                  i32 *out_fixed, i32 *out_failed) {
    i32 fixed = 0, failed = 0;
    if (r) {
        for (i32 i = 0; i < r->count; i++) {
            DiagItem *it = &r->items[i];
            if (it->fix == DIAG_FIX_NONE) continue;
            if (it->status != DIAG_WARN && it->status != DIAG_FAIL) continue;
            if (diag_fix(it, notify_bin)) fixed++; else failed++;
        }
        diag_recount(r);
    }
    if (out_fixed)  *out_fixed  = fixed;
    if (out_failed) *out_failed = failed;
}
