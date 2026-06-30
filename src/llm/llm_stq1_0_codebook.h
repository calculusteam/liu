/*
 * Liu - STQ1_0 codebook: the single source of truth.
 *
 * `(sign << 4) | code` -> a packed 4-lane pattern byte, verbatim from
 * llama.cpp PR #22836's `stq1_0_codebook`. Each lane is 2 bits
 * (0b00 = -1, 0b01 = 0, 0b10 = +1); the decoded weight is
 * ((bits) - 1) * d. The 3:4 structural rule (exactly one zero per group
 * of 4) yields 4 zero-positions x 2^3 signs = 32 patterns.
 *
 * Both the CPU dequant (llm_quant_stq1_0.c, `static const u8`) and the
 * Metal kernels (llm_metal_kernels.metal, `constant uchar`) need this
 * table but with different storage qualifiers, so this header exposes
 * only the brace initializer — there is exactly one copy of the 32
 * bytes in the tree and the two backends cannot drift apart.
 *
 * Worked examples (from the PR, both verified against these bytes):
 *   slot 0, sign 0 -> 0xA9 = 10_10_10_01 -> lanes (0, +1, +1, +1)
 *   slot 0, sign 1 -> 0x01 = 00_00_00_01 -> lanes (0, -1, -1, -1)
 */
#ifndef LLM_LLM_STQ1_0_CODEBOOK_H
#define LLM_LLM_STQ1_0_CODEBOOK_H

#define STQ1_0_CODEBOOK_INIT {                          \
    /* sign = 0 (first non-zero lane is +1) */          \
    0xA9, 0x89, 0x29, 0x09, 0xA6, 0x86, 0x26, 0x06,     \
    0x9A, 0x92, 0x1A, 0x12, 0x6A, 0x62, 0x4A, 0x42,     \
    /* sign = 1 (every non-zero lane negated) */        \
    0x01, 0x21, 0x81, 0xA1, 0x04, 0x24, 0x84, 0xA4,     \
    0x10, 0x18, 0x90, 0x98, 0x40, 0x48, 0x60, 0x68      \
}

#endif /* LLM_LLM_STQ1_0_CODEBOOK_H */
