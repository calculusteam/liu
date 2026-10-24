/*
 * Liu - shared constants and helpers for the Vulkan Whisper compute shaders.
 *
 * The Metal kernels are the reference; these mirror them op for op so both
 * backends can be diffed against the same CPU oracle. Two deliberate
 * differences:
 *
 *   - No cooperative-matrix ops. VK_KHR_cooperative_matrix exists but is not
 *     on every driver this is meant to reach, and a backend that only runs on
 *     recent NVIDIA defeats the point. These are plain tiled loops; a
 *     discrete GPU has enough raw throughput that they still beat the CPU
 *     path by a wide margin.
 *   - Weights arrive as a uint array rather than bytes. 8-bit storage is an
 *     extension, and the block layout needs byte addressing regardless — a
 *     Q5_0 block is 22 bytes, so nothing inside one is word-aligned.
 *
 * whisper_dequant.glsl is a separate include because its accessors read the
 * weight buffer by name: it has to come AFTER that buffer's declaration.
 */
#ifndef WHISPER_COMMON_GLSL
#define WHISPER_COMMON_GLSL

#define WM_TYPE_F32  0
#define WM_TYPE_F16  1
#define WM_TYPE_Q4_0 2
#define WM_TYPE_Q4_1 3
#define WM_TYPE_Q5_0 6
#define WM_TYPE_Q5_1 7
#define WM_TYPE_Q8_0 8

/* tanh-approximation GELU — the one Whisper was trained with. The erf form
 * differs enough to matter. */
float wmGelu(float x) {
    const float a = 0.044715;
    const float s = 0.7978845608028654;   /* sqrt(2/pi) */
    return 0.5 * x * (1.0 + tanh(s * x * (1.0 + a * x * x)));
}

#endif
