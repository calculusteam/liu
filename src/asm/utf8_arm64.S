/*
 * Liu - ARM64 NEON-accelerated UTF-8 validation
 * Fast-path: all-ASCII check using NEON; falls back to scalar for non-ASCII.
 */

.global _utf8_validate
.extern _utf8_validate_scalar

.text
.align 4

/*
 * bool utf8_validate(const uint8_t *buf, size_t len)
 * x0 = buf, x1 = len
 * Returns: 1 if valid, 0 if invalid
 *
 * Strategy: Check 16 bytes at a time. If all bytes < 0x80 (ASCII), skip.
 * Otherwise fall back to C scalar validator for that chunk.
 */
_utf8_validate:
    cbz     x1, .Lvalid

    /* Process 16 bytes at a time */
    cmp     x1, #16
    b.lt    .Lscalar_fallback

.Lcheck_16:
    ldr     q0, [x0]
    /* Check if any byte has high bit set */
    umaxv   b1, v0.16b
    umov    w2, v1.b[0]
    tst     w2, #0x80
    b.ne    .Lscalar_fallback   /* Non-ASCII found, use C validator */

    add     x0, x0, #16
    sub     x1, x1, #16
    cmp     x1, #16
    b.ge    .Lcheck_16

    /* Remaining bytes < 16 — check individually for ASCII fast path */
    cbz     x1, .Lvalid

.Lcheck_tail:
    ldrb    w2, [x0]
    tst     w2, #0x80
    b.ne    .Lscalar_fallback
    add     x0, x0, #1
    sub     x1, x1, #1
    cbnz    x1, .Lcheck_tail

.Lvalid:
    mov     w0, #1
    ret

.Lscalar_fallback:
    /* Tail-call the scalar validator for non-ASCII input. */
    b       _utf8_validate_scalar
