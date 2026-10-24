#ifndef LLM_CONTROL_H
#define LLM_CONTROL_H
#include "core/types.h"

enum { LLM_ERROR = -1, LLM_CANCELLED = -2 };
/* Polled on the engine worker, including before the first generated token.
 * Latch a request until the call returns. The callback/user storage must
 * outlive the synchronous load/generate call. */
typedef struct {
    bool (*requested)(void *user);
    void *user;
} LlmCancel;

static inline bool llm_cancelled(const LlmCancel *cancel) {
    return cancel && cancel->requested && cancel->requested(cancel->user);
}
#endif
