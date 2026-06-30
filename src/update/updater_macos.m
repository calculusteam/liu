/* updater_macos.m — Cocoa shim for the auto-updater (MRC, no ARC).
 * curl(1) feed fetch + artifact download (spawned via NSTask, argv-only — no
 * shell, so a feed-supplied URL cannot inject) + ditto extract + atomic install
 * + detached relaunch. No JSON/crypto here — that lives in updater.c, and the
 * downloaded artifact is SHA-256 + Ed25519 verified there regardless of how it
 * arrived, so curl is untrusted transport, NOT a trust boundary.
 */
#import <Foundation/Foundation.h>
#include "update/updater.h"

#include <mach-o/dyld.h>
#include <limits.h>
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
    pthread_mutex_lock(&g_task_mu);
    if (st->io_task) { [(id)st->io_task release]; }
    st->io_task = task ? (void *)[task retain] : NULL;
    pthread_mutex_unlock(&g_task_mu);
}
static void task_clear(UpdateState *st) {
    pthread_mutex_lock(&g_task_mu);
    if (st->io_task) { [(id)st->io_task release]; st->io_task = NULL; }
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
    int maj = 0, min = 0, pat = 0;
    sscanf(min_os, "%d.%d.%d", &maj, &min, &pat);
    NSOperatingSystemVersion want = { maj, min, pat };
    return [[NSProcessInfo processInfo] isOperatingSystemAtLeastVersion:want] ? true : false;
}

bool updater_plat_bundle_path(char *out, size_t cap) {
    char exe[PATH_MAX]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) != 0) return false;
    char real[PATH_MAX];
    if (!realpath(exe, real)) return false;
    @autoreleasepool {
        NSString *p = [NSString stringWithUTF8String:real];
        NSRange r = [p rangeOfString:@".app" options:NSBackwardsSearch];
        if (r.location == NSNotFound) return false;
        NSString *appPath = [p substringToIndex:r.location + 4];
        snprintf(out, cap, "%s", [appPath fileSystemRepresentation]);
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
    @autoreleasepool {
        NSString *u = [NSString stringWithUTF8String:(url ? url : "")];
        /* Configure + task_store() SYNCHRONOUSLY before dispatch so cancel is
         * never a no-op against a NULL io_task while curl runs (the old
         * dispatch-then-store ordering left that window open). */
        NSTask *t = [[NSTask alloc] init];
        [t setLaunchPath:@"/usr/bin/curl"];
        [t setArguments:@[ @"-fsSL",
                           @"--proto", @"=https", @"--proto-redir", @"=https",
                           @"--max-time", @"20",
                           @"--max-filesize", @"5242880",   /* feeds are tiny */
                           u ]];
        NSPipe *out = [NSPipe pipe];
        [t setStandardOutput:out];
        [t setStandardError:[NSFileHandle fileHandleWithNullDevice]];
        task_store(st, t);
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            @autoreleasepool {
                NSData *data = nil;
                int rc = -1;
                @try {
                    [t launch];
                    /* read to EOF first (drains the pipe), THEN wait — avoids a
                     * full-pipe deadlock. */
                    data = [[out fileHandleForReading] readDataToEndOfFile];
                    [t waitUntilExit];
                    rc = [t terminationStatus];
                } @catch (NSException *e) { (void)e; }
                task_clear(st);
                bool ok = false;
                if (rc == 0 && data && [data length] > 0) {
                    size_t len = (size_t)[data length];
                    char *body = (char *)malloc(len + 1);
                    if (body) {
                        memcpy(body, [data bytes], len);
                        body[len] = '\0';
                        st->feed_body = body;
                        atomic_store(&st->feed_len, (long)len);
                        ok = true;
                    } else {
                        snprintf(st->io_err, sizeof st->io_err, "out of memory");
                    }
                } else {
                    snprintf(st->io_err, sizeof st->io_err,
                             "Couldn't reach the update server (curl %d).", rc);
                }
                [t release];
                atomic_store(&st->io_ok, ok);
                atomic_store(&st->io_done, true);
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
    @autoreleasepool {
        NSString *u = [NSString stringWithUTF8String:(url ? url : "")];
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
         * never observe a NULL io_task while curl is (about to be) running — the
         * old dispatch-then-store ordering left a window where cancel was a
         * no-op. mkdtemp is cheap; only the network leg runs on the worker. */
        char tmpl[1100];
        snprintf(tmpl, sizeof tmpl, "%s/liu-update-XXXXXX",
                 [NSTemporaryDirectory() fileSystemRepresentation]);
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
        [t setArguments:@[ @"-fSL",
                           @"--proto", @"=https", @"--proto-redir", @"=https",
                           @"--max-time", @"600",
                           @"--max-filesize", capStr,
                           @"-o", dst, u ]];
        [t setStandardOutput:[NSFileHandle fileHandleWithNullDevice]];
        [t setStandardError:[NSFileHandle fileHandleWithNullDevice]];
        task_store(st, t);

        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            @autoreleasepool {
                /* dst/dir are retained by the block copy; re-derive the C path
                 * inside the block's pool. */
                const char *dstc = [dst fileSystemRepresentation];
                int rc = -1;
                bool oversize = false;
                /* Launch under the task lock, and only while we are still the
                 * active task. updater_plat_cancel() NULLs st->io_task under the
                 * SAME lock, so a cancel that raced ahead of us makes this launch
                 * impossible (io_task != t -> we skip it); a cancel that arrives
                 * after launch sees a live task and terminates it. Closes the
                 * cancel/teardown race where curl would start AFTER cancellation
                 * (e.g. quitting during the startup autocheck). */
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
                            atomic_store(&st->bytes_done, (long long)sb.st_size);
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
                } @catch (NSException *e) { (void)e; }
                task_clear(st);
                bool ok = false;
                if (rc == 0 && !oversize) {
                    struct stat sb;
                    if (stat(dstc, &sb) == 0)
                        atomic_store(&st->bytes_done, (long long)sb.st_size);
                    snprintf(st->downloaded_path, sizeof st->downloaded_path, "%s", dstc);
                    ok = true;
                } else {
                    if (oversize)
                        snprintf(st->io_err, sizeof st->io_err,
                                 "Download exceeded the %lld-byte size limit.", cap);
                    else if (launched)
                        snprintf(st->io_err, sizeof st->io_err, "Download failed (curl %d).", rc);
                    /* else: canceled before launch — curl never ran, leave io_err
                     * empty; just clean up the temp dir. */
                    [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
                }
                [t release];
                atomic_store(&st->io_ok, ok);
                atomic_store(&st->io_done, true);
            }
        });
    }
}

void updater_plat_cancel(UpdateState *st) {
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
    @autoreleasepool {
        NSString *zip = [NSString stringWithUTF8String:zip_path];
        NSString *dest = [[zip stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"x"];
        [[NSFileManager defaultManager] removeItemAtPath:dest error:nil];
        if (run_task(@"/usr/bin/ditto", @[@"-x", @"-k", zip, dest]) != 0) return false;
        /* find the single *.app inside dest */
        NSArray *items = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dest error:nil];
        for (NSString *it in items) {
            if ([it hasSuffix:@".app"]) {
                NSString *app = [dest stringByAppendingPathComponent:it];
                snprintf(out_app_path, cap, "%s", [app fileSystemRepresentation]);
                return true;
            }
        }
        return false;
    }
}

bool updater_plat_bundle_version(const char *app_path, char *out, size_t cap) {
    @autoreleasepool {
        NSString *plist = [[NSString stringWithUTF8String:app_path]
                           stringByAppendingPathComponent:@"Contents/Info.plist"];
        NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:plist];
        NSString *v = d ? d[@"CFBundleShortVersionString"] : nil;
        if (![v isKindOfClass:[NSString class]]) return false;
        snprintf(out, cap, "%s", [v UTF8String]);
        return true;
    }
}

void updater_plat_strip_quarantine(const char *path) {
    @autoreleasepool {
        run_task(@"/usr/bin/xattr", @[@"-dr", @"com.apple.quarantine",
                                      [NSString stringWithUTF8String:path]]);
    }
}

bool updater_plat_install(const char *new_app, char *out_installed, size_t cap) {
    @autoreleasepool {
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *newApp = [NSString stringWithUTF8String:new_app];

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
         * copy to the gap between two renames. */
        NSString *staged = [target stringByAppendingFormat:@".new-%d", (int)getpid()];
        [fm removeItemAtPath:staged error:nil];
        if (run_task(@"/usr/bin/ditto", @[newApp, staged]) != 0) {
            [fm removeItemAtPath:staged error:nil];
            return false;
        }
        NSString *old = nil;
        if ([fm fileExistsAtPath:target]) {
            old = [target stringByAppendingFormat:@".old-%d", (int)getpid()];
            [fm removeItemAtPath:old error:nil];
            if (![fm moveItemAtPath:target toPath:old error:nil]) {
                [fm removeItemAtPath:staged error:nil];
                return false;
            }
        }
        if (![fm moveItemAtPath:staged toPath:target error:nil]) {
            if (old) [fm moveItemAtPath:old toPath:target error:nil];   /* restore */
            [fm removeItemAtPath:staged error:nil];
            return false;
        }
        if (old) [fm removeItemAtPath:old error:nil];
        snprintf(out_installed, cap, "%s", [target fileSystemRepresentation]);
        return true;
    }
}

void updater_plat_relaunch_and_quit(const char *installed_app_path) {
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
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);   /* detach into its own session */
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
