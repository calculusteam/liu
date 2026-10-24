/*
 * Liu — URL detection in terminal content
 * Scans terminal cells to find URLs at a given position.
 * Uses simple string matching — no regex library required.
 */
#include "terminal/url.h"
#include "terminal/bidi.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* Characters that are valid within a URL (not including delimiters) */
static bool is_url_char(u32 cp) {
    if (cp < 32 || cp > 126) return false;  /* ASCII printable only */
    /* Exclude common delimiters and whitespace */
    switch (cp) {
    case ' ': case '\t': case '<': case '>': case '"': case '\'':
        return false;
    default:
        return true;
    }
}

/* Characters that shouldn't end a URL (trailing punctuation) */
static bool is_trailing_punct(char c) {
    switch (c) {
    case '.': case ',': case ';': case ':': case '!': case '?':
        return true;
    default:
        return false;
    }
}

/* Walk a URL-like token across actual soft wraps. The coordinates remain
 * viewport-relative, including negative rows when the prefix is in history.
 * Cache one row while walking it; wrap metadata reads do not invalidate the
 * scratch pointer. Resolving the next row replaces the cached pointer. */
typedef struct {
    const Cell *cells;
    i32 row, len;
    bool valid;
} URLRow;

static u32 url_cell(Terminal *t, URLRow *cache, i32 row, i32 col) {
    if (!cache->valid || cache->row != row) {
        cache->cells = terminal_row_unclipped(t, row, &cache->len);
        cache->row = row;
        cache->valid = true;
    }
    return cache->cells && col >= 0 && col < cache->len
        ? cache->cells[col].codepoint : 0;
}

static bool url_step(Terminal *t, i32 *row, i32 *col, i32 direction) {
    if (direction < 0) {
        if (*col > 0) { --*col; return true; }
        if (!terminal_row_wrapped_unclipped(t, *row - 1)) return false;
        --*row; *col = t->cols - 1;
    } else {
        if (*col + 1 < t->cols) { ++*col; return true; }
        if (!terminal_row_wrapped_unclipped(t, *row)) return false;
        ++*row; *col = 0;
    }
    return terminal_row_exists(t, *row);
}

/* Check if text at position starts with a given prefix (case-insensitive) */
static bool starts_with_ci(const char *text, const char *prefix) {
    for (i32 i = 0; prefix[i]; i++) {
        if (!text[i]) return false;
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

/* Find the start of a URL that contains the byte at `byte_pos` in `text`.
 * Returns the byte offset of the URL start, or -1 if not found. */
static i32 find_url_start(const char *text, i32 text_len, i32 byte_pos) {
    /* Scan leftward to find the beginning of the URL-like string */
    i32 start = byte_pos;
    while (start > 0 && is_url_char((u32)(unsigned char)text[start - 1])) {
        start--;
    }

    /* Check known protocol prefixes starting from 'start' */
    static const char *protocols[] = {
        "https://", "http://", "ftp://", "file://", "mailto:",
        NULL
    };

    /* Try to find a protocol prefix within the candidate range */
    for (i32 s = start; s <= byte_pos; s++) {
        for (i32 p = 0; protocols[p]; p++) {
            if (starts_with_ci(text + s, protocols[p])) {
                return s;
            }
        }
    }

    /* Check for www. prefix (bare domain) */
    for (i32 s = start; s <= byte_pos; s++) {
        if (starts_with_ci(text + s, "www.")) {
            return s;
        }
    }

    /* Check for email pattern: scan for @ sign */
    i32 at_pos = -1;
    for (i32 i = start; i < text_len && is_url_char((u32)(unsigned char)text[i]); i++) {
        if (text[i] == '@') { at_pos = i; break; }
    }
    if (at_pos > start && at_pos < text_len - 1) {
        /* Verify left side looks like an email local part */
        bool valid_local = true;
        for (i32 i = start; i < at_pos; i++) {
            char c = text[i];
            if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '%' &&
                c != '+' && c != '-') {
                valid_local = false;
                break;
            }
        }
        /* Verify right side looks like a domain */
        if (valid_local) {
            bool has_dot = false;
            i32 domain_end = at_pos + 1;
            for (; domain_end < text_len && is_url_char((u32)(unsigned char)text[domain_end]); domain_end++) {
                if (text[domain_end] == '.') has_dot = true;
            }
            if (has_dot && domain_end > at_pos + 3) {
                return start;
            }
        }
    }

    return -1;
}

/* Find the end of a URL starting at `start` in `text`.
 * Returns the byte offset one past the last URL character. */
static i32 find_url_end(const char *text, i32 text_len, i32 start) {
    i32 end = start;
    i32 paren_depth = 0;
    i32 bracket_depth = 0;

    while (end < text_len && is_url_char((u32)(unsigned char)text[end])) {
        if (text[end] == '(') paren_depth++;
        else if (text[end] == ')') {
            if (paren_depth > 0) paren_depth--;
            else break;  /* unmatched closing paren — not part of URL */
        }
        else if (text[end] == '[') bracket_depth++;
        else if (text[end] == ']') {
            if (bracket_depth > 0) bracket_depth--;
            else break;
        }
        end++;
    }

    /* Trim trailing punctuation */
    while (end > start && is_trailing_punct(text[end - 1])) {
        end--;
    }

    return end;
}

/* The visual->logical entry point. See url.h for why every pointer-driven path
 * goes through one function instead of converting at each site. */
bool url_detect_at_visual(Terminal *t, i32 visual_col, i32 row, TermURL *result) {
    if (!t || !result) return false;
    if (visual_col < 0 || visual_col >= t->cols || row < 0 || row >= t->rows)
        return false;

    i32 logical = visual_col;
    /* The gate the renderer applies, and it has to be the same one: a row drawn in
     * logical order must convert as the identity here, or input and output disagree
     * about where a cell is. `bidi_line_has_rtl` is inside
     * bidi_visual_to_logical_col, so a row with no RTL costs one scan and returns
     * the column unchanged. */
    if (t->bidi_enabled && t->cols > 0 && t->cols <= 1024) {
        /* The SAME source the URL scanner reads, so the conversion and the
         * detection cannot be looking at different text — which under a scrolled
         * view they would be if either used the live grid. COPIED rather than held:
         * terminal_visible_row hands back a scratch buffer that url_detect_at's own
         * reads below invalidate. */
        i32 row_len = 0;
        const Cell *vis = terminal_visible_row(t, row, &row_len);
        if (vis && row_len >= t->cols) {
            Cell rowbuf[1024];
            memcpy(rowbuf, vis, (usize)t->cols * sizeof(Cell));
            logical = bidi_visual_to_logical_col(rowbuf, t->cols, visual_col);
        }
        /* A row shorter than the grid is a trimmed scrollback line, and the renderer
         * does not reorder those either (the row_len >= cols gate in
         * app_term_row_bidi_map) — so the identity here is what agrees with it. */
    }
    return url_detect_at(t, logical, row, result);
}

bool url_detect_at(Terminal *t, i32 col, i32 row, TermURL *result) {
    if (!t || !result) return false;
    if (col < 0 || col >= t->cols || row < 0 || row >= t->rows) return false;

    memset(result, 0, sizeof(*result));

    /* Bounded by the public URL capacity. Refuse an oversized token rather
     * than opening a silently truncated URL or scanning unbounded history. */
    char text[2048];
    i32 col_map[2048], row_map[2048];
    URLRow cache = {0};
    i32 first_row = row, first_col = col;
    if (!is_url_char(url_cell(t, &cache, row, col))) return false;
    for (i32 count = 0; ; count++) {
        i32 rr = first_row, cc = first_col;
        if (!url_step(t, &rr, &cc, -1) || !is_url_char(url_cell(t, &cache, rr, cc))) break;
        if (count >= (i32)sizeof(text) - 2) return false;
        first_row = rr; first_col = cc;
    }
    i32 text_len = 0, byte_pos = -1;
    i32 rr = first_row, cc = first_col;
    do {
        u32 cp = url_cell(t, &cache, rr, cc);
        if (!is_url_char(cp)) break;
        if (text_len >= (i32)sizeof(text) - 1) return false;
        if (rr == row && cc == col) byte_pos = text_len;
        row_map[text_len] = rr;
        col_map[text_len] = cc;
        text[text_len++] = (char)cp;
    } while (url_step(t, &rr, &cc, 1));
    text[text_len] = '\0';
    if (byte_pos < 0) return false;

    /* Check if the character at click position is a valid URL char */
    if (!is_url_char((u32)(unsigned char)text[byte_pos])) return false;

    /* Find URL boundaries */
    i32 url_start = find_url_start(text, text_len, byte_pos);
    if (url_start < 0) return false;

    i32 url_end = find_url_end(text, text_len, url_start);
    if (url_end <= url_start) return false;

    /* Verify the click position is within the URL */
    if (byte_pos < url_start || byte_pos >= url_end) return false;

    /* Extract the URL string */
    i32 url_len = url_end - url_start;
    if (url_len >= (i32)sizeof(result->url)) return false;
    memcpy(result->url, text + url_start, (usize)url_len);
    result->url[url_len] = '\0';

    /* For email addresses without mailto:, prepend it */
    if (!starts_with_ci(result->url, "http") &&
        !starts_with_ci(result->url, "ftp") &&
        !starts_with_ci(result->url, "file") &&
        !starts_with_ci(result->url, "mailto:") &&
        !starts_with_ci(result->url, "www.")) {
        /* Check if it looks like an email (has @ and no protocol) */
        bool has_at = false;
        for (i32 i = 0; i < url_len; i++) {
            if (result->url[i] == '@') { has_at = true; break; }
        }
        if (has_at) {
            char tmp[2048];
            int n = snprintf(tmp, sizeof(tmp), "mailto:%s", result->url);
            if (n < 0 || (usize)n >= sizeof(tmp) ||
                (usize)n >= sizeof(result->url)) return false;
            memcpy(result->url, tmp, (usize)n + 1u);
        } else {
            return false;  /* Not a recognized URL pattern */
        }
    }

    /* For www. URLs, prepend https:// */
    if (starts_with_ci(result->url, "www.")) {
        char tmp[2048];
        int n = snprintf(tmp, sizeof(tmp), "https://%s", result->url);
        if (n < 0 || (usize)n >= sizeof(tmp) ||
            (usize)n >= sizeof(result->url)) return false;
        memcpy(result->url, tmp, (usize)n + 1u);
    }

    /* Set position info */
    result->start_row = row_map[url_start];
    result->end_row = row_map[url_end - 1];
    result->start_col = col_map[url_start];
    result->end_col = (url_end > 0 && url_end <= text_len)
                       ? col_map[url_end - 1] : col_map[url_start];

    return true;
}

bool url_contains(const TermURL *url, i32 col, i32 row) {
    if (!url || url->url[0] == '\0') return false;

    /* Single-row URL (most common case) */
    if (url->start_row == url->end_row) {
        return row == url->start_row &&
               col >= url->start_col && col <= url->end_col;
    }

    /* Multi-row URL */
    if (row < url->start_row || row > url->end_row) return false;
    if (row == url->start_row) return col >= url->start_col;
    if (row == url->end_row) return col <= url->end_col;
    return true;  /* middle rows are fully contained */
}
