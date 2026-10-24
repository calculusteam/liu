/*
 * The session poll runs before a frame can paint, so parsing time delays the
 * next frame. A flat per-pane budget without an aggregate cap makes frame cost
 * grow with the number of active panes. This policy preserves the per-pane
 * budget when few panes are busy, then shares a fixed total across panes so
 * frame cost remains bounded. The focused pane is serviced first; deferred
 * panes retain their bytes and are revisited through rotating traversal, which
 * avoids starvation without re-arming a watch over unread data. The PTY queue
 * applies backpressure when output exceeds the rate a user can read.
 */
#ifndef CORE_POLL_BUDGET_H
#define CORE_POLL_BUDGET_H

#include "core/types.h"

/* A queued write can become writable without producing any read event.
 * Its retry timer must service I/O, not just wake an otherwise idle loop. */
static inline bool poll_needs_service(i32 fd_fires, bool event, bool saturated,
                                     bool backlog, bool pending_write) {
    return fd_fires > 0 || event || saturated || backlog || pending_write;
}

/* Deferred does not imply visible backlog: an empty foreground reader may
 * receive a zero grant because hidden panes spent the process allowance. Its
 * presence must not turn background-only work into an immediate-wake loop. */
static inline bool poll_backlog_is_visible(bool limited, i32 fed, i64 pending,
                                           bool reader, bool tab_active) {
    return limited && tab_active && (pending > 0 || (!reader && fed > 0));
}

/* Per pane, per frame, when little else is competing. */
#define POLL_BYTES_PER_PANE   (256 * 1024)

/* Ceiling on the sum across every pane of every tab in one frame. Chosen so
 * four saturating panes still run at the full per-pane rate — beyond that the
 * user is not reading them anyway — and the poll stays inside roughly half a
 * 60 Hz frame, leaving the rest for layout, render and present. */
#define POLL_BYTES_TOTAL      (1024 * 1024)

/* Desired minimum share. The ledger's shared byte/time limit takes precedence
 * and may defer a pane entirely; this floor does not reserve work. */
#define POLL_BYTES_MIN_PANE   (32 * 1024)

#define POLL_WORK_SECONDS       0.004
#define POLL_FOCUS_SECONDS      0.002
#define POLL_BACKGROUND_SECONDS 0.0005

/* `busy` is how many panes produced data on the PREVIOUS frame — the current
 * frame's count is not known until it has already been spent, and last frame's
 * is a good predictor of this one. `focused` marks the pane the user is looking
 * at. Returns the byte budget for that pane this frame. */
/* Which pane the user is actually in.
 *
 * It follows app_focused_session(), which reads `active_pane` ONLY while the
 * tab is split and otherwise returns pane 0, and that guard matters:
 * `active_pane` is a slot index that can outlive the split which gave it
 * meaning (it is also parked on tab->browser_pane when the embedded browser has
 * focus).
 *
 * `window_focused` is required too: the exemption exists because the user is
 * LOOKING at that pane, and a background window has no such pane. Without it
 * every open window would claim its own full POLL_BYTES_PER_PANE, which would
 * make POLL_BYTES_TOTAL a per-window cap rather than the per-process one it is.
 *
 * KNOWN DIVERGENCE from app_focused_session: that one also falls back to pane 0
 * when the split's active pane has no session (a pane that died before the
 * split collapsed). This predicate does not model that, so for those frames no
 * pane in the tab takes the exemption — transient, and it costs one frame of
 * shared budget rather than a wrong one.
 *
 * Pure and primitive-typed on purpose, so it can be checked without linking the
 * UI. */
static inline bool poll_pane_is_focused(bool window_focused, bool tab_is_active,
                                        bool tab_is_split, i32 active_pane,
                                        i32 pane) {
    if (!window_focused || !tab_is_active) return false;
    return pane == (tab_is_split ? active_pane : 0);
}

static inline i32 poll_budget_for_pane(i32 busy, bool focused) {
    if (focused || busy <= 1) return POLL_BYTES_PER_PANE;
    i32 share = POLL_BYTES_TOTAL / busy;
    if (share > POLL_BYTES_PER_PANE) share = POLL_BYTES_PER_PANE;
    if (share < POLL_BYTES_MIN_PANE) share = POLL_BYTES_MIN_PANE;
    return share;
}

/* ---------------------------------------------------------------------------
 * The process-wide ledger
 *
 * POLL_BYTES_TOTAL is the sum across every pane of every tab in one frame.
 * app_poll_sessions() runs once per window per iteration, so a busy count kept
 * per WINDOW would let every window take its own share and drain its own
 * megabyte: eight windows would make the cap 8 MB, and the frame cost the
 * whole policy exists to bound would be linear in window count.
 *
 * One instance is reset before any window is polled. It charges actual work,
 * not reserved intent, and clips every grant to the remaining process pool.
 * Returning zero requires the caller to hold the pane's watch and preserve a
 * backlog/service latch. The shared deadline has no per-pane floor exemption. */
typedef struct {
    u64 turn;        /* one process poll; rotates background service         */
    i32 rotation_windows; /* radix for per-window/tab/pane rotation           */
    i32 busy_prev;    /* panes that produced on the previous iteration        */
    i32 busy_now;     /* recount, becomes busy_prev at the end of the pass    */
    i64 spent;        /* bytes actually drained this iteration, all windows   */
    i64 backlog;      /* bytes still pending behind panes that hit a budget   */
    i32 panes;        /* panes offered a budget this iteration                */
    i32 floored;      /* grants clipped by the remaining shared byte pool     */
    i32 saturated;    /* panes that spent their whole budget                  */
    f64 work_seconds; /* measured read/parse work, shared across windows       */
    f64 seconds_per_byte; /* retained conservative cost estimate for chunks  */
    bool time_unlimited; /* Enables the alternate per-pane quota policy when configured. */
} PollLedger;

/* Once per main-loop iteration, before the first window is polled. */
static inline void poll_ledger_begin(PollLedger *l) {
    l->turn++;
    l->busy_prev = l->busy_now > 0 ? l->busy_now : 1;
    l->busy_now  = 0;
    l->spent     = 0;
    l->backlog   = 0;
    l->panes     = 0;
    l->floored   = 0;
    l->saturated = 0;
    l->work_seconds = 0;
}

/* The budget for one pane. `focused` is the pane the user is looking at, in the
 * focused window — at most one in the process. Its desired share is larger,
 * while the process pool/deadline still clips the actual grant. */
static inline i32 poll_ledger_take(PollLedger *l, bool focused) {
    l->panes++;
    if (!l->time_unlimited && l->work_seconds >= POLL_WORK_SECONDS) return 0;
    i32 want = poll_budget_for_pane(l->busy_prev, focused);
    i64 left = (i64)POLL_BYTES_TOTAL - l->spent;
    if ((i64)want > left) {
        want = left > 0 ? (i32)left : 0;
        l->floored++;
    }
    return want;
}

/* What that pane actually did. `pending` is what it still has buffered. */
static inline void poll_ledger_spend(PollLedger *l, i32 bytes,
                                     bool saturated, i64 pending) {
    if (bytes > 0) { l->spent += bytes; l->busy_now++; }
    if (saturated) l->saturated++;
    if (pending > 0) l->backlog += pending;
}

/* Byte quotas alone do not bound latency: escape-heavy output and CPU
 * contention change their cost by orders of magnitude. Yield BETWEEN reads,
 * retaining every byte in the reader/kernel queue. A parser call cannot be
 * preempted, so keep cold reads small and shrink subsequent reads using their
 * measured cost. No pane has a first-read exemption after the deadline. */
#define POLL_READ_CHUNK          1024
#define POLL_READ_COLD_CHUNK      256
#define POLL_READ_TIME_FLOOR       64

static inline i32 poll_ledger_read_size(const PollLedger *l, bool focused,
                                        i32 pane_bytes, f64 pane_seconds) {
    if (!l || l->time_unlimited) return POLL_READ_CHUNK;
    f64 left = POLL_WORK_SECONDS - l->work_seconds;
    f64 pane_left = (focused ? POLL_FOCUS_SECONDS : POLL_BACKGROUND_SECONDS) - pane_seconds;
    if (left <= 0 || pane_left <= 0 || l->spent >= POLL_BYTES_TOTAL) return 0;
    if (pane_left < left) left = pane_left;
    /* Half the remaining allowance leaves room for cost changes (for example
     * a chunk that switches from ordinary text to full-screen erase commands). */
    if (l->seconds_per_byte <= 0) return POLL_READ_COLD_CHUNK;
    f64 predicted = left * 0.5 / l->seconds_per_byte;
    if (predicted < POLL_READ_TIME_FLOOR) {
        /* A descheduled chunk can leave an enormous retained estimate. Each
         * pane may probe once while its actual shared/pane allowance remains.
         * Empty earlier reads consume time but provide no cost observation;
         * requiring zero prior work would strand every queued sibling behind
         * an idle focused pane. The guards above still forbid any probe after
         * either deadline, and a productive probe consumes this pane's chance. */
        return pane_bytes == 0 ? POLL_READ_TIME_FLOOR : 0;
    }
    return predicted < POLL_READ_CHUNK ? (i32)predicted : POLL_READ_CHUNK;
}
static inline void poll_ledger_work(PollLedger *l, f64 seconds) {
    if (l && seconds > 0) l->work_seconds += seconds;
}

/* Charge a successful read once, including parsing; retain the estimate
 * across turns so a known expensive stream does not get a cold-start burst
 * on every frame. Raise it immediately and relax it gradually. */
static inline void poll_ledger_observe(PollLedger *l, i32 bytes, f64 seconds) {
    poll_ledger_work(l, seconds);
    if (!l || bytes <= 0 || seconds <= 0) return;
    f64 cost = seconds / (f64)bytes;
    if (cost > l->seconds_per_byte) l->seconds_per_byte = cost;
    else l->seconds_per_byte = l->seconds_per_byte * 0.875 + cost * 0.125;
}

/* Visit the focused window/pane first, then each other slot exactly once. */
static inline i32 poll_priority_index(i32 step, i32 count, i32 priority) {
    if (priority < 0 || priority >= count) return step;
    return step == 0 ? priority : (step <= priority ? step - 1 : step);
}

/* Every member is visited once, but no background member owns first place
 * forever. The focused echo is drained separately before this traversal. */
static inline i32 poll_rotating_index(i32 step, i32 count, u64 turn) {
    if (count <= 0 || step < 0 || step >= count) return -1;
    return (i32)(((u64)step + turn % (u64)count) % (u64)count);
}

/* Advance a child's order after a full parent cycle. Reusing turn % count
 * at every level locks equal-sized windows/tabs/panes into the same phase. */
static inline u64 poll_rotation_child_turn(u64 turn, i32 parent_count) {
    return parent_count > 1 ? turn / (u64)parent_count : turn;
}

#endif /* CORE_POLL_BUDGET_H */
