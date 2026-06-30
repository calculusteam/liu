/*
 * Liu - Hy-MT transformer forward pass (single batch, single thread).
 *
 * Llama-family decoder: RMSNorm -> QKV -> RoPE -> GQA attention with a
 * KV cache -> output proj -> SwiGLU MLP, repeated per layer, then a
 * final RMSNorm + LM head. Every hyperparameter is read from the GGUF
 * KV metadata at create time — nothing about the model is hardcoded.
 *
 * The model holds GgufTensor pointers into the still-mapped GGUF file;
 * the GgufFile passed to llm_model_create() must outlive the model.
 * Weights are dequantized one row at a time during each matvec, so peak
 * RSS stays at (mmap + KV cache + a few activation buffers).
 */
#ifndef LLM_LLM_MODEL_H
#define LLM_LLM_MODEL_H

#include "core/types.h"
#include "llm/llm_gguf.h"

/* KV-cache ceiling. 2048 positions comfortably fits a multi-paragraph
 * translation (prompt + output) while keeping the KV cache modest
 * (~270 MB total for a 1.8B model). */
#define LLM_MODEL_MAX_CTX 2048

typedef struct {
    i32 n_layers;
    i32 n_embd;
    i32 n_head;
    i32 n_head_kv;
    i32 head_dim;
    i32 n_ff;
    i32 n_vocab;
    i32 n_ctx;              /* min(GGUF context_length, LLM_MODEL_MAX_CTX) */
    f32 rope_freq_base;
    f32 rms_eps;
    bool rope_neox;         /* true = NeoX-style rotary, false = adjacent-pair */
    bool qk_norm;           /* true = per-head RMSNorm on Q/K after projection */
    char arch[32];
} LlmHParams;

typedef struct LlmModel LlmModel;

/* Parse every hyperparameter from the GGUF KV metadata into `h` (arch
 * prefix, layer/head/ff dims, head_dim, rope base, n_ctx, n_vocab from
 * the token-embedding table, rope style). `h->qk_norm` is left false —
 * it depends on per-layer tensors and is set by whichever backend
 * resolves them. Returns false on missing keys or an invalid shape.
 * Shared by the CPU (llm_model) and Metal (llm_metal) backends. */
bool llm_hparams_load(const GgufFile *g, LlmHParams *h);

/* Build a model view over an open GGUF file. Returns NULL on missing
 * tensors/metadata, an unsupported dtype, or allocation failure. */
LlmModel *llm_model_create(const GgufFile *g);
void      llm_model_free(LlmModel *m);

const LlmHParams *llm_model_hparams(const LlmModel *m);

/* Decode one step: feed `token_id` at absolute position `pos` (fed in
 * order from 0). Writes n_vocab logits into `logits`. Returns false if
 * pos >= n_ctx or on internal error. */
bool llm_model_decode(LlmModel *m, i32 token_id, i32 pos, f32 *logits);

/* Drop the KV cache to start a fresh sequence. */
void llm_model_reset(LlmModel *m);

#endif /* LLM_LLM_MODEL_H */
