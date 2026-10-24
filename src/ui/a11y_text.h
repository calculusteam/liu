/*
 * Liu - the terminal's text as the accessibility tree sees it.
 *
 * Builds the PlatformA11yText snapshot (platform/platform_a11y.h) from ONE
 * Terminal: its visible rows, read through the same row reader the renderer
 * uses and turned back into text the way a copy does (selection.c) — wide
 * placeholders skipped, a never-written cell is a blank, a cell that holds a
 * grapheme cluster HANDLE is encoded whole through utf8_cluster_encode, and a
 * soft wrap is not a line break. Logical order, like the copy: BiDi reordering
 * is a display transform and neither of them applies it.
 *
 * Two departures from the copy, both about what a screen reader should say:
 * a concealed cell (SGR 8, ATTR_HIDDEN) reads as a blank because a sighted
 * user cannot see it either, and the whole viewport is described, one entry in
 * the line table per screen row.
 *
 * Pure over a Terminal, so it runs against a real parser with no app around
 * it; the AppState glue (which pane, where it sits, the tab's title) lives in
 * a11y_bridge.c.
 */
#ifndef LIU_UI_A11Y_TEXT_H
#define LIU_UI_A11Y_TEXT_H

#include "core/types.h"
#include "platform/platform_a11y.h"
#include "terminal/terminal.h"

/* Growable storage behind a snapshot. Zero-initialise; reuse across builds;
 * a11y_text_buf_free releases it. */
typedef struct {
    char *text;         u32 text_len, text_cap;
    u16  *col16;        u32 len16, col_cap;
    u32  *line_start16;
    u32  *line_text_end16;
    u16  *line_end_col;
    i32   line_cap;
    char *sel_text;
} A11yTextBuf;

/* The two change counters, without building anything and without touching
 * the terminal (the selection is read raw, not synced). */
void a11y_text_generation(Terminal *t, u64 *out_value, u64 *out_selection);

/* Describe `t`'s viewport into `out`, whose pointers then point into `buf`.
 * Fills the generations, the text and line tables, the selection and the
 * cursor; leaves geometry and label to the caller. False on allocation
 * failure or an empty terminal (out is then an empty description). */
bool a11y_text_build(Terminal *t, A11yTextBuf *buf, PlatformA11yText *out);

void a11y_text_buf_free(A11yTextBuf *buf);

/* The provider main.c registers per window (a11y_bridge.c): `user` is that
 * window's AppState. Describes the active tab's FOCUSED pane — its text,
 * where its grid sits in the view, and "Terminal — <tab title>". */
bool app_a11y_provider(PlatformWindow *w, void *user, PlatformA11yText *out,
                       bool generation_only);

#endif /* LIU_UI_A11Y_TEXT_H */
