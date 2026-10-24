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
#include "core/mem_budget.h"
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
    bool generation_finished;
    /* What this instance reported to the memory ledger. Recorded rather than
     * recomputed, so llm_metal_free subtracts exactly what was added even
     * though the KV buffers are reallocated when the context grows. */
    usize   gpu_accounted;
    usize   kv_accounted;
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
    /* Batched prefill (encode_prefill_chunk): nil when the library lacks them. */
    id<MTLComputePipelineState> psoGemmStq, psoRmsnormRows, psoSwiglu;
    id<MTLComputePipelineState> psoQPrepRows, psoAttnPrefill, psoKvPrepRows;

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
    /* Batched prefill: PREFILL_CHUNK rows of the residual, its normed copy,
     * Q / K / V, the attention output, gate and up. Allocated only when the
     * model can take the batched path (prefill_batched). */
    id<MTLBuffer> bXb, bXN, bQb, bKb, bVb, bAb, bGb, bUb;
    bool          prefill_batched;
    bool          prefill_attn;      /* the chunk's attention in one pass (k_attn_prefill) */
    /* CPU-visible buffers — Shared storage. bTokens holds the prompt +
     * generated token ids; bLogits backs the non-Q6_K LM head and stays nil on
     * the Q6_K (top-k) path. */
    id<MTLBuffer> bTokens, bLogits;

    /* Number of context positions bKcache/bVcache/bRope/bTokens are
     * currently sized for (<= h.n_ctx; 0 = not allocated yet). These four
     * are the only allocations that scale with the context length, and at
     * the model ceiling they dominate the engine's footprint — so they are
     * sized to the request that is about to run, not to h.n_ctx. Owned by
     * ensure_ctx(), which also keeps base.n_ctx (the KV-cache layer stride
     * the kernels index with) equal to it. */
    i32 ctx_alloc;

    /* Model-constant LlmMetalParams fields, filled once at create time;
     * encode_forward copies this and patches only the per-dispatch fields. */
    LlmMetalParams base;
};

/* ---- tensor resolution --------------------------------------------- */

/* The kernels read a tensor's words in place: an F32 norm weight as floats,
 * an STQ1_0 row's codes, signs and scale as 16-bit words (stq_chunk_load).
 * A GGUF writer puts every tensor on a `general.alignment` boundary (8 at
 * the least), but nothing in the file's parser holds a crafted one to it —
 * so a tensor that is not on a 4-byte boundary of the blob is not one the
 * GPU path takes. */
static bool tensor_offset_ok(const GgufTensor *t) {
    return (t->offset & 3u) == 0;
}

static MTensor resolve(const GgufFile *g, const char *name, i64 d0, i64 d1) {
    MTensor mt = {0};
    const GgufTensor *t = gguf_tensor(g, name);
    if (!t) return mt;
    if (d0 > 0 && (i64)t->dims[0] != d0) return mt;
    if (d1 > 0 && (i64)t->dims[1] != d1) return mt;
    if (t->dims[2] != 1 || t->dims[3] != 1 || !t->data) return mt;
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
    if (t->offset > UINT32_MAX || t->nbytes > UINT32_MAX - t->offset)
        return mt;
    if (!tensor_offset_ok(t)) return mt;
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
    if (t->dims[1] != 1 || t->dims[2] != 1 || t->dims[3] != 1 || !t->data)
        return mt;
    if (t->offset > UINT32_MAX || t->nbytes > UINT32_MAX - t->offset)
        return mt;   /* GPU kernels do all byte-offset arithmetic as uint */
    if (!tensor_offset_ok(t)) return mt;
    mt.w_offset = (u32)t->offset;
    mt.dtype = GGML_TYPE_F32;
    mt.present = true;
    return mt;
}

/* ---- metallib + pipeline setup ------------------------------------- */

static id<MTLComputePipelineState> make_pso(id<MTLDevice> dev,
                                            id<MTLLibrary> lib,
                                            const char *fn, const LlmCancel *cancel) {
    if (llm_cancelled(cancel)) return nil;
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
    int n = snprintf(path, sizeof path, "%s/LiuLLM.metallib", dir);
    if (n < 0 || (usize)n >= sizeof path) return nil;
    NSString *ns_path = [NSString stringWithUTF8String:path];
    if (!ns_path) return nil;
    NSURL *url = [NSURL fileURLWithPath:ns_path];
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

/* (Re)size the four position-indexed GPU buffers — the per-layer K and V
 * caches, the RoPE cos/sin table and the token-id buffer — to cover `need`
 * context positions.
 *
 * Everything else in the engine is sized by the model's dimensions; these
 * four scale with the context length, so sizing them at h.n_ctx (the
 * ceiling from the GGUF, clamped to LLM_MODEL_MAX_CTX) reserves the worst
 * case for every request no matter how short. A single short generation
 * touches only the positions it actually visits, so the cache is sized
 * from the request instead.
 *
 * Grow-only: a longer request re-allocates, a shorter one reuses what is
 * already there. Calling it between generations is safe because these
 * buffers carry no state across llm_metal_generate calls (each call's
 * prefill refills the KV cache from position 0) and every exit path of the
 * decode loop waits on its command buffers, so no in-flight GPU work can
 * still reference the buffers being released. Returns false on allocation
 * failure, leaving the previous buffers intact.
 */
static bool ensure_ctx(LlmMetal *m, i32 need) {
    const LlmHParams *h = &m->h;
    if (need < 1) need = 1;
    if (need > h->n_ctx) need = h->n_ctx;
    if (need <= m->ctx_alloc) return true;

    i32 kv_dim  = h->n_head_kv * h->head_dim;
    i32 half_hd = h->head_dim / 2;
    u64 kv_bytes = (u64)h->n_layers * (u64)need * (u64)kv_dim * sizeof(float);

    id<MTLBuffer> kc =
        [m->device newBufferWithLength:(NSUInteger)kv_bytes
                               options:MTLResourceStorageModePrivate];
    id<MTLBuffer> vc =
        [m->device newBufferWithLength:(NSUInteger)kv_bytes
                               options:MTLResourceStorageModePrivate];
    id<MTLBuffer> rope =
        [m->device newBufferWithLength:(NSUInteger)((u64)need * (u64)half_hd
                                                    * 2u * sizeof(float))
                               options:MTLResourceStorageModeShared];
    id<MTLBuffer> toks =
        [m->device newBufferWithLength:(NSUInteger)need * sizeof(int)
                               options:MTLResourceStorageModeShared];
    if (!kc || !vc || !rope || !toks) {
        [kc release]; [vc release]; [rope release]; [toks release];
        return false;
    }
    {
        /* This path REPLACES the previous set when the context grows, so the
         * ledger is set to the new total rather than incremented. */
        usize now = (usize)(kv_bytes * 2u)
                  + (usize)need * (usize)half_hd * 2u * sizeof(float)
                  + (usize)need * sizeof(int);
        mem_budget_transient_adjust(MEM_MODEL_KV,
                                    (isize)now - (isize)m->kv_accounted);
        m->kv_accounted = now;
    }

    /* Each entry depends only on its position, so resizing preserves
     * values at all existing positions. */
    float *rt = (float *)[rope contents];
    for (i32 pos = 0; pos < need; pos++) {
        for (i32 i = 0; i < half_hd; i++) {
            float theta = (float)pos *
                powf(h->rope_freq_base, -2.0f * (float)i / (float)h->head_dim);
            usize off = ((usize)pos * (usize)half_hd + (usize)i) * 2u;
            rt[off + 0] = cosf(theta);
            rt[off + 1] = sinf(theta);
        }
    }

    [m->bKcache release]; [m->bVcache release];
    [m->bRope release];   [m->bTokens release];
    m->bKcache = kc;
    m->bVcache = vc;
    m->bRope   = rope;
    m->bTokens = toks;
    m->ctx_alloc  = need;
    m->base.n_ctx = need;   /* KV-cache layer stride used by the kernels */
    return true;
}

/* Declared here rather than in llm_metal.h: the header entry belongs with
 * the call-site change that would pass a real hint. `want_ctx` only
 * pre-sizes the position-indexed buffers — ensure_ctx still grows them if
 * a generation asks for more, so a wrong hint costs a re-allocation, never
 * a truncated answer. */
static LlmMetal *metal_create(const GgufFile *g, i32 want_ctx, const LlmCancel *cancel);
LlmMetal *llm_metal_create_ctx(const GgufFile *g, i32 want_ctx);

LlmMetal *llm_metal_create(const GgufFile *g) {
    return metal_create(g, 0, NULL);
}

LlmMetal *llm_metal_create_cancellable(const GgufFile *g, const LlmCancel *cancel) {
    return metal_create(g, 0, cancel);
}

LlmMetal *llm_metal_create_ctx(const GgufFile *g, i32 want_ctx) {
    return metal_create(g, want_ctx, NULL);
}

static LlmMetal *metal_create(const GgufFile *g, i32 want_ctx, const LlmCancel *cancel) {
    if (!g || !g->map || llm_cancelled(cancel)) return NULL;

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
    /* k_attn_block reads a head's K and V rows as float4s, a lane to a
     * column of four: a head_dim that is not a multiple of 4 would leave
     * its last elements out of every score. */
    if (h->head_dim % 4 != 0) { free(m); return NULL; }

    /* Resolve tensors (same names/shapes the CPU path validates). */
    m->tok_embd = resolve(g, "token_embd.weight", h->n_embd, 0);
    m->out_norm = resolve_f32(g, "output_norm.weight", h->n_embd);
    m->out_proj = resolve(g, "output.weight", h->n_embd, h->n_vocab);
    if (!gguf_tensor(g, "output.weight")) m->out_proj = m->tok_embd;  /* tied */
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

    if (llm_cancelled(cancel)) { llm_metal_free(m); return NULL; }

    @autoreleasepool {
        /* MTLCreateSystemDefaultDevice follows the create rule: the returned
         * device is already owned (+1), and llm_metal_free releases it once.
         * An extra retain here would leak one MTLDevice per model load. */
        m->device = MTLCreateSystemDefaultDevice();
        if (!m->device) { llm_metal_free(m); return NULL; }
        m->queue = [m->device newCommandQueue];
        if (!m->queue) { llm_metal_free(m); return NULL; }

        m->library = load_metallib(m->device);
        if (!m->library) { llm_metal_free(m); return NULL; }

        m->psoRmsnorm   = make_pso(m->device, m->library, "k_rmsnorm", cancel);
        m->psoMatvecStq = make_pso(m->device, m->library, "k_matvec_stq1_0", cancel);
        m->psoMatvecQ6k = make_pso(m->device, m->library, "k_matvec_q6k", cancel);
        m->psoMatvecF32 = make_pso(m->device, m->library, "k_matvec_f32", cancel);
        m->psoQkv       = make_pso(m->device, m->library, "k_qkv", cancel);
        m->psoGateUp    = make_pso(m->device, m->library, "k_gate_up", cancel);
        m->psoKvPrep    = make_pso(m->device, m->library, "k_kv_prep", cancel);
        m->psoAttnBlock = make_pso(m->device, m->library, "k_attn_block", cancel);
        m->psoEmbed     = make_pso(m->device, m->library, "k_embed", cancel);
        m->psoArgmax    = make_pso(m->device, m->library, "k_argmax", cancel);
        m->psoLmHeadQ6kBest = make_pso(m->device, m->library, "k_lm_head_q6k_best", cancel);
        m->psoArgmaxPartials = make_pso(m->device, m->library, "k_argmax_partials", cancel);
        m->psoGemmStq     = make_pso(m->device, m->library, "k_gemm_stq1_0", cancel);
        m->psoRmsnormRows = make_pso(m->device, m->library, "k_rmsnorm_rows", cancel);
        m->psoSwiglu      = make_pso(m->device, m->library, "k_swiglu", cancel);
        m->psoQPrepRows   = make_pso(m->device, m->library, "k_q_prep_rows", cancel);
        m->psoAttnPrefill = make_pso(m->device, m->library, "k_attn_prefill", cancel);
        m->psoKvPrepRows  = make_pso(m->device, m->library, "k_kv_prep_rows", cancel);
        if (!m->psoRmsnorm || !m->psoMatvecStq || !m->psoMatvecQ6k ||
            !m->psoMatvecF32 || !m->psoQkv || !m->psoGateUp ||
            !m->psoKvPrep || !m->psoAttnBlock || !m->psoEmbed || !m->psoArgmax ||
            !m->psoLmHeadQ6kBest || !m->psoArgmaxPartials) {
            llm_metal_free(m);
            return NULL;
        }
        /* k_attn_block's scratch is laid out for ATTN_TG threads in SIMD
         * groups of 32, and it is dispatched with exactly that. */
        if (m->psoAttnBlock.maxTotalThreadsPerThreadgroup < ATTN_TG ||
            m->psoAttnBlock.threadExecutionWidth != 32) {
            fprintf(stderr, "llm_metal: k_attn_block needs %d threads in groups of 32 "
                            "(this GPU: %lu, %lu)\n", ATTN_TG,
                    (unsigned long)m->psoAttnBlock.maxTotalThreadsPerThreadgroup,
                    (unsigned long)m->psoAttnBlock.threadExecutionWidth);
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
        /* A GPU copy of the whole tensor blob — ~0.5 GB for the shipped
         * model, and the single largest allocation in the process while a
         * translation runs. transient_adjust: the translate worker owns the
         * load/free cycle, and the plain counters are main-thread only. */
        m->gpu_accounted += (usize)blob_size;
        mem_budget_transient_adjust(MEM_MODEL_GPU, (isize)blob_size);
        {
            const NSUInteger CHUNK = 32u * 1024u * 1024u;
            NSUInteger stage_len = blob_size < CHUNK ? (NSUInteger)blob_size : CHUNK;
            id<MTLBuffer> staging =
                [m->device newBufferWithLength:stage_len
                                       options:MTLResourceStorageModeShared];
            if (!staging) { llm_metal_free(m); return NULL; }
            const char *src = (const char *)g->tensor_data;
            for (u64 off = 0; off < blob_size; off += CHUNK) {
                if (llm_cancelled(cancel)) { [staging release]; llm_metal_free(m); return NULL; }
                NSUInteger n = (NSUInteger)(blob_size - off < CHUNK
                                           ? blob_size - off : CHUNK);
                memcpy([staging contents], src + off, n);
                id<MTLCommandBuffer> bcb = [m->queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [bcb blitCommandEncoder];
                if (!bcb || !blit) { [staging release]; llm_metal_free(m); return NULL; }
                [blit copyFromBuffer:staging sourceOffset:0
                            toBuffer:m->weights destinationOffset:(NSUInteger)off
                                size:n];
                [blit endEncoding];
                [bcb commit];
                [bcb waitUntilCompleted];   /* serialize: staging is reused */
                if (bcb.status != MTLCommandBufferStatusCompleted) {
                    [staging release]; llm_metal_free(m); return NULL;
                }
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

        if (llm_cancelled(cancel)) { llm_metal_free(m); return NULL; }
        /* GPU-only activation + KV-cache buffers — Private storage (never
         * touched by the CPU: k_embed runs the embedding lookup on the
         * GPU). */
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
        /* bKcache/bVcache/bRope/bTokens are the context-sized buffers and
         * are allocated by ensure_ctx() below, not here. */
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

        /* Batched prefill: every projection STQ1_0 and every width a whole
         * number of blocks (the GEMM's K walk), or the prompt goes a position
         * at a time as before. LIU_LLM_PREFILL_BATCH=0 forces that. */
        const char *pb = getenv("LIU_LLM_PREFILL_BATCH");
        bool batched = !(pb && pb[0] == '0') && m->psoGemmStq && m->psoRmsnormRows &&
                       m->psoSwiglu && m->psoKvPrepRows && h->n_embd % 256 == 0 && h->n_ff % 256 == 0 &&
                       q_dim % 256 == 0 &&
                       m->psoGemmStq.maxTotalThreadsPerThreadgroup >= GEMM_TG &&
                       m->psoGemmStq.threadExecutionWidth == 32;
        for (i32 l = 0; batched && l < h->n_layers; l++) {
            const MLayer *ly = &m->layers[l];
            batched = ly->attn_q.dtype == GGML_TYPE_STQ1_0 && ly->attn_k.dtype == GGML_TYPE_STQ1_0 &&
                      ly->attn_v.dtype == GGML_TYPE_STQ1_0 && ly->attn_o.dtype == GGML_TYPE_STQ1_0 &&
                      ly->ffn_gate.dtype == GGML_TYPE_STQ1_0 && ly->ffn_up.dtype == GGML_TYPE_STQ1_0 &&
                      ly->ffn_down.dtype == GGML_TYPE_STQ1_0;
        }
        if (batched) {
            NSUInteger c = PREFILL_CHUNK;
            NSUInteger floats[8] = {
                c * (NSUInteger)h->n_embd, c * (NSUInteger)h->n_embd, c * (NSUInteger)q_dim,
                c * (NSUInteger)kv_dim, c * (NSUInteger)kv_dim, c * (NSUInteger)q_dim,
                c * (NSUInteger)h->n_ff, c * (NSUInteger)h->n_ff };
            id<MTLBuffer> *bufs[8] = { &m->bXb, &m->bXN, &m->bQb, &m->bKb, &m->bVb, &m->bAb,
                                       &m->bGb, &m->bUb };
            usize bytes = 0;
            for (int i = 0; i < 8 && batched; i++) {
                *bufs[i] = [m->device newBufferWithLength:floats[i] * sizeof(float)
                                                  options:MTLResourceStorageModePrivate];
                if (!*bufs[i]) batched = false;
                bytes += floats[i] * sizeof(float);
            }
            if (batched) {
                m->gpu_accounted += bytes;
                mem_budget_transient_adjust(MEM_MODEL_GPU, (isize)bytes);
            }
        }
        m->prefill_batched = batched;

        /* The chunk's attention in one pass: a kv head's query heads (at most
         * FA_MAX_GROUP) as SIMD groups of one threadgroup, a head of at most
         * FA_MAX_HD in 8-wide tiles. LIU_LLM_PREFILL_ATTN=0 keeps the decode
         * kernel a position at a time. */
        const char *pa = getenv("LIU_LLM_PREFILL_ATTN");
        i32 group = h->n_head_kv > 0 ? h->n_head / h->n_head_kv : 0;
        m->prefill_attn = batched && !(pa && pa[0] == '0') && m->psoQPrepRows && m->psoAttnPrefill &&
                          h->n_head_kv > 0 && h->n_head % h->n_head_kv == 0 &&
                          group >= 1 && group <= FA_MAX_GROUP &&
                          h->head_dim % 8 == 0 && h->head_dim <= FA_MAX_HD &&
                          m->psoAttnPrefill.maxTotalThreadsPerThreadgroup >= (NSUInteger)(group * 32) &&
                          m->psoAttnPrefill.threadExecutionWidth == 32 &&
                          m->psoQPrepRows.maxTotalThreadsPerThreadgroup >= FA_PREP_TG;
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
    /* base.n_ctx is deliberately absent here: it is the KV-cache layer
     * stride, and ensure_ctx() owns it so the stride can never disagree
     * with the size the cache was actually allocated at. */
    m->base.rope_neox      = h->rope_neox ? 1 : 0;
    m->base.rms_eps        = h->rms_eps;
    m->base.rope_freq_base = h->rope_freq_base;
    m->base.attn_scale     = 1.0f / sqrtf((f32)h->head_dim);
    m->base.embed_dtype    = (int)m->tok_embd.dtype;

    /* Optional pre-size. Without a hint the context-sized buffers stay
     * unallocated until llm_metal_generate sizes them from its request. */
    if (want_ctx > 0 && !ensure_ctx(m, want_ctx)) {
        llm_metal_free(m);
        return NULL;
    }

    return m;
}

void llm_metal_free(LlmMetal *m) {
    if (!m) return;
    [m->bX release];       [m->bHb release];     [m->bQ release];
    [m->bKc release];      [m->bVc release];     [m->bAttnOut release];
    [m->bFfnG release];    [m->bLogits release]; [m->bTokens release];
    [m->bKcache release];  [m->bVcache release]; [m->bRope release];
    [m->bBestVals release]; [m->bBestIdx release];
    [m->bXb release]; [m->bXN release]; [m->bQb release]; [m->bKb release];
    [m->bVb release]; [m->bAb release]; [m->bGb release]; [m->bUb release];
    [m->weights release];
    if (m->gpu_accounted)
        mem_budget_transient_adjust(MEM_MODEL_GPU, -(isize)m->gpu_accounted);
    if (m->kv_accounted)
        mem_budget_transient_adjust(MEM_MODEL_KV, -(isize)m->kv_accounted);
    [m->psoRmsnorm release];   [m->psoMatvecStq release];
    [m->psoMatvecQ6k release]; [m->psoMatvecF32 release];
    [m->psoQkv release];       [m->psoGateUp release];
    [m->psoKvPrep release];    [m->psoAttnBlock release];
    [m->psoEmbed release];     [m->psoArgmax release];
    [m->psoLmHeadQ6kBest release]; [m->psoArgmaxPartials release];
    [m->psoGemmStq release]; [m->psoRmsnormRows release]; [m->psoSwiglu release];
    [m->psoQPrepRows release]; [m->psoAttnPrefill release]; [m->psoKvPrepRows release];
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
         * attention over the prepared cache. One TG of ATTN_TG threads per
         * query head. */
        p = base; p.layer = l;
        p.qk_norm = ly->attn_q_norm.present ? 1 : 0;
        p.w_offset = ly->attn_q_norm.w_offset;
        enc_groups(enc, m->psoAttnBlock, &p, (NSUInteger)h->n_head, ATTN_TG,
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
    if (!cb || !enc) return nil;
    encode_forward(m, enc, pos, do_head);
    [enc endEncoding];
    [cb commit];
    return cb;
}

/* Bound work between cancellation polls: LLM_PREFILL_BATCH prompt positions per
 * command buffer, so a long prompt never runs as one uncancellable command
 * buffer. The batching keeps the dispatch order unchanged. */
#define LLM_PREFILL_BATCH 4
static id<MTLCommandBuffer> commit_prompt_range(LlmMetal *m, i32 first,
                                                i32 end, i32 n_prompt) {
    id<MTLCommandBuffer> cb = [m->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    if (!cb || !enc) return nil;
    for (i32 pos = first; pos < end; pos++)
        encode_forward(m, enc, pos, pos == n_prompt - 1);
    [enc endEncoding];
    [cb commit];
    return cb;
}

/* ---- batched prefill ------------------------------------------------- */

static void enc_buf(id<MTLComputeCommandEncoder> enc, id<MTLBuffer> b,
                    NSUInteger offset_floats, NSUInteger index) {
    [enc setBuffer:b offset:offset_floats * sizeof(float) atIndex:index];
}

/* Y (=|+=) W . X for `n_tok` rows of X (k_gemm_stq1_0). */
static void enc_gemm(id<MTLComputeCommandEncoder> enc, LlmMetal *m,
                     const LlmMetalParams *base, u32 w_offset, i32 in_dim, i32 out_dim,
                     i32 n_tok, int accumulate, id<MTLBuffer> x, id<MTLBuffer> y) {
    LlmMetalParams p = *base;
    p.in_dim = in_dim; p.out_dim = out_dim; p.n_tok = n_tok;
    p.w_offset = w_offset; p.accumulate = accumulate;
    [enc setComputePipelineState:m->psoGemmStq];
    [enc setBytes:&p length:sizeof p atIndex:0];
    [enc setBuffer:x offset:0 atIndex:1];
    [enc setBuffer:m->weights offset:0 atIndex:2];
    [enc setBuffer:y offset:0 atIndex:3];
    [enc dispatchThreadgroups:MTLSizeMake((NSUInteger)((out_dim + GEMM_ROWS - 1) / GEMM_ROWS),
                                          (NSUInteger)((n_tok + GEMM_TOKS - 1) / GEMM_TOKS), 1)
        threadsPerThreadgroup:MTLSizeMake(GEMM_TG, 1, 1)];
}

static void enc_rmsnorm_rows(id<MTLComputeCommandEncoder> enc, LlmMetal *m,
                             const LlmMetalParams *base, u32 w_offset, i32 n_tok) {
    LlmMetalParams p = *base;
    p.in_dim = m->h.n_embd; p.n_tok = n_tok; p.w_offset = w_offset;
    [enc setComputePipelineState:m->psoRmsnormRows];
    [enc setBytes:&p length:sizeof p atIndex:0];
    [enc setBuffer:m->bXb offset:0 atIndex:1];
    [enc setBuffer:m->weights offset:0 atIndex:2];
    [enc setBuffer:m->bXN offset:0 atIndex:3];
    [enc dispatchThreadgroups:MTLSizeMake((NSUInteger)n_tok, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(MATVEC_TG, 1, 1)];
}

/* Positions first .. first + n - 1 of the prompt through every layer, the
 * projections batched (k_gemm_stq1_0) and the per-position steps — the
 * embedding lookup, RoPE + the KV write, the causal attention — through the
 * decode path's own kernels a position at a time, each reading its row of
 * the chunk. When `head` (the chunk ends the prompt), the last position's
 * final norm, LM head and argmax write the first answer at bTokens[pos+1],
 * as encode_forward's would. */
static void encode_prefill_chunk(LlmMetal *m, id<MTLComputeCommandEncoder> enc,
                                 i32 first, i32 n, bool head) {
    const LlmHParams *h = &m->h;
    i32 q_dim  = h->n_head * h->head_dim;
    i32 kv_dim = h->n_head_kv * h->head_dim;
    LlmMetalParams base = m->base;

    for (i32 t = 0; t < n; t++) {
        LlmMetalParams p = base;
        p.pos = first + t; p.in_dim = h->n_embd; p.w_offset = m->tok_embd.w_offset;
        [enc setComputePipelineState:m->psoEmbed];
        [enc setBytes:&p length:sizeof p atIndex:0];
        [enc setBuffer:m->weights offset:0 atIndex:1];
        [enc setBuffer:m->bTokens offset:0 atIndex:2];
        enc_buf(enc, m->bXb, (NSUInteger)t * (NSUInteger)h->n_embd, 3);
        [enc dispatchThreadgroups:MTLSizeMake((NSUInteger)((h->n_embd + 63) / 64), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    }

    for (i32 l = 0; l < h->n_layers; l++) {
        MLayer *ly = &m->layers[l];
        LlmMetalParams p;

        enc_rmsnorm_rows(enc, m, &base, ly->attn_norm.w_offset, n);
        enc_gemm(enc, m, &base, ly->attn_q.w_offset, h->n_embd, q_dim,  n, 0, m->bXN, m->bQb);
        enc_gemm(enc, m, &base, ly->attn_k.w_offset, h->n_embd, kv_dim, n, 0, m->bXN, m->bKb);
        enc_gemm(enc, m, &base, ly->attn_v.w_offset, h->n_embd, kv_dim, n, 0, m->bXN, m->bVb);

        /* RoPE + QK-norm of every kv head of every position, into the cache */
        p = base; p.layer = l; p.pos = first; p.n_tok = n;
        p.qk_norm = ly->attn_k_norm.present ? 1 : 0;
        p.w_offset = ly->attn_k_norm.w_offset;
        [enc setComputePipelineState:m->psoKvPrepRows];
        [enc setBytes:&p length:sizeof p atIndex:0];
        [enc setBuffer:m->bKb offset:0 atIndex:1];
        [enc setBuffer:m->bVb offset:0 atIndex:2];
        [enc setBuffer:m->weights offset:0 atIndex:3];
        [enc setBuffer:m->bKcache offset:0 atIndex:4];
        [enc setBuffer:m->bVcache offset:0 atIndex:5];
        [enc setBuffer:m->bRope offset:0 atIndex:6];
        [enc dispatchThreadgroups:MTLSizeMake((NSUInteger)h->n_head_kv, (NSUInteger)n, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];

        if (m->prefill_attn) {
            /* every query head of every position: RoPE + QK-norm, in place */
            p = base; p.pos = first; p.n_tok = n;
            p.qk_norm = ly->attn_q_norm.present ? 1 : 0;
            p.w_offset = ly->attn_q_norm.w_offset;
            [enc setComputePipelineState:m->psoQPrepRows];
            [enc setBytes:&p length:sizeof p atIndex:0];
            [enc setBuffer:m->bQb offset:0 atIndex:1];
            [enc setBuffer:m->weights offset:0 atIndex:2];
            [enc setBuffer:m->bRope offset:0 atIndex:3];
            [enc dispatchThreadgroups:MTLSizeMake((NSUInteger)h->n_head, (NSUInteger)n, 1)
                threadsPerThreadgroup:MTLSizeMake(FA_PREP_TG, 1, 1)];
            /* the chunk's causal attention, a kv head and FA_QP positions a group */
            p = base; p.layer = l; p.pos = first; p.n_tok = n;
            [enc setComputePipelineState:m->psoAttnPrefill];
            [enc setBytes:&p length:sizeof p atIndex:0];
            [enc setBuffer:m->bQb offset:0 atIndex:1];
            [enc setBuffer:m->bKcache offset:0 atIndex:2];
            [enc setBuffer:m->bVcache offset:0 atIndex:3];
            [enc setBuffer:m->bAb offset:0 atIndex:4];
            [enc dispatchThreadgroups:MTLSizeMake((NSUInteger)h->n_head_kv,
                                                  (NSUInteger)((n + FA_QP - 1) / FA_QP), 1)
                threadsPerThreadgroup:MTLSizeMake((NSUInteger)(h->n_head / h->n_head_kv) * 32, 1, 1)];
        } else {
            for (i32 t = 0; t < n; t++) {
                p = base; p.layer = l; p.pos = first + t;
                p.qk_norm = ly->attn_q_norm.present ? 1 : 0;
                p.w_offset = ly->attn_q_norm.w_offset;
                [enc setComputePipelineState:m->psoAttnBlock];
                [enc setBytes:&p length:sizeof p atIndex:0];
                enc_buf(enc, m->bQb, (NSUInteger)t * (NSUInteger)q_dim, 1);
                [enc setBuffer:m->weights offset:0 atIndex:2];
                [enc setBuffer:m->bKcache offset:0 atIndex:3];
                [enc setBuffer:m->bVcache offset:0 atIndex:4];
                enc_buf(enc, m->bAb, (NSUInteger)t * (NSUInteger)q_dim, 5);
                [enc setBuffer:m->bRope offset:0 atIndex:6];
                [enc dispatchThreadgroups:MTLSizeMake((NSUInteger)h->n_head, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(ATTN_TG, 1, 1)];
            }
        }

        enc_gemm(enc, m, &base, ly->attn_o.w_offset, q_dim, h->n_embd, n, 1, m->bAb, m->bXb);

        enc_rmsnorm_rows(enc, m, &base, ly->ffn_norm.w_offset, n);
        enc_gemm(enc, m, &base, ly->ffn_gate.w_offset, h->n_embd, h->n_ff, n, 0, m->bXN, m->bGb);
        enc_gemm(enc, m, &base, ly->ffn_up.w_offset,   h->n_embd, h->n_ff, n, 0, m->bXN, m->bUb);
        p = base; p.n_tok = n;
        [enc setComputePipelineState:m->psoSwiglu];
        [enc setBytes:&p length:sizeof p atIndex:0];
        [enc setBuffer:m->bGb offset:0 atIndex:1];
        [enc setBuffer:m->bUb offset:0 atIndex:2];
        [enc dispatchThreadgroups:MTLSizeMake(((NSUInteger)n * (NSUInteger)h->n_ff + 255) / 256, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        enc_gemm(enc, m, &base, ly->ffn_down.w_offset, h->n_ff, h->n_embd, n, 1, m->bGb, m->bXb);
    }

    if (!head) return;

    /* The last position's final norm + LM head + argmax -> bTokens[pos+1]. */
    LlmMetalParams p = base;
    p.pos = first + n - 1;
    p.in_dim = h->n_embd; p.w_offset = m->out_norm.w_offset;
    [enc setComputePipelineState:m->psoRmsnorm];
    [enc setBytes:&p length:sizeof p atIndex:0];
    enc_buf(enc, m->bXb, (NSUInteger)(n - 1) * (NSUInteger)h->n_embd, 1);
    [enc setBuffer:m->weights offset:0 atIndex:2];
    [enc setBuffer:m->bHb offset:0 atIndex:3];
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    p = base; p.pos = first + n - 1; p.in_dim = h->n_embd; p.out_dim = h->n_vocab;
    p.w_offset = m->out_proj.w_offset;
    if (m->out_proj.dtype == GGML_TYPE_Q6_K) {
        enc_lm_head_q6k_best(enc, m, &p, m->bHb);
        p = base; p.pos = first + n - 1; p.out_dim = (int)m->bestCount; p.write_token = 1;
        enc_groups(enc, m->psoArgmaxPartials, &p, 1, 256,
                   m->bBestVals, m->bBestIdx, m->bTokens, nil, nil, nil);
    } else {
        enc_matvec(enc, matvec_pso(m, m->out_proj.dtype), &p,
                   (NSUInteger)h->n_vocab, m->bHb, m->weights, m->bLogits);
        p = base; p.pos = first + n - 1; p.write_token = 1;
        enc_groups(enc, m->psoArgmax, &p, 1, 256,
                   m->bLogits, m->bTokens, nil, nil, nil, nil);
    }
}

static id<MTLCommandBuffer> commit_prefill_chunk(LlmMetal *m, i32 first, i32 n, bool head) {
    id<MTLCommandBuffer> cb = [m->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    if (!cb || !enc) return nil;
    encode_prefill_chunk(m, enc, first, n, head);
    [enc endEncoding];
    [cb commit];
    return cb;
}

i32 llm_metal_generate(LlmMetal *m, const i32 *prompt_ids, i32 n_prompt,
                       i32 max_gen, i32 eos, i32 eot,
                       LlmMetalTokenFn on_id, void *user) {
    return llm_metal_generate_cancellable(m, prompt_ids, n_prompt, max_gen,
                                          eos, eot, on_id, user, NULL);
}

i32 llm_metal_generate_cancellable(LlmMetal *m, const i32 *prompt_ids, i32 n_prompt,
                                    i32 max_gen, i32 eos, i32 eot,
                                    LlmMetalTokenFn on_id, void *user,
                                    const LlmCancel *cancel) {
    if (m) m->generation_finished = false;
    if (llm_cancelled(cancel)) return LLM_CANCELLED;
    if (!m || !prompt_ids) return LLM_ERROR;
    const LlmHParams *h = &m->h;
    /* Prefill already produces the first answer at bTokens[n_prompt]. No
     * additional forward pass is needed when only this final slot remains. */
    if (n_prompt <= 0 || n_prompt >= h->n_ctx) return LLM_ERROR;
    if (max_gen <= 0) return 0;
    i32 limit = max_gen < h->n_ctx - n_prompt ? max_gen : h->n_ctx - n_prompt;
    if (!ensure_ctx(m, n_prompt + limit)) return LLM_ERROR;
    int *toks = (int *)[m->bTokens contents];
    for (i32 i = 0; i < n_prompt; i++) {
        if (prompt_ids[i] < 0 || prompt_ids[i] >= h->n_vocab) return LLM_ERROR;
        toks[i] = prompt_ids[i];
    }

    i32 generated = 0;
    bool profile = llm_metal_profile();
    double t_start = now_sec(), t_prefill = t_start;
    /* A prompt goes PREFILL_CHUNK positions at a time through the batched
     * projections when the model allows it (prefill_batched), else a position
     * at a time; either way a command buffer is a bounded piece of work and
     * cancellation is looked at between them. */
    i32 step = m->prefill_batched ? PREFILL_CHUNK : LLM_PREFILL_BATCH;
    for (i32 first = 0; first < n_prompt; first += step) {
        if (llm_cancelled(cancel)) return LLM_CANCELLED;
        i32 end = first + step;
        if (end > n_prompt) end = n_prompt;
        @autoreleasepool {
            id<MTLCommandBuffer> cb = m->prefill_batched
                ? commit_prefill_chunk(m, first, end - first, end == n_prompt)
                : commit_prompt_range(m, first, end, n_prompt);
            [cb waitUntilCompleted];
            if (!cb || cb.status != MTLCommandBufferStatusCompleted) return LLM_ERROR;
        }
    }
    if (llm_cancelled(cancel)) return LLM_CANCELLED;
    t_prefill = now_sec();

    @autoreleasepool {
        i32 gen_pos = n_prompt;
        id<MTLCommandBuffer> prev = nil;
        /* Keep at most two decode passes queued, but never speculate beyond
         * the requested output count or the final allocated token position. */
        if (limit > 1) {
            @autoreleasepool { prev = [commit_forward(m, gen_pos, true) retain]; }
            if (!prev) return LLM_ERROR;
        }
        i32 result = 0;
        for (;;) {
            if (llm_cancelled(cancel)) { result = LLM_CANCELLED; break; }
            i32 tok = toks[gen_pos];
            if (tok == eos || tok == eot) { m->generation_finished = true; break; }
            if (on_id && !on_id(user, tok)) break;
            generated++;
            if (generated >= limit) break;

            @autoreleasepool {
                id<MTLCommandBuffer> next = nil;
                bool more = generated + 1 < limit;
                bool cancelled = llm_cancelled(cancel);
                if (more && !cancelled)
                    next = [commit_forward(m, gen_pos + 1, true) retain];
                [prev waitUntilCompleted];
                bool failed = !prev || prev.status != MTLCommandBufferStatusCompleted;
                if (profile && prev) fprintf(stderr, "[metal] pos=%d gpu=%.2fms\n", gen_pos,
                                            (prev.GPUEndTime - prev.GPUStartTime) * 1e3);
                [prev release];
                prev = next;
                if (failed || (more && !next && !cancelled)) result = LLM_ERROR;
                else if (cancelled) result = LLM_CANCELLED;
            }
            if (result < 0) break;
            gen_pos++;
        }
        /* Cancellation/EOS can leave a speculative pass queued. Drain it
         * before callers reuse the buffers or free the engine. */
        if (prev) {
            [prev waitUntilCompleted];
            if (prev.status != MTLCommandBufferStatusCompleted) result = LLM_ERROR;
            [prev release];
        }
        if (llm_cancelled(cancel)) return LLM_CANCELLED;
        if (result < 0) return result;
    }
    if (profile) {
        double pre_s = t_prefill - t_start, dec_s = now_sec() - t_prefill;
        fprintf(stderr, "[metal] prefill %d tok / %.2fs = %.1f tok/s   decode %d tok / %.2fs = %.1f tok/s\n",
                n_prompt, pre_s, pre_s > 0 ? n_prompt / pre_s : 0,
                generated, dec_s, dec_s > 0 ? generated / dec_s : 0);
    }
    return generated;
}

bool llm_metal_generation_finished(const LlmMetal *m) {
    return m && m->generation_finished;
}
