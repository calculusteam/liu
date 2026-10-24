/*
 * Liu - local LLM engine.
 *
 * The engine runs on the GPU. The forward-pass backend is chosen at
 * compile time, not at runtime: a normal build (LIU_LLM_METAL) is
 * Metal-only — if the GPU device or the metallib is missing,
 * llm_engine_load fails rather than silently dropping to the CPU.
 *
 * The portable CPU path (llm_model) is compiled in only when
 * LIU_LLM_METAL is off: on macOS that configuration is the correctness
 * reference, and off Apple it is the only backend. Either way exactly one
 * backend exists in a given build — there is no runtime fallback.
 */
#include "llm/llm_engine.h"
#include <time.h>
#include "llm/llm_gguf.h"
#include "llm/llm_model.h"
#include "llm/llm_sampler.h"
#include "llm/llm_tokenizer.h"
#include "llm/llm_metal.h"     /* declarations only; impl gated by LIU_LLM_METAL */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HunYuan chat-template special tokens, as verbatim vocab strings. The
 * bar is U+FF5C (fullwidth `｜`), not ASCII '|' — resolved to ids at
 * generate time; if the model isn't a HunYuan chat model they're absent
 * and chat-wrapping is skipped. */
#define HY_TOK_USER      "<｜hy_User｜>"
#define HY_TOK_ASSISTANT "<｜hy_Assistant｜>"
#define HY_TOK_EOT       "<｜hy_EOT｜>"

struct LlmEngine {
    bool generation_finished;
    GgufFile      gguf;
    LlmTokenizer *tok;
#ifdef LIU_LLM_METAL
    LlmMetal     *metal;      /* GPU backend — the only backend in a normal build */
#else
    LlmModel     *model;      /* CPU backend — built only for -DLIU_LLM_METAL=OFF */
    f32          *logits;     /* n_vocab scratch — CPU path only */
#endif
};

/* ---- backend dispatch ---------------------------------------------- */

static const LlmHParams *eng_hparams(const LlmEngine *e) {
#ifdef LIU_LLM_METAL
    return llm_metal_hparams(e->metal);
#else
    return llm_model_hparams(e->model);
#endif
}

static void set_err(char *err, usize cap, const char *msg) {
    if (err && cap > 0) snprintf(err, cap, "%s", msg);
}

/* ---- load / free --------------------------------------------------- */

/* Not guarded by an env flag: four clock_gettime calls against a load that
 * costs hundreds of milliseconds is free, and a profile that has to be
 * switched on is a profile nobody has when they need it. */
static LlmLoadProfile g_load_profile;

const LlmLoadProfile *llm_engine_load_profile(void) { return &g_load_profile; }

static f64 lp_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (f64)t.tv_sec * 1000.0 + (f64)t.tv_nsec / 1e6;
}

LlmEngine *llm_engine_load(const char *path, char *err, usize err_cap) {
    return llm_engine_load_cancellable(path, err, err_cap, NULL);
}

LlmEngine *llm_engine_load_cancellable(const char *path, char *err, usize err_cap,
                                       const LlmCancel *cancel) {
    if (llm_cancelled(cancel)) { set_err(err, err_cap, "cancelled"); return NULL; }
    set_err(err, err_cap, "");
    f64 lp_t0 = lp_now_ms(), lp_t = lp_t0;
    g_load_profile = (LlmLoadProfile){ .free_ms = g_load_profile.free_ms };
    if (!path || !path[0]) { set_err(err, err_cap, "no model path"); return NULL; }

    LlmEngine *e = (LlmEngine *)calloc(1, sizeof *e);
    if (!e) { set_err(err, err_cap, "out of memory"); return NULL; }

    if (!gguf_open(&e->gguf, path)) {
        set_err(err, err_cap, "not a valid GGUF v3 file");
        free(e);
        return NULL;
    }
    g_load_profile.gguf_open_ms = lp_now_ms() - lp_t; lp_t = lp_now_ms();
    if (llm_cancelled(cancel)) goto cancelled;
    e->tok = llm_tokenizer_create_cancellable(&e->gguf, cancel);
    if (!e->tok) {
        if (llm_cancelled(cancel)) goto cancelled;
        set_err(err, err_cap, "unsupported or invalid tokenizer metadata");
        gguf_close(&e->gguf);
        free(e);
        return NULL;
    }

    g_load_profile.tokenizer_ms = lp_now_ms() - lp_t; lp_t = lp_now_ms();
    if (llm_cancelled(cancel)) goto cancelled;

#ifdef LIU_LLM_METAL
    /* GPU-only: the engine always runs on Metal. A missing device or
     * metallib, or a model the GPU kernels can't handle, is a hard
     * error — never a silent CPU fallback. */
    if (!llm_metal_available()) {
        set_err(err, err_cap, "Metal GPU backend unavailable");
        llm_tokenizer_free(e->tok);
        gguf_close(&e->gguf);
        free(e);
        return NULL;
    }
    e->metal = llm_metal_create_cancellable(&e->gguf, cancel);
    if (llm_cancelled(cancel)) goto cancelled;
    if (!e->metal) {
        set_err(err, err_cap,
                "model cannot run on the Metal backend "
                "(unsupported architecture or dtype)");
        llm_tokenizer_free(e->tok);
        gguf_close(&e->gguf);
        free(e);
        return NULL;
    }
#else
    /* CPU engine. On macOS this is the -DLIU_LLM_METAL=OFF correctness
     * reference rather than a shipping configuration — but off Apple there is
     * no GPU backend to be the alternative, so this IS what Linux ships and
     * the only path a Linux user ever runs. The NEON kernels are guarded so
     * x86 falls back to scalar. */
    e->model = llm_model_create(&e->gguf);
    if (llm_cancelled(cancel)) goto cancelled;
    if (!e->model) {
        set_err(err, err_cap, "unsupported model architecture or dtype");
        llm_tokenizer_free(e->tok);
        gguf_close(&e->gguf);
        free(e);
        return NULL;
    }
    {
        const LlmHParams *h = eng_hparams(e);
        e->logits = (f32 *)malloc((usize)h->n_vocab * sizeof(f32));
        if (!e->logits) {
            set_err(err, err_cap, "out of memory");
            llm_engine_free(e);
            return NULL;
        }
    }
#endif
    g_load_profile.backend_ms   = lp_now_ms() - lp_t;
    g_load_profile.total_load_ms = lp_now_ms() - lp_t0;
    return e;
cancelled:
    set_err(err, err_cap, "cancelled");
    llm_engine_free(e);
    return NULL;
}

void llm_engine_free(LlmEngine *e) {
    if (!e) return;
    f64 lp_t0 = lp_now_ms();
#ifdef LIU_LLM_METAL
    if (e->metal) llm_metal_free(e->metal);
#else
    free(e->logits);
    if (e->model) llm_model_free(e->model);
#endif
    if (e->tok) llm_tokenizer_free(e->tok);
    gguf_close(&e->gguf);
    free(e);
    g_load_profile.free_ms = lp_now_ms() - lp_t0;
}

/* ---- generate ------------------------------------------------------ */

#ifdef LIU_LLM_METAL
/* Bridges llm_metal_generate's raw-token-id callback to the engine's
 * decoded-UTF-8 streaming callback. */
typedef struct {
    LlmEngine  *e;
    LlmTokenFn  on_token;
    void       *user;
    char       *piece;       /* sized from the vocabulary: no token is cut */
    i32         piece_cap;
} MetalGenCtx;

static bool metal_on_id(void *user, i32 token_id) {
    MetalGenCtx *ctx = (MetalGenCtx *)user;
    i32 w = llm_tokenizer_decode(ctx->e->tok, token_id, ctx->piece, ctx->piece_cap);
    if (w > 0 && ctx->on_token) return ctx->on_token(ctx->user, ctx->piece, w);
    return true;
}
#endif

i32 llm_engine_generate(LlmEngine *e, const char *prompt, i32 max_tokens,
                        LlmTokenFn on_token, void *user) {
    return llm_engine_generate_cancellable(e, prompt, max_tokens, on_token, user, NULL);
}

i32 llm_engine_generate_cancellable(LlmEngine *e, const char *prompt, i32 max_tokens,
                                     LlmTokenFn on_token, void *user,
                                     const LlmCancel *cancel) {
    if (e) e->generation_finished = false;
    if (llm_cancelled(cancel)) return LLM_CANCELLED;
    if (!e || !prompt) return LLM_ERROR;
    const LlmHParams *h = eng_hparams(e);
    if (h->n_ctx < 2) return -1;

    /* Reserve one context slot for at least one generated token. */
    i32 cap_ids = h->n_ctx;
    i32 *ids = (i32 *)malloc((usize)cap_ids * sizeof(i32));
    if (!ids) return -1;

    /* HunYuan chat wrapping. Hy-MT is an instruct model trained with
     *   <hy_begin_of_sentence><hy_User>{content}<hy_Assistant>
     * Feeding raw text instead makes it ramble — the wrapper tokens are
     * resolved by their verbatim vocab string. If the model isn't a
     * HunYuan chat model the tokens are absent and the prompt falls back
     * to a plain BOS + content sequence. */
    i32 nprompt   = 0;
    i32 bos       = llm_tokenizer_bos(e->tok);
    i32 user_tok  = llm_tokenizer_token_id(e->tok, HY_TOK_USER);
    i32 asst_tok  = llm_tokenizer_token_id(e->tok, HY_TOK_ASSISTANT);

    i32 wrappers = (bos >= 0) + (user_tok >= 0) + (asst_tok >= 0);
    i32 body_cap = cap_ids - wrappers - 1;  /* one output token fits on both backends */
    if (body_cap < 0) { free(ids); return LLM_ERROR; }
    if (bos >= 0)      ids[nprompt++] = bos;
    if (user_tok >= 0) ids[nprompt++] = user_tok;
    i32 body = llm_tokenizer_encode(e->tok, prompt, ids + nprompt, body_cap);
    if (body < 0) { free(ids); return LLM_ERROR; }
    nprompt += body;
    if (asst_tok >= 0) ids[nprompt++] = asst_tok;
    if (llm_cancelled(cancel)) { free(ids); return LLM_CANCELLED; }
    if (nprompt == 0) { free(ids); return 0; }

    i32 eos = llm_tokenizer_eos(e->tok);
    i32 eot = llm_tokenizer_token_id(e->tok, HY_TOK_EOT);
    if (max_tokens <= 0) max_tokens = 256;

    /* One piece buffer for the whole generation, as long as the longest token
     * the vocabulary can produce: llm_tokenizer_decode truncates at its cap
     * without a word, and a vocabulary can hold tokens of 512 bytes (Hy-MT's
     * does). Failing to get it is an error, never a shortened reply. */
    i32 piece_cap = llm_tokenizer_max_piece(e->tok);
    if (piece_cap < 256) piece_cap = 256;
    char *piece = (char *)malloc((usize)piece_cap);
    if (!piece) { free(ids); return LLM_ERROR; }

#ifdef LIU_LLM_METAL
    /* The whole autoregressive loop runs GPU-resident and pipelined
     * inside llm_metal_generate — no per-token CPU round-trip. */
    MetalGenCtx ctx = { e, on_token, user, piece, piece_cap };
    i32 g = llm_metal_generate_cancellable(e->metal, ids, nprompt, max_tokens,
                                           eos, eot, metal_on_id, &ctx, cancel);
    e->generation_finished = g >= 0 && llm_metal_generation_finished(e->metal);
    free(ids);
    free(piece);
    return g;
#else
    /* CPU reference path: feed the prompt, then greedily decode token by
     * token. The KV cache is sized to THIS request (prompt + output), as the
     * Metal path's ensure_ctx does, rather than to the model's ceiling. */
    llm_model_reset(e->model);
    (void)llm_model_reserve_ctx(e->model, nprompt + max_tokens);
    f32 *logits = e->logits;
    i32 pos = 0;
    for (i32 i = 0; i < nprompt; i++) {
        if (llm_cancelled(cancel)) { free(ids); free(piece); return LLM_CANCELLED; }
        if (!llm_model_decode(e->model, ids[i], pos, logits)) {
            free(ids);
            free(piece);
            return -1;
        }
        pos++;
    }
    free(ids);

    i32 generated = 0;
    while (generated < max_tokens && pos < h->n_ctx) {
        if (llm_cancelled(cancel)) { free(piece); return LLM_CANCELLED; }
        i32 next = llm_sampler_argmax(logits, h->n_vocab);
        if (next < 0) break;
        if (next == eos || next == eot) { e->generation_finished = true; break; }

        i32 w = llm_tokenizer_decode(e->tok, next, piece, piece_cap);
        if (w > 0 && on_token) {
            if (!on_token(user, piece, w)) break;
        }
        generated++;
        if (generated == max_tokens || pos + 1 == h->n_ctx) break;
        if (llm_cancelled(cancel)) { free(piece); return LLM_CANCELLED; }
        if (!llm_model_decode(e->model, next, pos, logits)) { free(piece); return LLM_ERROR; }
        pos++;
    }
    free(piece);
    return llm_cancelled(cancel) ? LLM_CANCELLED : generated;
#endif
}

/* ---- diagnostics --------------------------------------------------- */

bool llm_engine_generation_finished(const LlmEngine *e) {
    return e && e->generation_finished;
}

const char *llm_engine_arch(const LlmEngine *e) {
    if (!e) return "";
    const LlmHParams *h = eng_hparams(e);
    return h ? h->arch : "";
}

i32 llm_engine_tensor_count(const LlmEngine *e) {
    return e ? (i32)e->gguf.tensor_count : 0;
}

i32 llm_engine_n_ctx(const LlmEngine *e) {
    if (!e) return 0;
    const LlmHParams *h = eng_hparams(e);
    return h ? h->n_ctx : 0;
}

const char *llm_engine_backend(const LlmEngine *e) {
    if (!e) return "none";
#ifdef LIU_LLM_METAL
    return "metal";
#else
    return "cpu";
#endif
}
