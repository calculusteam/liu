#ifndef LLM_PRETOKENIZER_H
#define LLM_PRETOKENIZER_H
#include "core/types.h"

typedef enum { LLM_PRE_GPT2, LLM_PRE_HUNYUAN } LlmPreType;
typedef struct {
    const u8 *p, *end, *number_end, *segment_end;
    LlmPreType type;
} LlmPreTokenizer;

void llm_pre_init(LlmPreTokenizer *it, const char *text, LlmPreType type);
bool llm_pre_next(LlmPreTokenizer *it, const u8 **start, usize *len);
#endif
