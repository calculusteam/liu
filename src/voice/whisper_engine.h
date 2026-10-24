/*
 * Liu - Whisper speech-to-text engine: load a model, hand it audio, get text.
 *
 * Everything below the API — the container parser, the mel front end, the
 * weight table, the two backends and the greedy decoder with Whisper's
 * timestamp rules — is private to this module. Callers see load / transcribe
 * / free, mirroring llm_engine.h.
 *
 * Blocking and single-threaded from the caller's point of view (it fans out
 * internally), so it runs on the voice worker thread, never the UI thread.
 */
#ifndef VOICE_WHISPER_ENGINE_H
#define VOICE_WHISPER_ENGINE_H

#include "core/types.h"

typedef struct WhisperEngine WhisperEngine;

typedef struct {
    /* ISO code ("tr", "en", …). Empty or NULL auto-detects from the first
     * 30 s window, which costs one extra decoder step. */
    char lang[8];
    /* Emit English regardless of what was spoken (Whisper's translate task)
     * instead of transcribing verbatim. */
    bool translate;
    /* Hard ceiling on generated tokens per 30 s window; <= 0 uses the
     * model's own text context. Guards against the repetition loops greedy
     * decoding can fall into on noise. */
    i32 max_tokens;
} WhisperParams;

WhisperParams whisper_params_default(void);

/* Load a whisper.cpp-format model (`ggml-*.bin`). Returns NULL on failure
 * with a short reason in `err`. */
WhisperEngine *whisper_engine_load(const char *path, char *err, usize err_cap);
void           whisper_engine_free(WhisperEngine *e);

/* Transcribe 16 kHz mono f32 PCM. Writes NUL-terminated UTF-8 into `out` and
 * returns its byte length, or -1 on error. Audio longer than 30 s is
 * processed as successive windows, seeking by the timestamps the model
 * emits rather than by a fixed stride, so words are not cut at the seam. */
i32 whisper_engine_transcribe(WhisperEngine *e,
                              const f32 *samples, i32 n_samples,
                              const WhisperParams *p,
                              char *out, i32 out_cap);

/* Ask the caller mid-transcription whether to keep going. Returning false
 * stops at a safe boundary (CPU encoder kernels, GPU encoder completion,
 * or decoder tokens) and yields already committed text. The fraction may
 * repeat while processing a window. NULL (the default) never aborts. */
typedef bool (*WhisperProgressFn)(void *user, f32 fraction);
void whisper_engine_set_progress(WhisperEngine *e, WhisperProgressFn fn, void *user);

/* "metal" or "cpu" — which backend this engine actually built. */
const char *whisper_engine_backend(const WhisperEngine *e);

/* True when this model includes Whisper's TRANSLATE task (speech to
 * English). Distilled builds lack this task; `liu_whisper_can_translate()`
 * in `voice_paths.h` derives support from decoder depth. When unsupported,
 * `WhisperParams.translate` is downgraded to transcription, so callers can
 * query this function before deciding whether to warn. */
bool whisper_engine_can_translate(const WhisperEngine *e);

/* Language the last transcription ran with (auto-detected or requested),
 * as an ISO code. Empty before the first call. */
const char *whisper_engine_language(const WhisperEngine *e);

#endif /* VOICE_WHISPER_ENGINE_H */
