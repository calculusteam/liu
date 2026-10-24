/*
 * Whisper ggml container parser for large-v3-turbo model files.
 *
 * These files use a flat little-endian binary layout, not GGUF, so this parser
 * is separate from src/llm/llm_gguf.c. Layout, in file order, all integers
 * little-endian i32:
 *
 *   magic 0x67676d6c ("ggml")
 *   n_vocab n_audio_ctx n_audio_state n_audio_head n_audio_layer
 *   n_text_ctx n_text_state n_text_head n_text_layer n_mels ftype
 *   n_mel n_fft  +  n_mel*n_fft f32 mel-filterbank coefficients
 *   n_vocab_tok  +  per token: i32 len, len raw bytes (no NUL)
 *   then tensors until EOF, each:
 *     i32 n_dims, name_len, dtype
 *     i32 ne[n_dims]        (ggml order: ne[0] is the fastest-moving axis)
 *     char name[name_len]   (no NUL)
 *     raw tensor bytes
 *
 * Two properties of that layout drive the API:
 *
 *   - There is NO alignment padding anywhere. A tensor's payload starts
 *     wherever the preceding one ended, so a mapped block pointer is very
 *     often misaligned. Nothing here (and nothing downstream) may cast a
 *     mapped pointer to a struct or a f32* — read through memcpy.
 *   - The tensor table is a stream, not an index: the names are only known
 *     after a full walk. whisper_open() does that walk once and records a
 *     name -> {ptr,dims,dtype} row per tensor.
 *
 * The file is untrusted input: every read is bounds-checked against the
 * mapping, and a malformed file can at worst make whisper_open() fail.
 */
#ifndef VOICE_WHISPER_GGML_H
#define VOICE_WHISPER_GGML_H

#include "core/types.h"

/* ggml tensor dtype ids as written by whisper.cpp's converter/quantizer.
 * Only the ones the official model files actually use are accepted. */
enum {
    WGGML_TYPE_F32  = 0,
    WGGML_TYPE_F16  = 1,
    WGGML_TYPE_Q4_0 = 2,
    WGGML_TYPE_Q4_1 = 3,
    WGGML_TYPE_Q5_0 = 6,
    WGGML_TYPE_Q5_1 = 7,
    WGGML_TYPE_Q8_0 = 8,
};

/* Longest tensor name in the model is
 * "decoder.blocks.NN.cross_attn_ln.weight" — 64 is comfortable. */
#define WHISPER_TENSOR_NAME_CAP 64

typedef struct {
    char      name[WHISPER_TENSOR_NAME_CAP];
    i32       n_dims;
    i32       ne[4];        /* ggml order, unused axes are 1 */
    i32       dtype;        /* WGGML_TYPE_* */
    const u8 *data;         /* into the mapping; MAY BE MISALIGNED */
    u64       nbytes;
} WhisperTensor;

typedef struct {
    i32 n_vocab;
    i32 n_audio_ctx;
    i32 n_audio_state;
    i32 n_audio_head;
    i32 n_audio_layer;
    i32 n_text_ctx;
    i32 n_text_state;
    i32 n_text_head;
    i32 n_text_layer;
    i32 n_mels;
    i32 ftype;              /* quantization-version factor already stripped */
} WhisperHParams;

/* One vocabulary entry: a pointer into the mapping plus its length. Token
 * strings are raw GPT-2 byte-level BPE pieces, not NUL-terminated and not
 * necessarily valid UTF-8 on their own. */
typedef struct { const char *p; i32 len; } WhisperTokenStr;

typedef struct {
    int             fd;
    const u8       *map;
    usize           map_size;
    /* What this mapping last reported to MEM_VOICE: its RESIDENT size, not
     * map_size. See whisper_ggml.c's whisper_map_resident(). */
    usize           accounted;

    WhisperHParams  h;

    /* Mel filterbank straight from the model file — Whisper's filters are a
     * librosa/Slaney bank that must not be re-derived, and shipping them in
     * the model is exactly why the format carries them. Copied out of the
     * mapping (rather than pointed at) because the payload is unaligned. */
    i32             n_mel;
    i32             n_fft;      /* 1 + WHISPER_N_FFT/2 = 201 */
    f32            *filters;    /* heap [n_mel * n_fft] */

    /* Vocabulary as written in the file. May be SHORTER than h.n_vocab —
     * whisper.cpp synthesizes the trailing special tokens. */
    i32              n_vocab_file;
    WhisperTokenStr *vocab;     /* heap [n_vocab_file], points into the map */

    i32             tensor_count;
    WhisperTensor  *tensors;    /* heap [tensor_count] */
} WhisperFile;

/* Open + mmap + parse. Returns false (and leaves *w zeroed) on any I/O or
 * malformed-file error; when `err`/`err_cap` are given a short reason is
 * written there. */
bool whisper_open(WhisperFile *w, const char *path, char *err, usize err_cap);
void whisper_close(WhisperFile *w);
/* Refresh the mapped weights' resident charge after backend loading or
 * inference touches new pages. Owner-thread only, like open/close. */
void whisper_refresh_residency(WhisperFile *w);

/* Exact-name tensor lookup. Returns NULL when absent. */
const WhisperTensor *whisper_tensor(const WhisperFile *w, const char *name);

/* printf-style lookup for the per-layer names ("encoder.blocks.%d.attn.query.weight").
 * Returns NULL when the formatted name is absent or would not fit. */
const WhisperTensor *whisper_tensor_fmt(const WhisperFile *w, const char *fmt, i32 layer);

/* Bytes one tensor of `dtype` with `nelem` elements occupies on disk, or 0
 * for an unsupported dtype or a nelem that is not a whole number of blocks. */
u64 whisper_type_nbytes(i32 dtype, u64 nelem);

/* Elements per quantization block (1 for F32/F16), or 0 if unsupported. */
i32 whisper_type_block(i32 dtype);

/* Bytes per quantization block, or 0 if unsupported. */
i32 whisper_type_block_bytes(i32 dtype);

#endif /* VOICE_WHISPER_GGML_H */
