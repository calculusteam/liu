/* IOSurface presentation. The GPU completes before CALayer receives a surface.
 * A bounded pool excludes both GPU leases and compositor-owned surfaces. */
#ifndef LIU_METAL_SURFACE_H
#define LIU_METAL_SURFACE_H
#import <IOSurface/IOSurface.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAAnimation.h>
#import <QuartzCore/CAMediaTimingFunction.h>
#include <math.h>
#include <string.h>

/* A compact image must save enough to repay one background scan/copy. It is
 * never a lower-resolution image: whole BGRA pixels and their scale survive. */
#define LIU_SURFACE_COMPACT_MIN_BYTES (1024u*1024u)
/* What a frame does with each hand-off rect (LiuSurfaceDrawable.fade_mode). */
enum { LIU_FADE_END = 0, LIU_FADE_KEEP = 1, LIU_FADE_START = 2 };
#define LIU_SURFACE_CACHE_SEC 2.0
static bool active_target_enabled(void) {
    const char *flag=liu_dev_env("LIU_ACTIVE_TARGET");return flag && flag[0]=='1';
}

@interface LiuSurfaceImage : NSObject {
@public
    IOSurfaceRef surface;
    id<MTLTexture> texture;
    _Atomic bool leased;
    usize accounted;
    f64 last_required;
    u64 lease_serial;   /* the drawable that last leased it (a hand-off's source check) */
}
- (instancetype)initWithDevice:(id<MTLDevice>)device width:(NSUInteger)w height:(NSUInteger)h;
@end
@implementation LiuSurfaceImage
- (instancetype)initWithDevice:(id<MTLDevice>)device width:(NSUInteger)w height:(NSUInteger)h {
    if (!(self=[super init])) return nil;
    atomic_init(&leased,false);
    if (!w || !h || w>16384 || h>16384) { [self release];return nil; }
    NSDictionary *properties=@{(id)kIOSurfaceWidth:@(w),(id)kIOSurfaceHeight:@(h),
        (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfacePixelFormat:@(0x42475241)};
    surface=IOSurfaceCreate((CFDictionaryRef)properties);
    if (!surface) { [self release];return nil; }
    CGColorSpaceRef colors=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (colors) {
        CFPropertyListRef description=CGColorSpaceCopyPropertyList(colors);
        if (description) { IOSurfaceSetValue(surface,kIOSurfaceColorSpace,description);CFRelease(description); }
        CGColorSpaceRelease(colors);
    }
    MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB width:w height:h mipmapped:NO];
    desc.storageMode=device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    texture=[device newTextureWithDescriptor:desc iosurface:surface plane:0];
    if (!texture) { [self release];return nil; }
    accounted=IOSurfaceGetAllocSize(surface);
    mem_budget_transient_adjust(MEM_RENDER_GPU,(isize)accounted);
    return self;
}
- (void)dealloc {
    [texture release];if(surface)CFRelease(surface);
    mem_budget_transient_adjust(MEM_RENDER_GPU,-(isize)accounted);
    [super dealloc];
}
@end

@class LiuSurfaceDrawable;
@interface LiuSurfacePresenter : NSObject {
@public
    CAMetalLayer *host;
    CALayer *display;
    CALayer *cursor_layer;
    CALayer *terminal_layers[RENDERER_TERMINAL_RECT_CAP];
    CALayer *backfill;
    /* Hand-off fades (renderer_fade_note): a layer per rect showing what the
     * screen had there, fading out over the frame drawn under it, and the
     * small images those pixels are copied into. */
    CALayer *fade_layers[RENDERER_FADE_CAP];
    LiuSurfaceImage *fade_shows[RENDERER_FADE_CAP];   /* what each live layer shows */
    RendererFade fade_live[RENDERER_FADE_CAP];        /* each live layer's owner, rect, picture */
    LiuSurfaceImage *fade_images[3];
    u64 fades_started, fades_kept, fades_ended;
    id<MTLDevice> device;
    LiuSurfaceImage *images[3], *shown;
    LiuSurfaceImage *snapshot;  /* owns a compact image outside the drawable pool */
    u64 next_serial, published;
    _Atomic bool stopped;
    _Atomic(void (*)(void)) wake;
    /* A frame is trying to, or failed to, take a surface: the next one handed
     * back wakes the loop for it. Raised before each acquisition and lowered
     * when it succeeds, so a release between the two still wakes. */
    _Atomic bool waiting;
    bool wake_always;
    u64 accepted, discarded, starved;
    bool allocation_failed;
    bool cursor_blink;
    NSUInteger width, height;
    NSUInteger target_width, target_height;
    u64 pool_reuses, pool_allocations, pool_retired;
    bool compact_allowed, compact_pending;
    u32 compact_background;
    u64 compact_attempted, compacted, compact_discarded;
    f64 compact_work_sec;
    /* LIU_SURFACE_PROBE=1: what the first frame after a quiet spell cost —
     * from the first attempt to acquire a surface to that frame being put on
     * the layer, and what stood in the way (no free slot, a fresh surface). */
    f64 probe_last_publish, probe_resume_t0;
    u64 probe_starved0, probe_alloc0;
}
- (instancetype)initWithLayer:(CAMetalLayer *)layer device:(id<MTLDevice>)dev;
- (id<CAMetalDrawable>)nextWidth:(NSUInteger)w height:(NSUInteger)h;
- (id<CAMetalDrawable>)nextRecordingWidth:(NSUInteger)w height:(NSUInteger)h;
- (void)publish:(LiuSurfaceDrawable *)drawable;
- (bool)resolve:(LiuSurfaceDrawable *)drawable width:(NSUInteger)w height:(NSUInteger)h;
- (void)stop;
- (usize)trimIdle:(bool)idle;
- (void)updateCursor:(RendererCursor)cursor;
- (bool)prepareTerminalRects:(u32)count;
- (void)updateTerminalRects:(const RendererTerminalRect *)rects count:(u32)count;
- (void)compactIfIdle;
- (LiuSurfaceImage *)fadeImageWidth:(NSUInteger)w height:(NSUInteger)h;
- (void)publishFades:(LiuSurfaceDrawable *)drawable;
@end

@interface LiuSurfaceDrawable : NSObject <CAMetalDrawable> {
@public
    LiuSurfacePresenter *owner;
    LiuSurfaceImage *image;
    u64 serial;
    NSUInteger logical_width,logical_height;
    NSMutableArray *handlers;
    RendererCursor cursor;
    RendererTerminalRect terminal_rects[RENDERER_TERMINAL_RECT_CAP];
    u32 terminal_rect_count;
    /* The frame's hand-off fades: what it drew where, and for each whether a
     * fade starts (its pixels at fade_slice_x of fade_image), goes on, or is
     * ended (LIU_FADE_*). */
    RendererFade fades[RENDERER_FADE_CAP];
    u8 fade_mode[RENDERER_FADE_CAP];
    u32 fade_slice_x[RENDERER_FADE_CAP];
    u32 fade_count;
    LiuSurfaceImage *fade_image;   /* retained and leased while the frame is in flight */
    bool compact_allowed;
    u32 compact_background;
    /* Host time at which this frame's surface became the layer's contents
     * (0 = it never did). NOT a presentation time — the compositor shows it at
     * its next refresh — and deliberately not what -presentedTime returns. */
    CFTimeInterval layer_time;
}
- (instancetype)initWithOwner:(LiuSurfacePresenter *)presenter image:(LiuSurfaceImage *)value;
- (CFTimeInterval)liuLayerTime;
@end
@implementation LiuSurfaceDrawable
- (instancetype)initWithOwner:(LiuSurfacePresenter *)presenter image:(LiuSurfaceImage *)value {
    if ((self=[super init])) { owner=[presenter retain];image=[value retain];serial=++owner->next_serial;
        image->lease_serial=serial;
        logical_width=value->texture.width;logical_height=value->texture.height; }
    return self;
}
- (id<MTLTexture>)texture { return image->texture; }
- (CAMetalLayer *)layer { return owner->host; }
- (NSUInteger)drawableID { return (NSUInteger)serial; }
/* CALayer publication has no hardware presentation timestamp. Reporting zero
 * makes native presentation observers classify it as unavailable. */
- (CFTimeInterval)presentedTime { return 0; }
/* What IS known on this path: when the frame was handed to Core Animation.
 * It gives the default presenter a key-to-frame figure where it would
 * otherwise have no sample. The caller labels it for what it is ("on
 * layer"). */
- (CFTimeInterval)liuLayerTime { return layer_time; }
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)handler {
    if (!handlers) handlers=[NSMutableArray new];
    id copy=[handler copy];[handlers addObject:copy];[copy release];
}
- (void)present { [owner publish:self]; }
- (void)presentAtTime:(CFTimeInterval)time { (void)time;[self present]; }
- (void)presentAfterMinimumDuration:(CFTimeInterval)duration { (void)duration;[self present]; }
- (void)dealloc {
    atomic_store_explicit(&image->leased,false,memory_order_release);
    /* Only a frame that is waiting for a surface needs the loop woken for
     * this one: every frame ends here, and an unconditional wake is a loop
     * turn per presented frame with nothing to do. */
    void (*notify)(void)=atomic_load(&owner->wake);
    if (!atomic_load(&owner->stopped) && notify &&
        (atomic_exchange_explicit(&owner->waiting,false,memory_order_acq_rel) || owner->wake_always)) notify();
    if (fade_image) { atomic_store_explicit(&fade_image->leased,false,memory_order_release);[fade_image release]; }
    [handlers release];[image release];[owner release];[super dealloc];
}
@end

/* Called only on a utility worker with an exclusive lease on a completed
 * shared IOSurface. No explicit GPU transfer buffer, full-frame malloc or
 * main-thread wait. A busy/full/transparent image simply keeps its ordinary presentation. */
static LiuSurfaceImage *liu_surface_compact_image(LiuSurfaceImage *source,
                                                  id<MTLDevice> device,u32 bg) {
    if (!source || !device.hasUnifiedMemory || (bg>>24)!=255) return nil;
    IOSurfaceRef surface=source->surface;
    usize w=IOSurfaceGetWidth(surface),h=IOSurfaceGetHeight(surface);
    if (!w || !h || source->accounted<=LIU_SURFACE_COMPACT_MIN_BYTES) return nil;
    if (IOSurfaceLock(surface,kIOSurfaceLockReadOnly,NULL)!=kIOReturnSuccess) return nil;
    const u8 *base=IOSurfaceGetBaseAddress(surface);
    usize pitch=IOSurfaceGetBytesPerRow(surface),used=h;
    if (!base || pitch<w*4u) { IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL);return nil; }
    while (used>0) {
        const u32 *row=(const u32 *)(base+(used-1)*pitch);
        bool blank=true;
        for (usize x=0;x<w;x++) if(row[x]!=bg) { blank=false;break; }
        if(!blank)break;
        used--;
    }
    usize rows=(used+15u)&~(usize)15u;if(!rows)rows=16;
    LiuSurfaceImage *result=nil;
    if (rows<h && rows<=h*3u/4u &&
        source->accounted>w*rows*4u+LIU_SURFACE_COMPACT_MIN_BYTES) {
        /* The background is behind the small image, so preserve source-over
         * semantics by rejecting any transparency in the part that is kept
         * too. */
        bool opaque=true;
        for (usize y=0;y<rows && opaque;y++) {
            const u32 *row=(const u32 *)(base+y*pitch);
            for(usize x=0;x<w;x++)if((row[x]>>24)!=255){opaque=false;break;}
        }
        if (opaque) result=[[LiuSurfaceImage alloc]initWithDevice:device width:w height:rows];
        if (result) {
            if (source->accounted<=result->accounted+LIU_SURFACE_COMPACT_MIN_BYTES ||
                IOSurfaceLock(result->surface,0,NULL)!=kIOReturnSuccess) {
                [result release];result=nil;
            } else {
                u8 *dst=IOSurfaceGetBaseAddress(result->surface);
                usize dst_pitch=IOSurfaceGetBytesPerRow(result->surface);
                if (dst && dst_pitch>=w*4u) {
                    for(usize y=0;y<rows;y++)memcpy(dst+y*dst_pitch,base+y*pitch,w*4u);
                } else { IOSurfaceUnlock(result->surface,0,NULL);[result release];result=nil; }
                if (result) IOSurfaceUnlock(result->surface,0,NULL);
            }
        }
    }
    IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL);
    return result;
}

@implementation LiuSurfacePresenter
- (instancetype)initWithLayer:(CAMetalLayer *)layer device:(id<MTLDevice>)dev {
    if (!(self=[super init])) return nil;
    atomic_init(&stopped,false);atomic_init(&wake,NULL);host=[layer retain];device=[dev retain];
    /* LIU_SURFACE_WAKE_ALWAYS=1: every surface handed back wakes the loop, as
     * it did before `waiting`. Resolved here, on the main thread. */
    const char *wa=getenv("LIU_SURFACE_WAKE_ALWAYS");
    atomic_init(&waiting,false);wake_always=wa && wa[0]=='1';
    display=[CALayer new];display.frame=host.bounds;
    if (!display) { [self release];return nil; }
    display.autoresizingMask=kCALayerWidthSizable|kCALayerHeightSizable;
    /* AppKit manages the backing layer's coordinate orientation. The surface
     * already matches it; adding a content flip would invert the window. */
    display.contentsGravity=kCAGravityTopLeft;display.contentsScale=host.contentsScale;
    display.actions=@{@"contents":[NSNull null],@"bounds":[NSNull null],@"position":[NSNull null]};
    [host addSublayer:display];
    cursor_layer=[CALayer new];
    cursor_layer.hidden=YES;
    cursor_layer.actions=@{@"hidden":[NSNull null],@"bounds":[NSNull null],
        @"position":[NSNull null],@"backgroundColor":[NSNull null],@"opacity":[NSNull null]};
    [host addSublayer:cursor_layer];
    return self;
}
- (bool)resolve:(LiuSurfaceDrawable *)drawable width:(NSUInteger)w height:(NSUInteger)h {
    if(atomic_load(&stopped) || !drawable || drawable->owner!=self || !w || !h ||
       w!=drawable->logical_width || h>drawable->logical_height ||
       !atomic_load_explicit(&drawable->image->leased,memory_order_acquire))return false;
    u32 slot=3;
    for(u32 i=0;i<3;i++)if(images[i]==drawable->image){slot=i;break;}
    if(slot==3)return false;
    LiuSurfaceImage *previous=drawable->image;
    if(previous->texture.width!=w || previous->texture.height!=h) {
        LiuSurfaceImage *matching=nil;
        for(u32 i=0;i<3;i++) {
            LiuSurfaceImage *candidate=images[i];
            if(candidate && candidate!=shown && candidate!=previous &&
               candidate->texture.width==w && candidate->texture.height==h &&
               !atomic_load_explicit(&candidate->leased,memory_order_acquire) && !IOSurfaceIsInUse(candidate->surface)) {
                matching=candidate;break;
            }
        }
        if(matching) {
            atomic_store(&matching->leased,true);
            drawable->image=[matching retain];
            atomic_store_explicit(&previous->leased,false,memory_order_release);
            [previous release]; /* only the drawable; its pool owner survives */
            pool_reuses++;
        } else {
            LiuSurfaceImage *replacement=[[LiuSurfaceImage alloc]initWithDevice:device width:w height:h];
            if(!replacement){allocation_failed=true;return false;}
            atomic_store(&replacement->leased,true);
            drawable->image=[replacement retain];images[slot]=replacement;
            atomic_store_explicit(&previous->leased,false,memory_order_release);
            [previous release];[previous release]; /* drawable + replaced pool owner */
            pool_allocations++;
        }
    }
    target_width=w;target_height=h;
    drawable->image->last_required=CACurrentMediaTime();
    drawable->image->lease_serial=drawable->serial;
    return true;
}
/* What a spare is kept FOR is the next frame, so while frames keep coming a
 * spare stays only as long as it was recently asked for (LIU_SURFACE_CACHE_SEC).
 *
 * A spare is not kept on size alone. A burst needs the THIRD surface (the
 * compositor still holds the previous frame while the GPU holds the next), but
 * an agent's spinner or seconds counter keeps frames coming for minutes at a
 * rate two surfaces serve, so the renderer never goes idle and the third would
 * sit there unused — a full-window surface's worth of memory.
 *
 * Getting it back when a burst wants it again is one IOSurfaceCreate and one
 * pass over fresh pages, once. LIU_SURFACE_KEEP=1 also keeps any spare of the
 * current target size until the renderer is idle. */
static bool liu_surface_keep_exact(void) {
    static i32 keep=-1;
    if (keep<0) { const char *flag=getenv("LIU_SURFACE_KEEP");keep=(flag && flag[0]=='1')?1:0; }
    return keep==1;
}
- (usize)trimIdle:(bool)idle {
    usize freed=0;
    f64 now=CACurrentMediaTime();
    NSUInteger needed_w=target_width?target_width:width;
    NSUInteger needed_h=target_height?target_height:height;
    bool keep_exact=liu_surface_keep_exact();
    for (u32 i=0;i<3;i++) {
        LiuSurfaceImage *candidate=images[i];
        if (!candidate || candidate==shown ||
            atomic_load_explicit(&candidate->leased,memory_order_acquire) ||
            IOSurfaceIsInUse(candidate->surface)) continue;
        bool recent=candidate->last_required>0 && now-candidate->last_required<LIU_SURFACE_CACHE_SEC;
        bool exact=candidate->texture.width==needed_w && candidate->texture.height==needed_h;
        if (!idle && (recent || (keep_exact && exact))) continue;
        freed+=candidate->accounted;
        images[i]=nil;pool_retired++;
        [candidate release];
    }
    if (idle) {
        for (u32 i=0;i<3;i++) {
            LiuSurfaceImage *candidate=fade_images[i];
            if (!candidate || atomic_load_explicit(&candidate->leased,memory_order_acquire) ||
                IOSurfaceIsInUse(candidate->surface)) continue;
            bool shows=false;
            for (u32 k=0;k<RENDERER_FADE_CAP;k++) if (fade_shows[k]==candidate) { shows=true;break; }
            if (shows) continue;
            freed+=candidate->accounted;
            fade_images[i]=nil;
            [candidate release];
        }
    }
    return freed;
}
- (void)compactIfIdle {
    if (atomic_load(&stopped) || !compact_allowed || compact_pending || snapshot ||
        !shown || !published || next_serial!=published || compact_attempted==published ||
        !device.hasUnifiedMemory) return;
    const char *mode=liu_dev_env("LIU_IDLE_SNAPSHOT");
    if(mode && mode[0]=='0')return;
    /* The worker's lease prevents this slot being recycled if another frame
     * replaces `shown` while its pixels are being read. A retain alone would
     * protect the object but not the contents. */
    bool expected=false;
    if(!atomic_compare_exchange_strong(&shown->leased,&expected,true))return;
    compact_pending=true;compact_attempted=published;
    LiuSurfaceImage *source=shown;
    u64 serial=published;
    u32 bg=compact_background;
    /* USER_INITIATED, not UTILITY. The job is a few milliseconds, once, after
     * two idle seconds — but it holds a LEASE on the surface that is on screen,
     * and the lease is only returned when the job has run. UTILITY is the class
     * that starves on a loaded machine, so the job could leave the pool one
     * slot short for as long as the machine is busy — exactly when the user
     * comes back and frames are wanted. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{ @autoreleasepool {
        f64 start=platform_time_sec();
        LiuSurfaceImage *small=atomic_load(&self->stopped) ? nil : liu_surface_compact_image(source,self->device,bg);
        f64 elapsed=platform_time_sec()-start;
        /* Blocks retain self/source/small. Closing a window never waits for
         * this worker and never leaves it pointing into freed RendererMetal. */
        dispatch_async(dispatch_get_main_queue(),^{
            self->compact_pending=false;self->compact_work_sec+=elapsed;
            atomic_store_explicit(&source->leased,false,memory_order_release);
            CGSize pixels=CGSizeMake(self->host.bounds.size.width*self->host.contentsScale,
                                     self->host.bounds.size.height*self->host.contentsScale);
            bool valid=!atomic_load(&self->stopped) && self->shown==source &&
                self->published==serial && self->next_serial==serial && self->compact_allowed &&
                source->texture.width==(NSUInteger)llround(pixels.width) &&
                source->texture.height==(NSUInteger)llround(pixels.height);
            if(small && valid) {
                if(!self->backfill) {
                    self->backfill=[CALayer new];
                    self->backfill.autoresizingMask=kCALayerWidthSizable|kCALayerHeightSizable;
                    self->backfill.actions=@{@"hidden":[NSNull null],@"bounds":[NSNull null],
                        @"position":[NSNull null],@"backgroundColor":[NSNull null]};
                    [self->host insertSublayer:self->backfill below:self->display];
                }
                if(self->backfill) {
                    CGColorSpaceRef space=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                    CGFloat rgba[]={((bg>>16)&255)/255.0,((bg>>8)&255)/255.0,(bg&255)/255.0,1};
                    CGColorRef color=space ? CGColorCreate(space,rgba) : NULL;
                    if(color) {
                        CGFloat scale=self->host.contentsScale;
                        CGRect frame=self->host.bounds;
                        frame.size=CGSizeMake(small->texture.width/scale,small->texture.height/scale);
                        if(!self->host.geometryFlipped)frame.origin.y+=self->host.bounds.size.height-frame.size.height;
                        [CATransaction begin];[CATransaction setDisableActions:YES];
                        self->backfill.frame=self->host.bounds;self->backfill.backgroundColor=color;
                        self->backfill.hidden=NO;
                        self->display.autoresizingMask=self->host.geometryFlipped ? 0 : kCALayerMinYMargin;
                        self->display.frame=frame;self->display.contentsGravity=kCAGravityResize;
                        self->display.contents=(id)small->surface;
                        self->snapshot=[small retain];self->shown=small;
                        [CATransaction commit];self->compacted++;
                        [self trimIdle:true];
                        CGColorRelease(color);
                    }
                    if(space)CGColorSpaceRelease(space);
                }
            } else if(small)self->compact_discarded++;
            void (*notify)(void)=atomic_load(&self->wake);
            if(!atomic_load(&self->stopped) && notify)notify();
        });
        [small release];
    }});
}
- (void)updateCursor:(RendererCursor)value {
    bool visible=value.visible && value.w>0 && value.h>0;
    cursor_layer.hidden=!visible;
    bool blink=visible && value.blink;
    if (visible) {
        CGFloat scale=host.contentsScale>0 ? host.contentsScale : 1;
        CGFloat y=value.y/scale;
        if (!host.geometryFlipped) y=host.bounds.size.height-y-value.h/scale;
        cursor_layer.frame=CGRectMake(value.x/scale,y,value.w/scale,value.h/scale);
        cursor_layer.contentsScale=scale;
        /* Match the renderer's linear-light colors; CA handles display-space
         * conversion, and the existing translucent block leaves text legible. */
        Color linear=color_linear_from_srgb(value.color);
        CGColorSpaceRef space=CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
        CGFloat components[]={linear.r,linear.g,linear.b,linear.a};
        CGColorRef color=CGColorCreate(space,components);
        cursor_layer.backgroundColor=color;
        CGColorRelease(color);CGColorSpaceRelease(space);
    }
    if (blink!=cursor_blink) {
        cursor_blink=blink;
        [cursor_layer removeAnimationForKey:@"liu.cursor.blink"];
        cursor_layer.opacity=1;
        if (blink) {
            /* Same 1.1 s / 0.55..1 pulse as the rect path. Animation stays in
             * the render server; idle Liu does not submit Metal command buffers. */
            NSMutableArray *values=[NSMutableArray arrayWithCapacity:65];
            for (u32 i=0;i<=64;i++)
                [values addObject:@(0.775+0.225*sin((double)i*2*M_PI/64))];
            CAKeyframeAnimation *animation=[CAKeyframeAnimation animationWithKeyPath:@"opacity"];
            animation.values=values;animation.duration=1.1;
            animation.repeatCount=HUGE_VALF;
            animation.beginTime=[cursor_layer convertTime:platform_time_origin() fromLayer:nil];
            [cursor_layer addAnimation:animation forKey:@"liu.cursor.blink"];
        }
    }
}
- (bool)prepareTerminalRects:(u32)count {
    if (count>RENDERER_TERMINAL_RECT_CAP) return false;
    for (u32 i=0;i<count;i++) {
        if (terminal_layers[i]) continue;
        CALayer *layer=[CALayer new];
        if (!layer) return false;
        layer.hidden=YES;
        layer.actions=@{@"hidden":[NSNull null],@"bounds":[NSNull null],
            @"position":[NSNull null],@"backgroundColor":[NSNull null],
            @"cornerRadius":[NSNull null]};
        [host addSublayer:layer];terminal_layers[i]=layer;
    }
    return true;
}
- (void)updateTerminalRects:(const RendererTerminalRect *)rects count:(u32)count {
    CGFloat scale=host.contentsScale>0 ? host.contentsScale : 1;
    CGColorSpaceRef space=CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    for (u32 i=0;i<RENDERER_TERMINAL_RECT_CAP;i++) {
        CALayer *layer=terminal_layers[i];
        if (!layer) continue;
        layer.hidden=i>=count;
        if (i>=count) continue;
        RendererTerminalRect value=rects[i];
        CGFloat y=value.y/scale;
        if (!host.geometryFlipped) y=host.bounds.size.height-y-value.h/scale;
        layer.frame=CGRectMake(value.x/scale,y,value.w/scale,value.h/scale);
        layer.contentsScale=scale;layer.cornerRadius=value.radius/scale;
        Color linear=color_linear_from_srgb(value.color);
        CGFloat components[]={linear.r,linear.g,linear.b,linear.a};
        CGColorRef color=space ? CGColorCreate(space,components) : NULL;
        layer.backgroundColor=color;
        if (color) CGColorRelease(color);
    }
    if (space) CGColorSpaceRelease(space);
}
/* An image to copy a frame's hand-off pixels into, `w` x `h` or larger,
 * leased to the caller: none a live fade shows, none in flight, none the
 * compositor still holds. Three are kept; nil when all three are busy (the
 * frame then hands nothing off). Main thread. */
- (LiuSurfaceImage *)fadeImageWidth:(NSUInteger)w height:(NSUInteger)h {
    if (atomic_load(&stopped) || !w || !h) return nil;
    u32 spare=3;
    for (u32 i=0;i<3;i++) {
        LiuSurfaceImage *candidate=fade_images[i];
        if (!candidate) { if (spare==3) spare=i; continue; }
        if (atomic_load_explicit(&candidate->leased,memory_order_acquire) ||
            IOSurfaceIsInUse(candidate->surface)) continue;
        bool shows=false;
        for (u32 k=0;k<RENDERER_FADE_CAP;k++) if (fade_shows[k]==candidate) { shows=true;break; }
        if (shows) continue;
        if (candidate->texture.width>=w && candidate->texture.height>=h) {
            atomic_store(&candidate->leased,true);
            return candidate;
        }
        if (spare==3) spare=i;   /* too small and free: replaced */
    }
    if (spare==3) return nil;
    /* Room for a little growth, so a tab that widens by a pixel does not
     * replace it every frame. */
    NSUInteger aw=(w+63)&~(NSUInteger)63, ah=(h+15)&~(NSUInteger)15;
    LiuSurfaceImage *made=[[LiuSurfaceImage alloc]initWithDevice:device width:aw height:ah];
    if (!made) return nil;
    [fade_images[spare] release];
    fade_images[spare]=made;
    atomic_store(&made->leased,true);
    return made;
}
static bool liu_fade_same_place(const RendererFade *a,const RendererFade *b) {
    return a->owner==b->owner && a->x==b->x && a->y==b->y && a->w==b->w && a->h==b->h;
}
- (void)endFadeAt:(u32)i {
    CALayer *layer=fade_layers[i];
    [layer removeAnimationForKey:@"liu.fade"];
    layer.hidden=YES;layer.contents=nil;
    [fade_shows[i] release];fade_shows[i]=nil;
    fades_ended++;
}
/* Inside publish's transaction, with the frame's own contents: a live fade
 * goes on only where the frame says KEEP for its owner at its rect; every
 * START puts what the screen had there over the new frame, opaque, and fades
 * it out by the rect's `until`. */
- (void)publishFades:(LiuSurfaceDrawable *)drawable {
    for (u32 i=0;i<RENDERER_FADE_CAP;i++) {
        if (!fade_shows[i]) continue;
        bool keep=false;
        for (u32 k=0;k<drawable->fade_count;k++)
            if (drawable->fade_mode[k]==LIU_FADE_KEEP && liu_fade_same_place(&drawable->fades[k],&fade_live[i])) {
                keep=true;break;
            }
        if (keep) fades_kept++;
        else [self endFadeAt:i];
    }
    LiuSurfaceImage *image=drawable->fade_image;
    if (!image) return;
    CGFloat scale=host.contentsScale>0 ? host.contentsScale : 1;
    f64 now=platform_time_sec();
    f64 iw=(f64)image->texture.width, ih=(f64)image->texture.height;
    for (u32 k=0;k<drawable->fade_count;k++) {
        if (drawable->fade_mode[k]!=LIU_FADE_START) continue;
        const RendererFade *f=&drawable->fades[k];
        f64 left=f->until-now;
        if (left<0.004) continue;   /* its key passed while the frame was in flight */
        u32 slot=RENDERER_FADE_CAP;
        for (u32 i=0;i<RENDERER_FADE_CAP;i++) if (!fade_shows[i]) { slot=i;break; }
        if (slot==RENDERER_FADE_CAP) break;
        CALayer *layer=fade_layers[slot];
        if (!layer) {
            layer=[CALayer new];
            if (!layer) break;
            layer.actions=@{@"hidden":[NSNull null],@"bounds":[NSNull null],@"position":[NSNull null],
                @"contents":[NSNull null],@"contentsRect":[NSNull null],@"opacity":[NSNull null]};
            layer.contentsGravity=kCAGravityResize;
            layer.magnificationFilter=kCAFilterNearest;layer.minificationFilter=kCAFilterNearest;
            [host insertSublayer:layer above:display];
            fade_layers[slot]=layer;
        }
        CGFloat y=f->y/scale;
        if (!host.geometryFlipped) y=host.bounds.size.height-y-f->h/scale;
        layer.frame=CGRectMake(f->x/scale,y,f->w/scale,f->h/scale);
        layer.contentsScale=scale;
        layer.contents=(id)image->surface;
        /* The copy is the image's TOP f->h rows, and the image can be taller
         * (fadeImageWidth rounds it up). The unit square counts from the
         * bottom unless the geometry is flipped, as the frame above does. */
        CGFloat cy=host.geometryFlipped ? 0 : (ih-f->h)/ih;
        layer.contentsRect=CGRectMake(drawable->fade_slice_x[k]/iw,cy,f->w/iw,f->h/ih);
        layer.hidden=NO;
        /* Opaque pixels at an opacity going linearly from 1 to 0: the
         * compositor's mix of the two pictures, in its encoded space, is the
         * per-cell sRGB mix a frame drawn between the keys would show. */
        layer.opacity=0;
        CABasicAnimation *fade=[CABasicAnimation animationWithKeyPath:@"opacity"];
        fade.fromValue=@1.0;fade.toValue=@0.0;fade.duration=left;
        fade.timingFunction=[CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionLinear];
        [layer addAnimation:fade forKey:@"liu.fade"];
        fade_shows[slot]=[image retain];
        fade_live[slot]=*f;
        fades_started++;
    }
}
/* Acquire a free role once. The role count remains three even if stale
 * storage was retired. Prefer exact storage; never borrow a GPU/compositor
 * lease or the surface currently shown by the layer. */
- (id<CAMetalDrawable>)nextWidth:(NSUInteger)w height:(NSUInteger)h {
    if(atomic_load(&stopped))return nil;
    width=target_width=w;height=target_height=h;
    id<CAMetalDrawable> result=[self nextRecordingWidth:w height:h];
    if(result && ![self resolve:(LiuSurfaceDrawable *)result width:w height:h])return nil;
    return result;
}
static bool liu_surface_probe_on(void) {
    static i32 on=-1;
    if (on<0) { const char *flag=liu_dev_env("LIU_SURFACE_PROBE");on=(flag && flag[0]=='1')?1:0; }
    return on==1;
}
- (id<CAMetalDrawable>)nextRecordingWidth:(NSUInteger)w height:(NSUInteger)h {
    if(atomic_load(&stopped))return nil;
    if(liu_surface_probe_on() && probe_resume_t0==0 && probe_last_publish>0 &&
       CACurrentMediaTime()-probe_last_publish>1.5) {
        probe_resume_t0=CACurrentMediaTime();probe_starved0=starved;probe_alloc0=pool_allocations;
    }
    /* Allocation failure belongs to this acquisition, not an earlier frame.
     * A recovered OOM must not turn ordinary GPU pressure into a fallback. */
    allocation_failed=false;
    width=w;height=h;[self trimIdle:false];
    u32 selected=3,fallback=3;
    for(u32 i=0;i<3;i++) {
        LiuSurfaceImage *candidate=images[i];
        if(candidate && (candidate==shown || atomic_load_explicit(&candidate->leased,memory_order_acquire) || IOSurfaceIsInUse(candidate->surface)))continue;
        if(fallback==3)fallback=i;
        if(candidate && target_width==w && candidate->texture.width==w && candidate->texture.height==target_height){selected=i;break;}
    }
    if(selected==3)selected=fallback;
    if(selected==3){starved++;return nil;}
    LiuSurfaceImage *candidate=images[selected];
    if(!candidate) {
        NSUInteger seed=(target_width==w && target_height && target_height<=h)?target_height:16;
        candidate=[[LiuSurfaceImage alloc]initWithDevice:device width:w height:seed];
        if(!candidate){allocation_failed=true;return nil;}images[selected]=candidate;pool_allocations++;
    }
    atomic_store(&candidate->leased,true);
    LiuSurfaceDrawable *d=[[LiuSurfaceDrawable alloc]initWithOwner:self image:candidate];
    if(d){d->logical_width=w;d->logical_height=h;}else atomic_store(&candidate->leased,false);
    return [d autorelease];
}
- (void)publish:(LiuSurfaceDrawable *)drawable {
    /* A single main-queue publication per completed frame. Its retained lease
     * prevents pool reuse even while the main event loop is temporarily busy. */
    void (^publish)(void)=^{
        CGSize size=self->host.bounds.size;
        size=size.width>0 && size.height>0 ? CGSizeMake(size.width*self->host.contentsScale,size.height*self->host.contentsScale) : self->host.drawableSize;
        bool valid=!atomic_load(&self->stopped) && drawable->serial>self->published &&
            drawable->logical_width==(NSUInteger)llround(size.width) &&
            drawable->logical_height==(NSUInteger)llround(size.height) &&
            drawable.texture.width==drawable->logical_width && drawable.texture.height<=drawable->logical_height;
        if (valid) {
            self->published=drawable->serial;
            if(liu_surface_probe_on()) {
                f64 now=CACurrentMediaTime();
                if(self->probe_resume_t0>0) {
                    fprintf(stderr,"[surf] first frame after %.1f s quiet: on the layer %.1f ms after its first acquire attempt (attempts with no free slot %llu, surfaces created %llu, compaction pending %d)\n",
                            self->probe_resume_t0-self->probe_last_publish,(now-self->probe_resume_t0)*1000.0,
                            (unsigned long long)(self->starved-self->probe_starved0),
                            (unsigned long long)(self->pool_allocations-self->probe_alloc0),(int)self->compact_pending);
                    self->probe_resume_t0=0;
                }
                self->probe_last_publish=now;
            }
            [CATransaction begin];[CATransaction setDisableActions:YES];
            self->display.contentsScale=self->host.contentsScale;
            bool short_image=drawable.texture.height<drawable->logical_height;
            CGRect frame=self->host.bounds;
            if(short_image) {
                if(!self->backfill) {
                    self->backfill=[CALayer new];self->backfill.autoresizingMask=kCALayerWidthSizable|kCALayerHeightSizable;
                    [self->host insertSublayer:self->backfill below:self->display];
                }
                u32 bg=drawable->compact_background;
                CGColorSpaceRef space=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                CGFloat rgba[]={((bg>>16)&255)/255.0,((bg>>8)&255)/255.0,(bg&255)/255.0,1};
                CGColorRef color=CGColorCreate(space,rgba);
                self->backfill.frame=self->host.bounds;self->backfill.backgroundColor=color;
                CGColorRelease(color);CGColorSpaceRelease(space);
                frame.size.height=drawable.texture.height/self->host.contentsScale;
                if(!self->host.geometryFlipped)frame.origin.y+=self->host.bounds.size.height-frame.size.height;
            }
            self->backfill.hidden=!short_image;
            self->display.autoresizingMask=short_image ? (self->host.geometryFlipped?0:kCALayerMinYMargin) : kCALayerWidthSizable|kCALayerHeightSizable;
            self->display.frame=frame;
            self->display.contentsGravity=short_image?kCAGravityResize:kCAGravityTopLeft;
            self->display.opaque=self->host.opaque;
            self->display.contents=(id)drawable->image->surface;
            self->shown=drawable->image;
            self->compact_allowed=drawable->compact_allowed;
            self->compact_background=drawable->compact_background;
            [self updateCursor:drawable->cursor];
            [self updateTerminalRects:drawable->terminal_rects count:drawable->terminal_rect_count];
            [self publishFades:drawable];
            [CATransaction commit];self->accepted++;
            drawable->layer_time=CACurrentMediaTime();
            [self->snapshot release];self->snapshot=nil;
            [self trimIdle:false];
        } else self->discarded++;
        for (MTLDrawablePresentedHandler handler in drawable->handlers) handler(drawable);
    };
    if ([NSThread isMainThread]) publish();
    else dispatch_async(dispatch_get_main_queue(),publish);
}
- (void)stop {
    atomic_store(&stopped,true);display.contents=nil;shown=nil;[display removeFromSuperlayer];
    [cursor_layer removeAllAnimations];[cursor_layer removeFromSuperlayer];
    for(u32 i=0;i<RENDERER_TERMINAL_RECT_CAP;i++)[terminal_layers[i]removeFromSuperlayer];
    for(u32 i=0;i<RENDERER_FADE_CAP;i++) {
        [fade_layers[i] removeAllAnimations];[fade_layers[i] removeFromSuperlayer];
        fade_layers[i].contents=nil;[fade_shows[i] release];fade_shows[i]=nil;
    }
    [backfill removeFromSuperlayer];
}
- (void)dealloc {
    for(u32 i=0;i<3;i++)[images[i]release];
    for(u32 i=0;i<RENDERER_TERMINAL_RECT_CAP;i++)[terminal_layers[i]release];
    for(u32 i=0;i<RENDERER_FADE_CAP;i++) { [fade_layers[i] release];[fade_shows[i] release]; }
    for(u32 i=0;i<3;i++)[fade_images[i]release];
    [snapshot release];[backfill release];[cursor_layer release];[display release];[host release];[device release];[super dealloc];
}
@end
#endif
