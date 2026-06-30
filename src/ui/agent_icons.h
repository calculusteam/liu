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

/* Sleeping-tab placeholder artwork (assets/liu-sleep.png). RGBA8, cached for the
 * process lifetime. Returns NULL if the asset wasn't shipped. */
const u8 *liu_sleep_icon_rgba(i32 *w, i32 *h);

#endif
