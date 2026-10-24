/*
 * Liu - the key a chord names on any keyboard layout (X11)
 *
 * Liu's chords (Ctrl+Shift+T, Alt+1, Ctrl+, …) and a pane's Ctrl+<letter>
 * are matched by the key the layout's keysym names (platform_linux.c,
 * map_keysym). On a layout whose keys type something else that name is
 * missing, and the chord would do nothing:
 *
 *   - Cyrillic, Greek, Arabic, Hebrew … : the letter keys type their own
 *     script, so Ctrl+Shift+T is Ctrl+Shift+Т, and Ctrl+C in a shell would
 *     send nothing at all;
 *   - French AZERTY: the top row types & é " ' ( - è _ ç à and the digits
 *     need Shift, so Alt+1 is Alt+&.
 *
 * Desktop applications match such a chord by the key's other symbols, and so
 * does Liu: XKB is asked what the SAME physical key carries — first on the
 * other shift level of the active layout (AZERTY's digits), then in the other
 * layouts the keymap holds (the Latin layout a Russian user switches to). No
 * layout is named anywhere: whatever the keymap says is what is used. A
 * keymap with no Latin layout at all has nothing to offer, and the chord
 * stays unmatched.
 *
 * Only the base and Shift levels are read: an AltGr symbol is typed with a
 * modifier that was not pressed. Pure C over the symbols the caller read from
 * XKB (XkbKeycodeToKeysym), so the order can be checked without an X server.
 */
#ifndef LIU_PLATFORM_X11_KEYPOS_H
#define LIU_PLATFORM_X11_KEYPOS_H

#include "core/types.h"

#define X11_KEYPOS_GROUPS 4   /* XKB holds at most four layouts */
#define X11_KEYPOS_LEVELS 2   /* base and Shift */

/* The symbols of one physical key: syms[group][level], 0 = none. */
typedef struct {
    u32 syms[X11_KEYPOS_GROUPS][X11_KEYPOS_LEVELS];
    i32 groups;          /* layouts the keymap holds for this key (1..4) */
} X11KeySyms;

/* The first of the key's symbols `known` maps to a key (non-zero): the active
 * layout's levels, then every other layout's in order. 0 when none does. */
static inline u32 x11_key_by_position(const X11KeySyms *k, i32 active,
                                      u32 (*known)(u32 keysym)) {
    i32 groups = k->groups;
    if (groups > X11_KEYPOS_GROUPS) groups = X11_KEYPOS_GROUPS;
    if (groups < 1) return 0;
    if (active < 0 || active >= groups) active = 0;
    for (i32 pass = 0; pass < groups; pass++) {
        i32 g = pass == 0 ? active : (pass <= active ? pass - 1 : pass);
        for (i32 l = 0; l < X11_KEYPOS_LEVELS; l++) {
            u32 s = k->syms[g][l];
            if (!s) continue;
            u32 key = known(s);
            if (key) return key;
        }
    }
    return 0;
}

#endif /* LIU_PLATFORM_X11_KEYPOS_H */
