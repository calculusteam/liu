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
#import <QuartzCore/CATransaction.h>   /* explicit commit around present */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <stdarg.h>
#include "renderer/renderer.h"
#include "terminal/palette256.h"
#include "core/utf8.h"           /* utf8_is_emoji — colour glyphs ignore fg */
#include "platform/platform.h"   /* platform_time_sec — probe timing */
#include "core/memory.h"
#include "core/checked.h"
#include "core/imgproc.h"
#include "core/mem_budget.h"
#include "core/percentile.h"
#include "renderer/image_stats.h"
#include "renderer/renderer_metal_sync.h"
#include "renderer/retained_batch.h"
#include "renderer/metal_record.h"
#include "renderer/metal_surface.h"
#include "terminal/terminal.h"

/* Atlas staging/clear buffers move between pending, in-flight and pooled
 * ownership. Charge only allocation/final release, never those transfers. */
static void metal_atlas_buffer_release(id<MTLBuffer> buffer) {
    if (!buffer) return;
    mem_budget_transient_adjust(MEM_RENDER_GPU, -(isize)buffer.length);
    [buffer release];
}
/* A worker owns only this mailbox and the layer, never RendererMetal. Closing
 * a window can abandon a pending drawable request without a join on the UI. */
@interface LiuDrawableMailbox : NSObject {
@public
    NSLock *lock;
    id<CAMetalDrawable> ready;
    BOOL pending, stopped, waiting;
    u64 submitted, consumed, lost;
    struct { u64 id; f64 when, through; BOOL done; } observations[64];
    /* The stamps come from "on the layer", not from a hardware presentation
     * (the IOSurface presenter has none). Sticky; read under `lock`. */
    BOOL layer_stamps;
}
@end
@implementation LiuDrawableMailbox
- (instancetype)init { if ((self=[super init])) lock=[NSLock new]; return self; }
- (void)dealloc { [ready release]; [lock release]; [super dealloc]; }
@end

/* Keep at most one drawable ready for the next frame. Asking only from
 * begin_frame would guarantee a failed attempt and a 4 ms event-loop retry
 * for every frame, even when nextDrawable itself returns immediately. The
 * worker owns no renderer pointer and can safely finish after window
 * teardown. */
static void metal_request_drawable(LiuDrawableMailbox *box, CAMetalLayer *layer,
                                   void (*wake)(void)) {
    if (!box || !layer) return;
    [box->lock lock];
    if (!box->stopped && !box->ready && !box->pending) {
        box->pending = YES;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
            @autoreleasepool {
                id<CAMetalDrawable> drawable = [[layer nextDrawable] retain];
                [box->lock lock];
                BOOL notify = NO;
                if (!box->stopped) {
                    box->ready = drawable; drawable = nil;
                    notify = box->ready && box->waiting;
                    if (notify) box->waiting = NO;
                }
                box->pending = NO;
                [box->lock unlock];
                [drawable release];
                /* Prefetch alone must not wake an idle window. No renderer or
                 * window pointer escapes into this completion. */
                if (notify && wake) wake();
            }
        });
    }
    [box->lock unlock];
}

/* GPU completion bookkeeping lives in an object separate from RendererMetal because
 * a command-buffer completion handler can run after the submitting window is gone.
 * The counters therefore outlive the renderer and do not point back to it.
 *
 * submitted counts frame command buffers handed to the queue; ok and failed count
 * completions by outcome. When ok + failed reaches submitted, no submitted frame
 * remains outstanding. The ok and failed totals are monotonic.
 *
 * A failed frame did not reach the screen, so the main loop must know about it
 * whether or not benchmark telemetry is enabled. A submitted frame alone does not
 * prove success, because completion may arrive after a poll.
 *
 * failRun preserves completion order that aggregate totals do not contain. It
 * resets on success and increments on failure, so it gives the consecutive failure
 * count used by retry bounds.
 *
 * All four fields are protected by one lock to provide a consistent snapshot for
 * renderer_gpu_idle. OpenGL counts signalled fences as ok but cannot report
 * per-submission errors; zero failed and failRun values mean those outcomes are
 * unavailable, not that GL frames cannot fail. */
@interface LiuGpuEpoch : NSObject {
@public
    NSLock *lock;
    unsigned long long submitted, ok, failed;
    unsigned int failRun;
}
@end
@implementation LiuGpuEpoch
- (instancetype)init { if ((self = [super init])) lock = [NSLock new]; return self; }
- (void)dealloc { [lock release]; [super dealloc]; }
@end

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <libgen.h>
#include <sys/stat.h>   /* wallpaper share key: (path, mtime, size) */
#include <time.h>       /* nanosleep: the atlas handler wait in destroy */

/* Whole-loop frame budget probe (main.c, LIU_FRAME_PROBE=1). The two calls
 * below block on the GPU from inside app_render, so they are timed separately
 * and subtracted from the "render" phase. */
#include <stdatomic.h>

/* Optional fault injection for command-buffer error paths. */
#ifdef LIU_METAL_FAULT_INJECT
typedef enum {
    LIU_METAL_FAULT_COMMAND_BUFFER = 1,   /* [queue commandBuffer] answers nil   */
    LIU_METAL_FAULT_ENCODER        = 2,   /* renderCommandEncoder answers nil    */
    LIU_METAL_FAULT_COMPLETE_ERROR = 3,   /* this frame completes in Error       */
    /* The atlas queue's own branches: */
    LIU_METAL_FAULT_ATLAS_STAGING  = 4,   /* newBufferWithLength for staging answers nil */
    LIU_METAL_FAULT_ATLAS_BLIT     = 5,   /* blitCommandEncoder for the log answers nil  */
    LIU_METAL_FAULT_ATLAS_ERROR    = 6,   /* the batch's frame is treated as Error       */
    LIU_METAL_FAULT_ATLAS_OPS      = 7,   /* the op-log realloc answers NULL             */
} LiuMetalFaultKind;
bool (*g_liu_metal_fault_hook)(int kind) = NULL;
#define LIU_METAL_FAULT(kind) (g_liu_metal_fault_hook && g_liu_metal_fault_hook(kind))
#else
#define LIU_METAL_FAULT(kind) false
#endif

extern bool g_liu_fprobe_on;
extern f64  g_liu_fprobe_gpuwait;
/* Filled from the command buffer's completion handler (a Metal thread), read
 * and zeroed by the frame probe's per-second report in main.c. */
extern _Atomic unsigned long long g_liu_fprobe_gpu_ns;
extern _Atomic unsigned int       g_liu_fprobe_gpu_cnt;
/* True per-command-buffer GPU latency, written from the completion thread. */
extern LiuHistAtomic             g_liu_fprobe_gpu_hist;
#ifdef LIU_RENDER_REPLAY_PROBE
extern LiuHistAtomic             g_liu_replay_gpu_hist[2];
#endif
/* Written by the main loop, read from the command buffer completion handler on
 * a Metal thread — atomic for that reason, and relaxed because it is a
 * diagnostic gate, not a synchronisation point. */
extern _Atomic bool g_liu_gpu_timing_on;
extern f64  g_liu_fprobe_fence;

/* =========================================================================
 * Internal Metal state
 * ========================================================================= */

/* Must match `Uniforms` in LiuShaders.metal field for field AND in total size.
 * The shader struct contains a float4x4, so Metal rounds it to a multiple of
 * 16; the explicit tail padding here is what keeps the two at 96 bytes. A
 * mismatch is silent — the shader reads the wrong floats and the text goes
 * strange colours. */
typedef struct MetalUniforms_s {
    float projection[16];
    float cell_w, cell_h;
    float text_gamma;      /* coverage gamma for the glyph shader; 1.0 = passthrough */
    float min_contrast;    /* WCAG floor; <= 1.0 = off */
    float blend_correct;   /* 0 = off, 1 = luminance-corrected coverage */
    float text_weight;     /* manual coverage trim, -1..+1; 0 = untouched */
    float _pad[2];
} MetalUniforms;

#define METAL_FRAME_BUFFER_COUNT 3
#define METAL_IMAGE_CACHE_CAP 128
/* Ceiling on resident inline-image textures. 32 MiB is the working set of a
 * file browser scrolling through a directory of images plus a couple of
 * terminal-inline pictures; anything colder than that is cheaper to re-upload
 * than to keep pinned in GPU memory for the rest of the session. */
#define METAL_IMAGE_CACHE_BUDGET (32ull * 1024ull * 1024ull)
/* Above this a single texture goes to the dedicated `oversize` tier instead of
 * the pool. Deliberately NOT derived from the budget: tied to it, this would
 * halve whenever the budget did, pushing ordinary 2-4 MP viewer images onto
 * the oversize path. 16 MiB ≈ a 2048x2048 RGBA image — comfortably above
 * anything the pool should be asked to hold several of. */
#define METAL_IMAGE_OVERSIZE_BYTES (16ull * 1024ull * 1024ull)
/* How many above-threshold textures may be resident at once, and their own
 * byte ceiling.
 *
 * More than one: with a single slot, two above-threshold images drawn in the
 * same frame would trade it every frame and re-upload both. Two is not the
 * ceiling of the problem either — a terminal owns MAX_TERM_IMAGES = 32
 * inline-image slots and render_terminal_pane draws every one whose anchored
 * row is on screen, so N-way trading is reachable from one `cat` of a few
 * pictures.
 *
 * The budget is what bounds this, not the slot count: four slots at the 32
 * MiB inline-image maximum would be 128 MiB, so the loop evicts on BYTES and
 * the slots are only an array bound. Worst-case residency is that of a single
 * slot — admission evicts until the newcomer fits, so the total after an
 * install is max(BUDGET, one image), and one image may reach
 * METAL_IMAGE_MAX_BYTES on its own. */
#define METAL_IMAGE_OVERSIZE_SLOTS 4
#define METAL_IMAGE_OVERSIZE_BUDGET (96ull * 1024ull * 1024ull)
_Static_assert(METAL_IMAGE_OVERSIZE_BUDGET > METAL_IMAGE_OVERSIZE_BYTES * 2,
               "the oversize budget must hold at least two threshold images, "
               "or the pool it replaces the single slot with ping-pongs too");

/* Absolute ceiling on one image at the renderer boundary. The per-axis
 * METAL_MAX_TEXTURE_DIM check is not a size check: 16384 per axis is a gigabyte
 * of RGBA, which the oversize slot would happily hold because it sits outside
 * the pool budget and nothing else reclaims it. Sized to admit a full 8K image
 * (7680x4320 = 126.6 MiB) and refuse the absurd. Defence in depth: every caller
 * is supposed to have bounded this already, and the wallpaper path next door has
 * its own version of this constant. */
#define METAL_IMAGE_MAX_BYTES (256ull * 1024ull * 1024ull)
#define METAL_MAX_TEXTURE_DIM 16384
/* Reclaim a cached image texture after this many rendered frames without a
 * draw. Closed tabs/panes and scrolled-away images stop being drawn, so they
 * free promptly instead of lingering until 64 MiB LRU pressure evicts them.
 * Generous so an image briefly off-screen isn't re-uploaded on every scroll. */
#define METAL_IMAGE_CACHE_IDLE_SEC 15.0
/* The oversize slot holds by far the most expensive single texture (up to
 * ~64 MiB) and, sitting outside the byte budget, nothing else will ever reclaim
 * it. Give it a much shorter idle window than the pool so closing a viewer
 * returns the memory promptly instead of holding it for 600 frames. */
#define METAL_IMAGE_OVERSIZE_IDLE_SEC 3.0
/* Ceiling for the wallpaper texture. The device caps a 2D texture at
 * METAL_MAX_TEXTURE_DIM per axis, but a 16384x16384 RGBA image is 1 GiB — the
 * decode alone would stall the main thread and blow through RSS before Metal
 * ever saw the descriptor. 128 MiB covers a full 8K wallpaper (7680x4320x4 =
 * 126.6 MiB) with nothing sane left over. */
#define METAL_BG_IMAGE_MAX_BYTES (128ull * 1024ull * 1024ull)

/* The LRU admission loop computes `METAL_IMAGE_CACHE_BUDGET - bytes` in
 * unsigned arithmetic, which only means anything while every pooled texture is
 * smaller than the whole budget. The two constants above are deliberately
 * decoupled, so pin the one relationship that has to survive a retune: push the
 * oversize threshold past the budget and that subtraction wraps to ~SIZE_MAX,
 * the eviction loop never runs, and the pool grows without bound. */
_Static_assert(METAL_IMAGE_OVERSIZE_BYTES < METAL_IMAGE_CACHE_BUDGET,
               "oversize threshold must stay below the image pool budget");

typedef struct {
    const void     *key;
    u64             generation;
    i32             width;
    i32             height;
    usize           bytes;
    u64             last_used_frame;
    f64             last_used_sec;
    id<MTLTexture>  texture;
    /* Generations are not frames: one open frame may draw several versions.
     * Reuse only storage whose last sampling command has completed. */
    u64             texture_serial;
    id<MTLTexture>  spare[METAL_FRAME_BUFFER_COUNT - 1];
    u64             spare_serial[METAL_FRAME_BUFFER_COUNT - 1];
    u8              spare_count;
} MetalImageCacheEntry;

/* --- Wallpaper (background image) share table -----------------------------
 * The wallpaper texture is the single largest allocation a window can own — a
 * 4K picture is 31.6 MiB of GPU-visible memory — and without sharing every
 * window showing the SAME file would decode and upload its OWN copy. All
 * windows render on the same MTLDevice (platform_get_gpu_device returns the
 * one process-wide device), so one texture can back all of them.
 *
 * Entries are refcounted and live only while somebody holds a reference —
 * dropping to zero releases the texture immediately, so nothing lingers after
 * the last window drops it and nothing leaks at process exit.
 *
 * The key is (path, captured FileStamp, GPU device): new bytes must NOT
 * alias, or editing the picture in place would keep showing the stale one. */
typedef struct {
    char            path[512];   /* AppConfig.background_image is char[512] */
    FileStamp       source;
    id<MTLDevice>   device;      /* borrowed; identity comparison only */
} MetalBgImageKey;

typedef struct MetalBgImageEntry {
    MetalBgImageKey key;
    id<MTLTexture>  texture;     /* the entry owns exactly one +1 */
    i32             width;
    i32             height;
    usize           bytes;
    u32             refcount;    /* number of renderers pointing at it */
    bool            in_use;
} MetalBgImageEntry;

/* =========================================================================
 * Atlas upload queue — GPU-ordered texel writes
 * =========================================================================
 *
 * font.c never writes glyph texels straight into the texture the GPU is
 * sampling. Every write is a RECORD: the bytes are copied into a staging
 * buffer this renderer owns, and at the next frame's preparation — before its
 * render encoder opens — a blit encoder copies them into the texture. Command
 * buffers on one queue execute in order and Metal's hazard tracking orders a
 * blit into a texture behind earlier readers of it, so a frame already
 * submitted samples the OLD texels and the prepared frame the new ones, with
 * no CPU wait anywhere — even when a reclaim rewrites live cells.
 *
 * The staging buffer travels with the frame: the completion handler hands it
 * back to a small pool on success, or hands the WHOLE batch back to be encoded
 * again if the frame completed in Error — so the CPU's "the GPU has version N"
 * cannot outrun what the GPU actually holds by more than the frame that
 * failed. A staging buffer is never reused before the frame that read it has
 * completed, by construction.
 *
 * A texture replacement (the alpha atlas doubling, the colour atlas resetting
 * to a bigger empty one) is published the same way: the frame in flight keeps
 * the texture it was encoded against; the next preparation clears the new
 * one, copies the kept block GPU-side, binds it, and retires the old one when
 * the last frame that sampled it completes. Uploads recorded after the
 * announcement target the new texture, in staging order, so a glyph packed
 * right after a grow lands in the texture the grow produced.
 *
 * Classic Metal only: MTLBlitCommandEncoder, tracked resources, one command
 * queue. Nothing here assumes Metal 4 residency sets or explicit barriers. */
/* One entry of the ordered log the queue keeps. The ORDER is the contract:
 * an upload recorded before a replacement targets the texture being
 * replaced and must land there before the copy carries it over (the alpha
 * atlas has no CPU mirror on Metal, so the GPU copy is the only carrier of
 * those pixels); an upload recorded after it targets the replacement. Two
 * replacements of the same kind between two preparations — the colour atlas
 * dropping its texture and creating the bigger one, or the alpha atlas
 * doubling twice under one long frame — chain the same way. */
typedef enum {
    METAL_ATLAS_OP_UPLOAD  = 0,
    METAL_ATLAS_OP_REPLACE = 1,
    METAL_ATLAS_OP_CLEAR   = 2,   /* zero a region from the renderer's zero buffer: no staging bytes */
} MetalAtlasOpKind;

typedef struct {
    u8  op;                    /* MetalAtlasOpKind */
    u8  kind;                  /* REPLACE: 0 alpha, 1 colour */
    /* UPLOAD */
    id<MTLTexture> tex;        /* retained by the record */
    u32 x, y, w, h;
    u32 bpp;
    usize offset;              /* into the batch's staging buffer */
    usize pitch;               /* bytes per staged row */
    u32 version;               /* atlas content version of this write */
    /* REPLACE */
    id<MTLTexture> old;        /* retained; NULL when the kind had no texture */
    id<MTLTexture> new;        /* retained; NULL = the kind is being unbound */
    u32 copy_w, copy_h;        /* top-left block of `old` carried into `new` */
} MetalAtlasOp;

typedef struct MetalAtlasBatch {
    id<MTLBuffer>  staging;            /* retained; returned to the pool on success */
    MetalAtlasOp  *ops;
    u32            count;
    u32            ready_version;      /* what the frame publishes as prepared */
} MetalAtlasBatch;

typedef struct {
    id<MTLDevice>               device;
    id<MTLCommandQueue>         commandQueue;
    CAMetalLayer               *layer;
    id<MTLLibrary>              shaderLibrary;  /* retained for lazy pipeline compile */
    id<MTLRenderPipelineState>  glyphPipeline;
    id<MTLRenderPipelineState>  rectPipeline;
    id<MTLRenderPipelineState>  rrectPipeline;  /* SDF rounded rect + shadow */
    /* Same shader, alpha factors (Zero, One): colour is written, the
     * framebuffer's alpha is left exactly as the clear wrote it. Backs
     * renderer_draw_rrect_translucent — see renderer.h. */
    id<MTLRenderPipelineState>  rrectPipelineKeepAlpha;
    id<MTLRenderPipelineState>  strokePipeline;
    bool strokePipelineTried;
    id<MTLRenderPipelineState>  linePipeline;   /* thick AA segments (graph edges) */
    id<MTLRenderPipelineState>  blurPipeline;   /* backdrop-blur sampling */
    id<MTLTexture>              blurSnapshot;   /* drawable copy for blur sampling */
    id<MTLTexture>              blurIntermediate; /* horizontal-blur target for 2-pass */
    /* What the pair above is currently reported as on the ledger. Kept
     * rather than recomputed from width*height so every release subtracts
     * exactly what its create added — a resize that recomputes from the NEW
     * size would drift, and there is nothing left to measure once the
     * textures are gone. */
    usize                       blurAccounted;
    bool                        blurValid;      /* set by capture, cleared each frame */
    f64                         blurLastUseSec; /* for idle release of full-screen textures */
    id<MTLSamplerState>         sampler;        /* bilinear — UI chrome, sized glyphs, images */
    id<MTLSamplerState>         samplerNearest; /* terminal glyphs — see the pick in renderer_flush_glyphs */
    id<CAMetalDrawable>         currentDrawable;
    LiuDrawableMailbox         *acquire;
    LiuSurfacePresenter        *surfacePresenter;
    bool                        frameTargetPlanned;
    f64                         lastFrameTime;
    LiuGpuEpoch                *epoch;      /* GPU completion counters — see above */
    id<MTLCommandBuffer>        currentCommandBuffer;
    id<MTLRenderCommandEncoder> currentEncoder;
    /* Background image pipeline */
    id<MTLRenderPipelineState>  bgImagePipeline;
    /* BORROWED when bgImageEntry != NULL (the share-table entry owns the +1);
     * owned outright only on the fallback path where the table was full. */
    id<MTLTexture>              bgImageTexture;
    MetalBgImageEntry          *bgImageEntry;   /* NULL = not shared */
    usize                       bgPrivateAccounted;
    id<MTLSamplerState>         bgImageSampler;
    id<MTLBuffer>               frameBuffers[METAL_FRAME_BUFFER_COUNT];
    usize                       frameBufferSizes[METAL_FRAME_BUFFER_COUNT];
    usize                       frameBufferOffset;
    u32                         frameBufferIndex;
    u64                         frameSerial;
    f64                         frameNowSec;
    MetalImageCacheEntry        imageCache[METAL_IMAGE_CACHE_CAP];
    usize                       imageCacheBytes;
    usize                       imageAccountedBytes;
    /* Textures too large to admit to the LRU pool are held OUTSIDE
     * imageCacheBytes so one giant image cannot squeeze the ordinary entries
     * out, and are kept across frames so an unchanged image is not allocated,
     * uploaded and released again on every frame it is drawn. */
    /* Above-threshold textures. A small LRU with its own byte budget, kept
     * apart from imageCache so one 32 MiB picture cannot evict the whole
     * 32 MiB pool of icons and thumbnails. */
    MetalImageCacheEntry        oversize[METAL_IMAGE_OVERSIZE_SLOTS];
    usize                       oversizeBytes;
    /* Cached uniforms for flush hot path; rebuilt when Renderer.uniforms_version
     * changes or when cw/ch differs from the last bake. */
    MetalUniforms               cachedUniforms;
    u32                         cachedUniformsVersion;
    /* CPU/GPU frame-pacing fence. Initialized to METAL_FRAME_BUFFER_COUNT so the
     * CPU may run up to that many frames ahead of the GPU before begin_frame
     * blocks. Every successful wait at the top of renderer_begin_frame is
     * balanced by exactly one signal — either from the command buffer's
     * completed handler (committed path) or inline on a path that never commits
     * (early returns, blocked end_frame). frameSlotHeld tracks whether the
     * current CPU frame still owes a signal, so a slot is never double-signalled
     * or leaked. */
    dispatch_semaphore_t frameSem;
    dispatch_queue_t     encodeQueue;
    bool                 frameSlotHeld;

    /* ---- Atlas synchronisation (see "Atlas upload queue" below) ----------
     * Textures the CURRENT and NEXT frames sample, per atlas kind (0 alpha,
     * 1 colour): what the frame in flight was encoded against, retained here
     * for as long as that lasts. A replacement font.c announces sits in the
     * pending log until the next preparation binds it. */
    id<MTLTexture>       atlasBound[2];
    /* The log recorded since the last preparation: upload bytes sit in
     * pendingStaging, the ops (uploads and replacements, in order) in
     * pendingOps. */
    MetalAtlasOp        *pendingOps;
    u32                  pendingCount, pendingCap;
    id<MTLBuffer>        pendingStaging;
    usize                pendingUsed;
    /* Everything touched from a completion handler is under atlasLock: the
     * staging buffers handed back by finished frames, and the batches a
     * FAILED frame handed back to be encoded again. */
    NSLock              *atlasLock;
    id<MTLBuffer>        stagingPool[4];
    u32                  stagingPoolCount;
    f64                  stagingLastUseSec; /* UI thread, like the idle trim */
    f64                  surfaceStatsLastSec;
    /* A block of zeroes the CLEAR op blits from in strips — the shelf reclaim
     * zeroes up to a whole 4096² atlas, and this avoids staging 16 MiB of
     * zeroes per reclaim. Sized on first use, never freed early: every batch
     * that references it retains it. */
    id<MTLBuffer>        zeroBuffer;
    /* Batches handed back by frames that completed in Error, oldest first.
     * A batch is only ever lost past this cap — sixteen consecutive failed
     * frames, far beyond the retry policy's give-up — and counted as
     * `dropped` when it is. */
    #define METAL_ATLAS_REQUEUE_CAP 16
    MetalAtlasBatch     *requeue[METAL_ATLAS_REQUEUE_CAP];
    u32                  requeueCount;
    /* Batch completion handlers attached and not yet returned. The teardown
     * drain completes every frame, but "completed" is the GPU's word, not
     * the handler thread's — renderer_destroy waits for this to reach zero
     * before it frees `m`, which every handler dereferences. */
    _Atomic u32          atlasHandlersLive;
    RendererAtlasStats   atlasStats;
} RendererMetal;

static bool metal_atlas_prepare(RendererMetal *m, Renderer *r, id<MTLCommandBuffer> cb);
static void metal_atlas_destroy(RendererMetal *m);
static void metal_atlas_drop_pending(RendererMetal *m);
static void metal_commit_tracked(RendererMetal *m, id<MTLCommandBuffer> cb);
static void metal_retained_destroy(RetainedBatch *batch);
static void metal_retained_trim(Renderer *r, f64 now);

usize renderer_cpu_bytes(const Renderer *r) {
    usize bytes = renderer_batch_cpu_bytes(r);
    if (r && r->backend) bytes += sizeof(RendererMetal);
    return bytes;
}

static void metal_blur_textures_release(RendererMetal *m);

static void metal_image_account(RendererMetal *m) {
    if (!m) return;
    usize total = m->imageCacheBytes;
    if (total <= (usize)-1 - m->oversizeBytes) total += m->oversizeBytes;
    if (total > m->imageAccountedBytes)
        mem_budget_adjust(MEM_IMAGE_GPU,
                          (isize)(total - m->imageAccountedBytes));
    else if (m->imageAccountedBytes > total)
        mem_budget_adjust(MEM_IMAGE_GPU,
                          -(isize)(m->imageAccountedBytes - total));
    m->imageAccountedBytes = total;
}

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
/* Glyphs use one strip: the same two triangles with four vertex invocations. */
static const float s_glyph_quad[] = { 1,0, 1,1, 0,0, 0,1 };
#ifdef LIU_GLYPH_TOPOLOGY_TEST
static bool s_glyph_legacy = false;
#else
#define s_glyph_legacy false
#endif

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
f32  g_srgb_decode_lut[256];
bool g_srgb_decode_lut_ready = false;

void renderer_init_colors(void) {
    /* sRGB→linear LUT — see renderer.h for the exactness argument. */
    for (i32 i = 0; i < 256; i++)
        g_srgb_decode_lut[i] = srgb_decode((f32)i * (1.0f / 255.0f));
    g_srgb_decode_lut_ready = true;
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
    /* The 216-colour cube and the 24 greys: terminal/palette256.h, which an
     * OSC 4 query is answered from too. */
    for (int i = 16; i < 256; i++) {
        u32 rgb = palette256_rgb(i);
        g_ansi_colors[i] = COLOR_RGB((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu);
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
        m->cachedUniforms.cell_w == cw && m->cachedUniforms.cell_h == ch &&
        m->cachedUniforms.text_gamma == font_text_gamma() &&
        m->cachedUniforms.min_contrast == r->text_min_contrast &&
        m->cachedUniforms.blend_correct == r->text_blend_correct &&
        m->cachedUniforms.text_weight == renderer_text_weight(r)) {
        MetalUniforms u;
        memcpy(&u, &m->cachedUniforms, sizeof(u));
        retained_projection(r, u.projection);
        return u;
    }
    MetalUniforms u;
    memcpy(u.projection, r->projection, sizeof(u.projection));
    u.cell_w = cw;
    u.cell_h = ch;
    u.text_gamma = font_text_gamma();
    u.min_contrast = r->text_min_contrast;
    u.blend_correct = r->text_blend_correct;
    u.text_weight = renderer_text_weight(r);
    u._pad[0] = u._pad[1] = 0.0f;
    if (m) {
        memcpy(&m->cachedUniforms, &u, sizeof(u));
        m->cachedUniformsVersion = r->uniforms_version;
    }
    retained_projection(r, u.projection);
    return u;
}

/* Upload vertex data: setVertexBytes for small data, per-frame ring buffers
 * for large batches. The ring avoids per-flush MTLBuffer allocation while
 * keeping previous in-flight frames untouched. */
/* LIU_GLYPH_FILTER=linear restores the pre-split behaviour (bilinear for
 * terminal glyphs too) — the A/B arm and the revert, same convention as
 * LIU_PACE / LIU_DISPLAY_LINK / LIU_POINTER_GATE. */
static bool metal_glyph_nearest(void) {
    static i32 v = -1;
    if (v < 0) {
        const char *e = getenv("LIU_GLYPH_FILTER");
        v = (e && strcmp(e, "linear") == 0) ? 0 : 1;
    }
    return v != 0;
}

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
        /* Start small and let the doubling below find the real high-water mark.
         * A full 4K grid of glyphs is ~1.3 MB of instance data and a typical
         * frame stays under 512 KB, so seeding each of the three ring slots at
         * 4 MB reserved 12 MB of MTLResourceStorageModeShared memory that
         * nothing ever wrote to. The growth path is amortised — it fires at
         * most a handful of times per session, on the frames that genuinely
         * need the room. */
        usize new_size = m->frameBufferSizes[idx] ? m->frameBufferSizes[idx] : (usize)(512 * 1024);
        while (new_size < need) new_size *= 2;
        id<MTLBuffer> new_buf = [m->device newBufferWithLength:(NSUInteger)new_size
                                                       options:MTLResourceStorageModeShared];
        if (!new_buf) return false;
        if (m->frameBuffers[idx]) [m->frameBuffers[idx] release];
        /* Charge capacity changes once. A long idle interval or memory pressure
         * can retire the completed ring, while ordinary pauses keep it warm. The
         * next burst follows this same bounded growth path. */
        mem_budget_adjust(MEM_RENDER_GPU,
                          (isize)new_size - (isize)m->frameBufferSizes[idx]);
        m->frameBuffers[idx] = new_buf;
        m->frameBufferSizes[idx] = new_size;
    }
    *out_buf = m->frameBuffers[idx];
    *out_offset = (NSUInteger)offset;
    *out_ptr = (u8 *)[m->frameBuffers[idx] contents] + offset;
    m->frameBufferOffset = need;
    return true;
}

/* Returns FALSE when neither the frame ring nor the fallback allocation could
 * supply a buffer, so the caller can skip the draw: otherwise it would use
 * whatever vertex buffer the encoder still holds from the PREVIOUS batch, at
 * the current batch's instance count. Garbage geometry from an allocation
 * failure is not a better outcome than a missing batch. */
static bool metal_set_vertex_data(RendererMetal *m, id<MTLRenderCommandEncoder> enc,
                                  const void *data, usize bytes, NSUInteger idx) {
    /* FAULT INJECTION. This allocation-failure path cannot otherwise be reached: it needs the
     * device to be out of memory. LIU_METAL_ALLOC_FAIL=1 makes every set fail — the inline path
     * too, since the batches a headless run produces are small and would otherwise never reach
     * the buffered one — so the guards at the call sites can be seen to hold: the app keeps
     * drawing, with the affected batches missing, instead of drawing this batch's instance count
     * against the previous batch's vertex buffer. Resolved once: this is the read-only, main-thread
     * cached-getenv idiom used throughout the tree. */
    static i32 s_force_fail = -1;
    if (s_force_fail < 0) {
        const char *v = liu_dev_env("LIU_METAL_ALLOC_FAIL");
        s_force_fail = (v && *v && *v != '0') ? 1 : 0;
    }
    if (s_force_fail) return false;   /* both paths, so the guards are reachable */
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
            if (!tmp) return false;          /* out of memory, both ways */
            [enc setVertexBuffer:tmp offset:0 atIndex:idx];
            [tmp release];
        }
    }
    return true;
}

/* =========================================================================
 * Init / Destroy
 * ========================================================================= */

bool renderer_init(Renderer *r, f32 dpi_scale) {
    memset(r, 0, sizeof(*r));
    /* Text knobs, resolved once. See the GlyphInstance and Renderer comments in
     * renderer.h: both need the per-glyph background.
     *
     * Must run AFTER the memset, or every value computed here would be zeroed
     * on the next line and LIU_TEXT_CONTRAST and LIU_TEXT_BLEND would have no
     * effect. */
    {
        const char *e = getenv("LIU_TEXT_CONTRAST");
        f32 v = e ? (f32)atof(e) : 0.0f;
        r->text_min_contrast = (v > 1.0f) ? v : 0.0f;
        /* Blend correction is ON unless asked for plain linear blending —
         * see renderer.h's text_blend_correct. */
        const char *b = getenv("LIU_TEXT_BLEND");
        r->text_blend_correct = (b && strcmp(b, "linear") == 0) ? 0.0f : 1.0f;
    }
    r->dpi_scale = dpi_scale;
    r->backend = NULL;
    /* Opaque until the frame loop says otherwise: a zeroed value would
     * read as fully transparent and boost glyph alpha on an opaque
     * window, which is exactly the look the boost exists to avoid. */
    r->win_opacity = 1.0f;
    renderer_init_colors();

    /* Default clear color (black) */
    r->clear_color[0] = 0.0f;
    r->clear_color[1] = 0.0f;
    r->clear_color[2] = 0.0f;
    r->clear_color[3] = 1.0f;

    /* CPU-side instance buffers grow on demand, so an idle renderer holds
     * no max-size buffers before a single glyph is drawn. */
    if (!renderer_reserve_glyphs(r, INIT_GLYPHS)) return false;
    if (!renderer_reserve_rects(r, INIT_RECTS)) return false;
    if (!renderer_reserve_rrects(r, INIT_RRECTS)) return false;

    return r->glyph_instances && r->rect_batch && r->rrect_batch;
}

/* Why the last renderer_set_gpu() left a renderer without a backend, in words a
 * person can be shown, so that a failure (a shader library this macOS cannot
 * read, for one) is a message rather than a black window. Main thread only, like
 * the call. */
static char g_gpu_error[320];

static void gpu_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void gpu_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_gpu_error, sizeof g_gpu_error, fmt, ap);
    va_end(ap);
    fprintf(stderr, "renderer_metal: %s\n", g_gpu_error);
}

bool renderer_gpu_ready(const Renderer *r) { return r && r->backend != NULL; }
const char *renderer_gpu_error(void) { return g_gpu_error; }

void renderer_set_gpu(Renderer *r, void *device, void *layer, void *queue) {
    @autoreleasepool {
        /* One backend per renderer. A second calloc'd RendererMetal written
         * over r->backend would orphan every pipeline, ring buffer, cached
         * texture and the frame semaphore of the first — an unreachable leak
         * of the entire GPU state. */
        if (r->backend) {
            fprintf(stderr, "renderer_metal: backend already initialized\n");
            return;
        }
        g_gpu_error[0] = '\0';
        /* No device is its own answer (a virtual machine without a GPU): asked
         * of nil, the library load below "fails" with a null description. */
        if (!device) {
            gpu_fail("this Mac offers no Metal graphics device");
            return;
        }
        RendererMetal *m = calloc(1, sizeof(RendererMetal));
        if (!m) {
            gpu_fail("out of memory setting up the renderer");
            return;
        }

        m->device       = (__bridge id<MTLDevice>)device;
        m->commandQueue = (__bridge id<MTLCommandQueue>)queue;
        m->layer        = (__bridge CAMetalLayer *)layer;

        /* --- Load .metallib from executable directory --- */
        char exe_path[4096];
        u32 exe_size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &exe_size) != 0) {
            gpu_fail("cannot find the app's own folder to load shaders from");
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
            gpu_fail("could not load the shader library LiuShaders.metallib (%s)",
                     error.localizedDescription.UTF8String
                         ? error.localizedDescription.UTF8String : "no reason given");
            free(m);
            return;
        }
        /* Retain the library so the bg-image pipeline can be compiled later
         * (deferred until renderer_load_background_image is actually called).
         * Every failure path from here on must drop BOTH references — the one
         * `newLibraryWithURL:` handed over and this extra retain — before
         * free(m) puts the m->shaderLibrary field out of reach. */
        m->shaderLibrary = [library retain];

        /* --- Glyph pipeline --- */
        id<MTLFunction> glyphVert = [library newFunctionWithName:@"glyph_vertex"];
        id<MTLFunction> glyphFrag = [library newFunctionWithName:@"glyph_fragment"];
        if (!glyphVert || !glyphFrag) {
            gpu_fail("the shader library has no glyph shaders");
            [library release];
            [m->shaderLibrary release];
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
            gpu_fail("the glyph pipeline could not be built (%s)",
                     error.localizedDescription.UTF8String
                         ? error.localizedDescription.UTF8String : "no reason given");
            [library release];
            [m->shaderLibrary release];
            free(m);
            return;
        }

        /* --- Rect pipeline --- */
        id<MTLFunction> rectVert = [library newFunctionWithName:@"rect_vertex"];
        id<MTLFunction> rectFrag = [library newFunctionWithName:@"rect_fragment"];
        if (!rectVert || !rectFrag) {
            gpu_fail("the shader library has no rect shaders");
            [library release];
            [m->shaderLibrary release];
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
        /* Alpha blends as a real "over" (One / 1-SrcAlpha), matching the RGB
         * channel, so alpha rises with the colour. A Max(One, One) alpha
         * blend would keep it from rising: a translucent rect over a
         * translucent background would raise RGB to the stacked value while
         * alpha stayed put, and the result would composite brighter and more
         * opaque-looking than its own alpha allows. */
        rectDesc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
        rectDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        m->rectPipeline = [m->device newRenderPipelineStateWithDescriptor:rectDesc error:&error];
        [rectDesc release];
        [rectVert release];
        [rectFrag release];
        if (!m->rectPipeline) {
            [library release];
            [m->shaderLibrary release];
            gpu_fail("the rect pipeline could not be built (%s)",
                     error.localizedDescription.UTF8String
                         ? error.localizedDescription.UTF8String : "no reason given");
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
            lineDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            m->linePipeline = [m->device newRenderPipelineStateWithDescriptor:lineDesc error:&error];
            [lineDesc release];
        }
        if (lineVert) [lineVert release];
        if (lineFrag) [lineFrag release];

        /* Backdrop blur has no pipeline built here — it is compiled on first
         * use by ensure_blur_pipeline() below. The layer runs framebufferOnly,
         * so renderer_blur_capture always bails and nothing asks for it;
         * building it at startup would cost every launch for nothing. */

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
            rrectDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

            m->rrectPipeline = [m->device newRenderPipelineStateWithDescriptor:rrectDesc error:&error];
            /* Alpha-preserving twin: identical except that the alpha channel
             * is (Zero, One), i.e. dst survives. Built here rather than lazily
             * because it is on the tab bar's per-frame path — see
             * renderer_draw_rrect_translucent. A failure is non-fatal: the
             * flush falls back to the ordinary pipeline. */
            rrectDesc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorZero;
            rrectDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
            NSError *ka_error = nil;
            m->rrectPipelineKeepAlpha =
                [m->device newRenderPipelineStateWithDescriptor:rrectDesc error:&ka_error];
            if (!m->rrectPipelineKeepAlpha) {
                fprintf(stderr, "renderer_metal: rrect keep-alpha pipeline failed (%s) — "
                        "translucent chrome falls back to opaque\n",
                        ka_error.localizedDescription.UTF8String);
            }
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

        /* Terminal glyphs sample the atlas 1:1 and use nearest filtering.
         * Bilinear filtering can blend neighboring texels when a quad is
         * aligned between pixels. UI chrome and sized rasters use linear
         * filtering when they are scaled. */
        MTLSamplerDescriptor *nearDesc = [[MTLSamplerDescriptor alloc] init];
        nearDesc.minFilter    = MTLSamplerMinMagFilterNearest;
        nearDesc.magFilter    = MTLSamplerMinMagFilterNearest;
        nearDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        nearDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        m->samplerNearest = [m->device newSamplerStateWithDescriptor:nearDesc];
        [nearDesc release];

        /* --- Background image pipeline ---
         * DEFERRED: only needed when the user sets config.background_image.
         * Lazy-compiled by renderer_load_background_image() below — saves
         * ~1 MB of pipeline state for the default (no-bg-image) config. */
        m->bgImagePipeline = nil;

        /* bgImageSampler is allocated inside ensure_bg_image_pipeline() on
         * first use — avoids ~2 KB of sampler state + descriptor at idle. */

        [library release];

        /* CPU/GPU frame fence — allows up to METAL_FRAME_BUFFER_COUNT frames of
         * CPU lead before begin_frame defers until the GPU finishes an older
         * in-flight frame. This throttles writers of the per-frame ring buffers
         * (frameBuffers[]) and the in-place glyph atlas to <= N frames ahead,
         * eliminating torn glyphs / flicker under sustained animation. */
        m->frameSem = dispatch_semaphore_create(METAL_FRAME_BUFFER_COUNT);
        m->frameSlotHeld = false;
        m->acquire = [LiuDrawableMailbox new];
        const char *presentation=getenv("LIU_METAL_PRESENTATION");
        bool surface=presentation ? strcmp(presentation,"iosurface")==0 :
            [m->device supportsFamily:MTLGPUFamilyApple1];
        if (surface && [m->layer isKindOfClass:[CAMetalLayer class]])
            m->surfacePresenter=[[LiuSurfacePresenter alloc]initWithLayer:m->layer device:m->device];
        const char *render_thread=getenv("LIU_METAL_RENDER_THREAD");
        /* The IOSurface path removes the drawable-present wait from this
         * queue. Keep native presentation's older default available as the
         * explicit compatibility path and for non-Apple GPUs. */
        if (render_thread ? render_thread[0]=='1' : m->surfacePresenter!=nil) {
            dispatch_queue_attr_t attr=dispatch_queue_attr_make_with_qos_class(
                DISPATCH_QUEUE_SERIAL,QOS_CLASS_USER_INITIATED,0);
            m->encodeQueue=dispatch_queue_create("liu.metal.encode",attr);
        }
        m->epoch   = [LiuGpuEpoch new];
        m->atlasLock = [NSLock new];

        r->backend = m;
        fprintf(stderr, "renderer_metal: GPU backend initialized (%s, %s encoding)\n",
            m->surfacePresenter ? "IOSurface" : "CAMetalLayer",m->encodeQueue ? "worker" : "native");
    }
}

void renderer_set_clear_color(Renderer *r, f32 red, f32 green, f32 blue, f32 alpha) {
    /* Framebuffer attachment is _sRGB — Metal interprets the clear color as
     * linear and encodes to sRGB on write. Feed it linear.
     *
     * The clear colour is PREMULTIPLIED, which translucent windows depend on.
     * The window server reads this buffer as premultiplied alpha, and the glyph
     * pipeline blends into it with (ONE, 1-SrcAlpha) — also premultiplied. A
     * straight (un-multiplied) clear puts a colour in there that is too bright
     * for its own alpha, and every antialiased glyph edge inherits the error: a
     * half-covered edge pixel gets far less of the foreground in RGB than its
     * alpha says, composites DARKER than the background next to it, and rings
     * each glyph with a dark outline.
     *
     * With the premultiply in place an edge pixel lands exactly where it should
     * — the same mix of "glyph" and "whatever the background composites to" as
     * its coverage says. Glyph BODIES are untouched: coverage 1 writes alpha 1,
     * so text stays fully opaque no matter how low the window opacity goes. */
    r->clear_color[0] = srgb_decode(red)   * alpha;
    r->clear_color[1] = srgb_decode(green) * alpha;
    r->clear_color[2] = srgb_decode(blue)  * alpha;
    r->clear_color[3] = alpha;
    /* UI text is drawn against the STRAIGHT colour, not the premultiplied one:
     * opacity changes what is behind the window, not what the panels are made of. */
    renderer_note_text_surface(r, red, green, blue);
}

/* Drops this renderer's reference on the shared wallpaper texture (defined
 * with the rest of the background-image code at the bottom of the file). */
static void metal_bg_image_unref(RendererMetal *m);

void renderer_destroy(Renderer *r) {
    retained_collect(r, true, metal_retained_destroy);
    if (r->backend) {
        RendererMetal *m = r->backend;
        [m->surfacePresenter stop];
        if (m->currentEncoder) {
            [m->currentEncoder endEncoding];
            [m->currentEncoder release];m->currentEncoder=nil;
        }
        if (m->currentCommandBuffer && !metal_record_was_committed(m->currentCommandBuffer))
            metal_commit_tracked(m,m->currentCommandBuffer);
        if (m->encodeQueue) dispatch_sync(m->encodeQueue,^{});
        if (m->acquire) {
            [m->acquire->lock lock]; m->acquire->stopped=YES;
            [m->acquire->ready release]; m->acquire->ready=nil;
            [m->acquire->lock unlock]; [m->acquire release];
        }
        /* Drain committed GPU work before freeing `m`: the scheduled handler
         * attached in renderer_end_frame captures raw pointers INTO `m`, so a
         * handler firing after free(m) would be a use-after-free. Submit an
         * empty command buffer on the serial queue and wait — its completion
         * guarantees every previously committed frame's handler has run.
         * currentCommandBuffer is NOT waited on: at teardown it is normally
         * nil, and an uncommitted buffer would never complete (would hang). */
        if (m->commandQueue) {
            id<MTLCommandBuffer> drain = [m->commandQueue commandBuffer];
            if (drain) { [drain commit]; [drain waitUntilCompleted]; }
        }
        if (m->frameSlotHeld) { dispatch_semaphore_signal(m->frameSem);m->frameSlotHeld=false; }
        [m->currentCommandBuffer release];m->currentCommandBuffer=nil;
        [m->currentDrawable release];m->currentDrawable=nil;
        [m->surfacePresenter release];m->surfacePresenter=nil;
        /* Release owned Metal objects (MRR — no ARC) */
        [m->glyphPipeline release];
        [m->rectPipeline release];
        if (m->rrectPipeline) [m->rrectPipeline release];
        if (m->rrectPipelineKeepAlpha) [m->rrectPipelineKeepAlpha release];
        if (m->linePipeline) [m->linePipeline release];
        [m->strokePipeline release];
        if (m->blurPipeline) [m->blurPipeline release];
        metal_blur_textures_release(m);
        [m->sampler release];
        [m->samplerNearest release];
        if (m->shaderLibrary) [m->shaderLibrary release];
        if (m->bgImagePipeline) [m->bgImagePipeline release];
        /* NOT a plain release: the wallpaper texture is shared process-wide and
         * other windows may still be drawing it. Closing one window must only
         * drop its reference; the last one out frees the texture. */
        metal_bg_image_unref(m);
        if (m->bgImageSampler) [m->bgImageSampler release];
        for (u32 i = 0; i < METAL_FRAME_BUFFER_COUNT; i++) {
            if (m->frameBuffers[i]) [m->frameBuffers[i] release];
            if (m->frameBufferSizes[i]) {
                mem_budget_adjust(MEM_RENDER_GPU, -(isize)m->frameBufferSizes[i]);
                m->frameBufferSizes[i] = 0;
            }
        }
        for (u32 i = 0; i < METAL_IMAGE_CACHE_CAP; i++) {
            MetalImageCacheEntry *e = &m->imageCache[i];
            if (e->texture) [e->texture release];
            for (u8 si = 0; si < e->spare_count; si++) [e->spare[si] release];
        }
        for (u32 i = 0; i < METAL_IMAGE_OVERSIZE_SLOTS; i++) {
            MetalImageCacheEntry *o = &m->oversize[i];
            if (o->texture) [o->texture release];
            for (u8 k = 0; k < o->spare_count; k++) [o->spare[k] release];
        }
        if (m->imageAccountedBytes)
            mem_budget_adjust(MEM_IMAGE_GPU, -(isize)m->imageAccountedBytes);
        /* The fence is a dispatch object and this file is MRC — it needs an
         * explicit release. The drain above guarantees every completed handler
         * that captured it has already run, so this drops the last reference. */
        if (m->frameSem) {
            dispatch_release(m->frameSem);
            m->frameSem = NULL;
        }
        /* Same guarantee as the fence above: the teardown drain has run every
         * completion handler that retained this, so this drops the last
         * reference. A handler still in flight would keep it alive anyway. */
        if (m->epoch) { [m->epoch release]; m->epoch = nil; }
        if (m->encodeQueue) { dispatch_release(m->encodeQueue);m->encodeQueue=NULL; }
        /* The atlas queue: bound textures (retained), the pending staging
         * buffer and records, the pool and any requeued batch. After the
         * drain nothing on the GPU reads or writes them. */
        metal_atlas_destroy(m);
        /* device, commandQueue, layer are borrowed — not retained here */
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
static void metal_oversize_release(RendererMetal *m, MetalImageCacheEntry *e);

void renderer_begin_frame(Renderer *r, i32 w, i32 h) {
    retained_collect(r, false, metal_retained_destroy);
    r->frame_deferred = false;
    r->cursor_blink_in_frame = false;
    r->frame_waiting_drawable = false;
    r->screen_width  = w;
    r->screen_height = h;
    r->glyph_count   = 0;
    r->rect_batch_count = 0;
    r->rrect_batch_count = 0;
    r->last_drawable_wait_sec = 0.0;
#ifdef LIU_RENDER_REPLAY_PROBE
    r->replay_cache_to_renderer_bytes = 0;
    r->replay_cache_to_renderer_ns = 0;
    r->replay_renderer_to_backend_bytes = 0;
    r->replay_renderer_to_backend_ns = 0;
    r->replay_glyph_instances = 0;
    r->replay_rect_instances = 0;
    r->replay_deco_instances = 0;
    r->replay_terminal_panes = 0;
    r->replay_rows_rebuilt = 0;
    r->replay_terminal_upload = false;
#endif
    /* Not flushed by renderer_end_frame (the line pipeline is additive, so a
     * late flush would paint stale coordinates over an unrelated frame). No
     * producer leaks one today — every renderer_draw_line region ends in an
     * unconditional renderer_flush_lines — so this only bounds the damage if
     * one ever does. Do NOT add renderer_flush_lines to end_frame instead: it
     * would DRAW the leaked batch rather than drop it, and inserting it into
     * the rects->rrects->glyphs order breaks the layering invariant. */
    r->line_batch_count = 0;

    RendererMetal *m = r->backend;
    if (m) m->blurValid = false;
    if (!m) return;
    r->frame_deferred = true;

    /* A busy GPU must not block the event thread: the next keystroke and PTY
     * drain run on this same thread. Keep the paint pending until a slot is
     * available; only a successful acquisition owes a completion signal. */
    if (m->frameSem) {
        f64 _st0 = atomic_load_explicit(&g_liu_gpu_timing_on, memory_order_relaxed)
                       ? platform_time_sec() : 0.0;
        long busy = dispatch_semaphore_wait(m->frameSem, DISPATCH_TIME_NOW);
        if (atomic_load_explicit(&g_liu_gpu_timing_on, memory_order_relaxed))
            g_liu_fprobe_fence += platform_time_sec() - _st0;
        if (busy != 0) return;
        m->frameSlotHeld = true;
    }

    @autoreleasepool {
        /* Match the layer's drawable size to the render dimensions BEFORE
         * acquiring the drawable, so the drawable texture is the size about to be
         * rendered — preventing a stale/oversized drawable for one frame during
         * interactive resize (split divider / sidebar / window edge). The
         * inequality guard keeps this a no-op on steady-state frames; on a
         * presentsWithTransaction layer (set during split/sidebar drag) this size
         * commit rides the same CA transaction as the present, so resize is
         * atomic. */
        if (m->layer) {
            CGSize _backing = CGSizeMake((CGFloat)w, (CGFloat)h);
            if (!CGSizeEqualToSize(m->layer.drawableSize, _backing))
                m->layer.drawableSize = _backing;
        }
        if (m->surfacePresenter) {
            atomic_store(&m->surfacePresenter->wake,r->frame_ready_wake);
            /* Planning is opt-in until end-to-end latency acceptance. Features that
             * read the framebuffer keep a full attachment from frame begin. */
            m->frameTargetPlanned=active_target_enabled() && m->encodeQueue &&
                m->layer.framebufferOnly && r->clear_color[3]==1 && !r->bg_texture &&
                !liu_dev_env("LIU_DUMP_FRAME") && w>0 && h>16 &&
                (usize)w*(usize)(h-16)*4>LIU_SURFACE_COMPACT_MIN_BYTES;
            m->currentDrawable=[(m->frameTargetPlanned ? [m->surfacePresenter nextRecordingWidth:(NSUInteger)w height:(NSUInteger)h] : [m->surfacePresenter nextWidth:(NSUInteger)w height:(NSUInteger)h]) retain];
            if (!m->currentDrawable && m->surfacePresenter->allocation_failed) {
                [m->surfacePresenter stop];[m->surfacePresenter release];m->surfacePresenter=nil;
                fprintf(stderr,"renderer_metal: IOSurface allocation failed; using CAMetalLayer\n");
            }
        } else {
        LiuDrawableMailbox *box=m->acquire;
        [box->lock lock];
        m->currentDrawable=box->ready; box->ready=nil;
        if (m->currentDrawable &&
            (m->currentDrawable.texture.width != (NSUInteger)w ||
             m->currentDrawable.texture.height != (NSUInteger)h)) {
            [m->currentDrawable release]; m->currentDrawable=nil;
        }
        box->waiting = !m->currentDrawable;
        [box->lock unlock];
        }
        if (!m->currentDrawable) {
            r->frame_waiting_drawable = true;
            if (!m->surfacePresenter) metal_request_drawable(m->acquire, m->layer, r->frame_ready_wake);
            if (m->frameSlotHeld) { dispatch_semaphore_signal(m->frameSem); m->frameSlotHeld = false; }
            return;
        }

        m->currentCommandBuffer = LIU_METAL_FAULT(LIU_METAL_FAULT_COMMAND_BUFFER)
                                ? nil : [metal_record_command_buffer(m->commandQueue,m->encodeQueue) retain];
        if (!m->currentCommandBuffer) {
            [m->currentDrawable release]; m->currentDrawable = nil;
            if (m->frameSlotHeld) { dispatch_semaphore_signal(m->frameSem); m->frameSlotHeld = false; }
            return;
        }
        m->lastFrameTime=platform_time_sec();

        /* The atlas preparation goes on this command buffer BEFORE the render
         * encoder opens: blits and the clears of replacement textures are
         * their own encoders, and Metal orders them ahead of the draw that
         * follows on the same buffer. After this point the frame samples a
         * fixed atlas generation — anything font.c writes from here on is
         * queued for the next preparation and gated out of this frame's
         * glyph lookups. */
        bool atlas_work = metal_atlas_prepare(m, r, m->currentCommandBuffer);

        MTLRenderPassDescriptor *passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        passDesc.colorAttachments[0].texture    = m->currentDrawable.texture;
        passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
        passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
        passDesc.colorAttachments[0].clearColor = MTLClearColorMake(
            r->clear_color[0], r->clear_color[1],
            r->clear_color[2], r->clear_color[3]);
#ifdef LIU_METAL_BENCH_PASS
        LIU_METAL_BENCH_PASS(passDesc);
#endif

        m->currentEncoder = LIU_METAL_FAULT(LIU_METAL_FAULT_ENCODER)
                          ? nil
                          : [[m->currentCommandBuffer renderCommandEncoderWithDescriptor:passDesc] retain];
        if (!m->currentEncoder) {
            /* If the buffer carries atlas work, font.c has been told those
             * texels are on their way — so it is committed, draw-less.
             * Dropping it would leave the CPU's "GPU up to date" ahead of the
             * GPU. An empty buffer is simply let go, as before. The slot is
             * released inline: no present, no handler owes it. */
            if (atlas_work) metal_commit_tracked(m, m->currentCommandBuffer);
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

    /* Advance only for a frame that will be submitted. Failed acquisitions
     * return their permit immediately, so counting them in the ring rotation
     * could select storage that an older submitted frame still reads. */
    m->frameBufferIndex = (m->frameBufferIndex + 1) % METAL_FRAME_BUFFER_COUNT;
    m->frameBufferOffset = 0;
    m->frameSerial++;
    m->frameNowSec = platform_time_sec();
    r->frame_deferred = false;

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

/* Capture the finished drawable as RGBA8 before the window server composites
 * it. The dump includes the color and alpha values produced by the renderer.
 */
static void metal_debug_dump_drawable(RendererMetal *m, const char *path) {
    if (!m || !m->currentDrawable || !m->currentCommandBuffer) return;
    id<MTLTexture> tex = m->currentDrawable.texture;
    if (!tex) return;
    if (m->layer && m->layer.framebufferOnly) {
        fprintf(stderr, "liu: LIU_DUMP_FRAME needs framebufferOnly=NO "
                        "(set the env var before launch)\n");
        return;
    }

    NSUInteger w = tex.width, h = tex.height, bpr = w * 4;
    id<MTLBuffer> staging = [m->device newBufferWithLength:bpr * h
                                                   options:MTLResourceStorageModeShared];
    if (!staging) return;

    id<MTLBlitCommandEncoder> blit = [m->currentCommandBuffer blitCommandEncoder];
    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:staging destinationOffset:0
    destinationBytesPerRow:bpr destinationBytesPerImage:bpr * h];
    [blit endEncoding];
    [m->currentCommandBuffer commit];
    [m->currentCommandBuffer waitUntilCompleted];

    FILE *f = fopen(path, "wb");
    if (f) {
        /* Pixel order follows the layer format; bgra8Unorm(_sRGB) is what
         * CAMetalLayer uses here, so say so rather than making the reader
         * guess which two channels are swapped. */
        const char *order = (tex.pixelFormat == MTLPixelFormatRGBA8Unorm ||
                             tex.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB)
                            ? "rgba" : "bgra";
        fprintf(f, "LIUFB %lu %lu %s\n", (unsigned long)w, (unsigned long)h, order);
        fwrite([staging contents], 1, bpr * h, f);
        fclose(f);
        fprintf(stderr, "liu: dumped %lux%lu %s frame to %s\n",
                (unsigned long)w, (unsigned long)h, order, path);
    } else {
        fprintf(stderr, "liu: LIU_DUMP_FRAME could not open %s\n", path);
    }
    [staging release];

    /* One shot. The command buffer above was committed and waited on, so the
     * caller must not commit it a second time — end_frame checks this flag. */
}

/* Per-command-buffer GPU busy time for LIU_FRAME_PROBE. Runs on a Metal
 * completion thread, so everything here is lock-free; all of it is zero-cost
 * when the probe is off. GPUStartTime/GPUEndTime are 0 on a command buffer
 * the GPU never ran.
 *
 * The histogram is fed HERE, with each buffer's own duration, rather than
 * leaving the main thread to reconstruct samples from a running total and a
 * count. That reconstruction can only produce total_delta/count_delta — an
 * average — so a frame of 0.5, 0.5 and 15.0 ms would come back as three
 * samples of 5.33 ms and the spike would be lost.
 *
 * The running total and count stay: the per-second probe line reports GPU
 * busy per frame from them, which is a different question from the tail. */
static inline void metal_probe_gpu_time(id<MTLCommandBuffer> cb,
                                        i32 replay_bucket) {
#ifndef LIU_RENDER_REPLAY_PROBE
    (void)replay_bucket;
#endif
    if (!cb) return;
#ifndef LIU_RENDER_REPLAY_PROBE
    if (!atomic_load_explicit(&g_liu_gpu_timing_on, memory_order_relaxed)) return;
#endif
    double busy = cb.GPUEndTime - cb.GPUStartTime;
    if (busy <= 0.0) return;
    unsigned long long ns = (unsigned long long)(busy * 1e9);
#ifdef LIU_RENDER_REPLAY_PROBE
    if (replay_bucket >= 0 && replay_bucket < 2)
        liu_hist_atomic_add_ns(&g_liu_replay_gpu_hist[replay_bucket], ns);
#endif
    if (!atomic_load_explicit(&g_liu_gpu_timing_on, memory_order_relaxed)) return;
    liu_hist_atomic_add_ns(&g_liu_fprobe_gpu_hist, ns);
    atomic_fetch_add(&g_liu_fprobe_gpu_ns, ns);
    atomic_fetch_add(&g_liu_fprobe_gpu_cnt, 1u);
}

/* Every commit of a FRAME command buffer goes through here.
 *
 * Frames are the only thing that samples the glyph atlas, so counting them is
 * what makes renderer_gpu_mark()'s barrier sound. The other two command buffers
 * this file creates (the teardown drain and renderer_gpu_drain's own) encode
 * nothing and read nothing, and deliberately stay uncounted — counting them
 * would make a mark wait on a buffer created to satisfy that very mark.
 *
 * The handler captures only `ep`, never `m`: it can fire after the window is
 * closed, and a block capturing an ObjC pointer retains it under MRC. */
/* The completion handler's body, as a named function so it can be called
 * directly with either status: a real GPU reports MTLCommandBufferStatusError
 * only after a device reset or a kernel that runs past the watchdog. What
 * matters here is small and ordered. */
static void metal_epoch_note_completion(LiuGpuEpoch *ep,
                                        MTLCommandBufferStatus status) {
    if (!ep) return;
    [ep->lock lock];
    /* Exactly one of the two totals moves per completion, and `failRun` records
     * WHICH — the run of consecutive failures ending here. A serial command queue
     * completes buffers in submission order and Metal calls the handlers in that
     * order, so this run is the run the screen actually experienced. That
     * assumption is the one thing this rests on; if handlers could be reordered the
     * order would not exist to publish. */
    if (status == MTLCommandBufferStatusError) {
        ep->failed++;
        ep->failRun++;
    } else {
        ep->ok++;
        ep->failRun = 0;
    }
    [ep->lock unlock];
}

static void metal_commit_tracked(RendererMetal *m, id<MTLCommandBuffer> cb) {
    if (!cb) return;
    /* Deferred/non-presented frames still write their render target. Retain
     * the drawable lease through completion even when there is no publication
     * block to own it; the pool must not recycle that target while in flight. */
    id<CAMetalDrawable> lease=m ? m->currentDrawable : nil;
    if ([(id)lease isKindOfClass:[LiuSurfaceDrawable class]]) {
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            (void)done;(void)[lease texture]; /* retained block capture */
        }];
    }
    LiuGpuEpoch *ep = m ? m->epoch : nil;
    if (ep) {
        [ep->lock lock];
        ep->submitted++;
        [ep->lock unlock];
        /* Decided at COMMIT and carried into the block, so the injected
         * outcome rides the real completion handler at the real time — the
         * ordering the retry policy depends on is the one under test. */
        const bool inject_error = LIU_METAL_FAULT(LIU_METAL_FAULT_COMPLETE_ERROR);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            metal_epoch_note_completion(ep, inject_error ? MTLCommandBufferStatusError
                                                         : done.status);
        }];
    }
    [cb commit];
}

#include "renderer/metal_target.h"
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

    if(m->surfacePresenter && m->currentDrawable) {
        LiuSurfaceDrawable *drawable=(LiuSurfaceDrawable *)m->currentDrawable;
        drawable->compact_allowed=r->idle_snapshot_allowed && !r->bg_texture &&
            r->clear_color[3]>=1.0f && !r->cursor_blink_in_frame && !liu_dev_env("LIU_DUMP_FRAME");
        u32 bg=0xff000000u;
        for(i32 i=0;i<3;i++) {
            f32 c=r->clear_color[i];
            c=fminf(1,fmaxf(0,c));
            f32 srgb=c<=0.0031308f ? c*12.92f : 1.055f*powf(c,1.0f/2.4f)-0.055f;
            bg|=(u32)lroundf(srgb*255.0f)<<(16-i*8);
        }
        drawable->compact_background=bg;
    }
    if(!active_target_finish(r,m)) {
        if([(id)m->currentCommandBuffer isKindOfClass:[LiuMetalRecordedBuffer class]])
            atomic_store(&((LiuMetalRecordedBuffer *)m->currentCommandBuffer)->packet->state->failed,true);
        r->frame_deferred=true;
    }

    if (m->currentEncoder) {
        [m->currentEncoder endEncoding];
        [m->currentEncoder release];
        m->currentEncoder = nil;
    }

    /* Attach the fence-release completed handler to the command buffer about
     * to be committed. The handler signals the semaphore exactly once when the
     * GPU finishes this frame, releasing the in-flight slot acquired in
     * begin_frame. Capture the semaphore by value (a retained dispatch object
     * pointer) rather than `m`, so the block does NOT keep `m` alive and is
     * safe even if the renderer is torn down — the handler touches only the
     * semaphore, never `m`'s fields. frameSlotHeld is cleared up-front because
     * the slot's release is now owned by the handler (or by the inline signal
     * on the no-command-buffer path below). */
    bool slotOwedToHandler = m->frameSlotHeld;
    m->frameSlotHeld = false;
    dispatch_semaphore_t fenceSem = m->frameSem;
#ifdef LIU_RENDER_REPLAY_PROBE
    const i32 replay_bucket = r->replay_rows_rebuilt > 0 ? 1 : 0;
#else
    const i32 replay_bucket = -1;
#endif

    /* Armed dump: consume the env var so exactly one frame is written, and
     * take over the command buffer (the blit needs it committed and waited on).
     * Done BEFORE present so the pixels are the ones about to be shown. */
    const char *dump_path = liu_dev_env("LIU_DUMP_FRAME");
    if (dump_path && *dump_path && m->currentDrawable && !r->frame_deferred) {
        /* Frame 0 is an empty terminal — the shell has not printed its prompt
         * yet, so a dump taken there shows nothing worth measuring. Wait a beat
         * (LIU_DUMP_FRAME_AFTER, default ~2s at 60 Hz) before arming. */
        static i32 dump_countdown = -1;
        if (dump_countdown < 0) {
            const char *after = liu_dev_env("LIU_DUMP_FRAME_AFTER");
            dump_countdown = after ? atoi(after) : 120;
        }
        if (dump_countdown-- > 0) goto dump_not_yet;
        /* Copy before the unsetenv: getenv's pointer is into the environment's
         * own storage, and for a value that arrived through setenv (the dump
         * armed from inside the process) unsetenv frees it, which would leave
         * the path empty and the dump going nowhere. */
        char dump_copy[1024];
        snprintf(dump_copy, sizeof dump_copy, "%s", dump_path);
        unsetenv("LIU_DUMP_FRAME");
        /* Commits and WAITS internally, so this frame's buffer has finished by
         * the time the call returns — which is why it is the one frame commit
         * that does not go through metal_commit_tracked() and still cannot fool
         * a GPU mark into thinking a live reader is done. */
        metal_debug_dump_drawable(m, dump_copy);
        if (m->currentDrawable) [m->currentDrawable present];
        [m->currentDrawable release]; m->currentDrawable = nil;
        [m->currentCommandBuffer release]; m->currentCommandBuffer = nil;
        if (slotOwedToHandler && fenceSem) dispatch_semaphore_signal(fenceSem);
        return;
    }
dump_not_yet:

    if (m->currentDrawable && !r->frame_deferred) {
        LiuDrawableMailbox *box=m->acquire;
        bool observe=r->observe_presentation;
        f64 through=observe ? platform_time_sec() : 0;
        f64 origin=observe ? platform_time_origin() : 0;
        u64 record=0;
        if (observe) {
        [box->lock lock];
        if (box->submitted-box->consumed >= 64) {
            box->lost++; box->consumed=box->submitted;
        }
        record=box->submitted++;
        box->observations[record%64].id=record;
        box->observations[record%64].done=NO;
        [box->lock unlock];
        [m->currentDrawable addPresentedHandler:^(id<MTLDrawable> drawable) {
            [box->lock lock];
            if (record>=box->consumed && box->observations[record%64].id==record) {
                f64 host=drawable.presentedTime;
                /* The surface presenter has no presentation time; it does know
                 * when the frame became the layer's contents. */
                if (host<=0 && [(id)drawable respondsToSelector:@selector(liuLayerTime)]) {
                    host=[(LiuSurfaceDrawable *)drawable liuLayerTime];
                    if (host>0) box->layer_stamps=YES;
                }
                box->observations[record%64].when=host>0 ? host-origin : 0;
                box->observations[record%64].through=through;
                box->observations[record%64].done=YES;
            }
            [box->lock unlock];
        }];
        }
        if (slotOwedToHandler && fenceSem) {
            [m->currentCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull cb) {
                if (observe && cb.status == MTLCommandBufferStatusError) {
                    [box->lock lock];
                    if (record>=box->consumed && box->observations[record%64].id==record) {
                        box->observations[record%64].through=through;
                        box->observations[record%64].when=0;
                        box->observations[record%64].done=YES;
                    }
                    [box->lock unlock];
                }
                metal_probe_gpu_time(cb, replay_bucket);
                dispatch_semaphore_signal(fenceSem);
            }];
            slotOwedToHandler = false; /* present path: handler owns the release */
        }

        /* If the layer uses presentsWithTransaction, the drawable is
         * presented once the buffer is scheduled; otherwise the command
         * buffer presents it asynchronously. */
        if (m->surfacePresenter) {
            id<CAMetalDrawable> drawable=m->currentDrawable;
            [m->currentCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
                if (cb.status==MTLCommandBufferStatusCompleted) [drawable present];
            }];
            metal_commit_tracked(m,m->currentCommandBuffer);
        } else if (m->layer.presentsWithTransaction) {
            id<CAMetalDrawable> drawable=m->currentDrawable;
            [m->currentCommandBuffer addScheduledHandler:^(id<MTLCommandBuffer> cb) {
                (void)cb;
                [CATransaction begin]; [drawable present]; [CATransaction commit];
            }];
            metal_commit_tracked(m, m->currentCommandBuffer);
        } else {
            [m->currentCommandBuffer presentDrawable:m->currentDrawable];
            metal_commit_tracked(m, m->currentCommandBuffer);
        }
    } else if (m->currentCommandBuffer) {
        if (slotOwedToHandler && fenceSem) {
            [m->currentCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull cb) {
                metal_probe_gpu_time(cb, replay_bucket);
                dispatch_semaphore_signal(fenceSem);
            }];
            slotOwedToHandler = false; /* handler now owns the release */
        }
        metal_commit_tracked(m, m->currentCommandBuffer);
    }
    if (m->currentCommandBuffer) {
        [m->currentCommandBuffer release];
        m->currentCommandBuffer = nil;
    }

    if (m->currentDrawable) {
        [m->currentDrawable release];
        m->currentDrawable = nil;
        /* Acquire during the interval before the next present, without
         * submitting another frame or blocking the input thread. */
        if (!m->surfacePresenter) metal_request_drawable(m->acquire, m->layer, r->frame_ready_wake);
    }

    /* Safety net: if we still owe a slot release but never attached a handler
     * (no command buffer at all to carry it — should not happen when
     * begin_frame succeeded, but covers any future early-out), release inline
     * so the fence cannot leak a permanent slot and eventually deadlock. */
    if (slotOwedToHandler && fenceSem) {
        dispatch_semaphore_signal(fenceSem);
    }
}

usize renderer_reclaim_image_cache(Renderer *r) {
    RendererMetal *m = r ? r->backend : NULL;
    if (!m) return 0;
    usize before = m->imageAccountedBytes;
    f64 now = platform_time_sec();
    /* Under pressure the idle timeout is not the question — anything that was
     * not drawn in the last few frames is cold, and a dropped entry costs a
     * re-upload from the CPU image the caller still holds, never a picture.
     * The 0.1 s grace is what keeps a texture being drawn every frame (and a
     * second window's, drawn on its own cadence) out of it. */
    for (u32 i = 0; i < METAL_IMAGE_CACHE_CAP; i++) {
        MetalImageCacheEntry *e = &m->imageCache[i];
        if (e->texture && e->last_used_sec > 0.0 &&
            now - e->last_used_sec > 0.1)
            metal_image_cache_release_entry(m, e);
    }
    for (u32 i = 0; i < METAL_IMAGE_OVERSIZE_SLOTS; i++) {
        MetalImageCacheEntry *o = &m->oversize[i];
        if (o->texture && o->last_used_sec > 0.0 &&
            now - o->last_used_sec > 0.1)
            metal_oversize_release(m, o);   /* which re-accounts */
    }
    return before > m->imageAccountedBytes ? before - m->imageAccountedBytes : 0;
}

bool renderer_set_terminal_cursor(Renderer *r, RendererCursor cursor) {
    RendererMetal *m=r ? r->backend : NULL;
    if (!m || !m->surfacePresenter || !m->surfacePresenter->cursor_layer || !r->cursor_layer_allowed ||
        !m->currentDrawable || liu_dev_env("LIU_DUMP_FRAME")) return false;
    /* Match draw_rect's pixel snapping and this pane's current clip. */
    f32 x=floorf(cursor.x), y=floorf(cursor.y);
    f32 right=x+ceilf(cursor.w), bottom=y+ceilf(cursor.h);
    f32 left=0, top=0, max_x=r->screen_width, max_y=r->screen_height;
    if (r->scissor_active) {
        left=fmaxf(left,r->scissor_x);top=fmaxf(top,r->scissor_y);
        max_x=fminf(max_x,r->scissor_x+r->scissor_w);
        max_y=fminf(max_y,r->scissor_y+r->scissor_h);
    }
    cursor.x=fmaxf(x,left);cursor.y=fmaxf(y,top);
    cursor.w=fmaxf(0,fminf(right,max_x)-cursor.x);
    cursor.h=fmaxf(0,fminf(bottom,max_y)-cursor.y);
    ((LiuSurfaceDrawable *)m->currentDrawable)->cursor=cursor;
    return true;
}

/* Terminal chrome as native layers is OPT-IN (LIU_NATIVE_CHROME=1); the
 * default is the GPU draw. A layer is composited by CoreAnimation in the
 * display's encoded space while the framebuffer blends in linear light, so a
 * translucent rect — the inactive pane's wash, a 0.42-alpha thumb — does not
 * come out the same strength (black at 10% darkens about twice as much). Frame
 * dumps take the GPU path only (see LIU_DUMP_FRAME below), so they cannot show
 * this path. What it buys is memory: a visible scrollbar does not pin the full
 * surface. Main thread only. */
bool renderer_native_chrome_wanted(const char *env) {
    return env && env[0] == '1' && env[1] == '\0';
}

static bool native_chrome_enabled(void) {
    static i32 on = -1;
    if (on < 0) on = renderer_native_chrome_wanted(getenv("LIU_NATIVE_CHROME")) ? 1 : 0;
    return on == 1;
}

bool renderer_set_terminal_rects(Renderer *r, const RendererTerminalRect *rects, u32 count) {
    RendererMetal *m=r ? r->backend : NULL;
    if (!native_chrome_enabled()) return false;
    if (!m || !m->surfacePresenter || !r->cursor_layer_allowed || !m->currentDrawable ||
        !rects || !count || liu_dev_env("LIU_DUMP_FRAME")) return false;
    LiuSurfaceDrawable *drawable=(LiuSurfaceDrawable *)m->currentDrawable;
    if (count>RENDERER_TERMINAL_RECT_CAP-drawable->terminal_rect_count) return false;
    f32 left=0,top=0,right=r->screen_width,bottom=r->screen_height;
    if (r->scissor_active) {
        left=fmaxf(left,r->scissor_x);top=fmaxf(top,r->scissor_y);
        right=fminf(right,r->scissor_x+r->scissor_w);
        bottom=fminf(bottom,r->scissor_y+r->scissor_h);
    }
    /* A clipped rounded pill must keep its original shape. Leave partial
     * clips to the GPU instead of rounding the newly clipped rectangle. */
    for (u32 i=0;i<count;i++) {
        const RendererTerminalRect *v=&rects[i];
        if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->w) || !isfinite(v->h) ||
            !isfinite(v->radius) || v->w<=0 || v->h<=0 || v->radius<0 ||
            v->x<left || v->y<top || v->x+v->w>right || v->y+v->h>bottom) return false;
    }
    if (![m->surfacePresenter prepareTerminalRects:drawable->terminal_rect_count+count]) return false;
    memcpy(drawable->terminal_rects+drawable->terminal_rect_count,rects,count*sizeof(*rects));
    drawable->terminal_rect_count+=count;
    return true;
}

static usize metal_trim_frame_buffers(Renderer *r) {
    RendererMetal *m=r ? r->backend : NULL;
    if(!m || !renderer_gpu_idle(r))return 0;
    usize freed=0;
    for(u32 i=0;i<METAL_FRAME_BUFFER_COUNT;i++) {
        freed+=m->frameBufferSizes[i];
        [m->frameBuffers[i] release];m->frameBuffers[i]=nil;
        m->frameBufferSizes[i]=0;
    }
    if(freed)mem_budget_adjust(MEM_RENDER_GPU,-(isize)freed);
    m->frameBufferOffset=0;
    return freed;
}

usize renderer_reclaim_presenter(Renderer *r) {
    RendererMetal *m=r ? r->backend : NULL;
    if(!m)return 0;
    usize freed=m->surfacePresenter ? [m->surfacePresenter trimIdle:true] : 0;
    return freed+metal_trim_frame_buffers(r);
}

void renderer_trim_idle_resources(Renderer *r, f64 now_sec) {
    RendererMetal *m = r ? r->backend : NULL;
    if (!m) return;
    if (now_sec <= 0.0) now_sec = platform_time_sec();
    m->frameNowSec = now_sec;
    /* LIU_SURFACE_IDLE_SEC=<s>: how long without a frame before the spare
     * surfaces are given back and the shown one is compacted (2 s). */
    static f64 idle_after=-1.0;
    if (idle_after<0.0) {
        const char *e=getenv("LIU_SURFACE_IDLE_SEC");
        f64 v=e && e[0] ? strtod(e,NULL) : 0.0;
        idle_after=(v>=0.5 && v<=86400.0) ? v : 2.0;
    }
    bool idle=m->lastFrameTime>0 && now_sec-m->lastFrameTime>=idle_after;
    if (m->surfacePresenter) {
        [m->surfacePresenter trimIdle:idle];
        if(idle)[m->surfacePresenter compactIfIdle];
    }
    /* Preserve the ring across ordinary pauses and bursts. A genuinely cold
     * renderer returns its high-water storage; the next frame grows lazily.
     * Never retire an open/deferred packet or any outstanding GPU submission. */
    if(m->lastFrameTime>0 && now_sec-m->lastFrameTime>=15.0)
        (void)metal_trim_frame_buffers(r);
    /* Opt-in resource evidence for a real-app idle run. These are owned
     * surface/buffer capacities, separate from external physical footprint. */
    const char *surface_stats=liu_dev_env("LIU_SURFACE_STATS");
    if (surface_stats && surface_stats[0]=='1' && m->surfacePresenter &&
        now_sec-m->surfaceStatsLastSec>=1.0) {
        LiuSurfacePresenter *p=m->surfacePresenter;
        usize pool=0,stream=0;u32 count=0;
        for(u32 i=0;i<3;i++)if(p->images[i]){pool+=p->images[i]->accounted;count++;}
        for(u32 i=0;i<METAL_FRAME_BUFFER_COUNT;i++)stream+=m->frameBufferSizes[i];
        fprintf(stderr,"SURFACE_MEMORY {\"time\":%.3f,\"pool_bytes\":%zu,\"pool_count\":%u,"
            "\"snapshot_bytes\":%zu,\"shown_height\":%lu,\"stream_bytes\":%zu,"
            "\"compact_allowed\":%d,\"compacted\":%llu,\"blink_in_frame\":%d,\"image_bytes\":%zu}\n",
            now_sec,pool,count,p->snapshot?p->snapshot->accounted:0,
            (unsigned long)(p->shown?p->shown->texture.height:0),stream,(i32)p->compact_allowed,
            (unsigned long long)p->compacted,(i32)r->cursor_blink_in_frame,m->imageAccountedBytes);
        m->surfaceStatsLastSec=now_sec;
    }
    /* UI handles can be retired while this window is hidden. Recorded/native
     * command buffers retain their own resources until encoding/GPU completion. */
    retained_collect(r, false, metal_retained_destroy);
    metal_retained_trim(r, now_sec);

    /* Retire at most one unused upload buffer per maintenance sweep. Font
     * textures and in-flight batches remain intact; returning to the window
     * can draw its existing glyphs without allocating upload scratch. */
    if (m->stagingLastUseSec > 0.0 && now_sec - m->stagingLastUseSec > 30.0) {
        [m->atlasLock lock];
        id<MTLBuffer> retired = m->stagingPoolCount ? m->stagingPool[--m->stagingPoolCount] : nil;
        [m->atlasLock unlock];
        metal_atlas_buffer_release(retired);
    }

    /* Wall-clock eviction, not rendered-frame eviction: a closed viewer in a
     * completely idle window produces no more frames, and its 32-256 MiB
     * texture would otherwise stay resident indefinitely. Main calls this
     * lightweight sweep once per second even when there is nothing to paint.
     */
    for (u32 i = 0; i < METAL_IMAGE_CACHE_CAP; i++) {
        MetalImageCacheEntry *e = &m->imageCache[i];
        if (e->texture && e->last_used_sec > 0.0 &&
            now_sec - e->last_used_sec > METAL_IMAGE_CACHE_IDLE_SEC)
            metal_image_cache_release_entry(m, e);
    }
    for (u32 i = 0; i < METAL_IMAGE_OVERSIZE_SLOTS; i++) {
        MetalImageCacheEntry *o = &m->oversize[i];
        if (o->texture && o->last_used_sec > 0.0 &&
            now_sec - o->last_used_sec > METAL_IMAGE_OVERSIZE_IDLE_SEC)
            metal_oversize_release(m, o);   /* which re-accounts */
    }

    if (!m->blurSnapshot && !m->blurIntermediate) return;

    /* These are full-frame private textures. Keeping them while a modal is
     * open avoids churn; once blur has been unused for a few seconds, freeing
     * them gives macOS/Metal back the resident IOSurface/VRAM without changing
     * the next modal's visual result. */
    if (m->blurLastUseSec <= 0.0 || now_sec - m->blurLastUseSec < 3.0) return;
    metal_blur_textures_release(m);
    m->blurValid = false;
}

/* =========================================================================
 * Pass 1 + 3: Rect batching
 * ========================================================================= */

void renderer_draw_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h, Color c) {
    /* LIU_BLUR_DEBUG: flag a fill opaque and large enough to hide the window
     * blur. Lives in renderer.c outside its OpenGL guard, so the probe exists
     * on Metal, the only backend that has a blur. */
    renderer_blur_probe_draw(r, x, y, w, h, c.a, "rect");
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

/* Draw-call accounting.
 *
 * Every flush is a pipeline switch plus a draw, and the UI layer calls them
 * from hundreds of sites; how many fire in one frame cannot be read off the
 * source, so LIU_FRAME_PROBE prints the count. Two ints and two increments —
 * cheap enough to stay compiled in. */
u32 g_liu_draw_calls;
u32 g_liu_draw_instances;

void renderer_flush_rects(Renderer *r) {
    if (r->rect_batch_count == 0) return;
    g_liu_draw_calls++; g_liu_draw_instances += r->rect_batch_count;

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
        bool vb_ok;
        if (r->retained_draw_buffer) {
            [m->currentEncoder setVertexBuffer:(id<MTLBuffer>)r->retained_draw_buffer offset:0 atIndex:1];
            vb_ok = true;
        } else vb_ok = metal_set_vertex_data(m, m->currentEncoder, r->rect_batch, instance_bytes, 1);

        /* Uniforms at buffer(2) — matches glyph pipeline layout */
        [m->currentEncoder setVertexBytes:&uniforms
                                   length:sizeof(uniforms)
                                  atIndex:2];

        /* An allocation failure leaves the encoder holding the previous
         * batch's vertex buffer; drawing this batch's instance count
         * against it is worse than dropping the batch. */
        if (vb_ok) {
            [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                  vertexStart:0
                                  vertexCount:6
                                instanceCount:r->rect_batch_count];
        }
    }

    r->rect_batch_count = 0;
}

/* =========================================================================
 * Line batching — thick AA segments (graph-view edges)
 * ========================================================================= */

void renderer_draw_line(Renderer *r, f32 x0, f32 y0, f32 x1, f32 y1,
                        f32 thickness, Color c) {
    if(r->line_batch_count&&r->line_batch_stroke)renderer_flush_lines(r);
    r->line_batch_stroke=false;
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

void renderer_draw_stroke_segment(Renderer *r,f32 x0,f32 y0,f32 x1,f32 y1,
                                 f32 nx0,f32 ny0,f32 nx1,f32 ny1,f32 thickness,Color c) {
    if(r->line_batch_count&&!r->line_batch_stroke)renderer_flush_lines(r);
    if(r->line_batch_count+2>r->line_batch_cap)renderer_flush_lines(r);
    if(r->line_batch_count+2>r->line_batch_cap&&!renderer_reserve_lines(r,r->line_batch_count+2))return;
    Color lin=color_linear_from_srgb(c);
    LineInstance *li=&r->line_batch[r->line_batch_count];
    li[0]=(LineInstance){x0,y0,x1,y1,fmaxf(.5f,thickness),lin.r,lin.g,lin.b,lin.a};
    li[1]=(LineInstance){.x0=nx0,.y0=ny0,.x1=nx1,.y1=ny1};
    r->line_batch_count+=2;r->line_batch_stroke=true;
}

/* Brush ribbons have shared endpoint normals: one continuous AA fringe,
 * rather than overlapping round stamps at every pointer sample. This small
 * pipeline is compiled lazily by Metal itself and leaves the bundled terminal
 * shaders and their instance ABI unchanged. */
static bool ensure_stroke_pipeline(RendererMetal *m) {
    if(m->strokePipeline)return true;if(m->strokePipelineTried)return false;
    m->strokePipelineTried=true;
    NSString *src=@"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct L { packed_float4 p; float width; packed_float4 color; };\n"
    "struct O {float4 position [[position]];float4 color;float distance;float radius;};\n"
    "vertex O stroke_vertex(uint v [[vertex_id]],uint i [[instance_id]],constant float2 *quad [[buffer(0)]],constant L *items [[buffer(1)]],constant float4x4 &projection [[buffer(2)]]) {\n"
    " float2 q=quad[v];L s=items[i*2];float4 n=float4(items[i*2+1].p);float across=(q.y-.5)*(s.width+2);\n"
    " float2 pos=mix(float2(s.p.xy),float2(s.p.zw),q.x)+mix(n.xy,n.zw,q.x)*across;\n"
    " O o;o.position=projection*float4(pos,0,1);o.color=float4(s.color);o.distance=across;o.radius=s.width*.5;return o;}\n"
    "fragment float4 stroke_fragment(O o [[stage_in]]) {float aa=max(fwidth(o.distance),.75f);float cov=1-smoothstep(o.radius-aa*.5,o.radius+aa*.5,abs(o.distance));return float4(o.color.rgb,o.color.a*cov);}\n";
    NSError *error=nil;
    id<MTLLibrary> lib=[m->device newLibraryWithSource:src options:nil error:&error];
    if(!lib){fprintf(stderr,"renderer_metal: stroke shader: %s\n",error.localizedDescription.UTF8String);return false;}
    id<MTLFunction> vert=[lib newFunctionWithName:@"stroke_vertex"],frag=[lib newFunctionWithName:@"stroke_fragment"];
    MTLRenderPipelineDescriptor *d=[[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction=vert;d.fragmentFunction=frag;
    d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm_sRGB;
    d.colorAttachments[0].blendingEnabled=YES;
    d.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;
    d.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
    d.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorOne;
    d.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
    m->strokePipeline=[m->device newRenderPipelineStateWithDescriptor:d error:&error];
    [d release];[vert release];[frag release];[lib release];
    if(!m->strokePipeline)fprintf(stderr,"renderer_metal: stroke pipeline: %s\n",error.localizedDescription.UTF8String);
    return m->strokePipeline!=nil;
}

void renderer_prepare_strokes(Renderer *r){if(r->backend)ensure_stroke_pipeline(r->backend);}
void renderer_flush_lines(Renderer *r) {
    if (r->line_batch_count == 0) return;
    RendererMetal *pipeline_owner=r->backend;
    if(r->line_batch_stroke && (!pipeline_owner||!ensure_stroke_pipeline(pipeline_owner))) {
        u32 count=r->line_batch_count/2;for(u32 i=0;i<count;i++)r->line_batch[i]=r->line_batch[i*2];
        r->line_batch_count=count;r->line_batch_stroke=false;
    }
    u32 instances=r->line_batch_stroke?r->line_batch_count/2:r->line_batch_count;
    g_liu_draw_calls++; g_liu_draw_instances += instances;
    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder || !m->linePipeline) {
        r->line_batch_count = 0;
        return;
    }
    @autoreleasepool {
        [m->currentEncoder setRenderPipelineState:r->line_batch_stroke?m->strokePipeline:m->linePipeline];
        MetalUniforms uniforms = metal_build_uniforms(r,
            r->font.loaded ? r->font.cell_width  : 0.0f,
            r->font.loaded ? r->font.cell_height : 0.0f);
        usize instance_bytes = r->line_batch_count * sizeof(LineInstance);
        [m->currentEncoder setVertexBytes:s_quad length:sizeof(s_quad) atIndex:0];
        bool vb_ok = metal_set_vertex_data(m, m->currentEncoder, r->line_batch, instance_bytes, 1);
        [m->currentEncoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:2];
        /* An allocation failure leaves the encoder holding the previous
         * batch's vertex buffer; drawing this batch's instance count
         * against it is worse than dropping the batch. */
        if (vb_ok) {
            [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                  vertexStart:0
                                  vertexCount:6
                                instanceCount:instances];
        }
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
    renderer_blur_probe_draw(r, x, y, w, h, fill.a, "rrect");
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

/* Lazy-compile the blur pipeline on first use, mirroring
 * ensure_bg_image_pipeline. Returns false when the shader library predates the
 * blur_* functions, in which case the C API falls back to a flat tinted rrect. */
static bool ensure_blur_pipeline(RendererMetal *m) {
    if (!m || !m->device) return false;
    if (m->blurPipeline) return true;
    if (!m->shaderLibrary)  return false;

    @autoreleasepool {
        id<MTLFunction> vert = [m->shaderLibrary newFunctionWithName:@"blur_vertex"];
        id<MTLFunction> frag = [m->shaderLibrary newFunctionWithName:@"blur_fragment"];
        if (!vert || !frag) {
            if (vert) [vert release];
            if (frag) [frag release];
            return false;
        }
        MTLRenderPipelineDescriptor *blurDesc = [[MTLRenderPipelineDescriptor alloc] init];
        blurDesc.vertexFunction   = vert;
        blurDesc.fragmentFunction = frag;
        blurDesc.colorAttachments[0].pixelFormat     = MTLPixelFormatBGRA8Unorm_sRGB;
        blurDesc.colorAttachments[0].blendingEnabled  = YES;
        /* Premul output: src.rgb already includes outer_mask. */
        blurDesc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
        blurDesc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        blurDesc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
        blurDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        NSError *error = nil;
        m->blurPipeline = [m->device newRenderPipelineStateWithDescriptor:blurDesc error:&error];
        [blurDesc release];
        [vert release];
        [frag release];
        if (!m->blurPipeline) {
            fprintf(stderr, "renderer_metal: blur pipeline failed (%s) — flat-tint fallback\n",
                    error.localizedDescription.UTF8String);
            return false;
        }
    }
    return true;
}

/* Release the blur pair and take it off the ledger. Idempotent, so the idle
 * sweep, a resize and renderer destruction can all call it without any risk
 * of subtracting twice. */
static void metal_blur_textures_release(RendererMetal *m) {
    if (m->blurSnapshot) { [m->blurSnapshot release]; m->blurSnapshot = nil; }
    if (m->blurIntermediate) { [m->blurIntermediate release]; m->blurIntermediate = nil; }
    if (m->blurAccounted) {
        mem_budget_adjust(MEM_RENDER_GPU, -(isize)m->blurAccounted);
        m->blurAccounted = 0;
    }
}

static bool metal_ensure_blur_textures(RendererMetal *m, NSUInteger w, NSUInteger h) {
    if (m->blurSnapshot && m->blurSnapshot.width == w && m->blurSnapshot.height == h
        && m->blurIntermediate && m->blurIntermediate.width == w && m->blurIntermediate.height == h) {
        return true;
    }
    metal_blur_textures_release(m);

    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                     width:w height:h mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;
    m->blurSnapshot     = [m->device newTextureWithDescriptor:desc];
    m->blurIntermediate = [m->device newTextureWithDescriptor:desc];
    if (!m->blurSnapshot || !m->blurIntermediate) return false;
    /* BGRA8, two of them, no mips: 8 bytes a pixel (34 MiB per window at
     * 2560x1664), charged to MEM_RENDER_GPU. */
    usize px = 0;
    if (checked_mul_usize((usize)w, (usize)h, &px) &&
        px <= (usize)-1 / 8u) {
        m->blurAccounted = px * 8u;
        mem_budget_adjust(MEM_RENDER_GPU, (isize)m->blurAccounted);
    }
    return true;
}

void renderer_blur_capture(Renderer *r) {
    RendererMetal *m = r->backend;
    if (!m || !m->currentDrawable || !m->currentEncoder
        || !m->currentCommandBuffer) {
        return;
    }
    /* framebufferOnly drawables cannot be blit sources. The layer runs with
     * framebufferOnly=YES for lossless drawable compression (see
     * platform_macos.m); if blur panels are ever wired up again, flip that
     * flag. Until then bail before touching the encoder — blurValid stays
     * false, so renderer_draw_blur_panel falls back to a flat tinted rrect.
     * Checked BEFORE the lazy pipeline build so the dead path never pays for
     * a compile it cannot use. */
    if (m->layer && m->layer.framebufferOnly) return;
    if (!ensure_blur_pipeline(m)) return;
    /* Flush anything still in the batches so the snapshot reflects current scene. */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);

    NSUInteger w = m->currentDrawable.texture.width;
    NSUInteger h = m->currentDrawable.texture.height;
    if (!metal_ensure_blur_textures(m, w, h)) return;
    m->blurLastUseSec = platform_time_sec();

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
    g_liu_draw_calls++; g_liu_draw_instances += r->rrect_batch_count;

    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder || !m->rrectPipeline) {
        r->rrect_batch_count = 0;
        return;
    }

    @autoreleasepool {
        id<MTLRenderPipelineState> pipe =
            (r->rrect_keep_dst_alpha && m->rrectPipelineKeepAlpha)
                ? m->rrectPipelineKeepAlpha : m->rrectPipeline;
        [m->currentEncoder setRenderPipelineState:pipe];

        MetalUniforms uniforms = metal_build_uniforms(r,
            r->font.loaded ? r->font.cell_width  : 0.0f,
            r->font.loaded ? r->font.cell_height : 0.0f);

        usize instance_bytes = r->rrect_batch_count * sizeof(RRectInstance);

        [m->currentEncoder setVertexBytes:s_quad
                                   length:sizeof(s_quad)
                                  atIndex:0];
        bool vb_ok = metal_set_vertex_data(m, m->currentEncoder, r->rrect_batch, instance_bytes, 1);
        [m->currentEncoder setVertexBytes:&uniforms
                                   length:sizeof(uniforms)
                                  atIndex:2];

        /* An allocation failure leaves the encoder holding the previous
         * batch's vertex buffer; drawing this batch's instance count
         * against it is worse than dropping the batch. */
        if (vb_ok) {
            [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                  vertexStart:0
                                  vertexCount:6
                                instanceCount:r->rrect_batch_count];
        }
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

    /* Large UI text (markdown headings) draws each glyph well above the atlas
     * cell size. Ask for a raster made at that size rather than magnifying the
     * terminal-sized one — the magnification is what reads as blur. */
    FontGlyphRect rect;
    bool got = font_get_glyph_rect_for_batch(&r->font, codepoint,
        r->ui_cell_w > r->font.cell_width * 1.12f ? (i32)roundf(r->ui_cell_w) : 0, &rect);
    if (!got) return;

    GlyphInstance *gi = &r->glyph_instances[r->glyph_count++];
    gi->x = floorf(x);
    /* UI text: centre the (aspect-corrected) glyph box on the layout box the
     * caller reserved — see renderer_ui_glyph_dy. No-op for terminal cells. */
    gi->y = floorf(y + renderer_ui_glyph_dy(r));
    /* Texel rect; the vertex shader divides by the bound texture's size, so
     * the same instance is right whichever atlas generation the frame binds. */
    bool is_color = (rect.kind == FONT_GLYPH_COLOR);
    gi->tex_kind = rect.kind==FONT_GLYPH_SEED ? GLYPH_TEX_SEED : (is_color ? GLYPH_TEX_COLOR : GLYPH_TEX_ALPHA);
    gi->ax0 = rect.x0;
    gi->ay0 = rect.y0;
    gi->ax1 = rect.x1;
    gi->ay1 = rect.y1;
    /* Colours stay sRGB — the vertex shader decodes. See the GlyphInstance
     * comment in renderer.h for why glyphs alone are decoded there. */
    glyph_set_fg(gi, fg, fg.a);
    /* The colour branch of the shader MULTIPLIES the texel by fg, which is how
     * a white premultiplied mask gets tinted at draw time. An emoji is already
     * full colour, so it must pass white or it takes on the text colour — a
     * flag inside a yellow syntax-highlighted string came out yellow. */
    if (is_color && utf8_is_emoji(codepoint)) {
        gi->fg[0] = gi->fg[1] = gi->fg[2] = 255;
    }
    /* This entry point is the UI-chrome / free-text path: there is no cell
     * behind the glyph. It still sits on the window's surface, and handing the
     * shader that as the background is what puts this text on the same weight
     * as the terminal's — a glyph with none skipped the blend correction and
     * kept the stem-darkening gamma, drawing ~1.5x heavier. See
     * glyph_assumed_bg (renderer.h). Colour glyphs ignore the field. */
    glyph_set_chrome_bg(gi, r, fg);
    /* Color emoji are rasterized into a 2-cell-wide bitmap (see rasterize_glyph
     * in font.c: ecw = cw * 2). The quad has to match or the GPU samples a wide
     * texture into a single-cell quad and the emoji ends up horizontally
     * squashed. The same applies to CJK and other double-width glyphs, which
     * raster two cells wide too. */
    gi->w_cells = (is_color ? utf8_char_width(codepoint) >= 2
                            : font_glyph_is_wide(codepoint)) ? 2 : 1;
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

/* UI strings can discover sized glyphs after the initial frame preparation.
 * End the preceding pass, upload the whole pending batch, then resume without
 * clearing. Earlier draws keep their bound textures; no CPU/GPU wait is used. */
static bool metal_prepare_text_batch(Renderer *r, RendererMetal *m) {
    if (!font_atlas_upload_pending(&r->font)) return true;
    if (!m->currentEncoder || !m->currentCommandBuffer || !m->currentDrawable) return false;
    @autoreleasepool {
    [m->currentEncoder endEncoding];
    [m->currentEncoder release];m->currentEncoder=nil;
    (void)metal_atlas_prepare(m,r,m->currentCommandBuffer);
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=m->currentDrawable.texture;
    pass.colorAttachments[0].loadAction=MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    m->currentEncoder=[[m->currentCommandBuffer renderCommandEncoderWithDescriptor:pass] retain];
    if (!m->currentEncoder) return false;
    [m->currentEncoder setViewport:(MTLViewport){0,0,r->screen_width,r->screen_height,0,1}];
    if (r->scissor_active) {
        [m->currentEncoder setScissorRect:(MTLScissorRect){
            (NSUInteger)r->scissor_x,(NSUInteger)r->scissor_y,
            (NSUInteger)r->scissor_w,(NSUInteger)r->scissor_h}];
    }
    return !font_atlas_upload_pending(&r->font);
    }
}

void renderer_flush_glyphs(Renderer *r) {
    /* Layering invariant: glyphs always draw on top of rects/rrects. Many
     * call sites (renderer_set_ui_scale at heading boundaries, modal closers)
     * call flush_glyphs while regular rects are still pending in the batch.
     * Without draining rects FIRST those pending rects would flush in
     * end_frame *after* this glyph batch hit the encoder — and since the
     * Metal command encoder preserves submission order, the rects would then
     * paint over the glyphs just drawn (a code-block body would turn
     * invisible when a heading follows the block). Flushing rects + rrects
     * here keeps the rect → rrect → glyph order regardless of how the caller
     * sequences the individual push calls. */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);

    if (r->glyph_count == 0) return;
    g_liu_draw_calls++; g_liu_draw_instances += r->glyph_count;

    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder || !m->atlasBound[0]) {
        r->glyph_count = 0;
        return;
    }

    if (!metal_prepare_text_batch(r,m)) {
        r->frame_deferred=true;
        r->glyph_count=0;
        return;
    }
    @autoreleasepool {
        [m->currentEncoder setRenderPipelineState:m->glyphPipeline];

        /* Glyph quad size. For UI chrome text the layout assumes a fixed 1:2
         * cell (8x16pt), but a glyph is rasterized to fill the font's real
         * atlas cell — which is taller than 2:1 for some fonts (JetBrains Mono
         * is 20x45). Mapping that cell onto a 1:2 quad compresses it
         * vertically (squashed). Anchor the glyph height to the requested width
         * times the atlas aspect ratio so UI text scales uniformly. The layout
         * keeps using its own ui_cell_h for spacing — only the drawn glyph is
         * corrected. For a 1:2 font this is identical to ui_cell_h (no change).
         * Terminal text (ui_cell_w == 0) uses the atlas cell verbatim. */
        f32 gcw, gch;
        if (r->ui_cell_w > 0.0f) {
            gcw = r->ui_cell_w;
            /* Rounded to whole pixels: the aspect correction lands on a
             * fraction (16 x 35.368 for JetBrains Mono at 8pt/2x), and while
             * the glyph ORIGIN is floor'd, a fractional quad HEIGHT still puts
             * the bottom edge on a half pixel — the texel grid no longer lines
             * up and the bilinear sampler smears every horizontal stem. */
            gch = (r->font.cell_width > 0.0f)
                ? roundf(gcw * (r->font.cell_height / r->font.cell_width))
                : ((r->ui_cell_h > 0.0f) ? r->ui_cell_h : gcw * 2.0f);
        } else {
            gcw = r->font.cell_width;
            gch = r->font.cell_height;
        }
        MetalUniforms uniforms = metal_build_uniforms(r, gcw, gch);

        usize instance_bytes = r->glyph_count * sizeof(GlyphInstance);

        [m->currentEncoder setVertexBytes:s_glyph_legacy?s_quad:s_glyph_quad
                                   length:s_glyph_legacy?sizeof(s_quad):sizeof(s_glyph_quad)
                                  atIndex:0];

        bool vb_ok;
        if (r->retained_draw_buffer) {
            [m->currentEncoder setVertexBuffer:(id<MTLBuffer>)r->retained_draw_buffer offset:0 atIndex:1];
            vb_ok = true;
        } else vb_ok = metal_set_vertex_data(m, m->currentEncoder, r->glyph_instances, instance_bytes, 1);

        [m->currentEncoder setVertexBytes:&uniforms
                                   length:sizeof(uniforms)
                                  atIndex:2];

        /* The frame's atlas generation — bound at the last preparation, never
         * read off r->font here: font.c may already hold a bigger texture
         * that this frame's instances were not laid out against. The vertex
         * stage needs the same textures to normalise the texel rects. */
        id<MTLTexture> atex = m->atlasBound[0];
        id<MTLTexture> ctex = m->atlasBound[1] ? m->atlasBound[1] : atex;
        [m->currentEncoder setFragmentTexture:atex atIndex:0];
        [m->currentEncoder setFragmentTexture:ctex atIndex:1];
        [m->currentEncoder setVertexTexture:atex atIndex:0];
        [m->currentEncoder setVertexTexture:ctex atIndex:1];
        id<MTLTexture> seed = r->font.metal_seed_texture ? (id<MTLTexture>)r->font.metal_seed_texture : atex;
        [m->currentEncoder setFragmentTexture:seed atIndex:2];
        [m->currentEncoder setVertexTexture:seed atIndex:2];
        /* `ui_cell_w > 0` is the UI-scale override, i.e. "this batch is chrome
         * or magnified text"; 0 is terminal text drawn at the atlas cell size.
         * That is exactly the split the two samplers exist for. */
        id<MTLSamplerState> gsamp = (r->ui_cell_w > 0.0f || !metal_glyph_nearest())
                                        ? m->sampler : m->samplerNearest;
        [m->currentEncoder setFragmentSamplerState:gsamp atIndex:0];
        /* Index 1 is the COLOUR atlas's own sampler and is always the bilinear
         * one — an emoji is a photographic bitmap, not a coverage mask, and the
         * OpenGL backend gives this atlas a LINEAR texture filter too; sharing
         * sampler 0 would point-sample a terminal emoji. Bound unconditionally:
         * the shader declares the argument, so leaving it unset would sample
         * through whatever the encoder last had at that index. */
        [m->currentEncoder setFragmentSamplerState:m->sampler atIndex:1];

        /* An allocation failure leaves the encoder holding the previous
         * batch's vertex buffer; drawing this batch's instance count
         * against it is worse than dropping the batch. */
        if (vb_ok) {
            [m->currentEncoder drawPrimitives:s_glyph_legacy?MTLPrimitiveTypeTriangle:MTLPrimitiveTypeTriangleStrip
                                  vertexStart:0
                                  vertexCount:s_glyph_legacy?6:4
                                instanceCount:r->glyph_count];
        }
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

static void metal_retained_drop_slot(RetainedBatch *b, RetainedSlot *slot) {
    if (slot->buffer) {
        b->owner->stats.live_bytes-=slot->capacity;
        mem_budget_transient_adjust(MEM_RENDER_GPU,-(isize)slot->capacity);
        [(id<MTLBuffer>)slot->buffer release];
    }
    retained_rows_free(slot);
    memset(slot,0,sizeof(*slot));
}

static void metal_retained_destroy(RetainedBatch *b) {
    for (u32 i=0;i<3;i++) metal_retained_drop_slot(b,&b->slots[i]);
}

static void metal_retained_trim(Renderer *r, f64 now) {
    RendererMetal *m=r->backend;
    RetainedStore *store=r->retained_store;
    if (!store || !m->epoch) return;
    [m->epoch->lock lock];
    u64 completed=m->epoch->ok+m->epoch->failed;
    [m->epoch->lock unlock];
    for (RetainedBatch *b=store->head;b;b=b->next) {
        if (!retained_trim_due(b,now)) continue;
        for (u32 i=0;i<3;i++) {
            RetainedSlot *slot=&b->slots[i];
            if (retained_slot_oversized(b,slot) && slot->last_use_serial<=completed)
                metal_retained_drop_slot(b,slot);
        }
        if (!retained_oversized(b)) b->oversized_since=0;
    }
}

void renderer_retained_stats(const Renderer *r, RendererRetainedStats *out) {
    if (!out) return;
    const RetainedStore *s=r ? r->retained_store : NULL;
    *out=s?s->stats:(RendererRetainedStats){0};
}

void renderer_set_draw_origin(Renderer *r, f32 x, f32 y) {
    if (r->draw_origin_x==x && r->draw_origin_y==y) return;
    renderer_flush_glyphs(r); renderer_flush_lines(r);
    r->draw_origin_x=x; r->draw_origin_y=y;
}

bool renderer_draw_retained_rows(Renderer *r, RendererRetainedBatch **handle,
                                 u64 revision, const RendererRows *rows) {
    RendererMetal *m=r?r->backend:NULL;
    if (!m || !m->currentEncoder || !m->currentCommandBuffer || !handle || !rows) return false;
    RetainedBatch *b=retained_get(r,handle,rows->glyphs);
    if (!b) return false;
    RetainedSlot *slot=&b->slots[b->current];
    u64 completed=0, serial=1;
    if (m->epoch) {
        [m->epoch->lock lock];
        completed=m->epoch->ok+m->epoch->failed;
        serial=m->epoch->submitted+1;
        [m->epoch->lock unlock];
    }
    if (!slot->valid || slot->revision!=revision) {
        u32 count; usize bytes;
        if (!retained_size(rows,&count,&bytes)) return false;
        bool writable=false;
        for (u32 i=0;i<3;i++) {
            if (!b->slots[i].buffer || (m->epoch && b->slots[i].last_use_serial<=completed)) {
                b->current=i; writable=true; break;
            }
        }
        slot=&b->slots[b->current];
        if (bytes && !writable) return false; /* caller uses the bounded frame ring */
        bool metadata=retained_rows_prepare(slot,rows);
        bool full=!slot->valid || !metadata || !slot->rows_valid;
        if (bytes && slot->capacity<bytes) {
            usize capacity=4096;
            while (capacity<bytes) {
                if (capacity>SIZE_MAX/2) { capacity=bytes; break; }
                capacity*=2;
            }
            id<MTLBuffer> next=[m->device newBufferWithLength:capacity options:MTLResourceStorageModeShared];
            if (!next) return false;
            /* Only a completed slot is replaced; busy revisions use the
             * caller's frame-ring fallback rather than growing this cache. */
            if (slot->buffer) {
                [(id<MTLBuffer>)slot->buffer release];
                mem_budget_transient_adjust(MEM_RENDER_GPU,-(isize)slot->capacity);
                b->owner->stats.live_bytes-=slot->capacity;
            }
            slot->buffer=next; slot->capacity=capacity;
            slot->valid=false;full=true;
            mem_budget_transient_adjust(MEM_RENDER_GPU,(isize)capacity);
            b->owner->stats.live_bytes+=capacity;
        }
        usize offset=0, copied=0, stride=rows->glyphs?sizeof(GlyphInstance):sizeof(RectInstance);
        u8 *dst=bytes?[(id<MTLBuffer>)slot->buffer contents]:NULL;
        if (bytes && !dst) return false;
        for (i32 row=0;row<rows->rows;row++) {
            usize n=(usize)rows->counts[row]*stride;
            const RetainedRow *old=metadata?&slot->rows[row]:NULL;
            if (n && (full || old->version!=rows->versions[row] ||
                      old->offset!=offset || old->count!=rows->counts[row])) {
                memcpy(dst+offset,retained_row(rows,row),n);
                copied+=n;
            }
            offset+=n;
        }
        /* Publish after every source row has been copied. Empty rows and
         * shifted packed offsets participate even when they write no bytes. */
        if (metadata) {
            offset=0;
            for (i32 row=0;row<rows->rows;row++) {
                slot->rows[row]=(RetainedRow){rows->versions[row],offset,rows->counts[row]};
                offset+=(usize)rows->counts[row]*stride;
            }
        }
        slot->rows_valid=metadata;
        slot->revision=revision; slot->valid=true; slot->count=count;
        b->api.count=count;
        retained_note_size(b,bytes);
        b->owner->stats.uploads++; b->owner->stats.upload_bytes+=copied;
#ifdef LIU_RENDER_REPLAY_PROBE
        r->replay_renderer_to_backend_bytes+=copied;
#endif
    } else b->owner->stats.reuses++;
    if (!slot->count) return true;
    renderer_flush_glyphs(r); renderer_flush_lines(r);
    slot->last_use_serial=serial;
    r->retained_draw_buffer=slot->buffer;
    if (rows->glyphs) { r->glyph_count=slot->count; renderer_flush_glyphs(r); }
    else { r->rect_batch_count=slot->count; renderer_flush_rects(r); }
    r->retained_draw_buffer=NULL;
    return true;
}

void renderer_push_scissor(Renderer *r, f32 x, f32 y, f32 w, f32 h) {
    if (!r) return;
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
    i32 sw = r->screen_width > 0 ? r->screen_width : 0;
    i32 sh = r->screen_height > 0 ? r->screen_height : 0;
    f64 x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    if (isfinite(x) && isfinite(y) && isfinite(w) && isfinite(h) &&
        w > 0.0f && h > 0.0f) {
        x0 = fmax((f64)x, 0.0); y0 = fmax((f64)y, 0.0);
        x1 = fmin((f64)x + (f64)w, (f64)sw);
        y1 = fmin((f64)y + (f64)h, (f64)sh);
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
    }
    NSUInteger ix0 = (NSUInteger)fmin(floor(x0), (f64)sw);
    NSUInteger iy0 = (NSUInteger)fmin(floor(y0), (f64)sh);
    NSUInteger ix1 = (NSUInteger)fmin(ceil(x1),  (f64)sw);
    NSUInteger iy1 = (NSUInteger)fmin(ceil(y1),  (f64)sh);
    r->scissor_active = true;
    r->scissor_x = (f32)ix0; r->scissor_y = (f32)iy0;
    r->scissor_w = (f32)(ix1 - ix0); r->scissor_h = (f32)(iy1 - iy0);
    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder) return;
    /* Metal scissor: top-left origin, matches the renderer's coordinate
     * system */
    MTLScissorRect scissor = {
        .x      = ix0,
        .y      = iy0,
        .width  = ix1 - ix0,
        .height = iy1 - iy0,
    };
    [m->currentEncoder setScissorRect:scissor];
}

void renderer_pop_scissor(Renderer *r) {
    if (!r) return;
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
    r->scissor_active = false;
    RendererMetal *m = r->backend;
    if (!m || !m->currentEncoder) return;
    MTLScissorRect full = {
        .x = 0, .y = 0,
        .width  = (NSUInteger)(r->screen_width > 0 ? r->screen_width : 0),
        .height = (NSUInteger)(r->screen_height > 0 ? r->screen_height : 0),
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
    if (!m) return;
    /* The initial binding, before any frame: font.c seeded the texture
     * synchronously. Later textures arrive through renderer_atlas_texture_
     * replaced and are bound at a frame boundary. Retained: a frame in flight
     * samples whatever was bound when it was encoded, and font.c drops its
     * own references the moment it grows. */
    id<MTLTexture> want[2] = {
        (__bridge id<MTLTexture>)texture,
        (__bridge id<MTLTexture>)r->font.color_texture,
    };
    /* The font's own textures are the truth at this boundary (a fresh
     * atlas, or a replacement whose predecessor the caller already drained
     * the GPU for). Anything still pending was recorded for the atlas that
     * is gone: a replacement in it would bind a texture that atlas no longer
     * owns, and its uploads have nowhere to be seen. */
    metal_atlas_drop_pending(m);
    for (i32 k = 0; k < 2; k++) {
        if (m->atlasBound[k] != want[k]) {
            id<MTLTexture> old = m->atlasBound[k];
            m->atlasBound[k] = want[k] ? [want[k] retain] : nil;
            if (old) [old release];
        }
    }
    /* From here on every texel this atlas writes goes through the queue, and
     * a glyph is drawable only once the frame's preparation has carried it. */
    r->font.gpu_uploader = m;
    r->font.gpu_gated = true;
    font_atlas_set_gpu_ready(&r->font, font_atlas_content_version(&r->font));
}

/* The immediate helpers (renderer_metal_upload_texture and friends) and the
 * upload accounting live in renderer_metal_sync.m; this file shares their
 * bounds check, the render-pass clear and the immediate queue through
 * renderer_metal_sync.h. */
#define metal_upload_validate  renderer_metal_upload_validate
#define metal_texture_clear_on renderer_metal_texture_clear_on
#define metal_sync_queue       renderer_metal_sync_queue

/* ---- the queue ---------------------------------------------------------- */

#define METAL_STAGING_ALIGN   64u       /* sourceOffset: a multiple of the texel size is required; 64 covers every format here */
#define METAL_STAGING_PITCH   16u       /* sourceBytesPerRow alignment */
#define METAL_STAGING_MIN     (256u * 1024u)
#define METAL_STAGING_KEEP    (8u * 1024u * 1024u)   /* a pooled buffer bigger than this is dropped */


/* Grow the pending staging buffer to hold `need` more bytes. Allocate the
 * bigger buffer FIRST, copy what is staged, then let the old one go (to the
 * pool, where a later frame can use it). A failure leaves everything as it
 * was and the record is refused — never a half-staged write. */
static bool metal_staging_reserve(RendererMetal *m, usize need) {
    usize want;
    if (!checked_add_usize(m->pendingUsed, need, &want)) return false;
    m->stagingLastUseSec = platform_time_sec();
    if (m->pendingStaging && want <= m->pendingStaging.length) return true;
    usize cap = m->pendingStaging ? m->pendingStaging.length : 0;
    usize ncap = cap ? cap : METAL_STAGING_MIN;
    while (ncap < want) {
        if (ncap > SIZE_MAX / 2) { ncap = want; break; }
        ncap *= 2;
    }
    id<MTLBuffer> nb = nil;
    /* Prefer a pooled buffer that fits — a finished frame's staging, already
     * paid for. */
    [m->atlasLock lock];
    for (u32 i = 0; i < m->stagingPoolCount; i++) {
        if (m->stagingPool[i].length >= want) {
            nb = m->stagingPool[i];
            m->stagingPool[i] = m->stagingPool[--m->stagingPoolCount];
            break;
        }
    }
    [m->atlasLock unlock];
    if (!nb && !LIU_METAL_FAULT(LIU_METAL_FAULT_ATLAS_STAGING)) {
        nb = [m->device newBufferWithLength:ncap options:MTLResourceStorageModeShared];
        if (nb) mem_budget_transient_adjust(MEM_RENDER_GPU, (isize)nb.length);
    }
    if (!nb) return false;
    if (m->pendingStaging) {
        if (m->pendingUsed) memcpy(nb.contents, m->pendingStaging.contents, m->pendingUsed);
        metal_atlas_buffer_release(m->pendingStaging);
    }
    m->pendingStaging = nb;
    if (nb.length > m->atlasStats.staging_high_water)
        m->atlasStats.staging_high_water = nb.length;
    return true;
}

static bool metal_pending_reserve_op(RendererMetal *m) {
    if (m->pendingCount < m->pendingCap) return true;
    if (LIU_METAL_FAULT(LIU_METAL_FAULT_ATLAS_OPS)) return false;
    u32 ncap = m->pendingCap ? m->pendingCap * 2 : 64;
    MetalAtlasOp *nr = realloc(m->pendingOps, (usize)ncap * sizeof *nr);
    if (!nr) return false;
    m->pendingOps = nr;
    m->pendingCap = ncap;
    return true;
}

static void metal_atlas_op_release(MetalAtlasOp *o) {
    if (o->tex) [o->tex release];
    if (o->old) [o->old release];
    if (o->new) [o->new release];
    o->tex = o->old = o->new = nil;
}

bool renderer_atlas_upload(void *uploader, void *texture, i32 x, i32 y, i32 w, i32 h,
                           const void *data, usize src_bytes, u32 content_version) {
    if (!texture || !data) return false;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    usize row_bytes = 0, need = 0;
    NSUInteger bpp = 0;
    if (!metal_upload_validate(tex, x, y, w, h, src_bytes, &row_bytes, &need, &bpp)) return false;
    if (!uploader) {
        renderer_metal_upload_texture(texture, x, y, w, h, data, src_bytes);
        return true;
    }
    RendererMetal *m = (RendererMetal *)uploader;
    struct timespec _u0;
    clock_gettime(CLOCK_MONOTONIC, &_u0);

    /* Coalesce with the previous record when this one continues the same
     * shelf row to the right — the shape every glyph burst has. The two
     * blocks are re-laid as one at the end of the staging buffer (the old
     * bytes become dead space, bounded by the bytes themselves); one blit
     * then carries the whole run. Never across a replacement: the log's
     * order is what carries a write into the right texture. */
    /* The packer leaves a 2-texel gutter between glyphs (ax = pack_x + 1,
     * pack_x += cw + 2), so "adjacent" means within that gutter; the gap is
     * staged as zeroes, which is what the gutter holds by construction.
     *
     * A merged record's staged block keeps a row stride (`pitch`) wider than
     * its content, doubled each time it runs out, so a shelf row that fills
     * one glyph at a time costs O(n) staging bytes and O(n) copies; laying
     * every merge out afresh at the end of the buffer would make both
     * quadratic. */
    MetalAtlasOp *prev = m->pendingCount ? &m->pendingOps[m->pendingCount - 1] : NULL;
    u32 gap = 0;
    bool merge = prev && prev->op == METAL_ATLAS_OP_UPLOAD && prev->tex == tex &&
                 prev->y == (u32)y && prev->h == (u32)h && prev->bpp == bpp &&
                 (u32)x >= prev->x + prev->w && (u32)x - (prev->x + prev->w) <= 2u &&
                 (usize)(x + w) - (usize)prev->x <= 4096u;
    if (merge) gap = (u32)x - (prev->x + prev->w);
    if (merge && ((usize)(x + w) - (usize)prev->x) * bpp <= prev->pitch) {
        /* Fits in the stride already staged: append in place. */
        u8 *blk = (u8 *)m->pendingStaging.contents + prev->offset;
        usize old_row = (usize)prev->w * bpp, gap_bytes = (usize)gap * bpp;
        const u8 *src = (const u8 *)data;
        for (i32 r = 0; r < h; r++) {
            u8 *row = blk + (usize)r * prev->pitch + old_row;
            if (gap_bytes) memset(row, 0, gap_bytes);
            memcpy(row + gap_bytes, src + (usize)r * row_bytes, row_bytes);
        }
        prev->w = (u32)((usize)(x + w) - (usize)prev->x);
        if (content_version > prev->version) prev->version = content_version;
        m->atlasStats.coalesced++;
        m->atlasStats.records++;
        m->atlasStats.bytes += need;
        atomic_fetch_add(&g_liu_tex_uploads, 1ull);
        atomic_fetch_add(&g_liu_tex_bytes, (unsigned long long)need);
        return true;
    }
    usize total_w = merge ? (usize)(x + w) - (usize)prev->x : (usize)w;
    /* Stride: the content, or double the previous stride when re-laying a
     * merge, capped at the widest row an atlas can have. */
    usize pitch = metal_align_up(total_w * bpp, METAL_STAGING_PITCH);
    if (merge) {
        usize grown = prev->pitch * 2;
        usize cap = metal_align_up(4096u * bpp, METAL_STAGING_PITCH);
        if (grown > cap) grown = cap;
        if (grown > pitch) pitch = grown;
    }
    usize block = 0;
    if (!checked_mul_usize(pitch, (usize)h, &block)) return false;
    usize offset = metal_align_up(m->pendingUsed, METAL_STAGING_ALIGN);
    /* A merge rewrites `prev` in place and appends NO op, so the op log must
     * not be grown here: metal_pending_reserve_op() reallocs m->pendingOps at
     * capacity and `prev` — a raw pointer into that array — would dangle for
     * every write below. Only the non-merge branch appends. */
    if (!merge && !metal_pending_reserve_op(m)) return false;
    if (!metal_staging_reserve(m, (offset - m->pendingUsed) + block)) return false;
    u8 *dst = (u8 *)m->pendingStaging.contents + offset;
    const u8 *src = (const u8 *)data;
    if (merge) {
        const u8 *old = (const u8 *)m->pendingStaging.contents + prev->offset;
        usize old_row = (usize)prev->w * bpp;
        usize gap_bytes = (usize)gap * bpp;
        for (i32 r = 0; r < h; r++) {
            u8 *row = dst + (usize)r * pitch;
            memcpy(row, old + (usize)r * prev->pitch, old_row);
            if (gap_bytes) memset(row + old_row, 0, gap_bytes);
            memcpy(row + old_row + gap_bytes, src + (usize)r * row_bytes, row_bytes);
        }
        prev->w = (u32)total_w;
        prev->offset = offset;
        prev->pitch = pitch;
        if (content_version > prev->version) prev->version = content_version;
        m->atlasStats.coalesced++;
    } else {
        for (i32 r = 0; r < h; r++)
            memcpy(dst + (usize)r * pitch, src + (usize)r * row_bytes, row_bytes);
        MetalAtlasOp *rec = &m->pendingOps[m->pendingCount++];
        memset(rec, 0, sizeof *rec);
        rec->op = METAL_ATLAS_OP_UPLOAD;
        rec->tex = [tex retain];
        rec->x = (u32)x; rec->y = (u32)y; rec->w = (u32)w; rec->h = (u32)h;
        rec->bpp = (u32)bpp;
        rec->offset = offset;
        rec->pitch = pitch;
        rec->version = content_version;
    }
    m->pendingUsed = offset + block;
    m->atlasStats.records++;
    m->atlasStats.bytes += need;
    {
        struct timespec u1;
        clock_gettime(CLOCK_MONOTONIC, &u1);
        u64 dt = (u64)(u1.tv_sec - _u0.tv_sec) * 1000000000ull
               + (u64)(u1.tv_nsec - _u0.tv_nsec);
        atomic_fetch_add(&g_liu_tex_uploads, 1ull);
        atomic_fetch_add(&g_liu_tex_bytes, (unsigned long long)need);
        atomic_fetch_add(&g_liu_tex_ns, (unsigned long long)dt);
    }
    return true;
}

#define METAL_ZERO_STRIP_ROWS 64u

bool renderer_atlas_clear(void *uploader, void *texture, i32 x, i32 y, i32 w, i32 h,
                          u32 content_version) {
    if (!texture) return false;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    usize row_bytes = 0, need = 0;
    NSUInteger bpp = 0;
    /* The source bound is the region itself: nothing is read from a caller. */
    if (!metal_upload_validate(tex, x, y, w, h, SIZE_MAX, &row_bytes, &need, &bpp)) return false;
    if (!uploader) {
        /* Headless / pre-frame: stage the zeroes and blit, waited on. */
        u8 *zeros = calloc(1, need);
        if (!zeros) return false;
        renderer_metal_upload_texture(texture, x, y, w, h, zeros, need);
        free(zeros);
        return true;
    }
    RendererMetal *m = (RendererMetal *)uploader;
    usize strip = (usize)tex.width * bpp * METAL_ZERO_STRIP_ROWS;
    if (!m->zeroBuffer || m->zeroBuffer.length < strip) {
        id<MTLBuffer> nb = [m->device newBufferWithLength:strip options:MTLResourceStorageModeShared];
        if (!nb) return false;
        mem_budget_transient_adjust(MEM_RENDER_GPU, (isize)nb.length);
        memset(nb.contents, 0, strip);
        if (m->zeroBuffer) metal_atlas_buffer_release(m->zeroBuffer);
        m->zeroBuffer = nb;
    }
    if (!metal_pending_reserve_op(m)) return false;
    MetalAtlasOp *rec = &m->pendingOps[m->pendingCount++];
    memset(rec, 0, sizeof *rec);
    rec->op = METAL_ATLAS_OP_CLEAR;
    rec->tex = [tex retain];
    rec->x = (u32)x; rec->y = (u32)y; rec->w = (u32)w; rec->h = (u32)h;
    rec->bpp = (u32)bpp;
    rec->pitch = (usize)w * bpp;
    rec->version = content_version;
    m->atlasStats.records++;
    return true;
}

void renderer_atlas_texture_replaced(void *uploader, i32 kind, void *old_tex,
                                     void *new_tex, i32 copy_w, i32 copy_h) {
    RendererMetal *m = (RendererMetal *)uploader;
    if (!m || kind < 0 || kind > 1) return;
    id<MTLTexture> ot = (__bridge id<MTLTexture>)old_tex;
    id<MTLTexture> nt = (__bridge id<MTLTexture>)new_tex;
    if (!metal_pending_reserve_op(m)) {
        /* Without the record the next preparation would keep sampling a
         * texture font.c no longer packs into. Refusing is not an option, so
         * the bounded thing is to bind now — the block copy is lost, which
         * costs a re-raster of what was on screen, not a wrong picture:
         * font.c has invalidated every rect that named the old texture.
         * Frames in flight retain what they sample. */
        id<MTLTexture> prev = m->atlasBound[kind];
        m->atlasBound[kind] = nt ? [nt retain] : nil;
        if (prev) [prev release];
        m->atlasStats.prepare_failures++;
        return;
    }
    MetalAtlasOp *rec = &m->pendingOps[m->pendingCount++];
    memset(rec, 0, sizeof *rec);
    rec->op = METAL_ATLAS_OP_REPLACE;
    rec->kind = (u8)kind;
    rec->old = ot ? [ot retain] : nil;
    rec->new = nt ? [nt retain] : nil;
    rec->copy_w = (copy_w > 0 && ot && nt) ? (u32)copy_w : 0;
    rec->copy_h = (copy_h > 0 && ot && nt) ? (u32)copy_h : 0;
    if (rec->copy_w && (rec->copy_w > ot.width || rec->copy_h > ot.height ||
                        rec->copy_w > nt.width || rec->copy_h > nt.height))
        rec->copy_w = rec->copy_h = 0;
}

static void metal_atlas_batch_free(MetalAtlasBatch *b) {
    if (!b) return;
    for (u32 i = 0; i < b->count; i++) metal_atlas_op_release(&b->ops[i]);
    free(b->ops);
    if (b->staging) metal_atlas_buffer_release(b->staging);
    free(b);
}

/* Encode one batch's log onto `cb`, in order. Uploads share one blit
 * encoder for as long as the run lasts; a replacement ends it (its clear is
 * a render pass), copies the kept block, and the next upload opens a fresh
 * one. Returns false if an encoder could not be made — the ops before that
 * point ARE on the buffer and will run; the caller re-encodes the whole
 * batch next time, which is safe because every op is idempotent. */
static bool metal_atlas_encode_batch(RendererMetal *m, id<MTLCommandBuffer> cb,
                                     MetalAtlasBatch *b) {
    id<MTLBlitCommandEncoder> blit = nil;
    for (u32 i = 0; i < b->count; i++) {
        MetalAtlasOp *o = &b->ops[i];
        if (o->op == METAL_ATLAS_OP_REPLACE) {
            if (blit) { [blit endEncoding]; blit = nil; }
            if (!o->new) continue;
            metal_texture_clear_on(cb, o->new);
            if (o->copy_w && o->old) {
                blit = LIU_METAL_FAULT(LIU_METAL_FAULT_ATLAS_BLIT) ? nil : [cb blitCommandEncoder];
                if (!blit) return false;
                [blit copyFromTexture:o->old sourceSlice:0 sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(o->copy_w, o->copy_h, 1)
                            toTexture:o->new destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding]; blit = nil;
                m->atlasStats.blits++;
            }
            continue;
        }
        if (!blit) {
            blit = LIU_METAL_FAULT(LIU_METAL_FAULT_ATLAS_BLIT) ? nil : [cb blitCommandEncoder];
            if (!blit) return false;
            m->atlasStats.blits++;
        }
        if (o->op == METAL_ATLAS_OP_CLEAR) {
            /* Strips of zeroes out of the shared zero buffer; the last strip
             * is shorter. `m->zeroBuffer` may have been re-sized since this
             * op was recorded, only ever upwards, so it still covers a strip. */
            usize row_bytes = (usize)o->w * o->bpp;
            for (u32 y0 = 0; y0 < o->h; y0 += METAL_ZERO_STRIP_ROWS) {
                u32 rows = o->h - y0 < METAL_ZERO_STRIP_ROWS ? o->h - y0 : METAL_ZERO_STRIP_ROWS;
                [blit copyFromBuffer:m->zeroBuffer sourceOffset:0
                   sourceBytesPerRow:row_bytes sourceBytesPerImage:row_bytes * rows
                          sourceSize:MTLSizeMake(o->w, rows, 1)
                           toTexture:o->tex destinationSlice:0 destinationLevel:0
                   destinationOrigin:MTLOriginMake(o->x, o->y + y0, 0)];
            }
            continue;
        }
        [blit copyFromBuffer:b->staging sourceOffset:o->offset
           sourceBytesPerRow:o->pitch sourceBytesPerImage:o->pitch * o->h
                  sourceSize:MTLSizeMake(o->w, o->h, 1)
                   toTexture:o->tex destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(o->x, o->y, 0)];
    }
    if (blit) [blit endEncoding];
    return true;
}

/* The completion every prepared batch gets: on success its staging buffer
 * goes back to the pool and its retained textures are let go (the old
 * generations retire here — nothing on the GPU reads them any more); on an
 * Error status the batch is handed back whole to be encoded at the next
 * preparation, staging bytes and all. Runs on Metal's completion thread;
 * everything it touches is under atlasLock. `m` outlives every handler by
 * the teardown drain in renderer_destroy. */
static void metal_atlas_attach_completion(RendererMetal *m, id<MTLCommandBuffer> cb,
                                          MetalAtlasBatch *b) {
    /* Decided at attach time and carried into the block, like the frame
     * retry's injection: the outcome rides the real completion at the real
     * time. */
    const bool inject_error = LIU_METAL_FAULT(LIU_METAL_FAULT_ATLAS_ERROR);
    atomic_fetch_add_explicit(&m->atlasHandlersLive, 1u, memory_order_acq_rel);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        [m->atlasLock lock];
        bool failed = inject_error || done.status == MTLCommandBufferStatusError;
        if (failed && m->requeueCount < METAL_ATLAS_REQUEUE_CAP) {
            m->requeue[m->requeueCount++] = b;
        } else {
            if (failed) m->atlasStats.dropped++;
            if (b->staging && m->stagingPoolCount < 4 && b->staging.length <= METAL_STAGING_KEEP) {
                m->stagingPool[m->stagingPoolCount++] = b->staging;
                b->staging = nil;
            }
            for (u32 i = 0; i < b->count; i++)
                if (b->ops[i].op == METAL_ATLAS_OP_REPLACE && b->ops[i].old)
                    m->atlasStats.retired++;
            metal_atlas_batch_free(b);
        }
        [m->atlasLock unlock];
        atomic_fetch_sub_explicit(&m->atlasHandlersLive, 1u, memory_order_acq_rel);
    }];
}

/* The frame boundary. Called with the frame's command buffer open and NO
 * encoder yet: takes the batches a failed frame handed back, then everything
 * recorded since the last preparation, encodes them in that order, binds the
 * replacements, and attaches the completion that retires or requeues. The
 * ready version published to font.c is the atlas content version at this
 * moment — the log holds exactly what was written since the last
 * preparation, so once it is encoded, all of it is on its way. Returns
 * whether anything was encoded onto `cb` — a caller that then cannot open
 * its render encoder must still COMMIT a buffer that carries atlas work. */
static bool metal_atlas_prepare(RendererMetal *m, Renderer *r, id<MTLCommandBuffer> cb) {
    if (!m || !cb) return false;
    bool encoded = false;

    /* 1. Batches a failed frame handed back, in their original order. */
    MetalAtlasBatch *redo[METAL_ATLAS_REQUEUE_CAP];
    u32 nredo = 0;
    [m->atlasLock lock];
    for (u32 i = 0; i < m->requeueCount; i++) redo[nredo++] = m->requeue[i];
    m->requeueCount = 0;
    [m->atlasLock unlock];
    for (u32 i = 0; i < nredo; i++) {
        if (!metal_atlas_encode_batch(m, cb, redo[i])) {
            /* Could not encode: put it back, and everything after it. */
            [m->atlasLock lock];
            for (u32 j = i; j < nredo; j++) m->requeue[m->requeueCount++] = redo[j];
            [m->atlasLock unlock];
            m->atlasStats.prepare_failures++;
            return encoded;
        }
        encoded = true;
        m->atlasStats.requeued++;
        metal_atlas_attach_completion(m, cb, redo[i]);
    }

    /* 2. The pending log, as one batch. */
    u32 content = font_atlas_content_version(&r->font);
    if (m->pendingCount == 0) {
        font_atlas_set_gpu_ready(&r->font, content);
        return encoded;
    }
    MetalAtlasBatch *b = calloc(1, sizeof *b);
    if (!b) { m->atlasStats.prepare_failures++; return encoded; }
    b->staging = m->pendingStaging;          /* ownership moves to the batch */
    b->ops = m->pendingOps;
    b->count = m->pendingCount;
    b->ready_version = content;
    if (!metal_atlas_encode_batch(m, cb, b)) {
        /* An encoder could not be made. Hand everything back to the pending
         * state, ready version untouched: what was encoded before the
         * failure runs (and runs again next time, idempotently). */
        free(b);
        m->atlasStats.prepare_failures++;
        return true;   /* part of the log is on the buffer */
    }
    m->pendingStaging = nil;
    m->pendingUsed = 0;
    m->pendingOps = NULL;
    m->pendingCount = m->pendingCap = 0;
    /* Bind the last replacement of each kind for this and later frames; the
     * batch keeps every old texture alive until this frame completes. */
    for (u32 i = 0; i < b->count; i++) {
        MetalAtlasOp *o = &b->ops[i];
        if (o->op != METAL_ATLAS_OP_REPLACE) continue;
        id<MTLTexture> prev_bound = m->atlasBound[o->kind];
        m->atlasBound[o->kind] = o->new ? [o->new retain] : nil;
        if (prev_bound) [prev_bound release];
    }
    font_atlas_set_gpu_ready(&r->font, content);
    metal_atlas_attach_completion(m, cb, b);
    return true;
}

/* Drop the pending log without encoding it: the atlas it was recorded for
 * is gone (a font change replaced it wholesale, seeded synchronously). The
 * textures it names are retained by the ops, so releasing them here is the
 * whole cleanup; frames in flight retain what they sample. */
static void metal_atlas_drop_pending(RendererMetal *m) {
    for (u32 i = 0; i < m->pendingCount; i++) metal_atlas_op_release(&m->pendingOps[i]);
    m->pendingCount = 0;
    m->pendingUsed = 0;
}

static void metal_atlas_destroy(RendererMetal *m) {
    /* The teardown drain has completed every frame; a handler can still be
     * on its way out on the completion thread, so wait for the last one to
     * return (bounded — a handler that never returns is a driver fault, and
     * leaking `m` is the lesser harm). Then whatever is here is ours. */
    for (i32 spins = 0; spins < 20000 &&
                        atomic_load_explicit(&m->atlasHandlersLive, memory_order_acquire) != 0; spins++) {
        struct timespec ts = { 0, 100 * 1000 };
        nanosleep(&ts, NULL);
    }
    [m->atlasLock lock];
    for (u32 i = 0; i < m->requeueCount; i++) metal_atlas_batch_free(m->requeue[i]);
    m->requeueCount = 0;
    for (u32 i = 0; i < m->stagingPoolCount; i++) metal_atlas_buffer_release(m->stagingPool[i]);
    m->stagingPoolCount = 0;
    [m->atlasLock unlock];
    metal_atlas_drop_pending(m);
    free(m->pendingOps);
    m->pendingOps = NULL; m->pendingCap = 0;
    if (m->pendingStaging) { metal_atlas_buffer_release(m->pendingStaging); m->pendingStaging = nil; }
    for (i32 k = 0; k < 2; k++) {
        if (m->atlasBound[k]) { [m->atlasBound[k] release]; m->atlasBound[k] = nil; }
    }
    if (m->zeroBuffer) { metal_atlas_buffer_release(m->zeroBuffer); m->zeroBuffer = nil; }
    [m->atlasLock release];
    m->atlasLock = nil;
}

void renderer_atlas_stats(const Renderer *r, RendererAtlasStats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    const RendererMetal *m = r ? r->backend : NULL;
    if (m) *out = m->atlasStats;
}

/* True once a stamp handed out by renderer_take_presentation() was "the frame
 * became the layer's contents" rather than a hardware presentation time. */
bool renderer_presentation_is_layer_time(Renderer *r) {
    RendererMetal *m=r?r->backend:NULL;
    if (!m || !m->acquire) return false;
    LiuDrawableMailbox *b=m->acquire;
    [b->lock lock];
    bool v=b->layer_stamps;
    [b->lock unlock];
    return v;
}

bool renderer_take_presentation(Renderer *r, f64 *through, f64 *when) {
    RendererMetal *m=r?r->backend:NULL;
    if (!m || !m->acquire) return false;
    LiuDrawableMailbox *b=m->acquire;
    [b->lock lock];
    bool got=false;
    if (b->lost) { *through=0; *when=-1; b->lost=0; got=true; }
    else if (b->consumed<b->submitted && b->observations[b->consumed%64].done) {
        *through=b->observations[b->consumed%64].through;
        *when=b->observations[b->consumed%64].when;
        b->consumed++; got=true;
    }
    [b->lock unlock]; return got;
}

/* Wait for everything already committed on this renderer's queue.
 *
 * Same trick teardown uses: the queue is serial, so an empty command buffer
 * committed now completes only after every buffer committed before it. An
 * UNCOMMITTED currentCommandBuffer is deliberately not waited on — it would
 * never complete, and it has not sampled anything yet anyway.
 *
 * The caller is font.c's shelf reclaim, which is about to overwrite atlas cells
 * earlier frames drew from — and it reaches this only as the bounded fallback
 * behind renderer_gpu_mark(), because this call blocks the UI thread for as long
 * as the queue is busy. */
bool renderer_gpu_drain(Renderer *r) {
    RendererMetal *m = r ? (RendererMetal *)r->backend : NULL;
    if (!m || !m->commandQueue) return false;
    if (m->encodeQueue) dispatch_sync(m->encodeQueue,^{});
    bool ok = false;
    @autoreleasepool {
        id<MTLCommandBuffer> drain = [m->commandQueue commandBuffer];
        /* `nil` here is not "nothing to wait for", it is "the wait did not
         * happen". Reporting it as success would let the shelf reclaim reuse
         * cells earlier frames are still sampling. */
        if (!drain) return false;
        [drain commit];
        [drain waitUntilCompleted];
        /* An empty buffer that finished in ERROR says the queue was reset
         * underneath this wait, so the ordering argument it rests on ("a serial
         * queue completes an empty buffer only after everything committed
         * before it") no longer holds. Refuse rather than guess. */
        ok = (drain.status == MTLCommandBufferStatusCompleted);
    }
    return ok;
}

bool renderer_drawable_ready(Renderer *r) {
    RendererMetal *m = r ? r->backend : NULL;
    if (m && m->surfacePresenter && !atomic_load(&m->surfacePresenter->stopped)) {
        for (u32 i=0;i<3;i++) {
            LiuSurfaceImage *slot=m->surfacePresenter->images[i];
            if (!slot || (slot!=m->surfacePresenter->shown && !atomic_load(&slot->leased) && !IOSurfaceIsInUse(slot->surface))) return true;
        }
        return false;
    }
    if (!m || !m->acquire) return false;
    LiuDrawableMailbox *box = m->acquire;
    [box->lock lock];
    bool ready = !box->stopped && box->ready != nil;
    [box->lock unlock];
    return ready;
}

bool renderer_gpu_idle(Renderer *r) {
    RendererMetal *m = r ? (RendererMetal *)r->backend : NULL;
    if (!m || !m->epoch) return true;      /* no GPU: nothing can be reading */
    /* An open encoder is a reader too, and a caller that has one has not kept its
     * side of the contract — answer no rather than let it act on this. */
    if (m->currentCommandBuffer || m->currentEncoder) return false;
    /* One consistent snapshot of all three, so a submission counted after the
     * completions were read cannot produce a false idle. */
    [m->epoch->lock lock];
    bool idle = (m->epoch->ok + m->epoch->failed) >= m->epoch->submitted;
    [m->epoch->lock unlock];
    return idle;
}

void renderer_gpu_frame_counts(Renderer *r, u64 *out_ok, u64 *out_failed,
                               u32 *out_fail_run) {
    RendererMetal *m = r ? (RendererMetal *)r->backend : NULL;
    u64 ok = 0, failed = 0;
    u32 run = 0;
    if (m && m->epoch) {
        /* All three together. `fail_run` only means anything alongside the `failed`
         * it belongs to — reading them separately could pair a stale run with a
         * fresh failure. */
        [m->epoch->lock lock];
        failed = m->epoch->failed;
        ok     = m->epoch->ok;
        run    = m->epoch->failRun;
        [m->epoch->lock unlock];
    }
    if (out_ok)       *out_ok = ok;
    if (out_failed)   *out_failed = failed;
    if (out_fail_run) *out_fail_run = run;
}

void renderer_metal_upload_stats(u64 *out_uploads, u64 *out_bytes, u64 *out_ns) {
    if (out_uploads) *out_uploads = atomic_load(&g_liu_tex_uploads);
    if (out_bytes)   *out_bytes   = atomic_load(&g_liu_tex_bytes);
    if (out_ns)      *out_ns      = atomic_load(&g_liu_tex_ns);
}

/* =========================================================================
 * Inline image rendering
 * ========================================================================= */

static void metal_image_cache_release_entry(RendererMetal *m, MetalImageCacheEntry *e) {
    if (!e || !e->texture) return;
    usize resident = e->bytes * (usize)(1u + e->spare_count);
    if (m->imageCacheBytes >= resident) m->imageCacheBytes -= resident;
    else m->imageCacheBytes = 0;
    metal_image_account(m);
    [e->texture release];
    for (u8 i = 0; i < e->spare_count; i++) [e->spare[i] release];
    memset(e, 0, sizeof(*e));
}

/* --- the oversize LRU ----------------------------------------------------
 * Same entry type and the same LRU rule as the pool next door, with its own
 * byte total. Kept as three small functions rather than folded into the pool's
 * because the two differ in exactly one place that matters: this one admits a
 * single image LARGER than its whole budget (by emptying itself first), which
 * the pool must never do. */
static void metal_oversize_release(RendererMetal *m, MetalImageCacheEntry *e) {
    if (!e || !e->texture) return;
    /* Subtract exactly what admission ADDED — e->bytes, once. The pool's
     * release multiplies by the spare ring because the pool fills one; this
     * tier never does (a generation bump here swaps in a fresh texture and
     * drops the old reference immediately), so spare_count is always 0. The
     * loop below is kept anyway: a release that assumed the count and a
     * subtraction that assumed the count would then be wrong in the SAME
     * direction, and the ledger would not notice. */
    if (m->oversizeBytes >= e->bytes) m->oversizeBytes -= e->bytes;
    else m->oversizeBytes = 0;
    [e->texture release];
    for (u8 i = 0; i < e->spare_count; i++) [e->spare[i] release];
    memset(e, 0, sizeof(*e));
    metal_image_account(m);
}

static MetalImageCacheEntry *metal_oversize_find(RendererMetal *m, const void *key,
                                                 i32 w, i32 h) {
    for (u32 i = 0; i < METAL_IMAGE_OVERSIZE_SLOTS; i++) {
        MetalImageCacheEntry *e = &m->oversize[i];
        if (e->texture && e->key == key && e->width == w && e->height == h)
            return e;
    }
    return NULL;
}

/* The least recently drawn OCCUPIED entry, or NULL when the tier is empty.
 *
 * Separate from the placement helper below, and the separation is load-bearing:
 * an eviction loop that asks "where would a new entry go" gets an EMPTY slot
 * first, releases nothing, and either spins forever or — with a `break` on the
 * empty answer — silently stops enforcing the budget and installs over it. With
 * four slots and a 256 MiB per-image ceiling that is 1 GiB of resident texture.
 * Eviction asks for something to FREE; placement asks for somewhere to PUT. */
static MetalImageCacheEntry *metal_oversize_lru(RendererMetal *m) {
    MetalImageCacheEntry *oldest = NULL;
    for (u32 i = 0; i < METAL_IMAGE_OVERSIZE_SLOTS; i++) {
        MetalImageCacheEntry *e = &m->oversize[i];
        if (!e->texture) continue;
        if (!oldest || e->last_used_frame < oldest->last_used_frame) oldest = e;
    }
    return oldest;
}

/* An empty slot if there is one, else the least recently drawn. Never NULL:
 * METAL_IMAGE_OVERSIZE_SLOTS is a compile-time positive. */
static MetalImageCacheEntry *metal_oversize_victim(RendererMetal *m) {
    MetalImageCacheEntry *empty = NULL, *oldest = NULL;
    for (u32 i = 0; i < METAL_IMAGE_OVERSIZE_SLOTS; i++) {
        MetalImageCacheEntry *e = &m->oversize[i];
        if (!e->texture) { if (!empty) empty = e; continue; }
        if (!oldest || e->last_used_frame < oldest->last_used_frame) oldest = e;
    }
    return empty ? empty : oldest;
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

/* Allocate + upload one RGBA image texture. The caller owns the +1 reference.
 * A whole-image CPU write is safe only after its last GPU reader completes.
 * When no completed cached storage is available, allocate an immutable new
 * version; commands retain the outgoing object until their reads retire. */
/* `oversize` only classifies the sample — the work is identical either way.
 * The timer covers exactly the texture create and the pixel write, so
 * image_upload_cpu_ns cannot absorb encoder setup or draw time. */
static id<MTLTexture> metal_new_image_texture(RendererMetal *m, const u8 *pixels,
                                              i32 img_w, i32 img_h, bool oversize) {
    f64 t0 = platform_time_sec();
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
           bytesPerRow:(NSUInteger)img_w * 4u];
    liu_image_stat_upload(oversize, /*is_alloc*/true,
                          (usize)img_w * (usize)img_h * 4u,
                          (u64)((platform_time_sec() - t0) * 1e9));
    return tex;
}

static id<MTLTexture> metal_image_texture(RendererMetal *m, const void *key,
                                          u64 generation, const u8 *pixels,
                                          i32 img_w, i32 img_h) {
    if (!m || !key || !pixels || img_w <= 0 || img_h <= 0 ||
        img_w > METAL_MAX_TEXTURE_DIM || img_h > METAL_MAX_TEXTURE_DIM) return nil;
    u64 completed=0,serial=1;
    if(m->epoch) {
        [m->epoch->lock lock];
        completed=m->epoch->ok+m->epoch->failed;
        serial=m->epoch->submitted+1;
        [m->epoch->lock unlock];
    }
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
                e->texture_serial=serial;
                e->last_used_frame = m->frameSerial;
                e->last_used_sec = m->frameNowSec;
                hit = e->texture;
            }
            /* Duplicate same-generation entries shouldn't exist, but if a
             * race produced one, evict the duplicate. */
            else metal_image_cache_release_entry(m, e);
        } else if (!reusable && e->width == img_w && e->height == img_h) {
            /* Same key + same dimensions but a stale generation. This is the
             * animated-GIF hot path: every frame bumps `generation` so the
             * key-gen lookup misses. The slot keeps its identity and its byte
             * accounting (dimensions are unchanged) — only the texture behind
             * it is replaced, see the swap below. */
            reusable = e;
        } else {
            /* Older / mismatched dimensions for the same key — caller has
             * moved on; this entry will never be reused. */
            metal_image_cache_release_entry(m, e);
        }
    }
    if (hit) { g_liu_image_stats.hits++; return hit; }
    if (reusable) {
        /* Same key and geometry, newer contents: an animation frame. */
        g_liu_image_stats.restage_count++;
        id<MTLTexture> fresh = nil;
        i32 available=-1;
        if(m->epoch && reusable->texture_serial<=completed)fresh=reusable->texture;
        if(!fresh) {
            for(u8 i=0;i<reusable->spare_count;i++)
                if(m->epoch && reusable->spare_serial[i]<=completed) { available=i;break; }
            if(available>=0)fresh=reusable->spare[available];
        }
        if(fresh) {
            MTLRegion region = MTLRegionMake2D(0, 0, (NSUInteger)img_w,
                                               (NSUInteger)img_h);
            f64 up0 = platform_time_sec();
            [fresh replaceRegion:region mipmapLevel:0 withBytes:pixels
                     bytesPerRow:(NSUInteger)img_w * 4u];
            liu_image_stat_upload(false, /*is_alloc*/false,
                                  (usize)img_w * (usize)img_h * 4u,
                                  (u64)((platform_time_sec() - up0) * 1e9));
            if(available>=0) {
                reusable->spare[available] = reusable->texture;
                reusable->spare_serial[available] = reusable->texture_serial;
            }
            reusable->texture = fresh;
        } else {
            /* No completed storage: never overwrite an earlier draw, including
             * another generation emitted into this same open command buffer. */
            fresh = metal_new_image_texture(m, pixels, img_w, img_h, false);
            if (!fresh) {
                reusable->last_used_frame = m->frameSerial;
                reusable->last_used_sec = m->frameNowSec;
                reusable->texture_serial = serial;
                return reusable->texture;
            }
            if(reusable->spare_count<METAL_FRAME_BUFFER_COUNT-1 &&
               m->imageCacheBytes<=METAL_IMAGE_CACHE_BUDGET-reusable->bytes) {
                u8 index=reusable->spare_count++;
                reusable->spare[index]=reusable->texture;
                reusable->spare_serial[index]=reusable->texture_serial;
                m->imageCacheBytes+=reusable->bytes;
                metal_image_account(m);
            } else [reusable->texture release];
            reusable->texture = fresh;
        }
        reusable->generation     = generation;
        reusable->texture_serial = serial;
        reusable->last_used_frame = m->frameSerial;
        reusable->last_used_sec   = m->frameNowSec;
        return fresh;
    }

    /* Metal textures here are mipmapped:NO, so the base level IS the retained
     * size — unlike the OpenGL backend, which generates a full chain. */
    usize bytes;
    if (!img_rgba_bytes_checked(img_w, img_h, &bytes) ||
        bytes > METAL_IMAGE_MAX_BYTES) return nil;
    /* A single texture larger than 1/4 of the budget would, on its own, force
     * every other entry out — a 4K RGBA frame (64 MiB) thrashes the LRU pool to
     * nothing. Such textures get the dedicated `oversize` slot instead, which
     * lives outside imageCacheBytes and therefore cannot evict anything.
     *
     * The slot keeps its texture while the image is unchanged: the app redraws
     * on a cursor blink, on hover animation, on any agent output, and a
     * full-screen photo re-uploaded on each of those frames would cost
     * megabytes per second of PCIe traffic for a picture that never changed. */
    if (bytes > METAL_IMAGE_OVERSIZE_BYTES) {
        MetalImageCacheEntry *o = metal_oversize_find(m, key, img_w, img_h);
        if (o) {
            if (o->generation != generation) {
                /* Same logical image, new contents (animation frame). Swap in a
                 * fresh texture — the slot may still be bound by in-flight
                 * frames, so restaging the bytes in place would race the GPU. */
                id<MTLTexture> fresh = metal_new_image_texture(m, pixels, img_w, img_h, true);
                if (fresh) {
                    [o->texture release];
                    o->texture    = fresh;
                    o->generation = generation;
                }
                /* Allocation failed: keep the previous frame's texture and
                 * leave the generation stale so the next frame retries. */
            }
            o->last_used_frame = m->frameSerial;
            o->last_used_sec = m->frameNowSec;
            o->texture_serial = serial;
            if (o->generation == generation) g_liu_image_stats.hits++;
            else                             g_liu_image_stats.restage_count++;
            return o->texture;
        }

        /* Not resident. Make room by BYTES, oldest first. The loop stops when
         * the newcomer fits or the pool is empty — so an image larger than the
         * whole budget still draws, alone, exactly as the single slot let it. */
        while (m->oversizeBytes + bytes > METAL_IMAGE_OVERSIZE_BUDGET) {
            MetalImageCacheEntry *victim = metal_oversize_lru(m);
            if (!victim) break;   /* nothing left to free — the image is alone */
            /* An entry losing its place is what the ping-pong counter has
             * always meant: with one slot every miss forced one of these. */
            g_liu_image_stats.oversize_replace_count++;
            metal_oversize_release(m, victim);
        }

        MetalImageCacheEntry *slot = metal_oversize_victim(m);
        if (!slot) return nil;
        if (slot->texture) {
            g_liu_image_stats.oversize_replace_count++;
            metal_oversize_release(m, slot);
        }

        id<MTLTexture> tex = metal_new_image_texture(m, pixels, img_w, img_h, true);
        if (!tex) return nil;

        /* MRC: metal_new_image_texture hands back a +1 owned object and the slot
         * now holds that reference. It is released when the slot is replaced,
         * when the idle sweep reclaims it, or at renderer teardown. */
        *slot = (MetalImageCacheEntry){
            .key = key,
            .generation = generation,
            .width = img_w,
            .height = img_h,
            .bytes = bytes,
            .last_used_frame = m->frameSerial,
            .last_used_sec = m->frameNowSec,
            .texture = tex,
            .texture_serial = serial,
        };
        m->oversizeBytes += bytes;
        metal_image_account(m);
        return tex;
    }

    while (m->imageCacheBytes > METAL_IMAGE_CACHE_BUDGET - bytes) {
        MetalImageCacheEntry *victim = metal_image_cache_victim(m);
        if (!victim || !victim->texture) break;
        g_liu_image_stats.evict_count++;
        metal_image_cache_release_entry(m, victim);
    }

    MetalImageCacheEntry *slot = metal_image_cache_victim(m);
    if (!slot) return nil;
    metal_image_cache_release_entry(m, slot);

    id<MTLTexture> tex = metal_new_image_texture(m, pixels, img_w, img_h, false);
    if (!tex) return nil;

    *slot = (MetalImageCacheEntry){
        .key = key,
        .generation = generation,
        .width = img_w,
        .height = img_h,
        .bytes = bytes,
        .last_used_frame = m->frameSerial,
        .last_used_sec = m->frameNowSec,
        .texture = tex,
        .texture_serial = serial,
    };
    m->imageCacheBytes += bytes;
    metal_image_account(m);
    return tex;
}

void renderer_draw_image_tinted(Renderer *r, const void *cache_key, u64 cache_generation,
                                const u8 *pixels, i32 img_w, i32 img_h,
                                f32 x, f32 y, f32 draw_w, f32 draw_h,
                                Color tint) {
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
         * alpha blending: one GlyphInstance covering the whole image. This is
         * the IMAGE kind — its rect is NORMALISED (0..1), not texels, because
         * an image texture is not an atlas and the shader must not divide by
         * an atlas size it was never laid out against. The fragment stage
         * samples it through the colour slot, tinted by fg (white = as-is). */
        GlyphInstance gi;
        gi.x = floorf(x);
        gi.y = floorf(y);
        gi.ax0 = 0.0f;
        gi.ay0 = 0.0f;
        gi.ax1 = 1.0f;
        gi.ay1 = 1.0f;
        glyph_set_fg(&gi, tint, tint.a);
        glyph_set_no_bg(&gi);
        gi.tex_kind = GLYPH_TEX_IMAGE;
        gi.w_cells = 1;
        gi._pad[0] = gi._pad[1] = 0;

        [m->currentEncoder setRenderPipelineState:m->glyphPipeline];

        /* Build uniforms with draw_w/draw_h as cell size so the unit quad
         * scales to the desired image dimensions */
        MetalUniforms uniforms;
        memcpy(uniforms.projection, r->projection, sizeof(uniforms.projection));
        uniforms.cell_w = draw_w;
        uniforms.cell_h = draw_h;
        /* Colour-image path: the fragment shader takes the is_color branch and
         * never touches the gamma, but keep it defined. */
        uniforms.text_gamma = 1.0f;
        /* The image carries no background, so neither correction can fire —
         * but the struct must be fully initialised or the shader reads stack
         * garbage as a contrast ratio. */
        uniforms.min_contrast = 0.0f;
        uniforms.blend_correct = 0.0f;
        uniforms.text_weight = 0.0f;   /* an image is not text */
        uniforms._pad[0] = uniforms._pad[1] = 0.0f;

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
         * also as the atlas (index 0) since the shader samples from it. The
         * vertex stage takes the same two: the IMAGE kind never reads their
         * size, but a stage's texture argument must be bound regardless. */
        [m->currentEncoder setFragmentTexture:tex atIndex:0];
        [m->currentEncoder setFragmentTexture:tex atIndex:1];
        [m->currentEncoder setVertexTexture:tex atIndex:0];
        [m->currentEncoder setVertexTexture:tex atIndex:1];
        [m->currentEncoder setFragmentTexture:tex atIndex:2];
        [m->currentEncoder setVertexTexture:tex atIndex:2];
        [m->currentEncoder setFragmentSamplerState:m->sampler atIndex:0];
        /* The is_color branch samples through sampler 1 (see the colour-atlas
         * note in renderer_flush_glyphs), and this path takes that branch — so it
         * has to bind one, or an image would be sampled through whatever the
         * encoder last left there. Bilinear, which is what an image wants and
         * what this path already asked for at index 0. */
        [m->currentEncoder setFragmentSamplerState:m->sampler atIndex:1];

        [m->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                              vertexStart:0
                              vertexCount:6
                            instanceCount:1];
    }
}

void renderer_draw_image_cached(Renderer *r, const void *cache_key, u64 cache_generation,
                                const u8 *pixels, i32 img_w, i32 img_h,
                                f32 x, f32 y, f32 draw_w, f32 draw_h) {
    renderer_draw_image_tinted(r, cache_key, cache_generation, pixels,
                               img_w, img_h, x, y, draw_w, draw_h,
                               (Color){1.0f, 1.0f, 1.0f, 1.0f});
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
 * at startup because most configurations set no background image, and the
 * pipeline descriptor + shader compilation together cost ~1 MB of Metal
 * state that would otherwise go unused. */
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

/* Process-wide wallpaper share table. Bounded by the number of renderers that
 * can hold a reference at once (LIU_MAX_WINDOWS is 8) — an entry exists only
 * while refcount > 0, so the table can never hold more live entries than there
 * are windows. Sized past that so a full table stays a theoretical branch.
 *
 * Not locked: every entry point that touches it (config apply, renderer
 * teardown) runs on the main thread, like every other renderer call. */
#define METAL_BG_IMAGE_CACHE_CAP 16
static MetalBgImageEntry s_bg_image_cache[METAL_BG_IMAGE_CACHE_CAP];

/* Build the share key. Returns false when the file cannot be stat'd or the path
 * does not fit the key verbatim — both mean "do not share": a truncated path
 * would alias two different files onto one texture. */
static bool metal_bg_image_key_from_stamp(const char *path, id<MTLDevice> dev,
                                          const FileStamp *source, MetalBgImageKey *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));   /* never leave the key half-built */
    if (!path || !source) return false;
    usize len = strlen(path);
    if (len == 0 || len >= sizeof(out->path)) return false;

    memcpy(out->path, path, len + 1);
    out->source     = *source;
    out->device     = dev;
    return true;
}

static bool metal_bg_image_key_eq(const MetalBgImageKey *a, const MetalBgImageKey *b) {
    return a->device == b->device &&
           file_stamp_equal(&a->source, &b->source) &&
           strcmp(a->path, b->path) == 0;
}

static MetalBgImageEntry *metal_bg_image_find(const MetalBgImageKey *key) {
    for (u32 i = 0; i < METAL_BG_IMAGE_CACHE_CAP; i++) {
        MetalBgImageEntry *e = &s_bg_image_cache[i];
        if (!e->in_use || !e->texture) continue;
        if (metal_bg_image_key_eq(&e->key, key)) return e;
    }
    return NULL;
}

static MetalBgImageEntry *metal_bg_image_alloc(void) {
    for (u32 i = 0; i < METAL_BG_IMAGE_CACHE_CAP; i++) {
        if (!s_bg_image_cache[i].in_use) return &s_bg_image_cache[i];
    }
    return NULL;   /* caller falls back to a private, unshared texture */
}

static void metal_bg_image_unref(RendererMetal *m) {
    if (!m) return;
    MetalBgImageEntry *e   = m->bgImageEntry;
    id<MTLTexture>     own = m->bgImageTexture;
    m->bgImageEntry   = NULL;
    m->bgImageTexture = nil;
    if (!e) {
        /* Unshared fallback texture: this renderer owned the only reference. */
        if (own) [own release];
        if (m->bgPrivateAccounted) {
            mem_budget_adjust(MEM_IMAGE_GPU, -(isize)m->bgPrivateAccounted);
            m->bgPrivateAccounted = 0;
        }
        return;
    }
    if (e->refcount > 0) e->refcount--;
    if (e->refcount == 0) {
        if (e->texture) [e->texture release];
        if (e->bytes) mem_budget_adjust(MEM_IMAGE_GPU, -(isize)e->bytes);
        memset(e, 0, sizeof(*e));   /* clears in_use — slot is free again */
    }
}

bool renderer_load_background_rgba_versioned(Renderer *r, const char *path,
                                             const u8 *pixels, i32 w, i32 h,
                                             void *gpu_device, const FileStamp *source) {
    RendererMetal *m = r ? r->backend : NULL;
    if (!m || !path || !path[0] || !pixels || !gpu_device ||
        w <= 0 || h <= 0 || w > METAL_MAX_TEXTURE_DIM || h > METAL_MAX_TEXTURE_DIM)
        return false;
    if (source && !file_stamp_matches(path, source)) return false;
    usize bytes = 0;
    if (!img_rgba_bytes_checked(w, h, &bytes) || bytes > METAL_BG_IMAGE_MAX_BYTES)
        return false;
    if (!ensure_bg_image_pipeline(m)) return false;

    id<MTLDevice> dev = (__bridge id<MTLDevice>)gpu_device;
    MetalBgImageKey key;
    bool shareable = source && metal_bg_image_key_from_stamp(path, dev, source, &key);
    if (shareable) {
        MetalBgImageEntry *hit = metal_bg_image_find(&key);
        if (hit) {
            hit->refcount++;
            renderer_destroy_background_image(r);
            m->bgImageEntry = hit;
            m->bgImageTexture = hit->texture;
            r->bg_texture = (__bridge void *)hit->texture;
            r->bg_image_w = hit->width;
            r->bg_image_h = hit->height;
            return true;
        }
    }

    @autoreleasepool {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
            width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:desc];
        if (!tex) return false;
        [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
               mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)w * 4u];

        /* Publish before dropping the old image so reloading a shared key
         * cannot momentarily take its refcount to zero. */
        MetalBgImageEntry *slot = shareable ? metal_bg_image_alloc() : NULL;
        if (slot) {
            memset(slot, 0, sizeof(*slot));
            slot->key = key; slot->texture = tex;
            slot->width = w; slot->height = h;
            slot->bytes = bytes;
            slot->refcount = 1; slot->in_use = true;
            mem_budget_adjust(MEM_IMAGE_GPU, (isize)bytes);
        }
        renderer_destroy_background_image(r);
        m->bgImageEntry = slot;
        m->bgImageTexture = tex;
        if (!slot) {
            m->bgPrivateAccounted = bytes;
            mem_budget_adjust(MEM_IMAGE_GPU, (isize)bytes);
        }
        r->bg_texture = (__bridge void *)tex;
        r->bg_image_w = w;
        r->bg_image_h = h;
    }
    fprintf(stderr, "renderer_metal: uploaded background image %dx%d from %s\n", w, h, path);
    return true;
}

bool renderer_load_background_rgba(Renderer *r, const char *path,
                                   const u8 *pixels, i32 w, i32 h,
                                   void *gpu_device) {
    /* Memory pixels without a captured stamp cannot truthfully claim the
     * current version of a file. The UI decoder uses the versioned entry. */
    return renderer_load_background_rgba_versioned(r,path,pixels,w,h,gpu_device,NULL);
}

bool renderer_load_background_image(Renderer *r, const char *path, void *gpu_device) {
    if (!r || !path || !path[0] || !gpu_device) return false;

    RendererMetal *m = r->backend;
    if (!m) return false;

    /* Ensure the bg-image render pipeline exists before uploading the texture
     * — if compile fails, skip the texture upload so no memory goes to a
     * bitmap that can never be drawn. */
    if (!ensure_bg_image_pipeline(m)) return false;

    id<MTLDevice> dev = (__bridge id<MTLDevice>)gpu_device;
    FileStamp source;
    if (!file_stamp_read(path, &source)) return false;

    /* Another window already has this exact file resident: take a reference and
     * skip the decode + upload entirely. Saves the duplicate texture (31.6 MiB
     * per extra window on a 4K wallpaper) and the decode. */
    MetalBgImageKey key;
    bool shareable = metal_bg_image_key_from_stamp(path, dev, &source, &key);
    if (shareable) {
        MetalBgImageEntry *hit = metal_bg_image_find(&key);
        if (hit) {
            /* Claim BEFORE dropping the old reference: reloading the same
             * wallpaper into the same window would otherwise take the refcount
             * to zero, free the texture, and hand back a dangling entry. */
            hit->refcount++;
            renderer_destroy_background_image(r);
            m->bgImageEntry   = hit;
            m->bgImageTexture = hit->texture;   /* borrowed — entry owns the +1 */
            r->bg_texture = (__bridge void *)hit->texture;
            r->bg_image_w = hit->width;
            r->bg_image_h = hit->height;
            fprintf(stderr, "renderer_metal: loaded background image %dx%d from %s\n",
                    hit->width, hit->height, path);
            return true;
        }
    }

    /* Gate on the header BEFORE decoding. stb_image happily accepts dimensions
     * up to 1<<24 per axis, so without this a 20000x20000 PNG first tries a
     * ~1.6 GB decode allocation and then hands Metal a descriptor above the
     * device's 16384 limit — a texture that can never be created, paid for with
     * the whole decode and a multi-second main-thread stall. The other texture
     * paths gate the same way (metal_image_texture clamps to
     * METAL_MAX_TEXTURE_DIM; the terminal's image protocols call
     * img_dims_ok_mem before stbi_load_from_memory). */
    i32 w = 0, h = 0, channels = 0;
    if (!stbi_info(path, &w, &h, &channels)) {
        fprintf(stderr, "renderer_metal: cannot read background image header: %s\n", path);
        return false;
    }
    usize bg_pixels = 0, bg_bytes = 0;
    if (w <= 0 || h <= 0 || w > METAL_MAX_TEXTURE_DIM || h > METAL_MAX_TEXTURE_DIM ||
        !checked_mul_usize((usize)w, (usize)h, &bg_pixels) ||
        !checked_mul_usize(bg_pixels, 4u, &bg_bytes) ||
        bg_bytes > METAL_BG_IMAGE_MAX_BYTES) {
        fprintf(stderr, "renderer_metal: background image %dx%d rejected (max %d px/axis, %llu MiB)\n",
                w, h, METAL_MAX_TEXTURE_DIM,
                (unsigned long long)(METAL_BG_IMAGE_MAX_BYTES / (1024ull * 1024ull)));
        return false;
    }

    /* Keep the existing image until decode, revision validation and upload
     * have all succeeded. The upload also rechecks the decoded byte ceiling. */
    w = h = channels = 0;
    u8 *pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) return false;
    bool uploaded = renderer_load_background_rgba_versioned(r,path,pixels,w,h,
                                                           gpu_device,&source);
    stbi_image_free(pixels);
    return uploaded;
}

void renderer_destroy_background_image(Renderer *r) {
    RendererMetal *m = r->backend;
    /* Drops THIS renderer's reference. The texture itself only goes away when
     * the last window using that wallpaper lets go of it. */
    if (m) metal_bg_image_unref(m);
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
