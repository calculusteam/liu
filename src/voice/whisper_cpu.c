/*
 * Liu - Whisper forward pass, portable C reference.
 *
 * Activation layout is token-major throughout: x[t*n_state + c]. That is
 * ggml's ne = [n_state, n_tokens] with ne[0] fastest, so weight rows and
 * activation rows are both contiguous over the reduction axis and every
 * matmul is a plain dot product.
 *
 * Weights are dequantized whole into one reused scratch buffer before each
 * matmul rather than per row inside it. Per-row dequant is the right call
 * for a single-token decode (llm_model.c does exactly that) but wrong here:
 * the encoder multiplies each weight row against 1500 activation rows, so
 * hoisting the dequant out of the token loop turns it from a hot cost into
 * a rounding error.
 */
#include "voice/whisper_cpu.h"
#include "voice/whisper_mel.h"
#include "voice/whisper_quant.h"
#include "voice/whisper_simd.h"
#include "core/checked.h"
#include "core/mem_budget.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WC_LN_EPS  1e-5f

/* Tokens accumulated against one weight row at a time. This is the matmul's
 * whole performance story: with one token per pass the weight matrix (up to
 * 26 MB dequantized) is re-read from memory for every one of the encoder's
 * 1500 tokens and the kernel is hopelessly bandwidth-bound. Holding eight
 * accumulators turns eight loads of the weight row into one, and the inner
 * loop still vectorizes because all nine streams are contiguous over k. */
#define WC_TOKEN_BLOCK 8

#include "voice/whisper_pool.h"

/* ========================================================================
 * Backend state
 * ===================================================================== */

struct WhisperCpu {
    usize accounted;
    WhisperCpuCancelFn cancel;
    void *cancel_user;
    const WhisperWeights *w;
    i32 n_threads;
    WcPool pool;

    i32 n_state, n_ctx_a, n_ctx_t, n_mels, n_head_a, n_head_t, hd_a, hd_t;
    i32 n_ff_a, n_ff_t, n_vocab, n_layer_a, n_layer_t;

    f32 *wdq;        /* dequantized-weight scratch, largest tensor          */
    usize wdq_cap;

    f32 *conv_a;     /* [n_state * WHISPER_N_FRAMES] channel-major          */
    f32 *conv_b;
    f32 *enc;        /* [n_ctx_a * n_state] token-major, encoder output     */

    /* Encoder/decoder working set, sized for the encoder's 1500 tokens. */
    f32 *x, *xn, *q, *k, *v, *att, *ff;

    /* Per-thread score scratch for attention: n_threads * n_ctx_a floats. */
    f32 *scores;

    /* Cross-attention K/V, [n_layer_t][n_ctx_a][n_state]. */
    f32 *cross_k, *cross_v;

    /* Self-attention KV cache, [n_layer_t][n_ctx_t][n_state]. */
    f32 *self_k, *self_v;
    i32  kv_len;
};

static f32 *wc_alloc(WhisperCpu *c, usize n, bool *ok) {
    usize bytes = 0;
    if (!checked_mul_usize(n, sizeof(f32), &bytes)) { *ok = false; return NULL; }
    f32 *p = malloc(bytes);
    if (!p) *ok = false;
    else {
        c->accounted += bytes;
        mem_budget_transient_adjust(MEM_VOICE, (isize)bytes);
    }
    return p;
}

void whisper_cpu_free(WhisperCpu *c) {
    if (!c) return;
    wc_pool_destroy(&c->pool);
    free(c->wdq); free(c->conv_a); free(c->conv_b); free(c->enc);
    free(c->x); free(c->xn); free(c->q); free(c->k); free(c->v);
    free(c->att); free(c->ff); free(c->scores);
    free(c->cross_k); free(c->cross_v); free(c->self_k); free(c->self_v);
    mem_budget_transient_adjust(MEM_VOICE, -(isize)c->accounted);
    free(c);
}

WhisperCpu *whisper_cpu_create(const WhisperWeights *w, i32 n_threads,
                               char *err, usize err_cap) {
    if (!w) return NULL;
    if (err && err_cap) err[0] = '\0';

    WhisperCpu *c = calloc(1, sizeof *c);
    if (!c) { if (err && err_cap) snprintf(err, err_cap, "out of memory"); return NULL; }
    c->accounted = sizeof *c;
    mem_budget_transient_adjust(MEM_VOICE, (isize)c->accounted);

    c->w = w;
    if (n_threads <= 0) {
        long hw = sysconf(_SC_NPROCESSORS_ONLN);
        /* Leave one logical CPU available for the terminal and its PTY. */
        n_threads = hw > 2 ? (i32)hw - 1 : hw > 0 ? (i32)hw : 4;
    }
    if (n_threads > WC_MAX_THREADS) n_threads = WC_MAX_THREADS;
    c->n_threads = n_threads;

    c->n_state   = w->h.n_audio_state;
    c->n_ctx_a   = w->h.n_audio_ctx;
    c->n_ctx_t   = w->h.n_text_ctx;
    c->n_mels    = w->h.n_mels;
    c->n_head_a  = w->h.n_audio_head;
    c->n_head_t  = w->h.n_text_head;
    c->hd_a      = w->n_audio_head_dim;
    c->hd_t      = w->n_text_head_dim;
    c->n_ff_a    = w->n_audio_ff;
    c->n_ff_t    = w->n_text_ff;
    c->n_vocab   = w->h.n_vocab;
    c->n_layer_a = w->h.n_audio_layer;
    c->n_layer_t = w->h.n_text_layer;

    /* The encoder consumes exactly WHISPER_N_FRAMES mel frames and halves
     * them in conv2; a model whose audio context disagrees would silently
     * read past the convolution output. */
    if (c->n_ctx_a * 2 != WHISPER_N_FRAMES) {
        if (err && err_cap) snprintf(err, err_cap, "unexpected audio context length");
        whisper_cpu_free(c);
        return NULL;
    }

    bool ok = true;
    const usize ns = (usize)c->n_state;
    const usize ta = (usize)c->n_ctx_a;
    /* The attention scratch is indexed by the KEY count of whichever
     * attention is running: n_ctx_a for the encoder and cross-attention, but
     * up to n_ctx_t for decoder self-attention. Sized by the larger of the
     * two, so a model with n_text_ctx > n_audio_ctx cannot write past the
     * end once generation crosses n_ctx_a tokens. Every real Whisper has 448
     * < 1500, so this costs nothing there. */
    const usize tk = (usize)(c->n_ctx_t > c->n_ctx_a ? c->n_ctx_t : c->n_ctx_a);

    /* The dequant scratch holds one whole weight matrix. The token
     * embedding is deliberately NOT sized for: at 51866 x 1280 it is 266 MB
     * of f32, and both the paths that touch it (row lookup and the tied LM
     * head) stream it a row at a time instead. */
    usize big = (usize)c->n_ff_a * ns;
    usize mlp_t = (usize)c->n_ff_t * ns;
    usize pos_a = ta * ns;
    if (mlp_t > big) big = mlp_t;
    if (pos_a > big) big = pos_a;
    c->wdq_cap = big;
    c->wdq = wc_alloc(c, big, &ok);

    c->conv_a  = wc_alloc(c, ns * WHISPER_N_FRAMES, &ok);
    c->conv_b  = wc_alloc(c, ns * WHISPER_N_FRAMES, &ok);
    c->enc     = wc_alloc(c, ta * ns, &ok);
    c->x       = wc_alloc(c, ta * ns, &ok);
    c->xn      = wc_alloc(c, ta * ns, &ok);
    c->q       = wc_alloc(c, ta * ns, &ok);
    c->k       = wc_alloc(c, ta * ns, &ok);
    c->v       = wc_alloc(c, ta * ns, &ok);
    c->att     = wc_alloc(c, ta * ns, &ok);
    c->ff      = wc_alloc(c, ta * (usize)(c->n_ff_a > c->n_ff_t ? c->n_ff_a : c->n_ff_t), &ok);
    c->scores  = wc_alloc(c, (usize)c->n_threads * tk, &ok);
    c->cross_k = wc_alloc(c, (usize)c->n_layer_t * ta * ns, &ok);
    c->cross_v = wc_alloc(c, (usize)c->n_layer_t * ta * ns, &ok);
    c->self_k  = wc_alloc(c, (usize)c->n_layer_t * (usize)c->n_ctx_t * ns, &ok);
    c->self_v  = wc_alloc(c, (usize)c->n_layer_t * (usize)c->n_ctx_t * ns, &ok);

    if (!ok) {
        if (err && err_cap) snprintf(err, err_cap, "out of memory");
        whisper_cpu_free(c);
        return NULL;
    }
    wc_pool_init(&c->pool, c->n_threads);
    return c;
}

const f32 *whisper_cpu_encoder_out(const WhisperCpu *c) { return c ? c->enc : NULL; }

void whisper_cpu_decode_reset(WhisperCpu *c) { if (c) c->kv_len = 0; }

/* ========================================================================
 * Primitives
 * ===================================================================== */

static inline f32 wc_gelu(f32 x) {
    /* tanh approximation — the one Whisper was trained with, and the one
     * ggml_gelu implements. The erf form differs enough to matter. */
    const f32 a = 0.044715f;
    const f32 s = 0.79788456080286535587989211986876f;   /* sqrt(2/pi) */
    return 0.5f * x * (1.0f + tanhf(s * x * (1.0f + a * x * x)));
}

/* Every dot product in this file goes through whisper_simd.h: a scalar loop
 * is far too slow for an encoder of ~1.9 TFLOP a window. */
static inline f32 wc_dot(const f32 *a, const f32 *b, i32 n) {
    return wsimd_dot(a, b, n);
}

/* Dequantize a whole tensor into c->wdq. Returns NULL if it does not fit or
 * the dtype is unsupported. */
typedef struct { WhisperCpu *c; const WhisperTensor *t; i64 per; atomic_bool ok; } WcDqCtx;

static void wc_dq_slice(void *p, i32 begin, i32 end, i32 ith) {
    (void)ith;
    WcDqCtx *d = p;
    const i32  blk = whisper_type_block(d->t->dtype);
    const i32  bb  = whisper_type_block_bytes(d->t->dtype);
    for (i32 r = begin; r < end; r++) {
        const u8 *src = d->t->data + (i64)r * (d->per / blk) * bb;
        if (!whisper_dequant_row(src, d->t->dtype, d->per, d->c->wdq + (i64)r * d->per))
            atomic_store_explicit(&d->ok, false, memory_order_relaxed);
    }
}

static const f32 *wc_dequant(WhisperCpu *c, const WhisperTensor *t) {
    if (!t) return NULL;
    i64 n = 1;
    for (i32 d = 0; d < t->n_dims; d++) n *= t->ne[d];
    if ((usize)n > c->wdq_cap) return NULL;

    /* Split by rows so the slices stay block-aligned; ne[0] is always a
     * multiple of 32 for the quantized tensors in a Whisper model. */
    const i32 blk = whisper_type_block(t->dtype);
    i32 rows = (t->ne[0] % blk == 0) ? (i32)(n / t->ne[0]) : 1;
    if (rows < 1) rows = 1;
    i64 per = n / rows;

    WcDqCtx d = { c, t, per, true };
    wc_par_for(&c->pool, rows, wc_dq_slice, &d);
    return atomic_load_explicit(&d.ok, memory_order_relaxed) ? c->wdq : NULL;
}

/* y[t*N + n] = bias[n] + dot(x[t*K .. ], W[n*K .. ]) */
typedef struct {
    const f32 *x, *wq, *bias;
    f32 *y;
    i32 M, K, N;
} WcMatmul;

static void wc_matmul_slice(void *p, i32 begin, i32 end, i32 ith) {
    (void)ith;
    WcMatmul *m = p;
    const i32 K = m->K, N = m->N;

    for (i32 tb = begin; tb < end; tb++) {
        const i32 t0 = tb * WC_TOKEN_BLOCK;
        i32 t1 = t0 + WC_TOKEN_BLOCK;
        if (t1 > m->M) t1 = m->M;
        const i32 span = t1 - t0;

        for (i32 n = 0; n < N; n++) {
            const f32 *wrow = m->wq + (i64)n * K;
            const f32  b    = m->bias ? m->bias[n] : 0.0f;

            if (span == WC_TOKEN_BLOCK) {
                /* Eight tokens against one weight row: the row is loaded once
                 * and reused, and every load on both sides is contiguous and
                 * vector-width. */
                f32 acc[WC_TOKEN_BLOCK];
                wsimd_dot8(m->x + (i64)t0 * K, K, wrow, K, acc);
                for (i32 u = 0; u < WC_TOKEN_BLOCK; u++) {
                    m->y[(i64)(t0 + u) * N + n] = b + acc[u];
                }
            } else {
                for (i32 t = t0; t < t1; t++) {
                    m->y[(i64)t * N + n] = b + wc_dot(m->x + (i64)t * K, wrow, K);
                }
            }
        }
    }
}

/* Multiply with a quantized/half weight tensor. `bias` may be NULL. Both
 * tensors are dequantized through c->wdq, so the bias is materialized first
 * (it is small) and the weight second. */
static bool wc_matmul(WhisperCpu *c, const f32 *x, i32 M,
                      const WhisperTensor *W, const WhisperTensor *B,
                      f32 *y) {
    const i32 K = W->ne[0];
    const i32 N = W->ne[1];

    f32 *bias = NULL;
    f32  bias_stack[8192];
    if (B) {
        if (B->ne[0] != N) return false;
        if ((usize)N > sizeof bias_stack / sizeof *bias_stack) return false;
        if (!whisper_dequant_row(B->data, B->dtype, N, bias_stack)) return false;
        bias = bias_stack;
    }

    const f32 *wq = wc_dequant(c, W);
    if (!wq) return false;

    WcMatmul m = { x, wq, bias, y, M, K, N };
    const i32 blocks = (M + WC_TOKEN_BLOCK - 1) / WC_TOKEN_BLOCK;
    wc_par_for(&c->pool, blocks, wc_matmul_slice, &m);
    return true;
}

/* LayerNorm with affine, in place over rows of `n` floats. */
typedef struct { f32 *x; const f32 *g, *b; i32 n; } WcNorm;

static void wc_norm_slice(void *p, i32 begin, i32 end, i32 ith) {
    (void)ith;
    WcNorm *L = p;
    for (i32 t = begin; t < end; t++) {
        f32 *row = L->x + (i64)t * L->n;
        f64 sum = 0;
        for (i32 i = 0; i < L->n; i++) sum += row[i];
        const f32 mean = (f32)(sum / L->n);
        f64 var = 0;
        for (i32 i = 0; i < L->n; i++) { f64 d = row[i] - mean; var += d * d; }
        const f32 inv = 1.0f / sqrtf((f32)(var / L->n) + WC_LN_EPS);
        for (i32 i = 0; i < L->n; i++) row[i] = (row[i] - mean) * inv * L->g[i] + L->b[i];
    }
}

static bool wc_layernorm(WhisperCpu *c, f32 *x, i32 M,
                         const WhisperTensor *gw, const WhisperTensor *bw) {
    const i32 n = c->n_state;
    /* n_state is 1280 for every Whisper size; the stack copies stay small. */
    static _Thread_local f32 g[8192], b[8192];
    if ((usize)n > sizeof g / sizeof *g) return false;
    if (!whisper_dequant_row(gw->data, gw->dtype, n, g)) return false;
    if (!whisper_dequant_row(bw->data, bw->dtype, n, b)) return false;
    WcNorm L = { x, g, b, n };
    wc_par_for(&c->pool, M, wc_norm_slice, &L);
    return true;
}

/* ========================================================================
 * Encoder
 * ===================================================================== */

/* conv1d over channel-major data: in[ic*T_in + t], out[oc*T_out + t].
 * Kernel 3, padding 1 ("same" for stride 1, halved for stride 2). */
typedef struct {
    const f32 *in, *wq, *bias;
    f32 *out;
    i32 in_ch, out_ch, t_in, t_out, stride;
} WcConv;

static void wc_conv_slice(void *p, i32 begin, i32 end, i32 ith) {
    (void)ith;
    WcConv *k = p;
    for (i32 oc = begin; oc < end; oc++) {
        const f32 *w = k->wq + (i64)oc * k->in_ch * 3;
        f32 *o = k->out + (i64)oc * k->t_out;
        const f32 b = k->bias[oc];
        for (i32 t = 0; t < k->t_out; t++) o[t] = b;
        for (i32 ic = 0; ic < k->in_ch; ic++) {
            const f32 *x = k->in + (i64)ic * k->t_in;
            const f32 w0 = w[ic * 3 + 0], w1 = w[ic * 3 + 1], w2 = w[ic * 3 + 2];
            for (i32 t = 0; t < k->t_out; t++) {
                const i32 c0 = t * k->stride;          /* centre - 1 + 1 = t*s */
                f32 acc = 0;
                if (c0 - 1 >= 0)         acc += w0 * x[c0 - 1];
                if (c0     < k->t_in)    acc += w1 * x[c0];
                if (c0 + 1 < k->t_in)    acc += w2 * x[c0 + 1];
                o[t] += acc;
            }
        }
    }
}

static bool wc_conv1d(WhisperCpu *c, const f32 *in, i32 in_ch, i32 t_in,
                      const WhisperTensor *W, const WhisperTensor *B,
                      i32 stride, f32 *out, i32 out_ch, i32 t_out) {
    static _Thread_local f32 bias[8192];
    if ((usize)out_ch > sizeof bias / sizeof *bias) return false;
    if (!whisper_dequant_row(B->data, B->dtype, out_ch, bias)) return false;

    const f32 *wq = wc_dequant(c, W);
    if (!wq) return false;

    WcConv k = { in, wq, bias, out, in_ch, out_ch, t_in, t_out, stride };
    wc_par_for(&c->pool, out_ch, wc_conv_slice, &k);
    return true;
}

/* Multi-head attention over `q`/`k`/`v` laid out token-major, writing into
 * `out`. `n_q` queries attend to `n_kv` keys; `causal` masks j > i + offset.
 * The scale is applied to the scores, which is algebraically what whisper
 * does by pre-scaling Q and K by d^-1/4 each. */
typedef struct {
    const f32 *q, *k, *v;
    f32 *out, *scratch;
    i32 n_q, n_kv, n_head, hd, n_state, scratch_stride;
    f32 scale;
    bool causal;
    i32 causal_offset;
} WcAttn;

static void wc_attn_slice(void *p, i32 begin, i32 end, i32 ith) {
    WcAttn *a = p;
    f32 *sc = a->scratch + (i64)ith * a->scratch_stride;

    for (i32 unit = begin; unit < end; unit++) {
        const i32 h = unit / a->n_q;
        const i32 i = unit % a->n_q;

        const f32 *qv = a->q + (i64)i * a->n_state + (i64)h * a->hd;

        i32 last = a->n_kv;
        if (a->causal) {
            last = a->causal_offset + i + 1;
            if (last > a->n_kv) last = a->n_kv;
        }

        f32 mx = -INFINITY;
        for (i32 j = 0; j < last; j++) {
            f32 s = a->scale * wc_dot(qv, a->k + (i64)j * a->n_state + (i64)h * a->hd, a->hd);
            sc[j] = s;
            if (s > mx) mx = s;
        }
        f32 sum = 0;
        for (i32 j = 0; j < last; j++) { sc[j] = expf(sc[j] - mx); sum += sc[j]; }
        const f32 inv = sum > 0 ? 1.0f / sum : 0.0f;

        f32 *o = a->out + (i64)i * a->n_state + (i64)h * a->hd;
        for (i32 d = 0; d < a->hd; d++) o[d] = 0;
        for (i32 j = 0; j < last; j++) {
            const f32 wgt = sc[j] * inv;
            if (wgt == 0.0f) continue;
            const f32 *vv = a->v + (i64)j * a->n_state + (i64)h * a->hd;
            wsimd_axpy(o, vv, wgt, a->hd);
        }
    }
}

static void wc_attention(WhisperCpu *c, const f32 *q, const f32 *k, const f32 *v,
                         i32 n_q, i32 n_kv, i32 n_head, i32 hd, f32 scale,
                         bool causal, i32 causal_offset, f32 *out) {
    const i32 stride = c->n_ctx_t > c->n_ctx_a ? c->n_ctx_t : c->n_ctx_a;
    if (n_kv > stride) return;   /* cannot happen after the allocation above; belt */
    WcAttn a = { q, k, v, out, c->scores, n_q, n_kv, n_head, hd, c->n_state,
                 stride, scale, causal, causal_offset };
    wc_par_for(&c->pool, n_head * n_q, wc_attn_slice, &a);
}

typedef struct { f32 *x; const f32 *y; i64 n; } WcAdd;
static void wc_add_slice(void *p, i32 begin, i32 end, i32 ith) {
    (void)ith;
    WcAdd *a = p;
    for (i32 i = begin; i < end; i++) a->x[i] += a->y[i];
}

typedef struct { f32 *x; } WcGelu;
static void wc_gelu_slice(void *p, i32 begin, i32 end, i32 ith) {
    (void)ith;
    WcGelu *g = p;
    for (i32 i = begin; i < end; i++) g->x[i] = wc_gelu(g->x[i]);
}

void whisper_cpu_set_cancel(WhisperCpu *c, WhisperCpuCancelFn fn, void *user) {
    if (c) { c->cancel = fn; c->cancel_user = user; }
}

static bool wc_cancelled(WhisperCpu *c) {
    return c->cancel && c->cancel(c->cancel_user);
}

bool whisper_cpu_encode(WhisperCpu *c, const f32 *mel) {
    if (!c || !mel) return false;
    if (wc_cancelled(c)) return false;
    const WhisperWeights *W = c->w;
    const i32 ns = c->n_state;
    const i32 T  = c->n_ctx_a;

    /* --- convolutional stem -------------------------------------------- */
    if (!wc_conv1d(c, mel, c->n_mels, WHISPER_N_FRAMES, W->e_conv1_w, W->e_conv1_b,
                   1, c->conv_a, ns, WHISPER_N_FRAMES)) return false;
    { WcGelu g = { c->conv_a };
      wc_par_for(&c->pool, ns * WHISPER_N_FRAMES, wc_gelu_slice, &g); }

    if (!wc_conv1d(c, c->conv_a, ns, WHISPER_N_FRAMES, W->e_conv2_w, W->e_conv2_b,
                   2, c->conv_b, ns, T)) return false;
    { WcGelu g = { c->conv_b };
      wc_par_for(&c->pool, ns * T, wc_gelu_slice, &g); }

    /* --- transpose to token-major and add the positional embedding ------ */
    {
        const f32 *pos = wc_dequant(c, W->e_pos);
        if (!pos) return false;
        /* wdq is about to be reused by the first matmul, so fold the
         * positional embedding in during the transpose rather than keeping
         * a pointer to it. */
        for (i32 t = 0; t < T; t++) {
            f32 *dst = c->x + (i64)t * ns;
            const f32 *p = pos + (i64)t * ns;
            for (i32 ch = 0; ch < ns; ch++) dst[ch] = c->conv_b[(i64)ch * T + t] + p[ch];
        }
    }

    /* --- 32 encoder blocks ---------------------------------------------- */
    const f32 scale_a = 1.0f / sqrtf((f32)c->hd_a);
    for (i32 il = 0; il < c->n_layer_a; il++) {
        if (wc_cancelled(c)) return false;
        const WhisperLayer *L = &W->enc[il];

        memcpy(c->xn, c->x, (usize)T * (usize)ns * sizeof(f32));
        if (!wc_layernorm(c, c->xn, T, L->attn_ln_w, L->attn_ln_b)) return false;

        if (!wc_matmul(c, c->xn, T, L->attn_q_w, L->attn_q_b, c->q)) return false;
        if (!wc_matmul(c, c->xn, T, L->attn_k_w, NULL,        c->k)) return false;
        if (!wc_matmul(c, c->xn, T, L->attn_v_w, L->attn_v_b, c->v)) return false;

        wc_attention(c, c->q, c->k, c->v, T, T, c->n_head_a, c->hd_a,
                     scale_a, false, 0, c->att);
        if (wc_cancelled(c)) return false;

        if (!wc_matmul(c, c->att, T, L->attn_o_w, L->attn_o_b, c->xn)) return false;
        { WcAdd a = { c->x, c->xn, (i64)T * ns };
          wc_par_for(&c->pool, T * ns, wc_add_slice, &a); }

        memcpy(c->xn, c->x, (usize)T * (usize)ns * sizeof(f32));
        if (!wc_layernorm(c, c->xn, T, L->mlp_ln_w, L->mlp_ln_b)) return false;
        if (!wc_matmul(c, c->xn, T, L->mlp_0_w, L->mlp_0_b, c->ff)) return false;
        { WcGelu g = { c->ff };
          wc_par_for(&c->pool, T * c->n_ff_a, wc_gelu_slice, &g); }
        if (!wc_matmul(c, c->ff, T, L->mlp_2_w, L->mlp_2_b, c->xn)) return false;
        { WcAdd a = { c->x, c->xn, (i64)T * ns };
          wc_par_for(&c->pool, T * ns, wc_add_slice, &a); }
    }

    if (!wc_layernorm(c, c->x, T, W->e_ln_w, W->e_ln_b)) return false;
    memcpy(c->enc, c->x, (usize)T * (usize)ns * sizeof(f32));

    /* --- cross-attention K/V for every decoder layer -------------------- */
    for (i32 il = 0; il < c->n_layer_t; il++) {
        if (wc_cancelled(c)) return false;
        const WhisperLayer *L = &W->dec[il];
        f32 *ck = c->cross_k + (i64)il * T * ns;
        f32 *cv = c->cross_v + (i64)il * T * ns;
        if (!wc_matmul(c, c->enc, T, L->cross_k_w, NULL,         ck)) return false;
        if (!wc_matmul(c, c->enc, T, L->cross_v_w, L->cross_v_b, cv)) return false;
    }

    c->kv_len = 0;
    return true;
}

/* ========================================================================
 * Decoder
 * ===================================================================== */

/* Tied LM head. The embedding table is streamed one row at a time into a
 * per-thread buffer: it is 266 MB dequantized, so materializing it to reuse
 * the general matmul would dominate a decode that only needs one dot per
 * row. */
typedef struct {
    const f32           *x;        /* [n_state] final hidden state */
    const WhisperTensor *emb;
    f32                 *logits;
    i32                  n_state;
    atomic_bool          ok;   /* cleared from any lane, like WcDqCtx.ok */
} WcLmHead;

static void wc_lm_head_slice(void *p, i32 begin, i32 end, i32 ith) {
    (void)ith;
    WcLmHead *L = p;
    const i32 ns  = L->n_state;
    const i32 blk = whisper_type_block(L->emb->dtype);
    const i32 bb  = whisper_type_block_bytes(L->emb->dtype);
    if (blk == 0 || ns % blk != 0) { atomic_store_explicit(&L->ok, false, memory_order_relaxed); return; }

    f32 row[8192];
    if ((usize)ns > sizeof row / sizeof *row) { atomic_store_explicit(&L->ok, false, memory_order_relaxed); return; }

    for (i32 v = begin; v < end; v++) {
        const u8 *src = L->emb->data + (i64)v * (ns / blk) * bb;
        if (!whisper_dequant_row(src, L->emb->dtype, ns, row)) { atomic_store_explicit(&L->ok, false, memory_order_relaxed); return; }
        L->logits[v] = wc_dot(L->x, row, ns);
    }
}

bool whisper_cpu_decode(WhisperCpu *c, const i32 *tokens, i32 n, i32 n_past,
                        f32 *logits) {
    if (!c || !tokens || n <= 0 || !logits) return false;
    if (n_past < 0 || n_past + n > c->n_ctx_t) return false;

    const WhisperWeights *W = c->w;
    const i32 ns = c->n_state;
    const i32 Ta = c->n_ctx_a;

    /* --- token + position embedding ------------------------------------- */
    {
        const i32 blk = whisper_type_block(W->d_tok_emb->dtype);
        const i32 bb  = whisper_type_block_bytes(W->d_tok_emb->dtype);
        if (blk == 0 || ns % blk != 0) return false;
        const f32 *pos = wc_dequant(c, W->d_pos);
        if (!pos) return false;
        /* One row at a time out of the (huge) embedding table — dequantizing
         * all 51866 rows to fetch four of them would cost more than the
         * whole decode. */
        for (i32 i = 0; i < n; i++) {
            const i32 id = tokens[i];
            if (id < 0 || id >= c->n_vocab) return false;
            f32 *dst = c->x + (i64)i * ns;
            const u8 *src = W->d_tok_emb->data + (i64)id * (ns / blk) * bb;
            if (!whisper_dequant_row(src, W->d_tok_emb->dtype, ns, dst)) return false;
            const f32 *p = pos + (i64)(n_past + i) * ns;
            for (i32 ch = 0; ch < ns; ch++) dst[ch] += p[ch];
        }
    }

    const f32 scale_t = 1.0f / sqrtf((f32)c->hd_t);

    for (i32 il = 0; il < c->n_layer_t; il++) {
        const WhisperLayer *L = &W->dec[il];

        /* ---- causal self-attention ------------------------------------ */
        memcpy(c->xn, c->x, (usize)n * (usize)ns * sizeof(f32));
        if (!wc_layernorm(c, c->xn, n, L->attn_ln_w, L->attn_ln_b)) return false;

        if (!wc_matmul(c, c->xn, n, L->attn_q_w, L->attn_q_b, c->q)) return false;
        if (!wc_matmul(c, c->xn, n, L->attn_k_w, NULL,        c->k)) return false;
        if (!wc_matmul(c, c->xn, n, L->attn_v_w, L->attn_v_b, c->v)) return false;

        f32 *sk = c->self_k + (i64)il * c->n_ctx_t * ns;
        f32 *sv = c->self_v + (i64)il * c->n_ctx_t * ns;
        memcpy(sk + (i64)n_past * ns, c->k, (usize)n * (usize)ns * sizeof(f32));
        memcpy(sv + (i64)n_past * ns, c->v, (usize)n * (usize)ns * sizeof(f32));

        wc_attention(c, c->q, sk, sv, n, n_past + n, c->n_head_t, c->hd_t,
                     scale_t, true, n_past, c->att);

        if (!wc_matmul(c, c->att, n, L->attn_o_w, L->attn_o_b, c->xn)) return false;
        { WcAdd a = { c->x, c->xn, (i64)n * ns };
          wc_par_for(&c->pool, n * ns, wc_add_slice, &a); }

        /* ---- cross-attention ------------------------------------------ */
        memcpy(c->xn, c->x, (usize)n * (usize)ns * sizeof(f32));
        if (!wc_layernorm(c, c->xn, n, L->cross_ln_w, L->cross_ln_b)) return false;
        if (!wc_matmul(c, c->xn, n, L->cross_q_w, L->cross_q_b, c->q)) return false;

        wc_attention(c, c->q,
                     c->cross_k + (i64)il * Ta * ns,
                     c->cross_v + (i64)il * Ta * ns,
                     n, Ta, c->n_head_t, c->hd_t, scale_t, false, 0, c->att);

        if (!wc_matmul(c, c->att, n, L->cross_o_w, L->cross_o_b, c->xn)) return false;
        { WcAdd a = { c->x, c->xn, (i64)n * ns };
          wc_par_for(&c->pool, n * ns, wc_add_slice, &a); }

        /* ---- MLP ------------------------------------------------------- */
        memcpy(c->xn, c->x, (usize)n * (usize)ns * sizeof(f32));
        if (!wc_layernorm(c, c->xn, n, L->mlp_ln_w, L->mlp_ln_b)) return false;
        if (!wc_matmul(c, c->xn, n, L->mlp_0_w, L->mlp_0_b, c->ff)) return false;
        { WcGelu g = { c->ff };
          wc_par_for(&c->pool, n * c->n_ff_t, wc_gelu_slice, &g); }
        if (!wc_matmul(c, c->ff, n, L->mlp_2_w, L->mlp_2_b, c->xn)) return false;
        { WcAdd a = { c->x, c->xn, (i64)n * ns };
          wc_par_for(&c->pool, n * ns, wc_add_slice, &a); }
    }

    if (!wc_layernorm(c, c->x, n, W->d_ln_w, W->d_ln_b)) return false;

    /* --- tied LM head, last token only ---------------------------------- */
    {
        WcLmHead lh = { c->x + (i64)(n - 1) * ns, W->d_tok_emb, logits, ns, true };
        wc_par_for(&c->pool, c->n_vocab, wc_lm_head_slice, &lh);
        if (!atomic_load_explicit(&lh.ok, memory_order_relaxed)) return false;
    }

    c->kv_len = n_past + n;
    return true;
}
