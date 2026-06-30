/*
 * Liu - Hy-MT transformer forward pass.
 *
 * UNVERIFIED ASSUMPTIONS (need confirmation against the real model file +
 * llama.cpp PR #22836):
 *   - Tensor names follow the mainline llama.cpp convention
 *     (token_embd.weight, blk.N.attn_q.weight, ...). Hunyuan variants
 *     with extra tensors (expert routing, etc.) are not handled.
 *   - RoPE style is picked from the architecture name (NeoX for
 *     non-"llama" arches). If translation output is garbled, this is the
 *     first knob to flip.
 *   - QK-RMSNorm is applied iff blk.0.attn_q_norm.weight is present.
 *   - STQ1_0 byte layout per llm_quant_stq1_0.h.
 *
 * The scaffold (GGUF wiring, KV cache, attention math, SwiGLU) is
 * standard and correct; the items above are the model-specific risk.
 */
#include "llm/llm_model.h"
#include "llm/llm_quant_stq1_0.h"
#include "llm/llm_quant_q6k.h"
#include "llm/llm_stq1_0_kernel.h"
#include "core/memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>     /* worker pool */
#include <stdatomic.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include <unistd.h>      /* sysconf(_SC_NPROCESSORS_ONLN) */

/* Worker-pool ceiling. Per-token work is ~1.5 G MACs (Hy-MT 1.8B) split
 * across row-independent matvecs; past ~8 threads the dispatch latency and
 * memory bandwidth eat the gains on every machine we care about. */
#define LLM_POOL_MAX_THREADS 8

typedef struct LlmModel LlmModel_fwd_unused;  /* (struct tag declared by header) */
typedef struct { struct LlmModel *m; i32 lane; } LlmPoolArg;

typedef struct {
    const GgufTensor *attn_norm;
    const GgufTensor *attn_q, *attn_k, *attn_v, *attn_o;
    const GgufTensor *attn_q_norm, *attn_k_norm;   /* optional */
    const GgufTensor *ffn_norm;
    const GgufTensor *ffn_gate, *ffn_up, *ffn_down;
} LlmLayer;

struct LlmModel {
    LlmHParams h;

    const GgufTensor *tok_embd;
    const GgufTensor *out_norm;
    const GgufTensor *out_proj;     /* may alias tok_embd (tied) */
    LlmLayer        *layers;

    /* KV cache: [layer][pos][kv_head][head_dim], flat. */
    f32 *k_cache;
    f32 *v_cache;

    /* RoPE inverse frequencies, head_dim/2 entries — pos-independent, so
     * computed once at create instead of powf()-ing per element per head
     * per layer per token. */
    f32 *inv_freq;

    /* Activation scratch — allocated once from `arena`, reused per step. */
    Arena arena;
    f32 *xb, *xb2, *hb;
    f32 *q, *kc, *vc;
    f32 *attn_out;
    f32 *ffn_g, *ffn_u;
    i32  row_cap;
    void *q8k;                      /* STQ1_0 matvec: Q8_K activation scratch */

    /* ---- worker pool (row-split matvec + per-head attention) ----
     * Per-LANE scratch: lane 0 is the calling thread, lanes 1..n are pool
     * workers. Row-splitting computes each out[r] bit-identically to the
     * serial path, so threading never changes the numerics. */
    i32  lanes;                     /* 1 (serial) .. LLM_POOL_MAX_THREADS */
    f32 *t_row;                     /* lanes x row_cap dequant scratch */
    f32 *t_att;                     /* lanes x n_ctx attention scores */
    struct {
        pthread_t       th[LLM_POOL_MAX_THREADS];
        pthread_mutex_t mu;
        pthread_cond_t  wake, done;
        u64             gen;
        i32             active;     /* workers still inside current gen */
        atomic_int      cursor;     /* next chunk start index */
        struct {
            void (*fn)(struct LlmModel *m, const void *job, i32 lane,
                       i32 i0, i32 i1);
            const void *job;
            i32 total, chunk;
        } work;
        bool quit;
        bool inited;
        i32  n_workers;             /* spawned threads = lanes - 1 */
        LlmPoolArg args[LLM_POOL_MAX_THREADS];
    } pool;
};

/* ---- helpers -------------------------------------------------------- */

static f32 dotf(const f32 *a, const f32 *b, i32 n) {
#if defined(__ARM_NEON)
    float32x4_t acc = vdupq_n_f32(0.0f);
    i32 i = 0;
    for (; i + 4 <= n; i += 4) {
        acc = vmlaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
    }
    f32 s = vaddvq_f32(acc);
    for (; i < n; i++) s += a[i] * b[i];
    return s;
#else
    f32 s = 0.0f;
    for (i32 i = 0; i < n; i++) s += a[i] * b[i];
    return s;
#endif
}

/* Dequantize row `row` of weight `w` (in_dim contiguous values) into out. */
static void dequant_row(const GgufTensor *w, i32 row, i32 in_dim, f32 *out) {
    switch (w->dtype) {
        case GGML_TYPE_F32: {
            const f32 *src = (const f32 *)w->data + (usize)row * in_dim;
            memcpy(out, src, (usize)in_dim * sizeof(f32));
            break;
        }
        case GGML_TYPE_F16: {
            const u16 *src = (const u16 *)w->data + (usize)row * in_dim;
            for (i32 i = 0; i < in_dim; i++) out[i] = gguf_f16_to_f32(src[i]);
            break;
        }
        case GGML_TYPE_Q6_K: {
            const u8 *src = w->data + (usize)row * q6k_row_bytes(in_dim);
            q6k_dequant_row(src, out, in_dim);
            break;
        }
        case GGML_TYPE_STQ1_0: {
            const u8 *src = w->data + (usize)row * stq1_0_row_bytes(in_dim);
            stq1_0_dequant_row(src, out, in_dim);
            break;
        }
        default:
            memset(out, 0, (usize)in_dim * sizeof(f32));
            break;
    }
}

/* ---- worker pool -----------------------------------------------------
 * Fork-join over an index range. The caller participates as lane 0, so a
 * "pool" of N lanes spawns N-1 threads. Chunks are claimed with an atomic
 * cursor (work-stealing granularity), and the mutex/cond pair only guards
 * generation hand-off — the hot path is lock-free. */

/* SAFETY INVARIANT: pool.work and pool.cursor are read here without the
 * mutex. That is sound because pool_dispatch is a full join barrier — it
 * cannot return (and therefore no new generation's fields can be written)
 * until every worker has exited this function and decremented `active`
 * under the mutex. Workers observe the new generation's fields through the
 * mutex acquire that woke them, so relaxed cursor ops only need atomicity,
 * not ordering. */
static void pool_run_chunks(LlmModel *m, i32 lane) {
    for (;;) {
        i32 i0 = atomic_fetch_add_explicit(&m->pool.cursor, m->pool.work.chunk,
                                           memory_order_relaxed);
        if (i0 >= m->pool.work.total) break;
        i32 i1 = i0 + m->pool.work.chunk;
        if (i1 > m->pool.work.total) i1 = m->pool.work.total;
        m->pool.work.fn(m, m->pool.work.job, lane, i0, i1);
    }
}

static void *pool_worker_main(void *argp) {
    LlmPoolArg *a = (LlmPoolArg *)argp;
    LlmModel *m = a->m;
    u64 seen = 0;
    pthread_mutex_lock(&m->pool.mu);
    for (;;) {
        while (!m->pool.quit && m->pool.gen == seen)
            pthread_cond_wait(&m->pool.wake, &m->pool.mu);
        if (m->pool.quit) break;
        seen = m->pool.gen;
        pthread_mutex_unlock(&m->pool.mu);
        pool_run_chunks(m, a->lane);
        pthread_mutex_lock(&m->pool.mu);
        if (--m->pool.active == 0) pthread_cond_signal(&m->pool.done);
    }
    pthread_mutex_unlock(&m->pool.mu);
    return NULL;
}

/* Run fn over [0, total) with `chunk`-sized claims. Falls back to a plain
 * serial call when the pool is absent or the job is too small to amortize
 * the wake/join handshake. */
static void pool_dispatch(LlmModel *m,
                          void (*fn)(LlmModel *, const void *, i32, i32, i32),
                          const void *job, i32 total, i32 chunk,
                          bool serial) {
    if (total <= 0) return;
    if (serial || m->pool.n_workers <= 0 || total <= chunk) {
        fn(m, job, 0, 0, total);
        return;
    }
    pthread_mutex_lock(&m->pool.mu);
    m->pool.work.fn    = fn;
    m->pool.work.job   = job;
    m->pool.work.total = total;
    m->pool.work.chunk = chunk;
    atomic_store_explicit(&m->pool.cursor, 0, memory_order_relaxed);
    m->pool.active = m->pool.n_workers;
    m->pool.gen++;
    pthread_cond_broadcast(&m->pool.wake);
    pthread_mutex_unlock(&m->pool.mu);

    pool_run_chunks(m, 0);                       /* caller lane */

    pthread_mutex_lock(&m->pool.mu);
    while (m->pool.active > 0)
        pthread_cond_wait(&m->pool.done, &m->pool.mu);
    pthread_mutex_unlock(&m->pool.mu);
}

static i32 pool_pick_lanes(void) {
    const char *env = getenv("LIU_LLM_THREADS");
    if (env && env[0]) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end != env && *end == '\0' && v >= 1) {
            return v > LLM_POOL_MAX_THREADS ? LLM_POOL_MAX_THREADS : (i32)v;
        }
        fprintf(stderr, "llm: ignoring malformed LIU_LLM_THREADS=\"%s\"\n", env);
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > LLM_POOL_MAX_THREADS) n = LLM_POOL_MAX_THREADS;
    return (i32)n;
}

/* ---- matvec ----------------------------------------------------------
 * out[r] = dot(in, W[r]) for r in 0..out_dim. W is [in_dim, out_dim] in
 * GGUF terms — ne[0]=in_dim contiguous, ne[1]=out_dim rows. Rows are
 * independent, so the pool splits the row range; each lane that needs a
 * dequant buffer uses its own t_row slice. The STQ1_0 activation (q8k) is
 * quantized ONCE by the caller and only read inside the job. */

typedef struct {
    const GgufTensor *w;
    const f32 *in;
    i32 in_dim;
    f32 *out;
} MatvecJob;

static void matvec_rows_job(LlmModel *m, const void *jp, i32 lane,
                            i32 r0, i32 r1) {
    const MatvecJob *j = (const MatvecJob *)jp;
    const GgufTensor *w = j->w;
    if (w->dtype == GGML_TYPE_F32) {
        const f32 *base = (const f32 *)w->data;
        for (i32 r = r0; r < r1; r++)
            j->out[r] = dotf(base + (usize)r * j->in_dim, j->in, j->in_dim);
    } else if (w->dtype == GGML_TYPE_STQ1_0) {
        /* llama.cpp PR #22836's STQ1_0 path: integer-SIMD vec_dot of each
         * weight row against the pre-quantized Q8_K activation. */
        usize row_bytes = stq1_0_row_bytes(j->in_dim);
        for (i32 r = r0; r < r1; r++)
            j->out[r] = stq1_0_vec_dot(w->data + (usize)r * row_bytes,
                                       m->q8k, j->in_dim);
    } else {
        f32 *row = m->t_row + (usize)lane * m->row_cap;
        for (i32 r = r0; r < r1; r++) {
            dequant_row(w, r, j->in_dim, row);
            j->out[r] = dotf(row, j->in, j->in_dim);
        }
    }
}

static void matvec(LlmModel *m, const GgufTensor *w, const f32 *in,
                   i32 in_dim, i32 out_dim, f32 *out) {
    if (w->dtype == GGML_TYPE_STQ1_0)
        stq1_0_q8k_quantize(in, m->q8k, in_dim);
    MatvecJob j = { w, in, in_dim, out };
    /* 32-row chunks: fine enough that 8 lanes share a 512-row KV matvec,
     * coarse enough that the atomic claim is noise. */
    pool_dispatch(m, matvec_rows_job, &j, out_dim, 32, false);
}

/* ---- attention (per query head) -------------------------------------- */

static void softmax(f32 *x, i32 n);   /* defined below */

typedef struct {
    const f32 *k_layer;   /* k_cache + layer_base */
    const f32 *v_layer;
    const f32 *q;
    f32       *attn_out;
    i32 pos, hd, kv_dim, group;
    f32 att_scale;
} AttnJob;

static void attn_heads_job(LlmModel *m, const void *jp, i32 lane,
                           i32 h0, i32 h1) {
    const AttnJob *j = (const AttnJob *)jp;
    f32 *att = m->t_att + (usize)lane * m->h.n_ctx;
    for (i32 hh = h0; hh < h1; hh++) {
        i32 kvh = hh / j->group;
        const f32 *qh = j->q + hh * j->hd;
        for (i32 p = 0; p <= j->pos; p++) {
            const f32 *kp = j->k_layer + (usize)p * j->kv_dim + kvh * j->hd;
            att[p] = dotf(qh, kp, j->hd) * j->att_scale;
        }
        softmax(att, j->pos + 1);
        f32 *out_h = j->attn_out + hh * j->hd;
        memset(out_h, 0, (usize)j->hd * sizeof(f32));
        for (i32 p = 0; p <= j->pos; p++) {
            const f32 *vp = j->v_layer + (usize)p * j->kv_dim + kvh * j->hd;
            f32 w = att[p];
            for (i32 d = 0; d < j->hd; d++) out_h[d] += w * vp[d];
        }
    }
}

static void rmsnorm(f32 *out, const f32 *in, const f32 *weight, i32 n, f32 eps) {
    f32 ss = 0.0f;
    for (i32 i = 0; i < n; i++) ss += in[i] * in[i];
    f32 scale = 1.0f / sqrtf(ss / (f32)n + eps);
    for (i32 i = 0; i < n; i++) out[i] = in[i] * scale * weight[i];
}

static void softmax(f32 *x, i32 n) {
    f32 mx = x[0];
    for (i32 i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    f32 sum = 0.0f;
    for (i32 i = 0; i < n; i++) { x[i] = expf(x[i] - mx); sum += x[i]; }
    f32 inv = sum > 0.0f ? 1.0f / sum : 0.0f;
    for (i32 i = 0; i < n; i++) x[i] *= inv;
}

/* Rotary position embedding, applied in place to one head's vector.
 * `inv_freq[i]` is the pos-independent freq_base^(-2i/head_dim), built
 * once at model create — only the per-element cos/sin is per-token. */
static void rope_head(f32 *vec, i32 head_dim, i32 pos, const f32 *inv_freq,
                      bool neox) {
    i32 half = head_dim / 2;
    if (neox) {
        for (i32 i = 0; i < half; i++) {
            f32 theta = (f32)pos * inv_freq[i];
            f32 c = cosf(theta), s = sinf(theta);
            f32 x0 = vec[i], x1 = vec[i + half];
            vec[i]        = x0 * c - x1 * s;
            vec[i + half] = x0 * s + x1 * c;
        }
    } else {
        for (i32 i = 0; i < half; i++) {
            f32 theta = (f32)pos * inv_freq[i];
            f32 c = cosf(theta), s = sinf(theta);
            f32 x0 = vec[2 * i], x1 = vec[2 * i + 1];
            vec[2 * i]     = x0 * c - x1 * s;
            vec[2 * i + 1] = x0 * s + x1 * c;
        }
    }
}

/* ---- tensor lookup + shape validation ------------------------------- */

/* Look up `name`; require ne[0]==d0 and (d1<=0 or ne[1]==d1). Returns
 * NULL with no side effect if missing or mis-shaped. Projection weights
 * may be F32/F16/STQ1_0 (matvec dequantizes per row). */
static const GgufTensor *want(const GgufFile *g, const char *name,
                              i64 d0, i64 d1) {
    const GgufTensor *t = gguf_tensor(g, name);
    if (!t) return NULL;
    if (d0 > 0 && (i64)t->dims[0] != d0) return NULL;
    if (d1 > 0 && (i64)t->dims[1] != d1) return NULL;
    /* These are at most 2-D weights ([in_dim, out_dim]); the matvec/embed
     * kernels read dims[0]*dims[1] contiguous elements. Reject any inflated
     * higher dim so the element count the loader bounds-checked matches what
     * is actually read — otherwise dims[2..3] can be used to wrap nbytes. */
    if (t->dims[2] != 1 || t->dims[3] != 1) return NULL;
    if (t->dtype != GGML_TYPE_F32 && t->dtype != GGML_TYPE_F16 &&
        t->dtype != GGML_TYPE_Q6_K && t->dtype != GGML_TYPE_STQ1_0) {
        return NULL;
    }
    if (!t->data) return NULL;
    return t;
}

/* Same, but require F32 — norm weights are consumed directly as f32*,
 * so a quantized norm tensor would be both wrong and an over-read. */
static const GgufTensor *want_f32(const GgufFile *g, const char *name, i64 d0) {
    const GgufTensor *t = gguf_tensor(g, name);
    if (!t || t->dtype != GGML_TYPE_F32 || !t->data) return NULL;
    if (d0 > 0 && (i64)t->dims[0] != d0) return NULL;
    /* Norm weights are strictly 1-D and consumed directly as `dims[0]`
     * contiguous f32. Any non-1 higher dim is malformed and could inflate
     * the element count past what rmsnorm() reads — reject it. */
    if (t->dims[1] != 1 || t->dims[2] != 1 || t->dims[3] != 1) return NULL;
    return t;
}

/* ---- hyperparameters (shared by CPU + Metal backends) --------------- */

bool llm_hparams_load(const GgufFile *g, LlmHParams *h) {
    if (!g || !h) return false;
    memset(h, 0, sizeof *h);

    /* Architecture prefix drives every other metadata key. */
    const char *arch = NULL; u64 arch_len = 0;
    if (!gguf_kv_str(g, "general.architecture", &arch, &arch_len) ||
        arch_len == 0 || arch_len >= sizeof h->arch) {
        return false;
    }
    memcpy(h->arch, arch, (usize)arch_len);
    h->arch[arch_len] = '\0';

    char key[96];
    u32 u = 0;
    #define KEY(suffix) (snprintf(key, sizeof key, "%s.%s", h->arch, suffix), key)

    if (!gguf_kv_u32(g, KEY("block_count"), &u))      return false;
    h->n_layers = (i32)u;
    if (!gguf_kv_u32(g, KEY("embedding_length"), &u)) return false;
    h->n_embd = (i32)u;
    if (!gguf_kv_u32(g, KEY("attention.head_count"), &u)) return false;
    h->n_head = (i32)u;
    if (!gguf_kv_u32(g, KEY("attention.head_count_kv"), &u)) u = (u32)h->n_head;
    h->n_head_kv = (i32)u;
    if (!gguf_kv_u32(g, KEY("feed_forward_length"), &u)) return false;
    h->n_ff = (i32)u;

    /* head_dim: explicit key_length, else n_embd / n_head. */
    if (gguf_kv_u32(g, KEY("attention.key_length"), &u) && u > 0) {
        h->head_dim = (i32)u;
    } else {
        h->head_dim = h->n_head > 0 ? h->n_embd / h->n_head : 0;
    }

    h->rms_eps = 1e-5f;
    gguf_kv_f32(g, KEY("attention.layer_norm_rms_epsilon"), &h->rms_eps);
    h->rope_freq_base = 10000.0f;
    gguf_kv_f32(g, KEY("rope.freq_base"), &h->rope_freq_base);

    u32 ctx_train = LLM_MODEL_MAX_CTX;
    gguf_kv_u32(g, KEY("context_length"), &ctx_train);
    h->n_ctx = (i32)(ctx_train < LLM_MODEL_MAX_CTX ? ctx_train : LLM_MODEL_MAX_CTX);

    /* RoPE style: mainline "llama" uses adjacent-pair; everything else
     * (Hunyuan included) uses NeoX. */
    h->rope_neox = (strstr(h->arch, "llama") == NULL);
    #undef KEY

    /* n_vocab from the token-embedding table's second dim. */
    const GgufTensor *te = gguf_tensor(g, "token_embd.weight");
    if (!te || te->n_dims < 2) return false;
    h->n_vocab = (i32)te->dims[1];

    /* Upper bounds are not just sanity — they keep q_dim = n_head*head_dim
     * and kv_dim = n_head_kv*head_dim (computed as i32 by both backends)
     * from overflowing on a crafted GGUF. With these ceilings the products
     * stay below 2^22, well inside i32, before any allocation derives a
     * size from them. */
    if (h->n_layers <= 0 || h->n_layers > 256 ||
        h->n_embd <= 0 || h->n_embd > (1 << 18) ||
        h->n_head <= 0 || h->n_head > 4096 ||
        h->n_head_kv <= 0 || h->n_head_kv > 4096 ||
        h->head_dim <= 0 || h->head_dim > 1024 ||
        h->n_ff <= 0 || h->n_ff > (1 << 20) || h->n_ctx <= 0 ||
        h->n_vocab <= 0 || h->n_vocab > (1 << 21) ||
        (h->n_head % h->n_head_kv) != 0) {
        return false;
    }
    return true;
}

/* ---- create / free -------------------------------------------------- */

LlmModel *llm_model_create(const GgufFile *g) {
    if (!g) return NULL;

    LlmModel *m = (LlmModel *)calloc(1, sizeof *m);
    if (!m) return NULL;
    LlmHParams *h = &m->h;

    if (!llm_hparams_load(g, h)) { free(m); return NULL; }

    /* token embedding: [n_embd, n_vocab]. */
    m->tok_embd = want(g, "token_embd.weight", h->n_embd, 0);
    if (!m->tok_embd) { free(m); return NULL; }

    m->out_norm = want_f32(g, "output_norm.weight", h->n_embd);
    if (!m->out_norm) { free(m); return NULL; }
    /* output proj — tied to the embedding table when absent. */
    m->out_proj = gguf_tensor(g, "output.weight");
    if (m->out_proj) {
        m->out_proj = want(g, "output.weight", h->n_embd, h->n_vocab);
        if (!m->out_proj) { free(m); return NULL; }
    } else {
        m->out_proj = m->tok_embd;
    }

    i32 q_dim = h->n_head * h->head_dim;
    i32 kv_dim = h->n_head_kv * h->head_dim;

    m->layers = (LlmLayer *)calloc((usize)h->n_layers, sizeof(LlmLayer));
    if (!m->layers) { free(m); return NULL; }

    bool qk_norm_seen = false;
    for (i32 l = 0; l < h->n_layers; l++) {
        LlmLayer *ly = &m->layers[l];
        char nm[80];
        #define LKEY(suffix) (snprintf(nm, sizeof nm, "blk.%d.%s", l, suffix), nm)

        ly->attn_norm = want_f32(g, LKEY("attn_norm.weight"), h->n_embd);
        ly->attn_q    = want(g, LKEY("attn_q.weight"),    h->n_embd, q_dim);
        ly->attn_k    = want(g, LKEY("attn_k.weight"),    h->n_embd, kv_dim);
        ly->attn_v    = want(g, LKEY("attn_v.weight"),    h->n_embd, kv_dim);
        ly->attn_o    = want(g, LKEY("attn_output.weight"), q_dim,   h->n_embd);
        ly->ffn_norm  = want_f32(g, LKEY("ffn_norm.weight"), h->n_embd);
        ly->ffn_gate  = want(g, LKEY("ffn_gate.weight"),  h->n_embd, h->n_ff);
        ly->ffn_up    = want(g, LKEY("ffn_up.weight"),    h->n_embd, h->n_ff);
        ly->ffn_down  = want(g, LKEY("ffn_down.weight"),  h->n_ff,   h->n_embd);

        if (!ly->attn_norm || !ly->attn_q || !ly->attn_k || !ly->attn_v ||
            !ly->attn_o || !ly->ffn_norm || !ly->ffn_gate || !ly->ffn_up ||
            !ly->ffn_down) {
            free(m->layers); free(m); return NULL;
        }

        /* Optional QK-RMSNorm (Hunyuan / Qwen3-style). Either both heads
         * carry it or neither does — a partial set is treated as absent. */
        ly->attn_q_norm = want_f32(g, LKEY("attn_q_norm.weight"), h->head_dim);
        ly->attn_k_norm = want_f32(g, LKEY("attn_k_norm.weight"), h->head_dim);
        if (ly->attn_q_norm && ly->attn_k_norm) {
            qk_norm_seen = true;
        } else {
            ly->attn_q_norm = ly->attn_k_norm = NULL;
        }
        #undef LKEY
    }
    h->qk_norm = qk_norm_seen;

    /* KV cache. malloc, not calloc: every slot is written by decode()
     * at its position before any later position reads it, so the
     * zero-fill (~100+ MB for a deep model) would be wasted. */
    usize kv_elems = (usize)h->n_layers * h->n_ctx * kv_dim;
    m->k_cache = (f32 *)malloc(kv_elems * sizeof(f32));
    m->v_cache = (f32 *)malloc(kv_elems * sizeof(f32));
    if (!m->k_cache || !m->v_cache) { llm_model_free(m); return NULL; }

    /* Activation scratch from an arena (sized generously, MB-scale). */
    m->row_cap = h->n_embd;
    if (q_dim  > m->row_cap) m->row_cap = q_dim;
    if (h->n_ff > m->row_cap) m->row_cap = h->n_ff;

    /* Lane count first — the per-lane scratch below is sized by it. */
    m->lanes = pool_pick_lanes();

    i32 half_hd = h->head_dim / 2;
    usize need = ((usize)h->n_embd * 4 + (usize)q_dim * 2 + (usize)kv_dim * 2 +
                  (usize)m->lanes * (usize)h->n_ctx +
                  (usize)h->n_ff * 2 +
                  (usize)m->lanes * (usize)m->row_cap +
                  (usize)half_hd) * sizeof(f32)
                 + stq1_0_q8k_bytes(m->row_cap)
                 + 4096;
    usize arena_sz = need < MB(4) ? MB(4) : need + MB(1);
    m->arena = arena_create(arena_sz);
    if (!m->arena.base) { llm_model_free(m); return NULL; }

    m->xb       = (f32 *)arena_alloc(&m->arena, (usize)h->n_embd * sizeof(f32));
    m->xb2      = (f32 *)arena_alloc(&m->arena, (usize)h->n_embd * sizeof(f32));
    m->hb       = (f32 *)arena_alloc(&m->arena, (usize)h->n_embd * sizeof(f32));
    m->q        = (f32 *)arena_alloc(&m->arena, (usize)q_dim     * sizeof(f32));
    m->kc       = (f32 *)arena_alloc(&m->arena, (usize)kv_dim    * sizeof(f32));
    m->vc       = (f32 *)arena_alloc(&m->arena, (usize)kv_dim    * sizeof(f32));
    m->attn_out = (f32 *)arena_alloc(&m->arena, (usize)q_dim     * sizeof(f32));
    m->ffn_g    = (f32 *)arena_alloc(&m->arena, (usize)h->n_ff   * sizeof(f32));
    m->ffn_u    = (f32 *)arena_alloc(&m->arena, (usize)h->n_ff   * sizeof(f32));
    /* Even lanes==1 (serial) allocates the per-lane scratch with one slot —
     * the job functions always index [lane * stride], so there is no
     * special-cased serial layout to keep in sync. */
    m->t_row    = (f32 *)arena_alloc(&m->arena,
                       (usize)m->lanes * (usize)m->row_cap * sizeof(f32));
    m->t_att    = (f32 *)arena_alloc(&m->arena,
                       (usize)m->lanes * (usize)h->n_ctx * sizeof(f32));
    m->inv_freq = (f32 *)arena_alloc(&m->arena, (usize)half_hd   * sizeof(f32));
    m->q8k      = arena_alloc(&m->arena, stq1_0_q8k_bytes(m->row_cap));
    if (!m->xb || !m->xb2 || !m->hb || !m->q || !m->kc || !m->vc ||
        !m->attn_out || !m->ffn_g || !m->ffn_u || !m->t_row || !m->t_att ||
        !m->inv_freq || !m->q8k) {
        llm_model_free(m);
        return NULL;
    }

    /* Spawn the worker pool (lanes-1 threads; lane 0 is the caller). A
     * failed spawn just shrinks the pool — worst case n_workers==0 and
     * every dispatch runs serial, which is the old behavior. */
    if (m->lanes > 1) {
        pthread_mutex_init(&m->pool.mu, NULL);
        pthread_cond_init(&m->pool.wake, NULL);
        pthread_cond_init(&m->pool.done, NULL);
        m->pool.inited = true;
        i32 spawned = 0;
        for (i32 t = 0; t < m->lanes - 1; t++) {
            m->pool.args[t].m = m;
            m->pool.args[t].lane = t + 1;
            if (pthread_create(&m->pool.th[t], NULL, pool_worker_main,
                               &m->pool.args[t]) != 0)
                break;
            spawned++;
        }
        m->pool.n_workers = spawned;
    }

    /* RoPE inverse frequencies — pos-independent, computed once here. */
    for (i32 i = 0; i < half_hd; i++) {
        m->inv_freq[i] = powf(h->rope_freq_base,
                              -2.0f * (f32)i / (f32)h->head_dim);
    }

    return m;
}

void llm_model_free(LlmModel *m) {
    if (!m) return;
    /* pool.inited means the mutex/conds exist; n_workers is how many
     * threads actually spawned (a mid-loop pthread_create failure leaves
     * it lower than lanes-1 — joining just those is correct). */
    if (m->pool.inited) {
        pthread_mutex_lock(&m->pool.mu);
        m->pool.quit = true;
        pthread_cond_broadcast(&m->pool.wake);
        pthread_mutex_unlock(&m->pool.mu);
        for (i32 t = 0; t < m->pool.n_workers; t++)
            pthread_join(m->pool.th[t], NULL);
        pthread_mutex_destroy(&m->pool.mu);
        pthread_cond_destroy(&m->pool.wake);
        pthread_cond_destroy(&m->pool.done);
    }
    free(m->layers);
    free(m->k_cache);
    free(m->v_cache);
    arena_destroy(&m->arena);
    free(m);
}

const LlmHParams *llm_model_hparams(const LlmModel *m) {
    return m ? &m->h : NULL;
}

void llm_model_reset(LlmModel *m) {
    /* The KV cache is overwritten position-by-position as a sequence is
     * fed from pos 0, so there is nothing to clear — decode() at pos 0
     * starts fresh. Kept as an explicit API for the engine's clarity. */
    (void)m;
}

/* ---- decode --------------------------------------------------------- */

bool llm_model_decode(LlmModel *m, i32 token_id, i32 pos, f32 *logits) {
    if (!m || !logits) return false;
    const LlmHParams *h = &m->h;
    if (token_id < 0 || token_id >= h->n_vocab) return false;
    if (pos < 0 || pos >= h->n_ctx) return false;

    i32 q_dim = h->n_head * h->head_dim;
    i32 kv_dim = h->n_head_kv * h->head_dim;
    i32 hd = h->head_dim;
    i32 group = h->n_head / h->n_head_kv;       /* query heads per kv head */
    f32 att_scale = 1.0f / sqrtf((f32)hd);

    /* x = embedding(token_id) */
    f32 *x = m->xb;
    dequant_row(m->tok_embd, token_id, h->n_embd, x);

    for (i32 l = 0; l < h->n_layers; l++) {
        const LlmLayer *ly = &m->layers[l];

        /* --- attention --- */
        rmsnorm(m->hb, x, (const f32 *)ly->attn_norm->data, h->n_embd, h->rms_eps);
        matvec(m, ly->attn_q, m->hb, h->n_embd, q_dim,  m->q);
        matvec(m, ly->attn_k, m->hb, h->n_embd, kv_dim, m->kc);
        matvec(m, ly->attn_v, m->hb, h->n_embd, kv_dim, m->vc);

        /* RoPE first, THEN optional per-head QK-RMSNorm. Order matches
         * llama.cpp's hunyuan graph (build_qkv -> rope_ext -> build_norm);
         * RoPE preserves the vector norm but the learned per-element norm
         * weight does not commute with the rotation, so the order is
         * load-bearing. */
        for (i32 hh = 0; hh < h->n_head; hh++) {
            f32 *qh = m->q + hh * hd;
            rope_head(qh, hd, pos, m->inv_freq, h->rope_neox);
            if (ly->attn_q_norm)
                rmsnorm(qh, qh, (const f32 *)ly->attn_q_norm->data, hd, h->rms_eps);
        }
        for (i32 kh = 0; kh < h->n_head_kv; kh++) {
            f32 *kh_v = m->kc + kh * hd;
            rope_head(kh_v, hd, pos, m->inv_freq, h->rope_neox);
            if (ly->attn_k_norm)
                rmsnorm(kh_v, kh_v, (const f32 *)ly->attn_k_norm->data, hd, h->rms_eps);
        }

        /* write K/V for this position into the cache */
        usize layer_base = (usize)l * h->n_ctx * kv_dim;
        f32 *k_dst = m->k_cache + layer_base + (usize)pos * kv_dim;
        f32 *v_dst = m->v_cache + layer_base + (usize)pos * kv_dim;
        memcpy(k_dst, m->kc, (usize)kv_dim * sizeof(f32));
        memcpy(v_dst, m->vc, (usize)kv_dim * sizeof(f32));

        /* per query head: attend over cached keys 0..pos. Heads are
         * independent (own att-score lane scratch, disjoint attn_out
         * slices), so the pool splits the head range one head per claim.
         * Short sequences run serial — the handshake would dominate. */
        {
            AttnJob aj = {
                m->k_cache + layer_base,
                m->v_cache + layer_base,
                m->q, m->attn_out,
                pos, hd, kv_dim, group, att_scale,
            };
            bool serial = ((i64)(pos + 1) * h->n_head * hd) < (64 * 1024);
            pool_dispatch(m, attn_heads_job, &aj, h->n_head, 1, serial);
        }

        /* output projection + residual */
        matvec(m, ly->attn_o, m->attn_out, q_dim, h->n_embd, m->xb2);
        for (i32 i = 0; i < h->n_embd; i++) x[i] += m->xb2[i];

        /* --- SwiGLU MLP --- */
        rmsnorm(m->hb, x, (const f32 *)ly->ffn_norm->data, h->n_embd, h->rms_eps);
        matvec(m, ly->ffn_gate, m->hb, h->n_embd, h->n_ff, m->ffn_g);
        matvec(m, ly->ffn_up,   m->hb, h->n_embd, h->n_ff, m->ffn_u);
        for (i32 i = 0; i < h->n_ff; i++) {
            f32 g = m->ffn_g[i];
            f32 silu = g / (1.0f + expf(-g));
            m->ffn_g[i] = silu * m->ffn_u[i];
        }
        matvec(m, ly->ffn_down, m->ffn_g, h->n_ff, h->n_embd, m->xb2);
        for (i32 i = 0; i < h->n_embd; i++) x[i] += m->xb2[i];
    }

    /* final norm + LM head */
    rmsnorm(m->hb, x, (const f32 *)m->out_norm->data, h->n_embd, h->rms_eps);
    matvec(m, m->out_proj, m->hb, h->n_embd, h->n_vocab, logits);
    return true;
}
