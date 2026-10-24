/*
 * Liu — keyboard shortcut indicators: the keys of a combination, legibly.
 *
 * A shortcut indicator has to separate the MODIFIERS from the KEY: the reader
 * is looking for the letter, and the modifiers only qualify it. The order is
 * the one the HIG fixes, ⌃ ⌥ ⇧ ⌘, and every shortcut drawn through here has
 * the same spelling whatever surface shows it.
 *
 * This module only changes the TEXT. Nothing is boxed and nothing gets a
 * background: each surface keeps its own colour and chrome (the palette its
 * pill, the sidebar its dim text), and the row is drawn as the keys pressed,
 * in HIG order, a half cell apart, the modifiers a step dimmer than the key —
 * "⇧ ⌘ N", not "Shift Cmd N" or "⇧⌘N". Measured before it is drawn, so a
 * caller can right-align it.
 *
 * `keycap_split` is the parser and is pure: it accepts every spelling the
 * tree produces — keybind_format's "Cmd+Shift+T" / "Ctrl+Alt+Left" (with
 * "Super" on Linux), the menus' "Shift Cmd N", the glyph form "⌘⇧M", and bare
 * key words ("Esc", "F2", "Del") — and normalises modifiers to the platform's
 * spelling (glyphs on macOS, short words elsewhere) and key names to the
 * glyph or short word that reads best. The parse and the measurement need no
 * renderer.
 */
#ifndef UI_KEYCAP_H
#define UI_KEYCAP_H

#include "core/types.h"
#include "core/config.h"
#include "renderer/renderer.h"

#define KEYCAP_MAX_KEYS 6

typedef struct {
    char text[16];      /* what the cap shows: "⌘", "⇧", "T", "Esc", "←" */
    bool modifier;
} KeycapKey;

/* Split `accel` into caps. Returns the number written (0 for an empty or
 * unreadable string). `glyph_mods` selects the modifier spelling: true draws
 * ⌃ ⌥ ⇧ ⌘, false draws Ctrl Alt Shift Super — keycap_platform_glyphs() is
 * the platform's default. */
i32  keycap_split(const char *accel, KeycapKey *out, i32 max, bool glyph_mods);
bool keycap_platform_glyphs(void);

/* Width of the row in pixels for a UI cell of `ui_cw`: the keys' columns plus
 * a half-cell gap between keys. */
f32 keycap_row_width(const KeycapKey *keys, i32 n, f32 ui_cw);

/* Draw the row as text. `x` is the row's LEFT edge, or its RIGHT edge when
 * `align_right` (the usual case: a shortcut sits at the end of a row).
 * `text_y` is the top of the text line, exactly as the caller would pass it
 * to draw_text_ex. `base` is the colour the surface draws its shortcut in:
 * the key is drawn in it as given, the modifiers at KEYCAP_MOD_ALPHA of its
 * alpha. Queues glyphs only — no rect, no border, no fill — into the glyph
 * batch; the caller must have set the renderer's UI scale to (ui_cw, ui_ch).
 * Returns the row's width. */
#define KEYCAP_MOD_ALPHA 0.62f
f32 keycap_draw(Renderer *r, const KeycapKey *keys, i32 n, f32 x, f32 text_y,
                bool align_right, f32 ui_cw, Color base);

/* Split and draw in one call; returns the width (0 for nothing drawn). */
f32 keycap_draw_accel(Renderer *r, const char *accel, f32 x, f32 text_y,
                      bool align_right, f32 ui_cw, Color base);
f32 keycap_accel_width(const char *accel, f32 ui_cw);

#endif /* UI_KEYCAP_H */
