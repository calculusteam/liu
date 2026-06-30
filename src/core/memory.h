/*
 * Liu - arena memory allocator
 * Fast bump allocation with zero fragmentation.
 */
#ifndef CORE_MEMORY_H
#define CORE_MEMORY_H

#include "types.h"

/* Arena allocator - linear bump allocator */
typedef struct {
    u8    *base;
    usize  size;
    usize  used;
    usize  peak;
} Arena;

Arena  arena_create(usize size);
void   arena_destroy(Arena *a);
void  *arena_alloc(Arena *a, usize size);
void  *arena_alloc_aligned(Arena *a, usize size, usize align);
void   arena_reset(Arena *a);
usize  arena_remaining(const Arena *a);

/* Temp arena - save/restore point for scratch allocations */
typedef struct {
    Arena *arena;
    usize  saved_used;
} ArenaTemp;

ArenaTemp arena_temp_begin(Arena *a);
void      arena_temp_end(ArenaTemp t);

/* Zero a buffer in a way the compiler cannot dead-store eliminate.
 * Use for scrubbing passphrases, passwords, key material before free. */
void  secure_zero(void *dst, usize n);

#endif /* CORE_MEMORY_H */
