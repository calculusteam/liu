/*
 * liu-history - discover session files from all supported agents.
 *
 * Walks these roots (honoring env overrides):
 *   claude:      $CLAUDE_CONFIG_DIR/projects       | ~/.claude/projects
 *   codex:       $CODEX_HOME/sessions              | ~/.codex/sessions
 *   copilot:     $COPILOT_HOME/session-state       | ~/.copilot/session-state
 *   antigravity: ~/.gemini/antigravity/conversations/ (SQLite .db files)
 *   cline family: ~/Library/Application Support/Code/User/globalStorage/
 *                 <extension-id>/tasks/<task-id>/ui_messages.json  (macOS)
 *                 (paths under $XDG_CONFIG_HOME on Linux, %APPDATA% on Win32)
 *
 * Claude stores one flat directory per project-slug with *.jsonl files.
 * Codex nests by date: sessions/YYYY/MM/DD/rollout-*.jsonl.
 * Copilot is scanned shallowly under its session-state root.
 * Cline et al. store a single ui_messages.json per task-id subdirectory.
 *
 * Results are delivered via callback sorted-by-mtime descending. Memory for
 * ChatSessionMeta is caller-owned; scanner only passes by value.
 */
#include "core/mem_budget.h"
#include "history/session.h"
#include "history/util.h"
#include "history/opencode_store.h"   /* 1.x or 2.x tables */
#include "core/memory.h"
#include "core/checked.h"
#include "sqlite3.h"
#include "core/sqlite_harden.h"
#include "cJSON.h"

#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

/* One cancellation scope per calling thread. Some discovery helpers (IDE
 * probes and small header readers) do not carry EntryVec; a scoped TLS token
 * lets ALL of their I/O honor the same request without a process-global race.
 * chat_scan_cancellable saves/restores the scope, including nested scans from
 * delivery callbacks. No connection or scope escapes this synchronous call. A
 * blocked OS read/stat cannot be preempted: the scan stops before the next
 * one. */
typedef struct {
    const _Atomic bool *cancel;
    bool stopped;
} ScanCancelScope;
static _Thread_local ScanCancelScope g_scan_scope;

static bool scan_cancelled(void) {
    if (g_scan_scope.cancel &&
        atomic_load_explicit(g_scan_scope.cancel, memory_order_acquire))
        g_scan_scope.stopped = true;
    return g_scan_scope.stopped;
}

static DIR *scan_opendir(const char *path) {
    if (scan_cancelled()) return NULL;
    DIR *d = opendir(path);
    if (scan_cancelled()) { if (d) closedir(d); return NULL; }
    return d;
}
static struct dirent *scan_readdir(DIR *d) {
    if (scan_cancelled()) return NULL;
    struct dirent *de = readdir(d);
    return scan_cancelled() ? NULL : de;
}
static int scan_stat(const char *path, struct stat *st) {
    if (scan_cancelled()) return -1;
    int rc = stat(path, st);
    return scan_cancelled() ? -1 : rc;
}
static int scan_lstat(const char *path, struct stat *st) {
    if (scan_cancelled()) return -1;
    int rc = lstat(path, st);
    return scan_cancelled() ? -1 : rc;
}
static FILE *scan_fopen(const char *path, const char *mode) {
    if (scan_cancelled()) return NULL;
    FILE *f = fopen(path, mode);
    if (scan_cancelled()) { if (f) fclose(f); return NULL; }
    return f;
}
static usize scan_fread(void *dst, usize size, usize count, FILE *f) {
    if (scan_cancelled()) return 0;
    usize n = fread(dst, size, count, f);
    return scan_cancelled() ? 0 : n;
}
static bool scan_slurp(Arena *a, const char *path, usize cap, char **buf, usize *len) {
    if (scan_cancelled()) return false;
    bool ok = hist_slurp_file(a, path, cap, buf, len);
    return !scan_cancelled() && ok;
}
static int scan_sql_progress(void *unused) {
    (void)unused;
    return scan_cancelled() ? 1 : 0;
}
static int scan_sql_open(const char *path, sqlite3 **db, int flags, const char *vfs) {
    *db = NULL;
    if (scan_cancelled()) return SQLITE_INTERRUPT;
    int rc = sqlite3_open_v2(path, db, flags, vfs);
    if (rc == SQLITE_OK) {
        liu_sqlite_harden_foreign(*db);          /* somebody else's file */
        sqlite3_progress_handler(*db, 256, scan_sql_progress, NULL);
    }
    if (scan_cancelled()) {
        if (*db) sqlite3_close(*db);
        *db = NULL;
        return SQLITE_INTERRUPT;
    }
    return rc;
}
static int scan_sql_prepare(sqlite3 *db, const char *sql, int n,
                             sqlite3_stmt **stmt, const char **tail) {
    *stmt = NULL;
    if (scan_cancelled()) return SQLITE_INTERRUPT;
    int rc = sqlite3_prepare_v2(db, sql, n, stmt, tail);
    if (scan_cancelled()) {
        if (*stmt) sqlite3_finalize(*stmt);
        *stmt = NULL;
        return SQLITE_INTERRUPT;
    }
    return rc;
}
static int scan_sql_step(sqlite3_stmt *stmt) {
    if (scan_cancelled()) return SQLITE_INTERRUPT;
    int rc = sqlite3_step(stmt);
    return scan_cancelled() ? SQLITE_INTERRUPT : rc;
}

typedef struct {
    ChatSessionMeta meta;
} Entry;

typedef struct {
    Entry *items;
    usize  count;
    usize  cap;
    Arena *arena;       /* string allocator for meta fields */
} EntryVec;

static void vec_push(EntryVec *v, const ChatSessionMeta *m) {
    if (!v || !m || scan_cancelled() || v->count == SIZE_MAX) return;
    /* An empty session has no transcript to show and nothing to resume —
     * listing it only costs the reader a row to skip past. Applied here
     * rather than in the picker so the CLI and the UI agree on what counts
     * as a session.
     *
     * "Empty" is measured per storage model. File-backed tools use the byte
     * size of the transcript. OpenCode is DB-backed and reports size_bytes =
     * 0 (see scan_opencode: summing message lengths would read every message
     * body), so it is tested on its message count instead — without this
     * branch the byte gate would drop every OpenCode row. */
    if (chat_tool_is_db_store(m->tool)) {
        if (m->event_count == 0) return;
    } else if (m->size_bytes <= 0) {
        return;
    }
    if (v->count >= v->cap) {
        usize ncap = 64;
        usize bytes;
        if (v->cap && !checked_mul_usize(v->cap, 2, &ncap)) return;
        if (!checked_mul_usize(ncap, sizeof *v->items, &bytes)) return;
        Entry *p = realloc(v->items, bytes);
        if (!p) return;
        v->items = p;
        v->cap = ncap;
    }
    v->items[v->count++].meta = *m;
}

/* Duplicate a NUL-terminated string into the supplied arena. Returns NULL
 * on arena exhaustion (caller treats as fatal for that meta entry). */
static const char *arena_dup_str(Arena *a, const char *s) {
    if (!s) return NULL;
    usize n;
    if (!checked_add_usize(strlen(s), 1, &n)) return NULL;
    char *p = arena_alloc(a, n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

/* Build "<dir>/<fname>" inside the arena and return a pointer to it. */
static const char *arena_join_path(Arena *a, const char *dir, const char *fname) {
    if (!dir || !fname) return NULL;
    usize dn = strlen(dir);
    usize fn = strlen(fname);
    usize tot;
    if (!checked_add_usize(dn, 1, &tot) ||
        !checked_add_usize(tot, fn, &tot) ||
        !checked_add_usize(tot, 1, &tot)) {
        return NULL;
    }
    char *p = arena_alloc(a, tot);
    if (!p) return NULL;
    memcpy(p, dir, dn);
    p[dn] = '/';
    memcpy(p + dn + 1, fname, fn);
    p[dn + 1 + fn] = '\0';
    return p;
}

/* Stable bottom-up merge sort with bounded cancellation checkpoints. qsort
 * offered no early exit: cancellation during a large sort still paid for the
 * entire store. Cancellation never publishes a partially sorted result. */
static bool scan_sort(EntryVec *v) {
    if (scan_cancelled()) return false;
    if (v->count < 2) return true;
    usize bytes;
    if (!checked_mul_usize(v->count, sizeof *v->items, &bytes)) return false;
    Entry *scratch = malloc(bytes);
    if (!scratch) return false;
    Entry *src = v->items, *dst = scratch;
    usize n = v->count;
    for (usize width = 1; width < n;) {
        for (usize left = 0; left < n;) {
            usize mid = left + (n - left < width ? n - left : width);
            usize end = mid + (n - mid < width ? n - mid : width);
            usize a = left, b = mid;
            for (usize k = left; k < end; k++) {
                if ((k & 255u) == 0 && scan_cancelled()) goto cancelled;
                if (a < mid && (b == end ||
                    src[a].meta.last_modified_ms >= src[b].meta.last_modified_ms))
                    dst[k] = src[a++];
                else
                    dst[k] = src[b++];
            }
            left = end;
        }
        Entry *swap = src; src = dst; dst = swap;
        if (width > n / 2) break;
        width *= 2;
    }
    if (src != v->items) {
        for (usize i = 0; i < n;) {
            if (scan_cancelled()) goto cancelled;
            usize count = n - i;
            if (count > 256) count = 256;
            memcpy(v->items + i, src + i, count * sizeof *src);
            i += count;
        }
    }
    free(scratch);
    return !scan_cancelled();
cancelled:
    free(scratch);
    return false;
}

static bool ends_with(const char *s, const char *suffix) {
    usize sn = strlen(s), fn = strlen(suffix);
    return sn >= fn && strcmp(s + sn - fn, suffix) == 0;
}

static u32 short_hash32(const char *s, usize n) {
    u32 h = 2166136261u;
    for (usize i = 0; i < n; i++) {
        h ^= (u8)s[i];
        h *= 16777619u;
    }
    return h;
}

/* Allocate `len` + 1 bytes from the arena and copy the (NUL-terminated)
 * session id. If `len` exceeds SESSION_ID_HARD_CAP the id is truncated and
 * a stable 8-hex hash appended, so long rollout IDs cannot silently
 * collide. */
#define SESSION_ID_HARD_CAP 96
static const char *arena_session_id(Arena *a, const char *fname, usize base_len) {
    if (!fname) return NULL;
    if (base_len < SESSION_ID_HARD_CAP) {
        usize alloc_len;
        if (!checked_add_usize(base_len, 1, &alloc_len)) return NULL;
        char *p = arena_alloc(a, alloc_len);
        if (!p) return NULL;
        memcpy(p, fname, base_len);
        p[base_len] = '\0';
        return p;
    }
    const usize hash_len = 9;     /* "~" + 8 hex chars */
    const usize cap      = SESSION_ID_HARD_CAP;
    usize keep = cap > hash_len + 1 ? cap - hash_len - 1 : 0;
    char *p = arena_alloc(a, cap);
    if (!p) return NULL;
    if (keep > 0) memcpy(p, fname, keep);
    u32 h = short_hash32(fname, base_len);
    snprintf(p + keep, cap - keep, "~%08x", h);
    return p;
}

/* The real working directory of a Claude-style session: the first top-level
 * `"cwd":"…"` record in its JSONL.
 *
 * The directory NAME cannot give it back. Claude slugs every non-alphanumeric
 * byte to '-', so "/w/my-project", "/w/my/project" and "/w/My App.v2" are
 * indistinguishable from "-w-my-project" and a reversal can only guess. The
 * records carry the real path, usually near the start of the file.
 *
 * Bounded read (CWD_PEEK_BYTES) in 8 KB steps, stopping at the first hit. A
 * raw `"cwd":"` can only be a record's own key: inside message text the quotes
 * are escaped. Returns NULL when there is none — the caller then matches by
 * slug, which is a comparison that CAN be made, and claims no path. */
#define CWD_PEEK_BYTES (64u * 1024u)
static const char *peek_record_cwd(Arena *a, const char *path) {
    if (!a || !path) return NULL;
    FILE *f = scan_fopen(path, "r");
    if (!f) return NULL;
    char *buf = (char *)malloc(CWD_PEEK_BYTES + 1);
    if (!buf) { fclose(f); return NULL; }
    usize have = 0, searched = 0;
    char *out = NULL;
    static const char key[] = "\"cwd\":\"";
    const usize klen = sizeof key - 1;
    while (have < CWD_PEEK_BYTES && !out) {
        usize want = 8192;
        if (want > CWD_PEEK_BYTES - have) want = CWD_PEEK_BYTES - have;
        usize n = scan_fread(buf + have, 1, want, f);
        if (n == 0) break;
        have += n;
        buf[have] = '\0';
        /* Resume a little before where the last pass stopped: the key may
         * straddle the chunk boundary. */
        usize from = searched > klen ? searched - klen : 0;
        char *hit = NULL;
        for (usize i = from; i + klen <= have; i++) {
            if (buf[i] == '"' && memcmp(buf + i, key, klen) == 0) { hit = buf + i + klen; break; }
        }
        searched = have;
        if (!hit) continue;
        /* The value: a JSON string. Decode the escapes a path can carry. */
        char tmp[1024];
        usize o = 0;
        const char *p = hit;
        bool closed = false, bad = false;
        while (p < buf + have) {
            unsigned char c = (unsigned char)*p++;
            if (c == '"') { closed = true; break; }
            if (c < 0x20 || o + 4 >= sizeof tmp) { bad = true; break; }
            if (c != '\\') { tmp[o++] = (char)c; continue; }
            if (p >= buf + have) break;                       /* cut by the window */
            char e = *p++;
            if (e == '"' || e == '\\' || e == '/') tmp[o++] = e;
            else if (e == 'u' && p + 4 <= buf + have) {
                u32 cp = 0;
                for (int k = 0; k < 4; k++) {
                    char h = *p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (u32)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (u32)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (u32)(h - 'A' + 10);
                    else { bad = true; break; }
                }
                if (bad) break;
                if (cp < 0x80) tmp[o++] = (char)cp;
                else if (cp < 0x800) { tmp[o++] = (char)(0xC0 | (cp >> 6)); tmp[o++] = (char)(0x80 | (cp & 0x3F)); }
                else if (cp >= 0xD800 && cp <= 0xDFFF) { bad = true; break; }   /* astral escapes: leave to the slug */
                else { tmp[o++] = (char)(0xE0 | (cp >> 12)); tmp[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[o++] = (char)(0x80 | (cp & 0x3F)); }
            } else { bad = true; break; }
        }
        if (bad) break;
        if (!closed) continue;                                /* value cut by the window: read more */
        if (o == 0 || tmp[0] != '/') break;                   /* not an absolute path */
        out = arena_alloc(a, o + 1);
        if (out) { memcpy(out, tmp, o); out[o] = '\0'; }
        break;
    }
    free(buf);
    fclose(f);
    return out;
}

/* Is this rollout a SUB-AGENT's thread? Its header says so:
 * `"source":{"subagent":{…}}`, where a session the user started reads
 * `"source":"cli"` / `"vscode"` / `"exec"`.
 *
 * Such a thread is a fork of its parent — it carries the parent's history, so
 * it reads as the same conversation again — and Codex's own resume picker does
 * not offer it. Sub-agent threads can outnumber the user's own sessions and
 * would push them out of the newest rows Agent History shows. Claude Code's
 * sidechain transcripts are left out for the same reason.
 *
 * `line` is the start of the rollout's first line, where the key sits. A raw
 * `"source"` can only be the record's own key: inside a string value the
 * quotes are escaped. */
static bool codex_header_is_subagent(const char *line) {
    const char *k = line ? strstr(line, "\"source\"") : NULL;
    if (!k) return false;
    const char *p = k + 8;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '{') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return strncmp(p, "\"subagent\"", 10) == 0;
}

/* Codex stores a JSON object on the very first line of every rollout file
 * containing a "cwd" string field. Read up to 8 KB (covers any reasonable
 * header size; Codex emits ~400 B), find `"cwd":"…"`, and arena-dup the
 * value. Returns NULL on any I/O or parse failure — caller treats the cwd
 * as unknown and falls back to project-slug matching. `*subagent` (the same
 * read) says whether the rollout is a sub-agent's thread. */
static const char *peek_codex_cwd(Arena *a, const char *path, bool *subagent) {
    if (subagent) *subagent = false;
    if (!a || !path) return NULL;
    FILE *f = scan_fopen(path, "r");
    if (!f) return NULL;
    char buf[8192];
    usize n = scan_fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return NULL;
    buf[n] = '\0';

    /* Stop at the end of the first line so a `"cwd"` appearing inside
     * later message content is not picked up. */
    char *nl = memchr(buf, '\n', n);
    if (nl) *nl = '\0';
    if (subagent) *subagent = codex_header_is_subagent(buf);

    const char *key = strstr(buf, "\"cwd\"");
    if (!key) return NULL;
    const char *colon = strchr(key, ':');
    if (!colon) return NULL;
    const char *q1 = strchr(colon, '"');
    if (!q1) return NULL;
    q1++;
    const char *q2 = strchr(q1, '"');
    if (!q2 || q2 == q1) return NULL;
    usize cwd_len = (usize)(q2 - q1);
    if (cwd_len >= 1024) return NULL;        /* sanity */
    char *p = arena_alloc(a, cwd_len + 1);
    if (!p) return NULL;
    memcpy(p, q1, cwd_len);
    p[cwd_len] = '\0';
    return p;
}

/* Process-static scan arena reused by chat_scan_arena_borrow /
 * chat_scan_with_arena. Main-thread only (palette, transcript, CLI). */
static Arena g_scan_arena;
static bool  g_scan_arena_ready;

Arena chat_scan_arena_create(void) {
    return arena_create(CHAT_SCAN_ARENA_BYTES);
}

Arena *chat_scan_arena_borrow(void) {
    if (!g_scan_arena_ready) {
        g_scan_arena = arena_create(CHAT_SCAN_ARENA_BYTES);
        if (!g_scan_arena.base) return NULL;
        g_scan_arena_ready = true;
        /* The scan arena is charged to MEM_HISTORY so it appears in memory
         * reports. The arena is reserved in one block and RESET rather than
         * shrunk between scans, so the level is its full capacity for as long
         * as it lives — reporting occupancy instead would understate what the
         * process is actually holding.
         *
         * `set` rather than an adjust: this arena is main-thread-only (see the
         * note above the declaration) and there is exactly one of it. */
        mem_budget_set(MEM_HISTORY, CHAT_SCAN_ARENA_BYTES);
    } else {
        arena_reset(&g_scan_arena);
    }
    return &g_scan_arena;
}

void chat_scan_with_arena(u32 tools_mask, chat_session_cb cb, void *user) {
    Arena *a = chat_scan_arena_borrow();
    if (!a) return;
    chat_scan(a, tools_mask, cb, user);
}

/* Release process-lifetime scanner state. Idempotent. */
void chat_scan_shutdown(void) {
    if (g_scan_arena_ready) {
        arena_destroy(&g_scan_arena);
        g_scan_arena_ready = false;
        mem_budget_set(MEM_HISTORY, 0);
    }
}

static bool fill_meta_from_path(EntryVec *vec, ChatSessionMeta *m, ChatTool tool,
                                const char *dir, const char *project_slug,
                                const char *fname, const struct stat *st_in) {
    Arena *a = vec ? vec->arena : NULL;
    if (!a || scan_cancelled()) return false;
    m->tool = tool;

    m->path = arena_join_path(a, dir, fname);
    if (!m->path) return false;

    m->project = arena_dup_str(a, project_slug ? project_slug : "-");
    if (!m->project) return false;

    /* session_id = fname with its extension stripped (.jsonl or .json).
     *
     * For tools that store one session per directory under a fixed file
     * name — Cline-family ("ui_messages.json"), Copilot CLI
     * (events.jsonl), Kimi (context.jsonl) — the bare filename would
     * collide across every session, so the parent directory name is
     * promoted to the session id instead. The check is filename-based so
     * Kimi's *flat* layout (where the filename IS the unique id, e.g.
     * abc123.jsonl) keeps using the filename. */
    usize flen = strlen(fname);
    usize base_len = flen;
    if (flen > 6 && strcmp(fname + flen - 6, ".jsonl") == 0) base_len = flen - 6;
    else if (flen > 5 && strcmp(fname + flen - 5, ".json") == 0) base_len = flen - 5;

    bool fname_is_constant =
        (strcmp(fname, "ui_messages.json")   == 0 ||
         strcmp(fname, "events.jsonl")       == 0 ||
         strcmp(fname, "context.jsonl")      == 0 ||
         strcmp(fname, "wire.jsonl")         == 0 ||
         strcmp(fname, "chat_history.jsonl") == 0);   /* Grok: one dir per session */

    const char *src;
    usize src_len;
    if (base_len == 0 || fname_is_constant) {
        const char *slash = strrchr(dir, '/');
        const char *parent = slash ? slash + 1 : dir;
        src = parent;
        src_len = strlen(parent);
    } else {
        src = fname;
        src_len = base_len;
    }
    /* Pi and Oh My Pi name a file "<timestamp>_<uuid>.jsonl". What both CLIs
     * take to resume is the uuid (a prefix of it, or the path), and the whole
     * stem would carry the characters a pasted resume command may not. */
    if (tool == CHAT_TOOL_PI || tool == CHAT_TOOL_OMP) {
        const char *us = memchr(src, '_', src_len);
        if (us && (usize)(us - src) + 1 < src_len) {
            src_len -= (usize)(us - src) + 1;
            src = us + 1;
        }
    }
    m->session_id = arena_session_id(a, src, src_len);
    if (!m->session_id) return false;

    struct stat st;
    const struct stat *st_src = st_in;
    if (!st_src) {
        if (scan_lstat(m->path, &st) != 0) st_src = NULL;
        else st_src = &st;
    }

    if (st_src && S_ISREG(st_src->st_mode)) {
        m->size_bytes = (i64)st_src->st_size;
        m->last_modified_ms = (i64)st_src->st_mtime * 1000;
    } else {
        m->size_bytes = 0;
        m->last_modified_ms = 0;
    }
    m->event_count = 0;

    /* Best-effort cwd extraction so the picker can filter by current
     * project across more than just Claude. Costs:
     *   - Claude: zero I/O, just decodes the slug already at hand.
     *   - Codex:  one fopen + fread of ≤8 KB per session file. Run during
     *             a user-initiated scan, so the latency is acceptable.
     *   - Other tools: skipped (parsers would have to walk message bodies). */
    m->cwd = NULL;
    if (tool == CHAT_TOOL_CLAUDE) {
        /* The record's own cwd, or NOTHING: the directory name is a lossy
         * encoding and a path reversed from it is a guess. A session with no
         * record is matched by slug (chat_session_matches_cwd). */
        m->cwd = peek_record_cwd(a, m->path);
    } else if (tool == CHAT_TOOL_CODEX) {
        bool subagent = false;
        m->cwd = peek_codex_cwd(a, m->path, &subagent);
        if (subagent) return false;        /* not a session of the user's: not listed */
    } else if (tool == CHAT_TOOL_QWEN || tool == CHAT_TOOL_PI || tool == CHAT_TOOL_OMP) {
        /* Same story under `~/.qwen/projects/<slug>/chats`: the record when
         * there is one, the slug comparison otherwise. Pi and Oh My Pi put the
         * folder in the header line (their bucket name is a lossy slug of it:
         * "/" and "-" both read as "-"). */
        m->cwd = peek_record_cwd(a, m->path);
    } else if (tool == CHAT_TOOL_KIMI) {
        /* scan_kimi resolves the bucket hash back to the literal work dir
         * via kimi.json and passes it here, so the cwd is exact — no lossy
         * slug round-trip. Falls back to the hash when kimi.json has no
         * entry, which is not a path and must not be reported as a cwd. */
        if (project_slug && project_slug[0] == '/')
            m->cwd = arena_dup_str(a, project_slug);
    }
    return true;
}

/* Walk a single directory and push every *.jsonl file as a meta entry. */
static void scan_flat(ChatTool tool, const char *dir, const char *project_slug,
                      EntryVec *out) {
    DIR *d = scan_opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!ends_with(de->d_name, ".jsonl")) continue;
        char path[CHAT_PATH_CAP];
        int wrote = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (wrote <= 0 || (usize)wrote >= sizeof path) continue;
        struct stat st;
        if (scan_lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        ChatSessionMeta m = {0};
        if (fill_meta_from_path(out, &m, tool, dir, project_slug, de->d_name, &st))
            vec_push(out, &m);
    }
    closedir(d);
}

/* Recursively walk `root` up to `depth` levels looking for *.jsonl.
 * If `skip_top_files` is true, regular files directly under `root` are NOT
 * emitted — only subdirectories are descended. Useful when the caller already
 * did a flat pass over `root` and only wants nested entries picked up. */
static void scan_recursive_opts(ChatTool tool, const char *root, i32 depth,
                                const char *project_slug, bool skip_top_files,
                                EntryVec *out) {
    if (depth < 0) return;
    DIR *d = scan_opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[CHAT_PATH_CAP];
        int wrote = snprintf(child, sizeof child, "%s/%s", root, de->d_name);
        if (wrote <= 0 || (usize)wrote >= sizeof child) continue;
        struct stat st;
        if (scan_lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_recursive_opts(tool, child, depth - 1, project_slug, false, out);
        } else if (!skip_top_files && S_ISREG(st.st_mode) &&
                   ends_with(de->d_name, ".jsonl")) {
            ChatSessionMeta m = {0};
            if (fill_meta_from_path(out, &m, tool, root, project_slug, de->d_name, &st))
                vec_push(out, &m);
        }
    }
    closedir(d);
}

static void scan_recursive(ChatTool tool, const char *root, i32 depth,
                           const char *project_slug, EntryVec *out) {
    scan_recursive_opts(tool, root, depth, project_slug, false, out);
}

static void scan_claude(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_resolve_root(root, sizeof root,
                           "CLAUDE_CONFIG_DIR", "projects",
                           ".claude/projects")) return;
    DIR *d = scan_opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char proj[CHAT_PATH_CAP];
        int wrote = snprintf(proj, sizeof proj, "%s/%s", root, de->d_name);
        if (wrote <= 0 || (usize)wrote >= sizeof proj) continue;
        struct stat st;
        if (scan_lstat(proj, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        scan_flat(CHAT_TOOL_CLAUDE, proj, de->d_name, out);
    }
    closedir(d);
}

static void scan_codex(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (hist_resolve_root(root, sizeof root,
                          "CODEX_HOME", "sessions",
                          ".codex/sessions")) {
        /* Date nesting: YYYY/MM/DD/rollout-*.jsonl → 3 levels. */
        scan_recursive(CHAT_TOOL_CODEX, root, 3, NULL, out);
    }
    /* `codex apply`/cleanup moves cold rollouts here — flat in practice,
     * but the same depth-3 walk covers either shape. (Background-compressed
     * *.jsonl.zst siblings are skipped by the .jsonl suffix match.) */
    if (hist_resolve_root(root, sizeof root,
                          "CODEX_HOME", "archived_sessions",
                          ".codex/archived_sessions")) {
        scan_recursive(CHAT_TOOL_CODEX, root, 3, NULL, out);
    }
}

static void scan_copilot(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    char primary[CHAT_PATH_CAP] = {0};
    if (hist_resolve_root(root, sizeof root,
                          "COPILOT_HOME", "session-state",
                          ".copilot/session-state")) {
        snprintf(primary, sizeof primary, "%s", root);
        scan_recursive(CHAT_TOOL_COPILOT, root, 4, NULL, out);
    }
    /* Copilot CLI also lands under XDG config/state roots on Linux when
     * those vars are set (dot-prefixed subdir — non-spec but real). Probe
     * them only when the env var exists; skip exact duplicates of the
     * primary root. */
    if (hist_env_dir("XDG_CONFIG_HOME") &&
        hist_xdg_config_path(root, sizeof root, ".copilot/session-state") &&
        strcmp(root, primary) != 0) {
        scan_recursive(CHAT_TOOL_COPILOT, root, 4, NULL, out);
    }
    if (hist_env_dir("XDG_STATE_HOME") &&
        hist_xdg_state_path(root, sizeof root, ".copilot/session-state") &&
        strcmp(root, primary) != 0) {
        scan_recursive(CHAT_TOOL_COPILOT, root, 4, NULL, out);
    }
}

static void scan_antigravity_dir(EntryVec *out, const char *root) {
    DIR *d = scan_opendir(root);
    if (!d) return;

    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        usize nlen = strlen(de->d_name);
        if (nlen < 4 || strcmp(de->d_name + nlen - 3, ".db") != 0) continue;

        char fpath[CHAT_PATH_CAP];
        int wrote = snprintf(fpath, sizeof fpath, "%s/%s", root, de->d_name);
        if (wrote <= 0 || (usize)wrote >= sizeof fpath) continue;

        struct stat st;
        if (scan_lstat(fpath, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        sqlite3 *db = NULL;
        if (scan_sql_open(fpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            continue;
        }

        const char *sql = "SELECT trajectory_id FROM trajectory_meta LIMIT 1";
        sqlite3_stmt *stmt = NULL;
        if (scan_sql_prepare(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            sqlite3_close(db);
            continue;
        }

        if (scan_sql_step(stmt) == SQLITE_ROW) {
            const char *tid = (const char *)sqlite3_column_text(stmt, 0);
            if (tid && tid[0]) {
                ChatSessionMeta m = {0};
                m.tool             = CHAT_TOOL_ANTIGRAVITY;
                m.path             = arena_dup_str(out->arena, fpath);
                m.session_id       = arena_dup_str(out->arena, tid);
                m.project          = arena_dup_str(out->arena, "-");
                m.cwd              = NULL;
                m.last_modified_ms = (i64)st.st_mtime * 1000;
                m.size_bytes       = (i64)st.st_size;
                m.event_count      = 0;
                if (m.path && m.session_id && m.project) vec_push(out, &m);
            }
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
    closedir(d);
}

static void scan_antigravity(EntryVec *out) {
    if (!out || !out->arena) return;

    /* Idempotent and thread-safe by SQLite's own contract, so a worker-thread
     * scan and a main-thread find can both call it without a flag of their
     * own. */
    sqlite3_initialize();

    char root[CHAT_PATH_CAP];
    if (hist_resolve_root(root, sizeof root,
                          "GEMINI_CLI_HOME", "antigravity/conversations",
                          ".gemini/antigravity/conversations")) {
        scan_antigravity_dir(out, root);
    }
    if (hist_resolve_root(root, sizeof root,
                          "GEMINI_CLI_HOME", "antigravity-cli/conversations",
                          ".gemini/antigravity-cli/conversations")) {
        scan_antigravity_dir(out, root);
    }
}

/* VS Code globalStorage product list, ordered to match the
 * CHAT_VSCODE_HOST_* bit indices in session.h:
 *   bit 0 = Code, 1 = Code - Insiders, 2 = Cursor, 3 = VSCodium
 *
 * Building one base path per requested host avoids scanning all four
 * when the UI knows which IDE the user lives in. */
#define VSCODE_HOST_COUNT 4
static const char *kVSCodeProducts[VSCODE_HOST_COUNT] = {
    "Code", "Code - Insiders", "Cursor", "VSCodium",
};

/* Build globalStorage root for host bit `h` into `out`. Returns true on
 * success. Empty path / unsupported platform → false. */
static bool vscode_storage_root_for(i32 h, char *out, usize cap) {
    if (h < 0 || h >= VSCODE_HOST_COUNT) return false;
    /* The roaming app-data base covers all three OSes (and honors
     * $XDG_CONFIG_HOME on Linux):
     *   mac   ~/Library/Application Support/<Host>/User/globalStorage
     *   linux $XDG_CONFIG_HOME|~/.config/<Host>/User/globalStorage
     *   win   %APPDATA%/<Host>/User/globalStorage  */
    char rel[CHAT_PATH_CAP];
    int w = snprintf(rel, sizeof rel, "%s/User/globalStorage", kVSCodeProducts[h]);
    if (w <= 0 || (usize)w >= sizeof rel) return false;
    return hist_appdata_path(out, cap, rel);
}

/* macOS .app bundle name per VS Code host. Lined up with kVSCodeProducts so
 * the host bit index addresses both arrays. */
static const char *kVSCodeAppBundle[VSCODE_HOST_COUNT] = {
    "Visual Studio Code",
    "Visual Studio Code - Insiders",
    "Cursor",
    "VSCodium",
};

/* Per-host extensions directory (relative to $HOME). Detecting an extension
 * means probing this dir for a subdirectory whose name starts with
 * "<publisher>.<name>". VS Code installs are immutable per-version so each
 * extension dir is suffixed with "-<version>". */
static const char *kVSCodeExtSubdir[VSCODE_HOST_COUNT] = {
    ".vscode/extensions",
    ".vscode-insiders/extensions",
    ".cursor/extensions",
    ".vscode-oss/extensions",
};

/* On macOS, check application bundles in standard locations, then the
 * Application Support directory to detect custom or external-volume
 * installs. On other platforms, the extension-directory probe is the gate.
 * This avoids including stale globalStorage for an IDE that is not
 * installed. */
static bool vscode_app_installed(i32 h) {
    if (h < 0 || h >= VSCODE_HOST_COUNT) return false;
#if defined(__APPLE__)
    char p[CHAT_PATH_CAP];
    struct stat st;
    int w = snprintf(p, sizeof p, "/Applications/%s.app", kVSCodeAppBundle[h]);
    if (w > 0 && (usize)w < sizeof p &&
        scan_stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    char home[CHAT_PATH_CAP];
    if (hist_home_path(home, sizeof home, "Applications")) {
        w = snprintf(p, sizeof p, "%s/%s.app", home, kVSCodeAppBundle[h]);
        if (w > 0 && (usize)w < sizeof p &&
            scan_stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    }
    /* Fallback: IDE has been launched at least once on this box (Setapp,
     * non-standard install path, removable-drive bundle, …). */
    if (hist_home_path(p, sizeof p, "Library/Application Support")) {
        usize n = strlen(p);
        if (n + 2 + strlen(kVSCodeProducts[h]) < sizeof p) {
            snprintf(p + n, sizeof p - n, "/%s", kVSCodeProducts[h]);
            if (scan_stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return true;
        }
    }
    return false;
#else
    (void)h;
    return true;
#endif
}

/* Is `ext_id` installed in host `h`'s extensions directory? Looks for any
 * subdirectory whose name starts with "<ext_id>-" (VS Code appends the
 * extension version) or matches `<ext_id>` exactly. */
static bool vscode_ext_installed(i32 h, const char *ext_id) {
    if (h < 0 || h >= VSCODE_HOST_COUNT || !ext_id || !*ext_id) return false;
    char dir[CHAT_PATH_CAP];
    if (!hist_home_path(dir, sizeof dir, kVSCodeExtSubdir[h])) return false;
    DIR *d = scan_opendir(dir);
    if (!d) return false;
    bool found = false;
    usize n = strlen(ext_id);
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strncmp(de->d_name, ext_id, n) == 0 &&
            (de->d_name[n] == '-' || de->d_name[n] == '\0')) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Walk `dir` shallowly and push every regular file whose name ends in either
 * suffix as a meta entry, tagged with `tool`. Useful when an agent drops flat
 * json/jsonl files under one directory (amp, kiro, kimi, ...). */
static void scan_flat_exts(ChatTool tool, const char *dir,
                           const char *project_slug,
                           const char *s1, const char *s2,
                           EntryVec *out) {
    DIR *d = scan_opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if ((!s1 || !ends_with(de->d_name, s1)) &&
            (!s2 || !ends_with(de->d_name, s2))) continue;
        char path[CHAT_PATH_CAP];
        int wrote = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (wrote <= 0 || (usize)wrote >= sizeof path) continue;
        struct stat st;
        if (scan_lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        ChatSessionMeta m = {0};
        if (fill_meta_from_path(out, &m, tool, dir, project_slug, de->d_name, &st))
            vec_push(out, &m);
    }
    closedir(d);
}

static void scan_amp(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_xdg_data_path(root, sizeof root, "amp/threads")) return;
    scan_flat_exts(CHAT_TOOL_AMP, root, NULL, ".json", ".jsonl", out);
}

/* Kimi work-dir table, recovered from <share>/kimi.json.
 *
 * Kimi buckets sessions under md5(<work dir>) (kimi_cli metadata.py,
 * WorkDirMeta.sessions_dir), so the directory name on disk is a hash and
 * carries no project identity. kimi.json lists every work dir the CLI has
 * seen, so hashing those recovers bucket → real path: the picker then shows
 * "liuterm" instead of "42fe0a40bb43…", and cwd-scoped history works. */
enum { KIMI_WORKDIR_MAX = 128 };
typedef struct {
    char bucket[40];                  /* md5 hex, or "<kaos>_<md5>" */
    char path[CHAT_PATH_CAP];
} KimiWorkDir;

static i32 kimi_load_workdirs(KimiWorkDir *tab, i32 cap) {
    char meta_path[CHAT_PATH_CAP];
    if (!hist_resolve_root(meta_path, sizeof meta_path, "KIMI_SHARE_DIR",
                           "kimi.json", ".kimi/kimi.json")) return 0;

    /* Tiny file (one entry per work dir ever opened). */
    char buf[64 * 1024];
    FILE *f = scan_fopen(meta_path, "rb");
    if (!f) return 0;
    usize n = scan_fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return 0;
    cJSON *dirs = cJSON_GetObjectItemCaseSensitive(root, "work_dirs");
    i32 count = 0;
    if (cJSON_IsArray(dirs)) {
        cJSON *wd = NULL;
        cJSON_ArrayForEach(wd, dirs) {
            if (scan_cancelled()) break;
            if (count >= cap) break;
            const char *p = hist_cjson_str(wd, "path");
            if (!p || !p[0]) continue;
            const char *kaos = hist_cjson_str(wd, "kaos");
            char digest[33];
            hist_md5_hex(p, strlen(p), digest);
            /* Non-local KAOS prefixes the bucket with its name; "local" is
             * the bare digest. Mirrors WorkDirMeta.sessions_dir exactly. */
            int w = (kaos && kaos[0] && strcmp(kaos, "local") != 0)
                  ? snprintf(tab[count].bucket, sizeof tab[count].bucket,
                             "%s_%s", kaos, digest)
                  : snprintf(tab[count].bucket, sizeof tab[count].bucket,
                             "%s", digest);
            if (w <= 0 || (usize)w >= sizeof tab[count].bucket) continue;
            hist_copy_bounded(tab[count].path, sizeof tab[count].path, p);
            count++;
        }
    }
    cJSON_Delete(root);
    return count;
}

static const char *kimi_lookup_workdir(const KimiWorkDir *tab, i32 count,
                                       const char *bucket) {
    for (i32 i = 0; i < count; i++)
        if (strcmp(tab[i].bucket, bucket) == 0) return tab[i].path;
    return NULL;
}

static void scan_kimi(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_resolve_root(root, sizeof root, "KIMI_SHARE_DIR", "sessions",
                           ".kimi/sessions")) return;

    /* Layout (kimi-cli 1.12 and 1.49):
     *
     *   <share>/sessions/<bucket>/<session-uuid>/context.jsonl   ← current
     *   <share>/sessions/<bucket>/<session-uuid>.jsonl           ← legacy
     *
     * with <bucket> = md5(work dir). The legacy flat file is migrated into a
     * per-session directory the next time that session is opened
     * (session.py _migrate_session_context_file), so both shapes coexist on
     * an install with history — Session.list() itself accepts either.
     *
     * The work-dir bucket sits between `sessions/` and the session
     * directory, so each bucket is walked in turn.
     *
     * Only context.jsonl is emitted — wire.jsonl is the raw API/event
     * transcript for the same conversation and would double every row. */
    KimiWorkDir *dirs = calloc(KIMI_WORKDIR_MAX, sizeof *dirs);
    i32 dir_count = dirs ? kimi_load_workdirs(dirs, KIMI_WORKDIR_MAX) : 0;

    DIR *d = scan_opendir(root);
    if (!d) { free(dirs); return; }
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char bucket[CHAT_PATH_CAP];
        int w = snprintf(bucket, sizeof bucket, "%s/%s", root, de->d_name);
        if (w <= 0 || (usize)w >= sizeof bucket) continue;
        struct stat bst;
        if (scan_lstat(bucket, &bst) != 0 || !S_ISDIR(bst.st_mode)) continue;

        /* Absolute work dir when it can be named; otherwise the hash, so
         * the row still has a stable (if opaque) project label. */
        const char *project = kimi_lookup_workdir(dirs, dir_count, de->d_name);
        if (!project) project = de->d_name;

        /* Legacy flat sessions live INSIDE the bucket, not at the root. */
        scan_flat_exts(CHAT_TOOL_KIMI, bucket, project, ".json", ".jsonl", out);

        DIR *bd = scan_opendir(bucket);
        if (!bd) continue;
        struct dirent *se;
        while ((se = scan_readdir(bd)) != NULL) {
            if (se->d_name[0] == '.') continue;
            char sess[CHAT_PATH_CAP];
            w = snprintf(sess, sizeof sess, "%s/%s", bucket, se->d_name);
            if (w <= 0 || (usize)w >= sizeof sess) continue;
            struct stat st;
            if (scan_lstat(sess, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            char ctx[CHAT_PATH_CAP];
            w = snprintf(ctx, sizeof ctx, "%s/context.jsonl", sess);
            if (w <= 0 || (usize)w >= sizeof ctx) continue;
            struct stat cst;
            /* A session created but never prompted leaves a zero-byte
             * context.jsonl; Kimi hides those (Session.list → is_empty) and
             * so does this scan, rather than filling the picker with empty
             * rows. */
            if (scan_lstat(ctx, &cst) != 0 || !S_ISREG(cst.st_mode) ||
                cst.st_size <= 0) continue;
            ChatSessionMeta m = {0};
            if (fill_meta_from_path(out, &m, CHAT_TOOL_KIMI, sess, project,
                                    "context.jsonl", &cst))
                vec_push(out, &m);
        }
        closedir(bd);
    }
    closedir(d);
    free(dirs);
}

/* Kiro stores session-per-workspace. Two layouts seen in the wild:
 *   - newer: ~/.kiro/sessions/<workspace>/<session>.json
 *   - older: ~/Library/Application Support/Kiro/workspace-sessions/<ws>/<id>.json (macOS)
 *            ~/.config/Kiro/workspace-sessions/<ws>/<id>.json (Linux)
 * Walk both: a current install only populates the dotfile root. */
static void scan_kiro_root(const char *root, EntryVec *out) {
    DIR *d = scan_opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char ws[CHAT_PATH_CAP];
        int w = snprintf(ws, sizeof ws, "%s/%s", root, de->d_name);
        if (w <= 0 || (usize)w >= sizeof ws) continue;
        struct stat st;
        if (scan_lstat(ws, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        scan_flat_exts(CHAT_TOOL_KIRO, ws, de->d_name, ".json", NULL, out);
    }
    closedir(d);
}

static void scan_kiro(EntryVec *out) {
    char root[CHAT_PATH_CAP];

    /* Newer dotfile root. */
    if (hist_home_path(root, sizeof root, ".kiro/sessions"))
        scan_kiro_root(root, out);

    /* Legacy GUI-app root — roaming app data on every OS (mac App Support,
     * linux XDG config, windows %APPDATA%). */
    if (hist_appdata_path(root, sizeof root, "Kiro/workspace-sessions"))
        scan_kiro_root(root, out);
}

static void scan_droid(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_home_path(root, sizeof root, ".factory/sessions")) return;
    scan_flat_exts(CHAT_TOOL_DROID, root, NULL, ".json", ".jsonl", out);
    /* Some installations nest by date — recurse into subdirs only; top-level
     * files were already picked up by the flat pass above. */
    scan_recursive_opts(CHAT_TOOL_DROID, root, 3, NULL, true, out);
}

static void scan_cursor(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_home_path(root, sizeof root, ".cursor/projects")) return;
    DIR *d = scan_opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char proj[CHAT_PATH_CAP];
        int w = snprintf(proj, sizeof proj, "%s/%s/agent-transcripts",
                         root, de->d_name);
        if (w <= 0 || (usize)w >= sizeof proj) continue;
        struct stat st;
        if (scan_lstat(proj, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        /* Cursor stores each transcript one directory below the project
         * tree: agent-transcripts/<sess-id>/<sess-id>.jsonl. Scan each
         * session directory to find its transcript. */
        DIR *td = scan_opendir(proj);
        if (!td) continue;
        struct dirent *tde;
        while ((tde = scan_readdir(td)) != NULL) {
            if (tde->d_name[0] == '.') continue;
            char sess_dir[CHAT_PATH_CAP];
            int w2 = snprintf(sess_dir, sizeof sess_dir, "%s/%s",
                              proj, tde->d_name);
            if (w2 <= 0 || (usize)w2 >= sizeof sess_dir) continue;
            struct stat sst;
            if (scan_lstat(sess_dir, &sst) != 0 || !S_ISDIR(sst.st_mode)) continue;
            scan_flat_exts(CHAT_TOOL_CURSOR, sess_dir, de->d_name,
                           ".jsonl", ".json", out);
        }
        closedir(td);
    }
    closedir(d);
}

static void scan_qwen(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    /* Precedence mirrors upstream: $QWEN_RUNTIME_DIR > $QWEN_HOME > ~/.qwen. */
    const char *rt = hist_env_dir("QWEN_RUNTIME_DIR");
    if (rt) {
        int w = snprintf(root, sizeof root, "%s/projects", rt);
        if (w <= 0 || (usize)w >= sizeof root) return;
    } else if (!hist_resolve_root(root, sizeof root,
                                  "QWEN_HOME", "projects",
                                  ".qwen/projects")) {
        return;
    }
    DIR *d = scan_opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char chats[CHAT_PATH_CAP];
        int w = snprintf(chats, sizeof chats, "%s/%s/chats", root, de->d_name);
        if (w <= 0 || (usize)w >= sizeof chats) continue;
        struct stat st;
        if (scan_lstat(chats, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        scan_flat_exts(CHAT_TOOL_QWEN, chats, de->d_name, ".jsonl", ".json", out);
    }
    closedir(d);
}

/* OpenCode keeps every session in a SQLite DB at
 * $XDG_DATA_HOME/opencode/opencode.db (or
 * ~/.local/share/opencode/opencode.db). The `session` table (2.x:
 * `session_v2`, see history/opencode_store.h) holds the per-session cwd in
 * its `directory` column, the title in `title`, and a millisecond timestamp
 * in `time_updated`. The table is queried directly so the cwd-aware history
 * filter works for OpenCode, and so that users who don't dump session JSONs
 * to disk still see their sessions in the picker. The DB is opened read-only
 * with the `?immutable=1` URI flag so the scan does not compete with a
 * running OpenCode for the write lock. */
static void scan_opencode(EntryVec *out) {
    if (!out || !out->arena) return;

    /* Resolve the SQLite file under whichever storage root the user has.
     * opencode honors $XDG_DATA_HOME on every OS (Windows included) and
     * otherwise writes ~/.local/share even there — NOT %LOCALAPPDATA%. */
    char db_path[CHAT_PATH_CAP];
    if (!hist_xdg_data_path(db_path, sizeof db_path, "opencode/opencode.db"))
        return;

    struct stat st;
    if (scan_stat(db_path, &st) != 0 || !S_ISREG(st.st_mode)) return;

    /* The vendored sqlite3 build sets SQLITE_OMIT_AUTOINIT, so callers must
     * initialize the library explicitly. The function is idempotent and
     * cheap on repeat calls. */
    sqlite3_initialize();

    sqlite3 *db = NULL;
    if (scan_sql_open(db_path,
                        &db,
                        SQLITE_OPEN_READONLY,
                        NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }

    /* Listing must never touch message BODIES — only their count.
     *
     * OpenCode keeps its sessions in this DB rather than in per-session
     * files, so there is nothing to stat for size_bytes, and summing the
     * message lengths, COALESCE((SELECT SUM(LENGTH(m.data)) FROM message m
     * WHERE ...), 0), is no substitute:
     *
     *  - Memory. message.data is TEXT and LENGTH() forces SQLite to
     *    reassemble each value out of its overflow pages in one contiguous
     *    block, so the largest single message sets the high-water mark for
     *    the whole scan, whatever the listing asked for. Neither
     *    CAST(data AS BLOB) nor a smaller PRAGMA cache_size helps — the
     *    value has to be materialized either way, so it is not read at all.
     *
     *  - Semantics. LENGTH() on a TEXT value counts CHARACTERS, not bytes,
     *    so the number would not measure what size_bytes means.
     *
     * size_bytes therefore stays 0 for OpenCode rows and callers should show
     * event_count instead. The COUNT(*) subquery rides the
     * (session_id, time_created, id) index and never opens a value. */
    /* The two shapes of the store (history/opencode_store.h). 2.x counts only
     * the user and assistant rows — its table also holds a row per idle mark,
     * model switch and notice, which are not messages — and leaves out the
     * sessions a sub-agent ran in (`parent_id`; most of a busy store). The
     * (session_id, type, seq) index answers the count without opening a row. */
    const char *sql = opencode_store_is_v2(db)
        ? "SELECT s.id, s.directory, s.title, s.time_updated, "
          "       (SELECT COUNT(*) FROM session_message m "
          "        WHERE m.session_id = s.id AND m.type IN ('user','assistant')) "
          "FROM session_v2 s WHERE s.parent_id IS NULL ORDER BY s.time_updated DESC"
        : "SELECT s.id, s.directory, s.title, s.time_updated, "
          "       (SELECT COUNT(*) FROM message m WHERE m.session_id = s.id) "
          "FROM session s ORDER BY s.time_updated DESC";
    sqlite3_stmt *stmt = NULL;
    if (scan_sql_prepare(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    while (scan_sql_step(stmt) == SQLITE_ROW) {
        const char *id        = (const char *)sqlite3_column_text(stmt, 0);
        const char *directory = (const char *)sqlite3_column_text(stmt, 1);
        const char *title     = (const char *)sqlite3_column_text(stmt, 2);
        sqlite3_int64 t_ms    = sqlite3_column_int64(stmt, 3);
        sqlite3_int64 msgs    = sqlite3_column_int64(stmt, 4);
        if (!id || !id[0]) continue;

        ChatSessionMeta m = {0};
        m.tool = CHAT_TOOL_OPENCODE;

        /* path: there is no per-session file; point at the DB so callers
         * that try to open the path see something coherent (the resume CLI
         * uses --session=<id> anyway, not a path). */
        m.path        = arena_dup_str(out->arena, db_path);
        m.session_id  = arena_dup_str(out->arena, id);
        m.project     = arena_dup_str(out->arena,
                                      title && title[0] ? title : "-");
        m.cwd         = directory && directory[0]
                          ? arena_dup_str(out->arena, directory)
                          : NULL;
        m.last_modified_ms = (i64)t_ms;
        m.size_bytes       = 0;   /* see the query comment: never read bodies */
        m.event_count      = (u32)(msgs < 0 ? 0 : msgs);

        if (m.path && m.session_id && m.project) vec_push(out, &m);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

/* Hermes Agent: one SQLite file per profile, `$HERMES_HOME/state.db` (default
 * ~/.hermes/state.db; a named profile's is ~/.hermes/profiles/<name>/state.db).
 * The schema follows Hermes's own source (hermes_state_common.py, schema 31).
 *
 * `sessions.source` says where a session ran: "cli" / "tui" are a terminal's,
 * the messaging gateways' ("telegram", …) and "oneshot" (`hermes -z`, hidden
 * from its own pickers) are not listed here. A hidden or archived row is not
 * either. Times are REAL epoch seconds. The messages are never touched by the
 * scan: the count is the column the session row keeps. */
static void scan_hermes_db(const char *db_path, EntryVec *out) {
    struct stat st;
    if (scan_stat(db_path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    sqlite3_initialize();
    sqlite3 *db = NULL;
    if (scan_sql_open(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    static const char sql[] =
        "SELECT id, title, cwd, started_at, ended_at, message_count FROM sessions "
        "WHERE COALESCE(hidden, 0) = 0 AND COALESCE(archived, 0) = 0 "
        "AND source IN ('cli', 'tui') "
        "ORDER BY COALESCE(ended_at, started_at) DESC";
    sqlite3_stmt *stmt = NULL;
    if (scan_sql_prepare(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }
    while (scan_sql_step(stmt) == SQLITE_ROW) {
        const char *id    = (const char *)sqlite3_column_text(stmt, 0);
        const char *title = (const char *)sqlite3_column_text(stmt, 1);
        const char *cwd   = (const char *)sqlite3_column_text(stmt, 2);
        double started    = sqlite3_column_double(stmt, 3);
        double ended      = sqlite3_column_type(stmt, 4) == SQLITE_NULL ? 0.0 : sqlite3_column_double(stmt, 4);
        sqlite3_int64 msgs = sqlite3_column_int64(stmt, 5);
        if (!id || !id[0]) continue;

        ChatSessionMeta m = {0};
        m.tool       = CHAT_TOOL_HERMES;
        m.path       = arena_dup_str(out->arena, db_path);
        m.session_id = arena_dup_str(out->arena, id);
        m.project    = arena_dup_str(out->arena, title && title[0] ? title : "-");
        m.cwd        = cwd && cwd[0] == '/' ? arena_dup_str(out->arena, cwd) : NULL;
        m.last_modified_ms = (i64)((ended > started ? ended : started) * 1000.0);
        m.size_bytes       = 0;
        m.event_count      = (u32)(msgs < 0 ? 0 : msgs);
        if (m.path && m.session_id && m.project) vec_push(out, &m);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void scan_hermes(EntryVec *out) {
    if (!out || !out->arena) return;
    char path[CHAT_PATH_CAP];
    const char *home_env = getenv("HERMES_HOME");
    char root[CHAT_PATH_CAP];
    if (home_env && home_env[0] == '/') snprintf(root, sizeof root, "%s", home_env);
    else if (!hist_home_path(root, sizeof root, ".hermes")) return;

    int n = snprintf(path, sizeof path, "%s/state.db", root);
    if (n > 0 && (usize)n < sizeof path) scan_hermes_db(path, out);

    char profiles[CHAT_PATH_CAP];
    n = snprintf(profiles, sizeof profiles, "%s/profiles", root);
    if (n <= 0 || (usize)n >= sizeof profiles) return;
    DIR *d = scan_opendir(profiles);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        n = snprintf(path, sizeof path, "%s/%s/state.db", profiles, de->d_name);
        if (n > 0 && (usize)n < sizeof path) scan_hermes_db(path, out);
    }
    closedir(d);
}

/* OpenClaw: `$OPENCLAW_STATE_DIR` (default ~/.openclaw)/agents/<agent>/agent/
 * openclaw-agent.sqlite. Sessions are keyed `agent:<agent>:<key>` and belong to
 * an AGENT with a workspace, not to the folder a terminal was in, so a row has
 * no cwd and the "this folder" filter never matches one. Schema version 25;
 * the transcript is not read at all: its events are zstd-compressed above
 * 1 KiB. `status` / `archived_at` leave out what its own picker hides. */
static void scan_openclaw_db(const char *db_path, const char *agent, EntryVec *out) {
    struct stat st;
    if (scan_stat(db_path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    sqlite3_initialize();
    sqlite3 *db = NULL;
    if (scan_sql_open(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    static const char sql[] =
        "SELECT n.session_key, COALESCE(n.display_name, n.label), "
        "       COALESCE(n.last_interaction_at, n.updated_at, n.created_at), "
        "       (SELECT COUNT(*) FROM transcript_events e WHERE e.session_id = n.current_session_id) "
        "FROM session_nodes n WHERE n.archived_at IS NULL "
        "ORDER BY COALESCE(n.last_interaction_at, n.updated_at, n.created_at) DESC";
    sqlite3_stmt *stmt = NULL;
    if (scan_sql_prepare(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }
    while (scan_sql_step(stmt) == SQLITE_ROW) {
        const char *key   = (const char *)sqlite3_column_text(stmt, 0);
        const char *title = (const char *)sqlite3_column_text(stmt, 1);
        sqlite3_int64 t_ms = sqlite3_column_int64(stmt, 2);
        sqlite3_int64 evs  = sqlite3_column_int64(stmt, 3);
        if (!key || !key[0]) continue;
        ChatSessionMeta m = {0};
        m.tool       = CHAT_TOOL_OPENCLAW;
        m.path       = arena_dup_str(out->arena, db_path);
        m.session_id = arena_dup_str(out->arena, key);
        m.project    = arena_dup_str(out->arena, title && title[0] ? title : agent);
        m.cwd        = NULL;
        m.last_modified_ms = (i64)t_ms;
        m.size_bytes       = 0;
        m.event_count      = (u32)(evs < 0 ? 0 : evs);   /* events, header included: the index answers it */
        if (m.path && m.session_id && m.project) vec_push(out, &m);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void scan_openclaw(EntryVec *out) {
    if (!out || !out->arena) return;
    char agents[CHAT_PATH_CAP];
    const char *env = getenv("OPENCLAW_STATE_DIR");
    if (env && env[0] == '/') {
        int n = snprintf(agents, sizeof agents, "%s/agents", env);
        if (n <= 0 || (usize)n >= sizeof agents) return;
    } else if (!hist_home_path(agents, sizeof agents, ".openclaw/agents")) {
        return;
    }
    DIR *d = scan_opendir(agents);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char db[CHAT_PATH_CAP];
        int n = snprintf(db, sizeof db, "%s/%s/agent/openclaw-agent.sqlite", agents, de->d_name);
        if (n > 0 && (usize)n < sizeof db) scan_openclaw_db(db, de->d_name, out);
    }
    closedir(d);
}

/* Each Cline-family extension drops its sessions under a well-known ID. */
struct ClineExtMap {
    const char *ext_id;
    ChatTool    tool;
};

/* Iterate Cline-family extensions × VS Code-fork hosts, gated by two masks.
 *   tool_mask:  bits selecting CHAT_TOOL_CLINE/ROO/KILO (other bits ignored).
 *   host_mask:  bits selecting CHAT_VSCODE_HOST_* (Code/Insiders/Cursor/VSCodium).
 * Both masks must be non-zero for any work to happen. The same extension id
 * can map to several tool enum values (e.g. roo) — those rows just skip when
 * their tool bit is cleared. */
static void scan_cline_family_filtered(EntryVec *out, u32 tool_mask, u32 host_mask) {
    if (tool_mask == 0 || host_mask == 0) return;
    static const struct ClineExtMap extensions[] = {
        { "saoudrizwan.claude-dev",     CHAT_TOOL_CLINE },
        { "rooveterinaryinc.roo-cline", CHAT_TOOL_ROO   },
        { "roo-code.roo-cline",         CHAT_TOOL_ROO   },
        { "kilocode.kilo-code",         CHAT_TOOL_KILO  },
    };
    for (i32 h = 0; h < VSCODE_HOST_COUNT; h++) {
        if (!(host_mask & (1u << h))) continue;
        /* Detection step 1: IDE installed (app bundle present)?
         * Skips hosts whose globalStorage lingers after an uninstall. */
        if (!vscode_app_installed(h)) continue;
        char base[CHAT_PATH_CAP];
        if (!vscode_storage_root_for(h, base, sizeof base)) continue;
        for (usize e = 0; e < sizeof extensions / sizeof extensions[0]; e++) {
            if (!(tool_mask & (1u << extensions[e].tool))) continue;
            /* Detection step 2: extension actually installed in this IDE?
             * Probes ~/.vscode/extensions/<id>-* and equivalents. Without
             * this, an extension's leftover task data shows up months after
             * the user removed the extension. */
            if (!vscode_ext_installed(h, extensions[e].ext_id)) continue;
            char tasks_root[CHAT_PATH_CAP];
            int w = snprintf(tasks_root, sizeof tasks_root, "%s/%s/tasks",
                             base, extensions[e].ext_id);
            if (w <= 0 || (usize)w >= sizeof tasks_root) continue;
            DIR *d = scan_opendir(tasks_root);
            if (!d) continue;
            struct dirent *de;
            while ((de = scan_readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                char task_dir[CHAT_PATH_CAP];
                int w2 = snprintf(task_dir, sizeof task_dir, "%s/%s",
                                  tasks_root, de->d_name);
                if (w2 <= 0 || (usize)w2 >= sizeof task_dir) continue;
                struct stat st;
                if (scan_lstat(task_dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                char msg_path[CHAT_PATH_CAP];
                int w3 = snprintf(msg_path, sizeof msg_path,
                                  "%s/ui_messages.json", task_dir);
                if (w3 <= 0 || (usize)w3 >= sizeof msg_path) continue;
                struct stat mst;
                if (scan_lstat(msg_path, &mst) != 0 || !S_ISREG(mst.st_mode)) continue;
                ChatSessionMeta m = {0};
                if (fill_meta_from_path(out, &m, extensions[e].tool, task_dir,
                                        de->d_name, "ui_messages.json", &mst))
                    vec_push(out, &m);
            }
            closedir(d);
        }
    }
}

/* chat_scan's Cline-family entry point. host_mask = 0xF → all four
 * VSCODE_HOST_COUNT bits, i.e. Code + Insiders + Cursor + VSCodium. The
 * scanner silently skips any host whose globalStorage directory doesn't
 * exist, so this is also the de-facto IDE auto-detector for
 * installed-and-used VS Code forks.
 *
 * `tool_mask` is the CALLER's mask, forwarded verbatim. The three tools
 * share one storage root and one file name, so nothing downstream can tell
 * their rows apart: a fixed mask would make `--tool=kilo` list Cline and Roo
 * sessions too — the same silent widening the CLI's parse_tools_mask comment
 * describes for an unknown id. */
static void scan_cline_family(EntryVec *out, u32 tool_mask) {
    scan_cline_family_filtered(out, tool_mask, 0xFu);
}

/* Grok (xAI CLI): ~/.grok/sessions (GROK_HOME honored). Two layouts coexist:
 *   <root>/<uuid>/chat_history.jsonl                          — global
 *   <root>/<percent-encoded-cwd>/<uuid>/chat_history.jsonl    — per-project
 * One directory per session; summary.json beside the transcript names the
 * real cwd and the session kind. Internal sub-agent sessions
 * ("session_kind":"subagent") are skipped so the picker lists only the
 * user-facing chats. */
static bool grok_session_emit(EntryVec *out, const char *sess_dir,
                              const char *project_slug) {
    char chat[CHAT_PATH_CAP];
    int n = snprintf(chat, sizeof chat, "%s/chat_history.jsonl", sess_dir);
    if (n <= 0 || (usize)n >= sizeof chat) return false;
    struct stat st;
    if (scan_lstat(chat, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    /* summary.json: drop sub-agent worker sessions, lift the absolute cwd.
     *
     * The slurp goes into a TEMPORARY region of the shared scan arena. Only
     * the short cwd survives this block, while the raw JSON would otherwise
     * sit in the scan arena until the entire multi-tool scan finished. So:
     * park the cwd in a stack buffer, rewind the arena, then dup it
     * permanently afterwards — hist_strdup must not run before
     * arena_temp_end or the rewind reclaims the copy. Same rule for the
     * subagent early-out: it returns after the rewind, never from inside it.
     */
    char cwd_buf[CHAT_PATH_CAP];
    bool have_cwd = false;
    {
        ArenaTemp tmp = arena_temp_begin(out->arena);
        bool is_subagent = false;
        char sum[CHAT_PATH_CAP];
        int m2 = snprintf(sum, sizeof sum, "%s/summary.json", sess_dir);
        char *buf = NULL; usize blen = 0;
        if (m2 > 0 && (usize)m2 < sizeof sum &&
            scan_slurp(out->arena, sum, 64 * 1024, &buf, &blen)) {
            cJSON *root = cJSON_ParseWithLength(buf, blen);
            if (root) {
                const char *kind = hist_cjson_str(root, "session_kind");
                if (kind && strcmp(kind, "subagent") == 0) {
                    is_subagent = true;
                } else {
                    cJSON *info = cJSON_GetObjectItemCaseSensitive(root, "info");
                    const char *cw = info ? hist_cjson_str(info, "cwd") : NULL;
                    if (cw && cw[0] == '/') {
                        usize cwl = strlen(cw);
                        if (cwl < sizeof cwd_buf) {
                            memcpy(cwd_buf, cw, cwl + 1);
                            have_cwd = true;
                        }
                    }
                }
                cJSON_Delete(root);
            }
        }
        arena_temp_end(tmp);
        if (is_subagent) return false;
    }

    ChatSessionMeta m = {0};
    if (!fill_meta_from_path(out, &m, CHAT_TOOL_XAI, sess_dir, project_slug,
                             "chat_history.jsonl", &st))
        return false;
    if (have_cwd) {
        char *cwd_own = hist_strdup(out->arena, cwd_buf);
        if (cwd_own) m.cwd = cwd_own;   /* keep fill_meta's value on OOM */
    }
    vec_push(out, &m);
    return true;
}

static void scan_grok(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_resolve_root(root, sizeof root, "GROK_HOME", "sessions",
                           ".grok/sessions"))
        return;
    DIR *d = scan_opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[CHAT_PATH_CAP];
        int n = snprintf(child, sizeof child, "%s/%s", root, de->d_name);
        if (n <= 0 || (usize)n >= sizeof child) continue;
        struct stat st;
        if (scan_lstat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        /* A session dir directly under the root (global session)… */
        if (grok_session_emit(out, child, NULL)) continue;

        /* …otherwise a percent-encoded project dir: session dirs one deeper. */
        DIR *pd = scan_opendir(child);
        if (!pd) continue;
        struct dirent *se;
        while ((se = scan_readdir(pd)) != NULL) {
            if (se->d_name[0] == '.') continue;
            char sess[CHAT_PATH_CAP];
            int m2 = snprintf(sess, sizeof sess, "%s/%s", child, se->d_name);
            if (m2 <= 0 || (usize)m2 >= sizeof sess) continue;
            struct stat st2;
            if (scan_lstat(sess, &st2) != 0 || !S_ISDIR(st2.st_mode)) continue;
            grok_session_emit(out, sess, de->d_name);
        }
        closedir(pd);
    }
    closedir(d);
}

/* CommandCode (Claude Code-style fork): ~/.commandcode/projects/<slug>/
 * <uuid>.jsonl. No env override exists for the storage root. The slug is the
 * cwd run through hist_slugify_lower() — lowercased, every non-alnum run
 * collapsed to one dash — so Claude's slug decode doesn't apply and m->cwd
 * stays NULL. cwd matching goes the other way instead, via
 * chat_session_matches_cwd().
 *
 * Sidecars per session (cli.mjs `sessionSidecarPaths`): .meta.json,
 * .share.json, .v2.bak — none end in .jsonl — plus .checkpoints.jsonl and
 * .prompts.jsonl, which DO match the suffix and are excluded explicitly so
 * they don't list as phantom sessions. */
static void scan_commandcode(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_home_path(root, sizeof root, ".commandcode/projects")) return;
    DIR *d = scan_opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char proj[CHAT_PATH_CAP];
        int n = snprintf(proj, sizeof proj, "%s/%s", root, de->d_name);
        if (n <= 0 || (usize)n >= sizeof proj) continue;
        DIR *pd = scan_opendir(proj);
        if (!pd) continue;
        struct dirent *se;
        while ((se = scan_readdir(pd)) != NULL) {
            if (se->d_name[0] == '.') continue;
            if (!ends_with(se->d_name, ".jsonl")) continue;
            if (ends_with(se->d_name, ".checkpoints.jsonl")) continue;
            if (ends_with(se->d_name, ".prompts.jsonl")) continue;
            char path[CHAT_PATH_CAP];
            int w = snprintf(path, sizeof path, "%s/%s", proj, se->d_name);
            if (w <= 0 || (usize)w >= sizeof path) continue;
            struct stat st;
            if (scan_lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            ChatSessionMeta m = {0};
            if (fill_meta_from_path(out, &m, CHAT_TOOL_COMMANDCODE, proj,
                                    de->d_name, se->d_name, &st))
                vec_push(out, &m);
        }
        closedir(pd);
    }
    closedir(d);
}

/* Pi (pi-coding-agent) and Oh My Pi (omp):
 * <agent dir>/sessions/--<cwd, / as ->--/<timestamp>_<uuid>.jsonl.
 *
 * Only the files directly in a bucket are sessions. omp keeps a folder named
 * like the session next to it with the sub-agents' transcripts and tool
 * artefacts in it (RootDraftStatus.jsonl, url-search/…), and a hidden
 * ".<name>.jsonl.lock.os" file while one is open: depth 1 and the leading-dot
 * rule leave all of those out, the way the Claude scan leaves its sidechains.
 *
 * Roots: Pi's agent dir is ~/.pi/agent, or $PI_CODING_AGENT_DIR (its
 * documented override); omp's is ~/.omp/agent, and `omp --profile <name>`
 * keeps a store of its own under ~/.omp/profiles/<name>/agent. */
static void scan_pi_root(ChatTool tool, const char *root, EntryVec *out) {
    DIR *d = scan_opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char bucket[CHAT_PATH_CAP];
        int n = snprintf(bucket, sizeof bucket, "%s/%s", root, de->d_name);
        if (n <= 0 || (usize)n >= sizeof bucket) continue;
        struct stat st;
        if (scan_lstat(bucket, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        scan_flat(tool, bucket, de->d_name, out);
    }
    closedir(d);
}

static void scan_pi(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    const char *env = getenv("PI_CODING_AGENT_DIR");
    if (env && env[0] == '/') {
        int n = snprintf(root, sizeof root, "%s/sessions", env);
        if (n > 0 && (usize)n < sizeof root) scan_pi_root(CHAT_TOOL_PI, root, out);
    } else if (hist_home_path(root, sizeof root, ".pi/agent/sessions")) {
        scan_pi_root(CHAT_TOOL_PI, root, out);
    }
}

static void scan_omp(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (hist_home_path(root, sizeof root, ".omp/agent/sessions"))
        scan_pi_root(CHAT_TOOL_OMP, root, out);
    char profiles[CHAT_PATH_CAP];
    if (!hist_home_path(profiles, sizeof profiles, ".omp/profiles")) return;
    DIR *d = scan_opendir(profiles);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        int n = snprintf(root, sizeof root, "%s/%s/agent/sessions", profiles, de->d_name);
        if (n > 0 && (usize)n < sizeof root) scan_pi_root(CHAT_TOOL_OMP, root, out);
    }
    closedir(d);
}

/* Cline CLI (the standalone `cline` binary, NOT the VS Code extension handled
 * by scan_cline_family above — different store, different resume flag).
 *
 * ~/.cline/data/sessions/<ts>_<rand>/
 *     <ts>_<rand>.json           metadata: cwd, started_at, model, status
 *     <ts>_<rand>.messages.json  the transcript
 *
 * The session id is the directory name, which is exactly what `cline --id`
 * takes, so the picker's resume needs no translation. `--data-dir` can move
 * the whole tree; there is no way to discover a relocated one, so only the
 * default location is scanned (the flag is per-invocation, not persisted).
 *
 * The transcript file is the one registered as the session's path — the
 * sibling metadata file is read only for the cwd, which is what lets the
 * picker filter these by project like it does Claude's and Codex's. */
static const char *peek_cline_cli_cwd(Arena *a, const char *meta_path) {
    if (!a || !meta_path) return NULL;
    FILE *f = scan_fopen(meta_path, "r");
    if (!f) return NULL;
    char buf[8192];
    usize n = scan_fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0) return NULL;
    buf[n] = '\0';
    /* "cwd" and "workspace_root" are both top-level and both absolute; cwd is
     * where the agent actually ran. Read it with the same quoted-string peek
     * the Codex path uses rather than paying for a full JSON parse per
     * session — a scan touches every session on disk. */
    const char *key = strstr(buf, "\"cwd\"");
    if (!key) return NULL;
    const char *colon = strchr(key, ':');
    if (!colon) return NULL;
    const char *q1 = strchr(colon, '"');
    if (!q1) return NULL;
    q1++;
    const char *q2 = strchr(q1, '"');
    if (!q2 || q2 == q1) return NULL;
    usize len = (usize)(q2 - q1);
    if (len >= 1024) return NULL;
    char tmp[1024];
    memcpy(tmp, q1, len);
    tmp[len] = '\0';
    return arena_dup_str(a, tmp);
}

static void scan_cline_cli(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_home_path(root, sizeof root, ".cline/data/sessions")) return;
    DIR *d = scan_opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = scan_readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char dir[CHAT_PATH_CAP];
        int n = snprintf(dir, sizeof dir, "%s/%s", root, de->d_name);
        if (n <= 0 || (usize)n >= sizeof dir) continue;

        char fname[512];
        int fn = snprintf(fname, sizeof fname, "%s.messages.json", de->d_name);
        if (fn <= 0 || (usize)fn >= sizeof fname) continue;
        char path[CHAT_PATH_CAP];
        int w = snprintf(path, sizeof path, "%s/%s", dir, fname);
        if (w <= 0 || (usize)w >= sizeof path) continue;
        struct stat st;
        if (scan_lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        /* The sidecar first, because it supplies the PROJECT as well as the
         * cwd. The directory name here is a timestamp and five random
         * characters ("1788812106263_lmcec"); as the project it would title
         * every Cline row with its own session id, which the row already
         * shows, and say nothing of the project. fmt_project_label cannot
         * rescue it either: it prints the last `/` or `-` segment, and that id
         * has neither.
         *
         * Kimi does the same (it resolves its bucket hash to the real work dir
         * and passes THAT as the slug), so the cwd is what goes in. It stays
         * the directory name when the sidecar has no cwd — a session recorded
         * before the field existed, or a truncated file — because a wrong
         * project is worse than an opaque one. */
        char meta_path[CHAT_PATH_CAP];
        const char *cwd = NULL;
        int mw = snprintf(meta_path, sizeof meta_path, "%s/%s.json",
                          dir, de->d_name);
        if (mw > 0 && (usize)mw < sizeof meta_path)
            cwd = peek_cline_cli_cwd(out->arena, meta_path);

        ChatSessionMeta m = {0};
        if (!fill_meta_from_path(out, &m, CHAT_TOOL_CLINE_CLI, dir,
                                 cwd ? cwd : de->d_name, fname, &st))
            continue;
        /* fill_meta_from_path derives the id by stripping one extension, which
         * on "<id>.messages.json" leaves "<id>.messages" — not something
         * `cline --id` accepts. The directory name IS the id. */
        m.session_id = arena_dup_str(out->arena, de->d_name);
        if (!m.session_id) continue;
        /* Set directly rather than through the tool ladder in
         * fill_meta_from_path: this one is read from the session's own
         * metadata, so it is exact and needs no slug round-trip. */
        m.cwd = cwd;
        vec_push(out, &m);
    }
    closedir(d);
}

bool chat_scan_cancellable(Arena *arena, u32 tools_mask, chat_session_cb cb,
                           void *user, const _Atomic bool *cancel) {
    if (!cb || !arena) return false;
    ScanCancelScope saved_scope = g_scan_scope;
    g_scan_scope = (ScanCancelScope){ .cancel = cancel };
    if (tools_mask == 0) tools_mask = 0xFFFFFFFFu;

    EntryVec v = {0};
    v.arena = arena;
    /* The I/O guards and SQLite progress handler also check inside stores. */
#define SCAN_STEP(cond, call) \
    do { if (scan_cancelled()) goto done; \
         if (cond) call; } while (0)
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_CLAUDE),      scan_claude(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_CODEX),       scan_codex(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_COPILOT),     scan_copilot(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_ANTIGRAVITY), scan_antigravity(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_AMP),         scan_amp(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_KIRO),        scan_kiro(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_CURSOR),      scan_cursor(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_DROID),       scan_droid(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_OPENCODE),    scan_opencode(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_KIMI),        scan_kimi(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_QWEN),        scan_qwen(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_XAI),         scan_grok(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_COMMANDCODE), scan_commandcode(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_CLINE_CLI),   scan_cline_cli(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_PI),          scan_pi(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_OMP),         scan_omp(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_HERMES),      scan_hermes(&v));
    SCAN_STEP(tools_mask & (1u << CHAT_TOOL_OPENCLAW),    scan_openclaw(&v));
    SCAN_STEP(tools_mask & ((1u << CHAT_TOOL_CLINE) |
                            (1u << CHAT_TOOL_ROO)   |
                            (1u << CHAT_TOOL_KILO)),      scan_cline_family(&v, tools_mask));
#undef SCAN_STEP

    if (scan_cancelled()) goto done;
    if (!scan_sort(&v)) goto done;

    for (usize i = 0; i < v.count; i++) {
        if (scan_cancelled()) goto done;
        bool more = cb(&v.items[i].meta, user);
        if (scan_cancelled()) goto done;
        if (!more) break;
    }
    free(v.items);
    g_scan_scope = saved_scope;
    return true;

done:
    free(v.items);
    g_scan_scope = saved_scope;
    return false;
}

void chat_scan(Arena *arena, u32 tools_mask, chat_session_cb cb, void *user) {
    chat_scan_cancellable(arena, tools_mask, cb, user, NULL);
}

bool chat_session_matches_cwd(const ChatSessionMeta *m, const char *cwd) {
    if (!m || !cwd || !cwd[0]) return false;

    /* Trailing slashes are noise: "/foo/" and "/foo" are the same directory. */
    usize len = strlen(cwd);
    while (len > 1 && cwd[len - 1] == '/') len--;

    if (m->cwd) {
        return strlen(m->cwd) == len && strncmp(m->cwd, cwd, len) == 0;
    }

    /* No recorded cwd, but CommandCode's project directory IS the slugged
     * cwd — so slug the caller's directory and compare in that direction.
     * Decoding the stored slug instead would be wrong: it cannot tell a dash
     * inside a directory name from a path separator, which is why
     * "volumes-yazilim-yazilim-todolist-clone" survives this comparison but
     * would decode to the nonexistent "/volumes/.../todolist/clone". */
    /* Claude / Qwen with no recorded cwd: slug the caller's directory the way
     * Claude does and compare with the project directory name. Exact in the
     * direction that is computable, and it never claims a path — two
     * directories that slug alike both match, which is all the name knows. */
    if ((m->tool == CHAT_TOOL_CLAUDE || m->tool == CHAT_TOOL_QWEN) && m->project) {
        char trimmed[CHAT_PATH_CAP];
        if (len >= sizeof trimmed) return false;
        memcpy(trimmed, cwd, len);
        trimmed[len] = '\0';
        char slug[CHAT_PATH_CAP];
        if (!hist_slugify_claude(slug, sizeof slug, trimmed)) return false;
        return strcmp(slug, m->project) == 0;
    }

    if (m->tool == CHAT_TOOL_COMMANDCODE && m->project) {
        char trimmed[CHAT_PATH_CAP];
        if (len >= sizeof trimmed) return false;
        memcpy(trimmed, cwd, len);
        trimmed[len] = '\0';

        char slug[CHAT_PATH_CAP];
        if (!hist_slugify_lower(slug, sizeof slug, trimmed)) return false;
        return strcmp(slug, m->project) == 0;
    }

    return false;
}

/* ------------------------------------------------------------------------- */

typedef struct {
    const char       *needle;
    ChatSessionMeta  *out;
    bool              found;
} FindCtx;

static bool find_cb(const ChatSessionMeta *m, void *user) {
    FindCtx *ctx = user;
    if (!ctx->needle) return false;
    /* Match by session_id prefix, or by full path, or by path basename. */
    usize nlen = strlen(ctx->needle);
    if (strcmp(ctx->needle, m->session_id) == 0 ||
        strncmp(ctx->needle, m->session_id, nlen) == 0) {
        *ctx->out = *m; ctx->found = true; return false;
    }
    /* …or as a trailing component of the session id.
     *
     * Codex is the case that needs this: the scanner derives its ids from the
     * rollout FILENAME, so the id here is
     * `rollout-2026-08-28T03-45-48-<uuid>` while everything that addresses a
     * Codex session — `codex resume`, app-server's threadId, the cross-agent
     * lineage journal — uses the bare `<uuid>`. Without this, a lookup by the
     * id the agent itself hands out would fail, and a chained migration
     * (Claude -> Codex -> …) would stop at the second hop.
     *
     * Two guards keep this from matching by accident: at least 8 characters,
     * so a short numeric needle cannot suffix-match half the store, and the
     * character before the match must be a separator, so "827" does not match
     * a session id merely ending in those digits. */
    usize idlen = strlen(m->session_id);
    if (nlen >= 8 && idlen > nlen) {
        const char *tail = m->session_id + (idlen - nlen);
        char before = tail[-1];
        if ((before == '-' || before == '_' || before == '.') &&
            strcmp(tail, ctx->needle) == 0) {
            *ctx->out = *m; ctx->found = true; return false;
        }
    }
    if (strcmp(ctx->needle, m->path) == 0) {
        *ctx->out = *m; ctx->found = true; return false;
    }
    /* Basename compare */
    const char *bn = strrchr(m->path, '/');
    bn = bn ? bn + 1 : m->path;
    if (strcmp(ctx->needle, bn) == 0) {
        *ctx->out = *m; ctx->found = true; return false;
    }
    return true;
}

bool chat_find_session(Arena *arena, const char *needle, ChatSessionMeta *out) {
    if (!arena || !needle || !*needle || !out) return false;

    /* Absolute / relative path → stat directly. */
    if (needle[0] == '/' || needle[0] == '.') {
        struct stat st;
        if (scan_lstat(needle, &st) == 0 && S_ISREG(st.st_mode)) {
            memset(out, 0, sizeof *out);
            out->path = arena_dup_str(arena, needle);
            if (!out->path) return false;
            const char *bn = strrchr(needle, '/');
            bn = bn ? bn + 1 : needle;
            usize bnlen = strlen(bn);
            if (bnlen > 6 && strcmp(bn + bnlen - 6, ".jsonl") == 0) bnlen -= 6;
            out->session_id = arena_session_id(arena, bn, bnlen);
            if (!out->session_id) return false;
            out->project = arena_dup_str(arena, "-");
            out->size_bytes = (i64)st.st_size;
            out->last_modified_ms = (i64)st.st_mtime * 1000;
            /* Guess tool from a path-substring table, with a filename fallback
             * for cline-family (they all share ui_messages.json). */
            static const struct { const char *needle; ChatTool tool; } path_hints[] = {
                { "/antigravity/",                CHAT_TOOL_ANTIGRAVITY },
                { "/.claude/",                    CHAT_TOOL_CLAUDE      },
                { "/.codex/",                     CHAT_TOOL_CODEX       },
                { "/.copilot/",                   CHAT_TOOL_COPILOT     },
                { "/saoudrizwan.claude-dev/",     CHAT_TOOL_CLINE       },
                { "/rooveterinaryinc.roo-cline/", CHAT_TOOL_ROO         },
                { "/roo-code.roo-cline/",         CHAT_TOOL_ROO         },
                { "/kilocode.kilo-code/",         CHAT_TOOL_KILO        },
                { "/.cursor/",                    CHAT_TOOL_CURSOR      },
                { "/amp/threads/",                CHAT_TOOL_AMP         },
                { "/Kiro/",                       CHAT_TOOL_KIRO        },
                { "/.factory/",                   CHAT_TOOL_DROID       },
                { "/opencode/",                   CHAT_TOOL_OPENCODE    },
                { "/.kimi/",                      CHAT_TOOL_KIMI        },
                { "/.qwen/",                      CHAT_TOOL_QWEN        },
                { "/.commandcode/",               CHAT_TOOL_COMMANDCODE },
                { "/.pi/agent/",                  CHAT_TOOL_PI          },
                { "/.omp/",                       CHAT_TOOL_OMP         },
                { "/.hermes/",                    CHAT_TOOL_HERMES      },
                { "/.openclaw/",                  CHAT_TOOL_OPENCLAW    },
            };
            out->tool = CHAT_TOOL_UNKNOWN;
            for (usize i = 0; i < sizeof path_hints / sizeof path_hints[0]; i++) {
                if (strstr(needle, path_hints[i].needle)) {
                    out->tool = path_hints[i].tool;
                    break;
                }
            }
            if (out->tool == CHAT_TOOL_UNKNOWN &&
                strcmp(bn, "ui_messages.json") == 0) {
                out->tool = CHAT_TOOL_CLINE;
            }
            return true;
        }
        return false;
    }

    FindCtx ctx = { .needle = needle, .out = out, .found = false };
    chat_scan(arena, 0, find_cb, &ctx);
    return ctx.found;
}
