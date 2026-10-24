#include "voice/voice_sound.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The microphone cue (`up` = opening), term for term:
 *
 *     a, b = (520, 880) if up else (880, 520)
 *     put(x, glide(n_(0.08), a, a * 1.25) * decay(n_(0.08), 0.03, 0.003), 0)
 *     put(x, glide(n_(0.1),  b, b * 1.2)  * decay(n_(0.1),  0.035, 0.003), 0.085)
 *     return at_peak(x, -16)
 *
 * glide is a sine whose frequency slides geometrically from f0 to f1 over the
 * blip (phase = the running sum of the frequency, the sample itself included);
 * decay is exp(-t / tau) under a linear attack. Kept in f64 until the end so
 * the result matches the formula to the last bit that f32 holds. */
typedef struct { f64 at_sec, dur_sec, f0, bend, tau, attack; } Blip;

static const Blip k_blips[VOICE_SOUND_COUNT][2] = {
    [VOICE_SOUND_ON]  = { { 0.0, 0.08, 520.0, 1.25, 0.030, 0.003 },
                          { 0.085, 0.10, 880.0, 1.20, 0.035, 0.003 } },
    [VOICE_SOUND_OFF] = { { 0.0, 0.08, 880.0, 1.25, 0.030, 0.003 },
                          { 0.085, 0.10, 520.0, 1.20, 0.035, 0.003 } },
};

static const Blip *blip_of(VoiceSound which, i32 i) {
    if (which < 0 || which >= VOICE_SOUND_COUNT) which = VOICE_SOUND_ON;
    return &k_blips[which][i ? 1 : 0];
}

void voice_sound_blip(VoiceSound which, i32 i, i32 *first, i32 *count, f64 *f0, f64 *f1) {
    const Blip *b = blip_of(which, i);
    if (first) *first = (i32)(b->at_sec * VOICE_SOUND_RATE);
    if (count) *count = (i32)(b->dur_sec * VOICE_SOUND_RATE);
    if (f0) *f0 = b->f0;
    if (f1) *f1 = b->f0 * b->bend;
}

void voice_sound_render(VoiceSound which, f32 *out) {
    if (!out) return;
    static f64 x[VOICE_SOUND_FRAMES];        /* main thread only, like its one caller */
    memset(x, 0, sizeof x);
    const f64 sr = (f64)VOICE_SOUND_RATE;

    for (i32 k = 0; k < 2; k++) {
        const Blip *b = blip_of(which, k);
        const i32 at = (i32)(b->at_sec * sr), n = (i32)(b->dur_sec * sr);
        const f64 t_last = (f64)(n - 1) / sr;
        const f64 span = t_last > 1e-9 ? t_last : 1e-9;
        f64 run = 0.0;
        for (i32 i = 0; i < n && at + i < VOICE_SOUND_FRAMES; i++) {
            const f64 t = (f64)i / sr;
            run += b->f0 * pow(b->bend, t / span);
            const f64 rise = t / b->attack;
            x[at + i] += sin(2.0 * M_PI * run / sr) * exp(-t / b->tau) * (rise < 1.0 ? rise : 1.0);
        }
    }

    f64 peak = 0.0;
    for (i32 i = 0; i < VOICE_SOUND_FRAMES; i++) { const f64 v = fabs(x[i]); if (v > peak) peak = v; }
    const f64 gain = pow(10.0, -16.0 / 20.0) / (peak + 1e-12);
    for (i32 i = 0; i < VOICE_SOUND_FRAMES; i++) out[i] = (f32)(x[i] * gain);
}
