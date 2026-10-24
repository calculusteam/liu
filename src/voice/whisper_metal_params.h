/*
 * Liu - parameter blocks shared by whisper_metal.m and whisper_kernels.metal.
 *
 * Compiled by both the Objective-C host and the Metal shader compiler, so it
 * must stay in the intersection of the two languages: plain structs, plain
 * ints, no typedefs from core/types.h (the shader has no access to them) and
 * no implicit padding assumptions — every field is 4 bytes.
 */
#ifndef VOICE_WHISPER_METAL_PARAMS_H
#define VOICE_WHISPER_METAL_PARAMS_H

/* Mirrors the WGGML_TYPE_* ids. Duplicated rather than included because
 * whisper_ggml.h pulls in core/types.h, which the Metal compiler cannot
 * parse. Kept in sync by a _Static_assert on the host side. */
#define WM_TYPE_F32  0
#define WM_TYPE_F16  1
#define WM_TYPE_Q4_0 2
#define WM_TYPE_Q4_1 3
#define WM_TYPE_Q5_0 6
#define WM_TYPE_Q5_1 7
#define WM_TYPE_Q8_0 8

/* GEMM tile dimensions. BK is 32 because each supported quantization block
 * contains 32 elements, so one k-tile aligns with one block and requires no
 * partial-block handling.
 *
 * BM is 32 to limit accumulator and drain-barrier usage. The output tile
 * reuses the threadgroup memory occupied by the A and B tiles after they are
 * no longer needed.
 */
#define WM_BM 32
#define WM_BN 64
#define WM_BK 32
#define WM_GEMM_THREADS 128

/* Simdgroups per matvec threadgroup; each handles one output element. */
#define WM_MATVEC_SG 8
#define WM_MATVEC_THREADS (WM_MATVEC_SG * 32)

/* Attention tiling. One threadgroup covers WM_ATTN_QB queries of one head and
 * walks the keys in WM_ATTN_KB-sized steps, carrying a running max/sum/output
 * so no full score row is ever materialized.
 *
 * With ONE query per threadgroup, the head's whole K and V slice is re-read
 * for every query and attention is bound by memory bandwidth; tiling over
 * queries divides those reads by the tile height. What is left is arithmetic,
 * so both products are simdgroup 8x8 matrix ops, which is what the 32-wide
 * tile below is for. */
#define WM_ATTN_QB 32
#define WM_ATTN_KB 32
#define WM_ATTN_THREADS 128
#define WM_ATTN_SG (WM_ATTN_THREADS / 32)

/* Largest head_dim the kernel stages an output tile for. Every Whisper size
 * has exactly 64 (n_state / n_head is 64 from tiny through large-v3); the
 * host refuses anything larger rather than letting the kernel drop the tail
 * of each value vector. */
#define WM_ATTN_HD_MAX 64

/* Threads per conv1d threadgroup; also the number of output timesteps one
 * threadgroup covers, so the weight row it stages is amortized over 256
 * outputs. */
#define WM_CONV_THREADS 256

struct WmGemm {
    int M, N, K;          /* C[M,N] = A[M,K] . B[N,K]^T + bias[N] */
    int dtype;            /* WM_TYPE_* of B                       */
    int has_bias;
    int pad0, pad1, pad2;
};

struct WmConv {
    int in_ch, out_ch;
    int t_in, t_out;
    int stride;
    int apply_gelu;
    int pad0, pad1;
};

struct WmNorm {
    int n_state;
    int n_tokens;
    int pad0, pad1;
};

struct WmAttn {
    int n_q;              /* queries in this call                       */
    int n_kv;             /* keys/values visible                        */
    int n_head;
    int head_dim;
    int n_state;          /* row stride of q/k/v/out                    */
    int causal;           /* mask j > causal_offset + i                 */
    int causal_offset;    /* absolute position of query 0               */
    float scale;
};

struct WmElem {
    int n;
    int pad0, pad1, pad2;
};

struct WmTranspose {
    int n_state;
    int n_tokens;         /* == t_out of conv2 */
    int pad0, pad1;
};

struct WmEmbed {
    int n_state;
    int n_tokens;
    int n_past;
    int dtype;            /* of the token-embedding table */
};

#endif /* VOICE_WHISPER_METAL_PARAMS_H */
