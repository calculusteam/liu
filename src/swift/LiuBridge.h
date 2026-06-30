/*
 * Liu — C↔Swift bridging header
 * Exposes C terminal state to Swift rendering layer.
 */

#ifndef Liu_BRIDGE_H
#define Liu_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

/* Terminal cell data — matches terminal.h CellAttr + Cell */
typedef struct {
    uint32_t codepoint;
    uint32_t fg;              /* ANSI 0-255 or 0x01RRGGBB */
    uint32_t bg;
    uint16_t flags;           /* BOLD=1, DIM=2, ITALIC=4, UNDERLINE=8, ... */
    uint16_t _pad;
} LiuCell;

/* Terminal grid snapshot for Swift renderer */
typedef struct {
    const LiuCell *cells;
    int32_t cols;
    int32_t rows;
    int32_t cursor_x;
    int32_t cursor_y;
    uint8_t cursor_style;   /* 0=block, 1=underline, 2=bar */
    bool    cursor_visible;
} LiuTerminalGrid;

/* Color — matches renderer Color */
typedef struct {
    float r, g, b, a;
} LiuColor;

/* ANSI 256 color palette */
extern LiuColor Liu_ansi_colors[256];

/* Truecolor check */
#define Liu_IS_TRUECOLOR(c) ((c) & 0x01000000)

/* Attribute flags */
#define Liu_ATTR_BOLD       (1 << 0)
#define Liu_ATTR_DIM        (1 << 1)
#define Liu_ATTR_ITALIC     (1 << 2)
#define Liu_ATTR_UNDERLINE  (1 << 3)
#define Liu_ATTR_INVERSE    (1 << 5)
#define Liu_ATTR_STRIKETHROUGH (1 << 7)
#define Liu_ATTR_WIDE       (1 << 8)
#define Liu_ATTR_WDUMMY     (1 << 9)
#define Liu_ATTR_OVERLINE   (1 << 10)
#define Liu_ATTR_HYPERLINK  (1 << 11)

/* Default color indices */
#define Liu_FG_DEFAULT 7
#define Liu_BG_DEFAULT 0

#endif /* Liu_BRIDGE_H */
