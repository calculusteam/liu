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
    /* Allocations REFUSED since arena_create/arena_reset. A bump allocator
     * that is out of room returns NULL and nothing else: a caller that pushes
     * many entries (a parse, a scan) and checks each one individually still
     * cannot say afterwards whether the whole run fit. Compare before and after
     * — a non-zero delta means the result is INCOMPLETE, whatever it looks
     * like (md_parse returned a valid, empty document for a 180 KB note this
     * way). */
    u32    refused;
} Arena;

Arena  arena_create(usize size);
void   arena_destroy(Arena *a);
void  *arena_alloc(Arena *a, usize size);
void  *arena_alloc_aligned(Arena *a, usize size, usize align);
/* count * elem_size with overflow check, then arena_alloc.
 * Returns NULL on overflow or OOM. count==0 or elem_size==0 yields a
 * zero-size bump (may return a non-NULL pointer). */
void  *arena_alloc_array(Arena *a, usize count, usize elem_size);
/* arena_alloc + zero-fill. NULL on OOM. */
void  *arena_calloc(Arena *a, usize size);
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
