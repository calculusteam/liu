/*
 * liu-notify - macOS TTS via AVSpeechSynthesizer (AVFoundation).
 *
 * AVSpeechSynthesizer is preferred over the soft-deprecated NSSpeechSynthesizer
 * and over shelling to `say`. Single long-lived synthesizer kept in a file-
 * scope strong ARC reference — recreating per utterance costs ~100 ms of
 * audio-unit setup.
 */
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>     /* NSSound for audio file playback */
#import <CoreGraphics/CoreGraphics.h>  /* CGWindowListCopyWindowInfo */
#import <Foundation/Foundation.h>

#include "notify/platform_notify.h"
#include "core/types.h"

#include <math.h>

static AVSpeechSynthesizer *g_synth = nil;

static float clamp_rate(f32 rate) {
    /* Caller passes 0.5..2.0, where 1.0 == AVSpeechUtteranceDefaultSpeechRate. */
    if (!(rate == rate)) return AVSpeechUtteranceDefaultSpeechRate; /* NaN */
    if (rate < 0.5f) rate = 0.5f;
    if (rate > 2.0f) rate = 2.0f;
    float mn = AVSpeechUtteranceMinimumSpeechRate;
    float mx = AVSpeechUtteranceMaximumSpeechRate;
    float def = AVSpeechUtteranceDefaultSpeechRate;
    if (rate <= 1.0f) {
        float t = (rate - 0.5f) / 0.5f;       /* 0..1 */
        return mn + t * (def - mn);
    }
    float t = (rate - 1.0f) / 1.0f;           /* 0..1 */
    return def + t * (mx - def);
}

bool tts_init(void) {
    @autoreleasepool {
        if (g_synth == nil) {
            g_synth = [[AVSpeechSynthesizer alloc] init];
        }
        return g_synth != nil;
    }
}

void tts_shutdown(void) {
    @autoreleasepool {
        if (g_synth) {
            [g_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
            [g_synth release];   /* MRC: this file is built without -fobjc-arc */
            g_synth = nil;
        }
    }
}

void tts_speak(const char *text, const char *voice, f32 rate) {
    if (!text || !*text) return;
    @autoreleasepool {
        if (g_synth == nil && !tts_init()) return;

        NSString *ns = [NSString stringWithUTF8String:text];
        if (ns == nil) return;  /* invalid UTF-8 slipped past validation */

        AVSpeechUtterance *u = [AVSpeechUtterance speechUtteranceWithString:ns];
        u.rate = clamp_rate(rate);

        AVSpeechSynthesisVoice *v = nil;
        if (voice && *voice) {
            NSString *vstr = [NSString stringWithUTF8String:voice];
            if (vstr) {
                v = [AVSpeechSynthesisVoice voiceWithIdentifier:vstr];
                if (v == nil) v = [AVSpeechSynthesisVoice voiceWithLanguage:vstr];
            }
        }
        if (v == nil) {
            NSString *lang = [AVSpeechSynthesisVoice currentLanguageCode];
            if (lang) v = [AVSpeechSynthesisVoice voiceWithLanguage:lang];
        }
        if (v) u.voice = v;

        [g_synth speakUtterance:u];
    }
}

bool tts_busy(void) {
    @autoreleasepool {
        return g_synth != nil && [g_synth isSpeaking];
    }
}

void tts_cancel(void) {
    @autoreleasepool {
        if (g_synth) {
            [g_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
        }
    }
}

/* --- Sound file playback via NSSound --------------------------------------
 *
 * NSSound is kept as a file-scope reference so it isn't released mid-playback
 * (MRC — this file is built without -fobjc-arc, so every reassign/clear of
 * g_sound must [release] the old value). We replace the previous sound on each
 * call — overlapping
 * playback is rarely what the user wants and the queue's rate limiter already
 * caps firing frequency.
 *
 * `notify_sound_busy()` can't trust `[NSSound isPlaying]`: the liu-notify
 * daemon is a plain C poll() loop with no NSRunLoop on the main thread, so
 * NSSound's completion notifications never fire and `isPlaying` returns YES
 * forever. We sidestep that by reading the sound's `duration` up front and
 * marking the deadline; busy returns true only until that deadline elapses,
 * which lets the queue drain reliably for subsequent events. */
#include <time.h>
static NSSound *g_sound = nil;
static double   g_sound_end_mono = 0.0;

static double notify_mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

bool notify_play_sound(const char *path) {
    if (!path || !*path) return false;
    @autoreleasepool {
        NSString *p = [NSString stringWithUTF8String:path];
        if (p == nil) return false;
        if (![[NSFileManager defaultManager] isReadableFileAtPath:p]) return false;

        NSSound *s = [[NSSound alloc] initWithContentsOfFile:p byReference:YES];
        if (s == nil) return false;

        /* Replace any in-flight sound. MRC: release the previous +1 before
         * overwriting, else every played sound leaks an NSSound. */
        if (g_sound) { [g_sound stop]; [g_sound release]; }
        g_sound = s;   /* s is +1 from alloc/init; ownership moves to g_sound */
        if (![g_sound play]) {
            [g_sound release];
            g_sound = nil;
            g_sound_end_mono = 0.0;
            return false;
        }
        /* Pad slightly so we don't mark "done" while CoreAudio is still
         * draining the last buffers (perceptual rather than measured). */
        double dur = [s duration];
        if (dur < 0.05) dur = 0.05;
        g_sound_end_mono = notify_mono_now() + dur + 0.05;
        return true;
    }
}

bool notify_sound_busy(void) {
    if (g_sound_end_mono <= 0.0) return false;
    if (notify_mono_now() < g_sound_end_mono) return true;
    /* Past the deadline — release + drop the ref so the next play starts
     * with a clean slate (and so [stop] in notify_sound_cancel doesn't
     * touch a sound that's already silent). MRC: must [release], not just nil. */
    [g_sound release];
    g_sound = nil;
    g_sound_end_mono = 0.0;
    return false;
}

void notify_sound_cancel(void) {
    @autoreleasepool {
        if (g_sound) {
            [g_sound stop];
            [g_sound release];   /* MRC */
            g_sound = nil;
        }
        g_sound_end_mono = 0.0;
    }
}

/* True when a Liu window is the topmost onscreen regular window. Daemon
 * uses this to skip notifications the user can already see.
 *
 * We can't use NSWorkspace.frontmostApplication here: that property is
 * updated via distributed notifications, which only arrive on processes
 * pumping a CFRunLoop. The notify daemon is a plain poll() loop with no
 * runloop, so the workspace cache is frozen at whatever the frontmost
 * app was when the daemon started — Liu switches happen but the daemon
 * never sees them. CGWindowListCopyWindowInfo queries the WindowServer
 * synchronously on each call and reflects current z-order, so it works
 * from a daemon. Owner name "Liu" matches both the packaged .app and
 * the dev build at build/Liu since both expose the same process name. */
bool notify_target_active(void) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!windows) return false;

    bool is_liu = false;
    CFIndex n = CFArrayGetCount(windows);
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef w = CFArrayGetValueAtIndex(windows, i);
        /* Layer 0 = ordinary application window; skip menu bar (-9),
         * dock items, status indicators, etc. so a transient overlay
         * from another process doesn't flip the result. */
        CFNumberRef layer_ref = CFDictionaryGetValue(w, kCGWindowLayer);
        int32_t layer = 999;
        if (layer_ref) CFNumberGetValue(layer_ref, kCFNumberSInt32Type, &layer);
        if (layer != 0) continue;

        CFStringRef name = CFDictionaryGetValue(w, kCGWindowOwnerName);
        if (!name) continue;

        char buf[64];
        if (!CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8)) continue;
        is_liu = (strcmp(buf, "Liu") == 0);
        /* Topmost layer-0 window decides — array is sorted front-to-back. */
        break;
    }

    CFRelease(windows);
    return is_liu;
}

/* --- Desktop notifications via osascript (no shell, argv-separated) --------
 *
 * macOS CLI binaries without an .app bundle cannot deliver via
 * UNUserNotificationCenter. `osascript` is the documented CLI-friendly path.
 * We exec it via fork/exec with argv separation, so the payload bytes are
 * never interpreted by a shell — only by AppleScript's string literal parser,
 * which we defuse via explicit escape of `\` and `"`.
 */
#include <spawn.h>
#include <errno.h>
#include <sys/wait.h>

extern char **environ;

static void applescript_escape(char *out, size_t cap, const char *in) {
    if (cap == 0) return;
    size_t j = 0;
    if (j + 1 < cap) out[j++] = '"';
    for (size_t i = 0; in && in[i] && j + 3 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\\' || c == '"') {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c == '\n') {
            out[j++] = '\\'; out[j++] = 'n';
        } else if (c < 0x20 || c == 0x7f) {
            /* drop C0 controls */
        } else {
            out[j++] = (char)c;
        }
    }
    if (j + 1 < cap) out[j++] = '"';
    out[j] = '\0';
}

static void reap_spawned_child(pid_t pid) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        while (waitpid(pid, NULL, 0) < 0) {
            if (errno != EINTR) break;
        }
    });
}

void platform_notify_desktop(const char *title, const char *body) {
    if (!title || !body) return;
    char esc_title[512], esc_body[4096], script[5120];
    applescript_escape(esc_title, sizeof esc_title, title);
    applescript_escape(esc_body,  sizeof esc_body,  body);
    int n = snprintf(script, sizeof script,
                     "display notification %s with title %s",
                     esc_body, esc_title);
    if (n <= 0 || (size_t)n >= sizeof script) return;

    char *argv[] = { "osascript", "-e", script, NULL };
    pid_t pid;
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return;
    /* Redirect stdio to /dev/null so osascript's chatter doesn't leak into
     * our (detached) log stream. */
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

    int rc = posix_spawnp(&pid, "osascript", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return;

    reap_spawned_child(pid);
}
