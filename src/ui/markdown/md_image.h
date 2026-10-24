/*
 * Liu - Markdown image cache
 *
 * Path-keyed RGBA cache used by md_render for inline images and embeds.
 * Pixel buffers come from stbi_load (malloc-backed) — they cannot live in
 * the parse arena because their size is not bounded by source length.
 *
 * One cache per file-browser viewer. Cache publication/LRU is main-thread-only;
 * an owned serial worker performs bounded decode/downscale off the render path.
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
    bool   failed;              /* header probe or decode failed. Sticky per
                                 * frame, NOT per session: see fail_next. */
    /* A failure is retried on a backoff, and only when the file CHANGED: a
     * corrupt file is not decoded again, while a screenshot an agent is still
     * writing, or a path about to be created, is drawn once the file is there
     * without reopening the document. At `fail_next` the entry is stat'ed
     * (one syscall, no decode); a file whose identity matches what failed
     * doubles the interval up to MD_IMAGE_RETRY_MAX_SEC, a different one is
     * re-probed and, if it decodes now, drawn. */
    f64    fail_next;
    f64    fail_backoff;
    bool   fail_stat_ok;        /* the file existed when it failed */
    i64    fail_size;
    i64    fail_mtime_sec, fail_mtime_nsec;
    bool   evicted;             /* pixels released under budget pressure; the
                                 * entry is still valid and the next get()
                                 * reloads it from abs_path with a fresh
                                 * generation. Kept distinct from `failed`,
                                 * which is sticky: an evicted image must
                                 * come back, not turn into a red
                                 * "[image not found]". */
    bool   loading;             /* async decode queued/in flight */
    u64    load_token;          /* rejects a late result after slot reuse */
    usize  bytes;               /* size of rgba allocation, 0 when evicted/failed */
    u64    epoch;               /* render pass in which this entry was last
                                 * requested for drawing, including while loading
                                 * (stamped by md_image_cache_get,
                                 * NOT by the measure-only dims lookup). Eviction
                                 * refuses to take an entry stamped with the
                                 * current epoch — see md_image_cache_begin_pass. */
    struct MdImageEntry *next;  /* hash-chain */
    struct MdImageEntry *lru_prev, *lru_next; /* LRU list (head = most recent) */
} MdImageEntry;

typedef struct MdImageCache {
    MdImageEntry *buckets[64];
    MdImageEntry *lru_head, *lru_tail;
    usize         total_bytes;
    i32           entry_count;   /* total entries across all buckets,
                                  * including evicted (rgba=NULL) ones */
    u64           epoch;         /* bumped once per render pass */
    /* Wall clock of the last render pass. The grace below is expressed in
     * EPOCHS, and epochs only advance while something draws — so a cache whose
     * owner is hidden freezes its epoch, and its last-drawn entries would stay
     * "recently drawn" for as long as the tab stays hidden. The
     * memory-pressure path judges by this clock instead. */
    f64           last_pass_time;
    void         *async;         /* private MdImageAsync worker state */
    bool          async_headers; /* UI: header/stat work uses the same worker */
    u64           layout_revision;
} MdImageCache;

MdImageCache *md_image_cache_create(void);
void          md_image_cache_destroy(MdImageCache *c);

/* Start a new render pass. Entries fetched (drawn) after this call are pinned
 * against eviction until the next pass begins.
 *
 * Without this pin, a document whose *visible* images exceed the byte budget
 * degenerates: drawing image B evicts image A, drawing A evicts B, and because
 * an evicted entry reloads, that becomes a full stbi_load + premultiply +
 * downscale of the whole visible set on every single frame. Overshooting the
 * budget for the duration of one pass is strictly cheaper, and the overshoot
 * is bounded by what is simultaneously on screen. Called from md_render(), so
 * no caller can forget it. */
void          md_image_cache_begin_pass(MdImageCache *c);
bool          md_image_cache_pump(MdImageCache *c);

/* Drop the pixels of every entry that is not on screen (and was not on screen
 * in the previous pass), returning the bytes released. The entries survive as
 * `evicted` and reload from disk on their next get(), so this costs a decode
 * and never a picture — which is what makes it safe to call under memory
 * pressure. Nothing here is dropped by time; the caller decides when. */
usize         md_image_cache_reclaim(MdImageCache *c);
bool          md_image_cache_completion_ready(const MdImageCache *c);

/* Resolve a (possibly relative) markdown image reference against base_dir
 * and load the file via stb_image. Returns the cache entry (always non-NULL
 * once md_image_cache_create succeeded — use entry->failed to detect load
 * errors). The returned pointer is stable for the cache lifetime. */
const MdImageEntry *md_image_cache_get(MdImageCache *c, const char *base_dir,
                                       const char *ref, u32 ref_len);
/* Read metadata already requested by dims(), without starting pixel decode. */
const MdImageEntry *md_image_cache_peek(const MdImageCache *c, const char *base_dir,
                                      const char *ref, u32 ref_len);

/* Metadata-only lookup: report the eventual dimensions without pixel decode. UI
 * caches set async_headers before their first request; false then means pending
 * or failed metadata, and the completion changes layout_revision. Standalone
 * synchronous callers read only the header. Failures are memoised and retried
 * on a backoff rather than re-probed on every frame.
 *
 * This exists for md_render's measure pass, which needs only the on-screen box
 * of an off-screen image. md_image_cache_get there would fully decode every
 * image of a document on its first frame and, because the walk runs in document
 * order, leave the LRU tail pointing at the image at the top of the file, i.e.
 * the one actually on screen. Deliberately does NOT touch the LRU order:
 * measuring is not using. */
bool md_image_cache_dims(MdImageCache *c, const char *base_dir,
                         const char *ref, u32 ref_len, i32 *w, i32 *h);

#endif /* UI_MARKDOWN_IMAGE_H */
