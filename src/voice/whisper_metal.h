/*
 * Metal backend for Whisper inference. It mirrors the CPU backend
 * interface so `whisper_engine.c` can dispatch to either implementation.
 * The backend creates its own `MTLDevice` and queue, independent of the
 * renderer, so it can be initialized on the voice worker thread. A NULL
 * return allows the caller to fall back to the CPU backend. The
 * declarations remain visible when `LIU_WHISPER_METAL` is not defined so
 * call sites can guard with that macro.
 */
#ifndef VOICE_WHISPER_METAL_H
#define VOICE_WHISPER_METAL_H

#include "core/types.h"
#include "voice/whisper_model.h"

typedef struct WhisperMetal WhisperMetal;

/* Build a Metal backend over a resolved weight table. `w` (and the model
 * file behind it) must outlive the returned handle only until this call
 * returns: the weights are copied into device buffers here. Returns NULL on
 * any failure, with a reason in `err` when provided. */
WhisperMetal *whisper_metal_create(const WhisperWeights *w, char *err, usize err_cap);
void          whisper_metal_free(WhisperMetal *m);

/* Same contract as the CPU backend. */
bool whisper_metal_encode(WhisperMetal *m, const f32 *mel);
bool whisper_metal_decode(WhisperMetal *m, const i32 *tokens, i32 n, i32 n_past,
                          f32 *logits);
void whisper_metal_decode_reset(WhisperMetal *m);

#endif /* VOICE_WHISPER_METAL_H */
