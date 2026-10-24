/*
 * Liu - Translate-on-Tab: agent session-store cleanup (grok + opencode).
 * See translate_history_cleanup.h for the contract and the safety model.
 */
#include "translate/translate_history_cleanup.h"

#include "cJSON.h"
#include "sqlite3.h"
#include "core/sqlite_harden.h"
#include "history/opencode_store.h"   /* 1.x or 2.x tables */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef PLATFORM_WIN32
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

/* Canonical translate prompt markers (see translate_build_prompt in
 * translate.c: "Translate the following segment into %s, without additional
 * explanation. %s"). Both must appear for a match — together they pin Liu's own
 * prompt and exclude the legacy "Translate to English…" and Create-Theme
 * prompts. Kept in sync with the prompt wording. */
#define TR_PREFIX "Translate the following segment into "
#define TR_INFIX  ", without additional explanation."

/* Dry-run: when LIU_TRANSLATE_CLEANUP_DRYRUN is set, report what WOULD be
 * removed on stderr instead of deleting — lets the signature/diff logic be
 * verified safely against a real store without touching it. */
static bool cleanup_dryrun(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = liu_dev_env("LIU_TRANSLATE_CLEANUP_DRYRUN");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v == 1;
}

static const char *home_dir(void) {
    const char *home = getenv("HOME");
#ifndef PLATFORM_WIN32
    if (!home || !*home) {
        struct passwd *pw = getpwuid(geteuid());
        if (pw && pw->pw_dir) home = pw->pw_dir;
    }
#endif
    return (home && *home) ? home : NULL;
}

/* Read up to `cap` bytes of `path` into a malloc'd NUL-terminated buffer. */
static char *read_all(const char *path, size_t cap) {
    if (!path || !*path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > cap || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return NULL;
    }
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)sz, f);
    bool ok = got == (size_t)sz && !ferror(f);
    fclose(f);
    if (!ok) { free(b); return NULL; }
    b[got] = '\0';
    return b;
}

/* Percent-encode `s` the way grok keys its per-cwd session dir: keep RFC-3986
 * unreserved bytes, %XX everything else (so '/' -> %2F). A mismatch only means
 * "dir not found → skip cleanup" (junk left, never a wrong delete). */
static bool pct_encode(const char *s, char *out, size_t cap) {
    if (!s || !out || cap == 0) return false;
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            if (o + 1 >= cap) { out[0] = '\0'; return false; }
            out[o++] = (char)c;
        } else {
            if (o + 3 >= cap) { out[0] = '\0'; return false; }
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
    return true;
}

/* Recursively remove `path` (a session dir already signature-matched). */
static void rm_rf_depth(const char *path, i32 depth) {
    if (!path || depth > 64) return;
    struct stat root_st;
    if (lstat(path, &root_st) != 0) return;
    if (!S_ISDIR(root_st.st_mode)) { (void)unlink(path); return; }
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[2048];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= sizeof child)
            continue;
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf_depth(child, depth + 1);
        else unlink(child);
    }
    closedir(d);
    rmdir(path);
}
static void rm_rf(const char *path) { rm_rf_depth(path, 0); }

/* ======================== grok ======================== */

static bool grok_cwd_dir(const char *cwd, char *out, size_t cap) {
    if (!cwd || !cwd[0]) return false;
    char base[700];
    const char *gh = getenv("GROK_HOME");
    if (gh && *gh) {
        if ((size_t)snprintf(base, sizeof base, "%s/sessions", gh) >= sizeof base) return false;
    } else {
        const char *h = home_dir();
        if (!h) return false;
        if ((size_t)snprintf(base, sizeof base, "%s/.grok/sessions", h) >= sizeof base) return false;
    }
    char enc[1100];
    if (!pct_encode(cwd, enc, sizeof enc)) return false;
    return (size_t)snprintf(out, cap, "%s/%s", base, enc) < cap;
}

/* True iff <sessdir> is a translate ONE-SHOT: chat_history.jsonl is exactly 5
 * lines typed system,user,user,reasoning,assistant with a single assistant and
 * the translate prefix on line[2], summary.json has num_chat_messages==5, and
 * the transcript is from this run (mtime >= spawn_time). */
static bool grok_oneshot_match(const char *sessdir, double spawn_time) {
    char chp[1600];
    if ((size_t)snprintf(chp, sizeof chp, "%s/chat_history.jsonl", sessdir) >= sizeof chp)
        return false;
    struct stat st;
    if (stat(chp, &st) != 0) return false;
    if ((double)st.st_mtime + 3.0 < spawn_time) return false;   /* not from this run */

    char *txt = read_all(chp, 512 * 1024);
    if (!txt) return false;

    static const char *want[5] = { "system", "user", "user", "reasoning", "assistant" };
    bool ok = true, prefix_ok = false;
    int li = 0, assistant_n = 0;
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line) continue;
        if (li >= 5) { ok = false; break; }       /* more than 5 non-empty lines */
        cJSON *o = cJSON_Parse(line);
        if (!o) { ok = false; break; }
        cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "type");
        const char *ts = (cJSON_IsString(t) && t->valuestring) ? t->valuestring : "";
        if (strcmp(ts, want[li]) != 0) ok = false;
        if (strcmp(ts, "assistant") == 0) assistant_n++;
        if (li == 2 && strstr(line, TR_PREFIX) && strstr(line, TR_INFIX)) prefix_ok = true;
        cJSON_Delete(o);
        li++;
        if (!ok) break;
    }
    free(txt);
    if (!ok || li != 5 || assistant_n != 1 || !prefix_ok) return false;

    char sp[1600];
    if ((size_t)snprintf(sp, sizeof sp, "%s/summary.json", sessdir) >= sizeof sp) return false;
    char *s = read_all(sp, 256 * 1024);
    if (!s) return false;                          /* require the sidecar */
    bool sum_ok = false;
    cJSON *o = cJSON_Parse(s);
    if (o) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "num_chat_messages");
        if (cJSON_IsNumber(n) && isfinite(n->valuedouble) && n->valuedouble == 5.0)
            sum_ok = true;
        cJSON_Delete(o);
    }
    free(s);
    return sum_ok;
}

static void grok_snapshot(const char *cwd, TranslateCleanupSnap *snap) {
    char dir[1300];
    if (!grok_cwd_dir(cwd, dir, sizeof dir)) return;
    DIR *d = opendir(dir);
    if (!d) { snap->valid = errno == ENOENT; return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (snap->count >= TRANSLATE_CLEANUP_MAX ||
            strlen(e->d_name) >= TRANSLATE_CLEANUP_ID) {
            snap->count = 0;
            snap->valid = false;
            closedir(d);
            return;
        }
        snprintf(snap->entries[snap->count++], TRANSLATE_CLEANUP_ID, "%s", e->d_name);
    }
    closedir(d);
    snap->valid = true;
}

static void grok_after_exit(const char *cwd, double spawn_time,
                            const TranslateCleanupSnap *snap) {
    char dir[1300];
    if (!grok_cwd_dir(cwd, dir, sizeof dir)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        bool seen = false;
        for (i32 i = 0; i < snap->count; i++)
            if (strcmp(snap->entries[i], e->d_name) == 0) { seen = true; break; }
        if (seen) continue;                        /* existed at spawn → keep */
        char sd[1500];
        if ((size_t)snprintf(sd, sizeof sd, "%s/%s", dir, e->d_name) >= sizeof sd) continue;
        struct stat st;
        if (lstat(sd, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (grok_oneshot_match(sd, spawn_time)) {            /* only on full match */
            if (cleanup_dryrun())
                fprintf(stderr, "[liu translate-cleanup] would rm grok dir %s\n", e->d_name);
            else
                rm_rf(sd);
        }
    }
    closedir(d);
}

/* ======================== opencode ======================== */

/* Open opencode.db as a pure reader that can still SEE a WAL-mode db's
 * uncheckpointed rows. A SQLITE_OPEN_READONLY connection can't create the -shm
 * it needs for WAL, so it would read an empty/stale snapshot (the symptom:
 * SELECT id FROM session → 0 rows). Open READWRITE, then PRAGMA query_only=1 to
 * forbid every write — actual deletes go through the `opencode session delete`
 * CLI, never this handle. Returns NULL on failure (and never creates the db,
 * since SQLITE_OPEN_CREATE is omitted). */
static sqlite3 *opencode_open_reader(const char *dbp) {
    /* The vendored sqlite3 is built with SQLITE_OMIT_AUTOINIT, so the library
     * must be initialized explicitly before the first open. Idempotent — a
     * no-op if the host (vault) already initialized it. */
    sqlite3_initialize();
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(dbp, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    liu_sqlite_harden_foreign(db);               /* somebody else's file */
    sqlite3_busy_timeout(db, 800);
    sqlite3_exec(db, "PRAGMA query_only=1;", NULL, NULL, NULL);
    return db;
}

static bool opencode_db_path(char *out, size_t cap) {
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        return (size_t)snprintf(out, cap, "%s/opencode/opencode.db", xdg) < cap;
    const char *h = home_dir();
    if (!h) return false;
    return (size_t)snprintf(out, cap, "%s/.local/share/opencode/opencode.db", h) < cap;
}

/* Every session id, whichever shape the store has (history/opencode_store.h: 1.x
 * `session`, 2.x `session_v2`). Against a 2.x store `SELECT id FROM session`
 * does not even prepare, so the query follows the store's shape or the snapshot
 * is never valid. */
static const char *opencode_session_ids_sql(sqlite3 *db) {
    return opencode_store_is_v2(db) ? "SELECT id FROM session_v2" : "SELECT id FROM session";
}

static void opencode_snapshot(TranslateCleanupSnap *snap) {
    char dbp[1024];
    if (!opencode_db_path(dbp, sizeof dbp)) return;
    struct stat st;
    if (stat(dbp, &st) != 0) { snap->valid = errno == ENOENT; return; } /* no db yet → empty */
    sqlite3 *db = opencode_open_reader(dbp);
    if (!db) return;
    sqlite3_stmt *stmt = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(db, opencode_session_ids_sql(db), -1, &stmt, NULL) == SQLITE_OK) {
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char *id = (const char *)sqlite3_column_text(stmt, 0);
            if (!id || snap->count >= TRANSLATE_CLEANUP_MAX ||
                strlen(id) >= TRANSLATE_CLEANUP_ID) {
                snap->count = 0;
                rc = SQLITE_ERROR;
                break;
            }
            snprintf(snap->entries[snap->count++], TRANSLATE_CLEANUP_ID, "%s", id);
        }
        ok = rc == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    snap->valid = ok;
    if (!ok) snap->count = 0;
}

/* One translate one-shot = exactly one user message, <=2 total, and the first
 * text part begins with the canonical prefix (after opencode's literal quote).
 * In a 2.x store the conversation is `session_message`, whose rows also include
 * the idle marks, model switches and notices around a turn: only the user and
 * assistant rows count, and the user row's own `text` is the prompt. */
static bool opencode_oneshot_match(sqlite3 *db, const char *sid) {
    const bool v2 = opencode_store_is_v2(db);
    sqlite3_stmt *s = NULL;
    int total = -1, users = -1;
    if (sqlite3_prepare_v2(db,
            v2 ? "SELECT COALESCE(SUM(type IN ('user','assistant')), 0), "
                 "COALESCE(SUM(type = 'user'), 0) "
                 "FROM session_message WHERE session_id=?1"
               : "SELECT COUNT(*), "
                 "SUM(CASE WHEN json_extract(data,'$.role')='user' THEN 1 ELSE 0 END) "
                 "FROM message WHERE session_id=?1", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            total = sqlite3_column_int(s, 0);
            users = sqlite3_column_int(s, 1);
        }
        sqlite3_finalize(s);
    }
    if (total < 1 || total > 2 || users != 1) return false;

    bool ok = false;
    s = NULL;
    if (sqlite3_prepare_v2(db,
            v2 ? "SELECT json_extract(data,'$.text') FROM session_message "
                 "WHERE session_id=?1 AND type='user' ORDER BY seq LIMIT 1"
               : "SELECT json_extract(p.data,'$.text') FROM part p "
                 "JOIN message m ON p.message_id=m.id "
                 "WHERE m.session_id=?1 AND json_extract(p.data,'$.type')='text' "
                 "ORDER BY p.id LIMIT 1", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(s, 0);
            if (t) {
                const char *p = t;
                if (*p == '"') p++;                /* opencode wraps the arg in a quote */
                while (*p == ' ' || *p == '\n' || *p == '\t') p++;
                if (strncmp(p, TR_PREFIX, strlen(TR_PREFIX)) == 0 && strstr(p, TR_INFIX))
                    ok = true;
            }
        }
        sqlite3_finalize(s);
    }
    return ok;
}

/* Remove via the CLI so the FK cascade + any opencode bookkeeping run correctly
 * (fork+exec mirrors the rest of the translate plumbing; silenced + reaped). */
static void opencode_session_delete(const char *id) {
#ifndef PLATFORM_WIN32
    if (!id || !id[0] || strnlen(id, TRANSLATE_CLEANUP_ID) >=
        TRANSLATE_CLEANUP_ID) return;
    /* The id is a text column out of opencode.db and lands on argv: one
     * starting with '-' is an OPTION to the CLI. opencode ids are "ses_" +
     * base62; anything outside [A-Za-z0-9_-] is not one. */
    if (id[0] == '-') return;
    for (const char *p = id; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return;
    }
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return;
    int rc = posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null",
                                               O_WRONLY, 0);
    if (rc == 0)
        rc = posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
                                               O_WRONLY, 0);
    char *argv[] = { (char *)"opencode", (char *)"session", (char *)"delete",
                     (char *)id, NULL };
    pid_t p = -1;
    if (rc == 0) rc = posix_spawnp(&p, "opencode", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc == 0 && p > 0) {
        int st;
        while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    }
#else
    (void)id;
#endif
}

static void opencode_after_exit(const TranslateCleanupSnap *snap) {
    char dbp[1024];
    if (!opencode_db_path(dbp, sizeof dbp)) return;
    sqlite3 *db = opencode_open_reader(dbp);
    if (cleanup_dryrun())
        fprintf(stderr, "[liu translate-cleanup] opencode db=%s opened=%d\n", dbp, db != NULL);
    if (!db) return;

    char to_delete[16][TRANSLATE_CLEANUP_ID];
    i32 ndel = 0, considered = 0, prep_rc = -1;
    sqlite3_stmt *stmt = NULL;
    prep_rc = sqlite3_prepare_v2(db, opencode_session_ids_sql(db), -1, &stmt, NULL);
    if (prep_rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && ndel < 16) {
            const char *id = (const char *)sqlite3_column_text(stmt, 0);
            if (!id) continue;
            bool seen = false;
            for (i32 i = 0; i < snap->count; i++)
                if (strcmp(snap->entries[i], id) == 0) { seen = true; break; }
            if (seen) continue;                    /* existed at spawn → keep */
            considered++;
            if (opencode_oneshot_match(db, id) && strlen(id) < TRANSLATE_CLEANUP_ID)
                snprintf(to_delete[ndel++], TRANSLATE_CLEANUP_ID, "%s", id);
        }
        sqlite3_finalize(stmt);
    }
    if (cleanup_dryrun())
        fprintf(stderr, "[liu translate-cleanup] opencode session-select rc=%d considered=%d matched=%d\n",
                prep_rc, considered, ndel);
    sqlite3_close(db);

    for (i32 i = 0; i < ndel; i++) {
        if (cleanup_dryrun())
            fprintf(stderr, "[liu translate-cleanup] would delete opencode session %s\n", to_delete[i]);
        else
            opencode_session_delete(to_delete[i]);
    }
}

/* ======================== dispatch ======================== */

void translate_cleanup_snapshot(const char *agent_id, const char *cwd,
                                TranslateCleanupSnap *snap) {
    if (!snap) return;
    snap->valid = false;
    snap->count = 0;
    if (!agent_id) return;
    if (strcmp(agent_id, "grok") == 0)          grok_snapshot(cwd, snap);
    else if (strcmp(agent_id, "opencode") == 0) opencode_snapshot(snap);
}

void translate_cleanup_after_exit(const char *agent_id, const char *cwd,
                                  f64 spawn_time, const TranslateCleanupSnap *snap) {
    if (!snap || !snap->valid || snap->count < 0 ||
        snap->count > TRANSLATE_CLEANUP_MAX || !agent_id) return;
    if (strcmp(agent_id, "grok") == 0)          grok_after_exit(cwd, spawn_time, snap);
    else if (strcmp(agent_id, "opencode") == 0) opencode_after_exit(snap);
}
