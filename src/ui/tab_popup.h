/*
 * Liu — tab popup: the panel that drops out from under the tab strip.
 *
 * One surface, two sources:
 *
 *   PREVIEW  Hovering a tab THAT IS RUNNING AN AGENT and holding still for the
 *            dwell opens the tab's agent list: one row per pane — icon, agent,
 *            the chat's topic and a status pill saying "working 1m 12s" or
 *            "idle 40s". Follows the pointer from tab to tab without
 *            re-opening. Plain shell tabs never open it.
 *
 *   DONE     A pane's agent finished a working stretch: a top-right card shows
 *            the agent, chat topic, duration and remaining working agents,
 *            plays the completion sound, and auto-dismisses.
 *
 * Cost model — nothing here runs unless the panel is on screen or a dwell is
 * in flight. tab_popup_hover() is a couple of comparisons per frame, and the
 * paint reads Tab.pane_agents (which the session poll already maintains): no
 * terminal grids are walked and no processes are probed.
 */
#ifndef UI_TAB_POPUP_H
#define UI_TAB_POPUP_H

#include "core/types.h"

typedef struct AppState AppState;

/* Pointer report, called once per frame from render_toolbar with the tab the
 * pointer is over (-1 = none) and that tab's painted pill rect. Starts /
 * cancels the dwell timer and drives the open + glide animations. */
void tab_popup_hover(AppState *app, i32 tab_index, f32 pill_x, f32 pill_w);

/* Same dwell/open rule as tab_popup_hover, for a tab whose pill is a ROW in
 * the docked sidebar: the panel opens to the right of `sidebar_right`, top
 * aligned with `row_y`, instead of under the strip. */
void tab_popup_hover_side(AppState *app, i32 tab_index, f32 sidebar_right, f32 row_y);
/* Opens the top-right DONE card for `tab_index` and plays the completion sound.
 * An empty `chat_title` omits the topic; `tool` is a ChatTool as i32. */
void tab_popup_agent_done(AppState *app, i32 tab_index, i32 pane, i32 tool,
                          const char *chat_title, f64 work_sec);
/* The configured sound for (tool, event), from the app itself — only while Liu
 * is the active app, which is when the notify server plays nothing. `tool` is
 * a ChatTool as i32, `event` a NotifyEvent. Main thread. */
void tab_popup_play_event_sound(i32 tool, u8 event);

/* Close whatever is open (click, keystroke, tab drag, tab close, modal open).
 * Plays the close animation rather than cutting.
 *
 * Takes the window: the panel is per AppState (TabPopup in ui.h), because its
 * `tab` is an index into THAT window's tabs[] and its dwell is driven from a
 * render path that runs for every window every frame. */
void tab_popup_dismiss(AppState *app);

/* True while anything is painted or animating in `app`'s window — used by
 * hit-testing and the frame pacing. */
bool tab_popup_visible(AppState *app);

/* Draw the panel. Cheap no-op when nothing is open. */
void tab_popup_render(AppState *app);

#endif /* UI_TAB_POPUP_H */
