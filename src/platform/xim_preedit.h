/*
 * Liu - an on-the-spot composition as the input method describes it (X11)
 *
 * With XIMPreeditCallbacks the input method draws nothing itself: it tells
 * Liu what to change in the composition (replace `chg_length` characters at
 * `chg_first` with some new ones) and Liu keeps the text and draws it at the
 * cursor (platform_linux.c, xim_preedit_*). This header is that buffer, so the
 * edits can be checked without an input method.
 *
 * One more rule than the protocol states: a commit of exactly the text being
 * composed ends the composition. ibus over XIM commits a Chinese word and
 * leaves its draw of that word standing — no empty draw, no PreeditDone — so
 * the committed word would stay drawn at the cursor, over whatever comes next,
 * until another composition began. An input method that keeps composing after
 * a commit (part of a long conversion committed, the rest still open) commits
 * something else, and its composition stays.
 *
 * And one for Escape: ibus-libpinyin cancels a composition on Escape and tells
 * an on-the-spot client nothing — no draw, no PreeditDone, the key not given
 * back — so the cancelled composition would stay drawn. An Escape the input
 * method kept, during a composition, followed by XIM_ESCAPE_QUIET_SEC of
 * silence (no draw, no commit, no key back) ends the composition. Input
 * methods that do answer an Escape (a conversion stepped back, a syllable
 * committed) answer at once, and their answer is what is drawn.
 */
#ifndef LIU_PLATFORM_XIM_PREEDIT_H
#define LIU_PLATFORM_XIM_PREEDIT_H

#include "core/types.h"

#include <string.h>

/* Replace `del` code points at `first` with `ins[0..nins)` in `cp[0..*len)`
 * (capacity `cap`), every count clamped to what the buffer holds: a draw that
 * names more than there is removes what there is. */
static inline void xim_preedit_edit(u32 *cp, i32 *len, i32 cap,
                                    i32 first, i32 del, const u32 *ins, i32 nins) {
    i32 n = *len;
    if (first < 0) first = 0;
    if (first > n) first = n;
    if (del < 0) del = 0;
    if (del > n - first) del = n - first;
    if (nins < 0) nins = 0;
    if (n - del + nins > cap) nins = cap - (n - del);
    memmove(&cp[first + nins], &cp[first + del], (usize)(n - first - del) * sizeof(u32));
    if (nins > 0) memcpy(&cp[first], ins, (usize)nins * sizeof(u32));
    *len = n - del + nins;
}

/* Does the commit `got[0..n)` end the composition `cp[0..len)`? Only when it
 * is that text exactly. */
static inline bool xim_commit_ends_composition(const u32 *cp, i32 len, const u32 *got, i32 n) {
    if (len <= 0 || n != len) return false;
    return memcmp(cp, got, (usize)n * sizeof(u32)) == 0;
}

/* How long the input method has to answer an Escape it kept. */
#define XIM_ESCAPE_QUIET_SEC 0.3

/* Has a kept Escape (at `escape_at`; 0 = none pending) gone unanswered? */
static inline bool xim_escape_cancelled(f64 escape_at, f64 now) {
    return escape_at > 0.0 && now - escape_at >= XIM_ESCAPE_QUIET_SEC;
}

#endif /* LIU_PLATFORM_XIM_PREEDIT_H */
