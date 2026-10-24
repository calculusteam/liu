/*
 * Liu - Whisper vocabulary + special-token resolution.
 *
 * Two things about Whisper's vocabulary are easy to get wrong and both are
 * settled here once:
 *
 *   1. Token strings in the model file are RAW BYTES, not GPT-2 byte-level
 *      BPE symbols. whisper.cpp's converter already applies the byte decoder
 *      (or base64-decodes the tiktoken ranks) before writing, so turning ids
 *      back into text is plain byte concatenation — no "Ġ"-to-space mapping.
 *      The flip side is that a single token can end mid-codepoint (id 464 in
 *      large-v3 is two bytes of a three-byte Cyrillic character), so a caller
 *      that wants to render partial output must buffer bytes and only then
 *      interpret UTF-8.
 *
 *   2. The special tokens are NOT in the file. large-v3-turbo writes 50257
 *      vocabulary entries but declares n_vocab = 51866; everything above the
 *      written table is synthesized. Their ids are the base GPT-2 offsets
 *      shifted by how many languages the model knows, which is itself derived
 *      from n_vocab. Hardcoding 50258 for <|startoftranscript|> happens to be
 *      right for v3 and wrong for every earlier model, so the offsets are
 *      recomputed from the header instead.
 */
#ifndef VOICE_WHISPER_VOCAB_H
#define VOICE_WHISPER_VOCAB_H

#include "core/types.h"
#include "voice/whisper_ggml.h"

typedef struct {
    const WhisperFile *w;      /* borrowed; must outlive the vocab */

    i32 n_vocab;               /* == w->h.n_vocab, incl. synthesized tokens */
    bool multilingual;
    i32 n_languages;

    i32 token_eot;             /* <|endoftext|>          */
    i32 token_sot;             /* <|startoftranscript|>  */
    i32 token_translate;
    i32 token_transcribe;
    i32 token_solm;
    i32 token_prev;
    i32 token_nosp;
    i32 token_not;             /* <|notimestamps|>       */
    i32 token_beg;             /* timestamp 0.00; every id above is a timestamp */
} WhisperVocab;

/* Derive the vocabulary view from an open model. Returns false only when the
 * header is inconsistent (a multilingual claim with fewer languages than the
 * base offsets need). */
bool whisper_vocab_init(WhisperVocab *v, const WhisperFile *w);

/* Language-code ("tr", "en", …) to its token id, or -1 when the code is
 * unknown or the model is English-only. Case-sensitive, lowercase. */
i32 whisper_vocab_lang_token(const WhisperVocab *v, const char *code);

/* ISO code for a language index in Whisper's own order (0 == "en"), or NULL
 * when out of range. whisper_vocab_lang_count() bounds the index. */
const char *whisper_vocab_lang_code(i32 index);
i32         whisper_vocab_lang_count(void);

/* Language index carried by `token`, or -1 when it is not a language token. */
i32 whisper_vocab_token_lang(const WhisperVocab *v, i32 token);

/* True for ids the decoder must never emit as text: everything at or above
 * token_eot. Timestamps live above token_beg and are also filtered by this. */
static inline bool whisper_vocab_is_special(const WhisperVocab *v, i32 token) {
    return token >= v->token_eot;
}

/* Append `token`'s raw bytes to `out` (which holds `*len` bytes and has
 * capacity `cap`). Special/synthesized ids contribute nothing. Returns false
 * when the bytes would not fit; *len is left unchanged in that case. */
bool whisper_vocab_append(const WhisperVocab *v, i32 token,
                          char *out, i32 *len, i32 cap);

#endif /* VOICE_WHISPER_VOCAB_H */
