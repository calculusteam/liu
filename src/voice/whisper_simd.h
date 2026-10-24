/*
 * Liu - portable 4-wide f32 SIMD for the Whisper reference forward pass.
 *
 * Intrinsics rather than assembly, unlike src/asm/. The routines there are
 * short, fixed-shape and called from everywhere (memcpy, UTF-8 validation,
 * row scrolling); this is one arithmetic kernel whose shape is driven by the
 * model's dimensions, and keeping it in C means the ARM and x86 versions
 * cannot drift apart the way two .S files would.
 *
 * NEON and SSE2 are both guaranteed baselines on their architectures — NEON
 * on AArch64, SSE2 on x86-64 — so there is no runtime dispatch and no CPUID,
 * matching the compile-time policy the assembly paths already use. Anything
 * else falls back to scalar C that the compiler is free to vectorize itself.
 */
#ifndef VOICE_WHISPER_SIMD_H
#define VOICE_WHISPER_SIMD_H

#include "core/types.h"

#if defined(__aarch64__) || defined(__ARM_NEON)
#  include <arm_neon.h>
#  define WSIMD_NEON 1
#elif defined(__x86_64__) || defined(__SSE2__)
#  include <emmintrin.h>
#  define WSIMD_SSE2 1
#endif

/* Four independent accumulators, because a single one serializes on the
 * FMA's latency rather than its throughput: the multiply-add pipelines are
 * several cycles deep and one dependency chain leaves most of them idle.
 * Four is the point where the dot products below stop being latency-bound
 * on both an M-series core and a modern x86 one. */
#define WSIMD_UNROLL 16   /* elements consumed per iteration (4 lanes x 4) */

/* dot(a, b) over `n` elements. `n` need not be a multiple of the unroll. */
static inline f32 wsimd_dot(const f32 *a, const f32 *b, i32 n) {
#if defined(WSIMD_NEON)
    float32x4_t s0 = vdupq_n_f32(0.0f), s1 = vdupq_n_f32(0.0f);
    float32x4_t s2 = vdupq_n_f32(0.0f), s3 = vdupq_n_f32(0.0f);
    i32 i = 0;
    for (; i + WSIMD_UNROLL <= n; i += WSIMD_UNROLL) {
        s0 = vfmaq_f32(s0, vld1q_f32(a + i +  0), vld1q_f32(b + i +  0));
        s1 = vfmaq_f32(s1, vld1q_f32(a + i +  4), vld1q_f32(b + i +  4));
        s2 = vfmaq_f32(s2, vld1q_f32(a + i +  8), vld1q_f32(b + i +  8));
        s3 = vfmaq_f32(s3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    }
    for (; i + 4 <= n; i += 4)
        s0 = vfmaq_f32(s0, vld1q_f32(a + i), vld1q_f32(b + i));
    f32 acc = vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3)));
    for (; i < n; i++) acc += a[i] * b[i];
    return acc;
#elif defined(WSIMD_SSE2)
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    __m128 s2 = _mm_setzero_ps(), s3 = _mm_setzero_ps();
    i32 i = 0;
    for (; i + WSIMD_UNROLL <= n; i += WSIMD_UNROLL) {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i +  0), _mm_loadu_ps(b + i +  0)));
        s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a + i +  4), _mm_loadu_ps(b + i +  4)));
        s2 = _mm_add_ps(s2, _mm_mul_ps(_mm_loadu_ps(a + i +  8), _mm_loadu_ps(b + i +  8)));
        s3 = _mm_add_ps(s3, _mm_mul_ps(_mm_loadu_ps(a + i + 12), _mm_loadu_ps(b + i + 12)));
    }
    for (; i + 4 <= n; i += 4)
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
    __m128 s = _mm_add_ps(_mm_add_ps(s0, s1), _mm_add_ps(s2, s3));
    /* Horizontal add without SSE3's haddps — SSE2 is the baseline. */
    __m128 hi = _mm_movehl_ps(s, s);
    s = _mm_add_ps(s, hi);
    hi = _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1));
    s = _mm_add_ss(s, hi);
    f32 acc = _mm_cvtss_f32(s);
    for (; i < n; i++) acc += a[i] * b[i];
    return acc;
#else
    f32 s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    i32 i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += a[i + 0] * b[i + 0];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    f32 acc = (s0 + s1) + (s2 + s3);
    for (; i < n; i++) acc += a[i] * b[i];
    return acc;
#endif
}

/* Eight dot products against one shared vector `b`, which is what a matmul
 * row actually wants: the weight row is loaded once and reused across the
 * whole token block instead of being re-read per token. */
static inline void wsimd_dot8(const f32 *a, i64 a_stride, const f32 *b, i32 n,
                              f32 out[8]) {
#if defined(WSIMD_NEON)
    float32x4_t acc[8];
    for (i32 u = 0; u < 8; u++) acc[u] = vdupq_n_f32(0.0f);
    i32 i = 0;
    for (; i + 4 <= n; i += 4) {
        const float32x4_t bv = vld1q_f32(b + i);
        for (i32 u = 0; u < 8; u++)
            acc[u] = vfmaq_f32(acc[u], vld1q_f32(a + (i64)u * a_stride + i), bv);
    }
    for (i32 u = 0; u < 8; u++) {
        f32 s = vaddvq_f32(acc[u]);
        const f32 *ar = a + (i64)u * a_stride;
        for (i32 j = i; j < n; j++) s += ar[j] * b[j];
        out[u] = s;
    }
#elif defined(WSIMD_SSE2)
    __m128 acc[8];
    for (i32 u = 0; u < 8; u++) acc[u] = _mm_setzero_ps();
    i32 i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m128 bv = _mm_loadu_ps(b + i);
        for (i32 u = 0; u < 8; u++)
            acc[u] = _mm_add_ps(acc[u],
                                _mm_mul_ps(_mm_loadu_ps(a + (i64)u * a_stride + i), bv));
    }
    for (i32 u = 0; u < 8; u++) {
        __m128 s = acc[u];
        __m128 hi = _mm_movehl_ps(s, s);
        s = _mm_add_ps(s, hi);
        hi = _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1));
        s = _mm_add_ss(s, hi);
        f32 r = _mm_cvtss_f32(s);
        const f32 *ar = a + (i64)u * a_stride;
        for (i32 j = i; j < n; j++) r += ar[j] * b[j];
        out[u] = r;
    }
#else
    for (i32 u = 0; u < 8; u++) out[u] = wsimd_dot(a + (i64)u * a_stride, b, n);
#endif
}

/* y[i] += w * x[i] over `n` elements — the attention value accumulation. */
static inline void wsimd_axpy(f32 *y, const f32 *x, f32 w, i32 n) {
#if defined(WSIMD_NEON)
    const float32x4_t wv = vdupq_n_f32(w);
    i32 i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(y + i, vfmaq_f32(vld1q_f32(y + i), vld1q_f32(x + i), wv));
    for (; i < n; i++) y[i] += w * x[i];
#elif defined(WSIMD_SSE2)
    const __m128 wv = _mm_set1_ps(w);
    i32 i = 0;
    for (; i + 4 <= n; i += 4)
        _mm_storeu_ps(y + i, _mm_add_ps(_mm_loadu_ps(y + i),
                                        _mm_mul_ps(_mm_loadu_ps(x + i), wv)));
    for (; i < n; i++) y[i] += w * x[i];
#else
    for (i32 i = 0; i < n; i++) y[i] += w * x[i];
#endif
}

/* Which path this build compiled, for diagnostics to report. */
static inline const char *wsimd_name(void) {
#if defined(WSIMD_NEON)
    return "neon";
#elif defined(WSIMD_SSE2)
    return "sse2";
#else
    return "scalar";
#endif
}

#endif /* VOICE_WHISPER_SIMD_H */
