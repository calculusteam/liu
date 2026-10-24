/*
 * Liu - Whisper compute kernels (Metal).
 *
 * Dequantization portions derived from llama.cpp / ggml:
 * Copyright (c) 2023-2026 The ggml authors
 * These portions are licensed under the MIT License; see
 * packaging/licenses/llama.cpp-LICENSE.txt and THIRD_PARTY_NOTICES.md.
 *
 * Weights stay quantized on the GPU and are dequantized inside the kernels,
 * the same policy the local-LLM backend follows: expanding large-v3-turbo to
 * F16 would put 1.6 GB of unified memory behind a terminal, where the
 * quantized file is 0.55 GB.
 *
 * Two structural notes:
 *
 *   - The GEMM's k-tile is 32 because every ggml block quantization packs 32
 *     elements. One k-tile of one output row is therefore exactly one block,
 *     and staging a tile into threadgroup memory is a flat "each thread
 *     dequantizes half a block" with no partial-block case.
 *   - Blocks are byte-addressed. The container has no alignment padding and
 *     a block is 18/20/22/24/34 bytes, so nothing here may reinterpret a
 *     device pointer as ushort* or float*; scalars are reassembled from
 *     bytes.
 */
#include <metal_stdlib>
#include <metal_simdgroup_matrix>

#include "whisper_metal_params.h"

using namespace metal;

/* Tile decomposition, derived so the shape can be retuned from the header
 * alone: each simdgroup owns a WM_SG_NTILES-wide column strip and the full
 * height of the tile. */
#define WM_SG_COUNT   (WM_GEMM_THREADS / 32)
#define WM_MTILES     (WM_BM / 8)
#define WM_SG_NTILES  (WM_BN / 8 / WM_SG_COUNT)
/* Rows drained per pass, sized so the staging fits the A/B tile memory. */
#define WM_DRAIN_ROWS 16

/* The kernels below hardcode their tile decomposition in the accumulator
 * shapes and the staging loops, so the tile constants are not free to change
 * without the derivations above following along. Pinning that here turns
 * "someone edited a #define" into a build error instead of silently wrong
 * arithmetic. */
static_assert(WM_BM % 8 == 0 && WM_BK == 32,
              "k_gemm walks the tile in 8x8 simdgroup matrices");
static_assert(WM_BN % (8 * WM_SG_COUNT) == 0,
              "the output columns must divide evenly among the simdgroups");
static_assert(WM_BN * 2 <= WM_GEMM_THREADS,
              "B staging gives each participating thread half of one quant block");
static_assert(WM_DRAIN_ROWS % 8 == 0 && WM_BM % WM_DRAIN_ROWS == 0,
              "the drain walks whole 8-row simdgroup tiles");
static_assert(WM_DRAIN_ROWS * WM_BN * 4 <= (WM_BM * WM_BK + WM_BN * WM_BK) * 2,
              "the output staging must fit inside the A/B tile memory it aliases");
static_assert(WM_ATTN_QB % 8 == 0 && WM_ATTN_KB % 8 == 0,
              "attention walks both products in 8x8 simdgroup matrices");
static_assert(WM_ATTN_KB / 8 == WM_ATTN_SG,
              "the score product gives each simdgroup one 8-wide key column");
static_assert(WM_ATTN_THREADS >= WM_ATTN_QB,
              "the softmax step runs one thread per query row");

/* Report the tile constants this metallib was compiled with.
 *
 * The host and the shader share whisper_metal_params.h, and when the two are
 * built from different versions of it nothing fails: the host dispatches one
 * threadgroup count and the kernel tiles by another, and the model transcribes
 * fluent gibberish. CMake gets the dependency right, but a bundle assembled by
 * hand (or a stale copy next to the binary) will not, so the host checks these
 * at startup and falls back to the CPU backend rather than trusting them. */
kernel void k_params_probe(device int *out [[buffer(0)]],
                           uint gid [[thread_position_in_grid]]) {
    if (gid != 0) return;
    out[0] = WM_BM;
    out[1] = WM_BN;
    out[2] = WM_BK;
    out[3] = WM_GEMM_THREADS;
    out[4] = WM_ATTN_QB;
    out[5] = WM_ATTN_KB;
    out[6] = WM_ATTN_THREADS;
    out[7] = WM_ATTN_HD_MAX;
}

/* ======================================================================
 * Block dequantization
 * =================================================================== */

/* Little-endian half from two unaligned bytes. */
inline half wm_half_at(device const uchar *p) {
    ushort bits = (ushort)p[0] | ((ushort)p[1] << 8);
    return as_type<half>(bits);
}

inline float wm_float_at(device const uchar *p) {
    uint bits = (uint)p[0] | ((uint)p[1] << 8) | ((uint)p[2] << 16) | ((uint)p[3] << 24);
    return as_type<float>(bits);
}

inline int wm_block_bytes(int dt) {
    switch (dt) {
        case WM_TYPE_F32:  return 128;   /* 32 floats */
        case WM_TYPE_F16:  return 64;    /* 32 halfs  */
        case WM_TYPE_Q4_0: return 18;
        case WM_TYPE_Q4_1: return 20;
        case WM_TYPE_Q5_0: return 22;
        case WM_TYPE_Q5_1: return 24;
        case WM_TYPE_Q8_0: return 34;
        default:           return 0;
    }
}

/* Dequantize elements [h*16, h*16+16) of one 32-element block.
 *
 * The 4- and 5-bit layouts split a byte across element j and element j+16,
 * never j*2 and j*2+1 — so h == 0 reads the low nibbles and h == 1 the high
 * ones, and each half lands contiguously in the output. */
inline void wm_dq_half(device const uchar *blk, int dt, int h, thread half out[16]) {
    switch (dt) {
        case WM_TYPE_F32: {
            device const uchar *p = blk + h * 64;
            for (int i = 0; i < 16; i++) out[i] = (half)wm_float_at(p + i * 4);
            break;
        }
        case WM_TYPE_F16: {
            device const uchar *p = blk + h * 32;
            for (int i = 0; i < 16; i++) out[i] = wm_half_at(p + i * 2);
            break;
        }
        case WM_TYPE_Q4_0: {
            const half d = wm_half_at(blk);
            device const uchar *qs = blk + 2;
            for (int j = 0; j < 16; j++) {
                int q = h == 0 ? (qs[j] & 0x0F) : (qs[j] >> 4);
                out[j] = (half)(q - 8) * d;
            }
            break;
        }
        case WM_TYPE_Q4_1: {
            const half d = wm_half_at(blk);
            const half m = wm_half_at(blk + 2);
            device const uchar *qs = blk + 4;
            for (int j = 0; j < 16; j++) {
                int q = h == 0 ? (qs[j] & 0x0F) : (qs[j] >> 4);
                out[j] = (half)q * d + m;
            }
            break;
        }
        case WM_TYPE_Q5_0: {
            const half d = wm_half_at(blk);
            uint qh = (uint)blk[2] | ((uint)blk[3] << 8) | ((uint)blk[4] << 16) | ((uint)blk[5] << 24);
            device const uchar *qs = blk + 6;
            for (int j = 0; j < 16; j++) {
                int q, hi;
                if (h == 0) { q = qs[j] & 0x0F; hi = (int)(((qh >> j) << 4) & 0x10); }
                else        { q = qs[j] >> 4;   hi = (int)((qh >> (j + 12)) & 0x10); }
                out[j] = (half)((q | hi) - 16) * d;
            }
            break;
        }
        case WM_TYPE_Q5_1: {
            const half d = wm_half_at(blk);
            const half m = wm_half_at(blk + 2);
            uint qh = (uint)blk[4] | ((uint)blk[5] << 8) | ((uint)blk[6] << 16) | ((uint)blk[7] << 24);
            device const uchar *qs = blk + 8;
            for (int j = 0; j < 16; j++) {
                int q, hi;
                if (h == 0) { q = qs[j] & 0x0F; hi = (int)(((qh >> j) << 4) & 0x10); }
                else        { q = qs[j] >> 4;   hi = (int)((qh >> (j + 12)) & 0x10); }
                out[j] = (half)(q | hi) * d + m;
            }
            break;
        }
        case WM_TYPE_Q8_0: {
            const half d = wm_half_at(blk);
            device const uchar *qs = blk + 2 + h * 16;
            for (int j = 0; j < 16; j++) out[j] = (half)((int)(char)qs[j]) * d;
            break;
        }
        default:
            for (int j = 0; j < 16; j++) out[j] = 0.0h;
            break;
    }
}

/* Single element `e` (0..31) of a block — the matvec path, where each lane
 * owns one lane-indexed element rather than a contiguous run. */
inline float wm_dq_one(device const uchar *blk, int dt, int e) {
    switch (dt) {
        case WM_TYPE_F32: return wm_float_at(blk + e * 4);
        case WM_TYPE_F16: return (float)wm_half_at(blk + e * 2);
        case WM_TYPE_Q4_0: {
            const float d = (float)wm_half_at(blk);
            int j = e & 15;
            int q = (e < 16) ? (blk[2 + j] & 0x0F) : (blk[2 + j] >> 4);
            return (float)(q - 8) * d;
        }
        case WM_TYPE_Q4_1: {
            const float d = (float)wm_half_at(blk);
            const float m = (float)wm_half_at(blk + 2);
            int j = e & 15;
            int q = (e < 16) ? (blk[4 + j] & 0x0F) : (blk[4 + j] >> 4);
            return (float)q * d + m;
        }
        case WM_TYPE_Q5_0: {
            const float d = (float)wm_half_at(blk);
            uint qh = (uint)blk[2] | ((uint)blk[3] << 8) | ((uint)blk[4] << 16) | ((uint)blk[5] << 24);
            int j = e & 15;
            int q, hi;
            if (e < 16) { q = blk[6 + j] & 0x0F; hi = (int)(((qh >> j) << 4) & 0x10); }
            else        { q = blk[6 + j] >> 4;   hi = (int)((qh >> (j + 12)) & 0x10); }
            return (float)((q | hi) - 16) * d;
        }
        case WM_TYPE_Q5_1: {
            const float d = (float)wm_half_at(blk);
            const float m = (float)wm_half_at(blk + 2);
            uint qh = (uint)blk[4] | ((uint)blk[5] << 8) | ((uint)blk[6] << 16) | ((uint)blk[7] << 24);
            int j = e & 15;
            int q, hi;
            if (e < 16) { q = blk[8 + j] & 0x0F; hi = (int)(((qh >> j) << 4) & 0x10); }
            else        { q = blk[8 + j] >> 4;   hi = (int)((qh >> (j + 12)) & 0x10); }
            return (float)(q | hi) * d + m;
        }
        case WM_TYPE_Q8_0: {
            const float d = (float)wm_half_at(blk);
            return (float)((int)(char)blk[2 + e]) * d;
        }
        default: return 0.0f;
    }
}

/* Byte offset of the block covering element `k` of row `n`. */
inline uint wm_row_block(int n, int k, int K, int dt) {
    const int bb = wm_block_bytes(dt);
    const int blocks_per_row = K / 32;
    return (uint)((((long)n * blocks_per_row) + (k / 32)) * bb);
}

/* ======================================================================
 * GEMM:  C[M,N] = A[M,K] . B[N,K]^T + bias[N]
 *
 * B is row-major over K, which is what makes this a dot-product GEMM with
 * no transpose of the weight in memory: output n owns the K contiguous
 * values at n*K.
 * =================================================================== */

kernel void k_gemm(device const float  *A     [[buffer(0)]],
                   device const uchar  *B     [[buffer(1)]],
                   device const float  *bias  [[buffer(2)]],
                   device       float  *C     [[buffer(3)]],
                   constant WmGemm     &p     [[buffer(4)]],
                   uint3  tgid [[threadgroup_position_in_grid]],
                   uint   tid  [[thread_index_in_threadgroup]],
                   uint   sgid [[simdgroup_index_in_threadgroup]]) {
    /* One allocation, two lives: the A and B tiles while the k-loop runs,
     * then the output staging once it is done. Giving the output its own
     * buffer would raise the threadgroup footprint and cost occupancy for
     * memory that is provably dead. */
    threadgroup half tg[WM_BM * WM_BK + WM_BN * WM_BK];
    threadgroup half *As = tg;
    threadgroup half *Bs = tg + WM_BM * WM_BK;

    const int m0 = (int)tgid.y * WM_BM;
    const int n0 = (int)tgid.x * WM_BN;
    const int sg_n0 = (int)sgid * (WM_SG_NTILES * 8);

    simdgroup_float8x8 acc[WM_MTILES][WM_SG_NTILES];
    for (int i = 0; i < WM_MTILES; i++)
        for (int j = 0; j < WM_SG_NTILES; j++)
            acc[i][j] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);

    const int n_ktiles = p.K / WM_BK;

    for (int kt = 0; kt < n_ktiles; kt++) {
        const int k0 = kt * WM_BK;

        /* Stage A. Rows past M are zeroed so the accumulators stay clean for
         * the ragged last tile. */
        for (int idx = (int)tid; idx < WM_BM * WM_BK; idx += WM_GEMM_THREADS) {
            const int mm = idx / WM_BK;
            const int kk = idx % WM_BK;
            const int m  = m0 + mm;
            As[idx] = (m < p.M) ? (half)A[(long)m * p.K + k0 + kk] : 0.0h;
        }

        /* Stage B: one k-tile of one output row IS one quant block, so each
         * thread unpacks half a block and there is nothing left over. */
        if (tid < WM_BN * 2) {
            const int n  = (int)tid >> 1;
            const int hh = (int)tid & 1;
            const int gn = n0 + n;
            half tmp[16];
            if (gn < p.N) {
                wm_dq_half(B + wm_row_block(gn, k0, p.K, p.dtype), p.dtype, hh, tmp);
            } else {
                for (int i = 0; i < 16; i++) tmp[i] = 0.0h;
            }
            for (int i = 0; i < 16; i++) Bs[n * WM_BK + hh * 16 + i] = tmp[i];
        }

        threadgroup_barrier(mem_flags::mem_threadgroup);

        for (int kk = 0; kk < WM_BK / 8; kk++) {
            simdgroup_half8x8 bmat[WM_SG_NTILES];
            /* transpose: Bs is [n][k]; the matmul wants [k][n]. */
            for (int j = 0; j < WM_SG_NTILES; j++)
                simdgroup_load(bmat[j], Bs, WM_BK, ulong2(kk * 8, sg_n0 + j * 8), true);
            /* Keep one A tile live at a time. Holding all tiles in an array
             * increases register use at larger WM_BM values; the half8x8
             * matrices and float8x8 accumulators can exceed the register
             * file and spill. */
            for (int i = 0; i < WM_MTILES; i++) {
                simdgroup_half8x8 amat;
                simdgroup_load(amat, As, WM_BK, ulong2(kk * 8, i * 8));
                for (int j = 0; j < WM_SG_NTILES; j++)
                    simdgroup_multiply_accumulate(acc[i][j], amat, bmat[j], acc[i][j]);
            }
        }

        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    /* Drain through the (now dead) tile memory, WM_DRAIN_ROWS at a time. */
    threadgroup float *Cs = (threadgroup float *)tg;

    for (int r0 = 0; r0 < WM_BM; r0 += WM_DRAIN_ROWS) {
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (int i = 0; i < WM_DRAIN_ROWS / 8; i++) {
            for (int j = 0; j < WM_SG_NTILES; j++) {
                simdgroup_store(acc[r0 / 8 + i][j],
                                Cs + i * 8 * WM_BN + sg_n0 + j * 8, WM_BN);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        for (int idx = (int)tid; idx < WM_DRAIN_ROWS * WM_BN; idx += WM_GEMM_THREADS) {
            const int mm = idx / WM_BN;
            const int nn = idx % WM_BN;
            const int m = m0 + r0 + mm;
            const int n = n0 + nn;
            if (m < p.M && n < p.N) {
                C[(long)m * p.N + n] = Cs[idx] + (p.has_bias ? bias[n] : 0.0f);
            }
        }
    }
}

/* Same product for a handful of rows. The tiled GEMM would spend 31 of its
 * 32 m-lanes on padding during token-by-token decoding, and the tied LM head
 * (M = 1, N = 51866) is the single most-dispatched matmul in a transcript. */
kernel void k_matvec(device const float  *A     [[buffer(0)]],
                     device const uchar  *B     [[buffer(1)]],
                     device const float  *bias  [[buffer(2)]],
                     device       float  *C     [[buffer(3)]],
                     constant WmGemm     &p     [[buffer(4)]],
                     uint3  tgid [[threadgroup_position_in_grid]],
                     uint   sgid [[simdgroup_index_in_threadgroup]],
                     uint   lane [[thread_index_in_simdgroup]]) {
    const int n = (int)tgid.x * WM_MATVEC_SG + (int)sgid;
    if (n >= p.N) return;

    const int nblk = p.K / 32;
    const int bb   = wm_block_bytes(p.dtype);
    device const uchar *row = B + (long)n * nblk * bb;

    for (int m = 0; m < p.M; m++) {
        device const float *a = A + (long)m * p.K;
        float acc = 0.0f;
        for (int b = 0; b < nblk; b++) {
            acc += a[b * 32 + (int)lane] * wm_dq_one(row + (long)b * bb, p.dtype, (int)lane);
        }
        acc = simd_sum(acc);
        if (lane == 0) C[(long)m * p.N + n] = acc + (p.has_bias ? bias[n] : 0.0f);
    }
}

/* ======================================================================
 * conv1d (kernel 3, padding 1) + optional GELU
 *
 * Channel-major in and out: in[ic*t_in + t], out[oc*t_out + t]. One
 * threadgroup owns one output channel and 256 consecutive timesteps, so the
 * weight row it stages is read once for 256 outputs instead of once each.
 * =================================================================== */

inline float wm_gelu(float x) {
    const float a = 0.044715f;
    const float s = 0.79788456080286535587989211986876f;   /* sqrt(2/pi) */
    return 0.5f * x * (1.0f + precise::tanh(s * x * (1.0f + a * x * x)));
}

kernel void k_conv1d(device const float *in    [[buffer(0)]],
                     device const float *w     [[buffer(1)]],   /* [out_ch][in_ch][3], f32 */
                     device const float *bias  [[buffer(2)]],
                     device       float *out   [[buffer(3)]],
                     constant WmConv    &p     [[buffer(4)]],
                     uint3 tgid [[threadgroup_position_in_grid]],
                     uint  tid  [[thread_index_in_threadgroup]]) {
    const int oc = (int)tgid.y;
    const int t  = (int)tgid.x * WM_CONV_THREADS + (int)tid;

    device const float *wrow = w + (long)oc * p.in_ch * 3;
    float acc = bias[oc];

    if (t < p.t_out) {
        const int c = t * p.stride;   /* taps at c-1, c, c+1 */
        for (int ic = 0; ic < p.in_ch; ic++) {
            device const float *x = in + (long)ic * p.t_in;
            device const float *k = wrow + ic * 3;
            if (c - 1 >= 0)      acc += k[0] * x[c - 1];
            if (c     < p.t_in)  acc += k[1] * x[c];
            if (c + 1 < p.t_in)  acc += k[2] * x[c + 1];
        }
        out[(long)oc * p.t_out + t] = p.apply_gelu ? wm_gelu(acc) : acc;
    }
}

/* ======================================================================
 * LayerNorm (affine), one threadgroup per token
 * =================================================================== */

/* Out-of-place so the residual stream survives: every Whisper sub-block
 * normalizes a COPY of x, transforms it, and adds the result back to the
 * untouched x. Normalizing in place would need a separate 7.7 MB blit per
 * sub-block, three per layer, ninety-six per encode.
 *
 * Reduction slots are deliberately disjoint (0..7 sum, 8..15 varsum, 16
 * mean, 17 inv) rather than recycled: threads read a broadcast slot without
 * a barrier guarding the next write to it, and reusing red[0] lets a fast
 * lane clobber the mean while a slow one is still reading it. */
kernel void k_layernorm(device       float *out  [[buffer(0)]],
                        device const float *in   [[buffer(1)]],
                        device const float *g    [[buffer(2)]],
                        device const float *b    [[buffer(3)]],
                        constant WmNorm    &p    [[buffer(4)]],
                        uint3 tgid [[threadgroup_position_in_grid]],
                        uint  tid  [[thread_index_in_threadgroup]],
                        uint3 ntg3 [[threads_per_threadgroup]]) {
    const uint ntg = ntg3.x;
    threadgroup float red[32];

    device const float *src = in  + (long)tgid.x * p.n_state;
    device       float *dst = out + (long)tgid.x * p.n_state;
    const uint nsg = ntg >> 5;

    float s = 0.0f;
    for (int i = (int)tid; i < p.n_state; i += (int)ntg) s += src[i];
    s = simd_sum(s);
    if ((tid & 31) == 0) red[tid >> 5] = s;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (tid == 0) {
        float t = 0.0f;
        for (uint i = 0; i < nsg; i++) t += red[i];
        red[16] = t / (float)p.n_state;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const float mean = red[16];

    float v = 0.0f;
    for (int i = (int)tid; i < p.n_state; i += (int)ntg) {
        const float d = src[i] - mean;
        v += d * d;
    }
    v = simd_sum(v);
    if ((tid & 31) == 0) red[8 + (tid >> 5)] = v;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (tid == 0) {
        float t = 0.0f;
        for (uint i = 0; i < nsg; i++) t += red[8 + i];
        red[17] = rsqrt(t / (float)p.n_state + 1e-5f);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const float inv = red[17];

    for (int i = (int)tid; i < p.n_state; i += (int)ntg) {
        dst[i] = (src[i] - mean) * inv * g[i] + b[i];
    }
}

/* ======================================================================
 * Multi-head attention, tiled over queries with an online softmax
 *
 * One threadgroup owns WM_ATTN_QB queries of one head and walks the keys in
 * WM_ATTN_KB steps, keeping a running (max, sum, output) per query instead of
 * materializing a full score row. Three properties follow:
 *
 *   - Each K and V row is fetched once per threadgroup and used by all 32
 *     queries in it. Fetched once PER QUERY instead, the encoder's attention
 *     would be bound by memory bandwidth.
 *   - Both products run on the simdgroup matrix units rather than as scalar
 *     dot products.
 *   - No O(n_kv) score buffer, so threadgroup memory stays around 12 KB and
 *     more than one threadgroup stays resident per core.
 *
 * Q, K and V are read straight from device memory by simdgroup_load rather
 * than staged first — they are already f32 and contiguous over head_dim, and
 * the matrix loader handles the strides (including the transpose K needs).
 * The cost is that the loader has no bounds check, so the caller MUST pad
 * every activation buffer to a whole number of WM_ATTN_QB rows and zero the
 * padding: a NaN in a padded V row would survive its zero probability and
 * poison the output. whisper_metal_create does both.
 *
 * The running output lives in threadgroup memory, not in accumulators. The
 * online softmax rescales it by exp(m_old - m_new) every step, and a per-row
 * scale is awkward to apply to a simdgroup matrix but trivial to a plain
 * array — which is also why the PV product loads O as its accumulator and
 * stores it straight back.
 * =================================================================== */

kernel void k_attention(device const float *q    [[buffer(0)]],
                        device const float *k    [[buffer(1)]],
                        device const float *v    [[buffer(2)]],
                        device       float *out  [[buffer(3)]],
                        constant WmAttn    &p    [[buffer(4)]],
                        uint3 tgid [[threadgroup_position_in_grid]],
                        uint  tid  [[thread_index_in_threadgroup]],
                        uint  sgid [[simdgroup_index_in_threadgroup]]) {
    threadgroup float Sf[WM_ATTN_QB * WM_ATTN_KB];
    threadgroup float Ot[WM_ATTN_QB * WM_ATTN_HD_MAX];
    threadgroup float m_run[WM_ATTN_QB];
    threadgroup float l_run[WM_ATTN_QB];
    threadgroup float corr[WM_ATTN_QB];

    const int h   = (int)tgid.y;
    const int q0  = (int)tgid.x * WM_ATTN_QB;
    const int hd  = p.head_dim;
    const int hdt = hd / 8;               /* head-dim tiles */
    const int ns  = p.n_state;

    device const float *qbase = q + (long)q0 * ns + (long)h * hd;

    for (int idx = (int)tid; idx < WM_ATTN_QB * hd; idx += WM_ATTN_THREADS) Ot[idx] = 0.0f;
    if (tid < WM_ATTN_QB) { m_run[tid] = -INFINITY; l_run[tid] = 0.0f; }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (int j0 = 0; j0 < p.n_kv; j0 += WM_ATTN_KB) {
        device const float *kbase = k + (long)j0 * ns + (long)h * hd;
        device const float *vbase = v + (long)j0 * ns + (long)h * hd;

        /* ---- S = Q . K^T ------------------------------------------- */
        {
            const int nn = (int)sgid;     /* this simdgroup's 8 key columns */
            simdgroup_float8x8 acc[WM_ATTN_QB / 8];
            for (int i = 0; i < WM_ATTN_QB / 8; i++)
                acc[i] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);

            for (int dd = 0; dd < hdt; dd++) {
                simdgroup_float8x8 kt;
                /* transpose: K is [key][dim] and the product wants [dim][key] */
                simdgroup_load(kt, kbase, ns, ulong2(dd * 8, nn * 8), true);
                for (int mm = 0; mm < WM_ATTN_QB / 8; mm++) {
                    simdgroup_float8x8 qm;
                    simdgroup_load(qm, qbase, ns, ulong2(dd * 8, mm * 8));
                    simdgroup_multiply_accumulate(acc[mm], qm, kt, acc[mm]);
                }
            }
            for (int mm = 0; mm < WM_ATTN_QB / 8; mm++)
                simdgroup_store(acc[mm], Sf + mm * 8 * WM_ATTN_KB + nn * 8, WM_ATTN_KB);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        /* ---- mask, scale, online softmax (one thread per query row) -- */
        if (tid < WM_ATTN_QB) {
            const int row = (int)tid;
            const int i   = q0 + row;
            threadgroup float *srow = Sf + row * WM_ATTN_KB;

            float tile_max = -INFINITY;
            for (int c = 0; c < WM_ATTN_KB; c++) {
                const int j = j0 + c;
                bool live = (j < p.n_kv) && (i < p.n_q);
                if (live && p.causal && j > p.causal_offset + i) live = false;
                const float s = live ? srow[c] * p.scale : -INFINITY;
                srow[c] = s;
                tile_max = max(tile_max, s);
            }

            const float m_old = m_run[row];
            const float m_new = max(m_old, tile_max);
            const bool  has_m = (m_new > -INFINITY);
            /* Guarded: the first tile (and a fully masked one) leaves m_old
             * at -inf, where exp(-inf - -inf) is NaN rather than 0. */
            const float c_old = (m_old > -INFINITY && has_m) ? exp(m_old - m_new) : 0.0f;

            float sum = 0.0f;
            for (int c = 0; c < WM_ATTN_KB; c++) {
                const float pe = has_m ? exp(srow[c] - m_new) : 0.0f;
                srow[c] = pe;
                sum += pe;
            }

            m_run[row] = m_new;
            l_run[row] = l_run[row] * c_old + sum;
            corr[row]  = c_old;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        /* ---- rescale the running output onto the new maximum --------- */
        for (int idx = (int)tid; idx < WM_ATTN_QB * hd; idx += WM_ATTN_THREADS)
            Ot[idx] *= corr[idx / hd];
        threadgroup_barrier(mem_flags::mem_threadgroup);

        /* ---- O += P . V --------------------------------------------- */
        for (int dd = (int)sgid; dd < hdt; dd += WM_ATTN_SG) {
            for (int mm = 0; mm < WM_ATTN_QB / 8; mm++) {
                simdgroup_float8x8 om;
                simdgroup_load(om, Ot + mm * 8 * hd + dd * 8, hd);
                for (int jj = 0; jj < WM_ATTN_KB / 8; jj++) {
                    simdgroup_float8x8 pm, vm;
                    simdgroup_load(pm, Sf, WM_ATTN_KB, ulong2(jj * 8, mm * 8));
                    simdgroup_load(vm, vbase, ns, ulong2(dd * 8, jj * 8));
                    simdgroup_multiply_accumulate(om, pm, vm, om);
                }
                simdgroup_store(om, Ot + mm * 8 * hd + dd * 8, hd);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    for (int idx = (int)tid; idx < WM_ATTN_QB * hd; idx += WM_ATTN_THREADS) {
        const int row = idx / hd;
        const int col = idx % hd;
        const int i = q0 + row;
        if (i >= p.n_q) continue;
        const float l = l_run[row];
        out[(long)i * ns + (long)h * hd + col] = l > 0.0f ? Ot[idx] / l : 0.0f;
    }
}

/* ======================================================================
 * Elementwise helpers
 * =================================================================== */

kernel void k_add(device float *x [[buffer(0)]],
                  device const float *y [[buffer(1)]],
                  constant WmElem &p [[buffer(2)]],
                  uint gid [[thread_position_in_grid]]) {
    if ((int)gid < p.n) x[gid] += y[gid];
}

kernel void k_gelu(device float *x [[buffer(0)]],
                   constant WmElem &p [[buffer(1)]],
                   uint gid [[thread_position_in_grid]]) {
    if ((int)gid < p.n) x[gid] = wm_gelu(x[gid]);
}

/* conv2 output is channel-major [n_state][n_tokens]; the transformer wants
 * token-major [n_tokens][n_state]. Folding the positional embedding into the
 * same pass saves a full round trip over 7.7 MB. */
kernel void k_transpose_add_pos(device const float *in  [[buffer(0)]],
                                device const float *pos [[buffer(1)]],
                                device       float *out [[buffer(2)]],
                                constant WmTranspose &p [[buffer(3)]],
                                uint2 gid [[thread_position_in_grid]]) {
    const int t = (int)gid.x;
    const int c = (int)gid.y;
    if (t >= p.n_tokens || c >= p.n_state) return;
    out[(long)t * p.n_state + c] = in[(long)c * p.n_tokens + t] + pos[(long)t * p.n_state + c];
}

/* Token embedding lookup + learned positional embedding. The table stays
 * quantized: it is 51866 x 1280, and a decode step needs at most a handful
 * of its rows. */
kernel void k_embed(device const uchar *table [[buffer(0)]],
                    device const int   *ids   [[buffer(1)]],
                    device const float *pos   [[buffer(2)]],
                    device       float *out   [[buffer(3)]],
                    constant WmEmbed   &p     [[buffer(4)]],
                    uint2 gid [[thread_position_in_grid]]) {
    const int t = (int)gid.y;
    const int c = (int)gid.x;
    if (t >= p.n_tokens || c >= p.n_state) return;

    const int bb   = wm_block_bytes(p.dtype);
    const int nblk = p.n_state / 32;
    device const uchar *row = table + (long)ids[t] * nblk * bb;

    const float e = wm_dq_one(row + (long)(c / 32) * bb, p.dtype, c % 32);
    out[(long)t * p.n_state + c] = e + pos[(long)(p.n_past + t) * p.n_state + c];
}

/* Copy `n_tokens` rows of K/V into a cache at row `n_past`. */
kernel void k_kv_store(device const float *src [[buffer(0)]],
                       device       float *dst [[buffer(1)]],
                       constant WmEmbed   &p   [[buffer(2)]],
                       uint2 gid [[thread_position_in_grid]]) {
    const int t = (int)gid.y;
    const int c = (int)gid.x;
    if (t >= p.n_tokens || c >= p.n_state) return;
    dst[(long)(p.n_past + t) * p.n_state + c] = src[(long)t * p.n_state + c];
}
