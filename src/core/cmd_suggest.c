/*
 * Liu — learned command suggestion engine (see cmd_suggest.h).
 *
 * Single-slot model mirroring cmd_history's cache pattern: one directory is
 * hot at a time (the focused shell's cwd). Keyed on (dir, CmdHistoryKey) of
 * the backing cmdhistory log; observe() adopts the post-append key so the
 * common path never rebuilds. Rebuild = arena/table RESET (no allocator
 * traffic) + replay of the CmdHistory snapshot oldest→newest.
 */
#include "core/cmd_suggest.h"
#include "core/cmd_history.h"
#include "core/checked.h"
#include "core/project_dir.h"   /* liu_project_data_dir — sidecar location */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- tunables --------------------------------------------------------- */

#define SUG_MAX_CMDS        512      /* unique commands per directory       */
#define SUG_MAX_TRANS       4096     /* (prev,next) transition pairs        */
#define SUG_ARENA_CAP       (256 * 1024)
#define SUG_DECAY           0.995f   /* λ per recorded command              */
#define SUG_MARGIN          1.25f    /* model must beat newest-first by 25% */
#define SUG_GATE1_RATIO     1.5f     /* 1-char prefix: top1/top2 confidence */
#define SUG_GATE1_MINFREQ   1.5f     /* 1-char prefix: min decayed count.
                                        Two real uses decay to ~1.98 (dedup
                                        forbids age 0), so 2.0 would be
                                        unreachable; 1.5 = "two recent uses
                                        pass, one never does, stale pairs
                                        decay back under". */
#define SUG_SIDECAR_FLUSH   32       /* dirty feedback updates per flush    */

/* ---- model ------------------------------------------------------------ */

typedef struct {
    u32  off;          /* offset into the interning arena                  */
    f32  count;        /* decayed execution count (lazy decay)             */
    u32  last_idx;     /* event index of last execution                    */
    u16  accepts, shows, rejects;
} SugCmd;

typedef struct {
    u16 prev, next;    /* command ids; SUG_NONE = empty slot               */
    f32 count;
    u32 last_idx;
} SugTrans;

#define SUG_NONE 0xFFFFu

typedef struct {
    char    dir[1024];
    CmdHistoryKey key;               /* freshness key of the backing log   */
    bool    valid;

    char   *arena;                   /* interned command strings           */
    u32     arena_used;
    bool    arena_full;              /* stop interning; fallback covers it */

    SugCmd  cmds[SUG_MAX_CMDS];
    i32     cmd_count;
    u16     sorted[SUG_MAX_CMDS];    /* ids ordered by strcmp of strings   */

    SugTrans trans[SUG_MAX_TRANS];   /* open-addressing hash               */

    u32     event_idx;
    u16     prev_cmd;                /* SUG_NONE until first observe       */

    i32     dirty;                   /* feedback updates since last flush  */
    bool    replaying;               /* suppress sidecar writes in rebuild */
} SugModel;

static _Thread_local SugModel g_sug = { .prev_cmd = SUG_NONE };

static const char *sug_str(const SugModel *m, u16 id) {
    return m->arena + m->cmds[id].off;
}

/* Lazy exponential decay: counts age by "events since last touch". */
static f32 sug_decayed(const SugModel *m, const SugCmd *c) {
    u32 age = m->event_idx - c->last_idx;
    if (age > 4096) return 0.0f;
    return c->count * powf(SUG_DECAY, (f32)age);
}

/* ---- transition hash --------------------------------------------------- */

static u32 sug_trans_slot(u16 prev, u16 next) {
    u32 h = ((u32)prev * 31u + (u32)next) * 2654435761u;
    return h % SUG_MAX_TRANS;
}

static SugTrans *sug_trans_find(SugModel *m, u16 prev, u16 next, bool create) {
    u32 idx = sug_trans_slot(prev, next);
    for (u32 i = 0; i < 64; i++) {
        SugTrans *t = &m->trans[(idx + i) % SUG_MAX_TRANS];
        if (t->prev == prev && t->next == next) return t;
        if (t->prev == SUG_NONE) {
            if (!create) return NULL;
            t->prev = prev; t->next = next; t->count = 0; t->last_idx = 0;
            return t;
        }
    }
    return NULL;   /* probe window full — drop the update, not the frame */
}

/* Eviction must purge every transition row holding the dead id, or its
 * reissued id silently inherits the old command's transition mass. */
static void sug_trans_purge(SugModel *m, u16 id) {
    for (u32 i = 0; i < SUG_MAX_TRANS; i++) {
        if (m->trans[i].prev == id || m->trans[i].next == id) {
            m->trans[i].prev = SUG_NONE;
            m->trans[i].next = SUG_NONE;
            m->trans[i].count = 0;
        }
    }
}

/* ---- sorted prefix index ----------------------------------------------- */

static void sug_sorted_insert(SugModel *m, u16 id) {
    const char *str = sug_str(m, id);
    i32 lo = 0, hi = m->cmd_count - 1;   /* id already appended; slot open */
    while (lo < hi) {
        i32 mid = (lo + hi) / 2;
        if (strcmp(sug_str(m, m->sorted[mid]), str) < 0) lo = mid + 1;
        else hi = mid;
    }
    memmove(&m->sorted[lo + 1], &m->sorted[lo],
            (usize)(m->cmd_count - 1 - lo) * sizeof(u16));
    m->sorted[lo] = id;
}

static void sug_sorted_remove(SugModel *m, u16 id) {
    for (i32 i = 0; i < m->cmd_count; i++) {
        if (m->sorted[i] == id) {
            memmove(&m->sorted[i], &m->sorted[i + 1],
                    (usize)(m->cmd_count - 1 - i) * sizeof(u16));
            return;
        }
    }
}

/* ---- intern / evict ---------------------------------------------------- */

static u16 sug_find(const SugModel *m, const char *cmd) {
    /* Binary search over the sorted index. */
    i32 lo = 0, hi = m->cmd_count - 1;
    while (lo <= hi) {
        i32 mid = (lo + hi) / 2;
        int c = strcmp(sug_str(m, m->sorted[mid]), cmd);
        if (c == 0) return m->sorted[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return SUG_NONE;
}

static u16 sug_intern(SugModel *m, const char *cmd) {
    u16 id = sug_find(m, cmd);
    if (id != SUG_NONE) return id;

    for (const unsigned char *p = (const unsigned char *)cmd; *p; p++)
        if (*p < 32 || *p == 127) return SUG_NONE;
    usize len = strlen(cmd);
    usize needed;
    if (!checked_add_usize(len, 1u, &needed) ||
        m->arena_full || (usize)m->arena_used > SUG_ARENA_CAP ||
        needed > SUG_ARENA_CAP - (usize)m->arena_used) {
        /* Arena exhausted: stop interning new commands; the newest-first
         * fallback in cmd_suggest_best still serves them. A future rebuild
         * compacts naturally (replay re-interns only live log entries). */
        m->arena_full = true;
        return SUG_NONE;
    }

    if (m->cmd_count >= SUG_MAX_CMDS) {
        /* Evict the lowest-decayed command; purge its transitions so the
         * reused id starts clean. */
        i32 worst = 0;
        f32 worst_score = 1e30f;
        for (i32 i = 0; i < m->cmd_count; i++) {
            f32 sc = sug_decayed(m, &m->cmds[i]);
            if (sc < worst_score) { worst_score = sc; worst = i; }
        }
        u16 wid = (u16)worst;
        sug_sorted_remove(m, wid);
        sug_trans_purge(m, wid);
        if (m->prev_cmd == wid) m->prev_cmd = SUG_NONE;
        /* Reuse the slot in place (arena bytes leak until next rebuild —
         * accepted: rebuild compacts). */
        memcpy(m->arena + m->arena_used, cmd, len + 1);
        m->cmds[wid] = (SugCmd){ .off = m->arena_used, .count = 0,
                                 .last_idx = m->event_idx };
        m->arena_used += (u32)(len + 1);
        /* Re-insert into sorted order: temporarily treat as appended. */
        u16 saved = m->sorted[m->cmd_count - 1];
        (void)saved;
        m->cmd_count--;            /* sorted[] currently has count-1 live  */
        m->cmd_count++;            /* restore; insert expects open slot    */
        sug_sorted_insert(m, wid);
        return wid;
    }

    memcpy(m->arena + m->arena_used, cmd, len + 1);
    u16 nid = (u16)m->cmd_count;
    m->cmds[nid] = (SugCmd){ .off = m->arena_used, .count = 0,
                             .last_idx = m->event_idx };
    m->arena_used += (u32)(len + 1);
    m->cmd_count++;
    sug_sorted_insert(m, nid);
    return nid;
}

/* ---- sidecar (feedback counters: the only non-log-derivable state) ----- */

static bool sug_sidecar_path(const char *dir, char *out, usize cap) {
    char data_dir[1024];
    if (!liu_project_data_dir(dir, data_dir, sizeof data_dir)) return false;
    int n = snprintf(out, cap, "%s/cmdsuggest", data_dir);
    return n > 0 && (usize)n < cap;
}

static bool sug_parse_counter(const char **cursor, u16 *out) {
    if (!cursor || !*cursor || !out) return false;
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return false;
    u32 value = 0;
    while (*p >= '0' && *p <= '9') {
        u32 digit = (u32)(*p++ - '0');
        value = value > (UINT16_MAX - digit) / 10u
              ? UINT16_MAX : value * 10u + digit;
    }
    if (*p != ' ' && *p != '\t') return false;
    *out = (u16)value;
    *cursor = p;
    return true;
}

static void sug_sidecar_load(SugModel *m) {
    char path[1100];
    if (!sug_sidecar_path(m->dir, path, sizeof path)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[CMD_HISTORY_ENTRY_CAP + 128];
    while (fgets(line, sizeof line, f)) {
        u16 a, sh, rj;
        const char *cursor = line;
        if (!sug_parse_counter(&cursor, &a) ||
            !sug_parse_counter(&cursor, &sh) ||
            !sug_parse_counter(&cursor, &rj))
            continue;   /* malformed line → ignore (degrade gracefully) */
        if (*cursor == ' ' || *cursor == '\t') cursor++;
        char *cmd = (char *)cursor;
        usize cl = strlen(cmd);
        while (cl && (cmd[cl - 1] == '\n' || cmd[cl - 1] == '\r')) cmd[--cl] = '\0';
        if (!cl) continue;
        u16 id = sug_find(m, cmd);
        if (id == SUG_NONE) continue;   /* command fell out of the log */
        m->cmds[id].accepts = a;
        m->cmds[id].shows   = sh;
        m->cmds[id].rejects = rj;
    }
    fclose(f);
}

static void sug_sidecar_flush(SugModel *m) {
    if (!m->valid || m->dirty == 0) return;
    char path[1100], tmp[1140];
    if (!sug_sidecar_path(m->dir, path, sizeof path)) return;
    snprintf(tmp, sizeof tmp, "%s.tmp-XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return; }
    for (i32 i = 0; i < m->cmd_count; i++) {
        const SugCmd *c = &m->cmds[i];
        if (c->accepts == 0 && c->shows == 0 && c->rejects == 0) continue;
        fprintf(f, "%u %u %u %s\n", c->accepts, c->shows, c->rejects,
                sug_str(m, (u16)i));
    }
    bool ok = !ferror(f);
    if (fclose(f)) ok = false;
    if (ok && rename(tmp, path) == 0) m->dirty = 0;
    else unlink(tmp);
}

/* ---- observe (the online-learning core) -------------------------------- */

static void sug_observe_line(SugModel *m, const char *cmd) {
    m->event_idx++;
    u16 id = sug_intern(m, cmd);
    if (id == SUG_NONE) { m->prev_cmd = SUG_NONE; return; }

    SugCmd *c = &m->cmds[id];
    c->count = sug_decayed(m, c) + 1.0f;   /* lazy decay, then bump */
    c->last_idx = m->event_idx;

    if (m->prev_cmd != SUG_NONE && m->prev_cmd != id) {
        SugTrans *t = sug_trans_find(m, m->prev_cmd, id, true);
        if (t) {
            u32 age = m->event_idx - t->last_idx;
            t->count = (age > 4096 ? 0.0f
                                   : t->count * powf(SUG_DECAY, (f32)age)) + 1.0f;
            t->last_idx = m->event_idx;
        }
    }
    m->prev_cmd = id;
}

/* Reset in place (zero allocator traffic) and replay the log snapshot. */
static void sug_rebuild(SugModel *m, const char *dir, const CmdHistory *hist) {
    if (!m->arena) {
        m->arena = malloc(SUG_ARENA_CAP);
        if (!m->arena) return;
    }
    /* Flush pending feedback BEFORE the overlay wipes it. */
    sug_sidecar_flush(m);

    snprintf(m->dir, sizeof m->dir, "%s", dir);
    m->arena_used = 0;
    m->arena_full = false;
    m->cmd_count  = 0;
    m->event_idx  = 0;
    m->prev_cmd   = SUG_NONE;
    m->dirty      = 0;
    for (u32 i = 0; i < SUG_MAX_TRANS; i++) {
        m->trans[i].prev = SUG_NONE;
        m->trans[i].next = SUG_NONE;
        m->trans[i].count = 0;
    }

    m->replaying = true;
    i32 n = cmd_history_count(hist);
    for (i32 i = 0; i < n; i++) {            /* oldest → newest */
        const char *e = cmd_history_entry(hist, i);
        if (e && e[0]) sug_observe_line(m, e);
    }
    m->replaying = false;

    sug_sidecar_load(m);
    m->valid = true;
}

/* Adopt the log's freshness key so the next best() is a cache hit. */
static void sug_adopt_key(SugModel *m, const CmdHistoryKey *key) {
    m->key = *key;
}

static bool sug_fresh(const SugModel *m, const char *dir,
                      const CmdHistoryKey *key) {
    return m->valid && cmd_history_key_equal(&m->key, key) &&
           strcmp(m->dir, dir) == 0;
}

/* ---- public API --------------------------------------------------------- */

void cmd_suggest_observe(const char *dir, const char *cmd,
                         const CmdHistoryKey *previous,
                         const CmdHistoryKey *now) {
    SugModel *m = &g_sug;
    if (!dir || !dir[0] || !cmd || !cmd[0] || !previous || !now) return;
    if (!m->valid || strcmp(m->dir, dir) != 0) {
        /* A background pane's directory cannot change this model. Keep the
         * hot model; best() still validates its own log's identity. */
        sug_sidecar_flush(m);
        return;
    }
    if (!sug_fresh(m, dir, previous)) {
        /* Another process appended since this model was built. Adopting only
         * the key of the record just written would hide those unseen
         * executions forever. */
        sug_sidecar_flush(m);
        m->valid = false;
        return;
    }
    sug_observe_line(m, cmd);
    sug_adopt_key(m, now);                   /* lockstep with the log */
}

void cmd_suggest_feedback(const char *dir, const char *cmd, bool accepted) {
    SugModel *m = &g_sug;
    if (!m->valid || !dir || !cmd || strcmp(m->dir, dir) != 0) return;
    u16 id = sug_find(m, cmd);
    if (id == SUG_NONE) return;
    SugCmd *c = &m->cmds[id];
    if (c->shows   < 0xFFFF) c->shows++;
    if (accepted) { if (c->accepts < 0xFFFF) c->accepts++; }
    else          { if (c->rejects < 0xFFFF) c->rejects++; }
    if (++m->dirty >= SUG_SIDECAR_FLUSH) sug_sidecar_flush(m);
}

const char *cmd_suggest_best(const char *dir, const char *prefix,
                             i32 prefix_len) {
    SugModel *m = &g_sug;
    if (!dir || !dir[0] || !prefix || prefix_len <= 0) return NULL;

    /* The CmdHistory cache is the freshness source — cmd_history_get costs
     * one stat on its own cache-hit path. */
    const CmdHistory *hist = cmd_history_get(dir);
    if (!hist) return NULL;
    CmdHistoryKey h_key;
    cmd_history_cache_key(hist, &h_key);

    if (!sug_fresh(m, dir, &h_key)) {
        sug_rebuild(m, dir, hist);
        sug_adopt_key(m, &h_key);
        if (!m->valid) return cmd_history_match(hist, prefix, prefix_len);
    }

    /* The contract baseline: the plain newest-first prefix match. */
    const char *newest = cmd_history_match(hist, prefix, prefix_len);

    /* Candidate range over the sorted index. */
    i32 lo = 0, hi = m->cmd_count;
    {
        i32 a = 0, b = m->cmd_count - 1;
        lo = m->cmd_count;
        while (a <= b) {
            i32 mid = (a + b) / 2;
            if (strncmp(sug_str(m, m->sorted[mid]), prefix,
                        (usize)prefix_len) < 0) a = mid + 1;
            else { lo = mid; b = mid - 1; }
        }
    }

    f32 best_score = -1e30f, second = -1e30f;
    i32 best_id = -1;
    f32 best_freq = 0.0f;
    for (i32 i = lo; i < hi && i < lo + 256; i++) {
        u16 id = m->sorted[i];
        const char *str = sug_str(m, id);
        if (strncmp(str, prefix, (usize)prefix_len) != 0) break;
        if ((i32)strlen(str) <= prefix_len) continue;   /* nothing to ghost */

        const SugCmd *c = &m->cmds[id];
        f32 freq = sug_decayed(m, c);
        f32 trans = 0.0f;
        if (m->prev_cmd != SUG_NONE) {
            SugTrans *t = sug_trans_find(m, m->prev_cmd, id, false);
            if (t) {
                u32 age = m->event_idx - t->last_idx;
                trans = age > 4096 ? 0.0f : t->count * powf(SUG_DECAY, (f32)age);
            }
        }
        f32 recency = powf(SUG_DECAY, (f32)(m->event_idx - c->last_idx));
        f32 acc = c->shows ? (f32)c->accepts / (f32)c->shows : 0.0f;
        f32 rej = c->shows ? (f32)c->rejects / (f32)c->shows : 0.0f;

        f32 score = 2.0f * log1pf(freq)
                  + 3.0f * log1pf(trans)
                  + 1.5f * recency
                  + 2.5f * acc
                  - 2.0f * rej;
        if (score > best_score) {
            second = best_score;
            best_score = score; best_id = id; best_freq = freq;
        } else if (score > second) {
            second = score;
        }
    }

    if (best_id < 0) return newest;   /* model has nothing — newest-first match */

    /* 1-char prefixes: only ghost when clearly confident ("no ghost beats
     * a wrong ghost"). A deliberate, documented change vs newest-first. */
    if (prefix_len == 1) {
        bool confident = best_freq >= SUG_GATE1_MINFREQ &&
                         (second <= 0.0f ||
                          best_score >= second * SUG_GATE1_RATIO);
        if (!confident) return NULL;
    }

    const char *best_str = sug_str(m, (u16)best_id);

    /* Margin contract: when the model barely disagrees with newest-first,
     * newest-first wins — enforced, not rhetorical. */
    if (newest && strcmp(newest, best_str) != 0) {
        /* Score the newest match with the same features for comparison. */
        u16 nid = sug_find(m, newest);
        if (nid != SUG_NONE) {
            const SugCmd *nc = &m->cmds[nid];
            f32 nfreq = sug_decayed(m, nc);
            f32 nrec  = powf(SUG_DECAY, (f32)(m->event_idx - nc->last_idx));
            f32 nacc  = nc->shows ? (f32)nc->accepts / (f32)nc->shows : 0.0f;
            f32 nrej  = nc->shows ? (f32)nc->rejects / (f32)nc->shows : 0.0f;
            f32 ntr   = 0.0f;
            if (m->prev_cmd != SUG_NONE) {
                SugTrans *t = sug_trans_find(m, m->prev_cmd, nid, false);
                if (t) {
                    /* Same decay as the candidate loop — the explicit
                     * SUG_MARGIN is the only thumb on the scale. */
                    u32 age = m->event_idx - t->last_idx;
                    ntr = age > 4096 ? 0.0f
                                     : t->count * powf(SUG_DECAY, (f32)age);
                }
            }
            f32 nscore = 2.0f * log1pf(nfreq) + 3.0f * log1pf(ntr)
                       + 1.5f * nrec + 2.5f * nacc - 2.0f * nrej;
            if (best_score < nscore * SUG_MARGIN) return newest;
        } else {
            return newest;   /* newest not even in the model — trust the log */
        }
    }
    return best_str;
}

void cmd_suggest_shutdown(void) {
    sug_sidecar_flush(&g_sug);
    free(g_sug.arena);
    g_sug.arena = NULL;
    g_sug.valid = false;
}

/* UI facade: only this worker performs file I/O and model rebuilds. The
 * synchronous API above uses thread-local caches, so history panels cannot
 * race the worker's model or borrow its arena pointers. */
#include <pthread.h>
#include <time.h>
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    bool started, stop, pending, query_active, notice;
    bool pending_fenced, query_fenced;
    bool history_pending, history_active, history_ready;
    bool history_pending_fenced, history_fenced;
    u64 sequence, query_order, history_order, last_write_order, query_dirty_order;
    u64 history_refresh_after;
    u64 history_generation;
    char history_dir[1024], history_result_dir[1024];
    char history_result[5][4096];
    i32 history_count;
    u64 generation;
    f64 queried;
    char dir[1024], prefix[4096];
    char result_dir[1024], result_prefix[4096], result[4096];
    struct SugTask *head, *tail;
    u32 tasks;
    u64 dropped_tasks;
    void (*wake)(void);
} sug_async = {.mu=PTHREAD_MUTEX_INITIALIZER,.cv=PTHREAD_COND_INITIALIZER};
typedef struct SugTask {
    struct SugTask *next;
    u64 order;
    bool feedback, accepted, completed;
    i32 exit_code;
    f64 duration;
    u16 text_offset;
    char data[]; /* owned directory + NUL + command + NUL */
} SugTask;
static f64 sug_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (f64)t.tv_sec+(f64)t.tv_nsec*1e-9;
}
static void *sug_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&sug_async.mu);
        while (!sug_async.stop && !sug_async.head && !sug_async.pending && !sug_async.history_pending)
            pthread_cond_wait(&sug_async.cv,&sug_async.mu);
        if (sug_async.stop && !sug_async.head) { pthread_mutex_unlock(&sug_async.mu); break; }
        /* Merge the two coalesced read mailboxes with the write FIFO. A read
         * waits for writes accepted before it, never for later producers.
         * Comparing tickets also prevents one read type starving the other. */
        u64 read_order = 0;
        if (sug_async.pending) read_order = sug_async.query_order;
        if (sug_async.history_pending &&
            (!read_order || sug_async.history_order < read_order))
            read_order = sug_async.history_order;
        SugTask *task=sug_async.head;
        if (!sug_async.stop && read_order && task && task->order > read_order)
            task = NULL;
        if (task) {
            sug_async.head=task->next;
            sug_async.tasks--;
            if (!sug_async.head) sug_async.tail=NULL;
            pthread_mutex_unlock(&sug_async.mu);
            const char *dir=task->data, *text=task->data+task->text_offset;
            if (task->feedback) cmd_suggest_feedback(dir,text,task->accepted);
            else if (task->completed)
                cmd_history_record_result(dir, text, task->exit_code, task->duration);
            else cmd_history_record(dir,text);
            free(task); continue;
        }
        if (sug_async.history_pending && sug_async.history_order == read_order) {
            char dir[1024], rows[5][4096] = {{0}};
            memcpy(dir, sug_async.history_dir, sizeof dir);
            u64 generation = sug_async.history_generation;
            u64 served_order = sug_async.history_order;
            sug_async.history_pending = false;
            sug_async.history_active = true;
            sug_async.history_fenced = sug_async.history_pending_fenced;
            sug_async.history_pending_fenced = false;
            pthread_mutex_unlock(&sug_async.mu);
            const CmdHistory *h = cmd_history_get(dir);
            i32 count = 0;
            for (i32 i = cmd_history_count(h) - 1; i >= 0 && count < 5; i--) {
                const char *entry = cmd_history_entry(h, i);
                bool duplicate = false;
                for (i32 j = 0; j < count; j++)
                    if (!strcmp(rows[j], entry)) duplicate = true;
                if (!duplicate) snprintf(rows[count++], sizeof rows[0], "%s", entry);
            }
            pthread_mutex_lock(&sug_async.mu);
            sug_async.history_active = false;
            sug_async.history_fenced = false;
            void (*wake)(void) = NULL;
            if (!sug_async.stop && generation == sug_async.history_generation) {
                wake = sug_async.wake;
                memcpy(sug_async.history_result, rows, sizeof rows);
                memcpy(sug_async.history_result_dir, dir, sizeof dir);
                sug_async.history_count = count;
                sug_async.history_ready = true;
                sug_async.notice = true;
                if (!sug_async.history_pending && sug_async.history_refresh_after > served_order) {
                    sug_async.history_pending = true;
                    sug_async.history_pending_fenced = true;
                    sug_async.history_order = ++sug_async.sequence;
                }
            }
            pthread_mutex_unlock(&sug_async.mu);
            if (wake) wake();
            continue;
        }
        char dir[1024],prefix[4096],answer[4096];
        memcpy(dir,sug_async.dir,sizeof dir); memcpy(prefix,sug_async.prefix,sizeof prefix);
        u64 generation=sug_async.generation;
        u64 served_order=sug_async.query_order;
        sug_async.pending=false;
        sug_async.query_active=true;
        sug_async.query_fenced=sug_async.pending_fenced;
        sug_async.pending_fenced=false;
        pthread_mutex_unlock(&sug_async.mu);
        const char *best=cmd_suggest_best(dir,prefix,(i32)strlen(prefix));
        snprintf(answer,sizeof answer,"%s",best?best:"");
        pthread_mutex_lock(&sug_async.mu);
        sug_async.query_active=false;
        sug_async.query_fenced=false;
        void (*wake)(void) = NULL;
        if (!sug_async.stop && generation==sug_async.generation) {
            wake = sug_async.wake;
            memcpy(sug_async.result_dir,dir,sizeof dir);
            memcpy(sug_async.result_prefix,prefix,sizeof prefix);
            memcpy(sug_async.result,answer,sizeof answer);
            sug_async.queried=sug_now(); /* age the completed answer, not its I/O wait */
            sug_async.notice=true;
            /* Writes accepted while this read was still queued are after its
             * fence too. Refresh once they have run, even if no producer
             * happened to race the backend call itself. */
            if (!sug_async.pending && sug_async.query_dirty_order > served_order) {
                sug_async.pending=true;
                sug_async.pending_fenced=true;
                sug_async.query_order=++sug_async.sequence;
            }
        }
        pthread_mutex_unlock(&sug_async.mu);
        if (wake) wake();
    }
    cmd_suggest_shutdown(); cmd_history_shutdown(); return NULL;
}
static bool sug_start_locked(void) {
    if (sug_async.started) return !sug_async.stop;
    sug_async.stop=false;
    if (pthread_create(&sug_async.thread,NULL,sug_worker,NULL)) return false;
    sug_async.started=true; return true;
}
bool cmd_suggest_async_best(const char *dir,const char *prefix,i32 len,char *out,usize cap) {
    if (!out || !cap) return false;
    out[0]=0;
    if (!dir || !prefix || len<=0 || strlen(dir)>=sizeof sug_async.dir ||
        (usize)len>=sizeof sug_async.prefix) return false;
    char request[4096];
    memcpy(request,prefix,(usize)len);request[len]=0;
    pthread_mutex_lock(&sug_async.mu);
    if (!sug_start_locked()) { pthread_mutex_unlock(&sug_async.mu); return false; }
    f64 now=sug_now();
    /* Slow storage must not make each periodic refresh supersede the same
     * unfinished query. A changed key still replaces pending work at once. */
    if (strcmp(dir,sug_async.dir) || strcmp(request,sug_async.prefix) ||
        (!sug_async.pending && !sug_async.query_active && now-sug_async.queried>1.0)) {
        snprintf(sug_async.dir,sizeof sug_async.dir,"%s",dir);
        memcpy(sug_async.prefix,request,(usize)len+1);
        sug_async.generation++; sug_async.queried=now; sug_async.pending=true;
        sug_async.pending_fenced=false;
        sug_async.query_dirty_order=0;
        sug_async.query_order=++sug_async.sequence;
        pthread_cond_signal(&sug_async.cv);
    }
    if (!strcmp(dir,sug_async.result_dir) && !strcmp(request,sug_async.result_prefix) &&
        strlen(sug_async.result)<cap)
        snprintf(out,cap,"%s",sug_async.result);
    pthread_mutex_unlock(&sug_async.mu);
    return out[0]!=0;
}
bool cmd_suggest_async_history(const char *dir, char out[5][4096], i32 *count, bool refresh) {
    if (!dir || !dir[0] || strlen(dir) >= 1024 || !out || !count) return false;
    *count = 0;
    pthread_mutex_lock(&sug_async.mu);
    if (!sug_start_locked()) { pthread_mutex_unlock(&sug_async.mu); return false; }
    bool changed = strcmp(dir, sug_async.history_dir) != 0;
    if (changed ||
        (refresh && !sug_async.history_pending &&
         (!sug_async.history_active || sug_async.last_write_order > sug_async.history_order))) {
        snprintf(sug_async.history_dir, sizeof sug_async.history_dir, "%s", dir);
        if (changed || !sug_async.history_fenced) sug_async.history_generation++;
        sug_async.history_pending_fenced = !changed && sug_async.history_active;
        sug_async.history_pending = true;
        sug_async.history_order = ++sug_async.sequence;
        sug_async.history_refresh_after = 0;
        sug_async.history_ready = false;
        pthread_cond_signal(&sug_async.cv);
    } else if (refresh && sug_async.history_pending) {
        /* A newer refresh must not push the existing fence forward or be
         * forgotten. Service the queued snapshot, then cover these writes. */
        sug_async.history_refresh_after = sug_async.last_write_order;
    }
    bool ready = sug_async.history_ready && !strcmp(dir, sug_async.history_result_dir);
    if (ready) {
        memcpy(out, sug_async.history_result, sizeof sug_async.history_result);
        *count = sug_async.history_count;
    }
    pthread_mutex_unlock(&sug_async.mu);
    return ready;
}

static void sug_task(const char *dir,const char *text,bool feedback,bool accepted,
                     bool completed, i32 exit_code, f64 duration) {
    if (!dir || !text || !text[0]) return;
    usize dir_len=strlen(dir), text_len=strlen(text);
    if (dir_len>=1024 || text_len>=4096) return;
    /* The bounds above also bound this sum. Short commands should not each
     * retain two maximum-size buffers while the disk worker is busy. */
    SugTask *t=malloc(sizeof *t+dir_len+1+text_len+1); if (!t) return;
    *t=(SugTask){.feedback=feedback,.accepted=accepted,.completed=completed,
                 .exit_code=exit_code,.duration=duration,.text_offset=(u16)(dir_len+1)};
    memcpy(t->data,dir,dir_len+1);
    memcpy(t->data+t->text_offset,text,text_len+1);
    pthread_mutex_lock(&sug_async.mu);
    if (!sug_start_locked()) { pthread_mutex_unlock(&sug_async.mu); free(t); return; }
    /* Learning/history metadata is best effort under overload; it must not
     * grow without bound or block the terminal behind slow storage. */
    if (sug_async.tasks >= 1024) {
        sug_async.dropped_tasks++;pthread_mutex_unlock(&sug_async.mu);free(t);return;
    }
    sug_async.tasks++;
    t->order=++sug_async.sequence;
    sug_async.last_write_order=t->order;
    if (!strcmp(dir,sug_async.result_dir)) sug_async.result[0] = 0;
    if (!strcmp(dir,sug_async.dir)) {
        sug_async.queried = 0;
        sug_async.query_dirty_order=t->order;
        /* A first lookup racing a mutation is replaced. Its replacement is a
         * snapshot at a fixed write fence: invalidating every replacement
         * under continuous writes would run reads fairly but never publish
         * any answer. Later writes schedule another fenced refresh; changed
         * directory/prefix keys still invalidate at once. */
        if (!sug_async.query_active || !sug_async.query_fenced)
            sug_async.generation++;
        /* A mutation racing a lookup needs one replacement after the queued
         * writes. Unrelated directories leave the foreground query intact. */
        if (!sug_async.pending && sug_async.query_active) {
            sug_async.pending=true;
            sug_async.pending_fenced=true;
            sug_async.query_order=++sug_async.sequence;
        }
    }
    if (sug_async.tail) sug_async.tail->next=t;else sug_async.head=t;
    sug_async.tail=t;pthread_cond_signal(&sug_async.cv);pthread_mutex_unlock(&sug_async.mu);
}
void cmd_suggest_async_record(const char *dir,const char *text){sug_task(dir,text,false,false,false,0,0);}
void cmd_suggest_async_feedback(const char *dir,const char *text,bool accepted){sug_task(dir,text,true,accepted,false,0,0);}
void cmd_suggest_async_record_result(const char *dir, const char *command,
                                     i32 exit_code, f64 duration) {
    sug_task(dir, command, false, false, true, exit_code, duration);
}
void cmd_suggest_async_set_wake(void (*wake)(void)) {
    pthread_mutex_lock(&sug_async.mu);
    sug_async.wake = wake;
    pthread_mutex_unlock(&sug_async.mu);
}
bool cmd_suggest_async_notice(void) {
    pthread_mutex_lock(&sug_async.mu);bool notice=sug_async.notice;sug_async.notice=false;
    pthread_mutex_unlock(&sug_async.mu);return notice;
}
void cmd_suggest_async_shutdown(void) {
    pthread_mutex_lock(&sug_async.mu);bool started=sug_async.started;
    sug_async.stop=true;pthread_cond_signal(&sug_async.cv);pthread_mutex_unlock(&sug_async.mu);
    if (started) pthread_join(sug_async.thread,NULL);
    pthread_mutex_lock(&sug_async.mu);sug_async.started=false;sug_async.pending=false;sug_async.query_active=false;sug_async.notice=false;
    sug_async.pending_fenced=false;sug_async.query_fenced=false;
    sug_async.history_pending=false;sug_async.history_active=false;
    sug_async.history_pending_fenced=false;sug_async.history_fenced=false;
    sug_async.history_refresh_after=0;
    sug_async.history_ready=false;sug_async.history_dir[0]=0;
    sug_async.dir[0]=sug_async.prefix[0]=sug_async.result[0]=0;
    pthread_mutex_unlock(&sug_async.mu);
}
