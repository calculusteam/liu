/*
 * Liu — UTF-8 sequences that only deceive.
 *
 * Valid UTF-8 can still lie to a reader or a consumer: C1 controls (U+0080-
 * U+009F — an 8-bit CSI to anything that decodes them), the bidi embedding /
 * override / isolate controls (U+202A-202E, U+2066-2069 — "Trojan Source"),
 * the invisible space characters (U+200B ZWSP, U+2060-2064 word joiner and the
 * invisible math operators, U+FEFF, the Mongolian vowel separator U+180E), the
 * line / paragraph separators (U+2028, U+2029) and the whole default-ignorable
 * plane-14 block U+E0000-E0FFF — the "tag characters" (U+E0000-E007F) mirror
 * ASCII one for one and render as NOTHING, so a page or a document can hide a
 * sentence in a string a person sees as blank and a language model reads (ASCII
 * smuggling), and the variation selectors supplement (U+E0100-E01EF) is the
 * same trick with a different alphabet. One accepted cost: a subdivision flag
 * (England, Scotland, Wales) IS U+1F3F4 followed by tag characters, so in a tab
 * title or a dictated transcript it degrades to the plain black flag, and in
 * browser-to-agent text it shows as its escapes. Text that Liu TYPES into a prompt on the user's
 * behalf — a dictated transcript above all, which a hosted or custom speech
 * endpoint controls entirely — should carry none of them. (The tab title
 * cleaner in vt_parser.c applies the same list to decoded code points.)
 *
 * NOT here, on purpose: LRM / RLM (U+200E / U+200F — a right-to-left transcript
 * uses them and they reorder nothing beyond their neighbours) and ZWNJ / ZWJ
 * (U+200C / U+200D). The last two are not decoration: Persian and Kurdish
 * cannot be spelled without ZWNJ, Hindi and other Indic conjuncts and every
 * emoji family need ZWJ, and Whisper dictates all of those — dropping them
 * would silently change the words being typed.
 */
#ifndef CORE_TEXT_SANITIZE_H
#define CORE_TEXT_SANITIZE_H

#include "core/types.h"
#include <stdbool.h>

/* How many bytes at `p` (of `n` available) form a deceptive sequence; 0 when
 * `p` does not start one. Byte-level on purpose: callers walk a string. */
/* The same list over a decoded code point, for callers that already decode (the
 * OSC title cleaner). Both forms must agree on every code point. */
static inline bool liu_is_deceptive_cp(u32 cp) {
    return (cp >= 0x80 && cp <= 0x9F) ||
           (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2066 && cp <= 0x2069) ||
           (cp >= 0x2060 && cp <= 0x2064) ||
           cp == 0x200B || cp == 0xFEFF || cp == 0x180E ||
           cp == 0x2028 || cp == 0x2029 ||
           (cp >= 0xE0000 && cp <= 0xE0FFF);
}

static inline usize liu_deceptive_utf8_len(const unsigned char *p, usize n) {
    if (n >= 2 && p[0] == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) return 2;       /* C1 */
    if (n >= 3 && p[0] == 0xE2) {
        if (p[1] == 0x80 && (p[2] == 0x8B ||                                   /* ZWSP only (see below) */
                             (p[2] >= 0xAA && p[2] <= 0xAE) ||                 /* LRE RLE PDF LRO RLO */
                             p[2] == 0xA8 || p[2] == 0xA9)) return 3;          /* LS PS */
        if (p[1] == 0x81 && ((p[2] >= 0xA0 && p[2] <= 0xA4) ||                 /* word joiner, invisible math */
                             (p[2] >= 0xA6 && p[2] <= 0xA9))) return 3;        /* LRI RLI FSI PDI */
    }
    if (n >= 3 && p[0] == 0xE1 && p[1] == 0xA0 && p[2] == 0x8E) return 3;      /* Mongolian vowel separator */
    /* U+E0000-E0FFF: F3 A0 80 80 .. F3 A0 BF BF — tags, variation selectors supplement. */
    if (n >= 4 && p[0] == 0xF3 && p[1] == 0xA0 &&
        p[2] >= 0x80 && p[2] <= 0xBF && p[3] >= 0x80 && p[3] <= 0xBF) return 4;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) return 3;      /* BOM / ZWNBSP */
    return 0;
}

/* A transcript, collapsed into something safe to type at a prompt (the rule
 * main.c's app_voice_clean_text applies to every dictation, hosted or local).
 *
 * Line breaks are the reason it exists: a carriage return in injected text is
 * not whitespace to an agent CLI — it SUBMITS the prompt — so CR, LF and TAB
 * become single spaces (the user decides when to submit), leading and trailing
 * space goes, C0 controls and DEL are dropped, and so are the deceptive
 * sequences above. Returns the bytes written, excluding the NUL. */
static inline i32 liu_clean_dictation(const char *src, char *out, i32 cap) {
    if (!src || !out || cap <= 0) return 0;
    i32 n = 0;
    bool pending_space = false, wrote_any = false;
    usize left = 0;
    while (src[left]) left++;
    for (const char *p = src; *p && n < cap - 1; p++, left--) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (wrote_any) pending_space = true;
            continue;
        }
        if (c < 0x20 || c == 0x7f) continue;
        usize dz = liu_deceptive_utf8_len((const unsigned char *)p, left);
        if (dz) { p += dz - 1; left -= dz - 1; continue; }
        if (pending_space) {
            /* The space and the character it leads in go in together or not
             * at all: with one byte of room left this wrote both, and then
             * the terminator one past the buffer. */
            if (n + 2 > cap - 1) break;
            out[n++] = ' ';
            pending_space = false;
        }
        out[n++] = (char)c;
        wrote_any = true;
    }
    /* A cut never leaves the head of a code point without its tail. */
    i32 k = n;
    while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) k--;
    if (k > 0) {
        unsigned char lead = (unsigned char)out[k - 1];
        i32 want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (n - (k - 1) < want) n = k - 1;
    }
    /* …and the space that led that character in goes with it. */
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
    return n;
}

#endif /* CORE_TEXT_SANITIZE_H */
