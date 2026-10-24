/*
 * Liu - overflow-checked size arithmetic
 *
 * Header-only helpers for safe count*elem_size style computations.
 * Prefer checked_mul_usize when you need a bool + out product; use
 * size_mul when a zero product is enough to signal failure (callers
 * must treat 0 as fail only when both operands are non-zero).
 */
#ifndef CORE_CHECKED_H
#define CORE_CHECKED_H

#include "types.h"

/* True and *out = a*b when the product fits in usize; false on overflow
 * (*out is set to 0 if out is non-NULL). a==0 or b==0 always succeeds
 * with product 0. */
static inline bool checked_mul_usize(usize a, usize b, usize *out) {
    if (a != 0 && b > (usize)SIZE_MAX / a) {
        if (out) *out = 0;
        return false;
    }
    if (out) *out = a * b;
    return true;
}

/* Alias: same as checked_mul_usize. */
static inline bool size_mul_ok(usize a, usize b, usize *out) {
    return checked_mul_usize(a, b, out);
}

/* Returns a*b, or 0 on overflow. Callers must treat 0 as failure when
 * both a and b are non-zero (0*x and x*0 legitimately yield 0). */
static inline usize size_mul(usize a, usize b) {
    usize n;
    if (!checked_mul_usize(a, b, &n)) return 0;
    return n;
}

/* True and *out = a+b when the sum fits; false on overflow. */
static inline bool checked_add_usize(usize a, usize b, usize *out) {
    if (b > (usize)SIZE_MAX - a) {
        if (out) *out = 0;
        return false;
    }
    if (out) *out = a + b;
    return true;
}

/* Parse a signed decimal i32 without strtol/atoi locale or overflow hazards.
 * Leading ASCII whitespace and an optional sign are accepted. end_out, when
 * non-NULL, receives the first byte after the digits. */
static inline bool checked_parse_i32_prefix(const char *s, i32 *out,
                                            const char **end_out) {
    if (!s || !out) return false;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    bool neg = false;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    if (*s < '0' || *s > '9') return false;

    u32 value = 0;
    const u32 limit = neg ? (u32)INT32_MAX + 1u : (u32)INT32_MAX;
    do {
        u32 digit = (u32)(*s - '0');
        if (value > (limit - digit) / 10u) return false;
        value = value * 10u + digit;
        s++;
    } while (*s >= '0' && *s <= '9');

    *out = neg ? (value == (u32)INT32_MAX + 1u ? INT32_MIN : -(i32)value)
               : (i32)value;
    if (end_out) *end_out = s;
    return true;
}

/* Strict sibling: only trailing ASCII whitespace may follow the number. */
static inline bool checked_parse_i32(const char *s, i32 *out) {
    const char *end;
    if (!checked_parse_i32_prefix(s, out, &end)) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    return *end == '\0';
}

#endif /* CORE_CHECKED_H */
