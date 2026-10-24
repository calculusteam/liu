/* The first-run intro's renderer: two textures, one pipeline, one triangle.
 * See intro_shader.metal for the picture and intro_timeline.h for the story. */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "intro/intro_render.h"
#include "intro/intro_geom.h"
#include "intro/intro_assets.h"

#include "stb_image.h"

#include <stdlib.h>
#include <string.h>

struct IntroRenderer {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pipeline;
    id<MTLTexture> bunny;
    id<MTLTexture> captions;
};

/* The artwork's geometry reaches the shader as #defines, so it stays a set of
 * compile-time constants there and intro_geom.h stays the one place it is
 * written down. */
static NSString *shader_source(void) {
    return [NSString stringWithFormat:
        @"#define BUNNY_W %f\n#define BUNNY_H %f\n"
         "#define MARK_X0 %f\n#define MARK_Y0 %f\n#define MARK_X1 %f\n#define MARK_Y1 %f\n"
         "#define EYE_X0 %f\n#define EYE_Y0 %f\n#define EYE_Y1 %f\n"
         "#define MOUTH_X0 %f\n#define MOUTH_Y0 %f\n#define MOUTH_X1 %f\n#define MOUTH_Y1 %f\n"
         "#define FACE_SPLIT_X %f\n#define FACE_CX %f\n#define FACE_CY %f\n"
         "#define FLOOD_REACH %f\n"
         "#define CAPTION_COUNT %d\n#define CAPTION_HINTS %d\n"
         "#define CAPTION_CELL_W %f\n#define CAPTION_CELL_H %f\n"
         "#define CAPTION_SCALE %f\n%s",
        (double)INTRO_BUNNY_W, (double)INTRO_BUNNY_H,
        (double)INTRO_MARK_X0, (double)INTRO_MARK_Y0, (double)INTRO_MARK_X1, (double)INTRO_MARK_Y1,
        (double)INTRO_EYE_X0, (double)INTRO_EYE_Y0, (double)INTRO_EYE_Y1,
        (double)INTRO_MOUTH_X0, (double)INTRO_MOUTH_Y0, (double)INTRO_MOUTH_X1, (double)INTRO_MOUTH_Y1,
        (double)INTRO_FACE_SPLIT_X, (double)INTRO_FACE_CX, (double)INTRO_FACE_CY,
        (double)INTRO_FLOOD_REACH,
        (int)INTRO_CAPTION_COUNT, (int)INTRO_CAPTION_HINTS,
        (double)INTRO_CAPTION_CELL_W, (double)INTRO_CAPTION_CELL_H,
        (double)INTRO_CAPTION_SCALE,
        (const char *)INTRO_SHADER_data];
}

/* An embedded PNG as a mipmapped texture: `channels` 4 gives RGBA8 (the file's
 * RGB with an opaque alpha), 1 gives R8. The coarser levels are what the
 * shader samples for anything blurred, so they are generated here, once. */
static id<MTLTexture> load_texture(IntroRenderer *r, const u8 *png, i32 png_size, int channels) {
    if (!png || png_size <= 0) return nil;
    int w = 0, h = 0, comp = 0;
    u8 *pixels = stbi_load_from_memory(png, png_size, &w, &h, &comp, channels);
    if (!pixels) return nil;
    id<MTLTexture> tex = nil;
    if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:(channels == 4 ? MTLPixelFormatRGBA8Unorm
                                                              : MTLPixelFormatR8Unorm)
                                         width:(NSUInteger)w height:(NSUInteger)h mipmapped:YES];
        /* generateMipmaps needs a renderable texture on some GPUs. */
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        tex = [r->device newTextureWithDescriptor:desc];
        if (tex) {
            [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
                   mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)(w * channels)];
            id<MTLCommandBuffer> buffer = [r->queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
            [blit generateMipmapsForTexture:tex];
            [blit endEncoding];
            [buffer commit];
            [buffer waitUntilCompleted];
        }
    }
    stbi_image_free(pixels);
    return tex;
}

IntroRenderer *intro_renderer_create(id<MTLDevice> device, i32 lang) {
    if (!device) return NULL;
    IntroRenderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    @autoreleasepool {
        r->device = [device retain];
        r->queue = [device newCommandQueue];

        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:shader_source() options:nil error:&error];
        if (library) {
            MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
            id<MTLFunction> vertex = [library newFunctionWithName:@"liu_intro_vertex"];
            id<MTLFunction> fragment = [library newFunctionWithName:@"liu_intro_fragment"];
            desc.vertexFunction = vertex;
            desc.fragmentFunction = fragment;
            desc.colorAttachments[0].pixelFormat = INTRO_PIXEL_FORMAT;
            r->pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            [vertex release];
            [fragment release];
            [desc release];
            [library release];
        }
        if (!r->pipeline) {
            NSLog(@"[intro] shader: %@", error);
        } else if (r->queue) {
            r->bunny = load_texture(r, INTRO_BUNNY_LAYERS_data, INTRO_BUNNY_LAYERS_size, 4);
            /* One atlas per language is embedded; only the reader's is decoded. */
            _Static_assert(sizeof INTRO_CAPTIONS_by_lang / sizeof INTRO_CAPTIONS_by_lang[0] == INTRO_LANG_COUNT,
                           "one caption atlas per language in INTRO_LANG_CODES");
            i32 atlas = lang >= 0 && lang < INTRO_LANG_COUNT ? lang : 0;
            r->captions = load_texture(r, INTRO_CAPTIONS_by_lang[atlas].data,
                                       INTRO_CAPTIONS_by_lang[atlas].size, 4);
        }
    }
    if (!r->pipeline || !r->queue || !r->bunny || !r->captions) {
        intro_renderer_destroy(r);
        return NULL;
    }
    return r;
}

void intro_renderer_destroy(IntroRenderer *r) {
    if (!r) return;
    [r->captions release];
    [r->bunny release];
    [r->pipeline release];
    [r->queue release];
    [r->device release];
    free(r);
}

static void encode(IntroRenderer *r, const IntroFrame *f, const IntroLayout *l, f64 time,
                   f32 pixels_w, f32 pixels_h, MTLRenderPassDescriptor *pass,
                   id<MTLCommandBuffer> buffer) {
    id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return;
    /* Mirrors `IntroUniforms` in the shader: eleven float4s. */
    const f32 uniforms[11][4] = {
        { pixels_w, pixels_h, l->scale, (f32)time },
        { l->center_x, l->center_y, l->unit, f->dim },
        { f->cursor, f->prompt, f->zoom, f->camera },
        { f->flood, f->glow, f->haze, f->pulse },
        { 0.0f, f->spark, f->dissolve, f->morph },             /* .x is free */
        { f->frame_alpha, f->reveal, f->out, l->target_radius },
        { l->target_cx, l->target_cy, l->target_hw, l->target_hh },
        { 0.0f, 0.0f, 0.0f, 0.0f },                             /* free */
        { f->whole_window, l->dim_strength, f->wipes, f->mark_alpha },
        { f->uncover, f->plate, f->feature, f->caption },
        { f->caption_rise, f->hint, 0.0f, 0.0f },
    };
    [encoder setRenderPipelineState:r->pipeline];
    [encoder setFragmentBytes:uniforms length:sizeof uniforms atIndex:0];
    [encoder setFragmentTexture:r->bunny atIndex:0];
    [encoder setFragmentTexture:r->captions atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

bool intro_renderer_draw(IntroRenderer *r, const IntroFrame *f, const IntroLayout *l,
                         f64 time, CAMetalLayer *layer) {
    if (!r || !layer) return false;
    bool drawn = false;
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        id<MTLCommandBuffer> buffer = drawable ? [r->queue commandBuffer] : nil;
        if (buffer) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            encode(r, f, l, time, (f32)drawable.texture.width, (f32)drawable.texture.height,
                   pass, buffer);
            [buffer presentDrawable:drawable];
            [buffer commit];
            drawn = true;
        }
    }
    return drawn;
}

u8 *intro_renderer_image(IntroRenderer *r, const IntroFrame *f, const IntroLayout *l,
                         f64 time, i32 *out_w, i32 *out_h) {
    if (!r) return NULL;
    i32 width = (i32)(l->view_w * l->scale + 0.5f);
    i32 height = (i32)(l->view_h * l->scale + 0.5f);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return NULL;
    u8 *bytes = NULL;
    @autoreleasepool {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:INTRO_PIXEL_FORMAT
                                         width:(NSUInteger)width height:(NSUInteger)height
                                     mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
#if TARGET_CPU_ARM64
        desc.storageMode = MTLStorageModeShared;
#else
        desc.storageMode = MTLStorageModeManaged;
#endif
        id<MTLTexture> texture = [r->device newTextureWithDescriptor:desc];
        id<MTLCommandBuffer> buffer = texture ? [r->queue commandBuffer] : nil;
        if (buffer) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            encode(r, f, l, time, (f32)width, (f32)height, pass, buffer);
#if !TARGET_CPU_ARM64
            id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
            [blit synchronizeResource:texture];
            [blit endEncoding];
#endif
            [buffer commit];
            [buffer waitUntilCompleted];
            bytes = malloc((usize)width * (usize)height * 4);
            if (bytes) {
                [texture getBytes:bytes bytesPerRow:(NSUInteger)width * 4
                       fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
                      mipmapLevel:0];
            }
        }
        [texture release];
    }
    if (bytes) {
        if (out_w) *out_w = width;
        if (out_h) *out_h = height;
    }
    return bytes;
}
