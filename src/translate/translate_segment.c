/*
 * Liu - Translate-on-Tab: prompt segmenter.
 *
 * See translate_segment.h for the contract. The scanner walks
 * whitespace-separated tokens (backslash-escaped bytes never break a
 * token, matching drop_path_escape's output), classifies each one, and
 * folds the result into TEXT / KEEP segments. KEEP segments absorb the
 * whitespace touching them so reassembly is exact; TEXT segments come
 * out trimmed, ready to be handed to a translation backend.
 */
#include "translate/translate_segment.h"

#include <string.h>

static bool seg_is_ws(unsigned char c) {
    return c <= ' ';
}

static i32 seg_bracket_end(const char *s, i32 len, i32 i);

/* Is the whitespace byte at `pos` escaped — i.e. preceded by an odd run
 * of backslashes? Keeps the KEEP back-absorption from splitting a "\ "
 * escape pair across a segment boundary. */
static bool seg_ws_escaped(const char *s, i32 floor_off, i32 pos) {
    i32 bs = 0;
    while (pos - 1 - bs >= floor_off && s[pos - 1 - bs] == '\\') bs++;
    return (bs & 1) != 0;
}

/* Advance past one token. A backslash escapes the next byte — dropped
 * paths arrive as "/tmp/My\ Shot.png" — so "\ " stays inside the token.
 * An attachment chip glued to a word ("bak[Image #1]") also ends the
 * token so the chip is picked up as its own KEEP on the next scan. */
static i32 seg_token_end(const char *s, i32 len, i32 i) {
    i32 start = i;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < len) { i += 2; continue; }
        if (seg_is_ws(c)) break;
        if (c == '[' && i > start && seg_bracket_end(s, len, i) > 0) break;
        i++;
    }
    return i;
}

/* Case-insensitive ASCII prefix check. */
static bool seg_prefix(const char *t, i32 n, const char *p) {
    i32 pl = (i32)strlen(p);
    if (n < pl) return false;
    for (i32 i = 0; i < pl; i++) {
        char a = t[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != p[i]) return false;
    }
    return true;
}

/* "[Image #1]" / "[Pasted text #2 +12 lines]" — attachment chips that
 * agent TUIs draw in the prompt (visual capture picks them up) or that
 * the user types to reference an attachment. Returns the offset just
 * past ']' when s+i starts such a placeholder, else 0. Real chips have
 * a rigid shape — the bracket body STARTS with the keyword and carries
 * a number — so the match is anchored and requires a digit; ordinary
 * bracketed prose like "[image boyutu]" stays translatable text. */
static i32 seg_bracket_end(const char *s, i32 len, i32 i) {
    if (s[i] != '[') return 0;
    i32 limit = i + 64 < len ? i + 64 : len;
    i32 close = -1;
    for (i32 j = i + 1; j < limit; j++) {
        if (s[j] == '\n' || s[j] == '\r' || s[j] == '[') break;
        if (s[j] == ']') { close = j; break; }
    }
    if (close < 0) return 0;
    char low[64];
    i32 m = 0;
    bool has_digit = false;
    for (i32 j = i + 1; j < close && m < (i32)sizeof low - 1; j++) {
        char c = s[j];
        if (c >= '0' && c <= '9') has_digit = true;
        low[m++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    low[m] = '\0';
    if (!has_digit) return 0;
    if (strncmp(low, "image", 5) == 0 || strncmp(low, "pasted", 6) == 0) {
        return close + 1;
    }
    return 0;
}

/* Does this token name something that must survive translation
 * byte-for-byte? Paths, URLs and @file references qualify; a token
 * merely containing a slash ("ve/veya") does not. */
static bool seg_tok_is_keep(const char *t, i32 n) {
    /* Unwrap before classifying: a leading "\ " escape pair or common
     * wrapper punctuation hides the real prefix — "(https://x)" and
     * "\ /tmp/a.png" must still be kept. The wrapper bytes stay inside
     * the KEEP token (punctuation passes through translation-neutral). */
    for (;;) {
        if (n >= 2 && t[0] == '\\' && seg_is_ws((unsigned char)t[1])) {
            t += 2; n -= 2; continue;
        }
        if (n >= 1 && (t[0] == '(' || t[0] == '<' ||
                       t[0] == '"' || t[0] == '\'')) {
            t += 1; n -= 1; continue;
        }
        break;
    }
    if (n < 2) return false;
    if (t[0] == '/') return true;                              /* /abs/path */
    if (t[0] == '~' && t[1] == '/') return true;               /* ~/path    */
    if (t[0] == '.' && t[1] == '/') return true;               /* ./rel     */
    if (n >= 3 && t[0] == '.' && t[1] == '.' && t[2] == '/') return true;
    if (seg_prefix(t, n, "http://") ||
        seg_prefix(t, n, "https://") ||
        seg_prefix(t, n, "file://")) return true;
    if (t[0] == '@') {                                         /* @src/x.c  */
        for (i32 i = 1; i < n; i++) {
            if (t[i] == '/' || t[i] == '.') return true;
        }
    }
    return false;
}

i32 translate_segment_split(const char *text, TranslateSeg *out, i32 cap) {
    if (!text || !out || cap <= 0) return 0;
    i32 len = (i32)strlen(text);
    i32 n = 0;
    i32 text_start = -1;   /* start of the pending TEXT run, -1 = none */
    i32 i = 0;

    while (i < len) {
        if (seg_is_ws((unsigned char)text[i])) { i++; continue; }

        i32 tok_start = i;
        i32 tok_end;
        bool keep;
        i32 br = seg_bracket_end(text, len, i);
        if (br > 0) {
            tok_end = br;
            keep = true;
        } else {
            tok_end = seg_token_end(text, len, i);
            keep = seg_tok_is_keep(text + tok_start, tok_end - tok_start);
        }

        if (!keep) {
            if (text_start < 0) text_start = tok_start;
            i = tok_end;
            continue;
        }

        /* Accepting this KEEP can push two segments (pending TEXT + the
         * KEEP itself); keep one more slot in reserve for the trailing
         * TEXT segment so the tail of an over-long prompt is folded into
         * TEXT, never lost. */
        if (n >= cap - 2) {
            if (text_start < 0) text_start = tok_start;
            break;
        }

        /* Absorb the whitespace on both sides into the KEEP segment.
         * Backing up stops at the first non-ws byte — and refuses to
         * cross an escaped whitespace byte ("\ "), which the tokenizer
         * treats as token-internal — so it can never eat into the
         * pending TEXT run's content. */
        i32 floor_off = n > 0 ? out[n - 1].off + out[n - 1].len : 0;
        i32 ks = tok_start;
        while (ks > floor_off && seg_is_ws((unsigned char)text[ks - 1]) &&
               !seg_ws_escaped(text, floor_off, ks - 1)) {
            ks--;
        }
        i32 ke = tok_end;
        while (ke < len && seg_is_ws((unsigned char)text[ke])) ke++;

        if (text_start >= 0 && text_start < ks) {
            out[n].kind = TRANSLATE_SEG_TEXT;
            out[n].off  = text_start;
            out[n].len  = ks - text_start;
            n++;
        }
        text_start = -1;

        if (n > 0 && out[n - 1].kind == TRANSLATE_SEG_KEEP &&
            out[n - 1].off + out[n - 1].len == ks) {
            out[n - 1].len = ke - out[n - 1].off;   /* merge adjacent KEEPs */
        } else {
            out[n].kind = TRANSLATE_SEG_KEEP;
            out[n].off  = ks;
            out[n].len  = ke - ks;
            n++;
        }
        i = ke;
    }

    /* Flush the trailing TEXT run (also the overflow remainder). */
    if (text_start >= 0) {
        i32 end = len;
        while (end > text_start && seg_is_ws((unsigned char)text[end - 1])) end--;
        if (end > text_start && n < cap) {
            out[n].kind = TRANSLATE_SEG_TEXT;
            out[n].off  = text_start;
            out[n].len  = end - text_start;
            n++;
        }
    }
    return n;
}

bool translate_segment_has_text(const TranslateSeg *segs, i32 count) {
    if (!segs) return false;
    for (i32 i = 0; i < count; i++) {
        if (segs[i].kind == TRANSLATE_SEG_TEXT) return true;
    }
    return false;
}
