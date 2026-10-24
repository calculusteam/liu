#include "intro/intro_timeline.h"
#include "intro/intro_geom.h"

#include <math.h>

static f64 ramp(f64 t, f64 a, f64 b) {
    f64 x = (t - a) / (b - a);
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

static f64 mix(f64 a, f64 b, f64 t) { return a + (b - a) * t; }

f64 intro_ease_smooth(f64 x) { return x * x * (3.0 - 2.0 * x); }

f64 intro_ease_out_cubic(f64 x) { return 1.0 - pow(1.0 - x, 3.0); }

f64 intro_ease_in_out_cubic(f64 x) {
    return x < 0.5 ? 4.0 * x * x * x : 1.0 - pow(-2.0 * x + 2.0, 3.0) / 2.0;
}

f64 intro_ease_in_out_quart(f64 x) {
    return x < 0.5 ? 8.0 * x * x * x * x : 1.0 - pow(-2.0 * x + 2.0, 4.0) / 2.0;
}

f64 intro_ease_spring(f64 t, f64 response, f64 damping) {
    if (t <= 0.0) return 0.0;
    f64 omega = 2.0 * M_PI / response;
    if (damping >= 1.0) return 1.0 - exp(-omega * t) * (1.0 + omega * t);
    f64 wd = omega * sqrt(1.0 - damping * damping);
    f64 envelope = exp(-damping * omega * t);
    return 1.0 - envelope * (cos(wd * t) + (damping * omega / wd) * sin(wd * t));
}

static IntroFrame frame_rest(void) {
    IntroFrame f = {0};
    f.wipes = 1.0f;
    f.zoom = INTRO_OPEN_ZOOM;
    f.pulse = -1.0f;
    f.mark_alpha = 1.0f;
    f.out = 1.0f;
    return f;
}

static const f64 g_feature_begin[INTRO_FEATURE_COUNT] = {
    INTRO_BEGIN_SPLIT, INTRO_BEGIN_TABS, INTRO_BEGIN_GROUPS, INTRO_BEGIN_PALETTE,
    INTRO_BEGIN_HISTORY, INTRO_BEGIN_FILES, INTRO_BEGIN_VOICE, INTRO_BEGIN_THEMES,
};
static const f64 g_feature_seconds[INTRO_FEATURE_COUNT] = {
    INTRO_SECONDS_SPLIT, INTRO_SECONDS_TABS, INTRO_SECONDS_GROUPS, INTRO_SECONDS_PALETTE,
    INTRO_SECONDS_HISTORY, INTRO_SECONDS_FILES, INTRO_SECONDS_VOICE, INTRO_SECONDS_THEMES,
};

static i32 clamp_feature(i32 feature) {
    return feature < 0 ? 0 : (feature > INTRO_FEATURE_COUNT - 1 ? INTRO_FEATURE_COUNT - 1 : feature);
}

f64 intro_feature_begin(i32 feature) { return g_feature_begin[clamp_feature(feature)]; }
f64 intro_feature_seconds(i32 feature) { return g_feature_seconds[clamp_feature(feature)]; }

i32 intro_feature_at(f64 t) {
    i32 k = 0;
    while (k < INTRO_FEATURE_COUNT - 1 && t >= g_feature_begin[k + 1]) k++;
    return k;
}

IntroFrame intro_frame_dismissed(IntroFrame held, IntroFrame ending, f64 seconds) {
    f32 keep = (f32)(1.0 - intro_ease_smooth(ramp(seconds, 0.0, INTRO_DISMISS_FADE)));
    ending.feature = held.feature;
    ending.caption = held.caption * keep;
    ending.caption_rise = held.caption_rise;
    ending.plate = held.plate * keep;
    ending.hint = held.hint * keep;
    return ending;
}

/* The tour's caption, which the full intro and the Reduce Motion one both
 * run on the same schedule from `start`: a plate at the foot of the window, a
 * caption per feature on it — in, read, out, one at a time — and the hint. The
 * features themselves are the app's to show; nothing of them is drawn here. */
static void captions(IntroFrame *f, f64 t, f64 start) {
    f64 end = start + INTRO_SHOW_SECONDS;

    f->plate = (f32)(intro_ease_smooth(ramp(t, start - 0.14, start + 0.20))
                     * (1.0 - intro_ease_smooth(ramp(t, end - 0.26, end + 0.04))));
    f->hint = (f32)(intro_ease_smooth(ramp(t, 1.60, 2.20))
                    * (1.0 - intro_ease_smooth(ramp(t, end - 0.20, end + 0.10))));

    /* Show one caption at a time before its feature begins, then clear it
     * before the next feature starts. */
    i32 k = intro_feature_at(t - start);
    f64 D = intro_feature_seconds(k);
    f64 local = t - (start + intro_feature_begin(k));
    f->feature = (f32)k;
    f->caption = (f32)(intro_ease_smooth(ramp(local, 0.00, INTRO_CAPTION_IN))
                       * (1.0 - intro_ease_smooth(ramp(local, D - INTRO_CAPTION_OUT, D - 0.06))));
    f->caption_rise = (f32)intro_ease_out_cubic(ramp(local, 0.00, INTRO_CAPTION_IN + 0.14));
}

IntroFrame intro_frame_at(f64 t) {
    IntroFrame f = frame_rest();
    f.dim = (f32)intro_ease_in_out_cubic(ramp(t, 0.00, 1.00));

    /* The cursor: on, one blink, on for good. A terminal's cursor steps rather
     * than fades, so the edges are a few frames long and no longer. */
    f64 on = intro_ease_smooth(ramp(t, INTRO_T_CURSOR, INTRO_T_CURSOR + 0.08));
    f64 blink = intro_ease_smooth(ramp(t, 0.80, 0.84))
              * (1.0 - intro_ease_smooth(ramp(t, 1.06, 1.10)));
    f.cursor = (f32)(on * (1.0 - blink));
    f.prompt = (f32)intro_ease_out_cubic(ramp(t, INTRO_T_PROMPT, INTRO_T_PROMPT + 0.16));

    /* The camera pulls back: what looked like a prompt is the mark's face, and
     * the body fills in around it. */
    /* Interpolated in the exponent: a pull-back at a constant RATE of scale
     * is what reads as one camera move; a linear one rushes at the start. */
    f.zoom = (f32)pow((f64)INTRO_OPEN_ZOOM,
                      1.0 - intro_ease_in_out_cubic(ramp(t, INTRO_T_PULL, INTRO_T_LOCK - 0.04)));
    f.camera = (f32)intro_ease_in_out_cubic(ramp(t, INTRO_T_PULL, INTRO_T_LOCK));
    f.flood = (f32)intro_ease_smooth(ramp(t, INTRO_T_PULL + 0.06, INTRO_T_LOCK - 0.04));

    f.glow = (f32)(intro_ease_smooth(ramp(t, INTRO_T_PULL + 0.06, INTRO_T_LOCK - 0.10))
                   * mix(1.0, 0.45, intro_ease_smooth(ramp(t, INTRO_T_LOCK, INTRO_T_LOCK + 1.0))));
    /* The dithered background: it comes up with the dark and then holds
     * still. */
    f.haze = (f32)intro_ease_smooth(ramp(t, 0.20, 1.40));
    f.pulse = t >= INTRO_T_LOCK ? (f32)ramp(t, INTRO_T_LOCK, INTRO_T_LOCK + 1.10) : -1.0f;

    /* Handoff: the mark drains back into its cursor, which lights up and grows
     * into the window's frame. */
    f.spark = (f32)intro_ease_smooth(ramp(t, 3.72, 3.96));
    f.dissolve = (f32)intro_ease_smooth(ramp(t, 3.84, 4.30));
    f.morph = (f32)intro_ease_in_out_quart(ramp(t, INTRO_T_FLY, INTRO_T_LAND));
    /* The frame stays on the window through the tour: a hairline of the accent
     * around the thing being shown. */
    f.frame_alpha = (f32)(intro_ease_smooth(ramp(t, INTRO_T_FLY, INTRO_T_FLY + 0.10))
                          * (1.0 - intro_ease_smooth(ramp(t, INTRO_T_LIFT, INTRO_T_LIFT + 0.45))));

    /* The window comes to full opacity under the frame's obsidian, unseen;
     * then the overlay clears from over it. From here on what is inside the
     * frame is the real window. */
    f.reveal = (f32)intro_ease_smooth(ramp(t, INTRO_T_LAND - 0.30, INTRO_T_LAND - 0.04));
    f.uncover = (f32)intro_ease_smooth(ramp(t, INTRO_T_LAND - 0.02, INTRO_T_LAND + 0.44));

    captions(&f, t, INTRO_T_SHOW);

    /* The dark has stayed up around the window all through the tour. Now it
     * lifts. */
    f.out = (f32)(1.0 - intro_ease_in_out_cubic(ramp(t, INTRO_T_LIFT, INTRO_DURATION - 0.04)));
    return f;
}

IntroFrame intro_frame_reduced_at(f64 t) {
    IntroFrame f = frame_rest();
    f64 fade_in = intro_ease_smooth(ramp(t, 0.15, 0.70));
    f64 fade_out = intro_ease_smooth(ramp(t, 1.85, 2.45));
    f.dim = (f32)intro_ease_smooth(ramp(t, 0.00, 0.50));
    f.wipes = 0.0f;
    f.cursor = 1.0f;
    f.prompt = 1.0f;
    f.zoom = 1.0f;
    f.camera = 1.0f;
    f.flood = 1.0f;
    f.glow = 0.45f;
    f.haze = (f32)fade_in;
    f.mark_alpha = (f32)(fade_in * (1.0 - fade_out));

    /* The frame does not fly here: it is simply there, on the window, once the
     * mark has gone — it fades in already landed, and the window inside it. */
    f.morph = t >= INTRO_T_REDUCED_HAND ? 1.0f : 0.0f;
    f.frame_alpha = (f32)(intro_ease_smooth(ramp(t, INTRO_T_REDUCED_HAND + 0.02,
                                                 INTRO_T_REDUCED_HAND + 0.40))
                          * (1.0 - intro_ease_smooth(ramp(t, INTRO_T_REDUCED_LIFT,
                                                          INTRO_T_REDUCED_LIFT + 0.40))));
    f.reveal = (f32)intro_ease_smooth(ramp(t, INTRO_T_REDUCED_HAND + 0.16,
                                           INTRO_T_REDUCED_HAND + 0.40));
    f.uncover = (f32)intro_ease_smooth(ramp(t, INTRO_T_REDUCED_HAND + 0.42,
                                            INTRO_T_REDUCED_HAND + 0.78));
    f.whole_window = 1.0f;

    captions(&f, t, INTRO_T_REDUCED_SHOW);

    f.out = (f32)(1.0 - intro_ease_smooth(ramp(t, INTRO_T_REDUCED_LIFT,
                                               INTRO_REDUCED_DURATION - 0.04)));
    return f;
}

IntroFrame intro_frame_skipped(IntroFrame f, f64 progress) {
    f64 x = progress < 0.0 ? 0.0 : (progress > 1.0 ? 1.0 : progress);
    IntroFrame g = f;

    /* The frame is down on the window and still covering it: there is
     * something to hide behind, so the window comes to full opacity under the
     * cover, and then the cover and the dark go together. A plain cross-fade
     * here showed the desktop through a half-faded window. */
    if (f.morph >= 1.0f && f.frame_alpha >= 0.999f && f.uncover <= 0.0f) {
        f32 in = (f32)intro_ease_smooth(ramp(x, 0.00, 0.40));
        f32 away = (f32)intro_ease_smooth(ramp(x, 0.40, 1.00));
        if (in > g.reveal) g.reveal = in;
        g.uncover = away;
        g.out = f.out * (1.0f - away);
        return g;
    }

    f32 k = (f32)intro_ease_smooth(x);
    if (k > g.reveal) g.reveal = k;
    if (k > g.uncover) g.uncover = k;
    /* Ease across, so a skip during the handoff does not pop. */
    f32 whole = k * 4.0f > 1.0f ? 1.0f : k * 4.0f;
    if (whole > g.whole_window) g.whole_window = whole;
    g.out = f.out * (1.0f - k);
    return g;
}

/* ---- layout ------------------------------------------------------------ */

f32 intro_mark_height(f32 view_h) {
    f32 h = view_h * 0.17f;
    return h < 132.0f ? 132.0f : (h > 196.0f ? 196.0f : h);
}

IntroLayout intro_layout_make(f32 view_w, f32 view_h, f32 scale, f32 mark_height,
                              f32 target_x, f32 target_y, f32 target_w, f32 target_h,
                              f32 target_radius) {
    IntroLayout l = {0};
    l.view_w = view_w;
    l.view_h = view_h;
    l.scale = scale > 0.0f ? scale : 1.0f;
    l.unit = mark_height / (INTRO_MARK_Y1 - INTRO_MARK_Y0);
    l.dim_strength = 1.0f;
    l.target_radius = target_radius;

    /* The mark stands alone in the middle of the screen. (The bunny's ink is
     * not centred in its own box — the ears lean right and the tail sticks
     * out — but centring the box is what the eye expects anyway.) */
    l.center_x = view_w * 0.5f;
    l.center_y = view_h * 0.5f;

    intro_layout_set_target(&l, target_x, target_y, target_w, target_h);
    return l;
}

void intro_layout_set_target(IntroLayout *l, f32 x, f32 y, f32 w, f32 h) {
    l->target_hw = w * 0.5f;
    l->target_hh = h * 0.5f;
    l->target_cx = x + l->target_hw;
    l->target_cy = y + l->target_hh;
}
