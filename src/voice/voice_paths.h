/*
 * Shared filename, expected size, URL, and Application Support directory for
 * the Whisper model. The presence check and downloader use the same values.
 * The default model is whisper.cpp large-v3-turbo Q5_0.
 */
#ifndef VOICE_VOICE_PATHS_H
#define VOICE_VOICE_PATHS_H

#include "core/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef LIU_WHISPER_FILENAME
#define LIU_WHISPER_FILENAME "ggml-large-v3-turbo-q5_0.bin"
#endif

#ifndef LIU_WHISPER_URL
#define LIU_WHISPER_URL \
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin"
#endif

#ifndef LIU_WHISPER_SHA256
#define LIU_WHISPER_SHA256 \
    "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2"
#endif

#ifndef LIU_WHISPER_SIZE
#define LIU_WHISPER_SIZE 574041195ULL
#endif

/* Same models directory as the Translate GGUF. */
#include "core/app_support.h"
#define LIU_WHISPER_SUBDIR LIU_APP_SUPPORT_SUBDIR "/models"

/* Write "$HOME/<subdir>/<filename>" into `out`. Returns `out`, or NULL (and
 * empties `out`) when $HOME is unavailable. */
static inline const char *liu_whisper_default_path(char *out, usize cap) {
    if (!out || cap == 0) return NULL;
    const char *home = getenv("HOME");
    if (!home || !home[0]) { out[0] = '\0'; return NULL; }
    snprintf(out, cap, "%s/%s/%s", home, LIU_WHISPER_SUBDIR, LIU_WHISPER_FILENAME);
    return out;
}

/* Accept regular files of at least 16 MiB rather than requiring an exact
 * match to LIU_WHISPER_SIZE. Multiple Whisper model sizes and quantizations
 * are supported. whisper_open() validates the header and tensor shapes; this
 * check only rejects truncated or partial files. */
static inline bool liu_whisper_file_ok(const char *path) {
    if (!path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    return (u64)st.st_size >= (u64)(16u << 20);   /* smallest real model is ~75 MB */
}

/* Decoder depth, or 0 when `path` is not a readable whisper ggml file.
 *
 * Reads only the fixed header — magic plus eleven i32s, in the order
 * whisper_ggml.h documents — so Settings can ask about a 547 MB model without
 * mapping it or walking its tensor stream. */
static inline i32 liu_whisper_decoder_layers(const char *path) {
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    u32 hdr[12];
    const bool ok = fread(hdr, sizeof hdr[0], 12, f) == 12;
    fclose(f);
    if (!ok || hdr[0] != 0x67676d6cu) return 0;   /* "ggml" */
    /* [0] magic [1] n_vocab [2] n_audio_ctx [3] n_audio_state [4] n_audio_head
     * [5] n_audio_layer [6] n_text_ctx [7] n_text_state [8] n_text_head
     * [9] n_text_layer ... */
    return (i32)hdr[9];
}

/* Can this model perform Whisper's TRANSLATE task (speech -> English)?
 *
 * It is not a flag in the file, but the decoder depth answers it. The
 * distilled builds — large-v3-turbo (4 decoder layers, pruned from 32) and the
 * distil-whisper family (2) — were trained on transcription ALONE, and asked
 * to translate they do not fail: they emit degenerate output. Every model that
 * does carry the task keeps its full decoder — 12 for small, 24 for medium, 32
 * for large — so the cutoff sits in a wide empty gap rather than on a
 * boundary, and it holds for future distillations for the same reason they are
 * distillations: the decoder is what gets pruned. */
#define LIU_WHISPER_TRANSLATE_MIN_DECODER_LAYERS 8

static inline bool liu_whisper_can_translate(const char *path) {
    i32 n = liu_whisper_decoder_layers(path);
    return n >= LIU_WHISPER_TRANSLATE_MIN_DECODER_LAYERS;
}

#endif /* VOICE_VOICE_PATHS_H */
