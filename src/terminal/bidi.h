/*
 * Liu — Bidirectional text (BiDi) support
 * Simplified UAX #9 (Unicode BiDi Algorithm) for terminal rendering.
 * Covers Hebrew, Arabic, and mixed LTR/RTL text.
 */
#ifndef BIDI_H
#define BIDI_H

#include "core/types.h"
#include "terminal/terminal.h"

/* =========================================================================
 * BiDi character types (UAX #9 Section 3.2)
 * ========================================================================= */

typedef enum {
    /* Strong types */
    BIDI_L,     /* Left-to-Right (Latin, CJK, etc.) */
    BIDI_R,     /* Right-to-Left (Hebrew) */
    BIDI_AL,    /* Arabic Letter */

    /* Weak types */
    BIDI_EN,    /* European Number (0-9) */
    BIDI_AN,    /* Arabic-Indic Number */
    BIDI_ES,    /* European Separator (+, -) */
    BIDI_ET,    /* European Terminator (%, $, etc.) */
    BIDI_CS,    /* Common Separator (,, ., :, /) */
    BIDI_NSM,   /* Non-Spacing Mark */
    BIDI_BN,    /* Boundary Neutral (formatting controls) */

    /* Neutral types */
    BIDI_WS,    /* Whitespace */
    BIDI_ON,    /* Other Neutral (punctuation, symbols) */

    BIDI_TYPE_COUNT
} BidiType;

/* Base paragraph direction */
typedef enum {
    BIDI_DIR_LTR = 0,
    BIDI_DIR_RTL = 1,
} BidiDir;

/* =========================================================================
 * API
 * ========================================================================= */

/* Classify a Unicode codepoint's BiDi type */
BidiType bidi_char_type(u32 cp);

/* Quick check: does a row contain any RTL characters? */
bool bidi_line_has_rtl(const Cell *cells, i32 cols);

/* Apply the BiDi algorithm to reorder a row for visual display.
 *
 * cells:            input logical-order cells
 * cols:             number of columns
 * visual:           output visual-order cells (must be pre-allocated, cols entries)
 * logical_to_visual: optional mapping from logical index to visual index
 *                    (NULL if not needed). Must be pre-allocated, cols entries.
 *
 * The caller can use logical_to_visual for cursor/selection mapping. */
void bidi_reorder_line(const Cell *cells, i32 cols, Cell *visual,
                       i32 *logical_to_visual);

/* Same, plus the INVERSE map.
 *
 * visual_to_logical[v] is the logical column drawn at visual column v. Rendering
 * needs the forward direction (it knows which logical cell it is drawing and asks
 * where to put it); INPUT needs this one — the hit test turns a pixel into a
 * visual column and the selection engine addresses logical columns. Without it a
 * click on the leftmost of three Hebrew letters would select the rightmost, and
 * Cmd+C would hand back text the user had not highlighted.
 *
 * Either map may be NULL. Both must have room for `cols` entries. */
void bidi_reorder_line_ex(const Cell *cells, i32 cols, Cell *visual,
                          i32 *logical_to_visual, i32 *visual_to_logical);

/* The logical column drawn at visual column `vis_col`, for one row.
 *
 * The input-side counterpart of logical_to_visual, and the reason it is a function
 * rather than a map the caller inverts itself: it also resolves a wide glyph's
 * CONTINUATION cell to its head, because clicking the right half of a CJK
 * character or an emoji has to name the character. Returns `vis_col` unchanged
 * for a row with no RTL in it (the identity case, which is every LTR row) and
 * clamps to [0, cols). */
i32 bidi_visual_to_logical_col(const Cell *cells, i32 cols, i32 vis_col);

/* Determine the base (paragraph) direction for a line.
 * Scans for the first strong character (L, R, or AL). */
BidiDir bidi_base_direction(const Cell *cells, i32 cols);

/* Simple Arabic presentation form shaping.
 * Modifies codepoints in-place within the visual cell array.
 * Must be called AFTER bidi_reorder_line(). */
void bidi_shape_arabic(Cell *visual, i32 cols);

#endif /* BIDI_H */
