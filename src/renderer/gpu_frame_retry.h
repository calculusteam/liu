/*
 * What to do when a frame's GPU work comes back with an error.
 *
 * A command buffer that completes in MTLCommandBufferStatusError put nothing
 * on the screen, so the last content change is invisible until some unrelated
 * redraw happens — and with the caret off and nothing animating, that redraw
 * need never come.
 *
 * Asking for another frame is the repair (the row caches are intact; app_render
 * replays them). The part that needs stating carefully is the BOUND: a device
 * that fails every submission would turn this into a frame loop at whatever rate
 * the queue rejects work, which is worse than the blank frame it is trying to
 * fix. So each consecutive failure backs the retry off further, and after
 * GPU_FRAME_RETRY_MAX consecutive failures the window stops asking.
 *
 * THE POLICY DOES NOT INFER ANYTHING FROM COUNTERS. It reads the run of
 * consecutive failures the RENDERER publishes. Two totals cannot stand in for
 * it, in either direction:
 *
 *   - "a frame was submitted and no new failure has been counted" is not
 *     recovery. Completions are asynchronous, so on a device failing every
 *     frame the loop can poll between the submit and the failure landing, see
 *     an unchanged failure count and forgive the streak — no bound at all.
 *
 *   - keying recovery on the SUCCESS total breaks the other side: when a
 *     success and a failure land in the same poll interval both totals move,
 *     and nothing says which came last.
 *
 * Two totals do not carry the ORDER of the completions inside a batch. The
 * completion handler does know it, so it keeps the run and
 * renderer_gpu_frame_counts() publishes it. Everything here is then a
 * comparison rather than a deduction.
 *
 * Split out of main.c's loop for the reason ui/sleep_policy.h and
 * ui/dpi_refresh.h are: it is a few comparisons that decide whether the app
 * spins, and a GPU error cannot be injected on demand without a long-running
 * kernel or a device reset, so the policy takes the values a failing device
 * produces, in the order it produces them, as plain arguments.
 */
#ifndef LIU_GPU_FRAME_RETRY_H
#define LIU_GPU_FRAME_RETRY_H

#include "core/types.h"

/* The most repaints a window will ask for while nothing is succeeding. A transient
 * failure (a device reset, a resource limit hit under memory pressure) clears in a
 * frame or two; more than this many in a row with no frame completing successfully
 * in between is a broken device, and asking again forever turns a blank window into
 * a spinning one. */
#define GPU_FRAME_RETRY_MAX 8

typedef enum {
    GPU_RETRY_NONE = 0,   /* nothing is owed */
    GPU_RETRY_AGAIN,      /* ask for another frame, after *out_delay seconds */
    GPU_RETRY_GAVE_UP,    /* the bound was passed just now — log once */
} GpuRetryAction;

typedef struct {
    u64  failed_seen;     /* this window's snapshot of the failure TOTAL */
    i32  streak;          /* the renderer's run, mirrored for the HUD and tests */
    bool gave_up;         /* GPU_RETRY_GAVE_UP already reported */
} GpuFrameRetry;

/* `failed_total` and `fail_run` both come from renderer_gpu_frame_counts(), read
 * under one lock so they describe the same moment.
 *
 * `failed_total` answers "has anything failed since the last call" — it is monotonic
 * and is the only thing that can tell a new failure from an old one. `fail_run`
 * answers "how many failures in a row end at the newest completion" — a success
 * resets it, so it is zero exactly when the most recently completed frame succeeded.
 *
 * `burst_dt` is the loop's short retry period; the delay grows with the run so a
 * queue refusing everything is not asked 250 times a second.
 *
 * MULTIPLE FRAMES IN FLIGHT need no special case. Whatever landed between two calls,
 * `fail_run` at call time is the true run and `failed_total` says whether any of it
 * failed. A batch of three failures is one observation of a run of three — one
 * repaint owed, backed off three periods. A batch ending in a success owes nothing
 * at all: the queue is serial, so the newest frame is the last to complete, and if
 * that one reached the screen the screen is current. */
static inline GpuRetryAction gpu_frame_retry_observe(GpuFrameRetry *g,
                                                     u64 failed_total,
                                                     u32 fail_run,
                                                     f64 burst_dt,
                                                     f64 *out_delay) {
    if (!g) return GPU_RETRY_NONE;
    /* Mirrored, not computed: the renderer's run IS the streak. */
    g->streak = (i32)fail_run;
    /* A success has completed since the last failure, so whatever went wrong is
     * over — arm the report again for a future episode. */
    if (fail_run == 0) g->gave_up = false;

    if (failed_total == g->failed_seen) return GPU_RETRY_NONE;
    g->failed_seen = failed_total;

    /* Failures landed, but a success completed AFTER them. On a serial queue that
     * success is the newest frame, so the screen is up to date and no repaint is
     * owed — this is the case the previous version turned into a streak. */
    if (fail_run == 0) return GPU_RETRY_NONE;

    if (fail_run > (u32)GPU_FRAME_RETRY_MAX) {
        if (g->gave_up) return GPU_RETRY_NONE;
        g->gave_up = true;
        return GPU_RETRY_GAVE_UP;
    }
    if (out_delay) *out_delay = burst_dt * (f64)fail_run;
    return GPU_RETRY_AGAIN;
}

#endif /* LIU_GPU_FRAME_RETRY_H */
