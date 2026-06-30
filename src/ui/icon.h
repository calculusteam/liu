/*
 * Liu — unified UI icon API.
 *
 * One enum, one draw call, one source of truth for every glyph-style icon
 * the app paints (tab close, toolbar buttons, modal close, sidebar header
 * actions, etc.).
 *
 * Implementation strategy, in priority order:
 *   1. macOS SF Symbol (platform_get_system_symbol_rgba, cached) — pixel-
 *      perfect at any DPI, weight-matched, accessibility-aware.
 *   2. Hand-drawn vector fallback (rect primitives) — used on Linux /
 *      older macOS where SF Symbols are unavailable. Each fallback is
 *      tuned to read at small icon sizes (12–18 dpi) without aliasing.
 *
 * Why a separate module from fb_icon / fb_asset_icons? Those are
 * file-type semantic icons (folder, code, image, …) backed by PNGs.
 * This module is for *UI structure* icons that belong to the app chrome
 * itself — the visual language is different (monoline, weight-matched,
 * tinted at draw time) and the asset pipeline is different (SF Symbol
 * first, no PNGs).
 */
#ifndef LIU_UI_ICON_H
#define LIU_UI_ICON_H

#include "core/types.h"
#include "renderer/renderer.h"

typedef enum {
    ICON_NONE = 0,
    /* Toolbar */
    ICON_SIDEBAR,        /* SF: sidebar.left */
    ICON_GEAR,           /* SF: gearshape */
    ICON_NETWORK,        /* SF: network */
    ICON_FONT_LARGER,    /* SF: textformat.size.larger / fallback: A↑ */
    ICON_FONT_SMALLER,   /* SF: textformat.size.smaller / fallback: A↓ */
    /* General */
    ICON_CLOSE,          /* SF: xmark — tab/modal close */
    ICON_PLUS,           /* SF: plus  — new tab, increase */
    ICON_MINUS,          /* SF: minus — decrease */
    ICON_SEARCH,         /* SF: magnifyingglass */
    ICON_UP,             /* SF: arrow.up — sidebar parent */
    ICON_REFRESH,        /* SF: arrow.clockwise */
    ICON_CHEVRON_DOWN,   /* SF: chevron.down */
    ICON_CHEVRON_RIGHT,  /* SF: chevron.right */
    ICON_BELL,           /* SF: bell.fill */
    ICON_MOON,           /* SF: moon.fill — sleeping tab */
    ICON_CHECK,          /* SF: checkmark */
    ICON_COUNT,
} IconKind;

/* Draw one icon centered on (x, y, size, size). Color is sRGB; alpha is
 * respected (passes through the per-glyph alpha pipeline). When the
 * platform layer can't produce an SF Symbol the call falls back to a
 * vector approximation drawn with renderer_draw_rect primitives, so a
 * call is *always* safe to make — never a no-op. */
void icon_draw(Renderer *r, IconKind kind, f32 x, f32 y, f32 size, Color color);

#endif /* LIU_UI_ICON_H */
