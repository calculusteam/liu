/*
 * Liu — Lucide icon rasteriser.
 *
 * Every chrome icon is a real vector (Lucide, MIT) embedded as SVG source and
 * rasterised by nanosvg at the exact pixel size it will be drawn at.
 * Rasterising the real path at draw size gives correct curves, correct joins
 * and correct optical weight at every DPI, where a curve approximated by
 * axis-aligned rectangles is a staircase.
 *
 * Output is a white, premultiplied-alpha RGBA mask: the shape lives in the
 * alpha channel and the caller tints it at draw time, which is what the whole
 * icon pipeline expects.
 */
#ifndef UI_LUCIDE_H
#define UI_LUCIDE_H

#include "core/types.h"

/* Rasterise `name` (a Lucide icon id, e.g. "chevron-right") at px × px.
 *
 * Returns a cached, premultiplied RGBA8 buffer of px*px*4 bytes, or NULL when
 * the icon isn't embedded or px is out of range. The cache owns the memory.
 *
 * The buffer is valid until a later lucide call evicts its slot. The cache is
 * LRU with a byte budget, so the pointer is not stable: DO NOT key a cache on
 * it. Key on a stable (kind, size) anchor instead; see the banner in lucide.c.
 *
 * Sizes are rounded to whole pixels by the caller; asking for the same (name,
 * px) twice is a hash lookup, not a re-raster. */
const u8 *lucide_icon_rgba(const char *name, i32 px);

/* Same icon, tinted. The mask is rasterised once per (name, px) and the tint
 * applied into a cached premultiplied copy keyed by (name, px, colour), so a
 * redraw with an unchanged colour is a lookup and a colour change costs one
 * multiply pass rather than a re-raster. Components are 0-255 straight values;
 * `a` scales the whole icon's coverage. */
const u8 *lucide_icon_rgba_tinted(const char *name, i32 px,
                                  u8 red, u8 green, u8 blue, u8 alpha);

/* Horizontal ink insets inside the px × px box: blank columns before the first
 * inked column and after the last. Lucide icons are drawn on a 24-unit grid
 * with ~2 units of built-in padding, so call sites that need optical (rather
 * than box) alignment subtract these. Either pointer may be NULL; both are set
 * to 0 when the icon can't be rasterised. Memoised per (name, px). */
void lucide_icon_ink_insets(const char *name, i32 px, f32 *left, f32 *right);

/* True when `name` is embedded in this build. */
bool lucide_icon_exists(const char *name);

/* Number of embedded icons, and the name at `index` (NULL when out of range).
 * Used by the icon-table self-check to prove every icon.c entry resolves. */
i32 lucide_icon_count(void);
const char *lucide_icon_name_at(i32 index);

#endif /* UI_LUCIDE_H */
