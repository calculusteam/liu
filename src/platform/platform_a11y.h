/*
 * Liu - accessibility text bridge (VoiceOver; AT-SPI2 on Linux)
 *
 * The terminal is drawn by the GPU into one view, which on its own is an empty
 * rectangle to the accessibility tree. The macOS view answers as a read-only
 * TEXT AREA, the X11 backend as an AT-SPI2 terminal object
 * (platform/atspi_bridge.c), and this header is the contract between them and
 * the app.
 *
 * The platform never learns what a Terminal is. It asks a per-window PROVIDER
 * for a snapshot of the focused pane's visible text, already laid out in the
 * units NSAccessibility speaks: UTF-16 code units (NSString indices). Every
 * index below is one of those, never a byte and never a column.
 *
 *   text          UTF-8, the visible rows of the focused pane. A row that ENDS
 *                 its line ends in '\n'; a row that soft-wraps into the next
 *                 joins it with no separator (the same rule a copy follows),
 *                 so a wrapped command reads as the one line it was typed as.
 *                 Trailing blanks are trimmed off rows that end a line.
 *   line_start16  one entry per VISUAL row plus a terminator: line L is
 *                 [line_start16[L], line_start16[L+1]). Lines are screen rows,
 *                 like NSTextView's soft-wrapped lines, so line N is always
 *                 row N — which is what makes frames computable.
 *   col16         the grid column each UTF-16 unit came from. A '\n' unit
 *                 carries its row's end column.
 *   line_end_col  the column just past each row's last emitted cell.
 *   line_text_end16  where each row's CONTENT ends: before its '\n', or at
 *                 the next line's start for a row that wraps.
 *
 * Ownership: every pointer the provider hands back stays valid until the next
 * call of ANY window's provider. The platform copies what it keeps.
 *
 * The helpers below are the ONE mapping between those tables and what the
 * NSAccessibility text methods answer, pure C so they run without a window.
 */
#ifndef LIU_PLATFORM_A11Y_H
#define LIU_PLATFORM_A11Y_H

#include "core/types.h"

typedef struct PlatformWindow PlatformWindow;

typedef struct {
    /* Change detection. Cheap to compute; filled in BOTH modes. `gen_value`
     * moves when the text may have changed, `gen_selection` when the
     * selection did. */
    u64         gen_value;
    u64         gen_selection;
    /* Which pane is described: it moves when the focus goes to another pane
     * or tab, and for nothing else; 0 when nothing is. An AT-SPI tree makes
     * the described pane a new object when it moves (atspi_bridge.c). */
    u64         source;

    /* Everything below is filled only when generation_only is false. */
    const char *text;
    u32         text_bytes;
    u32         length16;
    i32         line_count;
    const u32  *line_start16;   /* line_count + 1 entries */
    const u16  *col16;          /* length16 entries */
    const u16  *line_end_col;   /* line_count entries */
    const u32  *line_text_end16;/* line_count entries: content end, before '\n' */
    i32         cols;

    bool        has_selection;
    u32         sel_start16, sel_len16;  /* the on-screen part, as one span */
    const char *sel_text;                /* what Cmd+C would copy; may be NULL */
    u32         sel_text_bytes;

    bool        has_cursor;              /* cursor is on a visible row */
    u32         cursor16;
    i32         cursor_col;              /* its grid column (with has_cursor): past
                                          * the row's trimmed text it is where
                                          * the next character will appear */

    /* Cell geometry in the view's POINTS, origin top-left, y down. */
    bool        has_geometry;
    f32         grid_x, grid_y, cell_w, cell_h;

    const char *label;                   /* "Terminal — <tab title>" */
} PlatformA11yText;

/* Fill `out` for window `w`. With `generation_only`, only the two generation
 * fields and `source` are required (the platform's change detector calls it
 * that way, once per presented frame while a screen reader listens). Returns
 * false when there is
 * nothing to describe; the view then answers as an empty text area. Always
 * called on the main thread. */
typedef bool (*PlatformA11yProviderFn)(PlatformWindow *w, void *user,
                                       PlatformA11yText *out,
                                       bool generation_only);

/* Per window. NULL clears. On Linux the same snapshot feeds the AT-SPI2
 * bridge (atspi_bridge.c), in characters instead of UTF-16 units. */
void platform_window_set_a11y_provider(PlatformWindow *w,
                                       PlatformA11yProviderFn fn, void *user);

/* ---- mapping helpers ---------------------------------------------------- */

/* The line (visual row) holding UTF-16 index `idx`. The end of the text
 * belongs to the last line, as NSTextView answers. */
static inline i32 a11y_line_for_index(const PlatformA11yText *s, u32 idx) {
    if (!s || s->line_count <= 0 || !s->line_start16) return 0;
    if (idx >= s->length16) return s->line_count - 1;
    i32 lo = 0, hi = s->line_count - 1;
    while (lo < hi) {                     /* last line whose start <= idx */
        i32 mid = lo + (hi - lo + 1) / 2;
        if (s->line_start16[mid] <= idx) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* Line `line`'s range, including its '\n' when it has one. False when the
 * line does not exist. */
static inline bool a11y_range_for_line(const PlatformA11yText *s, i32 line,
                                       u32 *out_loc, u32 *out_len) {
    if (!s || !s->line_start16 || line < 0 || line >= s->line_count) return false;
    u32 a = s->line_start16[line], b = s->line_start16[line + 1];
    if (out_loc) *out_loc = a;
    if (out_len) *out_len = b > a ? b - a : 0;
    return true;
}

/* Column of UTF-16 index `idx`; the end of a line is its end column. */
static inline i32 a11y_col_for_index(const PlatformA11yText *s, u32 idx) {
    if (!s || s->line_count <= 0) return 0;
    i32 line = a11y_line_for_index(s, idx);
    u32 end  = s->line_text_end16 ? s->line_text_end16[line] : s->line_start16[line + 1];
    if (idx < s->length16 && idx < end && s->col16) return (i32)s->col16[idx];
    return s->line_end_col ? (i32)s->line_end_col[line] : 0;
}

/* The cell box a range covers: rows [*r0, *r1] inclusive, columns [*c0, *c1)
 * as the bounding box over every row touched. An empty range is one cell wide
 * so a caret has somewhere to be drawn. */
static inline void a11y_cells_for_range(const PlatformA11yText *s, u32 loc, u32 len,
                                        i32 *c0, i32 *r0, i32 *c1, i32 *r1) {
    i32 rs = a11y_line_for_index(s, loc);
    u32 last = (len > 0) ? loc + len - 1 : loc;
    if (len > 0 && last >= s->length16 && s->length16 > 0) last = s->length16 - 1;
    i32 re = a11y_line_for_index(s, last);
    i32 xs = a11y_col_for_index(s, loc);
    i32 xe;
    if (len == 0) {
        xe = xs + 1;
    } else if (rs == re) {
        u32 e    = loc + len;
        u32 tend = s->line_text_end16 ? s->line_text_end16[re] : s->line_start16[re + 1];
        xe = (e < tend && s->col16) ? (i32)s->col16[e]
                                    : (s->line_end_col ? (i32)s->line_end_col[re] : xs + 1);
        /* A '\n' alone, or a range ending on one: at least one cell. */
        if (xe <= xs) xe = xs + 1;
    } else {
        /* Several rows: the bounding box, from column 0 (every row after the
         * first starts there) to the widest row end touched. */
        i32 maxe = 0;
        for (i32 r = rs; r <= re; r++) {
            i32 ec = s->line_end_col ? (i32)s->line_end_col[r] : 0;
            if (ec > maxe) maxe = ec;
        }
        xs = 0;
        xe = maxe > 0 ? maxe : 1;
    }
    if (c0) *c0 = xs;
    if (r0) *r0 = rs;
    if (c1) *c1 = xe;
    if (r1) *r1 = re;
}

/* The UTF-16 index at cell (col, row): the first unit of the cell covering
 * `col`, or the row's content end when `col` is past it. A row out of range
 * clamps. */
static inline u32 a11y_index_for_cell(const PlatformA11yText *s, i32 col, i32 row) {
    if (!s || s->line_count <= 0 || !s->line_start16) return 0;
    if (row < 0) row = 0;
    if (row >= s->line_count) row = s->line_count - 1;
    u32 a = s->line_start16[row];
    u32 e = s->line_text_end16 ? s->line_text_end16[row] : s->line_start16[row + 1];
    if (col < 0) col = 0;
    u32 hit = e;
    for (u32 i = a; i < e; i++) {
        if ((i32)s->col16[i] <= col) hit = i;
        else break;
    }
    if (hit == e) return e;
    if (s->line_end_col && col >= (i32)s->line_end_col[row]) return e;
    /* `hit` is the LAST unit starting at or before `col`; a cluster or a
     * surrogate pair spans several units of one cell, so step back to the
     * first of them. */
    while (hit > a && s->col16[hit - 1] == s->col16[hit]) hit--;
    return hit;
}

#endif /* LIU_PLATFORM_A11Y_H */
