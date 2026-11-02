/*
 * Liu - which window's input context holds the input method's focus (X11)
 *
 * An input method keeps ONE focused context per client. What it does with a
 * context that loses the focus while something is being composed differs:
 * ibus drops the draft, fcitx5 in its on-the-spot style COMMITS it - the raw
 * "ni hao" of a pinyin draft - to the context's window, where Liu typed it into
 * the pane it was composed in. The context loses the focus in two ways: the
 * window's own FocusOut (XUnsetICFocus), and another window's context taking
 * the focus (XSetICFocus of a new window, which fcitx5 answers by unfocusing
 * the old one itself). In both Liu first resets the context - the input
 * method's own "end the composition": a pinyin draft is dropped, a Hangul
 * syllable is committed as the character it is and typed into the pane, as
 * ibus-hangul does on its own at the loss - and only then lets go of the focus
 * (platform_linux.c, linux_ic_release).
 *
 * This is the bookkeeping that tells those two moments apart from the ones
 * that need no reset: who holds the focus, who must be released when another
 * window takes it, and whether a FocusOut is the holder's. Windows are
 * identities, compared and never dereferenced. Pure: no display is needed.
 */
#ifndef LIU_PLATFORM_XIM_FOCUS_H
#define LIU_PLATFORM_XIM_FOCUS_H

#include "core/types.h"

typedef struct {
    const void *holder;   /* the window whose context the input method has focused, or NULL */
} XicFocus;

/* `w`'s context takes the focus. Returns the window whose context held it
 * until now and is to be released first, or NULL when nobody else did. */
static inline const void *xic_focus_take(XicFocus *f, const void *w) {
    const void *prev = f->holder;
    f->holder = w;
    return (prev && prev != w) ? prev : NULL;
}

/* `w`'s window lost the keyboard (FocusOut) or is going away. True when its
 * context held the focus, so it is to be released; false when it did not -
 * another window's context took the focus first and released it then. */
static inline bool xic_focus_drop(XicFocus *f, const void *w) {
    if (!w || f->holder != w) return false;
    f->holder = NULL;
    return true;
}

#endif /* LIU_PLATFORM_XIM_FOCUS_H */
