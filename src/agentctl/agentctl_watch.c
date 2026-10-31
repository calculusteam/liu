/*
 * Liu — the folders a team works in, watched for changed files. See
 * agentctl_watch.h.
 *
 * Everything this module keeps lives on its own serial queue: the streams
 * are made, changed and stopped there, their callbacks run there, and so do
 * the delayed calls the caller asks for. Nothing here takes a lock.
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

#else   /* not macOS: nothing is watched */

bool agentctl_watch_available(void) { return false; }
bool agentctl_watch_start(AgentctlWatchFn fn, void *ctx) { (void)fn; (void)ctx; return false; }
void agentctl_watch_folders(const char *const *dirs, usize n) { (void)dirs; (void)n; }
void agentctl_watch_later(u32 ms, void (*fn)(void *ctx), void *ctx) { (void)ms; (void)fn; (void)ctx; }
void agentctl_watch_stop(void) {}

#endif
