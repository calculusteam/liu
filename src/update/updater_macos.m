/* updater_macos.m — Cocoa shim for the auto-updater (MRC, no ARC).
 * curl(1) feed fetch + artifact download (spawned via NSTask, argv-only — no
 * shell, so a feed-supplied URL cannot inject) + ditto extract + atomic install
 * + detached relaunch. No JSON/crypto here — that lives in updater.c, and the
 * downloaded artifact is SHA-256 + Ed25519 verified there regardless of how it
 * arrived, so curl is untrusted transport, NOT a trust boundary.
 */
#import <Foundation/Foundation.h>
#include "update/updater.h"
#include "update/http_status.h"

#include <mach-o/dyld.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <spawn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>

/* Absolute ceiling on a downloaded update, independent of any feed- or
 * server-declared size (a hostile server's Content-Length must not raise it). */
#define LIU_UPDATE_HARD_CAP (1024LL * 1024 * 1024)   /* 1 GiB */

extern char **environ;

/* Guards st->io_task across the session queue (completion) and main (cancel). */
static pthread_mutex_t g_task_mu = PTHREAD_MUTEX_INITIALIZER;

static void task_store(UpdateState *st, id task) {
    if (!st) return;
    pthread_mutex_lock(&g_task_mu);
    if (st->io_task) { [(id)st->io_task release]; }
    st->io_task = task ? (void *)[task retain] : NULL;
    pthread_mutex_unlock(&g_task_mu);
}
static void task_progress_if_active(UpdateState *st, id task, long long done) {
    pthread_mutex_lock(&g_task_mu);
    if (st && st->io_task == (void *)task)
        atomic_store(&st->bytes_done, done);
    pthread_mutex_unlock(&g_task_mu);
}

/* ===================== environment ===================== */

const char *updater_plat_arch(void) {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#else
    return "x86_64";
#endif
}

bool updater_plat_os_at_least(const char *min_os) {
    if (!min_os || !*min_os) return true;
    long parts[3] = {0, 0, 0};
    const char *p = min_os;
    for (i32 i = 0; i < 3 && *p; i++) {
        errno = 0;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p || errno == ERANGE || v < 0 || v > INT_MAX) return false;
        parts[i] = v;
        if (*end == '\0') { p = end; break; }
        if (*end != '.' || end[1] == '\0') return false;
        p = end + 1;
    }
    if (*p != '\0') return false;
    int maj = (int)parts[0], min = (int)parts[1], pat = (int)parts[2];
    NSOperatingSystemVersion want = { maj, min, pat };
    return [[NSProcessInfo processInfo] isOperatingSystemAtLeastVersion:want] ? true : false;
}

bool updater_plat_bundle_path(char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    char exe[PATH_MAX]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) != 0) return false;
    char real[PATH_MAX];
    if (!realpath(exe, real)) return false;
    @autoreleasepool {
        NSString *p = [NSString stringWithUTF8String:real];
        NSRange r = [p rangeOfString:@".app" options:NSBackwardsSearch];
        if (r.location == NSNotFound) return false;
        NSString *appPath = [p substringToIndex:r.location + 4];
        const char *fs = [appPath fileSystemRepresentation];
        int wrote = fs ? snprintf(out, cap, "%s", fs) : -1;
        if (wrote < 0 || (size_t)wrote >= cap) { out[0] = '\0'; return false; }
    }
    return true;
}

bool updater_plat_can_autoinstall(void) {
    char exe[PATH_MAX]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) != 0) return false;
    char real[PATH_MAX];
    if (!realpath(exe, real)) return false;
    @autoreleasepool {
        NSString *p = [NSString stringWithUTF8String:real];
        if (![p containsString:@".app/Contents/MacOS/"]) return false;   /* bare binary */
        if ([p containsString:@"/build/"] || [p containsString:@"/build_release/"])
            return false;                                                /* dev/build tree */
        return true;
    }
}

/* ===================== feed fetch (curl -> stdout) =====================
 * curl is spawned with an argv array (never a shell string), so a feed/URL
 * containing shell metacharacters cannot break out. https is forced on both the
 * request and any redirect, mirroring updater.c's scheme check. Runs on a
 * background queue and hands results back through the _Atomic fields in
 * UpdateState, so updater.c's state machine never touches Cocoa or curl. */

void updater_plat_fetch_feed(UpdateState *st, const char *url) {
    if (!st || !url || !*url) {
        if (st) {
            snprintf(st->io_err, sizeof st->io_err, "invalid update URL");
            atomic_store(&st->io_ok, false);
            atomic_store(&st->io_done, true);
        }
        return;
    }
    @autoreleasepool {
        NSString *u = [NSString stringWithUTF8String:url];
        if (!u) {
            snprintf(st->io_err, sizeof st->io_err, "invalid update URL");
            atomic_store(&st->io_ok, false);
            atomic_store(&st->io_done, true);
            return;
        }
        /* Configure + task_store() SYNCHRONOUSLY before dispatch so cancel is
         * never a no-op against a NULL io_task while curl runs. */
        NSTask *t = [[NSTask alloc] init];
        [t setLaunchPath:@"/usr/bin/curl"];
        /* --globoff: a feed-supplied `[1-9]` / `{a,b}` is a name, not nine requests.
         * --max-redirs: a redirect loop ends. The body is stdout, so the last
         * response's status goes to stderr, which carries nothing else (-s
         * without -S). */
        [t setArguments:@[ @"-fsL",
                           @"--proto", @"=https", @"--proto-redir", @"=https",
                           @"--globoff", @"--max-redirs", @"5",
                           @"--max-time", @"20",
                           @"--max-filesize", @"5242880",   /* feeds are tiny */
                           @"--write-out", @"%{stderr}%{http_code}",
                           u ]];
        NSPipe *out = [NSPipe pipe];
        NSPipe *errp = [NSPipe pipe];
        [t setStandardOutput:out];
        [t setStandardError:errp];
        task_store(st, t);
        atomic_fetch_add_explicit(&st->io_workers, 1, memory_order_release);
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            @autoreleasepool {
                NSData *data = nil;
                int rc = -1;
                int http = 0;
                bool over = false;
                @try {
                    [t launch];
                    /* Read incrementally with an ACTIVE byte ceiling. --max-filesize
                     * only refuses a KNOWN Content-Length up front; a chunked /
                     * unknown-length hostile feed would otherwise be buffered
                     * unbounded into RAM (OOM DoS). Abort curl the moment the
                     * accumulated body exceeds the 5 MiB feed cap. Draining the pipe
                     * as it fills also avoids the full-pipe deadlock. */
                    NSFileHandle *rh = [out fileHandleForReading];
                    NSMutableData *acc = [NSMutableData data];
                    const NSUInteger FEED_CAP = 5u * 1024 * 1024;
                    for (;;) {
                        NSData *chunk = [rh readDataOfLength:65536];
                        if (!chunk || [chunk length] == 0) break;   /* EOF */
                        if ([acc length] + [chunk length] > FEED_CAP) {
                            over = true;
                            @try { [t terminate]; } @catch (NSException *e) { (void)e; }
                            break;
                        }
                        [acc appendData:chunk];
                    }
                    [t waitUntilExit];
                    rc = [t terminationStatus];
                    if (!over) data = acc;
                    /* A few bytes, written as curl ends: read once it has. */
                    NSData *ed = [[errp fileHandleForReading] readDataToEndOfFile];
                    http = updater_http_status((const char *)[ed bytes], (size_t)[ed length]);
                } @catch (NSException *e) { (void)e; }
                bool ok = false;
                char *body = NULL;
                size_t body_len = 0;
                char local_err[200] = {0};
                if (rc == 0 && !over && data && [data length] > 0) {
                    size_t len = (size_t)[data length];
                    body = (char *)malloc(len + 1);
                    if (body) {
                        memcpy(body, [data bytes], len);
                        body[len] = '\0';
                        body_len = len;
                        ok = true;
                    } else {
                        snprintf(local_err, sizeof local_err, "out of memory");
                    }
                } else {
                    snprintf(local_err, sizeof local_err,
                             "Couldn't reach the update server (curl %d).", rc);
                }
                /* Publish only if this is still the active task. Cancel or a
                 * newer request replaces io_task under the same mutex; a late
                 * callback then discards its local result instead of clearing
                 * the new task and racing its feed_body/io_err fields. */
                pthread_mutex_lock(&g_task_mu);
                bool active = st->io_task == (void *)t;
                if (active) {
                    [(id)st->io_task release];
                    st->io_task = NULL;
                    if (ok) {
                        st->feed_body = body;
                        atomic_store(&st->feed_len, (long)body_len);
                        body = NULL;
                    } else {
                        snprintf(st->io_err, sizeof st->io_err, "%s", local_err);
                    }
                    atomic_store(&st->io_http, http);
                    atomic_store(&st->io_ok, ok);
                    atomic_store(&st->io_done, true);
                }
                pthread_mutex_unlock(&g_task_mu);
                free(body);
                [t release];
                /* Last access through st. updater_shutdown may free the
                 * containing AppState as soon as this reaches zero. */
                atomic_fetch_sub_explicit(&st->io_workers, 1, memory_order_release);
            }
        });
    }
}

/* ===================== artifact download (curl -o) =====================
 * Same argv-only, https-forced curl invocation, writing into an exclusively
 * created mode-0700 temp dir. --max-filesize is the streaming oversize guard
 * (updater.c re-stats the file before slurping it, too). Live progress comes
 * from polling the growing file rather than parsing curl's stderr. */

void updater_plat_download(UpdateState *st, const char *url, long long sz) {
    if (!st || !url || !*url) {
        if (st) {
            snprintf(st->io_err, sizeof st->io_err, "invalid update URL");
            atomic_store(&st->io_ok, false);
            atomic_store(&st->io_done, true);
        }
        return;
    }
    @autoreleasepool {
        NSString *u = [NSString stringWithUTF8String:url];
        if (!u) {
            snprintf(st->io_err, sizeof st->io_err, "invalid update URL");
            atomic_store(&st->io_ok, false);
            atomic_store(&st->io_done, true);
            return;
        }
        /* FEED-declared size (trusted) + 5% slack, capped at the hard ceiling.
         * Clamp the feed-supplied size to the hard cap and compute in unsigned
         * 64-bit *before* the add, mirroring updater.c — a hostile feed size near
         * LLONG_MAX would otherwise overflow the signed add (UB / wrap-negative)
         * and yield a negative --max-filesize, defeating the streaming bound. */
        long long cap = LIU_UPDATE_HARD_CAP;
        if (sz > 0) {
            unsigned long long usz = (unsigned long long)sz;
            if (usz > (unsigned long long)LIU_UPDATE_HARD_CAP) usz = (unsigned long long)LIU_UPDATE_HARD_CAP;
            unsigned long long fc = usz + usz / 20 + 65536;   /* usz <= 1 GiB, can't overflow */
            if (fc < (unsigned long long)cap) cap = (long long)fc;
        }
        atomic_store(&st->bytes_total, sz);
        atomic_store(&st->bytes_done, 0);
        NSString *capStr = [NSString stringWithFormat:@"%lld", cap];

        /* Create the temp dir + configure the task SYNCHRONOUSLY on the caller
         * thread and task_store() it BEFORE dispatch, so updater_plat_cancel can
         * never observe a NULL io_task while curl is (about to be) running.
         * mkdtemp is cheap; only the network leg runs on the worker. */
        char tmpl[1100];
        const char *tmp_root = [NSTemporaryDirectory() fileSystemRepresentation];
        int tmpl_n = tmp_root ? snprintf(tmpl, sizeof tmpl, "%s/liu-update-XXXXXX", tmp_root) : -1;
        if (tmpl_n < 0 || (usize)tmpl_n >= sizeof tmpl) {
            snprintf(st->io_err, sizeof st->io_err, "temp path is too long");
            atomic_store(&st->io_ok, false);
            atomic_store(&st->io_done, true);
            return;
        }
        char *made = mkdtemp(tmpl);
        if (!made) {
            snprintf(st->io_err, sizeof st->io_err, "couldn't create a temp dir");
            atomic_store(&st->io_ok, false);
            atomic_store(&st->io_done, true);
            return;
        }
        NSString *dir = [NSString stringWithUTF8String:made];
        NSString *dst = [dir stringByAppendingPathComponent:@"update.zip"];

        NSTask *t = [[NSTask alloc] init];
        [t setLaunchPath:@"/usr/bin/curl"];
        /* The body goes to -o, so stdout carries only the last response's
         * status (--write-out). */
        [t setArguments:@[ @"-fSL",
                           @"--proto", @"=https", @"--proto-redir", @"=https",
                           @"--globoff", @"--max-redirs", @"5",
                           @"--max-time", @"600",
                           @"--max-filesize", capStr,
                           @"--write-out", @"%{http_code}",
                           @"-o", dst, u ]];
        NSPipe *outp = [NSPipe pipe];
        [t setStandardOutput:outp];
        [t setStandardError:[NSFileHandle fileHandleWithNullDevice]];
        task_store(st, t);
        atomic_fetch_add_explicit(&st->io_workers, 1, memory_order_release);

        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            @autoreleasepool {
                /* dst/dir are retained by the block copy; re-derive the C path
                 * inside the block's pool. */
                const char *dstc = [dst fileSystemRepresentation];
                int rc = -1;
                int http = 0;
                bool oversize = false;
                /* Launch under the task lock, and only while this is still the
                 * active task. updater_plat_cancel() NULLs st->io_task under the
                 * SAME lock, so a cancel that raced ahead makes this launch
                 * impossible (io_task != t -> skipped); a cancel that arrives
                 * after launch sees a live task and terminates it. curl therefore
                 * never starts AFTER cancellation (e.g. quitting during the
                 * startup autocheck). */
                bool launched = false;
                pthread_mutex_lock(&g_task_mu);
                if (st->io_task == (void *)t) {
                    @try { [t launch]; launched = true; } @catch (NSException *e) { (void)e; }
                }
                pthread_mutex_unlock(&g_task_mu);
                if (launched) @try {
                    while ([t isRunning]) {              /* live progress */
                        struct stat sb;
                        if (stat(dstc, &sb) == 0) {
                            task_progress_if_active(st, t, (long long)sb.st_size);
                            /* ACTIVE oversize cap: --max-filesize only refuses a
                             * KNOWN Content-Length up front; a chunked/unknown
                             * hostile stream is otherwise unbounded on disk.
                             * Abort curl the moment bytes-on-disk exceed cap. */
                            if ((long long)sb.st_size > cap) {
                                oversize = true;
                                @try { [t terminate]; } @catch (NSException *e) { (void)e; }
                                break;
                            }
                        }
                        usleep(150000);
                    }
                    [t waitUntilExit];
                    rc = [t terminationStatus];
                    NSData *od = [[outp fileHandleForReading] readDataToEndOfFile];
                    http = updater_http_status((const char *)[od bytes], (size_t)[od length]);
                } @catch (NSException *e) { (void)e; }
                bool ok = false;
                char local_err[200] = {0};
                char local_path[1024] = {0};
                if (rc == 0 && !oversize) {
                    struct stat sb;
                    if (stat(dstc, &sb) == 0)
                        task_progress_if_active(st, t, (long long)sb.st_size);
                    int pn = snprintf(local_path, sizeof local_path, "%s", dstc);
                    ok = pn >= 0 && (usize)pn < sizeof local_path;
                    if (!ok) snprintf(local_err, sizeof local_err, "download path is too long");
                } else {
                    if (oversize)
                        snprintf(local_err, sizeof local_err,
                                 "Download exceeded the %lld-byte size limit.", cap);
                    else if (launched)
                        snprintf(local_err, sizeof local_err, "Download failed (curl %d).", rc);
                }
                pthread_mutex_lock(&g_task_mu);
                bool active = st->io_task == (void *)t;
                if (active) {
                    [(id)st->io_task release];
                    st->io_task = NULL;
                    if (ok) {
                        int pn = snprintf(st->downloaded_path,
                                          sizeof st->downloaded_path, "%s", local_path);
                        if (pn < 0 || (usize)pn >= sizeof st->downloaded_path) {
                            st->downloaded_path[0] = '\0';
                            ok = false;
                            snprintf(local_err, sizeof local_err, "download path is too long");
                        }
                    }
                    if (!ok) snprintf(st->io_err, sizeof st->io_err, "%s", local_err);
                    atomic_store(&st->io_http, http);
                    atomic_store(&st->io_ok, ok);
                    atomic_store(&st->io_done, true);
                }
                pthread_mutex_unlock(&g_task_mu);
                if (!active || !ok)
                    [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
                [t release];
                /* Last access through st; pairs with updater_shutdown. */
                atomic_fetch_sub_explicit(&st->io_workers, 1, memory_order_release);
            }
        });
    }
}

void updater_plat_cancel(UpdateState *st) {
    if (!st) return;
    pthread_mutex_lock(&g_task_mu);
    if (st->io_task) {
        /* -terminate raises NSInvalidArgumentException if the task was never
         * launched (cancel can race ahead of [t launch] in the worker block)
         * or has already exited — guard it like the rest of this file does. */
        @try { [(id)st->io_task terminate]; }
        @catch (NSException *e) { (void)e; }
        [(id)st->io_task release];
        st->io_task = NULL;
    }
    pthread_mutex_unlock(&g_task_mu);
}

/* ===================== install helpers (synchronous) ===================== */

static int run_task(NSString *launch, NSArray *args) {
    @autoreleasepool {
        NSTask *t = [[NSTask alloc] init];
        [t setLaunchPath:launch];
        [t setArguments:args];
        [t setStandardOutput:[NSFileHandle fileHandleWithNullDevice]];
        [t setStandardError:[NSFileHandle fileHandleWithNullDevice]];
        int rc = -1;
        @try { [t launch]; [t waitUntilExit]; rc = [t terminationStatus]; }
        @catch (NSException *e) { (void)e; rc = -1; }
        [t release];
        return rc;
    }
}

bool updater_plat_extract(const char *zip_path, char *out_app_path, size_t cap) {
    if (!zip_path || !*zip_path || !out_app_path || cap == 0) return false;
    out_app_path[0] = '\0';
    @autoreleasepool {
        NSString *zip = [NSString stringWithUTF8String:zip_path];
        if (!zip) return false;
        NSString *dest = [[zip stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"x"];
        [[NSFileManager defaultManager] removeItemAtPath:dest error:nil];
        if (run_task(@"/usr/bin/ditto", @[@"-x", @"-k", zip, dest]) != 0) return false;
        /* find the single *.app inside dest */
        NSArray *items = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dest error:nil];
        for (NSString *it in items) {
            if ([it hasSuffix:@".app"]) {
                NSString *app = [dest stringByAppendingPathComponent:it];
                const char *fs = [app fileSystemRepresentation];
                int n = fs ? snprintf(out_app_path, cap, "%s", fs) : -1;
                if (n < 0 || (size_t)n >= cap) { out_app_path[0] = '\0'; return false; }
                return true;
            }
        }
        return false;
    }
}

bool updater_plat_bundle_version(const char *app_path, char *out, size_t cap) {
    if (!app_path || !*app_path || !out || cap == 0) return false;
    out[0] = '\0';
    @autoreleasepool {
        NSString *app = [NSString stringWithUTF8String:app_path];
        if (!app) return false;
        NSString *plist = [app
                           stringByAppendingPathComponent:@"Contents/Info.plist"];
        NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:plist];
        NSString *v = d ? d[@"CFBundleShortVersionString"] : nil;
        if (![v isKindOfClass:[NSString class]]) return false;
        const char *vs = [v UTF8String];
        int n = vs ? snprintf(out, cap, "%s", vs) : -1;
        if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return false; }
        return true;
    }
}

void updater_plat_strip_quarantine(const char *path) {
    if (!path || !*path) return;
    @autoreleasepool {
        run_task(@"/usr/bin/xattr", @[@"-dr", @"com.apple.quarantine",
                                      [NSString stringWithUTF8String:path]]);
    }
}

bool updater_plat_install(const char *new_app, char *out_installed, size_t cap) {
    if (!new_app || !*new_app || !out_installed || cap == 0) return false;
    out_installed[0] = '\0';
    @autoreleasepool {
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *newApp = [NSString stringWithUTF8String:new_app];
        if (!newApp) return false;

        /* Target: the running bundle if its parent is writable, else ~/Applications. */
        NSString *target = nil;
        char bundle[PATH_MAX];
        if (updater_plat_bundle_path(bundle, sizeof bundle)) {
            NSString *b = [NSString stringWithUTF8String:bundle];
            NSString *parent = [b stringByDeletingLastPathComponent];
            if (access([parent fileSystemRepresentation], W_OK) == 0) target = b;
        }
        if (!target) {
            NSString *apps = [NSHomeDirectory() stringByAppendingPathComponent:@"Applications"];
            [fm createDirectoryAtPath:apps withIntermediateDirectories:YES attributes:nil error:nil];
            target = [apps stringByAppendingPathComponent:@"Liu.app"];
        }

        /* Copy the new bundle into a sibling temp FIRST (target stays intact
         * during the slow ditto, which is cross-volume safe and preserves the
         * signature/xattrs), then swap it in with two fast same-volume renames.
         * This shrinks the "app missing / partial bundle" window from the whole
         * copy to the gap between two renames.
         *
         * Stage inside a private mkdtemp dir (0700, unpredictable suffix) in the
         * target's parent rather than a predictable "Liu.app.new-<pid>": the
         * parent may be group-writable (/Applications is admin-writable), where
         * another local user could otherwise pre-create or swap the staging path
         * between ditto and the rename and have THEIR bundle installed. Mirrors
         * install.sh's mktemp staging. */
        NSString *parent = [target stringByDeletingLastPathComponent];
        char stage_tmpl[PATH_MAX];
        const char *parentc = [parent fileSystemRepresentation];
        int sn = parentc ? snprintf(stage_tmpl, sizeof stage_tmpl,
                                    "%s/.liu-stage.XXXXXX", parentc) : -1;
        if (sn < 0 || (size_t)sn >= sizeof stage_tmpl) return false;
        if (!mkdtemp(stage_tmpl)) return false;
        NSString *stageDir = [NSString stringWithUTF8String:stage_tmpl];
        NSString *staged = [stageDir stringByAppendingPathComponent:
                                         [target lastPathComponent]];
        if (run_task(@"/usr/bin/ditto", @[newApp, staged]) != 0) {
            [fm removeItemAtPath:stageDir error:nil];
            return false;
        }
        NSString *old = nil;
        if ([fm fileExistsAtPath:target]) {
            old = [stageDir stringByAppendingPathComponent:@"old"];
            if (![fm moveItemAtPath:target toPath:old error:nil]) {
                [fm removeItemAtPath:stageDir error:nil];
                return false;
            }
        }
        if (![fm moveItemAtPath:staged toPath:target error:nil]) {
            /* Put the old app back — and delete the stage directory only once
             * it IS back. If this move fails too, `old` inside that directory
             * is the only copy of the app left: removing the directory here
             * would leave no Liu at all. It stays where it is (a hidden folder
             * beside the app) and the path goes to stderr. */
            BOOL restored = !old || [fm moveItemAtPath:old toPath:target error:nil];
            if (restored) {
                [fm removeItemAtPath:stageDir error:nil];
            } else {
                [fm removeItemAtPath:staged error:nil];
                fprintf(stderr, "liu: update: could not put the previous app back; it is kept at %s\n",
                        [old fileSystemRepresentation]);
            }
            return false;
        }
        [fm removeItemAtPath:stageDir error:nil];
        const char *fs = [target fileSystemRepresentation];
        int n = fs ? snprintf(out_installed, cap, "%s", fs) : -1;
        if (n < 0 || (size_t)n >= cap) { out_installed[0] = '\0'; return false; }
        return true;
    }
}

void updater_plat_relaunch_and_quit(const char *installed_app_path) {
    if (!installed_app_path || !*installed_app_path) return;
    /* The app path is passed as a positional argument ($1), NOT interpolated
     * into the script text, so a path containing shell metacharacters can't
     * break out of the command. Only the PID (an integer) is interpolated. */
    char script[160];
    snprintf(script, sizeof script,
             "while /bin/kill -0 %d 2>/dev/null; do /bin/sleep 0.2; done; "
             "exec /usr/bin/open \"$1\"", (int)getpid());
    char *argv[] = { (char *)"/bin/sh", (char *)"-c", script,
                     (char *)"liu-relaunch", (char *)installed_app_path, NULL };
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0) return;
    (void)posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);   /* detach into its own session */
    pid_t child;
    posix_spawn(&child, "/bin/sh", NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
}

void updater_plat_cleanup_temp(const char *path) {
    if (!path || !*path) return;
    @autoreleasepool {
        /* remove the whole liu-update-<pid> dir (the zip's parent), not just the file */
        NSString *p = [NSString stringWithUTF8String:path];
        NSString *dir = [p stringByDeletingLastPathComponent];
        if ([[dir lastPathComponent] hasPrefix:@"liu-update-"])
            [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
        else
            [[NSFileManager defaultManager] removeItemAtPath:p error:nil];
    }
}
