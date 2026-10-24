/*
 * autosuggest — when the ghost suggestion at the prompt may be shown, and so
 * taken.
 *
 * The ghost is the still-untyped rest of a command from this folder's history,
 * drawn dim after the cursor; the accept key types it. A ghost clipped at the
 * row's end would hide part of what accepting sends: a history entry with a
 * run of spaces in the middle could show a harmless head and keep its tail off
 * screen. Whatever fills the history — and a folder's history is only as
 * trustworthy as what was allowed to write to it — the rule for the ghost
 * itself is the one a person assumes: what is taken is what was seen.
 *
 *   - the whole of it fits between the cursor and the right edge, or it is
 *     not offered;
 *   - it carries no control byte and none of the code points that only
 *     deceive (core/text_sanitize.h: bidi overrides, zero-width, tags);
 *   - it is not offered while the pane's program has echo off: the "prefix"
 *     being completed there is a secret being typed.
 *
 * Pure: no state and no I/O.
 */
#ifndef UI_AUTOSUGGEST_H
#define UI_AUTOSUGGEST_H

#include "core/types.h"
#include "core/utf8.h"
#include "core/text_sanitize.h"

#include <string.h>

/* Columns the text takes on screen, or -1 when it holds something that must
 * not be drawn as a suggestion at all. */
static inline i32 autosuggest_cols(const char *text) {
    if (!text) return -1;
    usize n = strlen(text);
    const unsigned char *p = (const unsigned char *)text;
    for (usize i = 0; i < n; ) {
        if (p[i] < 0x20 || p[i] == 0x7f) return -1;
        usize bad = liu_deceptive_utf8_len(p + i, n - i);
        if (bad) return -1;
        /* next code point */
        usize step = 1;
        if      (p[i] >= 0xF0) step = 4;
        else if (p[i] >= 0xE0) step = 3;
        else if (p[i] >= 0xC0) step = 2;
        if (step > n - i) return -1;                 /* cut mid-sequence */
        i += step;
    }
    if (!utf8_validate((const u8 *)text, n)) return -1;
    return (i32)utf8_display_cols((const u8 *)text, n);
}

/* May `suffix` be shown (and accepted) with the cursor at column `cursor_x`
 * of a `cols`-wide pane? */
static inline bool autosuggest_offerable(const char *suffix, i32 cursor_x, i32 cols, bool echo_off) {
    if (echo_off || !suffix || !suffix[0] || cols <= 0 || cursor_x < 0 || cursor_x >= cols) return false;
    i32 w = autosuggest_cols(suffix);
    return w > 0 && w <= cols - cursor_x;
}

/* May this line go into the history at all? The shells' own convention: a
 * line that starts with a space is not recorded (zsh HIST_IGNORE_SPACE, bash
 * HISTCONTROL=ignorespace) — the way to keep one command out. */
static inline bool autosuggest_recordable(const char *line, bool history_on, bool echo_off) {
    return history_on && !echo_off && line && line[0] && line[0] != ' ';
}

#endif /* UI_AUTOSUGGEST_H */
