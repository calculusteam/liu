/* The first-run intro's overlay: a borderless, screen-sized, transparent window
 * above the desktop, the clock that runs it, and the sound.
 *
 * Nothing here blocks: there is no modal session and no wait. Every frame the
 * overlay sets the target window's alpha from the timeline; by the time the
 * tour begins the target is fully visible, and it stays so however the intro
 * ends — by itself when the tour has played, dismissed, skipped, the target
 * closing, a display link that never draws (the watchdog).
 *
 * It is one flow, and it ends by itself; it cannot be stepped back or forward.
 * All the viewer can do is end it: any key, from wherever it is. A click does
 * nothing — a click is too easy to make by accident.
 *
 * The captions are in the system's language when the intro has captions in it
 * (intro_lang.h) and in English otherwise; it is looked up once, here, before
 * anything is drawn.
 *
 * The picture is drawn on a CVDisplayLink's own thread, not the main one. Liu's
 * main thread spends the intro's first second doing what a first launch has to
 * do (app_init, the PATH probe, hook installation, the first PTY), and a clock
 * that lived there would stutter through exactly the frames the dark arrives
 * in. Only two things cross back to the main thread — the target window's
 * alpha, and reading its frame — and both tolerate being a frame late.
 *
 *   LIU_INTRO_SPEED=0.25   slow motion (silent: the sound is scored for 1x)
 *   LIU_INTRO_SOUND=0      no sound
 *   LIU_INTRO_REDUCE=1     the Reduce Motion variant, whatever the system says
 *   LIU_INTRO_REPORT=1     one line on stderr when it ends
 *   LIU_INTRO_VOLUME=0.3   the sound's volume (default 0.7; 0 still runs the
 *                          engine and fires every cue, without being heard)
 *   LIU_INTRO_SKIP_AT=1.5  skip by itself at that many seconds, through the
 *                          same call a key press makes
 *   LIU_INTRO_SCRIPT=9:click,13.9:end
 *                          the viewer's part, scripted: at each time (seconds on
 *                          the clock) `end` is what a key press does, and
 *                          `click` a mouse-down event handed to the overlay's
 *                          window, so it takes the path a real click does from
 *                          there — which is to nothing
 *   LIU_INTRO_LANG=tr      the captions' language, whatever the system's is (a
 *                          language with no captions gets English, as it
 *                          would from the system)
 *   LIU_INTRO_STALL=1      never draw a frame, so only the watchdog can end it
 *   LIU_INTRO_DEMO=1       quit once it has ended (main() leaves the saved
 *                          session alone for such a run); with LIU_INTRO=1 this
 *                          is how the real launch path is looked at */

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "intro/intro.h"
#include "intro/intro_lang.h"
#include "intro/intro_render.h"
#include "intro/intro_sound.h"
#include "intro/intro_timeline.h"

#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* If no frame has been drawn this long after the tour should have begun, the
 * display link is not running: the watchdog ends the intro. (Once frames are
 * being drawn the timeline ends it; how long that takes is the viewer's doing,
 * since going back a feature makes it longer, so there is no total to time.) */
#define INTRO_WATCHDOG_GRACE 1.5
/* Must match the window's corners (LIU_CORNER_RADIUS in platform_macos.m): it
 * is the radius the frame lands on. */
#define INTRO_TARGET_RADIUS 10.0f
/* LIU_INTRO_REPORT counts the frames drawn in each of this many seconds. */
#define INTRO_REPORT_SECONDS 240
#define INTRO_SOUND_VOLUME 0.7f

/* The overlay never takes the keyboard. The terminal window stays key under it
 * for the whole intro — it has to be, for the tour: a window that is not key
 * draws itself unfocused — so a key press arrives in main.c's event loop, which
 * hands it to liu_intro_end(). */
@interface LiuIntroWindow : NSWindow
@end

@implementation LiuIntroWindow
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

/* It takes the mouse, and does nothing with it. A click must not end the intro
 * — an absent-minded click would close it — and it must not fall through to
 * whatever is under the dark either: on the terminal it would move the focus
 * about in the middle of the tour, and on another app's window that app would
 * come forward, which ends the intro by another road. A key is what ends it.
 */
@interface LiuIntroView : NSView
@end

@implementation LiuIntroView
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (void)mouseDown:(NSEvent *)event { (void)event; }
- (void)rightMouseDown:(NSEvent *)event { (void)event; }
- (void)otherMouseDown:(NSEvent *)event { (void)event; }
- (void)scrollWheel:(NSEvent *)event { (void)event; }
@end

typedef struct IntroOverlay {
    u64 token;
    NSWindow *target;              /* retained */
    LiuIntroWindow *window;        /* retained */
    CAMetalLayer *layer;           /* retained */
    IntroRenderer *renderer;
    CVDisplayLinkRef link;
    NSTimer *watchdog;             /* retained */
    id close_observer;             /* retained */
    id resign_observer;            /* retained */
    CGRect screen_frame;
    f64 speed;
    /* The picture's time at which the tour begins, the ending begins, and the
     * ending is over — the full intro's, or the Reduce Motion one's. */
    f64 show, lift, total;
    bool reduced;
    bool plays_sound;
    bool report;
    /* Test hook: the display link fires and draws nothing. */
    bool stalls;
    CFTimeInterval start;

    /* Shared with the display-link thread. */
    os_unfair_lock lock;
    IntroLayout layout;
    CFTimeInterval skip_start;     /* 0: no skip in progress */
    CFTimeInterval end_start;      /* 0: the viewer has not dismissed it */
    f64 end_tour;                  /* where the tour stood when they did */
    _Atomic bool finished;
    _Atomic bool finish_requested;
    _Atomic bool close_cued;
    _Atomic bool apply_pending;
    _Atomic u32 reveal_bits;

    /* Report. `per_second` is written by the display-link thread alone and read
     * once that thread has been stopped. */
    _Atomic i32 frames;
    u16 per_second[INTRO_REPORT_SECONDS];
    bool skipped;
    bool dismissed;
    /* The timeline reached its own end. */
    bool ran_out;
    i32 lang;
    bool ended_by_watchdog;
    bool reveal_monotonic;
    f32 last_reveal;
} IntroOverlay;

/* Main thread only. */
static IntroOverlay *g_overlay;
static u64 g_token;

static void overlay_finish(IntroOverlay *o);
static bool env_flag(const char *name);

/* ---- sound ------------------------------------------------------------- */

/* One engine, and four kinds of voice on it: the opening (one buffer, joined
 * to the clock), the bed (a loop under the tour), and two small pools the cues
 * are fired into — ticks, which are over in a moment, and notes, which ring.
 * Separate pools because a voice is taken over when its turn comes round
 * again: a run of typed keys must be able to go through all the tick voices
 * without cutting off the caption's note that started just before them.
 *
 * Only the output side of the engine is ever touched. (Its input node is what
 * would raise the microphone prompt.) */
#define INTRO_TICK_VOICES 8
#define INTRO_NOTE_VOICES 4

typedef struct {
    u64 id;
    AVAudioEngine *engine;                             /* retained */
    AVAudioFormat *format;                             /* retained */
    AVAudioPlayerNode *score, *bed;                    /* retained */
    AVAudioPlayerNode *ticks[INTRO_TICK_VOICES];       /* retained */
    AVAudioPlayerNode *notes[INTRO_NOTE_VOICES];       /* retained */
    int next_tick, next_note;
    AVAudioPCMBuffer *bed_in, *bed_loop;               /* retained */
    AVAudioPCMBuffer *cues[INTRO_CUE_COUNT][INTRO_CUE_MAX_VARIANTS];   /* retained */
    bool bed_on, closing;
    i32 cues_played;
    u64 master_ramp, bed_ramp;
} IntroAudio;

/* Main thread only. NULL: no sound (off, not ready yet, or over). */
static IntroAudio *g_audio;
/* A render still under way belongs to this one; bumping it disowns it. Read
 * from the render's own thread, hence atomic. */
static _Atomic u64 g_audio_token;

static void after_seconds(NSTimeInterval seconds, dispatch_block_t body) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), body);
}

/* `frames` interleaved stereo frames from `from` on, as a buffer the engine
 * plays. `fade_in` seconds of it come up from nothing. +1 retained. */
static AVAudioPCMBuffer *audio_buffer(AVAudioFormat *format, const f32 *samples, usize frames,
                                      usize from, f64 fade_in) {
    if (!samples || from >= frames) return nil;
    usize count = frames - from;
    AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format
                                                              frameCapacity:(AVAudioFrameCount)count];
    if (!buffer) return nil;
    buffer.frameLength = (AVAudioFrameCount)count;
    float *left = buffer.floatChannelData[0], *right = buffer.floatChannelData[1];
    usize rise = (usize)(fade_in * INTRO_SOUND_RATE);
    for (usize i = 0; i < count; i++) {
        f32 g = 1.0f;
        if (i < rise) {
            f32 x = (f32)i / (f32)rise;
            g = x * x * (3.0f - 2.0f * x);
        }
        left[i] = samples[(from + i) * 2] * g;
        right[i] = samples[(from + i) * 2 + 1] * g;
    }
    return buffer;
}

static void audio_free(IntroAudio *a) {
    if (!a) return;
    [a->score stop];
    [a->bed stop];
    for (int i = 0; i < INTRO_TICK_VOICES; i++) [a->ticks[i] stop];
    for (int i = 0; i < INTRO_NOTE_VOICES; i++) [a->notes[i] stop];
    [a->engine stop];
    [a->score release];
    [a->bed release];
    for (int i = 0; i < INTRO_TICK_VOICES; i++) [a->ticks[i] release];
    for (int i = 0; i < INTRO_NOTE_VOICES; i++) [a->notes[i] release];
    [a->bed_in release];
    [a->bed_loop release];
    for (int c = 0; c < INTRO_CUE_COUNT; c++)
        for (int v = 0; v < INTRO_CUE_MAX_VARIANTS; v++) [a->cues[c][v] release];
    [a->engine release];
    [a->format release];
    free(a);
}

/* A voice on the engine, running. +1 retained; nil if it could not be made. */
static AVAudioPlayerNode *audio_voice(IntroAudio *a) {
    AVAudioPlayerNode *node = [[AVAudioPlayerNode alloc] init];
    if (!node) return nil;
    [a->engine attachNode:node];
    [a->engine connect:node to:a->engine.mainMixerNode format:a->format];
    return node;
}

/* Bring the master (or the bed alone) to `to` over `seconds`, in small steps. */
static void audio_ramp(IntroAudio *a, bool master, f32 to, NSTimeInterval seconds) {
    f32 from = master ? a->engine.mainMixerNode.outputVolume : a->bed.volume;
    u64 id = a->id, token = master ? ++a->master_ramp : ++a->bed_ramp;
    int steps = (int)(seconds * 60.0);
    if (steps < 1) steps = 1;
    for (int i = 1; i <= steps; i++) {
        f32 v = from + (to - from) * (f32)i / (f32)steps;
        after_seconds(seconds * (NSTimeInterval)i / (NSTimeInterval)steps, ^{
            /* `a` is only COMPARED until it is known to be the live one. */
            if (g_audio != a || g_audio->id != id) return;
            if ((master ? a->master_ramp : a->bed_ramp) != token) return;
            if (master) a->engine.mainMixerNode.outputVolume = v;
            else        a->bed.volume = v;
        });
    }
}

static void audio_free_after(NSTimeInterval seconds) {
    IntroAudio *a = g_audio;
    if (!a) return;
    u64 id = a->id;
    after_seconds(seconds, ^{
        if (g_audio != a || g_audio->id != id) return;
        g_audio = NULL;
        audio_free(a);
    });
}

/* The intro was skipped, or ended some way it was not meant to: the sound goes
 * with the picture, and a render still on its way is disowned. */
static void audio_abort(NSTimeInterval seconds) {
    atomic_fetch_add(&g_audio_token, 1);
    IntroAudio *a = g_audio;
    if (!a) return;
    audio_ramp(a, true, 0.0f, seconds);
    audio_free_after(seconds + 0.1);
}

/* One cue, now. */
static void audio_cue(IntroCue cue, i32 variant) {
    IntroAudio *a = g_audio;
    if (!a || cue <= INTRO_CUE_NONE || cue >= INTRO_CUE_COUNT) return;
    i32 v = variant % intro_cue_variants(cue);
    AVAudioPCMBuffer *buffer = v >= 0 ? a->cues[cue][v] : nil;
    if (!buffer) return;
    AVAudioPlayerNode *voice = intro_cue_rings(cue)
        ? a->notes[a->next_note++ % INTRO_NOTE_VOICES]
        : a->ticks[a->next_tick++ % INTRO_TICK_VOICES];
    if (!voice) return;
    [voice scheduleBuffer:buffer atTime:nil options:AVAudioPlayerNodeBufferInterrupts
        completionHandler:nil];
    a->cues_played++;
}

/* The bed comes up under the tour: once through with a fade on it, then round
 * and round. */
static void audio_bed_on(void) {
    IntroAudio *a = g_audio;
    if (!a || a->bed_on || a->closing || !a->bed_in || !a->bed_loop) return;
    a->bed_on = true;
    [a->bed scheduleBuffer:a->bed_in atTime:nil options:0 completionHandler:nil];
    [a->bed scheduleBuffer:a->bed_loop atTime:nil options:AVAudioPlayerNodeBufferLoops
         completionHandler:nil];
}

/* The intro is ending — it ran out, or the viewer dismissed it: the signature
 * once more, the bed away under it, and the engine let go once it has rung. */
static void audio_close(void) {
    IntroAudio *a = g_audio;
    if (!a || a->closing) return;
    a->closing = true;
    audio_cue(INTRO_CUE_CLOSE, 0);
    audio_ramp(a, false, 0.0f, INTRO_BED_FADE_OUT);
    audio_free_after(4.2);
}

/* Starts the engine and renders everything off the main thread, and joins the
 * opening to an intro whose clock (CACurrentMediaTime) started at
 * `clock_start` — at the point the picture has reached by then, rather than
 * starting late by however long that took. The bed is due `bed_at` seconds
 * into that clock.
 *
 * The first sound is the cursor, 0.28 s in, so the order matters: the engine
 * is started WHILE the opening renders, the opening is joined from this thread
 * the moment both are there (not from the main thread, which on a first launch
 * is busy for most of that first second; waiting for it, or for the cues,
 * would join the opening after the cursor), and only then are the bed and the
 * cues rendered. Those are not needed until the tour. */
static void audio_start(bool reduced, f32 volume, CFTimeInterval clock_start, f64 bed_at, bool report) {
    u64 token = atomic_fetch_add(&g_audio_token, 1) + 1;
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_async(queue, ^{
        @autoreleasepool {
            IntroAudio *a = calloc(1, sizeof *a);
            if (!a) return;
            a->id = token;
            __block f32 *opening = NULL;
            __block usize opening_frames = 0;
            dispatch_group_t rendering = dispatch_group_create();
            dispatch_group_async(rendering, queue, ^{
                opening = intro_sound_opening(reduced, &opening_frames);
            });

            a->format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:INTRO_SOUND_RATE
                                                                       channels:2];
            a->engine = [[AVAudioEngine alloc] init];
            bool ok = a->format && a->engine;
            if (ok) {
                a->score = audio_voice(a);
                a->bed = audio_voice(a);
                for (int i = 0; i < INTRO_TICK_VOICES; i++) a->ticks[i] = audio_voice(a);
                for (int i = 0; i < INTRO_NOTE_VOICES; i++) a->notes[i] = audio_voice(a);
                a->engine.mainMixerNode.outputVolume = volume;
                NSError *error = nil;
                ok = a->score && a->bed && [a->engine startAndReturnError:&error];
            }
            dispatch_group_wait(rendering, DISPATCH_TIME_FOREVER);
            dispatch_release(rendering);

            /* Where the picture is now, plus what the output adds before a
             * sample is heard. */
            f64 ready = CACurrentMediaTime() - clock_start, joined = -1.0;
            if (ok && opening && token == atomic_load(&g_audio_token)) {
                /* The opening's voice alone, first: setting the other thirteen
                 * going takes longer, and they are not needed yet. */
                [a->score play];
                joined = CACurrentMediaTime() - clock_start + a->engine.outputNode.presentationLatency;
                if (joined < 0.0) joined = 0.0;
                AVAudioPCMBuffer *rest = audio_buffer(a->format, opening, opening_frames,
                                                      (usize)(joined * INTRO_SOUND_RATE), 0.004);
                if (rest) {
                    [a->score scheduleBuffer:rest atTime:nil options:0 completionHandler:nil];
                    [rest release];
                }
                [a->bed play];
                for (int i = 0; i < INTRO_TICK_VOICES; i++) [a->ticks[i] play];
                for (int i = 0; i < INTRO_NOTE_VOICES; i++) [a->notes[i] play];
            }
            free(opening);

            int made = 0;
            if (ok) {
                usize bed_frames = 0;
                f32 *bed = intro_sound_bed(&bed_frames);
                a->bed_in = audio_buffer(a->format, bed, bed_frames, 0, INTRO_BED_FADE_IN);
                a->bed_loop = audio_buffer(a->format, bed, bed_frames, 0, 0.0);
                free(bed);
                for (int c = INTRO_CUE_NONE + 1; c < INTRO_CUE_COUNT; c++) {
                    for (i32 v = 0; v < intro_cue_variants((IntroCue)c); v++) {
                        usize frames = 0;
                        f32 *samples = intro_sound_cue((IntroCue)c, v, &frames);
                        a->cues[c][v] = audio_buffer(a->format, samples, frames, 0, 0.0);
                        free(samples);
                        if (a->cues[c][v]) made++;
                    }
                }
            }
            f64 complete = CACurrentMediaTime() - clock_start;

            dispatch_async(dispatch_get_main_queue(), ^{
                /* Skipped, or over, while this was on its way: it is not
                 * wanted, and whatever it started stops here. */
                if (!ok || token != atomic_load(&g_audio_token)) {
                    audio_free(a);
                    return;
                }
                if (g_audio) audio_free(g_audio);
                g_audio = a;
                f64 bed_in = bed_at - INTRO_BED_LEAD - (CACurrentMediaTime() - clock_start);
                u64 id = a->id;
                after_seconds(bed_in > 0.0 ? bed_in : 0.0, ^{
                    if (g_audio == a && g_audio->id == id) audio_bed_on();
                });
                if (report) {
                    fprintf(stderr, "[intro] sound: engine and opening ready %.0f ms in, opening joined at "
                                    "%.3f s; %d cue buffers by %.0f ms; output latency %.0f ms\n",
                            ready * 1000.0, joined, made, complete * 1000.0,
                            a->engine.outputNode.presentationLatency * 1000.0);
                }
            });
        }
    });
}

/* ---- the clock --------------------------------------------------------- */

static CGRect target_rect(const IntroOverlay *o) {
    NSRect frame = [o->target frame];
    return CGRectMake(frame.origin.x - o->screen_frame.origin.x,
                      CGRectGetMaxY(o->screen_frame) - NSMaxY(frame),
                      frame.size.width, frame.size.height);
}

/* Main thread: the two things the picture cannot do from its own thread. */
static void overlay_apply(IntroOverlay *o) {
    u32 bits = atomic_load(&o->reveal_bits);
    f32 reveal;
    memcpy(&reveal, &bits, sizeof reveal);
    if (reveal < o->last_reveal) o->reveal_monotonic = false;
    o->last_reveal = reveal;
    [o->target setAlphaValue:(CGFloat)reveal];
    /* The frame follows the window if anything moves or resizes it. */
    CGRect rect = target_rect(o);
    os_unfair_lock_lock(&o->lock);
    intro_layout_set_target(&o->layout, (f32)rect.origin.x, (f32)rect.origin.y,
                            (f32)rect.size.width, (f32)rect.size.height);
    os_unfair_lock_unlock(&o->lock);
}

/* Seconds into the tour: negative before it has begun. */
static f64 tour_time(const IntroOverlay *o, CFTimeInterval now) {
    return (now - o->start) * o->speed - o->show;
}

/* Display-link thread. */
static void overlay_tick(IntroOverlay *o) {
    if (atomic_load(&o->finished) || atomic_load(&o->finish_requested) || o->stalls) return;
    CFTimeInterval now = CACurrentMediaTime();
    f64 clock = (now - o->start) * o->speed;

    /* The picture's time is the clock's, start to finish — or, from the moment
     * the viewer dismisses the tour, the ending's alone. */
    f64 t = clock;
    f64 ending = -1.0, held_at = 0.0;
    bool done = false, lifting = false;
    os_unfair_lock_lock(&o->lock);
    IntroLayout layout = o->layout;
    CFTimeInterval skip_start = o->skip_start;
    if (o->end_start > 0) {
        ending = (now - o->end_start) * o->speed;
        held_at = o->show + o->end_tour;
        t = o->lift + ending;
        done = t >= o->total;
    } else if (clock >= o->show) {
        done = t >= o->total;
        lifting = t >= o->lift;
    }
    os_unfair_lock_unlock(&o->lock);

    /* The tour has run out and the dark starts to lift: the close. */
    if (lifting && !atomic_exchange(&o->close_cued, true)) {
        u64 closing = o->token;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_overlay && g_overlay->token == closing && !g_overlay->skipped) audio_close();
        });
    }

    IntroFrame frame = o->reduced ? intro_frame_reduced_at(t) : intro_frame_at(t);
    if (ending >= 0.0) {
        IntroFrame held = o->reduced ? intro_frame_reduced_at(held_at) : intro_frame_at(held_at);
        frame = intro_frame_dismissed(held, frame, ending);
    }
    if (skip_start > 0) {
        f64 progress = (now - skip_start) / INTRO_SKIP_DURATION;
        if (progress >= 1.0) done = true;
        frame = intro_frame_skipped(frame, progress);
    }

    u64 token = o->token;
    if (done) {
        if (!atomic_exchange(&o->finish_requested, true)) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!g_overlay || g_overlay->token != token) return;
                g_overlay->ran_out = true;
                overlay_finish(g_overlay);
            });
        }
        return;
    }
    if (intro_renderer_draw(o->renderer, &frame, &layout, clock, o->layer)) {
        atomic_fetch_add(&o->frames, 1);
        f64 second = now - o->start;
        if (second >= 0.0 && second < INTRO_REPORT_SECONDS) o->per_second[(i32)second]++;
    }

    u32 bits;
    memcpy(&bits, &frame.reveal, sizeof bits);
    atomic_store(&o->reveal_bits, bits);
    if (!atomic_exchange(&o->apply_pending, true)) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!g_overlay || g_overlay->token != token) return;
            atomic_store(&g_overlay->apply_pending, false);
            overlay_apply(g_overlay);
        });
    }
}

static CVReturn overlay_link_callback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                      const CVTimeStamp *output, CVOptionFlags flags_in,
                                      CVOptionFlags *flags_out, void *context) {
    (void)link; (void)now; (void)output; (void)flags_in; (void)flags_out;
    overlay_tick((IntroOverlay *)context);
    return kCVReturnSuccess;
}

/* Fade out from wherever the intro is and bring the window in. */
static void overlay_skip(IntroOverlay *o) {
    if (atomic_load(&o->finished)) return;
    os_unfair_lock_lock(&o->lock);
    bool already = o->skip_start > 0;
    if (!already) o->skip_start = CACurrentMediaTime();
    os_unfair_lock_unlock(&o->lock);
    if (already) return;
    o->skipped = true;
    audio_abort(INTRO_SKIP_DURATION);
}

/* When a key ends the intro, transition directly to the ending if the window
 * has appeared. A key before the tour starts skips it; after the tour
 * completes, no further transition is needed. */
static void overlay_end(IntroOverlay *o) {
    if (atomic_load(&o->finished)) return;
    CFTimeInterval now = CACurrentMediaTime();
    f64 tour = tour_time(o, now);
    bool touring = tour >= 0.0;
    os_unfair_lock_lock(&o->lock);
    bool already = o->end_start > 0 || o->skip_start > 0 || tour >= INTRO_SHOW_SECONDS;
    if (touring && !already) {
        o->end_tour = tour;
        o->end_start = now;
    }
    os_unfair_lock_unlock(&o->lock);
    if (!touring) { overlay_skip(o); return; }
    if (already) return;
    o->dismissed = true;
    /* The same close the tour comes to by itself. */
    audio_close();
}

static void overlay_finish(IntroOverlay *o) {
    if (!o || atomic_exchange(&o->finished, true)) return;
    f64 elapsed = CACurrentMediaTime() - o->start;

    /* Stop the link first: CVDisplayLinkStop returns only once a callback in
     * flight has returned, so nothing below can race the drawing thread. */
    if (o->link) {
        CVDisplayLinkStop(o->link);
        CVDisplayLinkRelease(o->link);
        o->link = NULL;
    }
    [o->watchdog invalidate];
    [o->watchdog release];
    o->watchdog = nil;
    if (o->close_observer) {
        [[NSNotificationCenter defaultCenter] removeObserver:o->close_observer];
        [o->close_observer release];
        o->close_observer = nil;
    }
    if (o->resign_observer) {
        [[NSNotificationCenter defaultCenter] removeObserver:o->resign_observer];
        [o->resign_observer release];
        o->resign_observer = nil;
    }

    /* Ended the way it is meant to — it ran out, or the viewer dismissed it:
     * the close has been cued and rings out over the window. Ended any other
     * way: the sound goes with the picture. */
    bool ran_to_end = o->ran_out && !o->skipped && !o->dismissed && !o->ended_by_watchdog;
    bool rings = g_audio && g_audio->closing && !o->skipped && !o->ended_by_watchdog;
    i32 cues_played = g_audio ? g_audio->cues_played : 0;
    bool sounding = g_audio != NULL;
    if (!rings) audio_abort(0.25);

    [o->target setAlphaValue:1.0];
    [o->window orderOut:nil];
    if ([o->target isVisible]) {
        /* makeKey, then order — not makeKeyAndOrderFront. While another app is
         * active a plain orderFront puts the window BEHIND that app's windows. */
        [o->target makeKeyWindow];
        [o->target orderFrontRegardless];
    }

    if (o->report) {
        CGSize drawable = o->layer.drawableSize;
        fprintf(stderr, "[intro] frames=%d elapsed=%.2fs %s lang=%s skipped=%d watchdog=%d "
                        "reveal_monotonic=%d reduced=%d drawable=%.0fx%.0f screen=%.0fx%.0f "
                        "scale=%.1f target_alpha=%.2f sound=%s cues=%d\n",
                atomic_load(&o->frames), elapsed,
                ran_to_end ? "ran_to_end" : (o->dismissed && !o->skipped ? "dismissed" : "ended_early"),
                intro_lang_code(o->lang), o->skipped, o->ended_by_watchdog, o->reveal_monotonic, o->reduced,
                drawable.width, drawable.height, o->screen_frame.size.width,
                o->screen_frame.size.height, (double)o->layout.scale,
                (double)[o->target alphaValue],
                !o->plays_sound ? "off" : (sounding ? "on" : "none"), cues_played);
        /* Frames drawn in each whole second it ran. */
        i32 whole = (i32)elapsed < INTRO_REPORT_SECONDS ? (i32)elapsed : INTRO_REPORT_SECONDS;
        i32 least = -1;
        for (i32 i = 1; i < whole; i++) if (least < 0 || o->per_second[i] < o->per_second[least]) least = i;
        if (least >= 0) {
            fprintf(stderr, "[intro] frames per second: least %d (second %d);", o->per_second[least], least);
            for (i32 i = 0; i < whole; i++) fprintf(stderr, " %d", o->per_second[i]);
            fprintf(stderr, "\n");
        }
    }

    /* The sound outlasts the picture; a demo that quit on the picture's heels
     * would cut the tail it is being listened to for. */
    f64 linger = rings ? 3.6 : 0.9;

    intro_renderer_destroy(o->renderer);
    [o->layer release];
    [o->window release];
    [o->target release];
    if (g_overlay == o) g_overlay = NULL;
    free(o);

    /* Long enough to see the window standing on its own afterwards. */
    if (env_flag("LIU_INTRO_DEMO")) after_seconds(linger, ^{ exit(0); });
}

/* Every name these three are asked about is a LIU_INTRO_* staging variable
 * (core/dev_hooks.h): a build without the hooks reads none of them. */
static const char *intro_env(const char *name) {
    return LIU_DEV_HOOKS_ON ? getenv(name) : NULL;
}

static f64 env_number(const char *name, f64 fallback) {
    const char *value = intro_env(name);
    if (!value || !value[0]) return fallback;
    f64 v = atof(value);
    return v > 0.0 ? v : fallback;
}

static bool env_flag(const char *name) {
    const char *value = intro_env(name);
    return value && value[0] && value[0] != '0';
}

static bool env_off(const char *name) {
    const char *value = intro_env(name);
    return value && value[0] == '0';
}

/* ---- the seam ---------------------------------------------------------- */

bool liu_intro_active(void) { return g_overlay != NULL; }

void liu_intro_end(void) {
    if (g_overlay) overlay_end(g_overlay);
}

void liu_intro_cue(i32 cue, i32 variant) {
    if (g_overlay && !g_overlay->skipped && !g_overlay->dismissed) audio_cue((IntroCue)cue, variant);
}

bool liu_intro_leaving(void) {
    IntroOverlay *o = g_overlay;
    if (!o) return false;
    os_unfair_lock_lock(&o->lock);
    bool leaving = o->end_start > 0 || o->skip_start > 0;
    os_unfair_lock_unlock(&o->lock);
    return leaving || tour_time(o, CACurrentMediaTime()) >= INTRO_SHOW_SECONDS;
}

bool liu_intro_tour_clock(f64 *seconds) {
    IntroOverlay *o = g_overlay;
    if (!o || atomic_load(&o->finished) || o->skipped) return false;
    os_unfair_lock_lock(&o->lock);
    bool running = o->end_start == 0;
    os_unfair_lock_unlock(&o->lock);
    if (!running) return false;
    if (seconds) *seconds = tour_time(o, CACurrentMediaTime());
    return true;
}

/* LIU_INTRO_SCRIPT: "9:click,13.9:end" — the viewer's part. */
static void run_script(IntroOverlay *o, const char *script) {
    u64 token = o->token;
    char *copy = strdup(script);
    if (!copy) return;
    for (char *item = strtok(copy, ","); item; item = strtok(NULL, ",")) {
        char *colon = strchr(item, ':');
        if (!colon) continue;
        f64 at = atof(item);
        bool click = strcmp(colon + 1, "click") == 0;
        if (!click && strcmp(colon + 1, "end") != 0) continue;
        after_seconds(at / o->speed, ^{
            if (!g_overlay || g_overlay->token != token) return;
            if (!click) { overlay_end(g_overlay); return; }
            /* A mouse-down in the middle of the overlay — over the terminal,
             * where the overlay has drawn nothing — sent the way AppKit sends
             * one: to the window, which finds the view under it. Nothing
             * should come of it. */
            NSWindow *window = g_overlay->window;
            NSRect frame = [window frame];
            NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                location:NSMakePoint(frame.size.width * 0.5, frame.size.height * 0.5)
                modifierFlags:0 timestamp:[[NSProcessInfo processInfo] systemUptime]
                windowNumber:[window windowNumber] context:nil eventNumber:0 clickCount:1 pressure:1.0];
            [window sendEvent:down];
        });
    }
    free(copy);
}

/* Use the system's first preferred language when the intro includes
 * captions; otherwise use English. The selection does not search for a later
 * supported language. */
static i32 system_language(void) {
    const char *forced = liu_dev_env("LIU_INTRO_LANG");
    if (forced && forced[0]) return intro_lang_for_tag(forced);
    NSString *first = [[NSLocale preferredLanguages] firstObject];
    return intro_lang_for_tag(first.UTF8String);
}

void liu_intro_begin(PlatformWindow *w) {
    NSView *native = (__bridge NSView *)platform_window_native_view(w);
    NSWindow *target = [native window];
    if (!target) return;
    @autoreleasepool {
        if (g_overlay) overlay_finish(g_overlay);

        NSScreen *screen = [target screen];
        if (!screen) screen = [NSScreen mainScreen];
        id<MTLDevice> device = [MTLCreateSystemDefaultDevice() autorelease];
        i32 lang = system_language();
        IntroRenderer *renderer = screen ? intro_renderer_create(device, lang) : NULL;
        IntroOverlay *o = renderer ? calloc(1, sizeof *o) : NULL;
        if (!o) {
            /* The overlay cannot be built: the window is simply shown. */
            intro_renderer_destroy(renderer);
            [target setAlphaValue:1.0];
            return;
        }
        /* Before it plays, not after: an intro that somehow fails must not
         * come back on every launch. */
        liu_intro_mark_seen();

        o->token = ++g_token;
        o->lock = OS_UNFAIR_LOCK_INIT;
        o->target = [target retain];
        o->renderer = renderer;
        o->screen_frame = NSRectToCGRect([screen frame]);
        o->speed = env_number("LIU_INTRO_SPEED", 1.0);
        o->reduced = env_flag("LIU_INTRO_REDUCE")
            || [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
        o->lang = lang;
        o->show = o->reduced ? INTRO_T_REDUCED_SHOW : INTRO_T_SHOW;
        o->lift = o->reduced ? INTRO_T_REDUCED_LIFT : INTRO_T_LIFT;
        o->total = o->reduced ? INTRO_REDUCED_DURATION : INTRO_DURATION;
        const char *script = liu_dev_env("LIU_INTRO_SCRIPT");
        /* The sound is written for the picture at normal speed. */
        o->plays_sound = !env_off("LIU_INTRO_SOUND") && o->speed == 1.0;
        o->report = env_flag("LIU_INTRO_REPORT");
        o->stalls = env_flag("LIU_INTRO_STALL");
        o->reveal_monotonic = true;

        CGFloat scale = [screen backingScaleFactor];
        CGSize size = o->screen_frame.size;
        CGRect rect = target_rect(o);
        o->layout = intro_layout_make((f32)size.width, (f32)size.height, (f32)scale,
            intro_mark_height((f32)size.height), (f32)rect.origin.x, (f32)rect.origin.y,
            (f32)rect.size.width, (f32)rect.size.height, INTRO_TARGET_RADIUS);

        LiuIntroWindow *window = [[LiuIntroWindow alloc]
            initWithContentRect:[screen frame] styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered defer:NO];
        [window setReleasedWhenClosed:NO];
        [window setOpaque:NO];
        [window setBackgroundColor:[NSColor clearColor]];
        [window setHasShadow:NO];
        /* Above the menu bar and the Dock. */
        [window setLevel:CGWindowLevelForKey(kCGMainMenuWindowLevelKey) + 1];
        [window setAnimationBehavior:NSWindowAnimationBehaviorNone];
        [window setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces
            | NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle
            | NSWindowCollectionBehaviorFullScreenAuxiliary];
        /* Said out loud: a transparent window otherwise lets a click through
         * wherever it has drawn nothing — and over the terminal it draws
         * nothing. */
        [window setIgnoresMouseEvents:NO];
        o->window = window;

        LiuIntroView *view = [[LiuIntroView alloc]
            initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
        [view setWantsLayer:YES];
        CAMetalLayer *layer = (CAMetalLayer *)[view layer];
        layer.device = device;
        layer.pixelFormat = INTRO_PIXEL_FORMAT;
        layer.framebufferOnly = YES;
        layer.opaque = NO;
        layer.contentsScale = scale;
        /* If the drawable is not the view's size times `scale`, everything is
         * magnified or shrunk and off centre. */
        layer.drawableSize = CGSizeMake(size.width * scale, size.height * scale);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        layer.colorspace = space;
        CGColorSpaceRelease(space);
        o->layer = [layer retain];
        u64 token = o->token;
        [window setContentView:view];
        [view release];

        g_overlay = o;
        [target setAlphaValue:0.0];
        o->start = CACurrentMediaTime();
        [window orderFrontRegardless];
        if (o->plays_sound) {
            const char *volume = liu_dev_env("LIU_INTRO_VOLUME");
            f32 v = volume && volume[0] ? (f32)atof(volume) : INTRO_SOUND_VOLUME;
            audio_start(o->reduced, v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v), o->start, o->show,
                        o->report);
        }

        /* Whatever happens to the display link, the window is not left hidden:
         * if nothing has been drawn by the time the tour should be on, there is
         * no picture, and the intro ends. */
        NSTimer *watchdog = [NSTimer
            timerWithTimeInterval:o->show / o->speed + INTRO_WATCHDOG_GRACE repeats:NO
                            block:^(NSTimer *timer) {
            (void)timer;
            if (!g_overlay || g_overlay->token != token) return;
            if (atomic_load(&g_overlay->frames) > 0) return;
            g_overlay->ended_by_watchdog = true;
            overlay_finish(g_overlay);
        }];
        [[NSRunLoop mainRunLoop] addTimer:watchdog forMode:NSRunLoopCommonModes];
        o->watchdog = [watchdog retain];

        o->close_observer = [[[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowWillCloseNotification object:target
                         queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *note) {
            (void)note;
            if (g_overlay && g_overlay->token == token) overlay_finish(g_overlay);
        }] retain];

        /* The dark sits above every app on every Space. If the viewer goes
         * somewhere else — another app, by any route — it does not follow them
         * there for the half minute it has left: it ends. */
        o->resign_observer = [[[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidResignActiveNotification object:NSApp
                         queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *note) {
            (void)note;
            if (g_overlay && g_overlay->token == token) overlay_end(g_overlay);
        }] retain];

        f64 skip_at = env_number("LIU_INTRO_SKIP_AT", 0.0);
        if (skip_at > 0.0) {
            after_seconds(skip_at / o->speed, ^{
                if (g_overlay && g_overlay->token == token) overlay_skip(g_overlay);
            });
        }

        if (script && script[0]) run_script(o, script);

        NSNumber *display = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
        CVReturn made = CVDisplayLinkCreateWithCGDisplay(
            display ? (CGDirectDisplayID)[display unsignedIntValue] : CGMainDisplayID(), &o->link);
        if (made != kCVReturnSuccess || !o->link
            || CVDisplayLinkSetOutputCallback(o->link, overlay_link_callback, o) != kCVReturnSuccess
            || CVDisplayLinkStart(o->link) != kCVReturnSuccess) {
            /* No clock to draw by: end it now rather than wait for the watchdog. */
            overlay_finish(o);
        }
    }
}
