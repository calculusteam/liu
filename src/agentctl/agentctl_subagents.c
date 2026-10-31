/*
 * Liu — the subagents an agent in a pane starts itself (agentctl_subagents.h).
 *
 * One mutex covers the panes the main thread names and the subagents' shared
 * half (their log, state and words); the watcher owns the rest of a
 * subagent — where it is read from and how far — and is the only one that
 * adds or frees one. The main thread only marks a subagent to go (`retire`);
 * the watcher frees it at the start of its next pass.
 */
#include "agentctl/agentctl_subagents.h"
#include "agentctl/agentctl_ring.h"
#include "agentctl/agentctl_subread.h"
#include "core/sqlite_harden.h"
#include "history/event.h"
#include "history/opencode_store.h"
#include "sqlite3.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SUB_PANES_MAX       32
#define SUB_SEEN_MAX        8192
#define SUB_HEAD_BYTES      (64u * 1024u)      /* a long transcript's start: its task */
#define SUB_READ_MAX        (512u * 1024u)     /* read of one transcript in one pass */
#define SUB_FIRSTLINE_MAX   (1024u * 1024u)    /* a Codex rollout's header line */
#define SUB_META_MAX        (256u * 1024u)
#define SUB_LOOK_MS         1000
#define SUB_READ_MS         500
#define SUB_SETTLED_READ_MS 3000
#define SUB_CLAUDE_QUIET_MS 4000               /* a final answer, then nothing: done */
#define SUB_OC_ROWS         400

typedef struct { char **v; u32 n, cap; } KeySet;

/* Lines read for one subagent, kept until they go into its ring under the
 * lock: records of [style u8][length u32][bytes]. */
typedef struct { u8 *buf; usize len, cap; } LineBuf;

typedef struct {
    bool used;
    bool gone;                    /* the pane closed: the watcher frees it */
    AgentctlSubPane reg;
    u64  reg_gen;                 /* moves when the agent or its session changes */
    bool prompted;                /* a prompt went to its agent since the last pass: it is about to work */
    /* the watcher's */
    AgentctlSubPane wreg;         /* reg as the watcher took it at the start of its pass */
    u64  done_gen;                /* the reg_gen its looking state is for */
    bool baseline;                /* the first look at this session is done */
    i64  look_until, next_look;
    bool agent_left;              /* the agent of reg_gen left: its running subagents were stopped */
    bool root_ok;
    struct timespec root_mt;
    char day_dir[2][1100];
    struct timespec day_mt[2];
    bool day_ok[2];
    KeySet seen;
    char sdir[1100];              /* where its subagents live (Claude, Grok, Kimi, Oh My Pi) */
    char thread[64];              /* Codex: the pane's thread */
    /* Command Code: the parent's transcript, read as it grows */
    u64  cc_off;
    dev_t cc_dev;
    ino_t cc_ino;
    bool cc_open;
    char *cc_part;
    usize cc_part_len, cc_part_cap;
    /* the session of an agent Liu's own lookup does not find (Droid, Copilot
     * CLI, Crush, OpenClaw): found here, from the pane's folder */
    char own_id[256];
    char own_path[1100];
    /* the database some agents keep their subagents in */
    sqlite3 *db;
    char db_path[1100];
    i64  db_retry;
    i64  row;                     /* Devin: the session's last row read */
} SubPane;

typedef struct {
    u32  id;                      /* 0 = a free slot */
    u64  pane;
    u64  pane_gen;
    i32  tool;
    char key[200];
    char label[160];
    char kind[64];
    char cwd[1024];
    char owner_label[96];
    char path[1100];              /* its transcript, "" = none */
    char meta[1100];              /* the file that says its status, "" = none */
    char oc_id[128];              /* OpenCode: its session */
    char cc_call[128];            /* Command Code: the call that started it */
    char cc_bg[64];               /* … and its background agent's id */
    bool cc_background;
    /* the watcher's */
    bool head_done;
    u64  off;
    dev_t dev;
    ino_t ino;
    i64  size_seen, grew_at, next_read;
    struct timespec meta_mt;
    bool meta_ok;
    char *part;
    usize part_len, part_cap;
    i64  oc_seq;                  /* the last row read (OpenCode, Crush, Hermes, OpenClaw); messages read (Cline CLI) */
    char ev_id[128];              /* OpenClaw: the window its transcript events are filed under */
    char cp_agent[96];            /* Copilot CLI: its instance id */
    i64  file_mt;                 /* Cline CLI: the messages file as last read (mtime, ns) */
    AgentctlSubRead rd;
    LineBuf pend;                 /* lines read and not yet in the ring */
    /* shared, under g_mu */
    AgentctlRing ring;
    u8   state, outcome;
    i64  created_ms, start_ms, end_ms;
    char model[96];
    char error[256];
    u64  tok_in, tok_out;
    bool retire;                  /* dismissed, or its turn is over: the watcher frees it */
} SubJob;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static SubPane g_panes[SUB_PANES_MAX];
static SubJob  g_jobs[AGENTCTL_MAX_WATCHED];
static u32     g_next_id = 1;
static bool    g_enabled = true;
static bool    g_thread_mode, g_thread_on, g_stop, g_kick;
static pthread_t g_thread;
static void  (*g_wake)(void);
static _Atomic u64  g_jobs_gen, g_cards_gen;
static _Atomic i32  g_shown;
static _Atomic bool g_log_watched;

/* the watcher's OpenCode connection */
static sqlite3 *g_oc_db;
static bool     g_oc_v2;
static i64      g_oc_retry_at;

static i64 wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static i64 ts_ms(const struct timespec *t) { return (i64)t->tv_sec * 1000 + t->tv_nsec / 1000000; }

#ifdef __APPLE__
#define ST_MTIM(st) ((st).st_mtimespec)
#else
#define ST_MTIM(st) ((st).st_mtim)
#endif

static bool same_time(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

bool agentctl_subagents_supported(i32 tool) {
    switch (tool) {
    case CHAT_TOOL_CLAUDE: case CHAT_TOOL_CODEX: case CHAT_TOOL_OPENCODE: case CHAT_TOOL_XAI:
    case CHAT_TOOL_KIMI:   case CHAT_TOOL_OMP:   case CHAT_TOOL_COMMANDCODE:
    case CHAT_TOOL_QWEN:   case CHAT_TOOL_DROID: case CHAT_TOOL_CLINE_CLI: case CHAT_TOOL_CRUSH:
    case CHAT_TOOL_HERMES: case CHAT_TOOL_OPENCLAW: case CHAT_TOOL_PI: case CHAT_TOOL_COPILOT:
    case CHAT_TOOL_DEVIN:  case CHAT_TOOL_QODER: case CHAT_TOOL_CURSOR: case CHAT_TOOL_KILO:
    case CHAT_TOOL_AMAZON_Q:
        return true;
    default:
        return false;
    }
}

/* The agents whose subagents are read from the parent's own log: a job has
 * no file of its own to read. */
static bool parent_fed(i32 tool) {
    return tool == CHAT_TOOL_COMMANDCODE || tool == CHAT_TOOL_PI || tool == CHAT_TOOL_COPILOT ||
           tool == CHAT_TOOL_DEVIN;
}

/* The agents whose session Liu's lookup does not name: the watcher finds it
 * from the pane's folder. */
static bool self_locating(i32 tool) {
    return tool == CHAT_TOOL_DROID || tool == CHAT_TOOL_COPILOT || tool == CHAT_TOOL_CRUSH ||
           tool == CHAT_TOOL_OPENCLAW || tool == CHAT_TOOL_CURSOR || tool == CHAT_TOOL_KILO ||
           tool == CHAT_TOOL_AMAZON_Q;
}

/* The agents whose record says no end: a final answer and quiet is one. */
static bool quiet_done(i32 tool) {
    return tool == CHAT_TOOL_CLAUDE || tool == CHAT_TOOL_DROID || tool == CHAT_TOOL_CLINE_CLI ||
           tool == CHAT_TOOL_CRUSH || tool == CHAT_TOOL_QODER || tool == CHAT_TOOL_CURSOR ||
           tool == CHAT_TOOL_KILO;
}

/* ------------------------------------------------------------- key set --- */

static bool seen_has(const KeySet *s, const char *k) {
    for (u32 i = 0; i < s->n; i++) if (!strcmp(s->v[i], k)) return true;
    return false;
}

static void seen_add(KeySet *s, const char *k) {
    if (seen_has(s, k)) return;
    if (s->n >= SUB_SEEN_MAX) {                      /* the oldest goes */
        free(s->v[0]);
        memmove(s->v, s->v + 1, (s->n - 1) * sizeof *s->v);
        s->n--;
    }
    if (s->n == s->cap) {
        u32 cap = s->cap ? s->cap * 2 : 64;
        char **nv = realloc(s->v, cap * sizeof *nv);
        if (!nv) return;
        s->v = nv;
        s->cap = cap;
    }
    char *d = strdup(k);
    if (d) s->v[s->n++] = d;
}

static void seen_free(KeySet *s) {
    for (u32 i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    memset(s, 0, sizeof *s);
}

/* ---------------------------------------------------------------- files --- */

/* A regular file opened for reading, judged on its descriptor: a FIFO or a
 * device under a store's folder never holds the watcher. */
static int open_regular(const char *path, struct stat *st) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, st) != 0 || !S_ISREG(st->st_mode)) { close(fd); return -1; }
    return fd;
}

/* Up to `cap` bytes of a small file (a meta file), NUL-terminated, malloc'd. */
static char *slurp(const char *path, usize cap, usize *len, struct timespec *mt) {
    struct stat st;
    int fd = open_regular(path, &st);
    if (fd < 0) return NULL;
    if (mt) *mt = ST_MTIM(st);
    usize want = st.st_size > 0 && (u64)st.st_size < cap ? (usize)st.st_size : cap;
    char *buf = malloc(want + 1);
    if (!buf) { close(fd); return NULL; }
    usize got = 0;
    while (got < want) {
        ssize_t r = pread(fd, buf + got, want - got, (off_t)got);
        if (r <= 0) break;
        got += (usize)r;
    }
    close(fd);
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

/* The first line of a file, when it is whole within `cap` bytes. */
static char *first_line(const char *path, usize cap, usize *len) {
    usize n = 0;
    char *buf = slurp(path, cap, &n, NULL);
    if (!buf) return NULL;
    char *nl = memchr(buf, '\n', n);
    if (!nl) { free(buf); return NULL; }
    *nl = '\0';
    if (len) *len = (usize)(nl - buf);
    return buf;
}

static bool path_join(char *out, usize cap, const char *a, const char *b) {
    int w = snprintf(out, cap, "%s/%s", a, b);
    return w > 0 && (usize)w < cap;
}

static void dir_of(const char *path, char *out, usize cap) {
    snprintf(out, cap, "%s", path);
    char *s = strrchr(out, '/');
    if (s && s != out) *s = '\0';
    else out[0] = '\0';
}

static bool ends_with(const char *s, const char *suffix) {
    usize n = strlen(s), k = strlen(suffix);
    return n >= k && !strcmp(s + n - k, suffix);
}

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    return h && h[0] == '/' ? h : NULL;
}

/* ----------------------------------------------------------- the lines --- */

static void lb_emit(void *ctx, AgentctlStyle st, const char *text, usize n) {
    LineBuf *lb = ctx;
    if (!lb || !text) return;
    if (n > 64u * 1024u) n = 64u * 1024u;
    usize need = lb->len + 1 + 4 + n;
    if (need > lb->cap) {
        usize cap = lb->cap ? lb->cap : 4096;
        while (cap < need) cap *= 2;
        u8 *nb = realloc(lb->buf, cap);
        if (!nb) return;
        lb->buf = nb;
        lb->cap = cap;
    }
    u32 len = (u32)n;
    lb->buf[lb->len++] = (u8)st;
    memcpy(lb->buf + lb->len, &len, 4);
    lb->len += 4;
    memcpy(lb->buf + lb->len, text, n);
    lb->len += n;
}

static bool lb_push(LineBuf *lb, AgentctlRing *ring) {
    usize i = 0;
    bool any = false;
    while (i + 5 <= lb->len) {
        u8 st = lb->buf[i];
        u32 n;
        memcpy(&n, lb->buf + i + 1, 4);
        i += 5;
        if (i + n > lb->len) break;
        agentctl_ring_push(ring, st, lb->buf + i, n);
        i += n;
        any = true;
    }
    lb->len = 0;
    return any;
}

/* --------------------------------------------------------------- jobs --- */

static SubJob *job_by_id(u32 id) {
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) if (g_jobs[i].id == id) return &g_jobs[i];
    return NULL;
}

static bool job_shown(const SubJob *j) { return j->id && !j->retire; }

static SubPane *pane_by_serial(u64 serial) {
    for (i32 i = 0; i < SUB_PANES_MAX; i++)
        if (g_panes[i].used && !g_panes[i].gone && g_panes[i].reg.pane == serial) return &g_panes[i];
    return NULL;
}

static void count_shown(void) {
    i32 n = 0;
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) if (job_shown(&g_jobs[i])) n++;
    atomic_store_explicit(&g_shown, n, memory_order_release);
}

/* Under g_mu. The shown count is taken before the generation moves, so a
 * reader that sees the new generation sees the jobs behind it: the main
 * thread reads the generation, then whether there are any, and keeps what it
 * read until the generation moves again. */
static void bump_jobs(void) {
    count_shown();
    atomic_fetch_add_explicit(&g_jobs_gen, 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&g_cards_gen, 1, memory_order_acq_rel);
}

/* Under g_mu, by the watcher. */
static void job_free(SubJob *j) {
    agentctl_ring_free(&j->ring);
    agentctl_subread_free(&j->rd);
    free(j->part);
    free(j->pend.buf);
    memset(j, 0, sizeof *j);
}

/* A new subagent, from what its store says. Under g_mu, by the watcher.
 * NULL when every slot holds a running one. */
static SubJob *job_new(const SubPane *p, const char *key, const AgentctlSubMeta *m, i64 now) {
    SubJob *slot = NULL;
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED && !slot; i++) if (!g_jobs[i].id) slot = &g_jobs[i];
    if (!slot) {
        /* the one that settled longest ago makes room */
        SubJob *old = NULL;
        for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) {
            SubJob *j = &g_jobs[i];
            if (j->state == AGJ_RUNNING && !j->retire) continue;
            if (!old || j->end_ms < old->end_ms) old = j;
        }
        if (!old) return NULL;
        job_free(old);
        slot = old;
    }
    memset(slot, 0, sizeof *slot);
    slot->id = AGENTCTL_WATCHED_BIT | (g_next_id++ & (AGENTCTL_WATCHED_BIT - 1));
    if (g_next_id >= AGENTCTL_WATCHED_BIT) g_next_id = 1;
    slot->pane = p->wreg.pane;
    slot->pane_gen = p->done_gen;
    slot->tool = p->wreg.tool;
    snprintf(slot->key, sizeof slot->key, "%s", key);
    snprintf(slot->label, sizeof slot->label, "%s", m && m->label[0] ? m->label : "subagent");
    snprintf(slot->kind, sizeof slot->kind, "%s", m ? m->kind : "");
    snprintf(slot->cwd, sizeof slot->cwd, "%s", p->wreg.cwd);
    snprintf(slot->owner_label, sizeof slot->owner_label, "%s", p->wreg.owner_label);
    if (m && m->model[0]) snprintf(slot->model, sizeof slot->model, "%s", m->model);
    agentctl_subread_init(&slot->rd, p->wreg.tool);
    if (m) {
        slot->rd.ordinal_from = m->ordinal_from;
        if (m->start_ms > 0) slot->rd.start_ms = m->start_ms;
    }
    slot->state = AGJ_RUNNING;
    slot->created_ms = now;
    slot->start_ms = m && m->start_ms > 0 ? m->start_ms : now;
    return slot;
}

/* The shared half of a job, from what the watcher read; true when its state
 * changed. Under g_mu. */
static bool job_publish(SubJob *j) {
    bool lines = lb_push(&j->pend, &j->ring);
    u8 state = AGJ_RUNNING, outcome = AGO_NONE;
    switch (j->rd.status) {
    case AGSUB_DONE:    state = AGJ_IDLE; outcome = AGO_DONE; break;
    case AGSUB_FAILED:  state = AGJ_IDLE; outcome = AGO_FAILED; break;
    case AGSUB_STOPPED: state = AGJ_IDLE; outcome = AGO_STOPPED; break;
    default: break;
    }
    bool changed = state != j->state || outcome != j->outcome;
    j->state = state;
    j->outcome = outcome;
    if (j->rd.start_ms > 0) j->start_ms = j->rd.start_ms;
    j->end_ms = state == AGJ_RUNNING ? 0 : (j->rd.end_ms > 0 ? j->rd.end_ms : wall_ms());
    if (j->rd.model[0]) snprintf(j->model, sizeof j->model, "%s", j->rd.model);
    snprintf(j->error, sizeof j->error, "%s", j->rd.error);
    if (j->rd.kind[0] && !j->kind[0]) snprintf(j->kind, sizeof j->kind, "%s", j->rd.kind);
    /* a record with no title of its own: the first line of its task */
    if (j->rd.task_head[0] && !strcmp(j->label, "subagent")) snprintf(j->label, sizeof j->label, "%s", j->rd.task_head);
    j->tok_in = j->rd.tok_in;
    j->tok_out = j->rd.tok_out;
    if (changed) bump_jobs();
    else if (lines) atomic_fetch_add_explicit(&g_cards_gen, 1, memory_order_acq_rel);
    return changed;
}

/* ------------------------------------------------------ reading a file --- */

static void feed_lines(SubJob *j, const char *data, usize n, LineBuf *lb) {
    usize i = 0;
    while (i < n) {
        const char *nl = memchr(data + i, '\n', n - i);
        if (!nl) {
            usize rest = n - i;
            if (j->part_len + rest > AGENTCTL_STREAM_LINE_MAX) {
                j->part_len = 0;                     /* an over-long line is skipped */
            } else {
                if (j->part_len + rest > j->part_cap) {
                    usize cap = j->part_cap ? j->part_cap : 4096;
                    while (cap < j->part_len + rest) cap *= 2;
                    char *np = realloc(j->part, cap);
                    if (!np) return;
                    j->part = np;
                    j->part_cap = cap;
                }
                memcpy(j->part + j->part_len, data + i, rest);
                j->part_len += rest;
            }
            return;
        }
        usize len = (usize)(nl - (data + i));
        if (j->part_len) {
            if (j->part_len + len <= AGENTCTL_STREAM_LINE_MAX && j->part_len + len <= j->part_cap) {
                memcpy(j->part + j->part_len, data + i, len);
                agentctl_subread_line(&j->rd, j->part, j->part_len + len, lb_emit, lb);
            } else if (j->part_len + len <= AGENTCTL_STREAM_LINE_MAX) {
                char *np = realloc(j->part, j->part_len + len);
                if (np) {
                    j->part = np;
                    j->part_cap = j->part_len + len;
                    memcpy(j->part + j->part_len, data + i, len);
                    agentctl_subread_line(&j->rd, j->part, j->part_len + len, lb_emit, lb);
                }
            }
            j->part_len = 0;
        } else {
            agentctl_subread_line(&j->rd, data + i, len, lb_emit, lb);
        }
        i += len + 1;
    }
}

static void read_range(int fd, SubJob *j, u64 from, u64 to, LineBuf *lb) {
    char buf[64 * 1024];
    u64 at = from;
    while (at < to) {
        usize want = to - at < sizeof buf ? (usize)(to - at) : sizeof buf;
        ssize_t r = pread(fd, buf, want, (off_t)at);
        if (r <= 0) break;
        feed_lines(j, buf, (usize)r, lb);
        at += (u64)r;
    }
    j->off = at;
}

/* What the transcript grew by. The first read of a long one takes its start
 * (the task) and then its end. */
static void read_transcript(SubJob *j, i64 now, LineBuf *lb) {
    struct stat st;
    int fd = open_regular(j->path, &st);
    if (fd < 0) return;
    if (j->dev != st.st_dev || j->ino != st.st_ino || (u64)st.st_size < j->off) {
        /* a new file at the path: read from its start */
        j->dev = st.st_dev;
        j->ino = st.st_ino;
        j->off = 0;
        j->part_len = 0;
        j->head_done = false;
    }
    u64 size = (u64)st.st_size;
    if ((i64)size != j->size_seen) {
        j->size_seen = (i64)size;
        j->grew_at = now;
    }
    if (!j->head_done) {
        j->head_done = true;
        if (size > AGENTCTL_SUB_TAIL_BYTES + SUB_HEAD_BYTES) {
            read_range(fd, j, 0, SUB_HEAD_BYTES, lb);
            j->part_len = 0;
            lb_emit(lb, AGS_DIM, "  \xE2\x80\xA6 (earlier steps are in its transcript)", 44);
            /* from the first whole line of the end */
            u64 from = size - AGENTCTL_SUB_TAIL_BYTES;
            char c;
            while (from < size && pread(fd, &c, 1, (off_t)(from - 1)) == 1 && c != '\n') from++;
            j->off = from;
        }
    }
    if (size > j->off) {
        u64 to = size - j->off > SUB_READ_MAX ? j->off + SUB_READ_MAX : size;
        read_range(fd, j, j->off, to, lb);
    }
    close(fd);
}

/* The status its meta file says, when the file changed. */
static void read_meta(SubJob *j, LineBuf *lb) {
    if (!j->meta[0]) return;
    struct stat st;
    if (stat(j->meta, &st) != 0) return;
    if (j->meta_ok && same_time(&j->meta_mt, &ST_MTIM(st))) return;
    usize n = 0;
    struct timespec mt;
    char *buf = slurp(j->meta, SUB_META_MAX, &n, &mt);
    if (!buf) return;
    j->meta_mt = mt;
    j->meta_ok = true;
    AgentctlSubMeta m;
    bool ok = false;
    if (j->tool == CHAT_TOOL_CLAUDE || j->tool == CHAT_TOOL_QODER) ok = agentctl_submeta_claude(buf, n, &m);
    else if (j->tool == CHAT_TOOL_XAI) ok = agentctl_submeta_grok(buf, n, &m);
    else if (j->tool == CHAT_TOOL_KIMI) ok = agentctl_submeta_kimi(buf, n, &m);
    else if (j->tool == CHAT_TOOL_QWEN) {
        ok = agentctl_submeta_qwen(buf, n, &m);
        if (ok && m.status == AGSUB_FAILED) agentctl_submeta_qwen_error(buf, n, j->rd.error, sizeof j->rd.error);
    }
    free(buf);
    if (!ok) return;
    if (m.label[0] && !strcmp(j->label, "subagent")) {
        pthread_mutex_lock(&g_mu);
        snprintf(j->label, sizeof j->label, "%s", m.label);
        pthread_mutex_unlock(&g_mu);
    }
    if (m.status == AGSUB_RUNNING) {
        agentctl_subread_status(&j->rd, AGSUB_RUNNING, NULL, 0, lb_emit, lb);
    } else if (m.status == AGSUB_DONE || m.status == AGSUB_FAILED || m.status == AGSUB_STOPPED) {
        AgentctlSubStatus s = (AgentctlSubStatus)m.status;
        if (s == AGSUB_DONE && j->rd.error[0] && j->tool == CHAT_TOOL_CLAUDE) s = AGSUB_FAILED;
        agentctl_subread_status(&j->rd, s, j->rd.error[0] ? j->rd.error : NULL, m.end_ms, lb_emit, lb);
    }
}

/* ------------------------------------------------------------ OpenCode --- */

static sqlite3 *oc_db(i64 now) {
    if (g_oc_db) return g_oc_db;
    if (now < g_oc_retry_at) return NULL;
    g_oc_retry_at = now + 5000;
    char path[1100];
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = home_dir();
    int w;
    if (xdg && xdg[0] == '/') w = snprintf(path, sizeof path, "%s/opencode/opencode.db", xdg);
    else if (home)             w = snprintf(path, sizeof path, "%s/.local/share/opencode/opencode.db", home);
    else return NULL;
    if (w <= 0 || (usize)w >= sizeof path) return NULL;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    /* the library is built without automatic initialization; idempotent */
    if (sqlite3_initialize() != SQLITE_OK) return NULL;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    liu_sqlite_harden_foreign(db);
    sqlite3_busy_timeout(db, 50);
    g_oc_v2 = opencode_store_is_v2(db);
    if (!g_oc_v2) {                                  /* 1.x keeps no session_message rows */
        sqlite3_close(db);
        return NULL;
    }
    g_oc_db = db;
    return db;
}

static void oc_close(void) {
    if (g_oc_db) sqlite3_close(g_oc_db);
    g_oc_db = NULL;
}

static const char *col_text(sqlite3_stmt *s, int i) {
    const unsigned char *t = sqlite3_column_text(s, i);
    return t ? (const char *)t : "";
}

static void read_opencode(SubJob *j, i64 now, LineBuf *lb) {
    sqlite3 *db = oc_db(now);
    if (!db) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT seq, type, data FROM session_message WHERE session_id = ?1 AND seq > ?2 "
            "ORDER BY seq LIMIT ?3", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, j->oc_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, j->oc_seq);
    sqlite3_bind_int(s, 3, SUB_OC_ROWS);
    while (sqlite3_step(s) == SQLITE_ROW) {
        i64 seq = sqlite3_column_int64(s, 0);
        const char *type = col_text(s, 1);
        const char *data = (const char *)sqlite3_column_blob(s, 2);
        int n = sqlite3_column_bytes(s, 2);
        if (!agentctl_subread_opencode_row(&j->rd, type, data, n > 0 ? (usize)n : 0, lb_emit, lb)) break;
        j->oc_seq = seq;
        j->grew_at = now;
    }
    sqlite3_finalize(s);
    /* the session row says when it went idle and how */
    if (sqlite3_prepare_v2(db, "SELECT time_idle, idle_outcome FROM session_v2 WHERE id = ?1", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, j->oc_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL) {
        i64 idle = sqlite3_column_int64(s, 0);
        const char *o = col_text(s, 1);
        if (j->rd.status == AGSUB_RUNNING && idle > 0 && idle >= j->rd.last_ms) {
            AgentctlSubStatus st = !strcmp(o, "succeeded") ? AGSUB_DONE : !strcmp(o, "failed") ? AGSUB_FAILED
                                 : o[0] ? AGSUB_STOPPED : AGSUB_DONE;
            agentctl_subread_status(&j->rd, st, st == AGSUB_FAILED ? (j->rd.error[0] ? j->rd.error : "it failed") : NULL,
                                    idle, lb_emit, lb);
        }
    }
    sqlite3_finalize(s);
}

/* ------------------------------------------------------- looking for them --- */

typedef struct {
    char key[200];
    AgentctlSubMeta m;
    char path[1100];
    char meta[1100];
    char oc_id[128];
    char task_text[8192];
    bool has_task;
    bool settled;          /* already over when found (it is shown with its record) */
    u8   settled_as;       /* AgentctlSubStatus */
    char *answer;          /* what it answered then (malloc'd), NULL = nothing */
    usize answer_len;
    bool answer_pi;        /* `answer` is a Pi run's whole result */
    char why[256];         /* why it failed, when its record says */
    char agent[96];        /* Copilot CLI: its instance id */
    char *child;           /* its own lines read before it was shown, one a line (malloc'd) */
    usize child_len, child_cap;
    i64  born;             /* when it started, in ns as well as its record says: they are shown in that order */
    i32  seq;              /* the order it was found in, for two born together */
} Found;

typedef struct {
    Found *v;
    i32    n, cap;
} FoundList;

static Found *found_add(FoundList *fl) {
    if (fl->n == fl->cap) {
        i32 cap = fl->cap ? fl->cap * 2 : 8;
        Found *nv = realloc(fl->v, (usize)cap * sizeof *nv);
        if (!nv) return NULL;
        fl->v = nv;
        fl->cap = cap;
    }
    Found *f = &fl->v[fl->n++];
    memset(f, 0, sizeof *f);
    f->seq = fl->n;
    return f;
}

/* When a file came to be, in ns: its birth where the file system keeps one. */
static i64 born_ns(const char *path) {
    struct stat st;
    if (!path || !path[0] || stat(path, &st) != 0) return 0;
#ifdef __APPLE__
    return (i64)st.st_birthtimespec.tv_sec * 1000000000 + st.st_birthtimespec.tv_nsec;
#else
    return (i64)ST_MTIM(st).tv_sec * 1000000000 + ST_MTIM(st).tv_nsec;
#endif
}

static int found_order(const void *a, const void *b) {
    const Found *x = a, *y = b;
    if (x->born != y->born) return x->born < y->born ? -1 : 1;
    return x->seq - y->seq;
}

/* Whether the folder changed since the last look (and remembers it). */
static bool dir_changed(const char *dir, bool *ok, struct timespec *mt) {
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) { *ok = false; return false; }
    if (*ok && same_time(mt, &ST_MTIM(st))) return false;
    *ok = true;
    *mt = ST_MTIM(st);
    return true;
}

static i64 mtime_of(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return ts_ms(&ST_MTIM(st));
}

static void look_agent_files(SubPane *p, i64 now, FoundList *fl,
                             bool (*meta_of)(const char *, usize, AgentctlSubMeta *));

/* Claude Code: <session>/subagents/agent-<id>.jsonl and .meta.json. */
static void look_claude(SubPane *p, i64 now, FoundList *fl) {
    if (!p->sdir[0]) {
        const char *sp = p->wreg.session_path;
        if (sp[0] == '/' && ends_with(sp, ".jsonl")) {
            snprintf(p->sdir, sizeof p->sdir, "%.*s/subagents", (int)(strlen(sp) - 6), sp);
        } else if (p->wreg.session_id[0] && p->wreg.cwd[0] == '/') {
            /* the project folder is the session's folder spelled with dashes */
            const char *cfg = getenv("CLAUDE_CONFIG_DIR");
            const char *home = home_dir();
            char base[1024], slug[1024];
            if (cfg && cfg[0] == '/') snprintf(base, sizeof base, "%s/projects", cfg);
            else if (home) snprintf(base, sizeof base, "%s/.claude/projects", home);
            else return;
            usize k = 0;
            for (const char *c = p->wreg.cwd; *c && k + 1 < sizeof slug; c++)
                slug[k++] = ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) ? *c : '-';
            slug[k] = '\0';
            snprintf(p->sdir, sizeof p->sdir, "%s/%s/%s/subagents", base, slug, p->wreg.session_id);
        } else {
            return;
        }
    }
    look_agent_files(p, now, fl, agentctl_submeta_claude);
}

/* "rollout-2026-10-29T22-19-46-<uuid>.jsonl": when it was started, local time. */
static i64 rollout_name_ms(const char *nm) {
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    if (strncmp(nm, "rollout-", 8) || strlen(nm) < 27) return 0;
    if (sscanf(nm + 8, "%4d-%2d-%2dT%2d-%2d-%2d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday, &tmv.tm_hour,
               &tmv.tm_min, &tmv.tm_sec) != 6)
        return 0;
    tmv.tm_year -= 1900;
    tmv.tm_mon -= 1;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    return t > 0 ? (i64)t * 1000 : 0;
}

static bool codex_thread_of(const char *sid, char *out, usize cap) {
    usize n = sid ? strlen(sid) : 0;
    if (n < 36 || cap < 37) return false;
    const char *u = sid + (n - 36);
    for (i32 i = 0; i < 36; i++) {
        char c = u[i];
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    memcpy(out, u, 36);
    out[36] = '\0';
    return true;
}

/* Codex: a rollout in today's or yesterday's folder whose header names a
 * thread of this pane — its own, or a subagent's (one spawned by a subagent). */
static void look_codex(SubPane *p, i64 now, FoundList *fl) {
    if (!p->thread[0]) {
        const char *sid = p->wreg.session_id[0] ? p->wreg.session_id : p->wreg.session_path;
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", sid);
        if (ends_with(tmp, ".jsonl")) tmp[strlen(tmp) - 6] = '\0';
        if (!codex_thread_of(tmp, p->thread, sizeof p->thread)) return;
    }
    char base[1024];
    const char *ch = getenv("CODEX_HOME");
    const char *home = home_dir();
    if (ch && ch[0] == '/') snprintf(base, sizeof base, "%s/sessions", ch);
    else if (home) snprintf(base, sizeof base, "%s/.codex/sessions", home);
    else return;
    /* the threads that are this pane's: its own and its subagents' */
    char threads[AGENTCTL_MAX_WATCHED + 1][128];
    i32 nt = 0;
    snprintf(threads[nt++], sizeof threads[0], "%s", p->thread);
    pthread_mutex_lock(&g_mu);
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED && nt <= AGENTCTL_MAX_WATCHED; i++) {
        const SubJob *j = &g_jobs[i];
        if (j->id && j->pane == p->wreg.pane && j->tool == CHAT_TOOL_CODEX) snprintf(threads[nt++], sizeof threads[0], "%s", j->key);
    }
    pthread_mutex_unlock(&g_mu);
    typedef struct { char key[200]; char path[1100]; AgentctlSubMeta m; bool open; } Cand;
    Cand *cand = NULL;
    i32 nc = 0, cc = 0;
    time_t tnow = (time_t)(now / 1000);
    for (i32 day = 0; day < 2; day++) {
        time_t tt = tnow - (time_t)day * 86400;
        struct tm tmv;
        if (!localtime_r(&tt, &tmv)) continue;
        char dir[1100];
        snprintf(dir, sizeof dir, "%s/%04d/%02d/%02d", base, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        if (strcmp(dir, p->day_dir[day])) {
            snprintf(p->day_dir[day], sizeof p->day_dir[day], "%s", dir);
            p->day_ok[day] = false;
        }
        if (!dir_changed(dir, &p->day_ok[day], &p->day_mt[day])) continue;
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *nm = de->d_name;
            if (strncmp(nm, "rollout-", 8) || !ends_with(nm, ".jsonl")) continue;
            if (seen_has(&p->seen, nm)) continue;
            i64 born = rollout_name_ms(nm);
            if (born && born < p->wreg.since_ms - AGENTCTL_SUB_BASELINE_S * 1000) { seen_add(&p->seen, nm); continue; }
            char path[1100];
            if (!path_join(path, sizeof path, dir, nm)) continue;
            usize n = 0;
            char *head = first_line(path, SUB_FIRSTLINE_MAX, &n);
            if (!head) { p->day_ok[day] = false; continue; }   /* not written whole yet: next time */
            AgentctlSubMeta m;
            bool sub = agentctl_submeta_codex(head, n, &m);
            free(head);
            if (!sub || !strcmp(m.id, p->thread)) { seen_add(&p->seen, nm); continue; }
            if (nc == cc) {
                cc = cc ? cc * 2 : 16;
                Cand *nv = realloc(cand, (usize)cc * sizeof *nv);
                if (!nv) break;
                cand = nv;
            }
            Cand *c = &cand[nc++];
            snprintf(c->key, sizeof c->key, "%s", nm);
            snprintf(c->path, sizeof c->path, "%s", path);
            c->m = m;
            c->open = true;
        }
        closedir(d);
    }
    /* a subagent's own subagent may come before it in a listing */
    for (bool more = true; more;) {
        more = false;
        for (i32 i = 0; i < nc; i++) {
            if (!cand[i].open) continue;
            bool ours = false;
            for (i32 t = 0; t < nt && !ours; t++) ours = !strcmp(cand[i].m.parent, threads[t]);
            if (!ours) continue;
            cand[i].open = false;
            more = true;
            seen_add(&p->seen, cand[i].key);
            if (nt <= AGENTCTL_MAX_WATCHED) snprintf(threads[nt++], sizeof threads[0], "%s", cand[i].m.id);
            if (!p->baseline && mtime_of(cand[i].path) < now - AGENTCTL_SUB_BASELINE_S * 1000) continue;
            Found *f = found_add(fl);
            if (!f) break;
            /* the job is keyed by its thread, which its own subagents name */
            snprintf(f->key, sizeof f->key, "%s", cand[i].m.id);
            f->m = cand[i].m;
            snprintf(f->path, sizeof f->path, "%s", cand[i].path);
            f->born = born_ns(cand[i].path);
        }
    }
    /* the rest are other threads' */
    for (i32 i = 0; i < nc; i++) if (cand[i].open) seen_add(&p->seen, cand[i].key);
    free(cand);
}

/* OpenCode 2.x: the child sessions of the pane's session, and theirs. */
static void look_opencode(SubPane *p, i64 now, FoundList *fl) {
    if (!p->wreg.session_id[0]) return;
    sqlite3 *db = oc_db(now);
    if (!db) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "WITH RECURSIVE kids(id, depth) AS ("
            "  SELECT id, 1 FROM session_v2 WHERE parent_id = ?1"
            "  UNION ALL SELECT c.id, k.depth + 1 FROM session_v2 c JOIN kids k ON c.parent_id = k.id"
            "  WHERE k.depth < 8) "
            "SELECT s.id, s.title, s.agent, s.time_created, s.time_updated, s.time_idle, s.idle_outcome, k.depth "
            "FROM session_v2 s JOIN kids k ON s.id = k.id WHERE s.time_created >= ?2 ORDER BY s.time_created",
            -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, p->wreg.session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, p->wreg.since_ms - AGENTCTL_SUB_BASELINE_S * 1000);
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *id = col_text(s, 0);
        if (!id[0] || seen_has(&p->seen, id)) continue;
        seen_add(&p->seen, id);
        i64 updated = sqlite3_column_int64(s, 4);
        bool idle = sqlite3_column_type(s, 5) != SQLITE_NULL;
        if (!p->baseline && (idle || updated < now - AGENTCTL_SUB_BASELINE_S * 1000)) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", id);
        snprintf(f->oc_id, sizeof f->oc_id, "%s", id);
        agentctl_sanitize(col_text(s, 1), strlen(col_text(s, 1)), f->m.label, sizeof f->m.label, false);
        agentctl_sanitize(col_text(s, 2), strlen(col_text(s, 2)), f->m.kind, sizeof f->m.kind, false);
        f->m.start_ms = sqlite3_column_int64(s, 3);
        f->m.depth = sqlite3_column_int(s, 7);
        f->born = f->m.start_ms * 1000000;
    }
    sqlite3_finalize(s);
}

/* Grok and Kimi: <session folder>/subagents/<id>/meta.json. Grok keeps the
 * child's transcript as a session folder of its own beside the parent's; Kimi
 * keeps it in the subagent's folder. */
static void look_meta_dirs(SubPane *p, i64 now, FoundList *fl) {
    bool grok = p->wreg.tool == CHAT_TOOL_XAI;
    if (!p->sdir[0]) {
        if (p->wreg.session_path[0] != '/') return;
        char sd[1100];
        dir_of(p->wreg.session_path, sd, sizeof sd);
        if (!sd[0] || !path_join(p->sdir, sizeof p->sdir, sd, "subagents")) return;
    }
    if (!dir_changed(p->sdir, &p->root_ok, &p->root_mt) && p->baseline) return;
    DIR *d = opendir(p->sdir);
    if (!d) return;
    char session_dir[1100], bucket[1100];
    dir_of(p->sdir, session_dir, sizeof session_dir);
    dir_of(session_dir, bucket, sizeof bucket);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' || seen_has(&p->seen, nm)) continue;
        char sub[1100], mpath[1100];
        if (!path_join(sub, sizeof sub, p->sdir, nm) || !path_join(mpath, sizeof mpath, sub, "meta.json")) continue;
        usize n = 0;
        struct timespec mt = {0};
        char *mb = slurp(mpath, SUB_META_MAX, &n, &mt);
        AgentctlSubMeta m;
        bool ok = mb && (grok ? agentctl_submeta_grok(mb, n, &m) : agentctl_submeta_kimi(mb, n, &m));
        free(mb);
        if (!ok) { p->root_ok = false; continue; }           /* written in a moment: listed again */
        seen_add(&p->seen, nm);
        bool over = m.status == AGSUB_DONE || m.status == AGSUB_FAILED || m.status == AGSUB_STOPPED;
        if (!p->baseline && (over || ts_ms(&mt) < now - AGENTCTL_SUB_BASELINE_S * 1000)) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", nm);
        f->m = m;
        snprintf(f->meta, sizeof f->meta, "%s", mpath);
        if (grok) {
            char child[1100];
            if (path_join(child, sizeof child, bucket, m.id)) path_join(f->path, sizeof f->path, child, "chat_history.jsonl");
        } else {
            path_join(f->path, sizeof f->path, sub, "context.jsonl");
        }
        f->born = m.start_ms > 0 ? m.start_ms * 1000000 : born_ns(mpath);
    }
    closedir(d);
}

/* Oh My Pi: <session file without .jsonl>/<Name>.jsonl, whose session line
 * names the parent file. */
static void look_omp(SubPane *p, i64 now, FoundList *fl) {
    const char *sp = p->wreg.session_path;
    if (!p->sdir[0]) {
        if (sp[0] != '/' || !ends_with(sp, ".jsonl")) return;
        snprintf(p->sdir, sizeof p->sdir, "%.*s", (int)(strlen(sp) - 6), sp);
    }
    if (!dir_changed(p->sdir, &p->root_ok, &p->root_mt) && p->baseline) return;
    DIR *d = opendir(p->sdir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' || !ends_with(nm, ".jsonl") || seen_has(&p->seen, nm)) continue;
        char path[1100];
        if (!path_join(path, sizeof path, p->sdir, nm)) continue;
        /* its second line is the session line */
        usize n = 0;
        char *head = slurp(path, 64u * 1024u, &n, NULL);
        if (!head) continue;
        char *l1 = memchr(head, '\n', n);
        char *l2 = l1 ? memchr(l1 + 1, '\n', n - (usize)(l1 + 1 - head)) : NULL;
        if (!l2) { free(head); p->root_ok = false; continue; }   /* not written yet */
        AgentctlSubMeta m;
        bool ok = agentctl_submeta_omp(l1 + 1, (usize)(l2 - (l1 + 1)), &m);
        free(head);
        seen_add(&p->seen, nm);
        if (!ok || strcmp(m.parent, sp)) continue;
        if (!p->baseline && mtime_of(path) < now - AGENTCTL_SUB_BASELINE_S * 1000) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", nm);
        snprintf(m.label, sizeof m.label, "%.*s", (int)(strlen(nm) - 6), nm);
        f->m = m;
        snprintf(f->path, sizeof f->path, "%s", path);
        f->born = born_ns(path);
    }
    closedir(d);
}

/* Command Code: the parent's transcript, read as it grows. */
typedef struct {
    SubPane  *p;
    FoundList *fl;
    i64       now;
    bool      baseline;
} CcCtx;

static SubJob *cc_job_by(u64 pane, i32 tool, const char *call, const char *bg) {
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) {
        SubJob *j = &g_jobs[i];
        if (!j->id || j->pane != pane || j->tool != tool) continue;
        if (call && !strcmp(j->cc_call, call)) return j;
        if (bg && j->cc_bg[0] && !strcmp(j->cc_bg, bg)) return j;
    }
    return NULL;
}

static SubJob *cp_job_by_agent(u64 pane, const char *agent) {
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) {
        SubJob *j = &g_jobs[i];
        if (j->id && j->pane == pane && j->tool == CHAT_TOOL_COPILOT && j->cp_agent[0] && !strcmp(j->cp_agent, agent))
            return j;
    }
    return NULL;
}

static Found *cp_found_by_agent(FoundList *fl, const char *agent) {
    for (i32 i = 0; i < fl->n; i++)
        if (fl->v[i].agent[0] && !strcmp(fl->v[i].agent, agent)) return &fl->v[i];
    return NULL;
}

/* A line of a subagent's own, kept until it is shown. */
static void found_child(Found *f, const char *line, usize len) {
    if (f->child_len + len + 1 > f->child_cap) {
        usize cap = f->child_cap ? f->child_cap : 4096;
        while (cap < f->child_len + len + 1) cap *= 2;
        if (cap > 4u * 1024u * 1024u) return;
        char *nb = realloc(f->child, cap);
        if (!nb) return;
        f->child = nb;
        f->child_cap = cap;
    }
    memcpy(f->child + f->child_len, line, len);
    f->child_len += len;
    f->child[f->child_len++] = '\n';
}

static void found_answer(Found *f, const char *t, usize n) {
    free(f->answer);
    f->answer = NULL;
    f->answer_len = 0;
    if (t && n && (f->answer = malloc(n)) != NULL) {
        memcpy(f->answer, t, n);
        f->answer_len = n;
    }
}

static Found *cc_found_by(FoundList *fl, const char *call, const char *bg) {
    for (i32 i = 0; i < fl->n; i++) {
        Found *f = &fl->v[i];
        if (call && !strcmp(f->key, call)) return f;
        if (bg && f->oc_id[0] && !strcmp(f->oc_id, bg)) return f;
    }
    return NULL;
}

static void cc_settle(SubJob *j, u8 status, const char *text, usize len, i64 ts) {
    if (text && len) agentctl_subread_answer(&j->rd, text, len, lb_emit, &j->pend);
    agentctl_subread_status(&j->rd, (AgentctlSubStatus)status, status == AGSUB_FAILED ? "it failed" : NULL, ts,
                            lb_emit, &j->pend);
}

/* Copilot CLI: one of a subagent's own events, to its job (or the one found
 * in this same read). */
static void cp_child(CcCtx *c, const AgentctlCcEvent *e) {
    u64 pane = c->p->wreg.pane;
    Found *f = e->call_id ? cc_found_by(c->fl, e->call_id, NULL) : NULL;
    if (!f && e->agent_id) f = cp_found_by_agent(c->fl, e->agent_id);
    if (f) {
        if (e->agent_id && !f->agent[0]) snprintf(f->agent, sizeof f->agent, "%s", e->agent_id);
        found_child(f, e->line, e->line_len);
        return;
    }
    pthread_mutex_lock(&g_mu);
    SubJob *j = e->call_id ? cc_job_by(pane, CHAT_TOOL_COPILOT, e->call_id, NULL) : NULL;
    if (!j && e->agent_id) j = cp_job_by_agent(pane, e->agent_id);
    pthread_mutex_unlock(&g_mu);
    if (!j) return;
    if (e->agent_id && !j->cp_agent[0]) snprintf(j->cp_agent, sizeof j->cp_agent, "%s", e->agent_id);
    agentctl_subread_line(&j->rd, e->line, e->line_len, lb_emit, &j->pend);
}

static void cc_event(void *ctx, const AgentctlCcEvent *e) {
    CcCtx *c = ctx;
    i32 tool = c->p->wreg.tool;
    if (e->kind == AGCC_CHILD) { cp_child(c, e); return; }
    if (e->kind == AGCC_SPAWN) {
        if (seen_has(&c->p->seen, e->call_id)) {
            /* the same subagent told of again (Copilot CLI: its call, then its start) */
            Found *f = cc_found_by(c->fl, e->call_id, NULL);
            if (f && !f->m.kind[0] && e->agent_type)
                agentctl_sanitize(e->agent_type, strlen(e->agent_type), f->m.kind, sizeof f->m.kind, false);
            if (f && !f->m.model[0] && e->model)
                agentctl_sanitize(e->model, strlen(e->model), f->m.model, sizeof f->m.model, false);
            return;
        }
        seen_add(&c->p->seen, e->call_id);
        Found *f = found_add(c->fl);
        if (!f) return;
        snprintf(f->key, sizeof f->key, "%s", e->call_id);
        if (e->label) agentctl_sanitize(e->label, strlen(e->label), f->m.label, sizeof f->m.label, false);
        if (e->agent_type) agentctl_sanitize(e->agent_type, strlen(e->agent_type), f->m.kind, sizeof f->m.kind, false);
        if (e->model) agentctl_sanitize(e->model, strlen(e->model), f->m.model, sizeof f->m.model, false);
        f->m.start_ms = e->ts_ms;
        if (e->text && e->text_len) {
            usize n = e->text_len < sizeof f->task_text - 1 ? e->text_len : sizeof f->task_text - 1;
            memcpy(f->task_text, e->text, n);
            f->task_text[n] = '\0';
            f->has_task = true;
        }
        snprintf(f->meta, sizeof f->meta, "%s", e->background ? "bg" : "");
        return;
    }
    if (e->kind == AGCC_SETTLE) {
        if (!e->call_id) return;
        Found *f = cc_found_by(c->fl, e->call_id, NULL);
        if (f) {
            if (c->baseline) { f->key[0] = '\0'; return; }
            f->settled = true;
            f->settled_as = e->status;
            if (e->text && e->text_len) agentctl_sanitize(e->text, e->text_len, f->why, sizeof f->why, false);
            return;
        }
        pthread_mutex_lock(&g_mu);
        SubJob *j = cc_job_by(c->p->wreg.pane, tool, e->call_id, NULL);
        pthread_mutex_unlock(&g_mu);
        if (j) agentctl_subread_status(&j->rd, (AgentctlSubStatus)e->status,
                                       e->status == AGSUB_FAILED ? (e->text && e->text[0] ? e->text : "it failed") : NULL,
                                       e->ts_ms, lb_emit, &j->pend);
        return;
    }
    if (e->kind != AGCC_RESULT || !e->call_id) return;
    if (tool != CHAT_TOOL_COMMANDCODE) {
        /* Pi carries the run whole; Devin its answer; Copilot CLI only ends one
         * that its own events did not */
        Found *f = cc_found_by(c->fl, e->call_id, NULL);
        if (f) {
            if (c->baseline) { f->key[0] = '\0'; return; }
            if (f->settled) return;
            f->settled = true;
            f->settled_as = e->failed ? AGSUB_FAILED : AGSUB_DONE;
            if (tool == CHAT_TOOL_PI && e->line) { found_answer(f, e->line, e->line_len); f->answer_pi = true; }
            else if (tool == CHAT_TOOL_DEVIN) found_answer(f, e->text, e->text_len);
            return;
        }
        pthread_mutex_lock(&g_mu);
        SubJob *j = cc_job_by(c->p->wreg.pane, tool, e->call_id, NULL);
        pthread_mutex_unlock(&g_mu);
        if (!j) return;
        if (tool == CHAT_TOOL_PI && e->line) agentctl_subread_pi_result(&j->rd, e->line, e->line_len, e->ts_ms, lb_emit, &j->pend);
        else if (tool == CHAT_TOOL_DEVIN) cc_settle(j, e->failed ? AGSUB_FAILED : AGSUB_DONE, e->text, e->text_len, e->ts_ms);
        else if (j->rd.status == AGSUB_RUNNING)
            agentctl_subread_status(&j->rd, e->failed ? AGSUB_FAILED : AGSUB_DONE, e->failed ? "it failed" : NULL,
                                    e->ts_ms, lb_emit, &j->pend);
        else if (tool == CHAT_TOOL_PI && e->text) cc_settle(j, AGSUB_FAILED, e->text, e->text_len, e->ts_ms);
        return;
    }
    char bg[64];
    u8 st = AGSUB_UNKNOWN;
    const char *report = NULL;
    usize report_len = 0;
    bool says_bg = agentctl_subread_cc_background(e->text, e->text_len, bg, sizeof bg, &st, &report, &report_len);
    /* a call found in this same read: settled before it was ever shown */
    Found *f = cc_found_by(c->fl, e->call_id, says_bg ? bg : NULL);
    if (f) {
        if (says_bg && st == AGSUB_RUNNING) { snprintf(f->oc_id, sizeof f->oc_id, "%s", bg); return; }
        if (!says_bg && !strcmp(f->meta, "bg")) return;      /* the launch's own word */
        if (c->baseline) { f->key[0] = '\0'; return; }     /* over before Liu looked: not shown */
        f->settled = true;
        f->settled_as = says_bg ? st : e->failed ? AGSUB_FAILED : AGSUB_DONE;
        const char *t = says_bg ? report : e->text;
        usize n = says_bg ? report_len : e->text_len;
        free(f->answer);
        f->answer = NULL;
        f->answer_len = 0;
        if (t && n && (f->answer = malloc(n)) != NULL) {
            memcpy(f->answer, t, n);
            f->answer_len = n;
        }
        return;
    }
    pthread_mutex_lock(&g_mu);
    SubJob *j = cc_job_by(c->p->wreg.pane, tool, e->call_id, NULL);
    if (!j && says_bg) j = cc_job_by(c->p->wreg.pane, tool, NULL, bg);
    pthread_mutex_unlock(&g_mu);
    if (!j) return;
    if (says_bg && st == AGSUB_RUNNING) {
        snprintf(j->cc_bg, sizeof j->cc_bg, "%s", bg);
        return;
    }
    if (says_bg) {
        if (st != AGSUB_RUNNING) cc_settle(j, st, report, report_len, e->ts_ms);
        return;
    }
    if (!strcmp(j->cc_call, e->call_id) && !j->cc_background)
        cc_settle(j, e->failed ? AGSUB_FAILED : AGSUB_DONE, e->text, e->text_len, e->ts_ms);
}

/* The parent's log read as it grows, each line given to `router` (Command
 * Code, Pi, Copilot CLI). */
static void look_parent_log(SubPane *p, i64 now, FoundList *fl, const char *sp,
                            void (*router)(const char *, usize, AgentctlCcFn, void *)) {
    if (!sp || sp[0] != '/') return;
    struct stat st;
    int fd = open_regular(sp, &st);
    if (fd < 0) return;
    if (!p->cc_open || p->cc_dev != st.st_dev || p->cc_ino != st.st_ino || (u64)st.st_size < p->cc_off) {
        p->cc_open = true;
        p->cc_dev = st.st_dev;
        p->cc_ino = st.st_ino;
        p->cc_part_len = 0;
        /* a session Liu has just seen: its last megabyte, for calls still running */
        p->cc_off = (u64)st.st_size > 1024u * 1024u ? (u64)st.st_size - 1024u * 1024u : 0;
        if (p->cc_off) {
            char ch;
            while (p->cc_off < (u64)st.st_size && pread(fd, &ch, 1, (off_t)(p->cc_off - 1)) == 1 && ch != '\n') p->cc_off++;
        }
    }
    CcCtx c = { p, fl, now, !p->baseline };
    char buf[64 * 1024];
    u64 end = (u64)st.st_size;
    u64 budget = SUB_READ_MAX * 4;
    while (p->cc_off < end && budget > 0) {
        usize want = end - p->cc_off < sizeof buf ? (usize)(end - p->cc_off) : sizeof buf;
        ssize_t r = pread(fd, buf, want, (off_t)p->cc_off);
        if (r <= 0) break;
        p->cc_off += (u64)r;
        budget = budget > (u64)r ? budget - (u64)r : 0;
        usize i = 0;
        while (i < (usize)r) {
            const char *nl = memchr(buf + i, '\n', (usize)r - i);
            usize len = nl ? (usize)(nl - (buf + i)) : (usize)r - i;
            if (p->cc_part_len + len > AGENTCTL_STREAM_LINE_MAX) {
                p->cc_part_len = 0;
            } else {
                if (p->cc_part_len + len > p->cc_part_cap) {
                    usize cap = p->cc_part_cap ? p->cc_part_cap : 8192;
                    while (cap < p->cc_part_len + len) cap *= 2;
                    char *np = realloc(p->cc_part, cap);
                    if (!np) { close(fd); return; }
                    p->cc_part = np;
                    p->cc_part_cap = cap;
                }
                memcpy(p->cc_part + p->cc_part_len, buf + i, len);
                p->cc_part_len += len;
            }
            if (!nl) break;
            router(p->cc_part, p->cc_part_len, cc_event, &c);
            p->cc_part_len = 0;
            i += len + 1;
        }
    }
    close(fd);
}

/* ------------------------------------------------- more agents' stores --- */

/* A number of seconds or of milliseconds, as ms. */
static i64 any_ms(i64 v) {
    if (v <= 0) return 0;
    return v < 100000000000LL ? v * 1000 : v;
}

/* The pane's database, opened read-only; NULL while it is not there (asked
 * again 5 s later). */
static sqlite3 *pane_db(SubPane *p, const char *path, i64 now) {
    if (!path || !path[0]) return NULL;
    if (p->db && !strcmp(p->db_path, path)) return p->db;
    if (p->db) { sqlite3_close(p->db); p->db = NULL; p->db_path[0] = '\0'; }
    if (now < p->db_retry) return NULL;
    p->db_retry = now + 5000;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    if (sqlite3_initialize() != SQLITE_OK) return NULL;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    liu_sqlite_harden_foreign(db);
    sqlite3_busy_timeout(db, 50);
    p->db = db;
    snprintf(p->db_path, sizeof p->db_path, "%s", path);
    return db;
}

static void pane_db_close(SubPane *p) {
    if (p->db) sqlite3_close(p->db);
    p->db = NULL;
    p->db_path[0] = '\0';
    p->db_retry = 0;
}

/* Claude Code and Qwen Code: agent-<id>.jsonl beside agent-<id>.meta.json in
 * the pane's subagent folder (p->sdir). */
static void look_agent_files(SubPane *p, i64 now, FoundList *fl,
                             bool (*meta_of)(const char *, usize, AgentctlSubMeta *)) {
    if (!dir_changed(p->sdir, &p->root_ok, &p->root_mt) && p->baseline) return;
    DIR *d = opendir(p->sdir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (strncmp(nm, "agent-", 6) || !ends_with(nm, ".jsonl")) continue;
        char id[160];
        usize il = strlen(nm) - 6 - 6;
        if (il == 0 || il >= sizeof id) continue;
        memcpy(id, nm + 6, il);
        id[il] = '\0';
        if (seen_has(&p->seen, id)) continue;
        char tpath[1100], mpath[1100], mname[200];
        snprintf(mname, sizeof mname, "agent-%s.meta.json", id);
        if (!path_join(tpath, sizeof tpath, p->sdir, nm) || !path_join(mpath, sizeof mpath, p->sdir, mname)) continue;
        AgentctlSubMeta m;
        memset(&m, 0, sizeof m);
        usize n = 0;
        char *mb = slurp(mpath, SUB_META_MAX, &n, NULL);
        bool have_meta = mb && meta_of(mb, n, &m);
        free(mb);
        i64 tm = mtime_of(tpath);
        if (!have_meta && tm > now - 5000) { p->root_ok = false; continue; }   /* its meta file comes in a moment */
        seen_add(&p->seen, id);
        bool over = m.status == AGSUB_DONE || m.status == AGSUB_FAILED || m.status == AGSUB_STOPPED;
        if (!p->baseline && (over || tm < now - AGENTCTL_SUB_BASELINE_S * 1000)) continue;
        if (tm && tm < p->wreg.since_ms - AGENTCTL_SUB_BASELINE_S * 1000) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", id);
        f->m = m;
        snprintf(f->path, sizeof f->path, "%s", tpath);
        snprintf(f->meta, sizeof f->meta, "%s", mpath);
        f->born = born_ns(tpath);
    }
    closedir(d);
}

/* Qwen Code: <project>/subagents/<session>/, the session's own file being
 * <project>/chats/<session>.jsonl. */
static void look_qwen(SubPane *p, i64 now, FoundList *fl) {
    if (!p->sdir[0]) {
        const char *sp = p->wreg.session_path;
        if (sp[0] != '/' || !p->wreg.session_id[0]) return;
        char chats[1100], proj[1100], sid[256];
        dir_of(sp, chats, sizeof chats);
        dir_of(chats, proj, sizeof proj);
        usize k = 0;
        for (const char *c = p->wreg.session_id; *c && k + 1 < sizeof sid; c++)
            sid[k++] = ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                        *c == '_' || *c == '-') ? *c : '_';
        sid[k] = '\0';
        if (!proj[0] || snprintf(p->sdir, sizeof p->sdir, "%s/subagents/%s", proj, sid) >= (int)sizeof p->sdir)
            return;
    }
    look_agent_files(p, now, fl, agentctl_submeta_qwen);
}

/* Droid: ~/.factory/sessions/-<the folder, its slashes dashes>/<id>.jsonl, a
 * subagent's first line naming the calling session. The pane's own session is
 * the newest one the user started there since its agent took the pane. */
static void look_droid(SubPane *p, i64 now, FoundList *fl) {
    if (!p->sdir[0]) {
        const char *home = home_dir();
        if (!home || p->wreg.cwd[0] != '/') return;
        char real[1024], key[1024];
        if (!realpath(p->wreg.cwd, real)) snprintf(real, sizeof real, "%s", p->wreg.cwd);
        usize k = 0;
        const char *c = real;
        while (*c == '/') c++;
        while (*c && k + 1 < sizeof key) {
            if (*c == '/') {
                while (*c == '/') c++;
                if (*c) key[k++] = '-';
                continue;
            }
            key[k++] = *c++;
        }
        key[k] = '\0';
        if (snprintf(p->sdir, sizeof p->sdir, "%s/.factory/sessions/-%s", home, key) >= (int)sizeof p->sdir) {
            p->sdir[0] = '\0';
            return;
        }
    }
    if (!dir_changed(p->sdir, &p->root_ok, &p->root_mt) && p->baseline) return;
    DIR *d = opendir(p->sdir);
    if (!d) return;
    typedef struct { char name[200]; i64 mt; AgentctlSubMeta m; } Cand;
    Cand *c = NULL;
    i32 nc = 0, capc = 0;
    i64 since = p->wreg.since_ms - 10000;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' || !ends_with(nm, ".jsonl") || strlen(nm) >= sizeof c->name) continue;
        char path[1100];
        if (!path_join(path, sizeof path, p->sdir, nm)) continue;
        i64 mt = mtime_of(path);
        if (mt < since) continue;
        usize n = 0;
        char *l1 = first_line(path, 64u * 1024u, &n);
        AgentctlSubMeta m;
        bool ok = l1 && agentctl_submeta_droid(l1, n, &m);
        free(l1);
        if (!ok) { p->root_ok = false; continue; }      /* not written yet */
        if (nc == capc) {
            capc = capc ? capc * 2 : 16;
            Cand *nv = realloc(c, (usize)capc * sizeof *nv);
            if (!nv) break;
            c = nv;
        }
        snprintf(c[nc].name, sizeof c[nc].name, "%s", nm);
        c[nc].mt = mt;
        c[nc].m = m;
        nc++;
    }
    closedir(d);
    i64 best = 0;
    for (i32 i = 0; i < nc; i++)
        if (!c[i].m.parent[0] && c[i].mt > best) {
            best = c[i].mt;
            snprintf(p->own_id, sizeof p->own_id, "%s", c[i].m.id);
        }
    for (i32 i = 0; i < nc && p->own_id[0]; i++) {
        if (strcmp(c[i].m.parent, p->own_id) || seen_has(&p->seen, c[i].name)) continue;
        seen_add(&p->seen, c[i].name);
        if (!p->baseline && c[i].mt < now - AGENTCTL_SUB_BASELINE_S * 1000) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", c[i].name);
        f->m = c[i].m;
        f->m.parent[0] = '\0';
        if (!strcmp(f->m.label, "New Session")) f->m.label[0] = '\0';
        path_join(f->path, sizeof f->path, p->sdir, c[i].name);
        f->born = born_ns(f->path);
    }
    free(c);
}

/* Cline CLI: <session folder>/<agent id>.messages.json beside the root's own
 * <session>.messages.json. */
static void look_cline(SubPane *p, i64 now, FoundList *fl) {
    if (!p->sdir[0]) {
        const char *sp = p->wreg.session_path;
        if (sp[0] != '/') return;
        dir_of(sp, p->sdir, sizeof p->sdir);
        if (!p->sdir[0]) return;
    }
    if (!dir_changed(p->sdir, &p->root_ok, &p->root_mt) && p->baseline) return;
    DIR *d = opendir(p->sdir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' || !ends_with(nm, ".messages.json")) continue;
        char stem[200];
        usize sl = strlen(nm) - 14;
        if (sl == 0 || sl >= sizeof stem) continue;
        memcpy(stem, nm, sl);
        stem[sl] = '\0';
        if (!strcmp(stem, p->wreg.session_id) || seen_has(&p->seen, stem)) continue;
        char path[1100];
        if (!path_join(path, sizeof path, p->sdir, nm)) continue;
        seen_add(&p->seen, stem);
        i64 mt = mtime_of(path);
        if (!p->baseline && mt < now - AGENTCTL_SUB_BASELINE_S * 1000) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", stem);
        snprintf(f->path, sizeof f->path, "%s", path);
        f->born = born_ns(path);
    }
    closedir(d);
}

/* Crush: <folder>/.crush/crush.db, a subagent a `sessions` row under the
 * parent's. The pane's own session is the newest one at the top since its
 * agent took the pane. */
static void look_crush(SubPane *p, i64 now, FoundList *fl) {
    char path[1100];
    if (p->wreg.cwd[0] != '/' || snprintf(path, sizeof path, "%s/.crush/crush.db", p->wreg.cwd) >= (int)sizeof path)
        return;
    sqlite3 *db = pane_db(p, path, now);
    if (!db) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM sessions WHERE parent_session_id IS NULL AND updated_at >= ?1 "
                               "ORDER BY updated_at DESC LIMIT 1", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(s, 1, p->wreg.since_ms / 1000 - 10);
    if (sqlite3_step(s) == SQLITE_ROW) snprintf(p->own_id, sizeof p->own_id, "%s", col_text(s, 0));
    sqlite3_finalize(s);
    if (!p->own_id[0]) return;
    if (sqlite3_prepare_v2(db,
            "WITH RECURSIVE kids(id, d) AS (SELECT id, 1 FROM sessions WHERE parent_session_id = ?1 "
            "UNION ALL SELECT s.id, k.d + 1 FROM sessions s JOIN kids k ON s.parent_session_id = k.id WHERE k.d < 8) "
            "SELECT s.id, s.created_at, s.updated_at, k.d FROM kids k JOIN sessions s ON s.id = k.id "
            "ORDER BY s.created_at, s.rowid", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, p->own_id, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *id = col_text(s, 0);
        if (!id[0] || strlen(id) >= sizeof ((SubJob *)0)->oc_id || seen_has(&p->seen, id)) continue;
        seen_add(&p->seen, id);
        i64 born = any_ms(sqlite3_column_int64(s, 1)), upd = any_ms(sqlite3_column_int64(s, 2));
        if (!p->baseline && upd < now - AGENTCTL_SUB_BASELINE_S * 1000) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", id);
        snprintf(f->oc_id, sizeof f->oc_id, "%s", id);
        f->m.start_ms = born;
        f->m.depth = sqlite3_column_int(s, 3);
        f->born = born * 1000000;
    }
    sqlite3_finalize(s);
}

/* Hermes: state.db, a subagent a `sessions` row of source "subagent" under
 * the parent's. */
static void look_hermes(SubPane *p, i64 now, FoundList *fl) {
    sqlite3 *db = pane_db(p, p->wreg.session_path, now);
    if (!db || !p->wreg.session_id[0]) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "WITH RECURSIVE kids(id, d) AS (SELECT id, 1 FROM sessions WHERE parent_session_id = ?1 AND source = 'subagent' "
            "UNION ALL SELECT s.id, k.d + 1 FROM sessions s JOIN kids k ON s.parent_session_id = k.id "
            "WHERE s.source = 'subagent' AND k.d < 8) "
            "SELECT s.id, s.title, s.started_at, s.ended_at, s.model, k.d FROM kids k JOIN sessions s ON s.id = k.id "
            "ORDER BY s.started_at", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, p->wreg.session_id, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *id = col_text(s, 0);
        if (!id[0] || strlen(id) >= sizeof ((SubJob *)0)->oc_id || seen_has(&p->seen, id)) continue;
        seen_add(&p->seen, id);
        i64 born = (i64)(sqlite3_column_double(s, 2) * 1000.0);
        bool ended = sqlite3_column_type(s, 3) != SQLITE_NULL;
        i64 end = ended ? (i64)(sqlite3_column_double(s, 3) * 1000.0) : 0;
        if (!p->baseline && ended && end < now - AGENTCTL_SUB_BASELINE_S * 1000) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", id);
        snprintf(f->oc_id, sizeof f->oc_id, "%s", id);
        agentctl_sanitize(col_text(s, 1), strlen(col_text(s, 1)), f->m.label, sizeof f->m.label, false);
        agentctl_sanitize(col_text(s, 4), strlen(col_text(s, 4)), f->m.model, sizeof f->m.model, false);
        f->m.start_ms = born;
        f->m.depth = sqlite3_column_int(s, 5);
        f->born = born * 1000000;
    }
    sqlite3_finalize(s);
}

/* OpenClaw: <state>/agents/<agent>/agent/openclaw-agent.sqlite, a subagent a
 * `session_nodes` row under the parent's key. The pane's own session is the
 * one of the newest store last spoken to since its agent took the pane. */
static void look_openclaw(SubPane *p, i64 now, FoundList *fl) {
    char root[1024];
    const char *env = getenv("OPENCLAW_STATE_DIR");
    const char *home = home_dir();
    if (env && env[0] == '/') snprintf(root, sizeof root, "%s/agents", env);
    else if (home) snprintf(root, sizeof root, "%s/.openclaw/agents", home);
    else return;
    char best[1100] = "";
    i64 best_mt = 0;
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char db[1100], wal[1110];
        if (snprintf(db, sizeof db, "%s/%s/agent/openclaw-agent.sqlite", root, de->d_name) >= (int)sizeof db) continue;
        snprintf(wal, sizeof wal, "%s-wal", db);
        i64 mt = mtime_of(db), mw = mtime_of(wal);
        if (mw > mt) mt = mw;
        if (mt > best_mt) { best_mt = mt; snprintf(best, sizeof best, "%s", db); }
    }
    closedir(d);
    sqlite3 *db = pane_db(p, best, now);
    if (!db) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT session_key FROM session_nodes WHERE instr(session_key, ':subagent:') = 0 "
            "AND COALESCE(created_via, '') <> 'spawn' "
            "AND COALESCE(last_interaction_at, last_activity_at, updated_at) >= ?1 "
            "ORDER BY COALESCE(last_interaction_at, last_activity_at, updated_at) DESC LIMIT 1", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(s, 1, p->wreg.since_ms - 10000);
    if (sqlite3_step(s) == SQLITE_ROW) snprintf(p->own_id, sizeof p->own_id, "%s", col_text(s, 0));
    sqlite3_finalize(s);
    if (!p->own_id[0]) return;
    if (sqlite3_prepare_v2(db,
            "WITH RECURSIVE kids(k, d) AS (SELECT session_key, 1 FROM session_nodes "
            "WHERE parent_session_key = ?1 OR spawned_by = ?1 "
            "UNION ALL SELECT n.session_key, kids.d + 1 FROM session_nodes n JOIN kids "
            "ON (n.parent_session_key = kids.k OR n.spawned_by = kids.k) WHERE kids.d < 8) "
            "SELECT n.session_key, n.current_session_id, COALESCE(n.label, n.display_name), n.status, "
            "COALESCE(n.created_at, n.updated_at), n.updated_at, kids.d FROM kids JOIN session_nodes n "
            "ON n.session_key = kids.k ORDER BY COALESCE(n.created_at, n.updated_at)", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, p->own_id, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *key = col_text(s, 0), *win = col_text(s, 1), *st = col_text(s, 3);
        if (!key[0] || strlen(key) >= sizeof ((SubJob *)0)->oc_id || strlen(win) >= sizeof ((SubJob *)0)->ev_id ||
            seen_has(&p->seen, key))
            continue;
        seen_add(&p->seen, key);
        i64 born = any_ms(sqlite3_column_int64(s, 4)), upd = any_ms(sqlite3_column_int64(s, 5));
        bool over = st[0] && strcmp(st, "running");
        if (!p->baseline && (over || upd < now - AGENTCTL_SUB_BASELINE_S * 1000)) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", key);
        snprintf(f->oc_id, sizeof f->oc_id, "%s", key);
        snprintf(f->meta, sizeof f->meta, "%s", win);
        agentctl_sanitize(col_text(s, 2), strlen(col_text(s, 2)), f->m.label, sizeof f->m.label, false);
        f->m.start_ms = born;
        f->m.depth = sqlite3_column_int(s, 6);
        f->born = born * 1000000;
    }
    sqlite3_finalize(s);
}

/* Copilot CLI: <session-state>/<id>/events.jsonl. The pane's own session is
 * the newest one started in its folder since its agent took the pane. */
static void look_copilot(SubPane *p, i64 now, FoundList *fl) {
    char root[1024];
    const char *env = getenv("COPILOT_HOME");
    const char *home = home_dir();
    if (env && env[0] == '/') snprintf(root, sizeof root, "%s/session-state", env);
    else if (home) snprintf(root, sizeof root, "%s/.copilot/session-state", home);
    else return;
    if (dir_changed(root, &p->root_ok, &p->root_mt) || !p->own_path[0]) {
        DIR *d = opendir(root);
        if (d) {
            char best[1100] = "";
            i64 best_mt = 0;
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                char path[1100];
                if (snprintf(path, sizeof path, "%s/%s/events.jsonl", root, de->d_name) >= (int)sizeof path) continue;
                i64 mt = mtime_of(path);
                if (mt < p->wreg.since_ms - 10000 || mt <= best_mt) continue;
                usize n = 0;
                char *l1 = first_line(path, 256u * 1024u, &n);
                char cwd[1024];
                bool ok = l1 && agentctl_subread_copilot_start(l1, n, cwd, sizeof cwd) && !strcmp(cwd, p->wreg.cwd);
                free(l1);
                if (ok) { best_mt = mt; snprintf(best, sizeof best, "%s", path); }
            }
            closedir(d);
            if (best[0] && strcmp(best, p->own_path)) {
                snprintf(p->own_path, sizeof p->own_path, "%s", best);
                p->cc_open = false;
            }
        }
    }
    if (p->own_path[0]) look_parent_log(p, now, fl, p->own_path, agentctl_subread_copilot_line);
}

/* Devin: the session's `message_nodes` rows, read as they come. */
static void look_devin(SubPane *p, i64 now, FoundList *fl) {
    sqlite3 *db = pane_db(p, p->wreg.session_path, now);
    if (!db || !p->wreg.session_id[0]) return;
    sqlite3_stmt *s = NULL;
    if (!p->row) {
        /* a session Liu has just seen: its last rows, for calls still running */
        if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(row_id), 0) FROM message_nodes WHERE session_id = ?1", -1, &s,
                               NULL) != SQLITE_OK)
            return;
        sqlite3_bind_text(s, 1, p->wreg.session_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) p->row = sqlite3_column_int64(s, 0) - SUB_OC_ROWS;
        sqlite3_finalize(s);
        if (p->row <= 0) p->row = -1;
    }
    if (sqlite3_prepare_v2(db, "SELECT row_id, chat_message, created_at FROM message_nodes "
                               "WHERE session_id = ?1 AND row_id > ?2 ORDER BY row_id LIMIT ?3", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, p->wreg.session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, p->row < 0 ? 0 : p->row);
    sqlite3_bind_int(s, 3, SUB_OC_ROWS);
    CcCtx c = { p, fl, now, !p->baseline };
    while (sqlite3_step(s) == SQLITE_ROW) {
        p->row = sqlite3_column_int64(s, 0);
        const char *j = (const char *)sqlite3_column_blob(s, 1);
        int n = sqlite3_column_bytes(s, 1);
        if (j && n > 0) agentctl_subread_devin_row(j, (usize)n, any_ms(sqlite3_column_int64(s, 2)), cc_event, &c);
    }
    sqlite3_finalize(s);
}

/* Cursor: ~/.cursor/projects/<the folder, every other character a dash>/
 * agent-transcripts/<conversation>/<conversation>.jsonl, a subagent's in
 * <conversation>/subagents/<id>.jsonl. The pane's own conversation is the
 * newest written since its agent took the pane. */
static void look_cursor(SubPane *p, i64 now, FoundList *fl) {
    const char *home = home_dir();
    if (!home || p->wreg.cwd[0] != '/') return;
    char slug[1024], root[1100];
    usize k = 0;
    for (const char *c = p->wreg.cwd; *c && k + 1 < sizeof slug; c++) {
        bool keep = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9');
        if (keep) slug[k++] = *c;
        else if (k && slug[k - 1] != '-') slug[k++] = '-';
    }
    while (k && slug[k - 1] == '-') k--;
    slug[k] = '\0';
    if (snprintf(root, sizeof root, "%s/.cursor/projects/%s/agent-transcripts", home, slug) >= (int)sizeof root) return;
    if (dir_changed(root, &p->day_ok[0], &p->day_mt[0]) || !p->own_id[0]) {
        DIR *d = opendir(root);
        if (!d) return;
        i64 best = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.' || strlen(de->d_name) > 200) continue;
            char path[1100];
            if (snprintf(path, sizeof path, "%s/%s/%s.jsonl", root, de->d_name, de->d_name) >= (int)sizeof path) continue;
            i64 mt = mtime_of(path);
            if (mt >= p->wreg.since_ms - 10000 && mt > best) {
                best = mt;
                snprintf(p->own_id, sizeof p->own_id, "%s", de->d_name);
            }
        }
        closedir(d);
    }
    if (!p->own_id[0]) return;
    char sd[1100];
    if (snprintf(sd, sizeof sd, "%s/%s/subagents", root, p->own_id) >= (int)sizeof sd) return;
    if (strcmp(sd, p->sdir)) {
        snprintf(p->sdir, sizeof p->sdir, "%s", sd);
        p->root_ok = false;
    }
    if (!dir_changed(p->sdir, &p->root_ok, &p->root_mt) && p->baseline) return;
    DIR *d = opendir(p->sdir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' || !ends_with(nm, ".jsonl") || seen_has(&p->seen, nm)) continue;
        char path[1100];
        if (!path_join(path, sizeof path, p->sdir, nm)) continue;
        seen_add(&p->seen, nm);
        if (!p->baseline && mtime_of(path) < now - AGENTCTL_SUB_BASELINE_S * 1000) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", nm);
        snprintf(f->path, sizeof f->path, "%s", path);
        f->born = born_ns(path);
    }
    closedir(d);
}

/* Kilo CLI: $XDG_DATA_HOME/kilo/kilo.db (OpenCode 1.x's tables), a subagent a
 * `session` row whose `parent_id` is the parent. The pane's own session is the
 * newest at the top in its folder since its agent took the pane. */
static void look_kilo(SubPane *p, i64 now, FoundList *fl) {
    char path[1100];
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = home_dir();
    if (xdg && xdg[0] == '/') snprintf(path, sizeof path, "%s/kilo/kilo.db", xdg);
    else if (home) snprintf(path, sizeof path, "%s/.local/share/kilo/kilo.db", home);
    else return;
    sqlite3 *db = pane_db(p, path, now);
    if (!db) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM session WHERE parent_id IS NULL AND directory = ?1 AND time_updated >= ?2 "
                               "ORDER BY time_updated DESC LIMIT 1", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, p->wreg.cwd, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, p->wreg.since_ms - 10000);
    if (sqlite3_step(s) == SQLITE_ROW) snprintf(p->own_id, sizeof p->own_id, "%s", col_text(s, 0));
    sqlite3_finalize(s);
    if (!p->own_id[0]) return;
    if (sqlite3_prepare_v2(db,
            "WITH RECURSIVE kids(id, d) AS (SELECT id, 1 FROM session WHERE parent_id = ?1 "
            "UNION ALL SELECT s.id, k.d + 1 FROM session s JOIN kids k ON s.parent_id = k.id WHERE k.d < 8) "
            "SELECT s.id, s.title, s.time_created, s.time_updated, k.d FROM kids k JOIN session s ON s.id = k.id "
            "ORDER BY s.time_created", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, p->own_id, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *id = col_text(s, 0);
        if (!id[0] || strlen(id) >= sizeof ((SubJob *)0)->oc_id || seen_has(&p->seen, id)) continue;
        seen_add(&p->seen, id);
        i64 born = any_ms(sqlite3_column_int64(s, 2)), upd = any_ms(sqlite3_column_int64(s, 3));
        if (!p->baseline && upd < now - AGENTCTL_SUB_BASELINE_S * 1000) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", id);
        snprintf(f->oc_id, sizeof f->oc_id, "%s", id);
        const char *title = col_text(s, 1);
        /* a child's title is OpenCode's "… (@agent subagent)"; it names the task */
        agentctl_sanitize(title, strlen(title), f->m.label, sizeof f->m.label, false);
        f->m.start_ms = born;
        f->m.depth = sqlite3_column_int(s, 4);
        f->born = born * 1000000;
    }
    sqlite3_finalize(s);
}

/* Amazon Q: <folder>/.amazonq/.subagents/<agent>.json, one task an agent. */
static void look_amazonq(SubPane *p, i64 now, FoundList *fl) {
    if (!p->sdir[0]) {
        if (p->wreg.cwd[0] != '/' ||
            snprintf(p->sdir, sizeof p->sdir, "%s/.amazonq/.subagents", p->wreg.cwd) >= (int)sizeof p->sdir) {
            p->sdir[0] = '\0';
            return;
        }
    }
    if (!dir_changed(p->sdir, &p->root_ok, &p->root_mt) && p->baseline) return;
    DIR *d = opendir(p->sdir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' || !ends_with(nm, ".json")) continue;
        char path[1100];
        if (!path_join(path, sizeof path, p->sdir, nm)) continue;
        usize n = 0;
        char *buf = slurp(path, SUB_META_MAX, &n, NULL);
        AgentctlSubMeta m;
        bool ok = buf && agentctl_submeta_amazonq(buf, n, &m);
        free(buf);
        if (!ok || seen_has(&p->seen, m.id)) continue;
        seen_add(&p->seen, m.id);
        bool over = m.status == AGSUB_DONE || m.status == AGSUB_FAILED;
        if (!p->baseline && (over || m.start_ms < now - AGENTCTL_SUB_BASELINE_S * 1000)) continue;
        if (m.start_ms && m.start_ms < p->wreg.since_ms - 10000) continue;
        Found *f = found_add(fl);
        if (!f) break;
        snprintf(f->key, sizeof f->key, "%s", m.id);
        f->m = m;
        f->m.status = AGSUB_UNKNOWN;
        snprintf(f->path, sizeof f->path, "%s", path);
        f->born = m.start_ms * 1000000;
    }
    closedir(d);
}

static void read_kilo(SubJob *j, SubPane *p, i64 now, LineBuf *lb) {
    sqlite3 *db = p ? p->db : NULL;
    if (!db) return;
    sqlite3_stmt *ms = NULL, *ps = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, data FROM message WHERE session_id = ?1 AND id > ?2 ORDER BY id LIMIT ?3",
                           -1, &ms, NULL) != SQLITE_OK)
        return;
    if (sqlite3_prepare_v2(db, "SELECT data FROM part WHERE message_id = ?1 ORDER BY id", -1, &ps, NULL) != SQLITE_OK) {
        sqlite3_finalize(ms);
        return;
    }
    sqlite3_bind_text(ms, 1, j->oc_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ms, 2, j->ev_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ms, 3, SUB_OC_ROWS);
    while (sqlite3_step(ms) == SQLITE_ROW) {
        const char *mid = col_text(ms, 0);
        const char *data = (const char *)sqlite3_column_blob(ms, 1);
        int n = sqlite3_column_bytes(ms, 1);
        char role[16];
        if (strlen(mid) >= sizeof j->ev_id) break;
        if (!agentctl_subread_oc1_message(&j->rd, data, n > 0 ? (usize)n : 0, role, sizeof role, lb_emit, lb)) break;
        sqlite3_reset(ps);
        sqlite3_bind_text(ps, 1, mid, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(ps) == SQLITE_ROW) {
            const char *pd = (const char *)sqlite3_column_blob(ps, 0);
            int pn = sqlite3_column_bytes(ps, 0);
            if (pd && pn > 0) agentctl_subread_oc1_part(&j->rd, role, pd, (usize)pn, lb_emit, lb);
        }
        snprintf(j->ev_id, sizeof j->ev_id, "%s", mid);
        j->grew_at = now;
    }
    sqlite3_finalize(ps);
    sqlite3_finalize(ms);
}

static void read_amazonq(SubJob *j, i64 now, LineBuf *lb) {
    struct stat st;
    if (stat(j->path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    i64 mt = (i64)ST_MTIM(st).tv_sec * 1000000000 + ST_MTIM(st).tv_nsec;
    if (mt == j->file_mt) return;
    usize n = 0;
    char *buf = slurp(j->path, 8u * 1024u * 1024u, &n, NULL);
    if (!buf) return;
    j->file_mt = mt;
    j->grew_at = now;
    AgentctlSubMeta m;
    if (agentctl_submeta_amazonq(buf, n, &m) && strcmp(m.id, j->key)) {
        /* the agent took a new task: this one is over */
        agentctl_subread_status(&j->rd, AGSUB_DONE, NULL, m.start_ms, lb_emit, lb);
    } else {
        agentctl_subread_amazonq(&j->rd, buf, n, lb_emit, lb);
    }
    free(buf);
}

/* ---- reading their rows ---- */

static void read_crush(SubJob *j, SubPane *p, i64 now, LineBuf *lb) {
    sqlite3 *db = p ? p->db : NULL;
    if (!db) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT rowid, role, parts, finished_at, created_at FROM messages "
                               "WHERE session_id = ?1 AND rowid > ?2 ORDER BY rowid LIMIT ?3", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, j->oc_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, j->oc_seq);
    sqlite3_bind_int(s, 3, SUB_OC_ROWS);
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *parts = (const char *)sqlite3_column_blob(s, 2);
        int n = sqlite3_column_bytes(s, 2);
        bool fin = sqlite3_column_type(s, 3) != SQLITE_NULL;
        if (!agentctl_subread_crush_row(&j->rd, col_text(s, 1), parts, n > 0 ? (usize)n : 0, fin,
                                        any_ms(sqlite3_column_int64(s, 4)), lb_emit, lb))
            break;
        j->oc_seq = sqlite3_column_int64(s, 0);
        j->grew_at = now;
    }
    sqlite3_finalize(s);
}

static void read_hermes(SubJob *j, SubPane *p, i64 now, LineBuf *lb) {
    sqlite3 *db = p ? p->db : NULL;
    if (!db) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, role, content, tool_calls, timestamp FROM messages "
                               "WHERE session_id = ?1 AND id > ?2 ORDER BY id LIMIT ?3", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, j->oc_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, j->oc_seq);
    sqlite3_bind_int(s, 3, SUB_OC_ROWS);
    while (sqlite3_step(s) == SQLITE_ROW) {
        agentctl_subread_chat_row(&j->rd, col_text(s, 1), col_text(s, 2), col_text(s, 3),
                                  (i64)(sqlite3_column_double(s, 4) * 1000.0), lb_emit, lb);
        j->oc_seq = sqlite3_column_int64(s, 0);
        j->grew_at = now;
    }
    sqlite3_finalize(s);
    if (sqlite3_prepare_v2(db, "SELECT ended_at, end_reason FROM sessions WHERE id = ?1", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, j->oc_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL) {
        i64 end = (i64)(sqlite3_column_double(s, 0) * 1000.0);
        const char *why = col_text(s, 1);
        AgentctlSubStatus st = AGSUB_DONE;
        if (strstr(why, "error") || strstr(why, "fail") || strstr(why, "timeout")) st = AGSUB_FAILED;
        else if (strstr(why, "interrupt") || strstr(why, "cancel") || strstr(why, "abort") || strstr(why, "kill"))
            st = AGSUB_STOPPED;
        if (j->rd.status == AGSUB_RUNNING)
            agentctl_subread_status(&j->rd, st, st == AGSUB_FAILED ? why : NULL, end, lb_emit, lb);
    }
    sqlite3_finalize(s);
}

static void read_openclaw(SubJob *j, SubPane *p, i64 now, LineBuf *lb) {
    sqlite3 *db = p ? p->db : NULL;
    if (!db) return;
    sqlite3_stmt *s = NULL;
    char status[32] = "";
    i64 upd = 0;
    if (sqlite3_prepare_v2(db, "SELECT current_session_id, status, updated_at FROM session_nodes WHERE session_key = ?1",
                           -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, j->oc_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *win = col_text(s, 0);
        if (win[0] && strlen(win) < sizeof j->ev_id && strcmp(win, j->ev_id)) {
            snprintf(j->ev_id, sizeof j->ev_id, "%s", win);
            j->oc_seq = 0;
        }
        snprintf(status, sizeof status, "%s", col_text(s, 1));
        upd = any_ms(sqlite3_column_int64(s, 2));
    }
    sqlite3_finalize(s);
    if (j->ev_id[0] &&
        sqlite3_prepare_v2(db, "SELECT seq, event_json FROM transcript_events WHERE session_id = ?1 AND seq > ?2 "
                               "ORDER BY seq LIMIT ?3", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, j->ev_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, j->oc_seq);
        sqlite3_bind_int(s, 3, SUB_OC_ROWS);
        while (sqlite3_step(s) == SQLITE_ROW) {
            /* an event kept compressed is passed over */
            if (sqlite3_column_type(s, 1) != SQLITE_NULL) {
                const char *ev = (const char *)sqlite3_column_blob(s, 1);
                int n = sqlite3_column_bytes(s, 1);
                if (ev && n > 0) agentctl_subread_line(&j->rd, ev, (usize)n, lb_emit, lb);
            }
            j->oc_seq = sqlite3_column_int64(s, 0);
            j->grew_at = now;
        }
        sqlite3_finalize(s);
    }
    if (status[0] && strcmp(status, "running") && j->rd.status == AGSUB_RUNNING) {
        AgentctlSubStatus st = !strcmp(status, "done") ? AGSUB_DONE : !strcmp(status, "killed") ? AGSUB_STOPPED
                             : AGSUB_FAILED;
        agentctl_subread_status(&j->rd, st, st == AGSUB_FAILED ? (!strcmp(status, "timeout") ? "it timed out" : "it failed")
                                                               : NULL, upd, lb_emit, lb);
    }
}

/* Cline CLI: the whole messages file, read again when it changed; its end
 * from sessions.db where the store keeps one. */
static void read_cline(SubJob *j, SubPane *p, i64 now, LineBuf *lb) {
    struct stat st;
    if (stat(j->path, &st) == 0 && S_ISREG(st.st_mode)) {
        i64 mt = (i64)ST_MTIM(st).tv_sec * 1000000000 + ST_MTIM(st).tv_nsec;
        if (mt != j->file_mt) {
            usize n = 0;
            char *buf = slurp(j->path, 32u * 1024u * 1024u, &n, NULL);
            if (buf) {
                j->file_mt = mt;
                j->grew_at = now;
                j->oc_seq = agentctl_subread_cline_messages(&j->rd, buf, n, (u32)j->oc_seq, lb_emit, lb);
                free(buf);
            }
        }
    }
    if (!p || j->rd.status != AGSUB_RUNNING) return;
    char sdir[1100], root[1100], dbp[1120], sid[512];
    dir_of(j->path, sdir, sizeof sdir);
    dir_of(sdir, root, sizeof root);
    if (snprintf(dbp, sizeof dbp, "%s/sessions.db", root) >= (int)sizeof dbp) return;
    sqlite3 *db = pane_db(p, dbp, now);
    if (!db || snprintf(sid, sizeof sid, "%s__%s", p->wreg.session_id, j->key) >= (int)sizeof sid) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT status, ended_at FROM sessions WHERE session_id = ?1", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *w = col_text(s, 0);
        AgentctlSubStatus stt = !strcmp(w, "completed") ? AGSUB_DONE : !strcmp(w, "failed") ? AGSUB_FAILED
                              : !strcmp(w, "cancelled") || !strcmp(w, "aborted") ? AGSUB_STOPPED : AGSUB_UNKNOWN;
        if (stt != AGSUB_UNKNOWN)
            agentctl_subread_status(&j->rd, stt, stt == AGSUB_FAILED ? "it failed" : NULL,
                                    agentctl_iso_ms(col_text(s, 1)), lb_emit, lb);
    }
    sqlite3_finalize(s);
}

/* --------------------------------------------------------------- a pass --- */

static void pane_reset_looks(SubPane *p) {
    seen_free(&p->seen);
    p->baseline = false;
    p->root_ok = false;
    p->day_ok[0] = p->day_ok[1] = false;
    p->day_dir[0][0] = p->day_dir[1][0] = '\0';
    p->sdir[0] = '\0';
    p->thread[0] = '\0';
    p->cc_open = false;
    p->cc_part_len = 0;
    p->agent_left = false;
    p->next_look = 0;
    p->own_id[0] = '\0';
    p->own_path[0] = '\0';
    p->row = 0;
    pane_db_close(p);
}

static void pane_free(SubPane *p) {
    seen_free(&p->seen);
    free(p->cc_part);
    pane_db_close(p);
    memset(p, 0, sizeof *p);
}

/* Under g_mu: the subagents of a session the pane no longer runs go; one of
 * an agent that left the pane and was still running is shown stopped. */
static void pane_settle_jobs(SubPane *p, bool agent_left) {
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) {
        SubJob *j = &g_jobs[i];
        if (!j->id || j->pane != p->reg.pane) continue;
        if (p->gone || !agent_left) {
            if (p->gone || j->pane_gen != p->reg_gen) j->retire = true;
            continue;
        }
        if (j->rd.status == AGSUB_RUNNING) {
            agentctl_subread_status(&j->rd, AGSUB_STOPPED, NULL, wall_ms(), lb_emit, &j->pend);
            job_publish(j);
        }
    }
}

static void add_found(SubPane *p, FoundList *fl, i64 now) {
    /* the ones read from the parent's log are found in the order it called them */
    if (fl->n > 1 && !parent_fed(p->wreg.tool)) qsort(fl->v, (usize)fl->n, sizeof *fl->v, found_order);
    for (i32 i = 0; i < fl->n; i++) {
        Found *f = &fl->v[i];
        if (f->key[0]) {
            pthread_mutex_lock(&g_mu);
            SubJob *j = job_new(p, f->key, &f->m, now);
            if (j) {
                snprintf(j->path, sizeof j->path, "%s", f->path);
                if (parent_fed(p->wreg.tool)) {
                    snprintf(j->cc_call, sizeof j->cc_call, "%s", f->key);
                    j->cc_background = !strcmp(f->meta, "bg");
                    snprintf(j->cc_bg, sizeof j->cc_bg, "%s", f->oc_id);
                    snprintf(j->cp_agent, sizeof j->cp_agent, "%s", f->agent);
                } else if (p->wreg.tool == CHAT_TOOL_OPENCLAW) {
                    snprintf(j->oc_id, sizeof j->oc_id, "%s", f->oc_id);
                    snprintf(j->ev_id, sizeof j->ev_id, "%s", f->meta);
                } else {
                    snprintf(j->meta, sizeof j->meta, "%s", f->meta);
                    snprintf(j->oc_id, sizeof j->oc_id, "%s", f->oc_id);
                }
                if (f->has_task) agentctl_subread_task(&j->rd, f->task_text, strlen(f->task_text), lb_emit, &j->pend);
                /* its own lines read before it was shown */
                for (usize at = 0; at < f->child_len; ) {
                    const char *nl = memchr(f->child + at, '\n', f->child_len - at);
                    usize n = nl ? (usize)(nl - (f->child + at)) : f->child_len - at;
                    agentctl_subread_line(&j->rd, f->child + at, n, lb_emit, &j->pend);
                    at += n + 1;
                }
                if (f->settled && f->answer_pi) {
                    agentctl_subread_pi_result(&j->rd, f->answer, f->answer_len, now, lb_emit, &j->pend);
                } else if (f->settled) {
                    if (f->answer) agentctl_subread_answer(&j->rd, f->answer, f->answer_len, lb_emit, &j->pend);
                    agentctl_subread_status(&j->rd, (AgentctlSubStatus)f->settled_as,
                                            f->settled_as == AGSUB_FAILED ? (f->why[0] ? f->why : "it failed") : NULL,
                                            now, lb_emit, &j->pend);
                }
                job_publish(j);
                bump_jobs();
            }
            pthread_mutex_unlock(&g_mu);
        }
        free(f->answer);
        free(f->child);
    }
}

static void look_pane(SubPane *p, i64 now) {
    FoundList fl = {0};
    switch (p->wreg.tool) {
    case CHAT_TOOL_CLAUDE:
    case CHAT_TOOL_QODER:       look_claude(p, now, &fl); break;
    case CHAT_TOOL_CURSOR:      look_cursor(p, now, &fl); break;
    case CHAT_TOOL_KILO:        look_kilo(p, now, &fl); break;
    case CHAT_TOOL_AMAZON_Q:    look_amazonq(p, now, &fl); break;
    case CHAT_TOOL_CODEX:       look_codex(p, now, &fl); break;
    case CHAT_TOOL_OPENCODE:    look_opencode(p, now, &fl); break;
    case CHAT_TOOL_XAI:
    case CHAT_TOOL_KIMI:        look_meta_dirs(p, now, &fl); break;
    case CHAT_TOOL_OMP:         look_omp(p, now, &fl); break;
    case CHAT_TOOL_COMMANDCODE: look_parent_log(p, now, &fl, p->wreg.session_path, agentctl_subread_cc_line); break;
    case CHAT_TOOL_PI:          look_parent_log(p, now, &fl, p->wreg.session_path, agentctl_subread_pi_line); break;
    case CHAT_TOOL_COPILOT:     look_copilot(p, now, &fl); break;
    case CHAT_TOOL_DEVIN:       look_devin(p, now, &fl); break;
    case CHAT_TOOL_QWEN:        look_qwen(p, now, &fl); break;
    case CHAT_TOOL_DROID:       look_droid(p, now, &fl); break;
    case CHAT_TOOL_CLINE_CLI:   look_cline(p, now, &fl); break;
    case CHAT_TOOL_CRUSH:       look_crush(p, now, &fl); break;
    case CHAT_TOOL_HERMES:      look_hermes(p, now, &fl); break;
    case CHAT_TOOL_OPENCLAW:    look_openclaw(p, now, &fl); break;
    default: break;
    }
    add_found(p, &fl, now);
    free(fl.v);
    p->baseline = true;
}

static void read_job(SubJob *j, SubPane *p, i64 now) {
    LineBuf *lb = &j->pend;
    switch (j->tool) {
    case CHAT_TOOL_OPENCODE:  read_opencode(j, now, lb); break;
    case CHAT_TOOL_CRUSH:     read_crush(j, p, now, lb); break;
    case CHAT_TOOL_HERMES:    read_hermes(j, p, now, lb); break;
    case CHAT_TOOL_OPENCLAW:  read_openclaw(j, p, now, lb); break;
    case CHAT_TOOL_CLINE_CLI: read_cline(j, p, now, lb); break;
    case CHAT_TOOL_KILO:      read_kilo(j, p, now, lb); break;
    case CHAT_TOOL_AMAZON_Q:  read_amazonq(j, now, lb); break;
    default: if (j->path[0]) read_transcript(j, now, lb); break;
    }
    read_meta(j, lb);
    /* a final answer, and nothing since, where the record says no end: done */
    if (quiet_done(j->tool) && j->rd.status == AGSUB_RUNNING && j->rd.claude_answered &&
        j->grew_at && now - j->grew_at >= SUB_CLAUDE_QUIET_MS)
        agentctl_subread_status(&j->rd, j->rd.error[0] ? AGSUB_FAILED : AGSUB_DONE, j->rd.error[0] ? j->rd.error : NULL,
                                j->rd.last_ms, lb_emit, lb);
}

/* Under g_mu. */
static bool pane_has_running(const SubPane *p) {
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) {
        const SubJob *j = &g_jobs[i];
        if (j->id && !j->retire && j->pane == p->reg.pane && j->state == AGJ_RUNNING) return true;
    }
    return false;
}

static void free_retired(void) {
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++)
        if (g_jobs[i].id && g_jobs[i].retire) { job_free(&g_jobs[i]); bump_jobs(); }
}

static void at_least(i64 *next, i64 at) {
    if (!*next || at < *next) *next = at;
}

/* One pass: the panes' changes taken in, retired subagents freed, new ones
 * looked for, the due ones read. Returns when the next pass is due (0 =
 * nothing to do until the main thread says something). Only this pass adds
 * or frees a pane or a subagent, so between its turns of the lock what it
 * reads of them is its own. */
static i64 pass(i64 now) {
    i64 next = 0;
    u64 jobs_before = atomic_load_explicit(&g_jobs_gen, memory_order_acquire);
    u64 cards_before = atomic_load_explicit(&g_cards_gen, memory_order_acquire);
    bool look[SUB_PANES_MAX];
    memset(look, 0, sizeof look);

    pthread_mutex_lock(&g_mu);
    free_retired();
    bool enabled = g_enabled;
    for (i32 i = 0; i < SUB_PANES_MAX; i++) {
        SubPane *p = &g_panes[i];
        if (!p->used) continue;
        if (p->gone) {
            pane_settle_jobs(p, false);
            pane_free(p);
            continue;
        }
        if (p->done_gen != p->reg_gen) {
            /* another agent or session: what was looked at belongs to the old one */
            bool left = p->reg.tool == 0;
            pane_settle_jobs(p, left);
            pane_reset_looks(p);
            p->done_gen = p->reg_gen;
            p->agent_left = left;
        }
        p->wreg = p->reg;
        bool live = enabled && p->reg.tool && agentctl_subagents_supported(p->reg.tool) &&
                    (p->reg.session_id[0] || p->reg.session_path[0] ||
                     (self_locating(p->reg.tool) && p->reg.cwd[0] == '/'));
        if (!live) continue;
        if (p->reg.busy || p->prompted) p->look_until = now + AGENTCTL_SUB_LOOK_AFTER_S * 1000;
        p->prompted = false;
        bool wanted = !p->baseline || now < p->look_until || pane_has_running(p);
        if (!wanted) continue;
        if (now >= p->next_look) look[i] = true;
        else at_least(&next, p->next_look);
    }
    free_retired();
    pthread_mutex_unlock(&g_mu);

    for (i32 i = 0; i < SUB_PANES_MAX; i++) {
        if (!look[i]) continue;
        SubPane *p = &g_panes[i];
        look_pane(p, now);
        p->next_look = now + SUB_LOOK_MS;
        at_least(&next, p->next_look);
        if (parent_fed(p->wreg.tool)) {
            /* its subagents' words are in the parent's log */
            pthread_mutex_lock(&g_mu);
            for (i32 k = 0; k < AGENTCTL_MAX_WATCHED; k++)
                if (g_jobs[k].id && g_jobs[k].pane == p->wreg.pane && g_jobs[k].pend.len) job_publish(&g_jobs[k]);
            pthread_mutex_unlock(&g_mu);
        }
    }

    if (enabled) {
        for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) {
            SubJob *j = &g_jobs[i];
            pthread_mutex_lock(&g_mu);
            bool live = j->id && !j->retire && !parent_fed(j->tool);
            bool running = live && j->state == AGJ_RUNNING;
            SubPane *p = live ? pane_by_serial(j->pane) : NULL;
            bool looking = p && now < p->look_until;
            pthread_mutex_unlock(&g_mu);
            if (!live || (!running && !looking)) continue;
            if (now < j->next_read) { at_least(&next, j->next_read); continue; }
            read_job(j, p, now);
            j->next_read = now + (running ? SUB_READ_MS : SUB_SETTLED_READ_MS);
            at_least(&next, j->next_read);
            pthread_mutex_lock(&g_mu);
            job_publish(j);
            pthread_mutex_unlock(&g_mu);
        }
    }

    pthread_mutex_lock(&g_mu);
    count_shown();
    bool oc_wanted = false;
    for (i32 i = 0; i < SUB_PANES_MAX; i++)
        if (g_panes[i].used && g_panes[i].reg.tool == CHAT_TOOL_OPENCODE) oc_wanted = true;
    pthread_mutex_unlock(&g_mu);
    if (!oc_wanted) oc_close();

    u64 jobs_after = atomic_load_explicit(&g_jobs_gen, memory_order_acquire);
    u64 cards_after = atomic_load_explicit(&g_cards_gen, memory_order_acquire);
    if (g_wake && (jobs_after != jobs_before ||
                   (cards_after != cards_before && atomic_load_explicit(&g_log_watched, memory_order_acquire))))
        g_wake();
    return next;
}

/* -------------------------------------------------------------- thread --- */

static void *watcher_main(void *arg) {
    (void)arg;
    i64 next = 0;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (!g_stop && !g_kick) {
            i64 now = wall_ms();
            if (next && now >= next) break;
            if (next) {
                struct timespec until = { (time_t)(next / 1000), (long)((next % 1000) * 1000000) };
                pthread_cond_timedwait(&g_cv, &g_mu, &until);
            } else {
                pthread_cond_wait(&g_cv, &g_mu);
            }
        }
        if (g_stop) break;
        g_kick = false;
        pthread_mutex_unlock(&g_mu);
        next = pass(wall_ms());
        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* Under g_mu. */
static void kick(void) {
    g_kick = true;
    if (g_thread_mode && !g_thread_on && !g_stop) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1024u * 1024u);
        if (pthread_create(&g_thread, &at, watcher_main, NULL) == 0) g_thread_on = true;
        pthread_attr_destroy(&at);
    }
    pthread_cond_signal(&g_cv);
}

/* --------------------------------------------------------- the view --- */

static u64 view_jobs_gen(void) { return atomic_load_explicit(&g_jobs_gen, memory_order_acquire); }
static u64 view_cards_gen(void) { return atomic_load_explicit(&g_cards_gen, memory_order_acquire); }
static bool view_any(void) { return atomic_load_explicit(&g_shown, memory_order_acquire) > 0; }

static void brief_of(const SubJob *j, AgentctlJobBrief *b) {
    memset(b, 0, sizeof *b);
    b->job = j->id;
    b->owner_serial = j->pane;
    b->agent = AGENTCTL_AGENT_TOOL_BASE + j->tool;
    b->state = j->state;
    b->outcome = j->outcome;
    b->end = AGE_NONE;
    snprintf(b->label, sizeof b->label, "%s", j->label);
    b->turn_start_ms = j->start_ms;
    b->turn_end_ms = j->state == AGJ_RUNNING ? 0 : j->end_ms;
}

/* Oldest first, as delegated jobs are listed. */
static i32 view_brief(AgentctlJobBrief *out, i32 cap) {
    if (!out || cap <= 0 || !view_any()) return 0;
    i32 n = 0;
    pthread_mutex_lock(&g_mu);
    u32 last = 0;
    for (;;) {
        const SubJob *best = NULL;
        for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) {
            const SubJob *j = &g_jobs[i];
            if (!job_shown(j) || j->id <= last) continue;
            if (!best || j->id < best->id) best = j;
        }
        if (!best || n >= cap) break;
        last = best->id;
        brief_of(best, &out[n++]);
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

static void card_of(const SubJob *j, AgentctlCard *c) {
    memset(c, 0, sizeof *c);
    c->job = j->id;
    c->agent = AGENTCTL_AGENT_TOOL_BASE + j->tool;
    c->state = j->state;
    c->outcome = j->outcome;
    snprintf(c->label, sizeof c->label, "%s", j->label);
    c->created_ms = j->created_ms;
    c->turn_start_ms = j->start_ms;
    c->turn_end_ms = j->state == AGJ_RUNNING ? 0 : j->end_ms;
    agentctl_ring_card_lines(&j->ring, c);
}

static bool view_card(u32 job, AgentctlCard *out) {
    bool ok = false;
    pthread_mutex_lock(&g_mu);
    const SubJob *j = job_by_id(job);
    if (j && job_shown(j)) { card_of(j, out); ok = true; }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

static i32 view_owner_cards(u64 owner, AgentctlCard *out, i32 cap) {
    if (!owner || !out || cap <= 0 || !view_any()) return 0;
    i32 n = 0;
    pthread_mutex_lock(&g_mu);
    u32 last = 0;
    for (;;) {
        const SubJob *best = NULL;
        for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) {
            const SubJob *j = &g_jobs[i];
            if (!job_shown(j) || j->pane != owner || j->id <= last) continue;
            if (!best || j->id < best->id) best = j;
        }
        if (!best || n >= cap) break;
        last = best->id;
        card_of(best, &out[n++]);
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

static bool view_snapshot(u32 job, AgentctlSnapshot *out) {
    bool ok = false;
    pthread_mutex_lock(&g_mu);
    const SubJob *j = job_by_id(job);
    if (j && job_shown(j)) {
        memset(out, 0, sizeof *out);
        out->job = j->id;
        out->agent = AGENTCTL_AGENT_TOOL_BASE + j->tool;
        out->perm = -1;
        out->state = j->state;
        out->outcome = j->outcome;
        snprintf(out->model, sizeof out->model, "%s", j->model);
        snprintf(out->cwd, sizeof out->cwd, "%s", j->cwd);
        snprintf(out->label, sizeof out->label, "%s", j->label);
        snprintf(out->owner_label, sizeof out->owner_label, "%s", j->owner_label);
        snprintf(out->session, sizeof out->session, "%s", j->key);
        snprintf(out->error, sizeof out->error, "%s", j->error);
        snprintf(out->kind, sizeof out->kind, "%s", j->kind);
        out->turns = 1;
        out->turn_start_ms = j->start_ms;
        out->turn_end_ms = j->state == AGJ_RUNNING ? 0 : j->end_ms;
        out->owner_serial = j->pane;
        out->spent_in = j->tok_in;
        out->spent_out = j->tok_out;
        out->watched = true;
        agentctl_ring_snapshot_lines(&j->ring, out);
        ok = true;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

static void view_dismiss(u32 job) {
    pthread_mutex_lock(&g_mu);
    SubJob *j = job_by_id(job);
    if (j && !j->retire) {
        j->retire = true;
        bump_jobs();
        count_shown();
        kick();
    }
    pthread_mutex_unlock(&g_mu);
}

static void view_log_watched(bool on) {
    atomic_store_explicit(&g_log_watched, on, memory_order_release);
}

static const AgentctlWatchedView k_view = {
    .jobs_gen = view_jobs_gen,
    .cards_gen = view_cards_gen,
    .any = view_any,
    .brief = view_brief,
    .card = view_card,
    .owner_cards = view_owner_cards,
    .snapshot = view_snapshot,
    .dismiss = view_dismiss,
    .log_watched = view_log_watched,
};

/* --------------------------------------------------------- the API --- */

void agentctl_subagents_start(void (*wake_main)(void), bool thread) {
    pthread_mutex_lock(&g_mu);
    g_wake = wake_main;
    g_thread_mode = thread;
    g_stop = false;
    pthread_mutex_unlock(&g_mu);
    agentctl_set_watched_view(&k_view);
}

static void forget_all(void) {
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) if (g_jobs[i].id) job_free(&g_jobs[i]);
    for (i32 i = 0; i < SUB_PANES_MAX; i++) if (g_panes[i].used) pane_free(&g_panes[i]);
    oc_close();
    count_shown();
    bump_jobs();
}

void agentctl_subagents_stop(void) {
    agentctl_set_watched_view(NULL);
    pthread_mutex_lock(&g_mu);
    g_stop = true;
    bool joined = g_thread_on;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    if (joined) pthread_join(g_thread, NULL);
    pthread_mutex_lock(&g_mu);
    g_thread_on = false;
    forget_all();
    pthread_mutex_unlock(&g_mu);
}

void agentctl_subagents_exit(void) {
    pthread_mutex_lock(&g_mu);
    g_stop = true;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

void agentctl_subagents_set_enabled(bool on) {
    pthread_mutex_lock(&g_mu);
    if (g_enabled != on) {
        g_enabled = on;
        for (i32 i = 0; i < SUB_PANES_MAX; i++) if (g_panes[i].used) g_panes[i].reg_gen++;
        if (!on)
            for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) if (g_jobs[i].id) g_jobs[i].retire = true;
        count_shown();
        bump_jobs();
        kick();
    }
    pthread_mutex_unlock(&g_mu);
}

void agentctl_subagents_pane(const AgentctlSubPane *in) {
    if (!in || !in->pane) return;
    pthread_mutex_lock(&g_mu);
    SubPane *p = pane_by_serial(in->pane);
    bool wanted = in->tool && agentctl_subagents_supported(in->tool);
    if (!p && wanted) {
        for (i32 i = 0; i < SUB_PANES_MAX && !p; i++) if (!g_panes[i].used) p = &g_panes[i];
        if (p) {
            memset(p, 0, sizeof *p);
            p->used = true;
            p->reg_gen = 1;
            kick();
        }
    }
    if (p) {
        bool moved = p->reg.tool != in->tool || strcmp(p->reg.session_id, in->session_id) != 0 ||
                     strcmp(p->reg.session_path, in->session_path) != 0;
        bool woke = in->busy && !p->reg.busy;
        p->reg = *in;
        if (moved) p->reg_gen++;
        if (moved || woke) kick();
    }
    pthread_mutex_unlock(&g_mu);
}

void agentctl_subagents_pane_gone(u64 pane) {
    if (!pane) return;
    pthread_mutex_lock(&g_mu);
    SubPane *p = pane_by_serial(pane);
    if (p) p->gone = true;
    bool any = false;
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++)
        if (g_jobs[i].id && g_jobs[i].pane == pane && !g_jobs[i].retire) { g_jobs[i].retire = true; any = true; }
    if (any) { count_shown(); bump_jobs(); }
    if (p || any) kick();
    pthread_mutex_unlock(&g_mu);
}

void agentctl_subagents_prompt(u64 pane) {
    if (!pane) return;
    pthread_mutex_lock(&g_mu);
    SubPane *p = pane_by_serial(pane);
    if (p) {
        p->prompted = true;
        kick();
    }
    bool any = false;
    for (i32 i = 0; i < AGENTCTL_MAX_WATCHED; i++) {
        SubJob *j = &g_jobs[i];
        if (j->id && j->pane == pane && !j->retire && j->state != AGJ_RUNNING) { j->retire = true; any = true; }
    }
    if (any) { count_shown(); bump_jobs(); kick(); }
    pthread_mutex_unlock(&g_mu);
}

void agentctl_subagents_poll_once(i64 now_ms) {
    (void)pass(now_ms);
}

i32 agentctl_subagents_count(void) {
    return atomic_load_explicit(&g_shown, memory_order_acquire);
}
