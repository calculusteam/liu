/*
 * Map X server event timestamps to the application's monotonic clock.
 *
 * XKeyEvent.time is a wrapping 32-bit millisecond counter with no fixed offset
 * from the application clock. Calibrate the offset with a server-time round trip
 * and carry half the round-trip duration as uncertainty. Refresh the calibration
 * periodically; both clocks are monotonic, but recalibration detects changes.
 *
 * Map a stamp only when a recent calibration validates it. Mark it uncertain when
 * no calibration exists, the calibration is stale, the mapped time is in the
 * future beyond the error bound, or the stamp is CurrentTime. Count uncertain
 * stamps without including them in latency measurements.
 *
 * Unwrap the counter by applying signed differences between consecutive stamps.
 * This assumes consecutive events are less than half the counter range apart.
 *
 * The mapping is clock arithmetic without X11 dependencies, so platform code and
 * other callers can use it.
 */
#ifndef LIU_X_TIME_MAP_H
#define LIU_X_TIME_MAP_H

#include "core/types.h"

/* How often the platform re-verifies the offset while it has a connection.
 * Cheap: one ChangeProperty request and one PropertyNotify, no reply wait. */
#define X_TIME_CAL_INTERVAL_SEC   10.0
/* How old a calibration may be for a stamp measured against it to count. A
 * stall longer than this also stalls the pings, so the offset has not been
 * checked across the gap; the stamps that then flood in are counted, not
 * histogrammed, and the next ping — sent immediately — restores certainty. */
#define X_TIME_CAL_FRESH_SEC      30.0
/* A calibration landing this far from the previous one means a clock moved. */
#define X_TIME_REBASE_TOL_SEC     0.25
/* Slack on top of the calibration error bar before a stamp that maps into the
 * future is called a contradiction: X stamps are whole milliseconds. */
#define X_TIME_FUTURE_SLACK_SEC   0.002

typedef struct {
    /* ---- the offset: app_seconds = offset + server_seconds ---- */
    bool calibrated;        /* offset came from a server round trip */
    f64  offset;
    f64  cal_uncertainty;   /* +- seconds, half the round trip that produced it */
    f64  cal_at;            /* app clock when the calibration landed */
    /* ---- 32-bit counter unwrapping ---- */
    bool have_ms;
    u32  last_ms;
    i64  unwrapped_ms;      /* running 64-bit server time, wraps folded in */
    /* ---- what the platform should do next ---- */
    bool want_cal;          /* a stamp could not be vouched for: ping now */
    /* ---- diagnostics, monotonic ---- */
    u64  samples;           /* stamps mapped */
    u64  uncertain;         /* of those, reported as not measurable */
    u64  wraps;             /* counter wraps folded */
    u64  rebases;           /* calibrations that contradicted the previous one */
    u64  calibrations;
} XTimeMap;

/* Fold a raw server stamp into the running 64-bit counter. */
static inline i64 x_time_unwrap(XTimeMap *m, u32 server_ms) {
    if (!m->have_ms) {
        m->have_ms      = true;
        m->last_ms      = server_ms;
        m->unwrapped_ms = (i64)server_ms;
        return m->unwrapped_ms;
    }
    /* Signed 32-bit difference: a wrap from 0xFFFFFF00 to 0x000000FF reads as
     * +511, and a stamp a few ms behind the previous one (XI2 and core events
     * interleaving) reads as a small negative. */
    i32 delta = (i32)(server_ms - m->last_ms);
    if (delta > 0 && server_ms < m->last_ms) m->wraps++;
    m->unwrapped_ms += delta;
    m->last_ms = server_ms;
    return m->unwrapped_ms;
}

/* A server round trip: the request was sent at `t_before` and its PropertyNotify
 * was dispatched at `t_after` (both on the app clock); the notify carried
 * `server_ms`. The server processed the request somewhere inside that window, so
 * offset is inside [t_before - server, t_after - server]; the midpoint is taken
 * with the half-width as the error bar. A wide bracket (the loop was busy when
 * the reply came) is not wrong, only imprecise, and the next ping narrows it. */
static inline void x_time_map_calibrate(XTimeMap *m, u32 server_ms,
                                        f64 t_before, f64 t_after) {
    if (!m || server_ms == 0) return;
    if (t_after < t_before) t_after = t_before;
    f64 server = (f64)x_time_unwrap(m, server_ms) / 1000.0;
    f64 lo = t_before - server, hi = t_after - server;
    f64 est = 0.5 * (lo + hi);
    f64 unc = 0.5 * (hi - lo);
    if (m->calibrated) {
        f64 moved = est - m->offset;
        if (moved < 0) moved = -moved;
        if (moved > m->cal_uncertainty + unc + X_TIME_REBASE_TOL_SEC) {
            m->rebases++;
        } else if (unc > m->cal_uncertainty &&
                   m->offset >= lo - X_TIME_FUTURE_SLACK_SEC &&
                   m->offset <= hi + X_TIME_FUTURE_SLACK_SEC) {
            /* A WIDER bracket that still contains the current offset is a
             * confirmation, not a better number: the reply was dispatched late (the
             * loop was busy), and the midpoint of a wide window is no improvement
             * on a narrow one. Replacing the offset with it would widen the error
             * bar and flatten every later reading. Keep the tight offset, refresh
             * its age. */
            m->cal_at   = t_after;
            m->want_cal = false;
            m->calibrations++;
            return;
        }
    }
    m->calibrated      = true;
    m->offset          = est;
    m->cal_uncertainty = unc;
    m->cal_at          = t_after;
    m->want_cal        = false;
    m->calibrations++;
}

/* Does the platform owe the server a ping? True when nothing has been measured
 * yet, when the last calibration is due for a refresh, or when a stamp could not
 * be vouched for. The caller keeps at most one ping in flight. */
static inline bool x_time_map_wants_calibration(const XTimeMap *m, f64 now) {
    if (!m) return false;
    if (!m->calibrated) return true;
    if (m->want_cal) return true;
    return now - m->cal_at >= X_TIME_CAL_INTERVAL_SEC;
}

/* `server_ms` is the raw 32-bit server timestamp; `now` is the app's monotonic
 * clock read at dispatch. Returns the event time on the app's clock and says in
 * `*out_certain` whether that number is a measurement or a placeholder. An
 * uncertain answer is `now`, the dispatch clock, so a caller that ignores the
 * flag gets the dispatch time and a caller that honours it never histograms a
 * guess. */
static inline f64 x_time_map_ex(XTimeMap *m, u32 server_ms, f64 now,
                                bool *out_certain) {
    bool certain = false;
    f64  mapped  = now;
    if (!m || server_ms == 0) {              /* CurrentTime: no stamp at all */
        if (out_certain) *out_certain = false;
        return now;
    }
    m->samples++;
    f64 server = (f64)x_time_unwrap(m, server_ms) / 1000.0;
    if (!m->calibrated) {
        m->want_cal = true;
    } else if (now - m->cal_at > X_TIME_CAL_FRESH_SEC) {
        m->want_cal = true;                   /* stale: the loop was away too long */
    } else {
        mapped = m->offset + server;
        /* A stamp can map a hair into the future: the offset has an error bar
         * and the server rounds to whole milliseconds. Past the bar it is a
         * contradiction — a queue delay is never negative — so a clock moved. */
        f64 slack = m->cal_uncertainty + X_TIME_FUTURE_SLACK_SEC;
        if (mapped > now + slack) {
            m->want_cal = true;
            mapped = now;
        } else {
            if (mapped > now) mapped = now;
            certain = true;
        }
    }
    if (!certain) m->uncertain++;
    if (out_certain) *out_certain = certain;
    return mapped;
}

/* The pre-F05 signature, kept for callers that only want a number. */
static inline f64 x_time_map(XTimeMap *m, u32 server_ms, f64 now) {
    return x_time_map_ex(m, server_ms, now, NULL);
}

#endif /* LIU_X_TIME_MAP_H */
