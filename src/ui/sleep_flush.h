/*
 * Before destroying a tab's `Terminal`, feed bytes still held by the PTY
 * reader ring or kernel queue into the live terminal, then snapshot it.
 * Draining and discarding these bytes would lose output produced before the
 * process stopped.
 *
 * Order:
 * 1. Request a stop with `session_suspend()`.
 * 2. Observe the stopped state with `session_wait_suspended()`.
 * 3. Complete the reader handoff with `session_quiesce_begin()` and
 *    `session_quiesce_wait()`.
 * 4. Feed the bytes with `terminal_feed()`.
 * 5. Snapshot and destroy the terminal in `ui.c`, which owns the tab
 *    lifecycle.
 *
 * A posted `killpg()` signal does not prove the process has stopped; a child
 * can finish a write already in the kernel. Likewise,
 * `session_read_pending() == 0` does not prove the reader is done: it may be
 * inside `read()`, have unpublished ring data, or be polling a non-empty
 * kernel queue. The quiesce handshake covers those states.
 *
 * All waits are bounded. On timeout, the caller resumes sessions it already
 * stopped, leaves the tab awake, and keeps queued bytes intact.
 *
 * For a local pane with a reader thread, the barrier guarantees that bytes
 * held by the PTY when it was requested reach the terminal. With
 * `LIU_PTY_THREAD=0`, `EAGAIN` means the queue is empty. Mosh uses a network
 * protocol, so an empty drain proves only that no bytes have arrived yet; SSH
 * is excluded by `ui/sleep_policy.h`.
 *
 * Suspend signals the shell's process group. Jobs in separate groups are not
 * stopped and are outside this flush guarantee.
 */
#ifndef UI_SLEEP_FLUSH_H
#define UI_SLEEP_FLUSH_H

#include "core/types.h"
#include "ssh/ssh_session.h"
#include "terminal/terminal.h"

/* Timeouts are absolute deadlines because flushing runs on the main thread. A
 * tab shares one deadline across its panes, bounded by `SLEEP_FLUSH_TAB_MS`;
 * the scan is capped by `SLEEP_FLUSH_SCAN_MS` and continues on a later tick.
 * Stop, reader barrier, and drain work are bounded, including at most one 256
 * KiB ring. If the deadline expires, keep the pane awake. */
#define SLEEP_FLUSH_STOP_MS      50   /* for one child to reach 'T' */
#define SLEEP_FLUSH_TAB_MS      250   /* one tab's whole flush, every pane */
#define SLEEP_FLUSH_SWEEP_MS     50   /* grace for the post-ack final sweep */
#define SLEEP_FLUSH_SCAN_MS     500   /* one idle-scan pass, every tab */
#define SLEEP_FLUSH_SLICE_MS      2   /* per wait, so a full ring keeps draining */

typedef struct {
    bool suspended;     /* the producer was stopped and the Session kept */
    /* True means the stop was confirmed. For a session without a local
     * child to inspect, including a Mosh pane, true means this step raised
     * no objection; it does not mean a process was observed in state `T`.
     */
    bool stop_observed;
    bool barrier_met;   /* the read path acknowledged the handover */
    u32  bytes;         /* handed to terminal_feed */
    u32  chunks;
} SleepFlushResult;

typedef enum {
    SLEEP_FLUSH_PENDING = 0,
    SLEEP_FLUSH_DONE,
    SLEEP_FLUSH_FAILED,
} SleepFlushStatus;

typedef enum {
    SLEEP_FLUSH_STAGE_STOP = 0,
    SLEEP_FLUSH_STAGE_BARRIER,
    SLEEP_FLUSH_STAGE_FINAL_DRAIN,
    SLEEP_FLUSH_STAGE_UNSUSPENDED_DRAIN,
    SLEEP_FLUSH_STAGE_DONE,
    SLEEP_FLUSH_STAGE_FAILED,
} SleepFlushStage;

typedef struct {
    Session *session;             /* borrowed, must survive until completion */
    SleepFlushResult result;
    u32 token;
    SleepFlushStage stage;
} SleepFlushState;

/* Live UI path. begin posts stop without waiting; step uses zero-time polls
 * and <=1024-byte feed chunks until one short work deadline. The caller owns
 * the tab's overall wall timeout and resumes every result.suspended Session
 * if any pane fails or times out. Final drain completion is carried across
 * turns; DONE never means "the slice expired with bytes still queued". A local
 * stop refusal is FAILED, so a job that appeared since the UI precheck is
 * preserved; the legacy remote/MOSH fallback may drain before destruction. */
void sleep_flush_begin(SleepFlushState *state, Session *s);
SleepFlushStatus sleep_flush_step(SleepFlushState *state, Session *s,
                                  Terminal *t, f64 work_deadline);

/* This module's clock — CLOCK_MONOTONIC — exported so a caller can build a
 * deadline on the same one rather than mixing it with platform_time_sec(). */
f64 sleep_flush_now(void);

/* Steps 1-4 for one pane, giving up at `deadline` (absolute, sleep_flush_now
 * scale). Returns false ONLY when quiescence could not be established in time;
 * the caller must then abort the sleep for the whole tab and resume every pane
 * it already suspended. A NULL session is a success with nothing done.
 *
 * `out` may be NULL. Main thread only. */
bool sleep_flush_pane(Session *s, Terminal *t, f64 deadline,
                      SleepFlushResult *out);

/* If a child exits while its tab sleeps, `session_resume()` fails and the
 * session is about to be destroyed. Its final output may remain in the reader
 * ring (up to 256 KiB) or kernel PTY queue, so restore the snapshot into a
 * replacement terminal, append these bytes, and only then destroy the session.
 *
 * The drain is bounded by `deadline` and returns the number of bytes fed. Both
 * arguments may be `NULL`. */
u32 sleep_flush_final_drain(Session *s, Terminal *t, f64 deadline);

/* Budget for that drain. A full 256 KiB ring feeds in about two milliseconds;
 * this is two orders of magnitude of headroom over the case it exists for. */
#define SLEEP_FLUSH_WAKE_MS     200

#endif /* UI_SLEEP_FLUSH_H */
