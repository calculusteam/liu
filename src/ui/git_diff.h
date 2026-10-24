/*
 * Liu — the diff dock: the working tree's whole diff, docked on the right.
 * Opened from the status bar's git segment.
 *
 * There is no file list, no header and no menu. The panel is the diff and
 * nothing else — git's own file boundaries already separate it, so a picker
 * beside them would only duplicate what the content says, and the branch and
 * ±line figures already live in the status bar.
 *
 * The dock parses `git diff --no-color` and paints the rows itself: a
 * two-column line-number gutter, full-width tinting per row, a card header per
 * file that pins to the top while its rows scroll past, and horizontal
 * scrolling for long lines.
 *
 * It does not draw through a headless Terminal: a terminal grid WRAPS, and in
 * a sidebar at the terminal font most diff lines would fold onto a second
 * unindented row. Wrapping is the one thing a diff must not do, and it is not
 * something the grid can be asked not to do.
 *
 * Everything that shells out runs on a worker thread (see git_exec.h): a cold
 * `git diff` on a large repo is far longer than a frame. Parsing, syntax spans
 * and horizontal byte/column checkpoints are prepared on the worker. The main
 * thread only adopts a finished immutable model and paints visible rows and
 * columns.
 *
 * GitDiffView itself lives in ui.h, as an AppState member; the row and file
 * structs it points at are private to git_diff.c.
 */
#ifndef UI_GIT_DIFF_H
#define UI_GIT_DIFF_H

#include "ui/ui.h"

/* Toggle the dock for the active tab's repo. Opening kicks off the diff job;
 * closing releases the parsed diff (the text and the per-line array), since
 * every open re-runs the job from scratch anyway. */
void git_diff_toggle(AppState *app);
void git_diff_close(AppState *app);
bool git_diff_is_open(const AppState *app);

/* Per-frame: collect finished worker output, and close the dock when the
 * active tab is no longer the repo it was bound to. Returns true when
 * something changed and the frame needs repainting. */
bool git_diff_poll(AppState *app);

void git_diff_render(AppState *app);

/* Event hooks. Each returns true when it consumed the event. */
bool git_diff_handle_key(AppState *app, i32 key, u32 mods);
bool git_diff_handle_click(AppState *app, f32 mx, f32 my);
/* Both axes: long diff rows scroll sideways rather than folding. */
bool git_diff_handle_scroll(AppState *app, f32 mx, f32 my, f32 dx, f32 dy,
                            bool precise);
/* Divider drag on the dock's left edge. handle_drag returns true while it owns
 * the pointer — the caller must relayout, since the terminal grid is derived
 * from the dock width. */
bool git_diff_handle_drag(AppState *app, f32 mx);
void git_diff_end_drag(AppState *app);

void git_diff_free(GitDiffView *v);

#endif /* UI_GIT_DIFF_H */
