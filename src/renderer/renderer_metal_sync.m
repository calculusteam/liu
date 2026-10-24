/*
 * Liu — the IMMEDIATE Metal texture helpers: the seed before a window's first
 * frame, headless use, and readback.
 *
 * Its own translation unit, apart from renderer_metal.m, because a headless
 * caller that drives font.c against a real MTLDevice needs exactly these —
 * and nothing of the frame machinery, the pipelines, the layer — to put
 * texels into a PRIVATE atlas texture and read them back. Everything here
 * goes through one command queue of its own (renderer_metal_sync_queue) and
 * waits: it is never the per-frame path. The frame path is the staging queue
 * in renderer_metal.m (renderer_atlas_upload / metal_atlas_prepare), which
 * orders every glyph write behind the frames already submitted and never
 * blocks.
 *
 * Both files share the bounds check and the render-pass clear through
 * renderer_metal_sync.h.
 */
#import <Metal/Metal.h>
#include "renderer/renderer.h"
#include "renderer/renderer_metal_sync.h"
#include "core/checked.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>

/* Texture-upload accounting. Every glyph, emoji and image upload passes
 * through renderer_atlas_upload or the immediate helper below, so these are
 * the whole picture of what the CPU spends handing pixels to the GPU.
 * Monotonic and never reset — a reader keeps its own snapshot. */
_Atomic unsigned long long g_liu_tex_uploads = 0;
_Atomic unsigned long long g_liu_tex_bytes   = 0;
_Atomic unsigned long long g_liu_tex_ns      = 0;

NSUInteger renderer_metal_bpp_for(MTLPixelFormat fmt) {
    switch (fmt) {
        case MTLPixelFormatR8Unorm:        return 1;
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatRGBA8Unorm_sRGB: return 4;
        default:                            return 4;
    }
}

/* The one bounds check every upload — queued or immediate — goes through.
 * The DESTINATION region is bounded by the texture; the SOURCE by
 * `src_bytes`: Metal reads h * (w * bpp) bytes and takes the stride from the
 * destination format, which the caller does not state, so a caller whose
 * buffer holds fewer bytes than that is refused rather than read past. */
bool renderer_metal_upload_validate(id<MTLTexture> tex, i32 x, i32 y, i32 w, i32 h,
                                    usize src_bytes, usize *out_row_bytes,
                                    usize *out_need, NSUInteger *out_bpp) {
    if (!tex || x < 0 || y < 0 || w <= 0 || h <= 0) return false;
    if ((usize)x > (usize)tex.width || (usize)y > (usize)tex.height ||
        (usize)w > (usize)tex.width - (usize)x ||
        (usize)h > (usize)tex.height - (usize)y)
        return false;
    NSUInteger bpp = renderer_metal_bpp_for(tex.pixelFormat);
    usize row_bytes = 0, need = 0;
    if (!checked_mul_usize((usize)w, (usize)bpp, &row_bytes) ||
        !checked_mul_usize(row_bytes, (usize)h, &need) ||
        need > src_bytes)
        return false;
    *out_row_bytes = row_bytes;
    *out_need = need;
    *out_bpp = bpp;
    return true;
}

/* Use a separate queue for immediate helpers, including atlas seeding and
 * readback. An immediate blit must not be ordered behind frames in flight.
 */
id<MTLCommandQueue> renderer_metal_sync_queue(id<MTLDevice> dev) {
    static id<MTLCommandQueue> q = nil;
    static id<MTLDevice> qdev = nil;
    if (!q || qdev != dev) {
        if (q) [q release];
        /* newCommandQueue is a "new" method: it returns a +1 reference the
         * cache now owns, and it is not retained again. The caller gets a
         * borrowed reference and must not release it. */
        q = [dev newCommandQueue];
        qdev = dev;
    }
    return q;
}

void renderer_metal_upload_texture(void *texture, i32 x, i32 y, i32 w, i32 h,
                                   const void *data, usize src_bytes) {
    if (!texture || !data) return;
    struct timespec _u0;
    clock_gettime(CLOCK_MONOTONIC, &_u0);
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    usize row_bytes = 0, need = 0;
    NSUInteger bpp = 0;
    if (!renderer_metal_upload_validate(tex, x, y, w, h, src_bytes, &row_bytes, &need, &bpp)) return;
    MTLRegion region = MTLRegionMake2D((NSUInteger)x, (NSUInteger)y,
                                       (NSUInteger)w, (NSUInteger)h);
    if (tex.storageMode == MTLStorageModePrivate) {
        /* A private texture has no CPU path; stage and blit, and wait — this
         * is the pre-frame / headless path, never the per-frame one. */
        @autoreleasepool {
            id<MTLDevice> dev = tex.device;
            id<MTLBuffer> stage = [dev newBufferWithBytes:data length:need
                                                  options:MTLResourceStorageModeShared];
            if (!stage) return;
            id<MTLCommandBuffer> cb = [renderer_metal_sync_queue(dev) commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromBuffer:stage sourceOffset:0 sourceBytesPerRow:row_bytes
              sourceBytesPerImage:need sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                        toTexture:tex destinationSlice:0 destinationLevel:0
                 destinationOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0)];
            [blit endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            [stage release];
        }
    } else {
        [tex replaceRegion:region mipmapLevel:0 withBytes:data bytesPerRow:row_bytes];
    }
    {
        struct timespec u1;
        clock_gettime(CLOCK_MONOTONIC, &u1);
        u64 dt = (u64)(u1.tv_sec - _u0.tv_sec) * 1000000000ull
               + (u64)(u1.tv_nsec - _u0.tv_nsec);
        atomic_fetch_add(&g_liu_tex_uploads, 1ull);
        atomic_fetch_add(&g_liu_tex_bytes, (unsigned long long)need);
        atomic_fetch_add(&g_liu_tex_ns, (unsigned long long)dt);
    }
}

bool renderer_metal_texture_readback(void *texture, i32 x, i32 y, i32 w, i32 h,
                                     void *out, usize out_bytes) {
    if (!texture || !out) return false;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    usize row_bytes = 0, need = 0;
    NSUInteger bpp = 0;
    if (!renderer_metal_upload_validate(tex, x, y, w, h, out_bytes, &row_bytes, &need, &bpp)) return false;
    bool ok = false;
    @autoreleasepool {
        id<MTLDevice> dev = tex.device;
        id<MTLBuffer> stage = [dev newBufferWithLength:need options:MTLResourceStorageModeShared];
        if (!stage) return false;
        id<MTLCommandBuffer> cb = [renderer_metal_sync_queue(dev) commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0)
                   sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                     toBuffer:stage destinationOffset:0
       destinationBytesPerRow:row_bytes destinationBytesPerImage:need];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        ok = cb.status == MTLCommandBufferStatusCompleted;
        if (ok) memcpy(out, stage.contents, need);
        [stage release];
    }
    return ok;
}

/* Clear a texture to zero through a render pass — the only way to zero a
 * private texture without staging its whole size. Encoded on `cb` when
 * given, else committed and waited on the sync queue. RenderTarget usage is
 * what font.c's atlas textures are created with for exactly this. */
void renderer_metal_texture_clear_on(id<MTLCommandBuffer> cb, id<MTLTexture> tex) {
    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
    pd.colorAttachments[0].texture = tex;
    pd.colorAttachments[0].loadAction = MTLLoadActionClear;
    pd.colorAttachments[0].storeAction = MTLStoreActionStore;
    pd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pd];
    [enc endEncoding];
}

void renderer_metal_texture_clear_sync(void *texture) {
    if (!texture) return;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [renderer_metal_sync_queue(tex.device) commandBuffer];
        renderer_metal_texture_clear_on(cb, tex);
        [cb commit];
        [cb waitUntilCompleted];
    }
}

void renderer_metal_texture_copy_sync(void *src, void *dst, i32 w, i32 h) {
    if (!src || !dst || w <= 0 || h <= 0) return;
    id<MTLTexture> a = (__bridge id<MTLTexture>)src, b = (__bridge id<MTLTexture>)dst;
    if ((NSUInteger)w > a.width || (NSUInteger)h > a.height ||
        (NSUInteger)w > b.width || (NSUInteger)h > b.height) return;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [renderer_metal_sync_queue(a.device) commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:a sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                    toTexture:b destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
}

