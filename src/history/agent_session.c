/*
 * Liu — which stored session a running agent is writing. See agent_session.h.
 */
#include "history/agent_session.h"
#include "history/session.h"
#include "history/util.h"
#include "core/memory.h"

#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

/* A session the agent opened in this run has been written since it started;
 * the slack covers stores that stamp in whole seconds. */
#define AGENT_SESSION_SINCE_SLACK_MS 5000

/* When the process `pid` started, wall clock ms; 0 when it cannot be told. A
 * session this run opened was written at or after it. */
static i64 process_start_ms(i32 pid) {
#ifdef __APPLE__
    struct proc_bsdinfo bi;
    if (pid > 0 && proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) == (int)sizeof bi)
        return (i64)bi.pbi_start_tvsec * 1000 + (i64)bi.pbi_start_tvusec / 1000;
#else
    (void)pid;
#endif
    return 0;
}

static bool cancelled(const _Atomic bool *cancel) {
    return cancel && atomic_load_explicit(cancel, memory_order_acquire);
}

static bool excluded(const AgentSessionQuery *q, const char *id) {
    for (i32 i = 0; i < q->exclude_count && i < AGENT_SESSION_EXCLUDE; i++)
        if (q->exclude[i][0] && strcmp(q->exclude[i], id) == 0) return true;
    return false;
}

static bool copy_ref(AgentSessionRef *out, const char *id, const char *path) {
    if (!id || !id[0] || strlen(id) >= sizeof out->id) return false;
    if (path && strlen(path) >= sizeof out->path) return false;
    memcpy(out->id, id, strlen(id) + 1);
    snprintf(out->path, sizeof out->path, "%s", path ? path : "");
    return true;
}

bool agent_session_tool_supported(ChatTool tool) {
    switch (tool) {
        case CHAT_TOOL_CLAUDE:   case CHAT_TOOL_CODEX:     case CHAT_TOOL_QWEN:
        case CHAT_TOOL_PI:       case CHAT_TOOL_OMP:       case CHAT_TOOL_KIMI:
        case CHAT_TOOL_COMMANDCODE: case CHAT_TOOL_CLINE_CLI: case CHAT_TOOL_OPENCODE:
        case CHAT_TOOL_HERMES:   case CHAT_TOOL_XAI:
        case CHAT_TOOL_DEVIN:    case CHAT_TOOL_QODER:
            return true;
        default:
            return false;
    }
}

/* ---------------------------------------------------------------- Claude */

/* `<config>/sessions/<pid>.json` of an interactive run: its sessionId, when
 * the file names the folder `cwd` (or names none). */
static bool claude_registry_session(const char *path, const char *cwd, char *id, usize cap) {
    char buf[8192];
    FILE *f = hist_fopen_regular(path, true);
    if (!f) return false;
    usize n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0 || n >= sizeof buf - 1) return false;
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    bool ok = false;
    const char *sid  = hist_cjson_str(root, "sessionId");
    const char *kind = hist_cjson_str(root, "kind");
    const char *rcwd = hist_cjson_str(root, "cwd");
    bool interactive = !kind || strcmp(kind, "interactive") == 0;
    if (sid && sid[0] && interactive && (!rcwd || !cwd || !cwd[0] || strcmp(rcwd, cwd) == 0)) {
        hist_copy_bounded(id, cap, sid);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static bool locate_claude(const AgentSessionQuery *q, AgentSessionRef *out,
                          const _Atomic bool *cancel) {
    if (q->fg_pid <= 0) return false;
    char dir[1024];
    if (!hist_resolve_root(dir, sizeof dir, "CLAUDE_CONFIG_DIR", "sessions", ".claude/sessions"))
        return false;
    char path[1200], id[AGENT_SESSION_ID_CAP];
    /* An interactive run leads its job's process group, so its registry is
     * normally named after the pane's foreground pid. */
    snprintf(path, sizeof path, "%s/%d.json", dir, q->fg_pid);
    if (claude_registry_session(path, q->cwd, id, sizeof id))
        return copy_ref(out, id, NULL);
    /* Started through a wrapper (a node or npm shim leads the group): the
     * registry of a process inside the pane's foreground group. */
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent *de;
    while (!found && !cancelled(cancel) && (de = readdir(d)) != NULL) {
        i32 pid = atoi(de->d_name);
        usize nl = strlen(de->d_name);
        if (pid <= 0 || nl < 6 || strcmp(de->d_name + nl - 5, ".json") != 0) continue;
        if (pid == q->fg_pid || (i32)getpgid((pid_t)pid) != q->fg_pid) continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (claude_registry_session(path, q->cwd, id, sizeof id))
            found = copy_ref(out, id, NULL);
    }
    closedir(d);
    return found;
}

/* ----------------------------------------------------------------- Codex */

/* The rollout header: `{"type":"session_meta","payload":{"id","cwd",…}}`.
 * True for a thread of the user's own in `cwd`: not one another thread
 * started — a sub-agent's, a guardian's review — which names its parent
 * (`parent_thread_id`), says so in `source.subagent` or in `thread_source`. */
static bool codex_rollout_in(const char *path, const char *cwd) {
    HistLineReader rd;
    if (!hist_line_reader_open(&rd, path, true)) return false;
    const char *line = NULL;
    usize nline = 0;
    bool ok = false;
    if (hist_line_reader_next(&rd, 4u * 1024u * 1024u, &line, &nline) == HIST_LINE_OK && nline > 0) {
        cJSON *root = cJSON_ParseWithLength(line, nline);
        if (root) {
            const char *type = hist_cjson_str(root, "type");
            cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
            if (type && strcmp(type, "session_meta") == 0 && payload) {
                const char *pcwd = hist_cjson_str(payload, "cwd");
                const char *src  = hist_cjson_str(payload, "thread_source");
                cJSON *source = cJSON_GetObjectItemCaseSensitive(payload, "source");
                bool subagent = (src && strcmp(src, "subagent") == 0) ||
                                hist_cjson_str(payload, "parent_thread_id") != NULL ||
                                (cJSON_IsObject(source) && cJSON_GetObjectItemCaseSensitive(source, "subagent"));
                ok = !subagent && pcwd && strcmp(pcwd, cwd) == 0;
            }
            cJSON_Delete(root);
        }
    }
    hist_line_reader_close(&rd);
    return ok;
}

typedef struct {
    char path[1200];
    char id[AGENT_SESSION_ID_CAP];
    i64  mtime_ms;
} RolloutCand;

static bool locate_codex(const AgentSessionQuery *q, AgentSessionRef *out,
                         const _Atomic bool *cancel) {
    char base[1024];
    if (!hist_resolve_root(base, sizeof base, "CODEX_HOME", "sessions", ".codex/sessions"))
        return false;
    /* Candidates newest first, written since the agent started. A thread
     * running across midnight is in yesterday's folder. */
    enum { CANDS = 16 };
    RolloutCand cand[CANDS];
    i32 nc = 0;
    time_t now = time(NULL);
    for (i32 day = 0; day < 2 && !cancelled(cancel); day++) {
        time_t tt = now - (time_t)day * 86400;
        struct tm tmv;
        if (!localtime_r(&tt, &tmv)) continue;
        char dir[1100];
        snprintf(dir, sizeof dir, "%s/%04d/%02d/%02d", base,
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *de;
        while (!cancelled(cancel) && (de = readdir(d)) != NULL) {
            usize nl = strlen(de->d_name);
            if (nl < 7 || nl - 6 >= AGENT_SESSION_ID_CAP ||
                strcmp(de->d_name + nl - 6, ".jsonl") != 0) continue;
            RolloutCand c;
            if ((usize)snprintf(c.path, sizeof c.path, "%s/%s", dir, de->d_name) >= sizeof c.path)
                continue;
            struct stat st;
            if (lstat(c.path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            c.mtime_ms = (i64)st.st_mtime * 1000;
            if (c.mtime_ms + AGENT_SESSION_SINCE_SLACK_MS < q->since_ms) continue;
            memcpy(c.id, de->d_name, nl - 6);
            c.id[nl - 6] = '\0';
            if (excluded(q, c.id)) continue;
            /* Keep the CANDS newest, sorted. */
            i32 at = nc;
            while (at > 0 && cand[at - 1].mtime_ms < c.mtime_ms) at--;
            if (at >= CANDS) continue;
            i32 last = nc < CANDS ? nc : CANDS - 1;
            for (i32 k = last; k > at; k--) cand[k] = cand[k - 1];
            cand[at] = c;
            if (nc < CANDS) nc++;
        }
        closedir(d);
    }
    for (i32 i = 0; i < nc && !cancelled(cancel); i++)
        if (codex_rollout_in(cand[i].path, q->cwd))
            return copy_ref(out, cand[i].id, cand[i].path);
    return false;
}

/* ------------------------------------------------- every other agent store */

typedef struct {
    const AgentSessionQuery *q;
    AgentSessionRef         *out;
    bool                     found;
} ScanPick;

/* The scan hands sessions newest first: the first that is this folder's,
 * recent enough and not another pane's is the answer. */
static bool scan_pick_cb(const ChatSessionMeta *m, void *user) {
    ScanPick *p = user;
    if (!m || !m->session_id || m->tool != p->q->tool) return true;
    if (m->last_modified_ms + AGENT_SESSION_SINCE_SLACK_MS < p->q->since_ms) return true;
    if (!chat_session_matches_cwd(m, p->q->cwd)) return true;
    if (excluded(p->q, m->session_id)) return true;
    p->found = copy_ref(p->out, m->session_id, m->path);
    return !p->found;
}

static bool locate_by_scan(const AgentSessionQuery *q, AgentSessionRef *out,
                           const _Atomic bool *cancel) {
    Arena a = arena_create(CHAT_SCAN_ARENA_BYTES);
    if (!a.base) return false;
    ScanPick pick = { .q = q, .out = out, .found = false };
    chat_scan_cancellable(&a, CHAT_TOOL_BIT(q->tool), scan_pick_cb, &pick, cancel);
    arena_destroy(&a);
    return pick.found && !cancelled(cancel);
}

bool agent_session_locate(const AgentSessionQuery *q, AgentSessionRef *out,
                          const _Atomic bool *cancel) {
    if (!q || !out) return false;
    memset(out, 0, sizeof *out);
    if (q->cwd[0] != '/' || !agent_session_tool_supported(q->tool)) return false;
    /* The agent's own start, when its process can say: Liu notices an agent a
     * moment after it starts, and its session may have been written in that
     * moment and not since. */
    AgentSessionQuery qq = *q;
    i64 started = process_start_ms(q->fg_pid);
    if (started > 0 && (qq.since_ms <= 0 || started < qq.since_ms)) qq.since_ms = started;
    bool ok = false;
    switch (q->tool) {
        case CHAT_TOOL_CLAUDE: ok = locate_claude(&qq, out, cancel); break;
        case CHAT_TOOL_CODEX:  ok = locate_codex(&qq, out, cancel);  break;
        default:               ok = locate_by_scan(&qq, out, cancel); break;
    }
    if (!ok || cancelled(cancel)) { memset(out, 0, sizeof *out); return false; }
    return true;
}

/* The deadline of agent_session_locate_within: a thread that sleeps until
 * `until` (CLOCK_REALTIME, what pthread_cond_timedwait takes) and then sets
 * `expired`, unless `done` wakes it first. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            done;
    struct timespec until;
    _Atomic bool    expired;
} LocateClock;

static void *locate_clock_main(void *arg) {
    LocateClock *c = (LocateClock *)arg;
    pthread_mutex_lock(&c->mu);
    while (!c->done) {
        int rc = pthread_cond_timedwait(&c->cv, &c->mu, &c->until);
        if (rc == ETIMEDOUT) {
            atomic_store_explicit(&c->expired, true, memory_order_release);
            break;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

bool agent_session_locate_within(const AgentSessionQuery *q, AgentSessionRef *out,
                                 f64 seconds) {
    if (out) memset(out, 0, sizeof *out);
    if (!q || !out || !(seconds > 0.0)) return false;
    LocateClock c;
    memset(&c, 0, sizeof c);
    atomic_init(&c.expired, false);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (seconds > 3600.0) seconds = 3600.0;
    i64 ns = (i64)now.tv_nsec + (i64)((seconds - (f64)(i64)seconds) * 1e9);
    c.until.tv_sec = now.tv_sec + (time_t)seconds + (time_t)(ns / 1000000000);
    c.until.tv_nsec = (long)(ns % 1000000000);
    if (pthread_mutex_init(&c.mu, NULL) != 0) return false;
    if (pthread_cond_init(&c.cv, NULL) != 0) { pthread_mutex_destroy(&c.mu); return false; }
    pthread_t tid;
    bool ok = false;
    /* Without the clock there is no bound, and a lookup with no bound is
     * what this call exists to avoid. */
    if (pthread_create(&tid, NULL, locate_clock_main, &c) == 0) {
        ok = agent_session_locate(q, out, &c.expired);
        pthread_mutex_lock(&c.mu);
        c.done = true;
        pthread_cond_signal(&c.cv);
        pthread_mutex_unlock(&c.mu);
        pthread_join(tid, NULL);
    }
    pthread_cond_destroy(&c.cv);
    pthread_mutex_destroy(&c.mu);
    return ok;
}
