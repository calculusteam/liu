/*
 * Liu — learned command suggestion engine (see cmd_suggest.h).
 *
 * Single-slot model mirroring cmd_history's cache pattern: one directory is
 * hot at a time (the focused shell's cwd). Keyed on (dir, mtime, size) of
 * the backing cmdhistory log; observe() adopts the post-append key so the
 * common path never rebuilds. Rebuild = arena/table RESET (no allocator
 * traffic) + replay of the CmdHistory snapshot oldest→newest.
 */
#include "core/cmd_suggest.h"
#include "core/cmd_history.h"
#include "core/project_dir.h"   /* liu_project_data_dir — sidecar location */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    i64     key_mtime, key_size;     /* freshness key of the backing log   */
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

static SugModel g_sug = { .prev_cmd = SUG_NONE };

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

    usize len = strlen(cmd);
    if (m->arena_full || m->arena_used + len + 1 > SUG_ARENA_CAP) {
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

static void sug_sidecar_load(SugModel *m) {
    char path[1100];
    if (!sug_sidecar_path(m->dir, path, sizeof path)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        unsigned a, sh, rj;
        int off = 0;
        if (sscanf(line, "%u %u %u %n", &a, &sh, &rj, &off) != 3 || off <= 0)
            continue;   /* malformed line → ignore (degrade gracefully) */
        char *cmd = line + off;
        usize cl = strlen(cmd);
        while (cl && (cmd[cl - 1] == '\n' || cmd[cl - 1] == '\r')) cmd[--cl] = '\0';
        if (!cl) continue;
        u16 id = sug_find(m, cmd);
        if (id == SUG_NONE) continue;   /* command fell out of the log */
        m->cmds[id].accepts = (u16)(a  > 0xFFFF ? 0xFFFF : a);
        m->cmds[id].shows   = (u16)(sh > 0xFFFF ? 0xFFFF : sh);
        m->cmds[id].rejects = (u16)(rj > 0xFFFF ? 0xFFFF : rj);
    }
    fclose(f);
}

static void sug_sidecar_flush(SugModel *m) {
    if (!m->valid || m->dirty == 0) return;
    char path[1100], tmp[1140];
    if (!sug_sidecar_path(m->dir, path, sizeof path)) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (i32 i = 0; i < m->cmd_count; i++) {
        const SugCmd *c = &m->cmds[i];
        if (c->accepts == 0 && c->shows == 0 && c->rejects == 0) continue;
        fprintf(f, "%u %u %u %s\n", c->accepts, c->shows, c->rejects,
                sug_str(m, (u16)i));
    }
    fclose(f);
    rename(tmp, path);   /* atomic on POSIX; near-atomic enough on Win32 */
    m->dirty = 0;
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
static void sug_adopt_key(SugModel *m, i64 mtime, i64 size) {
    m->key_mtime = mtime;
    m->key_size  = size;
}

static bool sug_fresh(const SugModel *m, const char *dir,
                      i64 mtime, i64 size) {
    return m->valid &&
           m->key_mtime == mtime && m->key_size == size &&
           strcmp(m->dir, dir) == 0;
}

/* ---- public API --------------------------------------------------------- */

void cmd_suggest_observe(const char *dir, const char *cmd,
                         i64 log_mtime, i64 log_size) {
    SugModel *m = &g_sug;
    if (!dir || !dir[0] || !cmd || !cmd[0]) return;
    if (!m->valid || strcmp(m->dir, dir) != 0) {
        /* Different directory than the hot model: let the next best() do
         * the rebuild (it has the CmdHistory snapshot); just invalidate. */
        m->valid = false;
        return;
    }
    sug_observe_line(m, cmd);
    sug_adopt_key(m, log_mtime, log_size);   /* lockstep with the log */
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
     * one stat on its own cache-hit path, which the old code already paid. */
    const CmdHistory *hist = cmd_history_get(dir);
    if (!hist) return NULL;
    i64 h_mtime, h_size;
    cmd_history_cache_key(hist, &h_mtime, &h_size);

    if (!sug_fresh(m, dir, h_mtime, h_size)) {
        sug_rebuild(m, dir, hist);
        sug_adopt_key(m, h_mtime, h_size);
        if (!m->valid) return cmd_history_match(hist, prefix, prefix_len);
    }

    /* The contract baseline: what the old engine would have shown. */
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

    if (best_id < 0) return newest;   /* model has nothing — old behavior */

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
