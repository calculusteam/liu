/*
 * Liu — decode and cache compiled-in filebrowser PNG icons.
 * Build input: ${CMAKE_BINARY_DIR}/generated/ui/fb_asset_icons_data.h
 */
#include "ui/fb_asset_icons.h"
#include "ui/icon.h"          /* ui_icon_downscale_rgba */
#include "core/imgproc.h"     /* img_premultiply_rgba */
#include "stb_image.h"
#include "ui/fb_asset_icons_data.h"

#include <stdlib.h>

/* The cache holds the pixels that are DRAWN: one exact-size downscale per
 * (icon, size). The 128 px master exists only long enough to make one and is
 * freed again.
 *
 * The renderer is keyed on fb_asset_icon_cache_key() — one stable address per
 * (icon, size), as icon.c and seti_icons.c do — so a buffer may be freed
 * without a reused address aliasing a stale texture. Each icon keeps a few
 * sizes, the least recently used one replaced, and ONE byte budget covers all
 * of them: the least recently used buffer anywhere is dropped until a new one
 * fits. A size asked for again pays one more decode; eight slots is two
 * displays' worth of the three places an icon is drawn, so that does not
 * happen frame to frame. 1 MB is every slot of every logo at up to 60 px. */
#define FB_ICON_SIZE_SLOTS   8
#define FB_ICON_CACHE_BUDGET MB(1)
#define FB_ICON_REQ_NATIVE   (-1)     /* "the master as it is" */
typedef struct {
    i32   req;          /* requested px, or FB_ICON_REQ_NATIVE; 0 = empty */
    i32   w, h;         /* what the buffer holds: req x req, or the native size */
    u8   *rgba;         /* NULL with req set = failed; not retried per frame */
    u64   used;         /* g_clock at the last request */
    bool  ink_done;     /* ink_l / ink_r measured */
    f32   ink_l, ink_r;
} FbIconScaled;

typedef struct {
    const u8 *png_data;
    i32       png_size;
    i32       w, h;         /* native size; known once `seen` */
    bool      seen;         /* decoded at least once: w, h and mask are set */
    bool      failed;       /* stb said no: never retry */
    bool      mask;         /* silhouette re-inked to white; draw tinted */
    FbIconScaled scaled[FB_ICON_SIZE_SLOTS];
} FbAssetIconSlot;

/* A silhouette's fill only needs re-inking when it FAILS one of the two
 * panels: below this it is lost on a dark theme, above it on a light one. Same
 * band agent_icons.c keeps its flat marks in, so the two icon families make
 * the same call about the same artwork. */
#define FB_MASK_KEEP_MIN_LUMA 0.34f
#define FB_MASK_KEEP_MAX_LUMA 0.66f
/* Use the fill's luminance to choose whether a monochrome mark needs contrast
 * adjustment. Saturated marks retain their brand color because hue keeps them
 * legible on both panels. */
#define FB_MASK_MAX_SAT 0.25f

/* Every opaque texel the same colour? Measured on the DECODED master, before
 * premultiplication. Texels under half alpha are edge antialiasing and say
 * nothing about the fill; a tolerance of a few levels absorbs export drift.
 * Reports that colour's luminance and HSV saturation. */
static bool rgba_is_flat(const u8 *px, i32 w, i32 h, f32 *out_luma, f32 *out_sat) {
    if (!px || w <= 0 || h <= 0) return false;
    i32 n = w * h;
    bool seen = false;
    u8 r0 = 0, g0 = 0, b0 = 0;
    for (i32 i = 0; i < n; i++) {
        if (px[i * 4 + 3] < 128) continue;
        u8 r = px[i * 4 + 0], g = px[i * 4 + 1], b = px[i * 4 + 2];
        if (!seen) { r0 = r; g0 = g; b0 = b; seen = true; continue; }
        i32 dr = (i32)r - r0, dg = (i32)g - g0, db = (i32)b - b0;
        if (dr < 0) dr = -dr;
        if (dg < 0) dg = -dg;
        if (db < 0) db = -db;
        if (dr > 6 || dg > 6 || db > 6) return false;
    }
    if (seen && out_luma)
        *out_luma = (0.2126f * (f32)r0 + 0.7152f * (f32)g0 +
                     0.0722f * (f32)b0) / 255.0f;
    if (seen && out_sat) {
        u8 mx = r0 > g0 ? (r0 > b0 ? r0 : b0) : (g0 > b0 ? g0 : b0);
        u8 mn = r0 < g0 ? (r0 < b0 ? r0 : b0) : (g0 < b0 ? g0 : b0);
        *out_sat = mx ? (f32)(mx - mn) / (f32)mx : 0.0f;
    }
    return seen;
}

static FbAssetIconSlot g_fb_asset_icons[FB_ASSET_ICON_COUNT] = {
    [FB_ASSET_ICON_NONE]     = { .png_data = NULL, .png_size = 0 },
    [FB_ASSET_ICON_FOLDER]   = { .png_data = FB_ICON_FOLDER_data, .png_size = FB_ICON_FOLDER_size },
    [FB_ASSET_ICON_FILE]     = { .png_data = FB_ICON_FILE_data, .png_size = FB_ICON_FILE_size },
    [FB_ASSET_ICON_CODE]     = { .png_data = FB_ICON_CODE_data, .png_size = FB_ICON_CODE_size },
    [FB_ASSET_ICON_TEXT]     = { .png_data = FB_ICON_TEXT_data, .png_size = FB_ICON_TEXT_size },
    [FB_ASSET_ICON_IMAGE]    = { .png_data = FB_ICON_IMAGE_data, .png_size = FB_ICON_IMAGE_size },
    [FB_ASSET_ICON_VIDEO]    = { .png_data = FB_ICON_VIDEO_data, .png_size = FB_ICON_VIDEO_size },
    [FB_ASSET_ICON_AUDIO]    = { .png_data = FB_ICON_AUDIO_data, .png_size = FB_ICON_AUDIO_size },
    [FB_ASSET_ICON_ARCHIVE]  = { .png_data = FB_ICON_ARCHIVE_data, .png_size = FB_ICON_ARCHIVE_size },
    [FB_ASSET_ICON_PDF]      = { .png_data = FB_ICON_PDF_data, .png_size = FB_ICON_PDF_size },
    [FB_ASSET_ICON_TERMINAL] = { .png_data = FB_ICON_TERMINAL_data, .png_size = FB_ICON_TERMINAL_size },
    [FB_ASSET_ICON_GEAR]     = { .png_data = FB_ICON_GEAR_data, .png_size = FB_ICON_GEAR_size },
    [FB_ASSET_ICON_GIT]      = { .png_data = FB_ICON_GIT_data, .png_size = FB_ICON_GIT_size },
    [FB_ASSET_ICON_LOCK]     = { .png_data = FB_ICON_LOCK_data, .png_size = FB_ICON_LOCK_size },
    [FB_ASSET_ICON_UP]       = { .png_data = FB_ICON_UP_data, .png_size = FB_ICON_UP_size },
    [FB_ASSET_ICON_REFRESH]  = { .png_data = FB_ICON_REFRESH_data, .png_size = FB_ICON_REFRESH_size },
    [FB_ASSET_ICON_CLOSE]    = { .png_data = FB_ICON_CLOSE_data, .png_size = FB_ICON_CLOSE_size },
    [FB_ASSET_ICON_LANG_PYTHON    ] = { .png_data = FB_ICON_LANG_PYTHON_data, .png_size = FB_ICON_LANG_PYTHON_size },
    [FB_ASSET_ICON_LANG_ERLANG    ] = { .png_data = FB_ICON_LANG_ERLANG_data, .png_size = FB_ICON_LANG_ERLANG_size },
    [FB_ASSET_ICON_LANG_FORTRAN   ] = { .png_data = FB_ICON_LANG_FORTRAN_data, .png_size = FB_ICON_LANG_FORTRAN_size },
    [FB_ASSET_ICON_LANG_BUN       ] = { .png_data = FB_ICON_LANG_BUN_data, .png_size = FB_ICON_LANG_BUN_size },
    [FB_ASSET_ICON_LANG_VB        ] = { .png_data = FB_ICON_LANG_VB_data, .png_size = FB_ICON_LANG_VB_size },
    [FB_ASSET_ICON_LANG_DELPHI    ] = { .png_data = FB_ICON_LANG_DELPHI_data, .png_size = FB_ICON_LANG_DELPHI_size },
    [FB_ASSET_ICON_LANG_MATLAB    ] = { .png_data = FB_ICON_LANG_MATLAB_data, .png_size = FB_ICON_LANG_MATLAB_size },
    [FB_ASSET_ICON_LANG_ASTRO     ] = { .png_data = FB_ICON_LANG_ASTRO_data, .png_size = FB_ICON_LANG_ASTRO_size },
    [FB_ASSET_ICON_LANG_NIX       ] = { .png_data = FB_ICON_LANG_NIX_data, .png_size = FB_ICON_LANG_NIX_size },
};

static u64   g_clock;
static usize g_bytes;

static usize fb_buf_bytes(const FbIconScaled *e) {
    return e->rgba ? (usize)e->w * (usize)e->h * 4u : 0;
}

static void fb_scaled_drop(FbIconScaled *e) {
    g_bytes -= fb_buf_bytes(e);
    free(e->rgba);
    *e = (FbIconScaled){0};
}

usize fb_asset_icon_cache_bytes(void) { return g_bytes; }

/* Decode the master: premultiplied, a silhouette re-inked to white. The caller
 * owns it. Also where the icon's native size and its mask verdict are learned,
 * which is why they survive the master being freed. */
static u8 *fb_decode_master(FbAssetIconSlot *s) {
    if (s->failed) return NULL;
    int iw = 0, ih = 0, ch = 0;
    u8 *px = stbi_load_from_memory(s->png_data, s->png_size, &iw, &ih, &ch, 4);
    if (!px || iw <= 0 || ih <= 0) {
        free(px);
        s->failed = true;
        return NULL;
    }
    /* A flat silhouette outside the keep band is re-inked to WHITE so that,
     * drawn through the tinted image path, it comes out in exactly the colour
     * the caller asks for — the alpha carries the whole shape, so the recolour
     * is lossless. Done here, before premultiplication, for the same reason
     * the premultiply is: every downscale inherits it. */
    f32 luma = 0.0f, sat = 0.0f;
    bool mask = rgba_is_flat(px, iw, ih, &luma, &sat) && sat < FB_MASK_MAX_SAT &&
                (luma < FB_MASK_KEEP_MIN_LUMA || luma > FB_MASK_KEEP_MAX_LUMA);
    if (mask) {
        i32 n = iw * ih;
        for (i32 i = 0; i < n; i++) {
            px[i * 4 + 0] = 255;
            px[i * 4 + 1] = 255;
            px[i * 4 + 2] = 255;
        }
    }
    /* stb hands back straight alpha; the renderer's image blend is
     * (ONE, 1-SrcAlpha) and needs premultiplied. */
    img_premultiply_rgba(px, iw, ih);
    s->w = iw;
    s->h = ih;
    s->mask = mask;
    s->seen = true;
    return px;
}

static FbAssetIconSlot *fb_slot(FbAssetIconKind kind) {
    if (kind <= FB_ASSET_ICON_NONE || kind >= FB_ASSET_ICON_COUNT) return NULL;
    FbAssetIconSlot *s = &g_fb_asset_icons[kind];
    return (s->png_data && s->png_size > 0) ? s : NULL;
}

/* The cached buffer for `target_px` (<= 0: the master's own size), made on a
 * miss. NULL when the PNG does not decode. */
static FbIconScaled *fb_scaled_get(FbAssetIconSlot *s, i32 target_px) {
    i32 req = target_px > 0 ? target_px : FB_ICON_REQ_NATIVE;
    FbIconScaled *slot = &s->scaled[0];   /* where a miss goes: empty, else the oldest */
    for (i32 i = 0; i < FB_ICON_SIZE_SLOTS; i++) {
        FbIconScaled *e = &s->scaled[i];
        if (e->req == req) {
            e->used = ++g_clock;
            return e->rgba ? e : NULL;
        }
        if (slot->req != 0 && (e->req == 0 || e->used < slot->used)) slot = e;
    }

    /* Make the pixels before making room: a failure must not cost a live
     * buffer. A request the master cannot be reduced to (an upscale, its own
     * size, a non-square source) is answered with the master itself. */
    u8 *px = fb_decode_master(s);
    i32 bw = s->w, bh = s->h;
    if (px && req != FB_ICON_REQ_NATIVE && req < s->w && req < s->h && s->w == s->h) {
        u8 *small = ui_icon_downscale_rgba(px, s->w, s->h, req, req);
        if (small) {
            free(px);
            px = small;
            bw = bh = req;
        }
    }

    fb_scaled_drop(slot);
    usize want = px ? (usize)bw * (usize)bh * 4u : 0;
    while (g_bytes + want > FB_ICON_CACHE_BUDGET) {
        FbIconScaled *oldest = NULL;
        for (i32 k = 0; k < FB_ASSET_ICON_COUNT; k++) {
            for (i32 i = 0; i < FB_ICON_SIZE_SLOTS; i++) {
                FbIconScaled *c = &g_fb_asset_icons[k].scaled[i];
                if (c->rgba && (!oldest || c->used < oldest->used)) oldest = c;
            }
        }
        if (!oldest) break;
        fb_scaled_drop(oldest);
    }
    slot->req  = req;
    slot->w    = px ? bw : 0;
    slot->h    = px ? bh : 0;
    slot->rgba = px;
    slot->used = ++g_clock;
    g_bytes += want;
    return px ? slot : NULL;
}

const u8 *fb_asset_icon_rgba_scaled(FbAssetIconKind kind, i32 target_px,
                                    i32 *w, i32 *h) {
    FbAssetIconSlot *s = fb_slot(kind);
    FbIconScaled *e = s ? fb_scaled_get(s, target_px) : NULL;
    if (!e) return NULL;
    if (w) *w = e->w;
    if (h) *h = e->h;
    return e->rgba;
}

const u8 *fb_asset_icon_rgba(FbAssetIconKind kind, i32 *w, i32 *h) {
    return fb_asset_icon_rgba_scaled(kind, 0, w, h);
}

const void *fb_asset_icon_cache_key(FbAssetIconKind kind, i32 px) {
    /* One address per (icon, size), never read: the renderer's image cache
     * needs an identity that outlives the pixels. */
    #define FB_ICON_KEY_PX 256
    static char anchor[FB_ASSET_ICON_COUNT][FB_ICON_KEY_PX];
    if (kind <= FB_ASSET_ICON_NONE || kind >= FB_ASSET_ICON_COUNT) return NULL;
    if (px < 0) px = 0;
    if (px >= FB_ICON_KEY_PX) px = FB_ICON_KEY_PX - 1;
    return &anchor[kind][px];
    #undef FB_ICON_KEY_PX
}

bool fb_asset_icon_is_mask(FbAssetIconKind kind) {
    FbAssetIconSlot *s = fb_slot(kind);
    if (!s) return false;
    /* The verdict is the decode's. A draw has always decoded by the time it
     * asks; a caller that has not pays one decode for it and keeps nothing. */
    if (!s->seen) free(fb_decode_master(s));
    return s->seen && s->mask;
}

/* Columns carrying ink in a premultiplied RGBA square: alpha above ~10 %.
 * Below that is the antialiased fringe, which the eye does not read as an
 * edge — the same cut-off the Lucide rasteriser's insets use. */
static void measure_ink(const u8 *px, i32 w, i32 h, f32 *left, f32 *right) {
    i32 first = -1, last = -1;
    for (i32 x = 0; x < w; x++) {
        bool ink = false;
        for (i32 y = 0; y < h && !ink; y++)
            if (px[(y * w + x) * 4 + 3] >= 24) ink = true;
        if (!ink) continue;
        if (first < 0) first = x;
        last = x;
    }
    if (first < 0) { *left = 0.0f; *right = 0.0f; return; }
    *left  = (f32)first;
    *right = (f32)(w - 1 - last);
}

void fb_asset_icon_ink_insets(FbAssetIconKind kind, i32 size_px,
                              f32 *left, f32 *right) {
    if (left)  *left  = 0.0f;
    if (right) *right = 0.0f;
    if (size_px <= 0) return;
    FbAssetIconSlot *s = fb_slot(kind);
    FbIconScaled *e = s ? fb_scaled_get(s, size_px) : NULL;
    if (!e || e->w <= 0 || e->h <= 0) return;

    if (!e->ink_done) {
        measure_ink(e->rgba, e->w, e->h, &e->ink_l, &e->ink_r);
        e->ink_done = true;
    }
    /* The master was handed back (nothing to reduce at this size): its insets
     * are in ITS pixels, so scale them to the box that was asked about. */
    f32 k = (f32)size_px / (f32)e->w;
    if (left)  *left  = e->ink_l * k;
    if (right) *right = e->ink_r * k;
}
