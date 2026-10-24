/*
 * sftp_layout.h — where everything in the SFTP tab is, in ONE place.
 *
 * The tab is two panes side by side, the local disk on the left and the host
 * on the right, with a divider between them. Each pane is the same stack:
 *
 *   header   the side's tile and name (Local / user@host), Filter, Actions
 *   nav      back, forward, the folder as a breadcrumb
 *   columns  Name | Date Modified | Size | Kind | Permissions (sortable)
 *   list     one row per entry, the file browser's row height
 *   footer   what is listed and selected, and what the pane has to say
 *
 * The painter (ui/sftp_tab.c), the click (sftp_hit) and the driver's state
 * dump all read these rectangles; none of them derives a number of its own.
 *
 * Framebuffer pixels throughout; `dpi` turns the *_PT points into them, and
 * `cw` is the UI text step (8 pt × dpi) every label advances by.
 *
 * Pure: no renderer, no AppState.
 */
#ifndef LIU_UI_SFTP_LAYOUT_H
#define LIU_UI_SFTP_LAYOUT_H

#include "core/types.h"
#include "ui/layout.h"
#include "ui/sftp_view.h"
#include <math.h>
#include <string.h>

#define SFTP_HDR_PT          44.0f   /* pane header */
#define SFTP_NAV_PT          32.0f   /* back / forward / breadcrumb */
#define SFTP_COLS_PT         26.0f   /* column header */
#define SFTP_FOOTER_PT       24.0f
#define SFTP_PAD_PT          FB_ROW_PAD_PT   /* the rail every row hangs off */
#define SFTP_TILE_PT         26.0f   /* the side's icon tile */
#define SFTP_TILE_ICON_PT    15.0f
#define SFTP_TILE_GAP_PT     10.0f   /* tile -> name */
#define SFTP_BTN_H_PT        26.0f   /* Filter / Actions */
#define SFTP_BTN_PAD_PT      10.0f
#define SFTP_BTN_ICON_PT     14.0f
#define SFTP_BTN_GAP_PT       6.0f
#define SFTP_BTN_RADIUS_PT    6.0f
#define SFTP_FILTER_W_PT    200.0f   /* the field once it is open */
#define SFTP_NAV_BTN_PT      26.0f   /* back / forward hit boxes, tall */
#define SFTP_NAV_STEP_PT     22.0f   /* back -> forward: each box's width */
#define SFTP_NAV_CRUMB_GAP_PT 4.0f   /* forward's box -> the first crumb's chip */
/* Lucide's chevrons stroke x 9..15 of their 24 units at width 2, so their ink
 * is the box's middle third (8..16): the left 8/24 of the box is empty. */
#define SFTP_CHEVRON_INK_LEFT (8.0f / 24.0f)
#define SFTP_CRUMB_ICON_PT   14.0f
#define SFTP_CRUMB_GAP_PT     6.0f   /* folder glyph -> its name */
#define SFTP_CRUMB_PAD_PT     4.0f   /* each side of a crumb's hover chip */
#define SFTP_CRUMB_SEP_PT    16.0f   /* the chevron between two crumbs */
#define SFTP_COL_DATE_PT    150.0f
#define SFTP_COL_SIZE_PT     84.0f
#define SFTP_COL_KIND_PT    124.0f
#define SFTP_COL_PERMS_PT   104.0f
#define SFTP_COL_NAME_MIN_PT 190.0f
#define SFTP_COL_PAD_PT      12.0f   /* a column's text starts this far in */
#define SFTP_LIST_RIGHT_PT   10.0f   /* room for the overlay scrollbar */
#define SFTP_PANE_MIN_PT    300.0f
#define SFTP_DIVIDER_GRAB_PT  5.0f
#define SFTP_RECONNECT_W_PT 132.0f
#define SFTP_RECONNECT_H_PT  30.0f
#define SFTP_NOTICE_LINES       3      /* message lines under the title, at most */
#define SFTP_NOTICE_GAP_PT      6.0f   /* the title -> its message */
#define SFTP_NOTICE_LEAD_PT     3.0f   /* one message line -> the next */
#define SFTP_NOTICE_BTN_GAP_PT 16.0f   /* the message -> the Reconnect button */
#define SFTP_NOTICE_SIDE_PT    24.0f   /* free room each side of the text */

typedef struct { f32 x, y, w, h; } SftpRect;

/* What a pane's list shows instead of rows: a title, up to SFTP_NOTICE_LINES
 * lines of message and, while the host is down, the Reconnect button. */
typedef struct {
    f32 title_y;              /* the title's top */
    f32 msg_y;                /* the first message line's top */
    f32 step;                 /* one message line to the next */
    i32 lines;                /* message lines laid out */
    i32 cols;                 /* the room a line has, in cells */
} SftpNotice;

static inline bool sftp_rect_has(SftpRect r, f32 px, f32 py) {
    return r.w > 0.0f && r.h > 0.0f && px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

typedef struct {
    SftpRect pane, header, tile, title;
    SftpRect filter;          /* the Filter button, or the open field */
    SftpRect filter_clear;    /* the field's ✕ (open field with text only) */
    SftpRect actions;
    SftpRect nav, back, forward, crumbs;
    SftpRect cols, list, footer;
    SftpRect col[SFTP_COL_COUNT];
    bool     col_shown[SFTP_COL_COUNT];
    SftpRect reconnect;       /* the remote pane's button while it is down */
    SftpNotice notice;
    f32      row_h;
} SftpPaneGeom;

typedef struct {
    SftpPaneGeom p[2];        /* 0 = local (left), 1 = remote (right) */
    SftpRect     divider;     /* the hairline */
    SftpRect     grab;        /* where a press takes the divider */
    f32          ratio;       /* the left pane's share, as laid out */
} SftpGeom;

/* The left pane's share for `want` (0 = an even split) in a `w` wide tab:
 * neither pane narrower than SFTP_PANE_MIN_PT while there is room for two. */
static inline f32 sftp_ratio_clamp(f32 want, f32 w, f32 dpi) {
    if (!(want > 0.0f && want < 1.0f)) want = 0.5f;
    f32 min = SFTP_PANE_MIN_PT * dpi;
    if (w < 2.0f * min) return 0.5f;
    f32 lo = min / w, hi = 1.0f - min / w;
    return want < lo ? lo : want > hi ? hi : want;
}

/* Columns, right to left from the list's right edge: the name takes what is
 * left, and when that would be narrower than SFTP_COL_NAME_MIN_PT the least
 * needed column goes first — Permissions, then Kind, then the date, then the
 * size. */
static inline void sftp_columns(SftpPaneGeom *g, f32 dpi) {
    static const SftpCol drop_order[] = { SFTP_COL_PERMS, SFTP_COL_KIND, SFTP_COL_DATE, SFTP_COL_SIZE };
    static const SftpCol left_to_right[] = { SFTP_COL_DATE, SFTP_COL_SIZE, SFTP_COL_KIND, SFTP_COL_PERMS };
    f32 width[SFTP_COL_COUNT] = { 0, SFTP_COL_DATE_PT * dpi, SFTP_COL_SIZE_PT * dpi,
                                  SFTP_COL_KIND_PT * dpi, SFTP_COL_PERMS_PT * dpi };
    bool shown[SFTP_COL_COUNT] = { true, true, true, true, true };
    f32 right = g->list.x + g->list.w - SFTP_LIST_RIGHT_PT * dpi;
    f32 name_min = SFTP_COL_NAME_MIN_PT * dpi;
    for (i32 d = 0; d <= 4; d++) {
        f32 used = 0.0f;
        for (i32 c = 1; c < SFTP_COL_COUNT; c++) if (shown[c]) used += width[c];
        if (right - g->list.x - used >= name_min || d == 4) break;
        shown[drop_order[d]] = false;
    }
    f32 x = right;
    for (i32 k = 3; k >= 0; k--) {
        SftpCol c = left_to_right[k];
        if (!shown[c]) { g->col[c] = (SftpRect){ 0, 0, 0, 0 }; continue; }
        x -= width[c];
        g->col[c] = (SftpRect){ x, g->cols.y, width[c], g->cols.h };
    }
    g->col[SFTP_COL_NAME] = (SftpRect){ g->list.x, g->cols.y, fmaxf(0.0f, x - g->list.x), g->cols.h };
    memcpy(g->col_shown, shown, sizeof shown);
}

/* The width of the Filter button (closed) and the Actions button. */
static inline f32 sftp_filter_btn_w(f32 cw, f32 dpi) {
    return (2.0f * SFTP_BTN_PAD_PT + SFTP_BTN_ICON_PT + SFTP_BTN_GAP_PT) * dpi + 6.0f * cw;
}
static inline f32 sftp_actions_btn_w(f32 cw, f32 dpi) {
    return (2.0f * SFTP_BTN_PAD_PT + SFTP_BTN_GAP_PT + 12.0f) * dpi + 7.0f * cw;
}

/* The notice as ONE stack, a little above the list's middle: the title, the
 * `lines` lines its message took, and the button (`button`) under those. The
 * button is placed after the lines and never at a fixed offset from the
 * title, so a message cannot run under it; with no button its rect is empty.
 * The painter and sftp_hit both read what this leaves in `g`. */
static inline void sftp_pane_notice(SftpPaneGeom *g, f32 dpi, f32 cw, f32 ch, i32 lines, bool button) {
    if (lines < 0) lines = 0;
    if (lines > SFTP_NOTICE_LINES) lines = SFTP_NOTICE_LINES;
    f32 step = ch + SFTP_NOTICE_LEAD_PT * dpi;
    f32 msg_h = lines > 0 ? SFTP_NOTICE_GAP_PT * dpi + ch + (f32)(lines - 1) * step : 0.0f;
    f32 rw = SFTP_RECONNECT_W_PT * dpi, rh = SFTP_RECONNECT_H_PT * dpi;
    f32 btn_h = button ? SFTP_NOTICE_BTN_GAP_PT * dpi + rh : 0.0f;
    f32 top = g->list.y + floorf(fmaxf(0.0f, g->list.h - (ch + msg_h + btn_h)) * 0.42f);
    g->notice.title_y = top;
    g->notice.msg_y = top + ch + SFTP_NOTICE_GAP_PT * dpi;
    g->notice.step = step;
    g->notice.lines = lines;
    f32 room = g->list.w - 2.0f * SFTP_NOTICE_SIDE_PT * dpi;
    g->notice.cols = cw > 0.0f && room > cw ? (i32)(room / cw) : 1;
    g->reconnect = button ? (SftpRect){ g->list.x + floorf((g->list.w - rw) * 0.5f),
                                        top + ch + msg_h + SFTP_NOTICE_BTN_GAP_PT * dpi, rw, rh }
                          : (SftpRect){ 0, 0, 0, 0 };
}

static inline void sftp_pane_geom(SftpPaneGeom *g, f32 x, f32 y, f32 w, f32 h,
                                  f32 dpi, f32 cw, f32 ch, bool filter_open, bool filter_text) {
    memset(g, 0, sizeof *g);
    g->pane = (SftpRect){ x, y, w, h };
    f32 hdr = SFTP_HDR_PT * dpi, nav = SFTP_NAV_PT * dpi, cols = SFTP_COLS_PT * dpi;
    f32 foot = SFTP_FOOTER_PT * dpi;
    if (hdr + nav + cols + foot > h) foot = 0.0f;   /* a very short window keeps the list */
    g->header = (SftpRect){ x, y, w, hdr };
    g->nav    = (SftpRect){ x, y + hdr, w, nav };
    g->cols   = (SftpRect){ x, y + hdr + nav, w, cols };
    g->footer = (SftpRect){ x, y + h - foot, w, foot };
    g->list   = (SftpRect){ x, g->cols.y + cols, w, fmaxf(0.0f, g->footer.y - (g->cols.y + cols)) };
    g->row_h  = FB_ENTRY_H(ch, dpi);

    f32 pad = SFTP_PAD_PT * dpi;
    f32 tile = SFTP_TILE_PT * dpi;
    g->tile = (SftpRect){ x + pad, y + floorf((hdr - tile) * 0.5f), tile, tile };

    /* Right side: Actions at the edge, Filter (or its field) before it. */
    f32 bh = SFTP_BTN_H_PT * dpi, by = y + floorf((hdr - bh) * 0.5f);
    f32 aw = sftp_actions_btn_w(cw, dpi);
    g->actions = (SftpRect){ x + w - pad - aw, by, aw, bh };
    f32 fw = filter_open ? SFTP_FILTER_W_PT * dpi : sftp_filter_btn_w(cw, dpi);
    f32 room_left = g->actions.x - SFTP_BTN_GAP_PT * dpi - (g->tile.x + tile + SFTP_TILE_GAP_PT * dpi + 6.0f * cw);
    if (fw > room_left) fw = fmaxf(sftp_filter_btn_w(cw, dpi) * 0.6f, room_left);
    g->filter = (SftpRect){ g->actions.x - SFTP_BTN_GAP_PT * dpi - fw, by, fw, bh };
    if (filter_open && filter_text) {
        f32 cx = bh;
        g->filter_clear = (SftpRect){ g->filter.x + g->filter.w - cx, by, cx, bh };
    }
    f32 tx = g->tile.x + tile + SFTP_TILE_GAP_PT * dpi;
    g->title = (SftpRect){ tx, y, fmaxf(0.0f, g->filter.x - SFTP_BTN_GAP_PT * dpi - tx), hdr };

    /* Nav: back and forward, the breadcrumb after them. The back arrow's INK
     * is on the rail the tile above and the rows below hang off; the painter
     * centres the arrow (FB_HDR_ICON_PT, snapped) in its box. */
    f32 nb = SFTP_NAV_BTN_PT * dpi, ny = g->nav.y + floorf((nav - nb) * 0.5f);
    f32 step = SFTP_NAV_STEP_PT * dpi, isz = ui_icon_box_px(FB_HDR_ICON_PT, dpi);
    f32 nx = floorf(x + pad - floorf((step - isz) * 0.5f) - isz * SFTP_CHEVRON_INK_LEFT + 0.5f);
    g->back    = (SftpRect){ nx, ny, step, nb };
    g->forward = (SftpRect){ nx + step, ny, step, nb };
    f32 cx0 = g->forward.x + step + SFTP_NAV_CRUMB_GAP_PT * dpi;
    g->crumbs  = (SftpRect){ cx0, g->nav.y, fmaxf(0.0f, x + w - pad - cx0), nav };

    sftp_columns(g, dpi);

    /* Until the tab says what its notice holds: one line and the button. */
    sftp_pane_notice(g, dpi, cw, ch, 1, true);
}

/* The whole tab. `ratio` is the left pane's share (0 = even). */
static inline void sftp_geom(SftpGeom *G, f32 x, f32 y, f32 w, f32 h, f32 ratio,
                             f32 dpi, f32 cw, f32 ch,
                             const bool filter_open[2], const bool filter_text[2]) {
    memset(G, 0, sizeof *G);
    G->ratio = sftp_ratio_clamp(ratio, w, dpi);
    f32 line = fmaxf(1.0f, CHROME_LINE_PT * dpi);
    f32 lw = floorf(w * G->ratio);
    sftp_pane_geom(&G->p[0], x, y, lw, h, dpi, cw, ch, filter_open && filter_open[0], filter_text && filter_text[0]);
    sftp_pane_geom(&G->p[1], x + lw + line, y, fmaxf(0.0f, w - lw - line), h, dpi, cw, ch,
                   filter_open && filter_open[1], filter_text && filter_text[1]);
    G->divider = (SftpRect){ x + lw, y, line, h };
    f32 grab = SFTP_DIVIDER_GRAB_PT * dpi;
    G->grab = (SftpRect){ x + lw - grab, y, 2.0f * grab + line, h };
}

/* The breadcrumb, laid out. `cols(text, len)` is the width of a name in UI
 * cells (the painter's own measure, so a click lands on what was drawn). A
 * path too long for the row loses its FIRST folders to one "…" crumb that
 * goes to the deepest folder it hides; the current folder always shows, cut
 * short when it alone is too wide. */
typedef i32 (*SftpColsFn)(const char *text, usize len);

#define SFTP_CRUMB_BOXES 24

typedef struct {
    i32      crumb;           /* index into the crumbs, or -1 for the "…" */
    i32      go;              /* the crumb a click goes to */
    SftpRect box;             /* the hover chip and the click target */
    f32      text_x;          /* where the name starts */
    i32      text_cells;      /* how much of it fits */
    bool     clipped;
} SftpCrumbBox;

typedef struct {
    SftpCrumb    crumb[SFTP_CRUMB_MAX];
    i32          count;
    SftpCrumbBox box[SFTP_CRUMB_BOXES];
    i32          boxes;
} SftpCrumbRow;

static inline void sftp_crumb_layout(SftpCrumbRow *row, const char *path, SftpRect area,
                                     f32 dpi, f32 cw, SftpColsFn cols) {
    memset(row, 0, sizeof *row);
    if (!path || !path[0] || area.w <= 0.0f) return;
    row->count = sftp_crumbs(path, row->crumb, SFTP_CRUMB_MAX);
    if (row->count <= 0) return;
    f32 icon = SFTP_CRUMB_ICON_PT * dpi, gap = SFTP_CRUMB_GAP_PT * dpi;
    f32 pad = SFTP_CRUMB_PAD_PT * dpi, sep = SFTP_CRUMB_SEP_PT * dpi;
    f32 chip_h = fminf(area.h - 6.0f * dpi, 24.0f * dpi);
    f32 chip_y = area.y + floorf((area.h - chip_h) * 0.5f);

    i32 cells[SFTP_CRUMB_MAX];
    for (i32 i = 0; i < row->count; i++) {
        /* The root reads as "/", which the folder glyph alone says. */
        const SftpCrumb *c = &row->crumb[i];
        cells[i] = (c->start == 0 && c->len == 1 && path[0] == '/') ? 1
                 : (cols ? cols(path + c->start, c->len) : (i32)c->len);
        if (cells[i] < 1) cells[i] = 1;
    }
    #define SFTP_CRUMB_W(n) (pad + icon + gap + (f32)(n) * cw + pad)
    /* How many trailing crumbs fit, with a "…" in front when some do not. */
    f32 ell_w = pad + 1.0f * cw + pad;
    i32 first = row->count - 1;
    f32 used = SFTP_CRUMB_W(cells[first]);
    while (first > 0) {
        f32 next = SFTP_CRUMB_W(cells[first - 1]) + sep;
        f32 need = used + next + (first - 1 > 0 ? ell_w + sep : 0.0f);
        if (need > area.w) break;
        used += next;
        first--;
    }
    f32 x = area.x;
    if (first > 0 && row->boxes < SFTP_CRUMB_BOXES) {
        SftpCrumbBox *b = &row->box[row->boxes++];
        b->crumb = -1;
        b->go = first - 1;
        b->box = (SftpRect){ x, chip_y, ell_w, chip_h };
        b->text_x = x + pad;
        b->text_cells = 1;
        x += ell_w + sep;
    }
    for (i32 i = first; i < row->count && row->boxes < SFTP_CRUMB_BOXES; i++) {
        SftpCrumbBox *b = &row->box[row->boxes++];
        f32 bw = SFTP_CRUMB_W(cells[i]);
        i32 fit = cells[i];
        if (x + bw > area.x + area.w) {
            f32 room = area.x + area.w - x - (pad + icon + gap + pad);
            fit = room > 0.0f ? (i32)(room / cw) : 0;
            if (fit < cells[i]) b->clipped = true;
            if (fit < 1) fit = 1;
            bw = SFTP_CRUMB_W(fit);
        }
        b->crumb = i;
        b->go = i;
        b->box = (SftpRect){ x, chip_y, bw, chip_h };
        b->text_x = x + pad + icon + gap;
        b->text_cells = fit;
        x += bw + sep;
    }
    #undef SFTP_CRUMB_W
}

/* The crumb box a point is on, or -1. */
static inline i32 sftp_crumb_at(const SftpCrumbRow *row, f32 px, f32 py) {
    for (i32 i = 0; i < row->boxes; i++)
        if (sftp_rect_has(row->box[i].box, px, py)) return i;
    return -1;
}

/* ---- the click --------------------------------------------------------- */

typedef enum {
    SFTP_HIT_NONE = 0,
    SFTP_HIT_HEADER,          /* the header's empty space */
    SFTP_HIT_FILTER,          /* the button, or the open field */
    SFTP_HIT_FILTER_CLEAR,
    SFTP_HIT_ACTIONS,
    SFTP_HIT_BACK,
    SFTP_HIT_FORWARD,
    SFTP_HIT_CRUMB,           /* index: the crumb to go to */
    SFTP_HIT_NAV,             /* the nav row's empty space */
    SFTP_HIT_COLUMN,          /* index: an SftpCol */
    SFTP_HIT_ROW,             /* index: a view row */
    SFTP_HIT_LIST,            /* the list below its last row */
    SFTP_HIT_RECONNECT,
    SFTP_HIT_FOOTER,
    SFTP_HIT_DIVIDER,
} SftpHitKind;

typedef struct {
    SftpHitKind kind;
    i32         pane;         /* 0 / 1; -1 for the divider and none */
    i32         index;
} SftpHit;

/* The view row at y in pane geometry `g` scrolled by `scroll_px`, or -1. */
static inline i32 sftp_row_at(const SftpPaneGeom *g, f32 py, f32 scroll_px, i32 rows) {
    if (g->row_h <= 0.0f || py < g->list.y || py >= g->list.y + g->list.h) return -1;
    i32 i = (i32)floorf((py - g->list.y + scroll_px) / g->row_h);
    return (i >= 0 && i < rows) ? i : -1;
}

/* What a press at (px, py) is on. `scroll_px`/`rows` per pane; `crumbs` the
 * breadcrumb rows as laid out for each pane (NULL: crumbs not hit-tested);
 * `reconnect_shown` whether the remote pane shows its button. */
static inline SftpHit sftp_hit(const SftpGeom *G, f32 px, f32 py,
                               const f32 scroll_px[2], const i32 rows[2],
                               const SftpCrumbRow *crumbs, bool reconnect_shown) {
    SftpHit h = { SFTP_HIT_NONE, -1, -1 };
    if (sftp_rect_has(G->grab, px, py)) { h.kind = SFTP_HIT_DIVIDER; return h; }
    for (i32 p = 0; p < 2; p++) {
        const SftpPaneGeom *g = &G->p[p];
        if (!sftp_rect_has(g->pane, px, py)) continue;
        h.pane = p;
        if (sftp_rect_has(g->header, px, py)) {
            if (sftp_rect_has(g->filter_clear, px, py)) h.kind = SFTP_HIT_FILTER_CLEAR;
            else if (sftp_rect_has(g->filter, px, py)) h.kind = SFTP_HIT_FILTER;
            else if (sftp_rect_has(g->actions, px, py)) h.kind = SFTP_HIT_ACTIONS;
            else h.kind = SFTP_HIT_HEADER;
            return h;
        }
        if (sftp_rect_has(g->nav, px, py)) {
            if (sftp_rect_has(g->back, px, py)) h.kind = SFTP_HIT_BACK;
            else if (sftp_rect_has(g->forward, px, py)) h.kind = SFTP_HIT_FORWARD;
            else {
                i32 b = crumbs ? sftp_crumb_at(&crumbs[p], px, py) : -1;
                if (b >= 0) { h.kind = SFTP_HIT_CRUMB; h.index = crumbs[p].box[b].go; }
                else h.kind = SFTP_HIT_NAV;
            }
            return h;
        }
        if (sftp_rect_has(g->cols, px, py)) {
            for (i32 c = 0; c < SFTP_COL_COUNT; c++)
                if (g->col_shown[c] && sftp_rect_has(g->col[c], px, py)) { h.kind = SFTP_HIT_COLUMN; h.index = c; return h; }
            h.kind = SFTP_HIT_COLUMN;
            h.index = SFTP_COL_NAME;
            return h;
        }
        if (sftp_rect_has(g->footer, px, py)) { h.kind = SFTP_HIT_FOOTER; return h; }
        if (sftp_rect_has(g->list, px, py)) {
            if (p == 1 && reconnect_shown && sftp_rect_has(g->reconnect, px, py)) {
                h.kind = SFTP_HIT_RECONNECT;
                return h;
            }
            i32 r = sftp_row_at(g, py, scroll_px ? scroll_px[p] : 0.0f, rows ? rows[p] : 0);
            h.kind = r >= 0 ? SFTP_HIT_ROW : SFTP_HIT_LIST;
            h.index = r;
            return h;
        }
        h.kind = SFTP_HIT_HEADER;
        return h;
    }
    return h;
}

/* The scroll range of a pane's list for `rows` rows. */
static inline f32 sftp_scroll_max(const SftpPaneGeom *g, i32 rows) {
    f32 content = (f32)rows * g->row_h;
    return content > g->list.h ? content - g->list.h : 0.0f;
}

/* The scroll offset that keeps view row `i` wholly in sight, from `scroll`. */
static inline f32 sftp_scroll_to_row(const SftpPaneGeom *g, f32 scroll, i32 i, i32 rows) {
    if (i < 0 || g->row_h <= 0.0f) return scroll;
    f32 top = (f32)i * g->row_h, bottom = top + g->row_h;
    if (top < scroll) scroll = top;
    else if (bottom > scroll + g->list.h) scroll = bottom - g->list.h;
    f32 max = sftp_scroll_max(g, rows);
    return scroll < 0.0f ? 0.0f : scroll > max ? max : scroll;
}

#endif
