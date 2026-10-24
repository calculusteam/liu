/*
 * Liu - arena memory allocator implementation
 */
#include "memory.h"
#include "checked.h"
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
    if (size == 0) return a;
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
    if (!a) return;
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

void *arena_alloc_array(Arena *a, usize count, usize elem_size) {
    usize nbytes;
    if (!checked_mul_usize(count, elem_size, &nbytes)) return NULL;
    return arena_alloc(a, nbytes);
}

void *arena_calloc(Arena *a, usize size) {
    void *p = arena_alloc(a, size);
    if (p && size) memset(p, 0, size);
    return p;
}

void *arena_alloc_aligned(Arena *a, usize size, usize align) {
    if (UNLIKELY(!a || !a->base || a->used > a->size)) return NULL;
    if (align == 0) align = 1;
    if (UNLIKELY((align & (align - 1)) != 0)) return NULL;

    const usize base = (usize)a->base;
    usize current;
    usize align_end;
    if (UNLIKELY(!checked_add_usize(base, a->used, &current) ||
                 !checked_add_usize(current, align - 1, &align_end))) {
        a->refused++;
        return NULL;
    }
    const usize aligned = align_end & ~(align - 1);
    if (UNLIKELY(aligned < base)) { a->refused++; return NULL; }
    const usize offset = aligned - base;

    /* Guard offset + size overflow as well as arena capacity. */
    usize end;
    if (UNLIKELY(!checked_add_usize(offset, size, &end) || end > a->size)) {
        a->refused++;
        return NULL; /* overflow or out of memory */
    }

    a->used = end;
    if (a->used > a->peak) {
        a->peak = a->used;
    }
    return (void *)aligned;
}

void arena_reset(Arena *a) {
    if (a) { a->used = 0; a->refused = 0; }
}

usize arena_remaining(const Arena *a) {
    if (!a || a->used >= a->size) return 0;
    return a->size - a->used;
}

/* =========================================================================
 * Temp arena
 * ========================================================================= */

ArenaTemp arena_temp_begin(Arena *a) {
    return (ArenaTemp){ .arena = a, .saved_used = a ? a->used : 0 };
}

void arena_temp_end(ArenaTemp t) {
    if (t.arena && t.saved_used <= t.arena->size) {
        t.arena->used = t.saved_used;
    }
}

/* Always-available secure wipe. Writes through a volatile pointer so the
 * compiler cannot dead-store eliminate the zero, even when the caller
 * immediately frees or returns. Not performance-critical. */
void secure_zero(void *dst, usize n) {
    if (!dst || n == 0) return;
    volatile u8 *p = (volatile u8 *)dst;
    while (n--) *p++ = 0;
}
