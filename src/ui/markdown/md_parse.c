/*
 * Liu - Markdown parser implementation.
 *
 * Two-stage strategy:
 *
 *   1. block_parse — walks source line-by-line, builds a heap-allocated
 *      vector of MdBlock with each block's inline slice attached. Multi-line
 *      blocks (fenced code, callouts, blockquotes, tables, lists) consume
 *      multiple input lines. Tables also build a per-cell inline-slice grid.
 *
 *   2. inline_parse_for — for each block's stored inline slice, walks
 *      codepoints with utf8_decode and emits a vector of MdInline runs
 *      (text, emphasis, code, links, wikilinks, embeds, images, autolinks,
 *      tags, footnote refs, strikethrough, hard breaks).
 *
 *   3. finalize — copies all heap vecs into the parse arena, leaving the
 *      caller with a doc whose pointers all live in `arena`.
 *
 * Source bytes outlive the doc (filebrowser keeps fb->view_content alive
 * until viewer close), so AST slices point directly into source — no
 * per-string copies.
 */
#include "ui/markdown/md_parse.h"
#include "core/utf8.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Heap vectors used during parse
 * ========================================================================= */

typedef struct {
    MdBlock *data;
    u32      len, cap;
} BlockVec;

typedef struct {
    MdInline *data;
    u32       len, cap;
} InlineVec;

typedef struct {
    MdTableCell *data;
    u32          len, cap;
} CellVec;

static void *xrealloc(void *p, usize n) {
    void *r = realloc(p, n ? n : 1);
    return r;
}

/* Returns true on success, false if realloc failed (caller must abort the
 * push). Designed for non-void callers that need to bail with their own
 * return type — bb_emit returns NULL, inline pushers return silently. */
#define VEC_RESERVE(vec, type)                                                   \
    (((vec)->len < (vec)->cap)                                                    \
        ? true                                                                    \
        : ((vec)->cap = (vec)->cap ? (vec)->cap * 2u : 8u,                        \
           (vec)->data = (type *)xrealloc((vec)->data,                            \
                                          (usize)(vec)->cap * sizeof *(vec)->data),\
           (vec)->data != NULL))

#define VEC_PUSH(vec, type)                                                       \
    do { if (!VEC_RESERVE((vec), type)) {                                          \
            (vec)->cap = 0; (vec)->len = 0; return;                                \
        } } while (0)

/* =========================================================================
 * Slice helpers
 * ========================================================================= */

static MdSlice slice_make(const u8 *s, u32 n) { return (MdSlice){ s, n }; }
static MdSlice slice_empty(void)              { return (MdSlice){ NULL, 0 }; }

/* Trim leading + trailing ASCII whitespace from a slice. */
static MdSlice slice_trim(MdSlice s) {
    while (s.len && (s.data[0] == ' ' || s.data[0] == '\t')) { s.data++; s.len--; }
    while (s.len && (s.data[s.len-1] == ' ' || s.data[s.len-1] == '\t' ||
                     s.data[s.len-1] == '\r')) { s.len--; }
    return s;
}

static bool slice_eq_cstr(MdSlice s, const char *cstr) __attribute__((unused));
static bool slice_eq_cstr(MdSlice s, const char *cstr) {
    usize n = strlen(cstr);
    return s.len == n && memcmp(s.data, cstr, n) == 0;
}

static bool slice_eq_ci_cstr(MdSlice s, const char *cstr) {
    usize n = strlen(cstr);
    if (s.len != n) return false;
    for (usize i = 0; i < n; i++) {
        u8 a = s.data[i], b = (u8)cstr[i];
        if (a >= 'A' && a <= 'Z') a = (u8)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (u8)(b + 32);
        if (a != b) return false;
    }
    return true;
}

/* =========================================================================
 * Per-line scanner — yields next line as a slice without copying.
 * ========================================================================= */

typedef struct {
    const u8 *src;
    usize     len;
    usize     pos;
} LineScanner;

static bool scan_line(LineScanner *s, MdSlice *out) {
    if (s->pos >= s->len) return false;
    usize start = s->pos;
    while (s->pos < s->len && s->src[s->pos] != '\n') s->pos++;
    usize end = s->pos;
    if (s->pos < s->len) s->pos++;          /* skip '\n' */
    /* drop trailing '\r' */
    if (end > start && s->src[end - 1] == '\r') end--;
    *out = slice_make(s->src + start, (u32)(end - start));
    return true;
}

/* =========================================================================
 * Block-level helpers — classification predicates
 * ========================================================================= */

static u32 leading_spaces(MdSlice line) {
    u32 i = 0;
    while (i < line.len && line.data[i] == ' ') i++;
    return i;
}

/* Is `body` a horizontal-rule line? Three or more of '-', '*', or '_',
 * possibly separated by single spaces, with nothing else. */
static bool is_hr(MdSlice body) {
    if (body.len < 3) return false;
    u8 marker = body.data[0];
    if (marker != '-' && marker != '*' && marker != '_') return false;
    u32 count = 0;
    for (u32 i = 0; i < body.len; i++) {
        u8 c = body.data[i];
        if (c == marker) count++;
        else if (c == ' ' || c == '\t') continue;
        else return false;
    }
    return count >= 3;
}

/* Is `body` an ATX heading? On match, fills *level (1..6) and *text_start. */
static bool is_atx_heading(MdSlice body, u8 *level_out, u32 *text_start) {
    u32 i = 0;
    while (i < body.len && body.data[i] == '#' && i < 7) i++;
    if (i == 0 || i > 6) return false;
    if (i < body.len && body.data[i] != ' ') return false;
    *level_out = (u8)i;
    *text_start = i < body.len ? i + 1 : i;
    return true;
}

/* Match table separator row: each pipe-bounded cell is /:?-+:?/. */
static bool is_table_separator(MdSlice body, u16 *cols_out, char *aligns) {
    /* Strip leading/trailing pipe + whitespace. */
    u32 i = 0, end = body.len;
    while (i < end && (body.data[i] == ' ' || body.data[i] == '\t')) i++;
    if (i < end && body.data[i] == '|') i++;
    while (end > i && (body.data[end-1] == ' ' || body.data[end-1] == '\t')) end--;
    if (end > i && body.data[end-1] == '|') end--;
    if (i >= end) return false;

    u16 cols = 0;
    while (i < end) {
        u32 cs = i;
        while (i < end && body.data[i] != '|') i++;
        u32 ce = i;
        /* trim cell */
        while (cs < ce && (body.data[cs] == ' ' || body.data[cs] == '\t')) cs++;
        while (ce > cs && (body.data[ce-1] == ' ' || body.data[ce-1] == '\t')) ce--;
        if (cs >= ce) return false;
        bool lc = body.data[cs] == ':';
        bool rc = body.data[ce-1] == ':';
        u32 ds = cs + (lc ? 1 : 0);
        u32 de = ce - (rc ? 1 : 0);
        if (ds >= de) return false;
        for (u32 k = ds; k < de; k++) if (body.data[k] != '-') return false;
        /* aligns is a 32-entry buffer; cap the reported column count to it.
         * Counting past 32 made the caller memcpy `cols` bytes out of the
         * 32-byte stack buffer (over-read). Extra columns are simply dropped. */
        if (cols < 32) {
            aligns[cols] = lc && rc ? MD_ALIGN_CENTER
                          : rc       ? MD_ALIGN_RIGHT
                                     : MD_ALIGN_LEFT;
            cols++;
        }
        if (i < end && body.data[i] == '|') i++;
    }
    if (cols == 0) return false;
    *cols_out = cols;
    return true;
}

/* Count pipes in a line (rough table-row predicate). */
static bool looks_like_table_row(MdSlice body) {
    bool has_pipe = false;
    u32  visible  = 0;
    for (u32 i = 0; i < body.len; i++) {
        u8 c = body.data[i];
        if (c == '|') has_pipe = true;
        else if (c != ' ' && c != '\t') visible++;
    }
    return has_pipe && visible > 0;
}

/* Map "[!type]" tag to MdCalloutKind. Unknown ⇒ NOTE. */
static MdCalloutKind callout_lookup(MdSlice tag) {
    if (slice_eq_ci_cstr(tag, "info"))     return MD_CALLOUT_INFO;
    if (slice_eq_ci_cstr(tag, "tip") ||
        slice_eq_ci_cstr(tag, "hint"))     return MD_CALLOUT_TIP;
    if (slice_eq_ci_cstr(tag, "success") ||
        slice_eq_ci_cstr(tag, "check") ||
        slice_eq_ci_cstr(tag, "done"))     return MD_CALLOUT_SUCCESS;
    if (slice_eq_ci_cstr(tag, "warning") ||
        slice_eq_ci_cstr(tag, "attention") ||
        slice_eq_ci_cstr(tag, "caution"))  return MD_CALLOUT_WARNING;
    if (slice_eq_ci_cstr(tag, "danger") ||
        slice_eq_ci_cstr(tag, "error") ||
        slice_eq_ci_cstr(tag, "fail"))     return MD_CALLOUT_DANGER;
    if (slice_eq_ci_cstr(tag, "failure"))  return MD_CALLOUT_FAILURE;
    if (slice_eq_ci_cstr(tag, "bug"))      return MD_CALLOUT_BUG;
    if (slice_eq_ci_cstr(tag, "question") ||
        slice_eq_ci_cstr(tag, "help") ||
        slice_eq_ci_cstr(tag, "faq"))      return MD_CALLOUT_QUESTION;
    if (slice_eq_ci_cstr(tag, "example"))  return MD_CALLOUT_EXAMPLE;
    if (slice_eq_ci_cstr(tag, "quote") ||
        slice_eq_ci_cstr(tag, "cite"))     return MD_CALLOUT_QUOTE;
    if (slice_eq_ci_cstr(tag, "abstract") ||
        slice_eq_ci_cstr(tag, "summary") ||
        slice_eq_ci_cstr(tag, "tldr"))     return MD_CALLOUT_ABSTRACT;
    if (slice_eq_ci_cstr(tag, "todo"))     return MD_CALLOUT_TODO;
    return MD_CALLOUT_NOTE;
}

/* =========================================================================
 * Inline parser
 *
 * Single-pass tokenizer with non-recursive emphasis (emphasis ranges are
 * delimited spans whose inner content is rendered as plain styled text).
 * Triple delimiters `***x***` produce MD_INLINE_BOLD_ITALIC. Strikethrough
 * uses the same model.
 *
 * UTF-8 awareness: every codepoint advance goes through utf8_decode so
 * Turkish, CJK, emoji, RTL render as whole characters. Invalid bytes are
 * skipped (advance by 1) without aborting.
 *
 * Limitations (acceptable for v1):
 *   - emphasis bodies are not recursively parsed; "**bold *italic***"
 *     keeps the outer style and embeds the inner stars literally.
 *   - link text is rendered plain (no inline formatting inside [text]).
 * ========================================================================= */

static bool is_tag_start(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '/';
}
static bool is_tag_cont(u8 c) {
    return is_tag_start(c) || c == '-';
}

static bool is_url_scheme_char(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '+' || c == '.' || c == '-';
}
static bool is_url_char(u8 c) {
    return c > ' ' && c != '<' && c != '>' && c != '"' && c != '`' &&
           c != '|' && c != '\\' && c != '(' && c != ')';
}

/* Advance one codepoint, returning bytes consumed (>=1 even for invalid). */
static u32 cp_advance(const u8 *p, u32 left) {
    u32 cp = 0;
    u32 n = utf8_decode(p, left, &cp);
    return n ? n : 1;
}

/* Find a closing delimiter run of `n` matching chars starting search at
 * `from`. Returns position of closer's first byte, or -1 if not found.
 * Skips over backslash-escapes and inline-code spans so they don't count
 * as closers. */
static i64 find_delim(MdSlice s, u32 from, u8 ch, u32 n) {
    u32 i = from;
    while (i < s.len) {
        u8 c = s.data[i];
        if (c == '\\' && i + 1 < s.len) { i += 2; continue; }
        if (c == '`') {
            /* skip code span */
            u32 run = 0;
            while (i + run < s.len && s.data[i + run] == '`') run++;
            u32 j = i + run;
            while (j < s.len) {
                if (s.data[j] == '`') {
                    u32 run2 = 0;
                    while (j + run2 < s.len && s.data[j + run2] == '`') run2++;
                    if (run2 == run) { j += run2; break; }
                    j += run2;
                } else {
                    j++;
                }
            }
            i = j;
            continue;
        }
        if (c == ch) {
            u32 run = 0;
            while (i + run < s.len && s.data[i + run] == ch) run++;
            if (run >= n) return (i64)i;
            i += run;
            continue;
        }
        i++;
    }
    return -1;
}

/* Push a TEXT run covering [start, end) bytes. Splits at hard-break
 * trigger ("  \n") into TEXT + LINEBREAK. */
static void push_text_run(InlineVec *v, MdSlice slice, u32 start, u32 end) {
    if (end <= start) return;
    /* No special handling here; LINEBREAK is detected at the block scanner
     * when stitching multi-line paragraph slices. */
    VEC_PUSH(v, MdInline);
    MdInline *r = &v->data[v->len++];
    memset(r, 0, sizeof *r);
    r->kind = MD_INLINE_TEXT;
    r->text = slice_make(slice.data + start, end - start);
}

static void push_styled_run(InlineVec *v, u8 kind, MdSlice body) {
    VEC_PUSH(v, MdInline);
    MdInline *r = &v->data[v->len++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->text = body;
}

static void push_link_run(InlineVec *v, u8 kind, MdSlice text, MdSlice url, MdSlice alt) {
    VEC_PUSH(v, MdInline);
    MdInline *r = &v->data[v->len++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->text = text;
    r->url  = url;
    r->alt  = alt;
}

__attribute__((unused))
static void push_simple(InlineVec *v, u8 kind) {
    VEC_PUSH(v, MdInline);
    MdInline *r = &v->data[v->len++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
}

/* Recursion cap for nested inline emphasis (bold-in-italic, formatted link
 * text). Beyond this the body renders as flat text. */
#define MD_MAX_INLINE_NEST 8

static void inline_tokenize(MdSlice slice, InlineVec *out, Arena *arena, int depth);

/* Tokenize `body` into the run's children (arena-owned). `out->data` is not
 * touched by the recursion (it fills a separate temp vec), so `r` stays valid. */
static void push_children_from(MdInline *r, MdSlice body, Arena *arena, int depth) {
    if (!arena || depth >= MD_MAX_INLINE_NEST || body.len == 0) return;
    InlineVec sub = {0};
    inline_tokenize(body, &sub, arena, depth + 1);
    if (sub.len) {
        MdInline *kids = arena_alloc(arena, (usize)sub.len * sizeof(MdInline));
        if (kids) {
            memcpy(kids, sub.data, (usize)sub.len * sizeof(MdInline));
            r->children = kids;
            r->child_count = sub.len;
        }
    }
    free(sub.data);
}

/* Emphasis run whose body is recursively tokenized (so **bold *italic*** and
 * the like nest). `text` keeps the body slice for back-compat readers. */
static void push_emph(InlineVec *out, u8 kind, MdSlice body, Arena *arena, int depth) {
    VEC_PUSH(out, MdInline);
    u32 idx = out->len++;
    memset(&out->data[idx], 0, sizeof(MdInline));
    out->data[idx].kind = kind;
    out->data[idx].text = body;
    push_children_from(&out->data[idx], body, arena, depth);
}

/* Link run whose display text is recursively tokenized (so [**bold**](url)
 * formats). */
static void push_link_rec(InlineVec *out, u8 kind, MdSlice text, MdSlice url,
                          MdSlice alt, Arena *arena, int depth) {
    VEC_PUSH(out, MdInline);
    u32 idx = out->len++;
    memset(&out->data[idx], 0, sizeof(MdInline));
    out->data[idx].kind = kind;
    out->data[idx].text = text;
    out->data[idx].url  = url;
    out->data[idx].alt  = alt;
    push_children_from(&out->data[idx], text, arena, depth);
}

static void inline_tokenize(MdSlice slice, InlineVec *out, Arena *arena, int depth) {
    u32 i = 0;
    u32 text_start = 0;

#define FLUSH_TEXT() do { push_text_run(out, slice, text_start, i); text_start = i; } while (0)

    while (i < slice.len) {
        u8 c = slice.data[i];

        /* Backslash escape: literal next codepoint. */
        if (c == '\\' && i + 1 < slice.len) {
            FLUSH_TEXT();
            i++;                              /* skip backslash */
            u32 n = cp_advance(slice.data + i, slice.len - i);
            push_text_run(out, slice, i, i + n);
            i += n;
            text_start = i;
            continue;
        }

        /* Inline code: `...` or ``...`` matching backtick run length. */
        if (c == '`') {
            u32 run = 0;
            while (i + run < slice.len && slice.data[i + run] == '`') run++;
            i64 close = -1;
            for (u32 j = i + run; j + run <= slice.len; ) {
                if (slice.data[j] == '`') {
                    u32 r2 = 0;
                    while (j + r2 < slice.len && slice.data[j + r2] == '`') r2++;
                    if (r2 == run) { close = (i64)j; break; }
                    j += r2;
                } else { j++; }
            }
            if (close >= 0) {
                FLUSH_TEXT();
                MdSlice body = slice_make(slice.data + i + run, (u32)((u32)close - i - run));
                /* CommonMark: strip a single leading + trailing space if both
                 * are present and the content isn't all spaces. */
                if (body.len >= 2 && body.data[0] == ' ' && body.data[body.len-1] == ' ') {
                    bool all_space = true;
                    for (u32 k = 0; k < body.len; k++) {
                        if (body.data[k] != ' ') { all_space = false; break; }
                    }
                    if (!all_space) { body.data++; body.len -= 2; }
                }
                push_styled_run(out, MD_INLINE_CODE, body);
                i = (u32)close + run;
                text_start = i;
                continue;
            }
            /* Unmatched: fall through as literal. */
        }

        /* Embed `![[file]]` and image `![alt](url)`. */
        if (c == '!' && i + 1 < slice.len && slice.data[i + 1] == '[') {
            if (i + 2 < slice.len && slice.data[i + 2] == '[') {
                /* embed */
                u32 j = i + 3;
                while (j + 1 < slice.len && !(slice.data[j] == ']' && slice.data[j+1] == ']')) j++;
                if (j + 1 < slice.len) {
                    FLUSH_TEXT();
                    MdSlice body = slice_make(slice.data + i + 3, j - (i + 3));
                    /* Optional `|alias` */
                    MdSlice url = body, alt = slice_empty();
                    for (u32 k = 0; k < body.len; k++) {
                        if (body.data[k] == '|') {
                            url = slice_make(body.data, k);
                            alt = slice_make(body.data + k + 1, body.len - k - 1);
                            break;
                        }
                    }
                    push_link_run(out, MD_INLINE_EMBED, slice_empty(), url, alt);
                    i = j + 2;
                    text_start = i;
                    continue;
                }
            } else {
                /* image ![alt](url) */
                u32 close_bracket = i + 2;
                while (close_bracket < slice.len && slice.data[close_bracket] != ']') {
                    if (slice.data[close_bracket] == '\\' && close_bracket + 1 < slice.len)
                        close_bracket += 2;
                    else
                        close_bracket++;
                }
                if (close_bracket < slice.len &&
                    close_bracket + 1 < slice.len &&
                    slice.data[close_bracket + 1] == '(') {
                    u32 close_paren = close_bracket + 2;
                    while (close_paren < slice.len && slice.data[close_paren] != ')') close_paren++;
                    if (close_paren < slice.len) {
                        FLUSH_TEXT();
                        MdSlice alt = slice_make(slice.data + i + 2, close_bracket - (i + 2));
                        MdSlice url = slice_make(slice.data + close_bracket + 2,
                                                 close_paren - (close_bracket + 2));
                        push_link_run(out, MD_INLINE_IMAGE, slice_empty(), url, alt);
                        i = close_paren + 1;
                        text_start = i;
                        continue;
                    }
                }
            }
        }

        /* Wikilink `[[Page|alias]]`, footnote `[^id]`, link `[text](url)`. */
        if (c == '[') {
            if (i + 1 < slice.len && slice.data[i + 1] == '[') {
                u32 j = i + 2;
                while (j + 1 < slice.len && !(slice.data[j] == ']' && slice.data[j+1] == ']')) j++;
                if (j + 1 < slice.len) {
                    FLUSH_TEXT();
                    MdSlice body = slice_make(slice.data + i + 2, j - (i + 2));
                    MdSlice url = body, alt = slice_empty();
                    for (u32 k = 0; k < body.len; k++) {
                        if (body.data[k] == '|') {
                            url = slice_make(body.data, k);
                            alt = slice_make(body.data + k + 1, body.len - k - 1);
                            break;
                        }
                    }
                    push_link_run(out, MD_INLINE_WIKILINK, slice_empty(), url, alt);
                    i = j + 2;
                    text_start = i;
                    continue;
                }
            } else if (i + 1 < slice.len && slice.data[i + 1] == '^') {
                /* footnote reference */
                u32 j = i + 2;
                while (j < slice.len && slice.data[j] != ']') j++;
                if (j < slice.len) {
                    FLUSH_TEXT();
                    MdSlice id = slice_make(slice.data + i + 2, j - (i + 2));
                    push_styled_run(out, MD_INLINE_FOOTNOTE_REF, id);
                    i = j + 1;
                    text_start = i;
                    continue;
                }
            } else {
                /* regular link: scan for ](URL) */
                u32 close_bracket = i + 1;
                while (close_bracket < slice.len && slice.data[close_bracket] != ']') {
                    if (slice.data[close_bracket] == '\\' && close_bracket + 1 < slice.len)
                        close_bracket += 2;
                    else
                        close_bracket++;
                }
                if (close_bracket < slice.len &&
                    close_bracket + 1 < slice.len &&
                    slice.data[close_bracket + 1] == '(') {
                    u32 close_paren = close_bracket + 2;
                    while (close_paren < slice.len && slice.data[close_paren] != ')') close_paren++;
                    if (close_paren < slice.len) {
                        FLUSH_TEXT();
                        MdSlice text = slice_make(slice.data + i + 1, close_bracket - (i + 1));
                        MdSlice url  = slice_make(slice.data + close_bracket + 2,
                                                  close_paren - (close_bracket + 2));
                        push_link_rec(out, MD_INLINE_LINK, text, url, slice_empty(), arena, depth);
                        i = close_paren + 1;
                        text_start = i;
                        continue;
                    }
                }
            }
        }

        /* Bare-URL autolink: http(s)://… or <url>. */
        if (c == '<') {
            u32 j = i + 1;
            while (j < slice.len && slice.data[j] != '>' && slice.data[j] != ' ') j++;
            if (j < slice.len && slice.data[j] == '>') {
                /* require URL-ish content */
                MdSlice body = slice_make(slice.data + i + 1, j - (i + 1));
                bool ok = false;
                for (u32 k = 0; k + 2 < body.len; k++) {
                    if (body.data[k] == ':' && body.data[k+1] == '/' && body.data[k+2] == '/') {
                        ok = true; break;
                    }
                    if (!is_url_scheme_char(body.data[k])) { break; }
                }
                if (ok) {
                    FLUSH_TEXT();
                    push_styled_run(out, MD_INLINE_AUTOLINK, body);
                    i = j + 1;
                    text_start = i;
                    continue;
                }
            }
        }
        if ((c == 'h' || c == 'H') && i + 7 < slice.len &&
            (memcmp(slice.data + i, "http://", 7) == 0 ||
             (memcmp(slice.data + i, "https://", 8) == 0 && i + 8 < slice.len))) {
            u32 j = i;
            while (j < slice.len && is_url_char(slice.data[j])) j++;
            /* trim trailing punctuation */
            while (j > i && (slice.data[j-1] == '.' || slice.data[j-1] == ',' ||
                             slice.data[j-1] == ';' || slice.data[j-1] == ':' ||
                             slice.data[j-1] == '!' || slice.data[j-1] == '?')) j--;
            if (j > i + 8) {
                FLUSH_TEXT();
                push_styled_run(out, MD_INLINE_AUTOLINK,
                                slice_make(slice.data + i, j - i));
                i = j;
                text_start = i;
                continue;
            }
        }

        /* Strikethrough ~~…~~. */
        if (c == '~' && i + 1 < slice.len && slice.data[i + 1] == '~') {
            i64 close = find_delim(slice, i + 2, '~', 2);
            if (close >= 0) {
                FLUSH_TEXT();
                MdSlice body = slice_make(slice.data + i + 2, (u32)((u32)close - i - 2));
                push_emph(out, MD_INLINE_STRIKETHROUGH, body, arena, depth);
                i = (u32)close + 2;
                text_start = i;
                continue;
            }
        }

        /* Subscript ~sub~ (single tilde, no spaces) — runs after the ~~ strike
         * test, so "~~strike~~" never lands here. */
        if (c == '~') {
            u32 j = i + 1;
            while (j < slice.len && slice.data[j] != '~' && slice.data[j] != ' ' &&
                   slice.data[j] != '\t' && slice.data[j] != '\n') j++;
            if (j < slice.len && slice.data[j] == '~' && j > i + 1) {
                FLUSH_TEXT();
                push_styled_run(out, MD_INLINE_SUBSCRIPT,
                                slice_make(slice.data + i + 1, j - (i + 1)));
                i = j + 1; text_start = i; continue;
            }
        }

        /* Superscript ^sup^ (no spaces in body). */
        if (c == '^') {
            u32 j = i + 1;
            while (j < slice.len && slice.data[j] != '^' && slice.data[j] != ' ' &&
                   slice.data[j] != '\t' && slice.data[j] != '\n') j++;
            if (j < slice.len && slice.data[j] == '^' && j > i + 1) {
                FLUSH_TEXT();
                push_styled_run(out, MD_INLINE_SUPERSCRIPT,
                                slice_make(slice.data + i + 1, j - (i + 1)));
                i = j + 1; text_start = i; continue;
            }
        }

        /* Highlight ==…==. */
        if (c == '=' && i + 1 < slice.len && slice.data[i + 1] == '=') {
            i64 close = find_delim(slice, i + 2, '=', 2);
            if (close >= 0) {
                FLUSH_TEXT();
                MdSlice body = slice_make(slice.data + i + 2, (u32)((u32)close - i - 2));
                push_emph(out, MD_INLINE_HIGHLIGHT, body, arena, depth);
                i = (u32)close + 2;
                text_start = i;
                continue;
            }
        }

        /* Comment %%…%% — hidden in the rendered view. */
        if (c == '%' && i + 1 < slice.len && slice.data[i + 1] == '%') {
            i64 close = find_delim(slice, i + 2, '%', 2);
            if (close >= 0) {
                FLUSH_TEXT();           /* emit nothing — comment is hidden */
                i = (u32)close + 2;
                text_start = i;
                continue;
            }
        }

        /* Inline math $…$. Opening '$' must not be followed by space/digit (so
         * "$5 and $10" stays text); closing '$' must not be preceded by a space
         * or followed by a digit. \$ is already handled by the escape branch. */
        if (c == '$' && i + 1 < slice.len) {
            u8 nx = slice.data[i + 1];
            if (nx != ' ' && nx != '\t' && nx != '\n' && !(nx >= '0' && nx <= '9')) {
                u32 j = i + 1;
                i64 close = -1;
                while (j < slice.len && slice.data[j] != '\n') {
                    if (slice.data[j] == '\\' && j + 1 < slice.len) { j += 2; continue; }
                    if (slice.data[j] == '$') {
                        u8 prev  = slice.data[j - 1];
                        u8 after = (j + 1 < slice.len) ? slice.data[j + 1] : 0;
                        if (prev != ' ' && prev != '\t' && !(after >= '0' && after <= '9')) {
                            close = (i64)j; break;
                        }
                    }
                    j++;
                }
                if (close > (i64)i + 1) {
                    FLUSH_TEXT();
                    MdSlice body = slice_make(slice.data + i + 1, (u32)((u32)close - i - 1));
                    push_styled_run(out, MD_INLINE_MATH, body);
                    i = (u32)close + 1;
                    text_start = i;
                    continue;
                }
            }
        }

        /* Bold/italic. Triple `***x***` becomes BOLD_ITALIC. */
        if (c == '*' || c == '_') {
            u32 run = 0;
            while (i + run < slice.len && slice.data[i + run] == c) run++;
            /* flanking: opener requires next char to be non-space; not strict here */
            bool can_open = i + run < slice.len && slice.data[i + run] != ' ' &&
                            slice.data[i + run] != '\t';
            if (can_open && run >= 1) {
                u32 want = run >= 3 ? 3 : (run >= 2 ? 2 : 1);
                i64 close = find_delim(slice, i + run, c, want);
                if (close >= 0) {
                    /* A closing run longer than `want` (e.g. the `***` ending
                     * **bold *italic***) keeps its LAST `want` markers as the
                     * closer and gives the rest back to the body, so the inner
                     * emphasis can close when this body is re-tokenized. */
                    u32 crun = 0;
                    while ((u32)close + crun < slice.len &&
                           slice.data[(u32)close + crun] == c) crun++;
                    u32 close_start = (u32)close + (crun > want ? crun - want : 0);
                    /* closer must not be preceded by space */
                    if (close_start > i + run && slice.data[close_start - 1] != ' ') {
                        FLUSH_TEXT();
                        u8 kind = want == 3 ? MD_INLINE_BOLD_ITALIC
                                : want == 2 ? MD_INLINE_BOLD
                                            : MD_INLINE_ITALIC;
                        MdSlice body = slice_make(slice.data + i + want,
                                                  close_start - (i + want));
                        push_emph(out, kind, body, arena, depth);
                        i = close_start + want;
                        text_start = i;
                        continue;
                    }
                }
            }
            /* fall through: literal char */
        }

        /* Tag #word — only if preceded by whitespace/punct (not text), and
         * followed by a tag-start codepoint. */
        if (c == '#') {
            bool boundary = (i == 0);
            if (!boundary) {
                u8 prev = slice.data[i - 1];
                boundary = (prev == ' ' || prev == '\t' || prev == '(' || prev == '[' ||
                            prev == '{' || prev == ',' || prev == ';');
            }
            if (boundary && i + 1 < slice.len && is_tag_start(slice.data[i + 1])) {
                u32 j = i + 1;
                while (j < slice.len && is_tag_cont(slice.data[j])) j++;
                /* Purely-numeric tags (#123) are rejected so dates / "#1"
                 * stay literal text. Require at least one non-digit. */
                bool numeric_only = true;
                for (u32 k = i + 1; k < j; k++) {
                    u8 ch = slice.data[k];
                    if (ch < '0' || ch > '9') { numeric_only = false; break; }
                }
                if (j > i + 1 && !numeric_only) {
                    FLUSH_TEXT();
                    push_styled_run(out, MD_INLINE_TAG,
                                    slice_make(slice.data + i + 1, j - (i + 1)));
                    i = j;
                    text_start = i;
                    continue;
                }
            }
        }

        /* Default: advance one codepoint. */
        i += cp_advance(slice.data + i, slice.len - i);
    }
    FLUSH_TEXT();
#undef FLUSH_TEXT
}

/* =========================================================================
 * Block-level parser
 * ========================================================================= */

/* Cap on callout/quote body recursion so a pathological deeply-nested input
 * can't blow the stack (parse + render both recurse one level per nest). */
#define MD_MAX_NEST 4

typedef struct {
    BlockVec blocks;
    Arena   *arena;
    int      depth;     /* callout/quote nesting depth (0 at top level) */
} BlockBuilder;

static MdBlock *bb_emit(BlockBuilder *bb) {
    if (!VEC_RESERVE(&bb->blocks, MdBlock)) return NULL;
    MdBlock *b = &bb->blocks.data[bb->blocks.len++];
    memset(b, 0, sizeof *b);
    return b;
}

/* Single-paragraph extension: append `line` to existing paragraph slice
 * (which is contiguous in source). */
static void paragraph_extend(MdBlock *p, MdSlice line) {
    if (!p->raw.data) {
        p->raw = line;
        return;
    }
    /* slice must be physically adjacent (we walked source linearly) */
    p->raw.len = (u32)((line.data + line.len) - p->raw.data);
}

/* Multi-line collector for blockquote lines (already stripped of leading `>` ). */
typedef struct {
    const u8 *first;
    const u8 *last_end;
} BodyAccum;

static void body_extend(BodyAccum *b, MdSlice s) {
    if (!b->first) b->first = s.data;
    b->last_end = s.data + s.len;
}

static MdSlice body_slice(const BodyAccum *b) {
    return slice_make(b->first, b->first ? (u32)(b->last_end - b->first) : 0);
}

/* Collect the body lines of a callout/quote into a fresh arena buffer with the
 * leading '>' (+ one space) stripped from each line and the lines re-joined by
 * '\n'. This yields a clean markdown fragment (no embedded continuation-line
 * '>' markers) that can be recursively re-parsed into child blocks and rendered
 * without stray quote markers. Stops at the first non-'>' line. */
static MdSlice scanner_collect_quote_clean(LineScanner *s, Arena *arena) {
    u8 *buf = NULL; usize blen = 0, bcap = 0;
    bool first = true;
    while (s->pos < s->len) {
        usize save = s->pos;
        MdSlice line;
        if (!scan_line(s, &line)) break;
        u32 ind = leading_spaces(line);
        if (ind >= line.len || line.data[ind] != '>') { s->pos = save; break; }
        u32 q = ind + 1;
        if (q < line.len && (line.data[q] == ' ' || line.data[q] == '\t')) q++;
        u32 clen = line.len - q;
        usize need = blen + (first ? 0 : 1) + clen;
        if (need > bcap) {
            bcap = need * 2 + 64;
            u8 *nb = realloc(buf, bcap);
            if (!nb) { free(buf); return slice_make(NULL, 0); }
            buf = nb;
        }
        if (!first) buf[blen++] = '\n';
        if (clen) { memcpy(buf + blen, line.data + q, clen); blen += clen; }
        first = false;
    }
    if (blen == 0) { free(buf); return slice_make(NULL, 0); }
    u8 *out = arena_alloc(arena, blen);
    if (out) memcpy(out, buf, blen);
    free(buf);
    return out ? slice_make(out, (u32)blen) : slice_make(NULL, 0);
}

/* Strip ALL leading '>'(+ optional space/tab) runs from each line of `body`.
 * Used only when the nesting cap is reached: the deepest retained body is
 * rendered as flat text, and without this its lines would show literal '>'
 * markers from the levels that were never recursed. Returns an arena copy;
 * falls back to `body` unchanged on arena exhaustion. */
static MdSlice strip_quote_prefixes(Arena *arena, MdSlice body) {
    if (body.len == 0) return body;
    u8 *out = arena_alloc(arena, body.len);
    if (!out) return body;
    u32 w = 0, i = 0;
    bool at_line_start = true;
    while (i < body.len) {
        if (at_line_start) {
            while (i < body.len && body.data[i] == '>') {
                i++;
                if (i < body.len && (body.data[i] == ' ' || body.data[i] == '\t')) i++;
            }
            at_line_start = false;
            if (i >= body.len) break;
        }
        u8 c = body.data[i++];
        out[w++] = c;
        if (c == '\n') at_line_start = true;
    }
    return slice_make(out, w);
}

/* Build inline runs into the parse arena and attach to block. */
static void finalize_inline_run(Arena *arena, MdBlock *b, MdSlice src) {
    if (src.len == 0) {
        b->inlines = NULL;
        b->inline_count = 0;
        return;
    }
    InlineVec v = {0};
    inline_tokenize(src, &v, arena, 0);
    if (v.len == 0) {
        b->inlines = NULL;
        b->inline_count = 0;
        free(v.data);
        return;
    }
    MdInline *out = arena_alloc(arena, (usize)v.len * sizeof(MdInline));
    if (!out) {
        free(v.data);
        b->inlines = NULL;
        b->inline_count = 0;
        return;
    }
    memcpy(out, v.data, (usize)v.len * sizeof(MdInline));
    b->inlines = out;
    b->inline_count = v.len;
    free(v.data);
}

/* Parse a markdown body slice into a separate sub-vec of blocks. Used for
 * callout/quote children so nested headings, lists, and paragraphs render
 * properly inside callouts. */
static void block_parse_into(BlockBuilder *bb, const u8 *src, usize len);

static MdBlock *finalize_children(Arena *arena, MdBlock *parent, BlockVec *vec) {
    if (vec->len == 0) {
        parent->children = NULL;
        parent->child_count = 0;
        free(vec->data);
        return NULL;
    }
    MdBlock *out = arena_alloc(arena, (usize)vec->len * sizeof(MdBlock));
    if (!out) {
        free(vec->data);
        parent->children = NULL;
        parent->child_count = 0;
        return NULL;
    }
    memcpy(out, vec->data, (usize)vec->len * sizeof(MdBlock));
    parent->children = out;
    parent->child_count = vec->len;
    free(vec->data);
    return out;
}

/* Recursively inline-tokenize a block array (and any callout/quote children).
 * A callout/quote that parsed into child blocks keeps its content in those
 * children, so its own raw slice is not tokenized — the children render it. */
static void finalize_block_tree(Arena *arena, MdBlock *blocks, u32 count) {
    for (u32 i = 0; i < count; i++) {
        MdBlock *b = &blocks[i];
        switch (b->kind) {
        case MD_BLOCK_PARAGRAPH:
        case MD_BLOCK_HEADING:
        case MD_BLOCK_LIST_ITEM:
        case MD_BLOCK_TASK_ITEM:
        case MD_BLOCK_DEF_TERM:
        case MD_BLOCK_DEF_DESC:
            finalize_inline_run(arena, b, slice_trim(b->raw));
            break;
        case MD_BLOCK_QUOTE:
        case MD_BLOCK_CALLOUT:
            if (b->child_count == 0)
                finalize_inline_run(arena, b, slice_trim(b->raw));
            break;
        default:
            break;
        }
        if (b->child_count)
            finalize_block_tree(arena, b->children, b->child_count);
    }
}

/* Table parsing. Header line + separator + rows (each '|'-bounded). */
static bool parse_table_at(BlockBuilder *bb, LineScanner *s, MdSlice header) {
    /* peek next line to confirm separator */
    usize save = s->pos;
    MdSlice sep_line;
    if (!scan_line(s, &sep_line)) { s->pos = save; return false; }
    char aligns[32] = {0};
    u16 cols = 0;
    if (!is_table_separator(sep_line, &cols, aligns)) { s->pos = save; return false; }

    /* Tokenise a row into cell slices. */
    typedef struct { MdSlice s[32]; u16 n; } Row;
    /* static, not a ~130 KB on-stack frame reserved on every table-classification
     * call (incl. the common early-bail): the markdown parser is single-threaded
     * and non-reentrant, and only rows[0..row_count) is ever read, so reusing one
     * BSS buffer is safe and keeps the full 256-row capacity (no table truncation). */
    static Row rows[256]; u16 row_count = 0;

    /* Helper inline-lambda style — collect cells from a single row. */
    #define COLLECT_ROW(r, line) do {                                                  \
        u32 i = 0, end = (line).len;                                                   \
        while (i < end && ((line).data[i] == ' ' || (line).data[i] == '\t')) i++;      \
        if (i < end && (line).data[i] == '|') i++;                                     \
        while (end > i && ((line).data[end-1] == ' ' || (line).data[end-1] == '\t' ||  \
                            (line).data[end-1] == '\r')) end--;                         \
        if (end > i && (line).data[end-1] == '|') end--;                               \
        (r)->n = 0;                                                                     \
        while (i < end && (r)->n < 32) {                                                \
            u32 cs = i;                                                                 \
            while (i < end && (line).data[i] != '|') {                                  \
                if ((line).data[i] == '\\' && i + 1 < end) i++;                         \
                i++;                                                                     \
            }                                                                            \
            u32 ce = i;                                                                 \
            while (cs < ce && ((line).data[cs] == ' ' || (line).data[cs] == '\t')) cs++; \
            while (ce > cs && ((line).data[ce-1] == ' ' || (line).data[ce-1] == '\t')) ce--; \
            (r)->s[(r)->n++] = slice_make((line).data + cs, ce - cs);                   \
            if (i < end && (line).data[i] == '|') i++;                                  \
        }                                                                                \
    } while (0)

    Row hr; COLLECT_ROW(&hr, header);
    rows[row_count++] = hr;

    while (s->pos < s->len && row_count < 256) {
        usize rsave = s->pos;
        MdSlice line;
        if (!scan_line(s, &line)) break;
        if (line.len == 0 || !looks_like_table_row(line)) { s->pos = rsave; break; }
        Row r; COLLECT_ROW(&r, line);
        if (r.n == 0) { s->pos = rsave; break; }
        rows[row_count++] = r;
    }
    #undef COLLECT_ROW

    MdBlock *b = bb_emit(bb);
    if (!b) return false;
    b->kind = MD_BLOCK_TABLE;
    b->cols = cols;
    b->rows = row_count;

    /* alignment encoded as info bytes (one per column) */
    char *align_buf = arena_alloc(bb->arena, (usize)cols);
    if (align_buf) {
        memcpy(align_buf, aligns, cols);
        b->info = slice_make((const u8 *)align_buf, cols);
    }

    /* allocate cell grid; tokenize each cell as inline run */
    MdTableCell *cells = arena_alloc(bb->arena, (usize)row_count * (usize)cols * sizeof(MdTableCell));
    if (!cells) return true;
    memset(cells, 0, (usize)row_count * (usize)cols * sizeof(MdTableCell));
    b->cells = cells;
    for (u16 r = 0; r < row_count; r++) {
        for (u16 col = 0; col < cols; col++) {
            MdSlice cs = (col < rows[r].n) ? rows[r].s[col] : slice_empty();
            InlineVec v = {0};
            inline_tokenize(cs, &v, bb->arena, 0);
            MdTableCell *cell = &cells[(usize)r * cols + col];
            if (v.len) {
                MdInline *out = arena_alloc(bb->arena, (usize)v.len * sizeof(MdInline));
                if (out) {
                    memcpy(out, v.data, (usize)v.len * sizeof(MdInline));
                    cell->inlines = out;
                    cell->inline_count = v.len;
                }
            }
            free(v.data);
        }
    }
    return true;
}

/* Main classifier — consumes one or more lines per call. */
static void block_parse_loop(BlockBuilder *bb, LineScanner *s) {
    /* Frontmatter at offset 0 — only at document root. Child parses (callout/
     * quote bodies) run their own LineScanner from pos 0 too, so without the
     * depth gate a quoted `---\nkey: val\n---` would be mis-parsed as YAML. */
    if (bb->depth == 0 && s->pos == 0 && s->len >= 4 &&
        s->src[0] == '-' && s->src[1] == '-' && s->src[2] == '-' &&
        (s->src[3] == '\n' || (s->src[3] == '\r' && s->len >= 5 && s->src[4] == '\n'))) {
        usize save = s->pos;
        MdSlice first;
        scan_line(s, &first);
        const u8 *body_start = s->src + s->pos;
        const u8 *body_end = body_start;
        bool found_close = false;
        while (s->pos < s->len) {
            MdSlice line;
            usize prepos = s->pos;
            if (!scan_line(s, &line)) break;
            if (line.len == 3 && line.data[0] == '-' && line.data[1] == '-' && line.data[2] == '-') {
                body_end = s->src + prepos;
                /* trim final newline preceding the close marker */
                if (body_end > body_start && body_end[-1] == '\n') body_end--;
                if (body_end > body_start && body_end[-1] == '\r') body_end--;
                found_close = true;
                break;
            }
        }
        if (found_close) {
            MdBlock *b = bb_emit(bb);
            if (b) {
                b->kind = MD_BLOCK_FRONTMATTER;
                b->raw = slice_make(body_start, (u32)(body_end - body_start));
            }
        } else {
            /* not closed: rewind and treat as HR + paragraph */
            s->pos = save;
            (void)first;
        }
    }

    bool in_fence = false;
    u8   fence_marker = 0;
    u32  fence_run = 0;
    u32  fence_indent = 0;
    BodyAccum fence_acc = {0};
    MdSlice fence_lang = slice_empty();

    MdBlock *open_paragraph = NULL;

    while (s->pos < s->len) {
        usize before = s->pos;
        MdSlice line;
        if (!scan_line(s, &line)) break;

        /* fence body collection */
        if (in_fence) {
            u32 ind = leading_spaces(line);
            if (ind <= fence_indent + 3 && ind < line.len &&
                line.data[ind] == fence_marker) {
                u32 r = 0;
                while (ind + r < line.len && line.data[ind + r] == fence_marker) r++;
                if (r >= fence_run) {
                    /* close */
                    MdBlock *b = bb_emit(bb);
                    if (b) {
                        b->kind = MD_BLOCK_CODE;
                        b->info = fence_lang;
                        b->raw  = body_slice(&fence_acc);
                    }
                    in_fence = false;
                    fence_acc = (BodyAccum){0};
                    fence_lang = slice_empty();
                    open_paragraph = NULL;
                    continue;
                }
            }
            /* keep raw (no leading-indent trim, preserves code formatting) */
            body_extend(&fence_acc, slice_make(line.data,
                                               (u32)((s->src + (s->pos - 1)) - line.data)));
            /* That actually grabs trailing newline too; simpler: include the
             * full original line + newline by using our before/after positions. */
            (void)before;
            continue;
        }

        u32 ind = leading_spaces(line);
        MdSlice body = slice_make(line.data + ind, line.len - ind);

        /* Blank line — closes paragraph */
        if (body.len == 0) {
            open_paragraph = NULL;
            continue;
        }

        /* Fenced code open */
        if (body.len >= 3 &&
            (body.data[0] == '`' || body.data[0] == '~') &&
            body.data[1] == body.data[0] && body.data[2] == body.data[0]) {
            u32 run = 0;
            while (run < body.len && body.data[run] == body.data[0]) run++;
            in_fence = true;
            fence_marker = body.data[0];
            fence_run = run;
            fence_indent = ind;
            /* lang = trimmed remainder of the line */
            MdSlice lang = slice_make(body.data + run, body.len - run);
            fence_lang = slice_trim(lang);
            fence_acc = (BodyAccum){0};
            open_paragraph = NULL;
            continue;
        }

        /* Display math $$…$$ (raw TeX in `raw`). */
        if (body.len >= 2 && body.data[0] == '$' && body.data[1] == '$') {
            /* Single line "$$ … $$". */
            if (body.len >= 4 && body.data[body.len - 1] == '$' && body.data[body.len - 2] == '$') {
                MdSlice tex = slice_trim(slice_make(body.data + 2, body.len - 4));
                MdBlock *b = bb_emit(bb);
                if (b) { b->kind = MD_BLOCK_MATH; b->raw = tex; }
                open_paragraph = NULL;
                continue;
            }
            /* Multi-line: TeX runs from just after the opening $$ to the line
             * before the closing $$. */
            const u8 *mstart = body.data + 2;
            const u8 *mend = mstart;
            bool closed = false;
            usize msave = s->pos;          /* rewind point if never closed */
            while (s->pos < s->len) {
                usize prepos = s->pos;
                MdSlice ml;
                if (!scan_line(s, &ml)) break;
                u32 mind = leading_spaces(ml);
                if (ml.len - mind >= 2 && ml.data[mind] == '$' && ml.data[mind + 1] == '$') {
                    mend = s->src + prepos;
                    if (mend > mstart && mend[-1] == '\n') mend--;
                    if (mend > mstart && mend[-1] == '\r') mend--;
                    closed = true;
                    break;
                }
            }
            if (closed) {
                MdBlock *b = bb_emit(bb);
                if (b) {
                    b->kind = MD_BLOCK_MATH;
                    b->raw = slice_trim(slice_make(mstart, (u32)(mend - mstart)));
                }
                open_paragraph = NULL;
                continue;
            }
            /* Unterminated: rewind so the lines after the bare "$$" are parsed
             * normally (otherwise the scan consumed to EOF and every following
             * block would silently vanish), and treat this line as a paragraph. */
            s->pos = msave;
            /* fall through */
        }

        /* Callout: '> [!type]' */
        if (body.len >= 4 && body.data[0] == '>' &&
            (body.len >= 4) && (body.data[1] == ' ' ? body.data[2] == '[' && body.data[3] == '!'
                                                    : body.data[1] == '[' && body.data[2] == '!')) {
            /* normalize: skip '>' and optional space */
            u32 q = 1;
            if (q < body.len && body.data[q] == ' ') q++;
            /* expect '[!type]' */
            if (q + 2 < body.len && body.data[q] == '[' && body.data[q + 1] == '!') {
                u32 close = q + 2;
                while (close < body.len && body.data[close] != ']') close++;
                if (close < body.len) {
                    MdSlice tag = slice_make(body.data + q + 2, close - (q + 2));
                    /* title is rest of the line (may be empty) */
                    u32 ts = close + 1;
                    while (ts < body.len && (body.data[ts] == ' ' || body.data[ts] == '+' ||
                                             body.data[ts] == '-')) ts++;
                    MdSlice title = slice_make(body.data + ts, body.len - ts);
                    /* collect remaining quote lines into a clean fragment */
                    MdSlice body_clean = scanner_collect_quote_clean(s, bb->arena);

                    MdBlock *b = bb_emit(bb);
                    if (b) {
                        b->kind = MD_BLOCK_CALLOUT;
                        b->callout_kind = (u8)callout_lookup(tag);
                        b->info = title;
                        b->raw  = body_clean;
                        /* Recursively parse the body into child blocks so nested
                         * lists / paragraphs / callouts render structurally. */
                        if (body_clean.len && bb->depth < MD_MAX_NEST) {
                            BlockBuilder cbb = { .arena = bb->arena,
                                                 .depth = bb->depth + 1 };
                            block_parse_into(&cbb, body_clean.data, body_clean.len);
                            finalize_children(bb->arena, b, &cbb.blocks);
                        } else if (body_clean.len) {
                            b->raw = strip_quote_prefixes(bb->arena, body_clean);
                        }
                    }
                    open_paragraph = NULL;
                    continue;
                }
            }
            /* Fall through to plain blockquote handling. */
        }

        /* Blockquote: '> ...' */
        if (body.data[0] == '>') {
            /* Rewind: re-include this line, then collect all consecutive '>' lines */
            s->pos = before;
            MdSlice body_clean = scanner_collect_quote_clean(s, bb->arena);
            MdBlock *b = bb_emit(bb);
            if (b) {
                b->kind = MD_BLOCK_QUOTE;
                b->raw  = body_clean;
                if (body_clean.len && bb->depth < MD_MAX_NEST) {
                    BlockBuilder cbb = { .arena = bb->arena,
                                         .depth = bb->depth + 1 };
                    block_parse_into(&cbb, body_clean.data, body_clean.len);
                    finalize_children(bb->arena, b, &cbb.blocks);
                } else if (body_clean.len) {
                    b->raw = strip_quote_prefixes(bb->arena, body_clean);
                }
            }
            open_paragraph = NULL;
            continue;
        }

        /* Horizontal rule */
        if (is_hr(body)) {
            MdBlock *b = bb_emit(bb);
            if (b) b->kind = MD_BLOCK_HR;
            open_paragraph = NULL;
            continue;
        }

        /* ATX heading */
        u8 h_lvl = 0;
        u32 text_start = 0;
        if (is_atx_heading(body, &h_lvl, &text_start)) {
            MdSlice text = slice_make(body.data + text_start, body.len - text_start);
            /* Trim trailing '#' run + spaces (closing-style ATX) */
            text = slice_trim(text);
            while (text.len && text.data[text.len-1] == '#') text.len--;
            text = slice_trim(text);
            MdBlock *b = bb_emit(bb);
            if (b) {
                b->kind = MD_BLOCK_HEADING;
                b->heading_level = h_lvl;
                b->raw = text;     /* will inline-tokenize later */
            }
            open_paragraph = NULL;
            continue;
        }

        /* Task list: "- [ ] " / "- [x] " */
        if (body.len >= 6 &&
            (body.data[0] == '-' || body.data[0] == '*' || body.data[0] == '+') &&
            body.data[1] == ' ' && body.data[2] == '[' &&
            body.data[4] == ']' && body.data[5] == ' ') {
            MdBlock *b = bb_emit(bb);
            if (b) {
                b->kind = MD_BLOCK_TASK_ITEM;
                b->list_depth = (u8)(ind / 2);
                /* Store the marker char so custom states (x, /, -, …) render
                 * distinctly; 0 = unchecked. */
                b->task_state = (body.data[3] == ' ') ? 0 : (u8)body.data[3];
                /* Byte offset of the state char in the scanner source. For the
                 * top-level parse (src == the md_parse input == fb->view_content)
                 * this maps straight to the editable buffer; the click handler
                 * validates the bytes before toggling, so a recursive/arena-copy
                 * source (callout/quote body) can never corrupt the file. */
                b->src_off = (u32)((body.data + 3) - s->src);
                b->raw = slice_make(body.data + 6, body.len - 6);
            }
            open_paragraph = NULL;
            continue;
        }

        /* List item: unordered or ordered. */
        if ((body.data[0] == '-' || body.data[0] == '*' || body.data[0] == '+') &&
            body.len >= 2 && body.data[1] == ' ') {
            MdBlock *b = bb_emit(bb);
            if (b) {
                b->kind = MD_BLOCK_LIST_ITEM;
                b->list_ordered = 0;
                b->list_depth = (u8)(ind / 2);
                b->raw = slice_make(body.data + 2, body.len - 2);
            }
            open_paragraph = NULL;
            continue;
        }
        if (body.data[0] >= '0' && body.data[0] <= '9') {
            u32 d = 0;
            while (d < body.len && body.data[d] >= '0' && body.data[d] <= '9') d++;
            if (d > 0 && d + 1 < body.len && body.data[d] == '.' && body.data[d+1] == ' ') {
                u32 num = 0;
                for (u32 k = 0; k < d && k < 6; k++)
                    num = num * 10 + (u32)(body.data[k] - '0');
                MdBlock *b = bb_emit(bb);
                if (b) {
                    b->kind = MD_BLOCK_LIST_ITEM;
                    b->list_ordered = 1;
                    b->list_index = (u16)num;
                    b->list_depth = (u8)(ind / 2);
                    b->raw = slice_make(body.data + d + 2, body.len - d - 2);
                }
                open_paragraph = NULL;
                continue;
            }
        }

        /* Table: header line with '|' followed by separator row */
        if (looks_like_table_row(body)) {
            usize save = s->pos;
            if (parse_table_at(bb, s, body)) {
                open_paragraph = NULL;
                continue;
            }
            s->pos = save;
        }

        /* Definition list: a "Term" line followed by ": definition" line(s).
         * The leading ": " (colon + space) is the trigger; the preceding open
         * paragraph becomes the term. Lone ": x" (no term) still renders as an
         * indented description. */
        if (body.len >= 2 && body.data[0] == ':' && body.data[1] == ' ') {
            if (open_paragraph && open_paragraph->kind == MD_BLOCK_PARAGRAPH)
                open_paragraph->kind = MD_BLOCK_DEF_TERM;
            MdBlock *d = bb_emit(bb);
            if (d) {
                d->kind = MD_BLOCK_DEF_DESC;
                d->raw  = slice_make(body.data + 2, body.len - 2);
            }
            open_paragraph = NULL;
            continue;
        }

        /* Setext: paragraph followed by '====' or '----' line */
        if (open_paragraph && body.len > 0) {
            u8 m = body.data[0];
            if (m == '=' || m == '-') {
                bool all = true;
                for (u32 k = 0; k < body.len; k++) {
                    if (body.data[k] != m && body.data[k] != ' ') { all = false; break; }
                }
                if (all) {
                    open_paragraph->kind = MD_BLOCK_HEADING;
                    open_paragraph->heading_level = (m == '=') ? 1 : 2;
                    open_paragraph = NULL;
                    continue;
                }
            }
        }

        /* Paragraph */
        if (open_paragraph) {
            paragraph_extend(open_paragraph, line);
        } else {
            MdBlock *b = bb_emit(bb);
            if (b) {
                b->kind = MD_BLOCK_PARAGRAPH;
                b->raw  = line;
            }
            open_paragraph = b;
        }
    }

    /* unterminated fence: emit what we have */
    if (in_fence) {
        MdBlock *b = bb_emit(bb);
        if (b) {
            b->kind = MD_BLOCK_CODE;
            b->info = fence_lang;
            b->raw  = body_slice(&fence_acc);
        }
    }
}

static void block_parse_into(BlockBuilder *bb, const u8 *src, usize len) {
    LineScanner s = { src, len, 0 };
    block_parse_loop(bb, &s);
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

/* ---- footnote numbering pass ----
 * Assign each distinct [^id] a 1-based number (first-seen order) and stash it
 * in MdInline.flags, so the renderer can draw refs as superscript numbers and
 * the matching definition (a paragraph beginning with the same ref) gets the
 * same number. */
typedef struct { MdSlice ids[255]; u32 count; } FnTable;

static bool fn_slice_eq(MdSlice a, MdSlice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}
static u8 fn_number(FnTable *t, MdSlice id) {
    for (u32 i = 0; i < t->count; i++)
        if (fn_slice_eq(t->ids[i], id)) return (u8)(i + 1);
    if (t->count < 255) { t->ids[t->count++] = id; return (u8)t->count; }
    return 255;
}
static void fn_walk_inlines(FnTable *t, MdInline *runs, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (runs[i].kind == MD_INLINE_FOOTNOTE_REF)
            runs[i].flags = fn_number(t, runs[i].text);
        if (runs[i].children && runs[i].child_count)
            fn_walk_inlines(t, runs[i].children, runs[i].child_count);
    }
}
static void fn_walk_blocks(FnTable *t, MdBlock *blocks, u32 count) {
    for (u32 i = 0; i < count; i++) {
        MdBlock *b = &blocks[i];
        if (b->inlines && b->inline_count) fn_walk_inlines(t, b->inlines, b->inline_count);
        if (b->cells) {
            u32 nc = (u32)b->rows * (u32)b->cols;
            for (u32 c = 0; c < nc; c++)
                if (b->cells[c].inlines && b->cells[c].inline_count)
                    fn_walk_inlines(t, b->cells[c].inlines, b->cells[c].inline_count);
        }
        if (b->children && b->child_count) fn_walk_blocks(t, b->children, b->child_count);
    }
}

MdDoc *md_parse(Arena *arena, const u8 *source, usize len, const char *base_dir) {
    if (!arena || !arena->base) return NULL;

    MdDoc *doc = arena_alloc(arena, sizeof *doc);
    if (!doc) return NULL;
    memset(doc, 0, sizeof *doc);
    doc->arena = arena;
    doc->base_dir = base_dir;

    if (!source || len == 0) return doc;

    BlockBuilder bb = { .arena = arena };
    block_parse_into(&bb, source, len);

    /* Inline-tokenize every block's raw slice, recursing into callout/quote
     * children. Skip CODE bodies, HTML, FRONTMATTER, and table cells (table
     * cells were tokenized during parse_table_at). A callout/quote that parsed
     * into child blocks carries its content there, so its own raw is left
     * un-tokenized (the children render the body). */
    finalize_block_tree(arena, bb.blocks.data, bb.blocks.len);

    /* Stash frontmatter convenience handle. */
    if (bb.blocks.len && bb.blocks.data[0].kind == MD_BLOCK_FRONTMATTER) {
        doc->frontmatter_yaml = bb.blocks.data[0].raw;
    }

    /* Copy block array into arena. */
    if (bb.blocks.len) {
        MdBlock *out = arena_alloc(arena, (usize)bb.blocks.len * sizeof(MdBlock));
        if (out) {
            memcpy(out, bb.blocks.data, (usize)bb.blocks.len * sizeof(MdBlock));
            doc->blocks = out;
            doc->block_count = bb.blocks.len;
            /* Number footnotes across the whole tree (refs + their defs). */
            FnTable fn = {0};
            fn_walk_blocks(&fn, doc->blocks, doc->block_count);
        }
    }
    free(bb.blocks.data);
    return doc;
}

u32 md_block_count(const MdDoc *doc) {
    return doc ? doc->block_count : 0u;
}
