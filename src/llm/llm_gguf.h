/*
 * Liu - GGUF v3 container parser (local LLM engine, B izi).
 *
 * mmap-backed, read-only. Every offset coming off disk is bounds-checked
 * against the mapping before use — the GGUF file is untrusted input.
 *
 * Spec reference: github.com/ggml-org/llama.cpp (gguf-py / ggml).
 * Only the subset needed by the Hy-MT translation model is parsed:
 * metadata KV pairs (scalars, strings, arrays) and the tensor table.
 */
#ifndef LLM_LLM_GGUF_H
#define LLM_LLM_GGUF_H

#include "core/types.h"

/* ggml tensor data type ids. Confirmed against the real Hy-MT GGUF and
 * llama.cpp PR #22836: the Hy-MT 1.25bit file uses Q6_K for the (tied)
 * token-embedding / LM-head table and STQ1_0 (id 42) for every layer
 * projection weight. Norms are F32. */
enum {
    GGML_TYPE_F32    = 0,
    GGML_TYPE_F16    = 1,
    GGML_TYPE_Q8_0   = 8,
    GGML_TYPE_Q6_K   = 14,
    GGML_TYPE_STQ1_0 = 42,   /* llama.cpp PR #22836 — confirmed */
};

/* GGUF metadata value types. */
enum {
    GGUF_KV_U8 = 0, GGUF_KV_I8 = 1, GGUF_KV_U16 = 2, GGUF_KV_I16 = 3,
    GGUF_KV_U32 = 4, GGUF_KV_I32 = 5, GGUF_KV_F32 = 6, GGUF_KV_BOOL = 7,
    GGUF_KV_STR = 8, GGUF_KV_ARR = 9, GGUF_KV_U64 = 10, GGUF_KV_I64 = 11,
    GGUF_KV_F64 = 12,
};

typedef struct {
    char      name[96];
    u32       n_dims;
    u64       dims[4];
    u32       dtype;        /* raw ggml type id */
    u64       offset;       /* byte offset into the tensor-data blob */
    const u8 *data;         /* resolved mmap pointer (NULL until parsed) */
    u64       nbytes;       /* tensor data span in bytes */
} GgufTensor;

typedef struct {
    char        key[128];
    u32         type;       /* GGUF_KV_* */
    union { u64 u; i64 i; f64 f; i32 b; } scalar;
    const char *str;        /* GGUF_KV_STR: pointer into mmap (not NUL-term) */
    u64         str_len;
    u32         arr_type;   /* GGUF_KV_ARR element type */
    u64         arr_count;
    const u8   *arr_data;   /* GGUF_KV_ARR: first element in mmap */
} GgufKv;

typedef struct {
    int         fd;
    const u8   *map;
    usize       map_size;
    u32         version;
    u64         tensor_count;
    u64         kv_count;
    GgufKv     *kvs;          /* heap [kv_count] */
    GgufTensor *tensors;      /* heap [tensor_count] */
    const u8   *tensor_data;  /* aligned start of the tensor-data blob */
    u32         alignment;
} GgufFile;

/* Open + mmap + parse. Returns false (and leaves *g zeroed) on any
 * malformed-file or I/O error. */
bool gguf_open(GgufFile *g, const char *path);
void gguf_close(GgufFile *g);

const GgufKv *gguf_find(const GgufFile *g, const char *key);
bool gguf_kv_u32(const GgufFile *g, const char *key, u32 *out);
bool gguf_kv_f32(const GgufFile *g, const char *key, f32 *out);
bool gguf_kv_str(const GgufFile *g, const char *key, const char **out, u64 *out_len);

const GgufTensor *gguf_tensor(const GgufFile *g, const char *name);

/* Linear iterator over a GGUF_KV_ARR of GGUF_KV_STR (e.g.
 * tokenizer.ggml.tokens / .merges). gguf_arr_str_begin() seeds it from a
 * KV; each gguf_arr_str_next() yields the next element (pointer into the
 * mmap, not NUL-terminated) and advances. Returns false when exhausted or
 * on a malformed entry. One full pass is O(total bytes). */
typedef struct { const u8 *p; const u8 *end; u64 left; } GgufStrIter;
GgufStrIter gguf_arr_str_begin(const GgufFile *g, const GgufKv *kv);
bool gguf_arr_str_next(GgufStrIter *it, const char **out, u64 *out_len);

/* IEEE half → float. Branch-free; handles subnormals, inf, NaN. */
static inline f32 gguf_f16_to_f32(u16 h) {
    u32 sign = (u32)(h & 0x8000u) << 16;
    u32 exp  = (h >> 10) & 0x1Fu;
    u32 mant = h & 0x3FFu;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;                       /* +/- zero */
        } else {
            /* subnormal — normalize */
            i32 e = -1;
            do { e++; mant <<= 1; } while ((mant & 0x400u) == 0);
            mant &= 0x3FFu;
            bits = sign | (u32)((127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (mant << 13); /* inf / NaN */
    } else {
        bits = sign | (u32)((i32)exp - 15 + 127) << 23 | (mant << 13);
    }
    f32 out;
    __builtin_memcpy(&out, &bits, sizeof out);
    return out;
}

#endif /* LLM_LLM_GGUF_H */
