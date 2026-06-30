/*
 * Liu - Q6_K dequantization.
 *
 * Faithful port of llama.cpp's dequantize_row_q6_K. The 256-weight
 * super-block is processed in two halves of 128; within each half the
 * 6-bit quant is assembled from 4 low bits (ql) + 2 high bits (qh),
 * recentred by -32, then scaled by d * scales[sub].
 */
#include "llm/llm_quant_q6k.h"
#include "llm/llm_gguf.h"   /* gguf_f16_to_f32 */

#include <string.h>

void q6k_dequant_row(const u8 *src, f32 *dst, i32 n) {
    if (!src || !dst || n <= 0) return;
    i32 nblocks = n / Q6K_BLOCK;

    for (i32 b = 0; b < nblocks; b++) {
        const u8 *blk = src + (usize)b * Q6K_BLOCK_BYTES;
        const u8 *ql  = blk;            /* 128 bytes */
        const u8 *qh  = blk + 128;      /* 64 bytes  */
        const i8 *sc  = (const i8 *)(blk + 192);  /* 16 bytes */
        u16 dh;
        memcpy(&dh, blk + 208, 2);
        f32 d = gguf_f16_to_f32(dh);

        f32 *y = dst + (usize)b * Q6K_BLOCK;

        for (i32 half = 0; half < Q6K_BLOCK; half += 128) {
            for (i32 l = 0; l < 32; l++) {
                i32 is = l / 16;
                i32 q1 = (i32)((ql[l +  0] & 0x0F) | (((qh[l] >> 0) & 3) << 4)) - 32;
                i32 q2 = (i32)((ql[l + 32] & 0x0F) | (((qh[l] >> 2) & 3) << 4)) - 32;
                i32 q3 = (i32)((ql[l +  0] >> 4)   | (((qh[l] >> 4) & 3) << 4)) - 32;
                i32 q4 = (i32)((ql[l + 32] >> 4)   | (((qh[l] >> 6) & 3) << 4)) - 32;
                y[l +  0] = d * (f32)sc[is + 0] * (f32)q1;
                y[l + 32] = d * (f32)sc[is + 2] * (f32)q2;
                y[l + 64] = d * (f32)sc[is + 4] * (f32)q3;
                y[l + 96] = d * (f32)sc[is + 6] * (f32)q4;
            }
            ql += 64;
            qh += 32;
            sc += 8;
            y  += 128;
        }
    }
}
