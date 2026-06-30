/*
 * Liu - Metal compute kernels for the local LLM engine.
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

/* One SIMD lane's partial dot of an STQ1_0 weight row against a staged
 * input that fits in one tile (in_dim <= MATVEC_TILE). The 32 lanes of a
 * SIMD group split the 64 micro-groups of each 256-weight block, reading
 * adjacent code bytes (coalesced). Caller folds the 32 partials with
 * simd_sum. Shared by k_matvec_stq1_0's single-tile path and the fused
 * mega-matvecs. */
static inline float stq1_0_row_partial(device const uchar *weights,
                                       uint w_offset, uint row, int in_dim,
                                       threadgroup const float *in_s,
                                       uint simd_lane) {
    int nblocks = in_dim / 256;
    device const uchar *rowp = weights + w_offset + row * (uint)(nblocks * 42);
    float acc = 0.0f;
    for (int bb = 0; bb < nblocks; bb++) {
        device const uchar *blk = rowp + bb * 42;
        device const uchar *qs  = blk;
        device const uchar *sgn = blk + 32;
        float d = load_f16(blk + 40);
        threadgroup const float *iv = in_s + bb * 256;
        for (int g = (int)simd_lane; g < 64; g += 32) {
            uint code  = (qs[g >> 1] >> (4 * (g & 1))) & 0x0F;
            uint s     = (sgn[g >> 3] >> (g & 7)) & 0x01;
            uint qpack = stq1_0_codebook[(s << 4) | code];
            /* PR #22836 stride-16: group g's 4 lanes are at gloc, gloc+16,
             * gloc+32, gloc+48 within their 64-wide chunk (chunk = g/16). */
            int base = (g >> 4) * 64 + (g & 15);
            acc += (float)((int)((qpack >> 0) & 3) - 1) * d * iv[base +  0];
            acc += (float)((int)((qpack >> 2) & 3) - 1) * d * iv[base + 16];
            acc += (float)((int)((qpack >> 4) & 3) - 1) * d * iv[base + 32];
            acc += (float)((int)((qpack >> 6) & 3) - 1) * d * iv[base + 48];
        }
    }
    return acc;
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
    threadgroup float in_s[MATVEC_TILE];
    int in_dim = p.in_dim;
    int nblocks_total = in_dim / 256;
    uint r = tgid * (tgsize / 32) + simd_gid;
    bool active = ((int)r < p.out_dim);
    device const uchar *row = weights + p.w_offset + (uint)r * (uint)(nblocks_total * 42);

    float acc = 0.0f;
    for (int tile = 0; tile < in_dim; tile += MATVEC_TILE) {
        int tlen = min(MATVEC_TILE, in_dim - tile);
        for (int i = (int)tid; i < tlen; i += (int)tgsize) in_s[i] = in[tile + i];
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (active) {
            int b0 = tile / 256;
            int nbt = tlen / 256;
            for (int bb = 0; bb < nbt; bb++) {
                device const uchar *blk = row + (b0 + bb) * 42;
                device const uchar *qs  = blk;
                device const uchar *sgn = blk + 32;
                float d = load_f16(blk + 40);
                threadgroup const float *iv = in_s + bb * 256;
                for (int g = (int)simd_lane; g < 64; g += 32) {
                    uint code  = (qs[g >> 1] >> (4 * (g & 1))) & 0x0F;
                    uint s     = (sgn[g >> 3] >> (g & 7)) & 0x01;
                    uint qpack = stq1_0_codebook[(s << 4) | code];
                    /* PR #22836 stride-16 grouping (see stq1_0_row_partial). */
                    int base = (g >> 4) * 64 + (g & 15);
                    acc += (float)((int)((qpack >> 0) & 3) - 1) * d * iv[base +  0];
                    acc += (float)((int)((qpack >> 2) & 3) - 1) * d * iv[base + 16];
                    acc += (float)((int)((qpack >> 4) & 3) - 1) * d * iv[base + 32];
                    acc += (float)((int)((qpack >> 6) & 3) - 1) * d * iv[base + 48];
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
         * not bestCount. With the old `t < n`, every winning id >= bestCount
         * (~87% of the vocab, INCLUDING the EOS id) was silently rewritten to
         * token 0 (= "!"): that was the random "!!!!" degeneration AND why
         * generation never stopped (the clamped-away EOS never matched the stop
         * check). The genuine all-NaN sentinel (LLM_INT_MAX) is still > n_vocab
         * so a true degenerate step still falls back to token 0. */
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

/* Cooperative fused RMSNorm: stage `in` into `in_s`, then scale by
 * 1/rms * norm_weight. `norm_w_off` is the F32 norm tensor's byte
 * offset. All threadgroup threads must call this (it has barriers). */
static inline void fused_rmsnorm(constant LlmMetalParams &p,
                                 device const float *in,
                                 device const uchar *weights,
                                 uint norm_w_off, int n,
                                 threadgroup float *in_s,
                                 threadgroup float *red,
                                 uint tid, uint tgsize,
                                 uint simd_lane, uint simd_gid) {
    float ss = 0.0f;
    for (int i = (int)tid; i < n; i += (int)tgsize) {
        float x = in[i];
        in_s[i] = x;
        ss += x * x;
    }
    ss = simd_sum(ss);
    if (simd_lane == 0) red[simd_gid] = ss;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float total_ss = 0.0f;
    for (uint i = 0; i < tgsize / 32; i++) total_ss += red[i];
    float rms_scale = 1.0f / sqrt(total_ss / (float)n + p.rms_eps);
    device const float *nw = (device const float *)(weights + norm_w_off);
    for (int i = (int)tid; i < n; i += (int)tgsize)
        in_s[i] = in_s[i] * rms_scale * nw[i];
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
    threadgroup float in_s[MATVEC_TILE];
    threadgroup float red[MATVEC_SIMDS];
    int n      = p.in_dim;                       /* n_embd, <= MATVEC_TILE */
    int q_dim  = p.n_head * p.head_dim;
    int kv_dim = p.n_head_kv * p.head_dim;
    int total  = q_dim + 2 * kv_dim;

    fused_rmsnorm(p, in, weights, p.w_offset, n, in_s, red,
                  tid, tgsize, simd_lane, simd_gid);

    uint r = tgid * (tgsize / 32) + simd_gid;
    if ((int)r >= total) return;

    uint w_off; uint local_row; device float *out;
    if ((int)r < q_dim) {
        w_off = p.w_offset1; local_row = r;                       out = outQ;
    } else if ((int)r < q_dim + kv_dim) {
        w_off = p.w_offset2; local_row = r - (uint)q_dim;         out = outK;
    } else {
        w_off = p.w_offset3; local_row = r - (uint)(q_dim + kv_dim); out = outV;
    }
    float partial = stq1_0_row_partial(weights, w_off, local_row, n, in_s, simd_lane);
    float row_sum = simd_sum(partial);
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
    threadgroup float in_s[MATVEC_TILE];
    threadgroup float red[MATVEC_SIMDS];
    int n    = p.in_dim;                         /* n_embd, <= MATVEC_TILE */
    int n_ff = p.n_ff;

    fused_rmsnorm(p, in, weights, p.w_offset, n, in_s, red,
                  tid, tgsize, simd_lane, simd_gid);

    uint r = tgid * (tgsize / 32) + simd_gid;
    if ((int)r >= n_ff) return;

    float g = simd_sum(stq1_0_row_partial(weights, p.w_offset1, r, n, in_s, simd_lane));
    float u = simd_sum(stq1_0_row_partial(weights, p.w_offset2, r, n, in_s, simd_lane));
    if (simd_lane == 0) {
        float silu = g / (1.0f + exp(-g));
        outG[r] = silu * u;
    }
}

/* ---- fused attention block -----------------------------------------
 *
 * k_kv_prep and k_attn_block collapse the 6 standalone attention-path
 * dispatches (RoPE x2, QK-norm x2, store_kv, attention) into 2. Both run
 * one threadgroup per head (head_dim threads). k_kv_prep prepares the
 * KV heads (RoPE + QK-norm + write to the cache); k_attn_block prepares
 * each query head and runs the attention over the prepared cache.
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
 * attention over the KV cache prepared by k_kv_prep. */
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
    uint tgsize [[threads_per_threadgroup]])
{
    threadgroup float qs[256];
    threadgroup float scores[LLM_METAL_MAX_CTX];
    threadgroup float red[256];

    int hd     = p.head_dim;
    int group  = p.n_head / p.n_head_kv;
    int kvh    = (int)hh / group;
    int npos   = p.pos + 1;
    int kv_dim = p.n_head_kv * hd;
    ulong layer_base = (ulong)p.layer * p.n_ctx * kv_dim;

    device const float *qh = q + hh * hd;
    for (int i = (int)tid; i < hd; i += (int)tgsize) qs[i] = qh[i];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    rope_head_tg(qs, hd, p.pos, rope, p.rope_neox, tid, tgsize);
    if (p.qk_norm) {
        device const float *w = (device const float *)(weights + p.w_offset);
        qknorm_head_tg(qs, w, hd, p.rms_eps, red, tid, tgsize);
    }

    /* scores[pp] = dot(qs, k[pp]) * scale */
    for (int pp = (int)tid; pp < npos; pp += (int)tgsize) {
        device const float *kp = kcache + layer_base + (ulong)pp * kv_dim + kvh * hd;
        float acc = 0.0f;
        for (int d = 0; d < hd; d++) acc += qs[d] * kp[d];
        scores[pp] = acc * p.attn_scale;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* softmax — max reduce */
    float lmax = -INFINITY;
    for (int pp = (int)tid; pp < npos; pp += (int)tgsize) lmax = max(lmax, scores[pp]);
    red[tid] = lmax;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = tgsize / 2; s > 0; s >>= 1) {
        if (tid < s) red[tid] = max(red[tid], red[tid + s]);
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    float mx = red[0];
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* exp + sum reduce */
    float lsum = 0.0f;
    for (int pp = (int)tid; pp < npos; pp += (int)tgsize) {
        float e = exp(scores[pp] - mx);
        scores[pp] = e;
        lsum += e;
    }
    red[tid] = lsum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = tgsize / 2; s > 0; s >>= 1) {
        if (tid < s) red[tid] += red[tid + s];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    float inv = red[0] > 0.0f ? 1.0f / red[0] : 0.0f;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* attn_out[d] = sum_pp softmax[pp] * v[pp][d] */
    for (int d = (int)tid; d < hd; d += (int)tgsize) {
        float acc = 0.0f;
        for (int pp = 0; pp < npos; pp++) {
            device const float *vp = vcache + layer_base + (ulong)pp * kv_dim + kvh * hd;
            acc += scores[pp] * inv * vp[d];
        }
        attn_out[hh * hd + d] = acc;
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
    int g = j >> 2;               /* group of 4 */
    int lane = j & 3;
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
