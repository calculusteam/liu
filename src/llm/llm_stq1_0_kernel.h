/*
 * Liu - STQ1_0 x Q8_K vec_dot kernel.
 *
 * A faithful port of llama.cpp PR #22836's STQ1_0 inference kernel: the
 * ARM NEON `ggml_vec_dot_stq1_0_q8_K` (the centerpiece of that PR) plus
 * the ggml support code it depends on — the Q8_K activation format,
 * `quantize_row_q8_K`, the `table_b2b_0` sign LUT and the planar repack.
 *
 * llama.cpp computes an STQ1_0 matvec as: quantize the F32 activation to
 * Q8_K once, then run the integer-SIMD dot (codebook lookup via vqtbl2q
 * + native vdotq_s32) against each STQ1_0 weight row. This module mirrors
 * that exactly; the implementation internals keep ggml's <stdint.h>
 * types and structure so they stay byte-checkable against upstream.
 *
 * Usage (one matvec):
 *     stq1_0_q8k_quantize(activation, q8k_scratch, in_dim);
 *     for (r ...) out[r] = stq1_0_vec_dot(weight_row_r, q8k_scratch, in_dim);
 */
#ifndef LLM_LLM_STQ1_0_KERNEL_H
#define LLM_LLM_STQ1_0_KERNEL_H

#include "core/types.h"

/* Bytes of Q8_K scratch needed to hold `n` quantized activations
 * (`n` is rounded up to a 256-multiple — one block_q8_K per 256). */
usize stq1_0_q8k_bytes(i32 n);

/* Quantize `n` F32 activations into the caller's Q8_K scratch (which must
 * be >= stq1_0_q8k_bytes(n) bytes). On ARM NEON this also repacks each
 * block to the planar layout the vec_dot's vld1q reads expect, so it must
 * be called exactly once per matvec, before the per-row dot loop. */
void stq1_0_q8k_quantize(const f32 *x, void *q8k, i32 n);

/* Dot one STQ1_0 weight row (`weight` = `n` weights in STQ1_0 block
 * layout, i.e. stq1_0_row_bytes(n) bytes) against a Q8_K-quantized
 * activation prepared by stq1_0_q8k_quantize. Returns the f32 dot. This
 * is llama.cpp PR #22836's ggml_vec_dot_stq1_0_q8_K. */
f32 stq1_0_vec_dot(const u8 *weight, const void *q8k, i32 n);

#endif /* LLM_LLM_STQ1_0_KERNEL_H */
