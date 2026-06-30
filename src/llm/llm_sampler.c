/*
 * Liu - greedy argmax sampler.
 */
#include "llm/llm_sampler.h"

i32 llm_sampler_argmax(const f32 *logits, i32 n) {
    if (!logits || n <= 0) return -1;
    i32 best = 0;
    f32 bestv = logits[0];
    for (i32 i = 1; i < n; i++) {
        if (logits[i] > bestv) {
            bestv = logits[i];
            best = i;
        }
    }
    return best;
}
