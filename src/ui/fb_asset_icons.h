/*
 * liu filebrowser — compiled-in high-DPI PNG icons.
 */
#ifndef UI_FB_ASSET_ICONS_H
#define UI_FB_ASSET_ICONS_H

#include "core/types.h"

typedef enum {
    FB_ASSET_ICON_NONE = 0,
    FB_ASSET_ICON_FOLDER,
    FB_ASSET_ICON_FILE,
    FB_ASSET_ICON_CODE,
    FB_ASSET_ICON_TEXT,
    FB_ASSET_ICON_IMAGE,
    FB_ASSET_ICON_VIDEO,
    FB_ASSET_ICON_AUDIO,
    FB_ASSET_ICON_ARCHIVE,
    FB_ASSET_ICON_PDF,
    FB_ASSET_ICON_TERMINAL,
    FB_ASSET_ICON_GEAR,
    FB_ASSET_ICON_GIT,
    FB_ASSET_ICON_LOCK,
    FB_ASSET_ICON_UP,
    FB_ASSET_ICON_REFRESH,
    FB_ASSET_ICON_CLOSE,
    /* Logos (devicon, MIT) for the file types Seti UI has no icon for — a
     * file's icon is Seti's (ui/seti_icons.h), and one of these only when no
     * Seti rule names the file. */
    FB_ASSET_ICON_LANG_PYTHON,
    FB_ASSET_ICON_LOGO_FIRST = FB_ASSET_ICON_LANG_PYTHON,
    FB_ASSET_ICON_LANG_ERLANG,
    FB_ASSET_ICON_LANG_FORTRAN,
    FB_ASSET_ICON_LANG_BUN,
    FB_ASSET_ICON_LANG_VB,
    FB_ASSET_ICON_LANG_DELPHI,
    FB_ASSET_ICON_LANG_MATLAB,
    FB_ASSET_ICON_LANG_ASTRO,
    FB_ASSET_ICON_LANG_NIX,
    FB_ASSET_ICON_COUNT
} FbAssetIconKind;

/* The icon at its native (master) size, premultiplied. Same lifetime rule as
 * the scaled variant below. */
const u8 *fb_asset_icon_rgba(FbAssetIconKind kind, i32 *w, i32 *h);

/* Exact-size variant: a cached, area-averaged downscale of the icon at
 * target_px × target_px, so the GPU samples 1:1 instead of minifying the
 * 128px master (the Metal image path has no mipmaps, and direct minification
 * aliases, which blurs or crunches list icons). A request the master cannot
 * be reduced to (target_px >= the native size) gets the master, and *w / *h
 * say which it was. NULL when the PNG does not decode.
 *
 * The buffer is valid until the next call here that has to DECODE: the cache
 * holds a bounded number of bytes (fb_asset_icon_cache_bytes) and drops the
 * least recently used buffer to stay there. Draw with it, do not keep it, and
 * hand the renderer fb_asset_icon_cache_key() — never the pointer — as the
 * image's identity. */
const u8 *fb_asset_icon_rgba_scaled(FbAssetIconKind kind, i32 target_px,
                                    i32 *w, i32 *h);

/* A stable identity for (icon, size) to key the renderer's image cache on. */
const void *fb_asset_icon_cache_key(FbAssetIconKind kind, i32 px);

/* Bytes of pixels the cache holds right now; never above its budget. */
usize fb_asset_icon_cache_bytes(void);

/* True when the artwork is a SILHOUETTE: one flat colour whose luminance would
 * vanish on one of the two panels it can land on (the light-recoloured Rust,
 * Markdown, LaTeX marks on a light theme). Such an icon has been re-inked to
 * pure white at decode and is meant to be drawn TINTED with the surrounding
 * text colour (renderer_draw_image_tinted), the way a Lucide mask is; a
 * full-colour logo is drawn as it is. Decided from the decoded pixels, not
 * from a list, so a logo added later is covered automatically. */
bool fb_asset_icon_is_mask(FbAssetIconKind kind);

/* Horizontal ink insets of the icon as drawn at `size_px`: blank columns before
 * the first inked one and after the last. This is what lets a PNG logo hang
 * off the file list's ink rail the way icon_ink_insets() lets a Lucide icon
 * (see FB_ROW_PAD_PT in layout.h). Memoised per (kind, size); both 0 when the
 * icon cannot be decoded. Either pointer may be NULL. */
void fb_asset_icon_ink_insets(FbAssetIconKind kind, i32 size_px,
                              f32 *left, f32 *right);

/* Programming-language / ecosystem logos — the kinds that have a coloured PNG
 * of their own rather than a generic file-type shape. */
static inline bool fb_asset_icon_is_logo(FbAssetIconKind kind) {
    return kind >= FB_ASSET_ICON_LOGO_FIRST && kind < FB_ASSET_ICON_COUNT;
}

#endif /* UI_FB_ASSET_ICONS_H */
