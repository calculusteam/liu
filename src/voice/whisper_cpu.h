/*
 * Portable C implementation of the Whisper forward pass, used as the
 * fallback backend. The encoder runs once per 30-second window; each
 * generated token runs through the decoder layers using cross-attention K/V
 * computed from the encoder output.
 *
 * The encode/decode split follows the model's execution stages and allows
 * the engine to reuse encoder output across generated tokens.
 */
#ifndef VOICE_WHISPER_CPU_H
#define VOICE_WHISPER_CPU_H

#include "core/types.h"
#include "voice/whisper_model.h"

typedef struct WhisperCpu WhisperCpu;

/* `w` is borrowed and must outlive the returned backend. `n_threads` <= 0
 * picks a default from the machine. */
WhisperCpu *whisper_cpu_create(const WhisperWeights *w, i32 n_threads,
                               char *err, usize err_cap);
void        whisper_cpu_free(WhisperCpu *c);
typedef bool (*WhisperCpuCancelFn)(void *user);
/* Checked by the calling thread between complete kernels, never by lanes. */
void whisper_cpu_set_cancel(WhisperCpu *c, WhisperCpuCancelFn fn, void *user);

/* Run the encoder over one 30 s window (`mel` is n_mels * WHISPER_N_FRAMES,
 * mel-major) and pre-compute every decoder layer's cross-attention K/V.
 * Also clears the self-attention KV cache. */
bool whisper_cpu_encode(WhisperCpu *c, const f32 *mel);

/* Feed `n` tokens at absolute positions [n_past, n_past+n) and write
 * n_vocab logits for the LAST of them. Requires a preceding encode. */
bool whisper_cpu_decode(WhisperCpu *c, const i32 *tokens, i32 n, i32 n_past,
                        f32 *logits);

/* Drop the self-attention KV cache without re-running the encoder — used to
 * restart decoding of the same window with a different prompt. */
void whisper_cpu_decode_reset(WhisperCpu *c);

/* Encoder output, [n_audio_ctx * n_state]. Exposed so the Metal backend can
 * be diffed against this one tensor rather than only on final text. */
const f32 *whisper_cpu_encoder_out(const WhisperCpu *c);

#endif /* VOICE_WHISPER_CPU_H */
