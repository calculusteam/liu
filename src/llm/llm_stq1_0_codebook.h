/*
 * STQ1_0 codebook shared by the CPU and Metal implementations.
 *
 * Portions derived from llama.cpp / ggml: Copyright (c) 2023-2026 The
 * ggml authors These portions are licensed under the MIT License; see
 * packaging/licenses/llama.cpp-LICENSE.txt and THIRD_PARTY_NOTICES.md.
 *
 * The sign/code pair selects a packed four-lane pattern. Each lane uses
 * two bits (00 = -1, 01 = 0, 10 = +1); the decoded weight is (bits - 1)
 * * d. The 3:4 rule yields four zero positions times eight sign
 * patterns, for 32 possible patterns.
 *
 * Examples: slot 0, sign 0 selects 0xA9 and decodes to (0, +1, +1, +1);
 * sign 1 selects 0x01 and decodes to (0, -1, -1, -1).
 *
 * Both CPU dequantization and Metal kernels use this initializer with
 * different storage qualifiers, keeping one copy of the 32-byte table.
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
