#ifndef UI_SEMANTIC_MARGIN_H
#define UI_SEMANTIC_MARGIN_H
#include "terminal/terminal.h"

/* The current command's range ends at its output cursor or the next prompt.
 * Untouched rows below it are background, not command output. Live row indices
 * also do not describe an alternate screen or a scrollback viewport. */
static inline bool semantic_margin_output_row(const Terminal *term, i32 row) {
    if (!term || (term->mode & MODE_ALT_SCREEN) || term->scroll_offset != 0 ||
        term->output_start_row < 0 || row < term->output_start_row || row >= term->rows)
        return false;
    i32 end=term->cursor_y;
    if ((term->current_zone==ZONE_PROMPT || term->current_zone==ZONE_INPUT) &&
        term->prompt_start_row>=0) end=term->prompt_start_row-1;
    return row<=end;
}
#endif
