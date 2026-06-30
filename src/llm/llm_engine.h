/*
 * Liu - local LLM engine: GGUF + tokenizer + transformer + sampler glued
 * into a load / generate / free triple.
 *
 * Single-threaded and blocking — translate_local.c runs it on a worker
 * thread so the UI never stalls. Greedy decode (temperature 0).
 */
#ifndef LLM_LLM_ENGINE_H
#define LLM_LLM_ENGINE_H

#include "core/types.h"

typedef struct LlmEngine LlmEngine;

/* Load a GGUF model. Returns NULL on failure; when `err`/`err_cap` are
 * provided, a short human-readable reason is written there. */
LlmEngine *llm_engine_load(const char *path, char *err, usize err_cap);
void       llm_engine_free(LlmEngine *e);

/* Streaming callback — receives each generated token's decoded UTF-8
 * bytes (`len` bytes, not NUL-terminated). Return false to stop early. */
typedef bool (*LlmTokenFn)(void *user, const char *utf8, i32 len);

/* Run `prompt` through the model and greedily generate up to
 * `max_tokens` (or until EOS / context full). Each token's text is
 * streamed via `on_token`. Returns the number of tokens generated, or
 * -1 on error. */
i32 llm_engine_generate(LlmEngine *e, const char *prompt, i32 max_tokens,
                        LlmTokenFn on_token, void *user);

/* Diagnostics. */
const char *llm_engine_arch(const LlmEngine *e);
i32         llm_engine_tensor_count(const LlmEngine *e);
i32         llm_engine_n_ctx(const LlmEngine *e);
/* "metal" or "cpu" — which forward-pass backend this build uses. The
 * backend is fixed at compile time; a normal build is always "metal". */
const char *llm_engine_backend(const LlmEngine *e);

#endif /* LLM_LLM_ENGINE_H */
