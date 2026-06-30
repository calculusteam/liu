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

/* Determine the base (paragraph) direction for a line.
 * Scans for the first strong character (L, R, or AL). */
BidiDir bidi_base_direction(const Cell *cells, i32 cols);

/* Simple Arabic presentation form shaping.
 * Modifies codepoints in-place within the visual cell array.
 * Must be called AFTER bidi_reorder_line(). */
void bidi_shape_arabic(Cell *visual, i32 cols);

#endif /* BIDI_H */
