/*
 * Render benchmark metrics record frame phases and input-to-presentation
 * latency so the diagnostics panel can distinguish rendering time from
 * upstream session and scheduling delays.
 */
#ifndef UI_BENCH_METRICS_H
#define UI_BENCH_METRICS_H

#include "core/types.h"
#include "core/percentile.h"

/* Rolling report window. Matches the HUD's existing CPU/RAM cadence so every
 * number on the panel refreshes together — a panel whose halves update at
 * different rates reads as flicker, not as data. */
#define BENCH_WINDOW_SEC 1.0

/* How long a hitch stays on the panel. One window is one second, which is less
 * time than it takes to look at a number — and a screen recording of this
 * panel is how a "it lags" report usually arrives. */
#define BENCH_RECENT_WINDOWS 10
/* A loop turn longer than this is a frame the user did not get. */
#define BENCH_LONG_STALL_SEC (1.0 / 60.0)

typedef struct BenchMetrics {
    /* Native event time to PTY acceptance/redraw request/next frame. Pending
     * events are retained independently; unrelated modifiers aren't queued.
     * Frame presentation does not assert that a remote echo is visible. */
    f64     pending_keys[1024];
    f64     pending_handled[1024];
    u32     pending_head, pending_count;
    u64     input_samples_dropped;
    /* Native stamps the platform could not vouch for (PlatformEvent
     * .timestamp_uncertain). Counted, never histogrammed: the number the HUD
     * would otherwise show is the dispatch-relative wait, which
     * under-reports the latency. */
    u64     input_samples_uncertain;
    f64     last_event_time;
    f64     key_t0;
    bool    key_queued;
    f64     key_handled;
    bool    key_pty_seen;
    bool    key_req_seen;
    LiuHist key_pty, key_req, key_paint;
    f64     last_pty_ms, last_req_ms, last_paint_ms;

    /* ---- pty ----------------------------------------------------------- */
    u64     pty_bytes;        /* accumulating over the window */
    u64     pty_chunks;       /* session_read calls that returned bytes */
    u64     pty_parse_ns;     /* time inside terminal_feed */
    i32     pty_queue_bytes;  /* still buffered at the end of the last poll */
    /* When the queue last went from empty to non-empty. The oldest byte in a
     * FIFO cannot be younger than that, and it is the closest bound
     * available without stamping every chunk — the ring carries bytes, not
     * timestamps, and adding one per chunk would put a clock read in the
     * reader thread's hot loop. */
    f64     pty_backlog_since;
    f64     out_bytes_sec, out_chunks_sec, out_parse_ms, out_oldest_ms;

    /* ---- ui ------------------------------------------------------------ */
    LiuHist stall;            /* per-iteration loop work, event wait excluded */
    /* Record long frame stalls separately from percentiles, which can hide
     * pauses that occur in fewer than one percent of samples. */
    f64     stall_worst_cur;
    u32     stall_long_cur;
    f64     stall_worst_ring[BENCH_RECENT_WINDOWS];
    u32     stall_long_ring[BENCH_RECENT_WINDOWS];
    /* How long a WANTED frame waited: from the paint gate first seeing the
     * request to the frame being drawn. Pacing makes up to one refresh period
     * of that ordinary; `late` counts the ones past two. */
    f64     wait_worst_cur;
    u32     late_cur;
    f64     wait_worst_ring[BENCH_RECENT_WINDOWS];
    u32     late_ring[BENCH_RECENT_WINDOWS];
    u32     recent_i;
    f64     out_stall_worst_ms, out_wait_worst_ms;
    u32     out_stall_long, out_late;
    u32     wakes;            /* loop wake-ups requested from anywhere */
    u32     renders;          /* app_render calls */
    u32     bg_renders;       /* of those, into a window that is not focused */
    u32     frames;           /* loop iterations in the window */
    f64     out_wakes_frame, out_renders_frame, out_bg_frame;

    /* ---- grid ----------------------------------------------------------- */
    u64     rows_dirty;
    u64     cells_rebuilt;
    u64     glyphs_shaped;    /* instances emitted by a REBUILD, not a replay */
    u64     glyphs_total;     /* every glyph instance the frame emitted */
    f64     out_rows_frame, out_cells_frame, out_glyphs_frame, out_glyphs_total;

    /* Wake counter reading: the platform counter is monotonic and shared, so
     * this side keeps its own last-seen value and subtracts (the same rule
     * the GPU counters follow). */
    u64     wake_seen;

    /* `window_open` rather than a `window_t0 > 0` test: the frame clock is
     * monotonic from an arbitrary origin and CAN read 0 early in a process,
     * and a zero start time would read as "no window yet" on every tick —
     * the window would keep reopening and every rate would be divided by the
     * gap since the previous tick instead of by the window. */
    bool    window_open;
    f64     window_t0;
    u32     window_frames;    /* frames folded into the last published window */
} BenchMetrics;

/* ---------------------------------------------------------------- input */

static inline void bench_key_begin(BenchMetrics *m, f64 now) {
    if (!m) return;
    if (now <= 0) return;
    if (now == m->last_event_time) { m->key_t0=now; return; }
    m->last_event_time=now;
    m->key_t0 = now;
    m->key_queued = false;
    m->key_pty_seen = false;
    m->key_req_seen = false;
}

/* Ignore keystrokes without a reliable event timestamp so they cannot
 * inherit a previous key's start time. */
static inline void bench_key_uncertain(BenchMetrics *m) {
    if (!m) return;
    m->input_samples_uncertain++;
    m->key_t0 = 0;
    m->key_queued = false;
    m->key_pty_seen = false;
    m->key_req_seen = false;
}

static inline void bench_key_queue(BenchMetrics *m, f64 handled) {
    if (m->key_queued) return;
    m->key_queued = true;
    m->key_handled = handled;
    if (m->pending_count < 1024) {
        m->pending_keys[(m->pending_head+m->pending_count)%1024]=m->key_t0;
        m->pending_handled[(m->pending_head+m->pending_count)%1024]=handled;
        m->pending_count++;
    } else m->input_samples_dropped++;
}

static inline void bench_key_pty(BenchMetrics *m, f64 now) {
    if (!m || m->key_t0 <= 0.0 || m->key_pty_seen) return;
    bench_key_queue(m, now);
    m->key_pty_seen = true;
    m->last_pty_ms = (now - m->key_t0) * 1000.0;
    liu_hist_add_sec(&m->key_pty, now - m->key_t0);
}

static inline void bench_key_request(BenchMetrics *m, f64 now) {
    if (!m || m->key_t0 <= 0.0 || m->key_req_seen) return;
    bench_key_queue(m, now);
    m->key_req_seen = true;
    m->last_req_ms = (now - m->key_t0) * 1000.0;
    liu_hist_add_sec(&m->key_req, now - m->key_t0);
}

/* Retire only input processed before this frame was submitted. On Metal,
 * now is MTLDrawable.presentedTime; on GL the UI explicitly labels submission.
 * This is event-to-frame timing, not proof that a remote echo is in the frame. */
static inline void bench_key_presented(BenchMetrics *m, f64 through, f64 now) {
    if (!m || now <= 0) return;
    f64 worst=0;
    while (m->pending_count) {
        f64 t=m->pending_keys[m->pending_head];
        if (m->pending_handled[m->pending_head]>through || t>now) break;
        liu_hist_add_sec(&m->key_paint,now-t);
        if ((now-t)*1000>worst) worst=(now-t)*1000;
        m->pending_head=(m->pending_head+1)%1024;m->pending_count--;
    }
    if (worst>0) m->last_paint_ms=worst;
    if (m->key_queued && m->key_handled<=through) m->key_t0=0;
}
static inline void bench_key_paint(BenchMetrics *m, f64 now) {
    bench_key_presented(m,now,now);
}

/* ------------------------------------------------------------------ pty */

static inline void bench_pty_read(BenchMetrics *m, i32 bytes, u64 parse_ns) {
    if (!m || bytes <= 0) return;
    m->pty_bytes  += (u64)bytes;
    m->pty_chunks += 1;
    m->pty_parse_ns += parse_ns;
}

static inline void bench_pty_queue(BenchMetrics *m, i32 pending, f64 now) {
    if (!m) return;
    if (pending > 0) {
        if (m->pty_queue_bytes <= 0) m->pty_backlog_since = now;
    } else {
        m->pty_backlog_since = 0.0;
    }
    m->pty_queue_bytes = pending;
}

/* ------------------------------------------------------------------- ui */

/* One loop turn's own work (the event wait excluded). */
static inline void bench_stall_note(BenchMetrics *m, f64 sec) {
    if (!m || sec < 0.0) return;
    liu_hist_add_sec(&m->stall, sec);
    if (sec > m->stall_worst_cur) m->stall_worst_cur = sec;
    if (sec > BENCH_LONG_STALL_SEC) m->stall_long_cur++;
}

/* A frame that was wanted for `wait_sec` before it was drawn, on a panel whose
 * refresh period is `period_sec`. */
static inline void bench_frame_wait(BenchMetrics *m, f64 wait_sec, f64 period_sec) {
    if (!m || wait_sec < 0.0) return;
    if (wait_sec > m->wait_worst_cur) m->wait_worst_cur = wait_sec;
    if (period_sec > 0.0 && wait_sec > 2.0 * period_sec) m->late_cur++;
}

/* ---------------------------------------------------------------- frame */

static inline void bench_frame_grid(BenchMetrics *m, i32 rows_dirty,
                                    i32 cells, i32 glyphs_rebuilt,
                                    i32 glyphs_total) {
    if (!m) return;
    if (rows_dirty > 0)      m->rows_dirty    += (u64)rows_dirty;
    if (cells > 0)           m->cells_rebuilt += (u64)cells;
    if (glyphs_rebuilt > 0)  m->glyphs_shaped += (u64)glyphs_rebuilt;
    if (glyphs_total > 0)    m->glyphs_total  += (u64)glyphs_total;
}

/* Publish per-window rates. Called on the HUD's 1 Hz tick with the wake
 * counter's current total; returns false when the window is not up yet, so
 * the caller can leave the displayed numbers alone rather than divide by a
 * fraction of a second. */
static inline bool bench_window_tick(BenchMetrics *m, f64 now, u64 wake_total) {
    if (!m) return false;
    if (!m->window_open) {
        m->window_open = true;
        m->window_t0 = now;
        m->wake_seen = wake_total;
        return false;
    }
    f64 dt = now - m->window_t0;
    if (dt < BENCH_WINDOW_SEC) return false;

    m->wakes = (u32)(wake_total - m->wake_seen);
    m->wake_seen = wake_total;

    f64 frames = m->frames > 0 ? (f64)m->frames : 1.0;
    m->out_bytes_sec    = (f64)m->pty_bytes / dt;
    m->out_chunks_sec   = (f64)m->pty_chunks / dt;
    m->out_parse_ms     = (f64)m->pty_parse_ns / 1e6 / frames;
    m->out_oldest_ms    = (m->pty_queue_bytes > 0 && m->pty_backlog_since > 0.0)
                          ? (now - m->pty_backlog_since) * 1000.0 : 0.0;
    m->out_wakes_frame  = (f64)m->wakes / frames;
    m->out_renders_frame= (f64)m->renders / frames;
    m->out_bg_frame     = (f64)m->bg_renders / frames;
    /* Per PAINTED frame, not per loop iteration: a grid number divided by
     * iterations reads near zero on an idle loop that paints nothing, which
     * says something true about the loop and nothing at all about the grid. */
    f64 painted = m->renders > 0 ? (f64)m->renders : 1.0;
    m->out_rows_frame   = (f64)m->rows_dirty / painted;
    m->out_cells_frame  = (f64)m->cells_rebuilt / painted;
    m->out_glyphs_frame = (f64)m->glyphs_shaped / painted;
    m->out_glyphs_total = (f64)m->glyphs_total / painted;

    /* Close this window's worst-case figures into the ring and publish the
     * ring's worst and total: what happened in the last ten seconds. */
    {
        u32 slot = m->recent_i % BENCH_RECENT_WINDOWS;
        m->stall_worst_ring[slot] = m->stall_worst_cur;
        m->stall_long_ring[slot]  = m->stall_long_cur;
        m->wait_worst_ring[slot]  = m->wait_worst_cur;
        m->late_ring[slot]        = m->late_cur;
        m->recent_i = (slot + 1) % BENCH_RECENT_WINDOWS;
        m->stall_worst_cur = m->wait_worst_cur = 0.0;
        m->stall_long_cur = m->late_cur = 0;
        f64 sw = 0.0, ww = 0.0; u32 sl = 0, lt = 0;
        for (u32 i = 0; i < BENCH_RECENT_WINDOWS; i++) {
            if (m->stall_worst_ring[i] > sw) sw = m->stall_worst_ring[i];
            if (m->wait_worst_ring[i]  > ww) ww = m->wait_worst_ring[i];
            sl += m->stall_long_ring[i];
            lt += m->late_ring[i];
        }
        m->out_stall_worst_ms = sw * 1000.0;
        m->out_wait_worst_ms  = ww * 1000.0;
        m->out_stall_long = sl;
        m->out_late = lt;
    }

    m->window_frames = m->frames;
    m->window_t0 = now;
    m->pty_bytes = m->pty_chunks = m->pty_parse_ns = 0;
    m->renders = m->bg_renders = m->frames = 0;
    m->rows_dirty = m->cells_rebuilt = m->glyphs_shaped = m->glyphs_total = 0;
    return true;
}

/* The latency histograms are CUMULATIVE across the time the HUD is on, unlike
 * the per-second rates above: a p99 wants thousands of samples and a one
 * second window at 60 Hz has sixty. Reset when the HUD is toggled on. */
static inline void bench_metrics_reset(BenchMetrics *m) {
    if (!m) return;
    liu_hist_reset(&m->key_pty);
    liu_hist_reset(&m->key_req);
    liu_hist_reset(&m->key_paint);
    liu_hist_reset(&m->stall);
    m->key_t0 = 0.0;
    m->last_event_time = 0;
    m->key_queued = false; m->key_handled = 0;
    m->pending_head=m->pending_count=0;
    m->input_samples_dropped=0;
    m->key_pty_seen = m->key_req_seen = false;
    m->last_pty_ms = m->last_req_ms = m->last_paint_ms = 0.0;
    m->pty_bytes = m->pty_chunks = m->pty_parse_ns = 0;
    m->pty_queue_bytes = 0;
    m->pty_backlog_since = 0.0;
    m->wakes = m->renders = m->bg_renders = m->frames = 0;
    m->rows_dirty = m->cells_rebuilt = m->glyphs_shaped = m->glyphs_total = 0;
    m->window_open = false;
    m->window_t0 = 0.0;
    m->window_frames = 0;
    m->stall_worst_cur = m->wait_worst_cur = 0.0;
    m->stall_long_cur = m->late_cur = 0;
    for (u32 i = 0; i < BENCH_RECENT_WINDOWS; i++) {
        m->stall_worst_ring[i] = m->wait_worst_ring[i] = 0.0;
        m->stall_long_ring[i] = m->late_ring[i] = 0;
    }
    m->recent_i = 0;
    m->out_stall_worst_ms = m->out_wait_worst_ms = 0.0;
    m->out_stall_long = m->out_late = 0;
}

#endif /* UI_BENCH_METRICS_H */
