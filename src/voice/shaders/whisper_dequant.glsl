/*
 * Liu - block dequantization for the Vulkan Whisper shaders.
 *
 * Portions derived from llama.cpp / ggml:
 * Copyright (c) 2023-2026 The ggml authors
 * These portions are licensed under the MIT License; see
 * packaging/licenses/llama.cpp-LICENSE.txt and THIRD_PARTY_NOTICES.md.
 *
 * Include AFTER declaring the weight buffer as:
 *
 *   layout(std430, binding = 1) readonly buffer Wbuf { uint data[]; } Wbuf;
 *
 * The accessors below read it by that name. Everything is byte-addressed
 * because the container has no alignment: a quantized block is 18/20/22/24/34
 * bytes and lands wherever the previous one ended.
 */
#ifndef WHISPER_DEQUANT_GLSL
#define WHISPER_DEQUANT_GLSL

uint wqByte(uint off) {
    return (Wbuf.data[off >> 2u] >> ((off & 3u) * 8u)) & 0xFFu;
}

float wqHalf(uint off) {
    uint lo = wqByte(off);
    uint hi = wqByte(off + 1u);
    return unpackHalf2x16(lo | (hi << 8u)).x;
}

uint wqU32(uint off) {
    return wqByte(off) | (wqByte(off + 1u) << 8u) |
           (wqByte(off + 2u) << 16u) | (wqByte(off + 3u) << 24u);
}

int wqBlockBytes(int dt) {
    if (dt == WM_TYPE_F32)  return 128;
    if (dt == WM_TYPE_F16)  return 64;
    if (dt == WM_TYPE_Q4_0) return 18;
    if (dt == WM_TYPE_Q4_1) return 20;
    if (dt == WM_TYPE_Q5_0) return 22;
    if (dt == WM_TYPE_Q5_1) return 24;
    if (dt == WM_TYPE_Q8_0) return 34;
    return 0;
}

/* Element e (0..31) of the block at byte offset `blk`.
 *
 * The 4- and 5-bit layouts split a byte across element j and element j+16,
 * never j*2 and j*2+1 — the same trap the C and Metal dequantizers carry a
 * comment about, and the one that produces a plausible-looking matrix that is
 * silently interleaved wrong. */
float wqOne(uint blk, int dt, int e) {
    if (dt == WM_TYPE_F32) return uintBitsToFloat(wqU32(blk + uint(e) * 4u));
    if (dt == WM_TYPE_F16) return wqHalf(blk + uint(e) * 2u);

    if (dt == WM_TYPE_Q4_0) {
        float d = wqHalf(blk);
        uint j = uint(e & 15);
        uint b = wqByte(blk + 2u + j);
        int q = (e < 16) ? int(b & 0x0Fu) : int(b >> 4u);
        return float(q - 8) * d;
    }
    if (dt == WM_TYPE_Q4_1) {
        float d = wqHalf(blk);
        float m = wqHalf(blk + 2u);
        uint j = uint(e & 15);
        uint b = wqByte(blk + 4u + j);
        int q = (e < 16) ? int(b & 0x0Fu) : int(b >> 4u);
        return float(q) * d + m;
    }
    if (dt == WM_TYPE_Q5_0) {
        float d = wqHalf(blk);
        uint qh = wqU32(blk + 2u);
        uint j = uint(e & 15);
        uint b = wqByte(blk + 6u + j);
        int q, hi;
        if (e < 16) { q = int(b & 0x0Fu); hi = int(((qh >> j) << 4u) & 0x10u); }
        else        { q = int(b >> 4u);   hi = int((qh >> (j + 12u)) & 0x10u); }
        return float((q | hi) - 16) * d;
    }
    if (dt == WM_TYPE_Q5_1) {
        float d = wqHalf(blk);
        float m = wqHalf(blk + 2u);
        uint qh = wqU32(blk + 4u);
        uint j = uint(e & 15);
        uint b = wqByte(blk + 8u + j);
        int q, hi;
        if (e < 16) { q = int(b & 0x0Fu); hi = int(((qh >> j) << 4u) & 0x10u); }
        else        { q = int(b >> 4u);   hi = int((qh >> (j + 12u)) & 0x10u); }
        return float(q | hi) * d + m;
    }
    if (dt == WM_TYPE_Q8_0) {
        float d = wqHalf(blk);
        int s = int(wqByte(blk + 2u + uint(e)));
        if (s > 127) s -= 256;
        return float(s) * d;
    }
    return 0.0;
}

/* Byte offset of the block covering element k of weight row n. */
uint wqRowBlock(int n, int k, int K, int dt) {
    int bpr = K / 32;
    return uint((n * bpr + (k / 32)) * wqBlockBytes(dt));
}

#endif
