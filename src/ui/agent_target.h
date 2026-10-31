/*
 * Which pane of a tab receives text Liu hands to an agent: an element picked
 * in the browser, a page's connection error, a vault source.
 *
 * A tab can hold several agents side by side, and the one meant is the one the
 * user was working with — the pane that has the keyboard, or, while the
 * browser pane has it (a pick is made there), the terminal pane that had it
 * last. Only when neither runs an agent is the first pane that does taken.
 * A pane without an agent is never chosen here: the caller decides what to do
 * with no agent at all.
 */
#ifndef UI_AGENT_TARGET_H
#define UI_AGENT_TARGET_H

#include "core/types.h"

/* The agent running in `pane` (a ChatTool value, 0 = none). Asked at most
 * once per pane per call. */
typedef i32 (*AgentTargetProbe)(void *ctx, i32 pane);

/* `active`: the tab's active pane. `browser_pane`: its browser pane, -1 when
 * it has none. `last_term`: the terminal pane that last had the keyboard, -1
 * when unknown. Returns the pane, -1 when no pane runs an agent; `*tool_out`
 * gets that pane's agent. */
static inline i32 agent_target_pane(i32 panes, i32 active, i32 browser_pane,
                                    i32 last_term, AgentTargetProbe probe,
                                    void *ctx, i32 *tool_out) {
    if (tool_out) *tool_out = 0;
    if (!probe || panes <= 0) return -1;
    i32 order[2] = { active, last_term };
    i32 asked[2] = { -1, -1 };
    for (i32 k = 0; k < 2; k++) {
        i32 p = order[k];
        if (p < 0 || p >= panes || p == browser_pane) continue;
        if (k == 1 && p == asked[0]) continue;
        asked[k] = p;
        i32 tool = probe(ctx, p);
        if (tool) {
            if (tool_out) *tool_out = tool;
            return p;
        }
    }
    for (i32 p = 0; p < panes; p++) {
        if (p == browser_pane || p == asked[0] || p == asked[1]) continue;
        i32 tool = probe(ctx, p);
        if (tool) {
            if (tool_out) *tool_out = tool;
            return p;
        }
    }
    return -1;
}

#endif
