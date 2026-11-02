/*
 * Liu — when a window's input method takes part (X11)
 *
 * A window has ONE input context. Most of the time every key goes through it
 * first (XFilterEvent) and the text of a key is looked up through it. While a
 * text field that is ASCII by construction has the keyboard (an API key, a URL,
 * a model id, a host — ui/settings_field.h) the input method must not take
 * part: it would compose in a field that cannot take what it commits, and open
 * a candidate list over it. The app says so through platform_ime_allow(); this
 * is the platform's side of it, as plain decisions:
 *
 *   - the context loses the input focus, so the input method drops what it
 *     was composing and shows nothing; a window that gets the focus while the
 *     field is there does not hand it back; when the field is left the
 *     context has it again;
 *   - a real key (keycode != 0) is not filtered and its text is plain Xlib
 *     text — an input method that kept forwarding on an unfocused context
 *     could not swallow it, and it does not matter which one;
 *   - what an input method commits (a key event of keycode 0) is still
 *     filtered and looked up through the context, so an ended composition's
 *     commit still arrives through it.
 *
 * Pure: the decisions need no display.
 */
#ifndef LIU_PLATFORM_X11_IME_GATE_H
#define LIU_PLATFORM_X11_IME_GATE_H

#include "core/types.h"

typedef struct {
    bool blocked;   /* the app asked for the input method to stand down */
} X11ImeGate;

typedef enum {
    X11_IME_NO_CALL = 0,    /* nothing to tell the input method */
    X11_IME_SET_FOCUS,      /* XSetICFocus */
    X11_IME_UNSET_FOCUS,    /* XUnsetICFocus */
} X11ImeCall;

/* platform_ime_allow(allowed): what, if anything, to call on the context.
 * Only a change costs a round trip; a window without the focus applies it when
 * it gets the focus (x11_ime_focus_in_sets). */
static inline X11ImeCall x11_ime_allow(X11ImeGate *g, bool has_ic, bool allowed, bool window_focused) {
    if (!has_ic || g->blocked == !allowed) return X11_IME_NO_CALL;
    g->blocked = !allowed;
    if (!window_focused) return X11_IME_NO_CALL;
    return allowed ? X11_IME_SET_FOCUS : X11_IME_UNSET_FOCUS;
}

/* The window got the input focus: does the context get it too? */
static inline bool x11_ime_focus_in_sets(const X11ImeGate *g) { return !g->blocked; }

/* Does this key event go through XFilterEvent? `real_key`: keycode != 0 (an
 * input method's own commit has keycode 0). */
static inline bool x11_ime_filters_key(const X11ImeGate *g, bool real_key) {
    return !(g->blocked && real_key);
}

/* Is the text of this key event looked up through the input context
 * (Xutf8LookupString), or as plain Xlib text (XLookupString)? */
static inline bool x11_ime_looks_up_in_context(const X11ImeGate *g, bool has_ic, bool real_key) {
    return has_ic && !(g->blocked && real_key);
}

#endif /* LIU_PLATFORM_X11_IME_GATE_H */
