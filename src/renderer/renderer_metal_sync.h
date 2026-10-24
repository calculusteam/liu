/* Shared between renderer_metal.m (the frame path) and renderer_metal_sync.m
 * (the immediate helpers). Objective-C only. */
#ifndef LIU_RENDERER_METAL_SYNC_H
#define LIU_RENDERER_METAL_SYNC_H
#import <Metal/Metal.h>
#include "core/types.h"
#include <stdatomic.h>

/* Bytes per texel for the formats the atlases and images use. */
NSUInteger renderer_metal_bpp_for(MTLPixelFormat fmt);

/* The one bounds check every upload — queued or immediate — goes through.
 * The DESTINATION region is bounded by the texture; the SOURCE by
 * `src_bytes`: Metal reads h * (w * bpp) bytes and takes the stride from the
 * destination format, which the caller does not state, so a caller whose
 * buffer holds fewer bytes than that is refused rather than read past. */
bool renderer_metal_upload_validate(id<MTLTexture> tex, i32 x, i32 y, i32 w, i32 h,
                                    usize src_bytes, usize *out_row_bytes,
                                    usize *out_need, NSUInteger *out_bpp);

/* A command queue of its own for the immediate helpers — never the frame
 * queue: an immediate blit must not be ordered behind frames in flight. */
id<MTLCommandQueue> renderer_metal_sync_queue(id<MTLDevice> dev);

/* Encode a clear-to-zero of `tex` on `cb` through an empty render pass — the
 * only way to zero a private texture without staging its whole size; the
 * atlas textures are created with RenderTarget usage for exactly this. */
void renderer_metal_texture_clear_on(id<MTLCommandBuffer> cb, id<MTLTexture> tex);

/* Upload accounting (renderer_metal_upload_stats reads these). Monotonic. */
extern _Atomic unsigned long long g_liu_tex_uploads;
extern _Atomic unsigned long long g_liu_tex_bytes;
extern _Atomic unsigned long long g_liu_tex_ns;

#endif
