/*
 * Liu - Metal-accelerated LLM forward pass.
 *
 * One MTLComputeCommandEncoder per token holds the whole forward pass as
 * a serial chain of dispatches (the default encoder is MTLDispatchTypeSerial,
 * so Metal orders dependent dispatches automatically). Weights live in one
 * device buffer, copied from the GGUF tensor-data blob at create time.
 *
 * Manual retain/release (the project is not built with ARC) — long-lived
 * Metal objects are retained in LlmMetal and released in llm_metal_free.
 */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "llm/llm_metal.h"
#include "llm/llm_metal_params.h"

#include <libgen.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The attention kernel sizes its threadgroup score scratch from
 * LLM_METAL_MAX_CTX; the engine clamps n_ctx to LLM_MODEL_MAX_CTX — if
 * the former ever stopped covering the latter the scratch overflows. */
_Static_assert(LLM_METAL_MAX_CTX >= LLM_MODEL_MAX_CTX,
               "LLM_METAL_MAX_CTX must cover LLM_MODEL_MAX_CTX");

/* Light-mode profiling: LLM_METAL_PROFILE=1 prints the hardware GPU-busy
 * time (cb.GPUEndTime - cb.GPUStartTime) per decode — a true hardware
 * measurement, unaffected by CPU-side timing. */
static int llm_metal_profile(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LLM_METAL_PROFILE");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

/* Monotonic wall-clock seconds, for the prefill/decode throughput split. */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Resolved weight tensor: byte offset into the weights blob + dtype. */
typedef struct {
    u32 w_offset;
    u32 dtype;
    bool present;
} MTensor;

typedef struct {
    MTensor attn_norm, attn_q, attn_k, attn_v, attn_o;
    MTensor attn_q_norm, attn_k_norm;   /* .present == false when absent */
    MTensor ffn_norm, ffn_gate, ffn_up, ffn_down;
} MLayer;

struct LlmMetal {
    LlmHParams h;

    id<MTLDevice>        device;
    id<MTLCommandQueue>  queue;
    id<MTLLibrary>       library;

    id<MTLComputePipelineState> psoRmsnorm;
    id<MTLComputePipelineState> psoMatvecStq, psoMatvecQ6k, psoMatvecF32;
    id<MTLComputePipelineState> psoQkv, psoGateUp;       /* fused mega-matvecs */
    id<MTLComputePipelineState> psoKvPrep, psoAttnBlock;
    id<MTLComputePipelineState> psoEmbed, psoArgmax;     /* GPU embed + argmax */
    id<MTLComputePipelineState> psoLmHeadQ6kBest, psoArgmaxPartials;

    id<MTLBuffer> weights;       /* GGUF tensor-data blob, Private storage */

    MTensor  tok_embd, out_norm, out_proj;
    bool     qk_norm;
    MLayer  *layers;

    /* GPU-only activation + cache buffers — Private storage. The residual
     * add is folded into the matvec (accumulate mode); k_gate_up fuses
     * gate, up and SwiGLU so there is no separate up-projection buffer. */
    id<MTLBuffer> bX, bHb, bQ, bKc, bVc, bAttnOut, bFfnG;
    id<MTLBuffer> bKcache, bVcache, bRope;
    id<MTLBuffer> bBestVals, bBestIdx;
    NSUInteger    bestCount;
    /* CPU-visible buffers — Shared storage. bTokens holds the prompt +
     * generated token ids; bLogits backs the non-Q6_K LM head and stays nil on
     * the Q6_K (top-k) path. */
    id<MTLBuffer> bTokens, bLogits;

    /* Model-constant LlmMetalParams fields, filled once at create time;
     * encode_forward copies this and patches only the per-dispatch fields. */
    LlmMetalParams base;
};

/* ---- tensor resolution --------------------------------------------- */

static MTensor resolve(const GgufFile *g, const char *name, i64 d0, i64 d1) {
    MTensor mt = {0};
    const GgufTensor *t = gguf_tensor(g, name);
    if (!t) return mt;
    if (d0 > 0 && (i64)t->dims[0] != d0) return mt;
    if (d1 > 0 && (i64)t->dims[1] != d1) return mt;
    /* F16 is intentionally rejected: there is no matvec_f16 GPU kernel,
     * and the tied embedding can also be matvec'd as the LM head. A model
     * with any F16 weight fails llm_metal_create and the engine falls
     * back to the CPU path, which does handle F16. */
    if (t->dtype != GGML_TYPE_F32 &&
        t->dtype != GGML_TYPE_Q6_K && t->dtype != GGML_TYPE_STQ1_0) {
        return mt;
    }
    /* w_offset is a u32 the kernels index with; a >4 GiB blob would truncate
     * it and mis-locate the weight. Reject so the engine falls back to the
     * CPU path (which keeps offsets as usize). Returns absent (.present=0). */
    if (t->offset > 0xFFFFFFFFu) return mt;
    mt.w_offset = (u32)t->offset;
    mt.dtype = t->dtype;
    mt.present = true;
    return mt;
}

static MTensor resolve_f32(const GgufFile *g, const char *name, i64 d0) {
    MTensor mt = {0};
    const GgufTensor *t = gguf_tensor(g, name);
    if (!t || t->dtype != GGML_TYPE_F32) return mt;
    if (d0 > 0 && (i64)t->dims[0] != d0) return mt;
    if (t->offset > 0xFFFFFFFFu) return mt;   /* see resolve(): u32 w_offset */
    mt.w_offset = (u32)t->offset;
    mt.dtype = GGML_TYPE_F32;
    mt.present = true;
    return mt;
}

/* ---- metallib + pipeline setup ------------------------------------- */

static id<MTLComputePipelineState> make_pso(id<MTLDevice> dev,
                                            id<MTLLibrary> lib,
                                            const char *fn) {
    id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:fn]];
    if (!f) {
        fprintf(stderr, "llm_metal: missing kernel function '%s'\n", fn);
        return nil;
    }
    NSError *err = nil;
    id<MTLComputePipelineState> pso =
        [dev newComputePipelineStateWithFunction:f error:&err];
    [f release];
    if (!pso) {
        fprintf(stderr, "llm_metal: pipeline '%s' failed: %s\n", fn,
                err ? err.localizedDescription.UTF8String : "(unknown)");
    }
    return pso;
}

/* Load LiuLLM.metallib from the directory holding the running binary. */
static id<MTLLibrary> load_metallib(id<MTLDevice> dev) {
    char exe[4096];
    u32 sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) != 0) return nil;
    char *dir = dirname(exe);
    char path[4200];
    snprintf(path, sizeof path, "%s/LiuLLM.metallib", dir);
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:url error:&err];
    if (!lib) {
        fprintf(stderr, "llm_metal: cannot load %s: %s\n", path,
                err ? err.localizedDescription.UTF8String : "(unknown)");
    }
    return lib;
}

bool llm_metal_available(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        bool ok = (dev != nil);
        [dev release];
        return ok;
    }
}

/* ---- create / free ------------------------------------------------- */

LlmMetal *llm_metal_create(const GgufFile *g) {
    if (!g || !g->map) return NULL;

    LlmMetal *m = (LlmMetal *)calloc(1, sizeof *m);
    if (!m) return NULL;
    if (!llm_hparams_load(g, &m->h)) { free(m); return NULL; }
    const LlmHParams *h = &m->h;
    i32 q_dim = h->n_head * h->head_dim;
    i32 kv_dim = h->n_head_kv * h->head_dim;

    /* The fused mega-matvecs (k_qkv, k_gate_up) stage the whole n_embd
     * input into one MATVEC_TILE of threadgroup memory. A model wider
     * than that needs the non-fused path — fall back to the CPU engine. */
    if (h->n_embd > MATVEC_TILE) { free(m); return NULL; }

    /* The fused attention kernels stage one head into fixed [256]
     * threadgroup arrays; a larger head_dim would write past them and
     * corrupt GPU threadgroup memory. Reject here so a crafted/untrusted
     * GGUF (attention.key_length) cannot reach the kernels — the CPU
     * engine sizes its scratch dynamically and tolerates any head_dim. */
    if (h->head_dim <= 0 || h->head_dim > LLM_METAL_MAX_HEAD_DIM) {
        free(m);
        return NULL;
    }

    @autoreleasepool {
        /* MTLCreateSystemDefaultDevice follows the create rule: the returned
         * device is already owned (+1) by us, and llm_metal_free releases it
         * once. The previous extra retain here over-retained it, leaking one
         * MTLDevice per model load. */
        m->device = MTLCreateSystemDefaultDevice();
        if (!m->device) { llm_metal_free(m); return NULL; }
        m->queue = [m->device newCommandQueue];
        if (!m->queue) { llm_metal_free(m); return NULL; }

        m->library = load_metallib(m->device);
        if (!m->library) { llm_metal_free(m); return NULL; }

        m->psoRmsnorm   = make_pso(m->device, m->library, "k_rmsnorm");
        m->psoMatvecStq = make_pso(m->device, m->library, "k_matvec_stq1_0");
        m->psoMatvecQ6k = make_pso(m->device, m->library, "k_matvec_q6k");
        m->psoMatvecF32 = make_pso(m->device, m->library, "k_matvec_f32");
        m->psoQkv       = make_pso(m->device, m->library, "k_qkv");
        m->psoGateUp    = make_pso(m->device, m->library, "k_gate_up");
        m->psoKvPrep    = make_pso(m->device, m->library, "k_kv_prep");
        m->psoAttnBlock = make_pso(m->device, m->library, "k_attn_block");
        m->psoEmbed     = make_pso(m->device, m->library, "k_embed");
        m->psoArgmax    = make_pso(m->device, m->library, "k_argmax");
        m->psoLmHeadQ6kBest = make_pso(m->device, m->library, "k_lm_head_q6k_best");
        m->psoArgmaxPartials = make_pso(m->device, m->library, "k_argmax_partials");
        if (!m->psoRmsnorm || !m->psoMatvecStq || !m->psoMatvecQ6k ||
            !m->psoMatvecF32 || !m->psoQkv || !m->psoGateUp ||
            !m->psoKvPrep || !m->psoAttnBlock || !m->psoEmbed || !m->psoArgmax ||
            !m->psoLmHeadQ6kBest || !m->psoArgmaxPartials) {
            llm_metal_free(m);
            return NULL;
        }

        /* One device buffer = a copy of the GGUF tensor-data blob. Tensor
         * `t` then lives at `weights + t->offset`. The blob is GPU-only
         * (k_embed reads the embedding table on the GPU, not the CPU), so
         * it goes in Private storage. It is staged in through a small
         * fixed Shared scratch buffer, one chunk at a time — a full-blob
         * Shared staging copy would double peak memory (~2x the model)
         * for the length of the load. */
        u64 blob_size = (u64)g->map_size - (u64)(g->tensor_data - g->map);
        m->weights = [m->device newBufferWithLength:(NSUInteger)blob_size
                                            options:MTLResourceStorageModePrivate];
        if (!m->weights) { llm_metal_free(m); return NULL; }
        {
            const NSUInteger CHUNK = 32u * 1024u * 1024u;
            NSUInteger stage_len = blob_size < CHUNK ? (NSUInteger)blob_size : CHUNK;
            id<MTLBuffer> staging =
                [m->device newBufferWithLength:stage_len
                                       options:MTLResourceStorageModeShared];
            if (!staging) { llm_metal_free(m); return NULL; }
            const char *src = (const char *)g->tensor_data;
            for (u64 off = 0; off < blob_size; off += CHUNK) {
                NSUInteger n = (NSUInteger)(blob_size - off < CHUNK
                                           ? blob_size - off : CHUNK);
                memcpy([staging contents], src + off, n);
                id<MTLCommandBuffer> bcb = [m->queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [bcb blitCommandEncoder];
                [blit copyFromBuffer:staging sourceOffset:0
                            toBuffer:m->weights destinationOffset:(NSUInteger)off
                                size:n];
                [blit endEncoding];
                [bcb commit];
                [bcb waitUntilCompleted];   /* serialize: staging is reused */
            }
            [staging release];

            /* The tensor blob now lives in the Private GPU buffer and the CPU
             * never reads it again (the Metal path keeps tensors on-GPU; only
             * the KV/string region below tensor_data feeds the tokenizer).
             * Drop those clean, file-backed mmap pages so ~blob_size of RSS
             * isn't pinned for the engine's lifetime — they re-fault from disk
             * if ever touched. Round the start UP one page so the metadata
             * page that may share storage with the KV region is left mapped. */
            long pgsz = sysconf(_SC_PAGESIZE);
            if (pgsz > 0) {
                uintptr_t start = ((uintptr_t)g->tensor_data + (uintptr_t)pgsz - 1)
                                  & ~((uintptr_t)pgsz - 1);
                uintptr_t end   = (uintptr_t)g->map + (uintptr_t)g->map_size;
                if (end > start) {
                    (void)madvise((void *)start, (size_t)(end - start), MADV_DONTNEED);
                }
            }
        }

        /* Resolve tensors (same names/shapes the CPU path validates). */
        m->tok_embd = resolve(g, "token_embd.weight", h->n_embd, 0);
        m->out_norm = resolve_f32(g, "output_norm.weight", h->n_embd);
        m->out_proj = resolve(g, "output.weight", h->n_embd, h->n_vocab);
        if (!m->out_proj.present) m->out_proj = m->tok_embd;  /* tied */
        if (!m->tok_embd.present || !m->out_norm.present || !m->out_proj.present) {
            llm_metal_free(m);
            return NULL;
        }

        m->layers = (MLayer *)calloc((usize)h->n_layers, sizeof(MLayer));
        if (!m->layers) { llm_metal_free(m); return NULL; }

        bool qk_seen = false;
        for (i32 l = 0; l < h->n_layers; l++) {
            MLayer *ly = &m->layers[l];
            char nm[80];
            #define LN(s) (snprintf(nm, sizeof nm, "blk.%d.%s", l, s), nm)
            ly->attn_norm = resolve_f32(g, LN("attn_norm.weight"), h->n_embd);
            ly->attn_q    = resolve(g, LN("attn_q.weight"),     h->n_embd, q_dim);
            ly->attn_k    = resolve(g, LN("attn_k.weight"),     h->n_embd, kv_dim);
            ly->attn_v    = resolve(g, LN("attn_v.weight"),     h->n_embd, kv_dim);
            ly->attn_o    = resolve(g, LN("attn_output.weight"), q_dim,    h->n_embd);
            ly->ffn_norm  = resolve_f32(g, LN("ffn_norm.weight"), h->n_embd);
            ly->ffn_gate  = resolve(g, LN("ffn_gate.weight"),   h->n_embd, h->n_ff);
            ly->ffn_up    = resolve(g, LN("ffn_up.weight"),     h->n_embd, h->n_ff);
            ly->ffn_down  = resolve(g, LN("ffn_down.weight"),   h->n_ff,   h->n_embd);
            if (!ly->attn_norm.present || !ly->attn_q.present ||
                !ly->attn_k.present || !ly->attn_v.present || !ly->attn_o.present ||
                !ly->ffn_norm.present || !ly->ffn_gate.present ||
                !ly->ffn_up.present || !ly->ffn_down.present) {
                llm_metal_free(m);
                return NULL;
            }
            /* The fused QKV / gate-up mega-matvecs are STQ1_0-only. If a
             * model quantizes these projections differently, fall back to
             * the CPU engine rather than maintaining a second GPU path. */
            if (ly->attn_q.dtype != GGML_TYPE_STQ1_0 ||
                ly->attn_k.dtype != GGML_TYPE_STQ1_0 ||
                ly->attn_v.dtype != GGML_TYPE_STQ1_0 ||
                ly->ffn_gate.dtype != GGML_TYPE_STQ1_0 ||
                ly->ffn_up.dtype != GGML_TYPE_STQ1_0) {
                llm_metal_free(m);
                return NULL;
            }
            ly->attn_q_norm = resolve_f32(g, LN("attn_q_norm.weight"), h->head_dim);
            ly->attn_k_norm = resolve_f32(g, LN("attn_k_norm.weight"), h->head_dim);
            if (ly->attn_q_norm.present && ly->attn_k_norm.present) {
                qk_seen = true;
            } else {
                ly->attn_q_norm.present = ly->attn_k_norm.present = false;
            }
            #undef LN
        }
        m->qk_norm = qk_seen;
        m->h.qk_norm = qk_seen;

        /* GPU-only activation + KV-cache buffers — Private storage (never
         * touched by the CPU now that k_embed runs the embedding lookup
         * on the GPU). */
        #define NEWBUF(field, nfloats, mode) do { \
            m->field = [m->device newBufferWithLength:(NSUInteger)(nfloats) * sizeof(float) \
                                              options:(mode)]; \
            if (!m->field) { llm_metal_free(m); return NULL; } \
        } while (0)
        #define PRIV MTLResourceStorageModePrivate
        #define SHRD MTLResourceStorageModeShared
        NEWBUF(bX,       h->n_embd, PRIV);
        NEWBUF(bHb,      h->n_embd, PRIV);
        NEWBUF(bQ,       q_dim,     PRIV);
        NEWBUF(bKc,      kv_dim,    PRIV);
        NEWBUF(bVc,      kv_dim,    PRIV);
        NEWBUF(bAttnOut, q_dim,     PRIV);
        NEWBUF(bFfnG,    h->n_ff,   PRIV);
        NEWBUF(bKcache,  (i64)h->n_layers * h->n_ctx * kv_dim, PRIV);
        NEWBUF(bVcache,  (i64)h->n_layers * h->n_ctx * kv_dim, PRIV);
        /* bLogits (~n_vocab floats, Shared) feeds only the non-Q6_K LM head
         * (full-vocab matvec → argmax). The shipping Q6_K head uses the
         * bBestVals/bBestIdx top-k path and never binds bLogits, so skip the
         * allocation there — m->bLogits stays nil and [nil release] is a no-op. */
        if (m->out_proj.dtype != GGML_TYPE_Q6_K) {
            NEWBUF(bLogits, h->n_vocab, SHRD);
        }
        m->bestCount = (NSUInteger)((h->n_vocab + MATVEC_SIMDS - 1) / MATVEC_SIMDS);
        NEWBUF(bBestVals, (i64)m->bestCount, PRIV);
        #undef NEWBUF
        #undef PRIV
        #undef SHRD
        m->bBestIdx = [m->device newBufferWithLength:m->bestCount * sizeof(int)
                                             options:MTLResourceStorageModePrivate];
        if (!m->bBestIdx) { llm_metal_free(m); return NULL; }
        m->bTokens = [m->device newBufferWithLength:(NSUInteger)h->n_ctx * sizeof(int)
                                            options:MTLResourceStorageModeShared];
        if (!m->bTokens) { llm_metal_free(m); return NULL; }
        {
            i32 half_hd = h->head_dim / 2;
            NSUInteger rope_count = (NSUInteger)h->n_ctx * (NSUInteger)half_hd * 2u;
            m->bRope = [m->device newBufferWithLength:rope_count * sizeof(float)
                                              options:MTLResourceStorageModeShared];
            if (!m->bRope) { llm_metal_free(m); return NULL; }
            float *rt = (float *)[m->bRope contents];
            for (i32 pos = 0; pos < h->n_ctx; pos++) {
                for (i32 i = 0; i < half_hd; i++) {
                    float theta = (float)pos *
                        powf(h->rope_freq_base, -2.0f * (float)i / (float)h->head_dim);
                    usize off = ((usize)pos * (usize)half_hd + (usize)i) * 2u;
                    rt[off + 0] = cosf(theta);
                    rt[off + 1] = sinf(theta);
                }
            }
        }
    }

    /* Cache the model-constant params once — encode_forward then copies
     * this per decode and patches only `pos` plus the per-dispatch
     * offsets, instead of rebuilding the struct (and re-deriving
     * attn_scale) on every token. */
    m->base.n_embd         = h->n_embd;
    m->base.n_ff           = h->n_ff;
    m->base.n_head         = h->n_head;
    m->base.n_head_kv      = h->n_head_kv;
    m->base.head_dim       = h->head_dim;
    m->base.n_vocab        = h->n_vocab;
    m->base.n_ctx          = h->n_ctx;
    m->base.rope_neox      = h->rope_neox ? 1 : 0;
    m->base.rms_eps        = h->rms_eps;
    m->base.rope_freq_base = h->rope_freq_base;
    m->base.attn_scale     = 1.0f / sqrtf((f32)h->head_dim);
    m->base.embed_dtype    = (int)m->tok_embd.dtype;

    return m;
}

void llm_metal_free(LlmMetal *m) {
    if (!m) return;
    [m->bX release];       [m->bHb release];     [m->bQ release];
    [m->bKc release];      [m->bVc release];     [m->bAttnOut release];
    [m->bFfnG release];    [m->bLogits release]; [m->bTokens release];
    [m->bKcache release];  [m->bVcache release]; [m->bRope release];
    [m->bBestVals release]; [m->bBestIdx release];
    [m->weights release];
    [m->psoRmsnorm release];   [m->psoMatvecStq release];
    [m->psoMatvecQ6k release]; [m->psoMatvecF32 release];
    [m->psoQkv release];       [m->psoGateUp release];
    [m->psoKvPrep release];    [m->psoAttnBlock release];
    [m->psoEmbed release];     [m->psoArgmax release];
    [m->psoLmHeadQ6kBest release]; [m->psoArgmaxPartials release];
    [m->library release];
    [m->queue release];
    [m->device release];
    free(m->layers);
    free(m);
}

const LlmHParams *llm_metal_hparams(const LlmMetal *m) {
    return m ? &m->h : NULL;
}

/* ---- dispatch helpers ---------------------------------------------- */

/* Matvec dispatch: uniform MATVEC_TG-thread threadgroups = MATVEC_SIMDS
 * SIMD groups, one output row per SIMD group. The kernels stage the
 * input vector into threadgroup memory and bounds-check their row. */
static void enc_matvec(id<MTLComputeCommandEncoder> enc,
                       id<MTLComputePipelineState> pso,
                       const LlmMetalParams *p, NSUInteger out_dim,
                       id<MTLBuffer> in, id<MTLBuffer> weights,
                       id<MTLBuffer> out) {
    [enc setComputePipelineState:pso];
    [enc setBytes:p length:sizeof *p atIndex:0];
    [enc setBuffer:in offset:0 atIndex:1];
    [enc setBuffer:weights offset:0 atIndex:2];
    [enc setBuffer:out offset:0 atIndex:3];
    NSUInteger ngroups = (out_dim + MATVEC_SIMDS - 1) / MATVEC_SIMDS;
    [enc dispatchThreadgroups:MTLSizeMake(ngroups, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(MATVEC_TG, 1, 1)];
}

/* Fused mega-matvec dispatch (k_qkv / k_gate_up): like enc_matvec but
 * binds up to three output buffers (slots 3-5). `total_rows` is the
 * combined output-row count across the fused projections. */
static void enc_megamatvec(id<MTLComputeCommandEncoder> enc,
                           id<MTLComputePipelineState> pso,
                           const LlmMetalParams *p, NSUInteger total_rows,
                           id<MTLBuffer> in, id<MTLBuffer> weights,
                           id<MTLBuffer> out1, id<MTLBuffer> out2,
                           id<MTLBuffer> out3) {
    [enc setComputePipelineState:pso];
    [enc setBytes:p length:sizeof *p atIndex:0];
    [enc setBuffer:in offset:0 atIndex:1];
    [enc setBuffer:weights offset:0 atIndex:2];
    [enc setBuffer:out1 offset:0 atIndex:3];
    if (out2) [enc setBuffer:out2 offset:0 atIndex:4];
    if (out3) [enc setBuffer:out3 offset:0 atIndex:5];
    NSUInteger ngroups = (total_rows + MATVEC_SIMDS - 1) / MATVEC_SIMDS;
    [enc dispatchThreadgroups:MTLSizeMake(ngroups, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(MATVEC_TG, 1, 1)];
}

static void enc_groups(id<MTLComputeCommandEncoder> enc,
                       id<MTLComputePipelineState> pso,
                       const LlmMetalParams *p,
                       NSUInteger ngroups, NSUInteger groupsize,
                       id<MTLBuffer> b1, id<MTLBuffer> b2,
                       id<MTLBuffer> b3, id<MTLBuffer> b4,
                       id<MTLBuffer> b5, id<MTLBuffer> b6) {
    [enc setComputePipelineState:pso];
    [enc setBytes:p length:sizeof *p atIndex:0];
    if (b1) [enc setBuffer:b1 offset:0 atIndex:1];
    if (b2) [enc setBuffer:b2 offset:0 atIndex:2];
    if (b3) [enc setBuffer:b3 offset:0 atIndex:3];
    if (b4) [enc setBuffer:b4 offset:0 atIndex:4];
    if (b5) [enc setBuffer:b5 offset:0 atIndex:5];
    if (b6) [enc setBuffer:b6 offset:0 atIndex:6];
    [enc dispatchThreadgroups:MTLSizeMake(ngroups, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(groupsize, 1, 1)];
}

static id<MTLComputePipelineState> matvec_pso(LlmMetal *m, u32 dtype) {
    switch (dtype) {
        case GGML_TYPE_STQ1_0: return m->psoMatvecStq;
        case GGML_TYPE_Q6_K:   return m->psoMatvecQ6k;
        case GGML_TYPE_F32:    return m->psoMatvecF32;
        default:               return nil;   /* F16 weights unsupported on GPU v1 */
    }
}

static void enc_lm_head_q6k_best(id<MTLComputeCommandEncoder> enc,
                                 LlmMetal *m,
                                 const LlmMetalParams *p,
                                 id<MTLBuffer> in) {
    [enc setComputePipelineState:m->psoLmHeadQ6kBest];
    [enc setBytes:p length:sizeof *p atIndex:0];
    [enc setBuffer:in offset:0 atIndex:1];
    [enc setBuffer:m->weights offset:0 atIndex:2];
    [enc setBuffer:m->bBestVals offset:0 atIndex:3];
    [enc setBuffer:m->bBestIdx offset:0 atIndex:4];
    [enc dispatchThreadgroups:MTLSizeMake(m->bestCount, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(MATVEC_TG, 1, 1)];
}

/* ---- forward pass + generation ------------------------------------- */

/* Encode one decode step for position `pos` into `enc`: the GPU
 * embedding lookup of bTokens[pos], the transformer layers, and — when
 * `do_head` — the final RMSNorm, LM head, and greedy argmax that writes
 * the next token id into bTokens[pos+1]. */
static void encode_forward(LlmMetal *m, id<MTLComputeCommandEncoder> enc,
                           i32 pos, bool do_head) {
    const LlmHParams *h = &m->h;
    i32 q_dim  = h->n_head * h->head_dim;
    i32 kv_dim = h->n_head_kv * h->head_dim;

    LlmMetalParams base = m->base;
    base.pos = pos;

    /* GPU embedding lookup: k_embed reads bTokens[pos] -> bX. */
    {
        LlmMetalParams p = base;
        p.in_dim = h->n_embd;
        p.w_offset = m->tok_embd.w_offset;
        enc_groups(enc, m->psoEmbed, &p,
                   (NSUInteger)((h->n_embd + 63) / 64), 64,
                   m->weights, m->bTokens, m->bX, nil, nil, nil);
    }

    for (i32 l = 0; l < h->n_layers; l++) {
        MLayer *ly = &m->layers[l];
        LlmMetalParams p;

        /* Fused: attn RMSNorm(bX) + Q/K/V projections in one dispatch.
         * w_offset = norm weight, w_offset1/2/3 = Q/K/V weights. */
        p = base; p.in_dim = h->n_embd;
        p.w_offset  = ly->attn_norm.w_offset;
        p.w_offset1 = ly->attn_q.w_offset;
        p.w_offset2 = ly->attn_k.w_offset;
        p.w_offset3 = ly->attn_v.w_offset;
        enc_megamatvec(enc, m->psoQkv, &p, (NSUInteger)(q_dim + 2 * kv_dim),
                       m->bX, m->weights, m->bQ, m->bKc, m->bVc);

        /* Fused: RoPE + QK-norm of the KV heads, then write the prepared
         * K/V into the cache. One threadgroup per KV head. */
        p = base; p.layer = l;
        p.qk_norm = ly->attn_k_norm.present ? 1 : 0;
        p.w_offset = ly->attn_k_norm.w_offset;
        enc_groups(enc, m->psoKvPrep, &p, (NSUInteger)h->n_head_kv, 128,
                   m->bKc, m->bVc, m->weights, m->bKcache, m->bVcache, m->bRope);

        /* Fused: RoPE + QK-norm of each query head, then the causal
         * attention over the prepared cache. One TG per query head. */
        p = base; p.layer = l;
        p.qk_norm = ly->attn_q_norm.present ? 1 : 0;
        p.w_offset = ly->attn_q_norm.w_offset;
        enc_groups(enc, m->psoAttnBlock, &p, (NSUInteger)h->n_head, 128,
                   m->bQ, m->weights, m->bKcache, m->bVcache, m->bAttnOut, m->bRope);

        /* output projection — accumulate straight into the residual. */
        p = base; p.in_dim = q_dim; p.out_dim = h->n_embd;
        p.w_offset = ly->attn_o.w_offset; p.accumulate = 1;
        enc_matvec(enc, matvec_pso(m, ly->attn_o.dtype), &p,
                   (NSUInteger)h->n_embd, m->bAttnOut, m->weights, m->bX);

        /* Fused: ffn RMSNorm(bX) + gate/up projections + SwiGLU; output
         * is silu(gate) * up straight into bFfnG. */
        p = base; p.in_dim = h->n_embd;
        p.w_offset  = ly->ffn_norm.w_offset;
        p.w_offset1 = ly->ffn_gate.w_offset;
        p.w_offset2 = ly->ffn_up.w_offset;
        enc_megamatvec(enc, m->psoGateUp, &p, (NSUInteger)h->n_ff,
                       m->bX, m->weights, m->bFfnG, nil, nil);
        /* ffn_down — accumulate into the residual the same way. */
        p = base; p.in_dim = h->n_ff; p.out_dim = h->n_embd;
        p.w_offset = ly->ffn_down.w_offset; p.accumulate = 1;
        enc_matvec(enc, matvec_pso(m, ly->ffn_down.dtype), &p,
                   (NSUInteger)h->n_embd, m->bFfnG, m->weights, m->bX);
    }

    if (!do_head) return;

    /* final norm + LM head + greedy argmax -> bTokens[pos+1] */
    LlmMetalParams p = base;
    p.in_dim = h->n_embd; p.w_offset = m->out_norm.w_offset;
    enc_groups(enc, m->psoRmsnorm, &p, 1, 256,
               m->bX, m->weights, m->bHb, nil, nil, nil);
    p = base; p.in_dim = h->n_embd; p.out_dim = h->n_vocab;
    p.w_offset = m->out_proj.w_offset;
    if (m->out_proj.dtype == GGML_TYPE_Q6_K) {
        enc_lm_head_q6k_best(enc, m, &p, m->bHb);
        p = base; p.out_dim = (int)m->bestCount; p.write_token = 1;
        enc_groups(enc, m->psoArgmaxPartials, &p, 1, 256,
                   m->bBestVals, m->bBestIdx, m->bTokens, nil, nil, nil);
    } else {
        enc_matvec(enc, matvec_pso(m, m->out_proj.dtype), &p,
                   (NSUInteger)h->n_vocab, m->bHb, m->weights, m->bLogits);
        p = base; p.write_token = 1;
        enc_groups(enc, m->psoArgmax, &p, 1, 256,
                   m->bLogits, m->bTokens, nil, nil, nil, nil);
    }
}

/* Encode + commit one decode step; returns the committed (not yet
 * waited) command buffer. */
static id<MTLCommandBuffer> commit_forward(LlmMetal *m, i32 pos, bool do_head) {
    id<MTLCommandBuffer> cb = [m->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    encode_forward(m, enc, pos, do_head);
    [enc endEncoding];
    [cb commit];
    return cb;
}

/* Encode the whole prompt prefill into one serial command buffer. This
 * preserves the exact same dispatch order as committing one command buffer
 * per prompt token, but removes per-token command-buffer creation/commit
 * overhead from the CPU side. */
static id<MTLCommandBuffer> commit_prompt(LlmMetal *m, i32 n_prompt) {
    id<MTLCommandBuffer> cb = [m->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    for (i32 pos = 0; pos < n_prompt; pos++) {
        encode_forward(m, enc, pos, pos == n_prompt - 1);
    }
    [enc endEncoding];
    [cb commit];
    return cb;
}

i32 llm_metal_generate(LlmMetal *m, const i32 *prompt_ids, i32 n_prompt,
                       i32 max_gen, i32 eos, i32 eot,
                       LlmMetalTokenFn on_id, void *user) {
    if (!m || !prompt_ids) return -1;
    const LlmHParams *h = &m->h;
    /* The first generation step (commit_forward at gen_pos == n_prompt) has its
     * argmax write bTokens[n_prompt + 1], so n_prompt must leave room for that:
     * n_prompt <= n_ctx - 2. Allowing n_prompt == n_ctx - 1 wrote one int past
     * the bTokens buffer (sized n_ctx). */
    if (n_prompt <= 0 || n_prompt >= h->n_ctx - 1) return -1;
    if (max_gen <= 0) return 0;

    /* Seed the prompt token ids into the GPU-visible token buffer. */
    int *toks = (int *)[m->bTokens contents];
    for (i32 i = 0; i < n_prompt; i++) {
        if (prompt_ids[i] < 0 || prompt_ids[i] >= h->n_vocab) return -1;
        toks[i] = prompt_ids[i];
    }

    i32 generated = 0;
    bool profile = llm_metal_profile();
    double t_start = 0.0, t_prefill = 0.0;

    @autoreleasepool {
        /* Feed the prompt. Positions 0..n_prompt-2 run the layers only
         * (to populate the KV cache); the last position also runs the LM
         * head + argmax, producing the first generated token in
         * bTokens[n_prompt]. All prompt positions are encoded into one
         * serial command buffer, so dispatch order is identical to the
         * former one-command-buffer-per-position path. */
        t_start = now_sec();
        id<MTLCommandBuffer> last = commit_prompt(m, n_prompt);
        [last waitUntilCompleted];
        if (last.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "llm_metal: prompt command buffer failed\n");
            return -1;
        }
        t_prefill = now_sec();

        /* Generation, pipelined at depth 2: while the GPU runs the step
         * for `gen_pos`, the CPU streams the already-produced token and
         * encodes + commits the step for `gen_pos+1`. The next step
         * reads bTokens[gen_pos+1] on the GPU — written by the previous
         * step's argmax — and the serial queue orders write before read. */
        i32 gen_pos = n_prompt;
        id<MTLCommandBuffer> prev = commit_forward(m, gen_pos, true);

        for (;;) {
            i32 tok = toks[gen_pos];
            if (tok == eos || tok == eot) { [prev waitUntilCompleted]; break; }
            if (on_id && !on_id(user, tok)) { [prev waitUntilCompleted]; break; }
            generated++;
            if (generated >= max_gen) { [prev waitUntilCompleted]; break; }

            i32 next_pos = gen_pos + 1;
            if (next_pos > h->n_ctx - 2) { [prev waitUntilCompleted]; break; }

            id<MTLCommandBuffer> next = commit_forward(m, next_pos, true);
            [prev waitUntilCompleted];
            if (profile) {
                fprintf(stderr, "[metal] pos=%d gpu=%.2fms\n", gen_pos,
                        (prev.GPUEndTime - prev.GPUStartTime) * 1e3);
            }
            if (prev.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "llm_metal: decode command buffer failed\n");
                [next waitUntilCompleted];
                return generated;
            }
            prev = next;
            gen_pos = next_pos;
        }
    }

    /* prefill = the prompt-feed phase (n_prompt forward passes that fill
     * the KV cache); decode = the autoregressive generation loop. */
    if (profile) {
        double t_end = now_sec();
        double pre_s = t_prefill - t_start;
        double dec_s = t_end - t_prefill;
        fprintf(stderr,
                "[metal] prefill %d tok / %.2fs = %.1f tok/s   "
                "decode %d tok / %.2fs = %.1f tok/s\n",
                n_prompt, pre_s, pre_s > 0.0 ? (double)n_prompt / pre_s : 0.0,
                generated, dec_s, dec_s > 0.0 ? (double)generated / dec_s : 0.0);
    }
    return generated;
}
