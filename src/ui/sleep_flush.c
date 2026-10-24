#include "ui/sleep_flush.h"

#include <string.h>
#include <time.h>

/* Use `CLOCK_MONOTONIC` directly so this module does not depend on the
 * platform layer. */
f64 sleep_flush_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

/* Milliseconds left before `deadline`, clamped to [0, cap]. The barrier's own
 * waits are handed this rather than a constant, so a tab's budget is spent
 * across its panes instead of once per pane. */
static i32 ms_left(f64 deadline, i32 cap) {
    f64 ms = (deadline - sleep_flush_now()) * 1000.0;
    if (ms <= 0.0) return 0;
    return ms < (f64)cap ? (i32)ms : cap;
}

/* Copy whatever is available right now into the Terminal, in order, and report
 * whether the stream ended. Nothing is buffered: every chunk goes straight to
 * terminal_feed, so the memory this costs is one stack buffer no matter how
 * much output there is.
 *
 * The deadline is here and not only in the caller because "drain until it comes
 * back empty" is not by itself a bounded loop: a producer that is not actually
 * stopped — a job the shell left in its own process group, which
 * session_suspend() never signalled — can refill as fast as this empties, and
 * the main thread would sit here. Giving up mid-drain is safe: the bytes
 * already fed stay fed and the rest stay queued for the ordinary poll to
 * collect, so the only consequence is that the sleep aborts. */
static void drain_into(Session *s, Terminal *t, SleepFlushResult *r,
                       bool *over, f64 deadline) {
    u8 buf[16 * 1024];
    for (;;) {
        i32 n = session_read(s, buf, (i32)sizeof buf);
        if (n < 0) { if (over) *over = true; return; }
        if (n == 0) return;
        terminal_feed(t, buf, (usize)n);
        if (r) { r->bytes += (u32)n; r->chunks++; }
        if (sleep_flush_now() >= deadline) return;
    }
}

void sleep_flush_begin(SleepFlushState *state, Session *s) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->session = s;
    if (!s) { state->stage = SLEEP_FLUSH_STAGE_DONE; return; }
    state->result.suspended = session_suspend_begin(s);
    state->stage = state->result.suspended ? SLEEP_FLUSH_STAGE_STOP :
        session_type(s) == SESSION_LOCAL ? SLEEP_FLUSH_STAGE_FAILED :
                                         SLEEP_FLUSH_STAGE_UNSUSPENDED_DRAIN;
}

SleepFlushStatus sleep_flush_step(SleepFlushState *state, Session *s,
                                  Terminal *t, f64 work_deadline) {
    if (!state) return SLEEP_FLUSH_FAILED;
    if (s != state->session) state->stage = SLEEP_FLUSH_STAGE_FAILED;
    if (state->stage == SLEEP_FLUSH_STAGE_DONE) return SLEEP_FLUSH_DONE;
    if (state->stage == SLEEP_FLUSH_STAGE_FAILED) return SLEEP_FLUSH_FAILED;
    if (!s) { state->stage = SLEEP_FLUSH_STAGE_DONE; return SLEEP_FLUSH_DONE; }

    u8 buf[1024];
    while (sleep_flush_now() < work_deadline) {
        if (state->stage == SLEEP_FLUSH_STAGE_STOP) {
            if (!session_wait_suspended(s, 0)) return SLEEP_FLUSH_PENDING;
            state->result.stop_observed = true;
            if (!t) {
                state->result.barrier_met = true;
                state->stage = SLEEP_FLUSH_STAGE_DONE;
                return SLEEP_FLUSH_DONE;
            }
            state->token = session_quiesce_begin(s);
            state->stage = SLEEP_FLUSH_STAGE_BARRIER;
            continue;
        }
        if (!t) { state->stage = SLEEP_FLUSH_STAGE_DONE; return SLEEP_FLUSH_DONE; }
        i32 n = session_read(s, buf, (i32)sizeof buf);
        if (n > 0) {
            terminal_feed(t, buf, (usize)n);
            state->result.bytes += (u32)n;
            state->result.chunks++;
            continue;
        }
        if (state->stage == SLEEP_FLUSH_STAGE_BARRIER) {
            if (n < 0 || session_quiesce_wait(s, state->token, 0)) {
                state->result.barrier_met = true;
                state->stage = SLEEP_FLUSH_STAGE_FINAL_DRAIN;
                /* A producer may have published its last bytes between the
                 * empty read and the ack. Require another drain after proof,
                 * even if the work deadline defers it to the next turn. */
                continue;
            }
            return SLEEP_FLUSH_PENDING;
        }
        /* FINAL_DRAIN follows a stopped producer and acknowledged barrier.
         * UNSUSPENDED_DRAIN feeds buffered output before the caller destroys a
         * session that cannot be suspended. Both stages finish only after a real
         * empty or ended read, never when the slice expires. */
        state->stage = SLEEP_FLUSH_STAGE_DONE;
        return SLEEP_FLUSH_DONE;
    }
    return SLEEP_FLUSH_PENDING;
}

u32 sleep_flush_final_drain(Session *s, Terminal *t, f64 deadline) {
    if (!s || !t) return 0;
    SleepFlushResult r;
    memset(&r, 0, sizeof r);
    /* No barrier and no suspend here: the caller is about to destroy this
     * session, so "everything the fd will EVER hold" is what is in hand plus
     * whatever a dying child already wrote. drain_into stops the moment
     * session_read() reports empty or over, which for a dead child is the
     * complete remainder. */
    drain_into(s, t, &r, NULL, deadline);
    return r.bytes;
}

bool sleep_flush_pane(Session *s, Terminal *t, f64 deadline,
                      SleepFlushResult *out) {
    SleepFlushResult r;
    memset(&r, 0, sizeof r);
    if (out) *out = r;
    if (!s) return true;

    /* 1. Stop the producer. A refusal is not a failure of this function: the
     *    caller's fallback is to destroy the session outright, and there is
     *    then no producer left to be quiescent about. Feed what is already in
     *    hand first — the session is about to go, and those bytes are the tail
     *    of the last command's output. */
    r.suspended = session_suspend(s);
    if (!r.suspended) {
        if (t) drain_into(s, t, &r, NULL, deadline);
        if (out) *out = r;
        return true;
    }

    /* Wait until the process is observed stopped; `killpg()` only posts the
     * signal. Without confirmation, a child could write after the flush
     * concludes. */
    r.stop_observed = session_wait_suspended(s, ms_left(deadline, SLEEP_FLUSH_STOP_MS));
    if (!r.stop_observed) { if (out) *out = r; return false; }

    /* No Terminal to feed: an extra pane can hold a Session without one. Do
     * not drain — bytes left in the ring survive the sleep and reach whichever
     * Terminal wakes, which beats discarding them. */
    if (!t) { r.barrier_met = true; if (out) *out = r; return true; }

    /* 3 + 4 together, because they have to interleave. A full ring parks the
     *    gather thread where it cannot reach the empty read that acknowledges
     *    the barrier, so the wait is sliced and the drain runs between slices:
     *    draining is what frees the space that lets the thread finish. */
    u32 token = session_quiesce_begin(s);
    bool over = false;
    for (;;) {
        drain_into(s, t, &r, &over, deadline);
        if (over) { r.barrier_met = true; break; }   /* stream ended: nothing more exists */
        if (session_quiesce_wait(s, token, 0)) { r.barrier_met = true; break; }
        if (sleep_flush_now() >= deadline) break;
        session_quiesce_wait(s, token, SLEEP_FLUSH_SLICE_MS);
    }
    if (!r.barrier_met) { if (out) *out = r; return false; }

    /* The ack says the fd was empty at some point after the request; anything
     * published between the last drain and that ack is still in the ring. This
     * sweep gets a small grace past the deadline rather than reusing it: the
     * barrier is already met, so what remains is bounded by one ring, and
     * abandoning it on a deadline the barrier has just consumed would drop
     * exactly the bytes this function exists to save. */
    drain_into(s, t, &r, NULL, sleep_flush_now() + (f64)SLEEP_FLUSH_SWEEP_MS / 1000.0);

    if (out) *out = r;
    return true;
}
