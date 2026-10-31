/*
 * Liu — tab popup (see tab_popup.h for what it is).
 *
 * The preview answers one question: WHO is in this tab and WHAT are they
 * doing. It lists the tab's panes as agent rows — icon, agent, the chat's
 * topic, and a status pill that says "working 1m 12s" or "idle 40s" — rather
 * than showing a shrunken picture of the terminals, which at thumbnail size
 * is texture, not information.
 *
 * Cost model: nothing runs until the panel is actually painted (a pointer
 * crossing the strip pays two comparisons per frame in tab_popup_hover), and
 * the paint itself is a handful of rects plus a few short strings per pane —
 * it reads Tab.pane_agents, which the session poll already maintains, and
 * never touches a terminal grid or probes a process.
 */
#include "ui/ui.h"
#include "ui/tab_popup.h"
#include "ui/layout.h"
#include "ui/anim.h"
#include "ui/agent_icons.h"
#include "ui/icon.h"
#include "ui/chrome_palette.h"
#include "ui/notification.h"
#include "history/event.h"
#include "notify/config.h"
#include "notify/protocol.h"
#include "notify/platform_notify.h"
#include "platform/platform.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* ui.c owns this (static there); declared here the way browser_ui.c does. */
extern void draw_text_ex(Renderer *r, const char *text, f32 x, f32 y,
                         Color fg, i32 max_chars, f32 step);

/* ---------------------------------------------------------------- tuning */

/* Pointer rest time before the preview opens. Long enough that sweeping the
 * strip to reach the (+) button never flashes panels, short enough that a
 * deliberate "what's in there?" hover feels answered rather than delayed. */
#define POPUP_DWELL_S        0.55
#define POPUP_OPEN_S         0.22
#define POPUP_CLOSE_S        0.15
/* How long a done notice stays up on its own. */
#define POPUP_DONE_HOLD_S    7.0

#define POPUP_W_PT         380.0f

#define POPUP_PAD_PT        12.0f
#define POPUP_GAP_PT         6.0f    /* gap under the tab strip */
#define POPUP_RADIUS_PT     10.0f
#define POPUP_HEADER_PT     26.0f
#define POPUP_ROW_H_PT      42.0f
#define POPUP_ROW_GAP_PT     4.0f
/* Rows past this get folded into a "+N more" line — a tab with eight panes
 * would otherwise grow a panel taller than the window. */
#define POPUP_MAX_ROWS         6

typedef enum {
    POPUP_NONE = 0,
    POPUP_PREVIEW,
    POPUP_DONE,
} PopupKind;

/* The panel's state lives on AppState (TabPopup, see ui.h) — one per window.
 * Every window drives it from its own render pass, so a shared struct would
 * break: the unfocused window's tab_popup_hover(app, -1, …) would clear the
 * dwell every frame, a DONE notice raised in one window would be painted by
 * another against its tab of the same index, and the glide target would flip
 * between two windows' pixel geometries.
 *
 * A calloc'd AppState starts with dwell_tab = 0 rather than -1, which reads as
 * "the pointer is already resting on tab 0". Harmless: dwell_since is 0 too,
 * and tab_popup_hover restarts the timer whenever it finds dwell_since <= 0. */
#define POP(app)  (&(app)->tab_popup)

/* ------------------------------------------------------------ small utils */

static f32 clampf(f32 v, f32 lo, f32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Cells a UI string occupies (one per codepoint) — what the fixed-cell text
 * path actually advances by, so widths measured with this line up exactly. */
static i32 cell_count(const char *s) {
    i32 n = 0;
    for (const u8 *p = (const u8 *)s; *p; p++) if ((*p & 0xC0) != 0x80) n++;
    return n;
}

/* Truncate to `max_chars` printable cells, appending an ellipsis when cut.
 * Operates on whole UTF-8 sequences so a cut never splits a codepoint. */
static void fit_text(const char *src, char *out, usize cap, i32 max_chars) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!src || !*src || max_chars <= 0) return;

    const u8 *p = (const u8 *)src;
    usize w = 0;
    i32 chars = 0;
    while (*p && chars < max_chars && w + 5 < cap) {
        usize seq = 1;
        if ((*p & 0xE0) == 0xC0) seq = 2;
        else if ((*p & 0xF0) == 0xE0) seq = 3;
        else if ((*p & 0xF8) == 0xF0) seq = 4;
        for (usize k = 0; k < seq && p[k]; k++) out[w++] = (char)p[k];
        p += seq;
        chars++;
    }
    out[w] = '\0';
    if (*p && chars >= max_chars && w + 4 < cap) {
        /* Overwrite the last cell with "…" so the width stays put. */
        usize back = w;
        while (back > 0 && ((u8)out[back - 1] & 0xC0) == 0x80) back--;
        if (back > 0) back--;
        memcpy(out + back, "\xE2\x80\xA6", 3);
        out[back + 3] = '\0';
    }
}

/* "1m 12s" / "42s" / "2h 5m" — durations the eye can read at a glance. */
static void fmt_duration(f64 secs, char *out, usize cap) {
    if (secs < 0.0) secs = 0.0;
    i32 s = (i32)(secs + 0.5);
    if (s < 60)        snprintf(out, cap, "%ds", s);
    else if (s < 3600) snprintf(out, cap, "%dm %ds", s / 60, s % 60);
    else               snprintf(out, cap, "%dh %dm", s / 3600, (s % 3600) / 60);
}

static const Theme *popup_theme(AppState *app) {
    const Theme *t = app ? app->config.theme : NULL;
    return t ? t : &THEME_Liu;
}

/* Everything the paint derives from the theme, resolved ONCE per frame.
 *
 * chrome_palette_for() runs seven relative-luminance conversions (three powf
 * each) — cheap once, wasteful when every row, pill and tile re-derives it.
 */
typedef struct {
    const Theme  *theme;
    ChromePalette cp;
    Color acc;
    f32   dpi;
    f32   ui_cw, ui_ch;   /* body text cell */
    f32   sm_cw, sm_ch;   /* small text cell (pills, tallies) */
} PopupStyle;

static PopupStyle popup_style(AppState *app) {
    PopupStyle s;
    s.theme = popup_theme(app);
    s.cp    = chrome_palette_for(s.theme);
    s.acc   = theme_ui_accent(s.theme);
    s.dpi   = app->dpi_scale;
    s.ui_cw = 8.0f * s.dpi;  s.ui_ch = 16.0f * s.dpi;
    s.sm_cw = 7.0f * s.dpi;  s.sm_ch = 14.0f * s.dpi;
    return s;
}

/* --------------------------------------------------------------- lifetime */

static void popup_open(AppState *app, PopupKind kind, i32 tab) {
    TabPopup *g = POP(app);
    bool fresh = (g->kind != (u8)kind) || (g->tab != tab) || !g->want_open;
    g->kind = (u8)kind;
    g->tab  = tab;
    g->want_open = true;
    if (fresh) {
        /* A kind switch changes the panel's height, so replay the reveal
         * rather than letting the new layout snap into the old box. */
        anim_start(&g->open_anim, POPUP_OPEN_S);
        anim_reset(&g->close_anim);
    }
}

void tab_popup_dismiss(AppState *app) {
    if (!app) return;
    TabPopup *g = POP(app);
    g->dwell_tab = -1;
    g->dwell_since = 0.0;
    if (!g->want_open) return;
    g->want_open = false;
    anim_start(&g->close_anim, POPUP_CLOSE_S);
    anim_reset(&g->open_anim);
}

bool tab_popup_visible(AppState *app) {
    if (!app) return false;
    const TabPopup *g = &app->tab_popup;
    return g->want_open || !anim_done(&g->close_anim);
}

/* The preview only has something to say about a tab that is running an agent.
 * On a plain shell tab it would be a panel announcing "no agents" — noise over
 * the terminal every time the pointer crosses the strip. Gated on an agent
 * being PRESENT, not on it being busy, so the list stays reachable while the
 * agent sits idle at its prompt (which is when "idle 40s" is worth reading). */
static bool tab_has_agent(const AppState *app, i32 idx) {
    if (!app || idx < 0 || idx >= app->tab_count) return false;
    const Tab *tab = &app->tabs[idx];
    return tab->kind == TAB_TERMINAL && tab->agent_count > 0;
}

static void tab_popup_hover_ex(AppState *app, i32 tab_index, f32 pill_x, f32 pill_w,
                               bool side, f32 side_x, f32 side_y);

void tab_popup_hover(AppState *app, i32 tab_index, f32 pill_x, f32 pill_w) {
    tab_popup_hover_ex(app, tab_index, pill_x, pill_w, false, 0.0f, 0.0f);
}

void tab_popup_hover_side(AppState *app, i32 tab_index, f32 sidebar_right, f32 row_y) {
    tab_popup_hover_ex(app, tab_index, 0.0f, 0.0f, true, sidebar_right, row_y);
}

static void tab_popup_hover_ex(AppState *app, i32 tab_index, f32 pill_x, f32 pill_w,
                               bool side, f32 side_x, f32 side_y) {
    if (!app) return;
    TabPopup *g = POP(app);
    f64 now = platform_time_sec();

    bool valid = tab_index >= 0 && tab_index < app->tab_count &&
                 (side || pill_w > 0.0f) && tab_has_agent(app, tab_index);

    if (!valid) {
        if (g->dwell_tab != -1) { g->dwell_tab = -1; g->dwell_since = 0.0; }
        if (g->kind == POPUP_PREVIEW && g->want_open) tab_popup_dismiss(app);
        return;
    }

    if (g->dwell_tab != tab_index) {
        g->dwell_tab = tab_index;
        g->dwell_since = now;
        /* Sliding onto a neighbour while a preview is already up should feel
         * continuous — glide, don't re-dwell. */
        if (g->kind == POPUP_PREVIEW && g->want_open) g->dwell_since = 0.0;
    }

    g->anchor_cx = pill_x + pill_w * 0.5f;
    if (g->anchor_side != side) g->have_draw_y = false;
    g->anchor_side = side;
    g->anchor_x = side_x;
    g->anchor_y = side_y;

    if (g->kind == POPUP_PREVIEW && g->want_open) {
        g->tab = tab_index;                   /* follow the pointer */
        return;
    }
    if (g->dwell_since <= 0.0) g->dwell_since = now;
    if (now - g->dwell_since >= POPUP_DWELL_S) {
        /* A done notice is superseded: the pointer resting on a tab is the
         * user asking a question, and the preview answers it. */
        popup_open(app, POPUP_PREVIEW, tab_index);
    } else {
        /* Keep the loop ticking so the dwell can elapse with the pointer at
         * rest. Soft rate: nothing is animating yet, the loop only needs to
         * wake once the timer is up. */
        anim_register_soft_until(g->dwell_since + POPUP_DWELL_S + 0.05);
    }
}

/* ------------------------------------------------------- completion sound */

/* ChatTool → NotifyTool, so a user who assigned a per-tool sound in Settings
 * hears the same one here. Everything the notify protocol has no id for maps
 * to TOOL_CUSTOM, which the (any, event) rules still cover. */
static u8 notify_tool_for_chat_tool(i32 tool) {
    switch ((ChatTool)tool) {
    case CHAT_TOOL_CLAUDE:  return TOOL_CLAUDE;
    case CHAT_TOOL_CODEX:   return TOOL_CODEX;
    case CHAT_TOOL_COPILOT: return TOOL_COPILOT;
    default:                return TOOL_CUSTOM;
    }
}

/* The app's own cue for an agent event, played from the main thread.
 *
 * It exists for the case the notify server deliberately sits out: Liu is the
 * active app, so the server plays nothing ("the user is already watching").
 * With another app in front the server's sound and banner ARE the cue, and
 * playing here as well would be a second copy of the same sound a moment
 * later, from a second thread, into the same player. So this one is the
 * server's complement, never its companion.
 *
 * It follows the server's configuration exactly, through the one loader both
 * use (notify_config_load_effective), so a missing `enabled` line, a cleared
 * rule and a cleared completion sound mean the same here as in the server.
 */
void tab_popup_play_event_sound(i32 tool, u8 event) {
    static NotifyConfig cfg;
    static f64 cfg_loaded_at = 0.0;
    static f64 last_played   = 0.0;
    if ((usize)event >= sizeof cfg.event_enabled / sizeof cfg.event_enabled[0]) return;
    if (!notify_target_active()) return;
    f64 now = platform_time_sec();

    /* Re-read at most every few seconds and only when an event actually
     * fires, so Settings edits land without a per-frame stat(). */
    if (cfg_loaded_at == 0.0 || now - cfg_loaded_at > 5.0) {
        notify_config_load_effective(&cfg);
        cfg_loaded_at = now;
    }
    /* Honour the same switches the daemon does — a user who muted completion
     * notifications does not want the app itself chiming instead. */
    if (!cfg.enabled) return;
    if (!cfg.event_enabled[event]) return;
    /* Two panes finishing together are one event as far as the ear cares. */
    if (now - last_played < 1.5) return;

    u8 tool_id = notify_tool_for_chat_tool(tool);
    const char *path = notify_config_sound_for(&cfg, tool_id, event);
    if (path && *path && notify_play_sound(path)) last_played = now;
}

void tab_popup_agent_done(AppState *app, i32 tab_index, i32 pane, i32 tool,
                          const char *chat_title, f64 work_sec) {
    if (!app || tab_index < 0 || tab_index >= app->tab_count || !tool) return;
    TabPopup *g = POP(app);

    /* A preview the user is actively reading is not interrupted; the sound
     * still plays and the pane's row flips to "idle" in place, so nothing is
     * lost by not stealing the panel. */
    if (g->kind == POPUP_PREVIEW && g->want_open) {
        tab_popup_play_event_sound(tool, EVT_COMPLETE);
        return;
    }

    g->done_tool = tool;
    g->done_pane = pane;
    g->done_secs = work_sec > 0.0 ? work_sec : 0.0;
    g->done_until = platform_time_sec() + POPUP_DONE_HOLD_S;
    snprintf(g->done_title, sizeof g->done_title, "%s",
             (chat_title && *chat_title) ? chat_title : "");

    f32 pw = (tab_index < MAX_TABS) ? app->_tab_pill_w[tab_index] : 0.0f;
    /* Docked tabs: the notice opens beside the row, like the preview. */
    bool side = app->_tab_pills_in_sidebar;
    if (g->anchor_side != side) g->have_draw_y = false;
    g->anchor_side = side;
    if (pw > 0.0f) {
        g->anchor_cx = app->_tab_pill_x[tab_index] + pw * 0.5f;
        g->anchor_x  = app->sidebar_width;
        g->anchor_y  = app->_tab_pill_y[tab_index];
    } else {
        /* Tab isn't painted (collapsed group / scrolled out): drop the panel
         * at the strip's left edge rather than pointing at nothing. */
        g->anchor_cx = app->_tab_view_x + POPUP_W_PT * app->dpi_scale * 0.5f;
        g->anchor_x  = app->sidebar_width;
        g->anchor_y  = app->tab_bar_height + 8.0f * app->dpi_scale;
        g->have_draw_cx = false;
    }
    popup_open(app, POPUP_DONE, tab_index);
    tab_popup_play_event_sound(tool, EVT_COMPLETE);
}

/* ------------------------------------------------------------- agent rows */

/* Icon tile: the agent's logo on its theme-aware backdrop, or a letter badge
 * when no PNG shipped for that tool. `tool == 0` draws the plain-shell tile. */
static void draw_agent_tile(AppState *app, const PopupStyle *st, i32 tool,
                            f32 x, f32 y, f32 sz, f32 alpha) {
    Renderer *r = &app->renderer;
    const Theme *t = st->theme;
    f32 dpi = st->dpi;

    if (!tool) {
        /* No agent in this pane — a quiet neutral tile, no borrowed branding,
         * carrying the same `>_` mark the tab pill leads with so the popup
         * and the strip name the plain shell the same way. */
        Color fill = st->cp.surface_sunken; fill.a = 0.9f * alpha;
        renderer_draw_rrect_simple(r, x, y, sz, sz, fill, 6.0f * dpi);
        renderer_flush_rrects(r);
        Color dim = t->fg; dim.a *= 0.6f * alpha;
        f32 pad = roundf(sz * 0.2f);
        icon_draw(r, ICON_TERMINAL, x + pad, y + pad, sz - 2.0f * pad, dim);
        return;
    }

    Color tile = agent_icon_backdrop((ChatTool)tool, t);
    tile.a *= alpha;
    renderer_draw_rrect_simple(r, x, y, sz, sz, tile, 6.0f * dpi);
    renderer_flush_rrects(r);

    f32 pad = roundf(sz * 0.16f);
    f32 isz = sz - 2.0f * pad;
    i32 iw = 0, ih = 0;
    const u8 *px = agent_icon_rgba_scaled_on((ChatTool)tool,
                                             (i32)(isz + 0.5f), tile, &iw, &ih);
    if (px) {
        /* The tinted path, so the logo fades with the tile under it. */
        renderer_draw_image_tinted(r, px, 0, px, iw, ih, x + pad, y + pad, isz, isz,
                                   (Color){1, 1, 1, alpha});
    } else {
        AgentTint tint = agent_icon_tint((ChatTool)tool);
        /* Same inset the icon above uses, so the two are the same size. */
        agent_icon_draw_letter(r, (ChatTool)tool, x + pad, y + pad, isz, isz,
                               (Color){tint.r, tint.g, tint.b, alpha});
    }
}

/* Right-aligned status pill. Working pills take the UI accent and carry a
 * breathing dot; idle pills stay neutral so a glance separates the two
 * without reading a word. Returns the pill's left edge so the caller knows
 * where the text beside it has to stop. */
static f32 draw_status_pill(AppState *app, const PopupStyle *st,
                            const char *label, bool working,
                            f32 right_x, f32 cy, f32 alpha) {
    Renderer *r = &app->renderer;
    const Theme *t = st->theme;
    f32 dpi = st->dpi;
    f32 cw = st->sm_cw, ch = st->sm_ch;

    i32 n = cell_count(label);

    f32 dot = working ? fmaxf(3.0f, 4.0f * dpi) : 0.0f;
    f32 dot_gap = working ? 5.0f * dpi : 0.0f;
    f32 pad_x = 8.0f * dpi;
    f32 pill_w = pad_x * 2.0f + dot + dot_gap + (f32)n * cw;
    f32 pill_h = ch + 7.0f * dpi;
    f32 px = right_x - pill_w;
    f32 py = cy - pill_h * 0.5f;

    Color acc = st->acc;
    Color fill = working ? (Color){acc.r, acc.g, acc.b, 0.20f * alpha}
                         : (Color){st->cp.surface_sunken.r, st->cp.surface_sunken.g,
                                   st->cp.surface_sunken.b, 0.85f * alpha};
    renderer_draw_rrect_simple(r, px, py, pill_w, pill_h, fill, pill_h * 0.5f);
    renderer_flush_rrects(r);

    f32 tx = px + pad_x;
    if (working) {
        /* Breathing dot — the same "something is happening" language the tab's
         * accent bar uses, at a rate the soft (30 Hz) frame budget carries. */
        f32 pulse = 0.55f + 0.45f * (f32)sin(platform_time_sec() * 3.4);
        renderer_draw_rrect_simple(r, tx, cy - dot * 0.5f, dot, dot,
                                   (Color){acc.r, acc.g, acc.b, alpha * pulse},
                                   dot * 0.5f);
        renderer_flush_rrects(r);
        tx += dot + dot_gap;
    }

    Color fg = working ? (Color){acc.r, acc.g, acc.b, alpha}
                       : (Color){t->fg.r, t->fg.g, t->fg.b, 0.62f * alpha};
    renderer_set_ui_scale(r, cw, ch);
    draw_text_ex(r, label, tx, cy - ch * 0.5f, fg, n, cw);
    renderer_flush_glyphs(r);
    return px;
}

/* One pane = one row: tile, agent name (+ pane number when the tab is split),
 * the chat topic underneath, and the status pill on the right. */
static void draw_agent_row(AppState *app, const PopupStyle *st, const Tab *tab,
                           i32 pane, i32 pane_count, f32 x, f32 y, f32 w, f32 h,
                           f64 now, f32 alpha) {
    Renderer *r = &app->renderer;
    const Theme *t = st->theme;
    f32 dpi = st->dpi;
    f32 ui_cw = st->ui_cw, ui_ch = st->ui_ch;

    const PaneAgent *pa = &tab->pane_agents[pane];
    bool working = pa->tool && now < pa->work_until;
    bool focused = (tab->active_pane == pane);

    /* Row plate. The focused pane gets an accent hairline so the preview also
     * answers "which one am I typing into". */
    Color acc = st->acc;
    Color plate = st->cp.surface_neutral; plate.a = 0.9f * alpha;
    Color edge = focused ? (Color){acc.r, acc.g, acc.b, 0.55f * alpha}
                         : (Color){st->cp.divider_subtle.r, st->cp.divider_subtle.g,
                                   st->cp.divider_subtle.b, 0.7f * alpha};
    renderer_draw_rrect_bordered(r, x, y, w, h, plate, edge, fmaxf(1.0f, dpi),
                                 7.0f * dpi, 7.0f * dpi, 7.0f * dpi, 7.0f * dpi,
                                 0.0f, 0.0f, 0.0f, 0.0f);
    renderer_flush_rrects(r);

    f32 tile = fminf(24.0f * dpi, h - 10.0f * dpi);
    f32 tx = x + 9.0f * dpi;
    draw_agent_tile(app, st, pa->tool, tx, y + (h - tile) * 0.5f, tile, alpha);

    /* Status pill first: it owns the right edge, and the text has to stop
     * before it rather than run underneath. */
    char status[32], dur[24];
    if (working) {
        fmt_duration(now - pa->work_since, dur, sizeof dur);
        snprintf(status, sizeof status, "working %s", dur);
    } else if (pa->tool && pa->work_until > 0.0) {
        /* work_until sits WORK_HOLD past the last burst — subtract it back out
         * so this reports when the agent actually fell quiet. */
        fmt_duration(now - (pa->work_until - TAB_ACCENT_WORK_HOLD),
                     dur, sizeof dur);
        snprintf(status, sizeof status, "idle %s", dur);
    } else if (pa->tool) {
        snprintf(status, sizeof status, "idle");
    } else {
        snprintf(status, sizeof status, "no agent");
    }
    f32 pill_left = draw_status_pill(app, st, status, working,
                                     x + w - 9.0f * dpi, y + h * 0.5f, alpha);

    renderer_set_ui_scale(r, ui_cw, ui_ch);
    f32 text_x = tx + tile + 10.0f * dpi;
    f32 text_w = pill_left - 8.0f * dpi - text_x;
    i32 maxc = (i32)(text_w / ui_cw);
    if (maxc < 3) maxc = 3;

    char line[192], fitted[224];
    const char *name = pa->tool ? app_agent_display_name(pa->tool) : "Shell";
    if (pane_count > 1) snprintf(line, sizeof line, "%d · %s", pane + 1, name);
    else                snprintf(line, sizeof line, "%s", name);
    fit_text(line, fitted, sizeof fitted, maxc);

    Color fg = t->fg; fg.a *= alpha;
    f32 ty = y + h * 0.5f - ui_ch - 1.0f * dpi;
    draw_text_ex(r, fitted, text_x, ty, fg, maxc, ui_cw);

    /* Second line: what this agent is actually on.
     *
     * The order matters. A working pane is asked "what are you doing", and
     * PaneAgent.activity answers it from the agent's own screen — the chat
     * title, when there even is one, names the whole conversation and says
     * nothing about the current turn. Once the pane falls quiet the
     * conversation IS the answer, so the title takes the line back and the
     * last activity stands in when the tool never published one. */
    const char *topic = NULL;
    if (working && pa->activity[0])   topic = pa->activity;
    else if (pa->chat_title[0])       topic = pa->chat_title;
    else if (pa->activity[0])         topic = pa->activity;
    else if (!pa->tool)               topic = "no agent running";
    else                              topic = "waiting for a prompt";
    Color sub = t->fg; sub.a *= 0.55f * alpha;
    fit_text(topic, fitted, sizeof fitted, maxc);
    draw_text_ex(r, fitted, text_x, ty + ui_ch + 3.0f * dpi, sub, maxc, ui_cw);
    renderer_flush_glyphs(r);
}

/* ------------------------------------------------------------------ paint */

/* Completion cards put the conversation first, with agent identity above and
 * elapsed time / remaining agents below. Toast and notice share one surface. */
static void draw_done_notice(AppState *app, const PopupStyle *st,
                             const Tab *tab, f64 now, f32 alpha, f32 e) {
    Renderer *r = &app->renderer;
    const TabPopup *g = POP(app);
    f32 dpi = st->dpi;
    f32 pad = NOTIFICATION_PAD_PT * dpi;
    f32 tile = NOTIFICATION_TILE_PT * dpi;
    f32 gap = NOTIFICATION_GAP_PT * dpi;
    f32 ui_cw = st->ui_cw, ui_ch = st->ui_ch;
    f32 sm_cw = st->sm_cw, sm_ch = st->sm_ch;
    NotificationStyle ns = notification_style(st->theme, st->cp);

    char title[128], duration[24], metadata[96];
    snprintf(title, sizeof title, "%s finished", app_agent_display_name(g->done_tool));
    fmt_duration(g->done_secs, duration, sizeof duration);
    i32 still = tab ? tab->agent_working_count : 0;
    if (still > 0)
        snprintf(metadata, sizeof metadata, "%s · %d Agent%s still working",
                 duration, still, still == 1 ? "" : "s");
    else
        snprintf(metadata, sizeof metadata, "%s", duration);

    f32 need = fmaxf(notification_columns(title) * sm_cw,
                     notification_columns(metadata) * sm_cw);
    if (g->done_title[0])
        need = fmaxf(need, notification_columns(g->done_title) * ui_cw);
    f32 room = app->fb_width - 2.0f * NOTIFICATION_EDGE_PT * dpi;
    if (room < 2.0f * pad + tile + gap + 8.0f * ui_cw) return;
    f32 panel_w = roundf(fminf(room, fminf(NOTIFICATION_MAX_W_PT * dpi,
                             fmaxf(NOTIFICATION_NOTICE_MIN_W_PT * dpi, 2.0f * pad + tile + gap + need))));
    f32 text_w = panel_w - 2.0f * pad - tile - gap;
    NotificationText heading = notification_wrap(title, (i32)(text_w / sm_cw), 1);
    NotificationText topic = notification_wrap(g->done_title, (i32)(text_w / ui_cw), 2);
    NotificationText meta = notification_wrap(metadata, (i32)(text_w / sm_cw), 1);
    f32 topic_h = topic.count * ui_ch +
                 fmaxf(0.0f, topic.count - 1.0f) * NOTIFICATION_LINE_GAP_PT * dpi;
    f32 text_h = sm_ch + (topic.count ? NOTIFICATION_TOPIC_GAP_PT * dpi + topic_h : 0.0f) +
                 NOTIFICATION_META_GAP_PT * dpi + sm_ch;
    f32 panel_h = roundf(2.0f * pad + fmaxf(tile, text_h));

    /* Hang below the notices, as they were last drawn: the stack's own
     * extent, which follows a notice that is moving to its slot. */
    toast_stack_retire(&app->toasts, platform_time_sec());   /* a finished one holds no room */
    f32 stack = toast_stack_extent(&app->toasts);
    if (stack > 0.0f) stack += NOTIFICATION_STACK_GAP_PT * dpi;
    f32 px = roundf(app->fb_width - NOTIFICATION_EDGE_PT * dpi - panel_w +
                    (1.0f - e) * NOTIFICATION_SLIDE_PT * dpi);
    f32 py = roundf(app->tab_bar_height + NOTIFICATION_TOP_PT * dpi + stack);

    ns.body.a *= alpha; ns.border.a *= alpha;
    ns.fg.a *= alpha; ns.muted.a *= alpha; ns.accent.a *= alpha;
    ui_draw_panel_chrome(r, px, py, panel_w, panel_h, ns.body, ns.border,
                         NOTIFICATION_RADIUS_PT * dpi, dpi, 16.0f * dpi,
                         ns.shadow_alpha * alpha);
    renderer_flush_rrects(r);
    renderer_push_scissor(r, px, py, panel_w, panel_h);

    f32 col_x = px + pad + tile + gap;
    f32 text_y = py + pad;
    draw_agent_tile(app, st, g->done_tool, px + pad, text_y, tile, alpha);
    /* Completion check on the logo tile: readable without borrowing colour
     * from the chat topic or adding another competing status pill. */
    f32 check = 12.0f * dpi;
    f32 check_x = px + pad + tile - check + 3.0f * dpi;
    f32 check_y = text_y + tile - check + 3.0f * dpi;
    Color badge = ns.badge; badge.a *= alpha;
    renderer_draw_rrect_bordered(r, check_x, check_y, check, check,
                                 badge, ns.body, 2.0f * dpi,
                                 check * 0.5f, check * 0.5f, check * 0.5f, check * 0.5f,
                                 0.0f, 0.0f, 0.0f, 0.0f);
    renderer_flush_rrects(r);
    icon_draw(r, ICON_CHECK, check_x + 2.0f * dpi, check_y + 2.0f * dpi,
              check - 4.0f * dpi, ns.accent);

    renderer_set_ui_scale(r, sm_cw, sm_ch);
    draw_text_ex(r, heading.lines[0], col_x, text_y, ns.muted,
                 NOTIFICATION_TEXT_CAP, sm_cw);
    renderer_flush_glyphs(r);
    text_y += sm_ch;
    if (topic.count) {
        text_y += NOTIFICATION_TOPIC_GAP_PT * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);
        for (i32 i = 0; i < topic.count; i++)
            draw_text_ex(r, topic.lines[i], col_x,
                         text_y + i * (ui_ch + NOTIFICATION_LINE_GAP_PT * dpi),
                         ns.fg, NOTIFICATION_TEXT_CAP, ui_cw);
        renderer_flush_glyphs(r);
        text_y += topic_h;
    }
    renderer_set_ui_scale(r, sm_cw, sm_ch);
    draw_text_ex(r, meta.lines[0], col_x, text_y + NOTIFICATION_META_GAP_PT * dpi,
                 ns.muted, NOTIFICATION_TEXT_CAP, sm_cw);
    renderer_flush_glyphs(r);

    f32 left = clampf((f32)((g->done_until - now) / POPUP_DONE_HOLD_S), 0.0f, 1.0f);
    f32 bar_y = py + panel_h - NOTIFICATION_RAIL_INSET_PT * dpi;
    f32 bar_w = panel_w - 2.0f * pad;
    Color track = ns.border; track.a *= 0.45f;
    renderer_draw_rrect_simple(r, px + pad, bar_y, bar_w,
                               NOTIFICATION_PROGRESS_PT * dpi, track, dpi);
    renderer_draw_rrect_simple(r, px + pad, bar_y, bar_w * left,
                               NOTIFICATION_PROGRESS_PT * dpi,
                               (Color){ns.accent.r, ns.accent.g, ns.accent.b, 0.55f * alpha}, dpi);
    renderer_flush_rrects(r);
    renderer_reset_ui_scale(r);
    renderer_pop_scissor(r);
}

/* The hover preview: the agent list, hanging off the tab it describes. */
static void draw_agent_list(AppState *app, const PopupStyle *st,
                            const Tab *tab, f64 now, f32 alpha, f32 e) {
    Renderer *r = &app->renderer;
    TabPopup *g = POP(app);
    const Theme *t = st->theme;
    f32 dpi = st->dpi;

    f32 pad     = POPUP_PAD_PT * dpi;
    f32 hdr_h   = POPUP_HEADER_PT * dpi;
    f32 row_h   = POPUP_ROW_H_PT * dpi;
    f32 row_gap = POPUP_ROW_GAP_PT * dpi;
    f32 ui_cw = st->ui_cw, ui_ch = st->ui_ch;
    f32 sm_cw = st->sm_cw, sm_ch = st->sm_ch;

    f32 panel_w = POPUP_W_PT * dpi;
    f32 max_w = (f32)app->fb_width - 16.0f * dpi;
    if (panel_w > max_w) panel_w = max_w;

    i32 panes = (tab->kind == TAB_TERMINAL) ? app_tab_pane_count(tab) : 1;
    if (panes < 1) panes = 1;
    i32 rows   = panes > POPUP_MAX_ROWS ? POPUP_MAX_ROWS : panes;
    i32 hidden = panes - rows;

    f32 full_h = pad * 2.0f + hdr_h + (f32)rows * row_h +
                 (f32)(rows > 0 ? rows - 1 : 0) * row_gap;
    if (hidden > 0) full_h += ui_ch + row_gap;
    f32 max_h = (f32)app->fb_height - app->tab_bar_height -
                app->status_bar_height - 16.0f * dpi;
    if (full_h > max_h) full_h = max_h;
    if (full_h < 40.0f * dpi) return;         /* window too short to bother */

    /* Horizontal glide between tabs. dt-based so it lands in the same time
     * regardless of the frame rate the loop happens to be running at. */
    f32 target_cx = clampf(g->anchor_cx, panel_w * 0.5f + 8.0f * dpi,
                           (f32)app->fb_width - panel_w * 0.5f - 8.0f * dpi);
    if (!g->have_draw_cx) {
        g->draw_cx = target_cx;
        g->have_draw_cx = true;
        g->glide_t = now;
    } else {
        f64 dt = now - g->glide_t;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.1) dt = 0.1;
        g->glide_t = now;
        f32 k = (f32)(dt * 16.0);
        if (k > 1.0f) k = 1.0f;
        g->draw_cx += (target_cx - g->draw_cx) * k;
        if (fabsf(target_cx - g->draw_cx) < 0.5f) g->draw_cx = target_cx;
        else anim_register_until(now + 0.05);
    }

    f32 px = roundf(g->draw_cx - panel_w * 0.5f);
    f32 py = roundf(app->tab_bar_height + POPUP_GAP_PT * dpi +
                    (1.0f - e) * 6.0f * dpi);
    if (g->anchor_side) {
        /* Beside the docked list: flush to the sidebar's right edge, top
         * aligned with the hovered row and kept inside the window; the glide
         * runs vertically here, the same 16/s chase. */
        px = roundf(g->anchor_x + POPUP_GAP_PT * dpi);
        if (px + panel_w > (f32)app->fb_width - 8.0f * dpi)
            px = (f32)app->fb_width - 8.0f * dpi - panel_w;
        f32 target_y = clampf(g->anchor_y, app->tab_bar_height + 4.0f * dpi,
                              (f32)app->fb_height - app->status_bar_height - full_h - 8.0f * dpi);
        if (!g->have_draw_y) { g->draw_y = target_y; g->have_draw_y = true; }
        else {
            /* The horizontal glide above already consumed this frame's dt
             * (it stamps glide_t), so chase at a fixed fraction per painted
             * frame — the registration keeps them coming until it lands. */
            g->draw_y += (target_y - g->draw_y) * 0.30f;
            if (fabsf(target_y - g->draw_y) < 0.5f) g->draw_y = target_y;
            else anim_register_until(now + 0.05);
        }
        py = roundf(g->draw_y + (1.0f - e) * 6.0f * dpi);
    }
    /* The reveal: the box grows to full height while the content inside stays
     * put and is clipped — a drawer opening, not a scaling sticker. */
    f32 panel_h = roundf(full_h * (0.55f + 0.45f * e));

    Color body = st->cp.surface_raised; body.a = 0.985f * alpha;
    Color bd = { st->cp.divider_strong.r, st->cp.divider_strong.g,
                 st->cp.divider_strong.b, 0.85f * alpha };
    ui_draw_panel_chrome(r, px, py, panel_w, panel_h, body, bd,
                         POPUP_RADIUS_PT * dpi, dpi, 16.0f * dpi, 0.32f * alpha);
    renderer_flush_rrects(r);

    renderer_push_scissor(r, px, py, panel_w, panel_h);
    renderer_set_ui_scale(r, ui_cw, ui_ch);

    Color fg_main = t->fg;  fg_main.a *= alpha;
    Color fg_dim  = t->fg;  fg_dim.a  *= 0.55f * alpha;

    /* Header: the tab on the left, the working/idle tally on the right, both
     * on the same baseline band and inset by the same PAD as every row. */
    char sum[64];
    if (tab->agent_working_count > 0 &&
        tab->agent_count > tab->agent_working_count) {
        snprintf(sum, sizeof sum, "%d working · %d idle",
                 tab->agent_working_count,
                 tab->agent_count - tab->agent_working_count);
    } else if (tab->agent_working_count > 0) {
        snprintf(sum, sizeof sum, "%d working", tab->agent_working_count);
    } else if (tab->agent_count > 0) {
        snprintf(sum, sizeof sum, "%d Agent%s idle", tab->agent_count,
                 tab->agent_count == 1 ? "" : "s");
    } else {
        snprintf(sum, sizeof sum, "no agents");
    }
    i32 sn = cell_count(sum);
    f32 sum_x = px + panel_w - pad - (f32)sn * sm_cw;

    i32 hmax = (i32)((sum_x - 10.0f * dpi - (px + pad)) / ui_cw);
    if (hmax < 4) hmax = 4;
    char hfit[224];
    fit_text(tab_effective_title(tab), hfit, sizeof hfit, hmax);
    draw_text_ex(r, hfit, px + pad, py + pad, fg_main, hmax, ui_cw);
    renderer_flush_glyphs(r);

    renderer_set_ui_scale(r, sm_cw, sm_ch);
    draw_text_ex(r, sum, sum_x, py + pad + (ui_ch - sm_ch) * 0.5f,
                 fg_dim, sn, sm_cw);
    renderer_flush_glyphs(r);
    renderer_set_ui_scale(r, ui_cw, ui_ch);

    if (tab->kind == TAB_TERMINAL) {
        f32 ry = py + pad + hdr_h;
        for (i32 p = 0; p < rows; p++) {
            draw_agent_row(app, st, tab, p, panes, px + pad, ry,
                           panel_w - 2.0f * pad, row_h, now, alpha);
            renderer_set_ui_scale(r, ui_cw, ui_ch);
            ry += row_h + row_gap;
        }
        if (hidden > 0) {
            char more[48], mfit[64];
            snprintf(more, sizeof more, "+%d more pane%s", hidden,
                     hidden == 1 ? "" : "s");
            fit_text(more, mfit, sizeof mfit, hmax);
            draw_text_ex(r, mfit, px + pad, ry, fg_dim, hmax, ui_cw);
            renderer_flush_glyphs(r);
        }
    } else {
        /* File-browser / browser tabs run no agents; say what the tab is
         * rather than painting an empty agent list. */
        const char *what = (tab->kind == TAB_FILEBROWSER) ? "File browser — no agents"
                         : (tab->kind == TAB_SFTP)        ? "SFTP — local and host files"
                         : "Browser — no agents";
        char wfit[96];
        fit_text(what, wfit, sizeof wfit, hmax);
        draw_text_ex(r, wfit, px + pad, py + pad + hdr_h, fg_dim, hmax, ui_cw);
        renderer_flush_glyphs(r);
    }

    renderer_reset_ui_scale(r);
    renderer_pop_scissor(r);
}

void tab_popup_render(AppState *app) {
    if (!app) return;
    if (!tab_popup_visible(app)) return;
    TabPopup *g = POP(app);

    f64 now = platform_time_sec();

    /* Auto-dismiss a done notice; the preview is dismissed by the pointer. */
    if (g->kind == POPUP_DONE && g->want_open && now >= g->done_until)
        tab_popup_dismiss(app);

    /* The tab may have closed (or the strip shrunk) while the popup was
     * open. */
    if (g->tab < 0 || g->tab >= app->tab_count) {
        if (g->want_open) tab_popup_dismiss(app);
        if (anim_done(&g->close_anim)) { g->kind = POPUP_NONE; return; }
    }

    /* --- envelope: fade + the per-kind motion ---------------------------- */
    f32 e, alpha;
    if (g->want_open) {
        e = anim_eased(&g->open_anim, EASE_OUT_CUBIC);
        if (anim_done(&g->open_anim)) e = 1.0f;
        alpha = e;
    } else {
        f32 raw = anim_progress(&g->close_anim);
        if (anim_done(&g->close_anim)) {
            g->kind = POPUP_NONE;
            g->have_draw_cx = false;
            g->have_draw_y  = false;
            return;
        }
        f32 inv = 1.0f - raw;
        alpha = inv * inv;                   /* quadratic — no late snap */
        e = 1.0f - ease_apply(raw, EASE_IN_OUT_CUBIC) * 0.45f;
    }
    if (alpha <= 0.004f) return;

    const Tab *tab = (g->tab >= 0 && g->tab < app->tab_count)
                   ? &app->tabs[g->tab] : NULL;

    /* Frame pacing. A preview showing a working agent animates (durations tick,
     * the dot breathes) so it wants the soft rate; an all-idle preview is a
     * still image and only needs the odd frame. The notice has to stay awake
     * until its auto-dismiss is due — those frames drain its countdown. */
    bool anim_content = false;
    if (g->kind == POPUP_PREVIEW && tab) {
        for (i32 p = 0; p < MAX_SPLIT_PANES; p++) {
            if (tab->pane_agents[p].tool && now < tab->pane_agents[p].work_until) {
                anim_content = true;
                break;
            }
        }
    }
    if (e < 1.0f || !g->want_open) {
        anim_register_until(now + 0.05);          /* transition — real frames */
    } else if (g->kind == POPUP_DONE) {
        anim_register_soft_until(g->done_until + 0.05);
    } else if (anim_content) {
        anim_register_soft_until(now + 0.12);
    }
    /* else: a settled preview with nothing moving is a STATIC image. Asking
     * for frames would repaint the whole window ~30 times a second to redraw
     * the same pixels — and a pointer parked on a tab would do it forever.
     * The panel simply stays on screen; any incidental repaint (keystroke,
     * cursor blink, output) refreshes the durations it shows. */

    /* One theme resolve for the whole frame — see PopupStyle. */
    PopupStyle st = popup_style(app);
    if (g->kind == POPUP_DONE)          draw_done_notice(app, &st, tab, now, alpha, e);
    else if (tab)                      draw_agent_list(app, &st, tab, now, alpha, e);
}
