/*
 * stb_image implementation — compiled once as a separate TU.
 * Provides PNG/JPEG/BMP decoding for Liu graphics, inline images,
 * and background wallpaper loading (which needs JPEG/BMP + stdio).
 *
 * It also hosts the two image helpers that need stb's *internals*:
 * the header-only preflight (imgproc.h) and the bounded animated-GIF
 * decoder, which drives stb's own per-frame compositor rather than
 * stbi_load_gif_from_memory()'s expand-everything-then-measure.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#include "stb_image.h"

#include "core/imgproc.h"
#include "core/checked.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Header-only preflight — see imgproc.h.
 * ------------------------------------------------------------------------- */

static ImgPreflight img_preflight_finish(int ok, int iw, int ih,
                                         const ImgLimits *lim,
                                         i32 *out_w, i32 *out_h) {
    if (!ok || iw <= 0 || ih <= 0) return IMG_PREFLIGHT_UNREADABLE;
    if (!img_limits_ok((i32)iw, (i32)ih, lim)) return IMG_PREFLIGHT_TOO_LARGE;
    if (out_w) *out_w = (i32)iw;
    if (out_h) *out_h = (i32)ih;
    return IMG_PREFLIGHT_OK;
}

ImgPreflight img_preflight_path(const char *path, const ImgLimits *lim,
                                i32 *out_w, i32 *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!path || !*path) return IMG_PREFLIGHT_UNREADABLE;
    int iw = 0, ih = 0, ich = 0;
    return img_preflight_finish(stbi_info(path, &iw, &ih, &ich), iw, ih,
                                lim, out_w, out_h);
}

ImgPreflight img_preflight_memory(const u8 *data, usize len, const ImgLimits *lim,
                                  i32 *out_w, i32 *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!data || len == 0 || len > (usize)INT32_MAX) return IMG_PREFLIGHT_UNREADABLE;
    int iw = 0, ih = 0, ich = 0;
    return img_preflight_finish(
        stbi_info_from_memory(data, (int)len, &iw, &ih, &ich), iw, ih,
        lim, out_w, out_h);
}

/* The bounded animated-GIF decoder uses two passes over the input. The first
 * composites frames without retaining them and counts the frames. The second
 * allocates the retained buffer from that count and writes each downscaled
 * frame directly into it.
 *
 * The frame count determines whether the decoder shrinks frames further or
 * drops frames. Retaining full-resolution frames in a growable buffer would
 * make output allocation depend on the full animation size, so the two-pass
 * approach keeps retained memory bounded. */

/* Per-frame callback. Returning 0 stops the walk. */
/* Ceiling used when the caller names none. See pass one below. */
#define IMG_GIF_HARD_MAX_FRAMES 65536

typedef int (*ImgGifFrameFn)(void *ctx, const stbi_uc *rgba, i32 w, i32 h,
                             i32 index, i32 delay_ms);

typedef struct {
    stbi_uc *roll[2];       /* frame i-2 lives in roll[i & 1] */
    usize    roll_bytes;
    bool     have_roll;
} ImgGifRollback;

static void img_gif_rollback_free(ImgGifRollback *rb) {
    free(rb->roll[0]);
    free(rb->roll[1]);
    rb->roll[0] = rb->roll[1] = NULL;
    rb->have_roll = false;
}

/* Walk every frame of the GIF, handing each composited canvas to `fn`.
 * `rb`, when non-NULL and allocated, supplies the "two frames back" canvas
 * that disposal method 3 restores from. */
static ImgGifResult img_gif_walk(const u8 *data, usize len,
                                 const ImgGifPolicy *pol,
                                 ImgGifRollback *rb,
                                 ImgGifFrameFn fn, void *ctx,
                                 i32 *out_w, i32 *out_h, i32 *out_frames) {
    if (out_frames) *out_frames = 0;
    if (!data || len == 0 || len > (usize)INT32_MAX) return IMG_GIF_NOT_GIF;

    stbi__context s;
    stbi__start_mem(&s, (const stbi_uc *)data, (int)len);
    if (!stbi__gif_test(&s)) return IMG_GIF_NOT_GIF;

    /* The canvas is the Logical Screen Descriptor's: bytes 6..9, little-endian
     * width then height. stbi__gif_load_next() allocates AND zero-fills 4 * w
     * * h of it on its first call, so the policy has to be applied HERE, from
     * the header, not after that call returns: a 35-byte GIF can declare 55298
     * x 8962, a 1.98 GB malloc + memset (in the viewer's worker, on a file an
     * agent or a page chose) before the 128 MB frame budget gets to say no.
     * stb only refuses what overflows an int. */
    if (len >= 10) {
        i32 hw = (i32)data[6] | ((i32)data[7] << 8);
        i32 hh = (i32)data[8] | ((i32)data[9] << 8);
        if (hw > 0 && hh > 0) {
            usize hb = 0;
            if (!img_rgba_bytes_checked(hw, hh, &hb) ||
                (pol->max_dim > 0 && (hw > pol->max_dim || hh > pol->max_dim)) ||
                (pol->max_frame_bytes && hb > pol->max_frame_bytes))
                return IMG_GIF_TOO_LARGE;
        }
    }

    stbi__gif g;
    memset(&g, 0, sizeof g);

    ImgGifResult res = IMG_GIF_OK;
    i32 frames = 0;
    int comp = 0;

    for (;;) {
        stbi_uc *two_back = NULL;
        if (rb && rb->have_roll && frames >= 2) two_back = rb->roll[frames & 1];

        stbi_uc *u = stbi__gif_load_next(&s, &g, &comp, 4, two_back);
        if (u == (stbi_uc *)&s) break;        /* end-of-animation marker */
        if (!u) { if (frames == 0) res = IMG_GIF_DECODE_ERROR; break; }

        /* The canvas size is only known once the first frame is composited. */
        if (frames == 0) {
            if (g.w <= 0 || g.h <= 0) { res = IMG_GIF_DECODE_ERROR; break; }
            usize frame_bytes = 0;
            if (!img_rgba_bytes_checked((i32)g.w, (i32)g.h, &frame_bytes) ||
                (pol->max_dim > 0 && (g.w > pol->max_dim || g.h > pol->max_dim)) ||
                (pol->max_frame_bytes && frame_bytes > pol->max_frame_bytes)) {
                res = IMG_GIF_TOO_LARGE;
                break;
            }
            if (out_w) *out_w = (i32)g.w;
            if (out_h) *out_h = (i32)g.h;

            /* Allocate the rollback canvases now that the size is known, and
             * only when the policy will pay for them. Without them stb's own
             * documented fallback applies (disposal 3 behaves as 2), which is
             * all stb's own loader can do safely — its two_back pointer is
             * out of bounds. */
            if (rb && !rb->have_roll && pol->max_total_bytes &&
                frame_bytes <= pol->max_total_bytes / 2) {
                rb->roll[0] = (stbi_uc *)calloc(1, frame_bytes);
                rb->roll[1] = (stbi_uc *)calloc(1, frame_bytes);
                rb->roll_bytes = frame_bytes;
                rb->have_roll = rb->roll[0] && rb->roll[1];
                if (!rb->have_roll) img_gif_rollback_free(rb);
            }
        }

        if (!fn(ctx, u, (i32)g.w, (i32)g.h, frames, g.delay)) { frames++; break; }

        if (rb && rb->have_roll)
            memcpy(rb->roll[frames & 1], u, rb->roll_bytes);

        frames++;
        if (pol->max_frames > 0 && frames >= pol->max_frames) break;
        if (frames == INT32_MAX) break;
    }

    STBI_FREE(g.out);
    STBI_FREE(g.history);
    STBI_FREE(g.background);
    if (out_frames) *out_frames = frames;
    return res;
}

/* Pass one: count only. */
static int img_gif_count_cb(void *ctx, const stbi_uc *rgba, i32 w, i32 h,
                            i32 index, i32 delay_ms) {
    (void)ctx; (void)rgba; (void)w; (void)h; (void)index; (void)delay_ms;
    return 1;
}

/* Premultiply each frame before downscaling so the box filter averages
 * premultiplied texels and avoids color fringes around transparency. */
typedef struct {
    u8    *px;
    int   *delays;
    i32    dst_w, dst_h;
    usize  frame_bytes;
    i32    capacity;
    i32    written;
    u8    *scratch;         /* premultiply workspace, one canvas */
    usize  scratch_bytes;
    bool   oom;
} ImgGifEmit;

static int img_gif_emit_cb(void *ctx, const stbi_uc *rgba, i32 w, i32 h,
                           i32 index, i32 delay_ms) {
    ImgGifEmit *e = (ImgGifEmit *)ctx;
    if (e->written >= e->capacity) return 0;
    (void)index;

    /* img_premultiply_rgba works in place and stb's canvas is reused for the
     * next frame's compositing, so it must not be modified — copy first. */
    usize src_bytes = 0;
    if (!img_rgba_bytes_checked(w, h, &src_bytes) ||
        src_bytes > e->scratch_bytes) { e->oom = true; return 0; }
    memcpy(e->scratch, rgba, src_bytes);
    img_premultiply_rgba(e->scratch, w, h);

    u8 *dst = e->px + (usize)e->written * e->frame_bytes;
    if (w == e->dst_w && h == e->dst_h)
        memcpy(dst, e->scratch, e->frame_bytes);
    else
        img_box_downscale_rgba(e->scratch, w, h, dst, e->dst_w, e->dst_h);

    if (e->delays) e->delays[e->written] = delay_ms;
    e->written++;
    return 1;
}

ImgGifResult img_gif_load_bounded(const u8 *data, usize len,
                                  const ImgGifPolicy *pol, ImgGif *out) {
    if (!out) return IMG_GIF_DECODE_ERROR;
    memset(out, 0, sizeof *out);
    if (!data || !pol) return IMG_GIF_DECODE_ERROR;

    /* ---- pass one: how many frames, and how big is the canvas ----
     *
     * The walk is capped because pass one's cost is frames x canvas pixels and
     * a GIF frame can be a handful of bytes: without a ceiling a 32 MB file can
     * declare over a million frames. stb's own loader has no ceiling at all
     * (and reallocates the all-frames buffer once per frame), so a file this
     * cap truncates would exhaust memory there instead. */
    ImgGifPolicy pass1 = *pol;
    if (pass1.max_frames <= 0) pass1.max_frames = IMG_GIF_HARD_MAX_FRAMES;

    i32 src_w = 0, src_h = 0, src_frames = 0;
    ImgGifResult r = img_gif_walk(data, len, &pass1, NULL,
                                  img_gif_count_cb, NULL,
                                  &src_w, &src_h, &src_frames);
    if (r != IMG_GIF_OK) return r;
    if (src_frames <= 0 || src_w <= 0 || src_h <= 0) return IMG_GIF_DECODE_ERROR;

    out->source_w = src_w;
    out->source_h = src_h;
    out->source_frames = src_frames;

    /* True when pass one stopped at the ceiling rather than at the file's end,
     * so "dropped" stays accurate even though the real count is unknown. */
    bool capped_scan = src_frames >= pass1.max_frames;

    /* ---- retention policy: shrink before dropping frames ---- */
    i32 keep = src_frames;
    if (pol->max_frames > 0 && keep > pol->max_frames) keep = pol->max_frames;

    i32 shrink_dim = pol->shrink_dim > 0 ? pol->shrink_dim
                                         : (src_w > src_h ? src_w : src_h);
    i32 dst_w = src_w, dst_h = src_h;
    for (;;) {
        dst_w = src_w; dst_h = src_h;
        img_shrink_dims(&dst_w, &dst_h, shrink_dim);
        usize fb = 0, total = 0;
        if (!img_rgba_bytes_checked(dst_w, dst_h, &fb)) return IMG_GIF_TOO_LARGE;
        bool fits = checked_mul_usize(fb, (usize)keep, &total) &&
                    (!pol->max_total_bytes || total <= pol->max_total_bytes);
        if (fits) break;

        if (pol->shrink_min_dim > 0 && shrink_dim > pol->shrink_min_dim) {
            i32 next = shrink_dim * 3 / 4;
            if (next < pol->shrink_min_dim) next = pol->shrink_min_dim;
            if (next >= shrink_dim) next = shrink_dim - 1;   /* always decreases */
            shrink_dim = next;
            continue;
        }
        /* The smallest size that will be drawn still does not fit: keep as
         * many whole frames as the budget allows. */
        usize max_keep = (fb && pol->max_total_bytes) ? pol->max_total_bytes / fb : 1;
        if (max_keep < 1) max_keep = 1;
        keep = (i32)(max_keep > (usize)keep ? (usize)keep : max_keep);
        break;
    }
    if (keep < 1) keep = 1;

    usize frame_bytes = 0, total_bytes = 0, canvas_bytes = 0;
    if (!img_rgba_bytes_checked(dst_w, dst_h, &frame_bytes) ||
        !checked_mul_usize(frame_bytes, (usize)keep, &total_bytes) ||
        !img_rgba_bytes_checked(src_w, src_h, &canvas_bytes))
        return IMG_GIF_TOO_LARGE;

    /* ---- pass two: decode exactly `keep` frames into an exact buffer ---- */
    ImgGifEmit e;
    memset(&e, 0, sizeof e);
    e.px            = (u8 *)malloc(total_bytes);
    e.scratch       = (u8 *)malloc(canvas_bytes);
    e.scratch_bytes = canvas_bytes;
    e.dst_w = dst_w; e.dst_h = dst_h;
    e.frame_bytes = frame_bytes;
    e.capacity = keep;
    if (keep > 1) e.delays = (int *)calloc((usize)keep, sizeof(int));
    if (!e.px || !e.scratch || (keep > 1 && !e.delays)) {
        free(e.px); free(e.scratch); free(e.delays);
        return IMG_GIF_OOM;
    }

    ImgGifPolicy pass2 = *pol;
    pass2.max_frames = keep;

    ImgGifRollback rb;
    memset(&rb, 0, sizeof rb);
    i32 got_w = 0, got_h = 0, got_frames = 0;
    r = img_gif_walk(data, len, &pass2, keep >= 3 ? &rb : NULL,
                     img_gif_emit_cb, &e, &got_w, &got_h, &got_frames);
    img_gif_rollback_free(&rb);

    /* Fail closed: a partial or oversized buffer is never handed back. */
    if (r != IMG_GIF_OK || e.oom || e.written < 1 ||
        got_w != src_w || got_h != src_h) {
        free(e.px); free(e.scratch); free(e.delays);
        return e.oom ? IMG_GIF_OOM
                     : (r == IMG_GIF_OK ? IMG_GIF_DECODE_ERROR : r);
    }
    free(e.scratch);

    /* A second decode that yielded fewer frames than the first (a truncated
     * file read concurrently, say) must not leave the tail uninitialised. */
    if (e.written < keep) {
        u8 *tight = (u8 *)realloc(e.px, (usize)e.written * frame_bytes);
        if (tight) e.px = tight;
        if (e.delays) {
            int *td = (int *)realloc(e.delays, (usize)e.written * sizeof(int));
            if (td) e.delays = td;
        }
    }

    out->px      = e.px;
    out->delays  = e.written > 1 ? e.delays : (free(e.delays), NULL);
    out->w       = dst_w;
    out->h       = dst_h;
    out->frames  = e.written;
    out->frames_dropped = e.written < src_frames || capped_scan;
    return IMG_GIF_OK;
}
