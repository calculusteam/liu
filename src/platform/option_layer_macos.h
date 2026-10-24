/*
 * Which ASCII characters the active keyboard layout can reach WITHOUT Option.
 *
 * Used by the Option-as-Alt path: when a chord composes an ASCII letter, that
 * letter is only worth stealing from Meta if the layout offers no other way to
 * type it (Bulgarian, Korean, Kana, Cangjie put the whole Latin alphabet
 * behind Option; Sámi and Turkmen put only Q/W/X/Z there; Turkish puts 'i'
 * there but also gives it its own key).
 *
 * Answers come from macOS's own layout tables via UCKeyTranslate, so no
 * per-language data lives in Liu.
 */
#ifndef PLATFORM_OPTION_LAYER_MACOS_H
#define PLATFORM_OPTION_LAYER_MACOS_H

#include "core/types.h"

/* 128-bit set of ASCII code points, one bit per code point. */
typedef struct {
    u64 bits[2];
} AsciiSet;

static inline bool ascii_set_has(const AsciiSet *s, u32 c) {
    if (c >= 128) return false;
    return (s->bits[c >> 6] >> (c & 63)) & 1u;
}

static inline void ascii_set_add(AsciiSet *s, u32 c) {
    if (c < 128) s->bits[c >> 6] |= (u64)1 << (c & 63);
}

/* Every ASCII character the given layout produces without the Option key
 * (bare and Shift). `layout_data` is a UCKeyboardLayout*, `kbd_type` an
 * LMGetKbdType() value. Exposed for testing against specific layouts. */
AsciiSet option_layer_scan_bare_ascii(const void *layout_data, u32 kbd_type);

/* Does the ACTIVE layout require Option to type `codepoint`? False for
 * anything non-ASCII, and false if the layout is unavailable — the caller
 * then treats the chord as Meta, which is the conservative answer. Result is
 * cached per input source and recomputed when the user switches layouts. */
bool option_layer_needs_option(u32 codepoint);

#endif /* PLATFORM_OPTION_LAYER_MACOS_H */
