/*
 * Liu — x86-64 SSE2 terminal buffer operations (counterpart to
 * terminal_arm64.S).  Cell layout (16 bytes):
 *     u32 codepoint (0) | u32 fg (4) | u32 bg (8) | u16 flags (12) | u16 pad (14)
 *
 *   void asm_buffer_clear_row (void *row, int cols, u32 cp, u32 fg, u32 bg)
 *   void asm_buffer_copy_row  (void *dst, const void *src, int cols)
 *   void asm_buffer_scroll_up (void *buffer, int rows, int cols, int n)
 *   int  asm_find_printable_run(const u8 *data, int len)
 */

#include "x86_64_abi.h"

/* The 5th integer argument (clear_row's bg) is passed in r8d under the
 * System V AMD64 ABI. */
#define LOAD_BG_EAX  mov %r8d, %eax

.text
.globl SYM(asm_buffer_clear_row)
.globl SYM(asm_buffer_copy_row)
.globl SYM(asm_buffer_scroll_up)
.globl SYM(asm_find_printable_run)

/* -------------------------------------------------------------- clear_row */
/* ARG0 = row, ARG1d = cols, ARG2d = cp, ARG3d = fg, arg4 = bg. */
.p2align 4
SYM(asm_buffer_clear_row):
    test    %ARG1d, %ARG1d
    jz      .Lclear_done
    /* Assemble the 16-byte cell into xmm0: [cp|fg | bg|0]. */
    mov     %ARG2d, %eax            /* cp (zero-extended) */
    mov     %ARG3d, %r10d           /* fg */
    shl     $32, %r10
    or      %r10, %rax              /* rax = cp | (fg << 32) */
    movq    %rax, %xmm0             /* low 8 bytes */
    LOAD_BG_EAX                     /* eax = bg (zero-extended into rax) */
    movq    %rax, %xmm1             /* low 8 of xmm1 = bg | 0 (flags/pad = 0) */
    punpcklqdq %xmm1, %xmm0         /* xmm0 = full cell pattern */
    /* 4x unroll: 4 cells (64 bytes) per iteration. */
    mov     %ARG1d, %eax            /* eax = cols */
    mov     %eax, %r10d
    shr     $2, %r10d               /* r10d = cols / 4 */
    jz      .Lclear_tail
.Lclear_4:
    movdqu  %xmm0, 0(%ARG0)
    movdqu  %xmm0, 16(%ARG0)
    movdqu  %xmm0, 32(%ARG0)
    movdqu  %xmm0, 48(%ARG0)
    add     $64, %ARG0
    dec     %r10d
    jnz     .Lclear_4
.Lclear_tail:
    and     $3, %eax                /* cols % 4 */
    jz      .Lclear_done
.Lclear_tail_loop:
    movdqu  %xmm0, 0(%ARG0)
    add     $16, %ARG0
    dec     %eax
    jnz     .Lclear_tail_loop
.Lclear_done:
    ret

/* --------------------------------------------------------------- copy_row */
/* ARG0 = dst, ARG1 = src, ARG2d = cols.  16 bytes/cell, 64 bytes/iteration. */
.p2align 4
SYM(asm_buffer_copy_row):
    mov     %ARG2d, %eax            /* cols */
    test    %eax, %eax
    jz      .Lcopyrow_done
    mov     %eax, %r10d
    shr     $2, %r10d               /* groups of 4 cells */
    jz      .Lcopyrow_tail
.Lcopyrow_64:
    movdqu  0(%ARG1),  %xmm0
    movdqu  16(%ARG1), %xmm1
    movdqu  32(%ARG1), %xmm2
    movdqu  48(%ARG1), %xmm3
    movdqu  %xmm0, 0(%ARG0)
    movdqu  %xmm1, 16(%ARG0)
    movdqu  %xmm2, 32(%ARG0)
    movdqu  %xmm3, 48(%ARG0)
    add     $64, %ARG1
    add     $64, %ARG0
    dec     %r10d
    jnz     .Lcopyrow_64
.Lcopyrow_tail:
    and     $3, %eax                /* cols % 4 */
    jz      .Lcopyrow_done
.Lcopyrow_tail_loop:
    movdqu  (%ARG1), %xmm0
    movdqu  %xmm0, (%ARG0)
    add     $16, %ARG1
    add     $16, %ARG0
    dec     %eax
    jnz     .Lcopyrow_tail_loop
.Lcopyrow_done:
    ret

/* -------------------------------------------------------------- scroll_up */
/* ARG0 = buffer (dst), ARG1d = rows, ARG2d = cols, ARG3d = n.
 * Move rows [n..rows) to [0..rows-n): forward copy, src > dst (safe). */
.p2align 4
SYM(asm_buffer_scroll_up):
    mov     %ARG1d, %eax
    sub     %ARG3d, %eax            /* move_rows = rows - n (signed) */
    jle     .Lscroll_done
    mov     %eax, %eax              /* zero-extend move_rows into rax */
    mov     %ARG2d, %r10d           /* cols */
    shl     $4, %r10                /* r10 = cols*16 = row_bytes */
    mov     %ARG3d, %r11d           /* n */
    imul    %r10, %r11              /* r11 = n * row_bytes */
    lea     (%ARG0,%r11), %r11      /* r11 = src = buffer + n*row_bytes */
    imul    %r10, %rax              /* rax = total bytes to copy */
.Lscroll_64:
    cmp     $64, %rax
    jb      .Lscroll_16
    movdqu  0(%r11),  %xmm0
    movdqu  16(%r11), %xmm1
    movdqu  32(%r11), %xmm2
    movdqu  48(%r11), %xmm3
    movdqu  %xmm0, 0(%ARG0)
    movdqu  %xmm1, 16(%ARG0)
    movdqu  %xmm2, 32(%ARG0)
    movdqu  %xmm3, 48(%ARG0)
    add     $64, %r11
    add     $64, %ARG0
    sub     $64, %rax
    jmp     .Lscroll_64
.Lscroll_16:
    cmp     $16, %rax
    jb      .Lscroll_byte
    movdqu  0(%r11), %xmm0
    movdqu  %xmm0, 0(%ARG0)
    add     $16, %r11
    add     $16, %ARG0
    sub     $16, %rax
    jmp     .Lscroll_16
.Lscroll_byte:
    test    %rax, %rax
    jz      .Lscroll_done
.Lscroll_byte_loop:
    mov     (%r11), %r10b
    mov     %r10b, (%ARG0)
    inc     %r11
    inc     %ARG0
    dec     %rax
    jnz     .Lscroll_byte_loop
.Lscroll_done:
    ret

/* ------------------------------------------------------- find_printable_run */
/* ARG0 = data, ARG1d = len.  Returns eax = length of the leading run of
 * printable ASCII (0x20..0x7E).  SSE2 unsigned range check: (byte-0x20)
 * saturating-minus 0x5E is zero iff the byte is in [0x20, 0x7E]. */
.p2align 4
SYM(asm_find_printable_run):
    xor     %eax, %eax              /* count = 0 (also the running offset) */
    test    %ARG1d, %ARG1d
    jle     .Lrun_ret               /* len <= 0 → 0 */
    mov     %ARG1d, %r11d           /* remaining */
    mov     $0x20202020, %r10d
    movd    %r10d, %xmm2
    pshufd  $0, %xmm2, %xmm2        /* xmm2 = 0x20 x16 */
    mov     $0x5E5E5E5E, %r10d
    movd    %r10d, %xmm3
    pshufd  $0, %xmm3, %xmm3        /* xmm3 = 0x5E x16 */
.Lrun_loop16:
    cmp     $16, %r11d
    jb      .Lrun_scalar
    movdqu  (%ARG0,%rax), %xmm0
    psubb   %xmm2, %xmm0           /* byte - 0x20 */
    movdqa  %xmm0, %xmm1
    psubusb %xmm3, %xmm1          /* max(0, (byte-0x20) - 0x5E); 0 iff in range */
    pxor    %xmm0, %xmm0
    pcmpeqb %xmm0, %xmm1          /* 0xFF where in range, 0x00 where out */
    pmovmskb %xmm1, %r10d          /* bit i = 1 where byte i in range */
    xor     $0xFFFF, %r10d         /* bit i = 1 where byte i OUT of range */
    jnz     .Lrun_partial
    add     $16, %rax
    sub     $16, %r11d
    jmp     .Lrun_loop16
.Lrun_partial:
    bsf     %r10d, %r10d           /* index of first out-of-range byte (0..15) */
    add     %r10d, %eax
    jmp     .Lrun_ret
.Lrun_scalar:
    test    %r11d, %r11d
    jz      .Lrun_ret
.Lrun_scalar_loop:
    movzbl  (%ARG0,%rax), %r10d
    cmp     $0x20, %r10d
    jb      .Lrun_ret
    cmp     $0x7E, %r10d
    ja      .Lrun_ret
    inc     %rax
    dec     %r11d
    jnz     .Lrun_scalar_loop
.Lrun_ret:
    ret                            /* eax = count */
