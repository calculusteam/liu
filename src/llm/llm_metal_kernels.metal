/*
 * Liu - Metal compute kernels for the local LLM engine.
 *
 * Quantization portions derived from llama.cpp / ggml:
 * Copyright (c) 2023-2026 The ggml authors
 * These portions are licensed under the MIT License; see
 * packaging/licenses/llama.cpp-LICENSE.txt and THIRD_PARTY_NOTICES.md.
 *
 * Each kernel is a direct port of the verified CPU forward pass in
 * llm_model.c (which produces a correct Hy-MT translation). batch = 1,
 * single-token decode: every "matmul" is a matrix x vector. Weights stay
 * quantized in one big device buffer and are dequantized inside the
 * matvec kernels (one output row per thread).
 *
 * Buffer 0 is always the shared LlmMetalParams block. Weight tensors are
 * reached through the one `weights` buffer + params.w_offset (a byte
 * offset) — GGUF tensor offsets are not 256-aligned, so binding the
 * whole blob and indexing is safer than setBuffer:offset:.
 */
#include <metal_stdlib>
#include "llm_metal_params.h"     /* LlmMetalParams + MATVEC_* + LLM_METAL_MAX_CTX */
#include "llm_stq1_0_codebook.h"  /* STQ1_0_CODEBOOK_INIT */
using namespace metal;

#define LLM_INT_MAX 2147483647

/* STQ1_0 (sign<<4 | code) -> packed 4-lane pattern. Single source of
 * truth shared with the CPU path — see llm_stq1_0_codebook.h. */
constant uchar stq1_0_codebook[32] = STQ1_0_CODEBOOK_INIT;

/* Little-endian f16 load from an unaligned device byte pointer. */
static inline float load_f16(device const uchar *p) {
    ushort bits = (ushort)p[0] | ((ushort)p[1] << 8);
    return float(as_type<half>(bits));
}

/* ---- STQ1_0 rows: one 64-weight chunk per lane ----------------------
 *
 * The STQ1_0 matvecs are bound by the integer work of DECODING — not by
 * memory, and not by the number of dispatches: the same weight loads with
 * nothing done to them stream several times faster than the decode they
 * feed. The row kernels below are built on three things that follow from
 * it:
 *
 *   - A lane owns one CHUNK of its row: 64 weights, a quarter of a block —
 *     eight code bytes, two sign bytes and the block's scale. Its six
 *     16-bit loads depend on nothing, so every load of a row is asked for
 *     before the first barrier instead of a block at a time between uses.
 *   - The input is staged TRANSPOSED and already scaled: for micro-group
 *     gl of chunk c, xt[] holds the four activations its weights multiply
 *     as (x0, x1/4, x2/16, x3/64). The codebook byte keeps lane p's q in
 *     bits 2p..2p+1, so `pat & (3 << 2p)` IS q_p * 4^p: no weight is
 *     shifted into place, the power of two went into the activation
 *     (exactly), and a shift costs more than the FMA that absorbs it.
 *   - A weight is (q - 1) * d. The "- 1" of a whole micro-group is one
 *     subtraction of x0+x1+x2+x3, staged beside it in xs[]; d multiplies
 *     the chunk's sum once.
 *
 * xt/xs are indexed [gl * STQ_TILE_CHUNKS + chunk], so the 32 lanes of a
 * row read 32 neighbouring entries.
 */
#define STQ_CHUNK        64                           /* weights per lane   */
#define STQ_TILE_CHUNKS  (MATVEC_TILE / STQ_CHUNK)    /* a chunk per lane   */
#define STQ_XT_LEN       (16 * STQ_TILE_CHUNKS)
/* Tiles of k_matvec_stq1_0's row whose weights are loaded up front. Three
 * cover ffn_down (n_ff = 3 * n_embd); a wider row takes the rest a tile at
 * a time. */
#define STQ_FRONT_TILES  3
#if STQ_TILE_CHUNKS != 32
#error "a tile is one 64-weight chunk per lane of a 32-lane SIMD group"
#endif

struct StqChunk {
    uint  q0, q1;   /* sixteen 4-bit codes: micro-group gl at bit 4 * gl   */
    uint  s;        /* sixteen sign bits:   micro-group gl at bit gl       */
    float d;        /* the block's scale                                   */
};

static inline StqChunk stq_chunk_none() {
    StqChunk w;
    w.q0 = 0; w.q1 = 0; w.s = 0; w.d = 0.0f;
    return w;
}

/* Chunk `chunk` of the row at `row`. The words are read as 16-bit: the
 * host hands these kernels only tensors at a 4-byte offset (resolve() in
 * llm_metal.m), and a row, a block and a chunk each start an even number
 * of bytes in. Nothing here waits on the input. */
static inline StqChunk stq_chunk_load(device const uchar *row, uint chunk) {
    device const uchar *blk = row + (chunk >> 2) * 42;
    int k = (int)(chunk & 3u);
    device const ushort *q16 = (device const ushort *)(blk + 8 * k);
    uint a0 = q16[0], a1 = q16[1], a2 = q16[2], a3 = q16[3];
    StqChunk w;
    w.q0 = a0 | (a1 << 16);
    w.q1 = a2 | (a3 << 16);
    w.s  = *((device const ushort *)(blk + 32 + 2 * k));
    w.d  = float(as_type<half>(*((device const ushort *)(blk + 40))));
    return w;
}

/* The chunk's dot with the staged input; `chunk` is its index in the tile
 * (= the lane that owns it). */
static inline float stq_chunk_dot(StqChunk w, threadgroup const float4 *xt,
                                  threadgroup const float *xs, uint chunk) {
    float a = 0.0f;
    #pragma unroll
    for (int gl = 0; gl < 16; gl++) {
        float4 x  = xt[gl * STQ_TILE_CHUNKS + chunk];
        float  sx = xs[gl * STQ_TILE_CHUNKS + chunk];
        uint code = ((gl < 8 ? w.q0 : w.q1) >> (4 * (gl & 7))) & 0xFu;
        /* the group's sign bit, moved to bit 4 (the codebook's index) */
        uint sb = gl >= 4 ? ((w.s >> (gl - 4)) & 0x10u)
                          : ((w.s << (4 - gl)) & 0x10u);
        uint pat = (uint)stq1_0_codebook[code | sb];
        a = fma((float)(pat & 0x03u), x.x, a);
        a = fma((float)(pat & 0x0Cu), x.y, a);
        a = fma((float)(pat & 0x30u), x.z, a);
        a = fma((float)(pat & 0xC0u), x.w, a);
        a -= sx;
    }
    return w.d * a;
}

/* Stage `tlen` (<= MATVEC_TILE) input values at `src` into xt/xs. The
 * caller owns the barrier that follows. */
static inline void stq_stage(device const float *src, int tlen,
                             threadgroup float4 *xt, threadgroup float *xs,
                             uint tid, uint tgsize) {
    int nch = tlen / STQ_CHUNK;
    for (int i = (int)tid; i < nch * 16; i += (int)tgsize) {
        int ch = i >> 4, gl = i & 15;
        /* Stride-16 layout: group gl's four weights are at offsets gl,
         * gl+16, gl+32, and gl+48 in each 64-wide chunk. */
        device const float *v = src + ch * STQ_CHUNK + gl;
        float x0 = v[0], x1 = v[16], x2 = v[32], x3 = v[48];
        xt[gl * STQ_TILE_CHUNKS + ch] =
            float4(x0, x1 * 0.25f, x2 * 0.0625f, x3 * 0.015625f);
        xs[gl * STQ_TILE_CHUNKS + ch] = (x0 + x1) + (x2 + x3);
    }
}

/* ---- RMSNorm: one threadgroup, threadgroup-memory reduction --------- */
kernel void k_rmsnorm(
    constant LlmMetalParams &p   [[buffer(0)]],
    device const float      *in  [[buffer(1)]],
    device const uchar      *weights [[buffer(2)]],
    device float            *out [[buffer(3)]],
    uint tid    [[thread_index_in_threadgroup]],
    uint tgsize [[threads_per_threadgroup]])
{
    threadgroup float shared[256];
    int n = p.in_dim;

    float local = 0.0f;
    for (int i = (int)tid; i < n; i += (int)tgsize) local += in[i] * in[i];
    shared[tid] = local;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = tgsize / 2; s > 0; s >>= 1) {
        if (tid < s) shared[tid] += shared[tid + s];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    float scale = 1.0f / sqrt(shared[0] / (float)n + p.rms_eps);

    device const float *w = (device const float *)(weights + p.w_offset);
    for (int i = (int)tid; i < n; i += (int)tgsize) out[i] = in[i] * scale * w[i];
}

/* ---- matvec: out[r] = dot(dequant(W[r]), in) ------------------------
 *
 * Threadgroup-cooperative: one 32-lane SIMD group per output row, so the
 * 32 lanes of a row read *adjacent* weight bytes (coalesced) rather than
 * adjacent threads each striding to a far-apart row. The input vector is
 * staged once per threadgroup into `in_s` (threadgroup memory), tiled at
 * MATVEC_TILE so the scratch stays at 8 KB. Lanes split the per-block
 * micro-groups, accumulate a partial dot, and `simd_sum` folds the 32
 * partials. `p.accumulate` does the residual `out[r] += dot` in-kernel,
 * removing a separate add dispatch + a scratch-buffer round-trip.
 *
 * Threadgroup = MATVEC_TG (256) = 8 SIMD groups = 8 rows. Every thread
 * runs the tile loop (so all hit the same barriers); `simd_sum` is only
 * reached by active simdgroups, and `active` is uniform within a
 * simdgroup (its 32 lanes share the row r).
 *
 * That is k_matvec_q6k, k_lm_head_q6k_best and k_matvec_f32. The STQ1_0
 * row is decoded a chunk per lane (above): same threadgroup, same rows,
 * same barriers for every thread.
 */

static inline void matvec_write(device float *out, uint idx, float v,
                                int accumulate) {
    if (accumulate) out[idx] += v;
    else            out[idx]  = v;
}

kernel void k_matvec_stq1_0(
    constant LlmMetalParams &p   [[buffer(0)]],
    device const float      *in  [[buffer(1)]],
    device const uchar      *weights [[buffer(2)]],
    device float            *out [[buffer(3)]],
    uint tgid      [[threadgroup_position_in_grid]],
    uint tid       [[thread_index_in_threadgroup]],
    uint tgsize    [[threads_per_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]],
    uint simd_gid  [[simdgroup_index_in_threadgroup]])
{
    threadgroup float4 xt[STQ_XT_LEN];
    threadgroup float  xs[STQ_XT_LEN];
    int in_dim = p.in_dim;
    int nchunks = in_dim / STQ_CHUNK;
    uint r = tgid * (tgsize / 32) + simd_gid;
    /* A SIMD group past the last row runs every barrier like the rest: it
     * reads the last row and writes nothing. */
    uint rr = min(r, (uint)(p.out_dim - 1));
    device const uchar *row = weights + p.w_offset + rr * (uint)((in_dim / 256) * 42);

    /* Every weight load of the first tiles leaves before the first barrier:
     * lane l owns chunk l of each tile. */
    StqChunk w[STQ_FRONT_TILES];
    #pragma unroll
    for (int t = 0; t < STQ_FRONT_TILES; t++) {
        uint c = simd_lane + (uint)(t * STQ_TILE_CHUNKS);
        if ((int)c < nchunks) w[t] = stq_chunk_load(row, c);
        else                  w[t] = stq_chunk_none();
    }

    float acc = 0.0f;
    #pragma unroll
    for (int t = 0; t < STQ_FRONT_TILES; t++) {
        int tile = t * MATVEC_TILE;
        if (tile >= in_dim) break;
        int tlen = min(MATVEC_TILE, in_dim - tile);
        stq_stage(in + tile, tlen, xt, xs, tid, tgsize);
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if ((int)simd_lane < tlen / STQ_CHUNK) acc += stq_chunk_dot(w[t], xt, xs, simd_lane);
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    /* A row wider than the tiles loaded up front: the same decode, each
     * further tile's chunk asked for when its turn comes. */
    for (int tile = STQ_FRONT_TILES * MATVEC_TILE; tile < in_dim; tile += MATVEC_TILE) {
        int tlen = min(MATVEC_TILE, in_dim - tile);
        bool mine = (int)simd_lane < tlen / STQ_CHUNK;
        StqChunk wt = stq_chunk_none();
        if (mine) wt = stq_chunk_load(row, simd_lane + (uint)(tile / STQ_CHUNK));
        stq_stage(in + tile, tlen, xt, xs, tid, tgsize);
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (mine) acc += stq_chunk_dot(wt, xt, xs, simd_lane);
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    float row_sum = simd_sum(acc);
    if (simd_lane == 0 && (int)r < p.out_dim)
        matvec_write(out, p.out_offset + r, row_sum, p.accumulate);
}

kernel void k_matvec_q6k(
    constant LlmMetalParams &p   [[buffer(0)]],
    device const float      *in  [[buffer(1)]],
    device const uchar      *weights [[buffer(2)]],
    device float            *out [[buffer(3)]],
    uint tgid      [[threadgroup_position_in_grid]],
    uint tid       [[thread_index_in_threadgroup]],
    uint tgsize    [[threads_per_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]],
    uint simd_gid  [[simdgroup_index_in_threadgroup]])
{
    threadgroup float in_s[MATVEC_TILE];
    int in_dim = p.in_dim;
    int nblocks_total = in_dim / 256;
    uint r = tgid * (tgsize / 32) + simd_gid;
    bool active = ((int)r < p.out_dim);
    device const uchar *row = weights + p.w_offset + (uint)r * (uint)(nblocks_total * 210);

    float acc = 0.0f;
    for (int tile = 0; tile < in_dim; tile += MATVEC_TILE) {
        int tlen = min(MATVEC_TILE, in_dim - tile);
        for (int i = (int)tid; i < tlen; i += (int)tgsize) in_s[i] = in[tile + i];
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (active) {
            int b0 = tile / 256;
            int nbt = tlen / 256;
            for (int bb = 0; bb < nbt; bb++) {
                device const uchar *blk = row + (b0 + bb) * 210;
                device const uchar *ql = blk;
                device const uchar *qh = blk + 128;
                device const char  *sc = (device const char *)(blk + 192);
                float d = load_f16(blk + 208);
                threadgroup const float *iv = in_s + bb * 256;
                for (int idx = (int)simd_lane; idx < 64; idx += 32) {
                    int half_i = idx >> 5;       /* 0 or 1 */
                    int l      = idx & 31;       /* 0..31  */
                    device const uchar *qlh = ql + half_i * 64;
                    device const uchar *qhh = qh + half_i * 32;
                    device const char  *sch = sc + half_i * 8;
                    threadgroup const float *ivh = iv + half_i * 128;
                    int is = l / 16;
                    int q1 = (int)((qlh[l]      & 0x0F) | (((qhh[l] >> 0) & 3) << 4)) - 32;
                    int q2 = (int)((qlh[l + 32] & 0x0F) | (((qhh[l] >> 2) & 3) << 4)) - 32;
                    int q3 = (int)((qlh[l]      >>   4) | (((qhh[l] >> 4) & 3) << 4)) - 32;
                    int q4 = (int)((qlh[l + 32] >>   4) | (((qhh[l] >> 6) & 3) << 4)) - 32;
                    acc += d * (float)sch[is + 0] * (float)q1 * ivh[l];
                    acc += d * (float)sch[is + 2] * (float)q2 * ivh[l + 32];
                    acc += d * (float)sch[is + 4] * (float)q3 * ivh[l + 64];
                    acc += d * (float)sch[is + 6] * (float)q4 * ivh[l + 96];
                }
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (active) {
        float row_sum = simd_sum(acc);
        if (simd_lane == 0) matvec_write(out, p.out_offset + r, row_sum, p.accumulate);
    }
}

kernel void k_lm_head_q6k_best(
    constant LlmMetalParams &p   [[buffer(0)]],
    device const float      *in  [[buffer(1)]],
    device const uchar      *weights [[buffer(2)]],
    device float            *best_vals [[buffer(3)]],
    device int              *best_idx  [[buffer(4)]],
    uint tgid      [[threadgroup_position_in_grid]],
    uint tid       [[thread_index_in_threadgroup]],
    uint tgsize    [[threads_per_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]],
    uint simd_gid  [[simdgroup_index_in_threadgroup]])
{
    threadgroup float in_s[MATVEC_TILE];
    threadgroup float tg_bestv[MATVEC_SIMDS];
    threadgroup int   tg_besti[MATVEC_SIMDS];
    int in_dim = p.in_dim;
    int nblocks_total = in_dim / 256;
    uint r = tgid * (tgsize / 32) + simd_gid;
    bool active = ((int)r < p.out_dim);
    device const uchar *row = weights + p.w_offset + (uint)r * (uint)(nblocks_total * 210);

    float acc = 0.0f;
    for (int tile = 0; tile < in_dim; tile += MATVEC_TILE) {
        int tlen = min(MATVEC_TILE, in_dim - tile);
        for (int i = (int)tid; i < tlen; i += (int)tgsize) in_s[i] = in[tile + i];
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (active) {
            int b0 = tile / 256;
            int nbt = tlen / 256;
            for (int bb = 0; bb < nbt; bb++) {
                device const uchar *blk = row + (b0 + bb) * 210;
                device const uchar *ql = blk;
                device const uchar *qh = blk + 128;
                device const char  *sc = (device const char *)(blk + 192);
                float d = load_f16(blk + 208);
                threadgroup const float *iv = in_s + bb * 256;
                for (int idx = (int)simd_lane; idx < 64; idx += 32) {
                    int half_i = idx >> 5;
                    int l      = idx & 31;
                    device const uchar *qlh = ql + half_i * 64;
                    device const uchar *qhh = qh + half_i * 32;
                    device const char  *sch = sc + half_i * 8;
                    threadgroup const float *ivh = iv + half_i * 128;
                    int is = l / 16;
                    int q1 = (int)((qlh[l]      & 0x0F) | (((qhh[l] >> 0) & 3) << 4)) - 32;
                    int q2 = (int)((qlh[l + 32] & 0x0F) | (((qhh[l] >> 2) & 3) << 4)) - 32;
                    int q3 = (int)((qlh[l]      >>   4) | (((qhh[l] >> 4) & 3) << 4)) - 32;
                    int q4 = (int)((qlh[l + 32] >>   4) | (((qhh[l] >> 6) & 3) << 4)) - 32;
                    acc += d * (float)sch[is + 0] * (float)q1 * ivh[l];
                    acc += d * (float)sch[is + 2] * (float)q2 * ivh[l + 32];
                    acc += d * (float)sch[is + 4] * (float)q3 * ivh[l + 64];
                    acc += d * (float)sch[is + 6] * (float)q4 * ivh[l + 96];
                }
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    float row_sum = active ? simd_sum(acc) : -INFINITY;
    if (simd_lane == 0) {
        tg_bestv[simd_gid] = row_sum;
        tg_besti[simd_gid] = active ? (int)r : LLM_INT_MAX;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (tid == 0) {
        float bv = tg_bestv[0];
        int bi = tg_besti[0];
        for (uint i = 1; i < tgsize / 32; i++) {
            float ov = tg_bestv[i];
            int oi = tg_besti[i];
            if (ov > bv || (ov == bv && oi < bi)) {
                bv = ov; bi = oi;
            }
        }
        best_vals[tgid] = bv;
        best_idx[tgid] = bi;
    }
}

kernel void k_matvec_f32(
    constant LlmMetalParams &p   [[buffer(0)]],
    device const float      *in  [[buffer(1)]],
    device const uchar      *weights [[buffer(2)]],
    device float            *out [[buffer(3)]],
    uint tgid      [[threadgroup_position_in_grid]],
    uint tid       [[thread_index_in_threadgroup]],
    uint tgsize    [[threads_per_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]],
    uint simd_gid  [[simdgroup_index_in_threadgroup]])
{
    threadgroup float in_s[MATVEC_TILE];
    int in_dim = p.in_dim;
    uint r = tgid * (tgsize / 32) + simd_gid;
    bool active = ((int)r < p.out_dim);
    device const float *row =
        (device const float *)(weights + p.w_offset) + (uint)r * (uint)in_dim;

    float acc = 0.0f;
    for (int tile = 0; tile < in_dim; tile += MATVEC_TILE) {
        int tlen = min(MATVEC_TILE, in_dim - tile);
        for (int i = (int)tid; i < tlen; i += (int)tgsize) in_s[i] = in[tile + i];
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (active) {
            for (int i = (int)simd_lane; i < tlen; i += 32)
                acc += row[tile + i] * in_s[i];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (active) {
        float row_sum = simd_sum(acc);
        if (simd_lane == 0) matvec_write(out, p.out_offset + r, row_sum, p.accumulate);
    }
}

kernel void k_argmax_partials(
    constant LlmMetalParams &p [[buffer(0)]],
    device const float *best_vals [[buffer(1)]],
    device const int   *best_idx  [[buffer(2)]],
    device int         *tokens    [[buffer(3)]],
    uint tid    [[thread_index_in_threadgroup]],
    uint tgsize [[threads_per_threadgroup]])
{
    threadgroup float redv[256];
    threadgroup int   redi[256];
    int n = p.out_dim;
    float bv = -INFINITY;
    int bi = LLM_INT_MAX;
    for (int i = (int)tid; i < n; i += (int)tgsize) {
        float ov = best_vals[i];
        int oi = best_idx[i];
        if (ov > bv || (ov == bv && oi < bi)) {
            bv = ov; bi = oi;
        }
    }
    redv[tid] = bv;
    redi[tid] = bi;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = tgsize / 2; s > 0; s >>= 1) {
        if (tid < s) {
            float ov = redv[tid + s];
            int oi = redi[tid + s];
            if (ov > redv[tid] || (ov == redv[tid] && oi < redi[tid])) {
                redv[tid] = ov;
                redi[tid] = oi;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid == 0 && p.write_token) {
        /* `n` (= p.out_dim) is the number of LM-head PARTIALS (bestCount ≈
         * n_vocab/8) — the correct REDUCTION bound used above. But `t` is a
         * GLOBAL vocab id in [0, n_vocab), so the CLAMP must bound by n_vocab,
         * not bestCount: a clamp to `n` would rewrite every winning id >=
         * bestCount (most of the vocab, INCLUDING the EOS id) to token 0, and
         * generation would never match the stop check. The genuine all-NaN
         * sentinel (LLM_INT_MAX) is still > n_vocab so a true degenerate step
         * still falls back to token 0. */
        int t = redi[0];
        tokens[p.pos + 1] = (t >= 0 && t < p.n_vocab) ? t : 0;
    }
}

/* ---- fused mega-matvecs --------------------------------------------
 *
 * k_qkv and k_gate_up each do the layer's RMSNorm once (cooperatively,
 * threadgroup-shared) and then several STQ1_0 projections from that one
 * normed input — collapsing 4 dispatches into 1. They require the input
 * (n_embd) to fit in one MATVEC_TILE and all projection weights to be
 * STQ1_0; the host checks both at create time and falls back to the CPU
 * engine otherwise. Threadgroup = MATVEC_TG (256) = MATVEC_SIMDS (8)
 * SIMD groups, one output row per SIMD group.
 */

/* Cooperative fused RMSNorm for the mega-matvecs: the sum of squares over
 * `in`, then `in * 1/rms * norm_weight` staged straight into xt/xs the way
 * stq_chunk_dot reads it. `norm_w_off` is the F32 norm tensor's byte
 * offset; n <= MATVEC_TILE. All threadgroup threads must call this (it has
 * barriers, and ends on one). */
static inline void stq_stage_rmsnorm(constant LlmMetalParams &p,
                                     device const float *in,
                                     device const uchar *weights,
                                     uint norm_w_off, int n,
                                     threadgroup float4 *xt,
                                     threadgroup float *xs,
                                     threadgroup float *red,
                                     uint tid, uint tgsize,
                                     uint simd_lane, uint simd_gid) {
    float ss = 0.0f;
    for (int i = (int)tid; i < n; i += (int)tgsize) {
        float x = in[i];
        ss += x * x;
    }
    ss = simd_sum(ss);
    if (simd_lane == 0) red[simd_gid] = ss;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float total_ss = 0.0f;
    for (uint i = 0; i < tgsize / 32; i++) total_ss += red[i];
    float rms_scale = 1.0f / sqrt(total_ss / (float)n + p.rms_eps);
    device const float *nw = (device const float *)(weights + norm_w_off);
    int nch = n / STQ_CHUNK;
    for (int i = (int)tid; i < nch * 16; i += (int)tgsize) {
        int ch = i >> 4, gl = i & 15;
        int b = ch * STQ_CHUNK + gl;
        float x0 = in[b]      * rms_scale * nw[b];
        float x1 = in[b + 16] * rms_scale * nw[b + 16];
        float x2 = in[b + 32] * rms_scale * nw[b + 32];
        float x3 = in[b + 48] * rms_scale * nw[b + 48];
        xt[gl * STQ_TILE_CHUNKS + ch] =
            float4(x0, x1 * 0.25f, x2 * 0.0625f, x3 * 0.015625f);
        xs[gl * STQ_TILE_CHUNKS + ch] = (x0 + x1) + (x2 + x3);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
}

/* attn RMSNorm + Q/K/V projection. Output rows are laid out
 * [Q rows | K rows | V rows]; each SIMD group resolves its row to a
 * tensor + output buffer. */
kernel void k_qkv(
    constant LlmMetalParams &p   [[buffer(0)]],
    device const float      *in  [[buffer(1)]],
    device const uchar      *weights [[buffer(2)]],
    device float            *outQ [[buffer(3)]],
    device float            *outK [[buffer(4)]],
    device float            *outV [[buffer(5)]],
    uint tgid      [[threadgroup_position_in_grid]],
    uint tid       [[thread_index_in_threadgroup]],
    uint tgsize    [[threads_per_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]],
    uint simd_gid  [[simdgroup_index_in_threadgroup]])
{
    threadgroup float4 xt[STQ_XT_LEN];
    threadgroup float  xs[STQ_XT_LEN];
    threadgroup float  red[MATVEC_SIMDS];
    int n      = p.in_dim;                       /* n_embd, <= MATVEC_TILE */
    int q_dim  = p.n_head * p.head_dim;
    int kv_dim = p.n_head_kv * p.head_dim;
    int total  = q_dim + 2 * kv_dim;
    int nch    = n / STQ_CHUNK;

    uint r = tgid * (tgsize / 32) + simd_gid;
    bool active = (int)r < total;
    bool mine   = active && (int)simd_lane < nch;

    uint w_off = p.w_offset1; uint local_row = r; device float *out = outQ;
    if ((int)r >= q_dim + kv_dim) {
        w_off = p.w_offset3; local_row = r - (uint)(q_dim + kv_dim); out = outV;
    } else if ((int)r >= q_dim) {
        w_off = p.w_offset2; local_row = r - (uint)q_dim;            out = outK;
    }
    /* The row's weights first: they do not wait on the norm. */
    StqChunk w = stq_chunk_none();
    if (mine) w = stq_chunk_load(weights + w_off + local_row * (uint)((n / 256) * 42),
                                 simd_lane);

    stq_stage_rmsnorm(p, in, weights, p.w_offset, n, xt, xs, red,
                      tid, tgsize, simd_lane, simd_gid);
    if (!active) return;

    float row_sum = simd_sum(mine ? stq_chunk_dot(w, xt, xs, simd_lane) : 0.0f);
    if (simd_lane == 0) out[local_row] = row_sum;
}

/* ffn RMSNorm + gate/up projection + SwiGLU. Each SIMD group computes
 * one row of both gate and up from the shared normed input, then writes
 * silu(gate) * up — collapsing rmsnorm + 2 matvecs + swiglu into one. */
kernel void k_gate_up(
    constant LlmMetalParams &p   [[buffer(0)]],
    device const float      *in  [[buffer(1)]],
    device const uchar      *weights [[buffer(2)]],
    device float            *outG [[buffer(3)]],
    uint tgid      [[threadgroup_position_in_grid]],
    uint tid       [[thread_index_in_threadgroup]],
    uint tgsize    [[threads_per_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]],
    uint simd_gid  [[simdgroup_index_in_threadgroup]])
{
    threadgroup float4 xt[STQ_XT_LEN];
    threadgroup float  xs[STQ_XT_LEN];
    threadgroup float  red[MATVEC_SIMDS];
    int n    = p.in_dim;                         /* n_embd, <= MATVEC_TILE */
    int n_ff = p.n_ff;
    int nch  = n / STQ_CHUNK;

    uint r = tgid * (tgsize / 32) + simd_gid;
    bool active = (int)r < n_ff;
    bool mine   = active && (int)simd_lane < nch;

    StqChunk wg = stq_chunk_none(), wu = stq_chunk_none();
    if (mine) {
        uint row_off = r * (uint)((n / 256) * 42);
        wg = stq_chunk_load(weights + p.w_offset1 + row_off, simd_lane);
        wu = stq_chunk_load(weights + p.w_offset2 + row_off, simd_lane);
    }

    stq_stage_rmsnorm(p, in, weights, p.w_offset, n, xt, xs, red,
                      tid, tgsize, simd_lane, simd_gid);
    if (!active) return;

    float pg = 0.0f, pu = 0.0f;
    if (mine) {
        pg = stq_chunk_dot(wg, xt, xs, simd_lane);
        pu = stq_chunk_dot(wu, xt, xs, simd_lane);
    }
    float g = simd_sum(pg);
    float u = simd_sum(pu);
    if (simd_lane == 0) {
        float silu = g / (1.0f + exp(-g));
        outG[r] = silu * u;
    }
}

/* ---- fused attention block -----------------------------------------
 *
 * k_kv_prep and k_attn_block collapse the 6 standalone attention-path
 * dispatches (RoPE x2, QK-norm x2, store_kv, attention) into 2. Both run
 * one threadgroup per head. k_kv_prep prepares the KV heads (RoPE +
 * QK-norm + write to the cache); k_attn_block prepares each query head
 * and runs the attention over the prepared cache.
 */

/* RoPE one head's vector held in threadgroup memory. All threadgroup
 * threads must call (it ends with a barrier). */
static inline void rope_head_tg(threadgroup float *v, int hd, int pos,
                                device const float2 *rope, int neox,
                                uint tid, uint tgsize) {
    int half_d = hd / 2;
    for (int i = (int)tid; i < half_d; i += (int)tgsize) {
        float2 cs = rope[(ulong)pos * (ulong)half_d + (ulong)i];
        float c = cs.x, s = cs.y;
        if (neox) {
            float x0 = v[i], x1 = v[i + half_d];
            v[i]          = x0 * c - x1 * s;
            v[i + half_d] = x0 * s + x1 * c;
        } else {
            float x0 = v[2 * i], x1 = v[2 * i + 1];
            v[2 * i]     = x0 * c - x1 * s;
            v[2 * i + 1] = x0 * s + x1 * c;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
}

/* Per-head RMSNorm of a head vector in threadgroup memory `v`, weight
 * `w` (F32, head_dim long). `red` is threadgroup scratch >= tgsize. All
 * threadgroup threads must call (it ends with a barrier). */
static inline void qknorm_head_tg(threadgroup float *v, device const float *w,
                                  int hd, float eps, threadgroup float *red,
                                  uint tid, uint tgsize) {
    float ss = 0.0f;
    for (int i = (int)tid; i < hd; i += (int)tgsize) ss += v[i] * v[i];
    red[tid] = ss;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = tgsize / 2; s > 0; s >>= 1) {
        if (tid < s) red[tid] += red[tid + s];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    float scale = 1.0f / sqrt(red[0] / (float)hd + eps);
    for (int i = (int)tid; i < hd; i += (int)tgsize) v[i] = v[i] * scale * w[i];
    threadgroup_barrier(mem_flags::mem_threadgroup);
}

/* Prepare one KV head: RoPE + optional QK-norm of K, then write the
 * rotated/normed K and the raw V into the cache at (layer, pos). */
kernel void k_kv_prep(
    constant LlmMetalParams &p [[buffer(0)]],
    device const float *kc [[buffer(1)]],       /* K projections (kv_dim) */
    device const float *vc [[buffer(2)]],       /* V projections (kv_dim) */
    device const uchar *weights [[buffer(3)]],  /* k_norm weight @ w_offset */
    device float       *kcache [[buffer(4)]],
    device float       *vcache [[buffer(5)]],
    device const float2 *rope [[buffer(6)]],
    uint kvh    [[threadgroup_position_in_grid]],
    uint tid    [[thread_index_in_threadgroup]],
    uint tgsize [[threads_per_threadgroup]])
{
    threadgroup float ks[256];
    threadgroup float red[256];
    int hd = p.head_dim;

    device const float *kh = kc + kvh * hd;
    for (int i = (int)tid; i < hd; i += (int)tgsize) ks[i] = kh[i];
    threadgroup_barrier(mem_flags::mem_threadgroup);

    rope_head_tg(ks, hd, p.pos, rope, p.rope_neox, tid, tgsize);
    if (p.qk_norm) {
        device const float *w = (device const float *)(weights + p.w_offset);
        qknorm_head_tg(ks, w, hd, p.rms_eps, red, tid, tgsize);
    }

    int kv_dim = p.n_head_kv * hd;
    ulong slot = ((ulong)p.layer * p.n_ctx + p.pos) * (ulong)kv_dim
               + (ulong)kvh * hd;
    device const float *vh = vc + kvh * hd;
    for (int i = (int)tid; i < hd; i += (int)tgsize) {
        kcache[slot + i] = ks[i];
        vcache[slot + i] = vh[i];
    }
}

/* Prepare one query head (RoPE + optional QK-norm) and run the causal
 * attention over the KV cache prepared by k_kv_prep.
 *
 * One threadgroup of ATTN_TG threads per query head. A SIMD group owns a
 * POSITION at a time: its lanes read that position's K row as float4s —
 * neighbouring lanes, neighbouring bytes — and fold the dot with simd_sum,
 * ATTN_UNROLL positions in flight; SIMD group sg takes positions sg, sg +
 * nsg, sg + 2 nsg, … The same split carries the values: a lane keeps a
 * float4 column of the weighted V sum over its group's positions, and the
 * groups' partials are added at the end. Reads of the cache stay coalesced
 * this way; a thread walking a whole K row alone, then a V loop per output
 * element, would read it uncoalesced.
 *
 * head_dim is a multiple of 4 and at most LLM_METAL_MAX_HEAD_DIM — a lane
 * holds columns `lane` and `lane + 32` — and the threadgroup is at most
 * ATTN_TG wide; the host checks all three. */
#define ATTN_UNROLL 4
kernel void k_attn_block(
    constant LlmMetalParams &p [[buffer(0)]],
    device const float *q       [[buffer(1)]],   /* Q projections (q_dim)  */
    device const uchar *weights [[buffer(2)]],   /* q_norm weight @ w_offset */
    device const float *kcache  [[buffer(3)]],
    device const float *vcache  [[buffer(4)]],
    device float       *attn_out[[buffer(5)]],
    device const float2 *rope   [[buffer(6)]],
    uint hh     [[threadgroup_position_in_grid]],
    uint tid    [[thread_index_in_threadgroup]],
    uint tgsize [[threads_per_threadgroup]],
    uint lane   [[thread_index_in_simdgroup]],
    uint sg     [[simdgroup_index_in_threadgroup]])
{
    /* 27 KB of the 32 KB a threadgroup has: `part` is most of it. */
    threadgroup float4 qs4[LLM_METAL_MAX_HEAD_DIM / 4];
    threadgroup float  scores[LLM_METAL_MAX_CTX];
    threadgroup float  red[ATTN_TG];
    threadgroup float4 part[2][ATTN_SIMDS][32];
    threadgroup float  redg[ATTN_SIMDS];
    threadgroup float *qs = (threadgroup float *)qs4;

    int hd     = p.head_dim;
    int hd4    = hd / 4;
    int group  = p.n_head / p.n_head_kv;
    int kvh    = (int)hh / group;
    int npos   = p.pos + 1;
    int kv_dim = p.n_head_kv * hd;
    int nsg    = (int)(tgsize / 32);
    ulong layer_base = (ulong)p.layer * p.n_ctx * kv_dim;

    device const float *qh = q + hh * hd;
    for (int i = (int)tid; i < hd; i += (int)tgsize) qs[i] = qh[i];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    rope_head_tg(qs, hd, p.pos, rope, p.rope_neox, tid, tgsize);
    if (p.qk_norm) {
        device const float *w = (device const float *)(weights + p.w_offset);
        qknorm_head_tg(qs, w, hd, p.rms_eps, red, tid, tgsize);
    }
    bool has0 = (int)lane < hd4;
    bool has1 = (int)lane + 32 < hd4;
    float4 q0 = has0 ? qs4[lane]      : float4(0.0f);
    float4 q1 = has1 ? qs4[lane + 32] : float4(0.0f);

    device const float *kbase = kcache + layer_base + kvh * hd;
    device const float *vbase = vcache + layer_base + kvh * hd;

    /* scores[pp] = dot(qs, k[pp]) * scale */
    for (int pb = (int)sg; pb < npos; pb += nsg * ATTN_UNROLL) {
        float s[ATTN_UNROLL];
        #pragma unroll
        for (int u = 0; u < ATTN_UNROLL; u++) {
            int pp = pb + u * nsg;
            float v = 0.0f;
            if (pp < npos) {
                device const float4 *kp =
                    (device const float4 *)(kbase + (ulong)pp * kv_dim);
                if (has0) v  = dot(q0, kp[lane]);
                if (has1) v += dot(q1, kp[lane + 32]);
            }
            s[u] = v;
        }
        #pragma unroll
        for (int u = 0; u < ATTN_UNROLL; u++) {
            float v = simd_sum(s[u]);
            int pp = pb + u * nsg;
            if (lane == 0 && pp < npos) scores[pp] = v * p.attn_scale;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* softmax — max reduce */
    float lmax = -INFINITY;
    for (int pp = (int)tid; pp < npos; pp += (int)tgsize) lmax = max(lmax, scores[pp]);
    lmax = simd_max(lmax);
    if (lane == 0) redg[sg] = lmax;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float mx = redg[0];
    for (int i = 1; i < nsg; i++) mx = max(mx, redg[i]);
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* exp + sum reduce */
    float lsum = 0.0f;
    for (int pp = (int)tid; pp < npos; pp += (int)tgsize) {
        float e = exp(scores[pp] - mx);
        scores[pp] = e;
        lsum += e;
    }
    lsum = simd_sum(lsum);
    if (lane == 0) redg[sg] = lsum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float tot = 0.0f;
    for (int i = 0; i < nsg; i++) tot += redg[i];
    float inv = tot > 0.0f ? 1.0f / tot : 0.0f;

    /* attn_out[d] = sum_pp softmax[pp] * v[pp][d] */
    float4 a0 = float4(0.0f), a1 = float4(0.0f);
    for (int pb = (int)sg; pb < npos; pb += nsg * ATTN_UNROLL) {
        #pragma unroll
        for (int u = 0; u < ATTN_UNROLL; u++) {
            int pp = pb + u * nsg;
            if (pp < npos) {
                float wgt = scores[pp] * inv;
                device const float4 *vp =
                    (device const float4 *)(vbase + (ulong)pp * kv_dim);
                if (has0) a0 += wgt * vp[lane];
                if (has1) a1 += wgt * vp[lane + 32];
            }
        }
    }
    part[0][sg][lane] = a0;
    part[1][sg][lane] = a1;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if ((int)tid < hd4) {
        float4 acc = float4(0.0f);
        for (int i = 0; i < nsg; i++) acc += part[tid >> 5][i][tid & 31];
        device float4 *o4 = (device float4 *)(attn_out + hh * hd);
        o4[tid] = acc;
    }
}

/* ---- embedding + argmax (keep the per-token loop on the GPU) -------- */

/* Per-element Q6_K dequant within one 256-weight block (mirrors the
 * block layout in llm_quant_q6k.c). */
static inline float q6k_elem(device const uchar *blk, int j) {
    float d = load_f16(blk + 208);
    device const uchar *ql = blk;
    device const uchar *qh = blk + 128;
    device const char  *sc = (device const char *)(blk + 192);
    int half_i = j >> 7;          /* j / 128            */
    int jj = j & 127;
    int l  = jj & 31;
    int quad = jj >> 5;
    device const uchar *qlh = ql + half_i * 64;
    device const uchar *qhh = qh + half_i * 32;
    device const char  *sch = sc + half_i * 8;
    int is = l >> 4;
    int q, sci;
    if (quad == 0)      { q = (int)((qlh[l]      & 0x0F) | (((qhh[l] >> 0) & 3) << 4)) - 32; sci = is + 0; }
    else if (quad == 1) { q = (int)((qlh[l + 32] & 0x0F) | (((qhh[l] >> 2) & 3) << 4)) - 32; sci = is + 2; }
    else if (quad == 2) { q = (int)((qlh[l]      >>   4) | (((qhh[l] >> 4) & 3) << 4)) - 32; sci = is + 4; }
    else                { q = (int)((qlh[l + 32] >>   4) | (((qhh[l] >> 6) & 3) << 4)) - 32; sci = is + 6; }
    return d * (float)sch[sci] * (float)q;
}

/* Per-element STQ1_0 dequant within one 256-weight block. */
static inline float stq1_0_elem(device const uchar *blk, int j) {
    device const uchar *qs  = blk;
    device const uchar *sgn = blk + 32;
    float d = load_f16(blk + 40);
    /* Each 64-wide tile has 16 groups; a group's lanes are 16 apart. */
    int g = (j >> 6) * 16 + (j & 15);
    int lane = (j >> 4) & 3;
    uint code  = (qs[g >> 1] >> (4 * (g & 1))) & 0x0F;
    uint s     = (sgn[g >> 3] >> (g & 7)) & 0x01;
    uint qpack = stq1_0_codebook[(s << 4) | code];
    return (float)((int)((qpack >> (2 * lane)) & 3) - 1) * d;
}

/* Embedding lookup: dequantize token_embd row tokens[p.pos] into `out`. */
kernel void k_embed(
    constant LlmMetalParams &p [[buffer(0)]],
    device const uchar *weights [[buffer(1)]],
    device const int   *tokens  [[buffer(2)]],
    device float       *out     [[buffer(3)]],
    uint tid [[thread_position_in_grid]])
{
    int n = p.in_dim;
    if ((int)tid >= n) return;
    int tok = tokens[p.pos];
    int j = (int)tid;
    if (p.embed_dtype == LLM_DT_Q6_K) {
        device const uchar *row = weights + p.w_offset
                                + (uint)tok * (uint)((n / 256) * 210);
        out[j] = q6k_elem(row + (j / 256) * 210, j % 256);
    } else if (p.embed_dtype == LLM_DT_STQ1_0) {
        device const uchar *row = weights + p.w_offset
                                + (uint)tok * (uint)((n / 256) * 42);
        out[j] = stq1_0_elem(row + (j / 256) * 42, j % 256);
    } else { /* LLM_DT_F32 */
        device const float *row =
            (device const float *)(weights + p.w_offset) + (uint)tok * n;
        out[j] = row[j];
    }
}

/* Greedy argmax of `logits` (n_vocab). When write_token is set, the
 * winning index is written to tokens[p.pos + 1] so the next decode step
 * can read its input without a CPU round-trip. Lowest index wins ties,
 * matching the CPU sampler. One threadgroup, threadgroup reduction. */
kernel void k_argmax(
    constant LlmMetalParams &p [[buffer(0)]],
    device const float *logits [[buffer(1)]],
    device int         *tokens [[buffer(2)]],
    uint tid    [[thread_index_in_threadgroup]],
    uint tgsize [[threads_per_threadgroup]])
{
    threadgroup float bestv[256];
    threadgroup int   besti[256];
    int n = p.n_vocab;
    float bv = -INFINITY;
    int   bi = 0;
    for (int i = (int)tid; i < n; i += (int)tgsize) {
        float v = logits[i];
        if (v > bv) { bv = v; bi = i; }
    }
    bestv[tid] = bv;
    besti[tid] = bi;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = tgsize / 2; s > 0; s >>= 1) {
        if (tid < s) {
            float ov = bestv[tid + s];
            int   oi = besti[tid + s];
            if (ov > bestv[tid] || (ov == bestv[tid] && oi < besti[tid])) {
                bestv[tid] = ov;
                besti[tid] = oi;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid == 0 && p.write_token) tokens[p.pos + 1] = besti[0];
}

/* ---- batched prefill ------------------------------------------------
 *
 * A prompt run through the layers one position at a time streams all of a
 * layer's weights through a matvec for every position. The batched path
 * takes PREFILL_CHUNK positions at once: the projections are GEMMs that
 * decode a weight tile once for GEMM_TOKS tokens, and only the per-position
 * steps (RoPE + the KV write, the causal attention) still run a position at
 * a time, through the decode path's own kernels.
 *
 * k_gemm_stq1_0: Y[t][n] (=|+=) dot(W[n], X[t]) for t < n_tok, n < out_dim.
 * A threadgroup of GEMM_TG threads owns GEMM_ROWS rows x GEMM_TOKS tokens;
 * eight SIMD groups each multiply a 32-row x 16-token block of it in 8x8
 * fp32 tiles (fp32 runs at the fp16 rate here, so no precision is traded).
 * K goes in 32-wide slices: a thread decodes its row's codebook patterns
 * for a 64-weight chunk once (the decode of stq_chunk_dot) and writes them,
 * scale folded in, as the chunk's two slices come round; the tokens' inputs
 * of the slice are staged beside them. The sums are taken in another order
 * than the matvec's, so the result can differ from it in the last bits. The
 * finished dot is written, or added to Y, a value at a time, so the
 * residual gets `out += dot` as the matvec gives it, and a row or token
 * past the end is simply not written.
 *
 * Fewer SIMD groups with larger blocks need more accumulators per group and
 * run slower; a 128-token chunk is no faster. */
kernel void k_gemm_stq1_0(
    constant LlmMetalParams &p   [[buffer(0)]],
    device const float      *X   [[buffer(1)]],   /* n_tok x in_dim  */
    device const uchar      *W   [[buffer(2)]],
    device float            *Y   [[buffer(3)]],   /* n_tok x out_dim */
    uint2 tgp [[threadgroup_position_in_grid]],
    uint  tid [[thread_index_in_threadgroup]],
    uint  sg  [[simdgroup_index_in_threadgroup]])
{
    threadgroup float smem[(GEMM_ROWS + GEMM_TOKS) * GEMM_LD];
    threadgroup float *sa = smem, *sb = smem + GEMM_ROWS * GEMM_LD;
    const int K = p.in_dim, N = p.out_dim, T = p.n_tok;
    const int n0 = (int)tgp.x * GEMM_ROWS, t0 = (int)tgp.y * GEMM_TOKS;
    const int row0 = (int)(sg & 1) * 32;      /* this SIMD group's 32 rows   */
    const int tok0 = (int)(sg >> 1) * 16;     /* ... and its 16 tokens       */

    simdgroup_float8x8 acc[8];
    #pragma unroll
    for (int i = 0; i < 8; i++) acc[i] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);

    /* Thread tid decodes row (tid & 63), micro-groups 4 * (tid >> 6) .. +3. */
    const int r_dec = (int)(tid & 63u), hq = (int)(tid >> 6);
    const int n_dec = n0 + r_dec;
    const bool row_live = n_dec < N;
    device const uchar *wrow = W + p.w_offset +
                               (uint)(row_live ? n_dec : 0) * (uint)((K / 256) * 42);

    for (int c = 0; c < K / 64; c++) {
        device const uchar *blk = wrow + (c >> 2) * 42;
        int k4 = c & 3;
        device const ushort *q16 = (device const ushort *)(blk + 8 * k4);
        uint s16 = *((device const ushort *)(blk + 32 + 2 * k4));
        float d = row_live ? float(as_type<half>(*((device const ushort *)(blk + 40)))) : 0.0f;
        uint u = q16[hq];                       /* micro-groups 4*hq .. 4*hq+3 */
        uint pats[4];
        #pragma unroll
        for (int j = 0; j < 4; j++) {
            int gl = hq * 4 + j;
            uint code = (u >> (4 * j)) & 0xFu;
            pats[j] = (uint)stq1_0_codebook[(((s16 >> gl) & 1u) << 4) | code];
        }
        #pragma unroll
        for (int h2 = 0; h2 < 2; h2++) {
            #pragma unroll
            for (int j = 0; j < 4; j++) {
                int gl = hq * 4 + j;
                uint pt = pats[j] >> (4 * h2);
                threadgroup float *dst = sa + r_dec * GEMM_LD + gl;
                dst[0]  = d * (float)((int)(pt & 3u) - 1);
                dst[16] = d * (float)((int)((pt >> 2) & 3u) - 1);
            }
            #pragma unroll
            for (int i = 0; i < 2; i++) {
                int idx4 = (int)tid + GEMM_TG * i;
                int tk = idx4 >> 3, kq = (idx4 & 7) * 4;
                float4 v = 0.0f;
                if (t0 + tk < T)
                    v = *((device const float4 *)(X + (ulong)(t0 + tk) * (ulong)K + c * 64 + h2 * 32 + kq));
                *((threadgroup float4 *)(sb + tk * GEMM_LD + kq)) = v;
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            #pragma unroll
            for (int kk = 0; kk < 4; kk++) {
                simdgroup_float8x8 ma[4], mb[2];
                #pragma unroll
                for (int i = 0; i < 4; i++)
                    simdgroup_load(ma[i], sa + (row0 + 8 * i) * GEMM_LD + kk * 8, GEMM_LD);
                #pragma unroll
                for (int j = 0; j < 2; j++)
                    simdgroup_load(mb[j], sb + (tok0 + 8 * j) * GEMM_LD + kk * 8, GEMM_LD, ulong2(0, 0), true);
                #pragma unroll
                for (int i = 0; i < 4; i++)
                    #pragma unroll
                    for (int j = 0; j < 2; j++)
                        simdgroup_multiply_accumulate(acc[i * 2 + j], ma[i], mb[j], acc[i * 2 + j]);
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
    }

    threadgroup float *res = smem;
    #pragma unroll
    for (int i = 0; i < 4; i++)
        #pragma unroll
        for (int j = 0; j < 2; j++)
            simdgroup_store(acc[i * 2 + j], res + (row0 + 8 * i) * GEMM_RES_LD + (tok0 + 8 * j), GEMM_RES_LD);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int i = (int)tid; i < GEMM_ROWS * GEMM_TOKS; i += GEMM_TG) {
        int r = i % GEMM_ROWS, t = i / GEMM_ROWS;
        int n = n0 + r, tt = t0 + t;
        if (n >= N || tt >= T) continue;
        float v = res[r * GEMM_RES_LD + t];
        device float *dst = Y + (ulong)tt * (ulong)N + (ulong)n;
        if (p.accumulate) *dst += v;
        else              *dst  = v;
    }
}

/* RMSNorm of each of n_tok rows (in_dim wide): one MATVEC_TG group a row,
 * the sum of squares reduced exactly as stq_stage_rmsnorm reduces it, and
 * each value written as `x * scale * w` as it stages it — so a batched
 * projection sees the inputs the fused per-position kernels compute. */
kernel void k_rmsnorm_rows(
    constant LlmMetalParams &p   [[buffer(0)]],
    device const float      *in  [[buffer(1)]],
    device const uchar      *weights [[buffer(2)]],
    device float            *out [[buffer(3)]],
    uint t         [[threadgroup_position_in_grid]],
    uint tid       [[thread_index_in_threadgroup]],
    uint tgsize    [[threads_per_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]],
    uint simd_gid  [[simdgroup_index_in_threadgroup]])
{
    threadgroup float red[MATVEC_SIMDS];
    int n = p.in_dim;
    device const float *x = in + (ulong)t * (ulong)n;
    float ss = 0.0f;
    for (int i = (int)tid; i < n; i += (int)tgsize) {
        float v = x[i];
        ss += v * v;
    }
    ss = simd_sum(ss);
    if (simd_lane == 0) red[simd_gid] = ss;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float total_ss = 0.0f;
    for (uint i = 0; i < tgsize / 32; i++) total_ss += red[i];
    float rms_scale = 1.0f / sqrt(total_ss / (float)n + p.rms_eps);
    device const float *nw = (device const float *)(weights + p.w_offset);
    device float *y = out + (ulong)t * (ulong)n;
    for (int i = (int)tid; i < n; i += (int)tgsize) y[i] = x[i] * rms_scale * nw[i];
}

/* SwiGLU over n_tok x n_ff values: g = silu(g) * u, as k_gate_up writes it. */
kernel void k_swiglu(
    constant LlmMetalParams &p [[buffer(0)]],
    device float       *g [[buffer(1)]],
    device const float *u [[buffer(2)]],
    uint i [[thread_position_in_grid]])
{
    if ((int)i >= p.n_tok * p.n_ff) return;
    float gv = g[i];
    float silu = gv / (1.0f + exp(-gv));
    g[i] = silu * u[i];
}

/* ---- batched prefill: attention over a chunk of positions ------------
 *
 * The decode path's k_attn_block runs one query head of one position per
 * threadgroup: over a whole chunk that is n_tok x n_head groups, each
 * reading its kv head's K and V rows itself — the four query heads of a
 * group and the chunk's positions read the same rows again, and a score is
 * a SIMD reduction per position.
 *
 * k_q_prep_rows first does what k_attn_block does to its query before it
 * scores (RoPE at the position, then QK-RMSNorm), for every head of every
 * position, in place. k_attn_prefill then takes one kv head and FA_QP
 * positions: its rows are the group's query heads x those positions (a SIMD
 * group a head), and it walks the keys in tiles of FA_BK — each K and V row
 * read once for all of them — with 8x8 fp32 tiles for Q.K^T and P.V and the
 * softmax kept online (a running max and sum a row, the output rescaled as
 * the max moves). The causal limit is per row: position first + q0 + r sees
 * keys 0 .. itself. The sums are taken in another order than k_attn_block's.
 *
 * Needs head_dim a multiple of 8, at most FA_MAX_HD, and at most FA_MAX_GROUP
 * query heads per kv head; the host checks and otherwise keeps k_attn_block.
 *
 * The key tiles are read as float4s. A 128-position chunk is no faster. */
kernel void k_q_prep_rows(
    constant LlmMetalParams &p [[buffer(0)]],
    device float       *q       [[buffer(1)]],   /* n_tok x q_dim, in place */
    device const uchar *weights [[buffer(2)]],   /* q_norm weight @ w_offset */
    device const float2 *rope   [[buffer(3)]],
    uint2 tg    [[threadgroup_position_in_grid]],   /* x head, y position in the chunk */
    uint tid    [[thread_index_in_threadgroup]],
    uint2 tgsz  [[threads_per_threadgroup]])
{
    const uint tgsize = tgsz.x;
    threadgroup float v[LLM_METAL_MAX_HEAD_DIM];
    threadgroup float red[FA_PREP_TG];
    int hd = p.head_dim;
    device float *qh = q + (ulong)tg.y * (ulong)(p.n_head * hd) + (ulong)tg.x * (ulong)hd;
    for (int i = (int)tid; i < hd; i += (int)tgsize) v[i] = qh[i];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    rope_head_tg(v, hd, p.pos + (int)tg.y, rope, p.rope_neox, tid, tgsize);
    if (p.qk_norm) {
        device const float *w = (device const float *)(weights + p.w_offset);
        qknorm_head_tg(v, w, hd, p.rms_eps, red, tid, tgsize);
    }
    for (int i = (int)tid; i < hd; i += (int)tgsize) qh[i] = v[i];
}

kernel void k_attn_prefill(
    constant LlmMetalParams &p [[buffer(0)]],
    device const float *q       [[buffer(1)]],   /* n_tok x q_dim, prepared  */
    device const float *kcache  [[buffer(2)]],
    device const float *vcache  [[buffer(3)]],
    device float       *out     [[buffer(4)]],   /* n_tok x q_dim            */
    uint2 tg   [[threadgroup_position_in_grid]],    /* x kv head, y position tile */
    uint  tid  [[thread_index_in_threadgroup]],
    uint  lane [[thread_index_in_simdgroup]],
    uint  sg   [[simdgroup_index_in_threadgroup]])
{
    threadgroup float qs[FA_MAX_GROUP * FA_QP * FA_LD];   /* query rows; the output at the end */
    threadgroup float kv[FA_BK * FA_LD];                  /* a tile of K, then of V */
    threadgroup float ps[FA_MAX_GROUP * FA_QP * FA_BK];   /* scores, then weights */
    threadgroup float dg[FA_MAX_GROUP][64];               /* a row scale per SIMD group, as a diagonal */

    const int hd = p.head_dim, hd8 = hd / 8;
    const int group = p.n_head / p.n_head_kv;
    const int kvh = (int)tg.x;
    const int q0 = (int)tg.y * FA_QP;
    const int nq = min(FA_QP, p.n_tok - q0);
    const int q_dim = p.n_head * hd, kv_dim = p.n_head_kv * hd;
    const int first = p.pos;                            /* the chunk's first position */
    const int n_keys = first + q0 + nq;                 /* keys 0 .. the tile's last position */
    const ulong layer_base = (ulong)p.layer * (ulong)p.n_ctx * (ulong)kv_dim;
    const int nthreads = group * 32;

    /* The query rows: SIMD group h holds query head kvh*group + h at
     * positions q0 .. q0 + FA_QP - 1 (zero past the chunk). */
    const int hd4 = hd / 4;
    for (int i = (int)tid; i < group * FA_QP * hd4; i += nthreads) {
        int r = i / hd4, d = (i % hd4) * 4;
        int h = r / FA_QP, t = r % FA_QP;
        float4 v = 0.0f;
        if (t < nq) v = *((device const float4 *)(q + (ulong)(q0 + t) * (ulong)q_dim +
                                                   (ulong)((kvh * group + h) * hd + d)));
        *((threadgroup float4 *)(qs + r * FA_LD + d)) = v;
    }
    for (int i = (int)lane; i < 64; i += 32) dg[sg][i] = 0.0f;

    simdgroup_float8x8 o[FA_MAX_HD / 8];
    for (int c = 0; c < hd8; c++) o[c] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);

    /* Lane `lane` looks after row lane / 4 of its SIMD group, columns
     * (lane % 4) * 4 .. + 3 of a key tile; the four lanes of a row keep the
     * row's running max and sum alike. */
    const int my_row = (int)lane >> 2, my_col = ((int)lane & 3) * 4;
    const int my_pos = first + q0 + my_row;              /* the row's own position */
    const bool row_live = my_row < nq;
    float m_run = -INFINITY, l_run = 0.0f;

    device const float *kbase = kcache + layer_base + (ulong)kvh * (ulong)hd;
    device const float *vbase = vcache + layer_base + (ulong)kvh * (ulong)hd;
    threadgroup float *my_ps = ps + (int)sg * FA_QP * FA_BK;

    for (int kb = 0; kb < n_keys; kb += FA_BK) {
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (int i = (int)tid; i < FA_BK * hd4; i += nthreads) {
            int r = i / hd4, d = (i % hd4) * 4;
            float4 v = 0.0f;
            if (kb + r < n_keys) v = *((device const float4 *)(kbase + (ulong)(kb + r) * (ulong)kv_dim + (ulong)d));
            *((threadgroup float4 *)(kv + r * FA_LD + d)) = v;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        /* S = Q . K^T for this SIMD group's 8 rows and the tile's 16 keys */
        simdgroup_float8x8 s0 = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
        simdgroup_float8x8 s1 = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
        for (int c = 0; c < hd8; c++) {
            simdgroup_float8x8 qf, k0, k1;
            simdgroup_load(qf, qs + (int)sg * FA_QP * FA_LD + c * 8, FA_LD);
            simdgroup_load(k0, kv + 0 * FA_LD + c * 8, FA_LD, ulong2(0, 0), true);
            simdgroup_load(k1, kv + 8 * FA_LD + c * 8, FA_LD, ulong2(0, 0), true);
            simdgroup_multiply_accumulate(s0, qf, k0, s0);
            simdgroup_multiply_accumulate(s1, qf, k1, s1);
        }
        simdgroup_store(s0, my_ps + 0, FA_BK);
        simdgroup_store(s1, my_ps + 8, FA_BK);
        simdgroup_barrier(mem_flags::mem_threadgroup);

        /* the online softmax: this lane's four scores of its row */
        float sv[4];
        float tmax = -INFINITY;
        #pragma unroll
        for (int j = 0; j < 4; j++) {
            int key = kb + my_col + j;
            float v = my_ps[my_row * FA_BK + my_col + j] * p.attn_scale;
            bool ok = row_live && key <= my_pos && key < n_keys;
            sv[j] = ok ? v : -INFINITY;
            tmax = max(tmax, sv[j]);
        }
        tmax = max(tmax, simd_shuffle_xor(tmax, 1));
        tmax = max(tmax, simd_shuffle_xor(tmax, 2));
        float m_new = max(m_run, tmax);
        float alpha = (m_new == -INFINITY) ? 1.0f : exp(m_run - m_new);
        float tsum = 0.0f;
        #pragma unroll
        for (int j = 0; j < 4; j++) {
            float e = (sv[j] == -INFINITY) ? 0.0f : exp(sv[j] - m_new);
            my_ps[my_row * FA_BK + my_col + j] = e;
            tsum += e;
        }
        tsum += simd_shuffle_xor(tsum, 1);
        tsum += simd_shuffle_xor(tsum, 2);
        l_run = l_run * alpha + tsum;
        m_run = m_new;
        if ((lane & 3) == 0) dg[sg][my_row * 8 + my_row] = alpha;
        simdgroup_barrier(mem_flags::mem_threadgroup);

        /* O = diag(alpha) . O, then O += P . V once V is in */
        simdgroup_float8x8 dm;
        simdgroup_load(dm, dg[sg], 8);
        for (int c = 0; c < hd8; c++) {
            simdgroup_float8x8 t;
            simdgroup_multiply(t, dm, o[c]);
            o[c] = t;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (int i = (int)tid; i < FA_BK * hd4; i += nthreads) {
            int r = i / hd4, d = (i % hd4) * 4;
            float4 v = 0.0f;
            if (kb + r < n_keys) v = *((device const float4 *)(vbase + (ulong)(kb + r) * (ulong)kv_dim + (ulong)d));
            *((threadgroup float4 *)(kv + r * FA_LD + d)) = v;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        simdgroup_float8x8 p0, p1;
        simdgroup_load(p0, my_ps + 0, FA_BK);
        simdgroup_load(p1, my_ps + 8, FA_BK);
        for (int c = 0; c < hd8; c++) {
            simdgroup_float8x8 v0, v1;
            simdgroup_load(v0, kv + 0 * FA_LD + c * 8, FA_LD);
            simdgroup_load(v1, kv + 8 * FA_LD + c * 8, FA_LD);
            simdgroup_multiply_accumulate(o[c], p0, v0, o[c]);
            simdgroup_multiply_accumulate(o[c], p1, v1, o[c]);
        }
    }

    /* O / l, through threadgroup memory into the rows' places */
    if ((lane & 3) == 0) dg[sg][my_row * 8 + my_row] = l_run > 0.0f ? 1.0f / l_run : 0.0f;
    simdgroup_barrier(mem_flags::mem_threadgroup);
    simdgroup_float8x8 dm;
    simdgroup_load(dm, dg[sg], 8);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int c = 0; c < hd8; c++) {
        simdgroup_float8x8 t;
        simdgroup_multiply(t, dm, o[c]);
        simdgroup_store(t, qs + (int)sg * FA_QP * FA_LD + c * 8, FA_LD);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int i = (int)tid; i < group * FA_QP * hd; i += nthreads) {
        int r = i / hd, d = i % hd;
        int h = r / FA_QP, t = r % FA_QP;
        if (t >= nq) continue;
        out[(ulong)(q0 + t) * (ulong)q_dim + (ulong)((kvh * group + h) * hd + d)] = qs[r * FA_LD + d];
    }
}

/* k_kv_prep for n_tok positions at once (prefill): kv head tg.x of position
 * p.pos + tg.y, its K and V rows at row tg.y of the chunk's buffers. The same
 * steps with the same threadgroup width as k_kv_prep, so the cache holds the
 * very bytes the per-position kernel writes. */
kernel void k_kv_prep_rows(
    constant LlmMetalParams &p [[buffer(0)]],
    device const float *kc [[buffer(1)]],       /* n_tok x kv_dim K projections */
    device const float *vc [[buffer(2)]],       /* n_tok x kv_dim V projections */
    device const uchar *weights [[buffer(3)]],  /* k_norm weight @ w_offset */
    device float       *kcache [[buffer(4)]],
    device float       *vcache [[buffer(5)]],
    device const float2 *rope [[buffer(6)]],
    uint2 tg    [[threadgroup_position_in_grid]],
    uint  tid   [[thread_index_in_threadgroup]],
    uint2 tgsz  [[threads_per_threadgroup]])
{
    threadgroup float ks[256];
    threadgroup float red[256];
    const uint tgsize = tgsz.x;
    int hd = p.head_dim;
    int kv_dim = p.n_head_kv * hd;
    int pos = p.pos + (int)tg.y;
    uint kvh = tg.x;

    device const float *kh = kc + (ulong)tg.y * (ulong)kv_dim + kvh * hd;
    for (int i = (int)tid; i < hd; i += (int)tgsize) ks[i] = kh[i];
    threadgroup_barrier(mem_flags::mem_threadgroup);

    rope_head_tg(ks, hd, pos, rope, p.rope_neox, tid, tgsize);
    if (p.qk_norm) {
        device const float *w = (device const float *)(weights + p.w_offset);
        qknorm_head_tg(ks, w, hd, p.rms_eps, red, tid, tgsize);
    }

    ulong slot = ((ulong)p.layer * p.n_ctx + (ulong)pos) * (ulong)kv_dim + (ulong)kvh * hd;
    device const float *vh = vc + (ulong)tg.y * (ulong)kv_dim + kvh * hd;
    for (int i = (int)tid; i < hd; i += (int)tgsize) {
        kcache[slot + i] = ks[i];
        vcache[slot + i] = vh[i];
    }
}
