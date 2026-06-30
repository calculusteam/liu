/*
 * Liu - arena memory allocator implementation
 */
#include "memory.h"
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
    #include <sys/mman.h>
    #define OS_ALLOC(size) mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0)
    #define OS_FREE(ptr, size) munmap(ptr, size)
#else
    #include <sys/mman.h>
    #define OS_ALLOC(size) mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    #define OS_FREE(ptr, size) munmap(ptr, size)
#endif

/* =========================================================================
 * Arena allocator
 * ========================================================================= */

Arena arena_create(usize size) {
    Arena a = {0};
    a.base = (u8 *)OS_ALLOC(size);
    if (a.base != NULL && a.base != (u8 *)(isize)-1) {
        a.size = size;
    } else {
        /* Mirror pool_create: hand back a clean zeroed handle on failure so
         * callers can test a->base for NULL and arena_destroy doesn't fire a
         * bogus munmap((void*)-1, 0) on MAP_FAILED. */
        a.base = NULL;
    }
    return a;
}

void arena_destroy(Arena *a) {
    if (a->base) {
        OS_FREE(a->base, a->size);
        a->base = NULL;
        a->size = 0;
        a->used = 0;
    }
}

void *arena_alloc(Arena *a, usize size) {
    return arena_alloc_aligned(a, size, 16);
}

void *arena_alloc_aligned(Arena *a, usize size, usize align) {
    usize current = (usize)(a->base + a->used);
    usize aligned = (current + align - 1) & ~(align - 1);
    usize offset  = aligned - (usize)a->base;

    if (UNLIKELY(offset + size > a->size)) {
        return NULL; /* out of memory */
    }

    a->used = offset + size;
    if (a->used > a->peak) {
        a->peak = a->used;
    }
    return (void *)aligned;
}

void arena_reset(Arena *a) {
    a->used = 0;
}

usize arena_remaining(const Arena *a) {
    return a->size - a->used;
}

/* =========================================================================
 * Temp arena
 * ========================================================================= */

ArenaTemp arena_temp_begin(Arena *a) {
    return (ArenaTemp){ .arena = a, .saved_used = a->used };
}

void arena_temp_end(ArenaTemp t) {
    t.arena->used = t.saved_used;
}

/* Always-available secure wipe. Writes through a volatile pointer so the
 * compiler cannot dead-store eliminate the zero, even when the caller
 * immediately frees or returns. Not performance-critical. */
void secure_zero(void *dst, usize n) {
    if (!dst || n == 0) return;
    volatile u8 *p = (volatile u8 *)dst;
    while (n--) *p++ = 0;
}
