/*
 * Whisper Metal backend. Large projection matrices and the token-embedding
 * table stay quantized in `wbuf` and are dequantized in kernels. Small
 * tensors are stored as F32 in `sbuf` to avoid dtype switches in their
 * kernels. Tensors in `wbuf` are aligned to 256-byte boundaries; blocks
 * within a tensor can remain unaligned, so kernels read them byte-wise. The
 * full encoder runs in one command buffer and one compute encoder because it
 * has no intermediate results to read back.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <libgen.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voice/whisper_metal.h"
#include "voice/whisper_mel.h"
#include "voice/whisper_metal_params.h"
#include "voice/whisper_quant.h"
#include "core/mem_budget.h"

/* The shader carries its own copy of the dtype ids; keep the two spellings
 * from drifting. */
_Static_assert(WM_TYPE_F32  == WGGML_TYPE_F32,  "dtype id drift");
_Static_assert(WM_TYPE_F16  == WGGML_TYPE_F16,  "dtype id drift");
_Static_assert(WM_TYPE_Q4_0 == WGGML_TYPE_Q4_0, "dtype id drift");
_Static_assert(WM_TYPE_Q4_1 == WGGML_TYPE_Q4_1, "dtype id drift");
_Static_assert(WM_TYPE_Q5_0 == WGGML_TYPE_Q5_0, "dtype id drift");
_Static_assert(WM_TYPE_Q5_1 == WGGML_TYPE_Q5_1, "dtype id drift");
_Static_assert(WM_TYPE_Q8_0 == WGGML_TYPE_Q8_0, "dtype id drift");

/* Below this many rows the tiled GEMM wastes most of its 32-row tile on
 * padding, so the matvec kernel takes over. Decoding runs at M = 1. */
#define WM_MATVEC_MAX_M 4

/* A quantized matrix in wbuf. */
typedef struct { u64 off; i32 dtype; i32 K, N; } WmMat;
/* A dequantized vector/matrix in sbuf, as a float index. */
typedef struct { u64 off; i32 n; } WmVec;

typedef struct {
    WmVec attn_ln_w, attn_ln_b;
    WmMat attn_q_w;  WmVec attn_q_b;
    WmMat attn_k_w;
    WmMat attn_v_w;  WmVec attn_v_b;
    WmMat attn_o_w;  WmVec attn_o_b;

    WmVec cross_ln_w, cross_ln_b;
    WmMat cross_q_w; WmVec cross_q_b;
    WmMat cross_k_w;
    WmMat cross_v_w; WmVec cross_v_b;
    WmMat cross_o_w; WmVec cross_o_b;

    WmVec mlp_ln_w, mlp_ln_b;
    WmMat mlp_0_w;   WmVec mlp_0_b;
    WmMat mlp_2_w;   WmVec mlp_2_b;
} WmLayer;

struct WhisperMetal {
    usize cpu_accounted, gpu_accounted;
    id<MTLDevice>       dev;
    id<MTLCommandQueue> queue;
    id<MTLLibrary>      lib;

    id<MTLComputePipelineState> psoGemm, psoMatvec, psoConv, psoNorm,
                                psoAttn, psoAdd, psoGelu, psoTransPos,
                                psoEmbed, psoKvStore;

    id<MTLBuffer> wbuf, sbuf;
    id<MTLBuffer> mel, ff, x, xn, q, k, v, att;
    id<MTLBuffer> cross_k, cross_v, self_k, self_v;
    id<MTLBuffer> ids, logits;

    /* Dimensions, copied so the weight table need not outlive the
     * backend. */
    i32 n_state, n_ctx_a, n_ctx_t, n_mels, n_head_a, n_head_t, hd_a, hd_t;
    i32 n_ff_a, n_ff_t, n_vocab, n_layer_a, n_layer_t;

    /* Row counts rounded up to a whole attention tile. The attention kernel
     * hands Q/K/V straight to simdgroup_load, which has no bounds check, so
     * the last tile of a 1500-row context reads four rows past the end unless
     * the buffers carry them. Every row-strided buffer — including the
     * per-layer stride inside the cross-attention cache — uses these. */
    i32 ctx_a_pad, ctx_t_pad;

    WmVec conv1_w, conv1_b, conv2_w, conv2_b, e_pos, e_ln_w, e_ln_b;
    WmMat tok_emb;
    WmVec d_pos, d_ln_w, d_ln_b;

    WmLayer *enc;
    WmLayer *dec;

    bool encoded;
};

/* ---- pipeline / library ------------------------------------------------ */

static id<MTLComputePipelineState> wm_pso(id<MTLDevice> dev, id<MTLLibrary> lib,
                                          const char *fn) {
    id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:fn]];
    if (!f) {
        fprintf(stderr, "whisper_metal: missing kernel '%s'\n", fn);
        return nil;
    }
    NSError *err = nil;
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:f error:&err];
    [f release];
    if (!pso) {
        fprintf(stderr, "whisper_metal: pipeline '%s' failed: %s\n", fn,
                err ? err.localizedDescription.UTF8String : "(unknown)");
    }
    return pso;
}

/* LiuWhisper.metallib sits next to the running binary, the same convention
 * the renderer and the LLM engine use: it must be in Contents/MacOS, not
 * Resources. */
static id<MTLLibrary> wm_load_lib(id<MTLDevice> dev) {
    char exe[4096];
    u32 sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) != 0) return nil;
    char *dir = dirname(exe);
    char path[4200];
    int n = snprintf(path, sizeof path, "%s/LiuWhisper.metallib", dir);
    if (n < 0 || (usize)n >= sizeof path) return nil;
    NSString *p = [NSString stringWithUTF8String:path];
    if (!p) return nil;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:p] error:&err];
    if (!lib) {
        fprintf(stderr, "whisper_metal: cannot load %s: %s\n", path,
                err ? err.localizedDescription.UTF8String : "(unknown)");
    }
    return lib;
}

/* Reject a metallib whose tile constants differ from this build. The host
 * and shader must use matching constants for dispatch dimensions and kernel
 * tiling; a mismatch can produce an invalid transcript. Check once at
 * startup and fall back to the CPU backend on mismatch. */
static bool wm_params_match(id<MTLDevice> dev, id<MTLCommandQueue> queue,
                            id<MTLLibrary> lib, char *err, usize err_cap) {
    id<MTLComputePipelineState> pso = wm_pso(dev, lib, "k_params_probe");
    if (!pso) {
        if (err && err_cap) snprintf(err, err_cap, "Metal shader library is out of date");
        return false;
    }
    const i32 want[8] = {
        WM_BM, WM_BN, WM_BK, WM_GEMM_THREADS,
        WM_ATTN_QB, WM_ATTN_KB, WM_ATTN_THREADS, WM_ATTN_HD_MAX,
    };
    id<MTLBuffer> buf = [dev newBufferWithLength:sizeof want
                                         options:MTLResourceStorageModeShared];
    if (!buf) { [pso release]; return false; }
    memset(buf.contents, 0, sizeof want);

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:pso];
    [e setBuffer:buf offset:0 atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    bool ok = (cb.status == MTLCommandBufferStatusCompleted) &&
              (memcmp(buf.contents, want, sizeof want) == 0);
    if (!ok && err && err_cap) {
        snprintf(err, err_cap,
                 "LiuWhisper.metallib was built from different tile constants");
    }
    [buf release];
    [pso release];
    return ok;
}

bool whisper_metal_available(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        bool ok = (dev != nil);
        [dev release];
        return ok;
    }
}

/* ---- weight packing ---------------------------------------------------- */

/* Two passes over the tensor list: one to measure, one to copy. Keeping the
 * layout decision in one place is what lets the alignment rule (256 bytes
 * per tensor) hold for every tensor without a per-call-site reminder. */
typedef struct {
    u8   *base;       /* NULL during the measuring pass */
    u64   cursor;
    bool  ok;
} WmPack;

static u64 wm_align(u64 v) { return (v + 255ull) & ~255ull; }

static WmMat wm_pack_mat(WmPack *p, const WhisperTensor *t) {
    WmMat m = { 0, 0, 0, 0 };
    if (!p->ok || !t) { p->ok = false; return m; }

    /* K must be a whole number of quant blocks: the GEMM's k-tile IS one
     * block, and the matvec assigns one lane per block element. */
    const i32 blk = whisper_type_block(t->dtype);
    if (blk == 0 || t->ne[0] % 32 != 0) { p->ok = false; return m; }

    p->cursor = wm_align(p->cursor);
    m.off = p->cursor;
    m.dtype = t->dtype;
    m.K = t->ne[0];
    m.N = t->ne[1];
    if (p->base) memcpy(p->base + m.off, t->data, (usize)t->nbytes);
    p->cursor += t->nbytes;
    return m;
}

/* Dequantize into sbuf. `n` is the element count; the offset returned is a
 * FLOAT index, because every consumer of sbuf binds it as `device float*`. */
static WmVec wm_pack_vec(WmPack *p, const WhisperTensor *t) {
    WmVec v = { 0, 0 };
    if (!p->ok || !t) { p->ok = false; return v; }

    i64 n = 1;
    for (i32 d = 0; d < t->n_dims; d++) n *= t->ne[d];

    p->cursor = wm_align(p->cursor);
    v.off = p->cursor / sizeof(f32);
    v.n   = (i32)n;
    if (p->base) {
        if (!whisper_dequant_row(t->data, t->dtype, n, (f32 *)(p->base + p->cursor))) {
            p->ok = false;
            return v;
        }
    }
    p->cursor += (u64)n * sizeof(f32);
    return v;
}

static void wm_pack_layer(WmPack *w, WmPack *s, const WhisperLayer *L,
                          WmLayer *out, bool with_cross) {
    out->attn_ln_w = wm_pack_vec(s, L->attn_ln_w);
    out->attn_ln_b = wm_pack_vec(s, L->attn_ln_b);
    out->attn_q_w  = wm_pack_mat(w, L->attn_q_w);
    out->attn_q_b  = wm_pack_vec(s, L->attn_q_b);
    out->attn_k_w  = wm_pack_mat(w, L->attn_k_w);
    out->attn_v_w  = wm_pack_mat(w, L->attn_v_w);
    out->attn_v_b  = wm_pack_vec(s, L->attn_v_b);
    out->attn_o_w  = wm_pack_mat(w, L->attn_o_w);
    out->attn_o_b  = wm_pack_vec(s, L->attn_o_b);

    if (with_cross) {
        out->cross_ln_w = wm_pack_vec(s, L->cross_ln_w);
        out->cross_ln_b = wm_pack_vec(s, L->cross_ln_b);
        out->cross_q_w  = wm_pack_mat(w, L->cross_q_w);
        out->cross_q_b  = wm_pack_vec(s, L->cross_q_b);
        out->cross_k_w  = wm_pack_mat(w, L->cross_k_w);
        out->cross_v_w  = wm_pack_mat(w, L->cross_v_w);
        out->cross_v_b  = wm_pack_vec(s, L->cross_v_b);
        out->cross_o_w  = wm_pack_mat(w, L->cross_o_w);
        out->cross_o_b  = wm_pack_vec(s, L->cross_o_b);
    }

    out->mlp_ln_w = wm_pack_vec(s, L->mlp_ln_w);
    out->mlp_ln_b = wm_pack_vec(s, L->mlp_ln_b);
    out->mlp_0_w  = wm_pack_mat(w, L->mlp_0_w);
    out->mlp_0_b  = wm_pack_vec(s, L->mlp_0_b);
    out->mlp_2_w  = wm_pack_mat(w, L->mlp_2_w);
    out->mlp_2_b  = wm_pack_vec(s, L->mlp_2_b);
}

/* Walk every tensor exactly as the copy pass will. */
static void wm_pack_all(WhisperMetal *m, const WhisperWeights *W,
                        WmPack *w, WmPack *s) {
    m->conv1_w = wm_pack_vec(s, W->e_conv1_w);
    m->conv1_b = wm_pack_vec(s, W->e_conv1_b);
    m->conv2_w = wm_pack_vec(s, W->e_conv2_w);
    m->conv2_b = wm_pack_vec(s, W->e_conv2_b);
    m->e_pos   = wm_pack_vec(s, W->e_pos);
    m->e_ln_w  = wm_pack_vec(s, W->e_ln_w);
    m->e_ln_b  = wm_pack_vec(s, W->e_ln_b);

    for (i32 il = 0; il < m->n_layer_a; il++)
        wm_pack_layer(w, s, &W->enc[il], &m->enc[il], false);

    m->tok_emb = wm_pack_mat(w, W->d_tok_emb);
    m->d_pos   = wm_pack_vec(s, W->d_pos);
    m->d_ln_w  = wm_pack_vec(s, W->d_ln_w);
    m->d_ln_b  = wm_pack_vec(s, W->d_ln_b);

    for (i32 il = 0; il < m->n_layer_t; il++)
        wm_pack_layer(w, s, &W->dec[il], &m->dec[il], true);
}

/* ---- create / free ------------------------------------------------------ */

static id<MTLBuffer> wm_buf(WhisperMetal *m, usize bytes) {
    if (bytes == 0) bytes = 16;
    id<MTLBuffer> buffer = [m->dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (buffer) {
        m->gpu_accounted += buffer.length;
        mem_budget_transient_adjust(MEM_MODEL_GPU, (isize)buffer.length);
    }
    return buffer;
}

static void *wm_alloc(WhisperMetal *m, usize bytes) {
    void *p = calloc(1, bytes);
    if (p) {
        m->cpu_accounted += bytes;
        mem_budget_transient_adjust(MEM_VOICE, (isize)bytes);
    }
    return p;
}

void whisper_metal_free(WhisperMetal *m) {
    if (!m) return;
    @autoreleasepool {
        [m->psoGemm release]; [m->psoMatvec release]; [m->psoConv release];
        [m->psoNorm release]; [m->psoAttn release]; [m->psoAdd release];
        [m->psoGelu release]; [m->psoTransPos release]; [m->psoEmbed release];
        [m->psoKvStore release];

        [m->wbuf release]; [m->sbuf release];
        [m->mel release]; [m->ff release]; [m->x release]; [m->xn release];
        [m->q release]; [m->k release]; [m->v release]; [m->att release];
        [m->cross_k release]; [m->cross_v release];
        [m->self_k release]; [m->self_v release];
        [m->ids release]; [m->logits release];

        [m->lib release]; [m->queue release]; [m->dev release];
    }
    free(m->enc);
    free(m->dec);
    mem_budget_transient_adjust(MEM_VOICE, -(isize)m->cpu_accounted);
    mem_budget_transient_adjust(MEM_MODEL_GPU, -(isize)m->gpu_accounted);
    free(m);
}

WhisperMetal *whisper_metal_create(const WhisperWeights *W, char *err, usize err_cap) {
    if (!W) return NULL;
    if (err && err_cap) err[0] = '\0';

    @autoreleasepool {
        WhisperMetal *m = calloc(1, sizeof *m);
        if (!m) { if (err && err_cap) snprintf(err, err_cap, "out of memory"); return NULL; }
        m->cpu_accounted = sizeof *m;
        mem_budget_transient_adjust(MEM_VOICE, (isize)m->cpu_accounted);

        m->n_state   = W->h.n_audio_state;
        m->n_ctx_a   = W->h.n_audio_ctx;
        m->n_ctx_t   = W->h.n_text_ctx;
        m->n_mels    = W->h.n_mels;
        m->n_head_a  = W->h.n_audio_head;
        m->n_head_t  = W->h.n_text_head;
        m->hd_a      = W->n_audio_head_dim;
        m->hd_t      = W->n_text_head_dim;
        m->n_ff_a    = W->n_audio_ff;
        m->n_ff_t    = W->n_text_ff;
        m->n_vocab   = W->h.n_vocab;
        m->n_layer_a = W->h.n_audio_layer;
        m->n_layer_t = W->h.n_text_layer;

        /* The attention kernel stages its scores in threadgroup memory, so
         * both context lengths have to fit the fixed WM_MAX_KV window. */
        if (m->n_ctx_a > 1536 || m->n_ctx_t > 1536) {
            if (err && err_cap) snprintf(err, err_cap, "model context exceeds the GPU score window");
            whisper_metal_free(m); return NULL;
        }
        if (m->n_ctx_a * 2 != WHISPER_N_FRAMES) {
            if (err && err_cap) snprintf(err, err_cap, "unexpected audio context length");
            whisper_metal_free(m); return NULL;
        }
        /* The attention kernel walks head_dim in 8x8 simdgroup matrices and
         * stages a WM_ATTN_QB x head_dim output tile in threadgroup memory.
         * Every Whisper size has head_dim exactly 64, so this is a guard
         * rather than a real limit — but it has to be a loud one, since a
         * larger head would silently drop the tail of every value vector. */
        if (m->hd_a % 8 != 0 || m->hd_t % 8 != 0 ||
            m->hd_a > WM_ATTN_HD_MAX || m->hd_t > WM_ATTN_HD_MAX) {
            if (err && err_cap) snprintf(err, err_cap, "head dimension unsupported by the GPU kernel");
            whisper_metal_free(m); return NULL;
        }

        m->dev = MTLCreateSystemDefaultDevice();
        if (!m->dev) { if (err && err_cap) snprintf(err, err_cap, "no Metal device"); whisper_metal_free(m); return NULL; }
        m->queue = [m->dev newCommandQueue];
        m->lib   = wm_load_lib(m->dev);
        if (!m->queue || !m->lib) {
            if (err && err_cap) snprintf(err, err_cap, "Metal shader library not found");
            whisper_metal_free(m); return NULL;
        }
        if (!wm_params_match(m->dev, m->queue, m->lib, err, err_cap)) {
            whisper_metal_free(m); return NULL;
        }

        m->psoGemm     = wm_pso(m->dev, m->lib, "k_gemm");
        m->psoMatvec   = wm_pso(m->dev, m->lib, "k_matvec");
        m->psoConv     = wm_pso(m->dev, m->lib, "k_conv1d");
        m->psoNorm     = wm_pso(m->dev, m->lib, "k_layernorm");
        m->psoAttn     = wm_pso(m->dev, m->lib, "k_attention");
        m->psoAdd      = wm_pso(m->dev, m->lib, "k_add");
        m->psoGelu     = wm_pso(m->dev, m->lib, "k_gelu");
        m->psoTransPos = wm_pso(m->dev, m->lib, "k_transpose_add_pos");
        m->psoEmbed    = wm_pso(m->dev, m->lib, "k_embed");
        m->psoKvStore  = wm_pso(m->dev, m->lib, "k_kv_store");
        if (!m->psoGemm || !m->psoMatvec || !m->psoConv || !m->psoNorm ||
            !m->psoAttn || !m->psoAdd || !m->psoGelu || !m->psoTransPos ||
            !m->psoEmbed || !m->psoKvStore) {
            if (err && err_cap) snprintf(err, err_cap, "Metal kernel compilation failed");
            whisper_metal_free(m); return NULL;
        }

        m->enc = wm_alloc(m, (usize)m->n_layer_a * sizeof *m->enc);
        m->dec = wm_alloc(m, (usize)m->n_layer_t * sizeof *m->dec);
        if (!m->enc || !m->dec) {
            if (err && err_cap) snprintf(err, err_cap, "out of memory");
            whisper_metal_free(m); return NULL;
        }

        /* Pass 1: measure. */
        WmPack wp = { NULL, 0, true }, sp = { NULL, 0, true };
        wm_pack_all(m, W, &wp, &sp);
        if (!wp.ok || !sp.ok) {
            if (err && err_cap) snprintf(err, err_cap, "unsupported weight layout for the GPU backend");
            whisper_metal_free(m); return NULL;
        }

        m->wbuf = wm_buf(m, (usize)wp.cursor);
        m->sbuf = wm_buf(m, (usize)sp.cursor);
        if (!m->wbuf || !m->sbuf) {
            if (err && err_cap) snprintf(err, err_cap, "cannot allocate %llu MB of GPU memory",
                                         (unsigned long long)((wp.cursor + sp.cursor) >> 20));
            whisper_metal_free(m); return NULL;
        }

        /* Pass 2: copy. */
        wp = (WmPack){ (u8 *)m->wbuf.contents, 0, true };
        sp = (WmPack){ (u8 *)m->sbuf.contents, 0, true };
        wm_pack_all(m, W, &wp, &sp);
        if (!wp.ok || !sp.ok) {
            if (err && err_cap) snprintf(err, err_cap, "failed to dequantize model weights");
            whisper_metal_free(m); return NULL;
        }

        m->ctx_a_pad = ((m->n_ctx_a + WM_ATTN_QB - 1) / WM_ATTN_QB) * WM_ATTN_QB;
        m->ctx_t_pad = ((m->n_ctx_t + WM_ATTN_QB - 1) / WM_ATTN_QB) * WM_ATTN_QB;

        const usize ns = (usize)m->n_state;
        const usize ta = (usize)m->ctx_a_pad;
        const usize tt = (usize)m->ctx_t_pad;
        const usize ff = (usize)(m->n_ff_a > m->n_ff_t ? m->n_ff_a : m->n_ff_t);

        m->mel     = wm_buf(m, (usize)m->n_mels * WHISPER_N_FRAMES * sizeof(f32));
        /* ff doubles as the first convolution's output: 1280 x 3000 floats
         * fits inside 1500 x 5120 with room to spare. */
        m->ff      = wm_buf(m, ta * ff * sizeof(f32));
        m->x       = wm_buf(m, ta * ns * sizeof(f32));
        m->xn      = wm_buf(m, ta * ns * sizeof(f32));
        m->q       = wm_buf(m, ta * ns * sizeof(f32));
        m->k       = wm_buf(m, ta * ns * sizeof(f32));
        m->v       = wm_buf(m, ta * ns * sizeof(f32));
        m->att     = wm_buf(m, ta * ns * sizeof(f32));
        m->cross_k = wm_buf(m, (usize)m->n_layer_t * ta * ns * sizeof(f32));
        m->cross_v = wm_buf(m, (usize)m->n_layer_t * ta * ns * sizeof(f32));
        m->self_k  = wm_buf(m, (usize)m->n_layer_t * tt * ns * sizeof(f32));
        m->self_v  = wm_buf(m, (usize)m->n_layer_t * tt * ns * sizeof(f32));
        m->ids     = wm_buf(m, (usize)m->n_ctx_t * sizeof(i32));
        m->logits  = wm_buf(m, (usize)m->n_vocab * sizeof(f32));

        if (!m->mel || !m->ff || !m->x || !m->xn || !m->q || !m->k || !m->v ||
            !m->att || !m->cross_k || !m->cross_v || !m->self_k || !m->self_v ||
            !m->ids || !m->logits) {
            if (err && err_cap) snprintf(err, err_cap, "cannot allocate GPU work buffers");
            whisper_metal_free(m); return NULL;
        }

        /* Zero every work buffer once. The padded tail rows are masked out of
         * the softmax, so their scores never matter — but P is zero there and
         * a NaN left in the matching V row would make 0 * NaN poison the
         * output. Metal does not promise fresh buffers are zeroed. */
        id<MTLBuffer> zeroed[] = { m->mel, m->ff, m->x, m->xn, m->q, m->k, m->v,
                                   m->att, m->cross_k, m->cross_v,
                                   m->self_k, m->self_v, m->ids, m->logits };
        for (usize i = 0; i < sizeof zeroed / sizeof zeroed[0]; i++)
            memset(zeroed[i].contents, 0, zeroed[i].length);

        /* ff is aliased as conv1's output; make sure that assumption holds
         * rather than silently corrupting the first layer. */
        if (ta * ff < (usize)m->n_state * WHISPER_N_FRAMES) {
            if (err && err_cap) snprintf(err, err_cap, "convolution scratch does not fit");
            whisper_metal_free(m); return NULL;
        }

        return m;
    }
}

void whisper_metal_decode_reset(WhisperMetal *m) { (void)m; /* cache is position-indexed */ }

/* ---- dispatch helpers --------------------------------------------------- */

/* Whole threadgroups with a ceiling division, never dispatchThreads. The
 * non-uniform variant needs a GPU family this project does not require (the
 * tree still builds and ships for Intel Macs), and every 2-D kernel here
 * already bounds-checks its coordinates, so the padding threads are free. */
static void wm_dispatch2d(id<MTLComputeCommandEncoder> e,
                          NSUInteger nx, NSUInteger ny,
                          NSUInteger tx, NSUInteger ty) {
    [e dispatchThreadgroups:MTLSizeMake((nx + tx - 1) / tx, (ny + ty - 1) / ty, 1)
      threadsPerThreadgroup:MTLSizeMake(tx, ty, 1)];
}

static void wm_norm(WhisperMetal *m, id<MTLComputeCommandEncoder> e,
                    id<MTLBuffer> out, id<MTLBuffer> in,
                    WmVec g, WmVec b, i32 n_tokens) {
    struct WmNorm p = { m->n_state, n_tokens, 0, 0 };
    [e setComputePipelineState:m->psoNorm];
    [e setBuffer:out offset:0 atIndex:0];
    [e setBuffer:in  offset:0 atIndex:1];
    [e setBuffer:m->sbuf offset:g.off * sizeof(f32) atIndex:2];
    [e setBuffer:m->sbuf offset:b.off * sizeof(f32) atIndex:3];
    [e setBytes:&p length:sizeof p atIndex:4];
    [e dispatchThreadgroups:MTLSizeMake((NSUInteger)n_tokens, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
}

/* out[M,N] = in[M,K] . W^T + bias. `bias` may be a zero-length WmVec, in
 * which case none is added (Whisper's K projections have no bias). */
static void wm_mul(WhisperMetal *m, id<MTLComputeCommandEncoder> e,
                   id<MTLBuffer> out, id<MTLBuffer> in, usize in_off,
                   WmMat W, WmVec bias, bool has_bias, i32 M) {
    struct WmGemm p = { M, W.N, W.K, W.dtype, has_bias ? 1 : 0, 0, 0, 0 };

    const bool tiny = (M <= WM_MATVEC_MAX_M);
    [e setComputePipelineState:tiny ? m->psoMatvec : m->psoGemm];
    [e setBuffer:in   offset:in_off atIndex:0];
    [e setBuffer:m->wbuf offset:W.off atIndex:1];
    [e setBuffer:m->sbuf offset:has_bias ? bias.off * sizeof(f32) : 0 atIndex:2];
    [e setBuffer:out  offset:0 atIndex:3];
    [e setBytes:&p length:sizeof p atIndex:4];

    if (tiny) {
        NSUInteger groups = (NSUInteger)((W.N + WM_MATVEC_SG - 1) / WM_MATVEC_SG);
        [e dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(WM_MATVEC_THREADS, 1, 1)];
    } else {
        NSUInteger gx = (NSUInteger)((W.N + WM_BN - 1) / WM_BN);
        NSUInteger gy = (NSUInteger)((M + WM_BM - 1) / WM_BM);
        [e dispatchThreadgroups:MTLSizeMake(gx, gy, 1)
          threadsPerThreadgroup:MTLSizeMake(WM_GEMM_THREADS, 1, 1)];
    }
}

static void wm_attn(WhisperMetal *m, id<MTLComputeCommandEncoder> e,
                    id<MTLBuffer> kbuf, usize k_off,
                    id<MTLBuffer> vbuf, usize v_off,
                    i32 n_q, i32 n_kv, i32 n_head, i32 hd,
                    bool causal, i32 causal_offset) {
    struct WmAttn p = {
        n_q, n_kv, n_head, hd, m->n_state,
        causal ? 1 : 0, causal_offset, 1.0f / sqrtf((f32)hd)
    };
    [e setComputePipelineState:m->psoAttn];
    [e setBuffer:m->q  offset:0 atIndex:0];
    [e setBuffer:kbuf  offset:k_off atIndex:1];
    [e setBuffer:vbuf  offset:v_off atIndex:2];
    [e setBuffer:m->att offset:0 atIndex:3];
    [e setBytes:&p length:sizeof p atIndex:4];
    [e dispatchThreadgroups:MTLSizeMake((NSUInteger)((n_q + WM_ATTN_QB - 1) / WM_ATTN_QB),
                                        (NSUInteger)n_head, 1)
      threadsPerThreadgroup:MTLSizeMake(WM_ATTN_THREADS, 1, 1)];
}

static void wm_add(WhisperMetal *m, id<MTLComputeCommandEncoder> e,
                   id<MTLBuffer> x, id<MTLBuffer> y, i32 n) {
    struct WmElem p = { n, 0, 0, 0 };
    [e setComputePipelineState:m->psoAdd];
    [e setBuffer:x offset:0 atIndex:0];
    [e setBuffer:y offset:0 atIndex:1];
    [e setBytes:&p length:sizeof p atIndex:2];
    [e dispatchThreadgroups:MTLSizeMake((NSUInteger)((n + 255) / 256), 1, 1)
      threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
}

static void wm_gelu(WhisperMetal *m, id<MTLComputeCommandEncoder> e,
                    id<MTLBuffer> x, i32 n) {
    struct WmElem p = { n, 0, 0, 0 };
    [e setComputePipelineState:m->psoGelu];
    [e setBuffer:x offset:0 atIndex:0];
    [e setBytes:&p length:sizeof p atIndex:1];
    [e dispatchThreadgroups:MTLSizeMake((NSUInteger)((n + 255) / 256), 1, 1)
      threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
}

/* ---- encode ------------------------------------------------------------- */

bool whisper_metal_encode(WhisperMetal *m, const f32 *mel) {
    if (!m || !mel) return false;

    @autoreleasepool {
        memcpy(m->mel.contents, mel,
               (usize)m->n_mels * WHISPER_N_FRAMES * sizeof(f32));

        id<MTLCommandBuffer> cb = [m->queue commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];

        const i32 T  = m->n_ctx_a;
        const i32 ns = m->n_state;

        /* conv1: mel -> ff (aliased as the conv scratch), stride 1, GELU. */
        {
            struct WmConv p = { m->n_mels, ns, WHISPER_N_FRAMES, WHISPER_N_FRAMES, 1, 1, 0, 0 };
            [e setComputePipelineState:m->psoConv];
            [e setBuffer:m->mel offset:0 atIndex:0];
            [e setBuffer:m->sbuf offset:m->conv1_w.off * sizeof(f32) atIndex:1];
            [e setBuffer:m->sbuf offset:m->conv1_b.off * sizeof(f32) atIndex:2];
            [e setBuffer:m->ff offset:0 atIndex:3];
            [e setBytes:&p length:sizeof p atIndex:4];
            [e dispatchThreadgroups:MTLSizeMake((WHISPER_N_FRAMES + WM_CONV_THREADS - 1) / WM_CONV_THREADS,
                                                (NSUInteger)ns, 1)
              threadsPerThreadgroup:MTLSizeMake(WM_CONV_THREADS, 1, 1)];
        }
        /* conv2: ff -> q (free until the first layer), stride 2, GELU. */
        {
            struct WmConv p = { ns, ns, WHISPER_N_FRAMES, T, 2, 1, 0, 0 };
            [e setComputePipelineState:m->psoConv];
            [e setBuffer:m->ff offset:0 atIndex:0];
            [e setBuffer:m->sbuf offset:m->conv2_w.off * sizeof(f32) atIndex:1];
            [e setBuffer:m->sbuf offset:m->conv2_b.off * sizeof(f32) atIndex:2];
            [e setBuffer:m->q offset:0 atIndex:3];
            [e setBytes:&p length:sizeof p atIndex:4];
            [e dispatchThreadgroups:MTLSizeMake((NSUInteger)((T + WM_CONV_THREADS - 1) / WM_CONV_THREADS),
                                                (NSUInteger)ns, 1)
              threadsPerThreadgroup:MTLSizeMake(WM_CONV_THREADS, 1, 1)];
        }
        /* channel-major -> token-major, folding in the positional table. */
        {
            struct WmTranspose p = { ns, T, 0, 0 };
            [e setComputePipelineState:m->psoTransPos];
            [e setBuffer:m->q offset:0 atIndex:0];
            [e setBuffer:m->sbuf offset:m->e_pos.off * sizeof(f32) atIndex:1];
            [e setBuffer:m->x offset:0 atIndex:2];
            [e setBytes:&p length:sizeof p atIndex:3];
            wm_dispatch2d(e, (NSUInteger)T, (NSUInteger)ns, 32, 8);
        }

        for (i32 il = 0; il < m->n_layer_a; il++) {
            const WmLayer *L = &m->enc[il];

            wm_norm(m, e, m->xn, m->x, L->attn_ln_w, L->attn_ln_b, T);
            wm_mul(m, e, m->q, m->xn, 0, L->attn_q_w, L->attn_q_b, true,  T);
            wm_mul(m, e, m->k, m->xn, 0, L->attn_k_w, L->attn_q_b, false, T);
            wm_mul(m, e, m->v, m->xn, 0, L->attn_v_w, L->attn_v_b, true,  T);
            wm_attn(m, e, m->k, 0, m->v, 0, T, T, m->n_head_a, m->hd_a, false, 0);
            wm_mul(m, e, m->xn, m->att, 0, L->attn_o_w, L->attn_o_b, true, T);
            wm_add(m, e, m->x, m->xn, T * ns);

            wm_norm(m, e, m->xn, m->x, L->mlp_ln_w, L->mlp_ln_b, T);
            wm_mul(m, e, m->ff, m->xn, 0, L->mlp_0_w, L->mlp_0_b, true, T);
            wm_gelu(m, e, m->ff, T * m->n_ff_a);
            wm_mul(m, e, m->xn, m->ff, 0, L->mlp_2_w, L->mlp_2_b, true, T);
            wm_add(m, e, m->x, m->xn, T * ns);
        }

        /* Final norm lands in xn, which is then the encoder output. x is
         * about to be reused by the decoder for token embeddings. */
        wm_norm(m, e, m->xn, m->x, m->e_ln_w, m->e_ln_b, T);

        for (i32 il = 0; il < m->n_layer_t; il++) {
            const WmLayer *L = &m->dec[il];
            /* Padded stride: the attention kernel reads a whole tile past
             * the last real row, which without padding would spill into
             * the next layer's slice (and past the buffer on the last). */
            const usize off = (usize)il * (usize)m->ctx_a_pad * (usize)ns * sizeof(f32);
            /* Cross K/V are written straight into their per-layer slice of
             * the cache; the GEMM's output offset is the slice base. */
            struct WmGemm pk = { T, L->cross_k_w.N, L->cross_k_w.K, L->cross_k_w.dtype, 0, 0, 0, 0 };
            [e setComputePipelineState:m->psoGemm];
            [e setBuffer:m->xn offset:0 atIndex:0];
            [e setBuffer:m->wbuf offset:L->cross_k_w.off atIndex:1];
            [e setBuffer:m->sbuf offset:0 atIndex:2];
            [e setBuffer:m->cross_k offset:off atIndex:3];
            [e setBytes:&pk length:sizeof pk atIndex:4];
            [e dispatchThreadgroups:MTLSizeMake((NSUInteger)((L->cross_k_w.N + WM_BN - 1) / WM_BN),
                                                (NSUInteger)((T + WM_BM - 1) / WM_BM), 1)
              threadsPerThreadgroup:MTLSizeMake(WM_GEMM_THREADS, 1, 1)];

            struct WmGemm pv = { T, L->cross_v_w.N, L->cross_v_w.K, L->cross_v_w.dtype, 1, 0, 0, 0 };
            [e setComputePipelineState:m->psoGemm];
            [e setBuffer:m->xn offset:0 atIndex:0];
            [e setBuffer:m->wbuf offset:L->cross_v_w.off atIndex:1];
            [e setBuffer:m->sbuf offset:L->cross_v_b.off * sizeof(f32) atIndex:2];
            [e setBuffer:m->cross_v offset:off atIndex:3];
            [e setBytes:&pv length:sizeof pv atIndex:4];
            [e dispatchThreadgroups:MTLSizeMake((NSUInteger)((L->cross_v_w.N + WM_BN - 1) / WM_BN),
                                                (NSUInteger)((T + WM_BM - 1) / WM_BM), 1)
              threadsPerThreadgroup:MTLSizeMake(WM_GEMM_THREADS, 1, 1)];
        }

        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        if (cb.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "whisper_metal: encoder command buffer failed (%ld)\n",
                    (long)cb.status);
            return false;
        }
        m->encoded = true;
        return true;
    }
}

bool whisper_metal_encoder_out(WhisperMetal *m, f32 *dst) {
    if (!m || !dst || !m->encoded) return false;
    memcpy(dst, m->xn.contents, (usize)m->n_ctx_a * (usize)m->n_state * sizeof(f32));
    return true;
}

/* ---- decode ------------------------------------------------------------- */

bool whisper_metal_decode(WhisperMetal *m, const i32 *tokens, i32 n, i32 n_past,
                          f32 *logits) {
    if (!m || !tokens || n <= 0 || !logits || !m->encoded) return false;
    if (n_past < 0 || n_past + n > m->n_ctx_t) return false;
    for (i32 i = 0; i < n; i++) if (tokens[i] < 0 || tokens[i] >= m->n_vocab) return false;

    @autoreleasepool {
        memcpy(m->ids.contents, tokens, (usize)n * sizeof(i32));

        id<MTLCommandBuffer> cb = [m->queue commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];

        const i32 ns = m->n_state;
        const i32 Ta = m->n_ctx_a;

        {
            struct WmEmbed p = { ns, n, n_past, m->tok_emb.dtype };
            [e setComputePipelineState:m->psoEmbed];
            [e setBuffer:m->wbuf offset:m->tok_emb.off atIndex:0];
            [e setBuffer:m->ids offset:0 atIndex:1];
            [e setBuffer:m->sbuf offset:m->d_pos.off * sizeof(f32) atIndex:2];
            [e setBuffer:m->x offset:0 atIndex:3];
            [e setBytes:&p length:sizeof p atIndex:4];
            wm_dispatch2d(e, (NSUInteger)ns, (NSUInteger)n, 64, 4);
        }

        for (i32 il = 0; il < m->n_layer_t; il++) {
            const WmLayer *L = &m->dec[il];
            const usize self_off  = (usize)il * (usize)m->ctx_t_pad * (usize)ns * sizeof(f32);
            const usize cross_off = (usize)il * (usize)m->ctx_a_pad * (usize)ns * sizeof(f32);

            /* --- causal self-attention --- */
            wm_norm(m, e, m->xn, m->x, L->attn_ln_w, L->attn_ln_b, n);
            wm_mul(m, e, m->q, m->xn, 0, L->attn_q_w, L->attn_q_b, true,  n);
            wm_mul(m, e, m->k, m->xn, 0, L->attn_k_w, L->attn_q_b, false, n);
            wm_mul(m, e, m->v, m->xn, 0, L->attn_v_w, L->attn_v_b, true,  n);

            struct WmEmbed kv = { ns, n, n_past, 0 };
            [e setComputePipelineState:m->psoKvStore];
            [e setBuffer:m->k offset:0 atIndex:0];
            [e setBuffer:m->self_k offset:self_off atIndex:1];
            [e setBytes:&kv length:sizeof kv atIndex:2];
            wm_dispatch2d(e, (NSUInteger)ns, (NSUInteger)n, 64, 4);

            [e setComputePipelineState:m->psoKvStore];
            [e setBuffer:m->v offset:0 atIndex:0];
            [e setBuffer:m->self_v offset:self_off atIndex:1];
            [e setBytes:&kv length:sizeof kv atIndex:2];
            wm_dispatch2d(e, (NSUInteger)ns, (NSUInteger)n, 64, 4);

            wm_attn(m, e, m->self_k, self_off, m->self_v, self_off,
                    n, n_past + n, m->n_head_t, m->hd_t, true, n_past);
            wm_mul(m, e, m->xn, m->att, 0, L->attn_o_w, L->attn_o_b, true, n);
            wm_add(m, e, m->x, m->xn, n * ns);

            /* --- cross-attention --- */
            wm_norm(m, e, m->xn, m->x, L->cross_ln_w, L->cross_ln_b, n);
            wm_mul(m, e, m->q, m->xn, 0, L->cross_q_w, L->cross_q_b, true, n);
            wm_attn(m, e, m->cross_k, cross_off, m->cross_v, cross_off,
                    n, Ta, m->n_head_t, m->hd_t, false, 0);
            wm_mul(m, e, m->xn, m->att, 0, L->cross_o_w, L->cross_o_b, true, n);
            wm_add(m, e, m->x, m->xn, n * ns);

            /* --- MLP --- */
            wm_norm(m, e, m->xn, m->x, L->mlp_ln_w, L->mlp_ln_b, n);
            wm_mul(m, e, m->ff, m->xn, 0, L->mlp_0_w, L->mlp_0_b, true, n);
            wm_gelu(m, e, m->ff, n * m->n_ff_t);
            wm_mul(m, e, m->xn, m->ff, 0, L->mlp_2_w, L->mlp_2_b, true, n);
            wm_add(m, e, m->x, m->xn, n * ns);
        }

        wm_norm(m, e, m->xn, m->x, m->d_ln_w, m->d_ln_b, n);

        /* Tied LM head, last token only. */
        wm_mul(m, e, m->logits, m->xn, (usize)(n - 1) * (usize)ns * sizeof(f32),
               m->tok_emb, (WmVec){ 0, 0 }, false, 1);

        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        if (cb.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "whisper_metal: decoder command buffer failed (%ld)\n",
                    (long)cb.status);
            return false;
        }

        memcpy(logits, m->logits.contents, (usize)m->n_vocab * sizeof(f32));
        return true;
    }
}
