/*
 * Liu — image texture upload/cache telemetry, shared by both backends.
 *
 * Both renderers keep a 128-entry LRU pool with a 32 MiB byte budget plus a
 * separate small LRU for anything above 16 MiB. Three subsystems can hand the
 * renderer a buffer past that threshold — the file-browser viewer at up to
 * 25 MiB, a terminal inline image at up to 32 MiB (and up to 32 of those per
 * pane), and the lightbox at up to 256 MiB — so entries there are keyed on
 * different pointers and an eviction costs a whole re-upload.
 *
 * How often that actually happens is a question about a running app, not about
 * the source, so this counts it rather than reasoning about it. The counters
 * separate the cases that cost differently:
 *
 *   hit / miss          did the cache answer without touching the GPU
 *   alloc vs update     a new texture object, or a write into an existing one
 *   oversize_upload     went through the above-threshold tier
 *   oversize_replace    an entry there was evicted and will need re-uploading
 *   restage             same key, bumped generation (an animation frame)
 *
 * Upload duration is the CPU time around the texture create and the pixel
 * write ONLY — no encoder setup, no draw. It is kept as a histogram as well as
 * a sum, because the question is a tail: one 256 MiB upload inside a frame is
 * invisible in a mean over a thousand small ones.
 *
 * Main-thread only, like the memory ledger it sits beside: every writer is on
 * the render path. Plain integers for that reason.
 */
#ifndef RENDERER_IMAGE_STATS_H
#define RENDERER_IMAGE_STATS_H

#include "core/types.h"
#include "core/percentile.h"

typedef struct {
    u64 hits;
    u64 misses;

    u64 upload_count;          /* every pixel write that reached the GPU */
    u64 upload_bytes;
    u64 upload_cpu_ns;         /* summed; see upload_ns for the distribution */
    u64 max_upload_bytes;
    u64 max_upload_ns;

    u64 alloc_count;           /* a texture object was created */
    u64 update_count;          /* an existing texture was written into */

    u64 oversize_upload_count;
    /* An above-threshold entry lost its place and will have to be uploaded
     * again to be drawn again: how many re-uploads residency policy FORCED.
     * Zero is the healthy value. */
    u64 oversize_replace_count;
    u64 restage_count;          /* same key, new generation — animation */

    u64 evict_count;            /* pool entries dropped to make room */

    LiuHist upload_ns;          /* per-upload CPU time */
} LiuImageStats;

/* One per process. Both backends write it; only one is compiled in. */
extern LiuImageStats g_liu_image_stats;

/* Cheap enough to leave unconditional: a handful of adds on a path that is
 * already about to touch the GPU with megabytes. The histogram add is one CLZ
 * and an increment.
 *
 * A MISS is counted here rather than at the pool lookup. The oversize slot is
 * a second cache behind the first, so incrementing misses when the 128-entry
 * pool fails to answer charged every oversize HIT with a miss as well —
 * hits+misses stopped meaning "draws that reached the cache". Counting it
 * where the GPU work actually happens makes misses == upload_count by
 * construction, which is redundant on purpose: the two disagreeing would mean
 * an upload path had been added without telemetry. */
static inline void liu_image_stat_upload(bool oversize, bool is_alloc,
                                         usize bytes, u64 ns) {
    LiuImageStats *s = &g_liu_image_stats;
    s->misses++;
    s->upload_count++;
    s->upload_bytes += bytes;
    s->upload_cpu_ns += ns;
    if (bytes > s->max_upload_bytes) s->max_upload_bytes = bytes;
    if (ns > s->max_upload_ns) s->max_upload_ns = ns;
    if (is_alloc) s->alloc_count++; else s->update_count++;
    if (oversize) s->oversize_upload_count++;
    liu_hist_add_ns(&s->upload_ns, ns);
}

static inline void liu_image_stats_reset(void) {
    LiuImageStats *s = &g_liu_image_stats;
    *s = (LiuImageStats){0};
    liu_hist_reset(&s->upload_ns);
}

#endif /* RENDERER_IMAGE_STATS_H */
