/*
 * palette256.h — the xterm table above the sixteen colours a theme sets: the
 * 6x6x6 cube (16-231) and the grey ramp (232-255), as 0xRRGGBB.
 *
 * Both renderers draw indices 16-255 from it and an OSC 4 query is answered
 * from it (vt_parser.c), so the screen and the answer cannot disagree. Pure.
 */
#ifndef LIU_TERMINAL_PALETTE256_H
#define LIU_TERMINAL_PALETTE256_H

#include "core/types.h"

static inline u32 palette256_rgb(i32 i) {
    if (i < 16 || i > 255) return 0;
    if (i >= 232) {
        u32 v = 8u + (u32)(i - 232) * 10u;
        return (v << 16) | (v << 8) | v;
    }
    i32 c = i - 16;
    i32 r = (c / 36) % 6, g = (c / 6) % 6, b = c % 6;
    u32 R = r ? 55u + (u32)r * 40u : 0u;
    u32 G = g ? 55u + (u32)g * 40u : 0u;
    u32 B = b ? 55u + (u32)b * 40u : 0u;
    return (R << 16) | (G << 8) | B;
}

#endif
