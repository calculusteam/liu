/*
 * liu filebrowser — extension → icon asset + legacy Nerd Font fallback.
 */
#ifndef UI_FB_ICON_H
#define UI_FB_ICON_H

#include "core/types.h"
#include "renderer/renderer.h"   /* Color */
#include "ui/fb_asset_icons.h"

typedef struct {
    u32             codepoint;   /* legacy fallback glyph, 0 -> no icon */
    Color           color;       /* legacy fallback colour (sRGB 0..1) */
    FbAssetIconKind asset;       /* preferred high-DPI PNG asset */
} FbIcon;

/* Look up the icon for a filesystem entry.
 *   name   — basename (may include extension, may have no slash)
 *   is_dir — if true, a folder icon is returned regardless of name
 * Recognises case-insensitive file extensions and a handful of special
 * filenames (Dockerfile, Makefile, README, LICENSE, .gitignore, …). */
FbIcon fb_icon_for(const char *name, bool is_dir);

/* Neutral fallback — generic file icon with the theme's dim fg colour. */
FbIcon fb_icon_default_file(Color dim_fg);

#endif
