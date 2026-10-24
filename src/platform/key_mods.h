/*
 * Normalize modifier state to the state after a key event. X11 reports the
 * state from before the event; the application uses post-event state so
 * modifier presses and releases form consistent chords across backends.
 */
#ifndef PLATFORM_KEY_MODS_H
#define PLATFORM_KEY_MODS_H

#include "platform/platform.h"

static inline u32 key_mods_after(u32 before, u32 key, bool pressed) {
    u32 bit;
    switch (key) {
    case KEY_LSHIFT: case KEY_RSHIFT: bit = MOD_SHIFT; break;
    case KEY_LCTRL:  case KEY_RCTRL:  bit = MOD_CTRL;  break;
    case KEY_LALT:   case KEY_RALT:   bit = MOD_ALT;   break;
    case KEY_LSUPER: case KEY_RSUPER: bit = MOD_SUPER; break;
    default: return before;
    }
    return pressed ? (before | bit) : (before & ~bit);
}

#endif /* PLATFORM_KEY_MODS_H */
