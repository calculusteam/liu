#ifndef LIU_INTRO_TIMELINE_H
#define LIU_INTRO_TIMELINE_H

/* The first-run intro sequence is deterministic from t, allowing frames to be
 * rendered at any point in the timeline. This file defines the sequence's
 * captions, timing and window placement; the renderer draws it elsewhere.
 *
 * Sequence: cursor, prompt, mark reveal, lock, cursor, terminal window, feature
 * tour, restore the window and reveal the desktop. A key or click cancels the
 * sequence. The tour is driven by intro_tour.h through the main event loop. */

#include "core/types.h"

typedef struct {
    /* The desktop going dark, 0..1. The dark spreads outward from the middle
     * of the screen; at 1 it has reached the corners. */
    f32 dim;
    /* 1 = the dark arrives and leaves as a wave, 0 = it fades (reduce motion). */
    f32 wipes;
    /* The `_`: 0..1, its blink folded in. */
    f32 cursor;
    /* The `>`: 0..1. */
    f32 prompt;
    /* Scale of the mark about the stage centre. 1 = lockup size; it opens
     * magnified, with only the face in view. */
    f32 zoom;
    /* 0 = the stage is centred on the face, 1 = on the whole mark. */
    f32 camera;
    /* The body filling in outward from the face, 0..1. */
    f32 flood;
    /* Light around the mark. */
    f32 glow;
    /* The field behind everything — the dithered background. */
    f32 haze;
    /* Ring of light as the mark locks: 0..1, negative when absent. */
    f32 pulse;
    /* Opacity of the mark as a whole (reduce motion fades it; the full intro
     * leaves this at 1 and uses flood / dissolve). */
    f32 mark_alpha;
    /* The `_` lighting up again before it leaves the mark, 0..1. */
    f32 spark;
    /* The mark draining back into the `_`, 0..1. */
    f32 dissolve;
    /* 0 = the `_`, 1 = the target window's frame. */
    f32 morph;
    f32 frame_alpha;
    /* How far the target window has come in, 0..1: the overlay drives the real
     * window's alpha from this. */
    f32 reveal;
    /* How far the overlay has cleared from over the window, 0..1. It trails
     * `reveal`: the window reaches full opacity underneath the frame's own
     * obsidian, and only then is it uncovered — so the desktop is never seen
     * through a half-faded window. A skip before the frame has landed has no
     * cover to hide behind and cross-fades instead. */
    f32 uncover;
    /* 0 = the window shows through the frame as it grows (the handoff),
     * 1 = through its whole rectangle (reduce motion, a skip). */
    f32 whole_window;
    /* 1 while the intro plays, 0 when it is over. What is drawn fades with it;
     * the dark lifts as a wave outward from the window. */
    f32 out;

    /* ---- the tour's caption, drawn over the foot of the real window ---- */
    /* The plate the captions sit on. */
    f32 plate;
    /* Which feature (0..INTRO_FEATURE_COUNT-1), its caption's opacity, and how
     * far it has risen into place. */
    f32 feature, caption, caption_rise;
    /* "press any key to skip", in the reader's language. */
    f32 hint;
} IntroFrame;

#define INTRO_SKIP_DURATION     0.35

/* The beats the sound is scored to. Change one here and intro_sound.c follows. */
#define INTRO_T_CURSOR   0.28   /* the `_` appears                      */
#define INTRO_T_PROMPT   1.30   /* the `>` arrives                      */
#define INTRO_T_PULL     1.66   /* the camera starts pulling back       */
#define INTRO_T_LOCK     2.50   /* the mark is whole                    */
#define INTRO_T_FLY      4.12   /* the `_` leaves for the window        */
#define INTRO_T_LAND     4.98   /* the frame is on the window           */

/* INTRO_FEATURE_COUNT must match INTRO_CAPTION_COUNT and the feature
 * sequences in intro_tour.c and intro_sound.c. */
#define INTRO_FEATURE_COUNT     8
#define INTRO_FEATURE_SPLIT     0
#define INTRO_FEATURE_TABS      1
#define INTRO_FEATURE_GROUPS    2
#define INTRO_FEATURE_PALETTE   3
#define INTRO_FEATURE_HISTORY   4
#define INTRO_FEATURE_FILES     5
#define INTRO_FEATURE_VOICE     6
#define INTRO_FEATURE_THEMES    7

/* Feature duration in seconds. Show each caption before its action, keep
 * presses visible for about one second, then leave the result on screen
 * before the next caption. The theme feature takes longest because it
 * previews three themes; Agent History allows time for a disk scan. */
#define INTRO_SECONDS_SPLIT     3.00
#define INTRO_SECONDS_TABS      3.20
#define INTRO_SECONDS_GROUPS    3.90
#define INTRO_SECONDS_PALETTE   3.00
#define INTRO_SECONDS_HISTORY   3.40
#define INTRO_SECONDS_FILES     2.80
#define INTRO_SECONDS_VOICE     2.80
#define INTRO_SECONDS_THEMES    4.60
/* Where each begins, in seconds into the tour. Macros, because the sound's
 * score is a static table. */
#define INTRO_BEGIN_SPLIT       0.0
#define INTRO_BEGIN_TABS        (INTRO_BEGIN_SPLIT + INTRO_SECONDS_SPLIT)
#define INTRO_BEGIN_GROUPS      (INTRO_BEGIN_TABS + INTRO_SECONDS_TABS)
#define INTRO_BEGIN_PALETTE     (INTRO_BEGIN_GROUPS + INTRO_SECONDS_GROUPS)
#define INTRO_BEGIN_HISTORY     (INTRO_BEGIN_PALETTE + INTRO_SECONDS_PALETTE)
#define INTRO_BEGIN_FILES       (INTRO_BEGIN_HISTORY + INTRO_SECONDS_HISTORY)
#define INTRO_BEGIN_VOICE       (INTRO_BEGIN_FILES + INTRO_SECONDS_FILES)
#define INTRO_BEGIN_THEMES      (INTRO_BEGIN_VOICE + INTRO_SECONDS_VOICE)
#define INTRO_SHOW_SECONDS      (INTRO_BEGIN_THEMES + INTRO_SECONDS_THEMES)
/* The same, by index; out-of-range features clamp to the first / last. */
f64 intro_feature_begin(i32 feature);
f64 intro_feature_seconds(i32 feature);
/* Which feature's slot `t` seconds into the tour falls in. */
i32 intro_feature_at(f64 t);
/* A caption takes this long to come up, and starts to leave this long before
 * its slot ends. The tour's script keeps its presses between the two. */
#define INTRO_CAPTION_IN        0.42
#define INTRO_CAPTION_OUT       0.40
/* How long before the tour's end the window is put back as it was given: while
 * the last caption is leaving, so the dark lifts on the user's window and not
 * on the tour's leftovers. */
#define INTRO_PUT_BACK_LEAD     0.24

/* The viewer dismissed the tour (a key, a click) while the picture was `held`:
 * `ending` is the ending's own frame `seconds` into it (intro_frame_at from
 * INTRO_T_LIFT on), and this is the frame to draw — the ending, with the
 * caption that was up fading out of it over INTRO_DISMISS_FADE rather than
 * vanishing. (The ending's timeline has no caption of its own: by the time the
 * tour reaches it by itself, the last caption has left.) */
#define INTRO_DISMISS_FADE      0.30
IntroFrame intro_frame_dismissed(IntroFrame held, IntroFrame ending, f64 seconds);
/* The window is uncovered by INTRO_T_LAND + 0.44; the tour starts just after. */
#define INTRO_T_SHOW            (INTRO_T_LAND + 0.60)
#define INTRO_T_SHOW_END        (INTRO_T_SHOW + INTRO_SHOW_SECONDS)
/* The dark starts to lift. */
#define INTRO_T_LIFT            (INTRO_T_SHOW_END + 0.10)
/* Seconds, at speed 1. */
#define INTRO_DURATION          (INTRO_T_LIFT + 0.95)

/* Reduce motion: the mark fades in, cross-fades to the window in its frame,
 * the tour runs (the app's own motion is the app's), and the dark fades. */
#define INTRO_T_REDUCED_MARK    0.32
#define INTRO_T_REDUCED_HAND    2.05
#define INTRO_T_REDUCED_SHOW    3.00
#define INTRO_T_REDUCED_SHOW_END (INTRO_T_REDUCED_SHOW + INTRO_SHOW_SECONDS)
#define INTRO_T_REDUCED_LIFT    (INTRO_T_REDUCED_SHOW_END + 0.10)
#define INTRO_REDUCED_DURATION  (INTRO_T_REDUCED_LIFT + 0.70)

/* How much larger than its lockup size the face is when the intro opens. */
#define INTRO_OPEN_ZOOM  2.6f

IntroFrame intro_frame_at(f64 t);
IntroFrame intro_frame_reduced_at(f64 t);
/* A skip in progress (0..1): whatever is on screen fades while the window
 * comes in. Never jumps the choreography. */
IntroFrame intro_frame_skipped(IntroFrame f, f64 progress);

/* ---- easing ------------------------------------------------------------ */

f64 intro_ease_smooth(f64 x);
f64 intro_ease_out_cubic(f64 x);
f64 intro_ease_in_out_cubic(f64 x);
f64 intro_ease_in_out_quart(f64 x);
/* Unit step response of a damped spring: 0 before t = 0, settling on 1.
 * `response` is the undamped period in seconds; damping < 1 overshoots. */
f64 intro_ease_spring(f64 t, f64 response, f64 damping);

/* ---- layout ------------------------------------------------------------ */

/* Where things sit on the overlay, in points with y growing downward (the
 * shader's convention, and the artwork's). */
typedef struct {
    f32 view_w, view_h;
    /* Pixels per point. */
    f32 scale;
    /* Where the centre of the mark lands: the middle of the screen. */
    f32 center_x, center_y;
    /* Points per pixel of the bunny file at lockup size. */
    f32 unit;
    /* The window the intro hands off to: centre, half-size, corner radius. */
    f32 target_cx, target_cy, target_hw, target_hh, target_radius;
    /* Scales how dark the desktop goes behind the intro. */
    f32 dim_strength;
} IntroLayout;

/* The mark's height in points for a screen `view_h` points tall. */
f32 intro_mark_height(f32 view_h);

/* `target_*` is the window's rectangle in overlay points (x, y of its top-left
 * corner, y down). The mark and its name are centred on the screen as a group,
 * wherever the window is. */
IntroLayout intro_layout_make(f32 view_w, f32 view_h, f32 scale, f32 mark_height,
                              f32 target_x, f32 target_y, f32 target_w, f32 target_h,
                              f32 target_radius);
void intro_layout_set_target(IntroLayout *l, f32 x, f32 y, f32 w, f32 h);

#endif /* LIU_INTRO_TIMELINE_H */
