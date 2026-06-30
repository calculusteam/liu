/*
 * Liu - terminal text search (per-terminal instance)
 * Supports both plain substring matching and POSIX extended regex.
 */
#include "terminal/terminal.h"
#include "core/string_utils.h"
#include "core/utf8.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#ifndef PLATFORM_WIN32
#include <regex.h>   /* MinGW has no POSIX regex; see terminal_search_start */
#endif

/* Maximum byte length for a single row converted to UTF-8.
 * Each cell can produce up to 4 bytes, and terminals rarely exceed 1024 cols. */
#define ROW_UTF8_BUF_SIZE (4096)

static bool char_match(u32 a, u32 b, bool case_sens) {
    if (case_sens) return a == b;
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

/* Grow the match array if needed. Returns false on allocation failure. */
static bool ensure_match_capacity(TermSearch *s) {
    if (s->match_count < s->match_cap) return true;
    i32 new_cap = s->match_cap * 2;
    void *new_buf = realloc(s->matches, (size_t)new_cap * sizeof(TermSearchMatch));
    if (!new_buf) return false;
    s->matches = new_buf;
    s->match_cap = new_cap;
    return true;
}

#ifndef PLATFORM_WIN32
/*
 * Convert a row of cells to a UTF-8 string.
 * Fills col_map so that col_map[col] = byte offset where that column starts.
 * Returns the total byte length written (not null-terminated in the count).
 * The buffer IS null-terminated for regex use.
 */
static i32 cells_to_utf8(Cell *cells, i32 ncols, char *buf, i32 buf_size, i32 *col_map) {
    i32 byte_off = 0;
    i32 col = 0;
    for (; col < ncols; col++) {
        col_map[col] = byte_off;
        u32 cp = cells[col].codepoint;
        if (cp == 0) cp = ' '; /* empty cells are spaces */
        u8 tmp[4];
        u32 n = utf8_encode(cp, tmp);
        if (n == 0) { tmp[0] = '?'; n = 1; } /* fallback for invalid */
        if (byte_off + (i32)n >= buf_size - 1) break; /* leave room for null */
        memcpy(buf + byte_off, tmp, n);
        byte_off += (i32)n;
    }
    /* On truncation the loop broke with col < ncols, so col_map[col..ncols-1]
     * (the columns whose bytes never fit) are uninitialized. Consumers bound
     * their col_map scans by ncols, so clamp the tail to the final byte_off to
     * keep the map monotonic and avoid reading uninitialized entries. */
    for (; col < ncols; col++) col_map[col] = byte_off;
    buf[byte_off] = '\0';
    return byte_off;
}

/*
 * Given a byte offset in the UTF-8 row string, find the corresponding column
 * by searching the col_map. Returns -1 if not found.
 */
static i32 byte_offset_to_col(i32 *col_map, i32 ncols, i32 byte_off) {
    /* Binary search would be possible, but col_map is monotonically increasing
     * and ncols is small. Linear scan from the end is fine. */
    for (i32 c = ncols - 1; c >= 0; c--) {
        if (col_map[c] <= byte_off) return c;
    }
    return 0;
}

/*
 * Given start and end byte offsets of a regex match, compute the column span
 * (start_col, length in columns).
 */
static void match_byte_range_to_cols(i32 *col_map, i32 ncols,
                                     i32 match_start_byte, i32 match_end_byte,
                                     i32 *out_col, i32 *out_len) {
    i32 start_col = byte_offset_to_col(col_map, ncols, match_start_byte);
    /* end byte is exclusive, so find the column for (end_byte - 1) then +1 */
    i32 end_col;
    if (match_end_byte <= match_start_byte) {
        end_col = start_col;
    } else {
        end_col = byte_offset_to_col(col_map, ncols, match_end_byte - 1) + 1;
    }
    *out_col = start_col;
    *out_len = end_col - start_col;
    if (*out_len < 1) *out_len = 1;
}

/*
 * Search a single row using regex. Adds all non-overlapping matches.
 */
static void regex_search_row(TermSearch *s, regex_t *re, Cell *cells, i32 ncols, i32 row) {
    char buf[ROW_UTF8_BUF_SIZE];
    i32 col_map[4096]; /* col -> byte offset; 4096 cols max */
    if (ncols > 4096) ncols = 4096;

    i32 buf_len = cells_to_utf8(cells, ncols, buf, ROW_UTF8_BUF_SIZE, col_map);
    if (buf_len == 0) return;

    /* Find all non-overlapping matches in this line */
    const char *search_start = buf;
    regmatch_t pmatch[1];
    while (regexec(re, search_start, 1, pmatch, (search_start != buf) ? REG_NOTBOL : 0) == 0) {
        i32 so = pmatch[0].rm_so;
        i32 eo = pmatch[0].rm_eo;
        if (so == eo) {
            /* Zero-length match: advance by one byte to avoid infinite loop */
            search_start++;
            if (*search_start == '\0') break;
            continue;
        }

        /* Convert byte offsets (relative to buf start) to column positions */
        i32 abs_so = (i32)(search_start - buf) + so;
        i32 abs_eo = (i32)(search_start - buf) + eo;

        i32 match_col, match_len;
        match_byte_range_to_cols(col_map, ncols, abs_so, abs_eo, &match_col, &match_len);

        if (!ensure_match_capacity(s)) return;
        s->matches[s->match_count++] = (TermSearchMatch){
            .col = match_col, .row = row, .length = match_len
        };

        /* Advance past this match */
        search_start += eo;
        if (*search_start == '\0') break;
    }
}
#endif /* !PLATFORM_WIN32 */

/* =========================================================================
 * Plain text search (original logic)
 * ========================================================================= */

static void plain_search_screen(Terminal *t, TermSearch *s, const char *query, i32 qlen, bool case_sensitive) {
    for (i32 row = 0; row < t->rows; row++) {
        for (i32 col = 0; col <= t->cols - qlen; col++) {
            bool found = true;
            for (i32 k = 0; k < qlen; k++) {
                Cell *c = terminal_cell_at(t, col + k, row);
                if (!c || !char_match(c->codepoint, (u32)(unsigned char)query[k], case_sensitive)) {
                    found = false;
                    break;
                }
            }
            if (found) {
                if (!ensure_match_capacity(s)) return;
                s->matches[s->match_count++] = (TermSearchMatch){
                    .col = col, .row = row, .length = qlen
                };
            }
        }
    }
}

static void plain_search_scrollback(Terminal *t, TermSearch *s, const char *query, i32 qlen, bool case_sensitive) {
    extern TermLine *sb_get(Terminal *t, i32 index);
    for (i32 i = 0; i < t->sb_count; i++) {
        TermLine *line = sb_get(t, i);
        if (!line) continue;

        /* Cold rows: read codepoints directly, no Cell materialize needed. */
        const u32 *cps;
        i32        line_len;
        if (line->cold) {
            cps = termline_codepoints(line->cold, &line_len);
        } else if (line->cells) {
            cps = NULL;       /* signals "use line->cells->codepoint below" */
            line_len = line->len;
        } else {
            continue;
        }
        if (line_len <= 0) continue;

        for (i32 col = 0; col <= line_len - qlen; col++) {
            bool found = true;
            for (i32 k = 0; k < qlen; k++) {
                u32 cp = cps ? cps[col + k] : line->cells[col + k].codepoint;
                if (!char_match(cp, (u32)(unsigned char)query[k], case_sensitive)) {
                    found = false;
                    break;
                }
            }
            if (found) {
                if (!ensure_match_capacity(s)) return;
                i32 sb_row = -(t->sb_count - i);
                s->matches[s->match_count++] = (TermSearchMatch){
                    .col = col, .row = sb_row, .length = qlen
                };
            }
        }
    }
}

#ifndef PLATFORM_WIN32
/* =========================================================================
 * Regex search
 * ========================================================================= */

static void regex_search_screen(Terminal *t, TermSearch *s, regex_t *re) {
    for (i32 row = 0; row < t->rows; row++) {
        Cell *cells = terminal_cell_at(t, 0, row);
        if (!cells) continue;
        regex_search_row(s, re, cells, t->cols, row);
    }
}

static void regex_search_scrollback(Terminal *t, TermSearch *s, regex_t *re) {
    extern TermLine *sb_get(Terminal *t, i32 index);
    /* One grow-on-demand scratch reused across every cold line, instead of a
     * malloc+free per line (a wide scrollback is thousands of lines per
     * regex search). */
    Cell *buf = NULL;
    i32   buf_cap = 0;
    for (i32 i = 0; i < t->sb_count; i++) {
        TermLine *line = sb_get(t, i);
        if (!line) continue;
        i32 sb_row = -(t->sb_count - i);

        if (line->cold) {
            i32 lc = line->cold->len;
            if (lc <= 0) continue;
            if (lc > buf_cap) {
                Cell *nb = realloc(buf, (usize)lc * sizeof(Cell));
                if (!nb) continue;   /* keep prior buf; skip this line */
                buf = nb;
                buf_cap = lc;
            }
            i32 wrote = termline_materialize(line->cold, buf, lc);
            regex_search_row(s, re, buf, wrote, sb_row);
        } else if (line->cells) {
            regex_search_row(s, re, line->cells, line->len, sb_row);
        }
    }
    free(buf);
}
#endif /* !PLATFORM_WIN32 */

/* =========================================================================
 * Public API
 * ========================================================================= */

void terminal_search_start(Terminal *t, const char *query, bool case_sensitive, bool use_regex) {
    TermSearch *s = &t->search;
    free(s->matches);
    memset(s, 0, sizeof(*s));
    snprintf(s->query, sizeof(s->query), "%s", query);
    s->case_sensitive = case_sensitive;
    s->use_regex = use_regex;
    s->active = true;
    s->match_cap = 256;
    s->matches = malloc((size_t)s->match_cap * sizeof(TermSearchMatch));
    if (!s->matches) { s->active = false; return; }

    i32 qlen = (i32)strlen(query);
    if (qlen == 0) return;

#ifndef PLATFORM_WIN32
    if (use_regex) {
        /* Compile POSIX extended regex */
        regex_t re;
        int flags = REG_EXTENDED;
        if (!case_sensitive) flags |= REG_ICASE;
        int rc = regcomp(&re, query, flags);
        if (rc != 0) {
            /* Invalid regex: treat as no matches */
            return;
        }

        /* Search current screen */
        regex_search_screen(t, s, &re);

        /* Search scrollback */
        regex_search_scrollback(t, s, &re);

        regfree(&re);
    } else
#endif
    {
        /* Plain substring search */
        plain_search_screen(t, s, query, qlen, case_sensitive);
        plain_search_scrollback(t, s, query, qlen, case_sensitive);
    }

    s->current = s->match_count > 0 ? s->match_count - 1 : -1;
}

void terminal_search_next(Terminal *t) {
    TermSearch *s = &t->search;
    if (s->match_count == 0) return;
    s->current = (s->current + 1) % s->match_count;
}

void terminal_search_prev(Terminal *t) {
    TermSearch *s = &t->search;
    if (s->match_count == 0) return;
    s->current = (s->current - 1 + s->match_count) % s->match_count;
}

void terminal_search_stop(Terminal *t) {
    free(t->search.matches);
    memset(&t->search, 0, sizeof(t->search));
}

bool terminal_search_is_highlighted(Terminal *t, i32 col, i32 row) {
    TermSearch *s = &t->search;
    if (!s->active) return false;
    for (i32 i = 0; i < s->match_count; i++) {
        TermSearchMatch *m = &s->matches[i];
        if (m->row == row && col >= m->col && col < m->col + m->length)
            return true;
    }
    return false;
}

bool terminal_search_is_current(Terminal *t, i32 col, i32 row) {
    TermSearch *s = &t->search;
    if (!s->active || s->current < 0) return false;
    TermSearchMatch *m = &s->matches[s->current];
    return m->row == row && col >= m->col && col < m->col + m->length;
}
