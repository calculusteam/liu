/*
 * Liu - UTF-8 implementation
 */
#include "utf8.h"

u32 utf8_decode(const u8 *buf, usize len, u32 *codepoint) {
    if (len == 0) return 0;

    u8 b0 = buf[0];

    if (b0 < 0x80) {
        *codepoint = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        if (len < 2 || !utf8_is_continuation(buf[1])) return 0;
        *codepoint = ((u32)(b0 & 0x1F) << 6) | (buf[1] & 0x3F);
        if (*codepoint < 0x80) return 0; /* overlong */
        return 2;
    }
    if ((b0 & 0xF0) == 0xE0) {
        if (len < 3 || !utf8_is_continuation(buf[1]) || !utf8_is_continuation(buf[2])) return 0;
        *codepoint = ((u32)(b0 & 0x0F) << 12) | ((u32)(buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
        if (*codepoint < 0x800) return 0;
        if (*codepoint >= 0xD800 && *codepoint <= 0xDFFF) return 0; /* surrogate */
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0) {
        if (len < 4 || !utf8_is_continuation(buf[1]) ||
            !utf8_is_continuation(buf[2]) || !utf8_is_continuation(buf[3])) return 0;
        *codepoint = ((u32)(b0 & 0x07) << 18) | ((u32)(buf[1] & 0x3F) << 12) |
                     ((u32)(buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
        if (*codepoint < 0x10000 || *codepoint > 0x10FFFF) return 0;
        return 4;
    }
    return 0;
}

u32 utf8_encode(u32 cp, u8 *out) {
    if (cp < 0x80) {
        out[0] = (u8)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = 0xC0 | (u8)(cp >> 6);
        out[1] = 0x80 | (u8)(cp & 0x3F);
        return 2;
    }
    if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        out[0] = 0xE0 | (u8)(cp >> 12);
        out[1] = 0x80 | (u8)((cp >> 6) & 0x3F);
        out[2] = 0x80 | (u8)(cp & 0x3F);
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = 0xF0 | (u8)(cp >> 18);
        out[1] = 0x80 | (u8)((cp >> 12) & 0x3F);
        out[2] = 0x80 | (u8)((cp >> 6) & 0x3F);
        out[3] = 0x80 | (u8)(cp & 0x3F);
        return 4;
    }
    return 0;
}

usize utf8_len(const u8 *buf, usize byte_len) {
    usize count = 0;
    const u8 *end = buf + byte_len;
    while (buf < end) {
        buf = utf8_next(buf, end);
        if (!buf) break;
        count++;
    }
    return count;
}

bool utf8_validate_scalar(const u8 *buf, usize len) {
    const u8 *end = buf + len;
    while (buf < end) {
        u32 cp;
        u32 consumed = utf8_decode(buf, (usize)(end - buf), &cp);
        if (consumed == 0) return false;
        buf += consumed;
    }
    return true;
}

#ifndef USE_ASM

bool utf8_validate(const u8 *buf, usize len) {
    return utf8_validate_scalar(buf, len);
}

#endif /* !USE_ASM */

const u8 *utf8_next(const u8 *buf, const u8 *end) {
    if (buf >= end) return NULL;
    u32 cp;
    u32 n = utf8_decode(buf, (usize)(end - buf), &cp);
    if (n == 0) return NULL;
    return buf + n;
}

int utf8_char_width(u32 cp) {
    if (cp == 0) return 0;
    /* C0/C1 control characters */
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    /* Combining characters (simplified ranges) */
    if (cp >= 0x0300 && cp <= 0x036F) return 0;  /* Combining Diacriticals */
    if (cp >= 0xFE20 && cp <= 0xFE2F) return 0;  /* Combining Half Marks */
    /* Keep terminal cell width in sync with the renderer's color emoji path.
     * Color glyphs are rasterized/drawn as two-cell quads, so the grid must
     * reserve the following cell as ATTR_WDUMMY or adjacent emoji overlap. */
    if (utf8_is_emoji(cp)) return 2;
    /* CJK fullwidth ranges */
    if ((cp >= 0x1100 && cp <= 0x115F) ||   /* Hangul Jamo */
        (cp >= 0x2E80 && cp <= 0x303E) ||   /* CJK Radicals */
        (cp >= 0x3040 && cp <= 0x33BF) ||   /* Hiragana, Katakana, CJK */
        (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK Unified Ext A */
        (cp >= 0x4E00 && cp <= 0xA4CF) ||   /* CJK Unified */
        (cp >= 0xAC00 && cp <= 0xD7AF) ||   /* Hangul Syllables */
        (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK Compat */
        (cp >= 0xFE30 && cp <= 0xFE6F) ||   /* CJK Compat Forms */
        (cp >= 0xFF01 && cp <= 0xFF60) ||   /* Fullwidth Forms */
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||   /* Fullwidth Signs */
        (cp >= 0x20000 && cp <= 0x2FFFF) || /* CJK Ext B-F */
        (cp >= 0x30000 && cp <= 0x3FFFF))   /* CJK Ext G-I */
        return 2;
    return 1;
}

static bool utf8_is_bmp_default_emoji(u32 cp) {
    /* Keep this deliberately narrow. Many BMP symbols live in emoji-capable
     * blocks but default to text presentation unless followed by VS16
     * (U+FE0F). We do not store variation selectors in cells yet, so treating
     * whole blocks as emoji makes terminal spinners such as U+273B render via
     * Apple Color Emoji instead of the configured text font. */
    if (cp >= 0x231A && cp <= 0x231B) return true;
    if (cp >= 0x23E9 && cp <= 0x23EC) return true;
    if (cp == 0x23F0 || cp == 0x23F3) return true;
    if (cp >= 0x25FD && cp <= 0x25FE) return true;
    if (cp >= 0x2614 && cp <= 0x2615) return true;
    if (cp >= 0x2648 && cp <= 0x2653) return true;
    if (cp == 0x267F || cp == 0x2693 || cp == 0x26A1) return true;
    if (cp >= 0x26AA && cp <= 0x26AB) return true;
    if (cp >= 0x26BD && cp <= 0x26BE) return true;
    if (cp >= 0x26C4 && cp <= 0x26C5) return true;
    if (cp == 0x26CE || cp == 0x26D4 || cp == 0x26EA) return true;
    if (cp >= 0x26F2 && cp <= 0x26F3) return true;
    if (cp == 0x26F5 || cp == 0x26FA || cp == 0x26FD) return true;
    if (cp == 0x2705 || cp == 0x2728 || cp == 0x274C || cp == 0x274E) return true;
    if (cp >= 0x270A && cp <= 0x270B) return true;
    if (cp >= 0x2753 && cp <= 0x2755) return true;
    if (cp == 0x2757) return true;
    if (cp >= 0x2795 && cp <= 0x2797) return true;
    if (cp == 0x27B0 || cp == 0x27BF) return true;
    return false;
}

bool utf8_is_emoji(u32 cp) {
    if (cp >= 0xFE00  && cp <= 0xFE0F)  return false; /* Variation selectors — not emoji themselves */
    if (cp == 0x200D) return false;                    /* ZWJ — not emoji itself */
    if (utf8_is_bmp_default_emoji(cp)) return true;

    /* Common supplementary emoji ranges — covers most emoji without full
     * Unicode tables. BMP symbols are handled by the stricter helper above. */
    if (cp >= 0x1F600 && cp <= 0x1F64F) return true;  /* Emoticons */
    if (cp >= 0x1F300 && cp <= 0x1F5FF) return true;  /* Misc Symbols & Pictographs */
    if (cp >= 0x1F680 && cp <= 0x1F6FF) return true;  /* Transport & Map */
    if (cp >= 0x1F700 && cp <= 0x1F77F) return true;  /* Alchemical */
    if (cp >= 0x1F900 && cp <= 0x1F9FF) return true;  /* Supplemental Symbols */
    if (cp >= 0x1FA00 && cp <= 0x1FA6F) return true;  /* Chess Symbols */
    if (cp >= 0x1FA70 && cp <= 0x1FAFF) return true;  /* Symbols Extended-A */
    if (cp >= 0x1F1E0 && cp <= 0x1F1FF) return true;  /* Regional Indicators (flags) */
    return false;
}
