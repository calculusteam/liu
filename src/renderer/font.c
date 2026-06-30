/*
 * Liu — font atlas with full Unicode support
 * Dynamic glyph cache: rasterizes glyphs on demand into a large atlas.
 * Supports ASCII, Latin Extended, Box Drawing, Block Elements, Braille,
 * Powerline, Arrows, Math symbols, and any other Unicode in the font.
 */
#include "renderer/renderer.h"
#include "core/utf8.h"
#include "core/string_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include <pthread.h>

#ifdef PLATFORM_MACOS
    #ifdef USE_METAL
        #import <Metal/Metal.h>
    #else
        #include <OpenGL/gl3.h>
    #endif
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <CoreText/CoreText.h>
    #include <CoreGraphics/CoreGraphics.h>
#elif defined(PLATFORM_LINUX)
    #include <GL/gl.h>
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <unistd.h>
#else
    #include <GL/gl.h>
#endif
#if !defined(__APPLE__) && !defined(GL_VERSION_1_5)
    #include <GL/glext.h>   /* GL_R8/GL_CLAMP_TO_EDGE: Windows' gl.h is 1.1-only */
#endif

#ifndef ATLAS_BPP
#define ATLAS_BPP 1
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

const char *font_user_dir(void);

/* =========================================================================
 * Dynamic glyph cache
 * ========================================================================= */

#define GLYPH_CACHE_SIZE 2048
/* Initial atlas side (pixels). 768×768 RGBA = 2.25 MB — fits ~390 glyphs at
 * 27×54 cells (12pt retina) and ~240 at 34×68 (24pt), which comfortably covers
 * ASCII + common Unicode for every realistic typing session. Cut from 1024
 * (4 MB) to save ~1.75 MB of IOSurface/GPU memory at idle. */
#define ATLAS_INIT_SIZE   768
#define ATLAS_MAX_SIZE    4096

typedef struct {
    u32  codepoint;
    f32  u0, v0, u1, v1;      /* UV in atlas */
    u64  last_used_frame;     /* coarse LRU timestamp; bumped per frame drain */
    bool valid;
    bool exhausted;           /* true = atlas was full when this cp was first
                              * requested; UVs point at the space glyph and the
                              * codepoint must NOT be re-rasterized every frame.
                              * Cleared automatically when the table is wiped
                              * (memset to 0) on font reload / eviction rebuild. */
} CachedGlyph;

/* Hash table for fast glyph lookup. Open-addressed with linear probing; no
 * tombstones (lookup stops on first empty slot). Eviction therefore must
 * rebuild the table — see cache_evict_cold().
 *
 * NOTE: The atlas texture itself is a single-allocation backing store and is
 * NOT LRU'd here — only the lookup table. After a font-size change the atlas
 * may still hold stale glyph pixels at unused UVs, but those pixels cost
 * nothing at runtime; reclaiming them would require atlas repacking, which is
 * out of scope. */
static CachedGlyph g_cache[GLYPH_CACHE_SIZE];
static u32         g_cache_used = 0;       /* count of valid slots */
static u64         g_glyph_frame = 0;      /* LRU clock, bumped per drain */
static stbtt_fontinfo g_font_info;
static u8 *g_font_data = NULL;
static usize g_font_data_size = 0;
static f32 g_font_scale = 0;
static f32 g_font_ascent = 0;

/* Synthetic stroke width applied at glyph rasterisation. 0 = native weight,
 * >0 thickens. Set by font_atlas_create from Config.font_weight. */
static f32 g_font_weight = 0.0f;

/* Forward declare MAX_FALLBACK_FONTS */
#define MAX_FALLBACK_FONTS 16

#ifdef PLATFORM_MACOS
/* Core Text font handle — native macOS text rendering */
static CTFontRef g_ct_font = NULL;
static CTFontRef g_ct_fallbacks[MAX_FALLBACK_FONTS];

/* Forward declarations for lazy-fallback plumbing defined further down. */
static void flush_lazy_fallbacks(void);
static i32 g_ct_fallback_count = 0;

/* Serializes all fallback-font-chain state (g_fallbacks / g_fallback_count,
 * g_ct_fallbacks / g_ct_fallback_count, g_cp_fb_cache) between the render
 * thread and the async raster worker. Both threads discover/append fallbacks,
 * and the unsynchronized check-then-act counter increments could push a count
 * past MAX_FALLBACK_FONTS (OOB array access) and leak font refs. This is always
 * the INNERMOST lock: a thread may hold g_ct_mu and then take g_fb_mu, never
 * the reverse. */
static pthread_mutex_t g_fb_mu = PTHREAD_MUTEX_INITIALIZER;

/* Supersampling factor. Core Text renders at OVERSAMPLE× the target cell size,
 * then a 2×2 box filter downsamples into the atlas. Pure grayscale AA — LCD
 * subpixel filtering is intentionally OFF because:
 *   (1) On Retina the human eye can't resolve LCD ordering; macOS itself
 *       dropped system LCD smoothing in Mojave.
 *   (2) Mixing 2× supersample with LCD subpixel + 2×2 box downsample destroys
 *       the subpixel pattern and leaves coloured fringing without sharpening. */
#define FONT_OVERSAMPLE 2

/* Pooled CGBitmapContext — avoids kernel alloc/dealloc per glyph.
 * Dimensions are OVERSAMPLE× the target cell; readback downsamples to R8. */
static CGContextRef g_glyph_ctx = NULL;
static u8          *g_glyph_buf = NULL;    /* OVERSAMPLE×, 1 byte/pixel (alpha) */
static i32          g_glyph_ctx_w = 0;     /* stored in target (1×) cells */
static i32          g_glyph_ctx_h = 0;

static CGContextRef get_glyph_context(i32 cw, i32 ch) {
    if (g_glyph_ctx && g_glyph_ctx_w == cw && g_glyph_ctx_h == ch)
        return g_glyph_ctx;
    if (g_glyph_ctx) CGContextRelease(g_glyph_ctx);
    free(g_glyph_buf);
    i32 bw = cw * FONT_OVERSAMPLE;
    i32 bh = ch * FONT_OVERSAMPLE;
    g_glyph_buf = calloc(1, (usize)(bw * bh));
    /* Alpha-only context: CG writes one byte per pixel = geometric glyph
     * coverage. No colour space, no gamma encoding involved → averaging in
     * the downsample is mathematically correct. */
    g_glyph_ctx = CGBitmapContextCreate(g_glyph_buf, (size_t)bw, (size_t)bh,
                                         8, (size_t)bw, NULL,
                                         (CGBitmapInfo)kCGImageAlphaOnly);
    if (g_glyph_ctx) {
        CGContextSetAllowsAntialiasing(g_glyph_ctx, true);
        CGContextSetShouldAntialias(g_glyph_ctx, true);
        /* LCD subpixel filter OFF — see FONT_OVERSAMPLE comment. */
        CGContextSetAllowsFontSmoothing(g_glyph_ctx, false);
        CGContextSetShouldSmoothFonts(g_glyph_ctx, false);
        /* Subpixel positioning OFF — our glyph cache is keyed by codepoint
         * only, so a position-dependent raster would be cached at one x and
         * reused at every other x, smearing the result. Pixel-snapped glyphs
         * stay sharp and predictable for a monospace terminal grid. */
        CGContextSetAllowsFontSubpixelPositioning(g_glyph_ctx, false);
        CGContextSetShouldSubpixelPositionFonts(g_glyph_ctx, false);
        CGContextSetAllowsFontSubpixelQuantization(g_glyph_ctx, false);
        CGContextSetShouldSubpixelQuantizeFonts(g_glyph_ctx, false);
        CGContextScaleCTM(g_glyph_ctx, (CGFloat)FONT_OVERSAMPLE, (CGFloat)FONT_OVERSAMPLE);
        g_glyph_ctx_w = cw;
        g_glyph_ctx_h = ch;
    }
    return g_glyph_ctx;
}

/* 2×2 box-filter downsample of the OVERSAMPLE× alpha buffer into R8.
 * CG writes geometric coverage (linear), so a plain arithmetic mean is the
 * mathematically correct gamma-aware operation here. */
static void downsample_glyph_buf(u8 *dst, i32 cw, i32 ch) {
    i32 bw = cw * FONT_OVERSAMPLE;
#if FONT_OVERSAMPLE == 2
    for (i32 y = 0; y < ch; y++) {
        const u8 *row0 = g_glyph_buf + (y * 2)     * bw;
        const u8 *row1 = g_glyph_buf + (y * 2 + 1) * bw;
        u8       *out  = dst + y * cw;
        for (i32 x = 0; x < cw; x++) {
            u32 s = row0[x*2 + 0] + row0[x*2 + 1] + row1[x*2 + 0] + row1[x*2 + 1];
            out[x] = (u8)((s + 2) >> 2);
        }
    }
#else
    for (i32 y = 0; y < ch; y++) {
        for (i32 x = 0; x < cw; x++) {
            u32 s = 0;
            for (i32 dy = 0; dy < FONT_OVERSAMPLE; dy++)
                for (i32 dx = 0; dx < FONT_OVERSAMPLE; dx++) {
                    s += g_glyph_buf[(y*FONT_OVERSAMPLE + dy) * bw + x*FONT_OVERSAMPLE + dx];
                }
            u32 d = FONT_OVERSAMPLE * FONT_OVERSAMPLE;
            dst[y * cw + x] = (u8)(s / d);
        }
    }
#endif
}

/* Rasterize a glyph using Core Text — pixel-perfect on macOS */
static bool ct_rasterize_glyph(u8 *buf, i32 cw, i32 ch, u32 cp, f32 ascent_px) {
    if (!g_ct_font) return false;

    /* Find which CT font has this glyph — handle surrogate pairs for cp > 0xFFFF */
    CTFontRef font = g_ct_font;
    UniChar uchars[2];
    i32 ulen;
    CGGlyph glyphs[2];
    if (cp > 0xFFFF) {
        /* Supplementary plane: encode as UTF-16 surrogate pair */
        uchars[0] = (UniChar)(((cp - 0x10000) >> 10) + 0xD800);
        uchars[1] = (UniChar)(((cp - 0x10000) & 0x3FF) + 0xDC00);
        ulen = 2;
    } else {
        uchars[0] = (UniChar)cp;
        ulen = 1;
    }
    CGGlyph glyph;
    if (!CTFontGetGlyphsForCharacters(font, uchars, glyphs, ulen)) {
        /* Primary didn't have this codepoint — drain the lazy fallback queue
         * before trying the manual list. First miss incurs the full 9-font
         * load; subsequent misses pay nothing extra. This whole discovery
         * section reads/mutates the shared fallback chain and runs from both
         * the render thread and the async raster worker, so serialize it under
         * g_fb_mu. */
        bool found = false;
        pthread_mutex_lock(&g_fb_mu);
        flush_lazy_fallbacks();
        /* Try manual fallbacks */
        for (i32 i = 0; i < g_ct_fallback_count; i++) {
            if (g_ct_fallbacks[i] && CTFontGetGlyphsForCharacters(g_ct_fallbacks[i], uchars, glyphs, ulen)) {
                font = g_ct_fallbacks[i];
                found = true;
                break;
            }
        }
        if (!found) {
            /* CoreText automatic font fallback — finds any system font with the glyph */
            CFStringRef str = CFStringCreateWithCharacters(NULL, uchars, ulen);
            if (str) {
                CTFontRef fallback = CTFontCreateForString(g_ct_font, str, CFRangeMake(0, ulen));
                CFRelease(str);
                if (fallback) {
                    if (CTFontGetGlyphsForCharacters(fallback, uchars, glyphs, ulen)) {
                        font = fallback;
                        found = true;
                        /* Cache this fallback for future use */
                        if (g_ct_fallback_count < MAX_FALLBACK_FONTS) {
                            g_ct_fallbacks[g_ct_fallback_count++] = fallback;
                        }
                    }
                    if (!found) CFRelease(fallback);
                }
            }
        }
        pthread_mutex_unlock(&g_fb_mu);
        if (!found) return false;
    }
    glyph = glyphs[0];

    /* Reuse pooled alpha-only bitmap context (OVERSAMPLE× the target size).
     * CTM is scaled by OVERSAMPLE× so glyph coords below are in TARGET cell
     * space. In an AlphaOnly context the fill colour's RGB is ignored;
     * only its alpha is written, so "white at alpha=1" produces full
     * geometric coverage where the glyph paints. */
    CGContextRef ctx = get_glyph_context(cw, ch);
    if (!ctx) return false;

    CGContextClearRect(ctx, CGRectMake(0, 0, cw, ch));
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);

    /* Synthetic emboldening: positive font_weight adds a same-colour stroke
     * around the filled glyph (effectively dilating the outline). Stroke
     * width is in oversampled context units, scaled by FONT_OVERSAMPLE so
     * the user-facing weight value reads as "extra target pixels". */
    if (g_font_weight > 0.01f) {
        CGContextSetGrayStrokeColor(ctx, 1.0, 1.0);
        CGContextSetLineWidth(ctx, (CGFloat)(g_font_weight * FONT_OVERSAMPLE));
        CGContextSetLineJoin(ctx, kCGLineJoinRound);
        CGContextSetTextDrawingMode(ctx, kCGTextFillStroke);
    } else {
        CGContextSetTextDrawingMode(ctx, kCGTextFill);
    }

    CGPoint pos = CGPointMake(0, ch - ascent_px);
    CTFontDrawGlyphs(font, &glyph, &pos, 1, ctx);

    downsample_glyph_buf(buf, cw, ch);
    return true;
}

/* Rasterize a color emoji using Core Text — RGBA output */
static bool ct_rasterize_color_glyph(u8 *rgba_buf, i32 cw, i32 ch, u32 cp, f32 ascent_px) {
    (void)ascent_px;
    /* Use Apple Color Emoji font */
    CTFontRef emoji_font = CTFontCreateWithName(CFSTR("Apple Color Emoji"), (CGFloat)(ch * 0.85f), NULL);
    if (!emoji_font) return false;

    UniChar uc = (UniChar)cp;
    CGGlyph glyph;
    if (!CTFontGetGlyphsForCharacters(emoji_font, &uc, &glyph, 1)) {
        /* Try surrogate pair for supplementary plane */
        if (cp > 0xFFFF) {
            UniChar pair[2];
            pair[0] = (UniChar)(((cp - 0x10000) >> 10) + 0xD800);
            pair[1] = (UniChar)(((cp - 0x10000) & 0x3FF) + 0xDC00);
            if (!CTFontGetGlyphsForCharacters(emoji_font, pair, &glyph, 2)) {
                CFRelease(emoji_font);
                return false;
            }
        } else {
            CFRelease(emoji_font);
            return false;
        }
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba_buf, (size_t)cw, (size_t)ch,
                                             8, (size_t)(cw * 4), cs,
                                             (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) { CFRelease(emoji_font); return false; }

    /* Clear to transparent */
    CGContextClearRect(ctx, CGRectMake(0, 0, cw, ch));
    CGContextSetAllowsAntialiasing(ctx, true);
    CGContextSetShouldAntialias(ctx, true);

    /* Center emoji in cell */
    CGSize advance;
    CTFontGetAdvancesForGlyphs(emoji_font, kCTFontOrientationHorizontal, &glyph, &advance, 1);
    f32 x_off = ((f32)cw - (f32)advance.width) / 2.0f;
    if (x_off < 0) x_off = 0;

    CGPoint pos = CGPointMake(x_off, ch * 0.1f);
    CTFontDrawGlyphs(emoji_font, &glyph, &pos, 1, ctx);

    CGContextRelease(ctx);
    CFRelease(emoji_font);
    return true;
}
#endif

/* =========================================================================
 * Fallback font chain — try multiple fonts for missing glyphs
 * ========================================================================= */

/* MAX_FALLBACK_FONTS defined above */

typedef struct {
    stbtt_fontinfo info;
    u8            *data;
    usize          data_size;
    f32            scale;
    bool           loaded;
    char           path[512]; /* font file path for dedup */
} FallbackFont;

static FallbackFont g_fallbacks[MAX_FALLBACK_FONTS];
static i32 g_fallback_count = 0;
static f32 g_pixel_h = 0; /* cached pixel height for dynamic fallback loading */

/* User-configured fallback font paths (set via font_set_user_fallback_config) */
#define MAX_USER_FALLBACK_PATHS 4
static char g_user_fallback_paths[MAX_USER_FALLBACK_PATHS][512];
static i32  g_user_fallback_count = 0;

/* ---- Codepoint-to-fallback-index cache ----
 * Avoids repeated CoreText CTFontCreateForString() calls for the same
 * codepoint. Maps codepoint -> fallback font index (or -1 = primary,
 * -2 = not found). Open-addressing hash table. */

#define CP_FALLBACK_CACHE_SIZE 4096

typedef struct {
    u32 codepoint;  /* 0 = empty slot */
    i16 fb_index;   /* -1 = primary font, -2 = no font found, 0..N = fallback index */
} CpFallbackEntry;

static CpFallbackEntry g_cp_fb_cache[CP_FALLBACK_CACHE_SIZE];

static void cp_fb_cache_clear(void) {
    memset(g_cp_fb_cache, 0, sizeof(g_cp_fb_cache));
}

static i16 cp_fb_cache_lookup(u32 cp) {
    if (cp == 0) return -2;
    u32 h = (cp * 2654435761u) % CP_FALLBACK_CACHE_SIZE;
    for (i32 i = 0; i < 16; i++) {
        u32 slot = (h + (u32)i) % CP_FALLBACK_CACHE_SIZE;
        if (g_cp_fb_cache[slot].codepoint == cp)
            return g_cp_fb_cache[slot].fb_index;
        if (g_cp_fb_cache[slot].codepoint == 0)
            return -2; /* not cached — sentinel for "unknown" */
    }
    return -2;
}

static void cp_fb_cache_insert(u32 cp, i16 fb_index) {
    if (cp == 0) return;
    u32 h = (cp * 2654435761u) % CP_FALLBACK_CACHE_SIZE;
    for (i32 i = 0; i < 16; i++) {
        u32 slot = (h + (u32)i) % CP_FALLBACK_CACHE_SIZE;
        if (g_cp_fb_cache[slot].codepoint == 0 || g_cp_fb_cache[slot].codepoint == cp) {
            g_cp_fb_cache[slot].codepoint = cp;
            g_cp_fb_cache[slot].fb_index = fb_index;
            return;
        }
    }
    /* Table very full — overwrite first probe slot */
    u32 slot = h % CP_FALLBACK_CACHE_SIZE;
    g_cp_fb_cache[slot].codepoint = cp;
    g_cp_fb_cache[slot].fb_index = fb_index;
}

/* Use a distinct sentinel for "looked up but nothing found" vs "not yet looked up" */
#define CP_FB_NOT_CACHED   ((i16)-2)
#define CP_FB_PRIMARY      ((i16)-1)
#define CP_FB_NOT_FOUND    ((i16)-3)

/* Load font file via mmap — OS manages memory, unused pages stay on disk */
static u8 *mmap_font_file(const char *path, usize *out_size) {
#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    *out_size = (usize)st.st_size;
    u8 *data = (u8 *)mmap(NULL, *out_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return NULL;
    return data;
#else
    /* Windows fallback: malloc+read */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *out_size = (usize)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *data = malloc(*out_size);
    if (!data) { fclose(f); return NULL; }
    fread(data, 1, *out_size, f);
    fclose(f);
    return data;
#endif
}

static void unmap_font_file(u8 *data, usize size) {
#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    if (data) munmap(data, size);
#else
    free(data);
#endif
}

/* Check if a font path is already loaded as a fallback (dedup) */
static bool fallback_path_loaded(const char *path) {
    for (i32 i = 0; i < g_fallback_count; i++) {
        if (g_fallbacks[i].loaded && strcmp(g_fallbacks[i].path, path) == 0)
            return true;
    }
    return false;
}

#ifdef PLATFORM_MACOS
/* Create a CTFontRef from a TTF/OTF file. Needed so ct_rasterize_glyph's
 * manual fallback scan finds our bundled fonts (e.g. Symbols Nerd Font Mono)
 * before the generic CTFontCreateForString() path returns an unrelated system
 * font for Private-Use-Area codepoints. */
static CTFontRef ct_font_from_file(const char *path, f32 pixel_h) {
    CFStringRef path_str = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    if (!path_str) return NULL;
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path_str, kCFURLPOSIXPathStyle, false);
    CFRelease(path_str);
    if (!url) return NULL;
    CGDataProviderRef provider = CGDataProviderCreateWithURL(url);
    CFRelease(url);
    if (!provider) return NULL;
    CGFontRef cg_font = CGFontCreateWithDataProvider(provider);
    CGDataProviderRelease(provider);
    if (!cg_font) return NULL;
    CTFontRef ct = CTFontCreateWithGraphicsFont(cg_font, (CGFloat)pixel_h, NULL, NULL);
    CGFontRelease(cg_font);
    return ct;
}

/* Measure a font's advance for a reference glyph (prefers 'M', falls back to
 * a Nerd Font folder icon if the font is symbols-only). Returns 0 on failure. */
static CGFloat ct_font_probe_advance(CTFontRef f) {
    if (!f) return 0;
    UniChar refs[] = { 'M', 0xF07B };
    for (u32 i = 0; i < sizeof(refs)/sizeof(refs[0]); i++) {
        CGGlyph g = 0;
        if (CTFontGetGlyphsForCharacters(f, &refs[i], &g, 1) && g != 0) {
            CGSize adv;
            CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &g, &adv, 1);
            if (adv.width > 0) return adv.width;
        }
    }
    return 0;
}

/* Load a fallback CTFont, rescaled so its natural advance matches the primary
 * cell width. Nerd Fonts draw icons at 1.0em, but typical mono fonts have an
 * 'M' advance around 0.6em — at the same point size the Nerd Font glyphs would
 * overflow and get clipped to the left portion of the cell. */
static CTFontRef ct_font_from_file_fit(const char *path, f32 pixel_h, CGFloat target_cw) {
    CTFontRef probe = ct_font_from_file(path, pixel_h);
    if (!probe || target_cw <= 0) return probe;

    CGFloat natural_adv = ct_font_probe_advance(probe);
    if (natural_adv <= target_cw * 1.02) return probe;  /* already fits */

    f32 scaled_h = (f32)(pixel_h * (target_cw / natural_adv));
    CFRelease(probe);
    return ct_font_from_file(path, scaled_h);
}
#endif

/* =========================================================================
 * Lazy fallback loading
 *
 * Fallback fonts are expensive to load — each one creates a CoreText CTFontRef
 * with character maps, metrics tables, shaping features (300-800 KB apiece).
 * Most shell sessions never exercise them (ASCII + primary font covers ~99%
 * of input). Instead of loading all 9 fallbacks at startup, register their
 * paths here and defer the actual load until the first codepoint miss.
 *
 * The one exception is SymbolsNerdFontMono — loaded eagerly because the
 * filebrowser shows Nerd-Font PUA icons on every first frame. Everything else
 * is only touched when a Unicode, CJK, emoji, or symbol glyph is actually
 * used.
 * ========================================================================= */
typedef struct {
    char path[512];
    f32  pixel_h;
    bool attempted;   /* load_fallback was called for this path */
} PendingFallback;
#define MAX_PENDING_FALLBACKS 16
static PendingFallback g_pending_fallbacks[MAX_PENDING_FALLBACKS];
static i32 g_pending_fallback_count = 0;

static void queue_lazy_fallback(const char *path, f32 pixel_h) {
    if (!path || !*path) return;
    if (g_pending_fallback_count >= MAX_PENDING_FALLBACKS) return;
    PendingFallback *p = &g_pending_fallbacks[g_pending_fallback_count++];
    snprintf(p->path, sizeof p->path, "%s", path);
    p->pixel_h = pixel_h;
    p->attempted = false;
}

static bool load_fallback(const char *path, f32 pixel_h); /* fwd decl */

/* Drain the lazy queue — load every pending fallback that wasn't tried yet.
 * Called on the first codepoint miss; subsequent calls are near-free. */
static void flush_lazy_fallbacks(void) {
    if (g_pending_fallback_count == 0) return;
    for (i32 i = 0; i < g_pending_fallback_count; i++) {
        if (g_pending_fallbacks[i].attempted) continue;
        g_pending_fallbacks[i].attempted = true;
        load_fallback(g_pending_fallbacks[i].path, g_pending_fallbacks[i].pixel_h);
    }
}

static bool load_fallback(const char *path, f32 pixel_h) {
    if (g_fallback_count >= MAX_FALLBACK_FONTS) return false;
    if (fallback_path_loaded(path)) return false; /* already loaded */

    usize sz = 0;
    u8 *data = mmap_font_file(path, &sz);
    if (!data) return false;

    FallbackFont *fb = &g_fallbacks[g_fallback_count];
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0) offset = 0;
    if (!stbtt_InitFont(&fb->info, data, offset)) { unmap_font_file(data, sz); return false; }
    fb->data = data;
    fb->data_size = sz;
    fb->scale = stbtt_ScaleForPixelHeight(&fb->info, pixel_h);
    fb->loaded = true;
    snprintf(fb->path, sizeof(fb->path), "%s", path);
    g_fallback_count++;

#ifdef PLATFORM_MACOS
    /* Mirror into CoreText fallback chain so ct_rasterize_glyph sees bundled
     * fonts (Nerd Font PUA icons, in particular) before falling through to
     * CTFontCreateForString. */
    if (g_ct_fallback_count < MAX_FALLBACK_FONTS) {
        CGFloat target_cw = ct_font_probe_advance(g_ct_font);
        CTFontRef ct = ct_font_from_file_fit(path, pixel_h, target_cw);
        if (ct) g_ct_fallbacks[g_ct_fallback_count++] = ct;
    }
#endif
    return true;
}

static void free_fallbacks(void) {
    for (i32 i = 0; i < g_fallback_count; i++) {
        unmap_font_file(g_fallbacks[i].data, g_fallbacks[i].data_size);
        g_fallbacks[i].loaded = false;
        g_fallbacks[i].path[0] = '\0';
    }
    g_fallback_count = 0;
    /* Reset the lazy queue so the next atlas-create re-registers with the
     * new pixel_h. Otherwise we'd skip loading them (attempted=true). */
    g_pending_fallback_count = 0;
    memset(g_pending_fallbacks, 0, sizeof(g_pending_fallbacks));
#ifdef PLATFORM_MACOS
    for (i32 i = 0; i < g_ct_fallback_count; i++) {
        if (g_ct_fallbacks[i]) CFRelease(g_ct_fallbacks[i]);
        g_ct_fallbacks[i] = NULL;
    }
    g_ct_fallback_count = 0;
#endif
    cp_fb_cache_clear();
}

#ifdef PLATFORM_MACOS
/* Discover and load a system fallback font for a given codepoint via CoreText.
 * Returns the fallback index in g_fallbacks[], or -1 if not found.
 * This dynamically expands the stb_truetype fallback chain based on what
 * the OS reports as the correct font for a codepoint. */
static i32 discover_system_fallback_for_cp(u32 cp) {
    if (!g_ct_font || g_pixel_h <= 0) return -1;

    /* Encode codepoint as UTF-16 */
    UniChar uchars[2];
    i32 ulen;
    if (cp > 0xFFFF) {
        uchars[0] = (UniChar)(((cp - 0x10000) >> 10) + 0xD800);
        uchars[1] = (UniChar)(((cp - 0x10000) & 0x3FF) + 0xDC00);
        ulen = 2;
    } else {
        uchars[0] = (UniChar)cp;
        ulen = 1;
    }

    CFStringRef str = CFStringCreateWithCharacters(NULL, uchars, ulen);
    if (!str) return -1;

    CTFontRef fallback_ct = CTFontCreateForString(g_ct_font, str, CFRangeMake(0, ulen));
    CFRelease(str);
    if (!fallback_ct) return -1;

    /* Get the file URL of the discovered font */
    CTFontDescriptorRef desc = CTFontCopyFontDescriptor(fallback_ct);
    CFRelease(fallback_ct);
    if (!desc) return -1;

    CFURLRef font_url = CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute);
    CFRelease(desc);
    if (!font_url) return -1;

    char font_path[512];
    if (!CFURLGetFileSystemRepresentation(font_url, true, (UInt8 *)font_path, sizeof(font_path))) {
        CFRelease(font_url);
        return -1;
    }
    CFRelease(font_url);

    /* Check if this font is already loaded */
    for (i32 i = 0; i < g_fallback_count; i++) {
        if (g_fallbacks[i].loaded && strcmp(g_fallbacks[i].path, font_path) == 0)
            return i;
    }

    /* Try to load as a new fallback */
    i32 new_idx = g_fallback_count;
    if (load_fallback(font_path, g_pixel_h)) {
        fprintf(stderr, "font: auto-discovered fallback '%s' for U+%04X\n", font_path, cp);
        return new_idx;
    }
    return -1;
}
#endif

/* Thread ID for main thread — used to restrict dynamic font discovery
 * to the main thread only (CoreText + load_fallback are not thread-safe). */
static pthread_t g_main_thread;

/* Find which font has the glyph — primary first, then fallbacks.
 * Uses the codepoint-to-fallback cache to skip scanning. On macOS, if
 * no loaded fallback has the glyph, asks CoreText to discover the right
 * system font and dynamically loads it (main thread only).
 *
 * The body runs with g_fb_mu held (see the locking wrapper below) because it
 * reads g_fallbacks / g_cp_fb_cache (shared with the raster worker) and, on the
 * main thread, mutates them via flush_lazy_fallbacks / discover. */
static stbtt_fontinfo *find_font_for_glyph_locked(u32 cp, f32 *out_scale) {
    /* Check primary font */
    i32 idx = stbtt_FindGlyphIndex(&g_font_info, (int)cp);
    if (idx != 0) {
        *out_scale = g_font_scale;
        return &g_font_info;
    }

    /* Check the codepoint-to-fallback cache */
    i16 cached_fb = cp_fb_cache_lookup(cp);
    if (cached_fb == CP_FB_PRIMARY) {
        *out_scale = g_font_scale;
        return &g_font_info;
    }
    if (cached_fb == CP_FB_NOT_FOUND) {
        return NULL;
    }
    if (cached_fb >= 0 && cached_fb < g_fallback_count && g_fallbacks[cached_fb].loaded) {
        /* Verify the cached result is still valid */
        idx = stbtt_FindGlyphIndex(&g_fallbacks[cached_fb].info, (int)cp);
        if (idx != 0) {
            *out_scale = g_fallbacks[cached_fb].scale;
            return &g_fallbacks[cached_fb].info;
        }
    }

    /* Linear scan of all loaded fallbacks */
    for (i32 i = 0; i < g_fallback_count; i++) {
        if (!g_fallbacks[i].loaded) continue;
        idx = stbtt_FindGlyphIndex(&g_fallbacks[i].info, (int)cp);
        if (idx != 0) {
            *out_scale = g_fallbacks[i].scale;
            cp_fb_cache_insert(cp, (i16)i);
            return &g_fallbacks[i].info;
        }
    }

#ifdef PLATFORM_MACOS
    /* Nothing in the primary or the already-loaded fallbacks? Drain the lazy
     * queue on the main thread; re-scan once the new fonts are loaded. */
    if (g_pending_fallback_count > 0 && pthread_equal(pthread_self(), g_main_thread)) {
        i32 before = g_fallback_count;
        flush_lazy_fallbacks();
        for (i32 i = before; i < g_fallback_count; i++) {
            if (!g_fallbacks[i].loaded) continue;
            idx = stbtt_FindGlyphIndex(&g_fallbacks[i].info, (int)cp);
            if (idx != 0) {
                *out_scale = g_fallbacks[i].scale;
                cp_fb_cache_insert(cp, (i16)i);
                return &g_fallbacks[i].info;
            }
        }
    }
#endif

#ifdef PLATFORM_MACOS
    /* System font discovery: ask CoreText to find a font, load it, try again.
     * Only safe on the main thread — load_fallback and g_fallback_count are
     * not protected by a mutex. Worker threads use the CoreText rasterization
     * path (ct_rasterize_glyph) which has its own font discovery. */
    if (pthread_equal(pthread_self(), g_main_thread)) {
        i32 discovered = discover_system_fallback_for_cp(cp);
        if (discovered >= 0 && discovered < g_fallback_count) {
            idx = stbtt_FindGlyphIndex(&g_fallbacks[discovered].info, (int)cp);
            if (idx != 0) {
                *out_scale = g_fallbacks[discovered].scale;
                cp_fb_cache_insert(cp, (i16)discovered);
                return &g_fallbacks[discovered].info;
            }
        }
    }
#endif

    /* Nothing found — cache the negative result to avoid future lookups */
    cp_fb_cache_insert(cp, CP_FB_NOT_FOUND);
    return NULL;
}

/* Locking wrapper: serializes fallback-chain reads/mutations and the
 * codepoint cache between the render thread and the raster worker. */
static stbtt_fontinfo *find_font_for_glyph(u32 cp, f32 *out_scale) {
    pthread_mutex_lock(&g_fb_mu);
    stbtt_fontinfo *r = find_font_for_glyph_locked(cp, out_scale);
    pthread_mutex_unlock(&g_fb_mu);
    return r;
}

/* Atlas packing state */
static i32 g_atlas_x = 0;    /* next free x in atlas */
static i32 g_atlas_y = 0;    /* current row y */
static i32 g_atlas_row_h = 0; /* tallest glyph in current row */
__attribute__((unused)) static u32 g_atlas_tex = 0;

static bool font_ensure_atlas_room(FontAtlas *atlas, i32 w, i32 h);

/* =========================================================================
 * Async glyph rasterization worker
 *
 * Cache miss on a non-trivial glyph used to stall the render thread for
 * 1–10 ms while stb_truetype / CoreText rasterized it.  Now a background
 * pthread drains a request ring, produces an alpha bitmap off-thread, and
 * hands it back to the main thread.  The main thread packs it into the
 * atlas + uploads + inserts into the glyph cache at the top of the next
 * frame (GPU APIs are main-thread-only on macOS Metal).
 *
 * Scope: only monochrome alpha glyphs (mono atlas path) are async.  Color
 * emoji and box drawing stay synchronous — they're fast and have separate
 * storage.
 * ========================================================================= */

#define RASTER_REQ_CAP  512
#define RASTER_DONE_CAP 512
#define PENDING_SET_CAP 2048

typedef struct {
    u32  cp;
    u8  *bitmap;     /* malloc'd, owned by main thread after dequeue */
    i32  w, h;
    bool ok;
} RasterCompletion;

static struct {
    pthread_t       thread;
    pthread_mutex_t req_mu;
    pthread_cond_t  req_cv;
    pthread_mutex_t done_mu;
    pthread_cond_t  done_cv;   /* signalled when the main thread drains done_ring */
    u32             req_ring[RASTER_REQ_CAP];
    i32             req_head, req_tail;   /* ring indices */
    RasterCompletion done_ring[RASTER_DONE_CAP];
    i32             done_head, done_tail;   /* ring indices (under done_mu) */
    /* main-thread-only set to dedup in-flight requests. 0 = empty slot. */
    u32             pending_set[PENDING_SET_CAP];
    i32             pending_count;
    FontAtlas      *atlas;
    bool            started;
    bool            stop;
    u32             completed_version; /* bumped when new glyphs land */
} g_raster = { .req_mu = PTHREAD_MUTEX_INITIALIZER,
               .req_cv = PTHREAD_COND_INITIALIZER,
               .done_mu = PTHREAD_MUTEX_INITIALIZER,
               .done_cv = PTHREAD_COND_INITIALIZER };

/* Main-thread-only helpers for the pending set */
static bool pending_contains(u32 cp) {
    u32 h = (cp * 2654435761u) % PENDING_SET_CAP;
    for (i32 i = 0; i < 32; i++) {
        u32 slot = (h + i) % PENDING_SET_CAP;
        if (g_raster.pending_set[slot] == cp) return true;
        if (g_raster.pending_set[slot] == 0)  return false;
    }
    return false;
}
static void pending_add(u32 cp) {
    if (g_raster.pending_count >= PENDING_SET_CAP - 32) return;
    u32 h = (cp * 2654435761u) % PENDING_SET_CAP;
    for (i32 i = 0; i < 32; i++) {
        u32 slot = (h + i) % PENDING_SET_CAP;
        if (g_raster.pending_set[slot] == 0) {
            g_raster.pending_set[slot] = cp;
            g_raster.pending_count++;
            return;
        }
        if (g_raster.pending_set[slot] == cp) return;
    }
}
static void pending_remove(u32 cp) {
    u32 h = (cp * 2654435761u) % PENDING_SET_CAP;
    for (i32 i = 0; i < 32; i++) {
        u32 slot = (h + i) % PENDING_SET_CAP;
        if (g_raster.pending_set[slot] == cp) {
            g_raster.pending_set[slot] = 0;
            g_raster.pending_count--;
            return;
        }
        if (g_raster.pending_set[slot] == 0) return;
    }
}

/* Forward declarations for functions defined later in the file */
static bool render_box_drawing(u8 *buf, i32 cw, i32 ch, u32 cp);
#ifdef PLATFORM_MACOS
static bool ct_rasterize_glyph(u8 *buf, i32 cw, i32 ch, u32 cp, f32 ascent_px);
#endif

/* Produce an alpha bitmap for a single codepoint. Caller provides a
 * zeroed buffer of size cw*ch. Returns true if something was drawn.
 *
 * IMPORTANT: this function is worker-thread-safe for stb_truetype and box
 * drawing paths. The CoreText path uses a mutex around the shared glyph
 * context pool. */
static pthread_mutex_t g_ct_mu = PTHREAD_MUTEX_INITIALIZER;

/* Produce a single-channel R8 alpha bitmap for a codepoint. Caller provides a
 * zeroed buffer of size cw*ch. Returns true if something was drawn. */
static bool rasterize_alpha_bitmap(u32 cp, u8 *buf, i32 cw, i32 ch) {
    /* Box drawing / block elements — render directly into the R8 buffer. */
    if ((cp >= 0x2500 && cp <= 0x259F)) {
        if (render_box_drawing(buf, cw, ch, cp)) return true;
    }

#ifdef PLATFORM_MACOS
    /* Core Text writes R8 alpha coverage directly into buf. Serialize on the
     * shared pooled bitmap context. */
    pthread_mutex_lock(&g_ct_mu);
    bool ok = ct_rasterize_glyph(buf, cw, ch, cp, g_font_ascent);
    pthread_mutex_unlock(&g_ct_mu);
    if (ok) return true;
#endif

    /* stb_truetype fallback — rasterize straight into buf. */
    f32 scale;
    stbtt_fontinfo *font = find_font_for_glyph(cp, &scale);
    if (!font) return false;

    i32 x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(font, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
    i32 gw = x1 - x0, gh = y1 - y0;
    if (gw > 0 && gh > 0) {
        i32 advance_w, lsb;
        stbtt_GetCodepointHMetrics(font, (int)cp, &advance_w, &lsb);
        i32 bx = (i32)floorf(lsb * scale);
        i32 by = (i32)g_font_ascent + y0;
        if (bx < 0) bx = 0;
        if (by < 0) by = 0;
        i32 rw = gw, rh = gh;
        if (bx + rw > cw) rw = cw - bx;
        if (by + rh > ch) rh = ch - by;
        if (rw > 0 && rh > 0) {
            stbtt_MakeCodepointBitmap(font, buf + by * cw + bx,
                                       rw, rh, cw, scale, scale, (int)cp);
        }
    }
    return true;
}

static void *raster_worker_main(void *arg) {
    FontAtlas *atlas = (FontAtlas *)arg;
    for (;;) {
        pthread_mutex_lock(&g_raster.req_mu);
        while (g_raster.req_head == g_raster.req_tail && !g_raster.stop)
            pthread_cond_wait(&g_raster.req_cv, &g_raster.req_mu);
        if (g_raster.stop) { pthread_mutex_unlock(&g_raster.req_mu); break; }
        u32 cp = g_raster.req_ring[g_raster.req_head];
        g_raster.req_head = (g_raster.req_head + 1) % RASTER_REQ_CAP;
        pthread_mutex_unlock(&g_raster.req_mu);

        i32 cw = (i32)atlas->cell_width;
        i32 ch = (i32)atlas->cell_height;
        if (cw <= 0 || ch <= 0) continue;
        u8 *bmp = calloc(1, (usize)(cw * ch) * ATLAS_BPP);
        if (!bmp) continue;
        bool ok = rasterize_alpha_bitmap(cp, bmp, cw, ch);

        pthread_mutex_lock(&g_raster.done_mu);
        i32 dnext = (g_raster.done_tail + 1) % RASTER_DONE_CAP;
        /* If the completion ring is full, wait for the main thread to drain it
         * instead of dropping the result. Dropping stranded the codepoint in
         * the main thread's pending_set forever (it's never re-queued), so the
         * glyph would never render. The bitmap is already rasterized; just hold
         * it until there is room. */
        while (dnext == g_raster.done_head && !g_raster.stop) {
            pthread_cond_wait(&g_raster.done_cv, &g_raster.done_mu);
            dnext = (g_raster.done_tail + 1) % RASTER_DONE_CAP;
        }
        if (g_raster.stop) {
            free(bmp);
            pthread_mutex_unlock(&g_raster.done_mu);
            break;
        }
        RasterCompletion *rc = &g_raster.done_ring[g_raster.done_tail];
        rc->cp = cp;
        rc->bitmap = bmp;
        rc->w = cw;
        rc->h = ch;
        rc->ok = ok;
        g_raster.done_tail = dnext;
        pthread_mutex_unlock(&g_raster.done_mu);
    }
    return NULL;
}

void font_start_async_raster(FontAtlas *atlas) {
    if (g_raster.started) return;
    g_raster.atlas = atlas;
    g_raster.stop  = false;
    g_raster.req_head = g_raster.req_tail = 0;
    g_raster.done_head = g_raster.done_tail = 0;
    g_raster.pending_count = 0;
    memset(g_raster.pending_set, 0, sizeof(g_raster.pending_set));
    /* Raster worker handles small glyph rasterization requests — doesn't need
     * the default 8 MB pthread stack. Cap at 512 KB to save virtual VM range
     * and keep the stack hot in cache. */
    pthread_attr_t attr;
    pthread_attr_t *pattr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setstacksize(&attr, 512 * 1024);
        pattr = &attr;
    }
    if (pthread_create(&g_raster.thread, pattr, raster_worker_main, atlas) == 0) {
        g_raster.started = true;
    }
    if (pattr) pthread_attr_destroy(pattr);
}

void font_stop_async_raster(void) {
    if (!g_raster.started) return;
    pthread_mutex_lock(&g_raster.req_mu);
    g_raster.stop = true;
    pthread_cond_broadcast(&g_raster.req_cv);
    pthread_mutex_unlock(&g_raster.req_mu);
    /* Also wake a worker blocked waiting for done-ring space so it can observe
     * stop and exit — otherwise pthread_join below would hang. */
    pthread_mutex_lock(&g_raster.done_mu);
    pthread_cond_broadcast(&g_raster.done_cv);
    pthread_mutex_unlock(&g_raster.done_mu);
    pthread_join(g_raster.thread, NULL);
    /* Drain any leftover completions to avoid bitmap leaks */
    pthread_mutex_lock(&g_raster.done_mu);
    for (i32 i = g_raster.done_head; i != g_raster.done_tail; i = (i + 1) % RASTER_DONE_CAP)
        free(g_raster.done_ring[i].bitmap);
    g_raster.done_head = g_raster.done_tail = 0;
    pthread_mutex_unlock(&g_raster.done_mu);
    g_raster.started = false;
}

/* Enqueue a codepoint for async rasterization. Main thread only. */
static void raster_enqueue(u32 cp) {
    if (!g_raster.started) return;
    if (pending_contains(cp)) return;

    pthread_mutex_lock(&g_raster.req_mu);
    i32 next = (g_raster.req_tail + 1) % RASTER_REQ_CAP;
    if (next == g_raster.req_head) {
        /* Request ring full — drop this request; it'll be retried next frame. */
        pthread_mutex_unlock(&g_raster.req_mu);
        return;
    }
    g_raster.req_ring[g_raster.req_tail] = cp;
    g_raster.req_tail = next;
    pthread_cond_signal(&g_raster.req_cv);
    pthread_mutex_unlock(&g_raster.req_mu);

    pending_add(cp);
}

/* Defined below — forward declare for drain. */
static CachedGlyph *cache_insert(u32 cp, f32 u0, f32 v0, f32 u1, f32 v1);
static void cache_insert_exhausted(FontAtlas *atlas, u32 cp);  /* atlas-full sentinel */

/* Drain completed rasterizations: pack into atlas, upload, insert into cache.
 * Called from the main thread at frame start. Returns the number of newly
 * installed glyphs — callers use this to invalidate downstream row caches. */
u32 font_drain_raster_completions(FontAtlas *atlas) {
    /* Tick the LRU clock once per frame. Called from the main thread at frame
     * start regardless of whether any glyphs are pending. */
    g_glyph_frame++;

    if (!g_raster.started) return 0;

    RasterCompletion batch[16];
    i32 take = 0;
    pthread_mutex_lock(&g_raster.done_mu);
    while (take < 16 && g_raster.done_head != g_raster.done_tail) {
        batch[take++] = g_raster.done_ring[g_raster.done_head];
        g_raster.done_head = (g_raster.done_head + 1) % RASTER_DONE_CAP;
    }
    /* Wake the worker if it was blocked waiting for room in the done ring. */
    if (take > 0) pthread_cond_broadcast(&g_raster.done_cv);
    pthread_mutex_unlock(&g_raster.done_mu);

    u32 installed = 0;
    for (i32 i = 0; i < take; i++) {
        RasterCompletion *rc = &batch[i];
        pending_remove(rc->cp);
        if (!rc->ok || !rc->bitmap) { free(rc->bitmap); continue; }

        /* Atlas packing — main-thread-only state */
        i32 cw = rc->w, ch = rc->h;
        if (!font_ensure_atlas_room(atlas, cw, ch)) {
            free(rc->bitmap); /* atlas full */
            /* Record an atlas-exhausted sentinel so font_get_glyph_uv stops
             * re-enqueuing this codepoint to the raster worker every frame.
             * Without it, cache_lookup keeps missing, raster_enqueue keeps
             * firing, and the worker keeps rasterising a glyph we can never
             * pack — a sustained background-CPU cliff. */
            cache_insert_exhausted(atlas, rc->cp);
            continue;
        }
        i32 ax = g_atlas_x + 1, ay = g_atlas_y + 1;

#ifdef USE_METAL
        if (atlas->metal_texture)
            renderer_metal_upload_texture(atlas->metal_texture, ax, ay, cw, ch, rc->bitmap);
        if (atlas->atlas_bitmap) {
            for (i32 row = 0; row < ch; row++)
                memcpy(atlas->atlas_bitmap + ((ay + row) * atlas->atlas_width + ax) * ATLAS_BPP,
                       rc->bitmap + row * cw * ATLAS_BPP, (usize)(cw * ATLAS_BPP));
        }
#else
        glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, cw, ch, GL_RED, GL_UNSIGNED_BYTE, rc->bitmap);
#endif

        f32 u0 = (f32)ax / atlas->atlas_width;
        f32 v0 = (f32)ay / atlas->atlas_height;
        f32 u1 = (f32)(ax + cw) / atlas->atlas_width;
        f32 v1 = (f32)(ay + ch) / atlas->atlas_height;
        g_atlas_x += cw + 2;
        if (ch + 2 > g_atlas_row_h) g_atlas_row_h = ch + 2;

        cache_insert(rc->cp, u0, v0, u1, v1);
        free(rc->bitmap);
        installed++;
    }

    if (installed > 0) g_raster.completed_version++;
    return installed;
}

u32 font_raster_completion_version(void) { return g_raster.completed_version; }

static u32 glyph_hash(u32 cp) {
    return (cp * 2654435761u) % GLYPH_CACHE_SIZE;
}

static CachedGlyph *cache_lookup(u32 cp) {
    u32 idx = glyph_hash(cp);
    for (u32 i = 0; i < 32; i++) { /* linear probe — longer chain for multi-script */
        u32 slot = (idx + i) % GLYPH_CACHE_SIZE;
        if (g_cache[slot].valid && g_cache[slot].codepoint == cp) {
            g_cache[slot].last_used_frame = g_glyph_frame;
            return &g_cache[slot];
        }
        if (!g_cache[slot].valid)
            return NULL;
    }
    return NULL;
}

/* Eviction: when the table is hot we evict the coldest ~20% of entries. We
 * cannot leave holes mid-chain (lookup stops on the first empty slot), so we
 * snapshot survivors, zero the table, and reinsert — full rehash. This is
 * O(N) but only fires once per saturation event, and N is just 2048. */
#define GLYPH_CACHE_HIGH_WATER (GLYPH_CACHE_SIZE * 4 / 5)   /* 1638 / 80% */
#define GLYPH_CACHE_EVICT_FRAC 5                            /* drop 1/5th */

static void cache_evict_cold(void) {
    /* Single-threaded (main thread only — invoked from cache_insert via the
     * frame-drain path). Static scratch buffers avoid ~80 KiB of stack. */
    static u64 stamps[GLYPH_CACHE_SIZE];
    static CachedGlyph survivors[GLYPH_CACHE_SIZE];

    u32 n = 0;
    for (u32 i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (g_cache[i].valid) stamps[n++] = g_cache[i].last_used_frame;
    }
    if (n == 0) { g_cache_used = 0; return; }

    /* Shell sort to find the kth smallest last_used_frame. n is bounded at
     * 2048 and this fires only at saturation, so cost is negligible. No
     * stdlib qsort dep — keeps the hot file lean. */
    for (u32 gap = n / 2; gap > 0; gap /= 2) {
        for (u32 i = gap; i < n; i++) {
            u64 t = stamps[i];
            u32 j = i;
            while (j >= gap && stamps[j - gap] > t) {
                stamps[j] = stamps[j - gap];
                j -= gap;
            }
            stamps[j] = t;
        }
    }
    u32 k = n / GLYPH_CACHE_EVICT_FRAC;
    if (k == 0) k = 1;
    if (k >= n) k = n - 1;
    u64 cutoff = stamps[k]; /* entries with last_used_frame <= cutoff are cold */

    /* Snapshot survivors, then rehash. Snapshot must come first because we're
     * about to wipe the table. */
    u32 survivor_count = 0;
    u32 evicted = 0;
    for (u32 i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (!g_cache[i].valid) continue;
        if (g_cache[i].last_used_frame <= cutoff && evicted < k) {
            evicted++;
            continue;
        }
        survivors[survivor_count++] = g_cache[i];
    }

    memset(g_cache, 0, sizeof(g_cache));
    g_cache_used = 0;

    /* Reinsert survivors via the standard probe path so lookup invariants
     * hold. */
    for (u32 s = 0; s < survivor_count; s++) {
        u32 idx = glyph_hash(survivors[s].codepoint);
        for (u32 i = 0; i < 32; i++) {
            u32 slot = (idx + i) % GLYPH_CACHE_SIZE;
            if (!g_cache[slot].valid) {
                g_cache[slot] = survivors[s];
                g_cache_used++;
                break;
            }
        }
        /* If a survivor can't find a slot within the probe window we drop it;
         * that's acceptable — it'll be re-rasterized on demand. */
    }
}

/* Shared 32-slot linear probe for both insert paths. Returns the slot to
 * write: the slot already holding `cp` (caller updates in place — g_cache_used
 * must NOT grow), else the first invalid slot in the window (fresh insert —
 * caller bumps g_cache_used), else — window full, all 32 slots valid with
 * other codepoints — the first slot of the chain to overwrite (one valid
 * entry replaces another; g_cache_used unchanged). Callers distinguish the
 * three cases by inspecting g_cache[slot] before writing. */
static u32 cache_probe_slot(u32 idx, u32 cp) {
    for (u32 i = 0; i < 32; i++) {
        u32 slot = (idx + i) % GLYPH_CACHE_SIZE;
        if (g_cache[slot].valid && g_cache[slot].codepoint == cp) return slot;
        if (!g_cache[slot].valid) return slot;
    }
    return idx % GLYPH_CACHE_SIZE;
}

/* Insert a negative/"atlas-exhausted" sentinel for `cp`: the codepoint could
 * not be packed (atlas hit ATLAS_MAX_SIZE). We cache an entry whose UVs point
 * at the space glyph and whose `exhausted` flag is set, so font_get_glyph_uv
 * returns the space placeholder on a cache HIT instead of re-running the full
 * rasteriser every single frame. The entry participates in normal LRU/eviction
 * and is wiped on font reload, so if the atlas is later reset/regrown the
 * codepoint gets a fresh real attempt. */
static void cache_insert_exhausted(FontAtlas *atlas, u32 cp) {
    if (g_cache_used >= GLYPH_CACHE_HIGH_WATER) cache_evict_cold();
    f32 su0 = atlas->glyphs[32].u0, sv0 = atlas->glyphs[32].v0;
    f32 su1 = atlas->glyphs[32].u1, sv1 = atlas->glyphs[32].v1;
    u32 slot = cache_probe_slot(glyph_hash(cp), cp);
    if (g_cache[slot].valid && g_cache[slot].codepoint == cp) {
        /* Already present (real or sentinel) — mark/refresh in place. */
        g_cache[slot].exhausted = true;
        g_cache[slot].u0 = su0; g_cache[slot].v0 = sv0;
        g_cache[slot].u1 = su1; g_cache[slot].v1 = sv1;
        g_cache[slot].last_used_frame = g_glyph_frame;
        return;
    }
    /* Fresh insert into an empty slot grows the live count; the window-full
     * fallback overwrites a valid entry (same policy as cache_insert) and
     * leaves g_cache_used unchanged. */
    bool was_empty = !g_cache[slot].valid;
    g_cache[slot] = (CachedGlyph){ .codepoint = cp, .u0 = su0, .v0 = sv0,
                                    .u1 = su1, .v1 = sv1,
                                    .last_used_frame = g_glyph_frame,
                                    .valid = true, .exhausted = true };
    if (was_empty) g_cache_used++;
}

static CachedGlyph *cache_insert(u32 cp, f32 u0, f32 v0, f32 u1, f32 v1) {
    if (g_cache_used >= GLYPH_CACHE_HIGH_WATER) cache_evict_cold();

    u32 slot = cache_probe_slot(glyph_hash(cp), cp);
    /* Only a fresh insert into an empty slot grows the live count. The other
     * two probe outcomes replace like-for-like: updating an existing entry in
     * place (e.g. an exhausted sentinel being upgraded to a real glyph, or a
     * redundant insert), or — probe window exhausted, rare — overwriting the
     * first slot of the chain, which is guaranteed valid there (all 32 probed
     * slots were valid, else the probe would have returned an empty one).
     * Incrementing g_cache_used in either case would over-count a single
     * codepoint across two valid slots and trip cache_evict_cold prematurely. */
    bool was_empty = !g_cache[slot].valid;
    g_cache[slot] = (CachedGlyph){ .codepoint = cp, .u0 = u0, .v0 = v0,
                                    .u1 = u1, .v1 = v1,
                                    .last_used_frame = g_glyph_frame,
                                    .valid = true };
    if (was_empty) g_cache_used++;
    return &g_cache[slot];
}

/* =========================================================================
 * Pixel-perfect box drawing / block element rendering
 * These are drawn programmatically, not from font glyphs, for exact cell fit.
 * ========================================================================= */

static void draw_hline(u8 *buf, i32 cw, i32 ch, i32 y, i32 x0, i32 x1, i32 thick) {
    if (y < 0) y = 0;
    for (i32 t = 0; t < thick && y + t < ch; t++)
        for (i32 x = (x0 < 0 ? 0 : x0); x < x1 && x < cw; x++)
            buf[(y + t) * cw + x] = 255;
}

static void draw_vline(u8 *buf, i32 cw, i32 ch, i32 x, i32 y0, i32 y1, i32 thick) {
    if (x < 0) x = 0;
    for (i32 t = 0; t < thick && x + t < cw; t++)
        for (i32 y = (y0 < 0 ? 0 : y0); y < y1 && y < ch; y++)
            buf[y * cw + (x + t)] = 255;
}

static bool render_box_drawing(u8 *buf, i32 cw, i32 ch, u32 cp) {
    i32 mx = cw / 2, my = ch / 2;
    /* Minimum 2px thick on retina for visibility */
    i32 thin = 2;
    if (cw <= 8) thin = 1;
    i32 thick = thin + 2;

    memset(buf, 0, (usize)(cw * ch));

    if (cp >= 0x2580 && cp <= 0x259F) {
        /* Block elements */
        switch (cp) {
        case 0x2580: /* upper half */ for (i32 y=0;y<ch/2;y++) memset(buf+y*cw,255,(usize)cw); return true;
        case 0x2584: /* lower half */ for (i32 y=ch/2;y<ch;y++) memset(buf+y*cw,255,(usize)cw); return true;
        case 0x2588: /* full block */ memset(buf,255,(usize)(cw*ch)); return true;
        case 0x258C: /* left half */  for (i32 y=0;y<ch;y++) memset(buf+y*cw,255,(usize)(cw/2)); return true;
        case 0x2590: /* right half */ for (i32 y=0;y<ch;y++) memset(buf+y*cw+cw/2,255,(usize)(cw-cw/2)); return true;
        case 0x2591: /* light shade */
            for (i32 y=0;y<ch;y++) for (i32 x=0;x<cw;x++) if ((x+y)%4==0) buf[y*cw+x]=255; return true;
        case 0x2592: /* medium shade */
            for (i32 y=0;y<ch;y++) for (i32 x=0;x<cw;x++) if ((x+y)%2==0) buf[y*cw+x]=255; return true;
        case 0x2593: /* dark shade */
            for (i32 y=0;y<ch;y++) for (i32 x=0;x<cw;x++) if ((x+y)%4!=0) buf[y*cw+x]=255; return true;
        default: break;
        }
        /* Fractional blocks */
        if (cp >= 0x2581 && cp <= 0x2587) {
            i32 eighths = (i32)(cp - 0x2580);
            i32 h = ch * eighths / 8;
            for (i32 y = ch - h; y < ch; y++) memset(buf + y * cw, 255, (usize)cw);
            return true;
        }
        if (cp >= 0x2589 && cp <= 0x258F) {
            i32 eighths = 8 - (i32)(cp - 0x2588);
            i32 w = cw * eighths / 8;
            for (i32 y = 0; y < ch; y++) memset(buf + y * cw, 255, (usize)w);
            return true;
        }
        return false;
    }

    if (cp < 0x2500 || cp > 0x257F) return false;

    /* Box drawing characters — decoded by line segments */
    /* Encoding: each char has up/down/left/right segments, thin or thick */
    /* We decode the most common ones explicitly */

    switch (cp) {
    /* Light lines */
    case 0x2500: draw_hline(buf,cw,ch,my,0,cw,thin); return true;              /* ─ */
    case 0x2502: draw_vline(buf,cw,ch,mx,0,ch,thin); return true;              /* │ */
    case 0x250C: draw_hline(buf,cw,ch,my,mx,cw,thin); draw_vline(buf,cw,ch,mx,my,ch,thin); return true; /* ┌ */
    case 0x2510: draw_hline(buf,cw,ch,my,0,mx+thin,thin); draw_vline(buf,cw,ch,mx,my,ch,thin); return true; /* ┐ */
    case 0x2514: draw_hline(buf,cw,ch,my,mx,cw,thin); draw_vline(buf,cw,ch,mx,0,my+thin,thin); return true; /* └ */
    case 0x2518: draw_hline(buf,cw,ch,my,0,mx+thin,thin); draw_vline(buf,cw,ch,mx,0,my+thin,thin); return true; /* ┘ */
    case 0x251C: draw_hline(buf,cw,ch,my,mx,cw,thin); draw_vline(buf,cw,ch,mx,0,ch,thin); return true; /* ├ */
    case 0x2524: draw_hline(buf,cw,ch,my,0,mx+thin,thin); draw_vline(buf,cw,ch,mx,0,ch,thin); return true; /* ┤ */
    case 0x252C: draw_hline(buf,cw,ch,my,0,cw,thin); draw_vline(buf,cw,ch,mx,my,ch,thin); return true; /* ┬ */
    case 0x2534: draw_hline(buf,cw,ch,my,0,cw,thin); draw_vline(buf,cw,ch,mx,0,my+thin,thin); return true; /* ┴ */
    case 0x253C: draw_hline(buf,cw,ch,my,0,cw,thin); draw_vline(buf,cw,ch,mx,0,ch,thin); return true; /* ┼ */

    /* Heavy/bold lines */
    case 0x2501: draw_hline(buf,cw,ch,my-thick/2,0,cw,thick); return true;     /* ━ */
    case 0x2503: draw_vline(buf,cw,ch,mx-thick/2,0,ch,thick); return true;     /* ┃ */
    case 0x250F: draw_hline(buf,cw,ch,my-thick/2,mx,cw,thick); draw_vline(buf,cw,ch,mx-thick/2,my,ch,thick); return true;
    case 0x2513: draw_hline(buf,cw,ch,my-thick/2,0,mx+thick,thick); draw_vline(buf,cw,ch,mx-thick/2,my,ch,thick); return true;
    case 0x2517: draw_hline(buf,cw,ch,my-thick/2,mx,cw,thick); draw_vline(buf,cw,ch,mx-thick/2,0,my+thick,thick); return true;
    case 0x251B: draw_hline(buf,cw,ch,my-thick/2,0,mx+thick,thick); draw_vline(buf,cw,ch,mx-thick/2,0,my+thick,thick); return true;
    case 0x2523: draw_hline(buf,cw,ch,my-thick/2,mx,cw,thick); draw_vline(buf,cw,ch,mx-thick/2,0,ch,thick); return true;
    case 0x252B: draw_hline(buf,cw,ch,my-thick/2,0,mx+thick,thick); draw_vline(buf,cw,ch,mx-thick/2,0,ch,thick); return true;
    case 0x2533: draw_hline(buf,cw,ch,my-thick/2,0,cw,thick); draw_vline(buf,cw,ch,mx-thick/2,my,ch,thick); return true;
    case 0x253B: draw_hline(buf,cw,ch,my-thick/2,0,cw,thick); draw_vline(buf,cw,ch,mx-thick/2,0,my+thick,thick); return true;
    case 0x254B: draw_hline(buf,cw,ch,my-thick/2,0,cw,thick); draw_vline(buf,cw,ch,mx-thick/2,0,ch,thick); return true;

    /* Double lines */
    case 0x2550: draw_hline(buf,cw,ch,my-2,0,cw,thin); draw_hline(buf,cw,ch,my+2,0,cw,thin); return true; /* ═ */
    case 0x2551: draw_vline(buf,cw,ch,mx-2,0,ch,thin); draw_vline(buf,cw,ch,mx+2,0,ch,thin); return true; /* ║ */
    case 0x2554: /* ╔ */ draw_hline(buf,cw,ch,my-2,mx,cw,thin); draw_hline(buf,cw,ch,my+2,mx+2,cw,thin);
                         draw_vline(buf,cw,ch,mx-2,my,ch,thin); draw_vline(buf,cw,ch,mx+2,my+2,ch,thin); return true;
    case 0x2557: /* ╗ */ draw_hline(buf,cw,ch,my-2,0,mx+thin,thin); draw_hline(buf,cw,ch,my+2,0,mx-2,thin);
                         draw_vline(buf,cw,ch,mx+2,my,ch,thin); draw_vline(buf,cw,ch,mx-2,my+2,ch,thin); return true;
    case 0x255A: /* ╚ */ draw_hline(buf,cw,ch,my-2,mx+2,cw,thin); draw_hline(buf,cw,ch,my+2,mx,cw,thin);
                         draw_vline(buf,cw,ch,mx-2,0,my+thin,thin); draw_vline(buf,cw,ch,mx+2,0,my-2,thin); return true;
    case 0x255D: /* ╝ */ draw_hline(buf,cw,ch,my-2,0,mx-2,thin); draw_hline(buf,cw,ch,my+2,0,mx+thin,thin);
                         draw_vline(buf,cw,ch,mx+2,0,my-2,thin); draw_vline(buf,cw,ch,mx-2,0,my+thin,thin); return true;
    case 0x2560: /* ╠ */ draw_hline(buf,cw,ch,my-2,mx+2,cw,thin); draw_hline(buf,cw,ch,my+2,mx+2,cw,thin);
                         draw_vline(buf,cw,ch,mx-2,0,ch,thin); draw_vline(buf,cw,ch,mx+2,0,my-2,thin); draw_vline(buf,cw,ch,mx+2,my+2,ch,thin); return true;
    case 0x2563: /* ╣ */ draw_hline(buf,cw,ch,my-2,0,mx-2,thin); draw_hline(buf,cw,ch,my+2,0,mx-2,thin);
                         draw_vline(buf,cw,ch,mx+2,0,ch,thin); draw_vline(buf,cw,ch,mx-2,0,my-2,thin); draw_vline(buf,cw,ch,mx-2,my+2,ch,thin); return true;
    case 0x2566: /* ╦ */ draw_hline(buf,cw,ch,my-2,0,cw,thin); draw_hline(buf,cw,ch,my+2,0,mx-2,thin); draw_hline(buf,cw,ch,my+2,mx+2,cw,thin);
                         draw_vline(buf,cw,ch,mx-2,my+2,ch,thin); draw_vline(buf,cw,ch,mx+2,my+2,ch,thin); return true;
    case 0x2569: /* ╩ */ draw_hline(buf,cw,ch,my+2,0,cw,thin); draw_hline(buf,cw,ch,my-2,0,mx-2,thin); draw_hline(buf,cw,ch,my-2,mx+2,cw,thin);
                         draw_vline(buf,cw,ch,mx-2,0,my-2,thin); draw_vline(buf,cw,ch,mx+2,0,my-2,thin); return true;
    case 0x256C: /* ╬ */ draw_hline(buf,cw,ch,my-2,0,mx-2,thin); draw_hline(buf,cw,ch,my-2,mx+2,cw,thin);
                         draw_hline(buf,cw,ch,my+2,0,mx-2,thin); draw_hline(buf,cw,ch,my+2,mx+2,cw,thin);
                         draw_vline(buf,cw,ch,mx-2,0,my-2,thin); draw_vline(buf,cw,ch,mx-2,my+2,ch,thin);
                         draw_vline(buf,cw,ch,mx+2,0,my-2,thin); draw_vline(buf,cw,ch,mx+2,my+2,ch,thin); return true;

    /* Dashed lines — approximate with segments */
    case 0x2504: case 0x2508: /* ┄ ┈ */
        for (i32 x=0;x<cw;x+=4) draw_hline(buf,cw,ch,my,x,x+2<cw?x+2:cw,thin); return true;
    case 0x2505: case 0x2509:
        for (i32 x=0;x<cw;x+=4) draw_hline(buf,cw,ch,my-thick/2,x,x+2<cw?x+2:cw,thick); return true;
    case 0x2506: case 0x250A: /* ┆ ┊ */
        for (i32 y=0;y<ch;y+=4) draw_vline(buf,cw,ch,mx,y,y+2<ch?y+2:ch,thin); return true;
    case 0x2507: case 0x250B:
        for (i32 y=0;y<ch;y+=4) draw_vline(buf,cw,ch,mx-thick/2,y,y+2<ch?y+2:ch,thick); return true;

    /* Rounded corners */
    case 0x256D: /* ╭ */ draw_hline(buf,cw,ch,my,mx,cw,thin); draw_vline(buf,cw,ch,mx,my,ch,thin); return true;
    case 0x256E: /* ╮ */ draw_hline(buf,cw,ch,my,0,mx+thin,thin); draw_vline(buf,cw,ch,mx,my,ch,thin); return true;
    case 0x256F: /* ╯ */ draw_hline(buf,cw,ch,my,0,mx+thin,thin); draw_vline(buf,cw,ch,mx,0,my+thin,thin); return true;
    case 0x2570: /* ╰ */ draw_hline(buf,cw,ch,my,mx,cw,thin); draw_vline(buf,cw,ch,mx,0,my+thin,thin); return true;

    default:
        /* Generic fallback for any box drawing char (U+2500-257F):
         * Decode direction segments from codepoint structure.
         * Most mixed light/heavy variants (0x250D-0x254A) follow a pattern:
         * - offset from base determines which segments are light vs heavy */
        if (cp >= 0x250D && cp <= 0x254A) {
            /* Mixed light/heavy — approximate with thin lines everywhere
             * since the exact pattern is complex but visually close enough */
            bool has_right = false, has_left = false, has_down = false, has_up = false;
            i32 base = (i32)(cp - 0x2500);

            /* Decode from pattern tables — simplified */
            /* Right/left from horizontal component */
            if (base >= 0x0D && base <= 0x10) { has_down = true; has_right = true; }
            else if (base >= 0x11 && base <= 0x14) { has_down = true; has_left = true; }
            else if (base >= 0x15 && base <= 0x18) { has_up = true; has_right = true; }
            else if (base >= 0x19 && base <= 0x1C) { has_up = true; has_left = true; }
            else if (base >= 0x1D && base <= 0x24) { has_up = true; has_down = true; has_right = true; }
            else if (base >= 0x25 && base <= 0x2C) { has_up = true; has_down = true; has_left = true; }
            else if (base >= 0x2D && base <= 0x32) { has_left = true; has_right = true; has_down = true; }
            else if (base >= 0x35 && base <= 0x3C) { has_left = true; has_right = true; has_up = true; }
            else if (base >= 0x3D && base <= 0x4A) { has_left = true; has_right = true; has_up = true; has_down = true; }
            else { has_left = true; has_right = true; has_up = true; has_down = true; }

            if (has_right) draw_hline(buf, cw, ch, my, mx, cw, thin);
            if (has_left)  draw_hline(buf, cw, ch, my, 0, mx + thin, thin);
            if (has_down)  draw_vline(buf, cw, ch, mx, my, ch, thin);
            if (has_up)    draw_vline(buf, cw, ch, mx, 0, my + thin, thin);
            return true;
        }

        /* For any remaining U+2500-257F not handled: draw a cross as placeholder */
        if (cp >= 0x2500 && cp <= 0x257F) {
            draw_hline(buf, cw, ch, my, 0, cw, thin);
            draw_vline(buf, cw, ch, mx, 0, ch, thin);
            return true;
        }

        return false;
    }
}

/* Rasterize a glyph on demand — searches primary + fallback fonts */
static CachedGlyph *rasterize_glyph(FontAtlas *atlas, u32 cp) {
    if (!g_font_data || g_font_scale == 0) return NULL;

    i32 cw = (i32)atlas->cell_width;
    i32 ch = (i32)atlas->cell_height;

#ifdef PLATFORM_MACOS
    /* Color emoji: rasterize to RGBA color atlas. Lazy-create the MTLTexture
     * (and retain once) on the very first emoji the process encounters —
     * saves ~1 MB of GPU memory for users who never type emoji. */
    if (utf8_is_emoji(cp) && atlas->color_atlas_w > 0) {
#ifdef USE_METAL
        if (!atlas->color_texture) {
            (void)font_ensure_color_atlas_metal(atlas);
        }
#endif
        i32 ecw = cw * 2; /* emoji are double-width */
        u8 *rgba = calloc(1, (usize)(ecw * ch * 4));
        if (!rgba) goto skip_emoji;

        if (ct_rasterize_color_glyph(rgba, ecw, ch, cp, g_font_ascent)) {
            /* Pack into color atlas */
            if (atlas->color_atlas_x + ecw + 2 > atlas->color_atlas_w) {
                atlas->color_atlas_x = 0;
                atlas->color_atlas_y += atlas->color_row_h + 2;
                atlas->color_row_h = 0;
            }
            if (atlas->color_atlas_y + ch + 2 > atlas->color_atlas_h) {
                free(rgba);
                /* Color atlas full and non-growable: install an atlas-exhausted
                 * sentinel so this emoji renders as a space placeholder instead
                 * of re-running the full CoreText color raster every frame.
                 * Emoji always take the synchronous path (font_get_glyph_uv
                 * line ~1546 excludes emoji from the async worker), so without
                 * a cache entry this CTLine raster repeats on every frame the
                 * glyph is visible. */
                cache_insert_exhausted(atlas, cp);
                return NULL;
            }

            i32 ax = atlas->color_atlas_x + 1;
            i32 ay = atlas->color_atlas_y + 1;

#ifdef USE_METAL
            if (atlas->color_texture)
                renderer_metal_upload_texture(atlas->color_texture, ax, ay, ecw, ch, rgba);
#endif
            if (atlas->color_bitmap) {
                for (i32 row = 0; row < ch; row++)
                    memcpy(atlas->color_bitmap + (ay + row) * atlas->color_atlas_w * 4 + ax * 4,
                           rgba + row * ecw * 4, (usize)(ecw * 4));
            }
            free(rgba);

            f32 u0 = (f32)ax / atlas->color_atlas_w;
            f32 v0 = (f32)ay / atlas->color_atlas_h;
            f32 u1 = (f32)(ax + ecw) / atlas->color_atlas_w;
            f32 v1 = (f32)(ay + ch) / atlas->color_atlas_h;

            atlas->color_atlas_x += ecw + 2;
            if (ch + 2 > atlas->color_row_h) atlas->color_row_h = ch + 2;

            /* Cache with negative UV to signal color glyph (u0 negative = color) */
            return cache_insert(cp, -u0, v0, -u1, v1);
        }
        free(rgba);
    }
skip_emoji:
#endif

    /* Box drawing / block elements: pixel-perfect programmatic rendering */
    if ((cp >= 0x2500 && cp <= 0x257F) || (cp >= 0x2580 && cp <= 0x259F)) {
        u8 *alpha = calloc(1, (usize)(cw * ch));
        if (!alpha) return NULL;
        if (render_box_drawing(alpha, cw, ch, cp)) {
            if (!font_ensure_atlas_room(atlas, cw, ch)) { free(alpha); return NULL; }
            i32 ax = g_atlas_x + 1, ay = g_atlas_y + 1;
#ifdef USE_METAL
            if (atlas->metal_texture)
                renderer_metal_upload_texture(atlas->metal_texture, ax, ay, cw, ch, alpha);
            if (atlas->atlas_bitmap) {
                for (i32 row = 0; row < ch; row++)
                    memcpy(atlas->atlas_bitmap + ((ay + row) * atlas->atlas_width + ax),
                           alpha + row * cw, (usize)cw);
            }
#else
            glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, cw, ch, GL_RED, GL_UNSIGNED_BYTE, alpha);
#endif
            free(alpha);
            f32 u0 = (f32)ax / atlas->atlas_width, v0 = (f32)ay / atlas->atlas_height;
            f32 u1 = (f32)(ax + cw) / atlas->atlas_width, v1 = (f32)(ay + ch) / atlas->atlas_height;
            g_atlas_x += cw + 2;
            if (ch + 2 > g_atlas_row_h) g_atlas_row_h = ch + 2;
            if (cp < 256) { atlas->glyphs[cp].u0=u0; atlas->glyphs[cp].v0=v0; atlas->glyphs[cp].u1=u1; atlas->glyphs[cp].v1=v1; }
            return cache_insert(cp, u0, v0, u1, v1);
        }
        free(alpha);
        /* Fall through to font-based rendering */
    }

    if (!font_ensure_atlas_room(atlas, cw, ch)) {
        /* Atlas is full and cannot grow (hit ATLAS_MAX_SIZE). Record a sentinel
         * so this codepoint renders as a space and is NOT re-rasterised on every
         * subsequent frame. Without this, the same cp re-enters rasterize_glyph
         * each frame -> sustained CPU/CoreText cliff. */
        cache_insert_exhausted(atlas, cp);
        return NULL;
    }

    /* Rasterize into R8 alpha temp buffer. */
    u8 *tmp = calloc(1, (usize)(cw * ch));
    if (!tmp) return NULL;

    bool rasterized = false;

#ifdef PLATFORM_MACOS
    /* Serialize on g_ct_mu: the async raster worker draws into the same shared
     * pooled CG context (g_glyph_ctx/g_glyph_buf). Without the lock both threads
     * scribble the same backing store — or get_glyph_context frees it under the
     * other mid-draw — and CoreGraphics faults inside ripc_Render. */
    pthread_mutex_lock(&g_ct_mu);
    rasterized = ct_rasterize_glyph(tmp, cw, ch, cp, g_font_ascent);
    pthread_mutex_unlock(&g_ct_mu);
#endif

    if (!rasterized) {
        f32 scale;
        stbtt_fontinfo *font = find_font_for_glyph(cp, &scale);
        if (!font && cp != ' ') {
            free(tmp);
            CachedGlyph *space = cache_lookup(32);
            if (space) return cache_insert(cp, space->u0, space->v0, space->u1, space->v1);
            return NULL;
        }
        if (!font) font = &g_font_info;

        i32 x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(font, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
        i32 gw = x1 - x0, gh = y1 - y0;

        if (gw > 0 && gh > 0) {
            i32 advance_w, lsb;
            stbtt_GetCodepointHMetrics(font, (int)cp, &advance_w, &lsb);
            i32 bx = (i32)floorf(lsb * scale);
            i32 by = (i32)g_font_ascent + y0;
            if (bx < 0) bx = 0;
            if (by < 0) by = 0;
            i32 rw = gw, rh = gh;
            if (bx + rw > cw) rw = cw - bx;
            if (by + rh > ch) rh = ch - by;
            if (rw > 0 && rh > 0) {
                stbtt_MakeCodepointBitmap(font, tmp + by * cw + bx,
                                           rw, rh, cw, scale, scale, (int)cp);
            }
        }
    }

    /* Upload to atlas texture */
    i32 ax = g_atlas_x + 1;
    i32 ay = g_atlas_y + 1;

#ifdef USE_METAL
    if (atlas->metal_texture)
        renderer_metal_upload_texture(atlas->metal_texture, ax, ay, cw, ch, tmp);
    if (atlas->atlas_bitmap) {
        for (i32 row = 0; row < ch; row++)
            memcpy(atlas->atlas_bitmap + ((ay + row) * atlas->atlas_width + ax),
                   tmp + row * cw, (usize)cw);
    }
#else
    glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, cw, ch, GL_RED, GL_UNSIGNED_BYTE, tmp);
#endif
    free(tmp);

    /* UV coords */
    f32 u0f = (f32)ax / atlas->atlas_width;
    f32 v0f = (f32)ay / atlas->atlas_height;
    f32 u1f = (f32)(ax + cw) / atlas->atlas_width;
    f32 v1f = (f32)(ay + ch) / atlas->atlas_height;

    /* Advance packing cursor */
    g_atlas_x += cw + 2;
    if (ch + 2 > g_atlas_row_h) g_atlas_row_h = ch + 2;

    /* Store in atlas glyphs array for ASCII fast path */
    if (cp < 256) {
        atlas->glyphs[cp].u0 = u0f;
        atlas->glyphs[cp].v0 = v0f;
        atlas->glyphs[cp].u1 = u1f;
        atlas->glyphs[cp].v1 = v1f;
    }

    return cache_insert(cp, u0f, v0f, u1f, v1f);
}

/* =========================================================================
 * Public: get glyph UV (with on-demand rasterization)
 * ========================================================================= */

bool font_get_glyph_uv(FontAtlas *atlas, u32 cp, f32 *u0, f32 *v0, f32 *u1, f32 *v1) {
    /* Fast path: ASCII */
    if (cp >= 32 && cp < 127) {
        *u0 = atlas->glyphs[cp].u0;
        *v0 = atlas->glyphs[cp].v0;
        *u1 = atlas->glyphs[cp].u1;
        *v1 = atlas->glyphs[cp].v1;
        return true;
    }

    /* Cache lookup */
    CachedGlyph *cached = cache_lookup(cp);
    if (cached) {
        *u0 = cached->u0; *v0 = cached->v0;
        *u1 = cached->u1; *v1 = cached->v1;
        /* Atlas-exhausted sentinel: UVs already point at the space glyph.
         * Returning false tells the caller this is a placeholder, while the
         * cache HIT prevents the per-frame re-rasterisation cliff. */
        return !cached->exhausted;
    }

    /* async path — if worker is live, enqueue and return a space
     * placeholder. The next frame (after font_drain_raster_completions) the
     * real glyph will be cached and this lookup will hit the cache.  Emoji
     * still go through the synchronous path because they use a separate
     * color atlas and packing state. */
    if (g_raster.started && !utf8_is_emoji(cp)) {
        raster_enqueue(cp);
        *u0 = atlas->glyphs[32].u0; *v0 = atlas->glyphs[32].v0;
        *u1 = atlas->glyphs[32].u1; *v1 = atlas->glyphs[32].v1;
        return true; /* placeholder is valid */
    }

    /* Synchronous fallback: worker not started, or emoji path */
    cached = rasterize_glyph(atlas, cp);
    if (cached) {
        *u0 = cached->u0; *v0 = cached->v0;
        *u1 = cached->u1; *v1 = cached->v1;
        return true;
    }

    /* Fallback to space */
    *u0 = atlas->glyphs[32].u0; *v0 = atlas->glyphs[32].v0;
    *u1 = atlas->glyphs[32].u1; *v1 = atlas->glyphs[32].v1;
    return false;
}

void font_warm_text_glyphs_n(FontAtlas *atlas, const char *utf8_text, usize len) {
    if (!atlas || !utf8_text) return;
    const u8 *p = (const u8 *)utf8_text;
    usize i = 0;
    while (i < len && (len != (usize)-1 || p[i] != 0)) {
        usize remain = (len == (usize)-1) ? 4 : (len - i);
        if (remain > 4) remain = 4;
        u32 cp = 0;
        u32 n = utf8_decode(p + i, remain, &cp);
        if (n == 0) { i++; continue; }
        i += n;
        /* ASCII printable lives in atlas->glyphs[32..126] from the warm-on-
         * boot pass; nothing to do. Controls and DEL we wouldn't render
         * anyway. */
        if (cp < 127) continue;
        if (cache_lookup(cp)) continue;
        /* Sync rasterize. Goes through the same path the worker uses for
         * the bitmap, but we own the atlas mutation here (main thread),
         * so the cache entry is installed before we return. */
        (void)rasterize_glyph(atlas, cp);
    }
}

void font_warm_text_glyphs(FontAtlas *atlas, const char *utf8_text) {
    font_warm_text_glyphs_n(atlas, utf8_text, (usize)-1);
}

/* =========================================================================
 * Composite glyph rendering (base + combining marks)
 * ========================================================================= */

/* Generate a unique cache key from base codepoint + combining marks.
 * We hash them into a single u32 in the private-use supplementary area
 * (U+100000-U+10FFFF) to avoid collisions with real codepoints. */
static u32 composite_cache_key(u32 base, const u32 *combining, u8 count) {
    u32 h = base;
    for (u8 i = 0; i < count; i++) {
        h ^= combining[i] * 2654435761u;
        h = (h << 7) | (h >> 25);
    }
    /* Map into a region that won't collide with normal codepoints.
     * Use high bits to distinguish from regular codepoints. */
    return 0x80000000u | (h & 0x7FFFFFFFu);
}

bool font_get_composite_glyph_uv(FontAtlas *atlas, u32 base, const u32 *combining,
                                  u8 combining_count, f32 *u0, f32 *v0, f32 *u1, f32 *v1) {
    if (combining_count == 0) {
        return font_get_glyph_uv(atlas, base, u0, v0, u1, v1);
    }

    u32 key = composite_cache_key(base, combining, combining_count);

    /* Cache lookup */
    CachedGlyph *cached = cache_lookup(key);
    if (cached) {
        *u0 = cached->u0; *v0 = cached->v0;
        *u1 = cached->u1; *v1 = cached->v1;
        return true;
    }

    /* Rasterize composite glyph */
    i32 cw = (i32)atlas->cell_width;
    i32 ch = (i32)atlas->cell_height;

#ifdef PLATFORM_MACOS
    if (g_ct_font) {
        /* Build the full Unicode string: base + combining marks */
        UniChar uchars[16];
        i32 ulen = 0;

        /* Encode base */
        if (base > 0xFFFF) {
            uchars[ulen++] = (UniChar)(((base - 0x10000) >> 10) + 0xD800);
            uchars[ulen++] = (UniChar)(((base - 0x10000) & 0x3FF) + 0xDC00);
        } else {
            uchars[ulen++] = (UniChar)base;
        }

        /* Encode combining marks */
        for (u8 i = 0; i < combining_count && ulen < 14; i++) {
            u32 cp = combining[i];
            if (cp > 0xFFFF) {
                uchars[ulen++] = (UniChar)(((cp - 0x10000) >> 10) + 0xD800);
                uchars[ulen++] = (UniChar)(((cp - 0x10000) & 0x3FF) + 0xDC00);
            } else {
                uchars[ulen++] = (UniChar)cp;
            }
        }

        /* Use Core Text to render the shaped composite string */
        CFStringRef str = CFStringCreateWithCharacters(NULL, uchars, ulen);
        if (str) {
            CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 1,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(attrs, kCTFontAttributeName, g_ct_font);

            CFAttributedStringRef astr = CFAttributedStringCreate(NULL, str, attrs);
            CTLineRef line = CTLineCreateWithAttributedString(astr);

            u8 *tmp = calloc(1, (usize)(cw * ch));
            if (tmp && line) {
                /* Shared pooled CG context — lock against the async raster
                 * worker (see g_ct_mu use in rasterize_alpha_bitmap). */
                pthread_mutex_lock(&g_ct_mu);
                CGContextRef ctx = get_glyph_context(cw, ch);
                if (ctx) {
                    /* Alpha-only context — see ct_rasterize_glyph for the
                     * same clear/fill discipline. */
                    CGContextClearRect(ctx, CGRectMake(0, 0, cw, ch));
                    CGContextSetGrayFillColor(ctx, 1.0, 1.0);
                    CGContextSetTextPosition(ctx, 0, ch - g_font_ascent);
                    CTLineDraw(line, ctx);
                    downsample_glyph_buf(tmp, cw, ch);
                }
                pthread_mutex_unlock(&g_ct_mu);

                if (font_ensure_atlas_room(atlas, cw, ch)) {
                    i32 ax = g_atlas_x + 1;
                    i32 ay = g_atlas_y + 1;
#ifdef USE_METAL
                    if (atlas->metal_texture)
                        renderer_metal_upload_texture(atlas->metal_texture, ax, ay, cw, ch, tmp);
                    if (atlas->atlas_bitmap) {
                        for (i32 row = 0; row < ch; row++)
                            memcpy(atlas->atlas_bitmap + ((ay + row) * atlas->atlas_width + ax),
                                   tmp + row * cw, (usize)cw);
                    }
#else
                    glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, cw, ch,
                                    GL_RED, GL_UNSIGNED_BYTE, tmp);
#endif
                    *u0 = (f32)ax / atlas->atlas_width;
                    *v0 = (f32)ay / atlas->atlas_height;
                    *u1 = (f32)(ax + cw) / atlas->atlas_width;
                    *v1 = (f32)(ay + ch) / atlas->atlas_height;

                    g_atlas_x += cw + 2;
                    if (ch + 2 > g_atlas_row_h) g_atlas_row_h = ch + 2;

                    free(tmp);
                    CFRelease(line);
                    CFRelease(astr);
                    CFRelease(attrs);
                    CFRelease(str);

                    cache_insert(key, *u0, *v0, *u1, *v1);
                    return true;
                }
            }
            free(tmp);
            if (line) CFRelease(line);
            CFRelease(astr);
            CFRelease(attrs);
            CFRelease(str);
        }
    }
#endif /* PLATFORM_MACOS */

    /* Fallback: render base glyph only (combining marks overlay not supported
     * without CoreText on non-macOS platforms). Render each combining mark
     * overlaid by blending into the same bitmap using stb_truetype. */
    {
        /* `tmp` is a 1-channel alpha accumulator. CT and stb both write
         * coverage straight into it; combining marks max-blend in place. */
        u8 *tmp = calloc(1, (usize)(cw * ch));
        if (!tmp) goto composite_fail;

        /* Render base glyph */
        bool ok = false;
#ifdef PLATFORM_MACOS
        pthread_mutex_lock(&g_ct_mu);
        ok = ct_rasterize_glyph(tmp, cw, ch, base, g_font_ascent);
        pthread_mutex_unlock(&g_ct_mu);
#endif
        if (!ok) {
            f32 scale;
            stbtt_fontinfo *font = find_font_for_glyph(base, &scale);
            if (!font) font = &g_font_info;
            if (font) {
                i32 x0b, y0b, x1b, y1b;
                stbtt_GetCodepointBitmapBox(font, (int)base, scale, scale, &x0b, &y0b, &x1b, &y1b);
                i32 gw = x1b - x0b, gh = y1b - y0b;
                if (gw > 0 && gh > 0) {
                    i32 advance_w, lsb;
                    stbtt_GetCodepointHMetrics(font, (int)base, &advance_w, &lsb);
                    i32 bx = (i32)floorf(lsb * scale);
                    i32 by = (i32)g_font_ascent + y0b;
                    if (bx < 0) bx = 0;
                    if (by < 0) by = 0;
                    i32 rw = gw, rh = gh;
                    if (bx + rw > cw) rw = cw - bx;
                    if (by + rh > ch) rh = ch - by;
                    if (rw > 0 && rh > 0)
                        stbtt_MakeCodepointBitmap(font, tmp + by * cw + bx,
                                                   rw, rh, cw, scale, scale, (int)base);
                    ok = true;
                }
            }
        }

        /* Overlay combining marks using stb_truetype (additive blend) */
        for (u8 ci = 0; ci < combining_count && ok; ci++) {
            u32 cc = combining[ci];
            f32 scale;
            stbtt_fontinfo *font = find_font_for_glyph(cc, &scale);
            if (!font) font = &g_font_info;
            if (!font) continue;

            i32 x0c, y0c, x1c, y1c;
            stbtt_GetCodepointBitmapBox(font, (int)cc, scale, scale, &x0c, &y0c, &x1c, &y1c);
            i32 gw = x1c - x0c, gh = y1c - y0c;
            if (gw <= 0 || gh <= 0) continue;

            i32 advance_w, lsb;
            stbtt_GetCodepointHMetrics(font, (int)cc, &advance_w, &lsb);
            i32 bx = (i32)floorf(lsb * scale);
            i32 by = (i32)g_font_ascent + y0c;
            if (bx < 0) bx = 0;
            if (by < 0) by = 0;
            i32 rw = gw, rh = gh;
            if (bx + rw > cw) rw = cw - bx;
            if (by + rh > ch) rh = ch - by;
            if (rw <= 0 || rh <= 0) continue;

            /* Rasterize combining mark into temp buffer, then max-blend */
            u8 *cbuf = calloc(1, (usize)(rw * rh));
            if (!cbuf) continue;
            stbtt_MakeCodepointBitmap(font, cbuf, rw, rh, rw, scale, scale, (int)cc);
            for (i32 row = 0; row < rh; row++) {
                for (i32 x = 0; x < rw; x++) {
                    i32 dst_idx = (by + row) * cw + (bx + x);
                    u8 src = cbuf[row * rw + x];
                    if (src > tmp[dst_idx]) tmp[dst_idx] = src;
                }
            }
            free(cbuf);
        }

        if (!ok) { free(tmp); goto composite_fail; }

        if (!font_ensure_atlas_room(atlas, cw, ch)) { free(tmp); goto composite_fail; }

        i32 ax = g_atlas_x + 1, ay = g_atlas_y + 1;
#ifdef USE_METAL
        if (atlas->metal_texture)
            renderer_metal_upload_texture(atlas->metal_texture, ax, ay, cw, ch, tmp);
        if (atlas->atlas_bitmap) {
            for (i32 row = 0; row < ch; row++)
                memcpy(atlas->atlas_bitmap + ((ay + row) * atlas->atlas_width + ax),
                       tmp + row * cw, (usize)cw);
        }
#else
        glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, cw, ch, GL_RED, GL_UNSIGNED_BYTE, tmp);
#endif
        free(tmp);

        *u0 = (f32)ax / atlas->atlas_width;
        *v0 = (f32)ay / atlas->atlas_height;
        *u1 = (f32)(ax + cw) / atlas->atlas_width;
        *v1 = (f32)(ay + ch) / atlas->atlas_height;
        g_atlas_x += cw + 2;
        if (ch + 2 > g_atlas_row_h) g_atlas_row_h = ch + 2;

        cache_insert(key, *u0, *v0, *u1, *v1);
        return true;
    }

composite_fail:
    /* Fall back to just the base glyph */
    return font_get_glyph_uv(atlas, base, u0, v0, u1, v1);
}

/* =========================================================================
 * Ligature cache -- keyed by codepoint sequence
 *
 * Each entry stores the codepoints forming a ligature, the number of chars,
 * the atlas UV coordinates, and whether the entry has been queried and found
 * to NOT be a ligature (negative cache).
 * ========================================================================= */

#define LIGATURE_CACHE_SIZE 512
#define LIGATURE_MAX_LEN    4

typedef struct {
    u32  codepoints[LIGATURE_MAX_LEN]; /* codepoint sequence */
    u8   count;                        /* number of chars */
    u8   width;                        /* ligature width in cells (0 = not a ligature) */
    bool valid;                        /* slot occupied */
    bool negative;                     /* true = known NOT to be a ligature */
    f32  u0, v0, u1, v1;              /* atlas UV (only meaningful if width > 0) */
} LigatureEntry;

static LigatureEntry g_lig_cache[LIGATURE_CACHE_SIZE];

static void font_rescale_uv(f32 *u0, f32 *v0, f32 *u1, f32 *v1,
                            i32 old_w, i32 old_h, i32 new_w, i32 new_h) {
    if (!u0 || !v0 || !u1 || !v1) return;
    if (*u1 == 0.0f && *v1 == 0.0f) return;
    f32 x0 = *u0 * (f32)old_w;
    f32 y0 = *v0 * (f32)old_h;
    f32 x1 = *u1 * (f32)old_w;
    f32 y1 = *v1 * (f32)old_h;
    *u0 = x0 / (f32)new_w;
    *v0 = y0 / (f32)new_h;
    *u1 = x1 / (f32)new_w;
    *v1 = y1 / (f32)new_h;
}

static void font_rescale_atlas_caches(FontAtlas *atlas,
                                      i32 old_w, i32 old_h,
                                      i32 new_w, i32 new_h) {
    for (u32 i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (!g_cache[i].valid) continue;
        if (g_cache[i].u0 < 0.0f || g_cache[i].u1 < 0.0f) continue; /* color atlas */
        font_rescale_uv(&g_cache[i].u0, &g_cache[i].v0,
                        &g_cache[i].u1, &g_cache[i].v1,
                        old_w, old_h, new_w, new_h);
    }
    for (u32 cp = 0; cp < 256; cp++) {
        if (atlas->glyphs[cp].u1 == 0.0f && atlas->glyphs[cp].v1 == 0.0f) continue;
        font_rescale_uv(&atlas->glyphs[cp].u0, &atlas->glyphs[cp].v0,
                        &atlas->glyphs[cp].u1, &atlas->glyphs[cp].v1,
                        old_w, old_h, new_w, new_h);
    }
    for (u32 i = 0; i < LIGATURE_CACHE_SIZE; i++) {
        LigatureEntry *e = &g_lig_cache[i];
        if (!e->valid || e->negative || e->width == 0) continue;
        font_rescale_uv(&e->u0, &e->v0, &e->u1, &e->v1,
                        old_w, old_h, new_w, new_h);
    }
}

static bool font_ensure_atlas_room(FontAtlas *atlas, i32 w, i32 h) {
    if (!atlas || w <= 0 || h <= 0) return false;
    if (w + 2 > ATLAS_MAX_SIZE || h + 2 > ATLAS_MAX_SIZE) return false;

    if (g_atlas_x + w + 2 > atlas->atlas_width) {
        g_atlas_x = 0;
        g_atlas_y += g_atlas_row_h + 2;
        g_atlas_row_h = 0;
    }
    if (g_atlas_y + h + 2 <= atlas->atlas_height) return true;

    i32 old_w = atlas->atlas_width;
    i32 old_h = atlas->atlas_height;
    i32 new_w = old_w;
    i32 new_h = old_h;
    while (g_atlas_y + h + 2 > new_h && new_h < ATLAS_MAX_SIZE) {
        new_w *= 2;
        new_h *= 2;
        if (new_w > ATLAS_MAX_SIZE) new_w = ATLAS_MAX_SIZE;
        if (new_h > ATLAS_MAX_SIZE) new_h = ATLAS_MAX_SIZE;
    }
    if (new_w == old_w && new_h == old_h) return false;
    if (g_atlas_y + h + 2 > new_h) return false;

    u8 *new_bitmap = calloc(1, (usize)new_w * (usize)new_h * ATLAS_BPP);
    if (!new_bitmap) return false;
    if (atlas->atlas_bitmap) {
        for (i32 row = 0; row < old_h; row++) {
            memcpy(new_bitmap + (usize)row * (usize)new_w * ATLAS_BPP,
                   atlas->atlas_bitmap + (usize)row * (usize)old_w * ATLAS_BPP,
                   (usize)old_w * ATLAS_BPP);
        }
    }

#ifdef USE_METAL
    /* Keep the CPU mirror while the metal texture doesn't exist yet: we're
     * still inside font_atlas_create (the bulk upload in
     * font_atlas_create_metal_texture hasn't run). Dropping it here — which is
     * what happens when a LARGER font's pre-raster grows the atlas — would
     * leave the freshly-created texture empty and ALL glyphs would vanish.
     * Once the texture exists (steady state) we drop the mirror to save RSS. */
    bool keep_cpu_mirror = (atlas->metal_texture == NULL);
    if (!atlas->atlas_bitmap && atlas->metal_texture) {
        id<MTLTexture> old_tex = (__bridge id<MTLTexture>)atlas->metal_texture;
        MTLRegion old_region = MTLRegionMake2D(0, 0, (NSUInteger)old_w, (NSUInteger)old_h);
        [old_tex getBytes:new_bitmap
              bytesPerRow:(NSUInteger)new_w * ATLAS_BPP
               fromRegion:old_region
              mipmapLevel:0];
    }
    if (atlas->metal_texture) {
        if (!atlas->metal_device) {
            free(new_bitmap);
            return false;
        }
        id<MTLDevice> dev = (__bridge id<MTLDevice>)atlas->metal_device;
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
            width:(NSUInteger)new_w height:(NSUInteger)new_h mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> new_tex = [dev newTextureWithDescriptor:desc];
        if (!new_tex) {
            free(new_bitmap);
            return false;
        }
        [new_tex retain];
        renderer_metal_upload_texture((__bridge void *)new_tex, 0, 0, new_w, new_h, new_bitmap);
        id old_tex = (__bridge id)atlas->metal_texture;
        [old_tex release];
        [old_tex release];
        atlas->metal_texture = (__bridge void *)new_tex;
    }
#else
    bool keep_cpu_mirror = true;    /* OpenGL: per-glyph memcpy + glTexSubImage2D needs the mirror */
    if (atlas->texture_id) {
        if (!atlas->atlas_bitmap) {
            u8 *old_bitmap = calloc(1, (usize)old_w * (usize)old_h * ATLAS_BPP);
            if (old_bitmap) {
                glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, old_bitmap);
                for (i32 row = 0; row < old_h; row++) {
                    memcpy(new_bitmap + (usize)row * (usize)new_w * ATLAS_BPP,
                           old_bitmap + (usize)row * (usize)old_w * ATLAS_BPP,
                           (usize)old_w * ATLAS_BPP);
                }
                free(old_bitmap);
            }
        }
        glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, new_w, new_h, 0,
                     GL_RED, GL_UNSIGNED_BYTE, new_bitmap);
        g_atlas_tex = atlas->texture_id;
    }
#endif

    free(atlas->atlas_bitmap);
    /* Under Metal the per-glyph upload path goes through
     * renderer_metal_upload_texture directly with a small staging buffer; the
     * CPU mirror only exists to feed the GPU texture in bulk on init/grow.
     * Now that the new texture has been seeded above, drop the mirror — the
     * next grow rebuilds it via [old_tex getBytes:] readback (see top of
     * function). Saves the full atlas footprint (e.g. 2.25 MB at 768², up to
     * 16 MB at 2048²) in steady-state RSS. */
    if (keep_cpu_mirror) {
        atlas->atlas_bitmap = new_bitmap;
    } else {
        free(new_bitmap);
        atlas->atlas_bitmap = NULL;
    }
    atlas->atlas_width = new_w;
    atlas->atlas_height = new_h;
    font_rescale_atlas_caches(atlas, old_w, old_h, new_w, new_h);
    g_raster.completed_version++;
    fprintf(stderr, "font: atlas grew %dx%d -> %dx%d%s\n", old_w, old_h, new_w, new_h,
            keep_cpu_mirror ? "" : " (mirror released)");
    return true;
}

static u32 ligature_hash(const u32 *cps_in, i32 cnt) {
    u32 h = 2166136261u;
    for (i32 i = 0; i < cnt; i++) {
        h ^= cps_in[i];
        h *= 16777619u;
    }
    return h % LIGATURE_CACHE_SIZE;
}

static bool lig_keys_equal(const u32 *a, const u32 *b, i32 cnt) {
    for (i32 i = 0; i < cnt; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static LigatureEntry *lig_cache_lookup(const u32 *cps_in, i32 cnt) {
    u32 idx = ligature_hash(cps_in, cnt);
    for (u32 i = 0; i < 16; i++) {
        u32 slot = (idx + i) % LIGATURE_CACHE_SIZE;
        if (!g_lig_cache[slot].valid) return NULL;
        if (g_lig_cache[slot].count == (u8)cnt &&
            lig_keys_equal(g_lig_cache[slot].codepoints, cps_in, cnt))
            return &g_lig_cache[slot];
    }
    return NULL;
}

static LigatureEntry *lig_cache_insert(const u32 *cps_in, i32 cnt, i32 width,
                                        f32 lu0, f32 lv0, f32 lu1, f32 lv1,
                                        bool negative) {
    u32 idx = ligature_hash(cps_in, cnt);
    for (u32 i = 0; i < 16; i++) {
        u32 slot = (idx + i) % LIGATURE_CACHE_SIZE;
        if (!g_lig_cache[slot].valid) {
            LigatureEntry *e = &g_lig_cache[slot];
            for (i32 j = 0; j < cnt && j < LIGATURE_MAX_LEN; j++)
                e->codepoints[j] = cps_in[j];
            e->count = (u8)cnt;
            e->width = (u8)width;
            e->valid = true;
            e->negative = negative;
            e->u0 = lu0; e->v0 = lv0;
            e->u1 = lu1; e->v1 = lv1;
            return e;
        }
    }
    /* Cache full -- overwrite first slot */
    u32 slot = idx % LIGATURE_CACHE_SIZE;
    LigatureEntry *e = &g_lig_cache[slot];
    for (i32 j = 0; j < cnt && j < LIGATURE_MAX_LEN; j++)
        e->codepoints[j] = cps_in[j];
    e->count = (u8)cnt;
    e->width = (u8)width;
    e->valid = true;
    e->negative = negative;
    e->u0 = lu0; e->v0 = lv0;
    e->u1 = lu1; e->v1 = lv1;
    return e;
}

/* =========================================================================
 * Ligature detection and rasterization via CoreText shaping
 * ========================================================================= */

i32 font_check_ligature(FontAtlas *atlas, const u32 *cps, i32 count,
                        f32 *u0, f32 *v0, f32 *u1, f32 *v1) {
    if (count < 2 || count > LIGATURE_MAX_LEN) return 0;

    /* 1. Check ligature cache first */
    LigatureEntry *cached = lig_cache_lookup(cps, count);
    if (cached) {
        if (cached->negative || cached->width == 0) return 0;
        *u0 = cached->u0; *v0 = cached->v0;
        *u1 = cached->u1; *v1 = cached->v1;
        return (i32)cached->width;
    }

#ifdef PLATFORM_MACOS
    if (!g_ct_font) return 0;

    /* 2. Convert codepoints to UniChar string */
    UniChar uchars[8];
    i32 ulen = 0;
    for (i32 i = 0; i < count && ulen < 7; i++) {
        if (cps[i] <= 0xFFFF) {
            uchars[ulen++] = (UniChar)cps[i];
        }
    }
    if (ulen < 2) {
        lig_cache_insert(cps, count, 0, 0, 0, 0, 0, true);
        return 0;
    }

    /* 3. Create attributed string with ligature attribute enabled */
    CFStringRef str = CFStringCreateWithCharacters(NULL, uchars, ulen);
    if (!str) return 0;

    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCTFontAttributeName, g_ct_font);
    /* Request standard ligatures (value 1) */
    i32 lig_val = 1;
    CFNumberRef lig_num = CFNumberCreate(NULL, kCFNumberSInt32Type, &lig_val);
    CFDictionarySetValue(attrs, kCTLigatureAttributeName, lig_num);
    CFRelease(lig_num);

    CFAttributedStringRef astr = CFAttributedStringCreate(NULL, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(astr);

    /* 4. Check if shaping produced fewer glyphs than input */
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    CFIndex run_count = CFArrayGetCount(runs);
    CFIndex total_glyphs = 0;
    for (CFIndex ri = 0; ri < run_count; ri++) {
        CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, ri);
        total_glyphs += CTRunGetGlyphCount(run);
    }

    i32 result = 0;
    if (total_glyphs < ulen && total_glyphs > 0) {
        /* Ligature detected — rasterize via an alpha-only context to match the
         * single-codepoint glyph path. */
        i32 lig_w = (i32)atlas->cell_width * count;
        i32 lig_h = (i32)atlas->cell_height;
        u8 *tmp = calloc(1, (usize)(lig_w * lig_h));
        if (tmp) {
            CGContextRef ctx = CGBitmapContextCreate(tmp, (size_t)lig_w, (size_t)lig_h,
                                                     8, (size_t)lig_w, NULL,
                                                     (CGBitmapInfo)kCGImageAlphaOnly);
            if (ctx) {
                CGContextClearRect(ctx, CGRectMake(0, 0, lig_w, lig_h));
                CGContextSetAllowsAntialiasing(ctx, true);
                CGContextSetShouldAntialias(ctx, true);
                CGContextSetAllowsFontSmoothing(ctx, false);
                CGContextSetShouldSmoothFonts(ctx, false);
                CGContextSetAllowsFontSubpixelPositioning(ctx, false);
                CGContextSetShouldSubpixelPositionFonts(ctx, false);
                CGContextSetGrayFillColor(ctx, 1.0, 1.0);

                /* Draw the shaped text */
                CGContextSetTextPosition(ctx, 0, lig_h - g_font_ascent);
                CTLineDraw(line, ctx);
                CGContextRelease(ctx);

                if (font_ensure_atlas_room(atlas, lig_w, lig_h)) {
                    i32 ax = g_atlas_x + 1;
                    i32 ay = g_atlas_y + 1;
#ifdef USE_METAL
                    if (atlas->metal_texture)
                        renderer_metal_upload_texture(atlas->metal_texture, ax, ay, lig_w, lig_h, tmp);
                    if (atlas->atlas_bitmap) {
                        for (i32 row = 0; row < lig_h; row++)
                            memcpy(atlas->atlas_bitmap + ((ay + row) * atlas->atlas_width + ax),
                                   tmp + row * lig_w, (usize)lig_w);
                    }
#else
                    glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, lig_w, lig_h,
                                    GL_RED, GL_UNSIGNED_BYTE, tmp);
#endif
                    *u0 = (f32)ax / atlas->atlas_width;
                    *v0 = (f32)ay / atlas->atlas_height;
                    *u1 = (f32)(ax + lig_w) / atlas->atlas_width;
                    *v1 = (f32)(ay + lig_h) / atlas->atlas_height;

                    g_atlas_x += lig_w + 2;
                    if (lig_h + 2 > g_atlas_row_h) g_atlas_row_h = lig_h + 2;
                    result = count;

                    /* Insert into ligature cache */
                    lig_cache_insert(cps, count, result, *u0, *v0, *u1, *v1, false);
                }
            }
            free(tmp);
        }
    }

    if (result == 0) {
        /* Not a ligature -- negative cache to avoid re-shaping */
        lig_cache_insert(cps, count, 0, 0, 0, 0, 0, true);
    }

    CFRelease(line);
    CFRelease(astr);
    CFRelease(attrs);
    CFRelease(str);
    return result;
#else
    /* Non-macOS: stb_truetype has no ligature support; negative cache */
    lig_cache_insert(cps, count, 0, 0, 0, 0, 0, true);
    (void)atlas; (void)u0; (void)v0; (void)u1; (void)v1;
    return 0;
#endif
}

/* High-level wrapper: returns true if ligature found, setting UVs. */
bool font_get_ligature_uv(FontAtlas *font, const u32 *codepoints, i32 count,
                           f32 *u0, f32 *v0, f32 *u1, f32 *v1) {
    i32 w = font_check_ligature(font, codepoints, count, u0, v0, u1, v1);
    return w > 0;
}

/* =========================================================================
 * Font atlas creation
 * ========================================================================= */

bool font_atlas_create(FontAtlas *atlas, const char *font_path, f32 font_size, f32 dpi_scale, f32 font_weight) {
    g_main_thread = pthread_self(); /* record main thread for thread-safe discovery */
    g_font_weight = font_weight;

    /* A bare filename (no path separator) is resolved against the bundled font
     * directory (assets/fonts in the source tree, ~/.config/Liu/fonts otherwise),
     * so config can just say "JetBrainsMono-Regular.ttf" instead of an absolute
     * path. Anything containing a '/' is used verbatim. */
    char fp_buf[1024];
    if (font_path && font_path[0] && !strchr(font_path, '/')
    ) {
        liu_path_join(fp_buf, sizeof fp_buf, font_user_dir(), font_path);
        font_path = fp_buf;   /* parameter is a local pointer; fp_buf lives for the call */
    }
    /* Cache is keyed by codepoint only — re-rasterise everything when the
     * stroke width changes so the new weight actually shows up. */
    memset(atlas, 0, sizeof(*atlas));
    memset(g_cache, 0, sizeof(g_cache));
    memset(g_lig_cache, 0, sizeof(g_lig_cache));
    g_cache_used = 0;
    g_atlas_x = 0;
    g_atlas_y = 0;
    g_atlas_row_h = 0;

    /* Load font file via mmap — minimal RSS impact */
    if (g_font_data) unmap_font_file(g_font_data, g_font_data_size);
    g_font_data = mmap_font_file(font_path, &g_font_data_size);
    if (!g_font_data) {
        fprintf(stderr, "font: cannot open '%s'\n", font_path);
        return false;
    }

    /* Init font */
    int font_offset = stbtt_GetFontOffsetForIndex(g_font_data, 0);
    if (font_offset < 0) font_offset = 0;
    if (!stbtt_InitFont(&g_font_info, g_font_data, font_offset)) {
        fprintf(stderr, "font: invalid '%s'\n", font_path);
        unmap_font_file(g_font_data, g_font_data_size); g_font_data = NULL;
        return false;
    }

    f32 pixel_h = font_size * dpi_scale;
    g_font_scale = stbtt_ScaleForPixelHeight(&g_font_info, pixel_h);

    int ascent_i, descent_i, line_gap_i;
    stbtt_GetFontVMetrics(&g_font_info, &ascent_i, &descent_i, &line_gap_i);
    f32 ascent  = ceilf(ascent_i * g_font_scale);
    f32 descent = floorf(descent_i * g_font_scale);
    f32 line_h  = ascent - descent + floorf(line_gap_i * g_font_scale);

    g_font_ascent = ascent;
    atlas->ascent = ascent;
    atlas->descent = descent;
    atlas->line_height = line_h;

    int advance_w, lsb;
    stbtt_GetCodepointHMetrics(&g_font_info, 'M', &advance_w, &lsb);
    /* roundf for even cell spacing — see Core Text branch below. */
    atlas->cell_width  = roundf(advance_w * g_font_scale);
    atlas->cell_height = line_h;

#ifdef PLATFORM_MACOS
    /* Create Core Text font for pixel-perfect glyph rendering */
    if (g_ct_font) { CFRelease(g_ct_font); g_ct_font = NULL; }
    for (i32 i = 0; i < g_ct_fallback_count; i++) {
        if (g_ct_fallbacks[i]) CFRelease(g_ct_fallbacks[i]);
        g_ct_fallbacks[i] = NULL;
    }
    g_ct_fallback_count = 0;

    /* Create CTFont from file path */
    CFStringRef path_str = CFStringCreateWithCString(NULL, font_path, kCFStringEncodingUTF8);
    if (path_str) {
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path_str, kCFURLPOSIXPathStyle, false);
        if (url) {
            CGDataProviderRef provider = CGDataProviderCreateWithURL(url);
            if (provider) {
                CGFontRef cg_font = CGFontCreateWithDataProvider(provider);
                if (cg_font) {
                    g_ct_font = CTFontCreateWithGraphicsFont(cg_font, (CGFloat)pixel_h, NULL, NULL);
                    CGFontRelease(cg_font);
                }
                CGDataProviderRelease(provider);
            }
            CFRelease(url);
        }
        CFRelease(path_str);
    }
    if (g_ct_font) {
        /* Override cell metrics from Core Text — these are authoritative on macOS */
        CGGlyph m_glyph;
        UniChar m_char = 'M';
        if (CTFontGetGlyphsForCharacters(g_ct_font, &m_char, &m_glyph, 1)) {
            CGSize advance;
            CTFontGetAdvancesForGlyphs(g_ct_font, kCTFontOrientationHorizontal, &m_glyph, &advance, 1);
            /* roundf instead of ceilf: ceil systematically inflates the cell by
             * up to 1 px on a font whose natural advance is e.g. 8.3, leaving
             * glyphs floating left-aligned in a too-wide cell. Round keeps the
             * cell within ±0.5 px of the true advance for even spacing. */
            atlas->cell_width = roundf((f32)advance.width);
        }
        f32 ct_ascent = (f32)CTFontGetAscent(g_ct_font);
        f32 ct_descent = (f32)CTFontGetDescent(g_ct_font);
        f32 ct_leading = (f32)CTFontGetLeading(g_ct_font);
        atlas->ascent = ceilf(ct_ascent);
        atlas->descent = floorf(ct_descent);
        atlas->line_height = ceilf(ct_ascent + ct_descent + ct_leading);
        atlas->cell_height = atlas->line_height;
        g_font_ascent = atlas->ascent;
        fprintf(stderr, "font: Core Text metrics: cell=%.0fx%.0f ascent=%.0f\n",
                atlas->cell_width, atlas->cell_height, atlas->ascent);
    }
#endif

    atlas->atlas_width = ATLAS_INIT_SIZE;
    atlas->atlas_height = ATLAS_INIT_SIZE;

#ifdef USE_METAL
    /* Metal: allocate CPU bitmap; texture created later via font_atlas_create_metal_texture */
    atlas->texture_id = 0;
    atlas->metal_texture = NULL;
    atlas->atlas_bitmap = calloc(1, (usize)(ATLAS_INIT_SIZE * ATLAS_INIT_SIZE) * ATLAS_BPP);

    /* Color emoji atlas (RGBA, 512x512) */
    atlas->color_atlas_w = 512;
    atlas->color_atlas_h = 512;
    atlas->color_atlas_x = 0;
    atlas->color_atlas_y = 0;
    atlas->color_row_h = 0;
    atlas->color_texture = NULL;
    atlas->color_bitmap = NULL; /* allocated on first emoji, or in create_metal_texture */
    atlas->metal_device = NULL;
#else
    /* Create atlas texture */
    glGenTextures(1, &atlas->texture_id);
    g_atlas_tex = atlas->texture_id;
    glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* Allocate empty texture (R8 single-channel coverage) */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_INIT_SIZE, ATLAS_INIT_SIZE,
                 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    atlas->atlas_bitmap = NULL;
#endif

    /* Pre-rasterize only essential — rest lazy on-demand */
    for (u32 cp = 32; cp < 127; cp++) rasterize_glyph(atlas, cp);       /* ASCII 95 glyphs */
    for (u32 cp = 0x2500; cp <= 0x257F; cp++) rasterize_glyph(atlas, cp); /* Box Drawing 128 */
    for (u32 cp = 0x2580; cp <= 0x259F; cp++) rasterize_glyph(atlas, cp); /* Block Elements 32 */
    /* Total: ~255 pre-cached, ~165 slots remaining for on-demand */

    /* ---- Load fallback fonts for missing glyphs ---- */
    free_fallbacks();
    g_pixel_h = pixel_h; /* cache for dynamic discovery */

#ifdef PLATFORM_MACOS
    /* Default fallback chain:
     * 1. Symbols Nerd Font (PRIORITY — we want PUA codepoints for filebrowser
     *    icons to resolve to this font before any system fallback picks up a
     *    neighbouring glyph by accident)
     * 2. Fonts from the local Liu fonts directory
     * 3. System monospace / symbol fallbacks */
    /* Eager: SymbolsNerdFontMono only — the filebrowser renders Nerd-Font PUA
     * icons on every first frame, so deferring it would cause a visible
     * rasterize stall. Everything else is queued for lazy load (see
     * flush_lazy_fallbacks), which typically never fires for ASCII-only
     * shell sessions → ~2-4 MB CoreText cache saved at idle. */
    {
        char nf[512];
        snprintf(nf, sizeof(nf), "%s/SymbolsNerdFontMono-Regular.ttf", font_user_dir());
        load_fallback(nf, pixel_h);
        snprintf(nf, sizeof(nf), "%s/JetBrainsMono-Regular.ttf", font_user_dir());
        queue_lazy_fallback(nf, pixel_h);
        snprintf(nf, sizeof(nf), "%s/FiraCode-Regular.ttf", font_user_dir());
        queue_lazy_fallback(nf, pixel_h);
        snprintf(nf, sizeof(nf), "%s/CascadiaCode.ttf", font_user_dir());
        queue_lazy_fallback(nf, pixel_h);
        snprintf(nf, sizeof(nf), "%s/Hack-Regular.ttf", font_user_dir());
        queue_lazy_fallback(nf, pixel_h);
        snprintf(nf, sizeof(nf), "%s/SourceCodePro-Regular.ttf", font_user_dir());
        queue_lazy_fallback(nf, pixel_h);
    }
    queue_lazy_fallback("/System/Library/Fonts/Menlo.ttc", pixel_h);
    queue_lazy_fallback("/System/Library/Fonts/SFNSText.ttf", pixel_h);
    queue_lazy_fallback("/System/Library/Fonts/SFNSTextItalic.ttf", pixel_h);
    queue_lazy_fallback("/Library/Fonts/Arial Unicode.ttf", pixel_h);
    queue_lazy_fallback("/System/Library/Fonts/Apple Symbols.ttf", pixel_h);
    /* Note: CJK, Arabic, Hebrew, Devanagari, etc. are discovered dynamically
     * via CTFontCreateForString() in discover_system_fallback_for_cp() */
#elif defined(PLATFORM_LINUX)
    /* Default fallback chain for Linux:
     * 1. Symbols Nerd Font (priority — PUA icons for filebrowser)
     * 2. Local Liu fonts directory
     * 3. System compatibility fallback */
    {
        char nf[512];
        snprintf(nf, sizeof(nf), "%s/SymbolsNerdFontMono-Regular.ttf", font_user_dir());
        load_fallback(nf, pixel_h);
        snprintf(nf, sizeof(nf), "%s/JetBrainsMono-Regular.ttf", font_user_dir());
        load_fallback(nf, pixel_h);
        snprintf(nf, sizeof(nf), "%s/FiraCode-Regular.ttf", font_user_dir());
        load_fallback(nf, pixel_h);
        snprintf(nf, sizeof(nf), "%s/CascadiaCode.ttf", font_user_dir());
        load_fallback(nf, pixel_h);
        snprintf(nf, sizeof(nf), "%s/Hack-Regular.ttf", font_user_dir());
        load_fallback(nf, pixel_h);
        snprintf(nf, sizeof(nf), "%s/SourceCodePro-Regular.ttf", font_user_dir());
        load_fallback(nf, pixel_h);
    }
    load_fallback("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", pixel_h);
    load_fallback("/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf", pixel_h);
    load_fallback("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", pixel_h);
    load_fallback("/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf", pixel_h);
    load_fallback("/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf", pixel_h);
    /* Additional paths for different distros */
    load_fallback("/usr/share/fonts/noto/NotoSansMono-Regular.ttf", pixel_h);
    load_fallback("/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf", pixel_h);
    load_fallback("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", pixel_h);
#endif

    /* Load user-configured fallback fonts (set via font_set_user_fallback_config) */
    for (i32 i = 0; i < g_user_fallback_count; i++) {
        if (g_user_fallback_paths[i][0]) {
            if (load_fallback(g_user_fallback_paths[i], pixel_h))
                fprintf(stderr, "font: loaded user fallback '%s'\n", g_user_fallback_paths[i]);
        }
    }

    fprintf(stderr, "font: %d fallback font(s) loaded (%d user)\n",
            g_fallback_count, g_user_fallback_count);

    atlas->loaded = true;
    fprintf(stderr, "font: loaded '%s' %.0fpt (cell: %.0fx%.0f, atlas: %dx%d)\n",
            font_path, font_size, atlas->cell_width, atlas->cell_height,
            atlas->atlas_width, atlas->atlas_height);

    /* kick off the background rasterizer for on-demand glyphs. */
    font_start_async_raster(atlas);
    return true;
}

void font_set_user_fallback_config(const char **paths, i32 count) {
    g_user_fallback_count = 0;
    memset(g_user_fallback_paths, 0, sizeof(g_user_fallback_paths));
    for (i32 i = 0; i < count && i < MAX_USER_FALLBACK_PATHS; i++) {
        if (paths[i] && paths[i][0]) {
            snprintf(g_user_fallback_paths[g_user_fallback_count], 512, "%s", paths[i]);
            g_user_fallback_count++;
        }
    }
}

void font_atlas_load_user_fallbacks(const char **paths, i32 count) {
    if (g_pixel_h <= 0) return;
    for (i32 i = 0; i < count; i++) {
        if (!paths[i] || !paths[i][0]) continue;
        if (load_fallback(paths[i], g_pixel_h)) {
            fprintf(stderr, "font: loaded user fallback '%s'\n", paths[i]);
        }
    }
}

void font_atlas_destroy(FontAtlas *atlas) {
    /* halt worker first; otherwise it would race with texture/
     * atlas state teardown and new requests vs new atlas dimensions. */
    font_stop_async_raster();
#ifdef USE_METAL
    if (atlas->metal_texture) {
        /* metal_texture holds a +2 ref: +1 from newTextureWithDescriptor: and
         * +1 from the explicit retain in font_atlas_create_metal_texture().
         * Release both so the texture is actually deallocated. */
        id tex = (__bridge id)atlas->metal_texture;
        [tex release];
        [tex release];
        atlas->metal_texture = NULL;
    }
#else
    if (atlas->texture_id) {
        glDeleteTextures(1, &atlas->texture_id);
        atlas->texture_id = 0;
        g_atlas_tex = 0;
    }
#endif
    if (atlas->atlas_bitmap) {
        free(atlas->atlas_bitmap);
        atlas->atlas_bitmap = NULL;
    }
#ifdef USE_METAL
    if (atlas->color_texture) {
        /* Matches the +1 from newTextureWithDescriptor: and the explicit
         * +1 retain in font_ensure_color_atlas_metal(). */
        id ctex = (__bridge id)atlas->color_texture;
        [ctex release];
        [ctex release];
        atlas->color_texture = NULL;
    }
    atlas->metal_device = NULL;
#endif
    if (atlas->color_bitmap) {
        free(atlas->color_bitmap);
        atlas->color_bitmap = NULL;
    }
    atlas->loaded = false;
    memset(g_cache, 0, sizeof(g_cache));
    memset(g_lig_cache, 0, sizeof(g_lig_cache));
    g_cache_used = 0;
    g_atlas_x = 0;
    g_atlas_y = 0;
    g_atlas_row_h = 0;
    free_fallbacks();
#ifdef PLATFORM_MACOS
    if (g_glyph_ctx) { CGContextRelease(g_glyph_ctx); g_glyph_ctx = NULL; }
    free(g_glyph_buf); g_glyph_buf = NULL;
    g_glyph_ctx_w = 0; g_glyph_ctx_h = 0;
#endif
}

#ifdef USE_METAL
void font_atlas_create_metal_texture(FontAtlas *atlas, void *device) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
    /* Glyph atlas is single-channel coverage (R8). The shader broadcasts the
     * red channel as alpha so the fragment can do gamma-correct fg * coverage
     * compositing against a linear-encoded foreground colour. */
    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
        width:atlas->atlas_width height:atlas->atlas_height mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:desc];
    /* newTextureWithDescriptor: returns +1 under MRC — retain one more time
     * so the texture stays alive even if the autorelease-pool enclosing this
     * call drains before the next frame binds it. font_atlas_destroy() does
     * the matching release below. */
    if (tex) [tex retain];
    atlas->metal_texture = (__bridge void *)tex;

    /* Bulk upload the pre-rasterized atlas bitmap, then free CPU copy */
    if (atlas->atlas_bitmap) {
        renderer_metal_upload_texture(atlas->metal_texture, 0, 0,
                                      atlas->atlas_width, atlas->atlas_height,
                                      atlas->atlas_bitmap);
        free(atlas->atlas_bitmap);
        atlas->atlas_bitmap = NULL;
    }

    /* Remember the device for lazy color-atlas creation; the emoji texture
     * and its CPU bitmap together cost ~1 MB — skip until we see the first
     * emoji glyph. font_ensure_color_atlas_metal() below performs the actual
     * upload-once-then-retain dance. */
    atlas->metal_device = device;
}

/* Lazy create the emoji RGBA atlas texture + CPU bitmap. Returns true if the
 * atlas is ready to accept emoji uploads (either already created or just now
 * built successfully). Called from rasterize_glyph() on the emoji path. */
bool font_ensure_color_atlas_metal(FontAtlas *atlas) {
    if (!atlas || atlas->color_atlas_w <= 0) return false;
    if (atlas->color_texture) return true;
    if (!atlas->metal_device) return false;

    id<MTLDevice> dev = (__bridge id<MTLDevice>)atlas->metal_device;
    MTLTextureDescriptor *cdesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
        width:atlas->color_atlas_w height:atlas->color_atlas_h mipmapped:NO];
    cdesc.usage = MTLTextureUsageShaderRead;
    cdesc.storageMode = MTLStorageModeShared;
    /* Apple Silicon GPUs apply lossless compression to shared textures by
     * default. Partial `replaceRegion:` updates (which is how we drop one
     * emoji at a time into the atlas) crash inside the driver's
     * CompressedTexturePartialMacroblockAccess path when the dirty region
     * doesn't sit on an internal macroblock boundary. Disabling the
     * optimization keeps the texture uncompressed and the per-glyph
     * uploads safe. */
    if ([cdesc respondsToSelector:@selector(setAllowGPUOptimizedContents:)]) {
        cdesc.allowGPUOptimizedContents = NO;
    }
    id<MTLTexture> ctex = [dev newTextureWithDescriptor:cdesc];
    if (!ctex) return false;
    [ctex retain];
    atlas->color_texture = (__bridge void *)ctex;
    fprintf(stderr, "font: color emoji atlas created on demand (%dx%d)\n",
            atlas->color_atlas_w, atlas->color_atlas_h);
    return true;
}
#endif
