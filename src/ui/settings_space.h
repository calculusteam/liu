/*
 * Liu — the Settings panel's vertical spacing, in points.
 *
 * The panel draws at a fixed cell (8 x 16 points, times the display's scale).
 * Its row spacing is a point value, times the display's scale where it is
 * used. A raw pixel constant (`item_h = ch + 10`, `y += 10`,
 * `content_end_y = y + ch + 16`) is half as many points on a 2x display as on
 * a 1x one, so the same page would sit tighter at 2x.
 *
 * The numbers are the pixel values the panel is designed at, halved, because
 * its design scale is 2: at that scale every expression below evaluates to
 * exactly the float of the pixel constant it stands for (a half-integer times
 * 2.0f is exact), so a 2x display is laid out in those pixels, and a 1x display
 * gets the same proportions instead of a looser page.
 *
 * Pure arithmetic: no display is needed.
 */
#ifndef UI_SETTINGS_SPACE_H
#define UI_SETTINGS_SPACE_H

#include "core/types.h"

/* Appearance. A stepper row (Size, Line Height, Text Weight) is the cell
 * height plus this. */
#define SETTINGS_ITEM_PAD_PT      5.0f
/* ... the air between Appearance's blocks: the font grid, the steppers, Theme. */
#define SETTINGS_BLOCK_GAP_PT     5.0f
/* ... under the "Theme" label, and again under the hairline below it. */
#define SETTINGS_THEME_HEAD_PT    4.0f
/* ... below the Text Weight steppers, before the next block's own gap. */
#define SETTINGS_WEIGHT_TAIL_PT   4.0f

/* Every page: the space under its last row that the scroll extent keeps, and
 * the margin the visible band loses to the same sum. */
#define SETTINGS_TAIL_PT          8.0f
#define SETTINGS_BODY_SLACK_PT    8.0f

/* Keys (the rebind prompt) and Vault (the status line): the gap a line puts
 * under itself, narrow and wide. */
#define SETTINGS_LINE_GAP_PT      2.0f
#define SETTINGS_LINE_GAP_WIDE_PT 6.0f

/* The row height of Appearance's steppers for the panel's cell height. */
static inline f32 settings_item_h(f32 ch, f32 dpi) {
    return ch + SETTINGS_ITEM_PAD_PT * dpi;
}

#endif /* UI_SETTINGS_SPACE_H */
