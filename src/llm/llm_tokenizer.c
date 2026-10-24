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
#include "core/checked.h"
#include "llm/llm_pretokenizer.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

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
    LlmPreType   pre_type;

    u32          byte_to_cp[256];
    bool         byte_known[256];
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
#define TOK_MAX_PROBES 128u
typedef struct {
    const LlmCancel *cancel;
    u64 steps, bytes;
    u32 poll;
} TokenBuildWork;

static bool token_work(TokenBuildWork *w, usize steps, usize bytes) {
    if ((u64)steps > w->steps || (u64)bytes > w->bytes) return false;
    w->steps -= steps; w->bytes -= bytes;
    w->poll += (u32)steps;
    if (w->poll >= 1024u) {
        w->poll = 0;
        if (llm_cancelled(w->cancel)) return false;
    }
    return true;
}

static bool token_hash(TokenBuildWork *w, const char *s, i32 n, u32 *out) {
    if (n < 0 || !token_work(w, 1, (usize)n)) return false;
    u32 h = 2166136261u;
    for (i32 i = 0; i < n; i++) {
        if (!(i & 1023) && llm_cancelled(w->cancel)) return false;
        h = (h ^ (u8)s[i]) * 16777619u;
    }
    *out = h; return true;
}

static bool vmap_build(LlmTokenizer *t, TokenBuildWork *w) {
    u32 cap = next_pow2((u32)t->vocab_size * 2u + 1u);
    t->vmap = (i32 *)malloc(cap * sizeof(i32));
    if (!t->vmap) return false;
    t->vmap_mask = cap - 1;
    for (u32 i = 0; i < cap; i++) {
        if (!(i & 1023u) && !token_work(w, 1024, 0)) return false;
        t->vmap[i] = -1;
    }

    for (i32 id = 0; id < t->vocab_size; id++) {
        u32 hash;
        if (!token_hash(w, t->tok_ptr[id], t->tok_len[id], &hash)) return false;
        u32 slot = hash & t->vmap_mask, probes = 0;
        while (t->vmap[slot] != -1) {
            if (++probes >= TOK_MAX_PROBES || !token_work(w, 1, 0)) return false;
            i32 old = t->vmap[slot];
            if (t->tok_len[old] == t->tok_len[id]) {
                if (!token_work(w, 1, (usize)t->tok_len[id])) return false;
                if (!memcmp(t->tok_ptr[old], t->tok_ptr[id], (usize)t->tok_len[id])) break;
            }
            slot = (slot + 1) & t->vmap_mask;
        }
        /* Duplicate strings retain the first token id, as lookup always did. */
        if (t->vmap[slot] == -1) t->vmap[slot] = id;
    }
    return true;
}

static i32 vmap_lookup(const LlmTokenizer *t, const char *s, i32 n) {
    u32 slot = fnv1a(s, n) & t->vmap_mask;
    for (u32 probes = 0; probes < TOK_MAX_PROBES; probes++) {
        i32 id = t->vmap[slot];
        if (id == -1) return -1;
        if (t->tok_len[id] == n && memcmp(t->tok_ptr[id], s, (usize)n) == 0)
            return id;
        slot = (slot + 1) & t->vmap_mask;
    }
    return -1;
}

/* Hash a merge pair in place. Valid Hunyuan merges include 512 spaces,
 * whose byte-alphabet spelling does not fit a fixed 512-byte key buffer. */
#ifdef LIU_BPE_COUNT_LOOKUPS
unsigned long g_liu_bpe_lookups;     /* Count pair lookups when LIU_BPE_COUNT_LOOKUPS is enabled. */
#endif
static i32 mh_lookup_pair(const LlmTokenizer *t, const char *a, i32 na,
                           const char *b, i32 nb) {
#ifdef LIU_BPE_COUNT_LOOKUPS
    g_liu_bpe_lookups++;
#endif
    u32 hash = fnv1a(a, na);
    hash = (hash ^ (u8)' ') * 16777619u;
    for (i32 i = 0; i < nb; i++) hash = (hash ^ (u8)b[i]) * 16777619u;
    u32 slot = hash & t->mh_mask;
    for (u32 probes = 0; probes < TOK_MAX_PROBES; probes++) {
        i32 rank = t->mrank[slot];
        if (rank == -1) return -1;
        const char *key = t->mkey_ptr[slot];
        if (t->mkey_len[slot] == na + 1 + nb && key[na] == ' ' &&
            memcmp(key, a, (usize)na) == 0 && memcmp(key + na + 1, b, (usize)nb) == 0)
            return rank;
        slot = (slot + 1) & t->mh_mask;
    }
    return -1;
}

/* ---- create / free -------------------------------------------------- */
LlmTokenizer *llm_tokenizer_create(const GgufFile *g) {
    return llm_tokenizer_create_cancellable(g, NULL);
}

LlmTokenizer *llm_tokenizer_create_cancellable(const GgufFile *g, const LlmCancel *cancel) {
    if (!g || llm_cancelled(cancel)) return NULL;
    TokenBuildWork work = {cancel, 64ull * 1024 * 1024, 256ull * 1024 * 1024, 0};

    /* Only byte-level BPE is supported. */
    const char *model = NULL; u64 model_len = 0;
    if (gguf_kv_str(g, "tokenizer.ggml.model", &model, &model_len)) {
        if (!(model_len == 4 && memcmp(model, "gpt2", 4) == 0)) {
            /* SentencePiece / wordpiece — not implemented. */
            return NULL;
        }
    }

    LlmPreType pre_type = LLM_PRE_GPT2;
    const char *pre = NULL; u64 pre_len = 0;
    if (gguf_find(g, "tokenizer.ggml.pre")) {
        if (!gguf_kv_str(g, "tokenizer.ggml.pre", &pre, &pre_len)) return NULL;
        if (pre_len == 13 && memcmp(pre, "hunyuan-dense", 13) == 0)
            pre_type = LLM_PRE_HUNYUAN;
        else if (!((pre_len == 4 && memcmp(pre, "gpt2", 4) == 0) ||
                   (pre_len == 5 && memcmp(pre, "gpt-2", 5) == 0) ||
                   (pre_len == 7 && memcmp(pre, "default", 7) == 0)))
            return NULL;
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
    t->pre_type = pre_type;
    t->bos = -1;
    t->eos = -1;

    build_byte_maps(t);

    t->tok_ptr  = (const char **)malloc((usize)t->vocab_size * sizeof(char *));
    t->tok_len  = (i32 *)malloc((usize)t->vocab_size * sizeof(i32));
    t->tok_type = (i8 *)malloc((usize)t->vocab_size);
    if (!t->tok_ptr || !t->tok_len || !t->tok_type) { llm_tokenizer_free(t); return NULL; }
    for (i32 i = 0; i < t->vocab_size; i++) {
        if (!(i & 1023) && !token_work(&work, 1024, 0)) { llm_tokenizer_free(t); return NULL; }
        t->tok_type[i] = 1;
    }

    {
        GgufStrIter it = gguf_arr_str_begin(g, toks);
        i32 i = 0;
        const char *s; u64 sl;
        while (i < t->vocab_size && gguf_arr_str_next(&it, &s, &sl)) {
            if (sl > INT32_MAX || !token_work(&work, 1, 0)) { llm_tokenizer_free(t); return NULL; }
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
                if (!(i & 1023) && !token_work(&work, 1024, 0)) { llm_tokenizer_free(t); return NULL; }
                i32 v;
                memcpy(&v, tt->arr_data + (usize)i * 4, 4);
                t->tok_type[i] = (i8)v;
            }
        }
    }

    if (!vmap_build(t, &work)) { llm_tokenizer_free(t); return NULL; }
    for (i32 b = 0; b < 256; b++) {
        u8 encoded[4];
        u32 n = utf8_encode(t->byte_to_cp[b], encoded);
        t->byte_known[b] = vmap_lookup(t, (const char *)encoded, (i32)n) >= 0;
    }

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
        for (u32 i = 0; i < cap; i++) {
            if (!(i & 1023u) && !token_work(&work, 1024, 0)) { llm_tokenizer_free(t); return NULL; }
            t->mrank[i] = -1;
        }

        GgufStrIter it = gguf_arr_str_begin(g, merges);
        const char *s; u64 sl;
        i32 rank = 0;
        while (gguf_arr_str_next(&it, &s, &sl)) {
            if (sl > INT32_MAX || rank >= (i32)mcount) {
                llm_tokenizer_free(t);
                return NULL;
            }
            u32 hash;
            if (!token_hash(&work, s, (i32)sl, &hash)) { llm_tokenizer_free(t); return NULL; }
            u32 slot = hash & t->mh_mask, probes = 0;
            while (t->mrank[slot] != -1) {
                if (++probes >= TOK_MAX_PROBES || !token_work(&work, 1, 0)) {
                    llm_tokenizer_free(t); return NULL;
                }
                if (t->mkey_len[slot] == (i32)sl) {
                    if (!token_work(&work, 1, (usize)sl)) { llm_tokenizer_free(t); return NULL; }
                    if (!memcmp(t->mkey_ptr[slot], s, (usize)sl)) break;
                }
                slot = (slot + 1) & t->mh_mask;
            }
            if (t->mrank[slot] == -1) {
                t->mkey_ptr[slot] = s; t->mkey_len[slot] = (i32)sl; t->mrank[slot] = rank;
            }
            rank++;
        }
        if (rank != (i32)mcount) {
            llm_tokenizer_free(t);
            return NULL;
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

/* ---- BPE per chunk -------------------------------------------------- */
/* `enc` holds the byte-level remap of one chunk; `sym` describes the
 * current symbol spans into it. Returns the final symbol count. */
/* The reference: rescan every neighbouring pair for the lowest rank (leftmost
 * wins a tie), merge it, shift the arrays, repeat. O(n^2) lookups AND moves on
 * one long pre-token — a 100,000-character run of one letter is ~5e9 lookups.
 * Kept as the oracle the heap version is diffed against, and as the fallback
 * when its scratch cannot be allocated. */
static i32 bpe_merge_scan(const LlmTokenizer *t, const char *enc,
                          i32 sym_off[], i32 sym_len[], i32 nsym) {
    for (;;) {
        i32 best_rank = 0x7FFFFFFF;
        i32 best_i = -1;
        for (i32 i = 0; i + 1 < nsym; i++) {
            i32 l1 = sym_len[i], l2 = sym_len[i + 1];
            i32 rank = mh_lookup_pair(t, enc + sym_off[i], l1,
                                       enc + sym_off[i + 1], l2);
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

/* The same merges in O(n log n): symbols live in a doubly linked list, every
 * adjacent pair is looked up ONCE when it comes into being, and a min-heap
 * orders the candidates by (rank, left position) — "lowest rank, leftmost on a
 * tie", exactly the scan's choice. A merge changes at most two pairs (the one
 * before and the one after), so at most two lookups per merge. Entries that a
 * merge made stale are recognised by the lengths they were pushed with and
 * skipped when popped. The result is written back into sym_off/sym_len in
 * order, so callers see the same arrays the scan produced. */
typedef struct { i32 rank, left, right, llen, rlen; } BpeCand;

static inline bool bpe_cand_less(const BpeCand *a, const BpeCand *b) {
    return a->rank < b->rank || (a->rank == b->rank && a->left < b->left);
}
static void bpe_heap_push(BpeCand *h, i32 *n, BpeCand c) {
    i32 i = (*n)++;
    while (i > 0) {
        i32 p = (i - 1) / 2;
        if (!bpe_cand_less(&c, &h[p])) break;
        h[i] = h[p];
        i = p;
    }
    h[i] = c;
}
static BpeCand bpe_heap_pop(BpeCand *h, i32 *n) {
    BpeCand top = h[0];
    BpeCand last = h[--(*n)];
    i32 i = 0;
    for (;;) {
        i32 c = 2 * i + 1;
        if (c >= *n) break;
        if (c + 1 < *n && bpe_cand_less(&h[c + 1], &h[c])) c++;
        if (!bpe_cand_less(&h[c], &last)) break;
        h[i] = h[c];
        i = c;
    }
    if (*n > 0) h[i] = last;
    return top;
}

static i32 bpe_merge(const LlmTokenizer *t, const char *enc,
                     i32 sym_off[], i32 sym_len[], i32 nsym) {
    if (nsym < 2) return nsym;
    /* prev/next/alive per symbol + up to 3n candidates (n initial pairs, two
     * new ones per merge). */
    usize n = (usize)nsym;
    usize cand_cap = 3u * n + 4u;
    usize bytes = n * 3u * sizeof(i32) + cand_cap * sizeof(BpeCand);
    i32 *mem = (i32 *)malloc(bytes);
    if (!mem) return bpe_merge_scan(t, enc, sym_off, sym_len, nsym);
    i32 *prev = mem, *next = mem + n, *alive = mem + 2 * n;
    BpeCand *heap = (BpeCand *)(mem + 3 * n);
    i32 hn = 0;
    for (i32 i = 0; i < nsym; i++) {
        prev[i] = i - 1;
        next[i] = i + 1 < nsym ? i + 1 : -1;
        alive[i] = 1;
    }
    for (i32 i = 0; i + 1 < nsym; i++) {
        i32 rank = mh_lookup_pair(t, enc + sym_off[i], sym_len[i],
                                  enc + sym_off[i + 1], sym_len[i + 1]);
        if (rank >= 0)
            bpe_heap_push(heap, &hn, (BpeCand){ rank, i, i + 1, sym_len[i], sym_len[i + 1] });
    }
    while (hn > 0) {
        BpeCand c = bpe_heap_pop(heap, &hn);
        /* Stale: either symbol is gone, they are no longer neighbours, or one
         * of them has grown since this pair was priced. */
        if (!alive[c.left] || !alive[c.right] || next[c.left] != c.right ||
            sym_len[c.left] != c.llen || sym_len[c.right] != c.rlen) continue;
        sym_len[c.left] += sym_len[c.right];          /* contiguous in enc */
        alive[c.right] = 0;
        i32 r = next[c.right];
        next[c.left] = r;
        if (r >= 0) prev[r] = c.left;
        /* The two pairs the merged symbol now forms. */
        if (prev[c.left] >= 0) {
            i32 p = prev[c.left];
            i32 rank = mh_lookup_pair(t, enc + sym_off[p], sym_len[p],
                                      enc + sym_off[c.left], sym_len[c.left]);
            if (rank >= 0)
                bpe_heap_push(heap, &hn, (BpeCand){ rank, p, c.left, sym_len[p], sym_len[c.left] });
        }
        if (r >= 0) {
            i32 rank = mh_lookup_pair(t, enc + sym_off[c.left], sym_len[c.left],
                                      enc + sym_off[r], sym_len[r]);
            if (rank >= 0)
                bpe_heap_push(heap, &hn, (BpeCand){ rank, c.left, r, sym_len[c.left], sym_len[r] });
        }
    }
    /* Compact the survivors, in order, into the caller's arrays. */
    i32 out = 0;
    for (i32 i = 0; i < nsym; i++) {
        if (!alive[i]) continue;
        sym_off[out] = sym_off[i];
        sym_len[out] = sym_len[i];
        out++;
    }
    free(mem);
    return out;
}

/* Byte-level-encode one chunk into `enc` (cap bytes), recording each
 * original byte as one initial symbol. Returns enc length, or -1 if it
 * doesn't fit. */
static i32 encode_chunk(const LlmTokenizer *t, const char *chunk, i32 chunk_len,
                        char *enc, i32 enc_cap, i32 sym_off[], i32 sym_len[],
                        i32 *out_nsym) {
    i32 elen = 0, nsym = 0;
    for (i32 i = 0; i < chunk_len; i++) {
        /* Like the reference BPE with no unk token, omit missing alphabet
         * symbols before merging. Hunyuan's vocabulary omits CR, so CRLF
         * must encode like LF rather than fail or block adjacent merges. */
        if (!t->byte_known[(u8)chunk[i]]) continue;
        u32 cp = t->byte_to_cp[(u8)chunk[i]];
        u8 tmp[4];
        u32 used = utf8_encode(cp, tmp);
        if (used == 0) used = 1, tmp[0] = '?';
        if (elen + (i32)used > enc_cap || nsym >= chunk_len) return -1;
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
    if (!t || !text || cap < 0 || (!out_ids && cap > 0)) return -1;
    if (!text[0]) return 0;
    if (cap == 0) return -1;

    char stack_enc[TOK_MAX_SYMBOLS * 2];
    i32 stack_off[TOK_MAX_SYMBOLS], stack_len[TOK_MAX_SYMBOLS];
    char *enc = stack_enc;
    i32 *sym_off = stack_off, *sym_len = stack_len;
    void *heap = NULL;
    usize scratch_cap = TOK_MAX_SYMBOLS;
    i32 nout = 0;
    LlmPreTokenizer it;
    llm_pre_init(&it, text, t->pre_type);
    const u8 *chunk;
    usize len;
    while (llm_pre_next(&it, &chunk, &len)) {
        /* A hard scratch boundary changes BPE merges. Grow only for unusually
         * long pre-tokens instead of splitting their text at an arbitrary byte. */
        if (len > (usize)(INT32_MAX - 1) / 2) { nout = -1; break; }
        if (len > scratch_cap) {
            usize enc_bytes = (len * 2u + sizeof(i32) - 1u) & ~(sizeof(i32) - 1u);
            usize symbol_bytes, bytes;
            if (!checked_mul_usize(len, 2u * sizeof(i32), &symbol_bytes) ||
                !checked_add_usize(enc_bytes, symbol_bytes, &bytes)) { nout = -1; break; }
            void *next = realloc(heap, bytes);
            if (!next) { nout = -1; break; }
            heap = next; scratch_cap = len;
            enc = heap;
            sym_off = (i32 *)((u8 *)heap + enc_bytes);
            sym_len = sym_off + len;
        }
        i32 nsym = 0;
        if (encode_chunk(t, (const char *)chunk, (i32)len, enc, (i32)(scratch_cap * 2u),
                          sym_off, sym_len, &nsym) < 0) { nout = -1; break; }
        nsym = bpe_merge(t, enc, sym_off, sym_len, nsym);
        if (nsym > cap - nout) { nout = -1; break; }
        for (i32 i = 0; i < nsym; i++) {
            i32 id = vmap_lookup(t, enc + sym_off[i], sym_len[i]);
            if (id < 0) { nout = -1; break; }
            out_ids[nout++] = id;
        }
        if (nout < 0) break;
    }
    free(heap);
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
            if ((i32)used > cap - w) break;
            memcpy(out + w, s + i - used, used);
            w += (i32)used;
        } else {
            if (w + 1 > cap) break;
            out[w++] = (char)b;
        }
    }
    return w;
}

i32 llm_tokenizer_decoded_len(const LlmTokenizer *t, i32 id) {
    if (!t || id < 0 || id >= t->vocab_size) return 0;
    i8 tt = t->tok_type[id];
    if (tt == 2 || tt == 3 || tt == 5) return 0;     /* no printable form */
    const char *s = t->tok_ptr[id];
    i32 n = t->tok_len[id];
    i64 w = 0;
    for (i32 i = 0; i < n; ) {
        u32 cp;
        u32 used = utf8_decode((const u8 *)s + i, (usize)(n - i), &cp);
        if (used == 0) { i++; continue; }
        i += (i32)used;
        i16 b = (cp < 512) ? t->cp_to_byte[cp] : -1;
        w += b < 0 ? (i64)used : 1;                  /* verbatim vs byte-level */
    }
    return w > INT32_MAX ? INT32_MAX : (i32)w;
}

i32 llm_tokenizer_max_piece(const LlmTokenizer *t) {
    if (!t) return 0;
    i32 best = 0;
    for (i32 id = 0; id < t->vocab_size; id++) {
        i32 n = llm_tokenizer_decoded_len(t, id);
        if (n > best) best = n;
    }
    return best;
}

i32 llm_tokenizer_bos(const LlmTokenizer *t) { return t ? t->bos : -1; }
i32 llm_tokenizer_eos(const LlmTokenizer *t) { return t ? t->eos : -1; }
i32 llm_tokenizer_vocab_size(const LlmTokenizer *t) { return t ? t->vocab_size : 0; }

i32 llm_tokenizer_token_id(const LlmTokenizer *t, const char *literal) {
    if (!t || !literal) return -1;
    usize len = strlen(literal);
    if (len > INT32_MAX) return -1;
    return vmap_lookup(t, literal, (i32)len);
}
