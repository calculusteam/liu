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

#include <limits.h>
#include <string.h>
#include "core/utf8.h"

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
    char quote = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < len) { i += 2; continue; }
        if (quote) {
            if (c == (unsigned char)quote) quote = 0;
            i++; continue;
        }
        if ((c == '"' || c == '\'') &&
            (i == start || s[i - 1] == '(' || s[i - 1] == '@')) {
            quote = (char)c; i++; continue;
        }
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
    i32 limit = len - i > 64 ? i + 64 : len;
    i32 close = -1;
    for (i32 j = i + 1; j < limit; j++) {
        if (s[j] == '\n' || s[j] == '\r' || s[j] == '[') break;
        if (s[j] == ']') { close = j; break; }
    }
    if (close < 0) return 0;
    char low[64];
    i32 m = 0;
    for (i32 j = i + 1; j < close && m < (i32)sizeof low - 1; j++) {
        char c = s[j];
        low[m++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    low[m] = '\0';
    i32 p;
    bool pasted = strncmp(low, "pasted text ", 12) == 0;
    /* What Claude Code leaves in the middle of a paste too long to keep:
     * "[...Truncated text #1 +120 lines...]". An attachment like the others. */
    bool cut = strncmp(low, "...truncated text ", 18) == 0;
    if (pasted) p = 12;
    else if (cut) p = 18;
    else if (strncmp(low, "image ", 6) == 0) p = 6;
    else return 0;
    if (low[p] == '#') p++;
    i32 number = p;
    while (low[p] >= '0' && low[p] <= '9') p++;
    if (p == number) return 0;
    if ((pasted || cut) && low[p] == ' ' && low[p + 1] == '+') {
        p += 2;
        number = p;
        while (low[p] >= '0' && low[p] <= '9') p++;
        if (p == number) return 0;
        if (cut) return strcmp(low + p, " lines...") == 0 ? close + 1 : 0;
        if (strcmp(low + p, " lines") != 0 && strcmp(low + p, " line") != 0) return 0;
        return close + 1;
    }
    if (cut) return 0;
    return low[p] == '\0' ? close + 1 : 0;
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
    if (t[0] == '.' && t[1] != '.') return true;              /* .env */
    if ((n >= 3 && ((t[0] >= 'A' && t[0] <= 'Z') ||
                    (t[0] >= 'a' && t[0] <= 'z')) && t[1] == ':' &&
                    (t[2] == '/' || t[2] == '\\')) ||
        (t[0] == '\\' && t[1] == '\\') || t[0] == '$') return true;
    if (t[0] == '/') return true;                              /* /abs/path */
    if (t[0] == '~' && t[1] == '/') return true;               /* ~/path    */
    if (t[0] == '.' && t[1] == '/') return true;               /* ./rel     */
    if (n >= 3 && t[0] == '.' && t[1] == '.' && t[2] == '/') return true;
    if (seg_prefix(t, n, "http://") ||
        seg_prefix(t, n, "https://") ||
        seg_prefix(t, n, "file://")) return true;
    if (((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= 'A' && t[0] <= 'Z')) &&
        !(n == 7 && memcmp(t, "ve/veya", 7) == 0) &&
        !(n == 6 && memcmp(t, "and/or", 6) == 0)) {
        for (i32 i = 1; i < n; i++) if (t[i] == '/') return true;
    }
    if (t[0] == '@') {                                         /* @src/x.c  */
        for (i32 i = 1; i < n; i++) {
            if (t[i] == '/' || t[i] == '.') return true;
        }
    }
    /* Relative filenames, including src/main.c and screenshot.png. A dot
     * must have a filename on both sides; sentence-final punctuation is not
     * an extension. Slash-only prose is resolved by the optional FS probe. */
    i32 end = n;
    while (end > 0 && strchr(".,;:!?)]}>\"'", t[end - 1])) end--;
    if (end > 1 && t[end - 1] == '/') return true;
    if (seg_prefix(t, n, "src/") || seg_prefix(t, n, "tests/") ||
        seg_prefix(t, n, "assets/") || seg_prefix(t, n, "docs/")) return true;
    i32 dot = -1;
    for (i32 i = 1; i < end; i++) if (t[i] == '.') dot = i;
    if (dot >= 1 && dot + 1 < end) {
        i32 ext = end - dot - 1;
        bool alpha = true;
        for (i32 i = dot + 1; i < end; i++) {
            unsigned char c = (unsigned char)t[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9'))) alpha = false;
        }
        if (alpha && (ext > 1 || strchr("chmrCHMR", t[dot + 1]))) return true;
    }
    return false;
}

/* Does this token open a filesystem path? Only these may be grown across
 * spaces — a URL or an @file reference never carries a raw space, and a
 * bracket chip has its own terminator. Wrapper punctuation disqualifies it:
 * "(/tmp/a" would grow past the closing paren into prose. */
static bool seg_tok_is_bare_path(const char *t, i32 n) {
    if (n < 2) return false;
    if (t[0] == '/') return true;
    if (t[0] == '~' && t[1] == '/') return true;
    if (t[0] == '.' && t[1] == '/') return true;
    if (n >= 3 && t[0] == '.' && t[1] == '.' && t[2] == '/') return true;
    return false;
}

/* Longest extension of the path starting at `tok_start` that names a file
 * that actually exists, growing one space-separated token at a time.
 * Returns `tok_end` when nothing longer exists, so a path without spaces —
 * and prose that merely follows one — is left exactly as the tokenizer had
 * it. Only plain spaces are crossed: a newline or a tab is a real boundary. */
static i32 seg_path_grow(const char *s, i32 len, i32 tok_start, i32 tok_end,
                         TranslatePathProbe probe, void *ud) {
    if (!probe) return tok_end;
    char first = s[tok_start];
    if (first == '"' || first == '\'' || first == '(' || first == '<' || first == '@' ||
        seg_prefix(s + tok_start, tok_end - tok_start, "http") ||
        seg_prefix(s + tok_start, tok_end - tok_start, "file://")) return tok_end;
    bool bare = seg_tok_is_bare_path(s + tok_start, tok_end - tok_start);

    i32 best = tok_end;
    i32 j = tok_end;
    for (i32 tries = 0; tries < TRANSLATE_PATH_MAX_SPACE_TOKENS; tries++) {
        i32 ws = j;
        while (ws < len && s[ws] == ' ') ws++;
        if (ws == j || ws >= len) break;          /* no gap, or end of line */
        if (seg_bracket_end(s, len, ws) > 0) break;
        i32 nxt = seg_token_end(s, len, ws);
        if (nxt <= ws) break;
        j = nxt;
        if ((bare || seg_tok_is_keep(s + ws, nxt - ws)) &&
            probe(s + tok_start, j - tok_start, ud)) best = j;
    }
    return best;
}

i32 translate_segment_split(const char *text, TranslateSeg *out, i32 cap) {
    return translate_segment_split_ex(text, out, cap, NULL, NULL);
}

i32 translate_segment_split_ex(const char *text, TranslateSeg *out, i32 cap,
                               TranslatePathProbe probe, void *ud) {
    if (!text || !out || cap <= 0) return 0;
    usize text_len = strlen(text);
    if (text_len > (usize)INT32_MAX) return 0;
    i32 len = (i32)text_len;
    i32 n = 0;
    i32 text_start = -1;   /* start of the pending TEXT run, -1 = none */
    i32 i = 0;

    while (i < len) {
        if (seg_is_ws((unsigned char)text[i])) { i++; continue; }

        i32 tok_start = i;
        i32 tok_end;
        bool keep;
        bool chip = false;
        i32 br = seg_bracket_end(text, len, i);
        if (br > 0) {
            tok_end = br;
            keep = true;
            chip = true;
        } else {
            tok_end = seg_token_end(text, len, i);
            keep = seg_tok_is_keep(text + tok_start, tok_end - tok_start);
            if (!keep && probe)
                keep = probe(text + tok_start, tok_end - tok_start, ud);
            /* An unescaped path with spaces looks like a path followed by
             * words; ask the filesystem where it really ends. */
            i32 grown = seg_path_grow(text, len, tok_start, tok_end, probe, ud);
            if (grown > tok_end) { tok_end = grown; keep = true; }
        }

        if (!keep) {
            if (text_start < 0) text_start = tok_start;
            i = tok_end;
            continue;
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
            if (n == cap) return -1;
            out[n].kind = TRANSLATE_SEG_TEXT;
            out[n].off  = text_start;
            out[n].len  = ks - text_start;
            n++;
        }
        text_start = -1;

        /* Only plain KEEPs merge. A chip is one editing unit of its own, so
         * folding it together with a neighbour would lose the count of ←/→
         * presses needed to step over it. */
        if (!chip && n > 0 && out[n - 1].kind == TRANSLATE_SEG_KEEP &&
            out[n - 1].off + out[n - 1].len == ks) {
            out[n - 1].len = ke - out[n - 1].off;   /* merge adjacent KEEPs */
        } else {
            if (n == cap) return -1;
            out[n].kind = chip ? TRANSLATE_SEG_CHIP : TRANSLATE_SEG_KEEP;
            out[n].off  = ks;
            out[n].len  = ke - ks;
            n++;
        }
        i = ke;
    }

    /* Fail closed on overflow: protected tokens must never become TEXT. */
    if (text_start >= 0) {
        i32 end = len;
        while (end > text_start && seg_is_ws((unsigned char)text[end - 1])) end--;
        if (end > text_start) {
            if (n == cap) return -1;
            out[n].kind = TRANSLATE_SEG_TEXT;
            out[n].off  = text_start;
            out[n].len  = end - text_start;
            n++;
        }
    }

    /* Editing units, now that every segment's extent is final. */
    for (i32 k = 0; k < n; k++) {
        out[k].units = translate_segment_units(text, &out[k]);
    }
    return n;
}

/* Characters in [off, off+len), counting a UTF-8 sequence once. */
static i32 seg_count_chars(const char *text, i32 off, i32 len) {
    i32 chars = 0;
    for (i32 i = off; i < off + len;) {
        u32 cp;
        i32 cols;
        u32 n = utf8_next_grapheme((const u8 *)text + i,
                                  (usize)(off + len - i), &cp, &cols);
        if (!n) return -1;
        i += (i32)n;
        chars++;
    }
    return chars;
}

/* A line worth translating has a letter in it: an ASCII one, or anything
 * past ASCII. Digits, bullets and fences alone are typed back as they are. */
static bool seg_line_has_letter(const char *s, i32 from, i32 to) {
    for (i32 i = from; i < to; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    }
    return false;
}

i32 translate_segment_split_lines(const char *text, TranslateSeg *out, i32 cap) {
    if (!text || !out || cap <= 0) return 0;
    usize text_len = strlen(text);
    if (text_len > (usize)INT32_MAX) return 0;
    i32 len = (i32)text_len, n = 0, texts = 0, prev_end = -1;
    for (i32 i = 0; i <= len;) {
        i32 eol = i;
        while (eol < len && text[eol] != '\n') eol++;
        i32 a = i, b = eol;
        while (a < b && seg_is_ws((unsigned char)text[a])) a++;
        while (b > a && seg_is_ws((unsigned char)text[b - 1])) b--;
        if (b > a) {
            if (prev_end >= 0) {
                /* Everything from the last line's last word to this line's
                 * first: its line breaks are what is typed back. */
                if (n == cap) return -1;
                out[n++] = (TranslateSeg){ TRANSLATE_SEG_BREAK, prev_end, a - prev_end, 0 };
            }
            if (n == cap) return -1;
            bool words = seg_line_has_letter(text, a, b);
            out[n++] = (TranslateSeg){ words ? TRANSLATE_SEG_TEXT : TRANSLATE_SEG_KEEP, a, b - a, 0 };
            texts += words;
            prev_end = b;
        }
        i = eol + 1;
    }
    if (!texts) return 0;
    for (i32 k = 0; k < n; k++) out[k].units = translate_segment_units(text, &out[k]);
    return n;
}

i32 translate_segment_units(const char *text, const TranslateSeg *sg) {
    if (!text || !sg || sg->len <= 0) return 0;
    if (sg->kind == TRANSLATE_SEG_BREAK) {
        i32 breaks = 0;
        for (i32 i = sg->off; i < sg->off + sg->len; i++) breaks += text[i] == '\n';
        return breaks;
    }
    i32 chars = seg_count_chars(text, sg->off, sg->len);
    if (sg->kind != TRANSLATE_SEG_CHIP) return chars;

    /* The chip collapses to a single unit; the whitespace it absorbed is
     * still one unit per character. */
    i32 lb = -1, rb = -1;
    for (i32 i = sg->off; i < sg->off + sg->len; i++) {
        if (text[i] == '[' && lb < 0) lb = i;
        if (text[i] == ']') rb = i;
    }
    if (lb < 0 || rb < lb) return chars;
    i32 chip_chars = seg_count_chars(text, lb, rb - lb + 1);
    i32 units = chars - chip_chars + 1;
    return units > 0 ? units : 1;
}

bool translate_segment_has_text(const TranslateSeg *segs, i32 count) {
    if (!segs) return false;
    for (i32 i = 0; i < count; i++) {
        if (segs[i].kind == TRANSLATE_SEG_TEXT) return true;
    }
    return false;
}
