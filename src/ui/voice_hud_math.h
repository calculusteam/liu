/*
 * Arithmetic for the voice card's layout and waveform animation.
 *
 * The card uses reference pixels in VHUD_FILM_* and scales them with
 * VHUD_SCALE. It is positioned at the window's bottom-right and uses a
 * spring entrance, a sliding waveform, and a black drop shadow.
 *
 * Keeping the calculations independent of AppState and the renderer lets the
 * drawing code use the same geometry and timing values across frames.
 */
#ifndef UI_VOICE_HUD_MATH_H
#define UI_VOICE_HUD_MATH_H

#include "core/types.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline f32 vhud_clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* Exponential approach with a time constant, evaluated against REAL elapsed
 * time rather than per frame.
 *
 * The usual shortcut is `v += (target - v) * 0.2f` once a frame, and it is
 * wrong for anything a user watches: it settles twice as fast at 120 Hz as at
 * 60, so the same animation is a different animation on a different display.
 * `tau` is the time to close ~63% of the remaining distance, in seconds. */
static inline f32 vhud_approach(f32 v, f32 target, f32 tau, f32 dt) {
    /* No time passed, so nothing moves. This is NOT the same as "no smoothing
     * requested": treated as that, a frame with a zero delta would snap the
     * value straight to its target, a jump in an animation meant to have none.
     * dt can genuinely be 0: two paints inside one tick of a coarse clock will
     * do it. */
    if (dt <= 0.0f)  return v;
    if (tau <= 0.0f) return target;      /* smoothing explicitly disabled */
    f32 k = 1.0f - expf(-dt / tau);
    if (k > 1.0f) k = 1.0f;
    return v + (target - v) * k;
}

/* ---- the card ------------------------------------------------------------
 *
 * The reference numbers, in reference pixels. The reference design sets a
 * terminal line every 25 px where the app's is 18.5 pt, and that ratio is the
 * conversion: the card keeps its size RELATIVE TO THE TEXT it floats over,
 * not a pixel count. */
#define VHUD_FILM_PT        (18.5f / 25.0f)
/* ...and then all of it, uniformly, a little smaller. One number: the card's
 * proportions are kept at any value of it. */
#define VHUD_SCALE          0.8f
/* How far it stays from the window's right and bottom edges, in points — the
 * gutter the toast keeps on the same edge. */
#define VHUD_MARGIN_PT      16.0f
#define VHUD_FILM_W         500.0f
#define VHUD_FILM_H         92.0f
#define VHUD_FILM_RADIUS    26.0f
#define VHUD_FILM_DOT       16.0f               /* the light's diameter */
#define VHUD_FILM_DOT_X     32.0f
#define VHUD_FILM_BAR_X0    76.0f
#define VHUD_FILM_BAR_PAD_R 30.0f
#define VHUD_FILM_BAR_W     4.0f
#define VHUD_FILM_BAR_MIN   6.0f                /* a bar with nothing in it: a dot */
#define VHUD_FILM_BAR_SPAN  54.0f               /* ...and what a full one adds */
#define VHUD_FILM_RISE      12.0f               /* it comes up this far as it opens */
#define VHUD_FILM_SHADOW    70.0f
#define VHUD_FILM_SHADOW_Y  24.0f

typedef struct {
    f32 x, y, w, h, radius;
    f32 dot_cx, dot_cy, dot_r;
    f32 bar_x0, bar_track, bar_pitch, bar_w, bar_min, bar_span;
    f32 rise, shadow, shadow_y;
    f32 unit;                    /* device pixels per reference pixel */
} VhudGeom;

/* ---- the scrolling history ---------------------------------------------
 *
 * 34 bars, one every 36 ms, so the row holds the last 1.2 s and moves a
 * bar's pitch 28 times a second. */
#define VHUD_WAVE_BARS 34
#define VHUD_WAVE_MS   36
#define VHUD_WAVE_DT   (VHUD_WAVE_MS / 1000.0)

/* Where the card is in a win_w x win_h window (device pixels): the bottom
 * right corner, a margin in from both edges. `top_floor` is the lowest y the
 * window's own chrome reaches at the top (the tab strip) and `bottom_inset`
 * what it takes at the bottom (the status bar, when it is shown): the card
 * sits on neither. A window too narrow for it gets the whole card smaller,
 * not a squeezed one. */
static inline void vhud_geom(VhudGeom *g, f32 win_w, f32 win_h, f32 top_floor,
                             f32 bottom_inset, f32 dpi) {
    f32 u = VHUD_FILM_PT * VHUD_SCALE * dpi;
    const f32 margin = VHUD_MARGIN_PT * dpi;
    if (VHUD_FILM_W * u > win_w - 2.0f * margin && win_w > 2.0f * margin)
        u = (win_w - 2.0f * margin) / VHUD_FILM_W;
    if (u < 0.25f * dpi) u = 0.25f * dpi;
    g->unit = u;
    g->w = floorf(VHUD_FILM_W * u + 0.5f);
    g->h = floorf(VHUD_FILM_H * u + 0.5f);
    g->radius = VHUD_FILM_RADIUS * u;
    f32 x = win_w - margin - g->w;
    if (x < 0.0f) x = 0.0f;
    g->x = floorf(x + 0.5f);
    if (bottom_inset < 0.0f) bottom_inset = 0.0f;
    f32 y = win_h - bottom_inset - margin - g->h;
    if (y < top_floor + margin) y = top_floor + margin;      /* a very short window */
    if (y + g->h > win_h) y = win_h - g->h;
    if (y < 0.0f) y = 0.0f;
    g->y = floorf(y + 0.5f);
    g->dot_r  = VHUD_FILM_DOT * 0.5f * u;
    g->dot_cx = g->x + (VHUD_FILM_DOT_X + VHUD_FILM_DOT * 0.5f) * u;
    g->dot_cy = g->y + g->h * 0.5f;
    g->bar_x0    = g->x + VHUD_FILM_BAR_X0 * u;
    g->bar_track = g->w - (VHUD_FILM_BAR_X0 + VHUD_FILM_BAR_PAD_R) * u;
    g->bar_pitch = g->bar_track / (f32)VHUD_WAVE_BARS;
    g->bar_w     = VHUD_FILM_BAR_W * u;
    g->bar_min   = VHUD_FILM_BAR_MIN * u;
    g->bar_span  = VHUD_FILM_BAR_SPAN * u;
    g->rise      = VHUD_FILM_RISE * u;
    g->shadow    = VHUD_FILM_SHADOW * u;
    g->shadow_y  = VHUD_FILM_SHADOW_Y * u;
}

/* The entrance uses a spring response with hz 3.2 and zeta 0.78: it reaches
 * halfway at 75 ms and 0.9 at 145 ms, overshoots by 2% at 250 ms, and returns
 * within 0.5% by 360 ms. The response drives opacity, scale, and vertical
 * offset. Exit uses a 0.24 s smoothstep. */
#define VHUD_OPEN_HZ    3.2f
#define VHUD_OPEN_ZETA  0.78f
#define VHUD_CLOSE_SEC  0.24f

static inline f32 vhud_spring(f32 x, f32 hz, f32 zeta) {
    if (x <= 0.0f) return 0.0f;
    const f32 w = 2.0f * (f32)M_PI * hz;
    if (zeta >= 1.0f) return 1.0f - expf(-w * x) * (1.0f + w * x);
    const f32 wd = w * sqrtf(1.0f - zeta * zeta);
    return 1.0f - expf(-zeta * w * x) * (cosf(wd * x) + ((zeta * w) / wd) * sinf(wd * x));
}

static inline f32 vhud_smooth(f32 x) { x = vhud_clamp01(x); return x * x * (3.0f - 2.0f * x); }

/* How much of the card is there: `since_open` seconds after it opened, and —
 * when it is on its way out — `since_close` seconds after that began (pass a
 * negative number while it is open). */
static inline f32 vhud_presence(f32 since_open, f32 since_close) {
    f32 on = vhud_spring(since_open, VHUD_OPEN_HZ, VHUD_OPEN_ZETA);
    if (since_close >= 0.0f) on *= 1.0f - vhud_smooth(since_close / VHUD_CLOSE_SEC);
    return on;
}
static inline f32 vhud_presence_alpha(f32 on) { return vhud_clamp01(on * 1.5f); }
static inline f32 vhud_presence_scale(f32 on) { return 0.9f + 0.1f * on; }

/* The history shifts one slot per interval and is drawn offset by the elapsed
 * fraction so adjacent indices meet at the same x-coordinate at each shift. An
 * extra bar starts beyond the right edge and enters the final visible slot
 * during the interval, preventing a gap at the live edge. */
static inline f32 vhud_bar_x(f32 origin, i32 i, f32 slot, f32 frac) {
    return origin + (f32)i * slot - frac * slot;
}

/* Everything else about a bar is a function of WHERE it is drawn, 0 at the
 * left end of the track and 1 at the right — never of its index, which steps
 * at the push while the bar itself does not move (the row then changes
 * brightness 28 times a second with nothing travelling). */
static inline f32 vhud_bar_pos(f32 x, f32 x0, f32 track) {
    return track > 0.0f ? vhud_clamp01((x - x0) / track) : 0.0f;
}

/* A bar is carried out of the left end over an eighth of the track and comes
 * in at the right end already faintly there. */
static inline f32 vhud_bar_edge(f32 pos) {
    f32 e = pos * 8.0f;
    const f32 r = (1.0f - pos) * 8.0f + 0.15f;
    if (r < e) e = r;
    return vhud_clamp01(e);
}

/* While the recording is being read back, a highlight travels along the row:
 * how lit a bar at `pos` is with the highlight's centre at `comet`. */
#define VHUD_COMET_WIDTH 0.09f
static inline f32 vhud_bar_near(f32 pos, f32 comet) {
    const f32 d = (pos - comet) / VHUD_COMET_WIDTH;
    return expf(-d * d);
}

/* `scan` is 0 while listening and 1 while transcribing (eased between). While
 * listening the row brightens toward the present; while transcribing it dims
 * and only the highlight is bright, and lifts what it passes by a third. */
static inline f32 vhud_bar_alpha(f32 pos, f32 near, f32 scan) {
    const f32 listen = 0.45f + 0.55f * pos, read = 0.4f + 0.6f * near;
    return vhud_bar_edge(pos) * (listen + (read - listen) * scan);
}
static inline f32 vhud_bar_height(f32 level, f32 near, f32 scan, f32 bar_min, f32 bar_span) {
    return (bar_min + vhud_clamp01(level) * bar_span) * (1.0f + 0.35f * near * scan);
}

/* Where the highlight is, `since` seconds into the wait. Whisper reports a
 * fraction per 30 s window and nearly every dictation is one window, so
 * `progress` is 0 for the whole wait far more often than not: the highlight
 * then just crosses the row, from fully off the left end to fully off the
 * right one (which is what makes the repeat invisible — at both ends it is not
 * on the card), once every VHUD_COMET_SEC. When a real fraction does arrive,
 * it sits there. */
#define VHUD_COMET_SEC 1.15
static inline f32 vhud_comet(f64 since, f32 progress) {
    if (progress > 0.0f) return progress > 1.0f ? 1.0f : progress;
    const f64 ph = fmod(since < 0.0 ? 0.0 : since, VHUD_COMET_SEC) / VHUD_COMET_SEC;
    return (f32)(-0.25 + 1.5 * ph);
}

/* ---- how tall --------------------------------------------------------------
 *
 * Reference amplitude the waveform's height is measured against.
 *
 * Microphone gain varies by more than an order of magnitude across machines
 * and input devices, and drawn against a fixed full scale a quiet input is a
 * flat line, indistinguishable from the failure the waveform exists to show
 * (a muted or wrong input device). So the trace is scaled to the loudest
 * thing recently heard: it rises instantly, and falls with a long time
 * constant so a pause between words does not pump the whole picture. */
#define VHUD_GAIN_TAU   3.0f

/* The gain floor is slightly above VOICE_SPEECH_LEVEL, so samples treated as
 * silence remain near-zero after normalization. */
#define VHUD_GAIN_FLOOR 0.05f

static inline f32 vhud_gain_ref(f32 ref, f32 raw, f32 dt) {
    if (raw > ref) return raw;
    return vhud_approach(ref, raw, VHUD_GAIN_TAU, dt);
}

/* Bar height, 0..1, for a peak of `raw` against reference `ref`.
 *
 * The square root is a display curve, not smoothing: speech spends most of its
 * time well below its own peak, and a linear bar barely leaves the floor. It
 * is applied AFTER normalisation on purpose — applying it to the raw peak
 * first compresses the signal before the scale is chosen, so what is left to
 * normalise is already flat. */
static inline f32 vhud_bar_level(f32 raw, f32 ref) {
    f32 r = ref > VHUD_GAIN_FLOOR ? ref : VHUD_GAIN_FLOOR;
    f32 v = raw / r;
    if (v <= 0.0f) return 0.0f;
    if (v > 1.0f)  v = 1.0f;
    return sqrtf(v);
}

/* Capture delivers 4096-frame batches about every 85 ms, while the waveform
 * advances one bar every 36 ms. Queue levels when they arrive and consume one
 * per interval to avoid gaps between capture buffers.
 *
 * Prime playback after the queue exceeds the largest observed capture burst.
 * On underrun, fade the most recent level and count it; after a long stall,
 * discard stale levels and wait for the queue to fill again. */
#define VHUD_QUEUE 24
#define VHUD_PRIME 2            /* never start with fewer than this in hand */
#define VHUD_TRIM  12           /* more than this waiting means frames stopped */
#define VHUD_STALL 4            /* intervals in one step that count as a stall */

typedef struct {
    /* One slot more than there are visible bars — index VHUD_WAVE_BARS is the
     * bar arriving from beyond the right edge. See vhud_bar_x(). */
    f32 bars[VHUD_WAVE_BARS + 1];
    f64 last_push;              /* 0 until the scroll has started */
    f32 q[VHUD_QUEUE];          /* heard, not yet shown; oldest first */
    i32 qn;
    i32 burst;                  /* the most levels one buffer has brought */
    f32 last;                   /* the level that entered last */
    u32 pushed, starved, trimmed;
} VhudWave;

static inline void vhud_wave_reset(VhudWave *w) {
    for (i32 i = 0; i <= VHUD_WAVE_BARS; i++) w->bars[i] = 0.0f;
    w->last_push = 0.0;
    w->qn = 0;
    w->burst = 0;
    w->last = 0.0f;
    w->pushed = w->starved = w->trimmed = 0;
}

/* How many levels the scroll wants in hand before it (re)starts. */
static inline i32 vhud_wave_need(const VhudWave *w) {
    i32 n = w->burst + 1;
    if (n < VHUD_PRIME) n = VHUD_PRIME;
    if (n > VHUD_TRIM - 2) n = VHUD_TRIM - 2;
    return n;
}

/* One buffer just brought `n` levels (after they were queued). */
static inline void vhud_wave_arrived(VhudWave *w, i32 n) {
    if (n > w->burst) w->burst = n;
}

/* A level was heard (already scaled: vhud_bar_level of one bar's peak). */
static inline void vhud_wave_queue(VhudWave *w, f32 level) {
    if (w->qn == VHUD_QUEUE) {
        for (i32 i = 1; i < VHUD_QUEUE; i++) w->q[i - 1] = w->q[i];
        w->qn--;
        w->trimmed++;
    }
    w->q[w->qn++] = vhud_clamp01(level);
}

static inline f32 vhud_wave_next_(VhudWave *w) {
    if (w->qn > VHUD_TRIM) {
        const i32 drop = w->qn - vhud_wave_need(w);
        for (i32 i = drop; i < w->qn; i++) w->q[i - drop] = w->q[i];
        w->qn -= drop;
        w->trimmed += (u32)drop;
    }
    if (w->qn > 0) {
        w->last = w->q[0];
        for (i32 i = 1; i < w->qn; i++) w->q[i - 1] = w->q[i];
        w->qn--;
    } else {
        w->last *= 0.6f;
        w->starved++;
    }
    return w->last;
}

/* Advance the row to `now`: one bar enters per VHUD_WAVE_DT, whatever the
 * frame rate and however the levels arrived. */
static inline void vhud_wave_play(VhudWave *w, f64 now) {
    if (w->last_push == 0.0) {
        if (w->qn < vhud_wave_need(w)) return;
        w->last_push = now;
        w->bars[VHUD_WAVE_BARS] = vhud_wave_next_(w);
        w->pushed++;
        return;
    }
    i32 steps = 0;
    while (now - w->last_push >= VHUD_WAVE_DT) {
        w->last_push += VHUD_WAVE_DT;
        for (i32 i = 0; i < VHUD_WAVE_BARS; i++) w->bars[i] = w->bars[i + 1];
        w->bars[VHUD_WAVE_BARS] = vhud_wave_next_(w);
        w->pushed++;
        /* Longer than the whole row: nothing of the old picture is left to
         * scroll, so stop counting intervals. */
        if (++steps > VHUD_WAVE_BARS + 1) break;
    }
    /* If an update misses at least VHUD_STALL intervals, clear last_push.
     * The next update waits for enough buffered samples and establishes a
     * new timing baseline instead of processing a backlog of stale
     * intervals. */
    if (steps >= VHUD_STALL) w->last_push = 0.0;
}

/* Hold the row exactly where it is (the recording has ended and is being read
 * back): the fraction it had is the fraction it keeps. Pinning the clock to
 * `now` instead would snap the whole row right by up to a bar's pitch on the
 * frame the recording stopped. */
static inline void vhud_wave_hold(VhudWave *w, f64 now, f32 frac) {
    if (w->last_push != 0.0) w->last_push = now - (f64)frac * VHUD_WAVE_DT;
}

/* How far through the current interval the clock is, for the sub-slot
 * shift. */
static inline f32 vhud_wave_frac(const VhudWave *w, f64 now) {
    if (w->last_push == 0.0) return 0.0f;
    f32 f = (f32)((now - w->last_push) / VHUD_WAVE_DT);
    if (f < 0.0f) return 0.0f;
    return f > 1.0f ? 1.0f : f;
}

#endif /* UI_VOICE_HUD_MATH_H */
