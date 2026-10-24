/*
 * Liu — where a Markdown link's destination points.
 *
 * `[text](destination)` hands the reader one string, and what to do with it
 * depends on its shape:
 *
 *   https://example.com/page    another program's business   EXTERNAL
 *   mailto:someone@example.com  the same — a scheme needs no "//"
 *   #installation               a heading of the note already open   FRAGMENT
 *   guide.md#installation       a file, and a heading inside it      PATH
 *   ../img/shot%20one.png       a file, written the way a URL is
 *
 * A "://" test alone cannot tell these apart: it would open "mailto:..." as a
 * file of that name, "#installation" as a file next to the note, and
 * "guide.md#installation" as a file named exactly that. The table of contents
 * at the top of every README is made of the second kind.
 *
 * The other half is the heading side: a fragment does not spell the heading,
 * it spells the ANCHOR a heading is given — "## What's new in C#" is reached
 * by "#whats-new-in-c". md_link_slug() is that rule (GitHub's, which is what
 * the files the reader opens were written against) and md_link_find_heading()
 * finds the block a fragment names.
 *
 * Pure: no state, no I/O, no allocation, nothing to link — only the AST's
 * types.
 */
#ifndef UI_MARKDOWN_LINK_TARGET_H
#define UI_MARKDOWN_LINK_TARGET_H

#include "core/types.h"
#include "ui/markdown/md_ast.h"

typedef enum {
    MD_LINK_NONE,        /* an empty destination: nothing to open */
    MD_LINK_EXTERNAL,    /* "scheme:…" — not this reader's to resolve */
    MD_LINK_FRAGMENT,    /* "#anchor" — a place in the document already open */
    MD_LINK_PATH,        /* a file, with an optional "#anchor" inside it */
} MdLinkKind;

/* A destination taken apart. Both slices point into the string that was
 * classified and are NOT NUL-terminated.
 *
 *   EXTERNAL  path = the whole destination, fragment empty
 *   FRAGMENT  path empty, fragment = what follows the '#' (may be empty: a
 *             bare "#" is the top of the document)
 *   PATH      path = up to the first '#', fragment = what follows it
 *
 * Neither part is decoded: "%20" is still "%20". md_link_percent_decode()
 * turns a part into the bytes it stands for. */
typedef struct {
    MdLinkKind  kind;
    const char *path;
    u32         path_len;
    const char *fragment;
    u32         fragment_len;
} MdLinkTarget;

/* Length of the URI scheme `s` opens with, colon excluded, or 0.
 *
 * RFC 3986: a letter, then letters, digits, '+', '-' or '.', then ':'. Two
 * characters at least, because "C:\notes\a.md" and "C:/notes/a.md" are paths
 * and one letter is all that tells them from a scheme; thirty-two at most,
 * CommonMark's own bound, so a sentence with a colon in it is not one. */
static inline u32 md_link_scheme_len(const char *s, usize len) {
    if (!s || len < 3) return 0;
    u8 c = (u8)s[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 0;
    u32 i = 1;
    while (i < len && i <= 32) {
        c = (u8)s[i];
        if (c == ':') return i >= 2 ? i : 0;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) return 0;
        i++;
    }
    return 0;
}

static inline MdLinkTarget md_link_classify(const char *dest, usize len) {
    MdLinkTarget t = { MD_LINK_NONE, NULL, 0, NULL, 0 };
    if (!dest) return t;
    while (len && (dest[0] == ' ' || dest[0] == '\t')) { dest++; len--; }
    while (len && (dest[len - 1] == ' ' || dest[len - 1] == '\t')) len--;
    if (len == 0) return t;
    if (len > 0xFFFFFFFFu) len = 0xFFFFFFFFu;
    if (md_link_scheme_len(dest, len)) {
        t.kind = MD_LINK_EXTERNAL;
        t.path = dest;
        t.path_len = (u32)len;
        return t;
    }
    usize hash = 0;
    while (hash < len && dest[hash] != '#') hash++;
    if (hash < len) {
        t.fragment     = dest + hash + 1;
        t.fragment_len = (u32)(len - hash - 1);
    }
    if (hash == 0) {
        t.kind = MD_LINK_FRAGMENT;
        return t;
    }
    t.kind     = MD_LINK_PATH;
    t.path     = dest;
    t.path_len = (u32)hash;
    return t;
}

/* Undo URL percent-encoding: "my%20note.md" → "my note.md". `out` is always
 * NUL-terminated; returns the bytes written, cut at cap-1. A "%00" and a '%'
 * that is not followed by two hex digits stay as written — a decoded NUL would
 * end the C string every caller is about to make of this. */
static inline usize md_link_percent_decode(const char *s, usize len,
                                           char *out, usize cap) {
    usize n = 0;
    if (!out || cap == 0) return 0;
    for (usize i = 0; s && i < len && n + 1 < cap; i++) {
        u8 c = (u8)s[i];
        if (c == '%' && i + 2 < len) {
            u8 h = (u8)s[i + 1], l = (u8)s[i + 2];
            i32 hv = (h >= '0' && h <= '9') ? h - '0'
                   : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                   : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
            i32 lv = (l >= '0' && l <= '9') ? l - '0'
                   : (l >= 'a' && l <= 'f') ? l - 'a' + 10
                   : (l >= 'A' && l <= 'F') ? l - 'A' + 10 : -1;
            if (hv >= 0 && lv >= 0 && (hv | lv) != 0) {
                out[n++] = (char)((hv << 4) | lv);
                i += 2;
                continue;
            }
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n;
}

/* ---- anchors ---------------------------------------------------------- */

/* One code point of UTF-8, lenient: a malformed byte is returned as U+FFFD
 * and consumed alone, so a walk always advances. */
static inline u32 md_link__decode(const u8 *s, usize len, u32 *cp) {
    u8 c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    u32 need = (c & 0xE0) == 0xC0 ? 1u : (c & 0xF0) == 0xE0 ? 2u
             : (c & 0xF8) == 0xF0 ? 3u : 0u;
    if (need == 0 || need >= len) { *cp = 0xFFFD; return 1; }
    u32 v = need == 1 ? (c & 0x1Fu) : need == 2 ? (c & 0x0Fu) : (c & 0x07u);
    for (u32 k = 1; k <= need; k++) {
        if ((s[k] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (s[k] & 0x3Fu);
    }
    *cp = v;
    return need + 1;
}

static inline u32 md_link__encode(u32 cp, char *out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Lower case, for the scripts whose capitals are a fixed distance from their
 * small letters: Latin (basic, Latin-1, Extended-A), Greek, Cyrillic. Not a
 * Unicode case table — an anchor has to agree with the one a heading was
 * given, and these are the alphabets headings are written in. */
static inline u32 md_link__lower(u32 cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp < 0xC0) return cp;
    if (cp <= 0xDE) return cp == 0xD7 ? cp : cp + 32;            /* À…Þ, not × */
    if (cp >= 0x100 && cp <= 0x17F) {
        if (cp == 0x178) return 0xFF;                             /* Ÿ */
        if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E))
            return (cp & 1) ? cp + 1 : cp;                        /* odd capitals */
        if (cp == 0x130 || cp == 0x138 || cp == 0x149 || cp == 0x17F) return cp;
        return (cp & 1) ? cp : cp + 1;                            /* even capitals */
    }
    if (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2) return cp + 32;   /* Α…Ϋ */
    if (cp >= 0x400 && cp <= 0x40F) return cp + 80;                  /* Ѐ…Џ */
    if (cp >= 0x410 && cp <= 0x42F) return cp + 32;                  /* А…Я */
    return cp;
}

/* Does a code point survive into an anchor? Letters, digits, '-' and '_' do;
 * punctuation and symbols do not (a space is handled by the caller). Outside
 * ASCII the answer is by BLOCK: the punctuation, symbol, arrow, dingbat and
 * emoji ranges are dropped and everything else is taken for a letter, which
 * is right for every script and avoids carrying the category tables. */
static inline bool md_link__keeps(u32 cp) {
    if (cp < 0x80)
        return (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') ||
               cp == '-' || cp == '_';
    if (cp < 0xC0)                       /* Latin-1 signs; ª µ º ² ³ ¹ ¼ ½ ¾ stay */
        return cp == 0xAA || cp == 0xB5 || cp == 0xBA || cp == 0xB2 ||
               cp == 0xB3 || cp == 0xB9 || (cp >= 0xBC && cp <= 0xBE);
    if (cp == 0xD7 || cp == 0xF7) return false;                  /* × ÷ */
    if (cp >= 0x2000 && cp <= 0x206F) return false;   /* general punctuation */
    if (cp >= 0x20A0 && cp <= 0x214F) return false;   /* currency, letterlike */
    if (cp >= 0x2190 && cp <= 0x2BFF) return false;   /* arrows … dingbats */
    if (cp >= 0x2E00 && cp <= 0x2E7F) return false;   /* more punctuation */
    if (cp >= 0x3000 && cp <= 0x303F) return false;   /* CJK punctuation */
    if (cp >= 0xFE00 && cp <= 0xFE6F) return false;   /* variation selectors, small forms */
    if (cp >= 0xFF00 && cp <= 0xFF0F) return false;   /* fullwidth punctuation */
    if (cp >= 0xFF1A && cp <= 0xFF20) return false;
    if (cp >= 0xFF3B && cp <= 0xFF40) return false;
    if (cp >= 0xFF5B && cp <= 0xFF65) return false;
    if (cp == 0xFFFD) return false;
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return false; /* emoji and pictographs */
    if (cp >= 0xE0000) return false;                  /* tags, private use */
    return true;
}

/* A slug under construction: text goes in a piece at a time (a heading is
 * several inline runs), the anchor comes out. `full` latches when a character
 * did not fit, so a long heading is cut at a character and stays cut. */
typedef struct { char *out; usize n, cap; bool full; } MdLinkSlug;

static inline void md_link__slug_put(MdLinkSlug *g, u32 cp) {
    char enc[4];
    u32 el = md_link__encode(cp, enc);
    if (g->full || g->n + el + 1 > g->cap) { g->full = true; return; }
    for (u32 k = 0; k < el; k++) g->out[g->n++] = enc[k];
}

static inline void md_link__slug_add(MdLinkSlug *g, const u8 *text, usize len) {
    usize i = 0;
    while (text && i < len) {
        u32 cp = 0;
        i += md_link__decode(text + i, len - i, &cp);
        if (cp == ' ') { md_link__slug_put(g, '-'); continue; }
        /* İ lower-cases to an i and a combining dot — two code points, and
         * the one capital the fixed-distance table cannot hold. */
        if (cp == 0x130) { md_link__slug_put(g, 'i'); md_link__slug_put(g, 0x307); continue; }
        cp = md_link__lower(cp);
        if (md_link__keeps(cp)) md_link__slug_put(g, cp);
    }
}

/* The anchor GitHub gives a heading that reads `text`: lower case, every
 * space a hyphen, punctuation dropped — "What's new in C#" → "whats-new-in-c".
 * Hyphens are not merged ("a - b" → "a---b") and nothing is trimmed, because
 * GitHub does neither. `out` is NUL-terminated; returns its length. This is
 * the anchor of the FIRST heading with that text: the second is the same with
 * "-1" appended, the third "-2" (md_link_find_heading counts them). */
static inline usize md_link_slug(const u8 *text, usize len, char *out, usize cap) {
    if (!out || cap == 0) return 0;
    MdLinkSlug g = { out, 0, cap, false };
    md_link__slug_add(&g, text, len);
    out[g.n] = '\0';
    return g.n;
}

/* The text a heading shows, run by run, into a slug: emphasis and links give
 * their content, a wikilink what it displays, a code span its code. A picture
 * and a footnote mark are not text. */
static inline void md_link__slug_runs(MdLinkSlug *g, const MdInline *runs, u32 n,
                                      u32 depth) {
    for (u32 i = 0; runs && i < n; i++) {
        const MdInline *r = &runs[i];
        switch (r->kind) {
        case MD_INLINE_IMAGE:
        case MD_INLINE_EMBED:
        case MD_INLINE_FOOTNOTE_REF:
            break;
        case MD_INLINE_LINEBREAK:
            md_link__slug_add(g, (const u8 *)" ", 1);
            break;
        case MD_INLINE_WIKILINK: {
            MdSlice d = r->alt.len ? r->alt : r->url.len ? r->url : r->anchor;
            md_link__slug_add(g, d.data, d.len);
            break;
        }
        default:
            if (r->children && r->child_count && depth < 16)
                md_link__slug_runs(g, r->children, r->child_count, depth + 1);
            else
                md_link__slug_add(g, r->text.data, r->text.len);
            break;
        }
    }
}

/* The anchor of heading block `b`, before any "-1" for a repeated one. */
static inline usize md_link_heading_slug(const MdBlock *b, char *out, usize cap) {
    if (!out || cap == 0) return 0;
    MdLinkSlug g = { out, 0, cap, false };
    if (b) md_link__slug_runs(&g, b->inlines, b->inline_count, 0);
    out[g.n] = '\0';
    return g.n;
}

#define MD_LINK_NO_HEADING 0xFFFFFFFFu
#define MD_LINK_SLUG_MAX   256

/* Index, among `doc`'s top-level blocks, of the heading a fragment names, or
 * MD_LINK_NO_HEADING. `fragment` is as written in the link, without its '#':
 * percent-escapes are undone and capitals folded here, so "#Getting-Started"
 * and "#%C3%B6zet" find what a browser would.
 *
 * Repeats count the way GitHub counts them: of three headings "Usage" the
 * first is "#usage", the second "#usage-1", the third "#usage-2". A heading
 * that really reads "Usage 1" owns "#usage-1" when it comes first. The index
 * is what MdRenderCtx.seek_block takes. */
static inline u32 md_link_find_heading(const MdDoc *doc, const char *fragment, usize len) {
    if (!doc || !doc->blocks || !fragment || len == 0) return MD_LINK_NO_HEADING;
    char raw[MD_LINK_SLUG_MAX], want[MD_LINK_SLUG_MAX];
    usize rn = md_link_percent_decode(fragment, len, raw, sizeof raw);
    /* Through the slug rule as well: it lower-cases, and a fragment that was
     * typed as the heading's own text ("#Getting Started") then matches too. */
    usize wn = md_link_slug((const u8 *)raw, rn, want, sizeof want);
    if (wn == 0) return MD_LINK_NO_HEADING;

    /* "name-3" is either a heading that reads so, or the fourth "name". */
    usize base_n = 0;
    u32   nth = 0;
    {
        usize d = wn;
        u32 mul = 1;
        while (d > 0 && want[d - 1] >= '0' && want[d - 1] <= '9' && wn - d < 6) {
            nth += (u32)(want[d - 1] - '0') * mul;
            mul *= 10;
            d--;
        }
        if (d < wn && d >= 2 && want[d - 1] == '-' && nth > 0) base_n = d - 1;
    }

    u32 seen_base = 0;
    for (u32 i = 0; i < doc->block_count; i++) {
        const MdBlock *b = &doc->blocks[i];
        if (b->kind != MD_BLOCK_HEADING) continue;
        char slug[MD_LINK_SLUG_MAX];
        usize sn = md_link_heading_slug(b, slug, sizeof slug);
        if (sn == wn) {
            bool same = true;
            for (usize k = 0; k < sn && same; k++) same = slug[k] == want[k];
            if (same) return i;
        }
        if (base_n && sn == base_n) {
            bool same = true;
            for (usize k = 0; k < sn && same; k++) same = slug[k] == want[k];
            if (same && seen_base++ == nth) return i;
        }
    }
    return MD_LINK_NO_HEADING;
}

#endif /* UI_MARKDOWN_LINK_TARGET_H */
