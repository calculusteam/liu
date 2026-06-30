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

#endif /* CORE_UTF8_H */
