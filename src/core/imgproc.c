/*
 * Liu — shared RGBA pixel helpers. See imgproc.h for the renderer contract
 * these exist to satisfy.
 */
#include "core/imgproc.h"

#include <pthread.h>
#include "core/checked.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Premultiply
 * ------------------------------------------------------------------------- */

/* [alpha][channel] -> premultiplied sRGB byte. 64 KiB, built once on first use.
 *
 * A full table instead of per-pixel powf() for two reasons: the transform is
 * exact for every one of the 65 536 input pairs (no LUT interpolation error),
 * and the hot loop degenerates to three dependent loads with no FP work. Built
 * lazily so a run that never touches an image never pays for it. */
static u8   s_premul_tab[256 * 256];
static pthread_once_t s_premul_once = PTHREAD_ONCE_INIT;

/* f64 throughout: the table is built exactly once for the whole process, so the
 * precision is free, and at f32 a handful of the 65 536 entries land on the
 * wrong side of the rounding boundary (a=188, c=183 was one) — enough to make
 * the output depend on the compiler's float contraction rather than on the
 * spec. */
static f64 srgb_to_linear_f(f64 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static f64 linear_to_srgb_f(f64 c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

static void premul_tab_init(void) {
    for (i32 a = 0; a < 256; a++) {
        f64 af = (f64)a / 255.0;
        for (i32 c = 0; c < 256; c++) {
            f64 lin = srgb_to_linear_f((f64)c / 255.0) * af;
            f64 enc = linear_to_srgb_f(lin) * 255.0 + 0.5;
            if (enc < 0.0) enc = 0.0;
            if (enc > 255.0) enc = 255.0;
            s_premul_tab[a * 256 + c] = (u8)enc;
        }
    }
}

void img_premultiply_rgba(u8 *px, i32 w, i32 h) {
    if (!px || w <= 0 || h <= 0) return;
    /* The 64 KiB table is built on first use, and first use can be on any
     * thread: this is reached from the markdown decode worker, the
     * file-browser decode worker, the lightbox worker, the wallpaper worker,
     * the prompt-thumbnail worker and the terminal's own inline-image worker.
     *
     * Every thread computes identical values, but without a release/acquire
     * pairing between the table writes and the flag written after them, a
     * second thread on ARM64 could observe the flag and then read rows still
     * holding zero — and zero here means fully transparent, i.e. black pixels
     * in a decoded image rather than a crash.
     *
     * pthread_once gives that ordering and keeps the properties that matter:
     * a run that never decodes an image never builds the table, and the
     * steady-state cost is one atomic load per CALL, not per pixel. */
    pthread_once(&s_premul_once, premul_tab_init);

    i64 n = (i64)w * (i64)h;
    for (i64 i = 0; i < n; i++) {
        u8 *p = px + i * 4;
        u8 a = p[3];
        if (a == 255) continue;   /* opaque: leave bit-exact */
        const u8 *row = &s_premul_tab[(i32)a * 256];
        p[0] = row[p[0]];
        p[1] = row[p[1]];
        p[2] = row[p[2]];
    }
}

/* ---------------------------------------------------------------------------
 * Box downscale
 * ------------------------------------------------------------------------- */

void img_box_downscale_rgba(const u8 *src, i32 sw, i32 sh,
                            u8 *dst, i32 dw, i32 dh) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (i32 dy = 0; dy < dh; dy++) {
        i32 sy0 = (i32)((i64)dy * sh / dh);
        i32 sy1 = (i32)((i64)(dy + 1) * sh / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > sh) sy1 = sh;
        for (i32 dx = 0; dx < dw; dx++) {
            i32 sx0 = (i32)((i64)dx * sw / dw);
            i32 sx1 = (i32)((i64)(dx + 1) * sw / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > sw) sx1 = sw;
            u64 r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (i32 yy = sy0; yy < sy1; yy++) {
                const u8 *row = src + ((i64)yy * sw + sx0) * 4;
                for (i32 xx = sx0; xx < sx1; xx++) {
                    r += row[0]; g += row[1]; b += row[2]; a += row[3];
                    row += 4; cnt++;
                }
            }
            u8 *d = dst + ((i64)dy * dw + dx) * 4;
            if (cnt == 0) { memset(d, 0, 4); continue; }
            d[0] = (u8)(r / cnt); d[1] = (u8)(g / cnt);
            d[2] = (u8)(b / cnt); d[3] = (u8)(a / cnt);
        }
    }
}

void img_shrink_dims(i32 *w, i32 *h, i32 max_dim) {
    if (!w || !h || max_dim <= 0) return;
    i32 iw = *w, ih = *h;
    if (iw <= 0 || ih <= 0) return;
    i32 longest = iw > ih ? iw : ih;
    if (longest <= max_dim) return;
    f32 s  = (f32)max_dim / (f32)longest;
    i32 nw = (i32)((f32)iw * s + 0.5f); if (nw < 1) nw = 1;
    i32 nh = (i32)((f32)ih * s + 0.5f); if (nh < 1) nh = 1;
    *w = nw;
    *h = nh;
}

bool img_shrink_to_max_dim(u8 **px, i32 *w, i32 *h, i32 max_dim) {
    if (!px || !*px || !w || !h || max_dim <= 0) return false;
    i32 iw = *w, ih = *h;
    if (iw <= 0 || ih <= 0) return false;

    i32 longest = iw > ih ? iw : ih;
    if (longest <= max_dim) return true;   /* already small enough */

    i32 nw = iw, nh = ih;
    img_shrink_dims(&nw, &nh, max_dim);

    usize pixels = 0, bytes = 0;
    if (!checked_mul_usize((usize)nw, (usize)nh, &pixels) ||
        !checked_mul_usize(pixels, 4u, &bytes) || bytes == 0) return false;

    u8 *down = (u8 *)malloc(bytes);
    if (!down) return false;

    img_box_downscale_rgba(*px, iw, ih, down, nw, nh);
    free(*px);
    *px = down;
    *w  = nw;
    *h  = nh;
    return true;
}

/* ---------------------------------------------------------------------------
 * Decoded-size policy — see imgproc.h.
 * ------------------------------------------------------------------------- */

bool img_rgba_bytes_checked(i32 w, i32 h, usize *out_bytes) {
    if (out_bytes) *out_bytes = 0;
    if (w <= 0 || h <= 0) return false;
    usize px = 0, bytes = 0;
    if (!checked_mul_usize((usize)w, (usize)h, &px)) return false;
    if (!checked_mul_usize(px, 4u, &bytes)) return false;
    if (out_bytes) *out_bytes = bytes;
    return true;
}

bool img_mip_chain_bytes_checked(i32 w, i32 h, usize *out_bytes) {
    if (out_bytes) *out_bytes = 0;
    if (w <= 0 || h <= 0) return false;
    usize total = 0;
    for (i32 lw = w, lh = h;;) {
        usize level = 0;
        if (!img_rgba_bytes_checked(lw, lh, &level)) return false;
        if (!checked_add_usize(total, level, &total)) return false;
        if (lw == 1 && lh == 1) break;
        if (lw > 1) lw /= 2;
        if (lh > 1) lh /= 2;
    }
    if (out_bytes) *out_bytes = total;
    return true;
}

/* Would shrinking to `target` fit `max_bytes`? Asks img_shrink_dims rather than
 * modelling it, so the answer is about the dimensions actually produced. */
static bool img_dim_fits(i32 w, i32 h, i32 target, usize max_bytes) {
    i32 nw = w, nh = h;
    img_shrink_dims(&nw, &nh, target);
    usize bytes = 0;
    return img_rgba_bytes_checked(nw, nh, &bytes) && bytes <= max_bytes;
}

i32 img_max_dim_for_bytes(i32 w, i32 h, usize max_bytes) {
    i32 longest = w > h ? w : h;
    if (longest < 1) return 1;
    if (max_bytes == 0) return longest;
    usize bytes = 0;
    if (img_rgba_bytes_checked(w, h, &bytes) && bytes <= max_bytes) return longest;

    /* Binary search for the largest edge that fits. The closed-form estimate
     * (edge scale = sqrt(budget/bytes)) is within a pixel, but img_shrink_dims
     * rounds each axis independently and a single pixel of rounding on the long
     * edge can put the result BACK over the budget. Search, so the answer is a
     * guarantee rather than an estimate. */
    i32 lo = 1, hi = longest, best = 1;
    while (lo <= hi) {
        i32 mid = lo + (hi - lo) / 2;
        if (img_dim_fits(w, h, mid, max_bytes)) { best = mid; lo = mid + 1; }
        else                                     { hi = mid - 1; }
    }
    return best;
}

bool img_limits_ok(i32 w, i32 h, const ImgLimits *lim) {
    if (w <= 0 || h <= 0) return false;
    if (!lim) return true;
    if (lim->max_dim > 0 && (w > lim->max_dim || h > lim->max_dim)) return false;

    usize px = 0;
    if (!checked_mul_usize((usize)w, (usize)h, &px)) return false;
    if (lim->max_pixels && px > lim->max_pixels) return false;

    usize bytes = 0;
    if (!img_rgba_bytes_checked(w, h, &bytes)) return false;
    if (lim->max_rgba_bytes && bytes > lim->max_rgba_bytes) return false;
    return true;
}

/* Decoder-dependent image preflight and GIF loading live in
 * core/stb_image_impl.c. Keeping them there lets imgproc.c remain independent
 * of stb_image. */
