/*
 * Liu — URL detection in terminal content
 * Scans terminal cells to find URLs at a given position.
 * Uses simple string matching — no regex library required.
 */
#include "terminal/url.h"
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

/*
 * Extract a text line from terminal cells into a UTF-8 buffer.
 * Returns the number of bytes written. Only handles ASCII/BMP for URL detection.
 * col_map[i] gives the column index for byte i in the output buffer.
 */
static i32 extract_row_text(Terminal *t, i32 row, char *buf, i32 buf_size,
                             i32 *col_map, i32 map_size) {
    i32 pos = 0;
    for (i32 c = 0; c < t->cols && pos < buf_size - 1 && pos < map_size; c++) {
        Cell *cell = terminal_cell_at(t, c, row);
        if (!cell) break;
        u32 cp = cell->codepoint;
        if (cp == 0) cp = ' ';
        /* Only handle ASCII for URL matching */
        if (cp < 128) {
            col_map[pos] = c;
            buf[pos++] = (char)cp;
        } else {
            /* Non-ASCII: treat as a valid URL char (could be internationalized domain) */
            col_map[pos] = c;
            buf[pos++] = '?';  /* placeholder — won't match protocol prefixes */
        }
    }
    buf[pos] = '\0';
    return pos;
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

bool url_detect_at(Terminal *t, i32 col, i32 row, TermURL *result) {
    if (!t || !result) return false;
    if (col < 0 || col >= t->cols || row < 0 || row >= t->rows) return false;

    memset(result, 0, sizeof(*result));

    /* Extract the row text */
    char text[4096];
    i32 col_map[4096];
    i32 text_len = extract_row_text(t, row, text, sizeof(text),
                                     col_map, ARRAY_LEN(col_map));
    if (text_len == 0) return false;

    /* Find which byte position corresponds to the clicked column */
    i32 byte_pos = -1;
    for (i32 i = 0; i < text_len; i++) {
        if (col_map[i] == col) { byte_pos = i; break; }
    }
    /* If exact col not found, try closest */
    if (byte_pos < 0) {
        for (i32 i = 0; i < text_len; i++) {
            if (col_map[i] >= col) { byte_pos = i; break; }
        }
    }
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
    if (url_len >= (i32)sizeof(result->url)) url_len = (i32)sizeof(result->url) - 1;
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
            snprintf(tmp, sizeof(tmp), "mailto:%s", result->url);
            memcpy(result->url, tmp, strlen(tmp) + 1);
        } else {
            return false;  /* Not a recognized URL pattern */
        }
    }

    /* For www. URLs, prepend https:// */
    if (starts_with_ci(result->url, "www.")) {
        char tmp[2048];
        snprintf(tmp, sizeof(tmp), "https://%s", result->url);
        memcpy(result->url, tmp, strlen(tmp) + 1);
    }

    /* Set position info */
    result->start_row = row;
    result->end_row = row;
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
