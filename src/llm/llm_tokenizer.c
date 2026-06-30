/*
 * Liu - GPT-2 byte-level BPE tokenizer.
 *
 * Pipeline (encode): UTF-8 text -> pre-token chunks -> byte-level
 * remap -> greedy rank-ordered BPE merges -> vocab id lookup.
 *
 * Token and merge strings are referenced in place from the GGUF mmap;
 * only the index/hash tables are heap-owned.
 */
#include "llm/llm_tokenizer.h"

#include "core/utf8.h"

#include <stdlib.h>
#include <string.h>

#define TOK_MERGE_SCRATCH 512   /* max "A B" merge key length we consider */
#define TOK_MAX_SYMBOLS   1024  /* max symbols in a single pre-token chunk */

struct LlmTokenizer {
    /* id -> token string (pointers into the GGUF mmap) */
    const char **tok_ptr;
    i32         *tok_len;
    i8          *tok_type;     /* GGUF token_type, 1=NORMAL when unknown */
    i32          vocab_size;

    /* vocab hash: token string -> id. Open addressing, -1 = empty. */
    i32         *vmap;
    u32          vmap_mask;

    /* merge hash: "A B" key -> rank. Keys point into the mmap. */
    const char **mkey_ptr;
    i32         *mkey_len;
    i32         *mrank;        /* -1 = empty slot */
    u32          mh_mask;

    i32          bos, eos;

    u32          byte_to_cp[256];
    i16          cp_to_byte[512];   /* -1 where no byte maps to that cp */
};

/* ---- FNV-1a over a byte span ---------------------------------------- */
static u32 fnv1a(const char *p, i32 n) {
    u32 h = 2166136261u;
    for (i32 i = 0; i < n; i++) {
        h ^= (u8)p[i];
        h *= 16777619u;
    }
    return h;
}

static u32 next_pow2(u32 v) {
    /* Saturate at 2^31: without this, an argument > 2^31 spins forever
     * (p reaches 0x80000000, `p <<= 1` wraps to 0, and `0 < v` stays
     * true). Callers cap their inputs, but this is a cheap backstop. */
    if (v > 0x80000000u) return 0x80000000u;
    u32 p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* ---- GPT-2 bytes_to_unicode ----------------------------------------- */
static void build_byte_maps(LlmTokenizer *t) {
    for (i32 i = 0; i < 512; i++) t->cp_to_byte[i] = -1;

    bool self[256] = {0};
    for (i32 b = 33;  b <= 126; b++) self[b] = true;   /* '!'..'~'        */
    for (i32 b = 161; b <= 172; b++) self[b] = true;   /* U+00A1..U+00AC  */
    for (i32 b = 174; b <= 255; b++) self[b] = true;   /* U+00AE..U+00FF  */

    u32 n = 0;
    for (i32 b = 0; b < 256; b++) {
        u32 cp = self[b] ? (u32)b : (256u + n++);
        t->byte_to_cp[b] = cp;
        if (cp < 512) t->cp_to_byte[cp] = (i16)b;
    }
}

/* ---- hash table builders -------------------------------------------- */
static bool vmap_build(LlmTokenizer *t) {
    u32 cap = next_pow2((u32)t->vocab_size * 2u + 1u);
    t->vmap = (i32 *)malloc(cap * sizeof(i32));
    if (!t->vmap) return false;
    t->vmap_mask = cap - 1;
    for (u32 i = 0; i < cap; i++) t->vmap[i] = -1;

    for (i32 id = 0; id < t->vocab_size; id++) {
        u32 slot = fnv1a(t->tok_ptr[id], t->tok_len[id]) & t->vmap_mask;
        while (t->vmap[slot] != -1) slot = (slot + 1) & t->vmap_mask;
        t->vmap[slot] = id;
    }
    return true;
}

static i32 vmap_lookup(const LlmTokenizer *t, const char *s, i32 n) {
    u32 slot = fnv1a(s, n) & t->vmap_mask;
    for (;;) {
        i32 id = t->vmap[slot];
        if (id == -1) return -1;
        if (t->tok_len[id] == n && memcmp(t->tok_ptr[id], s, (usize)n) == 0)
            return id;
        slot = (slot + 1) & t->vmap_mask;
    }
}

static i32 mh_lookup(const LlmTokenizer *t, const char *s, i32 n) {
    u32 slot = fnv1a(s, n) & t->mh_mask;
    for (;;) {
        i32 rank = t->mrank[slot];
        if (rank == -1) return -1;
        if (t->mkey_len[slot] == n && memcmp(t->mkey_ptr[slot], s, (usize)n) == 0)
            return rank;
        slot = (slot + 1) & t->mh_mask;
    }
}

/* ---- create / free -------------------------------------------------- */
LlmTokenizer *llm_tokenizer_create(const GgufFile *g) {
    if (!g) return NULL;

    /* Only byte-level BPE is supported. */
    const char *model = NULL; u64 model_len = 0;
    if (gguf_kv_str(g, "tokenizer.ggml.model", &model, &model_len)) {
        if (!(model_len == 4 && memcmp(model, "gpt2", 4) == 0)) {
            /* SentencePiece / wordpiece — not implemented. */
            return NULL;
        }
    }

    const GgufKv *toks = gguf_find(g, "tokenizer.ggml.tokens");
    const GgufKv *merges = gguf_find(g, "tokenizer.ggml.merges");
    if (!toks || toks->type != GGUF_KV_ARR || toks->arr_type != GGUF_KV_STR)
        return NULL;
    if (!merges || merges->type != GGUF_KV_ARR || merges->arr_type != GGUF_KV_STR)
        return NULL;
    if (toks->arr_count == 0 || toks->arr_count > (1u << 21)) return NULL;
    /* Cap merges like tokens: an unbounded count overflows the u32 hash
     * sizing (mcount*2+1) and could otherwise spin next_pow2. A real
     * gpt2 model has tens of thousands of merges, never millions. */
    if (merges->arr_count == 0 || merges->arr_count > (1u << 21)) return NULL;

    LlmTokenizer *t = (LlmTokenizer *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->vocab_size = (i32)toks->arr_count;
    t->bos = -1;
    t->eos = -1;

    build_byte_maps(t);

    t->tok_ptr  = (const char **)malloc((usize)t->vocab_size * sizeof(char *));
    t->tok_len  = (i32 *)malloc((usize)t->vocab_size * sizeof(i32));
    t->tok_type = (i8 *)malloc((usize)t->vocab_size);
    if (!t->tok_ptr || !t->tok_len || !t->tok_type) { llm_tokenizer_free(t); return NULL; }
    for (i32 i = 0; i < t->vocab_size; i++) t->tok_type[i] = 1; /* NORMAL */

    {
        GgufStrIter it = gguf_arr_str_begin(g, toks);
        i32 i = 0;
        const char *s; u64 sl;
        while (i < t->vocab_size && gguf_arr_str_next(&it, &s, &sl)) {
            t->tok_ptr[i] = s;
            t->tok_len[i] = (i32)sl;
            i++;
        }
        if (i != t->vocab_size) { llm_tokenizer_free(t); return NULL; }
    }

    /* token_type array — optional. */
    {
        const GgufKv *tt = gguf_find(g, "tokenizer.ggml.token_type");
        if (tt && tt->type == GGUF_KV_ARR && tt->arr_type == GGUF_KV_I32 &&
            tt->arr_count == (u64)t->vocab_size && tt->arr_data) {
            for (i32 i = 0; i < t->vocab_size; i++) {
                i32 v;
                memcpy(&v, tt->arr_data + (usize)i * 4, 4);
                t->tok_type[i] = (i8)v;
            }
        }
    }

    if (!vmap_build(t)) { llm_tokenizer_free(t); return NULL; }

    /* merge hash. */
    {
        u32 mcount = (u32)merges->arr_count;
        u32 cap = next_pow2(mcount * 2u + 1u);
        t->mkey_ptr = (const char **)malloc(cap * sizeof(char *));
        t->mkey_len = (i32 *)malloc(cap * sizeof(i32));
        t->mrank    = (i32 *)malloc(cap * sizeof(i32));
        if (!t->mkey_ptr || !t->mkey_len || !t->mrank) {
            llm_tokenizer_free(t); return NULL;
        }
        t->mh_mask = cap - 1;
        for (u32 i = 0; i < cap; i++) t->mrank[i] = -1;

        GgufStrIter it = gguf_arr_str_begin(g, merges);
        const char *s; u64 sl;
        i32 rank = 0;
        while (gguf_arr_str_next(&it, &s, &sl)) {
            u32 slot = fnv1a(s, (i32)sl) & t->mh_mask;
            while (t->mrank[slot] != -1) slot = (slot + 1) & t->mh_mask;
            t->mkey_ptr[slot] = s;
            t->mkey_len[slot] = (i32)sl;
            t->mrank[slot] = rank++;
        }
    }

    /* bos / eos ids. */
    {
        u32 v;
        if (gguf_kv_u32(g, "tokenizer.ggml.bos_token_id", &v) && v < (u32)t->vocab_size)
            t->bos = (i32)v;
        if (gguf_kv_u32(g, "tokenizer.ggml.eos_token_id", &v) && v < (u32)t->vocab_size)
            t->eos = (i32)v;
    }

    return t;
}

void llm_tokenizer_free(LlmTokenizer *t) {
    if (!t) return;
    free(t->tok_ptr);
    free(t->tok_len);
    free(t->tok_type);
    free(t->vmap);
    free(t->mkey_ptr);
    free(t->mkey_len);
    free(t->mrank);
    free(t);
}

/* ---- pre-tokenizer -------------------------------------------------- */
/* Simplified GPT-2 splitter: a chunk is an optional single leading space
 * plus a maximal run of one category (letter / digit / other). Runs of
 * 2+ spaces emit the surplus as their own chunk; a lone space attaches
 * to the next word. Adequate for typed sentences — the translation
 * feature's actual input. */
typedef enum { CAT_SPACE, CAT_LETTER, CAT_DIGIT, CAT_OTHER } Cat;

static Cat classify(u32 cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
        cp == '\f' || cp == '\v')
        return CAT_SPACE;
    if (cp >= '0' && cp <= '9') return CAT_DIGIT;
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp >= 0x80)
        return CAT_LETTER;
    return CAT_OTHER;
}

/* ---- BPE per chunk -------------------------------------------------- */
/* `enc` holds the byte-level remap of one chunk; `sym` describes the
 * current symbol spans into it. Returns the final symbol count. */
static i32 bpe_merge(const LlmTokenizer *t, const char *enc,
                     i32 sym_off[], i32 sym_len[], i32 nsym) {
    char scratch[TOK_MERGE_SCRATCH];

    for (;;) {
        i32 best_rank = 0x7FFFFFFF;
        i32 best_i = -1;
        for (i32 i = 0; i + 1 < nsym; i++) {
            i32 l1 = sym_len[i], l2 = sym_len[i + 1];
            i32 klen = l1 + 1 + l2;
            if (klen > TOK_MERGE_SCRATCH) continue;
            memcpy(scratch, enc + sym_off[i], (usize)l1);
            scratch[l1] = ' ';
            memcpy(scratch + l1 + 1, enc + sym_off[i + 1], (usize)l2);
            i32 rank = mh_lookup(t, scratch, klen);
            if (rank >= 0 && rank < best_rank) {
                best_rank = rank;
                best_i = i;
            }
        }
        if (best_i < 0) break;
        /* Merge best_i and best_i+1 — adjacent spans are contiguous in
         * `enc`, so the merged symbol is just the combined span. */
        sym_len[best_i] += sym_len[best_i + 1];
        for (i32 j = best_i + 1; j + 1 < nsym; j++) {
            sym_off[j] = sym_off[j + 1];
            sym_len[j] = sym_len[j + 1];
        }
        nsym--;
    }
    return nsym;
}

/* Byte-level-encode one chunk into `enc` (cap bytes), recording each
 * original byte as one initial symbol. Returns enc length, or -1 if it
 * doesn't fit. */
static i32 encode_chunk(const LlmTokenizer *t, const char *chunk, i32 chunk_len,
                        char *enc, i32 enc_cap, i32 sym_off[], i32 sym_len[],
                        i32 *out_nsym) {
    i32 elen = 0, nsym = 0;
    for (i32 i = 0; i < chunk_len; i++) {
        u32 cp = t->byte_to_cp[(u8)chunk[i]];
        u8 tmp[4];
        u32 used = utf8_encode(cp, tmp);
        if (used == 0) used = 1, tmp[0] = '?';
        if (elen + (i32)used > enc_cap || nsym >= TOK_MAX_SYMBOLS) return -1;
        sym_off[nsym] = elen;
        sym_len[nsym] = (i32)used;
        nsym++;
        memcpy(enc + elen, tmp, used);
        elen += (i32)used;
    }
    *out_nsym = nsym;
    return elen;
}

/* ---- encode --------------------------------------------------------- */
i32 llm_tokenizer_encode(const LlmTokenizer *t, const char *text,
                         i32 *out_ids, i32 cap) {
    if (!t || !text || !out_ids || cap <= 0) return 0;

    i32 nout = 0;
    const u8 *p = (const u8 *)text;
    const u8 *end = p + strlen(text);

    /* enc / symbol scratch sized for the worst case: every byte maps to
     * 2 UTF-8 bytes and stays its own symbol. */
    char enc[TOK_MAX_SYMBOLS * 2];
    i32 sym_off[TOK_MAX_SYMBOLS];
    i32 sym_len[TOK_MAX_SYMBOLS];

    while (p < end && nout < cap) {
        /* Decode the leading codepoint to classify it. */
        u32 cp;
        u32 used = utf8_decode(p, (usize)(end - p), &cp);
        if (used == 0) { p++; continue; }
        Cat cat = classify(cp);

        const u8 *chunk_start;
        const u8 *chunk_end;

        if (cat == CAT_SPACE) {
            /* Count the whitespace run. */
            const u8 *q = p;
            i32 spaces = 0;
            while (q < end) {
                u32 c2;
                u32 u2 = utf8_decode(q, (usize)(end - q), &c2);
                if (u2 == 0 || classify(c2) != CAT_SPACE) break;
                q += u2;
                spaces++;
            }
            bool word_follows = (q < end);
            if (spaces >= 2 && word_follows) {
                /* surplus spaces -> own chunk; keep the last for the word */
                chunk_start = p;
                chunk_end = q - 1;          /* one byte == one ASCII space */
                p = q - 1;
            } else if (!word_follows) {
                chunk_start = p;
                chunk_end = q;
                p = q;
            } else {
                /* lone space — attach to the following word run. Every
                 * CAT_SPACE codepoint is 1 byte, so here q == p + 1. */
                const u8 *r = q;
                u32 c3;
                u32 u3 = utf8_decode(r, (usize)(end - r), &c3);
                Cat wc = (u3 ? classify(c3) : CAT_OTHER);
                r += u3 ? u3 : 1;
                while (r < end) {
                    u32 c4;
                    u32 u4 = utf8_decode(r, (usize)(end - r), &c4);
                    if (u4 == 0 || classify(c4) != wc) break;
                    r += u4;
                }
                chunk_start = p;            /* includes the leading space */
                chunk_end = r;
                p = r;
            }
        } else {
            /* letter/digit/other run starting at p (no leading space) */
            const u8 *r = p + used;
            while (r < end) {
                u32 c2;
                u32 u2 = utf8_decode(r, (usize)(end - r), &c2);
                if (u2 == 0 || classify(c2) != cat) break;
                r += u2;
            }
            chunk_start = p;
            chunk_end = r;
            p = r;
        }

        i32 chunk_len = (i32)(chunk_end - chunk_start);
        if (chunk_len <= 0) continue;

        i32 nsym = 0;
        i32 elen = encode_chunk(t, (const char *)chunk_start, chunk_len,
                                enc, (i32)sizeof enc, sym_off, sym_len, &nsym);
        if (elen < 0 || nsym == 0) continue;

        nsym = bpe_merge(t, enc, sym_off, sym_len, nsym);

        for (i32 i = 0; i < nsym && nout < cap; i++) {
            i32 id = vmap_lookup(t, enc + sym_off[i], sym_len[i]);
            if (id >= 0) out_ids[nout++] = id;
            /* A miss is impossible with byte-level BPE (every single-byte
             * symbol is in vocab), so we just skip if it somehow happens. */
        }
    }
    return nout;
}

/* ---- decode --------------------------------------------------------- */
i32 llm_tokenizer_decode(const LlmTokenizer *t, i32 id, char *out, i32 cap) {
    if (!t || id < 0 || id >= t->vocab_size || !out || cap <= 0) return 0;
    /* CONTROL (3) / UNKNOWN (2) / UNUSED (5) tokens have no printable
     * form — suppress them. NORMAL (1), USER_DEFINED (4), BYTE (6) fall
     * through to the byte-level decode. */
    i8 tt = t->tok_type[id];
    if (tt == 2 || tt == 3 || tt == 5) return 0;

    const char *s = t->tok_ptr[id];
    i32 n = t->tok_len[id];
    i32 w = 0;
    i32 i = 0;
    while (i < n) {
        u32 cp;
        u32 used = utf8_decode((const u8 *)s + i, (usize)(n - i), &cp);
        if (used == 0) { i++; continue; }
        i += (i32)used;
        i16 b = (cp < 512) ? t->cp_to_byte[cp] : -1;
        if (b < 0) {
            /* Codepoint outside the byte-level alphabet — emit verbatim
             * UTF-8 (handles literal user-defined tokens gracefully). */
            if (w + (i32)used > cap) break;
            memcpy(out + w, s + i - used, used);
            w += (i32)used;
        } else {
            if (w + 1 > cap) break;
            out[w++] = (char)b;
        }
    }
    return w;
}

i32 llm_tokenizer_bos(const LlmTokenizer *t) { return t ? t->bos : -1; }
i32 llm_tokenizer_eos(const LlmTokenizer *t) { return t ? t->eos : -1; }
i32 llm_tokenizer_vocab_size(const LlmTokenizer *t) { return t ? t->vocab_size : 0; }

i32 llm_tokenizer_token_id(const LlmTokenizer *t, const char *literal) {
    if (!t || !literal) return -1;
    return vmap_lookup(t, literal, (i32)strlen(literal));
}
