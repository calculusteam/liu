/*
 * Liu - STQ1_0 dequantization (AngelSlim "Sherry" 1.3125-bit ternary).
 *
 * Layout confirmed against llama.cpp PR #22836 (`block_stq1_0`):
 *
 *     uint8_t qs[32];    // QK_K/8  — one 4-bit code per group of 4 weights
 *     uint8_t sign[8];   // QK_K/32 — one 1-bit table-select per group
 *     f16     d;         // super-block scale
 *                        // = 42 bytes per 256-weight super-block
 *
 * Each group of 4 weights has exactly one 0 and three non-zero (+1/-1)
 * lanes — 4 zero-positions x 2^3 signs = 32 patterns. The (sign,code)
 * pair indexes a 32-entry codebook of packed 2-bit lanes (qpack); lane p
 * decodes to q = (qpack >> 2p) & 3, value = (q - 1) * d, q-1 in {-1,0,1}.
 */
#ifndef LLM_LLM_QUANT_STQ1_0_H
#define LLM_LLM_QUANT_STQ1_0_H

#include "core/types.h"

#define STQ1_0_BLOCK       256              /* weights per super-block      */
#define STQ1_0_BLOCK_BYTES 42               /* qs[32] + sign[8] + d(f16,2)  */

/* Bytes occupied by `n` STQ1_0 weights (n must be a multiple of BLOCK). */
static inline usize stq1_0_row_bytes(i32 n) {
    return (usize)(n / STQ1_0_BLOCK) * STQ1_0_BLOCK_BYTES;
}

/* Dequantize `n` weights (multiple of STQ1_0_BLOCK) from `src` into the
 * caller's F32 `dst`. */
void stq1_0_dequant_row(const u8 *src, f32 *dst, i32 n);

/* Pack one 256-weight super-block. `codes` must be exactly ternary
 * (-1/0/+1) with exactly one zero per consecutive group of 4 — the only
 * shape the STQ1_0 codebook can represent. `out` receives
 * STQ1_0_BLOCK_BYTES bytes. Returns false if the constraint is violated.
 * Used by the unit test; documents the encoder side of the layout. */
bool stq1_0_encode_block(const i8 *codes, f32 scale, u8 *out);

#endif /* LLM_LLM_QUANT_STQ1_0_H */
