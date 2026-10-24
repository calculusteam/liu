/* Immutable Metal command packets. Only values and retained GPU resources
 * cross the encoding queue; Renderer/AppState are never read by that worker. */
#ifndef LIU_METAL_RECORD_H
#define LIU_METAL_RECORD_H
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include "core/checked.h"
#include "core/mem_budget.h"

typedef enum {
    MR_RENDER, MR_BLIT, MR_END, MR_PIPELINE, MR_VIEWPORT, MR_SCISSOR,
    MR_VERTEX_BYTES, MR_VERTEX_BUFFER, MR_VERTEX_TEXTURE, MR_FRAGMENT_TEXTURE,
    MR_SAMPLER, MR_DRAW, MR_TEXTURE_TEXTURE, MR_TEXTURE_BUFFER, MR_BUFFER_TEXTURE,
    MR_PRESENT, MR_WAIT_EVENT, MR_SIGNAL_EVENT, MR_NOP
} MetalRecordKind;
typedef struct {
    MetalRecordKind kind;
    id a, b;
    union {
        MTLViewport viewport;
        MTLScissorRect scissor;
        struct { usize offset, length, index; } binding;
        struct { MTLPrimitiveType type; usize start, count, instances; } draw;
        struct {
            MTLOrigin src, dst;
            MTLSize size;
            usize src_slice, src_level, dst_slice, dst_level;
            usize offset, row_bytes, image_bytes;
        } copy;
        u64 event_value;
    } u;
} MetalRecord;

/* Size hints for the next packet. A packet is born per frame; growing its two
 * arrays from 64 records / 4 KiB by doubling would pay the same reallocs (and
 * copies) every frame just to get back to where the previous frame ended. The
 * hint is the recent high-water, decaying by a quarter per packet, so a
 * packet starts at roughly the size it will reach. Nothing is pooled or kept
 * alive between packets — the memory held at once is what one packet would
 * have grown to anyway. Shared by every window; a mismatch only costs the
 * growth it saves. */
static _Atomic usize g_metal_packet_cmd_hint;
static _Atomic usize g_metal_packet_byte_hint;
static _Atomic u64   g_metal_packet_grows;      /* Total packet-array growths since startup. */

static usize metal_packet_hint_next(usize hint, usize seen) {
    usize decayed = hint - hint / 4;
    return seen > decayed ? seen : decayed;
}
static usize metal_packet_pow2(usize want, usize floor_, usize cap) {
    usize v = floor_;
    while (v < want && v < cap) v *= 2;
    return v > cap ? cap : v;
}

@interface LiuMetalEncodingState : NSObject {
@public
    _Atomic bool failed;
}
@end
@implementation LiuMetalEncodingState
- (instancetype)init { if ((self=[super init])) atomic_init(&failed,false);return self; }
@end

/* Failure is observable through the same completion contract as driver errors.
 * This view exists only for the duration of a callback; it owns the real CB. */
@interface LiuMetalCommandResult : NSObject {
    id<MTLCommandBuffer> native;
}
- (instancetype)initWithBuffer:(id<MTLCommandBuffer>)buffer;
@end
@implementation LiuMetalCommandResult
- (instancetype)initWithBuffer:(id<MTLCommandBuffer>)buffer {
    if ((self=[super init])) native=[buffer retain];return self;
}
- (MTLCommandBufferStatus)status { return MTLCommandBufferStatusError; }
- (id)forwardingTargetForSelector:(SEL)sel { (void)sel;return native; }
- (void)dealloc { [native release];[super dealloc]; }
@end

@interface LiuMetalPacket : NSObject {
@public
    id<MTLCommandBuffer> native;
    dispatch_queue_t queue;
    LiuMetalEncodingState *state;
    MetalRecord *commands;
    usize count, capacity;
    u8 *bytes;
    usize used, bytes_capacity;
    bool force_encoder_failure;
    bool sealed;
}
- (instancetype)initWithBuffer:(id<MTLCommandBuffer>)buffer queue:(dispatch_queue_t)q;
- (void)append:(MetalRecord)command;
- (void)vertexBytes:(const void *)data length:(NSUInteger)n index:(NSUInteger)index;
- (void)encode;
@end
@implementation LiuMetalPacket
- (instancetype)initWithBuffer:(id<MTLCommandBuffer>)buffer queue:(dispatch_queue_t)q {
    if ((self=[super init])) {
        native=[buffer retain];queue=q;dispatch_retain(queue);state=[LiuMetalEncodingState new];
        if (!state) { [self release];return nil; }
    }
    return self;
}
- (void)append:(MetalRecord)command {
    if (sealed) { atomic_store(&state->failed,true);return; }
    if (atomic_load_explicit(&state->failed,memory_order_relaxed)) return;
    if (count==capacity) {
        usize next=capacity?capacity*2:
            metal_packet_pow2(atomic_load_explicit(&g_metal_packet_cmd_hint,memory_order_relaxed),64,65536);
        atomic_fetch_add_explicit(&g_metal_packet_grows,1,memory_order_relaxed);
        if (next>65536) { atomic_store(&state->failed,true);return; }
        MetalRecord *grown=realloc(commands,next*sizeof(*commands));
        if (!grown) { atomic_store(&state->failed,true);return; }
        mem_budget_transient_adjust(MEM_RENDER_CPU,(isize)((next-capacity)*sizeof(*commands)));
        commands=grown;capacity=next;
    }
    [command.a retain];[command.b retain];commands[count++]=command;
}
- (void)vertexBytes:(const void *)data length:(NSUInteger)n index:(NSUInteger)index {
    if (sealed) { atomic_store(&state->failed,true);return; }
    usize aligned, need;
    if (!checked_add_usize(used,15,&aligned)) { atomic_store(&state->failed,true);return; }
    aligned&=~(usize)15;
    if (!data || !checked_add_usize(aligned,n,&need) || need>MB(16)) {
        atomic_store(&state->failed,true);return;
    }
    if (need>bytes_capacity) {
        usize next=bytes_capacity?bytes_capacity:
            metal_packet_pow2(atomic_load_explicit(&g_metal_packet_byte_hint,memory_order_relaxed),4096,MB(16));
        while(next<need)next*=2;
        atomic_fetch_add_explicit(&g_metal_packet_grows,1,memory_order_relaxed);
        u8 *grown=realloc(bytes,next);
        if (!grown) { atomic_store(&state->failed,true);return; }
        mem_budget_transient_adjust(MEM_RENDER_CPU,(isize)(next-bytes_capacity));
        bytes=grown;bytes_capacity=next;
    }
    memcpy(bytes+aligned,data,n);used=need;
    [self append:(MetalRecord){.kind=MR_VERTEX_BYTES,.u.binding={aligned,n,index}}];
}
- (void)encode {
    id<MTLRenderCommandEncoder> render=nil;
    id<MTLBlitCommandEncoder> blit=nil;
    for (usize i=0;i<count && !atomic_load(&state->failed);i++) {
        const MetalRecord *c=&commands[i];
        switch(c->kind) {
        case MR_NOP: break;
        case MR_RENDER:
            render=force_encoder_failure?nil:[native renderCommandEncoderWithDescriptor:c->a];
            if (!render) atomic_store(&state->failed,true);
            break;
        case MR_BLIT:
            blit=force_encoder_failure?nil:[native blitCommandEncoder];
            if (!blit) atomic_store(&state->failed,true);
            break;
        case MR_END:
            if(render){[render endEncoding];render=nil;}if(blit){[blit endEncoding];blit=nil;}break;
        case MR_PIPELINE: [render setRenderPipelineState:c->a];break;
        case MR_VIEWPORT: [render setViewport:c->u.viewport];break;
        case MR_SCISSOR: [render setScissorRect:c->u.scissor];break;
        case MR_VERTEX_BYTES: [render setVertexBytes:bytes+c->u.binding.offset length:c->u.binding.length atIndex:c->u.binding.index];break;
        case MR_VERTEX_BUFFER: [render setVertexBuffer:c->a offset:c->u.binding.offset atIndex:c->u.binding.index];break;
        case MR_VERTEX_TEXTURE: [render setVertexTexture:c->a atIndex:c->u.binding.index];break;
        case MR_FRAGMENT_TEXTURE: [render setFragmentTexture:c->a atIndex:c->u.binding.index];break;
        case MR_SAMPLER: [render setFragmentSamplerState:c->a atIndex:c->u.binding.index];break;
        case MR_DRAW: [render drawPrimitives:c->u.draw.type vertexStart:c->u.draw.start vertexCount:c->u.draw.count instanceCount:c->u.draw.instances];break;
        case MR_TEXTURE_TEXTURE:
            [blit copyFromTexture:c->a sourceSlice:c->u.copy.src_slice sourceLevel:c->u.copy.src_level
                    sourceOrigin:c->u.copy.src sourceSize:c->u.copy.size toTexture:c->b
                destinationSlice:c->u.copy.dst_slice destinationLevel:c->u.copy.dst_level destinationOrigin:c->u.copy.dst];break;
        case MR_TEXTURE_BUFFER:
            [blit copyFromTexture:c->a sourceSlice:c->u.copy.src_slice sourceLevel:c->u.copy.src_level
                    sourceOrigin:c->u.copy.src sourceSize:c->u.copy.size toBuffer:c->b
               destinationOffset:c->u.copy.offset destinationBytesPerRow:c->u.copy.row_bytes
        destinationBytesPerImage:c->u.copy.image_bytes];break;
        case MR_BUFFER_TEXTURE:
            [blit copyFromBuffer:c->a sourceOffset:c->u.copy.offset sourceBytesPerRow:c->u.copy.row_bytes
            sourceBytesPerImage:c->u.copy.image_bytes sourceSize:c->u.copy.size toTexture:c->b
               destinationSlice:c->u.copy.dst_slice destinationLevel:c->u.copy.dst_level destinationOrigin:c->u.copy.dst];break;
        case MR_PRESENT: [native presentDrawable:c->a];break;
        case MR_WAIT_EVENT: [native encodeWaitForEvent:c->a value:c->u.event_value];break;
        case MR_SIGNAL_EVENT: [native encodeSignalEvent:c->a value:c->u.event_value];break;
        }
    }
    if(render)[render endEncoding];if(blit)[blit endEncoding];
    /* Even a failed packet must complete its handlers: atlas retries and the
     * frame semaphore own resources until this exact completion boundary. */
    [native commit];
}
- (void)dealloc {
    atomic_store_explicit(&g_metal_packet_cmd_hint,
        metal_packet_hint_next(atomic_load_explicit(&g_metal_packet_cmd_hint,memory_order_relaxed),count),
        memory_order_relaxed);
    atomic_store_explicit(&g_metal_packet_byte_hint,
        metal_packet_hint_next(atomic_load_explicit(&g_metal_packet_byte_hint,memory_order_relaxed),used),
        memory_order_relaxed);
    for(usize i=0;i<count;i++){[commands[i].a release];[commands[i].b release];}
    mem_budget_transient_adjust(MEM_RENDER_CPU,-(isize)(capacity*sizeof(*commands)+bytes_capacity));
    free(commands);free(bytes);[state release];[native release];dispatch_release(queue);[super dealloc];
}
@end

/* These are borrowed references: the packet's earlier binding records retain
 * every non-nil object until replay ends. Cache state belongs to one encoder,
 * never to the renderer or a subsequent render/blit pass. */
#define METAL_RECORD_BINDING_SLOTS 8
static bool metal_record_same_binding(LiuMetalPacket *packet,id *saved,u8 *valid,
                                      id value,NSUInteger index) {
    if(packet->sealed || index>=METAL_RECORD_BINDING_SLOTS)return false;
    u8 bit=(u8)(1u<<index);
    if((*valid&bit) && saved[index]==value)return true;
    *valid|=bit;saved[index]=value;return false;
}
@interface LiuMetalRecordedEncoder : NSObject {
    LiuMetalPacket *packet;
    id saved_pipeline;
    id saved_vertex[METAL_RECORD_BINDING_SLOTS];
    id saved_fragment[METAL_RECORD_BINDING_SLOTS];
    id saved_sampler[METAL_RECORD_BINDING_SLOTS];
    bool pipeline_valid;
    u8 vertex_valid,fragment_valid,sampler_valid;
}
- (instancetype)initWithPacket:(LiuMetalPacket *)p;
@end
@implementation LiuMetalRecordedEncoder
- (instancetype)initWithPacket:(LiuMetalPacket *)p { if((self=[super init]))packet=[p retain];return self; }
- (void)dealloc { [packet release];[super dealloc]; }
- (void)endEncoding {
    pipeline_valid=false;vertex_valid=fragment_valid=sampler_valid=0;
    [packet append:(MetalRecord){.kind=MR_END}];
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)p {
    if(!packet->sealed) {
        if(pipeline_valid && saved_pipeline==p)return;
        pipeline_valid=true;saved_pipeline=p;
    }
    [packet append:(MetalRecord){.kind=MR_PIPELINE,.a=p}];
}
- (void)setViewport:(MTLViewport)v { [packet append:(MetalRecord){.kind=MR_VIEWPORT,.u.viewport=v}]; }
- (void)setScissorRect:(MTLScissorRect)v { [packet append:(MetalRecord){.kind=MR_SCISSOR,.u.scissor=v}]; }
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [packet vertexBytes:p length:n index:i]; }
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {
    [packet append:(MetalRecord){.kind=MR_VERTEX_BUFFER,.a=b,.u.binding={o,0,i}}];
}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {
    if(metal_record_same_binding(packet,saved_vertex,&vertex_valid,t,i))return;
    [packet append:(MetalRecord){.kind=MR_VERTEX_TEXTURE,.a=t,.u.binding={0,0,i}}];
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {
    if(metal_record_same_binding(packet,saved_fragment,&fragment_valid,t,i))return;
    [packet append:(MetalRecord){.kind=MR_FRAGMENT_TEXTURE,.a=t,.u.binding={0,0,i}}];
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {
    if(metal_record_same_binding(packet,saved_sampler,&sampler_valid,s,i))return;
    [packet append:(MetalRecord){.kind=MR_SAMPLER,.a=s,.u.binding={0,0,i}}];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i {
    [packet append:(MetalRecord){.kind=MR_DRAW,.u.draw={t,s,n,i}}];
}
/* The proxy is an NSObject cast to id<MTLRenderCommandEncoder>, so protocol
 * coverage is by inspection: renderer_draw_background_image sends the
 * non-instanced form, which without this method would fall through to
 * doesNotRecognizeSelector: under LIU_METAL_RENDER_THREAD=1 as soon as a
 * background image is set. */
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n {
    [packet append:(MetalRecord){.kind=MR_DRAW,.u.draw={t,s,n,1}}];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)d {
    [packet append:(MetalRecord){.kind=MR_TEXTURE_TEXTURE,.a=src,.b=dst,.u.copy={.src=so,.dst=d,.size=size,.src_slice=ss,.src_level=sl,.dst_slice=ds,.dst_level=dl}}];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)o destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image {
    [packet append:(MetalRecord){.kind=MR_TEXTURE_BUFFER,.a=src,.b=dst,.u.copy={.src=so,.size=size,.src_slice=ss,.src_level=sl,.offset=o,.row_bytes=row,.image_bytes=image}}];
}
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)o sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)d {
    [packet append:(MetalRecord){.kind=MR_BUFFER_TEXTURE,.a=src,.b=dst,.u.copy={.dst=d,.size=size,.dst_slice=ds,.dst_level=dl,.offset=o,.row_bytes=row,.image_bytes=image}}];
}
@end

@interface LiuMetalRecordedBuffer : NSObject { @public LiuMetalPacket *packet; bool committed; }
- (instancetype)initWithBuffer:(id<MTLCommandBuffer>)b queue:(dispatch_queue_t)q;
@end
@implementation LiuMetalRecordedBuffer
- (instancetype)initWithBuffer:(id<MTLCommandBuffer>)b queue:(dispatch_queue_t)q {
    if((self=[super init])) {
        packet=[[LiuMetalPacket alloc]initWithBuffer:b queue:q];
        if (!packet) { [self release];return nil; }
    }
    return self;
}
- (void)dealloc { [packet release];[super dealloc]; }
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)desc {
    MTLRenderPassDescriptor *copy=[desc copy];
    [packet append:(MetalRecord){.kind=MR_RENDER,.a=copy}];[copy release];
    if(atomic_load(&packet->state->failed))return nil;
    return (id<MTLRenderCommandEncoder>)[[[LiuMetalRecordedEncoder alloc]initWithPacket:packet]autorelease];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
    [packet append:(MetalRecord){.kind=MR_BLIT}];
    if(atomic_load(&packet->state->failed))return nil;
    return (id<MTLBlitCommandEncoder>)[[[LiuMetalRecordedEncoder alloc]initWithPacket:packet]autorelease];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable { [packet append:(MetalRecord){.kind=MR_PRESENT,.a=drawable}]; }
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(u64)value { [packet append:(MetalRecord){.kind=MR_WAIT_EVENT,.a=event,.u.event_value=value}]; }
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(u64)value { [packet append:(MetalRecord){.kind=MR_SIGNAL_EVENT,.a=event,.u.event_value=value}]; }
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
    LiuMetalEncodingState *state=packet->state;
    [packet->native addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        if(atomic_load_explicit(&state->failed,memory_order_acquire)) {
            LiuMetalCommandResult *result=[[LiuMetalCommandResult alloc]initWithBuffer:buffer];
            handler((id<MTLCommandBuffer>)result);[result release];
        } else handler(buffer);
    }];
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)handler {
    LiuMetalEncodingState *state=packet->state;
    [packet->native addScheduledHandler:^(id<MTLCommandBuffer> buffer) {
        if(!atomic_load_explicit(&state->failed,memory_order_acquire))handler(buffer);
    }];
}
- (void)commit {
    if(committed)return;committed=true;
    packet->sealed=true;
    /* Reserve the GPU queue position now. A later readback/drain must not pass
     * this frame while the worker is still encoding it. */
    [packet->native enqueue];
    LiuMetalPacket *snapshot=packet;
    dispatch_async(packet->queue,^{ @autoreleasepool { [snapshot encode]; } });
}
- (void)waitUntilCompleted { dispatch_sync(packet->queue,^{});[packet->native waitUntilCompleted]; }
- (void)waitUntilScheduled { dispatch_sync(packet->queue,^{});[packet->native waitUntilScheduled]; }
- (MTLCommandBufferStatus)status {
    if(atomic_load(&packet->state->failed))return MTLCommandBufferStatusError;
    MTLCommandBufferStatus status=packet->native.status;
    return committed && status<MTLCommandBufferStatusCommitted?MTLCommandBufferStatusCommitted:status;
}
- (id)forwardingTargetForSelector:(SEL)sel {
    if(sel==@selector(GPUStartTime)||sel==@selector(GPUEndTime)||sel==@selector(error)||
       sel==@selector(label)||sel==@selector(commandQueue)||sel==@selector(device))return packet->native;
    return [super forwardingTargetForSelector:sel];
}
@end

static id<MTLCommandBuffer> metal_record_command_buffer(id<MTLCommandQueue> gpu, dispatch_queue_t encoder) {
    id<MTLCommandBuffer> native=[gpu commandBuffer];
    if(!native || !encoder)return native;
    return (id<MTLCommandBuffer>)[[[LiuMetalRecordedBuffer alloc]initWithBuffer:native queue:encoder]autorelease];
}
static bool metal_record_was_committed(id<MTLCommandBuffer> buffer) {
    if ([(id)buffer isKindOfClass:[LiuMetalRecordedBuffer class]])
        return ((LiuMetalRecordedBuffer *)buffer)->committed;
    return buffer.status>=MTLCommandBufferStatusCommitted;
}
#endif
