/*
 * Liu — Lucide icon rasteriser. See lucide.h.
 */
#include "ui/lucide.h"
#include "ui/icon.h"          /* ui_icon_downscale_rgba */

#include "ui/lucide_data.h"   /* generated: LUCIDE_SVGS / LUCIDE_SVG_COUNT */

/* Declarations only — the implementation is compiled once, in nanosvg_impl.c. */
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include <stdlib.h>
#include <string.h>

/* Rasterise at 2× and box-average down. nanosvg's rasteriser antialiases with
 * a fixed subsample count that thins hairlines at small sizes; supersampling
 * lands stroke weight where the vector says it should be. Above this size the
 * direct raster is already clean and 2× would just cost memory. */
#define LUCIDE_SS_MAX_PX 64
#define LUCIDE_MIN_PX     4
#define LUCIDE_MAX_PX   512

typedef struct {
    const char *name;   /* points into LUCIDE_SVGS, never freed */
    i32   px;
    u8   *rgba;         /* premultiplied, px*px*4 */
    f32   ink_left, ink_right;
    bool  insets_done;
    u64   last_used;    /* LRU stamp from g_cache_clock; 0 only while unused */
} LucideCacheEntry;

/* Small linear cache. The chrome draws on the order of a few dozen icons at two
 * or three sizes, so in practice it neither fills nor evicts.
 *
 * The cap is a recycling limit, not a wall (past a wall the icon would simply
 * not paint): LRU by last access, with a byte budget on top so a pathological
 * run of large sizes can't sit on 128 × 512² × 4 = 134 MB.
 *
 * Recycling is safe here for two independent reasons:
 *
 *  1. icon.c — the only in-tree consumer — keys the renderer's image cache by a
 *     stable per-(kind, size) anchor, NOT by the pixel pointer (see the banner
 *     in icon_draw). So a recycled malloc address cannot alias a previously
 *     uploaded texture, and a re-raster of the same (name, px) produces
 *     byte-identical pixels for a key the renderer already holds.
 *  2. renderer_draw_image_tinted() copies `pixels` into a GPU texture
 *     synchronously (Metal replaceRegion / GL glTexImage2D) before it returns;
 *     nothing reads the buffer afterwards.
 *
 * The rule a caller must keep: treat the returned buffer as valid until the
 * next lucide call that MISSES. Draw with it, don't store it. */
#define LUCIDE_CACHE_CAP    128
#define LUCIDE_CACHE_BUDGET MB(4)
static LucideCacheEntry g_cache[LUCIDE_CACHE_CAP];
static i32   g_cache_count;
static usize g_cache_bytes;
static u64   g_cache_clock;

static usize lucide_entry_bytes(i32 px) {
    return (usize)px * (usize)px * 4u;
}

/* Least-recently-used live entry, or NULL when the table holds nothing. */
static LucideCacheEntry *lucide_cache_victim(void) {
    LucideCacheEntry *lru = NULL;
    for (i32 i = 0; i < g_cache_count; i++) {
        if (!g_cache[i].rgba) continue;
        if (!lru || g_cache[i].last_used < lru->last_used) lru = &g_cache[i];
    }
    return lru;
}

static void lucide_cache_drop(LucideCacheEntry *e) {
    if (!e || !e->rgba) return;
    usize b = lucide_entry_bytes(e->px);
    g_cache_bytes = (g_cache_bytes >= b) ? g_cache_bytes - b : 0;
    free(e->rgba);
    e->rgba = NULL;
    e->name = NULL;
    e->px = 0;
    e->ink_left = e->ink_right = 0.0f;
    e->insets_done = false;
    e->last_used = 0;
}

static const LucideSvg *lucide_find(const char *name) {
    if (!name || !name[0]) return NULL;
    for (i32 i = 0; i < LUCIDE_SVG_COUNT; i++) {
        if (strcmp(LUCIDE_SVGS[i].name, name) == 0) return &LUCIDE_SVGS[i];
    }
    return NULL;
}

bool lucide_icon_exists(const char *name) { return lucide_find(name) != NULL; }
i32  lucide_icon_count(void) { return LUCIDE_SVG_COUNT; }

const char *lucide_icon_name_at(i32 index) {
    if (index < 0 || index >= LUCIDE_SVG_COUNT) return NULL;
    return LUCIDE_SVGS[index].name;
}

/* nsvgParse() mutates the buffer it is handed, and the embedded source lives
 * in read-only .rodata — so parse a copy. `currentColor` is meaningless
 * without a document context; rewriting it to white makes nanosvg produce the
 * white mask the caller tints, instead of falling back to black. */
static NSVGimage *lucide_parse(const LucideSvg *svg) {
    usize len = (usize)svg->size;
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, svg->svg, len);
    copy[len] = '\0';

    for (char *p = copy; (p = strstr(p, "currentColor")) != NULL; ) {
        /* Same byte count (12) as "currentColor", so this stays in place and
         * the document keeps its length. nanosvg's colour parser reads the six
         * hex digits after '#' and ignores the trailing spaces. */
        memcpy(p, "#ffffff     ", 12);
        p += 12;
    }

    NSVGimage *img = nsvgParse(copy, "px", 96.0f);
    free(copy);
    return img;
}

/* Rasterise into a straight-alpha buffer at `px`, then premultiply. */
static u8 *lucide_raster_at(const LucideSvg *svg, i32 px) {
    NSVGimage *img = lucide_parse(svg);
    if (!img) return NULL;
    if (img->width <= 0.0f || img->height <= 0.0f) { nsvgDelete(img); return NULL; }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(img); return NULL; }

    u8 *out = (u8 *)calloc((usize)px * (usize)px * 4u, 1);
    if (!out) { nsvgDeleteRasterizer(rast); nsvgDelete(img); return NULL; }

    /* Uniform scale + centre: Lucide art is square (24×24), but honouring both
     * axes keeps a non-square source from stretching. */
    f32 scale = (f32)px / (img->width > img->height ? img->width : img->height);
    f32 tx = ((f32)px - img->width  * scale) * 0.5f;
    f32 ty = ((f32)px - img->height * scale) * 0.5f;

    nsvgRasterize(rast, img, tx, ty, scale, out, px, px, px * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);

    /* nanosvg emits straight alpha; every renderer image upload consumes
     * premultiplied (the pipeline blend is ONE, 1-SrcAlpha). Doing it here
     * means the downscaler below is alpha-correct too. */
    for (usize i = 0, n = (usize)px * (usize)px; i < n; i++) {
        u8 *p = out + i * 4u;
        u32 a = p[3];
        p[0] = (u8)((p[0] * a + 127u) / 255u);
        p[1] = (u8)((p[1] * a + 127u) / 255u);
        p[2] = (u8)((p[2] * a + 127u) / 255u);
    }
    return out;
}

static u8 *lucide_render(const LucideSvg *svg, i32 px) {
    if (px > LUCIDE_SS_MAX_PX) return lucide_raster_at(svg, px);

    /* Supersample: raster at 2×, area-average down to px. */
    u8 *big = lucide_raster_at(svg, px * 2);
    if (!big) return NULL;
    u8 *small = ui_icon_downscale_rgba(big, px * 2, px * 2, px, px);
    free(big);
    return small ? small : lucide_raster_at(svg, px);
}

#if LIU_ANIM_PROBE
/* Probe instrumentation: how many icons this frame had to rasterise, and what
 * that cost. A cold cache is invisible on screen except as a dropped frame, so
 * the only way to attribute one is to count the misses. */
#include "platform/platform.h"
i32 g_lucide_probe_raster = 0, g_lucide_probe_tint = 0;
f64 g_lucide_probe_secs = 0.0;
#endif

static LucideCacheEntry *lucide_cache_get(const char *name, i32 px) {
    const LucideSvg *svg = lucide_find(name);
    if (!svg) return NULL;
    if (px < LUCIDE_MIN_PX || px > LUCIDE_MAX_PX) return NULL;

    for (i32 i = 0; i < g_cache_count; i++) {
        if (g_cache[i].rgba && g_cache[i].px == px &&
            strcmp(g_cache[i].name, name) == 0) {
            g_cache[i].last_used = ++g_cache_clock;
            return &g_cache[i];
        }
    }

#if LIU_ANIM_PROBE
    f64 _t0 = platform_time_sec();
#endif
    u8 *rgba = lucide_render(svg, px);
#if LIU_ANIM_PROBE
    g_lucide_probe_secs += platform_time_sec() - _t0;
    g_lucide_probe_raster++;
#endif
    /* Rasterise BEFORE picking a victim: a failed raster must not cost a live
     * entry its pixels. */
    if (!rgba) return NULL;

    /* Trim to the byte budget, then take a free slot or recycle the LRU one.
     * Both loops only ever touch entries other than the one being inserted,
     * which has not been placed yet. */
    usize want = lucide_entry_bytes(px);
    while (g_cache_bytes + want > LUCIDE_CACHE_BUDGET) {
        LucideCacheEntry *victim = lucide_cache_victim();
        if (!victim) break;              /* nothing left to reclaim */
        lucide_cache_drop(victim);
    }

    LucideCacheEntry *e = NULL;
    for (i32 i = 0; i < g_cache_count; i++) {
        if (!g_cache[i].rgba) { e = &g_cache[i]; break; }   /* freed by a trim */
    }
    if (!e && g_cache_count < LUCIDE_CACHE_CAP) e = &g_cache[g_cache_count++];
    if (!e) {
        e = lucide_cache_victim();
        if (!e) { free(rgba); return NULL; }   /* cannot happen: table is full */
        lucide_cache_drop(e);
    }

    e->name = svg->name;
    e->px = px;
    e->rgba = rgba;
    e->ink_left = e->ink_right = 0.0f;
    e->insets_done = false;
    e->last_used = ++g_cache_clock;
    g_cache_bytes += want;
    return e;
}

/* Tinted variants. Separate, larger table than the mask cache: the chrome
 * draws the same icon in a handful of states (normal, hover, accent, dimmed)
 * and each is one entry. */
typedef struct {
    const char *name;
    i32 px;
    u32 rgba;      /* packed tint, the cache key alongside (name, px) */
    u8 *pixels;
} LucideTintEntry;

#define LUCIDE_TINT_CAP 512
static LucideTintEntry g_tints[LUCIDE_TINT_CAP];
static i32 g_tint_count;

const u8 *lucide_icon_rgba_tinted(const char *name, i32 px,
                                  u8 red, u8 green, u8 blue, u8 alpha) {
    u32 key = ((u32)red << 24) | ((u32)green << 16) | ((u32)blue << 8) | (u32)alpha;
    for (i32 i = 0; i < g_tint_count; i++) {
        if (g_tints[i].px == px && g_tints[i].rgba == key &&
            strcmp(g_tints[i].name, name) == 0)
            return g_tints[i].pixels;
    }

    LucideCacheEntry *mask = lucide_cache_get(name, px);
    if (!mask) return NULL;
    /* Untinted beats nothing. Valid until the next lucide call that misses —
     * the caller draws with it immediately (the upload is synchronous). */
    if (g_tint_count >= LUCIDE_TINT_CAP) return mask->rgba;
#if LIU_ANIM_PROBE
    g_lucide_probe_tint++;
#endif

    usize n = (usize)px * (usize)px;
    u8 *out = (u8 *)malloc(n * 4u);
    if (!out) return NULL;

    /* The mask is premultiplied white, so its RGB already equals its coverage.
     * Multiplying by the tint keeps it premultiplied — which is what the
     * renderer's (ONE, 1-SrcAlpha) blend consumes. */
    for (usize i = 0; i < n; i++) {
        const u8 *s = mask->rgba + i * 4u;
        u8 *d = out + i * 4u;
        u32 cov = s[3];
        d[0] = (u8)((cov * red   * alpha + 32512u) / 65025u);
        d[1] = (u8)((cov * green * alpha + 32512u) / 65025u);
        d[2] = (u8)((cov * blue  * alpha + 32512u) / 65025u);
        d[3] = (u8)((cov * alpha + 127u) / 255u);
    }

    LucideTintEntry *e = &g_tints[g_tint_count++];
    e->name = mask->name;
    e->px = px;
    e->rgba = key;
    e->pixels = out;
    return out;
}

const u8 *lucide_icon_rgba(const char *name, i32 px) {
    LucideCacheEntry *e = lucide_cache_get(name, px);
    return e ? e->rgba : NULL;
}

void lucide_icon_ink_insets(const char *name, i32 px, f32 *left, f32 *right) {
    if (left)  *left = 0.0f;
    if (right) *right = 0.0f;
    LucideCacheEntry *e = lucide_cache_get(name, px);
    if (!e) return;

    if (!e->insets_done) {
        i32 first = -1, last = -1;
        for (i32 x = 0; x < e->px; x++) {
            bool inked = false;
            for (i32 y = 0; y < e->px; y++) {
                if (e->rgba[((usize)y * (usize)e->px + (usize)x) * 4u + 3u] > 8u) {
                    inked = true;
                    break;
                }
            }
            if (inked) { if (first < 0) first = x; last = x; }
        }
        e->ink_left  = (first < 0) ? 0.0f : (f32)first;
        e->ink_right = (last  < 0) ? 0.0f : (f32)(e->px - 1 - last);
        e->insets_done = true;
    }
    if (left)  *left  = e->ink_left;
    if (right) *right = e->ink_right;
}
