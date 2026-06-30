/*
 * Liu - GGUF v3 container parser.
 *
 * The file is untrusted: a bounds-checked cursor (`Cur`) gates every read
 * against the end of the mapping, so a truncated or hostile header can at
 * worst make gguf_open() return false — never an out-of-bounds read.
 */
#include "llm/llm_gguf.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#define GGUF_MAGIC 0x46554747u  /* "GGUF" little-endian */

/* Bounds-checked forward cursor over the mmap. */
typedef struct {
    const u8 *p;
    const u8 *end;
    bool      bad;       /* sticky: set on any over-read attempt */
} Cur;

static const u8 *cur_take(Cur *c, usize n) {
    if (c->bad || (usize)(c->end - c->p) < n) { c->bad = true; return NULL; }
    const u8 *r = c->p;
    c->p += n;
    return r;
}

static u8  cur_u8(Cur *c)  { const u8 *p = cur_take(c, 1); return p ? p[0] : 0; }
static u16 cur_u16(Cur *c) { const u8 *p = cur_take(c, 2); u16 v = 0; if (p) memcpy(&v, p, 2); return v; }
static u32 cur_u32(Cur *c) { const u8 *p = cur_take(c, 4); u32 v = 0; if (p) memcpy(&v, p, 4); return v; }
static u64 cur_u64(Cur *c) { const u8 *p = cur_take(c, 8); u64 v = 0; if (p) memcpy(&v, p, 8); return v; }
static f32 cur_f32(Cur *c) { const u8 *p = cur_take(c, 4); f32 v = 0; if (p) memcpy(&v, p, 4); return v; }
static f64 cur_f64(Cur *c) { const u8 *p = cur_take(c, 8); f64 v = 0; if (p) memcpy(&v, p, 8); return v; }

/* GGUF string: u64 length + raw bytes (no NUL). Returns the in-map pointer
 * and length; advances the cursor past the bytes. */
static const char *cur_str(Cur *c, u64 *out_len) {
    u64 len = cur_u64(c);
    /* Guard against absurd lengths before asking the cursor for them. */
    if (c->bad || len > (u64)(c->end - c->p)) { c->bad = true; *out_len = 0; return NULL; }
    const u8 *p = cur_take(c, (usize)len);
    *out_len = p ? len : 0;
    return (const char *)p;
}

/* Byte width of a fixed-size scalar KV/array element type. 0 for variable
 * (string) or invalid types. */
static usize kv_scalar_width(u32 type) {
    switch (type) {
        case GGUF_KV_U8: case GGUF_KV_I8: case GGUF_KV_BOOL: return 1;
        case GGUF_KV_U16: case GGUF_KV_I16: return 2;
        case GGUF_KV_U32: case GGUF_KV_I32: case GGUF_KV_F32: return 4;
        case GGUF_KV_U64: case GGUF_KV_I64: case GGUF_KV_F64: return 8;
        default: return 0;
    }
}

/* Read a scalar KV value into `kv->scalar`. */
static void parse_scalar(Cur *c, GgufKv *kv) {
    switch (kv->type) {
        case GGUF_KV_U8:   kv->scalar.u = cur_u8(c); break;
        case GGUF_KV_I8:   kv->scalar.i = (i8)cur_u8(c); break;
        case GGUF_KV_U16:  kv->scalar.u = cur_u16(c); break;
        case GGUF_KV_I16:  kv->scalar.i = (i16)cur_u16(c); break;
        case GGUF_KV_U32:  kv->scalar.u = cur_u32(c); break;
        case GGUF_KV_I32:  kv->scalar.i = (i32)cur_u32(c); break;
        case GGUF_KV_F32:  kv->scalar.f = cur_f32(c); break;
        case GGUF_KV_BOOL: kv->scalar.b = cur_u8(c) ? 1 : 0; break;
        case GGUF_KV_U64:  kv->scalar.u = cur_u64(c); break;
        case GGUF_KV_I64:  kv->scalar.i = (i64)cur_u64(c); break;
        case GGUF_KV_F64:  kv->scalar.f = cur_f64(c); break;
        default: c->bad = true; break;
    }
}

static void parse_kv(Cur *c, GgufKv *kv) {
    memset(kv, 0, sizeof *kv);

    u64 klen = 0;
    const char *kp = cur_str(c, &klen);
    if (c->bad || !kp) { c->bad = true; return; }
    if (klen >= sizeof kv->key) klen = sizeof kv->key - 1;
    memcpy(kv->key, kp, (usize)klen);
    kv->key[klen] = '\0';

    kv->type = cur_u32(c);
    if (kv->type == GGUF_KV_STR) {
        kv->str = cur_str(c, &kv->str_len);
    } else if (kv->type == GGUF_KV_ARR) {
        kv->arr_type  = cur_u32(c);
        kv->arr_count = cur_u64(c);
        if (c->bad) return;
        if (kv->arr_type == GGUF_KV_STR) {
            /* Array of strings: remember where it starts, then skip each
             * entry so the cursor lands on the next KV. Element access is
             * done lazily by the tokenizer via gguf_arr_str(). */
            kv->arr_data = c->p;
            for (u64 i = 0; i < kv->arr_count && !c->bad; i++) {
                u64 sl = 0;
                cur_str(c, &sl);
            }
        } else {
            usize w = kv_scalar_width(kv->arr_type);
            if (w == 0) { c->bad = true; return; }
            kv->arr_data = c->p;
            /* Overflow-safe: count * w must fit in the remaining span. */
            if (kv->arr_count > (u64)(c->end - c->p) / w) { c->bad = true; return; }
            (void)cur_take(c, (usize)(kv->arr_count * w));
        }
    } else {
        parse_scalar(c, kv);
    }
}

static void parse_tensor(Cur *c, GgufTensor *t) {
    memset(t, 0, sizeof *t);

    u64 nlen = 0;
    const char *np = cur_str(c, &nlen);
    if (c->bad || !np) { c->bad = true; return; }
    if (nlen >= sizeof t->name) nlen = sizeof t->name - 1;
    memcpy(t->name, np, (usize)nlen);
    t->name[nlen] = '\0';

    t->n_dims = cur_u32(c);
    if (t->n_dims > 4) { c->bad = true; return; }
    for (u32 i = 0; i < 4; i++) t->dims[i] = (i < t->n_dims) ? cur_u64(c) : 1;
    t->dtype  = cur_u32(c);
    t->offset = cur_u64(c);
}

bool gguf_open(GgufFile *g, const char *path) {
    if (!g || !path) return false;
    memset(g, 0, sizeof *g);
    g->fd = -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 24 || !S_ISREG(st.st_mode)) {
        close(fd);
        return false;
    }
    usize size = (usize)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return false; }

    g->fd = fd;
    g->map = (const u8 *)map;
    g->map_size = size;

    Cur c = { g->map, g->map + size, false };
    u32 magic = cur_u32(&c);
    g->version = cur_u32(&c);
    g->tensor_count = cur_u64(&c);
    g->kv_count = cur_u64(&c);
    if (c.bad || magic != GGUF_MAGIC || g->version != 3) goto fail;
    /* Sanity ceilings — a real Hy-MT model has hundreds of tensors and
     * dozens of KVs, not billions. Rejects corrupt counts before alloc. */
    if (g->tensor_count > 1u << 20 || g->kv_count > 1u << 16) goto fail;

    g->kvs = (GgufKv *)calloc(g->kv_count ? g->kv_count : 1, sizeof(GgufKv));
    g->tensors = (GgufTensor *)calloc(g->tensor_count ? g->tensor_count : 1,
                                      sizeof(GgufTensor));
    if (!g->kvs || !g->tensors) goto fail;

    for (u64 i = 0; i < g->kv_count; i++) {
        parse_kv(&c, &g->kvs[i]);
        if (c.bad) goto fail;
    }
    for (u64 i = 0; i < g->tensor_count; i++) {
        parse_tensor(&c, &g->tensors[i]);
        if (c.bad) goto fail;
    }

    /* general.alignment defaults to 32 when absent. */
    g->alignment = 32;
    {
        u32 a = 0;
        if (gguf_kv_u32(g, "general.alignment", &a) && a > 0) g->alignment = a;
    }

    /* Tensor data starts at the next `alignment` boundary after the
     * tensor table. */
    {
        usize header_used = (usize)(c.p - g->map);
        usize aligned = (header_used + g->alignment - 1) & ~((usize)g->alignment - 1);
        if (aligned > size) goto fail;
        g->tensor_data = g->map + aligned;
    }

    /* Resolve each tensor's data pointer + byte span, bounds-checked. */
    for (u64 i = 0; i < g->tensor_count; i++) {
        GgufTensor *t = &g->tensors[i];
        u64 elems = 1;
        for (u32 d = 0; d < 4; d++) {
            if (t->dims[d] == 0) { elems = 0; break; }
            /* overflow-safe multiply */
            if (t->dims[d] != 0 && elems > (u64)-1 / t->dims[d]) goto fail;
            elems *= t->dims[d];
        }
        u64 nbytes = 0;
        /* Block-quantized types must have a block-aligned element count;
         * a `(elems / blk) * bytes` formula would otherwise silently
         * round nbytes DOWN and defeat the bounds check below. Reject
         * the file rather than under-count. Every byte-size multiply is
         * overflow-checked first: a wrapped nbytes (e.g. 2^62 * 4 == 0)
         * would otherwise sail through the `nbytes > avail - offset`
         * bounds test and hand out an out-of-bounds data pointer. */
        switch (t->dtype) {
            case GGML_TYPE_F32:
                if (elems > (u64)-1 / 4) goto fail;
                nbytes = elems * 4; break;
            case GGML_TYPE_F16:
                if (elems > (u64)-1 / 2) goto fail;
                nbytes = elems * 2; break;
            case GGML_TYPE_Q8_0:
                if (elems % 32 != 0) goto fail;
                { u64 blk = elems / 32;
                  if (blk > (u64)-1 / 34) goto fail;
                  nbytes = blk * 34; }
                break;
            case GGML_TYPE_Q6_K:
                /* block_q6_K: ql[128] + qh[64] + scales[16] + d(f16) = 210.
                 * Each ROW (dims[0] contiguous weights) is independently
                 * block-quantized, so the row length itself — not just the
                 * total element count — must be block-aligned. A non-aligned
                 * dims[0] makes the per-row dequant (nblocks = in_dim/256)
                 * silently drop the tail lanes and read stale scratch. */
                if (t->dims[0] % 256 != 0 || elems % 256 != 0) goto fail;
                { u64 blk = elems / 256;
                  if (blk > (u64)-1 / 210) goto fail;
                  nbytes = blk * 210; }
                break;
            case GGML_TYPE_STQ1_0:
                /* block_stq1_0: qs[32] + sign[8] + d(f16) = 42 (PR #22836) */
                if (t->dims[0] % 256 != 0 || elems % 256 != 0) goto fail;
                { u64 blk = elems / 256;
                  if (blk > (u64)-1 / 42) goto fail;
                  nbytes = blk * 42; }
                break;
            default:
                /* Unknown dtype — leave nbytes 0; the model loader will
                 * reject the tensor with a clear message. */
                break;
        }
        t->nbytes = nbytes;
        usize avail = (usize)(g->map + size - g->tensor_data);
        if (t->offset > avail || nbytes > avail - t->offset) {
            /* A tensor pointing outside the blob is fatal. */
            goto fail;
        }
        t->data = g->tensor_data + t->offset;
    }

    return true;

fail:
    gguf_close(g);
    return false;
}

void gguf_close(GgufFile *g) {
    if (!g) return;
    free(g->kvs);
    free(g->tensors);
    if (g->map && g->map_size) munmap((void *)g->map, g->map_size);
    if (g->fd >= 0) close(g->fd);
    memset(g, 0, sizeof *g);
    g->fd = -1;
}

const GgufKv *gguf_find(const GgufFile *g, const char *key) {
    if (!g || !key) return NULL;
    for (u64 i = 0; i < g->kv_count; i++) {
        if (strcmp(g->kvs[i].key, key) == 0) return &g->kvs[i];
    }
    return NULL;
}

bool gguf_kv_u32(const GgufFile *g, const char *key, u32 *out) {
    const GgufKv *kv = gguf_find(g, key);
    if (!kv) return false;
    switch (kv->type) {
        case GGUF_KV_U8: case GGUF_KV_U16: case GGUF_KV_U32: case GGUF_KV_U64:
            *out = (u32)kv->scalar.u; return true;
        case GGUF_KV_I8: case GGUF_KV_I16: case GGUF_KV_I32: case GGUF_KV_I64:
            if (kv->scalar.i < 0) return false;
            *out = (u32)kv->scalar.i; return true;
        default: return false;
    }
}

bool gguf_kv_f32(const GgufFile *g, const char *key, f32 *out) {
    const GgufKv *kv = gguf_find(g, key);
    if (!kv) return false;
    if (kv->type == GGUF_KV_F32 || kv->type == GGUF_KV_F64) {
        *out = (f32)kv->scalar.f;
        return true;
    }
    /* Allow an integer KV to satisfy a float request (some exporters store
     * rope.freq_base as an int). */
    if (kv->type == GGUF_KV_U32 || kv->type == GGUF_KV_U64) {
        *out = (f32)kv->scalar.u; return true;
    }
    if (kv->type == GGUF_KV_I32 || kv->type == GGUF_KV_I64) {
        *out = (f32)kv->scalar.i; return true;
    }
    return false;
}

bool gguf_kv_str(const GgufFile *g, const char *key, const char **out, u64 *out_len) {
    const GgufKv *kv = gguf_find(g, key);
    if (!kv || kv->type != GGUF_KV_STR) return false;
    *out = kv->str;
    *out_len = kv->str_len;
    return true;
}

const GgufTensor *gguf_tensor(const GgufFile *g, const char *name) {
    if (!g || !name) return NULL;
    for (u64 i = 0; i < g->tensor_count; i++) {
        if (strcmp(g->tensors[i].name, name) == 0) return &g->tensors[i];
    }
    return NULL;
}

GgufStrIter gguf_arr_str_begin(const GgufFile *g, const GgufKv *kv) {
    GgufStrIter it = {0};
    if (!g || !kv || kv->type != GGUF_KV_ARR || kv->arr_type != GGUF_KV_STR ||
        !kv->arr_data) {
        return it;
    }
    it.p = kv->arr_data;
    it.end = g->map + g->map_size;
    it.left = kv->arr_count;
    return it;
}

bool gguf_arr_str_next(GgufStrIter *it, const char **out, u64 *out_len) {
    if (!it || it->left == 0 || !it->p) return false;
    if ((usize)(it->end - it->p) < 8) { it->left = 0; return false; }
    u64 len;
    memcpy(&len, it->p, 8);
    it->p += 8;
    if (len > (u64)(it->end - it->p)) { it->left = 0; return false; }
    *out = (const char *)it->p;
    *out_len = len;
    it->p += len;
    it->left--;
    return true;
}
