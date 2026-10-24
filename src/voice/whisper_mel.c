/*
 * Liu - Whisper log-mel spectrogram front end.
 *
 * The transform mirrors whisper.cpp's log_mel_spectrogram step for step,
 * including its recursive Cooley-Tukey shape. That is deliberate: 400 is
 * 16*25, so the radix-2 recursion bottoms out at a 25-point naive DFT, and
 * the twiddles are read from one 400-entry table by stride rather than
 * recomputed. Reordering the arithmetic would drift from the reference in
 * the last few bits for no gain.
 *
 * Zero-padding the frame to 512 to get a "clean" power-of-two FFT is the
 * tempting mistake here: it changes the bin spacing from 40 Hz to 31.25 Hz
 * and the model's 201-bin filterbank then integrates the wrong frequencies.
 */
#include "voice/whisper_mel.h"
#include "core/checked.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Twiddle table length == frame size, so every recursion level indexes it
 * with the stride SIN_COS_N / N. */
#define MEL_SINCOS_N WHISPER_N_FFT

/* fft() uses the tail of its input buffer as even/odd scratch and writes its
 * recursion's partial results past its own output. These are the sizes
 * whisper.cpp uses; the deepest 400-point call reaches float 775 of `in` and
 * float 2300 of `out`, so both are comfortable. */
#define MEL_FFT_IN_CAP  (WHISPER_N_FFT * 2)
#define MEL_FFT_OUT_CAP (WHISPER_N_FFT * 2 * 2 * 2)

typedef struct {
    f32 sin_vals[MEL_SINCOS_N];
    f32 cos_vals[MEL_SINCOS_N];
    f32 hann[WHISPER_N_FFT];
} MelTables;

static void mel_tables_init(MelTables *t) {
    for (i32 i = 0; i < MEL_SINCOS_N; i++) {
        f64 theta = (2.0 * M_PI * i) / MEL_SINCOS_N;
        t->sin_vals[i] = sinf((f32)theta);
        t->cos_vals[i] = cosf((f32)theta);
    }
    /* Periodic Hann: the divisor is the length, not length-1. */
    for (i32 i = 0; i < WHISPER_N_FFT; i++) {
        t->hann[i] = (f32)(0.5 * (1.0 - cosf((f32)((2.0 * M_PI * i) / WHISPER_N_FFT))));
    }
}

/* Naive DFT — the odd-size base case of the recursion below. */
static void mel_dft(const MelTables *t, const f32 *in, i32 N, f32 *out) {
    const i32 step = MEL_SINCOS_N / N;
    for (i32 k = 0; k < N; k++) {
        f32 re = 0, im = 0;
        for (i32 n = 0; n < N; n++) {
            i32 idx = (k * n * step) % MEL_SINCOS_N;
            re += in[n] * t->cos_vals[idx];
            im -= in[n] * t->sin_vals[idx];
        }
        out[k * 2 + 0] = re;
        out[k * 2 + 1] = im;
    }
}

/* Real-input, complex-output FFT. `in` must have 2*N floats of room: the
 * upper half is used as the de-interleaving scratch. */
static void mel_fft(const MelTables *t, f32 *in, i32 N, f32 *out) {
    if (N == 1) { out[0] = in[0]; out[1] = 0; return; }

    const i32 half = N / 2;
    if (N - half * 2 == 1) { mel_dft(t, in, N, out); return; }

    f32 *even = in + N;
    for (i32 i = 0; i < half; i++) even[i] = in[2 * i];
    f32 *even_fft = out + 2 * N;
    mel_fft(t, even, half, even_fft);

    /* `even` is dead once its transform is out, so odd reuses the scratch. */
    f32 *odd = even;
    for (i32 i = 0; i < half; i++) odd[i] = in[2 * i + 1];
    f32 *odd_fft = even_fft + N;
    mel_fft(t, odd, half, odd_fft);

    const i32 step = MEL_SINCOS_N / N;
    for (i32 k = 0; k < half; k++) {
        i32 idx = k * step;
        f32 re = t->cos_vals[idx];
        f32 im = -t->sin_vals[idx];

        f32 re_odd = odd_fft[2 * k + 0];
        f32 im_odd = odd_fft[2 * k + 1];

        out[2 * k + 0] = even_fft[2 * k + 0] + re * re_odd - im * im_odd;
        out[2 * k + 1] = even_fft[2 * k + 1] + re * im_odd + im * re_odd;

        out[2 * (k + half) + 0] = even_fft[2 * k + 0] - re * re_odd + im * im_odd;
        out[2 * (k + half) + 1] = even_fft[2 * k + 1] - re * im_odd - im * re_odd;
    }
}

void whisper_mel_free(WhisperMel *m) {
    if (!m) return;
    free(m->data);
    memset(m, 0, sizeof *m);
}

bool whisper_mel_compute(const f32 *samples, i32 n_samples,
                         const f32 *filters, i32 n_mel, i32 n_fft_bins,
                         WhisperMel *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!filters || n_mel <= 0) return false;
    if (n_samples < 0) return false;
    if (n_fft_bins != 1 + WHISPER_N_FFT / 2) return false;
    if (!samples && n_samples > 0) return false;

    /* Padding: 200 reflected samples in front (so frame i is centred on
     * sample i*160), then the signal, then a full 30 s of silence plus 200
     * so the last real frame has a complete window. */
    const i32 pad_front = WHISPER_N_FFT / 2;          /* 200    */
    const i32 pad_tail  = WHISPER_N_SAMPLES;          /* 480000 */

    usize padded_n = 0;
    if (!checked_add_usize((usize)n_samples, (usize)pad_tail, &padded_n) ||
        !checked_add_usize(padded_n, (usize)pad_front * 2, &padded_n)) {
        return false;
    }

    f32 *padded = calloc(padded_n, sizeof *padded);
    if (!padded) return false;

    if (n_samples > 0) memcpy(padded + pad_front, samples, (usize)n_samples * sizeof *samples);

    /* Reflect the front. Clamped to what the input actually holds so a
     * recording shorter than the pad cannot read past `samples`. */
    i32 n_reflect = n_samples - 1;
    if (n_reflect > pad_front) n_reflect = pad_front;
    if (n_reflect < 0) n_reflect = 0;
    for (i32 i = 0; i < n_reflect; i++) {
        padded[pad_front - n_reflect + i] = samples[n_reflect - i];
    }

    /* Frames that carry real audio end where the signal plus its front pad
     * does; everything after is the silence tail. */
    const i32 n_eff = n_samples + pad_front;
    const i32 n_len = (i32)((padded_n - WHISPER_N_FFT) / WHISPER_HOP_LENGTH);
    if (n_len <= 0) { free(padded); return false; }

    usize cells = 0;
    if (!checked_mul_usize((usize)n_mel, (usize)n_len, &cells)) { free(padded); return false; }
    f32 *mel = malloc(cells * sizeof *mel);
    if (!mel) { free(padded); return false; }

    MelTables *t = malloc(sizeof *t);
    if (!t) { free(padded); free(mel); return false; }
    mel_tables_init(t);

    f32 *fft_in  = calloc(MEL_FFT_IN_CAP, sizeof *fft_in);
    f32 *fft_out = calloc(MEL_FFT_OUT_CAP, sizeof *fft_out);
    if (!fft_in || !fft_out) {
        free(fft_in); free(fft_out); free(t); free(padded); free(mel);
        return false;
    }

    i32 n_active = n_eff / WHISPER_HOP_LENGTH + 1;
    if (n_active > n_len) n_active = n_len;

    for (i32 i = 0; i < n_active; i++) {
        const i32 offset = i * WHISPER_HOP_LENGTH;

        i32 span = n_eff - offset;
        if (span > WHISPER_N_FFT) span = WHISPER_N_FFT;
        if (span < 0) span = 0;

        for (i32 j = 0; j < span; j++) fft_in[j] = t->hann[j] * padded[offset + j];
        for (i32 j = span; j < MEL_FFT_IN_CAP; j++) fft_in[j] = 0.0f;

        mel_fft(t, fft_in, WHISPER_N_FFT, fft_out);

        /* Power spectrum, folded in place. Safe because bin j is written at
         * index j while its source pair lives at 2j / 2j+1. */
        for (i32 j = 0; j < n_fft_bins; j++) {
            fft_out[j] = fft_out[2 * j + 0] * fft_out[2 * j + 0] +
                         fft_out[2 * j + 1] * fft_out[2 * j + 1];
        }

        for (i32 j = 0; j < n_mel; j++) {
            const f32 *row = filters + (usize)j * (usize)n_fft_bins;
            f64 sum = 0.0;
            for (i32 k = 0; k < n_fft_bins; k++) sum += (f64)fft_out[k] * (f64)row[k];
            if (sum < 1e-10) sum = 1e-10;
            mel[(usize)j * (usize)n_len + (usize)i] = (f32)log10(sum);
        }
    }

    /* The silence tail is constant, so it is filled rather than transformed. */
    const f32 floor_val = (f32)log10(1e-10);
    for (i32 i = n_active; i < n_len; i++) {
        for (i32 j = 0; j < n_mel; j++) mel[(usize)j * (usize)n_len + (usize)i] = floor_val;
    }

    /* Global clamp + scale. Deliberately over the whole matrix, tail
     * included — see the header. */
    f32 mmax = -1e20f;
    for (usize i = 0; i < cells; i++) if (mel[i] > mmax) mmax = mel[i];
    mmax -= 8.0f;
    for (usize i = 0; i < cells; i++) {
        f32 v = mel[i] < mmax ? mmax : mel[i];
        mel[i] = (v + 4.0f) / 4.0f;
    }

    free(fft_in);
    free(fft_out);
    free(t);
    free(padded);

    out->n_mel     = n_mel;
    out->n_len     = n_len;
    out->n_len_org = 1 + (n_samples + pad_front - WHISPER_N_FFT) / WHISPER_HOP_LENGTH;
    if (out->n_len_org < 0) out->n_len_org = 0;
    if (out->n_len_org > n_len) out->n_len_org = n_len;
    out->data = mel;
    return true;
}

bool whisper_mel_window(const WhisperMel *m, i32 offset, f32 *dst) {
    if (!m || !m->data || !dst) return false;
    if (offset < 0 || offset >= m->n_len) return false;

    const i32 n_len = m->n_len;
    for (i32 j = 0; j < m->n_mel; j++) {
        const f32 *src = m->data + (usize)j * (usize)n_len;
        f32       *row = dst + (usize)j * (usize)WHISPER_N_FRAMES;

        i32 avail = n_len - offset;
        if (avail > WHISPER_N_FRAMES) avail = WHISPER_N_FRAMES;
        memcpy(row, src + offset, (usize)avail * sizeof *row);

        /* Past the end, repeat the last column. It is inside the transform's
         * own silence tail, so it already holds the normalized floor — the
         * same value the reference would have produced there. */
        for (i32 i = avail; i < WHISPER_N_FRAMES; i++) row[i] = src[n_len - 1];
    }
    return true;
}
