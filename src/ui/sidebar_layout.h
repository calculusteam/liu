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
 * - Rail mode is laid out separately as one column of icons.
 */
#ifndef UI_SIDEBAR_LAYOUT_H
#define UI_SIDEBAR_LAYOUT_H

#include "core/types.h"

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
#define SIDEBAR_ROW_PT             30.0f   /* tab row: 28pt pill + 1pt above and below */
#define SIDEBAR_ROW_COMPACT_PT     26.0f
#define SIDEBAR_GROUP_HDR_PT       26.0f
#define SIDEBAR_SEC_HDR_PT         28.0f   /* == SIDEBAR_HEADER_PT: same band as the explorer's */
#define SIDEBAR_SEC_GAP_PT          6.0f
#define SIDEBAR_AGENT_ROW_PT       44.0f
#define SIDEBAR_NOTE_ROW_PT        28.0f
#define SIDEBAR_AGENTS_MAX_ROWS     4
#define SIDEBAR_NOTES_MAX_ROWS      6
#define SIDEBAR_TABS_MAX_FRAC       0.45f
#define SIDEBAR_TABS_MIN_ROWS       3      /* the cap never hides the active tab's neighbours */

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

#endif /* UI_SIDEBAR_LAYOUT_H */
