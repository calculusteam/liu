/*
 * Liu - Markdown parser implementation.
 *
 * Four stages, in this order — the ordering is load-bearing, because each
 * later stage needs the whole document the earlier one produced:
 *
 *   1. block_parse_loop — walks source line-by-line into a heap vector of
 *      MdBlock, each carrying the raw slice of its inline text. Multi-line
 *      blocks (fenced code, indented code, callouts, blockquotes, tables,
 *      lists, HTML, comments) consume several input lines. Tables also build
 *      a per-cell inline grid. Link reference definitions are recorded into
 *      a document-scope table here and emit no block at all.
 *
 *   2. finalize_block_tree — strips trailing "^block-id"s, then inline-
 *      tokenizes every block's raw slice (recursing into callout/quote
 *      children) into MdInline runs.
 *
 *   3. RESOLUTION — the passes that need the finished document:
 *        linkref_resolve_*  "[text][label]" → its destination, or plain text
 *        fn_walk_* / fn_reflow  footnote numbering + back matter
 *        fm_extract         frontmatter title/tags/aliases
 *      A reference may point forward, so none of this can happen inline.
 *
 *   4. finalize — copies the heap vectors into the parse arena, leaving the
 *      caller with a doc whose pointers all live in `arena`.
 *
 * Source bytes outlive the doc (filebrowser keeps fb->view_content alive
 * until viewer close), so AST slices point directly into source — no
 * per-string copies. The one exception is indented-code dedent, which must
 * remove bytes from the middle of a span and so builds its result in the
 * arena instead.
 */
#include "ui/markdown/md_parse.h"
#include "core/utf8.h"
#include "core/checked.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Each parse owns one cumulative ledger, shared by every nested block and
 * inline fragment. Suffix lookaheads may otherwise revisit the same bytes for
 * every unmatched opener (links, embeds, code, HTML and emphasis alike).
 * The linear allowance bounds all of them together, including resolution.
 * Thread-local state keeps concurrent graph/view/embed jobs independent;
 * md_parse_ex saves/restores the pointer so a nested call also stays safe. */
#define MD_WORK_PER_BYTE 64u
#define MD_WORK_FIXED KB(64)
#define MD_CANCEL_INTERVAL 1024u

typedef struct {
    usize remaining;
    u32 until_cancel;
    bool limited;
    const _Atomic bool *cancel;
} MdParseWork;

static _Thread_local MdParseWork *g_md_work;

static bool md_work_step(void) {
    MdParseWork *w = g_md_work;
    if (!w) return true;
    if (w->limited) return false;
    if (w->remaining == 0 ||
        (w->until_cancel == 0 && w->cancel &&
         atomic_load_explicit(w->cancel, memory_order_relaxed))) {
        w->limited = true;
        return false;
    }
    w->remaining--;
    if (w->until_cancel == 0) w->until_cancel = MD_CANCEL_INTERVAL;
    w->until_cancel--;
    return true;
}

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

#define MD_MAX_VEC_ITEMS (1024u * 1024u)

static bool md_work_refuse(void) {
    if (g_md_work) g_md_work->limited = true;
    return false;
}

static bool vec_reserve(void **data, u32 len, u32 *cap, usize elem_size) {
    if (!data || !cap || elem_size == 0 || len > *cap ||
        len >= MD_MAX_VEC_ITEMS) return md_work_refuse();
    if (len < *cap) return true;
    u32 next;
    if (*cap == 0) next = 8;
    else {
        if (*cap > MD_MAX_VEC_ITEMS / 2u) next = MD_MAX_VEC_ITEMS;
        else
        next = *cap * 2u;
    }
    if (next <= len || (usize)next > SIZE_MAX / elem_size) return md_work_refuse();
    usize bytes = (usize)next * elem_size;
    void *grown = realloc(*data, bytes);
    if (!grown) return md_work_refuse();
    *data = grown;
    *cap = next;
    return true;
}

/* Returns true on success, false if realloc failed (caller must abort the
 * push). Designed for non-void callers that need to bail with their own
 * return type — bb_emit returns NULL, inline pushers return silently. */
#define VEC_RESERVE(vec, type) \
    vec_reserve((void **)&(vec)->data, (vec)->len, &(vec)->cap, sizeof(type))

#define VEC_PUSH(vec, type)                                                       \
    do { if (!VEC_RESERVE((vec), type)) return; } while (0)

/* =========================================================================
 * Slice helpers
 * ========================================================================= */

static MdSlice slice_make(const u8 *s, u32 n) { return (MdSlice){ s, n }; }
static MdSlice slice_empty(void)              { return (MdSlice){ NULL, 0 }; }

/* Trim leading + trailing ASCII whitespace from a slice. */
static MdSlice slice_trim(MdSlice s) {
    while ((s.len && (s.data[0] == ' ' || s.data[0] == '\t')) && md_work_step()) { s.data++; s.len--; }
    while ((s.len && (s.data[s.len-1] == ' ' || s.data[s.len-1] == '\t' ||
                     s.data[s.len-1] == '\r')) && md_work_step()) { s.len--; }
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
    for (usize i = 0;( i < n) && md_work_step(); i++) {
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
    if (s->pos >= s->len || !md_work_step()) return false;
    usize start = s->pos;
    while ((s->pos < s->len && s->src[s->pos] != '\n') && md_work_step()) s->pos++;
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
    while ((i < line.len && line.data[i] == ' ') && md_work_step()) i++;
    return i;
}

/* Leading whitespace measured in COLUMNS, with a tab advancing to the next
 * multiple of 4 (CommonMark's tab stop). `*out_bytes` receives the byte count,
 * which is what you index the line with — the two differ the moment a tab is
 * involved.
 *
 * Indentation is load-bearing here: it decides list nesting AND the 4-column
 * indented-code threshold. Counting a tab as one column would nest
 * tab-indented lists at depth 0 and hide tab-indented code blocks entirely. */
static u32 leading_indent(MdSlice line, u32 *out_bytes) {
    u32 i = 0, col = 0;
    while ((i < line.len) && md_work_step()) {
        if (line.data[i] == ' ')       col += 1;
        else if (line.data[i] == '\t') col += 4 - (col % 4);
        else break;
        i++;
    }
    if (out_bytes) *out_bytes = i;
    return col;
}

/* Column at which the enclosing block's content starts — used to decide
 * whether a following line continues that block or dedents out of it. */
#define MD_INDENT_CODE   4u    /* columns past the container = indented code */
#define MD_MAX_LIST_DEPTH 16u

/* One currently-open list item, innermost last.
 *
 * CommonMark nests by CONTENT COLUMN, not by a fixed indent step: an item
 * belongs to the innermost open item whose content column its marker reaches. A
 * 4-space-indented sub-item under a parent whose text begins at column 2 is one
 * level down, not two. */
typedef struct {
    u32 marker_col;    /* column the "-" / "1." marker starts at */
    u32 content_col;   /* column the item's text starts at */
    bool ordered;      /* "1." rather than "-": its text column is wider */
} MdListLevel;

typedef struct {
    MdListLevel lv[MD_MAX_LIST_DEPTH];
    u32         depth;
    /* Running ordinal per nesting level, so an ordered list RENUMBERS: the
     * common "1. / 1. / 1." spelling has to come out 1, 2, 3, and a list that
     * starts at "5." has to continue 6, 7. Kept outside `lv` because siblings
     * pop and re-push their level, which would otherwise reset the count on
     * every item. */
    u16         ord[MD_MAX_LIST_DEPTH];
    u32         prev_depth;
} MdListStack;

/* Depth for a new item whose marker sits at `marker_col` and whose text starts
 * at `content_col`. Pops every open item the marker has dedented out of.
 *
 * `same_list` says whether the previous emitted block was this list's previous
 * item; when it is not, the numbering restarts at the source number.
 * `*io_index` carries the author's number in and the display number out. */
static u8 list_stack_push(MdListStack *st, u32 marker_col, u32 content_col,
                          bool same_list, u16 *io_index) {
    while ((st->depth > 0 && marker_col < st->lv[st->depth - 1].content_col) && md_work_step())
        st->depth--;
    u8 depth = (u8)(st->depth < 255 ? st->depth : 255);
    if (st->depth < MD_MAX_LIST_DEPTH) {
        st->lv[st->depth].marker_col  = marker_col;
        st->lv[st->depth].content_col = content_col;
        st->lv[st->depth].ordered     = io_index != NULL;
        st->depth++;
    }
    if (io_index) {
        u32 d = depth < MD_MAX_LIST_DEPTH ? depth : MD_MAX_LIST_DEPTH - 1;
        /* A fresh list, or a level entered from a shallower one, takes the
         * author's start number; anything else continues the count. */
        if (!same_list || depth > st->prev_depth) st->ord[d] = *io_index ? *io_index : 1;
        else if (st->ord[d] < UINT16_MAX)         st->ord[d]++;
        *io_index = st->ord[d];
    }
    st->prev_depth = depth;
    return depth;
}

/* Content column of the innermost open list item, or 0 at top level. Lines
 * indented at least this far continue that item rather than starting a new
 * top-level block. */
static u32 list_stack_content_col(const MdListStack *st) {
    return st->depth ? st->lv[st->depth - 1].content_col : 0u;
}

/* Is `body` a horizontal-rule line? Three or more of '-', '*', or '_',
 * possibly separated by single spaces, with nothing else. */
static bool is_hr(MdSlice body) {
    if (body.len < 3) return false;
    u8 marker = body.data[0];
    if (marker != '-' && marker != '*' && marker != '_') return false;
    u32 count = 0;
    for (u32 i = 0;( i < body.len) && md_work_step(); i++) {
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
    while ((i < body.len && body.data[i] == '#' && i < 7) && md_work_step()) i++;
    if (i == 0 || i > 6) return false;
    if (i < body.len && body.data[i] != ' ') return false;
    *level_out = (u8)i;
    *text_start = i < body.len ? i + 1 : i;
    return true;
}

/* Bytes of the list marker `body` opens with, or 0: a dash, star or plus, or
 * up to nine digits and a "." or ")" — in every case followed by a space or a
 * tab. An ordered marker reports its number through *out_num. */
static u32 list_marker(MdSlice body, bool *out_ordered, u32 *out_num) {
    if (out_ordered) *out_ordered = false;
    if (out_num)     *out_num = 0;
    if (body.len == 0) return 0;
    if (body.data[0] == '-' || body.data[0] == '*' || body.data[0] == '+')
        return (body.len == 1 || body.data[1] == ' ' || body.data[1] == '\t') ? 1u : 0u;
    u32 d = 0, num = 0;
    while ((d < body.len && d < 9 && body.data[d] >= '0' && body.data[d] <= '9') && md_work_step()) {
        num = num * 10 + (u32)(body.data[d] - '0');
        d++;
    }
    /* "1." and "1)" are both ordered-list markers (CommonMark). */
    if (d == 0 || d >= body.len) return 0;
    if (body.data[d] != '.' && body.data[d] != ')') return 0;
    if (d + 1 < body.len && body.data[d + 1] != ' ' && body.data[d + 1] != '\t') return 0;
    if (out_ordered) *out_ordered = true;
    if (out_num)     *out_num = num;
    return d + 1;
}

/* Table capacity. Both are DEFENSIVE caps on untrusted input, not format
 * limits, and both truncate rather than corrupt:
 *   - past MD_TABLE_MAX_COLS the extra columns are dropped from every row
 *     (the separator's column count is clamped to match, so the cell grid and
 *     the alignment string can never disagree about the width);
 *   - past MD_TABLE_MAX_ROWS the table block ends and the rows below it open
 *     a fresh one that repeats the header row, as a printed table does on its
 *     next page. The rows cannot simply be handed back to the block parser:
 *     with no header and no delimiter row above them they are not a table to
 *     it, and they would come back as ONE paragraph of pipes. Parts, rather
 *     than one block of any size, keep what a frame lays out bounded by what
 *     is on screen.
 * Nothing is silently merged into the wrong cell in either case. */
#define MD_TABLE_MAX_COLS 32u
#define MD_TABLE_MAX_ROWS 256u

/* Match table separator row: each pipe-bounded cell is /:?-+:?/. */
static bool is_table_separator(MdSlice body, u16 *cols_out, char *aligns) {
    /* Strip leading/trailing pipe + whitespace. */
    u32 i = 0, end = body.len;
    while ((i < end && (body.data[i] == ' ' || body.data[i] == '\t')) && md_work_step()) i++;
    if (i < end && body.data[i] == '|') i++;
    while ((end > i && (body.data[end-1] == ' ' || body.data[end-1] == '\t')) && md_work_step()) end--;
    if (end > i && body.data[end-1] == '|') end--;
    if (i >= end) return false;

    u16 cols = 0;
    while ((i < end) && md_work_step()) {
        u32 cs = i;
        while ((i < end && body.data[i] != '|') && md_work_step()) {
            /* A pipe escaped with a backslash belongs to the cell, not to the
             * row: `[[Link\|Display]]` and `![[img\|100]]` are what the
             * escape is for. */
            if (body.data[i] == '\\' && i + 1 < end) i += 2; else i++;
        }
        u32 ce = i;
        /* trim cell */
        while ((cs < ce && (body.data[cs] == ' ' || body.data[cs] == '\t')) && md_work_step()) cs++;
        while ((ce > cs && (body.data[ce-1] == ' ' || body.data[ce-1] == '\t')) && md_work_step()) ce--;
        if (cs >= ce) return false;
        bool lc = body.data[cs] == ':';
        bool rc = body.data[ce-1] == ':';
        u32 ds = cs + (lc ? 1 : 0);
        u32 de = ce - (rc ? 1 : 0);
        if (ds >= de) return false;
        for (u32 k = ds;( k < de) && md_work_step(); k++) if (body.data[k] != '-') return false;
        /* `aligns` holds MD_TABLE_MAX_COLS entries; cap the reported column
         * count to it. Counting past the cap made the caller memcpy `cols`
         * bytes out of that buffer (over-read). Extra columns are dropped. */
        if (cols < MD_TABLE_MAX_COLS) {
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
    for (u32 i = 0;( i < body.len) && md_work_step(); i++) {
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

/* The characters a backslash may escape. CommonMark's list is exactly the
 * ASCII punctuation set, and the boundary matters in both directions: too
 * narrow and "\*" stays an emphasis marker, too wide and the backslash in
 * "C:\Users" is eaten as an escape of 'U'. */
static bool is_md_punct(u8 c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

/* Advance one codepoint, returning bytes consumed (>=1 even for invalid). */
static u32 cp_advance(const u8 *p, u32 left) {
    u32 cp = 0;
    u32 n = utf8_decode(p, left, &cp);
    return n ? n : 1;
}

/* A byte that belongs to a word: a letter, a digit, or any part of a non-ASCII
 * character. What '_' may not open or close emphasis next to. */
static bool is_word_byte(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c >= 0x80;
}

static bool is_inline_space(u8 c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void emph_flanks(u8 prev, u8 next, u8 marker, bool *opens, bool *closes) {
    bool left = !is_inline_space(next) &&
                (!is_md_punct(next) || is_inline_space(prev) || is_md_punct(prev));
    bool right = !is_inline_space(prev) &&
                 (!is_md_punct(prev) || is_inline_space(next) || is_md_punct(next));
    *opens = left && (marker != '_' || !right || is_md_punct(prev));
    *closes = right && (marker != '_' || !left || is_md_punct(next));
}

/* Find a closing delimiter run of `n` matching chars starting search at
 * `from`. Returns position of closer's first byte, or -1 if not found.
 * Skips over backslash-escapes and inline-code spans so they don't count
 * as closers. */
static i64 find_delim(MdSlice s, u32 from, u8 ch, u32 n) {
    /* Emphasis needs flanking rules; the symmetric markers (~~ %%) do not.
     * "%% hidden %%" closes on a run preceded by a space, which a flanking
     * test would reject outright. */
    bool flanking = (ch == '*' || ch == '_');
    /* "==" sits between the two: no nesting to count, but its closer has to
     * touch the text it marks, the way an emphasis closer does. Without that
     * "a == b and c == d" highlighted everything between the comparisons. */
    bool hug = (ch == '=');
    /* Unmatched opener markers seen so far, counted in CHARACTERS. A nested
     * "**" inside a "*" span owes two closing chars, so a later "**" spends
     * itself on that and must not be mistaken for this span's closer —
     * otherwise "*a **b** c*" would close on the "**" before "b" and come
     * out as one italic run reading "a *". */
    u32 pending = 0;
    u32 i = from;
    while ((i < s.len) && md_work_step()) {
        u8 c = s.data[i];
        if (c == '\\' && i + 1 < s.len) { i += 2; continue; }
        if (c == '`') {
            /* skip code span */
            u32 run = 0;
            while ((i + run < s.len && s.data[i + run] == '`') && md_work_step()) run++;
            u32 j = i + run;
            while ((j < s.len) && md_work_step()) {
                if (s.data[j] == '`') {
                    u32 run2 = 0;
                    while ((j + run2 < s.len && s.data[j + run2] == '`') && md_work_step()) run2++;
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
            while ((i + run < s.len && s.data[i + run] == ch) && md_work_step()) run++;
            if (!flanking) {
                bool hugs = i > from && !is_inline_space(s.data[i - 1]);
                if (run >= n && (!hug || hugs)) return (i64)i;
                i += run;
                continue;
            }
            u8 prev = (i > 0)             ? s.data[i - 1]     : ' ';
            u8 next = (i + run < s.len)   ? s.data[i + run]   : ' ';
            bool can_close, can_open;
            emph_flanks(prev, next, ch, &can_open, &can_close);
            /* '_' is a word character to everyone who writes identifiers, so
             * CommonMark lets it emphasise only at a word's edge: a run with a
             * word byte after it does not close, one with a word byte before
             * it does not open, and the underscore inside "_private_method"
             * closes nothing. '*' has no such rule. */
            if (ch == '_') {
                if (is_word_byte(next)) can_close = false;
                if (is_word_byte(prev)) can_open  = false;
            }
            if (can_close && run >= pending + n) return (i64)(i + pending);
            if (can_close && pending) pending -= (run < pending ? run : pending);
            else if (can_open)        pending += run;
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

/* Split an Obsidian target — "Note#Heading|Alias", "img.png|640x480",
 * "Note#^blk" — into its parts.
 *
 * Two separators, two jobs: `#` starts a heading (or a block id) and is not
 * part of the file name, and the `|` suffix is a SIZE when it reads as digits
 * or `WxH` (`![[img.png|300]]` is an image 300 wide), otherwise it is the
 * alias. That is how the syntax disambiguates the one separator serving both.
 */
typedef struct {
    MdSlice path;     /* target without anchor or suffix */
    MdSlice anchor;   /* heading text or block id, '#'/'^' stripped */
    bool    anchor_is_block;
    MdSlice alias;    /* display override, empty when the suffix was a size */
    u16     w, h;     /* 0 when the suffix was not a size */
} MdTarget;

static bool target_parse_size(MdSlice s, u16 *out_w, u16 *out_h) {
    if (s.len == 0 || s.len > 11) return false;
    u32 w = 0, h = 0, i = 0, digits = 0;
    while ((i < s.len && s.data[i] >= '0' && s.data[i] <= '9') && md_work_step()) {
        w = w * 10 + (u32)(s.data[i] - '0');
        if (w > 20000) return false;
        i++; digits++;
    }
    if (digits == 0) return false;
    if (i == s.len) { *out_w = (u16)w; *out_h = 0; return true; }
    if (s.data[i] != 'x' && s.data[i] != 'X') return false;
    i++; digits = 0;
    while ((i < s.len && s.data[i] >= '0' && s.data[i] <= '9') && md_work_step()) {
        h = h * 10 + (u32)(s.data[i] - '0');
        if (h > 20000) return false;
        i++; digits++;
    }
    if (digits == 0 || i != s.len) return false;
    *out_w = (u16)w; *out_h = (u16)h;
    return true;
}

static MdTarget target_split(MdSlice body) {
    MdTarget t = {0};
    t.path = body;
    /* `|` last: an alias may itself contain '#', so the suffix comes off
     * before the anchor is looked for.
     *
     * `\|` is the SAME separator. Inside a table the backslash is mandatory —
     * it is what stops the table parser splitting the cell — so treating it as
     * an escape to step over left the whole "Note\|Alias" as the path, which
     * then resolved to a file with a backslash in its name and dropped the
     * alias (and, for embeds, the |size). */
    for (u32 k = 0;( k < body.len) && md_work_step(); k++) {
        u32 sep = 0;                     /* separator width, 0 = not one */
        if (body.data[k] == '|') sep = 1;
        else if (body.data[k] == '\\' && k + 1 < body.len && body.data[k + 1] == '|') sep = 2;
        else continue;
        MdSlice suffix = slice_make(body.data + k + sep, body.len - k - sep);
        t.path = slice_make(body.data, k);
        if (!target_parse_size(suffix, &t.w, &t.h)) t.alias = suffix;
        break;
    }
    for (u32 k = 0;( k < t.path.len) && md_work_step(); k++) {
        if (t.path.data[k] != '#') continue;
        MdSlice anc = slice_make(t.path.data + k + 1, t.path.len - k - 1);
        t.path = slice_make(t.path.data, k);
        if (anc.len && anc.data[0] == '^') {
            t.anchor_is_block = true;
            t.anchor = slice_make(anc.data + 1, anc.len - 1);
        } else {
            t.anchor = anc;
        }
        break;
    }
    return t;
}

/* Split a link destination "(url)" body into the URL and drop the optional
 * title, so `[a](url "Title")` opens `url` alone. Angle-bracketed destinations
 * `<url with spaces>` are unwrapped. */
static MdSlice link_dest_only(MdSlice body) {
    u32 i = 0;
    while ((i < body.len && (body.data[i] == ' ' || body.data[i] == '\t')) && md_work_step()) i++;
    if (i < body.len && body.data[i] == '<') {
        u32 j = i + 1;
        while ((j < body.len && body.data[j] != '>') && md_work_step()) j++;
        return slice_make(body.data + i + 1, j - i - 1);
    }
    u32 s = i;
    while ((i < body.len && body.data[i] != ' ' && body.data[i] != '\t') && md_work_step()) i++;
    return slice_make(body.data + s, i - s);
}

/* Index just past the code span that opens with the backtick run at `at`, or
 * just past that run when nothing closes it (the backticks are then literal,
 * which is what the tokenizer makes of them too). */
static u32 code_span_end(MdSlice s, u32 at) {
    u32 run = 0;
    while ((at + run < s.len && s.data[at + run] == '`') && md_work_step()) run++;
    u32 j = at + run;
    while ((j < s.len) && md_work_step()) {
        if (s.data[j] != '`') { j++; continue; }
        u32 run2 = 0;
        while ((j + run2 < s.len && s.data[j + run2] == '`') && md_work_step()) run2++;
        if (run2 == run) return j + run2;
        j += run2;
    }
    return at + run;
}

/* The ']' that closes the '[' at `open`.
 *
 * Link text and image alt text are not "up to the first ']'": brackets inside
 * them nest as long as they balance. That is what makes a README badge — an
 * image INSIDE a link, `[![Build](img.svg)](https://ci)` — one link; taking
 * the first ']' would read it as a link to the image with the text "![Build",
 * followed by a stray "](" and a bare URL. A backslash escapes a bracket and
 * a code span hides the ones inside it. */
static bool bracket_close(MdSlice s, u32 open, u32 *out_close) {
    u32 depth = 0;
    u32 j = open;
    while ((j < s.len) && md_work_step()) {
        u8 c = s.data[j];
        if (c == '\\' && j + 1 < s.len) { j += 2; continue; }
        if (c == '`') { j = code_span_end(s, j); continue; }
        if (c == '[') depth++;
        else if (c == ']' && --depth == 0) { *out_close = j; return true; }
        j++;
    }
    return false;
}

/* What follows the "](" of an inline link or image, read the way CommonMark
 * writes it: a destination, an optional title, the closing ')'. `lp` is the
 * index of the '('.
 *
 * The destination is not "up to the first ')'" either. Parentheses inside it
 * belong to it while they balance — every Wikipedia disambiguation page has a
 * pair — and a quoted title may hold any character up to its own closing
 * quote. Returns false when the text does not have that shape. */
static bool link_tail_strict(MdSlice s, u32 lp, MdSlice *out_dest, u32 *out_close) {
    u32 i = lp + 1;
    while ((i < s.len && is_inline_space(s.data[i])) && md_work_step()) i++;
    MdSlice dest;
    if (i < s.len && s.data[i] == '<') {
        u32 j = i + 1;
        while ((j < s.len && s.data[j] != '>' && s.data[j] != '\n') && md_work_step()) j++;
        if (j >= s.len || s.data[j] != '>') return false;
        dest = slice_make(s.data + i + 1, j - i - 1);
        i = j + 1;
    } else {
        u32 start = i, depth = 0;
        while ((i < s.len) && md_work_step()) {
            u8 c = s.data[i];
            if (c == '\\' && i + 1 < s.len) { i += 2; continue; }
            if (is_inline_space(c)) break;
            if (c == '(') depth++;
            else if (c == ')') { if (depth == 0) break; depth--; }
            i++;
        }
        if (depth != 0) return false;
        dest = slice_make(s.data + start, i - start);
    }
    while ((i < s.len && is_inline_space(s.data[i])) && md_work_step()) i++;
    if (i < s.len && (s.data[i] == '"' || s.data[i] == '\'' || s.data[i] == '(')) {
        u8 end = s.data[i] == '(' ? ')' : s.data[i];
        u32 j = i + 1;
        while ((j < s.len && s.data[j] != end) && md_work_step())
            j += (s.data[j] == '\\' && j + 1 < s.len) ? 2u : 1u;
        if (j >= s.len) return false;
        i = j + 1;
        while ((i < s.len && is_inline_space(s.data[i])) && md_work_step()) i++;
    }
    if (i >= s.len || s.data[i] != ')') return false;
    *out_dest  = dest;
    *out_close = i;
    return true;
}

/* The tail of an inline link, strictly when it is well formed and otherwise
 * leniently: the link ends at the first ')' and its destination is the first
 * word before that. The lenient reading goes beyond CommonMark (which makes
 * no link at all of "[a](one two)"), so loosely written links still open. */
static bool link_tail(MdSlice s, u32 lp, MdSlice *out_dest, u32 *out_close) {
    if (link_tail_strict(s, lp, out_dest, out_close)) return true;
    u32 close_paren = lp + 1;
    while ((close_paren < s.len && s.data[close_paren] != ')') && md_work_step()) close_paren++;
    if (close_paren >= s.len) return false;
    *out_dest  = link_dest_only(slice_make(s.data + lp + 1, close_paren - (lp + 1)));
    *out_close = close_paren;
    return true;
}

/* ---- character references ----
 * "&amp;", "&#8212;", "&#x2014;": text that names a character instead of
 * containing it. The AST is zero-copy, so a decoded character cannot be
 * written back over the reference; it becomes a TEXT run of its own whose
 * slice is the UTF-8 for it — a string constant for a named reference, four
 * arena bytes for a numeric one. The renderer already takes text slices from
 * outside the source (they carry no source offset).
 *
 * The named list is the references people type or paste, not HTML's 2,231:
 * a name that is not here stays as written, which is also what CommonMark
 * does with a name it does not know. */
static const struct { const char *name; const char *utf8; } MD_ENTITIES[] = {
    { "amp", "&" },          { "lt", "<" },           { "gt", ">" },
    { "quot", "\"" },        { "apos", "'" },         { "nbsp", "\xc2\xa0" },
    { "copy", "\xc2\xa9" },  { "reg", "\xc2\xae" },   { "trade", "\xe2\x84\xa2" },
    { "hellip", "\xe2\x80\xa6" }, { "mdash", "\xe2\x80\x94" }, { "ndash", "\xe2\x80\x93" },
    { "lsquo", "\xe2\x80\x98" },  { "rsquo", "\xe2\x80\x99" }, { "sbquo", "\xe2\x80\x9a" },
    { "ldquo", "\xe2\x80\x9c" },  { "rdquo", "\xe2\x80\x9d" }, { "bdquo", "\xe2\x80\x9e" },
    { "laquo", "\xc2\xab" }, { "raquo", "\xc2\xbb" }, { "bull", "\xe2\x80\xa2" },
    { "middot", "\xc2\xb7" }, { "deg", "\xc2\xb0" },  { "plusmn", "\xc2\xb1" },
    { "times", "\xc3\x97" }, { "divide", "\xc3\xb7" }, { "minus", "\xe2\x88\x92" },
    { "micro", "\xc2\xb5" }, { "para", "\xc2\xb6" },  { "sect", "\xc2\xa7" },
    { "cent", "\xc2\xa2" },  { "pound", "\xc2\xa3" }, { "yen", "\xc2\xa5" },
    { "euro", "\xe2\x82\xac" }, { "curren", "\xc2\xa4" }, { "iexcl", "\xc2\xa1" },
    { "iquest", "\xc2\xbf" }, { "ordf", "\xc2\xaa" }, { "ordm", "\xc2\xba" },
    { "not", "\xc2\xac" },   { "shy", "\xc2\xad" },   { "dagger", "\xe2\x80\xa0" },
    { "Dagger", "\xe2\x80\xa1" }, { "permil", "\xe2\x80\xb0" }, { "prime", "\xe2\x80\xb2" },
    { "Prime", "\xe2\x80\xb3" }, { "frac12", "\xc2\xbd" }, { "frac14", "\xc2\xbc" },
    { "frac34", "\xc2\xbe" }, { "sup1", "\xc2\xb9" }, { "sup2", "\xc2\xb2" },
    { "sup3", "\xc2\xb3" },  { "ensp", "\xe2\x80\x82" }, { "emsp", "\xe2\x80\x83" },
    { "thinsp", "\xe2\x80\x89" },
    { "larr", "\xe2\x86\x90" }, { "uarr", "\xe2\x86\x91" }, { "rarr", "\xe2\x86\x92" },
    { "darr", "\xe2\x86\x93" }, { "harr", "\xe2\x86\x94" }, { "lArr", "\xe2\x87\x90" },
    { "rArr", "\xe2\x87\x92" }, { "hArr", "\xe2\x87\x94" },
    { "le", "\xe2\x89\xa4" }, { "ge", "\xe2\x89\xa5" }, { "ne", "\xe2\x89\xa0" },
    { "asymp", "\xe2\x89\x88" }, { "equiv", "\xe2\x89\xa1" }, { "infin", "\xe2\x88\x9e" },
    { "sum", "\xe2\x88\x91" }, { "prod", "\xe2\x88\x8f" }, { "radic", "\xe2\x88\x9a" },
    { "check", "\xe2\x9c\x93" }, { "cross", "\xe2\x9c\x97" },
    { "hearts", "\xe2\x99\xa5" }, { "spades", "\xe2\x99\xa0" }, { "clubs", "\xe2\x99\xa3" },
    { "diams", "\xe2\x99\xa6" },
    { "Agrave", "\xc3\x80" }, { "Aacute", "\xc3\x81" }, { "Acirc", "\xc3\x82" },
    { "Atilde", "\xc3\x83" }, { "Auml", "\xc3\x84" },   { "Aring", "\xc3\x85" },
    { "AElig", "\xc3\x86" },  { "Ccedil", "\xc3\x87" }, { "Egrave", "\xc3\x88" },
    { "Eacute", "\xc3\x89" }, { "Ecirc", "\xc3\x8a" },  { "Euml", "\xc3\x8b" },
    { "Igrave", "\xc3\x8c" }, { "Iacute", "\xc3\x8d" }, { "Icirc", "\xc3\x8e" },
    { "Iuml", "\xc3\x8f" },   { "Ntilde", "\xc3\x91" }, { "Ograve", "\xc3\x92" },
    { "Oacute", "\xc3\x93" }, { "Ocirc", "\xc3\x94" },  { "Otilde", "\xc3\x95" },
    { "Ouml", "\xc3\x96" },   { "Oslash", "\xc3\x98" }, { "Ugrave", "\xc3\x99" },
    { "Uacute", "\xc3\x9a" }, { "Ucirc", "\xc3\x9b" },  { "Uuml", "\xc3\x9c" },
    { "Yacute", "\xc3\x9d" }, { "szlig", "\xc3\x9f" },
    { "agrave", "\xc3\xa0" }, { "aacute", "\xc3\xa1" }, { "acirc", "\xc3\xa2" },
    { "atilde", "\xc3\xa3" }, { "auml", "\xc3\xa4" },   { "aring", "\xc3\xa5" },
    { "aelig", "\xc3\xa6" },  { "ccedil", "\xc3\xa7" }, { "egrave", "\xc3\xa8" },
    { "eacute", "\xc3\xa9" }, { "ecirc", "\xc3\xaa" },  { "euml", "\xc3\xab" },
    { "igrave", "\xc3\xac" }, { "iacute", "\xc3\xad" }, { "icirc", "\xc3\xae" },
    { "iuml", "\xc3\xaf" },   { "ntilde", "\xc3\xb1" }, { "ograve", "\xc3\xb2" },
    { "oacute", "\xc3\xb3" }, { "ocirc", "\xc3\xb4" },  { "otilde", "\xc3\xb5" },
    { "ouml", "\xc3\xb6" },   { "oslash", "\xc3\xb8" }, { "ugrave", "\xc3\xb9" },
    { "uacute", "\xc3\xba" }, { "ucirc", "\xc3\xbb" },  { "uuml", "\xc3\xbc" },
    { "yacute", "\xc3\xbd" }, { "yuml", "\xc3\xbf" },
    { "alpha", "\xce\xb1" }, { "beta", "\xce\xb2" },   { "gamma", "\xce\xb3" },
    { "delta", "\xce\xb4" }, { "epsilon", "\xce\xb5" }, { "theta", "\xce\xb8" },
    { "lambda", "\xce\xbb" }, { "mu", "\xce\xbc" },    { "pi", "\xcf\x80" },
    { "sigma", "\xcf\x83" }, { "tau", "\xcf\x84" },    { "phi", "\xcf\x86" },
    { "omega", "\xcf\x89" }, { "Delta", "\xce\x94" },  { "Sigma", "\xce\xa3" },
    { "Omega", "\xce\xa9" },
};

/* Decode the character reference that starts at the '&' at `at`. Returns the
 * bytes it occupies in the source (0 = not a reference, leave it as text) and
 * the character's UTF-8 through *out. */
static u32 entity_decode(MdSlice s, u32 at, Arena *arena, MdSlice *out) {
    u32 i = at + 1;
    if (i < s.len && s.data[i] == '#') {
        i++;
        bool hex = i < s.len && (s.data[i] == 'x' || s.data[i] == 'X');
        if (hex) i++;
        u32 cp = 0, digits = 0;
        /* 7 decimal / 6 hexadecimal digits is CommonMark's own bound, and it
         * keeps the value inside 32 bits. */
        while ((i < s.len && digits < (hex ? 6u : 7u)) && md_work_step()) {
            u8 c = s.data[i];
            u32 v;
            if (c >= '0' && c <= '9')             v = (u32)(c - '0');
            else if (hex && c >= 'a' && c <= 'f') v = (u32)(c - 'a') + 10;
            else if (hex && c >= 'A' && c <= 'F') v = (u32)(c - 'A') + 10;
            else break;
            cp = cp * (hex ? 16u : 10u) + v;
            digits++; i++;
        }
        if (digits == 0 || i >= s.len || s.data[i] != ';') return 0;
        /* NUL, a surrogate and anything past the last code point are not
         * characters: U+FFFD, as CommonMark asks. */
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            *out = slice_make((const u8 *)"\xef\xbf\xbd", 3);
            return i + 1 - at;
        }
        u8 *buf = arena ? (u8 *)arena_alloc(arena, 4) : NULL;
        if (!buf) return 0;                     /* refused: the text stays */
        u32 n = utf8_encode(cp, buf);
        if (n == 0) return 0;
        *out = slice_make(buf, n);
        return i + 1 - at;
    }
    u32 start = i;
    while ((i < s.len && i - start < 10 &&
            ((s.data[i] >= 'a' && s.data[i] <= 'z') || (s.data[i] >= 'A' && s.data[i] <= 'Z') ||
             (s.data[i] >= '0' && s.data[i] <= '9'))) && md_work_step()) i++;
    if (i == start || i >= s.len || s.data[i] != ';') return 0;
    MdSlice name = slice_make(s.data + start, i - start);
    if (!md_work_step()) return 0;
    for (u32 k = 0; k < sizeof MD_ENTITIES / sizeof MD_ENTITIES[0]; k++) {
        const char *nm = MD_ENTITIES[k].name;
        if (strlen(nm) != name.len || memcmp(nm, name.data, name.len) != 0) continue;
        *out = slice_make((const u8 *)MD_ENTITIES[k].utf8, (u32)strlen(MD_ENTITIES[k].utf8));
        return i + 1 - at;
    }
    return 0;
}

/* Emit a wikilink / embed / image from an already-split target. */
static void push_target_run(InlineVec *v, u8 kind, const MdTarget *t) {
    VEC_PUSH(v, MdInline);
    MdInline *r = &v->data[v->len++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->url   = t->path;
    r->alt   = t->alias;
    r->anchor = t->anchor;
    r->anchor_is_block = t->anchor_is_block ? 1 : 0;
    r->img_w = t->w;
    r->img_h = t->h;
}


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
        MdInline *kids = arena_alloc_array(arena, (usize)sub.len, sizeof(MdInline));
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

static MdSlice link_unescape(Arena *arena, MdSlice src) {
    if (!src.len || !memchr(src.data, '\\', src.len)) return src;
    u8 *buf = arena_alloc(arena, src.len);
    if (!buf) return src;
    u32 n = 0;
    for (u32 i = 0; i < src.len; i++) {
        if (src.data[i] == '\\' && i + 1 < src.len && is_md_punct(src.data[i + 1])) i++;
        buf[n++] = src.data[i];
    }
    return slice_make(buf, n);
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
    out->data[idx].url  = link_unescape(arena, url);
    out->data[idx].alt  = alt;
    push_children_from(&out->data[idx], text, arena, depth);
}

static void inline_tokenize(MdSlice slice, InlineVec *out, Arena *arena, int depth) {
    u32 i = 0;
    u32 text_start = 0;

#define FLUSH_TEXT() do { push_text_run(out, slice, text_start, i); text_start = i; } while (0)

    while ((i < slice.len) && md_work_step()) {
        u8 c = slice.data[i];

        /* Line break inside a paragraph. CommonMark distinguishes two, and the
         * distinction is the parser's to make — the renderer only lays out what
         * it is handed:
         *
         *   HARD  two-or-more trailing spaces, or a trailing backslash
         *         → a real new line (MD_INLINE_LINEBREAK)
         *   SOFT  anything else → a SPACE; the paragraph reflows to the
         *         viewport instead of preserving the author's line endings
         *
         * The soft case leaves the '\n' byte inside the text run rather than
         * splitting it, so the run stays a zero-copy slice of the source and
         * every glyph keeps its source offset for find/selection. */
        if (c == '\\' && i + 1 < slice.len && slice.data[i + 1] == '\n') {
            FLUSH_TEXT();
            push_simple(out, MD_INLINE_LINEBREAK);
            i += 2;
            while (i < slice.len && (slice.data[i] == ' ' || slice.data[i] == '\t')) i++;
            text_start = i;
            continue;
        }
        if (c == '\n') {
            u32 sp = 0;
            while ((i > text_start + sp && slice.data[i - 1 - sp] == ' ') && md_work_step()) sp++;
            if (sp >= 2) {
                push_text_run(out, slice, text_start, i - sp);
                push_simple(out, MD_INLINE_LINEBREAK);
                i += 1;
                while (i < slice.len && (slice.data[i] == ' ' || slice.data[i] == '\t')) i++;
                text_start = i;
                continue;
            }
            i += 1;                      /* soft: stays in the text run */
            continue;
        }

        /* Backslash escape — but ONLY before ASCII punctuation, which is what
         * CommonMark defines as escapable. Escaping anything at all deleted the
         * backslash in "C:\Users\test" and "\d+", turning a Windows path or a
         * regex into mangled prose. Before a non-punctuation byte the backslash
         * is literal text and falls through. */
        if (c == '\\' && i + 1 < slice.len && is_md_punct(slice.data[i + 1])) {
            FLUSH_TEXT();
            i++;                              /* skip backslash */
            u32 n = cp_advance(slice.data + i, slice.len - i);
            push_text_run(out, slice, i, i + n);
            i += n;
            text_start = i;
            continue;
        }

        /* Character reference: "&amp;", "&#169;", "&#xA9;". Code spans are
         * taken below, so a reference inside one is never looked at here. */
        if (c == '&') {
            MdSlice ch = slice_empty();
            u32 used = entity_decode(slice, i, arena, &ch);
            if (used) {
                FLUSH_TEXT();
                push_styled_run(out, MD_INLINE_TEXT, ch);
                i += used;
                text_start = i;
                continue;
            }
        }

        /* Inline code: `...` or ``...`` matching backtick run length. */
        if (c == '`') {
            u32 run = 0;
            while ((i + run < slice.len && slice.data[i + run] == '`') && md_work_step()) run++;
            i64 close = -1;
            for (u32 j = i + run;( j + run <= slice.len) && md_work_step(); ) {
                if (slice.data[j] == '`') {
                    u32 r2 = 0;
                    while ((j + r2 < slice.len && slice.data[j + r2] == '`') && md_work_step()) r2++;
                    if (r2 == run) { close = (i64)j; break; }
                    j += r2;
                } else { j++; }
            }
            if (close >= 0) {
                FLUSH_TEXT();
                MdSlice body = slice_make(slice.data + i + run, (u32)((u32)close - i - run));
                /* CommonMark: strip a single leading + trailing space if both
                 * are present and the content isn't all spaces. */
                if (body.len >= 2 && (body.data[0] == ' ' || body.data[0] == '\n') && (body.data[body.len-1] == ' ' || body.data[body.len-1] == '\n')) {
                    bool all_space = true;
                    for (u32 k = 0;( k < body.len) && md_work_step(); k++) {
                        if (body.data[k] != ' ' && body.data[k] != '\n') { all_space = false; break; }
                    }
                    if (!all_space) { body.data++; body.len -= 2; }
                }
                push_styled_run(out, MD_INLINE_CODE, body);
                i = (u32)close + run;
                text_start = i;
                continue;
            }
            /* An unmatched RUN is literal; its suffix cannot open a span. */
            i += run;
            continue;
        }

        /* Embed `![[file]]` and image `![alt](url)`. */
        if (c == '!' && i + 1 < slice.len && slice.data[i + 1] == '[') {
            if (i + 2 < slice.len && slice.data[i + 2] == '[') {
                /* embed */
                u32 j = i + 3;
                while ((j + 1 < slice.len && !(slice.data[j] == ']' && slice.data[j+1] == ']')) && md_work_step()) j++;
                if (j + 1 < slice.len) {
                    FLUSH_TEXT();
                    MdSlice body = slice_make(slice.data + i + 3, j - (i + 3));
                    MdTarget t = target_split(body);
                    push_target_run(out, MD_INLINE_EMBED, &t);
                    i = j + 2;
                    text_start = i;
                    continue;
                }
            } else {
                /* image ![alt](url) — the alt text may hold balanced brackets
                 * and the destination balanced parentheses, as a link's may. */
                u32 close_bracket = 0, close_paren = 0;
                MdSlice url = slice_empty();
                if (bracket_close(slice, i + 1, &close_bracket) &&
                    close_bracket + 1 < slice.len &&
                    slice.data[close_bracket + 1] == '(' &&
                    link_tail(slice, close_bracket + 1, &url, &close_paren)) {
                    FLUSH_TEXT();
                    MdSlice alt = slice_make(slice.data + i + 2, close_bracket - (i + 2));
                    /* Obsidian puts the display size in the ALT slot for
                     * markdown-style images: ![alt|300](url). */
                    MdTarget t = {0};
                    t.path = link_unescape(arena, url);
                    t.alias = alt;
                    for (u32 k = alt.len;( k > 0) && md_work_step(); k--) {
                        if (alt.data[k - 1] != '|') continue;
                        MdSlice suffix = slice_make(alt.data + k, alt.len - k);
                        if (target_parse_size(suffix, &t.w, &t.h))
                            t.alias = slice_make(alt.data, k - 1);
                        break;
                    }
                    push_target_run(out, MD_INLINE_IMAGE, &t);
                    i = close_paren + 1;
                    text_start = i;
                    continue;
                }
                if (bracket_close(slice, i + 1, &close_bracket)) {
                    FLUSH_TEXT();
                    VEC_PUSH(out, MdInline);
                    MdInline *r = &out->data[out->len++];
                    memset(r, 0, sizeof *r);
                    r->kind = MD_INLINE_IMAGE;
                    r->flags = MD_LINK_REF_PENDING;
                    r->text = slice_make(slice.data + i + 2, close_bracket - i - 2);
                    r->alt = r->text;
                    r->url = r->text;
                    i = close_bracket + 1;
                    text_start = i;
                    continue;
                }
            }
        }

        /* Wikilink `[[Page|alias]]`, footnote `[^id]`, link `[text](url)`. */
        if (c == '[') {
            if (i + 1 < slice.len && slice.data[i + 1] == '[') {
                u32 j = i + 2;
                while ((j + 1 < slice.len && !(slice.data[j] == ']' && slice.data[j+1] == ']')) && md_work_step()) j++;
                if (j + 1 < slice.len) {
                    FLUSH_TEXT();
                    MdSlice body = slice_make(slice.data + i + 2, j - (i + 2));
                    MdTarget t = target_split(body);
                    push_target_run(out, MD_INLINE_WIKILINK, &t);
                    i = j + 2;
                    text_start = i;
                    continue;
                }
            } else if (i + 1 < slice.len && slice.data[i + 1] == '^') {
                /* footnote reference */
                u32 j = i + 2;
                while ((j < slice.len && slice.data[j] != ']') && md_work_step()) j++;
                if (j < slice.len) {
                    FLUSH_TEXT();
                    MdSlice id = slice_make(slice.data + i + 2, j - (i + 2));
                    push_styled_run(out, MD_INLINE_FOOTNOTE_REF, id);
                    i = j + 1;
                    text_start = i;
                    continue;
                }
            } else {
                u32 close_bracket = 0;
                if (bracket_close(slice, i, &close_bracket)) {
                    MdSlice text = slice_make(slice.data + i + 1, close_bracket - (i + 1));
                    /* inline link: [text](destination "title") */
                    MdSlice url = slice_empty();
                    u32 close_paren = 0;
                    if (close_bracket + 1 < slice.len &&
                        slice.data[close_bracket + 1] == '(' &&
                        link_tail(slice, close_bracket + 1, &url, &close_paren)) {
                        FLUSH_TEXT();
                        push_link_rec(out, MD_INLINE_LINK, text, url, slice_empty(), arena, depth);
                        /* A nested link can be discovered only later, when
                         * references resolve. Keep the literal closing syntax
                         * in the link's otherwise-unused alt slot for that case. */
                        out->data[out->len - 1].alt = slice_make(slice.data + close_bracket,
                                                                close_paren - close_bracket + 1);
                        i = close_paren + 1;
                        text_start = i;
                        continue;
                    }
                    /* Anything else in brackets MAY be a reference link, and
                     * whether it is depends on definitions that can sit below
                     * it in the document. So ONE bracket group becomes one
                     * run, flagged MD_LINK_REF_PENDING with its own text as
                     * the label in `url`, and the resolve pass decides with
                     * the whole definition table in hand:
                     *   [text][label]  full      → two groups, the second
                     *                              names the label
                     *   [text][]       collapsed → the second group is empty
                     *   [label]        shortcut  → one group
                     * A group nothing defines goes back to being text —
                     * brackets and all, with whatever markup it holds still
                     * parsed (see linkref_resolve_runs). */
                    FLUSH_TEXT();
                    VEC_PUSH(out, MdInline);
                    u32 idx = out->len++;
                    memset(&out->data[idx], 0, sizeof(MdInline));
                    out->data[idx].kind  = MD_INLINE_LINK;
                    out->data[idx].flags = MD_LINK_REF_PENDING;
                    out->data[idx].text  = text;
                    out->data[idx].url   = text;
                    push_children_from(&out->data[idx], text, arena, depth);
                    i = close_bracket + 1;
                    text_start = i;
                    continue;
                }
            }
        }

        /* Bare-URL autolink: http(s)://… or <url>. */
        if (c == '<') {
            u32 j = i + 1;
            while ((j < slice.len && slice.data[j] != '>' && !is_inline_space(slice.data[j])) && md_work_step()) j++;
            if (j < slice.len && slice.data[j] == '>') {
                /* require URL-ish content */
                MdSlice body = slice_make(slice.data + i + 1, j - (i + 1));
                bool ok = false, email = false;
                for (u32 k = 0; k < body.len; k++) {
                    if (body.data[k] == ':' && k >= 2 && k <= 32 &&
                        ((body.data[0] >= 'a' && body.data[0] <= 'z') ||
                         (body.data[0] >= 'A' && body.data[0] <= 'Z'))) { ok = true; break; }
                    if (!is_url_scheme_char(body.data[k])) break;
                }
                const u8 *at = memchr(body.data, '@', body.len);
                if (!ok && at && at > body.data && at + 1 < body.data + body.len &&
                    memchr(at + 1, '.', (usize)(body.data + body.len - at - 1))) {
                    ok = true;
                    email = true;
                }
                if (ok) {
                    FLUSH_TEXT();
                    MdSlice url = body;
                    if (email) {
                        u8 *buf = arena_alloc(arena, body.len + 7);
                        if (buf) {
                            memcpy(buf, "mailto:", 7);
                            memcpy(buf + 7, body.data, body.len);
                            url = slice_make(buf, body.len + 7);
                        }
                    }
                    push_link_rec(out, MD_INLINE_LINK, body, url, slice_empty(), arena, depth);
                    out->data[out->len - 1].children = NULL;
                    out->data[out->len - 1].child_count = 0;
                    i = j + 1;
                    text_start = i;
                    continue;
                }
            }
        }
        if ((c == 'h' || c == 'H') && i + 7 < slice.len &&
            (memcmp(slice.data + i, "http://", 7) == 0 ||
             (memcmp(slice.data + i, "https://", 8) == 0 && i + 8 < slice.len))) {
            /* A parenthesis is part of the URL while it is balanced INSIDE the
             * URL ("…/wiki/Foo_(bar)"); the ')' that closes one opened before
             * the URL began — "(see https://example.com)" — ends it. */
            u32 j = i, paren = 0;
            while ((j < slice.len) && md_work_step()) {
                u8 uc = slice.data[j];
                if (uc == '(') paren++;
                else if (uc == ')') { if (paren == 0) break; paren--; }
                else if (!is_url_char(uc)) break;
                j++;
            }
            /* trim trailing punctuation */
            while ((j > i && (slice.data[j-1] == '.' || slice.data[j-1] == ',' ||
                             slice.data[j-1] == ';' || slice.data[j-1] == ':' ||
                             slice.data[j-1] == '!' || slice.data[j-1] == '?')) && md_work_step()) j--;
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
            while ((j < slice.len && slice.data[j] != '~' && slice.data[j] != ' ' &&
                   slice.data[j] != '\t' && slice.data[j] != '\n') && md_work_step()) j++;
            if (j < slice.len && slice.data[j] == '~' && j > i + 1) {
                FLUSH_TEXT();
                push_styled_run(out, MD_INLINE_SUBSCRIPT,
                                slice_make(slice.data + i + 1, j - (i + 1)));
                i = j + 1; text_start = i; continue;
            }
        }

        /* Inline HTML with a native treatment. There is no HTML engine here,
         * so a small table of tags that map onto existing treatments is
         * rendered, and everything else stays literal text rather than being
         * silently dropped. */
        if (c == '<' && i + 1 < slice.len) {
            /* `<!-- … -->` is a comment: hidden, like %%…%%. */
            if (i + 3 < slice.len && slice.data[i+1] == '!' &&
                slice.data[i+2] == '-' && slice.data[i+3] == '-') {
                u32 j = i + 4;
                while ((j + 2 < slice.len &&
                       !(slice.data[j] == '-' && slice.data[j+1] == '-' &&
                         slice.data[j+2] == '>')) && md_work_step()) j++;
                if (j + 2 < slice.len) {
                    FLUSH_TEXT();
                    i = j + 3; text_start = i; continue;
                }
            }
            static const struct { const char *tag; u8 flag; } HTML_TAGS[] = {
                { "kbd",    MD_HTML_KBD },
                { "mark",   MD_HTML_MARK },
                { "b",      MD_HTML_BOLD },
                { "strong", MD_HTML_BOLD },
                { "i",      MD_HTML_ITALIC },
                { "em",     MD_HTML_ITALIC },
                { "u",      MD_HTML_UNDERLINE },
            };
            /* `<br>` / `<br/>` — a hard line break. */
            if (slice.len - i >= 4 &&
                (slice.data[i+1] == 'b' || slice.data[i+1] == 'B') &&
                (slice.data[i+2] == 'r' || slice.data[i+2] == 'R')) {
                u32 j = i + 3;
                while ((j < slice.len && slice.data[j] != '>') && md_work_step()) j++;
                if (j < slice.len && j - i <= 6) {
                    FLUSH_TEXT();
                    push_simple(out, MD_INLINE_LINEBREAK);
                    i = j + 1; text_start = i; continue;
                }
            }
            bool matched = false;
            for (u32 ti = 0;( ti < sizeof HTML_TAGS / sizeof HTML_TAGS[0] && !matched) && md_work_step(); ti++) {
                const char *tag = HTML_TAGS[ti].tag;
                u32 tl = (u32)strlen(tag);
                if (i + tl + 1 >= slice.len) continue;
                if (slice.data[i + tl + 1] != '>') continue;
                bool same = true;
                for (u32 k = 0;( k < tl) && md_work_step(); k++) {
                    u8 a = slice.data[i + 1 + k], b2 = (u8)tag[k];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (a != b2) { same = false; break; }
                }
                if (!same) continue;
                /* Find the matching close tag. */
                u32 body_start = i + tl + 2;
                u32 j = body_start;
                bool closed = false;
                while ((j + tl + 2 < slice.len) && md_work_step()) {
                    if (slice.data[j] == '<' && slice.data[j+1] == '/') {
                        bool cs = true;
                        for (u32 k = 0;( k < tl) && md_work_step(); k++) {
                            u8 a = slice.data[j + 2 + k], b2 = (u8)tag[k];
                            if (a >= 'A' && a <= 'Z') a += 32;
                            if (a != b2) { cs = false; break; }
                        }
                        if (cs && slice.data[j + 2 + tl] == '>') { closed = true; break; }
                    }
                    j++;
                }
                if (!closed) continue;
                FLUSH_TEXT();
                MdSlice body = slice_make(slice.data + body_start, j - body_start);
                VEC_PUSH(out, MdInline);
                u32 idx = out->len++;
                memset(&out->data[idx], 0, sizeof(MdInline));
                out->data[idx].kind  = MD_INLINE_HTML;
                out->data[idx].flags = HTML_TAGS[ti].flag;
                out->data[idx].text  = body;
                push_children_from(&out->data[idx], body, arena, depth);
                i = j + tl + 3;
                text_start = i;
                matched = true;
            }
            if (matched) continue;
        }

        /* Inline footnote `^[text]`. Numbered alongside the [^id] refs; the
         * body travels in `url` so the collection pass can emit it as a
         * definition without a second parse of the source. */
        if (c == '^' && i + 1 < slice.len && slice.data[i + 1] == '[') {
            u32 depth_br = 1;
            u32 j = i + 2;
            while ((j < slice.len && depth_br) && md_work_step()) {
                if (slice.data[j] == '\\' && j + 1 < slice.len) { j += 2; continue; }
                if (slice.data[j] == '[') depth_br++;
                else if (slice.data[j] == ']') depth_br--;
                if (depth_br) j++;
            }
            if (j < slice.len && depth_br == 0) {
                FLUSH_TEXT();
                MdSlice body = slice_make(slice.data + i + 2, j - (i + 2));
                VEC_PUSH(out, MdInline);
                MdInline *r = &out->data[out->len++];
                memset(r, 0, sizeof *r);
                r->kind = MD_INLINE_FOOTNOTE_REF;
                r->text = slice_empty();   /* anonymous: numbered by position */
                r->url  = body;            /* the note itself */
                i = j + 1; text_start = i; continue;
            }
        }

        /* Superscript ^sup^ (no spaces in body). */
        if (c == '^') {
            u32 j = i + 1;
            while ((j < slice.len && slice.data[j] != '^' && slice.data[j] != ' ' &&
                   slice.data[j] != '\t' && slice.data[j] != '\n') && md_work_step()) j++;
            if (j < slice.len && slice.data[j] == '^' && j > i + 1) {
                FLUSH_TEXT();
                push_styled_run(out, MD_INLINE_SUPERSCRIPT,
                                slice_make(slice.data + i + 1, j - (i + 1)));
                i = j + 1; text_start = i; continue;
            }
        }

        /* Highlight ==…==. Both markers hug the text, as emphasis markers do:
         * an "==" with a space after it is a comparison, not an opener. */
        if (c == '=' && i + 1 < slice.len && slice.data[i + 1] == '=') {
            bool opens = i + 2 < slice.len && !is_inline_space(slice.data[i + 2]);
            i64 close = opens ? find_delim(slice, i + 2, '=', 2) : -1;
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
                while ((j < slice.len && slice.data[j] != '\n') && md_work_step()) {
                    if (slice.data[j] == '\\' && j + 1 < slice.len) { j += 2; continue; }
                    if (slice.data[j] == '$') {
                        u8 prev  = slice.data[j - 1];
                        u8 after = (j + 1 < slice.len) ? slice.data[j + 1] : 0;
                        if (prev != ' ' && prev != '\t' && !is_word_byte(after)) {
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
            while ((i + run < slice.len && slice.data[i + run] == c) && md_work_step()) run++;
            bool can_open, can_close;
            emph_flanks(i ? slice.data[i - 1] : ' ',
                        i + run < slice.len ? slice.data[i + run] : ' ',
                        c, &can_open, &can_close);
            (void)can_close;
            if (can_open && run >= 1) {
                u32 want = run >= 3 ? 3 : (run >= 2 ? 2 : 1);
                i64 close = find_delim(slice, i + run, c, want);
                if (close >= 0) {
                    u32 close_start = (u32)close;
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
            i += run;
            continue;
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
                while ((j < slice.len && is_tag_cont(slice.data[j])) && md_work_step()) j++;
                /* Purely-numeric tags (#123) are rejected so dates / "#1"
                 * stay literal text. Require at least one non-digit. */
                bool numeric_only = true;
                for (u32 k = i + 1;( k < j) && md_work_step(); k++) {
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

/* ---- link reference definitions ----
 * "[label]: https://example.com" is a DEFINITION, not prose: it never renders,
 * and it supplies the destination for every "[text][label]" in the document —
 * including ones written ABOVE it. That forward reference is why this is a
 * document-scope table filled during the block walk and consumed by a pass
 * afterwards, rather than something the inline tokenizer could resolve on the
 * spot. Same shape as the footnote table below it, for the same reason. */
#define MD_MAX_LINK_DEFS 256

typedef struct {
    MdSlice label[MD_MAX_LINK_DEFS];   /* without the brackets, trimmed */
    MdSlice dest[MD_MAX_LINK_DEFS];
    u32     count;
} MdLinkDefs;

typedef struct {
    BlockVec blocks;
    Arena   *arena;
    int      depth;     /* callout/quote nesting depth (0 at top level) */
    /* Borrowed from the top-level parse and threaded through every nested
     * builder, so a definition written inside a callout still resolves a
     * reference in the body text and vice versa. */
    MdLinkDefs *defs;
    /* The document's own byte span. A task checkbox is only editable when its
     * bytes provably live here — see MD_TASK_NO_SRC in md_ast.h. Threaded
     * rather than inferred from depth, because a <details> body IS a slice of
     * the source while a callout body is an arena copy, and both recurse. */
    const u8 *doc_src;
    usize     doc_len;
    /* Set once per source line, applied by bb_emit to whatever block that line
     * produces: when the line is indented into an open list item's content
     * column, every block it yields continues that item. Doing it here rather
     * than in each of the dozen block branches is what makes fenced code,
     * quotes and tables inside a list item work as well as paragraphs. */
    u8   cont_depth;
    u8   cont_ordered;
    bool cont_active;
} BlockBuilder;

/* Case-insensitive, whitespace-collapsed label match (CommonMark normalizes
 * labels this way, so "[Foo Bar]" and "[foo   bar]" are the same link). */
static bool linkdef_label_eq(MdSlice a, MdSlice b) {
    u32 i = 0, j = 0;
    for (;md_work_step();) {
        while ((i < a.len && (a.data[i] == ' ' || a.data[i] == '\t' || a.data[i] == '\n')) && md_work_step()) i++;
        while ((j < b.len && (b.data[j] == ' ' || b.data[j] == '\t' || b.data[j] == '\n')) && md_work_step()) j++;
        bool ai = i < a.len, bj = j < b.len;
        if (!ai || !bj) return ai == bj;
        u8 ca = a.data[i], cb = b.data[j];
        if (ca >= 'A' && ca <= 'Z') ca = (u8)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (u8)(cb + 32);
        /* Compare one run of non-space so a space run collapses to one break. */
        if (ca != cb) return false;
        i++; j++;
    }
    return false;
}

static void linkdef_add(MdLinkDefs *d, MdSlice label, MdSlice dest) {
    if (!d || d->count >= MD_MAX_LINK_DEFS || label.len == 0) return;
    for (u32 i = 0;( i < d->count) && md_work_step(); i++)
        if (linkdef_label_eq(d->label[i], label)) return;   /* first wins */
    d->label[d->count] = label;
    d->dest[d->count]  = dest;
    d->count++;
}

static MdSlice linkdef_lookup(const MdLinkDefs *d, MdSlice label) {
    if (!d) return slice_empty();
    for (u32 i = 0;( i < d->count) && md_work_step(); i++)
        if (linkdef_label_eq(d->label[i], label)) return d->dest[i];
    return slice_empty();
}

/* Match a whole line as "[label]: destination [optional title]".
 * The title is parsed only to confirm the line is a definition — it is not
 * kept, because nothing in the reader displays link titles. */
static bool parse_link_def(MdSlice body, MdSlice *out_label, MdSlice *out_dest) {
    if (body.len < 4 || body.data[0] != '[') return false;
    u32 i = 1;
    while ((i < body.len && body.data[i] != ']') && md_work_step()) {
        if (body.data[i] == '\\' && i + 1 < body.len) i += 2;
        else if (body.data[i] == '[') return false;     /* nested: not a label */
        else i++;
    }
    if (i >= body.len || i == 1) return false;          /* unclosed or empty */
    MdSlice label = slice_make(body.data + 1, i - 1);
    /* "[^id]: text" is a FOOTNOTE definition, not a link definition — the two
     * spellings differ by one character and the footnote pass owns that one.
     * Claiming it here silently deleted every footnote whose body happened to
     * be a single word (a link destination is "the next run of non-space", so
     * "[^n]: Once." parsed as label "^n" pointing at "Once."). */
    if (label.data[0] == '^') return false;
    i++;
    if (i >= body.len || body.data[i] != ':') return false;
    i++;
    while (i < body.len && is_inline_space(body.data[i])) i++;
    if (i >= body.len) return false;
    MdSlice dest;
    if (body.data[i] == '<') {
        u32 start = ++i;
        while (i < body.len && body.data[i] != '>' && body.data[i] != '\n') {
            if (body.data[i] == '\\' && i + 1 < body.len) i++;
            i++;
        }
        if (i >= body.len || body.data[i] != '>') return false;
        dest = slice_make(body.data + start, i - start);
        i++;
    } else {
        u32 start = i, nesting = 0;
        while (i < body.len && !is_inline_space(body.data[i])) {
            u8 c = body.data[i];
            if (c == '\\' && i + 1 < body.len) { i += 2; continue; }
            if (c == '(') nesting++;
            if (c == ')') { if (!nesting) return false; nesting--; }
            i++;
        }
        if (nesting || start == i) return false;
        dest = slice_make(body.data + start, i - start);
    }
    while (i < body.len && is_inline_space(body.data[i])) i++;
    if (i < body.len) {
        u8 end = body.data[i++];
        if (end != '"' && end != '\'' && end != '(') return false;
        if (end == '(') end = ')';
        while (i < body.len && body.data[i] != end) {
            if (body.data[i] == '\\' && i + 1 < body.len) i++;
            i++;
        }
        if (i == body.len) return false;
        i++;
        while (i < body.len && is_inline_space(body.data[i])) i++;
        if (i != body.len) return false;
    }
    *out_label = label;
    *out_dest  = dest;
    return true;
}

static MdBlock *bb_emit(BlockBuilder *bb) {
    if (!VEC_RESERVE(&bb->blocks, MdBlock)) return NULL;
    MdBlock *b = &bb->blocks.data[bb->blocks.len++];
    memset(b, 0, sizeof *b);
    if (bb->cont_active) {
        b->list_cont    = 1;
        b->list_depth   = bb->cont_depth;
        b->list_ordered = bb->cont_ordered;
    }
    return b;
}

/* Single-paragraph extension: append `line` to existing paragraph slice
 * (which is contiguous in source). */
static void paragraph_extend(MdBlock *p, MdSlice line) {
    if (!p->raw.data) {
        p->raw = line;
        return;
    }
    /* slice must be physically adjacent (source is walked linearly) */
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

/* ---- lazy continuation ----
 * CommonMark lets a quote's paragraph run on without its marker:
 *
 *     > a quoted sentence that
 *     wraps without one
 *
 * is ONE quote, not a quote followed by a paragraph of its own. Two questions
 * decide it, and both are asked of text with the quote markers already off. */

/* Would this line open a block of its own even directly under paragraph text?
 * Such a line is never a continuation — it ends the quote. Deliberately the
 * wide reading (every list marker, anything that looks like a tag): calling a
 * real continuation a new block costs one paragraph break, calling a new
 * block a continuation swallows it into the quote. */
static bool line_opens_block(MdSlice body) {
    if (body.len == 0) return true;
    u8 c = body.data[0];
    u8 lvl = 0;
    u32 ts = 0;
    if (c == '>' || c == '|' || is_hr(body) || is_atx_heading(body, &lvl, &ts)) return true;
    if (body.len >= 3 && (c == '`' || c == '~') && body.data[1] == c && body.data[2] == c)
        return true;
    if (body.len >= 2 && (c == '$' || c == '%') && body.data[1] == c) return true;
    if (c == '<' && body.len >= 2 &&
        (body.data[1] == '!' || body.data[1] == '/' ||
         (body.data[1] >= 'a' && body.data[1] <= 'z') ||
         (body.data[1] >= 'A' && body.data[1] <= 'Z'))) return true;
    if (c == ':' && body.len >= 2 && body.data[1] == ' ') return true;
    if (c == '[' && body.len >= 2 && body.data[1] == '^') return true;
    /* a setext underline would turn the paragraph above it into a heading */
    if (c == '=' || c == '-') {
        u32 k = 0;
        while ((k < body.len && body.data[k] == c) && md_work_step()) k++;
        while ((k < body.len && (body.data[k] == ' ' || body.data[k] == '\t')) && md_work_step()) k++;
        if (k == body.len) return true;
    }
    return list_marker(body, NULL, NULL) != 0;
}

/* What the collector remembers from one quoted line to the next. */
typedef struct {
    bool para_open;      /* the last quoted line left paragraph text open */
    u8   fence_marker;   /* '`' / '~' while a quoted fence is open, else 0 */
    u32  fence_run;
    bool in_table;       /* inside a quoted table's rows */
    bool prev_pipe;      /* the previous quoted line had a '|' */
} QuoteLazy;

/* Note one quoted line (markers off) and work out whether it leaves a
 * paragraph open — the only state an unmarked line may continue. */
static void quote_lazy_note(QuoteLazy *q, MdSlice content) {
    /* A nested quote's own markers come off first: "> > text" leaves the
     * inner quote's paragraph open. */
    u32 nb = 0;
    u32 ind = leading_indent(content, &nb);
    while ((nb < content.len && content.data[nb] == '>') && md_work_step()) {
        nb++;
        if (nb < content.len && (content.data[nb] == ' ' || content.data[nb] == '\t')) nb++;
        content = slice_make(content.data + nb, content.len - nb);
        ind = leading_indent(content, &nb);
    }
    MdSlice body = slice_make(content.data + nb, content.len - nb);
    bool pipe = body.len && memchr(body.data, '|', body.len) != NULL;
    bool was_pipe = q->prev_pipe;
    q->prev_pipe = pipe;

    if (q->fence_marker) {                       /* inside a quoted fence */
        u32 r = 0;
        while ((r < body.len && body.data[r] == q->fence_marker) && md_work_step()) r++;
        u32 rest = r;
        while ((rest < body.len && (body.data[rest] == ' ' || body.data[rest] == '\t')) && md_work_step()) rest++;
        if (r >= q->fence_run && rest == body.len) q->fence_marker = 0;
        q->para_open = false;
        return;
    }
    if (body.len == 0) { q->para_open = false; q->in_table = false; return; }
    if (body.len >= 3 && (body.data[0] == '`' || body.data[0] == '~') &&
        body.data[1] == body.data[0] && body.data[2] == body.data[0]) {
        u32 r = 0;
        while ((r < body.len && body.data[r] == body.data[0]) && md_work_step()) r++;
        q->fence_marker = body.data[0];
        q->fence_run    = r;
        q->para_open    = false;
        return;
    }
    /* A table's rows are not paragraph text. The delimiter row under a line
     * with a pipe is what says a table began; it lasts while the pipes do. */
    u16 cols = 0;
    char aligns[MD_TABLE_MAX_COLS];
    if (pipe && was_pipe && is_table_separator(body, &cols, aligns)) q->in_table = true;
    else if (!pipe) q->in_table = false;
    if (q->in_table) { q->para_open = false; return; }
    /* Indented four columns: more of the paragraph if one is open, otherwise
     * code. Either way the state stands. */
    if (ind >= MD_INDENT_CODE) return;
    /* A list item's text is a paragraph a later line may continue; every
     * other block start (heading, rule, HTML, …) leaves nothing open. */
    if (line_opens_block(body))
        q->para_open = !is_hr(body) && list_marker(body, NULL, NULL) != 0;
    else
        q->para_open = true;
}

/* Collect the body lines of a callout/quote into a fresh arena buffer with the
 * leading '>' (+ one space) stripped from each line and the lines re-joined by
 * '\n'. This yields a clean markdown fragment (no embedded continuation-line
 * '>' markers) that can be recursively re-parsed into child blocks and rendered
 * without stray quote markers. Stops at the first line that is neither marked
 * nor a lazy continuation of the paragraph the marked lines left open. */
static MdSlice scanner_collect_quote_clean(LineScanner *s, Arena *arena) {
    u8 *buf = NULL; usize blen = 0, bcap = 0;
    bool first = true;
    QuoteLazy lazy = {0};
    while ((s->pos < s->len) && md_work_step()) {
        usize save = s->pos;
        MdSlice line;
        if (!scan_line(s, &line)) break;
        u32 ind = leading_spaces(line);
        u32 q;
        if (ind < line.len && line.data[ind] == '>') {
            q = ind + 1;
            if (q < line.len && (line.data[q] == ' ' || line.data[q] == '\t')) q++;
            quote_lazy_note(&lazy, slice_make(line.data + q, line.len - q));
        } else {
            /* Unmarked. It joins the quote only as the next line of an open
             * paragraph, and it goes in without its indentation so the
             * re-parse below cannot take it for anything but that. */
            u32 nb = 0;
            (void)leading_indent(line, &nb);
            if (!lazy.para_open ||
                line_opens_block(slice_make(line.data + nb, line.len - nb))) {
                s->pos = save;
                break;
            }
            q = nb;
        }
        u32 clen = line.len - q;
        usize need;
        if (!checked_add_usize(blen, first ? 0u : 1u, &need) ||
            !checked_add_usize(need, (usize)clen, &need) ||
            need > UINT32_MAX) { free(buf); return slice_make(NULL, 0); }
        if (need > bcap) {
            usize new_cap, doubled;
            if (!checked_mul_usize(need, 2u, &doubled) ||
                !checked_add_usize(doubled, 64u, &new_cap)) new_cap = need;
            u8 *nb = realloc(buf, new_cap);
            if (!nb) { free(buf); return slice_make(NULL, 0); }
            buf = nb;
            bcap = new_cap;
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
    while ((i < body.len) && md_work_step()) {
        if (at_line_start) {
            while ((i < body.len && body.data[i] == '>') && md_work_step()) {
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
/* Tokenize `src` into an arena-owned inline run. Always writes both outputs
 * (NULL/0 on empty input or allocation failure). */
static void tokenize_run(Arena *arena, MdSlice src,
                         MdInline **out_runs, u32 *out_count) {
    *out_runs  = NULL;
    *out_count = 0;
    if (src.len == 0) return;
    InlineVec v = {0};
    inline_tokenize(src, &v, arena, 0);
    if (v.len == 0) { free(v.data); return; }
    MdInline *out = arena_alloc_array(arena, (usize)v.len, sizeof(MdInline));
    if (out) {
        memcpy(out, v.data, (usize)v.len * sizeof(MdInline));
        *out_runs  = out;
        *out_count = v.len;
    }
    free(v.data);
}

static void finalize_inline_run(Arena *arena, MdBlock *b, MdSlice src) {
    tokenize_run(arena, src, &b->inlines, &b->inline_count);
}

/* Parse a markdown body slice into a separate sub-vec of blocks. Used for
 * callout/quote children so nested headings, lists, and paragraphs render
 * properly inside callouts. */
static void block_parse_into(BlockBuilder *bb, const u8 *src, usize len);

/* The sizing and copying passes must use the same tab/column rules. A single
 * tab may leave three spaces after dedent, so source length is not a capacity
 * bound. Check each padding/copy against cap before advancing the output. */
static bool code_dedent_pass(const u8 *src, u32 len, u32 strip,
                            u8 *out, u32 cap, u32 *written) {
    u32 o = 0, i = 0;
    while (i < len) {
        if (!md_work_step()) return false;
        u64 col = 0;
        while (i < len && col < strip) {
            if (!md_work_step()) return false;
            if (src[i] == ' ')       col += 1;
            else if (src[i] == '\t') col += 4 - (col % 4);
            else break;
            i++;
        }
        u32 padding = col > strip ? (u32)(col - strip) : 0;
        if (padding > cap - o) return md_work_refuse();
        if (out && padding) memset(out + o, ' ', padding);
        o += padding;

        u32 start = i;
        while (i < len && src[i] != '\n') {
            if (!md_work_step()) return false;
            i++;
        }
        if (i < len) i++;                         /* keep the newline */
        u32 bytes = i - start;
        if (bytes > cap - o) return md_work_refuse();
        if (out && bytes) memcpy(out + o, src + start, bytes);
        o += bytes;
    }
    *written = o;
    return true;
}

/* Strip exactly `strip` columns of leading whitespace from every line of an
 * indented code block, expanding tabs so a tab-indented block dedents by the
 * same visual amount a space-indented one does.
 *
 * This is the one place the parser cannot hand back a slice of the source: the
 * dedent removes bytes from the middle of the span, so the result is built in
 * the arena. Everything BEYOND the strip column is copied verbatim — the code's
 * own relative indentation is content, not syntax. */
static MdSlice code_dedent(Arena *arena, const u8 *src, u32 len, u32 strip,
                           bool trim_tail) {
    if (!arena || len == 0) return slice_empty();
    u32 needed = 0;
    if (!code_dedent_pass(src, len, strip, NULL, UINT32_MAX, &needed))
        return slice_empty();
    usize capacity;
    if (!checked_add_usize((usize)needed, 1u, &capacity)) {
        md_work_refuse();
        return slice_empty();
    }
    u8 *out = (u8 *)arena_alloc(arena, capacity);
    if (!out) return slice_make(src, len);
    u32 o = 0;
    if (!code_dedent_pass(src, len, strip, out, needed, &o))
        return slice_empty();
    /* Trailing blank lines are trailing whitespace, not code — in an INDENTED
     * block, which ends wherever its indentation does. A fence says where it
     * ends, so what stands before its closer is content and is kept. */
    while ((trim_tail && o > 0 && (out[o-1] == '\n' || out[o-1] == '\r' ||
                     out[o-1] == ' '  || out[o-1] == '\t')) && md_work_step()) o--;
    return slice_make(out, o);
}

/* Body of a fenced block whose opening fence sat `indent` columns in.
 *
 * A fence under a list item is indented to the item's text column, and so is
 * every line of its body: that indentation belongs to the LIST, not to the
 * code. CommonMark removes it — the container's indent and up to the fence's
 * own, from each line that has it — so a heredoc copied out of a list item
 * keeps a terminator that still ends the heredoc. An unindented fence stays
 * a zero-copy slice of the source. */
static MdSlice fence_body(Arena *arena, const BodyAccum *acc, u32 indent) {
    MdSlice raw = body_slice(acc);
    if (indent == 0 || raw.len == 0) return raw;
    return code_dedent(arena, raw.data, raw.len, indent, false);
}

static MdBlock *finalize_children(Arena *arena, MdBlock *parent, BlockVec *vec) {
    if (vec->len == 0) {
        parent->children = NULL;
        parent->child_count = 0;
        free(vec->data);
        return NULL;
    }
    MdBlock *out = arena_alloc_array(arena, (usize)vec->len, sizeof(MdBlock));
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
/* Is `s` a bare block identifier — "^abc-123"? Obsidian's rule is letters,
 * digits and hyphens after the caret. */
static bool slice_is_block_id(MdSlice s) {
    if (s.len < 2 || s.data[0] != '^') return false;
    for (u32 i = 1;( i < s.len) && md_work_step(); i++) {
        u8 c = s.data[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

/* Split a trailing " ^block-id" off a block's text.
 *
 * The id is a link TARGET, not prose — Obsidian hides it in reading view, and
 * leaving it in printed a stray "^blok-1" at the end of the paragraph. */
static MdSlice strip_block_id(MdSlice body, MdSlice *out_id) {
    u32 end = body.len;
    while ((end > 0 && (body.data[end-1] == ' ' || body.data[end-1] == '\t')) && md_work_step()) end--;
    u32 start = end;
    while ((start > 0) && md_work_step()) {
        u8 c = body.data[start-1];
        if (c == ' ' || c == '\t') break;
        start--;
    }
    if (start == 0 && end == body.len) {
        /* The whole block is the id (the form used after a list or a quote). */
        MdSlice whole = slice_make(body.data, end);
        if (slice_is_block_id(whole)) {
            if (out_id) *out_id = slice_make(whole.data + 1, whole.len - 1);
            return slice_empty();
        }
        return body;
    }
    MdSlice tail = slice_make(body.data + start, end - start);
    if (!slice_is_block_id(tail)) return body;
    if (out_id) *out_id = slice_make(tail.data + 1, tail.len - 1);
    while ((start > 0 && (body.data[start-1] == ' ' || body.data[start-1] == '\t')) && md_work_step()) start--;
    return slice_make(body.data, start);
}

static void finalize_block_tree(Arena *arena, MdBlock *blocks, u32 count) {
    for (u32 i = 0;( i < count) && md_work_step(); i++) {
        MdBlock *b = &blocks[i];
        switch (b->kind) {
        case MD_BLOCK_PARAGRAPH:
        case MD_BLOCK_LIST_ITEM:
        case MD_BLOCK_TASK_ITEM: {
            MdSlice id = slice_empty();
            MdSlice body = strip_block_id(slice_trim(b->raw), &id);
            if (id.len) {
                b->block_id = id;
                b->raw = body;
                /* A line that was ONLY an id belongs to the block above it. */
                if (body.len == 0 && i > 0) blocks[i-1].block_id = id;
            }
            break;
        }
        default: break;
        }
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
        /* Header labels are markdown too. */
        if ((b->kind == MD_BLOCK_CALLOUT || b->kind == MD_BLOCK_DETAILS) &&
            b->info.len)
            tokenize_run(arena, slice_trim(b->info),
                         &b->title_inlines, &b->title_inline_count);
        if (b->child_count)
            finalize_block_tree(arena, b->children, b->child_count);
    }
}

/* Table parsing. Header line + separator + rows (each '|'-bounded). */

/* One row split into cell slices. */
typedef struct { MdSlice s[MD_TABLE_MAX_COLS]; u16 n; } MdTableRow;

static void table_split_row(MdSlice line, MdTableRow *r) {
    u32 i = 0, end = line.len;
    while ((i < end && (line.data[i] == ' ' || line.data[i] == '\t')) && md_work_step()) i++;
    if (i < end && line.data[i] == '|') i++;
    while ((end > i && (line.data[end-1] == ' ' || line.data[end-1] == '\t' ||
                        line.data[end-1] == '\r')) && md_work_step()) end--;
    if (end > i && line.data[end-1] == '|') end--;
    r->n = 0;
    while ((i < end && r->n < MD_TABLE_MAX_COLS) && md_work_step()) {
        u32 cs = i;
        while ((i < end && line.data[i] != '|') && md_work_step()) {
            if (line.data[i] == '\\' && i + 1 < end) i++;
            i++;
        }
        u32 ce = i;
        while ((cs < ce && (line.data[cs] == ' ' || line.data[cs] == '\t')) && md_work_step()) cs++;
        while ((ce > cs && (line.data[ce-1] == ' ' || line.data[ce-1] == '\t')) && md_work_step()) ce--;
        r->s[r->n++] = slice_make(line.data + cs, ce - cs);
        if (i < end && line.data[i] == '|') i++;
    }
}

static void table_unescape_code(Arena *arena, MdInline *runs, u32 count) {
    for (u32 i = 0; i < count; i++) {
        MdInline *r = &runs[i];
        if (r->children) table_unescape_code(arena, r->children, r->child_count);
        if (r->kind != MD_INLINE_CODE || !memchr(r->text.data, '\\', r->text.len)) continue;
        u8 *buf = arena_alloc(arena, r->text.len);
        if (!buf) continue;
        u32 n = 0;
        for (u32 j = 0; j < r->text.len; j++) {
            if (r->text.data[j] == '\\' && j + 1 < r->text.len && r->text.data[j + 1] == '|') j++;
            buf[n++] = r->text.data[j];
        }
        r->text = slice_make(buf, n);
    }
}

/* Emit one TABLE block from `row_count` collected rows (row 0 is the header)
 * and tokenize its cells. False only when no block could be emitted at all. */
static bool table_emit(BlockBuilder *bb, const MdTableRow *rows, u16 row_count,
                       u16 cols, const char *aligns) {
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
    usize ncells;
    if (!checked_mul_usize((usize)row_count, (usize)cols, &ncells)) return true;
    MdTableCell *cells = arena_alloc_array(bb->arena, ncells, sizeof(MdTableCell));
    if (!cells) return true;
    memset(cells, 0, ncells * sizeof(MdTableCell));
    b->cells = cells;
    for (u16 r = 0;( r < row_count) && md_work_step(); r++) {
        for (u16 col = 0;( col < cols) && md_work_step(); col++) {
            MdSlice cs = (col < rows[r].n) ? rows[r].s[col] : slice_empty();
            InlineVec v = {0};
            inline_tokenize(cs, &v, bb->arena, 0);
            table_unescape_code(bb->arena, v.data, v.len);
            MdTableCell *cell = &cells[(usize)r * cols + col];
            if (v.len) {
                MdInline *out = arena_alloc_array(bb->arena, (usize)v.len, sizeof(MdInline));
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

static bool parse_table_at(BlockBuilder *bb, LineScanner *s, MdSlice header) {
    /* peek next line to confirm separator */
    usize save = s->pos;
    MdSlice sep_line;
    if (!scan_line(s, &sep_line)) { s->pos = save; return false; }
    char aligns[MD_TABLE_MAX_COLS] = {0};
    u16 cols = 0;
    if (!is_table_separator(sep_line, &cols, aligns)) { s->pos = save; return false; }
    /* GFM: the delimiter row is a row — it has a pipe — and it has as many
     * cells as the header. Without the first test "a | b" over "---" would
     * be a one-column table whose header lost its "b" (it is a setext
     * heading); without the second a header would be cut to however many
     * columns the line under it happened to describe. */
    if (!memchr(sep_line.data, '|', sep_line.len)) { s->pos = save; return false; }
    MdTableRow hr;
    table_split_row(header, &hr);
    if (hr.n != cols) { s->pos = save; return false; }

    /* Heap-allocated per call, NOT a shared static/BSS buffer: md_parse runs on
     * both the main thread AND the background graph-build worker
     * (fb_graph_build_worker → md_graph_build → md_parse), so a single static
     * rows[] buffer is a data race — worse, each row's slices point into that
     * thread's own source buffer, so a cross-thread read can dereference a freed
     * editor buffer. ~131 KB is also too large for a stack frame on the 512 KB
     * worker thread. parse_table_at is not hot (once per table), so malloc/free
     * is negligible. Every return past this point must free(rows). */
    MdTableRow *rows = (MdTableRow *)malloc(MD_TABLE_MAX_ROWS * sizeof(MdTableRow));
    if (!rows) { s->pos = save; return false; }

    /* One pass of this loop is one TABLE block. A table that outgrows
     * MD_TABLE_MAX_ROWS goes round again with the header row back in slot 0
     * (see the note on the caps above). */
    bool emitted = false;
    for (bool more = true; more && md_work_step();) {
        u16 row_count = 0;
        rows[row_count++] = hr;
        more = false;
        while ((s->pos < s->len) && md_work_step()) {
            usize rsave = s->pos;
            MdSlice line;
            if (!scan_line(s, &line)) break;
            if (line.len == 0 || !looks_like_table_row(line)) { s->pos = rsave; break; }
            MdTableRow r;
            table_split_row(line, &r);
            if (r.n == 0) { s->pos = rsave; break; }
            if (row_count == MD_TABLE_MAX_ROWS) { s->pos = rsave; more = true; break; }
            rows[row_count++] = r;
        }
        if (!table_emit(bb, rows, row_count, cols, aligns)) break;
        emitted = true;
    }
    free(rows);
    return emitted;
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
        while ((s->pos < s->len) && md_work_step()) {
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
    /* How many list items were open around the paragraph when it began. A
     * line that has dedented out of them is no longer INSIDE that paragraph's
     * container, which is what the "may this interrupt a paragraph" rule
     * turns on. */
    u32 open_para_depth = 0;
    MdListStack lists = {0};
    bool blank_before = true;      /* start-of-input counts as a fresh block */

    /* Item a lazy continuation line would extend, held as an INDEX rather than
     * a pointer: bb_emit reallocs the block vector, so a pointer kept across
     * iterations could dangle. Continuation is only legal while the item is
     * still the last block emitted, which `idx + 1 == len` checks for free —
     * anything emitted in between (a heading, a rule) ends the item. */
    u32 open_list_idx = UINT32_MAX;
    u32 first_line_indent = 0;

    while ((s->pos < s->len) && md_work_step()) {
        usize before = s->pos;
        MdSlice line;
        if (!scan_line(s, &line)) break;

        /* fence body collection */
        if (in_fence) {
            u32 ind_b = 0;
            u32 ind = leading_indent(line, &ind_b);
            if (ind <= fence_indent + 3 && ind_b < line.len &&
                line.data[ind_b] == fence_marker) {
                u32 r = 0;
                while ((ind_b + r < line.len && line.data[ind_b + r] == fence_marker) && md_work_step()) r++;
                /* A closing fence is the marker run followed only by spaces
                 * or tabs. A line such as ```js has trailing content, so it
                 * opens a nested code block rather than closing the current
                 * one. */
                u32 rest = ind_b + r;
                while ((rest < line.len && (line.data[rest] == ' ' || line.data[rest] == '\t')) && md_work_step()) rest++;
                if (r >= fence_run && rest == line.len) {
                    /* close */
                    MdBlock *b = bb_emit(bb);
                    if (b) {
                        b->kind = MD_BLOCK_CODE;
                        b->info = fence_lang;
                        b->raw  = fence_body(bb->arena, &fence_acc, fence_indent);
                    }
                    in_fence = false;
                    fence_acc = (BodyAccum){0};
                    fence_lang = slice_empty();
                    open_paragraph = NULL;
                    continue;
                }
            }
            /* keep raw (no leading-indent trim, preserves code formatting) */
            /* Preserve the exact original span, including a trailing newline
             * when present. The former `pos - 1` endpoint excluded the final
             * byte of an unterminated fenced block at EOF. */
            body_extend(&fence_acc, slice_make(line.data,
                                               (u32)((s->src + s->pos) - line.data)));
            (void)before;
            continue;
        }

        u32 ind_bytes = 0;
        u32 ind = leading_indent(line, &ind_bytes);
        MdSlice body = slice_make(line.data + ind_bytes, line.len - ind_bytes);
        ind += first_line_indent;
        first_line_indent = 0;

        /* Blank line — closes the paragraph. The list stack survives it: a
         * blank line between two items makes the list "loose", it does not end
         * it. Only a non-blank line at a shallower indent pops levels. */
        if (body.len == 0) {
            open_paragraph = NULL;
            blank_before = true;
            continue;
        }

        /* A non-blank line shallower than the innermost open item's content
         * ends those items — otherwise the next top-level paragraph would keep
         * inheriting the list's nesting depth. Runs before everything below so
         * the container column the rest of this iteration measures against is
         * the correct one. */
        while ((lists.depth > 0 && ind < lists.lv[lists.depth - 1].content_col) && md_work_step())
            lists.depth--;

        /* Does this line continue the innermost open list item? Anything it
         * produces — paragraph, fence, quote, table — belongs to that item.
         * A list MARKER at this indent opens a sibling or child instead, so
         * those two branches clear the flag on their own block. */
        bb->cont_active  = (lists.depth > 0 && ind >= list_stack_content_col(&lists));
        bb->cont_depth   = bb->cont_active ? (u8)(lists.depth - 1) : 0;
        /* Of the item the line continues — the innermost one still open at
         * this indent — not of the last item written: under "1. outer" with a
         * nested bullet, a block back at the outer item's column continues
         * the ORDERED item. */
        bb->cont_ordered = (bb->cont_active && lists.lv[lists.depth - 1].ordered) ? 1 : 0;

        /* Indented code block — 4+ columns past the enclosing container, and
         * only where a paragraph could not simply continue (CommonMark: an
         * indented line right after paragraph text is lazy continuation, not
         * code). Consumes every following line that stays indented or blank.
         *
         * `raw` keeps the source bytes with only the container indent removed,
         * so the code's own relative indentation survives verbatim. */
        {
            u32 code_col = list_stack_content_col(&lists) + MD_INDENT_CODE;
            if (ind >= code_col && !open_paragraph && !in_fence &&
                (blank_before || open_list_idx == UINT32_MAX || open_list_idx + 1 != bb->blocks.len)) {
                usize blk_start = (usize)(line.data - s->src);
                usize blk_end   = (usize)((line.data + line.len) - s->src);
                for (;md_work_step();) {
                    usize peek = s->pos;
                    MdSlice nl;
                    if (!scan_line(s, &nl)) break;
                    u32 nb = 0;
                    u32 nind = leading_indent(nl, &nb);
                    if (nb >= nl.len) continue;            /* blank: may be interior */
                    if (nind < code_col) { s->pos = peek; break; }
                    blk_end = (usize)((nl.data + nl.len) - s->src);
                }
                MdBlock *b = bb_emit(bb);
                if (b) {
                    b->kind = MD_BLOCK_CODE;
                    b->info = slice_empty();               /* no language */
                    b->raw  = code_dedent(bb->arena, s->src + blk_start,
                                          (u32)(blk_end - blk_start), code_col, true);
                }
                open_paragraph = NULL;
                blank_before = false;
                continue;
            }
        }


        /* Fenced code open */
        if (body.len >= 3 &&
            (body.data[0] == '`' || body.data[0] == '~') &&
            body.data[1] == body.data[0] && body.data[2] == body.data[0]) {
            u32 run = 0;
            while ((run < body.len && body.data[run] == body.data[0]) && md_work_step()) run++;
            if (body.data[0] == '`' && memchr(body.data + run, '`', body.len - run))
                goto not_a_fence;
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

not_a_fence:
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
            while ((s->pos < s->len) && md_work_step()) {
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

        /* Obsidian comment block — "%%" opening a line hides everything through
         * the closing "%%", across as many lines as it takes. The single-line
         * form "%% note %%" is handled by the inline tokenizer; this is the
         * multi-line one, which the inline pass never sees because the blank
         * lines inside it would have split the paragraph first.
         *
         * An UNCLOSED "%%" does not comment out the rest of the file: the
         * parser rewinds and renders the line as prose, so a typo cannot blank
         * the remainder of a note. Same policy the unterminated "$$" above
         * uses. */
        if (body.len >= 2 && body.data[0] == '%' && body.data[1] == '%') {
            bool same_line = false;
            for (u32 k = 2;( k + 1 < body.len) && md_work_step(); k++) {
                if (body.data[k] == '%' && body.data[k + 1] == '%') { same_line = true; break; }
            }
            if (same_line) {
                open_paragraph = NULL;
                blank_before = false;
                continue;
            }
            usize csave = s->pos;
            bool closed = false;
            while ((s->pos < s->len) && md_work_step()) {
                MdSlice cl;
                if (!scan_line(s, &cl)) break;
                for (u32 k = 0;( k + 1 < cl.len + 1 && k + 2 <= cl.len) && md_work_step(); k++) {
                    if (cl.data[k] == '%' && cl.data[k + 1] == '%') { closed = true; break; }
                }
                if (closed) break;
            }
            if (closed) {
                open_paragraph = NULL;
                blank_before = false;
                continue;
            }
            s->pos = csave;      /* unterminated: fall through as prose */
        }

        /* HTML comment block "<!-- … -->" spanning lines. The inline tokenizer
         * already hides the single-line form. */
        if (body.len >= 4 && body.data[0] == '<' && body.data[1] == '!' &&
            body.data[2] == '-' && body.data[3] == '-') {
            bool closed = false;
            for (u32 k = 4;( k + 3 <= body.len) && md_work_step(); k++) {
                if (body.data[k] == '-' && body.data[k+1] == '-' && body.data[k+2] == '>') {
                    closed = true; break;
                }
            }
            usize hsave = s->pos;
            while ((!closed && s->pos < s->len) && md_work_step()) {
                MdSlice cl;
                if (!scan_line(s, &cl)) break;
                for (u32 k = 0;( k + 3 <= cl.len) && md_work_step(); k++) {
                    if (cl.data[k] == '-' && cl.data[k+1] == '-' && cl.data[k+2] == '>') {
                        closed = true; break;
                    }
                }
            }
            if (closed) {
                open_paragraph = NULL;
                blank_before = false;
                continue;
            }
            s->pos = hsave;
        }

        /* <details><summary>…</summary> … </details>
         *
         * The one HTML block worth understanding structurally: it is the
         * standard way to fold a section in a note, and passing it through as
         * dim literal text (what the HTML fallback does) loses both the label
         * and the content. The body is parsed as markdown, which is what
         * Obsidian does inside it too. */
        if (body.len >= 9 && body.data[0] == '<' &&
            (body.data[1] == 'd' || body.data[1] == 'D') &&
            slice_eq_ci_cstr(slice_make(body.data, 8), "<details")) {
            /* Accept attributes, not just a bare "<details>": the `open`
             * attribute is the documented way to write a section that starts
             * expanded, and requiring byte 8 to be '>' made "<details open>"
             * fall through and render as literal HTML. */
            u32 tag_end = 8;
            while ((tag_end < body.len && body.data[tag_end] != '>') && md_work_step()) tag_end++;
            bool starts_open = false;
            for (u32 k = 8;( k + 4 <= tag_end) && md_work_step(); k++) {
                if (slice_eq_ci_cstr(slice_make(body.data + k, 4), "open")) {
                    starts_open = true; break;
                }
            }
            if (tag_end < body.len) {
                /* Collect through the closing tag. */
                usize dsave = s->pos;
                BodyAccum acc = {0};
                MdSlice rest = slice_make(body.data + tag_end + 1,
                                          body.len - tag_end - 1);
                bool closed = false;
                MdSlice cur = rest;
                for (;md_work_step();) {
                    i32 close_at = -1;
                    for (u32 k = 0;( k + 9 < cur.len + 1) && md_work_step(); k++) {
                        if (k + 10 <= cur.len &&
                            slice_eq_ci_cstr(slice_make(cur.data + k, 10), "</details>")) {
                            close_at = (i32)k; break;
                        }
                    }
                    if (close_at >= 0) {
                        body_extend(&acc, slice_make(cur.data, (u32)close_at));
                        closed = true;
                        break;
                    }
                    body_extend(&acc, cur);
                    if (!scan_line(s, &cur)) break;
                }
                if (!closed) { s->pos = dsave; }
                else {
                    MdSlice inner = body_slice(&acc);
                    /* <summary>…</summary> becomes the header label. Find it
                     * ANYWHERE in the leading whitespace rather than requiring
                     * byte 0: in the multi-line spelling the summary sits on
                     * the line BELOW the open tag, so `inner` starts with a
                     * newline. */
                    MdSlice summary = slice_empty();
                    u32 skip = 0;
                    {
                        u32 sp = 0;
                        while ((sp < inner.len && (inner.data[sp] == ' ' ||
                               inner.data[sp] == '\t' || inner.data[sp] == '\n' ||
                               inner.data[sp] == '\r')) && md_work_step()) sp++;
                        if (sp + 9 <= inner.len &&
                            slice_eq_ci_cstr(slice_make(inner.data + sp, 9), "<summary>")) {
                            for (u32 k = sp + 9;( k + 10 <= inner.len) && md_work_step(); k++) {
                                if (slice_eq_ci_cstr(slice_make(inner.data + k, 10),
                                                     "</summary>")) {
                                    summary = slice_trim(
                                        slice_make(inner.data + sp + 9, k - sp - 9));
                                    skip = k + 10;
                                    break;
                                }
                            }
                        }
                    }
                    MdSlice inner_body = slice_trim(
                        slice_make(inner.data + skip, inner.len - skip));
                    MdBlock *b2 = bb_emit(bb);
                    if (b2) {
                        b2->kind = MD_BLOCK_DETAILS;
                        b2->info = summary.len ? summary
                                               : slice_make((const u8 *)"Details", 7);
                        /* 1 = starts closed, 2 = starts open ("<details open>"). */
                        b2->callout_fold = starts_open ? 2 : 1;
                        b2->raw = inner_body;
                        if (inner_body.len && bb->depth < MD_MAX_NEST) {
                            BlockBuilder cbb = { .arena = bb->arena,
                                                 .depth = bb->depth + 1,
                                                 .defs  = bb->defs,
                                                 .doc_src = bb->doc_src,
                                                 .doc_len = bb->doc_len };
                            block_parse_into(&cbb, inner_body.data, inner_body.len);
                            finalize_children(bb->arena, b2, &cbb.blocks);
                        }
                    }
                    open_paragraph = NULL;
                    continue;
                }
            }
        }

        /* Raw HTML block.
         *
         * Liu has no HTML engine, so the block shows the author their own
         * source: it is recognized structurally (so it does not get mangled
         * into half-parsed markdown) and rendered as dimmed monospace by
         * render_html. Nothing is interpreted, no script runs, no resource is
         * fetched.
         *
         * Only a known BLOCK-level tag opens one. That restriction is what
         * keeps "<kbd>Esc</kbd> quits" a normal paragraph with a key-cap in it,
         * and "<https://example.com>" an autolink, instead of both vanishing
         * into a grey code dump. <details> was matched above and never lands
         * here. Runs to the next blank line, per CommonMark's block rule. */
        if (body.len >= 2 && body.data[0] == '<') {
            static const char *const HTML_BLOCK_TAGS[] = {
                "div", "p", "table", "thead", "tbody", "tr", "td", "th",
                "ul", "ol", "li", "dl", "dt", "dd", "section", "article",
                "header", "footer", "nav", "aside", "main", "figure",
                "figcaption", "blockquote", "pre", "form", "fieldset",
                "iframe", "script", "style", "center", "hr", "video", "audio",
                "canvas", "svg", "object", "embed", "h1", "h2", "h3", "h4",
                "h5", "h6",
            };
            u32 ts = (body.data[1] == '/') ? 2u : 1u;
            u32 te = ts;
            while ((te < body.len && ((body.data[te] >= 'a' && body.data[te] <= 'z') ||
                                     (body.data[te] >= 'A' && body.data[te] <= 'Z') ||
                                     (body.data[te] >= '0' && body.data[te] <= '9'))) && md_work_step()) te++;
            bool is_block_tag = false;
            if (te > ts && te < body.len &&
                (body.data[te] == '>' || body.data[te] == ' ' ||
                 body.data[te] == '/' || body.data[te] == '\t')) {
                MdSlice tag = slice_make(body.data + ts, te - ts);
                for (u32 k = 0;( k < sizeof HTML_BLOCK_TAGS / sizeof HTML_BLOCK_TAGS[0]) && md_work_step(); k++) {
                    if (slice_eq_ci_cstr(tag, HTML_BLOCK_TAGS[k])) { is_block_tag = true; break; }
                }
            }
            if (is_block_tag) {
                const u8 *hs = body.data;
                const u8 *he = body.data + body.len;
                while ((s->pos < s->len) && md_work_step()) {
                    usize peek = s->pos;
                    MdSlice hl;
                    if (!scan_line(s, &hl)) break;
                    u32 hb = 0;
                    (void)leading_indent(hl, &hb);
                    if (hb >= hl.len) { s->pos = peek; break; }   /* blank ends it */
                    he = hl.data + hl.len;
                }
                MdBlock *b = bb_emit(bb);
                if (b) {
                    b->kind = MD_BLOCK_HTML;
                    b->raw  = slice_make(hs, (u32)(he - hs));
                }
                open_paragraph = NULL;
                blank_before = false;
                continue;
            }
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
                while ((close < body.len && body.data[close] != ']') && md_work_step()) close++;
                if (close < body.len) {
                    MdSlice tag = slice_make(body.data + q + 2, close - (q + 2));
                    /* title is rest of the line (may be empty) */
                    u32 ts = close + 1;
                    u8 fold_marker = 0;
                    /* "[!type]-" starts folded, "[!type]+" starts open. */
                    if (ts < body.len && (body.data[ts] == '-' || body.data[ts] == '+'))
                        fold_marker = body.data[ts] == '-' ? 1 : 2;
                    while ((ts < body.len && (body.data[ts] == ' ' || body.data[ts] == '+' ||
                                             body.data[ts] == '-')) && md_work_step()) ts++;
                    MdSlice title = slice_make(body.data + ts, body.len - ts);
                    /* collect remaining quote lines into a clean fragment */
                    MdSlice body_clean = scanner_collect_quote_clean(s, bb->arena);

                    MdBlock *b = bb_emit(bb);
                    if (b) {
                        b->kind = MD_BLOCK_CALLOUT;
                        b->callout_kind = (u8)callout_lookup(tag);
                        b->callout_fold = fold_marker;
                        b->info = title;
                        b->raw  = body_clean;
                        /* Recursively parse the body into child blocks so nested
                         * lists / paragraphs / callouts render structurally. */
                        if (body_clean.len && bb->depth < MD_MAX_NEST) {
                            BlockBuilder cbb = { .arena = bb->arena,
                                                 .depth = bb->depth + 1,
                                                 .defs  = bb->defs,
                                                 .doc_src = bb->doc_src,
                                                 .doc_len = bb->doc_len };
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
                                         .depth = bb->depth + 1,
                                         .defs  = bb->defs,
                                         .doc_src = bb->doc_src,
                                         .doc_len = bb->doc_len };
                    block_parse_into(&cbb, body_clean.data, body_clean.len);
                    finalize_children(bb->arena, b, &cbb.blocks);
                } else if (body_clean.len) {
                    b->raw = strip_quote_prefixes(bb->arena, body_clean);
                }
            }
            open_paragraph = NULL;
            continue;
        }

        /* Setext underline, BEFORE the thematic-break test: with an open
         * paragraph above it, "---" underlines that paragraph into an H2 and
         * is not a rule at all (CommonMark gives setext the precedence). An
         * underline is one repeated marker with no interior spaces, which is
         * what keeps "- - -" a thematic break. */
        if (open_paragraph && body.len > 0 &&
            (body.data[0] == '=' || body.data[0] == '-')) {
            u8 m = body.data[0];
            u32 k = 0;
            while ((k < body.len && body.data[k] == m) && md_work_step()) k++;
            u32 tail = k;
            while ((tail < body.len && (body.data[tail] == ' ' || body.data[tail] == '\t')) && md_work_step())
                tail++;
            if (tail == body.len) {
                open_paragraph->kind = MD_BLOCK_HEADING;
                open_paragraph->heading_level = (m == '=') ? 1 : 2;
                open_paragraph = NULL;
                blank_before = false;
                continue;
            }
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
            /* Closing-style ATX ("## Title ##"): the trailing '#' run comes
             * off only when a space stands in front of it — or when it is all
             * there is. A '#' that ends a WORD is the word: "What's new in
             * C#" keeps its sharp. */
            text = slice_trim(text);
            u32 closer = text.len;
            while ((closer && text.data[closer - 1] == '#') && md_work_step()) closer--;
            if (closer == 0 || text.data[closer - 1] == ' ' || text.data[closer - 1] == '\t')
                text.len = closer;
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

        /* List item: unordered (dash, star, plus) or ordered "1." / "1)",
         * followed by 1..4 COLUMNS of space or tab; the text column that lands
         * on is what nested items measure themselves against. A tab counts to
         * the next stop of four, which is how "-<tab>item" is an item at all.
         *
         * A TASK is the same line whose text opens with a box: "- [ ] text",
         * "1. [x] text", and Obsidian's custom states ("- [/] doing"). The
         * state character is stored verbatim; only a space means unchecked. A
         * bare "- [x]" with no text is a legal empty task. */
        {
            bool ordered = false;
            u32 num = 0;
            u32 mark = list_marker(body, &ordered, &num);   /* 0 = not a list line */
            /* An ordered list may interrupt a paragraph only when it starts at
             * 1 (CommonMark), so a sentence that happens to wrap right before
             * "14." stays one paragraph. The paragraph has to be this line's
             * own container, though: "2." at the margin under item 1's indented
             * second paragraph is the next item, not more of that paragraph. */
            if (mark && ordered && num != 1 && open_paragraph &&
                lists.depth >= open_para_depth) mark = 0;
            if (mark) {
                u32 txt = mark;                    /* byte offset of the text */
                u32 col = ind + mark;              /* and its column */
                for (u32 gap = 0;( txt < body.len && gap < 4) && md_work_step(); txt++) {
                    u32 step;
                    if (body.data[txt] == ' ')       step = 1;
                    else if (body.data[txt] == '\t') step = 4 - (col % 4);
                    else break;
                    if (gap + step > 4) break;
                    gap += step;
                    col += step;
                }
                if (txt == mark) col++; /* an empty item still owns a content column */
                bool task = txt + 3 <= body.len &&
                            body.data[txt] == '[' && body.data[txt + 2] == ']' &&
                            body.data[txt + 1] != '[' && body.data[txt + 1] != ']' &&
                            (txt + 3 == body.len || body.data[txt + 3] == ' ' ||
                             body.data[txt + 3] == '\t');
                MdBlock *b = bb_emit(bb);
                if (b) {
                    b->kind = task ? MD_BLOCK_TASK_ITEM : MD_BLOCK_LIST_ITEM;
                    b->list_cont = 0;  /* a marker opens an item, not a continuation */
                    b->list_ordered = ordered ? 1 : 0;
                    /* The author's start number goes in; the renumbered
                     * display ordinal comes back out. */
                    u16 idx = ordered ? (u16)(num > UINT16_MAX ? UINT16_MAX : num) : 0;
                    bool same_list = (open_list_idx != UINT32_MAX &&
                                      open_list_idx + 1 == bb->blocks.len - 1);
                    /* Children nest against the text column for a task too —
                     * the checkbox is content, not marker. */
                    b->list_depth = list_stack_push(&lists, ind, col, same_list,
                                                    ordered ? &idx : NULL);
                    if (ordered) b->list_index = idx;
                    if (task) {
                        /* Store the marker char so custom states (x, /, -, …)
                         * render distinctly; 0 = unchecked. */
                        const u8 *sp = body.data + txt + 1;
                        b->task_state = (*sp == ' ') ? 0 : *sp;
                        /* Byte offset of the state char in the DOCUMENT. Only
                         * an offset that provably points into it is usable;
                         * anything else (a callout/quote body is an arena
                         * copy) would toggle an unrelated task. */
                        b->src_off = (bb->doc_src && sp >= bb->doc_src &&
                                      sp < bb->doc_src + bb->doc_len)
                                         ? (u32)(sp - bb->doc_src) : MD_TASK_NO_SRC;
                        u32 after = txt + 3;
                        if (after < body.len) after++;   /* the space after the box */
                        b->raw = slice_make(body.data + after, body.len - after);
                    } else {
                        b->raw = slice_make(body.data + txt, body.len - txt);
                    }
                    open_list_idx = bb->blocks.len - 1;
                    MdSlice content = b->raw;
                    if (!task && content.len && line_opens_block(content) &&
                        bb->depth + lists.depth < MD_MAX_NEST) {
                        b->raw = slice_empty();
                        s->pos = (usize)(content.data - s->src);
                        first_line_indent = col;
                    }
                }
                open_paragraph = NULL;
                blank_before = false;
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

        /* "[^id]: body" opens its own block. Otherwise consecutive definitions
         * would merge into one paragraph and, since a soft break renders as a
         * space, read as a single run-on line. An INDENTED line after one is
         * still a continuation, because it reaches the paragraph-extend path
         * below with this block left open. */
        if (body.len >= 4 && body.data[0] == '[' && body.data[1] == '^') {
            u32 k = 2;
            while ((k < body.len && body.data[k] != ']') && md_work_step()) k++;
            if (k + 1 < body.len && body.data[k + 1] == ':') open_paragraph = NULL;
        }

        /* Link reference definition — recorded, never rendered. CommonMark does
         * not let one interrupt a paragraph, so this only fires at the start of
         * a block; inside a paragraph the same bytes stay prose. */
        if (!open_paragraph && body.data[0] == '[') {
            MdSlice label, dest, best_label = {0}, best_dest = {0};
            MdSlice candidate = body;
            usize best_pos = 0;
            LineScanner peek = *s;
            for (u32 n = 0; n < 3; n++) {
                if (parse_link_def(candidate, &label, &dest)) {
                    best_label = label; best_dest = dest; best_pos = peek.pos;
                }
                MdSlice next;
                if (!scan_line(&peek, &next) || !slice_trim(next).len) break;
                candidate.len = (u32)(next.data + next.len - candidate.data);
            }
            if (best_pos) {
                linkdef_add(bb->defs, best_label, link_unescape(bb->arena, best_dest));
                s->pos = best_pos;
                blank_before = false;
                continue;
            }
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
                for (u32 k = 0;( k < body.len) && md_work_step(); k++) {
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

        /* Lazy continuation of a list item: a plain line that follows an item
         * without a blank line in between belongs to that item, however it is
         * indented. Without this a wrapped bullet became its own paragraph and
         * lost both the item's indent and its marker alignment. */
        if (!blank_before && !open_paragraph &&
            open_list_idx != UINT32_MAX && open_list_idx + 1 == bb->blocks.len) {
            paragraph_extend(&bb->blocks.data[open_list_idx], line);
            continue;
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
            open_para_depth = lists.depth;
        }
        blank_before = false;
    }

    /* unterminated fence: emit what was collected */
    if (in_fence) {
        MdBlock *b = bb_emit(bb);
        if (b) {
            b->kind = MD_BLOCK_CODE;
            b->info = fence_lang;
            b->raw  = fence_body(bb->arena, &fence_acc, fence_indent);
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

/* ---- link reference resolution pass ----
 * Runs once over the finished tree, after every definition in the document has
 * been seen. The tokenizer left one MD_LINK_REF_PENDING run per bracket group;
 * here each becomes a link or goes back to being the text it was written as,
 * so "[see below]" reads as prose instead of as a link to nowhere. */

/* A group this pass has found to be no link. It exists only between the two
 * halves of linkref_resolve_runs: every run carrying it is rewritten before
 * that function returns. */
#define MD_LINK_REF_LITERAL 0x80u

static bool linkref_is(const MdInline *r, u8 flag) {
    return (r->kind == MD_INLINE_LINK || r->kind == MD_INLINE_IMAGE) && r->flags == flag;
}

/* Does group `b` open exactly where group `a` closes — "[a][b]" with nothing
 * between? A run's text is the inside of its brackets, so the two slices sit
 * two bytes ("][") apart. */
static bool linkref_adjacent(const MdInline *a, const MdInline *b) {
    return a->text.data && b->text.data &&
           b->text.data == a->text.data + a->text.len + 2;
}

/* Is the group's content plain text — nothing in it the tokenizer made a run
 * of its own? Then the literal form is one slice of the source. */
static bool linkref_text_is_plain(const MdInline *r) {
    if (r->child_count == 0 || !r->children) return true;
    return r->child_count == 1 && r->children[0].kind == MD_INLINE_TEXT &&
           r->children[0].text.data == r->text.data &&
           r->children[0].text.len == r->text.len;
}

/* A group back to one TEXT run reading "[content]". The brackets are the
 * bytes on either side of the content in the source, so widening the slice by
 * one each way costs nothing and copies nothing. */
static void linkref_make_text(MdInline *r) {
    bool image = r->kind == MD_INLINE_IMAGE;
    r->kind  = MD_INLINE_TEXT;
    r->flags = 0;
    if (r->text.data) {
        r->text.data -= image ? 2 : 1;
        r->text.len  += image ? 3 : 2;
    }
    r->url         = slice_empty();
    r->children    = NULL;
    r->child_count = 0;
}

/* Resolve one run list in place. The list can change LENGTH — a label group
 * is spent by the link it names, and a literal group that holds markup is
 * replaced by "[", its children and "]" — so it is handed over through its
 * owner's pointer and count.
 *
 * The pairing is CommonMark's, read left to right:
 *   - a group followed at once by a group whose label is defined is a full
 *     reference (an EMPTY second group means "the label is my own text");
 *   - failing that, a group whose own text is a defined label is a shortcut
 *     reference, and the group after it is looked at on its own;
 *   - failing both it is text, so `matrix[i][j]` keeps both of its groups.
 */
static bool runs_have_link(const MdInline *runs, u32 count) {
    for (u32 i = 0; i < count; i++) {
        const MdInline *r = &runs[i];
        if (r->kind == MD_INLINE_LINK || r->kind == MD_INLINE_AUTOLINK) return true;
        if (r->kind != MD_INLINE_IMAGE && r->children && runs_have_link(r->children, r->child_count)) return true;
    }
    return false;
}

static void linkref_resolve_runs(Arena *arena, const MdLinkDefs *defs,
                                 MdInline **io_runs, u32 *io_n) {
    MdInline *runs = *io_runs;
    u32 n = *io_n;
    if (!runs || n == 0) return;

    u32 kept = 0, extra = 0;
    for (u32 i = 0;( i < n) && md_work_step(); i++) {
        MdInline r = runs[i];
        if (r.children && r.child_count)
            linkref_resolve_runs(arena, defs, &r.children, &r.child_count);
        bool nested_link = r.kind == MD_INLINE_LINK && runs_have_link(r.children, r.child_count);
        if (nested_link) {
            r.flags = MD_LINK_REF_LITERAL;
            extra += r.child_count + 1;
        } else if (linkref_is(&r, MD_LINK_REF_PENDING)) {
            MdSlice dest = slice_empty();
            if (r.text.len && i + 1 < n &&
                linkref_is(&runs[i + 1], MD_LINK_REF_PENDING) &&
                linkref_adjacent(&r, &runs[i + 1])) {
                MdSlice label = runs[i + 1].text.len ? runs[i + 1].text : r.text;
                dest = linkdef_lookup(defs, label);
                if (dest.len) i++;               /* the label group is spent */
            }
            if (!dest.len && r.text.len) dest = linkdef_lookup(defs, r.text);
            if (dest.len) {
                r.url   = dest;
                r.flags = 0;
            } else {
                r.flags = MD_LINK_REF_LITERAL;
                if (!linkref_text_is_plain(&r)) extra += r.child_count + 1;
            }
        }
        runs[kept++] = r;
    }
    n = kept;

    /* Second half: the literal groups. One that holds markup needs more runs
     * than the one it has, so the list is rebuilt in the arena. If the arena
     * refuses, every group falls back to its source text in one piece — the
     * marks show, nothing is lost, and Arena.refused already says the
     * document is incomplete. */
    MdInline *grown = extra ? arena_alloc_array(arena, (usize)n + extra, sizeof(MdInline))
                            : NULL;
    u32 out = 0;
    for (u32 i = 0; i < n; i++) {
        MdInline r = runs[i];
        if (!linkref_is(&r, MD_LINK_REF_LITERAL)) {
            if (grown) grown[out++] = r;
            continue;
        }
        if (!grown || linkref_text_is_plain(&r)) {
            linkref_make_text(&r);
            if (grown) grown[out++] = r; else runs[i] = r;
            continue;
        }
        MdInline bracket;
        memset(&bracket, 0, sizeof bracket);
        bracket.kind = MD_INLINE_TEXT;
        bracket.text = slice_make(r.text.data - 1, 1);
        grown[out++] = bracket;
        memcpy(&grown[out], r.children, (usize)r.child_count * sizeof(MdInline));
        out += r.child_count;
        bracket.text = r.alt.len ? r.alt : slice_make(r.text.data + r.text.len, 1);
        grown[out++] = bracket;
    }
    if (grown) { *io_runs = grown; *io_n = out; }
    else       { *io_n = n; }
}

static void linkref_resolve_blocks(Arena *arena, const MdLinkDefs *defs,
                                   MdBlock *blocks, u32 count) {
    for (u32 i = 0;( i < count) && md_work_step(); i++) {
        MdBlock *b = &blocks[i];
        linkref_resolve_runs(arena, defs, &b->inlines, &b->inline_count);
        /* A callout's or a <details>' label is markdown too, so its
         * references are resolved like the body's. */
        linkref_resolve_runs(arena, defs, &b->title_inlines, &b->title_inline_count);
        if (b->cells) {
            u32 nc = (u32)b->rows * (u32)b->cols;
            for (u32 c = 0;( c < nc) && md_work_step(); c++)
                linkref_resolve_runs(arena, defs, &b->cells[c].inlines,
                                     &b->cells[c].inline_count);
        }
        if (b->children && b->child_count)
            linkref_resolve_blocks(arena, defs, b->children, b->child_count);
    }
}

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
    /* An inline footnote `^[…]` has no id: every occurrence is its own note,
     * so it must NOT collide with the other anonymous ones on the empty id. */
    if (id.len == 0) {
        if (t->count < 255) { t->ids[t->count++] = id; return (u8)t->count; }
        return 255;
    }
    for (u32 i = 0;( i < t->count) && md_work_step(); i++)
        if (t->ids[i].len && fn_slice_eq(t->ids[i], id)) return (u8)(i + 1);
    if (t->count < 255) { t->ids[t->count++] = id; return (u8)t->count; }
    return 255;
}
static void fn_walk_inlines(FnTable *t, MdInline *runs, u32 n) {
    for (u32 i = 0;( i < n) && md_work_step(); i++) {
        if (runs[i].kind == MD_INLINE_FOOTNOTE_REF)
            runs[i].flags = fn_number(t, runs[i].text);
        if (runs[i].children && runs[i].child_count)
            fn_walk_inlines(t, runs[i].children, runs[i].child_count);
    }
}
static void fn_walk_blocks(FnTable *t, MdBlock *blocks, u32 count) {
    for (u32 i = 0;( i < count) && md_work_step(); i++) {
        MdBlock *b = &blocks[i];
        if (b->inlines && b->inline_count) fn_walk_inlines(t, b->inlines, b->inline_count);
        if (b->cells) {
            u32 nc = (u32)b->rows * (u32)b->cols;
            for (u32 c = 0;( c < nc) && md_work_step(); c++)
                if (b->cells[c].inlines && b->cells[c].inline_count)
                    fn_walk_inlines(t, b->cells[c].inlines, b->cells[c].inline_count);
        }
        if (b->children && b->child_count) fn_walk_blocks(t, b->children, b->child_count);
    }
}

/* Is this paragraph a footnote DEFINITION — "[^id]: body"? */
static bool block_is_footnote_def(const MdBlock *b) {
    return b && b->kind == MD_BLOCK_PARAGRAPH && b->inlines && b->inline_count >= 2 &&
           b->inlines[0].kind == MD_INLINE_FOOTNOTE_REF &&
           b->inlines[1].kind == MD_INLINE_TEXT &&
           b->inlines[1].text.len >= 1 && b->inlines[1].text.data[0] == ':';
}

/* Move every footnote definition to the end of the document, in the order the
 * REFERENCES appear, and give the inline `^[…]` notes a definition of their own.
 *
 * Reading view is not the source: a note's back matter belongs at the bottom
 * where it can be read as a block, not wherever the author happened to park it
 * mid-paragraph. This is also the only place an inline footnote's body can
 * surface at all. */
static void fn_reflow(Arena *arena, const MdLinkDefs *defs, BlockVec *blocks) {
    u32 n = blocks->len;
    if (n == 0) return;

    /* Body of each numbered inline footnote, discovered while walking refs. */
    MdSlice inline_bodies[256];
    memset(inline_bodies, 0, sizeof inline_bodies);
    bool any_inline = false;
    for (u32 i = 0;( i < n) && md_work_step(); i++) {
        MdBlock *b = &blocks->data[i];
        for (u32 k = 0;( k < b->inline_count) && md_work_step(); k++) {
            const MdInline *r = &b->inlines[k];
            if (r->kind != MD_INLINE_FOOTNOTE_REF) continue;
            if (r->text.len || !r->url.len) continue;
            if (r->flags == 0 || r->flags > 255) continue;
            inline_bodies[r->flags - 1] = r->url;
            any_inline = true;
        }
    }

    u32 def_count = 0;
    MdBlock *definitions[256] = {0};
    for (u32 i = 0;( i < n) && md_work_step(); i++) {
        MdBlock *b = &blocks->data[i];
        if (!block_is_footnote_def(b)) continue;
        def_count++;
        u32 num = b->inlines[0].flags;
        if (num && num < 256 && !definitions[num]) definitions[num] = b;
    }
    if (def_count == 0 && !any_inline) return;

    MdBlock *reordered = (MdBlock *)calloc(n + 258, sizeof(MdBlock));
    if (!reordered) return;
    u32 out = 0;
    for (u32 i = 0;( i < n) && md_work_step(); i++)
        if (!block_is_footnote_def(&blocks->data[i]))
            reordered[out++] = blocks->data[i];

    /* Separator, then the definitions in footnote-number order. */
    reordered[out].kind = MD_BLOCK_HR;
    out++;
    u32 emitted_defs = 0;
    for (u32 num = 1;( num <= 255) && md_work_step(); num++) {
        if (definitions[num]) {
            reordered[out++] = *definitions[num];
            emitted_defs++;
            continue;
        }
        MdSlice body = inline_bodies[num - 1];
        if (!body.len) continue;
        /* Synthesise "[^n]: body" so it renders identically to a written one. */
        MdBlock nb;
        memset(&nb, 0, sizeof nb);
        nb.kind = MD_BLOCK_PARAGRAPH;
        InlineVec v = {0};
        /* Use `VEC_RESERVE` directly because `VEC_PUSH` returns from this void
         * function on allocation failure without freeing `reordered`. The
         * temporary buffer is owned by `fn_reflow` and must be released on this
         * exit. */
        if (!VEC_RESERVE(&v, MdInline)) { free(reordered); return; }
        MdInline *ref = &v.data[v.len++];
        memset(ref, 0, sizeof *ref);
        ref->kind  = MD_INLINE_FOOTNOTE_REF;
        ref->flags = (u8)num;
        if (!VEC_RESERVE(&v, MdInline)) { free(v.data); free(reordered); return; }
        MdInline *colon = &v.data[v.len++];
        memset(colon, 0, sizeof *colon);
        colon->kind = MD_INLINE_TEXT;
        colon->text = slice_make((const u8 *)": ", 2);
        inline_tokenize(body, &v, arena, 0);
        if (v.len) {
            MdInline *arr = arena_alloc_array(arena, (usize)v.len, sizeof(MdInline));
            if (arr) {
                memcpy(arr, v.data, (usize)v.len * sizeof(MdInline));
                nb.inlines = arr;
                nb.inline_count = v.len;
                /* Tokenized after the document's resolve pass ran, so the
                 * references in a note's own text are resolved here. */
                linkref_resolve_runs(arena, defs, &nb.inlines, &nb.inline_count);
                reordered[out++] = nb;
                emitted_defs++;
            }
        }
        free(v.data);
    }

    /* Nothing but the separator? Then there was no back matter after all —
     * and the rule would be a divider the author never wrote. */
    if (emitted_defs == 0) { free(reordered); return; }
    if (out > blocks->cap) {
        MdBlock *grown = (MdBlock *)realloc(blocks->data, (usize)out * sizeof(MdBlock));
        if (!grown) { free(reordered); return; }
        blocks->data = grown;
        blocks->cap  = out;
    }
    memcpy(blocks->data, reordered, (usize)out * sizeof(MdBlock));
    blocks->len = out;
    free(reordered);
}

/* ---- frontmatter properties ----
 * A deliberately small YAML reader: enough to pull `title`, `tags` and
 * `aliases` out of Obsidian's property block, and nothing more. Anchors,
 * multi-line scalars, nested maps and typed values are NOT modelled — the raw
 * YAML stays in doc->frontmatter_yaml and the Properties panel renders it
 * verbatim, so nothing is lost by this view being partial. That is the whole
 * reason not to link a YAML library for three keys. */
static MdSlice fm_unquote(MdSlice s) {
    s = slice_trim(s);
    if (s.len >= 2 && (s.data[0] == '"' || s.data[0] == '\'') &&
        s.data[s.len - 1] == s.data[0]) {
        s.data += 1;
        s.len  -= 2;
    }
    return s;
}

static void fm_list_push(MdSlice *arr, u8 *n, MdSlice v) {
    v = fm_unquote(v);
    if (v.len == 0 || *n >= MD_MAX_FM_ITEMS) return;
    /* A leading '#' is how people write tags in the property block. */
    if (v.data[0] == '#' && v.len > 1) { v.data++; v.len--; }
    arr[*n] = v;
    (*n)++;
}

/* Split "a, b, c" (optionally already stripped of its [ ]) into list entries. */
static void fm_parse_flow(MdSlice s, MdSlice *arr, u8 *n) {
    s = slice_trim(s);
    if (s.len >= 2 && s.data[0] == '[' && s.data[s.len - 1] == ']') {
        s.data += 1; s.len -= 2;
    }
    u32 start = 0;
    for (u32 i = 0;( i <= s.len) && md_work_step(); i++) {
        if (i == s.len || s.data[i] == ',') {
            if (i > start) fm_list_push(arr, n, slice_make(s.data + start, i - start));
            start = i + 1;
        }
    }
}

static void fm_extract(MdDoc *doc) {
    MdSlice y = doc->frontmatter_yaml;
    if (!y.data || y.len == 0) return;
    LineScanner s = { y.data, y.len, 0 };
    /* Which list the following "  - item" lines belong to. */
    enum { FM_NONE, FM_TAGS, FM_ALIASES } open_list = FM_NONE;
    MdSlice line;
    while ((scan_line(&s, &line)) && md_work_step()) {
        u32 ind_b = 0;
        u32 ind = leading_indent(line, &ind_b);
        MdSlice body = slice_trim(slice_make(line.data + ind_b, line.len - ind_b));
        if (body.len == 0) continue;
        if (ind > 0 && body.data[0] == '-') {          /* list continuation */
            MdSlice v = slice_make(body.data + 1, body.len - 1);
            if (open_list == FM_TAGS)         fm_list_push(doc->fm_tags, &doc->fm_tag_count, v);
            else if (open_list == FM_ALIASES) fm_list_push(doc->fm_aliases, &doc->fm_alias_count, v);
            continue;
        }
        if (ind > 0) continue;                          /* nested map: not modelled */
        /* "key:" — the colon must end the key, so "time: 12:00" keeps its value. */
        u32 c = 0;
        while ((c < body.len && body.data[c] != ':') && md_work_step()) c++;
        if (c >= body.len) { open_list = FM_NONE; continue; }
        MdSlice key = slice_trim(slice_make(body.data, c));
        MdSlice val = slice_trim(slice_make(body.data + c + 1, body.len - c - 1));
        open_list = FM_NONE;
        if (slice_eq_ci_cstr(key, "title")) {
            doc->fm_title = fm_unquote(val);
        } else if (slice_eq_ci_cstr(key, "tags") || slice_eq_ci_cstr(key, "tag")) {
            if (val.len) fm_parse_flow(val, doc->fm_tags, &doc->fm_tag_count);
            else         open_list = FM_TAGS;
        } else if (slice_eq_ci_cstr(key, "aliases") || slice_eq_ci_cstr(key, "alias")) {
            if (val.len) fm_parse_flow(val, doc->fm_aliases, &doc->fm_alias_count);
            else         open_list = FM_ALIASES;
        }
    }
}

static MdDoc *parse_document(Arena *arena, const u8 *source, usize len,
                              const char *base_dir) {
    if (!arena || !arena->base) return NULL;

    MdDoc *doc = arena_alloc(arena, sizeof *doc);
    if (!doc) return NULL;
    memset(doc, 0, sizeof *doc);
    doc->arena = arena;
    doc->base_dir = base_dir;

    if (!source || len == 0) return doc;
    if (len > UINT32_MAX) return NULL;

    /* See MdDoc.incomplete: every refusal the arena records from here to the
     * end of the parse means something was dropped. */
    u32 refused_before = arena->refused;

    MdLinkDefs defs = {0};
    BlockBuilder bb = { .arena = arena, .defs = &defs,
                        .doc_src = source, .doc_len = len };
    block_parse_into(&bb, source, len);
    if (g_md_work && g_md_work->limited) goto parse_done;

    /* Inline-tokenize every block's raw slice, recursing into callout/quote
     * children. Skip CODE bodies, HTML, FRONTMATTER, and table cells (table
     * cells were tokenized during parse_table_at). A callout/quote that parsed
     * into child blocks carries its content there, so its own raw is left
     * un-tokenized (the children render the body). */
    finalize_block_tree(arena, bb.blocks.data, bb.blocks.len);
    if (g_md_work && g_md_work->limited) goto parse_done;

    /* Resolve "[text][label]" against the definitions collected above. Must run
     * before the footnote passes, which reshuffle the block array. */
    linkref_resolve_blocks(arena, &defs, bb.blocks.data, bb.blocks.len);
    if (g_md_work && g_md_work->limited) goto parse_done;

    /* Stash frontmatter convenience handle. */
    if (bb.blocks.len && bb.blocks.data[0].kind == MD_BLOCK_FRONTMATTER) {
        doc->frontmatter_yaml = bb.blocks.data[0].raw;
        fm_extract(doc);
    }

    /* Number footnotes across the whole tree (refs + their defs), then move
     * the definitions to the back matter where a reader expects them. Both run
     * on the builder's array: the reflow reorders it, so the arena copy has to
     * happen afterwards. */
    {
        FnTable fn = {0};
        fn_walk_blocks(&fn, bb.blocks.data, bb.blocks.len);
    }
    fn_reflow(arena, &defs, &bb.blocks);

    if (g_md_work && g_md_work->limited) goto parse_done;

    /* Copy block array into arena. */
    if (bb.blocks.len) {
        MdBlock *out = arena_alloc_array(arena, (usize)bb.blocks.len, sizeof(MdBlock));
        if (out) {
            memcpy(out, bb.blocks.data, (usize)bb.blocks.len * sizeof(MdBlock));
            doc->blocks = out;
            doc->block_count = bb.blocks.len;
        }
    }
parse_done:
    free(bb.blocks.data);
    doc->parse_limited = g_md_work && g_md_work->limited;
    doc->incomplete = arena->refused != refused_before || doc->parse_limited;
    /* An aborted tree must never look like a successfully parsed prefix. */
    if (doc->parse_limited) { doc->blocks = NULL; doc->block_count = 0; }
    return doc;
}

MdDoc *md_parse_ex(Arena *arena, const u8 *source, usize len, const char *base_dir,
                    const _Atomic bool *cancel) {
    usize allowance;
    if (!checked_mul_usize(len, MD_WORK_PER_BYTE, &allowance) ||
        !checked_add_usize(allowance, MD_WORK_FIXED, &allowance)) return NULL;
    MdParseWork work = { .remaining = allowance, .cancel = cancel };
    MdParseWork *previous = g_md_work;
    g_md_work = &work;
    /* Check even an empty input or an already cancelled queued job. */
    bool admitted = md_work_step();
    MdDoc *doc = admitted ? parse_document(arena, source, len, base_dir) : NULL;
    g_md_work = previous;
    return doc;
}

MdDoc *md_parse(Arena *arena, const u8 *source, usize len, const char *base_dir) {
    return md_parse_ex(arena, source, len, base_dir, NULL);
}

u32 md_block_count(const MdDoc *doc) {
    return doc ? doc->block_count : 0u;
}
