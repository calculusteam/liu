/*
 * Liu — centralized UI hit testing
 */
#ifndef UI_HITTEST_H
#define UI_HITTEST_H

#include "core/types.h"

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
    HIT_SPLIT_DIVIDER,
    HIT_SCROLLBAR,
    HIT_STATUS_BAR,
    HIT_VIEWER,
    HIT_VIEWER_TITLE,
    HIT_VIEWER_CLOSE,
    HIT_VIEWER_MD_TOGGLE,
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
    HIT_FBTAB_VIEWER_MD_TOGGLE,
} HitType;

typedef enum {
    TB_NONE = 0,
    TB_SIDEBAR,
    TB_SSH,
    TB_SETTINGS,
    TB_FONT_DOWN,
    TB_FONT_UP,
} ToolbarButton;

typedef struct {
    HitType type;
    i32     index;
    i32     sub_index;
    f32     rel_x;
    f32     rel_y;
} HitResult;

HitResult ui_hit_test(const struct AppState *app, f32 mx, f32 my);

#endif
