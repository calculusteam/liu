/*
 * Liu — ARM64 SIMD memory operations
 * Generic AArch64 path with NEON vector loads/stores plus optional DC ZVA
 * zeroing when the current CPU exposes a usable zeroing granule.
 */

.global _fast_memcpy
.global _fast_memset
.global _fast_memzero

.text
.align 4

/*
 * void fast_memcpy(void *dst, const void *src, size_t n)
 * x0 = dst, x1 = src, x2 = n
 */
_fast_memcpy:
    cbz     x2, .Lcopy_done

    /* Small sizes: avoid loop overhead */
    cmp     x2, #16
    b.lt    .Lcopy_tiny
    cmp     x2, #32
    b.lt    .Lcopy_16
    cmp     x2, #64
    b.lt    .Lcopy_32
    cmp     x2, #128
    b.lt    .Lcopy_64
    cmp     x2, #256
    b.lt    .Lcopy_128

    /* 256-byte inner loop — good throughput on modern AArch64 cores */
.Lcopy_256:
    prfm    PLDL1KEEP, [x1, #256]
    ldp     q0, q1, [x1]
    ldp     q2, q3, [x1, #32]
    ldp     q4, q5, [x1, #64]
    ldp     q6, q7, [x1, #96]
    stp     q0, q1, [x0]
    stp     q2, q3, [x0, #32]
    stp     q4, q5, [x0, #64]
    stp     q6, q7, [x0, #96]
    ldp     q0, q1, [x1, #128]
    ldp     q2, q3, [x1, #160]
    ldp     q4, q5, [x1, #192]
    ldp     q6, q7, [x1, #224]
    stp     q0, q1, [x0, #128]
    stp     q2, q3, [x0, #160]
    stp     q4, q5, [x0, #192]
    stp     q6, q7, [x0, #224]
    add     x0, x0, #256
    add     x1, x1, #256
    sub     x2, x2, #256
    cmp     x2, #256
    b.ge    .Lcopy_256

    cbz     x2, .Lcopy_done
    cmp     x2, #128
    b.lt    .Lcopy_tail64

.Lcopy_128:
    ldp     q0, q1, [x1]
    ldp     q2, q3, [x1, #32]
    ldp     q4, q5, [x1, #64]
    ldp     q6, q7, [x1, #96]
    stp     q0, q1, [x0]
    stp     q2, q3, [x0, #32]
    stp     q4, q5, [x0, #64]
    stp     q6, q7, [x0, #96]
    add     x0, x0, #128
    add     x1, x1, #128
    sub     x2, x2, #128
    cbz     x2, .Lcopy_done

.Lcopy_tail64:
    cmp     x2, #64
    b.lt    .Lcopy_tail32

.Lcopy_64:
    ldp     q0, q1, [x1]
    ldp     q2, q3, [x1, #32]
    stp     q0, q1, [x0]
    stp     q2, q3, [x0, #32]
    add     x0, x0, #64
    add     x1, x1, #64
    sub     x2, x2, #64
    cbz     x2, .Lcopy_done

.Lcopy_tail32:
    cmp     x2, #32
    b.lt    .Lcopy_tail16

.Lcopy_32:
    ldp     q0, q1, [x1]
    stp     q0, q1, [x0]
    add     x0, x0, #32
    add     x1, x1, #32
    sub     x2, x2, #32
    cbz     x2, .Lcopy_done

.Lcopy_tail16:
    cmp     x2, #16
    b.lt    .Lcopy_tiny

.Lcopy_16:
    ldr     q0, [x1]
    str     q0, [x0]
    add     x0, x0, #16
    add     x1, x1, #16
    sub     x2, x2, #16
    cbz     x2, .Lcopy_done

.Lcopy_tiny:
    /* 1-15 bytes: byte-by-byte */
    ldrb    w3, [x1], #1
    strb    w3, [x0], #1
    subs    x2, x2, #1
    b.ne    .Lcopy_tiny

.Lcopy_done:
    ret

/*
 * void fast_memset(void *dst, uint8_t val, size_t n)
 * x0 = dst, w1 = val, x2 = n
 */
_fast_memset:
    cbz     x2, .Lset_done
    dup     v0.16b, w1
    cmp     x2, #64
    b.lt    .Lset_small

.Lset_64:
    stp     q0, q0, [x0]
    stp     q0, q0, [x0, #32]
    add     x0, x0, #64
    sub     x2, x2, #64
    cmp     x2, #64
    b.ge    .Lset_64

.Lset_small:
    cmp     x2, #16
    b.lt    .Lset_tiny
    str     q0, [x0]
    add     x0, x0, #16
    sub     x2, x2, #16
    b       .Lset_small

.Lset_tiny:
    cbz     x2, .Lset_done
    strb    w1, [x0], #1
    subs    x2, x2, #1
    b.ne    .Lset_tiny

.Lset_done:
    ret

/*
 * void fast_memzero(void *dst, size_t n)
 * x0 = dst, x1 = n
 *
 * Uses dc zva when available: zeroes an entire CPU-defined cache block
 * without loading it first. Falls back to NEON stores otherwise.
 */
_fast_memzero:
    cbz     x1, .Lzero_done
    cmp     x1, #64
    b.lt    .Lzero_small

    /* Read DC ZVA capabilities.
     * dczid_el0[4] = DZP (prohibited), bits[3:0] encode log2(words). */
    mrs     x9, dczid_el0
    tbnz    x9, #4, .Lzero_small
    and     x10, x9, #15
    mov     x11, #4
    lsl     x11, x11, x10       /* granule bytes = 4 << log2(words) */
    cmp     x11, #16
    b.lt    .Lzero_small

    /* Align dst to the discovered ZVA granule. */
    movi    v0.16b, #0
    sub     x12, x11, #1
    and     x2, x0, x12            /* misalignment = dst & (granule - 1) */
    cbz     x2, .Lzero_aligned
    sub     x2, x2, x11
    neg     x2, x2                 /* bytes to align = granule - misalignment */
    cmp     x2, x1
    csel    x2, x2, x1, ls        /* min(align_bytes, n) */
    sub     x1, x1, x2

.Lzero_head:
    strb    wzr, [x0], #1
    subs    x2, x2, #1
    b.ne    .Lzero_head

.Lzero_aligned:
    /* Zero one CPU-defined block per iteration. */
    cmp     x1, x11
    b.lt    .Lzero_small

.Lzero_zva:
    dc      zva, x0
    add     x0, x0, x11
    sub     x1, x1, x11
    cmp     x1, x11
    b.ge    .Lzero_zva

.Lzero_small:
    /* Remaining bytes: NEON 16-byte stores */
    movi    v0.16b, #0
    cmp     x1, #16
    b.lt    .Lzero_tiny

.Lzero_16:
    str     q0, [x0]
    add     x0, x0, #16
    sub     x1, x1, #16
    cmp     x1, #16
    b.ge    .Lzero_16

.Lzero_tiny:
    cbz     x1, .Lzero_done
    strb    wzr, [x0], #1
    subs    x1, x1, #1
    b.ne    .Lzero_tiny

.Lzero_done:
    ret
