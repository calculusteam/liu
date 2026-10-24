/*
 * Liu — where an input method's composition belongs (X11)
 *
 * A window has ONE input context, and an input method keeps what is being
 * composed inside it. If the keyboard moves to another place in the window —
 * another tab or pane, a field opened or closed — in the middle of a
 * composition, the composition would go along: drawn at the new place and
 * committed there, so a word composed in one tab would land in the next one.
 *
 * Here a composition ends where it was made. Whenever the keyboard's target
 * changes, the window's input context is reset (platform_ime_reset), which ends
 * any composition in it; what the input method commits for that — a Korean
 * syllable is committed, a pinyin draft just goes — is the old target's: it is
 * typed into the pane the composition was made in when that still exists, and
 * dropped otherwise (a field that closed, a pane that went). The platform marks
 * those commits (PlatformEvent.char_input.ime_reset): input-method text that
 * arrives after the reset and before the next real keystroke. A switch with no
 * composition in progress resets nothing that matters: the input method has
 * nothing to commit.
 *
 * The target is an identity only — a pane's Session, a field's buffer — and is
 * compared, never dereferenced: a pane that closed is looked for among the
 * live ones before anything is typed into it (main.c).
 *
 * Pure: no platform or AppState.
 */
#ifndef LIU_UI_IME_TARGET_H
#define LIU_UI_IME_TARGET_H

#include "core/types.h"

/* How long after a reset its commits are the old target's: past it, input
 * method text is what the user typed next. */
#define IME_ORPHAN_SEC 0.5

typedef struct {
    const void *target;        /* where the keyboard is: a pane's Session or a field */
    const void *pane;          /* ... the Session when that is a pane, else NULL */
    const void *orphan;        /* the pane a switch ended a composition in, or NULL */
    f64         orphan_until;  /* its commits are taken until then */
    bool        known;         /* `target` has been set once */
} ImeTarget;

/* Once per frame, with where the keyboard is now (`pane_now` is the target
 * again when it is a pane, NULL for a field). True when the input context must
 * be reset: the target changed since the last frame. */
static inline bool ime_target_frame(ImeTarget *it, const void *target_now,
                                    const void *pane_now, f64 now) {
    bool moved = it->known && target_now != it->target;
    if (moved) {
        it->orphan = it->pane;
        it->orphan_until = now + IME_ORPHAN_SEC;
    }
    it->target = target_now;
    it->pane = pane_now;
    it->known = true;
    return moved;
}

typedef enum {
    IME_COMMIT_HERE = 0,   /* the keyboard's target, as any typed text */
    IME_COMMIT_ORPHAN,     /* the pane the composition was made in */
    IME_COMMIT_DROP,       /* it was made in a field, which is not typed into */
} ImeCommitRoute;

/* Where a CHAR_INPUT goes: `reset_commit` is the platform's mark (text the input
 * method committed after a reset, before any new keystroke). */
static inline ImeCommitRoute ime_commit_route(const ImeTarget *it, bool reset_commit, f64 now) {
    if (!reset_commit || now >= it->orphan_until) return IME_COMMIT_HERE;
    return it->orphan ? IME_COMMIT_ORPHAN : IME_COMMIT_DROP;
}

#endif /* LIU_UI_IME_TARGET_H */
