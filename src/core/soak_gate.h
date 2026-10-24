/*
 * Compare exact-return resources with the post-warm-up baseline; they must
 * return exactly after teardown. Evaluate RSS and phys_footprint with a robust
 * per-operation slope across later epochs, subtract a no-op control, and
 * compare projected growth with series noise and a platform floor. This avoids
 * treating one-time cache growth or isolated noisy samples as leaks.
 */
#ifndef CORE_SOAK_GATE_H
#define CORE_SOAK_GATE_H

#include "core/types.h"
#include <string.h>

/* At least this many post-warm-up epochs, or the answer is INCONCLUSIVE. Five
 * is the smallest series where a median-of-pairs slope survives one outlier. */
#define SOAK_MIN_EPOCHS 5

/* Fixed stack scratch inside soak_trend. A soak with more epochs than this
 * still works — the extra points are used for the slope, and the residual
 * statistics are taken over the first SOAK_TREND_MAXN, which is a robust
 * estimator's own answer either way. */
#define SOAK_TREND_MAXN 256

/* "No control run was available." Not the same as a control slope of zero, and
 * kept distinct so a report cannot claim a comparison it did not make. */
#define SOAK_NO_CONTROL ((f64)-1.0e300)

typedef enum {
    SOAK_TREND_FLAT = 0,     /* bounded: no persistent growth per operation  */
    SOAK_TREND_GROWING,      /* a real, reproducible slope                   */
    SOAK_TREND_INCONCLUSIVE, /* too few epochs, or no work was done          */
} SoakTrendVerdict;

/* Growth below this floor is not attributed to a workload because allocator
 * and framework caches can vary independently. */
#define SOAK_GROWTH_FLOOR_BYTES (4.0 * 1024.0 * 1024.0)

/* Additional growth floor for the sites32 workload. */
#define SOAK_PHYS_FLOOR_SITES32_BYTES (16.0 * 1024.0 * 1024.0)

/* The trend must exceed three robust deviations, using the same threshold
 * for every scenario. The factor 1.349 converts an interquartile range to
 * an estimated standard deviation for a normal distribution. */
#define SOAK_NOISE_SIGMAS 3.0
#define SOAK_IQR_TO_SIGMA (1.0 / 1.349)

/* A pass whose growth sits just under its own threshold is not the same result
 * as one an order of magnitude clear of it, and a report that prints both the
 * same way invites the second to be read as the first.
 *
 * A FLAT phys_footprint verdict above this fraction of its threshold, on a
 * series that swings widely, is correct and also noise; the run's real evidence
 * is the malloc in-use series. This marks the difference so nobody has to
 * notice it by reading the numbers. */
#define SOAK_MARGIN_THIN 0.60

static inline bool soak_trend_thin(f64 growth, f64 threshold) {
    return threshold > 0.0 && growth > threshold * SOAK_MARGIN_THIN;
}

/* Treat growth within the reciprocal margin above the threshold as marginal
 * rather than a definitive failure; allocator variation can place similar
 * samples on either side of the cutoff. */
static inline bool soak_trend_marginal(f64 growth, f64 threshold) {
    return threshold > 0.0 && growth < threshold / SOAK_MARGIN_THIN;
}

typedef struct {
    SoakTrendVerdict verdict;
    i32  n;                  /* epochs used (warm-up excluded)               */
    f64  slope;              /* bytes per operation, robust                  */
    f64  slope_net;          /* slope minus the control's                    */
    f64  span_ops;           /* operations across the measured window        */
    f64  growth;             /* slope_net x span_ops: bytes the trend explains */
    f64  noise;              /* SOAK_NOISE_SIGMAS x MAD of the residuals     */
    f64  threshold;          /* max(noise, floor) — what growth was compared to */
    f64  median_bytes;       /* the series' own scale, for context           */
    f64  first, last;        /* endpoints, reported but never decisive       */
    f64  max;                /* the high-water inside the measured window    */
} SoakTrend;

/* Median of a scratch array. Sorts it in place — the caller's copy, never the
 * sample series itself. */
static inline f64 soak_median_inplace(f64 *v, i32 n) {
    if (n <= 0) return 0.0;
    for (i32 i = 1; i < n; i++) {          /* insertion sort: n is at most a
                                            * few hundred pairwise slopes    */
        f64 key = v[i];
        i32 j = i - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; j--; }
        v[j + 1] = key;
    }
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* Theil-Sen slope of `bytes` against `ops` over n points. Pairs whose x values
 * are equal contribute nothing: an epoch that completed no operations says
 * nothing about bytes per operation, and dividing by its zero would invent a
 * verdict out of a stall. Returns 0 when no pair is usable. */
static inline f64 soak_robust_slope(const f64 *bytes, const f64 *ops, i32 n,
                                    f64 *scratch, i32 scratch_cap) {
    i32 m = 0;
    for (i32 i = 0; i < n; i++)
        for (i32 j = i + 1; j < n; j++) {
            f64 dx = ops[j] - ops[i];
            if (dx == 0.0) continue;
            if (m >= scratch_cap) break;
            scratch[m++] = (bytes[j] - bytes[i]) / dx;
        }
    if (m == 0) return 0.0;
    return soak_median_inplace(scratch, m);
}

/* The whole rule in one call.
 *
 * `bytes` / `ops` are the POST-WARM-UP series only — the caller drops epoch 0
 * before calling, because "which epoch was the warm-up" is a property of the
 * scenario, not of the arithmetic.
 *
 * `control_slope` is the same statistic from a no-op run on the same machine,
 * or SOAK_NO_CONTROL.
 *
 * `floor_bytes` is the platform's own unattributable drift across one window,
 * normally SOAK_GROWTH_FLOOR_BYTES. Passed rather than hard-coded so a test
 * can drive the rule at a scale it can construct exactly.
 *
 * `scratch` must hold n*(n-1)/2 doubles, and at least n. */
static inline SoakTrend soak_trend(const f64 *bytes, const f64 *ops, i32 n,
                                   f64 control_slope, f64 floor_bytes,
                                   f64 *scratch, i32 scratch_cap) {
    SoakTrend t;
    memset(&t, 0, sizeof t);
    t.n = n;
    if (n < SOAK_MIN_EPOCHS) { t.verdict = SOAK_TREND_INCONCLUSIVE; return t; }

    t.first = bytes[0];
    t.last  = bytes[n - 1];
    t.max   = bytes[0];
    for (i32 i = 1; i < n; i++) if (bytes[i] > t.max) t.max = bytes[i];

    /* Total work done across the window. No work means no answer. */
    if (ops[n - 1] - ops[0] <= 0.0) { t.verdict = SOAK_TREND_INCONCLUSIVE; return t; }

    t.slope = soak_robust_slope(bytes, ops, n, scratch, scratch_cap);
    /* A NEGATIVE control is never subtracted, because subtracting it ADDS.
     *
     * The control exists to remove a cost the scenario did not cause, so it can
     * only ever make a verdict kinder; a control that measured negative growth
     * says the reference gave memory back during its own run, and turning that
     * into extra growth for the scenario is an artifact, not a stricter test.
     * This guard keeps a noisy control from ever inventing growth. */
    f64 ctl = control_slope;
    if (ctl != SOAK_NO_CONTROL && ctl < 0.0) ctl = 0.0;
    t.slope_net = (ctl == SOAK_NO_CONTROL) ? t.slope : t.slope - ctl;

    t.span_ops = ops[n - 1] - ops[0];
    t.growth   = t.slope_net * t.span_ops;

    /* The series' own scale, reported for context only — nothing is decided
     * from it, as note 6 requires. */
    {
        f64 med[SOAK_TREND_MAXN];
        i32 m = n < SOAK_TREND_MAXN ? n : SOAK_TREND_MAXN;
        for (i32 i = 0; i < m; i++) med[i] = bytes[i];
        t.median_bytes = soak_median_inplace(med, m);
    }

    /* The scatter the trend has to stand out from: the interquartile range of
     * the residuals around the fitted slope. RESIDUALS, not raw values —
     * otherwise a perfectly linear leak is its own noise floor and can never
     * be detected. IQR rather than MAD for the reason in note 6. */
    {
        f64 resid[SOAK_TREND_MAXN];
        i32 m = n < SOAK_TREND_MAXN ? n : SOAK_TREND_MAXN;
        for (i32 i = 0; i < m; i++)
            resid[i] = bytes[i] - t.slope * (ops[i] - ops[0]);
        (void)soak_median_inplace(resid, m);          /* sorts in place */
        i32 q1 = m / 4, q3 = (3 * m) / 4;
        if (q3 >= m) q3 = m - 1;
        f64 iqr = resid[q3] - resid[q1];
        if (iqr < 0.0) iqr = 0.0;
        t.noise = SOAK_NOISE_SIGMAS * SOAK_IQR_TO_SIGMA * iqr;
    }

    t.threshold = t.noise > floor_bytes ? t.noise : floor_bytes;

    /* Only GROWTH fails. A negative slope is memory being given back, which is
     * never a reason to fail a soak. */
    t.verdict = (t.growth > t.threshold) ? SOAK_TREND_GROWING : SOAK_TREND_FLAT;
    return t;
}

static inline const char *soak_trend_name(SoakTrendVerdict v) {
    switch (v) {
        case SOAK_TREND_FLAT:         return "FLAT";
        case SOAK_TREND_GROWING:      return "GROWING";
        case SOAK_TREND_INCONCLUSIVE: return "INCONCLUSIVE";
    }
    return "?";
}

/* ---- exact-return resources --------------------------------------------
 *
 * No statistics, no tolerance: a count either came back or it did not. Kept
 * beside the plateau rule so the difference between the two is impossible to
 * miss when reading either. */
typedef struct {
    const char *name;
    i64 baseline;
    i64 final;
} SoakExact;

static inline bool soak_exact_ok(const SoakExact *e) {
    return e && e->final == e->baseline;
}

#endif /* CORE_SOAK_GATE_H */
