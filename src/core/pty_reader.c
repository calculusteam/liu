#include "core/pty_reader.h"
#include "core/mem_budget.h"
#include "core/term_reply.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>

#if defined(PLATFORM_MACOS) || defined(__APPLE__)
#include <pthread/qos.h>
#endif

struct PtyReader {
    int             fd;        /* borrowed; never closed here */
    int             ctl[2];    /* ctl[0] polled by the thread; stop() writes ctl[1] */
    pthread_t       th;
    bool            th_live;
    u8             *buf;
    u32             cap;       /* power of two */
    /* Free-running counters, never masked: `head - tail` stays correct across
     * wraparound for any cap <= 2^31, and the index is `counter & (cap - 1)`. */
    _Atomic u32     head;      /* producer writes */
    _Atomic u32     tail;      /* consumer writes */
    _Atomic u32     barrier_req;
    _Atomic u32     barrier_ack;
    _Atomic bool    eof, quit, waiting;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    /* Quiescence barrier, one generation counter each way. The reader copies
     * `q_req` into `q_ack` at the first read() that returns EAGAIN, having
     * snapshotted it BEFORE that read started — see pty_reader.h for why that
     * ordering is the whole proof. Its own mutex/condvar rather than mu/cv:
     * those carry the producer-waits/consumer-signals Dekker pairing and this
     * handshake runs the other way round. */
    _Atomic u32     q_req;     /* main thread bumps */
    _Atomic u32     q_ack;     /* reader thread publishes */
    pthread_mutex_t qmu;
    pthread_cond_t  qcv;
    /* Resolved once by pty_reader_start on the MAIN thread. The cached-getenv
     * shape used elsewhere in the tree (liu_pace_enabled and friends) is
     * main-thread-only by construction; calling it from N reader threads is a
     * real data race on the cache, which is exactly what TSan reported. */
    i32             spin_cap;
    u64             reply_wait_ns; /* PTY_REPLY_WAIT_NS, or what LIU_PTY_REPLY_WAIT_US says */
    bool            fast_reply;    /* LIU_PTY_FAST_REPLY (default on): answer a chunk of questions here */
    /* What the main thread has parsed, and where a cursor report would put
     * the cursor after it (pty_reader_publish_reply): the consumer's `tail`
     * in the high half, then bit 31 "may be answered here", the row in bits
     * 16-30 and the column in 0-15, both 1-based. 0 = nothing may be. */
    _Atomic u64     reply_snap;
    _Atomic u32     fast_replies;  /* chunks of questions answered by this thread */

    /* PTY master operations such as write(), tcgetpgrp(), and TIOCSWINSZ can
     * wait for the terminal lock while the child process handles a page fault.
     * Running them on the main thread could stall the UI, so this reader thread
     * performs them while the main thread updates request fields.
     *
     * omu protects the outgoing queue during memory copies. Do not hold it
     * across a system call, which could block the thread that queues the next
     * byte. */
    pthread_mutex_t omu;
    u8             *obuf;
    u32             ocap, ooff, olen;
    _Atomic u32     out_pending;   /* accepted and not yet written (queue + the thread's carry) */
    _Atomic bool    out_refused;   /* a write was turned away: wake the loop when there is room */
    _Atomic bool    out_error;     /* write() failed hard — the other side is gone */
    _Atomic i32     fg_pgid;       /* last tcgetpgrp(): 0 never sampled, -1 no answer */
    _Atomic i32     pw_mode;       /* last tcgetattr(): 0 never sampled, 1 echo off in line
                                    * mode (a password is being read), -1 anything else */
    _Atomic bool    fg_want;       /* a fresher sample was asked for */
    _Atomic u32     fg_req;        /* fresh foreground probe generation */
    _Atomic u32     fg_ack;        /* ioctl begun after this generation */
    _Atomic u64     winsz;         /* (seq << 32) | rows << 16 | cols; seq 0 = never set */
    u32             winsz_applied; /* reader thread only */
    _Atomic u32     cellpx;        /* the cell in pixels, width << 16 | height; 0 = not said */
    /* A request is waiting. Its own flag because the reader has TWO places to
     * park — poll() and the ring-full condition — and the ctl pipe only breaks
     * the first. */
    _Atomic bool    attn;

    /* Reading from the PTY master copies bytes into its destination while
     * holding the terminal lock. If a destination ring page is not resident,
     * the fault can delay writes from the child process. Wire the 256 KiB ring
     * when the memory budget permits. If wiring is unavailable, read into a
     * thread-local stack buffer and copy into the ring afterward so ring-page
     * faults occur outside the terminal lock. */
    usize           map_len;
    bool            wired;
    _Atomic u32     bounced;       /* reads taken through the stack buffer */
    _Atomic u32     reply_waits;   /* quiet reads that asked the terminal something */
    _Atomic u32     reply_caught;  /* ... whose answer went out while the thread was still up */
};

/* What a reader thread reads into when its ring is not wired. */
#ifndef PTY_READ_BOUNCE
#define PTY_READ_BOUNCE    4096u
#endif

/* The most the outgoing queue holds. Session keeps the real queue (megabytes,
 * for a paste); this is one hop of it, sized so a paste moves in few hand-offs
 * and a stalled child cannot make every pane hold much. */
#define PTY_OUT_CAP        ((u32)64 * 1024)
#define PTY_OUT_LOW_WATER  ((u32)8 * 1024)
#define PTY_OUT_CARRY      2048u

/* Set the wake callback on the main thread before the first reader starts;
 * reader threads also access it, so it uses atomic storage. */
static _Atomic(void (*)(void)) g_wake;
static _Atomic(void (*)(void)) g_wake_now;
/* How long the pty must have been quiet for its next bytes to wake the loop at
 * once (pty_reader_set_wake_now). The loop's own batching quantum is 1 ms
 * (LIU_WAKE_QUANTUM_US): inside a flood the reads come closer than that, so
 * they keep their batching; a program answering after a pause does not wait
 * for it. */
#define PTY_WAKE_QUIET_NS ((u64)1000000)
/* How long the thread stays up, after a quiet read that asked the terminal a
 * question (pty_bytes_ask_reply), for the answer the main thread is about to
 * queue — instead of parking in poll() and being woken for it through the
 * control pipe, the slower hand-off. An idle main thread answers well inside
 * this bound (the drain runs from inside its wait,
 * platform_set_wake_now_handler); a busy one gives up the wait at the bound
 * and the answer takes the pipe. At most this much of one core per question,
 * and only for questions. LIU_PTY_REPLY_WAIT_US sets it; 0 parks at once. */
#define PTY_REPLY_WAIT_NS ((u64)250000)
static _Atomic(void (*)(isize)) g_memory_adjust;

static _Atomic bool g_backlog;
static _Atomic i32  g_live;
static _Atomic usize g_owned_bytes;
static _Atomic usize g_wired_bytes;

void pty_reader_set_wake(void (*wake)(void)) {
    atomic_store_explicit(&g_wake, wake, memory_order_release);
}
void pty_reader_set_wake_now(void (*wake_now)(void)) {
    atomic_store_explicit(&g_wake_now, wake_now, memory_order_release);
}
void pty_reader_set_memory_hook(void (*adjust)(isize)) {
    atomic_store_explicit(&g_memory_adjust, adjust, memory_order_release);
}
usize pty_reader_owned_bytes(void) {
    return atomic_load_explicit(&g_owned_bytes, memory_order_acquire);
}

/* Account each reader's retained ring capacity and reader object in this
 * module's g_owned_bytes total and the process memory ledger. Do not route the
 * same delta through the memory hook as well, or the ledger counts it twice.
 *
 * Use the transient counter because teardown can run on the tab-trash worker
 * or the main thread. The per-thread stack mapped by pthread_create is not
 * included. */
static void reader_account(isize delta) {
    if (delta >= 0) atomic_fetch_add_explicit(&g_owned_bytes, (usize)delta,
                                              memory_order_acq_rel);
    else            atomic_fetch_sub_explicit(&g_owned_bytes, (usize)(-delta),
                                              memory_order_acq_rel);
    mem_budget_transient_adjust(MEM_PTY_IO, delta);
}
void pty_reader_note_backlog(void)  { atomic_store_explicit(&g_backlog, true,  memory_order_release); }
bool pty_reader_backlog(void)       { return atomic_load_explicit(&g_backlog, memory_order_acquire); }
void pty_reader_backlog_clear(void) { atomic_store_explicit(&g_backlog, false, memory_order_release); }
i32  pty_reader_live_count(void)    { return atomic_load_explicit(&g_live, memory_order_acquire); }

static void wake_loop(void) {
    void (*w)(void) = atomic_load_explicit(&g_wake, memory_order_acquire);
    if (w) w();
}

static void wake_loop_now(void) {
    void (*w)(void) = atomic_load_explicit(&g_wake_now, memory_order_acquire);
    if (!w) w = atomic_load_explicit(&g_wake, memory_order_acquire);
    if (w) w();
}

static u64 reader_clock_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static void barrier_notify(PtyReader *r) {
    pthread_mutex_lock(&r->mu);
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
}

/* ---- tunables, each read from the environment exactly once ---------------
 * Same `static i32 v = -1` shape as liu_pace_enabled() in main.c. */
static u32 ring_default_bytes(void) {
    static i32 kb = -1;
    if (kb < 0) {
        const char *e = getenv("LIU_PTY_RING_KB");
        i32 v = e ? atoi(e) : 0;
        /* 256 KiB matches POLL_BYTES_PER_PANE exactly, so ONE focused-pane
         * drain can empty a full ring — which is what keeps the backlog latch
         * from becoming the steady state. */
        kb = (v >= 4 && v <= 4096) ? v : 256;
    }
    return (u32)kb * 1024u;
}
/* How much ring memory all readers together may wire, in bytes; 0 turns the
 * wiring off (LIU_PTY_WIRE=0, the A/B switch — the bounce buffer then stands
 * in). 64 MiB by default: 256 panes at the default ring. */
static usize ring_wire_budget(void) {
    static i64 v = -1;
    if (v < 0) {
        const char *off = getenv("LIU_PTY_WIRE");
        const char *mb  = getenv("LIU_PTY_WIRE_MAX_MB");
        i32 n = mb ? atoi(mb) : 0;
        if (off && off[0] == '0') v = 0;
        else v = (i64)((n >= 1 && n <= 4096) ? n : 64) * 1024 * 1024;
    }
    return (usize)v;
}

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
/* On macOS an anonymous mapping can carry a VM tag in place of the fd. Without
 * one the rings would sit in the footprint audit's "untagged" bucket with the
 * binary's __bss and guard pages; core/vm_regions.c files this tag under the
 * heap, where a malloc'd ring would be, and vmmap shows it by number. */
#if defined(__APPLE__)
#include <mach/vm_inherit.h>
#include <mach/vm_statistics.h>
/* VM_MAKE_TAG is `tag << 24` on an int, which for a tag of 240 shifts into the
 * sign bit (undefined, and UBSan says so); the same bits, unsigned first. */
#define RING_MAP_FD ((int)((u32)PTY_READER_VM_TAG << 24))
#else
#define RING_MAP_FD (-1)
#endif

/* The ring: a mapping of its own, so wiring it wires nothing else and
 * unmapping it gives the pages straight back. Main thread (start). */
static u8 *ring_map(u32 cap, usize *map_len, bool *wired) {
    long ps = sysconf(_SC_PAGESIZE);
    usize page = (ps > 0 && ps <= (1 << 20)) ? (usize)ps : 16384;
    usize len = ((usize)cap + page - 1) & ~(page - 1);
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, RING_MAP_FD, 0);
    if (p == MAP_FAILED) return NULL;
    *map_len = len;
    *wired = false;
    /* A child gets none of it. fork() cannot share a wired page copy-on-write,
     * so it COPIES every wired ring before it returns, a cost that grows with
     * the number of rings — and Liu forks for every tab, every git sample,
     * every helper. The child only ever execs; it has no use for another
     * pane's output either. */
#if defined(__APPLE__)
    (void)minherit(p, len, VM_INHERIT_NONE);
#elif defined(MADV_DONTFORK)
    (void)madvise(p, len, MADV_DONTFORK);
#endif
    usize budget = ring_wire_budget();
    if (budget && atomic_load_explicit(&g_wired_bytes, memory_order_acquire) + len <= budget &&
        mlock(p, len) == 0) {
        atomic_fetch_add_explicit(&g_wired_bytes, len, memory_order_acq_rel);
        *wired = true;
    }
    return p;
}

static void ring_unmap(PtyReader *r) {
    if (!r->buf) return;
    if (r->wired) atomic_fetch_sub_explicit(&g_wired_bytes, r->map_len, memory_order_acq_rel);
    munmap(r->buf, r->map_len);        /* unwires what it covers */
    r->buf = NULL;
}

static i32 spin_max(void) {
    static i32 v = -1;
    if (v < 0) {
        const char *e = getenv("LIU_PTY_SPIN");
        i32 n = e ? atoi(e) : -1;
        v = (n >= 0 && n <= 4096) ? n : 16;
    }
    return v;
}
/* LIU_PTY_FAST_REPLY=0 sends every question to the parser again. */
static bool fast_reply_enabled(void) {
    static i32 v = -1;
    if (v < 0) {
        const char *e = getenv("LIU_PTY_FAST_REPLY");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v != 0;
}
static u64 reply_wait_max(void) {
    static i64 v = -1;
    if (v < 0) {
        const char *e = getenv("LIU_PTY_REPLY_WAIT_US");
        long us = e ? atol(e) : -1;
        v = (us >= 0 && us <= 10000) ? (i64)us * 1000 : (i64)PTY_REPLY_WAIT_NS;
    }
    return (u64)v;
}
static i32 max_live(void) {
    static i32 v = -1;
    if (v < 0) {
        const char *e = getenv("LIU_PTY_MAX_READERS");
        i32 n = e ? atoi(e) : 0;
        v = (n > 0 && n <= 4096) ? n : 128;
    }
    return v;
}

/* Both loads are seq_cst because these participate in the Dekker pairing with
 * `waiting` below. Three of the four accesses being seq_cst is NOT a Dekker
 * pair, and the failure mode is a reader parked in the condvar forever with a
 * consumer that will never signal: one pane goes silent, no crash, no log. */
static inline u32 ring_used(const PtyReader *r) {
    u32 h = atomic_load_explicit(&r->head, memory_order_seq_cst);
    u32 t = atomic_load_explicit(&r->tail, memory_order_seq_cst);
    return h - t;
}
static inline u32 ring_free(const PtyReader *r) { return r->cap - ring_used(r); }

i32 pty_reader_read(PtyReader *r, u8 *out, i32 cap_out) {
    if (!r || !out || cap_out <= 0) return -1;
    u32 head = atomic_load_explicit(&r->head, memory_order_acquire);
    u32 tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    u32 used = head - tail;
    if (used == 0)
        return atomic_load_explicit(&r->eof, memory_order_acquire) ? -1 : 0;
    u32 n = used < (u32)cap_out ? used : (u32)cap_out;
    u32 off = tail & (r->cap - 1);
    u32 first = r->cap - off; if (first > n) first = n;
    memcpy(out, r->buf + off, first);
    if (n > first) memcpy(out + first, r->buf, n - first);
    /* seq_cst, not release: it has to order against the producer's seq_cst
     * store of `waiting` (Dekker). */
    atomic_store_explicit(&r->tail, tail + n, memory_order_seq_cst);
    if (atomic_load_explicit(&r->waiting, memory_order_seq_cst)) {
        pthread_mutex_lock(&r->mu);
        pthread_cond_signal(&r->cv);
        pthread_mutex_unlock(&r->mu);
    }
    return (i32)n;
}

i32 pty_reader_pending(const PtyReader *r) {
    if (!r) return 0;
    return (i32)ring_used(r);
}

u32 pty_reader_ring_bytes(const PtyReader *r) { return r ? r->cap : 0u; }
bool pty_reader_ring_wired(const PtyReader *r) { return r && r->wired; }
u32 pty_reader_bounced_reads(const PtyReader *r) {
    return r ? atomic_load_explicit(&((PtyReader *)r)->bounced, memory_order_relaxed) : 0u;
}
void pty_reader_publish_reply(PtyReader *r, bool ok, i32 row, i32 col) {
    if (!r) return;
    u64 v = 0;
    if (ok && row >= 1 && row <= 0x7fff && col >= 1 && col <= 0xffff) {
        /* `tail` is the consumer's own counter, and this is the consumer. */
        u32 tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
        v = ((u64)tail << 32) | 0x80000000ull | ((u64)row << 16) | (u64)col;
    }
    atomic_store_explicit(&r->reply_snap, v, memory_order_release);
}
void pty_reader_reply_hold(PtyReader *r) {
    if (r) atomic_store_explicit(&r->reply_snap, 0, memory_order_seq_cst);
}
u32 pty_reader_fast_replies(const PtyReader *r) {
    return r ? atomic_load_explicit(&((PtyReader *)r)->fast_replies, memory_order_relaxed) : 0u;
}
u32 pty_reader_reply_waits(const PtyReader *r, u32 *caught) {
    PtyReader *w = (PtyReader *)r;
    if (caught) *caught = w ? atomic_load_explicit(&w->reply_caught, memory_order_relaxed) : 0u;
    return w ? atomic_load_explicit(&w->reply_waits, memory_order_relaxed) : 0u;
}

/* The questions a program asks its terminal that Liu answers (vt_parser.c's
 * emit_response callers): a status or cursor report (CSI … n), device
 * attributes (CSI c, CSI > c, CSI = c), the version (CSI > q), a mode
 * (CSI … $ p), the keyboard flags (CSI ? u), the window or cell size
 * (CSI 14 / 16 / 18 t), a colour (OSC 10 / 11 / 12 ; ?) and a capability
 * (DCS + q). Only the start of a question has to be in these bytes: a program
 * writes one in a single write(), and one that is cut only misses the wait. A
 * false yes costs PTY_REPLY_WAIT_NS of this thread; a pushed title
 * (CSI 22 ; 0 t), a colour or a cursor move is not a question. */
bool pty_bytes_ask_reply(const u8 *b, usize n) {
    if (!b || n < 3 || n > PTY_REPLY_SCAN_MAX) return false;
    for (usize i = 0; i + 2 < n; i++) {
        if (b[i] != 0x1b) continue;
        if (b[i + 1] == '[') {
            usize j = i + 2;
            u8 lead = (b[j] >= '<' && b[j] <= '?') ? b[j++] : 0;   /* < = > ? */
            u32 first = 0;
            bool first_done = false;
            while (j < n && ((b[j] >= '0' && b[j] <= '9') || b[j] == ';' || b[j] == ':')) {
                if (b[j] == ';' || b[j] == ':') first_done = true;
                else if (!first_done && first < 10000) first = first * 10 + (u32)(b[j] - '0');
                j++;
            }
            u8 inter = (j < n && b[j] >= 0x20 && b[j] <= 0x2f) ? b[j++] : 0;
            if (j >= n) continue;
            u8 f = b[j];
            if (inter) { if (inter == '$' && f == 'p') return true; continue; }
            if (f == 'n' && lead != '>') return true;
            if (f == 'c' && (lead == 0 || lead == '>' || lead == '=')) return true;
            if (f == 'q' && lead == '>') return true;
            if (f == 'u' && lead == '?') return true;
            if (f == 't' && lead == 0 && (first == 14 || first == 16 || first == 18)) return true;
        } else if (b[i + 1] == ']') {
            if (i + 5 < n && b[i + 2] == '1' && b[i + 3] >= '0' && b[i + 3] <= '2' &&
                b[i + 4] == ';' && b[i + 5] == '?') return true;
        } else if (b[i + 1] == 'P') {
            if (i + 3 < n && b[i + 2] == '+' && b[i + 3] == 'q') return true;
        }
    }
    return false;
}
usize pty_reader_wired_bytes(void) { return atomic_load_explicit(&g_wired_bytes, memory_order_acquire); }

/* ---- quiescence barrier ------------------------------------------------ */

/* Reader side. `gen` was loaded before the read() that just returned EAGAIN,
 * so publishing it here says: a read begun after generation `gen` was
 * requested found the kernel queue empty. The store is under qmu so a waiter
 * that has already evaluated the predicate and is about to wait cannot miss
 * the broadcast. The early-out keeps the idle path lock-free — an interactive
 * pane reaches this line on every poll cycle. */
static void quiesce_publish(PtyReader *r, u32 gen) {
    if (atomic_load_explicit(&r->q_ack, memory_order_relaxed) == gen) return;
    pthread_mutex_lock(&r->qmu);
    atomic_store_explicit(&r->q_ack, gen, memory_order_release);
    pthread_cond_broadcast(&r->qcv);
    pthread_mutex_unlock(&r->qmu);
}

/* Wake any waiter for a reason other than an ack — end of stream, or a stop.
 * pty_reader_quiesce_acked() reports both as satisfied, so this only has to
 * make sure the waiter gets to look. */
static void quiesce_wake_all(PtyReader *r) {
    pthread_mutex_lock(&r->qmu);
    pthread_cond_broadcast(&r->qcv);
    pthread_mutex_unlock(&r->qmu);
}

u32 pty_reader_quiesce_request(PtyReader *r) {
    if (!r) return 0;
    u32 gen = atomic_fetch_add_explicit(&r->q_req, 1u, memory_order_acq_rel) + 1u;
    /* Poke the control pipe. Without it a reader parked in poll(-1) over an
     * idle fd would never come back to the read() that acks — the fd is quiet,
     * which is exactly the case a quiesce is asked about. The pipe is
     * non-blocking: if the write fails the pipe is already holding an unread
     * poke, which breaks the same poll. */
    if (r->ctl[1] >= 0) { char w = 'w'; ssize_t n = write(r->ctl[1], &w, 1); (void)n; }
    return gen;
}

bool pty_reader_quiesce_acked(const PtyReader *r, u32 token) {
    if (!r) return true;
    /* Both mean the same thing for the caller: no further byte will ever be
     * published, so there is nothing left to wait for. */
    if (atomic_load_explicit(&r->eof,  memory_order_acquire)) return true;
    if (atomic_load_explicit(&r->quit, memory_order_acquire)) return true;
    u32 ack = atomic_load_explicit(&r->q_ack, memory_order_acquire);
    /* Signed difference, so a generation counter that wraps past UINT32_MAX
     * keeps comparing correctly. */
    return (i32)(ack - token) >= 0;
}

bool pty_reader_quiesce_wait(PtyReader *r, u32 token, i32 timeout_ms) {
    if (!r) return true;
    if (pty_reader_quiesce_acked(r, token)) return true;
    if (timeout_ms <= 0) return false;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }

    bool ok;
    pthread_mutex_lock(&r->qmu);
    for (;;) {
        ok = pty_reader_quiesce_acked(r, token);
        if (ok) break;
        /* ANY non-zero return ends the wait, not only ETIMEDOUT. EINVAL and
         * EPERM are not spurious wakeups, and retrying them is an unbounded
         * loop that holds the very mutex the reader needs in order to publish
         * the ack — the caller's bounded wait would become a hang. */
        if (pthread_cond_timedwait(&r->qcv, &r->qmu, &ts) != 0) {
            ok = pty_reader_quiesce_acked(r, token);
            break;
        }
    }
    pthread_mutex_unlock(&r->qmu);
    return ok;
}

bool pty_reader_drain_barrier(PtyReader *r, PtyReaderConsumeFn consume,
                              void *ctx) {
    if (!r || !consume) return false;

    u32 token = pty_reader_quiesce_request(r);
    u8  buf[16384];
    const i32 slice_ms = 25, budget_ms = 2000;

    for (i32 waited = 0; ; waited += slice_ms) {
        /* Drain BEFORE testing the ack: a full ring parks the reader in
         * ring_wait_space(), where it never reaches the read() that acks. */
        for (;;) {
            i32 n = pty_reader_read(r, buf, (i32)sizeof buf);
            if (n <= 0) break;
            consume(ctx, buf, (usize)n);
        }
        if (pty_reader_quiesce_acked(r, token)) break;
        if (waited >= budget_ms) return false;
        pty_reader_quiesce_wait(r, token, slice_ms);
    }

    /* The ack proves the fd was empty when the reader last looked; whatever it
     * published between the drain above and that ack is still in the ring. */
    for (;;) {
        i32 n = pty_reader_read(r, buf, (i32)sizeof buf);
        if (n <= 0) break;
        consume(ctx, buf, (usize)n);
    }
    return true;
}

/* Backpressure — the ring's `slot_free`. When the ring is full the reader stops
 * reading, the kernel tty queue fills, and the child blocks in write(): the
 * producer is throttled by the consumer, which is the property that keeps a
 * `yes` from turning into unbounded memory. */
static void ring_wait_space(PtyReader *r) {
    pthread_mutex_lock(&r->mu);
    atomic_store_explicit(&r->waiting, true, memory_order_seq_cst);
#ifndef LIU_PTY_NO_TIMEDWAIT
    i32 wait_ms = 100;
#endif
    while (!atomic_load_explicit(&r->quit, memory_order_acquire) &&
           ring_free(r) == 0 &&
           /* seq_cst, like `waiting` above: the same store-then-check pairing,
            * so a request posted while this thread is on its way to sleep is
            * either seen here or finds `waiting` set and signals. */
           !atomic_load_explicit(&r->attn, memory_order_seq_cst)) {
#ifdef LIU_PTY_NO_TIMEDWAIT
        /* Schedule control for exercising the producer/consumer handshake
         * around its Dekker pairing. */
        pthread_cond_wait(&r->cv, &r->mu);
#else
        /* The consumer and stop paths signal this condition immediately.
         * Timeouts only guard against a lost signal: back off while the ring
         * stays full, instead of waking every parked reader ten times/second.
         * Any progress leaves this loop, so the next saturation starts at
         * 100 ms again. The safety-net timeout remains bounded at one second. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += wait_ms / 1000;
        ts.tv_nsec += (wait_ms % 1000) * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
        if (pthread_cond_timedwait(&r->cv, &r->mu, &ts) == ETIMEDOUT)
            wait_ms = wait_ms < 500 ? wait_ms * 2 : 1000;
#endif
    }
    atomic_store_explicit(&r->waiting, false, memory_order_relaxed);
    pthread_mutex_unlock(&r->mu);
}

/* ---- requests served on the reader thread ---------------------------------- */

static void reader_poke(PtyReader *r) {
    atomic_store_explicit(&r->attn, true, memory_order_seq_cst);
    /* Non-blocking pipe: a failed write means a poke is already sitting in it. */
    if (r->ctl[1] >= 0) { char c = 'p'; ssize_t n = write(r->ctl[1], &c, 1); (void)n; }
    if (atomic_load_explicit(&r->waiting, memory_order_seq_cst)) {
        pthread_mutex_lock(&r->mu);
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->mu);
    }
}

static void reader_sample_fg(PtyReader *r) {
    /* Snapshot BEFORE taking the tty lock. A request posted while this ioctl
     * is blocked must wait for the next sample, not accept this older one. */
    u32 requested = atomic_load_explicit(&r->fg_req, memory_order_acquire);
    pid_t pg = tcgetpgrp(r->fd);
    i32 v = pg > 0 ? (i32)pg : -1;
    i32 prev = atomic_exchange_explicit(&r->fg_pgid, v, memory_order_acq_rel);
    /* The same moment, the same lock: is the program reading with echo off in
     * line mode — the state getpass(), sudo, ssh and `read -s` put the tty in?
     * (A full-screen program turns echo off too, but leaves line mode.) The
     * app takes Secure Event Input for as long as this says so. */
    struct termios tio;
    i32 pw = -1;
    if (tcgetattr(r->fd, &tio) == 0 && !(tio.c_lflag & ECHO) && (tio.c_lflag & ICANON)) pw = 1;
    i32 pw_prev = atomic_exchange_explicit(&r->pw_mode, pw, memory_order_acq_rel);
    u32 acknowledged = atomic_load_explicit(&r->fg_ack, memory_order_relaxed);
    atomic_store_explicit(&r->fg_ack, requested, memory_order_release);
    /* A different foreground group is something the UI shows (the agent's
     * icon, the tab's title): tell the loop. Not on the very first sample —
     * nothing was derived from "never sampled". */
    if ((prev != v && prev != 0) || requested != acknowledged || (pw_prev != pw && pw_prev != 0)) wake_loop();
}

static void reader_apply_winsize(PtyReader *r) {
    u64 w = atomic_load_explicit(&r->winsz, memory_order_acquire);
    u32 seq = (u32)(w >> 32);
    if (seq == r->winsz_applied) return;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)((w >> 16) & 0xffffu);
    ws.ws_col = (unsigned short)(w & 0xffffu);
    /* The window in pixels, for programs that size an image from the tty
     * (given 0 x 0 they guess, or refuse). */
    u32 px = atomic_load_explicit(&r->cellpx, memory_order_acquire);
    u32 xpix = (u32)ws.ws_col * (px >> 16), ypix = (u32)ws.ws_row * (px & 0xffffu);
    ws.ws_xpixel = (unsigned short)(xpix > 0xffffu ? 0xffffu : xpix);
    ws.ws_ypixel = (unsigned short)(ypix > 0xffffu ? 0xffffu : ypix);
    (void)ioctl(r->fd, TIOCSWINSZ, &ws);
    r->winsz_applied = seq;
}

static void reader_out_close(PtyReader *r);

/* Write as much of the queue as the pty takes right now; true when at least
 * one byte went. `carry` is this thread's own copy of the bytes in flight, so
 * the queue's lock is never held across write(). */
static bool reader_pump_out(PtyReader *r, u8 *carry, u32 *coff, u32 *clen) {
    bool moved = false;
    for (;;) {
        if (*coff == *clen) {
            pthread_mutex_lock(&r->omu);
            u32 n = r->olen < PTY_OUT_CARRY ? r->olen : PTY_OUT_CARRY;
            if (n) {
                memcpy(carry, r->obuf + r->ooff, n);
                r->ooff += n; r->olen -= n;
                if (r->olen == 0) r->ooff = 0;
            }
            pthread_mutex_unlock(&r->omu);
            *coff = 0; *clen = n;
            if (n == 0) break;
        }
        ssize_t w = write(r->fd, carry + *coff, *clen - *coff);
        if (w > 0) {
            *coff += (u32)w;
            atomic_fetch_sub_explicit(&r->out_pending, (u32)w, memory_order_acq_rel);
            moved = true;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w == 0 || errno == EAGAIN || errno == EWOULDBLOCK) break;
        /* The other side is gone. Whatever was queued has nowhere to go, and
         * the writer is told on its next call. */
        reader_out_close(r);
        *coff = *clen = 0;
        break;
    }
    if (atomic_load_explicit(&r->out_pending, memory_order_acquire) <= PTY_OUT_LOW_WATER &&
        atomic_exchange_explicit(&r->out_refused, false, memory_order_acq_rel))
        wake_loop();
    return moved;
}

/* The pty is finished (or the reader is): nothing queued can be delivered. */
static void reader_out_close(PtyReader *r) {
    atomic_store_explicit(&r->out_error, true, memory_order_release);
    pthread_mutex_lock(&r->omu);
    r->olen = r->ooff = 0;
    pthread_mutex_unlock(&r->omu);
    atomic_store_explicit(&r->out_pending, 0, memory_order_release);
    /* A writer that was told "not now" must hear "never" instead of waiting
     * for room that is not coming. */
    if (atomic_exchange_explicit(&r->out_refused, false, memory_order_acq_rel))
        wake_loop();
}

/* ---- a chunk of questions answered here ---------------------------------
 *
 * The bytes just read are nothing but questions whose answer needs only the
 * cursor (core/term_reply.h), the main thread has parsed every byte before
 * them (its published `tail` is this read's `head`: the ring is empty and
 * nothing is mid-parse), and nothing is on its way to the program (the queue
 * and this thread's carry are empty). Then the answer the parser would give is
 * written from here, and the question never enters the ring: the main thread
 * is not woken, and the reply does not wait for it to finish a frame.
 *
 * Ordering. A reply is input to the program like a keystroke, so it must not
 * land inside input the program is already receiving. The main thread takes
 * the publication back (pty_reader_reply_hold) BEFORE it hands this thread any
 * input, and publishes again only after a parse with nothing left owed. What
 * it hands over after that point is queued behind this reply: a reply that
 * found the queue empty went out first, and anything this thread later takes
 * from the queue was handed over after the hold, which this thread then sees.
 * A reply the pty does not take whole goes first in the carry and is owed
 * like queued input. */
static bool reader_answer_alone(PtyReader *r, const u8 *b, usize n, u32 head,
                                u8 *carry, u32 *coff, u32 *clen) {
    if (n > 64) return false;                       /* a handful of questions at most */
    u64 snap = atomic_load_explicit(&r->reply_snap, memory_order_acquire);
    if (!(snap & 0x80000000ull) || (u32)(snap >> 32) != head) return false;
    if (*coff != *clen || atomic_load_explicit(&r->out_pending, memory_order_acquire) != 0)
        return false;
    u8 out[256];
    usize len = term_fast_reply(b, n, (i32)((snap >> 16) & 0x7fffu), (i32)(snap & 0xffffu),
                                out, sizeof out);
    if (!len || len > PTY_OUT_CARRY) return false;
    usize done = 0;
    while (done < len) {
        ssize_t w = write(r->fd, out + done, len - done);
        if (w > 0) { done += (usize)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        break;
    }
    if (done < len) {
        u32 rest = (u32)(len - done);
        memcpy(carry, out + done, rest);
        *coff = 0; *clen = rest;
        atomic_fetch_add_explicit(&r->out_pending, rest, memory_order_acq_rel);
    }
    atomic_fetch_add_explicit(&r->fast_replies, 1, memory_order_relaxed);
    return true;
}

static void *reader_main(void *p) {
    PtyReader *r = p;
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    pthread_setname_np("liu-pty-reader");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    struct pollfd pf[2] = { { .fd = r->fd,     .events = POLLIN, .revents = 0 },
                            { .fd = r->ctl[0], .events = POLLIN, .revents = 0 } };
    i32 spins = 0;
    bool saturated = false;
    const i32 spin_cap = r->spin_cap;   /* resolved on the main thread */
    u8   carry[PTY_OUT_CARRY];
    u32  carry_off = 0, carry_len = 0;
    /* poll() said "writable" and write() still took nothing, this many times
     * running. The two answers come from the same kernel test and should
     * never disagree — but if they ever do, POLLOUT is level-triggered and
     * this thread would spin on it for as long as the bytes are queued. Past
     * the limit it stops asking for POLLOUT and looks again every 20 ms. */
    i32  out_stuck = 0;
    bool said_writable = false;
    u8   bounce[PTY_READ_BOUNCE];       /* this thread's stack: always warm */
    u64  last_data_ns = 0;              /* when the last bytes arrived (PTY_WAKE_QUIET_NS) */
    u64  reply_wait_until = 0;          /* up for an answer until then (PTY_REPLY_WAIT_NS) */
    const u64 reply_wait_ns = r->reply_wait_ns;   /* resolved on the main thread */

    reader_sample_fg(r);                /* an answer exists before anyone asks */

    for (;;) {
        if (atomic_load_explicit(&r->quit, memory_order_acquire)) break;
        /* Requests first: a keystroke must not wait for the output ahead of it
         * (that output may be exactly what the key is meant to stop). */
        (void)atomic_exchange_explicit(&r->attn, false, memory_order_seq_cst);
        reader_apply_winsize(r);
        if (atomic_exchange_explicit(&r->fg_want, false, memory_order_acq_rel))
            reader_sample_fg(r);
        /* Tried on EVERY turn while bytes are queued, not only after POLLOUT:
         * under a flood the read below never comes back empty, so this thread
         * never reaches poll() — and a ^C typed at `yes` must still go out. */
        if (atomic_load_explicit(&r->out_pending, memory_order_acquire)) {
            bool moved = reader_pump_out(r, carry, &carry_off, &carry_len);
            if (moved) {
                out_stuck = 0;
                if (reply_wait_until) {
                    reply_wait_until = 0;
                    atomic_fetch_add_explicit(&r->reply_caught, 1, memory_order_relaxed);
                }
            } else if (said_writable && out_stuck < 1000) out_stuck++;
            said_writable = false;
        }
        /* Snapshot the barrier generation BEFORE this iteration's read(). An
         * EAGAIN from a read that STARTED after generation N was requested
         * proves the fd held nothing at a moment after the request — which is
         * the guarantee pty_reader_quiesce_request() sells. Reading it after
         * the read would let a request that arrived mid-read be acked by an
         * EAGAIN that predates it. */
        u32 qgen = atomic_load_explicit(&r->q_req, memory_order_acquire);
        u32 head = atomic_load_explicit(&r->head, memory_order_relaxed);
        u32 freeb = ring_free(r);
        if (freeb == 0) { ring_wait_space(r); continue; }
        u32 off  = head & (r->cap - 1);
        u32 span = r->cap - off; if (span > freeb) span = freeb;
        /* Never into memory that may have to be paged in: that fault would be
         * taken under the tty lock and the program in the pane would wait for
         * it (see `wired`). A wired ring is read into directly; otherwise the
         * bytes land on this thread's stack and the copy — which is where a
         * cold ring page faults — happens after the kernel has let go. */
        ssize_t n;
        if (r->wired) {
            n = read(r->fd, r->buf + off, span);
        } else {
            u32 want = span < PTY_READ_BOUNCE ? span : PTY_READ_BOUNCE;
            n = read(r->fd, bounce, want);
            if (n > 0) {
                memcpy(r->buf + off, bounce, (usize)n);
                span = want;            /* "came back full" is about what was asked for */
                atomic_fetch_add_explicit(&r->bounced, 1, memory_order_relaxed);
            }
        }
        if (n > 0) {
            /* Questions alone, answered here: nothing for the loop to parse,
             * and the quiet that ends at the program's next output is not
             * broken by them (PTY_WAKE_QUIET_NS). */
            if (r->fast_reply &&
                reader_answer_alone(r, r->buf + off, (usize)n, head, carry, &carry_off, &carry_len))
                continue;
            atomic_store_explicit(&r->head, head + (u32)n, memory_order_release);
            /* Spin only while the last read came back FULL — a full read means
             * the kernel queue was saturated and more is almost certainly
             * behind it. `span` is part of the test because it can be under
             * 1 KiB right at the wrap, where a full read would otherwise look
             * like a short one and drop the spin exactly where it pays. An
             * interactive pty never spins. */
            saturated = ((u32)n >= span || n >= 1024);
            spins = 0;
            u64 now = reader_clock_ns();
            bool after_quiet = last_data_ns == 0 || now - last_data_ns >= PTY_WAKE_QUIET_NS;
            last_data_ns = now;
            if (after_quiet) {
                wake_loop_now();
                if (reply_wait_ns && pty_bytes_ask_reply(r->wired ? r->buf + off : bounce, (usize)n)) {
                    reply_wait_until = now + reply_wait_ns;
                    atomic_fetch_add_explicit(&r->reply_waits, 1, memory_order_relaxed);
                }
            } else {
                wake_loop();
            }
            if (atomic_load_explicit(&r->barrier_req, memory_order_acquire) !=
                atomic_load_explicit(&r->barrier_ack, memory_order_relaxed))
                barrier_notify(r);
            continue;
        }
        if (n == 0) break;                                   /* EOF */
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;   /* EIO: child gone */
        /* The one moment the kernel queue is provably empty. Ahead of the spin
         * check on purpose: EAGAIN is already the proof, and a saturated pane
         * would otherwise make a barrier wait out up to spin_cap extra reads. */
        quiesce_publish(r, qgen);
        /* A question went to the loop: stay up for its answer (the pump at the
         * top of the loop writes it) rather than park and be poked awake. */
        if (reply_wait_until) {
            if (reader_clock_ns() < reply_wait_until) continue;
            reply_wait_until = 0;
        }
        if (saturated && spins < spin_cap) { spins++; continue; }
        spins = 0; saturated = false;
        /* The queue is empty: whatever the program was doing, it has done it.
         * One ioctl per quiet moment keeps the foreground answer current
         * without the main thread ever making the call. */
        reader_sample_fg(r);
        bool out_wait = atomic_load_explicit(&r->out_pending, memory_order_acquire) != 0;
        bool ask_out  = out_wait && out_stuck < 4;
        pf[0].events = (short)(POLLIN | (ask_out ? POLLOUT : 0));
        pf[0].revents = pf[1].revents = 0;
        int pr = poll(pf, 2, out_wait && !ask_out ? 20 : -1);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;                       /* the 20 ms look-again */
        if (pf[0].revents & POLLOUT) said_writable = true;
        if (pf[1].revents) {
            /* The pipe carries two messages, so this is not an unconditional
             * break. stop() sets `quit` BEFORE it writes, so testing the flag
             * rather than the byte keeps that path exact; anything else is a
             * quiesce poke and the loop simply goes back to read(), which is
             * what acks it. Draining is not optional: POLLIN is
             * level-triggered, so an unread poke would spin this thread at
             * 100% for the life of the pane. An error on the ctl fd still
             * breaks — the alternative is the same spin with no byte to drain.
             */
            if (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            char sink[64];
            while (read(r->ctl[0], sink, sizeof sink) > 0) {}
            if (atomic_load_explicit(&r->quit, memory_order_acquire)) break;
            continue;
        }
        /* POLLIN must beat POLLHUP: a macOS pty master reports BOTH while the
         * dead child's last bytes are still queued, and breaking there
         * truncates the final output of every command that exits. */
        if (!(pf[0].revents & (POLLIN | POLLOUT)) &&
             (pf[0].revents & (POLLHUP | POLLERR | POLLNVAL))) break;
    }

    /* Stopped with bytes still queued (a pane closed in the same turn it was
     * typed into): one last attempt, so "write, then close" delivers the
     * write as a synchronous write would. Never on EOF — there is nobody
     * left to read it. */
    if (atomic_load_explicit(&r->quit, memory_order_acquire) &&
        atomic_load_explicit(&r->out_pending, memory_order_acquire))
        (void)reader_pump_out(r, carry, &carry_off, &carry_len);
    reader_out_close(r);
    atomic_store_explicit(&r->eof, true, memory_order_release);
    /* End of stream satisfies a barrier as completely as an ack does, so a
     * waiter must be told rather than sitting out its timeout. */
    quiesce_wake_all(r);
    /* A death must wake the loop too: once the fd is handed to a reader
     * nothing watches it any more, so without this a `sleep 5 && exit` in a
     * background tab would go unnoticed until some unrelated timer fired. */
    wake_loop();
    return NULL;
}

static u32 round_pow2(u32 v) {
    u32 p = 4096;
    while (p < v && p < (1u << 30)) p <<= 1;
    return p;
}

PtyReader *pty_reader_start(int fd, u32 ring_bytes) {
    if (fd < 0) return NULL;
    if (atomic_load_explicit(&g_live, memory_order_acquire) >= max_live()) return NULL;

    /* THE DATA FD MUST BE NON-BLOCKING. reader_main's loop is `read()` then
     * `poll()`, and pty_reader_stop breaks the POLL — it writes a byte to the
     * control pipe. A thread parked inside a blocking `read()` never sees that
     * byte, so stop() would block in pthread_join FOREVER, with no diagnostic
     * anywhere.
     *
     * A local pty master goes through local_parent_fd_setup(), which sets
     * O_NONBLOCK, but another caller (an ordinary pipe, say) may hand over a
     * blocking fd. One fcntl at start, idempotent for a non-blocking fd,
     * enforces the contract instead of documenting it. The reader owns this fd
     * for its lifetime — session_io_fd() returns -1 once a reader has it — so
     * changing the flag cannot surprise a concurrent user. */
    {
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || (!(fl & O_NONBLOCK) && fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0))
            return NULL;
    }

    PtyReader *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->fd  = fd;
    r->cap = round_pow2(ring_bytes ? ring_bytes : ring_default_bytes());
    r->spin_cap = spin_max();          /* main thread; never touched again */
    r->reply_wait_ns = reply_wait_max();
    r->fast_reply = fast_reply_enabled();
    r->buf = ring_map(r->cap, &r->map_len, &r->wired);
    r->ctl[0] = r->ctl[1] = -1;
    if (!r->buf) { free(r); return NULL; }

    if (pipe(r->ctl) != 0) { ring_unmap(r); free(r); return NULL; }
    for (int i = 0; i < 2; i++) {
        /* CLOEXEC is not optional: every new tab forkpty()s, and without it
         * each child would inherit every other session's control pipe (and
         * hold its write end open, so stop() could never break that poll).
         * Safe against a concurrent fork because sessions are only ever
         * created on the main thread. */
        int fl = fcntl(r->ctl[i], F_GETFD);
        int st = fcntl(r->ctl[i], F_GETFL);
        if (fl < 0 || st < 0 || fcntl(r->ctl[i], F_SETFD, fl | FD_CLOEXEC) < 0 ||
            fcntl(r->ctl[i], F_SETFL, st | O_NONBLOCK) < 0) {
            close(r->ctl[0]); close(r->ctl[1]);
            ring_unmap(r); free(r);
            return NULL;
        }
    }
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->cv, NULL);
    pthread_mutex_init(&r->qmu, NULL);
    pthread_cond_init(&r->qcv, NULL);
    pthread_mutex_init(&r->omu, NULL);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* These threads only ever call read/poll/memcpy; the platform default
     * (512 KB on macOS) times a wall of panes is real memory for nothing. */
    pthread_attr_setstacksize(&attr, 64 * 1024);
    int rc = pthread_create(&r->th, &attr, reader_main, r);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        close(r->ctl[0]); close(r->ctl[1]);
        pthread_mutex_destroy(&r->mu);
        pthread_cond_destroy(&r->cv);
        pthread_mutex_destroy(&r->qmu);
        pthread_cond_destroy(&r->qcv);
        pthread_mutex_destroy(&r->omu);
        ring_unmap(r); free(r);
        return NULL;
    }
    r->th_live = true;
    atomic_fetch_add_explicit(&g_live, 1, memory_order_acq_rel);
    /* What this process is holding for this reader: the ring at its full
     * CAPACITY (it never shrinks and the pending count is not the question)
     * plus the reader itself. Through the TRANSIENT counter because the
     * matching subtraction in stop() runs on whichever thread owns the
     * teardown — the tab-trash worker for a closed tab, the main thread
     * otherwise — and that counter is the only one safe from both.
     *
     * Not counted, and named here so the number is not read as more than it
     * is: the 64 KiB thread stack pthread_create maps below. */
    reader_account((isize)(r->cap + sizeof *r));
    return r;
}

void pty_reader_stop(PtyReader *r) {
    if (!r) return;
    if (r->th_live) {
        /* Order matters: `quit` first, so both wake paths observe it already
         * set; then the ctl write to break poll(); then the broadcast to break
         * ring_wait_space(). */
        atomic_store_explicit(&r->quit, true, memory_order_release);
        if (r->ctl[1] >= 0) { char q = 'q'; ssize_t w = write(r->ctl[1], &q, 1); (void)w; }
        pthread_mutex_lock(&r->mu);
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->mu);
        /* Wake barrier waiters before joining the reader because quit also
         * satisfies the barrier. */
        quiesce_wake_all(r);
        pthread_join(r->th, NULL);
        r->th_live = false;
        atomic_fetch_sub_explicit(&g_live, 1, memory_order_acq_rel);
    }
    if (r->ctl[0] >= 0) close(r->ctl[0]);
    if (r->ctl[1] >= 0) close(r->ctl[1]);
    pthread_mutex_destroy(&r->mu);
    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->qmu);
    pthread_cond_destroy(&r->qcv);
    pthread_mutex_destroy(&r->omu);
    /* Before the frees, so a reader that is somehow reported twice cannot
     * subtract a capacity read out of released memory. */
    reader_account(-(isize)(r->cap + sizeof *r + r->ocap));
    free(r->obuf);
    ring_unmap(r);
    free(r);   /* never touches r->fd — the owner closes that */
}

/* ---- the main thread's side of the requests above ------------------------- */

isize pty_reader_write(PtyReader *r, const u8 *data, usize len) {
    if (!r || !data) return -1;
    if (atomic_load_explicit(&r->out_error, memory_order_acquire)) return -1;
    if (len == 0) return 0;
    u32 took = 0;
    isize grew = 0;
    pthread_mutex_lock(&r->omu);
    u32 room = r->olen < PTY_OUT_CAP ? PTY_OUT_CAP - r->olen : 0;
    took = len < (usize)room ? (u32)len : room;
    if (took) {
        if (r->ooff + r->olen + took > r->ocap) {
            if (r->ooff) { memmove(r->obuf, r->obuf + r->ooff, r->olen); r->ooff = 0; }
            if (r->olen + took > r->ocap) {
                u32 want = r->ocap ? r->ocap : 4096;
                while (want < r->olen + took) want *= 2;
                if (want > PTY_OUT_CAP) want = PTY_OUT_CAP;
                u8 *nb = realloc(r->obuf, want);
                if (!nb) took = 0;
                else { grew = (isize)want - (isize)r->ocap; r->obuf = nb; r->ocap = want; }
            }
        }
        if (took) { memcpy(r->obuf + r->ooff + r->olen, data, took); r->olen += took; }
    }
    pthread_mutex_unlock(&r->omu);
    if (grew) reader_account(grew);
    if (took) {
        u32 before = atomic_fetch_add_explicit(&r->out_pending, took, memory_order_acq_rel);
        if (before == 0) reader_poke(r);
    }
    if ((usize)took < len) {
        atomic_store_explicit(&r->out_refused, true, memory_order_release);
        /* The thread may have drained past the mark between the copy above and
         * this store; then nobody else is going to say "there is room". */
        if (atomic_load_explicit(&r->out_pending, memory_order_acquire) <= PTY_OUT_LOW_WATER &&
            atomic_exchange_explicit(&r->out_refused, false, memory_order_acq_rel))
            wake_loop();
    }
    return (isize)took;
}

u32 pty_reader_write_pending(const PtyReader *r) {
    return r ? atomic_load_explicit(&r->out_pending, memory_order_acquire) : 0;
}

i32 pty_reader_fg_pgid(const PtyReader *r) {
    return r ? atomic_load_explicit(&r->fg_pgid, memory_order_acquire) : 0;
}

bool pty_reader_password_mode(const PtyReader *r) {
    return r && atomic_load_explicit(&r->pw_mode, memory_order_acquire) == 1;
}

void pty_reader_fg_refresh(PtyReader *r) {
    if (!r) return;
    atomic_store_explicit(&r->fg_want, true, memory_order_release);
    reader_poke(r);
}

u32 pty_reader_fg_request(PtyReader *r) {
    if (!r) return 0;
    u32 token = atomic_fetch_add_explicit(&r->fg_req, 1u, memory_order_acq_rel) + 1u;
    if (token == 0) token = atomic_fetch_add_explicit(&r->fg_req, 1u, memory_order_acq_rel) + 1u;
    pty_reader_fg_refresh(r);
    return token;
}

bool pty_reader_fg_result(const PtyReader *r, u32 token, i32 *pgid) {
    if (!r || !token || !pgid) return false;
    u32 acknowledged = atomic_load_explicit(&r->fg_ack, memory_order_acquire);
    if ((i32)(acknowledged - token) < 0) return false;
    *pgid = atomic_load_explicit(&r->fg_pgid, memory_order_acquire);
    return true;
}

void pty_reader_set_cell_pixels(PtyReader *r, u16 w, u16 h) {
    if (!r) return;
    atomic_store_explicit(&r->cellpx, ((u32)w << 16) | (u32)h, memory_order_release);
}

void pty_reader_set_winsize(PtyReader *r, u16 cols, u16 rows) {
    if (!r) return;
    u64 prev = atomic_load_explicit(&r->winsz, memory_order_acquire);
    u64 next;
    do {
        u32 seq = (u32)(prev >> 32) + 1u;
        if (seq == 0) seq = 1;
        next = ((u64)seq << 32) | ((u64)rows << 16) | (u64)cols;
    } while (!atomic_compare_exchange_weak_explicit(&r->winsz, &prev, next,
                                                    memory_order_acq_rel, memory_order_acquire));
    reader_poke(r);
}
