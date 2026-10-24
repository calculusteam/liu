/*
 * Liu — color bitmap font reader (CBDT/CBLC)
 *
 * stb_truetype cannot open a color emoji font at all: NotoColorEmoji and its
 * kin carry no `glyf` or `CFF ` outline table, only PNG bitmaps in `CBDT`, so
 * stbtt_InitFont rejects the file outright and the font never reaches the
 * fallback chain; emoji would render as blank cells on Linux.
 *
 * This is the minimum needed: locate a codepoint's glyph via the font's own
 * `cmap`, find its bitmap through `CBLC`, and hand back the decoded RGBA.
 * Nothing here is platform-specific — macOS gets emoji from Core Text and
 * simply never calls it.
 */
#ifndef EMOJI_BITMAP_H
#define EMOJI_BITMAP_H

#include "core/types.h"

typedef struct EmojiFont EmojiFont;

/* Map `path` and validate that it really is a CBDT/CBLC color font. Returns
 * NULL for a missing file or any font without color bitmaps — callers treat
 * that as "no emoji source available", not an error. */
EmojiFont *emoji_font_open(const char *path);

void emoji_font_close(EmojiFont *f);

/* Decode `cp`'s bitmap into a freshly malloc'd RGBA8 buffer (`*w` * `*h` * 4),
 * which the caller owns and frees. Returns NULL when the font has no glyph for
 * the codepoint or the entry is malformed.
 *
 * Every offset read out of the file is bounds-checked against the mapping: a
 * font is untrusted input (it can arrive from a user's ~/.fonts), and a
 * truncated or hostile one must fail the lookup rather than read out of
 * bounds. */
u8 *emoji_font_glyph_rgba(EmojiFont *f, u32 cp, i32 *w, i32 *h);

/* True if the font contains a bitmap for `cp`. Cheaper than decoding — used to
 * decide whether to take the color path at all. */
bool emoji_font_has_glyph(EmojiFont *f, u32 cp);

#endif /* EMOJI_BITMAP_H */
