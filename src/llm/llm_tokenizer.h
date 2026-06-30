/*
 * Liu - GPT-2-style byte-level BPE tokenizer for the Hy-MT model.
 *
 * Vocab + merges come straight from the GGUF tokenizer.ggml.* metadata
 * (tokens / merges / token ids). Token and merge strings are referenced
 * in-place from the still-mapped GGUF file — the tokenizer must not
 * outlive the GgufFile it was built from.
 *
 * Supported tokenizer model: "gpt2" (byte-level BPE). SentencePiece
 * ("llama"/"spm") is rejected at create time with a NULL return.
 */
#ifndef LLM_LLM_TOKENIZER_H
#define LLM_LLM_TOKENIZER_H

#include "core/types.h"
#include "llm/llm_gguf.h"

typedef struct LlmTokenizer LlmTokenizer;

/* Build from an open GGUF file. Returns NULL on missing metadata or an
 * unsupported tokenizer model. */
LlmTokenizer *llm_tokenizer_create(const GgufFile *g);
void          llm_tokenizer_free(LlmTokenizer *t);

/* Encode UTF-8 `text` into token ids. Writes at most `cap` ids into
 * `out_ids`; returns the number actually written (input past `cap` is
 * dropped). Does not add BOS/EOS — the engine handles that. */
i32 llm_tokenizer_encode(const LlmTokenizer *t, const char *text,
                         i32 *out_ids, i32 cap);

/* Decode a single token id into raw UTF-8 bytes. Writes up to `cap`
 * bytes into `out`, returns the byte count (0 for an out-of-range id or
 * a control/added token with no printable form). */
i32 llm_tokenizer_decode(const LlmTokenizer *t, i32 id, char *out, i32 cap);

i32 llm_tokenizer_bos(const LlmTokenizer *t);
i32 llm_tokenizer_eos(const LlmTokenizer *t);
i32 llm_tokenizer_vocab_size(const LlmTokenizer *t);

/* Direct vocab lookup of a verbatim token string — used to resolve
 * special / control tokens (e.g. "<|hy_User|>") that must be emitted as
 * a single id rather than run through BPE. Returns -1 if not in vocab. */
i32 llm_tokenizer_token_id(const LlmTokenizer *t, const char *literal);

#endif /* LLM_LLM_TOKENIZER_H */
