#ifndef LIU_INTRO_H
#define LIU_INTRO_H

/* First-run intro overlay for macOS with Metal. It runs asynchronously, ends on
 * key input or after the tour, and leaves the terminal window visible on
 * completion or failure. Captions use the system language when available and
 * English otherwise. */

#include "core/types.h"
#include "platform/platform.h"

/* Decides whether this launch opens with the intro. Must run before anything
 * creates Liu's config directory — "fresh install" is read off its absence. */
void liu_intro_sample_first_run(void);
bool liu_intro_wanted(void);

/* Plays the intro over `w`, which should have been created transparent
 * (PlatformWindowConfig.start_transparent) so that it never flashes ahead of
 * its own introduction. */
void liu_intro_begin(PlatformWindow *w);
bool liu_intro_active(void);

/* Records that the intro has been shown (a no-op when LIU_INTRO forced it). */
void liu_intro_mark_seen(void);

/* The tour: once the window is uncovered, main.c makes it show what it can do
 * (intro_tour.h). This is its clock — seconds since the tour began, negative
 * while the window is not up yet, and running on past INTRO_SHOW_SECONDS while
 * the dark lifts. False when no intro is playing, or it is being skipped or
 * has been dismissed: there is then no tour to run, and whatever it had
 * changed is to be put back. */
bool liu_intro_tour_clock(f64 *seconds);
/* True while the intro is on its way out — the tour ran out, or the viewer
 * ended it: the terminal is theirs again, and what they type is theirs too. */
bool liu_intro_leaving(void);

/* The intro is one flow and ends by itself; all a viewer can do is end it
 * sooner, with a key. While it plays the window is key, so the keyboard
 * arrives in main.c's event loop, which calls this for any key. (Not for a
 * click: the overlay swallows those, and nothing comes of them.) Before the
 * tour it fades the intro out and shows the window; in the tour it lifts the
 * dark. */
void liu_intro_end(void);

/* The tour is heard as well as seen: main.c calls this as each step happens —
 * and as each caption arrives — with an IntroCue (intro_sound.h) and its
 * variant, and the overlay's sound plays it at once. Nothing happens when the
 * intro is silent, skipped or over. */
void liu_intro_cue(i32 cue, i32 variant);

#endif /* LIU_INTRO_H */
