/*
 * liu — live chat-title probe for foreground agent CLIs.
 *
 * See live_title.h for the contract. Everything here is best-effort: any
 * parse/IO failure degrades to "no title" and the tab keeps showing the
 * tool display name. No arenas — the inputs are a handful of tiny JSON
 * files, read into stack buffers at a seconds-scale cadence.
 */
#include "history/live_title.h"
#include "history/util.h"
#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>      /* PATH_MAX — realpath's contract */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    LiveTitleCancelled cancelled;
    void *ctx;
} TitleProbe;

static bool probe_cancelled(const TitleProbe *probe) {
    return probe->cancelled && probe->cancelled(probe->ctx);
}

/* "Which session file is the live one" is answered by recency everywhere
 * below, and recency needs two things: sub-second resolution (two sessions
 * opened back to back share a whole-second st_mtime) and a STABLE tiebreak
 * (with a `<=` compare the winner of an exact tie would be whichever entry
 * readdir() yielded first, which differs between filesystems and between
 * runs). Ties go to the greater file name — Codex and Cline embed their start
 * time in it, so that is also the later session. */
static u64 lt_mtime_ns(const struct stat *st) {
#if defined(__APPLE__)
    return (u64)st->st_mtimespec.tv_sec * 1000000000ull + (u64)st->st_mtimespec.tv_nsec;
#elif defined(st_mtime)   /* POSIX.1-2008 exposes st_mtim and #defines st_mtime */
    return (u64)st->st_mtim.tv_sec * 1000000000ull + (u64)st->st_mtim.tv_nsec;
#else
    return (u64)st->st_mtime * 1000000000ull;
#endif
}

static bool lt_newer(u64 ns, const char *name, u64 best_ns, const char *best_name) {
    if (!best_name || !best_name[0]) return true;
    if (ns != best_ns) return ns > best_ns;
    return strcmp(name, best_name) > 0;
}

/* Titles land in the tab bar — strip control bytes (a registry written by
 * a hostile/broken tool must not smuggle escapes into the renderer) and
 * trim the edges. */
static void sanitize_title(char *s) {
    for (char *p = s; *p; p++) {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f) *p = ' ';
    }
    usize len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') s[--len] = '\0';
    char *start = s;
    while (*start == ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    /* Drop a trailing UTF-8 sequence that got cut in half. Titles are copied
     * into fixed tab buffers (Tab.agent_chat_title is 96 bytes), so any
     * non-Latin name long enough to be truncated would otherwise end in a
     * partial rune and render as a replacement box. */
    len = strlen(s);
    while (len > 0) {
        usize i = len - 1;
        while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
        unsigned char lead = (unsigned char)s[i];
        usize need = (lead < 0x80)          ? 1
                   : ((lead & 0xE0) == 0xC0) ? 2
                   : ((lead & 0xF0) == 0xE0) ? 3
                   : ((lead & 0xF8) == 0xF0) ? 4 : 0;
        if (need != 0 && i + need == len) break;   /* last rune is complete */
        s[i] = '\0';
        len = i;
    }
}

/* ---------------------------------------------------------------- Claude */

typedef struct {
    char name[96];
    char cwd[512];
    i64  updated_ms;
} ClaudeReg;

/* Parse one Claude session registry file and return a non-empty model- or
 * user-provided title. Reject malformed or oversized input. */
static bool claude_reg_read(const char *path, ClaudeReg *out, bool *live) {
    memset(out, 0, sizeof *out);
    *live = false;
    char buf[4096];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    usize n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    const char *name = hist_cjson_str(root, "name");
    const char *cwd  = hist_cjson_str(root, "cwd");
    const char *kind = hist_cjson_str(root, "kind");
    const char *nsrc = hist_cjson_str(root, "nameSource");
    *live = !kind || strcmp(kind, "interactive") == 0;
    if (nsrc && strcmp(nsrc, "derived") == 0) name = NULL;   /* placeholder */
    if (name) hist_copy_bounded(out->name, sizeof out->name, name);
    if (cwd)  hist_copy_bounded(out->cwd,  sizeof out->cwd,  cwd);
    hist_cjson_i64(root, "updatedAt", &out->updated_ms);
    cJSON_Delete(root);
    return out->name[0] != '\0';
}

static bool live_title_claude(i32 fg_pid, const char *cwd,
                              char *out, usize cap, const TitleProbe *probe) {
    char dir[1024];
    if (!hist_resolve_root(dir, sizeof dir,
                           "CLAUDE_CONFIG_DIR", "sessions",
                           ".claude/sessions")) return false;

    char path[1200];
    ClaudeReg reg;
    bool live = false;   /* did the file just parsed hold a live session? */

    /* After a registry file matches this tab's process group, do not use the
     * cwd fallback. A young session may not have a title during its first
     * exchange, so a fallback could assign the title of another session in
     * the same project.
     *
     * Keep this guard in sync with claude_reg_read()'s derived-placeholder
     * filter. When the filter reports no title, this guard prevents the cwd
     * fallback from assigning a neighboring session's title. Track the
     * registry match even when it has no title. */
    bool pid_evidence = false;
    /* A same-cwd live session that has no title yet. It may be this tab's own
     * — the cwd fallback has no way to tell — so it disqualifies the fallback
     * just as pid evidence does, rather than let the OTHER session's title
     * win by default. */
    bool untitled_here = false;

    /* Fast path: registry files are named after the CLI's own pid, and an
     * interactive claude is its job's pgroup leader, so <fg_pid>.json is
     * normally a direct hit — one open of a ~300-byte file. */
    i32 read_pid = -1;   /* registry parsed here; the sweep must not redo it */
    if (fg_pid > 0) {
        snprintf(path, sizeof path, "%s/%d.json", dir, fg_pid);
        if (access(path, F_OK) == 0) {
            pid_evidence = true;
            read_pid = fg_pid;
            if (claude_reg_read(path, &reg, &live) && live) {
                hist_copy_bounded(out, cap, reg.name);
                return true;
            }
        }
    }

    /* Wrapper-launched claude (a node/npm shim leads the pgroup, claude's
     * own pid differs): find a registry pid inside the tab's fg process
     * group. Failing that, fall back to the same-cwd live entry with the
     * newest heartbeat. The dir holds one file per session — tiny. */
    DIR *d = opendir(dir);
    if (!d) return false;
    char best[96];
    best[0] = '\0';
    i64 best_ms = -1;
    struct dirent *de;
    while (!probe_cancelled(probe) && (de = readdir(d)) != NULL) {
        i32 pid = atoi(de->d_name);   /* "4535.json" → 4535 */
        if (pid <= 0) continue;
        /* Skip the pid already opened by the fast path to avoid reading and
         * parsing the same registry file twice. */
        if (pid == read_pid) continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);

        /* Ours by pid: either the registry is named after the leader of this
         * tab's foreground job, or its process sits inside that job's group
         * (wrapper-launched claude, where a node/npm shim leads the pgroup).
         * The direct pid compare is not redundant — getpgid() answers only
         * while the process still exists, and the file can also appear
         * between the fast path's access() and this readdir(). */
        if (fg_pid > 0 &&
            (pid == fg_pid || (i32)getpgid((pid_t)pid) == fg_pid)) {
            /* kind filter matters here: Task-tool subagents share the
             * pgroup and register their own files. */
            if (claude_reg_read(path, &reg, &live) && live) {
                hist_copy_bounded(out, cap, reg.name);
                closedir(d);
                return true;
            }
            if (live) pid_evidence = true;
            continue;
        }

        if (!cwd || !cwd[0]) continue;
        errno = 0;
        if (kill((pid_t)pid, 0) != 0 && errno != EPERM) continue; /* stale */
        /* An untitled registration still indicates a Claude process running
         * in this directory. Its title may be absent until the first
         * exchange completes. */
        bool named = claude_reg_read(path, &reg, &live);
        if (!live) continue;
        if (strcmp(reg.cwd, cwd) != 0) continue;
        if (!named) { untitled_here = true; continue; }
        if (reg.updated_ms > best_ms) {
            best_ms = reg.updated_ms;
            hist_copy_bounded(best, sizeof best, reg.name);
        }
    }
    closedir(d);
    if (best[0] && !pid_evidence && !untitled_here) {
        hist_copy_bounded(out, cap, best);
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------- Codex */

/* First-line session_meta memo. The header of a rollout file never changes,
 * so cache (path → id/cwd/subagent) and skip the re-read on every probe.
 * Only successful parses are cached: a half-flushed first line must retry
 * on the next probe, not be pinned as broken. */
enum { CODEX_HEAD_CACHE = 256 };
/* Upper bound on the codex session_meta first line. */
#define CODEX_META_LINE_MAX ((usize)(4u * 1024u * 1024u))
/* `path` and `cwd` are heap copies, not inline arrays: inline 512-byte arrays
 * would make the table over 256 KB of .bss for a cache that is usually a
 * handful of rollouts deep. The strings are owned by the slot: whoever
 * overwrites a slot frees what was there.
 *
 * The pointer codex_head_get() returns therefore stays valid only until the
 * NEXT codex_head_get() call, which may evict this very slot. Read what you
 * need out of it before probing another path. */
typedef struct {
    char *path;
    char  id[80];
    char *cwd;
    bool  subagent;
} CodexHead;
static CodexHead g_codex_heads[CODEX_HEAD_CACHE];
static u32       g_codex_head_next = 0;
static u32       g_codex_head_used = 0;

static const CodexHead *codex_head_get(const char *path) {
    for (u32 i = 0; i < g_codex_head_used; i++) {
        if (g_codex_heads[i].path && strcmp(g_codex_heads[i].path, path) == 0)
            return &g_codex_heads[i];
    }

    CodexHead h;
    memset(&h, 0, sizeof h);
    bool ok = false;
    /* Maximum bytes read from the first Codex session_meta line. */
    HistLineReader rd;
    if (!hist_line_reader_open(&rd, path, false)) return NULL;
    const char *line = NULL;
    usize nline = 0;
    if (hist_line_reader_next(&rd, CODEX_META_LINE_MAX, &line,
                              &nline) == HIST_LINE_OK && nline > 0) {
        cJSON *root = cJSON_ParseWithLength(line, nline);
        if (root) {
            const char *type = hist_cjson_str(root, "type");
            cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
            if (type && strcmp(type, "session_meta") == 0 && payload) {
                const char *id   = hist_cjson_str(payload, "id");
                const char *pcwd = hist_cjson_str(payload, "cwd");
                const char *src  = hist_cjson_str(payload, "thread_source");
                h.subagent = src && strcmp(src, "subagent") == 0;
                if (id && pcwd) {
                    h.path = strdup(path);
                    h.cwd  = strdup(pcwd);
                    hist_copy_bounded(h.id, sizeof h.id, id);
                    /* All or nothing: a half-allocated entry must not be
                     * cached, or the lookup above would compare against a
                     * NULL path / the caller would read a NULL cwd. */
                    ok = h.path != NULL && h.cwd != NULL;
                }
            }
            cJSON_Delete(root);
        }
    }
    hist_line_reader_close(&rd);
    if (!ok) {
        free(h.path);
        free(h.cwd);
        return NULL;
    }

    u32 slot = g_codex_head_next++ % CODEX_HEAD_CACHE;
    /* Once the ring wraps, the slot still owns the previous entry's strings —
     * overwriting the struct without freeing them leaks a path + cwd per
     * probe. Slots never written are zeroed .bss, and free(NULL) is a no-op. */
    free(g_codex_heads[slot].path);
    free(g_codex_heads[slot].cwd);
    g_codex_heads[slot] = h;
    if (g_codex_head_used < CODEX_HEAD_CACHE) g_codex_head_used++;
    return &g_codex_heads[slot];
}

/* The session_meta line includes base instructions. Bound the read while
 * allowing the header to grow without changing title lookup behavior. */
enum {
    CODEX_IDX_TAIL      = 256 * 1024,
    CODEX_IDX_CHUNK     = 256 * 1024,
    CODEX_IDX_BACK_MAX  = 4 * 1024 * 1024,   /* backward search, per miss */
    CODEX_IDX_MEMO      = 4,
};
typedef struct {
    bool valid;
    char id[80];
    char title[256];            /* "" = the id has no thread_name (yet) */
    u64  dev, ino;
    i64  searched_end;          /* [.., searched_end) searched; a line boundary */
    u64  used;                  /* LRU tick */
} CodexIdxMemo;
static CodexIdxMemo g_codex_idx[CODEX_IDX_MEMO];
static u64          g_codex_idx_tick;
static u64          g_codex_idx_bytes_read;   /* tests: bytes read from the index */

u64 live_title_codex_index_bytes_read(void) { return g_codex_idx_bytes_read; }

/* Search [start, end) for the last line naming `id`. When `start` is not a
 * line boundary the first (partial) line is skipped and *skip receives its
 * length including the '\n', so the caller can hand it to the chunk before.
 * *complete_end receives the offset just past the last '\n' seen. */
static bool codex_idx_scan(FILE *f, i64 start, i64 end, bool start_is_boundary,
                           const char *id, char *title, usize cap,
                           i64 *skip, i64 *complete_end, const TitleProbe *probe) {
    if (skip) *skip = 0;
    if (complete_end) *complete_end = start;
    if (end <= start) return false;
    usize want = (usize)(end - start);
    if (fseeko(f, (off_t)start, SEEK_SET) != 0) return false;
    char *buf = (char *)malloc(want + 1);
    if (!buf) return false;
    usize n = fread(buf, 1, want, f);
    g_codex_idx_bytes_read += n;
    buf[n] = '\0';

    char *line = buf;
    if (!start_is_boundary) {
        char *nl = memchr(buf, '\n', n);
        if (!nl) { free(buf); if (skip) *skip = (i64)n; return false; }
        if (skip) *skip = (i64)(nl + 1 - buf);
        line = nl + 1;
    }
    char *last_nl = NULL;
    for (usize i = n; i > 0; i--) if (buf[i - 1] == '\n') { last_nl = &buf[i - 1]; break; }
    if (complete_end) *complete_end = last_nl ? start + (i64)(last_nl + 1 - buf) : start;

    bool found = false;
    while (line && *line && !probe_cancelled(probe)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        /* Cheap substring gate before the JSON parse; an unterminated last
         * line (a write in progress) fails cJSON_Parse and is skipped. */
        if (strstr(line, id)) {
            cJSON *root = cJSON_Parse(line);
            if (root) {
                const char *lid = hist_cjson_str(root, "id");
                const char *tn  = hist_cjson_str(root, "thread_name");
                if (lid && strcmp(lid, id) == 0 && tn && tn[0]) {
                    hist_copy_bounded(title, cap, tn);
                    found = true;   /* keep scanning: last match wins */
                }
                cJSON_Delete(root);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(buf);
    return found;
}

static bool codex_index_thread_name(const char *id, char *out, usize cap, const TitleProbe *probe) {
    char path[1024];
    if (!hist_resolve_root(path, sizeof path,
                           "CODEX_HOME", "session_index.jsonl",
                           ".codex/session_index.jsonl")) return false;

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return false;
    i64 size = (i64)st.st_size;

    CodexIdxMemo *memo = NULL;
    for (i32 i = 0; i < CODEX_IDX_MEMO; i++)
        if (g_codex_idx[i].valid && strcmp(g_codex_idx[i].id, id) == 0) { memo = &g_codex_idx[i]; break; }
    if (memo && (memo->dev != (u64)st.st_dev || memo->ino != (u64)st.st_ino ||
                 size < memo->searched_end ||
                 size - memo->searched_end > CODEX_IDX_TAIL))
        memo->valid = false, memo = NULL;       /* replaced, truncated, or far behind */

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char title[256];
    title[0] = '\0';

    if (memo) {
        /* Only what was appended since the last look can rename the session. */
        i64 end_done = memo->searched_end;
        if (size > memo->searched_end &&
            codex_idx_scan(f, memo->searched_end, size, true, id, title, sizeof title,
                           NULL, &end_done, probe))
            hist_copy_bounded(memo->title, sizeof memo->title, title);
        fclose(f);
        if (probe_cancelled(probe)) return false;
        memo->searched_end = end_done;
        memo->used = ++g_codex_idx_tick;
        if (!memo->title[0]) return false;
        hist_copy_bounded(out, cap, memo->title);
        return true;
    }

    /* Fresh: the tail first, then backwards within the budget. */
    i64 start = size > CODEX_IDX_TAIL ? size - CODEX_IDX_TAIL : 0;
    i64 skip = 0, end_done = start;
    bool found = codex_idx_scan(f, start, size, start == 0, id, title, sizeof title,
                                &skip, &end_done, probe);
    i64 back_used = 0;
    i64 chunk_end = start + skip;               /* the tail's partial first line */
    while (!found && start > 0 && back_used < CODEX_IDX_BACK_MAX && !probe_cancelled(probe)) {
        i64 cs = chunk_end > CODEX_IDX_CHUNK ? chunk_end - CODEX_IDX_CHUNK : 0;
        i64 cskip = 0;
        found = codex_idx_scan(f, cs, chunk_end, cs == 0, id, title, sizeof title,
                               &cskip, NULL, probe);
        back_used += chunk_end - cs;
        if (cs == 0) break;
        chunk_end = cs + cskip;
        start = cs;
    }
    fclose(f);
    if (probe_cancelled(probe)) return false;

    /* Remember the answer, negative included, so the next probe of this id
     * reads only what gets appended. */
    CodexIdxMemo *slot = &g_codex_idx[0];
    for (i32 i = 0; i < CODEX_IDX_MEMO; i++) {
        if (!g_codex_idx[i].valid) { slot = &g_codex_idx[i]; break; }
        if (g_codex_idx[i].used < slot->used) slot = &g_codex_idx[i];
    }
    memset(slot, 0, sizeof *slot);
    slot->valid = true;
    hist_copy_bounded(slot->id, sizeof slot->id, id);
    hist_copy_bounded(slot->title, sizeof slot->title, found ? title : "");
    slot->dev = (u64)st.st_dev;
    slot->ino = (u64)st.st_ino;
    slot->searched_end = end_done;
    slot->used = ++g_codex_idx_tick;

    if (!found) return false;
    hist_copy_bounded(out, cap, title);
    return true;
}

static bool live_title_codex(const char *cwd, char *out, usize cap, const TitleProbe *probe) {
    if (!cwd || !cwd[0]) return false;
    char base[1024];
    if (!hist_resolve_root(base, sizeof base,
                           "CODEX_HOME", "sessions",
                           ".codex/sessions")) return false;

    /* The live rollout is the one codex keeps appending to, so newest
     * mtime among same-cwd non-subagent files identifies the session.
     * Rollouts are sharded by day — today covers it, yesterday catches a
     * session running across midnight. */
    time_t now = time(NULL);
    char   best_id[80];
    best_id[0] = '\0';
    u64    best_ns = 0;
    char   best_name[256];
    best_name[0] = '\0';

    for (i32 day = 0; day < 2 && !probe_cancelled(probe); day++) {
        time_t    tt = now - (time_t)day * 86400;
        struct tm tmv;
        if (!localtime_r(&tt, &tmv)) continue;
        char dir[1200];
        snprintf(dir, sizeof dir, "%s/%04d/%02d/%02d", base,
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *de;
        while (!probe_cancelled(probe) && (de = readdir(d)) != NULL) {
            usize nl = strlen(de->d_name);
            if (nl < 7 || strcmp(de->d_name + nl - 6, ".jsonl") != 0) continue;
            char path[1600];
            snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            u64 ns = lt_mtime_ns(&st);
            if (!lt_newer(ns, de->d_name, best_ns, best_name)) continue;
            const CodexHead *h = codex_head_get(path);
            if (!h || h->subagent) continue;
            if (strcmp(h->cwd, cwd) != 0) continue;
            best_ns = ns;
            hist_copy_bounded(best_name, sizeof best_name, de->d_name);
            hist_copy_bounded(best_id, sizeof best_id, h->id);
        }
        closedir(d);
    }

    if (!best_id[0] || probe_cancelled(probe)) return false;
    return codex_index_thread_name(best_id, out, cap, probe);
}

/* ------------------------------------------------------------------ Kimi */

/* Kimi never writes a title to disk. kimi_cli's Session.refresh() derives one
 * at runtime from the FIRST TurnBegin record in the session's wire.jsonl
 * (textwrap.shorten of the user's opening message), and `last_session_id` in
 * ~/.kimi/kimi.json is only written at CLI shutdown — useless while the
 * session is live. So the title has to be recomputed the same way.
 *
 * On-disk layout (kimi_cli metadata.py, WorkDirMeta.sessions_dir):
 *     <share>/sessions/<md5(work_dir)>/<session-uuid>/{context,wire}.jsonl
 * <share> is $KIMI_SHARE_DIR or ~/.kimi; work_dir is the canonical absolute
 * path Kimi was started in — the tab's cwd. wire.jsonl records look like
 *     {"timestamp":…,"message":{"type":"TurnBegin","payload":{"user_input":…}}}
 * with user_input either a plain string or an array of content parts. */

enum {
    KIMI_TITLE_CHARS   = 50,          /* matches kimi_cli's shorten(width=50) */
    KIMI_SCAN_LINES    = 48,          /* TurnBegin sits within the first few */
    KIMI_MAX_LINE      = 1024 * 1024, /* skip absurd lines rather than parse */
};

/* Append `src` to out[cap] at *pos, collapsing every whitespace run to one
 * space (what Message.extract_text + textwrap.shorten do before truncating). */
static void kimi_append_collapsed(char *out, usize cap, usize *pos,
                                  const char *src) {
    if (!src) return;
    for (const char *s = src; *s && *pos + 1 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch <= 0x20) {
            if (*pos > 0 && out[*pos - 1] != ' ') out[(*pos)++] = ' ';
            continue;
        }
        out[(*pos)++] = *s;
    }
    out[*pos] = '\0';
}

/* Flatten a TurnBegin `user_input` (string, or array of content parts) into
 * `out`. Only text parts contribute — an image/audio part carries no words. */
static void kimi_flatten_user_input(cJSON *ui, char *out, usize cap) {
    usize pos = 0;
    out[0] = '\0';
    if (!ui) return;
    if (cJSON_IsString(ui)) {
        kimi_append_collapsed(out, cap, &pos, ui->valuestring);
        return;
    }
    if (!cJSON_IsArray(ui)) return;
    cJSON *part = NULL;
    cJSON_ArrayForEach(part, ui) {
        const char *type = hist_cjson_str(part, "type");
        if (type && strcmp(type, "text") != 0) continue;
        const char *txt = hist_cjson_str(part, "text");
        if (!txt || !txt[0]) continue;
        if (pos > 0 && pos + 1 < cap) { out[pos++] = ' '; out[pos] = '\0'; }
        kimi_append_collapsed(out, cap, &pos, txt);
    }
}

/* Truncate to at most KIMI_TITLE_CHARS *characters* on a word boundary,
 * appending an ellipsis when anything was cut. Counting characters (not
 * bytes) keeps a Turkish or CJK prompt from being cut to a third of the
 * intended width, and never splits a UTF-8 sequence. kimi_cli's own
 * " [...]" placeholder is dropped — the tab bar has its own truncation and
 * the marker would just eat title space. */
static void kimi_shorten(char *s, usize cap) {
    usize bytes = 0, chars = 0, last_space_bytes = 0;
    for (const char *p = s; *p; p++) {
        if (((unsigned char)*p & 0xC0) == 0x80) { bytes++; continue; }  /* cont */
        if (chars == KIMI_TITLE_CHARS) break;
        if (*p == ' ') last_space_bytes = bytes;
        chars++;
        bytes++;
    }
    if (!s[bytes]) return;                    /* fit whole — nothing to cut */
    usize cut = last_space_bytes ? last_space_bytes : bytes;
    s[cut] = '\0';
    /* `cut` is normally far inside the buffer (≤50 runes), but a wire.jsonl
     * carrying invalid UTF-8 can walk `bytes` much further, so bound the
     * ellipsis rather than assume there is room. */
    if (cut + 4 <= cap) memcpy(s + cut, "…", 4);
}

/* Read the first TurnBegin out of one wire.jsonl. Returns false when the file
 * has no turn yet (a session created but never prompted — kimi itself shows
 * "Untitled" then, and the tab should keep the tool name). */
static bool kimi_wire_title(const char *path, char *out, usize cap, const TitleProbe *probe) {
    /* KIMI_MAX_LINE is enforced by the reader, so an absurd line is never
     * allocated in the first place. */
    HistLineReader rd;
    if (!hist_line_reader_open(&rd, path, false)) return false;

    const char *line = NULL;
    bool found = false;
    for (i32 i = 0; i < KIMI_SCAN_LINES && !found && !probe_cancelled(probe); i++) {
        usize n = 0;
        HistLineResult r = hist_line_reader_next(&rd, KIMI_MAX_LINE, &line, &n);
        if (r == HIST_LINE_EOF || r == HIST_LINE_IO_ERROR) break;
        if (r == HIST_LINE_TOO_LONG) continue;
        if (n == 0) continue;
        /* Cheap gate before the JSON parse — most lines are step/tool events. */
        if (!strstr(line, "\"TurnBegin\"")) continue;

        cJSON *root = cJSON_Parse(line);
        if (!root) continue;
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *type = hist_cjson_str(msg, "type");
        if (type && strcmp(type, "TurnBegin") == 0) {
            cJSON *payload = cJSON_GetObjectItemCaseSensitive(msg, "payload");
            cJSON *ui = payload
                ? cJSON_GetObjectItemCaseSensitive(payload, "user_input") : NULL;
            char flat[512];
            kimi_flatten_user_input(ui, flat, sizeof flat);
            if (flat[0]) {
                kimi_shorten(flat, sizeof flat);
                hist_copy_bounded(out, cap, flat);
                found = true;
            }
        }
        cJSON_Delete(root);
    }
    hist_line_reader_close(&rd);
    return found;
}

static bool live_title_kimi(const char *cwd, char *out, usize cap, const TitleProbe *probe) {
    if (!cwd || !cwd[0]) return false;
    char root[1024];
    if (!hist_resolve_root(root, sizeof root, "KIMI_SHARE_DIR", "sessions",
                           ".kimi/sessions")) return false;

    /* Kimi canonicalizes the work dir before hashing it, so resolve symlinks
     * here too — a tab whose cwd came in through /tmp (a symlink to
     * /private/tmp on macOS) would otherwise hash to a bucket that does not
     * exist. */
    /* PATH_MAX, not a smaller round number: realpath() is specified to write
     * up to PATH_MAX bytes into the caller's buffer and glibc ENFORCES that
     * under _FORTIFY_SOURCE — __realpath_chk aborts the process outright when
     * __builtin_object_size says the destination is smaller. At -O0 the check
     * is compiled out, so a 1024-byte buffer here ran fine in every Debug build
     * and killed the app on the first deep cwd in a Release one. */
    char canon[PATH_MAX];
    if (!realpath(cwd, canon)) hist_copy_bounded(canon, sizeof canon, cwd);

    char digest[33];
    hist_md5_hex(canon, strlen(canon), digest);

    char bucket[1200];
    snprintf(bucket, sizeof bucket, "%s/%s", root, digest);

    /* The live session is the one kimi keeps appending to: newest wire.jsonl
     * mtime in this work dir's bucket. A second kimi in the same directory
     * makes the newer of the two win, which is the best a cwd-keyed lookup can
     * do — same limitation the Codex probe has.
     *
     * Sub-second resolution matters: two sessions opened back to back in one
     * directory share a whole-second st_mtime, and a plain `<=` compare would
     * then hand the tab whichever readdir() happened to yield first. */
    DIR *d = opendir(bucket);
    if (!d) return false;
    char best[1400];
    best[0] = '\0';
    u64 best_ns = 0;
    char best_name[256];
    best_name[0] = '\0';
    struct dirent *de;
    while (!probe_cancelled(probe) && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char wire[1400];
        int w = snprintf(wire, sizeof wire, "%s/%s/wire.jsonl", bucket, de->d_name);
        if (w <= 0 || (usize)w >= sizeof wire) continue;
        struct stat st;
        if (stat(wire, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
            continue;
        u64 ns = lt_mtime_ns(&st);
        if (!lt_newer(ns, de->d_name, best_ns, best_name)) continue;
        best_ns = ns;
        hist_copy_bounded(best_name, sizeof best_name, de->d_name);
        hist_copy_bounded(best, sizeof best, wire);
    }
    closedir(d);

    if (!best[0] || probe_cancelled(probe)) return false;
    return kimi_wire_title(best, out, cap, probe);
}

/* --------------------------------------------------------- Command Code */

/* Command Code names the conversation itself and persists it in the session's
 * `<uuid>.meta.json` sidecar, so the probe is a directory listing plus one
 * small JSON read — no transcript scan.
 *
 * The bucket is ~/.commandcode/projects/<slugify(cwd)>, and within it the live
 * session is the newest <uuid>.jsonl. Same cwd-keyed limitation as the Codex
 * and Kimi probes: two Command Codes in one directory make the more recently
 * written one win. */
static bool live_title_commandcode(const char *cwd, char *out, usize cap, const TitleProbe *probe) {
    if (!cwd || !cwd[0]) return false;

    char slug[512];
    if (!hist_slugify_lower(slug, sizeof slug, cwd)) return false;

    char rel[600];
    int n = snprintf(rel, sizeof rel, ".commandcode/projects/%s", slug);
    if (n <= 0 || (usize)n >= sizeof rel) return false;
    char bucket[1024];
    if (!hist_home_path(bucket, sizeof bucket, rel)) return false;

    DIR *d = opendir(bucket);
    if (!d) return false;
    char best[1400];       /* sized like the paths built below, not smaller */
    char meta_path[1400];
    best[0] = '\0';
    u64 best_ns = 0;
    char best_name[256];
    best_name[0] = '\0';
    struct dirent *de;
    while (!probe_cancelled(probe) && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        usize nlen = strlen(de->d_name);
        /* Session transcripts only — the .checkpoints/.prompts sidecars share
         * the .jsonl suffix and have no meta of their own. */
        if (nlen <= 6 || strcmp(de->d_name + nlen - 6, ".jsonl") != 0) continue;
        if (nlen > 18 && strcmp(de->d_name + nlen - 18, ".checkpoints.jsonl") == 0) continue;
        if (nlen > 14 && strcmp(de->d_name + nlen - 14, ".prompts.jsonl") == 0) continue;

        char path[1300];
        int w = snprintf(path, sizeof path, "%s/%s", bucket, de->d_name);
        if (w <= 0 || (usize)w >= sizeof path) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        u64 ns = lt_mtime_ns(&st);
        if (!lt_newer(ns, de->d_name, best_ns, best_name)) continue;
        /* Store the meta sidecar path, not the transcript: name minus ".jsonl"
         * plus ".meta.json".
         *
         * best_ns is advanced only AFTER the path is built and kept; advancing
         * it first would let a path that failed to build raise the bar, and
         * every older-but-valid candidate would then be rejected. Sidecars are
         * also required to EXIST: `commandcode -p` writes a transcript without
         * one (its title mode is off in print runs), and Liu's own
         * theme/translate backends invoke it in the user's project — so the
         * newest transcript in a directory is regularly one that can never
         * yield a title, and stopping there would drop the interactive
         * session's title. */
        int mw = snprintf(meta_path, sizeof meta_path, "%s/%.*s.meta.json",
                          bucket, (int)(nlen - 6), de->d_name);
        if (mw <= 0 || (usize)mw >= sizeof meta_path) continue;
        if (access(meta_path, R_OK) != 0) continue;   /* no sidecar → not a candidate */
        best_ns = ns;
        hist_copy_bounded(best_name, sizeof best_name, de->d_name);
        hist_copy_bounded(best, sizeof best, meta_path);
    }
    closedir(d);
    if (!best[0] || probe_cancelled(probe)) return false;

    /* A session that has not been titled yet has no "title" key at all (the
     * sidecar is written on creation with just traceIds), which reads as
     * "no title" and leaves the tab showing the tool name. */
    FILE *f = fopen(best, "rb");
    if (!f) return false;
    char buf[8192];
    usize len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (len == 0) return false;
    buf[len] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) return false;
    const char *title = hist_cjson_str(root, "title");
    bool ok = false;
    if (title && title[0]) {
        /* Titles are model-written and occasionally run to several lines
         * ("Initial greeting\n\nSession description:\n<description>…").
         * Keep the first line only — sanitize_title would otherwise flatten
         * the newlines into one very long tab label. */
        const char *nl = strchr(title, '\n');
        if (nl) {
            usize first = (usize)(nl - title);
            if (first >= cap) first = cap - 1;
            memcpy(out, title, first);
            out[first] = '\0';
        } else {
            hist_copy_bounded(out, cap, title);
        }
        ok = out[0] != '\0';
    }
    cJSON_Delete(root);
    return ok;
}

/* ----------------------------------------------------------------- Cline */

/* Cline CLI writes one directory per session under ~/.cline/data/sessions, holding
 * <id>.json (metadata) next to <id>.messages.json (the transcript).
 *
 * There is no model-written title in that metadata — nothing names the session. What
 * IS there is `prompt`: the user's opening message, wrapped in the mode tag Cline
 * feeds the model ("<user_input mode=\"act\">fix it</user_input>"). Unwrapping that
 * gives the label, and it is what a user would call the conversation anyway.
 *
 * Attribution is by cwd plus recency, and NOT by the metadata's `pid`: that field
 * records Cline's background hub daemon, not the CLI in the pane. Sessions in
 * different working directories carry the same pid, so matching on it would hand a
 * pane a title from an unrelated project. A pane's foreground pgid never equals the
 * daemon's pid either, so a pid check could only ever fire wrongly.
 *
 * Finished sessions are skipped: `ended_at` or a terminal `status` means the
 * conversation is over, and its prompt must not become the title of a shell that
 * merely sits in the same directory. */
typedef struct {
    char  name[256];      /* session directory name (= session id) */
    u64   ns;
} ClineCand;

/* Newest-first shortlist of sessions written inside the live window. Reading
 * every session directory on every probe would be one open + parse per
 * session every 2.5 s, and keeping only the newest few sessions of ALL
 * projects before filtering by directory would let newer sessions elsewhere
 * push a live session in the asked-for directory off the list, so its title
 * would never be found. The list therefore holds every session written
 * inside CLINE_STALE_AFTER_SEC (older ones cannot win anyway), up to
 * CLINE_CAND_MAX, and what bounds the work is a READ budget: metadata files
 * are opened newest-first until one answers or the budget is spent, and a
 * file for another directory is rejected by a substring check before it is
 * parsed. */
#define CLINE_CAND_MAX      64
#define CLINE_READ_MAX      24
#define CLINE_READ_BYTES    (16u * 1024u * 1024u)

/* How long a session file may go unwritten and still be treated as live. */
#define CLINE_STALE_AFTER_SEC (12.0 * 60.0 * 60.0)

static u64 g_cline_meta_reads;          /* tests: metadata files opened */
static u64 g_cline_meta_parses;         /* tests: metadata files parsed */
u64 live_title_cline_meta_reads(void)  { return g_cline_meta_reads; }
u64 live_title_cline_meta_parses(void) { return g_cline_meta_parses; }

static void cline_cand_insert(ClineCand *c, i32 *n, const char *name, u64 ns) {
    i32 pos = *n;
    while (pos > 0 && (c[pos - 1].ns < ns ||
                       (c[pos - 1].ns == ns && strcmp(c[pos - 1].name, name) < 0))) pos--;
    if (pos >= CLINE_CAND_MAX) return;
    i32 last = (*n < CLINE_CAND_MAX) ? *n : CLINE_CAND_MAX - 1;
    for (i32 i = last; i > pos; i--) c[i] = c[i - 1];
    hist_copy_bounded(c[pos].name, sizeof c[pos].name, name);
    c[pos].ns = ns;
    if (*n < CLINE_CAND_MAX) (*n)++;
}

/* What one session's metadata says about the asked-for directory. The caller
 * needs all four: another directory or a finished session means "look at an
 * older one", but a LIVE session here without a prompt yet means "this is the
 * session, and it has no title" — falling through to an older live-looking
 * session in the same directory (one killed without writing its end) would
 * hand the new conversation the old one's prompt. */
typedef enum {
    CLINE_META_OTHER,       /* unreadable, another directory */
    CLINE_META_FINISHED,    /* this directory, but over */
    CLINE_META_UNTITLED,    /* this directory, live, no prompt yet */
    CLINE_META_TITLED,      /* this directory, live, prompt → title */
} ClineMeta;

static ClineMeta cline_meta_read(const char *path, const char *cwd,
                                 char *out, usize cap, usize *bytes) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return CLINE_META_OTHER;
    /* Cline embeds the whole system prompt in this file, so it runs to several
     * KB and the prompt text appears twice; a fixed stack buffer would
     * truncate it and cJSON would then reject the half-JSON, losing the title
     * silently. Read the real size, with a ceiling so a corrupt length can't
     * be trusted. */
    if (st.st_size > 4 * 1024 * 1024) return CLINE_META_OTHER;
    usize sz = (usize)st.st_size;
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return CLINE_META_OTHER;

    FILE *f = fopen(path, "rb");
    if (!f) { free(buf); return CLINE_META_OTHER; }
    g_cline_meta_reads++;
    usize got = fread(buf, 1, sz, f);
    fclose(f);
    if (bytes) *bytes += got;
    if (got == 0) { free(buf); return CLINE_META_OTHER; }
    buf[got] = '\0';

    /* A session of another directory is the common case and needs no parse:
     * the directory string must appear verbatim. JSON would escape only '"'
     * and '\\' in a path (Cline writes '/' unescaped), so the gate is skipped
     * for a cwd holding either — then the exact check below decides alone. */
    if (!strpbrk(cwd, "\"\\") && !strstr(buf, cwd)) { free(buf); return CLINE_META_OTHER; }

    g_cline_meta_parses++;
    cJSON *root = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (!root) return CLINE_META_OTHER;

    const char *scwd = hist_cjson_str(root, "cwd");
    if (!scwd || strcmp(scwd, cwd) != 0) {
        cJSON_Delete(root);
        return CLINE_META_OTHER;
    }

    /* Finished? `ended_at` is written on exit; `status` reaches a terminal
     * value for both clean and failed runs. Either one disqualifies it. */
    const char *ended = hist_cjson_str(root, "ended_at");
    const char *status = hist_cjson_str(root, "status");
    if ((ended && ended[0]) ||
        (status && (!strcmp(status, "completed") || !strcmp(status, "failed") ||
                    !strcmp(status, "cancelled") || !strcmp(status, "aborted")))) {
        cJSON_Delete(root);
        return CLINE_META_FINISHED;
    }

    ClineMeta r = CLINE_META_UNTITLED;
    const char *prompt = hist_cjson_str(root, "prompt");
    if (prompt && prompt[0]) {
        /* Unwrap "<user_input …>text</user_input>". The closing tag is matched
         * by NAME: a prompt that mentions markup ("fix the </div> here") would
         * otherwise be cut at the first "</" it contains. */
        const char *sv = prompt;
        if (*sv == '<') {
            const char *gt = strchr(sv, '>');
            if (gt) sv = gt + 1;
        }
        const char *endp = strstr(sv, "</user_input>");
        usize n = endp ? (usize)(endp - sv) : strlen(sv);

        /* One line only: a pasted multi-line prompt would otherwise be
         * flattened into a very long tab label by sanitize_title. */
        const char *nl = memchr(sv, '\n', n);
        if (nl) n = (usize)(nl - sv);
        if (n >= cap) n = cap - 1;
        memcpy(out, sv, n);
        out[n] = '\0';
        if (out[0]) r = CLINE_META_TITLED;
    }
    cJSON_Delete(root);
    return r;
}

static bool live_title_cline(i32 fg_pid, const char *cwd, char *out, usize cap, const TitleProbe *probe) {
    (void)fg_pid;   /* see the note above: the metadata's pid is the daemon's */
    if (!cwd || !cwd[0]) return false;

    char root[1024];
    if (!hist_home_path(root, sizeof root, ".cline/data/sessions")) return false;

    DIR *d = opendir(root);
    if (!d) return false;

    /* A session that merely never got a terminal status — killed with SIGKILL,
     * crashed, or left over from a machine that went to sleep — stays "idle"
     * on disk forever. Without an age bound, opening a shell in that directory
     * months later inherits its prompt as the tab title before the user has
     * typed anything. A stale title is worse than none: it names a
     * conversation that is not happening. Cline rewrites the metadata on each
     * turn, so an actually-live session stays well inside this window, and a
     * session outside it is not even a candidate. */
    u64 now_ns = (u64)time(NULL) * 1000000000ull;
    u64 stale_ns = (u64)(CLINE_STALE_AFTER_SEC * 1e9);

    ClineCand *cand = (ClineCand *)malloc(sizeof(ClineCand) * CLINE_CAND_MAX);
    if (!cand) { closedir(d); return false; }
    i32 cand_n = 0;
    struct dirent *de;
    while (!probe_cancelled(probe) && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char meta[1400];
        int w = snprintf(meta, sizeof meta, "%s/%s/%s.json",
                         root, de->d_name, de->d_name);
        if (w <= 0 || (usize)w >= sizeof meta) continue;
        struct stat st;
        if (stat(meta, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
            continue;
        u64 ns = lt_mtime_ns(&st);
        if (now_ns > ns && now_ns - ns > stale_ns) continue;
        cline_cand_insert(cand, &cand_n, de->d_name, ns);
    }
    closedir(d);

    /* Newest first: the live session in this directory is the one most
     * recently written to. */
    bool ok = false;
    usize bytes = 0;
    i32 reads = 0;
    for (i32 i = 0; i < cand_n && !probe_cancelled(probe); i++) {
        if (reads >= CLINE_READ_MAX || bytes >= CLINE_READ_BYTES) break;
        char meta[1400];
        int w = snprintf(meta, sizeof meta, "%s/%s/%s.json", root, cand[i].name, cand[i].name);
        if (w <= 0 || (usize)w >= sizeof meta) continue;
        reads++;
        ClineMeta m = cline_meta_read(meta, cwd, out, cap, &bytes);
        if (m == CLINE_META_TITLED) { ok = true; break; }
        if (m == CLINE_META_UNTITLED) break;     /* the live session has no title yet */
        /* OTHER / FINISHED: an older session may still be the live one here. */
    }
    free(cand);
    return ok;
}

/* ---------------------------------------------------- Pi and Oh My Pi -- */

/* The newest session in the folder's bucket, ~/.pi/agent/sessions/--<cwd>--/ or
 * ~/.omp/agent/sessions/--<cwd>--/ (the cwd without its leading "/", "/" "\\"
 * and ":" written as "-": Pi's own rule, kept by its fork). Liu's helper runs
 * pass --no-session, so the newest file is the interactive one.
 *
 *   omp   writes {"type":"title","title":…} as the FIRST line, padded to a
 *         fixed width and rewritten in place when the title changes, so the
 *         head of the file is always the current one;
 *   pi    keeps a name only when the user sets one (`--name`, `/name`): a
 *         {"type":"session_info","name":…} entry appended to the file, the
 *         last of which wins — looked for in the file's tail. */
static bool live_title_pi_family(ChatTool tool, const char *cwd, char *out, usize cap,
                                 const TitleProbe *probe) {
    if (!cwd || cwd[0] != '/') return false;
    char slug[900];
    usize o = 0;
    for (const char *p = cwd + 1; *p && o + 1 < sizeof slug; p++)
        slug[o++] = (*p == '/' || *p == '\\' || *p == ':') ? '-' : *p;
    slug[o] = '\0';

    char rel[1000];
    int n = snprintf(rel, sizeof rel, "%s/sessions/--%s--",
                     tool == CHAT_TOOL_PI ? ".pi/agent" : ".omp/agent", slug);
    if (n <= 0 || (usize)n >= sizeof rel) return false;
    char bucket[1024];
    if (!hist_home_path(bucket, sizeof bucket, rel)) return false;
    DIR *d = opendir(bucket);
    if (!d) return false;

    char best[1400], best_name[256];
    best[0] = best_name[0] = '\0';
    u64 best_ns = 0;
    struct dirent *de;
    while (!probe_cancelled(probe) && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        usize nlen = strlen(de->d_name);
        if (nlen <= 6 || strcmp(de->d_name + nlen - 6, ".jsonl") != 0) continue;
        char path[1400];
        int w = snprintf(path, sizeof path, "%s/%s", bucket, de->d_name);
        if (w <= 0 || (usize)w >= sizeof path) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        u64 ns = lt_mtime_ns(&st);
        if (!lt_newer(ns, de->d_name, best_ns, best_name)) continue;
        best_ns = ns;
        hist_copy_bounded(best_name, sizeof best_name, de->d_name);
        hist_copy_bounded(best, sizeof best, path);
    }
    closedir(d);
    if (!best[0] || probe_cancelled(probe)) return false;

    FILE *f = fopen(best, "rb");
    if (!f) return false;
    bool ok = false;
    if (tool == CHAT_TOOL_OMP) {
        char head[4096];
        usize len = fread(head, 1, sizeof head - 1, f);
        head[len] = '\0';
        char *nl = memchr(head, '\n', len);
        if (nl) {
            cJSON *root = cJSON_ParseWithLength(head, (usize)(nl - head));
            if (root) {
                const char *type = hist_cjson_str(root, "type");
                const char *title = hist_cjson_str(root, "title");
                if (type && !strcmp(type, "title") && title && title[0]) {
                    hist_copy_bounded(out, cap, title);
                    ok = out[0] != '\0';
                }
                cJSON_Delete(root);
            }
        }
    } else {
        enum { TAIL = 65536 };
        char *tail = (char *)malloc(TAIL + 1);
        if (tail) {
            long size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
            long from = size > TAIL ? size - TAIL : 0;
            if (size > 0 && fseek(f, from, SEEK_SET) == 0) {
                usize len = fread(tail, 1, TAIL, f);
                tail[len] = '\0';
                /* Newest first; a first line cut by the window is not parsed. */
                char *end = tail + len;
                while (end > tail && !ok && !probe_cancelled(probe)) {
                    char *start = end;
                    while (start > tail && start[-1] != '\n') start--;
                    if ((start > tail || from == 0) && end - start > 20 &&
                        memmem(start, (usize)(end - start), "session_info", 12)) {
                        cJSON *root = cJSON_ParseWithLength(start, (usize)(end - start));
                        if (root) {
                            const char *type = hist_cjson_str(root, "type");
                            const char *name = hist_cjson_str(root, "name");
                            if (type && !strcmp(type, "session_info") && name && name[0]) {
                                hist_copy_bounded(out, cap, name);
                                ok = out[0] != '\0';
                            }
                            cJSON_Delete(root);
                        }
                    }
                    end = start > tail ? start - 1 : tail;
                }
            }
            free(tail);
        }
    }
    fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ API */

bool chat_live_title_cancellable(ChatTool tool, i32 fg_pid, const char *cwd,
                                 char *out, usize cap,
                                 LiveTitleCancelled cancelled, void *ctx) {
    const TitleProbe state = {cancelled, ctx};
    const TitleProbe *probe = &state;
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (probe_cancelled(probe)) return false;

    bool ok = false;
    switch (tool) {
    case CHAT_TOOL_CLAUDE: ok = live_title_claude(fg_pid, cwd, out, cap, probe); break;
    case CHAT_TOOL_CODEX:  ok = live_title_codex(cwd, out, cap, probe);          break;
    case CHAT_TOOL_KIMI:   ok = live_title_kimi(cwd, out, cap, probe);           break;
    case CHAT_TOOL_COMMANDCODE:
        ok = live_title_commandcode(cwd, out, cap, probe);
        break;
    case CHAT_TOOL_CLINE:  ok = live_title_cline(fg_pid, cwd, out, cap, probe); break;
    case CHAT_TOOL_PI:
    case CHAT_TOOL_OMP:    ok = live_title_pi_family(tool, cwd, out, cap, probe); break;
    default: break;   /* no known on-disk chat title for this tool */
    }
    if (!ok || probe_cancelled(probe)) {
        out[0] = '\0';
        return false;
    }
    sanitize_title(out);
    return out[0] != '\0';
}

bool chat_live_title(ChatTool tool, i32 fg_pid, const char *cwd,
                     char *out, usize cap) {
    return chat_live_title_cancellable(tool, fg_pid, cwd, out, cap, NULL, NULL);
}
