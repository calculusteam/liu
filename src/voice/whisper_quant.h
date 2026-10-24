/*
 * ggml block-quantization dequantizers for Whisper weights.
 *
 * Portions derived from llama.cpp / ggml:
 * Copyright (c) 2023-2026 The ggml authors
 * These portions are licensed under the MIT License; see
 * packaging/licenses/llama.cpp-LICENSE.txt and THIRD_PARTY_NOTICES.md.
 *
 * Supported model files include F16, Q4_0, Q4_1, Q5_0, Q5_1, and Q8_0.
 *
 * Two properties of this format require care:
 *
 *   - The nibble order is not sequential. In every 4-bit and 5-bit block the
 *     low nibble of byte j is element j and the high nibble is element j + 16,
 *     not j*2 and j*2+1. Reading them in sequence interleaves the values.
 *   - The container has no alignment padding, so a block can start on any
 *     byte offset. Do not cast the source pointer to a block struct or u16;
 *     read each scalar through memcpy.
 */
#ifndef VOICE_WHISPER_QUANT_H
#define VOICE_WHISPER_QUANT_H

#include "core/types.h"

#include <string.h>

/* IEEE half -> float. Branch-free; handles subnormals, inf and NaN. */
static inline f32 wq_f16_to_f32(u16 h) {
    u32 sign = (u32)(h & 0x8000u) << 16;
    u32 exp  = (h >> 10) & 0x1Fu;
    u32 mant = h & 0x3FFu;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            i32 e = -1;
            do { e++; mant <<= 1; } while ((mant & 0x400u) == 0);
            mant &= 0x3FFu;
            bits = sign | (u32)((127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((u32)((i32)exp - 15 + 127) << 23) | (mant << 13);
    }
    f32 out;
    memcpy(&out, &bits, sizeof out);
    return out;
}

/* Unaligned little-endian half read. */
static inline f32 wq_load_f16(const u8 *p) {
    u16 h;
    memcpy(&h, p, sizeof h);
    return wq_f16_to_f32(h);
}

/* Dequantize `n` elements of `dtype` starting at `src` into `dst`.
 * `n` must be a whole number of blocks for quantized types. Returns false
 * for an unsupported dtype or a non-block-aligned count. */
bool whisper_dequant_row(const u8 *src, i32 dtype, i64 n, f32 *dst);

#endif /* VOICE_WHISPER_QUANT_H */
