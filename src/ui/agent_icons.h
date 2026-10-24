/*
 * Liu — compiled-in agent logos.
 *
 * PNG bytes are embedded by CMake from assets/agents/. On first request each
 * icon is decoded by stb_image into an RGBA buffer and cached for the process
 * lifetime. Missing assets return NULL so callers can fall back to a letter
 * badge.
 */
#ifndef UI_AGENT_ICONS_H
#define UI_AGENT_ICONS_H

#include "core/types.h"
#include "core/config.h"
#include "renderer/renderer.h"
#include "history/event.h"

/* Return a decoded RGBA8 bitmap for `tool`. `*w` and `*h` are set to the image
 * dimensions. Returns NULL if no PNG was shipped for this tool. The caller
 * must NOT free the pointer — the cache owns it. */
const u8 *agent_icon_rgba(ChatTool tool, i32 *w, i32 *h);

/* Exact-size variant: a cached area-averaged downscale at target_px square,
 * so the GPU samples 1:1 instead of minifying the 128-256px master (no
 * mipmaps on the Metal image path — direct minification aliases). Falls back
 * to the native buffer for upscales or on failure. The returned pointer is
 * stable for the process lifetime — safe as a renderer image-cache key. */
const u8 *agent_icon_rgba_scaled(ChatTool tool, i32 target_px, i32 *w, i32 *h);

/* Same, but picks the cut that survives the background it will be drawn on.
 * A monochrome logo (OpenCode) ships two: the light one for dark surfaces and
 * the dark one for the near-white icon tile. Pass the colour actually painted
 * under the icon — the tile from agent_icon_backdrop() where there is one, the
 * pill/panel fill otherwise. Tools with a single icon ignore `bg`.
 *
 * Prefer this over agent_icon_rgba_scaled() at every draw site: the plain form
 * always answers with the dark-surface cut. */
const u8 *agent_icon_rgba_scaled_on(ChatTool tool, i32 target_px, Color bg,
                                    i32 *w, i32 *h);

/* Rough brand color for the tool — used as a colored backdrop under the icon
 * and as a fallback when no PNG is available. */
typedef struct { f32 r, g, b, a; } AgentTint;
AgentTint agent_icon_tint(ChatTool tool);

/* Theme-aware tile color drawn under the (transparent-background) PNG. The
 * tile picks up a hint of the theme bg so it does not look like a stark
 * sticker, but is guaranteed to contrast against the icon's brand tint so the
 * logo never washes out (e.g. Claude's orange logo never sits on an orange
 * tile). Pass NULL theme for the safe near-white default. */
Color agent_icon_backdrop(ChatTool tool, const Theme *t);

/* One-letter fallback label (uppercase) when the icon bitmap is missing. */
const char *agent_icon_letter(ChatTool tool);

/* Draw that letter so it FILLS the box a real logo would have occupied.
 *
 * Every draw site pairs an icon with a letter fallback. The PNG is drawn
 * across the whole box (26 pt in the picker, inset 2), while a letter at the
 * surrounding panel's text scale would be roughly a third of it and read as a
 * broken icon rather than a missing asset. A tool added ahead of its logo
 * needs the fallback to hold the same box on its own.
 *
 * The glyph is centred in the renderer's UI cell (renderer_ui_glyph_dy), so
 * the cell IS the box: this hands the box in as the scale and restores
 * whatever the caller had set. It flushes pending glyphs twice by way of
 * renderer_set_ui_scale, which is why it is a fallback path and not something
 * to call per label. */
void agent_icon_draw_letter(Renderer *r, ChatTool tool,
                            f32 x, f32 y, f32 w, f32 h, Color fg);

/* Sleeping-tab placeholder artwork (assets/liu-sleep.png). RGBA8, cached for the
 * process lifetime. Returns NULL if the asset wasn't shipped. */
const u8 *liu_sleep_icon_rgba(i32 *w, i32 *h);

/* Same mascot, but rasterised from its SVG source with the artwork's three
 * fills swapped for theme colours — so the sleeping card stops being purple on
 * a green or amber theme.
 *
 * `body_lo`/`body_hi` are the two ends of the coat gradient and `ink` is the
 * outline/closed-eyes colour; pass 0xRRGGBB. Returns PREMULTIPLIED RGBA at
 * px*px, cached per (px, colours) for the process lifetime — the renderer keys
 * textures by pixel pointer, so these must never be freed. NULL when the SVG
 * wasn't embedded or rasterisation failed; callers should fall back to
 * liu_sleep_icon_rgba(). */
const u8 *liu_sleep_icon_themed(i32 px, u32 body_lo, u32 body_hi, u32 ink);


/* The app mark (assets/appicon-256.png — a derived copy of the same drawing the
 * Dock icon is built from), rendered square with squircle corners masked in.
 *
 * PREMULTIPLIED RGBA, cached per requested size for the process lifetime (the
 * renderer's image cache keys textures by pixel pointer, so these buffers must
 * never be freed). Ask for the exact on-screen pixel size — the GPU path has no
 * mipmaps, so anything but a 1:1 sample shimmers.
 *
 * `*w`/`*h` report what you actually got, which is `target_px` in every real
 * case; a request larger than the embedded 256² bitmap returns it at its own
 * size for the sampler to upscale. NULL when the asset wasn't shipped or on
 * OOM — callers should keep a fallback mark. */
const u8 *liu_app_icon_rgba(i32 target_px, i32 *w, i32 *h);

#endif
