#ifndef LIU_INTRO_SOUND_H
#define LIU_INTRO_SOUND_H

/* Procedural sound cues for the first-run intro. The opening motif uses B and
 * E; the cursor, prompt, dither, lock, and UI actions map to short
 * synthesized sounds. The mixdown schedules the opening, ambience, and event
 * cues for the tour. */

#include "core/types.h"

#define INTRO_SOUND_RATE 48000

typedef enum {
    INTRO_CUE_NONE = 0,
    INTRO_CUE_FEATURE,      /* a caption arrives; variant: which feature */
    INTRO_CUE_SPLIT,        /* a pane splits; variant 0 side by side, 1 stacked */
    INTRO_CUE_TAB,          /* a tab opens; variant: the first, second, third */
    INTRO_CUE_TAB_SWITCH,   /* back to a tab */
    INTRO_CUE_GROUP,        /* tabs gather into a group */
    INTRO_CUE_FOLD,         /* the group folds into its chip */
    INTRO_CUE_UNFOLD,
    INTRO_CUE_PALETTE,      /* the palette opens */
    INTRO_CUE_KEY,          /* one typed character; the variant varies the click */
    INTRO_CUE_ENTER,
    INTRO_CUE_DISMISS,      /* the palette closes */
    INTRO_CUE_RAIL_OPEN,    /* the file rail slides out */
    INTRO_CUE_RAIL_CLOSE,
    INTRO_CUE_VOICE_ON,     /* the voice card: listening */
    INTRO_CUE_VOICE_OFF,
    INTRO_CUE_THEME,        /* a theme; variant 0..2 the ones shown, 3 home */
    INTRO_CUE_CLOSE,        /* the intro ends: the signature again */
    INTRO_CUE_COUNT
} IntroCue;

#define INTRO_CUE_MAX_VARIANTS 8

/* How many variants a cue has (at least 1), and its name. A variant past the
 * last wraps. */
i32 intro_cue_variants(IntroCue cue);
const char *intro_cue_name(IntroCue cue);
/* Whether a cue rings on for seconds (a note) rather than tens of
 * milliseconds (a tick). A player keeps the two on separate voices, so a run
 * of ticks never cuts a note's tail short. */
bool intro_cue_rings(IntroCue cue);

/* The bed comes up under the tour this long before it begins, over this long,
 * and goes over this long once the intro starts to end. A caption's note is
 * this long after its slot begins — as the caption can be seen, not before. */
#define INTRO_BED_LEAD            0.40
#define INTRO_BED_FADE_IN         1.20
#define INTRO_BED_FADE_OUT        0.90
#define INTRO_FEATURE_CUE_DELAY   0.12

/* Everything below is interleaved stereo in -1..1, `*out_frames` frames of it,
 * malloc'd; NULL when out of memory. */

/* From the first frame to the window, with its tail. */
f32 *intro_sound_opening(bool reduced, usize *out_frames);
/* One loop of the bed: it repeats without a seam. */
f32 *intro_sound_bed(usize *out_frames);
f32 *intro_sound_cue(IntroCue cue, i32 variant, usize *out_frames);

/* A whole undisturbed run, mixed down: the opening, the bed, every cue at the
 * moment the tour's script fires it, the close. */
f32 *intro_sound_mixdown(bool reduced, usize *out_frames);
f64 intro_sound_duration(bool reduced);

/* 16-bit PCM WAV of `count` interleaved samples (frames x 2). malloc'd. */
u8 *intro_sound_wav(const f32 *samples, usize count, usize *out_size);

#endif /* LIU_INTRO_SOUND_H */
