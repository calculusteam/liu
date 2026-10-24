/*
 * A gather thread drains each local PTY into a ring so the main loop does not
 * perform a blocking read for every kernel refill. The main loop still drains
 * the ring and parses VT data in terminal_feed, preserving the terminal's
 * single-writer invariant. This module collects I/O; it does not parse terminal
 * data.
 */
#ifndef CORE_PTY_READER_H
#define CORE_PTY_READER_H

#include "core/types.h"

typedef struct PtyReader PtyReader;
typedef void (*PtyReaderConsumeFn)(void *ctx, const u8 *data, usize len);

/* Install the process-wide hook that wakes the main loop. Set it once on the
 * main thread before the first pty_reader_start call. The hook is atomic, but
 * readers started before installation will not wake the loop. It may run on
 * any thread and must coalesce repeated wake requests. The application uses
 * platform_wake_event_loop. If the immediate hook is unset, the regular wake
 * hook is used. */
void pty_reader_set_wake(void (*wake)(void));
/* The hook for output that ends a silence: the first bytes after the pty was
 * quiet for PTY_WAKE_QUIET_NS. That is a program answering something (a
 * cursor-position request, a key's echo), and the loop may put the plain wake
 * off for up to a quantum to batch a flood (watch_post_wake_event_ex) — a
 * cost a burst's FIRST read should not pay. The app passes
 * platform_wake_event_loop_now; unset, the plain hook is used. */
void pty_reader_set_wake_now(void (*wake_now)(void));

/* Optional main-thread memory-ledger hook. Set before the first reader. Deltas
 * cover only Liu-owned heap bytes (reader object + rounded ring capacity), not
 * pthread stack VM reservation or kernel pipe storage. */
void pty_reader_set_memory_hook(void (*adjust)(isize delta));
usize pty_reader_owned_bytes(void);

/* Spawn a thread that drains `fd` into a ring of `ring_bytes`, rounded up to a
 * power of two; 0 means the default (LIU_PTY_RING_KB, 256 KiB). `fd` must
 * already be non-blocking; the reader never closes it and never writes to it.
 *
 * Returns NULL on ANY failure, including the live-reader cap. That is a
 * supported outcome, not an error: the caller keeps reading `fd` itself, which
 * is exactly today's behaviour, so this is a pure optimisation. */
PtyReader *pty_reader_start(int fd, u32 ring_bytes);

/* The reader's retained ring capacity in bytes, not the number of bytes
 * currently pending. Returns 0 for NULL. */
u32 pty_reader_ring_bytes(const PtyReader *r);

/* Is this reader's ring wired into memory (mlock)? It is by default: read() on
 * a pty master copies into its destination under the kernel's tty lock, so a
 * ring page that has to come back from the compressor or from swap makes the
 * PROGRAM in the pane wait — it cannot write to its terminal meanwhile (see
 * pty_reader.c). False when LIU_PTY_WIRE=0, when the readers' shared budget
 * (LIU_PTY_WIRE_MAX_MB, 64) is spent, or when the system refused; the thread
 * then reads through a buffer on its own stack, so the fault is still never
 * taken under that lock. The second call is the total wired on behalf of all
 * live readers. */
bool  pty_reader_ring_wired(const PtyReader *r);
/* The VM tag the rings are mapped under on macOS (an application-specific one:
 * VM_MEMORY_APPLICATION_SPECIFIC_1). core/vm_regions.c counts it as heap. */
#define PTY_READER_VM_TAG 240
usize pty_reader_wired_bytes(void);
/* Number of reads copied into this reader's stack buffer; zero when the ring
 * buffer is used for every read. */
u32   pty_reader_bounced_reads(const PtyReader *r);
/* How many quiet reads asked the terminal a question (pty_bytes_ask_reply), so
 * that the thread stayed up for the answer; *caught, if given, how many of
 * those answers it wrote before the wait ran out. */
u32   pty_reader_reply_waits(const PtyReader *r, u32 *caught);
/* Do these bytes, the first after a quiet spell, ask the terminal for an
 * answer (a cursor report, device attributes, a colour…)? Looked at only up to
 * PTY_REPLY_SCAN_MAX bytes: a question comes on its own, not inside a flood. */
#define PTY_REPLY_SCAN_MAX 512u
bool  pty_bytes_ask_reply(const u8 *b, usize n);

/* A chunk that is nothing but questions answerable from the cursor (a cursor
 * or status report, device attributes, the version: core/term_reply.h) is
 * answered by this thread itself when the main thread has parsed everything
 * before it and nothing is on its way to the program — the reply does not wait
 * for the loop. LIU_PTY_FAST_REPLY=0 turns it off.
 *
 * pty_reader_publish_reply   CONSUMER side, after it has parsed every byte it
 *                            has taken: `ok` (the parser is at rest) and the
 *                            1-based row and column a cursor report would say
 *                            (terminal_reply_position). Anything else, or an
 *                            out-of-range position, publishes "not here".
 * pty_reader_reply_hold      "not here" until the next publish. Called BEFORE
 *                            any input is handed over (a keystroke, a paste, a
 *                            resize): a reply never lands inside input the
 *                            program is receiving.
 * pty_reader_fast_replies    how many chunks this thread answered. */
void  pty_reader_publish_reply(PtyReader *r, bool ok, i32 row, i32 col);
void  pty_reader_reply_hold(PtyReader *r);
u32   pty_reader_fast_replies(const PtyReader *r);

/* Stop the thread and join it. MUST run before the owner closes `fd`: closing
 * an fd another thread is blocked in read()/poll() on is not a wake-up on
 * macOS, and the fd number can be recycled underneath it. NULL-safe. */
void pty_reader_stop(PtyReader *r);

/* Consumer side, MAIN THREAD ONLY. Copies up to `cap` bytes out of the ring.
 * Returns bytes copied, 0 when the ring is empty, and -1 when it is empty AND
 * the stream is over — the same tri-state session_read() already returns, so
 * every existing `if (n <= 0) break;` drain loop keeps working unchanged. */
i32 pty_reader_read(PtyReader *r, u8 *buf, i32 cap);

/* Bytes sitting unread in the ring. Consumer side. */
i32 pty_reader_pending(const PtyReader *r);

/* ---- quiescence barrier ------------------------------------------------
 *
 * "Is the ring empty?" is NOT "has the producer finished handing over".
 * pty_reader_pending() answers 0 for a reader that is blocked inside read()
 * about to return 8 KB, for one that has copied those bytes in but not yet
 * published `head`, and for one parked in poll() over a kernel tty queue that
 * still holds a screenful. All three are the normal state of an idle pane.
 * Tab sleep needs the stronger statement — everything the fd held at the
 * request is now in the ring — because after it stops the child it destroys
 * the Terminal, and whatever was still in flight is unrecoverable.
 *
 * The barrier is one generation counter each way. request() bumps `req` and
 * pokes the control pipe so a reader parked in poll() comes back; the reader
 * copies `req` into `ack` at the first read() that returns EAGAIN, which is
 * the exact moment the kernel queue is provably empty. Because the reader
 * snapshots `req` BEFORE that read() starts, an ack of generation N proves a
 * read that began after request N returned nothing — i.e. every byte the fd
 * held at request time has been published.
 *
 * MAIN THREAD ONLY, and the caller must keep draining while it waits: a full
 * ring parks the reader in ring_wait_space() where it cannot reach the EAGAIN
 * that would ack. pty_reader_quiesce_wait() below is the bounded wait, and it
 * is a wait, not the proof — the proof is the ack, and the timeout only
 * decides how long the main thread may be held. */
u32  pty_reader_quiesce_request(PtyReader *r);

/* Has generation `token` been acknowledged? Also true once the stream is over
 * (eof) or the reader has been asked to stop — in both cases no further byte
 * will ever be published, which is the same guarantee by a different route. */
bool pty_reader_quiesce_acked(const PtyReader *r, u32 token);

/* Block up to `timeout_ms` for `token`, returning as soon as it is acked.
 * Returns false only on timeout. Wakes early on the ack, so the common case
 * costs one context switch rather than a poll interval; call it in a loop that
 * drains the ring between waits. */
bool pty_reader_quiesce_wait(PtyReader *r, u32 token, i32 timeout_ms);

/* The same barrier expressed as one blocking call, for a caller that has
 * already stopped the producer and wants every byte handed to `consume`
 * before it destroys what was reading them. Requests a generation, drains the
 * ring while it waits (a full ring parks the reader where it can never reach
 * the EAGAIN that acks), and drains once more after the ack, because the
 * reader publishes what it held between the last drain and that ack.
 *
 * MAIN THREAD ONLY. Returns false only if the ack does not arrive inside its
 * own bounded budget — the producer must already be unable to write more;
 * this is a finite quiescence barrier, not a way to chase a live stream. */
bool pty_reader_drain_barrier(PtyReader *r, PtyReaderConsumeFn consume,
                              void *ctx);

/* Live readers in this process. The cap (LIU_PTY_MAX_READERS, default 128)
 * exists because the true ceiling is LIU_MAX_WINDOWS(8) x MAX_TABS(32) x
 * MAX_SPLIT_PANES(8) = 2048 threads and 512 MB of rings. */
/* ---- the three things the main thread does not ask the pty itself --------
 *
 * write(), tcgetpgrp() and TIOCSWINSZ on a pty master take the kernel's tty
 * lock, which the program on the other side may hold across a page fault. The
 * reader thread makes those calls; these only leave a request with it and
 * never touch the fd.
 *
 * pty_reader_write        queue bytes for the pty. Returns how many were taken
 *                         (the queue is bounded: a short count means "keep the
 *                         rest and offer it again" — the loop is woken when
 *                         there is room), or -1 once the other side is gone.
 * pty_reader_write_pending bytes taken and not yet written to the pty.
 * pty_reader_fg_pgid      the foreground process group as last sampled: > 0 a
 *                         group, -1 none, 0 not sampled yet. Sampled whenever
 *                         the pty goes quiet and on request; the loop is woken
 *                         when the answer changes.
 * pty_reader_fg_refresh   ask for a fresh sample (the answer arrives later).
 * pty_reader_set_winsize  the size the pty should have; the last call wins. */
isize pty_reader_write(PtyReader *r, const u8 *data, usize len);
u32   pty_reader_write_pending(const PtyReader *r);
i32   pty_reader_fg_pgid(const PtyReader *r);
void  pty_reader_fg_refresh(PtyReader *r);
/* Is the program on the pty reading a password — echo off, line mode? Sampled
 * with the foreground group (the same tty lock, the same moments), and the
 * loop is woken when it changes. */
bool  pty_reader_password_mode(const PtyReader *r);
/* A result is fresh only after a tcgetpgrp() begun after this request. A
 * cached foreground group cannot satisfy it. Main-thread nonblocking API;
 * poll result on subsequent turns, continuing to drain output. */
u32   pty_reader_fg_request(PtyReader *r);
bool  pty_reader_fg_result(const PtyReader *r, u32 token, i32 *pgid);
void  pty_reader_set_winsize(PtyReader *r, u16 cols, u16 rows);
/* The cell in pixels, reported with the NEXT window size (ws_xpixel / ws_ypixel). */
void  pty_reader_set_cell_pixels(PtyReader *r, u16 w, u16 h);

i32 pty_reader_live_count(void);

/* Process-global latch: "some drain stopped on its byte budget with bytes
 * still sitting in a ring". The main loop clears it before the poll pass and
 * refuses to sleep on the next turn while it is set. Without it a
 * budget-capped drain parks the loop with a full ring and a producer blocked
 * on ring-full — which publishes nothing more, so nothing wakes the loop and
 * the pane stalls until the idle ladder's next tick (8 ms while the
 * visible-data flag is still hot, 80 ms for a background talker, and the
 * 0.5 s blink edge once neither holds). */
void pty_reader_note_backlog(void);
bool pty_reader_backlog(void);
void pty_reader_backlog_clear(void);

#endif /* CORE_PTY_READER_H */
