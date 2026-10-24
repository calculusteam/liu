/*
 * Liu — shared RGBA pixel helpers for every image producer.
 *
 * The renderer's image path has one hard contract: `renderer_draw_image_cached`
 * uploads the bytes it is given into an *sRGB* texture (MTLPixelFormatRGBA8Unorm_sRGB
 * / GL_SRGB8_ALPHA8) and draws them through the glyph pipeline, whose blend is
 * (ONE, OneMinusSourceAlpha). Source factor ONE means the shader's rgb output is
 * composited *unscaled*, so the texels must already be PREMULTIPLIED — anything
 * feeding straight alpha in gets `dst*(1-a) + rgb` where `dst*(1-a) + rgb*a` was
 * meant, i.e. a bright halo wherever partial alpha meets light colours.
 *
 * Every decoder in the tree (stb_image and the inline-graphics protocol paths,
 * the icon loaders) hands back STRAIGHT alpha, so each of them must run its
 * pixels through img_premultiply_rgba() before the renderer sees them.
 */
#ifndef LIU_IMGPROC_H
#define LIU_IMGPROC_H

#include "core/types.h"
#include <stdbool.h>

/* Premultiply straight-alpha RGBA in place.
 *
 * The multiply happens in LINEAR light, not in the stored sRGB byte domain.
 * That distinction is not cosmetic: the sampler decodes sRGB->linear on read and
 * the blend runs linear, so the texel has to satisfy
 *     srgb_decode(stored) == linear(colour) * alpha
 * which means storing srgb_encode(srgb_decode(c) * a). Multiplying the raw bytes
 * instead (`c * a / 255`) stores srgb_decode(c*a), and for a=0.5 on white that
 * lands at 0.214 instead of 0.5 — edges come out roughly 2.3x too dark.
 *
 * Backed by a lazily-built 64 KiB [alpha][channel] table, so this is three
 * lookups per non-opaque pixel and exact rather than approximated. Fully opaque
 * texels (a == 255) are left bit-identical, so opaque images are untouched. */
void img_premultiply_rgba(u8 *px, i32 w, i32 h);

/* Callers must premultiply first: averaging straight-alpha pixels lets RGB from
 * transparent texels bleed into visible neighbors.
 *
 * The area average uses stored sRGB byte values, matching the image viewer's
 * existing downscale path. This approximates a linear-light filter. */
void img_box_downscale_rgba(const u8 *src, i32 sw, i32 sh,
                            u8 *dst, i32 dw, i32 dh);

/* Compute the dimensions img_shrink_to_max_dim() would produce, without
 * touching any pixels. Layout passes that need an image's final on-screen size
 * before (or instead of) decoding it use this, and because the real shrink is
 * defined in terms of this function the two can never drift apart — a layout
 * that measured one size and then drew another would visibly jump. */
void img_shrink_dims(i32 *w, i32 *h, i32 max_dim);

/* Shrink the image at *px so its longest edge is <= max_dim, updating the
 * dimensions through *w and *h.
 *
 * On success the old buffer is released with free() and replaced by a malloc'd
 * one (stb's STBI_FREE defaults to free(), so a stbi_load result can be passed
 * in and the result stays free()-compatible either way). Returns false only if
 * the replacement allocation fails, in which case the buffer and both
 * dimensions are left untouched and the caller decides whether to keep the
 * full-resolution image or bail.
 *
 * A no-op returning true when the image already fits. Callers should premultiply
 * before calling — see img_box_downscale_rgba. */
bool img_shrink_to_max_dim(u8 **px, i32 *w, i32 *h, i32 max_dim);


/* ---------------------------------------------------------------------------
 * Decoded-size policy, shared by every image producer
 *
 * The rule these exist to enforce: never hand an attacker-controlled file to a
 * decoder before knowing how big the decode will be. stbi_load() on a PNG of
 * under a megabyte that declares 16000x16000 allocates ~1 GB and stalls the
 * main thread on its way to being refused. stbi_info() reads the header only.
 *
 * Every viewer preflights through these helpers instead of spelling out its
 * own check: one helper, four policies. */

/* w*h*4 with overflow checked. false on non-positive dimensions or overflow. */
bool img_rgba_bytes_checked(i32 w, i32 h, usize *out_bytes);

/* Bytes a COMPLETE RGBA mip chain retains, base level included.
 *
 * The OpenGL image cache calls glGenerateMipmap, so an entry holds the whole
 * chain and not just the `w * h * 4` of its base level. The ratio approaches
 * 4/3 for a square power-of-two chain but is not 4/3 in general (an odd or
 * very oblong image rounds up at every level), so this sums the levels exactly
 * rather than scaling by a constant.
 *
 * Returns false on non-positive dimensions or overflow. */
bool img_mip_chain_bytes_checked(i32 w, i32 h, usize *out_bytes);

/* The longest edge an image of `w` x `h` must be shrunk to for its decoded RGBA
 * to fit `max_bytes`, preserving aspect ratio. Returns the current longest edge
 * unchanged when it already fits, and never returns less than 1. Pure — pair it
 * with img_shrink_to_max_dim. */
i32 img_max_dim_for_bytes(i32 w, i32 h, usize max_bytes);

/* The formats a PROGRAM has any reason to put on a terminal: PNG (what the APC
 * _G graphics protocol and Liu's own images carry), JPEG, GIF and BMP. By magic
 * bytes only; it says nothing about whether the rest is valid.
 *
 * Why it exists: stb_image decodes eight formats, and the least used ones — PSD,
 * PIC, PNM, TGA — have the longest record of memory-safety bugs. A terminal's
 * image protocols hand stb whatever bytes a remote program printed (`cat`, an
 * SSH host, an agent's tool output), so on that path the other formats are
 * attack surface and nothing else. The file browser, which opens a file the USER
 * chose, keeps all of them. */
static inline bool img_is_terminal_format(const u8 *d, usize n) {
    if (!d) return false;
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' &&
        d[4] == '\r' && d[5] == '\n' && d[6] == 0x1a && d[7] == '\n') return true;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return true;
    if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8' &&
        (d[4] == '7' || d[4] == '9') && d[5] == 'a') return true;
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return true;
    return false;
}

/* A viewer's decoded-size policy. 0 in any field means "no limit for this
 * dimension of the policy" — a caller that only cares about total bytes leaves
 * max_dim at 0. */
typedef struct {
    i32   max_dim;          /* longest edge, in pixels                       */
    usize max_pixels;       /* w*h                                           */
    usize max_rgba_bytes;   /* w*h*4 once decoded                            */
} ImgLimits;

typedef enum {
    IMG_PREFLIGHT_OK = 0,
    IMG_PREFLIGHT_UNREADABLE,   /* not an image, or the header is unusable   */
    IMG_PREFLIGHT_TOO_LARGE     /* header is valid but busts the policy      */
} ImgPreflight;

/* Header-only inspection of `path`, then the policy check. On OK the declared
 * dimensions are written through out_w/out_h. Nothing is decoded. */
ImgPreflight img_preflight_path(const char *path, const ImgLimits *lim,
                                i32 *out_w, i32 *out_h);

/* Same for an in-memory encoded image. */
ImgPreflight img_preflight_memory(const u8 *data, usize len, const ImgLimits *lim,
                                  i32 *out_w, i32 *out_h);

/* Re-apply the policy to the dimensions a decoder actually returned. Formats
 * exist whose header and decoder disagree, so the preflight is a gate, not a
 * promise; every caller must re-check what it got. */
bool img_limits_ok(i32 w, i32 h, const ImgLimits *lim);

/* Bound animated-image decoding before allocating the full frame set. Enforce
 * limits on frame count and total decoded pixels so compact files cannot expand
 * into unbounded memory use. */
typedef struct {
    i32   max_dim;              /* refuse a canvas longer than this on a side */
    usize max_frame_bytes;      /* refuse a canvas costing more than this     */
    i32   max_frames;           /* hard ceiling on retained frames            */
    usize max_total_bytes;      /* retained animation budget                  */
    i32   shrink_dim;           /* longest edge of the RETAINED frames        */
    i32   shrink_min_dim;       /* shrink no further than this before dropping
                                 * frames; 0 means never shrink further       */
} ImgGifPolicy;

typedef struct {
    u8   *px;           /* frames, contiguous, premultiplied, w x h each      */
    int  *delays;       /* one per retained frame, in ms; may be NULL         */
    i32   w, h;         /* retained frame dimensions                          */
    i32   frames;       /* retained frame count                               */
    i32   source_w, source_h;   /* the canvas the file declared               */
    i32   source_frames;        /* frames the file actually held              */
    bool  frames_dropped;       /* source_frames > frames                     */
} ImgGif;

typedef enum {
    IMG_GIF_OK = 0,
    IMG_GIF_NOT_GIF,
    IMG_GIF_DECODE_ERROR,
    IMG_GIF_TOO_LARGE,   /* the canvas alone busts max_dim/max_frame_bytes */
    IMG_GIF_OOM
} ImgGifResult;

/* Decode `data` under `pol`. On IMG_GIF_OK the caller owns out->px (free()) and
 * out->delays (free()). On anything else nothing is allocated and *out is
 * zeroed — the decode fails closed rather than handing back a partial or
 * oversized buffer. */
ImgGifResult img_gif_load_bounded(const u8 *data, usize len,
                                  const ImgGifPolicy *pol, ImgGif *out);

#endif /* LIU_IMGPROC_H */
