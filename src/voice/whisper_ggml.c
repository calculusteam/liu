/*
 * Liu - whisper.cpp "ggml" container parser.
 *
 * Same threat model as llm_gguf.c: the file is untrusted, so a bounds-checked
 * forward cursor gates every read against the end of the mapping and a
 * truncated or hostile header can at worst make whisper_open() return false.
 *
 * Unlike GGUF there is no alignment guarantee and no tensor index — see the
 * header. The one-pass walk below both validates and records the table.
 */
#include "voice/whisper_ggml.h"
#include "core/checked.h"
#include "core/mem_budget.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define WGGML_MAGIC 0x67676d6cu   /* "ggml" little-endian */

/* whisper.cpp folds a quantization-format version into ftype as
 * ftype = qntvr*GGML_QNT_VERSION_FACTOR + real_ftype. */
#define WGGML_QNT_VERSION_FACTOR 1000

/* Sanity ceilings. These are not the model's real limits, just bounds loose
 * enough for every official Whisper file and tight enough that a corrupt
 * header cannot force a wild allocation. */
#define WGGML_MAX_TENSORS   4096
#define WGGML_MAX_VOCAB     (1 << 20)
#define WGGML_MAX_TOKEN_LEN 1024
#define WGGML_MAX_MEL       512
#define WGGML_MAX_FFT       4096

typedef struct {
    const u8 *p;
    const u8 *end;
    bool      bad;      /* sticky: set on any over-read attempt */
} Cur;

static const u8 *cur_take(Cur *c, usize n) {
    if (c->bad || (usize)(c->end - c->p) < n) { c->bad = true; return NULL; }
    const u8 *r = c->p;
    c->p += n;
    return r;
}

static i32 cur_i32(Cur *c) {
    const u8 *p = cur_take(c, 4);
    i32 v = 0;
    if (p) memcpy(&v, p, 4);   /* memcpy: the stream is not 4-byte aligned */
    return v;
}

static void fail(char *err, usize cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

i32 whisper_type_block(i32 dtype) {
    switch (dtype) {
        case WGGML_TYPE_F32:
        case WGGML_TYPE_F16:  return 1;
        case WGGML_TYPE_Q4_0:
        case WGGML_TYPE_Q4_1:
        case WGGML_TYPE_Q5_0:
        case WGGML_TYPE_Q5_1:
        case WGGML_TYPE_Q8_0: return 32;
        default:              return 0;
    }
}

i32 whisper_type_block_bytes(i32 dtype) {
    switch (dtype) {
        case WGGML_TYPE_F32:  return 4;
        case WGGML_TYPE_F16:  return 2;
        case WGGML_TYPE_Q4_0: return 18;   /* f16 d + 16 nibble bytes      */
        case WGGML_TYPE_Q4_1: return 20;   /* f16 d + f16 m + 16           */
        case WGGML_TYPE_Q5_0: return 22;   /* f16 d + 4 high-bit + 16      */
        case WGGML_TYPE_Q5_1: return 24;   /* f16 d + f16 m + 4 + 16       */
        case WGGML_TYPE_Q8_0: return 34;   /* f16 d + 32 int8              */
        default:              return 0;
    }
}

u64 whisper_type_nbytes(i32 dtype, u64 nelem) {
    i32 blk = whisper_type_block(dtype);
    i32 bb  = whisper_type_block_bytes(dtype);
    if (blk <= 0 || bb <= 0) return 0;
    if (nelem % (u64)blk != 0) return 0;
    u64 nblocks = nelem / (u64)blk;
    if (nblocks > UINT64_MAX / (u64)bb) return 0;
    return nblocks * (u64)bb;
}

const WhisperTensor *whisper_tensor(const WhisperFile *w, const char *name) {
    if (!w || !name) return NULL;
    for (i32 i = 0; i < w->tensor_count; i++) {
        if (strcmp(w->tensors[i].name, name) == 0) return &w->tensors[i];
    }
    return NULL;
}

const WhisperTensor *whisper_tensor_fmt(const WhisperFile *w, const char *fmt, i32 layer) {
    if (!w || !fmt) return NULL;
    char name[WHISPER_TENSOR_NAME_CAP];
    int n = snprintf(name, sizeof name, fmt, layer);
    if (n < 0 || (usize)n >= sizeof name) return NULL;
    return whisper_tensor(w, name);
}

/* Charge the resident pages of the read-only Whisper model mapping to
 * MEM_VOICE rather than charging its full virtual size. mincore() reports
 * which pages are resident; load-time and close-time readings reflect the
 * pages touched at each measurement. A failed query skips the charge rather
 * than estimating it. */
static usize whisper_map_resident(const WhisperFile *w) {
    if (!w || !w->map || w->map == MAP_FAILED || w->map_size == 0) return 0;
    usize page = (usize)getpagesize();
    if (page == 0) return 0;
    usize pages = (w->map_size + page - 1) / page;
    char *vec = (char *)malloc(pages);
    if (!vec) return 0;
    usize resident = 0;
    if (mincore((void *)w->map, w->map_size, vec) == 0) {
        for (usize i = 0; i < pages; i++)
            if (vec[i] & 1) resident += page;
        if (resident > w->map_size) resident = w->map_size;
    }
    free(vec);
    return resident;
}

/* Bring MEM_VOICE in line with what is resident now. Transient because the
 * voice worker owns this file on its own thread; mem_budget_transient_adjust
 * is the only thread-safe half of the ledger. */
void whisper_refresh_residency(WhisperFile *w) {
    if (!w) return;
    if (!w) return;
    usize now = whisper_map_resident(w);
    if (now == w->accounted) return;
    mem_budget_transient_adjust(MEM_VOICE, (isize)now - (isize)w->accounted);
    w->accounted = now;
}

void whisper_close(WhisperFile *w) {
    if (!w) return;
    free(w->filters);
    free(w->vocab);
    free(w->tensors);
    /* Refresh first, then give it all back: measuring here is what makes the
     * ledger's peak reflect what the DECODER paged in, not just the load-time
     * working set. Then discharge exactly what is accounted — after munmap
     * there is nothing left to measure and `accounted` is the only record. */
    whisper_refresh_residency(w);
    if (w->accounted) {
        mem_budget_transient_adjust(MEM_VOICE, -(isize)w->accounted);
        w->accounted = 0;
    }
    if (w->map && w->map != MAP_FAILED) munmap((void *)w->map, w->map_size);
    if (w->fd >= 0) close(w->fd);
    memset(w, 0, sizeof *w);
    w->fd = -1;
}

bool whisper_open(WhisperFile *w, const char *path, char *err, usize err_cap) {
    if (!w) return false;
    memset(w, 0, sizeof *w);
    w->fd = -1;
    if (err && err_cap) err[0] = '\0';

    if (!path || !path[0]) { fail(err, err_cap, "no model path"); return false; }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { fail(err, err_cap, "model file not found"); return false; }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        fail(err, err_cap, "cannot stat model file");
        return false;
    }

    usize size = (usize)st.st_size;
    const u8 *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        fail(err, err_cap, "cannot map model file");
        return false;
    }

    w->fd       = fd;
    w->map      = map;
    w->map_size = size;

    Cur c = { map, map + size, false };

    if ((u32)cur_i32(&c) != WGGML_MAGIC) {
        fail(err, err_cap, "not a whisper ggml model (bad magic)");
        goto bad;
    }

    /* ---- hparams -------------------------------------------------------- */
    w->h.n_vocab       = cur_i32(&c);
    w->h.n_audio_ctx   = cur_i32(&c);
    w->h.n_audio_state = cur_i32(&c);
    w->h.n_audio_head  = cur_i32(&c);
    w->h.n_audio_layer = cur_i32(&c);
    w->h.n_text_ctx    = cur_i32(&c);
    w->h.n_text_state  = cur_i32(&c);
    w->h.n_text_head   = cur_i32(&c);
    w->h.n_text_layer  = cur_i32(&c);
    w->h.n_mels        = cur_i32(&c);
    w->h.ftype         = cur_i32(&c) % WGGML_QNT_VERSION_FACTOR;
    if (c.bad) { fail(err, err_cap, "truncated header"); goto bad; }

    if (w->h.n_vocab       <= 0 || w->h.n_vocab       > WGGML_MAX_VOCAB ||
        w->h.n_audio_ctx   <= 0 || w->h.n_audio_ctx   > 65536 ||
        w->h.n_audio_state <= 0 || w->h.n_audio_state > 65536 ||
        w->h.n_audio_head  <= 0 || w->h.n_audio_head  > 1024  ||
        w->h.n_audio_layer <= 0 || w->h.n_audio_layer > 1024  ||
        /* The prompt alone is up to three tokens (sot, language, task) and
         * the engine sizes its token buffer to EXACTLY n_text_ctx, so a
         * context shorter than that would be a heap write off the end.
         * Real models have 448. */
        w->h.n_text_ctx    < 8  || w->h.n_text_ctx    > 65536 ||
        w->h.n_text_state  <= 0 || w->h.n_text_state  > 65536 ||
        w->h.n_text_head   <= 0 || w->h.n_text_head   > 1024  ||
        w->h.n_text_layer  <= 0 || w->h.n_text_layer  > 1024  ||
        w->h.n_mels        <= 0 || w->h.n_mels        > WGGML_MAX_MEL) {
        fail(err, err_cap, "implausible model hyperparameters");
        goto bad;
    }
    /* The attention reshape divides state by head; a non-multiple would make
     * every downstream head stride wrong rather than fail loudly. */
    if (w->h.n_audio_state % w->h.n_audio_head != 0 ||
        w->h.n_text_state  % w->h.n_text_head  != 0) {
        fail(err, err_cap, "state size is not a multiple of head count");
        goto bad;
    }

    /* ---- mel filterbank ------------------------------------------------- */
    w->n_mel = cur_i32(&c);
    w->n_fft = cur_i32(&c);
    if (c.bad) { fail(err, err_cap, "truncated mel filters"); goto bad; }
    if (w->n_mel <= 0 || w->n_mel > WGGML_MAX_MEL ||
        w->n_fft <= 0 || w->n_fft > WGGML_MAX_FFT) {
        fail(err, err_cap, "implausible mel filter shape");
        goto bad;
    }
    {
        usize nf = 0;
        if (!checked_mul_usize((usize)w->n_mel, (usize)w->n_fft, &nf) || nf == 0) {
            fail(err, err_cap, "implausible mel filter shape");
            goto bad;
        }
        usize nbytes = 0;
        if (!checked_mul_usize(nf, sizeof(f32), &nbytes)) {
            fail(err, err_cap, "implausible mel filter shape");
            goto bad;
        }
        const u8 *src = cur_take(&c, nbytes);
        if (!src) { fail(err, err_cap, "truncated mel filters"); goto bad; }
        w->filters = malloc(nbytes);
        if (!w->filters) { fail(err, err_cap, "out of memory"); goto bad; }
        /* Copied rather than aliased: the payload is not 4-byte aligned. */
        memcpy(w->filters, src, nbytes);
    }

    /* ---- vocabulary ----------------------------------------------------- */
    w->n_vocab_file = cur_i32(&c);
    if (c.bad || w->n_vocab_file <= 0 || w->n_vocab_file > WGGML_MAX_VOCAB) {
        fail(err, err_cap, "implausible vocabulary size");
        goto bad;
    }
    w->vocab = calloc((usize)w->n_vocab_file, sizeof *w->vocab);
    if (!w->vocab) { fail(err, err_cap, "out of memory"); goto bad; }
    for (i32 i = 0; i < w->n_vocab_file; i++) {
        i32 len = cur_i32(&c);
        if (c.bad || len < 0 || len > WGGML_MAX_TOKEN_LEN) {
            fail(err, err_cap, "malformed vocabulary entry");
            goto bad;
        }
        /* len == 0 is legitimate: multilingual models carry an empty-string
         * token at the <|endoftext|> slot. */
        const u8 *p = len ? cur_take(&c, (usize)len) : c.p;
        if (len && !p) { fail(err, err_cap, "truncated vocabulary"); goto bad; }
        w->vocab[i].p   = (const char *)p;
        w->vocab[i].len = len;
    }

    /* ---- tensors -------------------------------------------------------- */
    {
        i32   cap = 512;
        WhisperTensor *arr = calloc((usize)cap, sizeof *arr);
        if (!arr) { fail(err, err_cap, "out of memory"); goto bad; }
        w->tensors = arr;

        /* The stream ends at EOF, not at a count. A header that starts within
         * 12 bytes of the end is the normal terminator. */
        while ((usize)(c.end - c.p) >= 12) {
            i32 n_dims = cur_i32(&c);
            i32 nlen   = cur_i32(&c);
            i32 dtype  = cur_i32(&c);
            if (c.bad) break;

            if (n_dims < 1 || n_dims > 4 ||
                nlen < 1 || nlen >= WHISPER_TENSOR_NAME_CAP) {
                fail(err, err_cap, "malformed tensor header");
                goto bad;
            }
            i32 blk = whisper_type_block(dtype);
            if (blk == 0) {
                fail(err, err_cap, "unsupported tensor dtype in model");
                goto bad;
            }

            WhisperTensor t;
            memset(&t, 0, sizeof t);
            t.n_dims = n_dims;
            t.dtype  = dtype;
            t.ne[0] = t.ne[1] = t.ne[2] = t.ne[3] = 1;

            u64 nelem = 1;
            for (i32 d = 0; d < n_dims; d++) {
                i32 v = cur_i32(&c);
                if (c.bad || v <= 0) {
                    fail(err, err_cap, "malformed tensor shape");
                    goto bad;
                }
                t.ne[d] = v;
                if (nelem > UINT64_MAX / (u64)v) {
                    fail(err, err_cap, "implausible tensor shape");
                    goto bad;
                }
                nelem *= (u64)v;
            }

            const u8 *np = cur_take(&c, (usize)nlen);
            if (!np) { fail(err, err_cap, "truncated tensor name"); goto bad; }
            memcpy(t.name, np, (usize)nlen);
            t.name[nlen] = '\0';

            t.nbytes = whisper_type_nbytes(dtype, nelem);
            if (t.nbytes == 0) {
                fail(err, err_cap, "tensor size is not a whole number of blocks");
                goto bad;
            }
            if (t.nbytes > (u64)(c.end - c.p)) {
                fail(err, err_cap, "truncated tensor data");
                goto bad;
            }
            t.data = cur_take(&c, (usize)t.nbytes);
            if (!t.data) { fail(err, err_cap, "truncated tensor data"); goto bad; }

            if (w->tensor_count == cap) {
                if (cap >= WGGML_MAX_TENSORS) {
                    fail(err, err_cap, "too many tensors in model");
                    goto bad;
                }
                i32 ncap = cap * 2;
                WhisperTensor *grown = realloc(w->tensors, (usize)ncap * sizeof *grown);
                if (!grown) { fail(err, err_cap, "out of memory"); goto bad; }
                w->tensors = grown;
                cap = ncap;
            }
            w->tensors[w->tensor_count++] = t;
        }
    }

    if (w->tensor_count == 0) {
        fail(err, err_cap, "model contains no tensors");
        goto bad;
    }
    /* The parse has walked the tensor table, so what is resident now is the
     * model's load-time working set. The decoder pages in more as it runs;
     * this is measured again on close, and the discharge is exact either way. */
    whisper_refresh_residency(w);
    return true;

bad:
    whisper_close(w);
    return false;
}
