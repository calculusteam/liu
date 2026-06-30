/*
 * Liu — shared layout constants (in points, multiply by dpi_scale)
 * Single source of truth for all UI dimensions.
 */
#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

/* Toolbar (transparent macOS title bar) */
#define TOOLBAR_HEIGHT_PT      36.0f   /* was 42 — dead air under the tab pill read as a gap */
#define TRAFFIC_LIGHT_ZONE_PT  80.0f

/* Chrome geometry */
#define CHROME_RADIUS_PT        0.0f
#define CHROME_LINE_PT          1.0f

/* Status bar */
#define STATUS_BAR_HEIGHT_PT   22.0f

/* Sidebar */
#define SIDEBAR_DEFAULT_PT    240.0f
#define SIDEBAR_MIN_PT        160.0f
#define SIDEBAR_MAX_PT        500.0f
#define SIDEBAR_HEADER_PT      28.0f
#define SIDEBAR_HEADER_PAD_PT  10.0f
#define SIDEBAR_BORDER_W_PT     2.0f
#define SIDEBAR_RESIZE_GRAB_PT  5.0f

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
#define PILL_INNER_PAD_PT      12.5f   /* content padding inside the pill — label sits left, close button right */

/* Toolbar icon buttons */
#define TB_ICON_SIZE_PT        14.0f
/* The sidebar/panel toggle is the primary chrome affordance — its button box
 * (hover pill + hit target + glyph) is a touch larger than the other toolbar
 * glyphs. Render (ui.c) and hit-test (hittest.c) both derive from this so the
 * box, glyph, separator gap and click target stay in lockstep. */
#define TB_SIDEBAR_ICON_SIZE_PT (TB_ICON_SIZE_PT * 1.25f)
#define TB_BTN_PAD_PT           8.0f
#define TB_BTN_GAP_PT           2.0f

/* Terminal */
#define TERMINAL_PADDING_PT     6.0f
#define TERMINAL_TOP_GAP_PT     2.0f

/* File viewer */
#define VIEWER_TITLE_H_PT       30.0f
#define VIEWER_TITLE_PAD_PT     12.0f
#define VIEWER_CLOSE_W_PT       28.0f
#define VIEWER_MD_TOGGLE_W_PT   124.0f   /* roomy "Read"/"Edit" segmented control */
#define VIEWER_WIDTH_RATIO       0.45f  /* initial width as fraction of window */
#define VIEWER_MIN_PT           300.0f
#define VIEWER_LIST_MIN_PT      200.0f  /* min list-pane width in the fb tab split */
#define VIEWER_RESIZE_GRAB_PT    5.0f

/* Sites / dev-server manager overlay */
#define SITES_HEADER_H_PT       52.0f
#define SITES_FOOTER_H_PT       46.0f
#define SITES_LIST_W_PT        300.0f
/* List row height is computed at render time from the font's real glyph aspect
 * (see sites_compute_layout) so the 3 stacked text lines never overflow. */
#define SITES_LOG_PAD_PT         8.0f

#endif
