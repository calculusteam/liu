#ifndef LIU_INTRO_TOUR_H
#define LIU_INTRO_TOUR_H

/* Sequence the first-run tour through the application's UI. Use normal event
 * handling for shortcuts and direct actions for features without a safe
 * default key, such as microphone preview. */

#include "core/types.h"
#include "intro/intro_sound.h"

typedef enum {
    /* A key chord: `key` with `mods`. */
    INTRO_TOUR_KEY,
    /* Typed text: `text`, one character every INTRO_TOUR_TYPE_GAP seconds. As
     * an event it arrives one character at a time (`codepoint`). */
    INTRO_TOUR_TEXT,
    /* Esc, only if the palette is still open. */
    INTRO_TOUR_DISMISS,
    /* The voice card's preview: `arg` 1 on, 0 off. */
    INTRO_TOUR_VOICE,
    /* A built-in theme by name (`text`); NULL: back to the user's own. */
    INTRO_TOUR_THEME,
    /* Tab groups, `arg` one of INTRO_TOUR_GROUP_*: put the tabs the tour
     * opened into a named, coloured group; fold it into its chip; unfold. */
    INTRO_TOUR_GROUP,
} IntroTourKind;

#define INTRO_TOUR_GROUP_MAKE    1
#define INTRO_TOUR_GROUP_FOLD    2
#define INTRO_TOUR_GROUP_UNFOLD  3

#define INTRO_TOUR_CMD    1u
#define INTRO_TOUR_SHIFT  2u

/* `key` is an ASCII letter or digit, or one of these. */
#define INTRO_TOUR_KEY_ENTER   0x100
#define INTRO_TOUR_KEY_ESCAPE  0x101

#define INTRO_TOUR_TYPE_GAP  0.055

typedef struct {
    /* Which feature's slot, and how many seconds into it. */
    i32 feature;
    f64 at;
    IntroTourKind kind;
    i32 key;
    u32 mods;
    const char *text;
    i32 arg;
    /* What it sounds like (intro_sound.h): played as the step happens. For
     * typed text it is each character's sound, and the variant goes round
     * with the character. INTRO_CUE_NONE: silent. */
    IntroCue cue;
    i32 cue_variant;
} IntroTourStep;

const IntroTourStep *intro_tour_script(usize *count);

/* Seconds into the tour at which `step` fires (its `index`-th character, for
 * typed text). */
f64 intro_tour_step_time(const IntroTourStep *step, usize index);

/* One thing for the app to do. */
typedef struct {
    IntroTourKind kind;
    i32 feature;
    i32 key;
    u32 mods;
    u32 codepoint;
    const char *text;
    i32 arg;
    IntroCue cue;
    i32 cue_variant;
} IntroTourEvent;

/* Where a run of the tour has got to. Zero-initialise. */
typedef struct {
    usize step;
    usize ch;
} IntroTourCursor;

/* The next event due by `t` seconds into the tour; false when none is due yet
 * or the script is finished. Call it again until it says false. */
bool intro_tour_next(IntroTourCursor *cursor, f64 t, IntroTourEvent *out);
bool intro_tour_finished(const IntroTourCursor *cursor);

#endif /* LIU_INTRO_TOUR_H */
