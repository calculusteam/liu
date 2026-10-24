/*
 * Liu — x86-64 SSE2 memory operations (counterpart to memops_arm64.S).
 *
 * SSE2 is part of the mandatory x86-64 baseline, so these need no CPUID gate
 * and run on every x86-64 CPU.  System V AMD64 ABI via x86_64_abi.h; leaf
 * functions, no stack frame, AT&T syntax.
 *
 *   void fast_memcpy (void *dst, const void *src, size_t n)
 *   void fast_memset (void *dst, uint8_t val,     size_t n)
 *   void fast_memzero(void *dst, size_t n)
 */

#include "x86_64_abi.h"

.text
.globl SYM(fast_memcpy)
.globl SYM(fast_memset)
.globl SYM(fast_memzero)

/* ------------------------------------------------------------------ memcpy */
/* ARG0 = dst, ARG1 = src, ARG2 = n.  Counter kept in rax. */
.p2align 4
SYM(fast_memcpy):
    mov     %ARG2, %rax              /* rax = n */
    test    %rax, %rax
    jz      .Lcpy_done

.Lcpy_64:
    cmp     $64, %rax
    jb      .Lcpy_32
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
    sub     $64, %rax
    jmp     .Lcpy_64

.Lcpy_32:
    cmp     $32, %rax
    jb      .Lcpy_16
    movdqu  0(%ARG1),  %xmm0
    movdqu  16(%ARG1), %xmm1
    movdqu  %xmm0, 0(%ARG0)
    movdqu  %xmm1, 16(%ARG0)
    add     $32, %ARG1
    add     $32, %ARG0
    sub     $32, %rax

.Lcpy_16:
    cmp     $16, %rax
    jb      .Lcpy_tiny
    movdqu  0(%ARG1), %xmm0
    movdqu  %xmm0, 0(%ARG0)
    add     $16, %ARG1
    add     $16, %ARG0
    sub     $16, %rax

.Lcpy_tiny:
    test    %rax, %rax
    jz      .Lcpy_done
.Lcpy_byte:
    mov     (%ARG1), %r10b
    mov     %r10b, (%ARG0)
    inc     %ARG1
    inc     %ARG0
    dec     %rax
    jnz     .Lcpy_byte

.Lcpy_done:
    ret

/* ------------------------------------------------------------------ memset */
/* ARG0 = dst, ARG1 = val (low byte), ARG2 = n.  Counter in rax (after the
 * byte is isolated in al and broadcast across xmm0). */
.p2align 4
SYM(fast_memset):
    mov     %ARG2, %r11             /* r11 = n */
    test    %r11, %r11
    jz      .Lset_done

    /* Broadcast the low byte of val across all 16 lanes of xmm0 (SSE2-only):
     *   al = val & 0xFF ; eax = al*0x01010101 (byte replicated 4x) ; pshufd. */
    mov     %ARG1d, %eax
    movzbl  %al, %eax
    imull   $0x01010101, %eax, %eax
    movd    %eax, %xmm0
    pshufd  $0, %xmm0, %xmm0        /* xmm0 = val x16 */
    /* keep the scalar byte for the tail */
    movzbl  %al, %r10d

.Lset_64:
    cmp     $64, %r11
    jb      .Lset_16
    movdqu  %xmm0, 0(%ARG0)
    movdqu  %xmm0, 16(%ARG0)
    movdqu  %xmm0, 32(%ARG0)
    movdqu  %xmm0, 48(%ARG0)
    add     $64, %ARG0
    sub     $64, %r11
    jmp     .Lset_64

.Lset_16:
    cmp     $16, %r11
    jb      .Lset_tiny
    movdqu  %xmm0, 0(%ARG0)
    add     $16, %ARG0
    sub     $16, %r11
    jmp     .Lset_16

.Lset_tiny:
    test    %r11, %r11
    jz      .Lset_done
.Lset_byte:
    mov     %r10b, (%ARG0)
    inc     %ARG0
    dec     %r11
    jnz     .Lset_byte

.Lset_done:
    ret

/* ----------------------------------------------------------------- memzero */
/* ARG0 = dst, ARG1 = n.  xmm0 = 0 via pxor; plain SSE2 stores (x86 has no
 * direct DC-ZVA analog — non-temporal stores only help past cache size). */
.p2align 4
SYM(fast_memzero):
    mov     %ARG1, %rax             /* rax = n */
    test    %rax, %rax
    jz      .Lzero_done
    pxor    %xmm0, %xmm0

.Lzero_64:
    cmp     $64, %rax
    jb      .Lzero_16
    movdqu  %xmm0, 0(%ARG0)
    movdqu  %xmm0, 16(%ARG0)
    movdqu  %xmm0, 32(%ARG0)
    movdqu  %xmm0, 48(%ARG0)
    add     $64, %ARG0
    sub     $64, %rax
    jmp     .Lzero_64

.Lzero_16:
    cmp     $16, %rax
    jb      .Lzero_tiny
    movdqu  %xmm0, 0(%ARG0)
    add     $16, %ARG0
    sub     $16, %rax
    jmp     .Lzero_16

.Lzero_tiny:
    test    %rax, %rax
    jz      .Lzero_done
.Lzero_byte:
    movb    $0, (%ARG0)
    inc     %ARG0
    dec     %rax
    jnz     .Lzero_byte

.Lzero_done:
    ret
