/*
 * The sidebar is a vertical stack of sections: docked tabs, file explorer,
 * agents, and notes, in configured order. `render_sidebar()`, `ui_hit_test()`,
 * and the tab popup use this shared geometry. `ui.c` fills `SidebarLayoutIn`;
 * the layout functions do not depend on `AppState`.
 *
 * Rules:
 * - Disabled sections take no space. A collapsed section keeps its header; the
 *   explorer draws its own header.
 * - One section absorbs leftover height: the open explorer, otherwise the tab
 *   list, otherwise the last open section.
 * - The tab list requests its natural height but is capped at
 *   `SIDEBAR_TABS_MAX_FRAC` while another section is open below it; excess tabs
 *   scroll.
 * - Agents and notes request their natural height, capped at a few rows.
 * - The Customize popover opens from Cmd+K or Settings and does not reserve a
 *   row. `bottom` is the lower boundary for sections.
 * - A section is also moved by dragging its header: the drop slot, the line
 *   drawn for it and the change to the configured order are computed here.
 * - Rail mode is laid out separately as one column of icons.
 */
#ifndef UI_SIDEBAR_LAYOUT_H
#define UI_SIDEBAR_LAYOUT_H

#include "core/types.h"

#include <math.h>
#include <string.h>

typedef enum {
    SB_SEC_TABS = 0,
    SB_SEC_FILES,
    SB_SEC_AGENTS,
    SB_SEC_NOTES,
    SB_SEC_COUNT
} SidebarSection;

/* Points. Mirrored nowhere else — the render and the hit test both come here. */
#define SIDEBAR_RAIL_PT            44.0f   /* collapsed: one column of 28pt buttons */
#define SIDEBAR_RAIL_BTN_PT        28.0f
#define SIDEBAR_ROW_PT             35.0f   /* tab row: 33pt pill + 1pt above and below */
#define SIDEBAR_ROW_COMPACT_PT     30.0f
#define SIDEBAR_GROUP_HDR_PT       30.0f
#define SIDEBAR_SEC_HDR_PT         32.0f
#define SIDEBAR_SEC_GAP_PT          6.0f
#define SIDEBAR_AGENT_ROW_PT       51.0f
#define SIDEBAR_NOTE_ROW_PT        32.0f
#define SIDEBAR_AGENTS_MAX_ROWS     4
#define SIDEBAR_NOTES_MAX_ROWS      6
#define SIDEBAR_TABS_MAX_FRAC       0.45f
#define SIDEBAR_TABS_MIN_ROWS       3      /* the cap never hides the active tab's neighbours */
#define SIDEBAR_DRAG_SLOP_PT        5.0f   /* a header press moves this far before it is a drag */

typedef struct {
    f32  x, y, w, h;         /* the sidebar's rect, framebuffer px */
    f32  dpi;
    bool docked;             /* tabs live here (else the top strip) */
    bool compact;
    u8   order[SB_SEC_COUNT];        /* section ids, top to bottom */
    bool enabled[SB_SEC_COUNT];      /* indexed by section id */
    bool open[SB_SEC_COUNT];         /* body shown (indexed by id) */
    /* natural content of each section, in ROWS of that section's row height */
    i32  tab_rows;           /* tab rows + group header rows + the new-tab row, as row_h units */
    i32  agent_rows;
    i32  note_rows;
    bool files_has_content;  /* the explorer has a folder open */
} SidebarLayoutIn;

typedef struct {
    SidebarSection id;
    bool enabled;
    bool open;
    bool flexible;
    f32  hdr_y, hdr_h;       /* hdr_h == 0 when the section draws its own (files) */
    f32  body_y, body_h;     /* body_h == 0 when collapsed */
    f32  content_h;          /* natural height; > body_h means it scrolls */
} SidebarSectionRect;

typedef struct {
    bool rail;               /* caller asked for the rail — nothing else is laid out */
    f32  row_h;              /* tab row height, px */
    f32  hdr_h;
    f32  bottom;             /* the bar's bottom edge: no section may cross it */
    i32  n;                  /* laid-out sections, in display order */
    SidebarSectionRect sec[SB_SEC_COUNT];
} SidebarLayout;

static inline f32 sidebar_row_px(bool compact, f32 dpi) {
    return (compact ? SIDEBAR_ROW_COMPACT_PT : SIDEBAR_ROW_PT) * dpi;
}

static inline void sidebar_layout_compute(const SidebarLayoutIn *in, SidebarLayout *out) {
    out->rail = false;
    out->row_h = sidebar_row_px(in->compact, in->dpi);
    out->hdr_h = SIDEBAR_SEC_HDR_PT * in->dpi;
    out->bottom = in->y + in->h;
    out->n = 0;

    f32 gap    = SIDEBAR_SEC_GAP_PT * in->dpi;
    f32 avail  = in->h;
    if (avail < 0.0f) avail = 0.0f;

    /* Pass 1: which sections exist, their headers and natural bodies. */
    f32 fixed = 0.0f;                 /* everything that is not flexible */
    i32 flex  = -1;
    for (i32 k = 0; k < SB_SEC_COUNT; k++) {
        u8 id = in->order[k];
        if (id >= SB_SEC_COUNT) continue;
        if (id == SB_SEC_TABS && !in->docked) continue;   /* the strip has them */
        if (!in->enabled[id]) continue;
        SidebarSectionRect *s = &out->sec[out->n++];
        s->id = (SidebarSection)id;
        s->enabled = true;
        s->open = in->open[id];
        s->flexible = false;
        s->hdr_h = (id == SB_SEC_FILES) ? 0.0f : out->hdr_h;
        s->body_h = 0.0f;
        switch (id) {
        case SB_SEC_TABS:
            s->content_h = (f32)in->tab_rows * out->row_h; break;
        case SB_SEC_AGENTS: {
            i32 rows = in->agent_rows < SIDEBAR_AGENTS_MAX_ROWS ? in->agent_rows : SIDEBAR_AGENTS_MAX_ROWS;
            if (rows < 1) rows = 1;   /* an empty list still says "no agents" */
            s->content_h = (f32)rows * SIDEBAR_AGENT_ROW_PT * in->dpi; break;
        }
        case SB_SEC_NOTES: {
            i32 rows = in->note_rows < SIDEBAR_NOTES_MAX_ROWS ? in->note_rows : SIDEBAR_NOTES_MAX_ROWS;
            if (rows < 1) rows = 1;
            s->content_h = (f32)rows * SIDEBAR_NOTE_ROW_PT * in->dpi; break;
        }
        default:
            s->content_h = 0.0f; break;   /* files: takes what is left */
        }
        if (!s->open) s->content_h = 0.0f;
    }

    /* The flexible one: files when open, else the tab list, else the last open. */
    for (i32 i = 0; i < out->n; i++)
        if (out->sec[i].id == SB_SEC_FILES && out->sec[i].open) flex = i;
    if (flex < 0)
        for (i32 i = 0; i < out->n; i++)
            if (out->sec[i].id == SB_SEC_TABS && out->sec[i].open) flex = i;
    if (flex < 0)
        for (i32 i = out->n - 1; i >= 0; i--)
            if (out->sec[i].open) { flex = i; break; }

    /* Pass 2: body heights. The tab list is capped only while something else
     * open sits in the stack with it. */
    bool other_open = false;
    for (i32 i = 0; i < out->n; i++)
        if (out->sec[i].open && out->sec[i].id != SB_SEC_TABS) other_open = true;
    for (i32 i = 0; i < out->n; i++) {
        SidebarSectionRect *s = &out->sec[i];
        if (i == flex) { s->flexible = true; continue; }
        f32 h = s->content_h;
        if (s->id == SB_SEC_TABS && other_open) {
            f32 cap = avail * SIDEBAR_TABS_MAX_FRAC;
            f32 floor_h = (f32)SIDEBAR_TABS_MIN_ROWS * out->row_h;
            if (cap < floor_h) cap = floor_h;
            if (h > cap) h = cap;
        }
        s->body_h = h;
        fixed += s->hdr_h + s->body_h + gap;
    }
    if (flex >= 0) {
        SidebarSectionRect *s = &out->sec[flex];
        f32 h = avail - fixed - s->hdr_h - gap;
        if (h < 0.0f) h = 0.0f;
        /* A flexible tab list still stops at its content; the space below it
         * simply stays empty rather than becoming a scrollable void. */
        if (s->id == SB_SEC_TABS && h > s->content_h) h = s->content_h;
        s->body_h = h;
    }

    /* Pass 3: positions, top to bottom. A sidebar too short for its fixed
     * sections cuts the ones at the bottom rather than running them past its
     * edge: whatever does not fit gets no body (and no header once even that
     * is past the bottom). The painter and the hit test then agree that those
     * pixels belong to nothing. */
    f32 y = in->y;
    for (i32 i = 0; i < out->n; i++) {
        SidebarSectionRect *s = &out->sec[i];
        if (y > out->bottom) y = out->bottom;   /* pushed off: parked at the bottom, empty */
        if (y + s->hdr_h > out->bottom) { s->hdr_h = 0.0f; s->body_h = 0.0f; s->open = false; }
        s->hdr_y  = y;
        s->body_y = y + s->hdr_h;
        if (s->body_y + s->body_h > out->bottom) {
            s->body_h = out->bottom - s->body_y;
            if (s->body_h < 0.0f) s->body_h = 0.0f;
        }
        y = s->body_y + s->body_h + (s->hdr_h > 0.0f || s->body_h > 0.0f ? gap : 0.0f);
    }
}

/* Row index under a y inside a section body, honouring its scroll. -1 above/below. */
static inline i32 sidebar_row_at(const SidebarSectionRect *s, f32 row_h, f32 scroll, f32 y, i32 rows) {
    if (row_h <= 0.0f || y < s->body_y || y >= s->body_y + s->body_h) return -1;
    i32 r = (i32)((y - s->body_y + scroll) / row_h);
    return (r >= 0 && r < rows) ? r : -1;
}

/* Clamp a section's scroll offset to its overflow. */
static inline f32 sidebar_scroll_clamp(const SidebarSectionRect *s, f32 scroll) {
    f32 max = s->content_h - s->body_h;
    if (max < 0.0f) max = 0.0f;
    if (scroll < 0.0f) scroll = 0.0f;
    if (scroll > max) scroll = max;
    return scroll;
}

/* ---- reordering by dragging a header ------------------------------------
 *
 * A section is dragged by its header (the explorer by its breadcrumb) and
 * dropped between two others. The drop is a SLOT: the display index of the
 * section it lands in front of, or one past the last drawn section. Sections
 * parked at the bottom with nothing drawn are no target. */

/* Display index of the laid-out section `id`, -1 when it is not laid out. */
static inline i32 sidebar_layout_index(const SidebarLayout *L, i32 id) {
    for (i32 i = 0; i < L->n; i++)
        if ((i32)L->sec[i].id == id) return i;
    return -1;
}

/* A section that has pixels: neither parked nor empty. */
static inline bool sidebar_sec_drawn(const SidebarSectionRect *s) {
    return s->hdr_h + s->body_h > 0.0f;
}

/* The display index of the last drawn section, -1 when none is. Its next
 * index is the END slot. */
static inline i32 sidebar_drawn_last(const SidebarLayout *L) {
    i32 last = -1;
    for (i32 i = 0; i < L->n; i++)
        if (sidebar_sec_drawn(&L->sec[i])) last = i;
    return last;
}

/* Whether `slot` is one a drop can name: a drawn section, or the end. */
static inline bool sidebar_slot_valid(const SidebarLayout *L, i32 slot) {
    i32 last = sidebar_drawn_last(L);
    if (last < 0 || slot < 0 || slot > last + 1) return false;
    return slot == last + 1 || sidebar_sec_drawn(&L->sec[slot]);
}

/* The slot just behind display index `i`: the next drawn section, or the end. */
static inline i32 sidebar_slot_after(const SidebarLayout *L, i32 i) {
    for (i32 k = i + 1; k < L->n; k++)
        if (sidebar_sec_drawn(&L->sec[k])) return k;
    return sidebar_drawn_last(L) + 1;
}

/* The slot under a pointer at `y`: the section the pointer is over, in front
 * of it in its upper half and behind it in its lower half. */
static inline i32 sidebar_drop_slot(const SidebarLayout *L, f32 y) {
    for (i32 i = 0; i < L->n; i++) {
        const SidebarSectionRect *s = &L->sec[i];
        if (!sidebar_sec_drawn(s)) continue;
        if (y < s->hdr_y + (s->hdr_h + s->body_h) * 0.5f) return i;
    }
    return sidebar_drawn_last(L) + 1;
}

/* Whether dropping the section at display index `from` into `slot` moves it:
 * the slots on either side of it leave it where it is. */
static inline bool sidebar_drop_moves(const SidebarLayout *L, i32 from, i32 slot) {
    if (from < 0 || from >= L->n || !sidebar_sec_drawn(&L->sec[from])) return false;
    if (!sidebar_slot_valid(L, slot)) return false;
    return slot != from && slot != sidebar_slot_after(L, from);
}

/* Where the drop line for `slot` is drawn: centred in the gap above the
 * section it lands in front of, or below the last drawn section; never
 * outside the bar. */
static inline f32 sidebar_drop_line_y(const SidebarLayout *L, i32 slot, f32 top, f32 gap) {
    i32 last = sidebar_drawn_last(L);
    f32 y = top;
    if (last < 0 || !sidebar_slot_valid(L, slot)) return y;
    if (slot <= last) {
        y = L->sec[slot].hdr_y;
        if (y > top) y -= gap * 0.5f;
    } else {
        const SidebarSectionRect *s = &L->sec[last];
        y = s->body_y + s->body_h + gap * 0.5f;
    }
    if (y < top) y = top;
    if (y > L->bottom) y = L->bottom;
    return y;
}

/* Apply a drop to the configured order, which also holds the sections that
 * are switched off or not laid out (the tab list while undocked): `id` is
 * moved in front of the section at `slot`, or just behind the last drawn one
 * for the end slot. Every other section keeps its place relative to the
 * rest. Returns whether the order changed. */
static inline bool sidebar_order_drop(u8 order[SB_SEC_COUNT], const SidebarLayout *L, i32 id, i32 slot) {
    if (id < 0 || id >= SB_SEC_COUNT) return false;
    if (!sidebar_drop_moves(L, sidebar_layout_index(L, id), slot)) return false;
    i32 last = sidebar_drawn_last(L);
    i32 anchor = slot <= last ? (i32)L->sec[slot].id : (i32)L->sec[last].id;
    bool behind = slot > last;

    u8 rest[SB_SEC_COUNT];
    i32 n = 0;
    for (i32 k = 0; k < SB_SEC_COUNT; k++)
        if ((i32)order[k] != id) rest[n++] = order[k];
    if (n != SB_SEC_COUNT - 1) return false;   /* `id` was not in the order once */
    i32 at = -1;
    for (i32 k = 0; k < n; k++)
        if ((i32)rest[k] == anchor) { at = behind ? k + 1 : k; break; }
    if (at < 0) return false;

    u8 next[SB_SEC_COUNT];
    for (i32 k = 0, j = 0; k < SB_SEC_COUNT; k++)
        next[k] = (k == at) ? (u8)id : rest[j++];
    bool changed = memcmp(next, order, SB_SEC_COUNT) != 0;
    memcpy(order, next, SB_SEC_COUNT);
    return changed;
}

/* ---- the jump pills over the docked tab list ----------------------------
 *
 * When the list scrolls, a tab that matters — the open one, one that rang,
 * one whose agent is at work — can end up out of sight above or below. A
 * pill then floats at that edge of the list: the icons of the nearest such
 * tabs (up to SIDEBAR_JUMP_MAX_ICONS, then "+N") and an arrow; a click goes
 * to the nearest. A row counts as out of sight once less than half of it
 * shows. When the list fits, or is too short to carry a pill, there is none;
 * when both would not fit, the one toward the open tab stays, else the lower
 * one. */
#define SIDEBAR_JUMP_H_PT         28.0f
#define SIDEBAR_JUMP_ICON_PT      16.0f
#define SIDEBAR_JUMP_ICON_GAP_PT   4.0f
#define SIDEBAR_JUMP_ARROW_PT     14.0f
#define SIDEBAR_JUMP_PAD_PT       10.0f   /* inside the pill, each end */
#define SIDEBAR_JUMP_ARROW_GAP_PT  6.0f   /* last icon → arrow */
#define SIDEBAR_JUMP_MARGIN_PT     8.0f   /* from the list's edge */
#define SIDEBAR_JUMP_CELL_PT       8.0f   /* a cell of the "+N" text */
#define SIDEBAR_JUMP_MAX_ICONS     3
#define SIDEBAR_GLIDE_S            0.22f  /* the list's scroll to a jump's tab */

enum { SIDEBAR_JUMP_UP = 0, SIDEBAR_JUMP_DOWN = 1 };

typedef struct {
    bool shown;
    i32  rows[SIDEBAR_JUMP_MAX_ICONS];   /* row indexes shown as icons, nearest first */
    i32  tabs[SIDEBAR_JUMP_MAX_ICONS];   /* their tabs (filled by app_side_jumps) */
    i32  n_icons;
    i32  more;                           /* rows that matter past those */
    f32  x, y, w, h;                     /* px */
} SidebarJump;

/* The pills for a list of `n` rows of `row_h` in section `s`, scrolled by
 * `scroll`, laid in [x0, x0 + w). `notable[r]` marks the rows that matter;
 * `active_row` is the open tab's row (-1 none). */
static inline void sidebar_jumps_compute(const SidebarSectionRect *s, f32 row_h, f32 scroll,
                                         const bool *notable, i32 n, i32 active_row,
                                         f32 x0, f32 w, f32 dpi, SidebarJump out[2]) {
    memset(out, 0, 2 * sizeof *out);
    f32 h = SIDEBAR_JUMP_H_PT * dpi;
    f32 margin = SIDEBAR_JUMP_MARGIN_PT * dpi;
    if (!s || !s->open || row_h <= 0.0f || n <= 0) return;
    if (s->content_h <= s->body_h + 0.5f) return;
    if (s->body_h < h + 2.0f * margin) return;
    f32 top = s->body_y, bottom = s->body_y + s->body_h;
    /* Above: the nearest is the LAST hidden row; below: the first. */
    for (i32 r = n - 1; r >= 0; r--) {
        if (!notable[r]) continue;
        f32 mid = top - scroll + ((f32)r + 0.5f) * row_h;
        if (mid >= top) continue;
        SidebarJump *j = &out[SIDEBAR_JUMP_UP];
        if (j->n_icons < SIDEBAR_JUMP_MAX_ICONS) j->rows[j->n_icons++] = r;
        else j->more++;
    }
    for (i32 r = 0; r < n; r++) {
        if (!notable[r]) continue;
        f32 mid = top - scroll + ((f32)r + 0.5f) * row_h;
        if (mid <= bottom) continue;
        SidebarJump *j = &out[SIDEBAR_JUMP_DOWN];
        if (j->n_icons < SIDEBAR_JUMP_MAX_ICONS) j->rows[j->n_icons++] = r;
        else j->more++;
    }
    for (i32 d = 0; d < 2; d++) {
        SidebarJump *j = &out[d];
        if (j->n_icons == 0) continue;
        f32 cw = 0.0f;
        if (j->more > 0) {
            i32 cells = 2 + (j->more >= 10) + (j->more >= 100);   /* "+N" */
            cw = (f32)cells * SIDEBAR_JUMP_CELL_PT * dpi + SIDEBAR_JUMP_ICON_GAP_PT * dpi;
        }
        f32 pw = 2.0f * SIDEBAR_JUMP_PAD_PT * dpi
               + (f32)j->n_icons * SIDEBAR_JUMP_ICON_PT * dpi
               + (f32)(j->n_icons - 1) * SIDEBAR_JUMP_ICON_GAP_PT * dpi
               + cw + SIDEBAR_JUMP_ARROW_GAP_PT * dpi + SIDEBAR_JUMP_ARROW_PT * dpi;
        if (pw > w - 2.0f * margin) pw = w - 2.0f * margin;
        j->shown = true;
        j->w = floorf(pw);
        j->h = h;
        j->x = floorf(x0 + (w - j->w) * 0.5f);
        j->y = d == SIDEBAR_JUMP_UP ? floorf(top + margin) : floorf(bottom - margin - h);
    }
    SidebarJump *up = &out[SIDEBAR_JUMP_UP], *dn = &out[SIDEBAR_JUMP_DOWN];
    if (up->shown && dn->shown && up->y + up->h + margin > dn->y) {
        bool active_up = false;
        for (i32 k = 0; k < up->n_icons; k++) if (up->rows[k] == active_row) active_up = true;
        if (active_up) dn->shown = false; else up->shown = false;
    }
}

/* Whether (px, py) is on a shown pill; its direction, else -1. */
static inline i32 sidebar_jump_at(const SidebarJump jumps[2], f32 px, f32 py) {
    for (i32 d = 0; d < 2; d++) {
        const SidebarJump *j = &jumps[d];
        if (j->shown && px >= j->x && px < j->x + j->w && py >= j->y && py < j->y + j->h) return d;
    }
    return -1;
}

/* The scroll that brings row `r` to the middle of the list, as far as the
 * list lets it. */
static inline f32 sidebar_scroll_to_row(const SidebarSectionRect *s, f32 row_h, i32 r) {
    f32 want = ((f32)r + 0.5f) * row_h - s->body_h * 0.5f;
    return sidebar_scroll_clamp(s, want);
}

/* The list's scroll on its way from `from` to `to`, started at `t0`, at
 * `now`, eased out over SIDEBAR_GLIDE_S. */
static inline f32 sidebar_glide_at(f32 from, f32 to, f64 t0, f64 now) {
    f32 t = (f32)((now - t0) / (f64)SIDEBAR_GLIDE_S);
    if (t >= 1.0f) return to;
    if (t <= 0.0f) return from;
    f32 u = 1.0f - t;
    return from + (to - from) * (1.0f - u * u * u);
}

/* Top of the lifted copy that follows the pointer: where the pointer took it
 * (`grab` below its top), kept inside [top, bottom]. */
static inline f32 sidebar_lift_top(f32 pointer_y, f32 grab, f32 lift_h, f32 top, f32 bottom) {
    if (grab < 0.0f) grab = 0.0f;
    if (grab > lift_h - 1.0f) grab = lift_h - 1.0f;
    f32 y = pointer_y - grab;
    if (y > bottom - lift_h) y = bottom - lift_h;
    if (y < top) y = top;
    return y;
}

/* ---- the slide after a reorder ------------------------------------------
 *
 * A new order changes where sections are, never how tall: every height comes
 * from the bar's size and the sections' content, not from their places. So
 * each section glides from the top it was drawn at to the top it has now,
 * eased out over SIDEBAR_SLIDE_S. The section that was moved (the LEAD)
 * starts where its lifted copy was let go and is drawn over the others, which
 * it crosses; the rest shift by its height and never cross each other. The
 * same holds for the rail's section buttons (`rail`). */
#define SIDEBAR_SLIDE_S 0.20f

typedef struct {
    bool active;
    bool rail;                      /* measured on the rail's buttons */
    i32  lead;                      /* the section that moved, -1 none */
    f64  t0;
    bool has[SB_SEC_COUNT];         /* a section had a place to slide from */
    f32  from_y[SB_SEC_COUNT];      /* its top then, px */
} SidebarSlide;

static inline f32 sidebar_slide_ease(f32 t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    f32 u = 1.0f - t;
    return 1.0f - u * u * u;
}

static inline bool sidebar_slide_running(const SidebarSlide *sl, f64 now) {
    return sl->active && now - sl->t0 < (f64)SIDEBAR_SLIDE_S;
}

/* How far above (negative: below) its place section `id` is drawn at `now`,
 * its place being `to_y`. Zero once the slide is over, for a section it did
 * not record, and for the other kind of stack. */
static inline f32 sidebar_slide_dy(const SidebarSlide *sl, bool rail, i32 id, f32 to_y, f64 now) {
    if (!sidebar_slide_running(sl, now) || sl->rail != rail) return 0.0f;
    if (id < 0 || id >= SB_SEC_COUNT || !sl->has[id]) return 0.0f;
    f32 t = (f32)((now - sl->t0) / (f64)SIDEBAR_SLIDE_S);
    return (sl->from_y[id] - to_y) * (1.0f - sidebar_slide_ease(t));
}

/* Start a slide from what `stack` shows at `now` — its places plus whatever a
 * slide still running adds, so a second reorder mid-slide carries on from
 * where each section is drawn rather than jumping. `lead_y` (< 0: none) is
 * where the lead starts instead of its own place. */
static inline void sidebar_slide_begin(SidebarSlide *sl, const SidebarLayout *stack, i32 lead,
                                       f32 lead_y, f64 now) {
    SidebarSlide next;
    memset(&next, 0, sizeof next);
    next.active = true;
    next.rail = stack->rail;
    next.lead = lead;
    next.t0 = now;
    for (i32 i = 0; i < stack->n; i++) {
        const SidebarSectionRect *s = &stack->sec[i];
        if (!sidebar_sec_drawn(s) || (i32)s->id < 0 || s->id >= SB_SEC_COUNT) continue;
        next.has[s->id] = true;
        next.from_y[s->id] = s->hdr_y + sidebar_slide_dy(sl, stack->rail, s->id, s->hdr_y, now);
    }
    if (lead >= 0 && lead < SB_SEC_COUNT && next.has[lead] && lead_y >= 0.0f)
        next.from_y[lead] = lead_y;
    *sl = next;
}

#endif /* UI_SIDEBAR_LAYOUT_H */
