/*
 * Liu - ggml block-quantization dequantizers.
 *
 * Portions derived from llama.cpp / ggml:
 * Copyright (c) 2023-2026 The ggml authors
 * These portions are licensed under the MIT License; see
 * packaging/licenses/llama.cpp-LICENSE.txt and THIRD_PARTY_NOTICES.md.
 *
 * Each routine mirrors ggml's dequantize_row_* exactly, including the
 * split-nibble ordering documented in the header.
 */
#include "voice/whisper_quant.h"
#include "voice/whisper_ggml.h"

#define WQ_QK 32   /* every supported quant type uses 32-element blocks */

static void wq_q4_0(const u8 *src, i64 nb, f32 *dst) {
    for (i64 b = 0; b < nb; b++) {
        const u8 *blk = src + b * 18;
        const f32 d   = wq_load_f16(blk);
        const u8 *qs  = blk + 2;
        f32 *y = dst + b * WQ_QK;
        for (i32 j = 0; j < WQ_QK / 2; j++) {
            y[j]             = (f32)((i32)(qs[j] & 0x0F) - 8) * d;
            y[j + WQ_QK / 2] = (f32)((i32)(qs[j] >> 4)   - 8) * d;
        }
    }
}

static void wq_q4_1(const u8 *src, i64 nb, f32 *dst) {
    for (i64 b = 0; b < nb; b++) {
        const u8 *blk = src + b * 20;
        const f32 d   = wq_load_f16(blk);
        const f32 m   = wq_load_f16(blk + 2);
        const u8 *qs  = blk + 4;
        f32 *y = dst + b * WQ_QK;
        for (i32 j = 0; j < WQ_QK / 2; j++) {
            y[j]             = (f32)(qs[j] & 0x0F) * d + m;
            y[j + WQ_QK / 2] = (f32)(qs[j] >> 4)   * d + m;
        }
    }
}

static void wq_q5_0(const u8 *src, i64 nb, f32 *dst) {
    for (i64 b = 0; b < nb; b++) {
        const u8 *blk = src + b * 22;
        const f32 d   = wq_load_f16(blk);
        u32 qh;
        memcpy(&qh, blk + 2, sizeof qh);
        const u8 *qs = blk + 6;
        f32 *y = dst + b * WQ_QK;
        for (i32 j = 0; j < WQ_QK / 2; j++) {
            /* The 5th bit of element j sits in bit j of qh, and the 5th bit
             * of element j+16 in bit j+16 — hence the +12 shift landing it
             * on 0x10 directly. */
            const u8 xh_0 = (u8)(((qh >> (j +  0)) << 4) & 0x10);
            const u8 xh_1 = (u8)(((qh >> (j + 12))     ) & 0x10);
            y[j]             = (f32)((i32)((qs[j] & 0x0F) | xh_0) - 16) * d;
            y[j + WQ_QK / 2] = (f32)((i32)((qs[j] >> 4)   | xh_1) - 16) * d;
        }
    }
}

static void wq_q5_1(const u8 *src, i64 nb, f32 *dst) {
    for (i64 b = 0; b < nb; b++) {
        const u8 *blk = src + b * 24;
        const f32 d   = wq_load_f16(blk);
        const f32 m   = wq_load_f16(blk + 2);
        u32 qh;
        memcpy(&qh, blk + 4, sizeof qh);
        const u8 *qs = blk + 8;
        f32 *y = dst + b * WQ_QK;
        for (i32 j = 0; j < WQ_QK / 2; j++) {
            const u8 xh_0 = (u8)(((qh >> (j +  0)) << 4) & 0x10);
            const u8 xh_1 = (u8)(((qh >> (j + 12))     ) & 0x10);
            y[j]             = (f32)((qs[j] & 0x0F) | xh_0) * d + m;
            y[j + WQ_QK / 2] = (f32)((qs[j] >> 4)   | xh_1) * d + m;
        }
    }
}

static void wq_q8_0(const u8 *src, i64 nb, f32 *dst) {
    for (i64 b = 0; b < nb; b++) {
        const u8 *blk = src + b * 34;
        const f32 d   = wq_load_f16(blk);
        const u8 *qs  = blk + 2;
        f32 *y = dst + b * WQ_QK;
        for (i32 j = 0; j < WQ_QK; j++) y[j] = (f32)(i8)qs[j] * d;
    }
}

bool whisper_dequant_row(const u8 *src, i32 dtype, i64 n, f32 *dst) {
    if (!src || !dst || n < 0) return false;

    if (dtype == WGGML_TYPE_F32) {
        memcpy(dst, src, (usize)n * sizeof(f32));   /* memcpy: src may be unaligned */
        return true;
    }
    if (dtype == WGGML_TYPE_F16) {
        for (i64 i = 0; i < n; i++) dst[i] = wq_load_f16(src + i * 2);
        return true;
    }

    if (n % WQ_QK != 0) return false;
    const i64 nb = n / WQ_QK;

    switch (dtype) {
        case WGGML_TYPE_Q4_0: wq_q4_0(src, nb, dst); return true;
        case WGGML_TYPE_Q4_1: wq_q4_1(src, nb, dst); return true;
        case WGGML_TYPE_Q5_0: wq_q5_0(src, nb, dst); return true;
        case WGGML_TYPE_Q5_1: wq_q5_1(src, nb, dst); return true;
        case WGGML_TYPE_Q8_0: wq_q8_0(src, nb, dst); return true;
        default: return false;
    }
}
