/*
 * Liu — centralized UI hit testing
 */
#ifndef UI_HITTEST_H
#define UI_HITTEST_H

#include "core/types.h"
#include "ui/find_bar_geom.h"

struct AppState; /* forward declare */

typedef enum {
    HIT_NONE = 0,
    HIT_TAB,
    HIT_TAB_CLOSE,
    HIT_TAB_NEW,
    HIT_TAB_GROUP_HEADER,  /* clicked a tab group header chip */
    HIT_TOOLBAR_BTN,
    HIT_TERMINAL,
    HIT_SIDEBAR,
    HIT_SIDEBAR_ITEM,
    HIT_SIDEBAR_RESIZE,
    /* Sectioned sidebar (ui/sidebar_layout.h). */
    HIT_SIDEBAR_SECTION,   /* index = SidebarSection; sub_index: 0 header (toggle),
                            * 1/2 the header's actions, 3 a rail button (expand) */
    HIT_SIDEBAR_AGENT,     /* index = tab, sub_index = pane */
    HIT_SIDEBAR_NOTE,      /* index = row in AppState.side_notes */
    HIT_SPLIT_DIVIDER,
    HIT_SCROLLBAR,
    HIT_SCROLL_BOTTOM,     /* floating "jump to bottom" button in a pane;
                            * `index` selects app->scroll_btn[] */
    HIT_STATUS_BAR,
    HIT_STATUS_GIT,        /* git segment inside the status bar → diff dock */
    HIT_VIEWER,
    HIT_VIEWER_TITLE,
    HIT_VIEWER_CLOSE,
    HIT_VIEWER_TOGGLE,
    HIT_VIEWER_RESIZE,
    /* File-browser tab content (replaces HIT_TERMINAL when active tab
     * has kind == TAB_FILEBROWSER). */
    HIT_FBTAB_HEADER,         /* header row inside the FB tab */
    HIT_FBTAB_ITEM,           /* list row */
    HIT_FBTAB_BODY,           /* empty list area */
    HIT_FBTAB_DIVIDER,        /* vertical divider between list and viewer */
    HIT_FBTAB_VIEWER,         /* file viewer area (right pane) */
    HIT_FBTAB_VIEWER_TITLE,
    HIT_FBTAB_VIEWER_CLOSE,
    HIT_FBTAB_VIEWER_TOGGLE,
    /* An SFTP tab's content: ui/sftp_layout.h says what is under the point
     * (sftp_tab_mouse_down asks it). */
    HIT_SFTP,
    /* Browser chrome (URL bar / toolbar — page is native WebView) */
    HIT_BROWSER_CHROME,
    HIT_BROWSER_BACK,
    HIT_BROWSER_FWD,
    HIT_BROWSER_RELOAD,
    HIT_BROWSER_URL,
    HIT_BROWSER_DEVICE,
    HIT_BROWSER_PICK,
    HIT_BROWSER_ATTACH,
    HIT_BROWSER_EXTERNAL,
    HIT_BROWSER_CLOSE,
    /* The terminal Find bar (Cmd+F) while it is open; sub_index is a
     * FindBarPart (ui/find_bar_geom.h): the toggles, the close keycap, or the
     * body, which is consumed so a press on the bar never reaches the grid. */
    HIT_FIND_BAR,
} HitType;

typedef enum {
    TB_NONE = 0,
    TB_SIDEBAR,
    TB_SSH,
    TB_SETTINGS,
    TB_FONT_DOWN,
    TB_FONT_UP,
    TB_UPDATE,        /* update-available chip, toolbar far right */
    TB_BROADCAST,     /* broadcast indicator chip, left of the update chip */
    TB_SECURE_INPUT,  /* secure-input chip, left of those two */
    /* Liu's own window controls at the far right, where the window manager
     * draws none (X11). */
    TB_WIN_CLOSE,
    TB_WIN_MAXIMIZE,
    TB_WIN_MINIMIZE,
} ToolbarButton;

typedef struct {
    HitType type;
    i32     index;
    i32     sub_index;
    f32     rel_x;
    f32     rel_y;
} HitResult;

HitResult ui_hit_test(const struct AppState *app, f32 mx, f32 my);

/* The Find bar's parts where it RESTS (no open animation), from this window's
 * width, tab-bar height and DPI. The renderer draws from it (translated by the
 * slide-in offset) and the hit test routes presses by it, so the two cannot
 * disagree about where a toggle is. */
FindBarGeom ui_find_bar_geom(const struct AppState *app);

/* Top and bottom edge of the terminal body, in framebuffer pixels. The same
 * numbers ui_hit_test derives its rows from, so a caller asking "is the
 * pointer past the grid?" cannot disagree with the caller asking "which row
 * is it on?". Either output pointer may be NULL. */
void      ui_terminal_vspan(const struct AppState *app, f32 *out_top, f32 *out_bottom);

/* Left edge and width of the terminal body, in framebuffer pixels. The edge
 * is the sidebar's LIVE width (the grid moves with the bar while it slides),
 * the width is the one the grid was last laid out against — the two differ
 * only mid-slide, and then the grid keeps its size and translates. The
 * renderer (app_terminal_content_rect) and the hit test both come here. */
void      ui_terminal_hspan(const struct AppState *app, f32 *out_left, f32 *out_w);

/* Project a pointer position onto pane `pane`'s grid and CLAMP it: the answer
 * is always a valid cell of that pane's terminal, and `out_inside` says
 * whether the pointer was actually over the grid. A selection drag needs
 * exactly this. Once the button is down the pointer goes wherever the hand
 * takes it — into the tab bar, the sidebar, past the right edge, over the
 * other pane of a split — and the selection has to keep following it to the
 * nearest cell, the way every terminal's does. ui_hit_test cannot answer
 * that: it stops at the first surface that claims the point, and above the
 * grid that is never the terminal. Same origin arithmetic as its
 * HIT_TERMINAL branch, so the two cannot disagree about which cell a point is
 * on. `out_top`/`out_bottom` are the grid's own vertical edges, for a caller
 * deciding whether the pointer is past them. Any output may be NULL. False
 * when the pane has no terminal or the font has no metrics yet. */
bool      ui_terminal_pane_cell(const struct AppState *app, i32 pane, f32 x, f32 y,
                                i32 *out_col, i32 *out_row, bool *out_inside,
                                f32 *out_top, f32 *out_bottom);
/* Where pane `pane`'s scrollbar goes: `out_right` is the RIGHT edge of the
 * pane's slice of the terminal body (the bar's grab column ends there) and
 * `out_top`/`out_bottom` are its grid's vertical edges. Every pane of a split
 * carries its own bar; the hit test and the thumb drag take the placement from
 * here, and the renderer is handed the same edge by the layout it already
 * walks. Any output may be NULL. False when the pane has
 * no terminal or the font has no metrics yet. */
bool      ui_terminal_pane_scrollbar(const struct AppState *app, i32 pane,
                                     f32 *out_right, f32 *out_top, f32 *out_bottom);
/* The same projection, plus which HALF of the cell the point is in: true
 * when its nearest cell edge is the one after the cell. A selection drag is
 * placed on edges, not cells (TermSelection.edge_mode). The half is VISUAL —
 * a caller on a reordered (RTL) row maps it with the column. A point left or
 * right of the grid lands on the grid's outer edge. */
bool      ui_terminal_pane_point(const struct AppState *app, i32 pane, f32 x, f32 y,
                                 i32 *out_col, i32 *out_row, bool *out_right_half,
                                 bool *out_inside, f32 *out_top, f32 *out_bottom);

#endif
