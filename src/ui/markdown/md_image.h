/*
 * Liu - Markdown image cache
 *
 * Path-keyed RGBA cache used by md_render for inline images and embeds.
 * Pixel buffers come from stbi_load (malloc-backed) — they cannot live in
 * the parse arena because their size is not bounded by source length.
 *
 * Single-threaded; no locks. One cache per file-browser viewer instance.
 */
#ifndef UI_MARKDOWN_IMAGE_H
#define UI_MARKDOWN_IMAGE_H

#include "core/types.h"

typedef struct MdImageEntry {
    char  *abs_path;            /* malloc'd key */
    u8    *rgba;                /* stbi_load output, NULL on failure */
    u64    generation;          /* globally-unique, monotonic per rgba load. Passed
                                 * as the GPU image-cache generation so a recycled
                                 * MdImageEntry address (the struct is freed and
                                 * malloc-reused across document switches) can never
                                 * alias a prior document's texture. */
    i32    w, h;
    bool   failed;              /* cached failure to skip re-load */
    usize  bytes;               /* size of rgba allocation, 0 when evicted/failed */
    struct MdImageEntry *next;  /* hash-chain */
    struct MdImageEntry *lru_prev, *lru_next; /* LRU list (head = most recent) */
} MdImageEntry;

typedef struct MdImageCache {
    MdImageEntry *buckets[64];
    MdImageEntry *lru_head, *lru_tail;
    usize         total_bytes;
    i32           entry_count;   /* total entries across all buckets,
                                  * including evicted (rgba=NULL) ones */
} MdImageCache;

MdImageCache *md_image_cache_create(void);
void          md_image_cache_destroy(MdImageCache *c);

/* Resolve a (possibly relative) markdown image reference against base_dir
 * and load the file via stb_image. Returns the cache entry (always non-NULL
 * once md_image_cache_create succeeded — use entry->failed to detect load
 * errors). The returned pointer is stable for the cache lifetime. */
const MdImageEntry *md_image_cache_get(MdImageCache *c, const char *base_dir,
                                       const char *ref, u32 ref_len);

#endif /* UI_MARKDOWN_IMAGE_H */
