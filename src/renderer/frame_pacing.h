#ifndef LIU_FRAME_PACING_H
#define LIU_FRAME_PACING_H

#include "core/types.h"

/* A live display link owns the cadence. Its tick must not also wait for a
 * software deadline derived from the previous (possibly late) submission.
 * The deadline is only a fallback: the last software grid deadline plus at
 * least one observed callback interval. */
static inline bool frame_pacing_due(bool link, bool fresh_tick, bool paced,
                                    bool input, bool input_bypass, f64 now,
                                    f64 next, f64 period) {
    if (link) {
        if (fresh_tick) return true;
        if (input) return input_bypass || next <= 0.0 || now >= next;
        return next <= 0.0 || now >= next + period;
    }
    return !paced || input || now >= next;
}

/* Same deadline as the paint gate: a pending tick requires no extra wait;
 * a stalled link eventually falls back instead of leaving a dirty frame stuck. */
static inline f64 frame_pacing_wait(bool link, bool fresh_tick, f64 now,
                                    f64 next, f64 period) {
    if (link && fresh_tick) return 0.0;
    f64 due = next + (link ? period : 0.0);
    return due > now ? due - now : 0.0;
}

/* Keep software pacing on its existing grid despite wake-up jitter. A missed
 * whole period or an early input frame starts a new grid, with no catch-up burst.
 * With a link this is only the watchdog deadline, relative to the last frame. */
static inline f64 frame_pacing_advance(bool link, f64 now, f64 next, f64 period) {
    if (!link && next > 0.0 && now >= next && now < next + period)
        return next + period;
    return now + period;
}

/* A window can move from a slower display to a faster one while the software
 * grid still points at the old refresh slot. Give the new mode its own phase
 * immediately instead of carrying one stale old-rate deadline forward. */
static inline f64 frame_pacing_rebase_rate(f64 next, f64 now,
                                          f64 old_hz, f64 new_hz) {
    f64 delta = old_hz > new_hz ? old_hz - new_hz : new_hz - old_hz;
    return old_hz > 1.0 && new_hz > 1.0 && delta >= 0.5 ? now : next;
}

/* A variable-rate link may tick below the panel's advertised maximum rate.
 * Its no-tick fallback must follow the observed period, or it manufactures
 * extra frames before the next real 24/30 Hz tick. Keep the advertised period
 * as the floor so callback jitter never makes the watchdog more aggressive. */
static inline f64 frame_pacing_link_watchdog_period(f64 max_rate_period,
                                                    f64 observed_tick_period) {
    return observed_tick_period > max_rate_period ? observed_tick_period : max_rate_period;
}

/* How long a running link may stay silent before it stops being the frame's
 * clock. A fresh tick authorises a frame at once. Without this limit a frame
 * would wait for a watchdog built from the link's own last tick gap, so a link
 * whose callbacks come late, or stop coming, would set the window's frame rate
 * by itself. Three refresh periods (never under 50 ms) is long enough that
 * ordinary callback jitter never trips it and short enough that the hand-over
 * is not visible. */
static inline f64 frame_pacing_link_silence_limit(f64 panel_period) {
    f64 limit = panel_period * 3.0;
    return limit > 0.050 ? limit : 0.050;
}

/* A running link counts only while it is actually ticking. A silent one is
 * treated exactly like no link at all: the software grid paces the frames at
 * the panel rate until ticks return. */
static inline bool frame_pacing_link_live(bool running, f64 silence_sec,
                                          f64 panel_period) {
    return running && silence_sec < frame_pacing_link_silence_limit(panel_period);
}

/* Estimate the watchdog interval from the smallest recent callback gap rather
 * than the latest gap. This avoids treating a delayed callback as a lower
 * refresh rate while following a sustained slower cadence. */
static inline u64 frame_pacing_min_gap(const u64 *gaps, i32 count) {
    u64 best = 0;
    for (i32 i = 0; i < count; i++)
        if (gaps[i] && (best == 0 || gaps[i] < best)) best = gaps[i];
    return best;
}

#endif
