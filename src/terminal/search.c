/*
 * Liu - terminal text search (per-terminal instance)
 * Supports both plain substring matching and POSIX extended regex.
 */
#include "terminal/terminal.h"
#include "core/string_utils.h"
#include "core/checked.h"
#include "core/utf8.h"
#include "platform/platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#ifndef PLATFORM_WIN32
#include <regex.h>   /* MinGW has no POSIX regex; see terminal_search_start */
#endif

/* Hard ceiling on retained matches. The array outlives the search call (the
 * render path walks it every frame until the search is replaced or stopped), so
 * the cap is a *retention* budget, not just a scan guard: 64K matches x
 * sizeof(TermSearchMatch) = 768 KB per terminal worst case. A single-character
 * query over a 20K-line scrollback blows past that, and results beyond the first
 * 64K are useless to a human anyway — collection stops there and the result set
 * is truncated. */
#define MAX_SEARCH_MATCHES (64 * 1024)

/* True once collection hit the ceiling; callers use it to abandon the remaining
 * rows instead of re-running the matcher for every one of them. */
static inline bool search_matches_full(const TermSearch *s) {
    return s->match_count >= MAX_SEARCH_MATCHES;
}

/*
 * Give back the slack the doubling growth left behind (and the whole buffer when
 * nothing matched). Called once, after collection finishes: the peak capacity is
 * up to 2x the matches actually kept, and it would otherwise stay resident for
 * the terminal's lifetime.
 */
static void search_shrink_to_fit(TermSearch *s) {
    if (!s->matches) { s->match_cap = 0; return; }
    if (s->match_count <= 0) {
        free(s->matches);
        s->matches = NULL;
        s->match_cap = 0;
        return;
    }
    if (s->match_count >= s->match_cap) return;
    void *shrunk = realloc(s->matches,
                           (usize)s->match_count * sizeof(TermSearchMatch));
    if (!shrunk) return;   /* keep the larger buffer; its contents are intact */
    s->matches = shrunk;
    s->match_cap = s->match_count;
}

/* One letter, folded for a case-insensitive comparison beyond ASCII, so "özel"
 * finds "ÖZEL" and "é" finds "É". The ranges are the scripts whose upper and
 * lower case sit a fixed distance apart or in pairs: Latin-1, Latin
 * Extended-A, Greek and Cyrillic. It is a table of LETTERS, not of a language:
 * İ folds to i like I does, and ı stays itself. */
static u32 search_fold(u32 cp, bool case_sensitive) {
    if (case_sensitive) return cp;
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + ('a' - 'A') : cp;
    if (cp >= 0xC0 && cp <= 0xDE) return cp == 0xD7 ? cp : cp + 0x20;            /* À..Þ, not × */
    if (cp >= 0x100 && cp <= 0x17F) {
        if (cp == 0x130) return 'i';
        if (cp == 0x178) return 0xFF;                                             /* Ÿ */
        if (cp == 0x131 || cp == 0x138 || cp == 0x149 || cp == 0x17F) return cp;
        if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E))
            return (cp & 1u) ? cp + 1 : cp;                                       /* odd is the capital */
        return (cp & 1u) ? cp : cp + 1;                                           /* even is the capital */
    }
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 0x20;             /* Α..Ω */
    if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;                             /* А..Я */
    if (cp >= 0x400 && cp <= 0x40F) return cp + 0x50;                             /* Ѐ..Џ */
    return cp;
}

/* Grow the match array if needed. Returns false on allocation failure. */
static bool ensure_match_capacity(TermSearch *s) {
    if (!s || s->match_count < 0 || s->match_cap < 0 ||
        s->match_count >= MAX_SEARCH_MATCHES) return false;
    if (s->match_count < s->match_cap) return true;
    i32 new_cap = s->match_cap > 0 ? s->match_cap : 256;
    if (s->match_cap > 0) {
        if (s->match_cap > MAX_SEARCH_MATCHES / 2)
            new_cap = MAX_SEARCH_MATCHES;
        else
            new_cap = s->match_cap * 2;
    }
    usize bytes;
    if (new_cap <= s->match_count ||
        !checked_mul_usize((usize)new_cap, sizeof(TermSearchMatch), &bytes) ||
        bytes == 0)
        return false;
    void *new_buf = realloc(s->matches, bytes);
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
        if (cells[col].attr.flags & ATTR_WDUMMY) continue;
        u32 cp = cells[col].codepoint;
        if (cp == 0) cp = ' '; /* empty cells are spaces */
        /* A cell may hold a whole grapheme cluster; regex sees its real text. */
        u8 tmp[UTF8_CLUSTER_MAX * 4];
        u32 n = utf8_cluster_encode(cp, tmp, sizeof(tmp));
        if (n == 0) { tmp[0] = '?'; n = 1; } /* fallback for invalid */
        if (byte_off + (i32)n >= buf_size) break; /* leave room for null */
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
    /* Upper bound also handles duplicate offsets from wide-cell trailers.
     * A short regex matching every cell must not rescan the row per hit. */
    i32 lo = 0, hi = ncols;
    while (lo < hi) {
        i32 mid = lo + (hi - lo) / 2;
        if (col_map[mid] <= byte_off) lo = mid + 1;
        else hi = mid;
    }
    return lo > 0 ? lo - 1 : 0;
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
typedef struct {
    char *bytes;
    i32 *columns;
    usize byte_cap;
    i32 column_cap;
} RegexScratch;

static bool regex_scratch_reserve(RegexScratch *scratch, i32 cols) {
    if (cols <= 0) return false;
    usize bytes, columns;
    if (!checked_mul_usize((usize)cols, UTF8_CLUSTER_MAX * 4u, &bytes) ||
        !checked_add_usize(bytes, 1u, &bytes) || bytes > INT_MAX ||
        !checked_mul_usize((usize)cols, sizeof(i32), &columns)) return false;
    if (scratch->column_cap < cols) {
        i32 *next = realloc(scratch->columns, columns);
        if (!next) return false;
        scratch->columns = next; scratch->column_cap = cols;
    }
    if (scratch->byte_cap < bytes) {
        char *next = realloc(scratch->bytes, bytes);
        if (!next) return false;
        scratch->bytes = next; scratch->byte_cap = bytes;
    }
    return true;
}

static void regex_search_row(TermSearch *s, regex_t *re, Cell *cells, i32 ncols,
                              i32 row, RegexScratch *scratch) {
    if (!regex_scratch_reserve(scratch, ncols)) return;
    char *buf = scratch->bytes;
    i32 *col_map = scratch->columns;
    i32 buf_len = cells_to_utf8(cells, ncols, buf, (i32)scratch->byte_cap, col_map);
    if (buf_len == 0) return;

    /* Find all non-overlapping matches in this line */
    const char *search_start = buf;
    regmatch_t pmatch[1];
    while (regexec(re, search_start, 1, pmatch, (search_start != buf) ? REG_NOTBOL : 0) == 0) {
        i32 so = pmatch[0].rm_so;
        i32 eo = pmatch[0].rm_eo;
        if (so == eo) {
            /* Advance from the actual zero-width match, but never step past
             * the terminating NUL (patterns such as "$" match there). */
            const char *match_pos = search_start + so;
            if (*match_pos == '\0') break;
            u32 cp;
            u32 advance = utf8_decode((const u8 *)match_pos,
                                      (usize)(buf + buf_len - match_pos), &cp);
            search_start = match_pos + (advance ? advance : 1);
            continue;
        }

        /* Convert byte offsets (relative to buf start) to column positions */
        i32 abs_so = (i32)(search_start - buf) + so;
        i32 abs_eo = (i32)(search_start - buf) + eo;

        i32 match_col, match_len;
        match_byte_range_to_cols(col_map, ncols, abs_so, abs_eo, &match_col, &match_len);
        while (match_col + match_len < ncols &&
               (cells[match_col + match_len].attr.flags & ATTR_WDUMMY)) match_len++;

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

typedef struct {
    u32 text[256];
    i32 fallback[256];
    i32 length;
    bool case_sensitive;
    bool ascii;
} SearchPattern;

static bool search_pattern_build(SearchPattern *p, const char *query, i32 bytes,
                                  bool case_sensitive) {
    memset(p, 0, sizeof *p);
    p->case_sensitive = case_sensitive;
    p->ascii = true;
    for (i32 off = 0; off < bytes;) {
        u32 cp;
        u32 n = utf8_decode((const u8 *)query + off, (usize)(bytes - off), &cp);
        if (!n || p->length >= (i32)(sizeof p->text / sizeof p->text[0])) return false;
        p->text[p->length++] = search_fold(cp, case_sensitive);
        if (cp >= 128) p->ascii = false;
        off += (i32)n;
    }
    for (i32 i = 1, matched = 0; i < p->length; i++) {
        while (matched && p->text[i] != p->text[matched]) matched = p->fallback[matched - 1];
        if (p->text[i] == p->text[matched]) matched++;
        p->fallback[i] = matched;
    }
    return p->length > 0;
}

/* Short ASCII queries over ordinary logs keep the inexpensive cell matcher.
 * The bound prevents long shared prefixes from becoming quadratic. A vector-
 * friendly row check sends all grapheme/width cases through the scalar KMP. */
static bool plain_search_short_ascii(TermSearch *s, const SearchPattern *p,
                                     const Cell *cells, TermLineCompressed *cold,
                                     i32 cols, i32 row) {
    if (!p->ascii || p->length > 16) return false;
    if (cold && cold->ascii_search_state == 2) return false;
    if (!cold || cold->ascii_search_state != 1) {
        if (cold && cold->wide_bits) {
            for (i32 word=0;word<cols/64+(cols%64!=0);word++)
                if (cold->wide_bits[word]) { cold->ascii_search_state=2; return false; }
        }
        u32 codepoints = 0, flags = 0;
        if (cold) {
            for (i32 col=0;col<cols;col++) codepoints |= cold->codepoints[col];
        } else {
            for (i32 col=0;col<cols;col++) {
                codepoints |= cells[col].codepoint;
                flags |= cells[col].attr.flags;
            }
        }
        bool ascii=codepoints<128 && !(flags & (ATTR_WIDE | ATTR_WDUMMY));
        /* Cold rows never change their cells. Repeated search edits can reuse
         * this classification; alloc/restore zero the nonserialized byte. */
        if (cold) cold->ascii_search_state=ascii?1:2;
        if (!ascii) return false;
    }
    for (i32 col=0;col<=cols-p->length;col++) {
        i32 k=0;
        for (;k<p->length;k++) {
            u32 cp=cold?cold->codepoints[col+k]:cells[col+k].codepoint;
            if (search_fold(cp?cp:' ',p->case_sensitive)!=p->text[k]) break;
        }
        if (k!=p->length) continue;
        if (!ensure_match_capacity(s)) return true;
        s->matches[s->match_count++]=(TermSearchMatch){.col=col,.row=row,.length=p->length};
    }
    return true;
}

/* Linear substring search over text, mapping scalar matches back to visual
 * cells. Cold history remains compressed; only its codepoint/width arrays
 * are read. The KMP fallback preserves overlapping matches. */
static void plain_search_row(TermSearch *s, const SearchPattern *pattern,
                              const Cell *cells, TermLineCompressed *cold,
                              i32 cols, i32 row) {
    if (plain_search_short_ascii(s, pattern, cells, cold, cols, row)) return;
    i32 matched = 0, starts[256];
    usize accepted = 0;
    for (i32 col = 0; col < cols && !search_matches_full(s); col++) {
        u32 cp = cold ? cold->codepoints[col] : cells[col].codepoint;
        /* Most log cells cannot start the query. Avoid width/cluster work
         * until a prefix is active; clusters still take the full scalar path. */
        if (!matched && cp < 128 &&
            search_fold(cp ? cp : ' ', pattern->case_sensitive) != pattern->text[0]) continue;
        bool dummy = cold ? (cold->wide_bits &&
            ((cold->wide_bits[col / 64] >> (col % 64)) & 1u)) :
            (cells[col].attr.flags & ATTR_WDUMMY) != 0;
        if (dummy) continue;
        u32 n = 1;
        const u32 *cluster = utf8_is_cluster(cp) ? utf8_cluster_lookup(cp, &n) : NULL;
        if (!cluster) { if (cp == 0) cp = ' '; cluster = &cp; n = 1; }
        for (u32 j = 0; j < n; j++) {
            u32 value = search_fold(cluster[j], pattern->case_sensitive);
            while (matched && value != pattern->text[matched]) matched = pattern->fallback[matched - 1];
            if (value != pattern->text[matched]) continue;
            if (pattern->length > 1) starts[accepted++ & 255u] = col;
            matched++;
            if (matched == pattern->length) {
                i32 first = pattern->length == 1 ? col :
                    starts[(accepted - (usize)pattern->length) & 255u];
                bool wide = cold ? (col + 1 < cols && cold->wide_bits &&
                    ((cold->wide_bits[(col + 1) / 64] >> ((col + 1) % 64)) & 1u)) :
                    (cells[col].attr.flags & ATTR_WIDE) != 0;
                i32 end_col = MIN(cols, col + (wide ? 2 : 1));
                if (!ensure_match_capacity(s)) return;
                s->matches[s->match_count++] = (TermSearchMatch){
                    .col = first, .row = row, .length = end_col - first
                };
                matched = pattern->fallback[matched - 1];
                if (search_matches_full(s)) return;
            }
        }
    }
}

static void plain_search_screen(Terminal *t, TermSearch *s, const SearchPattern *pattern) {
    for (i32 row = 0; row < t->rows && !search_matches_full(s); row++) {
        const Cell *cells = terminal_cell_at(t, 0, row);
        if (cells) plain_search_row(s, pattern, cells, NULL, t->cols, row);
    }
}

static void plain_search_scrollback(Terminal *t, TermSearch *s, const SearchPattern *pattern) {
    extern TermLine *sb_get(Terminal *t, i32 index);
    for (i32 i = 0; i < t->sb_count && !search_matches_full(s); i++) {
        TermLine *line = sb_get(t, i);
        if (!line) continue;
        if (line->cold && line->cold->codepoints)
            plain_search_row(s, pattern, NULL, line->cold, line->cold->len, -(t->sb_count - i));
        else if (line->cells)
            plain_search_row(s, pattern, line->cells, NULL, line->len, -(t->sb_count - i));
    }
}

#ifndef PLATFORM_WIN32
/* =========================================================================
 * Regex search
 * ========================================================================= */

static void regex_search_screen(Terminal *t, TermSearch *s, regex_t *re, RegexScratch *scratch) {
    for (i32 row = 0; row < t->rows; row++) {
        if (search_matches_full(s)) return;
        Cell *cells = terminal_cell_at(t, 0, row);
        if (!cells) continue;
        regex_search_row(s, re, cells, t->cols, row, scratch);
    }
}

static void regex_search_scrollback(Terminal *t, TermSearch *s, regex_t *re, RegexScratch *scratch) {
    extern TermLine *sb_get(Terminal *t, i32 index);
    /* One grow-on-demand scratch reused across every cold line, instead of a
     * malloc+free per line (a wide scrollback is thousands of lines per
     * regex search). */
    Cell *buf = NULL;
    i32   buf_cap = 0;
    for (i32 i = 0; i < t->sb_count; i++) {
        if (search_matches_full(s)) break;   /* buf still needs freeing below */
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
            regex_search_row(s, re, buf, wrote, sb_row, scratch);
        } else if (line->cells) {
            regex_search_row(s, re, line->cells, line->len, sb_row, scratch);
        }
    }
    free(buf);
}
#endif /* !PLATFORM_WIN32 */

/* =========================================================================
 * Public API
 * ========================================================================= */

static int search_match_cmp(const void *a, const void *b) {
    const TermSearchMatch *x = a, *y = b;
    if (x->row != y->row) return x->row < y->row ? -1 : 1;
    if (x->col != y->col) return x->col < y->col ? -1 : 1;
    return 0;
}

/* Owned by one terminal and touched only on the UI thread. The row cursor
 * uses absolute document lines so a full scrollback ring can evict its front
 * between steps without skipping, repeating or misplacing the remaining
 * rows. Live output keeps the immutable scanned history prefix and replaces
 * only the old screen/new history suffix. */
struct TermSearchWork {
    SearchPattern pattern;
    TermSearch screen_matches;
#ifndef PLATFORM_WIN32
    regex_t regex;
    RegexScratch scratch;
    bool regex_ready;
    Cell *cells;
    i32 cell_cap;
#endif
    bool pending;
    bool refresh;
    bool query_ready;
    bool scanning_screen;
    bool history_truncated;
    i64 history_start;
    i64 retained_history_end;
    i64 next_abs;
    i64 end_abs;
    i64 screen_start;
    i64 keep_abs;
    i32 keep_col;
    u64 revision;
    u64 layout_generation;
    i32 cols, rows;
    bool alt;
};

static i64 search_grid_origin(const Terminal *t) {
    return t->sb_abs_base + (i64)t->sb_count;
}

static i64 search_match_absolute(const Terminal *t, const TermSearchMatch *m) {
    return (t->search.anchored ? t->search.row_origin : search_grid_origin(t)) + m->row;
}

i32 terminal_search_match_row(const Terminal *t, const TermSearchMatch *m) {
    if (!t || !m) return INT_MIN;
    i64 row = search_match_absolute(t, m) - search_grid_origin(t);
    return row < INT_MIN ? INT_MIN : row > INT_MAX ? INT_MAX : (i32)row;
}

static void search_work_free(TermSearchWork *w) {
    if (!w) return;
#ifndef PLATFORM_WIN32
    if (w->regex_ready) regfree(&w->regex);
    free(w->scratch.bytes);
    free(w->scratch.columns);
    free(w->cells);
#endif
    free(w->screen_matches.matches);
    free(w);
}

usize terminal_search_bytes(const Terminal *t) {
    if (!t) return 0;
    usize bytes = (usize)t->search.match_cap * sizeof(TermSearchMatch);
    const TermSearchWork *w = t->search.work;
    if (!w) return bytes;
    bytes += sizeof(*w);
    bytes += (usize)w->screen_matches.match_cap * sizeof(TermSearchMatch);
#ifndef PLATFORM_WIN32
    bytes += w->scratch.byte_cap + (usize)w->scratch.column_cap * sizeof(i32) +
             (usize)w->cell_cap * sizeof(Cell);
#endif
    return bytes;
}

static i32 search_lower_bound(const TermSearch *s, i64 abs) {
    i32 lo = 0, hi = s->match_count;
    while (lo < hi) {
        i32 mid = lo + (hi - lo) / 2;
        if (s->row_origin + s->matches[mid].row < abs) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void search_keep_current(Terminal *t) {
    TermSearch *s = &t->search;
    TermSearchWork *w = s->work;
    if (s->current >= 0 && s->current < s->match_count) {
        w->keep_abs = search_match_absolute(t, &s->matches[s->current]);
        w->keep_col = s->matches[s->current].col;
    }
}

static bool search_layout_changed(const Terminal *t, const TermSearchWork *w) {
    return w->layout_generation != t->search_layout_generation ||
           w->cols != t->cols || w->rows != t->rows ||
           w->alt != ((t->mode & MODE_ALT_SCREEN) != 0) ||
           search_grid_origin(t) < w->screen_start;
}

/* Start a new generation in document order; the retained prefix is already
 * sorted, so completion needs no full-array qsort. */
static void search_work_schedule(Terminal *t, bool full) {
    TermSearch *s = &t->search;
    TermSearchWork *w = s->work;
    i64 from = full ? t->sb_abs_base :
        MAX(w->history_truncated ? MIN(w->screen_start, w->retained_history_end) :
                                   w->screen_start, t->sb_abs_base);
    if (full) s->match_count = 0;
    else {
        i32 first = search_lower_bound(s, t->sb_abs_base);
        i32 end = search_lower_bound(s, from);
        i32 count = end - first;
        if (count > 0 && first > 0)
            memmove(s->matches, s->matches + first, (usize)count * sizeof(*s->matches));
        s->match_count = count;
    }
    /* Keep relative coordinates bounded even after years of output. This is
     * a linear walk over the match array, never a walk over terminal cells. */
    i64 origin = search_grid_origin(t);
    i64 shift = s->row_origin - origin;
    for (i32 i = 0; i < s->match_count; i++) s->matches[i].row += (i32)shift;
    s->row_origin = origin;
    s->anchored = true;
    w->history_start = from;
    w->screen_start = origin;
    w->next_abs = origin;
    w->end_abs = origin + t->rows;
    w->scanning_screen = true;
    w->screen_matches.match_count = 0;
    w->revision = t->search_revision;
    w->layout_generation = t->search_layout_generation;
    w->cols = t->cols; w->rows = t->rows;
    w->alt = (t->mode & MODE_ALT_SCREEN) != 0;
    w->refresh = false;
    w->pending = w->query_ready && s->status == TERM_SEARCH_OK && s->query[0] != '\0';
    s->current = s->match_count > 0 ? s->match_count - 1 : -1;
    if (w->keep_abs >= 0 && s->match_count > 0)
        terminal_search_set_current_at(t, w->keep_abs, w->keep_col);
}

void terminal_search_begin(Terminal *t, const char *query, bool case_sensitive, bool use_regex) {
    if (!t || !query) return;
    /* A caller may pass s->query itself; preserve it before releasing s. */
    char saved_query[256];
    snprintf(saved_query, sizeof saved_query, "%s", query);
    terminal_search_stop(t);
    TermSearch *s = &t->search;
    snprintf(s->query, sizeof s->query, "%s", saved_query);
    s->active = true;
    s->current = -1;
    s->case_sensitive = case_sensitive;
    s->use_regex = use_regex;
    s->row_origin = search_grid_origin(t);
    s->anchored = true;
    if (!s->query[0]) return;
    TermSearchWork *w = calloc(1, sizeof(*w));
    if (!w) { s->active = false; return; }
    s->work = w;
    w->keep_abs = -1;
#ifndef PLATFORM_WIN32
    if (use_regex) {
        int flags = REG_EXTENDED | (case_sensitive ? 0 : REG_ICASE);
        if (regcomp(&w->regex, s->query, flags) != 0) {
            s->status = TERM_SEARCH_BAD_REGEX;
            return;
        }
        w->regex_ready = true;
    } else
#endif
    if (!search_pattern_build(&w->pattern, s->query, (i32)strlen(s->query), case_sensitive)) return;
    w->query_ready = true;
    search_work_schedule(t, true);
}

void terminal_search_restart(Terminal *t) {
    if (!t || !t->search.active || !t->search.work) return;
    TermSearchWork *w = t->search.work;
    /* A pending scan may only have reached older rows; its provisional
     * current must not replace an anchor requested for a later row. Explicit
     * next/prev/set-current actions update that anchor themselves. */
    if (!w->pending) search_keep_current(t);
    if (search_layout_changed(t, w)) search_work_schedule(t, true);
    else if (w->pending) w->refresh = true;
    else search_work_schedule(t, false);
}

bool terminal_search_pending(const Terminal *t) {
    return t && t->search.active && t->search.work && t->search.work->pending;
}

static void search_work_row(Terminal *t, i64 abs) {
    TermSearch *owner = &t->search;
    TermSearchWork *w = owner->work;
    TermSearch *s = w->scanning_screen ? &w->screen_matches : owner;
    i64 origin = search_grid_origin(t);
    i64 relative = abs - owner->row_origin;
    if (relative < INT_MIN || relative > INT_MAX) return;
    i32 row = (i32)relative;
    const Cell *cells = NULL;
    TermLineCompressed *cold = NULL;
    i32 cols = 0;
    if (abs < origin) {
        extern TermLine *sb_get(Terminal *t, i32 index);
        i64 index = abs - t->sb_abs_base;
        if (index < 0 || index >= t->sb_count) return;
        TermLine *line = sb_get(t, (i32)index);
        if (!line) return;
        cold = line->cold;
        cells = line->cells;
        cols = cold ? cold->len : line->len;
    } else {
        i64 index = abs - origin;
        if (index < 0 || index >= t->rows) return;
        cells = terminal_cell_at(t, 0, (i32)index);
        cols = t->cols;
    }
    if (cols <= 0 || (!cold && !cells)) return;
#ifndef PLATFORM_WIN32
    if (w->regex_ready) {
        if (cold) {
            if (cols > w->cell_cap) {
                Cell *next = realloc(w->cells, (usize)cols * sizeof(Cell));
                if (!next) return;
                w->cells = next; w->cell_cap = cols;
            }
            cols = termline_materialize(cold, w->cells, cols);
            cells = w->cells;
        }
        regex_search_row(s, &w->regex, (Cell *)cells, cols, row, &w->scratch);
    } else
#endif
    plain_search_row(s, &w->pattern, cells, cold, cols, row);
}

static void search_work_publish_screen(TermSearch *s, TermSearchWork *w) {
    /* Match retention keeps the same screen-first priority as synchronous
     * Find, including when a one-character query reaches the64K ceiling.
     * Collection order differs only for efficiency: both ranges are sorted,
     * so they concatenate in document order without a qsort. */
    i32 live = w->screen_matches.match_count;
    w->history_truncated = search_matches_full(s) || s->match_count > MAX_SEARCH_MATCHES - live ||
        (search_matches_full(&w->screen_matches) && w->history_start < w->screen_start);
    if (s->match_count > MAX_SEARCH_MATCHES - live) s->match_count = MAX_SEARCH_MATCHES - live;
    /* If visible matches later shrink, reclaim the vacated quota by
     * rescanning the last retained historical row plus the omitted suffix.
     * Starting from that row also recovers hits clipped partway through it. */
    w->retained_history_end = s->match_count > 0 ?
        s->row_origin + s->matches[s->match_count - 1].row : w->history_start;
    for (i32 i = 0; i < live; i++) {
        if (!ensure_match_capacity(s)) break;
        s->matches[s->match_count++] = w->screen_matches.matches[i];
    }
    w->screen_matches.match_count = 0;
}

bool terminal_search_step(Terminal *t, f64 budget_seconds, i32 max_rows) {
    if (!t || !t->search.active || !t->search.work) return false;
    TermSearch *s = &t->search;
    TermSearchWork *w = s->work;
    if (search_layout_changed(t, w)) {
        if (!w->pending) search_keep_current(t);
        search_work_schedule(t, true);
    }
    else if (w->revision != t->search_revision) {
        if (w->pending) w->refresh = true;
        else { search_keep_current(t); search_work_schedule(t, false); }
    }
    if (!w->pending || max_rows <= 0 || budget_seconds <= 0) return w->pending;
    f64 begin = platform_time_sec();
    i32 rows = 0;
    while (w->pending) {
        if (w->next_abs < t->sb_abs_base) w->next_abs = t->sb_abs_base;
        if (w->next_abs >= w->end_abs ||
            search_matches_full(w->scanning_screen ? &w->screen_matches : s)) {
            if (w->scanning_screen) {
                w->scanning_screen = false;
                w->next_abs = MAX(w->history_start, t->sb_abs_base);
                w->end_abs = w->screen_start;
                /* History cannot contribute when the live grid itself fills
                 * the retention limit. */
                if (search_matches_full(&w->screen_matches)) w->next_abs = w->end_abs;
                continue;
            }
            search_work_publish_screen(s, w);
            w->pending = false;
            s->current = s->match_count > 0 ? s->match_count - 1 : -1;
            if (w->keep_abs >= 0 && s->match_count > 0)
                terminal_search_set_current_at(t, w->keep_abs, w->keep_col);
            if (w->refresh) {
                search_work_schedule(t, false);
            }
            if (!w->pending) break;
        }
        if (rows >= max_rows || (rows > 0 && platform_time_sec() - begin >= budget_seconds)) break;
        search_work_row(t, w->next_abs++);
        rows++;
    }
    /* Progressive results stay usable while a long initial history scan is
     * pending. A preserved anchor is resolved again when the scan completes. */
    if (s->current < 0 && s->match_count > 0) s->current = s->match_count - 1;
    return w->pending;
}

void terminal_search_start(Terminal *t, const char *query, bool case_sensitive, bool use_regex) {
    if (!t || !query) return;
    char saved_query[256];
    snprintf(saved_query, sizeof saved_query, "%s", query);
    TermSearch *s = &t->search;
    search_work_free(s->work);
    free(s->matches);
    memset(s, 0, sizeof(*s));
    snprintf(s->query, sizeof(s->query), "%s", saved_query);
    s->case_sensitive = case_sensitive;
    s->use_regex = use_regex;
    s->active = true;
    s->anchored = true;
    s->row_origin = search_grid_origin(t);
    s->match_cap = 256;
    s->matches = malloc((size_t)s->match_cap * sizeof(TermSearchMatch));
    /* On OOM leave cap at 0 too, so the struct keeps its "no buffer => no
     * capacity" invariant instead of advertising 256 slots behind a NULL. */
    if (!s->matches) { s->match_cap = 0; s->active = false; return; }

    i32 qlen = (i32)strlen(s->query);
    if (qlen == 0) goto finish;

#ifndef PLATFORM_WIN32
    if (use_regex) {
        /* Compile POSIX extended regex */
        regex_t re;
        int flags = REG_EXTENDED;
        if (!case_sensitive) flags |= REG_ICASE;
        int rc = regcomp(&re, s->query, flags);
        if (rc != 0) {
            /* Invalid regex: no matches, but SAY so — the Find bar shows an
             * error instead of the "0" a valid query with no hits gets. */
            s->status = TERM_SEARCH_BAD_REGEX;
            goto finish;
        }

        RegexScratch scratch = {0};
        regex_search_screen(t, s, &re, &scratch);
        regex_search_scrollback(t, s, &re, &scratch);
        free(scratch.bytes);
        free(scratch.columns);
        regfree(&re);
    } else
#endif
    {
        SearchPattern pattern;
        if (search_pattern_build(&pattern, s->query, qlen, case_sensitive)) {
            plain_search_screen(t, s, &pattern);
            plain_search_scrollback(t, s, &pattern);
        }
    }

finish:
    /* Collection is done — the array is never appended to again (next/prev only
     * move the cursor), so hand the growth slack back before it goes resident. */
    search_shrink_to_fit(s);
    /* DOCUMENT order. The walks above visit the screen first and the
     * scrollback second, so the array came out as [screen][oldest..newest]
     * and "next" jumped from the bottom of the screen to the top of history.
     * Sorted by (row, col) — scrollback rows are negative — the last entry is
     * the newest match, which is where a search starts. */
    if (s->match_count > 1)
        qsort(s->matches, (size_t)s->match_count, sizeof(TermSearchMatch), search_match_cmp);
    s->current = s->match_count > 0 ? s->match_count - 1 : -1;
}

i32 terminal_search_visible_row(const Terminal *t, i32 index) {
    if (!t || index < 0 || index >= t->search.match_count) return -1;
    return terminal_search_match_row(t, &t->search.matches[index]) + t->scroll_offset;
}

bool terminal_search_set_current_at(Terminal *t, i64 abs_line, i32 col) {
    if (!t) return false;
    if (t->search.work) {
        t->search.work->keep_abs = abs_line;
        t->search.work->keep_col = col;
    }
    if (t->search.match_count == 0) return false;
    i32 best = -1;
    i64 best_d = 0;
    for (i32 i = 0; i < t->search.match_count; i++) {
        const TermSearchMatch *m = &t->search.matches[i];
        i64 abs = search_match_absolute(t, m);
        i64 d = abs - abs_line;
        if (d < 0) d = -d;
        d = d * 4096 + (m->col > col ? m->col - col : col - m->col);
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    t->search.current = best;
    return best >= 0 && best_d == 0;
}

void terminal_search_next(Terminal *t) {
    if (!t) return;
    TermSearch *s = &t->search;
    if (s->match_count == 0) return;
    s->current = (s->current + 1) % s->match_count;
    if (s->work) search_keep_current(t);
}

void terminal_search_prev(Terminal *t) {
    if (!t) return;
    TermSearch *s = &t->search;
    if (s->match_count == 0) return;
    s->current = (s->current - 1 + s->match_count) % s->match_count;
    if (s->work) search_keep_current(t);
}

void terminal_search_stop(Terminal *t) {
    if (!t) return;
    /* Full release: frees the match array and zeroes every counter/flag
     * (matches=NULL, match_count=match_cap=0, current=0, active=false). */
    search_work_free(t->search.work);
    free(t->search.matches);
    memset(&t->search, 0, sizeof(t->search));
}

bool terminal_search_is_highlighted(Terminal *t, i32 col, i32 row) {
    TermSearch *s = &t->search;
    if (!s->active) return false;
    for (i32 i = 0; i < s->match_count; i++) {
        TermSearchMatch *m = &s->matches[i];
        if (terminal_search_match_row(t, m) == row && col >= m->col && col < m->col + m->length)
            return true;
    }
    return false;
}

bool terminal_search_is_current(Terminal *t, i32 col, i32 row) {
    TermSearch *s = &t->search;
    if (!s->active || s->current < 0 || s->current >= s->match_count) return false;
    TermSearchMatch *m = &s->matches[s->current];
    return terminal_search_match_row(t, m) == row && col >= m->col && col < m->col + m->length;
}
