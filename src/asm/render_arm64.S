/*
 * Liu - ARM64 NEON rendering acceleration
 * SIMD-optimized vertex batch building for terminal grid rendering.
 */

.global _asm_build_rect_vertices
.global _asm_color_to_float4

.text
.align 4

/*
 * void asm_build_rect_vertices(float *out, float x, float y, float w, float h,
 *                               float r, float g, float b, float a)
 * Build 6 vertices (2 triangles) for a rectangle.
 * Each vertex = 6 floats (x, y, r, g, b, a) = 36 floats total.
 * x0 = out, s0=x, s1=y, s2=w, s3=h, s4=r, s5=g, s6=b, s7=a
 */
_asm_build_rect_vertices:
    /* Compute x+w and y+h */
    fadd    s8, s0, s2          /* x+w */
    fadd    s9, s1, s3          /* y+h */

    /* Vertex 0: (x, y, r, g, b, a) */
    stp     s0, s1, [x0]
    stp     s4, s5, [x0, #8]
    stp     s6, s7, [x0, #16]

    /* Vertex 1: (x+w, y, r, g, b, a) */
    stp     s8, s1, [x0, #24]
    stp     s4, s5, [x0, #32]
    stp     s6, s7, [x0, #40]

    /* Vertex 2: (x+w, y+h, r, g, b, a) */
    stp     s8, s9, [x0, #48]
    stp     s4, s5, [x0, #56]
    stp     s6, s7, [x0, #64]

    /* Vertex 3: (x, y, r, g, b, a) = same as v0 */
    stp     s0, s1, [x0, #72]
    stp     s4, s5, [x0, #80]
    stp     s6, s7, [x0, #88]

    /* Vertex 4: (x+w, y+h, r, g, b, a) = same as v2 */
    stp     s8, s9, [x0, #96]
    stp     s4, s5, [x0, #104]
    stp     s6, s7, [x0, #112]

    /* Vertex 5: (x, y+h, r, g, b, a) */
    stp     s0, s9, [x0, #120]
    stp     s4, s5, [x0, #128]
    stp     s6, s7, [x0, #136]

    ret

/*
 * void asm_color_to_float4(u32 packed_color, float *out)
 * Convert packed ARGB or ANSI index to float4 RGBA.
 * w0 = color value (0x01RRGGBB for truecolor, or ANSI index)
 * x1 = output float[4]
 */
_asm_color_to_float4:
    /* Check if truecolor (bit 24 set) */
    tst     w0, #0x01000000
    b.eq    .Lcolor_ansi

    /* Truecolor: extract R, G, B */
    ubfx    w2, w0, #16, #8    /* R */
    ubfx    w3, w0, #8, #8     /* G */
    and     w4, w0, #0xFF      /* B */

    /* Convert to float (divide by 255) */
    ucvtf   s0, w2
    ucvtf   s1, w3
    ucvtf   s2, w4
    fmov    s3, #1.0            /* alpha */

    mov     w5, #255
    ucvtf   s4, w5
    fdiv    s0, s0, s4
    fdiv    s1, s1, s4
    fdiv    s2, s2, s4

    stp     s0, s1, [x1]
    stp     s2, s3, [x1, #8]
    ret

.Lcolor_ansi:
    /* ANSI palette lookup from g_ansi_colors[]. */
    mov     w8, #7
    cmp     w0, #256
    csel    w8, w0, w8, lo
    adrp    x9, _g_ansi_colors@GOTPAGE
    ldr     x9, [x9, _g_ansi_colors@GOTPAGEOFF]
    add     x8, x9, w8, uxtw #4
    ldr     d0, [x8]
    ldp     s2, s3, [x8, #8]
    mov     s1, v0[1]
    stp     s0, s1, [x1]
    stp     s2, s3, [x1, #8]
    ret
