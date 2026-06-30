/*
 * Liu — shared helpers for the hand-written x86-64 SIMD sources.
 *
 * The *_x86_64.S files run through the C preprocessor (capital ".S"), so this
 * header is #included to abstract the two things that vary by target:
 *
 *   1. Symbol naming.  Mach-O (Apple) prefixes C symbols with '_';
 *      ELF (Linux) does not.
 *
 *   2. Calling convention.  Integer arguments under the System V AMD64 ABI
 *      (Linux/macOS) are passed in: rdi, rsi, rdx, rcx, r8, r9.  We alias the
 *      first four as ARG0..ARG3 (and their 32-bit sub-registers).
 *
 * Every routine here is a LEAF function that touches only the ARGn registers
 * plus rax / r10 / r11 and xmm0..xmm5 — all caller-saved (volatile) under the
 * System V ABI.  That means: no prologue, no callee-save spills, no stack
 * frame, and no dependence on the red zone beyond what leaf functions may use.
 *
 * The 5th integer argument (only asm_buffer_clear_row needs one) lives in r8
 * — see LOAD_BG_EAX in terminal_x86_64.S.
 *
 * Register-alias macros are intentionally defined WITHOUT a leading '%': the
 * sources write "%ARG0", and cpp expands the ARG0 token even when preceded by
 * '%', yielding e.g. "%rdi".  (Verified against clang/gas.)
 */

#ifndef LIU_X86_64_ABI_H
#define LIU_X86_64_ABI_H

#if defined(__APPLE__)
#  define SYM(name) _##name
#else
#  define SYM(name) name
#endif

/* System V AMD64 (Linux, macOS) integer-argument registers. */
#define ARG0  rdi
#define ARG1  rsi
#define ARG2  rdx
#define ARG3  rcx
#define ARG0d edi
#define ARG1d esi
#define ARG2d edx
#define ARG3d ecx

/* Mark the stack non-executable on ELF targets (no-op elsewhere). */
#if defined(__linux__) || defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif

#endif /* LIU_X86_64_ABI_H */
