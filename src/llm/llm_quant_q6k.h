/*
 * Liu - Q6_K dequantization (mainline ggml 6-bit K-quant).
 *
 * The Hy-MT 1.25bit GGUF keeps its (tied) token-embedding / LM-head
 * table in Q6_K — STQ1_0 is reserved for the layer projection weights.
 * Q6_K is a long-stable llama.cpp format; this is a faithful port of
 * its reference dequantizer.
 *
 * block_q6_K (256 weights, 210 bytes):
 *   uint8_t ql[128]      lower 4 bits of each quant
 *   uint8_t qh[64]       upper 2 bits of each quant
 *   int8_t  scales[16]   per-16 sub-block scales
 *   f16     d            super-block scale
 */
#ifndef LLM_LLM_QUANT_Q6K_H
#define LLM_LLM_QUANT_Q6K_H

#include "core/types.h"

#define Q6K_BLOCK       256
#define Q6K_BLOCK_BYTES 210

/* Bytes occupied by `n` Q6_K weights (n must be a multiple of Q6K_BLOCK). */
static inline usize q6k_row_bytes(i32 n) {
    return (usize)(n / Q6K_BLOCK) * Q6K_BLOCK_BYTES;
}

/* Dequantize `n` weights (multiple of Q6K_BLOCK) from `src` into `dst`. */
void q6k_dequant_row(const u8 *src, f32 *dst, i32 n);

#endif /* LLM_LLM_QUANT_Q6K_H */
