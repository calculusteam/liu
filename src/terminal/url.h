/*
 * Liu — URL detection in terminal content
 * Detects clickable URLs by scanning cell content around a given position.
 */
#ifndef TERMINAL_URL_H
#define TERMINAL_URL_H

#include "core/types.h"
#include "terminal/terminal.h"

typedef struct {
    i32  start_col, start_row;
    i32  end_col, end_row;
    char url[2048];
} TermURL;

/* Find URL at a given cell position. Returns true if found.
 * Soft-wrapped rows are joined without separators; hard line breaks stop the
 * scan. Returned rows are viewport-relative and may extend outside the viewport
 * when the URL's prefix or suffix is in available scrollback/live rows.
 * URLs exceeding the output capacity are rejected rather than truncated.
 *
 * `col` is a LOGICAL grid column. Anything derived from a PIXEL is a visual column
 * and must come through url_detect_at_visual() instead — on a BiDi row the two name
 * different characters. */
bool url_detect_at(Terminal *t, i32 col, i32 row, TermURL *result);

/* Find the URL under a VISUAL (screen) column — the one a hit test produces by
 * dividing a pixel by the cell width.
 *
 * Every pointer-driven URL path goes through here: hover, Cmd+click and the
 * right-click copy. It is one function rather than a conversion repeated at each
 * site, so no site can hand a visual column straight to the logical grid. On an
 * LTR row this is url_detect_at with the same number; on a mixed RTL/LTR row a URL
 * can live at logical 3..23 and be DRAWN at visual 36..56.
 *
 * The reordering is derived from the SAME cells url_detect_at scans, so the
 * conversion and the detection cannot disagree about what is on the row.
 *
 * NOT for TUI mouse reporting: a program that grabbed the mouse drew the screen
 * itself and its protocol names physical cells. */
bool url_detect_at_visual(Terminal *t, i32 visual_col, i32 row, TermURL *result);

/* Check if a position is inside a detected URL */
bool url_contains(const TermURL *url, i32 col, i32 row);

#endif /* TERMINAL_URL_H */
