/*
 * Liu - Note transclusion cache. See md_embed.h.
 */
#include "ui/markdown/md_embed.h"
#include "ui/markdown/md_parse.h"
#include "core/memory.h"
#include "core/mem_budget.h"
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define MD_EMBED_SLOTS      8
#define MD_EMBED_PATH_CAP   1024
/* A transcluded note is a fragment of a page, not a database dump. Past this
 * the embed is refused rather than parsed — the cost lands on every frame of
 * the HOST document, which the reader did not ask to pay. */
#define MD_EMBED_MAX_BYTES  (512u * 1024u)
/* The largest arena one embed may be parsed into. The first attempt is
 * (file size + 256 KiB), which fits every ordinary note; a parse that reports
 * MdDoc.incomplete is retried ONCE with the host document's own worst-case
 * formula (md_parse expands emphasis-dense text to many nodes per byte), and
 * a note that does not fit even that is refused as MD_EMBED_TOO_LARGE rather
 * than shown as empty. The reservation is mmap and demand-paged, so this cap
 * bounds address space, not resident memory — `used` is what the ledger sees. */
#define MD_EMBED_ARENA_MAX  (64u * 1024u * 1024u)

typedef struct MdEmbedJob MdEmbedJob;
static void embed_job_drop(MdEmbedJob *job);
static void embed_job_cancel(MdEmbedJob *job);
static void embed_loader_notify(void);
static _Atomic u32 g_embed_caches;

typedef struct MdEmbedEntry {
    char   path[MD_EMBED_PATH_CAP];
    Arena  arena;
    MdDoc *doc;
    /* The file's identity at the last disk check. st_mtime in whole seconds
     * plus size cannot tell apart a note rewritten within the same second to
     * the same length (an editor's save, a `sed -i`): device and inode catch
     * the atomic-rename save every editor does, nanoseconds catch the
     * in-place one. */
    u64    dev, ino;
    i64    size;
    i64    mtime_sec, mtime_nsec;
    /* When the disk was last asked, on the monotonic clock. Between checks
     * the entry is served from memory with no syscall at all, so an embed is
     * not re-opened and re-stat'ed on every render pass, whether its note
     * changed, stayed the same or is not there. */
    f64    checked_at;
    u32    last_pass;
    bool   used;
    bool   failed;
    u8     status;          /* MdEmbedStatus */
    MdEmbedJob *job;
    struct MdEmbedEntry *staged;
} MdEmbedEntry;

struct MdEmbedCache {
    MdEmbedEntry slots[MD_EMBED_SLOTS];
    u32          pass;
    char         roots[MD_EMBED_MAX_ROOTS][MD_EMBED_PATH_CAP];   /* canonical */
    u32          root_count;
    bool         async_enabled, roots_dirty, retry_pending;
    u64          generation, revision;
    void       (*wake)(void);
};

MdEmbedCache *md_embed_cache_create(void) {
    return (MdEmbedCache *)calloc(1, sizeof(MdEmbedCache));
}

void md_embed_cache_set_roots(MdEmbedCache *c, const char *const *roots, u32 count) {
    if (!c) return;
    c->generation++; c->roots_dirty = true;
    /* A root change is part of the cache key: what was allowed under the old
     * roots may not be under the new ones, so every entry is re-validated on
     * its next request rather than served out of its recheck window. Marking
     * rather than releasing — an entry served in the current pass has rects
     * pointing into it, and the get path keeps serving those until the pass
     * ends. */
    for (u32 i = 0; i < MD_EMBED_SLOTS; i++)
        if (c->slots[i].used) c->slots[i].checked_at = -1e300;
    c->root_count = 0;
    for (u32 i = 0; roots && i < count && c->root_count < MD_EMBED_MAX_ROOTS; i++) {
        if (!roots[i] || !roots[i][0]) continue;
        char canon[PATH_MAX];
        if (!realpath(roots[i], canon)) continue;
        usize n = strlen(canon);
        if (n == 0 || n >= MD_EMBED_PATH_CAP) continue;
        bool dup = false;
        for (u32 k = 0; k < c->root_count; k++)
            if (strcmp(c->roots[k], canon) == 0) { dup = true; break; }
        if (dup) continue;
        memcpy(c->roots[c->root_count++], canon, n + 1);
    }
}

static bool embed_has_note_ext(const char *path) {
    static const char *const ext[] = { ".md", ".markdown", ".mdown", ".mkd", ".txt" };
    usize n = strlen(path);
    for (usize i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        usize el = strlen(ext[i]);
        if (n > el && strcasecmp(path + n - el, ext[i]) == 0) return true;
    }
    return false;
}

bool md_embed_path_allowed(const char *resolved, const char *const *roots, u32 count) {
    if (!resolved || resolved[0] != '/' || !embed_has_note_ext(resolved)) return false;
    for (u32 i = 0; roots && i < count; i++) {
        const char *r = roots[i];
        if (!r || !r[0]) continue;
        usize rl = strlen(r);
        while (rl > 1 && r[rl - 1] == '/') rl--;
        if (strncmp(resolved, r, rl) != 0) continue;
        /* "/vault" must not admit "/vault-secrets/x.md": the next byte after
         * the root has to be a separator (or the root is "/"). */
        if (rl == 1 || resolved[rl] == '/') return true;
    }
    return false;
}

static void entry_release(MdEmbedEntry *e) {
    if (e->job) { embed_job_cancel(e->job); embed_job_drop(e->job); }
    if (e->staged) { entry_release(e->staged); free(e->staged); }
    arena_destroy(&e->arena);
    memset(e, 0, sizeof *e);
}

void md_embed_cache_destroy(MdEmbedCache *c) {
    if (!c) return;
    for (u32 i = 0; i < MD_EMBED_SLOTS; i++) entry_release(&c->slots[i]);
    if (c->async_enabled) { atomic_fetch_sub(&g_embed_caches,1); embed_loader_notify(); }
    free(c);
}

void md_embed_cache_begin_pass(MdEmbedCache *c) {
    if (!c) return;
    (void)md_embed_cache_pump(c);
    c->pass++;
    if (c->async_enabled && c->roots_dirty) {
        for (u32 i=0;i<MD_EMBED_SLOTS;i++) entry_release(&c->slots[i]);
        c->revision++; c->roots_dirty=false;
    }
    for (u32 i=0;i<MD_EMBED_SLOTS;i++) {
        MdEmbedEntry *e=&c->slots[i], *next=e->staged;
        if (!next) continue;
        e->staged=NULL;
        /* A metadata-only result means the existing parse is unchanged. */
        if (next->status==MD_EMBED_OK && !next->doc) {
            e->checked_at=next->checked_at;
        } else {
            entry_release(e); *e=*next;
            if (e->doc) e->doc->arena=&e->arena;
            c->revision++;
        }
        free(next);
    }
}

void md_embed_cache_enable_async(MdEmbedCache *c, void (*wake)(void)) {
    if (c) {
        if (!c->async_enabled) atomic_fetch_add(&g_embed_caches,1);
        c->async_enabled=true; c->wake=wake;
    }
}
u64 md_embed_cache_revision(const MdEmbedCache *c) { return c?c->revision:0; }


usize md_embed_cache_used_bytes(const MdEmbedCache *c) {
    usize n = 0;
    if (!c) return 0;
    for (u32 i = 0; i < MD_EMBED_SLOTS; i++)
        { const MdEmbedEntry *e=&c->slots[i];
          n += e->arena.used;
          if(e->staged) n += sizeof(*e->staged)+e->staged->arena.used; }
    return n;
}

usize md_embed_cache_reserved_bytes(const MdEmbedCache *c) {
    usize n = 0;
    if (!c) return 0;
    for (u32 i = 0; i < MD_EMBED_SLOTS; i++)
        { const MdEmbedEntry *e=&c->slots[i];
          n += e->arena.size;
          if(e->staged) n += e->staged->arena.size; }
    return n;
}

/* Use `CLOCK_MONOTONIC` directly so system-clock changes cannot extend an
 * embed cache entry's lifetime. This file can run without the platform
 * layer. */
static f64 (*g_embed_clock)(void) = NULL;

void md_embed_set_clock(f64 (*now_sec)(void)) { g_embed_clock = now_sec; }

static f64 embed_now(void) {
    if (g_embed_clock) return g_embed_clock();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

static void entry_record_identity(MdEmbedEntry *e, const struct stat *st) {
    e->dev  = (u64)st->st_dev;
    e->ino  = (u64)st->st_ino;
    e->size = (i64)st->st_size;
    e->mtime_sec = (i64)st->st_mtime;
#if defined(__APPLE__)
    e->mtime_nsec = (i64)st->st_mtimespec.tv_nsec;
#else
    e->mtime_nsec = (i64)st->st_mtim.tv_nsec;
#endif
}

static bool entry_same_identity(const MdEmbedEntry *e, const struct stat *st) {
    i64 nsec;
#if defined(__APPLE__)
    nsec = (i64)st->st_mtimespec.tv_nsec;
#else
    nsec = (i64)st->st_mtim.tv_nsec;
#endif
    return e->dev == (u64)st->st_dev && e->ino == (u64)st->st_ino &&
           e->size == (i64)st->st_size && e->mtime_sec == (i64)st->st_mtime &&
           e->mtime_nsec == nsec;
}

/* How long an entry may be served without asking the disk again. A missing
 * note is retried less often than an existing one is re-validated: the
 * common case for "not found" is a broken link, and the open that fails is
 * the whole cost. */
static f64 entry_recheck_interval(const MdEmbedEntry *e) {
    return e->status == MD_EMBED_NOT_FOUND ? MD_EMBED_MISSING_SEC
                                            : MD_EMBED_RECHECK_SEC;
}

/* Join `target` onto `base_dir`, appending ".md" when the name carries no
 * extension — the wikilink form omits it. */
static void resolve_path(const char *base_dir, const u8 *target, u32 len,
                         char *out, usize cap) {
    char name[MD_EMBED_PATH_CAP];
    u32 n = len < sizeof(name) - 4 ? len : (u32)(sizeof(name) - 5);
    memcpy(name, target, n);
    name[n] = '\0';

    bool has_ext = false;
    for (i32 i = (i32)n - 1; i >= 0; i--) {
        if (name[i] == '/') break;
        if (name[i] == '.') { has_ext = true; break; }
    }
    if (!has_ext) snprintf(name + n, sizeof(name) - n, ".md");

    if (name[0] == '/' || !base_dir || !base_dir[0])
        snprintf(out, cap, "%s", name);
    else
        snprintf(out, cap, "%s/%s", base_dir, name);
}

/* The path of an OPEN descriptor, as the kernel names it right now — macOS
 * answers through F_GETPATH, Linux through /proc/self/fd. The confinement
 * check runs on THIS, not on realpath(): realpath answers about whatever the
 * name pointed at when it looked, and a second lookup for the read would
 * follow the name AGAIN, so a symlink swapped between the two (a rename() in a
 * hostile clone is enough) would be checked as one file and read as another.
 * Opening first and asking the descriptor makes the check and the read
 * describe the same inode. A name the kernel cannot give (an unlinked file, a
 * filesystem with no name cache) fails CLOSED. */
static bool embed_fd_path(int fd, char *out, usize cap) {
    if (fd < 0 || !out || cap < 2) return false;
#if defined(__APPLE__)
    char buf[PATH_MAX];
    if (fcntl(fd, F_GETPATH, buf) == -1) return false;
    usize n = strlen(buf);
    if (n == 0 || n >= cap) return false;
    memcpy(out, buf, n + 1);
    return true;
#else
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, out, cap - 1);
    if (n <= 0) return false;
    out[n] = '\0';
    /* "/x/y.md (deleted)" is the kernel saying the name no longer exists. */
    const char *gone = " (deleted)";
    usize gl = strlen(gone);
    if ((usize)n >= gl && strcmp(out + n - gl, gone) == 0) return false;
    return out[0] == '/';
#endif
}

/* Open `path` for reading and prove, on the descriptor itself, that it is a
 * regular note inside one of the roots. Returns the fd (with `st` filled) or
 * -1. O_NONBLOCK is there for a FIFO named like a note: a blocking open on
 * one waits for a writer that never comes and the UI thread stops with it. A
 * regular file ignores the flag, so nothing is cleared after. */
static int embed_open_confined(const MdEmbedCache *c, const char *path,
                               struct stat *st) {
    if (!c || c->root_count == 0) return -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return -1;
    char canon[PATH_MAX];
    const char *rp[MD_EMBED_MAX_ROOTS];
    for (u32 i = 0; i < c->root_count; i++) rp[i] = c->roots[i];
    if (fstat(fd, st) == 0 && S_ISREG(st->st_mode) &&
        embed_fd_path(fd, canon, sizeof canon) &&
        md_embed_path_allowed(canon, rp, c->root_count))
        return fd;
    close(fd);
    return -1;
}

/* Directory part of `path`, for the embedded document's own relative links. */
static void dir_of(const char *path, char *out, usize cap) {
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    else snprintf(out, cap, ".");
}

const MdDoc *md_embed_cache_get(MdEmbedCache *c, const char *base_dir,
                                const u8 *target, u32 target_len,
                                char *out_path, usize out_path_cap) {
    return md_embed_cache_get_ex(c, base_dir, target, target_len,
                                 out_path, out_path_cap, NULL);
}

/* The host document sizes its arena as `size * 8 + (size / 2) * (one inline
 * node + one block node)` (filebrowser.c, fb_open_file) because a dense
 * document overflows anything smaller mid-parse. Same formula here, for the
 * retry: an embed is a document too. */
static usize embed_arena_worst_case(usize size) {
    usize want = size * 8 + (size / 2) * (sizeof(MdInline) + sizeof(MdBlock));
    if (want < KB(256)) want = KB(256);
    if (want > MD_EMBED_ARENA_MAX) want = MD_EMBED_ARENA_MAX;
    return want;
}

/* Parse `bytes` into a fresh arena of `want` bytes. On success the arena and
 * the document are handed to the caller; a document the arena could not hold
 * (MdDoc.incomplete) or could not start is destroyed and NULL is returned. */
static MdDoc *embed_parse_into(const u8 *bytes, usize len, const char *dir,
                               usize want, Arena *out_arena,
                               const _Atomic bool *cancel, bool *limited) {
    Arena arena = arena_create(want);
    if (!arena.base) return NULL;
    u8 *buf = (u8 *)arena_alloc(&arena, len + 1);
    char *dir_copy = (char *)arena_alloc(&arena, strlen(dir) + 1);
    if (!buf || !dir_copy) { arena_destroy(&arena); return NULL; }
    memcpy(buf, bytes, len);
    buf[len] = '\0';
    memcpy(dir_copy, dir, strlen(dir) + 1);
    MdDoc *doc = md_parse_ex(&arena, buf, len, dir_copy, cancel);
    if (limited) *limited = (doc && doc->parse_limited) ||
        (cancel && atomic_load_explicit(cancel, memory_order_relaxed));
    if (!doc || doc->incomplete) { arena_destroy(&arena); return NULL; }
    *out_arena = arena;
    return doc;
}

static const MdDoc *entry_serve(MdEmbedEntry *e, u32 pass, MdEmbedStatus *st) {
    e->last_pass = pass;
    if (st) *st = (MdEmbedStatus)e->status;
    return e->failed ? NULL : e->doc;
}

static void entry_fail(MdEmbedEntry *slot, const char *path, MdEmbedStatus why,
                       const struct stat *st, f64 now, u32 pass) {
    entry_release(slot);
    snprintf(slot->path, sizeof slot->path, "%s", path);
    slot->used = true;
    slot->failed = true;
    slot->status = (u8)why;
    slot->last_pass = pass;
    slot->checked_at = now;
    if (st) entry_record_identity(slot, st);
}

static const MdDoc *embed_get_sync(MdEmbedCache *c, const char *base_dir,
                                   const u8 *target, u32 target_len,
                                   char *out_path, usize out_path_cap,
                                   MdEmbedStatus *out_status,
                                   const _Atomic bool *cancel) {
    if (out_status) *out_status = MD_EMBED_NOT_FOUND;
    if (!c || !target || target_len == 0 ||
        (cancel && atomic_load_explicit(cancel, memory_order_relaxed))) return NULL;

    char path[MD_EMBED_PATH_CAP];
    resolve_path(base_dir, target, target_len, path, sizeof path);
    if (out_path && out_path_cap) snprintf(out_path, out_path_cap, "%s", path);

    /* Look the path up BEFORE touching the filesystem. An entry already served
     * during this pass was validated when it was first served, so re-checking
     * it would cost a stat() per visible embed per frame and buy nothing,
     * since the document cannot be swapped out mid-frame anyway (see below).
     * An entry from an EARLIER pass is served the same way while its recheck
     * interval has not run out: the disk is asked on the clock, never per
     * frame. A negative answer is remembered on the same terms. */
    MdEmbedEntry *hit = NULL;
    for (u32 i = 0; i < MD_EMBED_SLOTS; i++) {
        MdEmbedEntry *e = &c->slots[i];
        if (e->used && strcmp(e->path, path) == 0) { hit = e; break; }
    }
    f64 now = embed_now();
    if (hit && (hit->last_pass == c->pass ||
                now - hit->checked_at < entry_recheck_interval(hit)))
        return entry_serve(hit, c->pass, out_status);

    /* Confinement runs on the CANONICAL path (symlinks and ".." resolved) of
     * the file actually OPENED, before any read: a target outside every root,
     * or without a note extension, is "note not found" to the reader — and is
     * remembered as such below, under the spelling the document used, so it
     * costs one open per MD_EMBED_MISSING_SEC rather than one per frame. The
     * descriptor stays open across the slot walk and is the one the bytes are
     * read from, so nothing can be swapped in between (see embed_fd_path). */
    struct stat st;
    int fd = embed_open_confined(c, path, &st);
    bool stat_ok = fd >= 0;

    if (hit) {
        /* Same file, unchanged on disk → serve the parse already held (or
         * the failure already recorded for exactly this file). */
        bool unchanged = stat_ok
            ? (hit->status != MD_EMBED_NOT_FOUND && entry_same_identity(hit, &st))
            : (hit->status == MD_EMBED_NOT_FOUND);
        if (unchanged) {
            hit->checked_at = now;
            if (fd >= 0) close(fd);
            return entry_serve(hit, c->pass, out_status);
        }
    }

    MdEmbedEntry *free_slot = NULL, *oldest = NULL;
    if (hit) {
        /* Changed, appeared or vanished: the slot is reparsed in place. The
         * arena goes now — the entry is not from this pass (that case
         * returned above), so no rect recorded this frame points into it. */
        entry_release(hit);
        free_slot = hit;
    } else {
        for (u32 i = 0; i < MD_EMBED_SLOTS; i++) {
            MdEmbedEntry *e = &c->slots[i];
            if (!e->used) { if (!free_slot) free_slot = e; continue; }
            /* An entry handed out earlier in THIS pass is off limits as an
             * eviction victim: the host document has already recorded link
             * and glyph rects pointing into that arena, and arena_destroy
             * would leave them dangling for the rest of the frame — a hover
             * hit-test then reads freed memory. Only entries from an earlier
             * pass may go. */
            if (e->last_pass == c->pass) continue;
            if (!oldest || e->last_pass < oldest->last_pass) oldest = e;
        }
    }

    /* Every slot is already live this frame: refuse rather than evict one out
     * from under the rects the host just recorded. The caller draws its
     * fallback chip, and the next frame — with a fresh pass number — is free
     * to evict normally, so a document with more transclusions than slots
     * degrades to "some embeds show as links" instead of crashing. */
    MdEmbedEntry *slot = free_slot ? free_slot : oldest;
    if (!slot) { if (fd >= 0) close(fd); return NULL; }

    if (!stat_ok) {
        entry_fail(slot, path, MD_EMBED_NOT_FOUND, NULL, now, c->pass);
        return entry_serve(slot, c->pass, out_status);
    }
    if (st.st_size <= 0 || (u64)st.st_size > MD_EMBED_MAX_BYTES) {
        close(fd);
        /* An empty note is not an error worth a different line; a huge one
         * is, and its identity is kept so the same bytes are not re-read on
         * every recheck. */
        entry_fail(slot, path, st.st_size <= 0 ? MD_EMBED_NOT_FOUND : MD_EMBED_TOO_LARGE,
                   &st, now, c->pass);
        return entry_serve(slot, c->pass, out_status);
    }

    /* Read from the descriptor the check was made on — never from the name.
     * Into a plain buffer first: a parse that does not fit is retried into a
     * larger arena, and the bytes have to outlive the first arena for that. */
    usize len = (usize)st.st_size;
    u8 *bytes = (u8 *)malloc(len);
    if (!bytes) { close(fd); return NULL; }
    usize rd = 0;
    while (rd < len) {
        if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) break;
        usize chunk = len - rd;
        if (chunk > KB(16)) chunk = KB(16);
        ssize_t n = read(fd, bytes + rd, chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        rd += (usize)n;
    }
    close(fd);
    if (rd != len) {
        free(bytes);
        entry_fail(slot,path,MD_EMBED_NOT_FOUND,&st,now,c->pass);
        return entry_serve(slot,c->pass,out_status);
    }

    char sub_dir[MD_EMBED_PATH_CAP];
    dir_of(path, sub_dir, sizeof sub_dir);

    /* One arena holds the file bytes AND the AST that points into them, so the
     * whole document is freed by a single arena_destroy. First the cheap
     * reservation, then — only for a note whose parse did not fit — the worst
     * case. */
    Arena arena = {0};
    bool limited = false;
    MdDoc *doc = embed_parse_into(bytes, rd, sub_dir, rd + KB(256), &arena,
                                   cancel, &limited);
    if (!doc && !limited) {
        usize worst = embed_arena_worst_case(rd);
        if (worst > rd + KB(256))
            doc = embed_parse_into(bytes, rd, sub_dir, worst, &arena, cancel, &limited);
    }
    free(bytes);
    if (!doc) {
        entry_fail(slot, path, MD_EMBED_TOO_LARGE, &st, now, c->pass);
        return entry_serve(slot, c->pass, out_status);
    }

    entry_release(slot);
    snprintf(slot->path, sizeof slot->path, "%s", path);
    slot->arena     = arena;
    slot->doc       = doc;
    doc->arena     = &slot->arena;
    entry_record_identity(slot, &st);
    slot->checked_at = now;
    slot->last_pass = c->pass;
    slot->used      = true;
    slot->failed    = false;
    slot->status    = MD_EMBED_OK;
    if (out_status) *out_status = MD_EMBED_OK;
    return doc;
}

/* One process-wide serial loader, bounded across visible and closing views.
 * Jobs own their paths, roots and parse; no worker borrows a UI cache/AST. */
struct MdEmbedJob {
    MdEmbedJob *next;
    _Atomic u32 refs;
    _Atomic bool done, cancelled;
    MdEmbedCache local;
    u64 generation;
    usize result_bytes;
    void (*wake)(void);
};
static struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    MdEmbedJob *head, *tail;
    bool running;
} g_embed_loader={.lock=PTHREAD_MUTEX_INITIALIZER,.ready=PTHREAD_COND_INITIALIZER};
static _Atomic u32 g_embed_jobs;
static void embed_loader_notify(void) {
    pthread_mutex_lock(&g_embed_loader.lock);
    pthread_cond_signal(&g_embed_loader.ready);
    pthread_mutex_unlock(&g_embed_loader.lock);
}

static void embed_job_cancel(MdEmbedJob *j) { atomic_store(&j->cancelled,true); }
static void embed_job_drop(MdEmbedJob *j) {
    if (atomic_fetch_sub_explicit(&j->refs,1,memory_order_acq_rel)!=1) return;
    for(u32 i=0;i<MD_EMBED_SLOTS;i++) entry_release(&j->local.slots[i]);
    mem_budget_transient_adjust(MEM_EDITOR,-(isize)(sizeof(*j)+j->result_bytes));
    free(j); atomic_fetch_sub(&g_embed_jobs,1);
}
static void *embed_loader(void *unused) {
    (void)unused;
    pthread_mutex_lock(&g_embed_loader.lock);
    for (;;) {
        while (!g_embed_loader.head) {
            if (!atomic_load(&g_embed_caches)) {
                g_embed_loader.running=false;
                pthread_mutex_unlock(&g_embed_loader.lock);return NULL;
            }
            struct timespec until; clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=2;
            if(pthread_cond_timedwait(&g_embed_loader.ready,&g_embed_loader.lock,&until)!=0 && !g_embed_loader.head) {
                g_embed_loader.running=false;
                pthread_mutex_unlock(&g_embed_loader.lock);return NULL;
            }
        }
        MdEmbedJob *j=g_embed_loader.head;g_embed_loader.head=j->next;
        if(!g_embed_loader.head)g_embed_loader.tail=NULL;
        pthread_mutex_unlock(&g_embed_loader.lock);
        if(!atomic_load(&j->cancelled)) {
            char path[MD_EMBED_PATH_CAP];snprintf(path,sizeof path,"%s",j->local.slots[0].path);
            MdEmbedStatus status=MD_EMBED_NOT_FOUND;
            (void)embed_get_sync(&j->local,NULL,(const u8*)path,(u32)strlen(path),NULL,0,&status,&j->cancelled);
            MdEmbedEntry *e=&j->local.slots[0];
            if(!e->used)entry_fail(e,path,status,NULL,embed_now(),j->local.pass);
            j->result_bytes=md_embed_cache_used_bytes(&j->local);
            mem_budget_transient_adjust(MEM_EDITOR,(isize)j->result_bytes);
        }
        atomic_store_explicit(&j->done,true,memory_order_release);
        if(!atomic_load(&j->cancelled) && j->wake)j->wake();
        embed_job_drop(j);
        pthread_mutex_lock(&g_embed_loader.lock);
    }
}
static void embed_schedule(MdEmbedCache *c,MdEmbedEntry *e) {
    if(e->job || e->staged)return;
    u32 count=atomic_load(&g_embed_jobs);
    do { if(count>=8){c->retry_pending=true;return;} }
    while(!atomic_compare_exchange_weak(&g_embed_jobs,&count,count+1));
    MdEmbedJob *j=calloc(1,sizeof(*j));
    if(!j){atomic_fetch_sub(&g_embed_jobs,1);return;}
    atomic_init(&j->refs,1);atomic_init(&j->done,false);atomic_init(&j->cancelled,false);
    mem_budget_transient_adjust(MEM_EDITOR,(isize)sizeof(*j));
    j->generation=c->generation;j->wake=c->wake;j->local.pass=1;
    j->local.root_count=c->root_count;memcpy(j->local.roots,c->roots,sizeof c->roots);
    j->local.slots[0]=*e;
    MdEmbedEntry *copy=&j->local.slots[0];
    copy->arena=(Arena){0};copy->doc=NULL;copy->job=NULL;copy->staged=NULL;
    copy->checked_at=-1e300;copy->last_pass=0;
    /* A never-loaded placeholder must perform its first read. */
    if(e->status==MD_EMBED_LOADING)copy->used=false;
    pthread_mutex_lock(&g_embed_loader.lock);
    if(!g_embed_loader.running) {
        pthread_t th;pthread_attr_t attr;pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
        int rc=pthread_create(&th,&attr,embed_loader,NULL);pthread_attr_destroy(&attr);
        if(rc){pthread_mutex_unlock(&g_embed_loader.lock);embed_job_drop(j);return;}
        g_embed_loader.running=true;
    }
    atomic_fetch_add(&j->refs,1);e->job=j;
    if(g_embed_loader.tail)g_embed_loader.tail->next=j;else g_embed_loader.head=j;
    g_embed_loader.tail=j;pthread_cond_signal(&g_embed_loader.ready);
    pthread_mutex_unlock(&g_embed_loader.lock);
}
bool md_embed_cache_completion_ready(const MdEmbedCache *c) {
    if(!c)return false;
    if(c->retry_pending && atomic_load(&g_embed_jobs)<8)return true;
    for(u32 i=0;i<MD_EMBED_SLOTS;i++)if(c->slots[i].job &&
        atomic_load_explicit(&c->slots[i].job->done,memory_order_acquire))return true;
    return false;
}
bool md_embed_cache_pump(MdEmbedCache *c) {
    if(!c)return false;
    bool changed=false;
    if(c->retry_pending && atomic_load(&g_embed_jobs)<8) {
        c->retry_pending=false; changed=true;
    }
    for(u32 i=0;i<MD_EMBED_SLOTS;i++) {
        MdEmbedEntry *e=&c->slots[i];MdEmbedJob *j=e->job;
        if(!j || !atomic_load_explicit(&j->done,memory_order_acquire))continue;
        e->job=NULL;
        if(j->generation==c->generation && !atomic_load(&j->cancelled)) {
            const MdEmbedEntry *result=&j->local.slots[0];
            if(!result->doc && result->status==e->status) {
                /* Unchanged parse/failure: no AST swap, allocation or redraw. */
                e->checked_at=result->checked_at;
                e->dev=result->dev; e->ino=result->ino; e->size=result->size;
                e->mtime_sec=result->mtime_sec; e->mtime_nsec=result->mtime_nsec;
            } else {
                MdEmbedEntry *next=malloc(sizeof(*next));
                if(next) {
                    *next=j->local.slots[0];memset(&j->local.slots[0],0,sizeof(MdEmbedEntry));
                    if(next->doc)next->doc->arena=&next->arena;
                    e->staged=next;changed=true;
                } else e->checked_at=embed_now();
            }
        }
        embed_job_drop(j);
    }
    return changed;
}
const MdDoc *md_embed_cache_get_ex(MdEmbedCache *c,const char *base_dir,
    const u8 *target,u32 target_len,char *out_path,usize out_cap,MdEmbedStatus *status) {
    if(!c || !c->async_enabled)return embed_get_sync(c,base_dir,target,target_len,out_path,out_cap,status,NULL);
    if(status)*status=MD_EMBED_NOT_FOUND;
    if(!target || !target_len)return NULL;
    char path[MD_EMBED_PATH_CAP];resolve_path(base_dir,target,target_len,path,sizeof path);
    if(out_path && out_cap)snprintf(out_path,out_cap,"%s",path);
    MdEmbedEntry *slot=NULL,*oldest=NULL;
    for(u32 i=0;i<MD_EMBED_SLOTS;i++) {
        MdEmbedEntry *e=&c->slots[i];
        if(e->used && !strcmp(e->path,path)){slot=e;break;}
        if(!e->used){if(!oldest)oldest=e;}
        else if(e->last_pass!=c->pass && (!oldest || (oldest->used && e->last_pass<oldest->last_pass)))oldest=e;
    }
    if(!slot) {
        slot=oldest;if(!slot)return NULL;entry_release(slot);
        snprintf(slot->path,sizeof slot->path,"%s",path);slot->used=true;
        slot->failed=true;slot->status=MD_EMBED_LOADING;slot->checked_at=-1e300;
    }
    if(slot->status==MD_EMBED_LOADING || (slot->last_pass!=c->pass &&
        embed_now()-slot->checked_at>=entry_recheck_interval(slot)))embed_schedule(c,slot);
    return entry_serve(slot,c->pass,status);
}

static bool slice_eq_bytes(MdSlice s, const u8 *b, u32 n) {
    return s.len == n && (n == 0 || memcmp(s.data, b, n) == 0);
}

/* Case-insensitive compare, so "#Heading" finds "## heading". */
static bool slice_eq_ci(MdSlice s, const u8 *b, u32 n) {
    if (s.len != n) return false;
    for (u32 i = 0; i < n; i++) {
        u8 x = s.data[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

/* Does this block or anything under it carry `id`? Depth is bounded by the
 * parser's own MD_MAX_NEST, so no explicit cap is needed here. */
static bool block_tree_has_id(const MdBlock *b, const u8 *id, u32 id_len) {
    if (slice_eq_bytes(b->block_id, id, id_len)) return true;
    for (u32 i = 0; i < b->child_count; i++)
        if (block_tree_has_id(&b->children[i], id, id_len)) return true;
    return false;
}

/* Plain text of a heading's inline run, flattened into `out`. */
static u32 heading_text(const MdBlock *b, u8 *out, u32 cap) {
    u32 n = 0;
    for (u32 i = 0; i < b->inline_count && n < cap; i++) {
        MdSlice t = b->inlines[i].text;
        if (!t.len) t = b->inlines[i].alt;
        if (!t.len) t = b->inlines[i].url;
        for (u32 k = 0; k < t.len && n < cap; k++) out[n++] = t.data[k];
    }
    /* trim */
    u32 s = 0;
    while (s < n && (out[s] == ' ' || out[s] == '\t')) s++;
    while (n > s && (out[n-1] == ' ' || out[n-1] == '\t')) n--;
    if (s > 0) memmove(out, out + s, n - s);
    return n - s;
}

bool md_embed_section(const MdDoc *doc, const u8 *anchor, u32 anchor_len,
                      bool anchor_is_block, u32 *out_first, u32 *out_count) {
    if (!doc || !doc->blocks || doc->block_count == 0) return false;
    if (anchor_len == 0) {
        *out_first = 0;
        *out_count = doc->block_count;
        return true;
    }

    if (anchor_is_block) {
        for (u32 i = 0; i < doc->block_count; i++) {
            if (slice_eq_bytes(doc->blocks[i].block_id, anchor, anchor_len)) {
                *out_first = i;
                *out_count = 1;
                return true;
            }
        }
        /* A "^id" written inside a callout, quote or <details> lands on a CHILD
         * block, which the flat scan above cannot see. The selection is still
         * expressed in top-level terms — the whole container — because that is
         * the unit the caller can render; without this walk, a block reference
         * into a callout would report "not found". */
        for (u32 i = 0; i < doc->block_count; i++) {
            if (block_tree_has_id(&doc->blocks[i], anchor, anchor_len)) {
                *out_first = i;
                *out_count = 1;
                return true;
            }
        }
        return false;
    }

    for (u32 i = 0; i < doc->block_count; i++) {
        const MdBlock *b = &doc->blocks[i];
        if (b->kind != MD_BLOCK_HEADING) continue;
        u8 text[256];
        u32 n = heading_text(b, text, sizeof text);
        MdSlice hs = { text, n };
        if (!slice_eq_ci(hs, anchor, anchor_len)) continue;
        /* Everything down to the next heading at the same or a higher level:
         * the section IS the heading plus what it introduces. */
        u32 end = i + 1;
        while (end < doc->block_count) {
            const MdBlock *nb = &doc->blocks[end];
            if (nb->kind == MD_BLOCK_HEADING &&
                nb->heading_level <= b->heading_level) break;
            end++;
        }
        *out_first = i;
        *out_count = end - i;
        return true;
    }
    return false;
}
