/*
 * Liu - UTF-8 encoding/decoding
 * Fast UTF-8 operations with optional SIMD validation.
 */
#ifndef CORE_UTF8_H
#define CORE_UTF8_H

#include "types.h"

/* Decode one codepoint from UTF-8. Returns bytes consumed (1-4) or 0 on error. */
u32 utf8_decode(const u8 *buf, usize len, u32 *codepoint);

/* Encode one codepoint to UTF-8. Returns bytes written (1-4) or 0 on error. */
u32 utf8_encode(u32 codepoint, u8 *out);

/* Number of codepoints in UTF-8 string */
usize utf8_len(const u8 *buf, usize byte_len);

/* Validate UTF-8 string. Returns true if valid.
 * When USE_ASM is defined, uses SIMD-accelerated validation. */
bool utf8_validate(const u8 *buf, usize len);

/* Advance to next codepoint. Returns pointer to next char or NULL at end. */
const u8 *utf8_next(const u8 *buf, const u8 *end);

/* Is this byte a UTF-8 continuation byte? */
static inline bool utf8_is_continuation(u8 b) {
    return (b & 0xC0) == 0x80;
}

/* Width of codepoint in terminal cells (0, 1, or 2 for fullwidth) */
int utf8_char_width(u32 codepoint);

/* Is codepoint an emoji that should be rendered in color? */
bool utf8_is_emoji(u32 codepoint);

/* The range ladders the two answers above are DEFINED by. Below U+20000 those
 * functions read a table built from these, a block of 256 codepoints at a time
 * on first use; tests compare the two over every codepoint. */
int  utf8_char_width_ladder(u32 codepoint);
bool utf8_is_emoji_ladder(u32 codepoint);

/* Does this codepoint join onto the preceding grapheme (ZWJ, variation
 * selector, skin tone, combining mark, flag tag) rather than open a new cell?
 * Everything that answers true has width 0. */
bool utf8_is_joiner(u32 codepoint);

/* =========================================================================
 * Grapheme clusters
 *
 * A terminal cell holds ONE u32, but a single visible emoji is often several
 * codepoints: ❤️ is U+2764 U+FE0F, 👍🏽 is U+1F44D U+1F3FD, 👨‍👩‍👧 is three
 * emoji joined by two ZWJs, 🇹🇷 is two regional indicators. The parser used
 * to drop the joiners, which split every one of those into separate cells.
 *
 * Instead of widening Cell (16 bytes, one per scrollback cell), multi-codepoint
 * clusters are interned in a process-global table and the cell stores a HANDLE:
 * a u32 with bit 30 set. Handles ride through every path that only copies the
 * u32 — scroll, compress/materialize, resize reflow — untouched.
 *
 * Bit 30 is free by construction: real codepoints stop at U+10FFFF, and the
 * font atlas's synthetic keys (sized/composite glyphs) all set bit 31.
 * ========================================================================= */

#define UTF8_CLUSTER_BIT   0x40000000u
#define UTF8_CLUSTER_MAX   16   /* codepoints per cluster; a skin-toned ZWJ
                                 * family needs 11, a subdivision flag 8 */

static inline bool utf8_is_cluster(u32 v) { return (v & UTF8_CLUSTER_BIT) != 0; }

/* Intern a codepoint sequence. Returns a handle, or the first codepoint when
 * `n` is 1 or the table is full (callers then degrade to the base glyph). */
u32 utf8_cluster_intern(const u32 *cps, u32 n);

/* Codepoints behind a handle. Returns NULL for a plain codepoint. The pointer
 * stays valid for the life of the process: storage is paged and a page is
 * never moved. Thread-safe against concurrent interning. */
const u32 *utf8_cluster_lookup(u32 handle, u32 *out_n);

/* Table state, for diagnostics and tests. `refused` counts interns turned
 * away (table full or out of memory) — each one drew a base glyph instead of
 * the cluster. */
u32   utf8_cluster_count(void);
u32   utf8_cluster_capacity(void);
u64   utf8_cluster_refused(void);
usize utf8_cluster_bytes(void);

/* First codepoint of a cluster, or `v` itself when it is a plain codepoint.
 * Every consumer that cannot handle clusters degrades through this. */
u32 utf8_cluster_base(u32 v);

/* UTF-8 bytes for a codepoint OR a cluster handle. Writes at most `cap` bytes
 * and returns how many; 0 if it does not fit. Use this instead of utf8_encode
 * anywhere cells are turned back into text (copy, search, URL scan). */
u32 utf8_cluster_encode(u32 v, u8 *out, u32 cap);

/* Decode ONE grapheme from UTF-8: the base codepoint plus every joiner that
 * belongs to it, interned into a handle when there is more than one. Returns
 * the bytes consumed (0 at the end or on malformed input); `out_cp` receives
 * the codepoint-or-handle to draw and `out_cols` how many terminal columns it
 * occupies (0, 1 or 2).
 *
 * This is THE text-drawing primitive outside the terminal grid — the reader,
 * the editor and every UI label walk their strings with it. Decoding one
 * codepoint at a time instead is what drew `❤️` as a thin `❤` followed by a
 * blank, and made every emoji overlap the character after it (the renderer
 * emits a two-cell quad, so the caller must advance two). */
u32 utf8_next_grapheme(const u8 *s, usize len, u32 *out_cp, i32 *out_cols);

/* Total columns a UTF-8 run occupies on a character grid, grapheme-aware.
 * Use for measuring text (wrapping, table columns, centring) — utf8_len counts
 * CODEPOINTS, which is a different number for emoji and CJK. */
usize utf8_display_cols(const u8 *s, usize byte_len);

#endif /* CORE_UTF8_H */
