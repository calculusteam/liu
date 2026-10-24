/*
 * Liu — how many lines one wheel event moves a view that scrolls BY LINE.
 *
 * The code editor, the plain-text view and the Markdown live editor keep their
 * place as a line number (FileBrowser.view_scroll), so a wheel event has to
 * become a whole number of lines. Rounding every event up to at least one line
 * ("a notch must always move something") is right for a wheel and wrong for a
 * trackpad, which sends a hundred events a second of a pixel or two each:
 * every one of them would become a full line. The terminal, the palette and
 * the transcript viewer handle this the same way, in main.c: accumulate the
 * pixels, take a step when there are enough.
 *
 * The pixels are carried from one event to the next in `*accum`, and the step
 * is the real height of a line of the view — the font's cell in the code
 * editor, the body line in the live editor — so the content follows the
 * fingers at the same speed as the rendered Markdown view, which scrolls by
 * the pixel. A wheel notch is `wheel_lines` lines (PLATFORM_VIEW_WHEEL_LINES:
 * three on macOS, the terminal's own step; one on X11, where the desktop's
 * lines-per-notch is already in the delta).
 *
 * The result is positive for "content moves down" (the finger or the wheel
 * went down), i.e. the caller SUBTRACTS it from the first visible line.
 *
 * Pure.
 */
#ifndef UI_LINE_SCROLL_H
#define UI_LINE_SCROLL_H

#include "core/types.h"

static inline i32 line_scroll_take(f32 *accum, f32 dy, bool precise,
                                   f32 row_px, f32 wheel_lines) {
    if (row_px < 1.0f) row_px = 1.0f;
    *accum += precise ? dy : dy * row_px * wheel_lines;
    f32 q = *accum / row_px;
    /* A runaway delta must not overflow the conversion. */
    if (q >  1.0e6f) q =  1.0e6f;
    if (q < -1.0e6f) q = -1.0e6f;
    i32 steps = (i32)q;
    *accum -= (f32)steps * row_px;
    return steps;
}

#endif /* UI_LINE_SCROLL_H */
