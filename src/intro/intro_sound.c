#include "intro/intro_sound.h"
#include "intro/intro_timeline.h"
#include "intro/intro_tour.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE ((f64)INTRO_SOUND_RATE)
#define TAU  6.283185307179586

/* The key is E, and everything is cut from its pentatonic scale (E F# G# B
 * C#), which cannot play a wrong interval against itself however the cues
 * fall on top of one another — and they do: a cue is fired by what the window
 * does, not by a bar line. */
#define N_E2   82.41
#define N_B2   123.47
#define N_E3   164.81
#define N_GS3  207.65
#define N_B3   246.94
#define N_E4   329.63
#define N_FS4  369.99
#define N_GS4  415.30
#define N_B4   493.88
#define N_CS5  554.37
#define N_E5   659.26
#define N_GS5  830.61
#define N_B5   987.77
#define N_CS6  1108.73
#define N_E6   1318.51

/* ---- the mix ----------------------------------------------------------- */

typedef struct {
    isize frames;
    /* Interleaved stereo: what is heard as it is, and what is sent to the
     * reverb. */
    f32 *dry, *send;
    u64 rng;
} Mix;

static bool mix_init(Mix *m, f64 seconds, u64 seed) {
    memset(m, 0, sizeof *m);
    m->frames = (isize)(seconds * RATE);
    if (m->frames <= 0) return false;
    m->dry = calloc((usize)m->frames * 2, sizeof(f32));
    m->send = calloc((usize)m->frames * 2, sizeof(f32));
    m->rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
    if (!m->dry || !m->send) { free(m->dry); free(m->send); return false; }
    return true;
}

static void mix_free(Mix *m) {
    free(m->dry);
    free(m->send);
}

/* -1..1 */
static f64 rnd(Mix *m) {
    m->rng ^= m->rng << 13;
    m->rng ^= m->rng >> 7;
    m->rng ^= m->rng << 17;
    return (f64)(m->rng >> 11) / (f64)(1ULL << 52) - 1.0;
}

static f64 rnd01(Mix *m) { return rnd(m) * 0.5 + 0.5; }

static f64 unit(f64 x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
static f64 smooth(f64 x) { x = unit(x); return x * x * (3.0 - 2.0 * x); }

/* Equal-power pan, -1 (left) .. 1 (right). */
static void pan_gains(f64 pan, f64 *l, f64 *r) {
    f64 angle = unit(pan * 0.5 + 0.5) * (TAU / 4.0);            /* 0 .. pi/2 */
    *l = cos(angle);
    *r = sin(angle);
}

static void mix_add(Mix *m, isize frame, f64 left, f64 right, f64 send) {
    if (frame < 0 || frame >= m->frames) return;
    m->dry[frame * 2] += (f32)left;
    m->dry[frame * 2 + 1] += (f32)right;
    m->send[frame * 2] += (f32)(left * send);
    m->send[frame * 2 + 1] += (f32)(right * send);
}

/* ---- the voices -------------------------------------------------------- */

/* The cursor's voice: a clean sine that starts a little sharp and settles in
 * a hundredth of a second — a "pip", the sound a `_` would make. */
static void v_blip(Mix *m, f64 at, f64 freq, f64 gain, f64 pan, f64 decay, f64 send) {
    isize start = (isize)(at * RATE), n = (isize)((decay * 7.0 + 0.01) * RATE);
    f64 l, r, phase = 0.0;
    pan_gains(pan, &l, &r);
    for (isize i = 0; i < n; i++) {
        f64 t = (f64)i / RATE;
        phase += TAU * freq * (1.0 + 0.55 * exp(-t / 0.004)) / RATE;
        f64 env = (1.0 - exp(-t / 0.0015)) * exp(-t / decay);
        f64 s = (sin(phase) + 0.16 * sin(2.0 * phase)) * env * gain;
        mix_add(m, start + i, s * l, s * r, send);
    }
}

/* The signature's voice: a glassy bell. A sine bent by itself (1:1 FM, which
 * is rich at the strike and pure a moment later), a ping of high partials on
 * the strike, and an octave of body under it. */
static void v_bell(Mix *m, f64 at, f64 freq, f64 gain, f64 pan, f64 decay, f64 bright, f64 send) {
    isize start = (isize)(at * RATE), n = (isize)((decay * 6.5 + 0.02) * RATE);
    f64 l, r;
    pan_gains(pan, &l, &r);
    for (isize i = 0; i < n; i++) {
        f64 t = (f64)i / RATE;
        f64 w = TAU * freq * t;
        f64 env = (1.0 - exp(-t / 0.002)) * exp(-t / decay);
        f64 index = bright * (1.5 * exp(-t / 0.16) + 0.22 * exp(-t / (decay * 0.8)));
        f64 body = sin(w + index * sin(w));
        f64 ping = bright * (0.20 * exp(-t / 0.07) * sin(3.0 * w)
                           + 0.09 * exp(-t / 0.035) * sin(5.03 * w));
        f64 under = 0.22 * exp(-t / (decay * 0.6)) * sin(0.5 * w) * (1.0 - exp(-t / 0.004));
        f64 s = ((body + ping) * env + under) * gain;
        /* The two ears a hair apart in phase: it sits in the room, not on the
         * speaker. */
        f64 s2 = ((sin(w + 0.35 + index * sin(w + 0.2)) + ping) * env + under) * gain;
        mix_add(m, start + i, s * l, s2 * r, send);
    }
}

/* A key: a click (a few milliseconds of noise through a resonant band) on a
 * small, low knock. `tone` is where the click sits, in Hz. */
static void v_tick(Mix *m, f64 at, f64 gain, f64 pan, f64 tone, f64 send) {
    isize start = (isize)(at * RATE), n = (isize)(0.075 * RATE);
    f64 l, r, low = 0.0, band = 0.0, phase = 0.0;
    pan_gains(pan, &l, &r);
    f64 f = 2.0 * sin(TAU * 0.5 * tone / RATE);
    for (isize i = 0; i < n; i++) {
        f64 t = (f64)i / RATE;
        f64 noise = rnd(m) * exp(-t / 0.0032);
        low += f * band;
        f64 high = noise - low - 0.45 * band;
        band += f * high;
        phase += TAU * (tone * 0.085) * (1.0 + 0.6 * exp(-t / 0.005)) / RATE;
        f64 knock = sin(phase) * exp(-t / 0.020) * (1.0 - exp(-t / 0.0006));
        f64 s = (band * 1.5 + knock * 0.55) * gain;
        mix_add(m, start + i, s * l, s * r, send);
    }
}

/* A pitched tick — "tok": the sound of one small thing arriving. A short sine
 * with one inharmonic partial on the strike, like a wood block. */
static void v_tok(Mix *m, f64 at, f64 freq, f64 gain, f64 pan, f64 decay, f64 send) {
    isize start = (isize)(at * RATE), n = (isize)((decay * 7.0 + 0.02) * RATE);
    f64 l, r;
    pan_gains(pan, &l, &r);
    for (isize i = 0; i < n; i++) {
        f64 t = (f64)i / RATE;
        f64 w = TAU * freq * t;
        f64 s = sin(w) * (1.0 - exp(-t / 0.0008)) * exp(-t / decay)
              + 0.30 * sin(2.76 * w) * exp(-t / (decay * 0.35))
              + 0.18 * rnd(m) * exp(-t / 0.0015);
        s *= gain;
        mix_add(m, start + i, s * l, s * r, send);
    }
}

/* A sine gliding from one pitch to another: something opening (up) or closing
 * (down). */
static void v_chirp(Mix *m, f64 at, f64 f0, f64 f1, f64 seconds, f64 gain, f64 pan, f64 send) {
    isize start = (isize)(at * RATE), n = (isize)(seconds * RATE);
    f64 l, r, phase = 0.0;
    pan_gains(pan, &l, &r);
    for (isize i = 0; i < n; i++) {
        f64 x = (f64)i / (f64)n;
        phase += TAU * f0 * pow(f1 / f0, x) / RATE;
        f64 env = sin(TAU * 0.5 * pow(x, 0.7));
        f64 s = (sin(phase) + 0.12 * sin(2.0 * phase)) * env * env * gain;
        mix_add(m, start + i, s * l, s * r, send);
    }
}

/* Air: noise through a band that moves from `f0` to `f1`, swelling to a peak
 * `peak` of the way through, and travelling from one side to the other. */
static void v_sweep(Mix *m, f64 from, f64 to, f64 f0, f64 f1, f64 damp, f64 gain,
                    f64 pan0, f64 pan1, f64 peak, f64 send) {
    isize start = (isize)(from * RATE), n = (isize)((to - from) * RATE);
    f64 low = 0.0, band = 0.0;
    for (isize i = 0; i < n; i++) {
        f64 x = (f64)i / (f64)n;
        f64 f = 2.0 * sin(TAU * 0.5 * f0 * pow(f1 / f0, x) / RATE);
        low += f * band;
        f64 high = rnd(m) - low - damp * band;
        band += f * high;
        f64 env = smooth(x < peak ? x / peak : (1.0 - x) / (1.0 - peak));
        f64 l, r;
        pan_gains(pan0 + (pan1 - pan0) * x, &l, &r);
        f64 s = band * env * gain;
        mix_add(m, start + i, s * l, s * r, send);
    }
}

/* Weight: a round, short thump. A sine falling from the chest to the floor,
 * pushed into a soft clip so that its overtones — which is all a laptop's
 * speakers can play of it — say how low it is. */
static void v_thump(Mix *m, f64 at, f64 gain, f64 weight) {
    isize start = (isize)(at * RATE), n = (isize)((0.30 + 0.90 * weight) * RATE);
    f64 phase = 0.0, lp = 0.0;
    const f64 drive = 2.2;
    for (isize i = 0; i < n; i++) {
        f64 t = (f64)i / RATE;
        phase += TAU * (46.0 + 74.0 * exp(-t / 0.035)) / RATE;
        f64 env = (1.0 - exp(-t / 0.002)) * exp(-t / (0.10 + 0.22 * weight));
        f64 body = tanh(drive * sin(phase) * env) / tanh(drive);
        lp += 0.12 * (rnd(m) * exp(-t / 0.006) - lp);
        f64 s = (body + lp * 0.5) * gain;
        mix_add(m, start + i, s * 0.7071, s * 0.7071, 0.12);
    }
}

/* Warmth: a few notes as plain sines with a little second and third harmonic,
 * each breathing at its own slow rate and a few cents apart between the ears.
 * No filter, no sawtooth: it is meant to be felt under things, not heard. */
static void v_pad(Mix *m, f64 from, f64 to, f64 attack, f64 release, const f64 *notes,
                  const f64 *levels, int count, f64 gain, f64 send) {
    isize start = (isize)(from * RATE), n = (isize)((to - from) * RATE);
    for (int k = 0; k < count; k++) {
        f64 f = notes[k], rate = 0.11 + 0.037 * (f64)k;
        for (isize i = 0; i < n; i++) {
            f64 t = (f64)i / RATE;
            f64 env = smooth(t / attack) * (1.0 - smooth((t - ((to - from) - release)) / release));
            f64 breath = 0.74 + 0.26 * sin(TAU * rate * t + 1.7 * (f64)k);
            f64 wl = TAU * f * t, wr = TAU * f * 1.0016 * t + 0.9;
            f64 l = sin(wl) + 0.28 * sin(2.0 * wl + 0.3) + 0.09 * sin(3.0 * wl);
            f64 r = sin(wr) + 0.28 * sin(2.0 * wr + 0.3) + 0.09 * sin(3.0 * wr);
            f64 g = env * breath * levels[k] * gain;
            mix_add(m, start + i, l * g, r * g, send);
        }
    }
}

/* The dither, heard: a flurry of tiny pitched grains from the scale, crushed
 * to a few bits and a low sample rate so that they are pixels rather than
 * bells. Rising, the grains come faster and climb (the mark filling in);
 * falling, they thin out and sink (it draining away). */
static bool v_grains(Mix *m, f64 from, f64 to, int count, bool rising, f64 gain, f64 send) {
    static const f64 scale[] = { N_E5, 739.99, N_GS5, N_B5, N_CS6, N_E6, 1479.98, 1661.22,
                                 1975.53, 2217.46, 2637.02 };
    const int notes = (int)(sizeof scale / sizeof scale[0]);
    const f64 grain = 0.030;
    isize start = (isize)(from * RATE), n = (isize)((to - from + grain + 0.01) * RATE);
    f32 *tmp = calloc((usize)n * 2, sizeof(f32));
    if (!tmp) return false;
    for (int g = 0; g < count; g++) {
        f64 u = ((f64)g + rnd01(m)) / (f64)count;
        /* More of them toward the end when rising, toward the start when falling. */
        f64 when = rising ? pow(u, 0.62) : 1.0 - pow(1.0 - u, 0.62);
        f64 height = rising ? u : 1.0 - u;
        int note = (int)floor(height * (f64)(notes - 3) + rnd01(m) * 3.0);
        if (note < 0) note = 0;
        if (note > notes - 1) note = notes - 1;
        f64 level = 0.45 + 0.55 * rnd01(m), l, r;
        pan_gains(rnd(m) * 0.85, &l, &r);
        isize at = (isize)(when * (to - from) * RATE), len = (isize)(grain * RATE);
        for (isize i = 0; i < len && at + i < n; i++) {
            f64 x = (f64)i / (f64)len;
            f64 s = sin(TAU * scale[note] * (f64)i / RATE) * sin(TAU * 0.5 * x) * level;
            tmp[(at + i) * 2] += (f32)(s * l);
            tmp[(at + i) * 2 + 1] += (f32)(s * r);
        }
    }
    /* Crush: hold every fourth sample, snap it to a coarse grid, and take the
     * edge off what that leaves. */
    const int hold = 4;
    const f64 step = 0.16, smoothing = 1.0 - exp(-TAU * 6500.0 / RATE);
    f64 held_l = 0.0, held_r = 0.0, lp_l = 0.0, lp_r = 0.0, lp2_l = 0.0, lp2_r = 0.0;
    for (isize i = 0; i < n; i++) {
        if (i % hold == 0) {
            held_l = round((f64)tmp[i * 2] / step) * step;
            held_r = round((f64)tmp[i * 2 + 1] / step) * step;
        }
        lp_l += smoothing * (held_l - lp_l);
        lp_r += smoothing * (held_r - lp_r);
        lp2_l += smoothing * (lp_l - lp2_l);
        lp2_r += smoothing * (lp_r - lp2_r);
        mix_add(m, start + i, lp2_l * gain, lp2_r * gain, send);
    }
    free(tmp);
    return true;
}

/* ---- the room, and the way out ----------------------------------------- */

#define RV_COMBS 8
#define RV_ALLS  4
#define RV_SPREAD 25

/* Eight combs, no two lengths sharing a factor worth the name: with fewer, a
 * held note finds the one comb it resonates in and the room SINGS it back,
 * ringing on at the note's own pitch. */
static const int g_comb_lengths[RV_COMBS] = { 1215, 1293, 1390, 1476, 1548, 1623, 1695, 1760 };
static const int g_all_lengths[RV_ALLS] = { 605, 480, 371, 245 };

typedef struct {
    f32 *comb[RV_COMBS];
    f32 damped[RV_COMBS];
    int comb_at[RV_COMBS], comb_len[RV_COMBS];
    f32 *all[RV_ALLS];
    int all_at[RV_ALLS], all_len[RV_ALLS];
    f32 rumble;
} Room;

static void room_free(Room *rm) {
    for (int i = 0; i < RV_COMBS; i++) free(rm->comb[i]);
    for (int i = 0; i < RV_ALLS; i++) free(rm->all[i]);
}

static bool room_init(Room *rm, int spread) {
    memset(rm, 0, sizeof *rm);
    bool ok = true;
    for (int i = 0; i < RV_COMBS; i++) {
        rm->comb_len[i] = g_comb_lengths[i] + spread;
        rm->comb[i] = calloc((usize)rm->comb_len[i], sizeof(f32));
        ok = ok && rm->comb[i];
    }
    for (int i = 0; i < RV_ALLS; i++) {
        rm->all_len[i] = g_all_lengths[i] + spread;
        rm->all[i] = calloc((usize)rm->all_len[i], sizeof(f32));
        ok = ok && rm->all[i];
    }
    if (!ok) room_free(rm);
    return ok;
}

/* A small, soft room: parallel combs with a low-pass in each loop (so the tail
 * darkens as it dies), then all-passes to smear it. A little over a second
 * long, and kept well under what is sent into it: it is there to put the
 * sounds in one place, not to be heard. The lows are kept out of it. */
static f32 room_process(Room *rm, f32 in) {
    rm->rumble += 0.0233f * (in - rm->rumble);         /* ~180 Hz */
    f32 x = in - rm->rumble, out = 0.0f;
    for (int i = 0; i < RV_COMBS; i++) {
        f32 y = rm->comb[i][rm->comb_at[i]];
        rm->damped[i] = y * 0.64f + rm->damped[i] * 0.36f;
        rm->comb[i][rm->comb_at[i]] = x + rm->damped[i] * 0.80f;
        rm->comb_at[i] = (rm->comb_at[i] + 1) % rm->comb_len[i];
        out += y;
    }
    for (int i = 0; i < RV_ALLS; i++) {
        /* A true all-pass (the same gain at every pitch). The shortcut form —
         * out = delayed - in, store in + delayed/2 — is not one: it lifts some
         * pitches by half again per stage, and four stages of that made one
         * caption's note 4 dB louder in one ear than its neighbours'. */
        f32 delayed = rm->all[i][rm->all_at[i]];
        f32 y = delayed - 0.5f * out;
        rm->all[i][rm->all_at[i]] = out + 0.5f * y;
        rm->all_at[i] = (rm->all_at[i] + 1) % rm->all_len[i];
        out = y;
    }
    return out * 0.075f;
}

/* Dry plus room, a soft clip so that a pile-up rounds off instead of
 * clipping, and a short fade at the end so that no buffer ends on a step.
 * Returns the finished samples; the mix is spent. */
static f32 *mix_master(Mix *m, bool with_room, f64 end_fade) {
    f32 *out = malloc((usize)m->frames * 2 * sizeof(f32));
    Room left, right;
    bool roomed = with_room && room_init(&left, 0);
    if (roomed && !room_init(&right, RV_SPREAD)) { room_free(&left); roomed = false; }
    if (!out) { if (roomed) { room_free(&left); room_free(&right); } return NULL; }
    isize fade = (isize)(end_fade * RATE);
    for (isize i = 0; i < m->frames; i++) {
        f32 l = m->dry[i * 2], r = m->dry[i * 2 + 1];
        if (roomed) {
            l += room_process(&left, m->send[i * 2]);
            r += room_process(&right, m->send[i * 2 + 1]);
        }
        f32 g = 1.0f;
        if (fade > 0 && i >= m->frames - fade) {
            f32 x = (f32)(m->frames - 1 - i) / (f32)fade;
            g = x * x;
        }
        out[i * 2] = tanhf(l) * g;
        out[i * 2 + 1] = tanhf(r) * g;
    }
    if (roomed) { room_free(&left); room_free(&right); }
    return out;
}

/* ---- the opening ------------------------------------------------------- */

#define OPENING_TAIL 2.6

/* The signature: `>` then `_`, B then E, on the bell — and the chord they
 * belong to, strummed low under them. */
static void signature(Mix *m, f64 at, f64 gain) {
    v_bell(m, at, N_B4, 0.22 * gain, -0.18, 1.5, 1.0, 0.50);
    v_bell(m, at + 0.17, N_E5, 0.26 * gain, 0.18, 1.9, 1.0, 0.50);
    v_bell(m, at + 0.00, N_E3, 0.10 * gain, 0.0, 2.3, 0.6, 0.40);
    v_bell(m, at + 0.03, N_B3, 0.08 * gain, -0.3, 2.1, 0.6, 0.40);
    v_bell(m, at + 0.06, N_GS4, 0.07 * gain, 0.3, 1.9, 0.6, 0.45);
}

static bool score_opening(Mix *m) {
    static const f64 low[] = { N_E2, N_B2, N_E3 }, low_levels[] = { 1.0, 0.7, 0.8 };
    static const f64 swell[] = { N_E3, N_GS3, N_B3, N_FS4 }, swell_levels[] = { 0.9, 0.6, 0.7, 0.35 };
    bool ok = true;

    /* The room goes dark: a breath, falling, and a low note coming up under it
     * that holds until the mark has drained away. */
    v_sweep(m, 0.00, 1.25, 950.0, 190.0, 0.9, 0.022, -0.25, 0.25, 0.32, 0.30);
    v_pad(m, 0.15, 4.70, 1.40, 1.30, low, low_levels, 3, 0.050, 0.10);

    /* The cursor; it blinks, and is there again. */
    v_tick(m, INTRO_T_CURSOR - 0.004, 0.05, 0.0, 2600.0, 0.20);
    v_blip(m, INTRO_T_CURSOR, N_E5, 0.17, 0.0, 0.11, 0.35);
    v_blip(m, 1.10, N_E5, 0.06, 0.0, 0.08, 0.30);

    /* The prompt arrives beside it: a key, and the cursor's note a fourth down. */
    v_tick(m, INTRO_T_PROMPT - 0.004, 0.10, -0.15, 2300.0, 0.20);
    v_blip(m, INTRO_T_PROMPT, N_B4, 0.16, -0.12, 0.12, 0.35);

    /* The camera pulls back and the mark fills in through its dither front:
     * pixels, faster and higher, air rising with them, warmth swelling under. */
    ok = ok && v_grains(m, INTRO_T_PULL, INTRO_T_LOCK + 0.02, 46, true, 0.085, 0.35);
    v_sweep(m, INTRO_T_PULL - 0.05, INTRO_T_LOCK + 0.03, 500.0, 4200.0, 0.7, 0.020, -0.5, 0.5, 0.86, 0.40);
    v_pad(m, INTRO_T_PULL, 4.35, 0.85, 1.10, swell, swell_levels, 4, 0.055, 0.15);

    /* The lock: weight, the signature, and a scatter of pixels. */
    v_thump(m, INTRO_T_LOCK, 0.50, 1.0);
    signature(m, INTRO_T_LOCK, 1.0);
    ok = ok && v_grains(m, INTRO_T_LOCK, INTRO_T_LOCK + 0.32, 14, true, 0.045, 0.50);

    /* The mark drains back into its cursor, which is lit again. */
    ok = ok && v_grains(m, 3.72, 4.20, 26, false, 0.065, 0.35);
    v_sweep(m, 3.70, 4.22, 3200.0, 400.0, 0.7, 0.014, 0.4, 0.0, 0.25, 0.35);
    v_blip(m, 4.10, N_E5, 0.13, 0.0, 0.10, 0.35);

    /* The cursor swells and flies out into the window's frame: air opening to
     * both sides, and a tone rising an octave with it. */
    v_sweep(m, INTRO_T_FLY - 0.04, INTRO_T_LAND + 0.10, 220.0, 2400.0, 0.8, 0.026, 0.0, -0.8, 0.82, 0.35);
    v_sweep(m, INTRO_T_FLY - 0.04, INTRO_T_LAND + 0.10, 240.0, 2600.0, 0.8, 0.026, 0.0, 0.8, 0.82, 0.35);
    v_chirp(m, INTRO_T_FLY, N_E4, N_E5, INTRO_T_LAND - INTRO_T_FLY, 0.045, 0.0, 0.30);

    /* It lands, and the window is there: weight again, an open chord, and the
     * dither it is uncovered through. */
    v_thump(m, INTRO_T_LAND - 0.01, 0.36, 0.55);
    v_bell(m, INTRO_T_LAND, N_E4, 0.12, -0.20, 1.6, 0.8, 0.45);
    v_bell(m, INTRO_T_LAND + 0.02, N_B4, 0.11, 0.20, 1.5, 0.8, 0.45);
    v_bell(m, INTRO_T_LAND + 0.04, N_GS5, 0.055, 0.0, 1.2, 0.8, 0.50);
    ok = ok && v_grains(m, INTRO_T_LAND + 0.02, INTRO_T_LAND + 0.46, 16, true, 0.032, 0.40);
    return ok;
}

/* Reduce Motion: the mark simply appears and gives way to the window, so the
 * sound keeps what stands still — the signature, the weight — and drops what
 * moved with the picture: the pixels, the air, the rising tone. */
static bool score_opening_reduced(Mix *m) {
    static const f64 low[] = { N_E2, N_B2, N_E3, N_GS3 }, low_levels[] = { 1.0, 0.7, 0.8, 0.5 };
    v_pad(m, 0.05, INTRO_T_REDUCED_HAND + 0.80, 0.50, 1.20, low, low_levels, 4, 0.050, 0.10);
    v_thump(m, INTRO_T_REDUCED_MARK, 0.40, 0.8);
    signature(m, INTRO_T_REDUCED_MARK, 0.9);
    v_thump(m, INTRO_T_REDUCED_HAND, 0.28, 0.45);
    v_bell(m, INTRO_T_REDUCED_HAND, N_E4, 0.11, -0.20, 1.6, 0.8, 0.45);
    v_bell(m, INTRO_T_REDUCED_HAND + 0.02, N_B4, 0.10, 0.20, 1.5, 0.8, 0.45);
    return true;
}

f32 *intro_sound_opening(bool reduced, usize *out_frames) {
    if (out_frames) *out_frames = 0;
    Mix m;
    f64 seconds = (reduced ? INTRO_T_REDUCED_SHOW : INTRO_T_SHOW) + OPENING_TAIL;
    if (!mix_init(&m, seconds, 0)) return NULL;
    bool ok = reduced ? score_opening_reduced(&m) : score_opening(&m);
    f32 *out = ok ? mix_master(&m, true, 0.6) : NULL;
    if (out && out_frames) *out_frames = (usize)m.frames;
    mix_free(&m);
    return out;
}

/* ---- the bed ----------------------------------------------------------- */

/* One loop, in seconds. Every frequency in it — the notes, the beating between
 * the ears, the slow breathing — completes a whole number of cycles in this
 * long, so the end runs into the beginning without a seam. (Which is why there
 * is no room on it and no noise in it: neither would come round.) */
#define BED_SECONDS 8.0

f32 *intro_sound_bed(usize *out_frames) {
    static const struct { f64 freq, level; int breaths; } notes[] = {
        { N_E2, 0.50, 1 }, { N_B2, 0.40, 2 }, { N_E3, 0.45, 3 },
        { N_GS3, 0.30, 1 }, { N_B3, 0.28, 2 }, { N_FS4, 0.11, 3 },
    };
    if (out_frames) *out_frames = 0;
    isize frames = (isize)(BED_SECONDS * RATE);
    f32 *out = calloc((usize)frames * 2, sizeof(f32));
    if (!out) return NULL;
    const f64 gain = 0.034;
    for (usize k = 0; k < sizeof notes / sizeof notes[0]; k++) {
        /* To the nearest eighth of a hertz: a whole number of cycles a loop. */
        f64 f = round(notes[k].freq * BED_SECONDS) / BED_SECONDS;
        f64 fr = f + 1.0 / BED_SECONDS;                     /* one slow beat a loop */
        for (isize i = 0; i < frames; i++) {
            f64 t = (f64)i / RATE;
            f64 breath = 0.72 + 0.28 * sin(TAU * (f64)notes[k].breaths * t / BED_SECONDS + 1.3 * (f64)k);
            f64 wl = TAU * f * t, wr = TAU * fr * t;
            f64 g = breath * notes[k].level * gain;
            out[i * 2] += (f32)((sin(wl) + 0.26 * sin(2.0 * wl)) * g);
            out[i * 2 + 1] += (f32)((sin(wr) + 0.26 * sin(2.0 * wr)) * g);
        }
    }
    if (out_frames) *out_frames = (usize)frames;
    return out;
}

/* ---- the cues ---------------------------------------------------------- */

i32 intro_cue_variants(IntroCue cue) {
    switch (cue) {
    case INTRO_CUE_FEATURE: return INTRO_FEATURE_COUNT;
    case INTRO_CUE_SPLIT:   return 2;
    case INTRO_CUE_TAB:     return 3;
    case INTRO_CUE_KEY:     return 6;
    case INTRO_CUE_THEME:   return 4;
    default:                return 1;
    }
}
_Static_assert(INTRO_FEATURE_COUNT <= INTRO_CUE_MAX_VARIANTS, "a caption's note per feature");

const char *intro_cue_name(IntroCue cue) {
    static const char *names[INTRO_CUE_COUNT] = {
        "none", "feature", "split", "tab", "tab_switch", "group", "fold", "unfold", "palette",
        "key", "enter", "dismiss", "rail_open", "rail_close", "voice_on", "voice_off", "theme",
        "close",
    };
    return cue >= 0 && cue < INTRO_CUE_COUNT ? names[cue] : "?";
}

bool intro_cue_rings(IntroCue cue) {
    return cue == INTRO_CUE_FEATURE || cue == INTRO_CUE_GROUP || cue == INTRO_CUE_PALETTE
        || cue == INTRO_CUE_THEME || cue == INTRO_CUE_CLOSE;
}

static f64 cue_seconds(IntroCue cue) {
    if (cue == INTRO_CUE_CLOSE) return 3.6;
    return intro_cue_rings(cue) ? 2.4 : 0.65;
}

f32 *intro_sound_cue(IntroCue cue, i32 variant, usize *out_frames) {
    if (out_frames) *out_frames = 0;
    if (cue <= INTRO_CUE_NONE || cue >= INTRO_CUE_COUNT) return NULL;
    i32 v = variant % intro_cue_variants(cue);
    if (v < 0) v = 0;
    Mix m;
    if (!mix_init(&m, cue_seconds(cue), 0x9E3779B97F4A7C15ULL ^ ((u64)cue * 0x100000001B3ULL + (u64)v * 7919ULL)))
        return NULL;
    /* A hair of lead, so that nothing starts on the buffer's first sample. */
    const f64 t = 0.003;
    switch (cue) {
    case INTRO_CUE_FEATURE: {
        /* One note per caption: a small tune over the eight of them, ending on
         * B — the `>` — so that the close's B-to-E answers it. */
        static const f64 tune[INTRO_CUE_MAX_VARIANTS] = { N_E4, N_GS4, N_B4, N_CS5, N_B4, N_GS4, N_FS4, N_B4 };
        v_bell(&m, t, tune[v], 0.150, (v % 2) ? 0.22 : -0.22, 1.05, 0.9, 0.45);
        break;
    }
    case INTRO_CUE_SPLIT:
        /* One thing becomes two: a key, then a tok on each side (or, stacked,
         * one low and one high). */
        v_tick(&m, t, 0.085, 0.0, 2400.0, 0.25);
        if (v == 0) {
            v_tok(&m, t + 0.012, N_E5, 0.13, -0.75, 0.045, 0.35);
            v_tok(&m, t + 0.085, N_B5, 0.13, 0.75, 0.045, 0.35);
        } else {
            v_tok(&m, t + 0.012, N_B5, 0.13, 0.0, 0.045, 0.35);
            v_tok(&m, t + 0.085, N_E5, 0.13, 0.0, 0.045, 0.35);
        }
        break;
    case INTRO_CUE_TAB: {
        /* "Tik": one tab, one tok — each a step higher than the last. */
        static const f64 pitch[3] = { N_B5, N_CS6, N_E6 };
        v_tick(&m, t, 0.060, 0.1 * (f64)v, 2700.0, 0.25);
        v_tok(&m, t + 0.006, pitch[v], 0.15, -0.2 + 0.25 * (f64)v, 0.040, 0.38);
        break;
    }
    case INTRO_CUE_TAB_SWITCH:
        v_tok(&m, t, N_E6, 0.075, 0.15, 0.028, 0.30);
        v_tok(&m, t + 0.050, N_B5, 0.085, -0.15, 0.032, 0.30);
        break;
    case INTRO_CUE_GROUP:
        /* The tabs gather — tik, tik, tik — and are one thing. */
        v_tok(&m, t, N_B5, 0.095, 0.30, 0.030, 0.30);
        v_tok(&m, t + 0.065, N_CS6, 0.095, 0.10, 0.030, 0.30);
        v_tok(&m, t + 0.130, N_E6, 0.105, -0.10, 0.030, 0.30);
        v_bell(&m, t + 0.200, N_E5, 0.10, -0.25, 0.70, 0.8, 0.45);
        v_bell(&m, t + 0.200, N_B5, 0.07, -0.05, 0.60, 0.8, 0.45);
        break;
    case INTRO_CUE_FOLD:
        v_chirp(&m, t, 940.0, 470.0, 0.120, 0.085, -0.3, 0.30);
        v_tick(&m, t + 0.105, 0.075, -0.3, 2000.0, 0.25);
        break;
    case INTRO_CUE_UNFOLD:
        v_tick(&m, t, 0.065, -0.3, 2500.0, 0.25);
        v_chirp(&m, t + 0.010, 470.0, 940.0, 0.120, 0.085, -0.3, 0.30);
        break;
    case INTRO_CUE_PALETTE:
        /* It opens over everything: up an octave, with a little air. */
        v_chirp(&m, t, N_E4, N_E5, 0.095, 0.075, 0.0, 0.35);
        v_sweep(&m, t, t + 0.16, 1200.0, 5200.0, 0.8, 0.012, -0.3, 0.3, 0.35, 0.40);
        v_bell(&m, t + 0.085, N_E5, 0.060, 0.0, 0.35, 0.7, 0.50);
        break;
    case INTRO_CUE_KEY: {
        /* A typed key. Six of them, no two the same, none of them loud. */
        static const f64 tone[6] = { 1750.0, 2100.0, 1900.0, 2300.0, 1650.0, 2200.0 };
        static const f64 side[6] = { -0.12, 0.10, 0.02, -0.06, 0.14, -0.02 };
        v_tick(&m, t, 0.070 + 0.006 * (f64)(v % 3), side[v], tone[v], 0.22);
        break;
    }
    case INTRO_CUE_ENTER:
        /* A heavier key, and yes: B, E. */
        v_tick(&m, t, 0.105, 0.0, 1650.0, 0.25);
        v_blip(&m, t + 0.012, N_B4, 0.085, -0.1, 0.050, 0.35);
        v_blip(&m, t + 0.070, N_E5, 0.095, 0.1, 0.075, 0.35);
        break;
    case INTRO_CUE_DISMISS:
        v_chirp(&m, t, N_E5, N_E4, 0.095, 0.085, 0.0, 0.30);
        break;
    case INTRO_CUE_RAIL_OPEN:
        /* It slides out from the left edge, and stops. */
        v_sweep(&m, t, t + 0.200, 500.0, 2600.0, 0.8, 0.034, -0.6, -0.15, 0.70, 0.30);
        v_tok(&m, t + 0.185, N_E5, 0.080, -0.2, 0.035, 0.30);
        break;
    case INTRO_CUE_RAIL_CLOSE:
        v_sweep(&m, t, t + 0.190, 2600.0, 500.0, 0.8, 0.030, -0.15, -0.6, 0.25, 0.30);
        v_tok(&m, t + 0.175, N_B4, 0.065, -0.5, 0.035, 0.30);
        break;
    case INTRO_CUE_VOICE_ON:
        /* "Listening": two soft notes up. */
        v_blip(&m, t, N_E5, 0.100, 0.20, 0.070, 0.40);
        v_blip(&m, t + 0.095, N_B5, 0.105, 0.28, 0.095, 0.40);
        break;
    case INTRO_CUE_VOICE_OFF:
        v_blip(&m, t, N_B5, 0.080, 0.28, 0.065, 0.40);
        v_blip(&m, t + 0.090, N_E5, 0.085, 0.20, 0.085, 0.40);
        break;
    case INTRO_CUE_THEME: {
        /* The whole window changes colour: a wash across it and one note,
         * brightest for the light theme; home is the tonic again. */
        static const f64 note[4] = { N_GS4, N_B4, N_E5, N_E4 };
        static const f64 top[4] = { 3600.0, 4400.0, 6200.0, 2600.0 };
        v_sweep(&m, t, t + 0.34, 700.0, top[v], 0.8, 0.022, -0.7, 0.7, 0.45, 0.40);
        v_bell(&m, t + 0.040, note[v], 0.130, 0.0, 0.95, 0.9, 0.50);
        if (v == 2) v_bell(&m, t + 0.070, N_B5, 0.050, 0.3, 0.70, 0.9, 0.50);
        break;
    }
    case INTRO_CUE_CLOSE:
        /* The dark lifts: the signature once more, softer, and a breath out. */
        v_thump(&m, t, 0.24, 0.5);
        signature(&m, t, 0.72);
        v_sweep(&m, t, t + 0.95, 600.0, 3000.0, 0.8, 0.018, -0.5, 0.5, 0.30, 0.40);
        break;
    default:
        break;
    }
    f32 *out = mix_master(&m, true, intro_cue_rings(cue) ? 0.35 : 0.06);
    if (out && out_frames) *out_frames = (usize)m.frames;
    mix_free(&m);
    return out;
}

/* ---- a whole run, mixed down ------------------------------------------- */

#define MIX_TAIL 3.0

f64 intro_sound_duration(bool reduced) {
    return (reduced ? INTRO_REDUCED_DURATION : INTRO_DURATION) + MIX_TAIL;
}

typedef struct {
    f32 *out;
    isize frames;
    f32 *cues[INTRO_CUE_COUNT][INTRO_CUE_MAX_VARIANTS];
    usize cue_frames[INTRO_CUE_COUNT][INTRO_CUE_MAX_VARIANTS];
    bool failed;
} Mixdown;

static void paste(Mixdown *md, const f32 *samples, usize frames, f64 at) {
    isize start = (isize)(at * RATE);
    for (usize i = 0; i < frames; i++) {
        isize f = start + (isize)i;
        if (f < 0 || f >= md->frames) continue;
        md->out[f * 2] += samples[i * 2];
        md->out[f * 2 + 1] += samples[i * 2 + 1];
    }
}

static void paste_cue(Mixdown *md, IntroCue cue, i32 variant, f64 at) {
    if (cue <= INTRO_CUE_NONE || cue >= INTRO_CUE_COUNT) return;
    i32 v = variant % intro_cue_variants(cue);
    if (v < 0) v = 0;
    if (!md->cues[cue][v]) md->cues[cue][v] = intro_sound_cue(cue, v, &md->cue_frames[cue][v]);
    if (!md->cues[cue][v]) { md->failed = true; return; }
    paste(md, md->cues[cue][v], md->cue_frames[cue][v], at);
}

f32 *intro_sound_mixdown(bool reduced, usize *out_frames) {
    if (out_frames) *out_frames = 0;
    const f64 show = reduced ? INTRO_T_REDUCED_SHOW : INTRO_T_SHOW;
    const f64 lift = reduced ? INTRO_T_REDUCED_LIFT : INTRO_T_LIFT;
    Mixdown md = {0};
    md.frames = (isize)(intro_sound_duration(reduced) * RATE);
    md.out = calloc((usize)md.frames * 2, sizeof(f32));
    usize opening_frames = 0, bed_frames = 0;
    f32 *opening = intro_sound_opening(reduced, &opening_frames);
    f32 *bed = intro_sound_bed(&bed_frames);
    if (!md.out || !opening || !bed) md.failed = true;

    if (!md.failed) {
        paste(&md, opening, opening_frames, 0.0);

        /* The bed: up under the tour, round and round, and out as it ends. */
        f64 from = show - INTRO_BED_LEAD, to = lift + INTRO_BED_FADE_OUT;
        isize start = (isize)(from * RATE), n = (isize)((to - from) * RATE);
        for (isize i = 0; i < n; i++) {
            isize f = start + i;
            if (f < 0 || f >= md.frames) continue;
            f64 t = (f64)i / RATE;
            f64 g = smooth(t / INTRO_BED_FADE_IN) * (1.0 - smooth((t - (lift - from)) / INTRO_BED_FADE_OUT));
            usize b = (usize)i % bed_frames;
            md.out[f * 2] += (f32)(bed[b * 2] * g);
            md.out[f * 2 + 1] += (f32)(bed[b * 2 + 1] * g);
        }

        for (i32 k = 0; k < INTRO_FEATURE_COUNT; k++)
            paste_cue(&md, INTRO_CUE_FEATURE, k, show + intro_feature_begin(k) + INTRO_FEATURE_CUE_DELAY);

        usize count = 0;
        const IntroTourStep *steps = intro_tour_script(&count);
        for (usize i = 0; i < count; i++) {
            if (steps[i].cue == INTRO_CUE_NONE) continue;
            if (steps[i].kind == INTRO_TOUR_TEXT) {
                for (usize c = 0; steps[i].text && steps[i].text[c]; c++)
                    paste_cue(&md, steps[i].cue, (i32)c, show + intro_tour_step_time(&steps[i], c));
            } else {
                paste_cue(&md, steps[i].cue, steps[i].cue_variant, show + intro_tour_step_time(&steps[i], 0));
            }
        }
        paste_cue(&md, INTRO_CUE_CLOSE, 0, lift);
    }

    for (int c = 0; c < INTRO_CUE_COUNT; c++)
        for (int v = 0; v < INTRO_CUE_MAX_VARIANTS; v++) free(md.cues[c][v]);
    free(opening);
    free(bed);
    if (md.failed) { free(md.out); return NULL; }
    if (out_frames) *out_frames = (usize)md.frames;
    return md.out;
}

/* ---- WAV --------------------------------------------------------------- */

static void put_u16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put_u32(u8 *p, u32 v) { put_u16(p, v & 0xFFFF); put_u16(p + 2, v >> 16); }

u8 *intro_sound_wav(const f32 *samples, usize count, usize *out_size) {
    if (out_size) *out_size = 0;
    if (!samples || count == 0 || count > (usize)0x3FFFFFFF) return NULL;
    u32 bytes = (u32)count * 2;
    u8 *wav = malloc(44 + (usize)bytes);
    if (!wav) return NULL;
    memcpy(wav, "RIFF", 4);          put_u32(wav + 4, 36 + bytes);
    memcpy(wav + 8, "WAVEfmt ", 8);  put_u32(wav + 16, 16);
    put_u16(wav + 20, 1);            put_u16(wav + 22, 2);
    put_u32(wav + 24, INTRO_SOUND_RATE);
    put_u32(wav + 28, INTRO_SOUND_RATE * 4);
    put_u16(wav + 32, 4);            put_u16(wav + 34, 16);
    memcpy(wav + 36, "data", 4);     put_u32(wav + 40, bytes);
    for (usize i = 0; i < count; i++) {
        f32 s = samples[i] < -1.0f ? -1.0f : (samples[i] > 1.0f ? 1.0f : samples[i]);
        i32 v = (i32)roundf(s * 32767.0f);
        put_u16(wav + 44 + i * 2, (u32)(v & 0xFFFF));
    }
    if (out_size) *out_size = 44 + (usize)bytes;
    return wav;
}
