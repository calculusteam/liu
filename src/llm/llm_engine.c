/*
 * Liu - local LLM engine.
 *
 * The engine runs on the GPU. The forward-pass backend is chosen at
 * compile time, not at runtime: a normal build (LIU_LLM_METAL) is
 * Metal-only — if the GPU device or the metallib is missing,
 * llm_engine_load fails rather than silently dropping to the CPU.
 *
 * The portable CPU path (llm_model) is compiled in only when
 * LIU_LLM_METAL is off; that configuration is the correctness reference
 * the test harness builds, not a shipping target. Either way exactly one
 * backend exists in a given build — there is no runtime fallback.
 */
#include "llm/llm_engine.h"
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

LlmEngine *llm_engine_load(const char *path, char *err, usize err_cap) {
    set_err(err, err_cap, "");
    if (!path || !path[0]) { set_err(err, err_cap, "no model path"); return NULL; }

    LlmEngine *e = (LlmEngine *)calloc(1, sizeof *e);
    if (!e) { set_err(err, err_cap, "out of memory"); return NULL; }

    if (!gguf_open(&e->gguf, path)) {
        set_err(err, err_cap, "not a valid GGUF v3 file");
        free(e);
        return NULL;
    }
    e->tok = llm_tokenizer_create(&e->gguf);
    if (!e->tok) {
        set_err(err, err_cap, "unsupported tokenizer (need byte-level BPE)");
        gguf_close(&e->gguf);
        free(e);
        return NULL;
    }

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
    e->metal = llm_metal_create(&e->gguf);
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
    /* CPU-only build (-DLIU_LLM_METAL=OFF): the correctness reference,
     * not a shipping configuration. */
    e->model = llm_model_create(&e->gguf);
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
    return e;
}

void llm_engine_free(LlmEngine *e) {
    if (!e) return;
#ifdef LIU_LLM_METAL
    if (e->metal) llm_metal_free(e->metal);
#else
    free(e->logits);
    if (e->model) llm_model_free(e->model);
#endif
    if (e->tok) llm_tokenizer_free(e->tok);
    gguf_close(&e->gguf);
    free(e);
}

/* ---- generate ------------------------------------------------------ */

#ifdef LIU_LLM_METAL
/* Bridges llm_metal_generate's raw-token-id callback to the engine's
 * decoded-UTF-8 streaming callback. */
typedef struct {
    LlmEngine  *e;
    LlmTokenFn  on_token;
    void       *user;
} MetalGenCtx;

static bool metal_on_id(void *user, i32 token_id) {
    MetalGenCtx *ctx = (MetalGenCtx *)user;
    char buf[256];
    i32 w = llm_tokenizer_decode(ctx->e->tok, token_id, buf, (i32)sizeof buf);
    if (w > 0 && ctx->on_token) return ctx->on_token(ctx->user, buf, w);
    return true;
}
#endif

i32 llm_engine_generate(LlmEngine *e, const char *prompt, i32 max_tokens,
                        LlmTokenFn on_token, void *user) {
    if (!e || !prompt) return -1;
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
     * HunYuan chat model the tokens are absent and we fall back to a
     * plain BOS + content sequence. */
    i32 nprompt   = 0;
    i32 bos       = llm_tokenizer_bos(e->tok);
    i32 user_tok  = llm_tokenizer_token_id(e->tok, HY_TOK_USER);
    i32 asst_tok  = llm_tokenizer_token_id(e->tok, HY_TOK_ASSISTANT);

    if (bos >= 0)      ids[nprompt++] = bos;
    if (user_tok >= 0) ids[nprompt++] = user_tok;
    /* Leave room for the trailing <hy_Assistant> + >=1 generated slot. */
    nprompt += llm_tokenizer_encode(e->tok, prompt, ids + nprompt,
                                    cap_ids - nprompt - 2);
    if (asst_tok >= 0 && nprompt < cap_ids - 1) ids[nprompt++] = asst_tok;
    if (nprompt == 0) { free(ids); return 0; }

    i32 eos = llm_tokenizer_eos(e->tok);
    i32 eot = llm_tokenizer_token_id(e->tok, HY_TOK_EOT);
    if (max_tokens <= 0) max_tokens = 256;

#ifdef LIU_LLM_METAL
    /* The whole autoregressive loop runs GPU-resident and pipelined
     * inside llm_metal_generate — no per-token CPU round-trip. */
    MetalGenCtx ctx = { e, on_token, user };
    i32 g = llm_metal_generate(e->metal, ids, nprompt, max_tokens,
                               eos, eot, metal_on_id, &ctx);
    free(ids);
    return g;
#else
    /* CPU reference path: feed the prompt, then greedily decode token by
     * token. */
    llm_model_reset(e->model);
    f32 *logits = e->logits;
    i32 pos = 0;
    for (i32 i = 0; i < nprompt; i++) {
        if (!llm_model_decode(e->model, ids[i], pos, logits)) {
            free(ids);
            return -1;
        }
        pos++;
    }
    free(ids);

    i32 generated = 0;
    while (generated < max_tokens && pos < h->n_ctx) {
        i32 next = llm_sampler_argmax(logits, h->n_vocab);
        if (next < 0) break;
        if (next == eos || next == eot) break;

        char buf[256];
        i32 w = llm_tokenizer_decode(e->tok, next, buf, (i32)sizeof buf);
        if (w > 0 && on_token) {
            if (!on_token(user, buf, w)) break;
        }
        generated++;

        if (!llm_model_decode(e->model, next, pos, logits)) break;
        pos++;
    }
    return generated;
#endif
}

/* ---- diagnostics --------------------------------------------------- */

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
