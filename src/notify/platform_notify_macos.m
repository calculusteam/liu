/*
 * liu-notify - macOS TTS via AVSpeechSynthesizer (AVFoundation).
 *
 * AVSpeechSynthesizer is preferred over the soft-deprecated NSSpeechSynthesizer
 * and over shelling to `say`. Single long-lived synthesizer kept in a
 * file-scope strong ARC reference, so the audio-unit setup is not repeated per
 * utterance.
 */
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>     /* NSSound for audio file playback */
#import <Foundation/Foundation.h>

#include "notify/platform_notify.h"
#include "core/types.h"

#include <math.h>
#include <stdatomic.h>
#include <time.h>

static AVSpeechSynthesizer *g_synth = nil;

/* Speaking state is tracked HERE instead of asked of the synthesizer.
 *
 * -[AVSpeechSynthesizer isSpeaking] is not safe to call off the main thread:
 * inside AVFoundation it hops to the main thread with
 * performSelector:onThread:waitUntilDone:YES. The poll loop calls tts_busy()
 * every 50-500 ms from the server thread, so a single one of those hops that
 * cannot be serviced wedges that thread for good — and the main thread cannot
 * service it while AppKit is in event-tracking mode (i.e. exactly while the
 * user is dragging a pane divider or the sidebar edge). Such a hang has no Liu
 * frame on its stack: notifications simply stop forever.
 *
 * The delegate below reports the same state from callbacks Liu already owns,
 * with no cross-thread message send at all. */
static atomic_bool g_speaking = false;
/* Wall clock of the speakUtterance: that set g_speaking. A delegate callback
 * that never arrives (utterance rejected, audio session lost) would otherwise
 * leave the flag stuck true and silence every later notification. */
static atomic_llong g_speak_started = 0;
#define TTS_MAX_UTTERANCE_SEC 120

@interface LiuSpeechWatcher : NSObject <AVSpeechSynthesizerDelegate>
@end

@implementation LiuSpeechWatcher
- (void)speechSynthesizer:(AVSpeechSynthesizer *)synth
 didFinishSpeechUtterance:(AVSpeechUtterance *)utt {
    (void)synth; (void)utt;
    atomic_store_explicit(&g_speaking, false, memory_order_release);
}
- (void)speechSynthesizer:(AVSpeechSynthesizer *)synth
 didCancelSpeechUtterance:(AVSpeechUtterance *)utt {
    (void)synth; (void)utt;
    atomic_store_explicit(&g_speaking, false, memory_order_release);
}
@end

static LiuSpeechWatcher *g_watcher = nil;

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
            if (g_synth) {
                if (g_watcher == nil) g_watcher = [[LiuSpeechWatcher alloc] init];
                g_synth.delegate = g_watcher;   /* drives g_speaking */
            }
        }
        return g_synth != nil;
    }
}

void tts_shutdown(void) {
    @autoreleasepool {
        atomic_store_explicit(&g_speaking, false, memory_order_release);
        if (g_synth) {
            [g_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
            g_synth.delegate = nil;   /* outlives the synthesizer otherwise */
            [g_synth release];   /* MRC: this file is built without -fobjc-arc */
            g_synth = nil;
        }
        if (g_watcher) { [g_watcher release]; g_watcher = nil; }
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

        atomic_store_explicit(&g_speak_started, (long long)time(NULL),
                              memory_order_relaxed);
        atomic_store_explicit(&g_speaking, true, memory_order_release);
        [g_synth speakUtterance:u];
    }
}

/* Reads only Liu's own flag — no message send to the synthesizer, so this
 * is safe from the server thread at poll rate (see g_speaking above). */
bool tts_busy(void) {
    if (!atomic_load_explicit(&g_speaking, memory_order_acquire)) return false;
    long long began = atomic_load_explicit(&g_speak_started, memory_order_relaxed);
    if (began && (long long)time(NULL) - began > TTS_MAX_UTTERANCE_SEC) {
        /* No completion callback in two minutes: treat it as finished rather
         * than block the queue forever. */
        atomic_store_explicit(&g_speaking, false, memory_order_release);
        return false;
    }
    return true;
}

void tts_cancel(void) {
    @autoreleasepool {
        atomic_store_explicit(&g_speaking, false, memory_order_release);
        if (g_synth) {
            [g_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
        }
    }
}

/* --- Sound file playback via NSSound --------------------------------------
 *
 * One sound at a time: a new one replaces whatever is playing. The queue's
 * rate limiter bounds how often a sound can be replaced.
 *
 * `notify_sound_busy()` cannot trust `[NSSound isPlaying]`: the liu-notify
 * daemon is a plain C poll() loop with no NSRunLoop on the main thread, so
 * NSSound's completion notifications never fire and `isPlaying` returns YES
 * indefinitely. Instead, read the sound's `duration` up front and mark the
 * deadline; busy returns true only until that deadline elapses, allowing the
 * queue to drain reliably for subsequent events.
 *
 * Threading. These functions are called from the notify server loop
 * (daemon.c) and the main thread (the done panel's sound, ui/tab_popup.c).
 * Manual retain/release requires synchronization: if both threads read the
 * same `g_sound` pointer, both could send `-stop` and `-release`, leaving the
 * second message directed at freed memory.
 *
 *   - Read and write g_sound and g_sound_end_mono only under g_sound_mu.
 *   - g_sound owns exactly one reference. A thread that takes the pointer out
 *     under the lock (sound_take_locked) takes that reference with it; only
 *     that thread messages the NSSound, before publishing or after removal.
 *   - Do not send NSSound messages while holding the lock. `-play` can wait
 *     for audio-device access, and AppKit can dispatch to the main thread while
 *     that thread waits for this mutex.
 */
#include <pthread.h>
#include <time.h>
static pthread_mutex_t g_sound_mu = PTHREAD_MUTEX_INITIALIZER;
static NSSound *g_sound = nil;            /* guarded by g_sound_mu; owns +1 */
static double   g_sound_end_mono = 0.0;   /* guarded by g_sound_mu */

static double notify_mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Empty the slot and hand its reference to the caller, who must -release it
 * (after -stop, if it may still be sounding) once the lock is dropped. */
static NSSound *sound_take_locked(void) {
    NSSound *s = g_sound;
    g_sound = nil;
    return s;
}

bool notify_play_sound(const char *path) {
    if (!path || !*path) return false;
    @autoreleasepool {
        NSString *p = [NSString stringWithUTF8String:path];
        if (p == nil) return false;
        if (![[NSFileManager defaultManager] isReadableFileAtPath:p]) return false;

        /* +1 from alloc/init, and private to this thread until it is published
         * below — so it can be asked its duration and started without a lock. */
        NSSound *s = [[NSSound alloc] initWithContentsOfFile:p byReference:YES];
        if (s == nil) return false;
        double dur = [s duration];
        if (dur < 0.05) dur = 0.05;

        /* Replace any in-flight sound: take it out, then silence and free it. */
        pthread_mutex_lock(&g_sound_mu);
        NSSound *old = sound_take_locked();
        pthread_mutex_unlock(&g_sound_mu);
        if (old) { [old stop]; [old release]; }

        if (![s play]) {
            [s release];
            pthread_mutex_lock(&g_sound_mu);
            /* Nothing of ours is sounding. Clear the deadline unless another
             * thread published a sound of its own in the meantime. */
            if (g_sound == nil) g_sound_end_mono = 0.0;
            pthread_mutex_unlock(&g_sound_mu);
            return false;
        }

        /* Publish the new sound under the mutex. If another thread publishes
         * between the take and this point, the later sound stops the earlier
         * one. Add a short delay so `done` is not reported while CoreAudio
         * drains its final buffers. */
        pthread_mutex_lock(&g_sound_mu);
        NSSound *raced = sound_take_locked();
        g_sound = s;                 /* ownership of the +1 moves to the slot */
        g_sound_end_mono = notify_mono_now() + dur + 0.05;
        pthread_mutex_unlock(&g_sound_mu);
        if (raced) { [raced stop]; [raced release]; }
        return true;
    }
}

bool notify_sound_busy(void) {
    NSSound *done = nil;
    bool busy = false;
    pthread_mutex_lock(&g_sound_mu);
    if (g_sound_end_mono > 0.0) {
        if (notify_mono_now() < g_sound_end_mono) {
            busy = true;
        } else {
            /* Past the deadline — drop the ref so the next play starts with a
             * clean slate (and so notify_sound_cancel has nothing to stop). */
            done = sound_take_locked();
            g_sound_end_mono = 0.0;
        }
    }
    pthread_mutex_unlock(&g_sound_mu);
    if (done) {
        @autoreleasepool { [done release]; }   /* MRC: must release, not just nil */
    }
    return busy;
}

void notify_sound_cancel(void) {
    pthread_mutex_lock(&g_sound_mu);
    NSSound *s = sound_take_locked();
    g_sound_end_mono = 0.0;
    pthread_mutex_unlock(&g_sound_mu);
    if (s) {
        @autoreleasepool {
            [s stop];
            [s release];   /* MRC */
        }
    }
}

/* notify_target_active() is not implemented here: a CGWindowListCopyWindowInfo
 * query for a frontmost window owned by "Liu" is behind macOS 15+'s
 * screen-recording TCC service, which makes the OS treat Liu as a screen
 * recorder (capture indicator over the traffic lights, a permission prompt
 * that keeps coming back). The app publishes its own frontmost state instead —
 * see notify/focus_state.c. */

/* --- Desktop notifications via osascript (no shell, argv-separated) --------
 *
 * macOS CLI binaries without an .app bundle cannot deliver via
 * UNUserNotificationCenter. `osascript` is the documented CLI-friendly path.
 * It is spawned with argv separation, so the payload bytes are never
 * interpreted by a shell — only by AppleScript's string literal parser,
 * which is defused by explicitly escaping `\` and `"`.
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
     * the (detached) log stream. */
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

    /* The system's own, by path — not the first `osascript` on a PATH that
     * Liu fills from the login shell (curl got the same treatment). */
    int rc = posix_spawn(&pid, "/usr/bin/osascript", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return;

    reap_spawned_child(pid);
}
