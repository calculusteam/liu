/*
 * Liu - Metal-accelerated forward pass for the local LLM engine.
 *
 * Mirrors llm_model.h's CPU interface but runs the whole per-token
 * forward pass on the GPU. Quantized weights (Q6_K + STQ1_0) are copied
 * once into a single device buffer and dequantized inside the matvec
 * kernels — no F16 expansion, so a 1.8B model stays near ~500 MB on the
 * GPU and runs on any Apple Silicon.
 *
 * Independent of the renderer: creates its own MTLDevice / queue, so it
 * is safe to build on the translate worker thread. A NULL return is a
 * fatal load error — the engine is GPU-only and does not fall back.
 *
 * The implementation (llm_metal.m) is compiled only when
 * LIU_LLM_METAL is defined; declarations stay visible so call sites can
 * guard with that macro.
 */
#ifndef LLM_LLM_METAL_H
#define LLM_LLM_METAL_H

#include "core/types.h"
#include "llm/llm_gguf.h"
#include "llm/llm_model.h"   /* LlmHParams */

typedef struct LlmMetal LlmMetal;

/* True if a usable Metal device + the LLM metallib are present. */
bool llm_metal_available(void);

/* Build a Metal-backed model over an open GGUF file. Returns NULL on any
 * failure (no device, metallib missing, tensor missing/mis-shaped,
 * unsupported dtype, allocation failure); llm_engine_load treats that as
 * a fatal error. The GGUF tensor-data blob is copied into a device
 * buffer here, so `g` need not outlive the returned handle. */
LlmMetal *llm_metal_create(const GgufFile *g);
void      llm_metal_free(LlmMetal *m);

const LlmHParams *llm_metal_hparams(const LlmMetal *m);

/* Per-generated-token callback. Receives the raw token id; return false
 * to stop generation early. */
typedef bool (*LlmMetalTokenFn)(void *user, i32 token_id);

/* Run the whole autoregressive loop on the GPU: feed `prompt_ids`
 * (n_prompt of them) to populate the KV cache, then greedily generate up
 * to `max_gen` tokens, invoking `on_id` for each. Stops on `eos`/`eot`
 * or when `on_id` returns false. The per-token loop stays GPU-resident
 * (embedding, argmax and the next-token hand-off all happen on-device)
 * and command buffers are pipelined so the GPU never idles between
 * tokens. Returns the number of tokens generated, or -1 on error. */
i32 llm_metal_generate(LlmMetal *m, const i32 *prompt_ids, i32 n_prompt,
                       i32 max_gen, i32 eos, i32 eot,
                       LlmMetalTokenFn on_id, void *user);

#endif /* LLM_LLM_METAL_H */
