/*
 * Allocation-free latency histogram for p50, p95, p99, p99.9, and maximum
 * values. Percentiles expose tail behavior that an average can hide.
 */
#ifndef CORE_PERCENTILE_H
#define CORE_PERCENTILE_H

#include "core/types.h"

#include <stdatomic.h>

/* 16 sub-buckets per octave: 3.1% worst-case relative error. */
#define LIU_HIST_SUB_BITS 4
#define LIU_HIST_SUB      (1u << LIU_HIST_SUB_BITS)
/* Exactly the buckets liu_hist_index can produce for a u64, and no more. The
 * largest sample, 2^64-1, lands at octave 63 -> shift 59 -> index 975, so 976
 * counters cover the whole range with nothing unreachable. A round 64*SUB
 * would leave 48 buckets that no sample can reach and whose bucket_top
 * overflows u64 — harmless for recording, but it breaks the tiling invariant.
 */
#define LIU_HIST_OCTAVES  (64u - LIU_HIST_SUB_BITS + 1u)
#define LIU_HIST_BUCKETS  (LIU_HIST_OCTAVES * LIU_HIST_SUB)

typedef struct {
    u32 bucket[LIU_HIST_BUCKETS];
    u64 count;
    u64 sum_ns;
    u64 max_ns;      /* the real maximum, not a bucket top */
    u64 min_ns;
} LiuHist;

static inline void liu_hist_reset(LiuHist *h) {
    if (!h) return;
    for (u32 i = 0; i < LIU_HIST_BUCKETS; i++) h->bucket[i] = 0;
    h->count = 0;
    h->sum_ns = 0;
    h->max_ns = 0;
    h->min_ns = (u64)-1;
}

/* Values below LIU_HIST_SUB get their own exact bucket; above it, the octave
 * plus the top LIU_HIST_SUB_BITS of the mantissa. The two ranges are
 * contiguous by construction — see liu_hist_bucket_top. */
static inline u32 liu_hist_index(u64 ns) {
    if (ns < LIU_HIST_SUB) return (u32)ns;
    /* __builtin_clzll is undefined at 0, which the branch above already
     * excludes. Both compilers this tree supports provide it. */
    u32 oct   = 63u - (u32)__builtin_clzll(ns);
    u32 shift = oct - LIU_HIST_SUB_BITS;
    u32 sub   = (u32)((ns >> shift) & (LIU_HIST_SUB - 1u));
    return (shift + 1u) * LIU_HIST_SUB + sub;
}

/* The largest value that lands in `i` — what a quantile reports. */
static inline u64 liu_hist_bucket_top(u32 i) {
    if (i < LIU_HIST_SUB) return i;
    u32 shift = i / LIU_HIST_SUB - 1u;
    u32 sub   = i % LIU_HIST_SUB;
    u64 base  = ((u64)LIU_HIST_SUB + sub) << shift;
    return base + ((u64)1 << shift) - 1u;
}

static inline void liu_hist_add_ns(LiuHist *h, u64 ns) {
    if (!h) return;
    h->bucket[liu_hist_index(ns)]++;
    h->count++;
    h->sum_ns += ns;
    if (ns > h->max_ns) h->max_ns = ns;
    if (ns < h->min_ns) h->min_ns = ns;
}

/* Seconds is what the frame loop measures in. Negative durations exist —
 * phases are derived by subtraction and can go slightly negative on a clock
 * that is not perfectly monotonic between reads — and are recorded as 0
 * rather than wrapping into an enormous bucket. */
static inline void liu_hist_add_sec(LiuHist *h, f64 sec) {
    if (sec <= 0.0) { liu_hist_add_ns(h, 0); return; }
    liu_hist_add_ns(h, (u64)(sec * 1e9));
}

/* q in [0,1]. Returns nanoseconds; 0 for an empty histogram.
 *
 * The rank is ceil(q * count) so that q=1 is the maximum bucket and q=0 is the
 * first recorded sample, and so a single sample answers every quantile with
 * itself. */
static inline u64 liu_hist_quantile_ns(const LiuHist *h, f64 q) {
    if (!h || h->count == 0) return 0;
    if (q <= 0.0) q = 0.0;
    if (q >= 1.0) return h->max_ns;

    f64 want_f = q * (f64)h->count;
    u64 want = (u64)want_f;
    if ((f64)want < want_f) want++;      /* ceil */
    if (want == 0) want = 1;

    u64 seen = 0;
    for (u32 i = 0; i < LIU_HIST_BUCKETS; i++) {
        seen += h->bucket[i];
        if (seen >= want) {
            u64 top = liu_hist_bucket_top(i);
            /* Never report above the real maximum: the top of the bucket the
             * maximum landed in can exceed it by up to the bucket width. */
            return top < h->max_ns ? top : h->max_ns;
        }
    }
    return h->max_ns;
}

static inline f64 liu_hist_mean_ns(const LiuHist *h) {
    if (!h || h->count == 0) return 0.0;
    return (f64)h->sum_ns / (f64)h->count;
}

static inline f64 liu_hist_quantile_ms(const LiuHist *h, f64 q) {
    return (f64)liu_hist_quantile_ns(h, q) / 1e6;
}

/* ---------------------------------------------------------------------------
 * Lock-free variant, for a producer that cannot be made to wait.
 *
 * The Metal completion handler knows each command buffer's real duration
 * (GPUEndTime - GPUStartTime) and runs on a thread the driver owns. Handing
 * that thread a mutex is not acceptable, and handing the main thread a running
 * TOTAL and a COUNT is worse: the main thread can then only reconstruct
 * total_delta / count_delta, which is an average. Samples of 0.5, 0.5 and
 * 15.0 ms come back as three samples of 5.33 ms and the spike — the only part
 * anyone wanted — is gone.
 *
 * So the producer records into the histogram directly: one CLZ and one
 * relaxed fetch_add on a bucket, plus the count and the sum, plus a CAS only
 * when a new maximum arrives. Same bucket geometry as LiuHist, so a snapshot
 * is a plain memory read and every quantile helper above works on it
 * unchanged.
 *
 * Relaxed ordering throughout: this is a distribution, not a handshake. A
 * snapshot taken while a sample is landing may miss it, and the next snapshot
 * will have it.
 * ------------------------------------------------------------------------- */
typedef struct {
    _Atomic u32 bucket[LIU_HIST_BUCKETS];
    _Atomic u64 count;
    _Atomic u64 sum_ns;
    _Atomic u64 max_ns;
} LiuHistAtomic;

static inline void liu_hist_atomic_reset(LiuHistAtomic *h) {
    if (!h) return;
    for (u32 i = 0; i < LIU_HIST_BUCKETS; i++)
        atomic_store_explicit(&h->bucket[i], 0u, memory_order_relaxed);
    atomic_store_explicit(&h->count, 0u, memory_order_relaxed);
    atomic_store_explicit(&h->sum_ns, 0u, memory_order_relaxed);
    atomic_store_explicit(&h->max_ns, 0u, memory_order_relaxed);
}

static inline void liu_hist_atomic_add_ns(LiuHistAtomic *h, u64 ns) {
    if (!h) return;
    atomic_fetch_add_explicit(&h->bucket[liu_hist_index(ns)], 1u,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&h->count, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->sum_ns, ns, memory_order_relaxed);
    u64 seen = atomic_load_explicit(&h->max_ns, memory_order_relaxed);
    while (ns > seen &&
           !atomic_compare_exchange_weak_explicit(&h->max_ns, &seen, ns,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {}
}

/* Copy into a plain histogram the quantile helpers can read.
 *
 * `count` is recomputed from the buckets rather than taken from the atomic,
 * so a snapshot torn by a concurrent sample is still SELF-CONSISTENT: the
 * rank a quantile walks to can always be reached. */
static inline void liu_hist_atomic_snapshot(const LiuHistAtomic *src,
                                            LiuHist *dst) {
    if (!dst) return;
    liu_hist_reset(dst);
    if (!src) return;
    u64 n = 0;
    for (u32 i = 0; i < LIU_HIST_BUCKETS; i++) {
        u32 c = atomic_load_explicit(&src->bucket[i], memory_order_relaxed);
        dst->bucket[i] = c;
        n += c;
    }
    dst->count  = n;
    dst->sum_ns = atomic_load_explicit(&src->sum_ns, memory_order_relaxed);
    dst->max_ns = atomic_load_explicit(&src->max_ns, memory_order_relaxed);
    dst->min_ns = n ? 0 : (u64)-1;
}

#endif /* CORE_PERCENTILE_H */
