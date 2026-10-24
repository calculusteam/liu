/*
 * Liu — Seti UI file icons (https://github.com/jesseweed/seti-ui, MIT).
 *
 * The icon a FILE wears in the explorer and on its editor tab. Which icon and
 * which colour a name gets is Seti's own mapping, generated into seti_map.h
 * from the stylesheet vendored under assets/icons/seti/ (scripts/
 * gen_seti_icons.py); the artwork is Seti's SVGs, embedded by CMake and
 * rasterised by nanosvg at the exact pixel size they are drawn at.
 *
 * Seti ships as an icon FONT, so an SVG's own fill colours mean nothing: a
 * glyph is a shape, and its colour comes from the mapping. The rasteriser
 * therefore returns a white premultiplied mask, the same thing the Lucide
 * pipeline returns, and the caller tints it.
 */
#ifndef UI_SETI_ICONS_H
#define UI_SETI_ICONS_H

#include "core/types.h"
#include "renderer/renderer.h"

typedef struct {
    u8 icon;    /* index into the embedded set, < seti_icon_count() */
    u8 color;   /* index into Seti's palette, for seti_icon_color() */
} SetiIcon;

/* The icon Seti's mapping gives a file NAME (a basename; case is ignored).
 * Tried in order: the whole name ("CMakeLists.txt"), the longest dot-suffix
 * ("a.spec.ts" is "spec.ts" before "ts"), then a family prefix at a separator
 * ("Dockerfile.dev"). Returns false when no rule names the file; *out is then
 * Seti's default mark, so a caller with nothing better can draw it as it is. */
bool seti_icon_for(const char *name, SetiIcon *out);

/* The glyph as a white premultiplied RGBA mask of px*px*4 bytes, or NULL when
 * it cannot be rasterised. Valid until the next call that has to rasterise
 * (the cache holds a bounded number of bytes and drops the least recently used
 * mask to stay there) — draw with it, do not keep it, and hand the renderer
 * seti_icon_cache_key() rather than the pointer. */
const u8 *seti_icon_mask(u32 icon, i32 px);

/* Bytes of mask the cache holds right now; never above its budget. */
usize seti_icon_cache_bytes(void);

/* A stable identity for (icon, px) to key the renderer's image cache on. */
const void *seti_icon_cache_key(u32 icon, i32 px);

/* A palette entry as straight sRGB, kept visible on `surface`. Seti's palette
 * was drawn for a dark panel and is returned untouched on one, its deliberately
 * faint "ignored" tone included; on a light surface a colour too pale to be
 * seen is darkened, hue kept, until it reaches SETI_MIN_CONTRAST. Alpha is 1. */
#define SETI_MIN_CONTRAST 3.0f
Color seti_icon_color(u32 color, Color surface);

/* The embedded set, for self-checks. */
i32 seti_icon_count(void);
const char *seti_icon_name(u32 icon);

#endif /* UI_SETI_ICONS_H */
