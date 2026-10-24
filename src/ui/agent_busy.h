/*
 * Liu — is a pane's agent still working?
 *
 * For an agent CLI that installs a stop hook this question is answered by the
 * agent itself and none of this runs. For every other one all there is to go
 * on is what the pane DREW, counted as cells whose codepoint changed (a
 * blinking cursor rewrites the same glyph and counts zero) and decayed with a
 * ~0.5 s time constant, so `out_accum` is roughly "changed cells per second / 2".
 *
 * Starting and staying lit are two questions with two thresholds (hysteresis).
 * Starting needs a burst — otherwise keystroke echo lights it — and 12 (about
 * 24 changed cells a second) is right for that. But an agent that is thinking
 * or running a tool draws only a spinner: one cell per tick, ten ticks a
 * second, `out_accum` about 5, so the start threshold alone would put the bar
 * out during exactly that wait. STAYING lit only requires that output has not
 * stopped. A spinner clears the sustain level with room to spare, an idle pane
 * sits at zero, and a status line ticking a clock (one or two cells a second)
 * stays below it.
 */
#ifndef UI_AGENT_BUSY_H
#define UI_AGENT_BUSY_H

#include "core/types.h"

#include <stdio.h>   /* snprintf, for agent_busy_tab_title */

/* Rising edge. ~24 changed cells/s — above keystroke echo and above a TUI
 * repainting a status line, below the slowest real token generation. */
#define TAB_ACCENT_WORK_THRESHOLD 12.0

/* Keep-alive level once lit. ~3 changed cells/s: a spinner ticking at four a
 * second or better clears it, a clock ticking one or two does not. */
#define TAB_ACCENT_WORK_SUSTAIN 1.5

/* Typing must not read as work. Keystroke echo and the per-key repaint a TUI
 * does land within a few ms of the key; real generation arrives later. */
#define TAB_ACCENT_KEY_GUARD 0.150

/* How long the gate stays armed after a BURST armed it. Its job is to bridge
 * the gaps between tokens, which is why it is seconds long. The tab popup
 * reports "quiet for N seconds" against this number. */
#define TAB_ACCENT_WORK_HOLD 2.5

/* ...and after the sustain level armed it. Much shorter on purpose: the
 * sustain path only fires while output is still trickling, so the trickle
 * itself is the liveness proof and there is no gap left to bridge. The full
 * hold would make the bar linger for seconds after a big answer finished — the
 * decay tail keeps clearing the sustain level long after the agent has
 * stopped. */
#define TAB_ACCENT_SUSTAIN_HOLD 1.0

/* How long should this pane's working gate be armed on this poll?
 *
 *   already_lit   the gate has not yet expired (now < work_until)
 *   out_accum     decayed changed-cell rate, see above
 *   key_age       seconds since the user last typed into this tab
 *
 * Returns the seconds to hold the gate open, or 0 to leave it alone. Hook-driven panes must not
 * ask at all — the agent's own start/stop is authoritative, and letting output
 * extend the gate on top of it would turn one long turn into a stream of
 * "finished" edges. */
static inline f64 agent_busy_arm_for(bool already_lit, f64 out_accum,
                                     f64 key_age) {
    if (key_age <= TAB_ACCENT_KEY_GUARD) return 0.0;
    if (out_accum > TAB_ACCENT_WORK_THRESHOLD) return TAB_ACCENT_WORK_HOLD;
    if (already_lit && out_accum > TAB_ACCENT_WORK_SUSTAIN)
        return TAB_ACCENT_SUSTAIN_HOLD;
    return 0.0;
}

/* What a tab pill says when 2+ of its panes are running an agent.
 *
 * A pill this narrow cannot name two agents, and naming only one of them would
 * misreport the tab — so it reports the COUNT. The part that has to be right
 * is WORKING vs idle: a tab with one agent mid-turn and one at its prompt, the
 * most common shape of a split agent tab, must not read as fully idle.
 *
 * Writes into `out` and returns it; `present` is assumed >= 2 (the caller
 * clears the title otherwise). Pure. */
/* --- a turn the agent itself opened ---------------------------------------
 *
 * An agent that reports its own turn edges (a START hook, then a COMPLETE)
 * cannot own the bar on those two signals alone. Claude Code does not run its
 * Stop hook when the user interrupts a turn, an API error ends a turn through
 * another hook altogether, and any COMPLETE can go astray on the way here —
 * each would leave a tab reading "working" with nothing running.
 *
 * So an open turn has to keep proving it is alive, the same way the heuristic
 * does: by drawing. A working agent animates (a spinner, a seconds counter);
 * one that was interrupted, is waiting at a permission prompt or died draws
 * nothing. A change on the pane's screen that is not the echo of a key keeps
 * the bar lit for TAB_AGENT_HOOK_LIVE_HOLD; without one it goes out by
 * itself.
 *
 * Coming BACK from dark takes more than one change. A turn left open by an
 * interrupt stays open until the next START, and in that state a lone repaint
 * (the window was resized, a status line refreshed) would otherwise read as
 * three seconds of work in every such pane at once. Activity is two changes
 * at least TAB_AGENT_HOOK_RESUME_GAP apart inside
 * TAB_AGENT_HOOK_RESUME_WINDOW: a spinner passes in 0.4 s, a once-a-second
 * counter in one second, a single burst of repaint never. */
#define TAB_AGENT_HOOK_LIVE_HOLD     3.0
#define TAB_AGENT_HOOK_RESUME_GAP    0.4
#define TAB_AGENT_HOOK_RESUME_WINDOW 2.5

/* The pane's screen changed during an open hook turn. `cap_until` is the hard
 * ceiling on the turn (its START plus TAB_AGENT_HOOK_WORK_MAX). Returns true
 * on the rising edge — the bar was dark and is lit again — so the caller can
 * re-anchor its grow-in. */
static inline bool agent_busy_hook_change(f64 now, f64 key_age, f64 cap_until,
                                          f64 *work_until, f64 *probe_t) {
    if (key_age <= TAB_ACCENT_KEY_GUARD) return false;   /* echo, not the agent */
    if (now >= cap_until) return false;
    f64 until = now + TAB_AGENT_HOOK_LIVE_HOLD;
    if (until > cap_until) until = cap_until;
    if (now < *work_until) {
        if (until > *work_until) *work_until = until;
        *probe_t = 0.0;
        return false;
    }
    if (*probe_t <= 0.0 || now - *probe_t > TAB_AGENT_HOOK_RESUME_WINDOW) {
        *probe_t = now;
        return false;
    }
    if (now - *probe_t < TAB_AGENT_HOOK_RESUME_GAP) return false;
    *probe_t    = 0.0;
    *work_until = until;
    return true;
}

static inline void agent_busy_tab_title(char *out, usize cap,
                                        i32 present, i32 working) {
    if (!out || cap == 0) return;
    if (working <= 0) {
        snprintf(out, cap, "%d Agents", present);
    } else if (working >= present) {
        snprintf(out, cap, "%d Agents working", working);
    } else {
        /* The mixed case. "1 of 2 working" fits the pill and says the two
         * things a glance needs: something IS running, and not everything. */
        snprintf(out, cap, "%d of %d working", working, present);
    }
}

#endif /* UI_AGENT_BUSY_H */
