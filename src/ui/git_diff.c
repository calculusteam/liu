/*
 * Liu — diff dock (see git_diff.h for the design note).
 */
#include "ui/git_diff.h"
#include "ui/git_exec.h"
#include <stdatomic.h>
#include "ui/chrome_palette.h"
#include "ui/syntax.h"
#include "ui/anim.h"   /* ui_hover_hit — the pointer-damage registry */
#include "core/utf8.h"
#include "core/mem_budget.h"
#include "core/file_stamp.h"
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
#include "platform/platform.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The whole worktree diff in one buffer. Big enough for anything a person
 * reads; past it the tail is dropped with a marker rather than silently
 * truncated. A CEILING, not an allocation: the buffer grows with the output
 * (GitGrowBuf), so a 73-byte diff holds a page and only a diff this long holds
 * this much. */
#define GD_DIFF_CAP  (8u * 1024u * 1024u)
/* Ceiling for the untracked-file list, and for each untracked file's own
 * diff — one enormous new file must not crowd every other one out. */
#define GD_SCRATCH_CAP (1u << 20)
/* Each untracked file costs one fork+exec of git; the cap keeps a directory
 * that should have been ignored from parking the worker for seconds. */
#define GD_UNTRACKED_MAX_FILES 200
/* A diff longer than this is past the point of being read in a sidebar, and
 * the row array and cached spans are bounded separately. */
#define GD_MAX_LINES 200000
/* Tab stop used when expanding '\t' in a diff row. */
#define GD_TAB_WIDTH 4
#define GD_REFRESH_SEC 0.75
#define GD_PAINT_CAP (32u * 1024u * 1024u)
#define GD_POINT_STRIDE 128u
#define GD_SPAN_LIMIT 16384u

/* =========================================================================
 * Render model — built once per diff, walked per frame.
 * ========================================================================= */

typedef enum {
    GDL_GAP = 0,   /* blank spacer above a file card */
    GDL_FILE,      /* file header row */
    GDL_HUNK,      /* @@ -a,b +c,d @@ */
    GDL_CTX,       /* unchanged */
    GDL_ADD,
    GDL_DEL,
    GDL_NOTE,      /* "Binary files differ", truncation marker */
} GdKind;

typedef struct GdLine {
    u32 off, len;      /* slice of GitDiffView.text, marker prefix stripped */
    i32 old_no, new_no;/* -1 when the row has no number on that side */
    i32 file;          /* index into files[], -1 for none */
    u8  kind;
    /* Scanner continuation for THIS row, resolved at parse time. It has to be
     * stored rather than threaded at paint time: the renderer starts at
     * whatever row the scroll offset lands on and has no earlier lines to
     * carry state forward from.
     *
     * The full u32, not a narrower field: the scanner's state word carries
     * block-comment NESTING depth in bits 8..11 (a u8 would drop it), the
     * embedded-region kind in bits 12..15 (a Vue file's <script> block, an
     * Astro frontmatter) and a `{ }` expression depth in bits 16..19 (a u16
     * would drop a multi-line Svelte expression's depth). Four bytes per row.
     */
    u32 syn_state;
    u32 paint_off, point_count, span_count, cols;
} GdLine;

/* Sparse byte/column checkpoints and syntax spans are built with the model.
 * A horizontal swipe seeks near its first visible glyph, never rescans a
 * multi-megabyte prefix on the UI thread. */
typedef struct { u32 off, col; } GdPoint;

typedef struct GdFile {
    u32 path_off, path_len;
    u32 old_path_off, old_path_len;
    i32 added, removed;
    u8 status;
    u8  lang;          /* SynLang, from the path's extension */
} GdFile;

/* =========================================================================
 * Worker — one job at a time, published under a mutex, drained per frame.
 * Mirrors git_status.c's pattern: the main thread never blocks on git.
 * ========================================================================= */

typedef struct {
    char root[1024];
    /* Which request this job answers. Every gd_request takes a new one, so a
     * result from a run that was cancelled, or that a rebind or a
     * close-and-reopen has since superseded, is recognised at pickup by its
     * number and dropped — a cancelled run's TRUNCATED diff is never
     * installed as the dock's content just because its root matches. */
    u32  gen;
    bool probe;
    u64 fingerprint;
} GdJob;

/* The parsed diff: the text every row slices into, the rows, the files, and
 * the figures the renderer reads off them. Built by the WORKER, in full, and
 * adopted by the view with a struct copy, so the parse of a large diff costs
 * the main thread no frame at the moment the dock opens. Nothing in the
 * parse needs the view: syntax.c is pure over const tables and utf8_decode
 * has no state, which is what makes it safe to run off the main thread. */
typedef struct {
    char   *text;
    usize   text_len, text_cap;
    GdLine *lines;
    i32     line_count, line_cap;
    GdFile *files;
    i32     file_count, file_cap;
    i32     total_added, total_removed;
    i32     gutter_digits;
    i32     max_cols;
    i32     current_file;
    GitGrowBuf paint;
    GitCancel *cancel;            /* parse-time only, never retained by the view */
    bool failed, truncated;
} GdModel;

typedef struct {
    char       root[1024];
    u32        gen;
    bool       unchanged, failed, truncated;
    u64        fingerprint;
    bool       cancelled;     /* the run was stopped; the text is partial */
    /* The diff text as captured. While it sits here its capacity is on the
     * ledger's TRANSIENT side (the worker charges it as it grows); the pickup
     * moves that capacity to the retained side in one step. */
    GitGrowBuf buf;
    /* And the parse of it, built by the worker after the capture; its arrays
     * are charged transient the same way. `model.text` aliases buf.data. */
    GdModel    model;
} GdResult;

/* One worker + one result slot PER VIEW (GitDiffView.worker), not per process.
 * A slot shared by every window's dock would let two docks on the same repo
 * both match the one result: whichever polled second would install it and the
 * first would stay `loading` forever — and gd_spawn refuses while a run is
 * live, so nothing would re-arm it. */
typedef struct {
    pthread_t       tid;
    pthread_mutex_t mtx;
    pthread_cond_t collected;
    _Atomic i32 refs;
    u32 claimed_gen;
    bool abandoned;
    bool            live;     /* thread handle live & unjoined */
    bool            ready;    /* result waiting for pickup */
    /* `ready`, readable without the mutex: the poll runs every frame for
     * every window and only needs the lock once something has landed. */
    _Atomic bool    published;
    u32             gen;      /* the newest request; older results are stale */
    GdJob           job;
    GdResult        res;
    f64             last_probe;
    u64             fingerprint;
    /* The job is a `git diff HEAD`, a `git ls-files --others`, and one
     * `git diff --no-index` fork per untracked file up to
     * GD_UNTRACKED_MAX_FILES. read() and waitpid() do not notice a flag,
     * so this carries the flag AND the process group of the run in
     * flight, and a cancel can actually stop it. */
    GitCancel       cancel;
} GdWorker;

static _Atomic i32 gd_live_workers;
#define GD_MAX_WORKERS 8

static GdWorker *gd_worker_state(GitDiffView *v) {
    if (!v->worker) {
        GdWorker *w = calloc(1, sizeof *w);
        if (!w) return NULL;
        if (pthread_mutex_init(&w->mtx, NULL) != 0) { free(w); return NULL; }
        if (pthread_cond_init(&w->collected, NULL) != 0) { pthread_mutex_destroy(&w->mtx); free(w); return NULL; }
        atomic_init(&w->refs, 1);
        v->worker = w;
    }
    return (GdWorker *)v->worker;
}

/* The worker's allocations, charged as they happen from the worker thread.
 * Transient because that is the ledger's thread-safe side; the pickup moves
 * what it keeps onto the retained side (see gd_install). */
static void gd_charge_transient(void *ctx, isize cap_delta) {
    (void)ctx;
    mem_budget_transient_adjust(MEM_GIT, cap_delta);
}

/* What the dock retains for its parsed diff: the text, the row array and the
 * file array, at their CAPACITIES. One function, so the charge and the credit
 * cannot disagree about what is being counted. */
static usize gd_retained_bytes(const GitDiffView *v) {
    return v->text_cap + (usize)v->line_cap * sizeof(GdLine) +
           (usize)v->file_cap * sizeof(GdFile) + v->paint_cache_cap;
}
static usize gd_model_bytes(const GdModel *m) {
    return m->text_cap + (usize)m->line_cap * sizeof(GdLine) +
           (usize)m->file_cap * sizeof(GdFile) + m->paint.cap;
}
static void gd_model_free(GdModel *m) {
    free(m->lines);
    free(m->files);
    git_grow_free(&m->paint);
    /* text is buf.data — released with the buffer by the caller */
    memset(m, 0, sizeof *m);
}

/* Release an uncollected or unwanted result: the arrays and the text, and
 * their transient charge, in one place. */
static void gd_result_drop(GdResult *r) {
    if (!r) return;
    usize arrays = (usize)r->model.line_cap * sizeof(GdLine) +
                   (usize)r->model.file_cap * sizeof(GdFile);
    if (arrays) mem_budget_transient_adjust(MEM_GIT, -(isize)arrays);
    gd_model_free(&r->model);
    git_grow_free(&r->buf);       /* charges its own release */
}

/* Append a NUL-terminated note, within the ceiling. */
static void gd_append_note(GitGrowBuf *g, usize ceiling, const char *note) {
    usize n = strlen(note);
    if (g->len >= ceiling - 1) return;
    if (n > ceiling - 1 - g->len) n = ceiling - 1 - g->len;
    if (!git_grow_reserve(g, n)) return;
    memcpy(g->data + g->len, note, n);
    g->len += n;
    g->data[g->len] = '\0';
}

/* Probe only the changed paths. The porcelain bytes include index/status
 * transitions; nanosecond mtime/ctime, inode and size catch edits that retain
 * the same +/- counts. Git/status and all filesystem I/O stay on the worker. */
static u64 gd_hash(u64 h, const void *bytes, usize n) {
    const u8 *p = bytes;
    for (usize i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}
static bool gd_fingerprint(const char *root, GitCancel *cx, u64 *out) {
    GitGrowBuf status = { .charge = gd_charge_transient };
    const char *args[] = {"git", "-C", root, "status", "--porcelain=v2",
                          "--branch", "--untracked-files=all", "-z", NULL};
    int rc = git_run_capture_grow(root, args, &status, GD_SCRATCH_CAP, cx);
    bool ok = rc == 0 && status.len < GD_SCRATCH_CAP;
    u64 h = gd_hash(1469598103934665603ULL, status.data, status.len);
    usize pos = 0;
    while (ok && pos < status.len) {
        if (cx && atomic_load_explicit(&cx->cancel, memory_order_acquire)) {
            ok = false; break;
        }
        const char *row = status.data + pos;
        usize n = strnlen(row, status.len - pos);
        if (n == status.len - pos) { ok = false; break; }
        const char *path = NULL;
        i32 spaces = row[0] == '1' ? 8 : row[0] == '2' ? 9 : row[0] == 'u' ? 10 : 0;
        if (spaces) {
            const char *q = row;
            while (spaces && q < row + n) if (*q++ == ' ') spaces--;
            if (!spaces) path = q;
        } else if (n > 2 && row[0] == '?' && row[1] == ' ') path = row + 2;
        if (path) {
            char full[4096];
            int z = snprintf(full, sizeof full, "%s/%s", root, path);
            FileStamp stamp = {0};
            struct stat path_st;
            bool link = z > 0 && (usize)z < sizeof full && lstat(full, &path_st) == 0 && S_ISLNK(path_st.st_mode);
            bool have = !link && z > 0 && (usize)z < sizeof full && file_stamp_read(full, &stamp);
            h = gd_hash(h, &have, sizeof have);
            if (have) h = gd_hash(h, &stamp, sizeof stamp);
            else {
                /* Deleted paths and symlinks still have a stable revision. */
                struct stat st;
                if (z > 0 && (usize)z < sizeof full && lstat(full, &st) == 0) {
                    h = gd_hash(h, &st.st_ino, sizeof st.st_ino);
                    h = gd_hash(h, &st.st_mode, sizeof st.st_mode);
#ifdef __APPLE__
                    h = gd_hash(h, &st.st_mtimespec.tv_sec, sizeof st.st_mtimespec.tv_sec);
                    h = gd_hash(h, &st.st_mtimespec.tv_nsec, sizeof st.st_mtimespec.tv_nsec);
                    h = gd_hash(h, &st.st_ctimespec.tv_sec, sizeof st.st_ctimespec.tv_sec);
                    h = gd_hash(h, &st.st_ctimespec.tv_nsec, sizeof st.st_ctimespec.tv_nsec);
#else
                    h = gd_hash(h, &st.st_mtim.tv_sec, sizeof st.st_mtim.tv_sec);
                    h = gd_hash(h, &st.st_mtim.tv_nsec, sizeof st.st_mtim.tv_nsec);
                    h = gd_hash(h, &st.st_ctim.tv_sec, sizeof st.st_ctim.tv_sec);
                    h = gd_hash(h, &st.st_ctim.tv_nsec, sizeof st.st_ctim.tv_nsec);
#endif
                    char target[4096];
                    ssize_t k = readlink(full, target, sizeof target);
                    if (k > 0) h = gd_hash(h, target, (usize)k);
                }
            }
        }
        pos += n + 1;
        if (row[0] == '2' && pos < status.len) {
            usize old_n = strnlen(status.data + pos, status.len - pos);
            pos += old_n + 1;
        }
    }
    git_grow_free(&status);
    if (ok) *out = h;
    return ok;
}

static void gd_run_diff(const GdJob *job, GdResult *out, GitCancel *cx) {
    GitGrowBuf *buf = &out->buf;
    const char *root = job->root;
    char head[128];
    const char *a_head[] = {"git", "-C", root, "rev-parse", "--verify", "HEAD", NULL};
    bool have_head = git_run_capture_cancellable(root, a_head, head, sizeof head, cx) == 0;
    if (cx && atomic_load_explicit(&cx->cancel, memory_order_acquire)) return;
    if (have_head) {
        /* Raw NUL metadata is authoritative for names (including newline,
         * quoting, rename and mode/binary-only changes). Fixed prefixes make
         * the human patch independent of diff.noprefix and mnemonicPrefix. */
        const char *args[] = {"git", "--no-pager", "-C", root, "diff",
            "--no-ext-diff", "--no-textconv", "--no-color", "--raw", "-z",
            "--patch", "--src-prefix=a/", "--dst-prefix=b/", "HEAD", "--", NULL};
        int rc = git_run_capture_grow(root, args, buf, GD_DIFF_CAP - 1, cx);
        if (rc != 0) {
            out->failed = true;
            gd_append_note(buf, GD_DIFF_CAP + 128, "\n\\ could not read tracked changes\n");
        }
        if (buf->len >= GD_DIFF_CAP - 1) {
            out->truncated = true;
            gd_append_note(buf, GD_DIFF_CAP + 128, "\n\\ diff truncated: output limit reached\n");
            return;
        }
    }
    /* Without HEAD, diff each indexed AND untracked working file against the
     * empty file. --cached alone loses a newer unstaged edit in an unborn repo. */
    GitGrowBuf list = { .charge = gd_charge_transient };
    const char *tracked[] = {"git", "-C", root, "ls-files", "--cached", "--others",
                             "--exclude-standard", "-z", NULL};
    const char *untracked[] = {"git", "-C", root, "ls-files", "--others",
                               "--exclude-standard", "-z", NULL};
    int rc = git_run_capture_grow(root, have_head ? untracked : tracked,
                                  &list, GD_SCRATCH_CAP, cx);
    if (rc != 0) {
        out->failed = true;
        gd_append_note(buf, GD_DIFF_CAP + 128, "\n\\ could not list working files\n");
    }
    usize pos = 0;
    i32 files = 0;
    while (rc == 0 && pos < list.len && files < GD_UNTRACKED_MAX_FILES && buf->len < GD_DIFF_CAP - 1) {
        if (cx && atomic_load_explicit(&cx->cancel, memory_order_acquire)) break;
        const char *path = list.data + pos;
        usize n = strnlen(path, list.len - pos);
        if (n == list.len - pos) { out->truncated = true; break; }
        pos += n + 1;
        /* A staged deletion in an unborn repo contributes no working file. */
        if (!have_head) {
            char full[4096]; struct stat st;
            int k = snprintf(full, sizeof full, "%s/%s", root, path);
            if (k <= 0 || (usize)k >= sizeof full) { out->failed = true; continue; }
            if (lstat(full, &st) != 0) continue;
        }
        files++;
        const char *args[] = {"git", "--no-pager", "-C", root, "diff", "--no-index",
            "--no-ext-diff", "--no-textconv", "--no-color", "--raw", "-z", "--patch",
            "--src-prefix=a/", "--dst-prefix=b/", "--", "/dev/null", path, NULL};
        usize room = GD_DIFF_CAP - 1 - buf->len;
        usize limit = room < GD_SCRATCH_CAP ? room : GD_SCRATCH_CAP;
        usize before = buf->len;
        int code = git_run_capture_grow(root, args, buf, limit, cx);
        if (code != 0 && code != 1) {
            out->failed = true;
            gd_append_note(buf, GD_DIFF_CAP + 128, "\n\\ could not read working file\n");
        }
        if (buf->len - before >= limit) {
            out->truncated = true;
            gd_append_note(buf, GD_DIFF_CAP + 128, "\n\\ file diff truncated: output limit reached\n");
        }
    }
    if (pos < list.len || list.len >= GD_SCRATCH_CAP) {
        out->truncated = true;
        gd_append_note(buf, GD_DIFF_CAP + 128, "\n\\ more working files not shown: output or file limit reached\n");
    }
    git_grow_free(&list);
}

static void gd_parse_cancel(GdModel *v, bool transient, GitCancel *cx);
static void gd_parse(GdModel *v, bool transient);

static void gd_worker_drop(GdWorker *w) {
    if (atomic_fetch_sub_explicit(&w->refs, 1, memory_order_acq_rel) != 1) return;
    pthread_cond_destroy(&w->collected);
    pthread_mutex_destroy(&w->mtx);
    free(w);
}

static void *gd_worker(void *arg) {
    GdWorker *w = (GdWorker *)arg;
    GdJob job;
    pthread_mutex_lock(&w->mtx);
    job = w->job;
    pthread_mutex_unlock(&w->mtx);

    GdResult res;
    memset(&res, 0, sizeof res);
    snprintf(res.root, sizeof res.root, "%s", job.root);
    res.gen = job.gen;
    res.buf.charge = gd_charge_transient;
    bool probed = gd_fingerprint(job.root, &w->cancel, &res.fingerprint);
    res.unchanged = job.probe && probed && res.fingerprint == job.fingerprint;
    if (!res.unchanged) gd_run_diff(&job, &res, &w->cancel);
    res.cancelled = atomic_load_explicit(&w->cancel.cancel, memory_order_acquire);
    /* The buffer doubled its way up; what it holds now is what the dock will
     * keep for as long as it is open. */
    git_grow_trim(&res.buf);
    /* Parse here, on the worker, into arrays charged on the transient side.
     * A cancelled run's text is partial and is dropped at pickup, so it is
     * not worth parsing. */
    if (!res.cancelled && res.buf.len > 0) {
        res.model.text     = res.buf.data;
        res.model.text_len = res.buf.len;
        res.model.text_cap = res.buf.cap;
        res.model.cancel = &w->cancel;
        gd_parse(&res.model, true);
        res.cancelled = atomic_load_explicit(&w->cancel.cancel, memory_order_acquire);
    }

    pthread_mutex_lock(&w->mtx);
    if (!w->abandoned) {
        w->res = res; memset(&res, 0, sizeof res);
        w->ready = true;
        atomic_store_explicit(&w->published, true, memory_order_release);
        while (!w->abandoned && w->claimed_gen < job.gen)
            pthread_cond_wait(&w->collected, &w->mtx);
        if (w->abandoned && w->ready && w->res.gen == job.gen) {
            res = w->res; memset(&w->res, 0, sizeof w->res);
            w->ready = false;
            atomic_store_explicit(&w->published, false, memory_order_release);
        }
    }
    pthread_mutex_unlock(&w->mtx);
    gd_result_drop(&res);
    atomic_fetch_sub_explicit(&gd_live_workers, 1, memory_order_relaxed);
    gd_worker_drop(w);
    return NULL;
}

static bool gd_spawn(GdWorker *w, const GdJob *job) {
    if (!w) return false;
    pthread_mutex_lock(&w->mtx);
    if (w->live || w->abandoned) { pthread_mutex_unlock(&w->mtx); return false; }
    i32 busy = atomic_load_explicit(&gd_live_workers, memory_order_relaxed);
    do {
        if (busy >= GD_MAX_WORKERS) { pthread_mutex_unlock(&w->mtx); return false; }
    } while (!atomic_compare_exchange_weak_explicit(&gd_live_workers, &busy, busy + 1,
                memory_order_relaxed, memory_order_relaxed));
    w->job = *job;
    atomic_store_explicit(&w->cancel.cancel, false, memory_order_release);
    atomic_store_explicit(&w->cancel.child_pgid, 0, memory_order_release);
    atomic_fetch_add_explicit(&w->refs, 1, memory_order_relaxed);
    pthread_attr_t attr;
    bool attributes = pthread_attr_init(&attr) == 0;
    if (attributes && pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        pthread_attr_destroy(&attr); attributes = false;
    }
    bool ok = attributes && pthread_create(&w->tid, &attr, gd_worker, w) == 0;
    if (attributes) pthread_attr_destroy(&attr);
    w->live = ok;
    if (!ok) { atomic_fetch_sub(&gd_live_workers, 1); atomic_fetch_sub(&w->refs, 1); }
    pthread_mutex_unlock(&w->mtx);
    return ok;
}

/* =========================================================================
 * Parse — unified diff → rows
 * ========================================================================= */

static bool gd_starts(const char *p, usize n, const char *lit) {
    usize l = strlen(lit);
    return n >= l && memcmp(p, lit, l) == 0;
}

/* The parse's allocations go on whichever ledger side its thread owns: the
 * worker's are TRANSIENT (thread-safe), the pickup moves them to retained. */
static void gd_charge(bool transient, isize delta) {
    if (transient) mem_budget_transient_adjust(MEM_GIT, delta);
    else           mem_budget_adjust(MEM_GIT, delta);
}

static GdLine *gd_push(GdModel *v, bool transient) {
    if (v->line_count >= GD_MAX_LINES) return NULL;
    if (v->line_count == v->line_cap) {
        i32 next = v->line_cap ? v->line_cap * 2 : 1024;
        if (next > GD_MAX_LINES) next = GD_MAX_LINES;
        GdLine *g = realloc(v->lines, (usize)next * sizeof *g);
        if (!g) { v->failed = true; return NULL; }
        gd_charge(transient, (isize)(next - v->line_cap) * (isize)sizeof *g);
        v->lines = g; v->line_cap = next;
    }
    GdLine *l = &v->lines[v->line_count++];
    memset(l, 0, sizeof *l);
    l->old_no = l->new_no = -1;
    l->file = v->current_file;
    l->paint_off = UINT_MAX;
    return l;
}

/* Width-only counterpart of utf8_next_grapheme. The cluster interner is
 * main-thread owned, so a worker must not call the drawing decoder. Boundaries
 * and column widths still match it, including ZWJ/flags and combining marks. */
static u32 gd_measure_grapheme(const u8 *s, usize n, i32 *cols) {
    u32 base = 0xfffd;
    u32 used = utf8_decode(s, n, &base);
    if (!used) { *cols = 1; return 1; }
    u32 last = base, count = 1;
    bool emoji = utf8_is_emoji(base);
    while (count < UTF8_CLUSTER_MAX && used < n) {
        u32 cp = 0;
        u32 bytes = utf8_decode(s + used, n - used, &cp);
        if (!bytes) break;
        bool joins = utf8_is_joiner(cp) || last == 0x200d ||
            (count == 1 && base >= 0x1f1e6 && base <= 0x1f1ff && cp >= 0x1f1e6 && cp <= 0x1f1ff);
        if (!joins) break;
        if (cp == 0xfe0f || cp == 0x20e3 || utf8_is_emoji(cp)) emoji = true;
        used += bytes; count++; last = cp;
    }
    *cols = count > 1 && emoji ? 2 : utf8_char_width(base);
    return used;
}

/* Rendered width of a row, with tabs expanded. Drives the h-scroll bound. */
static i32 gd_row_cols(const char *s, u32 n) {
    i32 col = 0;
    for (u32 i = 0; i < n; ) {
        if (s[i] == '\t') {
            col += GD_TAB_WIDTH - (col % GD_TAB_WIDTH);
            i++;
            continue;
        }
        i32 width = 0;
        i += gd_measure_grapheme((const u8 *)s + i, n - i, &width);
        if (width > 0) col += width;
    }
    return col;
}

static GdFile *gd_file_push(GdModel *v, bool transient) {
    if (v->file_count == v->file_cap) {
        i32 next = v->file_cap ? v->file_cap * 2 : 32;
        GdFile *files = realloc(v->files, (usize)next * sizeof *files);
        if (!files) { v->failed = true; return NULL; }
        gd_charge(transient, (isize)(next - v->file_cap) * (isize)sizeof *files);
        v->files = files; v->file_cap = next;
    }
    GdFile *f = &v->files[v->file_count++];
    memset(f, 0, sizeof *f);
    return f;
}

/* Fallback for plain patch input: decode Git's C quoting in place. A decoded
 * name is never longer than its spelling, so existing row offsets stay valid.
 * The normal worker path uses --raw -z and needs no quoting inference. */
static void gd_patch_path(GdModel *v, const char *p, usize n, bool prefix,
                          u32 *off, u32 *len) {
    char *q = (char *)p;
    usize used = 0;
    if (n && p[0] == '"') {
        usize i = 1;
        while (i < n && p[i] != '"') {
            u8 c = (u8)p[i++];
            if (c == '\\' && i < n) {
                c = (u8)p[i++];
                if (c >= '0' && c <= '7') {
                    u32 oct = c - '0';
                    for (i32 j = 0; j < 2 && i < n && p[i] >= '0' && p[i] <= '7'; j++)
                        oct = oct * 8 + (u8)p[i++] - '0';
                    c = (u8)oct;
                } else {
                    switch (c) {
                    case 'a': c = '\a'; break; case 'b': c = '\b'; break;
                    case 't': c = '\t'; break; case 'n': c = '\n'; break;
                    case 'v': c = '\v'; break; case 'f': c = '\f'; break;
                    case 'r': c = '\r'; break; default: break;
                    }
                }
            }
            q[used++] = (char)c;
        }
    } else {
        while (used < n && p[used] != '\t') used++;
    }
    if (prefix && used >= 2 && (q[0] == 'a' || q[0] == 'b') && q[1] == '/') {
        q += 2; used -= 2;
    }
    *off = (u32)(q - v->text); *len = (u32)used;
}

static void gd_charge_retained(void *ctx, isize delta) { (void)ctx; mem_budget_adjust(MEM_GIT, delta); }

static bool gd_paint_row(GdModel *v, GdLine *l, const SynSpan *spans, i32 ns,
                          bool transient, GitCancel *cx) {
    /* Reserve points before spans. The points bound horizontal seeking even
     * when a pathological token stream exhausts the optional colour budget. */
    usize count_max = (usize)l->len / GD_POINT_STRIDE + 2;
    usize need = count_max * sizeof(GdPoint) + (usize)ns * sizeof(SynSpan);
    if (v->paint.len + need >= GD_PAINT_CAP) { ns = 0; need = count_max * sizeof(GdPoint); }
    if (v->paint.len + need >= GD_PAINT_CAP) { v->failed = true; return false; }
    v->paint.charge = transient ? gd_charge_transient : gd_charge_retained;
    if (!git_grow_reserve(&v->paint, need)) { v->failed = true; return false; }
    l->paint_off = (u32)v->paint.len;
    GdPoint *points = (GdPoint *)(v->paint.data + l->paint_off);
    const char *row = v->text + l->off;
    u32 col = 0, last = 0;
    l->point_count = 1;
    points[0] = (GdPoint){0, 0};
    for (u32 i = 0; i < l->len;) {
        if (i - last >= GD_POINT_STRIDE) {
            points[l->point_count++] = (GdPoint){i, col}; last = i;
            if (cx && atomic_load_explicit(&cx->cancel, memory_order_acquire)) return false;
        }
        if (row[i] == '\t') { col += GD_TAB_WIDTH - col % GD_TAB_WIDTH; i++; }
        else {
            i32 width = 0;
            i += gd_measure_grapheme((const u8 *)row + i, l->len - i, &width);
            if (width > 0) col += (u32)width;
        }
    }
    l->span_count = (u32)ns;
    l->cols = col;
    usize bytes = (usize)l->point_count * sizeof(GdPoint);
    if (ns) memcpy(v->paint.data + l->paint_off + bytes, spans, (usize)ns * sizeof *spans);
    v->paint.len += bytes + (usize)ns * sizeof *spans;
    v->paint.data[v->paint.len] = 0;
    if ((i32)col > v->max_cols) v->max_cols = (i32)col;
    return true;
}

static void gd_parse_cancel(GdModel *v, bool transient, GitCancel *cx) {
    v->current_file = -1;
    i32 raw_next = 0;
    i32 old_no = 0, new_no = 0;
    /* Scanner continuation, carried down the hunk and reset at each new one —
     * a hunk is a fragment, so nothing above its first line is known. */
    u32 syn_old = 0, syn_new = 0;
    SynSpan *syn_scratch = NULL;
    usize scratch_cap = 0;
    /* `--- a/x` is seen before `+++ b/x`; a deletion reports /dev/null on the
     * new side, so the old path is the only name that file has left. */
    u32 pend_old_off = 0, pend_old_len = 0;

    const char *T = v->text;
    usize n = v->text_len;
    usize i = 0;
    i32 max_no = 0;

    while (i < n) {
        if (cx && atomic_load_explicit(&cx->cancel, memory_order_acquire)) break;
        /* --raw -z metadata is a NUL-terminated header followed by one or
         * two raw path records. It precedes the corresponding patch group. */
        if (T[i] == ':') {
            usize z = i;
            while (z < n && T[z] != '\0' && T[z] != '\n') z++;
            if (z < n && T[z] == '\0') {
                char status = 0;
                for (usize t = i; t + 1 < z; t++) if (T[t] == ' ') status = T[t + 1];
                usize a = z + 1, b = a;
                while (b < n && T[b] != '\0') b++;
                if (b == n) { v->truncated = true; break; }
                GdFile *f = gd_file_push(v, transient);
                if (!f) break;
                f->status = (u8)status;
                f->old_path_off = f->path_off = (u32)a;
                f->old_path_len = f->path_len = (u32)(b - a);
                i = b + 1;
                if (status == 'R' || status == 'C') {
                    a = i; b = a;
                    while (b < n && T[b] != '\0') b++;
                    if (b == n) { v->truncated = true; break; }
                    f->path_off = (u32)a; f->path_len = (u32)(b - a); i = b + 1;
                }
                f->lang = (u8)syntax_lang_for_path(T + f->path_off, f->path_len);
                continue;
            }
        }
        if (T[i] == '\0') { i++; continue; }
        if (v->line_count >= GD_MAX_LINES - 1) { v->truncated = true; break; }
        usize e = i;
        while (e < n && T[e] != '\n') e++;
        const char *p = T + i;
        usize      len = e - i;
        if (len && p[len - 1] == '\r') len--;

        if (gd_starts(p, len, "diff --git ")) {
            GdFile *f;
            if (raw_next < v->file_count) {
                v->current_file = raw_next++;
                f = &v->files[v->current_file];
            } else {
                f = gd_file_push(v, transient);
                if (!f) break;
                v->current_file = v->file_count - 1;
                raw_next = v->file_count;
                /* Plain-patch fallback for mode/binary-only headers. */
                const char *q = p + 11, *end = p + len, *new_path = NULL;
                if (q < end && *q == '"') {
                    bool escaped = false;
                    for (const char *t = q + 1; t < end; t++) {
                        if (!escaped && *t == '"') { new_path = t + 1; break; }
                        if (!escaped && *t == '\\') escaped = true; else escaped = false;
                    }
                    if (new_path) while (new_path < end && *new_path == ' ') new_path++;
                } else {
                    for (const char *t = q; t + 3 < end; t++)
                        if (t[0] == ' ' && t[1] == 'b' && t[2] == '/') { new_path = t + 1; break; }
                }
                if (new_path && new_path < end)
                    gd_patch_path(v, new_path, (usize)(end - new_path), true, &f->path_off, &f->path_len);
                f->lang = (u8)syntax_lang_for_path(T + f->path_off, f->path_len);
            }
            pend_old_len = 0;
            /* A spacer, then the card. Both are ordinary rows so the whole
             * document keeps one row height and scrolling stays trivial. */
            if (v->current_file > 0) { GdLine *g = gd_push(v, transient); if (g) g->kind = GDL_GAP; }
            GdLine *h = gd_push(v, transient);
            if (h) h->kind = GDL_FILE;
        } else if (gd_starts(p, len, "--- ")) {
            const char *q = p + 4; usize m = len - 4;
            if (!(m == 9 && memcmp(q, "/dev/null", 9) == 0)) {
                gd_patch_path(v, q, m, true, &pend_old_off, &pend_old_len);
            }
        } else if (gd_starts(p, len, "+++ ")) {
            const char *q = p + 4; usize m = len - 4;
            GdFile *f = v->current_file >= 0 ? &v->files[v->current_file] : NULL;
            if (f && !f->status) {
                if (m == 9 && memcmp(q, "/dev/null", 9) == 0) {
                    f->path_off = pend_old_off; f->path_len = pend_old_len;
                } else {
                    gd_patch_path(v, q, m, true, &f->path_off, &f->path_len);
                }
                f->lang = (u8)syntax_lang_for_path(T + f->path_off, f->path_len);
            }
        } else if (gd_starts(p, len, "@@")) {
            /* "@@ -old,cnt +new,cnt @@ trailer" — the trailer is the enclosing
             * function, which is worth keeping; the numbers drive the gutter. */
            old_no = new_no = 0;
            syn_old = syn_new = 0;                  /* new fragment: nothing above is known */
            const char *q = p + 2;
            const char *end = p + len;
            while (q < end && *q != '-') q++;
            if (q < end) { q++; while (q < end && *q >= '0' && *q <= '9') old_no = old_no * 10 + (*q++ - '0'); }
            while (q < end && *q != '+') q++;
            if (q < end) { q++; while (q < end && *q >= '0' && *q <= '9') new_no = new_no * 10 + (*q++ - '0'); }
            GdLine *l = gd_push(v, transient);
            if (l) {
                l->kind = GDL_HUNK;
                /* Show only the trailer (the function context), not the raw
                 * @@ arithmetic — the gutter already carries the numbers. */
                const char *tr = NULL;
                for (const char *s = p + 2; s + 1 < end; s++) {
                    if (s[0] == '@' && s[1] == '@') { tr = s + 2; break; }
                }
                while (tr && tr < end && *tr == ' ') tr++;
                l->off = (u32)((tr ? tr : end) - T);
                l->len = (u32)(tr ? (usize)(end - tr) : 0);
            }
        } else if (gd_starts(p, len, "index ") ||
                   gd_starts(p, len, "new file mode ") ||
                   gd_starts(p, len, "deleted file mode ") ||
                   gd_starts(p, len, "old mode ") ||
                   gd_starts(p, len, "new mode ") ||
                   gd_starts(p, len, "similarity index ") ||
                   gd_starts(p, len, "rename from ") ||
                   gd_starts(p, len, "rename to ") ||
                   gd_starts(p, len, "copy from ") ||
                   gd_starts(p, len, "copy to ")) {
            /* Plumbing. The card header says everything these rows do. */
        } else if (gd_starts(p, len, "Binary files ") || gd_starts(p, len, "\\ ")) {
            GdLine *l = gd_push(v, transient);
            if (l) { l->kind = GDL_NOTE; l->off = (u32)(p - T); l->len = (u32)len; }
        } else if (len && (p[0] == '+' || p[0] == '-' || p[0] == ' ')) {
            GdLine *l = gd_push(v, transient);
            if (!l) break;
            l->off = (u32)(p - T) + 1;          /* drop the marker column */
            l->len = (u32)(len - 1);
            GdFile *f = v->current_file >= 0 ? &v->files[v->current_file] : NULL;
            if (p[0] == '+') {
                l->kind = GDL_ADD; l->new_no = new_no++;
                if (f) f->added++;
                v->total_added++;
            } else if (p[0] == '-') {
                l->kind = GDL_DEL; l->old_no = old_no++;
                if (f) f->removed++;
                v->total_removed++;
            } else {
                l->kind = GDL_CTX; l->old_no = old_no++; l->new_no = new_no++;
            }
            /* Resolve this row's scanner state, then advance it for the next.
             * The guess only fires when nothing is carried in — it is what
             * makes a hunk that opens inside a block comment paint as one. */
            GdFile *lf = (l->file >= 0) ? &v->files[l->file] : NULL;
            SynLang lang = lf ? (SynLang)lf->lang : SYN_LANG_NONE;
            i32 ns = 0;
            u32 state = l->kind == GDL_DEL ? syn_old : syn_new;
            if (!state) state = syntax_guess_state(lang, T + l->off, l->len);
            l->syn_state = state;
            if (lang != SYN_LANG_NONE) {
                usize want = (usize)l->len / 2 + 1;
                if (want < 32) want = 32;
                if (want > GD_SPAN_LIMIT) want = GD_SPAN_LIMIT;
                if (scratch_cap < want) {
                    SynSpan *fresh = realloc(syn_scratch, want * sizeof *fresh);
                    if (!fresh) { v->failed = true; break; }
                    gd_charge(transient, (isize)(want - scratch_cap) * (isize)sizeof *fresh);
                    syn_scratch = fresh; scratch_cap = want;
                }
                ns = syntax_scan(lang, T + l->off, l->len, &state, syn_scratch, (i32)scratch_cap);
            }
            if (l->kind == GDL_DEL) syn_old = state;
            else if (l->kind == GDL_ADD) syn_new = state;
            else { /* Context advances both sides independently. */
                if (syn_old != syn_new)
                    syntax_advance_state(lang, T + l->off, l->len, &syn_old);
                else syn_old = state;
                syn_new = state;
            }
            if (!gd_paint_row(v, l, syn_scratch, ns, transient, cx)) break;
            if (l->old_no > max_no) max_no = l->old_no;
            if (l->new_no > max_no) max_no = l->new_no;
        }

        i = (e < n) ? e + 1 : n;
    }

    for (i32 row = 0; row < v->line_count; row++) {
        if (cx && atomic_load_explicit(&cx->cancel, memory_order_acquire)) break;
        GdLine *l = &v->lines[row];
        if (l->paint_off == UINT_MAX && l->len && !gd_paint_row(v, l, NULL, 0, transient, cx)) break;
    }
    i32 digits = 1;
    for (i32 m = max_no; m >= 10; m /= 10) digits++;
    v->gutter_digits = digits;
    free(syn_scratch);
    gd_charge(transient, -(isize)scratch_cap * (isize)sizeof *syn_scratch);
    git_grow_trim(&v->paint);

    /* The arrays doubled their way up and the parse is over: hand back the
     * headroom, so what the dock retains is proportional to the diff it
     * shows. A failed shrink keeps the larger block — nothing is lost,
     * only not returned. */
    if (v->lines && v->line_count > 0 && v->line_count < v->line_cap) {
        GdLine *g = realloc(v->lines, (usize)v->line_count * sizeof *g);
        if (g) {
            gd_charge(transient, -(isize)(v->line_cap - v->line_count) * (isize)sizeof *g);
            v->lines = g; v->line_cap = v->line_count;
        }
    }
    if (v->files && v->file_count > 0 && v->file_count < v->file_cap) {
        GdFile *f = realloc(v->files, (usize)v->file_count * sizeof *f);
        if (f) {
            gd_charge(transient, -(isize)(v->file_cap - v->file_count) * (isize)sizeof *f);
            v->files = f; v->file_cap = v->file_count;
        }
    }
}


static void gd_parse(GdModel *v, bool transient) { gd_parse_cancel(v, transient, v->cancel); }

static void gd_clear(GitDiffView *v) {
    mem_budget_adjust(MEM_GIT, -(isize)gd_retained_bytes(v));
    free(v->text);  v->text = NULL;  v->text_len = 0;  v->text_cap = 0;
    free(v->lines); v->lines = NULL; v->line_count = 0; v->line_cap = 0;
    free(v->files); v->files = NULL; v->file_count = 0; v->file_cap = 0;
    free(v->paint_cache); v->paint_cache = NULL; v->paint_cache_cap = 0;
    v->failed = v->truncated = false;
    v->total_added = v->total_removed = 0;
    v->gutter_digits = 1;
    v->max_cols = 0;
    v->scroll_px = v->hscroll_px = 0.0f;
    /* `loading` is the gate on every re-run (git_diff_poll skips the
     * save-refresh while it is set), and nothing else ever lowers it on the
     * paths that clear the view. Leaving it up meant a dock that lost its
     * result sat on "reading working tree…" with no way back. */
    v->loading = false;
}

/* Take the worker's published result, joining the thread that produced it.
 * `ready` means the worker is one instruction from returning, so the join is
 * not a wait — the reasoning git_status.c's pickup relies on too. Returns
 * false when nothing has been published; never blocks on a run in flight. */
static bool gd_take_result(GdWorker *w, GdResult *out) {
    if (!atomic_load_explicit(&w->published, memory_order_acquire)) return false;
    pthread_mutex_lock(&w->mtx);
    bool ready = w->ready;
    if (ready) {
        *out = w->res;
        memset(&w->res, 0, sizeof w->res);
        w->ready = false; w->live = false;
        if (out->gen > w->claimed_gen) w->claimed_gen = out->gen;
        atomic_store_explicit(&w->published, false, memory_order_release);
        pthread_cond_broadcast(&w->collected);
    }
    pthread_mutex_unlock(&w->mtx);
    return ready;
}

/* Everything a closed dock has no use for. The diff text is the whole
 * `git diff` output, row metadata and paint cache, so a dock opened once
 * would otherwise hold that for the rest of the session; every open re-runs
 * the job from scratch (gd_clear + gd_request below), so none of it is state
 * a later open would want back.
 *
 * A run still IN FLIGHT is asked to stop — it is a `git diff` plus up to
 * GD_UNTRACKED_MAX_FILES further forks producing a result no one will look
 * at. It is NOT joined here: a cancel takes effect when the child notices
 * the signal, and the main thread has no business waiting for that. The
 * worker lands its (partial, `cancelled`) result in the slot and
 * git_diff_poll reaps it on a later frame, open or not — that pickup is what
 * lets the next open start a run, since gd_spawn refuses while `live` is up.
 * A result already published is taken now, because that join is free. */
static void gd_release(GitDiffView *v) {
    gd_clear(v);

    GdWorker *w = (GdWorker *)v->worker;
    if (!w) return;
    GdResult res;
    if (gd_take_result(w, &res)) {
        gd_result_drop(&res);
        return;
    }
    pthread_mutex_lock(&w->mtx);
    bool live = w->live;
    pthread_mutex_unlock(&w->mtx);
    if (live) git_cancel_request(&w->cancel);
}

/* =========================================================================
 * View state
 * ========================================================================= */

bool git_diff_is_open(const AppState *app) {
    return app && app->git_diff.open;
}

void git_diff_close(AppState *app) {
    if (!app) return;
    app->git_diff.open = false;
    gd_release(&app->git_diff);
}

/* The repo a tab's status sample belongs to, as the dock binds it. */
static const char *gd_bound_root(const Tab *tab) {
    if (!tab) return NULL;
    if (tab->git_status.repo_root && tab->git_status.repo_root[0])
        return tab->git_status.repo_root;
    return tab->git_status.cached_cwd;
}

/* Kick off a whole-worktree diff for the repo the view is bound to. */
static void gd_request_kind(AppState *app, bool probe) {
    GitDiffView *v = &app->git_diff;
    GdWorker *w = gd_worker_state(v);
    GdJob job;
    memset(&job, 0, sizeof job);
    snprintf(job.root, sizeof job.root, "%s", v->root);
    job.probe = probe;
    if (w) { job.fingerprint = w->fingerprint; w->last_probe = platform_time_sec(); }
    if (w) {
        /* A new generation whether or not the spawn goes through: a run in
         * flight now answers a request nobody is waiting for, and the pickup
         * discards it by number and re-requests. */
        pthread_mutex_lock(&w->mtx);
        job.gen = ++w->gen;
        pthread_mutex_unlock(&w->mtx);
    }
    if (gd_spawn(w, &job)) { v->loading = true; return; }

    /* Refused. A run already in flight for THIS view still lands, and poll
     * re-requests for whatever root the view has moved to — so the dock
     * really is loading. A run that a close cancelled is in this state too;
     * it lands as a stale generation and the re-request is what starts the
     * fresh one. No worker at all (calloc / mutex_init / pthread_create
     * failed) means nothing will ever arrive: lower the gate, or the dock
     * reads "reading working tree…" for the rest of the session with its
     * save-refresh gated behind it. */
    bool in_flight = false;
    if (w) {
        pthread_mutex_lock(&w->mtx);
        in_flight = w->live;
        pthread_mutex_unlock(&w->mtx);
    }
    v->loading = in_flight;
    if (in_flight) git_cancel_request(&w->cancel);
    if (!in_flight) v->failed = true;
}

static void gd_request(AppState *app) { gd_request_kind(app, false); }

void git_diff_toggle(AppState *app) {
    if (!app) return;
    GitDiffView *v = &app->git_diff;
    if (v->open) { git_diff_close(app); return; }

    Tab *tab = app_active_tab(app);
    if (!tab || tab->kind != TAB_TERMINAL) return;
    if (!tab->git_status.valid || !tab->git_status.is_repo) return;
    /* The worktree root when the sampler resolved one, the sampled cwd
     * otherwise (bare repo / inside .git). Binding to the root is what keeps
     * a cd deeper into the same project from counting as a different repo —
     * see the rebind in git_diff_poll. */
    const char *cwd = gd_bound_root(tab);
    if (!cwd || !cwd[0]) return;

    snprintf(v->root, sizeof v->root, "%s", cwd);
    v->open         = true;
    v->seen_added   = tab->git_status.added;
    v->seen_removed = tab->git_status.removed;
    gd_clear(v);
    gd_request(app);
}

void git_diff_free(GitDiffView *v) {
    if (!v) return;
    gd_clear(v);
    GdWorker *w = v->worker; v->worker = NULL;
    if (!w) return;
    git_cancel_request(&w->cancel);
    pthread_mutex_lock(&w->mtx);
    w->abandoned = true;
    pthread_cond_broadcast(&w->collected);
    pthread_mutex_unlock(&w->mtx);
    gd_worker_drop(w); /* worker owns cleanup, including an unclaimed model */
}

/* =========================================================================
 * Poll
 * ========================================================================= */

bool git_diff_poll(AppState *app) {
    if (!app) return false;
    GitDiffView *v = &app->git_diff;

    /* The dock is bound to one repo, and its only handle is the status bar's
     * git segment. If the active tab has moved off that repo — a different
     * tab, a cd out of the worktree, a pane that is no longer a terminal — or
     * the segment itself has been switched off, there is nothing left to point
     * at and nothing left to close it with. Checked here rather than in the
     * renderer so it holds with the status bar hidden too. */
    if (v->open) {
        Tab *tab = app_active_tab(app);
        if (!tab || tab->kind != TAB_TERMINAL || !tab->git_status.valid ||
            !tab->git_status.is_repo ||
            !app->config.show_git_badge || !app->config.show_status_bar) {
            git_diff_close(app);
            return true;
        }
        /* The sampler follows the focused pane, so a split whose panes sit in
         * different repos moves the whole bar — badge and dock together — when
         * focus moves. Rebind rather than leave the dock showing repo A's rows
         * under repo B's figures. Keyed on the worktree root, so cd-ing around
         * inside one project is not a rebind; the rows stay up until the new
         * diff lands, exactly as they do for a save-triggered re-run. */
        const char *root = gd_bound_root(tab);
        if (root && root[0] && strcmp(root, v->root) != 0) {
            snprintf(v->root, sizeof v->root, "%s", root);
            v->seen_added   = tab->git_status.added;
            v->seen_removed = tab->git_status.removed;
            v->scroll_px    = 0.0f;
            v->hscroll_px   = 0.0f;
            gd_clear(v);
            gd_request(app);
            return true;
        }
        /* Counts are an early invalidation hint. A bounded worker probe also
         * catches same-size/same-count edits and index changes. */
        if (!v->loading && (tab->git_status.added   != v->seen_added ||
                            tab->git_status.removed != v->seen_removed)) {
            v->seen_added   = tab->git_status.added;
            v->seen_removed = tab->git_status.removed;
            gd_request_kind(app, true);
        } else if (!v->loading) {
            GdWorker *state = (GdWorker *)v->worker;
            if (state && platform_time_sec() - state->last_probe >= GD_REFRESH_SEC)
                gd_request_kind(app, true);
        }
    }

    GdWorker *w = (GdWorker *)v->worker;
    if (!w) { v->loading = false; return false; }

    /* Reap whatever the worker has published — WHETHER OR NOT the dock is
     * open or waiting. A run that a close cancelled still lands here, and
     * until it is taken its text is held and `live` blocks the next open's
     * spawn. Behind the atomic this is free on the frames where nothing has
     * landed, which is nearly all of them. */
    GdResult res;
    if (!gd_take_result(w, &res)) return false;

    /* Taken only if it answers the CURRENT request: not cancelled, the
     * newest generation, for the repo the view is bound to (the root compare
     * is the belt — a rebind takes a generation too). Anything else is
     * dropped, and if the dock is still waiting the run it wants is started
     * now — the refused spawn in gd_request left `loading` up for exactly
     * this. */
    u32 want_gen;
    pthread_mutex_lock(&w->mtx);
    want_gen = w->gen;
    pthread_mutex_unlock(&w->mtx);
    bool ours = v->open && v->loading && !res.cancelled &&
                res.gen == want_gen && strcmp(res.root, v->root) == 0;
    if (!ours) {
        gd_result_drop(&res);
        if (v->open && v->loading) {
            v->loading = false;
            gd_request(app);
        }
        return false;
    }

    v->loading = false;
    w->fingerprint = res.fingerprint;
    if (res.unchanged) { gd_result_drop(&res); return false; }
    /* Keep the reading position across a refresh: the diff is re-run every
     * time the user saves a file, and snapping back to the top each time would
     * make the dock unusable while editing. */
    f32 keep_v = v->scroll_px, keep_h = v->hscroll_px;
    gd_clear(v);
    if (res.buf.len > 0) {
        /* Handoff: the parsed model — text, rows, files — leaves the worker's
         * transient side and lands on the retained side in the same step, so
         * the ledger never shows it twice or not at all. From here the view
         * owns all of it. No parse here: that was the worker's. */
        const GdModel *m = &res.model;
        usize bytes = gd_model_bytes(m);
        mem_budget_transient_adjust(MEM_GIT, -(isize)bytes);
        mem_budget_adjust(MEM_GIT, (isize)bytes);
        v->text          = m->text;
        v->text_len      = m->text_len;
        v->text_cap      = m->text_cap;
        v->lines         = m->lines;
        v->line_count    = m->line_count;
        v->line_cap      = m->line_cap;
        v->files         = m->files;
        v->file_count    = m->file_count;
        v->file_cap      = m->file_cap;
        v->total_added   = m->total_added;
        v->total_removed = m->total_removed;
        v->gutter_digits = m->gutter_digits;
        v->max_cols      = m->max_cols;
        v->paint_cache   = m->paint.data;
        v->paint_cache_cap = m->paint.cap;
        v->failed = res.failed || m->failed;
        v->truncated = res.truncated || m->truncated;
        v->scroll_px = keep_v;
        v->hscroll_px = keep_h;
    } else {
        v->failed = res.failed;
        v->truncated = res.truncated;
        gd_result_drop(&res);
    }
    return true;
}

/* =========================================================================
 * Layout — computed once, shared by render and every hit test.
 * ========================================================================= */

/* Divider grab width. Matches the file-browser viewer's handle so the two
 * right-edge drags feel the same. */
#define GD_GRIP_PT     5.0f
#define GD_PAD_PT      12.0f   /* dock inner margin */
#define GD_SCROLLBAR_PT 4.0f

typedef struct {
    f32 dpi, cw, glyph_h, row_h;
    f32 px, py, pw, ph;          /* the dock */
    f32 grip_x, grip_w;
    f32 gutter_w;                /* line-number column, incl. its rule */
    f32 body_x, body_w;          /* where row text lives */
    f32 content_h;
    f32 max_scroll, max_hscroll;
} GdLayout;

static void gd_layout(AppState *app, GdLayout *L) {
    const GitDiffView *v = &app->git_diff;
    f32 dpi = app->dpi_scale;
    L->dpi = dpi;
    /* The dock paints at UI scale, not the terminal's. At a 16pt terminal font
     * a 460pt sidebar is ~24 columns wide — narrower than most diff rows, so
     * everything folded. At UI scale the same sidebar holds ~57. */
    L->cw = 8.0f * dpi;
    f32 fcw = app->renderer.font.cell_width;
    f32 fch = app->renderer.font.cell_height;
    L->glyph_h = (fcw > 0.0f) ? L->cw * (fch / fcw) : 16.0f * dpi;
    L->row_h   = floorf(L->glyph_h + 3.0f * dpi);
    if (L->row_h < 8.0f * dpi) L->row_h = 8.0f * dpi;

    f32 w = app_git_diff_dock_w(app);
    L->pw = w;
    L->px = (f32)app->fb_width - w;
    L->py = app->tab_bar_height;
    L->ph = (f32)app->fb_height - L->py - app->status_bar_height;

    L->grip_w = GD_GRIP_PT * dpi;
    L->grip_x = L->px - L->grip_w * 0.5f;

    /* Two numbers side by side, a space between them, and breathing room on
     * both sides of the pair. */
    i32 digits = v->gutter_digits > 0 ? v->gutter_digits : 1;
    L->gutter_w = (f32)(digits * 2 + 1) * L->cw + GD_PAD_PT * dpi;
    L->body_x = L->px + L->gutter_w + 8.0f * dpi;
    L->body_w = L->px + L->pw - L->body_x - GD_PAD_PT * dpi;
    if (L->body_w < L->cw) L->body_w = L->cw;

    L->content_h = (f32)v->line_count * L->row_h + GD_PAD_PT * dpi * 2.0f;
    L->max_scroll = L->content_h - L->ph;
    if (L->max_scroll < 0.0f) L->max_scroll = 0.0f;
    L->max_hscroll = (f32)v->max_cols * L->cw - L->body_w;
    if (L->max_hscroll < 0.0f) L->max_hscroll = 0.0f;
}

static void gd_clamp(GitDiffView *v, const GdLayout *L) {
    if (v->scroll_px > L->max_scroll) v->scroll_px = L->max_scroll;
    if (v->scroll_px < 0.0f) v->scroll_px = 0.0f;
    if (v->hscroll_px > L->max_hscroll) v->hscroll_px = L->max_hscroll;
    if (v->hscroll_px < 0.0f) v->hscroll_px = 0.0f;
}

/* =========================================================================
 * Render
 * ========================================================================= */

typedef struct {
    Color add_fg, add_bg, add_bar;
    Color del_fg, del_bg, del_bar;
    Color ctx_fg, gutter_fg, hunk_fg;
    Color card_bg, card_fg, rule, accent;
} GdPalette;

/* Diff green/red are semantic — no field in ChromePalette carries them — so
 * they come from the THEME's own ansi slots rather than a literal. The bright
 * pair reads on dark backgrounds; on a light theme the normal pair is the one
 * with enough contrast against white. */
static GdPalette gd_palette(const Theme *t) {
    ChromePalette cp = chrome_palette_for(t);
    GdPalette p;
    Color green = cp.is_light ? t->ansi[2] : t->ansi[10];
    Color red   = cp.is_light ? t->ansi[1] : t->ansi[9];

    p.add_fg = green; p.add_fg.a = 1.0f;
    p.del_fg = red;   p.del_fg.a = 1.0f;
    p.add_bg = green; p.add_bg.a = cp.is_light ? 0.13f : 0.10f;
    p.del_bg = red;   p.del_bg.a = cp.is_light ? 0.13f : 0.10f;
    p.add_bar = green; p.add_bar.a = 0.85f;
    p.del_bar = red;   p.del_bar.a = 0.85f;

    p.ctx_fg = t->fg;    p.ctx_fg.a = 0.78f;
    p.gutter_fg = t->fg; p.gutter_fg.a = 0.32f;
    p.hunk_fg = t->fg;   p.hunk_fg.a = 0.42f;
    p.card_bg = cp.surface_raised; p.card_bg.a = 1.0f;
    p.card_fg = t->fg;   p.card_fg.a = 0.96f;
    p.rule    = cp.divider_subtle;
    p.accent  = theme_ui_accent(t);
    return p;
}

static void gd_text(Renderer *r, const char *s, usize n, f32 x, f32 y,
                    Color fg, f32 adv, f32 clip_l, f32 clip_r) {
    const u8 *p = (const u8 *)s;
    const u8 *end = p + n;
    f32 prior_x = x;
    while (p < end) {
        u32 cp = 0;
        i32 width = 0;
        u32 c = utf8_next_grapheme(p, (usize)(end - p), &cp, &width);
        if (!c) { c = 1; cp = 0xfffd; width = 1; }
        if (c > 4 && !utf8_is_cluster(cp)) gd_measure_grapheme(p, (usize)(end - p), &width);
        f32 gx = width == 0 ? prior_x : x;
        if (cp >= 32 && gx + adv * (width > 0 ? width : 1) > clip_l && gx < clip_r)
            renderer_draw_glyph(r, cp, gx, y, fg, (Color){0,0,0,0});
        if (width > 0) { prior_x = x; x += adv * width; }
        p += c;
        if (x >= clip_r) break;
    }
}

/* Draw only the horizontal viewport. Checkpoint lookup is logarithmic;
 * UTF-8 decoding after it is bounded by one checkpoint plus visible columns.
 * Syntax byte offsets refer to the original row, so tabs need no copied or
 * expanded buffer and a 4 KiB/8 MiB line has the same per-frame work. */
static void gd_draw_row(Renderer *r, const GitDiffView *v, const GdLine *l,
                        const Theme *theme, Color base, f32 origin, f32 y,
                        f32 cw, f32 clip_l, f32 clip_r) {
    const char *row = v->text + l->off;
    const GdPoint *points = NULL;
    const SynSpan *spans = NULL;
    u32 off = 0, col = 0, si = 0;
    f32 prior_x = origin;
    if (v->paint_cache && l->paint_off != UINT_MAX && l->point_count) {
        points = (const GdPoint *)((const char *)v->paint_cache + l->paint_off);
        spans = (const SynSpan *)(points + l->point_count);
        u32 target = clip_l > origin ? (u32)((clip_l - origin) / cw) : 0;
        u32 lo = 0, hi = l->point_count;
        while (lo + 1 < hi) {
            u32 mid = lo + (hi - lo) / 2;
            if (points[mid].col <= target) lo = mid; else hi = mid;
        }
        /* Checkpoints always begin at a grapheme boundary. */
        off = points[lo].off; col = points[lo].col;
        prior_x = origin + (f32)(col > 0 ? col - 1 : 0) * cw;
        u32 a = 0, b = l->span_count;
        while (a < b) {
            u32 mid = a + (b - a) / 2;
            if (spans[mid].off + spans[mid].len <= off) a = mid + 1; else b = mid;
        }
        si = a;
    }
    while (off < l->len) {
        f32 x = origin + (f32)col * cw;
        if (x >= clip_r) break;
        if (row[off] == '\t') { col += GD_TAB_WIDTH - col % GD_TAB_WIDTH; off++; continue; }
        u32 cp = 0;
        i32 width = 0;
        u32 bytes = utf8_next_grapheme((const u8 *)row + off, l->len - off, &cp, &width);
        if (!bytes) { bytes = 1; cp = 0xfffd; width = 1; }
        /* A full global cluster table degrades the glyph to its base, but the
         * model/viewport must keep the intended cluster column advance. */
        if (bytes > 4 && !utf8_is_cluster(cp)) gd_measure_grapheme((const u8 *)row + off, l->len - off, &width);
        while (spans && si < l->span_count && spans[si].off + spans[si].len <= off) si++;
        Color color = base;
        if (spans && si < l->span_count && spans[si].off <= off)
            color = syntax_color(theme, (SynKind)spans[si].kind);
        f32 gx = width == 0 ? prior_x : x;
        if (cp >= 32 && gx + cw * (width > 0 ? width : 1) > clip_l && gx < clip_r)
            renderer_draw_glyph(r, cp, gx, y, color, (Color){0,0,0,0});
        if (width > 0) { prior_x = x; col += (u32)width; }
        off += bytes;
    }
}

/* The file card: an accent spine, the path, and the ±counts as a pair of
 * figures on the right. Drawn both inline and, for whichever file the top of
 * the viewport is inside, pinned to the dock's top edge. */
static void gd_draw_card(AppState *app, const GdLayout *L, const GdPalette *P,
                         const GdFile *f, f32 y, bool pinned) {
    Renderer *r = &app->renderer;
    GitDiffView *v = &app->git_diff;
    f32 dpi = L->dpi;
    f32 h = L->row_h;

    renderer_draw_rect(r, L->px, y, L->pw, h, P->card_bg);
    Color spine = P->accent; spine.a = 0.9f;
    renderer_draw_rect(r, L->px, y, 3.0f * dpi, h, spine);
    if (pinned) {
        /* Only the pinned copy gets a bottom rule — it is floating over rows
         * that scroll under it and needs the edge; inline cards sit in their
         * own spacer and would just gain a stray line. */
        renderer_draw_rect(r, L->px, y + h - fmaxf(1.0f, dpi), L->pw,
                           fmaxf(1.0f, dpi), P->rule);
    }
    renderer_flush_rects(r);

    f32 ty = y + (h - L->glyph_h) * 0.5f;
    f32 tx = L->px + 3.0f * dpi + 9.0f * dpi;

    /* Counts first — the path gets whatever width is left, elided from the
     * left so the file name (the part that identifies it) always survives. */
    char cnt[48]; usize cl = 0;
    if (f->added && f->removed)
        cl = (usize)snprintf(cnt, sizeof cnt, "+%d  -%d", f->added, f->removed);
    else if (f->added)   cl = (usize)snprintf(cnt, sizeof cnt, "+%d", f->added);
    else if (f->removed) cl = (usize)snprintf(cnt, sizeof cnt, "-%d", f->removed);
    f32 cnt_w = (f32)cl * L->cw;
    f32 cnt_x = L->px + L->pw - GD_PAD_PT * dpi - cnt_w;

    const char *raw = v->text + f->path_off;
    /* Keep the raw NUL metadata as file identity. Escape controls only for
     * the single-line card label so newline/tab names remain distinguishable. */
    char label[4096]; usize label_len = 0;
    usize start = f->path_len > sizeof label / 2 ? f->path_len - sizeof label / 2 : 0;
    while (start < f->path_len && ((u8)raw[start] & 0xc0) == 0x80) start++;
    for (usize j = start; j < f->path_len && label_len + 5 < sizeof label; j++) {
        u8 c = (u8)raw[j];
        if (c == '\n' || c == '\t' || c == '\r' || c == '\\') {
            label[label_len++] = '\\';
            label[label_len++] = c == '\n' ? 'n' : c == '\t' ? 't' : c == '\r' ? 'r' : '\\';
        } else if (c < 32 || c == 127) {
            int k = snprintf(label + label_len, sizeof label - label_len, "\\x%02x", c);
            label_len += (usize)k;
        } else label[label_len++] = (char)c;
    }
    label[label_len] = 0;
    const char *path = label;
    i32 plen = (i32)label_len;
    i32 pchars = gd_row_cols(path, (u32)plen);
    i32 budget = (i32)((cnt_x - tx - 10.0f * dpi) / L->cw);
    if (budget < 6) budget = 6;
    if (pchars > budget) {
        i32 want = budget - 1;
        const char *q = path + plen;
        while (q > path && want > 0) {
            q--;
            while (q > path && ((u8)*q & 0xC0) == 0x80) q--;
            u32 cp = 0;
            utf8_decode((const u8 *)q, (usize)(path + plen - q), &cp);
            i32 width = utf8_char_width(cp);
            if (width > 0) want -= width;
        }
        gd_text(r, "…", 3, tx, ty, P->card_fg, L->cw, L->px, L->px + L->pw);
        gd_text(r, q, (usize)(path + plen - q), tx + L->cw, ty, P->card_fg,
                L->cw, L->px, cnt_x);
    } else {
        gd_text(r, path, (usize)plen, tx, ty, P->card_fg, L->cw, L->px, cnt_x);
    }

    if (cl) {
        /* Split so "+12" is green and "-3" red without building two strings. */
        const char *sep = strstr(cnt, "  ");
        if (sep) {
            gd_text(r, cnt, (usize)(sep - cnt), cnt_x, ty, P->add_fg, L->cw,
                    L->px, L->px + L->pw);
            gd_text(r, sep + 2, strlen(sep + 2),
                    cnt_x + (f32)(sep - cnt + 2) * L->cw, ty, P->del_fg, L->cw,
                    L->px, L->px + L->pw);
        } else {
            gd_text(r, cnt, cl, cnt_x, ty, cnt[0] == '+' ? P->add_fg : P->del_fg,
                    L->cw, L->px, L->px + L->pw);
        }
    }
    renderer_flush_glyphs(r);
}

void git_diff_render(AppState *app) {
    if (!app || !app->git_diff.open) return;
    GitDiffView *v = &app->git_diff;
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme ? app->config.theme : &THEME_Liu;

    GdLayout L;
    gd_layout(app, &L);
    gd_clamp(v, &L);
    GdPalette P = gd_palette(t);
    ChromePalette cp = chrome_palette_for(t);

    renderer_set_ui_scale(r, L.cw, 16.0f * L.dpi);

    /* Flat panel, no scrim and no drop shadow: this is docked chrome sitting
     * beside the terminal, not something floating over it. The fill is the
     * theme's own background rather than a raised chrome surface, so the dock
     * continues the terminal's plane instead of drawing a second shade of grey
     * next to it — the file cards and row tints are what carry the structure.
     *
     * Scaled by the window opacity like every other chrome surface: the dock
     * can be half the window wide, and a fully opaque fill over that much of
     * it punched a rectangular hole through the window's translucency/blur. */
    Color body = t->bg; body.a = app->config.opacity;
    renderer_draw_rect(r, L.px, L.py, L.pw, L.ph, body);
    /* Left edge doubles as the resize handle — brightens on hover/drag. */
    bool grip_hot = v->resizing ||
                    ui_hover_hit(L.grip_x, L.py, L.grip_w, L.ph,
                                 app->hover_x, app->hover_y);
    Color edge = grip_hot ? (Color){P.accent.r, P.accent.g, P.accent.b, 0.95f}
                          : cp.divider_strong;
    renderer_draw_rect(r, L.px, L.py, grip_hot ? 2.0f * L.dpi : 1.0f * L.dpi,
                       L.ph, edge);
    renderer_flush_rects(r);

    if (v->line_count == 0) {
        Color c = t->fg; c.a = 0.40f;
        const GdWorker *state = (const GdWorker *)v->worker;
        bool initial_load = v->loading && (!state || !state->job.probe);
        const char *msg = initial_load ? "reading working tree…" :
                          v->failed ? "could not read working tree" : "no changes";
        f32 mw = (f32)utf8_len((const u8 *)msg, strlen(msg)) * L.cw;
        gd_text(r, msg, strlen(msg), L.px + (L.pw - mw) * 0.5f,
                L.py + L.ph * 0.42f, c, L.cw, L.px, L.px + L.pw);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
        return;
    }

    f32 top = L.py + GD_PAD_PT * L.dpi;
    i32 first = (i32)((v->scroll_px) / L.row_h);
    if (first < 0) first = 0;
    i32 last = first + (i32)(L.ph / L.row_h) + 2;
    if (last > v->line_count) last = v->line_count;

    renderer_push_scissor(r, L.px, L.py, L.pw, L.ph);

    /* --- pass 1: row backgrounds, gutter numbers, spines --- */
    for (i32 i = first; i < last; i++) {
        const GdLine *l = &v->lines[i];
        f32 y = top + (f32)i * L.row_h - v->scroll_px;
        if (l->kind == GDL_ADD || l->kind == GDL_DEL) {
            bool add = (l->kind == GDL_ADD);
            renderer_draw_rect(r, L.px, y, L.pw, L.row_h,
                               add ? P.add_bg : P.del_bg);
            renderer_draw_rect(r, L.px, y, 2.0f * L.dpi, L.row_h,
                               add ? P.add_bar : P.del_bar);
        } else if (l->kind == GDL_HUNK) {
            /* A hairline through the row's middle, broken by the label the
             * text pass writes over it. Cheaper to read than a filled band. */
            renderer_draw_rect(r, L.px + 8.0f * L.dpi,
                               y + L.row_h * 0.5f,
                               L.pw - 16.0f * L.dpi, fmaxf(1.0f, L.dpi),
                               P.rule);
        }
    }
    renderer_flush_rects(r);

    /* Vertical rule closing the number column. Drawn once, not per row. */
    renderer_draw_rect(r, L.px + L.gutter_w, L.py, fmaxf(1.0f, L.dpi), L.ph,
                       P.rule);
    renderer_flush_rects(r);

    /* --- pass 2: the rows --- */
    char num[16];
    f32 gut_r = L.px + L.gutter_w - 6.0f * L.dpi;   /* right edge of numbers */
    for (i32 i = first; i < last; i++) {
        const GdLine *l = &v->lines[i];
        f32 y  = top + (f32)i * L.row_h - v->scroll_px;
        f32 ty = y + (L.row_h - L.glyph_h) * 0.5f;

        if (l->kind == GDL_GAP) continue;
        if (l->kind == GDL_FILE) {
            renderer_flush_glyphs(r);
            if (l->file >= 0 && l->file < v->file_count)
                gd_draw_card(app, &L, &P, &v->files[l->file], y, false);
            continue;
        }

        if (l->kind == GDL_HUNK) {
            if (l->len) {
                /* Punch a gap in the hairline so the label doesn't sit on it.
                 * Opaque even when the dock body is not: this is ERASING the
                 * rule drawn underneath, and a translucent patch would let it
                 * show through the label. Label-width only, so it is not a
                 * meaningful hole in the window's translucency. */
                f32 lx = L.body_x - 4.0f * L.dpi;
                f32 lw = fminf((f32)l->cols * L.cw + 8.0f * L.dpi, L.body_w + 8.0f * L.dpi);
                Color punch = body; punch.a = 1.0f;
                renderer_flush_glyphs(r);
                renderer_draw_rect(r, lx, y, lw, L.row_h, punch);
                renderer_flush_rects(r);
                gd_draw_row(r, v, l, t, P.hunk_fg, L.body_x, ty, L.cw,
                            L.body_x, L.px + L.pw - GD_PAD_PT * L.dpi);
            }
            continue;
        }

        if (l->old_no >= 0) {
            i32 k = snprintf(num, sizeof num, "%d", l->old_no);
            gd_text(r, num, (usize)k,
                    gut_r - (f32)(v->gutter_digits * 2 + 1) * L.cw
                          + (f32)(v->gutter_digits - k) * L.cw,
                    ty, P.gutter_fg, L.cw, L.px, L.px + L.gutter_w);
        }
        if (l->new_no >= 0) {
            i32 k = snprintf(num, sizeof num, "%d", l->new_no);
            gd_text(r, num, (usize)k,
                    gut_r - (f32)k * L.cw,
                    ty, P.gutter_fg, L.cw, L.px, L.px + L.gutter_w);
        }

    }
    renderer_flush_glyphs(r);
    /* One body scissor for the viewport, rather than a per-row GPU flush. */
    renderer_push_scissor(r, L.body_x, L.py, L.body_w, L.ph);
    for (i32 i = first; i < last; i++) {
        const GdLine *l = &v->lines[i];
        if (l->kind != GDL_CTX && l->kind != GDL_ADD && l->kind != GDL_DEL && l->kind != GDL_NOTE) continue;
        f32 y = top + (f32)i * L.row_h - v->scroll_px;
        f32 ty = y + (L.row_h - L.glyph_h) * 0.5f;
        Color fg = l->kind == GDL_ADD ? P.add_fg : l->kind == GDL_DEL ? P.del_fg :
                   l->kind == GDL_NOTE ? P.hunk_fg : P.ctx_fg;
        gd_draw_row(r, v, l, t, fg, L.body_x - v->hscroll_px, ty, L.cw,
                     L.body_x, L.body_x + L.body_w);
    }
    renderer_flush_glyphs(r);
    renderer_pop_scissor(r);

    /* --- pass 3: the pinned file card ---
     * Whichever file the top of the viewport is inside stays legible while its
     * rows scroll past. Suppressed while the NEXT card is sliding into its
     * place, so the two never overlap. */
    {
        i32 fi = -1;
        for (i32 i = first; i < v->line_count; i++) {
            if (v->lines[i].file >= 0) { fi = v->lines[i].file; break; }
        }
        /* `first` is the row at the top edge; if it is itself a card, that card
         * is already drawn inline exactly there. */
        bool inline_at_top = first < v->line_count &&
                             v->lines[first].kind == GDL_FILE &&
                             fabsf(top + (f32)first * L.row_h - v->scroll_px - L.py)
                                 < L.row_h * 0.5f;
        if (fi >= 0 && fi < v->file_count && v->scroll_px > L.row_h * 0.5f &&
            !inline_at_top) {
            f32 pin_y = L.py;
            /* Push the pinned card up as the next one arrives underneath. */
            for (i32 i = first; i < last; i++) {
                if (v->lines[i].kind != GDL_FILE) continue;
                f32 cy = top + (f32)i * L.row_h - v->scroll_px;
                if (cy > L.py && cy < L.py + L.row_h)
                    pin_y = cy - L.row_h;
                break;
            }
            gd_draw_card(app, &L, &P, &v->files[fi], pin_y, true);
        }
    }

    renderer_pop_scissor(r);
    if (v->truncated || v->failed) {
        const char *msg = v->failed ? "some changes could not be read" : "some changes omitted: size limit";
        f32 y = L.py + L.ph - L.row_h;
        Color c = P.card_bg; c.a = 1.0f;
        renderer_draw_rect(r, L.px, y, L.pw, L.row_h, c);
        renderer_flush_rects(r);
        gd_text(r, msg, strlen(msg), L.px + 8.0f * L.dpi, y + (L.row_h - L.glyph_h) * 0.5f,
                P.hunk_fg, L.cw, L.px, L.px + L.pw);
        renderer_flush_glyphs(r);
    }

    /* --- scrollbar: a thin overlay, thicker while the pointer is in the dock */
    if (L.max_scroll > 0.0f) {
        bool near = ui_hover_hit(L.px, L.py, (f32)app->fb_width - L.px, L.ph,
                                 app->hover_x, app->hover_y);
        f32 sw = (near ? GD_SCROLLBAR_PT + 2.0f : GD_SCROLLBAR_PT) * L.dpi;
        f32 track_h = L.ph - 8.0f * L.dpi;
        f32 thumb_h = track_h * (L.ph / L.content_h);
        if (thumb_h < 24.0f * L.dpi) thumb_h = 24.0f * L.dpi;
        f32 tpos = (v->scroll_px / L.max_scroll) * (track_h - thumb_h);
        Color th = t->fg; th.a = near ? 0.28f : 0.16f;
        renderer_draw_rrect_simple(r, L.px + L.pw - sw - 3.0f * L.dpi,
                                   L.py + 4.0f * L.dpi + tpos, sw, thumb_h,
                                   th, sw * 0.5f);
        renderer_flush_rrects(r);
    }

    renderer_reset_ui_scale(r);
}

/* =========================================================================
 * Events
 * ========================================================================= */

bool git_diff_handle_key(AppState *app, i32 key, u32 mods) {
    if (!app || !app->git_diff.open) return false;
    (void)mods;
    /* Deliberately narrow. A docked sidebar sits BESIDE the terminal, which
     * still has focus — swallowing arrows or Page Up here would break scrolling
     * the shell while the dock happens to be open. Esc is the one key a panel
     * is allowed to claim, and even that only closes it. */
    if (key == KEY_ESCAPE) { git_diff_close(app); return true; }
    return false;
}

bool git_diff_handle_click(AppState *app, f32 mx, f32 my) {
    if (!app || !app->git_diff.open) return false;
    GitDiffView *v = &app->git_diff;
    GdLayout L; gd_layout(app, &L);

    /* The divider first: its grab zone straddles the dock's left edge, so a
     * click there must not read as a click on the terminal beside it. */
    if (mx >= L.grip_x && mx <= L.grip_x + L.grip_w &&
        my >= L.py && my < L.py + L.ph) {
        v->resizing = true;
        return true;
    }
    /* Outside the dock the click belongs to whatever is there — this is a
     * sidebar, not a modal, so it neither swallows the event nor closes. */
    if (mx < L.px || mx >= L.px + L.pw || my < L.py || my >= L.py + L.ph) return false;
    return true;   /* inside: consumed so it can't reach the terminal */
}

/* Divider drag. Returns true while it owns the pointer; the caller relayouts,
 * because the terminal grid is derived from the dock width. */
bool git_diff_handle_drag(AppState *app, f32 mx) {
    if (!app || !app->git_diff.open || !app->git_diff.resizing) return false;
    f32 w = (f32)app->fb_width - mx;
    app->git_diff.width = w;          /* clamped by app_git_diff_dock_w */
    return true;
}

void git_diff_end_drag(AppState *app) {
    if (app) app->git_diff.resizing = false;
}

bool git_diff_handle_scroll(AppState *app, f32 mx, f32 my, f32 dx, f32 dy,
                            bool precise) {
    if (!app || !app->git_diff.open) return false;
    GitDiffView *v = &app->git_diff;
    GdLayout L; gd_layout(app, &L);

    if (mx < L.px || mx >= L.px + L.pw || my < L.py || my >= L.py + L.ph) return false;    /* the terminal's wheel, not ours */

    /* Axis lock. A two-finger swipe is never purely one axis — scrolling down
     * a diff leaks a few pixels of dx per event, and applying those would make
     * the whole code column drift sideways while reading downward. Whichever
     * axis dominates the START of a gesture owns the whole gesture; a pause
     * resets it, so switching axes just means lifting the fingers. Mouse
     * wheels report one axis at a time and land on the same path harmlessly.
     *
     * On the VIEW, not a file static: the lock outlives 200 ms of idle, and a
     * shared one would carry between windows — a sideways swipe in a second
     * dock right after reading downward in the first would scroll the axis the
     * first gesture claimed. */
    f64 now = platform_time_sec();
    if (now - v->scroll_axis_ts > 0.20) v->scroll_axis = 0;
    v->scroll_axis_ts = now;
    if (v->scroll_axis == 0) {
        f32 ax = fabsf(dx), ay = fabsf(dy);
        /* Horizontal has to win clearly: a diagonal drift reads as vertical,
         * which is what the reader almost always meant. */
        if (ax > ay * 1.6f && ax > 0.5f) v->scroll_axis = 2;
        else if (ay > 0.5f)              v->scroll_axis = 1;
    }

    /* Trackpads report pixels, wheels report notches. Rows are painted at a
     * fixed height, so a pixel delta maps straight onto the scroll offset and
     * only the notch case needs converting. */
    if (v->scroll_axis == 2) v->hscroll_px -= precise ? dx : dx * L.cw * PLATFORM_VIEW_WHEEL_LINES;
    else                     v->scroll_px  -= precise ? dy : dy * L.row_h * PLATFORM_VIEW_WHEEL_LINES;
    gd_clamp(v, &L);
    return true;
}
