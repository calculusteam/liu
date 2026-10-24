/*
 * STQ1_0 x Q8_K vector dot kernel.
 *
 * Portions derived from llama.cpp / ggml: Copyright (c) 2023-2026 The ggml
 * authors These portions are licensed under the MIT License; see
 * packaging/licenses/llama.cpp-LICENSE.txt and THIRD_PARTY_NOTICES.md.
 *
 * The ARM NEON ggml_vec_dot_stq1_0_q8_K kernel uses the Q8_K activation
 * format, quantize_row_q8_K, the table_b2b_0 sign lookup table, and planar
 * repacking. A matrix-vector operation quantizes the F32 activation once,
 * then computes each STQ1_0 weight-row dot product using codebook lookup and
 * integer SIMD. The implementation preserves ggml-compatible layouts.
 *
 * For one matrix-vector operation, call stq1_0_q8k_quantize once, then call
 * stq1_0_vec_dot for each weight row.
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

/* Computes the ggml STQ1_0 x Q8_K dot product for one weight row. */
f32 stq1_0_vec_dot(const u8 *weight, const void *q8k, i32 n);

/* The same dot through the portable scalar path, whatever SIMD the build would
 * otherwise pick. Exists so the vector kernels can be checked against the
 * reference exactly rather than approximately — an inference kernel that is
 * merely close is a wrong answer that looks right. */
f32 stq1_0_vec_dot_ref(const u8 *weight, const void *q8k, i32 n);

#endif /* LLM_LLM_STQ1_0_KERNEL_H */
