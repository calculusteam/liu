/*
 * liu-history - discover session files from all supported agents.
 *
 * Walks these roots (honoring env overrides):
 *   claude:      $CLAUDE_CONFIG_DIR/projects       | ~/.claude/projects
 *   codex:       $CODEX_HOME/sessions              | ~/.codex/sessions
 *   copilot:     $COPILOT_HOME/session-state       | ~/.copilot/session-state
 *   antigravity: ~/.gemini/antigravity/code_tracker/<project>/
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
#include "history/session.h"
#include "history/util.h"
#include "core/memory.h"
#include "sqlite3.h"
#include "cJSON.h"

#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

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
    if (v->count + 1 > v->cap) {
        usize ncap = v->cap ? v->cap * 2 : 64;
        Entry *p = realloc(v->items, ncap * sizeof *p);
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
    usize n = strlen(s) + 1;
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
    usize tot = dn + 1 + fn + 1;
    char *p = arena_alloc(a, tot);
    if (!p) return NULL;
    memcpy(p, dir, dn);
    p[dn] = '/';
    memcpy(p + dn + 1, fname, fn);
    p[dn + 1 + fn] = '\0';
    return p;
}

static int cmp_mtime_desc(const void *a, const void *b) {
    const Entry *ea = a, *eb = b;
    if (eb->meta.last_modified_ms > ea->meta.last_modified_ms) return 1;
    if (eb->meta.last_modified_ms < ea->meta.last_modified_ms) return -1;
    return 0;
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
 * session id. If `len` exceeds SESSION_ID_HARD_CAP we truncate and append a
 * stable 8-hex hash so long rollout IDs cannot silently collide. */
#define SESSION_ID_HARD_CAP 96
static const char *arena_session_id(Arena *a, const char *fname, usize base_len) {
    if (!fname) return NULL;
    if (base_len + 1 <= SESSION_ID_HARD_CAP) {
        char *p = arena_alloc(a, base_len + 1);
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

/* Decode a Claude project slug back to its absolute path. The slug is the
 * absolute path with each '/' replaced by '-' and a leading '-' (because the
 * leading '/' itself becomes '-'). Returns NULL on alloc failure or when
 * `slug` doesn't look like an encoded path (no leading '-' / contains "-/" or
 * is "-"/empty). */
static const char *decode_claude_slug(Arena *a, const char *slug) {
    if (!slug || slug[0] != '-') return NULL;
    usize n = strlen(slug);
    if (n < 2) return NULL;
    char *p = arena_alloc(a, n + 1);
    if (!p) return NULL;
    for (usize i = 0; i < n; i++) p[i] = (slug[i] == '-') ? '/' : slug[i];
    p[n] = '\0';
    return p;
}

/* Codex stores a JSON object on the very first line of every rollout file
 * containing a "cwd" string field. Read up to 8 KB (covers any reasonable
 * header size; Codex emits ~400 B), find `"cwd":"…"`, and arena-dup the
 * value. Returns NULL on any I/O or parse failure — caller treats the cwd
 * as unknown and falls back to project-slug matching. */
static const char *peek_codex_cwd(Arena *a, const char *path) {
    if (!a || !path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[8192];
    usize n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return NULL;
    buf[n] = '\0';

    /* Stop at end of first line so we don't accidentally pick up `"cwd"`
     * appearing inside later message content. */
    char *nl = memchr(buf, '\n', n);
    if (nl) *nl = '\0';

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

/* Release process-lifetime scanner caches. Idempotent; call once at shutdown.
 * No tool currently keeps a process-lifetime cache, so this is a no-op kept for
 * the stable shutdown-hook API its callers expect. */
void chat_scan_shutdown(void) {
}

static bool fill_meta_from_path(EntryVec *vec, ChatSessionMeta *m, ChatTool tool,
                                const char *dir, const char *project_slug,
                                const char *fname, const struct stat *st_in) {
    Arena *a = vec ? vec->arena : NULL;
    if (!a) return false;
    m->tool = tool;

    m->path = arena_join_path(a, dir, fname);
    if (!m->path) return false;

    m->project = arena_dup_str(a, project_slug ? project_slug : "-");
    if (!m->project) return false;

    /* session_id = fname with its extension stripped (.jsonl or .json).
     *
     * For tools that store one session per directory under a fixed file
     * name — Cline-family ("ui_messages.json"), Copilot CLI events.jsonl),
     * Kimi (context.jsonl) — the bare filename would collide across every
     * session, so we promote the parent directory name to the session id
     * instead. The check is filename-based so Kimi's *flat* layout (where
     * the filename IS the unique id, e.g. abc123.jsonl) keeps using the
     * filename. */
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
    m->session_id = arena_session_id(a, src, src_len);
    if (!m->session_id) return false;

    struct stat st;
    const struct stat *st_src = st_in;
    if (!st_src) {
        if (lstat(m->path, &st) != 0) st_src = NULL;
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
     *   - Claude: zero I/O, just decodes the slug we already have.
     *   - Codex:  one fopen + fread of ≤8 KB per session file. Run during
     *             a user-initiated scan, so the latency is acceptable.
     *   - Other tools: skipped (parsers would have to walk message bodies). */
    m->cwd = NULL;
    if (tool == CHAT_TOOL_CLAUDE) {
        m->cwd = decode_claude_slug(a, project_slug);
    } else if (tool == CHAT_TOOL_CODEX) {
        m->cwd = peek_codex_cwd(a, m->path);
    } else if (tool == CHAT_TOOL_QWEN) {
        /* Qwen stores chats under `~/.qwen/projects/<sanitized-cwd>/chats`
         * where the sanitization is the same `/` → `-` slugging Claude uses,
         * so the same decoder works without any extra I/O. */
        m->cwd = decode_claude_slug(a, project_slug);
    }
    return true;
}

/* Walk a single directory and push every *.jsonl file as a meta entry. */
static void scan_flat(ChatTool tool, const char *dir, const char *project_slug,
                      EntryVec *out) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!ends_with(de->d_name, ".jsonl")) continue;
        char path[CHAT_PATH_CAP];
        int wrote = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (wrote <= 0 || (usize)wrote >= sizeof path) continue;
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
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
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[CHAT_PATH_CAP];
        int wrote = snprintf(child, sizeof child, "%s/%s", root, de->d_name);
        if (wrote <= 0 || (usize)wrote >= sizeof child) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
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
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char proj[CHAT_PATH_CAP];
        int wrote = snprintf(proj, sizeof proj, "%s/%s", root, de->d_name);
        if (wrote <= 0 || (usize)wrote >= sizeof proj) continue;
        struct stat st;
        if (lstat(proj, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
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

static void scan_antigravity(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_resolve_root(root, sizeof root,
                           "GEMINI_CLI_HOME", "antigravity/code_tracker",
                           ".gemini/antigravity/code_tracker")) return;
    /* One directory per project, each containing *.jsonl session files. */
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char proj[CHAT_PATH_CAP];
        int wrote = snprintf(proj, sizeof proj, "%s/%s", root, de->d_name);
        if (wrote <= 0 || (usize)wrote >= sizeof proj) continue;
        struct stat st;
        if (lstat(proj, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        /* Re-use scan_flat for .jsonl matching but re-tag the tool afterwards. */
        usize before = out->count;
        scan_flat(CHAT_TOOL_ANTIGRAVITY, proj, de->d_name, out);
        for (usize i = before; i < out->count; i++) {
            out->items[i].meta.tool = CHAT_TOOL_ANTIGRAVITY;
        }
    }
    closedir(d);
}

/* VS Code globalStorage product list, ordered to match the
 * CHAT_VSCODE_HOST_* bit indices in session.h:
 *   bit 0 = Code, 1 = Code - Insiders, 2 = Cursor, 3 = VSCodium
 *
 * Building one base path per requested host avoids the old "scan all four
 * unconditionally" cost when the UI knows which IDE the user lives in. */
#define VSCODE_HOST_COUNT 4
static const char *kVSCodeProducts[VSCODE_HOST_COUNT] = {
    "Code", "Code - Insiders", "Cursor", "VSCodium",
};

/* Build globalStorage root for host bit `h` into `out`. Returns true on
 * success. Empty path / unsupported platform → false. */
static bool vscode_storage_root_for(i32 h, char *out, usize cap) {
    if (h < 0 || h >= VSCODE_HOST_COUNT) return false;
    /* The roaming app-data base covers all three OSes (and honors
     * $XDG_CONFIG_HOME on Linux, which the old ~/.config hardcode missed):
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

/* Is the host IDE actually installed on this machine? macOS checks the
 * .app bundle in /Applications and ~/Applications first, then falls back
 * to the IDE's Application Support data dir (covers Setapp / custom path
 * / external-volume installs that don't drop a bundle in /Applications).
 * On other platforms we defer to the extensions-directory probe (returns
 * true so the ext check is the sole gate). Used to keep stale globalStorage
 * from a long-removed IDE out of Agent History. */
static bool vscode_app_installed(i32 h) {
    if (h < 0 || h >= VSCODE_HOST_COUNT) return false;
#if defined(__APPLE__)
    char p[CHAT_PATH_CAP];
    struct stat st;
    int w = snprintf(p, sizeof p, "/Applications/%s.app", kVSCodeAppBundle[h]);
    if (w > 0 && (usize)w < sizeof p &&
        stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    char home[CHAT_PATH_CAP];
    if (hist_home_path(home, sizeof home, "Applications")) {
        w = snprintf(p, sizeof p, "%s/%s.app", home, kVSCodeAppBundle[h]);
        if (w > 0 && (usize)w < sizeof p &&
            stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    }
    /* Fallback: IDE has been launched at least once on this box (Setapp,
     * non-standard install path, removable-drive bundle, …). */
    if (hist_home_path(p, sizeof p, "Library/Application Support")) {
        usize n = strlen(p);
        if (n + 2 + strlen(kVSCodeProducts[h]) < sizeof p) {
            snprintf(p + n, sizeof p - n, "/%s", kVSCodeProducts[h]);
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return true;
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
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    usize n = strlen(ext_id);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
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
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if ((!s1 || !ends_with(de->d_name, s1)) &&
            (!s2 || !ends_with(de->d_name, s2))) continue;
        char path[CHAT_PATH_CAP];
        int wrote = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (wrote <= 0 || (usize)wrote >= sizeof path) continue;
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
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

static void scan_kimi(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_resolve_root(root, sizeof root, "KIMI_SHARE_DIR", "sessions",
                           ".kimi/sessions")) return;
    /* Two layouts in the wild:
     *   - flat:      ~/.kimi/sessions/<id>.{json,jsonl}
     *   - per-dir:   ~/.kimi/sessions/<id>/{context,wire}.jsonl + state.json
     * Cover both by doing a flat sweep first, then walking one level deep
     * and emitting the canonical "context.jsonl" file from each session dir
     * (the prompt/response stream) with the parent dir as the session id —
     * "wire.jsonl" is the raw API transcript, redundant for our use. */
    scan_flat_exts(CHAT_TOOL_KIMI, root, NULL, ".json", ".jsonl", out);

    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char sess[CHAT_PATH_CAP];
        int w = snprintf(sess, sizeof sess, "%s/%s", root, de->d_name);
        if (w <= 0 || (usize)w >= sizeof sess) continue;
        struct stat st;
        if (lstat(sess, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char ctx[CHAT_PATH_CAP];
        w = snprintf(ctx, sizeof ctx, "%s/context.jsonl", sess);
        if (w <= 0 || (usize)w >= sizeof ctx) continue;
        struct stat cst;
        if (lstat(ctx, &cst) != 0 || !S_ISREG(cst.st_mode)) continue;
        ChatSessionMeta m = {0};
        if (fill_meta_from_path(out, &m, CHAT_TOOL_KIMI, sess, NULL,
                                "context.jsonl", &cst))
            vec_push(out, &m);
    }
    closedir(d);
}

/* Kiro stores session-per-workspace. Two layouts seen in the wild:
 *   - newer: ~/.kiro/sessions/<workspace>/<session>.json
 *   - older: ~/Library/Application Support/Kiro/workspace-sessions/<ws>/<id>.json (macOS)
 *            ~/.config/Kiro/workspace-sessions/<ws>/<id>.json (Linux)
 * Walk both — the second one was the only path the original scanner knew,
 * and a current install only populates the dotfile root. */
static void scan_kiro_root(const char *root, EntryVec *out) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char ws[CHAT_PATH_CAP];
        int w = snprintf(ws, sizeof ws, "%s/%s", root, de->d_name);
        if (w <= 0 || (usize)w >= sizeof ws) continue;
        struct stat st;
        if (lstat(ws, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
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
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char proj[CHAT_PATH_CAP];
        int w = snprintf(proj, sizeof proj, "%s/%s/agent-transcripts",
                         root, de->d_name);
        if (w <= 0 || (usize)w >= sizeof proj) continue;
        struct stat st;
        if (lstat(proj, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        /* Layout is one level deeper than the rest of the cursor tree:
         *   agent-transcripts/<sess-id>/<sess-id>.jsonl
         * The naive flat sweep only saw the <sess-id> directories and
         * skipped them, which is why "Cursor histories aren't detected"
         * — every Cursor session sits in its own subfolder. */
        DIR *td = opendir(proj);
        if (!td) continue;
        struct dirent *tde;
        while ((tde = readdir(td)) != NULL) {
            if (tde->d_name[0] == '.') continue;
            char sess_dir[CHAT_PATH_CAP];
            int w2 = snprintf(sess_dir, sizeof sess_dir, "%s/%s",
                              proj, tde->d_name);
            if (w2 <= 0 || (usize)w2 >= sizeof sess_dir) continue;
            struct stat sst;
            if (lstat(sess_dir, &sst) != 0 || !S_ISDIR(sst.st_mode)) continue;
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
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char chats[CHAT_PATH_CAP];
        int w = snprintf(chats, sizeof chats, "%s/%s/chats", root, de->d_name);
        if (w <= 0 || (usize)w >= sizeof chats) continue;
        struct stat st;
        if (lstat(chats, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        scan_flat_exts(CHAT_TOOL_QWEN, chats, de->d_name, ".jsonl", ".json", out);
    }
    closedir(d);
}

/* OpenCode keeps every session in a SQLite DB at $XDG_DATA_HOME/opencode/
 * opencode.db (or ~/.local/share/opencode/opencode.db). The `session` table
 * holds the per-session cwd in its `directory` column, the title in
 * `title`, and a millisecond timestamp in `time_updated`. We query the table
 * directly so the cwd-aware history filter works for OpenCode, and so that
 * users who don't dump session JSONs to disk still see their sessions in
 * the picker. The DB is opened read-only with the `?immutable=1` URI flag
 * so we don't compete with a running OpenCode for the write lock. */
static void scan_opencode(EntryVec *out) {
    if (!out || !out->arena) return;

    /* Resolve the SQLite file under whichever storage root the user has.
     * opencode honors $XDG_DATA_HOME on every OS (Windows included) and
     * otherwise writes ~/.local/share even there — NOT %LOCALAPPDATA%. */
    char db_path[CHAT_PATH_CAP];
    if (!hist_xdg_data_path(db_path, sizeof db_path, "opencode/opencode.db"))
        return;

    struct stat st;
    if (stat(db_path, &st) != 0 || !S_ISREG(st.st_mode)) return;

    /* The vendored sqlite3 build sets SQLITE_OMIT_AUTOINIT, so callers must
     * initialize the library explicitly. The function is idempotent and
     * cheap on repeat calls. */
    static bool sqlite_inited = false;
    if (!sqlite_inited) {
        sqlite3_initialize();
        sqlite_inited = true;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path,
                        &db,
                        SQLITE_OPEN_READONLY,
                        NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }

    const char *sql =
        "SELECT id, directory, title, time_updated "
        "FROM session ORDER BY time_updated DESC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *id        = (const char *)sqlite3_column_text(stmt, 0);
        const char *directory = (const char *)sqlite3_column_text(stmt, 1);
        const char *title     = (const char *)sqlite3_column_text(stmt, 2);
        sqlite3_int64 t_ms    = sqlite3_column_int64(stmt, 3);
        if (!id || !id[0]) continue;

        ChatSessionMeta m = {0};
        m.tool = CHAT_TOOL_OPENCODE;

        /* path: we don't have a per-session file; point at the DB so callers
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
        m.size_bytes       = 0;
        m.event_count      = 0;

        if (m.path && m.session_id && m.project) vec_push(out, &m);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
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
            DIR *d = opendir(tasks_root);
            if (!d) continue;
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                char task_dir[CHAT_PATH_CAP];
                int w2 = snprintf(task_dir, sizeof task_dir, "%s/%s",
                                  tasks_root, de->d_name);
                if (w2 <= 0 || (usize)w2 >= sizeof task_dir) continue;
                struct stat st;
                if (lstat(task_dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                char msg_path[CHAT_PATH_CAP];
                int w3 = snprintf(msg_path, sizeof msg_path,
                                  "%s/ui_messages.json", task_dir);
                if (w3 <= 0 || (usize)w3 >= sizeof msg_path) continue;
                struct stat mst;
                if (lstat(msg_path, &mst) != 0 || !S_ISREG(mst.st_mode)) continue;
                ChatSessionMeta m = {0};
                if (fill_meta_from_path(out, &m, extensions[e].tool, task_dir,
                                        de->d_name, "ui_messages.json", &mst))
                    vec_push(out, &m);
            }
            closedir(d);
        }
    }
}

/* chat_scan's "every host, every Cline-family tool" entry point.
 * host_mask = 0xF → all four VSCODE_HOST_COUNT bits, i.e. Code +
 * Insiders + Cursor + VSCodium. The scanner silently skips any host
 * whose globalStorage directory doesn't exist, so this is also the
 * de-facto IDE auto-detector for installed-and-used VS Code forks. */
static void scan_cline_family(EntryVec *out) {
    scan_cline_family_filtered(out,
        (1u << CHAT_TOOL_CLINE) | (1u << CHAT_TOOL_ROO) | (1u << CHAT_TOOL_KILO),
        0xFu);
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
    if (lstat(chat, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    /* summary.json: drop sub-agent worker sessions, lift the absolute cwd. */
    char *cwd_own = NULL;
    {
        char sum[CHAT_PATH_CAP];
        int m2 = snprintf(sum, sizeof sum, "%s/summary.json", sess_dir);
        char *buf = NULL; usize blen = 0;
        if (m2 > 0 && (usize)m2 < sizeof sum &&
            hist_slurp_file(out->arena, sum, 64 * 1024, &buf, &blen)) {
            cJSON *root = cJSON_ParseWithLength(buf, blen);
            if (root) {
                const char *kind = hist_cjson_str(root, "session_kind");
                if (kind && strcmp(kind, "subagent") == 0) {
                    cJSON_Delete(root);
                    return false;
                }
                cJSON *info = cJSON_GetObjectItemCaseSensitive(root, "info");
                const char *cw = info ? hist_cjson_str(info, "cwd") : NULL;
                if (cw && cw[0] == '/')
                    cwd_own = hist_strdup(out->arena, cw);
                cJSON_Delete(root);
            }
        }
    }

    ChatSessionMeta m = {0};
    if (!fill_meta_from_path(out, &m, CHAT_TOOL_XAI, sess_dir, project_slug,
                             "chat_history.jsonl", &st))
        return false;
    if (cwd_own) m.cwd = cwd_own;
    vec_push(out, &m);
    return true;
}

static void scan_grok(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_resolve_root(root, sizeof root, "GROK_HOME", "sessions",
                           ".grok/sessions"))
        return;
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child[CHAT_PATH_CAP];
        int n = snprintf(child, sizeof child, "%s/%s", root, de->d_name);
        if (n <= 0 || (usize)n >= sizeof child) continue;
        struct stat st;
        if (lstat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        /* A session dir directly under the root (global session)… */
        if (grok_session_emit(out, child, NULL)) continue;

        /* …otherwise a percent-encoded project dir: session dirs one deeper. */
        DIR *pd = opendir(child);
        if (!pd) continue;
        struct dirent *se;
        while ((se = readdir(pd)) != NULL) {
            if (se->d_name[0] == '.') continue;
            char sess[CHAT_PATH_CAP];
            int m2 = snprintf(sess, sizeof sess, "%s/%s", child, se->d_name);
            if (m2 <= 0 || (usize)m2 >= sizeof sess) continue;
            struct stat st2;
            if (lstat(sess, &st2) != 0 || !S_ISDIR(st2.st_mode)) continue;
            grok_session_emit(out, sess, de->d_name);
        }
        closedir(pd);
    }
    closedir(d);
}

/* CommandCode (Claude Code-style fork): ~/.commandcode/projects/<slug>/
 * <uuid>.jsonl. No env override exists for the storage root. The slug is the
 * LOWERCASED cwd with separators dashed and no leading dash, so Claude's
 * slug decode doesn't apply — cwd attribution stays NULL (lossy case).
 * Sibling files per session: <uuid>.meta.json (not .jsonl, skipped by the
 * suffix match) and <uuid>.checkpoints.jsonl, which WOULD match — excluded
 * explicitly so checkpoints don't list as phantom sessions. */
static void scan_commandcode(EntryVec *out) {
    char root[CHAT_PATH_CAP];
    if (!hist_home_path(root, sizeof root, ".commandcode/projects")) return;
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char proj[CHAT_PATH_CAP];
        int n = snprintf(proj, sizeof proj, "%s/%s", root, de->d_name);
        if (n <= 0 || (usize)n >= sizeof proj) continue;
        DIR *pd = opendir(proj);
        if (!pd) continue;
        struct dirent *se;
        while ((se = readdir(pd)) != NULL) {
            if (se->d_name[0] == '.') continue;
            if (!ends_with(se->d_name, ".jsonl")) continue;
            if (ends_with(se->d_name, ".checkpoints.jsonl")) continue;
            char path[CHAT_PATH_CAP];
            int w = snprintf(path, sizeof path, "%s/%s", proj, se->d_name);
            if (w <= 0 || (usize)w >= sizeof path) continue;
            struct stat st;
            if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            ChatSessionMeta m = {0};
            if (fill_meta_from_path(out, &m, CHAT_TOOL_COMMANDCODE, proj,
                                    de->d_name, se->d_name, &st))
                vec_push(out, &m);
        }
        closedir(pd);
    }
    closedir(d);
}

void chat_scan(Arena *arena, u32 tools_mask, chat_session_cb cb, void *user) {
    if (!cb || !arena) return;
    if (tools_mask == 0) tools_mask = 0xFFFFFFFFu;

    EntryVec v = {0};
    v.arena = arena;
    if (tools_mask & (1u << CHAT_TOOL_CLAUDE))      scan_claude(&v);
    if (tools_mask & (1u << CHAT_TOOL_CODEX))       scan_codex(&v);
    if (tools_mask & (1u << CHAT_TOOL_COPILOT))     scan_copilot(&v);
    if (tools_mask & (1u << CHAT_TOOL_ANTIGRAVITY)) scan_antigravity(&v);
    if (tools_mask & (1u << CHAT_TOOL_AMP))         scan_amp(&v);
    if (tools_mask & (1u << CHAT_TOOL_KIRO))        scan_kiro(&v);
    if (tools_mask & (1u << CHAT_TOOL_CURSOR))      scan_cursor(&v);
    if (tools_mask & (1u << CHAT_TOOL_DROID))       scan_droid(&v);
    if (tools_mask & (1u << CHAT_TOOL_OPENCODE))    scan_opencode(&v);
    if (tools_mask & (1u << CHAT_TOOL_KIMI))        scan_kimi(&v);
    if (tools_mask & (1u << CHAT_TOOL_QWEN))        scan_qwen(&v);
    if (tools_mask & (1u << CHAT_TOOL_XAI))         scan_grok(&v);
    if (tools_mask & (1u << CHAT_TOOL_COMMANDCODE)) scan_commandcode(&v);
    if (tools_mask & ((1u << CHAT_TOOL_CLINE) |
                       (1u << CHAT_TOOL_ROO)   |
                       (1u << CHAT_TOOL_KILO))) {
        scan_cline_family(&v);
    }

    if (v.count > 1) qsort(v.items, v.count, sizeof *v.items, cmp_mtime_desc);

    for (usize i = 0; i < v.count; i++) {
        if (!cb(&v.items[i].meta, user)) break;
    }
    free(v.items);
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
    if (strcmp(ctx->needle, m->session_id) == 0 ||
        strncmp(ctx->needle, m->session_id, strlen(ctx->needle)) == 0) {
        *ctx->out = *m; ctx->found = true; return false;
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
        if (lstat(needle, &st) == 0 && S_ISREG(st.st_mode)) {
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
