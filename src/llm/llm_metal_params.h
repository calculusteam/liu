/*
 * Liu - shared parameter block for the Metal LLM backend.
 *
 * Included by both llm_metal.m (host) and llm_metal_kernels.metal
 * (device). On macOS arm64 `int`, `unsigned`, and `float` are 32-bit in
 * both C and the Metal Shading Language, so a plain struct is ABI-safe
 * across the boundary.
 *
 * One struct serves every kernel — each dispatch fills only the fields
 * its kernel reads. Bound at buffer(0) via setBytes.
 */
#ifndef LLM_LLM_METAL_PARAMS_H
#define LLM_LLM_METAL_PARAMS_H

/* KV-cache / attention-score ceiling. Shared so the host (llm_metal.m)
 * can static_assert it against LLM_MODEL_MAX_CTX and the device kernels
 * can size threadgroup scratch from the same constant. At 2048 the
 * attention kernel's `scores[]` threadgroup array is 8 KB — well within
 * the 32 KB threadgroup-memory budget. */
#define LLM_METAL_MAX_CTX 2048

/* Per-threadgroup staging tile for the matvec input vector — a multiple
 * of the 256-weight quant block size. The fused-rmsnorm mega-matvecs
 * (k_qkv, k_gate_up) require their input (n_embd) to fit in one tile, so
 * the host rejects models with n_embd > MATVEC_TILE and falls back to
 * the CPU engine. */
#define MATVEC_TILE   2048
#define MATVEC_TG     256          /* matvec threadgroup size           */
#define MATVEC_SIMDS  (MATVEC_TG / 32)  /* SIMD groups (= rows) per TG   */

/* The fused attention kernels (k_kv_prep, k_attn_block) stage one head's
 * Q/K vector into fixed `ks[256]`/`qs[256]`/`red[256]` threadgroup arrays.
 * head_dim must not exceed this or those writes run past the arrays and
 * corrupt adjacent threadgroup memory. The host (llm_metal_create) rejects
 * any model with head_dim > this and falls back to the CPU engine. Keep in
 * sync with the [256] array sizes in llm_metal_kernels.metal. */
#define LLM_METAL_MAX_HEAD_DIM 256

/* ggml dtype ids the embed kernel can dequantize (mirror of the C-side
 * GGML_TYPE_* in llm_gguf.h — the .metal cannot include that header). */
#define LLM_DT_F32     0
#define LLM_DT_Q6_K    14
#define LLM_DT_STQ1_0  42

typedef struct {
    int      n_embd;
    int      n_ff;
    int      n_head;
    int      n_head_kv;
    int      head_dim;
    int      n_vocab;
    int      n_ctx;
    int      in_dim;          /* matvec / elementwise length            */
    int      out_dim;         /* matvec output rows                     */
    unsigned w_offset;        /* byte offset of a weight tensor in the
                               * one big weights buffer. For the fused
                               * mega-matvecs this is the RMSNorm weight */
    unsigned w_offset1;       /* fused matvec: 1st projection weight     */
    unsigned w_offset2;       /* fused matvec: 2nd projection weight     */
    unsigned w_offset3;       /* fused matvec: 3rd projection weight     */
    unsigned out_offset;      /* element (float) offset into the output */
    int      pos;             /* current decode position                */
    int      layer;           /* current layer index                    */
    int      rope_neox;       /* 1 = NeoX rotary, 0 = adjacent-pair      */
    int      accumulate;      /* matvec: 1 => out[r] += dot (fused
                               * residual), 0 => out[r] = dot            */
    int      qk_norm;         /* attention block: 1 => apply per-head
                               * QK-RMSNorm after RoPE                   */
    int      embed_dtype;     /* k_embed: LLM_DT_* of the embedding table */
    int      write_token;     /* k_argmax: 1 => write next token id      */
    float    rms_eps;
    float    rope_freq_base;
    float    attn_scale;      /* 1 / sqrt(head_dim)                      */
} LlmMetalParams;

#endif /* LLM_LLM_METAL_PARAMS_H */
