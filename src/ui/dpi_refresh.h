/*
 * The DPI-change transaction: which scale is in force, which one the display
 * asked for, and when to try again after a failed font rebuild.
 *
 * A backing-scale change has to do two things together — rasterise the font
 * atlas at the new density and lay the UI out at the new scale — and
 * font_atlas_replace() is deliberately transactional: on failure the current
 * atlas, its raster worker and its GPU texture all survive. Publishing the new
 * scale before the rebuild succeeds would leave the window laid out at one
 * density with glyphs rasterised at another, and permanently: the next event for
 * that display carries the scale already stored, and a
 * `new_dpi == app->dpi_scale` early-out would turn it away.
 *
 * So the two scales are separate here, and the transaction has an explicit
 * outcome. `wanted` is what the display asked for and is recorded before
 * anything can fail; the effective scale (AppState.dpi_scale) is the caller's
 * and moves only after a rebuild has actually succeeded.
 *
 * Split out of main.c because it is a handful of comparisons whose failure is
 * invisible on screen — text at the wrong density reads as a font problem, not a
 * layout one — and it cannot be exercised in place without a second display and
 * an injected font failure.
 */
#ifndef LIU_DPI_REFRESH_H
#define LIU_DPI_REFRESH_H

#include "core/types.h"

/* Retry interval for a rebuild that failed. Long enough that a font file which
 * cannot be loaded at all is retried twice a second rather than once per frame,
 * short enough that a transient allocation failure clears before the user has
 * finished dragging the window onto the other display. */
#define DPI_REBUILD_RETRY_SEC 0.5

typedef struct {
    f32  wanted;     /* the scale the display is asking for; 0 = never asked */
    bool pending;    /* a rebuild is owed at `wanted` */
    f64  retry_at;   /* earliest time to try again */
} DpiRefresh;

static inline void dpi_refresh_init(DpiRefresh *d, f32 initial) {
    d->wanted   = initial;
    d->pending  = false;
    d->retry_at = 0.0;
}

/* A display reported `new_dpi`. Returns true when the caller must attempt a font
 * rebuild at that scale.
 *
 * `effective` is the scale currently in force. The answer is yes whenever the
 * request differs from it — and ALSO when it does not but a rebuild is still
 * owed, the case a plain equality check would swallow: after a failure the
 * effective scale is the old one, so an identical repeat event is the retry. */
static inline bool dpi_refresh_want(DpiRefresh *d, f32 effective, f32 new_dpi) {
    if (!d || !(new_dpi > 0.0f)) return false;
    d->wanted = new_dpi;
    if (new_dpi == effective && !d->pending) return false;
    /* The display has come back to the scale already in force, so whatever
     * was owed no longer is. */
    if (new_dpi == effective) { d->pending = false; d->retry_at = 0.0; return false; }
    return true;
}

/* The rebuild failed. The caller has put the effective scale back; arm a retry. */
static inline void dpi_refresh_failed(DpiRefresh *d, f64 now) {
    if (!d) return;
    d->pending  = true;
    d->retry_at = now + DPI_REBUILD_RETRY_SEC;
}

/* The rebuild succeeded and the caller has published the new scale. */
static inline void dpi_refresh_ok(DpiRefresh *d) {
    if (!d) return;
    d->pending  = false;
    d->retry_at = 0.0;
}

/* Is a retry due? Fills `*out` with the scale to rebuild at.
 *
 * Answers false — and clears the request — when the display has meanwhile come
 * back to the scale in force: there is nothing left to rebuild, and leaving the
 * flag set would keep the event wait short forever. */
static inline bool dpi_refresh_due(DpiRefresh *d, f32 effective, f64 now,
                                   f32 *out) {
    if (!d || !d->pending) return false;
    if (now < d->retry_at) return false;
    if (!(d->wanted > 0.0f) || d->wanted == effective) {
        d->pending  = false;
        d->retry_at = 0.0;
        return false;
    }
    if (out) *out = d->wanted;
    return true;
}

#endif /* LIU_DPI_REFRESH_H */
