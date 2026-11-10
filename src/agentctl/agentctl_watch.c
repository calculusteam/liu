/*
 * Liu — the folders a team works in, watched for changed files. See
 * agentctl_watch.h.
 *
 * Everything this module keeps lives on its own serial queue: the watches
 * are made, changed and stopped there, their callbacks run there, and so do
 * the delayed calls the caller asks for. On macOS that is a dispatch queue
 * and nothing takes a lock; on Linux it is a thread of its own, and the
 * other threads post to it under one lock.
 */
#include "agentctl/agentctl_watch.h"

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#define WATCH_DIR_MAX 1024
/* how long FSEvents gathers changes before it hands them over, seconds */
#define WATCH_LATENCY 0.1

typedef struct {
    char             dir[WATCH_DIR_MAX];
    FSEventStreamRef stream;
} Watch;

static dispatch_queue_t g_queue;
static AgentctlWatchFn  g_fn;
static void            *g_ctx;
static Watch            g_watch[AGENTCTL_WATCH_MAX];    /* on the queue only */
static bool             g_stopped;                     /* on the queue only */

static void stream_cb(ConstFSEventStreamRef ref, void *info, size_t n, void *paths_v,
                      const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
    (void)ref; (void)info; (void)ids;
    if (g_stopped || !g_fn || n == 0) return;
    u32 *f = malloc(n * sizeof *f);
    if (!f) return;
    for (size_t i = 0; i < n; i++) {
        FSEventStreamEventFlags e = flags[i];
        u32 x = 0;
        if (e & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
                 kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagRootChanged))
            x |= AGW_LOST;
        if (e & kFSEventStreamEventFlagItemIsDir) x |= AGW_DIR;
        if (e & (kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed)) x |= AGW_REMOVED;
        if (e & kFSEventStreamEventFlagItemCreated) x |= AGW_CREATED;
        f[i] = x;
    }
    g_fn(g_ctx, (const char *const *)paths_v, f, n);
    free(f);
}

static FSEventStreamRef stream_open(const char *dir) {
    CFStringRef s = CFStringCreateWithCString(NULL, dir, kCFStringEncodingUTF8);
    if (!s) return NULL;
    CFArrayRef paths = CFArrayCreate(NULL, (const void **)&s, 1, &kCFTypeArrayCallBacks);
    CFRelease(s);
    if (!paths) return NULL;
    FSEventStreamContext ctx = { 0, NULL, NULL, NULL, NULL };
    FSEventStreamRef st = FSEventStreamCreate(NULL, stream_cb, &ctx, paths, kFSEventStreamEventIdSinceNow,
                                              WATCH_LATENCY,
                                              kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                                              kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    if (!st) return NULL;
    FSEventStreamSetDispatchQueue(st, g_queue);
    if (!FSEventStreamStart(st)) {
        FSEventStreamInvalidate(st);
        FSEventStreamRelease(st);
        return NULL;
    }
    return st;
}

static void stream_close(FSEventStreamRef st) {
    FSEventStreamStop(st);
    FSEventStreamInvalidate(st);
    FSEventStreamRelease(st);
}

typedef struct {
    char  dirs[AGENTCTL_WATCH_MAX][WATCH_DIR_MAX];
    usize n;
} FolderSet;

static bool set_has(const FolderSet *fs, const char *dir) {
    for (usize i = 0; i < fs->n; i++) if (!strcmp(fs->dirs[i], dir)) return true;
    return false;
}

static void apply_folders(void *arg) {
    FolderSet *fs = arg;
    if (!g_stopped) {
        for (i32 i = 0; i < AGENTCTL_WATCH_MAX; i++) {
            if (!g_watch[i].stream || set_has(fs, g_watch[i].dir)) continue;
            stream_close(g_watch[i].stream);
            memset(&g_watch[i], 0, sizeof g_watch[i]);
        }
        for (usize k = 0; k < fs->n; k++) {
            bool have = false;
            Watch *free_slot = NULL;
            for (i32 i = 0; i < AGENTCTL_WATCH_MAX; i++) {
                if (g_watch[i].stream && !strcmp(g_watch[i].dir, fs->dirs[k])) have = true;
                if (!g_watch[i].stream && !free_slot) free_slot = &g_watch[i];
            }
            if (have || !free_slot) continue;
            FSEventStreamRef st = stream_open(fs->dirs[k]);
            if (!st) continue;
            memcpy(free_slot->dir, fs->dirs[k], sizeof free_slot->dir);
            free_slot->stream = st;
        }
    }
    free(fs);
}

bool agentctl_watch_available(void) { return true; }

bool agentctl_watch_start(AgentctlWatchFn fn, void *ctx) {
    if (g_queue || !fn) return g_queue != NULL;
    g_fn = fn;
    g_ctx = ctx;
    g_queue = dispatch_queue_create("liu.agentctl.watch", DISPATCH_QUEUE_SERIAL);
    return g_queue != NULL;
}

void agentctl_watch_folders(const char *const *dirs, usize n) {
    if (!g_queue) return;
    FolderSet *fs = calloc(1, sizeof *fs);
    if (!fs) return;
    for (usize i = 0; i < n && fs->n < AGENTCTL_WATCH_MAX; i++) {
        if (!dirs[i] || dirs[i][0] != '/' || strlen(dirs[i]) >= WATCH_DIR_MAX || set_has(fs, dirs[i])) continue;
        memcpy(fs->dirs[fs->n++], dirs[i], strlen(dirs[i]) + 1);
    }
    dispatch_async_f(g_queue, fs, apply_folders);
}

typedef struct {
    void (*fn)(void *ctx);
    void *ctx;
} Later;

static void later_run(void *arg) {
    Later *l = arg;
    if (!g_stopped) l->fn(l->ctx);
    free(l);
}

void agentctl_watch_later(u32 ms, void (*fn)(void *ctx), void *ctx) {
    if (!g_queue || !fn) return;
    Later *l = malloc(sizeof *l);
    if (!l) return;
    l->fn = fn;
    l->ctx = ctx;
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * (int64_t)NSEC_PER_MSEC), g_queue, l, later_run);
}

static void stop_all(void *arg) {
    (void)arg;
    for (i32 i = 0; i < AGENTCTL_WATCH_MAX; i++) {
        if (g_watch[i].stream) stream_close(g_watch[i].stream);
        memset(&g_watch[i], 0, sizeof g_watch[i]);
    }
    g_stopped = true;
}

void agentctl_watch_stop(void) {
    if (!g_queue) return;
    /* the streams go and every delayed call still queued finds the module
     * stopped; the queue itself lives on with them */
    dispatch_sync_f(g_queue, NULL, stop_all);
}

#elif defined(__linux__)

/* One thread of its own plays the serial queue: it owns the inotify
 * descriptor and every watch, gathers events for WATCH_LATENCY_MS before it
 * hands them over (per path, flags merged, as FSEvents coalesces), and runs
 * the delayed calls. The other threads only post to it under g_mu. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WATCH_DIR_MAX      1024
#define WATCH_LATENCY_MS   100
#define WATCH_DEPTH_MAX    64
#define WATCH_DIRS_PER_ROOT 4096          /* folders under one root; past it the root is LOST */
#define WATCH_BATCH_MAX    1024
#define WATCH_MASK (IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_FROM | \
                    IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW | IN_ONLYDIR | IN_EXCL_UNLINK)

typedef struct {
    char  dirs[AGENTCTL_WATCH_MAX][WATCH_DIR_MAX];
    usize n;
} FolderSet;

typedef struct Later {
    u64            due_ms;
    void         (*fn)(void *ctx);
    void          *ctx;
    struct Later  *next;
} Later;

typedef struct {
    int   wd;
    i32   root;                 /* index into g_root */
    char *path;                 /* absolute, as the file system spells it */
} WDir;

/* posted by any thread, under g_mu */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static FolderSet      *g_post_set;
static Later          *g_laters;
static bool            g_stop_req;

static pthread_t       g_thread;
static bool            g_started;
static atomic_bool     g_stopped;
static AgentctlWatchFn g_fn;
static void           *g_ctx;
static int             g_ifd = -1;
static int             g_wake[2] = { -1, -1 };

/* the thread's own */
static char  g_root[AGENTCTL_WATCH_MAX][WATCH_DIR_MAX];
static bool  g_root_on[AGENTCTL_WATCH_MAX];
static i32   g_root_dirs[AGENTCTL_WATCH_MAX];
static bool  g_root_lost[AGENTCTL_WATCH_MAX];
static WDir *g_dirs;
static usize g_ndirs, g_capdirs;
static char *g_bpath[WATCH_BATCH_MAX];
static u32   g_bflag[WATCH_BATCH_MAX];
static usize g_bn;
static u64   g_bdue;

static u64 mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000u + (u64)t.tv_nsec / 1000000u;
}

static void wake(void) {
    if (g_wake[1] < 0) return;
    char b = 1;
    ssize_t r = write(g_wake[1], &b, 1);
    (void)r;                    /* a full pipe already wakes the thread */
}

static void batch_flush(void);

/* The batch's paths by hash, so a path already in it is found in a probe or
 * two: a build writes thousands of files a second. Open addressing, twice the
 * batch's size; 0 is an empty slot, else the entry's index + 1. */
#define WATCH_BATCH_SLOTS (2 * WATCH_BATCH_MAX)
static u16 g_bslot[WATCH_BATCH_SLOTS];

static u32 path_hash(const char *p) {
    u32 h = 2166136261u;
    for (; *p; p++) { h ^= (u8)*p; h *= 16777619u; }
    return h;
}

static void batch_add(const char *path, u32 flags) {
    u32 at = path_hash(path) & (WATCH_BATCH_SLOTS - 1);
    for (; g_bslot[at]; at = (at + 1) & (WATCH_BATCH_SLOTS - 1)) {
        usize i = (usize)g_bslot[at] - 1;
        if (!strcmp(g_bpath[i], path)) { g_bflag[i] |= flags; return; }
    }
    if (g_bn >= WATCH_BATCH_MAX) {               /* a full batch goes early, nothing is dropped */
        batch_flush();
        at = path_hash(path) & (WATCH_BATCH_SLOTS - 1);
    }
    char *copy = strdup(path);
    if (!copy) return;
    if (!g_bn) g_bdue = mono_ms() + WATCH_LATENCY_MS;
    g_bpath[g_bn] = copy;
    g_bflag[g_bn] = flags;
    g_bn++;
    g_bslot[at] = (u16)g_bn;
}

static void batch_flush(void) {
    if (!g_bn) return;
    if (!atomic_load(&g_stopped) && g_fn) g_fn(g_ctx, (const char *const *)g_bpath, g_bflag, g_bn);
    for (usize i = 0; i < g_bn; i++) free(g_bpath[i]);
    g_bn = 0;
    memset(g_bslot, 0, sizeof g_bslot);
}

static void root_lost(i32 root) {
    if (root < 0 || !g_root_on[root] || g_root_lost[root]) return;
    g_root_lost[root] = true;
    batch_add(g_root[root], AGW_LOST | AGW_DIR);
}

/* g_dirs' index by watch descriptor: open addressing over a power of two at
 * least twice the watches, so an event finds its folder in a probe or two
 * instead of a walk over every watch. A slot holds the index + 1; 0 is empty
 * and -1 a removed entry the probe passes over. */
static i32  *g_wdslot;
static usize g_wdcap, g_wdused;

static usize wd_home(int wd) { return ((usize)(u32)wd * 2654435761u) & (g_wdcap - 1); }

static void wdmap_rebuild(usize want) {
    usize cap = 64;
    while (cap < want * 2) cap <<= 1;
    i32 *slots = calloc(cap, sizeof *slots);
    if (!slots) return;                     /* the old table stays */
    free(g_wdslot);
    g_wdslot = slots;
    g_wdcap = cap;
    g_wdused = 0;
    for (usize i = 0; i < g_ndirs; i++) {
        usize at = wd_home(g_dirs[i].wd);
        while (g_wdslot[at] != 0) at = (at + 1) & (g_wdcap - 1);
        g_wdslot[at] = (i32)i + 1;
        g_wdused++;
    }
}

static i32 wdmap_find(int wd, usize *slot) {
    if (!g_wdcap) return -1;
    for (usize at = wd_home(wd), n = 0; n < g_wdcap; at = (at + 1) & (g_wdcap - 1), n++) {
        i32 v = g_wdslot[at];
        if (v == 0) return -1;
        if (v > 0 && g_dirs[v - 1].wd == wd) { if (slot) *slot = at; return v - 1; }
    }
    return -1;
}

/* g_dirs[i] was just appended or moved to i. */
static void wdmap_set(usize i) {
    usize at;
    if (wdmap_find(g_dirs[i].wd, &at) >= 0) { g_wdslot[at] = (i32)i + 1; return; }
    if ((g_wdused + 1) * 2 > g_wdcap) { wdmap_rebuild(g_ndirs + 1); if (wdmap_find(g_dirs[i].wd, NULL) >= 0) return; }
    if (!g_wdcap) return;
    /* A table the rebuild could not grow (no memory) may have no room left:
     * the watch then goes without an index entry and its events unseen. */
    at = wd_home(g_dirs[i].wd);
    usize n = 0;
    while (g_wdslot[at] > 0 && ++n < g_wdcap) at = (at + 1) & (g_wdcap - 1);
    if (g_wdslot[at] > 0) return;
    if (g_wdslot[at] == 0) g_wdused++;
    g_wdslot[at] = (i32)i + 1;
}

static WDir *dir_of(int wd) {
    i32 i = wdmap_find(wd, NULL);
    return i >= 0 ? &g_dirs[i] : NULL;
}

static void dir_forget_at(usize i) {
    usize at;
    if (wdmap_find(g_dirs[i].wd, &at) >= 0) g_wdslot[at] = -1;
    free(g_dirs[i].path);
    if (g_dirs[i].root >= 0) g_root_dirs[g_dirs[i].root]--;
    g_dirs[i] = g_dirs[--g_ndirs];
    if (i < g_ndirs) wdmap_set(i);
}

/* Watches `path` for `root`. False when it could not be (gone, not a folder,
 * the system's limit, this root's limit). */
static bool dir_add(i32 root, const char *path) {
    if (g_root_dirs[root] >= WATCH_DIRS_PER_ROOT) { root_lost(root); return false; }
    int wd = inotify_add_watch(g_ifd, path, WATCH_MASK);
    if (wd < 0) {
        if (errno == ENOSPC || errno == ENOMEM) root_lost(root);
        return false;
    }
    WDir *have = dir_of(wd);
    if (have) {                 /* the same folder under another name: keep the newer */
        char *copy = strdup(path);
        if (copy) { free(have->path); have->path = copy; }
        return true;
    }
    if (g_ndirs == g_capdirs) {
        usize cap = g_capdirs ? g_capdirs * 2 : 64;
        WDir *grown = realloc(g_dirs, cap * sizeof *grown);
        if (!grown) { inotify_rm_watch(g_ifd, wd); return false; }
        g_dirs = grown;
        g_capdirs = cap;
    }
    char *copy = strdup(path);
    if (!copy) { inotify_rm_watch(g_ifd, wd); return false; }
    g_dirs[g_ndirs++] = (WDir){ wd, root, copy };
    wdmap_set(g_ndirs - 1);
    g_root_dirs[root]++;
    return true;
}

/* The folders under `path`, watched; with `made`, its files are handed over
 * as made (they may have been written before the folder had a watch). */
/* Breadth first: when a root passes its folder cap, the folders left out are
 * the deepest (build output and caches nest deeper than the sources). */
typedef struct { char *path; i32 depth; } WalkDir;

static void walk(i32 root, const char *path, bool made, i32 depth) {
    WalkDir *q = NULL;
    usize qn = 0, qcap = 0;
    char *start = strdup(path);
    if (!start) return;
    q = malloc(16 * sizeof *q);
    if (!q) { free(start); return; }
    qcap = 16;
    q[qn++] = (WalkDir){ start, depth };
    char sub[WATCH_DIR_MAX];
    for (usize qi = 0; qi < qn; qi++) {
        WalkDir cur = q[qi];
        DIR *d = cur.depth <= WATCH_DEPTH_MAX ? opendir(cur.path) : NULL;
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            const char *n = e->d_name;
            if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]))) continue;
            usize nl = strlen(n);
            if (agentctl_watch_skip_name(n, nl)) continue;
            if ((usize)snprintf(sub, sizeof sub, "%s/%s", cur.path, n) >= sizeof sub) continue;
            bool is_dir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN) {
                struct stat st;
                is_dir = lstat(sub, &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (!is_dir) {
                if (made) batch_add(sub, AGW_CREATED);
                continue;
            }
            if (!dir_add(root, sub)) continue;
            if (qn == qcap) {
                WalkDir *grown = realloc(q, qcap * 2 * sizeof *grown);
                if (!grown) continue;           /* watched, its contents not walked */
                q = grown;
                qcap *= 2;
            }
            char *copy = strdup(sub);
            if (copy) q[qn++] = (WalkDir){ copy, cur.depth + 1 };
        }
        if (d) closedir(d);
        free(cur.path);
        q[qi].path = NULL;
    }
    free(q);
}

/* Every watch on `path` and below it. */
static void dir_forget_tree(const char *path) {
    usize pl = strlen(path);
    for (usize i = 0; i < g_ndirs; ) {
        const char *p = g_dirs[i].path;
        if (!strncmp(p, path, pl) && (p[pl] == '\0' || p[pl] == '/')) {
            inotify_rm_watch(g_ifd, g_dirs[i].wd);
            dir_forget_at(i);
        } else {
            i++;
        }
    }
}

static void root_close(i32 r) {
    for (usize i = 0; i < g_ndirs; ) {
        if (g_dirs[i].root == r) {
            inotify_rm_watch(g_ifd, g_dirs[i].wd);
            dir_forget_at(i);
        } else {
            i++;
        }
    }
    g_root_on[r] = false;
    g_root_lost[r] = false;
    g_root_dirs[r] = 0;
    g_root[r][0] = '\0';
}

static bool set_has(const FolderSet *fs, const char *dir) {
    for (usize i = 0; i < fs->n; i++) if (!strcmp(fs->dirs[i], dir)) return true;
    return false;
}

/* `p` is `dir` or a path below it. */
static bool path_within(const char *p, const char *dir) {
    usize n = strlen(dir);
    return !strncmp(p, dir, n) && (p[n] == '\0' || p[n] == '/' || (n && dir[n - 1] == '/'));
}

static void apply_folders(const FolderSet *fs) {
    /* inotify keeps one watch per folder, owned by whichever root added it
     * first, so a root inside another shares the folders they have in common:
     * closing one takes those watches from the other, which walks its tree
     * again (folders still watched cost a lookup). */
    bool rewalk[AGENTCTL_WATCH_MAX] = { false };
    for (i32 r = 0; r < AGENTCTL_WATCH_MAX; r++) {
        if (!g_root_on[r] || set_has(fs, g_root[r])) continue;
        for (i32 i = 0; i < AGENTCTL_WATCH_MAX; i++)
            if (i != r && g_root_on[i] && set_has(fs, g_root[i]) &&
                (path_within(g_root[i], g_root[r]) || path_within(g_root[r], g_root[i])))
                rewalk[i] = true;
        root_close(r);
    }
    for (i32 i = 0; i < AGENTCTL_WATCH_MAX; i++)
        if (rewalk[i] && g_root_on[i] && dir_add(i, g_root[i])) walk(i, g_root[i], false, 0);
    for (usize k = 0; k < fs->n; k++) {
        i32 slot = -1;
        bool have = false;
        for (i32 i = 0; i < AGENTCTL_WATCH_MAX; i++) {
            if (g_root_on[i] && !strcmp(g_root[i], fs->dirs[k])) have = true;
            if (!g_root_on[i] && slot < 0) slot = i;
        }
        if (have || slot < 0) continue;
        memcpy(g_root[slot], fs->dirs[k], sizeof g_root[slot]);
        g_root_on[slot] = true;
        g_root_lost[slot] = false;
        g_root_dirs[slot] = 0;
        if (!dir_add(slot, g_root[slot])) { root_close(slot); continue; }
        walk(slot, g_root[slot], false, 0);
    }
}

static void handle_events(void) {
    char buf[64 * 1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t got = read(g_ifd, buf, sizeof buf);
        if (got <= 0) return;
        for (char *p = buf; p < buf + got; ) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            p += sizeof *ev + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) {
                for (i32 r = 0; r < AGENTCTL_WATCH_MAX; r++) {
                    if (!g_root_on[r]) continue;
                    batch_add(g_root[r], AGW_LOST | AGW_DIR);
                }
                continue;
            }
            WDir *d = dir_of(ev->wd);
            if (!d) continue;
            if (ev->mask & IN_IGNORED) {            /* the system dropped the watch */
                dir_forget_at((usize)(d - g_dirs));
                continue;
            }
            if (!ev->len || !ev->name[0]) {         /* the watched folder itself */
                if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) && !strcmp(d->path, g_root[d->root]))
                    root_lost(d->root);
                continue;
            }
            usize nl = strnlen(ev->name, ev->len);
            if (agentctl_watch_skip_name(ev->name, nl)) continue;
            char full[WATCH_DIR_MAX];
            if ((usize)snprintf(full, sizeof full, "%s/%.*s", d->path, (int)nl, ev->name) >= sizeof full) continue;
            i32 root = d->root;                     /* `d` may move below */
            bool is_dir = (ev->mask & IN_ISDIR) != 0;
            u32 f = is_dir ? AGW_DIR : 0;
            if (ev->mask & IN_CREATE) f |= AGW_CREATED;
            /* a rename marks both of its names, as the system's other watch
             * does; whether the path is still there is the reader's to see */
            if (ev->mask & (IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) f |= AGW_REMOVED;
            batch_add(full, f);
            if (is_dir && (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
                if (dir_add(root, full)) walk(root, full, true, 1);
            } else if (is_dir && (ev->mask & (IN_MOVED_FROM | IN_DELETE))) {
                dir_forget_tree(full);
            }
        }
    }
}

static void *watch_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        bool stop = g_stop_req;
        FolderSet *fs = g_post_set;
        g_post_set = NULL;
        u64 next_later = 0;
        for (Later *l = g_laters; l; l = l->next)
            if (!next_later || l->due_ms < next_later) next_later = l->due_ms;
        pthread_mutex_unlock(&g_mu);
        if (stop) { free(fs); break; }
        if (fs) { apply_folders(fs); free(fs); }

        u64 now = mono_ms(), due = 0;
        if (g_bn) due = g_bdue;
        if (next_later && (!due || next_later < due)) due = next_later;
        int timeout = due ? (due > now ? (int)(due - now) : 0) : -1;
        struct pollfd pf[2] = { { g_ifd, POLLIN, 0 }, { g_wake[0], POLLIN, 0 } };
        if (poll(pf, 2, timeout) < 0 && errno != EINTR) break;
        if (pf[1].revents & POLLIN) {
            char drain[64];
            while (read(g_wake[0], drain, sizeof drain) > 0) {}
        }
        if (pf[0].revents & POLLIN) handle_events();

        now = mono_ms();
        if (g_bn && now >= g_bdue) batch_flush();
        /* the delayed calls that are due, oldest first, outside the lock:
         * one may post another */
        Later *run = NULL;
        pthread_mutex_lock(&g_mu);
        for (Later **pp = &g_laters; *pp; ) {
            if ((*pp)->due_ms <= now) {
                Later *l = *pp;
                *pp = l->next;
                l->next = run;
                run = l;
            } else {
                pp = &(*pp)->next;
            }
        }
        pthread_mutex_unlock(&g_mu);
        Later *ordered = NULL;
        while (run) { Later *l = run; run = l->next; l->next = ordered; ordered = l; }
        while (ordered) {
            Later *l = ordered;
            ordered = l->next;
            if (!atomic_load(&g_stopped)) l->fn(l->ctx);
            free(l);
        }
    }
    for (usize i = 0; i < g_bn; i++) free(g_bpath[i]);
    g_bn = 0;
    memset(g_bslot, 0, sizeof g_bslot);
    for (i32 r = 0; r < AGENTCTL_WATCH_MAX; r++) if (g_root_on[r]) root_close(r);
    free(g_dirs);
    free(g_wdslot);
    g_wdslot = NULL;
    g_wdcap = g_wdused = 0;
    g_dirs = NULL;
    g_ndirs = g_capdirs = 0;
    return NULL;
}

bool agentctl_watch_available(void) { return true; }

bool agentctl_watch_start(AgentctlWatchFn fn, void *ctx) {
    if (g_started || !fn) return g_started;
    g_ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_ifd < 0) return false;
    if (pipe2(g_wake, O_NONBLOCK | O_CLOEXEC) != 0) {
        close(g_ifd);
        g_ifd = -1;
        return false;
    }
    g_fn = fn;
    g_ctx = ctx;
    atomic_store(&g_stopped, false);
    if (pthread_create(&g_thread, NULL, watch_main, NULL) != 0) {
        close(g_ifd); close(g_wake[0]); close(g_wake[1]);
        g_ifd = g_wake[0] = g_wake[1] = -1;
        return false;
    }
    g_started = true;
    return true;
}

void agentctl_watch_folders(const char *const *dirs, usize n) {
    if (!g_started || atomic_load(&g_stopped)) return;
    FolderSet *fs = calloc(1, sizeof *fs);
    if (!fs) return;
    for (usize i = 0; i < n && fs->n < AGENTCTL_WATCH_MAX; i++) {
        if (!dirs[i] || dirs[i][0] != '/' || strlen(dirs[i]) >= WATCH_DIR_MAX || set_has(fs, dirs[i])) continue;
        memcpy(fs->dirs[fs->n++], dirs[i], strlen(dirs[i]) + 1);
    }
    pthread_mutex_lock(&g_mu);
    free(g_post_set);           /* the newest set is the one that counts */
    g_post_set = fs;
    pthread_mutex_unlock(&g_mu);
    wake();
}

void agentctl_watch_later(u32 ms, void (*fn)(void *ctx), void *ctx) {
    if (!g_started || !fn || atomic_load(&g_stopped)) return;
    Later *l = malloc(sizeof *l);
    if (!l) return;
    l->due_ms = mono_ms() + ms;
    l->fn = fn;
    l->ctx = ctx;
    pthread_mutex_lock(&g_mu);
    l->next = g_laters;
    g_laters = l;
    pthread_mutex_unlock(&g_mu);
    wake();
}

void agentctl_watch_stop(void) {
    if (!g_started || atomic_load(&g_stopped)) return;
    atomic_store(&g_stopped, true);
    pthread_mutex_lock(&g_mu);
    g_stop_req = true;
    pthread_mutex_unlock(&g_mu);
    wake();
    pthread_join(g_thread, NULL);
    pthread_mutex_lock(&g_mu);
    while (g_laters) { Later *l = g_laters; g_laters = l->next; free(l); }
    free(g_post_set);
    g_post_set = NULL;
    pthread_mutex_unlock(&g_mu);
    close(g_ifd); close(g_wake[0]); close(g_wake[1]);
    g_ifd = g_wake[0] = g_wake[1] = -1;
}

#else   /* neither macOS nor Linux: nothing is watched */

bool agentctl_watch_available(void) { return false; }
bool agentctl_watch_start(AgentctlWatchFn fn, void *ctx) { (void)fn; (void)ctx; return false; }
void agentctl_watch_folders(const char *const *dirs, usize n) { (void)dirs; (void)n; }
void agentctl_watch_later(u32 ms, void (*fn)(void *ctx), void *ctx) { (void)ms; (void)fn; (void)ctx; }
void agentctl_watch_stop(void) {}

#endif
