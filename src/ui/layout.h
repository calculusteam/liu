/*
 * Liu — shared layout constants (in points, multiply by dpi_scale)
 * Single source of truth for all UI dimensions.
 */
#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

#include "core/types.h"

/* A pane advertises only the cells that fit in its actual pixel rectangle. */
typedef struct { i32 cols, rows; } TermGridSize;
static inline TermGridSize term_grid_fit(f32 w, f32 h, f32 cw, f32 ch) {
    f32 cols = cw > 0 && w > 0 ? w / cw : 1;
    f32 rows = ch > 0 && h > 0 ? h / ch : 1;
    if (!(cols >= 1)) cols = 1;
    if (!(rows >= 1)) rows = 1;
    if (cols > 32767) cols = 32767;
    if (rows > 32767) rows = 32767;
    return (TermGridSize){ (i32)cols, (i32)rows };
}
static inline bool term_layout_same(i32 old_cols, i32 old_rows, f32 old_w, f32 old_h,
                                    i32 cols, i32 rows, f32 w, f32 h) {
    return old_cols == cols && old_rows == rows && old_w == w && old_h == h;
}

/* Toolbar (transparent macOS title bar) */
/* Strip height = pill height (28pt) + PILL_PAD_Y_PT above and below. Raise the
 * two together: the strip only exists to frame the pill, so extra height here
 * without matching PILL_PAD_Y just stretches the pill instead of padding it. */
#define TOOLBAR_HEIGHT_PT      36.0f   /* 28pt pill + 4pt top/bottom — tighter strip */

/* The secure-input chip (ui.c, the strip's quiet third chip): a capsule the
 * colour of the bar moved toward "protected" green, a solid padlock, and its
 * two words a step smaller than the strip's own text. */
#define SECURE_CHIP_H_PT        20.0f   /* capsule height; the radius is half of it */
#define SECURE_CHIP_PAD_L_PT     9.0f   /* capsule edge → padlock */
#define SECURE_CHIP_GAP_PT       7.0f   /* padlock → label */
#define SECURE_CHIP_PAD_R_PT    11.0f   /* label → capsule edge (a round end needs more than a square one) */
#define SECURE_CHIP_TEXT_W_PT    7.0f   /* label cell width (the strip's text is 8) */
#define SECURE_CHIP_LOCK_W_PT    9.0f   /* padlock body width */
#define SECURE_CHIP_LOCK_H_PT   11.0f   /* padlock, shackle included */
#define SECURE_CHIP_EDGE_PT     12.0f   /* from the window's right edge when it is the only chip */
/* Fallback width of the zone kept clear for the macOS traffic lights, used only
 * when the platform can't report where the buttons actually sit. Equals a
 * macOS 26-SDK build's zoom-button right edge (69pt) + TRAFFIC_LIGHT_GAP_PT.
 * Move it with that gap — the two are one number split across a measured and
 * an unmeasured path, and they have to land the icon in the same place. */
#define TRAFFIC_LIGHT_ZONE_PT  77.0f
/* Breathing room between the last traffic light and the first toolbar icon.
 * The inset is the MEASURED right edge of the buttons plus this gap — AppKit
 * places them differently depending on the SDK the binary was linked against
 * (61pt right edge under 14.5, 69pt under 26.x), so a hardcoded zone leaves a
 * release built on an older toolchain with 8pt of extra dead space. */
/* 8pt. The right gap (TB_SEP_GAP_PT) is defined FROM this one, so the leading
 * cluster stays symmetric and equally tight on both sides by construction. */
#define TRAFFIC_LIGHT_GAP_PT   8.0f
/* Left margin the toolbar falls back to when the traffic-light zone is not
 * reserved (macOS native fullscreen hides the buttons in AppKit's overlay).
 * Matches the 10pt breathing room the strip keeps on its right edge, so the
 * bar stays visually symmetric instead of gluing the first icon to x = 0. */
#define TOOLBAR_EDGE_PAD_PT    10.0f

/* Window controls at the toolbar's right end, drawn by Liu where the window
 * manager draws none (X11: platform_draws_own_window_controls). Close is the
 * rightmost, then maximize / restore, then minimize. ONE geometry: the
 * painter (render_toolbar) and the hit test (hittest.c) both ask
 * ui_win_ctl_x(), and the strip's right-hand content starts left of
 * ui_win_ctl_zone_pt(). */
#define WIN_CTL_COUNT          3
#define WIN_CTL_BTN_PT         28.0f   /* square box, centred on the bar */
#define WIN_CTL_GAP_PT          2.0f
#define WIN_CTL_EDGE_PT         6.0f   /* from the window's right edge */
#define WIN_CTL_GLYPH_PT       10.0f
#define WIN_CTL_LEAD_PT         6.0f   /* between the controls and the strip */
static inline f32 ui_win_ctl_x(i32 i, f32 fb_w, f32 dpi) {
    return fb_w - (WIN_CTL_EDGE_PT + WIN_CTL_BTN_PT +
                   (WIN_CTL_BTN_PT + WIN_CTL_GAP_PT) * (f32)i) * dpi;
}
static inline f32 ui_win_ctl_zone_pt(void) {
    return WIN_CTL_EDGE_PT + WIN_CTL_LEAD_PT + WIN_CTL_BTN_PT * (f32)WIN_CTL_COUNT +
           WIN_CTL_GAP_PT * (f32)(WIN_CTL_COUNT - 1);
}

/* Sketch toolbar: one edge-to-edge gap for tools, colors, and history. */
#define SKETCH_PANEL_RATIO      0.75f
#define SKETCH_BUTTON_SIZE_PT  36.0f
#define SKETCH_COLOR_SIZE_PT   28.0f
#define SKETCH_BUTTON_GAP_PT    8.0f
#define SKETCH_PANEL_PAD_PT   12.0f
#define SKETCH_TOOLBAR_PAD_PT  8.0f
#define SKETCH_SLIDER_H_PT   160.0f
#define SKETCH_SLIDER_W_PT    32.0f

/* Chrome geometry */
#define CHROME_RADIUS_PT        0.0f
#define CHROME_LINE_PT          1.0f

/* Floating notifications: one grid for toast, agent completion and AppKit. */
#define NOTIFICATION_EDGE_PT       16.0f
#define NOTIFICATION_TOP_PT        12.0f
#define NOTIFICATION_PAD_PT        14.0f
#define NOTIFICATION_GAP_PT        12.0f
#define NOTIFICATION_STACK_GAP_PT   8.0f
#define NOTIFICATION_RADIUS_PT     12.0f
#define NOTIFICATION_TILE_PT       28.0f
#define NOTIFICATION_ICON_PT       16.0f
#define NOTIFICATION_LINE_GAP_PT    4.0f
#define NOTIFICATION_MIN_W_PT     240.0f
#define NOTIFICATION_MAX_W_PT     420.0f
#define NOTIFICATION_PROGRESS_PT    2.0f
#define NOTIFICATION_BADGE_RADIUS_PT 8.0f
#define NOTIFICATION_RAIL_INSET_PT   6.0f
#define NOTIFICATION_NOTICE_MIN_W_PT 320.0f
#define NOTIFICATION_TOPIC_GAP_PT    6.0f
#define NOTIFICATION_META_GAP_PT    10.0f
#define NOTIFICATION_SLIDE_PT       10.0f
#define NOTIFICATION_HOLD_S         3.5

/* Image lightbox caption (ui/lightbox.c): the file's line and the key hints
 * sit on a plate of their own, because the scrim leaves the session legible
 * behind them on purpose. */
#define LIGHTBOX_CAPTION_PAD_X_PT   14.0f
#define LIGHTBOX_CAPTION_PAD_Y_PT    8.0f
#define LIGHTBOX_CAPTION_RADIUS_PT  10.0f

/* The app's rounded-chrome radius.
 *
 * The tab pill is the reference surface — it is the rounded thing the eye has
 * on screen at all times — so anything meant to read as the same material takes
 * its corner from here rather than picking its own. One constant, so a pill and
 * a floating card cannot drift into two different roundings. */
#define UI_PILL_RADIUS_PT       8.0f

/* Status bar */
#define STATUS_BAR_HEIGHT_PT   22.0f
/* Longest cwd the status bar paints before eliding from the left. Bounded so a
 * deep tree cannot push the git segment — a click target since the diff dock
 * moved here — past the right-hand group or off the bar entirely. */
#define STATUS_CWD_MAX_CHARS   44

/* In-app browser chrome (TAB_BROWSER / split pane). Base sizes in points;
 * browser_ui.c scales + progressive-disclosure based on pane width.
 * Icons match toolbar weight (TB_ICON_SIZE_PT ~14); hit boxes stay slightly
 * larger than the glyph so clicks remain comfortable without eating URL width. */
#define BROWSER_CHROME_H_PT     32.0f
#define BROWSER_BTN_SIZE_PT     18.0f   /* hit box; small enough for a split pane */
#define BROWSER_ICON_SIZE_PT    TB_ICON_SIZE_PT
/* Browser glyphs deliberately share the toolbar's 14 pt optical size.  The
 * button remains compact; only the invisible hit target is larger. */
#define BROWSER_URL_H_PT        22.0f
#define BROWSER_PAD_PT           5.0f
#define BROWSER_BTN_GAP_PT       1.0f
#define BROWSER_URL_GAP_PT       5.0f
#define BROWSER_URL_RADIUS_PT    6.0f
#define BROWSER_BTN_RADIUS_PT    4.0f
/* Width breakpoints (points) for progressive chrome collapse. */
#define BROWSER_BP_FULL_PT     320.0f   /* all actions */
#define BROWSER_BP_MED_PT      240.0f   /* drop attach */
#define BROWSER_BP_NARROW_PT   180.0f   /* drop pick + external */
#define BROWSER_BP_TINY_PT     130.0f   /* drop forward; compact URL */
#define BROWSER_MAX_TABS         4

/* Sidebar */
#define SIDEBAR_DEFAULT_PT    240.0f
#define SIDEBAR_MIN_PT        160.0f
#define SIDEBAR_MAX_PT        500.0f
#define SIDEBAR_HEADER_PT      28.0f
#define SIDEBAR_HEADER_PAD_PT  10.0f
#define SIDEBAR_BORDER_W_PT     2.0f
#define SIDEBAR_RESIZE_GRAB_PT  5.0f

/* A section header (render_side_header) and the drag that moves a section.
 * The mixes are display-space steps of the text colour over the panel's
 * surface (panel_wash). */
#define SIDE_HDR_RADIUS_PT       6.0f
#define SIDE_HDR_HOVER_MIX       0.045f   /* the explorer's hovered row */
#define SIDE_HDR_GRIP_PT        12.0f    /* in a 22pt box left of the header's actions */
#define SIDE_HDR_BADGE_H_PT     16.0f    /* the count's pill */
#define SIDE_HDR_BADGE_PAD_PT    4.0f
#define SIDE_HDR_BADGE_GAP_PT    7.0f
#define SIDE_HDR_BADGE_MIX       0.08f
#define SIDE_DRAG_LIFT_H_PT     30.0f    /* the lifted header under the pointer */
#define SIDE_DRAG_LIFT_X_PT     16.0f    /* its left edge: clear of the line's dot */
#define SIDE_DRAG_LIFT_INSET_PT  6.0f    /* its right edge */
#define SIDE_DRAG_SOURCE_VEIL    0.55f    /* the panel laid over the section being moved */
#define SIDE_DRAG_LINE_PT        2.0f
#define SIDE_DRAG_LINE_INSET_PT  8.0f
#define SIDE_DRAG_LINE_DOT_PT    6.0f
#define SIDE_RAIL_LINE_INSET_PT  5.0f    /* the rail's drop line: its dot's centre */
/* A tab group in the docked list: its chip is the strip's (GROUP_CHIP_*), its
 * open members are indented and the group's line (GROUP_LINE_*) runs down the
 * gutter the indent leaves. The painter and the hit test read the indent. */
/* The docked list's jump pill (render_side_jumps; its geometry is
 * SIDEBAR_JUMP_* in ui/sidebar_layout.h): text colour mixed into the panel. */
#define SIDE_JUMP_FILL_MIX       0.10f
#define SIDE_JUMP_HOVER_MIX      0.06f
#define SIDE_JUMP_RIM_ALPHA      0.22f
#define SIDE_GROUP_INDENT_PT    12.0f
#define SIDE_GROUP_LINE_X_PT    12.0f    /* the line's centre: mid-gutter, 6pt row inset + half the indent */

/* File-browser list rows: one uniform height everywhere — the 16pt text cell
 * plus an EQUAL top/bottom padding (content is centred inside the row, so the
 * padding lands half above / half below). The row height formula below is the
 * single source of truth; it is mirrored by fb_render_sidebar, fb_handle_click
 * and the two hit-test branches (HIT_SIDEBAR_ITEM / HIT_FBTAB_ITEM) in
 * hittest.c — always change it HERE, never inline. */
#define FB_ENTRY_VPAD_PT        6.0f    /* per-side vertical padding */
#define FB_ENTRY_H(ch, dpi)     ((ch) + 2.0f * FB_ENTRY_VPAD_PT * (dpi))

/* The explorer panel (fb_render_sidebar): a header over rows of one height.
 * The renderer, fb_handle_click and both hit tests read its geometry through
 * fb_list_hdr_h / fb_list_row_h / fb_list_entry_at (filebrowser.h).
 *
 * A row is [chevron][gap][icon][gap][name]. One level of depth moves a row by
 * the chevron and its gap, so a child's chevron stands under its parent's
 * icon and the indent guide under the parent's chevron. */
#define FB_PANEL_HDR_PT          26.0f   /* holds FB_PANEL_BTN_PT's chip with 2pt to spare */
#define FB_PANEL_ROW_PT          28.0f
#define FB_PANEL_PAD_PT          12.0f   /* header inset, left and right */
#define FB_PANEL_ROW_PAD_PT       8.0f   /* a top-level row's chevron from the edge */
#define FB_PANEL_CHEV_PT         12.0f
#define FB_PANEL_CHEV_GAP_PT      4.0f
#define FB_PANEL_INDENT_PT       (FB_PANEL_CHEV_PT + FB_PANEL_CHEV_GAP_PT)
#define FB_PANEL_ICON_PT         16.0f
#define FB_PANEL_ICON_GAP_PT      6.0f
#define FB_PANEL_BTN_PT          22.0f   /* a header button's hover chip, square */
#define FB_PANEL_BTN_RADIUS_PT    5.0f
#define FB_PANEL_SIZE_COL_PT     64.0f   /* room a file's size takes at the row's end */
#define FB_PANEL_SIZE_MIN_W_PT  300.0f   /* the size column shows from this panel width */
#define FB_PANEL_SCROLL_W_PT      5.0f
/* The panel's surface: the window's background moved this far toward its
 * text colour, lighter on a dark theme and darker on a light one. */
#define FB_PANEL_SURFACE_MIX      0.035f

/* The file browser's name prompt (ui/fb_menu.h: fb_prompt_geom). Its context
 * menu has no numbers of its own — see CTX_MENU_* below. */
#define FB_PROMPT_W_PT          460.0f
#define FB_PROMPT_BTN_H_PT       30.0f

/* Command history popover, aligned with the terminal's context menus. */
#define HISTORY_MENU_W_PT       440.0f
#define HISTORY_MENU_HEADER_PT   56.0f
#define HISTORY_MENU_ROW_PT      34.0f
#define HISTORY_MENU_FOOTER_PT   38.0f
#define HISTORY_MENU_INSET_PT     6.0f
#define HISTORY_MENU_PAD_PT      14.0f
#define HISTORY_MENU_GAP_PT       8.0f

/* Tabs */
#define TAB_WIDTH_PT          189.0f
#define TAB_GAP_PT              2.0f
/* Comfortable minimum tab width. Tabs grow to fill the bar (up to
 * TAB_WIDTH_PT) when few and shrink to this as more open; once even this
 * doesn't fit, the strip SCROLLS horizontally rather than shrinking further
 * (so tabs never collapse into unreadable slivers / overlapping ×'s). Shared
 * by render_toolbar() + hittest.c. */
#define TAB_MIN_W_PT           72.0f
/* Below this width a tab hides its close × (and the close hit zone is
 * suppressed) — for the active tab too. The × occupies a fixed ~27.5pt zone, so
 * on a narrower pill it would spill past the left edge into the neighbouring tab
 * (overlapping ×'s at high tab counts). Narrow tabs become pure select targets;
 * close them with Cmd+W until siblings close and widths recover. Shared by
 * render + hittest. */
#define TAB_CLOSE_MIN_W_PT     56.0f
#define TAB_CLOSE_SIZE_PT      14.0f
#define TAB_CLOSE_MARGIN_PT     6.0f
#define TAB_DOT_SIZE_PT         6.0f
#define TAB_TITLE_PAD_PT        8.0f
#define TAB_INDICATOR_H_PT      2.0f

/* Pill-tab visual padding (rounded-tab restyle). PILL_INSET_X (gap between
 * adjacent pills / pill edge inset) and PILL_INNER_PAD (content-to-pill-edge
 * padding) are shared with hittest.c so the close-button hit zone tracks the
 * painted pill. PILL_PAD_Y is vertical-only — purely visual, since hit zones
 * still span the full toolbar height. */
/* PILL_INSET_X is per-pill horizontal inset within its slot; the visible gap
 * between two adjacent pills is TAB_GAP_PT + 2*PILL_INSET_X_PT (= 4pt here),
 * intentionally tighter than the vertical (PILL_PAD_Y) and inner content pad. */
#define PILL_INSET_X_PT         1.0f
#define PILL_PAD_Y_PT           4.0f   /* symmetric top+bottom margin → pill centered in the bar */

/* Tab group chrome. One group reads as: a chip (the group's name) a little
 * shorter than the tab pills beside it, then the member pills — EVERY member
 * filled, the open one in the theme's active fill and the others in a faint
 * wash of the group's colour — with the same gap between chip and pill as
 * between pill and pill, and ONE thin line in the group's colour running under
 * all of it, detached from the pills.
 *
 * Every figure below is a fraction of the pill height or an alpha over the
 * strip's background, applied to the 28pt pill:
 *
 *   chip      23.5pt tall, 6pt corner; fill = group colour 19% over the bg
 *             (53,45,32 for gold 236,207,139 on 9,8,8 — no extra lift);
 *             border 0.6pt of the group colour at 48%; dot 26% of the chip's
 *             height, 39% in from its left edge
 *   members   closed: group colour 8% over the bg, no border (19,18,30 for
 *             violet 168,146,246); open: the theme's active fill with 3% of
 *             the group colour in it (37,37,41 -> 42,41,47); 4.5pt between
 *             neighbours and between the chip and the first one
 *   line      1.5pt, group colour at 86%, 2.5pt under the pills, 3.75pt in
 *             from the chip's left edge and from the last pill's right edge;
 *             none under a collapsed group. A thinner gap of near-black
 *             between a near-black pill and the line does not read as a gap
 *             at all, so the line takes everything the strip has under the
 *             pill (4pt = 2.5 + 1.5)
 *   collapsed the same chip and the same solid dot; the hidden-tab count is
 *             written one cell after the name ("infra 2") in the group colour
 *             at 90% (141,190,195 beside a 175,207,212 label on a teal group),
 *             6pt from the chip's right edge — no badge, no ring
 *
 * ui.c's render_toolbar, app_get_group_chip_width() and hittest.c read these
 * and nothing else. */
#define GROUP_CHIP_INSET_Y_PT    2.25f  /* chip is this much shorter than a pill, top AND bottom */
#define GROUP_CHIP_RADIUS_PT     6.0f
#define GROUP_CHIP_BORDER_PT     0.6f   /* ~1.2 px at 2x, at 48% of the group colour */
#define GROUP_CHIP_BORDER_ALPHA  0.48f
#define GROUP_CHIP_TINT          0.19f  /* group colour mixed into the chip's fill */
#define GROUP_CHIP_DOT_FRAC      0.26f  /* dot diameter / chip height */
#define GROUP_CHIP_DOT_X_FRAC    0.39f  /* dot's left edge / chip height */
#define GROUP_CHIP_DOT_GAP_PT    8.0f   /* dot → label */
#define GROUP_CHIP_PAD_R_PT      12.0f  /* label → chip's right edge (no badge) */
#define GROUP_CHIP_COUNT_PAD_R_PT 6.0f  /* collapsed: the count's last cell → chip's right edge */
#define GROUP_CHIP_LABEL_MIX     0.50f  /* the name: group colour mixed this far toward the chip's legible ink */
#define GROUP_CHIP_COUNT_ALPHA   0.90f  /* the count is the group colour itself, a step behind the label */
#define GROUP_MEMBER_INSET_X_PT  2.25f  /* a member pill's inset in its slot: 2x = the gap between members */
#define GROUP_CHIP_GAP_PT        2.25f  /* chip → first member's SLOT; + the member inset = the same 4.5pt */
#define GROUP_MEMBER_TINT        0.08f  /* closed member's fill */
#define GROUP_MEMBER_HOVER_TINT  0.05f  /* added under the pointer */
#define GROUP_ACTIVE_TINT        0.03f  /* group colour in the open member's fill */
#define GROUP_LINE_H_PT          1.5f
#define GROUP_LINE_GAP_PT        2.5f   /* pill bottom → line; + GROUP_LINE_H_PT = PILL_PAD_Y_PT, the strip's whole bottom margin */
#define GROUP_LINE_INSET_PT      3.75f  /* line's ends, in from the chip's left / the last pill's right */
#define GROUP_LINE_ALPHA         0.86f
#define PILL_INNER_PAD_PT      12.5f   /* content padding inside the pill — label sits left, close button right */

/* The "agent is working" indicator inside a tab pill: a hairline track near the
 * pill's bottom edge with one soft glow travelling along it. The track is 1.4pt
 * thick, its bottom edge 1.4pt above the pill's, its ends ~5pt in from the
 * pill's sides, and it is the indicator's colour mixed 18% into the pill's fill
 * (46,42,69 on 19,18,30 for 168,146,246). The glow is the same thickness and
 * the same colour at ~95%, about 43pt long, close to a triangle with a rounded
 * tip and a little longer behind its peak than ahead of it — no white core, no
 * bloom, nothing taller than the track. */
#define TAB_WORK_BAR_H_PT       1.5f
#define TAB_WORK_BAR_LIFT_PT    1.5f   /* pill bottom → the bar's bottom edge */
#define TAB_WORK_BAR_INSET_PT   5.0f   /* the bar's ends, in from the pill's sides */
#define TAB_WORK_TRACK_MIX      0.18f  /* the resting track */
#define TAB_WORK_PEAK_MIX       0.95f  /* the glow's brightest point */
#define TAB_WORK_GLOW_TAIL_PT   24.0f  /* glow length behind its peak */
#define TAB_WORK_GLOW_HEAD_PT   19.0f  /* ...and ahead of it */
#define TAB_WORK_GLOW_MAX_FRAC  0.46f  /* a short track scales the glow down to this share of it */
#define TAB_WORK_SPEED_MIN_PT   54.0f  /* pt/s while the agent trickles */
#define TAB_WORK_SPEED_MAX_PT   132.0f /* pt/s at full throughput */

/* The ✕ glyph is drawn at this fraction of its click target — small enough to
 * read as a light affordance, large enough to hit. */
#define TAB_CLOSE_ICON_RATIO    0.62f

/* X of the tab pill's close-button box.
 *
 * The ✕ is anchored OPTICALLY, not by its box: its ink sits PILL_INNER_PAD_PT
 * from the pill's right edge, the same padding the title has on the left.
 * Two blank margins hide inside the box and are pushed back out here — the
 * centring margin around the smaller glyph, and the SF symbol's own internal
 * padding (`ink_right`, from icon_ink_insets(ICON_CLOSE, close_sz *
 * TAB_CLOSE_ICON_RATIO, ...); pass 0 where the symbol isn't available).
 * Without this the ✕ floated ~9 px further from the right edge than the title
 * sat from the left. Render (ui.c) and hit-test (hittest.c) both call this so
 * the drawn glyph and its click zone stay on top of each other. */
static inline f32 ui_tab_close_x(f32 pill_x, f32 pill_w, f32 close_sz,
                                 f32 ink_right, f32 dpi) {
    f32 ix = close_sz * TAB_CLOSE_ICON_RATIO;
    return pill_x + pill_w - PILL_INNER_PAD_PT * dpi - close_sz
         + (close_sz - ix) * 0.5f + ink_right;
}

/* File-browser header row. One icon size and one square hover-chip size for
 * every control in the row (up / refresh / close, plus the leading folder
 * glyph), so the three buttons are interchangeable shapes rather than each
 * being sized by whatever space happened to be around it. Both go through
 * ui_icon_box_px, which snaps them to an even pixel count. */
#define FB_HDR_ICON_PT         14.0f
#define FB_HDR_CHIP_PT         20.0f
#define FB_HDR_PAD_PT          FB_PANEL_PAD_PT   /* the icons' optical margin from the right edge */
#define FB_HDR_BTN_W_CELLS      3.0f   /* click box width, in UI cells */

/* X of the FB header's RIGHT-MOST action button box (the close ✕).
 *
 * The icons are centred inside 3-cell click boxes, so anchoring the BOX to the
 * padding is not the same as anchoring the icon: the box carries (btn_w -
 * icon_w)/2 of centring slack, and the Lucide art carries a little more of its
 * own built-in padding on top. Together those pushed the row visibly further
 * from the right edge than the breadcrumb sits from the left — the icons read
 * as "not aligned". Anchor the icon's INK instead, exactly as ui_tab_close_x
 * does for the tab pill's ✕.
 *
 * Pass `ink_right` from icon_ink_insets(ICON_CLOSE, icon_w, ...), or 0 where
 * the caller has no icon metrics (the hit test) — the box then lands within a
 * pixel or two, which is inside the click target either way. Render and hit
 * test MUST both come through here. */
static inline f32 fb_hdr_close_x(f32 panel_x, f32 panel_w, f32 cw,
                                 f32 icon_w, f32 ink_right, f32 dpi) {
    f32 btn_w = FB_HDR_BTN_W_CELLS * cw;
    return panel_x + panel_w - FB_HDR_PAD_PT * dpi
         - (btn_w + icon_w) * 0.5f + ink_right;
}

/* File-browser left rail.
 *
 * The header breadcrumb and every list row hang their leading icon off the SAME
 * x, and they hang it by its INK, not by its box. Two things would otherwise
 * make that column read as ragged:
 *
 *   1. The header and the list must indent by the same amount, or the
 *      breadcrumb folder sits to the right of the folders directly beneath it.
 *   2. Lucide icons are drawn centred in a 24-unit box with per-icon internal
 *      padding. At a 28 px box `folder` insets 1 px but `arrow-up` insets 4 px
 *      and `file-text` 3 px — so ".." looks indented against a run of folders
 *      even when every box is aligned to the pixel.
 *
 * Hence FB_ROW_PAD_PT is where the ink starts, and callers subtract
 * icon_ink_insets()'s `left` from the draw origin to put it there. The name
 * column is measured from the rail plus the nominal BOX (ui_fb_row_text_x),
 * never from the ink-adjusted origin — otherwise the labels would jitter
 * left and right as the icon under them changed. */
#define FB_ROW_PAD_PT          16.0f
#define FB_LIST_ICON_PT        16.0f
#define FB_LIST_ICON_GAP_PT     6.0f
#define FB_TREE_INDENT_PT      16.0f
#define FB_TREE_DISCLOSURE_PT  12.0f
#define FB_TREE_DISCLOSURE_GAP_PT 6.0f

/* Ink rail x, and the text column that follows it. `panel_x` is the panel's
 * left edge; both the header and the row loop derive from these two. */
static inline f32 ui_fb_row_icon_x(f32 panel_x, f32 dpi) {
    return panel_x + FB_ROW_PAD_PT * dpi;
}
static inline f32 ui_fb_row_text_x(f32 panel_x, f32 icon_box, f32 dpi) {
    return ui_fb_row_icon_x(panel_x, dpi) + icon_box + FB_LIST_ICON_GAP_PT * dpi;
}

/* Toolbar icon buttons */
#define TB_ICON_SIZE_PT        14.0f
/* The sidebar/panel toggle is the primary chrome affordance, so its button box
 * (hover pill + hit target + glyph) is a touch larger than the other toolbar
 * glyphs. Render (ui.c) and hit-test (hittest.c) both derive from this so the
 * box, glyph, separator gap and click target stay in lockstep.
 *
 * Lucide's panel-left fills far more of its box than a glyph with its own
 * generous inset, so a larger multiplier paints a visibly bigger icon that
 * crowds the traffic lights. 1.08× gives the intended optical size difference
 * rather than a nominal one. */
#define TB_SIDEBAR_ICON_SIZE_PT (TB_ICON_SIZE_PT * 1.08f)
/* Agent icons in tab pills are full-bleed PNG rasters — unlike icon_draw's SF
 * symbols they carry no glyph inset, so an equal-sized box paints visibly more
 * ink. Trimming the box ~10 % lands them on the same optical size as the
 * toolbar's other icons; at 16 pt they read a third larger than the traffic
 * lights and the whole bar looked ragged. */
#define TB_AGENT_ICON_SIZE_PT   (TB_ICON_SIZE_PT * 0.90f)
/* Gap between consecutive agent icons in a tab pill. Also charged per extra
 * icon when sizing the tab slot, so the two must stay the same number. */
#define TAB_AGENT_ICON_GAP_PT   3.0f
#define TB_BTN_PAD_PT           8.0f
#define TB_BTN_GAP_PT           2.0f

/* Leading toolbar cluster: [traffic lights] gap [toggle] gap [separator] gap
 * [tab strip].
 *
 * The toggle's LEFT gap is not a free choice — it is TRAFFIC_LIGHT_GAP_PT,
 * baked into app->titlebar_left_inset so the icon clears the window buttons.
 * Its RIGHT gap therefore has to be the same number or the icon sits visibly
 * off-centre between the green light and the divider. The separator gets the
 * same padding on both sides for the same reason.
 *
 * Both sides of this arithmetic — the painter in ui.c and the hit test in
 * hittest.c — must walk the cursor identically, or every toolbar element
 * after the divider is hit-tested a few points off from where it was drawn.
 * Hence the helpers.
 *
 * In fullscreen there are no traffic lights and the inset collapses to
 * TOOLBAR_EDGE_PAD_PT (10 pt), a little off this gap — below the threshold
 * where an eye reads asymmetry, and not worth a second layout mode. */
#define TB_SEP_GAP_PT           TRAFFIC_LIGHT_GAP_PT

/* Separator hairline width — at least one physical pixel, scaled on Retina. */
static inline f32 tb_sep_w_px(f32 dpi) {
    return dpi > 1.0f ? dpi : 1.0f;
}
/* X of the divider, given the right edge of the icon box before it. */
static inline f32 tb_sep_x(f32 icon_right_px, f32 dpi) {
    return icon_right_px + TB_SEP_GAP_PT * dpi;
}
/* Where the next element starts after that divider. */
static inline f32 tb_after_sep_x(f32 icon_right_px, f32 dpi) {
    return tb_sep_x(icon_right_px, dpi) + tb_sep_w_px(dpi) + TB_SEP_GAP_PT * dpi;
}

/* Toolbar icon alignment.
 *
 * An icon box that comes out an ODD number of physical pixels centres on a
 * half-pixel row, so it renders half a pixel off its neighbours — enough to
 * read as "the icons don't line up" on a Retina bar full of 24 px glyphs.
 * Snap the box to an even pixel count and derive every icon's Y from one
 * rounded centre row. Render (ui.c) and hit-test (hittest.c) both call these
 * so the drawn box and the click target can't drift apart. */
static inline f32 ui_icon_box_px(f32 size_pt, f32 dpi) {
    f32 px = (f32)((i32)(size_pt * dpi * 0.5f + 0.5f)) * 2.0f;
    return px < 2.0f ? 2.0f : px;
}
/* Snapped centre row of a bar `bar_h` px tall. Pair with an even box size and
 * `y = ui_bar_mid_px(h) - box * 0.5f` lands on a whole pixel. */
static inline f32 ui_bar_mid_px(f32 bar_h) {
    return (f32)((i32)(bar_h * 0.5f + 0.5f));
}

/* Terminal */
#define TERMINAL_PADDING_PT     6.0f

/* Context menus — a tab's, a pane's, the explorer's, a tab group's. ONE set
 * of numbers: ui/ctx_menu.h places the box and the rows from them and
 * ctx_menu_draw_rows (ui.c) paints every menu's rows, so the four cannot
 * come to look different. A menu supplies its rows; nothing else. */
#define CTX_MENU_MIN_W_PT       212.0f
#define CTX_MENU_ROW_H_PT        26.0f
#define CTX_MENU_VPAD_PT          6.0f
#define CTX_MENU_SEP_H_PT         7.0f   /* hairline plus the air around it */
#define CTX_MENU_ICON_PT         15.0f   /* leading Lucide icon box */
#define CTX_MENU_ICON_X_PT       13.0f   /* its left inset inside the menu */
#define CTX_MENU_LABEL_GAP_PT    10.0f   /* icon box -> label */
#define CTX_MENU_KEYS_GAP_PT     22.0f   /* label -> shortcut keys, at least */
#define CTX_MENU_PAD_R_PT        14.0f
#define CTX_MENU_RADIUS_PT        8.0f   /* the panel's corner */
#define CTX_MENU_HL_INSET_PT      4.0f   /* highlighted row: inset from the panel */
#define CTX_MENU_HL_RADIUS_PT     5.0f   /* ...and its corner */
#define CTX_MENU_PANEL_ALPHA      0.985f /* the panel's fill */

/* Alias of the pane menu's icon size, for the popover that borrows it. */
#define TERM_MENU_ICON_PT       CTX_MENU_ICON_PT

/* Scrollback scrollbar width — shared by the renderer and the hit test, which
 * must agree or the grab zone sits beside the painted bar. */
#define SCROLLBAR_W_PT           8.0f

/* Scroll-to-bottom button — a floating round button pinned to the pane's
 * bottom-right corner, shown only while that pane's view is scrolled up into
 * scrollback. Sized like the tab-bar chips so it reads as chrome, not as a
 * terminal cell. The margin clears the grid's edges and doubles as the
 * clearance kept from the scrollbar when that is visible. */
#define SCROLL_BTN_SIZE_PT      30.0f
#define SCROLL_BTN_MARGIN_PT    10.0f
#define SCROLL_BTN_ICON_RATIO    0.46f

/* Prompt recall — the "what did I ask this agent?" chip pinned to a pane's
 * TOP-right corner, and the panel a click on it opens. Mirrors the scroll
 * button's margin so the two floating controls sit on the same inset from
 * opposite corners of the same pane.
 *
 * The widths are in CHARACTER COLUMNS of the chrome cell, not points: the
 * content is text, so what has to be bounded is how much of it fits. Both are
 * additionally clamped to a fraction of the pane, which is what keeps the chip
 * off the agent's own top-right chrome in a narrow split. */
#define PROMPT_RECALL_MARGIN_PT     14.0f  /* matches the voice card's inset */
#define PROMPT_RECALL_PAD_X_PT      12.0f
#define PROMPT_RECALL_CHIP_PAD_Y_PT 10.0f
#define PROMPT_RECALL_PANEL_PAD_Y_PT 12.0f
#define PROMPT_RECALL_RADIUS_PT     UI_PILL_RADIUS_PT
#define PROMPT_RECALL_CHIP_COLS     30     /* summary width when collapsed */
#define PROMPT_RECALL_PANEL_COLS    46     /* text width when expanded */
#define PROMPT_RECALL_PANEL_LINES   10     /* wrapped lines before it clips */
#define PROMPT_RECALL_MAX_PANE_FRAC  0.62f /* never wider than this much pane */
#define PROMPT_RECALL_MIN_PANE_COLS  24    /* narrower panes get the icon only */

/* Attachment tiles.
 *
 * A prompt is not always only words — the user drags a screenshot in and the
 * agent answers about a picture — so the card shows what was handed over. Two
 * sizes, because the card has two states and they answer different questions:
 * the chip only has to say "there were pictures" (a strip of thumbs the height
 * of one text line, which is all the room a one-line badge has), while the
 * panel has to let the user RECOGNISE them, which is what 56 pt buys.
 *
 * The tile width follows the image's aspect between MIN and MAX rather than
 * being square: a terminal screenshot cropped to a square is unrecognisable,
 * and letterboxing it inside one wastes the row. Beyond the clamp the picture
 * is contained (never cropped) and centred in the tile.
 *
 * INSET is what makes the rounding work. The renderer's image path draws a
 * rectangle — there is no rounded-image primitive and adding one is a change to
 * two shader backends — so the tile is a rounded rect and the picture sits
 * inside it by INSET points. With INSET >= RADIUS * (1 - 1/sqrt2) ~= 0.3 *
 * RADIUS the picture's square corners land inside the rounded boundary, so what
 * shows is a rounded mat around a photo rather than four corners poking out. */
#define PROMPT_RECALL_TILE_H_PT      56.0f
#define PROMPT_RECALL_TILE_MIN_W_PT  34.0f
#define PROMPT_RECALL_TILE_MAX_W_PT  92.0f
#define PROMPT_RECALL_TILE_GAP_PT     6.0f
#define PROMPT_RECALL_TILE_RADIUS_PT  6.0f
#define PROMPT_RECALL_TILE_INSET_PT   2.0f
/* Mini tiles in the collapsed chip: height is the chrome cell, so they sit on
 * the text baseline band, and the width clamp is expressed as a multiple of it
 * for the same reason. Three is what fits before the summary has nothing left. */
#define PROMPT_RECALL_CHIP_TILES      3
#define PROMPT_RECALL_CHIP_TILE_MIN   0.62f  /* x the chrome cell height */
#define PROMPT_RECALL_CHIP_TILE_MAX   1.70f
#define PROMPT_RECALL_CHIP_TILE_GAP_PT 3.0f
#define PROMPT_RECALL_CHIP_TILE_RADIUS_PT 3.0f
/* Appear/disappear uses the shared modal duration so the card arrives on the
 * same curve as the palette, the dialogs and the voice HUD. The chip→panel
 * grow is its own, slightly slower and eased in AND out, because it is one
 * object changing shape rather than a surface arriving. */
#define PROMPT_RECALL_CARD_SEC      0.24f
#define PROMPT_RECALL_GROW_SEC      0.26f

/* Vertical placement of the cell grid inside its pane.
 *
 * A grid is a whole number of rows; a pane is whatever height the layout hands
 * it, so `pane_h - rows * cell_h` pixels are always left over (up to one full
 * row — ~21 pt at a 16 pt font). The grid hangs from the top of its pane and
 * the leftover lies below the last row: the first row then sits the same
 * distance under the tab strip at every font size and window height. Split
 * above and below, the gap over the first row moved between nothing and half a
 * row as either changed.
 *
 * EVERY consumer must apply this: the renderer (grid origin, and therefore
 * cursor, selection and inline images) and the hit test (pixel → row). Applying
 * it in one and not the other puts clicks on the wrong line. */
/* Vertical reserve below the grid, in points.
 *
 * Horizontally the configured terminal padding still applies — that is what
 * keeps text off the window edge — with the leftover centred on top of it by
 * term_grid_pad_x below. Vertically the leftover below the last row is the
 * padding; this is a floor so a grid that happens to fit exactly still clears
 * the status bar. */
#define TERM_GRID_MIN_VPAD_PT   3.0f
static inline f32 term_grid_pad_y(f32 box_h, i32 rows, f32 cell_h) {
    (void)box_h; (void)rows; (void)cell_h;
    return 0.0f;
}

/* The same leftover, horizontally — which is why this sits next to its twin
 * rather than inside the layout code.
 *
 * A grid is a whole number of columns, so `pane_w - cols * cell_w` pixels are
 * always left over here too (up to one full cell). Left unclaimed they would
 * all sit to the RIGHT of the last column — the configured padding on the
 * left, that padding PLUS the leftover on the right — and a window is wide
 * enough that the eye compares its two edges directly.
 *
 * The leftover is arithmetic: only a window whose width happens to be a
 * multiple of the cell has none. Centring it — half each side — makes what
 * remains read as padding on both edges instead of a gap on one.
 *
 * Same two rules as the vertical twin. Derive it from the terminal's ACTUAL
 * `cols`, not a recomputed count, so it cannot disagree with the grid that is
 * really allocated (mid-resize, before the PTY has caught up, included). And
 * EVERY consumer must apply it — the renderer (grid origin, hence the cursor,
 * the selection wash, the image badges) and the hit test (pixel -> column).
 * Applying it in one and not the other puts clicks on the wrong column. */
static inline f32 term_grid_pad_x(f32 box_w, i32 cols, f32 cell_w) {
    if (cols <= 0 || cell_w <= 0.0f) return 0.0f;
    f32 left = box_w - (f32)cols * cell_w;
    if (left <= 0.0f) return 0.0f;
    /* Whole pixels, for a stronger reason than the vertical one: the terminal
     * atlas samples GL_NEAREST, which is only safe because the glyph origin is
     * pixel-exact on all four axes. A half-pixel X would smear every vertical
     * stem in the pane. */
    return (f32)(i32)(left * 0.5f);
}

/* File viewer */
#define VIEWER_TITLE_H_PT       30.0f
#define VIEWER_TITLE_PAD_PT     12.0f
#define VIEWER_CLOSE_W_PT       28.0f
#define VIEWER_TOGGLE_W_PT      124.0f   /* roomy segmented control: "Read"/"Edit", "Image"/"Code" */
#define VIEWER_WIDTH_RATIO       0.45f  /* initial width as fraction of window */
#define VIEWER_MIN_PT           300.0f
#define VIEWER_LIST_MIN_PT      200.0f  /* min list-pane width in the fb tab split */
/* Width of the docked explorer rail a document view (code / markdown / graph)
 * opens with. Wider than the hard minimum above on purpose: at 200pt a name
 * like "filebrowser.c" plus its icon, indent and size column runs out of room
 * and every third row ends in an ellipsis. Matches the docked sidebar's own
 * default so the two file lists are the same size. */
#define VIEWER_RAIL_PT          248.0f
#define VIEWER_RESIZE_GRAB_PT    5.0f
/* Terminal width the viewer's drag may never eat into. The viewer
 * reserves grid columns (app_file_viewer_w), so without a floor a drag
 * to the left edge would reflow the terminal to one column. */
#define VIEWER_TERM_KEEP_PT    160.0f

/* Code/plain-text editor chrome (FVIEW_CODE / FVIEW_TEXT).
 *
 * The status bar sits BELOW the text area, so it shortens the drawable
 * height. Everything that turns pixels into a line index — visible_lines
 * here, fb_editor_click_caret, the scroll clamps — has to agree on that,
 * which is why the height is a constant and not a literal at the draw site. */
#define VIEWER_STATUS_H_PT       22.0f  /* Ln/Col · language · encoding bar */
#define CODE_GUTTER_MIN_DIGITS      3   /* narrowest line-number column */
#define CODE_GUTTER_PAD_COLS      2.0f  /* cells of breathing room around it */
#define CODE_INDENT_GUIDE_COLS      4   /* one guide every N columns of indent */

/* Sites / dev-server manager overlay */
#define SITES_HEADER_H_PT       52.0f
#define SITES_FOOTER_H_PT       46.0f
#define SITES_LIST_W_PT        300.0f
/* List row height is computed at render time from the font's real glyph aspect
 * (see sites_compute_layout) so the 3 stacked text lines never overflow. */
#define SITES_LOG_PAD_PT         8.0f

#endif
