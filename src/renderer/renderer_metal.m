#ifdef USE_METAL
/*
 * Liu - Metal renderer backend
 *
 * Replaces the OpenGL renderer with Metal for lower memory usage
 * (IOSurface reduction from ~78MB to ~35MB via CAMetalLayer).
 *
 * Rendering pipeline (back-to-front):
 *   Pass 0: Clear with terminal background color
 *   Pass 1: Cell backgrounds — non-default bg (rect batch)
 *   Pass 2: Cell glyphs — instanced alpha-blended glyph quads (single draw call)
 *   Pass 3: Decorations — cursor, underline, strikethrough, selection (rect batch)
 */

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <dispatch/dispatch.h>
#include "renderer/renderer.h"
#include "core/memory.h"
#include "terminal/terminal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <libgen.h>

/* =========================================================================
 * Internal Metal state
 * ========================================================================= */

typedef struct MetalUniforms_s {
    float projection[16];
    float cell_w, cell_h;
    float _pad[2];
} MetalUniforms;

#define METAL_FRAME_BUFFER_COUNT 3
#define METAL_IMAGE_CACHE_CAP 128
#define METAL_IMAGE_CACHE_BUDGET (64ull * 1024ull * 1024ull)
/* Reclaim a cached image texture after this many rendered frames without a
 * draw. Closed tabs/panes and scrolled-away images stop being drawn, so they
 * free promptly instead of lingering until 64 MiB LRU pressure evicts them.
 * Generous so an image briefly off-screen isn't re-uploaded on every scroll. */
#define METAL_IMAGE_CACHE_IDLE_EVICT 600

typedef struct {
    const void     *key;
    u64             generation;
    i32             width;
    i32             height;
    usize           bytes;
    u64             last_used_frame;
    id<MTLTexture>  texture;
} MetalImageCacheEntry;

typedef struct {
    id<MTLDevice>               device;
    id<MTLCommandQueue>         commandQueue;
    CAMetalLayer               *layer;
    id<MTLLibrary>              shaderLibrary;  /* retained for lazy pipeline compile */
    id<MTLRenderPipelineState>  glyphPipeline;
    id<MTLRenderPipelineState>  rectPipeline;
    id<MTLRenderPipelineState>  rrectPipeline;  /* SDF rounded rect + shadow */
    id<MTLRenderPipelineState>  linePipeline;   /* thick AA segments (graph edges) */
    id<MTLRenderPipelineState>  blurPipeline;   /* backdrop-blur sampling */
    id<MTLTexture>              blurSnapshot;   /* drawable copy for blur sampling */
    id<MTLTexture>              blurIntermediate; /* horizontal-blur target for 2-pass */
    bool                        blurValid;      /* set by capture, cleared each frame */
    f64                         blurLastUseSec; /* for idle release of full-screen textures */
    id<MTLSamplerState>         sampler;
    id<MTLTexture>              atlasTexture;
    id<CAMetalDrawable>         currentDrawable;
    id<MTLCommandBuffer>        currentCommandBuffer;
    id<MTLRenderCommandEncoder> currentEncoder;
    /* Compute pipeline for GPU-driven cell→vertex conversion */
    id<MTLComputePipelineState> computeGlyphPipeline;
    id<MTLComputePipelineState> computeBgRectPipeline;
    /* Background image pipeline */
    id<MTLRenderPipelineState>  bgImagePipeline;
    id<MTLTexture>              bgImageTexture;
    id<MTLSamplerState>         bgImageSampler;
    id<MTLBuffer>               frameBuffers[METAL_FRAME_BUFFER_COUNT];
    usize                       frameBufferSizes[METAL_FRAME_BUFFER_COUNT];
    usize                       frameBufferOffset;
    u32                         frameBufferIndex;
    u64                         frameSerial;
    MetalImageCacheEntry        imageCache[METAL_IMAGE_CACHE_CAP];
    usize                       imageCacheBytes;
    /* Cached uniforms for flush hot path; rebuilt when Renderer.uniforms_version
     * changes or when cw/ch differs from the last bake. */
    MetalUniforms               cachedUniforms;
    u32                         cachedUniformsVersion;
    /* Inter-frame timing (diagnostics) */
    f64 last_frame_time;
    f64 last_frame_duration_ms;
    u32 dropped_frame_count;
    /* CPU/GPU frame-pacing fence. Initialized to METAL_FRAME_BUFFER_COUNT so the
     * CPU may run up to that many frames ahead of the GPU before begin_frame
     * blocks. Every successful wait at the top of renderer_begin_frame is
     * balanced by exactly one signal — either from the command buffer's
     * completed handler (committed path) or inline on a path that never
     * commits (early returns, blocked end_frame). frameSlotHeld tracks whether
     * the current CPU frame still owes a signal, so we never double-signal nor
     * leak a slot. */
    dispatch_semaphore_t frameSem;
    bool                 frameSlotHeld;
} RendererMetal;

/* Batch constants */
#define MAX_GLYPHS  16384
#define MAX_RECTS    8192
#define MAX_RRECTS   1024   /* rounded rects are rare (modals, tabs, cards) */
#define INIT_GLYPHS  2048
#define INIT_RECTS   1024
#define INIT_RRECTS   128
#define MAX_LINES   16384
#define INIT_LINES   1024
#define RECT_FPV       6   /* floats per vertex: pos(2) + color(4) */
#define RECT_VPR       6   /* vertices per rect (2 triangles) */

/* Unit quad for glyph instancing (6 vertices, 2 floats each) */
static const float s_quad[] = { 0,0, 1,0, 1,1, 0,0, 1,1, 0,1 };

static f64 metal_monotonic_sec(void) {
    static mach_timebase_info_data_t tb = {0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    u64 now = mach_absolute_time();
    return ((f64)now * (f64)tb.numer / (f64)tb.denom) / 1e9;
}

static bool renderer_reserve_glyphs(Renderer *r, u32 need) {
    if (r->glyph_cap >= need) return true;
    u32 cap = r->glyph_cap ? r->glyph_cap : INIT_GLYPHS;
    while (cap < need && cap < MAX_GLYPHS) cap *= 2;
    if (cap > MAX_GLYPHS) cap = MAX_GLYPHS;
    if (cap < need) return false;
    GlyphInstance *p = realloc(r->glyph_instances, (usize)cap * sizeof(*p));
    if (!p) return false;
    r->glyph_instances = p;
    r->glyph_cap = cap;
    return true;
}

static bool renderer_reserve_rects(Renderer *r, u32 need) {
    if (r->rect_batch_cap >= need) return true;
    u32 cap = r->rect_batch_cap ? r->rect_batch_cap : INIT_RECTS;
    while (cap < need && cap < MAX_RECTS) cap *= 2;
    if (cap > MAX_RECTS) cap = MAX_RECTS;
    if (cap < need) return false;
    RectInstance *p = realloc(r->rect_batch, (usize)cap * sizeof(*p));
    if (!p) return false;
    r->rect_batch = p;
    r->rect_batch_cap = cap;
    return true;
}

static bool renderer_reserve_rrects(Renderer *r, u32 need) {
    if (r->rrect_batch_cap >= need) return true;
    u32 cap = r->rrect_batch_cap ? r->rrect_batch_cap : INIT_RRECTS;
    while (cap < need && cap < MAX_RRECTS) cap *= 2;
    if (cap > MAX_RRECTS) cap = MAX_RRECTS;
    if (cap < need) return false;
    RRectInstance *p = realloc(r->rrect_batch, (usize)cap * sizeof(*p));
    if (!p) return false;
    r->rrect_batch = p;
    r->rrect_batch_cap = cap;
    return true;
}

static bool renderer_reserve_lines(Renderer *r, u32 need) {
    if (r->line_batch_cap >= need) return true;
    u32 cap = r->line_batch_cap ? r->line_batch_cap : INIT_LINES;
    while (cap < need && cap < MAX_LINES) cap *= 2;
    if (cap > MAX_LINES) cap = MAX_LINES;
    if (cap < need) return false;
    LineInstance *p = realloc(r->line_batch, (usize)cap * sizeof(*p));
    if (!p) return false;
    r->line_batch = p;
    r->line_batch_cap = cap;
    return true;
}

/* =========================================================================
 * ANSI 256 color palette
 * ========================================================================= */

Color g_ansi_colors[256];

void renderer_init_colors(void) {
    /* Standard 16 */
    g_ansi_colors[0]  = COLOR_RGB(0, 0, 0);
    g_ansi_colors[1]  = COLOR_RGB(205, 49, 49);
    g_ansi_colors[2]  = COLOR_RGB(13, 188, 121);
    g_ansi_colors[3]  = COLOR_RGB(229, 229, 16);
    g_ansi_colors[4]  = COLOR_RGB(36, 114, 200);
    g_ansi_colors[5]  = COLOR_RGB(188, 63, 188);
    g_ansi_colors[6]  = COLOR_RGB(17, 168, 205);
    g_ansi_colors[7]  = COLOR_RGB(229, 229, 229);
    g_ansi_colors[8]  = COLOR_RGB(102, 102, 102);
    g_ansi_colors[9]  = COLOR_RGB(241, 76, 76);
    g_ansi_colors[10] = COLOR_RGB(35, 209, 139);
    g_ansi_colors[11] = COLOR_RGB(245, 245, 67);
    g_ansi_colors[12] = COLOR_RGB(59, 142, 234);
    g_ansi_colors[13] = COLOR_RGB(214, 112, 214);
    g_ansi_colors[14] = COLOR_RGB(41, 184, 219);
    g_ansi_colors[15] = COLOR_RGB(255, 255, 255);
    /* 216 color cube */
    for (int i = 0; i < 216; i++) {
        int r = (i / 36) % 6, g = (i / 6) % 6, b = i % 6;
        g_ansi_colors[16 + i] = COLOR_RGB(r ? 55+r*40 : 0, g ? 55+g*40 : 0, b ? 55+b*40 : 0);
    }
    /* 24 greyscale */
    for (int i = 0; i < 24; i++) {
        int v = 8 + i * 10;
        g_ansi_colors[232 + i] = COLOR_RGB(v, v, v);
    }
}

/* =========================================================================
 * Orthographic projection (matches GL version exactly)
 * ========================================================================= */

static void ortho(f32 *m, f32 w, f32 h) {
    memset(m, 0, 16 * sizeof(f32));
    m[0]  =  2.0f / w;
    m[5]  = -2.0f / h;
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static MetalUniforms metal_build_uniforms(Renderer *r, f32 cw, f32 ch) {
    RendererMetal *m = r->backend;
    /* hot path — return cached uniforms when (version, cw, ch)
     * match what was last baked in. Only the common "use default font cell
     * size" flush path hits this; UI scale overrides fall through. */
    if (m && m->cachedUniformsVersion == r->uniforms_version &&
        m->cachedUniforms.cell_w == cw && m->cachedUniforms.cell_h == ch) {
        MetalUniforms u;
        memcpy(&u, &m->cachedUniforms, sizeof(u));
        return u;
    }
    MetalUniforms u;
    memcpy(u.projection, r->projection, sizeof(u.projection));
    u.cell_w = cw;
    u.cell_h = ch;
    u._pad[0] = 0.0f;
    u._pad[1] = 0.0f;
    if (m) {
        memcpy(&m->cachedUniforms, &u, sizeof(u));
        m->cachedUniformsVersion = r->uniforms_version;
    }
    return u;
}

/* Upload vertex data: setVertexBytes for small data, per-frame ring buffers
 * for large batches. The ring avoids per-flush MTLBuffer allocation while
 * keeping previous in-flight frames untouched. */
static usize metal_align_up(usize value, usize align) {
    return (value + align - 1) & ~(align - 1);
}

static bool metal_frame_alloc(RendererMetal *m, usize bytes, usize align,
                              id<MTLBuffer> *out_buf, NSUInteger *out_offset,
                              void **out_ptr) {
    if (!m || bytes == 0) return false;
    u32 idx = m->frameBufferIndex % METAL_FRAME_BUFFER_COUNT;
    usize offset = metal_align_up(m->frameBufferOffset, align);
    usize need = offset + bytes;
    if (!m->frameBuffers[idx] || m->frameBufferSizes[idx] < need) {
        usize new_size = m->frameBufferSizes[idx] ? m->frameBufferSizes[idx] : (usize)(4 * 1024 * 1024);
        while (new_size < need) new_size *= 2;
        id<MTLBuffer> new_buf = [m->device newBufferWithLength:(NSUInteger)new_size
                                                       options:MTLResourceStorageModeShared];
        if (!new_buf) return false;
        if (m->frameBuffers[idx]) [m->frameBuffers[idx] release];
        m->frameBuffers[idx] = new_buf;
        m->frameBufferSizes[idx] = new_size;
    }
    *out_buf = m->frameBuffers[idx];
    *out_offset = (NSUInteger)offset;
    *out_ptr = (u8 *)[m->frameBuffers[idx] contents] + offset;
    m->frameBufferOffset = need;
    return true;
}

static void metal_set_vertex_data(RendererMetal *m, id<MTLRenderCommandEncoder> enc,
                                  const void *data, usize bytes, NSUInteger idx) {
    if (bytes <= 4096) {
        [enc setVertexBytes:data length:bytes atIndex:idx];
    } else {
        id<MTLBuffer> buf = nil;
        NSUInteger offset = 0;
        void *dst = NULL;
        if (metal_frame_alloc(m, bytes, 256, &buf, &offset, &dst)) {
            memcpy(dst, data, bytes);
            [enc setVertexBuffer:buf offset:offset atIndex:idx];
        } else {
            id<MTLBuffer> tmp = [m->device newBufferWithBytes:data length:bytes
                                                      options:MTLResourceStorageModeShared];
            if (tmp) {
                [enc setVertexBuffer:tmp offset:0 atIndex:idx];
                [tmp release];
            }
        }
    }
}

/* =========================================================================
 * Init / Destroy
 * ========================================================================= */

bool renderer_init(Renderer *r, f32 dpi_scale) {
    memset(r, 0, sizeof(*r));
    r->dpi_scale = dpi_scale;
    r->backend = NULL;
    renderer_init_colors();

    /* Default clear color (black) */
    r->clear_color[0] = 0.0f;
    r->clear_color[1] = 0.0f;
    r->clear_color[2] = 0.0f;
    r->clear_color[3] = 1.0f;

    /* CPU-side instance buffers grow on demand. The old eager max-size
     * allocation cost ~1.1 MiB at idle before a single glyph was drawn. */
    if (!renderer_reserve_glyphs(r, INIT_GLYPHS)) return false;
    if (!renderer_reserve_rects(r, INIT_RECTS)) return false;
    if (!renderer_reserve_rrects(r, INIT_RRECTS)) return false;

    return r->glyph_instances && r->rect_batch && r->rrect_batch;
}

void renderer_set_gpu(Renderer *r, void *device, void *layer, void *queue) {
    @autoreleasepool {
        RendererMetal *m = calloc(1, sizeof(RendererMetal));
        if (!m) {
            fprintf(stderr, "renderer_metal: failed to allocate backend\n");
            return;
        }

        m->device       = (__bridge id<MTLDevice>)device;
        m->commandQueue = (__bridge id<MTLCommandQueue>)queue;
        m->layer        = (__bridge CAMetalLayer *)layer;

        /* --- Load .metallib from executable directory --- */
        char exe_path[4096];
        u32 exe_size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &exe_size) != 0) {
            fprintf(stderr, "renderer_metal: cannot get executable path\n");
            free(m);
            return;
        }
        char *dir = dirname(exe_path);
        char lib_path[4096];
        snprintf(lib_path, sizeof(lib_path), "%s/LiuShaders.metallib", dir);

        NSString *libPathStr = [NSString stringWithUTF8String:lib_path];
        NSURL *libURL = [NSURL fileURLWithPath:libPathStr];
        NSError *error = nil;
        id<MTLLibrary> library = [m->device newLibraryWithURL:libURL error:&error];
        if (!library) {
            fprintf(stderr, "renderer_metal: failed to load metallib: %s\n",
                    error.localizedDescription.UTF8String);
            free(m);
            return;
        }
        /* Retain the library so the bg-image pipeline can be compiled later
         * (deferred until renderer_load_background_image is actually called). */
        m->shaderLibrary = [library retain];

        /* --- Glyph pipeline --- */
        id<MTLFunction> glyphVert = [library newFunctionWithName:@"glyph_vertex"];
        id<MTLFunction> glyphFrag = [library newFunctionWithName:@"glyph_fragment"];
        if (!glyphVert || !glyphFrag) {
            fprintf(stderr, "renderer_metal: missing glyph shader functions\n");
            [library release];
            if (glyphVert) [glyphVert release];
            if (glyphFrag) [glyphFrag release];
            free(m);
            return;
        }

        /* Standard premultiplied alpha blending:
         *   out.rgb = src.rgb + dst.rgb * (1 - src.a)
         *   out.a   = src.a   + dst.a   * (1 - src.a)
         * Shader emits premultiplied (fg*cov, cov) — gamma-correct because
         * fg arrives linear and the sRGB framebuffer encodes on write. */
        MTLRenderPipelineDescriptor *glyphDesc = [[MTLRenderPipelineDescriptor alloc] init];
        glyphDesc.vertexFunction   = glyphVert;
        glyphDesc.fragmentFunction = glyphFrag;
        glyphDesc.colorAttachments[0].pixelFormat     = MTLPixelFormatBGRA8Unorm_sRGB;
        glyphDesc.colorAttachments[0].blendingEnabled  = YES;
        glyphDesc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
        glyphDesc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        glyphDesc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
        glyphDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        m->glyphPipeline = [m->device newRenderPipelineStateWithDescriptor:glyphDesc error:&error];
        [glyphDesc release];
        [glyphVert release];
        [glyphFrag release];
        if (!m->glyphPipeline) {
            fprintf(stderr, "renderer_metal: glyph pipeline: %s\n",
                    error.localizedDescription.UTF8String);
            [library release];
            free(m);
            return;
        }

        /* --- Rect pipeline --- */
        id<MTLFunction> rectVert = [library newFunctionWithName:@"rect_vertex"];
        id<MTLFunction> rectFrag = [library newFunctionWithName:@"rect_fragment"];
        if (!rectVert || !rectFrag) {
            fprintf(stderr, "renderer_metal: missing rect shader functions\n");
            [library release];
            if (rectVert) [rectVert release];
            if (rectFrag) [rectFrag release];
            [m->glyphPipeline release];
            free(m);
            return;
        }

        MTLRenderPipelineDescriptor *rectDesc = [[MTLRenderPipelineDescriptor alloc] init];
        rectDesc.vertexFunction   = rectVert;
        rectDesc.fragmentFunction = rectFrag;
        rectDesc.colorAttachments[0].pixelFormat     = MTLPixelFormatBGRA8Unorm_sRGB;
        rectDesc.colorAttachments[0].blendingEnabled  = YES;
        rectDesc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
        rectDesc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        /* Alpha channel: take MAX(src, dst) so overlay rects with alpha<1 cannot
         * reduce framebuffer alpha below what opaque chrome already wrote. Without
         * this, the macOS compositor sees < 1.0 alpha at the title bar (after a
         * dim/modal layer is drawn over it) and bleeds the desktop through. */
        rectDesc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
        rectDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        rectDesc.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationMax;

        m->rectPipeline = [m->device newRenderPipelineStateWithDescriptor:rectDesc error:&error];
        [rectDesc release];
        [rectVert release];
        [rectFrag release];
        if (!m->rectPipeline) {
            [library release];
            fprintf(stderr, "renderer_metal: rect pipeline: %s\n",
                    error.localizedDescription.UTF8String);
            [m->glyphPipeline release];
            free(m);
            return;
        }

        /* --- Line pipeline (graph-view edges) --- non-fatal if absent --- */
        id<MTLFunction> lineVert = [library newFunctionWithName:@"line_vertex"];
        id<MTLFunction> lineFrag = [library newFunctionWithName:@"line_fragment"];
        if (lineVert && lineFrag) {
            MTLRenderPipelineDescriptor *lineDesc = [[MTLRenderPipelineDescriptor alloc] init];
            lineDesc.vertexFunction   = lineVert;
            lineDesc.fragmentFunction = lineFrag;
            lineDesc.colorAttachments[0].pixelFormat     = MTLPixelFormatBGRA8Unorm_sRGB;
            lineDesc.colorAttachments[0].blendingEnabled  = YES;
            lineDesc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
            lineDesc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
            lineDesc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
            lineDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
            lineDesc.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationMax;
            m->linePipeline = [m->device newRenderPipelineStateWithDescriptor:lineDesc error:&error];
            [lineDesc release];
        }
        if (lineVert) [lineVert release];
        if (lineFrag) [lineFrag release];

        /* --- Backdrop blur pipeline (separable Gaussian sampler) ---
         * The shader is in LiuShaders.metal::blur_*. If the functions are
         * missing (older library), the pipeline stays nil and the C API
         * falls back to plain tinted rrects. */
        id<MTLFunction> blurVert = [library newFunctionWithName:@"blur_vertex"];
        id<MTLFunction> blurFrag = [library newFunctionWithName:@"blur_fragment"];
        if (blurVert && blurFrag) {
            MTLRenderPipelineDescriptor *blurDesc = [[MTLRenderPipelineDescriptor alloc] init];
            blurDesc.vertexFunction   = blurVert;
            blurDesc.fragmentFunction = blurFrag;
            blurDesc.colorAttachments[0].pixelFormat     = MTLPixelFormatBGRA8Unorm_sRGB;
            blurDesc.colorAttachments[0].blendingEnabled  = YES;
            /* Premul output: src.rgb already includes outer_mask. */
            blurDesc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
            blurDesc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
            blurDesc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
            blurDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
            blurDesc.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationMax;
            m->blurPipeline = [m->device newRenderPipelineStateWithDescriptor:blurDesc error:&error];
            [blurDesc release];
            if (!m->blurPipeline) {
                fprintf(stderr, "renderer_metal: blur pipeline failed (%s) — flat-tint fallback\n",
                        error.localizedDescription.UTF8String);
            }
        }
        if (blurVert) [blurVert release];
        if (blurFrag) [blurFrag release];

        /* --- Rounded-rect pipeline (SDF + soft shadow) --- */
        id<MTLFunction> rrectVert = [library newFunctionWithName:@"rrect_vertex"];
        id<MTLFunction> rrectFrag = [library newFunctionWithName:@"rrect_fragment"];
        if (rrectVert && rrectFrag) {
            MTLRenderPipelineDescriptor *rrectDesc = [[MTLRenderPipelineDescriptor alloc] init];
            rrectDesc.vertexFunction   = rrectVert;
            rrectDesc.fragmentFunction = rrectFrag;
            rrectDesc.colorAttachments[0].pixelFormat     = MTLPixelFormatBGRA8Unorm_sRGB;
            rrectDesc.colorAttachments[0].blendingEnabled  = YES;
            /* Premultiplied alpha output: shader emits (rgb*a, a). */
            rrectDesc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
            rrectDesc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
            rrectDesc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
            rrectDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
            rrectDesc.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationMax;

            m->rrectPipeline = [m->device newRenderPipelineStateWithDescriptor:rrectDesc error:&error];
            [rrectDesc release];
            if (!m->rrectPipeline) {
                fprintf(stderr, "renderer_metal: rrect pipeline failed (%s) — falling back to flat rects\n",
                        error.localizedDescription.UTF8String);
            }
        } else {
            fprintf(stderr, "renderer_metal: rrect shader functions missing — flat-rect fallback\n");
        }
        if (rrectVert) [rrectVert release];
        if (rrectFrag) [rrectFrag release];

        /* --- Sampler (linear filtering for smooth glyph edges on Retina) --- */
        MTLSamplerDescriptor *sampDesc = [[MTLSamplerDescriptor alloc] init];
        sampDesc.minFilter    = MTLSamplerMinMagFilterLinear;
        sampDesc.magFilter    = MTLSamplerMinMagFilterLinear;
        sampDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sampDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        m->sampler = [m->device newSamplerStateWithDescriptor:sampDesc];
        [sampDesc release];

        /* --- Compute pipelines (GPU-driven cell→vertex) ---
         * DEFERRED: the compute path is compiled source in LiuShaders.metal
         * but never actually dispatched at runtime — the CPU multi-pass path
         * wins until indirect-draw is implemented (see renderer_metal.m:801
         * comment). Skip compilation to save ~2-4 MB of Metal state. The
         * library `newFunctionWithName` lookup alone would retain a compiled
         * function object too, so we don't even fetch them. */
        m->computeGlyphPipeline  = nil;
        m->computeBgRectPipeline = nil;

        /* --- Background image pipeline ---
         * DEFERRED: only needed when the user sets config.background_image.
         * Lazy-compiled by renderer_load_background_image() below — saves
         * ~1 MB of pipeline state for the default (no-bg-image) config. */
        m->bgImagePipeline = nil;

        /* bgImageSampler is allocated inside ensure_bg_image_pipeline() on
         * first use — avoids ~2 KB of sampler state + descriptor at idle. */

        [library release];

        /* CPU/GPU frame fence — allows up to METAL_FRAME_BUFFER_COUNT frames of
         * CPU lead before begin_frame blocks on the GPU finishing the oldest
         * in-flight frame. This throttles writers of the per-frame ring buffers
         * (frameBuffers[]) and the in-place glyph atlas to <= N frames ahead,
         * eliminating torn glyphs / flicker under sustained animation. */
        m->frameSem = dispatch_semaphore_create(METAL_FRAME_BUFFER_COUNT);
        m->frameSlotHeld = false;

        r->backend = m;
        fprintf(stderr, "renderer_metal: GPU backend initialized\n");
    }
}

void renderer_set_clear_color(Renderer *r, f32 red, f32 green, f32 blue, f32 alpha) {
    /* Framebuffer attachment is _sRGB — Metal interprets the clear color as
     * linear and encodes to sRGB on write. Feed it linear. */
    r->clear_color[0] = srgb_decode(red);
    r->clear_color[1] = srgb_decode(green);
    r->clear_color[2] = srgb_decode(blue);
    r->clear_color[3] = alpha;
}

void renderer_destroy(Renderer *r) {
    if (r->backend) {
        RendererMetal *m = r->backend;
        /* Drain committed GPU work before freeing `m`: the scheduled handler
         * attached in renderer_end_frame captures raw pointers INTO `m`, so a
         * handler firing after free(m) would be a use-after-free. Submit an
         * empty command buffer on the serial queue and wait — its completion
         * guarantees every previously committed frame's handler has run. We do
         * NOT wait on currentCommandBuffer: at teardown it is normally nil, and
         * an uncommitted buffer would never complete (would hang). */
        if (m->commandQueue) {
            id<MTLCommandBuffer> drain = [m->commandQueue commandBuffer];
            if (drain) { [drain commit]; [drain waitUntilCompleted]; }
        }
        /* Release owned Metal objects (MRR — no ARC) */
        [m->glyphPipeline release];
        [m->rectPipeline release];
        if (m->rrectPipeline) [m->rrectPipeline release];
        if (m->linePipeline) [m->linePipeline release];
        if (m->blurPipeline) [m->blurPipeline release];
        if (m->blurSnapshot) [m->blurSnapshot release];
        if (m->blurIntermediate) [m->blurIntermediate release];
        [m->sampler release];
        if (m->shaderLibrary) [m->shaderLibrary release];
        if (m->computeGlyphPipeline) [m->computeGlyphPipeline release];
        if (m->computeBgRectPipeline) [m->computeBgRectPipeline release];
        if (m->bgImagePipeline) [m->bgImagePipeline release];
        if (m->bgImageTexture) [m->bgImageTexture release];
        if (m->bgImageSampler) [m->bgImageSampler release];
        for (u32 i = 0; i < METAL_FRAME_BUFFER_COUNT; i++) {
            if (m->frameBuffers[i]) [m->frameBuffers[i] release];
        }
        for (u32 i = 0; i < METAL_IMAGE_CACHE_CAP; i++) {
            if (m->imageCache[i].texture) [m->imageCache[i].texture release];
        }
        /* atlasTexture is a borrowed ref — font owns it */
        /* device, commandQueue, layer are borrowed — not retained by us */
        free(m);
        r->backend = NULL;
    }
    free(r->glyph_instances);
    free(r->rect_batch);
    free(r->rrect_batch);
    free(r->line_batch);
    r->glyph_instances = NULL;
    r->rect_batch = NULL;
    r->rrect_batch = NULL;
    r->line_batch = NULL;
    font_atlas_destroy(&r->font);
}

/* =========================================================================
 * Frame
 * ========================================================================= */

static void metal_image_cache_release_entry(RendererMetal *m, MetalImageCacheEntry *e);

void renderer_begin_frame(Renderer *r, i32 w, i32 h) {
    r->screen_width  = w;
    r->screen_height = h;
    r->glyph_count   = 0;
    r->rect_batch_count = 0;
    r->rrect_batch_count = 0;

    RendererMetal *m = r->backend;
    if (m) m->blurValid = false;
    if (!m) return;
    m->frameSerial++;

    /* Idle eviction: free image textures not drawn for a while (closed tabs,
     * scrolled-away images). Cheap 128-slot sweep; the 64 MiB LRU still caps
     * the live set, this just reclaims sooner. frameSerial only advances when
     * we render, so a fully idle app never churns the cache. */
    for (u32 i = 0; i < METAL_IMAGE_CACHE_CAP; i++) {
        MetalImageCacheEntry *e = &m->imageCache[i];
        if (e->texture &&
            m->frameSerial - e->last_used_frame > METAL_IMAGE_CACHE_IDLE_EVICT) {
            metal_image_cache_release_entry(m, e);
        }
    }

    /* Acquire one in-flight slot. Blocks if METAL_FRAME_BUFFER_COUNT frames are
     * already outstanding on the GPU, throttling CPU writes to the per-frame
     * ring + glyph atlas. From here until end_frame (or an early return below),
     * this frame owes exactly one signal, tracked by frameSlotHeld. */
    if (m->frameSem) {
        dispatch_semaphore_wait(m->frameSem, DISPATCH_TIME_FOREVER);
        m->frameSlotHeld = true;
    }

    /* Rotate the ring slot ONLY after the semaphore gate. The fence guarantees
     * at most METAL_FRAME_BUFFER_COUNT frames are in flight, so advancing the
     * index here ensures the slot we are about to overwrite belongs to a frame
     * whose GPU read already completed (its completion handler signalled the
     * semaphore we just waited on). Rotating before the wait would let the CPU
     * pick the next buffer before a slot was confirmed free, allowing a rewrite
     * while the GPU still reads the previous frame's copy. */
    m->frameBufferIndex = (m->frameBufferIndex + 1) % METAL_FRAME_BUFFER_COUNT;
    m->frameBufferOffset = 0;

    @autoreleasepool {
        /* Match the layer's drawable size to the render dimensions BEFORE
         * acquiring the drawable, so the drawable texture is the size we are
         * about to render — preventing a stale/oversized drawable for one frame
         * during interactive resize (split divider / sidebar / window edge). The
         * inequality guard keeps this a no-op on steady-state frames; on a
         * presentsWithTransaction layer (set during split/sidebar drag) this size
         * commit rides the same CA transaction as the present, so resize is
         * atomic. */
        if (m->layer) {
            CGSize _backing = CGSizeMake((CGFloat)w, (CGFloat)h);
            if (!CGSizeEqualToSize(m->layer.drawableSize, _backing))
                m->layer.drawableSize = _backing;
        }
        m->currentDrawable = [[m->layer nextDrawable] retain];
        if (!m->currentDrawable) {
            if (m->frameSlotHeld) { dispatch_semaphore_signal(m->frameSem); m->frameSlotHeld = false; }
            return;
        }

        m->currentCommandBuffer = [[m->commandQueue commandBuffer] retain];
        if (!m->currentCommandBuffer) {
            [m->currentDrawable release]; m->currentDrawable = nil;
            if (m->frameSlotHeld) { dispatch_semaphore_signal(m->frameSem); m->frameSlotHeld = false; }
            return;
        }

        MTLRenderPassDescriptor *passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        passDesc.colorAttachments[0].texture    = m->currentDrawable.texture;
        passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
        passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
        passDesc.colorAttachments[0].clearColor = MTLClearColorMake(
            r->clear_color[0], r->clear_color[1],
            r->clear_color[2], r->clear_color[3]);

        m->currentEncoder = [[m->currentCommandBuffer renderCommandEncoderWithDescriptor:passDesc] retain];
        if (!m->currentEncoder) {
            [m->currentCommandBuffer release]; m->currentCommandBuffer = nil;
            [m->currentDrawable release]; m->currentDrawable = nil;
            if (m->frameSlotHeld) { dispatch_semaphore_signal(m->frameSem); m->frameSlotHeld = false; }
            return;
        }

        /* Set viewport to drawable size (framebuffer pixels) */
        MTLViewport viewport = {
            .originX = 0, .originY = 0,
            .width  = (double)w,
            .height = (double)h,
            .znear  = 0.0, .zfar = 1.0
        };
        [m->currentEncoder setViewport:viewport];
    }

    /* Only recompute projection / bump uniform version on change. */
    f32 cell_w = r->font.loaded ? r->font.cell_width  : 0.0f;
    f32 cell_h = r->font.loaded ? r->font.cell_height : 0.0f;
    if (w != r->cached_proj_w || h != r->cached_proj_h) {
        ortho(r->projection, (f32)w, (f32)h);
        r->cached_proj_w = w;
        r->cached_proj_h = h;
        r->uniforms_version++;
    }
    if (cell_w != r->cached_cell_w || cell_h != r->cached_cell_h) {
        r->cached_cell_w = cell_w;
        r->cached_cell_h = cell_h;
        r->uniforms_version++;
    }
}

void renderer_end_frame(Renderer *r) {
    /* Flush any remaining batched draws.
     * Order matters for visual layering:
     *   rects   → flat backgrounds (cell bg, dim backdrop)
     *   rrects  → rounded panels/tabs/cards layered on top of plain rects
     *   glyphs  → text layered on top of everything else */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);

    RendererMetal *m = r->backend;
    if (!m) return;

    if (m->currentEncoder) {
        [m->currentEncoder endEncoding];
        [m->currentEncoder release];
        m->currentEncoder = nil;
    }

    /* Attach the fence-release completed handler to whatever command buffer we
     * are about to commit. The handler signals the semaphore exactly once when
     * the GPU finishes this frame, releasing the in-flight slot acquired in
     * begin_frame. Capture the semaphore by value (a retained dispatch object
     * pointer) rather than `m`, so the block does NOT keep `m` alive and is
     * safe even if the renderer is torn down — the handler touches only the
     * semaphore, never `m`'s fields. We clear frameSlotHeld up-front because
     * the slot's release is now owned by the handler (or by the inline signal
     * on the no-command-buffer path below). */
    bool slotOwedToHandler = m->frameSlotHeld;
    m->frameSlotHeld = false;
    dispatch_semaphore_t fenceSem = m->frameSem;

    if (m->currentDrawable) {
        /* Frame pacing: track inter-frame timing */
        static mach_timebase_info_data_t timebase = {0};
        if (timebase.denom == 0) mach_timebase_info(&timebase);

        if (slotOwedToHandler && fenceSem) {
            [m->currentCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull cb) {
                (void)cb;
                dispatch_semaphore_signal(fenceSem);
            }];
            slotOwedToHandler = false; /* present path: handler owns the release */
        }

        /* If layer uses presentsWithTransaction, we must waitUntilScheduled then present.
         * Otherwise normal async present via command buffer with scheduled handler
         * for frame timing diagnostics. */
        if (m->layer.presentsWithTransaction) {
            [m->currentCommandBuffer commit];
            [m->currentCommandBuffer waitUntilScheduled];
            [m->currentDrawable present];
        } else {
            [m->currentCommandBuffer presentDrawable:m->currentDrawable];
            /* Track frame pacing via scheduled handler */
            __block f64 *last_ft = &m->last_frame_time;
            __block f64 *last_dur = &m->last_frame_duration_ms;
            __block u32 *dropped = &m->dropped_frame_count;
            mach_timebase_info_data_t tb = timebase;
            [m->currentCommandBuffer addScheduledHandler:^(id<MTLCommandBuffer> _Nonnull cb) {
                (void)cb;
                u64 now = mach_absolute_time();
                if (*last_ft > 0) {
                    f64 delta_ns = (f64)(now - (u64)*last_ft) * (f64)tb.numer / (f64)tb.denom;
                    *last_dur = delta_ns / 1e6;  /* convert to ms */
                    if (*last_dur > 16.7) (*dropped)++;
                }
                *last_ft = (f64)now;
            }];
            [m->currentCommandBuffer commit];
        }
    } else if (m->currentCommandBuffer) {
        if (slotOwedToHandler && fenceSem) {
            [m->currentCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull cb) {
                (void)cb;
                dispatch_semaphore_signal(fenceSem);
            }];
            slotOwedToHandler = false; /* handler now owns the release */
        }
        [m->currentCommandBuffer commit];
    }
    if (m->currentCommandBuffer) {
        [m->currentCommandBuffer release];
        m->currentCommandBuffer = nil;
    }

    if (m->currentDrawable) {
        [m->currentDrawable release];
        m->currentDrawable = nil;
    }

    /* Safety net: if we still owe a slot release but never attached a handler
     * (no command buffer at all to carry it — should not happen when
     * begin_frame succeeded, but covers any future early-out), release inline
     * so the fence cannot leak a permanent slot and eventually deadlock. */
    if (slotOwedToHandler && fenceSem) {
        dispatch_semaphore_signal(fenceSem);
        slotOwedToHandler = false;
    }
}

void renderer_trim_idle_resources(Renderer *r, f64 now_sec) {
    (void)now_sec;
    RendererMetal *m = r ? r->backend : NULL;
    if (!m) return;
    if (!m->blurSnapshot && !m->blurIntermediate) return;

    /* These are full-frame private textures. Keeping them while a modal is
     * open avoids churn; once blur has been unused for a few seconds, freeing
     * them gives macOS/Metal back the resident IOSurface/VRAM without changing
     * the next modal's visual result. */
    f64 now = metal_monotonic_sec();
    if (m->blurLastUseSec <= 0.0 || now - m->blurLastUseSec < 3.0) return;
    if (m->blurSnapshot) {
        [m->blurSnapshot release];
        m->blurSnapshot = nil;
    }
    if (m->blurIntermediate) {
        [m->blurIntermediate release];
        m->blurIntermediate = nil;
    }
    m->blurValid = false;
}

/* =========================================================================
 * Pass 1 + 3: Rect batching
 * ========================================================================= */

void renderer_draw_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h, Color c) {
    if (r->rect_batch_count >= r->rect_batch_cap) renderer_flush_rects(r);
    if (r->rect_batch_count >= r->rect_batch_cap &&
        !renderer_reserve_rects(r, r->rect_batch_count + 1)) {
        return;
    }

    /* Pixel-align for crisp edges */
    x = floorf(x); y = floorf(y);
    w = ceilf(w);  h = ceilf(h);

    /* sRGB → linear at the CPU → GPU boundary (see renderer.h banner). */
    Color lin = color_linear_from_srgb(c);
    RectInstance *ri = &r->rect_batch[r->rect_batch_count++];
    ri->x = x; ri->y = y;
    ri->w = w; ri->h = h;
    ri->r = lin.r; ri->g = lin.g; ri->b = lin.b; ri->a = lin.a;
}

void renderer_flush_rects(Renderer *r) {
    if (r->rect_batch_count == 0) return;

    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder) {
        r->rect_batch_count = 0;
        return;
    }

    @autoreleasepool {
        [m->currentEncoder setRenderPipelineState:m->rectPipeline];

        MetalUniforms uniforms = metal_build_uniforms(r,
            r->font.loaded ? r->font.cell_width  : 0.0f,
            r->font.loaded ? r->font.cell_height : 0.0f);

        usize instance_bytes = r->rect_batch_count * sizeof(RectInstance);

        /* Shared unit quad at buffer(0) */
        [m->currentEncoder setVertexBytes:s_quad
                                   length:sizeof(s_quad)
                                  atIndex:0];

        /* Per-rect instance data at buffer(1) */
        metal_set_vertex_data(m, m->currentEncoder, r->rect_batch, instance_bytes, 1);

        /* Uniforms at buffer(2) — matches glyph pipeline layout */
        [m->currentEncoder setVertexBytes:&uniforms
                                   length:sizeof(uniforms)
                                  atIndex:2];

        [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                              vertexStart:0
                              vertexCount:6
                            instanceCount:r->rect_batch_count];
    }

    r->rect_batch_count = 0;
}

/* =========================================================================
 * Line batching — thick AA segments (graph-view edges)
 * ========================================================================= */

void renderer_draw_line(Renderer *r, f32 x0, f32 y0, f32 x1, f32 y1,
                        f32 thickness, Color c) {
    if (r->line_batch_count >= r->line_batch_cap) renderer_flush_lines(r);
    if (r->line_batch_count >= r->line_batch_cap &&
        !renderer_reserve_lines(r, r->line_batch_count + 1)) {
        return;
    }
    Color lin = color_linear_from_srgb(c);
    LineInstance *li = &r->line_batch[r->line_batch_count++];
    li->x0 = x0; li->y0 = y0; li->x1 = x1; li->y1 = y1;
    li->thickness = thickness < 0.5f ? 0.5f : thickness;
    li->r = lin.r; li->g = lin.g; li->b = lin.b; li->a = lin.a;
}

void renderer_flush_lines(Renderer *r) {
    if (r->line_batch_count == 0) return;
    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder || !m->linePipeline) {
        r->line_batch_count = 0;
        return;
    }
    @autoreleasepool {
        [m->currentEncoder setRenderPipelineState:m->linePipeline];
        MetalUniforms uniforms = metal_build_uniforms(r,
            r->font.loaded ? r->font.cell_width  : 0.0f,
            r->font.loaded ? r->font.cell_height : 0.0f);
        usize instance_bytes = r->line_batch_count * sizeof(LineInstance);
        [m->currentEncoder setVertexBytes:s_quad length:sizeof(s_quad) atIndex:0];
        metal_set_vertex_data(m, m->currentEncoder, r->line_batch, instance_bytes, 1);
        [m->currentEncoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:2];
        [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                              vertexStart:0
                              vertexCount:6
                            instanceCount:r->line_batch_count];
    }
    r->line_batch_count = 0;
}

/* =========================================================================
 * Pass 1.5: Rounded-rect batching (SDF + soft shadow)
 * Flushed between flat rects and glyphs so rounded panels/tabs sit above
 * plain backgrounds and below text.
 * ========================================================================= */

void renderer_draw_rrect_bordered(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill, Color border, f32 border_width,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
    f32 shadow_size, f32 shadow_alpha,
    f32 shadow_offset_x, f32 shadow_offset_y)
{
    RendererMetal *m = r->backend;
    /* If pipeline init failed, fall back to a flat rect — at least the panel
     * still gets drawn (without rounded corners, shadow or border). */
    if (!m || !m->rrectPipeline) {
        renderer_draw_rect(r, x, y, w, h, fill);
        return;
    }
    if (r->rrect_batch_count >= r->rrect_batch_cap) renderer_flush_rrects(r);
    if (r->rrect_batch_count >= r->rrect_batch_cap &&
        !renderer_reserve_rrects(r, r->rrect_batch_count + 1)) {
        return;
    }

    /* Pixel-align for crisp edges (matches renderer_draw_rect behaviour). */
    x = floorf(x); y = floorf(y);
    w = ceilf(w);  h = ceilf(h);

    /* sRGB → linear at the CPU → GPU boundary (see renderer.h banner). */
    Color lin_fill   = color_linear_from_srgb(fill);
    Color lin_border = color_linear_from_srgb(border);
    RRectInstance *ri = &r->rrect_batch[r->rrect_batch_count++];
    ri->x = x; ri->y = y;
    ri->w = w; ri->h = h;
    ri->r = lin_fill.r; ri->g = lin_fill.g; ri->b = lin_fill.b; ri->a = lin_fill.a;
    ri->r_tl = r_tl; ri->r_tr = r_tr;
    ri->r_br = r_br; ri->r_bl = r_bl;
    ri->shadow_size = shadow_size;
    ri->shadow_alpha = shadow_alpha;
    ri->shadow_offset_y = shadow_offset_y;
    ri->shadow_offset_x = shadow_offset_x;
    ri->border_r = lin_border.r; ri->border_g = lin_border.g;
    ri->border_b = lin_border.b; ri->border_a = lin_border.a;
    ri->border_width = border_width;
    ri->inner_shadow_size = 0.0f;
    ri->inner_shadow_alpha = 0.0f;
    ri->_pad0 = 0.0f;
}

void renderer_draw_rrect_inset(Renderer *r,
    f32 x, f32 y, f32 w, f32 h, Color fill, f32 radius,
    f32 inner_size, f32 inner_alpha)
{
    RendererMetal *m = r->backend;
    if (!m || !m->rrectPipeline) {
        renderer_draw_rect(r, x, y, w, h, fill);
        return;
    }
    if (r->rrect_batch_count >= r->rrect_batch_cap) renderer_flush_rrects(r);
    if (r->rrect_batch_count >= r->rrect_batch_cap &&
        !renderer_reserve_rrects(r, r->rrect_batch_count + 1)) {
        return;
    }

    x = floorf(x); y = floorf(y);
    w = ceilf(w);  h = ceilf(h);

    Color lin_fill = color_linear_from_srgb(fill);
    RRectInstance *ri = &r->rrect_batch[r->rrect_batch_count++];
    ri->x = x; ri->y = y;
    ri->w = w; ri->h = h;
    ri->r = lin_fill.r; ri->g = lin_fill.g; ri->b = lin_fill.b; ri->a = lin_fill.a;
    ri->r_tl = ri->r_tr = ri->r_br = ri->r_bl = radius;
    ri->shadow_size = 0.0f;
    ri->shadow_alpha = 0.0f;
    ri->shadow_offset_y = 0.0f;
    ri->shadow_offset_x = 0.0f;
    ri->border_r = ri->border_g = ri->border_b = ri->border_a = 0.0f;
    ri->border_width = 0.0f;
    ri->inner_shadow_size = inner_size;
    ri->inner_shadow_alpha = inner_alpha;
    ri->_pad0 = 0.0f;
}

/* =========================================================================
 * Backdrop blur — capture + sample
 * The capture step ends the active render encoder, blits the drawable into
 * a snapshot texture, and starts a new encoder targeting the same drawable.
 * The blur panel then issues a 2-pass separable Gaussian: first samples the
 * snapshot horizontally into an intermediate texture, then samples that
 * intermediate vertically directly to the drawable inside the rrect mask.
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    f32 pos_x, pos_y;
    f32 size_x, size_y;
    f32 tint_r, tint_g, tint_b, tint_a;
    f32 r_tl, r_tr, r_br, r_bl;
    f32 dir_x, dir_y;
    f32 inv_fb_w, inv_fb_h;
} BlurInstanceCPU;

static bool metal_ensure_blur_textures(RendererMetal *m, NSUInteger w, NSUInteger h) {
    if (m->blurSnapshot && m->blurSnapshot.width == w && m->blurSnapshot.height == h
        && m->blurIntermediate && m->blurIntermediate.width == w && m->blurIntermediate.height == h) {
        return true;
    }
    if (m->blurSnapshot) { [m->blurSnapshot release]; m->blurSnapshot = nil; }
    if (m->blurIntermediate) { [m->blurIntermediate release]; m->blurIntermediate = nil; }

    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                     width:w height:h mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;
    m->blurSnapshot     = [m->device newTextureWithDescriptor:desc];
    m->blurIntermediate = [m->device newTextureWithDescriptor:desc];
    return m->blurSnapshot != nil && m->blurIntermediate != nil;
}

void renderer_blur_capture(Renderer *r) {
    RendererMetal *m = r->backend;
    if (!m || !m->blurPipeline || !m->currentDrawable || !m->currentEncoder
        || !m->currentCommandBuffer) {
        return;
    }
    /* Flush anything still in the batches so the snapshot reflects current scene. */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);

    NSUInteger w = m->currentDrawable.texture.width;
    NSUInteger h = m->currentDrawable.texture.height;
    if (!metal_ensure_blur_textures(m, w, h)) return;
    m->blurLastUseSec = metal_monotonic_sec();

    /* End the current render encoder, blit drawable → snapshot, then start
     * a new encoder targeting the same drawable with LoadActionLoad so the
     * scene rendered so far is preserved. */
    [m->currentEncoder endEncoding];
    [m->currentEncoder release];
    m->currentEncoder = nil;

    id<MTLBlitCommandEncoder> blit = [m->currentCommandBuffer blitCommandEncoder];
    [blit copyFromTexture:m->currentDrawable.texture
              sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                toTexture:m->blurSnapshot
         destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture     = m->currentDrawable.texture;
    pass.colorAttachments[0].loadAction  = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    m->currentEncoder = [[m->currentCommandBuffer renderCommandEncoderWithDescriptor:pass] retain];
    if (m->currentEncoder) {
        MTLViewport vp = {0, 0, (double)w, (double)h, 0.0, 1.0};
        [m->currentEncoder setViewport:vp];
    }
    m->blurValid = true;
}

/* Helper: draw one blur pass. dir is in pixels; source is the texture to
 * sample. dst is implicit (the active encoder's color attachment). */
static void metal_draw_blur_pass(RendererMetal *m, Renderer *r,
                                 f32 x, f32 y, f32 w, f32 h,
                                 Color tint, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
                                 f32 dir_x, f32 dir_y,
                                 id<MTLTexture> source) {
    if (!m || !m->currentEncoder || !m->blurPipeline) return;

    NSUInteger fb_w = m->currentDrawable.texture.width;
    NSUInteger fb_h = m->currentDrawable.texture.height;

    BlurInstanceCPU inst;
    inst.pos_x = x; inst.pos_y = y;
    inst.size_x = w; inst.size_y = h;
    Color tlin = color_linear_from_srgb(tint);
    inst.tint_r = tlin.r; inst.tint_g = tlin.g;
    inst.tint_b = tlin.b; inst.tint_a = tlin.a;
    inst.r_tl = r_tl; inst.r_tr = r_tr;
    inst.r_br = r_br; inst.r_bl = r_bl;
    inst.dir_x = dir_x; inst.dir_y = dir_y;
    inst.inv_fb_w = 1.0f / (f32)fb_w;
    inst.inv_fb_h = 1.0f / (f32)fb_h;

    MetalUniforms uniforms = metal_build_uniforms(r,
        r->font.loaded ? r->font.cell_width  : 0.0f,
        r->font.loaded ? r->font.cell_height : 0.0f);

    [m->currentEncoder setRenderPipelineState:m->blurPipeline];
    [m->currentEncoder setVertexBytes:s_quad length:sizeof(s_quad) atIndex:0];
    [m->currentEncoder setVertexBytes:&inst length:sizeof(inst) atIndex:1];
    [m->currentEncoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:2];
    [m->currentEncoder setFragmentTexture:source atIndex:0];
    [m->currentEncoder setFragmentSamplerState:m->sampler atIndex:0];
    [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                          vertexStart:0 vertexCount:6 instanceCount:1];
}

void renderer_draw_blur_panel(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color tint,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl)
{
    RendererMetal *m = r->backend;
    /* No capture this frame, or blur unsupported → fall back to a tinted
     * rrect (no blur). Keeps the modal looking acceptable. */
    if (!m || !m->blurPipeline || !m->blurValid || !m->blurSnapshot) {
        renderer_draw_rrect(r, x, y, w, h, tint,
            r_tl, r_tr, r_br, r_bl, 0, 0, 0, 0);
        return;
    }
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);

    NSUInteger fb_w = m->currentDrawable.texture.width;
    NSUInteger fb_h = m->currentDrawable.texture.height;
    if (!metal_ensure_blur_textures(m, fb_w, fb_h)) {
        renderer_draw_rrect(r, x, y, w, h, tint,
            r_tl, r_tr, r_br, r_bl, 0, 0, 0, 0);
        return;
    }

    /* INVARIANT: every MTLRenderPassDescriptor/encoder allocation in this
     * sequence must live inside its own @autoreleasepool — descriptors
     * otherwise pile up on the frame's outer pool during continuous
     * animation. Any future pass added here must follow suit.
     *
     * Pass 1: horizontal blur from blurSnapshot → blurIntermediate.
     * Switch encoder target to intermediate, run pass with no tint
     * (intermediate must hold pure blur), then switch back to drawable.
     *
     * The two transient MTLRenderPassDescriptors are autoreleased; without
     * a local pool they accumulate on the outer pool for the whole frame,
     * and during a continuous blur animation (one call per frame) they pile
     * up until the frame's outer pool drains. Drain them here per call. The
     * pass-2 encoder is [retain]'d, so it survives the pool drain and is
     * handed back to the caller live (see proof below). */
    @autoreleasepool {
        [m->currentEncoder endEncoding];
        [m->currentEncoder release];
        m->currentEncoder = nil;

        MTLRenderPassDescriptor *pass1 = [MTLRenderPassDescriptor renderPassDescriptor];
        pass1.colorAttachments[0].texture     = m->blurIntermediate;
        pass1.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
        pass1.colorAttachments[0].storeAction = MTLStoreActionStore;
        m->currentEncoder = [[m->currentCommandBuffer renderCommandEncoderWithDescriptor:pass1] retain];
        if (m->currentEncoder) {
            MTLViewport vp = {0, 0, (double)fb_w, (double)fb_h, 0.0, 1.0};
            [m->currentEncoder setViewport:vp];
            /* Render across the FULL framebuffer area so the rect-local UV
             * arithmetic in blur_vertex still reads the right pixels for the
             * panel's fragment region. Tint=0 (pure blur output). */
            metal_draw_blur_pass(m, r, x, y, w, h, (Color){0,0,0,0},
                r_tl, r_tr, r_br, r_bl, 1.0f, 0.0f, m->blurSnapshot);
            [m->currentEncoder endEncoding];
            [m->currentEncoder release];
            m->currentEncoder = nil;
        }

        /* Pass 2: vertical blur from blurIntermediate → drawable, with tint. */
        MTLRenderPassDescriptor *pass2 = [MTLRenderPassDescriptor renderPassDescriptor];
        pass2.colorAttachments[0].texture     = m->currentDrawable.texture;
        pass2.colorAttachments[0].loadAction  = MTLLoadActionLoad;
        pass2.colorAttachments[0].storeAction = MTLStoreActionStore;
        m->currentEncoder = [[m->currentCommandBuffer renderCommandEncoderWithDescriptor:pass2] retain];
        if (m->currentEncoder) {
            MTLViewport vp = {0, 0, (double)fb_w, (double)fb_h, 0.0, 1.0};
            [m->currentEncoder setViewport:vp];
            metal_draw_blur_pass(m, r, x, y, w, h, tint,
                r_tl, r_tr, r_br, r_bl, 0.0f, 1.0f, m->blurIntermediate);
        }
    }
}

/* Border-less variant — keeps existing call sites unchanged. */
void renderer_draw_rrect(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
    f32 shadow_size, f32 shadow_alpha,
    f32 shadow_offset_x, f32 shadow_offset_y)
{
    renderer_draw_rrect_bordered(r, x, y, w, h, fill,
        (Color){0, 0, 0, 0}, 0.0f,
        r_tl, r_tr, r_br, r_bl,
        shadow_size, shadow_alpha, shadow_offset_x, shadow_offset_y);
}

void renderer_flush_rrects(Renderer *r) {
    if (r->rrect_batch_count == 0) return;

    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder || !m->rrectPipeline) {
        r->rrect_batch_count = 0;
        return;
    }

    @autoreleasepool {
        [m->currentEncoder setRenderPipelineState:m->rrectPipeline];

        MetalUniforms uniforms = metal_build_uniforms(r,
            r->font.loaded ? r->font.cell_width  : 0.0f,
            r->font.loaded ? r->font.cell_height : 0.0f);

        usize instance_bytes = r->rrect_batch_count * sizeof(RRectInstance);

        [m->currentEncoder setVertexBytes:s_quad
                                   length:sizeof(s_quad)
                                  atIndex:0];
        metal_set_vertex_data(m, m->currentEncoder, r->rrect_batch, instance_bytes, 1);
        [m->currentEncoder setVertexBytes:&uniforms
                                   length:sizeof(uniforms)
                                  atIndex:2];

        [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                              vertexStart:0
                              vertexCount:6
                            instanceCount:r->rrect_batch_count];
    }

    r->rrect_batch_count = 0;
}

/* =========================================================================
 * Pass 2: Instanced glyph rendering
 * ========================================================================= */

void renderer_push_glyph(Renderer *r, f32 x, f32 y, u32 codepoint, Color fg) {
    if (!r->font.loaded) return;
    if (codepoint <= 32) return;
    /* Skip the GPU round-trip entirely for fully-transparent glyphs.
     * Modal close animations multiply text alpha by a panel_alpha that
     * dips to ~0 on the trailing frames; without this the glyphs still
     * push instances through the pipeline (and a 0-alpha glyph still
     * costs ALU work and bandwidth). */
    if (fg.a <= 0.0f) return;

    if (r->glyph_count >= r->glyph_cap) renderer_flush_glyphs(r);
    if (r->glyph_count >= r->glyph_cap &&
        !renderer_reserve_glyphs(r, r->glyph_count + 1)) {
        return;
    }

    f32 u0, v0, u1, v1;
    if (!font_get_glyph_uv(&r->font, codepoint, &u0, &v0, &u1, &v1))
        return;

    GlyphInstance *gi = &r->glyph_instances[r->glyph_count++];
    gi->x = floorf(x);
    gi->y = floorf(y);
    /* Negative UV signals color glyph (from rasterize_glyph) */
    bool is_color = (u0 < 0);
    gi->is_color = is_color ? 1.0f : 0.0f;
    gi->u0 = is_color ? -u0 : u0;
    gi->v0 = v0;
    gi->u1 = is_color ? -u1 : u1;
    gi->v1 = v1;
    /* sRGB → linear at the CPU → GPU boundary (see renderer.h banner). */
    Color lin = color_linear_from_srgb(fg);
    gi->r = lin.r;
    gi->g = lin.g;
    gi->b = lin.b;
    /* Color emoji are rasterized into a 2-cell-wide bitmap (see
     * rasterize_glyph in font.c: ecw = cw * 2). The quad has to match or
     * the GPU samples a wide texture into a single-cell quad and the
     * emoji ends up horizontally squashed — which is exactly what
     * "emojiler aşırı basık" looked like in the terminal. */
    gi->w_cells = is_color ? 2.0f : 1.0f;
    gi->a = fg.a;
}

/* Append path: cached row instances already carry linear RGB. */
void renderer_append_rects(Renderer *r, const RectInstance *src, u32 count) {
    while (count > 0) {
        if (r->rect_batch_count >= r->rect_batch_cap) renderer_flush_rects(r);
        if (r->rect_batch_count >= r->rect_batch_cap &&
            !renderer_reserve_rects(r, r->rect_batch_count + 1)) {
            return;
        }
        u32 avail = r->rect_batch_cap - r->rect_batch_count;
        if (avail > 0 && count > avail && r->rect_batch_cap < MAX_RECTS) {
            (void)renderer_reserve_rects(r, r->rect_batch_count + count);
            avail = r->rect_batch_cap - r->rect_batch_count;
        }
        u32 take = count < avail ? count : avail;
        RectInstance *dst = &r->rect_batch[r->rect_batch_count];
        for (u32 i = 0; i < take; i++) {
            dst[i] = src[i];
        }
        r->rect_batch_count += take;
        src   += take;
        count -= take;
    }
}

void renderer_append_glyphs(Renderer *r, const GlyphInstance *src, u32 count) {
    if (!r->font.loaded) return;
    while (count > 0) {
        if (r->glyph_count >= r->glyph_cap) renderer_flush_glyphs(r);
        if (r->glyph_count >= r->glyph_cap &&
            !renderer_reserve_glyphs(r, r->glyph_count + 1)) {
            return;
        }
        u32 avail = r->glyph_cap - r->glyph_count;
        if (avail > 0 && count > avail && r->glyph_cap < MAX_GLYPHS) {
            (void)renderer_reserve_glyphs(r, r->glyph_count + count);
            avail = r->glyph_cap - r->glyph_count;
        }
        u32 take = count < avail ? count : avail;
        GlyphInstance *dst = &r->glyph_instances[r->glyph_count];
        for (u32 i = 0; i < take; i++) {
            dst[i] = src[i];
        }
        r->glyph_count += take;
        src   += take;
        count -= take;
    }
}

void renderer_flush_glyphs(Renderer *r) {
    /* Layering invariant: glyphs always draw on top of rects/rrects. Many
     * call sites (renderer_set_ui_scale at heading boundaries, modal closers)
     * call flush_glyphs while regular rects are still pending in the batch.
     * Without draining rects FIRST those pending rects would flush in
     * end_frame *after* this glyph batch hit the encoder — and since the
     * Metal command encoder preserves submission order, the rects then
     * paint over the glyphs we just drew (this is what makes code-block
     * bodies invisible when a heading follows the block). Flushing
     * rects + rrects here keeps the rect → rrect → glyph order regardless
     * of how the caller sequences the individual push calls. */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);

    if (r->glyph_count == 0) return;

    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder || !m->atlasTexture) {
        r->glyph_count = 0;
        return;
    }

    @autoreleasepool {
        [m->currentEncoder setRenderPipelineState:m->glyphPipeline];

        /* Glyph quad size. For UI chrome text the layout assumes a fixed 1:2
         * cell (8x16pt), but a glyph is rasterized to fill the font's real
         * atlas cell — which is taller than 2:1 for some fonts (JetBrains Mono
         * is 20x45). Mapping that cell onto a 1:2 quad compresses it
         * vertically ("basık"). Anchor the glyph height to the requested width
         * times the atlas aspect ratio so UI text scales uniformly. The layout
         * keeps using its own ui_cell_h for spacing — only the drawn glyph is
         * corrected. For a 1:2 font this is identical to ui_cell_h (no change).
         * Terminal text (ui_cell_w == 0) uses the atlas cell verbatim. */
        f32 gcw, gch;
        if (r->ui_cell_w > 0.0f) {
            gcw = r->ui_cell_w;
            gch = (r->font.cell_width > 0.0f)
                ? gcw * (r->font.cell_height / r->font.cell_width)
                : ((r->ui_cell_h > 0.0f) ? r->ui_cell_h : gcw * 2.0f);
        } else {
            gcw = r->font.cell_width;
            gch = r->font.cell_height;
        }
        MetalUniforms uniforms = metal_build_uniforms(r, gcw, gch);

        usize instance_bytes = r->glyph_count * sizeof(GlyphInstance);

        [m->currentEncoder setVertexBytes:s_quad
                                   length:sizeof(s_quad)
                                  atIndex:0];

        metal_set_vertex_data(m, m->currentEncoder, r->glyph_instances, instance_bytes, 1);

        [m->currentEncoder setVertexBytes:&uniforms
                                   length:sizeof(uniforms)
                                  atIndex:2];

        if (m->atlasTexture != (__bridge id<MTLTexture>)r->font.metal_texture) {
            m->atlasTexture = (__bridge id<MTLTexture>)r->font.metal_texture;
        }
        [m->currentEncoder setFragmentTexture:m->atlasTexture atIndex:0];
        /* Bind color emoji atlas if available */
        if (r->font.color_texture) {
            id<MTLTexture> ctex = (__bridge id<MTLTexture>)r->font.color_texture;
            [m->currentEncoder setFragmentTexture:ctex atIndex:1];
        }
        [m->currentEncoder setFragmentSamplerState:m->sampler atIndex:0];

        [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                              vertexStart:0
                              vertexCount:6
                            instanceCount:r->glyph_count];
    }

    r->glyph_count = 0;
}

/* =========================================================================
 * UI scale overrides
 * ========================================================================= */

void renderer_set_ui_scale(Renderer *r, f32 cw, f32 ch) {
    renderer_flush_glyphs(r);
    r->ui_cell_w = cw;
    r->ui_cell_h = ch;
}

void renderer_reset_ui_scale(Renderer *r) {
    renderer_flush_glyphs(r);
    r->ui_cell_w = 0.0f;
    r->ui_cell_h = 0.0f;
}

void renderer_push_scissor(Renderer *r, f32 x, f32 y, f32 w, f32 h) {
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
    r->scissor_active = true;
    r->scissor_x = x; r->scissor_y = y; r->scissor_w = w; r->scissor_h = h;
    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder) return;
    /* Metal scissor: top-left origin, matches our coordinate system */
    MTLScissorRect scissor = {
        .x      = (NSUInteger)fmaxf(x, 0.0f),
        .y      = (NSUInteger)fmaxf(y, 0.0f),
        .width  = (NSUInteger)fmaxf(w, 1.0f),
        .height = (NSUInteger)fmaxf(h, 1.0f),
    };
    [m->currentEncoder setScissorRect:scissor];
}

void renderer_pop_scissor(Renderer *r) {
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
    r->scissor_active = false;
    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder) return;
    MTLScissorRect full = {
        .x = 0, .y = 0,
        .width  = (NSUInteger)r->screen_width,
        .height = (NSUInteger)r->screen_height,
    };
    [m->currentEncoder setScissorRect:full];
}

/* =========================================================================
 * Compat / helper API
 * ========================================================================= */

void renderer_draw_glyph(Renderer *r, u32 codepoint, f32 x, f32 y, Color fg, Color bg) {
    (void)bg;
    renderer_push_glyph(r, x, y, codepoint, fg);
}

void renderer_flush_text(Renderer *r) { renderer_flush_glyphs(r); }
void renderer_flush_cells(Renderer *r) { renderer_flush_glyphs(r); }

void renderer_push_cell(Renderer *r, f32 x, f32 y, u32 codepoint, Color fg, Color bg) {
    (void)bg;
    renderer_push_glyph(r, x, y, codepoint, fg);
}

/* =========================================================================
 * Metal atlas helpers (called by font.c)
 * ========================================================================= */

void renderer_metal_set_atlas(Renderer *r, void *texture) {
    RendererMetal *m = r->backend;
    if (m) m->atlasTexture = (__bridge id<MTLTexture>)texture;
}

/* Glyph-atlas upload. This does an UNSYNCHRONIZED CPU write (replaceRegion)
 * into a texture the GPU may still be sampling from a previously-submitted
 * frame. The M1 fence (dispatch_semaphore in renderer_begin_frame) bounds the
 * CPU to at most METAL_FRAME_BUFFER_COUNT frames ahead of the GPU, but that
 * alone does NOT make an in-place atlas write race-free: a glyph rasterized
 * during frame N's encoding (between begin_frame and end_frame) calls this
 * function and mutates a region that frames N-1 / N-2 — still in flight on the
 * GPU — may be reading. Because the atlas is a single shared MTLStorageMode
 * texture (not triple-buffered), there is no slot the fence can protect.
 *
 * Why this is currently TOLERABLE (not correct): glyph atlas writes are
 * append-only into PREVIOUSLY-UNUSED atlas cells (font.c rasterizes a new
 * glyph into a fresh cell and never overwrites a live one within the same
 * session), so an in-flight frame samples only cells written in an earlier
 * frame, never the cell being written now. The hazard is limited to the
 * driver's own texture-coherency on the freshly-written region, which Metal
 * serializes for MTLStorageModeShared/Managed textures used by later-submitted
 * command buffers. The torn-glyph artifact the audit targets comes from the
 * ring buffer + drawable reuse (fixed by M1), not from atlas overwrite.
 *
 * The fully-correct fix (deferred — see M2 in audit) is a staging buffer +
 * blitCommandEncoder copy issued on the frame's command buffer, so the upload
 * is ordered on the GPU timeline relative to sampling. That requires threading
 * the current command buffer into this call site (font.c does not have it) and
 * is a larger refactor; it is intentionally NOT done here. Do not convert the
 * atlas to in-place overwrite-of-live-cells without that staging path. */
void renderer_metal_upload_texture(void *texture, i32 x, i32 y, i32 w, i32 h, const void *data) {
    if (!texture || !data) return;

    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    MTLRegion region = MTLRegionMake2D((NSUInteger)x, (NSUInteger)y,
                                       (NSUInteger)w, (NSUInteger)h);
    /* Bytes-per-pixel inferred from the texture's pixel format — supports both
     * the legacy R8 atlas and the current BGRA8 subpixel atlas. */
    NSUInteger bpp;
    switch (tex.pixelFormat) {
        case MTLPixelFormatR8Unorm:        bpp = 1; break;
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatRGBA8Unorm_sRGB: bpp = 4; break;
        default:                            bpp = 4; break;
    }
    [tex replaceRegion:region
           mipmapLevel:0
             withBytes:data
           bytesPerRow:(NSUInteger)w * bpp];
}

/* =========================================================================
 * GPU compute: terminal grid → vertex buffers on GPU
 * ========================================================================= */

/* ComputeParams must match Metal shader struct */
typedef struct {
    u32  cols;
    u32  rows;
    f32  origin_x;
    f32  origin_y;
    f32  cell_w;
    f32  cell_h;
    u32  bg_default;
    u32  fg_default;
} ComputeParams;

void renderer_upload_palette(Renderer *r) {
    (void)r;
    /* Palette is uploaded inline as setBytes in compute dispatch — no persistent buffer needed */
}

void renderer_compute_terminal(Renderer *r, const void *cells, i32 cols, i32 rows,
                                f32 origin_x, f32 origin_y) {
    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder || !m->computeGlyphPipeline || !m->atlasTexture)
        return;
    if (!r->font.loaded || cols <= 0 || rows <= 0) return;

    @autoreleasepool {
        usize cell_bytes = (usize)(cols * rows) * 16; /* sizeof(Cell) = 16 */
        usize max_glyphs = (usize)(cols * rows);
        usize max_rects  = (usize)(cols * rows);
        usize glyph_buf_bytes = max_glyphs * sizeof(GlyphInstance);
        usize rect_buf_bytes  = max_rects * sizeof(RectInstance); /* instanced */

        /* Create transient buffers for this frame */
        id<MTLBuffer> cellBuf = [m->device newBufferWithBytes:cells
                                                       length:cell_bytes
                                                      options:MTLResourceStorageModeShared];
        id<MTLBuffer> glyphOutBuf = [m->device newBufferWithLength:glyph_buf_bytes
                                                           options:MTLResourceStorageModeShared];
        id<MTLBuffer> rectOutBuf = [m->device newBufferWithLength:rect_buf_bytes
                                                          options:MTLResourceStorageModeShared];
        /* Atomic counters (initialized to 0) */
        u32 zero = 0;
        id<MTLBuffer> glyphCountBuf = [m->device newBufferWithBytes:&zero length:4
                                                            options:MTLResourceStorageModeShared];
        id<MTLBuffer> rectCountBuf = [m->device newBufferWithBytes:&zero length:4
                                                           options:MTLResourceStorageModeShared];

        if (!cellBuf || !glyphOutBuf || !rectOutBuf || !glyphCountBuf || !rectCountBuf) {
            if (cellBuf) [cellBuf release];
            if (glyphOutBuf) [glyphOutBuf release];
            if (rectOutBuf) [rectOutBuf release];
            if (glyphCountBuf) [glyphCountBuf release];
            if (rectCountBuf) [rectCountBuf release];
            return;
        }

        ComputeParams params = {
            .cols = (u32)cols, .rows = (u32)rows,
            .origin_x = origin_x, .origin_y = origin_y,
            .cell_w = r->font.cell_width, .cell_h = r->font.cell_height,
            .bg_default = BG_DEFAULT, .fg_default = FG_DEFAULT,
        };

        /* Build UV lookup table from glyph cache */
        /* Pack codepoint → UV entries for GPU lookup */
        extern bool font_get_glyph_uv(FontAtlas *atlas, u32 cp, f32 *u0, f32 *v0, f32 *u1, f32 *v1);
        typedef struct { f32 uv[4]; } GlyphUVEntry;
        u32 uv_keys[512];
        GlyphUVEntry uv_vals[512];
        u32 uv_count = 0;
        /* Pre-cache ASCII + recently used glyphs */
        for (u32 cp = 33; cp < 127; cp++) {
            f32 u0, v0, u1, v1;
            if (font_get_glyph_uv(&r->font, cp, &u0, &v0, &u1, &v1)) {
                uv_keys[uv_count] = cp;
                uv_vals[uv_count] = (GlyphUVEntry){{u0, v0, u1, v1}};
                uv_count++;
            }
        }
        /* Also scan cells for any non-ASCII codepoints and add them */
        const u8 *cell_data = (const u8 *)cells;
        for (i32 i = 0; i < cols * rows && uv_count < 510; i++) {
            u32 cp;
            memcpy(&cp, cell_data + i * 16, 4);
            if (cp >= 127) {
                /* Check if already in table */
                bool found = false;
                for (u32 k = 0; k < uv_count; k++) {
                    if (uv_keys[k] == cp) { found = true; break; }
                }
                if (!found) {
                    f32 u0, v0, u1, v1;
                    if (font_get_glyph_uv(&r->font, cp, &u0, &v0, &u1, &v1)) {
                        uv_keys[uv_count] = cp;
                        uv_vals[uv_count] = (GlyphUVEntry){{u0, v0, u1, v1}};
                        uv_count++;
                    }
                }
            }
        }

        /* Convert palette to float4 array for GPU, linearized at the boundary.
         * g_ansi_colors[] stores sRGB per the CPU-side invariant. */
        f32 palette[256 * 4];
        for (i32 i = 0; i < 256; i++) {
            palette[i*4+0] = srgb_decode(g_ansi_colors[i].r);
            palette[i*4+1] = srgb_decode(g_ansi_colors[i].g);
            palette[i*4+2] = srgb_decode(g_ansi_colors[i].b);
            palette[i*4+3] = g_ansi_colors[i].a;
        }

        /* === End current render encoder, run compute, restart render === */
        [m->currentEncoder endEncoding];
        [m->currentEncoder release];
        m->currentEncoder = nil;

        /* Compute pass */
        id<MTLComputeCommandEncoder> compute = [m->currentCommandBuffer computeCommandEncoder];
        if (!compute) goto skip_compute;

        MTLSize grid = MTLSizeMake((NSUInteger)cols, (NSUInteger)rows, 1);
        MTLSize threadgroup = MTLSizeMake(16, 8, 1);
        /* Clamp threadgroup to grid dimensions */
        if (threadgroup.width > grid.width) threadgroup.width = grid.width;
        if (threadgroup.height > grid.height) threadgroup.height = grid.height;

        /* --- Dispatch bg rect compute --- */
        if (m->computeBgRectPipeline) {
            [compute setComputePipelineState:m->computeBgRectPipeline];
            [compute setBuffer:cellBuf offset:0 atIndex:0];
            [compute setBuffer:rectOutBuf offset:0 atIndex:1];
            [compute setBuffer:rectCountBuf offset:0 atIndex:2];
            [compute setBytes:&params length:sizeof(params) atIndex:3];
            [compute setBytes:palette length:sizeof(palette) atIndex:4];
            [compute dispatchThreads:grid threadsPerThreadgroup:threadgroup];
        }

        /* --- Dispatch glyph compute --- */
        [compute setComputePipelineState:m->computeGlyphPipeline];
        [compute setBuffer:cellBuf offset:0 atIndex:0];
        [compute setBuffer:glyphOutBuf offset:0 atIndex:1];
        [compute setBuffer:glyphCountBuf offset:0 atIndex:2];
        [compute setBytes:&params length:sizeof(params) atIndex:3];
        [compute setBytes:uv_vals length:uv_count * sizeof(GlyphUVEntry) atIndex:4];
        [compute setBytes:palette length:sizeof(palette) atIndex:5];
        [compute setBytes:uv_keys length:uv_count * sizeof(u32) atIndex:6];
        [compute setBytes:&uv_count length:sizeof(u32) atIndex:7];
        [compute dispatchThreads:grid threadsPerThreadgroup:threadgroup];

        [compute endEncoding];

    skip_compute:;
        /* Restart render encoder for drawing compute results + remaining UI */
        MTLRenderPassDescriptor *passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        passDesc.colorAttachments[0].texture    = m->currentDrawable.texture;
        passDesc.colorAttachments[0].loadAction = MTLLoadActionLoad; /* preserve clear + any prior draws */
        passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;

        m->currentEncoder = [[m->currentCommandBuffer renderCommandEncoderWithDescriptor:passDesc] retain];
        if (!m->currentEncoder) goto cleanup_compute;

        MTLViewport viewport = {
            .originX = 0, .originY = 0,
            .width  = (double)r->screen_width,
            .height = (double)r->screen_height,
            .znear  = 0.0, .zfar = 1.0
        };
        [m->currentEncoder setViewport:viewport];

        /* Read back counts (GPU→CPU sync via shared memory) */
        /* Note: compute completed before render encoder starts because they're
         * sequential in the same command buffer */
        u32 glyph_n = *(u32 *)glyphCountBuf.contents;
        u32 rect_n  = *(u32 *)rectCountBuf.contents;

        /* Draw bg rects from compute output (instanced layout) */
        if (rect_n > 0 && m->computeBgRectPipeline) {
            [m->currentEncoder setRenderPipelineState:m->rectPipeline];
            MetalUniforms uniforms = metal_build_uniforms(r,
                r->font.cell_width, r->font.cell_height);
            [m->currentEncoder setVertexBytes:s_quad length:sizeof(s_quad) atIndex:0];
            [m->currentEncoder setVertexBuffer:rectOutBuf offset:0 atIndex:1];
            [m->currentEncoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:2];
            [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                  vertexStart:0
                                  vertexCount:6
                                instanceCount:rect_n];
        }

        /* Draw glyphs from compute output */
        if (glyph_n > 0) {
            [m->currentEncoder setRenderPipelineState:m->glyphPipeline];
            MetalUniforms uniforms = metal_build_uniforms(r,
                r->font.cell_width, r->font.cell_height);
            [m->currentEncoder setVertexBytes:s_quad length:sizeof(s_quad) atIndex:0];
            [m->currentEncoder setVertexBuffer:glyphOutBuf offset:0 atIndex:1];
            [m->currentEncoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:2];
            [m->currentEncoder setFragmentTexture:m->atlasTexture atIndex:0];
            [m->currentEncoder setFragmentSamplerState:m->sampler atIndex:0];
            [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                  vertexStart:0
                                  vertexCount:6
                                instanceCount:glyph_n];
        }

    cleanup_compute:
        [cellBuf release];
        [glyphOutBuf release];
        [rectOutBuf release];
        [glyphCountBuf release];
        [rectCountBuf release];
    }
}

/* =========================================================================
 * Inline image rendering
 * ========================================================================= */

static void metal_image_cache_release_entry(RendererMetal *m, MetalImageCacheEntry *e) {
    if (!e || !e->texture) return;
    if (m->imageCacheBytes >= e->bytes) m->imageCacheBytes -= e->bytes;
    else m->imageCacheBytes = 0;
    [e->texture release];
    memset(e, 0, sizeof(*e));
}

static MetalImageCacheEntry *metal_image_cache_victim(RendererMetal *m) {
    MetalImageCacheEntry *empty = NULL;
    MetalImageCacheEntry *oldest = NULL;
    for (u32 i = 0; i < METAL_IMAGE_CACHE_CAP; i++) {
        MetalImageCacheEntry *e = &m->imageCache[i];
        if (!e->texture) {
            if (!empty) empty = e;
            continue;
        }
        if (!oldest || e->last_used_frame < oldest->last_used_frame) oldest = e;
    }
    return empty ? empty : oldest;
}

static id<MTLTexture> metal_image_texture(RendererMetal *m, const void *key,
                                          u64 generation, const u8 *pixels,
                                          i32 img_w, i32 img_h) {
    if (!m || !key || !pixels || img_w <= 0 || img_h <= 0) return nil;
    /* Single pass: hit the (key,gen,w,h) match if present, AND drop any
     * stale-generation entries for the same key. Animated GIFs bump
     * `generation` on every frame, so without this drop the previous
     * frames pile up to the 64 MB budget before LRU evicts them — a
     * 30-frame GIF held ~30× the visible RAM until budget pressure.
     * Same-generation different-(w,h) is treated as stale too: it's the
     * "same logical image, just resized" case. */
    id<MTLTexture> hit = nil;
    MetalImageCacheEntry *reusable = NULL;
    for (u32 i = 0; i < METAL_IMAGE_CACHE_CAP; i++) {
        MetalImageCacheEntry *e = &m->imageCache[i];
        if (!e->texture) continue;
        if (e->key != key) continue;
        if (e->generation == generation && e->width == img_w && e->height == img_h) {
            if (!hit) {
                e->last_used_frame = m->frameSerial;
                hit = e->texture;
            }
            /* Duplicate same-generation entries shouldn't exist, but if a
             * race produced one, evict the duplicate. */
            else metal_image_cache_release_entry(m, e);
        } else if (!reusable && e->width == img_w && e->height == img_h) {
            /* Same key + same dimensions but a stale generation. This is the
             * animated-GIF hot path: every frame bumps `generation` so the
             * key-gen lookup misses, but the underlying texture's storage
             * is the right size and is owned by the same caller. Reuse it
             * in place — replaceRegion below — to avoid the per-frame
             * MTLTexture create + destroy + budget bookkeeping churn. */
            reusable = e;
        } else {
            /* Older / mismatched dimensions for the same key — caller has
             * moved on; this entry will never be reused. */
            metal_image_cache_release_entry(m, e);
        }
    }
    if (hit) return hit;
    if (reusable) {
        MTLRegion region = MTLRegionMake2D(0, 0,
                                           (NSUInteger)img_w, (NSUInteger)img_h);
        [reusable->texture replaceRegion:region
                             mipmapLevel:0
                               withBytes:pixels
                             bytesPerRow:(NSUInteger)(img_w * 4)];
        reusable->generation     = generation;
        reusable->last_used_frame = m->frameSerial;
        return reusable->texture;
    }

    usize bytes = (usize)img_w * (usize)img_h * 4;
    /* A single texture larger than 1/4 of the budget would, on its own,
     * force every other entry out — a 4K RGBA frame (64 MiB) thrashes
     * the cache to nothing. Don't admit such textures: render once,
     * uncached, then free at end of frame via the autorelease pool that
     * owns the returned MTLTexture. */
    if (bytes > METAL_IMAGE_CACHE_BUDGET / 4) {
        MTLTextureDescriptor *texDesc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                        width:(NSUInteger)img_w
                                       height:(NSUInteger)img_h
                                    mipmapped:NO];
        texDesc.usage = MTLTextureUsageShaderRead;
        texDesc.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [m->device newTextureWithDescriptor:texDesc];
        if (!tex) return nil;
        MTLRegion region = MTLRegionMake2D(0, 0, (NSUInteger)img_w, (NSUInteger)img_h);
        [tex replaceRegion:region
               mipmapLevel:0
                 withBytes:pixels
               bytesPerRow:(NSUInteger)(img_w * 4)];
        /* MRC: newTextureWithDescriptor returns a +1 owned object. It is NOT
         * cached and NOT stored, so without autorelease it leaks a whole
         * texture EVERY frame the oversize image is on screen. autorelease
         * hands ownership to renderer_draw_image_cached's @autoreleasepool,
         * which drains it after the draw is encoded (the command buffer
         * retains it for the GPU until completion). */
        return [tex autorelease];
    }

    while (m->imageCacheBytes + bytes > METAL_IMAGE_CACHE_BUDGET) {
        MetalImageCacheEntry *victim = metal_image_cache_victim(m);
        if (!victim || !victim->texture) break;
        metal_image_cache_release_entry(m, victim);
    }

    MetalImageCacheEntry *slot = metal_image_cache_victim(m);
    if (!slot) return nil;
    metal_image_cache_release_entry(m, slot);

    MTLTextureDescriptor *texDesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                    width:(NSUInteger)img_w
                                   height:(NSUInteger)img_h
                                mipmapped:NO];
    texDesc.usage = MTLTextureUsageShaderRead;
    texDesc.storageMode = MTLStorageModeShared;

    id<MTLTexture> tex = [m->device newTextureWithDescriptor:texDesc];
    if (!tex) return nil;

    MTLRegion region = MTLRegionMake2D(0, 0, (NSUInteger)img_w, (NSUInteger)img_h);
    [tex replaceRegion:region
           mipmapLevel:0
             withBytes:pixels
           bytesPerRow:(NSUInteger)(img_w * 4)];

    *slot = (MetalImageCacheEntry){
        .key = key,
        .generation = generation,
        .width = img_w,
        .height = img_h,
        .bytes = bytes,
        .last_used_frame = m->frameSerial,
        .texture = tex,
    };
    m->imageCacheBytes += bytes;
    return tex;
}

void renderer_draw_image_cached(Renderer *r, const void *cache_key, u64 cache_generation,
                                const u8 *pixels, i32 img_w, i32 img_h,
                                f32 x, f32 y, f32 draw_w, f32 draw_h) {
    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder || !pixels) return;
    if (img_w <= 0 || img_h <= 0 || draw_w <= 0 || draw_h <= 0) return;

    /* Flush any pending batches before switching pipeline state */
    renderer_flush_rects(r);
    renderer_flush_glyphs(r);

    @autoreleasepool {
        id<MTLTexture> tex = metal_image_texture(m, cache_key ? cache_key : pixels,
                                                 cache_generation, pixels, img_w, img_h);
        if (!tex) return;

        /* Use the glyph pipeline which already supports textured quads with
         * alpha blending. We create a single GlyphInstance that covers the
         * entire image area, using full UV range [0,1]. The is_color=1.0
         * flag tells the fragment shader to sample the texture directly
         * without applying foreground color tinting. */
        GlyphInstance gi;
        gi.x = floorf(x);
        gi.y = floorf(y);
        /* Negative UVs are used for color glyphs in the normal pipeline,
         * but here we use full [0,1] range and set is_color=1.0 */
        gi.u0 = 0.0f;
        gi.v0 = 0.0f;
        gi.u1 = 1.0f;
        gi.v1 = 1.0f;
        gi.r = 1.0f;
        gi.g = 1.0f;
        gi.b = 1.0f;
        gi.is_color = 1.0f;
        gi.w_cells = 1.0f;
        gi.a = 1.0f;

        [m->currentEncoder setRenderPipelineState:m->glyphPipeline];

        /* Build uniforms with draw_w/draw_h as cell size so the unit quad
         * scales to the desired image dimensions */
        MetalUniforms uniforms;
        memcpy(uniforms.projection, r->projection, sizeof(uniforms.projection));
        uniforms.cell_w = draw_w;
        uniforms.cell_h = draw_h;
        uniforms._pad[0] = 0.0f;
        uniforms._pad[1] = 0.0f;

        [m->currentEncoder setVertexBytes:s_quad
                                   length:sizeof(s_quad)
                                  atIndex:0];
        [m->currentEncoder setVertexBytes:&gi
                                   length:sizeof(gi)
                                  atIndex:1];
        [m->currentEncoder setVertexBytes:&uniforms
                                   length:sizeof(uniforms)
                                  atIndex:2];

        /* Bind the image texture as the color texture (index 1) and
         * also as the atlas (index 0) since the shader samples from it */
        [m->currentEncoder setFragmentTexture:tex atIndex:0];
        [m->currentEncoder setFragmentTexture:tex atIndex:1];
        [m->currentEncoder setFragmentSamplerState:m->sampler atIndex:0];

        [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                              vertexStart:0
                              vertexCount:6
                            instanceCount:1];
    }
}

void renderer_draw_image(Renderer *r, const u8 *pixels, i32 img_w, i32 img_h,
                         f32 x, f32 y, f32 draw_w, f32 draw_h) {
    renderer_draw_image_cached(r, pixels, 0, pixels, img_w, img_h,
                               x, y, draw_w, draw_h);
}

/* =========================================================================
 * Background image: load, destroy, draw
 * ========================================================================= */

#include "stb_image.h"

/* Lazy-compile the background-image render pipeline on first use. Skipped
 * at startup because ~90% of users never set a background image — the
 * pipeline descriptor + shader compilation together cost ~1 MB of Metal
 * state we otherwise never exercise. */
static bool ensure_bg_image_pipeline(RendererMetal *m) {
    if (!m || !m->device) return false;
    if (m->bgImagePipeline) return true;
    if (!m->shaderLibrary)  return false;

    @autoreleasepool {
        id<MTLFunction> vert = [m->shaderLibrary newFunctionWithName:@"bg_image_vertex2"];
        id<MTLFunction> frag = [m->shaderLibrary newFunctionWithName:@"bg_image_fragment2"];
        if (!vert || !frag) {
            if (vert) [vert release];
            if (frag) [frag release];
            return false;
        }
        MTLRenderPipelineDescriptor *bgDesc = [[MTLRenderPipelineDescriptor alloc] init];
        bgDesc.vertexFunction   = vert;
        bgDesc.fragmentFunction = frag;
        bgDesc.colorAttachments[0].pixelFormat     = MTLPixelFormatBGRA8Unorm_sRGB;
        bgDesc.colorAttachments[0].blendingEnabled  = YES;
        bgDesc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
        bgDesc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        bgDesc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorSourceAlpha;
        bgDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        NSError *error = nil;
        m->bgImagePipeline = [m->device newRenderPipelineStateWithDescriptor:bgDesc error:&error];
        [bgDesc release];
        [vert release];
        [frag release];
        if (!m->bgImagePipeline) {
            fprintf(stderr, "renderer_metal: bg image pipeline: %s\n",
                    error.localizedDescription.UTF8String);
            return false;
        }

        /* Also build the bg-image sampler (tiling repeat mode). */
        if (!m->bgImageSampler) {
            MTLSamplerDescriptor *bgSampDesc = [[MTLSamplerDescriptor alloc] init];
            bgSampDesc.minFilter    = MTLSamplerMinMagFilterLinear;
            bgSampDesc.magFilter    = MTLSamplerMinMagFilterLinear;
            bgSampDesc.sAddressMode = MTLSamplerAddressModeRepeat;
            bgSampDesc.tAddressMode = MTLSamplerAddressModeRepeat;
            m->bgImageSampler = [m->device newSamplerStateWithDescriptor:bgSampDesc];
            [bgSampDesc release];
        }
    }
    return true;
}

bool renderer_load_background_image(Renderer *r, const char *path, void *gpu_device) {
    if (!path || !path[0] || !gpu_device) return false;

    RendererMetal *m = r->backend;
    if (!m) return false;

    /* Ensure the bg-image render pipeline exists before uploading the texture
     * — if compile fails, skip the texture upload so we don't burn memory on
     * a bitmap that can never be drawn. */
    if (!ensure_bg_image_pipeline(m)) return false;

    /* Release old texture if any */
    renderer_destroy_background_image(r);

    /* Load image via stb_image */
    i32 w = 0, h = 0, channels = 0;
    u8 *pixels = stbi_load(path, &w, &h, &channels, 4); /* force RGBA */
    if (!pixels) {
        fprintf(stderr, "renderer_metal: failed to load background image: %s\n", path);
        return false;
    }

    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)gpu_device;
        /* sRGB wallpaper — PNG pixels are sRGB; same rationale as inline images. */
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
            width:(NSUInteger)w
            height:(NSUInteger)h
            mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;

        id<MTLTexture> tex = [dev newTextureWithDescriptor:desc];
        if (!tex) {
            stbi_image_free(pixels);
            fprintf(stderr, "renderer_metal: failed to create bg image texture\n");
            return false;
        }

        MTLRegion region = MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h);
        [tex replaceRegion:region
               mipmapLevel:0
                 withBytes:pixels
               bytesPerRow:(NSUInteger)(w * 4)];

        stbi_image_free(pixels);

        m->bgImageTexture = tex; /* retained by newTextureWithDescriptor */
        r->bg_texture = (__bridge void *)tex;
        r->bg_image_w = w;
        r->bg_image_h = h;
    }

    fprintf(stderr, "renderer_metal: loaded background image %dx%d from %s\n", w, h, path);
    return true;
}

void renderer_destroy_background_image(Renderer *r) {
    RendererMetal *m = r->backend;
    if (m && m->bgImageTexture) {
        [m->bgImageTexture release];
        m->bgImageTexture = nil;
    }
    r->bg_texture = NULL;
    r->bg_image_w = 0;
    r->bg_image_h = 0;
}

/* BgImageUniforms must match Metal shader BgImageUniforms struct */
typedef struct {
    float projection[16];
    float pos[2];
    float size[2];
    float uv_scale[2];
    float uv_offset[2];
    float opacity;
    float _pad[3];
} BgImageUniforms;

void renderer_draw_background_image(Renderer *r) {
    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder || !m->bgImageTexture || !m->bgImagePipeline) return;
    if (r->bg_opacity <= 0.001f) return;

    @autoreleasepool {
        [m->currentEncoder setRenderPipelineState:m->bgImagePipeline];

        f32 screen_w = (f32)r->screen_width;
        f32 screen_h = (f32)r->screen_height;
        f32 img_w = (f32)r->bg_image_w;
        f32 img_h = (f32)r->bg_image_h;

        BgImageUniforms uniforms;
        memcpy(uniforms.projection, r->projection, sizeof(uniforms.projection));
        uniforms.opacity = r->bg_opacity;
        uniforms._pad[0] = uniforms._pad[1] = uniforms._pad[2] = 0.0f;

        switch (r->bg_mode) {
            case 0: /* stretch */
                uniforms.pos[0] = 0; uniforms.pos[1] = 0;
                uniforms.size[0] = screen_w; uniforms.size[1] = screen_h;
                uniforms.uv_scale[0] = 1.0f; uniforms.uv_scale[1] = 1.0f;
                uniforms.uv_offset[0] = 0; uniforms.uv_offset[1] = 0;
                break;

            case 1: { /* center */
                f32 x = (screen_w - img_w) * 0.5f;
                f32 y = (screen_h - img_h) * 0.5f;
                uniforms.pos[0] = x; uniforms.pos[1] = y;
                uniforms.size[0] = img_w; uniforms.size[1] = img_h;
                uniforms.uv_scale[0] = 1.0f; uniforms.uv_scale[1] = 1.0f;
                uniforms.uv_offset[0] = 0; uniforms.uv_offset[1] = 0;
                break;
            }

            case 2: { /* tile */
                uniforms.pos[0] = 0; uniforms.pos[1] = 0;
                uniforms.size[0] = screen_w; uniforms.size[1] = screen_h;
                uniforms.uv_scale[0] = screen_w / img_w;
                uniforms.uv_scale[1] = screen_h / img_h;
                uniforms.uv_offset[0] = 0; uniforms.uv_offset[1] = 0;
                break;
            }

            case 3: /* fill (aspect) */
            default: {
                f32 scale_x = screen_w / img_w;
                f32 scale_y = screen_h / img_h;
                f32 scale = (scale_x > scale_y) ? scale_x : scale_y;
                f32 draw_w = img_w * scale;
                f32 draw_h = img_h * scale;
                f32 x = (screen_w - draw_w) * 0.5f;
                f32 y = (screen_h - draw_h) * 0.5f;
                uniforms.pos[0] = x; uniforms.pos[1] = y;
                uniforms.size[0] = draw_w; uniforms.size[1] = draw_h;
                uniforms.uv_scale[0] = 1.0f; uniforms.uv_scale[1] = 1.0f;
                uniforms.uv_offset[0] = 0; uniforms.uv_offset[1] = 0;
                break;
            }
        }

        [m->currentEncoder setVertexBytes:s_quad length:sizeof(s_quad) atIndex:0];
        [m->currentEncoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
        [m->currentEncoder setFragmentTexture:m->bgImageTexture atIndex:0];
        [m->currentEncoder setFragmentSamplerState:m->bgImageSampler atIndex:0];

        [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                              vertexStart:0
                              vertexCount:6];
    }
}

#endif /* USE_METAL */
