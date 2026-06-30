/*
 * Liu — x86-64 SSE2 UTF-8 validation (counterpart to utf8_arm64.S).
 *
 *   bool utf8_validate(const uint8_t *buf, size_t len)   ARG0 = buf, ARG1 = len
 *   returns eax = 1 (valid) / 0 (invalid)
 *
 * Fast path: scan 16 bytes at a time; if every byte is ASCII (<0x80) skip the
 * block.  The first non-ASCII byte hands off to the C scalar validator with
 * (current position, remaining length) — the skipped prefix was all 1-byte
 * ASCII, so validating the remainder from there is equivalent.  This mirrors
 * the ARM64 NEON version exactly.
 */

#include "x86_64_abi.h"

.text
.globl  SYM(utf8_validate)
.extern SYM(utf8_validate_scalar)

.p2align 4
SYM(utf8_validate):
    test    %ARG1, %ARG1
    jz      .Lvalid                 /* len == 0 → valid */
    mov     %ARG1, %rax             /* rax = remaining length */
    cmp     $16, %rax
    jb      .Lutf_tail

.Lutf_16:
    movdqu  (%ARG0), %xmm0
    pmovmskb %xmm0, %r10d           /* bit i = high bit of byte i */
    test    %r10d, %r10d
    jnz     .Lscalar                /* some byte >= 0x80 → scalar */
    add     $16, %ARG0
    sub     $16, %rax
    cmp     $16, %rax
    jae     .Lutf_16

.Lutf_tail:
    test    %rax, %rax
    jz      .Lvalid
.Lutf_tail_loop:
    mov     (%ARG0), %r10b
    test    $0x80, %r10b
    jnz     .Lscalar
    inc     %ARG0
    dec     %rax
    jnz     .Lutf_tail_loop

.Lvalid:
    mov     $1, %eax
    ret

.Lscalar:
    /* Tail-call utf8_validate_scalar(ARG0 = current pos, ARG1 = remaining).
     * ARG0 is already advanced past the ASCII prefix; sync ARG1 from rax. */
    mov     %rax, %ARG1
    jmp     SYM(utf8_validate_scalar)
