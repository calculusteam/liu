/*
 * Settings geometry shared by the renderer and hit-test. Each control's
 * geometry is defined once so clicks target the control drawn under the
 * pointer. These functions use only layout inputs, not renderer or
 * `AppState` state.
 */
#ifndef UI_SETTINGS_LAYOUT_H
#define UI_SETTINGS_LAYOUT_H

#include "core/types.h"

typedef struct { f32 x, y, w, h; } SettingsRect;
bool settings_rect_contains(SettingsRect r, f32 x, f32 y);

/* Shell and grouped navigation share geometry with pointer handling. */
typedef struct {
    f32 px, py, panel_w, panel_h, title_h;
    f32 nav_w, nav_row_h, nav_top, nav_bottom, nav_group_h, nav_group_gap;
    f32 nav_content_h;
    f32 content_x, content_top, content_w, clip_top, clip_bottom;
    bool nav_icon_only;
    SettingsRect close, reset;
} SettingsShellLayout;
SettingsShellLayout settings_shell_layout(f32 sw, f32 sh, f32 dpi,
                                          f32 scale, f32 yoff, i32 nav_count);
SettingsRect settings_nav_row(SettingsShellLayout l, i32 index, f32 scroll, f32 dpi);
f32 settings_nav_group_y(SettingsShellLayout l, i32 first, f32 scroll, f32 dpi);

typedef struct {
    SettingsRect access, toggle, input, submit;
    f32 x, w, dpi, local_y, heading_y, hint_y, status_y, list_y, row_h;
} SettingsBrowserLayout;
typedef struct {
    SettingsRect row, edit, remove;
    f32 text_x, title_y, detail_y, text_w;
} SettingsBrowserRow;
SettingsBrowserLayout settings_browser_layout(f32 x, f32 y, f32 w, f32 dpi);
SettingsBrowserRow settings_browser_row(SettingsBrowserLayout l, i32 index);

/* Everything a theme tile needs, in pixels. */
typedef struct {
    f32 x, y, w, h;                 /* the card itself                      */
    f32 swatch_x, swatch_y;         /* colour-preview strip inside the card */
    f32 swatch_w, swatch_h;
    f32 name_x, name_y;             /* baseline origin of the theme name    */
    f32 badge_x, badge_y, badge_sz; /* selected tick, on the swatch          */
    f32 del_x, del_y, del_sz;       /* delete GLYPH box, on the name row     */
    /* The delete BUTTON — the glyph box plus its padding. This is the rect
     * that gets a hover background and the rect the click lands in; the glyph
     * is drawn centred inside it. One rect for both, so every clickable pixel
     * also shows the hover. */
    f32 del_btn_x, del_btn_y, del_btn_sz;
} ThemeCardRect;

/* Height of one theme card for the given UI cell height. The grid pitch is
 * this plus settings_theme_gap(). */
f32 settings_theme_card_h(f32 ch, f32 dpi);

/* Colour-preview strip height, and the height of the ANSI band inside it.
 * The strip is sized from `ch` so the prompt mark drawn at the top never
 * overlaps the band at the bottom. */
f32 settings_theme_swatch_h(f32 ch, f32 dpi);
f32 settings_theme_band_h(f32 dpi);

/* Vertical gap between theme rows. */
f32 settings_theme_gap(f32 dpi);

/* Geometry of theme tile `index` in a `cols`-wide grid whose content area
 * starts at (origin_x, origin_y) and is `content_w` wide. */
ThemeCardRect settings_theme_card(i32 index, i32 cols,
                                  f32 origin_x, f32 origin_y, f32 content_w,
                                  f32 ch, f32 dpi);

/* Is (px, py) inside tile `index`? Uses the same rect the renderer draws. */
bool settings_theme_card_hit(i32 index, i32 cols,
                             f32 origin_x, f32 origin_y, f32 content_w,
                             f32 ch, f32 dpi, f32 px, f32 py);

/* Total height of a `count`-item grid, including the trailing margin. */
f32 settings_theme_grid_h(i32 count, i32 cols, f32 ch, f32 dpi);

/* ---------------------------------------------------------------------------
 * Text Weight — the block under Size / Line Height: a heading row and two
 * −/+ steppers (Terminal, Interface).
 *
 * The Appearance tab lays itself out by accumulating a y and the hit test
 * accumulates the same y again 3500 lines later, so this block's rows are
 * computed once, here, from the y the block STARTS at. `end` is where the next
 * section (Theme) starts, before that section's own leading gap.
 * ------------------------------------------------------------------------- */
typedef struct {
    f32 head_y;   /* "Text Weight" heading row                       */
    f32 term_y;   /* Terminal stepper row (top; height is item_h)    */
    f32 ui_y;     /* Interface stepper row                           */
    f32 end;      /* first y after the block                         */
} SettingsWeightBlock;

SettingsWeightBlock settings_weight_block(f32 y, f32 item_h, f32 dpi);

/* One click on a stepper half: `dir` is -1 (the − half) or +1. Moves `*w` by
 * TEXT_WEIGHT_STEP, snapped to the step grid and clamped to
 * TEXT_WEIGHT_MIN..MAX; a value within half a step of zero becomes exactly 0
 * (never -0.0, which would print as "-0.0"). Returns true when the value
 * changed — a click at the end of the scale changes nothing and must not
 * trigger a save. */
bool settings_weight_step(f32 *w, i32 dir);

/* Text for a stepper's value: "0.0" at zero, otherwise a signed "+0.3" /
 * "-0.5". Writes at most `n` bytes. */
void settings_weight_str(char *out, usize n, f32 w);

/* ---------------------------------------------------------------------------
 * Action buttons — "Create Theme", "Import Theme", "Add Custom Font".
 *
 * One shape for every icon+label control in the panel, matching the Reset
 * button pinned to the nav column: bordered rounded rect, vector icon and
 * label centred as a group, 7 pt corners.
 *
 * Width comes from the label's own length rather than a hand-counted
 * constant, so a changed label still fits, and both the renderer and the hit
 * test call the same function, so the painted button and its click zone
 * cannot drift.
 * ------------------------------------------------------------------------- */
typedef struct {
    f32 x, y, w, h;
    f32 radius;
    f32 icon_x, icon_y, icon_sz;  /* leading vector icon, box-aligned */
    f32 label_x, label_y;         /* glyph origin of the label          */
    i32 label_max;                /* cells that fit before the trailing pad */
} SettingsActionBtn;

/* Row metrics. Height matches the Reset button so every action affordance in
 * Settings is the same object at a different width. */
f32 settings_action_h(f32 ch, f32 dpi);
f32 settings_action_gap(f32 dpi);

/* Content-sized width for a `label_cells`-cell label plus its icon. */
f32 settings_action_w(i32 label_cells, f32 cw, f32 ch, f32 dpi);

/* Lay out a button of exactly `w` at (x, y). The icon+label group is centred,
 * so a pair given the same `w` reads as an aligned pair no matter which label
 * is longer. Falls back to left-anchored at the minimum pad when the group is
 * wider than the box. */
SettingsActionBtn settings_action_btn(f32 x, f32 y, f32 w, i32 label_cells,
                                      f32 cw, f32 ch, f32 dpi);

bool settings_action_hit(const SettingsActionBtn *b, f32 px, f32 py);

/* The Appearance tab's three actions. The labels live next to the geometry
 * because the geometry is measured FROM them — a label declared in the
 * renderer and a width computed in the hit test is exactly the drift this
 * file exists to prevent. */
/* Diagnostics tab. Same rule as the Appearance labels below: the button width
 * is measured FROM the label, so the two must be declared together. */
#define SETTINGS_LBL_DIAG_RUN     "Run checks"
#define SETTINGS_LBL_DIAG_FIXALL  "Fix what Liu can"

#define SETTINGS_LBL_CREATE_THEME  "Create Theme"
#define SETTINGS_LBL_IMPORT_THEME  "Import Theme"
#define SETTINGS_LBL_ADD_FONT      "Add Custom Font"

/* The About tab's config actions. Both write config.json; the second then hands
 * it to the system editor. The hint on the Terminal tab names the same button,
 * so the strings live here where both can reach them. */
#define SETTINGS_LBL_CFG_SAVE      "Save Config"
#define SETTINGS_LBL_CFG_OPEN      "Open in Editor"

/* "Add Custom Font" — right-aligned on the Font row, centred on `row_y`
 * (which is the row's TEXT origin, not the button's top edge). */
SettingsActionBtn settings_action_row_font(f32 content_x, f32 content_w,
                                           f32 row_y, f32 cw, f32 ch, f32 dpi);

typedef struct {
    SettingsActionBtn add;
    f32 value_x, value_w, grid_y;
} SettingsFontHeader;
SettingsFontHeader settings_font_header(f32 x, f32 y, f32 w, f32 cw, f32 ch, f32 dpi);

/* The track stays fixed when the switch changes state. Right-align the state
 * word with a `SETTINGS_SWITCH_GAP_PT` gap before the track, and size the rect
 * for the longer label. */
#define SETTINGS_LBL_ON   "On"
#define SETTINGS_LBL_OFF  "Off"
#define SETTINGS_SWITCH_GAP_PT  8.0f   /* word -> track, the same for both words */

typedef struct {
    f32 x, y, w, h;                          /* hover and click rect         */
    f32 track_x, track_y, track_w, track_h;
    f32 knob_x, knob_y, knob_sz;
    f32 word_x, word_y;                      /* glyph origin of the word     */
    const char *word;                        /* SETTINGS_LBL_ON / _OFF       */
} SettingsSwitch;

/* The rect's width: every hit test asks this, never a cell count of its own. */
f32 settings_switch_w(f32 cw, f32 dpi);
SettingsSwitch settings_switch(f32 x, f32 y, bool on, f32 cw, f32 ch, f32 dpi);

typedef struct {
    SettingsRect minus, plus;
    f32 value_x, value_w;
} SettingsStepper;
SettingsStepper settings_stepper(f32 x, f32 y, f32 w, f32 ch, f32 dpi);
bool settings_scrollback_step(i32 *lines, i32 direction);
f32 settings_section_h(f32 ch, f32 dpi);

/* "Customize Sidebar" — on the Terminal tab, in the VALUE column of its own
 * row (where the sidebar switches above it sit), centred on `row_y` the same
 * way. It opens the popover (ui/sidebar_customize.h); Cmd+K and this button
 * are the two ways in, so the docked bar spends no permanent row on a control
 * used once. */
#define SETTINGS_LBL_CUSTOMIZE_SIDEBAR "Customize Sidebar"
SettingsActionBtn settings_action_row_sidebar(f32 value_x, f32 value_room,
                                              f32 row_y, f32 cw, f32 ch, f32 dpi);

/* "Create Theme" / "Import Theme" — the pair under the theme grid, both given
 * the same width so the row reads as a pair regardless of label lengths, and
 * both shrunk to half the content width when that no longer fits. Either out
 * pointer may be NULL. */
void settings_action_row_theme(f32 content_x, f32 content_w, f32 row_y,
                               f32 cw, f32 ch, f32 dpi,
                               SettingsActionBtn *out_create,
                               SettingsActionBtn *out_import);

/* ---------------------------------------------------------------------------
 * The Model menu — the popover a Model chip opens (Translate tab).
 *
 * It hangs from the chip that opened it: same left edge, at least as wide,
 * drawn OVER the rows below instead of pushing them down, and upward when the
 * body has no room under the chip.
 *
 * It is one of the app's menus, so its rows are layout.h's CTX_MENU_* — the
 * row height, the rail the check sits in, the highlight's inset and corner.
 *
 * From the top: a head strip (the list's name or its search field, and where
 * the list came from), then an optional PINNED row — Default, "let the agent
 * choose", reachable however far the list is scrolled — a hairline, and a
 * window of `visible` rows over the rest.
 * ------------------------------------------------------------------------- */
#define SETTINGS_MODEL_MENU_ROWS      8   /* the window, before the list scrolls   */
#define SETTINGS_MODEL_MENU_MIN_ROWS  3   /* ...and the least it is squeezed to    */

typedef struct {
    SettingsRect card;      /* the popover                                        */
    SettingsRect head;      /* name or search field, and the list's status        */
    SettingsRect pinned;    /* the Default row; w == 0 when there is none         */
    SettingsRect list;      /* the scrolling rows' window; h == 0 when it is empty
                             * and a pinned row already says something            */
    SettingsRect track;     /* overflow scrollbar; w == 0 when every row fits     */
    f32  row_h, dpi;
    f32  rule_y;            /* hairline under the head                            */
    f32  sep_y;             /* hairline between the pinned row and the list; < 0
                             * when one of the two is missing                     */
    i32  visible;           /* rows the window holds                              */
    bool above;             /* opened upward                                      */
} SettingsModelMenu;

/* `chip` is the control the menu belongs to, `band_top`/`band_bottom` the
 * scrolled body's clip, `rows` the SCROLLING rows (the pinned one not counted).
 * With no row of either kind the window is one row tall, for the line that
 * says nothing matched. */
SettingsModelMenu settings_model_menu(SettingsRect chip, f32 band_top, f32 band_bottom,
                                      f32 content_x, f32 content_w,
                                      bool pinned, i32 rows, f32 dpi);

/* Row `vi` of the window (0 = its first visible row): the rect a click lands
 * in. The highlight is that rect inset from the panel, and from the scrollbar
 * when there is one. */
SettingsRect settings_model_menu_row(const SettingsModelMenu *m, i32 vi);
SettingsRect settings_model_menu_highlight(const SettingsModelMenu *m, SettingsRect row);

/* What is under a point: a window row (>= 0), or one of these. */
#define SETTINGS_MODEL_HIT_NONE    (-1)   /* outside the card                    */
#define SETTINGS_MODEL_HIT_CHROME  (-2)   /* on the card, on nothing that acts   */
#define SETTINGS_MODEL_HIT_HEAD    (-3)
#define SETTINGS_MODEL_HIT_PINNED  (-4)
#define SETTINGS_MODEL_HIT_TRACK   (-5)   /* the scrollbar's column: scrolls, picks nothing */
i32 settings_model_menu_hit(const SettingsModelMenu *m, f32 px, f32 py);

/* Scroll arithmetic over `rows` scrolling rows and a window of `visible`:
 * a scroll kept in range, and the nearest one that shows `row`. */
i32 settings_model_menu_clamp_scroll(i32 scroll, i32 rows, i32 visible);
i32 settings_model_menu_reveal(i32 scroll, i32 row, i32 rows, i32 visible);

#endif /* UI_SETTINGS_LAYOUT_H */
