/*
 * Liu - STQ1_0 1.3125-bit dequantization.
 *
 * Codebook + decode are a faithful port of llama.cpp PR #22836. The
 * 4-bit code + 1-bit sign select one of 32 packed lane patterns; the
 * lane-level table lookup does not vectorize cleanly, so dequant stays
 * scalar — the NEON win is in the matvec dot product (llm_model.c).
 */
#include "llm/llm_quant_stq1_0.h"
#include "llm/llm_gguf.h"            /* gguf_f16_to_f32 */
#include "llm/llm_stq1_0_codebook.h" /* STQ1_0_CODEBOOK_INIT */

#include <string.h>

/* (sign << 4 | code) -> packed 4-lane pattern. Lane p is 2 bits:
 * 0b00 = -1, 0b01 = 0, 0b10 = +1; decoded value = ((bits) - 1) * d.
 * Single source of truth shared with the Metal kernels. */
static const u8 stq1_0_codebook[32] = STQ1_0_CODEBOOK_INIT;

void stq1_0_dequant_row(const u8 *src, f32 *dst, i32 n) {
    if (!src || !dst || n <= 0) return;
    i32 nblocks = n / STQ1_0_BLOCK;

    for (i32 b = 0; b < nblocks; b++) {
        const u8 *blk  = src + (usize)b * STQ1_0_BLOCK_BYTES;
        const u8 *qs   = blk;          /* 32 bytes — 4-bit codes  */
        const u8 *sign = blk + 32;     /* 8 bytes  — 1-bit selects */
        u16 dh;
        memcpy(&dh, blk + 40, 2);
        f32 d = gguf_f16_to_f32(dh);

        f32 *out = dst + (usize)b * STQ1_0_BLOCK;
        for (i32 g = 0; g < STQ1_0_BLOCK / 4; g++) {
            u8 code  = (qs[g >> 1] >> (4 * (g & 1))) & 0x0F;
            u8 sgn   = (sign[g >> 3] >> (g & 7)) & 0x01;
            u8 qpack = stq1_0_codebook[((u32)sgn << 4) | code];
            i32 base = (g / 16) * 64 + (g % 16);   /* PR #22836 stride-16 */
            out[base +  0] = (f32)(((qpack >> 0) & 3) - 1) * d;
            out[base + 16] = (f32)(((qpack >> 2) & 3) - 1) * d;
            out[base + 32] = (f32)(((qpack >> 4) & 3) - 1) * d;
            out[base + 48] = (f32)(((qpack >> 6) & 3) - 1) * d;
        }
    }
}

/* Truncating f32 -> f16. Sufficient for the unit test's round-number
 * scales; the model never calls this (it only reads f16 scales). */
static u16 f32_to_f16_trunc(f32 v) {
    union { f32 f; u32 u; } s = { .f = v };
    u32 sign = (s.u >> 16) & 0x8000u;
    i32 exp  = (i32)((s.u >> 23) & 0xFF) - 127 + 15;
    u32 mant = s.u & 0x7FFFFFu;
    if (exp <= 0)   return (u16)sign;                 /* underflow -> +/-0 */
    if (exp >= 0x1F) return (u16)(sign | 0x7C00u);    /* overflow  -> inf  */
    return (u16)(sign | ((u32)exp << 10) | (mant >> 13));
}

bool stq1_0_encode_block(const i8 *codes, f32 scale, u8 *out) {
    if (!codes || !out) return false;
    memset(out, 0, STQ1_0_BLOCK_BYTES);

    u8 *qs   = out;
    u8 *sign = out + 32;
    for (i32 g = 0; g < STQ1_0_BLOCK / 4; g++) {
        /* Build qpack: lane bits -1->0b00, 0->0b01, +1->0b10. Lanes are
         * stride-16 within each 64-wide chunk (PR #22836). */
        i32 base = (g / 16) * 64 + (g % 16);
        u8 qpack = 0;
        for (i32 p = 0; p < 4; p++) {
            i8 c = codes[base + p*16];
            u8 lane;
            if (c == 0)      lane = 0x1;
            else if (c == 1) lane = 0x2;
            else if (c == -1) lane = 0x0;
            else return false;                 /* not ternary */
            qpack |= (u8)(lane << (2 * p));
        }
        /* Reverse the codebook: find (sign,code) with codebook[..] == qpack.
         * A miss means the group is not "one zero, three +/-1". */
        i32 idx = -1;
        for (i32 i = 0; i < 32; i++) {
            if (stq1_0_codebook[i] == qpack) { idx = i; break; }
        }
        if (idx < 0) return false;
        u8 sgn  = (u8)(idx >> 4);
        u8 code = (u8)(idx & 0x0F);
        qs[g >> 1]   |= (u8)(code << (4 * (g & 1)));
        sign[g >> 3] |= (u8)(sgn << (g & 7));
    }

    u16 dh = f32_to_f16_trunc(scale);
    memcpy(out + 40, &dh, 2);
    return true;
}
