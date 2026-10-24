/*
 * Liu - token sampler. v1 is greedy argmax (temperature 0) — the Hy-MT
 * translation path is deterministic by design.
 */
#ifndef LLM_LLM_SAMPLER_H
#define LLM_LLM_SAMPLER_H

#include "core/types.h"

/* Return the index of the largest logit. Returns -1 for n <= 0. */
i32 llm_sampler_argmax(const f32 *logits, i32 n);

#endif /* LLM_LLM_SAMPLER_H */
