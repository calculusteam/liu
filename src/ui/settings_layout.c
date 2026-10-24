/*
 * Liu — Settings geometry. See settings_layout.h.
 */
#include "ui/settings_layout.h"
#include "core/config.h"   /* TEXT_WEIGHT_MIN/MAX/STEP */
#include "ui/layout.h"     /* CTX_MENU_*: the model menu is one of the app's menus */

#include <string.h>
#include <math.h>
#include <stdio.h>

/* Card = colour strip on top, name underneath. The strip is what makes a
 * theme pickable at a glance; a list of names alone forces the user to select
 * each one to find out what it looks like. */
/* The swatch has to hold the prompt mark AND the colour band without them
 * touching. The text is `ch` tall, so a fixed strip height would clip the
 * underscore of ">_" at some cell sizes; the height is derived from ch so the
 * two cannot collide. */
#define THEME_BAND_H_PT     6.0f   /* colour band along the strip's bottom */
#define THEME_PAD_PT        6.0f
#define THEME_GAP_PT        8.0f
#define THEME_COL_GAP_PT    8.0f
#define THEME_DEL_PAD_PT    3.0f   /* glyph box -> delete button box */

f32 settings_theme_gap(f32 dpi) { return THEME_GAP_PT * dpi; }

/* Prompt text + a gap + the colour band, with padding above and below. */
f32 settings_theme_swatch_h(f32 ch, f32 dpi) {
    return ch + (THEME_BAND_H_PT + 9.0f) * dpi;
}

f32 settings_theme_band_h(f32 dpi) { return THEME_BAND_H_PT * dpi; }

f32 settings_theme_card_h(f32 ch, f32 dpi) {
    return settings_theme_swatch_h(ch, dpi) + ch + 3.0f * THEME_PAD_PT * dpi;
}

ThemeCardRect settings_theme_card(i32 index, i32 cols,
                                  f32 origin_x, f32 origin_y, f32 content_w,
                                  f32 ch, f32 dpi) {
    ThemeCardRect c = {0};
    if (cols < 1) cols = 1;

    f32 col_w = content_w / (f32)cols;
    f32 pad   = THEME_PAD_PT * dpi;
    f32 gapx  = THEME_COL_GAP_PT * dpi;

    c.w = col_w - gapx;
    if (c.w < 1.0f) c.w = 1.0f;
    c.h = settings_theme_card_h(ch, dpi);
    c.x = origin_x + (f32)(index % cols) * col_w;
    c.y = origin_y + (f32)(index / cols) * (c.h + settings_theme_gap(dpi));

    c.swatch_x = c.x + pad;
    c.swatch_y = c.y + pad;
    c.swatch_w = c.w - 2.0f * pad;
    c.swatch_h = settings_theme_swatch_h(ch, dpi);
    if (c.swatch_w < 1.0f) c.swatch_w = 1.0f;

    c.name_x = c.x + pad + 2.0f * dpi;
    c.name_y = c.swatch_y + c.swatch_h + pad;

    /* The tick sits on the swatch's top-right corner. */
    c.badge_sz = ch + 2.0f * dpi;
    c.badge_x  = c.x + c.w - pad - c.badge_sz;
    c.badge_y  = c.swatch_y + 2.0f * dpi;

    /* Delete gets its own slot on the NAME row, not the swatch: over a strip
     * of arbitrary theme colours a small tinted button has no dependable
     * contrast, so it read as a smudge. On the card surface it has a known
     * background and can be drawn as a proper icon. */
    c.del_sz = ch;
    c.del_x  = c.x + c.w - pad - c.del_sz;
    c.del_y  = c.name_y;

    /* Padding expands the clickable target beyond the 16 pt glyph and defines
     * the hover-background surface. */
    f32 dpad = THEME_DEL_PAD_PT * dpi;
    c.del_btn_sz = c.del_sz + 2.0f * dpad;
    c.del_btn_x  = c.del_x - dpad;
    c.del_btn_y  = c.del_y - dpad;
    return c;
}

bool settings_theme_card_hit(i32 index, i32 cols,
                             f32 origin_x, f32 origin_y, f32 content_w,
                             f32 ch, f32 dpi, f32 px, f32 py) {
    ThemeCardRect c = settings_theme_card(index, cols, origin_x, origin_y,
                                          content_w, ch, dpi);
    return px >= c.x && px < c.x + c.w && py >= c.y && py < c.y + c.h;
}

/* ---- Action buttons. See the header for why the geometry lives here. ---- */

#define ACTION_PAD_PT       12.0f   /* box edge -> content, when not centred  */
#define ACTION_ICON_GAP_PT   6.0f   /* icon -> label                          */
#define ACTION_GAP_PT        8.0f   /* button -> button                       */
#define ACTION_RADIUS_PT     7.0f
#define ACTION_VPAD_PT      14.0f   /* total, split above/below the text cell */

f32 settings_action_h(f32 ch, f32 dpi)  { return ch + ACTION_VPAD_PT * dpi; }
f32 settings_action_gap(f32 dpi)        { return ACTION_GAP_PT * dpi; }

/* The icon reads at 0.9× the text cell: a Lucide glyph fills more of its box
 * than a text character fills its cell, so an icon sized 1:1 with `ch` sits
 * visibly larger than the label beside it. */
static f32 action_icon_sz(f32 ch) { return ch * 0.9f; }

f32 settings_action_w(i32 label_cells, f32 cw, f32 ch, f32 dpi) {
    if (label_cells < 0) label_cells = 0;
    return 2.0f * ACTION_PAD_PT * dpi
         + action_icon_sz(ch) + ACTION_ICON_GAP_PT * dpi
         + (f32)label_cells * cw;
}

SettingsActionBtn settings_action_btn(f32 x, f32 y, f32 w, i32 label_cells,
                                      f32 cw, f32 ch, f32 dpi) {
    SettingsActionBtn b = {0};
    if (label_cells < 0) label_cells = 0;
    if (w < 1.0f) w = 1.0f;

    b.x = x;
    b.y = y;
    b.w = w;
    b.h = settings_action_h(ch, dpi);
    b.radius = ACTION_RADIUS_PT * dpi;
    b.icon_sz = action_icon_sz(ch);

    f32 pad  = ACTION_PAD_PT * dpi;
    f32 gap  = ACTION_ICON_GAP_PT * dpi;
    f32 group_w = b.icon_sz + gap + (f32)label_cells * cw;

    f32 gx = x + (w - group_w) * 0.5f;
    if (gx < x + pad) gx = x + pad;           /* never crowd the left edge */

    b.icon_x  = gx;
    b.icon_y  = y + (b.h - b.icon_sz) * 0.5f;
    b.label_x = gx + b.icon_sz + gap;
    b.label_y = y + (b.h - ch) * 0.5f;

    /* Whatever survives between the label origin and the trailing pad. The
     * caller clips to this rather than trusting label_cells, so an oversized
     * label degrades to an ellipsis-free truncation instead of spilling out of
     * the box.
     *
     * The epsilon is what makes an EXACT fit fit. The box is sized from the
     * label, so `room` is label_cells * cw exactly — in real arithmetic. In
     * float the same value comes back as 11.999997, and a bare truncation then
     * clips the last character off a button that was measured to hold it
     * ("Import Theme" would draw as "Import Them"). A hundredth of a cell
     * rescues the exact fit without ever admitting a character that does not
     * fit. */
    f32 room = (x + w - pad) - b.label_x;
    b.label_max = (cw > 0.0f) ? (i32)((room + cw * 0.01f) / cw) : 0;
    if (b.label_max < 1) b.label_max = 1;
    return b;
}

bool settings_action_hit(const SettingsActionBtn *b, f32 px, f32 py) {
    if (!b) return false;
    return px >= b->x && px < b->x + b->w &&
           py >= b->y && py < b->y + b->h;
}

SettingsActionBtn settings_action_row_sidebar(f32 value_x, f32 value_room,
                                              f32 row_y, f32 cw, f32 ch, f32 dpi) {
    i32 cells = (i32)strlen(SETTINGS_LBL_CUSTOMIZE_SIDEBAR);
    f32 w = settings_action_w(cells, cw, ch, dpi);
    /* The value column is what the row has; a cramped panel shrinks the box
     * (the label clips inside it) rather than running under the scrollbar. */
    if (value_room > 0.0f && w > value_room) w = value_room;
    if (w < 1.0f) w = 1.0f;
    f32 h = settings_action_h(ch, dpi);
    return settings_action_btn(value_x, row_y - (h - ch) * 0.5f, w, cells, cw, ch, dpi);
}

SettingsActionBtn settings_action_row_font(f32 content_x, f32 content_w,
                                           f32 row_y, f32 cw, f32 ch, f32 dpi) {
    i32 cells = (i32)strlen(SETTINGS_LBL_ADD_FONT);
    f32 w = settings_action_w(cells, cw, ch, dpi);
    /* Never let one button eat the row the value text shares with it. */
    f32 cap = content_w * 0.55f;
    if (w > cap) w = cap;
    if (w < 1.0f) w = 1.0f;
    f32 h = settings_action_h(ch, dpi);
    /* `row_y` is the text origin of the Font row; centre the taller button on
     * the text cell rather than hanging it off the same top edge. */
    return settings_action_btn(content_x + content_w - w, row_y - (h - ch) * 0.5f,
                               w, cells, cw, ch, dpi);
}

void settings_action_row_theme(f32 content_x, f32 content_w, f32 row_y,
                               f32 cw, f32 ch, f32 dpi,
                               SettingsActionBtn *out_create,
                               SettingsActionBtn *out_import) {
    i32 c_cells = (i32)strlen(SETTINGS_LBL_CREATE_THEME);
    i32 i_cells = (i32)strlen(SETTINGS_LBL_IMPORT_THEME);

    /* One width for both: a primary and its alternative sitting side by side
     * read as a pair only when they are the same size. Sizing each to its own
     * label made the row look accidental the moment the two differed. */
    f32 w = settings_action_w(c_cells, cw, ch, dpi);
    f32 wi = settings_action_w(i_cells, cw, ch, dpi);
    if (wi > w) w = wi;

    f32 gap = settings_action_gap(dpi);
    if (2.0f * w + gap > content_w) w = (content_w - gap) * 0.5f;
    if (w < 1.0f) w = 1.0f;

    if (out_create)
        *out_create = settings_action_btn(content_x, row_y, w, c_cells, cw, ch, dpi);
    if (out_import)
        *out_import = settings_action_btn(content_x + w + gap, row_y, w,
                                          i_cells, cw, ch, dpi);
}

f32 settings_theme_grid_h(i32 count, i32 cols, f32 ch, f32 dpi) {
    if (cols < 1) cols = 1;
    if (count < 0) count = 0;
    i32 rows = (count + cols - 1) / cols;
    if (rows == 0) return 0.0f;
    f32 pitch = settings_theme_card_h(ch, dpi) + settings_theme_gap(dpi);
    return (f32)rows * pitch + 16.0f * dpi;
}

SettingsWeightBlock settings_weight_block(f32 y, f32 item_h, f32 dpi) {
    SettingsWeightBlock b;
    b.head_y = y;
    /* The heading is a plain label row, so the first stepper sits one row
     * below it with a little air, and the two steppers keep the same 6 pt
     * spacing Size and Line Height already have between their boxes. */
    b.term_y = y + item_h + 4.0f * dpi;
    b.ui_y   = b.term_y + item_h + 6.0f * dpi;
    b.end    = b.ui_y + item_h + 8.0f;    /* same trailing gap Line Height had */
    return b;
}

bool settings_weight_step(f32 *w, i32 dir) {
    if (!w || dir == 0) return false;
    f32 v = *w + (dir < 0 ? -TEXT_WEIGHT_STEP : TEXT_WEIGHT_STEP);
    v = roundf(v / TEXT_WEIGHT_STEP) * TEXT_WEIGHT_STEP;   /* snap to the grid */
    if (v < TEXT_WEIGHT_MIN) v = TEXT_WEIGHT_MIN;
    if (v > TEXT_WEIGHT_MAX) v = TEXT_WEIGHT_MAX;
    if (fabsf(v) < TEXT_WEIGHT_STEP * 0.5f) v = 0.0f;
    if (v == *w) return false;
    *w = v;
    return true;
}

void settings_weight_str(char *out, usize n, f32 w) {
    if (!out || n == 0) return;
    if (fabsf(w) < TEXT_WEIGHT_STEP * 0.5f) snprintf(out, n, "0.0");
    else                                    snprintf(out, n, "%+.1f", (double)w);
}

bool settings_rect_contains(SettingsRect r, f32 x, f32 y) {
    return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

SettingsShellLayout settings_shell_layout(f32 sw, f32 sh, f32 dpi,
                                          f32 scale, f32 yoff, i32 nav_count) {
    SettingsShellLayout l = {0};
    if (dpi <= 0) dpi = 1;
    f32 margin = 24.0f * dpi;
    f32 bw = fminf(920.0f * dpi, fmaxf(1.0f, sw - margin * 2));
    f32 bh = fminf(720.0f * dpi, fmaxf(1.0f, sh - margin * 2));
    if (sw < 480 * dpi) bw = fmaxf(1.0f, sw - 8 * dpi);
    if (sh < 400 * dpi) bh = fmaxf(1.0f, sh - 8 * dpi);
    l.panel_w = bw * scale; l.panel_h = bh * scale;
    l.px = fmaxf(0, (sw - l.panel_w) * 0.5f);
    l.py = fmaxf(0, fminf(sh - l.panel_h, (sh - l.panel_h) * 0.5f - yoff));
    f32 d = dpi * scale;
    l.title_h = fminf(52.0f * d, l.panel_h * 0.22f);
    l.nav_icon_only = bw < 620 * dpi;
    l.nav_w = fminf(l.nav_icon_only ? 60 * d : 200 * d, l.panel_w * 0.40f);
    f32 pad = bw < 620 * dpi ? 18 * d : 32 * d;
    pad = fminf(pad, fmaxf(0, (l.panel_w - l.nav_w - d) * 0.25f));
    l.content_x = l.px + l.nav_w + pad;
    l.content_w = fmaxf(1, l.panel_w - l.nav_w - 2 * pad);
    l.content_top = l.py + l.title_h + 26 * d;
    l.clip_top = l.py + l.title_h;
    l.clip_bottom = l.py + l.panel_h - 18 * d;
    l.nav_top = l.py + l.title_h + 18 * d;
    l.nav_bottom = fmaxf(l.nav_top + d, l.py + l.panel_h - 78 * d);
    l.nav_row_h = (bh < 520 * dpi ? 30 : 36) * d;
    l.nav_group_h = !l.nav_icon_only && bh >= 520 * dpi ? 24 * d : 0;
    l.nav_group_gap = l.nav_group_h > 0 ? 14 * d : 6 * d;
    l.nav_content_h = (f32)nav_count * l.nav_row_h + 3 * l.nav_group_h + 2 * l.nav_group_gap;
    l.close = (SettingsRect){l.px + l.panel_w - 44 * d, l.py + (l.title_h - 28 * d) * 0.5f, 28 * d, 28 * d};
    l.reset = (SettingsRect){l.px + 12 * d, l.py + l.panel_h - 48 * d, fmaxf(1, l.nav_w - 24 * d), 30 * d};
    return l;
}

SettingsRect settings_nav_row(SettingsShellLayout l, i32 index, f32 scroll, f32 dpi) {
    i32 groups = (index >= 3) + (index >= 6);
    f32 y = l.nav_top + (f32)index * l.nav_row_h +
        (f32)(groups + 1) * l.nav_group_h + (f32)groups * l.nav_group_gap - scroll;
    f32 gutter = fminf(10 * dpi, l.nav_w * 0.15f);
    return (SettingsRect){l.px + gutter, y, fmaxf(1, l.nav_w - 2 * gutter), l.nav_row_h};
}

f32 settings_nav_group_y(SettingsShellLayout l, i32 first, f32 scroll, f32 dpi) {
    return settings_nav_row(l, first, scroll, dpi).y - l.nav_group_h;
}

SettingsBrowserLayout settings_browser_layout(f32 x, f32 y, f32 w, f32 dpi) {
    SettingsBrowserLayout l = {0};
    l.x = x; l.w = fmaxf(1, w); l.dpi = dpi;
    f32 pad = fminf(16 * dpi, w * 0.08f);
    l.access = (SettingsRect){x, y, w, 82 * dpi};
    /* The switch's own rect, at the panel's 8 pt cell (settings_switch). */
    f32 sw_w = settings_switch_w(8 * dpi, dpi);
    l.toggle = (SettingsRect){x + w - pad - sw_w, y + 20 * dpi, sw_w, 22 * dpi};
    l.local_y = y + 102 * dpi;
    l.heading_y = y + 154 * dpi;
    l.hint_y = l.heading_y + 28 * dpi;
    f32 input_y = l.hint_y + 46 * dpi;
    bool stack = w < 320 * dpi;
    f32 button_w = fminf(104 * dpi, w);
    l.input = (SettingsRect){x, input_y, stack ? w : fmaxf(1, w - button_w - 10 * dpi), 36 * dpi};
    l.submit = (SettingsRect){x + w - button_w, input_y + (stack ? 44 * dpi : 0), button_w, 36 * dpi};
    l.status_y = l.submit.y + l.submit.h + 12 * dpi;
    l.list_y = l.status_y + 34 * dpi;
    l.row_h = 58 * dpi;
    return l;
}

SettingsBrowserRow settings_browser_row(SettingsBrowserLayout l, i32 index) {
    SettingsBrowserRow r = {0};
    f32 pad = fminf(16 * l.dpi, l.w * 0.08f);
    r.row = (SettingsRect){l.x, l.list_y + (f32)index * l.row_h, l.w, l.row_h};
    f32 button = fminf(28 * l.dpi, l.w * 0.16f);
    r.remove = (SettingsRect){l.x + l.w - pad - button, r.row.y + (l.row_h - button) * 0.5f, button, button};
    r.edit = r.remove; r.edit.x -= button + 4 * l.dpi;
    r.text_x = l.x + pad;
    r.title_y = r.row.y + 10 * l.dpi;
    r.detail_y = r.title_y + 22 * l.dpi;
    r.text_w = fmaxf(1, r.edit.x - r.text_x - 10 * l.dpi);
    return r;
}

SettingsFontHeader settings_font_header(f32 x, f32 y, f32 w, f32 cw, f32 ch, f32 dpi) {
    SettingsFontHeader g = {0};
    bool stack = w < 440 * dpi;
    g.add = settings_action_row_font(x, w, y + (stack ? ch + 18 * dpi : 0), cw, ch, dpi);
    g.value_x = x + (stack ? 6 : 14) * cw;
    g.value_w = fmaxf(1, (stack ? x + w : g.add.x - 10 * dpi) - g.value_x);
    g.grid_y = stack ? g.add.y + g.add.h + 24 * dpi : y + ch + 34 * dpi;
    return g;
}

#define SWITCH_TRACK_W_PT  32.0f
#define SWITCH_TRACK_H_PT  18.0f
#define SWITCH_KNOB_PT     12.0f
#define SWITCH_KNOB_PAD_PT  3.0f

f32 settings_switch_w(f32 cw, f32 dpi) {
    /* Room for the longer word, the gap, the track. */
    usize cells = strlen(SETTINGS_LBL_OFF) > strlen(SETTINGS_LBL_ON)
                ? strlen(SETTINGS_LBL_OFF) : strlen(SETTINGS_LBL_ON);
    return (f32)cells * cw + (SETTINGS_SWITCH_GAP_PT + SWITCH_TRACK_W_PT) * dpi;
}

SettingsSwitch settings_switch(f32 x, f32 y, bool on, f32 cw, f32 ch, f32 dpi) {
    SettingsSwitch s = {0};
    s.x = x; s.y = y;
    s.w = settings_switch_w(cw, dpi);
    s.h = ch + 6.0f * dpi;
    s.track_w = SWITCH_TRACK_W_PT * dpi;
    s.track_h = SWITCH_TRACK_H_PT * dpi;
    s.track_x = x + s.w - s.track_w;
    s.track_y = y + (s.h - s.track_h) * 0.5f;
    s.knob_sz = SWITCH_KNOB_PT * dpi;
    s.knob_x = on ? s.track_x + s.track_w - s.knob_sz - SWITCH_KNOB_PAD_PT * dpi
                  : s.track_x + SWITCH_KNOB_PAD_PT * dpi;
    s.knob_y = s.track_y + SWITCH_KNOB_PAD_PT * dpi;
    s.word = on ? SETTINGS_LBL_ON : SETTINGS_LBL_OFF;
    s.word_x = s.track_x - SETTINGS_SWITCH_GAP_PT * dpi - (f32)strlen(s.word) * cw;
    s.word_y = y + (s.h - ch) * 0.5f;
    return s;
}

SettingsStepper settings_stepper(f32 x, f32 y, f32 w, f32 ch, f32 dpi) {
    SettingsStepper g = {0};
    w = fmaxf(1, fminf(w, 192 * dpi));
    f32 bw = fminf(24 * dpi, w * 0.25f);
    f32 gap = fminf(6 * dpi, w * 0.06f);
    g.plus = (SettingsRect){x + w - bw, y - 4 * dpi, bw, ch + 8 * dpi};
    g.minus = g.plus; g.minus.x -= bw + gap;
    g.value_x = x; g.value_w = fmaxf(1, g.minus.x - x - gap);
    return g;
}

bool settings_scrollback_step(i32 *lines, i32 direction) {
    static const i32 choices[] = {0, 1000, 2000, 5000, 10000, CONFIG_SCROLLBACK_MAX_LINES};
    if (!lines || !direction) return false;
    i32 next = *lines;
    if (direction > 0) {
        for (usize i = 0; i < sizeof choices / sizeof *choices; i++)
            if (choices[i] > *lines) { next = choices[i]; break; }
    } else {
        for (usize i = sizeof choices / sizeof *choices; i > 0; i--)
            if (choices[i - 1] < *lines) { next = choices[i - 1]; break; }
    }
    if (next == *lines) return false;
    *lines = next;
    return true;
}

f32 settings_section_h(f32 ch, f32 dpi) { return ch + 24 * dpi; }

/* ---- the Model menu -------------------------------------------------------
 * Its own numbers are the ones a context menu has no use for; everything a
 * row is made of comes from CTX_MENU_*. */
#define MODEL_MENU_GAP_PT      6.0f   /* chip -> popover                          */
#define MODEL_MENU_EDGE_PT     6.0f   /* popover -> the body's clip, at least     */
#define MODEL_MENU_HEAD_H_PT  34.0f
#define MODEL_MENU_TRACK_W_PT  3.0f
#define MODEL_MENU_TRACK_X_PT  5.0f   /* the track's right inset                  */
#define MODEL_MENU_TRACK_GAP_PT 4.0f  /* highlight -> track                       */

/* The card's height with a window of `vis` rows. */
static f32 model_menu_h(bool pinned, i32 rows, i32 vis, f32 dpi) {
    f32 row_h = CTX_MENU_ROW_H_PT * dpi;
    f32 h = MODEL_MENU_HEAD_H_PT * dpi + CTX_MENU_VPAD_PT * dpi * 2.0f;
    if (pinned) h += row_h;
    if (pinned && rows > 0) h += CTX_MENU_SEP_H_PT * dpi;
    if (rows > 0) h += (f32)vis * row_h;
    else if (!pinned) h += row_h;          /* the "nothing matches" line */
    return h;
}

i32 settings_model_menu_clamp_scroll(i32 scroll, i32 rows, i32 visible) {
    i32 max = rows - visible;
    if (max < 0) max = 0;
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
    return scroll;
}

i32 settings_model_menu_reveal(i32 scroll, i32 row, i32 rows, i32 visible) {
    if (visible < 1) visible = 1;
    if (row < scroll) scroll = row;
    if (row >= scroll + visible) scroll = row - visible + 1;
    return settings_model_menu_clamp_scroll(scroll, rows, visible);
}

SettingsModelMenu settings_model_menu(SettingsRect chip, f32 band_top, f32 band_bottom,
                                      f32 content_x, f32 content_w,
                                      bool pinned, i32 rows, f32 dpi) {
    SettingsModelMenu m = {0};
    if (rows < 0) rows = 0;
    m.row_h = CTX_MENU_ROW_H_PT * dpi;
    m.dpi = dpi;
    m.sep_y = -1.0f;

    f32 gap  = MODEL_MENU_GAP_PT * dpi;
    f32 edge = MODEL_MENU_EDGE_PT * dpi;
    f32 below = (band_bottom - edge) - (chip.y + chip.h + gap);
    f32 above = (chip.y - gap) - (band_top + edge);

    /* The whole window under the chip when it fits, the whole window above it
     * when only that fits; otherwise the roomier side, with the window
     * squeezed to what it holds. A menu that cannot show even the least of
     * its rows still opens (clipped by the body) rather than not at all. */
    i32 want = rows > SETTINGS_MODEL_MENU_ROWS ? SETTINGS_MODEL_MENU_ROWS : rows;
    i32 vis = want;
    if (model_menu_h(pinned, rows, want, dpi) <= below) {
        m.above = false;
    } else if (model_menu_h(pinned, rows, want, dpi) <= above) {
        m.above = true;
    } else {
        m.above = above > below;
        f32 room = m.above ? above : below;
        i32 least = rows < SETTINGS_MODEL_MENU_MIN_ROWS ? rows : SETTINGS_MODEL_MENU_MIN_ROWS;
        while (vis > least && model_menu_h(pinned, rows, vis, dpi) > room) vis--;
    }
    m.visible = vis;

    f32 w = chip.w;
    if (w < CTX_MENU_MIN_W_PT * dpi) w = CTX_MENU_MIN_W_PT * dpi;
    if (content_w > 0.0f && w > content_w) w = content_w;
    f32 x = chip.x;
    if (x + w > content_x + content_w) x = content_x + content_w - w;
    if (x < content_x) x = content_x;
    f32 h = model_menu_h(pinned, rows, vis, dpi);
    f32 y = m.above ? chip.y - gap - h : chip.y + chip.h + gap;
    m.card = (SettingsRect){x, y, w, h};

    m.head = (SettingsRect){x, y, w, MODEL_MENU_HEAD_H_PT * dpi};
    m.rule_y = y + m.head.h;
    f32 cy = m.rule_y + CTX_MENU_VPAD_PT * dpi;
    if (pinned) {
        m.pinned = (SettingsRect){x, cy, w, m.row_h};
        cy += m.row_h;
        if (rows > 0) {
            m.sep_y = cy + floorf(CTX_MENU_SEP_H_PT * dpi * 0.5f);
            cy += CTX_MENU_SEP_H_PT * dpi;
        }
    }
    if (rows > 0)      m.list = (SettingsRect){x, cy, w, (f32)vis * m.row_h};
    else if (!pinned)  m.list = (SettingsRect){x, cy, w, m.row_h};
    else               m.list = (SettingsRect){x, cy, w, 0.0f};

    if (rows > vis && vis > 0) {
        f32 tw = MODEL_MENU_TRACK_W_PT * dpi;
        m.track = (SettingsRect){x + w - MODEL_MENU_TRACK_X_PT * dpi - tw,
                                 m.list.y + 2.0f * dpi, tw, m.list.h - 4.0f * dpi};
    }
    return m;
}

SettingsRect settings_model_menu_row(const SettingsModelMenu *m, i32 vi) {
    return (SettingsRect){m->list.x, m->list.y + (f32)vi * m->row_h, m->list.w, m->row_h};
}

SettingsRect settings_model_menu_highlight(const SettingsModelMenu *m, SettingsRect row) {
    f32 inset = CTX_MENU_HL_INSET_PT * m->dpi;
    SettingsRect h = {row.x + inset, row.y, row.w - inset * 2.0f, row.h};
    if (m->track.w > 0.0f) {
        /* Stop short of the scrollbar: a bar under the highlight reads as part
         * of the row. */
        f32 right = m->track.x - MODEL_MENU_TRACK_GAP_PT * m->dpi;
        if (h.x + h.w > right) h.w = right - h.x;
    }
    if (h.w < 1.0f) h.w = 1.0f;
    return h;
}

i32 settings_model_menu_hit(const SettingsModelMenu *m, f32 px, f32 py) {
    if (!settings_rect_contains(m->card, px, py)) return SETTINGS_MODEL_HIT_NONE;
    if (settings_rect_contains(m->head, px, py)) return SETTINGS_MODEL_HIT_HEAD;
    if (m->pinned.w > 0.0f && settings_rect_contains(m->pinned, px, py))
        return SETTINGS_MODEL_HIT_PINNED;
    if (m->visible > 0 && settings_rect_contains(m->list, px, py)) {
        /* The scrollbar sits inside the window, where the highlight stops: a
         * click on it scrolled nothing and picked the row under the thumb. */
        if (m->track.w > 0.0f && px >= m->track.x - MODEL_MENU_TRACK_GAP_PT * m->dpi)
            return SETTINGS_MODEL_HIT_TRACK;
        i32 vi = (i32)((py - m->list.y) / m->row_h);
        if (vi >= 0 && vi < m->visible) return vi;
    }
    return SETTINGS_MODEL_HIT_CHROME;
}
