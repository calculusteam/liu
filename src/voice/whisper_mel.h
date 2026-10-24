/*
 * Whisper log-mel spectrogram front end.
 *
 * Converts 16 kHz mono f32 PCM into the 128 x N log-mel matrix expected by
 * the encoder. The transform must match the model's training input.
 *
 * Fixed transform requirements:
 *   - The Hann window is periodic (divide by N, not N - 1).
 *   - The filterbank comes from the model file and uses Whisper's
 *     librosa/Slaney normalization.
 *   - Use power magnitude |X|^2 and log10 with a 1e-10 floor.
 *   - Normalize globally across the padded matrix: clamp to max - 8, then
 *     apply (x + 4) / 4.
 *   - Reflect-pad 200 samples at the front and zero-pad 30 seconds plus 200
 *     samples at the end, centering frame i on sample i*160.
 *
 * Output is mel-major: mel[j * n_len + i] is bin j of frame i. This is the
 * layout expected by the encoder's first convolution and used by
 * whisper.cpp.
 */
#ifndef VOICE_WHISPER_MEL_H
#define VOICE_WHISPER_MEL_H

#include "core/types.h"

#define WHISPER_SAMPLE_RATE 16000
#define WHISPER_N_FFT       400     /* 25 ms window                        */
#define WHISPER_HOP_LENGTH  160     /* 10 ms hop                           */
#define WHISPER_CHUNK_SEC   30
#define WHISPER_N_SAMPLES   (WHISPER_SAMPLE_RATE * WHISPER_CHUNK_SEC)   /* 480000 */
#define WHISPER_N_FRAMES    (WHISPER_N_SAMPLES / WHISPER_HOP_LENGTH)    /*   3000 */

typedef struct {
    i32  n_mel;
    i32  n_len;        /* frames in `data`, incl. the 30 s zero tail       */
    i32  n_len_org;    /* frames backed by real audio (before the tail)    */
    f32 *data;         /* heap [n_mel * n_len], mel-major                  */
} WhisperMel;

/* Compute the log-mel matrix of `samples` (16 kHz mono, in [-1,1]).
 * `filters` is the model's [n_mel * n_fft_bins] bank with n_fft_bins ==
 * 1 + WHISPER_N_FFT/2. Returns false on allocation failure or a filterbank
 * whose bin count does not match the transform. On success the caller owns
 * `out->data` and frees it with whisper_mel_free. */
bool whisper_mel_compute(const f32 *samples, i32 n_samples,
                         const f32 *filters, i32 n_mel, i32 n_fft_bins,
                         WhisperMel *out);

void whisper_mel_free(WhisperMel *m);

/* Copy the 3000-frame window starting at frame `offset` into `dst`
 * (n_mel * WHISPER_N_FRAMES floats, mel-major). Frames past the end of the
 * matrix are filled with the padding value the transform produces for
 * silence, so a short recording yields the same input the reference does.
 * Returns false when `offset` is out of range. */
bool whisper_mel_window(const WhisperMel *m, i32 offset, f32 *dst);

#endif /* VOICE_WHISPER_MEL_H */
