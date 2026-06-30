/*
 * Liu - STQ1_0 x Q8_K vec_dot kernel.
 *
 * A faithful port of llama.cpp PR #22836's STQ1_0 inference kernel. The
 * function bodies below (block_q8_K, quantize_row_q8_K, table_b2b_0,
 * ggml_vdotq_s32, the generic and ARM NEON
 * ggml_vec_dot_stq1_0_q8_K) are transcribed verbatim from llama.cpp
 * (PR #22836 + the ggml support code it depends on) and intentionally
 * keep ggml's <stdint.h> types and structure so they stay byte-checkable
 * against upstream. Only the f16->f32 conversion is swapped to liu's
 * gguf_f16_to_f32 (identical IEEE semantics) and the outer signatures are
 * trimmed to nrc==1, which is all a batch=1 decoder ever needs.
 *
 * The liu-typed wrappers at the bottom (stq1_0_q8k_bytes/_quantize,
 * stq1_0_vec_dot) are the only entry points the rest of the engine uses.
 */
#include "llm/llm_stq1_0_kernel.h"
#include "llm/llm_quant_stq1_0.h"      /* STQ1_0_BLOCK, stq1_0_row_bytes */
#include "llm/llm_stq1_0_codebook.h"   /* STQ1_0_CODEBOOK_INIT */
#include "llm/llm_gguf.h"              /* gguf_f16_to_f32 */

#include <stdint.h>
#include <math.h>
#include <string.h>
#include <assert.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* ggml super-block size — STQ1_0 and Q8_K both group 256 values. */
#define QK_K 256

/* block_q8_K — verbatim ggml layout (ggml-common.h). The intermediate
 * activation format the STQ1_0 vec_dot multiplies against: a per-block
 * f32 scale, 256 int8 quants, and the 16 group-of-16 quant sums. */
typedef struct {
    float   d;
    int8_t  qs[QK_K];
    int16_t bsums[QK_K / 16];
} block_q8_K;
_Static_assert(sizeof(block_q8_K) == sizeof(float) + QK_K + QK_K / 16 * sizeof(int16_t),
               "wrong q8_K block size/padding");

/* block_stq1_0 — verbatim ggml layout (ggml-common.h); identical to liu's
 * raw 42-byte STQ1_0 super-block (qs[32] + sign[8] + f16 scale). */
typedef struct {
    uint8_t  qs[QK_K / 8];
    uint8_t  sign[QK_K / 32];
    uint16_t d;   /* ggml_half — f16 scale bits */
} block_stq1_0;
_Static_assert(sizeof(block_stq1_0) == sizeof(uint16_t) + QK_K / 8 + QK_K / 32,
               "wrong stq1_0 block size/padding");

/* Codebook — single source of truth, shared with the CPU dequant and the
 * Metal kernels. (sign<<4 | code) -> packed 4-lane pattern. */
static const uint8_t stq1_0_codebook[32] = STQ1_0_CODEBOOK_INIT;

#if defined(__ARM_NEON)

/* Precomputed table for expanding 8 bits to 8 bytes: bit b -> (b) << 4.
 * Verbatim from ggml-cpu/arch/arm/quants.c — used as a [256][8] sign LUT,
 * each set sign bit becoming 0x10 (the codebook index's sign bit). */
#define B1(c,s,n)  0x ## n ## c ,  0x ## n ## s
#define B2(c,s,n) B1(c,s,n ## c), B1(c,s,n ## s)
#define B3(c,s,n) B2(c,s,n ## c), B2(c,s,n ## s)
#define B4(c,s,n) B3(c,s,n ## c), B3(c,s,n ## s)
#define B5(c,s,n) B4(c,s,n ## c), B4(c,s,n ## s)
#define B6(c,s,n) B5(c,s,n ## c), B5(c,s,n ## s)
#define B7(c,s,n) B6(c,s,n ## c), B6(c,s,n ## s)
#define B8(c,s  ) B7(c,s,     c), B7(c,s,     s)
static const uint64_t table_b2b_0[1 << 8] = { B8(00, 10) }; /* ( b) << 4 */
#undef B1
#undef B2
#undef B3
#undef B4
#undef B5
#undef B6
#undef B7
#undef B8

#if !defined(__ARM_FEATURE_DOTPROD)
/* vdotq_s32 emulation for ARMv8.0 NEON without the dotprod extension.
 * Verbatim from ggml-cpu/ggml-cpu-impl.h. Produces the same total sum as
 * native vdotq_s32 but with different per-lane grouping — fine here, the
 * kernel reduces all four lanes at the end. */
static inline int32x4_t ggml_vdotq_s32(int32x4_t acc, int8x16_t a, int8x16_t b) {
    const int16x8_t p0 = vmull_s8(vget_low_s8 (a), vget_low_s8 (b));
    const int16x8_t p1 = vmull_s8(vget_high_s8(a), vget_high_s8(b));
    return vaddq_s32(acc, vaddq_s32(vpaddlq_s16(p0), vpaddlq_s16(p1)));
}
#endif

#endif /* __ARM_NEON */

/* ---- Q8_K activation quantizer (ggml quantize_row_q8_K_ref) --------- */

/* ggml's round-to-nearest via the float magic-number trick (ggml-quants.c). */
static inline int nearest_int(float fval) {
    assert(fabsf(fval) <= 4194303.0f);
    float val = fval + 12582912.0f;
    int i;
    memcpy(&i, &val, sizeof(int));
    return (i & 0x007fffff) - 0x00400000;
}

/* Verbatim port of ggml's quantize_row_q8_K_ref: per 256-block, scale by
 * -127/max, round to int8, and precompute the 16 group-of-16 sums. */
static void quantize_row_q8_K(const float *x, block_q8_K *y, int64_t k) {
    const int64_t nb = k / QK_K;
    for (int64_t i = 0; i < nb; i++) {
        float max = 0.0f, amax = 0.0f;
        for (int j = 0; j < QK_K; ++j) {
            float ax = fabsf(x[j]);
            if (ax > amax) { amax = ax; max = x[j]; }
        }
        if (!amax) {
            y[i].d = 0.0f;
            memset(y[i].qs, 0, QK_K);
            /* ggml leaves bsums uninitialized in this branch; the vec_dot
             * reads them, so zero them too — only reachable on an
             * all-zero 256-block, which a real activation never is. */
            memset(y[i].bsums, 0, sizeof y[i].bsums);
            x += QK_K;
            continue;
        }
        const float iscale = -127.0f / max;
        for (int j = 0; j < QK_K; ++j) {
            int v = nearest_int(iscale * x[j]);
            y[i].qs[j] = (int8_t)(v < 127 ? v : 127);   /* MIN(127, v) */
        }
        for (int j = 0; j < QK_K / 16; ++j) {
            int sum = 0;
            for (int ii = 0; ii < 16; ++ii) sum += y[i].qs[j*16 + ii];
            y[i].bsums[j] = (int16_t)sum;
        }
        y[i].d = 1.0f / iscale;
        x += QK_K;
    }
}

/* ---- STQ1_0 x Q8_K vec_dot (llama.cpp PR #22836) ------------------- */

#if !defined(__ARM_NEON)
/* Scalar fallback — verbatim from PR #22836's generic vec_dot. Reads
 * Q8_K in the original contiguous layout (no repack). */
static void ggml_vec_dot_stq1_0_q8_K_generic(int n, float *s,
                                             const block_stq1_0 *x,
                                             const block_q8_K *y) {
    const int nb = n / QK_K;
    float sumf = 0.0f;
    for (int i = 0; i < nb; ++i) {
        int32_t sumi = 0;
        for (int g = 0; g < QK_K/4; ++g) {
            const uint8_t code  = (x[i].qs[g/2] >> (4 * (g & 1))) & 0x0F;
            const uint8_t sign  = (x[i].sign[g/8] >> (g % 8)) & 0x01;
            const uint8_t qpack = stq1_0_codebook[((uint32_t) sign << 4) | code];
            const int base = (g / 16) * 64 + (g % 16);   /* PR #22836 stride-16 */
            for (int p = 0; p < 4; ++p) {
                const int q = (qpack >> (2*p)) & 0x3;
                sumi += (q - 1) * y[i].qs[base + p*16];
            }
        }
        sumf += (float) sumi * (gguf_f16_to_f32(x[i].d) * y[i].d);
    }
    *s = sumf;
}
#endif

/* ggml_vec_dot_stq1_0_q8_K — PR #22836's centerpiece. On NEON: one
 * 32-byte tbl lookup of the codebook + native vdotq per 16-lane group.
 * The q values fed to vdotq are the raw codebook nibbles (0/1/2); the
 * (q-1) bias is folded out by subtracting the Q8_K block sums at the end
 * (sum (q-1)*y == sum q*y - sum y, and sum y is exactly y.bsums). */
static void ggml_vec_dot_stq1_0_q8_K(int n, float *s,
                                     const block_stq1_0 *x,
                                     const block_q8_K *y) {
#if defined(__ARM_NEON)
    const int nb = n / QK_K;
    float sumf = 0.0f;

    const uint8x16_t m3 = vdupq_n_u8(3);
    const uint8x16_t mask_0f = vdupq_n_u8(0x0F);
    const uint8_t (*sign_lut_16)[8] = (const uint8_t (*)[8]) table_b2b_0;

#if defined(__ARM_FEATURE_DOTPROD)
    /* dotprod path: single 32-byte tbl lookup + native vdotq. */
    const uint8x16x2_t codebook2 = { { vld1q_u8(stq1_0_codebook), vld1q_u8(stq1_0_codebook + 16) } };
    #define STQ1_0_DOT(acc, sx, sy)  vdotq_s32((acc), (sx), (sy))
    #define STQ1_0_LOOKUP(idx)       vqtbl2q_u8(codebook2, (idx))
#else
    /* ARMv8.0 NEON without dotprod: emulate vdotq_s32 and split the
     * 32-byte codebook lookup into two vqtbl1q_u8 calls. vqtbl1q_u8
     * returns 0 for out-of-range indices, so OR-ing the low and high
     * halves reproduces vqtbl2q_u8 byte-for-byte. */
    const uint8x16_t cb_lo = vld1q_u8(stq1_0_codebook);
    const uint8x16_t cb_hi = vld1q_u8(stq1_0_codebook + 16);
    const uint8x16_t v16   = vdupq_n_u8(16);
    #define STQ1_0_DOT(acc, sx, sy)  ggml_vdotq_s32((acc), (sx), (sy))
    #define STQ1_0_LOOKUP(idx)       vorrq_u8(vqtbl1q_u8(cb_lo, (idx)), \
                                            vqtbl1q_u8(cb_hi, vsubq_u8((idx), v16)))
#endif

    /* Each half processes 16 bytes of x.qs (32 codes), 4 bytes of x.sign,
     * and 128 bytes of y.qs (4 lanes x 16 bytes x 2 wide-blocks). */
#define STQ1_0_DOT_HALF(QS_PTR, SIGN_PTR, YP_PTR) do {                              \
    const uint8x16_t packed = vld1q_u8(QS_PTR);                                     \
    const uint8x16_t lo     = vandq_u8(packed, mask_0f);                            \
    const uint8x16_t hi     = vshrq_n_u8(packed, 4);                                \
    const uint8x16_t idx0   = vzip1q_u8(lo, hi);                                    \
    const uint8x16_t idx1   = vzip2q_u8(lo, hi);                                    \
    const uint8_t * sp      = (SIGN_PTR);                                           \
    const uint8x16_t s0 = vcombine_u8(vld1_u8(sign_lut_16[sp[0]]),                  \
                                      vld1_u8(sign_lut_16[sp[1]]));                 \
    const uint8x16_t s1 = vcombine_u8(vld1_u8(sign_lut_16[sp[2]]),                  \
                                      vld1_u8(sign_lut_16[sp[3]]));                 \
    const uint8x16_t sel_0 = STQ1_0_LOOKUP(vorrq_u8(idx0, s0));                     \
    const uint8x16_t sel_1 = STQ1_0_LOOKUP(vorrq_u8(idx1, s1));                     \
    const int8x16_t sqx0 = vreinterpretq_s8_u8(vandq_u8(sel_0, m3));                \
    const int8x16_t sqx1 = vreinterpretq_s8_u8(vandq_u8(vshrq_n_u8(sel_0, 2), m3)); \
    const int8x16_t sqx2 = vreinterpretq_s8_u8(vandq_u8(vshrq_n_u8(sel_0, 4), m3)); \
    const int8x16_t sqx3 = vreinterpretq_s8_u8(vshrq_n_u8(sel_0, 6));               \
    const int8x16_t sqx4 = vreinterpretq_s8_u8(vandq_u8(sel_1, m3));                \
    const int8x16_t sqx5 = vreinterpretq_s8_u8(vandq_u8(vshrq_n_u8(sel_1, 2), m3)); \
    const int8x16_t sqx6 = vreinterpretq_s8_u8(vandq_u8(vshrq_n_u8(sel_1, 4), m3)); \
    const int8x16_t sqx7 = vreinterpretq_s8_u8(vshrq_n_u8(sel_1, 6));               \
    const int8_t * yp = (YP_PTR);                                                   \
    sumi0 = STQ1_0_DOT(sumi0, sqx0, vld1q_s8(yp +   0));                            \
    sumi1 = STQ1_0_DOT(sumi1, sqx1, vld1q_s8(yp +  16));                            \
    sumi2 = STQ1_0_DOT(sumi2, sqx2, vld1q_s8(yp +  32));                            \
    sumi3 = STQ1_0_DOT(sumi3, sqx3, vld1q_s8(yp +  48));                            \
    sumi0 = STQ1_0_DOT(sumi0, sqx4, vld1q_s8(yp +  64));                            \
    sumi1 = STQ1_0_DOT(sumi1, sqx5, vld1q_s8(yp +  80));                            \
    sumi2 = STQ1_0_DOT(sumi2, sqx6, vld1q_s8(yp +  96));                            \
    sumi3 = STQ1_0_DOT(sumi3, sqx7, vld1q_s8(yp + 112));                            \
} while (0)

    for (int i = 0; i < nb; ++i) {
        int32x4_t sumi0 = vdupq_n_s32(0);
        int32x4_t sumi1 = vdupq_n_s32(0);
        int32x4_t sumi2 = vdupq_n_s32(0);
        int32x4_t sumi3 = vdupq_n_s32(0);

        STQ1_0_DOT_HALF(x[i].qs,      x[i].sign,     y[i].qs);
        STQ1_0_DOT_HALF(x[i].qs + 16, x[i].sign + 4, y[i].qs + 128);

        const int16x8_t ysum0 = vld1q_s16(y[i].bsums);
        const int16x8_t ysum1 = vld1q_s16(y[i].bsums + 8);
        const float d = gguf_f16_to_f32(x[i].d) * y[i].d;

        sumi0 = vaddq_s32(vaddq_s32(sumi0, sumi1), vaddq_s32(sumi2, sumi3));
        sumi0 = vsubq_s32(sumi0, vpaddlq_s16(vaddq_s16(ysum0, ysum1)));

        sumf += d * (float) vaddvq_s32(sumi0);
    }

#undef STQ1_0_DOT
#undef STQ1_0_LOOKUP
#undef STQ1_0_DOT_HALF

    *s = sumf;
#else
    ggml_vec_dot_stq1_0_q8_K_generic(n, s, x, y);
#endif
}

/* ---- public, liu-typed interface ----------------------------------- */

usize stq1_0_q8k_bytes(i32 n) {
    i32 nb = (n + QK_K - 1) / QK_K;
    return (usize)nb * sizeof(block_q8_K);
}

void stq1_0_q8k_quantize(const f32 *x, void *q8k, i32 n) {
    if (!x || !q8k || n < QK_K) return;
    quantize_row_q8_K(x, (block_q8_K *)q8k, n);
    /* No activation repack: the stride-16 STQ1_0 grouping (PR #22836) lets
     * the NEON vec_dot read each lane plane as a contiguous y.qs slice. */
}

f32 stq1_0_vec_dot(const u8 *weight, const void *q8k, i32 n) {
    if (!weight || !q8k || n < QK_K) return 0.0f;
    float s = 0.0f;
    ggml_vec_dot_stq1_0_q8_K(n, &s, (const block_stq1_0 *)weight,
                             (const block_q8_K *)q8k);
    return s;
}
