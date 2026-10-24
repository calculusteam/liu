#include "intro/intro_tour.h"
#include "intro/intro_timeline.h"

#include <string.h>

/* Each feature runs in its own timed slot. Show its caption before the actions
 * begin, keep each input visible, and display the result before the next
 * caption. Space the transitions so each feature can be followed. */
static const IntroTourStep g_script[] = {
    /* Split panes: side by side, then the new pane top and bottom. */
    { INTRO_FEATURE_SPLIT,   0.85, INTRO_TOUR_KEY, 'D', INTRO_TOUR_CMD, NULL, 0, INTRO_CUE_SPLIT, 0 },
    { INTRO_FEATURE_SPLIT,   1.85, INTRO_TOUR_KEY, 'D', INTRO_TOUR_CMD | INTRO_TOUR_SHIFT, NULL, 0, INTRO_CUE_SPLIT, 1 },

    /* Tabs: two more — tik, tik, each a step higher — then back to the first,
     * the one that is split. */
    { INTRO_FEATURE_TABS,    0.85, INTRO_TOUR_KEY, 'T', INTRO_TOUR_CMD, NULL, 0, INTRO_CUE_TAB, 0 },
    { INTRO_FEATURE_TABS,    1.65, INTRO_TOUR_KEY, 'T', INTRO_TOUR_CMD, NULL, 0, INTRO_CUE_TAB, 1 },
    { INTRO_FEATURE_TABS,    2.45, INTRO_TOUR_KEY, '1', INTRO_TOUR_CMD, NULL, 0, INTRO_CUE_TAB_SWITCH, 0 },

    /* Tab groups: the two tabs just opened become a named, coloured group;
     * the group folds away into its chip, and comes back. */
    { INTRO_FEATURE_GROUPS,  0.85, INTRO_TOUR_GROUP, 0, 0, NULL, INTRO_TOUR_GROUP_MAKE, INTRO_CUE_GROUP, 0 },
    { INTRO_FEATURE_GROUPS,  1.85, INTRO_TOUR_GROUP, 0, 0, NULL, INTRO_TOUR_GROUP_FOLD, INTRO_CUE_FOLD, 0 },
    { INTRO_FEATURE_GROUPS,  2.80, INTRO_TOUR_GROUP, 0, 0, NULL, INTRO_TOUR_GROUP_UNFOLD, INTRO_CUE_UNFOLD, 0 },

    /* Command palette: open it, search for something it was just seen doing —
     * a key click for every character typed. */
    { INTRO_FEATURE_PALETTE, 0.80, INTRO_TOUR_KEY, 'K', INTRO_TOUR_CMD, NULL, 0, INTRO_CUE_PALETTE, 0 },
    { INTRO_FEATURE_PALETTE, 1.30, INTRO_TOUR_TEXT, 0, 0, "split", 0, INTRO_CUE_KEY, 0 },
    { INTRO_FEATURE_PALETTE, 2.62, INTRO_TOUR_DISMISS, 0, 0, NULL, 0, INTRO_CUE_DISMISS, 0 },

    /* Agent History: the palette's own command for it, typed and entered. The
     * whole name, so the top match cannot be anything else. Entered as early
     * as the typing allows: the list is scanned off the disk when the mode is
     * entered, and a store with years of sessions takes a noticeable part of
     * a second to scan. Two dismissals: if the first Esc only steps back out
     * of history mode, the second closes the palette; if the first closed it,
     * the second does nothing — and is silent either way, so that leaving is
     * heard once. */
    { INTRO_FEATURE_HISTORY, 0.75, INTRO_TOUR_KEY, 'K', INTRO_TOUR_CMD, NULL, 0, INTRO_CUE_PALETTE, 0 },
    { INTRO_FEATURE_HISTORY, 0.95, INTRO_TOUR_TEXT, 0, 0, "agent history", 0, INTRO_CUE_KEY, 0 },
    { INTRO_FEATURE_HISTORY, 1.72, INTRO_TOUR_KEY, INTRO_TOUR_KEY_ENTER, 0, NULL, 0, INTRO_CUE_ENTER, 0 },
    { INTRO_FEATURE_HISTORY, 3.00, INTRO_TOUR_DISMISS, 0, 0, NULL, 0, INTRO_CUE_DISMISS, 0 },
    { INTRO_FEATURE_HISTORY, 3.10, INTRO_TOUR_DISMISS, 0, 0, NULL, 0, INTRO_CUE_NONE, 0 },

    /* File browser: the rail out, and back. */
    { INTRO_FEATURE_FILES,   0.85, INTRO_TOUR_KEY, 'B', INTRO_TOUR_CMD, NULL, 0, INTRO_CUE_RAIL_OPEN, 0 },
    { INTRO_FEATURE_FILES,   2.40, INTRO_TOUR_KEY, 'B', INTRO_TOUR_CMD, NULL, 0, INTRO_CUE_RAIL_CLOSE, 0 },

    /* Voice: the card, listening. */
    { INTRO_FEATURE_VOICE,   0.80, INTRO_TOUR_VOICE, 0, 0, NULL, 1, INTRO_CUE_VOICE_ON, 0 },
    { INTRO_FEATURE_VOICE,   2.40, INTRO_TOUR_VOICE, 0, 0, NULL, 0, INTRO_CUE_VOICE_OFF, 0 },

    /* Themes: three of the built-in ones, a second each, and home. Chosen to
     * be UNMISTAKABLE against a near-black terminal — a blue-grey, a teal and
     * the light theme — because most of the nineteen are near-black themselves
     * and could not be told from the user's own at a glance. */
    { INTRO_FEATURE_THEMES,  0.85, INTRO_TOUR_THEME, 0, 0, "Nord", 0, INTRO_CUE_THEME, 0 },
    { INTRO_FEATURE_THEMES,  1.85, INTRO_TOUR_THEME, 0, 0, "Solarized Dark", 0, INTRO_CUE_THEME, 1 },
    { INTRO_FEATURE_THEMES,  2.85, INTRO_TOUR_THEME, 0, 0, "Liu Light", 0, INTRO_CUE_THEME, 2 },
    { INTRO_FEATURE_THEMES,  3.85, INTRO_TOUR_THEME, 0, 0, NULL, 0, INTRO_CUE_THEME, 3 },
};

const IntroTourStep *intro_tour_script(usize *count) {
    if (count) *count = sizeof g_script / sizeof g_script[0];
    return g_script;
}

f64 intro_tour_step_time(const IntroTourStep *step, usize index) {
    return intro_feature_begin(step->feature) + step->at
         + (step->kind == INTRO_TOUR_TEXT ? (f64)index * INTRO_TOUR_TYPE_GAP : 0.0);
}

bool intro_tour_finished(const IntroTourCursor *cursor) {
    return cursor->step >= sizeof g_script / sizeof g_script[0];
}

bool intro_tour_next(IntroTourCursor *cursor, f64 t, IntroTourEvent *out) {
    while (!intro_tour_finished(cursor)) {
        const IntroTourStep *step = &g_script[cursor->step];
        if (step->kind == INTRO_TOUR_TEXT && (!step->text || !step->text[cursor->ch])) {
            cursor->step++;                 /* typed out (or nothing to type) */
            cursor->ch = 0;
            continue;
        }
        if (t < intro_tour_step_time(step, cursor->ch)) return false;

        memset(out, 0, sizeof *out);
        out->kind = step->kind;
        out->feature = step->feature;
        out->key = step->key;
        out->mods = step->mods;
        out->text = step->text;
        out->arg = step->arg;
        out->cue = step->cue;
        out->cue_variant = step->cue_variant;
        if (step->kind == INTRO_TOUR_TEXT) {
            /* The script's typed text is ASCII: one byte, one character — and
             * no two keys in a row sound alike. */
            out->cue_variant = (i32)cursor->ch;
            out->codepoint = (u8)step->text[cursor->ch++];
        } else {
            cursor->step++;
            cursor->ch = 0;
        }
        return true;
    }
    return false;
}
