/*
 * Liu — how the status bar spells a git line count.
 *
 * "+120" and "-31" are the lines added and removed against HEAD. When one of
 * git's answers outgrew the capture (ui/git_status.c counts whole records only
 * and sets `partial`), the true figure is AT LEAST what was counted, and the
 * bar says so with a trailing plus — "+120+", "-31+" — the way a badge says
 * "99+". Without it a very large working tree would read as a small, exact
 * number.
 *
 * ASCII on purpose: the status bar's layout counts one cell per character.
 */
#ifndef UI_GIT_BADGE_H
#define UI_GIT_BADGE_H

#include "core/types.h"
#include <stdio.h>

/* Writes e.g. "+120" (or "+120+" when `at_least`) and returns its length in
 * characters; 0, and an empty string, when there is nothing to show. */
static inline i32 git_badge_figure(char *out, usize cap, char sign, i32 n, bool at_least) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (n <= 0) return 0;
    int w = snprintf(out, cap, "%c%d%s", sign, n, at_least ? "+" : "");
    if (w < 0 || (usize)w >= cap) { out[0] = '\0'; return 0; }
    return (i32)w;
}

#endif /* UI_GIT_BADGE_H */
