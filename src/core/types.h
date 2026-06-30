/*
 * Liu - core type definitions
 * Zero-overhead type aliases and common constants.
 */
#ifndef CORE_TYPES_H
#define CORE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef float     f32;
typedef double    f64;
typedef size_t    usize;
typedef ptrdiff_t isize;

#define KB(x) ((usize)(x) << 10)
#define MB(x) ((usize)(x) << 20)
#define GB(x) ((usize)(x) << 30)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#ifndef MIN
#define MIN(a, b)    ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#endif
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)

#define UNUSED(x) ((void)(x))

/* Alignment for SIMD operations */
#define ALIGN16 __attribute__((aligned(16)))

/* Likely/unlikely branch hints */
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Export/visibility */
#define EXPORT __attribute__((visibility("default")))

#endif /* CORE_TYPES_H */
