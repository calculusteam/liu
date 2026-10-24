/*
 * Liu — glyphs drawn from the cell geometry instead of the font.
 *
 * Box drawing, block elements, braille, the powerline separators and the
 * block mosaics of Symbols for Legacy Computing are shapes that have to MEET
 * the shape in the next cell: a line continues at the same rows, a block
 * reaches the cell edge, a quadrant's corner sits where the half block's edge
 * does. A font glyph is designed for one advance and one baseline, and is a
 * pixel short or a pixel off as soon as the cell is not the size it was drawn
 * for. Here every shape is computed from the cell (cw x ch) and one stroke
 * width, by the same few functions for every codepoint, so neighbours agree.
 *
 * Output is an R8 coverage bitmap, cw x ch with stride cw: 0..255 is the
 * fraction of the pixel covered (linear). Straight strokes and blocks sit on
 * whole pixels; arcs, diagonals and wedges are antialiased by supersampling.
 * Each glyph is drawn once into the atlas cache, so clarity wins over speed.
 *
 * Shape data comes from tables generated from the Unicode character names
 * (box arms, sextant and octant masks, block rectangles, mosaic polygons,
 * diagonal segments); only the drawing is written by hand.
 *
 * Everything here is static; the header is included by the rasteriser only.
 */
#ifndef LIU_SPRITE_DRAW_H
#define LIU_SPRITE_DRAW_H

#include "core/types.h"
#include <math.h>
#include <string.h>

/* Weight of one arm of a box-drawing glyph. */
enum { SPR_NONE = 0, SPR_LIGHT = 1, SPR_HEAVY = 2, SPR_DOUBLE = 3 };

/* Supersamples per axis for the antialiased shapes: 64 levels, enough that a
 * shallow diagonal steps smoothly. Braille dots and the separators
 * U+E0B0-E0B7 sample 4 (16 levels), the density their edges are set for. */
#define SPR_SS      8
#define SPR_SS_DOTS 4

typedef struct { u32 cp; u8 n; u8 r[4][5]; } SprRects;          /* x0 y0 x1 y1 level */
typedef struct { u32 cp; u8 level; u8 n; u8 pt[6][2]; } SprPoly;
typedef struct { u32 cp; u8 n; u8 seg[4]; } SprLines;

/* ---- Generated from the Unicode character names: do not edit by hand ---- */

/* U+2500 + i: bits 0-1 up, 2-3 right, 4-5 down, 6-7 left (SPR_NONE/LIGHT/HEAVY/DOUBLE). */
static const u8 spr_box_arms[128] = {
    0x44, 0x88, 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x14, 0x18, 0x24, 0x28,
    0x50, 0x90, 0x60, 0xA0, 0x05, 0x09, 0x06, 0x0A, 0x41, 0x81, 0x42, 0x82, 0x15, 0x19, 0x16, 0x25,
    0x26, 0x1A, 0x29, 0x2A, 0x51, 0x91, 0x52, 0x61, 0x62, 0x92, 0xA1, 0xA2, 0x54, 0x94, 0x58, 0x98,
    0x64, 0xA4, 0x68, 0xA8, 0x45, 0x85, 0x49, 0x89, 0x46, 0x86, 0x4A, 0x8A, 0x55, 0x95, 0x59, 0x99,
    0x56, 0x65, 0x66, 0x96, 0x5A, 0xA5, 0x69, 0x9A, 0xA9, 0xA6, 0x6A, 0xAA, 0x44, 0x88, 0x11, 0x22,
    0xCC, 0x33, 0x1C, 0x34, 0x3C, 0xD0, 0x70, 0xF0, 0x0D, 0x07, 0x0F, 0xC1, 0x43, 0xC3, 0x1D, 0x37,
    0x3F, 0xD1, 0x73, 0xF3, 0xDC, 0x74, 0xFC, 0xCD, 0x47, 0xCF, 0xDD, 0x77, 0xFF, 0x14, 0x50, 0x41,
    0x05, 0x00, 0x00, 0x00, 0x40, 0x01, 0x04, 0x10, 0x80, 0x02, 0x08, 0x20, 0x48, 0x21, 0x84, 0x12,
};

/* U+1FB00 + i: bit k-1 set when sextant k is filled (1 2 / 3 4 / 5 6). */
static const u8 spr_sextant[60] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21,
    0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32,
    0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
};

/* U+1CD00 + i: bit k-1 set when octant k is filled (1 2 / 3 4 / 5 6 / 7 8). */
static const u8 spr_octant[230] = {
    0x04, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0C, 0x0D, 0x0E, 0x10, 0x11, 0x12, 0x13, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
    0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x51, 0x52, 0x53, 0x54, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C, 0x5D,
    0x5E, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E,
    0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E,
    0x7F, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
    0xA1, 0xA2, 0xA3, 0xA4, 0xA6, 0xA7, 0xA8, 0xA9, 0xAB, 0xAC, 0xAD, 0xAE, 0xB0, 0xB1, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC1, 0xC2, 0xC3, 0xC4,
    0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4,
    0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4,
    0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF1, 0xF2, 0xF3, 0xF4, 0xF6,
    0xF7, 0xF8, 0xF9, 0xFB, 0xFD, 0xFE,
};

/* Up to four rectangles x0, y0, x1, y1 in 24ths of the cell, each with its coverage. */
static const SprRects spr_rects[] = {
    { 0x02580, 1, { {0, 0, 24, 12, 0xFF} } },  /* upper half block */
    { 0x02581, 1, { {0, 21, 24, 24, 0xFF} } },  /* lower one eighth block */
    { 0x02582, 1, { {0, 18, 24, 24, 0xFF} } },  /* lower one quarter block */
    { 0x02583, 1, { {0, 15, 24, 24, 0xFF} } },  /* lower three eighths block */
    { 0x02584, 1, { {0, 12, 24, 24, 0xFF} } },  /* lower half block */
    { 0x02585, 1, { {0, 9, 24, 24, 0xFF} } },  /* lower five eighths block */
    { 0x02586, 1, { {0, 6, 24, 24, 0xFF} } },  /* lower three quarters block */
    { 0x02587, 1, { {0, 3, 24, 24, 0xFF} } },  /* lower seven eighths block */
    { 0x02588, 1, { {0, 0, 24, 24, 0xFF} } },  /* full block */
    { 0x02589, 1, { {0, 0, 21, 24, 0xFF} } },  /* left seven eighths block */
    { 0x0258A, 1, { {0, 0, 18, 24, 0xFF} } },  /* left three quarters block */
    { 0x0258B, 1, { {0, 0, 15, 24, 0xFF} } },  /* left five eighths block */
    { 0x0258C, 1, { {0, 0, 12, 24, 0xFF} } },  /* left half block */
    { 0x0258D, 1, { {0, 0, 9, 24, 0xFF} } },  /* left three eighths block */
    { 0x0258E, 1, { {0, 0, 6, 24, 0xFF} } },  /* left one quarter block */
    { 0x0258F, 1, { {0, 0, 3, 24, 0xFF} } },  /* left one eighth block */
    { 0x02590, 1, { {12, 0, 24, 24, 0xFF} } },  /* right half block */
    { 0x02591, 1, { {0, 0, 24, 24, 0x40} } },  /* light shade */
    { 0x02592, 1, { {0, 0, 24, 24, 0x80} } },  /* medium shade */
    { 0x02593, 1, { {0, 0, 24, 24, 0xC0} } },  /* dark shade */
    { 0x02594, 1, { {0, 0, 24, 3, 0xFF} } },  /* upper one eighth block */
    { 0x02595, 1, { {21, 0, 24, 24, 0xFF} } },  /* right one eighth block */
    { 0x02596, 1, { {0, 12, 12, 24, 0xFF} } },  /* quadrant lower left */
    { 0x02597, 1, { {12, 12, 24, 24, 0xFF} } },  /* quadrant lower right */
    { 0x02598, 1, { {0, 0, 12, 12, 0xFF} } },  /* quadrant upper left */
    { 0x02599, 3, { {0, 0, 12, 12, 0xFF}, {0, 12, 12, 24, 0xFF}, {12, 12, 24, 24, 0xFF} } },  /* quadrant upper left and lower left and lower right */
    { 0x0259A, 2, { {0, 0, 12, 12, 0xFF}, {12, 12, 24, 24, 0xFF} } },  /* quadrant upper left and lower right */
    { 0x0259B, 3, { {0, 0, 12, 12, 0xFF}, {12, 0, 24, 12, 0xFF}, {0, 12, 12, 24, 0xFF} } },  /* quadrant upper left and upper right and lower left */
    { 0x0259C, 3, { {0, 0, 12, 12, 0xFF}, {12, 0, 24, 12, 0xFF}, {12, 12, 24, 24, 0xFF} } },  /* quadrant upper left and upper right and lower right */
    { 0x0259D, 1, { {12, 0, 24, 12, 0xFF} } },  /* quadrant upper right */
    { 0x0259E, 2, { {12, 0, 24, 12, 0xFF}, {0, 12, 12, 24, 0xFF} } },  /* quadrant upper right and lower left */
    { 0x0259F, 3, { {12, 0, 24, 12, 0xFF}, {0, 12, 12, 24, 0xFF}, {12, 12, 24, 24, 0xFF} } },  /* quadrant upper right and lower left and lower right */
    { 0x1FB70, 1, { {3, 0, 6, 24, 0xFF} } },  /* vertical one eighth block-2 */
    { 0x1FB71, 1, { {6, 0, 9, 24, 0xFF} } },  /* vertical one eighth block-3 */
    { 0x1FB72, 1, { {9, 0, 12, 24, 0xFF} } },  /* vertical one eighth block-4 */
    { 0x1FB73, 1, { {12, 0, 15, 24, 0xFF} } },  /* vertical one eighth block-5 */
    { 0x1FB74, 1, { {15, 0, 18, 24, 0xFF} } },  /* vertical one eighth block-6 */
    { 0x1FB75, 1, { {18, 0, 21, 24, 0xFF} } },  /* vertical one eighth block-7 */
    { 0x1FB76, 1, { {0, 3, 24, 6, 0xFF} } },  /* horizontal one eighth block-2 */
    { 0x1FB77, 1, { {0, 6, 24, 9, 0xFF} } },  /* horizontal one eighth block-3 */
    { 0x1FB78, 1, { {0, 9, 24, 12, 0xFF} } },  /* horizontal one eighth block-4 */
    { 0x1FB79, 1, { {0, 12, 24, 15, 0xFF} } },  /* horizontal one eighth block-5 */
    { 0x1FB7A, 1, { {0, 15, 24, 18, 0xFF} } },  /* horizontal one eighth block-6 */
    { 0x1FB7B, 1, { {0, 18, 24, 21, 0xFF} } },  /* horizontal one eighth block-7 */
    { 0x1FB7C, 2, { {0, 0, 3, 24, 0xFF}, {0, 21, 24, 24, 0xFF} } },  /* left and lower one eighth block */
    { 0x1FB7D, 2, { {0, 0, 3, 24, 0xFF}, {0, 0, 24, 3, 0xFF} } },  /* left and upper one eighth block */
    { 0x1FB7E, 2, { {21, 0, 24, 24, 0xFF}, {0, 0, 24, 3, 0xFF} } },  /* right and upper one eighth block */
    { 0x1FB7F, 2, { {21, 0, 24, 24, 0xFF}, {0, 21, 24, 24, 0xFF} } },  /* right and lower one eighth block */
    { 0x1FB80, 2, { {0, 0, 24, 3, 0xFF}, {0, 21, 24, 24, 0xFF} } },  /* upper and lower one eighth block */
    { 0x1FB81, 4, { {0, 0, 24, 3, 0xFF}, {0, 6, 24, 9, 0xFF}, {0, 12, 24, 15, 0xFF}, {0, 21, 24, 24, 0xFF} } },  /* horizontal one eighth block-1358 */
    { 0x1FB82, 1, { {0, 0, 24, 6, 0xFF} } },  /* upper one quarter block */
    { 0x1FB83, 1, { {0, 0, 24, 9, 0xFF} } },  /* upper three eighths block */
    { 0x1FB84, 1, { {0, 0, 24, 15, 0xFF} } },  /* upper five eighths block */
    { 0x1FB85, 1, { {0, 0, 24, 18, 0xFF} } },  /* upper three quarters block */
    { 0x1FB86, 1, { {0, 0, 24, 21, 0xFF} } },  /* upper seven eighths block */
    { 0x1FB87, 1, { {18, 0, 24, 24, 0xFF} } },  /* right one quarter block */
    { 0x1FB88, 1, { {15, 0, 24, 24, 0xFF} } },  /* right three eighths block */
    { 0x1FB89, 1, { {9, 0, 24, 24, 0xFF} } },  /* right five eighths block */
    { 0x1FB8A, 1, { {6, 0, 24, 24, 0xFF} } },  /* right three quarters block */
    { 0x1FB8B, 1, { {3, 0, 24, 24, 0xFF} } },  /* right seven eighths block */
    { 0x1FB8C, 1, { {0, 0, 12, 24, 0x80} } },  /* left half medium shade */
    { 0x1FB8D, 1, { {12, 0, 24, 24, 0x80} } },  /* right half medium shade */
    { 0x1FB8E, 1, { {0, 0, 24, 12, 0x80} } },  /* upper half medium shade */
    { 0x1FB8F, 1, { {0, 12, 24, 24, 0x80} } },  /* lower half medium shade */
    { 0x1FB90, 1, { {0, 0, 24, 24, 0x80} } },  /* inverse medium shade */
    { 0x1FB91, 2, { {0, 0, 24, 12, 0xFF}, {0, 12, 24, 24, 0x80} } },  /* upper half block and lower half inverse medium shade */
    { 0x1FB92, 2, { {0, 0, 24, 12, 0x80}, {0, 12, 24, 24, 0xFF} } },  /* upper half inverse medium shade and lower half block */
    { 0x1FB94, 2, { {0, 0, 12, 24, 0x80}, {12, 0, 24, 24, 0xFF} } },  /* left half inverse medium shade and right half block */
    { 0x1FB97, 2, { {0, 6, 24, 12, 0xFF}, {0, 18, 24, 24, 0xFF} } },  /* heavy horizontal fill */
    { 0x1FBCE, 1, { {0, 0, 16, 24, 0xFF} } },  /* left two thirds block */
    { 0x1FBCF, 1, { {0, 0, 8, 24, 0xFF} } },  /* left one third block */
    { 0x1FBE4, 1, { {6, 0, 18, 12, 0xFF} } },  /* upper centre one quarter block */
    { 0x1FBE5, 1, { {6, 12, 18, 24, 0xFF} } },  /* lower centre one quarter block */
    { 0x1FBE6, 1, { {0, 6, 12, 18, 0xFF} } },  /* middle left one quarter block */
    { 0x1FBE7, 1, { {12, 6, 24, 18, 0xFF} } },  /* middle right one quarter block */
    { 0x1CE90, 1, { {0, 0, 6, 6, 0xFF} } },  /* upper left one sixteenth block */
    { 0x1CE91, 1, { {6, 0, 12, 6, 0xFF} } },  /* upper centre left one sixteenth block */
    { 0x1CE92, 1, { {12, 0, 18, 6, 0xFF} } },  /* upper centre right one sixteenth block */
    { 0x1CE93, 1, { {18, 0, 24, 6, 0xFF} } },  /* upper right one sixteenth block */
    { 0x1CE94, 1, { {0, 6, 6, 12, 0xFF} } },  /* upper middle left one sixteenth block */
    { 0x1CE95, 1, { {6, 6, 12, 12, 0xFF} } },  /* upper middle centre left one sixteenth block */
    { 0x1CE96, 1, { {12, 6, 18, 12, 0xFF} } },  /* upper middle centre right one sixteenth block */
    { 0x1CE97, 1, { {18, 6, 24, 12, 0xFF} } },  /* upper middle right one sixteenth block */
    { 0x1CE98, 1, { {0, 12, 6, 18, 0xFF} } },  /* lower middle left one sixteenth block */
    { 0x1CE99, 1, { {6, 12, 12, 18, 0xFF} } },  /* lower middle centre left one sixteenth block */
    { 0x1CE9A, 1, { {12, 12, 18, 18, 0xFF} } },  /* lower middle centre right one sixteenth block */
    { 0x1CE9B, 1, { {18, 12, 24, 18, 0xFF} } },  /* lower middle right one sixteenth block */
    { 0x1CE9C, 1, { {0, 18, 6, 24, 0xFF} } },  /* lower left one sixteenth block */
    { 0x1CE9D, 1, { {6, 18, 12, 24, 0xFF} } },  /* lower centre left one sixteenth block */
    { 0x1CE9E, 1, { {12, 18, 18, 24, 0xFF} } },  /* lower centre right one sixteenth block */
    { 0x1CE9F, 1, { {18, 18, 24, 24, 0xFF} } },  /* lower right one sixteenth block */
    { 0x1CEA0, 1, { {12, 18, 24, 24, 0xFF} } },  /* right half lower one quarter block */
    { 0x1CEA1, 1, { {6, 18, 24, 24, 0xFF} } },  /* right three quarters lower one quarter block */
    { 0x1CEA2, 1, { {0, 18, 18, 24, 0xFF} } },  /* left three quarters lower one quarter block */
    { 0x1CEA3, 1, { {0, 18, 12, 24, 0xFF} } },  /* left half lower one quarter block */
    { 0x1CEA4, 1, { {0, 12, 6, 24, 0xFF} } },  /* lower half left one quarter block */
    { 0x1CEA5, 1, { {0, 6, 6, 24, 0xFF} } },  /* lower three quarters left one quarter block */
    { 0x1CEA6, 1, { {0, 0, 6, 18, 0xFF} } },  /* upper three quarters left one quarter block */
    { 0x1CEA7, 1, { {0, 0, 6, 12, 0xFF} } },  /* upper half left one quarter block */
    { 0x1CEA8, 1, { {0, 0, 12, 6, 0xFF} } },  /* left half upper one quarter block */
    { 0x1CEA9, 1, { {0, 0, 18, 6, 0xFF} } },  /* left three quarters upper one quarter block */
    { 0x1CEAA, 1, { {6, 0, 24, 6, 0xFF} } },  /* right three quarters upper one quarter block */
    { 0x1CEAB, 1, { {12, 0, 24, 6, 0xFF} } },  /* right half upper one quarter block */
    { 0x1CEAC, 1, { {18, 0, 24, 12, 0xFF} } },  /* upper half right one quarter block */
    { 0x1CEAD, 1, { {18, 0, 24, 18, 0xFF} } },  /* upper three quarters right one quarter block */
    { 0x1CEAE, 1, { {18, 6, 24, 24, 0xFF} } },  /* lower three quarters right one quarter block */
    { 0x1CEAF, 1, { {18, 12, 24, 24, 0xFF} } },  /* lower half right one quarter block */
};

/* One polygon, vertices in 24ths of the cell, filled at `level` (even-odd rule). */
static const SprPoly spr_polys[] = {
    { 0x1FB3C, 0xFF, 3, { {12, 24}, {0, 24}, {0, 16} } },  /* lower left block diagonal lower middle left to lower centre */
    { 0x1FB3D, 0xFF, 3, { {24, 24}, {0, 24}, {0, 16} } },  /* lower left block diagonal lower middle left to lower right */
    { 0x1FB3E, 0xFF, 3, { {12, 24}, {0, 24}, {0, 8} } },  /* lower left block diagonal upper middle left to lower centre */
    { 0x1FB3F, 0xFF, 3, { {24, 24}, {0, 24}, {0, 8} } },  /* lower left block diagonal upper middle left to lower right */
    { 0x1FB40, 0xFF, 3, { {0, 0}, {12, 24}, {0, 24} } },  /* lower left block diagonal upper left to lower centre */
    { 0x1FB41, 0xFF, 5, { {12, 0}, {24, 0}, {24, 24}, {0, 24}, {0, 8} } },  /* lower right block diagonal upper middle left to upper centre */
    { 0x1FB42, 0xFF, 4, { {24, 0}, {24, 24}, {0, 24}, {0, 8} } },  /* lower right block diagonal upper middle left to upper right */
    { 0x1FB43, 0xFF, 5, { {12, 0}, {24, 0}, {24, 24}, {0, 24}, {0, 16} } },  /* lower right block diagonal lower middle left to upper centre */
    { 0x1FB44, 0xFF, 4, { {24, 0}, {24, 24}, {0, 24}, {0, 16} } },  /* lower right block diagonal lower middle left to upper right */
    { 0x1FB45, 0xFF, 4, { {12, 0}, {24, 0}, {24, 24}, {0, 24} } },  /* lower right block diagonal lower left to upper centre */
    { 0x1FB46, 0xFF, 4, { {24, 8}, {24, 24}, {0, 24}, {0, 16} } },  /* lower right block diagonal lower middle left to upper middle right */
    { 0x1FB47, 0xFF, 3, { {24, 16}, {24, 24}, {12, 24} } },  /* lower right block diagonal lower centre to lower middle right */
    { 0x1FB48, 0xFF, 3, { {24, 16}, {24, 24}, {0, 24} } },  /* lower right block diagonal lower left to lower middle right */
    { 0x1FB49, 0xFF, 3, { {24, 8}, {24, 24}, {12, 24} } },  /* lower right block diagonal lower centre to upper middle right */
    { 0x1FB4A, 0xFF, 3, { {24, 8}, {24, 24}, {0, 24} } },  /* lower right block diagonal lower left to upper middle right */
    { 0x1FB4B, 0xFF, 3, { {24, 0}, {24, 24}, {12, 24} } },  /* lower right block diagonal lower centre to upper right */
    { 0x1FB4C, 0xFF, 5, { {0, 0}, {12, 0}, {24, 8}, {24, 24}, {0, 24} } },  /* lower left block diagonal upper centre to upper middle right */
    { 0x1FB4D, 0xFF, 4, { {0, 0}, {24, 8}, {24, 24}, {0, 24} } },  /* lower left block diagonal upper left to upper middle right */
    { 0x1FB4E, 0xFF, 5, { {0, 0}, {12, 0}, {24, 16}, {24, 24}, {0, 24} } },  /* lower left block diagonal upper centre to lower middle right */
    { 0x1FB4F, 0xFF, 4, { {0, 0}, {24, 16}, {24, 24}, {0, 24} } },  /* lower left block diagonal upper left to lower middle right */
    { 0x1FB50, 0xFF, 4, { {0, 0}, {12, 0}, {24, 24}, {0, 24} } },  /* lower left block diagonal upper centre to lower right */
    { 0x1FB51, 0xFF, 4, { {24, 16}, {24, 24}, {0, 24}, {0, 8} } },  /* lower left block diagonal upper middle left to lower middle right */
    { 0x1FB52, 0xFF, 5, { {0, 0}, {24, 0}, {24, 24}, {12, 24}, {0, 16} } },  /* upper right block diagonal lower middle left to lower centre */
    { 0x1FB53, 0xFF, 4, { {0, 0}, {24, 0}, {24, 24}, {0, 16} } },  /* upper right block diagonal lower middle left to lower right */
    { 0x1FB54, 0xFF, 5, { {0, 0}, {24, 0}, {24, 24}, {12, 24}, {0, 8} } },  /* upper right block diagonal upper middle left to lower centre */
    { 0x1FB55, 0xFF, 4, { {0, 0}, {24, 0}, {24, 24}, {0, 8} } },  /* upper right block diagonal upper middle left to lower right */
    { 0x1FB56, 0xFF, 4, { {0, 0}, {24, 0}, {24, 24}, {12, 24} } },  /* upper right block diagonal upper left to lower centre */
    { 0x1FB57, 0xFF, 3, { {0, 0}, {12, 0}, {0, 8} } },  /* upper left block diagonal upper middle left to upper centre */
    { 0x1FB58, 0xFF, 3, { {0, 0}, {24, 0}, {0, 8} } },  /* upper left block diagonal upper middle left to upper right */
    { 0x1FB59, 0xFF, 3, { {0, 0}, {12, 0}, {0, 16} } },  /* upper left block diagonal lower middle left to upper centre */
    { 0x1FB5A, 0xFF, 3, { {0, 0}, {24, 0}, {0, 16} } },  /* upper left block diagonal lower middle left to upper right */
    { 0x1FB5B, 0xFF, 3, { {0, 0}, {12, 0}, {0, 24} } },  /* upper left block diagonal lower left to upper centre */
    { 0x1FB5C, 0xFF, 4, { {0, 0}, {24, 0}, {24, 8}, {0, 16} } },  /* upper left block diagonal lower middle left to upper middle right */
    { 0x1FB5D, 0xFF, 5, { {0, 0}, {24, 0}, {24, 16}, {12, 24}, {0, 24} } },  /* upper left block diagonal lower centre to lower middle right */
    { 0x1FB5E, 0xFF, 4, { {0, 0}, {24, 0}, {24, 16}, {0, 24} } },  /* upper left block diagonal lower left to lower middle right */
    { 0x1FB5F, 0xFF, 5, { {0, 0}, {24, 0}, {24, 8}, {12, 24}, {0, 24} } },  /* upper left block diagonal lower centre to upper middle right */
    { 0x1FB60, 0xFF, 4, { {0, 0}, {24, 0}, {24, 8}, {0, 24} } },  /* upper left block diagonal lower left to upper middle right */
    { 0x1FB61, 0xFF, 4, { {0, 0}, {24, 0}, {12, 24}, {0, 24} } },  /* upper left block diagonal lower centre to upper right */
    { 0x1FB62, 0xFF, 3, { {12, 0}, {24, 0}, {24, 8} } },  /* upper right block diagonal upper centre to upper middle right */
    { 0x1FB63, 0xFF, 3, { {0, 0}, {24, 0}, {24, 8} } },  /* upper right block diagonal upper left to upper middle right */
    { 0x1FB64, 0xFF, 3, { {12, 0}, {24, 0}, {24, 16} } },  /* upper right block diagonal upper centre to lower middle right */
    { 0x1FB65, 0xFF, 3, { {0, 0}, {24, 0}, {24, 16} } },  /* upper right block diagonal upper left to lower middle right */
    { 0x1FB66, 0xFF, 3, { {12, 0}, {24, 0}, {24, 24} } },  /* upper right block diagonal upper centre to lower right */
    { 0x1FB67, 0xFF, 4, { {0, 0}, {24, 0}, {24, 16}, {0, 8} } },  /* upper right block diagonal upper middle left to lower middle right */
    { 0x1FB68, 0xFF, 5, { {0, 0}, {24, 0}, {24, 24}, {0, 24}, {12, 12} } },  /* upper and right and lower triangular three quarters block */
    { 0x1FB69, 0xFF, 5, { {0, 0}, {12, 12}, {24, 0}, {24, 24}, {0, 24} } },  /* left and lower and right triangular three quarters block */
    { 0x1FB6A, 0xFF, 5, { {0, 0}, {24, 0}, {12, 12}, {24, 24}, {0, 24} } },  /* upper and left and lower triangular three quarters block */
    { 0x1FB6B, 0xFF, 5, { {0, 0}, {24, 0}, {24, 24}, {12, 12}, {0, 24} } },  /* left and upper and right triangular three quarters block */
    { 0x1FB6C, 0xFF, 3, { {0, 0}, {12, 12}, {0, 24} } },  /* left triangular one quarter block */
    { 0x1FB6D, 0xFF, 3, { {0, 0}, {24, 0}, {12, 12} } },  /* upper triangular one quarter block */
    { 0x1FB6E, 0xFF, 3, { {24, 0}, {24, 24}, {12, 12} } },  /* right triangular one quarter block */
    { 0x1FB6F, 0xFF, 3, { {0, 24}, {12, 12}, {24, 24} } },  /* lower triangular one quarter block */
    { 0x1FB9A, 0xFF, 6, { {0, 0}, {24, 0}, {12, 12}, {24, 24}, {0, 24}, {12, 12} } },  /* upper and lower triangular half block */
    { 0x1FB9B, 0xFF, 6, { {0, 0}, {12, 12}, {24, 0}, {24, 24}, {12, 12}, {0, 24} } },  /* left and right triangular half block */
    { 0x1FB9C, 0x80, 3, { {0, 0}, {24, 0}, {0, 24} } },  /* upper left triangular medium shade */
    { 0x1FB9D, 0x80, 3, { {0, 0}, {24, 0}, {24, 24} } },  /* upper right triangular medium shade */
    { 0x1FB9E, 0x80, 3, { {24, 0}, {24, 24}, {0, 24} } },  /* lower right triangular medium shade */
    { 0x1FB9F, 0x80, 3, { {0, 0}, {24, 24}, {0, 24} } },  /* lower left triangular medium shade */
    { 0x0E0B8, 0xFF, 3, { {0, 0}, {24, 24}, {0, 24} } },  /* powerline lower left triangle */
    { 0x0E0BA, 0xFF, 3, { {24, 0}, {24, 24}, {0, 24} } },  /* powerline lower right triangle */
    { 0x0E0BC, 0xFF, 3, { {0, 0}, {24, 0}, {0, 24} } },  /* powerline upper left triangle */
    { 0x0E0BE, 0xFF, 3, { {0, 0}, {24, 0}, {24, 24} } },  /* powerline upper right triangle */
};

/* Segments: x0 | y0 << 2 | x1 << 4 | y1 << 6, where 0 is the left or top edge,
 * 1 the light stroke's centre line and 2 the right or bottom edge. */
static const SprLines spr_lines[] = {
    { 0x02571, 1, { 0x82 } },  /* box drawings light diagonal upper right to lower left */
    { 0x02572, 1, { 0xA0 } },  /* box drawings light diagonal upper left to lower right */
    { 0x02573, 2, { 0xA0, 0x82 } },  /* box drawings light diagonal cross */
    { 0x1FBA0, 1, { 0x41 } },  /* box drawings light diagonal upper centre to middle left */
    { 0x1FBA1, 1, { 0x61 } },  /* box drawings light diagonal upper centre to middle right */
    { 0x1FBA2, 1, { 0x94 } },  /* box drawings light diagonal middle left to lower centre */
    { 0x1FBA3, 1, { 0x96 } },  /* box drawings light diagonal middle right to lower centre */
    { 0x1FBA4, 2, { 0x41, 0x94 } },  /* box drawings light diagonal upper centre to middle left to lower centre */
    { 0x1FBA5, 2, { 0x61, 0x96 } },  /* box drawings light diagonal upper centre to middle right to lower centre */
    { 0x1FBA6, 2, { 0x94, 0x69 } },  /* box drawings light diagonal middle left to lower centre to middle right */
    { 0x1FBA7, 2, { 0x14, 0x61 } },  /* box drawings light diagonal middle left to upper centre to middle right */
    { 0x1FBA8, 2, { 0x41, 0x96 } },  /* box drawings light diagonal upper centre to middle left and middle right to lower centre */
    { 0x1FBA9, 2, { 0x61, 0x94 } },  /* box drawings light diagonal upper centre to middle right and middle left to lower centre */
    { 0x1FBAA, 3, { 0x61, 0x96, 0x49 } },  /* box drawings light diagonal upper centre to middle right to lower centre to middle left */
    { 0x1FBAB, 3, { 0x41, 0x94, 0x69 } },  /* box drawings light diagonal upper centre to middle left to lower centre to middle right */
    { 0x1FBAC, 3, { 0x14, 0x61, 0x96 } },  /* box drawings light diagonal middle left to upper centre to middle right to lower centre */
    { 0x1FBAD, 3, { 0x16, 0x41, 0x94 } },  /* box drawings light diagonal middle right to upper centre to middle left to lower centre */
    { 0x1FBAE, 4, { 0x61, 0x96, 0x49, 0x14 } },  /* box drawings light diagonal diamond */
    { 0x1FBD0, 1, { 0x86 } },  /* box drawings light diagonal middle right to lower left */
    { 0x1FBD1, 1, { 0x42 } },  /* box drawings light diagonal upper right to middle left */
    { 0x1FBD2, 1, { 0x60 } },  /* box drawings light diagonal upper left to middle right */
    { 0x1FBD3, 1, { 0xA4 } },  /* box drawings light diagonal middle left to lower right */
    { 0x1FBD4, 1, { 0x90 } },  /* box drawings light diagonal upper left to lower centre */
    { 0x1FBD5, 1, { 0xA1 } },  /* box drawings light diagonal upper centre to lower right */
    { 0x1FBD6, 1, { 0x92 } },  /* box drawings light diagonal upper right to lower centre */
    { 0x1FBD7, 1, { 0x81 } },  /* box drawings light diagonal upper centre to lower left */
    { 0x1FBD8, 2, { 0x50, 0x25 } },  /* box drawings light diagonal upper left to middle centre to upper right */
    { 0x1FBD9, 2, { 0x52, 0xA5 } },  /* box drawings light diagonal upper right to middle centre to lower right */
    { 0x1FBDA, 2, { 0x58, 0xA5 } },  /* box drawings light diagonal lower left to middle centre to lower right */
    { 0x1FBDB, 2, { 0x50, 0x85 } },  /* box drawings light diagonal upper left to middle centre to lower left */
    { 0x1FBDC, 2, { 0x90, 0x29 } },  /* box drawings light diagonal upper left to lower centre to upper right */
    { 0x1FBDD, 2, { 0x42, 0xA4 } },  /* box drawings light diagonal upper right to middle left to lower right */
    { 0x1FBDE, 2, { 0x18, 0xA1 } },  /* box drawings light diagonal lower left to upper centre to lower right */
    { 0x1FBDF, 2, { 0x60, 0x86 } },  /* box drawings light diagonal upper left to middle right to lower left */
    { 0x0E0B9, 1, { 0xA0 } },  /* powerline backslash separator */
    { 0x0E0BB, 1, { 0x28 } },  /* powerline forward slash separator */
    { 0x0E0BD, 1, { 0x28 } },  /* powerline forward slash separator */
    { 0x0E0BF, 1, { 0xA0 } },  /* powerline backslash separator */
};

/* ---- End of generated tables ---- */

/* --------------------------------------------------------------------------
 * Geometry shared by every glyph
 * ------------------------------------------------------------------------ */

/* The light stroke width used for a cw x ch cell: `line`, but never so wide
 * that a double line (two strokes and the gap between them, 3 * line) loses
 * the free pixel on each side that keeps ║║ two pairs instead of one block.
 * Depends on the cell and `line` only, so every glyph of one cell size gets
 * the same width. */
static inline i32 sprite_draw_line_width(i32 cw, i32 ch, i32 line) {
    i32 m = ((cw < ch ? cw : ch) - 2) / 3;
    if (line > m) line = m;
    if (line < 1) line = 1;
    return line;
}

/* Split point k/24 of a length, rounded half up. Every block, quadrant,
 * sextant, octant and checker boundary comes from here, so complementary
 * glyphs tile pixel for pixel: the half split of a left half block is the
 * same column as a quadrant's and a sextant's. */
static inline i32 spr_split(i32 len, i32 k) {
    return (2 * len * k + 24) / 48;
}

/* First pixel of a band `w` wide centred across `len` pixels. Every straight
 * stroke is placed by this, so light, heavy and double strokes share one axis
 * and a stroke ends at the same rows (columns) the next cell's begins. */
static inline i32 spr_band(i32 len, i32 w) {
    return (len - w) / 2;
}

/* Width of a stroke of one weight; a double stroke is two light strokes with
 * a light stroke's gap between them. */
static inline i32 spr_weight_w(i32 weight, i32 line) {
    switch (weight) {
    case SPR_LIGHT:  return line;
    case SPR_HEAVY:  return 2 * line;
    case SPR_DOUBLE: return 3 * line;
    default:         return 0;
    }
}

/* Fill [x0,x1) x [y0,y1) with `v`, clipped to the cell, merged by max so
 * overlapping parts never darken or lighten each other. */
static void spr_fill(u8 *buf, i32 cw, i32 ch, i32 x0, i32 y0, i32 x1, i32 y1, u8 v) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cw) x1 = cw;
    if (y1 > ch) y1 = ch;
    for (i32 y = y0; y < y1; y++) {
        u8 *row = buf + (usize)y * (usize)cw;
        for (i32 x = x0; x < x1; x++)
            if (row[x] < v) row[x] = v;
    }
}

typedef bool (*SprInside)(const void *shape, f32 x, f32 y);

/* Coverage of a shape given as an inside test, ss x ss samples per pixel at
 * the sample centres, scaled to `level` and merged by max. */
static void spr_raster(u8 *buf, i32 cw, i32 ch, i32 ss, SprInside inside,
                       const void *shape, u8 level) {
    const f32 inv = 1.0f / (f32)ss;
    const i32 n = ss * ss;
    for (i32 py = 0; py < ch; py++) {
        for (i32 px = 0; px < cw; px++) {
            i32 hits = 0;
            for (i32 sy = 0; sy < ss; sy++)
                for (i32 sx = 0; sx < ss; sx++)
                    if (inside(shape, (f32)px + ((f32)sx + 0.5f) * inv,
                                      (f32)py + ((f32)sy + 0.5f) * inv))
                        hits++;
            if (!hits) continue;
            u32 v = (u32)((hits * (i32)level + n / 2) / n);
            u8 *p = buf + (usize)py * (usize)cw + (usize)px;
            if (v > *p) *p = (u8)v;
        }
    }
}

/* --------------------------------------------------------------------------
 * Box drawing U+2500-257F
 * ------------------------------------------------------------------------ */

/* Straight arms. Each arm runs from the cell edge across the centre far
 * enough to close the joint with the strokes it meets: a horizontal arm spans
 * the widest vertical stroke, a vertical arm the widest horizontal one, so a
 * corner has a square outer edge and a junction no notch.
 *
 * Double strokes are two light rails. Where a double arm meets another double
 * arm, the rail on the OUTSIDE of the turn runs to the far rail of the other
 * arm and the rail on the inside stops at the near one, so the outer and the
 * inner strokes each form one continuous line (╔ is two nested corners, ╬
 * four separate ones). A single arm meeting double rails crosses both, except
 * at a T where it ends on the rail it touches (╟, ╤). */
static void spr_box_arms_draw(u8 *buf, i32 cw, i32 ch, i32 L, u8 arms) {
    const i32 up = arms & 3, rt = (arms >> 2) & 3, dn = (arms >> 4) & 3, lf = (arms >> 6) & 3;
    const i32 D = 3 * L;
    const i32 dx = spr_band(cw, D);   /* left rail of a double vertical */
    const i32 dy = spr_band(ch, D);   /* top rail of a double horizontal */
    const i32 wv = MAX(spr_weight_w(up, L), spr_weight_w(dn, L));
    const i32 wh = MAX(spr_weight_w(lf, L), spr_weight_w(rt, L));
    const bool vdouble = up == SPR_DOUBLE || dn == SPR_DOUBLE;
    const bool hdouble = lf == SPR_DOUBLE || rt == SPR_DOUBLE;
    /* The stroke a horizontal arm has to cross: the vertical one, or for a
     * lone horizontal line, the widest of itself (half lines meet there). */
    const i32 xw = wv ? wv : wh, yw = wh ? wh : wv;
    const i32 xc = spr_band(cw, xw), yc = spr_band(ch, yw);

    if (rt == SPR_DOUBLE) {
        i32 top0 = xc, bot0 = xc;
        if (vdouble) {
            top0 = up == SPR_DOUBLE ? dx + 2 * L : dx;
            bot0 = dn == SPR_DOUBLE ? dx + 2 * L : dx;
        }
        spr_fill(buf, cw, ch, top0, dy, cw, dy + L, 255);
        spr_fill(buf, cw, ch, bot0, dy + 2 * L, cw, dy + D, 255);
    } else if (rt) {
        i32 w = spr_weight_w(rt, L), y = spr_band(ch, w);
        i32 x0 = xc;
        if (vdouble) x0 = (up == SPR_DOUBLE && dn == SPR_DOUBLE && !lf) ? dx + 2 * L : dx;
        spr_fill(buf, cw, ch, x0, y, cw, y + w, 255);
    }

    if (lf == SPR_DOUBLE) {
        i32 top1 = xc + xw, bot1 = xc + xw;
        if (vdouble) {
            top1 = up == SPR_DOUBLE ? dx + L : dx + D;
            bot1 = dn == SPR_DOUBLE ? dx + L : dx + D;
        }
        spr_fill(buf, cw, ch, 0, dy, top1, dy + L, 255);
        spr_fill(buf, cw, ch, 0, dy + 2 * L, bot1, dy + D, 255);
    } else if (lf) {
        i32 w = spr_weight_w(lf, L), y = spr_band(ch, w);
        i32 x1 = xc + xw;
        if (vdouble) x1 = (up == SPR_DOUBLE && dn == SPR_DOUBLE && !rt) ? dx + L : dx + D;
        spr_fill(buf, cw, ch, 0, y, x1, y + w, 255);
    }

    if (dn == SPR_DOUBLE) {
        i32 left0 = yc, right0 = yc;
        if (hdouble) {
            left0  = lf == SPR_DOUBLE ? dy + 2 * L : dy;
            right0 = rt == SPR_DOUBLE ? dy + 2 * L : dy;
        }
        spr_fill(buf, cw, ch, dx, left0, dx + L, ch, 255);
        spr_fill(buf, cw, ch, dx + 2 * L, right0, dx + D, ch, 255);
    } else if (dn) {
        i32 w = spr_weight_w(dn, L), x = spr_band(cw, w);
        i32 y0 = yc;
        if (hdouble) y0 = (lf == SPR_DOUBLE && rt == SPR_DOUBLE && !up) ? dy + 2 * L : dy;
        spr_fill(buf, cw, ch, x, y0, x + w, ch, 255);
    }

    if (up == SPR_DOUBLE) {
        i32 left1 = yc + yw, right1 = yc + yw;
        if (hdouble) {
            left1  = lf == SPR_DOUBLE ? dy + L : dy + D;
            right1 = rt == SPR_DOUBLE ? dy + L : dy + D;
        }
        spr_fill(buf, cw, ch, dx, 0, dx + L, left1, 255);
        spr_fill(buf, cw, ch, dx + 2 * L, 0, dx + D, right1, 255);
    } else if (up) {
        i32 w = spr_weight_w(up, L), x = spr_band(cw, w);
        i32 y1 = yc + yw;
        if (hdouble) y1 = (lf == SPR_DOUBLE && rt == SPR_DOUBLE && !dn) ? dy + L : dy + D;
        spr_fill(buf, cw, ch, x, 0, x + w, y1, 255);
    }
}

/* Dashed lines: `n` dashes on the straight line's own band. The cell is cut
 * into n equal segments and every segment loses the same gap, half at each
 * end, so the gap between two cells equals the gap inside one and a run of
 * cells reads as one regular dash pattern. */
static void spr_dashes(u8 *buf, i32 cw, i32 ch, i32 L, u8 arms, i32 n) {
    const bool horiz = (arms & 0x0C) != 0;
    const i32 weight = horiz ? (arms >> 2) & 3 : arms & 3;
    const i32 w = spr_weight_w(weight, L);
    const i32 len = horiz ? cw : ch;
    const i32 across = spr_band(horiz ? ch : cw, w);
    i32 gap = (2 * len + 3 * n) / (6 * n);          /* a third of a segment */
    if (gap < 1) gap = 1;
    if (gap > len / n - 1) gap = len / n - 1;       /* every dash keeps a pixel */
    if (gap < 0) gap = 0;
    for (i32 i = 0; i < n; i++) {
        i32 a = (2 * len * i + n) / (2 * n) + gap / 2;
        i32 b = (2 * len * (i + 1) + n) / (2 * n) - (gap - gap / 2);
        if (horiz) spr_fill(buf, cw, ch, a, across, b, across + w, 255);
        else       spr_fill(buf, cw, ch, across, a, across + w, b, 255);
    }
}

/* Rounded corners ╭╮╯╰: a quarter circle on the light stroke's centre lines,
 * continued straight to the two cell edges. The radius leaves at least one
 * pixel of straight stroke before each edge, so the arc enters the next cell
 * at exactly the rows (columns) and thickness of a straight light arm. All
 * four corners use one radius, the smallest the cell allows on any side. */
typedef struct { f32 cx, cy, r, half, sx, sy; } SprArc;

static bool spr_arc_inside(const void *shape, f32 x, f32 y) {
    const SprArc *a = (const SprArc *)shape;
    /* Distances from the two centre lines, positive toward the arms. */
    f32 u = (x - a->cx) * a->sx, v = (y - a->cy) * a->sy;
    f32 d;
    if (u >= a->r)      d = fabsf(v);                   /* straight horizontal part */
    else if (v >= a->r) d = fabsf(u);                   /* straight vertical part */
    else {
        f32 du = u - a->r, dv = v - a->r;
        d = fabsf(sqrtf(du * du + dv * dv) - a->r);
    }
    return d < a->half;
}

static void spr_arc(u8 *buf, i32 cw, i32 ch, i32 L, u8 arms) {
    SprArc a;
    a.cx = (f32)spr_band(cw, L) + 0.5f * (f32)L;
    a.cy = (f32)spr_band(ch, L) + 0.5f * (f32)L;
    a.half = 0.5f * (f32)L;
    a.sx = (arms & 0x0C) ? 1.0f : -1.0f;      /* right arm: the arc opens to +x */
    a.sy = (arms & 0x30) ? 1.0f : -1.0f;      /* down arm: the arc opens to +y */
    f32 r = a.cx;
    if ((f32)cw - a.cx < r) r = (f32)cw - a.cx;
    if (a.cy < r) r = a.cy;
    if ((f32)ch - a.cy < r) r = (f32)ch - a.cy;
    r -= 1.0f;
    if (r < a.half) r = a.half;
    a.r = r;
    spr_raster(buf, cw, ch, SPR_SS, spr_arc_inside, &a, 255);
}

/* --------------------------------------------------------------------------
 * Diagonal lines: ╱╲╳, the light diagonals of Legacy Computing, the
 * powerline slashes. A segment endpoint is a cell edge or the light stroke's
 * centre line, so a diagonal that ends mid-edge meets a straight line there.
 * ------------------------------------------------------------------------ */

typedef struct { i32 n; f32 ax[4], ay[4], bx[4], by[4]; f32 half; } SprSegs;

static bool spr_segs_inside(const void *shape, f32 x, f32 y) {
    const SprSegs *s = (const SprSegs *)shape;
    for (i32 i = 0; i < s->n; i++) {
        f32 dx = s->bx[i] - s->ax[i], dy = s->by[i] - s->ay[i];
        f32 ll = dx * dx + dy * dy;
        f32 t = ll > 0.0f ? ((x - s->ax[i]) * dx + (y - s->ay[i]) * dy) / ll : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        f32 ex = x - (s->ax[i] + t * dx), ey = y - (s->ay[i] + t * dy);
        if (ex * ex + ey * ey < s->half * s->half) return true;
    }
    return false;
}

static void spr_lines_draw(u8 *buf, i32 cw, i32 ch, i32 L, const SprLines *l) {
    const f32 xs[3] = { 0.0f, (f32)spr_band(cw, L) + 0.5f * (f32)L, (f32)cw };
    const f32 ys[3] = { 0.0f, (f32)spr_band(ch, L) + 0.5f * (f32)L, (f32)ch };
    SprSegs s;
    s.n = l->n;
    s.half = 0.5f * (f32)L;
    for (i32 i = 0; i < l->n; i++) {
        u8 e = l->seg[i];
        s.ax[i] = xs[e & 3];        s.ay[i] = ys[(e >> 2) & 3];
        s.bx[i] = xs[(e >> 4) & 3]; s.by[i] = ys[(e >> 6) & 3];
    }
    spr_raster(buf, cw, ch, SPR_SS, spr_segs_inside, &s, 255);
}

/* --------------------------------------------------------------------------
 * Polygons: the smooth mosaics, triangular blocks and powerline triangles.
 * Vertices are split points of the cell, so an edge along the cell border is
 * exact and a wedge's corner lands on the sextant grid's rows.
 * ------------------------------------------------------------------------ */

typedef struct { i32 n; f32 x[6], y[6]; } SprPolyPx;

/* Even-odd crossing test. The intersection is computed from the edge's lower
 * end whichever way the polygon walks it, so two polygons that share an edge
 * (a wedge and its complement) split every sample on it exactly once. */
static bool spr_poly_inside(const void *shape, f32 x, f32 y) {
    const SprPolyPx *p = (const SprPolyPx *)shape;
    bool in = false;
    for (i32 i = 0, j = p->n - 1; i < p->n; j = i++) {
        f32 x0 = p->x[i], y0 = p->y[i], x1 = p->x[j], y1 = p->y[j];
        if ((y0 > y) == (y1 > y)) continue;
        if (y0 > y1) { f32 t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
        f32 xint = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
        if (x < xint) in = !in;
    }
    return in;
}

static void spr_poly_draw(u8 *buf, i32 cw, i32 ch, const SprPoly *poly) {
    SprPolyPx p;
    p.n = poly->n;
    for (i32 i = 0; i < poly->n; i++) {
        p.x[i] = (f32)spr_split(cw, poly->pt[i][0]);
        p.y[i] = (f32)spr_split(ch, poly->pt[i][1]);
    }
    spr_raster(buf, cw, ch, SPR_SS, spr_poly_inside, &p, poly->level);
}

/* --------------------------------------------------------------------------
 * Blocks: rectangles, mosaics, checkers
 * ------------------------------------------------------------------------ */

static void spr_rects_draw(u8 *buf, i32 cw, i32 ch, const SprRects *r) {
    for (i32 i = 0; i < r->n; i++)
        spr_fill(buf, cw, ch, spr_split(cw, r->r[i][0]), spr_split(ch, r->r[i][1]),
                 spr_split(cw, r->r[i][2]), spr_split(ch, r->r[i][3]), r->r[i][4]);
}

/* A 2-column mosaic of `rows` rows (sextants 3, octants 4); bit k is cell k,
 * numbered left to right, top to bottom. */
static void spr_mosaic(u8 *buf, i32 cw, i32 ch, u32 mask, i32 rows) {
    const i32 step = 24 / rows;
    for (i32 k = 0; k < 2 * rows; k++) {
        if (!(mask & (1u << k))) continue;
        i32 col = k & 1, row = k >> 1;
        spr_fill(buf, cw, ch, spr_split(cw, 12 * col), spr_split(ch, step * row),
                 spr_split(cw, 12 * col + 12), spr_split(ch, step * row + step), 255);
    }
}

/* Checkerboard of 4 x 4 squares of the CELL, not of pixels: four is even, so
 * the next cell continues the pattern with no seam. `phase` 0 fills the
 * top-left square. */
static void spr_checker(u8 *buf, i32 cw, i32 ch, i32 phase) {
    for (i32 j = 0; j < 4; j++)
        for (i32 i = 0; i < 4; i++)
            if (((i + j) & 1) == phase)
                spr_fill(buf, cw, ch, spr_split(cw, 6 * i), spr_split(ch, 6 * j),
                         spr_split(cw, 6 * i + 6), spr_split(ch, 6 * j + 6), 255);
}

/* --------------------------------------------------------------------------
 * Braille U+2800-28FF
 * ------------------------------------------------------------------------ */

static void spr_braille(u8 *buf, i32 cw, i32 ch, u32 cp) {
    u32 bits = cp - 0x2800;
    if (bits == 0) return;                   /* U+2800 is a blank, and IS a glyph */

    /* The 8 dots sit on a 2x4 grid. Unicode's bit order is column-major for
     * dots 1-6 and then appends 7 and 8 as the bottom row — that irregularity
     * is in the standard, not a mistake here:
     *
     *     bit 0 -> (0,0)   bit 3 -> (1,0)
     *     bit 1 -> (0,1)   bit 4 -> (1,1)
     *     bit 2 -> (0,2)   bit 5 -> (1,2)
     *     bit 6 -> (0,3)   bit 7 -> (1,3)
     */
    f32 sub_w = (f32)cw * 0.5f;
    f32 sub_h = (f32)ch * 0.25f;
    /* Radius large enough that a full cell (U+28FF) reads as solid rather than
     * as eight separate specks — that is the shape plotting libraries rely on
     * — but short of touching, so a sparse pattern still reads as dots. */
    f32 rad = (sub_w < sub_h ? sub_w : sub_h) * 0.42f;
    if (rad < 0.75f) rad = 0.75f;

    const f32 inv = 1.0f / (f32)SPR_SS_DOTS;
    const i32 n = SPR_SS_DOTS * SPR_SS_DOTS;
    for (i32 b = 0; b < 8; b++) {
        if (!(bits & (1u << b))) continue;
        i32 dcol = (b == 6) ? 0 : (b == 7) ? 1 : (b / 3);
        i32 drow = (b == 6 || b == 7) ? 3 : (b % 3);
        f32 ccx = ((f32)dcol + 0.5f) * sub_w;
        f32 ccy = ((f32)drow + 0.5f) * sub_h;

        i32 x0 = (i32)floorf(ccx - rad - 1.0f), x1 = (i32)ceilf(ccx + rad + 1.0f);
        i32 y0 = (i32)floorf(ccy - rad - 1.0f), y1 = (i32)ceilf(ccy + rad + 1.0f);
        if (x0 < 0) x0 = 0;
        if (x1 > cw) x1 = cw;
        if (y0 < 0) y0 = 0;
        if (y1 > ch) y1 = ch;

        for (i32 py = y0; py < y1; py++) {
            for (i32 px = x0; px < x1; px++) {
                i32 hits = 0;
                for (i32 sy = 0; sy < SPR_SS_DOTS; sy++) {
                    for (i32 sx = 0; sx < SPR_SS_DOTS; sx++) {
                        f32 x = (f32)px + ((f32)sx + 0.5f) * inv - ccx;
                        f32 y = (f32)py + ((f32)sy + 0.5f) * inv - ccy;
                        if (x * x + y * y <= rad * rad) hits++;
                    }
                }
                if (!hits) continue;
                u32 v = (u32)((hits * 255 + n / 2) / n);
                /* Dots never overlap at this radius, but take the max anyway so
                 * a future radius change cannot darken a seam into a blob. */
                u8 *p = buf + (usize)py * (usize)cw + (usize)px;
                if (v > *p) *p = (u8)v;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Powerline separators U+E0B0-E0B7 (E0B8-E0BF are polygons and lines above)
 * ------------------------------------------------------------------------ */

typedef enum {
    SPR_PL_TRI_RIGHT,      /* U+E0B0 filled triangle pointing right */
    SPR_PL_CHEV_RIGHT,     /* U+E0B1 open chevron pointing right */
    SPR_PL_TRI_LEFT,       /* U+E0B2 */
    SPR_PL_CHEV_LEFT,      /* U+E0B3 */
    SPR_PL_ARC_RIGHT,      /* U+E0B4 filled half-ellipse bulging right */
    SPR_PL_ARCLINE_RIGHT,  /* U+E0B5 its outline */
    SPR_PL_ARC_LEFT,       /* U+E0B6 */
    SPR_PL_ARCLINE_LEFT,   /* U+E0B7 */
} SprPlShape;

typedef struct { SprPlShape sh; f32 cw, ch, t; } SprPl;

/* Is the sample point (x,y), in pixel units inside a cw x ch cell, part of the
 * shape? `t` is the stroke half-width for the open variants. */
static bool spr_pl_inside(const void *shape, f32 x, f32 y) {
    const SprPl *p = (const SprPl *)shape;
    const f32 cw = p->cw, t = p->t;
    const f32 hy = p->ch * 0.5f;
    switch (p->sh) {
    case SPR_PL_TRI_RIGHT:
        /* Apex at (cw, hy); the two edges run from the left corners. The test
         * is "within the wedge", written so a zero-height cell cannot divide. */
        return cw > 0.0f && fabsf(y - hy) <= hy * (1.0f - x / cw);
    case SPR_PL_TRI_LEFT:
        return cw > 0.0f && fabsf(y - hy) <= hy * (x / cw);
    case SPR_PL_CHEV_RIGHT: {
        if (cw <= 0.0f) return false;
        f32 d = fabsf(y - hy) - hy * (1.0f - x / cw);
        /* Perpendicular-ish distance: scale the vertical gap by the edge slope
         * so the stroke keeps its width along a diagonal instead of thinning. */
        f32 k = sqrtf(1.0f + (hy / cw) * (hy / cw));
        return fabsf(d) * (1.0f / k) <= t;
    }
    case SPR_PL_CHEV_LEFT: {
        if (cw <= 0.0f) return false;
        f32 d = fabsf(y - hy) - hy * (x / cw);
        f32 k = sqrtf(1.0f + (hy / cw) * (hy / cw));
        return fabsf(d) * (1.0f / k) <= t;
    }
    case SPR_PL_ARC_RIGHT: {
        /* Half-ellipse, flat edge on the left, centred at (0, hy). */
        if (cw <= 0.0f || hy <= 0.0f) return false;
        f32 u = x / cw, v = (y - hy) / hy;
        return u * u + v * v <= 1.0f;
    }
    case SPR_PL_ARC_LEFT: {
        if (cw <= 0.0f || hy <= 0.0f) return false;
        f32 u = (cw - x) / cw, v = (y - hy) / hy;
        return u * u + v * v <= 1.0f;
    }
    case SPR_PL_ARCLINE_RIGHT:
    case SPR_PL_ARCLINE_LEFT: {
        if (cw <= 0.0f || hy <= 0.0f) return false;
        f32 u = (p->sh == SPR_PL_ARCLINE_RIGHT ? x : (cw - x)) / cw;
        f32 v = (y - hy) / hy;
        f32 rr = sqrtf(u * u + v * v);
        /* Band around r = 1. The band's width in normalised space has to be
         * converted back to pixels along the shorter axis, or the stroke is
         * several pixels thick at the top and hairline at the side. */
        f32 scale = (cw < hy ? cw : hy);
        return fabsf(rr - 1.0f) * scale <= t;
    }
    }
    return false;
}

static void spr_powerline(u8 *buf, i32 cw, i32 ch, i32 L, u32 cp) {
    /* The open variants are stroked at the box-drawing light width. */
    SprPl p = { (SprPlShape)(cp - 0xE0B0), (f32)cw, (f32)ch, 0.5f * (f32)L };
    spr_raster(buf, cw, ch, SPR_SS_DOTS, spr_pl_inside, &p, 255);
}

/* --------------------------------------------------------------------------
 * Table lookups and the entry points
 * ------------------------------------------------------------------------ */

static const SprRects *spr_find_rects(u32 cp) {
    for (usize i = 0; i < ARRAY_LEN(spr_rects); i++)
        if (spr_rects[i].cp == cp) return &spr_rects[i];
    return NULL;
}

static const SprPoly *spr_find_poly(u32 cp) {
    for (usize i = 0; i < ARRAY_LEN(spr_polys); i++)
        if (spr_polys[i].cp == cp) return &spr_polys[i];
    return NULL;
}

static const SprLines *spr_find_lines(u32 cp) {
    for (usize i = 0; i < ARRAY_LEN(spr_lines); i++)
        if (spr_lines[i].cp == cp) return &spr_lines[i];
    return NULL;
}

static void spr_box(u8 *buf, i32 cw, i32 ch, i32 L, u32 cp) {
    const u8 arms = spr_box_arms[cp - 0x2500];
    if (cp >= 0x2504 && cp <= 0x250B) {
        spr_dashes(buf, cw, ch, L, arms, cp <= 0x2507 ? 3 : 4);
    } else if (cp >= 0x254C && cp <= 0x254F) {
        spr_dashes(buf, cw, ch, L, arms, 2);
    } else if (cp >= 0x256D && cp <= 0x2570) {
        spr_arc(buf, cw, ch, L, arms);
    } else if (cp >= 0x2571 && cp <= 0x2573) {
        const SprLines *l = spr_find_lines(cp);
        if (l) spr_lines_draw(buf, cw, ch, L, l);
    } else {
        spr_box_arms_draw(buf, cw, ch, L, arms);
    }
}

/* True when sprite_draw draws cp. */
static bool sprite_draw_covers(u32 cp) {
    if (cp >= 0x2500 && cp <= 0x259F) return true;     /* box drawing, block elements */
    if (cp >= 0x2800 && cp <= 0x28FF) return true;     /* braille */
    if (cp >= 0xE0B0 && cp <= 0xE0BF) return true;     /* powerline separators */
    if (cp >= 0x1FB00 && cp <= 0x1FB97) return cp != 0x1FB93;  /* sextants .. fills */
    if (cp >= 0x1FB9A && cp <= 0x1FBAE) return true;   /* triangular halves, diagonals */
    if (cp >= 0x1FBCE && cp <= 0x1FBDF) return true;   /* thirds, diagonals */
    if (cp >= 0x1FBE4 && cp <= 0x1FBE7) return true;   /* centred quarter blocks */
    if (cp >= 0x1CD00 && cp <= 0x1CDE5) return true;   /* octants */
    if (cp >= 0x1CE90 && cp <= 0x1CEAF) return true;   /* sixteenths, quarter bars */
    return false;
}

/* Draws cp into buf, an R8 coverage bitmap of cw x ch (stride cw), which it
 * clears first. `line` is the light stroke width in pixels (>= 1; heavy is
 * 2*line), narrowed by sprite_draw_line_width() on a cell too small for a
 * double line. Returns false, and leaves buf alone, when cp is not covered. */
static bool sprite_draw(u32 cp, u8 *buf, i32 cw, i32 ch, i32 line) {
    if (!buf || cw <= 0 || ch <= 0 || !sprite_draw_covers(cp)) return false;
    memset(buf, 0, (usize)cw * (usize)ch);
    const i32 L = sprite_draw_line_width(cw, ch, line);

    if (cp >= 0x2500 && cp <= 0x257F) { spr_box(buf, cw, ch, L, cp); return true; }
    if (cp >= 0x2800 && cp <= 0x28FF) { spr_braille(buf, cw, ch, cp); return true; }
    if (cp >= 0xE0B0 && cp <= 0xE0B7) { spr_powerline(buf, cw, ch, L, cp); return true; }
    if (cp >= 0x1FB00 && cp <= 0x1FB3B) {
        spr_mosaic(buf, cw, ch, spr_sextant[cp - 0x1FB00], 3);
        return true;
    }
    if (cp >= 0x1CD00 && cp <= 0x1CDE5) {
        spr_mosaic(buf, cw, ch, spr_octant[cp - 0x1CD00], 4);
        return true;
    }
    if (cp == 0x1FB95 || cp == 0x1FB96) { spr_checker(buf, cw, ch, (i32)(cp - 0x1FB95)); return true; }

    const SprRects *r = spr_find_rects(cp);
    if (r) { spr_rects_draw(buf, cw, ch, r); return true; }
    const SprPoly *p = spr_find_poly(cp);
    if (p) { spr_poly_draw(buf, cw, ch, p); return true; }
    const SprLines *l = spr_find_lines(cp);
    if (l) { spr_lines_draw(buf, cw, ch, L, l); return true; }
    return true;
}

#endif /* LIU_SPRITE_DRAW_H */
