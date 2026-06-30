/*
 * Liu - ARM64 NEON terminal buffer operations
 * Accelerated buffer clearing, row scrolling, and cell operations.
 *
 * Cell struct layout (16 bytes):
 *   u32 codepoint          (4 bytes, offset  0)
 *   u32 fg                 (4 bytes, offset  4)   } CellAttr
 *   u32 bg                 (4 bytes, offset  8)   }
 *   u16 flags              (2 bytes, offset 12)   }
 *   u16 _pad               (2 bytes, offset 14)   }
 */

.global _asm_buffer_clear_row
.global _asm_buffer_scroll_up
.global _asm_buffer_copy_row
.global _asm_find_printable_run

.text
.align 4

/*
 * void asm_buffer_clear_row(void *row_start, int cols, u32 codepoint, u32 fg, u32 bg)
 * Clears a row of terminal cells to a uniform value.
 * x0 = row_start (Cell*), w1 = cols, w2 = codepoint, w3 = fg, w4 = bg
 *
 * Each Cell is 16 bytes:
 *   v0 = codepoint(4)|fg(4)|bg(4)|flags(2)|_pad(2)
 *
 * 4x unroll: 4 cells = 64 bytes per iteration.
 */
_asm_buffer_clear_row:
    cbz     w1, .Lclear_done

    /* Build the 16-byte Cell payload: codepoint | fg | bg | 0 */
    orr     x5, x2, x3, lsl #32    /* lower 8: codepoint(32) | fg(32) */
    mov     x6, x4                  /* upper 8: bg(32) | flags(16)=0 | _pad(16)=0 */
    fmov    d0, x5
    mov     v0.d[1], x6

    /* 4x unroll: process 4 cells (64 bytes) per iteration. */
    lsr     w7, w1, #2              /* w7 = cols / 4 */
    cbz     w7, .Lclear_tail

.Lclear_4:
    str     q0, [x0]                /* Cell 0: bytes  0-15 */
    str     q0, [x0, #16]           /* Cell 1: bytes 16-31 */
    str     q0, [x0, #32]           /* Cell 2: bytes 32-47 */
    str     q0, [x0, #48]           /* Cell 3: bytes 48-63 */
    add     x0, x0, #64
    subs    w7, w7, #1
    b.ne    .Lclear_4

.Lclear_tail:
    and     w7, w1, #3              /* remaining = cols % 4 */
    cbz     w7, .Lclear_done

.Lclear_tail_loop:
    str     q0, [x0], #16           /* trailing cell: 16 bytes */
    subs    w7, w7, #1
    b.ne    .Lclear_tail_loop

.Lclear_done:
    ret

/*
 * void asm_buffer_copy_row(void *dst, const void *src, int cols)
 * Copy one row of cells using NEON loads/stores.
 * x0 = dst, x1 = src, w2 = cols
 *
 * Each Cell = 16 bytes. Process 4 cells (64 bytes) per iteration.
 */
_asm_buffer_copy_row:
    cbz     w2, .Lcopyrow_done

    /* Process 4 cells (64 bytes) at a time. */
    lsr     w3, w2, #2      /* w3 = cols / 4 */
    cbz     w3, .Lcopyrow_tail

.Lcopyrow_64:
    ldp     q0, q1, [x1]
    ldp     q2, q3, [x1, #32]
    stp     q0, q1, [x0]
    stp     q2, q3, [x0, #32]
    add     x0, x0, #64
    add     x1, x1, #64
    subs    w3, w3, #1
    b.ne    .Lcopyrow_64

.Lcopyrow_tail:
    and     w3, w2, #3      /* remaining = cols % 4 */
    cbz     w3, .Lcopyrow_done

.Lcopyrow_tail_loop:
    ldr     q0, [x1], #16
    str     q0, [x0], #16
    subs    w3, w3, #1
    b.ne    .Lcopyrow_tail_loop

.Lcopyrow_done:
    ret

/*
 * void asm_buffer_scroll_up(void *buffer, int rows, int cols, int n)
 * Scroll buffer up by n rows (move rows n..end to 0..end-n).
 * x0 = buffer, w1 = total rows, w2 = cols, w3 = n (rows to scroll)
 */
_asm_buffer_scroll_up:
    sub     w4, w1, w3          /* w4 = rows to move = total_rows - n */
    cmp     w4, #0
    b.le    .Lscrollup_done

    /* Bytes per row = cols * 16 (sizeof(Cell) = 16) */
    lsl     w5, w2, #4          /* w5 = cols * 16 */

    /* src = buffer + n * row_bytes */
    umull   x6, w3, w5          /* x6 = n * row_bytes */
    add     x1, x0, x6          /* x1 = src */

    /* total bytes to copy = move_rows * row_bytes */
    umull   x7, w4, w5          /* x7 = move_rows * row_bytes */

    /* Use NEON copy (64 bytes per iteration) */
    lsr     x8, x7, #6          /* iterations = total / 64 */
    cbz     x8, .Lscrollup_tail

.Lscrollup_64:
    ldp     q0, q1, [x1]
    ldp     q2, q3, [x1, #32]
    stp     q0, q1, [x0]
    stp     q2, q3, [x0, #32]
    add     x0, x0, #64
    add     x1, x1, #64
    subs    x8, x8, #1
    b.ne    .Lscrollup_64

.Lscrollup_tail:
    and     x8, x7, #63         /* remaining bytes */
    cbz     x8, .Lscrollup_done

.Lscrollup_byte:
    ldrb    w9, [x1], #1
    strb    w9, [x0], #1
    subs    x8, x8, #1
    b.ne    .Lscrollup_byte

.Lscrollup_done:
    ret

/*
 * int asm_find_printable_run(const u8 *data, int len)
 * Find the length of a run of printable ASCII bytes (0x20-0x7E).
 * Used to fast-path bulk character output in the VT parser.
 * x0 = data, w1 = len
 * Returns: length of printable run
 *
 * Apple Silicon optimization: uses vector range compare plus a reduction
 * to detect non-printable bytes before a scalar exact-position scan.
 */
_asm_find_printable_run:
    cbz     w1, .Lrun_zero
    mov     w2, #0              /* counter */

    /* NEON path: check 16 bytes at a time */
    cmp     w1, #16
    b.lt    .Lrun_scalar

    /* Constants: range [0x20, 0x7E] — check via unsigned subtraction */
    movi    v1.16b, #0x20       /* low bound */
    movi    v2.16b, #0x5E       /* 0x7E - 0x20 = range width */

.Lrun_16:
    ldr     q0, [x0, x2]
    /* byte - 0x20 → unsigned: if byte < 0x20, wraps to large value */
    sub     v3.16b, v0.16b, v1.16b
    /* Compare: if (byte - 0x20) > 0x5E, then byte not in [0x20,0x7E] → high bit set */
    cmhi    v4.16b, v3.16b, v2.16b
    /* Reduce the 16-byte compare mask: any non-zero lane means failure. */
    umaxv   b5, v4.16b
    umov    w3, v5.b[0]
    cbnz    w3, .Lrun_partial  /* Some byte out of range */

    add     w2, w2, #16
    sub     w1, w1, #16
    cmp     w1, #16
    b.ge    .Lrun_16
    b       .Lrun_scalar

.Lrun_partial:
    /* Found a non-printable in this 16-byte block. Find exact position. */
    /* v4 has 0xFF at non-printable positions, 0x00 at printable */
    /* Count leading printable bytes */
.Lrun_partial_scan:
    cmp     w1, #0
    b.le    .Lrun_done
    ldrb    w3, [x0, x2]
    cmp     w3, #0x20
    b.lt    .Lrun_done
    cmp     w3, #0x7E
    b.gt    .Lrun_done
    add     w2, w2, #1
    sub     w1, w1, #1
    b       .Lrun_partial_scan

.Lrun_scalar:
    cmp     w1, #0
    b.le    .Lrun_done

    ldrb    w3, [x0, x2]
    cmp     w3, #0x20
    b.lt    .Lrun_done
    cmp     w3, #0x7E
    b.gt    .Lrun_done

    add     w2, w2, #1
    sub     w1, w1, #1
    b       .Lrun_scalar

.Lrun_done:
    mov     w0, w2
    ret

.Lrun_zero:
    mov     w0, #0
    ret
