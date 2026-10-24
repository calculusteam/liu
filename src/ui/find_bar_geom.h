/*
 * Liu — the terminal Find bar's (Cmd+F) geometry, in one place.
 *
 * The renderer paints the bar and the hit test decides what a press on it
 * means (the match-case and regex toggles, the close keycap, the body); both
 * read these rects. Every part gets its own rectangle, laid out right to
 * left — close, regex, match case, counter — and the query takes what is
 * left, so a long query never runs under the "3/12" counter. The rects are
 * DISJOINT by construction.
 *
 * Pure arithmetic, no AppState. Conventions: framebuffer pixels; `cw`/`ch` are
 * the UI text cell (8x16 pt times dpi_scale in ui.c), `dpi` the backing scale.
 */
#ifndef UI_FIND_BAR_GEOM_H
#define UI_FIND_BAR_GEOM_H

#include "core/types.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define FIND_BAR_W_PT            480.0f
#define FIND_BAR_H_PT             32.0f
#define FIND_BAR_TOP_GAP_PT        8.0f   /* below the tab bar */
#define FIND_BAR_SIDE_MIN_PT      16.0f   /* the bar keeps this off each window edge */
#define FIND_BAR_PAD_L_PT         12.0f
#define FIND_BAR_PAD_R_PT          8.0f
#define FIND_BAR_GAP_PT            8.0f   /* between query / counter / toggles / close */
#define FIND_BAR_TOGGLE_GAP_PT     4.0f   /* between the two toggles */
#define FIND_BAR_TOGGLE_PAD_PT     6.0f   /* label inset inside a toggle */
#define FIND_BAR_KEYCAP_PAD_PT     7.0f   /* draw_keycap's own inset (ui.c) */
#define FIND_BAR_QUERY_MIN_CELLS   4

#define FIND_BAR_LABEL        "Find: "
#define FIND_BAR_CASE_LABEL   "Aa"
#define FIND_BAR_REGEX_LABEL  ".*"
#define FIND_BAR_CLOSE_LABEL  "Esc"
#define FIND_BAR_ERR_REGEX    "Invalid regex"
/* The widest counter search.c can produce: MAX_SEARCH_MATCHES is 65536. The
 * counter's slot is reserved at the wider of this and the error string, so
 * walking the hits (or typing) never moves the query's right edge. */
#define FIND_BAR_COUNTER_WIDEST "65536/65536"

typedef struct { f32 x, y, w, h; } FindRect;

typedef enum {
    FIND_PART_NONE = 0,
    FIND_PART_BODY,        /* anywhere else on the bar: consumed, does nothing */
    FIND_PART_CASE,        /* "Aa" — match case */
    FIND_PART_REGEX,       /* ".*" — regular expression */
    FIND_PART_CLOSE,       /* the Esc keycap */
} FindBarPart;

typedef struct {
    FindRect bar;
    FindRect label;        /* "Find: " — w == 0 when squeezed out */
    FindRect query;        /* exactly query_cells * cw wide */
    FindRect counter;      /* w == 0 when squeezed out */
    FindRect case_btn;
    FindRect regex_btn;
    FindRect close;
    i32      query_cells;
    i32      counter_cells;
    f32      text_y;       /* baseline row for every text run on the bar */
} FindBarGeom;

static inline i32 find_bar_counter_reserve_cells(void) {
    i32 a = (i32)strlen(FIND_BAR_COUNTER_WIDEST);
    i32 b = (i32)strlen(FIND_BAR_ERR_REGEX);
    return a > b ? a : b;
}

/* Where the bar rests (no open animation applied): centred, below the tab bar,
 * never closer than FIND_BAR_SIDE_MIN_PT to either window edge. */
static inline FindRect find_bar_rest_rect(f32 fb_width, f32 top, f32 dpi) {
    FindRect b;
    b.w = FIND_BAR_W_PT * dpi;
    f32 room = fb_width - 2.0f * FIND_BAR_SIDE_MIN_PT * dpi;
    if (b.w > room) b.w = room;
    if (b.w < 0.0f) b.w = 0.0f;
    b.h = FIND_BAR_H_PT * dpi;
    b.x = (fb_width - b.w) * 0.5f;
    b.y = top + FIND_BAR_TOP_GAP_PT * dpi;
    return b;
}

static inline FindBarGeom find_bar_layout(FindRect bar, f32 dpi, f32 cw, f32 ch) {
    FindBarGeom g;
    memset(&g, 0, sizeof g);
    g.bar = bar;
    if (cw <= 0.0f) cw = 1.0f;
    g.text_y = bar.y + (bar.h - ch) * 0.5f;
    /* Controls are the keycap's box: one text row plus its 3.5 pt margins. */
    f32 ctl_h = ch + 2.0f * 3.5f * dpi;
    f32 ctl_y = g.text_y - 3.5f * dpi;
    f32 gap   = FIND_BAR_GAP_PT * dpi;
    f32 left  = bar.x + FIND_BAR_PAD_L_PT * dpi;

    /* Right to left: close, regex, match case. */
    f32 close_w  = (f32)strlen(FIND_BAR_CLOSE_LABEL) * cw + 2.0f * FIND_BAR_KEYCAP_PAD_PT * dpi;
    f32 toggle_w = 2.0f * cw + 2.0f * FIND_BAR_TOGGLE_PAD_PT * dpi;
    g.close     = (FindRect){ bar.x + bar.w - FIND_BAR_PAD_R_PT * dpi - close_w, ctl_y, close_w, ctl_h };
    g.regex_btn = (FindRect){ g.close.x - gap - toggle_w, ctl_y, toggle_w, ctl_h };
    g.case_btn  = (FindRect){ g.regex_btn.x - FIND_BAR_TOGGLE_GAP_PT * dpi - toggle_w,
                              ctl_y, toggle_w, ctl_h };
    /* A bar too narrow for its controls drops them rather than overlapping
     * the label: the close keycap goes last, since Esc does the same. */
    if (g.case_btn.x < left) {
        g.case_btn.w = g.regex_btn.w = 0.0f;
        g.case_btn.x = g.regex_btn.x = g.close.x;
        if (g.close.x < left) { g.close.w = 0.0f; g.close.x = bar.x + bar.w; }
    }
    f32 ctl_left = g.case_btn.w > 0.0f ? g.case_btn.x
                 : (g.close.w > 0.0f ? g.close.x : bar.x + bar.w);

    /* The counter's slot: fixed width, right-aligned text inside it. */
    g.counter_cells = find_bar_counter_reserve_cells();
    f32 counter_w = (f32)g.counter_cells * cw;
    g.counter = (FindRect){ ctl_left - gap - counter_w, g.text_y, counter_w, ch };

    f32 label_w = (f32)strlen(FIND_BAR_LABEL) * cw;
    g.label = (FindRect){ left, g.text_y, label_w, ch };

    /* The query takes what is left: counter squeezed out first, then the
     * label, so a narrow window still has somewhere to type. */
    f32 q_x = left + label_w;
    f32 q_right = g.counter.x - gap;
    if ((q_right - q_x) < (f32)FIND_BAR_QUERY_MIN_CELLS * cw) {
        g.counter_cells = 0;
        g.counter.w = 0.0f;
        g.counter.x = ctl_left;
        q_right = ctl_left - gap;
    }
    if ((q_right - q_x) < (f32)FIND_BAR_QUERY_MIN_CELLS * cw) {
        g.label.w = 0.0f;
        q_x = left;
    }
    f32 q_w = q_right - q_x;
    i32 cells = q_w > 0.0f ? (i32)(q_w / cw) : 0;
    if (cells < 0) cells = 0;
    g.query_cells = cells;
    g.query = (FindRect){ q_x, g.text_y, (f32)cells * cw, ch };
    return g;
}

/* Translate every part by (dx, dy) — the open animation slides the bar. */
static inline void find_bar_geom_offset(FindBarGeom *g, f32 dx, f32 dy) {
    FindRect *rs[] = { &g->bar, &g->label, &g->query, &g->counter,
                       &g->case_btn, &g->regex_btn, &g->close };
    for (usize i = 0; i < sizeof rs / sizeof rs[0]; i++) { rs[i]->x += dx; rs[i]->y += dy; }
    g->text_y += dy;
}

static inline bool find_rect_contains(FindRect r, f32 x, f32 y) {
    return r.w > 0.0f && r.h > 0.0f &&
           x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static inline bool find_rect_overlaps(FindRect a, FindRect b) {
    if (a.w <= 0.0f || b.w <= 0.0f || a.h <= 0.0f || b.h <= 0.0f) return false;
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static inline FindBarPart find_bar_part_at(const FindBarGeom *g, f32 x, f32 y) {
    if (!find_rect_contains(g->bar, x, y)) return FIND_PART_NONE;
    if (find_rect_contains(g->case_btn, x, y))  return FIND_PART_CASE;
    if (find_rect_contains(g->regex_btn, x, y)) return FIND_PART_REGEX;
    if (find_rect_contains(g->close, x, y))     return FIND_PART_CLOSE;
    return FIND_PART_BODY;
}

/* The counter's text: "3/12", "0" for a valid query with no hits, or the
 * error when the pattern did not compile. `*is_error` says which colour. */
static inline void find_bar_counter_text(char *out, usize cap, bool bad_regex,
                                         i32 current, i32 count, bool *is_error) {
    if (!out || cap == 0) return;
    if (is_error) *is_error = bad_regex;
    if (bad_regex)       { snprintf(out, cap, "%s", FIND_BAR_ERR_REGEX); return; }
    if (count > 0)       snprintf(out, cap, "%d/%d", current + 1, count);
    else                 snprintf(out, cap, "0");
}

#endif /* UI_FIND_BAR_GEOM_H */
