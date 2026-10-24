/*
 * Liu — what the CPU side of rendering is holding (MEM_RENDER_CPU).
 *
 * Two inventories, kept apart from the code that allocates them so the
 * arithmetic can be checked without a window server:
 *
 *   term_render_cache_bytes()  one pane's per-row glyph / background /
 *                              decoration caches and their index arrays
 *   renderer_cpu_bytes()       one renderer's instance and rect batches
 *
 * BOTH READ CAPACITIES, NEVER LIVE COUNTS. A row that grew to 200 glyphs and
 * now holds three is still holding the 200 — term_render_cache_shrink_row
 * only gives it back when a row overshoots by 4x — and a renderer batch that
 * peaked at a full-screen frame keeps that peak until renderer_destroy.
 * Counting `glyph_count` instead would report a screen that has gone quiet as
 * costing nothing, which is precisely the state where this memory is worth
 * seeing.
 *
 * The three index arrays per family (rows of pointers, counts and caps) are
 * derived from `cached_rows` rather than probed, because term_render_cache_
 * ensure allocates all of them together or none of them: a partial set is
 * released and the cache is left at cached_rows == 0.
 */
#ifndef UI_RENDER_CPU_BYTES_H
#define UI_RENDER_CPU_BYTES_H

#include "core/types.h"
#include "renderer/renderer.h"

/* These scalar helpers compute optional render-cache storage without
 * requiring an `AppState` or window. */
static inline usize term_render_cache_aux_bytes(i32 rows,bool versions,
        i32 spare_bg_cap,i32 spare_glyph_cap,bool palette) {
    return (palette?256u*sizeof(Color):0)+
        (versions && rows>0?(usize)rows*2u*sizeof(u64):0)+
        (spare_bg_cap>0?(usize)spare_bg_cap*sizeof(RectInstance):0)+
        (spare_glyph_cap>0?(usize)spare_glyph_cap*sizeof(GlyphInstance):0);
}

static inline usize term_render_cache_bytes_from(i32 cached_rows,
                                                 const i32 *bg_caps,
                                                 const i32 *glyph_caps,
                                                 const i32 *deco_caps,
                                                 RectInstance *const *bg_rows,
                                                 GlyphInstance *const *glyph_rows,
                                                 RectInstance *const *deco_rows) {
    if (cached_rows <= 0) return 0;
    usize rows = (usize)cached_rows;
    usize n = 0;
    /* One row-pointer array plus a count array plus a cap array, per family. */
    n += rows * (sizeof(RectInstance *)  + 2 * sizeof(i32));   /* bg    */
    n += rows * (sizeof(GlyphInstance *) + 2 * sizeof(i32));   /* glyph */
    n += rows * (sizeof(RectInstance *)  + 2 * sizeof(i32));   /* deco  */
    /* img_rows (u8), img_badge_cols (i32), pending_rows (u8). */
    n += rows * (sizeof(u8) + sizeof(i32) + sizeof(u8));
    for (i32 r = 0; r < cached_rows; r++) {
        if (bg_rows    && bg_rows[r]    && bg_caps)
            n += (usize)bg_caps[r]    * sizeof(RectInstance);
        if (glyph_rows && glyph_rows[r] && glyph_caps)
            n += (usize)glyph_caps[r] * sizeof(GlyphInstance);
        if (deco_rows  && deco_rows[r]  && deco_caps)
            n += (usize)deco_caps[r]  * sizeof(RectInstance);
    }
    return n;
}

/* `renderer_cpu_bytes()` is defined by each backend because their CPU metadata
 * differs. Both backends use `renderer_batch_cpu_bytes()`, which is inline in
 * `renderer.h`. */

#endif /* UI_RENDER_CPU_BYTES_H */
